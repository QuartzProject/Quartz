#pragma once

#include "ir/Ir.hpp"
#include "ir/Liveness.hpp"
#include "vm/Bytecode.hpp"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Module.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace quartz::compiler {

/// How one 8-byte interpreter slot is held in an SSA value.
enum class SlotKind : std::uint8_t { I1, I64, F64, F32, Ptr };

/// Runtime callbacks in the host, linked by name (see vm/NativeRuntime.cpp).
enum class Rt : std::uint8_t {
    Trap,
    Panic,
    IndexTrap,
    CallHost,
    Concat,
    Slice,
    CaseMap,
    FromI64,
    FromU64,
    FromF64,
    FromBool,
    FromChar,
    StrEq,
    StrCmp,
    StrContains,
    StrFind,
    StrStarts,
    StrEnds,
    ListNew,
    ListFilled,
    ListPush,
    ListSet,
    ListPop,
    ListClear,
    Join,
    MapNew,
    MapFind,
    MapSet,
    MapRemove,
    StateStore,
    ScopeIn,
    ScopeOut,
    Persist,
    Pow,
    Sin,
    Cos,
    TaskWait,
    TaskSave,
    TaskRestore,
    Closure,
    Count,
};

const char* runtimeName(Rt rt);

/// Builds one LLVM module holding a compiled entry and every function it can reach.
class UnitCodegen {
public:
    UnitCodegen(llvm::LLVMContext& ctx, llvm::Module& mod, const vm::Program& program);

    /// Emits the reachable functions and a C-callable wrapper named `entryName`; empty string on success.
    std::string build(std::uint32_t root, const std::string& entryName);

    [[nodiscard]] std::uint32_t loweredFunctions() const { return lowered_; }

    [[nodiscard]] std::uint32_t loweredInstructions() const { return instructions_; }

    // Shared by the function code generators.
    llvm::LLVMContext& C;
    llvm::Module& M;
    const vm::Program& P;
    const ir::Module& IR;
    const vm::Layouts& L;
    const ir::TypeTable& T;
    bool taskUnit = false; // rooted at a suspend fn: everything runs inside a task

    llvm::Type* i1;
    llvm::Type* i8;
    llvm::Type* i32;
    llvm::Type* i64;
    llvm::Type* f32;
    llvm::Type* f64;
    llvm::PointerType* ptr;

    llvm::MDNode* tbaaSlot;
    llvm::MDNode* tbaaList;
    llvm::MDNode* tbaaMap;
    llvm::MDNode* tbaaStr;
    llvm::MDNode* tbaaBytes;
    llvm::MDNode* tbaaFuel;
    llvm::MDNode* tbaaCtx;
    llvm::MDNode* tbaaClosure;
    llvm::MDNode* cold;

    const std::vector<SlotKind>& kinds(ir::TypeId t);
    llvm::Type* kindType(SlotKind k) const;
    llvm::FunctionCallee runtime(Rt rt);
    /// A string constant, linked by name (`qzk.<index>`) so cached objects hold no process addresses (D70).
    llvm::Constant* stringConstant(std::uint32_t index);
    /// The closure of a function that captures nothing, linked by name (`qzc.<function>`) like string constants.
    llvm::Constant* closureConstant(std::uint32_t function);
    /// Functions of this unit made into values whose type matches `fnType`: what a call through it can reach.
    [[nodiscard]] std::vector<std::uint32_t> closureTargets(ir::TypeId fnType) const;
    llvm::Function* function(std::uint32_t index);
    llvm::StructType* resultType(std::uint32_t index);
    /// True when a call from `caller` to `callee` can come back to `caller` (a recursion cycle).
    bool recursive(std::uint32_t caller, std::uint32_t callee) const;

private:
    void computeKinds(ir::TypeId t, std::vector<SlotKind>& out);
    void collectReachable(std::uint32_t root);
    void computeCycles();
    llvm::MDNode* scalarTbaa(llvm::MDNode* root, const char* name);
    void emitEntry(std::uint32_t root, const std::string& name);

