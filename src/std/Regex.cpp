#include "std/regex/Regex.hpp"

#include "std/Natives.hpp"

#include <algorithm>
#include <format>
#include <memory>
#include <optional>

namespace quartz::stdlib {

namespace {

using regex::kNoPos;

constexpr std::size_t kCacheEntries = 64;
constexpr std::size_t kCacheBytes = 8u << 20;
// Calibrated so a unit stays near the cost of a backward jump (D109): a visited instruction is one unit (skipped bytes
// are counted in sixteenths); a compile costs a fixed 128 plus 16 per pattern byte and 2 per instruction.
constexpr std::uint64_t kSkippedPerUnit = 16;
constexpr std::uint64_t kCompileFixed = 128;
constexpr std::uint64_t kCompilePerByte = 16;
constexpr std::uint64_t kCompilePerInst = 2;

} // namespace

struct RegexCache {
    struct Entry {
        std::string pattern;
        std::size_t hash = 0;
        regex::Program program;
        std::unique_ptr<regex::Matcher> matcher;
        std::size_t bytes = 0;
        std::uint64_t used = 0;
    };

    std::vector<std::unique_ptr<Entry>> entries;
    std::size_t bytes = 0;
    std::uint64_t tick = 0;

    void evict(vm::Sandbox& sb) {
        auto oldest = std::min_element(entries.begin(), entries.end(),
                                       [](const auto& a, const auto& b) { return a->used < b->used; });
        bytes -= (*oldest)->bytes;
        sb.giveMemory((*oldest)->bytes);
        entries.erase(oldest);
    }
};

namespace {

struct Charger {
    vm::HostCall& call;
    std::uint64_t charged = 0;

    static bool flush(void* ctx, regex::Meter& m) {
        auto* self = static_cast<Charger*>(ctx);
        std::uint64_t total = m.work + m.skipped / kSkippedPerUnit;
        std::uint64_t due = total - self->charged;
        self->charged = total;
        return self->call.charge(static_cast<std::int64_t>(due));
    }
};

struct Compiled {
    RegexCache::Entry* entry = nullptr;
    std::optional<regex::Error> error;
};

// The cached program of `pattern`, compiling it on a miss; a miss is charged, so fuel follows this sandbox's history.
Compiled lookup(vm::HostCall& call, void* user, std::string_view pattern) {
    const auto* ctx = static_cast<const NativeCtx*>(user);
    if (!ctx->data->regex) {
        ctx->data->regex = std::make_shared<RegexCache>();
    }
    RegexCache& cache = *ctx->data->regex;
    if (!call.charge(static_cast<std::int64_t>(pattern.size() / 16 + 1))) {
        return {};
    }
    std::size_t hash = std::hash<std::string_view>{}(pattern);
    for (auto& e : cache.entries) {
        if (e->hash == hash && e->pattern == pattern) {
            e->used = ++cache.tick;
            return {e.get(), std::nullopt};
        }
    }
    auto program = regex::compile(pattern);
    std::size_t insts = program ? program->insts.size() : program.error().insts;
    if (!call.charge(
            static_cast<std::int64_t>(kCompileFixed + pattern.size() * kCompilePerByte + insts * kCompilePerInst))) {
        return {};
    }
    if (!program) {
        return {nullptr, std::move(program.error())};
    }
    auto e = std::make_unique<RegexCache::Entry>();
    e->bytes = sizeof(RegexCache::Entry) + pattern.size() + program->footprint();
    while (!cache.entries.empty() && (cache.entries.size() >= kCacheEntries || cache.bytes + e->bytes > kCacheBytes)) {
        cache.evict(call.sandbox);
    }
    while (!call.sandbox.takeMemory(e->bytes)) {
        if (cache.entries.empty()) {
            call.sandbox.fail(ir::TrapCode::OutOfMemory, "compiled regex does not fit in the memory quota");
            return {};
        }
        cache.evict(call.sandbox);
    }
    e->pattern = std::string(pattern);
    e->hash = hash;
    e->program = std::move(*program);
    e->matcher = std::make_unique<regex::Matcher>(e->program);
    e->used = ++cache.tick;
    cache.bytes += e->bytes;
    cache.entries.push_back(std::move(e));
    return {cache.entries.back().get(), std::nullopt};
}

std::string describe(const regex::Error& e) { return std::format("{} at position {}", e.message, e.position); }

// A `Regex` built by hand around a bad pattern is a misuse, not bad data.
RegexCache::Entry* entryOf(vm::HostCall& call, void* user) {
    Compiled c = lookup(call, user, strArg(call.args[0]));
    if (c.error) {
        misuse(call, "invalid regex: " + describe(*c.error));
    }
    return c.entry;
}

struct Searcher {
    vm::HostCall& call;
    RegexCache::Entry& entry;
    std::string_view text;
    Charger charger{call};
    regex::Meter meter{.flush = Charger::flush, .ctx = &charger};
    std::vector<std::uint32_t> slots;

