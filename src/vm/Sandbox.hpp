#pragma once

#include "vm/Bytecode.hpp"
#include "vm/Heap.hpp"
#include "vm/Tier.hpp"

namespace quartz::stdlib {
struct DataTypes;
struct NativeCtx;
}

#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace quartz::vm {

struct Trap {
    ir::TrapCode code = ir::TrapCode::Unreachable;
    std::string message;

    struct Frame {
        std::string function;
        std::string file;
        std::uint32_t line = 0;
    };
    std::string function;
    std::uint32_t line = 0;
    std::vector<Frame> callers{}; // the calls that led here, innermost first
    std::string file{};

    [[nodiscard]] std::string describe() const;
};

struct Limits {
    std::size_t memoryBytes = 64u << 20;
    std::int64_t fuelPerCall = 100'000'000;
    std::uint32_t stackSlots = 1u << 16;
    std::uint32_t maxDepth = 4096;
    TierMode tier = TierMode::Default;
    std::size_t nativeStackBytes = 1u << 20; // native stack that recursive compiled code may use
    std::string nativeCacheDir; // compiled objects on disk (D70); empty reads QUARTZ_NATIVE_CACHE, which may be unset
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
    Slot result{};      // one-slot results
    std::string error;  // set it and return false to trap with HostError
    Slot* out = nullptr; // results wider than one slot (structs, Result) are written here directly

    Str* string(std::string_view text);
    /// Charges fuel for work done natively (spec 9.2); false when the budget is gone.
    bool charge(std::int64_t units);
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
    /// True when the task runs compiled code (spawned native, or moved there at a `wait`).
    [[nodiscard]] bool taskNative(std::uint32_t task) const;

    [[nodiscard]] const Program& program() const { return *program_; }

    /// Every state as JSON, with field names and versions: what `@persist` stores and what a reload carries over.
    std::expected<std::string, Trap> exportState();
    /// Loads states written by `exportState` of this or an older version of the program (spec 6.1 migration).
    std::expected<void, Trap> importState(std::string_view json);
    /// Builds a sandbox for `next` with the same host bindings and limits and this sandbox's state migrated
    /// into it. On failure this sandbox is untouched and keeps running (D23).
    std::expected<std::unique_ptr<Sandbox>, Trap> reload(std::shared_ptr<const Program> next);
    /// `@persist(per = Handle)` states of one handle as JSON (save on quit), and back (load on join).
    std::expected<std::string, Trap> exportHandle(std::uint64_t handle);
    std::expected<void, Trap> importHandle(std::uint64_t handle, std::string_view json);
    /// Frees the per-handle states of a handle that left; the next call with it starts from the defaults.
    void dropHandle(std::uint64_t handle);
    /// Binds this sandbox's imports `package.fn` (from `quartz interface`) to `provider`'s exported functions.
    /// A call runs in the provider, on its budget; scalars and str are copied both ways (spec 8.3).
    std::expected<void, std::string> link(std::string_view package, Sandbox& provider);
    [[nodiscard]] std::size_t memoryUsed() const { return quota_.used; }
    [[nodiscard]] TierMode tierMode() const { return tierMode_; }
    /// Changes the fuel each call and each task slice gets, so a host can keep a wall-clock budget as the tier changes.
    void setFuelPerCall(std::int64_t fuel) { limits_.fuelPerCall = fuel; }
    [[nodiscard]] std::int64_t fuelPerCall() const { return limits_.fuelPerCall; }
    /// The slots of the last call's result, for results wider than one slot (structs, tuples, Result).
    [[nodiscard]] std::span<const Slot> resultSlots(std::size_t count) const { return stack_.first(count); }
    /// Fuel left after the last call.
    [[nodiscard]] std::int64_t fuelLeft() const { return fuel_; }
    /// Outside a call: a string for the next call's arguments, valid until the call after it starts.
    Str* newStr(std::string_view text);
    /// An empty list of `listType` the host fills with `hostListPush`: an argument, a `resume` value or, inside a
    /// host function, its result. It lives as long as `newStr`'s strings.
    List* hostList(ir::TypeId listType);
    /// Appends one element (its slots, as many as the element type takes); false when the quota is exhausted.
    bool hostListPush(List* list, std::span<const Slot> element);
    /// The type a suspended task's `wait` produces, so the host can build a list of it.
    [[nodiscard]] ir::TypeId waitType(std::uint32_t task) const;

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
    Closure* newClosure(std::uint32_t function, std::uint32_t captures, std::uint32_t slots, const Slot* values);
    bool storeState(ir::TypeId type, const Slot* src, Slot* dst);
    /// Charges fuel for work done natively (spec 9.2); false, with an out of fuel trap, once the budget is gone.
    bool chargeFuel(std::int64_t units);
    void releaseState(ir::TypeId type, Slot* slots);

