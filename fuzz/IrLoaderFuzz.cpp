#include "ir/Serialize.hpp"
#include "ir/Validator.hpp"
#include "quartz/Package.hpp"
#include "vm/Bytecode.hpp"
#include "vm/Sandbox.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace {

using namespace quartz;

bool zeroHost(vm::HostCall& call, void*) {
    call.result = {};
    return true;
}

bool runnable(const ir::Module& m) {
    for (const ir::HostImport& imp : m.imports) {
        if (m.types.hasRefs(imp.result)) {
            return false;
        }
    }
    return true;
}

void exercise(std::shared_ptr<const vm::Program> program) {
    const ir::Module& m = *program->module;
    Manifest manifest;
    manifest.name = "org.fuzz.module";
    for (const ir::Function& f : m.functions) {
        if (f.exported && !f.isSuspend) {
            manifest.exports.push_back(f.name);
        }
    }
    (void)interfaceOf(*program, manifest);
    if (!runnable(m)) {
        return;
    }
    vm::Sandbox sb(program, {.memoryBytes = 1u << 20, .fuelPerCall = 20'000, .stackSlots = 1u << 14, .maxDepth = 64});
    for (const ir::HostImport& imp : m.imports) {
        sb.bind(imp.name, zeroHost);
    }
    if (!sb.start()) {
        return;
    }
    std::vector<vm::Slot> args;
    for (std::uint32_t f = 0; f < program->functions.size(); ++f) {
        const vm::CompiledFunction& fn = program->functions[f];
        if (!fn.exported) {
            continue;
        }
        args.clear();
        bool ok = true;
        for (ir::TypeId p : fn.params) {
            if (m.types.kind(p) == ir::TypeKind::State) {
                continue;
            }
            ok = ok && m.types.isScalar(p);
            args.push_back({});
        }
        if (ok) {
            (void)sb.call(f, args);
        }
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    std::span<const std::uint8_t> in(data, size);
    auto mod = ir::isBinaryModule(in) ? ir::deserialize(in) : ir::decodePayload(in);
    if (!mod || !ir::validate(**mod)) {
        return 0;
    }
    exercise(vm::compile(std::move(*mod)));
    return 0;
}
