#include "std/Builder.hpp"

namespace quartz::stdlib {

bool parseToml(vm::HostCall& call, void* user) {
    Builder b(call.sandbox, *static_cast<const DataTypes*>(user));
    return b.err(call.out, "toml: not implemented yet");
}

bool parseYaml(vm::HostCall& call, void* user) {
    Builder b(call.sandbox, *static_cast<const DataTypes*>(user));
    return b.err(call.out, "yaml: not implemented yet");
}

} // namespace quartz::stdlib
