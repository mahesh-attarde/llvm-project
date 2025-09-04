# MachineCSE.cpp : Machine Common Subexpression Elimination
## Context
  MachineCSE pass operates after instruction selection while the MachineFunction is still in MIR SSA form (virtual registers, single def per vreg), but before:
- Live interval construction / register allocation (important: CSE can affect register pressure).
- Machine DCE (which will remove dead defs introduced indirectly).
- Peephole optimizations (which may expose or erase further redundancy).

### Goal: 
  - Eliminate redundant machine-level computations and optionally hoist partially redundant computations to a dominator so they become fully redundant and removable (via its own second phase or later passes).

It works in two conceptual phases:
1. Simple PRE (PerformSimplePRE): transform partial redundancy into full redundancy opportunistically.
2. Global CSE (PerformCSE): eliminate fully redundant instructions using value numbering with scoped dominance-aware hash tables.

## Core Data Structures
A. ScopedHTType (ScopedHashTable<MachineInstr*, unsigned, MachineInstrExpressionTrait>)
- Maps an instruction (by structural identity) to a value number (VN).
- Scopes correspond to dominance regions: entering a basic block pushes a scope; exiting pops it. This mirrors classic SSA value scoping.
- MachineInstrExpressionTrait (defined elsewhere) canonicalizes instruction "expression identity": opcode, operands (ignoring defs), implicit operands, memory invariance (for invariant loads), etc.

B. Exps (SmallVector<MachineInstr*, 64>)
- Reverse mapping from VN → representative MachineInstr.
- Enables fast retrieval of the earlier instruction to reuse its result(s).

C. CurrVN
- Monotonically increasing counter assigned to first-seen expressions.

D. ScopeMap<MBB → ScopeType*>
- Tracks which dominance scope object corresponds to each block.

E. PREMap<MachineInstr* → MachineBasicBlock*>
- Tracks first-seen PRE candidate occurrences to detect second occurrence and decide whether to hoist.

F. PhysRefs (SmallSet<MCRegister>)
- Set of all physical registers (and aliases) read by a candidate instruction.
- Used to conservatively block unsafe CSE/PRE across defs/clobbers.

G. PhysDefVector (vector of (operand-index, Reg))
- Records live (non-trivially-dead) physical register defs in an instruction.
- Must ensure those defs remain valid (i.e. not clobbered) if reusing previous computation or hoisting.

H. LookAheadLimit
- Target-provided small integer bounding how far we scan forward for trivial physical register liveness reasoning (avoid full liveness analysis cost).

### High-Level Flow
run():
  - Initialize target hooks: TII, TRI, MRI, LookAheadLimit
  - ChangedPRE = PerformSimplePRE(DT)
  - ChangedCSE = PerformCSE(DT->getRootNode())
  - releaseMemory()
  - return ChangedPRE || ChangedCSE

Why PRE first? PRE may insert hoisted instructions that become globally redundant, enabling more eliminations during CSE.

## Phase 1: Simple PRE (PerformSimplePRE)
Purpose: Convert partially redundant expressions into fully redundant by hoisting a duplicate instruction to the nearest common dominator block (CMBB) of two occurrences.
Key routines:

isPRECandidate(MI, PhysRefs):
  Rejects more aggressively than isCSECandidate:
  - Must have exactly one (explicit) def (simplifies register replacement).
  - No loads (even invariant) because hoisting loads risks memory ordering unless proven safe.
  - Not ‘as cheap as a move’ (profitability – better leave copies cheap).
  - Not not-duplicable (e.g. inline asm, convergent, or special semantics).
  - Gathers physical register uses into PhysRefs for safety checks.

ProcessBlockPRE(DT, MBB):
  For each isPRECandidate MI:
    - Insert into PREMap if first time => remember block and continue.
    - Else (second occurrence):
        Get earlier block MBB1 = PREMap[MI].
        Find CMBB = nearest common dominator(MBB, MBB1).
        Reject if:
          - CMBB not legal to insert (CFG constraints).
          - Candidate not profitable (isProfitableToHoistInto).
          - Reachability test shows loops / cyclic reachability invalidates safety and convergent concerns.
          - Physical register usage isn't safely reaching (PhysRegDefsReach check).
          - Result register substitution wouldn’t be profitable later (checked via isProfitableToCSE using cloned vreg).
        If all pass:
          - Clone MI into CMBB before first terminator (duplicate()).
          - Assign new virtual register (cloned def).
          - Clear debug location (avoid misleading debug stepping).
          - Update PREMap so further occurrences consider hoisted site.
          - Increment NumPREs.

