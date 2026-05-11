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

} // namespace quartz::vm
