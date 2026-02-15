#include "ir/ByteIo.hpp"
#include "ir/Serialize.hpp"

#include <bit>

namespace quartz::ir {

using bin::Section;
using bin::TypeTag;
using bin::Writer;

namespace {

void writeType(Writer& w, const TypeTable& types, TypeId id) {
    const TypeInfo& t = types.info(id);
    auto tag = [&](TypeTag g) { w.u8(static_cast<std::uint8_t>(g)); };
    auto list = [&](const std::vector<TypeId>& ids) {
        w.u32(static_cast<std::uint32_t>(ids.size()));
        for (TypeId e : ids) {
            w.u32(e);
        }
    };
    auto generics = [&]() {
        w.u32(t.version);
        w.u8(static_cast<std::uint8_t>((t.persisted ? 1 : 0) | (t.reprC ? 2 : 0)));
        w.u32(t.persistPer);
        list(t.typeParams);
        w.u32(t.genericBase);
        list(t.typeArgs);
    };
    switch (t.kind) {
    case TypeKind::List:
        tag(TypeTag::List);
        w.u32(t.elem);
        return;
    case TypeKind::Map:
        tag(TypeTag::Map);
        w.u32(t.key);
        w.u32(t.elem);
        return;
    case TypeKind::Nullable:
        tag(TypeTag::Nullable);
        w.u32(t.elem);
        return;
    case TypeKind::Tuple:
        tag(TypeTag::Tuple);
        list(t.elems);
        return;
    case TypeKind::Fn:
        tag(TypeTag::Fn);
        list(t.elems);
        w.u32(t.result);
        return;
    case TypeKind::Struct:
    case TypeKind::State:
        tag(t.kind == TypeKind::Struct ? TypeTag::Struct : TypeTag::State);
        w.str(t.name);
        w.u32(static_cast<std::uint32_t>(t.fields.size()));
        for (const Field& f : t.fields) {
            w.str(f.name);
            w.u32(f.type);
            w.str(f.renamedFrom);
        }
        generics();
        return;
    case TypeKind::Enum:
        if (types.isResult(id)) {
            tag(TypeTag::Result);
            w.u32(t.variants[0].payload.empty() ? kError : t.variants[0].payload[0]);
            w.u32(t.variants[1].payload.empty() ? kError : t.variants[1].payload[0]);
            return;
        }
        tag(TypeTag::Enum);
        w.str(t.name);
        w.u32(static_cast<std::uint32_t>(t.variants.size()));
        for (const VariantInfo& v : t.variants) {
            w.str(v.name);
            list(v.payload);
        }
        generics();
        return;
    case TypeKind::Handle:
        tag(TypeTag::Handle);
        w.str(t.name);
        generics();
        return;
    case TypeKind::Param:
        tag(TypeTag::Param);
        w.str(t.name);
        return;
    default:
        // Builtin kinds never appear past kFirstUserType; tag 0 makes the reader reject the module.
        w.u8(0);
        return;
    }
}

void writeInst(Writer& w, const Inst& in) {
    std::uint8_t mask = 0;
    mask |= in.dst != kNoReg ? bin::kHasDst : 0;
    mask |= in.a != kNoReg ? bin::kHasA : 0;
    mask |= in.b != kNoReg ? bin::kHasB : 0;
    mask |= in.imm != 0 ? bin::kHasImm : 0;
    mask |= in.argCount != 0 ? bin::kHasArgs : 0;
    w.u8(static_cast<std::uint8_t>(in.op));
    w.u8(mask);
    if (mask & bin::kHasDst) {
        w.u32(in.dst);
    }
    if (mask & bin::kHasA) {
        w.u32(in.a);
    }
    if (mask & bin::kHasB) {
        w.u32(in.b);
    }
    if (mask & bin::kHasImm) {
        w.u32(in.imm);
    }
    if (mask & bin::kHasArgs) {
        w.u32(in.argBegin);
        w.u32(in.argCount);
    }
}

void writeFunction(Writer& w, const Function& f) {
    w.str(f.name);
    std::uint8_t flags = (f.exported ? bin::kFnExported : 0) | (f.isSuspend ? bin::kFnSuspend : 0) |
                         (f.inlining == Inlining::Always  ? bin::kFnInline
                          : f.inlining == Inlining::Never ? bin::kFnNoInline
                                                          : 0);
    w.u8(flags);
    w.u32(f.paramCount);
    w.u32(f.result);
    w.u32(static_cast<std::uint32_t>(f.regs.size()));
    for (TypeId r : f.regs) {
        w.u32(r);
    }
    w.u32(static_cast<std::uint32_t>(f.args.size()));
    for (Reg r : f.args) {
        w.u32(r);
    }
    w.u32(static_cast<std::uint32_t>(f.code.size()));
    for (const Inst& in : f.code) {
        writeInst(w, in);
    }
}

void writeAnnotation(Writer& w, const Annotation& a) {
    w.str(a.name);
    w.u32(a.function);
    w.opt(a.filter);
    w.u32(static_cast<std::uint32_t>(a.args.size()));
    for (const AnnotationArg& arg : a.args) {
        w.str(arg.key);
        w.u8(static_cast<std::uint8_t>(arg.kind));
        switch (arg.kind) {
        case AnnotationArg::Kind::Int:
            w.i64(arg.intValue);
            break;
        case AnnotationArg::Kind::Bool:
            w.u8(arg.intValue != 0 ? 1 : 0);
            break;
        case AnnotationArg::Kind::Float:
            w.fixed64(std::bit_cast<std::uint64_t>(arg.floatValue));
            break;
        case AnnotationArg::Kind::Str:
        case AnnotationArg::Kind::Name:
            w.str(arg.text);
            break;
        }
    }
}

void section(Writer& out, Section id, const Writer& body) {
    out.u8(static_cast<std::uint8_t>(id));
    out.u64(body.out.size());
    out.bytes(body.out);
}

} // namespace

std::vector<std::uint8_t> encodePayload(const Module& m, WriteOptions options) {
    Writer out;
    Writer w;
    w.u32(static_cast<std::uint32_t>(m.types.size() - kFirstUserType));
    for (TypeId id = kFirstUserType; id < m.types.size(); ++id) {
        writeType(w, m.types, id);
    }
    section(out, Section::Types, w);

    w.out.clear();
    w.u32(static_cast<std::uint32_t>(m.constants.size()));
    for (const Constant& c : m.constants) {
        w.u8(static_cast<std::uint8_t>(c.kind));
        w.u32(c.type);
        switch (c.kind) {
        case ConstKind::Int:
            w.i64(static_cast<std::int64_t>(c.bits));
            break;
        case ConstKind::Float:
            w.fixed64(c.bits);
            break;
        case ConstKind::Str:
            w.str(c.text);
            break;
        }
    }
    section(out, Section::Constants, w);

    w.out.clear();
    w.u32(static_cast<std::uint32_t>(m.imports.size()));
    for (const HostImport& imp : m.imports) {
        w.str(imp.name);
        w.u32(static_cast<std::uint32_t>(imp.params.size()));
        for (TypeId p : imp.params) {
            w.u32(p);
        }
        w.u32(imp.result);
    }
    section(out, Section::Imports, w);

    w.out.clear();
    w.u32(static_cast<std::uint32_t>(m.states.size()));
    for (std::size_t i = 0; i < m.states.size(); ++i) {
        w.u32(m.states[i]);
        w.opt(i < m.stateInit.size() ? m.stateInit[i] : ~0U);
    }
    section(out, Section::States, w);

    w.out.clear();
    w.u32(static_cast<std::uint32_t>(m.functions.size()));
    for (const Function& f : m.functions) {
        writeFunction(w, f);
    }
    section(out, Section::Functions, w);

    w.out.clear();
    w.u32(static_cast<std::uint32_t>(m.annotations.size()));
    for (const Annotation& a : m.annotations) {
        writeAnnotation(w, a);
    }
    section(out, Section::Annotations, w);

    if (options.lines) {
        w.out.clear();
        for (const Function& f : m.functions) {
            std::uint32_t prev = 0;
            for (const Inst& in : f.code) {
                w.i64(static_cast<std::int64_t>(in.line) - static_cast<std::int64_t>(prev));
                prev = in.line;
            }
        }
        section(out, Section::Lines, w);
    }
    if (!m.manifest.empty()) {
        w.out.clear();
        w.str(m.manifest);
        section(out, Section::Manifest, w);
    }
    return std::move(out.out);
}

} // namespace quartz::ir