PerformSimplePRE(DT):
  Traverse dominator tree in preorder (explicit stack), calling ProcessBlockPRE.

Notes:
- PRE does not itself eliminate the original instructions; it just creates a hoisted version so the later CSE phase can remove duplicates.
- If CSE fails to eliminate (e.g. heuristics block) the extra inserted instruction might become dead and later removed by dead code elimination.

isProfitableToHoistInto(CandidateBB, MBB, MBB1):
  - If function has minsize attribute => always hoist (favor size over frequency tradeoff heuristics).
  - Otherwise: ensure CandidateBB’s block frequency ≤ freq(MBB) + freq(MBB1).
    Rationale: Don’t move work into a hotter block unless it's beneficial.


5. Phase 2: Global CSE (PerformCSE)

PerformCSE(root):
  - DFS dominator traversal building vector Scopes.
  - OpenChildren counts outstanding children per dominator tree node.
  - For each node in traversal order:
      EnterScope(MBB)
      ProcessBlockCSE(MBB)
      ExitScopeIfDone(node) (pops scopes when all dominated descendants processed)

Dominance-based scoping ensures value numbers from a dominator remain visible in dominated blocks, but values from siblings do not leak across.

ProcessBlockCSE(MBB):
  Loop instructions (allow early-inc since instructions may be erased).
  For each MI:
    1. Filter: isCSECandidate(MI)?
    2. Try lookup in hash table (VNT.count(&MI)).
    3. If miss: attempt trivial copy propagation (PerformTrivialCopyPropagation) to canonicalize operands (exposes equivalence).
    4. If still miss and MI is commutable: attempt commute; if new pattern matches an existing VN, mark FoundCSE. Otherwise restore original if commute unhelpful.
    5. If candidate found but instruction uses/defines physical registers:
         - Collect physical reg usage/defs (hasLivePhysRegDefUses).
         - Try to prove safe reuse (PhysRegDefsReach) including cross-MBB single-predecessor extension.
    6. If still not FoundCSE: insert into value numbering table (VNT.insert) and continue.
    7. Else eliminate:
         - Retrieve representative CSMI via VN.
         - Reject if instruction is convergent and located in different BB (safety).
         - For each def operand pair (OldReg vs NewReg):
             * If same register and implicit => handle implicit bookkeeping.
             * Else check profitability (isProfitableToCSE).
             * Check register constraint compatibility (constrainRegAttrs).
             * Accumulate mapping pairs (OldReg → NewReg).
         - Apply substitutions:
             * Replace all uses of OldReg with NewReg (MRI->replaceRegWith).
             * Clear kill flags on NewReg and extend lifetimes of implicit defs as needed.
             * Update implicit defs’ dead/kill flags correctly (subtle: extending lifetime may invalidate intra-block kill flags; logic rescans instructions between CSMI and MI when local).
             * If cross-MBB physical def reused: ensure predecessor-defined phys regs are added to live-in of successor block.
         - Erase MI.
         - Update statistics.


6. Detailed Function Explanations


6.1 PerformTrivialCopyPropagation(MI, MBB)
- For each register operand use in MI:
  If its defining instruction is a COPY (vreg <- vreg) and:
    - No subregister complications
    - Attribute constraints compatible (constrainRegAttrs)
  Then substitute the source register directly.
  If the COPY becomes unused (hasOneNonDBGUse), erase it (coalescing).
Benefit: Normalizes register use, increasing structural match chance in value numbering.

