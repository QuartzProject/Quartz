#include "quartz/Package.hpp"
#include "quartz/Quartz.hpp"
#include "quartz/quartz.h"
#include "vm/Layout.hpp"

#include <bit>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

struct qz_program {
    std::shared_ptr<const quartz::vm::Program> program;
    std::vector<std::uint8_t> binary;
};

struct qz_call {
    quartz::vm::HostCall& call;
    qz_sandbox* owner;
    std::size_t argSlots;
};

struct qz_sandbox {
    struct Binding {
        qz_host_fn fn;
        void* user;
        qz_sandbox* owner;
        qz_host_fn_direct direct = nullptr;
        std::size_t argSlots = 0;
    };

    std::unique_ptr<quartz::vm::Sandbox> sandbox;
    std::vector<std::unique_ptr<Binding>> bindings;
    std::string error;
    qz_trap trap = QZ_TRAP_NONE;
};

namespace {

static_assert(sizeof(qz_slot) == sizeof(quartz::vm::Slot));
static_assert(static_cast<int>(quartz::ir::TrapCode::HostError) + 1 == QZ_TRAP_HOST_ERROR);

quartz::vm::Slot toVm(qz_slot s) { return std::bit_cast<quartz::vm::Slot>(s); }

qz_slot toC(quartz::vm::Slot s) { return std::bit_cast<qz_slot>(s); }

char* copyOut(std::string_view text) {
    auto* out = static_cast<char*>(std::malloc(text.size() + 1));
    if (out != nullptr) {
        std::memcpy(out, text.data(), text.size());
        out[text.size()] = '\0';
    }
    return out;
}

void report(char** where, std::string_view text) {
    if (where != nullptr) {
        *where = copyOut(text);
    }
}

std::vector<quartz::vm::Slot> slots(const qz_slot* args, std::size_t count) {
    std::vector<quartz::vm::Slot> out(count);
    for (std::size_t i = 0; i < count; ++i) {
        out[i] = toVm(args[i]);
    }
    return out;
}

qz_task_outcome outcomeOf(const quartz::vm::TaskOutcome& o) {
    return {static_cast<qz_task_kind>(o.kind), o.task, toC(o.value)};
}

qz_slot* resultSlots(quartz::vm::HostCall& call) {
    quartz::vm::Slot* where = call.out != nullptr ? call.out : &call.result;
    return std::launder(static_cast<qz_slot*>(static_cast<void*>(where)));
}

bool trampoline(quartz::vm::HostCall& call, void* user) {
    auto* binding = static_cast<qz_sandbox::Binding*>(user);
    qz_call c{call, binding->owner, binding->argSlots};
    if (binding->direct != nullptr) {
        const auto* args = std::launder(static_cast<const qz_slot*>(static_cast<const void*>(call.args)));
        return binding->direct(&c, args, resultSlots(call), binding->user);
    }
    return binding->fn(&c, binding->user);
}

std::size_t importArgSlots(const quartz::vm::Program& p, std::string_view name) {
    for (const quartz::ir::HostImport& imp : p.module->imports) {
        if (imp.name == name) {
            std::size_t n = 0;
            for (quartz::ir::TypeId t : imp.params) {
                n += p.layouts->slots(t);
            }
            return n;
        }
    }
    return 0;
}

bool bindWith(qz_sandbox* sandbox, const char* import_name, qz_sandbox::Binding b) {
    if (sandbox == nullptr || import_name == nullptr) {
        return false;
    }
    try {
        b.owner = sandbox;
        b.argSlots = importArgSlots(sandbox->sandbox->program(), import_name);
        auto binding = std::make_unique<qz_sandbox::Binding>(b);
        if (!sandbox->sandbox->bind(import_name, trampoline, binding.get())) {
            return false;
        }
        sandbox->bindings.push_back(std::move(binding));
        return true;
    } catch (...) {
        return false;
    }
}

qz_kind kindOf(const quartz::ir::TypeTable& types, quartz::ir::TypeId t) {
    using K = quartz::ir::TypeKind;
    switch (types.kind(t)) {
    case K::Void:
        return QZ_KIND_VOID;
    case K::Bool:
        return QZ_KIND_BOOL;
    case K::I8:
        return QZ_KIND_I8;
    case K::I16:
        return QZ_KIND_I16;
    case K::I32:
        return QZ_KIND_I32;
    case K::I64:
        return QZ_KIND_I64;
    case K::U8:
        return QZ_KIND_U8;
    case K::U16:
        return QZ_KIND_U16;
    case K::U32:
        return QZ_KIND_U32;
    case K::U64:
        return QZ_KIND_U64;
    case K::F32:
        return QZ_KIND_F32;
    case K::F64:
        return QZ_KIND_F64;
    case K::Char:
        return QZ_KIND_CHAR;
    case K::Str:
        return QZ_KIND_STR;
    case K::Bytes:
        return QZ_KIND_BYTES;
    case K::List:
        return QZ_KIND_LIST;
    case K::Map:
        return QZ_KIND_MAP;
    case K::Struct:
        return QZ_KIND_STRUCT;
    case K::Enum:
        return QZ_KIND_ENUM;
    case K::Nullable:
        return QZ_KIND_NULLABLE;
    case K::Tuple:
        return QZ_KIND_TUPLE;
    case K::Handle:
        return QZ_KIND_HANDLE;
    case K::Fn:
        return QZ_KIND_FUNCTION;
    default:
        return QZ_KIND_OTHER;
    }
}

qz_kind describe(const quartz::vm::Program& p, quartz::ir::TypeId t, uint32_t* slots) {
    if (slots != nullptr) {
        *slots = static_cast<uint32_t>(p.layouts->slots(t));
    }
    return kindOf(p.module->types, t);
}

// Parameters a caller passes: the sandbox supplies `state` ones.
std::vector<quartz::ir::TypeId> callerParams(const quartz::vm::Program& p, uint32_t function) {
    std::vector<quartz::ir::TypeId> out;
    if (function < p.functions.size()) {
        for (quartz::ir::TypeId t : p.functions[function].params) {
            if (p.module->types.kind(t) != quartz::ir::TypeKind::State) {
                out.push_back(t);
            }
        }
    }
    return out;
}

// A C caller cannot catch C++ exceptions, so every entry point turns them into an error.
template <typename F>
qz_status guarded(qz_sandbox* sb, F&& body) {
    sb->trap = QZ_TRAP_NONE;
    try {
        return body();
    } catch (const std::bad_alloc&) {
        sb->error = "out of memory";
    } catch (...) {
        sb->error = "internal error";
    }
    return QZ_ERROR;
}

qz_status trapped(qz_sandbox* sb, const quartz::vm::Trap& trap) {
    sb->error = trap.describe();
    sb->trap = static_cast<qz_trap>(static_cast<int>(trap.code) + 1);
    return QZ_TRAP;
}

} // namespace

