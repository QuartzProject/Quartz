#include "vm/Profiler.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <functional>

namespace quartz::vm {

namespace {

std::int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool profilerProbe(Sandbox& sandbox, const ProbeEvent& event, void* user) {
    static_cast<Profiler*>(user)->onEvent(sandbox, event);
    return true;
}

void jsonString(std::string& out, std::string_view s) {
    out += '"';
    for (char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            out += std::format("\\u{:04x}", static_cast<unsigned>(c));
        } else {
            out += c;
        }
    }
    out += '"';
}

} // namespace

Profiler::Profiler() { nodes_.emplace_back(); }

void Profiler::attach(Sandbox& sandbox) { sandbox.setProbe(profilerProbe, this); }

void Profiler::detach(Sandbox& sandbox) { sandbox.setProbe(nullptr); }

std::uint32_t Profiler::child(std::uint32_t node, std::uint32_t function) {
    for (std::uint32_t c : nodes_[node].children) {
        if (nodes_[c].function == function) {
            return c;
        }
    }
    auto id = static_cast<std::uint32_t>(nodes_.size());
    Node n;
    n.function = function;
    n.parent = node;
    nodes_.push_back(std::move(n));
    nodes_[node].children.push_back(id);
    return id;
}

void Profiler::account(Run& run, std::int64_t fuel, std::int64_t ns, bool withFuel) {
    Node& top = nodes_[run.stack.empty() ? 0 : run.stack.back()];
    if (withFuel) {
        top.selfFuel += run.lastFuel - fuel;
        run.lastFuel = fuel;
    }
    top.selfNs += ns - run.lastNs;
    run.lastNs = ns;
}

void Profiler::onEvent(Sandbox& sandbox, const ProbeEvent& event) {
    std::int64_t ns = nowNs();
    std::int64_t fuel = sandbox.fuelLeft();
    switch (event.kind) {
    case ProbeEvent::Kind::Begin: {
        if (!runs_.empty()) {
            account(runs_.back(), 0, ns, false); // a nested run (a state initializer) restores its caller's fuel
        }
        Run run;
        run.lastFuel = fuel;
        run.lastNs = ns;
        // A resumed task starts inside its frames, which the profiler last saw in another run.
        std::vector<FrameView> frames = sandbox.frames();
        std::uint32_t node = 0;
        for (std::size_t i = frames.size(); i > 0; --i) {
            if (i == 1 && frames[0].pc == 0) {
                break; // a fresh call: its entry probe pushes it
            }
            node = child(node, frames[i - 1].function);
            run.stack.push_back(node);
        }
        runs_.push_back(std::move(run));
        return;
    }
    case ProbeEvent::Kind::End: {
        if (runs_.empty()) {
            return;
        }
        account(runs_.back(), fuel, ns);
        runs_.pop_back();
        if (!runs_.empty()) {
            runs_.back().lastNs = ns;
        }
        return;
    }
    case ProbeEvent::Kind::Enter: {
        if (runs_.empty()) {
            return;
        }
        Run& run = runs_.back();
        account(run, fuel, ns);
        std::uint32_t node = child(run.stack.empty() ? 0 : run.stack.back(), event.function);
        ++nodes_[node].calls;
        run.stack.push_back(node);
        return;
    }
    case ProbeEvent::Kind::Leave: {
        if (runs_.empty()) {
            return;
        }
        Run& run = runs_.back();
        account(run, fuel, ns);
        if (!run.stack.empty()) {
            run.stack.pop_back();
        }
        return;
    }
    default:
        return;
    }
}

std::vector<std::int64_t> Profiler::totals(bool fuel) const {
    std::vector<std::int64_t> out(nodes_.size(), 0);
    // Children always come after their parent, so one backward pass sums every subtree.
    for (std::size_t i = nodes_.size(); i > 0; --i) {
        const Node& n = nodes_[i - 1];
        out[i - 1] += fuel ? n.selfFuel : n.selfNs;
        if (i > 1) {
            out[n.parent] += out[i - 1];
        }
    }
    return out;
}

std::vector<Profiler::FunctionStats> Profiler::functions() const {
    std::vector<std::int64_t> fuel = totals(true);
    std::vector<std::int64_t> ns = totals(false);
    std::vector<FunctionStats> out;
    auto statsOf = [&](std::uint32_t function) -> FunctionStats& {
        for (FunctionStats& s : out) {
            if (s.function == function) {
                return s;
            }
        }
        out.push_back({function, 0, 0, 0, 0, 0});
        return out.back();
    };
    for (std::uint32_t i = 1; i < nodes_.size(); ++i) {
        const Node& n = nodes_[i];
        FunctionStats& s = statsOf(n.function);
        s.calls += n.calls;
        s.selfFuel += n.selfFuel;
        s.selfNs += n.selfNs;
        bool nested = false;
        for (std::uint32_t a = n.parent; a != 0 && !nested; a = nodes_[a].parent) {
            nested = nodes_[a].function == n.function;
        }
        if (!nested) {
            s.totalFuel += fuel[i];
            s.totalNs += ns[i];
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const FunctionStats& a, const FunctionStats& b) {
        return a.selfFuel != b.selfFuel ? a.selfFuel > b.selfFuel : a.function < b.function;
    });
    return out;
}

