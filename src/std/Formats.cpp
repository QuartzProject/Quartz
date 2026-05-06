#include "std/Natives.hpp"

#include <algorithm>
#include <format>
#include <vector>

namespace quartz::stdlib {

namespace {

bool separatorOf(vm::HostCall& call, const vm::Slot& arg, char& sep) {
    std::string_view s = strArg(arg);
    if (s.size() != 1 || s[0] == '"' || s[0] == '\r' || s[0] == '\n' || (s[0] & 0x80) != 0) {
        return misuse(call, "the CSV separator is one ASCII character other than a quote or a line break");
    }
    sep = s[0];
    return true;
}

// RFC 4180, accepting LF or CR line ends and quotes inside unquoted fields; a blank line is a row with no fields.
bool parseCsv(vm::HostCall& call, void* user) {
    std::string_view text = strArg(call.args[0]);
    char sep = ',';
    if (!separatorOf(call, call.args[1], sep) || !call.charge(static_cast<std::int64_t>(text.size() / 4 + 1))) {
        return false;
    }
    const auto* ctx = static_cast<const NativeCtx*>(user);
    const ir::TypeTable& types = call.sandbox.program().module->types;
    ir::TypeId rowsType = types.info(ctx->import->result).variants[0].payload[0];
    ir::TypeId rowType = types.info(rowsType).elem;
    vm::List* rows = call.sandbox.newList(rowType);
    if (rows == nullptr) {
        return false;
    }
    std::size_t line = 1;
    std::size_t i = 0;
    std::string field;
    while (i < text.size()) {
        vm::List* row = call.sandbox.newList(ir::kStr);
        if (row == nullptr) {
            return false;
        }
        bool blank = text[i] == '\n' || text[i] == '\r';
        while (!blank) {
            field.clear();
            if (i < text.size() && text[i] == '"') {
                std::size_t opened = line;
                for (++i;; ++i) {
                    if (i >= text.size()) {
                        return errStr(call, std::format("csv: line {}: unterminated quoted field", opened));
                    }
                    if (text[i] == '"') {
                        if (i + 1 < text.size() && text[i + 1] == '"') {
                            field += '"';
                            ++i;
                            continue;
                        }
                        ++i;
                        break;
                    }
                    line += text[i] == '\n' ? 1 : 0;
                    field += text[i];
                }
                if (i < text.size() && text[i] != sep && text[i] != '\n' && text[i] != '\r') {
                    return errStr(call, std::format("csv: line {}: unexpected character after a quoted field", line));
                }
            } else {
                while (i < text.size() && text[i] != sep && text[i] != '\n' && text[i] != '\r') {
                    field += text[i++];
                }
            }
            vm::Slot s{};
            if (!putStr(call, field, &s) || !call.sandbox.listPush(row, &s)) {
                return false;
            }
            if (i < text.size() && text[i] == sep) {
                ++i;
                continue;
            }
            break;
        }
        if (i < text.size() && text[i] == '\r') {
            ++i;
        }
        if (i < text.size() && text[i] == '\n') {
            ++i;
        }
        ++line;
        vm::Slot r{};
        r.p = row;
        if (!call.sandbox.listPush(rows, &r)) {
            return false;
        }
    }
    call.out[0].u = 0;
    call.out[1].p = rows;
    return true;
}

bool toCsv(vm::HostCall& call, void*) {
    const auto* rows = static_cast<const vm::List*>(call.args[0].p);
    char sep = ',';
    if (!separatorOf(call, call.args[1], sep)) {
        return false;
    }
    std::string_view end = call.args[2].u != 0 ? "\r\n" : "\n";
    std::string out;
    std::size_t charged = 0;
    for (std::uint32_t r = 0; r < rows->len; ++r) {
        const auto* row = static_cast<const vm::List*>(rows->data[r].p);
        for (std::uint32_t c = 0; c < row->len; ++c) {
            std::string_view f = strArg(row->data[c]);
            bool quote = f.find_first_of(std::string{sep, '"', '\r', '\n'}) != std::string_view::npos ||
                         (row->len == 1 && f.empty());
            out += c > 0 ? std::string_view(&sep, 1) : std::string_view{};
            if (!quote) {
                out += f;
                continue;
            }
            out += '"';
            for (char ch : f) {
                out += ch;
                if (ch == '"') {
                    out += '"';
                }
            }
            out += '"';
        }
        out += end;
        if (!call.charge(static_cast<std::int64_t>((out.size() - charged) / 16 + 1))) {
            return false;
        }
        charged = out.size();
    }
    return putStr(call, out, &call.result);
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

int hexValue(char c) {
    return c >= '0' && c <= '9'   ? c - '0'
           : c >= 'a' && c <= 'f' ? c - 'a' + 10
           : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                  : -1;
}

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\f'; }

// One logical line of a properties file, joined across backslash continuations; the escapes are left in place.
struct LogicalLines {
    std::string_view text;
    std::size_t i = 0;
    std::size_t line = 0;

    bool next(std::string& out, std::size_t& firstLine) {
        out.clear();
        while (i < text.size()) {
            while (i < text.size() && (isSpace(text[i]) || text[i] == '\r' || text[i] == '\n')) {
                line += text[i] == '\n' || (text[i] == '\r' && (i + 1 >= text.size() || text[i + 1] != '\n'));
                ++i;
            }
            if (i >= text.size()) {
                return false;
            }
            firstLine = line + 1;
            bool comment = text[i] == '#' || text[i] == '!';
            while (true) {
                std::size_t from = i;
                while (i < text.size() && text[i] != '\n' && text[i] != '\r') {
                    ++i;
                }
                std::string_view piece = text.substr(from, i - from);
                std::size_t slashes = 0;
                while (slashes < piece.size() && piece[piece.size() - 1 - slashes] == '\\') {
                    ++slashes;
                }
                bool more = !comment && slashes % 2 == 1;
                out += more ? piece.substr(0, piece.size() - 1) : piece;
                if (i < text.size() && text[i] == '\r') {
                    ++i;
                }
                if (i < text.size() && text[i] == '\n') {
                    ++i;
                }
                ++line;
                if (!more || i >= text.size()) {
                    break;
                }
                while (i < text.size() && isSpace(text[i])) {
                    ++i;
                }
            }
            if (!comment) {
                return true;
            }
            out.clear();
        }
        return false;
    }
};

// Java's Properties.load escapes: \t \n \r \f, \uXXXX (surrogate pairs joined), and a backslash before anything else.
bool unescape(std::string_view s, std::string& out, std::string& error) {
    out.clear();
    std::uint32_t high = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        std::uint32_t cp = static_cast<unsigned char>(s[i]);
        bool unit = false;
        if (s[i] == '\\' && i + 1 < s.size()) {
            char e = s[++i];
            cp = static_cast<unsigned char>(e);
            if (e == 't' || e == 'n' || e == 'r' || e == 'f') {
                cp = e == 't' ? '\t' : e == 'n' ? '\n' : e == 'r' ? '\r' : '\f';
            } else if (e == 'u') {
                cp = 0;
                for (int k = 0; k < 4; ++k) {
                    int h = i + 1 < s.size() ? hexValue(s[i + 1]) : -1;
                    if (h < 0) {
                        error = "malformed \\uXXXX escape";
                        return false;
                    }
                    cp = cp * 16 + static_cast<std::uint32_t>(h);
                    ++i;
                }
                unit = true;
            }
        } else if (s[i] == '\\') {
            continue;
        }
        if (high != 0) {
            if (!unit || cp < 0xDC00 || cp > 0xDFFF) {
                error = "a \\u escape of a high surrogate without its low surrogate";
                return false;
            }
            appendUtf8(out, 0x10000 + ((high - 0xD800) << 10) + (cp - 0xDC00));
            high = 0;
            continue;
        }
        if (unit && cp >= 0xD800 && cp <= 0xDBFF) {
            high = cp;
            continue;
        }
        if (unit && cp >= 0xDC00 && cp <= 0xDFFF) {
            error = "a \\u escape of a lone low surrogate";
            return false;
        }
        if (unit) {
            appendUtf8(out, cp);
        } else {
            out += static_cast<char>(cp);
        }
    }
    if (high != 0) {
        error = "a \\u escape of a high surrogate without its low surrogate";
        return false;
    }
    return true;
}

bool parseProperties(vm::HostCall& call, void* user) {
    std::string_view text = strArg(call.args[0]);
    if (!call.charge(static_cast<std::int64_t>(text.size() / 4 + 1))) {
        return false;
    }
    const auto* ctx = static_cast<const NativeCtx*>(user);
    const ir::TypeTable& types = call.sandbox.program().module->types;
    vm::Map* map = call.sandbox.newMap(types.info(ctx->import->result).variants[0].payload[0]);
    if (map == nullptr) {
        return false;
    }
    LogicalLines lines{text};
    std::string logical;
    std::size_t lineNo = 0;
    std::string key;
    std::string value;
    std::string error;
    while (lines.next(logical, lineNo)) {
        std::size_t k = 0;
        while (k < logical.size() && !isSpace(logical[k]) && logical[k] != '=' && logical[k] != ':') {
            k += logical[k] == '\\' ? 2 : 1;
        }
        k = std::min(k, logical.size());
        std::size_t v = k;
        while (v < logical.size() && isSpace(logical[v])) {
            ++v;
        }
        if (v < logical.size() && (logical[v] == '=' || logical[v] == ':')) {
            ++v;
        }
        while (v < logical.size() && isSpace(logical[v])) {
            ++v;
        }
        if (!unescape(std::string_view(logical).substr(0, k), key, error) ||
            !unescape(std::string_view(logical).substr(v), value, error)) {
            return errStr(call, std::format("properties: line {}: {}", lineNo, error));
        }
        vm::Slot ks{};
        vm::Slot vs{};
        if (!putStr(call, key, &ks) || !putStr(call, value, &vs) || !call.sandbox.mapSet(map, &ks, &vs)) {
            return false;
        }
    }
    call.out[0].u = 0;
    call.out[1].p = map;
    return true;
}

void escapeProperty(std::string& out, std::string_view s, bool key) {
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        switch (c) {
        case '\\':
            out += "\\\\";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\f':
            out += "\\f";
            break;
        case '=':
        case ':':
        case '#':
        case '!':
            out += '\\';
            out += c;
            break;
        case ' ':
            out += key || i == 0 ? "\\ " : " ";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                out += std::format("\\u{:04X}", static_cast<unsigned>(c));
            } else {
                out += c;
            }
        }
    }
}

