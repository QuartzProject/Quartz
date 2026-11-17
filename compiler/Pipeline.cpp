#include "Codegen.hpp"

#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Analysis/GlobalsModRef.h>
#include <llvm/Analysis/InlineAdvisor.h>
#include <llvm/Analysis/InlineCost.h>
#include <llvm/Analysis/ProfileSummaryInfo.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Transforms/IPO/AlwaysInliner.h>
#include <llvm/Transforms/IPO/ConstantMerge.h>
#include <llvm/Transforms/IPO/DeadArgumentElimination.h>
#include <llvm/Transforms/IPO/FunctionAttrs.h>
#include <llvm/Transforms/IPO/GlobalDCE.h>
#include <llvm/Transforms/IPO/GlobalOpt.h>
#include <llvm/Transforms/IPO/InferFunctionAttrs.h>
#include <llvm/Transforms/IPO/Inliner.h>
#include <llvm/Transforms/IPO/SCCP.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar/ADCE.h>
#include <llvm/Transforms/Scalar/AlignmentFromAssumptions.h>
#include <llvm/Transforms/Scalar/BDCE.h>
#include <llvm/Transforms/Scalar/DivRemPairs.h>
#include <llvm/Transforms/Scalar/DropUnnecessaryAssumes.h>
#include <llvm/Transforms/Scalar/EarlyCSE.h>
#include <llvm/Transforms/Scalar/ExpandMemCmp.h>
#include <llvm/Transforms/Scalar/Float2Int.h>
#include <llvm/Transforms/Scalar/IndVarSimplify.h>
#include <llvm/Transforms/Scalar/InferAlignment.h>
#include <llvm/Transforms/Scalar/InstSimplifyPass.h>
#include <llvm/Transforms/Scalar/LICM.h>
#include <llvm/Transforms/Scalar/LoopDeletion.h>
#include <llvm/Transforms/Scalar/LoopIdiomRecognize.h>
#include <llvm/Transforms/Scalar/LoopInstSimplify.h>
#include <llvm/Transforms/Scalar/LoopLoadElimination.h>
#include <llvm/Transforms/Scalar/LoopPassManager.h>
#include <llvm/Transforms/Scalar/LoopRotation.h>
#include <llvm/Transforms/Scalar/LoopSimplifyCFG.h>
#include <llvm/Transforms/Scalar/LoopSink.h>
#include <llvm/Transforms/Scalar/LoopUnrollPass.h>
#include <llvm/Transforms/Scalar/LowerConstantIntrinsics.h>
#include <llvm/Transforms/Scalar/MemCpyOptimizer.h>
#include <llvm/Transforms/Scalar/MergeICmps.h>
#include <llvm/Transforms/Scalar/Reassociate.h>
#include <llvm/Transforms/Scalar/SCCP.h>
#include <llvm/Transforms/Scalar/SROA.h>
#include <llvm/Transforms/Scalar/SimpleLoopUnswitch.h>
#include <llvm/Transforms/Scalar/SimplifyCFG.h>
#include <llvm/Transforms/Scalar/TailRecursionElimination.h>
#include <llvm/Transforms/Utils/InjectTLIMappings.h>
#include <llvm/Transforms/Utils/LibCallsShrinkWrap.h>
#include <llvm/Transforms/Utils/Mem2Reg.h>
#include <llvm/Transforms/Utils/RelLookupTableConverter.h>
#include <llvm/Transforms/Vectorize/LoopVectorize.h>

