#include "vm/Bytecode.hpp"

namespace quartz::vm {

const char* opcodeName(Opc op) {
    static const char* const kNames[] = {
#define QUARTZ_OPC_NAME(name) #name,
        QUARTZ_OPCODES(QUARTZ_OPC_NAME)
#undef QUARTZ_OPC_NAME
    };
    auto i = static_cast<std::size_t>(op);
    return i < static_cast<std::size_t>(Opc::Count) ? kNames[i] : "?";
}

std::uint32_t Program::findFunction(std::string_view name) const {
    for (std::uint32_t i = 0; i < functions.size(); ++i) {
        if (functions[i].name == name) {
            return i;
        }
    }
    return ~0U;
}

namespace {

bool qualifiedIs(std::string_view package, std::string_view name, std::string_view wanted) {
    return !package.empty() && wanted.size() == package.size() + 1 + name.size() && wanted.starts_with(package) &&
           wanted[package.size()] == '.' && wanted.ends_with(name);
}

} // namespace

std::expected<std::uint32_t, LookupError> Program::lookupFunction(std::string_view name) const {
    std::uint32_t found = ~0U;
    std::uint32_t matches = 0;
    for (bool exportedOnly : {true, false}) {
        for (std::uint32_t i = 0; i < module->functions.size(); ++i) {
            const ir::Function& f = module->functions[i];
            if ((!exportedOnly || f.exported) && (f.name == name || qualifiedIs(f.package, f.name, name))) {
                found = i;
                ++matches;
            }
        }
        if (matches > 0) {
            break;
        }
    }
    if (matches > 1) {
        return std::unexpected(LookupError::Ambiguous);
    }
    return matches == 1 ? std::expected<std::uint32_t, LookupError>(found) : std::unexpected(LookupError::NotFound);
}

std::expected<std::uint32_t, LookupError> Program::lookupImport(std::string_view name) const {
    const auto& imports = module->imports;
    for (std::uint32_t i = 0; i < imports.size(); ++i) {
        if (imports[i].name == name) {
            return i;
        }
    }
    std::uint32_t found = ~0U;
    std::uint32_t matches = 0;
    for (std::uint32_t i = 0; i < imports.size(); ++i) {
        const ir::HostImport& imp = imports[i];
        if (!imp.package.empty() && std::string_view(imp.name).substr(imp.package.size() + 1) == name) {
            found = i;
            ++matches;
        }
    }
    if (matches > 1) {
        return std::unexpected(LookupError::Ambiguous);
    }
    return matches == 1 ? std::expected<std::uint32_t, LookupError>(found) : std::unexpected(LookupError::NotFound);
}

} // namespace quartz::vm
