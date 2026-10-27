#include "std/Std.hpp"
#include "support/Utf8.hpp"
#include "vm/Sandbox.hpp"
#include "vm/StateJson.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <map>

namespace quartz::vm {

using ir::TypeId;
using ir::TypeKind;

using json::Node;

std::expected<std::string, Trap> Sandbox::exportState() {
    if (!started_) {
        return std::unexpected(Trap{ir::TrapCode::HostError, "sandbox not started", {}, 0});
    }
    const ir::Module& m = *program_->module;
    std::string out = "{\"format\":2,\"states\":{";
    bool firstMain = true;
    for (std::size_t i = 0; i < m.states.size(); ++i) {
        const ir::TypeInfo& info = m.types.info(m.states[i]);
        if (info.persistPer != 0) {
            continue;
        }
        out += firstMain ? "" : ",";
        firstMain = false;
        json::writeString(out, info.name);
        out += std::format(":{{\"version\":{},\"fields\":", info.version);
        Slot ref{};
        ref.p = states_[i];
        exportValue(out, m.states[i], &ref);
        out += '}';
    }
    out += "},\"handles\":{";
    bool firstState = true;
    for (std::size_t i = 0; i < m.states.size(); ++i) {
        const ir::TypeInfo& info = m.types.info(m.states[i]);
        if (info.persistPer == 0) {
            continue;
        }
        out += firstState ? "" : ",";
        firstState = false;
        json::writeString(out, info.name);
        out += ":{";
        std::vector<std::uint64_t> handles;
        for (const auto& [h, st] : perHandle_[i]) {
            handles.push_back(h);
        }
        std::sort(handles.begin(), handles.end());
        for (std::size_t k = 0; k < handles.size(); ++k) {
            out += std::format("{}\"{}\":{{\"version\":{},\"fields\":", k ? "," : "", handles[k], info.version);
            Slot ref{};
            ref.p = perHandle_[i][handles[k]];
            exportValue(out, m.states[i], &ref);
            out += '}';
        }
        out += '}';
    }
    out += "}}";
    return out;
}

std::expected<void, Trap> Sandbox::importOne(std::size_t i, ImportCtx& ctx, const Node& saved, Slot* target) {
    auto failWith = [](std::string msg) {
        return std::unexpected(Trap{ir::TrapCode::HostError, std::move(msg), {}, 0});
    };
    const json::Doc& doc = *ctx.doc;
    const ir::Module& m = *program_->module;
    const ir::TypeInfo& info = m.types.info(m.states[i]);
    const Node* versionNode = doc.get(saved, "version");
    const Node* fields = doc.get(saved, "fields");
    std::uint64_t oldVersion = 0;
    if (versionNode != nullptr && versionNode->kind == Node::Kind::Num) {
        std::from_chars(versionNode->text.data(), versionNode->text.data() + versionNode->text.size(), oldVersion);
    }
    if (fields == nullptr || fields->kind != Node::Kind::Obj) {
        return failWith(std::format("state {} is malformed", info.name));
    }
    if (oldVersion > info.version) {
        return failWith(std::format("state {} was saved by version {}, newer than {}", info.name, oldVersion,
                                    info.version));
    }
    std::uint32_t migrate = program_->findFunction("migrate" + info.name);
    for (std::uint32_t f = 0; f < info.fields.size(); ++f) {
        const ir::Field& field = info.fields[f];
        const Node* value = doc.get(*fields, field.name);
        if (value == nullptr && !field.renamedFrom.empty()) {
            value = doc.get(*fields, field.renamedFrom);
        }
        if (value == nullptr) {
            continue; // added field: keeps its default
        }
        std::vector<Slot> tmp(std::max<std::uint32_t>(program_->layouts->slots(field.type), 1));
        ctx.error.clear();
        bool ok = importValue(ctx, value, field.type, tmp.data(), info.name + "." + field.name);
        if (!ok && trapped_) {
            return std::unexpected(trap_);
        }
        if (!ok) {
            if (migrate == ~0U) {
                return failWith(std::format("cannot migrate {}; bump @version and add migrate{}(old: Value, data: {})",
                                            ctx.error, info.name, info.name));
            }
            continue;
        }
        if (!storeState(field.type, tmp.data(), target + program_->layouts->fieldOffset(m.states[i], f))) {
            return std::unexpected(trap_);
        }
    }
    if (migrate == ~0U) {
        return {};
    }
    // migrate<State>(old: Value, data: State) sees the saved fields as an org.quartzlang.data value.
    std::string err;
    Slot value[2];
    if (dataTypes_->value == 0 ||
        !stdlib::jsonToValue(*this, *dataTypes_, std::string(doc.source(*fields)), value, err)) {
        return failWith("cannot hand the old state to migrate" + info.name + ": " + err);
    }
    const CompiledFunction& fn = program_->functions[migrate];
    if (fn.params.size() != 2 || fn.params[1] != m.states[i] || program_->layouts->slots(fn.params[0]) != 2) {
        return failWith(std::format("migrate{} must take (old: Value, data: {})", info.name, info.name));
    }
    stack_[0] = value[0];
    stack_[1] = value[1];
    stack_[2].p = target;
    fuel_ = limits_.fuelPerCall;
    if (!run(migrate, stack_.data())) {
        return std::unexpected(trap_);
    }
    return {};
}

std::expected<void, Trap> Sandbox::importState(std::string_view json) {
    auto failWith = [](std::string msg) {
        return std::unexpected(Trap{ir::TrapCode::HostError, std::move(msg), {}, 0});
    };
    if (!started_) {
        return failWith("sandbox not started");
    }
    json::Doc doc;
    const Node* states = doc.parse(json) ? doc.get(doc.root(), "states") : nullptr;
    if (states == nullptr) {
        return failWith("state document is not valid");
    }
    ImportCtx ctx;
    ctx.doc = &doc;
    const Node* format = doc.get(doc.root(), "format");
    ctx.boxesAsValues = format != nullptr && format->kind == Node::Kind::Num && format->text != "1";
    const ir::Module& m = *program_->module;
    resetRegions(&arena_);
    arena_.reset();
    inCall_ = true;
    trapped_ = false;
    std::expected<void, Trap> result;
    const Node* handles = doc.get(doc.root(), "handles");
    for (std::size_t i = 0; i < m.states.size() && result; ++i) {
        const ir::TypeInfo& info = m.types.info(m.states[i]);
        if (info.persistPer == 0) {
            if (const Node* saved = doc.get(*states, info.name)) {
                result = importOne(i, ctx, *saved, states_[i]);
            }
            continue;
        }
        const Node* perState = handles != nullptr ? doc.get(*handles, info.name) : nullptr;
        for (std::size_t k = 0; perState != nullptr && k < perState->count && result; ++k) {
            std::uint64_t handle = 0;
            const std::string& key = doc.key(*perState, k);
            if (std::from_chars(key.data(), key.data() + key.size(), handle).ptr != key.data() + key.size()) {
                result = failWith("bad handle key in the state document");
                break;
            }
            Slot* target = instanceFor(i, handle);
            result = target != nullptr ? importOne(i, ctx, doc.child(*perState, k), target) : std::unexpected(trap_);
        }
    }
    inCall_ = false;
    heap_.flush();
    return result;
}

std::expected<std::string, Trap> Sandbox::exportHandle(std::uint64_t handle) {
    const ir::Module& m = *program_->module;
    std::string out = "{\"format\":2,\"states\":{},\"handles\":{";
    bool first = true;
    for (std::size_t i = 0; i < m.states.size(); ++i) {
        const ir::TypeInfo& info = m.types.info(m.states[i]);
        auto it = perHandle_.size() > i ? perHandle_[i].find(handle) : perHandle_[0].end();
        if (info.persistPer == 0 || it == perHandle_[i].end()) {
            continue;
        }
        out += first ? "" : ",";
        first = false;
        json::writeString(out, info.name);
        out += std::format(":{{\"{}\":{{\"version\":{},\"fields\":", handle, info.version);
        Slot ref{};
        ref.p = it->second;
        exportValue(out, m.states[i], &ref);
        out += "}}";
    }
    out += "}}";
    return out;
}

std::expected<void, Trap> Sandbox::importHandle(std::uint64_t handle, std::string_view json) {
    json::Doc doc;
    const Node* handles = doc.parse(json) ? doc.get(doc.root(), "handles") : nullptr;
    if (handles == nullptr || handles->kind != Node::Kind::Obj) {
        return std::unexpected(Trap{ir::TrapCode::HostError, "handle state document is not valid", {}, 0});
    }
    // Saved under whatever handle value the player had then; restore it under the current one.
    const Node* format = doc.get(doc.root(), "format");
    std::string rebuilt = std::format("{{\"format\":{},\"states\":{{}},\"handles\":{{",
                                      format != nullptr && format->kind == Node::Kind::Num ? format->text : "1");
    bool first = true;
    for (std::size_t i = 0; i < handles->count; ++i) {
        const Node& perState = doc.child(*handles, i);
        if (perState.kind != Node::Kind::Obj || perState.count == 0) {
            continue;
        }
        rebuilt += first ? "" : ",";
        first = false;
        json::writeString(rebuilt, doc.key(*handles, i));
        rebuilt += std::format(":{{\"{}\":", handle);
        rebuilt += doc.source(doc.child(perState, 0));
        rebuilt += "}";
    }
    rebuilt += "}}";
    return importState(rebuilt);
}

std::expected<std::unique_ptr<Sandbox>, Trap> Sandbox::reload(std::shared_ptr<const Program> next) {
    auto fresh = std::make_unique<Sandbox>(std::move(next), limits_);
    const auto& oldImports = program_->module->imports;
    for (std::size_t i = 0; i < oldImports.size(); ++i) {
        if (bindings_[i].fn != nullptr && !oldImports[i].name.starts_with("org.quartzlang.")) {
            fresh->bind(oldImports[i].name, bindings_[i].fn, bindings_[i].user);
        }
    }
    if (auto s = fresh->start(); !s) {
        return std::unexpected(s.error());
    }
    std::uint32_t unload = program_->findFunction("onUnload");
    if (unload != ~0U && program_->functions[unload].params.empty()) {
        if (auto r = call(unload, {}); !r) {
            return std::unexpected(r.error());
        }
    }
    auto saved = exportState();
    if (!saved) {
        return std::unexpected(saved.error());
    }
    if (auto r = fresh->importState(*saved); !r) {
        return std::unexpected(r.error());
    }
    return fresh;
}

} // namespace quartz::vm
