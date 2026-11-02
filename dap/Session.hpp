#pragma once

#include "lsp/Json.hpp"
#include "lsp/Transport.hpp"
#include "vm/Inspect.hpp"
#include "vm/Sandbox.hpp"

#include <atomic>
#include <condition_variable>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace quartz::dap {

using lsp::Json;

/// What the tool that embeds the adapter compiled for a `launch`: a program with line probes and its plugins.
struct DebugTarget {
    std::shared_ptr<const vm::Program> program;
    std::string diagnostics;
    /// Starts the plugins the program imports from, with `bind` as their host, and links them into `client`.
    std::function<std::expected<std::shared_ptr<void>, std::string>(vm::Sandbox& client,
                                                                    const std::function<void(vm::Sandbox&)>& bind)>
        link;
};

/// Compiles the files or package folder at `path` with line and call probes (vm::Probes).
using DebugLoader = std::function<std::expected<DebugTarget, std::string>(const std::string& path)>;

/// The capabilities the adapter answers `initialize` with, including one exception filter per trap kind.
Json adapterCapabilities();
/// The exception filter of a trap kind (`divideByZero`, `panic`...).
std::string_view trapFilterOf(ir::TrapCode code);

/// The sourceReference of an embedded standard library file (`<std>/text.qz`), or 0.
std::int64_t librarySource(std::string_view file);
/// The name of the library file a sourceReference stands for, or empty.
std::string_view librarySourceName(std::int64_t ref);
/// The body of a `source` request: the text of a standard library file.
std::expected<Json, std::string> librarySourceText(const Json& args);

/// The trap kinds (bit per ir::TrapCode) of exception filter ids; `trap` stands for `all`.
std::uint32_t trapMaskOf(const Json& filters, std::uint32_t all);

/// Speaks the Debug Adapter Protocol over standard input and output until the client disconnects.
int runDebugAdapter(const DebugLoader& load);

class Session {
public:
    explicit Session(DebugLoader load) : load_(std::move(load)) {}

    int run();

    // Run.cpp: the program thread and its probe
    bool onProbe(vm::Sandbox& sandbox, const vm::ProbeEvent& event);
    void output(std::string_view category, std::string text);

private:
    enum class State : std::uint8_t { Configuring, Running, Stopped, Terminated };
    enum class Step : std::uint8_t { None, In, Over, Out };

    struct Breakpoint {
        std::uint32_t line = 0;
        std::string condition;
        std::int64_t id = 0;
        std::uint16_t column = 0;
    };

    struct LineRequest {
        std::uint32_t line = 0;
        std::uint32_t column = 0; // 0: the line's first statement
        std::string condition;
    };

    // A DAP thread: one task of a sandbox, or a plain call (a test that does not suspend).
    struct Thread {
        std::int64_t id = 0;
        const vm::Sandbox* sandbox = nullptr;
        std::optional<std::uint32_t> task;
        std::string name;
    };

    // Frames handed out for one thread while stopped: frames_[begin, begin + count).
    struct Shown {
        std::int64_t thread = 0;
        std::size_t begin = 0;
        std::size_t count = 0;
    };

    struct FunctionBreakpoint {
        std::string name;
        std::string condition;
        std::int64_t id = 0;
    };

    // A call to a function with a function breakpoint, waiting for its first line.
    struct Armed {
        std::int64_t thread = 0;
        std::size_t depth = 0;
        std::uint32_t function = 0;
    };

    struct Requested {
        std::string path;
        std::vector<LineRequest> lines;
    };

    // A variablesReference: a frame's scope, or a value with children.
    struct Ref {
        std::size_t frame = 0;
        int scope = -1; // 0 arguments, 1 locals; -1 for a value
        vm::ValueRef value;
    };

    // Session.cpp: protocol
    void handle(const Json& msg);
    void respond(const Json& request, Json body, bool success = true, std::string_view message = {});
    void event(std::string_view name, Json body);
    void send(Json msg);
    void launch(const Json& request);
    void setBreakpoints(const Json& request);
    void resolveBreakpoints(std::unique_lock<std::mutex>& lock);
    Json breakpointLocations(const Json& args);
    Json resolve(const Requested& req);
    void maybeStart();
    bool waitForStop(std::unique_lock<std::mutex>& lock);
    void resume(Step step, std::int64_t thread);

