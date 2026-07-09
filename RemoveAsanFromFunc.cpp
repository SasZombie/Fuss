#include "llvm/IR/PassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/Analysis/ProfileSummaryInfo.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Analysis/BlockFrequencyInfo.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"

#include "llvm/Support/CommandLine.h"

#include <fstream>
#include <cstdlib>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <charconv>
#include <queue>
#include <optional>
#include <exception>
#include <tuple>

using namespace llvm;

namespace
{

    struct RemoveAsanFromFunc : public PassInfoMixin<RemoveAsanFromFunc>
    {
    public:
        struct Worker
        {
        private:
            Module &M;
            ModuleAnalysisManager &MAM;

        public:
            Worker(Module &nM, ModuleAnalysisManager &nMAM)
                : M(nM), MAM(nMAM) {};

            struct Metric
            {
                double cost;
                BasicBlock *block;
            };

            bool isAsanFunction(const Function &f) const noexcept
            {
                StringRef Name = f.getName();

                return Name.starts_with("__asan_") ||
                       Name.starts_with("__sanitizer_cov_");
            }

            bool areAlmostEqual(double a, double b, double epsilon = 1e-9) const noexcept
            {
                return std::abs(a - b) < epsilon;
            }

            bool isAsanReportFunction(const Function &f) const noexcept
            {
                return f.getName().starts_with("__asan_report_");
            }

            bool isAsanReportBlock(const BasicBlock *BB) const noexcept
            {
                for (const Instruction &I : *BB)
                {
                    if (auto *CI = dyn_cast<CallInst>(&I))
                    {
                        if (Function *calledFn = CI->getCalledFunction())
                        {
                            if (isAsanReportFunction(*calledFn))
                            {
                                return true;
                            }
                        }
                    }
                }
                return false;
            }

            bool isAsanInstrumentation(const Function &f) const noexcept
            {
                StringRef Name = f.getName();
                return Name.starts_with("__asan_load") ||
                       Name.starts_with("__asan_store");
            }

            std::tuple<std::vector<Metric>, double> gatherInfo() const noexcept
            {
                ProfileSummaryInfo &PSI = MAM.getResult<ProfileSummaryAnalysis>(M);
                if (!PSI.hasProfileSummary())
                {
                    errs() << "No profile analysis found\n";
                    return {};
                }

                auto &proxy = MAM.getResult<FunctionAnalysisManagerModuleProxy>(M);
                FunctionAnalysisManager &FAM = proxy.getManager();

                double globalGrandCost = 0;

                std::vector<Metric> blocks;
                blocks.reserve(200);

                for (Function &F : M)
                {
                    if (F.isDeclaration())
                        continue;

                    BlockFrequencyInfo &BFI = FAM.getResult<BlockFrequencyAnalysis>(F);
                    TargetTransformInfo &TTI = FAM.getResult<TargetIRAnalysis>(F);

                    double entryFrq = static_cast<double>(BFI.getEntryFreq().getFrequency());

                    if (areAlmostEqual(entryFrq, 0))
                    {
                        entryFrq = 1.0;
                    }

                    for (BasicBlock &BB : F)
                    {
                        uint64_t totalBlockProfileCount = 0;
                        auto countOptional = BFI.getBlockProfileCount(&BB);

                        if (countOptional.has_value())
                        {
                            totalBlockProfileCount = countOptional.value();
                        }
                        else
                        {
                            totalBlockProfileCount = BFI.getBlockFreq(&BB).getFrequency();
                        }

                        if (totalBlockProfileCount == 0)
                        {
                            totalBlockProfileCount = 1;
                        }

                        std::vector<CallInst *> asanCalls;
                        for (Instruction &I : BB)
                        {
                            if (auto *CI = dyn_cast<CallInst>(&I))
                            {
                                if (Function *calledFn = CI->getCalledFunction())
                                {
                                    if (isAsanFunction(*calledFn))
                                    {
                                        asanCalls.push_back(CI);
                                    }
                                }
                            }
                        }

                        if (asanCalls.empty())
                            continue;

                        SmallPtrSet<Instruction *, 16> asanAtomInsts;
                        std::queue<Value *> worklist;

                        for (CallInst *asanCall : asanCalls)
                        {
                            asanAtomInsts.insert(asanCall);
                            if (asanCall->getNumOperands() > 0)
                            {
                                worklist.push(asanCall->getOperand(0));
                            }
                        }

                        while (!worklist.empty())
                        {
                            Value *currentVal = worklist.front();
                            worklist.pop();

                            if (auto *I = dyn_cast<Instruction>(currentVal))
                            {
                                if (I->getFunction() == &F && !asanAtomInsts.count(I))
                                {
                                    unsigned opcode = I->getOpcode();
                                    if (hasOpcode(opcode))
                                    {
                                        asanAtomInsts.insert(I);
                                        for (Value *Op : I->operands())
                                        {
                                            worklist.push(Op);
                                        }
                                    }
                                }
                            }
                        }

                        uint64_t atomStaticCostSum = 0;
                        double blockFreq = static_cast<double>(BFI.getBlockFreq(&BB).getFrequency());
                        double relativeWeight = blockFreq / entryFrq;

                        for (Instruction *I : asanAtomInsts)
                        {
                            const InstructionCost sInstr = TTI.getInstructionCost(I, TargetTransformInfo::TCK_CodeSize);
                            const auto costVal = sInstr.getValue();

                            if (!costVal)
                            {
                                continue;
                            }

                            atomStaticCostSum = atomStaticCostSum + costVal;
                        }

                        double totalBlockRuntimeCost = static_cast<double>(atomStaticCostSum) * relativeWeight;

                        Metric m;

                        m.cost = totalBlockRuntimeCost;
                        m.block = &BB;

                        blocks.push_back(m);

                        globalGrandCost = globalGrandCost + totalBlockRuntimeCost;
                    }
                }
                return std::make_tuple(blocks, globalGrandCost);
            }

