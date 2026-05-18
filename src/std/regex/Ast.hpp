#pragma once

#include "std/regex/Regex.hpp"

namespace quartz::stdlib::regex {

enum class NodeKind : std::uint8_t { Empty, Set, Any, AnyNotNl, Assert, Group, Concat, Alternate, Repeat };

struct Node {
    NodeKind kind = NodeKind::Empty;
    Assert assertion = Assert::Begin;
    bool greedy = true;
    std::uint32_t set = 0;     // Set: index into Ast::sets
    std::uint32_t capture = 0; // Group
    std::uint32_t min = 0;     // Repeat
    std::uint32_t max = 0;     // Repeat; kNoPos is unbounded
    std::uint32_t pos = 0;     // byte offset in the pattern, for errors
    std::vector<std::uint32_t> kids{};
};

/// A parsed pattern; sets are sorted, disjoint and non-adjacent code point ranges.
struct Ast {
    std::vector<Node> nodes;
    std::vector<std::vector<Range>> sets;
    std::vector<std::string> names;
    std::uint32_t root = 0;
};

std::expected<Ast, Error> parse(std::string_view pattern);

inline constexpr std::uint32_t kMaxCodePoint = 0x10FFFF;

} // namespace quartz::stdlib::regex
