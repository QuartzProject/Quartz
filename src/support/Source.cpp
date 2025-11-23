#include "support/Source.hpp"

#include <algorithm>

namespace quartz {

SourceFile::SourceFile(std::uint32_t id, std::string name, std::string text)
    : id_(id), name_(std::move(name)), text_(std::move(text)) {
    lineStarts_.push_back(0);
    for (std::uint32_t i = 0; i < text_.size(); ++i) {
        if (text_[i] == '\n') {
            lineStarts_.push_back(i + 1);
        }
    }
}

LineCol SourceFile::lineCol(std::uint32_t offset) const {
    auto it = std::upper_bound(lineStarts_.begin(), lineStarts_.end(), offset);
    auto line = static_cast<std::uint32_t>(it - lineStarts_.begin());
    return {line, offset - lineStarts_[line - 1] + 1};
}

std::string_view SourceFile::lineText(std::uint32_t line) const {
    if (line == 0 || line > lineStarts_.size()) {
        return {};
    }
    std::uint32_t begin = lineStarts_[line - 1];
    std::uint32_t end = line < lineStarts_.size() ? lineStarts_[line] : static_cast<std::uint32_t>(text_.size());
    while (end > begin && (text_[end - 1] == '\n' || text_[end - 1] == '\r')) {
        --end;
    }
    return std::string_view(text_).substr(begin, end - begin);
}

const SourceFile& SourceManager::add(std::string name, std::string text) {
    auto id = static_cast<std::uint32_t>(files_.size());
    files_.push_back(std::make_unique<SourceFile>(id, std::move(name), std::move(text)));
    return *files_.back();
}

} // namespace quartz
