#include "ir/ByteIo.hpp"
#include "ir/Serialize.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <format>

namespace quartz::ir {

using bin::Reader;
using bin::Section;
using bin::TypeTag;

namespace {

// The encoding stores these enums by value: a change to any of them needs a new kFormatVersion.
static_assert(static_cast<int>(Op::CallValue) == 49);
static_assert(static_cast<int>(Intrinsic::Count) == 75);
static_assert(static_cast<int>(TrapCode::HostError) == 9);
static_assert(static_cast<int>(ConstKind::Str) == 2);
static_assert(static_cast<int>(AnnotationArg::Kind::Name) == 4);
static_assert(kFirstUserType == 18);

constexpr std::uint32_t kOpCount = static_cast<std::uint32_t>(Op::CallValue) + 1;
constexpr std::uint32_t kMaxElements = 1u << 24;

class Decoder {
public:
    Decoder(Module& m, const ReadLimits& limits) : m_(m), limits_(limits) {}

    void types(Reader& r) {
        std::uint32_t n = r.count(limits_.maxTypes);
        depth_.assign(kFirstUserType, 0);
        for (std::uint32_t i = 0; i < n && !r.failed(); ++i) {
            readType(r, static_cast<TypeId>(kFirstUserType + i));
        }
    }

    void constants(Reader& r) {
        std::uint32_t n = r.count(kMaxElements, 3);
        m_.constants.reserve(n);
        for (std::uint32_t i = 0; i < n && !r.failed(); ++i) {
            Constant c;
            std::uint8_t kind = r.u8();
            c.type = r.u32();
            switch (kind) {
            case 0:
                c.kind = ConstKind::Int;
                c.bits = static_cast<std::uint64_t>(r.i64());
                break;
            case 1:
                c.kind = ConstKind::Float;
                c.bits = r.fixed64();
                break;
            case 2:
                c.kind = ConstKind::Str;
                c.text = r.str();
                break;
            default:
                r.fail("unknown constant kind");
                break;
            }
            m_.constants.push_back(std::move(c));
        }
    }

    void imports(Reader& r) {
        std::uint32_t n = r.count(kMaxElements, 3);
        m_.imports.reserve(n);
        for (std::uint32_t i = 0; i < n && !r.failed(); ++i) {
            HostImport imp;
            imp.name = r.str();
            imp.params = ids(r);
            imp.result = r.u32();
            m_.imports.push_back(std::move(imp));
        }
    }

    void states(Reader& r) {
        std::uint32_t n = r.count(kMaxElements, 2);
        m_.states.reserve(n);
        m_.stateInit.reserve(n);
        for (std::uint32_t i = 0; i < n && !r.failed(); ++i) {
            m_.states.push_back(r.u32());
            m_.stateInit.push_back(r.opt());
        }
    }

    void functions(Reader& r) {
        std::uint32_t n = r.count(limits_.maxFunctions, 7);
        m_.functions.reserve(n);
        for (std::uint32_t i = 0; i < n && !r.failed(); ++i) {
            m_.functions.emplace_back();
            readFunction(r, m_.functions.back());
        }
    }

    void annotations(Reader& r) {
        std::uint32_t n = r.count(kMaxElements, 4);
        m_.annotations.reserve(n);
        for (std::uint32_t i = 0; i < n && !r.failed(); ++i) {
            Annotation a;
            a.name = r.str();
            a.function = r.u32();
            a.filter = r.opt();
            std::uint32_t args = r.count(kMaxElements, 3);
            a.args.reserve(args);
            for (std::uint32_t k = 0; k < args && !r.failed(); ++k) {
                a.args.push_back(readArg(r));
            }
            m_.annotations.push_back(std::move(a));
        }
    }

    void manifest(Reader& r) {
        m_.manifest = r.str();
        if (m_.manifest.size() > 65536) {
            r.fail("manifest too large");
        }
    }

