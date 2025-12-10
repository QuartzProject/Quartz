#pragma once

#include "std/Std.hpp"

namespace quartz::stdlib {

// Variant order of `std.data.Value`.
enum class Tag : std::uint64_t { Null, Bool, Int, Float, Str, List, Table };

/// Builds `std.data.Value` trees in the running sandbox's arena.
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
        vm::Str* s = sb_.newStr(text);
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
    bool push(vm::List* l, const vm::Slot* v) { return sb_.listPush(l, v); }
    bool put(vm::Map* m, std::string_view key, const vm::Slot* v) {
        vm::Slot k{};
        k.p = sb_.newStr(key);
        return k.p != nullptr && sb_.mapSet(m, &k, v);
    }
    vm::Slot* find(vm::Map* m, std::string_view key) {
        vm::Slot k{};
        k.p = sb_.newStr(key);
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
        out[1].p = sb_.newStr(message);
        return out[1].p != nullptr;
    }

    vm::Sandbox& sandbox() { return sb_; }

private:
    vm::Sandbox& sb_;
    const DataTypes& t_;
};

} // namespace quartz::stdlib
