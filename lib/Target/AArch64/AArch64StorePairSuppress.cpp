//===--- AArch64StorePairSuppress.cpp --- Suppress store pair formation ---===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This pass identifies floating point stores that should not be combined into
// store pairs. Later we may do the same for floating point loads.
// ===---------------------------------------------------------------------===//

#include "AArch64InstrInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineTraceMetrics.h"
#include "llvm/CodeGen/TargetSchedule.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetInstrInfo.h"

using namespace llvm;

#define DEBUG_TYPE "aarch64-stp-suppress"

namespace {
class AArch64StorePairSuppress : public MachineFunctionPass {
  const AArch64InstrInfo *TII;
  const TargetRegisterInfo *TRI;
  const MachineRegisterInfo *MRI;
  MachineFunction *MF;
  TargetSchedModel SchedModel;
  MachineTraceMetrics *Traces;
  MachineTraceMetrics::Ensemble *MinInstr;

public:
  static char ID;
  AArch64StorePairSuppress() : MachineFunctionPass(ID) {}

  const char *getPassName() const override {
    return "AArch64 Store Pair Suppression";
  }

  bool runOnMachineFunction(MachineFunction &F) override;

private:
  bool shouldAddSTPToBlock(const MachineBasicBlock *BB);

  bool isNarrowFPStore(const MachineInstr &MI);

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    AU.addRequired<MachineTraceMetrics>();
    AU.addPreserved<MachineTraceMetrics>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};
char AArch64StorePairSuppress::ID = 0;
} // anonymous

FunctionPass *llvm::createAArch64StorePairSuppressPass() {
  return new AArch64StorePairSuppress();
}

/// Return true if an STP can be added to this block without increasing the
/// critical resource height. STP is good to form in Ld/St limited blocks and
/// bad to form in float-point limited blocks. This is true independent of the
/// critical path. If the critical path is longer than the resource height, the
/// extra vector ops can limit physreg renaming. Otherwise, it could simply
/// oversaturate the vector units.
bool AArch64StorePairSuppress::shouldAddSTPToBlock(const MachineBasicBlock *BB) {
  if (!MinInstr)
    MinInstr = Traces->getEnsemble(MachineTraceMetrics::TS_MinInstrCount);

  MachineTraceMetrics::Trace BBTrace = MinInstr->getTrace(BB);
  unsigned ResLength = BBTrace.getResourceLength();

  // Get the machine model's scheduling class for STPQi.
  // Bypass TargetSchedule's SchedClass resolution since we only have an opcode.
  unsigned SCIdx = TII->get(AArch64::STPDi).getSchedClass();
  const MCSchedClassDesc *SCDesc =
      SchedModel.getMCSchedModel()->getSchedClassDesc(SCIdx);

  // If a subtarget does not define resources for STPQi, bail here.
  if (SCDesc->isValid() && !SCDesc->isVariant()) {
    unsigned ResLenWithSTP = BBTrace.getResourceLength(None, SCDesc);
    if (ResLenWithSTP > ResLength) {
      DEBUG(dbgs() << "  Suppress STP in BB: " << BB->getNumber()
                   << " resources " << ResLength << " -> " << ResLenWithSTP
                   << "\n");
      return false;
    }
  }
  return true;
}

/// Return true if this is a floating-point store smaller than the V reg. On
/// cyclone, these require a vector shuffle before storing a pair.
/// Ideally we would call getMatchingPairOpcode() and have the machine model
/// tell us if it's profitable with no cpu knowledge here.
///
/// FIXME: We plan to develop a decent Target abstraction for simple loads and
/// stores. Until then use a nasty switch similar to AArch64LoadStoreOptimizer.
bool AArch64StorePairSuppress::isNarrowFPStore(const MachineInstr &MI) {
  switch (MI.getOpcode()) {
  default:
    return false;
  case AArch64::STRSui:
  case AArch64::STRDui:
  case AArch64::STURSi:
  case AArch64::STURDi:
    return true;
  }
}

bool AArch64StorePairSuppress::runOnMachineFunction(MachineFunction &mf) {
  MF = &mf;
  TII =
      static_cast<const AArch64InstrInfo *>(MF->getSubtarget().getInstrInfo());
  TRI = MF->getSubtarget().getRegisterInfo();
  MRI = &MF->getRegInfo();
  const TargetSubtargetInfo &ST =
      MF->getTarget().getSubtarget<TargetSubtargetInfo>();
  SchedModel.init(*ST.getSchedModel(), &ST, TII);

  Traces = &getAnalysis<MachineTraceMetrics>();
  MinInstr = nullptr;

  DEBUG(dbgs() << "*** " << getPassName() << ": " << MF->getName() << '\n');

  if (!SchedModel.hasInstrSchedModel()) {
    DEBUG(dbgs() << "  Skipping pass: no machine model present.\n");
    return false;
  }

  // Check for a sequence of stores to the same base address. We don't need to
  // precisely determine whether a store pair can be formed. But we do want to
  // filter out most situations where we can't form store pairs to avoid
  // computing trace metrics in those cases.
  for (auto &MBB : *MF) {
    bool SuppressSTP = false;
    unsigned PrevBaseReg = 0;
    for (auto &MI : MBB) {
      if (!isNarrowFPStore(MI))
        continue;
      unsigned BaseReg;
      unsigned Offset;
      if (TII->getLdStBaseRegImmOfs(&MI, BaseReg, Offset, TRI)) {
        if (PrevBaseReg == BaseReg) {
          // If this block can take STPs, skip ahead to the next block.
          if (!SuppressSTP && shouldAddSTPToBlock(MI.getParent()))
            break;
          // Otherwise, continue unpairing the stores in this block.
          DEBUG(dbgs() << "Unpairing store " << MI << "\n");
          SuppressSTP = true;
          TII->suppressLdStPair(&MI);
        }
        PrevBaseReg = BaseReg;
      } else
        PrevBaseReg = 0;
    }
  }
  // This pass just sets some internal MachineMemOperand flags. It can't really
  // invalidate anything.
  return false;
}
