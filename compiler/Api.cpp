#include "Codegen.hpp"
#include "vm/NativeAbi.hpp"

#include <chrono>
#include <cstdio>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/StringSaver.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Target/TargetMachine.h>
#include <mutex>

#define QC_API extern "C" __attribute__((visibility("default")))

struct quartz::vm::NativeJit {
    std::unique_ptr<llvm::orc::LLJIT> jit;
    std::unique_ptr<llvm::TargetMachine> tm;
    int opt = 1;
    std::uint64_t serial = 0;
};

namespace {

using quartz::vm::NativeJit;

void setError(char* err, std::size_t len, const std::string& msg) {
    if (err != nullptr && len > 0) {
        std::snprintf(err, len, "%s", msg.c_str());
    }
}

std::string take(llvm::Error e) { return llvm::toString(std::move(e)); }

double msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

llvm::OptimizationLevel level(int opt) {
    switch (opt) {
    case 2:
        return llvm::OptimizationLevel::O2;
    case 3:
        return llvm::OptimizationLevel::O3;
    default:
        return llvm::OptimizationLevel::O1;
    }
}

} // namespace

QC_API std::uint32_t qc_abi() { return quartz::vm::kNativeAbi; }

QC_API NativeJit* qc_create(const quartz::vm::NativeSymbol* runtime, std::uint32_t count, int optLevel, char* err,
                            std::size_t errLen) {
    static std::once_flag once;
    std::call_once(once, [] {
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
        // Developer knob for measuring LLVM options; the library has its own private copy of LLVM.
        if (const char* args = std::getenv("QUARTZ_LLVM_ARGS"); args != nullptr && *args != '\0') {
            llvm::SmallVector<const char*, 8> argv{"quartz"};
            llvm::BumpPtrAllocator alloc;
            llvm::StringSaver saver(alloc);
            llvm::cl::TokenizeGNUCommandLine(args, saver, argv);
            llvm::cl::ParseCommandLineOptions(static_cast<int>(argv.size()), argv.data());
        }
    });
    auto jtmb = llvm::orc::JITTargetMachineBuilder::detectHost();
    if (!jtmb) {
        setError(err, errLen, take(jtmb.takeError()));
        return nullptr;
    }
    jtmb->setCodeGenOptLevel(optLevel == 0 ? llvm::CodeGenOptLevel::None : llvm::CodeGenOptLevel::Default);
    // parity: never fuse a*b+c into an FMA, the interpreter rounds twice (D24)
    jtmb->getOptions().AllowFPOpFusion = llvm::FPOpFusion::Strict;
    auto tm = jtmb->createTargetMachine();
    if (!tm) {
        setError(err, errLen, take(tm.takeError()));
        return nullptr;
    }
    auto jit = llvm::orc::LLJITBuilder().setJITTargetMachineBuilder(*jtmb).create();
    if (!jit) {
        setError(err, errLen, take(jit.takeError()));
        return nullptr;
    }
    llvm::orc::JITDylib& jd = (*jit)->getMainJITDylib();
    llvm::orc::MangleAndInterner mangle((*jit)->getExecutionSession(), (*jit)->getDataLayout());
    llvm::orc::SymbolMap symbols;
    for (std::uint32_t i = 0; i < count; ++i) {
        symbols[mangle(runtime[i].name)] = {llvm::orc::ExecutorAddr::fromPtr(runtime[i].address),
                                            llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable};
    }
    if (auto e = jd.define(llvm::orc::absoluteSymbols(std::move(symbols)))) {
        setError(err, errLen, take(std::move(e)));
        return nullptr;
    }
    // memset, memcpy and fmod, which LLVM may emit on its own.
    auto process =
        llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess((*jit)->getDataLayout().getGlobalPrefix());
    if (!process) {
        setError(err, errLen, take(process.takeError()));
        return nullptr;
    }
    jd.addGenerator(std::move(*process));
    auto out = std::make_unique<NativeJit>();
    out->jit = std::move(*jit);
    out->tm = std::move(*tm);
    out->opt = optLevel;
    return out.release();
}

QC_API int qc_compile(NativeJit* jit, const void* program, std::uint32_t function, quartz::vm::NativeEntry* entry,
                      quartz::vm::NativeCompileStats* stats, char* err, std::size_t errLen) {
    auto t0 = std::chrono::steady_clock::now();
    const auto& prog = *static_cast<const quartz::vm::Program*>(program);
    if (function >= prog.functions.size()) {
        setError(err, errLen, "no such function");
        return 1;
    }
    auto ctx = std::make_unique<llvm::LLVMContext>();
    auto mod = std::make_unique<llvm::Module>("quartz", *ctx);
    mod->setDataLayout(jit->jit->getDataLayout());
    mod->setTargetTriple(jit->jit->getTargetTriple());
    std::string name = "qz_entry_" + std::to_string(function) + "_" + std::to_string(jit->serial++);
    quartz::compiler::UnitCodegen unit(*ctx, *mod, prog);
    std::string e = unit.build(function, name);
    if (!e.empty()) {
        setError(err, errLen, e);
        return 1;
    }
    double lowerMs = msSince(t0);
    auto t1 = std::chrono::steady_clock::now();
    if (jit->opt > 0) {
        llvm::LoopAnalysisManager lam;
        llvm::FunctionAnalysisManager fam;
        llvm::CGSCCAnalysisManager cgam;
        llvm::ModuleAnalysisManager mam;
        llvm::PassBuilder pb(jit->tm.get());
        pb.registerModuleAnalyses(mam);
        pb.registerCGSCCAnalyses(cgam);
        pb.registerFunctionAnalyses(fam);
        pb.registerLoopAnalyses(lam);
        pb.crossRegisterProxies(lam, fam, cgam, mam);
        pb.buildPerModuleDefaultPipeline(level(jit->opt)).run(*mod, mam);
    }
    double optMs = msSince(t1);
    if (std::getenv("QUARTZ_DUMP_LLVM") != nullptr) {
        mod->print(llvm::errs(), nullptr);
    }
    if (auto ae = jit->jit->addIRModule(llvm::orc::ThreadSafeModule(std::move(mod), std::move(ctx)))) {
        setError(err, errLen, take(std::move(ae)));
        return 1;
    }
    auto sym = jit->jit->lookup(name);
    if (!sym) {
        setError(err, errLen, take(sym.takeError()));
        return 1;
    }
    *entry = sym->toPtr<quartz::vm::NativeEntry>();
    if (stats != nullptr) {
        stats->totalMs = msSince(t0);
        stats->lowerMs = lowerMs;
        stats->optimizeMs = optMs;
        stats->functions = unit.loweredFunctions();
        stats->irInstructions = unit.loweredInstructions();
    }
    return 0;
}

QC_API void qc_destroy(NativeJit* jit) { std::unique_ptr<NativeJit> owned(jit); }
