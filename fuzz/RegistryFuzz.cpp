#include "quartz/Registry.hpp"

#include <cstdlib>
#include <string_view>

namespace reg = quartz::registry;

// Every parser of registry files must reject or accept without crashing, and what it accepts must print back the same.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    std::string_view text(reinterpret_cast<const char*>(data), size);
    if (auto index = reg::parseIndex(text, "org.example.x")) {
        std::string printed = reg::printIndex("org.example.x", *index);
        auto again = reg::parseIndex(printed, "org.example.x");
        if (!again || reg::printIndex("org.example.x", *again) != printed) {
            std::abort();
        }
    }
    if (auto lock = reg::parseLock(text)) {
        std::string printed = reg::printLock(*lock);
        auto again = reg::parseLock(printed);
        if (!again || reg::printLock(*again) != printed) {
            std::abort();
        }
        quartz::Manifest root;
        root.name = "org.example.root";
        root.version = "1.0.0";
        for (const reg::Locked& p : lock->packages) {
            root.dependencies.emplace_back(p.name, "*");
        }
        (void)reg::lockMatches(root, *lock);
    }
    if (auto owners = reg::parseOwners(text)) {
        (void)reg::printOwners(*owners);
    }
    (void)reg::unpackSources({data, size});
    return 0;
}
