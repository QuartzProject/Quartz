#include "std/Builder.hpp"
#include "std/Natives.hpp"

#include <charconv>
#include <format>
#include <unordered_map>
#include <vector>

namespace quartz::stdlib {

namespace {

// The CLDR operands of a formatted number: integer digits `i` (its last 18 digits when longer), fraction `f`, and the
// number of fraction digits `v`.
struct Operands {
    std::uint64_t i = 0;
    bool longInteger = false;
    std::uint64_t f = 0;
    std::size_t v = 0;
    bool plain = true; // false for exponents, infinities and NaN
};

Operands operandsOf(std::string_view text) {
    Operands o;
    if (text.starts_with('-')) {
        text.remove_prefix(1);
    }
    std::size_t dot = text.find('.');
    std::string_view whole = text.substr(0, dot);
    std::string_view frac = dot == std::string_view::npos ? std::string_view{} : text.substr(dot + 1);
    for (std::string_view part : {whole, frac}) {
        for (char c : part) {
            o.plain = o.plain && c >= '0' && c <= '9';
        }
    }
    if (!o.plain || whole.empty()) {
        o.plain = false;
        return o;
    }
    o.longInteger = whole.size() > 18;
    for (char c : whole.substr(whole.size() > 18 ? whole.size() - 18 : 0)) {
        o.i = o.i * 10 + static_cast<std::uint64_t>(c - '0');
    }
    o.v = frac.size();
    for (char c : frac.substr(0, 18)) {
        o.f = o.f * 10 + static_cast<std::uint64_t>(c - '0');
    }
    return o;
}

std::string languageOf(std::string_view locale) {
    std::string lang;
    for (char c : locale) {
        if (c == '-' || c == '_') {
            break;
        }
        lang += static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    }
    return lang;
}

// CLDR cardinal plural rules (CLDR 46) for the languages Quartz knows; any other gets only `other`.
std::string_view category(std::string_view lang, const Operands& o) {
    if (!o.plain) {
        return "other";
    }
    bool integral = o.f == 0;
    auto is = [&](std::uint64_t k) { return !o.longInteger && o.i == k; };
    std::uint64_t mod10 = o.i % 10;
    std::uint64_t mod100 = o.i % 100;
    bool million = !is(0) && o.i % 1000000 == 0 && o.v == 0;
    if (lang == "en" || lang == "de") {
        return is(1) && o.v == 0 ? "one" : "other";
    }
    if (lang == "es") {
        return is(1) && integral ? "one" : million ? "many" : "other";
    }
    if (lang == "fr") {
        return is(0) || is(1) ? "one" : million ? "many" : "other";
    }
    if (lang == "ru") {
        if (o.v != 0) {
            return "other";
        }
        if (mod10 == 1 && mod100 != 11) {
            return "one";
        }
        if (mod10 >= 2 && mod10 <= 4 && (mod100 < 12 || mod100 > 14)) {
            return "few";
        }
        return "many";
    }
    if (lang == "ar") {
        if (!integral) {
            return "other";
        }
        return is(0)                         ? "zero"
               : is(1)                       ? "one"
               : is(2)                       ? "two"
               : mod100 >= 3 && mod100 <= 10 ? "few"
               : mod100 >= 11                ? "many"
                                             : "other";
    }
    return "other";
}

constexpr int kMaxDepth = 32;

class Formatter {
public:
    Formatter(vm::HostCall& call, std::string_view text, std::string lang) : call_(call), s_(text), lang_(lang) {}

    std::unordered_map<std::string_view, const vm::Slot*> args;
    std::string out;
    std::string error;

    bool run() { return message(false, false, {}, true, 0); }

private:
    bool fail(std::string_view what) {
        if (error.empty()) {
            error = std::format("{} at offset {}", what, i_);
        }
        return false;
    }

    void skipSpace() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) {
            ++i_;
        }
    }

    std::string_view word() {
        std::size_t from = i_;
        while (i_ < s_.size() && ((s_[i_] >= 'a' && s_[i_] <= 'z') || (s_[i_] >= 'A' && s_[i_] <= 'Z') ||
                                  (s_[i_] >= '0' && s_[i_] <= '9') || s_[i_] == '_')) {
            ++i_;
        }
        return s_.substr(from, i_ - from);
    }

