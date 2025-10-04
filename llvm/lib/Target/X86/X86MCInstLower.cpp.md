X86MCInstLower
---

# Overview

**X86MCInstLower.cpp**  converts LLVM’s late-stage machine instructions (`MachineInstr`) into low-level, target-specific MC instructions (`MCInst`) for the X86 architecture. 
The result is used in code emission, binary generation, and assembly printing.
* X86 architecture—relocations
* runtime features
* optimizations
* cross-platform requirements

- **LLVM Compilation Pipeline Location**: PRE-EMIT
- This file sits between the "MachineInstr" representation and the "MCInst" objects used for final code emission.

# Components

## 1. **Class: X86MCInstLower**
- encapsulates the logic to lower `MachineInstr` into `MCInst`.
- **Members**:
  - `MCContext &Ctx`: The context for machine code emission.
  - `const MachineFunction &MF`: The function being compiled.
  - `const TargetMachine &TM`: Overall target information.
  - `const MCAsmInfo &MAI`: Assembly code emission details.
  - `X86AsmPrinter &AsmPrinter`: Handles code emission and auxiliary tasks.

- **Key Methods**:
  - **LowerMachineOperand()**: Converts a single `MachineOperand` to an `MCOperand`, handling registers, immediates, symbols, jump tables, etc.
  - **Lower()**: The main entry point. Converts a full `MachineInstr` to `MCInst`, calling `LowerMachineOperand` for each operand, then applies target-specific optimizations or special-case handling.
  - **GetSymbolFromOperand()**: Resolves global addresses, external symbols, and MBBs into MCSymbols, applying target-specific name mangling and stub management.
  - **LowerSymbolOperand()**: Applies target-specific flags (DLL import, TLS, PLT, GOT, etc.) to produce the proper MCExpr for symbolic operands.

## 2. **Lowering Process**

- **Operand Handling**:
  - Registers: Mapped directly, ignoring implicit ones.
  - Immediates: Passed as-is.
  - Memory references: Lowered to MCExprs, handling offsets and symbolic references.
  - Special flags: Handles various X86-specific relocation and linkage flags (TLS, GOT, PLT, etc.).

- **Instruction Handling**:
  - Opcode mapping: Sometimes pseudo-instructions (like TAILJMP) are mapped to their real binary opcodes.
  - Special cases: Instructions like LEA, MULX, CALL, RET, EH_RETURN, etc. have custom lowering logic, ensuring proper encoding and semantics.

- **Optimizations**:
  - What Kind of optimizations? 
    - converting VEX3 to VEX2,
    - optimizing shift/rotate instructions, MOVSX, INC/DEC.
    - MOV instructions for encoding compactness or performance.
    - Padding Insertion

## 3. **Special Regions and RAII**

- **NoAutoPaddingScope**: A RAII struct to temporarily disable automatic NOP padding between instructions, necessary for correctness in certain regions (e.g., shadow tracking for stackmaps, patchpoints).
- What?
- 

## 4. **NOP Emission**

- **emitX86Nops() / emitNop()**: Functions to generate optimal X86 NOP sequences of arbitrary length. This is used for aligning code (e.g., for instrumentation sleds or padding stackmap shadows)
- 

## 5. **TLS, Statepoint, Patchpoints, Faulting Operations**
- TLS ?
- PatchPoint?
- lowering routines for advanced runtime features:
  - **LowerTlsAddr**: Emits TLS address calculation sequences.
  - **LowerSTATEPOINT / LowerPATCHPOINT**: Supports garbage collection safe-points and patchable call sites needed for managed runtimes.
  - **LowerFAULTING_OP**: Emits faulting load operations for runtime error handling.

## 6. **XRay Instrumentation Support**

- Multiple lowering routines for XRay sleds, which are used for dynamic function tracing and instrumentation. These routines handle complex code emission patterns, including register handling, call trampolines, and patchable sled regions.

## 7. **Stackmap and Shadow Tracking**

