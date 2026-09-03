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

namespace {

void makeNames(const qz_program& p) {
    const quartz::ir::Module& m = *p.program->module;
    p.functionNames.reserve(m.functions.size());
    for (std::uint32_t f = 0; f < m.functions.size(); ++f) {
        const quartz::ir::Function& fn = m.functions[f];
        p.functionNames.push_back(fn.package.empty() ? fn.name : fn.package + "." + fn.name);
        if (fn.exported) {
            p.exports.push_back(f);
        }
    }
    p.attributes.resize(m.functions.size());
    for (const quartz::ir::Annotation& a : m.annotations) {
        if (a.function < p.attributes.size()) {
            p.attributes[a.function].push_back(&a);
        }
    }
    p.typeNames.reserve(m.types.size());
    for (quartz::ir::TypeId t = 0; t < m.types.size(); ++t) {
        p.typeNames.push_back(m.types.name(t));
    }
}

const qz_program* named(const qz_program* p) {
    std::call_once(p->namesOnce, makeNames, *p);
    return p;
}

const quartz::ir::Annotation* attributeOf(const qz_program* p, uint32_t function, uint32_t attribute) {
    const auto& all = named(p)->attributes;
    return function < all.size() && attribute < all[function].size() ? all[function][attribute] : nullptr;
}

const quartz::ir::Function* functionOf(const qz_program* p, uint32_t function) {
    const auto& fns = p->program->module->functions;
    return function < fns.size() ? &fns[function] : nullptr;
}

const quartz::ir::HostImport* importOf(const qz_program* p, uint32_t import) {
    const auto& imports = p->program->module->imports;
    return import < imports.size() ? &imports[import] : nullptr;
}

qz_lookup lookupResult(const std::expected<std::uint32_t, quartz::vm::LookupError>& found, uint32_t* index) {
    if (index != nullptr) {
        *index = found.value_or(UINT32_MAX);
    }
    if (found) {
        return QZ_LOOKUP_FOUND;
    }
    return found.error() == quartz::vm::LookupError::Ambiguous ? QZ_LOOKUP_AMBIGUOUS : QZ_LOOKUP_NOT_FOUND;
}

} // namespace