    void lines(Reader& r) {
        for (Function& f : m_.functions) {
            std::int64_t line = 0;
            for (Inst& in : f.code) {
                std::int64_t delta = r.i64();
                if (r.failed()) {
                    return;
                }
                if (delta < -line || delta > static_cast<std::int64_t>(0xFFFFFFFFu) - line) {
                    r.fail("line number out of range");
                    return;
                }
                line += delta;
                in.line = static_cast<std::uint32_t>(line);
            }
        }
    }

private:
    std::vector<TypeId> ids(Reader& r) {
        std::uint32_t n = r.count(kMaxElements);
        std::vector<TypeId> out;
        out.reserve(n);
        for (std::uint32_t i = 0; i < n && !r.failed(); ++i) {
            out.push_back(r.u32());
        }
        return out;
    }

    TypeId component(Reader& r, TypeId self, std::uint32_t& depth) {
        TypeId c = r.u32();
        if (c >= self) {
            r.fail("structural type refers to a later type");
            return kError;
        }
        depth = std::max(depth, depth_[c] + 1);
        return c;
    }

    // Version and generic data of a nominal type; instances only refer to earlier types, so hasParams terminates.
    void readGenerics(Reader& r, TypeId self, TypeInfo& info, std::uint32_t& depth) {
        depth = 0;
        info.version = r.u32();
        std::uint8_t flags = r.u8();
        info.persisted = (flags & 1) != 0;
        info.reprC = (flags & 2) != 0;
        if (flags > 3) {
            r.fail("unknown type flags");
        }
        info.persistPer = r.u32();
        if (info.persistPer >= self) {
            r.fail("per-handle state refers to a later type");
        }
        info.typeParams = ids(r);
        info.genericBase = r.u32();
        if (info.genericBase >= self) {
            r.fail("generic instance of a later type");
        }
        std::uint32_t n = r.count(kMaxElements);
        for (std::uint32_t i = 0; i < n && !r.failed(); ++i) {
            info.typeArgs.push_back(component(r, self, depth));
        }
    }

    void readType(Reader& r, TypeId self) {
        TypeTable& t = m_.types;
        std::uint8_t tag = r.u8();
        std::uint32_t depth = 1;
        TypeId got = kError;
        TypeInfo info;
        switch (static_cast<TypeTag>(tag)) {
        case TypeTag::List: {
            TypeId e = component(r, self, depth);
            got = r.failed() ? kError : t.list(e);
            break;
        }
        case TypeTag::Map: {
            TypeId k = component(r, self, depth);
            TypeId v = component(r, self, depth);
            got = r.failed() ? kError : t.map(k, v);
            break;
        }
        case TypeTag::Nullable: {
            TypeId e = component(r, self, depth);
            got = r.failed() ? kError : t.nullable(e);
            break;
        }
        case TypeTag::Tuple:
        case TypeTag::Fn: {
            std::uint32_t n = r.count(kMaxElements);
            std::vector<TypeId> elems;
            elems.reserve(n);
            for (std::uint32_t i = 0; i < n && !r.failed(); ++i) {
                elems.push_back(component(r, self, depth));
            }
            if (static_cast<TypeTag>(tag) == TypeTag::Tuple) {
                got = r.failed() ? kError : t.tuple(elems);
            } else {
                TypeId res = component(r, self, depth);
                got = r.failed() ? kError : t.fn(elems, res);
            }
            break;
        }
        case TypeTag::Result: {
            TypeId ok = component(r, self, depth);
            TypeId err = component(r, self, depth);
            got = r.failed() ? kError : t.result(ok, err);
            break;
        }
        case TypeTag::Struct:
        case TypeTag::State: {
            info.kind = static_cast<TypeTag>(tag) == TypeTag::Struct ? TypeKind::Struct : TypeKind::State;
            info.name = r.str();
            std::uint32_t n = r.count(kMaxElements, 3);
            info.fields.reserve(n);
            for (std::uint32_t i = 0; i < n && !r.failed(); ++i) {
                Field f;
                f.name = r.str();
                f.type = r.u32();
                f.renamedFrom = r.str();
                info.fields.push_back(std::move(f));
            }
            readGenerics(r, self, info, depth);
            got = r.failed() ? kError : t.addRaw(std::move(info));
            break;
        }
        case TypeTag::Enum: {
            info.kind = TypeKind::Enum;
            info.name = r.str();
            std::uint32_t n = r.count(kMaxElements, 2);
            info.variants.reserve(n);
            for (std::uint32_t i = 0; i < n && !r.failed(); ++i) {
                VariantInfo v;
                v.name = r.str();
                v.payload = ids(r);
                info.variants.push_back(std::move(v));
            }
            readGenerics(r, self, info, depth);
            got = r.failed() ? kError : t.addRaw(std::move(info));
            if (!r.failed() && t.isResult(got)) {
                r.fail("result type encoded as a plain enum");
            }
            break;
        }
        case TypeTag::Handle:
            info.kind = TypeKind::Handle;
            info.name = r.str();
            readGenerics(r, self, info, depth);
            got = r.failed() ? kError : t.addRaw(std::move(info));
            break;
        case TypeTag::Param:
            info.kind = TypeKind::Param;
            info.name = r.str();
            depth = 0;
            got = r.failed() ? kError : t.addRaw(std::move(info));
            break;
        default:
            r.fail("unknown type tag");
            return;
        }
        if (r.failed()) {
            return;
        }
        if (got != self) {
            r.fail("duplicate structural type");
            return;
        }
        if (depth > limits_.maxTypeDepth) {
            r.fail("type nested too deeply");
            return;
        }
        depth_.push_back(depth);
    }

