#include "Session.hpp"

#include <charconv>
#include <format>
#include <limits>

namespace quartz::dap {

namespace {

std::string_view trimmed(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s.remove_suffix(1);
    }
    return s;
}

void appendUtf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

bool scalarValue(std::uint64_t cp) { return cp <= 0x10FFFF && (cp < 0xD800 || cp > 0xDFFF); }

// The text between `quote`s with the escapes `showValue` writes: \" \' \\ \n \t \r \0 and \u{hex}.
std::expected<std::string, std::string> unquote(std::string_view text, char quote) {
    if (text.size() < 2 || text.front() != quote || text.back() != quote) {
        return std::unexpected(std::format("write the value between {}{}", quote, quote));
    }
    text = text.substr(1, text.size() - 2);
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c != '\\') {
            out += c;
            continue;
        }
        if (++i == text.size()) {
            return std::unexpected("a '\\' ends the text");
        }
        switch (text[i]) {
        case 'n':
            out += '\n';
            break;
        case 't':
            out += '\t';
            break;
        case 'r':
            out += '\r';
            break;
        case '0':
            out += '\0';
            break;
        case '\\':
        case '"':
        case '\'':
        case '{':
            out += text[i];
            break;
        case 'u': {
            std::size_t close = text.find('}', i);
            std::uint32_t cp = 0;
            if (i + 1 >= text.size() || text[i + 1] != '{' || close == std::string_view::npos ||
                std::from_chars(text.data() + i + 2, text.data() + close, cp, 16).ptr != text.data() + close ||
                !scalarValue(cp)) {
                return std::unexpected("write a character as \\u{hex}");
            }
            appendUtf8(out, cp);
            i = close;
            break;
        }
        default:
            return std::unexpected(std::format("unknown escape '\\{}'", text[i]));
        }
    }
    return out;
}

template <class T>
std::expected<T, std::string> integer(std::string_view text, std::string_view type) {
    T v{};
    int base = 10;
    bool negative = text.starts_with('-');
    std::string_view digits = negative ? text.substr(1) : text;
    if (digits.starts_with("0x") || digits.starts_with("0X")) {
        base = 16;
        digits.remove_prefix(2);
    }
    std::string buf = negative ? "-" + std::string(digits) : std::string(digits);
    std::erase(buf, '_');
    auto r = std::from_chars(buf.data(), buf.data() + buf.size(), v, base);
    if (buf.empty() || r.ec != std::errc{} || r.ptr != buf.data() + buf.size()) {
        return std::unexpected(std::format("'{}' is not a {} in range", text, type));
    }
    return v;
}

} // namespace