// Like Properties.store to a Writer, without the date comment and with the keys sorted: same map, same text.
bool toProperties(vm::HostCall& call, void*) {
    const auto* m = static_cast<const vm::Map*>(call.args[0].p);
    std::vector<const vm::Slot*> entries;
    std::uint32_t stride = 1u + m->valueSlots;
    for (std::uint32_t i = 0; i < m->cap; ++i) {
        if (m->hashes[i] >= 2) {
            entries.push_back(m->entries + static_cast<std::size_t>(i) * stride);
        }
    }
    std::sort(entries.begin(), entries.end(),
              [](const vm::Slot* a, const vm::Slot* b) { return strArg(a[0]) < strArg(b[0]); });
    std::string out;
    for (const vm::Slot* e : entries) {
        escapeProperty(out, strArg(e[0]), true);
        out += '=';
        escapeProperty(out, strArg(e[1]), false);
        out += '\n';
    }
    if (!call.charge(static_cast<std::int64_t>(out.size() / 4 + entries.size() + 1))) {
        return false;
    }
    return putStr(call, out, &call.result);
}

} // namespace

bool parseMsgPack(vm::HostCall& call, void* user);
bool toMsgPack(vm::HostCall& call, void* user);

void addFormatNatives(std::vector<Native>& out) {
    out.insert(out.end(), {
                              {"org.quartzlang.formats.parseCsv", parseCsv},
                              {"org.quartzlang.formats.toCsv", toCsv},
                              {"org.quartzlang.formats.parseProperties", parseProperties},
                              {"org.quartzlang.formats.toProperties", toProperties},
                              {"org.quartzlang.formats.parseMsgPack", parseMsgPack},
                              {"org.quartzlang.formats.toMsgPack", toMsgPack},
                          });
}

} // namespace quartz::stdlib