6.2 isCSECandidate(MI)
Rejects instructions with:
- Position markers, PHIs, implicit-def, kills, debug pseudo, inline asm, fake uses, jump table metadata.
- Copies (handled elsewhere).
- Memory stores, calls, terminators, FP-exception raising ops, unmodelled side-effects.
- Loads that are not invariant+dereferenceable (only invariant loads safe).
- Stack guard intrinsic loads (special semantic: must not be CSE’d across spills).
Returns true only if side-effect free (except possibly invariant load) and movable.

6.3 isProfitableToCSE(CSReg, Reg, CSBB, MI)
Heuristics to avoid increasing register pressure or hurting codegen:
1. If aggressive flag is set: always true.
2. If both are vregs, check that all uses of Reg are also uses of CSReg (otherwise CSE may lengthen CSReg’s live range).
3. Reject if instruction is "as cheap as a move" and prior definition isn’t local or immediate predecessor (avoid lengthening live ranges for trivial ops).
4. If MI does not use any virtual registers and its only uses are copy-like instructions => skip (little benefit).
5. If the common defining register is used by PHIs in other blocks and not already used in the target block => avoid (PHI interactions increase interference).
Otherwise accept.

6.4 hasLivePhysRegDefUses(MI, MBB, PhysRefs, PhysDefs, PhysUseDef)
Builds:
- PhysRefs: all physical regs (and aliases) used (excluding caller-preserved or ignorable).
- PhysDefs: all phys regs defined that are not trivially dead (look-ahead scan; if dead, we can ignore).
- PhysUseDef = true if any defined physical reg also appears in PhysRefs (use+def overlap).
Returns true if there are any physical interactions at all.
Reason: CSE must ensure we do not reorder or extend live ranges of physical regs incorrectly.

6.5 isPhysDefTriviallyDead(Reg, iterator I, E)
Forward scans up to LookAheadLimit instructions skipping debug instructions; if encounters a use → not dead; if encounters a clobber/def before any use → treat as trivially dead. Conservative if run out of scan window or reach block end.

6.6 PhysRegDefsReach(CSMI, MI, PhysRefs, PhysDefs, NonLocal)
Determines whether it is safe to reuse a previous instruction's physical register defs for the later MI. Conditions:
- Same block OR (special case) CSMI in sole predecessor block and phys regs not allocatable/reserved (to avoid forcing live-in usage of scarce phys regs).
- No intervening definitions/clobbers of the referenced physical registers (including regmask clobbers).
- Walk is bounded by LookAheadLimit.
- For cross-block: mark NonLocal = true and allow only if safe.
Returns true only if all phys register uses/defs survive intact.

6.7 EnterScope / ExitScope / ExitScopeIfDone
Maintain scoped hash table for value numbering keyed to dominance regions:
- EnterScope: create a new scope frame.
- ExitScopeIfDone: after finishing a leaf (or when all children processed) pop scopes up the dominator chain.
This mirrors classic lexical scoping of value numbers by dominance: a dominating instruction's value is visible in all dominated blocks, but not visible laterally across CFG siblings.

6.8 PerformCSE(Node)
Builds a DFS ordering (explicit stack) of dom tree nodes; processes blocks in that order, opening scopes as it encounters each. Performs bottom-up scope destruction when children done.

6.9 isPRECandidate explained earlier; stricter than normal CSE to avoid creating “junk” instructions.

6.10 ProcessBlockPRE(DT, MBB)
Matches pairs of candidate instructions across blocks. On second encounter, attempts hoist to CMBB if profitable and safe (physical regs, convergent, reachability safeguards).

6.11 PerformSimplePRE(DT)
Dominator-tree traversal applying ProcessBlockPRE. PREMap accumulates first occurrences. After pass, some hoisted duplicates are now fully redundant w.r.t. their original occurrences → CSE will remove them.

6.12 isProfitableToHoistInto(CandidateBB, MBB, MBB1)
Uses block frequency info (MBFI):
- Accept if minsize function (opt for size).
- Else require freq(CandidateBB) ≤ freq(MBB)+freq(MBB1): guerilla heuristic approximating that added upstream execution cost does not exceed total saved downstream cost.

6.13 releaseMemory()
Clears transient analysis-owned containers to avoid holding capacity across function boundaries in long compile sessions.

### Handling Special Instruction Categories

