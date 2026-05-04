#pragma once

#include "std/Std.hpp"

#include <span>
#include <string>
#include <string_view>

namespace quartz::stdlib {

// Shared argument and result helpers of the C++ natives; `Bytes` has the runtime layout of `str`.

inline std::span<const std::uint8_t> bytesArg(const vm::Slot& s) {
    const auto* b = static_cast<const vm::Str*>(s.p);
    return {reinterpret_cast<const std::uint8_t*>(b->data), b->len};
}

inline std::string_view strArg(const vm::Slot& s) { return static_cast<const vm::Str*>(s.p)->view(); }

/// Charges the fuel of writing `bytes` new bytes (vm::kFuelWriteFactor of 16 per unit) plus one unit.
inline bool chargeWrite(vm::HostCall& call, std::size_t bytes) {
    return call.charge(static_cast<std::int64_t>(bytes / 4 + 1));
}

inline bool putBytes(vm::HostCall& call, std::span<const std::uint8_t> data, vm::Slot* into) {
    if (!chargeWrite(call, data.size())) {
        return false;
    }
    into->p = call.string(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
    return into->p != nullptr;
}

inline bool putStr(vm::HostCall& call, std::string_view text, vm::Slot* into) {
    return putBytes(call, {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()}, into);
}

/// `Ok(bytes)` of a `Result<Bytes, str>` or `Result<str, str>`: a tag slot plus one payload slot.
inline bool okBytes(vm::HostCall& call, std::span<const std::uint8_t> data) {
    call.out[0].u = 0;
    return putBytes(call, data, &call.out[1]);
}

inline bool okStr(vm::HostCall& call, std::string_view text) {
    call.out[0].u = 0;
    return putStr(call, text, &call.out[1]);
}

/// `Ok(v)` of a result whose value takes one scalar slot.
inline bool okScalar(vm::HostCall& call, std::uint64_t v) {
    call.out[0].u = 0;
    call.out[1].u = v;
    return true;
}

inline bool errStr(vm::HostCall& call, std::string_view message) {
    call.out[0].u = 1;
    call.out[1].p = call.string(message);
    return call.out[1].p != nullptr;
}

/// Traps with a host error: for arguments a correct program never passes, not for bad data.
inline bool misuse(vm::HostCall& call, std::string message) {
    call.error = std::move(message);
    return false;
}

/// RFC 4648 base64; `url` picks the `-_` alphabet.
std::string base64Encode(std::span<const std::uint8_t> data, bool url, bool pad);
/// Padding is optional but must be right when present; stray characters and non-zero trailing bits are errors.
bool base64Decode(std::string_view text, bool url, std::vector<std::uint8_t>& out, std::string& error);

void addCryptoNatives(std::vector<Native>& out);
void addEncodingNatives(std::vector<Native>& out);
void addDecimalNatives(std::vector<Native>& out);

} // namespace quartz::stdlib
