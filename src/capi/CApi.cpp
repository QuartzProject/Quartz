#include "quartz/Quartz.hpp"
#include "quartz/quartz.h"

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
};

struct qz_sandbox {
    struct Binding {
        qz_host_fn fn;
        void* user;
        qz_sandbox* owner;
    };

    std::unique_ptr<quartz::vm::Sandbox> sandbox;
    std::vector<std::unique_ptr<Binding>> bindings;
    std::string error;
};

namespace {

static_assert(sizeof(qz_slot) == sizeof(quartz::vm::Slot));

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

bool trampoline(quartz::vm::HostCall& call, void* user) {
    auto* binding = static_cast<qz_sandbox::Binding*>(user);
    qz_call c{call, binding->owner};
    return binding->fn(&c, binding->user);
}

// A C caller cannot catch C++ exceptions, so every entry point turns them into an error.
template <typename F>
qz_status guarded(qz_sandbox* sb, F&& body) {
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
    if (sandbox == nullptr || import_name == nullptr || fn == nullptr) {
        return false;
    }
    try {
        auto binding = std::make_unique<qz_sandbox::Binding>(qz_sandbox::Binding{fn, user, sandbox});
        if (!sandbox->sandbox->bind(import_name, trampoline, binding.get())) {
            return false;
        }
        sandbox->bindings.push_back(std::move(binding));
        return true;
    } catch (...) {
        return false;
    }
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

qz_slot* qz_call_result_slots(qz_call* call) {
    quartz::vm::Slot* where = call->call.out != nullptr ? call->call.out : &call->call.result;
    return std::launder(static_cast<qz_slot*>(static_cast<void*>(where)));
}

qz_slot qz_call_str(qz_call* call, const char* utf8, size_t length) {
    qz_slot out{};
    out.p = call->call.string(std::string_view(utf8, length));
    return out;
}

bool qz_call_charge(qz_call* call, int64_t units) { return call->call.charge(units); }

void qz_call_fail(qz_call* call, const char* message) { call->call.error = message != nullptr ? message : ""; }

qz_sandbox* qz_call_sandbox(qz_call* call) { return call->owner; }

} // extern "C"
