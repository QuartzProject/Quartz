#include "ir/Prune.hpp"

#include <vector>

namespace quartz::ir {

namespace {

constexpr std::uint32_t kGone = ~0U;

bool isLibrary(const Function& f, std::string_view prefix) { return f.file.starts_with(prefix); }

} // namespace

void pruneLibrary(Module& m, std::string_view libraryPrefix,
                  const std::function<bool(std::string_view)>& isLibraryImport) {
    std::vector<bool> live(m.functions.size(), false);
    std::vector<std::uint32_t> work;
    auto reach = [&](std::uint32_t f) {
        if (f < live.size() && !live[f]) {
            live[f] = true;
            work.push_back(f);
        }
    };
    for (std::uint32_t f = 0; f < m.functions.size(); ++f) {
        if (!isLibrary(m.functions[f], libraryPrefix)) {
            reach(f);
        }
    }
    for (const Annotation& a : m.annotations) {
        reach(a.function);
        reach(a.filter);
    }
    for (std::uint32_t f : m.stateInit) {
        reach(f);
    }
    std::vector<bool> usedImport(m.imports.size(), false);
    while (!work.empty()) {
        std::uint32_t f = work.back();
        work.pop_back();
        for (const Inst& in : m.functions[f].code) {
            if (in.op == Op::Call || in.op == Op::Closure) {
                reach(in.imm);
            } else if (in.op == Op::CallHost && in.imm < usedImport.size()) {
                usedImport[in.imm] = true;
            }
        }
    }

    std::vector<std::uint32_t> fnMap(m.functions.size(), kGone);
    std::vector<Function> keptFns;
    for (std::uint32_t f = 0; f < m.functions.size(); ++f) {
        if (live[f]) {
            fnMap[f] = static_cast<std::uint32_t>(keptFns.size());
            keptFns.push_back(std::move(m.functions[f]));
            if (isLibrary(keptFns.back(), libraryPrefix)) {
                keptFns.back().exported = false;
            }
        }
    }
    std::vector<std::uint32_t> impMap(m.imports.size(), kGone);
    std::vector<HostImport> keptImports;
    for (std::uint32_t i = 0; i < m.imports.size(); ++i) {
        if (usedImport[i] || !isLibraryImport(m.imports[i].name)) {
            impMap[i] = static_cast<std::uint32_t>(keptImports.size());
            keptImports.push_back(std::move(m.imports[i]));
        }
    }
    for (Function& f : keptFns) {
        for (Inst& in : f.code) {
            if ((in.op == Op::Call || in.op == Op::Closure) && in.imm < fnMap.size()) {
                in.imm = fnMap[in.imm];
            } else if (in.op == Op::CallHost && in.imm < impMap.size()) {
                in.imm = impMap[in.imm];
            }
        }
    }
    auto remap = [&](std::uint32_t& f) {
        if (f < fnMap.size()) {
            f = fnMap[f];
        }
    };
    for (Annotation& a : m.annotations) {
        remap(a.function);
        remap(a.filter);
    }
    for (std::uint32_t& f : m.stateInit) {
        remap(f);
    }
    m.functions = std::move(keptFns);
    m.imports = std::move(keptImports);
}

} // namespace quartz::ir
