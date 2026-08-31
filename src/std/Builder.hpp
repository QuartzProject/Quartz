#pragma once

#include "std/Std.hpp"

#include <format>
#include <map>
#include <string>

namespace quartz::stdlib {

// Variant order of `org.quartzlang.data.Value`.
enum class Tag : std::uint64_t { Null, Bool, Int, Float, Str, List, Table };

/// The line each value of a parsed tree starts on, by (its table or list, its key or "[index]").
using LineMap = std::map<std::pair<const void*, std::string>, std::uint32_t>;

/// Counts lines up to a position, going on from the last position asked.
class LineCounter {
public:
    explicit LineCounter(std::string_view text) : s_(text) {}

    std::uint32_t at(std::size_t pos) {
        if (pos < pos_) {
            pos_ = 0;
            line_ = 1;
        }
        for (; pos_ < pos && pos_ < s_.size(); ++pos_) {
            line_ += s_[pos_] == '\n' ? 1 : 0;
        }
        return line_;
    }

private:
    std::string_view s_;
    std::size_t pos_ = 0;
    std::uint32_t line_ = 1;
};

/// Builds `org.quartzlang.data.Value` trees in the running sandbox's arena.
class Builder {
public:
    Builder(vm::Sandbox& sb, const DataTypes& types) : sb_(sb), t_(types) {}

    static void set(vm::Slot* v, Tag tag, vm::Slot payload) {
        v[0].u = static_cast<std::uint64_t>(tag);
        v[1] = payload;
    }
    static void setNull(vm::Slot* v) { set(v, Tag::Null, vm::Slot{}); }
    static void setBool(vm::Slot* v, bool b) {
        vm::Slot s{};
        s.u = b ? 1 : 0;
        set(v, Tag::Bool, s);
    }
    static void setInt(vm::Slot* v, std::int64_t i) {
        vm::Slot s{};
        s.i = i;
        set(v, Tag::Int, s);
    }
    static void setFloat(vm::Slot* v, double d) {
        vm::Slot s{};
        s.f = d;
        set(v, Tag::Float, s);
    }
    bool setStr(vm::Slot* v, std::string_view text) {
        vm::Str* s = sb_.copyStr(text);
        if (s == nullptr) {
            return false;
        }
        vm::Slot p{};
        p.p = s;
        set(v, Tag::Str, p);
        return true;
    }
    vm::List* newList(vm::Slot* v) {
        vm::List* l = sb_.newList(t_.value);
        if (l != nullptr) {
            vm::Slot p{};
            p.p = l;
            set(v, Tag::List, p);
        }
        return l;
    }
    vm::Map* newTable(vm::Slot* v) {
        vm::Map* m = sb_.newMap(t_.table);
        if (m != nullptr) {
            vm::Slot p{};
            p.p = m;
            set(v, Tag::Table, p);
        }
        return m;
    }

    bool push(vm::List* l, const vm::Slot* v) {
        if (lines != nullptr && line != 0) {
            (*lines)[{l, std::format("[{}]", l->len)}] = line;
        }
        return sb_.listPush(l, v);
    }
    bool put(vm::Map* m, std::string_view key, const vm::Slot* v) {
        if (lines != nullptr && line != 0) {
            (*lines)[{m, std::string(key)}] = line;
        }
        vm::Slot k{};
        k.p = sb_.copyStr(key);
        return k.p != nullptr && sb_.mapSet(m, &k, v);
    }
    vm::Slot* find(vm::Map* m, std::string_view key) {
        vm::Slot k{};
        k.p = sb_.copyStr(key);
        return k.p != nullptr ? sb_.mapFind(m, &k) : nullptr;
    }

    /// Writes `Ok(value)` or `Err(message)` of a `Result<Value, str>`.
    bool ok(vm::Slot* out, const vm::Slot* value) {
        out[0].u = 0;
        out[1] = value[0];
        out[2] = value[1];
        return true;
    }
    bool err(vm::Slot* out, std::string_view message) {
        out[0].u = 1;
        out[1].p = sb_.copyStr(message);
        return out[1].p != nullptr;
    }

    vm::Sandbox& sandbox() { return sb_; }

    /// When set, `put` and `push` record `line` (which the parser sets before them) into it.
    LineMap* lines = nullptr;
    std::uint32_t line = 0;

private:
    vm::Sandbox& sb_;
    const DataTypes& t_;
};

/// The parsers behind parseToml, parseJson and parseYaml, building into `b`; false with `error` set on bad text.
bool parseTomlText(Builder& b, std::string_view text, vm::Slot* root, std::string& error);
bool parseJsonText(Builder& b, std::string_view text, vm::Slot* root, std::string& error);
bool parseYamlText(Builder& b, std::string_view text, vm::Slot* root, std::string& error);

} // namespace quartz::stdlib
