#pragma once

#include "support/Source.hpp"

#include <string>
#include <vector>

namespace quartz {

enum class Severity : std::uint8_t { Error, Warning, Note };

struct Diagnostic {
    Severity severity;
    Span span;
    std::string message;
};

class Diagnostics {
public:
    explicit Diagnostics(const SourceManager& sources) : sources_(&sources) {}

    void error(Span span, std::string message) { report(Severity::Error, span, std::move(message)); }
    void warning(Span span, std::string message) { report(Severity::Warning, span, std::move(message)); }
    void note(Span span, std::string message) { report(Severity::Note, span, std::move(message)); }

    [[nodiscard]] bool hasErrors() const { return errorCount_ > 0; }
    [[nodiscard]] std::size_t errorCount() const { return errorCount_; }

    [[nodiscard]] std::size_t warningCount() const { return warningCount_; }
    [[nodiscard]] const std::vector<Diagnostic>& all() const { return items_; }
    [[nodiscard]] std::string render() const;
    [[nodiscard]] std::string renderOne(const Diagnostic& d) const;

private:
    void report(Severity severity, Span span, std::string message);

    static constexpr std::size_t kMaxDiagnostics = 200;
    const SourceManager* sources_;
    std::vector<Diagnostic> items_;
    std::size_t errorCount_ = 0;
    std::size_t warningCount_ = 0;
};

} // namespace quartz