// Text points at its arena: only a register of the stopped frame can take a new one without outliving it.
std::expected<void, std::string> Session::assign(const vm::ValueRef& v, std::size_t frame, std::string_view text) {
    const vm::Program& p = *target_->program;
    if (v.copied || v.at == nullptr || stopped_ == nullptr) {
        return std::unexpected("this value cannot be changed");
    }
    text = trimmed(text);
    auto* slot = const_cast<vm::Slot*>(v.at);
    std::string type = p.module->types.name(v.type);
    vm::Slot next{};
    switch (p.module->types.kind(v.type)) {
    case ir::TypeKind::Bool:
        if (text != "true" && text != "false") {
            return std::unexpected("a bool is true or false");
        }
        next.u = text == "true" ? 1 : 0;
        break;
    case ir::TypeKind::I8: {
        auto r = integer<std::int8_t>(text, type);
        if (!r) {
            return std::unexpected(r.error());
        }
        next.i = *r;
        break;
    }
    case ir::TypeKind::I16: {
        auto r = integer<std::int16_t>(text, type);
        if (!r) {
            return std::unexpected(r.error());
        }
        next.i = *r;
        break;
    }
    case ir::TypeKind::I32: {
        auto r = integer<std::int32_t>(text, type);
        if (!r) {
            return std::unexpected(r.error());
        }
        next.i = *r;
        break;
    }
    case ir::TypeKind::I64: {
        auto r = integer<std::int64_t>(text, type);
        if (!r) {
            return std::unexpected(r.error());
        }
        next.i = *r;
        break;
    }
    case ir::TypeKind::U8: {
        auto r = integer<std::uint8_t>(text, type);
        if (!r) {
            return std::unexpected(r.error());
        }
        next.u = *r;
        break;
    }
    case ir::TypeKind::U16: {
        auto r = integer<std::uint16_t>(text, type);
        if (!r) {
            return std::unexpected(r.error());
        }
        next.u = *r;
        break;
    }
    case ir::TypeKind::U32: {
        auto r = integer<std::uint32_t>(text, type);
        if (!r) {
            return std::unexpected(r.error());
        }
        next.u = *r;
        break;
    }
    case ir::TypeKind::U64: {
        auto r = integer<std::uint64_t>(text, type);
        if (!r) {
            return std::unexpected(r.error());
        }
        next.u = *r;
        break;
    }
    case ir::TypeKind::F32:
    case ir::TypeKind::F64: {
        double d = 0;
        if (text == "nan" || text == "NaN") {
            d = std::numeric_limits<double>::quiet_NaN();
        } else if (text == "inf" || text == "-inf") {
            d = text == "inf" ? std::numeric_limits<double>::infinity() : -std::numeric_limits<double>::infinity();
        } else if (auto r = std::from_chars(text.data(), text.data() + text.size(), d);
                   text.empty() || r.ec != std::errc{} || r.ptr != text.data() + text.size()) {
            return std::unexpected(std::format("'{}' is not a {}", text, type));
        }
        if (p.module->types.kind(v.type) == ir::TypeKind::F32) {
            next.f32 = static_cast<float>(d);
        } else {
            next.f = d;
        }
        break;
    }
    case ir::TypeKind::Char: {
        auto r = unquote(text, '\'');
        if (!r) {
            return std::unexpected(r.error());
        }
        std::uint32_t cp = 0;
        std::size_t n = r->size();
        auto b = [&](std::size_t i) { return static_cast<std::uint32_t>(static_cast<unsigned char>((*r)[i])); };
        if (n == 1 && b(0) < 0x80) {
            cp = b(0);
        } else if (n == 2 && (b(0) & 0xE0) == 0xC0) {
            cp = (b(0) & 0x1FU) << 6 | (b(1) & 0x3FU);
        } else if (n == 3 && (b(0) & 0xF0) == 0xE0) {
            cp = (b(0) & 0x0FU) << 12 | (b(1) & 0x3FU) << 6 | (b(2) & 0x3FU);
        } else if (n == 4 && (b(0) & 0xF8) == 0xF0) {
            cp = (b(0) & 0x07U) << 18 | (b(1) & 0x3FU) << 12 | (b(2) & 0x3FU) << 6 | (b(3) & 0x3FU);
        } else {
            return std::unexpected("a char is one character between ''");
        }
        if (!scalarValue(cp)) {
            return std::unexpected("not a Unicode scalar value");
        }
        next.u = cp;
        break;
    }
    case ir::TypeKind::Str: {
        bool inFrame = false;
        if (frame < frames_.size() && !shown_.empty() && frame < shown_[0].count) {
            const vm::FrameView& f = frames_[frame];
            std::uint32_t slots = p.functions[f.function].frameSlots;
            inFrame = v.at >= f.regs && v.at < f.regs + slots;
        }
        if (!inFrame) {
            return std::unexpected("text can be set only in the stopped thread's locals and their inline fields");
        }
        auto r = unquote(text, '"');
        if (!r) {
            return std::unexpected(r.error());
        }
        vm::Str* s = stopped_->probeStr(*r);
        if (s == nullptr) {
            return std::unexpected("the text is not valid UTF-8 or does not fit in the memory limit");
        }
        next.p = s;
        break;
    }
    default:
        return std::unexpected(std::format("a {} cannot be set from the debugger", type));
    }
    *slot = next;
    return {};
}

std::expected<Json, std::string> Session::setVariable(const Json& args) {
    std::int64_t id = args["variablesReference"].integer();
    if (id < 1 || static_cast<std::size_t>(id) > refs_.size()) {
        return std::unexpected("no such variable");
    }
    Ref ref = refs_[static_cast<std::size_t>(id - 1)];
    std::string_view name = args["name"].str();
    std::optional<vm::ValueRef> target;
    if (ref.scope >= 0) {
        for (auto& [local, value] : visibleLocals(ref.frame, ref.scope)) {
            if (local == name) {
                target = value;
            }
        }
    } else {
        const vm::Program& p = *target_->program;
        std::size_t start = 0;
        if (std::uint64_t index = 0;
            name.size() > 2 && name.front() == '[' &&
            std::from_chars(name.data() + 1, name.data() + name.size() - 1, index).ec == std::errc{}) {
            start = static_cast<std::size_t>(std::min<std::uint64_t>(index, vm::childCount(p, ref.value)));
        }
        for (vm::ValueChild& c :
             vm::childrenOf(p, ref.value, start, std::max<std::size_t>(vm::childCount(p, ref.value), 1))) {
            if (c.name == name) {
                target = c.value;
                break;
            }
        }
    }
    if (!target) {
        return std::unexpected(std::format("no variable '{}' here", name));
    }
    if (auto done = assign(*target, ref.frame, args["value"].str()); !done) {
        return std::unexpected(done.error());
    }
    Json var = variable(std::string(name), *target, ref.frame);
    return Json::object()
        .set("value", std::string(var["value"].str()))
        .set("type", std::string(var["type"].str()))
        .set("variablesReference", var["variablesReference"].integer());
}

std::expected<Json, std::string> Session::setExpression(const Json& args) {
    std::int64_t frame = args["frameId"].integer();
    std::size_t f = frame >= 1 ? static_cast<std::size_t>(frame - 1) : 0;
    auto v = lookup(f, args["expression"].str());
    if (!v) {
        return std::unexpected(v.error());
    }
    if (auto done = assign(*v, f, args["value"].str()); !done) {
        return std::unexpected(done.error());
    }
    Json var = variable("", *v, f);
    return Json::object()
        .set("value", std::string(var["value"].str()))
        .set("type", std::string(var["type"].str()))
        .set("variablesReference", var["variablesReference"].integer());
}

} // namespace quartz::dap
