#include "std/Natives.hpp"

#include <array>
#include <format>
#include <vector>

namespace quartz::stdlib {

namespace {

constexpr std::string_view kStandard = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr std::string_view kUrl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

constexpr std::array<std::uint8_t, 256> decodeTable(std::string_view alphabet) {
    std::array<std::uint8_t, 256> t{};
    t.fill(0xFF);
    for (std::size_t i = 0; i < alphabet.size(); ++i) {
        t[static_cast<unsigned char>(alphabet[i])] = static_cast<std::uint8_t>(i);
    }
    return t;
}

constexpr auto kStandardDecode = decodeTable(kStandard);
constexpr auto kUrlDecode = decodeTable(kUrl);

int hexDigit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool encode64(vm::HostCall& call, void*) {
    auto in = bytesArg(call.args[0]);
    return putStr(call, base64Encode(in, call.args[1].u != 0, call.args[2].u != 0), &call.result);
}

bool decode64(vm::HostCall& call, void*) {
    std::string_view text = strArg(call.args[0]);
    if (!call.charge(static_cast<std::int64_t>(text.size() / 16 + 1))) {
        return false;
    }
    std::vector<std::uint8_t> out;
    std::string error;
    if (!base64Decode(text, call.args[1].u != 0, out, error)) {
        return errStr(call, "base64: " + error);
    }
    return okBytes(call, out);
}

bool decodeHex(vm::HostCall& call, void*) {
    std::string_view text = strArg(call.args[0]);
    if (!call.charge(static_cast<std::int64_t>(text.size() / 16 + 1))) {
        return false;
    }
    if (text.size() % 2 != 0) {
        return errStr(call, std::format("hex: odd number of digits ({})", text.size()));
    }
    std::vector<std::uint8_t> out(text.size() / 2);
    for (std::size_t i = 0; i < out.size(); ++i) {
        int hi = hexDigit(text[2 * i]);
        int lo = hexDigit(text[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return errStr(call, std::format("hex: bad digit at {}", hi < 0 ? 2 * i : 2 * i + 1));
        }
        out[i] = static_cast<std::uint8_t>(hi << 4 | lo);
    }
    return okBytes(call, out);
}

} // namespace

std::string base64Encode(std::span<const std::uint8_t> data, bool url, bool pad) {
    const char* alphabet = url ? kUrl.data() : kStandard.data();
    std::size_t whole = data.size() / 3;
    std::size_t rest = data.size() - whole * 3;
    std::string out(whole * 4 + (rest == 0 ? 0 : pad ? 4 : rest + 1), '=');
    char* o = out.data();
    const std::uint8_t* in = data.data();
    for (std::size_t g = 0; g < whole; ++g, in += 3, o += 4) {
        std::uint32_t v = (static_cast<std::uint32_t>(in[0]) << 16) | (in[1] << 8) | in[2];
        o[0] = alphabet[v >> 18];
        o[1] = alphabet[(v >> 12) & 63];
        o[2] = alphabet[(v >> 6) & 63];
        o[3] = alphabet[v & 63];
    }
    if (rest > 0) {
        std::uint32_t v = static_cast<std::uint32_t>(in[0]) << 16 | (rest == 2 ? in[1] << 8 : 0);
        o[0] = alphabet[v >> 18];
        o[1] = alphabet[(v >> 12) & 63];
        if (rest == 2) {
            o[2] = alphabet[(v >> 6) & 63];
        }
    }
    return out;
}

bool base64Decode(std::string_view text, bool url, std::vector<std::uint8_t>& out, std::string& error) {
    const auto& table = url ? kUrlDecode : kStandardDecode;
    std::size_t body = text.size();
    while (body > 0 && text[body - 1] == '=' && text.size() - body < 2) {
        --body;
    }
    if (body < text.size() && text.size() % 4 != 0) {
        error = "wrong padding";
        return false;
    }
    if (body % 4 == 1) {
        error = "truncated input";
        return false;
    }
    out.resize(body / 4 * 3);
    std::size_t full = body / 4 * 4;
    for (std::size_t i = 0; i < full; i += 4) {
        std::uint32_t a = table[static_cast<unsigned char>(text[i])];
        std::uint32_t b = table[static_cast<unsigned char>(text[i + 1])];
        std::uint32_t c = table[static_cast<unsigned char>(text[i + 2])];
        std::uint32_t d = table[static_cast<unsigned char>(text[i + 3])];
        if ((a | b | c | d) >= 64) {
            std::size_t bad = i;
            while (table[static_cast<unsigned char>(text[bad])] != 0xFF) {
                ++bad;
            }
            error = std::format("unexpected character at {}", bad);
            return false;
        }
        std::uint32_t v = a << 18 | b << 12 | c << 6 | d;
        std::size_t o = i / 4 * 3;
        out[o] = static_cast<std::uint8_t>(v >> 16);
        out[o + 1] = static_cast<std::uint8_t>(v >> 8);
        out[o + 2] = static_cast<std::uint8_t>(v);
    }
    std::uint32_t acc = 0;
    int bits = 0;
    for (std::size_t i = full; i < body; ++i) {
        std::uint8_t d = table[static_cast<unsigned char>(text[i])];
        if (d == 0xFF) {
            error = std::format("unexpected character at {}", i);
            return false;
        }
        acc = (acc << 6) | d;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>(acc >> bits));
            acc &= (1u << bits) - 1;
        }
    }
    if (acc != 0) {
        error = "non-zero bits after the last byte";
        return false;
    }
    return true;
}

void addEncodingNatives(std::vector<Native>& out) {
    out.insert(out.end(), {
                              {"org.quartzlang.encoding.base64Encode", encode64},
                              {"org.quartzlang.encoding.base64Decode", decode64},
                              {"org.quartzlang.encoding.hexDecode", decodeHex},
                          });
}

} // namespace quartz::stdlib