- Tracks regions of code where stackmaps or patchpoints are required, ensuring alignment and proper emission for runtime support (e.g., garbage collection, deoptimization).

## 8. **Windows Exception Handling (SEH)**

- Contains logic for emitting the correct prologue/epilogue, frame, and unwind info for Windows SEH, including integration with Import Call Optimization (ICO) and the IP2State table requirements.

## 9. **Constant Pool and Shuffle Comments**

- Adds assembly comments explaining the effect of vector shuffle operations and constant loads, improving debuggability and transparency of generated code.

## 10. **Instruction Emission Integration**

- The file is closely integrated with X86AsmPrinter, the main code emission class for X86. Emission routines handle stackmap shadow regions, call padding, XRay sleds, and other runtime features.

---

# Design Patterns and Engineering Highlights
- **RAII for Padding Control**: Ensures regions with strict alignment constraints are handled safely.
- **Extensive Switch/Pattern Matching**: Handles the vast space of X86 instructions, opcodes, flags, and pseudo-instructions.
- **Target-specific Name Mangling and Relocation Handling**: Correctly produces symbols and expressions for global addresses under different OSes and linking models.
- **Extensibility**: Makes it straightforward to add new X86 features, pseudo-instructions, or runtime support.
- **Performance Awareness**: NOP emission and instruction optimization are tuned for microarchitecture-specific performance.
- **Correctness in Complex Environments**: Special care for Windows EH, ICO, XRay, TLS, and stackmap/pinpoint regions.


# Important Functions and Algorithms in X86MCInstLower.cpp (walk branch)

