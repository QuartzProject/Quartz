#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace quartz::lsp {

struct Position {
    std::uint32_t line = 0;
    std::uint32_t character = 0; // UTF-16 code units, as LSP requires by default
};

/// Converts between byte offsets in UTF-8 text and LSP positions.
class LineIndex {
public:
    explicit LineIndex(std::string_view text);

    [[nodiscard]] Position position(std::uint32_t offset) const;
    [[nodiscard]] std::uint32_t offset(Position p) const;

    [[nodiscard]] Position end() const { return position(static_cast<std::uint32_t>(text_.size())); }

private:
    std::string_view text_;
    std::vector<std::uint32_t> starts_;
};

std::string uriToPath(std::string_view uri);
std::string pathToUri(std::string_view path);

} // namespace quartz::lsp
