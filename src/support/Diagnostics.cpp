#include "support/Diagnostics.hpp"

#include <algorithm>
#include <format>

namespace quartz {

std::span<const std::string_view> warningRules() {
    static constexpr std::string_view kRules[] = {
        rule::kUnusedVariable,  rule::kUnusedParameter,      rule::kUnusedAssignment, rule::kUnusedImport,
        rule::kUnusedPrivate,   rule::kDiscardedResult,      rule::kShadowedVariable, rule::kUnreachablePattern,
        rule::kUnreachableCode, rule::kDeprecated,           rule::kUnknownAttribute, rule::kUnscopedWaitLoop,
        rule::kUnknownRule,     rule::kLiteralInterpolation,
    };
    return kRules;
}

void Diagnostics::setLevels(std::span<const RuleSetting> settings) {
    levels_.assign(settings.begin(), settings.end());
    std::span<const std::string_view> known = warningRules();
    for (const RuleSetting& s : settings) {
        if (s.rule != "*" && std::find(known.begin(), known.end(), s.rule) == known.end()) {
            warning({~0U, 0, 0}, rule::kUnknownRule, std::format("unknown warning rule '{}'", s.rule));
        }
    }
}

RuleLevel Diagnostics::levelOf(std::string_view rule) const {
    RuleLevel level = RuleLevel::Warning;
    for (const RuleSetting& s : levels_) {
        if (s.rule == "*" || s.rule == rule) {
            level = s.level;
        }
    }
    return level;
}

void Diagnostics::report(Severity severity, Span span, std::string message, std::string_view rule) {
    if (severity == Severity::Warning && !levels_.empty() && levelOf(rule) == RuleLevel::Ignore) {
        return;
    }
    if (severity == Severity::Error) {
        ++errorCount_;
    } else if (severity == Severity::Warning) {
        ++warningCount_;
    }
    if (items_.size() < kMaxDiagnostics) {
        items_.push_back({severity, span, std::move(message), rule});
    }
}

void Diagnostics::applyLevels() {
    if (levels_.empty()) {
        return;
    }
    for (Diagnostic& d : items_) {
        if (d.severity == Severity::Warning && levelOf(d.rule) == RuleLevel::Error) {
            d.severity = Severity::Error;
            --warningCount_;
            ++errorCount_;
        }
    }
}

std::string Diagnostics::renderOne(const Diagnostic& d) const {
    const char* label = d.severity == Severity::Error ? "error" : d.severity == Severity::Warning ? "warning" : "note";
    std::string message = d.rule.empty() ? d.message : std::format("{} [{}]", d.message, d.rule);
    if (d.span.file >= sources_->size()) {
        return std::format("{}: {}\n", label, message);
    }
    const SourceFile& file = sources_->file(d.span.file);
    LineCol at = file.lineCol(d.span.begin);
    std::string out = std::format("{}:{}:{}: {}: {}\n", file.name(), at.line, at.column, label, message);
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
