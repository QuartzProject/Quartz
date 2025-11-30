#pragma once

#include "vm/Bytecode.hpp"
#include "vm/Heap.hpp"

#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace quartz::vm {

struct Trap {
    ir::TrapCode code = ir::TrapCode::Unreachable;
    std::string message;
    std::string function;
    std::uint32_t line = 0;

    [[nodiscard]] std::string describe() const;
};

struct Limits {
    std::size_t memoryBytes = 64u << 20;
    std::int64_t fuelPerCall = 100'000'000;
    std::uint32_t stackSlots = 1u << 16;
    std::uint32_t maxDepth = 4096;
};

class Sandbox;

/// What a task (a call to a `suspend fn`) did before handing control back to the host.
struct TaskOutcome {
    enum class Kind : std::uint8_t { Done, Wait, AtEnter, AtLeave, Yield };
    Kind kind = Kind::Done;
    std::uint32_t task = 0;
    Slot value{}; // Done: the result; Wait/AtEnter/AtLeave: the awaited handle
};

/// What a host function sees: its arguments, a slot for the result and helpers to build values.
struct HostCall {
    Sandbox& sandbox;
    const Slot* args;
    Slot result{};
    std::string error; // set it and return false to trap with HostError

    Str* string(std::string_view text);
};

using HostFn = bool (*)(HostCall& call, void* user);

class Sandbox {
public:
    Sandbox(std::shared_ptr<const Program> program, Limits limits = {});
    Sandbox(const Sandbox&) = delete;
    Sandbox& operator=(const Sandbox&) = delete;
    ~Sandbox();

    /// Binds a host import by its `.qzi` name (`trusted`, `Player.level`).
    bool bind(std::string_view importName, HostFn fn, void* user = nullptr);
    /// Checks every import is bound, creates the states and runs their initializers.
    std::expected<void, Trap> start();
    /// Calls an exported function; `state` parameters are supplied by the sandbox and skipped in `args`.
    std::expected<Slot, Trap> call(std::uint32_t function, std::span<const Slot> args);
    /// Starts a task; it runs until it finishes, waits or spends its fuel slice (then it yields, spec 9.2).
    std::expected<TaskOutcome, Trap> spawn(std::uint32_t function, std::span<const Slot> args);
    /// Resumes a suspended task; `value` is what `wait` produces (ignored for void awaitables).
    std::expected<TaskOutcome, Trap> resume(std::uint32_t task, Slot value = {});
    void cancel(std::uint32_t task);
    [[nodiscard]] std::size_t liveTasks() const;

    [[nodiscard]] const Program& program() const { return *program_; }
    [[nodiscard]] std::size_t memoryUsed() const { return quota_.used; }
    /// Outside a call: a string for the next call's arguments, valid until the call after it starts.
    Str* newStr(std::string_view text);

    // Runtime.cpp (used by the interpreter)
    Str* allocStr(std::size_t len);
    Str* concat(const Slot* parts, std::uint32_t count);
    Str* sliceStr(const Str* s, std::int64_t begin, std::int64_t end);
    Str* caseMap(const Str* s, bool upper);
    Str* fromI64(std::int64_t v);
    Str* fromU64(std::uint64_t v);
    Str* fromF64(double v);
    Str* fromChar(std::uint32_t cp);
    List* newList(ir::TypeId elem);
    List* filledList(std::int64_t n, const Slot* value, ir::TypeId elem);
    bool listPush(List* l, const Slot* value);
    bool listSet(List* l, std::int64_t index, const Slot* value);
    void listPop(List* l, Slot* dst);
    void listClear(List* l);
    Str* join(const List* l, const Str* sep);
    Map* newMap(ir::TypeId mapType);
    Slot* mapFind(const Map* m, const Slot* key) const;
    bool mapSet(Map* m, const Slot* key, const Slot* value);
    bool mapRemove(Map* m, const Slot* key);
    bool storeState(ir::TypeId type, const Slot* src, Slot* dst);
    void releaseState(ir::TypeId type, Slot* slots);

    // Interp.cpp
    enum class Status : std::uint8_t { Done, Trapped, Suspended };
    bool run(std::uint32_t function, Slot* base);
    Status execute(const CompiledFunction* fn, const Ins* pc, Slot* R, Slot* base, std::size_t entryDepth);
    bool callHost(std::uint32_t import, const Slot* args, Slot* result);

    Trap trap_;
    bool trapped_ = false;
    bool fail(ir::TrapCode code, std::string message = {});

private:
    struct SavedFrame {
        std::uint32_t function;
        std::uint32_t pc;
        std::uint32_t base;
        std::uint32_t dst;
    };
    struct Task {
        bool live = false;
        std::unique_ptr<InvocationArena> arena;
        std::vector<Slot> slots;
        std::vector<SavedFrame> frames; // callers, outermost first
        std::uint32_t function = 0;     // innermost frame
        std::uint32_t pc = 0;
        std::uint32_t base = 0;
        std::uint16_t waitDst = 0xFFFF;
        ir::TypeId waitType = ir::kVoid;
    };
    std::expected<TaskOutcome, Trap> runTask(std::uint32_t id, const CompiledFunction* fn, const Ins* pc, Slot* R,
                                             std::size_t entryDepth);
    bool copyToArena(ir::TypeId type, const Slot* src, Slot* dst, bool onlyState);
    bool copyIntoState(ir::TypeId type, const Slot* src, Slot* dst);
    bool growList(List* l, std::uint32_t need);
    bool growMap(Map* m);
    void* allocFor(Owner owner, std::size_t bytes);

    struct Binding {
        HostFn fn = nullptr;
        void* user = nullptr;
    };

    struct CallRecord {
        const CompiledFunction* fn;
        const Ins* pc;
        Slot* base;
        std::uint32_t dst;
    };
    std::vector<CallRecord> calls_;
    std::vector<Task> tasks_;
    std::vector<std::unique_ptr<InvocationArena>> retired_;
    InvocationArena* cur_ = nullptr; // where arena values go: the invocation arena or the running task's
    bool taskMode_ = false;
    // Set by the interpreter when a task suspends.
    const CompiledFunction* suspendFn_ = nullptr;
    const Ins* suspendPc_ = nullptr;
    Slot* suspendR_ = nullptr;
    std::uint16_t suspendDst_ = 0xFFFF;
    ir::TypeId suspendType_ = ir::kVoid;
    TaskOutcome::Kind suspendKind_ = TaskOutcome::Kind::Wait;
    Slot awaited_{};

    std::shared_ptr<const Program> program_;
    Limits limits_;
    Quota quota_;
    InvocationArena arena_;
    InvocationArena hostArenas_[2];
    int hostArena_ = 0;
    bool inCall_ = false;
    StateHeap heap_;
    std::unique_ptr<Slot[]> stackMem_;
    std::span<Slot> stack_;
    std::vector<Binding> bindings_;
    std::vector<Slot*> states_;
    bool started_ = false;
    std::int64_t fuel_ = 0;

    friend struct HostCall;
    friend class Interpreter;
};

} // namespace quartz::vm