extern "C" {

uint32_t qz_abi_version(void) { return QZ_ABI_VERSION; }

void qz_free(void* memory) { std::free(memory); }

qz_program* qz_compile(const qz_source* sources, size_t count, const qz_compile_options* options, char** diagnostics) {
    try {
        std::vector<quartz::SourceInput> inputs;
        inputs.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            inputs.push_back({sources[i].name != nullptr ? sources[i].name : "<source>",
                              std::string(sources[i].text, sources[i].length)});
        }
        quartz::CompileOptions opts;
        if (options != nullptr) {
            opts.emitBinary = options->emit_binary;
            opts.binaryLines = options->keep_lines;
            opts.manifest = options->manifest != nullptr ? options->manifest : "";
        }
        quartz::CompileOutput out = quartz::compile(inputs, opts);
        report(diagnostics, out.diagnostics);
        if (!out.program) {
            return nullptr;
        }
        return new qz_program{std::move(out.program), std::move(out.binary)};
    } catch (...) {
        report(diagnostics, "out of memory");
        return nullptr;
    }
}

uint8_t* qz_program_binary(const qz_program* program, size_t* length) {
    if (program == nullptr || program->binary.empty()) {
        return nullptr;
    }
    auto* out = static_cast<uint8_t*>(std::malloc(program->binary.size()));
    if (out != nullptr) {
        std::memcpy(out, program->binary.data(), program->binary.size());
        if (length != nullptr) {
            *length = program->binary.size();
        }
    }
    return out;
}