    Searcher(vm::HostCall& c, RegexCache::Entry& e, std::string_view t, std::uint32_t wanted)
        : call(c), entry(e), text(t), slots(std::min(wanted, 2 * e.program.groups()), kNoPos) {}

    regex::Found run(regex::SearchOptions o) {
        o.slots = static_cast<std::uint32_t>(slots.size());
        regex::Found f = entry.matcher->search(text, o, slots, meter);
        if (f != regex::Found::Stopped && !Charger::flush(&charger, meter)) {
            return regex::Found::Stopped;
        }
        return f;
    }

    /// Up to `limit` matches (all when negative) as Python iterates: the next search starts where the last match
    /// ended and skips an empty match right there when the last match was empty too.
    template <typename F>
    bool each(std::int64_t limit, F&& onMatch) {
        std::uint32_t from = 0;
        bool lastEmpty = false;
        for (std::int64_t n = 0; limit < 0 || n < limit; ++n) {
            regex::Found f = run({.from = from, .nonEmptyAtFrom = lastEmpty});
            if (f == regex::Found::Stopped) {
                return false;
            }
            if (f == regex::Found::No) {
                break;
            }
            if (!onMatch()) {
                return false;
            }
            lastEmpty = slots[0] == slots[1];
            from = slots[1];
        }
        return true;
    }
};

vm::List* newResultList(vm::HostCall& call, void* user, bool insideResult) {
    const auto* ctx = static_cast<const NativeCtx*>(user);
    const ir::TypeTable& types = call.sandbox.program().module->types;
    ir::TypeId listType = ctx->import->result;
    if (insideResult) {
        listType = types.info(listType).variants[0].payload[0];
    }
    return call.sandbox.newList(types.info(listType).elem);
}

bool pushSpans(vm::HostCall& call, vm::List* list, std::span<const std::uint32_t> slots) {
    if (!chargeWrite(call, slots.size() * 8)) {
        return false;
    }
    for (std::uint32_t s : slots) {
        vm::Slot v{};
        v.i = s == kNoPos ? -1 : static_cast<std::int64_t>(s);
        if (!call.sandbox.listPush(list, &v)) {
            return false;
        }
    }
    return true;
}

bool regexGroups(vm::HostCall& call, void* user) {
    Compiled c = lookup(call, user, strArg(call.args[0]));
    if (c.error) {
        return errStr(call, describe(*c.error));
    }
    if (c.entry == nullptr) {
        return false;
    }
    vm::List* list = newResultList(call, user, true);
    if (list == nullptr) {
        return false;
    }
    for (const std::string& name : c.entry->program.names) {
        vm::Slot v{};
        if (!putStr(call, name, &v) || !call.sandbox.listPush(list, &v)) {
            return false;
        }
    }
    call.out[0].u = 0;
    call.out[1].p = list;
    return true;
}

bool regexIsMatch(vm::HostCall& call, void* user) {
    RegexCache::Entry* e = entryOf(call, user);
    if (e == nullptr) {
        return false;
    }
    bool full = (call.args[2].u & 1) != 0;
    Searcher s(call, *e, strArg(call.args[1]), 0);
    regex::Found f = s.run({.anchored = full, .fullOnly = full, .earliest = true});
    call.result.u = f == regex::Found::Yes ? 1 : 0;
    return f != regex::Found::Stopped;
}

bool regexFind(vm::HostCall& call, void* user) {
    RegexCache::Entry* e = entryOf(call, user);
    if (e == nullptr) {
        return false;
    }
    Searcher s(call, *e, strArg(call.args[1]), kNoPos);
    regex::Found f = s.run({});
    vm::List* list = f == regex::Found::Stopped ? nullptr : newResultList(call, user, false);
    if (list == nullptr || (f == regex::Found::Yes && !pushSpans(call, list, s.slots))) {
        return false;
    }
    call.result.p = list;
    return true;
}

bool regexFindAll(vm::HostCall& call, void* user) {
    RegexCache::Entry* e = entryOf(call, user);
    vm::List* list = e == nullptr ? nullptr : newResultList(call, user, false);
    if (list == nullptr) {
        return false;
    }
    Searcher s(call, *e, strArg(call.args[1]), kNoPos);
    if (!s.each(-1, [&] { return pushSpans(call, list, s.slots); })) {
        return false;
    }
    call.result.p = list;
    return true;
}

struct Piece {
    std::uint32_t group; // kNoPos: the literal text
    std::string_view literal;
};

// `$$` is a dollar, `$n` and `${n}` a group by number, `${name}` one by name; any other `$` is literal.
bool parseTemplate(std::string_view t, const regex::Program& p, std::vector<Piece>& out, std::string& error) {
    std::size_t lit = 0;
    auto flush = [&](std::size_t end) {
        if (end > lit) {
            out.push_back({kNoPos, t.substr(lit, end - lit)});
        }
    };
    auto digits = [](std::string_view s) {
        return !s.empty() && s.size() <= 9 &&
               std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
    };
    for (std::size_t i = 0; i < t.size();) {
        if (t[i] != '$' || i + 1 >= t.size()) {
            ++i;
            continue;
        }
        char c = t[i + 1];
        std::string_view ref;
        std::size_t end = i + 2;
        if (c == '$') {
            flush(i + 1);
            lit = i + 2;
            i += 2;
            continue;
        }
        if (c >= '0' && c <= '9') {
            while (end < t.size() && t[end] >= '0' && t[end] <= '9') {
                ++end;
            }
            ref = t.substr(i + 1, end - i - 1);
        } else if (c == '{') {
            std::size_t close = t.find('}', i + 2);
            if (close == std::string_view::npos) {
                error = std::format("unclosed '${{' at {} in the replacement", i);
                return false;
            }
            ref = t.substr(i + 2, close - i - 2);
            end = close + 1;
        } else {
            ++i;
            continue;
        }
        std::uint32_t group = kNoPos;
        if (digits(ref)) {
            std::uint64_t n = 0;
            for (char d : ref) {
                n = n * 10 + static_cast<std::uint64_t>(d - '0');
            }
            group = n < p.groups() ? static_cast<std::uint32_t>(n) : kNoPos;
        } else {
            auto it = std::find(p.names.begin() + 1, p.names.end(), ref);
            group = ref.empty() || it == p.names.end() ? kNoPos : static_cast<std::uint32_t>(it - p.names.begin());
        }
        if (group == kNoPos) {
            error = std::format("the replacement refers to group '{}', which the pattern does not have", ref);
            return false;
        }
        flush(i);
        out.push_back({group, {}});
        lit = end;
        i = end;
    }
    flush(t.size());
    return true;
}

bool regexReplace(vm::HostCall& call, void* user) {
    RegexCache::Entry* e = entryOf(call, user);
    if (e == nullptr) {
        return false;
    }
    std::string_view text = strArg(call.args[1]);
    std::vector<Piece> pieces;
    std::string error;
    if (!parseTemplate(strArg(call.args[2]), e->program, pieces, error)) {
        return misuse(call, error);
    }
    bool groups =
        std::any_of(pieces.begin(), pieces.end(), [](const Piece& p) { return p.group > 0 && p.group != kNoPos; });
    Searcher s(call, *e, text, groups ? kNoPos : 2);
    std::string out;
    std::size_t room = call.sandbox.memoryLimit() - call.sandbox.memoryUsed();
    std::uint32_t copied = 0;
    auto append = [&](std::string_view part) {
        if (part.size() > room - out.size()) {
            return call.sandbox.fail(ir::TrapCode::OutOfMemory, "the replaced text does not fit in the memory quota");
        }
        out += part;
        return chargeWrite(call, part.size());
    };
    bool ok = s.each((call.args[3].u & 1) != 0 ? -1 : 1, [&] {
        if (!append(text.substr(copied, s.slots[0] - copied))) {
            return false;
        }
        for (const Piece& p : pieces) {
            std::uint32_t a = p.group == kNoPos ? 0 : s.slots[2 * p.group];
            std::uint32_t b = p.group == kNoPos ? 0 : s.slots[2 * p.group + 1];
            if (!append(p.group == kNoPos ? p.literal : a == kNoPos ? std::string_view{} : text.substr(a, b - a))) {
                return false;
            }
        }
        copied = s.slots[1];
        return true;
    });
    if (!ok || !append(text.substr(copied))) {
        return false;
    }
    call.result.p = call.string(out);
    return call.result.p != nullptr;
}

bool regexSplit(vm::HostCall& call, void* user) {
    RegexCache::Entry* e = entryOf(call, user);
    vm::List* list = e == nullptr ? nullptr : newResultList(call, user, false);
    if (list == nullptr) {
        return false;
    }
    std::string_view text = strArg(call.args[1]);
    std::int64_t limit = call.args[2].i;
    Searcher s(call, *e, text, 2);
    std::uint32_t from = 0;
    auto piece = [&](std::uint32_t end) {
        vm::Slot v{};
        return chargeWrite(call, 128) && putStr(call, text.substr(from, end - from), &v) &&
               call.sandbox.listPush(list, &v);
    };
    bool ok = s.each(limit > 0 ? limit - 1 : -1, [&] {
        bool pushed = piece(s.slots[0]);
        from = s.slots[1];
        return pushed;
    });
    if (!ok || !piece(static_cast<std::uint32_t>(text.size()))) {
        return false;
    }
    call.result.p = list;
    return true;
}

bool escapeRegex(vm::HostCall& call, void*) {
    std::string_view text = strArg(call.args[0]);
    std::string out;
    for (char c : text) {
        if (std::string_view("\\.+*?()|[]{}^$-#&~").find(c) != std::string_view::npos) {
            out += '\\';
        }
        out += c;
    }
    return putStr(call, out, &call.result);
}

} // namespace

void addRegexNatives(std::vector<Native>& out) {
    out.insert(out.end(), {
                              {"org.quartzlang.regex.regexGroups", regexGroups},
                              {"org.quartzlang.regex.regexIsMatch", regexIsMatch},
                              {"org.quartzlang.regex.regexFind", regexFind},
                              {"org.quartzlang.regex.regexFindAll", regexFindAll},
                              {"org.quartzlang.regex.regexReplace", regexReplace},
                              {"org.quartzlang.regex.regexSplit", regexSplit},
                              {"org.quartzlang.regex.escapeRegex", escapeRegex},
                          });
}

} // namespace quartz::stdlib
