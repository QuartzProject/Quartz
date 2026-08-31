#pragma once

#include "support/Source.hpp"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace quartz {

enum class Severity : std::uint8_t { Error, Warning, Note };

/// Stable ids of the warning rules: hosts and build files name them to change their level.
namespace rule {
inline constexpr std::string_view kUnusedVariable = "unused-variable";
inline constexpr std::string_view kUnusedParameter = "unused-parameter";
inline constexpr std::string_view kUnusedAssignment = "unused-assignment";
inline constexpr std::string_view kUnusedImport = "unused-import";
inline constexpr std::string_view kUnusedPrivate = "unused-private";
inline constexpr std::string_view kDiscardedResult = "discarded-result";
inline constexpr std::string_view kShadowedVariable = "shadowed-variable";
inline constexpr std::string_view kUnreachablePattern = "unreachable-pattern";
inline constexpr std::string_view kUnreachableCode = "unreachable-code";
inline constexpr std::string_view kDeprecated = "deprecated";
inline constexpr std::string_view kUnknownAttribute = "unknown-attribute";
inline constexpr std::string_view kUnscopedWaitLoop = "unscoped-wait-loop";
inline constexpr std::string_view kUnknownRule = "unknown-rule";
} // namespace rule

/// Every warning rule id, in a stable order.
std::span<const std::string_view> warningRules();

enum class RuleLevel : std::uint8_t { Ignore, Warning, Error };

/// The level of one warning rule, or of every one with the rule `*`; later settings win.
struct RuleSetting {
    std::string rule;
    RuleLevel level = RuleLevel::Warning;
};

struct Diagnostic {
    Severity severity;
    Span span;
    std::string message;
    std::string_view rule{}; // a warning's rule id (one of warningRules), kept when it is raised to an error
};

class Diagnostics {
public:
    explicit Diagnostics(const SourceManager& sources) : sources_(&sources) {}

    void error(Span span, std::string message) { report(Severity::Error, span, std::move(message), {}); }

    void warning(Span span, std::string_view rule, std::string message) {
        report(Severity::Warning, span, std::move(message), rule);
    }

    void note(Span span, std::string message) { report(Severity::Note, span, std::move(message), {}); }

    /// Ignored rules are dropped as they are reported; raised ones become errors in `applyLevels`.
    void setLevels(std::span<const RuleSetting> settings);
    /// Raises the warnings whose rule is set to error; call it before deciding whether compilation failed.
    void applyLevels();

    [[nodiscard]] bool hasErrors() const { return errorCount_ > 0; }

    [[nodiscard]] std::size_t errorCount() const { return errorCount_; }

    [[nodiscard]] std::size_t warningCount() const { return warningCount_; }

    [[nodiscard]] const std::vector<Diagnostic>& all() const { return items_; }

    [[nodiscard]] std::string render() const;
    [[nodiscard]] std::string renderOne(const Diagnostic& d) const;

private:
    void report(Severity severity, Span span, std::string message, std::string_view rule);
    [[nodiscard]] RuleLevel levelOf(std::string_view rule) const;

    static constexpr std::size_t kMaxDiagnostics = 200;
    const SourceManager* sources_;
    std::vector<Diagnostic> items_;
    std::vector<RuleSetting> levels_;
    std::size_t errorCount_ = 0;
    std::size_t warningCount_ = 0;
};

} // namespace quartz