namespace quartz::compiler {

namespace {

using llvm::SimplifyCFGOptions;

SimplifyCFGOptions cfg() { return SimplifyCFGOptions().convertSwitchRangeToICmp(true); }

template <class Pass>
auto fn(Pass p) {
    return llvm::createModuleToFunctionPassAdaptor(std::move(p), true);
}

llvm::FunctionPassManager simplifyFunction(const llvm::PipelineTuningOptions& pto) {
    llvm::FunctionPassManager f;
    f.addPass(llvm::SROAPass(llvm::SROAOptions::ModifyCFG));
    f.addPass(llvm::EarlyCSEPass(true));
    f.addPass(llvm::SimplifyCFGPass(cfg()));
    f.addPass(llvm::InstCombinePass());
    f.addPass(llvm::LibCallsShrinkWrapPass());
    f.addPass(llvm::SimplifyCFGPass(cfg()));
    f.addPass(llvm::ReassociatePass());
    llvm::LoopPassManager hoist;
    hoist.addPass(llvm::LoopInstSimplifyPass());
    hoist.addPass(llvm::LoopSimplifyCFGPass());
    hoist.addPass(llvm::LICMPass(pto.LicmMssaOptCap, pto.LicmMssaNoAccForPromotionCap, false));
    hoist.addPass(llvm::LoopRotatePass(true, false));
    hoist.addPass(llvm::LICMPass(pto.LicmMssaOptCap, pto.LicmMssaNoAccForPromotionCap, true));
    hoist.addPass(llvm::SimpleLoopUnswitchPass());
    llvm::LoopPassManager induction;
    induction.addPass(llvm::LoopIdiomRecognizePass());
    induction.addPass(llvm::IndVarSimplifyPass());
    induction.addPass(llvm::LoopDeletionPass());
    induction.addPass(llvm::LoopFullUnrollPass(1, false, pto.ForgetAllSCEVInLoopUnroll));
    f.addPass(llvm::createFunctionToLoopPassAdaptor(std::move(hoist), true));
    f.addPass(llvm::SimplifyCFGPass(cfg()));
    f.addPass(llvm::InstCombinePass());
    f.addPass(llvm::createFunctionToLoopPassAdaptor(std::move(induction), false));
    f.addPass(llvm::SROAPass(llvm::SROAOptions::ModifyCFG));
    f.addPass(llvm::MemCpyOptPass());
    f.addPass(llvm::SCCPPass());
    f.addPass(llvm::BDCEPass());
    f.addPass(llvm::InstCombinePass());
    f.addPass(llvm::ADCEPass());
    f.addPass(llvm::SimplifyCFGPass(cfg()));
    f.addPass(llvm::InstCombinePass());
    return f;
}

llvm::FunctionPassManager optimizeFunction(const llvm::PipelineTuningOptions& pto) {
    llvm::FunctionPassManager f;
    f.addPass(llvm::DropUnnecessaryAssumesPass());
    f.addPass(llvm::Float2IntPass());
    f.addPass(llvm::LowerConstantIntrinsicsPass());
    llvm::LoopPassManager rotate;
    rotate.addPass(llvm::LoopRotatePass(true, false, true));
    rotate.addPass(llvm::LoopDeletionPass());
    f.addPass(llvm::createFunctionToLoopPassAdaptor(std::move(rotate), false));
    f.addPass(llvm::InjectTLIMappings());
    f.addPass(llvm::LoopVectorizePass(llvm::LoopVectorizeOptions(!pto.LoopInterleaving, !pto.LoopVectorization)));
    f.addPass(llvm::DropUnnecessaryAssumesPass(true));
    f.addPass(llvm::InferAlignmentPass());
    f.addPass(llvm::LoopLoadEliminationPass());
    f.addPass(llvm::InstCombinePass());
    f.addPass(llvm::SimplifyCFGPass(SimplifyCFGOptions()
                                        .forwardSwitchCondToPhi(true)
                                        .convertSwitchRangeToICmp(true)
                                        .convertSwitchToArithmetic(true)
                                        .convertSwitchToLookupTable(true)
                                        .needCanonicalLoops(false)
                                        .hoistCommonInsts(true)
                                        .sinkCommonInsts(true)));
    f.addPass(llvm::InstCombinePass());
    f.addPass(llvm::LoopUnrollPass(llvm::LoopUnrollOptions(1, false, pto.ForgetAllSCEVInLoopUnroll)));
    f.addPass(llvm::SROAPass(llvm::SROAOptions(llvm::SROAOptions::PreserveCFG, true)));
    f.addPass(llvm::InferAlignmentPass());
    f.addPass(llvm::InstCombinePass());
    f.addPass(llvm::createFunctionToLoopPassAdaptor(
        llvm::LICMPass(pto.LicmMssaOptCap, pto.LicmMssaNoAccForPromotionCap, true), true));
    f.addPass(llvm::AlignmentFromAssumptionsPass());
    f.addPass(llvm::LoopSinkPass());
    f.addPass(llvm::InstSimplifyPass());
    f.addPass(llvm::DivRemPairsPass());
    f.addPass(llvm::MergeICmpsPass());
    f.addPass(llvm::ExpandMemCmpPass());
    f.addPass(llvm::TailCallElimPass(false));
    f.addPass(llvm::SimplifyCFGPass(SimplifyCFGOptions()
                                        .convertSwitchRangeToICmp(true)
                                        .convertSwitchToArithmetic(true)
                                        .speculateUnpredictables(true)
                                        .hoistLoadsStoresWithCondFaulting(true)));
    return f;
}

} // namespace

void optimize(llvm::Module& mod, llvm::TargetMachine* tm) {
    llvm::PipelineTuningOptions pto;
    llvm::LoopAnalysisManager lam;
    llvm::FunctionAnalysisManager fam;
    llvm::CGSCCAnalysisManager cgam;
    llvm::ModuleAnalysisManager mam;
    llvm::PassBuilder pb(tm, pto);
    pb.registerModuleAnalyses(mam);
    pb.registerCGSCCAnalyses(cgam);
    pb.registerFunctionAnalyses(fam);
    pb.registerLoopAnalyses(lam);
    pb.crossRegisterProxies(lam, fam, cgam, mam);

    llvm::ModulePassManager m;
    m.addPass(llvm::InferFunctionAttrsPass());
    llvm::FunctionPassManager early;
    early.addPass(llvm::SimplifyCFGPass());
    early.addPass(llvm::SROAPass(llvm::SROAOptions::ModifyCFG));
    early.addPass(llvm::EarlyCSEPass());
    m.addPass(fn(std::move(early)));
    m.addPass(llvm::IPSCCPPass(llvm::IPSCCPOptions(true)));
    m.addPass(llvm::GlobalOptPass());
    llvm::FunctionPassManager cleanup;
    cleanup.addPass(llvm::PromotePass());
    cleanup.addPass(llvm::InstCombinePass());
    cleanup.addPass(llvm::SimplifyCFGPass(cfg()));
    m.addPass(fn(std::move(cleanup)));
    m.addPass(llvm::AlwaysInlinerPass(true));

    llvm::ModuleInlinerWrapperPass inliner(
        llvm::getInlineParamsFromOptLevel(1), false,
        llvm::InlineContext{llvm::ThinOrFullLTOPhase::None, llvm::InlinePass::CGSCCInliner},
        llvm::InliningAdvisorMode::Default, 4);
    inliner.addModulePass(llvm::RequireAnalysisPass<llvm::GlobalsAA, llvm::Module>());
    inliner.addModulePass(fn(llvm::InvalidateAnalysisPass<llvm::AAManager>()));
    inliner.addModulePass(llvm::RequireAnalysisPass<llvm::ProfileSummaryAnalysis, llvm::Module>());
    llvm::CGSCCPassManager& scc = inliner.getPM();
    scc.addPass(llvm::PostOrderFunctionAttrsPass(true));
    scc.addPass(llvm::createCGSCCToFunctionPassAdaptor(simplifyFunction(pto), true, true));
    scc.addPass(llvm::PostOrderFunctionAttrsPass());
    scc.addPass(llvm::createCGSCCToFunctionPassAdaptor(
        llvm::RequireAnalysisPass<llvm::ShouldNotRunFunctionPassesAnalysis, llvm::Function>()));
    inliner.addLateModulePass(fn(llvm::InvalidateAnalysisPass<llvm::ShouldNotRunFunctionPassesAnalysis>()));
    m.addPass(std::move(inliner));

    m.addPass(llvm::DeadArgumentEliminationPass());
    m.addPass(llvm::GlobalOptPass());
    m.addPass(llvm::GlobalDCEPass());
    m.addPass(llvm::ReversePostOrderFunctionAttrsPass());
    m.addPass(llvm::RecomputeGlobalsAAPass());
    m.addPass(fn(optimizeFunction(pto)));
    m.addPass(llvm::GlobalDCEPass());
    m.addPass(llvm::ConstantMergePass());
    m.addPass(llvm::RelLookupTableConverterPass());
    m.run(mod, mam);
}

} // namespace quartz::compiler
