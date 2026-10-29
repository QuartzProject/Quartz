#pragma once

#include "vm/Value.hpp"

#include <cstdint>

namespace quartz::vm {

/// What a Sandbox::WalkFrame holds still to visit.
enum WalkKind : std::uint8_t { kElements, kEntries, kCaptures, kReleaseList, kReleaseMap, kAcrossEntries };

inline std::size_t entrySlots(const Map* m) { return static_cast<std::size_t>(m->keySlots) + m->valueSlots; }

/// The next live position of `m`'s insertion order at or after `pos`.
inline std::uint32_t liveFrom(const Map* m, std::uint32_t pos, std::uint32_t end) {
    while (pos < end && m->order[pos] == 0) {
        ++pos;
    }
    return pos;
}

} // namespace quartz::vm
