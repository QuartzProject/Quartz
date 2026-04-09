#include "syntax/DocComment.hpp"

#include <vector>

namespace quartz::syntax {

namespace {

std::string_view trimmed(std::string_view line) {
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
        line.remove_prefix(1);
    }
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) {
        line.remove_suffix(1);
    }
    return line;
}

} // namespace

std::string docComment(std::string_view text, std::uint32_t offset) {
    if (offset > text.size()) {
        return {};
    }
    std::size_t lineStart = text.rfind('\n', offset == 0 ? 0 : offset - 1);
    lineStart = lineStart == std::string_view::npos ? 0 : lineStart + 1;
    std::vector<std::string_view> lines;
    while (lineStart > 0) {
        std::size_t end = lineStart - 1;
        std::size_t begin = text.rfind('\n', end == 0 ? 0 : end - 1);
        begin = begin == std::string_view::npos || end == 0 ? 0 : begin + 1;
        std::string_view line = trimmed(text.substr(begin, end - begin));
        lineStart = begin;
        if (line.starts_with("///") && !line.starts_with("////")) {
            line.remove_prefix(3);
            lines.push_back(line.starts_with(' ') ? line.substr(1) : line);
        } else if (line.starts_with('@') && lines.empty()) {
            continue;
        } else {
            break;
        }
    }
    std::string out;
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        out += (out.empty() ? "" : "\n") + std::string(*it);
    }
    return out;
}

} // namespace quartz::syntax