    // Text up to the end, or up to the `}` closing a nested message (left unread); `#` is `hash` in plural cases.
    bool message(bool nested, bool plural, std::string_view hash, bool emit, int depth) {
        while (i_ < s_.size()) {
            char c = s_[i_];
            if (c == '\'') {
                char next = i_ + 1 < s_.size() ? s_[i_ + 1] : '\0';
                if (next == '\'') {
                    out += emit ? "'" : "";
                    i_ += 2;
                } else if (next == '{' || next == '}' || next == '|' || (next == '#' && plural)) {
                    for (++i_; i_ < s_.size(); ++i_) {
                        if (s_[i_] == '\'') {
                            if (i_ + 1 < s_.size() && s_[i_ + 1] == '\'') {
                                ++i_;
                            } else {
                                ++i_;
                                break;
                            }
                        }
                        if (emit) {
                            out += s_[i_];
                        }
                    }
                } else {
                    out += emit ? "'" : "";
                    ++i_;
                }
            } else if (c == '{') {
                if (!argument(emit, depth + 1)) {
                    return false;
                }
            } else if (c == '}') {
                return nested || fail("unmatched '}'");
            } else {
                if (emit) {
                    out += c == '#' && plural ? hash : std::string_view(&s_[i_], 1);
                }
                ++i_;
            }
        }
        return !nested || fail("missing '}'");
    }

    const vm::Slot* lookup(std::string_view name) {
        auto it = args.find(name);
        if (it == args.end()) {
            fail(std::format("no argument named '{}'", name));
            return nullptr;
        }
        return it->second;
    }

    bool scalarText(const vm::Slot* v, std::string& text, bool numeric) {
        switch (static_cast<Tag>(v[0].u)) {
        case Tag::Int:
            text = std::to_string(v[1].i);
            return true;
        case Tag::Float: {
            vm::Str* s = call_.sandbox.fromF64(v[1].f);
            text = s != nullptr ? std::string(s->view()) : "";
            return s != nullptr;
        }
        case Tag::Str:
            text = std::string(static_cast<const vm::Str*>(v[1].p)->view());
            break;
        case Tag::Bool:
            text = v[1].u != 0 ? "true" : "false";
            break;
        case Tag::Null:
            text = "null";
            break;
        default:
            return fail("an argument is a list or a table, not a scalar");
        }
        return !numeric || fail("a number argument is not a number");
    }

    bool argument(bool emit, int depth) {
        if (depth > kMaxDepth) {
            return fail("arguments nested too deeply");
        }
        ++i_;
        skipSpace();
        std::string_view name = word();
        if (name.empty()) {
            return fail("expected an argument name");
        }
        const vm::Slot* value = emit ? lookup(name) : nullptr;
        if (emit && value == nullptr) {
            return false;
        }
        skipSpace();
        std::string text;
        if (i_ < s_.size() && s_[i_] == '}') {
            ++i_;
            if (emit && !scalarText(value, text, false)) {
                return false;
            }
            out += text;
            return true;
        }
        if (i_ >= s_.size() || s_[i_] != ',') {
            return fail("expected ',' or '}' after the argument name");
        }
        ++i_;
        skipSpace();
        std::string_view type = word();
        skipSpace();
        if (type == "number") {
            if (i_ >= s_.size() || s_[i_] != '}') {
                return fail("number styles are not supported");
            }
            ++i_;
            if (emit && !scalarText(value, text, true)) {
                return false;
            }
            out += text;
            return true;
        }
        if (type != "plural" && type != "select") {
            return fail(std::format("unsupported argument type '{}'", type));
        }
        return choice(type == "plural", value, emit, depth);
    }

