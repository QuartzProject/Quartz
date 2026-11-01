#include "Session.hpp"
#include "std/Std.hpp"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <format>

namespace quartz::dap {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s.remove_suffix(1);
    }
    return s;
}

bool identChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

std::optional<double> number(std::string_view s) {
    double v = 0;
    auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc{} || r.ptr != s.data() + s.size()) {
        return std::nullopt;
    }
    return v;
}

} // namespace

Json Session::stackTrace(const Json& args) {
    const vm::Program& p = *target_->program;
    auto start = static_cast<std::size_t>(std::max<std::int64_t>(args["startFrame"].integer(), 0));
    std::int64_t levels = args["levels"].integer();
    Json frames = Json::array();
    std::optional<Shown> shown = framesOf(args.has("threadId") ? args["threadId"].integer() : current_);
    std::size_t total = shown ? shown->count : 0;
    for (std::size_t i = start; i < total && (levels <= 0 || i < start + static_cast<std::size_t>(levels)); ++i) {
        const vm::FrameView& f = frames_[shown->begin + i];
        const vm::CompiledFunction& fn = p.functions[f.function];
        std::int64_t column = 1;
        for (std::size_t k = 0; i == 0 && k < p.probeSites.size(); ++k) {
            const vm::ProbeSite& site = p.probeSites[k];
            if (site.function == f.function && site.pc == f.pc && site.kind == vm::ProbeKind::Line &&
                site.column != 0) {
                column = site.column;
            }
        }
        Json frame = Json::object()
                         .set("id", static_cast<std::int64_t>(shown->begin + i + 1))
                         .set("name", fn.name)
                         .set("line", f.line)
                         .set("column", column);
        if (Json source = sourceOf(f.function); !source.isNull()) {
            frame.set("source", std::move(source));
        }
        frames.push(std::move(frame));
    }
    return Json::object().set("stackFrames", std::move(frames)).set("totalFrames", static_cast<std::int64_t>(total));
}

Json Session::sourceOf(std::uint32_t function) {
    const vm::CompiledFunction& fn = target_->program->functions[function];
    if (fn.file.empty()) {
        return Json();
    }
    Json source = Json::object().set("name", std::filesystem::path(fn.file).filename().string());
    if (fn.file.starts_with("<")) {
        source.set("presentationHint", "deemphasize");
        if (std::int64_t ref = librarySource(fn.file); ref != 0) {
            const auto& [name, text] = stdlib::sources()[static_cast<std::size_t>(ref - 1)];
            source.set("sourceReference", ref).set("origin", stdlib::packageOf(text));
        }
    } else {
        source.set("path", fn.file);
    }
    return source;
}

std::int64_t librarySource(std::string_view file) {
    const auto& all = stdlib::sources();
    for (std::size_t i = 0; i < all.size(); ++i) {
        if (all[i].first == file) {
            return static_cast<std::int64_t>(i + 1);
        }
    }
    return 0;
}

std::expected<Json, std::string> librarySourceText(const Json& args) {
    std::int64_t ref =
        args.has("sourceReference") ? args["sourceReference"].integer() : args["source"]["sourceReference"].integer();
    const auto& all = stdlib::sources();
    if (ref < 1 || static_cast<std::size_t>(ref) > all.size()) {
        return std::unexpected("no such source");
    }
    return Json::object()
        .set("content", all[static_cast<std::size_t>(ref - 1)].second)
        .set("mimeType", "text/x-quartz");
}

std::string_view librarySourceName(std::int64_t ref) {
    const auto& all = stdlib::sources();
    return ref >= 1 && static_cast<std::size_t>(ref) <= all.size() ? all[static_cast<std::size_t>(ref - 1)].first
                                                                   : std::string_view{};
}

std::int64_t Session::refFor(const Ref& ref) {
    refs_.push_back(ref);
    return static_cast<std::int64_t>(refs_.size());
}