    // Variables.cpp: frames, scopes, values, evaluate and conditions
    Json stackTrace(const Json& args);
    Json scopes(const Json& args);
    Json variables(const Json& args);
    std::expected<Json, std::string> evaluate(const Json& args);
    std::expected<vm::ValueRef, std::string> lookup(std::size_t frame, std::string_view path);
    std::vector<std::pair<std::string, vm::ValueRef>> visibleLocals(std::size_t frame, int scope);
    Json variable(std::string name, const vm::ValueRef& v, std::size_t frame);
    std::int64_t refFor(const Ref& ref);
    bool conditionHolds(const std::string& condition);

    // Breakpoints.cpp: function breakpoints and trap filters
    void setExceptionBreakpoints(const Json& request);
    void setFunctionBreakpoints(const Json& request);
    Json resolveFunctions();
    [[nodiscard]] bool stopsOnTrap(ir::TrapCode code) const;
    std::optional<std::string> functionEntry(vm::Sandbox& sandbox, const vm::ProbeEvent& event, std::int64_t thread);
    Json sourceOf(std::uint32_t function);

    // Assign.cpp: setVariable and setExpression
    std::expected<void, std::string> assign(const vm::ValueRef& v, std::size_t frame, std::string_view text);
    std::expected<Json, std::string> setVariable(const Json& args);
    std::expected<Json, std::string> setExpression(const Json& args);

    // Threads.cpp: tasks as threads
    std::int64_t threadOf(const vm::Sandbox& sandbox, std::optional<std::uint32_t> task);
    void endThread(const vm::Sandbox& sandbox, std::optional<std::uint32_t> task);
    void endThreads(const vm::Sandbox& sandbox);
    Json threadList();
    std::optional<Shown> framesOf(std::int64_t thread);
    int runTasks(vm::Sandbox& sb, const std::vector<std::uint32_t>& functions);

    // Run.cpp
    void programThread();
    int runMain();
    int runTests();
    void bindHost(vm::Sandbox& sb);
    std::optional<std::string> prepare(vm::Sandbox& sb, std::shared_ptr<void>& keep);
    void stop(vm::Sandbox& sandbox, std::string reason, std::string text, std::unique_lock<std::mutex>& lock);
    bool breakpointAt(const vm::ProbeEvent& event);

    DebugLoader load_;
    lsp::Transport transport_;
    std::mutex writeMutex_;
    std::int64_t seq_ = 1;

    std::mutex mutex_;
    std::condition_variable cv_;
    State state_ = State::Configuring;
    bool launched_ = false;
    bool configured_ = false;
    bool started_ = false;
    std::atomic<bool> pause_{false};
    std::atomic<bool> terminate_{false};
    Step step_ = Step::None;
    std::size_t stepDepth_ = 0;
    std::uint32_t trapMask_ = ~0U;  // bit per ir::TrapCode
    std::uint32_t trapKinds_ = ~0U; // what the `trap` filter covers: launch's `trapFilters`, else every kind
    std::vector<FunctionBreakpoint> functionBreakpoints_;
    std::vector<std::vector<std::size_t>> entryBreakpoints_; // per function: indices into functionBreakpoints_
    std::vector<Armed> armed_;
    std::optional<DebugTarget> target_;
    std::string entry_ = "main";
    bool tests_ = false;
    std::string filter_;
    std::vector<std::string> tasks_;
    std::vector<Thread> threads_;
    std::int64_t nextThread_ = 1;
    std::int64_t current_ = 0;    // the thread of the stop
    std::int64_t stepThread_ = 0; // the thread a step waits for
    std::string spawning_;        // the name of the next thread a probe meets
    std::vector<Requested> requested_;
    std::vector<std::vector<Breakpoint>> fnBreakpoints_; // per function
    std::vector<std::string> fnFile_;                    // per function: canonical source path
    std::int64_t nextBreakpointId_ = 1;
    std::thread program_;
    int exitCode_ = 0;
    bool noDebug_ = false;
    bool disconnected_ = false;

    // Valid while stopped.
    vm::Sandbox* stopped_ = nullptr;
    std::vector<vm::FrameView> frames_;
    std::vector<Shown> shown_;
    std::string stopReason_;
    std::string stopText_;
    ir::TrapCode stopTrap_ = ir::TrapCode::Unreachable;
    std::vector<Ref> refs_;
};

} // namespace quartz::dap