    std::unordered_map<ir::TypeId, std::vector<SlotKind>> kinds_;
    std::vector<llvm::Function*> functions_;
    std::vector<llvm::StructType*> results_;
    std::vector<llvm::FunctionCallee> runtime_;
    std::vector<std::uint32_t> reachable_;
    std::vector<std::uint32_t> taken_; // functions the unit makes into values
    std::vector<std::vector<bool>> reaches_;
    std::uint32_t lowered_ = 0;
    std::uint32_t instructions_ = 0;
};

/// Lowers one validated IR function into its LLVM function.
class FunctionCodegen {
public:
    FunctionCodegen(UnitCodegen& unit, std::uint32_t index);

    /// Empty string on success, otherwise why the function cannot be compiled.
    std::string run();

private:
    using Values = std::vector<llvm::Value*>;

    struct CountedLoop {
        ir::Reg i = ir::kNoReg;
        ir::Reg step = ir::kNoReg;
        ir::Reg hi = ir::kNoReg; // the bound, or the list whose length is the bound
        bool listBound = false;
        std::size_t end = 0;
    };

    struct Frame {
        ir::Op op;
        llvm::BasicBlock* target; // loop header, or the block after a Block/If
        llvm::BasicBlock* elseBlock = nullptr;
        std::size_t pc = 0;
    };

    struct ResumePoint {
        std::uint32_t state;
        std::vector<ir::Reg> regs;
        ir::Reg waitDst;
        llvm::BasicBlock* target;
    };

    // FunctionCodegen.cpp
    void lower(const ir::Inst& in);
    void branch(std::uint32_t depth, llvm::Value* cond);
    void ret(const ir::Inst& in);
    void binary(const ir::Inst& in);
    void unary(const ir::Inst& in);
    void compare(const ir::Inst& in);
    void convert(const ir::Inst& in);
    void call(const ir::Inst& in);
    void callHost(const ir::Inst& in);
    // Closures.cpp
    void makeClosure(const ir::Inst& in);
    void callValue(const ir::Inst& in);
    /// Stack and depth checks, then fuel, as before every call in the interpreter.
    void enterCall(std::uint32_t callee, bool recursive, std::uint32_t state, llvm::BasicBlock* retry);
    void field(const ir::Inst& in);
    void nullable(const ir::Inst& in);
    void enumOp(const ir::Inst& in);
    void scope(const ir::Inst& in);
    llvm::Value* normalize(ir::TypeId t, llvm::Value* v);
    llvm::Value* divRem(const ir::Inst& in, llvm::Value* a, llvm::Value* b);

    // Tasks.cpp (D69)
    void wait(const ir::Inst& in);
    /// Saves the registers kept at `state` into the task and returns as suspended.
    void suspendAndReturn(std::uint32_t state, const std::vector<ir::Reg>& regs);
    void addResume(std::uint32_t state, std::vector<ir::Reg> regs, ir::Reg waitDst, llvm::BasicBlock* target);
    /// Fuel ran out in a suspend fn: yield to the host and continue at `resume` with `regs` restored.
    void yieldIfOutOfFuel(llvm::Value* fuel, std::uint32_t state, llvm::BasicBlock* resume);
    void overdrawOrTrap(llvm::Value* fuel);
    /// Entry of a suspend fn: a resuming task restores its frame record and jumps to the saved state.
    void emitDispatch(llvm::BasicBlock* body);
    void zeroRefRegisters();

    // Loops.cpp
    std::size_t matchingEnd(std::size_t loop) const;
    bool countedLoop(std::size_t loop, CountedLoop& out) const;
    /// True when the Block closed at `end` is never the target of a branch.
    bool fallsThroughOnly(std::size_t end) const;
    /// Emits the loop twice: without fuel checks when the remaining fuel covers every back edge, and with them.
    void versionLoop(std::size_t loop, const CountedLoop& c);

    // Intrinsics.cpp
    void intrinsic(const ir::Inst& in);
    llvm::Value* elementAt(llvm::Value* list, llvm::Value* index, std::size_t slots, bool packed);
    Values loadElement(llvm::Value* at, const std::vector<SlotKind>& k, bool packed);
    void storeElement(llvm::Value* at, const Values& v, const std::vector<SlotKind>& k, bool packed);
    void listGet(const ir::Inst& in);
    void listSet(const ir::Inst& in);
    void listPush(const ir::Inst& in);
    /// True when storing references into `list` needs no deep copy or promotion.
    llvm::Value* needsNoCopy(llvm::Value* list);
    void mapGet(const ir::Inst& in);
    void stringOp(const ir::Inst& in, ir::Intrinsic op);
    void mathOp(const ir::Inst& in, ir::Intrinsic op);
    llvm::Value* javaMinMax(llvm::Value* a, llvm::Value* b, bool isMin);