qz_program* qz_load(const uint8_t* bytes, size_t length, const uint8_t (*trusted_keys)[32], size_t key_count,
                    bool require_signature, char** error) {
    try {
        std::vector<quartz::ir::PublicKey> keys(key_count);
        for (std::size_t i = 0; i < key_count; ++i) {
            std::memcpy(keys[i].data(), trusted_keys[i], 32);
        }
        quartz::ir::ReadOptions opts;
        opts.trustedKeys = keys;
        opts.requireSignature = require_signature;
        auto loaded = quartz::load(std::span(bytes, length), opts);
        if (!loaded) {
            report(error, loaded.error());
            return nullptr;
        }
        return new qz_program{std::move(*loaded), std::vector<std::uint8_t>(bytes, bytes + length)};
    } catch (...) {
        report(error, "out of memory");
        return nullptr;
    }
}

void qz_program_free(qz_program* program) { delete program; }

uint32_t qz_program_function(const qz_program* program, const char* name) {
    return program != nullptr && name != nullptr ? program->program->findFunction(name) : UINT32_MAX;
}

qz_sandbox* qz_sandbox_new(const qz_program* program, const qz_limits* limits) {
    if (program == nullptr) {
        return nullptr;
    }
    try {
        quartz::vm::Limits l;
        if (limits != nullptr) {
            l.memoryBytes = limits->memory_bytes != 0 ? limits->memory_bytes : l.memoryBytes;
            l.fuelPerCall = limits->fuel_per_call != 0 ? limits->fuel_per_call : l.fuelPerCall;
            l.maxDepth = limits->max_depth != 0 ? limits->max_depth : l.maxDepth;
            if (limits->tier >= 0 && limits->tier <= 3) {
                l.tier = static_cast<quartz::vm::TierMode>(limits->tier);
            }
        }
        auto sb = std::make_unique<qz_sandbox>();
        sb->sandbox = std::make_unique<quartz::vm::Sandbox>(program->program, l);
        return sb.release();
    } catch (...) {
        return nullptr;
    }
}

void qz_sandbox_free(qz_sandbox* sandbox) { delete sandbox; }

bool qz_sandbox_bind(qz_sandbox* sandbox, const char* import_name, qz_host_fn fn, void* user) {
    return fn != nullptr && bindWith(sandbox, import_name, {fn, user, nullptr});
}

bool qz_sandbox_bind_direct(qz_sandbox* sandbox, const char* import_name, qz_host_fn_direct fn, void* user) {
    return fn != nullptr && bindWith(sandbox, import_name, {nullptr, user, nullptr, fn});
}

qz_status qz_sandbox_start(qz_sandbox* sandbox) {
    return guarded(sandbox, [&] {
        sandbox->error.clear();
        auto r = sandbox->sandbox->start();
        return r ? QZ_OK : trapped(sandbox, r.error());
    });
}

qz_status qz_sandbox_call(qz_sandbox* sandbox, uint32_t function, const qz_slot* args, size_t count, qz_slot* result) {
    return guarded(sandbox, [&] {
        sandbox->error.clear();
        if (function >= sandbox->sandbox->program().functions.size()) {
            sandbox->error = "no such function";
            return QZ_ERROR;
        }
        auto r = sandbox->sandbox->call(function, slots(args, count));
        if (!r) {
            return trapped(sandbox, r.error());
        }
        if (result != nullptr) {
            *result = toC(*r);
        }
        return QZ_OK;
    });
}

qz_status qz_sandbox_spawn(qz_sandbox* sandbox, uint32_t function, const qz_slot* args, size_t count,
                           qz_task_outcome* outcome) {
    return guarded(sandbox, [&] {
        sandbox->error.clear();
        if (function >= sandbox->sandbox->program().functions.size()) {
            sandbox->error = "no such function";
            return QZ_ERROR;
        }
        auto r = sandbox->sandbox->spawn(function, slots(args, count));
        if (!r) {
            return trapped(sandbox, r.error());
        }
        if (outcome != nullptr) {
            *outcome = outcomeOf(*r);
        }
        return QZ_OK;
    });
}

qz_status qz_sandbox_resume(qz_sandbox* sandbox, uint32_t task, qz_slot value, qz_task_outcome* outcome) {
    return guarded(sandbox, [&] {
        sandbox->error.clear();
        auto r = sandbox->sandbox->resume(task, toVm(value));
        if (!r) {
            return trapped(sandbox, r.error());
        }
        if (outcome != nullptr) {
            *outcome = outcomeOf(*r);
        }
        return QZ_OK;
    });
}

