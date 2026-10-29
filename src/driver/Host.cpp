#include "quartz/Quartz.hpp"

#include <algorithm>

namespace quartz {

std::vector<std::uint8_t> deflate(std::span<const std::uint8_t> data, int level, Framing framing) {
    return stdlib::compress(data, level, framing);
}

std::expected<std::vector<std::uint8_t>, std::string> inflate(std::span<const std::uint8_t> data, Framing framing,
                                                              std::size_t maxLength) {
    std::vector<std::uint8_t> out;
    std::string error;
    if (!stdlib::decompress(data, framing, out, maxLength, error)) {
        return std::unexpected(std::move(error));
    }
    return out;
}

namespace {

std::string_view docIn(const std::vector<ir::DeclDocs>& all, std::uint32_t index) {
    auto it = std::lower_bound(all.begin(), all.end(), index,
                               [](const ir::DeclDocs& d, std::uint32_t i) { return d.index < i; });
    return it != all.end() && it->index == index ? std::string_view(it->doc) : std::string_view();
}

} // namespace

std::string_view functionDoc(const vm::Program& program, std::uint32_t function) {
    return docIn(program.module->functionDocs, function);
}

std::string_view typeDoc(const vm::Program& program, ir::TypeId type) { return docIn(program.module->typeDocs, type); }

} // namespace quartz
