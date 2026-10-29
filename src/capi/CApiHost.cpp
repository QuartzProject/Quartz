#include "capi/CApiImpl.hpp"
#include "std/Deflate.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <span>

namespace {

const quartz::ir::TypeInfo* typeInfo(const qz_program* program, uint32_t type) {
    const quartz::ir::TypeTable& types = program->program->module->types;
    return type != 0 && type < types.size() ? &types.info(type) : nullptr;
}

const quartz::ir::DeclDocs* docsOf(const std::vector<quartz::ir::DeclDocs>& all, std::uint32_t index) {
    auto it = std::lower_bound(all.begin(), all.end(), index,
                               [](const quartz::ir::DeclDocs& d, std::uint32_t i) { return d.index < i; });
    return it != all.end() && it->index == index ? &*it : nullptr;
}

uint8_t* copyBytes(const std::vector<std::uint8_t>& bytes, size_t* outLength) {
    auto* out = static_cast<uint8_t*>(std::malloc(std::max<std::size_t>(bytes.size(), 1)));
    if (out != nullptr && !bytes.empty()) {
        std::memcpy(out, bytes.data(), bytes.size());
    }
    if (outLength != nullptr) {
        *outLength = out != nullptr ? bytes.size() : 0;
    }
    return out;
}

char* textOut(std::string_view text) {
    auto* out = static_cast<char*>(std::malloc(text.size() + 1));
    if (out != nullptr) {
        std::memcpy(out, text.data(), text.size());
        out[text.size()] = '\0';
    }
    return out;
}

std::span<const std::uint8_t> bytesIn(const uint8_t* data, size_t length) {
    return data != nullptr ? std::span<const std::uint8_t>(data, length) : std::span<const std::uint8_t>();
}

} // namespace

extern "C" {

qz_slot qz_sandbox_str_lossy(qz_sandbox* sandbox, const char* text, size_t length) {
    qz_slot out{};
    out.p = sandbox->sandbox->newStrLossy(text != nullptr ? std::string_view(text, length) : std::string_view());
    return out;
}

qz_slot qz_call_str_lossy(qz_call* call, const char* text, size_t length) {
    qz_slot out{};
    out.p = call->call.stringLossy(text != nullptr ? std::string_view(text, length) : std::string_view());
    return out;
}

void qz_sandbox_str_lossy_into(qz_sandbox* sandbox, const char* text, size_t length, qz_slot* out) {
    *out = qz_sandbox_str_lossy(sandbox, text, length);
}

void qz_call_str_lossy_into(qz_call* call, const char* text, size_t length, qz_slot* out) {
    *out = qz_call_str_lossy(call, text, length);
}

uint8_t* qz_deflate(const uint8_t* data, size_t length, int level, qz_framing framing, size_t* out_length) {
    if (out_length != nullptr) {
        *out_length = 0;
    }
    auto f = static_cast<quartz::stdlib::Framing>(framing <= QZ_FRAMING_GZIP ? framing : QZ_FRAMING_RAW);
    try {
        return copyBytes(quartz::stdlib::compress(bytesIn(data, length), level, f), out_length);
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}

uint8_t* qz_inflate(const uint8_t* data, size_t length, qz_framing framing, size_t max_length, size_t* out_length,
                    char** error) {
    if (error != nullptr) {
        *error = nullptr;
    }
    if (out_length != nullptr) {
        *out_length = 0;
    }
    std::string why = "unknown framing";
    try {
        std::vector<std::uint8_t> out;
        if (framing <= QZ_FRAMING_GZIP &&
            quartz::stdlib::decompress(bytesIn(data, length), static_cast<quartz::stdlib::Framing>(framing), out,
                                       max_length, why)) {
            return copyBytes(out, out_length);
        }
    } catch (const std::bad_alloc&) {
        why = "out of memory";
    }
    if (error != nullptr) {
        *error = textOut(why);
    }
    return nullptr;
}

uint32_t qz_crc32(uint32_t crc, const uint8_t* data, size_t length) {
    return quartz::stdlib::crc32(bytesIn(data, length), crc);
}

const char* qz_program_function_doc(const qz_program* program, uint32_t function) {
    const quartz::ir::DeclDocs* d = docsOf(program->program->module->functionDocs, function);
    return d != nullptr ? d->doc.c_str() : "";
}

const char* qz_type_doc(const qz_program* program, uint32_t type) {
    const quartz::ir::DeclDocs* d = docsOf(program->program->module->typeDocs, type);
    return d != nullptr ? d->doc.c_str() : "";
}

const char* qz_type_member_doc(const qz_program* program, uint32_t type, uint32_t index) {
    const quartz::ir::DeclDocs* d = docsOf(program->program->module->typeDocs, type);
    return d != nullptr && index < d->members.size() ? d->members[index].c_str() : "";
}

bool qz_type_variant_boxed(const qz_program* program, uint32_t type, uint32_t variant, uint32_t index) {
    const quartz::ir::TypeInfo* info = typeInfo(program, type);
    return info != nullptr && info->kind == quartz::ir::TypeKind::Enum && variant < info->variants.size() &&
           info->variants[variant].isBoxed(index);
}

} // extern "C"