extern "C" {

qz_lookup qz_program_find_function(const qz_program* program, const char* name, uint32_t* index) {
    if (program == nullptr || name == nullptr) {
        return lookupResult(std::unexpected(quartz::vm::LookupError::NotFound), index);
    }
    return lookupResult(program->program->lookupFunction(name), index);
}

qz_lookup qz_program_find_import(const qz_program* program, const char* name, uint32_t* index) {
    if (program == nullptr || name == nullptr) {
        return lookupResult(std::unexpected(quartz::vm::LookupError::NotFound), index);
    }
    return lookupResult(program->program->lookupImport(name), index);
}

uint32_t qz_program_export_count(const qz_program* program) {
    return static_cast<uint32_t>(named(program)->exports.size());
}

uint32_t qz_program_export(const qz_program* program, uint32_t index) {
    const auto& exports = named(program)->exports;
    return index < exports.size() ? exports[index] : UINT32_MAX;
}

const char* qz_program_function_name(const qz_program* program, uint32_t function) {
    const auto& names = named(program)->functionNames;
    return function < names.size() ? names[function].c_str() : "";
}

const char* qz_program_function_package(const qz_program* program, uint32_t function) {
    const quartz::ir::Function* f = functionOf(program, function);
    return f != nullptr ? f->package.c_str() : "";
}

const char* qz_program_function_param_name(const qz_program* program, uint32_t function, uint32_t index) {
    const quartz::ir::Function* f = functionOf(program, function);
    if (f == nullptr || f->paramNames.size() != f->paramCount) {
        return "";
    }
    const quartz::ir::TypeTable& types = program->program->module->types;
    for (std::uint32_t r = 0; r < f->paramCount; ++r) {
        if (types.kind(f->regs[r]) == quartz::ir::TypeKind::State) {
            continue; // the sandbox supplies states, so callers do not count them
        }
        if (index-- == 0) {
            return f->paramNames[r].c_str();
        }
    }
    return "";
}

qz_visibility qz_program_function_visibility(const qz_program* program, uint32_t function) {
    const quartz::ir::Function* f = functionOf(program, function);
    return f != nullptr ? static_cast<qz_visibility>(f->visibility) : QZ_VISIBILITY_DEFAULT;
}

bool qz_program_function_is_suspend(const qz_program* program, uint32_t function) {
    const quartz::ir::Function* f = functionOf(program, function);
    return f != nullptr && f->isSuspend;
}

bool qz_program_function_is_exported(const qz_program* program, uint32_t function) {
    const quartz::ir::Function* f = functionOf(program, function);
    return f != nullptr && f->exported;
}

const char* qz_program_import_package(const qz_program* program, uint32_t import) {
    const quartz::ir::HostImport* imp = importOf(program, import);
    return imp != nullptr ? imp->package.c_str() : "";
}

bool qz_program_import_is_required(const qz_program* program, uint32_t import) {
    const auto& required = program->program->importRequired;
    return import < required.size() && required[import];
}

uint32_t qz_program_function_attribute_count(const qz_program* program, uint32_t function) {
    const auto& all = named(program)->attributes;
    return function < all.size() ? static_cast<uint32_t>(all[function].size()) : 0;
}

const char* qz_program_function_attribute_name(const qz_program* program, uint32_t function, uint32_t attribute) {
    const quartz::ir::Annotation* a = attributeOf(program, function, attribute);
    return a != nullptr ? a->name.c_str() : "";
}

uint32_t qz_program_function_attribute_filter(const qz_program* program, uint32_t function, uint32_t attribute) {
    const quartz::ir::Annotation* a = attributeOf(program, function, attribute);
    return a != nullptr && a->filter != ~0U ? a->filter : UINT32_MAX;
}

uint32_t qz_program_function_attribute_arg_count(const qz_program* program, uint32_t function, uint32_t attribute) {
    const quartz::ir::Annotation* a = attributeOf(program, function, attribute);
    return a != nullptr ? static_cast<uint32_t>(a->args.size()) : 0;
}

bool qz_program_function_attribute_arg(const qz_program* program, uint32_t function, uint32_t attribute, uint32_t index,
                                       qz_attribute_arg* out) {
    const quartz::ir::Annotation* a = attributeOf(program, function, attribute);
    if (a == nullptr || index >= a->args.size() || out == nullptr) {
        return false;
    }
    const quartz::ir::AnnotationArg& arg = a->args[index];
    *out = {arg.key.c_str(), static_cast<qz_attribute_arg_kind>(arg.kind), arg.intValue, arg.floatValue,
            arg.text.c_str()};
    return true;
}

const char* qz_type_name(const qz_program* program, uint32_t type) {
    const auto& names = named(program)->typeNames;
    return type != 0 && type < names.size() ? names[type].c_str() : "";
}

const char* qz_type_package(const qz_program* program, uint32_t type) {
    const quartz::ir::TypeTable& types = program->program->module->types;
    return type != 0 && type < types.size() ? types.info(type).package.c_str() : "";
}

} // extern "C"

namespace {

std::string_view bytesView(const uint8_t* data, size_t length) {
    return {reinterpret_cast<const char*>(data), data != nullptr ? length : 0};
}

} // namespace

extern "C" {

qz_slot qz_sandbox_bytes(qz_sandbox* sandbox, const uint8_t* data, size_t length) {
    qz_slot out{};
    out.p = sandbox->sandbox->newBytes(bytesView(data, length));
    return out;
}

qz_slot qz_call_bytes(qz_call* call, const uint8_t* data, size_t length) {
    qz_slot out{};
    out.p = call->call.bytes(bytesView(data, length));
    return out;
}

void qz_sandbox_bytes_into(qz_sandbox* sandbox, const uint8_t* data, size_t length, qz_slot* out) {
    *out = qz_sandbox_bytes(sandbox, data, length);
}

void qz_call_bytes_into(qz_call* call, const uint8_t* data, size_t length, qz_slot* out) {
    *out = qz_call_bytes(call, data, length);
}

} // extern "C"
