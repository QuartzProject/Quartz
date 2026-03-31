#pragma once

#include "Workspace.hpp"

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace quartz::lsp {

enum class EntityKind : std::uint8_t { Local, Param, Function, Method, Struct, Enum, Trait, Field, Variant, Const };

/// One name in a user file that resolves to a declaration.
struct Occurrence {
    Span span;
    const void* key; // the declaration node, shared by every occurrence of the same entity
    EntityKind kind;
    std::uint32_t declFile;
    bool declaration;
    bool exact; // the text is the declared name, not an import alias or `Self`
    bool mutable_;
};

/// Every resolved name of the program's user files, built once per analysis.
class Index {
public:
    explicit Index(const Analysis& a);

    /// Occurrences of `file` ordered by position.
    [[nodiscard]] std::span<const Occurrence> inFile(std::uint32_t file) const;
    /// The occurrence under `offset`, or one ending right at it.
    [[nodiscard]] const Occurrence* at(std::uint32_t file, std::uint32_t offset) const;
    [[nodiscard]] std::vector<const Occurrence*> of(const void* key) const;
    [[nodiscard]] bool declaredInUserFile(const void* key) const;

    [[nodiscard]] std::span<const Occurrence> all() const { return all_; }

private:
    std::vector<Occurrence> all_;
    std::vector<std::uint32_t> fileStart_; // first occurrence of each file
    std::unordered_map<const void*, std::vector<std::uint32_t>> byKey_;
};

} // namespace quartz::lsp