void qz_sandbox_cancel(qz_sandbox* sandbox, uint32_t task) {
    if (sandbox != nullptr) {
        sandbox->sandbox->cancel(task);
    }
}

const char* qz_sandbox_error(const qz_sandbox* sandbox) { return sandbox != nullptr ? sandbox->error.c_str() : ""; }

qz_slot qz_sandbox_str(qz_sandbox* sandbox, const char* utf8, size_t length) {
    qz_slot out{};
    out.p = sandbox->sandbox->newStr(std::string_view(utf8, length));
    return out;
}

void qz_sandbox_set_fuel(qz_sandbox* sandbox, int64_t fuel_per_call) {
    sandbox->sandbox->setFuelPerCall(fuel_per_call);
}

int64_t qz_sandbox_fuel_left(const qz_sandbox* sandbox) { return sandbox->sandbox->fuelLeft(); }

size_t qz_sandbox_memory_used(const qz_sandbox* sandbox) { return sandbox->sandbox->memoryUsed(); }

char* qz_sandbox_export_state(qz_sandbox* sandbox) {
    try {
        auto r = sandbox->sandbox->exportState();
        if (!r) {
            sandbox->error = r.error().describe();
            return nullptr;
        }
        return copyOut(*r);
    } catch (...) {
        sandbox->error = "out of memory";
        return nullptr;
    }
}

qz_status qz_sandbox_import_state(qz_sandbox* sandbox, const char* json, size_t length) {
    return guarded(sandbox, [&] {
        sandbox->error.clear();
        auto r = sandbox->sandbox->importState(std::string_view(json, length));
        return r ? QZ_OK : trapped(sandbox, r.error());
    });
}

qz_status qz_sandbox_link(qz_sandbox* client, const char* package, qz_sandbox* provider) {
    return guarded(client, [&] {
        client->error.clear();
        auto r = client->sandbox->link(package, *provider->sandbox);
        if (!r) {
            client->error = r.error();
            return QZ_ERROR;
        }
        return QZ_OK;
    });
}

const char* qz_str(qz_slot value, size_t* length) {
    const auto* s = static_cast<const quartz::vm::Str*>(value.p);
    if (s == nullptr) {
        if (length != nullptr) {
            *length = 0;
        }
        return "";
    }
    if (length != nullptr) {
        *length = s->len;
    }
    return s->data;
}

const qz_slot* qz_call_args(const qz_call* call) {
    static_assert(alignof(qz_slot) == alignof(quartz::vm::Slot));
    return std::launder(static_cast<const qz_slot*>(static_cast<const void*>(call->call.args)));
}

void qz_call_set_result(qz_call* call, qz_slot value) { call->call.result = toVm(value); }

qz_slot* qz_call_result_slots(qz_call* call) { return resultSlots(call->call); }

size_t qz_call_arg_count(const qz_call* call) { return call->argSlots; }

qz_slot qz_call_str(qz_call* call, const char* utf8, size_t length) {
    qz_slot out{};
    out.p = call->call.string(std::string_view(utf8, length));
    return out;
}

bool qz_call_charge(qz_call* call, int64_t units) { return call->call.charge(units); }

void qz_call_fail(qz_call* call, const char* message) { call->call.error = message != nullptr ? message : ""; }

qz_sandbox* qz_call_sandbox(qz_call* call) { return call->owner; }

uint32_t qz_program_function_arity(const qz_program* program, uint32_t function) {
    return static_cast<uint32_t>(callerParams(*program->program, function).size());
}

qz_kind qz_program_function_param(const qz_program* program, uint32_t function, uint32_t index, uint32_t* slots) {
    std::vector<quartz::ir::TypeId> params = callerParams(*program->program, function);
    return index < params.size() ? describe(*program->program, params[index], slots) : QZ_KIND_OTHER;
}

qz_kind qz_program_function_result(const qz_program* program, uint32_t function, uint32_t* slots) {
    const quartz::vm::Program& p = *program->program;
    return function < p.functions.size() ? describe(p, p.functions[function].result, slots) : QZ_KIND_OTHER;
}

uint32_t qz_program_import_count(const qz_program* program) {
    return static_cast<uint32_t>(program->program->module->imports.size());
}

const char* qz_program_import_name(const qz_program* program, uint32_t import) {
    const auto& imports = program->program->module->imports;
    return import < imports.size() ? imports[import].name.c_str() : "";
}

