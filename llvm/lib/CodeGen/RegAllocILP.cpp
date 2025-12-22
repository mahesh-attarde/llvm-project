//===-- RegAllocILP.cpp - ILP-based Register Allocator -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file implements an Integer Linear Programming (ILP) based register
/// allocator that drives allocation decisions using OR-Tools. The allocator
/// falls back to the basic greedy strategy when the ILP model cannot be
/// constructed or solved.
///
//===----------------------------------------------------------------------===//

#include "AllocationOrder.h"
#include "RegAllocBase.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/ProfileSummaryInfo.h"
#include "llvm/CodeGen/CalcSpillWeights.h"
#include "llvm/CodeGen/LiveDebugVariables.h"
#include "llvm/CodeGen/LiveIntervals.h"
#include "llvm/CodeGen/LiveRangeEdit.h"
#include "llvm/CodeGen/LiveRegMatrix.h"
#include "llvm/CodeGen/LiveStacks.h"
#include "llvm/CodeGen/MachineBlockFrequencyInfo.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineLoopInfo.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/RegAllocRegistry.h"
#include "llvm/CodeGen/Spiller.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/CodeGen/VirtRegMap.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/IR/Function.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

#include <memory>
#include <queue>

#if LLVM_WITH_ORTOOLS
#include "ortools/linear_solver/linear_solver.h"
#endif

using namespace llvm;

#define DEBUG_TYPE "regalloc-ilp"

namespace {

struct CompSpillWeight {
  bool operator()(const LiveInterval *A, const LiveInterval *B) const {
    return A->weight() < B->weight();
  }
};

#if LLVM_WITH_ORTOOLS
static constexpr bool HasORTools = true;
#else
static constexpr bool HasORTools = false;
#endif

class RegAllocILP : public MachineFunctionPass,
                    public RegAllocBase,
                    private LiveRangeEdit::Delegate {
  MachineFunction *MF = nullptr;
  std::unique_ptr<Spiller> SpillerInstance;
  std::priority_queue<const LiveInterval *, std::vector<const LiveInterval *>,
                      CompSpillWeight>
      Queue;

  DenseMap<Register, MCRegister> AssignedPhysRegs;
  DenseSet<Register> SpillDecisions;
  bool ILPAttempted = false;
  bool ILPSolved = false;

  BitVector UsableRegs;

  bool LRE_CanEraseVirtReg(Register VirtReg) override;
  void LRE_WillShrinkVirtReg(Register VirtReg) override;

public:
  static char ID;

  RegAllocILP(const RegAllocFilterFunc F = nullptr)
      : MachineFunctionPass(ID), RegAllocBase(F) {}

  StringRef getPassName() const override { return "ILP Register Allocator"; }

  void getAnalysisUsage(AnalysisUsage &AU) const override;

  void releaseMemory() override;

  Spiller &spiller() override { return *SpillerInstance; }

  void enqueueImpl(const LiveInterval *LI) override { Queue.push(LI); }

  const LiveInterval *dequeue() override {
    if (Queue.empty())
      return nullptr;
    const LiveInterval *LI = Queue.top();
    Queue.pop();
    return LI;
  }

  MCRegister selectOrSplit(const LiveInterval &VirtReg,
                           SmallVectorImpl<Register> &SplitVRegs) override;

  bool runOnMachineFunction(MachineFunction &MF) override;

  MachineFunctionProperties getRequiredProperties() const override {
    return MachineFunctionProperties().set(
        MachineFunctionProperties::Property::NoPHIs);
  }

  MachineFunctionProperties getClearedProperties() const override {
    return MachineFunctionProperties().set(
        MachineFunctionProperties::Property::IsSSA);
  }

private:
  bool solveWithILP();
  MCRegister selectOrSplitFallback(const LiveInterval &VirtReg,
                                   SmallVectorImpl<Register> &SplitVRegs);
  bool spillInterferences(const LiveInterval &VirtReg, MCRegister PhysReg,
                          SmallVectorImpl<Register> &SplitVRegs);
};

} // end anonymous namespace

