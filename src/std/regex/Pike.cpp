#include "std/regex/Regex.hpp"

#include <algorithm>
#include <cstring>

namespace quartz::stdlib::regex {

std::uint32_t decode(std::string_view s, std::size_t& i) {
    auto b0 = static_cast<unsigned char>(s[i]);
    if (b0 < 0x80) {
        ++i;
        return b0;
    }
    std::size_t n = 0;
    std::uint32_t cp = 0;
    std::uint32_t min = 0;
    if (b0 >= 0xC2 && b0 <= 0xDF) {
        n = 2, cp = b0 & 0x1Fu, min = 0x80;
    } else if (b0 >= 0xE0 && b0 <= 0xEF) {
        n = 3, cp = b0 & 0x0Fu, min = 0x800;
    } else if (b0 >= 0xF0 && b0 <= 0xF4) {
        n = 4, cp = b0 & 0x07u, min = 0x10000;
    }
    if (n == 0 || s.size() - i < n) {
        ++i;
        return 0xFFFD;
    }
    for (std::size_t k = 1; k < n; ++k) {
        auto b = static_cast<unsigned char>(s[i + k]);
        if ((b & 0xC0) != 0x80) {
            ++i;
            return 0xFFFD;
        }
        cp = (cp << 6) | (b & 0x3Fu);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        ++i;
        return 0xFFFD;
    }
    i += n;
    return cp;
}

namespace {

constexpr std::uint64_t kFlushWork = 1 << 16;
constexpr std::uint64_t kSearchWork = 8;
// Sixteenths of a work unit per byte skipped: the substring search runs about twice as fast as the byte table.
constexpr std::uint64_t kPrefixCost = 2;
constexpr std::uint64_t kTableCost = 4;

bool wordByte(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }

bool inBytes(const std::uint64_t* bytes, unsigned char b) { return (bytes[b >> 6] >> (b & 63) & 1) != 0; }

} // namespace

Matcher::Matcher(const Program& program) : prog_(program) {
    std::size_t n = program.insts.size();
    std::size_t slots = 2 * static_cast<std::size_t>(program.groups());
    for (List& l : lists_) {
        l.dense.assign(n, 0);
        l.sparse.assign(n, 0);
        l.caps.assign(n * slots, kNoPos);
    }
    stack_.reserve(2 * n);
    work_.assign(2 * slots, kNoPos);
}

bool Matcher::holds(Assert a, std::uint32_t pos) const {
    auto len = static_cast<std::uint32_t>(text_.size());
    switch (a) {
    case Assert::Begin:
        return pos == 0;
    case Assert::End:
        return pos == len;
    case Assert::BeginLine:
        return pos == 0 || text_[pos - 1] == '\n';
    case Assert::EndLine:
        return pos == len || text_[pos] == '\n';
    case Assert::Word:
    case Assert::NotWord: {
        bool before = pos > 0 && wordByte(text_[pos - 1]);
        bool after = pos < len && wordByte(text_[pos]);
        return (before != after) == (a == Assert::Word);
    }
    }
    return false;
}

// Follows the empty transitions from `pc` depth first, in priority order, and adds every thread it reaches once.
void Matcher::add(List& list, std::uint32_t pc0, std::uint32_t pos, std::uint32_t* caps, Meter& meter) {
    std::uint32_t* cur = work_.data();
    std::copy_n(caps, slots_, cur);
    stack_.clear();
    stack_.push_back({pc0, 0, 0});
    while (!stack_.empty()) {
        Frame f = stack_.back();
        stack_.pop_back();
        if (f.pc == kNoPos) {
            cur[f.slot] = f.value;
            continue;
        }
        std::uint32_t pc = f.pc;
        while (!list.contains(pc)) {
            list.insert(pc);
            ++meter.work;
            const Inst& in = prog_.insts[pc];
            if (in.op == Op::Jmp) {
                pc = in.x;
            } else if (in.op == Op::Split) {
                stack_.push_back({in.y, 0, 0});
                pc = in.x;
            } else if (in.op == Op::Save) {
                if (in.x < slots_) {
                    stack_.push_back({kNoPos, in.x, cur[in.x]});
                    cur[in.x] = pos;
                }
                ++pc;
            } else if (in.op == Op::Assert) {
                if (!holds(in.assertion, pos)) {
                    break;
                }
                ++pc;
            } else {
                std::copy_n(cur, slots_, &list.caps[static_cast<std::size_t>(pc) * slots_]);
                meter.work += slots_ / 4;
                break;
            }
        }
    }
}

Found Matcher::search(std::string_view text, const SearchOptions& o, std::span<std::uint32_t> out, Meter& meter) {
    text_ = text;
    slots_ = std::min<std::uint32_t>(o.slots, 2 * prog_.groups());
    auto len = static_cast<std::uint32_t>(text.size());
    if (o.from > len || (prog_.anchoredStart && o.from > 0)) {
        return Found::No;
    }
    bool anchored = o.anchored || prog_.anchoredStart;
    List* clist = &lists_[0];
    List* nlist = &lists_[1];
    clist->size = 0;
    nlist->size = 0;
    std::uint32_t* blank = work_.data() + slots_;
    std::fill_n(blank, slots_, kNoPos);
    bool matched = false;
    meter.work += kSearchWork;
    std::uint64_t flushed = meter.work;
    const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
    for (std::uint32_t i = o.from;;) {
        if (!matched && (!anchored || i == o.from)) {
            if (clist->size == 0 && !anchored) {
                std::uint32_t p = i;
                if (prog_.prefix.size() >= 2) {
                    std::size_t k = text.find(prog_.prefix, i);
                    p = k == std::string_view::npos ? len : static_cast<std::uint32_t>(k);
                    meter.skipped += kPrefixCost * (p - i);
                } else if (prog_.useFirstBytes) {
                    while (p < len && !inBytes(prog_.firstBytes, bytes[p])) {
                        ++p;
                    }
                    meter.skipped += kTableCost * (p - i);
                }
                i = p;
                if (i == len && (prog_.useFirstBytes || prog_.prefix.size() >= 2)) {
                    break;
                }
            }
            if (!prog_.useFirstBytes || (i < len && inBytes(prog_.firstBytes, bytes[i]))) {
                add(*clist, 0, i, blank, meter);
            }
        }
        if (clist->size == 0) {
            break;
        }
        std::uint32_t c = kNoPos;
        std::size_t next = i;
        if (i < len) {
            c = decode(text, next);
        }
        auto after = static_cast<std::uint32_t>(next);
        meter.work += 1 + clist->size;
        for (std::uint32_t j = 0; j < clist->size; ++j) {
            std::uint32_t pc = clist->dense[j];
            const Inst& in = prog_.insts[pc];
            std::uint32_t* caps = &clist->caps[static_cast<std::size_t>(pc) * slots_];
            bool step = false;
            switch (in.op) {
            case Op::Match:
                if ((o.fullOnly && i != len) || (o.nonEmptyAtFrom && i == o.from)) {
                    continue;
                }
                if (o.earliest) {
                    return Found::Yes;
                }
                std::copy_n(caps, slots_, out.data());
                matched = true;
                j = clist->size;
                continue;
            case Op::Char:
                step = c >= in.x && c <= in.y && c != kNoPos;
                break;
            case Op::Set:
                step = c != kNoPos && prog_.sets[in.x].has(c);
                break;
            case Op::Any:
                step = c != kNoPos;
                break;
            case Op::AnyNotNl:
                step = c != kNoPos && c != '\n';
                break;
            default:
                break;
            }
            if (step) {
                add(*nlist, pc + 1, after, caps, meter);
            }
        }
        std::swap(clist, nlist);
        nlist->size = 0;
        if (i >= len) {
            break;
        }
        i = after;
        if (meter.flush != nullptr && meter.work - flushed >= kFlushWork) {
            flushed = meter.work;
            if (!meter.flush(meter.ctx, meter)) {
                return Found::Stopped;
            }
        }
    }
    return matched ? Found::Yes : Found::No;
}

} // namespace quartz::stdlib::regex
