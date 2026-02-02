#include "Text.hpp"

#include <algorithm>

namespace quartz::lsp {

namespace {

// Length in bytes of the UTF-8 sequence starting with `lead`, and its UTF-16 width.
void utf8Step(unsigned char lead, std::uint32_t& bytes, std::uint32_t& units) {
    if (lead < 0x80) {
        bytes = 1;
        units = 1;
    } else if ((lead & 0xE0) == 0xC0) {
        bytes = 2;
        units = 1;
    } else if ((lead & 0xF0) == 0xE0) {
        bytes = 3;
        units = 1;
    } else if ((lead & 0xF8) == 0xF0) {
        bytes = 4;
        units = 2;
    } else {
        bytes = 1;
        units = 1;
    }
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

} // namespace

LineIndex::LineIndex(std::string_view text) : text_(text) {
    starts_.push_back(0);
    for (std::uint32_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n') {
            starts_.push_back(i + 1);
        }
    }
}

Position LineIndex::position(std::uint32_t offset) const {
    offset = std::min<std::uint32_t>(offset, static_cast<std::uint32_t>(text_.size()));
    auto it = std::upper_bound(starts_.begin(), starts_.end(), offset);
    auto line = static_cast<std::uint32_t>(it - starts_.begin() - 1);
    std::uint32_t at = starts_[line];
    std::uint32_t units = 0;
    while (at < offset) {
        std::uint32_t bytes = 0;
        std::uint32_t width = 0;
        utf8Step(static_cast<unsigned char>(text_[at]), bytes, width);
        at += bytes;
        units += width;
    }
    return {line, units};
}

std::uint32_t LineIndex::offset(Position p) const {
    if (p.line >= starts_.size()) {
        return static_cast<std::uint32_t>(text_.size());
    }
    std::uint32_t at = starts_[p.line];
    std::uint32_t lineEnd =
        p.line + 1 < starts_.size() ? starts_[p.line + 1] - 1 : static_cast<std::uint32_t>(text_.size());
    std::uint32_t units = 0;
    while (at < lineEnd && units < p.character) {
        std::uint32_t bytes = 0;
        std::uint32_t width = 0;
        utf8Step(static_cast<unsigned char>(text_[at]), bytes, width);
        at = std::min(at + bytes, lineEnd);
        units += width;
    }
    return at;
}

std::string uriToPath(std::string_view uri) {
    constexpr std::string_view kScheme = "file://";
    if (!uri.starts_with(kScheme)) {
        return {};
    }
    uri.remove_prefix(kScheme.size());
    std::size_t slash = uri.find('/');
    uri = slash == std::string_view::npos ? std::string_view{} : uri.substr(slash);
    std::string out;
    for (std::size_t i = 0; i < uri.size(); ++i) {
        if (uri[i] == '%' && i + 2 < uri.size() && hexValue(uri[i + 1]) >= 0 && hexValue(uri[i + 2]) >= 0) {
            out += static_cast<char>(hexValue(uri[i + 1]) * 16 + hexValue(uri[i + 2]));
            i += 2;
        } else {
            out += uri[i];
        }
    }
    // `/c:/dir` on Windows
    if (out.size() >= 3 && out[0] == '/' && out[2] == ':') {
        out.erase(0, 1);
    }
    return out;
}

std::string pathToUri(std::string_view path) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out = "file://";
    if (!path.starts_with('/')) {
        out += '/';
    }
    for (char c : path) {
        auto u = static_cast<unsigned char>(c);
        bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
                     c == '_' || c == '.' || c == '~' || c == '/';
        if (c == '\\') {
            out += '/';
        } else if (plain) {
            out += c;
        } else {
            out += '%';
            out += kHex[u >> 4];
            out += kHex[u & 0xF];
        }
    }
    return out;
}

} // namespace quartz::lsp
