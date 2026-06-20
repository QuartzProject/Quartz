#include "vm/ReprC.hpp"

#include <cstring>
#include <vector>

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

// Struct fields, or the elements of a fixed-size array: laid out in order, like `int32_t a[4]` in C.
std::vector<ir::TypeId> members(const ir::TypeTable& types, ir::TypeId t) {
    const ir::TypeInfo& info = types.info(t);
    if (info.kind == ir::TypeKind::Tuple) {
        return info.elems;
    }
    std::vector<ir::TypeId> out;
    for (const ir::Field& f : info.fields) {
        out.push_back(f.type);
    }
    return out;
}

} // namespace

CLayout cLayout(const ir::TypeTable& types, ir::TypeId t) {
    if (types.isScalar(t)) {
        std::size_t s = scalarSize(t);
        return {s, s};
    }
    CLayout out;
    for (ir::TypeId m : members(types, t)) {
        CLayout fl = cLayout(types, m);
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
    std::vector<ir::TypeId> ms = members(types, t);
    for (std::uint32_t i = 0; i < ms.size(); ++i) {
        CLayout fl = cLayout(types, ms[i]);
        off = alignUp(off, fl.align);
        toC(types, lay, ms[i], src + lay.fieldOffset(t, i), dst + off);
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
    std::vector<ir::TypeId> ms = members(types, t);
    for (std::uint32_t i = 0; i < ms.size(); ++i) {
        CLayout fl = cLayout(types, ms[i]);
        off = alignUp(off, fl.align);
        fromC(types, lay, ms[i], src + off, dst + lay.fieldOffset(t, i));
        off += fl.size;
    }
}

} // namespace quartz::vm