A. Commutable Instructions:
- MachineInstr::isCommutable used with TII->commuteInstruction.
- If commuting produces a structural match, eliminate; else commute back to original form (avoid changing semantics/canonical form unnecessarily).
- New temporary instruction variant may be created and deleted.

B. Convergent Instructions:
- Disallows non-local CSE and PRE hoisting because hardware semantics may depend on exact dynamic control flow convergence (e.g., GPU wavefront sync points).
- The pass uses a conservative interpretation (extended semantics) to avoid introducing illegal motion where LLVM lacks explicit attributes beyond isConvergent.

C. Invariant Loads:
- Only loads that are both dereferenceable and invariant may be CSE’d (no aliasing/memory reordering analysis here). Other loads are rejected to avoid subtle alias hazards.

D. Implicit Defs:
- Implicitly defined registers (flags, condition codes) require careful kill/dead flag maintenance.
- When removing MI and reusing CSMI’s implicit def, any intervening uses whose kill flags assumed a shorter lifetime must be adjusted.

E. Physical Register Live-Ins:
- If cross-BB CSE extends lifetime of a physical def into successor, the destination block’s live-in set is augmented (MBB->addLiveIn), ensuring later passes (like live range calc) see correct liveness.

### Heuristic Motivations and Trade-Offs
- Register Pressure Avoidance: CSE may increase number of simultaneously live virtual registers; heuristics prevent “cheap” ops from lengthening lifetimes across large regions.
- Avoid Over-committing Physical Registers: Physical registers are scarce; extending their lifetime risks spilling or incorrect scheduling; thus conservative cross-block logic.
- Limiting Scan Complexity: LookAheadLimit prevents quadratic scans; full liveness analysis would be more accurate but expensive at this stage.
- PRE Profitability: Uses block frequency (MBFI) rather than relying purely on structural redundancy to avoid hoisting into hot regions unnecessarily.
- PHI Interactions: Avoid reusing values with PHI consumers unless already present in that block; PHIs can amplify interference and complicate RA.

### Complexity Considerations
Let:
- N = number of machine instructions
- D = number of dominator tree nodes (≈ number of blocks)
- k = average instructions per block
- L = LookAheadLimit (small constant, target-defined)

Operations:
- Value numbering insert/lookup: O(1) average (hash table).
- For each instruction:
  - Copy propagation: proportional to operand uses (usually small).
  - Commutation: constant (if commutable).
  - PhysReg scanning:
      hasLivePhysRegDefUses: O(operands + L * avgOperandsPerInstr).
      PhysRegDefsReach: O(L) unless crossing one edge; crossing at most once due to sole predecessor restriction.
Overall expected complexity: O(N) with small constant factors; heuristics/pruning avoid pathological blow-ups.

### Interactions With Later Passes
- Register Allocation: Reduced redundancy can shrink live intervals, but if heuristics misfire, could extend lifetimes. That is why heuristics are tuned conservatively.
- Machine DCE: Cleans up unused hoisted or partially redundant code not eliminated by CSE.
- Peephole / Copy Propagation: Less noise improves those passes’ ability to canonicalize.
- Scheduling: Fewer instructions gives more flexibility.
- Debug Info: Hoisted instructions have debug loc cleared to avoid misleading user stepping.

### Potential Edge Cases / Pitfalls
- Subregister Copies: Commented out code hints at future enhancement for better coalescing of subreg operations (currently conservative).
- RegMask Interference: Any intervening regmask (call clobber) forces CSE refusal for instructions reading/writing phys regs.
- Multiple defs: CSE logic handles multi-def instructions but only if all compatible; if one def fails profitability, the whole instruction is not CSE’d.
- Implicit Flag Reuse: Kill flag clearing between CSMI and MI is essential to avoid miscompilations in condition flag consumers (condition-code lifetimes extended).
- Convergent semantics: Conservative interpretation may miss legal CSEs but favors correctness.

### Suggested Mental Model

Think of MachineCSEImpl as performing:
- Structural hashing of pure / invariant expressions under dominance scoping.
- Light local liveness reasoning for phys regs.
- Opportunistic PRE that seeds more global redundancy.
- A set of guardrails to avoid register pressure regressions and side-effect miscompilations.

