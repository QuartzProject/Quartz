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

template <typename F>
struct Signature;

template <typename R, typename... A>
struct Signature<R (*)(A...)> {
    using Result = R;
    using Args = std::tuple<A...>;
};

// A first parameter of type `vm::HostCall&` receives the call itself (to charge fuel, reach the sandbox or fail).
template <typename Args>
constexpr bool kTakesCall = false;
template <typename First, typename... Rest>
constexpr bool kTakesCall<std::tuple<First, Rest...>> = std::is_same_v<First, vm::HostCall&>;

template <typename Args, std::size_t Skip>
struct Params;

template <typename... A, std::size_t Skip>
struct Params<std::tuple<A...>, Skip> {
    template <std::size_t... I>
    static std::vector<ir::TypeId> types(std::index_sequence<I...>) {
        return {Host<Plain<std::tuple_element_t<I + Skip, std::tuple<A...>>>>::kType...};
    }
};

std::string describe(const ir::TypeTable& types, std::span<const ir::TypeId> params, ir::TypeId result);

template <auto Fn>
bool thunk(vm::HostCall& call, void*) {
    using Sig = Signature<decltype(Fn)>;
    constexpr std::size_t skip = kTakesCall<typename Sig::Args> ? 1 : 0;
    constexpr std::size_t count = std::tuple_size_v<typename Sig::Args> - skip;
    auto invoke = [&]<std::size_t... I>(std::index_sequence<I...>) -> decltype(auto) {
        if constexpr (skip == 1) {
            return Fn(call, Host<Plain<std::tuple_element_t<I + 1, typename Sig::Args>>>::get(call.args[I])...);
        } else {
            return Fn(Host<Plain<std::tuple_element_t<I, typename Sig::Args>>>::get(call.args[I])...);
        }
    };
    if constexpr (std::is_void_v<typename Sig::Result>) {
        invoke(std::make_index_sequence<count>{});
        return call.error.empty();
    } else {
        if (!Host<Plain<typename Sig::Result>>::put(call, call.result, invoke(std::make_index_sequence<count>{}))) {
            return false;
        }
        return call.error.empty();
    }
}

} // namespace bind_detail

/// Binds a C++ function to a host import, checking its parameter and result types against the `.qzi` declaration.
template <auto Fn>
std::expected<void, std::string> bind(vm::Sandbox& sandbox, std::string_view importName) {
    using Sig = bind_detail::Signature<decltype(Fn)>;
    constexpr std::size_t skip = bind_detail::kTakesCall<typename Sig::Args> ? 1 : 0;
    constexpr std::size_t count = std::tuple_size_v<typename Sig::Args> - skip;
    std::vector<ir::TypeId> params =
        bind_detail::Params<typename Sig::Args, skip>::types(std::make_index_sequence<count>{});
    ir::TypeId result = ir::kVoid;
    if constexpr (!std::is_void_v<typename Sig::Result>) {
        result = bind_detail::Host<bind_detail::Plain<typename Sig::Result>>::kType;
    }
    const ir::Module& m = *sandbox.program().module;
    for (const ir::HostImport& imp : m.imports) {
        if (imp.name != importName) {
            continue;
        }
        if (imp.params != params || imp.result != result) {
            return std::unexpected(std::format("host import '{}' is {}, the C++ function is {}", importName,
                                               bind_detail::describe(m.types, imp.params, imp.result),
                                               bind_detail::describe(m.types, params, result)));
        }
        sandbox.bind(importName, &bind_detail::thunk<Fn>);
        return {};
    }
    return std::unexpected(std::format("the program has no host import '{}'", importName));
}

} // namespace quartz
