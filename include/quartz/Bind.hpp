#pragma once

#include "vm/Sandbox.hpp"

#include <cstdint>
#include <expected>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace quartz {

namespace bind_detail {

template <typename T>
struct Host;

template <typename T, ir::TypeId Id>
struct SignedHost {
    static constexpr ir::TypeId kType = Id;

    static T get(const vm::Slot& s) { return static_cast<T>(s.i); }

    static bool put(vm::HostCall&, vm::Slot& s, T v) {
        s.i = v;
        return true;
    }
};

template <typename T, ir::TypeId Id>
struct UnsignedHost {
    static constexpr ir::TypeId kType = Id;

    static T get(const vm::Slot& s) { return static_cast<T>(s.u); }

    static bool put(vm::HostCall&, vm::Slot& s, T v) {
        s.u = v;
        return true;
    }
};

template <>
struct Host<std::int8_t> : SignedHost<std::int8_t, ir::kI8> {};

template <>
struct Host<std::int16_t> : SignedHost<std::int16_t, ir::kI16> {};

template <>
struct Host<std::int32_t> : SignedHost<std::int32_t, ir::kI32> {};

template <>
struct Host<std::int64_t> : SignedHost<std::int64_t, ir::kI64> {};

template <>
struct Host<std::uint8_t> : UnsignedHost<std::uint8_t, ir::kU8> {};

template <>
struct Host<std::uint16_t> : UnsignedHost<std::uint16_t, ir::kU16> {};

template <>
struct Host<std::uint32_t> : UnsignedHost<std::uint32_t, ir::kU32> {};

template <>
struct Host<std::uint64_t> : UnsignedHost<std::uint64_t, ir::kU64> {};

template <>
struct Host<char32_t> : UnsignedHost<char32_t, ir::kChar> {};

template <>
struct Host<bool> {
    static constexpr ir::TypeId kType = ir::kBool;

    static bool get(const vm::Slot& s) { return s.u != 0; }

    static bool put(vm::HostCall&, vm::Slot& s, bool v) {
        s.u = v ? 1 : 0;
        return true;
    }
};

template <>
struct Host<double> {
    static constexpr ir::TypeId kType = ir::kF64;

    static double get(const vm::Slot& s) { return s.f; }

    static bool put(vm::HostCall&, vm::Slot& s, double v) {
        s.f = v;
        return true;
    }
};

template <>
struct Host<float> {
    static constexpr ir::TypeId kType = ir::kF32;

    static float get(const vm::Slot& s) { return s.f32; }

    static bool put(vm::HostCall&, vm::Slot& s, float v) {
        s.u = 0;
        s.f32 = v;
        return true;
    }
};

template <>
struct Host<std::string_view> {
    static constexpr ir::TypeId kType = ir::kStr;

    static std::string_view get(const vm::Slot& s) { return static_cast<const vm::Str*>(s.p)->view(); }