#### Pseudocode
for domNode in dominatorDFS(root):
    EnterScope(domNode.block)
    for MI in block:
        if !isCSECandidate(MI): continue
        Found = VNT.lookup(MI)
        if !Found:
            if PerformTrivialCopyPropagation(MI): attempt lookup again
            if still !Found and MI.isCommutable(): try commute + lookup
        if Found:
            if phys reg complications: revalidate with hasLivePhysRegDefUses + PhysRegDefsReach
        if !Found:
            VNT.insert(MI, CurrVN++)
            continue
        if convergent and cross-BB: abort and insert as new
        if all defs compatible + profitable:
            replace uses, fix implicit flags, add live-ins if needed, erase MI
        else:
            VNT.insert(MI, CurrVN++)
    ExitScopeIfDone(domNode)


14. How to Extend / Improve

Possible enhancements (not in current code):
- Integrate more precise alias/memory dependence to allow more load CSE.
- Use LiveIntervals to more accurately decide profitability (after they are computed).
- Subregister-aware trivial coalescing to expose additional expression equality.
- Broader multi-predecessor cross-block physical register propagation with full liveness tracking.
- Track commuted canonical forms proactively to avoid repeated commute attempts.


15. Quick Reference: Function Roles
run                : Top-level driver combining PRE + CSE
PerformSimplePRE   : PRE phase loop
ProcessBlockPRE    : Per-block PRE detection + hoisting
isPRECandidate     : Strict filter for PRE
isProfitableToHoistInto : Frequency-based profitability
PerformCSE         : Dominator-driven CSE traversal
ProcessBlockCSE    : Per-block CSE logic
PerformTrivialCopyPropagation : Copy folding to normalize operands
isCSECandidate     : Safety + side-effect filter
isProfitableToCSE  : Register pressure & PHI heuristics
hasLivePhysRegDefUses : Collect phys reg use/def sets
isPhysDefTriviallyDead : Small forward scan for dead phys defs
PhysRegDefsReach   : Ensure earlier phys defs still valid
EnterScope / ExitScope / ExitScopeIfDone : Manage scoped value numbering
releaseMemory      : Clear state containers


### Explore More

- Look at MachineInstrExpressionTrait implementation (not in this file) for how structural equivalence is decided.
- Examine TargetInstrInfo::getMachineCSELookAheadLimit for per-target tuning.
- Compare with IR-level GVN / EarlyCSE to understand differences in memory model assumptions.
- Inspect passes that run before and after (e.g., MachineCopyPropagation, DeadMachineInstructionElim) to see synergy.

---