char RegAllocILP::ID = 0;

INITIALIZE_PASS_BEGIN(RegAllocILP, "regalloc-ilp", "ILP Register Allocator",
                      false, false)
INITIALIZE_PASS_DEPENDENCY(LiveDebugVariablesWrapperLegacy)
INITIALIZE_PASS_DEPENDENCY(SlotIndexesWrapperPass)
INITIALIZE_PASS_DEPENDENCY(LiveIntervalsWrapperPass)
INITIALIZE_PASS_DEPENDENCY(RegisterCoalescerLegacy)
INITIALIZE_PASS_DEPENDENCY(MachineSchedulerLegacy)
INITIALIZE_PASS_DEPENDENCY(LiveStacksWrapperLegacy)
INITIALIZE_PASS_DEPENDENCY(AAResultsWrapperPass)
INITIALIZE_PASS_DEPENDENCY(MachineDominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(MachineLoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(VirtRegMapWrapperLegacy)
INITIALIZE_PASS_DEPENDENCY(LiveRegMatrixWrapperLegacy)
INITIALIZE_PASS_DEPENDENCY(ProfileSummaryInfoWrapperPass)
INITIALIZE_PASS_END(RegAllocILP, "regalloc-ilp", "ILP Register Allocator",
                    false, false)

#if LLVM_WITH_ORTOOLS
static RegisterRegAlloc ILPRegAlloc("ilp", "ILP-based register allocator",
                                    createILPRegisterAllocator);
static RegisterRegAlloc ILPRegAllocAlias("regalloc-ilp",
                                         "ILP-based register allocator",
                                         createILPRegisterAllocator);
#endif

bool RegAllocILP::LRE_CanEraseVirtReg(Register VirtReg) {
  LiveInterval &LI = LIS->getInterval(VirtReg);
  if (VRM->hasPhys(VirtReg)) {
    Matrix->unassign(LI);
    aboutToRemoveInterval(LI);
    return true;
  }
  LI.clear();
  return false;
}

void RegAllocILP::LRE_WillShrinkVirtReg(Register VirtReg) {
  if (!VRM->hasPhys(VirtReg))
    return;

  LiveInterval &LI = LIS->getInterval(VirtReg);
  Matrix->unassign(LI);
  enqueue(&LI);
}

void RegAllocILP::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesCFG();
  AU.addRequired<AAResultsWrapperPass>();
  AU.addPreserved<AAResultsWrapperPass>();
  AU.addRequired<LiveIntervalsWrapperPass>();
  AU.addPreserved<LiveIntervalsWrapperPass>();
  AU.addPreserved<SlotIndexesWrapperPass>();
  AU.addRequired<LiveDebugVariablesWrapperLegacy>();
  AU.addPreserved<LiveDebugVariablesWrapperLegacy>();
  AU.addRequired<LiveStacksWrapperLegacy>();
  AU.addPreserved<LiveStacksWrapperLegacy>();
  AU.addRequired<ProfileSummaryInfoWrapperPass>();
  AU.addRequired<MachineBlockFrequencyInfoWrapperPass>();
  AU.addPreserved<MachineBlockFrequencyInfoWrapperPass>();
  AU.addRequired<MachineDominatorTreeWrapperPass>();
  AU.addRequiredID(MachineDominatorsID);
  AU.addPreservedID(MachineDominatorsID);
  AU.addRequired<MachineLoopInfoWrapperPass>();
  AU.addPreserved<MachineLoopInfoWrapperPass>();
  AU.addRequired<VirtRegMapWrapperLegacy>();
  AU.addPreserved<VirtRegMapWrapperLegacy>();
  AU.addRequired<LiveRegMatrixWrapperLegacy>();
  AU.addPreserved<LiveRegMatrixWrapperLegacy>();
  MachineFunctionPass::getAnalysisUsage(AU);
}

void RegAllocILP::releaseMemory() {
  SpillerInstance.reset();
  decltype(Queue)().swap(Queue);
  AssignedPhysRegs.clear();
  SpillDecisions.clear();
  ILPAttempted = false;
  ILPSolved = false;
}

bool RegAllocILP::spillInterferences(const LiveInterval &VirtReg,
                                     MCRegister PhysReg,
                                     SmallVectorImpl<Register> &SplitVRegs) {
  SmallVector<const LiveInterval *, 8> Intfs;

  for (MCRegUnit Unit : TRI->regunits(PhysReg)) {
    LiveIntervalUnion::Query &Q = Matrix->query(VirtReg, Unit);
    for (const auto *Intf : reverse(Q.interferingVRegs())) {
      if (!Intf->isSpillable() || Intf->weight() > VirtReg.weight())
        return false;
      Intfs.push_back(Intf);
    }
  }

  if (Intfs.empty())
    return false;

  for (const LiveInterval *Spill : Intfs) {
    if (!VRM->hasPhys(Spill->reg()))
      continue;

    Matrix->unassign(*Spill);
    LiveRangeEdit LRE(Spill, SplitVRegs, *MF, *LIS, VRM, this, &DeadRemats);
    spiller().spill(LRE);
  }
  return true;
}

MCRegister
RegAllocILP::selectOrSplitFallback(const LiveInterval &VirtReg,
                                   SmallVectorImpl<Register> &SplitVRegs) {
  SmallVector<MCRegister, 8> PhysRegSpillCands;

  auto Order =
      AllocationOrder::create(VirtReg.reg(), *VRM, RegClassInfo, Matrix);
  for (MCRegister PhysReg : Order) {
    assert(PhysReg.isValid());
    switch (Matrix->checkInterference(VirtReg, PhysReg)) {
    case LiveRegMatrix::IK_Free:
      return PhysReg;
    case LiveRegMatrix::IK_VirtReg:
      PhysRegSpillCands.push_back(PhysReg);
      continue;
    default:
      continue;
    }
  }

  for (MCRegister &PhysReg : PhysRegSpillCands) {
    if (!spillInterferences(VirtReg, PhysReg, SplitVRegs))
      continue;
    assert(!Matrix->checkInterference(VirtReg, PhysReg) &&
           "Interference after spill");
    return PhysReg;
  }

  LLVM_DEBUG(dbgs() << "ILP fallback spilling: " << VirtReg << '\n');
  if (!VirtReg.isSpillable())
    return ~0u;

  LiveRangeEdit LRE(&VirtReg, SplitVRegs, *MF, *LIS, VRM, this, &DeadRemats);
  spiller().spill(LRE);
  return 0;
}

bool RegAllocILP::solveWithILP() {
  AssignedPhysRegs.clear();
  SpillDecisions.clear();

  if (!HasORTools)
    return false;

#if LLVM_WITH_ORTOOLS
  using namespace operations_research;

  std::unique_ptr<MPSolver> Solver(
      MPSolver::CreateSolver("CBC_MIXED_INTEGER_PROGRAMMING"));
  if (!Solver)
    return false;

  struct Candidate {
    Register Reg;
    LiveInterval *LI;
    SmallVector<MCRegister, 16> PhysRegs;
    double SpillCost = 0.0;
    bool Spillable = true;
  };

  SmallVector<Candidate, 32> Candidates;
  DenseMap<Register, unsigned> Index;
  SmallVector<DenseSet<MCRegister>, 32> AllowedSets;

  for (unsigned I = 0, E = MRI->getNumVirtRegs(); I != E; ++I) {
    Register Reg = Register::index2VirtReg(I);
    if (MRI->reg_nodbg_empty(Reg))
      continue;
    if (!shouldAllocateRegister(Reg))
      continue;

    LiveInterval &LI = LIS->getInterval(Reg);
    Candidate Cand;
    Cand.Reg = Reg;
    Cand.LI = &LI;
    Cand.Spillable = LI.isSpillable();
    Cand.SpillCost = LI.weight();

    auto Order = AllocationOrder::create(Reg, *VRM, RegClassInfo, Matrix);
    for (MCRegister PhysReg : Order) {
      auto Interf = Matrix->checkInterference(LI, PhysReg);
      if (Interf == LiveRegMatrix::IK_Free ||
          Interf == LiveRegMatrix::IK_VirtReg)
        Cand.PhysRegs.push_back(PhysReg);
    }

    if (Cand.PhysRegs.empty() && !Cand.Spillable) {
      LLVM_DEBUG(dbgs() << "ILP: unsatisfied fixed register for "
                        << printReg(Reg, TRI) << '\n');
      return false;
    }

    Index[Reg] = Candidates.size();
    Candidates.push_back(std::move(Cand));
    AllowedSets.emplace_back();
    for (MCRegister PhysReg : Candidates.back().PhysRegs)
      AllowedSets.back().insert(PhysReg);
  }

  DenseMap<Register, DenseMap<MCRegister, MPVariable *>> AssignVars;
  DenseMap<Register, MPVariable *> SpillVars;

  MPObjective *Objective = Solver->MutableObjective();
  Objective->SetMinimization();

  for (Candidate &Cand : Candidates) {
    MPConstraint *AssignConstraint = Solver->MakeRowConstraint(1.0, 1.0);
    DenseMap<MCRegister, MPVariable *> &VMap = AssignVars[Cand.Reg];

    unsigned PhysIndex = 0;
    for (MCRegister PhysReg : Cand.PhysRegs) {
      std::string VarName =
          ("x_" + Twine(Cand.Reg.id()) + "_" + Twine(PhysReg)).str();
      MPVariable *Var = Solver->MakeBoolVar(VarName);
      VMap[PhysReg] = Var;
      AssignConstraint->SetCoefficient(Var, 1.0);

      double HintPenalty = 0.0;
      auto Hint = VRM->getRegInfo().getRegAllocationHint(Cand.Reg);
      Register HintReg = Hint.second;
      if (HintReg.isVirtual() && VRM->hasPhys(HintReg))
        HintReg = Register(VRM->getPhys(HintReg));
      if (HintReg.isPhysical() && HintReg == Register(PhysReg))
        HintPenalty = -0.1;

      Objective->SetCoefficient(Var, HintPenalty + (0.001 * PhysIndex++));
    }

    if (Cand.Spillable) {
      std::string VarName = ("spill_" + Twine(Cand.Reg.id())).str();
      MPVariable *SpillVar = Solver->MakeBoolVar(VarName);
      SpillVars[Cand.Reg] = SpillVar;
      AssignConstraint->SetCoefficient(SpillVar, 1.0);
      Objective->SetCoefficient(SpillVar, Cand.SpillCost);
    }
  }

  for (unsigned I = 0, E = Candidates.size(); I < E; ++I) {
    const Candidate &A = Candidates[I];
    for (unsigned J = I + 1; J < E; ++J) {
      const Candidate &B = Candidates[J];
      if (!A.LI->overlaps(*B.LI))
        continue;

      for (MCRegister PhysReg : A.PhysRegs) {
        if (!AllowedSets[J].contains(PhysReg))
          continue;
        MPVariable *VarA = AssignVars[A.Reg].lookup(PhysReg);
        MPVariable *VarB = AssignVars[B.Reg].lookup(PhysReg);
        if (!VarA || !VarB)
          continue;
        MPConstraint *C = Solver->MakeRowConstraint(0.0, 1.0);
        C->SetCoefficient(VarA, 1.0);
        C->SetCoefficient(VarB, 1.0);
      }
    }
  }

  const MPSolver::ResultStatus Status = Solver->Solve();
  if (Status != MPSolver::OPTIMAL && Status != MPSolver::FEASIBLE)
    return false;

  for (Candidate &Cand : Candidates) {
    MCRegister Assigned = MCRegister::NoRegister;
    for (MCRegister PhysReg : Cand.PhysRegs) {
      MPVariable *Var = AssignVars[Cand.Reg].lookup(PhysReg);
      if (Var && Var->solution_value() > 0.5) {
        Assigned = PhysReg;
        break;
      }
    }

    if (Assigned != MCRegister::NoRegister) {
      AssignedPhysRegs[Cand.Reg] = Assigned;
      continue;
    }

    MPVariable *SpillVar = SpillVars.lookup(Cand.Reg);
    if (SpillVar && SpillVar->solution_value() > 0.5) {
      SpillDecisions.insert(Cand.Reg);
      continue;
    }

    LLVM_DEBUG(dbgs() << "ILP: no assignment for " << printReg(Cand.Reg, TRI)
                      << '\n');
    return false;
  }

  return true;
#else
  (void)HasORTools;
  return false;
#endif
}

MCRegister RegAllocILP::selectOrSplit(const LiveInterval &VirtReg,
                                      SmallVectorImpl<Register> &SplitVRegs) {
  if (!ILPAttempted) {
    ILPAttempted = true;
    ILPSolved = solveWithILP();
    if (!ILPSolved)
      LLVM_DEBUG(dbgs() << "ILP solver unavailable, using fallback.\n");
  }

  if (ILPSolved) {
    auto It = AssignedPhysRegs.find(VirtReg.reg());
    if (It != AssignedPhysRegs.end())
      return It->second;

    if (SpillDecisions.contains(VirtReg.reg())) {
      SpillDecisions.erase(VirtReg.reg());
      if (!VirtReg.isSpillable())
        return ~0u;
      LiveRangeEdit LRE(&VirtReg, SplitVRegs, *MF, *LIS, VRM, this,
                        &DeadRemats);
      spiller().spill(LRE);
      return 0;
    }
  }

  return selectOrSplitFallback(VirtReg, SplitVRegs);
}

bool RegAllocILP::runOnMachineFunction(MachineFunction &MFParam) {
  LLVM_DEBUG(dbgs() << "********** ILP REGISTER ALLOCATION **********\n"
                    << "********** Function: " << MFParam.getName() << '\n');

  MF = &MFParam;

  auto &MBFI = getAnalysis<MachineBlockFrequencyInfoWrapperPass>().getMBFI();
  auto &LiveStks = getAnalysis<LiveStacksWrapperLegacy>().getLS();
  auto &MDT = getAnalysis<MachineDominatorTreeWrapperPass>().getDomTree();

  RegAllocBase::init(getAnalysis<VirtRegMapWrapperLegacy>().getVRM(),
                     getAnalysis<LiveIntervalsWrapperPass>().getLIS(),
                     getAnalysis<LiveRegMatrixWrapperLegacy>().getLRM());

  VirtRegAuxInfo VRAI(*MF, *LIS, *VRM,
                      getAnalysis<MachineLoopInfoWrapperPass>().getLI(), MBFI,
                      &getAnalysis<ProfileSummaryInfoWrapperPass>().getPSI());
  VRAI.calculateSpillWeightsAndHints();

  SpillerInstance.reset(
      createInlineSpiller({*LIS, LiveStks, MDT, MBFI}, *MF, *VRM, VRAI));

  ILPAttempted = false;
  ILPSolved = false;
  AssignedPhysRegs.clear();
  SpillDecisions.clear();

  allocatePhysRegs();
  postOptimization();

  LLVM_DEBUG(dbgs() << "Post ILP VirtRegMap:\n" << *VRM << "\n");

  releaseMemory();
  return true;
}

FunctionPass *llvm::createILPRegisterAllocator() {
  if (!HasORTools)
    report_fatal_error(
        "ILP register allocator was requested but LLVM was built without "
        "OR-Tools support");
  return new RegAllocILP();
}

FunctionPass *llvm::createILPRegisterAllocator(RegAllocFilterFunc F) {
  if (!HasORTools)
    report_fatal_error(
        "Filtered ILP register allocator requested but OR-Tools is missing");
  return new RegAllocILP(F);
}