    static bool put(vm::HostCall& call, vm::Slot& s, std::string_view v) {
        s.p = call.string(v);
        return s.p != nullptr;
    }
};

template <>
struct Host<std::string> : Host<std::string_view> {
    static bool put(vm::HostCall& call, vm::Slot& s, const std::string& v) {
        return Host<std::string_view>::put(call, s, v);
    }
};

template <typename T>
using Plain = std::remove_cvref_t<T>;

template <typename T>
bool matches(const ir::TypeTable& types, ir::TypeId id) {
    if constexpr (requires { Host<T>::kType; }) {
        return id == Host<T>::kType;
    } else {
        return Host<T>::matches(types, id);
    }
}

template <typename T>
std::string nameOf(const ir::TypeTable& types) {
    if constexpr (requires { Host<T>::kType; }) {
        return types.name(Host<T>::kType);
    } else {
        return Host<T>::name();
    }
}

template <typename R, typename O, typename... A>
struct SignatureOf {
    using Result = R;
    using Object = O; // the class of a member function, void for a free one
    using Args = std::tuple<A...>;
};

template <typename F>
struct Signature;

template <typename R, typename... A>
struct Signature<R (*)(A...)> : SignatureOf<R, void, A...> {};

template <typename R, typename... A>
struct Signature<R (*)(A...) noexcept> : SignatureOf<R, void, A...> {};

template <typename R, typename C, typename... A>
struct Signature<R (C::*)(A...)> : SignatureOf<R, C, A...> {};

template <typename R, typename C, typename... A>
struct Signature<R (C::*)(A...) noexcept> : SignatureOf<R, C, A...> {};

template <typename R, typename C, typename... A>
struct Signature<R (C::*)(A...) const> : SignatureOf<R, const C, A...> {};

template <typename R, typename C, typename... A>
struct Signature<R (C::*)(A...) const noexcept> : SignatureOf<R, const C, A...> {};

// A first parameter of type `vm::HostCall&` receives the call itself (to charge fuel, reach the sandbox or fail).
template <typename Args>
constexpr bool kTakesCall = false;
template <typename First, typename... Rest>
constexpr bool kTakesCall<std::tuple<First, Rest...>> = std::is_same_v<First, vm::HostCall&>;

/// How a bound function's parameters split: the call, then the user pointer of a free function, then the arguments.
template <auto Fn, bool WithUser>
struct Shape {
    using Sig = Signature<decltype(Fn)>;
    using Args = typename Sig::Args;
    static constexpr bool kMember = !std::is_void_v<typename Sig::Object>;
    static constexpr std::size_t kCall = kTakesCall<Args> ? 1 : 0;
    static constexpr std::size_t kUser = WithUser && !kMember ? 1 : 0;
    static constexpr std::size_t kSkip = kCall + kUser;
    static constexpr std::size_t kCount = std::tuple_size_v<Args> - kSkip;
    template <std::size_t I>
    using Arg = Plain<std::tuple_element_t<I + kSkip, Args>>;
};

std::string describe(const ir::TypeTable& types, std::span<const ir::TypeId> params, ir::TypeId result);

template <auto Fn, bool WithUser>
bool thunk(vm::HostCall& call, void* user) {
    using S = Shape<Fn, WithUser>;
    auto invoke = [&]<std::size_t... I>(std::index_sequence<I...>) -> decltype(auto) {
        auto prefix = [&] {
            if constexpr (S::kCall == 1 && S::kUser == 1) {
                using U = std::tuple_element_t<1, typename S::Args>;
                return std::tuple<vm::HostCall&, U>(call, static_cast<U>(user));
            } else if constexpr (S::kCall == 1) {
                return std::tuple<vm::HostCall&>(call);
            } else if constexpr (S::kUser == 1) {
                using U = std::tuple_element_t<0, typename S::Args>;
                return std::tuple<U>(static_cast<U>(user));
            } else {
                return std::tuple<>();
            }
        }();
        auto args = std::tuple_cat(prefix, std::make_tuple(Host<typename S::template Arg<I>>::get(call.args[I])...));
        return std::apply(
            [&](auto&&... a) -> decltype(auto) {
                if constexpr (S::kMember) {
                    auto* object = static_cast<typename S::Sig::Object*>(user);
                    return (object->*Fn)(std::forward<decltype(a)>(a)...);
                } else {
                    return Fn(std::forward<decltype(a)>(a)...);
                }
            },
            std::move(args));
    };
    if constexpr (std::is_void_v<typename S::Sig::Result>) {
        invoke(std::make_index_sequence<S::kCount>{});
        return call.error.empty();
    } else {
        if (!Host<Plain<typename S::Sig::Result>>::put(call, call.result,
                                                       invoke(std::make_index_sequence<S::kCount>{}))) {
            return false;
        }
        return call.error.empty();
    }
}

template <auto Fn, bool WithUser>
std::expected<void, std::string> bindChecked(vm::Sandbox& sandbox, std::string_view importName, void* user) {
    using S = Shape<Fn, WithUser>;
    const ir::Module& m = *sandbox.program().module;
    auto found = sandbox.program().lookupImport(importName);
    if (!found) {
        return std::unexpected(found.error() == vm::LookupError::Ambiguous
                                   ? std::format("several host imports are called '{}'; add their package", importName)
                                   : std::format("the program has no host import '{}'", importName));
    }
    const ir::HostImport& imp = m.imports[*found];
    bool same = imp.params.size() == S::kCount;
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        same = same && (matches<typename S::template Arg<I>>(m.types, imp.params[I]) && ...);
    }(std::make_index_sequence<S::kCount>{});
    std::string result = "void";
    if constexpr (std::is_void_v<typename S::Sig::Result>) {
        same = same && imp.result == ir::kVoid;
    } else {
        same = same && matches<Plain<typename S::Sig::Result>>(m.types, imp.result);
        result = nameOf<Plain<typename S::Sig::Result>>(m.types);
    }
    if (!same) {
        std::string params;
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            ((params += (I ? ", " : "") + nameOf<typename S::template Arg<I>>(m.types)), ...);
        }(std::make_index_sequence<S::kCount>{});
        return std::unexpected(std::format("host import '{}' is {}, the C++ function is ({}) -> {}", imp.name,
                                           describe(m.types, imp.params, imp.result), params, result));
    }
    sandbox.bind(imp.name, &thunk<Fn, WithUser>, user);
    return {};
}

} // namespace bind_detail