            bool hasOpcode(unsigned opcode) const noexcept
            {
                if (opcode == Instruction::LShr || opcode == Instruction::And ||
                    opcode == Instruction::Add || opcode == Instruction::Load ||
                    opcode == Instruction::ICmp || opcode == Instruction::PtrToInt ||
                    opcode == Instruction::IntToPtr || opcode == Instruction::GetElementPtr)

                {
                    return true;
                }

                return false;
            }

            PreservedAnalyses pruneASAN() noexcept
            {
                float fussCutoff = 1;

                if (const char *envVal = std::getenv("FUSS_CUTOFF"))
                {
                    if (!to_float(envVal, fussCutoff))
                    {
                        reportFatalUsageError("Invalid cutoff value! Range: 0-1");
                        return PreservedAnalyses::all();
                    }
                }

                if (fussCutoff < 0)
                {
                    reportFatalUsageError("Cuttoff set to a negative value! Range: 0-1");
                    return PreservedAnalyses::all();
                }

                if (fussCutoff > 1)
                {
                    reportFatalUsageError("Cuttoff bigger than 1! Range: 0-1");
                    return PreservedAnalyses::all();
                }

                // 0 = no changes
                if (areAlmostEqual(fussCutoff, 0))
                {
                    errs() << "Prunning 0 instructions\n";
                    return PreservedAnalyses::all();
                }

                auto [blocks, globalGrandCost] = gatherInfo();

                if (globalGrandCost <= 0)
                {
                    errs() << "Warning! The global cost is 0\n";
                    return PreservedAnalyses::all();
                }

                std::sort(blocks.begin(), blocks.end(), [](const Metric &m1, const Metric &m2)
                          { return m1.cost > m2.cost; });

                uint64_t countElemInHotArea = 0;
                double sumOfCosts = 0;

                for (const auto &block : blocks)
                {
                    ++countElemInHotArea;

                    sumOfCosts = sumOfCosts + block.cost;

                    if (sumOfCosts >= globalGrandCost * fussCutoff)
                    {
                        break;
                    }
                }
                uint64_t totalPrunedInstructions = pruneInstructions(blocks, countElemInHotArea);
                errs() << "FUSS: Pruned " << totalPrunedInstructions << " ASan instructions across "
                       << countElemInHotArea << " blocks.\n";

                if (totalPrunedInstructions == 0)
                {
                    return PreservedAnalyses::all();
                }

                return PreservedAnalyses::none();
            }