Json Session::scopes(const Json& args) {
    std::int64_t frame = args["frameId"].integer();
    Json out = Json::array();
    if (frame >= 1 && static_cast<std::size_t>(frame) <= frames_.size()) {
        auto f = static_cast<std::size_t>(frame - 1);
        out.push(Json::object()
                     .set("name", "Arguments")
                     .set("presentationHint", "arguments")
                     .set("variablesReference", refFor({f, 0, {}}))
                     .set("expensive", false));
        out.push(Json::object()
                     .set("name", "Locals")
                     .set("presentationHint", "locals")
                     .set("variablesReference", refFor({f, 1, {}}))
                     .set("expensive", false));
    }
    return Json::object().set("scopes", std::move(out));
}

std::vector<std::pair<std::string, vm::ValueRef>> Session::visibleLocals(std::size_t frame, int scope) {
    std::vector<std::pair<std::string, vm::ValueRef>> out;
    if (frame >= frames_.size()) {
        return out;
    }
    const vm::FrameView& f = frames_[frame];
    const vm::CompiledFunction& fn = target_->program->functions[f.function];
    std::vector<const vm::CompiledFunction::Local*> seen;
    for (const vm::CompiledFunction::Local& l : fn.locals) {
        if (f.pc < l.from || f.pc >= l.to) {
            continue;
        }
        bool argument = l.from == 0 && l.slot < fn.paramSlots;
        if (scope >= 0 && argument != (scope == 0)) {
            continue;
        }
        // An inner or later declaration shadows an outer one of the same name.
        auto same = std::find_if(seen.begin(), seen.end(), [&](const auto* s) { return s->name == l.name; });
        if (same == seen.end()) {
            seen.push_back(&l);
        } else if ((*same)->from <= l.from) {
            *same = &l;
        }
    }
    std::stable_sort(seen.begin(), seen.end(), [](const auto* a, const auto* b) { return a->from < b->from; });
    for (const auto* l : seen) {
        out.emplace_back(l->name, vm::ValueRef{l->type, f.regs + l->slot, {}, false});
    }
    return out;
}

Json Session::variable(std::string name, const vm::ValueRef& v, std::size_t frame) {
    const vm::Program& p = *target_->program;
    std::size_t kids = vm::childCount(p, v);
    Json out = Json::object()
                   .set("name", std::move(name))
                   .set("value", vm::showValue(p, v))
                   .set("type", p.module->types.name(v.type))
                   .set("variablesReference", kids > 0 ? refFor({frame, -1, v}) : std::int64_t{0});
    if (kids > 0 && p.module->types.kind(v.type) == ir::TypeKind::List) {
        out.set("indexedVariables", static_cast<std::int64_t>(kids));
    }
    return out;
}

Json Session::variables(const Json& args) {
    std::int64_t id = args["variablesReference"].integer();
    Json out = Json::array();
    if (id < 1 || static_cast<std::size_t>(id) > refs_.size()) {
        return Json::object().set("variables", std::move(out));
    }
    Ref ref = refs_[static_cast<std::size_t>(id - 1)];
    if (ref.scope >= 0) {
        for (auto& [name, value] : visibleLocals(ref.frame, ref.scope)) {
            out.push(variable(name, value, ref.frame));
        }
    } else {
        auto start = static_cast<std::size_t>(std::max<std::int64_t>(args["start"].integer(), 0));
        std::int64_t count = args["count"].integer();
        std::size_t limit = count > 0 ? static_cast<std::size_t>(count) : 1000;
        for (vm::ValueChild& c : vm::childrenOf(*target_->program, ref.value, start, limit)) {
            out.push(variable(std::move(c.name), c.value, ref.frame));
        }
    }
    return Json::object().set("variables", std::move(out));
}