    bool scopeIn();
    void scopeOut();

    // Interp.cpp
    enum class Status : std::uint8_t { Done, Trapped, Suspended };
    bool run(std::uint32_t function, Slot* base);
    Status execute(const CompiledFunction* fn, const Ins* pc, Slot* R, Slot* base, std::size_t entryDepth);
    bool callHost(std::uint32_t import, const Slot* args, Slot* result);
    bool callHostC(std::uint32_t import, const Slot* args, Slot* result);

    Trap trap_;
    bool trapped_ = false;
    void addCaller(std::uint32_t function, std::uint32_t line);
    void traceCallers(std::size_t entryDepth);
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
        std::vector<std::unique_ptr<InvocationArena>> scopes; // regions of `scope`s open at the suspension
        std::uint16_t waitDst = 0xFFFF;
        ir::TypeId waitType = ir::kVoid;
        std::uint32_t root = 0;
        TaskOutcome::Kind kind = TaskOutcome::Kind::Wait; // of the last suspension
        NativeEntry native = nullptr; // set when the task runs compiled code (D69)
        std::vector<Slot> saved;       // native frame records, innermost first
    };
    std::expected<TaskOutcome, Trap> runTask(std::uint32_t id, const CompiledFunction* fn, const Ins* pc, Slot* R,
                                             std::size_t entryDepth);
    std::expected<TaskOutcome, Trap> runNativeTask(std::uint32_t id, Slot resumeValue);
    NativeEntry taskEntry(std::uint32_t function);
    bool migrateTask(Task& task, NativeEntry entry);
    enum class Copy : std::uint8_t { All, StateOnly, NewerThan };
    bool copyToArena(ir::TypeId type, const Slot* src, Slot* dst, Copy mode, std::uint32_t level = 0);
    bool promoteInto(ir::TypeId type, Slot* slot, std::uint32_t level);
    void resetRegions(InvocationArena* base);
    std::vector<std::unique_ptr<InvocationArena>> takeScopes();
    InvocationArena* regionFor(Owner owner, std::uint32_t level);
    bool enter(std::uint32_t function, Slot* base);
    bool runNative(NativeEntry entry, std::uint32_t function, Slot* base);
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
    std::uint32_t curLevel_ = 0;
    std::vector<InvocationArena*> regions_;                     // level 0 is the invocation or task arena
    std::vector<std::unique_ptr<InvocationArena>> scopeArenas_; // owners of levels 1..
    std::vector<std::unique_ptr<InvocationArena>> spare_;
    bool taskMode_ = false;
    // Set by the interpreter when a task suspends.
    const CompiledFunction* suspendFn_ = nullptr;
    const Ins* suspendPc_ = nullptr;
    Slot* suspendR_ = nullptr;
    std::uint16_t suspendDst_ = 0xFFFF;
    ir::TypeId suspendType_ = ir::kVoid;
    TaskOutcome::Kind suspendKind_ = TaskOutcome::Kind::Wait;
    Slot awaited_{};
    std::vector<Slot>* nativeSaved_ = nullptr; // save stack of the native task running now

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
    void bindNatives();
    Slot* instanceFor(std::size_t state, std::uint64_t handle);
    bool resolveStates(const CompiledFunction& fn, std::span<const Slot> args, std::vector<Slot*>& out);
    std::expected<void, Trap> importOne(std::size_t state, const void* saved, Slot* target);
    std::vector<std::unordered_map<std::uint64_t, Slot*>> perHandle_;
    std::vector<Slot*> resolved_;
    struct LinkTarget {
        Sandbox* provider;
        std::uint32_t function;
        const ir::HostImport* import;
        std::vector<ir::TypeId> providerParams; // the provider's own type ids, matched by shape at link time
        ir::TypeId providerResult;
    };
    std::vector<std::unique_ptr<LinkTarget>> links_;
    static bool linkedCall(HostCall& call, void* user);
    bool copyAcross(const Sandbox& from, ir::TypeId fromType, const Slot* src, ir::TypeId toType, Slot* dst);
    struct ImportCtx;
    bool importValue(ImportCtx& ctx, const void* node, ir::TypeId type, Slot* out, const std::string& path);
    void exportValue(std::string& out, ir::TypeId type, const Slot* v);
    std::unique_ptr<stdlib::DataTypes> dataTypes_;
    std::vector<std::unique_ptr<stdlib::NativeCtx>> nativeCtx_;
    TierMode tierMode_ = TierMode::Interpreter;
    std::string nativeCache_;

    friend struct HostCall;
    friend class Interpreter;
    friend struct NativeRuntimeAccess;
    friend struct Builtins;
};

} // namespace quartz::vm
