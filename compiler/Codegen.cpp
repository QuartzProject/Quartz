#include "Codegen.hpp"

#include "vm/NativeAbi.hpp"

#include <llvm/IR/Attributes.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/ModRef.h>
#include <llvm/Support/raw_ostream.h>

namespace quartz::compiler {

using ir::TypeKind;

namespace {

struct RuntimeSig {
    const char* name;
    bool pure; // reads memory only
    bool cold;
};

constexpr RuntimeSig kRuntime[] = {
    {"qz_trap", false, true},       {"qz_panic", false, true},        {"qz_index_trap", false, true},
    {"qz_call_host", false, false}, {"qz_concat", false, false},      {"qz_slice", false, false},
    {"qz_case", false, false},      {"qz_from_i64", false, false},    {"qz_from_u64", false, false},
    {"qz_from_f64", false, false},  {"qz_from_bool", false, false},   {"qz_from_char", false, false},
    {"qz_str_eq", true, false},     {"qz_str_cmp", true, false},      {"qz_str_contains", true, false},
    {"qz_str_find", true, false},   {"qz_str_starts", true, false},   {"qz_str_ends", true, false},
    {"qz_list_new", false, false},  {"qz_list_filled", false, false}, {"qz_list_push", false, false},
    {"qz_list_set", false, false},  {"qz_list_pop", false, false},    {"qz_list_clear", false, false},
    {"qz_join", false, false},      {"qz_map_new", false, false},     {"qz_map_find", false, false},
    {"qz_map_set", false, false},   {"qz_map_remove", false, false},  {"qz_state_store", false, false},
    {"qz_pow", false, false},       {"qz_sin", false, false},         {"qz_cos", false, false},
};
static_assert(std::size(kRuntime) == static_cast<std::size_t>(Rt::Count));

} // namespace

const char* runtimeName(Rt rt) { return kRuntime[static_cast<std::size_t>(rt)].name; }

UnitCodegen::UnitCodegen(llvm::LLVMContext& ctx, llvm::Module& mod, const vm::Program& program)
    : C(ctx), M(mod), P(program), IR(*program.module), L(*program.layouts), T(program.module->types),
      i1(llvm::Type::getInt1Ty(ctx)), i8(llvm::Type::getInt8Ty(ctx)), i32(llvm::Type::getInt32Ty(ctx)),
      i64(llvm::Type::getInt64Ty(ctx)), f32(llvm::Type::getFloatTy(ctx)), f64(llvm::Type::getDoubleTy(ctx)),
      ptr(llvm::PointerType::getUnqual(ctx)) {
    llvm::MDBuilder md(C);
    llvm::MDNode* root = md.createTBAARoot("quartz");
    tbaaSlot = scalarTbaa(root, "slot");
    tbaaList = scalarTbaa(root, "list");
    tbaaMap = scalarTbaa(root, "map");
    tbaaStr = scalarTbaa(root, "str");
    tbaaBytes = scalarTbaa(root, "bytes");
    tbaaFuel = scalarTbaa(root, "fuel");
    tbaaCtx = scalarTbaa(root, "ctx");
    cold = md.createBranchWeights(1, 1u << 20);
    functions_.assign(IR.functions.size(), nullptr);
    results_.assign(IR.functions.size(), nullptr);
    runtime_.resize(static_cast<std::size_t>(Rt::Count));
}

llvm::MDNode* UnitCodegen::scalarTbaa(llvm::MDNode* root, const char* name) {
    llvm::MDBuilder md(C);
    llvm::MDNode* scalar = md.createTBAAScalarTypeNode(name, root);
    return md.createTBAAStructTagNode(scalar, scalar, 0);
}

void UnitCodegen::computeKinds(ir::TypeId t, std::vector<SlotKind>& out) {
    const ir::TypeInfo& info = T.info(t);
    switch (info.kind) {
    case TypeKind::Void:
    case TypeKind::Never:
    case TypeKind::Error:
    case TypeKind::Null:
        return;
    case TypeKind::Bool:
        out.push_back(SlotKind::I1);
        return;
    case TypeKind::F32:
        out.push_back(SlotKind::F32);
        return;
    case TypeKind::F64:
        out.push_back(SlotKind::F64);
        return;
    case TypeKind::Str:
    case TypeKind::Bytes:
    case TypeKind::List:
    case TypeKind::Map:
    case TypeKind::Fn:
    case TypeKind::State:
        out.push_back(SlotKind::Ptr);
        return;
    case TypeKind::Nullable:
        if (!L.nullableIsPointer(t)) {
            out.push_back(SlotKind::I1);
        }
        computeKinds(info.elem, out);
        return;
    case TypeKind::Struct:
        for (const ir::Field& f : info.fields) {
            computeKinds(f.type, out);
        }
        return;
    case TypeKind::Tuple:
        for (ir::TypeId e : info.elems) {
            computeKinds(e, out);
        }
        return;
    case TypeKind::Enum:
        out.insert(out.end(), L.slots(t), SlotKind::I64);
        return;
    default:
        out.push_back(SlotKind::I64);
        return; // integers, char, handles
    }
}

const std::vector<SlotKind>& UnitCodegen::kinds(ir::TypeId t) {
    auto it = kinds_.find(t);
    if (it != kinds_.end()) {
        return it->second;
    }
    std::vector<SlotKind> k;
    computeKinds(t, k);
    return kinds_.emplace(t, std::move(k)).first->second;
}

llvm::Type* UnitCodegen::kindType(SlotKind k) const {
    switch (k) {
    case SlotKind::I1:
        return i1;
    case SlotKind::I64:
        return i64;
    case SlotKind::F64:
        return f64;
    case SlotKind::F32:
        return f32;
    case SlotKind::Ptr:
        return ptr;
    }
    return i64;
}

llvm::FunctionCallee UnitCodegen::runtime(Rt rt) {
    auto i = static_cast<std::size_t>(rt);
    if (runtime_[i].getCallee() != nullptr) {
        return runtime_[i];
    }
    llvm::Type* v = llvm::Type::getVoidTy(C);
    llvm::FunctionType* ty = nullptr;
    switch (rt) {
    case Rt::Trap:
        ty = llvm::FunctionType::get(v, {ptr, i32, i32, i32}, false);
        break;
    case Rt::Panic:
        ty = llvm::FunctionType::get(v, {ptr, ptr, i32, i32}, false);
        break;
    case Rt::IndexTrap:
        ty = llvm::FunctionType::get(v, {ptr, i64, i64, i32, i32}, false);
        break;
    case Rt::CallHost:
        ty = llvm::FunctionType::get(i32, {ptr, i32, ptr, ptr}, false);
        break;
    case Rt::Concat:
        ty = llvm::FunctionType::get(ptr, {ptr, ptr, i32}, false);
        break;
    case Rt::Slice:
        ty = llvm::FunctionType::get(ptr, {ptr, ptr, i64, i64}, false);
        break;
    case Rt::CaseMap:
        ty = llvm::FunctionType::get(ptr, {ptr, ptr, i32}, false);
        break;
    case Rt::FromI64:
    case Rt::FromU64:
    case Rt::FromChar:
        ty = llvm::FunctionType::get(ptr, {ptr, i64}, false);
        break;
    case Rt::FromF64:
        ty = llvm::FunctionType::get(ptr, {ptr, f64}, false);
        break;
    case Rt::FromBool:
        ty = llvm::FunctionType::get(ptr, {ptr, i32}, false);
        break;
    case Rt::StrEq:
    case Rt::StrCmp:
    case Rt::StrContains:
    case Rt::StrStarts:
    case Rt::StrEnds:
        ty = llvm::FunctionType::get(i32, {ptr, ptr}, false);
        break;
    case Rt::StrFind:
        ty = llvm::FunctionType::get(i64, {ptr, ptr}, false);
        break;
    case Rt::ListNew:
    case Rt::MapNew:
        ty = llvm::FunctionType::get(ptr, {ptr, i32}, false);
        break;
    case Rt::ListFilled:
        ty = llvm::FunctionType::get(ptr, {ptr, i64, ptr, i32}, false);
        break;
    case Rt::ListPush:
        ty = llvm::FunctionType::get(i32, {ptr, ptr, ptr}, false);
        break;
    case Rt::ListSet:
        ty = llvm::FunctionType::get(i32, {ptr, ptr, i64, ptr}, false);
        break;
    case Rt::ListPop:
        ty = llvm::FunctionType::get(v, {ptr, ptr, ptr}, false);
        break;
    case Rt::ListClear:
        ty = llvm::FunctionType::get(v, {ptr, ptr}, false);
        break;
    case Rt::Join:
        ty = llvm::FunctionType::get(ptr, {ptr, ptr, ptr}, false);
        break;
    case Rt::MapFind:
        ty = llvm::FunctionType::get(ptr, {ptr, ptr, ptr}, false);
        break;
    case Rt::MapSet:
        ty = llvm::FunctionType::get(i32, {ptr, ptr, ptr, ptr}, false);
        break;
    case Rt::MapRemove:
        ty = llvm::FunctionType::get(i32, {ptr, ptr, ptr}, false);
        break;
    case Rt::StateStore:
        ty = llvm::FunctionType::get(i32, {ptr, i32, ptr, ptr}, false);
        break;
    case Rt::Pow:
        ty = llvm::FunctionType::get(f64, {f64, f64}, false);
        break;
    case Rt::Sin:
    case Rt::Cos:
        ty = llvm::FunctionType::get(f64, {f64}, false);
        break;
    case Rt::Count:
        break;
    }
    llvm::FunctionCallee callee = M.getOrInsertFunction(kRuntime[i].name, ty);
    auto* f = llvm::cast<llvm::Function>(callee.getCallee());
    f->addFnAttr(llvm::Attribute::NoUnwind);
    if (kRuntime[i].pure) {
        f->setMemoryEffects(llvm::MemoryEffects::readOnly());
        f->addFnAttr(llvm::Attribute::WillReturn);
    }
    if (rt == Rt::Pow || rt == Rt::Sin || rt == Rt::Cos) {
        f->setMemoryEffects(llvm::MemoryEffects::none());
        f->addFnAttr(llvm::Attribute::WillReturn);
    }
    if (kRuntime[i].cold) {
        f->addFnAttr(llvm::Attribute::Cold);
        f->addFnAttr(llvm::Attribute::NoInline);
    }
    runtime_[i] = callee;
    return callee;
}

llvm::StructType* UnitCodegen::resultType(std::uint32_t index) {
    if (results_[index] == nullptr) {
        std::vector<llvm::Type*> fields{i1};
        for (SlotKind k : kinds(IR.functions[index].result)) {
            fields.push_back(kindType(k));
        }
        results_[index] = llvm::StructType::get(C, fields);
    }
    return results_[index];
}

llvm::Function* UnitCodegen::function(std::uint32_t index) {
    if (functions_[index] != nullptr) {
        return functions_[index];
    }
    const ir::Function& fn = IR.functions[index];
    std::vector<llvm::Type*> params{ptr, ptr, i32};
    for (std::uint32_t p = 0; p < fn.paramCount; ++p) {
        for (SlotKind k : kinds(fn.regs[p])) {
            params.push_back(kindType(k));
        }
    }
    auto* ty = llvm::FunctionType::get(resultType(index), params, false);
    auto* f =
        llvm::Function::Create(ty, llvm::GlobalValue::InternalLinkage, "q" + std::to_string(index) + "." + fn.name, M);
    f->setCallingConv(llvm::CallingConv::Fast);
    f->addFnAttr(llvm::Attribute::NoUnwind);
    f->addParamAttr(0, llvm::Attribute::NoAlias);
    f->addParamAttr(1, llvm::Attribute::NoAlias);
    functions_[index] = f;
    return f;
}

void UnitCodegen::collectReachable(std::uint32_t root) {
    std::vector<bool> seen(IR.functions.size(), false);
    std::vector<std::uint32_t> work{root};
    seen[root] = true;
    while (!work.empty()) {
        std::uint32_t f = work.back();
        work.pop_back();
        reachable_.push_back(f);
        for (const ir::Inst& in : IR.functions[f].code) {
            if (in.op == ir::Op::Call && in.imm < IR.functions.size() && !seen[in.imm]) {
                seen[in.imm] = true;
                work.push_back(in.imm);
            }
        }
    }
}

void UnitCodegen::computeCycles() {
    std::size_t n = IR.functions.size();
    reaches_.assign(n, {});
    for (std::uint32_t f : reachable_) {
        std::vector<bool>& r = reaches_[f];
        r.assign(n, false);
        std::vector<std::uint32_t> work{f};
        while (!work.empty()) {
            std::uint32_t g = work.back();
            work.pop_back();
            for (const ir::Inst& in : IR.functions[g].code) {
                if (in.op == ir::Op::Call && in.imm < n && !r[in.imm]) {
                    r[in.imm] = true;
                    work.push_back(in.imm);
                }
            }
        }
    }
}

bool UnitCodegen::recursive(std::uint32_t caller, std::uint32_t callee) const {
    return !reaches_[callee].empty() && reaches_[callee][caller];
}

void UnitCodegen::emitEntry(std::uint32_t root, const std::string& name) {
    auto* ty = llvm::FunctionType::get(i32, {ptr, ptr}, false);
    auto* f = llvm::Function::Create(ty, llvm::GlobalValue::ExternalLinkage, name, M);
    f->addFnAttr(llvm::Attribute::NoUnwind);
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(C, "entry", f));
    llvm::Value* frame = f->getArg(0);
    std::vector<llvm::Value*> args{frame, f->getArg(1), llvm::ConstantInt::get(i32, 0)};
    const ir::Function& fn = IR.functions[root];
    std::uint64_t slot = 0;
    for (std::uint32_t p = 0; p < fn.paramCount; ++p) {
        for (SlotKind k : kinds(fn.regs[p])) {
            llvm::Value* at = b.CreateConstInBoundsGEP1_64(i64, frame, slot++);
            llvm::Value* v = nullptr;
            if (k == SlotKind::I1) {
                v = b.CreateICmpNE(b.CreateLoad(i64, at), llvm::ConstantInt::get(i64, 0));
            } else {
                v = b.CreateLoad(kindType(k), at);
            }
            args.push_back(v);
        }
    }
    llvm::CallInst* r = b.CreateCall(function(root), args);
    r->setCallingConv(llvm::CallingConv::Fast);
    llvm::BasicBlock* ok = llvm::BasicBlock::Create(C, "ok", f);
    llvm::BasicBlock* bad = llvm::BasicBlock::Create(C, "trap", f);
    b.CreateCondBr(b.CreateExtractValue(r, 0), bad, ok);
    b.SetInsertPoint(bad);
    b.CreateRet(llvm::ConstantInt::get(i32, 1));
    b.SetInsertPoint(ok);
    const std::vector<SlotKind>& rk = kinds(fn.result);
    for (std::size_t i = 0; i < rk.size(); ++i) {
        llvm::Value* v = b.CreateExtractValue(r, static_cast<unsigned>(i + 1));
        llvm::Value* at = b.CreateConstInBoundsGEP1_64(i64, frame, i);
        switch (rk[i]) {
        case SlotKind::I1:
            b.CreateStore(b.CreateZExt(v, i64), at);
            break;
        case SlotKind::F32:
            b.CreateStore(b.CreateZExt(b.CreateBitCast(v, i32), i64), at);
            break;
        default:
            b.CreateStore(v, at);
            break;
        }
    }
    b.CreateRet(llvm::ConstantInt::get(i32, 0));
}

std::string UnitCodegen::build(std::uint32_t root, const std::string& entryName) {
    collectReachable(root);
    computeCycles();
    for (std::uint32_t f : reachable_) {
        if (L.slots(IR.functions[f].result) != kinds(IR.functions[f].result).size()) {
            return "layout mismatch in " + IR.functions[f].name;
        }
        std::string err = FunctionCodegen(*this, f).run();
        if (!err.empty()) {
            return IR.functions[f].name + ": " + err;
        }
        ++lowered_;
        instructions_ += static_cast<std::uint32_t>(IR.functions[f].code.size());
    }
    emitEntry(root, entryName);
    std::string msg;
    llvm::raw_string_ostream os(msg);
    if (llvm::verifyModule(M, &os)) {
        if (std::getenv("QUARTZ_DUMP_LLVM") != nullptr) {
            M.print(llvm::errs(), nullptr);
        }
        return "invalid LLVM IR: " + msg;
    }
    return {};
}

} // namespace quartz::compiler