            uint64_t pruneInstructions(const std::vector<Metric> &blocks, uint64_t countElemInHotArea) noexcept
            {
                uint64_t totalPrunedInstructions = 0;
                for (uint64_t i = 0; i < countElemInHotArea; ++i)
                {
                    BasicBlock *BB = blocks[i].block;
                    std::vector<Instruction *> deadRoots;

                    if (auto *BI = dyn_cast<BranchInst>(BB->getTerminator()))
                    {
                        if (BI->isConditional())
                        {
                            BasicBlock *succTrue = BI->getSuccessor(0);
                            BasicBlock *succFalse = BI->getSuccessor(1);

                            bool trueIsCrash = isAsanReportBlock(succTrue);
                            bool falseIsCrash = isAsanReportBlock(succFalse);

                            if (trueIsCrash || falseIsCrash)
                            {
                                BasicBlock *safeSucc = trueIsCrash ? succFalse : succTrue;
                                BasicBlock *crashSucc = trueIsCrash ? succTrue : succFalse;

                                if (auto *condInst = dyn_cast<Instruction>(BI->getCondition()))
                                {
                                    deadRoots.push_back(condInst);
                                }

                                crashSucc->removePredecessor(BB);

                                BranchInst::Create(safeSucc, BB);

                                BI->eraseFromParent();
                                totalPrunedInstructions++;
                            }
                        }
                    }

                    std::vector<CallInst *> callsToDelete;
                    for (Instruction &I : *BB)
                    {
                        if (auto *CI = dyn_cast<CallInst>(&I))
                        {
                            if (Function *calledFn = CI->getCalledFunction())
                            {
                                if (isAsanInstrumentation(*calledFn) && CI->use_empty())
                                {
                                    callsToDelete.push_back(CI);
                                }
                            }
                        }
                    }

                    for (CallInst *CI : callsToDelete)
                    {
                        for (Value *Op : CI->operands())
                        {
                            if (auto *OpInst = dyn_cast<Instruction>(Op))
                            {
                                deadRoots.push_back(OpInst);
                            }
                        }
                        CI->eraseFromParent();
                        totalPrunedInstructions++;
                    }

                    bool changed;
                    do
                    {
                        changed = false;
                        std::vector<Instruction *> nextDeadRoots;

                        for (Instruction *I : deadRoots)
                        {
                            if (I->getParent() == BB && I->use_empty())
                            {
                                for (Value *Op : I->operands())
                                {
                                    if (auto *OpInst = dyn_cast<Instruction>(Op))
                                    {
                                        nextDeadRoots.push_back(OpInst);
                                    }
                                }

                                I->eraseFromParent();
                                totalPrunedInstructions++;
                                changed = true;
                            }
                        }
                        deadRoots = std::move(nextDeadRoots);

                    } while (changed && !deadRoots.empty());
                }
                return totalPrunedInstructions;
            }

            PreservedAnalyses exec() noexcept
            {
                return pruneASAN();
            }
        };

        PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM)
        {
            Worker w(M, MAM);

            return w.exec();
        }
    };
}

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo llvmGetPassPluginInfo()
{
    return {
        LLVM_PLUGIN_API_VERSION, "RemoveAsanFromFunc", LLVM_VERSION_STRING,
        [](llvm::PassBuilder &PB)
        {
            PB.registerFullLinkTimeOptimizationLastEPCallback(
                [](llvm::ModulePassManager &MPM, llvm::OptimizationLevel Level)
                {
                    MPM.addPass(RemoveAsanFromFunc());
                });

            PB.registerOptimizerLastEPCallback(
                [](llvm::ModulePassManager &MPM, llvm::OptimizationLevel Level, llvm::ThinOrFullLTOPhase Phase)
                {
                    MPM.addPass(RemoveAsanFromFunc());
                });
        }};
}