std::string Profiler::report(const Program& program) const {
    std::vector<std::int64_t> fuel = totals(true);
    std::vector<std::int64_t> ns = totals(false);
    auto name = [&](std::uint32_t f) {
        return f < program.functions.size() ? program.functions[f].name : std::string("?");
    };
    std::vector<FunctionStats> stats = functions();
    std::string out = std::format("profile: {} fuel, {:.3f} ms, {} functions\n\n", fuel[0],
                                  static_cast<double>(ns[0]) / 1e6, stats.size());
    out += std::format("{:>12} {:>14} {:>14} {:>10} {:>10}  {}\n", "calls", "self fuel", "total fuel", "self ms",
                       "total ms", "function");
    for (const FunctionStats& s : stats) {
        out += std::format("{:>12} {:>14} {:>14} {:>10.3f} {:>10.3f}  {}\n", s.calls, s.selfFuel, s.totalFuel,
                           static_cast<double>(s.selfNs) / 1e6, static_cast<double>(s.totalNs) / 1e6, name(s.function));
    }
    out += "\ncall tree (total fuel, share, calls), paths under 0.5% left out:\n";
    std::int64_t all = std::max<std::int64_t>(fuel[0], 1);
    std::function<void(std::uint32_t, int)> walk = [&](std::uint32_t node, int depth) {
        std::vector<std::uint32_t> kids = nodes_[node].children;
        std::stable_sort(kids.begin(), kids.end(), [&](std::uint32_t a, std::uint32_t b) { return fuel[a] > fuel[b]; });
        for (std::uint32_t c : kids) {
            if (fuel[c] * 200 < all || depth > 48) {
                continue;
            }
            out += std::format("{:>14} {:>6.1f}% {:>10}  {}{}\n", fuel[c],
                               100.0 * static_cast<double>(fuel[c]) / static_cast<double>(all), nodes_[c].calls,
                               std::string(static_cast<std::size_t>(depth) * 2, ' '), name(nodes_[c].function));
            walk(c, depth + 1);
        }
    };
    walk(0, 0);
    return out;
}

std::string Profiler::speedscope(const Program& program) const {
    std::string out = R"({"$schema":"https://www.speedscope.app/file-format-schema.json","shared":{"frames":[)";
    std::vector<std::int64_t> frameOf(program.functions.size(), -1);
    std::int64_t frames = 0;
    for (std::uint32_t i = 1; i < nodes_.size(); ++i) {
        std::uint32_t f = nodes_[i].function;
        if (f < frameOf.size() && frameOf[f] < 0) {
            frameOf[f] = frames++;
            out += frames > 1 ? "," : "";
            out += R"({"name":)";
            jsonString(out, program.functions[f].name);
            if (!program.functions[f].file.empty()) {
                out += R"(,"file":)";
                jsonString(out, program.functions[f].file);
            }
            out += "}";
        }
    }
    out += "]},\"profiles\":[";
    for (int kind = 0; kind < 2; ++kind) {
        bool fuel = kind == 0;
        std::string samples;
        std::string weights;
        std::int64_t total = 0;
        for (std::uint32_t i = 1; i < nodes_.size(); ++i) {
            std::int64_t w = fuel ? nodes_[i].selfFuel : nodes_[i].selfNs;
            if (w <= 0 || nodes_[i].function >= frameOf.size()) {
                continue;
            }
            std::vector<std::int64_t> stack;
            for (std::uint32_t n = i; n != 0; n = nodes_[n].parent) {
                stack.push_back(frameOf[nodes_[n].function]);
            }
            samples += samples.empty() ? "[" : ",[";
            for (std::size_t k = stack.size(); k > 0; --k) {
                samples += std::to_string(stack[k - 1]) + (k > 1 ? "," : "");
            }
            samples += "]";
            weights += (weights.empty() ? "" : ",") + std::to_string(w);
            total += w;
        }
        out += kind > 0 ? "," : "";
        out += std::format(R"({{"type":"sampled","name":"{}","unit":"{}","startValue":0,"endValue":{},)",
                           fuel ? "fuel" : "wall time", fuel ? "none" : "nanoseconds", total);
        out += "\"samples\":[" + samples + "],\"weights\":[" + weights + "]}";
    }
    out += "],\"name\":\"quartz\",\"activeProfileIndex\":0,\"exporter\":\"quartz\"}\n";
    return out;
}

} // namespace quartz::vm