## **Statistics and Configuration Options**
The pass tracks various statistics and exposes options for tuning aggressiveness and performance.
> [MachineCSE.cpp#L53-L71](https://github.com/mahesh-attarde/llvm-project/blob/cd7f7cf5cca6fa425523a5af9fd42f82c6566ebf/llvm/lib/CodeGen/MachineCSE.cpp#L53-L71)
```c++
STATISTIC(NumCoalesces, "Number of copies coalesced");
STATISTIC(NumCSEs,      "Number of common subexpression eliminated");
STATISTIC(NumPREs,      "Number of partial redundant expression"
                        " transformed to fully redundant");
...
static cl::opt<int>
    CSUsesThreshold("csuses-threshold", cl::Hidden, cl::init(1024),
                    cl::desc("Threshold for the size of CSUses"));

static cl::opt<bool> AggressiveMachineCSE(
    "aggressive-machine-cse", cl::Hidden, cl::init(false),
    cl::desc("Override the profitability heuristics for Machine CSE"));
```

---

## 3. **Core Data Structures**

The heart of the logic is in the `MachineCSEImpl` class, which orchestrates the analysis and transformation.

> [MachineCSE.cpp#L74-L133](https://github.com/mahesh-attarde/llvm-project/blob/cd7f7cf5cca6fa425523a5af9fd42f82c6566ebf/llvm/lib/CodeGen/MachineCSE.cpp#L74-L133)
```c++
class MachineCSEImpl {
  const TargetInstrInfo *TII = nullptr;
  const TargetRegisterInfo *TRI = nullptr;
  MachineDominatorTree *DT = nullptr;
  MachineRegisterInfo *MRI = nullptr;
  MachineBlockFrequencyInfo *MBFI = nullptr;

  // Value numbering table and supporting structures.
  using AllocatorTy = RecyclingAllocator<BumpPtrAllocator,
      ScopedHashTableVal<MachineInstr *, unsigned>>;
  using ScopedHTType = ScopedHashTable<MachineInstr *, unsigned, MachineInstrExpressionTrait,
      AllocatorTy>;
  using ScopeType = ScopedHTType::ScopeTy;

  ScopedHTType VNT; // Value numbering table
  SmallVector<MachineInstr *, 64> Exps; // VN → instruction
  unsigned CurrVN = 0;
  DenseMap<MachineBasicBlock *, ScopeType *> ScopeMap;
  ...
};
```
- **ScopedHTType**: Holds the value numbers for instructions, scoped to basic blocks (reflecting dominance).
- **Exps**: Map from value number to representative instruction for replacement.

---

## 4. **Trivial Copy Propagation**

Before CSE, the pass propagates trivial copies to maximize matching opportunities.

> [MachineCSE.cpp#L175-L225](https://github.com/mahesh-attarde/llvm-project/blob/cd7f7cf5cca6fa425523a5af9fd42f82c6566ebf/llvm/lib/CodeGen/MachineCSE.cpp#L175-L225)
[Permalink](https://github.com/mahesh-attarde/llvm-project/blob/cd7f7cf5cca6fa425523a5af9fd42f82c6566ebf/llvm/lib/CodeGen/MachineCSE.cpp#L175-L225)
```c++
bool MachineCSEImpl::PerformTrivialCopyPropagation(MachineInstr *MI,
                                                   MachineBasicBlock *MBB) {
  bool Changed = false;
  for (MachineOperand &MO : MI->all_uses()) {
    Register Reg = MO.getReg();
    if (!Reg.isVirtual())
      continue;
    bool OnlyOneUse = MRI->hasOneNonDBGUse(Reg);
    MachineInstr *DefMI = MRI->getVRegDef(Reg);
    if (!DefMI || !DefMI->isCopy())
      continue;
    Register SrcReg = DefMI->getOperand(1).getReg();
    if (!SrcReg.isVirtual())
      continue;
    ...
    // Propagate SrcReg of copies to MI.
    MO.setReg(SrcReg);
    MRI->clearKillFlags(SrcReg);
    // Coalesce single use copies.
    if (OnlyOneUse) {
      ...
      DefMI->eraseFromParent();
      ++NumCoalesces;
    }
    Changed = true;
  }
  return Changed;
}
```
- **Purpose**: Replace uses of a copied register with the original source register. Remove the copy if it’s now unused.

---

## 5. **Identifying CSE Candidates**

The pass uses strict filters to ensure only safe, profitable instructions are considered.

> [MachineCSE.cpp#L400-L432](https://github.com/mahesh-attarde/llvm-project/blob/cd7f7cf5cca6fa425523a5af9fd42f82c6566ebf/llvm/lib/CodeGen/MachineCSE.cpp#L400-L432)
[Permalink](https://github.com/mahesh-attarde/llvm-project/blob/cd7f7cf5cca6fa425523a5af9fd42f82c6566ebf/llvm/lib/CodeGen/MachineCSE.cpp#L400-L432)
```c++
bool MachineCSEImpl::isCSECandidate(MachineInstr *MI) {
  if (MI->isPosition() || MI->isPHI() || MI->isImplicitDef() || MI->isKill() ||
      MI->isInlineAsm() || MI->isDebugInstr() || MI->isJumpTableDebugInfo() ||
      MI->isFakeUse())
    return false;

  // Ignore copies.
  if (MI->isCopyLike())
    return false;

  // Ignore stuff that we obviously can't move.
  if (MI->mayStore() || MI->isCall() || MI->isTerminator() ||
      MI->mayRaiseFPException() || MI->hasUnmodeledSideEffects())
    return false;

  if (MI->mayLoad()) {
    if (!MI->isDereferenceableInvariantLoad())
      return false;
  }

  if (MI->getOpcode() == TargetOpcode::LOAD_STACK_GUARD)
    return false;

  return true;
}
```
- **Purpose**: Only instructions that are side-effect-free and not special (PHI, call, store, etc.) are eligible for CSE.

---

## 6. **Value Numbering and Dominance Scopes**

The pass uses a scoped hash table to assign value numbers to instructions, respecting dominance.

> [MachineCSE.cpp#L514-L526](https://github.com/mahesh-attarde/llvm-project/blob/cd7f7cf5cca6fa425523a5af9fd42f82c6566ebf/llvm/lib/CodeGen/MachineCSE.cpp#L514-L526)
[Permalink](https://github.com/mahesh-attarde/llvm-project/blob/cd7f7cf5cca6fa425523a5af9fd42f82c6566ebf/llvm/lib/CodeGen/MachineCSE.cpp#L514-L526)
```c++
void MachineCSEImpl::EnterScope(MachineBasicBlock *MBB) {
  LLVM_DEBUG(dbgs() << "Entering: " << MBB->getName() << '\n');
  ScopeType *Scope = new ScopeType(VNT);
  ScopeMap[MBB] = Scope;
}

void MachineCSEImpl::ExitScope(MachineBasicBlock *MBB) {
  LLVM_DEBUG(dbgs() << "Exiting: " << MBB->getName() << '\n');
  DenseMap<MachineBasicBlock*, ScopeType*>::iterator SI = ScopeMap.find(MBB);
  assert(SI != ScopeMap.end());
  delete SI->second;
  ScopeMap.erase(SI);
}
```
- **Purpose**: Ensures value numbers are only visible in dominated blocks, mirroring SSA semantics.

---

## 7. **Block-Level CSE Logic**

`ProcessBlockCSE` is the main loop that finds and eliminates redundant instructions.

> [MachineCSE.cpp#L528-L744](https://github.com/mahesh-attarde/llvm-project/blob/cd7f7cf5cca6fa425523a5af9fd42f82c6566ebf/llvm/lib/CodeGen/MachineCSE.cpp#L528-L744)
[Permalink](https://github.com/mahesh-attarde/llvm-project/blob/cd7f7cf5cca6fa425523a5af9fd42f82c6566ebf/llvm/lib/CodeGen/MachineCSE.cpp#L528-L744)
```c++
bool MachineCSEImpl::ProcessBlockCSE(MachineBasicBlock *MBB) {
  bool Changed = false;
  ...
  for (MachineInstr &MI : llvm::make_early_inc_range(*MBB)) {
    if (!isCSECandidate(&MI))
      continue;

    bool FoundCSE = VNT.count(&MI);
    if (!FoundCSE) {
      if (PerformTrivialCopyPropagation(&MI, MBB)) {
        Changed = true;
        if (MI.isCopyLike())
          continue;
        FoundCSE = VNT.count(&MI);
      }
    }
    // Commute instructions if possible
    bool Commuted = false;
    if (!FoundCSE && MI.isCommutable()) {
      if (MachineInstr *NewMI = TII->commuteInstruction(MI)) {
        Commuted = true;
        FoundCSE = VNT.count(NewMI);
        if (NewMI != &MI) {
          NewMI->eraseFromParent();
          Changed = true;
        } else if (!FoundCSE)
          (void)TII->commuteInstruction(MI);
      }
    }
    ...
    // If found, eliminate redundant instruction
    if (FoundCSE) {
      ...
      MI.eraseFromParent();
      ++NumCSEs;
      if (!PhysRefs.empty())
        ++NumPhysCSEs;
      if (Commuted)
        ++NumCommutes;
      Changed = true;
    } else {
      VNT.insert(&MI, CurrVN++);
      Exps.push_back(&MI);
    }
    ...
  }
  return Changed;
}
```
- **Key Steps**:
  - Check for candidates.
  - Try trivial copy propagation.
  - Try commuting instructions for further matches.
  - If a previous matching instruction exists, replace uses and erase the redundant instruction.

---

## 8. **Profitability Heuristics**

Before eliminating a redundant instruction, the pass checks if it’s actually beneficial to do so.

> [MachineCSE.cpp#L437-L512](https://github.com/mahesh-attarde/llvm-project/blob/cd7f7cf5cca6fa425523a5af9fd42f82c6566ebf/llvm/lib/CodeGen/MachineCSE.cpp#L437-L512)
[Permalink](https://github.com/mahesh-attarde/llvm-project/blob/cd7f7cf5cca6fa425523a5af9fd42f82c6566ebf/llvm/lib/CodeGen/MachineCSE.cpp#L437-L512)
```c++
bool MachineCSEImpl::isProfitableToCSE(Register CSReg, Register Reg,
                                       MachineBasicBlock *CSBB,
                                       MachineInstr *MI) {
  if (AggressiveMachineCSE)
    return true;
  ...
  // Heuristics #1: Don't CSE "cheap" computation if the def is not local or in
  // an immediate predecessor. We don't want to increase register pressure and
  // end up causing other computation to be spilled.
  if (TII->isAsCheapAsAMove(*MI)) {
    MachineBasicBlock *BB = MI->getParent();
    if (CSBB != BB && !CSBB->isSuccessor(BB))
      return false;
  }
  ...
  // Heuristics #2: If the expression doesn't not use a vr and the only use
  // of the redundant computation are copies, do not cse.
  ...
  // Heuristics #3: If the common subexpression is used by PHIs, do not reuse
  // it unless the defined value is already used in the BB of the new use.
  ...
  return !HasPHI;
}
```
- **Purpose**: Prevents CSE from increasing register pressure or introducing other inefficiencies.

---

## 9. **Partial Redundancy Elimination (PRE)**

PRE is handled before CSE to transform partial redundancies into full redundancies.

> [MachineCSE.cpp#L900-L918](https://github.com/mahesh-attarde/llvm-project/blob/cd7f7cf5cca6fa425523a5af9fd42f82c6566ebf/llvm/lib/CodeGen/MachineCSE.cpp#L900-L918)
[Permalink](https://github.com/mahesh-attarde/llvm-project/blob/cd7f7cf5cca6fa425523a5af9fd42f82c6566ebf/llvm/lib/CodeGen/MachineCSE.cpp#L900-L918)
```c++
bool MachineCSEImpl::PerformSimplePRE(MachineDominatorTree *DT) {
  SmallVector<MachineDomTreeNode *, 32> BBs;
  PREMap.clear();
  bool Changed = false;
  BBs.push_back(DT->getRootNode());
  do {
    auto Node = BBs.pop_back_val();
    append_range(BBs, Node->children());
    MachineBasicBlock *MBB = Node->getBlock();
    Changed |= ProcessBlockPRE(DT, MBB);
  } while (!BBs.empty());
  return Changed;
}
```
- **Purpose**: Hoists partially redundant instructions to a dominator block, so CSE can eliminate them.

---

## 10. **Top-Level Pass Entry**

The `run` method ties everything together.

> [MachineCSE.cpp#L938-L948](https://github.com/mahesh-attarde/llvm-project/blob/cd7f7cf5cca6fa425523a5af9fd42f82c6566ebf/llvm/lib/CodeGen/MachineCSE.cpp#L938-L948)
[Permalink](https://github.com/mahesh-attarde/llvm-project/blob/cd7f7cf5cca6fa425523a5af9fd42f82c6566ebf/llvm/lib/CodeGen/MachineCSE.cpp#L938-L948)
```c++
bool MachineCSEImpl::run(MachineFunction &MF) {
  TII = MF.getSubtarget().getInstrInfo();
  TRI = MF.getSubtarget().getRegisterInfo();
  MRI = &MF.getRegInfo();
  LookAheadLimit = TII->getMachineCSELookAheadLimit();
  bool ChangedPRE, ChangedCSE;
  ChangedPRE = PerformSimplePRE(DT);
  ChangedCSE = PerformCSE(DT->getRootNode());
  releaseMemory();
  return ChangedPRE || ChangedCSE;
}
```
- **Purpose**: Initializes and orchestrates the full CSE and PRE pipeline for a function.

---


