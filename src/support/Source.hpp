#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace quartz {

struct Span {
    std::uint32_t file = 0;
    std::uint32_t begin = 0;
    std::uint32_t end = 0;

    [[nodiscard]] Span to(Span other) const { return {file, begin, other.end}; }
};

struct LineCol {
    std::uint32_t line = 1;
    std::uint32_t column = 1;
};

class SourceFile {
public:
    SourceFile(std::uint32_t id, std::string name, std::string text);

    [[nodiscard]] std::uint32_t id() const { return id_; }
    [[nodiscard]] const std::string& name() const { return name_; }
    [[nodiscard]] std::string_view text() const { return text_; }
    [[nodiscard]] LineCol lineCol(std::uint32_t offset) const;
    [[nodiscard]] std::string_view lineText(std::uint32_t line) const;
    [[nodiscard]] bool isInterface() const { return name_.ends_with(".qzi"); }

private:
    std::uint32_t id_;
    std::string name_;
    std::string text_;
    std::vector<std::uint32_t> lineStarts_;
};

class SourceManager {
public:
    const SourceFile& add(std::string name, std::string text);
    [[nodiscard]] const SourceFile& file(std::uint32_t id) const { return *files_[id]; }
    [[nodiscard]] std::size_t size() const { return files_.size(); }

private:
    std::vector<std::unique_ptr<SourceFile>> files_;
};

} // namespace quartz