std::expected<vm::ValueRef, std::string> Session::lookup(std::size_t frame, std::string_view path) {
    path = trim(path);
    std::size_t i = 0;
    while (i < path.size() && identChar(path[i])) {
        ++i;
    }
    std::string_view name = path.substr(0, i);
    if (name.empty()) {
        return std::unexpected(std::format("cannot evaluate '{}': give a variable name", path));
    }
    std::optional<vm::ValueRef> v;
    for (auto& [local, value] : visibleLocals(frame, -1)) {
        if (local == name) {
            v = value;
        }
    }
    if (!v) {
        return std::unexpected(std::format("no variable '{}' here", name));
    }
    const vm::Program& p = *target_->program;
    while (i < path.size()) {
        std::string key;
        if (path[i] == '.') {
            std::size_t b = ++i;
            while (i < path.size() && identChar(path[i])) {
                ++i;
            }
            key = path.substr(b, i - b);
        } else if (path[i] == '[') {
            std::size_t close = path.find(']', i);
            if (close == std::string_view::npos) {
                return std::unexpected("missing ']'");
            }
            key = trim(path.substr(i + 1, close - i - 1));
            i = close + 1;
            if (p.module->types.kind(v->type) == ir::TypeKind::List) {
                key = "[" + key + "]";
            }
        } else {
            return std::unexpected(std::format("cannot evaluate '{}': only names, '.field' and '[index]'", path));
        }
        std::optional<vm::ValueRef> next;
        std::size_t n = vm::childCount(p, *v);
        std::size_t start = 0;
        if (std::uint64_t index = 0;
            key.size() > 2 && key.front() == '[' &&
            std::from_chars(key.data() + 1, key.data() + key.size() - 1, index).ec == std::errc{}) {
            start = static_cast<std::size_t>(std::min<std::uint64_t>(index, n));
        }
        for (vm::ValueChild& c : vm::childrenOf(p, *v, start, std::max<std::size_t>(n, 1))) {
            if (c.name == key) {
                next = c.value;
                break;
            }
        }
        if (!next) {
            return std::unexpected(std::format("no '{}' in {}", key, p.module->types.name(v->type)));
        }
        v = *next;
    }
    return *v;
}

std::expected<Json, std::string> Session::evaluate(const Json& args) {
    std::int64_t frame = args["frameId"].integer();
    std::size_t f = frame >= 1 ? static_cast<std::size_t>(frame - 1) : 0;
    auto v = lookup(f, args["expression"].str());
    if (!v) {
        return std::unexpected(v.error());
    }
    Json var = variable("", *v, f);
    return Json::object()
        .set("result", std::string(var["value"].str()))
        .set("type", std::string(var["type"].str()))
        .set("variablesReference", var["variablesReference"].integer());
}

bool Session::conditionHolds(const std::string& condition) {
    static constexpr std::string_view kOps[] = {"==", "!=", "<=", ">=", "<", ">"};
    std::string_view text = condition;
    std::string_view op;
    std::size_t at = std::string_view::npos;
    for (std::string_view o : kOps) {
        if (std::size_t k = text.find(o); k != std::string_view::npos && (at == std::string_view::npos || k < at)) {
            at = k;
            op = o;
        }
    }
    const vm::Program& p = *target_->program;
    auto lhs = lookup(0, op.empty() ? text : text.substr(0, at));
    if (!lhs) {
        output("console", std::format("breakpoint condition '{}': {}\n", condition, lhs.error()));
        return true;
    }
    std::string left = vm::showValue(p, *lhs, 4096);
    if (op.empty()) {
        return left == "true";
    }
    std::string_view rhsText = trim(text.substr(at + op.size()));
    std::string right(rhsText);
    if (!rhsText.empty() && identChar(rhsText.front()) && !(rhsText.front() >= '0' && rhsText.front() <= '9') &&
        rhsText != "true" && rhsText != "false" && rhsText != "null") {
        auto rhs = lookup(0, rhsText);
        if (!rhs) {
            output("console", std::format("breakpoint condition '{}': {}\n", condition, rhs.error()));
            return true;
        }
        right = vm::showValue(p, *rhs, 4096);
    }
    int cmp = 0;
    std::optional<double> a = number(left);
    std::optional<double> b = number(right);
    if (a && b) {
        cmp = *a < *b ? -1 : *a > *b ? 1 : 0;
    } else {
        cmp = left.compare(right) < 0 ? -1 : left.compare(right) > 0 ? 1 : 0;
    }
    if (op == "==") {
        return cmp == 0;
    }
    if (op == "!=") {
        return cmp != 0;
    }
    if (op == "<=") {
        return cmp <= 0;
    }
    if (op == ">=") {
        return cmp >= 0;
    }
    return op == "<" ? cmp < 0 : cmp > 0;
}

} // namespace quartz::dap
