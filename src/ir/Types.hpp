#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace quartz::ir {

using TypeId = std::uint32_t;

enum class TypeKind : std::uint8_t {
    Error,
    Void,
    Bool,
    I8,
    I16,
    I32,
    I64,
    U8,
    U16,
    U32,
    U64,
    F32,
    F64,
    Char,
    Str,
    Bytes,
    Null, // type of the `null` literal before it meets a nullable type
    Never, // type of `return`, `break` and other diverging expressions
    Handle,
    List,
    Map,
    Tuple,
    Struct,
    State,
    Enum,
    Nullable,
    Fn,
    Param, // a generic type parameter; never reaches validated IR
};

inline constexpr TypeId kError = 0;
inline constexpr TypeId kVoid = 1;
inline constexpr TypeId kBool = 2;
inline constexpr TypeId kI8 = 3;
inline constexpr TypeId kI16 = 4;
inline constexpr TypeId kI32 = 5;
inline constexpr TypeId kI64 = 6;
inline constexpr TypeId kU8 = 7;
inline constexpr TypeId kU16 = 8;
inline constexpr TypeId kU32 = 9;
inline constexpr TypeId kU64 = 10;
inline constexpr TypeId kF32 = 11;
inline constexpr TypeId kF64 = 12;
inline constexpr TypeId kChar = 13;
inline constexpr TypeId kStr = 14;
inline constexpr TypeId kBytes = 15;
inline constexpr TypeId kNull = 16;
inline constexpr TypeId kNever = 17;
inline constexpr TypeId kFirstUserType = 18;
/// Longest fixed-size array `[T; N]`; its elements live inline, so this bounds registers and copies.
inline constexpr std::uint32_t kMaxArrayLength = 4096;

struct Field {
    std::string name;
    TypeId type = kError;
    std::string renamedFrom; // `@renamed("old")`: the name in older versions of a state
};

struct VariantInfo {
    std::string name;
    std::vector<TypeId> payload;
};

struct TypeInfo {
    TypeKind kind = TypeKind::Error;
    std::string name;            // nominal types and handles
    TypeId elem = kError;        // List, Nullable; Map value
    TypeId key = kError;         // Map key
    TypeId result = kVoid;       // Fn
    std::vector<TypeId> elems;   // Tuple elements, Fn params
    std::vector<Field> fields;   // Struct, State
    std::vector<VariantInfo> variants;
    bool complete = true;        // nominal types are declared first and completed later
    std::uint32_t version = 0;   // `@version(n)` of a state
    bool persisted = false;      // `@persist`: the host saves it (exportState)
    bool reprC = false;          // `@repr(C)`: crosses to the host as a C struct
    bool array = false;          // a tuple whose `elems` are N copies of one type, written `[T; N]` (D144)
    TypeId persistPer = 0;       // `@persist(per = Handle)`: one instance per handle value
    std::vector<TypeId> typeParams; // generic bases: their parameters (Param types)
    TypeId genericBase = 0;         // instances: the generic base they come from (0 = none)
    std::vector<TypeId> typeArgs;   // instances: the arguments
    std::string package{};          // nominal types: the package declaring them
};

using TypeSubst = std::vector<std::pair<TypeId, TypeId>>;

/// Owns every type of a program; structural types are interned so equal types share one id.
class TypeTable {
public:
    TypeTable();

    [[nodiscard]] const TypeInfo& info(TypeId id) const { return types_[id]; }
    [[nodiscard]] TypeKind kind(TypeId id) const { return types_[id].kind; }
    [[nodiscard]] std::size_t size() const { return types_.size(); }

    TypeId list(TypeId elem);
    TypeId map(TypeId key, TypeId value);
    TypeId nullable(TypeId inner);
    TypeId tuple(const std::vector<TypeId>& elems);
    /// `[elem; n]`: a Tuple of n equal elements, distinct from the tuple with the same elements.
    TypeId array(TypeId elem, std::uint32_t n);

    [[nodiscard]] bool isArray(TypeId id) const { return types_[id].array; }
    TypeId fn(const std::vector<TypeId>& params, TypeId result);
    /// `Result<T, E>` is an enum with variants `Ok(T)` and `Err(E)`.
    TypeId result(TypeId ok, TypeId err);
    [[nodiscard]] bool isResult(TypeId id) const;
    TypeId param(std::string name);
    /// `Base<args>`: a nominal type whose fields are the base's with parameters replaced.
    TypeId instantiate(TypeId base, const std::vector<TypeId>& args);
    TypeId subst(TypeId t, const TypeSubst& map);
    [[nodiscard]] bool hasParams(TypeId t) const;
    /// Recomputes instance fields once every generic base is complete.
    void refreshInstances();
    [[nodiscard]] bool isGenericBase(TypeId t) const { return !types_[t].typeParams.empty(); }
    TypeId declareNominal(TypeKind kind, std::string name);
    TypeInfo& mutableInfo(TypeId id) { return types_[id]; }
    /// Adds a fully described type as read from a serialized module; structural types are interned.
    TypeId addRaw(TypeInfo info);

    [[nodiscard]] std::string name(TypeId id) const;

    [[nodiscard]] bool isInteger(TypeId id) const;
    [[nodiscard]] bool isSigned(TypeId id) const;
    [[nodiscard]] bool isFloat(TypeId id) const { return id == kF32 || id == kF64; }
    [[nodiscard]] bool isNumeric(TypeId id) const { return isInteger(id) || isFloat(id); }
    [[nodiscard]] bool isScalar(TypeId id) const { return id >= kBool && id <= kChar; }
    [[nodiscard]] int bitWidth(TypeId id) const;
    /// True when a value of this type holds a pointer the runtime must track (strings, collections, state).
    [[nodiscard]] bool hasRefs(TypeId id) const;
    [[nodiscard]] bool isRefKind(TypeId id) const;
    /// True when a value of this type can hold a function value, in itself or in its fields, elements or payloads.
    [[nodiscard]] bool containsFn(TypeId id) const;
    /// True when the host can build a value of this type: no state, function or type parameter anywhere in it.
    [[nodiscard]] bool hostBuildable(TypeId id) const;

private:
    TypeId intern(TypeInfo info, const std::string& key);

    std::vector<TypeInfo> types_;
    std::map<std::string, TypeId> structural_;
    std::map<std::pair<TypeId, std::vector<TypeId>>, TypeId> instances_;
};

} // namespace quartz::ir
