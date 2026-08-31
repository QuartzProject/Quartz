#include "capi/CApiImpl.hpp"

#include <cstring>
#include <span>

namespace quartz::capi {

CompileOutput compileSources(const qz_source* sources, std::size_t count, const qz_compile_options* options,
                             std::vector<RuleSetting> rules) {
    std::vector<SourceInput> inputs;
    inputs.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        inputs.push_back({sources[i].name != nullptr ? sources[i].name : "<source>",
                          std::string(sources[i].text, sources[i].length)});
    }
    CompileOptions opts;
    if (options != nullptr) {
        opts.emitBinary = options->emit_binary;
        opts.binaryLines = options->keep_lines;
        opts.manifest = options->manifest != nullptr ? options->manifest : "";
    }
    opts.rules = std::move(rules);
    return compile(inputs, opts);
}

} // namespace quartz::capi

namespace {

const quartz::CompileMessage* messageAt(const qz_diagnostics* d, std::size_t index) {
    return d != nullptr && index < d->items.size() ? &d->items[index] : nullptr;
}

} // namespace

extern "C" {

qz_program* qz_compile_with(const qz_source* sources, size_t count, const qz_compile_options* options,
                            const qz_rule_level* levels, size_t level_count, qz_diagnostics** diagnostics) {
    if (diagnostics != nullptr) {
        *diagnostics = nullptr;
    }
    try {
        std::vector<quartz::RuleSetting> rules;
        for (std::size_t i = 0; i < level_count; ++i) {
            auto level = levels[i].level == QZ_LEVEL_IGNORE  ? quartz::RuleLevel::Ignore
                         : levels[i].level == QZ_LEVEL_ERROR ? quartz::RuleLevel::Error
                                                             : quartz::RuleLevel::Warning;
            rules.push_back({levels[i].rule != nullptr ? levels[i].rule : "", level});
        }
        quartz::CompileOutput out = quartz::capi::compileSources(sources, count, options, std::move(rules));
        if (diagnostics != nullptr) {
            *diagnostics = new qz_diagnostics{std::move(out.diagnostics), std::move(out.messages)};
        }
        return out.program ? new qz_program{std::move(out.program), std::move(out.binary)} : nullptr;
    } catch (...) {
        return nullptr;
    }
}

void qz_diagnostics_free(qz_diagnostics* diagnostics) { delete diagnostics; }

size_t qz_diagnostics_count(const qz_diagnostics* diagnostics) {
    return diagnostics != nullptr ? diagnostics->items.size() : 0;
}

const char* qz_diagnostics_text(const qz_diagnostics* diagnostics) {
    return diagnostics != nullptr ? diagnostics->text.c_str() : "";
}

qz_severity qz_diagnostic_severity(const qz_diagnostics* diagnostics, size_t index) {
    const quartz::CompileMessage* m = messageAt(diagnostics, index);
    if (m == nullptr || m->severity == quartz::Severity::Error) {
        return QZ_SEVERITY_ERROR;
    }
    return m->severity == quartz::Severity::Warning ? QZ_SEVERITY_WARNING : QZ_SEVERITY_NOTE;
}

const char* qz_diagnostic_rule(const qz_diagnostics* diagnostics, size_t index) {
    const quartz::CompileMessage* m = messageAt(diagnostics, index);
    return m != nullptr ? m->rule.c_str() : "";
}

const char* qz_diagnostic_file(const qz_diagnostics* diagnostics, size_t index) {
    const quartz::CompileMessage* m = messageAt(diagnostics, index);
    return m != nullptr ? m->file.c_str() : "";
}

const char* qz_diagnostic_message(const qz_diagnostics* diagnostics, size_t index) {
    const quartz::CompileMessage* m = messageAt(diagnostics, index);
    return m != nullptr ? m->message.c_str() : "";
}

void qz_diagnostic_range(const qz_diagnostics* diagnostics, size_t index, uint32_t* line, uint32_t* column,
                         uint32_t* end_line, uint32_t* end_column) {
    const quartz::CompileMessage* m = messageAt(diagnostics, index);
    quartz::CompileMessage none;
    const quartz::CompileMessage& at = m != nullptr ? *m : none;
    for (auto [out, value] : {std::pair{line, at.line}, std::pair{column, at.column}, std::pair{end_line, at.endLine},
                              std::pair{end_column, at.endColumn}}) {
        if (out != nullptr) {
            *out = value;
        }
    }
}

size_t qz_warning_rule_count(void) { return quartz::warningRules().size(); }

const char* qz_warning_rule(size_t index) {
    std::span<const std::string_view> rules = quartz::warningRules();
    return index < rules.size() ? rules[index].data() : "";
}

} // extern "C"
