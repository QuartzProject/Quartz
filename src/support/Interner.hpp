#pragma once

#include "support/Arena.hpp"

#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace quartz {

struct Symbol {
    std::uint32_t id = 0;

    [[nodiscard]] bool valid() const { return id != 0; }
    friend bool operator==(Symbol, Symbol) = default;
};

class Interner {
public:
    Interner() { names_.emplace_back(); }

    Symbol intern(std::string_view text) {
        auto it = ids_.find(text);
        if (it != ids_.end()) {
            return {it->second};
        }
        std::string_view stored = arena_.copy(text);
        auto id = static_cast<std::uint32_t>(names_.size());
        names_.push_back(stored);
        ids_.emplace(stored, id);
        return {id};
    }

    [[nodiscard]] std::string_view name(Symbol s) const { return names_[s.id]; }

private:
    Arena arena_{16 * 1024};
    std::vector<std::string_view> names_;
    std::unordered_map<std::string_view, std::uint32_t> ids_;
};

} // namespace quartz

template <>
struct std::hash<quartz::Symbol> {
    std::size_t operator()(quartz::Symbol s) const noexcept { return s.id; }
};
