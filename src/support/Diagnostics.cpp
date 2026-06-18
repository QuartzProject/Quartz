#include "support/Diagnostics.hpp"

#include <format>

namespace quartz {

void Diagnostics::report(Severity severity, Span span, std::string message) {
    if (severity == Severity::Error) {
        ++errorCount_;
    } else if (severity == Severity::Warning) {
        ++warningCount_;
    }
    if (items_.size() < kMaxDiagnostics) {
        items_.push_back({severity, span, std::move(message)});
    }
}

std::string Diagnostics::renderOne(const Diagnostic& d) const {
    const char* label = d.severity == Severity::Error ? "error" : d.severity == Severity::Warning ? "warning" : "note";
    if (d.span.file >= sources_->size()) {
        return std::format("{}: {}\n", label, d.message);
    }
    const SourceFile& file = sources_->file(d.span.file);
    LineCol at = file.lineCol(d.span.begin);
    std::string out = std::format("{}:{}:{}: {}: {}\n", file.name(), at.line, at.column, label, d.message);
    std::string_view line = file.lineText(at.line);
    out += std::format("    {}\n    ", line);
    out.append(at.column - 1, ' ');
    std::uint32_t width = d.span.end > d.span.begin ? d.span.end - d.span.begin : 1;
    std::uint32_t room = static_cast<std::uint32_t>(line.size()) + 1 - (at.column - 1);
    out.append(width < room ? width : room, '^');
    out += '\n';
    return out;
}

std::string Diagnostics::render() const {
    std::string out;
    for (const Diagnostic& d : items_) {
        out += renderOne(d);
    }
    return out;
}

} // namespace quartz