uint32_t qz_program_import_arity(const qz_program* program, uint32_t import) {
    const auto& imports = program->program->module->imports;
    return import < imports.size() ? static_cast<uint32_t>(imports[import].params.size()) : 0;
}

qz_kind qz_program_import_param(const qz_program* program, uint32_t import, uint32_t index, uint32_t* slots) {
    const auto& imports = program->program->module->imports;
    if (import >= imports.size() || index >= imports[import].params.size()) {
        return QZ_KIND_OTHER;
    }
    return describe(*program->program, imports[import].params[index], slots);
}

qz_kind qz_program_import_result(const qz_program* program, uint32_t import, uint32_t* slots) {
    const auto& imports = program->program->module->imports;
    return import < imports.size() ? describe(*program->program, imports[import].result, slots) : QZ_KIND_OTHER;
}

char* qz_program_interface(const qz_program* program, const char* manifest, char** error) {
    try {
        auto m = quartz::parseManifest(manifest != nullptr ? manifest : program->program->module->manifest);
        if (!m) {
            report(error, m.error());
            return nullptr;
        }
        auto text = quartz::interfaceOf(*program->program, *m);
        if (!text) {
            report(error, text.error());
            return nullptr;
        }
        return copyOut(*text);
    } catch (...) {
        report(error, "out of memory");
        return nullptr;
    }
}

uint32_t qz_sandbox_wait_type(const qz_sandbox* sandbox, uint32_t task) { return sandbox->sandbox->waitType(task); }

qz_slot qz_sandbox_list(qz_sandbox* sandbox, uint32_t list_type) {
    qz_slot out{};
    out.p = sandbox->sandbox->hostList(list_type);
    return out;
}

bool qz_list_push(qz_sandbox* sandbox, qz_slot list, const qz_slot* element, size_t slots) {
    std::vector<quartz::vm::Slot> e = ::slots(element, slots);
    return sandbox->sandbox->hostListPush(static_cast<quartz::vm::List*>(list.p), e);
}

size_t qz_list_length(qz_slot list) {
    return list.p != nullptr ? static_cast<const quartz::vm::List*>(list.p)->len : 0;
}

bool qz_list_get(qz_slot list, size_t index, qz_slot* out, size_t slots) {
    const auto* l = static_cast<const quartz::vm::List*>(list.p);
    if (l == nullptr || index >= l->len || slots != l->elemSlots) {
        return false;
    }
    if (l->packed) {
        out[0].u = l->bytes()[index];
        return true;
    }
    for (std::size_t k = 0; k < slots; ++k) {
        out[k] = toC(l->data[index * l->elemSlots + k]);
    }
    return true;
}

qz_trap qz_sandbox_trap_code(const qz_sandbox* sandbox) { return sandbox->trap; }

qz_status qz_sandbox_reload(qz_sandbox* sandbox, const qz_program* program) {
    return guarded(sandbox, [&] {
        sandbox->error.clear();
        sandbox->trap = QZ_TRAP_NONE;
        auto next = sandbox->sandbox->reload(program->program);
        if (!next) {
            return trapped(sandbox, next.error());
        }
        sandbox->sandbox = std::move(*next);
        return QZ_OK;
    });
}

char* qz_sandbox_export_handle(qz_sandbox* sandbox, uint64_t handle) {
    try {
        auto r = sandbox->sandbox->exportHandle(handle);
        if (!r) {
            sandbox->error = r.error().describe();
            return nullptr;
        }
        return copyOut(*r);
    } catch (...) {
        sandbox->error = "out of memory";
        return nullptr;
    }
}

qz_status qz_sandbox_import_handle(qz_sandbox* sandbox, uint64_t handle, const char* json, size_t length) {
    return guarded(sandbox, [&] {
        sandbox->error.clear();
        auto r = sandbox->sandbox->importHandle(handle, std::string_view(json, length));
        return r ? QZ_OK : trapped(sandbox, r.error());
    });
}

void qz_sandbox_drop_handle(qz_sandbox* sandbox, uint64_t handle) { sandbox->sandbox->dropHandle(handle); }

size_t qz_sandbox_live_tasks(const qz_sandbox* sandbox) { return sandbox->sandbox->liveTasks(); }

int64_t qz_sandbox_fuel_per_call(const qz_sandbox* sandbox) { return sandbox->sandbox->fuelPerCall(); }

} // extern "C"