    bool choice(bool plural, const vm::Slot* value, bool emit, int depth) {
        if (i_ >= s_.size() || s_[i_] != ',') {
            return fail("expected ',' and the cases");
        }
        ++i_;
        skipSpace();
        std::int64_t offset = 0;
        if (plural && s_.substr(i_).starts_with("offset:")) {
            i_ += 7;
            skipSpace();
            auto r = std::from_chars(s_.data() + i_, s_.data() + s_.size(), offset);
            if (r.ec != std::errc{}) {
                return fail("bad offset");
            }
            i_ = static_cast<std::size_t>(r.ptr - s_.data());
        }

        struct Case {
            std::string_view key;
            std::size_t start;
        };

        std::vector<Case> cases;
        while (true) {
            skipSpace();
            if (i_ >= s_.size()) {
                return fail("missing '}'");
            }
            if (s_[i_] == '}') {
                break;
            }
            std::size_t keyFrom = i_;
            if (plural && s_[i_] == '=') {
                ++i_;
                if (i_ < s_.size() && s_[i_] == '-') {
                    ++i_;
                }
            }
            word();
            std::string_view key = s_.substr(keyFrom, i_ - keyFrom);
            skipSpace();
            if (key.empty() || i_ >= s_.size() || s_[i_] != '{') {
                return fail("expected a case keyword and '{'");
            }
            ++i_;
            cases.push_back({key, i_});
            if (!message(true, plural, {}, false, depth)) {
                return false;
            }
            ++i_;
        }
        std::size_t end = ++i_;
        bool hasOther = false;
        for (const Case& c : cases) {
            hasOther = hasOther || c.key == "other";
        }
        if (!hasOther) {
            return fail("plural and select need an 'other' case");
        }
        if (!emit) {
            return true;
        }
        std::string hash;
        std::string_view want;
        std::string exact;
        if (plural) {
            auto tag = static_cast<Tag>(value[0].u);
            if (tag != Tag::Int && tag != Tag::Float) {
                return fail("a plural argument is not a number");
            }
            vm::Slot shifted[2] = {value[0], value[1]};
            if (tag == Tag::Int) {
                shifted[1].i = value[1].i - offset;
                exact = std::format("={}", value[1].i);
            } else {
                shifted[1].f = value[1].f - static_cast<double>(offset);
                double whole = value[1].f;
                if (whole == static_cast<double>(static_cast<std::int64_t>(whole)) && whole > -9.2e18 &&
                    whole < 9.2e18) {
                    exact = std::format("={}", static_cast<std::int64_t>(whole));
                }
            }
            if (!scalarText(shifted, hash, true)) {
                return false;
            }
            want = category(lang_, operandsOf(hash));
        } else if (!scalarText(value, exact, false)) {
            return false;
        }
        const Case* chosen = nullptr;
        for (const Case& c : cases) {
            if (plural ? c.key == exact : c.key == exact) {
                chosen = &c;
                break;
            }
        }
        for (const Case& c : cases) {
            if (chosen == nullptr && plural && c.key == want) {
                chosen = &c;
            }
        }
        for (const Case& c : cases) {
            if (chosen == nullptr && c.key == "other") {
                chosen = &c;
            }
        }
        i_ = chosen->start;
        if (!message(true, plural, hash, true, depth)) {
            return false;
        }
        i_ = end;
        return true;
    }

    vm::HostCall& call_;
    std::string_view s_;
    std::string lang_;
    std::size_t i_ = 0;
};

} // namespace

bool formatMessage(vm::HostCall& call, void*) {
    std::string_view text = strArg(call.args[0]);
    const auto* map = static_cast<const vm::Map*>(call.args[1].p);
    if (!call.charge(static_cast<std::int64_t>((text.size() + map->cap * 16) / 16 + 1))) {
        return false;
    }
    Formatter f(call, text, languageOf(strArg(call.args[2])));
    std::size_t stride = 1 + map->valueSlots;
    for (std::uint32_t k = 0; k < map->cap; ++k) {
        if (map->hashes[k] >= 2) {
            const vm::Slot* e = map->entries + static_cast<std::size_t>(k) * stride;
            f.args.emplace(static_cast<const vm::Str*>(e[0].p)->view(), e + 1);
        }
    }
    if (!f.run()) {
        return f.error.empty() ? false : errStr(call, "message: " + f.error);
    }
    return okStr(call, f.out);
}

bool pluralCategory(vm::HostCall& call, void*) {
    return putStr(call, category(languageOf(strArg(call.args[0])), operandsOf(std::to_string(call.args[1].i))),
                  &call.result);
}

} // namespace quartz::stdlib