/// A `@handle` value in a typed binding, of any handle type.
struct Handle {
    std::uint64_t id = 0;
};

template <std::size_t N>
struct TypeName {
    char text[N]{};

    constexpr TypeName(const char (&name)[N]) {
        for (std::size_t i = 0; i < N; ++i) {
            text[i] = name[i];
        }
    }
};

/// A `@handle` value whose type has this name (`HandleOf<"Player">`).
template <TypeName Name>
struct HandleOf {
    std::uint64_t id = 0;
};

namespace bind_detail {

template <>
struct Host<Handle> {
    static bool matches(const ir::TypeTable& types, ir::TypeId id) { return types.kind(id) == ir::TypeKind::Handle; }

    static std::string name() { return "Handle"; }

    static Handle get(const vm::Slot& s) { return {s.u}; }

    static bool put(vm::HostCall&, vm::Slot& s, Handle v) {
        s.u = v.id;
        return true;
    }
};

template <TypeName Name>
struct Host<HandleOf<Name>> {
    static bool matches(const ir::TypeTable& types, ir::TypeId id) {
        return types.kind(id) == ir::TypeKind::Handle && types.info(id).name == Name.text;
    }

    static std::string name() { return Name.text; }

    static HandleOf<Name> get(const vm::Slot& s) { return {s.u}; }

    static bool put(vm::HostCall&, vm::Slot& s, HandleOf<Name> v) {
        s.u = v.id;
        return true;
    }
};

} // namespace bind_detail

/// Binds a C++ function to a host import (full name, or without the package while unambiguous), checking its
/// parameter and result types against the `.qzi` declaration.
template <auto Fn>
std::expected<void, std::string> bind(vm::Sandbox& sandbox, std::string_view importName) {
    static_assert(!bind_detail::Shape<Fn, false>::kMember,
                  "a member function needs its object: bind<&C::f>(sb, name, &c)");
    return bind_detail::bindChecked<Fn, false>(sandbox, importName, nullptr);
}

/// The same for a member function called on `user`, or a free function whose first parameter (after an optional
/// `vm::HostCall&`) is the `user` pointer.
template <auto Fn, typename U>
std::expected<void, std::string> bind(vm::Sandbox& sandbox, std::string_view importName, U* user) {
    return bind_detail::bindChecked<Fn, true>(sandbox, importName, const_cast<void*>(static_cast<const void*>(user)));
}

} // namespace quartz