Permalink: [llvm/lib/Target/X86/X86MCInstLower.cpp@walk](https://github.com/mahesh-attarde/llvm-project/blob/ab292606b186dc9a00191f0791fec028adb99e87/llvm/lib/Target/X86/X86MCInstLower.cpp)

This file contains the core logic for converting LLVM's X86 `MachineInstr` to MC-level instructions (`MCInst`). Below are the most important functions and algorithms used, with direct code snippets for illustration.

---

## 1. The X86MCInstLower Class

**Purpose:** Encapsulates lowering machinery.

```c++ name=X86MCInstLower.cpp url=https://github.com/mahesh-attarde/llvm-project/blob/ab292606b186dc9a00191f0791fec028adb99e87/llvm/lib/Target/X86/X86MCInstLower.cpp
class X86MCInstLower {
  MCContext &Ctx;
  const MachineFunction &MF;
  const TargetMachine &TM;
  const MCAsmInfo &MAI;
  X86AsmPrinter &AsmPrinter;

public:
  X86MCInstLower(const MachineFunction &MF, X86AsmPrinter &asmprinter);

  MCOperand LowerMachineOperand(const MachineInstr *MI, const MachineOperand &MO) const;
  void Lower(const MachineInstr *MI, MCInst &OutMI) const;
  MCSymbol *GetSymbolFromOperand(const MachineOperand &MO) const;
  MCOperand LowerSymbolOperand(const MachineOperand &MO, MCSymbol *Sym) const;

private:
  MachineModuleInfoMachO &getMachOMMI() const;
};
```

---

## 2. LowerMachineOperand

**Purpose:** Converts a `MachineOperand` to an `MCOperand`, handling all X86 operand kinds.

```c++ name=X86MCInstLower.cpp url=https://github.com/mahesh-attarde/llvm-project/blob/ab292606b186dc9a00191f0791fec028adb99e87/llvm/lib/Target/X86/X86MCInstLower.cpp
MCOperand X86MCInstLower::LowerMachineOperand(const MachineInstr *MI, const MachineOperand &MO) const {
  switch (MO.getType()) {
  ...
  case MachineOperand::MO_Register:
    if (MO.isImplicit())
      return MCOperand();
    return MCOperand::createReg(MO.getReg());
  case MachineOperand::MO_Immediate:
    return MCOperand::createImm(MO.getImm());
  case MachineOperand::MO_GlobalAddress:
    return LowerSymbolOperand(MO, GetSymbolFromOperand(MO));
  ...
  case MachineOperand::MO_ExternalSymbol: {
    MCSymbol *Sym = GetSymbolFromOperand(MO);
    Sym->setExternal(true);
    return LowerSymbolOperand(MO, Sym);
  }
  ...
  case MachineOperand::MO_RegisterMask:
    return MCOperand();
  }
}
```

**Algorithm Highlights:**
- Switches on operand type.
- Handles register, immediate, global address, symbols, jump tables, constant pools, block addresses.

---

## 3. Lower

**Purpose:** Converts a full `MachineInstr` to an `MCInst`, applying X86-specific transformations and encodings.

```c++ name=X86MCInstLower.cpp url=https://github.com/mahesh-attarde/llvm-project/blob/ab292606b186dc9a00191f0791fec028adb99e87/llvm/lib/Target/X86/X86MCInstLower.cpp
void X86MCInstLower::Lower(const MachineInstr *MI, MCInst &OutMI) const {
  OutMI.setOpcode(MI->getOpcode());
  for (const MachineOperand &MO : MI->operands())
    if (auto Op = LowerMachineOperand(MI, MO); Op.isValid())
      OutMI.addOperand(Op);

  bool In64BitMode = AsmPrinter.getSubtarget().is64Bit();
  if (X86::optimizeInstFromVEX3ToVEX2(OutMI, MI->getDesc()) ||
      X86::optimizeShiftRotateWithImmediateOne(OutMI) ||
      X86::optimizeVPCMPWithImmediateOneOrSix(OutMI) ||
      X86::optimizeMOVSX(OutMI) || X86::optimizeINCDEC(OutMI, In64BitMode) ||
      X86::optimizeMOV(OutMI, In64BitMode) ||
      X86::optimizeToFixedRegisterOrShortImmediateForm(OutMI))
    return;
  ...
  // Special cases for pseudo instructions, e.g. TAILJMP, LEA, MULX, CALL, RET, etc.
}
```

---

## 4. GetSymbolFromOperand

**Purpose:** Resolves the symbol for a global address, external symbol, or basic block operand, applying target-specific mangling and stub logic.

```c++ name=X86MCInstLower.cpp url=https://github.com/mahesh-attarde/llvm-project/blob/ab292606b186dc9a00191f0791fec028adb99e87/llvm/lib/Target/X86/X86MCInstLower.cpp
MCSymbol *X86MCInstLower::GetSymbolFromOperand(const MachineOperand &MO) const {
  ...
  switch (MO.getTargetFlags()) {
  case X86II::MO_DLLIMPORT:
    Name += "__imp_";
    break;
  case X86II::MO_COFFSTUB:
    Name += ".refptr.";
    break;
  case X86II::MO_DARWIN_NONLAZY:
  case X86II::MO_DARWIN_NONLAZY_PIC_BASE:
    Suffix = "$non_lazy_ptr";
    break;
  }
  ...
  if (MO.isGlobal()) {
    const GlobalValue *GV = MO.getGlobal();
    AsmPrinter.getNameWithPrefix(Name, GV);
  } else if (MO.isSymbol()) {
    Mangler::getNameWithPrefix(Name, MO.getSymbolName(), DL);
  } else if (MO.isMBB()) {
    Sym = MO.getMBB()->getSymbol();
  }
  ...
  return Sym;
}
```

---

## 5. LowerSymbolOperand

**Purpose:** Applies X86 relocation kinds to symbolic operands.

```c++ name=X86MCInstLower.cpp url=https://github.com/mahesh-attarde/llvm-project/blob/ab292606b186dc9a00191f0791fec028adb99e87/llvm/lib/Target/X86/X86MCInstLower.cpp
MCOperand X86MCInstLower::LowerSymbolOperand(const MachineOperand &MO, MCSymbol *Sym) const {
  const MCExpr *Expr = nullptr;
  MCSymbolRefExpr::VariantKind RefKind = MCSymbolRefExpr::VK_None;

  switch (MO.getTargetFlags()) {
  case X86II::MO_TLVP:
    RefKind = MCSymbolRefExpr::VK_TLVP;
    break;
  case X86II::MO_GOTPCREL:
    RefKind = MCSymbolRefExpr::VK_GOTPCREL;
    break;
  // ... many other relocation types
  }
  if (!Expr)
    Expr = MCSymbolRefExpr::create(Sym, RefKind, Ctx);
  if (!MO.isJTI() && !MO.isMBB() && MO.getOffset())
    Expr = MCBinaryExpr::createAdd(Expr, MCConstantExpr::create(MO.getOffset(), Ctx), Ctx);
  return MCOperand::createExpr(Expr);
}
```

---

## 6. convertTailJumpOpcode

**Purpose:** Maps pseudo tail call opcodes to real X86 opcodes.

```c++ name=X86MCInstLower.cpp url=https://github.com/mahesh-attarde/llvm-project/blob/ab292606b186dc9a00191f0791fec028adb99e87/llvm/lib/Target/X86/X86MCInstLower.cpp
static unsigned convertTailJumpOpcode(unsigned Opcode) {
  switch (Opcode) {
  case X86::TAILJMPr: Opcode = X86::JMP32r; break;
  case X86::TAILJMPm: Opcode = X86::JMP32m; break;
  case X86::TAILJMPr64: Opcode = X86::JMP64r; break;
  case X86::TAILJMPm64: Opcode = X86::JMP64m; break;
  // ... etc
  }
  return Opcode;
}
```

---

## 7. emitX86Nops and emitNop

**Purpose:** Emits sequences of optimal NOP instructions for alignment, padding, and instrumentation.

```c++ name=X86MCInstLower.cpp url=https://github.com/mahesh-attarde/llvm-project/blob/ab292606b186dc9a00191f0791fec028adb99e87/llvm/lib/Target/X86/X86MCInstLower.cpp
static unsigned emitNop(MCStreamer &OS, unsigned NumBytes, const X86Subtarget *Subtarget) {
  // Determine the longest nop which can be efficiently decoded for the given
  // target cpu.  15-bytes is the longest single NOP instruction, but some
  // platforms can't decode the longest forms efficiently.
  unsigned MaxNopLength = 1;
  if (Subtarget->is64Bit()) { ... }
  // Cap a single nop emission at the profitable value for the target
  NumBytes = std::min(NumBytes, MaxNopLength);
  // ... emits the actual NOP instructions
  return NopSize;
}

static void emitX86Nops(MCStreamer &OS, unsigned NumBytes, const X86Subtarget *Subtarget) {
  while (NumBytes) {
    NumBytes -= emitNop(OS, NumBytes, Subtarget);
  }
}
```

---

## 8. Advanced Features

- **LowerSTATEPOINT, LowerPATCHPOINT, LowerFAULTING_OP**: Lower statepoints, patchpoints, and faulting operations for GC, instrumentation, and error handling.
- **LowerPATCHABLE_FUNCTION_ENTER, LowerPATCHABLE_RET, LowerPATCHABLE_TAIL_CALL**: Instrumentation sled emission for XRay tracing.
- **EmitSEHInstruction**: Emits Windows SEH unwind info and related code.

---

## 9. Integration with AsmPrinter

**Purpose:** Connects lowering logic with the AsmPrinter, driving the emission of instructions and handling runtime features.

```c++ name=X86MCInstLower.cpp url=https://github.com/mahesh-attarde/llvm-project/blob/ab292606b186dc9a00191f0791fec028adb99e87/llvm/lib/Target/X86/X86MCInstLower.cpp
void X86AsmPrinter::emitInstruction(const MachineInstr *MI) {
  X86MCInstLower MCInstLowering(*MF, *this);
  ...
  MCInst TmpInst;
  MCInstLowering.Lower(MI, TmpInst);
  ...
  OutStreamer->emitInstruction(TmpInst, getSubtargetInfo());
}
```