    // Support (FunctionSupport.cpp)
    const std::vector<SlotKind>& kindsOf(ir::Reg r);
    Values read(ir::Reg r);
    void write(ir::Reg r, const Values& v);

    llvm::Value* read1(ir::Reg r) { return read(r)[0]; }

    void write1(ir::Reg r, llvm::Value* v) { write(r, Values{v}); }

    llvm::Value* loadSlot(llvm::Value* base, std::uint64_t index, SlotKind k, llvm::MDNode* tbaa);
    void storeSlot(llvm::Value* base, std::uint64_t index, llvm::Value* v, SlotKind k, llvm::MDNode* tbaa);
    Values loadSlots(llvm::Value* base, std::uint64_t first, const std::vector<SlotKind>& k, llvm::MDNode* tbaa);
    void storeSlots(llvm::Value* base, std::uint64_t first, const Values& v, const std::vector<SlotKind>& k,
                    llvm::MDNode* tbaa);
    llvm::Value* toBits(llvm::Value* v, SlotKind k);
    llvm::Value* fromBits(llvm::Value* bits, SlotKind k);
    llvm::Value* zeroOf(SlotKind k);
    /// Scratch slots in the native frame used to pass values to the runtime.
    llvm::Value* scratch(std::uint32_t slots);
    llvm::Value* gep(llvm::Value* base, std::uint64_t byteOffset);
    llvm::Value* loadField(llvm::Type* t, llvm::Value* base, std::uint64_t byteOffset, llvm::MDNode* tbaa);
    llvm::LoadInst* tag(llvm::LoadInst* l, llvm::MDNode* tbaa);
    llvm::StoreInst* tag(llvm::StoreInst* s, llvm::MDNode* tbaa);
    /// Branches to a cold trap block when `failed` is true.
    void trapIf(llvm::Value* failed, ir::TrapCode code);
    /// For runtime callbacks that already recorded the trap.
    void failIf(llvm::Value* failed);
    void emitTrap(ir::TrapCode code);
    /// Charges one unit; in a suspend fn running out yields at `state` and resumes at `resume`.
    void chargeFuel(std::uint32_t state = 0, llvm::BasicBlock* resume = nullptr);
    void spillFuel();
    void reloadFuel();
    void deadBlock();

    llvm::Value* constI64(std::int64_t v) { return llvm::ConstantInt::get(U.i64, static_cast<std::uint64_t>(v)); }

    llvm::Value* constI32(std::uint32_t v) { return llvm::ConstantInt::get(U.i32, v); }

    llvm::Value* ctxField(std::uint64_t offset, llvm::Type* t);
    llvm::Value* listLen(llvm::Value* list);
    llvm::Value* boundsFail(llvm::Value* index, llvm::Value* len);

    UnitCodegen& U;
    const ir::Function& fn_;
    const vm::CompiledFunction& cf_;
    std::uint32_t index_;
    llvm::Function* F_;
    llvm::IRBuilder<> B_;
    llvm::IRBuilder<> entryB_;
    llvm::Value* base_ = nullptr;
    llvm::Value* ctx_ = nullptr;
    llvm::Value* depth_ = nullptr;
    llvm::BasicBlock* failBlock_ = nullptr;
    llvm::AllocaInst* scratch_ = nullptr;
    llvm::AllocaInst* fuel_ = nullptr;
    bool checkFuel_ = true;
    bool recordResume_ = false;
    std::size_t pc_ = 0;
    std::unordered_map<std::size_t, llvm::BasicBlock*> resume_; // checked loop copy: block after each host call
    std::uint32_t scratchSlots_ = 0;
    std::vector<std::vector<llvm::AllocaInst*>> regs_;
    std::vector<Frame> frames_;
    std::uint32_t line_ = 0;
    std::string error_;
    std::unique_ptr<ir::Liveness> live_; // suspend fns only
    std::vector<ResumePoint> resumePoints_;
};

} // namespace quartz::compiler