    void readInst(Reader& r, Inst& in, std::uint32_t& blocks) {
        std::uint8_t op = r.u8();
        std::uint8_t mask = r.u8();
        if (op >= kOpCount || (mask & ~bin::kInstMask) != 0) {
            r.fail("unknown opcode or field mask");
            return;
        }
        in.op = static_cast<Op>(op);
        auto present = [&](std::uint8_t bit, std::uint32_t none) {
            if ((mask & bit) == 0) {
                return none;
            }
            std::uint32_t v = r.u32();
            if (v == none) {
                r.fail("non canonical instruction field");
            }
            return v;
        };
        in.dst = present(bin::kHasDst, kNoReg);
        in.a = present(bin::kHasA, kNoReg);
        in.b = present(bin::kHasB, kNoReg);
        in.imm = present(bin::kHasImm, 0);
        if (mask & bin::kHasArgs) {
            in.argBegin = r.u32();
            in.argCount = present(bin::kHasArgs, 0);
        }
        if (in.op == Op::Block || in.op == Op::Loop || in.op == Op::If) {
            if (++blocks > limits_.maxBlockDepth) {
                r.fail("blocks nested too deeply");
            }
        } else if (in.op == Op::End && blocks > 0) {
            --blocks;
        }
    }

    void readFunction(Reader& r, Function& f) {
        f.name = r.str();
        std::uint8_t flags = r.u8();
        if ((flags & ~bin::kFnMask) != 0) {
            r.fail("unknown function flags");
            return;
        }
        f.exported = (flags & bin::kFnExported) != 0;
        f.isSuspend = (flags & bin::kFnSuspend) != 0;
        if ((flags & bin::kFnInline) != 0 && (flags & bin::kFnNoInline) != 0) {
            r.fail("function both inline and noinline");
            return;
        }
        f.inlining = (flags & bin::kFnInline) != 0     ? Inlining::Always
                     : (flags & bin::kFnNoInline) != 0 ? Inlining::Never
                                                       : Inlining::Default;
        f.paramCount = r.u32();
        f.result = r.u32();
        std::uint32_t regs = r.count(limits_.maxRegisters);
        f.regs.reserve(regs);
        for (std::uint32_t i = 0; i < regs && !r.failed(); ++i) {
            f.regs.push_back(r.u32());
        }
        std::uint32_t args = r.count(kMaxElements);
        f.args.reserve(args);
        for (std::uint32_t i = 0; i < args && !r.failed(); ++i) {
            f.args.push_back(r.u32());
        }
        std::uint32_t code = r.count(limits_.maxCode, 2);
        f.code.resize(code, Inst{Op::Trap});
        std::uint32_t blocks = 0;
        for (std::uint32_t i = 0; i < code && !r.failed(); ++i) {
            readInst(r, f.code[i], blocks);
        }
    }

