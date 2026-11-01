#include "Session.hpp"

#include <algorithm>

namespace quartz::dap {

namespace {

struct TrapFilter {
    std::string_view filter;
    std::string_view label;
    ir::TrapCode code;
};

constexpr TrapFilter kTrapFilters[] = {
    {"panic", "Panics and failed checks", ir::TrapCode::Panic},
    {"divideByZero", "Division by zero", ir::TrapCode::DivideByZero},
    {"indexOutOfBounds", "Index out of bounds", ir::TrapCode::IndexOutOfBounds},
    {"nullUnwrap", "Null unwrapped", ir::TrapCode::NullUnwrap},
    {"outOfFuel", "Out of fuel", ir::TrapCode::OutOfFuel},
    {"outOfMemory", "Out of memory", ir::TrapCode::OutOfMemory},
    {"stackOverflow", "Stack overflow", ir::TrapCode::StackOverflow},
    {"invalidHandle", "Invalid handle", ir::TrapCode::InvalidHandle},
    {"hostError", "Host errors", ir::TrapCode::HostError},
    {"unreachable", "Unreachable code", ir::TrapCode::Unreachable},
};

// `Point.len<int>` is reached as `len`, `Point.len`, `pkg.Point.len` or with its type arguments.
bool functionNamed(const ir::Function& f, std::string_view name) {
    std::string_view full = f.name;
    std::string_view base = full.substr(0, full.find('<'));
    for (std::string_view own : {full, base}) {
        if (own == name || (!f.package.empty() && name.size() == f.package.size() + 1 + own.size() &&
                            name.starts_with(f.package) && name[f.package.size()] == '.' && name.ends_with(own))) {
            return true;
        }
    }
    std::size_t dot = base.rfind('.');
    return name.find('.') == std::string_view::npos && dot != std::string_view::npos && base.substr(dot + 1) == name;
}

} // namespace

Json adapterCapabilities() {
    Json filters = Json::array();
    filters.push(Json::object()
                     .set("filter", "trap")
                     .set("label", "All traps")
                     .set("description", "Stop where any trap (panic, failed check, out of fuel...) ends the program")
                     .set("default", true));
    for (const TrapFilter& f : kTrapFilters) {
        filters.push(Json::object().set("filter", f.filter).set("label", f.label).set("default", false));
    }
    return Json::object()
        .set("supportsConfigurationDoneRequest", true)
        .set("supportsConditionalBreakpoints", true)
        .set("supportsFunctionBreakpoints", true)
        .set("supportsEvaluateForHovers", true)
        .set("supportsSetVariable", true)
        .set("supportsSetExpression", true)
        .set("supportsExceptionInfoRequest", true)
        .set("supportsTerminateRequest", true)
        .set("exceptionBreakpointFilters", std::move(filters));
}

std::string_view trapFilterOf(ir::TrapCode code) {
    for (const TrapFilter& f : kTrapFilters) {
        if (f.code == code) {
            return f.filter;
        }
    }
    return "trap";
}

void Session::setExceptionBreakpoints(const Json& request) {
    std::uint32_t mask = 0;
    for (const Json& f : request["arguments"]["filters"].items()) {
        if (f.str() == "trap") {
            mask = ~0U;
        }
        for (const TrapFilter& t : kTrapFilters) {
            if (f.str() == t.filter) {
                mask |= 1U << static_cast<unsigned>(t.code);
            }
        }
    }
    {
        std::lock_guard lock(mutex_);
        trapMask_ = mask;
    }
    respond(request, Json::object().set("breakpoints", Json::array()));
}

bool Session::stopsOnTrap(ir::TrapCode code) const { return (trapMask_ >> static_cast<unsigned>(code) & 1U) != 0; }

void Session::setFunctionBreakpoints(const Json& request) {
    std::vector<FunctionBreakpoint> wanted;
    for (const Json& b : request["arguments"]["breakpoints"].items()) {
        wanted.push_back({std::string(b["name"].str()), std::string(b["condition"].str()), 0});
    }
    Json result;
    {
        std::lock_guard lock(mutex_);
        for (FunctionBreakpoint& b : wanted) {
            b.id = nextBreakpointId_++;
        }
        functionBreakpoints_ = std::move(wanted);
        result = resolveFunctions();
    }
    respond(request, Json::object().set("breakpoints", std::move(result)));
}

Json Session::resolveFunctions() {
    Json out = Json::array();
    const vm::Program* p = target_ ? target_->program.get() : nullptr;
    entryBreakpoints_.assign(p != nullptr ? p->functions.size() : 0, {});
    for (std::size_t i = 0; i < functionBreakpoints_.size(); ++i) {
        const FunctionBreakpoint& b = functionBreakpoints_[i];
        Json bp = Json::object().set("id", b.id);
        if (p == nullptr) {
            out.push(std::move(bp.set("verified", false).set("message", "not loaded yet")));
            continue;
        }
        std::optional<std::uint32_t> first;
        for (std::uint32_t f = 0; f < p->functions.size() && f < p->module->functions.size(); ++f) {
            if (!b.name.empty() && functionNamed(p->module->functions[f], b.name)) {
                entryBreakpoints_[f].push_back(i);
                first = first.value_or(f);
            }
        }
        bp.set("verified", first.has_value());
        if (!first) {
            out.push(std::move(bp.set("message", "no function '" + b.name + "'")));
            continue;
        }
        std::uint32_t line = 0;
        for (const vm::ProbeSite& site : p->probeSites) {
            if (site.function == *first && site.kind == vm::ProbeKind::Line && (line == 0 || site.line < line)) {
                line = site.line;
            }
        }
        if (line != 0) {
            bp.set("line", line);
        }
        if (Json source = sourceOf(*first); !source.isNull()) {
            bp.set("source", std::move(source));
        }
        out.push(std::move(bp));
    }
    return out;
}

// A function breakpoint arms on the call's entry probe and stops on the first line probe of that same frame.
std::optional<std::string> Session::functionEntry(vm::Sandbox& sandbox, const vm::ProbeEvent& event,
                                                  std::int64_t thread) {
    if (event.kind == vm::ProbeEvent::Kind::Enter) {
        std::erase_if(armed_, [&](const Armed& a) { return a.thread == thread && a.depth >= event.depth; });
        if (event.function < entryBreakpoints_.size() && !entryBreakpoints_[event.function].empty()) {
            armed_.push_back({thread, event.depth, event.function});
        }
        return std::nullopt;
    }
    auto it = std::find_if(armed_.begin(), armed_.end(), [&](const Armed& a) {
        return a.thread == thread && a.depth == event.depth && a.function == event.function;
    });
    if (it == armed_.end()) {
        return std::nullopt;
    }
    armed_.erase(it);
    if (event.function >= entryBreakpoints_.size()) {
        return std::nullopt;
    }
    // Conditions read the frame the probe stopped in.
    frames_ = sandbox.frames();
    bool hit = false;
    for (std::size_t i : entryBreakpoints_[event.function]) {
        const FunctionBreakpoint& b = functionBreakpoints_[i];
        hit = hit || b.condition.empty() || conditionHolds(b.condition);
    }
    frames_.clear();
    refs_.clear();
    return hit ? std::optional<std::string>("function breakpoint") : std::nullopt;
}

} // namespace quartz::dap
