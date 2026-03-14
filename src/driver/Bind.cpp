#include "quartz/Bind.hpp"

namespace quartz::bind_detail {

std::string describe(const ir::TypeTable& types, std::span<const ir::TypeId> params, ir::TypeId result) {
    std::string out = "(";
    for (std::size_t i = 0; i < params.size(); ++i) {
        out += (i ? ", " : "") + types.name(params[i]);
    }
    return out + ") -> " + types.name(result);
}

} // namespace quartz::bind_detail
