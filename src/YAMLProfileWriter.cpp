//===- YAMLProfileWriter.cpp - serialize profiling data in YAML -*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//
//===----------------------------------------------------------------------===//

#include "BinaryBasicBlock.h"
#include "BinaryFunction.h"
#include "DataAggregator.h"
#include "ProfileYAMLMapping.h"
#include "YAMLProfileWriter.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#undef  DEBUG_TYPE
#define DEBUG_TYPE "bolt-prof"

namespace llvm {
namespace bolt {

namespace {
void
convert(const BinaryFunction &BF, yaml::bolt::BinaryFunctionProfile &YamlBF) {
  auto &BC = BF.getBinaryContext();

  const auto LBRProfile = BF.getProfileFlags() & BinaryFunction::PF_LBR;

  YamlBF.Name = BF.getPrintName();
  YamlBF.Id = BF.getFunctionNumber();
  YamlBF.Hash = BF.computeHash(/*UseDFS=*/true);
  YamlBF.NumBasicBlocks = BF.size();
  YamlBF.ExecCount = BF.getKnownExecutionCount();

  for (const auto *BB : BF.dfs()) {
    yaml::bolt::BinaryBasicBlockProfile YamlBB;
    YamlBB.Index = BB->getLayoutIndex();
    YamlBB.NumInstructions = BB->getNumNonPseudos();

    if (!LBRProfile) {
      YamlBB.EventCount = BB->getKnownExecutionCount();
      if (YamlBB.EventCount)
        YamlBF.Blocks.emplace_back(YamlBB);
      continue;
    }

    YamlBB.ExecCount = BB->getKnownExecutionCount();

    for (const auto &Instr : *BB) {
      if (!BC.MIB->isCall(Instr) && !BC.MIB->isIndirectBranch(Instr))
        continue;

      yaml::bolt::CallSiteInfo CSI;
      auto Offset = BC.MIB->tryGetAnnotationAs<uint32_t>(Instr, "Offset");
      if (!Offset || Offset.get() < BB->getInputOffset())
        continue;
      CSI.Offset = Offset.get() - BB->getInputOffset();

      if (BC.MIB->isIndirectCall(Instr) || BC.MIB->isIndirectBranch(Instr)) {
        auto ICSP =
          BC.MIB->tryGetAnnotationAs<IndirectCallSiteProfile>(Instr,
                                                              "CallProfile");
        if (!ICSP)
          continue;
        for (auto &CSP : ICSP.get()) {
          CSI.DestId = 0; // designated for unknown functions
          CSI.EntryDiscriminator = 0;
          if (CSP.Symbol) {
            const auto *Callee = BC.getFunctionForSymbol(CSP.Symbol);
            if (Callee) {
              CSI.DestId = Callee->getFunctionNumber();
            }
          }
          CSI.Count = CSP.Count;
          CSI.Mispreds = CSP.Mispreds;
          YamlBB.CallSites.push_back(CSI);
        }
      } else { // direct call or a tail call
        uint64_t EntryID{0};
        const auto *CalleeSymbol = BC.MIB->getTargetSymbol(Instr);
        const auto Callee = BC.getFunctionForSymbol(CalleeSymbol, &EntryID);
        if (Callee) {
          CSI.DestId = Callee->getFunctionNumber();;
          CSI.EntryDiscriminator = EntryID;
        }

        if (BC.MIB->getConditionalTailCall(Instr)) {
          auto CTCCount =
            BC.MIB->tryGetAnnotationAs<uint64_t>(Instr, "CTCTakenCount");
          if (CTCCount) {
            CSI.Count = *CTCCount;
            auto CTCMispreds =
              BC.MIB->tryGetAnnotationAs<uint64_t>(Instr, "CTCMispredCount");
            if (CTCMispreds)
              CSI.Mispreds = *CTCMispreds;
          }
        } else {
          auto Count = BC.MIB->tryGetAnnotationAs<uint64_t>(Instr, "Count");
          if (Count)
            CSI.Count = *Count;
        }

        if (CSI.Count)
          YamlBB.CallSites.emplace_back(CSI);
      }
    }

    std::sort(YamlBB.CallSites.begin(), YamlBB.CallSites.end());

    // Skip printing if there's no profile data for non-entry basic block.
    // Include landing pads with non-zero execution count.
    if (YamlBB.CallSites.empty() &&
        !BB->isEntryPoint() &&
        !(BB->isLandingPad() && BB->getKnownExecutionCount() != 0)) {
      uint64_t SuccessorExecCount = 0;
      for (auto &BranchInfo : BB->branch_info()) {
        SuccessorExecCount += BranchInfo.Count;
      }
      if (!SuccessorExecCount)
        continue;
    }

    auto BranchInfo = BB->branch_info_begin();
    for (const auto *Successor : BB->successors()) {
      yaml::bolt::SuccessorInfo YamlSI;
      YamlSI.Index = Successor->getLayoutIndex();
      YamlSI.Count = BranchInfo->Count;
      YamlSI.Mispreds = BranchInfo->MispredictedCount;

      YamlBB.Successors.emplace_back(YamlSI);

      ++BranchInfo;
    }

    YamlBF.Blocks.emplace_back(YamlBB);
  }
}
} // end anonymous namespace

std::error_code
YAMLProfileWriter::writeProfile(const RewriteInstance &RI) {
  auto &BC = RI.getBinaryContext();
  const auto &Functions = BC.getBinaryFunctions();

  std::error_code EC;
  OS = llvm::make_unique<raw_fd_ostream>(Filename, EC, sys::fs::F_None);
  if (EC) {
    errs() << "BOLT-WARNING: " << EC.message() << " : unable to open "
           << Filename << " for output.\n";
    return EC;
  }

  yaml::bolt::BinaryProfile BP;

  // Fill out the header info.
  BP.Header.Version = 1;
  BP.Header.FileName = BC.getFilename();
  auto BuildID = BC.getFileBuildID();
  BP.Header.Id = BuildID ? *BuildID : "<unknown>";
  BP.Header.Origin = RI.getProfileReader()->getReaderName();

  auto EventNames = RI.getProfileReader()->getEventNames();
  if (!EventNames.empty()) {
    std::string Sep = "";
    for (const auto &EventEntry : EventNames) {
      BP.Header.EventNames += Sep + EventEntry.first().str();
      Sep = ",";
    }
  }

  // Make sure the profile is consistent across all functions.
  uint16_t ProfileFlags = BinaryFunction::PF_NONE;
  for (const auto &BFI : Functions) {
    const auto &BF = BFI.second;
    if (BF.hasProfile() && !BF.empty()) {
      assert(BF.getProfileFlags() != BinaryFunction::PF_NONE);
      if (ProfileFlags == BinaryFunction::PF_NONE) {
        ProfileFlags = BF.getProfileFlags();
      }
      assert(BF.getProfileFlags() == ProfileFlags &&
             "expected consistent profile flags across all functions");
    }
  }
  BP.Header.Flags = ProfileFlags;

  // Add all function objects.
  for (const auto &BFI : Functions) {
    const auto &BF = BFI.second;
    if (BF.hasProfile()) {
      if (!BF.hasValidProfile() && !RI.getProfileReader()->isTrustedSource())
        continue;

      yaml::bolt::BinaryFunctionProfile YamlBF;
      convert(BF, YamlBF);
      BP.Functions.emplace_back(YamlBF);
    }
  }

  // Write the profile.
  yaml::Output Out(*OS, nullptr, 0);
  Out << BP;

  return std::error_code();
}

} // namespace bolt
} // namespace llvm
