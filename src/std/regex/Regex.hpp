#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace quartz::stdlib::regex {

inline constexpr std::size_t kMaxPatternBytes = 64 * 1024;
inline constexpr std::uint32_t kMaxRepeat = 1000;
inline constexpr std::uint32_t kMaxDepth = 200;
inline constexpr std::uint32_t kMaxInsts = 50'000;
inline constexpr std::uint32_t kMaxGroups = 1000;
// Pike VM capture storage is instructions times slots per thread list, so their product is bounded too.
inline constexpr std::size_t kMaxCaptureCells = std::size_t{1} << 21;
inline constexpr std::uint32_t kNoPos = 0xFFFFFFFF;

struct Error {
    std::string message;
    std::size_t position = 0;
    std::size_t insts = 0; // instructions emitted before the error, for the fuel of a failed compile
};

struct Range {
    std::uint32_t lo;
    std::uint32_t hi;
};

/// A set of code points: a bitmap for ASCII and sorted, disjoint ranges for the rest.
struct CharSet {
    std::uint64_t ascii[2] = {0, 0};
    std::vector<Range> wide;

    [[nodiscard]] bool has(std::uint32_t c) const;
};

enum class Assert : std::uint8_t { Begin, End, BeginLine, EndLine, Word, NotWord };

enum class Op : std::uint8_t { Char, Set, Any, AnyNotNl, Split, Jmp, Save, Assert, Match };

struct Inst {
    Op op;
    Assert assertion = Assert::Begin;
    std::uint32_t x = 0; // Char: low code point; Set: set index; Split, Jmp: target; Save: slot
    std::uint32_t y = 0; // Char: high code point; Split: the lower-priority target
};

/// A compiled pattern: an NFA over code points whose consuming instructions always continue at pc + 1.
struct Program {
    std::vector<Inst> insts;
    std::vector<CharSet> sets;
    std::vector<std::string> names; // per group, "" when unnamed; group 0 is the whole match
    std::string prefix;             // UTF-8 bytes every match starts with, when known
    std::uint64_t firstBytes[4] = {0, 0, 0, 0};
    bool useFirstBytes = false;
    bool anchoredStart = false; // every match starts at the beginning of the text

    [[nodiscard]] std::uint32_t groups() const { return static_cast<std::uint32_t>(names.size()); }

    /// Bytes the program and its search buffers take, for the sandbox quota.
    [[nodiscard]] std::size_t footprint() const;
};

std::expected<Program, Error> compile(std::string_view pattern);

/// Work done: instruction visits, plus sixteenths of a visit for the bytes the prefilter skips.
struct Meter {
    std::uint64_t work = 0;
    std::uint64_t skipped = 0;
    bool (*flush)(void* ctx, Meter& meter) = nullptr; // called now and then; false stops the search
    void* ctx = nullptr;
};

struct SearchOptions {
    std::uint32_t from = 0;
    std::uint32_t slots = 2;     // capture slots to fill: 0 (only whether), 2 (bounds) or 2 * groups
    bool anchored = false;       // the match must start at `from`
    bool fullOnly = false;       // the match must end at the end of the text
    bool earliest = false;       // stop at the first match found, its bounds unspecified
    bool nonEmptyAtFrom = false; // an empty match at `from` does not count (iteration after an empty match)
};

enum class Found : std::uint8_t { No, Yes, Stopped };

/// Reusable buffers of the Pike VM for one program.
class Matcher {
public:
    explicit Matcher(const Program& program);
    Found search(std::string_view text, const SearchOptions& options, std::span<std::uint32_t> slots, Meter& meter);

private:
    struct List {
        std::vector<std::uint32_t> dense;
        std::vector<std::uint32_t> sparse;
        std::vector<std::uint32_t> caps;
        std::uint32_t size = 0;

        bool contains(std::uint32_t pc) const {
            std::uint32_t i = sparse[pc];
            return i < size && dense[i] == pc;
        }

        void insert(std::uint32_t pc) {
            sparse[pc] = size;
            dense[size++] = pc;
        }
    };

    struct Frame {
        std::uint32_t pc; // kNoPos: restore `slot` to `value`
        std::uint32_t slot;
        std::uint32_t value;
    };

    void add(List& list, std::uint32_t pc, std::uint32_t pos, std::uint32_t* caps, Meter& meter);
    [[nodiscard]] bool holds(Assert a, std::uint32_t pos) const;

    const Program& prog_;
    List lists_[2];
    std::vector<Frame> stack_;
    std::vector<std::uint32_t> work_;
    std::string_view text_;
    std::uint32_t slots_ = 0;
};

/// Decodes one code point at `i` and advances; a malformed byte reads as U+FFFD and advances by one.
std::uint32_t decode(std::string_view s, std::size_t& i);

} // namespace quartz::stdlib::regex
