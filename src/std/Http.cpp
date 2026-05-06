#include "std/Natives.hpp"

#include <format>
#include <string>

namespace quartz::stdlib {

namespace {

bool unreserved(unsigned char c, bool form) {
    bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    return alnum || c == '-' || c == '.' || c == '_' || (form ? c == '*' : c == '~');
}

// RFC 3986 unreserved characters stay; a form (application/x-www-form-urlencoded, WHATWG) also keeps `*` and writes
// a space as `+`.
void encodeTo(std::string& out, std::string_view s, bool form) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    for (char ch : s) {
        auto c = static_cast<unsigned char>(ch);
        if (unreserved(c, form)) {
            out += ch;
        } else if (form && c == ' ') {
            out += '+';
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 15];
        }
    }
}

int hexValue(char c) {
    return c >= '0' && c <= '9'   ? c - '0'
           : c >= 'a' && c <= 'f' ? c - 'a' + 10
           : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                  : -1;
}

bool decodeTo(std::string& out, std::string_view s, bool form, std::string& error) {
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%') {
            int hi = i + 2 < s.size() ? hexValue(s[i + 1]) : -1;
            int lo = i + 2 < s.size() ? hexValue(s[i + 2]) : -1;
            if (hi < 0 || lo < 0) {
                error = std::format("bad percent escape at {}", i);
                return false;
            }
            out += static_cast<char>(hi << 4 | lo);
            i += 2;
        } else {
            out += form && s[i] == '+' ? ' ' : s[i];
        }
    }
    std::size_t len = out.size();
    for (std::size_t i = 0; i < len;) {
        auto c = static_cast<unsigned char>(out[i]);
        std::size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        bool ok = n != 0 && i + n <= len;
        std::uint32_t cp = n == 1 ? c : c & (0x7F >> n);
        for (std::size_t k = 1; ok && k < n; ++k) {
            ok = (static_cast<unsigned char>(out[i + k]) & 0xC0) == 0x80;
            cp = (cp << 6) | (static_cast<unsigned char>(out[i + k]) & 0x3F);
        }
        static constexpr std::uint32_t kMin[] = {0, 0, 0x80, 0x800, 0x10000};
        if (!ok || cp < kMin[n] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            error = "the decoded bytes are not UTF-8 text";
            return false;
        }
        i += n;
    }
    return true;
}

bool percentEncode(vm::HostCall& call, void*) {
    std::string out;
    encodeTo(out, strArg(call.args[0]), false);
    return putStr(call, out, &call.result);
}

bool percentDecode(vm::HostCall& call, void*) {
    std::string_view s = strArg(call.args[0]);
    std::string out;
    std::string error;
    if (!call.charge(static_cast<std::int64_t>(s.size() / 16 + 1))) {
        return false;
    }
    if (!decodeTo(out, s, false, error)) {
        return errStr(call, "percent decoding: " + error);
    }
    return okStr(call, out);
}

// `[(str, str)]` in and out: each element is a tuple of two `str` slots.
bool encodeQuery(vm::HostCall& call, void*) {
    const auto* pairs = static_cast<const vm::List*>(call.args[0].p);
    std::string out;
    for (std::uint32_t i = 0; i < pairs->len; ++i) {
        const vm::Slot* e = pairs->data + static_cast<std::size_t>(i) * pairs->elemSlots;
        out += i > 0 ? "&" : "";
        encodeTo(out, strArg(e[0]), true);
        out += '=';
        encodeTo(out, strArg(e[1]), true);
    }
    return putStr(call, out, &call.result);
}

bool decodeQuery(vm::HostCall& call, void* user) {
    std::string_view s = strArg(call.args[0]);
    if (!call.charge(static_cast<std::int64_t>(s.size() / 4 + 1))) {
        return false;
    }
    const auto* ctx = static_cast<const NativeCtx*>(user);
    const ir::TypeTable& types = call.sandbox.program().module->types;
    ir::TypeId listType = types.info(ctx->import->result).variants[0].payload[0];
    vm::List* list = call.sandbox.newList(types.info(listType).elem);
    if (list == nullptr) {
        return false;
    }
    std::string key;
    std::string value;
    std::string error;
    for (std::size_t from = 0; from <= s.size() && !s.empty();) {
        std::size_t amp = s.find('&', from);
        std::string_view part = s.substr(from, amp == std::string_view::npos ? std::string_view::npos : amp - from);
        from = amp == std::string_view::npos ? s.size() + 1 : amp + 1;
        if (part.empty()) {
            continue;
        }
        std::size_t eq = part.find('=');
        key.clear();
        value.clear();
        if (!decodeTo(key, part.substr(0, eq), true, error) ||
            (eq != std::string_view::npos && !decodeTo(value, part.substr(eq + 1), true, error))) {
            return errStr(call, "query: " + error);
        }
        vm::Slot pair[2];
        if (!putStr(call, key, &pair[0]) || !putStr(call, value, &pair[1]) || !call.sandbox.listPush(list, pair)) {
            return false;
        }
    }
    call.out[0].u = 0;
    call.out[1].p = list;
    return true;
}

// The capability defaults: a host grants HTTP or a log sink by binding these names to its own functions.
bool sendUnavailable(vm::HostCall& call, void*) {
    return errStr(call, "http: this host does not grant network access");
}

bool discardLog(vm::HostCall&, void*) { return true; }

} // namespace

void addHttpNatives(std::vector<Native>& out) {
    out.insert(out.end(), {
                              {"org.quartzlang.http.percentEncode", percentEncode},
                              {"org.quartzlang.http.percentDecode", percentDecode},
                              {"org.quartzlang.http.encodeQuery", encodeQuery},
                              {"org.quartzlang.http.decodeQuery", decodeQuery},
                              {"org.quartzlang.http.send", sendUnavailable},
                              {"org.quartzlang.log.emit", discardLog},
                          });
}

} // namespace quartz::stdlib