    AnnotationArg readArg(Reader& r) {
        AnnotationArg a;
        a.key = r.str();
        std::uint8_t kind = r.u8();
        switch (kind) {
        case 0:
            a.kind = AnnotationArg::Kind::Int;
            a.intValue = r.i64();
            break;
        case 1:
            a.kind = AnnotationArg::Kind::Float;
            a.floatValue = std::bit_cast<double>(r.fixed64());
            break;
        case 2: {
            a.kind = AnnotationArg::Kind::Bool;
            std::uint8_t b = r.u8();
            if (b > 1) {
                r.fail("bool out of range");
            }
            a.intValue = b;
            break;
        }
        case 3:
        case 4:
            a.kind = kind == 3 ? AnnotationArg::Kind::Str : AnnotationArg::Kind::Name;
            a.text = r.str();
            break;
        default:
            r.fail("unknown annotation argument kind");
            break;
        }
        return a;
    }

    Module& m_;
    const ReadLimits& limits_;
    std::vector<std::uint32_t> depth_;
};

} // namespace

std::expected<std::unique_ptr<Module>, std::string> decodePayload(std::span<const std::uint8_t> payload,
                                                                  const ReadLimits& limits) {
    if (payload.size() > limits.maxBytes) {
        return std::unexpected("module too large");
    }
    auto m = std::make_unique<Module>();
    Decoder d(*m, limits);
    Reader top(payload);
    std::uint8_t last = 0;
    while (!top.atEnd() && !top.failed()) {
        std::uint8_t id = top.u8();
        std::uint64_t len = top.u64();
        std::span<const std::uint8_t> body = top.take(static_cast<std::size_t>(std::min<std::uint64_t>(len, SIZE_MAX)));
        if (top.failed()) {
            break;
        }
        if (id <= last || id > static_cast<std::uint8_t>(Section::Manifest)) {
            top.fail("unknown or out of order section");
            break;
        }
        for (std::uint8_t skipped = last + 1; skipped < id; ++skipped) {
            if (skipped != static_cast<std::uint8_t>(Section::Lines)) {
                top.fail("missing section");
            }
        }
        last = id;
        Reader r(body);
        switch (static_cast<Section>(id)) {
        case Section::Types:
            d.types(r);
            break;
        case Section::Constants:
            d.constants(r);
            break;
        case Section::Imports:
            d.imports(r);
            break;
        case Section::States:
            d.states(r);
            break;
        case Section::Functions:
            d.functions(r);
            break;
        case Section::Annotations:
            d.annotations(r);
            break;
        case Section::Lines:
            d.lines(r);
            break;
        case Section::Manifest:
            d.manifest(r);
            break;
        }
        if (!r.failed() && !r.atEnd()) {
            r.fail("trailing bytes in section");
        }
        if (r.failed()) {
            return std::unexpected(std::format("section {}: {}", id, r.error()));
        }
    }
    if (top.failed()) {
        return std::unexpected(top.error());
    }
    if (last < static_cast<std::uint8_t>(Section::Annotations)) {
        return std::unexpected("missing section");
    }
    return m;
}

} // namespace quartz::ir
