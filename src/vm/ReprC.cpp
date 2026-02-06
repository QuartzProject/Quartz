#include "vm/ReprC.hpp"

#include <cstring>

namespace quartz::vm {

namespace {

std::size_t scalarSize(ir::TypeId t) {
    switch (t) {
    case ir::kBool:
    case ir::kI8:
    case ir::kU8: return 1;
    case ir::kI16:
    case ir::kU16: return 2;
    case ir::kI32:
    case ir::kU32:
    case ir::kF32:
    case ir::kChar: return 4;
    default: return 8;
    }
}

std::size_t alignUp(std::size_t v, std::size_t a) { return (v + a - 1) / a * a; }

} // namespace

CLayout cLayout(const ir::TypeTable& types, ir::TypeId t) {
    if (types.isScalar(t)) {
        std::size_t s = scalarSize(t);
        return {s, s};
    }
    CLayout out;
    for (const ir::Field& f : types.info(t).fields) {
        CLayout fl = cLayout(types, f.type);
        out.size = alignUp(out.size, fl.align) + fl.size;
        out.align = fl.align > out.align ? fl.align : out.align;
    }
    out.size = alignUp(out.size, out.align);
    return out;
}

void toC(const ir::TypeTable& types, const Layouts& lay, ir::TypeId t, const Slot* src, std::uint8_t* dst) {
    if (types.isScalar(t)) {
        if (t == ir::kF32) {
            std::memcpy(dst, &src->f32, 4);
        } else {
            std::memcpy(dst, &src->u, scalarSize(t)); // little endian: the low bytes hold the value
        }
        return;
    }
    std::size_t off = 0;
    const ir::TypeInfo& info = types.info(t);
    for (std::uint32_t i = 0; i < info.fields.size(); ++i) {
        CLayout fl = cLayout(types, info.fields[i].type);
        off = alignUp(off, fl.align);
        toC(types, lay, info.fields[i].type, src + lay.fieldOffset(t, i), dst + off);
        off += fl.size;
    }
}

void fromC(const ir::TypeTable& types, const Layouts& lay, ir::TypeId t, const std::uint8_t* src, Slot* dst) {
    if (types.isScalar(t)) {
        dst->u = 0;
        switch (t) {
        case ir::kF32: std::memcpy(&dst->f32, src, 4); break;
        case ir::kF64: std::memcpy(&dst->f, src, 8); break;
        case ir::kBool: dst->u = *src != 0 ? 1 : 0; break;
        case ir::kI8: dst->i = static_cast<std::int8_t>(*src); break;
        case ir::kI16: {
            std::int16_t v = 0;
            std::memcpy(&v, src, 2);
            dst->i = v;
            break;
        }
        case ir::kI32: {
            std::int32_t v = 0;
            std::memcpy(&v, src, 4);
            dst->i = v;
            break;
        }
        default: std::memcpy(&dst->u, src, scalarSize(t)); break;
        }
        return;
    }
    std::size_t off = 0;
    const ir::TypeInfo& info = types.info(t);
    for (std::uint32_t i = 0; i < info.fields.size(); ++i) {
        CLayout fl = cLayout(types, info.fields[i].type);
        off = alignUp(off, fl.align);
        fromC(types, lay, info.fields[i].type, src + off, dst + lay.fieldOffset(t, i));
        off += fl.size;
    }
}

} // namespace quartz::vm
