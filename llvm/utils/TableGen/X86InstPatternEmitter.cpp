//===- utils/TableGen/X86InstPatternEmitter.cpp - X86 patterns ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This tablegen backend emits instruction-selection patterns for specified X86
// instruction records. When invoked with -gen-x86-inst-patterns and the
// -inst-record= option, it scans all Pat records and emits those whose
// destination DAG references any of the specified instruction names.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/StringSet.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/TableGen/Error.h"
#include "llvm/TableGen/Record.h"
#include "llvm/TableGen/TableGenBackend.h"
#include <string>
#include <vector>

using namespace llvm;

namespace {

static cl::OptionCategory X86InstPatternCat("Options for -gen-x86-inst-patterns");

static cl::opt<std::string> InstRecordNames(
    "inst-record",
    cl::desc("Comma-separated list of X86 instruction record names "
             "(e.g. ADD32rr,ADD32ri)"),
    cl::value_desc("name1,name2,..."),
    cl::cat(X86InstPatternCat));

/// Convert a DagInit or nested Init to a human-readable string.
static std::string dagToString(const Init *I) {
  if (!I)
    return "<null>";
  if (const auto *DI = dyn_cast<DagInit>(I)) {
    std::string Result = "(";
    Result += dagToString(DI->getOperator());
    for (unsigned Idx = 0, E = DI->getNumArgs(); Idx < E; ++Idx) {
      Result += " ";
      StringRef ArgName = DI->getArgNameStr(Idx);
      if (!ArgName.empty()) {
        Result += "$";
        Result += ArgName.str();
        Result += ":";
      }
      Result += dagToString(DI->getArg(Idx));
    }
    Result += ")";
    return Result;
  }
  if (const auto *DefI = dyn_cast<DefInit>(I))
    return DefI->getDef()->getName().str();
  return I->getAsString();
}

/// Return true if \p I (or any node in the DAG rooted at \p I) is a DefInit
/// whose record name is in \p Names.
static bool dagRefersToAny(const Init *I, const StringSet<> &Names) {
  if (!I)
    return false;
  if (const auto *DI = dyn_cast<DagInit>(I)) {
    if (dagRefersToAny(DI->getOperator(), Names))
      return true;
    for (unsigned Idx = 0, E = DI->getNumArgs(); Idx < E; ++Idx)
      if (dagRefersToAny(DI->getArg(Idx), Names))
        return true;
    return false;
  }
  if (const auto *DefI = dyn_cast<DefInit>(I))
    return Names.count(DefI->getDef()->getName()) > 0;
  return false;
}

class X86InstPatternEmitter {
  const RecordKeeper &Records;

public:
  explicit X86InstPatternEmitter(const RecordKeeper &R) : Records(R) {}

  void run(raw_ostream &OS);
};

void X86InstPatternEmitter::run(raw_ostream &OS) {
  // Parse the comma-separated list of instruction record names.
  StringSet<> RequestedNames;
  StringRef Input(InstRecordNames);
  while (!Input.empty()) {
    auto [Head, Tail] = Input.split(',');
    Head = Head.trim();
    if (!Head.empty())
      RequestedNames.insert(Head);
    Input = Tail;
  }

  if (RequestedNames.empty())
    PrintFatalError("No instruction record names specified; use "
                    "-inst-record=<name1,name2,...>");

  emitSourceFileHeader("X86 instruction-selection patterns", OS, Records);

  // Emit header comment listing the requested instruction names.
  OS << "// X86 patterns referencing any of:";
  bool First = true;
  for (const auto &KV : RequestedNames) {
    OS << (First ? " '" : ", '") << KV.getKey() << "'";
    First = false;
  }
  OS << "\n\n";

  // Collect all Pattern-derived definitions (Pat derives from Pattern).
  std::vector<const Record *> PatternDefs =
      Records.getAllDerivedDefinitions("Pattern");

  // Filter to those whose Dst (ResultInstrs[0]) references a requested record.
  std::vector<const Record *> Matching;
  for (const Record *R : PatternDefs) {
    // Safely obtain ResultInstrs field without calling fatal error.
    const RecordVal *RV = R->getValue("ResultInstrs");
    if (!RV)
      continue;
    const auto *ResultInstrs = dyn_cast<ListInit>(RV->getValue());
    if (!ResultInstrs || ResultInstrs->empty())
      continue;
    const auto *Dst = dyn_cast<DagInit>(ResultInstrs->getElement(0));
    if (!Dst)
      continue;

    if (dagRefersToAny(Dst, RequestedNames))
      Matching.push_back(R);
  }

  // Emit output.
  OS << "patterns = [\n";
  for (const Record *R : Matching) {
    // Safely obtain PatternToMatch and ResultInstrs.
    const RecordVal *SrcRV = R->getValue("PatternToMatch");
    const RecordVal *DstRV = R->getValue("ResultInstrs");
    if (!SrcRV || !DstRV)
      continue;
    const auto *Src = dyn_cast<DagInit>(SrcRV->getValue());
    const auto *LI = dyn_cast<ListInit>(DstRV->getValue());
    if (!Src || !LI || LI->empty())
      continue;
    const auto *Dst = dyn_cast<DagInit>(LI->getElement(0));
    if (!Dst)
      continue;

    OS << "  {\n";
    OS << "    // Pattern record: " << R->getName() << "\n";
    OS << "    src = " << dagToString(Src) << ";\n";
    OS << "    dst = " << dagToString(Dst) << ";\n";
    OS << "  },\n";
  }
  OS << "]\n";
}

} // anonymous namespace

static TableGen::Emitter::OptClass<X86InstPatternEmitter>
    X("gen-x86-inst-patterns",
      "Generate X86 instruction-selection patterns for specified records");
