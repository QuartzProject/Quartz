#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace quartz::toml {

/// One `key = value`; strings hold no quote, backslash or control character, so they print back unchanged.
struct Entry {
    enum class Kind : std::uint8_t { String, Integer, Bool, Strings };
    std::string key;
    Kind kind = Kind::String;
    std::string text;
    std::int64_t integer = 0;
    bool flag = false;
    std::vector<std::string> list;
    std::size_t line = 0;
};

/// The root table (empty name) first, then each `[table]` or `[[array]]` element in order.
struct Section {
    std::string name;
    bool array = false;
    std::size_t line = 0;
    std::vector<Entry> entries;

    [[nodiscard]] const Entry* find(std::string_view key) const;
};

/// The TOML subset the registry files use: one value per line, no escapes, no inline tables.
std::expected<std::vector<Section>, std::string> parse(std::string_view text, std::string_view file);

/// True when `s` can be written as a TOML basic string without escapes.
bool plainString(std::string_view s);

/// `"a", "b"` for a list.
std::string quoteList(const std::vector<std::string>& items);

} // namespace quartz::toml
