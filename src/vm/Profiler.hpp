#pragma once

#include "vm/Sandbox.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace quartz::vm {

/// Calls, exact fuel and wall time per function and call path, from the call probes (vm::Probes::calls).
class Profiler {
public:
    struct Node {
        std::uint32_t function = ~0U; // ~0 for the root
        std::uint32_t parent = 0;
        std::uint64_t calls = 0;
        std::int64_t selfFuel = 0;
        std::int64_t selfNs = 0;
        std::vector<std::uint32_t> children;
    };

    struct FunctionStats {
        std::uint32_t function = 0;
        std::uint64_t calls = 0;
        std::int64_t selfFuel = 0;
        std::int64_t totalFuel = 0;
        std::int64_t selfNs = 0;
        std::int64_t totalNs = 0;
    };

    Profiler();

    /// Becomes the sandbox's probe (replacing any other) until `detach`.
    void attach(Sandbox& sandbox);
    void detach(Sandbox& sandbox);

    /// Node 0 is the root; a node's children are the functions it called.
    [[nodiscard]] const std::vector<Node>& tree() const { return nodes_; }

    /// Per function, by self fuel, highest first; recursion counts a call's total once.
    [[nodiscard]] std::vector<FunctionStats> functions() const;
    /// A flat table and the call tree, for people.
    [[nodiscard]] std::string report(const Program& program) const;
    /// The call tree as a speedscope file (https://www.speedscope.app), with a fuel and a time profile.
    [[nodiscard]] std::string speedscope(const Program& program) const;

    /// Feeds one probe event; attach() routes the sandbox's probes here.
    void onEvent(Sandbox& sandbox, const ProbeEvent& event);

private:
    struct Run {
        std::vector<std::uint32_t> stack;
        std::int64_t lastFuel = 0;
        std::int64_t lastNs = 0;
    };

    std::uint32_t child(std::uint32_t node, std::uint32_t function);
    void account(Run& run, std::int64_t fuel, std::int64_t ns, bool withFuel = true);
    [[nodiscard]] std::vector<std::int64_t> totals(bool fuel) const;

    std::vector<Node> nodes_;
    std::vector<Run> runs_;
};

} // namespace quartz::vm
