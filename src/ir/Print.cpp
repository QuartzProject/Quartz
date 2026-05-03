#include "ir/Ir.hpp"

#include <bit>
#include <format>

namespace quartz::ir {

const char* trapName(TrapCode code) {
    switch (code) {
    case TrapCode::Unreachable: return "unreachable";
    case TrapCode::DivideByZero: return "division by zero";
    case TrapCode::IndexOutOfBounds: return "index out of bounds";
    case TrapCode::NullUnwrap: return "null value";
    case TrapCode::OutOfFuel: return "out of fuel";
    case TrapCode::OutOfMemory: return "out of memory";
    case TrapCode::StackOverflow: return "stack overflow";
    case TrapCode::InvalidHandle: return "invalid handle";
    case TrapCode::Panic: return "panic";
    case TrapCode::HostError: return "host error";
    }
    return "trap";
}

const char* intrinsicName(Intrinsic in) {
    static const char* const kNames[] = {
        "list.new",     "list.filled",  "list.len",    "list.get",   "list.set",    "list.push",   "list.pop",
        "list.clear",   "list.join",    "map.new",     "map.len",    "map.get",     "map.set",     "map.has",
        "map.remove",   "str.len",      "str.concat",  "str.slice",  "str.lower",   "str.upper",   "str.contains",
        "str.find",     "str.starts",   "str.ends",    "str.byteAt", "str.fromI64", "str.fromU64", "str.fromF64",
        "str.fromBool", "str.fromChar", "math.sqrt",   "math.floor", "math.ceil",   "math.absf",   "math.absi",
        "math.mini",    "math.maxi",    "math.minf",   "math.maxf",  "math.pow",    "math.sin",    "math.cos",
        "panic",
    };
    static const char* const kExtra[] = {
        "str.toInt",         "str.toFloat",       "str.replace",
        "str.split",         "str.trim",          "str.trimStart",
        "str.trimEnd",       "str.repeat",        "str.equalsIgnoreCase",
        "str.chars",         "char.fromInt",      "list.insert",
        "list.removeAt",     "list.contains",     "list.indexOf",
        "list.reverse",      "list.slice",        "list.insertAll",
        "list.sort",         "char.code",         "math.tan",
        "math.asin",         "math.acos",         "math.atan",
        "math.exp",          "math.log",          "math.log10",
        "math.round",        "math.atan2",        "math.fmod",
        "math.clampi",       "math.clampf",       "list.sortByKeys",
        "int.checkedAdd",    "int.checkedSub",    "int.checkedMul",
        "int.checkedDiv",    "int.saturatingAdd", "int.saturatingSub",
        "int.saturatingMul",
    };
    constexpr std::size_t kBase = sizeof(kNames) / sizeof(kNames[0]);
    static_assert(kBase + sizeof(kExtra) / sizeof(kExtra[0]) == static_cast<std::size_t>(Intrinsic::Count));
    auto i = static_cast<std::size_t>(in);
    return i < kBase ? kNames[i] : i < static_cast<std::size_t>(Intrinsic::Count) ? kExtra[i - kBase] : "?";
}

namespace {

const char* opName(Op op) {
    static const char* const kNames[] = {
        "block",    "loop",       "if",        "else",       "end",  "br",          "br_if",      "return",
        "trap",     "const",      "move",      "neg",        "not",  "bitnot",      "add",        "sub",
        "mul",      "div",        "rem",       "and",        "or",   "xor",         "shl",        "shr",
        "eq",       "ne",         "lt",        "le",         "gt",   "ge",          "convert",    "call",
        "callhost", "intrinsic",  "field.get", "field.set",  "make", "isnull",      "unwrap",     "wrap",
        "nullof",   "enum.make",  "enum.tag",  "enum.field", "wait", "scope.enter", "scope.exit", "persist",
        "closure",  "call.value",
    };
    return kNames[static_cast<std::size_t>(op)];
}

} // namespace

std::string print(const Module& m) {
    std::string out;
    for (const Annotation& a : m.annotations) {
        out += std::format("@{} on #{}", a.name, a.function);
        for (const AnnotationArg& arg : a.args) {
            std::string v = arg.kind == AnnotationArg::Kind::Str     ? std::format("\"{}\"", arg.text)
                            : arg.kind == AnnotationArg::Kind::Name  ? arg.text
                            : arg.kind == AnnotationArg::Kind::Float ? std::format("{}", arg.floatValue)
                                                                     : std::format("{}", arg.intValue);
            out += arg.key.empty() ? " " + v : std::format(" {}={}", arg.key, v);
        }
        if (a.filter != ~0U) {
            out += std::format(" where #{}", a.filter);
        }
        out += '\n';
    }
    for (std::size_t i = 0; i < m.imports.size(); ++i) {
        out += std::format("import #{} {}(", i, m.imports[i].name);
        for (std::size_t p = 0; p < m.imports[i].params.size(); ++p) {
            out += (p ? ", " : "") + m.types.name(m.imports[i].params[p]);
        }
        out += std::format(") -> {}\n", m.types.name(m.imports[i].result));
    }
    for (std::size_t f = 0; f < m.functions.size(); ++f) {
        const Function& fn = m.functions[f];
        out += std::format("\nfn #{} {}(", f, fn.name);
        for (std::uint32_t p = 0; p < fn.paramCount; ++p) {
            out += std::format("{}r{}: {}", p ? ", " : "", p, m.types.name(fn.regs[p]));
        }
        const char* inl = fn.inlining == Inlining::Always ? " inline" : fn.inlining == Inlining::Never ? " noinline" : "";
        out += std::format(") -> {}{}{}\n", m.types.name(fn.result), fn.exported ? " export" : "", inl);
        int indent = 1;
        for (const Inst& in : fn.code) {
            if (in.op == Op::End || in.op == Op::Else) {
                --indent;
            }
            out.append(static_cast<std::size_t>(indent) * 2, ' ');
            if (in.dst != kNoReg) {
                out += std::format("r{}: {} = ", in.dst, m.types.name(fn.regs[in.dst]));
            }
            out += opName(in.op);
            if (in.op == Op::Intrinsic) {
                out += std::format(" {}", intrinsicName(static_cast<Intrinsic>(in.imm)));
            } else if (in.op == Op::Const) {
                const Constant& c = m.constants[in.imm];
                if (c.kind == ConstKind::Str) {
                    out += std::format(" \"{}\"", c.text);
                } else if (c.kind == ConstKind::Float) {
                    out += std::format(" {}", std::bit_cast<double>(c.bits));
                } else {
                    out += std::format(" {}", static_cast<std::int64_t>(c.bits));
                }
            } else if (in.op == Op::Call || in.op == Op::Closure) {
                out += std::format(" {}", m.functions[in.imm].name);
            } else if (in.op == Op::CallHost) {
                out += std::format(" {}", m.imports[in.imm].name);
            } else if (in.op == Op::Trap) {
                out += std::format(" {}", trapName(static_cast<TrapCode>(in.imm)));
            } else if (in.op == Op::Br || in.op == Op::BrIf || in.op == Op::FieldGet || in.op == Op::FieldSet ||
                       in.op == Op::EnumMake || in.op == Op::EnumField) {
                out += std::format(" #{}", in.imm);
            }
            if (in.a != kNoReg) {
                out += std::format(" r{}", in.a);
            }
            if (in.b != kNoReg) {
                out += std::format(" r{}", in.b);
            }
            for (std::uint32_t i = 0; i < in.argCount; ++i) {
                out += std::format(" r{}", fn.args[in.argBegin + i]);
            }
            out += '\n';
            if (in.op == Op::Block || in.op == Op::Loop || in.op == Op::If || in.op == Op::Else) {
                ++indent;
            }
        }
    }
    return out;
}

} // namespace quartz::ir
