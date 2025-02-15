//===-- X86ISelLowering.cpp - X86 DAG Lowering Implementation -------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the interfaces that X86 uses to lower LLVM code into a
// selection DAG.
//
//===----------------------------------------------------------------------===//

#include "X86ISelLowering.h"
#include "Utils/X86ShuffleDecode.h"
#include "X86CallingConv.h"
#include "X86InstrBuilder.h"
#include "X86MachineFunctionInfo.h"
#include "X86TargetMachine.h"
#include "X86TargetObjectFile.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/ADT/VariadicFunction.h"
#include "llvm/CodeGen/IntrinsicLowering.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineJumpTableInfo.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/IR/CallSite.h"
#include "llvm/IR/CallingConv.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalAlias.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Target/TargetOptions.h"
#include "X86IntrinsicsInfo.h"
#include <bitset>
#include <numeric>
#include <cctype>
using namespace llvm;

#define DEBUG_TYPE "x86-isel"

STATISTIC(NumTailCalls, "Number of tail calls");

static cl::opt<bool> ExperimentalVectorWideningLegalization(
    "x86-experimental-vector-widening-legalization", cl::init(false),
    cl::desc("Enable an experimental vector type legalization through widening "
             "rather than promotion."),
    cl::Hidden);

static cl::opt<bool> ExperimentalVectorShuffleLowering(
    "x86-experimental-vector-shuffle-lowering", cl::init(false),
    cl::desc("Enable an experimental vector shuffle lowering code path."),
    cl::Hidden);

// Forward declarations.
static SDValue getMOVL(SelectionDAG &DAG, SDLoc dl, EVT VT, SDValue V1,
                       SDValue V2);

static SDValue ExtractSubVector(SDValue Vec, unsigned IdxVal,
                                SelectionDAG &DAG, SDLoc dl,
                                unsigned vectorWidth) {
  assert((vectorWidth == 128 || vectorWidth == 256) &&
         "Unsupported vector width");
  EVT VT = Vec.getValueType();
  EVT ElVT = VT.getVectorElementType();
  unsigned Factor = VT.getSizeInBits()/vectorWidth;
  EVT ResultVT = EVT::getVectorVT(*DAG.getContext(), ElVT,
                                  VT.getVectorNumElements()/Factor);

  // Extract from UNDEF is UNDEF.
  if (Vec.getOpcode() == ISD::UNDEF)
    return DAG.getUNDEF(ResultVT);

  // Extract the relevant vectorWidth bits.  Generate an EXTRACT_SUBVECTOR
  unsigned ElemsPerChunk = vectorWidth / ElVT.getSizeInBits();

  // This is the index of the first element of the vectorWidth-bit chunk
  // we want.
  unsigned NormalizedIdxVal = (((IdxVal * ElVT.getSizeInBits()) / vectorWidth)
                               * ElemsPerChunk);

  // If the input is a buildvector just emit a smaller one.
  if (Vec.getOpcode() == ISD::BUILD_VECTOR)
    return DAG.getNode(ISD::BUILD_VECTOR, dl, ResultVT,
                       makeArrayRef(Vec->op_begin()+NormalizedIdxVal,
                                    ElemsPerChunk));

  SDValue VecIdx = DAG.getIntPtrConstant(NormalizedIdxVal);
  SDValue Result = DAG.getNode(ISD::EXTRACT_SUBVECTOR, dl, ResultVT, Vec,
                               VecIdx);

  return Result;

}
/// Generate a DAG to grab 128-bits from a vector > 128 bits.  This
/// sets things up to match to an AVX VEXTRACTF128 / VEXTRACTI128
/// or AVX-512 VEXTRACTF32x4 / VEXTRACTI32x4
/// instructions or a simple subregister reference. Idx is an index in the
/// 128 bits we want.  It need not be aligned to a 128-bit bounday.  That makes
/// lowering EXTRACT_VECTOR_ELT operations easier.
static SDValue Extract128BitVector(SDValue Vec, unsigned IdxVal,
                                   SelectionDAG &DAG, SDLoc dl) {
  assert((Vec.getValueType().is256BitVector() ||
          Vec.getValueType().is512BitVector()) && "Unexpected vector size!");
  return ExtractSubVector(Vec, IdxVal, DAG, dl, 128);
}

/// Generate a DAG to grab 256-bits from a 512-bit vector.
static SDValue Extract256BitVector(SDValue Vec, unsigned IdxVal,
                                   SelectionDAG &DAG, SDLoc dl) {
  assert(Vec.getValueType().is512BitVector() && "Unexpected vector size!");
  return ExtractSubVector(Vec, IdxVal, DAG, dl, 256);
}

static SDValue InsertSubVector(SDValue Result, SDValue Vec,
                               unsigned IdxVal, SelectionDAG &DAG,
                               SDLoc dl, unsigned vectorWidth) {
  assert((vectorWidth == 128 || vectorWidth == 256) &&
         "Unsupported vector width");
  // Inserting UNDEF is Result
  if (Vec.getOpcode() == ISD::UNDEF)
    return Result;
  EVT VT = Vec.getValueType();
  EVT ElVT = VT.getVectorElementType();
  EVT ResultVT = Result.getValueType();

  // Insert the relevant vectorWidth bits.
  unsigned ElemsPerChunk = vectorWidth/ElVT.getSizeInBits();

  // This is the index of the first element of the vectorWidth-bit chunk
  // we want.
  unsigned NormalizedIdxVal = (((IdxVal * ElVT.getSizeInBits())/vectorWidth)
                               * ElemsPerChunk);

  SDValue VecIdx = DAG.getIntPtrConstant(NormalizedIdxVal);
  return DAG.getNode(ISD::INSERT_SUBVECTOR, dl, ResultVT, Result, Vec,
                     VecIdx);
}
/// Generate a DAG to put 128-bits into a vector > 128 bits.  This
/// sets things up to match to an AVX VINSERTF128/VINSERTI128 or
/// AVX-512 VINSERTF32x4/VINSERTI32x4 instructions or a
/// simple superregister reference.  Idx is an index in the 128 bits
/// we want.  It need not be aligned to a 128-bit bounday.  That makes
/// lowering INSERT_VECTOR_ELT operations easier.
static SDValue Insert128BitVector(SDValue Result, SDValue Vec,
                                  unsigned IdxVal, SelectionDAG &DAG,
                                  SDLoc dl) {
  assert(Vec.getValueType().is128BitVector() && "Unexpected vector size!");
  return InsertSubVector(Result, Vec, IdxVal, DAG, dl, 128);
}

static SDValue Insert256BitVector(SDValue Result, SDValue Vec,
                                  unsigned IdxVal, SelectionDAG &DAG,
                                  SDLoc dl) {
  assert(Vec.getValueType().is256BitVector() && "Unexpected vector size!");
  return InsertSubVector(Result, Vec, IdxVal, DAG, dl, 256);
}

/// Concat two 128-bit vectors into a 256 bit vector using VINSERTF128
/// instructions. This is used because creating CONCAT_VECTOR nodes of
/// BUILD_VECTORS returns a larger BUILD_VECTOR while we're trying to lower
/// large BUILD_VECTORS.
static SDValue Concat128BitVectors(SDValue V1, SDValue V2, EVT VT,
                                   unsigned NumElems, SelectionDAG &DAG,
                                   SDLoc dl) {
  SDValue V = Insert128BitVector(DAG.getUNDEF(VT), V1, 0, DAG, dl);
  return Insert128BitVector(V, V2, NumElems/2, DAG, dl);
}

static SDValue Concat256BitVectors(SDValue V1, SDValue V2, EVT VT,
                                   unsigned NumElems, SelectionDAG &DAG,
                                   SDLoc dl) {
  SDValue V = Insert256BitVector(DAG.getUNDEF(VT), V1, 0, DAG, dl);
  return Insert256BitVector(V, V2, NumElems/2, DAG, dl);
}

static TargetLoweringObjectFile *createTLOF(const Triple &TT) {
  if (TT.isOSBinFormatMachO()) {
    if (TT.getArch() == Triple::x86_64)
      return new X86_64MachoTargetObjectFile();
    return new TargetLoweringObjectFileMachO();
  }

  if (TT.isOSLinux())
    return new X86LinuxTargetObjectFile();
  if (TT.isOSBinFormatELF())
    return new TargetLoweringObjectFileELF();
  if (TT.isKnownWindowsMSVCEnvironment())
    return new X86WindowsTargetObjectFile();
  if (TT.isOSBinFormatCOFF())
    return new TargetLoweringObjectFileCOFF();
  llvm_unreachable("unknown subtarget type");
}

// FIXME: This should stop caching the target machine as soon as
// we can remove resetOperationActions et al.
X86TargetLowering::X86TargetLowering(X86TargetMachine &TM)
  : TargetLowering(TM, createTLOF(Triple(TM.getTargetTriple()))) {
  Subtarget = &TM.getSubtarget<X86Subtarget>();
  X86ScalarSSEf64 = Subtarget->hasSSE2();
  X86ScalarSSEf32 = Subtarget->hasSSE1();
  TD = getDataLayout();

  resetOperationActions();
}

void X86TargetLowering::resetOperationActions() {
  const TargetMachine &TM = getTargetMachine();
  static bool FirstTimeThrough = true;

  // If none of the target options have changed, then we don't need to reset the
  // operation actions.
  if (!FirstTimeThrough && TO == TM.Options) return;

  if (!FirstTimeThrough) {
    // Reinitialize the actions.
    initActions();
    FirstTimeThrough = false;
  }

  TO = TM.Options;

  // Set up the TargetLowering object.
  static const MVT IntVTs[] = { MVT::i8, MVT::i16, MVT::i32, MVT::i64 };

  // X86 is weird, it always uses i8 for shift amounts and setcc results.
  setBooleanContents(ZeroOrOneBooleanContent);
  // X86-SSE is even stranger. It uses -1 or 0 for vector masks.
  setBooleanVectorContents(ZeroOrNegativeOneBooleanContent);

  // For 64-bit since we have so many registers use the ILP scheduler, for
  // 32-bit code use the register pressure specific scheduling.
  // For Atom, always use ILP scheduling.
  if (Subtarget->isAtom())
    setSchedulingPreference(Sched::ILP);
  else if (Subtarget->is64Bit())
    setSchedulingPreference(Sched::ILP);
  else
    setSchedulingPreference(Sched::RegPressure);
  const X86RegisterInfo *RegInfo =
      TM.getSubtarget<X86Subtarget>().getRegisterInfo();
  setStackPointerRegisterToSaveRestore(RegInfo->getStackRegister());

  // Bypass expensive divides on Atom when compiling with O2
  if (Subtarget->hasSlowDivide() && TM.getOptLevel() >= CodeGenOpt::Default) {
    addBypassSlowDiv(32, 8);
    if (Subtarget->is64Bit())
      addBypassSlowDiv(64, 16);
  }

  if (Subtarget->isTargetKnownWindowsMSVC()) {
    // Setup Windows compiler runtime calls.
    setLibcallName(RTLIB::SDIV_I64, "_alldiv");
    setLibcallName(RTLIB::UDIV_I64, "_aulldiv");
    setLibcallName(RTLIB::SREM_I64, "_allrem");
    setLibcallName(RTLIB::UREM_I64, "_aullrem");
    setLibcallName(RTLIB::MUL_I64, "_allmul");
    setLibcallCallingConv(RTLIB::SDIV_I64, CallingConv::X86_StdCall);
    setLibcallCallingConv(RTLIB::UDIV_I64, CallingConv::X86_StdCall);
    setLibcallCallingConv(RTLIB::SREM_I64, CallingConv::X86_StdCall);
    setLibcallCallingConv(RTLIB::UREM_I64, CallingConv::X86_StdCall);
    setLibcallCallingConv(RTLIB::MUL_I64, CallingConv::X86_StdCall);

    // The _ftol2 runtime function has an unusual calling conv, which
    // is modeled by a special pseudo-instruction.
    setLibcallName(RTLIB::FPTOUINT_F64_I64, nullptr);
    setLibcallName(RTLIB::FPTOUINT_F32_I64, nullptr);
    setLibcallName(RTLIB::FPTOUINT_F64_I32, nullptr);
    setLibcallName(RTLIB::FPTOUINT_F32_I32, nullptr);
  }

  if (Subtarget->isTargetDarwin()) {
    // Darwin should use _setjmp/_longjmp instead of setjmp/longjmp.
    setUseUnderscoreSetJmp(false);
    setUseUnderscoreLongJmp(false);
  } else if (Subtarget->isTargetWindowsGNU()) {
    // MS runtime is weird: it exports _setjmp, but longjmp!
    setUseUnderscoreSetJmp(true);
    setUseUnderscoreLongJmp(false);
  } else {
    setUseUnderscoreSetJmp(true);
    setUseUnderscoreLongJmp(true);
  }

  // Set up the register classes.
  addRegisterClass(MVT::i8, &X86::GR8RegClass);
  addRegisterClass(MVT::i16, &X86::GR16RegClass);
  addRegisterClass(MVT::i32, &X86::GR32RegClass);
  if (Subtarget->is64Bit())
    addRegisterClass(MVT::i64, &X86::GR64RegClass);

  setLoadExtAction(ISD::SEXTLOAD, MVT::i1, Promote);

  // We don't accept any truncstore of integer registers.
  setTruncStoreAction(MVT::i64, MVT::i32, Expand);
  setTruncStoreAction(MVT::i64, MVT::i16, Expand);
  setTruncStoreAction(MVT::i64, MVT::i8 , Expand);
  setTruncStoreAction(MVT::i32, MVT::i16, Expand);
  setTruncStoreAction(MVT::i32, MVT::i8 , Expand);
  setTruncStoreAction(MVT::i16, MVT::i8,  Expand);

  // SETOEQ and SETUNE require checking two conditions.
  setCondCodeAction(ISD::SETOEQ, MVT::f32, Expand);
  setCondCodeAction(ISD::SETOEQ, MVT::f64, Expand);
  setCondCodeAction(ISD::SETOEQ, MVT::f80, Expand);
  setCondCodeAction(ISD::SETUNE, MVT::f32, Expand);
  setCondCodeAction(ISD::SETUNE, MVT::f64, Expand);
  setCondCodeAction(ISD::SETUNE, MVT::f80, Expand);

  // Promote all UINT_TO_FP to larger SINT_TO_FP's, as X86 doesn't have this
  // operation.
  setOperationAction(ISD::UINT_TO_FP       , MVT::i1   , Promote);
  setOperationAction(ISD::UINT_TO_FP       , MVT::i8   , Promote);
  setOperationAction(ISD::UINT_TO_FP       , MVT::i16  , Promote);

  if (Subtarget->is64Bit()) {
    setOperationAction(ISD::UINT_TO_FP     , MVT::i32  , Promote);
    setOperationAction(ISD::UINT_TO_FP     , MVT::i64  , Custom);
  } else if (!TM.Options.UseSoftFloat) {
    // We have an algorithm for SSE2->double, and we turn this into a
    // 64-bit FILD followed by conditional FADD for other targets.
    setOperationAction(ISD::UINT_TO_FP     , MVT::i64  , Custom);
    // We have an algorithm for SSE2, and we turn this into a 64-bit
    // FILD for other targets.
    setOperationAction(ISD::UINT_TO_FP     , MVT::i32  , Custom);
  }

  // Promote i1/i8 SINT_TO_FP to larger SINT_TO_FP's, as X86 doesn't have
  // this operation.
  setOperationAction(ISD::SINT_TO_FP       , MVT::i1   , Promote);
  setOperationAction(ISD::SINT_TO_FP       , MVT::i8   , Promote);

  if (!TM.Options.UseSoftFloat) {
    // SSE has no i16 to fp conversion, only i32
    if (X86ScalarSSEf32) {
      setOperationAction(ISD::SINT_TO_FP     , MVT::i16  , Promote);
      // f32 and f64 cases are Legal, f80 case is not
      setOperationAction(ISD::SINT_TO_FP     , MVT::i32  , Custom);
    } else {
      setOperationAction(ISD::SINT_TO_FP     , MVT::i16  , Custom);
      setOperationAction(ISD::SINT_TO_FP     , MVT::i32  , Custom);
    }
  } else {
    setOperationAction(ISD::SINT_TO_FP     , MVT::i16  , Promote);
    setOperationAction(ISD::SINT_TO_FP     , MVT::i32  , Promote);
  }

  // In 32-bit mode these are custom lowered.  In 64-bit mode F32 and F64
  // are Legal, f80 is custom lowered.
  setOperationAction(ISD::FP_TO_SINT     , MVT::i64  , Custom);
  setOperationAction(ISD::SINT_TO_FP     , MVT::i64  , Custom);

  // Promote i1/i8 FP_TO_SINT to larger FP_TO_SINTS's, as X86 doesn't have
  // this operation.
  setOperationAction(ISD::FP_TO_SINT       , MVT::i1   , Promote);
  setOperationAction(ISD::FP_TO_SINT       , MVT::i8   , Promote);

  if (X86ScalarSSEf32) {
    setOperationAction(ISD::FP_TO_SINT     , MVT::i16  , Promote);
    // f32 and f64 cases are Legal, f80 case is not
    setOperationAction(ISD::FP_TO_SINT     , MVT::i32  , Custom);
  } else {
    setOperationAction(ISD::FP_TO_SINT     , MVT::i16  , Custom);
    setOperationAction(ISD::FP_TO_SINT     , MVT::i32  , Custom);
  }

  // Handle FP_TO_UINT by promoting the destination to a larger signed
  // conversion.
  setOperationAction(ISD::FP_TO_UINT       , MVT::i1   , Promote);
  setOperationAction(ISD::FP_TO_UINT       , MVT::i8   , Promote);
  setOperationAction(ISD::FP_TO_UINT       , MVT::i16  , Promote);

  if (Subtarget->is64Bit()) {
    setOperationAction(ISD::FP_TO_UINT     , MVT::i64  , Expand);
    setOperationAction(ISD::FP_TO_UINT     , MVT::i32  , Promote);
  } else if (!TM.Options.UseSoftFloat) {
    // Since AVX is a superset of SSE3, only check for SSE here.
    if (Subtarget->hasSSE1() && !Subtarget->hasSSE3())
      // Expand FP_TO_UINT into a select.
      // FIXME: We would like to use a Custom expander here eventually to do
      // the optimal thing for SSE vs. the default expansion in the legalizer.
      setOperationAction(ISD::FP_TO_UINT   , MVT::i32  , Expand);
    else
      // With SSE3 we can use fisttpll to convert to a signed i64; without
      // SSE, we're stuck with a fistpll.
      setOperationAction(ISD::FP_TO_UINT   , MVT::i32  , Custom);
  }

  if (isTargetFTOL()) {
    // Use the _ftol2 runtime function, which has a pseudo-instruction
    // to handle its weird calling convention.
    setOperationAction(ISD::FP_TO_UINT     , MVT::i64  , Custom);
  }

  // TODO: when we have SSE, these could be more efficient, by using movd/movq.
  if (!X86ScalarSSEf64) {
    setOperationAction(ISD::BITCAST        , MVT::f32  , Expand);
    setOperationAction(ISD::BITCAST        , MVT::i32  , Expand);
    if (Subtarget->is64Bit()) {
      setOperationAction(ISD::BITCAST      , MVT::f64  , Expand);
      // Without SSE, i64->f64 goes through memory.
      setOperationAction(ISD::BITCAST      , MVT::i64  , Expand);
    }
  }

  // Scalar integer divide and remainder are lowered to use operations that
  // produce two results, to match the available instructions. This exposes
  // the two-result form to trivial CSE, which is able to combine x/y and x%y
  // into a single instruction.
  //
  // Scalar integer multiply-high is also lowered to use two-result
  // operations, to match the available instructions. However, plain multiply
  // (low) operations are left as Legal, as there are single-result
  // instructions for this in x86. Using the two-result multiply instructions
  // when both high and low results are needed must be arranged by dagcombine.
  for (unsigned i = 0; i != array_lengthof(IntVTs); ++i) {
    MVT VT = IntVTs[i];
    setOperationAction(ISD::MULHS, VT, Expand);
    setOperationAction(ISD::MULHU, VT, Expand);
    setOperationAction(ISD::SDIV, VT, Expand);
    setOperationAction(ISD::UDIV, VT, Expand);
    setOperationAction(ISD::SREM, VT, Expand);
    setOperationAction(ISD::UREM, VT, Expand);

    // Add/Sub overflow ops with MVT::Glues are lowered to EFLAGS dependences.
    setOperationAction(ISD::ADDC, VT, Custom);
    setOperationAction(ISD::ADDE, VT, Custom);
    setOperationAction(ISD::SUBC, VT, Custom);
    setOperationAction(ISD::SUBE, VT, Custom);
  }

  setOperationAction(ISD::BR_JT            , MVT::Other, Expand);
  setOperationAction(ISD::BRCOND           , MVT::Other, Custom);
  setOperationAction(ISD::BR_CC            , MVT::f32,   Expand);
  setOperationAction(ISD::BR_CC            , MVT::f64,   Expand);
  setOperationAction(ISD::BR_CC            , MVT::f80,   Expand);
  setOperationAction(ISD::BR_CC            , MVT::i8,    Expand);
  setOperationAction(ISD::BR_CC            , MVT::i16,   Expand);
  setOperationAction(ISD::BR_CC            , MVT::i32,   Expand);
  setOperationAction(ISD::BR_CC            , MVT::i64,   Expand);
  setOperationAction(ISD::SELECT_CC        , MVT::f32,   Expand);
  setOperationAction(ISD::SELECT_CC        , MVT::f64,   Expand);
  setOperationAction(ISD::SELECT_CC        , MVT::f80,   Expand);
  setOperationAction(ISD::SELECT_CC        , MVT::i8,    Expand);
  setOperationAction(ISD::SELECT_CC        , MVT::i16,   Expand);
  setOperationAction(ISD::SELECT_CC        , MVT::i32,   Expand);
  setOperationAction(ISD::SELECT_CC        , MVT::i64,   Expand);
  if (Subtarget->is64Bit())
    setOperationAction(ISD::SIGN_EXTEND_INREG, MVT::i32, Legal);
  setOperationAction(ISD::SIGN_EXTEND_INREG, MVT::i16  , Legal);
  setOperationAction(ISD::SIGN_EXTEND_INREG, MVT::i8   , Legal);
  setOperationAction(ISD::SIGN_EXTEND_INREG, MVT::i1   , Expand);
  setOperationAction(ISD::FP_ROUND_INREG   , MVT::f32  , Expand);
  setOperationAction(ISD::FREM             , MVT::f32  , Expand);
  setOperationAction(ISD::FREM             , MVT::f64  , Expand);
  setOperationAction(ISD::FREM             , MVT::f80  , Expand);
  setOperationAction(ISD::FLT_ROUNDS_      , MVT::i32  , Custom);

  // Promote the i8 variants and force them on up to i32 which has a shorter
  // encoding.
  setOperationAction(ISD::CTTZ             , MVT::i8   , Promote);
  AddPromotedToType (ISD::CTTZ             , MVT::i8   , MVT::i32);
  setOperationAction(ISD::CTTZ_ZERO_UNDEF  , MVT::i8   , Promote);
  AddPromotedToType (ISD::CTTZ_ZERO_UNDEF  , MVT::i8   , MVT::i32);
  if (Subtarget->hasBMI()) {
    setOperationAction(ISD::CTTZ_ZERO_UNDEF, MVT::i16  , Expand);
    setOperationAction(ISD::CTTZ_ZERO_UNDEF, MVT::i32  , Expand);
    if (Subtarget->is64Bit())
      setOperationAction(ISD::CTTZ_ZERO_UNDEF, MVT::i64, Expand);
  } else {
    setOperationAction(ISD::CTTZ           , MVT::i16  , Custom);
    setOperationAction(ISD::CTTZ           , MVT::i32  , Custom);
    if (Subtarget->is64Bit())
      setOperationAction(ISD::CTTZ         , MVT::i64  , Custom);
  }

  if (Subtarget->hasLZCNT()) {
    // When promoting the i8 variants, force them to i32 for a shorter
    // encoding.
    setOperationAction(ISD::CTLZ           , MVT::i8   , Promote);
    AddPromotedToType (ISD::CTLZ           , MVT::i8   , MVT::i32);
    setOperationAction(ISD::CTLZ_ZERO_UNDEF, MVT::i8   , Promote);
    AddPromotedToType (ISD::CTLZ_ZERO_UNDEF, MVT::i8   , MVT::i32);
    setOperationAction(ISD::CTLZ_ZERO_UNDEF, MVT::i16  , Expand);
    setOperationAction(ISD::CTLZ_ZERO_UNDEF, MVT::i32  , Expand);
    if (Subtarget->is64Bit())
      setOperationAction(ISD::CTLZ_ZERO_UNDEF, MVT::i64, Expand);
  } else {
    setOperationAction(ISD::CTLZ           , MVT::i8   , Custom);
    setOperationAction(ISD::CTLZ           , MVT::i16  , Custom);
    setOperationAction(ISD::CTLZ           , MVT::i32  , Custom);
    setOperationAction(ISD::CTLZ_ZERO_UNDEF, MVT::i8   , Custom);
    setOperationAction(ISD::CTLZ_ZERO_UNDEF, MVT::i16  , Custom);
    setOperationAction(ISD::CTLZ_ZERO_UNDEF, MVT::i32  , Custom);
    if (Subtarget->is64Bit()) {
      setOperationAction(ISD::CTLZ         , MVT::i64  , Custom);
      setOperationAction(ISD::CTLZ_ZERO_UNDEF, MVT::i64, Custom);
    }
  }

  // Special handling for half-precision floating point conversions.
  // If we don't have F16C support, then lower half float conversions
  // into library calls.
  if (TM.Options.UseSoftFloat || !Subtarget->hasF16C()) {
    setOperationAction(ISD::FP16_TO_FP, MVT::f32, Expand);
    setOperationAction(ISD::FP_TO_FP16, MVT::f32, Expand);
  }

  // There's never any support for operations beyond MVT::f32.
  setOperationAction(ISD::FP16_TO_FP, MVT::f64, Expand);
  setOperationAction(ISD::FP16_TO_FP, MVT::f80, Expand);
  setOperationAction(ISD::FP_TO_FP16, MVT::f64, Expand);
  setOperationAction(ISD::FP_TO_FP16, MVT::f80, Expand);

  setLoadExtAction(ISD::EXTLOAD, MVT::f16, Expand);
  setTruncStoreAction(MVT::f32, MVT::f16, Expand);
  setTruncStoreAction(MVT::f64, MVT::f16, Expand);
  setTruncStoreAction(MVT::f80, MVT::f16, Expand);

  if (Subtarget->hasPOPCNT()) {
    setOperationAction(ISD::CTPOP          , MVT::i8   , Promote);
  } else {
    setOperationAction(ISD::CTPOP          , MVT::i8   , Expand);
    setOperationAction(ISD::CTPOP          , MVT::i16  , Expand);
    setOperationAction(ISD::CTPOP          , MVT::i32  , Expand);
    if (Subtarget->is64Bit())
      setOperationAction(ISD::CTPOP        , MVT::i64  , Expand);
  }

  setOperationAction(ISD::READCYCLECOUNTER , MVT::i64  , Custom);

  if (!Subtarget->hasMOVBE())
    setOperationAction(ISD::BSWAP          , MVT::i16  , Expand);

  // These should be promoted to a larger select which is supported.
  setOperationAction(ISD::SELECT          , MVT::i1   , Promote);
  // X86 wants to expand cmov itself.
  setOperationAction(ISD::SELECT          , MVT::i8   , Custom);
  setOperationAction(ISD::SELECT          , MVT::i16  , Custom);
  setOperationAction(ISD::SELECT          , MVT::i32  , Custom);
  setOperationAction(ISD::SELECT          , MVT::f32  , Custom);
  setOperationAction(ISD::SELECT          , MVT::f64  , Custom);
  setOperationAction(ISD::SELECT          , MVT::f80  , Custom);
  setOperationAction(ISD::SETCC           , MVT::i8   , Custom);
  setOperationAction(ISD::SETCC           , MVT::i16  , Custom);
  setOperationAction(ISD::SETCC           , MVT::i32  , Custom);
  setOperationAction(ISD::SETCC           , MVT::f32  , Custom);
  setOperationAction(ISD::SETCC           , MVT::f64  , Custom);
  setOperationAction(ISD::SETCC           , MVT::f80  , Custom);
  if (Subtarget->is64Bit()) {
    setOperationAction(ISD::SELECT        , MVT::i64  , Custom);
    setOperationAction(ISD::SETCC         , MVT::i64  , Custom);
  }
  setOperationAction(ISD::EH_RETURN       , MVT::Other, Custom);
  // NOTE: EH_SJLJ_SETJMP/_LONGJMP supported here is NOT intended to support
  // SjLj exception handling but a light-weight setjmp/longjmp replacement to
  // support continuation, user-level threading, and etc.. As a result, no
  // other SjLj exception interfaces are implemented and please don't build
  // your own exception handling based on them.
  // LLVM/Clang supports zero-cost DWARF exception handling.
  setOperationAction(ISD::EH_SJLJ_SETJMP, MVT::i32, Custom);
  setOperationAction(ISD::EH_SJLJ_LONGJMP, MVT::Other, Custom);

  // Darwin ABI issue.
  setOperationAction(ISD::ConstantPool    , MVT::i32  , Custom);
  setOperationAction(ISD::JumpTable       , MVT::i32  , Custom);
  setOperationAction(ISD::GlobalAddress   , MVT::i32  , Custom);
  setOperationAction(ISD::GlobalTLSAddress, MVT::i32  , Custom);
  if (Subtarget->is64Bit())
    setOperationAction(ISD::GlobalTLSAddress, MVT::i64, Custom);
  setOperationAction(ISD::ExternalSymbol  , MVT::i32  , Custom);
  setOperationAction(ISD::BlockAddress    , MVT::i32  , Custom);
  if (Subtarget->is64Bit()) {
    setOperationAction(ISD::ConstantPool  , MVT::i64  , Custom);
    setOperationAction(ISD::JumpTable     , MVT::i64  , Custom);
    setOperationAction(ISD::GlobalAddress , MVT::i64  , Custom);
    setOperationAction(ISD::ExternalSymbol, MVT::i64  , Custom);
    setOperationAction(ISD::BlockAddress  , MVT::i64  , Custom);
  }
  // 64-bit addm sub, shl, sra, srl (iff 32-bit x86)
  setOperationAction(ISD::SHL_PARTS       , MVT::i32  , Custom);
  setOperationAction(ISD::SRA_PARTS       , MVT::i32  , Custom);
  setOperationAction(ISD::SRL_PARTS       , MVT::i32  , Custom);
  if (Subtarget->is64Bit()) {
    setOperationAction(ISD::SHL_PARTS     , MVT::i64  , Custom);
    setOperationAction(ISD::SRA_PARTS     , MVT::i64  , Custom);
    setOperationAction(ISD::SRL_PARTS     , MVT::i64  , Custom);
  }

  if (Subtarget->hasSSE1())
    setOperationAction(ISD::PREFETCH      , MVT::Other, Legal);

  setOperationAction(ISD::ATOMIC_FENCE  , MVT::Other, Custom);

  // Expand certain atomics
  for (unsigned i = 0; i != array_lengthof(IntVTs); ++i) {
    MVT VT = IntVTs[i];
    setOperationAction(ISD::ATOMIC_CMP_SWAP_WITH_SUCCESS, VT, Custom);
    setOperationAction(ISD::ATOMIC_LOAD_SUB, VT, Custom);
    setOperationAction(ISD::ATOMIC_STORE, VT, Custom);
  }

  if (Subtarget->hasCmpxchg16b()) {
    setOperationAction(ISD::ATOMIC_CMP_SWAP_WITH_SUCCESS, MVT::i128, Custom);
  }

  // FIXME - use subtarget debug flags
  if (!Subtarget->isTargetDarwin() && !Subtarget->isTargetELF() &&
      !Subtarget->isTargetCygMing() && !Subtarget->isTargetWin64()) {
    setOperationAction(ISD::EH_LABEL, MVT::Other, Expand);
  }

  if (Subtarget->is64Bit()) {
    setExceptionPointerRegister(X86::RAX);
    setExceptionSelectorRegister(X86::RDX);
  } else {
    setExceptionPointerRegister(X86::EAX);
    setExceptionSelectorRegister(X86::EDX);
  }
  setOperationAction(ISD::FRAME_TO_ARGS_OFFSET, MVT::i32, Custom);
  setOperationAction(ISD::FRAME_TO_ARGS_OFFSET, MVT::i64, Custom);

  setOperationAction(ISD::INIT_TRAMPOLINE, MVT::Other, Custom);
  setOperationAction(ISD::ADJUST_TRAMPOLINE, MVT::Other, Custom);

  setOperationAction(ISD::TRAP, MVT::Other, Legal);
  setOperationAction(ISD::DEBUGTRAP, MVT::Other, Legal);

  // VASTART needs to be custom lowered to use the VarArgsFrameIndex
  setOperationAction(ISD::VASTART           , MVT::Other, Custom);
  setOperationAction(ISD::VAEND             , MVT::Other, Expand);
  if (Subtarget->is64Bit() && !Subtarget->isTargetWin64()) {
    // TargetInfo::X86_64ABIBuiltinVaList
    setOperationAction(ISD::VAARG           , MVT::Other, Custom);
    setOperationAction(ISD::VACOPY          , MVT::Other, Custom);
  } else {
    // TargetInfo::CharPtrBuiltinVaList
    setOperationAction(ISD::VAARG           , MVT::Other, Expand);
    setOperationAction(ISD::VACOPY          , MVT::Other, Expand);
  }

  setOperationAction(ISD::STACKSAVE,          MVT::Other, Expand);
  setOperationAction(ISD::STACKRESTORE,       MVT::Other, Expand);

  setOperationAction(ISD::DYNAMIC_STACKALLOC, getPointerTy(), Custom);

  if (!TM.Options.UseSoftFloat && X86ScalarSSEf64) {
    // f32 and f64 use SSE.
    // Set up the FP register classes.
    addRegisterClass(MVT::f32, &X86::FR32RegClass);
    addRegisterClass(MVT::f64, &X86::FR64RegClass);

    // Use ANDPD to simulate FABS.
    setOperationAction(ISD::FABS , MVT::f64, Custom);
    setOperationAction(ISD::FABS , MVT::f32, Custom);

    // Use XORP to simulate FNEG.
    setOperationAction(ISD::FNEG , MVT::f64, Custom);
    setOperationAction(ISD::FNEG , MVT::f32, Custom);

    // Use ANDPD and ORPD to simulate FCOPYSIGN.
    setOperationAction(ISD::FCOPYSIGN, MVT::f64, Custom);
    setOperationAction(ISD::FCOPYSIGN, MVT::f32, Custom);

    // Lower this to FGETSIGNx86 plus an AND.
    setOperationAction(ISD::FGETSIGN, MVT::i64, Custom);
    setOperationAction(ISD::FGETSIGN, MVT::i32, Custom);

    // We don't support sin/cos/fmod
    setOperationAction(ISD::FSIN   , MVT::f64, Expand);
    setOperationAction(ISD::FCOS   , MVT::f64, Expand);
    setOperationAction(ISD::FSINCOS, MVT::f64, Expand);
    setOperationAction(ISD::FSIN   , MVT::f32, Expand);
    setOperationAction(ISD::FCOS   , MVT::f32, Expand);
    setOperationAction(ISD::FSINCOS, MVT::f32, Expand);

    // Expand FP immediates into loads from the stack, except for the special
    // cases we handle.
    addLegalFPImmediate(APFloat(+0.0)); // xorpd
    addLegalFPImmediate(APFloat(+0.0f)); // xorps
  } else if (!TM.Options.UseSoftFloat && X86ScalarSSEf32) {
    // Use SSE for f32, x87 for f64.
    // Set up the FP register classes.
    addRegisterClass(MVT::f32, &X86::FR32RegClass);
    addRegisterClass(MVT::f64, &X86::RFP64RegClass);

    // Use ANDPS to simulate FABS.
    setOperationAction(ISD::FABS , MVT::f32, Custom);

    // Use XORP to simulate FNEG.
    setOperationAction(ISD::FNEG , MVT::f32, Custom);

    setOperationAction(ISD::UNDEF,     MVT::f64, Expand);

    // Use ANDPS and ORPS to simulate FCOPYSIGN.
    setOperationAction(ISD::FCOPYSIGN, MVT::f64, Expand);
    setOperationAction(ISD::FCOPYSIGN, MVT::f32, Custom);

    // We don't support sin/cos/fmod
    setOperationAction(ISD::FSIN   , MVT::f32, Expand);
    setOperationAction(ISD::FCOS   , MVT::f32, Expand);
    setOperationAction(ISD::FSINCOS, MVT::f32, Expand);

    // Special cases we handle for FP constants.
    addLegalFPImmediate(APFloat(+0.0f)); // xorps
    addLegalFPImmediate(APFloat(+0.0)); // FLD0
    addLegalFPImmediate(APFloat(+1.0)); // FLD1
    addLegalFPImmediate(APFloat(-0.0)); // FLD0/FCHS
    addLegalFPImmediate(APFloat(-1.0)); // FLD1/FCHS

    if (!TM.Options.UnsafeFPMath) {
      setOperationAction(ISD::FSIN   , MVT::f64, Expand);
      setOperationAction(ISD::FCOS   , MVT::f64, Expand);
      setOperationAction(ISD::FSINCOS, MVT::f64, Expand);
    }
  } else if (!TM.Options.UseSoftFloat) {
    // f32 and f64 in x87.
    // Set up the FP register classes.
    addRegisterClass(MVT::f64, &X86::RFP64RegClass);
    addRegisterClass(MVT::f32, &X86::RFP32RegClass);

    setOperationAction(ISD::UNDEF,     MVT::f64, Expand);
    setOperationAction(ISD::UNDEF,     MVT::f32, Expand);
    setOperationAction(ISD::FCOPYSIGN, MVT::f64, Expand);
    setOperationAction(ISD::FCOPYSIGN, MVT::f32, Expand);

    if (!TM.Options.UnsafeFPMath) {
      setOperationAction(ISD::FSIN   , MVT::f64, Expand);
      setOperationAction(ISD::FSIN   , MVT::f32, Expand);
      setOperationAction(ISD::FCOS   , MVT::f64, Expand);
      setOperationAction(ISD::FCOS   , MVT::f32, Expand);
      setOperationAction(ISD::FSINCOS, MVT::f64, Expand);
      setOperationAction(ISD::FSINCOS, MVT::f32, Expand);
    }
    addLegalFPImmediate(APFloat(+0.0)); // FLD0
    addLegalFPImmediate(APFloat(+1.0)); // FLD1
    addLegalFPImmediate(APFloat(-0.0)); // FLD0/FCHS
    addLegalFPImmediate(APFloat(-1.0)); // FLD1/FCHS
    addLegalFPImmediate(APFloat(+0.0f)); // FLD0
    addLegalFPImmediate(APFloat(+1.0f)); // FLD1
    addLegalFPImmediate(APFloat(-0.0f)); // FLD0/FCHS
    addLegalFPImmediate(APFloat(-1.0f)); // FLD1/FCHS
  }

  // We don't support FMA.
  setOperationAction(ISD::FMA, MVT::f64, Expand);
  setOperationAction(ISD::FMA, MVT::f32, Expand);

  // Long double always uses X87.
  if (!TM.Options.UseSoftFloat) {
    addRegisterClass(MVT::f80, &X86::RFP80RegClass);
    setOperationAction(ISD::UNDEF,     MVT::f80, Expand);
    setOperationAction(ISD::FCOPYSIGN, MVT::f80, Expand);
    {
      APFloat TmpFlt = APFloat::getZero(APFloat::x87DoubleExtended);
      addLegalFPImmediate(TmpFlt);  // FLD0
      TmpFlt.changeSign();
      addLegalFPImmediate(TmpFlt);  // FLD0/FCHS

      bool ignored;
      APFloat TmpFlt2(+1.0);
      TmpFlt2.convert(APFloat::x87DoubleExtended, APFloat::rmNearestTiesToEven,
                      &ignored);
      addLegalFPImmediate(TmpFlt2);  // FLD1
      TmpFlt2.changeSign();
      addLegalFPImmediate(TmpFlt2);  // FLD1/FCHS
    }

    if (!TM.Options.UnsafeFPMath) {
      setOperationAction(ISD::FSIN   , MVT::f80, Expand);
      setOperationAction(ISD::FCOS   , MVT::f80, Expand);
      setOperationAction(ISD::FSINCOS, MVT::f80, Expand);
    }

    setOperationAction(ISD::FFLOOR, MVT::f80, Expand);
    setOperationAction(ISD::FCEIL,  MVT::f80, Expand);
    setOperationAction(ISD::FTRUNC, MVT::f80, Expand);
    setOperationAction(ISD::FRINT,  MVT::f80, Expand);
    setOperationAction(ISD::FNEARBYINT, MVT::f80, Expand);
    setOperationAction(ISD::FMA, MVT::f80, Expand);
  }

  // Always use a library call for pow.
  setOperationAction(ISD::FPOW             , MVT::f32  , Expand);
  setOperationAction(ISD::FPOW             , MVT::f64  , Expand);
  setOperationAction(ISD::FPOW             , MVT::f80  , Expand);

  setOperationAction(ISD::FLOG, MVT::f80, Expand);
  setOperationAction(ISD::FLOG2, MVT::f80, Expand);
  setOperationAction(ISD::FLOG10, MVT::f80, Expand);
  setOperationAction(ISD::FEXP, MVT::f80, Expand);
  setOperationAction(ISD::FEXP2, MVT::f80, Expand);

  // First set operation action for all vector types to either promote
  // (for widening) or expand (for scalarization). Then we will selectively
  // turn on ones that can be effectively codegen'd.
  for (int i = MVT::FIRST_VECTOR_VALUETYPE;
           i <= MVT::LAST_VECTOR_VALUETYPE; ++i) {
    MVT VT = (MVT::SimpleValueType)i;
    setOperationAction(ISD::ADD , VT, Expand);
    setOperationAction(ISD::SUB , VT, Expand);
    setOperationAction(ISD::FADD, VT, Expand);
    setOperationAction(ISD::FNEG, VT, Expand);
    setOperationAction(ISD::FSUB, VT, Expand);
    setOperationAction(ISD::MUL , VT, Expand);
    setOperationAction(ISD::FMUL, VT, Expand);
    setOperationAction(ISD::SDIV, VT, Expand);
    setOperationAction(ISD::UDIV, VT, Expand);
    setOperationAction(ISD::FDIV, VT, Expand);
    setOperationAction(ISD::SREM, VT, Expand);
    setOperationAction(ISD::UREM, VT, Expand);
    setOperationAction(ISD::LOAD, VT, Expand);
    setOperationAction(ISD::VECTOR_SHUFFLE, VT, Expand);
    setOperationAction(ISD::EXTRACT_VECTOR_ELT, VT,Expand);
    setOperationAction(ISD::INSERT_VECTOR_ELT, VT, Expand);
    setOperationAction(ISD::EXTRACT_SUBVECTOR, VT,Expand);
    setOperationAction(ISD::INSERT_SUBVECTOR, VT,Expand);
    setOperationAction(ISD::FABS, VT, Expand);
    setOperationAction(ISD::FSIN, VT, Expand);
    setOperationAction(ISD::FSINCOS, VT, Expand);
    setOperationAction(ISD::FCOS, VT, Expand);
    setOperationAction(ISD::FSINCOS, VT, Expand);
    setOperationAction(ISD::FREM, VT, Expand);
    setOperationAction(ISD::FMA,  VT, Expand);
    setOperationAction(ISD::FPOWI, VT, Expand);
    setOperationAction(ISD::FSQRT, VT, Expand);
    setOperationAction(ISD::FCOPYSIGN, VT, Expand);
    setOperationAction(ISD::FFLOOR, VT, Expand);
    setOperationAction(ISD::FCEIL, VT, Expand);
    setOperationAction(ISD::FTRUNC, VT, Expand);
    setOperationAction(ISD::FRINT, VT, Expand);
    setOperationAction(ISD::FNEARBYINT, VT, Expand);
    setOperationAction(ISD::SMUL_LOHI, VT, Expand);
    setOperationAction(ISD::MULHS, VT, Expand);
    setOperationAction(ISD::UMUL_LOHI, VT, Expand);
    setOperationAction(ISD::MULHU, VT, Expand);
    setOperationAction(ISD::SDIVREM, VT, Expand);
    setOperationAction(ISD::UDIVREM, VT, Expand);
    setOperationAction(ISD::FPOW, VT, Expand);
    setOperationAction(ISD::CTPOP, VT, Expand);
    setOperationAction(ISD::CTTZ, VT, Expand);
    setOperationAction(ISD::CTTZ_ZERO_UNDEF, VT, Expand);
    setOperationAction(ISD::CTLZ, VT, Expand);
    setOperationAction(ISD::CTLZ_ZERO_UNDEF, VT, Expand);
    setOperationAction(ISD::SHL, VT, Expand);
    setOperationAction(ISD::SRA, VT, Expand);
    setOperationAction(ISD::SRL, VT, Expand);
    setOperationAction(ISD::ROTL, VT, Expand);
    setOperationAction(ISD::ROTR, VT, Expand);
    setOperationAction(ISD::BSWAP, VT, Expand);
    setOperationAction(ISD::SETCC, VT, Expand);
    setOperationAction(ISD::FLOG, VT, Expand);
    setOperationAction(ISD::FLOG2, VT, Expand);
    setOperationAction(ISD::FLOG10, VT, Expand);
    setOperationAction(ISD::FEXP, VT, Expand);
    setOperationAction(ISD::FEXP2, VT, Expand);
    setOperationAction(ISD::FP_TO_UINT, VT, Expand);
    setOperationAction(ISD::FP_TO_SINT, VT, Expand);
    setOperationAction(ISD::UINT_TO_FP, VT, Expand);
    setOperationAction(ISD::SINT_TO_FP, VT, Expand);
    setOperationAction(ISD::SIGN_EXTEND_INREG, VT,Expand);
    setOperationAction(ISD::TRUNCATE, VT, Expand);
    setOperationAction(ISD::SIGN_EXTEND, VT, Expand);
    setOperationAction(ISD::ZERO_EXTEND, VT, Expand);
    setOperationAction(ISD::ANY_EXTEND, VT, Expand);
    setOperationAction(ISD::VSELECT, VT, Expand);
    setOperationAction(ISD::SELECT_CC, VT, Expand);
    for (int InnerVT = MVT::FIRST_VECTOR_VALUETYPE;
             InnerVT <= MVT::LAST_VECTOR_VALUETYPE; ++InnerVT)
      setTruncStoreAction(VT,
                          (MVT::SimpleValueType)InnerVT, Expand);
    setLoadExtAction(ISD::SEXTLOAD, VT, Expand);
    setLoadExtAction(ISD::ZEXTLOAD, VT, Expand);

    // N.b. ISD::EXTLOAD legality is basically ignored except for i1-like types,
    // we have to deal with them whether we ask for Expansion or not. Setting
    // Expand causes its own optimisation problems though, so leave them legal.
    if (VT.getVectorElementType() == MVT::i1)
      setLoadExtAction(ISD::EXTLOAD, VT, Expand);
  }

  // FIXME: In order to prevent SSE instructions being expanded to MMX ones
  // with -msoft-float, disable use of MMX as well.
  if (!TM.Options.UseSoftFloat && Subtarget->hasMMX()) {
    addRegisterClass(MVT::x86mmx, &X86::VR64RegClass);
    // No operations on x86mmx supported, everything uses intrinsics.
  }

  // MMX-sized vectors (other than x86mmx) are expected to be expanded
  // into smaller operations.
  setOperationAction(ISD::MULHS,              MVT::v8i8,  Expand);
  setOperationAction(ISD::MULHS,              MVT::v4i16, Expand);
  setOperationAction(ISD::MULHS,              MVT::v2i32, Expand);
  setOperationAction(ISD::MULHS,              MVT::v1i64, Expand);
  setOperationAction(ISD::AND,                MVT::v8i8,  Expand);
  setOperationAction(ISD::AND,                MVT::v4i16, Expand);
  setOperationAction(ISD::AND,                MVT::v2i32, Expand);
  setOperationAction(ISD::AND,                MVT::v1i64, Expand);
  setOperationAction(ISD::OR,                 MVT::v8i8,  Expand);
  setOperationAction(ISD::OR,                 MVT::v4i16, Expand);
  setOperationAction(ISD::OR,                 MVT::v2i32, Expand);
  setOperationAction(ISD::OR,                 MVT::v1i64, Expand);
  setOperationAction(ISD::XOR,                MVT::v8i8,  Expand);
  setOperationAction(ISD::XOR,                MVT::v4i16, Expand);
  setOperationAction(ISD::XOR,                MVT::v2i32, Expand);
  setOperationAction(ISD::XOR,                MVT::v1i64, Expand);
  setOperationAction(ISD::SCALAR_TO_VECTOR,   MVT::v8i8,  Expand);
  setOperationAction(ISD::SCALAR_TO_VECTOR,   MVT::v4i16, Expand);
  setOperationAction(ISD::SCALAR_TO_VECTOR,   MVT::v2i32, Expand);
  setOperationAction(ISD::SCALAR_TO_VECTOR,   MVT::v1i64, Expand);
  setOperationAction(ISD::INSERT_VECTOR_ELT,  MVT::v1i64, Expand);
  setOperationAction(ISD::SELECT,             MVT::v8i8,  Expand);
  setOperationAction(ISD::SELECT,             MVT::v4i16, Expand);
  setOperationAction(ISD::SELECT,             MVT::v2i32, Expand);
  setOperationAction(ISD::SELECT,             MVT::v1i64, Expand);
  setOperationAction(ISD::BITCAST,            MVT::v8i8,  Expand);
  setOperationAction(ISD::BITCAST,            MVT::v4i16, Expand);
  setOperationAction(ISD::BITCAST,            MVT::v2i32, Expand);
  setOperationAction(ISD::BITCAST,            MVT::v1i64, Expand);

  if (!TM.Options.UseSoftFloat && Subtarget->hasSSE1()) {
    addRegisterClass(MVT::v4f32, &X86::VR128RegClass);

    setOperationAction(ISD::FADD,               MVT::v4f32, Legal);
    setOperationAction(ISD::FSUB,               MVT::v4f32, Legal);
    setOperationAction(ISD::FMUL,               MVT::v4f32, Legal);
    setOperationAction(ISD::FDIV,               MVT::v4f32, Legal);
    setOperationAction(ISD::FSQRT,              MVT::v4f32, Legal);
    setOperationAction(ISD::FNEG,               MVT::v4f32, Custom);
    setOperationAction(ISD::FABS,               MVT::v4f32, Custom);
    setOperationAction(ISD::LOAD,               MVT::v4f32, Legal);
    setOperationAction(ISD::BUILD_VECTOR,       MVT::v4f32, Custom);
    setOperationAction(ISD::VECTOR_SHUFFLE,     MVT::v4f32, Custom);
    setOperationAction(ISD::EXTRACT_VECTOR_ELT, MVT::v4f32, Custom);
    setOperationAction(ISD::SELECT,             MVT::v4f32, Custom);
  }

  if (!TM.Options.UseSoftFloat && Subtarget->hasSSE2()) {
    addRegisterClass(MVT::v2f64, &X86::VR128RegClass);

    // FIXME: Unfortunately, -soft-float and -no-implicit-float mean XMM
    // registers cannot be used even for integer operations.
    addRegisterClass(MVT::v16i8, &X86::VR128RegClass);
    addRegisterClass(MVT::v8i16, &X86::VR128RegClass);
    addRegisterClass(MVT::v4i32, &X86::VR128RegClass);
    addRegisterClass(MVT::v2i64, &X86::VR128RegClass);

    setOperationAction(ISD::ADD,                MVT::v16i8, Legal);
    setOperationAction(ISD::ADD,                MVT::v8i16, Legal);
    setOperationAction(ISD::ADD,                MVT::v4i32, Legal);
    setOperationAction(ISD::ADD,                MVT::v2i64, Legal);
    setOperationAction(ISD::MUL,                MVT::v4i32, Custom);
    setOperationAction(ISD::MUL,                MVT::v2i64, Custom);
    setOperationAction(ISD::UMUL_LOHI,          MVT::v4i32, Custom);
    setOperationAction(ISD::SMUL_LOHI,          MVT::v4i32, Custom);
    setOperationAction(ISD::MULHU,              MVT::v8i16, Legal);
    setOperationAction(ISD::MULHS,              MVT::v8i16, Legal);
    setOperationAction(ISD::SUB,                MVT::v16i8, Legal);
    setOperationAction(ISD::SUB,                MVT::v8i16, Legal);
    setOperationAction(ISD::SUB,                MVT::v4i32, Legal);
    setOperationAction(ISD::SUB,                MVT::v2i64, Legal);
    setOperationAction(ISD::MUL,                MVT::v8i16, Legal);
    setOperationAction(ISD::FADD,               MVT::v2f64, Legal);
    setOperationAction(ISD::FSUB,               MVT::v2f64, Legal);
    setOperationAction(ISD::FMUL,               MVT::v2f64, Legal);
    setOperationAction(ISD::FDIV,               MVT::v2f64, Legal);
    setOperationAction(ISD::FSQRT,              MVT::v2f64, Legal);
    setOperationAction(ISD::FNEG,               MVT::v2f64, Custom);
    setOperationAction(ISD::FABS,               MVT::v2f64, Custom);

    setOperationAction(ISD::SETCC,              MVT::v2i64, Custom);
    setOperationAction(ISD::SETCC,              MVT::v16i8, Custom);
    setOperationAction(ISD::SETCC,              MVT::v8i16, Custom);
    setOperationAction(ISD::SETCC,              MVT::v4i32, Custom);

    setOperationAction(ISD::SCALAR_TO_VECTOR,   MVT::v16i8, Custom);
    setOperationAction(ISD::SCALAR_TO_VECTOR,   MVT::v8i16, Custom);
    setOperationAction(ISD::INSERT_VECTOR_ELT,  MVT::v8i16, Custom);
    setOperationAction(ISD::INSERT_VECTOR_ELT,  MVT::v4i32, Custom);
    setOperationAction(ISD::INSERT_VECTOR_ELT,  MVT::v4f32, Custom);

    // Custom lower build_vector, vector_shuffle, and extract_vector_elt.
    for (int i = MVT::v16i8; i != MVT::v2i64; ++i) {
      MVT VT = (MVT::SimpleValueType)i;
      // Do not attempt to custom lower non-power-of-2 vectors
      if (!isPowerOf2_32(VT.getVectorNumElements()))
        continue;
      // Do not attempt to custom lower non-128-bit vectors
      if (!VT.is128BitVector())
        continue;
      setOperationAction(ISD::BUILD_VECTOR,       VT, Custom);
      setOperationAction(ISD::VECTOR_SHUFFLE,     VT, Custom);
      setOperationAction(ISD::EXTRACT_VECTOR_ELT, VT, Custom);
    }

    // We support custom legalizing of sext and anyext loads for specific
    // memory vector types which we can load as a scalar (or sequence of
    // scalars) and extend in-register to a legal 128-bit vector type. For sext
    // loads these must work with a single scalar load.
    setLoadExtAction(ISD::SEXTLOAD, MVT::v4i8, Custom);
    setLoadExtAction(ISD::SEXTLOAD, MVT::v4i16, Custom);
    setLoadExtAction(ISD::SEXTLOAD, MVT::v8i8, Custom);
    setLoadExtAction(ISD::EXTLOAD, MVT::v2i8, Custom);
    setLoadExtAction(ISD::EXTLOAD, MVT::v2i16, Custom);
    setLoadExtAction(ISD::EXTLOAD, MVT::v2i32, Custom);
    setLoadExtAction(ISD::EXTLOAD, MVT::v4i8, Custom);
    setLoadExtAction(ISD::EXTLOAD, MVT::v4i16, Custom);
    setLoadExtAction(ISD::EXTLOAD, MVT::v8i8, Custom);

    setOperationAction(ISD::BUILD_VECTOR,       MVT::v2f64, Custom);
    setOperationAction(ISD::BUILD_VECTOR,       MVT::v2i64, Custom);
    setOperationAction(ISD::VECTOR_SHUFFLE,     MVT::v2f64, Custom);
    setOperationAction(ISD::VECTOR_SHUFFLE,     MVT::v2i64, Custom);
    setOperationAction(ISD::INSERT_VECTOR_ELT,  MVT::v2f64, Custom);
    setOperationAction(ISD::EXTRACT_VECTOR_ELT, MVT::v2f64, Custom);

    if (Subtarget->is64Bit()) {
      setOperationAction(ISD::INSERT_VECTOR_ELT,  MVT::v2i64, Custom);
      setOperationAction(ISD::EXTRACT_VECTOR_ELT, MVT::v2i64, Custom);
    }

    // Promote v16i8, v8i16, v4i32 load, select, and, or, xor to v2i64.
    for (int i = MVT::v16i8; i != MVT::v2i64; ++i) {
      MVT VT = (MVT::SimpleValueType)i;

      // Do not attempt to promote non-128-bit vectors
      if (!VT.is128BitVector())
        continue;

      setOperationAction(ISD::AND,    VT, Promote);
      AddPromotedToType (ISD::AND,    VT, MVT::v2i64);
      setOperationAction(ISD::OR,     VT, Promote);
      AddPromotedToType (ISD::OR,     VT, MVT::v2i64);
      setOperationAction(ISD::XOR,    VT, Promote);
      AddPromotedToType (ISD::XOR,    VT, MVT::v2i64);
      setOperationAction(ISD::LOAD,   VT, Promote);
      AddPromotedToType (ISD::LOAD,   VT, MVT::v2i64);
      setOperationAction(ISD::SELECT, VT, Promote);
      AddPromotedToType (ISD::SELECT, VT, MVT::v2i64);
    }

    setTruncStoreAction(MVT::f64, MVT::f32, Expand);

    // Custom lower v2i64 and v2f64 selects.
    setOperationAction(ISD::LOAD,               MVT::v2f64, Legal);
    setOperationAction(ISD::LOAD,               MVT::v2i64, Legal);
    setOperationAction(ISD::SELECT,             MVT::v2f64, Custom);
    setOperationAction(ISD::SELECT,             MVT::v2i64, Custom);

    setOperationAction(ISD::FP_TO_SINT,         MVT::v4i32, Legal);
    setOperationAction(ISD::SINT_TO_FP,         MVT::v4i32, Legal);

    setOperationAction(ISD::UINT_TO_FP,         MVT::v4i8,  Custom);
    setOperationAction(ISD::UINT_TO_FP,         MVT::v4i16, Custom);
    // As there is no 64-bit GPR available, we need build a special custom
    // sequence to convert from v2i32 to v2f32.
    if (!Subtarget->is64Bit())
      setOperationAction(ISD::UINT_TO_FP,       MVT::v2f32, Custom);

    setOperationAction(ISD::FP_EXTEND,          MVT::v2f32, Custom);
    setOperationAction(ISD::FP_ROUND,           MVT::v2f32, Custom);

    setLoadExtAction(ISD::EXTLOAD,              MVT::v2f32, Legal);

    setOperationAction(ISD::BITCAST,            MVT::v2i32, Custom);
    setOperationAction(ISD::BITCAST,            MVT::v4i16, Custom);
    setOperationAction(ISD::BITCAST,            MVT::v8i8,  Custom);
  }

  if (!TM.Options.UseSoftFloat && Subtarget->hasSSE41()) {
    setOperationAction(ISD::FFLOOR,             MVT::f32,   Legal);
    setOperationAction(ISD::FCEIL,              MVT::f32,   Legal);
    setOperationAction(ISD::FTRUNC,             MVT::f32,   Legal);
    setOperationAction(ISD::FRINT,              MVT::f32,   Legal);
    setOperationAction(ISD::FNEARBYINT,         MVT::f32,   Legal);
    setOperationAction(ISD::FFLOOR,             MVT::f64,   Legal);
    setOperationAction(ISD::FCEIL,              MVT::f64,   Legal);
    setOperationAction(ISD::FTRUNC,             MVT::f64,   Legal);
    setOperationAction(ISD::FRINT,              MVT::f64,   Legal);
    setOperationAction(ISD::FNEARBYINT,         MVT::f64,   Legal);

    setOperationAction(ISD::FFLOOR,             MVT::v4f32, Legal);
    setOperationAction(ISD::FCEIL,              MVT::v4f32, Legal);
    setOperationAction(ISD::FTRUNC,             MVT::v4f32, Legal);
    setOperationAction(ISD::FRINT,              MVT::v4f32, Legal);
    setOperationAction(ISD::FNEARBYINT,         MVT::v4f32, Legal);
    setOperationAction(ISD::FFLOOR,             MVT::v2f64, Legal);
    setOperationAction(ISD::FCEIL,              MVT::v2f64, Legal);
    setOperationAction(ISD::FTRUNC,             MVT::v2f64, Legal);
    setOperationAction(ISD::FRINT,              MVT::v2f64, Legal);
    setOperationAction(ISD::FNEARBYINT,         MVT::v2f64, Legal);

    // FIXME: Do we need to handle scalar-to-vector here?
    setOperationAction(ISD::MUL,                MVT::v4i32, Legal);

    setOperationAction(ISD::VSELECT,            MVT::v2f64, Custom);
    setOperationAction(ISD::VSELECT,            MVT::v2i64, Custom);
    setOperationAction(ISD::VSELECT,            MVT::v4i32, Custom);
    setOperationAction(ISD::VSELECT,            MVT::v4f32, Custom);
    setOperationAction(ISD::VSELECT,            MVT::v8i16, Custom);
    // There is no BLENDI for byte vectors. We don't need to custom lower
    // some vselects for now.
    setOperationAction(ISD::VSELECT,            MVT::v16i8, Legal);

    // SSE41 brings specific instructions for doing vector sign extend even in
    // cases where we don't have SRA.
    setLoadExtAction(ISD::SEXTLOAD, MVT::v2i8, Custom);
    setLoadExtAction(ISD::SEXTLOAD, MVT::v2i16, Custom);
    setLoadExtAction(ISD::SEXTLOAD, MVT::v2i32, Custom);

    // i8 and i16 vectors are custom because the source register and source
    // source memory operand types are not the same width.  f32 vectors are
    // custom since the immediate controlling the insert encodes additional
    // information.
    setOperationAction(ISD::INSERT_VECTOR_ELT,  MVT::v16i8, Custom);
    setOperationAction(ISD::INSERT_VECTOR_ELT,  MVT::v8i16, Custom);
    setOperationAction(ISD::INSERT_VECTOR_ELT,  MVT::v4i32, Custom);
    setOperationAction(ISD::INSERT_VECTOR_ELT,  MVT::v4f32, Custom);

    setOperationAction(ISD::EXTRACT_VECTOR_ELT, MVT::v16i8, Custom);
    setOperationAction(ISD::EXTRACT_VECTOR_ELT, MVT::v8i16, Custom);
    setOperationAction(ISD::EXTRACT_VECTOR_ELT, MVT::v4i32, Custom);
    setOperationAction(ISD::EXTRACT_VECTOR_ELT, MVT::v4f32, Custom);

    // FIXME: these should be Legal, but that's only for the case where
    // the index is constant.  For now custom expand to deal with that.
    if (Subtarget->is64Bit()) {
      setOperationAction(ISD::INSERT_VECTOR_ELT,  MVT::v2i64, Custom);
      setOperationAction(ISD::EXTRACT_VECTOR_ELT, MVT::v2i64, Custom);
    }
  }

  if (Subtarget->hasSSE2()) {
    setOperationAction(ISD::SRL,               MVT::v8i16, Custom);
    setOperationAction(ISD::SRL,               MVT::v16i8, Custom);

    setOperationAction(ISD::SHL,               MVT::v8i16, Custom);
    setOperationAction(ISD::SHL,               MVT::v16i8, Custom);

    setOperationAction(ISD::SRA,               MVT::v8i16, Custom);
    setOperationAction(ISD::SRA,               MVT::v16i8, Custom);

    // In the customized shift lowering, the legal cases in AVX2 will be
    // recognized.
    setOperationAction(ISD::SRL,               MVT::v2i64, Custom);
    setOperationAction(ISD::SRL,               MVT::v4i32, Custom);

    setOperationAction(ISD::SHL,               MVT::v2i64, Custom);
    setOperationAction(ISD::SHL,               MVT::v4i32, Custom);

    setOperationAction(ISD::SRA,               MVT::v4i32, Custom);
  }

  if (!TM.Options.UseSoftFloat && Subtarget->hasFp256()) {
    addRegisterClass(MVT::v32i8,  &X86::VR256RegClass);
    addRegisterClass(MVT::v16i16, &X86::VR256RegClass);
    addRegisterClass(MVT::v8i32,  &X86::VR256RegClass);
    addRegisterClass(MVT::v8f32,  &X86::VR256RegClass);
    addRegisterClass(MVT::v4i64,  &X86::VR256RegClass);
    addRegisterClass(MVT::v4f64,  &X86::VR256RegClass);

    setOperationAction(ISD::LOAD,               MVT::v8f32, Legal);
    setOperationAction(ISD::LOAD,               MVT::v4f64, Legal);
    setOperationAction(ISD::LOAD,               MVT::v4i64, Legal);

    setOperationAction(ISD::FADD,               MVT::v8f32, Legal);
    setOperationAction(ISD::FSUB,               MVT::v8f32, Legal);
    setOperationAction(ISD::FMUL,               MVT::v8f32, Legal);
    setOperationAction(ISD::FDIV,               MVT::v8f32, Legal);
    setOperationAction(ISD::FSQRT,              MVT::v8f32, Legal);
    setOperationAction(ISD::FFLOOR,             MVT::v8f32, Legal);
    setOperationAction(ISD::FCEIL,              MVT::v8f32, Legal);
    setOperationAction(ISD::FTRUNC,             MVT::v8f32, Legal);
    setOperationAction(ISD::FRINT,              MVT::v8f32, Legal);
    setOperationAction(ISD::FNEARBYINT,         MVT::v8f32, Legal);
    setOperationAction(ISD::FNEG,               MVT::v8f32, Custom);
    setOperationAction(ISD::FABS,               MVT::v8f32, Custom);

    setOperationAction(ISD::FADD,               MVT::v4f64, Legal);
    setOperationAction(ISD::FSUB,               MVT::v4f64, Legal);
    setOperationAction(ISD::FMUL,               MVT::v4f64, Legal);
    setOperationAction(ISD::FDIV,               MVT::v4f64, Legal);
    setOperationAction(ISD::FSQRT,              MVT::v4f64, Legal);
    setOperationAction(ISD::FFLOOR,             MVT::v4f64, Legal);
    setOperationAction(ISD::FCEIL,              MVT::v4f64, Legal);
    setOperationAction(ISD::FTRUNC,             MVT::v4f64, Legal);
    setOperationAction(ISD::FRINT,              MVT::v4f64, Legal);
    setOperationAction(ISD::FNEARBYINT,         MVT::v4f64, Legal);
    setOperationAction(ISD::FNEG,               MVT::v4f64, Custom);
    setOperationAction(ISD::FABS,               MVT::v4f64, Custom);

    // (fp_to_int:v8i16 (v8f32 ..)) requires the result type to be promoted
    // even though v8i16 is a legal type.
    setOperationAction(ISD::FP_TO_SINT,         MVT::v8i16, Promote);
    setOperationAction(ISD::FP_TO_UINT,         MVT::v8i16, Promote);
    setOperationAction(ISD::FP_TO_SINT,         MVT::v8i32, Legal);

    setOperationAction(ISD::SINT_TO_FP,         MVT::v8i16, Promote);
    setOperationAction(ISD::SINT_TO_FP,         MVT::v8i32, Legal);
    setOperationAction(ISD::FP_ROUND,           MVT::v4f32, Legal);

    setOperationAction(ISD::UINT_TO_FP,         MVT::v8i8,  Custom);
    setOperationAction(ISD::UINT_TO_FP,         MVT::v8i16, Custom);

    setLoadExtAction(ISD::EXTLOAD,              MVT::v4f32, Legal);

    setOperationAction(ISD::SRL,               MVT::v16i16, Custom);
    setOperationAction(ISD::SRL,               MVT::v32i8, Custom);

    setOperationAction(ISD::SHL,               MVT::v16i16, Custom);
    setOperationAction(ISD::SHL,               MVT::v32i8, Custom);

    setOperationAction(ISD::SRA,               MVT::v16i16, Custom);
    setOperationAction(ISD::SRA,               MVT::v32i8, Custom);

    setOperationAction(ISD::SETCC,             MVT::v32i8, Custom);
    setOperationAction(ISD::SETCC,             MVT::v16i16, Custom);
    setOperationAction(ISD::SETCC,             MVT::v8i32, Custom);
    setOperationAction(ISD::SETCC,             MVT::v4i64, Custom);

    setOperationAction(ISD::SELECT,            MVT::v4f64, Custom);
    setOperationAction(ISD::SELECT,            MVT::v4i64, Custom);
    setOperationAction(ISD::SELECT,            MVT::v8f32, Custom);

    setOperationAction(ISD::VSELECT,           MVT::v4f64, Custom);
    setOperationAction(ISD::VSELECT,           MVT::v4i64, Custom);
    setOperationAction(ISD::VSELECT,           MVT::v8i32, Custom);
    setOperationAction(ISD::VSELECT,           MVT::v8f32, Custom);

    setOperationAction(ISD::SIGN_EXTEND,       MVT::v4i64, Custom);
    setOperationAction(ISD::SIGN_EXTEND,       MVT::v8i32, Custom);
    setOperationAction(ISD::SIGN_EXTEND,       MVT::v16i16, Custom);
    setOperationAction(ISD::ZERO_EXTEND,       MVT::v4i64, Custom);
    setOperationAction(ISD::ZERO_EXTEND,       MVT::v8i32, Custom);
    setOperationAction(ISD::ZERO_EXTEND,       MVT::v16i16, Custom);
    setOperationAction(ISD::ANY_EXTEND,        MVT::v4i64, Custom);
    setOperationAction(ISD::ANY_EXTEND,        MVT::v8i32, Custom);
    setOperationAction(ISD::ANY_EXTEND,        MVT::v16i16, Custom);
    setOperationAction(ISD::TRUNCATE,          MVT::v16i8, Custom);
    setOperationAction(ISD::TRUNCATE,          MVT::v8i16, Custom);
    setOperationAction(ISD::TRUNCATE,          MVT::v4i32, Custom);

    if (Subtarget->hasFMA() || Subtarget->hasFMA4()) {
      setOperationAction(ISD::FMA,             MVT::v8f32, Legal);
      setOperationAction(ISD::FMA,             MVT::v4f64, Legal);
      setOperationAction(ISD::FMA,             MVT::v4f32, Legal);
      setOperationAction(ISD::FMA,             MVT::v2f64, Legal);
      setOperationAction(ISD::FMA,             MVT::f32, Legal);
      setOperationAction(ISD::FMA,             MVT::f64, Legal);
    }

    if (Subtarget->hasInt256()) {
      setOperationAction(ISD::ADD,             MVT::v4i64, Legal);
      setOperationAction(ISD::ADD,             MVT::v8i32, Legal);
      setOperationAction(ISD::ADD,             MVT::v16i16, Legal);
      setOperationAction(ISD::ADD,             MVT::v32i8, Legal);

      setOperationAction(ISD::SUB,             MVT::v4i64, Legal);
      setOperationAction(ISD::SUB,             MVT::v8i32, Legal);
      setOperationAction(ISD::SUB,             MVT::v16i16, Legal);
      setOperationAction(ISD::SUB,             MVT::v32i8, Legal);

      setOperationAction(ISD::MUL,             MVT::v4i64, Custom);
      setOperationAction(ISD::MUL,             MVT::v8i32, Legal);
      setOperationAction(ISD::MUL,             MVT::v16i16, Legal);
      // Don't lower v32i8 because there is no 128-bit byte mul

      setOperationAction(ISD::UMUL_LOHI,       MVT::v8i32, Custom);
      setOperationAction(ISD::SMUL_LOHI,       MVT::v8i32, Custom);
      setOperationAction(ISD::MULHU,           MVT::v16i16, Legal);
      setOperationAction(ISD::MULHS,           MVT::v16i16, Legal);

      setOperationAction(ISD::VSELECT,         MVT::v16i16, Custom);
      setOperationAction(ISD::VSELECT,         MVT::v32i8, Legal);
    } else {
      setOperationAction(ISD::ADD,             MVT::v4i64, Custom);
      setOperationAction(ISD::ADD,             MVT::v8i32, Custom);
      setOperationAction(ISD::ADD,             MVT::v16i16, Custom);
      setOperationAction(ISD::ADD,             MVT::v32i8, Custom);

      setOperationAction(ISD::SUB,             MVT::v4i64, Custom);
      setOperationAction(ISD::SUB,             MVT::v8i32, Custom);
      setOperationAction(ISD::SUB,             MVT::v16i16, Custom);
      setOperationAction(ISD::SUB,             MVT::v32i8, Custom);

      setOperationAction(ISD::MUL,             MVT::v4i64, Custom);
      setOperationAction(ISD::MUL,             MVT::v8i32, Custom);
      setOperationAction(ISD::MUL,             MVT::v16i16, Custom);
      // Don't lower v32i8 because there is no 128-bit byte mul
    }

    // In the customized shift lowering, the legal cases in AVX2 will be
    // recognized.
    setOperationAction(ISD::SRL,               MVT::v4i64, Custom);
    setOperationAction(ISD::SRL,               MVT::v8i32, Custom);

    setOperationAction(ISD::SHL,               MVT::v4i64, Custom);
    setOperationAction(ISD::SHL,               MVT::v8i32, Custom);

    setOperationAction(ISD::SRA,               MVT::v8i32, Custom);

    // Custom lower several nodes for 256-bit types.
    for (int i = MVT::FIRST_VECTOR_VALUETYPE;
             i <= MVT::LAST_VECTOR_VALUETYPE; ++i) {
      MVT VT = (MVT::SimpleValueType)i;

      // Extract subvector is special because the value type
      // (result) is 128-bit but the source is 256-bit wide.
      if (VT.is128BitVector())
        setOperationAction(ISD::EXTRACT_SUBVECTOR, VT, Custom);

      // Do not attempt to custom lower other non-256-bit vectors
      if (!VT.is256BitVector())
        continue;

      setOperationAction(ISD::BUILD_VECTOR,       VT, Custom);
      setOperationAction(ISD::VECTOR_SHUFFLE,     VT, Custom);
      setOperationAction(ISD::INSERT_VECTOR_ELT,  VT, Custom);
      setOperationAction(ISD::EXTRACT_VECTOR_ELT, VT, Custom);
      setOperationAction(ISD::SCALAR_TO_VECTOR,   VT, Custom);
      setOperationAction(ISD::INSERT_SUBVECTOR,   VT, Custom);
      setOperationAction(ISD::CONCAT_VECTORS,     VT, Custom);
    }

    // Promote v32i8, v16i16, v8i32 select, and, or, xor to v4i64.
    for (int i = MVT::v32i8; i != MVT::v4i64; ++i) {
      MVT VT = (MVT::SimpleValueType)i;

      // Do not attempt to promote non-256-bit vectors
      if (!VT.is256BitVector())
        continue;

      setOperationAction(ISD::AND,    VT, Promote);
      AddPromotedToType (ISD::AND,    VT, MVT::v4i64);
      setOperationAction(ISD::OR,     VT, Promote);
      AddPromotedToType (ISD::OR,     VT, MVT::v4i64);
      setOperationAction(ISD::XOR,    VT, Promote);
      AddPromotedToType (ISD::XOR,    VT, MVT::v4i64);
      setOperationAction(ISD::LOAD,   VT, Promote);
      AddPromotedToType (ISD::LOAD,   VT, MVT::v4i64);
      setOperationAction(ISD::SELECT, VT, Promote);
      AddPromotedToType (ISD::SELECT, VT, MVT::v4i64);
    }
  }

  if (!TM.Options.UseSoftFloat && Subtarget->hasAVX512()) {
    addRegisterClass(MVT::v16i32, &X86::VR512RegClass);
    addRegisterClass(MVT::v16f32, &X86::VR512RegClass);
    addRegisterClass(MVT::v8i64,  &X86::VR512RegClass);
    addRegisterClass(MVT::v8f64,  &X86::VR512RegClass);

    addRegisterClass(MVT::i1,     &X86::VK1RegClass);
    addRegisterClass(MVT::v8i1,   &X86::VK8RegClass);
    addRegisterClass(MVT::v16i1,  &X86::VK16RegClass);

    setOperationAction(ISD::BR_CC,              MVT::i1,    Expand);
    setOperationAction(ISD::SETCC,              MVT::i1,    Custom);
    setOperationAction(ISD::XOR,                MVT::i1,    Legal);
    setOperationAction(ISD::OR,                 MVT::i1,    Legal);
    setOperationAction(ISD::AND,                MVT::i1,    Legal);
    setLoadExtAction(ISD::EXTLOAD,              MVT::v8f32, Legal);
    setOperationAction(ISD::LOAD,               MVT::v16f32, Legal);
    setOperationAction(ISD::LOAD,               MVT::v8f64, Legal);
    setOperationAction(ISD::LOAD,               MVT::v8i64, Legal);
    setOperationAction(ISD::LOAD,               MVT::v16i32, Legal);
    setOperationAction(ISD::LOAD,               MVT::v16i1, Legal);

    setOperationAction(ISD::FADD,               MVT::v16f32, Legal);
    setOperationAction(ISD::FSUB,               MVT::v16f32, Legal);
    setOperationAction(ISD::FMUL,               MVT::v16f32, Legal);
    setOperationAction(ISD::FDIV,               MVT::v16f32, Legal);
    setOperationAction(ISD::FSQRT,              MVT::v16f32, Legal);
    setOperationAction(ISD::FNEG,               MVT::v16f32, Custom);

    setOperationAction(ISD::FADD,               MVT::v8f64, Legal);
    setOperationAction(ISD::FSUB,               MVT::v8f64, Legal);
    setOperationAction(ISD::FMUL,               MVT::v8f64, Legal);
    setOperationAction(ISD::FDIV,               MVT::v8f64, Legal);
    setOperationAction(ISD::FSQRT,              MVT::v8f64, Legal);
    setOperationAction(ISD::FNEG,               MVT::v8f64, Custom);
    setOperationAction(ISD::FMA,                MVT::v8f64, Legal);
    setOperationAction(ISD::FMA,                MVT::v16f32, Legal);

    setOperationAction(ISD::FP_TO_SINT,         MVT::i32, Legal);
    setOperationAction(ISD::FP_TO_UINT,         MVT::i32, Legal);
    setOperationAction(ISD::SINT_TO_FP,         MVT::i32, Legal);
    setOperationAction(ISD::UINT_TO_FP,         MVT::i32, Legal);
    if (Subtarget->is64Bit()) {
      setOperationAction(ISD::FP_TO_UINT,       MVT::i64, Legal);
      setOperationAction(ISD::FP_TO_SINT,       MVT::i64, Legal);
      setOperationAction(ISD::SINT_TO_FP,       MVT::i64, Legal);
      setOperationAction(ISD::UINT_TO_FP,       MVT::i64, Legal);
    }
    setOperationAction(ISD::FP_TO_SINT,         MVT::v16i32, Legal);
    setOperationAction(ISD::FP_TO_UINT,         MVT::v16i32, Legal);
    setOperationAction(ISD::FP_TO_UINT,         MVT::v8i32, Legal);
    setOperationAction(ISD::FP_TO_UINT,         MVT::v4i32, Legal);
    setOperationAction(ISD::SINT_TO_FP,         MVT::v16i32, Legal);
    setOperationAction(ISD::UINT_TO_FP,         MVT::v16i32, Legal);
    setOperationAction(ISD::UINT_TO_FP,         MVT::v8i32, Legal);
    setOperationAction(ISD::UINT_TO_FP,         MVT::v4i32, Legal);
    setOperationAction(ISD::FP_ROUND,           MVT::v8f32, Legal);
    setOperationAction(ISD::FP_EXTEND,          MVT::v8f32, Legal);

    setOperationAction(ISD::TRUNCATE,           MVT::i1, Custom);
    setOperationAction(ISD::TRUNCATE,           MVT::v16i8, Custom);
    setOperationAction(ISD::TRUNCATE,           MVT::v8i32, Custom);
    setOperationAction(ISD::TRUNCATE,           MVT::v8i1, Custom);
    setOperationAction(ISD::TRUNCATE,           MVT::v16i1, Custom);
    setOperationAction(ISD::TRUNCATE,           MVT::v16i16, Custom);
    setOperationAction(ISD::ZERO_EXTEND,        MVT::v16i32, Custom);
    setOperationAction(ISD::ZERO_EXTEND,        MVT::v8i64, Custom);
    setOperationAction(ISD::SIGN_EXTEND,        MVT::v16i32, Custom);
    setOperationAction(ISD::SIGN_EXTEND,        MVT::v8i64, Custom);
    setOperationAction(ISD::SIGN_EXTEND,        MVT::v16i8, Custom);
    setOperationAction(ISD::SIGN_EXTEND,        MVT::v8i16, Custom);
    setOperationAction(ISD::SIGN_EXTEND,        MVT::v16i16, Custom);

    setOperationAction(ISD::CONCAT_VECTORS,     MVT::v8f64,  Custom);
    setOperationAction(ISD::CONCAT_VECTORS,     MVT::v8i64,  Custom);
    setOperationAction(ISD::CONCAT_VECTORS,     MVT::v16f32,  Custom);
    setOperationAction(ISD::CONCAT_VECTORS,     MVT::v16i32,  Custom);
    setOperationAction(ISD::CONCAT_VECTORS,     MVT::v8i1,    Custom);
    setOperationAction(ISD::CONCAT_VECTORS,     MVT::v16i1, Legal);

    setOperationAction(ISD::SETCC,              MVT::v16i1, Custom);
    setOperationAction(ISD::SETCC,              MVT::v8i1, Custom);

    setOperationAction(ISD::MUL,              MVT::v8i64, Custom);

    setOperationAction(ISD::EXTRACT_VECTOR_ELT, MVT::v8i1,  Custom);
    setOperationAction(ISD::EXTRACT_VECTOR_ELT, MVT::v16i1, Custom);
    setOperationAction(ISD::INSERT_VECTOR_ELT,  MVT::v16i1, Custom);
    setOperationAction(ISD::INSERT_VECTOR_ELT,  MVT::v8i1, Custom);
    setOperationAction(ISD::BUILD_VECTOR,       MVT::v8i1, Custom);
    setOperationAction(ISD::BUILD_VECTOR,       MVT::v16i1, Custom);
    setOperationAction(ISD::SELECT,             MVT::v8f64, Custom);
    setOperationAction(ISD::SELECT,             MVT::v8i64, Custom);
    setOperationAction(ISD::SELECT,             MVT::v16f32, Custom);

    setOperationAction(ISD::ADD,                MVT::v8i64, Legal);
    setOperationAction(ISD::ADD,                MVT::v16i32, Legal);

    setOperationAction(ISD::SUB,                MVT::v8i64, Legal);
    setOperationAction(ISD::SUB,                MVT::v16i32, Legal);

    setOperationAction(ISD::MUL,                MVT::v16i32, Legal);

    setOperationAction(ISD::SRL,                MVT::v8i64, Custom);
    setOperationAction(ISD::SRL,                MVT::v16i32, Custom);

    setOperationAction(ISD::SHL,                MVT::v8i64, Custom);
    setOperationAction(ISD::SHL,                MVT::v16i32, Custom);

    setOperationAction(ISD::SRA,                MVT::v8i64, Custom);
    setOperationAction(ISD::SRA,                MVT::v16i32, Custom);

    setOperationAction(ISD::AND,                MVT::v8i64, Legal);
    setOperationAction(ISD::OR,                 MVT::v8i64, Legal);
    setOperationAction(ISD::XOR,                MVT::v8i64, Legal);
    setOperationAction(ISD::AND,                MVT::v16i32, Legal);
    setOperationAction(ISD::OR,                 MVT::v16i32, Legal);
    setOperationAction(ISD::XOR,                MVT::v16i32, Legal);

    if (Subtarget->hasCDI()) {
      setOperationAction(ISD::CTLZ,             MVT::v8i64, Legal);
      setOperationAction(ISD::CTLZ,             MVT::v16i32, Legal);
    }

    // Custom lower several nodes.
    for (int i = MVT::FIRST_VECTOR_VALUETYPE;
             i <= MVT::LAST_VECTOR_VALUETYPE; ++i) {
      MVT VT = (MVT::SimpleValueType)i;

      unsigned EltSize = VT.getVectorElementType().getSizeInBits();
      // Extract subvector is special because the value type
      // (result) is 256/128-bit but the source is 512-bit wide.
      if (VT.is128BitVector() || VT.is256BitVector())
        setOperationAction(ISD::EXTRACT_SUBVECTOR, VT, Custom);

      if (VT.getVectorElementType() == MVT::i1)
        setOperationAction(ISD::EXTRACT_SUBVECTOR, VT, Legal);

      // Do not attempt to custom lower other non-512-bit vectors
      if (!VT.is512BitVector())
        continue;

      if ( EltSize >= 32) {
        setOperationAction(ISD::VECTOR_SHUFFLE,      VT, Custom);
        setOperationAction(ISD::INSERT_VECTOR_ELT,   VT, Custom);
        setOperationAction(ISD::BUILD_VECTOR,        VT, Custom);
        setOperationAction(ISD::VSELECT,             VT, Legal);
        setOperationAction(ISD::EXTRACT_VECTOR_ELT,  VT, Custom);
        setOperationAction(ISD::SCALAR_TO_VECTOR,    VT, Custom);
        setOperationAction(ISD::INSERT_SUBVECTOR,    VT, Custom);
      }
    }
    for (int i = MVT::v32i8; i != MVT::v8i64; ++i) {
      MVT VT = (MVT::SimpleValueType)i;

      // Do not attempt to promote non-256-bit vectors
      if (!VT.is512BitVector())
        continue;

      setOperationAction(ISD::SELECT, VT, Promote);
      AddPromotedToType (ISD::SELECT, VT, MVT::v8i64);
    }
  }// has  AVX-512

  if (!TM.Options.UseSoftFloat && Subtarget->hasBWI()) {
    addRegisterClass(MVT::v32i16, &X86::VR512RegClass);
    addRegisterClass(MVT::v64i8,  &X86::VR512RegClass);

    addRegisterClass(MVT::v32i1,  &X86::VK32RegClass);
    addRegisterClass(MVT::v64i1,  &X86::VK64RegClass);

    setOperationAction(ISD::LOAD,               MVT::v32i16, Legal);
    setOperationAction(ISD::LOAD,               MVT::v64i8, Legal);
    setOperationAction(ISD::SETCC,              MVT::v32i1, Custom);
    setOperationAction(ISD::SETCC,              MVT::v64i1, Custom);

    for (int i = MVT::v32i8; i != MVT::v8i64; ++i) {
      const MVT VT = (MVT::SimpleValueType)i;

      const unsigned EltSize = VT.getVectorElementType().getSizeInBits();

      // Do not attempt to promote non-256-bit vectors
      if (!VT.is512BitVector())
        continue;

      if ( EltSize < 32) {
        setOperationAction(ISD::BUILD_VECTOR,        VT, Custom);
        setOperationAction(ISD::VSELECT,             VT, Legal);
      }
    }
  }

  if (!TM.Options.UseSoftFloat && Subtarget->hasVLX()) {
    addRegisterClass(MVT::v4i1,   &X86::VK4RegClass);
    addRegisterClass(MVT::v2i1,   &X86::VK2RegClass);

    setOperationAction(ISD::SETCC,              MVT::v4i1, Custom);
    setOperationAction(ISD::SETCC,              MVT::v2i1, Custom);
  }

  // SIGN_EXTEND_INREGs are evaluated by the extend type. Handle the expansion
  // of this type with custom code.
  for (int VT = MVT::FIRST_VECTOR_VALUETYPE;
           VT != MVT::LAST_VECTOR_VALUETYPE; VT++) {
    setOperationAction(ISD::SIGN_EXTEND_INREG, (MVT::SimpleValueType)VT,
                       Custom);
  }

  // We want to custom lower some of our intrinsics.
  setOperationAction(ISD::INTRINSIC_WO_CHAIN, MVT::Other, Custom);
  setOperationAction(ISD::INTRINSIC_W_CHAIN, MVT::Other, Custom);
  setOperationAction(ISD::INTRINSIC_VOID, MVT::Other, Custom);
  if (!Subtarget->is64Bit())
    setOperationAction(ISD::INTRINSIC_W_CHAIN, MVT::i64, Custom);

  // Only custom-lower 64-bit SADDO and friends on 64-bit because we don't
  // handle type legalization for these operations here.
  //
  // FIXME: We really should do custom legalization for addition and
  // subtraction on x86-32 once PR3203 is fixed.  We really can't do much better
  // than generic legalization for 64-bit multiplication-with-overflow, though.
  for (unsigned i = 0, e = 3+Subtarget->is64Bit(); i != e; ++i) {
    // Add/Sub/Mul with overflow operations are custom lowered.
    MVT VT = IntVTs[i];
    setOperationAction(ISD::SADDO, VT, Custom);
    setOperationAction(ISD::UADDO, VT, Custom);
    setOperationAction(ISD::SSUBO, VT, Custom);
    setOperationAction(ISD::USUBO, VT, Custom);
    setOperationAction(ISD::SMULO, VT, Custom);
    setOperationAction(ISD::UMULO, VT, Custom);
  }

  // There are no 8-bit 3-address imul/mul instructions
  setOperationAction(ISD::SMULO, MVT::i8, Expand);
  setOperationAction(ISD::UMULO, MVT::i8, Expand);

  if (!Subtarget->is64Bit()) {
    // These libcalls are not available in 32-bit.
    setLibcallName(RTLIB::SHL_I128, nullptr);
    setLibcallName(RTLIB::SRL_I128, nullptr);
    setLibcallName(RTLIB::SRA_I128, nullptr);
  }

  // Combine sin / cos into one node or libcall if possible.
  if (Subtarget->hasSinCos()) {
    setLibcallName(RTLIB::SINCOS_F32, "sincosf");
    setLibcallName(RTLIB::SINCOS_F64, "sincos");
    if (Subtarget->isTargetDarwin()) {
      // For MacOSX, we don't want to the normal expansion of a libcall to
      // sincos. We want to issue a libcall to __sincos_stret to avoid memory
      // traffic.
      setOperationAction(ISD::FSINCOS, MVT::f64, Custom);
      setOperationAction(ISD::FSINCOS, MVT::f32, Custom);
    }
  }

  if (Subtarget->isTargetWin64()) {
    setOperationAction(ISD::SDIV, MVT::i128, Custom);
    setOperationAction(ISD::UDIV, MVT::i128, Custom);
    setOperationAction(ISD::SREM, MVT::i128, Custom);
    setOperationAction(ISD::UREM, MVT::i128, Custom);
    setOperationAction(ISD::SDIVREM, MVT::i128, Custom);
    setOperationAction(ISD::UDIVREM, MVT::i128, Custom);
  }

  // We have target-specific dag combine patterns for the following nodes:
  setTargetDAGCombine(ISD::VECTOR_SHUFFLE);
  setTargetDAGCombine(ISD::EXTRACT_VECTOR_ELT);
  setTargetDAGCombine(ISD::VSELECT);
  setTargetDAGCombine(ISD::SELECT);
  setTargetDAGCombine(ISD::SHL);
  setTargetDAGCombine(ISD::SRA);
  setTargetDAGCombine(ISD::SRL);
  setTargetDAGCombine(ISD::OR);
  setTargetDAGCombine(ISD::AND);
  setTargetDAGCombine(ISD::ADD);
  setTargetDAGCombine(ISD::FADD);
  setTargetDAGCombine(ISD::FSUB);
  setTargetDAGCombine(ISD::FMA);
  setTargetDAGCombine(ISD::SUB);
  setTargetDAGCombine(ISD::LOAD);
  setTargetDAGCombine(ISD::STORE);
  setTargetDAGCombine(ISD::ZERO_EXTEND);
  setTargetDAGCombine(ISD::ANY_EXTEND);
  setTargetDAGCombine(ISD::SIGN_EXTEND);
  setTargetDAGCombine(ISD::SIGN_EXTEND_INREG);
  setTargetDAGCombine(ISD::TRUNCATE);
  setTargetDAGCombine(ISD::SINT_TO_FP);
  setTargetDAGCombine(ISD::SETCC);
  setTargetDAGCombine(ISD::INTRINSIC_WO_CHAIN);
  setTargetDAGCombine(ISD::BUILD_VECTOR);
  if (Subtarget->is64Bit())
    setTargetDAGCombine(ISD::MUL);
  setTargetDAGCombine(ISD::XOR);

  computeRegisterProperties();

  // On Darwin, -Os means optimize for size without hurting performance,
  // do not reduce the limit.
  MaxStoresPerMemset = 16; // For @llvm.memset -> sequence of stores
  MaxStoresPerMemsetOptSize = Subtarget->isTargetDarwin() ? 16 : 8;
  MaxStoresPerMemcpy = 8; // For @llvm.memcpy -> sequence of stores
  MaxStoresPerMemcpyOptSize = Subtarget->isTargetDarwin() ? 8 : 4;
  MaxStoresPerMemmove = 8; // For @llvm.memmove -> sequence of stores
  MaxStoresPerMemmoveOptSize = Subtarget->isTargetDarwin() ? 8 : 4;
  setPrefLoopAlignment(4); // 2^4 bytes.

  // Predictable cmov don't hurt on atom because it's in-order.
  PredictableSelectIsExpensive = !Subtarget->isAtom();

  setPrefFunctionAlignment(4); // 2^4 bytes.

  InitIntrinsicTables();
}

// This has so far only been implemented for 64-bit MachO.
bool X86TargetLowering::useLoadStackGuardNode() const {
  return Subtarget->getTargetTriple().getObjectFormat() == Triple::MachO &&
         Subtarget->is64Bit();
}

TargetLoweringBase::LegalizeTypeAction
X86TargetLowering::getPreferredVectorAction(EVT VT) const {
  if (ExperimentalVectorWideningLegalization &&
      VT.getVectorNumElements() != 1 &&
      VT.getVectorElementType().getSimpleVT() != MVT::i1)
    return TypeWidenVector;

  return TargetLoweringBase::getPreferredVectorAction(VT);
}

EVT X86TargetLowering::getSetCCResultType(LLVMContext &, EVT VT) const {
  if (!VT.isVector())
    return Subtarget->hasAVX512() ? MVT::i1: MVT::i8;

  const unsigned NumElts = VT.getVectorNumElements();
  const EVT EltVT = VT.getVectorElementType();
  if (VT.is512BitVector()) {
    if (Subtarget->hasAVX512())
      if (EltVT == MVT::i32 || EltVT == MVT::i64 ||
          EltVT == MVT::f32 || EltVT == MVT::f64)
        switch(NumElts) {
        case  8: return MVT::v8i1;
        case 16: return MVT::v16i1;
      }
    if (Subtarget->hasBWI())
      if (EltVT == MVT::i8 || EltVT == MVT::i16)
        switch(NumElts) {
        case 32: return MVT::v32i1;
        case 64: return MVT::v64i1;
      }
  }

  if (VT.is256BitVector() || VT.is128BitVector()) {
    if (Subtarget->hasVLX())
      if (EltVT == MVT::i32 || EltVT == MVT::i64 ||
          EltVT == MVT::f32 || EltVT == MVT::f64)
        switch(NumElts) {
        case 2: return MVT::v2i1;
        case 4: return MVT::v4i1;
        case 8: return MVT::v8i1;
      }
    if (Subtarget->hasBWI() && Subtarget->hasVLX())
      if (EltVT == MVT::i8 || EltVT == MVT::i16)
        switch(NumElts) {
        case  8: return MVT::v8i1;
        case 16: return MVT::v16i1;
        case 32: return MVT::v32i1;
      }
  }

  return VT.changeVectorElementTypeToInteger();
}

/// getMaxByValAlign - Helper for getByValTypeAlignment to determine
/// the desired ByVal argument alignment.
static void getMaxByValAlign(Type *Ty, unsigned &MaxAlign) {
  if (MaxAlign == 16)
    return;
  if (VectorType *VTy = dyn_cast<VectorType>(Ty)) {
    if (VTy->getBitWidth() == 128)
      MaxAlign = 16;
  } else if (ArrayType *ATy = dyn_cast<ArrayType>(Ty)) {
    unsigned EltAlign = 0;
    getMaxByValAlign(ATy->getElementType(), EltAlign);
    if (EltAlign > MaxAlign)
      MaxAlign = EltAlign;
  } else if (StructType *STy = dyn_cast<StructType>(Ty)) {
    for (unsigned i = 0, e = STy->getNumElements(); i != e; ++i) {
      unsigned EltAlign = 0;
      getMaxByValAlign(STy->getElementType(i), EltAlign);
      if (EltAlign > MaxAlign)
        MaxAlign = EltAlign;
      if (MaxAlign == 16)
        break;
    }
  }
}

/// getByValTypeAlignment - Return the desired alignment for ByVal aggregate
/// function arguments in the caller parameter area. For X86, aggregates
/// that contain SSE vectors are placed at 16-byte boundaries while the rest
/// are at 4-byte boundaries.
unsigned X86TargetLowering::getByValTypeAlignment(Type *Ty) const {
  if (Subtarget->is64Bit()) {
    // Max of 8 and alignment of type.
    unsigned TyAlign = TD->getABITypeAlignment(Ty);
    if (TyAlign > 8)
      return TyAlign;
    return 8;
  }

  unsigned Align = 4;
  if (Subtarget->hasSSE1())
    getMaxByValAlign(Ty, Align);
  return Align;
}

/// getOptimalMemOpType - Returns the target specific optimal type for load
/// and store operations as a result of memset, memcpy, and memmove
/// lowering. If DstAlign is zero that means it's safe to destination
/// alignment can satisfy any constraint. Similarly if SrcAlign is zero it
/// means there isn't a need to check it against alignment requirement,
/// probably because the source does not need to be loaded. If 'IsMemset' is
/// true, that means it's expanding a memset. If 'ZeroMemset' is true, that
/// means it's a memset of zero. 'MemcpyStrSrc' indicates whether the memcpy
/// source is constant so it does not need to be loaded.
/// It returns EVT::Other if the type should be determined using generic
/// target-independent logic.
EVT
X86TargetLowering::getOptimalMemOpType(uint64_t Size,
                                       unsigned DstAlign, unsigned SrcAlign,
                                       bool IsMemset, bool ZeroMemset,
                                       bool MemcpyStrSrc,
                                       MachineFunction &MF) const {
  const Function *F = MF.getFunction();
  if ((!IsMemset || ZeroMemset) &&
      !F->getAttributes().hasAttribute(AttributeSet::FunctionIndex,
                                       Attribute::NoImplicitFloat)) {
    if (Size >= 16 &&
        (Subtarget->isUnalignedMemAccessFast() ||
         ((DstAlign == 0 || DstAlign >= 16) &&
          (SrcAlign == 0 || SrcAlign >= 16)))) {
      if (Size >= 32) {
        if (Subtarget->hasInt256())
          return MVT::v8i32;
        if (Subtarget->hasFp256())
          return MVT::v8f32;
      }
      if (Subtarget->hasSSE2())
        return MVT::v4i32;
      if (Subtarget->hasSSE1())
        return MVT::v4f32;
    } else if (!MemcpyStrSrc && Size >= 8 &&
               !Subtarget->is64Bit() &&
               Subtarget->hasSSE2()) {
      // Do not use f64 to lower memcpy if source is string constant. It's
      // better to use i32 to avoid the loads.
      return MVT::f64;
    }
  }
  if (Subtarget->is64Bit() && Size >= 8)
    return MVT::i64;
  return MVT::i32;
}

bool X86TargetLowering::isSafeMemOpType(MVT VT) const {
  if (VT == MVT::f32)
    return X86ScalarSSEf32;
  else if (VT == MVT::f64)
    return X86ScalarSSEf64;
  return true;
}

bool
X86TargetLowering::allowsMisalignedMemoryAccesses(EVT VT,
                                                  unsigned,
                                                  unsigned,
                                                  bool *Fast) const {
  if (Fast)
    *Fast = Subtarget->isUnalignedMemAccessFast();
  return true;
}

/// getJumpTableEncoding - Return the entry encoding for a jump table in the
/// current function.  The returned value is a member of the
/// MachineJumpTableInfo::JTEntryKind enum.
unsigned X86TargetLowering::getJumpTableEncoding() const {
  // In GOT pic mode, each entry in the jump table is emitted as a @GOTOFF
  // symbol.
  if (getTargetMachine().getRelocationModel() == Reloc::PIC_ &&
      Subtarget->isPICStyleGOT())
    return MachineJumpTableInfo::EK_Custom32;

  // Otherwise, use the normal jump table encoding heuristics.
  return TargetLowering::getJumpTableEncoding();
}

const MCExpr *
X86TargetLowering::LowerCustomJumpTableEntry(const MachineJumpTableInfo *MJTI,
                                             const MachineBasicBlock *MBB,
                                             unsigned uid,MCContext &Ctx) const{
  assert(MBB->getParent()->getTarget().getRelocationModel() == Reloc::PIC_ &&
         Subtarget->isPICStyleGOT());
  // In 32-bit ELF systems, our jump table entries are formed with @GOTOFF
  // entries.
  return MCSymbolRefExpr::Create(MBB->getSymbol(),
                                 MCSymbolRefExpr::VK_GOTOFF, Ctx);
}

/// getPICJumpTableRelocaBase - Returns relocation base for the given PIC
/// jumptable.
SDValue X86TargetLowering::getPICJumpTableRelocBase(SDValue Table,
                                                    SelectionDAG &DAG) const {
  if (!Subtarget->is64Bit())
    // This doesn't have SDLoc associated with it, but is not really the
    // same as a Register.
    return DAG.getNode(X86ISD::GlobalBaseReg, SDLoc(), getPointerTy());
  return Table;
}

/// getPICJumpTableRelocBaseExpr - This returns the relocation base for the
/// given PIC jumptable, the same as getPICJumpTableRelocBase, but as an
/// MCExpr.
const MCExpr *X86TargetLowering::
getPICJumpTableRelocBaseExpr(const MachineFunction *MF, unsigned JTI,
                             MCContext &Ctx) const {
  // X86-64 uses RIP relative addressing based on the jump table label.
  if (Subtarget->isPICStyleRIPRel())
    return TargetLowering::getPICJumpTableRelocBaseExpr(MF, JTI, Ctx);

  // Otherwise, the reference is relative to the PIC base.
  return MCSymbolRefExpr::Create(MF->getPICBaseSymbol(), Ctx);
}

// FIXME: Why this routine is here? Move to RegInfo!
std::pair<const TargetRegisterClass*, uint8_t>
X86TargetLowering::findRepresentativeClass(MVT VT) const{
  const TargetRegisterClass *RRC = nullptr;
  uint8_t Cost = 1;
  switch (VT.SimpleTy) {
  default:
    return TargetLowering::findRepresentativeClass(VT);
  case MVT::i8: case MVT::i16: case MVT::i32: case MVT::i64:
    RRC = Subtarget->is64Bit() ? &X86::GR64RegClass : &X86::GR32RegClass;
    break;
  case MVT::x86mmx:
    RRC = &X86::VR64RegClass;
    break;
  case MVT::f32: case MVT::f64:
  case MVT::v16i8: case MVT::v8i16: case MVT::v4i32: case MVT::v2i64:
  case MVT::v4f32: case MVT::v2f64:
  case MVT::v32i8: case MVT::v8i32: case MVT::v4i64: case MVT::v8f32:
  case MVT::v4f64:
    RRC = &X86::VR128RegClass;
    break;
  }
  return std::make_pair(RRC, Cost);
}

bool X86TargetLowering::getStackCookieLocation(unsigned &AddressSpace,
                                               unsigned &Offset) const {
  if (!Subtarget->isTargetLinux())
    return false;

  if (Subtarget->is64Bit()) {
    // %fs:0x28, unless we're using a Kernel code model, in which case it's %gs:
    Offset = 0x28;
    if (getTargetMachine().getCodeModel() == CodeModel::Kernel)
      AddressSpace = 256;
    else
      AddressSpace = 257;
  } else {
    // %gs:0x14 on i386
    Offset = 0x14;
    AddressSpace = 256;
  }
  return true;
}

bool X86TargetLowering::isNoopAddrSpaceCast(unsigned SrcAS,
                                            unsigned DestAS) const {
  assert(SrcAS != DestAS && "Expected different address spaces!");

  return SrcAS < 256 && DestAS < 256;
}

//===----------------------------------------------------------------------===//
//               Return Value Calling Convention Implementation
//===----------------------------------------------------------------------===//

#include "X86GenCallingConv.inc"

bool
X86TargetLowering::CanLowerReturn(CallingConv::ID CallConv,
                                  MachineFunction &MF, bool isVarArg,
                        const SmallVectorImpl<ISD::OutputArg> &Outs,
                        LLVMContext &Context) const {
  SmallVector<CCValAssign, 16> RVLocs;
  CCState CCInfo(CallConv, isVarArg, MF, RVLocs, Context);
  return CCInfo.CheckReturn(Outs, RetCC_X86);
}

const MCPhysReg *X86TargetLowering::getScratchRegisters(CallingConv::ID) const {
  static const MCPhysReg ScratchRegs[] = { X86::R11, 0 };
  return ScratchRegs;
}

SDValue
X86TargetLowering::LowerReturn(SDValue Chain,
                               CallingConv::ID CallConv, bool isVarArg,
                               const SmallVectorImpl<ISD::OutputArg> &Outs,
                               const SmallVectorImpl<SDValue> &OutVals,
                               SDLoc dl, SelectionDAG &DAG) const {
  MachineFunction &MF = DAG.getMachineFunction();
  X86MachineFunctionInfo *FuncInfo = MF.getInfo<X86MachineFunctionInfo>();

  SmallVector<CCValAssign, 16> RVLocs;
  CCState CCInfo(CallConv, isVarArg, MF, RVLocs, *DAG.getContext());
  CCInfo.AnalyzeReturn(Outs, RetCC_X86);

  SDValue Flag;
  SmallVector<SDValue, 6> RetOps;
  RetOps.push_back(Chain); // Operand #0 = Chain (updated below)
  // Operand #1 = Bytes To Pop
  RetOps.push_back(DAG.getTargetConstant(FuncInfo->getBytesToPopOnReturn(),
                   MVT::i16));

  // Copy the result values into the output registers.
  for (unsigned i = 0; i != RVLocs.size(); ++i) {
    CCValAssign &VA = RVLocs[i];
    assert(VA.isRegLoc() && "Can only return in registers!");
    SDValue ValToCopy = OutVals[i];
    EVT ValVT = ValToCopy.getValueType();

    // Promote values to the appropriate types
    if (VA.getLocInfo() == CCValAssign::SExt)
      ValToCopy = DAG.getNode(ISD::SIGN_EXTEND, dl, VA.getLocVT(), ValToCopy);
    else if (VA.getLocInfo() == CCValAssign::ZExt)
      ValToCopy = DAG.getNode(ISD::ZERO_EXTEND, dl, VA.getLocVT(), ValToCopy);
    else if (VA.getLocInfo() == CCValAssign::AExt)
      ValToCopy = DAG.getNode(ISD::ANY_EXTEND, dl, VA.getLocVT(), ValToCopy);
    else if (VA.getLocInfo() == CCValAssign::BCvt)
      ValToCopy = DAG.getNode(ISD::BITCAST, dl, VA.getLocVT(), ValToCopy);

    assert(VA.getLocInfo() != CCValAssign::FPExt &&
           "Unexpected FP-extend for return value.");  

    // If this is x86-64, and we disabled SSE, we can't return FP values,
    // or SSE or MMX vectors.
    if ((ValVT == MVT::f32 || ValVT == MVT::f64 ||
         VA.getLocReg() == X86::XMM0 || VA.getLocReg() == X86::XMM1) &&
          (Subtarget->is64Bit() && !Subtarget->hasSSE1())) {
      report_fatal_error("SSE register return with SSE disabled");
    }
    // Likewise we can't return F64 values with SSE1 only.  gcc does so, but
    // llvm-gcc has never done it right and no one has noticed, so this
    // should be OK for now.
    if (ValVT == MVT::f64 &&
        (Subtarget->is64Bit() && !Subtarget->hasSSE2()))
      report_fatal_error("SSE2 register return with SSE2 disabled");

    // Returns in ST0/ST1 are handled specially: these are pushed as operands to
    // the RET instruction and handled by the FP Stackifier.
    if (VA.getLocReg() == X86::FP0 ||
        VA.getLocReg() == X86::FP1) {
      // If this is a copy from an xmm register to ST(0), use an FPExtend to
      // change the value to the FP stack register class.
      if (isScalarFPTypeInSSEReg(VA.getValVT()))
        ValToCopy = DAG.getNode(ISD::FP_EXTEND, dl, MVT::f80, ValToCopy);
      RetOps.push_back(ValToCopy);
      // Don't emit a copytoreg.
      continue;
    }

    // 64-bit vector (MMX) values are returned in XMM0 / XMM1 except for v1i64
    // which is returned in RAX / RDX.
    if (Subtarget->is64Bit()) {
      if (ValVT == MVT::x86mmx) {
        if (VA.getLocReg() == X86::XMM0 || VA.getLocReg() == X86::XMM1) {
          ValToCopy = DAG.getNode(ISD::BITCAST, dl, MVT::i64, ValToCopy);
          ValToCopy = DAG.getNode(ISD::SCALAR_TO_VECTOR, dl, MVT::v2i64,
                                  ValToCopy);
          // If we don't have SSE2 available, convert to v4f32 so the generated
          // register is legal.
          if (!Subtarget->hasSSE2())
            ValToCopy = DAG.getNode(ISD::BITCAST, dl, MVT::v4f32,ValToCopy);
        }
      }
    }

    Chain = DAG.getCopyToReg(Chain, dl, VA.getLocReg(), ValToCopy, Flag);
    Flag = Chain.getValue(1);
    RetOps.push_back(DAG.getRegister(VA.getLocReg(), VA.getLocVT()));
  }

  // The x86-64 ABIs require that for returning structs by value we copy
  // the sret argument into %rax/%eax (depending on ABI) for the return.
  // Win32 requires us to put the sret argument to %eax as well.
  // We saved the argument into a virtual register in the entry block,
  // so now we copy the value out and into %rax/%eax.
  if (DAG.getMachineFunction().getFunction()->hasStructRetAttr() &&
      (Subtarget->is64Bit() || Subtarget->isTargetKnownWindowsMSVC())) {
    MachineFunction &MF = DAG.getMachineFunction();
    X86MachineFunctionInfo *FuncInfo = MF.getInfo<X86MachineFunctionInfo>();
    unsigned Reg = FuncInfo->getSRetReturnReg();
    assert(Reg &&
           "SRetReturnReg should have been set in LowerFormalArguments().");
    SDValue Val = DAG.getCopyFromReg(Chain, dl, Reg, getPointerTy());

    unsigned RetValReg
        = (Subtarget->is64Bit() && !Subtarget->isTarget64BitILP32()) ?
          X86::RAX : X86::EAX;
    Chain = DAG.getCopyToReg(Chain, dl, RetValReg, Val, Flag);
    Flag = Chain.getValue(1);

    // RAX/EAX now acts like a return value.
    RetOps.push_back(DAG.getRegister(RetValReg, getPointerTy()));
  }

  RetOps[0] = Chain;  // Update chain.

  // Add the flag if we have it.
  if (Flag.getNode())
    RetOps.push_back(Flag);

  return DAG.getNode(X86ISD::RET_FLAG, dl, MVT::Other, RetOps);
}

bool X86TargetLowering::isUsedByReturnOnly(SDNode *N, SDValue &Chain) const {
  if (N->getNumValues() != 1)
    return false;
  if (!N->hasNUsesOfValue(1, 0))
    return false;

  SDValue TCChain = Chain;
  SDNode *Copy = *N->use_begin();
  if (Copy->getOpcode() == ISD::CopyToReg) {
    // If the copy has a glue operand, we conservatively assume it isn't safe to
    // perform a tail call.
    if (Copy->getOperand(Copy->getNumOperands()-1).getValueType() == MVT::Glue)
      return false;
    TCChain = Copy->getOperand(0);
  } else if (Copy->getOpcode() != ISD::FP_EXTEND)
    return false;

  bool HasRet = false;
  for (SDNode::use_iterator UI = Copy->use_begin(), UE = Copy->use_end();
       UI != UE; ++UI) {
    if (UI->getOpcode() != X86ISD::RET_FLAG)
      return false;
    // If we are returning more than one value, we can definitely
    // not make a tail call see PR19530
    if (UI->getNumOperands() > 4)
      return false;
    if (UI->getNumOperands() == 4 &&
        UI->getOperand(UI->getNumOperands()-1).getValueType() != MVT::Glue)
      return false;
    HasRet = true;
  }

  if (!HasRet)
    return false;

  Chain = TCChain;
  return true;
}

EVT
X86TargetLowering::getTypeForExtArgOrReturn(LLVMContext &Context, EVT VT,
                                            ISD::NodeType ExtendKind) const {
  MVT ReturnMVT;
  // TODO: Is this also valid on 32-bit?
  if (Subtarget->is64Bit() && VT == MVT::i1 && ExtendKind == ISD::ZERO_EXTEND)
    ReturnMVT = MVT::i8;
  else
    ReturnMVT = MVT::i32;

  EVT MinVT = getRegisterType(Context, ReturnMVT);
  return VT.bitsLT(MinVT) ? MinVT : VT;
}

/// LowerCallResult - Lower the result values of a call into the
/// appropriate copies out of appropriate physical registers.
///
SDValue
X86TargetLowering::LowerCallResult(SDValue Chain, SDValue InFlag,
                                   CallingConv::ID CallConv, bool isVarArg,
                                   const SmallVectorImpl<ISD::InputArg> &Ins,
                                   SDLoc dl, SelectionDAG &DAG,
                                   SmallVectorImpl<SDValue> &InVals) const {

  // Assign locations to each value returned by this call.
  SmallVector<CCValAssign, 16> RVLocs;
  bool Is64Bit = Subtarget->is64Bit();
  CCState CCInfo(CallConv, isVarArg, DAG.getMachineFunction(), RVLocs,
                 *DAG.getContext());
  CCInfo.AnalyzeCallResult(Ins, RetCC_X86);

  // Copy all of the result registers out of their specified physreg.
  for (unsigned i = 0, e = RVLocs.size(); i != e; ++i) {
    CCValAssign &VA = RVLocs[i];
    EVT CopyVT = VA.getValVT();

    // If this is x86-64, and we disabled SSE, we can't return FP values
    if ((CopyVT == MVT::f32 || CopyVT == MVT::f64) &&
        ((Is64Bit || Ins[i].Flags.isInReg()) && !Subtarget->hasSSE1())) {
      report_fatal_error("SSE register return with SSE disabled");
    }

    // If we prefer to use the value in xmm registers, copy it out as f80 and
    // use a truncate to move it from fp stack reg to xmm reg.
    if ((VA.getLocReg() == X86::FP0 || VA.getLocReg() == X86::FP1) &&
        isScalarFPTypeInSSEReg(VA.getValVT()))
      CopyVT = MVT::f80;

    Chain = DAG.getCopyFromReg(Chain, dl, VA.getLocReg(),
                               CopyVT, InFlag).getValue(1);
    SDValue Val = Chain.getValue(0);

    if (CopyVT != VA.getValVT())
      Val = DAG.getNode(ISD::FP_ROUND, dl, VA.getValVT(), Val,
                        // This truncation won't change the value.
                        DAG.getIntPtrConstant(1));

    InFlag = Chain.getValue(2);
    InVals.push_back(Val);
  }

  return Chain;
}

//===----------------------------------------------------------------------===//
//                C & StdCall & Fast Calling Convention implementation
//===----------------------------------------------------------------------===//
//  StdCall calling convention seems to be standard for many Windows' API
//  routines and around. It differs from C calling convention just a little:
//  callee should clean up the stack, not caller. Symbols should be also
//  decorated in some fancy way :) It doesn't support any vector arguments.
//  For info on fast calling convention see Fast Calling Convention (tail call)
//  implementation LowerX86_32FastCCCallTo.

/// CallIsStructReturn - Determines whether a call uses struct return
/// semantics.
enum StructReturnType {
  NotStructReturn,
  RegStructReturn,
  StackStructReturn
};
static StructReturnType
callIsStructReturn(const SmallVectorImpl<ISD::OutputArg> &Outs) {
  if (Outs.empty())
    return NotStructReturn;

  const ISD::ArgFlagsTy &Flags = Outs[0].Flags;
  if (!Flags.isSRet())
    return NotStructReturn;
  if (Flags.isInReg())
    return RegStructReturn;
  return StackStructReturn;
}

/// ArgsAreStructReturn - Determines whether a function uses struct
/// return semantics.
static StructReturnType
argsAreStructReturn(const SmallVectorImpl<ISD::InputArg> &Ins) {
  if (Ins.empty())
    return NotStructReturn;

  const ISD::ArgFlagsTy &Flags = Ins[0].Flags;
  if (!Flags.isSRet())
    return NotStructReturn;
  if (Flags.isInReg())
    return RegStructReturn;
  return StackStructReturn;
}

/// CreateCopyOfByValArgument - Make a copy of an aggregate at address specified
/// by "Src" to address "Dst" with size and alignment information specified by
/// the specific parameter attribute. The copy will be passed as a byval
/// function parameter.
static SDValue
CreateCopyOfByValArgument(SDValue Src, SDValue Dst, SDValue Chain,
                          ISD::ArgFlagsTy Flags, SelectionDAG &DAG,
                          SDLoc dl) {
  SDValue SizeNode = DAG.getConstant(Flags.getByValSize(), MVT::i32);

  return DAG.getMemcpy(Chain, dl, Dst, Src, SizeNode, Flags.getByValAlign(),
                       /*isVolatile*/false, /*AlwaysInline=*/true,
                       MachinePointerInfo(), MachinePointerInfo());
}

/// IsTailCallConvention - Return true if the calling convention is one that
/// supports tail call optimization.
static bool IsTailCallConvention(CallingConv::ID CC) {
  return (CC == CallingConv::Fast || CC == CallingConv::GHC ||
          CC == CallingConv::HiPE);
}

/// \brief Return true if the calling convention is a C calling convention.
static bool IsCCallConvention(CallingConv::ID CC) {
  return (CC == CallingConv::C || CC == CallingConv::X86_64_Win64 ||
          CC == CallingConv::X86_64_SysV);
}

bool X86TargetLowering::mayBeEmittedAsTailCall(CallInst *CI) const {
  if (!CI->isTailCall() || getTargetMachine().Options.DisableTailCalls)
    return false;

  CallSite CS(CI);
  CallingConv::ID CalleeCC = CS.getCallingConv();
  if (!IsTailCallConvention(CalleeCC) && !IsCCallConvention(CalleeCC))
    return false;

  return true;
}

/// FuncIsMadeTailCallSafe - Return true if the function is being made into
/// a tailcall target by changing its ABI.
static bool FuncIsMadeTailCallSafe(CallingConv::ID CC,
                                   bool GuaranteedTailCallOpt) {
  return GuaranteedTailCallOpt && IsTailCallConvention(CC);
}

SDValue
X86TargetLowering::LowerMemArgument(SDValue Chain,
                                    CallingConv::ID CallConv,
                                    const SmallVectorImpl<ISD::InputArg> &Ins,
                                    SDLoc dl, SelectionDAG &DAG,
                                    const CCValAssign &VA,
                                    MachineFrameInfo *MFI,
                                    unsigned i) const {
  // Create the nodes corresponding to a load from this parameter slot.
  ISD::ArgFlagsTy Flags = Ins[i].Flags;
  bool AlwaysUseMutable = FuncIsMadeTailCallSafe(
      CallConv, DAG.getTarget().Options.GuaranteedTailCallOpt);
  bool isImmutable = !AlwaysUseMutable && !Flags.isByVal();
  EVT ValVT;

  // If value is passed by pointer we have address passed instead of the value
  // itself.
  if (VA.getLocInfo() == CCValAssign::Indirect)
    ValVT = VA.getLocVT();
  else
    ValVT = VA.getValVT();

  // FIXME: For now, all byval parameter objects are marked mutable. This can be
  // changed with more analysis.
  // In case of tail call optimization mark all arguments mutable. Since they
  // could be overwritten by lowering of arguments in case of a tail call.
  if (Flags.isByVal()) {
    unsigned Bytes = Flags.getByValSize();
    if (Bytes == 0) Bytes = 1; // Don't create zero-sized stack objects.
    int FI = MFI->CreateFixedObject(Bytes, VA.getLocMemOffset(), isImmutable);
    return DAG.getFrameIndex(FI, getPointerTy());
  } else {
    int FI = MFI->CreateFixedObject(ValVT.getSizeInBits()/8,
                                    VA.getLocMemOffset(), isImmutable);
    SDValue FIN = DAG.getFrameIndex(FI, getPointerTy());
    return DAG.getLoad(ValVT, dl, Chain, FIN,
                       MachinePointerInfo::getFixedStack(FI),
                       false, false, false, 0);
  }
}

// FIXME: Get this from tablegen.
static ArrayRef<MCPhysReg> get64BitArgumentGPRs(CallingConv::ID CallConv,
                                                const X86Subtarget *Subtarget) {
  assert(Subtarget->is64Bit());

  if (Subtarget->isCallingConvWin64(CallConv)) {
    static const MCPhysReg GPR64ArgRegsWin64[] = {
      X86::RCX, X86::RDX, X86::R8,  X86::R9
    };
    return makeArrayRef(std::begin(GPR64ArgRegsWin64), std::end(GPR64ArgRegsWin64));
  }

  static const MCPhysReg GPR64ArgRegs64Bit[] = {
    X86::RDI, X86::RSI, X86::RDX, X86::RCX, X86::R8, X86::R9
  };
  return makeArrayRef(std::begin(GPR64ArgRegs64Bit), std::end(GPR64ArgRegs64Bit));
}

// FIXME: Get this from tablegen.
static ArrayRef<MCPhysReg> get64BitArgumentXMMs(MachineFunction &MF,
                                                CallingConv::ID CallConv,
                                                const X86Subtarget *Subtarget) {
  assert(Subtarget->is64Bit());
  if (Subtarget->isCallingConvWin64(CallConv)) {
    // The XMM registers which might contain var arg parameters are shadowed
    // in their paired GPR.  So we only need to save the GPR to their home
    // slots.
    // TODO: __vectorcall will change this.
    return None;
  }

  const Function *Fn = MF.getFunction();
  bool NoImplicitFloatOps = Fn->getAttributes().
      hasAttribute(AttributeSet::FunctionIndex, Attribute::NoImplicitFloat);
  assert(!(MF.getTarget().Options.UseSoftFloat && NoImplicitFloatOps) &&
         "SSE register cannot be used when SSE is disabled!");
  if (MF.getTarget().Options.UseSoftFloat || NoImplicitFloatOps ||
      !Subtarget->hasSSE1())
    // Kernel mode asks for SSE to be disabled, so there are no XMM argument
    // registers.
    return None;

  static const MCPhysReg XMMArgRegs64Bit[] = {
    X86::XMM0, X86::XMM1, X86::XMM2, X86::XMM3,
    X86::XMM4, X86::XMM5, X86::XMM6, X86::XMM7
  };
  return makeArrayRef(std::begin(XMMArgRegs64Bit), std::end(XMMArgRegs64Bit));
}

SDValue
X86TargetLowering::LowerFormalArguments(SDValue Chain,
                                        CallingConv::ID CallConv,
                                        bool isVarArg,
                                      const SmallVectorImpl<ISD::InputArg> &Ins,
                                        SDLoc dl,
                                        SelectionDAG &DAG,
                                        SmallVectorImpl<SDValue> &InVals)
                                          const {
  MachineFunction &MF = DAG.getMachineFunction();
  X86MachineFunctionInfo *FuncInfo = MF.getInfo<X86MachineFunctionInfo>();

  const Function* Fn = MF.getFunction();
  if (Fn->hasExternalLinkage() &&
      Subtarget->isTargetCygMing() &&
      Fn->getName() == "main")
    FuncInfo->setForceFramePointer(true);

  MachineFrameInfo *MFI = MF.getFrameInfo();
  bool Is64Bit = Subtarget->is64Bit();
  bool IsWin64 = Subtarget->isCallingConvWin64(CallConv);

  assert(!(isVarArg && IsTailCallConvention(CallConv)) &&
         "Var args not supported with calling convention fastcc, ghc or hipe");

  // Assign locations to all of the incoming arguments.
  SmallVector<CCValAssign, 16> ArgLocs;
  CCState CCInfo(CallConv, isVarArg, MF, ArgLocs, *DAG.getContext());

  // Allocate shadow area for Win64
  if (IsWin64)
    CCInfo.AllocateStack(32, 8);

  CCInfo.AnalyzeFormalArguments(Ins, CC_X86);

  unsigned LastVal = ~0U;
  SDValue ArgValue;
  for (unsigned i = 0, e = ArgLocs.size(); i != e; ++i) {
    CCValAssign &VA = ArgLocs[i];
    // TODO: If an arg is passed in two places (e.g. reg and stack), skip later
    // places.
    assert(VA.getValNo() != LastVal &&
           "Don't support value assigned to multiple locs yet");
    (void)LastVal;
    LastVal = VA.getValNo();

    if (VA.isRegLoc()) {
      EVT RegVT = VA.getLocVT();
      const TargetRegisterClass *RC;
      if (RegVT == MVT::i32)
        RC = &X86::GR32RegClass;
      else if (Is64Bit && RegVT == MVT::i64)
        RC = &X86::GR64RegClass;
      else if (RegVT == MVT::f32)
        RC = &X86::FR32RegClass;
      else if (RegVT == MVT::f64)
        RC = &X86::FR64RegClass;
      else if (RegVT.is512BitVector())
        RC = &X86::VR512RegClass;
      else if (RegVT.is256BitVector())
        RC = &X86::VR256RegClass;
      else if (RegVT.is128BitVector())
        RC = &X86::VR128RegClass;
      else if (RegVT == MVT::x86mmx)
        RC = &X86::VR64RegClass;
      else if (RegVT == MVT::i1)
        RC = &X86::VK1RegClass;
      else if (RegVT == MVT::v8i1)
        RC = &X86::VK8RegClass;
      else if (RegVT == MVT::v16i1)
        RC = &X86::VK16RegClass;
      else if (RegVT == MVT::v32i1)
        RC = &X86::VK32RegClass;
      else if (RegVT == MVT::v64i1)
        RC = &X86::VK64RegClass;
      else
        llvm_unreachable("Unknown argument type!");

      unsigned Reg = MF.addLiveIn(VA.getLocReg(), RC);
      ArgValue = DAG.getCopyFromReg(Chain, dl, Reg, RegVT);

      // If this is an 8 or 16-bit value, it is really passed promoted to 32
      // bits.  Insert an assert[sz]ext to capture this, then truncate to the
      // right size.
      if (VA.getLocInfo() == CCValAssign::SExt)
        ArgValue = DAG.getNode(ISD::AssertSext, dl, RegVT, ArgValue,
                               DAG.getValueType(VA.getValVT()));
      else if (VA.getLocInfo() == CCValAssign::ZExt)
        ArgValue = DAG.getNode(ISD::AssertZext, dl, RegVT, ArgValue,
                               DAG.getValueType(VA.getValVT()));
      else if (VA.getLocInfo() == CCValAssign::BCvt)
        ArgValue = DAG.getNode(ISD::BITCAST, dl, VA.getValVT(), ArgValue);

      if (VA.isExtInLoc()) {
        // Handle MMX values passed in XMM regs.
        if (RegVT.isVector())
          ArgValue = DAG.getNode(X86ISD::MOVDQ2Q, dl, VA.getValVT(), ArgValue);
        else
          ArgValue = DAG.getNode(ISD::TRUNCATE, dl, VA.getValVT(), ArgValue);
      }
    } else {
      assert(VA.isMemLoc());
      ArgValue = LowerMemArgument(Chain, CallConv, Ins, dl, DAG, VA, MFI, i);
    }

    // If value is passed via pointer - do a load.
    if (VA.getLocInfo() == CCValAssign::Indirect)
      ArgValue = DAG.getLoad(VA.getValVT(), dl, Chain, ArgValue,
                             MachinePointerInfo(), false, false, false, 0);

    InVals.push_back(ArgValue);
  }

  if (Subtarget->is64Bit() || Subtarget->isTargetKnownWindowsMSVC()) {
    for (unsigned i = 0, e = ArgLocs.size(); i != e; ++i) {
      // The x86-64 ABIs require that for returning structs by value we copy
      // the sret argument into %rax/%eax (depending on ABI) for the return.
      // Win32 requires us to put the sret argument to %eax as well.
      // Save the argument into a virtual register so that we can access it
      // from the return points.
      if (Ins[i].Flags.isSRet()) {
        unsigned Reg = FuncInfo->getSRetReturnReg();
        if (!Reg) {
          MVT PtrTy = getPointerTy();
          Reg = MF.getRegInfo().createVirtualRegister(getRegClassFor(PtrTy));
          FuncInfo->setSRetReturnReg(Reg);
        }
        SDValue Copy = DAG.getCopyToReg(DAG.getEntryNode(), dl, Reg, InVals[i]);
        Chain = DAG.getNode(ISD::TokenFactor, dl, MVT::Other, Copy, Chain);
        break;
      }
    }
  }

  unsigned StackSize = CCInfo.getNextStackOffset();
  // Align stack specially for tail calls.
  if (FuncIsMadeTailCallSafe(CallConv,
                             MF.getTarget().Options.GuaranteedTailCallOpt))
    StackSize = GetAlignedArgumentStackSize(StackSize, DAG);

  // If the function takes variable number of arguments, make a frame index for
  // the start of the first vararg value... for expansion of llvm.va_start. We
  // can skip this if there are no va_start calls.
  if (MFI->hasVAStart() &&
      (Is64Bit || (CallConv != CallingConv::X86_FastCall &&
                   CallConv != CallingConv::X86_ThisCall))) {
    FuncInfo->setVarArgsFrameIndex(
        MFI->CreateFixedObject(1, StackSize, true));
  }

  // 64-bit calling conventions support varargs and register parameters, so we
  // have to do extra work to spill them in the prologue or forward them to
  // musttail calls.
  if (Is64Bit && isVarArg &&
      (MFI->hasVAStart() || MFI->hasMustTailInVarArgFunc())) {
    // Find the first unallocated argument registers.
    ArrayRef<MCPhysReg> ArgGPRs = get64BitArgumentGPRs(CallConv, Subtarget);
    ArrayRef<MCPhysReg> ArgXMMs = get64BitArgumentXMMs(MF, CallConv, Subtarget);
    unsigned NumIntRegs =
        CCInfo.getFirstUnallocated(ArgGPRs.data(), ArgGPRs.size());
    unsigned NumXMMRegs =
        CCInfo.getFirstUnallocated(ArgXMMs.data(), ArgXMMs.size());
    assert(!(NumXMMRegs && !Subtarget->hasSSE1()) &&
           "SSE register cannot be used when SSE is disabled!");

    // Gather all the live in physical registers.
    SmallVector<SDValue, 6> LiveGPRs;
    SmallVector<SDValue, 8> LiveXMMRegs;
    SDValue ALVal;
    for (MCPhysReg Reg : ArgGPRs.slice(NumIntRegs)) {
      unsigned GPR = MF.addLiveIn(Reg, &X86::GR64RegClass);
      LiveGPRs.push_back(
          DAG.getCopyFromReg(DAG.getEntryNode(), dl, GPR, MVT::i64));
    }
    if (!ArgXMMs.empty()) {
      unsigned AL = MF.addLiveIn(X86::AL, &X86::GR8RegClass);
      ALVal = DAG.getCopyFromReg(DAG.getEntryNode(), dl, AL, MVT::i8);
      for (MCPhysReg Reg : ArgXMMs.slice(NumXMMRegs)) {
        unsigned XMMReg = MF.addLiveIn(Reg, &X86::VR128RegClass);
        LiveXMMRegs.push_back(
            DAG.getCopyFromReg(Chain, dl, XMMReg, MVT::v4f32));
      }
    }

    // Store them to the va_list returned by va_start.
    if (MFI->hasVAStart()) {
      if (IsWin64) {
        const TargetFrameLowering &TFI = *MF.getSubtarget().getFrameLowering();
        // Get to the caller-allocated home save location.  Add 8 to account
        // for the return address.
        int HomeOffset = TFI.getOffsetOfLocalArea() + 8;
        FuncInfo->setRegSaveFrameIndex(
          MFI->CreateFixedObject(1, NumIntRegs * 8 + HomeOffset, false));
        // Fixup to set vararg frame on shadow area (4 x i64).
        if (NumIntRegs < 4)
          FuncInfo->setVarArgsFrameIndex(FuncInfo->getRegSaveFrameIndex());
      } else {
        // For X86-64, if there are vararg parameters that are passed via
        // registers, then we must store them to their spots on the stack so
        // they may be loaded by deferencing the result of va_next.
        FuncInfo->setVarArgsGPOffset(NumIntRegs * 8);
        FuncInfo->setVarArgsFPOffset(ArgGPRs.size() * 8 + NumXMMRegs * 16);
        FuncInfo->setRegSaveFrameIndex(MFI->CreateStackObject(
            ArgGPRs.size() * 8 + ArgXMMs.size() * 16, 16, false));
      }

      // Store the integer parameter registers.
      SmallVector<SDValue, 8> MemOps;
      SDValue RSFIN = DAG.getFrameIndex(FuncInfo->getRegSaveFrameIndex(),
                                        getPointerTy());
      unsigned Offset = FuncInfo->getVarArgsGPOffset();
      for (SDValue Val : LiveGPRs) {
        SDValue FIN = DAG.getNode(ISD::ADD, dl, getPointerTy(), RSFIN,
                                  DAG.getIntPtrConstant(Offset));
        SDValue Store =
          DAG.getStore(Val.getValue(1), dl, Val, FIN,
                       MachinePointerInfo::getFixedStack(
                         FuncInfo->getRegSaveFrameIndex(), Offset),
                       false, false, 0);
        MemOps.push_back(Store);
        Offset += 8;
      }

      if (!ArgXMMs.empty() && NumXMMRegs != ArgXMMs.size()) {
        // Now store the XMM (fp + vector) parameter registers.
        SmallVector<SDValue, 12> SaveXMMOps;
        SaveXMMOps.push_back(Chain);
        SaveXMMOps.push_back(ALVal);
        SaveXMMOps.push_back(DAG.getIntPtrConstant(
                               FuncInfo->getRegSaveFrameIndex()));
        SaveXMMOps.push_back(DAG.getIntPtrConstant(
                               FuncInfo->getVarArgsFPOffset()));
        SaveXMMOps.insert(SaveXMMOps.end(), LiveXMMRegs.begin(),
                          LiveXMMRegs.end());
        MemOps.push_back(DAG.getNode(X86ISD::VASTART_SAVE_XMM_REGS, dl,
                                     MVT::Other, SaveXMMOps));
      }

      if (!MemOps.empty())
        Chain = DAG.getNode(ISD::TokenFactor, dl, MVT::Other, MemOps);
    } else {
      // Add all GPRs, al, and XMMs to the list of forwards.  We will add then
      // to the liveout set on a musttail call.
      assert(MFI->hasMustTailInVarArgFunc());
      auto &Forwards = FuncInfo->getForwardedMustTailRegParms();
      typedef X86MachineFunctionInfo::Forward Forward;

      for (unsigned I = 0, E = LiveGPRs.size(); I != E; ++I) {
        unsigned VReg =
            MF.getRegInfo().createVirtualRegister(&X86::GR64RegClass);
        Chain = DAG.getCopyToReg(Chain, dl, VReg, LiveGPRs[I]);
        Forwards.push_back(Forward(VReg, ArgGPRs[NumIntRegs + I], MVT::i64));
      }

      if (!ArgXMMs.empty()) {
        unsigned ALVReg =
            MF.getRegInfo().createVirtualRegister(&X86::GR8RegClass);
        Chain = DAG.getCopyToReg(Chain, dl, ALVReg, ALVal);
        Forwards.push_back(Forward(ALVReg, X86::AL, MVT::i8));

        for (unsigned I = 0, E = LiveXMMRegs.size(); I != E; ++I) {
          unsigned VReg =
              MF.getRegInfo().createVirtualRegister(&X86::VR128RegClass);
          Chain = DAG.getCopyToReg(Chain, dl, VReg, LiveXMMRegs[I]);
          Forwards.push_back(
              Forward(VReg, ArgXMMs[NumXMMRegs + I], MVT::v4f32));
        }
      }
    }
  }

  // Some CCs need callee pop.
  if (X86::isCalleePop(CallConv, Is64Bit, isVarArg,
                       MF.getTarget().Options.GuaranteedTailCallOpt)) {
    FuncInfo->setBytesToPopOnReturn(StackSize); // Callee pops everything.
  } else {
    FuncInfo->setBytesToPopOnReturn(0); // Callee pops nothing.
    // If this is an sret function, the return should pop the hidden pointer.
    if (!Is64Bit && !IsTailCallConvention(CallConv) &&
        !Subtarget->getTargetTriple().isOSMSVCRT() &&
        argsAreStructReturn(Ins) == StackStructReturn)
      FuncInfo->setBytesToPopOnReturn(4);
  }

  if (!Is64Bit) {
    // RegSaveFrameIndex is X86-64 only.
    FuncInfo->setRegSaveFrameIndex(0xAAAAAAA);
    if (CallConv == CallingConv::X86_FastCall ||
        CallConv == CallingConv::X86_ThisCall)
      // fastcc functions can't have varargs.
      FuncInfo->setVarArgsFrameIndex(0xAAAAAAA);
  }

  FuncInfo->setArgumentStackSize(StackSize);

  return Chain;
}

SDValue
X86TargetLowering::LowerMemOpCallTo(SDValue Chain,
                                    SDValue StackPtr, SDValue Arg,
                                    SDLoc dl, SelectionDAG &DAG,
                                    const CCValAssign &VA,
                                    ISD::ArgFlagsTy Flags) const {
  unsigned LocMemOffset = VA.getLocMemOffset();
  SDValue PtrOff = DAG.getIntPtrConstant(LocMemOffset);
  PtrOff = DAG.getNode(ISD::ADD, dl, getPointerTy(), StackPtr, PtrOff);
  if (Flags.isByVal())
    return CreateCopyOfByValArgument(Arg, PtrOff, Chain, Flags, DAG, dl);

  return DAG.getStore(Chain, dl, Arg, PtrOff,
                      MachinePointerInfo::getStack(LocMemOffset),
                      false, false, 0);
}

/// EmitTailCallLoadRetAddr - Emit a load of return address if tail call
/// optimization is performed and it is required.
SDValue
X86TargetLowering::EmitTailCallLoadRetAddr(SelectionDAG &DAG,
                                           SDValue &OutRetAddr, SDValue Chain,
                                           bool IsTailCall, bool Is64Bit,
                                           int FPDiff, SDLoc dl) const {
  // Adjust the Return address stack slot.
  EVT VT = getPointerTy();
  OutRetAddr = getReturnAddressFrameIndex(DAG);

  // Load the "old" Return address.
  OutRetAddr = DAG.getLoad(VT, dl, Chain, OutRetAddr, MachinePointerInfo(),
                           false, false, false, 0);
  return SDValue(OutRetAddr.getNode(), 1);
}

/// EmitTailCallStoreRetAddr - Emit a store of the return address if tail call
/// optimization is performed and it is required (FPDiff!=0).
static SDValue EmitTailCallStoreRetAddr(SelectionDAG &DAG, MachineFunction &MF,
                                        SDValue Chain, SDValue RetAddrFrIdx,
                                        EVT PtrVT, unsigned SlotSize,
                                        int FPDiff, SDLoc dl) {
  // Store the return address to the appropriate stack slot.
  if (!FPDiff) return Chain;
  // Calculate the new stack slot for the return address.
  int NewReturnAddrFI =
    MF.getFrameInfo()->CreateFixedObject(SlotSize, (int64_t)FPDiff - SlotSize,
                                         false);
  SDValue NewRetAddrFrIdx = DAG.getFrameIndex(NewReturnAddrFI, PtrVT);
  Chain = DAG.getStore(Chain, dl, RetAddrFrIdx, NewRetAddrFrIdx,
                       MachinePointerInfo::getFixedStack(NewReturnAddrFI),
                       false, false, 0);
  return Chain;
}

SDValue
X86TargetLowering::LowerCall(TargetLowering::CallLoweringInfo &CLI,
                             SmallVectorImpl<SDValue> &InVals) const {
  SelectionDAG &DAG                     = CLI.DAG;
  SDLoc &dl                             = CLI.DL;
  SmallVectorImpl<ISD::OutputArg> &Outs = CLI.Outs;
  SmallVectorImpl<SDValue> &OutVals     = CLI.OutVals;
  SmallVectorImpl<ISD::InputArg> &Ins   = CLI.Ins;
  SDValue Chain                         = CLI.Chain;
  SDValue Callee                        = CLI.Callee;
  CallingConv::ID CallConv              = CLI.CallConv;
  bool &isTailCall                      = CLI.IsTailCall;
  bool isVarArg                         = CLI.IsVarArg;

  MachineFunction &MF = DAG.getMachineFunction();
  bool Is64Bit        = Subtarget->is64Bit();
  bool IsWin64        = Subtarget->isCallingConvWin64(CallConv);
  StructReturnType SR = callIsStructReturn(Outs);
  bool IsSibcall      = false;
  X86MachineFunctionInfo *X86Info = MF.getInfo<X86MachineFunctionInfo>();

  if (MF.getTarget().Options.DisableTailCalls)
    isTailCall = false;

  bool IsMustTail = CLI.CS && CLI.CS->isMustTailCall();
  if (IsMustTail) {
    // Force this to be a tail call.  The verifier rules are enough to ensure
    // that we can lower this successfully without moving the return address
    // around.
    isTailCall = true;
  } else if (isTailCall) {
    // Check if it's really possible to do a tail call.
    isTailCall = IsEligibleForTailCallOptimization(Callee, CallConv,
                    isVarArg, SR != NotStructReturn,
                    MF.getFunction()->hasStructRetAttr(), CLI.RetTy,
                    Outs, OutVals, Ins, DAG);

    // Sibcalls are automatically detected tailcalls which do not require
    // ABI changes.
    if (!MF.getTarget().Options.GuaranteedTailCallOpt && isTailCall)
      IsSibcall = true;

    if (isTailCall)
      ++NumTailCalls;
  }

  assert(!(isVarArg && IsTailCallConvention(CallConv)) &&
         "Var args not supported with calling convention fastcc, ghc or hipe");

  // Analyze operands of the call, assigning locations to each operand.
  SmallVector<CCValAssign, 16> ArgLocs;
  CCState CCInfo(CallConv, isVarArg, MF, ArgLocs, *DAG.getContext());

  // Allocate shadow area for Win64
  if (IsWin64)
    CCInfo.AllocateStack(32, 8);

  CCInfo.AnalyzeCallOperands(Outs, CC_X86);

  // Get a count of how many bytes are to be pushed on the stack.
  unsigned NumBytes = CCInfo.getNextStackOffset();
  if (IsSibcall)
    // This is a sibcall. The memory operands are available in caller's
    // own caller's stack.
    NumBytes = 0;
  else if (MF.getTarget().Options.GuaranteedTailCallOpt &&
           IsTailCallConvention(CallConv))
    NumBytes = GetAlignedArgumentStackSize(NumBytes, DAG);

  int FPDiff = 0;
  if (isTailCall && !IsSibcall && !IsMustTail) {
    // Lower arguments at fp - stackoffset + fpdiff.
    unsigned NumBytesCallerPushed = X86Info->getBytesToPopOnReturn();

    FPDiff = NumBytesCallerPushed - NumBytes;

    // Set the delta of movement of the returnaddr stackslot.
    // But only set if delta is greater than previous delta.
    if (FPDiff < X86Info->getTCReturnAddrDelta())
      X86Info->setTCReturnAddrDelta(FPDiff);
  }

  unsigned NumBytesToPush = NumBytes;
  unsigned NumBytesToPop = NumBytes;

  // If we have an inalloca argument, all stack space has already been allocated
  // for us and be right at the top of the stack.  We don't support multiple
  // arguments passed in memory when using inalloca.
  if (!Outs.empty() && Outs.back().Flags.isInAlloca()) {
    NumBytesToPush = 0;
    if (!ArgLocs.back().isMemLoc())
      report_fatal_error("cannot use inalloca attribute on a register "
                         "parameter");
    if (ArgLocs.back().getLocMemOffset() != 0)
      report_fatal_error("any parameter with the inalloca attribute must be "
                         "the only memory argument");
  }

  if (!IsSibcall)
    Chain = DAG.getCALLSEQ_START(
        Chain, DAG.getIntPtrConstant(NumBytesToPush, true), dl);

  SDValue RetAddrFrIdx;
  // Load return address for tail calls.
  if (isTailCall && FPDiff)
    Chain = EmitTailCallLoadRetAddr(DAG, RetAddrFrIdx, Chain, isTailCall,
                                    Is64Bit, FPDiff, dl);

  SmallVector<std::pair<unsigned, SDValue>, 8> RegsToPass;
  SmallVector<SDValue, 8> MemOpChains;
  SDValue StackPtr;

  // Walk the register/memloc assignments, inserting copies/loads.  In the case
  // of tail call optimization arguments are handle later.
  const X86RegisterInfo *RegInfo = static_cast<const X86RegisterInfo *>(
      DAG.getSubtarget().getRegisterInfo());
  for (unsigned i = 0, e = ArgLocs.size(); i != e; ++i) {
    // Skip inalloca arguments, they have already been written.
    ISD::ArgFlagsTy Flags = Outs[i].Flags;
    if (Flags.isInAlloca())
      continue;

    CCValAssign &VA = ArgLocs[i];
    EVT RegVT = VA.getLocVT();
    SDValue Arg = OutVals[i];
    bool isByVal = Flags.isByVal();

    // Promote the value if needed.
    switch (VA.getLocInfo()) {
    default: llvm_unreachable("Unknown loc info!");
    case CCValAssign::Full: break;
    case CCValAssign::SExt:
      Arg = DAG.getNode(ISD::SIGN_EXTEND, dl, RegVT, Arg);
      break;
    case CCValAssign::ZExt:
      Arg = DAG.getNode(ISD::ZERO_EXTEND, dl, RegVT, Arg);
      break;
    case CCValAssign::AExt:
      if (RegVT.is128BitVector()) {
        // Special case: passing MMX values in XMM registers.
        Arg = DAG.getNode(ISD::BITCAST, dl, MVT::i64, Arg);
        Arg = DAG.getNode(ISD::SCALAR_TO_VECTOR, dl, MVT::v2i64, Arg);
        Arg = getMOVL(DAG, dl, MVT::v2i64, DAG.getUNDEF(MVT::v2i64), Arg);
      } else
        Arg = DAG.getNode(ISD::ANY_EXTEND, dl, RegVT, Arg);
      break;
    case CCValAssign::BCvt:
      Arg = DAG.getNode(ISD::BITCAST, dl, RegVT, Arg);
      break;
    case CCValAssign::Indirect: {
      // Store the argument.
      SDValue SpillSlot = DAG.CreateStackTemporary(VA.getValVT());
      int FI = cast<FrameIndexSDNode>(SpillSlot)->getIndex();
      Chain = DAG.getStore(Chain, dl, Arg, SpillSlot,
                           MachinePointerInfo::getFixedStack(FI),
                           false, false, 0);
      Arg = SpillSlot;
      break;
    }
    }

    if (VA.isRegLoc()) {
      RegsToPass.push_back(std::make_pair(VA.getLocReg(), Arg));
      if (isVarArg && IsWin64) {
        // Win64 ABI requires argument XMM reg to be copied to the corresponding
        // shadow reg if callee is a varargs function.
        unsigned ShadowReg = 0;
        switch (VA.getLocReg()) {
        case X86::XMM0: ShadowReg = X86::RCX; break;
        case X86::XMM1: ShadowReg = X86::RDX; break;
        case X86::XMM2: ShadowReg = X86::R8; break;
        case X86::XMM3: ShadowReg = X86::R9; break;
        }
        if (ShadowReg)
          RegsToPass.push_back(std::make_pair(ShadowReg, Arg));
      }
    } else if (!IsSibcall && (!isTailCall || isByVal)) {
      assert(VA.isMemLoc());
      if (!StackPtr.getNode())
        StackPtr = DAG.getCopyFromReg(Chain, dl, RegInfo->getStackRegister(),
                                      getPointerTy());
      MemOpChains.push_back(LowerMemOpCallTo(Chain, StackPtr, Arg,
                                             dl, DAG, VA, Flags));
    }
  }

  if (!MemOpChains.empty())
    Chain = DAG.getNode(ISD::TokenFactor, dl, MVT::Other, MemOpChains);

  if (Subtarget->isPICStyleGOT()) {
    // ELF / PIC requires GOT in the EBX register before function calls via PLT
    // GOT pointer.
    if (!isTailCall) {
      RegsToPass.push_back(std::make_pair(unsigned(X86::EBX),
               DAG.getNode(X86ISD::GlobalBaseReg, SDLoc(), getPointerTy())));
    } else {
      // If we are tail calling and generating PIC/GOT style code load the
      // address of the callee into ECX. The value in ecx is used as target of
      // the tail jump. This is done to circumvent the ebx/callee-saved problem
      // for tail calls on PIC/GOT architectures. Normally we would just put the
      // address of GOT into ebx and then call target@PLT. But for tail calls
      // ebx would be restored (since ebx is callee saved) before jumping to the
      // target@PLT.

      // Note: The actual moving to ECX is done further down.
      GlobalAddressSDNode *G = dyn_cast<GlobalAddressSDNode>(Callee);
      if (G && !G->getGlobal()->hasHiddenVisibility() &&
          !G->getGlobal()->hasProtectedVisibility())
        Callee = LowerGlobalAddress(Callee, DAG);
      else if (isa<ExternalSymbolSDNode>(Callee))
        Callee = LowerExternalSymbol(Callee, DAG);
    }
  }

  if (Is64Bit && isVarArg && !IsWin64 && !IsMustTail) {
    // From AMD64 ABI document:
    // For calls that may call functions that use varargs or stdargs
    // (prototype-less calls or calls to functions containing ellipsis (...) in
    // the declaration) %al is used as hidden argument to specify the number
    // of SSE registers used. The contents of %al do not need to match exactly
    // the number of registers, but must be an ubound on the number of SSE
    // registers used and is in the range 0 - 8 inclusive.

    // Count the number of XMM registers allocated.
    static const MCPhysReg XMMArgRegs[] = {
      X86::XMM0, X86::XMM1, X86::XMM2, X86::XMM3,
      X86::XMM4, X86::XMM5, X86::XMM6, X86::XMM7
    };
    unsigned NumXMMRegs = CCInfo.getFirstUnallocated(XMMArgRegs, 8);
    assert((Subtarget->hasSSE1() || !NumXMMRegs)
           && "SSE registers cannot be used when SSE is disabled");

    RegsToPass.push_back(std::make_pair(unsigned(X86::AL),
                                        DAG.getConstant(NumXMMRegs, MVT::i8)));
  }

  if (Is64Bit && isVarArg && IsMustTail) {
    const auto &Forwards = X86Info->getForwardedMustTailRegParms();
    for (const auto &F : Forwards) {
      SDValue Val = DAG.getCopyFromReg(Chain, dl, F.VReg, F.VT);
      RegsToPass.push_back(std::make_pair(unsigned(F.PReg), Val));
    }
  }

  // For tail calls lower the arguments to the 'real' stack slots.  Sibcalls
  // don't need this because the eligibility check rejects calls that require
  // shuffling arguments passed in memory.
  if (!IsSibcall && isTailCall) {
    // Force all the incoming stack arguments to be loaded from the stack
    // before any new outgoing arguments are stored to the stack, because the
    // outgoing stack slots may alias the incoming argument stack slots, and
    // the alias isn't otherwise explicit. This is slightly more conservative
    // than necessary, because it means that each store effectively depends
    // on every argument instead of just those arguments it would clobber.
    SDValue ArgChain = DAG.getStackArgumentTokenFactor(Chain);

    SmallVector<SDValue, 8> MemOpChains2;
    SDValue FIN;
    int FI = 0;
    for (unsigned i = 0, e = ArgLocs.size(); i != e; ++i) {
      CCValAssign &VA = ArgLocs[i];
      if (VA.isRegLoc())
        continue;
      assert(VA.isMemLoc());
      SDValue Arg = OutVals[i];
      ISD::ArgFlagsTy Flags = Outs[i].Flags;
      // Skip inalloca arguments.  They don't require any work.
      if (Flags.isInAlloca())
        continue;
      // Create frame index.
      int32_t Offset = VA.getLocMemOffset()+FPDiff;
      uint32_t OpSize = (VA.getLocVT().getSizeInBits()+7)/8;
      FI = MF.getFrameInfo()->CreateFixedObject(OpSize, Offset, true);
      FIN = DAG.getFrameIndex(FI, getPointerTy());

      if (Flags.isByVal()) {
        // Copy relative to framepointer.
        SDValue Source = DAG.getIntPtrConstant(VA.getLocMemOffset());
        if (!StackPtr.getNode())
          StackPtr = DAG.getCopyFromReg(Chain, dl,
                                        RegInfo->getStackRegister(),
                                        getPointerTy());
        Source = DAG.getNode(ISD::ADD, dl, getPointerTy(), StackPtr, Source);

        MemOpChains2.push_back(CreateCopyOfByValArgument(Source, FIN,
                                                         ArgChain,
                                                         Flags, DAG, dl));
      } else {
        // Store relative to framepointer.
        MemOpChains2.push_back(
          DAG.getStore(ArgChain, dl, Arg, FIN,
                       MachinePointerInfo::getFixedStack(FI),
                       false, false, 0));
      }
    }

    if (!MemOpChains2.empty())
      Chain = DAG.getNode(ISD::TokenFactor, dl, MVT::Other, MemOpChains2);

    // Store the return address to the appropriate stack slot.
    Chain = EmitTailCallStoreRetAddr(DAG, MF, Chain, RetAddrFrIdx,
                                     getPointerTy(), RegInfo->getSlotSize(),
                                     FPDiff, dl);
  }

  // Build a sequence of copy-to-reg nodes chained together with token chain
  // and flag operands which copy the outgoing args into registers.
  SDValue InFlag;
  for (unsigned i = 0, e = RegsToPass.size(); i != e; ++i) {
    Chain = DAG.getCopyToReg(Chain, dl, RegsToPass[i].first,
                             RegsToPass[i].second, InFlag);
    InFlag = Chain.getValue(1);
  }

  if (DAG.getTarget().getCodeModel() == CodeModel::Large) {
    assert(Is64Bit && "Large code model is only legal in 64-bit mode.");
    // In the 64-bit large code model, we have to make all calls
    // through a register, since the call instruction's 32-bit
    // pc-relative offset may not be large enough to hold the whole
    // address.
  } else if (GlobalAddressSDNode *G = dyn_cast<GlobalAddressSDNode>(Callee)) {
    // If the callee is a GlobalAddress node (quite common, every direct call
    // is) turn it into a TargetGlobalAddress node so that legalize doesn't hack
    // it.

    // We should use extra load for direct calls to dllimported functions in
    // non-JIT mode.
    const GlobalValue *GV = G->getGlobal();
    if (!GV->hasDLLImportStorageClass()) {
      unsigned char OpFlags = 0;
      bool ExtraLoad = false;
      unsigned WrapperKind = ISD::DELETED_NODE;

      // On ELF targets, in both X86-64 and X86-32 mode, direct calls to
      // external symbols most go through the PLT in PIC mode.  If the symbol
      // has hidden or protected visibility, or if it is static or local, then
      // we don't need to use the PLT - we can directly call it.
      if (Subtarget->isTargetELF() &&
          DAG.getTarget().getRelocationModel() == Reloc::PIC_ &&
          GV->hasDefaultVisibility() && !GV->hasLocalLinkage()) {
        OpFlags = X86II::MO_PLT;
      } else if (Subtarget->isPICStyleStubAny() &&
                 (GV->isDeclaration() || GV->isWeakForLinker()) &&
                 (!Subtarget->getTargetTriple().isMacOSX() ||
                  Subtarget->getTargetTriple().isMacOSXVersionLT(10, 5))) {
        // PC-relative references to external symbols should go through $stub,
        // unless we're building with the leopard linker or later, which
        // automatically synthesizes these stubs.
        OpFlags = X86II::MO_DARWIN_STUB;
      } else if (Subtarget->isPICStyleRIPRel() &&
                 isa<Function>(GV) &&
                 cast<Function>(GV)->getAttributes().
                   hasAttribute(AttributeSet::FunctionIndex,
                                Attribute::NonLazyBind)) {
        // If the function is marked as non-lazy, generate an indirect call
        // which loads from the GOT directly. This avoids runtime overhead
        // at the cost of eager binding (and one extra byte of encoding).
        OpFlags = X86II::MO_GOTPCREL;
        WrapperKind = X86ISD::WrapperRIP;
        ExtraLoad = true;
      }

      Callee = DAG.getTargetGlobalAddress(GV, dl, getPointerTy(),
                                          G->getOffset(), OpFlags);

      // Add a wrapper if needed.
      if (WrapperKind != ISD::DELETED_NODE)
        Callee = DAG.getNode(X86ISD::WrapperRIP, dl, getPointerTy(), Callee);
      // Add extra indirection if needed.
      if (ExtraLoad)
        Callee = DAG.getLoad(getPointerTy(), dl, DAG.getEntryNode(), Callee,
                             MachinePointerInfo::getGOT(),
                             false, false, false, 0);
    }
  } else if (ExternalSymbolSDNode *S = dyn_cast<ExternalSymbolSDNode>(Callee)) {
    unsigned char OpFlags = 0;

    // On ELF targets, in either X86-64 or X86-32 mode, direct calls to
    // external symbols should go through the PLT.
    if (Subtarget->isTargetELF() &&
        DAG.getTarget().getRelocationModel() == Reloc::PIC_) {
      OpFlags = X86II::MO_PLT;
    } else if (Subtarget->isPICStyleStubAny() &&
               (!Subtarget->getTargetTriple().isMacOSX() ||
                Subtarget->getTargetTriple().isMacOSXVersionLT(10, 5))) {
      // PC-relative references to external symbols should go through $stub,
      // unless we're building with the leopard linker or later, which
      // automatically synthesizes these stubs.
      OpFlags = X86II::MO_DARWIN_STUB;
    }

    Callee = DAG.getTargetExternalSymbol(S->getSymbol(), getPointerTy(),
                                         OpFlags);
  }

  // Returns a chain & a flag for retval copy to use.
  SDVTList NodeTys = DAG.getVTList(MVT::Other, MVT::Glue);
  SmallVector<SDValue, 8> Ops;

  if (!IsSibcall && isTailCall) {
    Chain = DAG.getCALLSEQ_END(Chain,
                               DAG.getIntPtrConstant(NumBytesToPop, true),
                               DAG.getIntPtrConstant(0, true), InFlag, dl);
    InFlag = Chain.getValue(1);
  }

  Ops.push_back(Chain);
  Ops.push_back(Callee);

  if (isTailCall)
    Ops.push_back(DAG.getConstant(FPDiff, MVT::i32));

  // Add argument registers to the end of the list so that they are known live
  // into the call.
  for (unsigned i = 0, e = RegsToPass.size(); i != e; ++i)
    Ops.push_back(DAG.getRegister(RegsToPass[i].first,
                                  RegsToPass[i].second.getValueType()));

  // Add a register mask operand representing the call-preserved registers.
  const TargetRegisterInfo *TRI = DAG.getSubtarget().getRegisterInfo();
  const uint32_t *Mask = TRI->getCallPreservedMask(CallConv);
  assert(Mask && "Missing call preserved mask for calling convention");
  Ops.push_back(DAG.getRegisterMask(Mask));

  if (InFlag.getNode())
    Ops.push_back(InFlag);

  if (isTailCall) {
    // We used to do:
    //// If this is the first return lowered for this function, add the regs
    //// to the liveout set for the function.
    // This isn't right, although it's probably harmless on x86; liveouts
    // should be computed from returns not tail calls.  Consider a void
    // function making a tail call to a function returning int.
    return DAG.getNode(X86ISD::TC_RETURN, dl, NodeTys, Ops);
  }

  Chain = DAG.getNode(X86ISD::CALL, dl, NodeTys, Ops);
  InFlag = Chain.getValue(1);

  // Create the CALLSEQ_END node.
  unsigned NumBytesForCalleeToPop;
  if (X86::isCalleePop(CallConv, Is64Bit, isVarArg,
                       DAG.getTarget().Options.GuaranteedTailCallOpt))
    NumBytesForCalleeToPop = NumBytes;    // Callee pops everything
  else if (!Is64Bit && !IsTailCallConvention(CallConv) &&
           !Subtarget->getTargetTriple().isOSMSVCRT() &&
           SR == StackStructReturn)
    // If this is a call to a struct-return function, the callee
    // pops the hidden struct pointer, so we have to push it back.
    // This is common for Darwin/X86, Linux & Mingw32 targets.
    // For MSVC Win32 targets, the caller pops the hidden struct pointer.
    NumBytesForCalleeToPop = 4;
  else
    NumBytesForCalleeToPop = 0;  // Callee pops nothing.

  // Returns a flag for retval copy to use.
  if (!IsSibcall) {
    Chain = DAG.getCALLSEQ_END(Chain,
                               DAG.getIntPtrConstant(NumBytesToPop, true),
                               DAG.getIntPtrConstant(NumBytesForCalleeToPop,
                                                     true),
                               InFlag, dl);
    InFlag = Chain.getValue(1);
  }

  // Handle result values, copying them out of physregs into vregs that we
  // return.
  return LowerCallResult(Chain, InFlag, CallConv, isVarArg,
                         Ins, dl, DAG, InVals);
}

//===----------------------------------------------------------------------===//
//                Fast Calling Convention (tail call) implementation
//===----------------------------------------------------------------------===//

//  Like std call, callee cleans arguments, convention except that ECX is
//  reserved for storing the tail called function address. Only 2 registers are
//  free for argument passing (inreg). Tail call optimization is performed
//  provided:
//                * tailcallopt is enabled
//                * caller/callee are fastcc
//  On X86_64 architecture with GOT-style position independent code only local
//  (within module) calls are supported at the moment.
//  To keep the stack aligned according to platform abi the function
//  GetAlignedArgumentStackSize ensures that argument delta is always multiples
//  of stack alignment. (Dynamic linkers need this - darwin's dyld for example)
//  If a tail called function callee has more arguments than the caller the
//  caller needs to make sure that there is room to move the RETADDR to. This is
//  achieved by reserving an area the size of the argument delta right after the
//  original RETADDR, but before the saved framepointer or the spilled registers
//  e.g. caller(arg1, arg2) calls callee(arg1, arg2,arg3,arg4)
//  stack layout:
//    arg1
//    arg2
//    RETADDR
//    [ new RETADDR
//      move area ]
//    (possible EBP)
//    ESI
//    EDI
//    local1 ..

/// GetAlignedArgumentStackSize - Make the stack size align e.g 16n + 12 aligned
/// for a 16 byte align requirement.
unsigned
X86TargetLowering::GetAlignedArgumentStackSize(unsigned StackSize,
                                               SelectionDAG& DAG) const {
  MachineFunction &MF = DAG.getMachineFunction();
  const TargetMachine &TM = MF.getTarget();
  const X86RegisterInfo *RegInfo = static_cast<const X86RegisterInfo *>(
      TM.getSubtargetImpl()->getRegisterInfo());
  const TargetFrameLowering &TFI = *TM.getSubtargetImpl()->getFrameLowering();
  unsigned StackAlignment = TFI.getStackAlignment();
  uint64_t AlignMask = StackAlignment - 1;
  int64_t Offset = StackSize;
  unsigned SlotSize = RegInfo->getSlotSize();
  if ( (Offset & AlignMask) <= (StackAlignment - SlotSize) ) {
    // Number smaller than 12 so just add the difference.
    Offset += ((StackAlignment - SlotSize) - (Offset & AlignMask));
  } else {
    // Mask out lower bits, add stackalignment once plus the 12 bytes.
    Offset = ((~AlignMask) & Offset) + StackAlignment +
      (StackAlignment-SlotSize);
  }
  return Offset;
}

/// MatchingStackOffset - Return true if the given stack call argument is
/// already available in the same position (relatively) of the caller's
/// incoming argument stack.
static
bool MatchingStackOffset(SDValue Arg, unsigned Offset, ISD::ArgFlagsTy Flags,
                         MachineFrameInfo *MFI, const MachineRegisterInfo *MRI,
                         const X86InstrInfo *TII) {
  unsigned Bytes = Arg.getValueType().getSizeInBits() / 8;
  int FI = INT_MAX;
  if (Arg.getOpcode() == ISD::CopyFromReg) {
    unsigned VR = cast<RegisterSDNode>(Arg.getOperand(1))->getReg();
    if (!TargetRegisterInfo::isVirtualRegister(VR))
      return false;
    MachineInstr *Def = MRI->getVRegDef(VR);
    if (!Def)
      return false;
    if (!Flags.isByVal()) {
      if (!TII->isLoadFromStackSlot(Def, FI))
        return false;
    } else {
      unsigned Opcode = Def->getOpcode();
      if ((Opcode == X86::LEA32r || Opcode == X86::LEA64r) &&
          Def->getOperand(1).isFI()) {
        FI = Def->getOperand(1).getIndex();
        Bytes = Flags.getByValSize();
      } else
        return false;
    }
  } else if (LoadSDNode *Ld = dyn_cast<LoadSDNode>(Arg)) {
    if (Flags.isByVal())
      // ByVal argument is passed in as a pointer but it's now being
      // dereferenced. e.g.
      // define @foo(%struct.X* %A) {
      //   tail call @bar(%struct.X* byval %A)
      // }
      return false;
    SDValue Ptr = Ld->getBasePtr();
    FrameIndexSDNode *FINode = dyn_cast<FrameIndexSDNode>(Ptr);
    if (!FINode)
      return false;
    FI = FINode->getIndex();
  } else if (Arg.getOpcode() == ISD::FrameIndex && Flags.isByVal()) {
    FrameIndexSDNode *FINode = cast<FrameIndexSDNode>(Arg);
    FI = FINode->getIndex();
    Bytes = Flags.getByValSize();
  } else
    return false;

  assert(FI != INT_MAX);
  if (!MFI->isFixedObjectIndex(FI))
    return false;
  return Offset == MFI->getObjectOffset(FI) && Bytes == MFI->getObjectSize(FI);
}

/// IsEligibleForTailCallOptimization - Check whether the call is eligible
/// for tail call optimization. Targets which want to do tail call
/// optimization should implement this function.
bool
X86TargetLowering::IsEligibleForTailCallOptimization(SDValue Callee,
                                                     CallingConv::ID CalleeCC,
                                                     bool isVarArg,
                                                     bool isCalleeStructRet,
                                                     bool isCallerStructRet,
                                                     Type *RetTy,
                                    const SmallVectorImpl<ISD::OutputArg> &Outs,
                                    const SmallVectorImpl<SDValue> &OutVals,
                                    const SmallVectorImpl<ISD::InputArg> &Ins,
                                                     SelectionDAG &DAG) const {
  if (!IsTailCallConvention(CalleeCC) && !IsCCallConvention(CalleeCC))
    return false;

  // If -tailcallopt is specified, make fastcc functions tail-callable.
  const MachineFunction &MF = DAG.getMachineFunction();
  const Function *CallerF = MF.getFunction();

  // If the function return type is x86_fp80 and the callee return type is not,
  // then the FP_EXTEND of the call result is not a nop. It's not safe to
  // perform a tailcall optimization here.
  if (CallerF->getReturnType()->isX86_FP80Ty() && !RetTy->isX86_FP80Ty())
    return false;

  CallingConv::ID CallerCC = CallerF->getCallingConv();
  bool CCMatch = CallerCC == CalleeCC;
  bool IsCalleeWin64 = Subtarget->isCallingConvWin64(CalleeCC);
  bool IsCallerWin64 = Subtarget->isCallingConvWin64(CallerCC);

  if (DAG.getTarget().Options.GuaranteedTailCallOpt) {
    if (IsTailCallConvention(CalleeCC) && CCMatch)
      return true;
    return false;
  }

  // Look for obvious safe cases to perform tail call optimization that do not
  // require ABI changes. This is what gcc calls sibcall.

  // Can't do sibcall if stack needs to be dynamically re-aligned. PEI needs to
  // emit a special epilogue.
  const X86RegisterInfo *RegInfo = static_cast<const X86RegisterInfo *>(
      DAG.getSubtarget().getRegisterInfo());
  if (RegInfo->needsStackRealignment(MF))
    return false;

  // Also avoid sibcall optimization if either caller or callee uses struct
  // return semantics.
  if (isCalleeStructRet || isCallerStructRet)
    return false;

  // An stdcall/thiscall caller is expected to clean up its arguments; the
  // callee isn't going to do that.
  // FIXME: this is more restrictive than needed. We could produce a tailcall
  // when the stack adjustment matches. For example, with a thiscall that takes
  // only one argument.
  if (!CCMatch && (CallerCC == CallingConv::X86_StdCall ||
                   CallerCC == CallingConv::X86_ThisCall))
    return false;

  // Do not sibcall optimize vararg calls unless all arguments are passed via
  // registers.
  if (isVarArg && !Outs.empty()) {

    // Optimizing for varargs on Win64 is unlikely to be safe without
    // additional testing.
    if (IsCalleeWin64 || IsCallerWin64)
      return false;

    SmallVector<CCValAssign, 16> ArgLocs;
    CCState CCInfo(CalleeCC, isVarArg, DAG.getMachineFunction(), ArgLocs,
                   *DAG.getContext());

    CCInfo.AnalyzeCallOperands(Outs, CC_X86);
    for (unsigned i = 0, e = ArgLocs.size(); i != e; ++i)
      if (!ArgLocs[i].isRegLoc())
        return false;
  }

  // If the call result is in ST0 / ST1, it needs to be popped off the x87
  // stack.  Therefore, if it's not used by the call it is not safe to optimize
  // this into a sibcall.
  bool Unused = false;
  for (unsigned i = 0, e = Ins.size(); i != e; ++i) {
    if (!Ins[i].Used) {
      Unused = true;
      break;
    }
  }
  if (Unused) {
    SmallVector<CCValAssign, 16> RVLocs;
    CCState CCInfo(CalleeCC, false, DAG.getMachineFunction(), RVLocs,
                   *DAG.getContext());
    CCInfo.AnalyzeCallResult(Ins, RetCC_X86);
    for (unsigned i = 0, e = RVLocs.size(); i != e; ++i) {
      CCValAssign &VA = RVLocs[i];
      if (VA.getLocReg() == X86::FP0 || VA.getLocReg() == X86::FP1)
        return false;
    }
  }

  // If the calling conventions do not match, then we'd better make sure the
  // results are returned in the same way as what the caller expects.
  if (!CCMatch) {
    SmallVector<CCValAssign, 16> RVLocs1;
    CCState CCInfo1(CalleeCC, false, DAG.getMachineFunction(), RVLocs1,
                    *DAG.getContext());
    CCInfo1.AnalyzeCallResult(Ins, RetCC_X86);

    SmallVector<CCValAssign, 16> RVLocs2;
    CCState CCInfo2(CallerCC, false, DAG.getMachineFunction(), RVLocs2,
                    *DAG.getContext());
    CCInfo2.AnalyzeCallResult(Ins, RetCC_X86);

    if (RVLocs1.size() != RVLocs2.size())
      return false;
    for (unsigned i = 0, e = RVLocs1.size(); i != e; ++i) {
      if (RVLocs1[i].isRegLoc() != RVLocs2[i].isRegLoc())
        return false;
      if (RVLocs1[i].getLocInfo() != RVLocs2[i].getLocInfo())
        return false;
      if (RVLocs1[i].isRegLoc()) {
        if (RVLocs1[i].getLocReg() != RVLocs2[i].getLocReg())
          return false;
      } else {
        if (RVLocs1[i].getLocMemOffset() != RVLocs2[i].getLocMemOffset())
          return false;
      }
    }
  }

  // If the callee takes no arguments then go on to check the results of the
  // call.
  if (!Outs.empty()) {
    // Check if stack adjustment is needed. For now, do not do this if any
    // argument is passed on the stack.
    SmallVector<CCValAssign, 16> ArgLocs;
    CCState CCInfo(CalleeCC, isVarArg, DAG.getMachineFunction(), ArgLocs,
                   *DAG.getContext());

    // Allocate shadow area for Win64
    if (IsCalleeWin64)
      CCInfo.AllocateStack(32, 8);

    CCInfo.AnalyzeCallOperands(Outs, CC_X86);
    if (CCInfo.getNextStackOffset()) {
      MachineFunction &MF = DAG.getMachineFunction();
      if (MF.getInfo<X86MachineFunctionInfo>()->getBytesToPopOnReturn())
        return false;

      // Check if the arguments are already laid out in the right way as
      // the caller's fixed stack objects.
      MachineFrameInfo *MFI = MF.getFrameInfo();
      const MachineRegisterInfo *MRI = &MF.getRegInfo();
      const X86InstrInfo *TII =
          static_cast<const X86InstrInfo *>(DAG.getSubtarget().getInstrInfo());
      for (unsigned i = 0, e = ArgLocs.size(); i != e; ++i) {
        CCValAssign &VA = ArgLocs[i];
        SDValue Arg = OutVals[i];
        ISD::ArgFlagsTy Flags = Outs[i].Flags;
        if (VA.getLocInfo() == CCValAssign::Indirect)
          return false;
        if (!VA.isRegLoc()) {
          if (!MatchingStackOffset(Arg, VA.getLocMemOffset(), Flags,
                                   MFI, MRI, TII))
            return false;
        }
      }
    }

    // If the tailcall address may be in a register, then make sure it's
    // possible to register allocate for it. In 32-bit, the call address can
    // only target EAX, EDX, or ECX since the tail call must be scheduled after
    // callee-saved registers are restored. These happen to be the same
    // registers used to pass 'inreg' arguments so watch out for those.
    if (!Subtarget->is64Bit() &&
        ((!isa<GlobalAddressSDNode>(Callee) &&
          !isa<ExternalSymbolSDNode>(Callee)) ||
         DAG.getTarget().getRelocationModel() == Reloc::PIC_)) {
      unsigned NumInRegs = 0;
      // In PIC we need an extra register to formulate the address computation
      // for the callee.
      unsigned MaxInRegs =
	(DAG.getTarget().getRelocationModel() == Reloc::PIC_) ? 2 : 3;

      for (unsigned i = 0, e = ArgLocs.size(); i != e; ++i) {
        CCValAssign &VA = ArgLocs[i];
        if (!VA.isRegLoc())
          continue;
        unsigned Reg = VA.getLocReg();
        switch (Reg) {
        default: break;
        case X86::EAX: case X86::EDX: case X86::ECX:
          if (++NumInRegs == MaxInRegs)
            return false;
          break;
        }
      }
    }
  }

  return true;
}

FastISel *
X86TargetLowering::createFastISel(FunctionLoweringInfo &funcInfo,
                                  const TargetLibraryInfo *libInfo) const {
  return X86::createFastISel(funcInfo, libInfo);
}

//===----------------------------------------------------------------------===//
//                           Other Lowering Hooks
//===----------------------------------------------------------------------===//

static bool MayFoldLoad(SDValue Op) {
  return Op.hasOneUse() && ISD::isNormalLoad(Op.getNode());
}

static bool MayFoldIntoStore(SDValue Op) {
  return Op.hasOneUse() && ISD::isNormalStore(*Op.getNode()->use_begin());
}

static bool isTargetShuffle(unsigned Opcode) {
  switch(Opcode) {
  default: return false;
  case X86ISD::PSHUFB:
  case X86ISD::PSHUFD:
  case X86ISD::PSHUFHW:
  case X86ISD::PSHUFLW:
  case X86ISD::SHUFP:
  case X86ISD::PALIGNR:
  case X86ISD::MOVLHPS:
  case X86ISD::MOVLHPD:
  case X86ISD::MOVHLPS:
  case X86ISD::MOVLPS:
  case X86ISD::MOVLPD:
  case X86ISD::MOVSHDUP:
  case X86ISD::MOVSLDUP:
  case X86ISD::MOVDDUP:
  case X86ISD::MOVSS:
  case X86ISD::MOVSD:
  case X86ISD::UNPCKL:
  case X86ISD::UNPCKH:
  case X86ISD::VPERMILP:
  case X86ISD::VPERM2X128:
  case X86ISD::VPERMI:
    return true;
  }
}

static SDValue getTargetShuffleNode(unsigned Opc, SDLoc dl, EVT VT,
                                    SDValue V1, SelectionDAG &DAG) {
  switch(Opc) {
  default: llvm_unreachable("Unknown x86 shuffle node");
  case X86ISD::MOVSHDUP:
  case X86ISD::MOVSLDUP:
  case X86ISD::MOVDDUP:
    return DAG.getNode(Opc, dl, VT, V1);
  }
}

static SDValue getTargetShuffleNode(unsigned Opc, SDLoc dl, EVT VT,
                                    SDValue V1, unsigned TargetMask,
                                    SelectionDAG &DAG) {
  switch(Opc) {
  default: llvm_unreachable("Unknown x86 shuffle node");
  case X86ISD::PSHUFD:
  case X86ISD::PSHUFHW:
  case X86ISD::PSHUFLW:
  case X86ISD::VPERMILP:
  case X86ISD::VPERMI:
    return DAG.getNode(Opc, dl, VT, V1, DAG.getConstant(TargetMask, MVT::i8));
  }
}

static SDValue getTargetShuffleNode(unsigned Opc, SDLoc dl, EVT VT,
                                    SDValue V1, SDValue V2, unsigned TargetMask,
                                    SelectionDAG &DAG) {
  switch(Opc) {
  default: llvm_unreachable("Unknown x86 shuffle node");
  case X86ISD::PALIGNR:
  case X86ISD::VALIGN:
  case X86ISD::SHUFP:
  case X86ISD::VPERM2X128:
    return DAG.getNode(Opc, dl, VT, V1, V2,
                       DAG.getConstant(TargetMask, MVT::i8));
  }
}

static SDValue getTargetShuffleNode(unsigned Opc, SDLoc dl, EVT VT,
                                    SDValue V1, SDValue V2, SelectionDAG &DAG) {
  switch(Opc) {
  default: llvm_unreachable("Unknown x86 shuffle node");
  case X86ISD::MOVLHPS:
  case X86ISD::MOVLHPD:
  case X86ISD::MOVHLPS:
  case X86ISD::MOVLPS:
  case X86ISD::MOVLPD:
  case X86ISD::MOVSS:
  case X86ISD::MOVSD:
  case X86ISD::UNPCKL:
  case X86ISD::UNPCKH:
    return DAG.getNode(Opc, dl, VT, V1, V2);
  }
}

SDValue X86TargetLowering::getReturnAddressFrameIndex(SelectionDAG &DAG) const {
  MachineFunction &MF = DAG.getMachineFunction();
  const X86RegisterInfo *RegInfo = static_cast<const X86RegisterInfo *>(
      DAG.getSubtarget().getRegisterInfo());
  X86MachineFunctionInfo *FuncInfo = MF.getInfo<X86MachineFunctionInfo>();
  int ReturnAddrIndex = FuncInfo->getRAIndex();

  if (ReturnAddrIndex == 0) {
    // Set up a frame object for the return address.
    unsigned SlotSize = RegInfo->getSlotSize();
    ReturnAddrIndex = MF.getFrameInfo()->CreateFixedObject(SlotSize,
                                                           -(int64_t)SlotSize,
                                                           false);
    FuncInfo->setRAIndex(ReturnAddrIndex);
  }

  return DAG.getFrameIndex(ReturnAddrIndex, getPointerTy());
}

bool X86::isOffsetSuitableForCodeModel(int64_t Offset, CodeModel::Model M,
                                       bool hasSymbolicDisplacement) {
  // Offset should fit into 32 bit immediate field.
  if (!isInt<32>(Offset))
    return false;

  // If we don't have a symbolic displacement - we don't have any extra
  // restrictions.
  if (!hasSymbolicDisplacement)
    return true;

  // FIXME: Some tweaks might be needed for medium code model.
  if (M != CodeModel::Small && M != CodeModel::Kernel)
    return false;

  // For small code model we assume that latest object is 16MB before end of 31
  // bits boundary. We may also accept pretty large negative constants knowing
  // that all objects are in the positive half of address space.
  if (M == CodeModel::Small && Offset < 16*1024*1024)
    return true;

  // For kernel code model we know that all object resist in the negative half
  // of 32bits address space. We may not accept negative offsets, since they may
  // be just off and we may accept pretty large positive ones.
  if (M == CodeModel::Kernel && Offset > 0)
    return true;

  return false;
}

/// isCalleePop - Determines whether the callee is required to pop its
/// own arguments. Callee pop is necessary to support tail calls.
bool X86::isCalleePop(CallingConv::ID CallingConv,
                      bool is64Bit, bool IsVarArg, bool TailCallOpt) {
  switch (CallingConv) {
  default:
    return false;
  case CallingConv::X86_StdCall:
  case CallingConv::X86_FastCall:
  case CallingConv::X86_ThisCall:
    return !is64Bit;
  case CallingConv::Fast:
  case CallingConv::GHC:
  case CallingConv::HiPE:
    if (IsVarArg)
      return false;
    return TailCallOpt;
  }
}

/// \brief Return true if the condition is an unsigned comparison operation.
static bool isX86CCUnsigned(unsigned X86CC) {
  switch (X86CC) {
  default: llvm_unreachable("Invalid integer condition!");
  case X86::COND_E:     return true;
  case X86::COND_G:     return false;
  case X86::COND_GE:    return false;
  case X86::COND_L:     return false;
  case X86::COND_LE:    return false;
  case X86::COND_NE:    return true;
  case X86::COND_B:     return true;
  case X86::COND_A:     return true;
  case X86::COND_BE:    return true;
  case X86::COND_AE:    return true;
  }
  llvm_unreachable("covered switch fell through?!");
}

/// TranslateX86CC - do a one to one translation of a ISD::CondCode to the X86
/// specific condition code, returning the condition code and the LHS/RHS of the
/// comparison to make.
static unsigned TranslateX86CC(ISD::CondCode SetCCOpcode, bool isFP,
                               SDValue &LHS, SDValue &RHS, SelectionDAG &DAG) {
  if (!isFP) {
    if (ConstantSDNode *RHSC = dyn_cast<ConstantSDNode>(RHS)) {
      if (SetCCOpcode == ISD::SETGT && RHSC->isAllOnesValue()) {
        // X > -1   -> X == 0, jump !sign.
        RHS = DAG.getConstant(0, RHS.getValueType());
        return X86::COND_NS;
      }
      if (SetCCOpcode == ISD::SETLT && RHSC->isNullValue()) {
        // X < 0   -> X == 0, jump on sign.
        return X86::COND_S;
      }
      if (SetCCOpcode == ISD::SETLT && RHSC->getZExtValue() == 1) {
        // X < 1   -> X <= 0
        RHS = DAG.getConstant(0, RHS.getValueType());
        return X86::COND_LE;
      }
    }

    switch (SetCCOpcode) {
    default: llvm_unreachable("Invalid integer condition!");
    case ISD::SETEQ:  return X86::COND_E;
    case ISD::SETGT:  return X86::COND_G;
    case ISD::SETGE:  return X86::COND_GE;
    case ISD::SETLT:  return X86::COND_L;
    case ISD::SETLE:  return X86::COND_LE;
    case ISD::SETNE:  return X86::COND_NE;
    case ISD::SETULT: return X86::COND_B;
    case ISD::SETUGT: return X86::COND_A;
    case ISD::SETULE: return X86::COND_BE;
    case ISD::SETUGE: return X86::COND_AE;
    }
  }

  // First determine if it is required or is profitable to flip the operands.

  // If LHS is a foldable load, but RHS is not, flip the condition.
  if (ISD::isNON_EXTLoad(LHS.getNode()) &&
      !ISD::isNON_EXTLoad(RHS.getNode())) {
    SetCCOpcode = getSetCCSwappedOperands(SetCCOpcode);
    std::swap(LHS, RHS);
  }

  switch (SetCCOpcode) {
  default: break;
  case ISD::SETOLT:
  case ISD::SETOLE:
  case ISD::SETUGT:
  case ISD::SETUGE:
    std::swap(LHS, RHS);
    break;
  }

  // On a floating point condition, the flags are set as follows:
  // ZF  PF  CF   op
  //  0 | 0 | 0 | X > Y
  //  0 | 0 | 1 | X < Y
  //  1 | 0 | 0 | X == Y
  //  1 | 1 | 1 | unordered
  switch (SetCCOpcode) {
  default: llvm_unreachable("Condcode should be pre-legalized away");
  case ISD::SETUEQ:
  case ISD::SETEQ:   return X86::COND_E;
  case ISD::SETOLT:              // flipped
  case ISD::SETOGT:
  case ISD::SETGT:   return X86::COND_A;
  case ISD::SETOLE:              // flipped
  case ISD::SETOGE:
  case ISD::SETGE:   return X86::COND_AE;
  case ISD::SETUGT:              // flipped
  case ISD::SETULT:
  case ISD::SETLT:   return X86::COND_B;
  case ISD::SETUGE:              // flipped
  case ISD::SETULE:
  case ISD::SETLE:   return X86::COND_BE;
  case ISD::SETONE:
  case ISD::SETNE:   return X86::COND_NE;
  case ISD::SETUO:   return X86::COND_P;
  case ISD::SETO:    return X86::COND_NP;
  case ISD::SETOEQ:
  case ISD::SETUNE:  return X86::COND_INVALID;
  }
}

/// hasFPCMov - is there a floating point cmov for the specific X86 condition
/// code. Current x86 isa includes the following FP cmov instructions:
/// fcmovb, fcomvbe, fcomve, fcmovu, fcmovae, fcmova, fcmovne, fcmovnu.
static bool hasFPCMov(unsigned X86CC) {
  switch (X86CC) {
  default:
    return false;
  case X86::COND_B:
  case X86::COND_BE:
  case X86::COND_E:
  case X86::COND_P:
  case X86::COND_A:
  case X86::COND_AE:
  case X86::COND_NE:
  case X86::COND_NP:
    return true;
  }
}

/// isFPImmLegal - Returns true if the target can instruction select the
/// specified FP immediate natively. If false, the legalizer will
/// materialize the FP immediate as a load from a constant pool.
bool X86TargetLowering::isFPImmLegal(const APFloat &Imm, EVT VT) const {
  for (unsigned i = 0, e = LegalFPImmediates.size(); i != e; ++i) {
    if (Imm.bitwiseIsEqual(LegalFPImmediates[i]))
      return true;
  }
  return false;
}

/// \brief Returns true if it is beneficial to convert a load of a constant
/// to just the constant itself.
bool X86TargetLowering::shouldConvertConstantLoadToIntImm(const APInt &Imm,
                                                          Type *Ty) const {
  assert(Ty->isIntegerTy());

  unsigned BitSize = Ty->getPrimitiveSizeInBits();
  if (BitSize == 0 || BitSize > 64)
    return false;
  return true;
}

/// isUndefOrInRange - Return true if Val is undef or if its value falls within
/// the specified range (L, H].
static bool isUndefOrInRange(int Val, int Low, int Hi) {
  return (Val < 0) || (Val >= Low && Val < Hi);
}

/// isUndefOrEqual - Val is either less than zero (undef) or equal to the
/// specified value.
static bool isUndefOrEqual(int Val, int CmpVal) {
  return (Val < 0 || Val == CmpVal);
}

/// isSequentialOrUndefInRange - Return true if every element in Mask, beginning
/// from position Pos and ending in Pos+Size, falls within the specified
/// sequential range (L, L+Pos]. or is undef.
static bool isSequentialOrUndefInRange(ArrayRef<int> Mask,
                                       unsigned Pos, unsigned Size, int Low) {
  for (unsigned i = Pos, e = Pos+Size; i != e; ++i, ++Low)
    if (!isUndefOrEqual(Mask[i], Low))
      return false;
  return true;
}

/// isPSHUFDMask - Return true if the node specifies a shuffle of elements that
/// is suitable for input to PSHUFD or PSHUFW.  That is, it doesn't reference
/// the second operand.
static bool isPSHUFDMask(ArrayRef<int> Mask, MVT VT) {
  if (VT == MVT::v4f32 || VT == MVT::v4i32 )
    return (Mask[0] < 4 && Mask[1] < 4 && Mask[2] < 4 && Mask[3] < 4);
  if (VT == MVT::v2f64 || VT == MVT::v2i64)
    return (Mask[0] < 2 && Mask[1] < 2);
  return false;
}

/// isPSHUFHWMask - Return true if the node specifies a shuffle of elements that
/// is suitable for input to PSHUFHW.
static bool isPSHUFHWMask(ArrayRef<int> Mask, MVT VT, bool HasInt256) {
  if (VT != MVT::v8i16 && (!HasInt256 || VT != MVT::v16i16))
    return false;

  // Lower quadword copied in order or undef.
  if (!isSequentialOrUndefInRange(Mask, 0, 4, 0))
    return false;

  // Upper quadword shuffled.
  for (unsigned i = 4; i != 8; ++i)
    if (!isUndefOrInRange(Mask[i], 4, 8))
      return false;

  if (VT == MVT::v16i16) {
    // Lower quadword copied in order or undef.
    if (!isSequentialOrUndefInRange(Mask, 8, 4, 8))
      return false;

    // Upper quadword shuffled.
    for (unsigned i = 12; i != 16; ++i)
      if (!isUndefOrInRange(Mask[i], 12, 16))
        return false;
  }

  return true;
}

/// isPSHUFLWMask - Return true if the node specifies a shuffle of elements that
/// is suitable for input to PSHUFLW.
static bool isPSHUFLWMask(ArrayRef<int> Mask, MVT VT, bool HasInt256) {
  if (VT != MVT::v8i16 && (!HasInt256 || VT != MVT::v16i16))
    return false;

  // Upper quadword copied in order.
  if (!isSequentialOrUndefInRange(Mask, 4, 4, 4))
    return false;

  // Lower quadword shuffled.
  for (unsigned i = 0; i != 4; ++i)
    if (!isUndefOrInRange(Mask[i], 0, 4))
      return false;

  if (VT == MVT::v16i16) {
    // Upper quadword copied in order.
    if (!isSequentialOrUndefInRange(Mask, 12, 4, 12))
      return false;

    // Lower quadword shuffled.
    for (unsigned i = 8; i != 12; ++i)
      if (!isUndefOrInRange(Mask[i], 8, 12))
        return false;
  }

  return true;
}

/// \brief Return true if the mask specifies a shuffle of elements that is
/// suitable for input to intralane (palignr) or interlane (valign) vector
/// right-shift.
static bool isAlignrMask(ArrayRef<int> Mask, MVT VT, bool InterLane) {
  unsigned NumElts = VT.getVectorNumElements();
  unsigned NumLanes = InterLane ? 1: VT.getSizeInBits()/128;
  unsigned NumLaneElts = NumElts/NumLanes;

  // Do not handle 64-bit element shuffles with palignr.
  if (NumLaneElts == 2)
    return false;

  for (unsigned l = 0; l != NumElts; l+=NumLaneElts) {
    unsigned i;
    for (i = 0; i != NumLaneElts; ++i) {
      if (Mask[i+l] >= 0)
        break;
    }

    // Lane is all undef, go to next lane
    if (i == NumLaneElts)
      continue;

    int Start = Mask[i+l];

    // Make sure its in this lane in one of the sources
    if (!isUndefOrInRange(Start, l, l+NumLaneElts) &&
        !isUndefOrInRange(Start, l+NumElts, l+NumElts+NumLaneElts))
      return false;

    // If not lane 0, then we must match lane 0
    if (l != 0 && Mask[i] >= 0 && !isUndefOrEqual(Start, Mask[i]+l))
      return false;

    // Correct second source to be contiguous with first source
    if (Start >= (int)NumElts)
      Start -= NumElts - NumLaneElts;

    // Make sure we're shifting in the right direction.
    if (Start <= (int)(i+l))
      return false;

    Start -= i;

    // Check the rest of the elements to see if they are consecutive.
    for (++i; i != NumLaneElts; ++i) {
      int Idx = Mask[i+l];

      // Make sure its in this lane
      if (!isUndefOrInRange(Idx, l, l+NumLaneElts) &&
          !isUndefOrInRange(Idx, l+NumElts, l+NumElts+NumLaneElts))
        return false;

      // If not lane 0, then we must match lane 0
      if (l != 0 && Mask[i] >= 0 && !isUndefOrEqual(Idx, Mask[i]+l))
        return false;

      if (Idx >= (int)NumElts)
        Idx -= NumElts - NumLaneElts;

      if (!isUndefOrEqual(Idx, Start+i))
        return false;

    }
  }

  return true;
}

/// \brief Return true if the node specifies a shuffle of elements that is
/// suitable for input to PALIGNR.
static bool isPALIGNRMask(ArrayRef<int> Mask, MVT VT,
                          const X86Subtarget *Subtarget) {
  if ((VT.is128BitVector() && !Subtarget->hasSSSE3()) ||
      (VT.is256BitVector() && !Subtarget->hasInt256()) ||
      VT.is512BitVector())
    // FIXME: Add AVX512BW.
    return false;

  return isAlignrMask(Mask, VT, false);
}

/// \brief Return true if the node specifies a shuffle of elements that is
/// suitable for input to VALIGN.
static bool isVALIGNMask(ArrayRef<int> Mask, MVT VT,
                          const X86Subtarget *Subtarget) {
  // FIXME: Add AVX512VL.
  if (!VT.is512BitVector() || !Subtarget->hasAVX512())
    return false;
  return isAlignrMask(Mask, VT, true);
}

/// CommuteVectorShuffleMask - Change values in a shuffle permute mask assuming
/// the two vector operands have swapped position.
static void CommuteVectorShuffleMask(SmallVectorImpl<int> &Mask,
                                     unsigned NumElems) {
  for (unsigned i = 0; i != NumElems; ++i) {
    int idx = Mask[i];
    if (idx < 0)
      continue;
    else if (idx < (int)NumElems)
      Mask[i] = idx + NumElems;
    else
      Mask[i] = idx - NumElems;
  }
}

/// isSHUFPMask - Return true if the specified VECTOR_SHUFFLE operand
/// specifies a shuffle of elements that is suitable for input to 128/256-bit
/// SHUFPS and SHUFPD. If Commuted is true, then it checks for sources to be
/// reverse of what x86 shuffles want.
static bool isSHUFPMask(ArrayRef<int> Mask, MVT VT, bool Commuted = false) {

  unsigned NumElems = VT.getVectorNumElements();
  unsigned NumLanes = VT.getSizeInBits()/128;
  unsigned NumLaneElems = NumElems/NumLanes;

  if (NumLaneElems != 2 && NumLaneElems != 4)
    return false;

  unsigned EltSize = VT.getVectorElementType().getSizeInBits();
  bool symetricMaskRequired =
    (VT.getSizeInBits() >= 256) && (EltSize == 32);

  // VSHUFPSY divides the resulting vector into 4 chunks.
  // The sources are also splitted into 4 chunks, and each destination
  // chunk must come from a different source chunk.
  //
  //  SRC1 =>   X7    X6    X5    X4    X3    X2    X1    X0
  //  SRC2 =>   Y7    Y6    Y5    Y4    Y3    Y2    Y1    Y9
  //
  //  DST  =>  Y7..Y4,   Y7..Y4,   X7..X4,   X7..X4,
  //           Y3..Y0,   Y3..Y0,   X3..X0,   X3..X0
  //
  // VSHUFPDY divides the resulting vector into 4 chunks.
  // The sources are also splitted into 4 chunks, and each destination
  // chunk must come from a different source chunk.
  //
  //  SRC1 =>      X3       X2       X1       X0
  //  SRC2 =>      Y3       Y2       Y1       Y0
  //
  //  DST  =>  Y3..Y2,  X3..X2,  Y1..Y0,  X1..X0
  //
  SmallVector<int, 4> MaskVal(NumLaneElems, -1);
  unsigned HalfLaneElems = NumLaneElems/2;
  for (unsigned l = 0; l != NumElems; l += NumLaneElems) {
    for (unsigned i = 0; i != NumLaneElems; ++i) {
      int Idx = Mask[i+l];
      unsigned RngStart = l + ((Commuted == (i<HalfLaneElems)) ? NumElems : 0);
      if (!isUndefOrInRange(Idx, RngStart, RngStart+NumLaneElems))
        return false;
      // For VSHUFPSY, the mask of the second half must be the same as the
      // first but with the appropriate offsets. This works in the same way as
      // VPERMILPS works with masks.
      if (!symetricMaskRequired || Idx < 0)
        continue;
      if (MaskVal[i] < 0) {
        MaskVal[i] = Idx - l;
        continue;
      }
      if ((signed)(Idx - l) != MaskVal[i])
        return false;
    }
  }

  return true;
}

/// isMOVHLPSMask - Return true if the specified VECTOR_SHUFFLE operand
/// specifies a shuffle of elements that is suitable for input to MOVHLPS.
static bool isMOVHLPSMask(ArrayRef<int> Mask, MVT VT) {
  if (!VT.is128BitVector())
    return false;

  unsigned NumElems = VT.getVectorNumElements();

  if (NumElems != 4)
    return false;

  // Expect bit0 == 6, bit1 == 7, bit2 == 2, bit3 == 3
  return isUndefOrEqual(Mask[0], 6) &&
         isUndefOrEqual(Mask[1], 7) &&
         isUndefOrEqual(Mask[2], 2) &&
         isUndefOrEqual(Mask[3], 3);
}

/// isMOVHLPS_v_undef_Mask - Special case of isMOVHLPSMask for canonical form
/// of vector_shuffle v, v, <2, 3, 2, 3>, i.e. vector_shuffle v, undef,
/// <2, 3, 2, 3>
static bool isMOVHLPS_v_undef_Mask(ArrayRef<int> Mask, MVT VT) {
  if (!VT.is128BitVector())
    return false;

  unsigned NumElems = VT.getVectorNumElements();

  if (NumElems != 4)
    return false;

  return isUndefOrEqual(Mask[0], 2) &&
         isUndefOrEqual(Mask[1], 3) &&
         isUndefOrEqual(Mask[2], 2) &&
         isUndefOrEqual(Mask[3], 3);
}

/// isMOVLPMask - Return true if the specified VECTOR_SHUFFLE operand
/// specifies a shuffle of elements that is suitable for input to MOVLP{S|D}.
static bool isMOVLPMask(ArrayRef<int> Mask, MVT VT) {
  if (!VT.is128BitVector())
    return false;

  unsigned NumElems = VT.getVectorNumElements();

  if (NumElems != 2 && NumElems != 4)
    return false;

  for (unsigned i = 0, e = NumElems/2; i != e; ++i)
    if (!isUndefOrEqual(Mask[i], i + NumElems))
      return false;

  for (unsigned i = NumElems/2, e = NumElems; i != e; ++i)
    if (!isUndefOrEqual(Mask[i], i))
      return false;

  return true;
}

/// isMOVLHPSMask - Return true if the specified VECTOR_SHUFFLE operand
/// specifies a shuffle of elements that is suitable for input to MOVLHPS.
static bool isMOVLHPSMask(ArrayRef<int> Mask, MVT VT) {
  if (!VT.is128BitVector())
    return false;

  unsigned NumElems = VT.getVectorNumElements();

  if (NumElems != 2 && NumElems != 4)
    return false;

  for (unsigned i = 0, e = NumElems/2; i != e; ++i)
    if (!isUndefOrEqual(Mask[i], i))
      return false;

  for (unsigned i = 0, e = NumElems/2; i != e; ++i)
    if (!isUndefOrEqual(Mask[i + e], i + NumElems))
      return false;

  return true;
}

/// isINSERTPSMask - Return true if the specified VECTOR_SHUFFLE operand
/// specifies a shuffle of elements that is suitable for input to INSERTPS.
/// i. e: If all but one element come from the same vector.
static bool isINSERTPSMask(ArrayRef<int> Mask, MVT VT) {
  // TODO: Deal with AVX's VINSERTPS
  if (!VT.is128BitVector() || (VT != MVT::v4f32 && VT != MVT::v4i32))
    return false;

  unsigned CorrectPosV1 = 0;
  unsigned CorrectPosV2 = 0;
  for (int i = 0, e = (int)VT.getVectorNumElements(); i != e; ++i) {
    if (Mask[i] == -1) {
      ++CorrectPosV1;
      ++CorrectPosV2;
      continue;
    }

    if (Mask[i] == i)
      ++CorrectPosV1;
    else if (Mask[i] == i + 4)
      ++CorrectPosV2;
  }

  if (CorrectPosV1 == 3 || CorrectPosV2 == 3)
    // We have 3 elements (undefs count as elements from any vector) from one
    // vector, and one from another.
    return true;

  return false;
}

//
// Some special combinations that can be optimized.
//
static
SDValue Compact8x32ShuffleNode(ShuffleVectorSDNode *SVOp,
                               SelectionDAG &DAG) {
  MVT VT = SVOp->getSimpleValueType(0);
  SDLoc dl(SVOp);

  if (VT != MVT::v8i32 && VT != MVT::v8f32)
    return SDValue();

  ArrayRef<int> Mask = SVOp->getMask();

  // These are the special masks that may be optimized.
  static const int MaskToOptimizeEven[] = {0, 8, 2, 10, 4, 12, 6, 14};
  static const int MaskToOptimizeOdd[]  = {1, 9, 3, 11, 5, 13, 7, 15};
  bool MatchEvenMask = true;
  bool MatchOddMask  = true;
  for (int i=0; i<8; ++i) {
    if (!isUndefOrEqual(Mask[i], MaskToOptimizeEven[i]))
      MatchEvenMask = false;
    if (!isUndefOrEqual(Mask[i], MaskToOptimizeOdd[i]))
      MatchOddMask = false;
  }

  if (!MatchEvenMask && !MatchOddMask)
    return SDValue();

  SDValue UndefNode = DAG.getNode(ISD::UNDEF, dl, VT);

  SDValue Op0 = SVOp->getOperand(0);
  SDValue Op1 = SVOp->getOperand(1);

  if (MatchEvenMask) {
    // Shift the second operand right to 32 bits.
    static const int ShiftRightMask[] = {-1, 0, -1, 2, -1, 4, -1, 6 };
    Op1 = DAG.getVectorShuffle(VT, dl, Op1, UndefNode, ShiftRightMask);
  } else {
    // Shift the first operand left to 32 bits.
    static const int ShiftLeftMask[] = {1, -1, 3, -1, 5, -1, 7, -1 };
    Op0 = DAG.getVectorShuffle(VT, dl, Op0, UndefNode, ShiftLeftMask);
  }
  static const int BlendMask[] = {0, 9, 2, 11, 4, 13, 6, 15};
  return DAG.getVectorShuffle(VT, dl, Op0, Op1, BlendMask);
}

/// isUNPCKLMask - Return true if the specified VECTOR_SHUFFLE operand
/// specifies a shuffle of elements that is suitable for input to UNPCKL.
static bool isUNPCKLMask(ArrayRef<int> Mask, MVT VT,
                         bool HasInt256, bool V2IsSplat = false) {

  assert(VT.getSizeInBits() >= 128 &&
         "Unsupported vector type for unpckl");

  // AVX defines UNPCK* to operate independently on 128-bit lanes.
  unsigned NumLanes;
  unsigned NumOf256BitLanes;
  unsigned NumElts = VT.getVectorNumElements();
  if (VT.is256BitVector()) {
    if (NumElts != 4 && NumElts != 8 &&
        (!HasInt256 || (NumElts != 16 && NumElts != 32)))
    return false;
    NumLanes = 2;
    NumOf256BitLanes = 1;
  } else if (VT.is512BitVector()) {
    assert(VT.getScalarType().getSizeInBits() >= 32 &&
           "Unsupported vector type for unpckh");
    NumLanes = 2;
    NumOf256BitLanes = 2;
  } else {
    NumLanes = 1;
    NumOf256BitLanes = 1;
  }

  unsigned NumEltsInStride = NumElts/NumOf256BitLanes;
  unsigned NumLaneElts = NumEltsInStride/NumLanes;

  for (unsigned l256 = 0; l256 < NumOf256BitLanes; l256 += 1) {
    for (unsigned l = 0; l != NumEltsInStride; l += NumLaneElts) {
      for (unsigned i = 0, j = l; i != NumLaneElts; i += 2, ++j) {
        int BitI  = Mask[l256*NumEltsInStride+l+i];
        int BitI1 = Mask[l256*NumEltsInStride+l+i+1];
        if (!isUndefOrEqual(BitI, j+l256*NumElts))
          return false;
        if (V2IsSplat && !isUndefOrEqual(BitI1, NumElts))
          return false;
        if (!isUndefOrEqual(BitI1, j+l256*NumElts+NumEltsInStride))
          return false;
      }
    }
  }
  return true;
}

/// isUNPCKHMask - Return true if the specified VECTOR_SHUFFLE operand
/// specifies a shuffle of elements that is suitable for input to UNPCKH.
static bool isUNPCKHMask(ArrayRef<int> Mask, MVT VT,
                         bool HasInt256, bool V2IsSplat = false) {
  assert(VT.getSizeInBits() >= 128 &&
         "Unsupported vector type for unpckh");

  // AVX defines UNPCK* to operate independently on 128-bit lanes.
  unsigned NumLanes;
  unsigned NumOf256BitLanes;
  unsigned NumElts = VT.getVectorNumElements();
  if (VT.is256BitVector()) {
    if (NumElts != 4 && NumElts != 8 &&
        (!HasInt256 || (NumElts != 16 && NumElts != 32)))
    return false;
    NumLanes = 2;
    NumOf256BitLanes = 1;
  } else if (VT.is512BitVector()) {
    assert(VT.getScalarType().getSizeInBits() >= 32 &&
           "Unsupported vector type for unpckh");
    NumLanes = 2;
    NumOf256BitLanes = 2;
  } else {
    NumLanes = 1;
    NumOf256BitLanes = 1;
  }

  unsigned NumEltsInStride = NumElts/NumOf256BitLanes;
  unsigned NumLaneElts = NumEltsInStride/NumLanes;

  for (unsigned l256 = 0; l256 < NumOf256BitLanes; l256 += 1) {
    for (unsigned l = 0; l != NumEltsInStride; l += NumLaneElts) {
      for (unsigned i = 0, j = l+NumLaneElts/2; i != NumLaneElts; i += 2, ++j) {
        int BitI  = Mask[l256*NumEltsInStride+l+i];
        int BitI1 = Mask[l256*NumEltsInStride+l+i+1];
        if (!isUndefOrEqual(BitI, j+l256*NumElts))
          return false;
        if (V2IsSplat && !isUndefOrEqual(BitI1, NumElts))
          return false;
        if (!isUndefOrEqual(BitI1, j+l256*NumElts+NumEltsInStride))
          return false;
      }
    }
  }
  return true;
}

/// isUNPCKL_v_undef_Mask - Special case of isUNPCKLMask for canonical form
/// of vector_shuffle v, v, <0, 4, 1, 5>, i.e. vector_shuffle v, undef,
/// <0, 0, 1, 1>
static bool isUNPCKL_v_undef_Mask(ArrayRef<int> Mask, MVT VT, bool HasInt256) {
  unsigned NumElts = VT.getVectorNumElements();
  bool Is256BitVec = VT.is256BitVector();

  if (VT.is512BitVector())
    return false;
  assert((VT.is128BitVector() || VT.is256BitVector()) &&
         "Unsupported vector type for unpckh");

  if (Is256BitVec && NumElts != 4 && NumElts != 8 &&
      (!HasInt256 || (NumElts != 16 && NumElts != 32)))
    return false;

  // For 256-bit i64/f64, use MOVDDUPY instead, so reject the matching pattern
  // FIXME: Need a better way to get rid of this, there's no latency difference
  // between UNPCKLPD and MOVDDUP, the later should always be checked first and
  // the former later. We should also remove the "_undef" special mask.
  if (NumElts == 4 && Is256BitVec)
    return false;

  // Handle 128 and 256-bit vector lengths. AVX defines UNPCK* to operate
  // independently on 128-bit lanes.
  unsigned NumLanes = VT.getSizeInBits()/128;
  unsigned NumLaneElts = NumElts/NumLanes;

  for (unsigned l = 0; l != NumElts; l += NumLaneElts) {
    for (unsigned i = 0, j = l; i != NumLaneElts; i += 2, ++j) {
      int BitI  = Mask[l+i];
      int BitI1 = Mask[l+i+1];

      if (!isUndefOrEqual(BitI, j))
        return false;
      if (!isUndefOrEqual(BitI1, j))
        return false;
    }
  }

  return true;
}

/// isUNPCKH_v_undef_Mask - Special case of isUNPCKHMask for canonical form
/// of vector_shuffle v, v, <2, 6, 3, 7>, i.e. vector_shuffle v, undef,
/// <2, 2, 3, 3>
static bool isUNPCKH_v_undef_Mask(ArrayRef<int> Mask, MVT VT, bool HasInt256) {
  unsigned NumElts = VT.getVectorNumElements();

  if (VT.is512BitVector())
    return false;

  assert((VT.is128BitVector() || VT.is256BitVector()) &&
         "Unsupported vector type for unpckh");

  if (VT.is256BitVector() && NumElts != 4 && NumElts != 8 &&
      (!HasInt256 || (NumElts != 16 && NumElts != 32)))
    return false;

  // Handle 128 and 256-bit vector lengths. AVX defines UNPCK* to operate
  // independently on 128-bit lanes.
  unsigned NumLanes = VT.getSizeInBits()/128;
  unsigned NumLaneElts = NumElts/NumLanes;

  for (unsigned l = 0; l != NumElts; l += NumLaneElts) {
    for (unsigned i = 0, j = l+NumLaneElts/2; i != NumLaneElts; i += 2, ++j) {
      int BitI  = Mask[l+i];
      int BitI1 = Mask[l+i+1];
      if (!isUndefOrEqual(BitI, j))
        return false;
      if (!isUndefOrEqual(BitI1, j))
        return false;
    }
  }
  return true;
}

// Match for INSERTI64x4 INSERTF64x4 instructions (src0[0], src1[0]) or
// (src1[0], src0[1]), manipulation with 256-bit sub-vectors
static bool isINSERT64x4Mask(ArrayRef<int> Mask, MVT VT, unsigned int *Imm) {
  if (!VT.is512BitVector())
    return false;

  unsigned NumElts = VT.getVectorNumElements();
  unsigned HalfSize = NumElts/2;
  if (isSequentialOrUndefInRange(Mask, 0, HalfSize, 0)) {
    if (isSequentialOrUndefInRange(Mask, HalfSize, HalfSize, NumElts)) {
      *Imm = 1;
      return true;
    }
  }
  if (isSequentialOrUndefInRange(Mask, 0, HalfSize, NumElts)) {
    if (isSequentialOrUndefInRange(Mask, HalfSize, HalfSize, HalfSize)) {
      *Imm = 0;
      return true;
    }
  }
  return false;
}

/// isMOVLMask - Return true if the specified VECTOR_SHUFFLE operand
/// specifies a shuffle of elements that is suitable for input to MOVSS,
/// MOVSD, and MOVD, i.e. setting the lowest element.
static bool isMOVLMask(ArrayRef<int> Mask, EVT VT) {
  if (VT.getVectorElementType().getSizeInBits() < 32)
    return false;
  if (!VT.is128BitVector())
    return false;

  unsigned NumElts = VT.getVectorNumElements();

  if (!isUndefOrEqual(Mask[0], NumElts))
    return false;

  for (unsigned i = 1; i != NumElts; ++i)
    if (!isUndefOrEqual(Mask[i], i))
      return false;

  return true;
}

/// isVPERM2X128Mask - Match 256-bit shuffles where the elements are considered
/// as permutations between 128-bit chunks or halves. As an example: this
/// shuffle bellow:
///   vector_shuffle <4, 5, 6, 7, 12, 13, 14, 15>
/// The first half comes from the second half of V1 and the second half from the
/// the second half of V2.
static bool isVPERM2X128Mask(ArrayRef<int> Mask, MVT VT, bool HasFp256) {
  if (!HasFp256 || !VT.is256BitVector())
    return false;

  // The shuffle result is divided into half A and half B. In total the two
  // sources have 4 halves, namely: C, D, E, F. The final values of A and
  // B must come from C, D, E or F.
  unsigned HalfSize = VT.getVectorNumElements()/2;
  bool MatchA = false, MatchB = false;

  // Check if A comes from one of C, D, E, F.
  for (unsigned Half = 0; Half != 4; ++Half) {
    if (isSequentialOrUndefInRange(Mask, 0, HalfSize, Half*HalfSize)) {
      MatchA = true;
      break;
    }
  }

  // Check if B comes from one of C, D, E, F.
  for (unsigned Half = 0; Half != 4; ++Half) {
    if (isSequentialOrUndefInRange(Mask, HalfSize, HalfSize, Half*HalfSize)) {
      MatchB = true;
      break;
    }
  }

  return MatchA && MatchB;
}

/// getShuffleVPERM2X128Immediate - Return the appropriate immediate to shuffle
/// the specified VECTOR_MASK mask with VPERM2F128/VPERM2I128 instructions.
static unsigned getShuffleVPERM2X128Immediate(ShuffleVectorSDNode *SVOp) {
  MVT VT = SVOp->getSimpleValueType(0);

  unsigned HalfSize = VT.getVectorNumElements()/2;

  unsigned FstHalf = 0, SndHalf = 0;
  for (unsigned i = 0; i < HalfSize; ++i) {
    if (SVOp->getMaskElt(i) > 0) {
      FstHalf = SVOp->getMaskElt(i)/HalfSize;
      break;
    }
  }
  for (unsigned i = HalfSize; i < HalfSize*2; ++i) {
    if (SVOp->getMaskElt(i) > 0) {
      SndHalf = SVOp->getMaskElt(i)/HalfSize;
      break;
    }
  }

  return (FstHalf | (SndHalf << 4));
}

// Symetric in-lane mask. Each lane has 4 elements (for imm8)
static bool isPermImmMask(ArrayRef<int> Mask, MVT VT, unsigned& Imm8) {
  unsigned EltSize = VT.getVectorElementType().getSizeInBits();
  if (EltSize < 32)
    return false;

  unsigned NumElts = VT.getVectorNumElements();
  Imm8 = 0;
  if (VT.is128BitVector() || (VT.is256BitVector() && EltSize == 64)) {
    for (unsigned i = 0; i != NumElts; ++i) {
      if (Mask[i] < 0)
        continue;
      Imm8 |= Mask[i] << (i*2);
    }
    return true;
  }

  unsigned LaneSize = 4;
  SmallVector<int, 4> MaskVal(LaneSize, -1);

  for (unsigned l = 0; l != NumElts; l += LaneSize) {
    for (unsigned i = 0; i != LaneSize; ++i) {
      if (!isUndefOrInRange(Mask[i+l], l, l+LaneSize))
        return false;
      if (Mask[i+l] < 0)
        continue;
      if (MaskVal[i] < 0) {
        MaskVal[i] = Mask[i+l] - l;
        Imm8 |= MaskVal[i] << (i*2);
        continue;
      }
      if (Mask[i+l] != (signed)(MaskVal[i]+l))
        return false;
    }
  }
  return true;
}

/// isVPERMILPMask - Return true if the specified VECTOR_SHUFFLE operand
/// specifies a shuffle of elements that is suitable for input to VPERMILPD*.
/// Note that VPERMIL mask matching is different depending whether theunderlying
/// type is 32 or 64. In the VPERMILPS the high half of the mask should point
/// to the same elements of the low, but to the higher half of the source.
/// In VPERMILPD the two lanes could be shuffled independently of each other
/// with the same restriction that lanes can't be crossed. Also handles PSHUFDY.
static bool isVPERMILPMask(ArrayRef<int> Mask, MVT VT) {
  unsigned EltSize = VT.getVectorElementType().getSizeInBits();
  if (VT.getSizeInBits() < 256 || EltSize < 32)
    return false;
  bool symetricMaskRequired = (EltSize == 32);
  unsigned NumElts = VT.getVectorNumElements();

  unsigned NumLanes = VT.getSizeInBits()/128;
  unsigned LaneSize = NumElts/NumLanes;
  // 2 or 4 elements in one lane

  SmallVector<int, 4> ExpectedMaskVal(LaneSize, -1);
  for (unsigned l = 0; l != NumElts; l += LaneSize) {
    for (unsigned i = 0; i != LaneSize; ++i) {
      if (!isUndefOrInRange(Mask[i+l], l, l+LaneSize))
        return false;
      if (symetricMaskRequired) {
        if (ExpectedMaskVal[i] < 0 && Mask[i+l] >= 0) {
          ExpectedMaskVal[i] = Mask[i+l] - l;
          continue;
        }
        if (!isUndefOrEqual(Mask[i+l], ExpectedMaskVal[i]+l))
          return false;
      }
    }
  }
  return true;
}

/// isCommutedMOVLMask - Returns true if the shuffle mask is except the reverse
/// of what x86 movss want. X86 movs requires the lowest  element to be lowest
/// element of vector 2 and the other elements to come from vector 1 in order.
static bool isCommutedMOVLMask(ArrayRef<int> Mask, MVT VT,
                               bool V2IsSplat = false, bool V2IsUndef = false) {
  if (!VT.is128BitVector())
    return false;

  unsigned NumOps = VT.getVectorNumElements();
  if (NumOps != 2 && NumOps != 4 && NumOps != 8 && NumOps != 16)
    return false;

  if (!isUndefOrEqual(Mask[0], 0))
    return false;

  for (unsigned i = 1; i != NumOps; ++i)
    if (!(isUndefOrEqual(Mask[i], i+NumOps) ||
          (V2IsUndef && isUndefOrInRange(Mask[i], NumOps, NumOps*2)) ||
          (V2IsSplat && isUndefOrEqual(Mask[i], NumOps))))
      return false;

  return true;
}

/// isMOVSHDUPMask - Return true if the specified VECTOR_SHUFFLE operand
/// specifies a shuffle of elements that is suitable for input to MOVSHDUP.
/// Masks to match: <1, 1, 3, 3> or <1, 1, 3, 3, 5, 5, 7, 7>
static bool isMOVSHDUPMask(ArrayRef<int> Mask, MVT VT,
                           const X86Subtarget *Subtarget) {
  if (!Subtarget->hasSSE3())
    return false;

  unsigned NumElems = VT.getVectorNumElements();

  if ((VT.is128BitVector() && NumElems != 4) ||
      (VT.is256BitVector() && NumElems != 8) ||
      (VT.is512BitVector() && NumElems != 16))
    return false;

  // "i+1" is the value the indexed mask element must have
  for (unsigned i = 0; i != NumElems; i += 2)
    if (!isUndefOrEqual(Mask[i], i+1) ||
        !isUndefOrEqual(Mask[i+1], i+1))
      return false;

  return true;
}

/// isMOVSLDUPMask - Return true if the specified VECTOR_SHUFFLE operand
/// specifies a shuffle of elements that is suitable for input to MOVSLDUP.
/// Masks to match: <0, 0, 2, 2> or <0, 0, 2, 2, 4, 4, 6, 6>
static bool isMOVSLDUPMask(ArrayRef<int> Mask, MVT VT,
                           const X86Subtarget *Subtarget) {
  if (!Subtarget->hasSSE3())
    return false;

  unsigned NumElems = VT.getVectorNumElements();

  if ((VT.is128BitVector() && NumElems != 4) ||
      (VT.is256BitVector() && NumElems != 8) ||
      (VT.is512BitVector() && NumElems != 16))
    return false;

  // "i" is the value the indexed mask element must have
  for (unsigned i = 0; i != NumElems; i += 2)
    if (!isUndefOrEqual(Mask[i], i) ||
        !isUndefOrEqual(Mask[i+1], i))
      return false;

  return true;
}

/// isMOVDDUPYMask - Return true if the specified VECTOR_SHUFFLE operand
/// specifies a shuffle of elements that is suitable for input to 256-bit
/// version of MOVDDUP.
static bool isMOVDDUPYMask(ArrayRef<int> Mask, MVT VT, bool HasFp256) {
  if (!HasFp256 || !VT.is256BitVector())
    return false;

  unsigned NumElts = VT.getVectorNumElements();
  if (NumElts != 4)
    return false;

  for (unsigned i = 0; i != NumElts/2; ++i)
    if (!isUndefOrEqual(Mask[i], 0))
      return false;
  for (unsigned i = NumElts/2; i != NumElts; ++i)
    if (!isUndefOrEqual(Mask[i], NumElts/2))
      return false;
  return true;
}

/// isMOVDDUPMask - Return true if the specified VECTOR_SHUFFLE operand
/// specifies a shuffle of elements that is suitable for input to 128-bit
/// version of MOVDDUP.
static bool isMOVDDUPMask(ArrayRef<int> Mask, MVT VT) {
  if (!VT.is128BitVector())
    return false;

  unsigned e = VT.getVectorNumElements() / 2;
  for (unsigned i = 0; i != e; ++i)
    if (!isUndefOrEqual(Mask[i], i))
      return false;
  for (unsigned i = 0; i != e; ++i)
    if (!isUndefOrEqual(Mask[e+i], i))
      return false;
  return true;
}

/// isVEXTRACTIndex - Return true if the specified
/// EXTRACT_SUBVECTOR operand specifies a vector extract that is
/// suitable for instruction that extract 128 or 256 bit vectors
static bool isVEXTRACTIndex(SDNode *N, unsigned vecWidth) {
  assert((vecWidth == 128 || vecWidth == 256) && "Unexpected vector width");
  if (!isa<ConstantSDNode>(N->getOperand(1).getNode()))
    return false;

  // The index should be aligned on a vecWidth-bit boundary.
  uint64_t Index =
    cast<ConstantSDNode>(N->getOperand(1).getNode())->getZExtValue();

  MVT VT = N->getSimpleValueType(0);
  unsigned ElSize = VT.getVectorElementType().getSizeInBits();
  bool Result = (Index * ElSize) % vecWidth == 0;

  return Result;
}

/// isVINSERTIndex - Return true if the specified INSERT_SUBVECTOR
/// operand specifies a subvector insert that is suitable for input to
/// insertion of 128 or 256-bit subvectors
static bool isVINSERTIndex(SDNode *N, unsigned vecWidth) {
  assert((vecWidth == 128 || vecWidth == 256) && "Unexpected vector width");
  if (!isa<ConstantSDNode>(N->getOperand(2).getNode()))
    return false;
  // The index should be aligned on a vecWidth-bit boundary.
  uint64_t Index =
    cast<ConstantSDNode>(N->getOperand(2).getNode())->getZExtValue();

  MVT VT = N->getSimpleValueType(0);
  unsigned ElSize = VT.getVectorElementType().getSizeInBits();
  bool Result = (Index * ElSize) % vecWidth == 0;

  return Result;
}

bool X86::isVINSERT128Index(SDNode *N) {
  return isVINSERTIndex(N, 128);
}

bool X86::isVINSERT256Index(SDNode *N) {
  return isVINSERTIndex(N, 256);
}

bool X86::isVEXTRACT128Index(SDNode *N) {
  return isVEXTRACTIndex(N, 128);
}

bool X86::isVEXTRACT256Index(SDNode *N) {
  return isVEXTRACTIndex(N, 256);
}

/// getShuffleSHUFImmediate - Return the appropriate immediate to shuffle
/// the specified VECTOR_SHUFFLE mask with PSHUF* and SHUFP* instructions.
/// Handles 128-bit and 256-bit.
static unsigned getShuffleSHUFImmediate(ShuffleVectorSDNode *N) {
  MVT VT = N->getSimpleValueType(0);

  assert((VT.getSizeInBits() >= 128) &&
         "Unsupported vector type for PSHUF/SHUFP");

  // Handle 128 and 256-bit vector lengths. AVX defines PSHUF/SHUFP to operate
  // independently on 128-bit lanes.
  unsigned NumElts = VT.getVectorNumElements();
  unsigned NumLanes = VT.getSizeInBits()/128;
  unsigned NumLaneElts = NumElts/NumLanes;

  assert((NumLaneElts == 2 || NumLaneElts == 4 || NumLaneElts == 8) &&
         "Only supports 2, 4 or 8 elements per lane");

  unsigned Shift = (NumLaneElts >= 4) ? 1 : 0;
  unsigned Mask = 0;
  for (unsigned i = 0; i != NumElts; ++i) {
    int Elt = N->getMaskElt(i);
    if (Elt < 0) continue;
    Elt &= NumLaneElts - 1;
    unsigned ShAmt = (i << Shift) % 8;
    Mask |= Elt << ShAmt;
  }

  return Mask;
}

/// getShufflePSHUFHWImmediate - Return the appropriate immediate to shuffle
/// the specified VECTOR_SHUFFLE mask with the PSHUFHW instruction.
static unsigned getShufflePSHUFHWImmediate(ShuffleVectorSDNode *N) {
  MVT VT = N->getSimpleValueType(0);

  assert((VT == MVT::v8i16 || VT == MVT::v16i16) &&
         "Unsupported vector type for PSHUFHW");

  unsigned NumElts = VT.getVectorNumElements();

  unsigned Mask = 0;
  for (unsigned l = 0; l != NumElts; l += 8) {
    // 8 nodes per lane, but we only care about the last 4.
    for (unsigned i = 0; i < 4; ++i) {
      int Elt = N->getMaskElt(l+i+4);
      if (Elt < 0) continue;
      Elt &= 0x3; // only 2-bits.
      Mask |= Elt << (i * 2);
    }
  }

  return Mask;
}

/// getShufflePSHUFLWImmediate - Return the appropriate immediate to shuffle
/// the specified VECTOR_SHUFFLE mask with the PSHUFLW instruction.
static unsigned getShufflePSHUFLWImmediate(ShuffleVectorSDNode *N) {
  MVT VT = N->getSimpleValueType(0);

  assert((VT == MVT::v8i16 || VT == MVT::v16i16) &&
         "Unsupported vector type for PSHUFHW");

  unsigned NumElts = VT.getVectorNumElements();

  unsigned Mask = 0;
  for (unsigned l = 0; l != NumElts; l += 8) {
    // 8 nodes per lane, but we only care about the first 4.
    for (unsigned i = 0; i < 4; ++i) {
      int Elt = N->getMaskElt(l+i);
      if (Elt < 0) continue;
      Elt &= 0x3; // only 2-bits
      Mask |= Elt << (i * 2);
    }
  }

  return Mask;
}

/// \brief Return the appropriate immediate to shuffle the specified
/// VECTOR_SHUFFLE mask with the PALIGNR (if InterLane is false) or with
/// VALIGN (if Interlane is true) instructions.
static unsigned getShuffleAlignrImmediate(ShuffleVectorSDNode *SVOp,
                                           bool InterLane) {
  MVT VT = SVOp->getSimpleValueType(0);
  unsigned EltSize = InterLane ? 1 :
    VT.getVectorElementType().getSizeInBits() >> 3;

  unsigned NumElts = VT.getVectorNumElements();
  unsigned NumLanes = VT.is512BitVector() ? 1 : VT.getSizeInBits()/128;
  unsigned NumLaneElts = NumElts/NumLanes;

  int Val = 0;
  unsigned i;
  for (i = 0; i != NumElts; ++i) {
    Val = SVOp->getMaskElt(i);
    if (Val >= 0)
      break;
  }
  if (Val >= (int)NumElts)
    Val -= NumElts - NumLaneElts;

  assert(Val - i > 0 && "PALIGNR imm should be positive");
  return (Val - i) * EltSize;
}

/// \brief Return the appropriate immediate to shuffle the specified
/// VECTOR_SHUFFLE mask with the PALIGNR instruction.
static unsigned getShufflePALIGNRImmediate(ShuffleVectorSDNode *SVOp) {
  return getShuffleAlignrImmediate(SVOp, false);
}

/// \brief Return the appropriate immediate to shuffle the specified
/// VECTOR_SHUFFLE mask with the VALIGN instruction.
static unsigned getShuffleVALIGNImmediate(ShuffleVectorSDNode *SVOp) {
  return getShuffleAlignrImmediate(SVOp, true);
}


static unsigned getExtractVEXTRACTImmediate(SDNode *N, unsigned vecWidth) {
  assert((vecWidth == 128 || vecWidth == 256) && "Unsupported vector width");
  if (!isa<ConstantSDNode>(N->getOperand(1).getNode()))
    llvm_unreachable("Illegal extract subvector for VEXTRACT");

  uint64_t Index =
    cast<ConstantSDNode>(N->getOperand(1).getNode())->getZExtValue();

  MVT VecVT = N->getOperand(0).getSimpleValueType();
  MVT ElVT = VecVT.getVectorElementType();

  unsigned NumElemsPerChunk = vecWidth / ElVT.getSizeInBits();
  return Index / NumElemsPerChunk;
}

static unsigned getInsertVINSERTImmediate(SDNode *N, unsigned vecWidth) {
  assert((vecWidth == 128 || vecWidth == 256) && "Unsupported vector width");
  if (!isa<ConstantSDNode>(N->getOperand(2).getNode()))
    llvm_unreachable("Illegal insert subvector for VINSERT");

  uint64_t Index =
    cast<ConstantSDNode>(N->getOperand(2).getNode())->getZExtValue();

  MVT VecVT = N->getSimpleValueType(0);
  MVT ElVT = VecVT.getVectorElementType();

  unsigned NumElemsPerChunk = vecWidth / ElVT.getSizeInBits();
  return Index / NumElemsPerChunk;
}

/// getExtractVEXTRACT128Immediate - Return the appropriate immediate
/// to extract the specified EXTRACT_SUBVECTOR index with VEXTRACTF128
/// and VINSERTI128 instructions.
unsigned X86::getExtractVEXTRACT128Immediate(SDNode *N) {
  return getExtractVEXTRACTImmediate(N, 128);
}

/// getExtractVEXTRACT256Immediate - Return the appropriate immediate
/// to extract the specified EXTRACT_SUBVECTOR index with VEXTRACTF64x4
/// and VINSERTI64x4 instructions.
unsigned X86::getExtractVEXTRACT256Immediate(SDNode *N) {
  return getExtractVEXTRACTImmediate(N, 256);
}

/// getInsertVINSERT128Immediate - Return the appropriate immediate
/// to insert at the specified INSERT_SUBVECTOR index with VINSERTF128
/// and VINSERTI128 instructions.
unsigned X86::getInsertVINSERT128Immediate(SDNode *N) {
  return getInsertVINSERTImmediate(N, 128);
}

/// getInsertVINSERT256Immediate - Return the appropriate immediate
/// to insert at the specified INSERT_SUBVECTOR index with VINSERTF46x4
/// and VINSERTI64x4 instructions.
unsigned X86::getInsertVINSERT256Immediate(SDNode *N) {
  return getInsertVINSERTImmediate(N, 256);
}

/// isZero - Returns true if Elt is a constant integer zero
static bool isZero(SDValue V) {
  ConstantSDNode *C = dyn_cast<ConstantSDNode>(V);
  return C && C->isNullValue();
}

/// isZeroNode - Returns true if Elt is a constant zero or a floating point
/// constant +0.0.
bool X86::isZeroNode(SDValue Elt) {
  if (isZero(Elt))
    return true;
  if (ConstantFPSDNode *CFP = dyn_cast<ConstantFPSDNode>(Elt))
    return CFP->getValueAPF().isPosZero();
  return false;
}

/// ShouldXformToMOVHLPS - Return true if the node should be transformed to
/// match movhlps. The lower half elements should come from upper half of
/// V1 (and in order), and the upper half elements should come from the upper
/// half of V2 (and in order).
static bool ShouldXformToMOVHLPS(ArrayRef<int> Mask, MVT VT) {
  if (!VT.is128BitVector())
    return false;
  if (VT.getVectorNumElements() != 4)
    return false;
  for (unsigned i = 0, e = 2; i != e; ++i)
    if (!isUndefOrEqual(Mask[i], i+2))
      return false;
  for (unsigned i = 2; i != 4; ++i)
    if (!isUndefOrEqual(Mask[i], i+4))
      return false;
  return true;
}

/// isScalarLoadToVector - Returns true if the node is a scalar load that
/// is promoted to a vector. It also returns the LoadSDNode by reference if
/// required.
static bool isScalarLoadToVector(SDNode *N, LoadSDNode **LD = nullptr) {
  if (N->getOpcode() != ISD::SCALAR_TO_VECTOR)
    return false;
  N = N->getOperand(0).getNode();
  if (!ISD::isNON_EXTLoad(N))
    return false;
  if (LD)
    *LD = cast<LoadSDNode>(N);
  return true;
}

// Test whether the given value is a vector value which will be legalized
// into a load.
static bool WillBeConstantPoolLoad(SDNode *N) {
  if (N->getOpcode() != ISD::BUILD_VECTOR)
    return false;

  // Check for any non-constant elements.
  for (unsigned i = 0, e = N->getNumOperands(); i != e; ++i)
    switch (N->getOperand(i).getNode()->getOpcode()) {
    case ISD::UNDEF:
    case ISD::ConstantFP:
    case ISD::Constant:
      break;
    default:
      return false;
    }

  // Vectors of all-zeros and all-ones are materialized with special
  // instructions rather than being loaded.
  return !ISD::isBuildVectorAllZeros(N) &&
         !ISD::isBuildVectorAllOnes(N);
}

/// ShouldXformToMOVLP{S|D} - Return true if the node should be transformed to
/// match movlp{s|d}. The lower half elements should come from lower half of
/// V1 (and in order), and the upper half elements should come from the upper
/// half of V2 (and in order). And since V1 will become the source of the
/// MOVLP, it must be either a vector load or a scalar load to vector.
static bool ShouldXformToMOVLP(SDNode *V1, SDNode *V2,
                               ArrayRef<int> Mask, MVT VT) {
  if (!VT.is128BitVector())
    return false;

  if (!ISD::isNON_EXTLoad(V1) && !isScalarLoadToVector(V1))
    return false;
  // Is V2 is a vector load, don't do this transformation. We will try to use
  // load folding shufps op.
  if (ISD::isNON_EXTLoad(V2) || WillBeConstantPoolLoad(V2))
    return false;

  unsigned NumElems = VT.getVectorNumElements();

  if (NumElems != 2 && NumElems != 4)
    return false;
  for (unsigned i = 0, e = NumElems/2; i != e; ++i)
    if (!isUndefOrEqual(Mask[i], i))
      return false;
  for (unsigned i = NumElems/2, e = NumElems; i != e; ++i)
    if (!isUndefOrEqual(Mask[i], i+NumElems))
      return false;
  return true;
}

/// isZeroShuffle - Returns true if N is a VECTOR_SHUFFLE that can be resolved
/// to an zero vector.
/// FIXME: move to dag combiner / method on ShuffleVectorSDNode
static bool isZeroShuffle(ShuffleVectorSDNode *N) {
  SDValue V1 = N->getOperand(0);
  SDValue V2 = N->getOperand(1);
  unsigned NumElems = N->getValueType(0).getVectorNumElements();
  for (unsigned i = 0; i != NumElems; ++i) {
    int Idx = N->getMaskElt(i);
    if (Idx >= (int)NumElems) {
      unsigned Opc = V2.getOpcode();
      if (Opc == ISD::UNDEF || ISD::isBuildVectorAllZeros(V2.getNode()))
        continue;
      if (Opc != ISD::BUILD_VECTOR ||
          !X86::isZeroNode(V2.getOperand(Idx-NumElems)))
        return false;
    } else if (Idx >= 0) {
      unsigned Opc = V1.getOpcode();
      if (Opc == ISD::UNDEF || ISD::isBuildVectorAllZeros(V1.getNode()))
        continue;
      if (Opc != ISD::BUILD_VECTOR ||
          !X86::isZeroNode(V1.getOperand(Idx)))
        return false;
    }
  }
  return true;
}

/// getZeroVector - Returns a vector of specified type with all zero elements.
///
static SDValue getZeroVector(EVT VT, const X86Subtarget *Subtarget,
                             SelectionDAG &DAG, SDLoc dl) {
  assert(VT.isVector() && "Expected a vector type");

  // Always build SSE zero vectors as <4 x i32> bitcasted
  // to their dest type. This ensures they get CSE'd.
  SDValue Vec;
  if (VT.is128BitVector()) {  // SSE
    if (Subtarget->hasSSE2()) {  // SSE2
      SDValue Cst = DAG.getTargetConstant(0, MVT::i32);
      Vec = DAG.getNode(ISD::BUILD_VECTOR, dl, MVT::v4i32, Cst, Cst, Cst, Cst);
    } else { // SSE1
      SDValue Cst = DAG.getTargetConstantFP(+0.0, MVT::f32);
      Vec = DAG.getNode(ISD::BUILD_VECTOR, dl, MVT::v4f32, Cst, Cst, Cst, Cst);
    }
  } else if (VT.is256BitVector()) { // AVX
    if (Subtarget->hasInt256()) { // AVX2
      SDValue Cst = DAG.getTargetConstant(0, MVT::i32);
      SDValue Ops[] = { Cst, Cst, Cst, Cst, Cst, Cst, Cst, Cst };
      Vec = DAG.getNode(ISD::BUILD_VECTOR, dl, MVT::v8i32, Ops);
    } else {
      // 256-bit logic and arithmetic instructions in AVX are all
      // floating-point, no support for integer ops. Emit fp zeroed vectors.
      SDValue Cst = DAG.getTargetConstantFP(+0.0, MVT::f32);
      SDValue Ops[] = { Cst, Cst, Cst, Cst, Cst, Cst, Cst, Cst };
      Vec = DAG.getNode(ISD::BUILD_VECTOR, dl, MVT::v8f32, Ops);
    }
  } else if (VT.is512BitVector()) { // AVX-512
      SDValue Cst = DAG.getTargetConstant(0, MVT::i32);
      SDValue Ops[] = { Cst, Cst, Cst, Cst, Cst, Cst, Cst, Cst,
                        Cst, Cst, Cst, Cst, Cst, Cst, Cst, Cst };
      Vec = DAG.getNode(ISD::BUILD_VECTOR, dl, MVT::v16i32, Ops);
  } else if (VT.getScalarType() == MVT::i1) {
    assert(VT.getVectorNumElements() <= 16 && "Unexpected vector type");
    SDValue Cst = DAG.getTargetConstant(0, MVT::i1);
    SmallVector<SDValue, 16> Ops(VT.getVectorNumElements(), Cst);
    return DAG.getNode(ISD::BUILD_VECTOR, dl, VT, Ops);
  } else
    llvm_unreachable("Unexpected vector type");

  return DAG.getNode(ISD::BITCAST, dl, VT, Vec);
}

/// getOnesVector - Returns a vector of specified type with all bits set.
/// Always build ones vectors as <4 x i32> or <8 x i32>. For 256-bit types with
/// no AVX2 supprt, use two <4 x i32> inserted in a <8 x i32> appropriately.
/// Then bitcast to their original type, ensuring they get CSE'd.
static SDValue getOnesVector(MVT VT, bool HasInt256, SelectionDAG &DAG,
                             SDLoc dl) {
  assert(VT.isVector() && "Expected a vector type");

  SDValue Cst = DAG.getTargetConstant(~0U, MVT::i32);
  SDValue Vec;
  if (VT.is256BitVector()) {
    if (HasInt256) { // AVX2
      SDValue Ops[] = { Cst, Cst, Cst, Cst, Cst, Cst, Cst, Cst };
      Vec = DAG.getNode(ISD::BUILD_VECTOR, dl, MVT::v8i32, Ops);
    } else { // AVX
      Vec = DAG.getNode(ISD::BUILD_VECTOR, dl, MVT::v4i32, Cst, Cst, Cst, Cst);
      Vec = Concat128BitVectors(Vec, Vec, MVT::v8i32, 8, DAG, dl);
    }
  } else if (VT.is128BitVector()) {
    Vec = DAG.getNode(ISD::BUILD_VECTOR, dl, MVT::v4i32, Cst, Cst, Cst, Cst);
  } else
    llvm_unreachable("Unexpected vector type");

  return DAG.getNode(ISD::BITCAST, dl, VT, Vec);
}

/// NormalizeMask - V2 is a splat, modify the mask (if needed) so all elements
/// that point to V2 points to its first element.
static void NormalizeMask(SmallVectorImpl<int> &Mask, unsigned NumElems) {
  for (unsigned i = 0; i != NumElems; ++i) {
    if (Mask[i] > (int)NumElems) {
      Mask[i] = NumElems;
    }
  }
}

/// getMOVLMask - Returns a vector_shuffle mask for an movs{s|d}, movd
/// operation of specified width.
static SDValue getMOVL(SelectionDAG &DAG, SDLoc dl, EVT VT, SDValue V1,
                       SDValue V2) {
  unsigned NumElems = VT.getVectorNumElements();
  SmallVector<int, 8> Mask;
  Mask.push_back(NumElems);
  for (unsigned i = 1; i != NumElems; ++i)
    Mask.push_back(i);
  return DAG.getVectorShuffle(VT, dl, V1, V2, &Mask[0]);
}

/// getUnpackl - Returns a vector_shuffle node for an unpackl operation.
static SDValue getUnpackl(SelectionDAG &DAG, SDLoc dl, MVT VT, SDValue V1,
                          SDValue V2) {
  unsigned NumElems = VT.getVectorNumElements();
  SmallVector<int, 8> Mask;
  for (unsigned i = 0, e = NumElems/2; i != e; ++i) {
    Mask.push_back(i);
    Mask.push_back(i + NumElems);
  }
  return DAG.getVectorShuffle(VT, dl, V1, V2, &Mask[0]);
}

/// getUnpackh - Returns a vector_shuffle node for an unpackh operation.
static SDValue getUnpackh(SelectionDAG &DAG, SDLoc dl, MVT VT, SDValue V1,
                          SDValue V2) {
  unsigned NumElems = VT.getVectorNumElements();
  SmallVector<int, 8> Mask;
  for (unsigned i = 0, Half = NumElems/2; i != Half; ++i) {
    Mask.push_back(i + Half);
    Mask.push_back(i + NumElems + Half);
  }
  return DAG.getVectorShuffle(VT, dl, V1, V2, &Mask[0]);
}

// PromoteSplati8i16 - All i16 and i8 vector types can't be used directly by
// a generic shuffle instruction because the target has no such instructions.
// Generate shuffles which repeat i16 and i8 several times until they can be
// represented by v4f32 and then be manipulated by target suported shuffles.
static SDValue PromoteSplati8i16(SDValue V, SelectionDAG &DAG, int &EltNo) {
  MVT VT = V.getSimpleValueType();
  int NumElems = VT.getVectorNumElements();
  SDLoc dl(V);

  while (NumElems > 4) {
    if (EltNo < NumElems/2) {
      V = getUnpackl(DAG, dl, VT, V, V);
    } else {
      V = getUnpackh(DAG, dl, VT, V, V);
      EltNo -= NumElems/2;
    }
    NumElems >>= 1;
  }
  return V;
}

/// getLegalSplat - Generate a legal splat with supported x86 shuffles
static SDValue getLegalSplat(SelectionDAG &DAG, SDValue V, int EltNo) {
  MVT VT = V.getSimpleValueType();
  SDLoc dl(V);

  if (VT.is128BitVector()) {
    V = DAG.getNode(ISD::BITCAST, dl, MVT::v4f32, V);
    int SplatMask[4] = { EltNo, EltNo, EltNo, EltNo };
    V = DAG.getVectorShuffle(MVT::v4f32, dl, V, DAG.getUNDEF(MVT::v4f32),
                             &SplatMask[0]);
  } else if (VT.is256BitVector()) {
    // To use VPERMILPS to splat scalars, the second half of indicies must
    // refer to the higher part, which is a duplication of the lower one,
    // because VPERMILPS can only handle in-lane permutations.
    int SplatMask[8] = { EltNo, EltNo, EltNo, EltNo,
                         EltNo+4, EltNo+4, EltNo+4, EltNo+4 };

    V = DAG.getNode(ISD::BITCAST, dl, MVT::v8f32, V);
    V = DAG.getVectorShuffle(MVT::v8f32, dl, V, DAG.getUNDEF(MVT::v8f32),
                             &SplatMask[0]);
  } else
    llvm_unreachable("Vector size not supported");

  return DAG.getNode(ISD::BITCAST, dl, VT, V);
}

/// PromoteSplat - Splat is promoted to target supported vector shuffles.
static SDValue PromoteSplat(ShuffleVectorSDNode *SV, SelectionDAG &DAG) {
  MVT SrcVT = SV->getSimpleValueType(0);
  SDValue V1 = SV->getOperand(0);
  SDLoc dl(SV);

  int EltNo = SV->getSplatIndex();
  int NumElems = SrcVT.getVectorNumElements();
  bool Is256BitVec = SrcVT.is256BitVector();

  assert(((SrcVT.is128BitVector() && NumElems > 4) || Is256BitVec) &&
         "Unknown how to promote splat for type");

  // Extract the 128-bit part containing the splat element and update
  // the splat element index when it refers to the higher register.
  if (Is256BitVec) {
    V1 = Extract128BitVector(V1, EltNo, DAG, dl);
    if (EltNo >= NumElems/2)
      EltNo -= NumElems/2;
  }

  // All i16 and i8 vector types can't be used directly by a generic shuffle
  // instruction because the target has no such instruction. Generate shuffles
  // which repeat i16 and i8 several times until they fit in i32, and then can
  // be manipulated by target suported shuffles.
  MVT EltVT = SrcVT.getVectorElementType();
  if (EltVT == MVT::i8 || EltVT == MVT::i16)
    V1 = PromoteSplati8i16(V1, DAG, EltNo);

  // Recreate the 256-bit vector and place the same 128-bit vector
  // into the low and high part. This is necessary because we want
  // to use VPERM* to shuffle the vectors
  if (Is256BitVec) {
    V1 = DAG.getNode(ISD::CONCAT_VECTORS, dl, SrcVT, V1, V1);
  }

  return getLegalSplat(DAG, V1, EltNo);
}

/// getShuffleVectorZeroOrUndef - Return a vector_shuffle of the specified
/// vector of zero or undef vector.  This produces a shuffle where the low
/// element of V2 is swizzled into the zero/undef vector, landing at element
/// Idx.  This produces a shuffle mask like 4,1,2,3 (idx=0) or  0,1,2,4 (idx=3).
static SDValue getShuffleVectorZeroOrUndef(SDValue V2, unsigned Idx,
                                           bool IsZero,
                                           const X86Subtarget *Subtarget,
                                           SelectionDAG &DAG) {
  MVT VT = V2.getSimpleValueType();
  SDValue V1 = IsZero
    ? getZeroVector(VT, Subtarget, DAG, SDLoc(V2)) : DAG.getUNDEF(VT);
  unsigned NumElems = VT.getVectorNumElements();
  SmallVector<int, 16> MaskVec;
  for (unsigned i = 0; i != NumElems; ++i)
    // If this is the insertion idx, put the low elt of V2 here.
    MaskVec.push_back(i == Idx ? NumElems : i);
  return DAG.getVectorShuffle(VT, SDLoc(V2), V1, V2, &MaskVec[0]);
}

/// getTargetShuffleMask - Calculates the shuffle mask corresponding to the
/// target specific opcode. Returns true if the Mask could be calculated. Sets
/// IsUnary to true if only uses one source. Note that this will set IsUnary for
/// shuffles which use a single input multiple times, and in those cases it will
/// adjust the mask to only have indices within that single input.
static bool getTargetShuffleMask(SDNode *N, MVT VT,
                                 SmallVectorImpl<int> &Mask, bool &IsUnary) {
  unsigned NumElems = VT.getVectorNumElements();
  SDValue ImmN;

  IsUnary = false;
  bool IsFakeUnary = false;
  switch(N->getOpcode()) {
  case X86ISD::SHUFP:
    ImmN = N->getOperand(N->getNumOperands()-1);
    DecodeSHUFPMask(VT, cast<ConstantSDNode>(ImmN)->getZExtValue(), Mask);
    IsUnary = IsFakeUnary = N->getOperand(0) == N->getOperand(1);
    break;
  case X86ISD::UNPCKH:
    DecodeUNPCKHMask(VT, Mask);
    IsUnary = IsFakeUnary = N->getOperand(0) == N->getOperand(1);
    break;
  case X86ISD::UNPCKL:
    DecodeUNPCKLMask(VT, Mask);
    IsUnary = IsFakeUnary = N->getOperand(0) == N->getOperand(1);
    break;
  case X86ISD::MOVHLPS:
    DecodeMOVHLPSMask(NumElems, Mask);
    IsUnary = IsFakeUnary = N->getOperand(0) == N->getOperand(1);
    break;
  case X86ISD::MOVLHPS:
    DecodeMOVLHPSMask(NumElems, Mask);
    IsUnary = IsFakeUnary = N->getOperand(0) == N->getOperand(1);
    break;
  case X86ISD::PALIGNR:
    ImmN = N->getOperand(N->getNumOperands()-1);
    DecodePALIGNRMask(VT, cast<ConstantSDNode>(ImmN)->getZExtValue(), Mask);
    break;
  case X86ISD::PSHUFD:
  case X86ISD::VPERMILP:
    ImmN = N->getOperand(N->getNumOperands()-1);
    DecodePSHUFMask(VT, cast<ConstantSDNode>(ImmN)->getZExtValue(), Mask);
    IsUnary = true;
    break;
  case X86ISD::PSHUFHW:
    ImmN = N->getOperand(N->getNumOperands()-1);
    DecodePSHUFHWMask(VT, cast<ConstantSDNode>(ImmN)->getZExtValue(), Mask);
    IsUnary = true;
    break;
  case X86ISD::PSHUFLW:
    ImmN = N->getOperand(N->getNumOperands()-1);
    DecodePSHUFLWMask(VT, cast<ConstantSDNode>(ImmN)->getZExtValue(), Mask);
    IsUnary = true;
    break;
  case X86ISD::PSHUFB: {
    IsUnary = true;
    SDValue MaskNode = N->getOperand(1);
    while (MaskNode->getOpcode() == ISD::BITCAST)
      MaskNode = MaskNode->getOperand(0);

    if (MaskNode->getOpcode() == ISD::BUILD_VECTOR) {
      // If we have a build-vector, then things are easy.
      EVT VT = MaskNode.getValueType();
      assert(VT.isVector() &&
             "Can't produce a non-vector with a build_vector!");
      if (!VT.isInteger())
        return false;

      int NumBytesPerElement = VT.getVectorElementType().getSizeInBits() / 8;

      SmallVector<uint64_t, 32> RawMask;
      for (int i = 0, e = MaskNode->getNumOperands(); i < e; ++i) {
        auto *CN = dyn_cast<ConstantSDNode>(MaskNode->getOperand(i));
        if (!CN)
          return false;
        APInt MaskElement = CN->getAPIntValue();

        // We now have to decode the element which could be any integer size and
        // extract each byte of it.
        for (int j = 0; j < NumBytesPerElement; ++j) {
          // Note that this is x86 and so always little endian: the low byte is
          // the first byte of the mask.
          RawMask.push_back(MaskElement.getLoBits(8).getZExtValue());
          MaskElement = MaskElement.lshr(8);
        }
      }
      DecodePSHUFBMask(RawMask, Mask);
      break;
    }

    auto *MaskLoad = dyn_cast<LoadSDNode>(MaskNode);
    if (!MaskLoad)
      return false;

    SDValue Ptr = MaskLoad->getBasePtr();
    if (Ptr->getOpcode() == X86ISD::Wrapper)
      Ptr = Ptr->getOperand(0);

    auto *MaskCP = dyn_cast<ConstantPoolSDNode>(Ptr);
    if (!MaskCP || MaskCP->isMachineConstantPoolEntry())
      return false;

    if (auto *C = dyn_cast<ConstantDataSequential>(MaskCP->getConstVal())) {
      // FIXME: Support AVX-512 here.
      if (!C->getType()->isVectorTy() ||
          (C->getNumElements() != 16 && C->getNumElements() != 32))
        return false;

      assert(C->getType()->isVectorTy() && "Expected a vector constant.");
      DecodePSHUFBMask(C, Mask);
      break;
    }

    return false;
  }
  case X86ISD::VPERMI:
    ImmN = N->getOperand(N->getNumOperands()-1);
    DecodeVPERMMask(cast<ConstantSDNode>(ImmN)->getZExtValue(), Mask);
    IsUnary = true;
    break;
  case X86ISD::MOVSS:
  case X86ISD::MOVSD: {
    // The index 0 always comes from the first element of the second source,
    // this is why MOVSS and MOVSD are used in the first place. The other
    // elements come from the other positions of the first source vector
    Mask.push_back(NumElems);
    for (unsigned i = 1; i != NumElems; ++i) {
      Mask.push_back(i);
    }
    break;
  }
  case X86ISD::VPERM2X128:
    ImmN = N->getOperand(N->getNumOperands()-1);
    DecodeVPERM2X128Mask(VT, cast<ConstantSDNode>(ImmN)->getZExtValue(), Mask);
    if (Mask.empty()) return false;
    break;
  case X86ISD::MOVDDUP:
  case X86ISD::MOVLHPD:
  case X86ISD::MOVLPD:
  case X86ISD::MOVLPS:
  case X86ISD::MOVSHDUP:
  case X86ISD::MOVSLDUP:
    // Not yet implemented
    return false;
  default: llvm_unreachable("unknown target shuffle node");
  }

  // If we have a fake unary shuffle, the shuffle mask is spread across two
  // inputs that are actually the same node. Re-map the mask to always point
  // into the first input.
  if (IsFakeUnary)
    for (int &M : Mask)
      if (M >= (int)Mask.size())
        M -= Mask.size();

  return true;
}

/// getShuffleScalarElt - Returns the scalar element that will make up the ith
/// element of the result of the vector shuffle.
static SDValue getShuffleScalarElt(SDNode *N, unsigned Index, SelectionDAG &DAG,
                                   unsigned Depth) {
  if (Depth == 6)
    return SDValue();  // Limit search depth.

  SDValue V = SDValue(N, 0);
  EVT VT = V.getValueType();
  unsigned Opcode = V.getOpcode();

  // Recurse into ISD::VECTOR_SHUFFLE node to find scalars.
  if (const ShuffleVectorSDNode *SV = dyn_cast<ShuffleVectorSDNode>(N)) {
    int Elt = SV->getMaskElt(Index);

    if (Elt < 0)
      return DAG.getUNDEF(VT.getVectorElementType());

    unsigned NumElems = VT.getVectorNumElements();
    SDValue NewV = (Elt < (int)NumElems) ? SV->getOperand(0)
                                         : SV->getOperand(1);
    return getShuffleScalarElt(NewV.getNode(), Elt % NumElems, DAG, Depth+1);
  }

  // Recurse into target specific vector shuffles to find scalars.
  if (isTargetShuffle(Opcode)) {
    MVT ShufVT = V.getSimpleValueType();
    unsigned NumElems = ShufVT.getVectorNumElements();
    SmallVector<int, 16> ShuffleMask;
    bool IsUnary;

    if (!getTargetShuffleMask(N, ShufVT, ShuffleMask, IsUnary))
      return SDValue();

    int Elt = ShuffleMask[Index];
    if (Elt < 0)
      return DAG.getUNDEF(ShufVT.getVectorElementType());

    SDValue NewV = (Elt < (int)NumElems) ? N->getOperand(0)
                                         : N->getOperand(1);
    return getShuffleScalarElt(NewV.getNode(), Elt % NumElems, DAG,
                               Depth+1);
  }

  // Actual nodes that may contain scalar elements
  if (Opcode == ISD::BITCAST) {
    V = V.getOperand(0);
    EVT SrcVT = V.getValueType();
    unsigned NumElems = VT.getVectorNumElements();

    if (!SrcVT.isVector() || SrcVT.getVectorNumElements() != NumElems)
      return SDValue();
  }

  if (V.getOpcode() == ISD::SCALAR_TO_VECTOR)
    return (Index == 0) ? V.getOperand(0)
                        : DAG.getUNDEF(VT.getVectorElementType());

  if (V.getOpcode() == ISD::BUILD_VECTOR)
    return V.getOperand(Index);

  return SDValue();
}

/// getNumOfConsecutiveZeros - Return the number of elements of a vector
/// shuffle operation which come from a consecutively from a zero. The
/// search can start in two different directions, from left or right.
/// We count undefs as zeros until PreferredNum is reached.
static unsigned getNumOfConsecutiveZeros(ShuffleVectorSDNode *SVOp,
                                         unsigned NumElems, bool ZerosFromLeft,
                                         SelectionDAG &DAG,
                                         unsigned PreferredNum = -1U) {
  unsigned NumZeros = 0;
  for (unsigned i = 0; i != NumElems; ++i) {
    unsigned Index = ZerosFromLeft ? i : NumElems - i - 1;
    SDValue Elt = getShuffleScalarElt(SVOp, Index, DAG, 0);
    if (!Elt.getNode())
      break;

    if (X86::isZeroNode(Elt))
      ++NumZeros;
    else if (Elt.getOpcode() == ISD::UNDEF) // Undef as zero up to PreferredNum.
      NumZeros = std::min(NumZeros + 1, PreferredNum);
    else
      break;
  }

  return NumZeros;
}

/// isShuffleMaskConsecutive - Check if the shuffle mask indicies [MaskI, MaskE)
/// correspond consecutively to elements from one of the vector operands,
/// starting from its index OpIdx. Also tell OpNum which source vector operand.
static
bool isShuffleMaskConsecutive(ShuffleVectorSDNode *SVOp,
                              unsigned MaskI, unsigned MaskE, unsigned OpIdx,
                              unsigned NumElems, unsigned &OpNum) {
  bool SeenV1 = false;
  bool SeenV2 = false;

  for (unsigned i = MaskI; i != MaskE; ++i, ++OpIdx) {
    int Idx = SVOp->getMaskElt(i);
    // Ignore undef indicies
    if (Idx < 0)
      continue;

    if (Idx < (int)NumElems)
      SeenV1 = true;
    else
      SeenV2 = true;

    // Only accept consecutive elements from the same vector
    if ((Idx % NumElems != OpIdx) || (SeenV1 && SeenV2))
      return false;
  }

  OpNum = SeenV1 ? 0 : 1;
  return true;
}

/// isVectorShiftRight - Returns true if the shuffle can be implemented as a
/// logical left shift of a vector.
static bool isVectorShiftRight(ShuffleVectorSDNode *SVOp, SelectionDAG &DAG,
                               bool &isLeft, SDValue &ShVal, unsigned &ShAmt) {
  unsigned NumElems =
    SVOp->getSimpleValueType(0).getVectorNumElements();
  unsigned NumZeros = getNumOfConsecutiveZeros(
      SVOp, NumElems, false /* check zeros from right */, DAG,
      SVOp->getMaskElt(0));
  unsigned OpSrc;

  if (!NumZeros)
    return false;

  // Considering the elements in the mask that are not consecutive zeros,
  // check if they consecutively come from only one of the source vectors.
  //
  //               V1 = {X, A, B, C}     0
  //                         \  \  \    /
  //   vector_shuffle V1, V2 <1, 2, 3, X>
  //
  if (!isShuffleMaskConsecutive(SVOp,
            0,                   // Mask Start Index
            NumElems-NumZeros,   // Mask End Index(exclusive)
            NumZeros,            // Where to start looking in the src vector
            NumElems,            // Number of elements in vector
            OpSrc))              // Which source operand ?
    return false;

  isLeft = false;
  ShAmt = NumZeros;
  ShVal = SVOp->getOperand(OpSrc);
  return true;
}

/// isVectorShiftLeft - Returns true if the shuffle can be implemented as a
/// logical left shift of a vector.
static bool isVectorShiftLeft(ShuffleVectorSDNode *SVOp, SelectionDAG &DAG,
                              bool &isLeft, SDValue &ShVal, unsigned &ShAmt) {
  unsigned NumElems =
    SVOp->getSimpleValueType(0).getVectorNumElements();
  unsigned NumZeros = getNumOfConsecutiveZeros(
      SVOp, NumElems, true /* check zeros from left */, DAG,
      NumElems - SVOp->getMaskElt(NumElems - 1) - 1);
  unsigned OpSrc;

  if (!NumZeros)
    return false;

  // Considering the elements in the mask that are not consecutive zeros,
  // check if they consecutively come from only one of the source vectors.
  //
  //                           0    { A, B, X, X } = V2
  //                          / \    /  /
  //   vector_shuffle V1, V2 <X, X, 4, 5>
  //
  if (!isShuffleMaskConsecutive(SVOp,
            NumZeros,     // Mask Start Index
            NumElems,     // Mask End Index(exclusive)
            0,            // Where to start looking in the src vector
            NumElems,     // Number of elements in vector
            OpSrc))       // Which source operand ?
    return false;

  isLeft = true;
  ShAmt = NumZeros;
  ShVal = SVOp->getOperand(OpSrc);
  return true;
}

/// isVectorShift - Returns true if the shuffle can be implemented as a
/// logical left or right shift of a vector.
static bool isVectorShift(ShuffleVectorSDNode *SVOp, SelectionDAG &DAG,
                          bool &isLeft, SDValue &ShVal, unsigned &ShAmt) {
  // Although the logic below support any bitwidth size, there are no
  // shift instructions which handle more than 128-bit vectors.
  if (!SVOp->getSimpleValueType(0).is128BitVector())
    return false;

  if (isVectorShiftLeft(SVOp, DAG, isLeft, ShVal, ShAmt) ||
      isVectorShiftRight(SVOp, DAG, isLeft, ShVal, ShAmt))
    return true;

  return false;
}

/// LowerBuildVectorv16i8 - Custom lower build_vector of v16i8.
///
static SDValue LowerBuildVectorv16i8(SDValue Op, unsigned NonZeros,
                                       unsigned NumNonZero, unsigned NumZero,
                                       SelectionDAG &DAG,
                                       const X86Subtarget* Subtarget,
                                       const TargetLowering &TLI) {
  if (NumNonZero > 8)
    return SDValue();

  SDLoc dl(Op);
  SDValue V;
  bool First = true;
  for (unsigned i = 0; i < 16; ++i) {
    bool ThisIsNonZero = (NonZeros & (1 << i)) != 0;
    if (ThisIsNonZero && First) {
      if (NumZero)
        V = getZeroVector(MVT::v8i16, Subtarget, DAG, dl);
      else
        V = DAG.getUNDEF(MVT::v8i16);
      First = false;
    }

    if ((i & 1) != 0) {
      SDValue ThisElt, LastElt;
      bool LastIsNonZero = (NonZeros & (1 << (i-1))) != 0;
      if (LastIsNonZero) {
        LastElt = DAG.getNode(ISD::ZERO_EXTEND, dl,
                              MVT::i16, Op.getOperand(i-1));
      }
      if (ThisIsNonZero) {
        ThisElt = DAG.getNode(ISD::ZERO_EXTEND, dl, MVT::i16, Op.getOperand(i));
        ThisElt = DAG.getNode(ISD::SHL, dl, MVT::i16,
                              ThisElt, DAG.getConstant(8, MVT::i8));
        if (LastIsNonZero)
          ThisElt = DAG.getNode(ISD::OR, dl, MVT::i16, ThisElt, LastElt);
      } else
        ThisElt = LastElt;

      if (ThisElt.getNode())
        V = DAG.getNode(ISD::INSERT_VECTOR_ELT, dl, MVT::v8i16, V, ThisElt,
                        DAG.getIntPtrConstant(i/2));
    }
  }

  return DAG.getNode(ISD::BITCAST, dl, MVT::v16i8, V);
}

/// LowerBuildVectorv8i16 - Custom lower build_vector of v8i16.
///
static SDValue LowerBuildVectorv8i16(SDValue Op, unsigned NonZeros,
                                     unsigned NumNonZero, unsigned NumZero,
                                     SelectionDAG &DAG,
                                     const X86Subtarget* Subtarget,
                                     const TargetLowering &TLI) {
  if (NumNonZero > 4)
    return SDValue();

  SDLoc dl(Op);
  SDValue V;
  bool First = true;
  for (unsigned i = 0; i < 8; ++i) {
    bool isNonZero = (NonZeros & (1 << i)) != 0;
    if (isNonZero) {
      if (First) {
        if (NumZero)
          V = getZeroVector(MVT::v8i16, Subtarget, DAG, dl);
        else
          V = DAG.getUNDEF(MVT::v8i16);
        First = false;
      }
      V = DAG.getNode(ISD::INSERT_VECTOR_ELT, dl,
                      MVT::v8i16, V, Op.getOperand(i),
                      DAG.getIntPtrConstant(i));
    }
  }

  return V;
}

/// LowerBuildVectorv4x32 - Custom lower build_vector of v4i32 or v4f32.
static SDValue LowerBuildVectorv4x32(SDValue Op, unsigned NumElems,
                                     unsigned NonZeros, unsigned NumNonZero,
                                     unsigned NumZero, SelectionDAG &DAG,
                                     const X86Subtarget *Subtarget,
                                     const TargetLowering &TLI) {
  // We know there's at least one non-zero element
  unsigned FirstNonZeroIdx = 0;
  SDValue FirstNonZero = Op->getOperand(FirstNonZeroIdx);
  while (FirstNonZero.getOpcode() == ISD::UNDEF ||
         X86::isZeroNode(FirstNonZero)) {
    ++FirstNonZeroIdx;
    FirstNonZero = Op->getOperand(FirstNonZeroIdx);
  }

  if (FirstNonZero.getOpcode() != ISD::EXTRACT_VECTOR_ELT ||
      !isa<ConstantSDNode>(FirstNonZero.getOperand(1)))
    return SDValue();

  SDValue V = FirstNonZero.getOperand(0);
  MVT VVT = V.getSimpleValueType();
  if (!Subtarget->hasSSE41() || (VVT != MVT::v4f32 && VVT != MVT::v4i32))
    return SDValue();

  unsigned FirstNonZeroDst =
      cast<ConstantSDNode>(FirstNonZero.getOperand(1))->getZExtValue();
  unsigned CorrectIdx = FirstNonZeroDst == FirstNonZeroIdx;
  unsigned IncorrectIdx = CorrectIdx ? -1U : FirstNonZeroIdx;
  unsigned IncorrectDst = CorrectIdx ? -1U : FirstNonZeroDst;

  for (unsigned Idx = FirstNonZeroIdx + 1; Idx < NumElems; ++Idx) {
    SDValue Elem = Op.getOperand(Idx);
    if (Elem.getOpcode() == ISD::UNDEF || X86::isZeroNode(Elem))
      continue;

    // TODO: What else can be here? Deal with it.
    if (Elem.getOpcode() != ISD::EXTRACT_VECTOR_ELT)
      return SDValue();

    // TODO: Some optimizations are still possible here
    // ex: Getting one element from a vector, and the rest from another.
    if (Elem.getOperand(0) != V)
      return SDValue();

    unsigned Dst = cast<ConstantSDNode>(Elem.getOperand(1))->getZExtValue();
    if (Dst == Idx)
      ++CorrectIdx;
    else if (IncorrectIdx == -1U) {
      IncorrectIdx = Idx;
      IncorrectDst = Dst;
    } else
      // There was already one element with an incorrect index.
      // We can't optimize this case to an insertps.
      return SDValue();
  }

  if (NumNonZero == CorrectIdx || NumNonZero == CorrectIdx + 1) {
    SDLoc dl(Op);
    EVT VT = Op.getSimpleValueType();
    unsigned ElementMoveMask = 0;
    if (IncorrectIdx == -1U)
      ElementMoveMask = FirstNonZeroIdx << 6 | FirstNonZeroIdx << 4;
    else
      ElementMoveMask = IncorrectDst << 6 | IncorrectIdx << 4;

    SDValue InsertpsMask =
        DAG.getIntPtrConstant(ElementMoveMask | (~NonZeros & 0xf));
    return DAG.getNode(X86ISD::INSERTPS, dl, VT, V, V, InsertpsMask);
  }

  return SDValue();
}

/// getVShift - Return a vector logical shift node.
///
static SDValue getVShift(bool isLeft, EVT VT, SDValue SrcOp,
                         unsigned NumBits, SelectionDAG &DAG,
                         const TargetLowering &TLI, SDLoc dl) {
  assert(VT.is128BitVector() && "Unknown type for VShift");
  EVT ShVT = MVT::v2i64;
  unsigned Opc = isLeft ? X86ISD::VSHLDQ : X86ISD::VSRLDQ;
  SrcOp = DAG.getNode(ISD::BITCAST, dl, ShVT, SrcOp);
  return DAG.getNode(ISD::BITCAST, dl, VT,
                     DAG.getNode(Opc, dl, ShVT, SrcOp,
                             DAG.getConstant(NumBits,
                                  TLI.getScalarShiftAmountTy(SrcOp.getValueType()))));
}

static SDValue
LowerAsSplatVectorLoad(SDValue SrcOp, MVT VT, SDLoc dl, SelectionDAG &DAG) {

  // Check if the scalar load can be widened into a vector load. And if
  // the address is "base + cst" see if the cst can be "absorbed" into
  // the shuffle mask.
  if (LoadSDNode *LD = dyn_cast<LoadSDNode>(SrcOp)) {
    SDValue Ptr = LD->getBasePtr();
    if (!ISD::isNormalLoad(LD) || LD->isVolatile())
      return SDValue();
    EVT PVT = LD->getValueType(0);
    if (PVT != MVT::i32 && PVT != MVT::f32)
      return SDValue();

    int FI = -1;
    int64_t Offset = 0;
    if (FrameIndexSDNode *FINode = dyn_cast<FrameIndexSDNode>(Ptr)) {
      FI = FINode->getIndex();
      Offset = 0;
    } else if (DAG.isBaseWithConstantOffset(Ptr) &&
               isa<FrameIndexSDNode>(Ptr.getOperand(0))) {
      FI = cast<FrameIndexSDNode>(Ptr.getOperand(0))->getIndex();
      Offset = Ptr.getConstantOperandVal(1);
      Ptr = Ptr.getOperand(0);
    } else {
      return SDValue();
    }

    // FIXME: 256-bit vector instructions don't require a strict alignment,
    // improve this code to support it better.
    unsigned RequiredAlign = VT.getSizeInBits()/8;
    SDValue Chain = LD->getChain();
    // Make sure the stack object alignment is at least 16 or 32.
    MachineFrameInfo *MFI = DAG.getMachineFunction().getFrameInfo();
    if (DAG.InferPtrAlignment(Ptr) < RequiredAlign) {
      if (MFI->isFixedObjectIndex(FI)) {
        // Can't change the alignment. FIXME: It's possible to compute
        // the exact stack offset and reference FI + adjust offset instead.
        // If someone *really* cares about this. That's the way to implement it.
        return SDValue();
      } else {
        MFI->setObjectAlignment(FI, RequiredAlign);
      }
    }

    // (Offset % 16 or 32) must be multiple of 4. Then address is then
    // Ptr + (Offset & ~15).
    if (Offset < 0)
      return SDValue();
    if ((Offset % RequiredAlign) & 3)
      return SDValue();
    int64_t StartOffset = Offset & ~(RequiredAlign-1);
    if (StartOffset)
      Ptr = DAG.getNode(ISD::ADD, SDLoc(Ptr), Ptr.getValueType(),
                        Ptr,DAG.getConstant(StartOffset, Ptr.getValueType()));

    int EltNo = (Offset - StartOffset) >> 2;
    unsigned NumElems = VT.getVectorNumElements();

    EVT NVT = EVT::getVectorVT(*DAG.getContext(), PVT, NumElems);
    SDValue V1 = DAG.getLoad(NVT, dl, Chain, Ptr,
                             LD->getPointerInfo().getWithOffset(StartOffset),
                             false, false, false, 0);

    SmallVector<int, 8> Mask;
    for (unsigned i = 0; i != NumElems; ++i)
      Mask.push_back(EltNo);

    return DAG.getVectorShuffle(NVT, dl, V1, DAG.getUNDEF(NVT), &Mask[0]);
  }

  return SDValue();
}

/// EltsFromConsecutiveLoads - Given the initializing elements 'Elts' of a
/// vector of type 'VT', see if the elements can be replaced by a single large
/// load which has the same value as a build_vector whose operands are 'elts'.
///
/// Example: <load i32 *a, load i32 *a+4, undef, undef> -> zextload a
///
/// FIXME: we'd also like to handle the case where the last elements are zero
/// rather than undef via VZEXT_LOAD, but we do not detect that case today.
/// There's even a handy isZeroNode for that purpose.
static SDValue EltsFromConsecutiveLoads(EVT VT, SmallVectorImpl<SDValue> &Elts,
                                        SDLoc &DL, SelectionDAG &DAG,
                                        bool isAfterLegalize) {
  EVT EltVT = VT.getVectorElementType();
  unsigned NumElems = Elts.size();

  LoadSDNode *LDBase = nullptr;
  unsigned LastLoadedElt = -1U;

  // For each element in the initializer, see if we've found a load or an undef.
  // If we don't find an initial load element, or later load elements are
  // non-consecutive, bail out.
  for (unsigned i = 0; i < NumElems; ++i) {
    SDValue Elt = Elts[i];

    if (!Elt.getNode() ||
        (Elt.getOpcode() != ISD::UNDEF && !ISD::isNON_EXTLoad(Elt.getNode())))
      return SDValue();
    if (!LDBase) {
      if (Elt.getNode()->getOpcode() == ISD::UNDEF)
        return SDValue();
      LDBase = cast<LoadSDNode>(Elt.getNode());
      LastLoadedElt = i;
      continue;
    }
    if (Elt.getOpcode() == ISD::UNDEF)
      continue;

    LoadSDNode *LD = cast<LoadSDNode>(Elt);
    if (!DAG.isConsecutiveLoad(LD, LDBase, EltVT.getSizeInBits()/8, i))
      return SDValue();
    LastLoadedElt = i;
  }

  // If we have found an entire vector of loads and undefs, then return a large
  // load of the entire vector width starting at the base pointer.  If we found
  // consecutive loads for the low half, generate a vzext_load node.
  if (LastLoadedElt == NumElems - 1) {

    if (isAfterLegalize &&
        !DAG.getTargetLoweringInfo().isOperationLegal(ISD::LOAD, VT))
      return SDValue();

    SDValue NewLd = SDValue();

    if (DAG.InferPtrAlignment(LDBase->getBasePtr()) >= 16)
      NewLd = DAG.getLoad(VT, DL, LDBase->getChain(), LDBase->getBasePtr(),
                          LDBase->getPointerInfo(),
                          LDBase->isVolatile(), LDBase->isNonTemporal(),
                          LDBase->isInvariant(), 0);
    NewLd = DAG.getLoad(VT, DL, LDBase->getChain(), LDBase->getBasePtr(),
                        LDBase->getPointerInfo(),
                        LDBase->isVolatile(), LDBase->isNonTemporal(),
                        LDBase->isInvariant(), LDBase->getAlignment());

    if (LDBase->hasAnyUseOfValue(1)) {
      SDValue NewChain = DAG.getNode(ISD::TokenFactor, DL, MVT::Other,
                                     SDValue(LDBase, 1),
                                     SDValue(NewLd.getNode(), 1));
      DAG.ReplaceAllUsesOfValueWith(SDValue(LDBase, 1), NewChain);
      DAG.UpdateNodeOperands(NewChain.getNode(), SDValue(LDBase, 1),
                             SDValue(NewLd.getNode(), 1));
    }

    return NewLd;
  }
  if (NumElems == 4 && LastLoadedElt == 1 &&
      DAG.getTargetLoweringInfo().isTypeLegal(MVT::v2i64)) {
    SDVTList Tys = DAG.getVTList(MVT::v2i64, MVT::Other);
    SDValue Ops[] = { LDBase->getChain(), LDBase->getBasePtr() };
    SDValue ResNode =
        DAG.getMemIntrinsicNode(X86ISD::VZEXT_LOAD, DL, Tys, Ops, MVT::i64,
                                LDBase->getPointerInfo(),
                                LDBase->getAlignment(),
                                false/*isVolatile*/, true/*ReadMem*/,
                                false/*WriteMem*/);

    // Make sure the newly-created LOAD is in the same position as LDBase in
    // terms of dependency. We create a TokenFactor for LDBase and ResNode, and
    // update uses of LDBase's output chain to use the TokenFactor.
    if (LDBase->hasAnyUseOfValue(1)) {
      SDValue NewChain = DAG.getNode(ISD::TokenFactor, DL, MVT::Other,
                             SDValue(LDBase, 1), SDValue(ResNode.getNode(), 1));
      DAG.ReplaceAllUsesOfValueWith(SDValue(LDBase, 1), NewChain);
      DAG.UpdateNodeOperands(NewChain.getNode(), SDValue(LDBase, 1),
                             SDValue(ResNode.getNode(), 1));
    }

    return DAG.getNode(ISD::BITCAST, DL, VT, ResNode);
  }
  return SDValue();
}

/// LowerVectorBroadcast - Attempt to use the vbroadcast instruction
/// to generate a splat value for the following cases:
/// 1. A splat BUILD_VECTOR which uses a single scalar load, or a constant.
/// 2. A splat shuffle which uses a scalar_to_vector node which comes from
/// a scalar load, or a constant.
/// The VBROADCAST node is returned when a pattern is found,
/// or SDValue() otherwise.
static SDValue LowerVectorBroadcast(SDValue Op, const X86Subtarget* Subtarget,
                                    SelectionDAG &DAG) {
  if (!Subtarget->hasFp256())
    return SDValue();

  MVT VT = Op.getSimpleValueType();
  SDLoc dl(Op);

  assert((VT.is128BitVector() || VT.is256BitVector() || VT.is512BitVector()) &&
         "Unsupported vector type for broadcast.");

  SDValue Ld;
  bool ConstSplatVal;

  switch (Op.getOpcode()) {
    default:
      // Unknown pattern found.
      return SDValue();

    case ISD::BUILD_VECTOR: {
      auto *BVOp = cast<BuildVectorSDNode>(Op.getNode());
      BitVector UndefElements;
      SDValue Splat = BVOp->getSplatValue(&UndefElements);

      // We need a splat of a single value to use broadcast, and it doesn't
      // make any sense if the value is only in one element of the vector.
      if (!Splat || (VT.getVectorNumElements() - UndefElements.count()) <= 1)
        return SDValue();

      Ld = Splat;
      ConstSplatVal = (Ld.getOpcode() == ISD::Constant ||
                       Ld.getOpcode() == ISD::ConstantFP);

      // Make sure that all of the users of a non-constant load are from the
      // BUILD_VECTOR node.
      if (!ConstSplatVal && !BVOp->isOnlyUserOf(Ld.getNode()))
        return SDValue();
      break;
    }

    case ISD::VECTOR_SHUFFLE: {
      ShuffleVectorSDNode *SVOp = cast<ShuffleVectorSDNode>(Op);

      // Shuffles must have a splat mask where the first element is
      // broadcasted.
      if ((!SVOp->isSplat()) || SVOp->getMaskElt(0) != 0)
        return SDValue();

      SDValue Sc = Op.getOperand(0);
      if (Sc.getOpcode() != ISD::SCALAR_TO_VECTOR &&
          Sc.getOpcode() != ISD::BUILD_VECTOR) {

        if (!Subtarget->hasInt256())
          return SDValue();

        // Use the register form of the broadcast instruction available on AVX2.
        if (VT.getSizeInBits() >= 256)
          Sc = Extract128BitVector(Sc, 0, DAG, dl);
        return DAG.getNode(X86ISD::VBROADCAST, dl, VT, Sc);
      }

      Ld = Sc.getOperand(0);
      ConstSplatVal = (Ld.getOpcode() == ISD::Constant ||
                       Ld.getOpcode() == ISD::ConstantFP);

      // The scalar_to_vector node and the suspected
      // load node must have exactly one user.
      // Constants may have multiple users.

      // AVX-512 has register version of the broadcast
      bool hasRegVer = Subtarget->hasAVX512() && VT.is512BitVector() &&
        Ld.getValueType().getSizeInBits() >= 32;
      if (!ConstSplatVal && ((!Sc.hasOneUse() || !Ld.hasOneUse()) &&
          !hasRegVer))
        return SDValue();
      break;
    }
  }

  bool IsGE256 = (VT.getSizeInBits() >= 256);

  // Handle the broadcasting a single constant scalar from the constant pool
  // into a vector. On Sandybridge it is still better to load a constant vector
  // from the constant pool and not to broadcast it from a scalar.
  if (ConstSplatVal && Subtarget->hasInt256()) {
    EVT CVT = Ld.getValueType();
    assert(!CVT.isVector() && "Must not broadcast a vector type");
    unsigned ScalarSize = CVT.getSizeInBits();

    if (ScalarSize == 32 || (IsGE256 && ScalarSize == 64)) {
      const Constant *C = nullptr;
      if (ConstantSDNode *CI = dyn_cast<ConstantSDNode>(Ld))
        C = CI->getConstantIntValue();
      else if (ConstantFPSDNode *CF = dyn_cast<ConstantFPSDNode>(Ld))
        C = CF->getConstantFPValue();

      assert(C && "Invalid constant type");

      const TargetLowering &TLI = DAG.getTargetLoweringInfo();
      SDValue CP = DAG.getConstantPool(C, TLI.getPointerTy());
      unsigned Alignment = cast<ConstantPoolSDNode>(CP)->getAlignment();
      Ld = DAG.getLoad(CVT, dl, DAG.getEntryNode(), CP,
                       MachinePointerInfo::getConstantPool(),
                       false, false, false, Alignment);

      return DAG.getNode(X86ISD::VBROADCAST, dl, VT, Ld);
    }
  }

  bool IsLoad = ISD::isNormalLoad(Ld.getNode());
  unsigned ScalarSize = Ld.getValueType().getSizeInBits();

  // Handle AVX2 in-register broadcasts.
  if (!IsLoad && Subtarget->hasInt256() &&
      (ScalarSize == 32 || (IsGE256 && ScalarSize == 64)))
    return DAG.getNode(X86ISD::VBROADCAST, dl, VT, Ld);

  // The scalar source must be a normal load.
  if (!IsLoad)
    return SDValue();

  if (ScalarSize == 32 || (IsGE256 && ScalarSize == 64))
    return DAG.getNode(X86ISD::VBROADCAST, dl, VT, Ld);

  // The integer check is needed for the 64-bit into 128-bit so it doesn't match
  // double since there is no vbroadcastsd xmm
  if (Subtarget->hasInt256() && Ld.getValueType().isInteger()) {
    if (ScalarSize == 8 || ScalarSize == 16 || ScalarSize == 64)
      return DAG.getNode(X86ISD::VBROADCAST, dl, VT, Ld);
  }

  // Unsupported broadcast.
  return SDValue();
}

/// \brief For an EXTRACT_VECTOR_ELT with a constant index return the real
/// underlying vector and index.
///
/// Modifies \p ExtractedFromVec to the real vector and returns the real
/// index.
static int getUnderlyingExtractedFromVec(SDValue &ExtractedFromVec,
                                         SDValue ExtIdx) {
  int Idx = cast<ConstantSDNode>(ExtIdx)->getZExtValue();
  if (!isa<ShuffleVectorSDNode>(ExtractedFromVec))
    return Idx;

  // For 256-bit vectors, LowerEXTRACT_VECTOR_ELT_SSE4 may have already
  // lowered this:
  //   (extract_vector_elt (v8f32 %vreg1), Constant<6>)
  // to:
  //   (extract_vector_elt (vector_shuffle<2,u,u,u>
  //                           (extract_subvector (v8f32 %vreg0), Constant<4>),
  //                           undef)
  //                       Constant<0>)
  // In this case the vector is the extract_subvector expression and the index
  // is 2, as specified by the shuffle.
  ShuffleVectorSDNode *SVOp = cast<ShuffleVectorSDNode>(ExtractedFromVec);
  SDValue ShuffleVec = SVOp->getOperand(0);
  MVT ShuffleVecVT = ShuffleVec.getSimpleValueType();
  assert(ShuffleVecVT.getVectorElementType() ==
         ExtractedFromVec.getSimpleValueType().getVectorElementType());

  int ShuffleIdx = SVOp->getMaskElt(Idx);
  if (isUndefOrInRange(ShuffleIdx, 0, ShuffleVecVT.getVectorNumElements())) {
    ExtractedFromVec = ShuffleVec;
    return ShuffleIdx;
  }
  return Idx;
}

static SDValue buildFromShuffleMostly(SDValue Op, SelectionDAG &DAG) {
  MVT VT = Op.getSimpleValueType();

  // Skip if insert_vec_elt is not supported.
  const TargetLowering &TLI = DAG.getTargetLoweringInfo();
  if (!TLI.isOperationLegalOrCustom(ISD::INSERT_VECTOR_ELT, VT))
    return SDValue();

  SDLoc DL(Op);
  unsigned NumElems = Op.getNumOperands();

  SDValue VecIn1;
  SDValue VecIn2;
  SmallVector<unsigned, 4> InsertIndices;
  SmallVector<int, 8> Mask(NumElems, -1);

  for (unsigned i = 0; i != NumElems; ++i) {
    unsigned Opc = Op.getOperand(i).getOpcode();

    if (Opc == ISD::UNDEF)
      continue;

    if (Opc != ISD::EXTRACT_VECTOR_ELT) {
      // Quit if more than 1 elements need inserting.
      if (InsertIndices.size() > 1)
        return SDValue();

      InsertIndices.push_back(i);
      continue;
    }

    SDValue ExtractedFromVec = Op.getOperand(i).getOperand(0);
    SDValue ExtIdx = Op.getOperand(i).getOperand(1);
    // Quit if non-constant index.
    if (!isa<ConstantSDNode>(ExtIdx))
      return SDValue();
    int Idx = getUnderlyingExtractedFromVec(ExtractedFromVec, ExtIdx);

    // Quit if extracted from vector of different type.
    if (ExtractedFromVec.getValueType() != VT)
      return SDValue();

    if (!VecIn1.getNode())
      VecIn1 = ExtractedFromVec;
    else if (VecIn1 != ExtractedFromVec) {
      if (!VecIn2.getNode())
        VecIn2 = ExtractedFromVec;
      else if (VecIn2 != ExtractedFromVec)
        // Quit if more than 2 vectors to shuffle
        return SDValue();
    }

    if (ExtractedFromVec == VecIn1)
      Mask[i] = Idx;
    else if (ExtractedFromVec == VecIn2)
      Mask[i] = Idx + NumElems;
  }

  if (!VecIn1.getNode())
    return SDValue();

  VecIn2 = VecIn2.getNode() ? VecIn2 : DAG.getUNDEF(VT);
  SDValue NV = DAG.getVectorShuffle(VT, DL, VecIn1, VecIn2, &Mask[0]);
  for (unsigned i = 0, e = InsertIndices.size(); i != e; ++i) {
    unsigned Idx = InsertIndices[i];
    NV = DAG.getNode(ISD::INSERT_VECTOR_ELT, DL, VT, NV, Op.getOperand(Idx),
                     DAG.getIntPtrConstant(Idx));
  }

  return NV;
}

// Lower BUILD_VECTOR operation for v8i1 and v16i1 types.
SDValue
X86TargetLowering::LowerBUILD_VECTORvXi1(SDValue Op, SelectionDAG &DAG) const {

  MVT VT = Op.getSimpleValueType();
  assert((VT.getVectorElementType() == MVT::i1) && (VT.getSizeInBits() <= 16) &&
         "Unexpected type in LowerBUILD_VECTORvXi1!");

  SDLoc dl(Op);
  if (ISD::isBuildVectorAllZeros(Op.getNode())) {
    SDValue Cst = DAG.getTargetConstant(0, MVT::i1);
    SmallVector<SDValue, 16> Ops(VT.getVectorNumElements(), Cst);
    return DAG.getNode(ISD::BUILD_VECTOR, dl, VT, Ops);
  }

  if (ISD::isBuildVectorAllOnes(Op.getNode())) {
    SDValue Cst = DAG.getTargetConstant(1, MVT::i1);
    SmallVector<SDValue, 16> Ops(VT.getVectorNumElements(), Cst);
    return DAG.getNode(ISD::BUILD_VECTOR, dl, VT, Ops);
  }

  bool AllContants = true;
  uint64_t Immediate = 0;
  int NonConstIdx = -1;
  bool IsSplat = true;
  unsigned NumNonConsts = 0;
  unsigned NumConsts = 0;
  for (unsigned idx = 0, e = Op.getNumOperands(); idx < e; ++idx) {
    SDValue In = Op.getOperand(idx);
    if (In.getOpcode() == ISD::UNDEF)
      continue;
    if (!isa<ConstantSDNode>(In)) {
      AllContants = false;
      NonConstIdx = idx;
      NumNonConsts++;
    }
    else {
      NumConsts++;
      if (cast<ConstantSDNode>(In)->getZExtValue())
      Immediate |= (1ULL << idx);
    }
    if (In != Op.getOperand(0))
      IsSplat = false;
  }

  if (AllContants) {
    SDValue FullMask = DAG.getNode(ISD::BITCAST, dl, MVT::v16i1,
      DAG.getConstant(Immediate, MVT::i16));
    return DAG.getNode(ISD::EXTRACT_SUBVECTOR, dl, VT, FullMask,
                       DAG.getIntPtrConstant(0));
  }

  if (NumNonConsts == 1 && NonConstIdx != 0) {
    SDValue DstVec;
    if (NumConsts) {
      SDValue VecAsImm = DAG.getConstant(Immediate,
                                         MVT::getIntegerVT(VT.getSizeInBits()));
      DstVec = DAG.getNode(ISD::BITCAST, dl, VT, VecAsImm);
    }
    else 
      DstVec = DAG.getUNDEF(VT);
    return DAG.getNode(ISD::INSERT_VECTOR_ELT, dl, VT, DstVec,
                       Op.getOperand(NonConstIdx),
                       DAG.getIntPtrConstant(NonConstIdx));
  }
  if (!IsSplat && (NonConstIdx != 0))
    llvm_unreachable("Unsupported BUILD_VECTOR operation");
  MVT SelectVT = (VT == MVT::v16i1)? MVT::i16 : MVT::i8;
  SDValue Select;
  if (IsSplat)
    Select = DAG.getNode(ISD::SELECT, dl, SelectVT, Op.getOperand(0),
                          DAG.getConstant(-1, SelectVT),
                          DAG.getConstant(0, SelectVT));
  else
    Select = DAG.getNode(ISD::SELECT, dl, SelectVT, Op.getOperand(0),
                         DAG.getConstant((Immediate | 1), SelectVT),
                         DAG.getConstant(Immediate, SelectVT));
  return DAG.getNode(ISD::BITCAST, dl, VT, Select);
}

/// \brief Return true if \p N implements a horizontal binop and return the
/// operands for the horizontal binop into V0 and V1.
/// 
/// This is a helper function of PerformBUILD_VECTORCombine.
/// This function checks that the build_vector \p N in input implements a
/// horizontal operation. Parameter \p Opcode defines the kind of horizontal
/// operation to match.
/// For example, if \p Opcode is equal to ISD::ADD, then this function
/// checks if \p N implements a horizontal arithmetic add; if instead \p Opcode
/// is equal to ISD::SUB, then this function checks if this is a horizontal
/// arithmetic sub.
///
/// This function only analyzes elements of \p N whose indices are
/// in range [BaseIdx, LastIdx).
static bool isHorizontalBinOp(const BuildVectorSDNode *N, unsigned Opcode,
                              SelectionDAG &DAG,
                              unsigned BaseIdx, unsigned LastIdx,
                              SDValue &V0, SDValue &V1) {
  EVT VT = N->getValueType(0);

  assert(BaseIdx * 2 <= LastIdx && "Invalid Indices in input!");
  assert(VT.isVector() && VT.getVectorNumElements() >= LastIdx &&
         "Invalid Vector in input!");
  
  bool IsCommutable = (Opcode == ISD::ADD || Opcode == ISD::FADD);
  bool CanFold = true;
  unsigned ExpectedVExtractIdx = BaseIdx;
  unsigned NumElts = LastIdx - BaseIdx;
  V0 = DAG.getUNDEF(VT);
  V1 = DAG.getUNDEF(VT);

  // Check if N implements a horizontal binop.
  for (unsigned i = 0, e = NumElts; i != e && CanFold; ++i) {
    SDValue Op = N->getOperand(i + BaseIdx);

    // Skip UNDEFs.
    if (Op->getOpcode() == ISD::UNDEF) {
      // Update the expected vector extract index.
      if (i * 2 == NumElts)
        ExpectedVExtractIdx = BaseIdx;
      ExpectedVExtractIdx += 2;
      continue;
    }

    CanFold = Op->getOpcode() == Opcode && Op->hasOneUse();

    if (!CanFold)
      break;

    SDValue Op0 = Op.getOperand(0);
    SDValue Op1 = Op.getOperand(1);

    // Try to match the following pattern:
    // (BINOP (extract_vector_elt A, I), (extract_vector_elt A, I+1))
    CanFold = (Op0.getOpcode() == ISD::EXTRACT_VECTOR_ELT &&
        Op1.getOpcode() == ISD::EXTRACT_VECTOR_ELT &&
        Op0.getOperand(0) == Op1.getOperand(0) &&
        isa<ConstantSDNode>(Op0.getOperand(1)) &&
        isa<ConstantSDNode>(Op1.getOperand(1)));
    if (!CanFold)
      break;

    unsigned I0 = cast<ConstantSDNode>(Op0.getOperand(1))->getZExtValue();
    unsigned I1 = cast<ConstantSDNode>(Op1.getOperand(1))->getZExtValue();

    if (i * 2 < NumElts) {
      if (V0.getOpcode() == ISD::UNDEF)
        V0 = Op0.getOperand(0);
    } else {
      if (V1.getOpcode() == ISD::UNDEF)
        V1 = Op0.getOperand(0);
      if (i * 2 == NumElts)
        ExpectedVExtractIdx = BaseIdx;
    }

    SDValue Expected = (i * 2 < NumElts) ? V0 : V1;
    if (I0 == ExpectedVExtractIdx)
      CanFold = I1 == I0 + 1 && Op0.getOperand(0) == Expected;
    else if (IsCommutable && I1 == ExpectedVExtractIdx) {
      // Try to match the following dag sequence:
      // (BINOP (extract_vector_elt A, I+1), (extract_vector_elt A, I))
      CanFold = I0 == I1 + 1 && Op1.getOperand(0) == Expected;
    } else
      CanFold = false;

    ExpectedVExtractIdx += 2;
  }

  return CanFold;
}

/// \brief Emit a sequence of two 128-bit horizontal add/sub followed by
/// a concat_vector. 
///
/// This is a helper function of PerformBUILD_VECTORCombine.
/// This function expects two 256-bit vectors called V0 and V1.
/// At first, each vector is split into two separate 128-bit vectors.
/// Then, the resulting 128-bit vectors are used to implement two
/// horizontal binary operations. 
///
/// The kind of horizontal binary operation is defined by \p X86Opcode.
///
/// \p Mode specifies how the 128-bit parts of V0 and V1 are passed in input to
/// the two new horizontal binop.
/// When Mode is set, the first horizontal binop dag node would take as input
/// the lower 128-bit of V0 and the upper 128-bit of V0. The second
/// horizontal binop dag node would take as input the lower 128-bit of V1
/// and the upper 128-bit of V1.
///   Example:
///     HADD V0_LO, V0_HI
///     HADD V1_LO, V1_HI
///
/// Otherwise, the first horizontal binop dag node takes as input the lower
/// 128-bit of V0 and the lower 128-bit of V1, and the second horizontal binop
/// dag node takes the the upper 128-bit of V0 and the upper 128-bit of V1.
///   Example:
///     HADD V0_LO, V1_LO
///     HADD V0_HI, V1_HI
///
/// If \p isUndefLO is set, then the algorithm propagates UNDEF to the lower
/// 128-bits of the result. If \p isUndefHI is set, then UNDEF is propagated to
/// the upper 128-bits of the result.
static SDValue ExpandHorizontalBinOp(const SDValue &V0, const SDValue &V1,
                                     SDLoc DL, SelectionDAG &DAG,
                                     unsigned X86Opcode, bool Mode,
                                     bool isUndefLO, bool isUndefHI) {
  EVT VT = V0.getValueType();
  assert(VT.is256BitVector() && VT == V1.getValueType() &&
         "Invalid nodes in input!");

  unsigned NumElts = VT.getVectorNumElements();
  SDValue V0_LO = Extract128BitVector(V0, 0, DAG, DL);
  SDValue V0_HI = Extract128BitVector(V0, NumElts/2, DAG, DL);
  SDValue V1_LO = Extract128BitVector(V1, 0, DAG, DL);
  SDValue V1_HI = Extract128BitVector(V1, NumElts/2, DAG, DL);
  EVT NewVT = V0_LO.getValueType();

  SDValue LO = DAG.getUNDEF(NewVT);
  SDValue HI = DAG.getUNDEF(NewVT);

  if (Mode) {
    // Don't emit a horizontal binop if the result is expected to be UNDEF.
    if (!isUndefLO && V0->getOpcode() != ISD::UNDEF)
      LO = DAG.getNode(X86Opcode, DL, NewVT, V0_LO, V0_HI);
    if (!isUndefHI && V1->getOpcode() != ISD::UNDEF)
      HI = DAG.getNode(X86Opcode, DL, NewVT, V1_LO, V1_HI);
  } else {
    // Don't emit a horizontal binop if the result is expected to be UNDEF.
    if (!isUndefLO && (V0_LO->getOpcode() != ISD::UNDEF ||
                       V1_LO->getOpcode() != ISD::UNDEF))
      LO = DAG.getNode(X86Opcode, DL, NewVT, V0_LO, V1_LO);

    if (!isUndefHI && (V0_HI->getOpcode() != ISD::UNDEF ||
                       V1_HI->getOpcode() != ISD::UNDEF))
      HI = DAG.getNode(X86Opcode, DL, NewVT, V0_HI, V1_HI);
  }

  return DAG.getNode(ISD::CONCAT_VECTORS, DL, VT, LO, HI);
}

/// \brief Try to fold a build_vector that performs an 'addsub' into the
/// sequence of 'vadd + vsub + blendi'.
static SDValue matchAddSub(const BuildVectorSDNode *BV, SelectionDAG &DAG,
                           const X86Subtarget *Subtarget) {
  SDLoc DL(BV);
  EVT VT = BV->getValueType(0);
  unsigned NumElts = VT.getVectorNumElements();
  SDValue InVec0 = DAG.getUNDEF(VT);
  SDValue InVec1 = DAG.getUNDEF(VT);

  assert((VT == MVT::v8f32 || VT == MVT::v4f64 || VT == MVT::v4f32 ||
          VT == MVT::v2f64) && "build_vector with an invalid type found!");

  // Don't try to emit a VSELECT that cannot be lowered into a blend.
  const TargetLowering &TLI = DAG.getTargetLoweringInfo();
  if (!TLI.isOperationLegalOrCustom(ISD::VSELECT, VT))
    return SDValue();

  // Odd-numbered elements in the input build vector are obtained from
  // adding two integer/float elements.
  // Even-numbered elements in the input build vector are obtained from
  // subtracting two integer/float elements.
  unsigned ExpectedOpcode = ISD::FSUB;
  unsigned NextExpectedOpcode = ISD::FADD;
  bool AddFound = false;
  bool SubFound = false;

  for (unsigned i = 0, e = NumElts; i != e; i++) {
    SDValue Op = BV->getOperand(i);
      
    // Skip 'undef' values.
    unsigned Opcode = Op.getOpcode();
    if (Opcode == ISD::UNDEF) {
      std::swap(ExpectedOpcode, NextExpectedOpcode);
      continue;
    }
      
    // Early exit if we found an unexpected opcode.
    if (Opcode != ExpectedOpcode)
      return SDValue();

    SDValue Op0 = Op.getOperand(0);
    SDValue Op1 = Op.getOperand(1);

    // Try to match the following pattern:
    // (BINOP (extract_vector_elt A, i), (extract_vector_elt B, i))
    // Early exit if we cannot match that sequence.
    if (Op0.getOpcode() != ISD::EXTRACT_VECTOR_ELT ||
        Op1.getOpcode() != ISD::EXTRACT_VECTOR_ELT ||
        !isa<ConstantSDNode>(Op0.getOperand(1)) ||
        !isa<ConstantSDNode>(Op1.getOperand(1)) ||
        Op0.getOperand(1) != Op1.getOperand(1))
      return SDValue();

    unsigned I0 = cast<ConstantSDNode>(Op0.getOperand(1))->getZExtValue();
    if (I0 != i)
      return SDValue();

    // We found a valid add/sub node. Update the information accordingly.
    if (i & 1)
      AddFound = true;
    else
      SubFound = true;

    // Update InVec0 and InVec1.
    if (InVec0.getOpcode() == ISD::UNDEF)
      InVec0 = Op0.getOperand(0);
    if (InVec1.getOpcode() == ISD::UNDEF)
      InVec1 = Op1.getOperand(0);

    // Make sure that operands in input to each add/sub node always
    // come from a same pair of vectors.
    if (InVec0 != Op0.getOperand(0)) {
      if (ExpectedOpcode == ISD::FSUB)
        return SDValue();

      // FADD is commutable. Try to commute the operands
      // and then test again.
      std::swap(Op0, Op1);
      if (InVec0 != Op0.getOperand(0))
        return SDValue();
    }

    if (InVec1 != Op1.getOperand(0))
      return SDValue();

    // Update the pair of expected opcodes.
    std::swap(ExpectedOpcode, NextExpectedOpcode);
  }

  // Don't try to fold this build_vector into a VSELECT if it has
  // too many UNDEF operands.
  if (AddFound && SubFound && InVec0.getOpcode() != ISD::UNDEF &&
      InVec1.getOpcode() != ISD::UNDEF) {
    // Emit a sequence of vector add and sub followed by a VSELECT.
    // The new VSELECT will be lowered into a BLENDI.
    // At ISel stage, we pattern-match the sequence 'add + sub + BLENDI'
    // and emit a single ADDSUB instruction.
    SDValue Sub = DAG.getNode(ExpectedOpcode, DL, VT, InVec0, InVec1);
    SDValue Add = DAG.getNode(NextExpectedOpcode, DL, VT, InVec0, InVec1);

    // Construct the VSELECT mask.
    EVT MaskVT = VT.changeVectorElementTypeToInteger();
    EVT SVT = MaskVT.getVectorElementType();
    unsigned SVTBits = SVT.getSizeInBits();
    SmallVector<SDValue, 8> Ops;

    for (unsigned i = 0, e = NumElts; i != e; ++i) {
      APInt Value = i & 1 ? APInt::getNullValue(SVTBits) :
                            APInt::getAllOnesValue(SVTBits);
      SDValue Constant = DAG.getConstant(Value, SVT);
      Ops.push_back(Constant);
    }

    SDValue Mask = DAG.getNode(ISD::BUILD_VECTOR, DL, MaskVT, Ops);
    return DAG.getSelect(DL, VT, Mask, Sub, Add);
  }
  
  return SDValue();
}

static SDValue PerformBUILD_VECTORCombine(SDNode *N, SelectionDAG &DAG,
                                          const X86Subtarget *Subtarget) {
  SDLoc DL(N);
  EVT VT = N->getValueType(0);
  unsigned NumElts = VT.getVectorNumElements();
  BuildVectorSDNode *BV = cast<BuildVectorSDNode>(N);
  SDValue InVec0, InVec1;

  // Try to match an ADDSUB.
  if ((Subtarget->hasSSE3() && (VT == MVT::v4f32 || VT == MVT::v2f64)) ||
      (Subtarget->hasAVX() && (VT == MVT::v8f32 || VT == MVT::v4f64))) {
    SDValue Value = matchAddSub(BV, DAG, Subtarget);
    if (Value.getNode())
      return Value;
  }

  // Try to match horizontal ADD/SUB.
  unsigned NumUndefsLO = 0;
  unsigned NumUndefsHI = 0;
  unsigned Half = NumElts/2;

  // Count the number of UNDEF operands in the build_vector in input.
  for (unsigned i = 0, e = Half; i != e; ++i)
    if (BV->getOperand(i)->getOpcode() == ISD::UNDEF)
      NumUndefsLO++;

  for (unsigned i = Half, e = NumElts; i != e; ++i)
    if (BV->getOperand(i)->getOpcode() == ISD::UNDEF)
      NumUndefsHI++;

  // Early exit if this is either a build_vector of all UNDEFs or all the
  // operands but one are UNDEF.
  if (NumUndefsLO + NumUndefsHI + 1 >= NumElts)
    return SDValue();

  if ((VT == MVT::v4f32 || VT == MVT::v2f64) && Subtarget->hasSSE3()) {
    // Try to match an SSE3 float HADD/HSUB.
    if (isHorizontalBinOp(BV, ISD::FADD, DAG, 0, NumElts, InVec0, InVec1))
      return DAG.getNode(X86ISD::FHADD, DL, VT, InVec0, InVec1);
    
    if (isHorizontalBinOp(BV, ISD::FSUB, DAG, 0, NumElts, InVec0, InVec1))
      return DAG.getNode(X86ISD::FHSUB, DL, VT, InVec0, InVec1);
  } else if ((VT == MVT::v4i32 || VT == MVT::v8i16) && Subtarget->hasSSSE3()) {
    // Try to match an SSSE3 integer HADD/HSUB.
    if (isHorizontalBinOp(BV, ISD::ADD, DAG, 0, NumElts, InVec0, InVec1))
      return DAG.getNode(X86ISD::HADD, DL, VT, InVec0, InVec1);
    
    if (isHorizontalBinOp(BV, ISD::SUB, DAG, 0, NumElts, InVec0, InVec1))
      return DAG.getNode(X86ISD::HSUB, DL, VT, InVec0, InVec1);
  }
  
  if (!Subtarget->hasAVX())
    return SDValue();

  if ((VT == MVT::v8f32 || VT == MVT::v4f64)) {
    // Try to match an AVX horizontal add/sub of packed single/double
    // precision floating point values from 256-bit vectors.
    SDValue InVec2, InVec3;
    if (isHorizontalBinOp(BV, ISD::FADD, DAG, 0, Half, InVec0, InVec1) &&
        isHorizontalBinOp(BV, ISD::FADD, DAG, Half, NumElts, InVec2, InVec3) &&
        ((InVec0.getOpcode() == ISD::UNDEF ||
          InVec2.getOpcode() == ISD::UNDEF) || InVec0 == InVec2) &&
        ((InVec1.getOpcode() == ISD::UNDEF ||
          InVec3.getOpcode() == ISD::UNDEF) || InVec1 == InVec3))
      return DAG.getNode(X86ISD::FHADD, DL, VT, InVec0, InVec1);

    if (isHorizontalBinOp(BV, ISD::FSUB, DAG, 0, Half, InVec0, InVec1) &&
        isHorizontalBinOp(BV, ISD::FSUB, DAG, Half, NumElts, InVec2, InVec3) &&
        ((InVec0.getOpcode() == ISD::UNDEF ||
          InVec2.getOpcode() == ISD::UNDEF) || InVec0 == InVec2) &&
        ((InVec1.getOpcode() == ISD::UNDEF ||
          InVec3.getOpcode() == ISD::UNDEF) || InVec1 == InVec3))
      return DAG.getNode(X86ISD::FHSUB, DL, VT, InVec0, InVec1);
  } else if (VT == MVT::v8i32 || VT == MVT::v16i16) {
    // Try to match an AVX2 horizontal add/sub of signed integers.
    SDValue InVec2, InVec3;
    unsigned X86Opcode;
    bool CanFold = true;

    if (isHorizontalBinOp(BV, ISD::ADD, DAG, 0, Half, InVec0, InVec1) &&
        isHorizontalBinOp(BV, ISD::ADD, DAG, Half, NumElts, InVec2, InVec3) &&
        ((InVec0.getOpcode() == ISD::UNDEF ||
          InVec2.getOpcode() == ISD::UNDEF) || InVec0 == InVec2) &&
        ((InVec1.getOpcode() == ISD::UNDEF ||
          InVec3.getOpcode() == ISD::UNDEF) || InVec1 == InVec3))
      X86Opcode = X86ISD::HADD;
    else if (isHorizontalBinOp(BV, ISD::SUB, DAG, 0, Half, InVec0, InVec1) &&
        isHorizontalBinOp(BV, ISD::SUB, DAG, Half, NumElts, InVec2, InVec3) &&
        ((InVec0.getOpcode() == ISD::UNDEF ||
          InVec2.getOpcode() == ISD::UNDEF) || InVec0 == InVec2) &&
        ((InVec1.getOpcode() == ISD::UNDEF ||
          InVec3.getOpcode() == ISD::UNDEF) || InVec1 == InVec3))
      X86Opcode = X86ISD::HSUB;
    else
      CanFold = false;

    if (CanFold) {
      // Fold this build_vector into a single horizontal add/sub.
      // Do this only if the target has AVX2.
      if (Subtarget->hasAVX2())
        return DAG.getNode(X86Opcode, DL, VT, InVec0, InVec1);
 
      // Do not try to expand this build_vector into a pair of horizontal
      // add/sub if we can emit a pair of scalar add/sub.
      if (NumUndefsLO + 1 == Half || NumUndefsHI + 1 == Half)
        return SDValue();

      // Convert this build_vector into a pair of horizontal binop followed by
      // a concat vector.
      bool isUndefLO = NumUndefsLO == Half;
      bool isUndefHI = NumUndefsHI == Half;
      return ExpandHorizontalBinOp(InVec0, InVec1, DL, DAG, X86Opcode, false,
                                   isUndefLO, isUndefHI);
    }
  }

  if ((VT == MVT::v8f32 || VT == MVT::v4f64 || VT == MVT::v8i32 ||
       VT == MVT::v16i16) && Subtarget->hasAVX()) {
    unsigned X86Opcode;
    if (isHorizontalBinOp(BV, ISD::ADD, DAG, 0, NumElts, InVec0, InVec1))
      X86Opcode = X86ISD::HADD;
    else if (isHorizontalBinOp(BV, ISD::SUB, DAG, 0, NumElts, InVec0, InVec1))
      X86Opcode = X86ISD::HSUB;
    else if (isHorizontalBinOp(BV, ISD::FADD, DAG, 0, NumElts, InVec0, InVec1))
      X86Opcode = X86ISD::FHADD;
    else if (isHorizontalBinOp(BV, ISD::FSUB, DAG, 0, NumElts, InVec0, InVec1))
      X86Opcode = X86ISD::FHSUB;
    else
      return SDValue();

    // Don't try to expand this build_vector into a pair of horizontal add/sub
    // if we can simply emit a pair of scalar add/sub.
    if (NumUndefsLO + 1 == Half || NumUndefsHI + 1 == Half)
      return SDValue();

    // Convert this build_vector into two horizontal add/sub followed by
    // a concat vector.
    bool isUndefLO = NumUndefsLO == Half;
    bool isUndefHI = NumUndefsHI == Half;
    return ExpandHorizontalBinOp(InVec0, InVec1, DL, DAG, X86Opcode, true,
                                 isUndefLO, isUndefHI);
  }

  return SDValue();
}

SDValue
X86TargetLowering::LowerBUILD_VECTOR(SDValue Op, SelectionDAG &DAG) const {
  SDLoc dl(Op);

  MVT VT = Op.getSimpleValueType();
  MVT ExtVT = VT.getVectorElementType();
  unsigned NumElems = Op.getNumOperands();

  // Generate vectors for predicate vectors.
  if (VT.getScalarType() == MVT::i1 && Subtarget->hasAVX512())
    return LowerBUILD_VECTORvXi1(Op, DAG);

  // Vectors containing all zeros can be matched by pxor and xorps later
  if (ISD::isBuildVectorAllZeros(Op.getNode())) {
    // Canonicalize this to <4 x i32> to 1) ensure the zero vectors are CSE'd
    // and 2) ensure that i64 scalars are eliminated on x86-32 hosts.
    if (VT == MVT::v4i32 || VT == MVT::v8i32 || VT == MVT::v16i32)
      return Op;

    return getZeroVector(VT, Subtarget, DAG, dl);
  }

  // Vectors containing all ones can be matched by pcmpeqd on 128-bit width
  // vectors or broken into v4i32 operations on 256-bit vectors. AVX2 can use
  // vpcmpeqd on 256-bit vectors.
  if (Subtarget->hasSSE2() && ISD::isBuildVectorAllOnes(Op.getNode())) {
    if (VT == MVT::v4i32 || (VT == MVT::v8i32 && Subtarget->hasInt256()))
      return Op;

    if (!VT.is512BitVector())
      return getOnesVector(VT, Subtarget->hasInt256(), DAG, dl);
  }

  SDValue Broadcast = LowerVectorBroadcast(Op, Subtarget, DAG);
  if (Broadcast.getNode())
    return Broadcast;

  unsigned EVTBits = ExtVT.getSizeInBits();

  unsigned NumZero  = 0;
  unsigned NumNonZero = 0;
  unsigned NonZeros = 0;
  bool IsAllConstants = true;
  SmallSet<SDValue, 8> Values;
  for (unsigned i = 0; i < NumElems; ++i) {
    SDValue Elt = Op.getOperand(i);
    if (Elt.getOpcode() == ISD::UNDEF)
      continue;
    Values.insert(Elt);
    if (Elt.getOpcode() != ISD::Constant &&
        Elt.getOpcode() != ISD::ConstantFP)
      IsAllConstants = false;
    if (X86::isZeroNode(Elt))
      NumZero++;
    else {
      NonZeros |= (1 << i);
      NumNonZero++;
    }
  }

  // All undef vector. Return an UNDEF.  All zero vectors were handled above.
  if (NumNonZero == 0)
    return DAG.getUNDEF(VT);

  // Special case for single non-zero, non-undef, element.
  if (NumNonZero == 1) {
    unsigned Idx = countTrailingZeros(NonZeros);
    SDValue Item = Op.getOperand(Idx);

    // If this is an insertion of an i64 value on x86-32, and if the top bits of
    // the value are obviously zero, truncate the value to i32 and do the
    // insertion that way.  Only do this if the value is non-constant or if the
    // value is a constant being inserted into element 0.  It is cheaper to do
    // a constant pool load than it is to do a movd + shuffle.
    if (ExtVT == MVT::i64 && !Subtarget->is64Bit() &&
        (!IsAllConstants || Idx == 0)) {
      if (DAG.MaskedValueIsZero(Item, APInt::getBitsSet(64, 32, 64))) {
        // Handle SSE only.
        assert(VT == MVT::v2i64 && "Expected an SSE value type!");
        EVT VecVT = MVT::v4i32;
        unsigned VecElts = 4;

        // Truncate the value (which may itself be a constant) to i32, and
        // convert it to a vector with movd (S2V+shuffle to zero extend).
        Item = DAG.getNode(ISD::TRUNCATE, dl, MVT::i32, Item);
        Item = DAG.getNode(ISD::SCALAR_TO_VECTOR, dl, VecVT, Item);
        Item = getShuffleVectorZeroOrUndef(Item, 0, true, Subtarget, DAG);

        // Now we have our 32-bit value zero extended in the low element of
        // a vector.  If Idx != 0, swizzle it into place.
        if (Idx != 0) {
          SmallVector<int, 4> Mask;
          Mask.push_back(Idx);
          for (unsigned i = 1; i != VecElts; ++i)
            Mask.push_back(i);
          Item = DAG.getVectorShuffle(VecVT, dl, Item, DAG.getUNDEF(VecVT),
                                      &Mask[0]);
        }
        return DAG.getNode(ISD::BITCAST, dl, VT, Item);
      }
    }

    // If we have a constant or non-constant insertion into the low element of
    // a vector, we can do this with SCALAR_TO_VECTOR + shuffle of zero into
    // the rest of the elements.  This will be matched as movd/movq/movss/movsd
    // depending on what the source datatype is.
    if (Idx == 0) {
      if (NumZero == 0)
        return DAG.getNode(ISD::SCALAR_TO_VECTOR, dl, VT, Item);

      if (ExtVT == MVT::i32 || ExtVT == MVT::f32 || ExtVT == MVT::f64 ||
          (ExtVT == MVT::i64 && Subtarget->is64Bit())) {
        if (VT.is256BitVector() || VT.is512BitVector()) {
          SDValue ZeroVec = getZeroVector(VT, Subtarget, DAG, dl);
          return DAG.getNode(ISD::INSERT_VECTOR_ELT, dl, VT, ZeroVec,
                             Item, DAG.getIntPtrConstant(0));
        }
        assert(VT.is128BitVector() && "Expected an SSE value type!");
        Item = DAG.getNode(ISD::SCALAR_TO_VECTOR, dl, VT, Item);
        // Turn it into a MOVL (i.e. movss, movsd, or movd) to a zero vector.
        return getShuffleVectorZeroOrUndef(Item, 0, true, Subtarget, DAG);
      }

      if (ExtVT == MVT::i16 || ExtVT == MVT::i8) {
        Item = DAG.getNode(ISD::ZERO_EXTEND, dl, MVT::i32, Item);
        Item = DAG.getNode(ISD::SCALAR_TO_VECTOR, dl, MVT::v4i32, Item);
        if (VT.is256BitVector()) {
          SDValue ZeroVec = getZeroVector(MVT::v8i32, Subtarget, DAG, dl);
          Item = Insert128BitVector(ZeroVec, Item, 0, DAG, dl);
        } else {
          assert(VT.is128BitVector() && "Expected an SSE value type!");
          Item = getShuffleVectorZeroOrUndef(Item, 0, true, Subtarget, DAG);
        }
        return DAG.getNode(ISD::BITCAST, dl, VT, Item);
      }
    }

    // Is it a vector logical left shift?
    if (NumElems == 2 && Idx == 1 &&
        X86::isZeroNode(Op.getOperand(0)) &&
        !X86::isZeroNode(Op.getOperand(1))) {
      unsigned NumBits = VT.getSizeInBits();
      return getVShift(true, VT,
                       DAG.getNode(ISD::SCALAR_TO_VECTOR, dl,
                                   VT, Op.getOperand(1)),
                       NumBits/2, DAG, *this, dl);
    }

    if (IsAllConstants) // Otherwise, it's better to do a constpool load.
      return SDValue();

    // Otherwise, if this is a vector with i32 or f32 elements, and the element
    // is a non-constant being inserted into an element other than the low one,
    // we can't use a constant pool load.  Instead, use SCALAR_TO_VECTOR (aka
    // movd/movss) to move this into the low element, then shuffle it into
    // place.
    if (EVTBits == 32) {
      Item = DAG.getNode(ISD::SCALAR_TO_VECTOR, dl, VT, Item);

      // Turn it into a shuffle of zero and zero-extended scalar to vector.
      Item = getShuffleVectorZeroOrUndef(Item, 0, NumZero > 0, Subtarget, DAG);
      SmallVector<int, 8> MaskVec;
      for (unsigned i = 0; i != NumElems; ++i)
        MaskVec.push_back(i == Idx ? 0 : 1);
      return DAG.getVectorShuffle(VT, dl, Item, DAG.getUNDEF(VT), &MaskVec[0]);
    }
  }

  // Splat is obviously ok. Let legalizer expand it to a shuffle.
  if (Values.size() == 1) {
    if (EVTBits == 32) {
      // Instead of a shuffle like this:
      // shuffle (scalar_to_vector (load (ptr + 4))), undef, <0, 0, 0, 0>
      // Check if it's possible to issue this instead.
      // shuffle (vload ptr)), undef, <1, 1, 1, 1>
      unsigned Idx = countTrailingZeros(NonZeros);
      SDValue Item = Op.getOperand(Idx);
      if (Op.getNode()->isOnlyUserOf(Item.getNode()))
        return LowerAsSplatVectorLoad(Item, VT, dl, DAG);
    }
    return SDValue();
  }

  // A vector full of immediates; various special cases are already
  // handled, so this is best done with a single constant-pool load.
  if (IsAllConstants)
    return SDValue();

  // For AVX-length vectors, build the individual 128-bit pieces and use
  // shuffles to put them in place.
  if (VT.is256BitVector() || VT.is512BitVector()) {
    SmallVector<SDValue, 64> V;
    for (unsigned i = 0; i != NumElems; ++i)
      V.push_back(Op.getOperand(i));

    EVT HVT = EVT::getVectorVT(*DAG.getContext(), ExtVT, NumElems/2);

    // Build both the lower and upper subvector.
    SDValue Lower = DAG.getNode(ISD::BUILD_VECTOR, dl, HVT,
                                makeArrayRef(&V[0], NumElems/2));
    SDValue Upper = DAG.getNode(ISD::BUILD_VECTOR, dl, HVT,
                                makeArrayRef(&V[NumElems / 2], NumElems/2));

    // Recreate the wider vector with the lower and upper part.
    if (VT.is256BitVector())
      return Concat128BitVectors(Lower, Upper, VT, NumElems, DAG, dl);
    return Concat256BitVectors(Lower, Upper, VT, NumElems, DAG, dl);
  }

  // Let legalizer expand 2-wide build_vectors.
  if (EVTBits == 64) {
    if (NumNonZero == 1) {
      // One half is zero or undef.
      unsigned Idx = countTrailingZeros(NonZeros);
      SDValue V2 = DAG.getNode(ISD::SCALAR_TO_VECTOR, dl, VT,
                                 Op.getOperand(Idx));
      return getShuffleVectorZeroOrUndef(V2, Idx, true, Subtarget, DAG);
    }
    return SDValue();
  }

  // If element VT is < 32 bits, convert it to inserts into a zero vector.
  if (EVTBits == 8 && NumElems == 16) {
    SDValue V = LowerBuildVectorv16i8(Op, NonZeros,NumNonZero,NumZero, DAG,
                                        Subtarget, *this);
    if (V.getNode()) return V;
  }

  if (EVTBits == 16 && NumElems == 8) {
    SDValue V = LowerBuildVectorv8i16(Op, NonZeros,NumNonZero,NumZero, DAG,
                                      Subtarget, *this);
    if (V.getNode()) return V;
  }

  // If element VT is == 32 bits and has 4 elems, try to generate an INSERTPS
  if (EVTBits == 32 && NumElems == 4) {
    SDValue V = LowerBuildVectorv4x32(Op, NumElems, NonZeros, NumNonZero,
                                      NumZero, DAG, Subtarget, *this);
    if (V.getNode())
      return V;
  }

  // If element VT is == 32 bits, turn it into a number of shuffles.
  SmallVector<SDValue, 8> V(NumElems);
  if (NumElems == 4 && NumZero > 0) {
    for (unsigned i = 0; i < 4; ++i) {
      bool isZero = !(NonZeros & (1 << i));
      if (isZero)
        V[i] = getZeroVector(VT, Subtarget, DAG, dl);
      else
        V[i] = DAG.getNode(ISD::SCALAR_TO_VECTOR, dl, VT, Op.getOperand(i));
    }

    for (unsigned i = 0; i < 2; ++i) {
      switch ((NonZeros & (0x3 << i*2)) >> (i*2)) {
        default: break;
        case 0:
          V[i] = V[i*2];  // Must be a zero vector.
          break;
        case 1:
          V[i] = getMOVL(DAG, dl, VT, V[i*2+1], V[i*2]);
          break;
        case 2:
          V[i] = getMOVL(DAG, dl, VT, V[i*2], V[i*2+1]);
          break;
        case 3:
          V[i] = getUnpackl(DAG, dl, VT, V[i*2], V[i*2+1]);
          break;
      }
    }

    bool Reverse1 = (NonZeros & 0x3) == 2;
    bool Reverse2 = ((NonZeros & (0x3 << 2)) >> 2) == 2;
    int MaskVec[] = {
      Reverse1 ? 1 : 0,
      Reverse1 ? 0 : 1,
      static_cast<int>(Reverse2 ? NumElems+1 : NumElems),
      static_cast<int>(Reverse2 ? NumElems   : NumElems+1)
    };
    return DAG.getVectorShuffle(VT, dl, V[0], V[1], &MaskVec[0]);
  }

  if (Values.size() > 1 && VT.is128BitVector()) {
    // Check for a build vector of consecutive loads.
    for (unsigned i = 0; i < NumElems; ++i)
      V[i] = Op.getOperand(i);

    // Check for elements which are consecutive loads.
    SDValue LD = EltsFromConsecutiveLoads(VT, V, dl, DAG, false);
    if (LD.getNode())
      return LD;

    // Check for a build vector from mostly shuffle plus few inserting.
    SDValue Sh = buildFromShuffleMostly(Op, DAG);
    if (Sh.getNode())
      return Sh;

    // For SSE 4.1, use insertps to put the high elements into the low element.
    if (getSubtarget()->hasSSE41()) {
      SDValue Result;
      if (Op.getOperand(0).getOpcode() != ISD::UNDEF)
        Result = DAG.getNode(ISD::SCALAR_TO_VECTOR, dl, VT, Op.getOperand(0));
      else
        Result = DAG.getUNDEF(VT);

      for (unsigned i = 1; i < NumElems; ++i) {
        if (Op.getOperand(i).getOpcode() == ISD::UNDEF) continue;
        Result = DAG.getNode(ISD::INSERT_VECTOR_ELT, dl, VT, Result,
                             Op.getOperand(i), DAG.getIntPtrConstant(i));
      }
      return Result;
    }

    // Otherwise, expand into a number of unpckl*, start by extending each of
    // our (non-undef) elements to the full vector width with the element in the
    // bottom slot of the vector (which generates no code for SSE).
    for (unsigned i = 0; i < NumElems; ++i) {
      if (Op.getOperand(i).getOpcode() != ISD::UNDEF)
        V[i] = DAG.getNode(ISD::SCALAR_TO_VECTOR, dl, VT, Op.getOperand(i));
      else
        V[i] = DAG.getUNDEF(VT);
    }

    // Next, we iteratively mix elements, e.g. for v4f32:
    //   Step 1: unpcklps 0, 2 ==> X: <?, ?, 2, 0>
    //         : unpcklps 1, 3 ==> Y: <?, ?, 3, 1>
    //   Step 2: unpcklps X, Y ==>    <3, 2, 1, 0>
    unsigned EltStride = NumElems >> 1;
    while (EltStride != 0) {
      for (unsigned i = 0; i < EltStride; ++i) {
        // If V[i+EltStride] is undef and this is the first round of mixing,
        // then it is safe to just drop this shuffle: V[i] is already in the
        // right place, the one element (since it's the first round) being
        // inserted as undef can be dropped.  This isn't safe for successive
        // rounds because they will permute elements within both vectors.
        if (V[i+EltStride].getOpcode() == ISD::UNDEF &&
            EltStride == NumElems/2)
          continue;

        V[i] = getUnpackl(DAG, dl, VT, V[i], V[i + EltStride]);
      }
      EltStride >>= 1;
    }
    return V[0];
  }
  return SDValue();
}

// LowerAVXCONCAT_VECTORS - 256-bit AVX can use the vinsertf128 instruction
// to create 256-bit vectors from two other 128-bit ones.
static SDValue LowerAVXCONCAT_VECTORS(SDValue Op, SelectionDAG &DAG) {
  SDLoc dl(Op);
  MVT ResVT = Op.getSimpleValueType();

  assert((ResVT.is256BitVector() ||
          ResVT.is512BitVector()) && "Value type must be 256-/512-bit wide");

  SDValue V1 = Op.getOperand(0);
  SDValue V2 = Op.getOperand(1);
  unsigned NumElems = ResVT.getVectorNumElements();
  if(ResVT.is256BitVector())
    return Concat128BitVectors(V1, V2, ResVT, NumElems, DAG, dl);

  if (Op.getNumOperands() == 4) {
    MVT HalfVT = MVT::getVectorVT(ResVT.getScalarType(),
                                ResVT.getVectorNumElements()/2);
    SDValue V3 = Op.getOperand(2);
    SDValue V4 = Op.getOperand(3);
    return Concat256BitVectors(Concat128BitVectors(V1, V2, HalfVT, NumElems/2, DAG, dl),
      Concat128BitVectors(V3, V4, HalfVT, NumElems/2, DAG, dl), ResVT, NumElems, DAG, dl);
  }
  return Concat256BitVectors(V1, V2, ResVT, NumElems, DAG, dl);
}

static SDValue LowerCONCAT_VECTORS(SDValue Op, SelectionDAG &DAG) {
  MVT LLVM_ATTRIBUTE_UNUSED VT = Op.getSimpleValueType();
  assert((VT.is256BitVector() && Op.getNumOperands() == 2) ||
         (VT.is512BitVector() && (Op.getNumOperands() == 2 ||
          Op.getNumOperands() == 4)));

  // AVX can use the vinsertf128 instruction to create 256-bit vectors
  // from two other 128-bit ones.

  // 512-bit vector may contain 2 256-bit vectors or 4 128-bit vectors
  return LowerAVXCONCAT_VECTORS(Op, DAG);
}


//===----------------------------------------------------------------------===//
// Vector shuffle lowering
//
// This is an experimental code path for lowering vector shuffles on x86. It is
// designed to handle arbitrary vector shuffles and blends, gracefully
// degrading performance as necessary. It works hard to recognize idiomatic
// shuffles and lower them to optimal instruction patterns without leaving
// a framework that allows reasonably efficient handling of all vector shuffle
// patterns.
//===----------------------------------------------------------------------===//

/// \brief Tiny helper function to identify a no-op mask.
///
/// This is a somewhat boring predicate function. It checks whether the mask
/// array input, which is assumed to be a single-input shuffle mask of the kind
/// used by the X86 shuffle instructions (not a fully general
/// ShuffleVectorSDNode mask) requires any shuffles to occur. Both undef and an
/// in-place shuffle are 'no-op's.
static bool isNoopShuffleMask(ArrayRef<int> Mask) {
  for (int i = 0, Size = Mask.size(); i < Size; ++i)
    if (Mask[i] != -1 && Mask[i] != i)
      return false;
  return true;
}

/// \brief Helper function to classify a mask as a single-input mask.
///
/// This isn't a generic single-input test because in the vector shuffle
/// lowering we canonicalize single inputs to be the first input operand. This
/// means we can more quickly test for a single input by only checking whether
/// an input from the second operand exists. We also assume that the size of
/// mask corresponds to the size of the input vectors which isn't true in the
/// fully general case.
static bool isSingleInputShuffleMask(ArrayRef<int> Mask) {
  for (int M : Mask)
    if (M >= (int)Mask.size())
      return false;
  return true;
}

// Hide this symbol with an anonymous namespace instead of 'static' so that MSVC
// 2013 will allow us to use it as a non-type template parameter.
namespace {

/// \brief Implementation of the \c isShuffleEquivalent variadic functor.
///
/// See its documentation for details.
bool isShuffleEquivalentImpl(ArrayRef<int> Mask, ArrayRef<const int *> Args) {
  if (Mask.size() != Args.size())
    return false;
  for (int i = 0, e = Mask.size(); i < e; ++i) {
    assert(*Args[i] >= 0 && "Arguments must be positive integers!");
    assert(*Args[i] < (int)Args.size() * 2 &&
           "Argument outside the range of possible shuffle inputs!");
    if (Mask[i] != -1 && Mask[i] != *Args[i])
      return false;
  }
  return true;
}

} // namespace

/// \brief Checks whether a shuffle mask is equivalent to an explicit list of
/// arguments.
///
/// This is a fast way to test a shuffle mask against a fixed pattern:
///
///   if (isShuffleEquivalent(Mask, 3, 2, 1, 0)) { ... }
///
/// It returns true if the mask is exactly as wide as the argument list, and
/// each element of the mask is either -1 (signifying undef) or the value given
/// in the argument.
static const VariadicFunction1<
    bool, ArrayRef<int>, int, isShuffleEquivalentImpl> isShuffleEquivalent = {};

/// \brief Get a 4-lane 8-bit shuffle immediate for a mask.
///
/// This helper function produces an 8-bit shuffle immediate corresponding to
/// the ubiquitous shuffle encoding scheme used in x86 instructions for
/// shuffling 4 lanes. It can be used with most of the PSHUF instructions for
/// example.
///
/// NB: We rely heavily on "undef" masks preserving the input lane.
static SDValue getV4X86ShuffleImm8ForMask(ArrayRef<int> Mask,
                                          SelectionDAG &DAG) {
  assert(Mask.size() == 4 && "Only 4-lane shuffle masks");
  assert(Mask[0] >= -1 && Mask[0] < 4 && "Out of bound mask element!");
  assert(Mask[1] >= -1 && Mask[1] < 4 && "Out of bound mask element!");
  assert(Mask[2] >= -1 && Mask[2] < 4 && "Out of bound mask element!");
  assert(Mask[3] >= -1 && Mask[3] < 4 && "Out of bound mask element!");

  unsigned Imm = 0;
  Imm |= (Mask[0] == -1 ? 0 : Mask[0]) << 0;
  Imm |= (Mask[1] == -1 ? 1 : Mask[1]) << 2;
  Imm |= (Mask[2] == -1 ? 2 : Mask[2]) << 4;
  Imm |= (Mask[3] == -1 ? 3 : Mask[3]) << 6;
  return DAG.getConstant(Imm, MVT::i8);
}

/// \brief Handle lowering of 2-lane 64-bit floating point shuffles.
///
/// This is the basis function for the 2-lane 64-bit shuffles as we have full
/// support for floating point shuffles but not integer shuffles. These
/// instructions will incur a domain crossing penalty on some chips though so
/// it is better to avoid lowering through this for integer vectors where
/// possible.
static SDValue lowerV2F64VectorShuffle(SDValue Op, SDValue V1, SDValue V2,
                                       const X86Subtarget *Subtarget,
                                       SelectionDAG &DAG) {
  SDLoc DL(Op);
  assert(Op.getSimpleValueType() == MVT::v2f64 && "Bad shuffle type!");
  assert(V1.getSimpleValueType() == MVT::v2f64 && "Bad operand type!");
  assert(V2.getSimpleValueType() == MVT::v2f64 && "Bad operand type!");
  ShuffleVectorSDNode *SVOp = cast<ShuffleVectorSDNode>(Op);
  ArrayRef<int> Mask = SVOp->getMask();
  assert(Mask.size() == 2 && "Unexpected mask size for v2 shuffle!");

  if (isSingleInputShuffleMask(Mask)) {
    // Straight shuffle of a single input vector. Simulate this by using the
    // single input as both of the "inputs" to this instruction..
    unsigned SHUFPDMask = (Mask[0] == 1) | ((Mask[1] == 1) << 1);
    return DAG.getNode(X86ISD::SHUFP, SDLoc(Op), MVT::v2f64, V1, V1,
                       DAG.getConstant(SHUFPDMask, MVT::i8));
  }
  assert(Mask[0] >= 0 && Mask[0] < 2 && "Non-canonicalized blend!");
  assert(Mask[1] >= 2 && "Non-canonicalized blend!");

  // Use dedicated unpack instructions for masks that match their pattern.
  if (isShuffleEquivalent(Mask, 0, 2))
    return DAG.getNode(X86ISD::UNPCKL, DL, MVT::v2f64, V1, V2);
  if (isShuffleEquivalent(Mask, 1, 3))
    return DAG.getNode(X86ISD::UNPCKH, DL, MVT::v2f64, V1, V2);

  unsigned SHUFPDMask = (Mask[0] == 1) | (((Mask[1] - 2) == 1) << 1);
  return DAG.getNode(X86ISD::SHUFP, SDLoc(Op), MVT::v2f64, V1, V2,
                     DAG.getConstant(SHUFPDMask, MVT::i8));
}

/// \brief Handle lowering of 2-lane 64-bit integer shuffles.
///
/// Tries to lower a 2-lane 64-bit shuffle using shuffle operations provided by
/// the integer unit to minimize domain crossing penalties. However, for blends
/// it falls back to the floating point shuffle operation with appropriate bit
/// casting.
static SDValue lowerV2I64VectorShuffle(SDValue Op, SDValue V1, SDValue V2,
                                       const X86Subtarget *Subtarget,
                                       SelectionDAG &DAG) {
  SDLoc DL(Op);
  assert(Op.getSimpleValueType() == MVT::v2i64 && "Bad shuffle type!");
  assert(V1.getSimpleValueType() == MVT::v2i64 && "Bad operand type!");
  assert(V2.getSimpleValueType() == MVT::v2i64 && "Bad operand type!");
  ShuffleVectorSDNode *SVOp = cast<ShuffleVectorSDNode>(Op);
  ArrayRef<int> Mask = SVOp->getMask();
  assert(Mask.size() == 2 && "Unexpected mask size for v2 shuffle!");

  if (isSingleInputShuffleMask(Mask)) {
    // Straight shuffle of a single input vector. For everything from SSE2
    // onward this has a single fast instruction with no scary immediates.
    // We have to map the mask as it is actually a v4i32 shuffle instruction.
    V1 = DAG.getNode(ISD::BITCAST, DL, MVT::v4i32, V1);
    int WidenedMask[4] = {
        std::max(Mask[0], 0) * 2, std::max(Mask[0], 0) * 2 + 1,
        std::max(Mask[1], 0) * 2, std::max(Mask[1], 0) * 2 + 1};
    return DAG.getNode(
        ISD::BITCAST, DL, MVT::v2i64,
        DAG.getNode(X86ISD::PSHUFD, SDLoc(Op), MVT::v4i32, V1,
                    getV4X86ShuffleImm8ForMask(WidenedMask, DAG)));
  }

  // Use dedicated unpack instructions for masks that match their pattern.
  if (isShuffleEquivalent(Mask, 0, 2))
    return DAG.getNode(X86ISD::UNPCKL, DL, MVT::v2i64, V1, V2);
  if (isShuffleEquivalent(Mask, 1, 3))
    return DAG.getNode(X86ISD::UNPCKH, DL, MVT::v2i64, V1, V2);

  // We implement this with SHUFPD which is pretty lame because it will likely
  // incur 2 cycles of stall for integer vectors on Nehalem and older chips.
  // However, all the alternatives are still more cycles and newer chips don't
  // have this problem. It would be really nice if x86 had better shuffles here.
  V1 = DAG.getNode(ISD::BITCAST, DL, MVT::v2f64, V1);
  V2 = DAG.getNode(ISD::BITCAST, DL, MVT::v2f64, V2);
  return DAG.getNode(ISD::BITCAST, DL, MVT::v2i64,
                     DAG.getVectorShuffle(MVT::v2f64, DL, V1, V2, Mask));
}

/// \brief Lower 4-lane 32-bit floating point shuffles.
///
/// Uses instructions exclusively from the floating point unit to minimize
/// domain crossing penalties, as these are sufficient to implement all v4f32
/// shuffles.
static SDValue lowerV4F32VectorShuffle(SDValue Op, SDValue V1, SDValue V2,
                                       const X86Subtarget *Subtarget,
                                       SelectionDAG &DAG) {
  SDLoc DL(Op);
  assert(Op.getSimpleValueType() == MVT::v4f32 && "Bad shuffle type!");
  assert(V1.getSimpleValueType() == MVT::v4f32 && "Bad operand type!");
  assert(V2.getSimpleValueType() == MVT::v4f32 && "Bad operand type!");
  ShuffleVectorSDNode *SVOp = cast<ShuffleVectorSDNode>(Op);
  ArrayRef<int> Mask = SVOp->getMask();
  assert(Mask.size() == 4 && "Unexpected mask size for v4 shuffle!");

  SDValue LowV = V1, HighV = V2;
  int NewMask[4] = {Mask[0], Mask[1], Mask[2], Mask[3]};

  int NumV2Elements =
      std::count_if(Mask.begin(), Mask.end(), [](int M) { return M >= 4; });

  if (NumV2Elements == 0)
    // Straight shuffle of a single input vector. We pass the input vector to
    // both operands to simulate this with a SHUFPS.
    return DAG.getNode(X86ISD::SHUFP, DL, MVT::v4f32, V1, V1,
                       getV4X86ShuffleImm8ForMask(Mask, DAG));

  // Use dedicated unpack instructions for masks that match their pattern.
  if (isShuffleEquivalent(Mask, 0, 4, 1, 5))
    return DAG.getNode(X86ISD::UNPCKL, DL, MVT::v4f32, V1, V2);
  if (isShuffleEquivalent(Mask, 2, 6, 3, 7))
    return DAG.getNode(X86ISD::UNPCKH, DL, MVT::v4f32, V1, V2);

  if (NumV2Elements == 1) {
    int V2Index =
        std::find_if(Mask.begin(), Mask.end(), [](int M) { return M >= 4; }) -
        Mask.begin();
    // Compute the index adjacent to V2Index and in the same half by toggling
    // the low bit.
    int V2AdjIndex = V2Index ^ 1;

    if (Mask[V2AdjIndex] == -1) {
      // Handles all the cases where we have a single V2 element and an undef.
      // This will only ever happen in the high lanes because we commute the
      // vector otherwise.
      if (V2Index < 2)
        std::swap(LowV, HighV);
      NewMask[V2Index] -= 4;
    } else {
      // Handle the case where the V2 element ends up adjacent to a V1 element.
      // To make this work, blend them together as the first step.
      int V1Index = V2AdjIndex;
      int BlendMask[4] = {Mask[V2Index] - 4, 0, Mask[V1Index], 0};
      V2 = DAG.getNode(X86ISD::SHUFP, DL, MVT::v4f32, V2, V1,
                       getV4X86ShuffleImm8ForMask(BlendMask, DAG));

      // Now proceed to reconstruct the final blend as we have the necessary
      // high or low half formed.
      if (V2Index < 2) {
        LowV = V2;
        HighV = V1;
      } else {
        HighV = V2;
      }
      NewMask[V1Index] = 2; // We put the V1 element in V2[2].
      NewMask[V2Index] = 0; // We shifted the V2 element into V2[0].
    }
  } else if (NumV2Elements == 2) {
    if (Mask[0] < 4 && Mask[1] < 4) {
      // Handle the easy case where we have V1 in the low lanes and V2 in the
      // high lanes. We never see this reversed because we sort the shuffle.
      NewMask[2] -= 4;
      NewMask[3] -= 4;
    } else {
      // We have a mixture of V1 and V2 in both low and high lanes. Rather than
      // trying to place elements directly, just blend them and set up the final
      // shuffle to place them.

      // The first two blend mask elements are for V1, the second two are for
      // V2.
      int BlendMask[4] = {Mask[0] < 4 ? Mask[0] : Mask[1],
                          Mask[2] < 4 ? Mask[2] : Mask[3],
                          (Mask[0] >= 4 ? Mask[0] : Mask[1]) - 4,
                          (Mask[2] >= 4 ? Mask[2] : Mask[3]) - 4};
      V1 = DAG.getNode(X86ISD::SHUFP, DL, MVT::v4f32, V1, V2,
                       getV4X86ShuffleImm8ForMask(BlendMask, DAG));

      // Now we do a normal shuffle of V1 by giving V1 as both operands to
      // a blend.
      LowV = HighV = V1;
      NewMask[0] = Mask[0] < 4 ? 0 : 2;
      NewMask[1] = Mask[0] < 4 ? 2 : 0;
      NewMask[2] = Mask[2] < 4 ? 1 : 3;
      NewMask[3] = Mask[2] < 4 ? 3 : 1;
    }
  }
  return DAG.getNode(X86ISD::SHUFP, DL, MVT::v4f32, LowV, HighV,
                     getV4X86ShuffleImm8ForMask(NewMask, DAG));
}

/// \brief Lower 4-lane i32 vector shuffles.
///
/// We try to handle these with integer-domain shuffles where we can, but for
/// blends we use the floating point domain blend instructions.
static SDValue lowerV4I32VectorShuffle(SDValue Op, SDValue V1, SDValue V2,
                                       const X86Subtarget *Subtarget,
                                       SelectionDAG &DAG) {
  SDLoc DL(Op);
  assert(Op.getSimpleValueType() == MVT::v4i32 && "Bad shuffle type!");
  assert(V1.getSimpleValueType() == MVT::v4i32 && "Bad operand type!");
  assert(V2.getSimpleValueType() == MVT::v4i32 && "Bad operand type!");
  ShuffleVectorSDNode *SVOp = cast<ShuffleVectorSDNode>(Op);
  ArrayRef<int> Mask = SVOp->getMask();
  assert(Mask.size() == 4 && "Unexpected mask size for v4 shuffle!");

  if (isSingleInputShuffleMask(Mask))
    // Straight shuffle of a single input vector. For everything from SSE2
    // onward this has a single fast instruction with no scary immediates.
    return DAG.getNode(X86ISD::PSHUFD, DL, MVT::v4i32, V1,
                       getV4X86ShuffleImm8ForMask(Mask, DAG));

  // Use dedicated unpack instructions for masks that match their pattern.
  if (isShuffleEquivalent(Mask, 0, 4, 1, 5))
    return DAG.getNode(X86ISD::UNPCKL, DL, MVT::v4i32, V1, V2);
  if (isShuffleEquivalent(Mask, 2, 6, 3, 7))
    return DAG.getNode(X86ISD::UNPCKH, DL, MVT::v4i32, V1, V2);

  // We implement this with SHUFPS because it can blend from two vectors.
  // Because we're going to eventually use SHUFPS, we use SHUFPS even to build
  // up the inputs, bypassing domain shift penalties that we would encur if we
  // directly used PSHUFD on Nehalem and older. For newer chips, this isn't
  // relevant.
  return DAG.getNode(ISD::BITCAST, DL, MVT::v4i32,
                     DAG.getVectorShuffle(
                         MVT::v4f32, DL,
                         DAG.getNode(ISD::BITCAST, DL, MVT::v4f32, V1),
                         DAG.getNode(ISD::BITCAST, DL, MVT::v4f32, V2), Mask));
}

/// \brief Lowering of single-input v8i16 shuffles is the cornerstone of SSE2
/// shuffle lowering, and the most complex part.
///
/// The lowering strategy is to try to form pairs of input lanes which are
/// targeted at the same half of the final vector, and then use a dword shuffle
/// to place them onto the right half, and finally unpack the paired lanes into
/// their final position.
///
/// The exact breakdown of how to form these dword pairs and align them on the
/// correct sides is really tricky. See the comments within the function for
/// more of the details.
static SDValue lowerV8I16SingleInputVectorShuffle(
    SDLoc DL, SDValue V, MutableArrayRef<int> Mask,
    const X86Subtarget *Subtarget, SelectionDAG &DAG) {
  assert(V.getSimpleValueType() == MVT::v8i16 && "Bad input type!");
  MutableArrayRef<int> LoMask = Mask.slice(0, 4);
  MutableArrayRef<int> HiMask = Mask.slice(4, 4);

  SmallVector<int, 4> LoInputs;
  std::copy_if(LoMask.begin(), LoMask.end(), std::back_inserter(LoInputs),
               [](int M) { return M >= 0; });
  std::sort(LoInputs.begin(), LoInputs.end());
  LoInputs.erase(std::unique(LoInputs.begin(), LoInputs.end()), LoInputs.end());
  SmallVector<int, 4> HiInputs;
  std::copy_if(HiMask.begin(), HiMask.end(), std::back_inserter(HiInputs),
               [](int M) { return M >= 0; });
  std::sort(HiInputs.begin(), HiInputs.end());
  HiInputs.erase(std::unique(HiInputs.begin(), HiInputs.end()), HiInputs.end());
  int NumLToL =
      std::lower_bound(LoInputs.begin(), LoInputs.end(), 4) - LoInputs.begin();
  int NumHToL = LoInputs.size() - NumLToL;
  int NumLToH =
      std::lower_bound(HiInputs.begin(), HiInputs.end(), 4) - HiInputs.begin();
  int NumHToH = HiInputs.size() - NumLToH;
  MutableArrayRef<int> LToLInputs(LoInputs.data(), NumLToL);
  MutableArrayRef<int> LToHInputs(HiInputs.data(), NumLToH);
  MutableArrayRef<int> HToLInputs(LoInputs.data() + NumLToL, NumHToL);
  MutableArrayRef<int> HToHInputs(HiInputs.data() + NumLToH, NumHToH);

  // Simplify the 1-into-3 and 3-into-1 cases with a single pshufd. For all
  // such inputs we can swap two of the dwords across the half mark and end up
  // with <=2 inputs to each half in each half. Once there, we can fall through
  // to the generic code below. For example:
  //
  // Input: [a, b, c, d, e, f, g, h] -PSHUFD[0,2,1,3]-> [a, b, e, f, c, d, g, h]
  // Mask:  [0, 1, 2, 7, 4, 5, 6, 3] -----------------> [0, 1, 4, 7, 2, 3, 6, 5]
  //
  // However in some very rare cases we have a 1-into-3 or 3-into-1 on one half
  // and an existing 2-into-2 on the other half. In this case we may have to
  // pre-shuffle the 2-into-2 half to avoid turning it into a 3-into-1 or
  // 1-into-3 which could cause us to cycle endlessly fixing each side in turn.
  // Fortunately, we don't have to handle anything but a 2-into-2 pattern
  // because any other situation (including a 3-into-1 or 1-into-3 in the other
  // half than the one we target for fixing) will be fixed when we re-enter this
  // path. We will also combine away any sequence of PSHUFD instructions that
  // result into a single instruction. Here is an example of the tricky case:
  //
  // Input: [a, b, c, d, e, f, g, h] -PSHUFD[0,2,1,3]-> [a, b, e, f, c, d, g, h]
  // Mask:  [3, 7, 1, 0, 2, 7, 3, 5] -THIS-IS-BAD!!!!-> [5, 7, 1, 0, 4, 7, 5, 3]
  //
  // This now has a 1-into-3 in the high half! Instead, we do two shuffles:
  //
  // Input: [a, b, c, d, e, f, g, h] PSHUFHW[0,2,1,3]-> [a, b, c, d, e, g, f, h]
  // Mask:  [3, 7, 1, 0, 2, 7, 3, 5] -----------------> [3, 7, 1, 0, 2, 7, 3, 6]
  //
  // Input: [a, b, c, d, e, g, f, h] -PSHUFD[0,2,1,3]-> [a, b, e, g, c, d, f, h]
  // Mask:  [3, 7, 1, 0, 2, 7, 3, 6] -----------------> [5, 7, 1, 0, 4, 7, 5, 6]
  //
  // The result is fine to be handled by the generic logic.
  auto balanceSides = [&](ArrayRef<int> AToAInputs, ArrayRef<int> BToAInputs,
                          ArrayRef<int> BToBInputs, ArrayRef<int> AToBInputs,
                          int AOffset, int BOffset) {
    assert((AToAInputs.size() == 3 || AToAInputs.size() == 1) &&
           "Must call this with A having 3 or 1 inputs from the A half.");
    assert((BToAInputs.size() == 1 || BToAInputs.size() == 3) &&
           "Must call this with B having 1 or 3 inputs from the B half.");
    assert(AToAInputs.size() + BToAInputs.size() == 4 &&
           "Must call this with either 3:1 or 1:3 inputs (summing to 4).");

    // Compute the index of dword with only one word among the three inputs in
    // a half by taking the sum of the half with three inputs and subtracting
    // the sum of the actual three inputs. The difference is the remaining
    // slot.
    int ADWord, BDWord;
    int &TripleDWord = AToAInputs.size() == 3 ? ADWord : BDWord;
    int &OneInputDWord = AToAInputs.size() == 3 ? BDWord : ADWord;
    int TripleInputOffset = AToAInputs.size() == 3 ? AOffset : BOffset;
    ArrayRef<int> TripleInputs = AToAInputs.size() == 3 ? AToAInputs : BToAInputs;
    int OneInput = AToAInputs.size() == 3 ? BToAInputs[0] : AToAInputs[0];
    int TripleInputSum = 0 + 1 + 2 + 3 + (4 * TripleInputOffset);
    int TripleNonInputIdx =
        TripleInputSum - std::accumulate(TripleInputs.begin(), TripleInputs.end(), 0);
    TripleDWord = TripleNonInputIdx / 2;

    // We use xor with one to compute the adjacent DWord to whichever one the
    // OneInput is in.
    OneInputDWord = (OneInput / 2) ^ 1;

    // Check for one tricky case: We're fixing a 3<-1 or a 1<-3 shuffle for AToA
    // and BToA inputs. If there is also such a problem with the BToB and AToB
    // inputs, we don't try to fix it necessarily -- we'll recurse and see it in
    // the next pass. However, if we have a 2<-2 in the BToB and AToB inputs, it
    // is essential that we don't *create* a 3<-1 as then we might oscillate.
    if (BToBInputs.size() == 2 && AToBInputs.size() == 2) {
      // Compute how many inputs will be flipped by swapping these DWords. We
      // need
      // to balance this to ensure we don't form a 3-1 shuffle in the other
      // half.
      int NumFlippedAToBInputs =
          std::count(AToBInputs.begin(), AToBInputs.end(), 2 * ADWord) +
          std::count(AToBInputs.begin(), AToBInputs.end(), 2 * ADWord + 1);
      int NumFlippedBToBInputs =
          std::count(BToBInputs.begin(), BToBInputs.end(), 2 * BDWord) +
          std::count(BToBInputs.begin(), BToBInputs.end(), 2 * BDWord + 1);
      if ((NumFlippedAToBInputs == 1 &&
           (NumFlippedBToBInputs == 0 || NumFlippedBToBInputs == 2)) ||
          (NumFlippedBToBInputs == 1 &&
           (NumFlippedAToBInputs == 0 || NumFlippedAToBInputs == 2))) {
        // We choose whether to fix the A half or B half based on whether that
        // half has zero flipped inputs. At zero, we may not be able to fix it
        // with that half. We also bias towards fixing the B half because that
        // will more commonly be the high half, and we have to bias one way.
        auto FixFlippedInputs = [&V, &DL, &Mask, &DAG](int PinnedIdx, int DWord,
                                                       ArrayRef<int> Inputs) {
          int FixIdx = PinnedIdx ^ 1; // The adjacent slot to the pinned slot.
          bool IsFixIdxInput = std::find(Inputs.begin(), Inputs.end(),
                                         PinnedIdx ^ 1) != Inputs.end();
          // Determine whether the free index is in the flipped dword or the
          // unflipped dword based on where the pinned index is. We use this bit
          // in an xor to conditionally select the adjacent dword.
          int FixFreeIdx = 2 * (DWord ^ (PinnedIdx / 2 == DWord));
          bool IsFixFreeIdxInput = std::find(Inputs.begin(), Inputs.end(),
                                             FixFreeIdx) != Inputs.end();
          if (IsFixIdxInput == IsFixFreeIdxInput)
            FixFreeIdx += 1;
          IsFixFreeIdxInput = std::find(Inputs.begin(), Inputs.end(),
                                        FixFreeIdx) != Inputs.end();
          assert(IsFixIdxInput != IsFixFreeIdxInput &&
                 "We need to be changing the number of flipped inputs!");
          int PSHUFHalfMask[] = {0, 1, 2, 3};
          std::swap(PSHUFHalfMask[FixFreeIdx % 4], PSHUFHalfMask[FixIdx % 4]);
          V = DAG.getNode(FixIdx < 4 ? X86ISD::PSHUFLW : X86ISD::PSHUFHW, DL,
                          MVT::v8i16, V,
                          getV4X86ShuffleImm8ForMask(PSHUFHalfMask, DAG));

          for (int &M : Mask)
            if (M != -1 && M == FixIdx)
              M = FixFreeIdx;
            else if (M != -1 && M == FixFreeIdx)
              M = FixIdx;
        };
        if (NumFlippedBToBInputs != 0) {
          int BPinnedIdx =
              BToAInputs.size() == 3 ? TripleNonInputIdx : OneInput;
          FixFlippedInputs(BPinnedIdx, BDWord, BToBInputs);
        } else {
          assert(NumFlippedAToBInputs != 0 && "Impossible given predicates!");
          int APinnedIdx =
              AToAInputs.size() == 3 ? TripleNonInputIdx : OneInput;
          FixFlippedInputs(APinnedIdx, ADWord, AToBInputs);
        }
      }
    }

    int PSHUFDMask[] = {0, 1, 2, 3};
    PSHUFDMask[ADWord] = BDWord;
    PSHUFDMask[BDWord] = ADWord;
    V = DAG.getNode(ISD::BITCAST, DL, MVT::v8i16,
                    DAG.getNode(X86ISD::PSHUFD, DL, MVT::v4i32,
                                DAG.getNode(ISD::BITCAST, DL, MVT::v4i32, V),
                                getV4X86ShuffleImm8ForMask(PSHUFDMask, DAG)));

    // Adjust the mask to match the new locations of A and B.
    for (int &M : Mask)
      if (M != -1 && M/2 == ADWord)
        M = 2 * BDWord + M % 2;
      else if (M != -1 && M/2 == BDWord)
        M = 2 * ADWord + M % 2;

    // Recurse back into this routine to re-compute state now that this isn't
    // a 3 and 1 problem.
    return DAG.getVectorShuffle(MVT::v8i16, DL, V, DAG.getUNDEF(MVT::v8i16),
                                Mask);
  };
  if ((NumLToL == 3 && NumHToL == 1) || (NumLToL == 1 && NumHToL == 3))
    return balanceSides(LToLInputs, HToLInputs, HToHInputs, LToHInputs, 0, 4);
  else if ((NumHToH == 3 && NumLToH == 1) || (NumHToH == 1 && NumLToH == 3))
    return balanceSides(HToHInputs, LToHInputs, LToLInputs, HToLInputs, 4, 0);

  // At this point there are at most two inputs to the low and high halves from
  // each half. That means the inputs can always be grouped into dwords and
  // those dwords can then be moved to the correct half with a dword shuffle.
  // We use at most one low and one high word shuffle to collect these paired
  // inputs into dwords, and finally a dword shuffle to place them.
  int PSHUFLMask[4] = {-1, -1, -1, -1};
  int PSHUFHMask[4] = {-1, -1, -1, -1};
  int PSHUFDMask[4] = {-1, -1, -1, -1};

  // First fix the masks for all the inputs that are staying in their
  // original halves. This will then dictate the targets of the cross-half
  // shuffles.
  auto fixInPlaceInputs =
      [&PSHUFDMask](ArrayRef<int> InPlaceInputs, ArrayRef<int> IncomingInputs,
                    MutableArrayRef<int> SourceHalfMask,
                    MutableArrayRef<int> HalfMask, int HalfOffset) {
    if (InPlaceInputs.empty())
      return;
    if (InPlaceInputs.size() == 1) {
      SourceHalfMask[InPlaceInputs[0] - HalfOffset] =
          InPlaceInputs[0] - HalfOffset;
      PSHUFDMask[InPlaceInputs[0] / 2] = InPlaceInputs[0] / 2;
      return;
    }
    if (IncomingInputs.empty()) {
      // Just fix all of the in place inputs.
      for (int Input : InPlaceInputs) {
        SourceHalfMask[Input - HalfOffset] = Input - HalfOffset;
        PSHUFDMask[Input / 2] = Input / 2;
      }
      return;
    }

    assert(InPlaceInputs.size() == 2 && "Cannot handle 3 or 4 inputs!");
    SourceHalfMask[InPlaceInputs[0] - HalfOffset] =
        InPlaceInputs[0] - HalfOffset;
    // Put the second input next to the first so that they are packed into
    // a dword. We find the adjacent index by toggling the low bit.
    int AdjIndex = InPlaceInputs[0] ^ 1;
    SourceHalfMask[AdjIndex - HalfOffset] = InPlaceInputs[1] - HalfOffset;
    std::replace(HalfMask.begin(), HalfMask.end(), InPlaceInputs[1], AdjIndex);
    PSHUFDMask[AdjIndex / 2] = AdjIndex / 2;
  };
  fixInPlaceInputs(LToLInputs, HToLInputs, PSHUFLMask, LoMask, 0);
  fixInPlaceInputs(HToHInputs, LToHInputs, PSHUFHMask, HiMask, 4);

  // Now gather the cross-half inputs and place them into a free dword of
  // their target half.
  // FIXME: This operation could almost certainly be simplified dramatically to
  // look more like the 3-1 fixing operation.
  auto moveInputsToRightHalf = [&PSHUFDMask](
      MutableArrayRef<int> IncomingInputs, ArrayRef<int> ExistingInputs,
      MutableArrayRef<int> SourceHalfMask, MutableArrayRef<int> HalfMask,
      MutableArrayRef<int> FinalSourceHalfMask, int SourceOffset,
      int DestOffset) {
    auto isWordClobbered = [](ArrayRef<int> SourceHalfMask, int Word) {
      return SourceHalfMask[Word] != -1 && SourceHalfMask[Word] != Word;
    };
    auto isDWordClobbered = [&isWordClobbered](ArrayRef<int> SourceHalfMask,
                                               int Word) {
      int LowWord = Word & ~1;
      int HighWord = Word | 1;
      return isWordClobbered(SourceHalfMask, LowWord) ||
             isWordClobbered(SourceHalfMask, HighWord);
    };

    if (IncomingInputs.empty())
      return;

    if (ExistingInputs.empty()) {
      // Map any dwords with inputs from them into the right half.
      for (int Input : IncomingInputs) {
        // If the source half mask maps over the inputs, turn those into
        // swaps and use the swapped lane.
        if (isWordClobbered(SourceHalfMask, Input - SourceOffset)) {
          if (SourceHalfMask[SourceHalfMask[Input - SourceOffset]] == -1) {
            SourceHalfMask[SourceHalfMask[Input - SourceOffset]] =
                Input - SourceOffset;
            // We have to swap the uses in our half mask in one sweep.
            for (int &M : HalfMask)
              if (M == SourceHalfMask[Input - SourceOffset] + SourceOffset)
                M = Input;
              else if (M == Input)
                M = SourceHalfMask[Input - SourceOffset] + SourceOffset;
          } else {
            assert(SourceHalfMask[SourceHalfMask[Input - SourceOffset]] ==
                       Input - SourceOffset &&
                   "Previous placement doesn't match!");
          }
          // Note that this correctly re-maps both when we do a swap and when
          // we observe the other side of the swap above. We rely on that to
          // avoid swapping the members of the input list directly.
          Input = SourceHalfMask[Input - SourceOffset] + SourceOffset;
        }

        // Map the input's dword into the correct half.
        if (PSHUFDMask[(Input - SourceOffset + DestOffset) / 2] == -1)
          PSHUFDMask[(Input - SourceOffset + DestOffset) / 2] = Input / 2;
        else
          assert(PSHUFDMask[(Input - SourceOffset + DestOffset) / 2] ==
                     Input / 2 &&
                 "Previous placement doesn't match!");
      }

      // And just directly shift any other-half mask elements to be same-half
      // as we will have mirrored the dword containing the element into the
      // same position within that half.
      for (int &M : HalfMask)
        if (M >= SourceOffset && M < SourceOffset + 4) {
          M = M - SourceOffset + DestOffset;
          assert(M >= 0 && "This should never wrap below zero!");
        }
      return;
    }

    // Ensure we have the input in a viable dword of its current half. This
    // is particularly tricky because the original position may be clobbered
    // by inputs being moved and *staying* in that half.
    if (IncomingInputs.size() == 1) {
      if (isWordClobbered(SourceHalfMask, IncomingInputs[0] - SourceOffset)) {
        int InputFixed = std::find(std::begin(SourceHalfMask),
                                   std::end(SourceHalfMask), -1) -
                         std::begin(SourceHalfMask) + SourceOffset;
        SourceHalfMask[InputFixed - SourceOffset] =
            IncomingInputs[0] - SourceOffset;
        std::replace(HalfMask.begin(), HalfMask.end(), IncomingInputs[0],
                     InputFixed);
        IncomingInputs[0] = InputFixed;
      }
    } else if (IncomingInputs.size() == 2) {
      if (IncomingInputs[0] / 2 != IncomingInputs[1] / 2 ||
          isDWordClobbered(SourceHalfMask, IncomingInputs[0] - SourceOffset)) {
        // We have two non-adjacent or clobbered inputs we need to extract from
        // the source half. To do this, we need to map them into some adjacent
        // dword slot in the source mask.
        int InputsFixed[2] = {IncomingInputs[0] - SourceOffset,
                              IncomingInputs[1] - SourceOffset};

        // If there is a free slot in the source half mask adjacent to one of
        // the inputs, place the other input in it. We use (Index XOR 1) to
        // compute an adjacent index.
        if (!isWordClobbered(SourceHalfMask, InputsFixed[0]) &&
            SourceHalfMask[InputsFixed[0] ^ 1] == -1) {
          SourceHalfMask[InputsFixed[0]] = InputsFixed[0];
          SourceHalfMask[InputsFixed[0] ^ 1] = InputsFixed[1];
          InputsFixed[1] = InputsFixed[0] ^ 1;
        } else if (!isWordClobbered(SourceHalfMask, InputsFixed[1]) &&
                   SourceHalfMask[InputsFixed[1] ^ 1] == -1) {
          SourceHalfMask[InputsFixed[1]] = InputsFixed[1];
          SourceHalfMask[InputsFixed[1] ^ 1] = InputsFixed[0];
          InputsFixed[0] = InputsFixed[1] ^ 1;
        } else if (SourceHalfMask[2 * ((InputsFixed[0] / 2) ^ 1)] == -1 &&
                   SourceHalfMask[2 * ((InputsFixed[0] / 2) ^ 1) + 1] == -1) {
          // The two inputs are in the same DWord but it is clobbered and the
          // adjacent DWord isn't used at all. Move both inputs to the free
          // slot.
          SourceHalfMask[2 * ((InputsFixed[0] / 2) ^ 1)] = InputsFixed[0];
          SourceHalfMask[2 * ((InputsFixed[0] / 2) ^ 1) + 1] = InputsFixed[1];
          InputsFixed[0] = 2 * ((InputsFixed[0] / 2) ^ 1);
          InputsFixed[1] = 2 * ((InputsFixed[0] / 2) ^ 1) + 1;
        } else {
          // The only way we hit this point is if there is no clobbering
          // (because there are no off-half inputs to this half) and there is no
          // free slot adjacent to one of the inputs. In this case, we have to
          // swap an input with a non-input.
          for (int i = 0; i < 4; ++i)
            assert((SourceHalfMask[i] == -1 || SourceHalfMask[i] == i) &&
                   "We can't handle any clobbers here!");
          assert(InputsFixed[1] != (InputsFixed[0] ^ 1) &&
                 "Cannot have adjacent inputs here!");

          SourceHalfMask[InputsFixed[0] ^ 1] = InputsFixed[1];
          SourceHalfMask[InputsFixed[1]] = InputsFixed[0] ^ 1;

          // We also have to update the final source mask in this case because
          // it may need to undo the above swap.
          for (int &M : FinalSourceHalfMask)
            if (M == (InputsFixed[0] ^ 1) + SourceOffset)
              M = InputsFixed[1] + SourceOffset;
            else if (M == InputsFixed[1] + SourceOffset)
              M = (InputsFixed[0] ^ 1) + SourceOffset;

          InputsFixed[1] = InputsFixed[0] ^ 1;
        }

        // Point everything at the fixed inputs.
        for (int &M : HalfMask)
          if (M == IncomingInputs[0])
            M = InputsFixed[0] + SourceOffset;
          else if (M == IncomingInputs[1])
            M = InputsFixed[1] + SourceOffset;

        IncomingInputs[0] = InputsFixed[0] + SourceOffset;
        IncomingInputs[1] = InputsFixed[1] + SourceOffset;
      }
    } else {
      llvm_unreachable("Unhandled input size!");
    }

    // Now hoist the DWord down to the right half.
    int FreeDWord = (PSHUFDMask[DestOffset / 2] == -1 ? 0 : 1) + DestOffset / 2;
    assert(PSHUFDMask[FreeDWord] == -1 && "DWord not free");
    PSHUFDMask[FreeDWord] = IncomingInputs[0] / 2;
    for (int &M : HalfMask)
      for (int Input : IncomingInputs)
        if (M == Input)
          M = FreeDWord * 2 + Input % 2;
  };
  moveInputsToRightHalf(HToLInputs, LToLInputs, PSHUFHMask, LoMask, HiMask,
                        /*SourceOffset*/ 4, /*DestOffset*/ 0);
  moveInputsToRightHalf(LToHInputs, HToHInputs, PSHUFLMask, HiMask, LoMask,
                        /*SourceOffset*/ 0, /*DestOffset*/ 4);

  // Now enact all the shuffles we've computed to move the inputs into their
  // target half.
  if (!isNoopShuffleMask(PSHUFLMask))
    V = DAG.getNode(X86ISD::PSHUFLW, DL, MVT::v8i16, V,
                    getV4X86ShuffleImm8ForMask(PSHUFLMask, DAG));
  if (!isNoopShuffleMask(PSHUFHMask))
    V = DAG.getNode(X86ISD::PSHUFHW, DL, MVT::v8i16, V,
                    getV4X86ShuffleImm8ForMask(PSHUFHMask, DAG));
  if (!isNoopShuffleMask(PSHUFDMask))
    V = DAG.getNode(ISD::BITCAST, DL, MVT::v8i16,
                    DAG.getNode(X86ISD::PSHUFD, DL, MVT::v4i32,
                                DAG.getNode(ISD::BITCAST, DL, MVT::v4i32, V),
                                getV4X86ShuffleImm8ForMask(PSHUFDMask, DAG)));

  // At this point, each half should contain all its inputs, and we can then
  // just shuffle them into their final position.
  assert(std::count_if(LoMask.begin(), LoMask.end(),
                       [](int M) { return M >= 4; }) == 0 &&
         "Failed to lift all the high half inputs to the low mask!");
  assert(std::count_if(HiMask.begin(), HiMask.end(),
                       [](int M) { return M >= 0 && M < 4; }) == 0 &&
         "Failed to lift all the low half inputs to the high mask!");

  // Do a half shuffle for the low mask.
  if (!isNoopShuffleMask(LoMask))
    V = DAG.getNode(X86ISD::PSHUFLW, DL, MVT::v8i16, V,
                    getV4X86ShuffleImm8ForMask(LoMask, DAG));

  // Do a half shuffle with the high mask after shifting its values down.
  for (int &M : HiMask)
    if (M >= 0)
      M -= 4;
  if (!isNoopShuffleMask(HiMask))
    V = DAG.getNode(X86ISD::PSHUFHW, DL, MVT::v8i16, V,
                    getV4X86ShuffleImm8ForMask(HiMask, DAG));

  return V;
}

/// \brief Detect whether the mask pattern should be lowered through
/// interleaving.
///
/// This essentially tests whether viewing the mask as an interleaving of two
/// sub-sequences reduces the cross-input traffic of a blend operation. If so,
/// lowering it through interleaving is a significantly better strategy.
static bool shouldLowerAsInterleaving(ArrayRef<int> Mask) {
  int NumEvenInputs[2] = {0, 0};
  int NumOddInputs[2] = {0, 0};
  int NumLoInputs[2] = {0, 0};
  int NumHiInputs[2] = {0, 0};
  for (int i = 0, Size = Mask.size(); i < Size; ++i) {
    if (Mask[i] < 0)
      continue;

    int InputIdx = Mask[i] >= Size;

    if (i < Size / 2)
      ++NumLoInputs[InputIdx];
    else
      ++NumHiInputs[InputIdx];

    if ((i % 2) == 0)
      ++NumEvenInputs[InputIdx];
    else
      ++NumOddInputs[InputIdx];
  }

  // The minimum number of cross-input results for both the interleaved and
  // split cases. If interleaving results in fewer cross-input results, return
  // true.
  int InterleavedCrosses = std::min(NumEvenInputs[1] + NumOddInputs[0],
                                    NumEvenInputs[0] + NumOddInputs[1]);
  int SplitCrosses = std::min(NumLoInputs[1] + NumHiInputs[0],
                              NumLoInputs[0] + NumHiInputs[1]);
  return InterleavedCrosses < SplitCrosses;
}

/// \brief Blend two v8i16 vectors using a naive unpack strategy.
///
/// This strategy only works when the inputs from each vector fit into a single
/// half of that vector, and generally there are not so many inputs as to leave
/// the in-place shuffles required highly constrained (and thus expensive). It
/// shifts all the inputs into a single side of both input vectors and then
/// uses an unpack to interleave these inputs in a single vector. At that
/// point, we will fall back on the generic single input shuffle lowering.
static SDValue lowerV8I16BasicBlendVectorShuffle(SDLoc DL, SDValue V1,
                                                 SDValue V2,
                                                 MutableArrayRef<int> Mask,
                                                 const X86Subtarget *Subtarget,
                                                 SelectionDAG &DAG) {
  assert(V1.getSimpleValueType() == MVT::v8i16 && "Bad input type!");
  assert(V2.getSimpleValueType() == MVT::v8i16 && "Bad input type!");
  SmallVector<int, 3> LoV1Inputs, HiV1Inputs, LoV2Inputs, HiV2Inputs;
  for (int i = 0; i < 8; ++i)
    if (Mask[i] >= 0 && Mask[i] < 4)
      LoV1Inputs.push_back(i);
    else if (Mask[i] >= 4 && Mask[i] < 8)
      HiV1Inputs.push_back(i);
    else if (Mask[i] >= 8 && Mask[i] < 12)
      LoV2Inputs.push_back(i);
    else if (Mask[i] >= 12)
      HiV2Inputs.push_back(i);

  int NumV1Inputs = LoV1Inputs.size() + HiV1Inputs.size();
  int NumV2Inputs = LoV2Inputs.size() + HiV2Inputs.size();
  (void)NumV1Inputs;
  (void)NumV2Inputs;
  assert(NumV1Inputs > 0 && NumV1Inputs <= 3 && "At most 3 inputs supported");
  assert(NumV2Inputs > 0 && NumV2Inputs <= 3 && "At most 3 inputs supported");
  assert(NumV1Inputs + NumV2Inputs <= 4 && "At most 4 combined inputs");

  bool MergeFromLo = LoV1Inputs.size() + LoV2Inputs.size() >=
                     HiV1Inputs.size() + HiV2Inputs.size();

  auto moveInputsToHalf = [&](SDValue V, ArrayRef<int> LoInputs,
                              ArrayRef<int> HiInputs, bool MoveToLo,
                              int MaskOffset) {
    ArrayRef<int> GoodInputs = MoveToLo ? LoInputs : HiInputs;
    ArrayRef<int> BadInputs = MoveToLo ? HiInputs : LoInputs;
    if (BadInputs.empty())
      return V;

    int MoveMask[] = {-1, -1, -1, -1, -1, -1, -1, -1};
    int MoveOffset = MoveToLo ? 0 : 4;

    if (GoodInputs.empty()) {
      for (int BadInput : BadInputs) {
        MoveMask[Mask[BadInput] % 4 + MoveOffset] = Mask[BadInput] - MaskOffset;
        Mask[BadInput] = Mask[BadInput] % 4 + MoveOffset + MaskOffset;
      }
    } else {
      if (GoodInputs.size() == 2) {
        // If the low inputs are spread across two dwords, pack them into
        // a single dword.
        MoveMask[MoveOffset] = Mask[GoodInputs[0]] - MaskOffset;
        MoveMask[MoveOffset + 1] = Mask[GoodInputs[1]] - MaskOffset;
        Mask[GoodInputs[0]] = MoveOffset + MaskOffset;
        Mask[GoodInputs[1]] = MoveOffset + 1 + MaskOffset;
      } else {
        // Otherwise pin the good inputs.
        for (int GoodInput : GoodInputs)
          MoveMask[Mask[GoodInput] - MaskOffset] = Mask[GoodInput] - MaskOffset;
      }

      if (BadInputs.size() == 2) {
        // If we have two bad inputs then there may be either one or two good
        // inputs fixed in place. Find a fixed input, and then find the *other*
        // two adjacent indices by using modular arithmetic.
        int GoodMaskIdx =
            std::find_if(std::begin(MoveMask) + MoveOffset, std::end(MoveMask),
                         [](int M) { return M >= 0; }) -
            std::begin(MoveMask);
        int MoveMaskIdx =
            ((((GoodMaskIdx - MoveOffset) & ~1) + 2) % 4) + MoveOffset;
        assert(MoveMask[MoveMaskIdx] == -1 && "Expected empty slot");
        assert(MoveMask[MoveMaskIdx + 1] == -1 && "Expected empty slot");
        MoveMask[MoveMaskIdx] = Mask[BadInputs[0]] - MaskOffset;
        MoveMask[MoveMaskIdx + 1] = Mask[BadInputs[1]] - MaskOffset;
        Mask[BadInputs[0]] = MoveMaskIdx + MaskOffset;
        Mask[BadInputs[1]] = MoveMaskIdx + 1 + MaskOffset;
      } else {
        assert(BadInputs.size() == 1 && "All sizes handled");
        int MoveMaskIdx = std::find(std::begin(MoveMask) + MoveOffset,
                                    std::end(MoveMask), -1) -
                          std::begin(MoveMask);
        MoveMask[MoveMaskIdx] = Mask[BadInputs[0]] - MaskOffset;
        Mask[BadInputs[0]] = MoveMaskIdx + MaskOffset;
      }
    }

    return DAG.getVectorShuffle(MVT::v8i16, DL, V, DAG.getUNDEF(MVT::v8i16),
                                MoveMask);
  };
  V1 = moveInputsToHalf(V1, LoV1Inputs, HiV1Inputs, MergeFromLo,
                        /*MaskOffset*/ 0);
  V2 = moveInputsToHalf(V2, LoV2Inputs, HiV2Inputs, MergeFromLo,
                        /*MaskOffset*/ 8);

  // FIXME: Select an interleaving of the merge of V1 and V2 that minimizes
  // cross-half traffic in the final shuffle.

  // Munge the mask to be a single-input mask after the unpack merges the
  // results.
  for (int &M : Mask)
    if (M != -1)
      M = 2 * (M % 4) + (M / 8);

  return DAG.getVectorShuffle(
      MVT::v8i16, DL, DAG.getNode(MergeFromLo ? X86ISD::UNPCKL : X86ISD::UNPCKH,
                                  DL, MVT::v8i16, V1, V2),
      DAG.getUNDEF(MVT::v8i16), Mask);
}

/// \brief Generic lowering of 8-lane i16 shuffles.
///
/// This handles both single-input shuffles and combined shuffle/blends with
/// two inputs. The single input shuffles are immediately delegated to
/// a dedicated lowering routine.
///
/// The blends are lowered in one of three fundamental ways. If there are few
/// enough inputs, it delegates to a basic UNPCK-based strategy. If the shuffle
/// of the input is significantly cheaper when lowered as an interleaving of
/// the two inputs, try to interleave them. Otherwise, blend the low and high
/// halves of the inputs separately (making them have relatively few inputs)
/// and then concatenate them.
static SDValue lowerV8I16VectorShuffle(SDValue Op, SDValue V1, SDValue V2,
                                       const X86Subtarget *Subtarget,
                                       SelectionDAG &DAG) {
  SDLoc DL(Op);
  assert(Op.getSimpleValueType() == MVT::v8i16 && "Bad shuffle type!");
  assert(V1.getSimpleValueType() == MVT::v8i16 && "Bad operand type!");
  assert(V2.getSimpleValueType() == MVT::v8i16 && "Bad operand type!");
  ShuffleVectorSDNode *SVOp = cast<ShuffleVectorSDNode>(Op);
  ArrayRef<int> OrigMask = SVOp->getMask();
  int MaskStorage[8] = {OrigMask[0], OrigMask[1], OrigMask[2], OrigMask[3],
                        OrigMask[4], OrigMask[5], OrigMask[6], OrigMask[7]};
  MutableArrayRef<int> Mask(MaskStorage);

  assert(Mask.size() == 8 && "Unexpected mask size for v8 shuffle!");

  auto isV1 = [](int M) { return M >= 0 && M < 8; };
  auto isV2 = [](int M) { return M >= 8; };

  int NumV1Inputs = std::count_if(Mask.begin(), Mask.end(), isV1);
  int NumV2Inputs = std::count_if(Mask.begin(), Mask.end(), isV2);

  if (NumV2Inputs == 0)
    return lowerV8I16SingleInputVectorShuffle(DL, V1, Mask, Subtarget, DAG);

  assert(NumV1Inputs > 0 && "All single-input shuffles should be canonicalized "
                            "to be V1-input shuffles.");

  if (NumV1Inputs + NumV2Inputs <= 4)
    return lowerV8I16BasicBlendVectorShuffle(DL, V1, V2, Mask, Subtarget, DAG);

  // Check whether an interleaving lowering is likely to be more efficient.
  // This isn't perfect but it is a strong heuristic that tends to work well on
  // the kinds of shuffles that show up in practice.
  //
  // FIXME: Handle 1x, 2x, and 4x interleaving.
  if (shouldLowerAsInterleaving(Mask)) {
    // FIXME: Figure out whether we should pack these into the low or high
    // halves.

    int EMask[8], OMask[8];
    for (int i = 0; i < 4; ++i) {
      EMask[i] = Mask[2*i];
      OMask[i] = Mask[2*i + 1];
      EMask[i + 4] = -1;
      OMask[i + 4] = -1;
    }

    SDValue Evens = DAG.getVectorShuffle(MVT::v8i16, DL, V1, V2, EMask);
    SDValue Odds = DAG.getVectorShuffle(MVT::v8i16, DL, V1, V2, OMask);

    return DAG.getNode(X86ISD::UNPCKL, DL, MVT::v8i16, Evens, Odds);
  }

  int LoBlendMask[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
  int HiBlendMask[8] = {-1, -1, -1, -1, -1, -1, -1, -1};

  for (int i = 0; i < 4; ++i) {
    LoBlendMask[i] = Mask[i];
    HiBlendMask[i] = Mask[i + 4];
  }

  SDValue LoV = DAG.getVectorShuffle(MVT::v8i16, DL, V1, V2, LoBlendMask);
  SDValue HiV = DAG.getVectorShuffle(MVT::v8i16, DL, V1, V2, HiBlendMask);
  LoV = DAG.getNode(ISD::BITCAST, DL, MVT::v2i64, LoV);
  HiV = DAG.getNode(ISD::BITCAST, DL, MVT::v2i64, HiV);

  return DAG.getNode(ISD::BITCAST, DL, MVT::v8i16,
                     DAG.getNode(X86ISD::UNPCKL, DL, MVT::v2i64, LoV, HiV));
}

/// \brief Check whether a compaction lowering can be done by dropping even
/// elements and compute how many times even elements must be dropped.
///
/// This handles shuffles which take every Nth element where N is a power of
/// two. Example shuffle masks:
///
///  N = 1:  0,  2,  4,  6,  8, 10, 12, 14,  0,  2,  4,  6,  8, 10, 12, 14
///  N = 1:  0,  2,  4,  6,  8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30
///  N = 2:  0,  4,  8, 12,  0,  4,  8, 12,  0,  4,  8, 12,  0,  4,  8, 12
///  N = 2:  0,  4,  8, 12, 16, 20, 24, 28,  0,  4,  8, 12, 16, 20, 24, 28
///  N = 3:  0,  8,  0,  8,  0,  8,  0,  8,  0,  8,  0,  8,  0,  8,  0,  8
///  N = 3:  0,  8, 16, 24,  0,  8, 16, 24,  0,  8, 16, 24,  0,  8, 16, 24
///
/// Any of these lanes can of course be undef.
///
/// This routine only supports N <= 3.
/// FIXME: Evaluate whether either AVX or AVX-512 have any opportunities here
/// for larger N.
///
/// \returns N above, or the number of times even elements must be dropped if
/// there is such a number. Otherwise returns zero.
static int canLowerByDroppingEvenElements(ArrayRef<int> Mask) {
  // Figure out whether we're looping over two inputs or just one.
  bool IsSingleInput = isSingleInputShuffleMask(Mask);

  // The modulus for the shuffle vector entries is based on whether this is
  // a single input or not.
  int ShuffleModulus = Mask.size() * (IsSingleInput ? 1 : 2);
  assert(isPowerOf2_32((uint32_t)ShuffleModulus) &&
         "We should only be called with masks with a power-of-2 size!");

  uint64_t ModMask = (uint64_t)ShuffleModulus - 1;

  // We track whether the input is viable for all power-of-2 strides 2^1, 2^2,
  // and 2^3 simultaneously. This is because we may have ambiguity with
  // partially undef inputs.
  bool ViableForN[3] = {true, true, true};

  for (int i = 0, e = Mask.size(); i < e; ++i) {
    // Ignore undef lanes, we'll optimistically collapse them to the pattern we
    // want.
    if (Mask[i] == -1)
      continue;

    bool IsAnyViable = false;
    for (unsigned j = 0; j != array_lengthof(ViableForN); ++j)
      if (ViableForN[j]) {
        uint64_t N = j + 1;

        // The shuffle mask must be equal to (i * 2^N) % M.
        if ((uint64_t)Mask[i] == (((uint64_t)i << N) & ModMask))
          IsAnyViable = true;
        else
          ViableForN[j] = false;
      }
    // Early exit if we exhaust the possible powers of two.
    if (!IsAnyViable)
      break;
  }

  for (unsigned j = 0; j != array_lengthof(ViableForN); ++j)
    if (ViableForN[j])
      return j + 1;

  // Return 0 as there is no viable power of two.
  return 0;
}

/// \brief Generic lowering of v16i8 shuffles.
///
/// This is a hybrid strategy to lower v16i8 vectors. It first attempts to
/// detect any complexity reducing interleaving. If that doesn't help, it uses
/// UNPCK to spread the i8 elements across two i16-element vectors, and uses
/// the existing lowering for v8i16 blends on each half, finally PACK-ing them
/// back together.
static SDValue lowerV16I8VectorShuffle(SDValue Op, SDValue V1, SDValue V2,
                                       const X86Subtarget *Subtarget,
                                       SelectionDAG &DAG) {
  SDLoc DL(Op);
  assert(Op.getSimpleValueType() == MVT::v16i8 && "Bad shuffle type!");
  assert(V1.getSimpleValueType() == MVT::v16i8 && "Bad operand type!");
  assert(V2.getSimpleValueType() == MVT::v16i8 && "Bad operand type!");
  ShuffleVectorSDNode *SVOp = cast<ShuffleVectorSDNode>(Op);
  ArrayRef<int> OrigMask = SVOp->getMask();
  assert(OrigMask.size() == 16 && "Unexpected mask size for v16 shuffle!");
  int MaskStorage[16] = {
      OrigMask[0],  OrigMask[1],  OrigMask[2],  OrigMask[3],
      OrigMask[4],  OrigMask[5],  OrigMask[6],  OrigMask[7],
      OrigMask[8],  OrigMask[9],  OrigMask[10], OrigMask[11],
      OrigMask[12], OrigMask[13], OrigMask[14], OrigMask[15]};
  MutableArrayRef<int> Mask(MaskStorage);
  MutableArrayRef<int> LoMask = Mask.slice(0, 8);
  MutableArrayRef<int> HiMask = Mask.slice(8, 8);

  // For single-input shuffles, there are some nicer lowering tricks we can use.
  if (isSingleInputShuffleMask(Mask)) {
    // Check whether we can widen this to an i16 shuffle by duplicating bytes.
    // Notably, this handles splat and partial-splat shuffles more efficiently.
    // However, it only makes sense if the pre-duplication shuffle simplifies
    // things significantly. Currently, this means we need to be able to
    // express the pre-duplication shuffle as an i16 shuffle.
    //
    // FIXME: We should check for other patterns which can be widened into an
    // i16 shuffle as well.
    auto canWidenViaDuplication = [](ArrayRef<int> Mask) {
      for (int i = 0; i < 16; i += 2) {
        if (Mask[i] != Mask[i + 1])
          return false;
      }
      return true;
    };
    auto tryToWidenViaDuplication = [&]() -> SDValue {
      if (!canWidenViaDuplication(Mask))
        return SDValue();
      SmallVector<int, 4> LoInputs;
      std::copy_if(Mask.begin(), Mask.end(), std::back_inserter(LoInputs),
                   [](int M) { return M >= 0 && M < 8; });
      std::sort(LoInputs.begin(), LoInputs.end());
      LoInputs.erase(std::unique(LoInputs.begin(), LoInputs.end()),
                     LoInputs.end());
      SmallVector<int, 4> HiInputs;
      std::copy_if(Mask.begin(), Mask.end(), std::back_inserter(HiInputs),
                   [](int M) { return M >= 8; });
      std::sort(HiInputs.begin(), HiInputs.end());
      HiInputs.erase(std::unique(HiInputs.begin(), HiInputs.end()),
                     HiInputs.end());

      bool TargetLo = LoInputs.size() >= HiInputs.size();
      ArrayRef<int> InPlaceInputs = TargetLo ? LoInputs : HiInputs;
      ArrayRef<int> MovingInputs = TargetLo ? HiInputs : LoInputs;

      int PreDupI16Shuffle[] = {-1, -1, -1, -1, -1, -1, -1, -1};
      SmallDenseMap<int, int, 8> LaneMap;
      for (int I : InPlaceInputs) {
        PreDupI16Shuffle[I/2] = I/2;
        LaneMap[I] = I;
      }
      int j = TargetLo ? 0 : 4, je = j + 4;
      for (int i = 0, ie = MovingInputs.size(); i < ie; ++i) {
        // Check if j is already a shuffle of this input. This happens when
        // there are two adjacent bytes after we move the low one.
        if (PreDupI16Shuffle[j] != MovingInputs[i] / 2) {
          // If we haven't yet mapped the input, search for a slot into which
          // we can map it.
          while (j < je && PreDupI16Shuffle[j] != -1)
            ++j;

          if (j == je)
            // We can't place the inputs into a single half with a simple i16 shuffle, so bail.
            return SDValue();

          // Map this input with the i16 shuffle.
          PreDupI16Shuffle[j] = MovingInputs[i] / 2;
        }

        // Update the lane map based on the mapping we ended up with.
        LaneMap[MovingInputs[i]] = 2 * j + MovingInputs[i] % 2;
      }
      V1 = DAG.getNode(
          ISD::BITCAST, DL, MVT::v16i8,
          DAG.getVectorShuffle(MVT::v8i16, DL,
                               DAG.getNode(ISD::BITCAST, DL, MVT::v8i16, V1),
                               DAG.getUNDEF(MVT::v8i16), PreDupI16Shuffle));

      // Unpack the bytes to form the i16s that will be shuffled into place.
      V1 = DAG.getNode(TargetLo ? X86ISD::UNPCKL : X86ISD::UNPCKH, DL,
                       MVT::v16i8, V1, V1);

      int PostDupI16Shuffle[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
      for (int i = 0; i < 16; i += 2) {
        if (Mask[i] != -1)
          PostDupI16Shuffle[i / 2] = LaneMap[Mask[i]] - (TargetLo ? 0 : 8);
        assert(PostDupI16Shuffle[i / 2] < 8 && "Invalid v8 shuffle mask!");
      }
      return DAG.getNode(
          ISD::BITCAST, DL, MVT::v16i8,
          DAG.getVectorShuffle(MVT::v8i16, DL,
                               DAG.getNode(ISD::BITCAST, DL, MVT::v8i16, V1),
                               DAG.getUNDEF(MVT::v8i16), PostDupI16Shuffle));
    };
    if (SDValue V = tryToWidenViaDuplication())
      return V;
  }

  // Check whether an interleaving lowering is likely to be more efficient.
  // This isn't perfect but it is a strong heuristic that tends to work well on
  // the kinds of shuffles that show up in practice.
  //
  // FIXME: We need to handle other interleaving widths (i16, i32, ...).
  if (shouldLowerAsInterleaving(Mask)) {
    // FIXME: Figure out whether we should pack these into the low or high
    // halves.

    int EMask[16], OMask[16];
    for (int i = 0; i < 8; ++i) {
      EMask[i] = Mask[2*i];
      OMask[i] = Mask[2*i + 1];
      EMask[i + 8] = -1;
      OMask[i + 8] = -1;
    }

    SDValue Evens = DAG.getVectorShuffle(MVT::v16i8, DL, V1, V2, EMask);
    SDValue Odds = DAG.getVectorShuffle(MVT::v16i8, DL, V1, V2, OMask);

    return DAG.getNode(X86ISD::UNPCKL, DL, MVT::v16i8, Evens, Odds);
  }

  // Check for SSSE3 which lets us lower all v16i8 shuffles much more directly
  // with PSHUFB. It is important to do this before we attempt to generate any
  // blends but after all of the single-input lowerings. If the single input
  // lowerings can find an instruction sequence that is faster than a PSHUFB, we
  // want to preserve that and we can DAG combine any longer sequences into
  // a PSHUFB in the end. But once we start blending from multiple inputs,
  // the complexity of DAG combining bad patterns back into PSHUFB is too high,
  // and there are *very* few patterns that would actually be faster than the
  // PSHUFB approach because of its ability to zero lanes.
  //
  // FIXME: The only exceptions to the above are blends which are exact
  // interleavings with direct instructions supporting them. We currently don't
  // handle those well here.
  if (Subtarget->hasSSSE3()) {
    SDValue V1Mask[16];
    SDValue V2Mask[16];
    for (int i = 0; i < 16; ++i)
      if (Mask[i] == -1) {
        V1Mask[i] = V2Mask[i] = DAG.getConstant(0x80, MVT::i8);
      } else {
        V1Mask[i] = DAG.getConstant(Mask[i] < 16 ? Mask[i] : 0x80, MVT::i8);
        V2Mask[i] =
            DAG.getConstant(Mask[i] < 16 ? 0x80 : Mask[i] - 16, MVT::i8);
      }
    V1 = DAG.getNode(X86ISD::PSHUFB, DL, MVT::v16i8, V1,
                     DAG.getNode(ISD::BUILD_VECTOR, DL, MVT::v16i8, V1Mask));
    if (isSingleInputShuffleMask(Mask))
      return V1; // Single inputs are easy.

    // Otherwise, blend the two.
    V2 = DAG.getNode(X86ISD::PSHUFB, DL, MVT::v16i8, V2,
                     DAG.getNode(ISD::BUILD_VECTOR, DL, MVT::v16i8, V2Mask));
    return DAG.getNode(ISD::OR, DL, MVT::v16i8, V1, V2);
  }

  // Check whether a compaction lowering can be done. This handles shuffles
  // which take every Nth element for some even N. See the helper function for
  // details.
  //
  // We special case these as they can be particularly efficiently handled with
  // the PACKUSB instruction on x86 and they show up in common patterns of
  // rearranging bytes to truncate wide elements.
  if (int NumEvenDrops = canLowerByDroppingEvenElements(Mask)) {
    // NumEvenDrops is the power of two stride of the elements. Another way of
    // thinking about it is that we need to drop the even elements this many
    // times to get the original input.
    bool IsSingleInput = isSingleInputShuffleMask(Mask);

    // First we need to zero all the dropped bytes.
    assert(NumEvenDrops <= 3 &&
           "No support for dropping even elements more than 3 times.");
    // We use the mask type to pick which bytes are preserved based on how many
    // elements are dropped.
    MVT MaskVTs[] = { MVT::v8i16, MVT::v4i32, MVT::v2i64 };
    SDValue ByteClearMask =
        DAG.getNode(ISD::BITCAST, DL, MVT::v16i8,
                    DAG.getConstant(0xFF, MaskVTs[NumEvenDrops - 1]));
    V1 = DAG.getNode(ISD::AND, DL, MVT::v16i8, V1, ByteClearMask);
    if (!IsSingleInput)
      V2 = DAG.getNode(ISD::AND, DL, MVT::v16i8, V2, ByteClearMask);

    // Now pack things back together.
    V1 = DAG.getNode(ISD::BITCAST, DL, MVT::v8i16, V1);
    V2 = IsSingleInput ? V1 : DAG.getNode(ISD::BITCAST, DL, MVT::v8i16, V2);
    SDValue Result = DAG.getNode(X86ISD::PACKUS, DL, MVT::v16i8, V1, V2);
    for (int i = 1; i < NumEvenDrops; ++i) {
      Result = DAG.getNode(ISD::BITCAST, DL, MVT::v8i16, Result);
      Result = DAG.getNode(X86ISD::PACKUS, DL, MVT::v16i8, Result, Result);
    }

    return Result;
  }

  int V1LoBlendMask[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
  int V1HiBlendMask[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
  int V2LoBlendMask[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
  int V2HiBlendMask[8] = {-1, -1, -1, -1, -1, -1, -1, -1};

  auto buildBlendMasks = [](MutableArrayRef<int> HalfMask,
                            MutableArrayRef<int> V1HalfBlendMask,
                            MutableArrayRef<int> V2HalfBlendMask) {
    for (int i = 0; i < 8; ++i)
      if (HalfMask[i] >= 0 && HalfMask[i] < 16) {
        V1HalfBlendMask[i] = HalfMask[i];
        HalfMask[i] = i;
      } else if (HalfMask[i] >= 16) {
        V2HalfBlendMask[i] = HalfMask[i] - 16;
        HalfMask[i] = i + 8;
      }
  };
  buildBlendMasks(LoMask, V1LoBlendMask, V2LoBlendMask);
  buildBlendMasks(HiMask, V1HiBlendMask, V2HiBlendMask);

  SDValue Zero = getZeroVector(MVT::v8i16, Subtarget, DAG, DL);

  auto buildLoAndHiV8s = [&](SDValue V, MutableArrayRef<int> LoBlendMask,
                             MutableArrayRef<int> HiBlendMask) {
    SDValue V1, V2;
    // Check if any of the odd lanes in the v16i8 are used. If not, we can mask
    // them out and avoid using UNPCK{L,H} to extract the elements of V as
    // i16s.
    if (std::none_of(LoBlendMask.begin(), LoBlendMask.end(),
                     [](int M) { return M >= 0 && M % 2 == 1; }) &&
        std::none_of(HiBlendMask.begin(), HiBlendMask.end(),
                     [](int M) { return M >= 0 && M % 2 == 1; })) {
      // Use a mask to drop the high bytes.
      V1 = DAG.getNode(ISD::BITCAST, DL, MVT::v8i16, V);
      V1 = DAG.getNode(ISD::AND, DL, MVT::v8i16, V1,
                       DAG.getConstant(0x00FF, MVT::v8i16));

      // This will be a single vector shuffle instead of a blend so nuke V2.
      V2 = DAG.getUNDEF(MVT::v8i16);

      // Squash the masks to point directly into V1.
      for (int &M : LoBlendMask)
        if (M >= 0)
          M /= 2;
      for (int &M : HiBlendMask)
        if (M >= 0)
          M /= 2;
    } else {
      // Otherwise just unpack the low half of V into V1 and the high half into
      // V2 so that we can blend them as i16s.
      V1 = DAG.getNode(ISD::BITCAST, DL, MVT::v8i16,
                       DAG.getNode(X86ISD::UNPCKL, DL, MVT::v16i8, V, Zero));
      V2 = DAG.getNode(ISD::BITCAST, DL, MVT::v8i16,
                       DAG.getNode(X86ISD::UNPCKH, DL, MVT::v16i8, V, Zero));
    }

    SDValue BlendedLo = DAG.getVectorShuffle(MVT::v8i16, DL, V1, V2, LoBlendMask);
    SDValue BlendedHi = DAG.getVectorShuffle(MVT::v8i16, DL, V1, V2, HiBlendMask);
    return std::make_pair(BlendedLo, BlendedHi);
  };
  SDValue V1Lo, V1Hi, V2Lo, V2Hi;
  std::tie(V1Lo, V1Hi) = buildLoAndHiV8s(V1, V1LoBlendMask, V1HiBlendMask);
  std::tie(V2Lo, V2Hi) = buildLoAndHiV8s(V2, V2LoBlendMask, V2HiBlendMask);

  SDValue LoV = DAG.getVectorShuffle(MVT::v8i16, DL, V1Lo, V2Lo, LoMask);
  SDValue HiV = DAG.getVectorShuffle(MVT::v8i16, DL, V1Hi, V2Hi, HiMask);

  return DAG.getNode(X86ISD::PACKUS, DL, MVT::v16i8, LoV, HiV);
}

/// \brief Dispatching routine to lower various 128-bit x86 vector shuffles.
///
/// This routine breaks down the specific type of 128-bit shuffle and
/// dispatches to the lowering routines accordingly.
static SDValue lower128BitVectorShuffle(SDValue Op, SDValue V1, SDValue V2,
                                        MVT VT, const X86Subtarget *Subtarget,
                                        SelectionDAG &DAG) {
  switch (VT.SimpleTy) {
  case MVT::v2i64:
    return lowerV2I64VectorShuffle(Op, V1, V2, Subtarget, DAG);
  case MVT::v2f64:
    return lowerV2F64VectorShuffle(Op, V1, V2, Subtarget, DAG);
  case MVT::v4i32:
    return lowerV4I32VectorShuffle(Op, V1, V2, Subtarget, DAG);
  case MVT::v4f32:
    return lowerV4F32VectorShuffle(Op, V1, V2, Subtarget, DAG);
  case MVT::v8i16:
    return lowerV8I16VectorShuffle(Op, V1, V2, Subtarget, DAG);
  case MVT::v16i8:
    return lowerV16I8VectorShuffle(Op, V1, V2, Subtarget, DAG);

  default:
    llvm_unreachable("Unimplemented!");
  }
}

static bool isHalfCrossingShuffleMask(ArrayRef<int> Mask) {
  int Size = Mask.size();
  for (int M : Mask.slice(0, Size / 2))
    if (M >= 0 && (M % Size) >= Size / 2)
      return true;
  for (int M : Mask.slice(Size / 2, Size / 2))
    if (M >= 0 && (M % Size) < Size / 2)
      return true;
  return false;
}

/// \brief Generic routine to split a 256-bit vector shuffle into 128-bit
/// shuffles.
///
/// There is a severely limited set of shuffles available in AVX1 for 256-bit
/// vectors resulting in routinely needing to split the shuffle into two 128-bit
/// shuffles. This can be done generically for any 256-bit vector shuffle and so
/// we encode the logic here for specific shuffle lowering routines to bail to
/// when they exhaust the features avaible to more directly handle the shuffle.
static SDValue splitAndLower256BitVectorShuffle(SDValue Op, SDValue V1,
                                                SDValue V2,
                                                const X86Subtarget *Subtarget,
                                                SelectionDAG &DAG) {
  SDLoc DL(Op);
  MVT VT = Op.getSimpleValueType();
  assert(VT.getSizeInBits() == 256 && "Only for 256-bit vector shuffles!");
  assert(V1.getSimpleValueType() == VT && "Bad operand type!");
  assert(V2.getSimpleValueType() == VT && "Bad operand type!");
  ShuffleVectorSDNode *SVOp = cast<ShuffleVectorSDNode>(Op);
  ArrayRef<int> Mask = SVOp->getMask();

  ArrayRef<int> LoMask = Mask.slice(0, Mask.size()/2);
  ArrayRef<int> HiMask = Mask.slice(Mask.size()/2);

  int NumElements = VT.getVectorNumElements();
  int SplitNumElements = NumElements / 2;
  MVT ScalarVT = VT.getScalarType();
  MVT SplitVT = MVT::getVectorVT(ScalarVT, NumElements / 2);

  SDValue LoV1 = DAG.getNode(ISD::EXTRACT_SUBVECTOR, DL, SplitVT, V1,
                             DAG.getIntPtrConstant(0));
  SDValue HiV1 = DAG.getNode(ISD::EXTRACT_SUBVECTOR, DL, SplitVT, V1,
                             DAG.getIntPtrConstant(SplitNumElements));
  SDValue LoV2 = DAG.getNode(ISD::EXTRACT_SUBVECTOR, DL, SplitVT, V2,
                             DAG.getIntPtrConstant(0));
  SDValue HiV2 = DAG.getNode(ISD::EXTRACT_SUBVECTOR, DL, SplitVT, V2,
                             DAG.getIntPtrConstant(SplitNumElements));

  // Now create two 4-way blends of these half-width vectors.
  auto HalfBlend = [&](ArrayRef<int> HalfMask) {
    SmallVector<int, 16> V1BlendMask, V2BlendMask, BlendMask;
    for (int i = 0; i < SplitNumElements; ++i) {
      int M = HalfMask[i];
      if (M >= NumElements) {
        V2BlendMask.push_back(M - NumElements);
        V1BlendMask.push_back(-1);
        BlendMask.push_back(SplitNumElements + i);
      } else if (M >= 0) {
        V2BlendMask.push_back(-1);
        V1BlendMask.push_back(M);
        BlendMask.push_back(i);
      } else {
        V2BlendMask.push_back(-1);
        V1BlendMask.push_back(-1);
        BlendMask.push_back(-1);
      }
    }
    SDValue V1Blend = DAG.getVectorShuffle(SplitVT, DL, LoV1, HiV1, V1BlendMask);
    SDValue V2Blend = DAG.getVectorShuffle(SplitVT, DL, LoV2, HiV2, V2BlendMask);
    return DAG.getVectorShuffle(SplitVT, DL, V1Blend, V2Blend, BlendMask);
  };
  SDValue Lo = HalfBlend(LoMask);
  SDValue Hi = HalfBlend(HiMask);
  return DAG.getNode(ISD::CONCAT_VECTORS, DL, VT, Lo, Hi);
}

/// \brief Handle lowering of 4-lane 64-bit floating point shuffles.
///
/// Also ends up handling lowering of 4-lane 64-bit integer shuffles when AVX2
/// isn't available.
static SDValue lowerV4F64VectorShuffle(SDValue Op, SDValue V1, SDValue V2,
                                       const X86Subtarget *Subtarget,
                                       SelectionDAG &DAG) {
  SDLoc DL(Op);
  assert(V1.getSimpleValueType() == MVT::v4f64 && "Bad operand type!");
  assert(V2.getSimpleValueType() == MVT::v4f64 && "Bad operand type!");
  ShuffleVectorSDNode *SVOp = cast<ShuffleVectorSDNode>(Op);
  ArrayRef<int> Mask = SVOp->getMask();
  assert(Mask.size() == 4 && "Unexpected mask size for v4 shuffle!");

  // FIXME: If we have AVX2, we should delegate to generic code as crossing
  // shuffles aren't a problem and FP and int have the same patterns.

  // FIXME: We can handle these more cleverly than splitting for v4f64.
  if (isHalfCrossingShuffleMask(Mask))
    return splitAndLower256BitVectorShuffle(Op, V1, V2, Subtarget, DAG);

  if (isSingleInputShuffleMask(Mask)) {
    // Non-half-crossing single input shuffles can be lowerid with an
    // interleaved permutation.
    unsigned VPERMILPMask = (Mask[0] == 1) | ((Mask[1] == 1) << 1) |
                            ((Mask[2] == 3) << 2) | ((Mask[3] == 3) << 3);
    return DAG.getNode(X86ISD::VPERMILP, DL, MVT::v4f64, V1,
                       DAG.getConstant(VPERMILPMask, MVT::i8));
  }

  // X86 has dedicated unpack instructions that can handle specific blend
  // operations: UNPCKH and UNPCKL.
  if (isShuffleEquivalent(Mask, 0, 4, 2, 6))
    return DAG.getNode(X86ISD::UNPCKL, DL, MVT::v4f64, V1, V2);
  if (isShuffleEquivalent(Mask, 1, 5, 3, 7))
    return DAG.getNode(X86ISD::UNPCKH, DL, MVT::v4f64, V1, V2);
  // FIXME: It would be nice to find a way to get canonicalization to commute
  // these patterns.
  if (isShuffleEquivalent(Mask, 4, 0, 6, 2))
    return DAG.getNode(X86ISD::UNPCKL, DL, MVT::v4f64, V2, V1);
  if (isShuffleEquivalent(Mask, 5, 1, 7, 3))
    return DAG.getNode(X86ISD::UNPCKH, DL, MVT::v4f64, V2, V1);

  // Check if the blend happens to exactly fit that of SHUFPD.
  if (Mask[0] < 4 && (Mask[1] == -1 || Mask[1] >= 4) &&
      Mask[2] < 4 && (Mask[3] == -1 || Mask[3] >= 4)) {
    unsigned SHUFPDMask = (Mask[0] == 1) | ((Mask[1] == 5) << 1) |
                          ((Mask[2] == 3) << 2) | ((Mask[3] == 7) << 3);
    return DAG.getNode(X86ISD::SHUFP, DL, MVT::v4f64, V1, V2,
                       DAG.getConstant(SHUFPDMask, MVT::i8));
  }
  if ((Mask[0] == -1 || Mask[0] >= 4) && Mask[1] < 4 &&
      (Mask[2] == -1 || Mask[2] >= 4) && Mask[3] < 4) {
    unsigned SHUFPDMask = (Mask[0] == 5) | ((Mask[1] == 1) << 1) |
                          ((Mask[2] == 7) << 2) | ((Mask[3] == 3) << 3);
    return DAG.getNode(X86ISD::SHUFP, DL, MVT::v4f64, V2, V1,
                       DAG.getConstant(SHUFPDMask, MVT::i8));
  }

  // Shuffle the input elements into the desired positions in V1 and V2 and
  // blend them together.
  int V1Mask[] = {-1, -1, -1, -1};
  int V2Mask[] = {-1, -1, -1, -1};
  for (int i = 0; i < 4; ++i)
    if (Mask[i] >= 0 && Mask[i] < 4)
      V1Mask[i] = Mask[i];
    else if (Mask[i] >= 4)
      V2Mask[i] = Mask[i] - 4;

  V1 = DAG.getVectorShuffle(MVT::v4f64, DL, V1, DAG.getUNDEF(MVT::v4f64), V1Mask);
  V2 = DAG.getVectorShuffle(MVT::v4f64, DL, V2, DAG.getUNDEF(MVT::v4f64), V2Mask);

  unsigned BlendMask = 0;
  for (int i = 0; i < 4; ++i)
    if (Mask[i] >= 4)
      BlendMask |= 1 << i;

  return DAG.getNode(X86ISD::BLENDI, DL, MVT::v4f64, V1, V2,
                     DAG.getConstant(BlendMask, MVT::i8));
}

/// \brief Handle lowering of 4-lane 64-bit integer shuffles.
///
/// Largely delegates to common code when we have AVX2 and to the floating-point
/// code when we only have AVX.
static SDValue lowerV4I64VectorShuffle(SDValue Op, SDValue V1, SDValue V2,
                                       const X86Subtarget *Subtarget,
                                       SelectionDAG &DAG) {
  SDLoc DL(Op);
  assert(Op.getSimpleValueType() == MVT::v4i64 && "Bad shuffle type!");
  assert(V1.getSimpleValueType() == MVT::v4i64 && "Bad operand type!");
  assert(V2.getSimpleValueType() == MVT::v4i64 && "Bad operand type!");
  ShuffleVectorSDNode *SVOp = cast<ShuffleVectorSDNode>(Op);
  ArrayRef<int> Mask = SVOp->getMask();
  assert(Mask.size() == 4 && "Unexpected mask size for v4 shuffle!");

  // FIXME: If we have AVX2, we should delegate to generic code as crossing
  // shuffles aren't a problem and FP and int have the same patterns.

  if (isHalfCrossingShuffleMask(Mask))
    return splitAndLower256BitVectorShuffle(Op, V1, V2, Subtarget, DAG);

  // AVX1 doesn't provide any facilities for v4i64 shuffles, bitcast and
  // delegate to floating point code.
  V1 = DAG.getNode(ISD::BITCAST, DL, MVT::v4f64, V1);
  V2 = DAG.getNode(ISD::BITCAST, DL, MVT::v4f64, V2);
  return DAG.getNode(ISD::BITCAST, DL, MVT::v4i64,
                     lowerV4F64VectorShuffle(Op, V1, V2, Subtarget, DAG));
}

/// \brief High-level routine to lower various 256-bit x86 vector shuffles.
///
/// This routine either breaks down the specific type of a 256-bit x86 vector
/// shuffle or splits it into two 128-bit shuffles and fuses the results back
/// together based on the available instructions.
static SDValue lower256BitVectorShuffle(SDValue Op, SDValue V1, SDValue V2,
                                        MVT VT, const X86Subtarget *Subtarget,
                                        SelectionDAG &DAG) {
  switch (VT.SimpleTy) {
  case MVT::v4f64:
    return lowerV4F64VectorShuffle(Op, V1, V2, Subtarget, DAG);
  case MVT::v4i64:
    return lowerV4I64VectorShuffle(Op, V1, V2, Subtarget, DAG);
  case MVT::v8i32:
  case MVT::v8f32:
  case MVT::v16i16:
  case MVT::v32i8:
    // Fall back to the basic pattern of extracting the high half and forming
    // a 4-way blend.
    // FIXME: Add targeted lowering for each type that can document rationale
    // for delegating to this when necessary.
    return splitAndLower256BitVectorShuffle(Op, V1, V2, Subtarget, DAG);

  default:
    llvm_unreachable("Not a valid 256-bit x86 vector type!");
  }
}

/// \brief Tiny helper function to test whether a shuffle mask could be
/// simplified by widening the elements being shuffled.
static bool canWidenShuffleElements(ArrayRef<int> Mask) {
  for (int i = 0, Size = Mask.size(); i < Size; i += 2)
    if (Mask[i] % 2 != 0 || Mask[i] + 1 != Mask[i+1])
      return false;

  return true;
}

/// \brief Top-level lowering for x86 vector shuffles.
///
/// This handles decomposition, canonicalization, and lowering of all x86
/// vector shuffles. Most of the specific lowering strategies are encapsulated
/// above in helper routines. The canonicalization attempts to widen shuffles
/// to involve fewer lanes of wider elements, consolidate symmetric patterns
/// s.t. only one of the two inputs needs to be tested, etc.
static SDValue lowerVectorShuffle(SDValue Op, const X86Subtarget *Subtarget,
                                  SelectionDAG &DAG) {
  ShuffleVectorSDNode *SVOp = cast<ShuffleVectorSDNode>(Op);
  ArrayRef<int> Mask = SVOp->getMask();
  SDValue V1 = Op.getOperand(0);
  SDValue V2 = Op.getOperand(1);
  MVT VT = Op.getSimpleValueType();
  int NumElements = VT.getVectorNumElements();
  SDLoc dl(Op);

  assert(VT.getSizeInBits() != 64 && "Can't lower MMX shuffles");

  bool V1IsUndef = V1.getOpcode() == ISD::UNDEF;
  bool V2IsUndef = V2.getOpcode() == ISD::UNDEF;
  if (V1IsUndef && V2IsUndef)
    return DAG.getUNDEF(VT);

  // When we create a shuffle node we put the UNDEF node to second operand,
  // but in some cases the first operand may be transformed to UNDEF.
  // In this case we should just commute the node.
  if (V1IsUndef)
    return DAG.getCommutedVectorShuffle(*SVOp);

  // Check for non-undef masks pointing at an undef vector and make the masks
  // undef as well. This makes it easier to match the shuffle based solely on
  // the mask.
  if (V2IsUndef)
    for (int M : Mask)
      if (M >= NumElements) {
        SmallVector<int, 8> NewMask(Mask.begin(), Mask.end());
        for (int &M : NewMask)
          if (M >= NumElements)
            M = -1;
        return DAG.getVectorShuffle(VT, dl, V1, V2, NewMask);
      }

  // For integer vector shuffles, try to collapse them into a shuffle of fewer
  // lanes but wider integers. We cap this to not form integers larger than i64
  // but it might be interesting to form i128 integers to handle flipping the
  // low and high halves of AVX 256-bit vectors.
  if (VT.isInteger() && VT.getScalarSizeInBits() < 64 &&
      canWidenShuffleElements(Mask)) {
    SmallVector<int, 8> NewMask;
    for (int i = 0, Size = Mask.size(); i < Size; i += 2)
      NewMask.push_back(Mask[i] / 2);
    MVT NewVT =
        MVT::getVectorVT(MVT::getIntegerVT(VT.getScalarSizeInBits() * 2),
                         VT.getVectorNumElements() / 2);
    V1 = DAG.getNode(ISD::BITCAST, dl, NewVT, V1);
    V2 = DAG.getNode(ISD::BITCAST, dl, NewVT, V2);
    return DAG.getNode(ISD::BITCAST, dl, VT,
                       DAG.getVectorShuffle(NewVT, dl, V1, V2, NewMask));
  }

  int NumV1Elements = 0, NumUndefElements = 0, NumV2Elements = 0;
  for (int M : SVOp->getMask())
    if (M < 0)
      ++NumUndefElements;
    else if (M < NumElements)
      ++NumV1Elements;
    else
      ++NumV2Elements;

  // Commute the shuffle as needed such that more elements come from V1 than
  // V2. This allows us to match the shuffle pattern strictly on how many
  // elements come from V1 without handling the symmetric cases.
  if (NumV2Elements > NumV1Elements)
    return DAG.getCommutedVectorShuffle(*SVOp);

  // When the number of V1 and V2 elements are the same, try to minimize the
  // number of uses of V2 in the low half of the vector.
  if (NumV1Elements == NumV2Elements) {
    int LowV1Elements = 0, LowV2Elements = 0;
    for (int M : SVOp->getMask().slice(0, NumElements / 2))
      if (M >= NumElements)
        ++LowV2Elements;
      else if (M >= 0)
        ++LowV1Elements;
    if (LowV2Elements > LowV1Elements)
      return DAG.getCommutedVectorShuffle(*SVOp);
  }

  // For each vector width, delegate to a specialized lowering routine.
  if (VT.getSizeInBits() == 128)
    return lower128BitVectorShuffle(Op, V1, V2, VT, Subtarget, DAG);

  if (VT.getSizeInBits() == 256)
    return lower256BitVectorShuffle(Op, V1, V2, VT, Subtarget, DAG);

  llvm_unreachable("Unimplemented!");
}


//===----------------------------------------------------------------------===//
// Legacy vector shuffle lowering
//
// This code is the legacy code handling vector shuffles until the above
// replaces its functionality and performance.
//===----------------------------------------------------------------------===//

static bool isBlendMask(ArrayRef<int> MaskVals, MVT VT, bool hasSSE41,
                        bool hasInt256, unsigned *MaskOut = nullptr) {
  MVT EltVT = VT.getVectorElementType();

  // There is no blend with immediate in AVX-512.
  if (VT.is512BitVector())
    return false;

  if (!hasSSE41 || EltVT == MVT::i8)
    return false;
  if (!hasInt256 && VT == MVT::v16i16)
    return false;

  unsigned MaskValue = 0;
  unsigned NumElems = VT.getVectorNumElements();
  // There are 2 lanes if (NumElems > 8), and 1 lane otherwise.
  unsigned NumLanes = (NumElems - 1) / 8 + 1;
  unsigned NumElemsInLane = NumElems / NumLanes;

  // Blend for v16i16 should be symetric for the both lanes.
  for (unsigned i = 0; i < NumElemsInLane; ++i) {

    int SndLaneEltIdx = (NumLanes == 2) ? MaskVals[i + NumElemsInLane] : -1;
    int EltIdx = MaskVals[i];

    if ((EltIdx < 0 || EltIdx == (int)i) &&
        (SndLaneEltIdx < 0 || SndLaneEltIdx == (int)(i + NumElemsInLane)))
      continue;

    if (((unsigned)EltIdx == (i + NumElems)) &&
        (SndLaneEltIdx < 0 ||
         (unsigned)SndLaneEltIdx == i + NumElems + NumElemsInLane))
      MaskValue |= (1 << i);
    else
      return false;
  }

  if (MaskOut)
    *MaskOut = MaskValue;
  return true;
}

// Try to lower a shuffle node into a simple blend instruction.
// This function assumes isBlendMask returns true for this
// SuffleVectorSDNode
static SDValue LowerVECTOR_SHUFFLEtoBlend(ShuffleVectorSDNode *SVOp,
                                          unsigned MaskValue,
                                          const X86Subtarget *Subtarget,
                                          SelectionDAG &DAG) {
  MVT VT = SVOp->getSimpleValueType(0);
  MVT EltVT = VT.getVectorElementType();
  assert(isBlendMask(SVOp->getMask(), VT, Subtarget->hasSSE41(),
                     Subtarget->hasInt256() && "Trying to lower a "
                                               "VECTOR_SHUFFLE to a Blend but "
                                               "with the wrong mask"));
  SDValue V1 = SVOp->getOperand(0);
  SDValue V2 = SVOp->getOperand(1);
  SDLoc dl(SVOp);
  unsigned NumElems = VT.getVectorNumElements();

  // Convert i32 vectors to floating point if it is not AVX2.
  // AVX2 introduced VPBLENDD instruction for 128 and 256-bit vectors.
  MVT BlendVT = VT;
  if (EltVT == MVT::i64 || (EltVT == MVT::i32 && !Subtarget->hasInt256())) {
    BlendVT = MVT::getVectorVT(MVT::getFloatingPointVT(EltVT.getSizeInBits()),
                               NumElems);
    V1 = DAG.getNode(ISD::BITCAST, dl, VT, V1);
    V2 = DAG.getNode(ISD::BITCAST, dl, VT, V2);
  }

  SDValue Ret = DAG.getNode(X86ISD::BLENDI, dl, BlendVT, V1, V2,
                            DAG.getConstant(MaskValue, MVT::i32));
  return DAG.getNode(ISD::BITCAST, dl, VT, Ret);
}

/// In vector type \p VT, return true if the element at index \p InputIdx
/// falls on a different 128-bit lane than \p OutputIdx.
static bool ShuffleCrosses128bitLane(MVT VT, unsigned InputIdx,
                                     unsigned OutputIdx) {
  unsigned EltSize = VT.getVectorElementType().getSizeInBits();
  return InputIdx * EltSize / 128 != OutputIdx * EltSize / 128;
}

/// Generate a PSHUFB if possible.  Selects elements from \p V1 according to
/// \p MaskVals.  MaskVals[OutputIdx] = InputIdx specifies that we want to
/// shuffle the element at InputIdx in V1 to OutputIdx in the result.  If \p
/// MaskVals refers to elements outside of \p V1 or is undef (-1), insert a
/// zero.
static SDValue getPSHUFB(ArrayRef<int> MaskVals, SDValue V1, SDLoc &dl,
                         SelectionDAG &DAG) {
  MVT VT = V1.getSimpleValueType();
  assert(VT.is128BitVector() || VT.is256BitVector());

  MVT EltVT = VT.getVectorElementType();
  unsigned EltSizeInBytes = EltVT.getSizeInBits() / 8;
  unsigned NumElts = VT.getVectorNumElements();

  SmallVector<SDValue, 32> PshufbMask;
  for (unsigned OutputIdx = 0; OutputIdx < NumElts; ++OutputIdx) {
    int InputIdx = MaskVals[OutputIdx];
    unsigned InputByteIdx;

    if (InputIdx < 0 || NumElts <= (unsigned)InputIdx)
      InputByteIdx = 0x80;
    else {
      // Cross lane is not allowed.
      if (ShuffleCrosses128bitLane(VT, InputIdx, OutputIdx))
        return SDValue();
      InputByteIdx = InputIdx * EltSizeInBytes;
      // Index is an byte offset within the 128-bit lane.
      InputByteIdx &= 0xf;
    }

    for (unsigned j = 0; j < EltSizeInBytes; ++j) {
      PshufbMask.push_back(DAG.getConstant(InputByteIdx, MVT::i8));
      if (InputByteIdx != 0x80)
        ++InputByteIdx;
    }
  }

  MVT ShufVT = MVT::getVectorVT(MVT::i8, PshufbMask.size());
  if (ShufVT != VT)
    V1 = DAG.getNode(ISD::BITCAST, dl, ShufVT, V1);
  return DAG.getNode(X86ISD::PSHUFB, dl, ShufVT, V1,
                     DAG.getNode(ISD::BUILD_VECTOR, dl, ShufVT, PshufbMask));
}

// v8i16 shuffles - Prefer shuffles in the following order:
// 1. [all]   pshuflw, pshufhw, optional move
// 2. [ssse3] 1 x pshufb
// 3. [ssse3] 2 x pshufb + 1 x por
// 4. [all]   mov + pshuflw + pshufhw + N x (pextrw + pinsrw)
static SDValue
LowerVECTOR_SHUFFLEv8i16(SDValue Op, const X86Subtarget *Subtarget,
                         SelectionDAG &DAG) {
  ShuffleVectorSDNode *SVOp = cast<ShuffleVectorSDNode>(Op);
  SDValue V1 = SVOp->getOperand(0);
  SDValue V2 = SVOp->getOperand(1);
  SDLoc dl(SVOp);
  SmallVector<int, 8> MaskVals;

  // Determine if more than 1 of the words in each of the low and high quadwords
  // of the result come from the same quadword of one of the two inputs.  Undef
  // mask values count as coming from any quadword, for better codegen.
  //
  // Lo/HiQuad[i] = j indicates how many words from the ith quad of the input
  // feeds this quad.  For i, 0 and 1 refer to V1, 2 and 3 refer to V2.
  unsigned LoQuad[] = { 0, 0, 0, 0 };
  unsigned HiQuad[] = { 0, 0, 0, 0 };
  // Indices of quads used.
  std::bitset<4> InputQuads;
  for (unsigned i = 0; i < 8; ++i) {
    unsigned *Quad = i < 4 ? LoQuad : HiQuad;
    int EltIdx = SVOp->getMaskElt(i);
    MaskVals.push_back(EltIdx);
    if (EltIdx < 0) {
      ++Quad[0];
      ++Quad[1];
      ++Quad[2];
      ++Quad[3];
      continue;
    }
    ++Quad[EltIdx / 4];
    InputQuads.set(EltIdx / 4);
  }

  int BestLoQuad = -1;
  unsigned MaxQuad = 1;
  for (unsigned i = 0; i < 4; ++i) {
    if (LoQuad[i] > MaxQuad) {
      BestLoQuad = i;
      MaxQuad = LoQuad[i];
    }
  }

  int BestHiQuad = -1;
  MaxQuad = 1;
  for (unsigned i = 0; i < 4; ++i) {
    if (HiQuad[i] > MaxQuad) {
      BestHiQuad = i;
      MaxQuad = HiQuad[i];
    }
  }

  // For SSSE3, If all 8 words of the result come from only 1 quadword of each
  // of the two input vectors, shuffle them into one input vector so only a
  // single pshufb instruction is necessary. If there are more than 2 input
  // quads, disable the next transformation since it does not help SSSE3.
  bool V1Used = InputQuads[0] || InputQuads[1];
  bool V2Used = InputQuads[2] || InputQuads[3];
  if (Subtarget->hasSSSE3()) {
    if (InputQuads.count() == 2 && V1Used && V2Used) {
      BestLoQuad = InputQuads[0] ? 0 : 1;
      BestHiQuad = InputQuads[2] ? 2 : 3;
    }
    if (InputQuads.count() > 2) {
      BestLoQuad = -1;
      BestHiQuad = -1;
    }
  }

  // If BestLoQuad or BestHiQuad are set, shuffle the quads together and update
  // the shuffle mask.  If a quad is scored as -1, that means that it contains
  // words from all 4 input quadwords.
  SDValue NewV;
  if (BestLoQuad >= 0 || BestHiQuad >= 0) {
    int MaskV[] = {
      BestLoQuad < 0 ? 0 : BestLoQuad,
      BestHiQuad < 0 ? 1 : BestHiQuad
    };
    NewV = DAG.getVectorShuffle(MVT::v2i64, dl,
                  DAG.getNode(ISD::BITCAST, dl, MVT::v2i64, V1),
                  DAG.getNode(ISD::BITCAST, dl, MVT::v2i64, V2), &MaskV[0]);
    NewV = DAG.getNode(ISD::BITCAST, dl, MVT::v8i16, NewV);

    // Rewrite the MaskVals and assign NewV to V1 if NewV now contains all the
    // source words for the shuffle, to aid later transformations.
    bool AllWordsInNewV = true;
    bool InOrder[2] = { true, true };
    for (unsigned i = 0; i != 8; ++i) {
      int idx = MaskVals[i];
      if (idx != (int)i)
        InOrder[i/4] = false;
      if (idx < 0 || (idx/4) == BestLoQuad || (idx/4) == BestHiQuad)
        continue;
      AllWordsInNewV = false;
      break;
    }

    bool pshuflw = AllWordsInNewV, pshufhw = AllWordsInNewV;
    if (AllWordsInNewV) {
      for (int i = 0; i != 8; ++i) {
        int idx = MaskVals[i];
        if (idx < 0)
          continue;
        idx = MaskVals[i] = (idx / 4) == BestLoQuad ? (idx & 3) : (idx & 3) + 4;
        if ((idx != i) && idx < 4)
          pshufhw = false;
        if ((idx != i) && idx > 3)
          pshuflw = false;
      }
      V1 = NewV;
      V2Used = false;
      BestLoQuad = 0;
      BestHiQuad = 1;
    }

    // If we've eliminated the use of V2, and the new mask is a pshuflw or
    // pshufhw, that's as cheap as it gets.  Return the new shuffle.
    if ((pshufhw && InOrder[0]) || (pshuflw && InOrder[1])) {
      unsigned Opc = pshufhw ? X86ISD::PSHUFHW : X86ISD::PSHUFLW;
      unsigned TargetMask = 0;
      NewV = DAG.getVectorShuffle(MVT::v8i16, dl, NewV,
                                  DAG.getUNDEF(MVT::v8i16), &MaskVals[0]);
      ShuffleVectorSDNode *SVOp = cast<ShuffleVectorSDNode>(NewV.getNode());
      TargetMask = pshufhw ? getShufflePSHUFHWImmediate(SVOp):
                             getShufflePSHUFLWImmediate(SVOp);
      V1 = NewV.getOperand(0);
      return getTargetShuffleNode(Opc, dl, MVT::v8i16, V1, TargetMask, DAG);
    }
  }

  // Promote splats to a larger type which usually leads to more efficient code.
  // FIXME: Is this true if pshufb is available?
  if (SVOp->isSplat())
    return PromoteSplat(SVOp, DAG);

  // If we have SSSE3, and all words of the result are from 1 input vector,
  // case 2 is generated, otherwise case 3 is generated.  If no SSSE3
  // is present, fall back to case 4.
  if (Subtarget->hasSSSE3()) {
    SmallVector<SDValue,16> pshufbMask;

    // If we have elements from both input vectors, set the high bit of the
    // shuffle mask element to zero out elements that come from V2 in the V1
    // mask, and elements that come from V1 in the V2 mask, so that the two
    // results can be OR'd together.
    bool TwoInputs = V1Used && V2Used;
    V1 = getPSHUFB(MaskVals, V1, dl, DAG);
    if (!TwoInputs)
      return DAG.getNode(ISD::BITCAST, dl, MVT::v8i16, V1);

    // Calculate the shuffle mask for the second input, shuffle it, and
    // OR it with the first shuffled input.
    CommuteVectorShuffleMask(MaskVals, 8);
    V2 = getPSHUFB(MaskVals, V2, dl, DAG);
    V1 = DAG.getNode(ISD::OR, dl, MVT::v16i8, V1, V2);
    return DAG.getNode(ISD::BITCAST, dl, MVT::v8i16, V1);
  }

  // If BestLoQuad >= 0, generate a pshuflw to put the low elements in order,
  // and update MaskVals with new element order.
  std::bitset<8> InOrder;
  if (BestLoQuad >= 0) {
    int MaskV[] = { -1, -1, -1, -1, 4, 5, 6, 7 };
    for (int i = 0; i != 4; ++i) {
      int idx = MaskVals[i];
      if (idx < 0) {
        InOrder.set(i);
      } else if ((idx / 4) == BestLoQuad) {
        MaskV[i] = idx & 3;
        InOrder.set(i);
      }
    }
    NewV = DAG.getVectorShuffle(MVT::v8i16, dl, NewV, DAG.getUNDEF(MVT::v8i16),
                                &MaskV[0]);

    if (NewV.getOpcode() == ISD::VECTOR_SHUFFLE && Subtarget->hasSSE2()) {
      ShuffleVectorSDNode *SVOp = cast<ShuffleVectorSDNode>(NewV.getNode());
      NewV = getTargetShuffleNode(X86ISD::PSHUFLW, dl, MVT::v8i16,
                                  NewV.getOperand(0),
                                  getShufflePSHUFLWImmediate(SVOp), DAG);
    }
  }

  // If BestHi >= 0, generate a pshufhw to put the high elements in order,
  // and update MaskVals with the new element order.
  if (BestHiQuad >= 0) {
    int MaskV[] = { 0, 1, 2, 3, -1, -1, -1, -1 };
    for (unsigned i = 4; i != 8; ++i) {
      int idx = MaskVals[i];
      if (idx < 0) {
        InOrder.set(i);
      } else if ((idx / 4) == BestHiQuad) {
        MaskV[i] = (idx & 3) + 4;
        InOrder.set(i);
      }
    }
    NewV = DAG.getVectorShuffle(MVT::v8i16, dl, NewV, DAG.getUNDEF(MVT::v8i16),
                                &MaskV[0]);

    if (NewV.getOpcode() == ISD::VECTOR_SHUFFLE && Subtarget->hasSSE2()) {
      ShuffleVectorSDNode *SVOp = cast<ShuffleVectorSDNode>(NewV.getNode());
      NewV = getTargetShuffleNode(X86ISD::PSHUFHW, dl, MVT::v8i16,
                                  NewV.getOperand(0),
                                  getShufflePSHUFHWImmediate(SVOp), DAG);
    }
  }

  // In case BestHi & BestLo were both -1, which means each quadword has a word
  // from each of the four input quadwords, calculate the InOrder bitvector now
  // before falling through to the insert/extract cleanup.
  if (BestLoQuad == -1 && BestHiQuad == -1) {
    NewV = V1;
    for (int i = 0; i != 8; ++i)
      if (MaskVals[i] < 0 || MaskVals[i] == i)
        InOrder.set(i);
  }

  // The other elements are put in the right place using pextrw and pinsrw.
  for (unsigned i = 0; i != 8; ++i) {
    if (InOrder[i])
      continue;
    int EltIdx = MaskVals[i];
    if (EltIdx < 0)
      continue;
    SDValue ExtOp = (EltIdx < 8) ?
      DAG.getNode(ISD::EXTRACT_VECTOR_ELT, dl, MVT::i16, V1,
                  DAG.getIntPtrConstant(EltIdx)) :
      DAG.getNode(ISD::EXTRACT_VECTOR_ELT, dl, MVT::i16, V2,
                  DAG.getIntPtrConstant(EltIdx - 8));
    NewV = DAG.getNode(ISD::INSERT_VECTOR_ELT, dl, MVT::v8i16, NewV, ExtOp,
                       DAG.getIntPtrConstant(i));
  }
  return NewV;
}

/// \brief v16i16 shuffles
///
/// FIXME: We only support generation of a single pshufb currently.  We can
/// generalize the other applicable cases from LowerVECTOR_SHUFFLEv8i16 as
/// well (e.g 2 x pshufb + 1 x por).
static SDValue
LowerVECTOR_SHUFFLEv16i16(SDValue Op, SelectionDAG &DAG) {
  ShuffleVectorSDNode *SVOp = cast<ShuffleVectorSDNode>(Op);
  SDValue V1 = SVOp->getOperand(0);
  SDValue V2 = SVOp->getOperand(1);
  SDLoc dl(SVOp);

  if (V2.getOpcode() != ISD::UNDEF)
    return SDValue();

  SmallVector<int, 16> MaskVals(SVOp->getMask().begin(), SVOp->getMask().end());
  return getPSHUFB(MaskVals, V1, dl, DAG);
}

// v16i8 shuffles - Prefer shuffles in the following order:
// 1. [ssse3] 1 x pshufb
// 2. [ssse3] 2 x pshufb + 1 x por
// 3. [all]   v8i16 shuffle + N x pextrw + rotate + pinsrw
static SDValue LowerVECTOR_SHUFFLEv16i8(ShuffleVectorSDNode *SVOp,
                                        const X86Subtarget* Subtarget,
                                        SelectionDAG &DAG) {
  const TargetLowering &TLI = DAG.getTargetLoweringInfo();
  SDValue V1 = SVOp->getOperand(0);
  SDValue V2 = SVOp->getOperand(1);
  SDLoc dl(SVOp);
  ArrayRef<int> MaskVals = SVOp->getMask();

  // Promote splats to a larger type which usually leads to more efficient code.
  // FIXME: Is this true if pshufb is available?
  if (SVOp->isSplat())
    return PromoteSplat(SVOp, DAG);

  // If we have SSSE3, case 1 is generated when all result bytes come from
  // one of  the inputs.  Otherwise, case 2 is generated.  If no SSSE3 is
  // present, fall back to case 3.

  // If SSSE3, use 1 pshufb instruction per vector with elements in the result.
  if (Subtarget->hasSSSE3()) {
    SmallVector<SDValue,16> pshufbMask;

    // If all result elements are from one input vector, then only translate
    // undef mask values to 0x80 (zero out result) in the pshufb mask.
    //
    // Otherwise, we have elements from both input vectors, and must zero out
    // elements that come from V2 in the first mask, and V1 in the second mask
    // so that we can OR them together.
    for (unsigned i = 0; i != 16; ++i) {
      int EltIdx = MaskVals[i];
      if (EltIdx < 0 || EltIdx >= 16)
        EltIdx = 0x80;
      pshufbMask.push_back(DAG.getConstant(EltIdx, MVT::i8));
    }
    V1 = DAG.getNode(X86ISD::PSHUFB, dl, MVT::v16i8, V1,
                     DAG.getNode(ISD::BUILD_VECTOR, dl,
                                 MVT::v16i8, pshufbMask));

    // As PSHUFB will zero elements with negative indices, it's safe to ignore
    // the 2nd operand if it's undefined or zero.
    if (V2.getOpcode() == ISD::UNDEF ||
        ISD::isBuildVectorAllZeros(V2.getNode()))
      return V1;

    // Calculate the shuffle mask for the second input, shuffle it, and
    // OR it with the first shuffled input.
    pshufbMask.clear();
    for (unsigned i = 0; i != 16; ++i) {
      int EltIdx = MaskVals[i];
      EltIdx = (EltIdx < 16) ? 0x80 : EltIdx - 16;
      pshufbMask.push_back(DAG.getConstant(EltIdx, MVT::i8));
    }
    V2 = DAG.getNode(X86ISD::PSHUFB, dl, MVT::v16i8, V2,
                     DAG.getNode(ISD::BUILD_VECTOR, dl,
                                 MVT::v16i8, pshufbMask));
    return DAG.getNode(ISD::OR, dl, MVT::v16i8, V1, V2);
  }

  // No SSSE3 - Calculate in place words and then fix all out of place words
  // With 0-16 extracts & inserts.  Worst case is 16 bytes out of order from
  // the 16 different words that comprise the two doublequadword input vectors.
  V1 = DAG.getNode(ISD::BITCAST, dl, MVT::v8i16, V1);
  V2 = DAG.getNode(ISD::BITCAST, dl, MVT::v8i16, V2);
  SDValue NewV = V1;
  for (int i = 0; i != 8; ++i) {
    int Elt0 = MaskVals[i*2];
    int Elt1 = MaskVals[i*2+1];

    // This word of the result is all undef, skip it.
    if (Elt0 < 0 && Elt1 < 0)
      continue;

    // This word of the result is already in the correct place, skip it.
    if ((Elt0 == i*2) && (Elt1 == i*2+1))
      continue;

    SDValue Elt0Src = Elt0 < 16 ? V1 : V2;
    SDValue Elt1Src = Elt1 < 16 ? V1 : V2;
    SDValue InsElt;

    // If Elt0 and Elt1 are defined, are consecutive, and can be load
    // using a single extract together, load it and store it.
    if ((Elt0 >= 0) && ((Elt0 + 1) == Elt1) && ((Elt0 & 1) == 0)) {
      InsElt = DAG.getNode(ISD::EXTRACT_VECTOR_ELT, dl, MVT::i16, Elt1Src,
                           DAG.getIntPtrConstant(Elt1 / 2));
      NewV = DAG.getNode(ISD::INSERT_VECTOR_ELT, dl, MVT::v8i16, NewV, InsElt,
                        DAG.getIntPtrConstant(i));
      continue;
    }

    // If Elt1 is defined, extract it from the appropriate source.  If the
    // source byte is not also odd, shift the extracted word left 8 bits
    // otherwise clear the bottom 8 bits if we need to do an or.
    if (Elt1 >= 0) {
      InsElt = DAG.getNode(ISD::EXTRACT_VECTOR_ELT, dl, MVT::i16, Elt1Src,
                           DAG.getIntPtrConstant(Elt1 / 2));
      if ((Elt1 & 1) == 0)
        InsElt = DAG.getNode(ISD::SHL, dl, MVT::i16, InsElt,
                             DAG.getConstant(8,
                                  TLI.getShiftAmountTy(InsElt.getValueType())));
      else if (Elt0 >= 0)
        InsElt = DAG.getNode(ISD::AND, dl, MVT::i16, InsElt,
                             DAG.getConstant(0xFF00, MVT::i16));
    }
    // If Elt0 is defined, extract it from the appropriate source.  If the
    // source byte is not also even, shift the extracted word right 8 bits. If
    // Elt1 was also defined, OR the extracted values together before
    // inserting them in the result.
    if (Elt0 >= 0) {
      SDValue InsElt0 = DAG.getNode(ISD::EXTRACT_VECTOR_ELT, dl, MVT::i16,
                                    Elt0Src, DAG.getIntPtrConstant(Elt0 / 2));
      if ((Elt0 & 1) != 0)
        InsElt0 = DAG.getNode(ISD::SRL, dl, MVT::i16, InsElt0,
                              DAG.getConstant(8,
                                 TLI.getShiftAmountTy(InsElt0.getValueType())));
      else if (Elt1 >= 0)
        InsElt0 = DAG.getNode(ISD::AND, dl, MVT::i16, InsElt0,
                             DAG.getConstant(0x00FF, MVT::i16));
      InsElt = Elt1 >= 0 ? DAG.getNode(ISD::OR, dl, MVT::i16, InsElt, InsElt0)
                         : InsElt0;
    }
    NewV = DAG.getNode(ISD::INSERT_VECTOR_ELT, dl, MVT::v8i16, NewV, InsElt,
                       DAG.getIntPtrConstant(i));
  }
  return DAG.getNode(ISD::BITCAST, dl, MVT::v16i8, NewV);
}

// v32i8 shuffles - Translate to VPSHUFB if possible.
static
SDValue LowerVECTOR_SHUFFLEv32i8(ShuffleVectorSDNode *SVOp,
                                 const X86Subtarget *Subtarget,
                                 SelectionDAG &DAG) {
  MVT VT = SVOp->getSimpleValueType(0);
  SDValue V1 = SVOp->getOperand(0);
  SDValue V2 = SVOp->getOperand(1);
  SDLoc dl(SVOp);
  SmallVector<int, 32> MaskVals(SVOp->getMask().begin(), SVOp->getMask().end());

  bool V2IsUndef = V2.getOpcode() == ISD::UNDEF;
  bool V1IsAllZero = ISD::isBuildVectorAllZeros(V1.getNode());
  bool V2IsAllZero = ISD::isBuildVectorAllZeros(V2.getNode());

  // VPSHUFB may be generated if
  // (1) one of input vector is undefined or zeroinitializer.
  // The mask value 0x80 puts 0 in the corresponding slot of the vector.
  // And (2) the mask indexes don't cross the 128-bit lane.
  if (VT != MVT::v32i8 || !Subtarget->hasInt256() ||
      (!V2IsUndef && !V2IsAllZero && !V1IsAllZero))
    return SDValue();

  if (V1IsAllZero && !V2IsAllZero) {
    CommuteVectorShuffleMask(MaskVals, 32);
    V1 = V2;
  }
  return getPSHUFB(MaskVals, V1, dl, DAG);
}

/// RewriteAsNarrowerShuffle - Try rewriting v8i16 and v16i8 shuffles as 4 wide
/// ones, or rewriting v4i32 / v4f32 as 2 wide ones if possible. This can be
/// done when every pair / quad of shuffle mask elements point to elements in
/// the right sequence. e.g.
/// vector_shuffle X, Y, <2, 3, | 10, 11, | 0, 1, | 14, 15>
static
SDValue RewriteAsNarrowerShuffle(ShuffleVectorSDNode *SVOp,
                                 SelectionDAG &DAG) {
  MVT VT = SVOp->getSimpleValueType(0);
  SDLoc dl(SVOp);
  unsigned NumElems = VT.getVectorNumElements();
  MVT NewVT;
  unsigned Scale;
  switch (VT.SimpleTy) {
  default: llvm_unreachable("Unexpected!");
  case MVT::v2i64:
  case MVT::v2f64:
           return SDValue(SVOp, 0);
  case MVT::v4f32:  NewVT = MVT::v2f64; Scale = 2; break;
  case MVT::v4i32:  NewVT = MVT::v2i64; Scale = 2; break;
  case MVT::v8i16:  NewVT = MVT::v4i32; Scale = 2; break;
  case MVT::v16i8:  NewVT = MVT::v4i32; Scale = 4; break;
  case MVT::v16i16: NewVT = MVT::v8i32; Scale = 2; break;
  case MVT::v32i8:  NewVT = MVT::v8i32; Scale = 4; break;
  }

  SmallVector<int, 8> MaskVec;
  for (unsigned i = 0; i != NumElems; i += Scale) {
    int StartIdx = -1;
    for (unsigned j = 0; j != Scale; ++j) {
      int EltIdx = SVOp->getMaskElt(i+j);
      if (EltIdx < 0)
        continue;
      if (StartIdx < 0)
        StartIdx = (EltIdx / Scale);
      if (EltIdx != (int)(StartIdx*Scale + j))
        return SDValue();
    }
    MaskVec.push_back(StartIdx);
  }

  SDValue V1 = DAG.getNode(ISD::BITCAST, dl, NewVT, SVOp->getOperand(0));
  SDValue V2 = DAG.getNode(ISD::BITCAST, dl, NewVT, SVOp->getOperand(1));
  return DAG.getVectorShuffle(NewVT, dl, V1, V2, &MaskVec[0]);
}

/// getVZextMovL - Return a zero-extending vector move low node.
///
static SDValue getVZextMovL(MVT VT, MVT OpVT,
                            SDValue SrcOp, SelectionDAG &DAG,
                            const X86Subtarget *Subtarget, SDLoc dl) {
  if (VT == MVT::v2f64 || VT == MVT::v4f32) {
    LoadSDNode *LD = nullptr;
    if (!isScalarLoadToVector(SrcOp.getNode(), &LD))
      LD = dyn_cast<LoadSDNode>(SrcOp);
    if (!LD) {
      // movssrr and movsdrr do not clear top bits. Try to use movd, movq
      // instead.
      MVT ExtVT = (OpVT == MVT::v2f64) ? MVT::i64 : MVT::i32;
      if ((ExtVT != MVT::i64 || Subtarget->is64Bit()) &&
          SrcOp.getOpcode() == ISD::SCALAR_TO_VECTOR &&
          SrcOp.getOperand(0).getOpcode() == ISD::BITCAST &&
          SrcOp.getOperand(0).getOperand(0).getValueType() == ExtVT) {
        // PR2108
        OpVT = (OpVT == MVT::v2f64) ? MVT::v2i64 : MVT::v4i32;
        return DAG.getNode(ISD::BITCAST, dl, VT,
                           DAG.getNode(X86ISD::VZEXT_MOVL, dl, OpVT,
                                       DAG.getNode(ISD::SCALAR_TO_VECTOR, dl,
                                                   OpVT,
                                                   SrcOp.getOperand(0)
                                                          .getOperand(0))));
      }
    }
  }

  return DAG.getNode(ISD::BITCAST, dl, VT,
                     DAG.getNode(X86ISD::VZEXT_MOVL, dl, OpVT,
                                 DAG.getNode(ISD::BITCAST, dl,
                                             OpVT, SrcOp)));
}

/// LowerVECTOR_SHUFFLE_256 - Handle all 256-bit wide vectors shuffles
/// which could not be matched by any known target speficic shuffle
static SDValue
LowerVECTOR_SHUFFLE_256(ShuffleVectorSDNode *SVOp, SelectionDAG &DAG) {

  SDValue NewOp = Compact8x32ShuffleNode(SVOp, DAG);
  if (NewOp.getNode())
    return NewOp;

  MVT VT = SVOp->getSimpleValueType(0);

  unsigned NumElems = VT.getVectorNumElements();
  unsigned NumLaneElems = NumElems / 2;

  SDLoc dl(SVOp);
  MVT EltVT = VT.getVectorElementType();
  MVT NVT = MVT::getVectorVT(EltVT, NumLaneElems);
  SDValue Output[2];

  SmallVector<int, 16> Mask;
  for (unsigned l = 0; l < 2; ++l) {
    // Build a shuffle mask for the output, discovering on the fly which
    // input vectors to use as shuffle operands (recorded in InputUsed).
    // If building a suitable shuffle vector proves too hard, then bail
    // out with UseBuildVector set.
    bool UseBuildVector = false;
    int InputUsed[2] = { -1, -1 }; // Not yet discovered.
    unsigned LaneStart = l * NumLaneElems;
    for (unsigned i = 0; i != NumLaneElems; ++i) {
      // The mask element.  This indexes into the input.
      int Idx = SVOp->getMaskElt(i+LaneStart);
      if (Idx < 0) {
        // the mask element does not index into any input vector.
        Mask.push_back(-1);
        continue;
      }

      // The input vector this mask element indexes into.
      int Input = Idx / NumLaneElems;

      // Turn the index into an offset from the start of the input vector.
      Idx -= Input * NumLaneElems;

      // Find or create a shuffle vector operand to hold this input.
      unsigned OpNo;
      for (OpNo = 0; OpNo < array_lengthof(InputUsed); ++OpNo) {
        if (InputUsed[OpNo] == Input)
          // This input vector is already an operand.
          break;
        if (InputUsed[OpNo] < 0) {
          // Create a new operand for this input vector.
          InputUsed[OpNo] = Input;
          break;
        }
      }

      if (OpNo >= array_lengthof(InputUsed)) {
        // More than two input vectors used!  Give up on trying to create a
        // shuffle vector.  Insert all elements into a BUILD_VECTOR instead.
        UseBuildVector = true;
        break;
      }

      // Add the mask index for the new shuffle vector.
      Mask.push_back(Idx + OpNo * NumLaneElems);
    }

    if (UseBuildVector) {
      SmallVector<SDValue, 16> SVOps;
      for (unsigned i = 0; i != NumLaneElems; ++i) {
        // The mask element.  This indexes into the input.
        int Idx = SVOp->getMaskElt(i+LaneStart);
        if (Idx < 0) {
          SVOps.push_back(DAG.getUNDEF(EltVT));
          continue;
        }

        // The input vector this mask element indexes into.
        int Input = Idx / NumElems;

        // Turn the index into an offset from the start of the input vector.
        Idx -= Input * NumElems;

        // Extract the vector element by hand.
        SVOps.push_back(DAG.getNode(ISD::EXTRACT_VECTOR_ELT, dl, EltVT,
                                    SVOp->getOperand(Input),
                                    DAG.getIntPtrConstant(Idx)));
      }

      // Construct the output using a BUILD_VECTOR.
      Output[l] = DAG.getNode(ISD::BUILD_VECTOR, dl, NVT, SVOps);
    } else if (InputUsed[0] < 0) {
      // No input vectors were used! The result is undefined.
      Output[l] = DAG.getUNDEF(NVT);
    } else {
      SDValue Op0 = Extract128BitVector(SVOp->getOperand(InputUsed[0] / 2),
                                        (InputUsed[0] % 2) * NumLaneElems,
                                        DAG, dl);
      // If only one input was used, use an undefined vector for the other.
      SDValue Op1 = (InputUsed[1] < 0) ? DAG.getUNDEF(NVT) :
        Extract128BitVector(SVOp->getOperand(InputUsed[1] / 2),
                            (InputUsed[1] % 2) * NumLaneElems, DAG, dl);
      // At least one input vector was used. Create a new shuffle vector.
      Output[l] = DAG.getVectorShuffle(NVT, dl, Op0, Op1, &Mask[0]);
    }

    Mask.clear();
  }

  // Concatenate the result back
  return DAG.getNode(ISD::CONCAT_VECTORS, dl, VT, Output[0], Output[1]);
}

/// LowerVECTOR_SHUFFLE_128v4 - Handle all 128-bit wide vectors with
/// 4 elements, and match them with several different shuffle types.
static SDValue
LowerVECTOR_SHUFFLE_128v4(ShuffleVectorSDNode *SVOp, SelectionDAG &DAG) {
  SDValue V1 = SVOp->getOperand(0);
  SDValue V2 = SVOp->getOperand(1);
  SDLoc dl(SVOp);
  MVT VT = SVOp->getSimpleValueType(0);

  assert(VT.is128BitVector() && "Unsupported vector size");

  std::pair<int, int> Locs[4];
  int Mask1[] = { -1, -1, -1, -1 };
  SmallVector<int, 8> PermMask(SVOp->getMask().begin(), SVOp->getMask().end());

  unsigned NumHi = 0;
  unsigned NumLo = 0;
  for (unsigned i = 0; i != 4; ++i) {
    int Idx = PermMask[i];
    if (Idx < 0) {
      Locs[i] = std::make_pair(-1, -1);
    } else {
      assert(Idx < 8 && "Invalid VECTOR_SHUFFLE index!");
      if (Idx < 4) {
        Locs[i] = std::make_pair(0, NumLo);
        Mask1[NumLo] = Idx;
        NumLo++;
      } else {
        Locs[i] = std::make_pair(1, NumHi);
        if (2+NumHi < 4)
          Mask1[2+NumHi] = Idx;
        NumHi++;
      }
    }
  }

  if (NumLo <= 2 && NumHi <= 2) {
    // If no more than two elements come from either vector. This can be
    // implemented with two shuffles. First shuffle gather the elements.
    // The second shuffle, which takes the first shuffle as both of its
    // vector operands, put the elements into the right order.
    V1 = DAG.getVectorShuffle(VT, dl, V1, V2, &Mask1[0]);

    int Mask2[] = { -1, -1, -1, -1 };

    for (unsigned i = 0; i != 4; ++i)
      if (Locs[i].first != -1) {
        unsigned Idx = (i < 2) ? 0 : 4;
        Idx += Locs[i].first * 2 + Locs[i].second;
        Mask2[i] = Idx;
      }

    return DAG.getVectorShuffle(VT, dl, V1, V1, &Mask2[0]);
  }

  if (NumLo == 3 || NumHi == 3) {
    // Otherwise, we must have three elements from one vector, call it X, and
    // one element from the other, call it Y.  First, use a shufps to build an
    // intermediate vector with the one element from Y and the element from X
    // that will be in the same half in the final destination (the indexes don't
    // matter). Then, use a shufps to build the final vector, taking the half
    // containing the element from Y from the intermediate, and the other half
    // from X.
    if (NumHi == 3) {
      // Normalize it so the 3 elements come from V1.
      CommuteVectorShuffleMask(PermMask, 4);
      std::swap(V1, V2);
    }

    // Find the element from V2.
    unsigned HiIndex;
    for (HiIndex = 0; HiIndex < 3; ++HiIndex) {
      int Val = PermMask[HiIndex];
      if (Val < 0)
        continue;
      if (Val >= 4)
        break;
    }

    Mask1[0] = PermMask[HiIndex];
    Mask1[1] = -1;
    Mask1[2] = PermMask[HiIndex^1];
    Mask1[3] = -1;
    V2 = DAG.getVectorShuffle(VT, dl, V1, V2, &Mask1[0]);

    if (HiIndex >= 2) {
      Mask1[0] = PermMask[0];
      Mask1[1] = PermMask[1];
      Mask1[2] = HiIndex & 1 ? 6 : 4;
      Mask1[3] = HiIndex & 1 ? 4 : 6;
      return DAG.getVectorShuffle(VT, dl, V1, V2, &Mask1[0]);
    }

    Mask1[0] = HiIndex & 1 ? 2 : 0;
    Mask1[1] = HiIndex & 1 ? 0 : 2;
    Mask1[2] = PermMask[2];
    Mask1[3] = PermMask[3];
    if (Mask1[2] >= 0)
      Mask1[2] += 4;
    if (Mask1[3] >= 0)
      Mask1[3] += 4;
    return DAG.getVectorShuffle(VT, dl, V2, V1, &Mask1[0]);
  }

  // Break it into (shuffle shuffle_hi, shuffle_lo).
  int LoMask[] = { -1, -1, -1, -1 };
  int HiMask[] = { -1, -1, -1, -1 };

  int *MaskPtr = LoMask;
  unsigned MaskIdx = 0;
  unsigned LoIdx = 0;
  unsigned HiIdx = 2;
  for (unsigned i = 0; i != 4; ++i) {
    if (i == 2) {
      MaskPtr = HiMask;
      MaskIdx = 1;
      LoIdx = 0;
      HiIdx = 2;
    }
    int Idx = PermMask[i];
    if (Idx < 0) {
      Locs[i] = std::make_pair(-1, -1);
    } else if (Idx < 4) {
      Locs[i] = std::make_pair(MaskIdx, LoIdx);
      MaskPtr[LoIdx] = Idx;
      LoIdx++;
    } else {
      Locs[i] = std::make_pair(MaskIdx, HiIdx);
      MaskPtr[HiIdx] = Idx;
      HiIdx++;
    }
  }

  SDValue LoShuffle = DAG.getVectorShuffle(VT, dl, V1, V2, &LoMask[0]);
  SDValue HiShuffle = DAG.getVectorShuffle(VT, dl, V1, V2, &HiMask[0]);
  int MaskOps[] = { -1, -1, -1, -1 };
  for (unsigned i = 0; i != 4; ++i)
    if (Locs[i].first != -1)
      MaskOps[i] = Locs[i].first * 4 + Locs[i].second;
  return DAG.getVectorShuffle(VT, dl, LoShuffle, HiShuffle, &MaskOps[0]);
}

static bool MayFoldVectorLoad(SDValue V) {
  while (V.hasOneUse() && V.getOpcode() == ISD::BITCAST)
    V = V.getOperand(0);

  if (V.hasOneUse() && V.getOpcode() == ISD::SCALAR_TO_VECTOR)
    V = V.getOperand(0);
  if (V.hasOneUse() && V.getOpcode() == ISD::BUILD_VECTOR &&
      V.getNumOperands() == 2 && V.getOperand(1).getOpcode() == ISD::UNDEF)
    // BUILD_VECTOR (load), undef
    V = V.getOperand(0);

  return MayFoldLoad(V);
}

static
SDValue getMOVDDup(SDValue &Op, SDLoc &dl, SDValue V1, SelectionDAG &DAG) {
  MVT VT = Op.getSimpleValueType();

  // Canonizalize to v2f64.
  V1 = DAG.getNode(ISD::BITCAST, dl, MVT::v2f64, V1);
  return DAG.getNode(ISD::BITCAST, dl, VT,
                     getTargetShuffleNode(X86ISD::MOVDDUP, dl, MVT::v2f64,
                                          V1, DAG));
}

static
SDValue getMOVLowToHigh(SDValue &Op, SDLoc &dl, SelectionDAG &DAG,
                        bool HasSSE2) {
  SDValue V1 = Op.getOperand(0);
  SDValue V2 = Op.getOperand(1);
  MVT VT = Op.getSimpleValueType();

  assert(VT != MVT::v2i64 && "unsupported shuffle type");

  if (HasSSE2 && VT == MVT::v2f64)
    return getTargetShuffleNode(X86ISD::MOVLHPD, dl, VT, V1, V2, DAG);

  // v4f32 or v4i32: canonizalized to v4f32 (which is legal for SSE1)
  return DAG.getNode(ISD::BITCAST, dl, VT,
                     getTargetShuffleNode(X86ISD::MOVLHPS, dl, MVT::v4f32,
                           DAG.getNode(ISD::BITCAST, dl, MVT::v4f32, V1),
                           DAG.getNode(ISD::BITCAST, dl, MVT::v4f32, V2), DAG));
}

static
SDValue getMOVHighToLow(SDValue &Op, SDLoc &dl, SelectionDAG &DAG) {
  SDValue V1 = Op.getOperand(0);
  SDValue V2 = Op.getOperand(1);
  MVT VT = Op.getSimpleValueType();

  assert((VT == MVT::v4i32 || VT == MVT::v4f32) &&
         "unsupported shuffle type");

  if (V2.getOpcode() == ISD::UNDEF)
    V2 = V1;

  // v4i32 or v4f32
  return getTargetShuffleNode(X86ISD::MOVHLPS, dl, VT, V1, V2, DAG);
}

static
SDValue getMOVLP(SDValue &Op, SDLoc &dl, SelectionDAG &DAG, bool HasSSE2) {
  SDValue V1 = Op.getOperand(0);
  SDValue V2 = Op.getOperand(1);
  MVT VT = Op.getSimpleValueType();
  unsigned NumElems = VT.getVectorNumElements();

  // Use MOVLPS and MOVLPD in case V1 or V2 are loads. During isel, the second
  // operand of these instructions is only memory, so check if there's a
  // potencial load folding here, otherwise use SHUFPS or MOVSD to match the
  // same masks.
  bool CanFoldLoad = false;

  // Trivial case, when V2 comes from a load.
  if (MayFoldVectorLoad(V2))
    CanFoldLoad = true;

  // When V1 is a load, it can be folded later into a store in isel, example:
  //  (store (v4f32 (X86Movlps (load addr:$src1), VR128:$src2)), addr:$src1)
  //    turns into:
  //  (MOVLPSmr addr:$src1, VR128:$src2)
  // So, recognize this potential and also use MOVLPS or MOVLPD
  else if (MayFoldVectorLoad(V1) && MayFoldIntoStore(Op))
    CanFoldLoad = true;

  ShuffleVectorSDNode *SVOp = cast<ShuffleVectorSDNode>(Op);
  if (CanFoldLoad) {
    if (HasSSE2 && NumElems == 2)
      return getTargetShuffleNode(X86ISD::MOVLPD, dl, VT, V1, V2, DAG);

    if (NumElems == 4)
      // If we don't care about the second element, proceed to use movss.
      if (SVOp->getMaskElt(1) != -1)
        return getTargetShuffleNode(X86ISD::MOVLPS, dl, VT, V1, V2, DAG);
  }

  // movl and movlp will both match v2i64, but v2i64 is never matched by
  // movl earlier because we make it strict to avoid messing with the movlp load
  // folding logic (see the code above getMOVLP call). Match it here then,
  // this is horrible, but will stay like this until we move all shuffle
  // matching to x86 specific nodes. Note that for the 1st condition all
  // types are matched with movsd.
  if (HasSSE2) {
    // FIXME: isMOVLMask should be checked and matched before getMOVLP,
    // as to remove this logic from here, as much as possible
    if (NumElems == 2 || !isMOVLMask(SVOp->getMask(), VT))
      return getTargetShuffleNode(X86ISD::MOVSD, dl, VT, V1, V2, DAG);
    return getTargetShuffleNode(X86ISD::MOVSS, dl, VT, V1, V2, DAG);
  }

  assert(VT != MVT::v4i32 && "unsupported shuffle type");

  // Invert the operand order and use SHUFPS to match it.
  return getTargetShuffleNode(X86ISD::SHUFP, dl, VT, V2, V1,
                              getShuffleSHUFImmediate(SVOp), DAG);
}

static SDValue NarrowVectorLoadToElement(LoadSDNode *Load, unsigned Index,
                                         SelectionDAG &DAG) {
  SDLoc dl(Load);
  MVT VT = Load->getSimpleValueType(0);
  MVT EVT = VT.getVectorElementType();
  SDValue Addr = Load->getOperand(1);
  SDValue NewAddr = DAG.getNode(
      ISD::ADD, dl, Addr.getSimpleValueType(), Addr,
      DAG.getConstant(Index * EVT.getStoreSize(), Addr.getSimpleValueType()));

  SDValue NewLoad =
      DAG.getLoad(EVT, dl, Load->getChain(), NewAddr,
                  DAG.getMachineFunction().getMachineMemOperand(
                      Load->getMemOperand(), 0, EVT.getStoreSize()));
  return NewLoad;
}

// It is only safe to call this function if isINSERTPSMask is true for
// this shufflevector mask.
static SDValue getINSERTPS(ShuffleVectorSDNode *SVOp, SDLoc &dl,
                           SelectionDAG &DAG) {
  // Generate an insertps instruction when inserting an f32 from memory onto a
  // v4f32 or when copying a member from one v4f32 to another.
  // We also use it for transferring i32 from one register to another,
  // since it simply copies the same bits.
  // If we're transferring an i32 from memory to a specific element in a
  // register, we output a generic DAG that will match the PINSRD
  // instruction.
  MVT VT = SVOp->getSimpleValueType(0);
  MVT EVT = VT.getVectorElementType();
  SDValue V1 = SVOp->getOperand(0);
  SDValue V2 = SVOp->getOperand(1);
  auto Mask = SVOp->getMask();
  assert((VT == MVT::v4f32 || VT == MVT::v4i32) &&
         "unsupported vector type for insertps/pinsrd");

  auto FromV1Predicate = [](const int &i) { return i < 4 && i > -1; };
  auto FromV2Predicate = [](const int &i) { return i >= 4; };
  int FromV1 = std::count_if(Mask.begin(), Mask.end(), FromV1Predicate);

  SDValue From;
  SDValue To;
  unsigned DestIndex;
  if (FromV1 == 1) {
    From = V1;
    To = V2;
    DestIndex = std::find_if(Mask.begin(), Mask.end(), FromV1Predicate) -
                Mask.begin();

    // If we have 1 element from each vector, we have to check if we're
    // changing V1's element's place. If so, we're done. Otherwise, we
    // should assume we're changing V2's element's place and behave
    // accordingly.
    int FromV2 = std::count_if(Mask.begin(), Mask.end(), FromV2Predicate);
    assert(DestIndex <= INT32_MAX && "truncated destination index");
    if (FromV1 == FromV2 &&
        static_cast<int>(DestIndex) == Mask[DestIndex] % 4) {
      From = V2;
      To = V1;
      DestIndex =
          std::find_if(Mask.begin(), Mask.end(), FromV2Predicate) - Mask.begin();
    }
  } else {
    assert(std::count_if(Mask.begin(), Mask.end(), FromV2Predicate) == 1 &&
           "More than one element from V1 and from V2, or no elements from one "
           "of the vectors. This case should not have returned true from "
           "isINSERTPSMask");
    From = V2;
    To = V1;
    DestIndex =
        std::find_if(Mask.begin(), Mask.end(), FromV2Predicate) - Mask.begin();
  }

  // Get an index into the source vector in the range [0,4) (the mask is
  // in the range [0,8) because it can address V1 and V2)
  unsigned SrcIndex = Mask[DestIndex] % 4;
  if (MayFoldLoad(From)) {
    // Trivial case, when From comes from a load and is only used by the
    // shuffle. Make it use insertps from the vector that we need from that
    // load.
    SDValue NewLoad =
        NarrowVectorLoadToElement(cast<LoadSDNode>(From), SrcIndex, DAG);
    if (!NewLoad.getNode())
      return SDValue();

    if (EVT == MVT::f32) {
      // Create this as a scalar to vector to match the instruction pattern.
      SDValue LoadScalarToVector =
          DAG.getNode(ISD::SCALAR_TO_VECTOR, dl, VT, NewLoad);
      SDValue InsertpsMask = DAG.getIntPtrConstant(DestIndex << 4);
      return DAG.getNode(X86ISD::INSERTPS, dl, VT, To, LoadScalarToVector,
                         InsertpsMask);
    } else { // EVT == MVT::i32
      // If we're getting an i32 from memory, use an INSERT_VECTOR_ELT
      // instruction, to match the PINSRD instruction, which loads an i32 to a
      // certain vector element.
      return DAG.getNode(ISD::INSERT_VECTOR_ELT, dl, VT, To, NewLoad,
                         DAG.getConstant(DestIndex, MVT::i32));
    }
  }

  // Vector-element-to-vector
  SDValue InsertpsMask = DAG.getIntPtrConstant(DestIndex << 4 | SrcIndex << 6);
  return DAG.getNode(X86ISD::INSERTPS, dl, VT, To, From, InsertpsMask);
}

// Reduce a vector shuffle to zext.
static SDValue LowerVectorIntExtend(SDValue Op, const X86Subtarget *Subtarget,
                                    SelectionDAG &DAG) {
  // PMOVZX is only available from SSE41.
  if (!Subtarget->hasSSE41())
    return SDValue();

  MVT VT = Op.getSimpleValueType();

  // Only AVX2 support 256-bit vector integer extending.
  if (!Subtarget->hasInt256() && VT.is256BitVector())
    return SDValue();

  ShuffleVectorSDNode *SVOp = cast<ShuffleVectorSDNode>(Op);
  SDLoc DL(Op);
  SDValue V1 = Op.getOperand(0);
  SDValue V2 = Op.getOperand(1);
  unsigned NumElems = VT.getVectorNumElements();

  // Extending is an unary operation and the element type of the source vector
  // won't be equal to or larger than i64.
  if (V2.getOpcode() != ISD::UNDEF || !VT.isInteger() ||
      VT.getVectorElementType() == MVT::i64)
    return SDValue();

  // Find the expansion ratio, e.g. expanding from i8 to i32 has a ratio of 4.
  unsigned Shift = 1; // Start from 2, i.e. 1 << 1.
  while ((1U << Shift) < NumElems) {
    if (SVOp->getMaskElt(1U << Shift) == 1)
      break;
    Shift += 1;
    // The maximal ratio is 8, i.e. from i8 to i64.
    if (Shift > 3)
      return SDValue();
  }

  // Check the shuffle mask.
  unsigned Mask = (1U << Shift) - 1;
  for (unsigned i = 0; i != NumElems; ++i) {
    int EltIdx = SVOp->getMaskElt(i);
    if ((i & Mask) != 0 && EltIdx != -1)
      return SDValue();
    if ((i & Mask) == 0 && (unsigned)EltIdx != (i >> Shift))
      return SDValue();
  }

  unsigned NBits = VT.getVectorElementType().getSizeInBits() << Shift;
  MVT NeVT = MVT::getIntegerVT(NBits);
  MVT NVT = MVT::getVectorVT(NeVT, NumElems >> Shift);

  if (!DAG.getTargetLoweringInfo().isTypeLegal(NVT))
    return SDValue();

  // Simplify the operand as it's prepared to be fed into shuffle.
  unsigned SignificantBits = NVT.getSizeInBits() >> Shift;
  if (V1.getOpcode() == ISD::BITCAST &&
      V1.getOperand(0).getOpcode() == ISD::SCALAR_TO_VECTOR &&
      V1.getOperand(0).getOperand(0).getOpcode() == ISD::EXTRACT_VECTOR_ELT &&
      V1.getOperand(0).getOperand(0)
        .getSimpleValueType().getSizeInBits() == SignificantBits) {
    // (bitcast (sclr2vec (ext_vec_elt x))) -> (bitcast x)
    SDValue V = V1.getOperand(0).getOperand(0).getOperand(0);
    ConstantSDNode *CIdx =
      dyn_cast<ConstantSDNode>(V1.getOperand(0).getOperand(0).getOperand(1));
    // If it's foldable, i.e. normal load with single use, we will let code
    // selection to fold it. Otherwise, we will short the conversion sequence.
    if (CIdx && CIdx->getZExtValue() == 0 &&
        (!ISD::isNormalLoad(V.getNode()) || !V.hasOneUse())) {
      MVT FullVT = V.getSimpleValueType();
      MVT V1VT = V1.getSimpleValueType();
      if (FullVT.getSizeInBits() > V1VT.getSizeInBits()) {
        // The "ext_vec_elt" node is wider than the result node.
        // In this case we should extract subvector from V.
        // (bitcast (sclr2vec (ext_vec_elt x))) -> (bitcast (extract_subvector x)).
        unsigned Ratio = FullVT.getSizeInBits() / V1VT.getSizeInBits();
        MVT SubVecVT = MVT::getVectorVT(FullVT.getVectorElementType(),
                                        FullVT.getVectorNumElements()/Ratio);
        V = DAG.getNode(ISD::EXTRACT_SUBVECTOR, DL, SubVecVT, V,
                        DAG.getIntPtrConstant(0));
      }
      V1 = DAG.getNode(ISD::BITCAST, DL, V1VT, V);
    }
  }

  return DAG.getNode(ISD::BITCAST, DL, VT,
                     DAG.getNode(X86ISD::VZEXT, DL, NVT, V1));
}

static SDValue NormalizeVectorShuffle(SDValue Op, const X86Subtarget *Subtarget,
                                      SelectionDAG &DAG) {
  ShuffleVectorSDNode *SVOp = cast<ShuffleVectorSDNode>(Op);
  MVT VT = Op.getSimpleValueType();
  SDLoc dl(Op);
  SDValue V1 = Op.getOperand(0);
  SDValue V2 = Op.getOperand(1);

  if (isZeroShuffle(SVOp))
    return getZeroVector(VT, Subtarget, DAG, dl);

  // Handle splat operations
  if (SVOp->isSplat()) {
    // Use vbroadcast whenever the splat comes from a foldable load
    SDValue Broadcast = LowerVectorBroadcast(Op, Subtarget, DAG);
    if (Broadcast.getNode())
      return Broadcast;
  }

  // Check integer expanding shuffles.
  SDValue NewOp = LowerVectorIntExtend(Op, Subtarget, DAG);
  if (NewOp.getNode())
    return NewOp;

  // If the shuffle can be profitably rewritten as a narrower shuffle, then
  // do it!
  if (VT == MVT::v8i16 || VT == MVT::v16i8 || VT == MVT::v16i16 ||
      VT == MVT::v32i8) {
    SDValue NewOp = RewriteAsNarrowerShuffle(SVOp, DAG);
    if (NewOp.getNode())
      return DAG.getNode(ISD::BITCAST, dl, VT, NewOp);
  } else if (VT.is128BitVector() && Subtarget->hasSSE2()) {
    // FIXME: Figure out a cleaner way to do this.
    if (ISD::isBuildVectorAllZeros(V2.getNode())) {
      SDValue NewOp = RewriteAsNarrowerShuffle(SVOp, DAG);
      if (NewOp.getNode()) {
        MVT NewVT = NewOp.getSimpleValueType();
        if (isCommutedMOVLMask(cast<ShuffleVectorSDNode>(NewOp)->getMask(),
                               NewVT, true, false))
          return getVZextMovL(VT, NewVT, NewOp.getOperand(0), DAG, Subtarget,
                              dl);
      }
    } else if (ISD::isBuildVectorAllZeros(V1.getNode())) {
      SDValue NewOp = RewriteAsNarrowerShuffle(SVOp, DAG);
      if (NewOp.getNode()) {
        MVT NewVT = NewOp.getSimpleValueType();
        if (isMOVLMask(cast<ShuffleVectorSDNode>(NewOp)->getMask(), NewVT))
          return getVZextMovL(VT, NewVT, NewOp.getOperand(1), DAG, Subtarget,
                              dl);
      }
    }
  }
  return SDValue();
}

SDValue
X86TargetLowering::LowerVECTOR_SHUFFLE(SDValue Op, SelectionDAG &DAG) const {
  ShuffleVectorSDNode *SVOp = cast<ShuffleVectorSDNode>(Op);
  SDValue V1 = Op.getOperand(0);
  SDValue V2 = Op.getOperand(1);
  MVT VT = Op.getSimpleValueType();
  SDLoc dl(Op);
  unsigned NumElems = VT.getVectorNumElements();
  bool V1IsUndef = V1.getOpcode() == ISD::UNDEF;
  bool V2IsUndef = V2.getOpcode() == ISD::UNDEF;
  bool V1IsSplat = false;
  bool V2IsSplat = false;
  bool HasSSE2 = Subtarget->hasSSE2();
  bool HasFp256    = Subtarget->hasFp256();
  bool HasInt256   = Subtarget->hasInt256();
  MachineFunction &MF = DAG.getMachineFunction();
  bool OptForSize = MF.getFunction()->getAttributes().
    hasAttribute(AttributeSet::FunctionIndex, Attribute::OptimizeForSize);

  // Check if we should use the experimental vector shuffle lowering. If so,
  // delegate completely to that code path.
  if (ExperimentalVectorShuffleLowering)
    return lowerVectorShuffle(Op, Subtarget, DAG);

  assert(VT.getSizeInBits() != 64 && "Can't lower MMX shuffles");

  if (V1IsUndef && V2IsUndef)
    return DAG.getUNDEF(VT);

  // When we create a shuffle node we put the UNDEF node to second operand,
  // but in some cases the first operand may be transformed to UNDEF.
  // In this case we should just commute the node.
  if (V1IsUndef)
    return DAG.getCommutedVectorShuffle(*SVOp);

  // Vector shuffle lowering takes 3 steps:
  //
  // 1) Normalize the input vectors. Here splats, zeroed vectors, profitable
  //    narrowing and commutation of operands should be handled.
  // 2) Matching of shuffles with known shuffle masks to x86 target specific
  //    shuffle nodes.
  // 3) Rewriting of unmatched masks into new generic shuffle operations,
  //    so the shuffle can be broken into other shuffles and the legalizer can
  //    try the lowering again.
  //
  // The general idea is that no vector_shuffle operation should be left to
  // be matched during isel, all of them must be converted to a target specific
  // node here.

  // Normalize the input vectors. Here splats, zeroed vectors, profitable
  // narrowing and commutation of operands should be handled. The actual code
  // doesn't include all of those, work in progress...
  SDValue NewOp = NormalizeVectorShuffle(Op, Subtarget, DAG);
  if (NewOp.getNode())
    return NewOp;

  SmallVector<int, 8> M(SVOp->getMask().begin(), SVOp->getMask().end());

  // NOTE: isPSHUFDMask can also match both masks below (unpckl_undef and
  // unpckh_undef). Only use pshufd if speed is more important than size.
  if (OptForSize && isUNPCKL_v_undef_Mask(M, VT, HasInt256))
    return getTargetShuffleNode(X86ISD::UNPCKL, dl, VT, V1, V1, DAG);
  if (OptForSize && isUNPCKH_v_undef_Mask(M, VT, HasInt256))
    return getTargetShuffleNode(X86ISD::UNPCKH, dl, VT, V1, V1, DAG);

  if (isMOVDDUPMask(M, VT) && Subtarget->hasSSE3() &&
      V2IsUndef && MayFoldVectorLoad(V1))
    return getMOVDDup(Op, dl, V1, DAG);

  if (isMOVHLPS_v_undef_Mask(M, VT))
    return getMOVHighToLow(Op, dl, DAG);

  // Use to match splats
  if (HasSSE2 && isUNPCKHMask(M, VT, HasInt256) && V2IsUndef &&
      (VT == MVT::v2f64 || VT == MVT::v2i64))
    return getTargetShuffleNode(X86ISD::UNPCKH, dl, VT, V1, V1, DAG);

  if (isPSHUFDMask(M, VT)) {
    // The actual implementation will match the mask in the if above and then
    // during isel it can match several different instructions, not only pshufd
    // as its name says, sad but true, emulate the behavior for now...
    if (isMOVDDUPMask(M, VT) && ((VT == MVT::v4f32 || VT == MVT::v2i64)))
      return getTargetShuffleNode(X86ISD::MOVLHPS, dl, VT, V1, V1, DAG);

    unsigned TargetMask = getShuffleSHUFImmediate(SVOp);

    if (HasSSE2 && (VT == MVT::v4f32 || VT == MVT::v4i32))
      return getTargetShuffleNode(X86ISD::PSHUFD, dl, VT, V1, TargetMask, DAG);

    if (HasFp256 && (VT == MVT::v4f32 || VT == MVT::v2f64))
      return getTargetShuffleNode(X86ISD::VPERMILP, dl, VT, V1, TargetMask,
                                  DAG);

    return getTargetShuffleNode(X86ISD::SHUFP, dl, VT, V1, V1,
                                TargetMask, DAG);
  }

  if (isPALIGNRMask(M, VT, Subtarget))
    return getTargetShuffleNode(X86ISD::PALIGNR, dl, VT, V1, V2,
                                getShufflePALIGNRImmediate(SVOp),
                                DAG);

  if (isVALIGNMask(M, VT, Subtarget))
    return getTargetShuffleNode(X86ISD::VALIGN, dl, VT, V1, V2,
                                getShuffleVALIGNImmediate(SVOp),
                                DAG);

  // Check if this can be converted into a logical shift.
  bool isLeft = false;
  unsigned ShAmt = 0;
  SDValue ShVal;
  bool isShift = HasSSE2 && isVectorShift(SVOp, DAG, isLeft, ShVal, ShAmt);
  if (isShift && ShVal.hasOneUse()) {
    // If the shifted value has multiple uses, it may be cheaper to use
    // v_set0 + movlhps or movhlps, etc.
    MVT EltVT = VT.getVectorElementType();
    ShAmt *= EltVT.getSizeInBits();
    return getVShift(isLeft, VT, ShVal, ShAmt, DAG, *this, dl);
  }

  if (isMOVLMask(M, VT)) {
    if (ISD::isBuildVectorAllZeros(V1.getNode()))
      return getVZextMovL(VT, VT, V2, DAG, Subtarget, dl);
    if (!isMOVLPMask(M, VT)) {
      if (HasSSE2 && (VT == MVT::v2i64 || VT == MVT::v2f64))
        return getTargetShuffleNode(X86ISD::MOVSD, dl, VT, V1, V2, DAG);

      if (VT == MVT::v4i32 || VT == MVT::v4f32)
        return getTargetShuffleNode(X86ISD::MOVSS, dl, VT, V1, V2, DAG);
    }
  }

  // FIXME: fold these into legal mask.
  if (isMOVLHPSMask(M, VT) && !isUNPCKLMask(M, VT, HasInt256))
    return getMOVLowToHigh(Op, dl, DAG, HasSSE2);

  if (isMOVHLPSMask(M, VT))
    return getMOVHighToLow(Op, dl, DAG);

  if (V2IsUndef && isMOVSHDUPMask(M, VT, Subtarget))
    return getTargetShuffleNode(X86ISD::MOVSHDUP, dl, VT, V1, DAG);

  if (V2IsUndef && isMOVSLDUPMask(M, VT, Subtarget))
    return getTargetShuffleNode(X86ISD::MOVSLDUP, dl, VT, V1, DAG);

  if (isMOVLPMask(M, VT))
    return getMOVLP(Op, dl, DAG, HasSSE2);

  if (ShouldXformToMOVHLPS(M, VT) ||
      ShouldXformToMOVLP(V1.getNode(), V2.getNode(), M, VT))
    return DAG.getCommutedVectorShuffle(*SVOp);

  if (isShift) {
    // No better options. Use a vshldq / vsrldq.
    MVT EltVT = VT.getVectorElementType();
    ShAmt *= EltVT.getSizeInBits();
    return getVShift(isLeft, VT, ShVal, ShAmt, DAG, *this, dl);
  }

  bool Commuted = false;
  // FIXME: This should also accept a bitcast of a splat?  Be careful, not
  // 1,1,1,1 -> v8i16 though.
  BitVector UndefElements;
  if (auto *BVOp = dyn_cast<BuildVectorSDNode>(V1.getNode()))
    if (BVOp->getConstantSplatNode(&UndefElements) && UndefElements.none())
      V1IsSplat = true;
  if (auto *BVOp = dyn_cast<BuildVectorSDNode>(V2.getNode()))
    if (BVOp->getConstantSplatNode(&UndefElements) && UndefElements.none())
      V2IsSplat = true;

  // Canonicalize the splat or undef, if present, to be on the RHS.
  if (!V2IsUndef && V1IsSplat && !V2IsSplat) {
    CommuteVectorShuffleMask(M, NumElems);
    std::swap(V1, V2);
    std::swap(V1IsSplat, V2IsSplat);
    Commuted = true;
  }

  if (isCommutedMOVLMask(M, VT, V2IsSplat, V2IsUndef)) {
    // Shuffling low element of v1 into undef, just return v1.
    if (V2IsUndef)
      return V1;
    // If V2 is a splat, the mask may be malformed such as <4,3,3,3>, which
    // the instruction selector will not match, so get a canonical MOVL with
    // swapped operands to undo the commute.
    return getMOVL(DAG, dl, VT, V2, V1);
  }

  if (isUNPCKLMask(M, VT, HasInt256))
    return getTargetShuffleNode(X86ISD::UNPCKL, dl, VT, V1, V2, DAG);

  if (isUNPCKHMask(M, VT, HasInt256))
    return getTargetShuffleNode(X86ISD::UNPCKH, dl, VT, V1, V2, DAG);

  if (V2IsSplat) {
    // Normalize mask so all entries that point to V2 points to its first
    // element then try to match unpck{h|l} again. If match, return a
    // new vector_shuffle with the corrected mask.p
    SmallVector<int, 8> NewMask(M.begin(), M.end());
    NormalizeMask(NewMask, NumElems);
    if (isUNPCKLMask(NewMask, VT, HasInt256, true))
      return getTargetShuffleNode(X86ISD::UNPCKL, dl, VT, V1, V2, DAG);
    if (isUNPCKHMask(NewMask, VT, HasInt256, true))
      return getTargetShuffleNode(X86ISD::UNPCKH, dl, VT, V1, V2, DAG);
  }

  if (Commuted) {
    // Commute is back and try unpck* again.
    // FIXME: this seems wrong.
    CommuteVectorShuffleMask(M, NumElems);
    std::swap(V1, V2);
    std::swap(V1IsSplat, V2IsSplat);

    if (isUNPCKLMask(M, VT, HasInt256))
      return getTargetShuffleNode(X86ISD::UNPCKL, dl, VT, V1, V2, DAG);

    if (isUNPCKHMask(M, VT, HasInt256))
      return getTargetShuffleNode(X86ISD::UNPCKH, dl, VT, V1, V2, DAG);
  }

  // Normalize the node to match x86 shuffle ops if needed
  if (!V2IsUndef && (isSHUFPMask(M, VT, /* Commuted */ true)))
    return DAG.getCommutedVectorShuffle(*SVOp);

  // The checks below are all present in isShuffleMaskLegal, but they are
  // inlined here right now to enable us to directly emit target specific
  // nodes, and remove one by one until they don't return Op anymore.

  if (ShuffleVectorSDNode::isSplatMask(&M[0], VT) &&
      SVOp->getSplatIndex() == 0 && V2IsUndef) {
    if (VT == MVT::v2f64 || VT == MVT::v2i64)
      return getTargetShuffleNode(X86ISD::UNPCKL, dl, VT, V1, V1, DAG);
  }

  if (isPSHUFHWMask(M, VT, HasInt256))
    return getTargetShuffleNode(X86ISD::PSHUFHW, dl, VT, V1,
                                getShufflePSHUFHWImmediate(SVOp),
                                DAG);

  if (isPSHUFLWMask(M, VT, HasInt256))
    return getTargetShuffleNode(X86ISD::PSHUFLW, dl, VT, V1,
                                getShufflePSHUFLWImmediate(SVOp),
                                DAG);

  unsigned MaskValue;
  if (isBlendMask(M, VT, Subtarget->hasSSE41(), Subtarget->hasInt256(),
                  &MaskValue))
    return LowerVECTOR_SHUFFLEtoBlend(SVOp, MaskValue, Subtarget, DAG);

  if (isSHUFPMask(M, VT))
    return getTargetShuffleNode(X86ISD::SHUFP, dl, VT, V1, V2,
                                getShuffleSHUFImmediate(SVOp), DAG);

  if (isUNPCKL_v_undef_Mask(M, VT, HasInt256))
    return getTargetShuffleNode(X86ISD::UNPCKL, dl, VT, V1, V1, DAG);
  if (isUNPCKH_v_undef_Mask(M, VT, HasInt256))
    return getTargetShuffleNode(X86ISD::UNPCKH, dl, VT, V1, V1, DAG);

  //===--------------------------------------------------------------------===//
  // Generate target specific nodes for 128 or 256-bit shuffles only
  // supported in the AVX instruction set.
  //

  // Handle VMOVDDUPY permutations
  if (V2IsUndef && isMOVDDUPYMask(M, VT, HasFp256))
    return getTargetShuffleNode(X86ISD::MOVDDUP, dl, VT, V1, DAG);

  // Handle VPERMILPS/D* permutations
  if (isVPERMILPMask(M, VT)) {
    if ((HasInt256 && VT == MVT::v8i32) || VT == MVT::v16i32)
      return getTargetShuffleNode(X86ISD::PSHUFD, dl, VT, V1,
                                  getShuffleSHUFImmediate(SVOp), DAG);
    return getTargetShuffleNode(X86ISD::VPERMILP, dl, VT, V1,
                                getShuffleSHUFImmediate(SVOp), DAG);
  }

  unsigned Idx;
  if (VT.is512BitVector() && isINSERT64x4Mask(M, VT, &Idx))
    return Insert256BitVector(V1, Extract256BitVector(V2, 0, DAG, dl),
                              Idx*(NumElems/2), DAG, dl);

  // Handle VPERM2F128/VPERM2I128 permutations
  if (isVPERM2X128Mask(M, VT, HasFp256))
    return getTargetShuffleNode(X86ISD::VPERM2X128, dl, VT, V1,
                                V2, getShuffleVPERM2X128Immediate(SVOp), DAG);

  if (Subtarget->hasSSE41() && isINSERTPSMask(M, VT))
    return getINSERTPS(SVOp, dl, DAG);

  unsigned Imm8;
  if (V2IsUndef && HasInt256 && isPermImmMask(M, VT, Imm8))
    return getTargetShuffleNode(X86ISD::VPERMI, dl, VT, V1, Imm8, DAG);

  if ((V2IsUndef && HasInt256 && VT.is256BitVector() && NumElems == 8) ||
      VT.is512BitVector()) {
    MVT MaskEltVT = MVT::getIntegerVT(VT.getVectorElementType().getSizeInBits());
    MVT MaskVectorVT = MVT::getVectorVT(MaskEltVT, NumElems);
    SmallVector<SDValue, 16> permclMask;
    for (unsigned i = 0; i != NumElems; ++i) {
      permclMask.push_back(DAG.getConstant((M[i]>=0) ? M[i] : 0, MaskEltVT));
    }

    SDValue Mask = DAG.getNode(ISD::BUILD_VECTOR, dl, MaskVectorVT, permclMask);
    if (V2IsUndef)
      // Bitcast is for VPERMPS since mask is v8i32 but node takes v8f32
      return DAG.getNode(X86ISD::VPERMV, dl, VT,
                          DAG.getNode(ISD::BITCAST, dl, VT, Mask), V1);
    return DAG.getNode(X86ISD::VPERMV3, dl, VT, V1,
                       DAG.getNode(ISD::BITCAST, dl, VT, Mask), V2);
  }

  //===--------------------------------------------------------------------===//
  // Since no target specific shuffle was selected for this generic one,
  // lower it into other known shuffles. FIXME: this isn't true yet, but
  // this is the plan.
  //

  // Handle v8i16 specifically since SSE can do byte extraction and insertion.
  if (VT == MVT::v8i16) {
    SDValue NewOp = LowerVECTOR_SHUFFLEv8i16(Op, Subtarget, DAG);
    if (NewOp.getNode())
      return NewOp;
  }

  if (VT == MVT::v16i16 && Subtarget->hasInt256()) {
    SDValue NewOp = LowerVECTOR_SHUFFLEv16i16(Op, DAG);
    if (NewOp.getNode())
      return NewOp;
  }

  if (VT == MVT::v16i8) {
    SDValue NewOp = LowerVECTOR_SHUFFLEv16i8(SVOp, Subtarget, DAG);
    if (NewOp.getNode())
      return NewOp;
  }

  if (VT == MVT::v32i8) {
    SDValue NewOp = LowerVECTOR_SHUFFLEv32i8(SVOp, Subtarget, DAG);
    if (NewOp.getNode())
      return NewOp;
  }

  // Handle all 128-bit wide vectors with 4 elements, and match them with
  // several different shuffle types.
  if (NumElems == 4 && VT.is128BitVector())
    return LowerVECTOR_SHUFFLE_128v4(SVOp, DAG);

  // Handle general 256-bit shuffles
  if (VT.is256BitVector())
    return LowerVECTOR_SHUFFLE_256(SVOp, DAG);

  return SDValue();
}

// This function assumes its argument is a BUILD_VECTOR of constants or
// undef SDNodes. i.e: ISD::isBuildVectorOfConstantSDNodes(BuildVector) is
// true.
static bool BUILD_VECTORtoBlendMask(BuildVectorSDNode *BuildVector,
                                    unsigned &MaskValue) {
  MaskValue = 0;
  unsigned NumElems = BuildVector->getNumOperands();
  // There are 2 lanes if (NumElems > 8), and 1 lane otherwise.
  unsigned NumLanes = (NumElems - 1) / 8 + 1;
  unsigned NumElemsInLane = NumElems / NumLanes;

  // Blend for v16i16 should be symetric for the both lanes.
  for (unsigned i = 0; i < NumElemsInLane; ++i) {
    SDValue EltCond = BuildVector->getOperand(i);
    SDValue SndLaneEltCond =
        (NumLanes == 2) ? BuildVector->getOperand(i + NumElemsInLane) : EltCond;

    int Lane1Cond = -1, Lane2Cond = -1;
    if (isa<ConstantSDNode>(EltCond))
      Lane1Cond = !isZero(EltCond);
    if (isa<ConstantSDNode>(SndLaneEltCond))
      Lane2Cond = !isZero(SndLaneEltCond);

    if (Lane1Cond == Lane2Cond || Lane2Cond < 0)
      // Lane1Cond != 0, means we want the first argument.
      // Lane1Cond == 0, means we want the second argument.
      // The encoding of this argument is 0 for the first argument, 1
      // for the second. Therefore, invert the condition.
      MaskValue |= !Lane1Cond << i;
    else if (Lane1Cond < 0)
      MaskValue |= !Lane2Cond << i;
    else
      return false;
  }
  return true;
}

// Try to lower a vselect node into a simple blend instruction.
static SDValue LowerVSELECTtoBlend(SDValue Op, const X86Subtarget *Subtarget,
                                   SelectionDAG &DAG) {
  SDValue Cond = Op.getOperand(0);
  SDValue LHS = Op.getOperand(1);
  SDValue RHS = Op.getOperand(2);
  SDLoc dl(Op);
  MVT VT = Op.getSimpleValueType();
  MVT EltVT = VT.getVectorElementType();
  unsigned NumElems = VT.getVectorNumElements();

  // There is no blend with immediate in AVX-512.
  if (VT.is512BitVector())
    return SDValue();

  if (!Subtarget->hasSSE41() || EltVT == MVT::i8)
    return SDValue();
  if (!Subtarget->hasInt256() && VT == MVT::v16i16)
    return SDValue();

  if (!ISD::isBuildVectorOfConstantSDNodes(Cond.getNode()))
    return SDValue();

  // Check the mask for BLEND and build the value.
  unsigned MaskValue = 0;
  if (!BUILD_VECTORtoBlendMask(cast<BuildVectorSDNode>(Cond), MaskValue))
    return SDValue();

  // Convert i32 vectors to floating point if it is not AVX2.
  // AVX2 introduced VPBLENDD instruction for 128 and 256-bit vectors.
  MVT BlendVT = VT;
  if (EltVT == MVT::i64 || (EltVT == MVT::i32 && !Subtarget->hasInt256())) {
    BlendVT = MVT::getVectorVT(MVT::getFloatingPointVT(EltVT.getSizeInBits()),
                               NumElems);
    LHS = DAG.getNode(ISD::BITCAST, dl, VT, LHS);
    RHS = DAG.getNode(ISD::BITCAST, dl, VT, RHS);
  }

  SDValue Ret = DAG.getNode(X86ISD::BLENDI, dl, BlendVT, LHS, RHS,
                            DAG.getConstant(MaskValue, MVT::i32));
  return DAG.getNode(ISD::BITCAST, dl, VT, Ret);
}

SDValue X86TargetLowering::LowerVSELECT(SDValue Op, SelectionDAG &DAG) const {
  // A vselect where all conditions and data are constants can be optimized into
  // a single vector load by SelectionDAGLegalize::ExpandBUILD_VECTOR().
  if (ISD::isBuildVectorOfConstantSDNodes(Op.getOperand(0).getNode()) &&
      ISD::isBuildVectorOfConstantSDNodes(Op.getOperand(1).getNode()) &&
      ISD::isBuildVectorOfConstantSDNodes(Op.getOperand(2).getNode()))
    return SDValue();
  
  SDValue BlendOp = LowerVSELECTtoBlend(Op, Subtarget, DAG);
  if (BlendOp.getNode())
    return BlendOp;

  // Some types for vselect were previously set to Expand, not Legal or
  // Custom. Return an empty SDValue so we fall-through to Expand, after
  // the Custom lowering phase.
  MVT VT = Op.getSimpleValueType();
  switch (VT.SimpleTy) {
  default:
    break;
  case MVT::v8i16:
  case MVT::v16i16:
    if (Subtarget->hasBWI() && Subtarget->hasVLX())
      break;
    return SDValue();
  }

  // We couldn't create a "Blend with immediate" node.
  // This node should still be legal, but we'll have to emit a blendv*
  // instruction.
  return Op;
}

static SDValue LowerEXTRACT_VECTOR_ELT_SSE4(SDValue Op, SelectionDAG &DAG) {
  MVT VT = Op.getSimpleValueType();
  SDLoc dl(Op);

  if (!Op.getOperand(0).getSimpleValueType().is128BitVector())
    return SDValue();

  if (VT.getSizeInBits() == 8) {
    SDValue Extract = DAG.getNode(X86ISD::PEXTRB, dl, MVT::i32,
                                  Op.getOperand(0), Op.getOperand(1));
    SDValue Assert  = DAG.getNode(ISD::AssertZext, dl, MVT::i32, Extract,
                                  DAG.getValueType(VT));
    return DAG.getNode(ISD::TRUNCATE, dl, VT, Assert);
  }

  if (VT.getSizeInBits() == 16) {
    unsigned Idx = cast<ConstantSDNode>(Op.getOperand(1))->getZExtValue();
    // If Idx is 0, it's cheaper to do a move instead of a pextrw.
    if (Idx == 0)
      return DAG.getNode(ISD::TRUNCATE, dl, MVT::i16,
                         DAG.getNode(ISD::EXTRACT_VECTOR_ELT, dl, MVT::i32,
                                     DAG.getNode(ISD::BITCAST, dl,
                                                 MVT::v4i32,
                                                 Op.getOperand(0)),
                                     Op.getOperand(1)));
    SDValue Extract = DAG.getNode(X86ISD::PEXTRW, dl, MVT::i32,
                                  Op.getOperand(0), Op.getOperand(1));
    SDValue Assert  = DAG.getNode(ISD::AssertZext, dl, MVT::i32, Extract,
                                  DAG.getValueType(VT));
    return DAG.getNode(ISD::TRUNCATE, dl, VT, Assert);
  }

  if (VT == MVT::f32) {
    // EXTRACTPS outputs to a GPR32 register which will require a movd to copy
    // the result back to FR32 register. It's only worth matching if the
    // result has a single use which is a store or a bitcast to i32.  And in
    // the case of a store, it's not worth it if the index is a constant 0,
    // because a MOVSSmr can be used instead, which is smaller and faster.
    if (!Op.hasOneUse())
      return SDValue();
    SDNode *User = *Op.getNode()->use_begin();
    if ((User->getOpcode() != ISD::STORE ||
         (isa<ConstantSDNode>(Op.getOperand(1)) &&
          cast<ConstantSDNode>(Op.getOperand(1))->isNullValue())) &&
        (User->getOpcode() != ISD::BITCAST ||
         User->getValueType(0) != MVT::i32))
      return SDValue();
    SDValue Extract = DAG.getNode(ISD::EXTRACT_VECTOR_ELT, dl, MVT::i32,
                                  DAG.getNode(ISD::BITCAST, dl, MVT::v4i32,
                                              Op.getOperand(0)),
                                              Op.getOperand(1));
    return DAG.getNode(ISD::BITCAST, dl, MVT::f32, Extract);
  }

  if (VT == MVT::i32 || VT == MVT::i64) {
    // ExtractPS/pextrq works with constant index.
    if (isa<ConstantSDNode>(Op.getOperand(1)))
      return Op;
  }
  return SDValue();
}

/// Extract one bit from mask vector, like v16i1 or v8i1.
/// AVX-512 feature.
SDValue
X86TargetLowering::ExtractBitFromMaskVector(SDValue Op, SelectionDAG &DAG) const {
  SDValue Vec = Op.getOperand(0);
  SDLoc dl(Vec);
  MVT VecVT = Vec.getSimpleValueType();
  SDValue Idx = Op.getOperand(1);
  MVT EltVT = Op.getSimpleValueType();

  assert((EltVT == MVT::i1) && "Unexpected operands in ExtractBitFromMaskVector");

  // variable index can't be handled in mask registers,
  // extend vector to VR512
  if (!isa<ConstantSDNode>(Idx)) {
    MVT ExtVT = (VecVT == MVT::v8i1 ?  MVT::v8i64 : MVT::v16i32);
    SDValue Ext = DAG.getNode(ISD::ZERO_EXTEND, dl, ExtVT, Vec);
    SDValue Elt = DAG.getNode(ISD::EXTRACT_VECTOR_ELT, dl,
                              ExtVT.getVectorElementType(), Ext, Idx);
    return DAG.getNode(ISD::TRUNCATE, dl, EltVT, Elt);
  }

  unsigned IdxVal = cast<ConstantSDNode>(Idx)->getZExtValue();
  const TargetRegisterClass* rc = getRegClassFor(VecVT);
  unsigned MaxSift = rc->getSize()*8 - 1;
  Vec = DAG.getNode(X86ISD::VSHLI, dl, VecVT, Vec,
                    DAG.getConstant(MaxSift - IdxVal, MVT::i8));
  Vec = DAG.getNode(X86ISD::VSRLI, dl, VecVT, Vec,
                    DAG.getConstant(MaxSift, MVT::i8));
  return DAG.getNode(X86ISD::VEXTRACT, dl, MVT::i1, Vec,
                       DAG.getIntPtrConstant(0));
}

SDValue
X86TargetLowering::LowerEXTRACT_VECTOR_ELT(SDValue Op,
                                           SelectionDAG &DAG) const {
  SDLoc dl(Op);
  SDValue Vec = Op.getOperand(0);
  MVT VecVT = Vec.getSimpleValueType();
  SDValue Idx = Op.getOperand(1);

  if (Op.getSimpleValueType() == MVT::i1)
    return ExtractBitFromMaskVector(Op, DAG);

  if (!isa<ConstantSDNode>(Idx)) {
    if (VecVT.is512BitVector() ||
        (VecVT.is256BitVector() && Subtarget->hasInt256() &&
         VecVT.getVectorElementType().getSizeInBits() == 32)) {

      MVT MaskEltVT =
        MVT::getIntegerVT(VecVT.getVectorElementType().getSizeInBits());
      MVT MaskVT = MVT::getVectorVT(MaskEltVT, VecVT.getSizeInBits() /
                                    MaskEltVT.getSizeInBits());

      Idx = DAG.getZExtOrTrunc(Idx, dl, MaskEltVT);
      SDValue Mask = DAG.getNode(X86ISD::VINSERT, dl, MaskVT,
                                getZeroVector(MaskVT, Subtarget, DAG, dl),
                                Idx, DAG.getConstant(0, getPointerTy()));
      SDValue Perm = DAG.getNode(X86ISD::VPERMV, dl, VecVT, Mask, Vec);
      return DAG.getNode(ISD::EXTRACT_VECTOR_ELT, dl, Op.getValueType(),
                        Perm, DAG.getConstant(0, getPointerTy()));
    }
    return SDValue();
  }

  // If this is a 256-bit vector result, first extract the 128-bit vector and
  // then extract the element from the 128-bit vector.
  if (VecVT.is256BitVector() || VecVT.is512BitVector()) {

    unsigned IdxVal = cast<ConstantSDNode>(Idx)->getZExtValue();
    // Get the 128-bit vector.
    Vec = Extract128BitVector(Vec, IdxVal, DAG, dl);
    MVT EltVT = VecVT.getVectorElementType();

    unsigned ElemsPerChunk = 128 / EltVT.getSizeInBits();

    //if (IdxVal >= NumElems/2)
    //  IdxVal -= NumElems/2;
    IdxVal -= (IdxVal/ElemsPerChunk)*ElemsPerChunk;
    return DAG.getNode(ISD::EXTRACT_VECTOR_ELT, dl, Op.getValueType(), Vec,
                       DAG.getConstant(IdxVal, MVT::i32));
  }

  assert(VecVT.is128BitVector() && "Unexpected vector length");

  if (Subtarget->hasSSE41()) {
    SDValue Res = LowerEXTRACT_VECTOR_ELT_SSE4(Op, DAG);
    if (Res.getNode())
      return Res;
  }

  MVT VT = Op.getSimpleValueType();
  // TODO: handle v16i8.
  if (VT.getSizeInBits() == 16) {
    SDValue Vec = Op.getOperand(0);
    unsigned Idx = cast<ConstantSDNode>(Op.getOperand(1))->getZExtValue();
    if (Idx == 0)
      return DAG.getNode(ISD::TRUNCATE, dl, MVT::i16,
                         DAG.getNode(ISD::EXTRACT_VECTOR_ELT, dl, MVT::i32,
                                     DAG.getNode(ISD::BITCAST, dl,
                                                 MVT::v4i32, Vec),
                                     Op.getOperand(1)));
    // Transform it so it match pextrw which produces a 32-bit result.
    MVT EltVT = MVT::i32;
    SDValue Extract = DAG.getNode(X86ISD::PEXTRW, dl, EltVT,
                                  Op.getOperand(0), Op.getOperand(1));
    SDValue Assert  = DAG.getNode(ISD::AssertZext, dl, EltVT, Extract,
                                  DAG.getValueType(VT));
    return DAG.getNode(ISD::TRUNCATE, dl, VT, Assert);
  }

  if (VT.getSizeInBits() == 32) {
    unsigned Idx = cast<ConstantSDNode>(Op.getOperand(1))->getZExtValue();
    if (Idx == 0)
      return Op;

    // SHUFPS the element to the lowest double word, then movss.
    int Mask[4] = { static_cast<int>(Idx), -1, -1, -1 };
    MVT VVT = Op.getOperand(0).getSimpleValueType();
    SDValue Vec = DAG.getVectorShuffle(VVT, dl, Op.getOperand(0),
                                       DAG.getUNDEF(VVT), Mask);
    return DAG.getNode(ISD::EXTRACT_VECTOR_ELT, dl, VT, Vec,
                       DAG.getIntPtrConstant(0));
  }

  if (VT.getSizeInBits() == 64) {
    // FIXME: .td only matches this for <2 x f64>, not <2 x i64> on 32b
    // FIXME: seems like this should be unnecessary if mov{h,l}pd were taught
    //        to match extract_elt for f64.
    unsigned Idx = cast<ConstantSDNode>(Op.getOperand(1))->getZExtValue();
    if (Idx == 0)
      return Op;

    // UNPCKHPD the element to the lowest double word, then movsd.
    // Note if the lower 64 bits of the result of the UNPCKHPD is then stored
    // to a f64mem, the whole operation is folded into a single MOVHPDmr.
    int Mask[2] = { 1, -1 };
    MVT VVT = Op.getOperand(0).getSimpleValueType();
    SDValue Vec = DAG.getVectorShuffle(VVT, dl, Op.getOperand(0),
                                       DAG.getUNDEF(VVT), Mask);
    return DAG.getNode(ISD::EXTRACT_VECTOR_ELT, dl, VT, Vec,
                       DAG.getIntPtrConstant(0));
  }

  return SDValue();
}

/// Insert one bit to mask vector, like v16i1 or v8i1.
/// AVX-512 feature.
SDValue 
X86TargetLowering::InsertBitToMaskVector(SDValue Op, SelectionDAG &DAG) const {
  SDLoc dl(Op);
  SDValue Vec = Op.getOperand(0);
  SDValue Elt = Op.getOperand(1);
  SDValue Idx = Op.getOperand(2);
  MVT VecVT = Vec.getSimpleValueType();

  if (!isa<ConstantSDNode>(Idx)) {
    // Non constant index. Extend source and destination,
    // insert element and then truncate the result.
    MVT ExtVecVT = (VecVT == MVT::v8i1 ?  MVT::v8i64 : MVT::v16i32);
    MVT ExtEltVT = (VecVT == MVT::v8i1 ?  MVT::i64 : MVT::i32);
    SDValue ExtOp = DAG.getNode(ISD::INSERT_VECTOR_ELT, dl, ExtVecVT, 
      DAG.getNode(ISD::ZERO_EXTEND, dl, ExtVecVT, Vec),
      DAG.getNode(ISD::ZERO_EXTEND, dl, ExtEltVT, Elt), Idx);
    return DAG.getNode(ISD::TRUNCATE, dl, VecVT, ExtOp);
  }

  unsigned IdxVal = cast<ConstantSDNode>(Idx)->getZExtValue();
  SDValue EltInVec = DAG.getNode(ISD::SCALAR_TO_VECTOR, dl, VecVT, Elt);
  if (Vec.getOpcode() == ISD::UNDEF)
    return DAG.getNode(X86ISD::VSHLI, dl, VecVT, EltInVec,
                       DAG.getConstant(IdxVal, MVT::i8));
  const TargetRegisterClass* rc = getRegClassFor(VecVT);
  unsigned MaxSift = rc->getSize()*8 - 1;
  EltInVec = DAG.getNode(X86ISD::VSHLI, dl, VecVT, EltInVec,
                    DAG.getConstant(MaxSift, MVT::i8));
  EltInVec = DAG.getNode(X86ISD::VSRLI, dl, VecVT, EltInVec,
                    DAG.getConstant(MaxSift - IdxVal, MVT::i8));
  return DAG.getNode(ISD::OR, dl, VecVT, Vec, EltInVec);
}

SDValue X86TargetLowering::LowerINSERT_VECTOR_ELT(SDValue Op,
                                                  SelectionDAG &DAG) const {
  MVT VT = Op.getSimpleValueType();
  MVT EltVT = VT.getVectorElementType();

  if (EltVT == MVT::i1)
    return InsertBitToMaskVector(Op, DAG);

  SDLoc dl(Op);
  SDValue N0 = Op.getOperand(0);
  SDValue N1 = Op.getOperand(1);
  SDValue N2 = Op.getOperand(2);
  if (!isa<ConstantSDNode>(N2))
    return SDValue();
  auto *N2C = cast<ConstantSDNode>(N2);
  unsigned IdxVal = N2C->getZExtValue();

  // If the vector is wider than 128 bits, extract the 128-bit subvector, insert
  // into that, and then insert the subvector back into the result.
  if (VT.is256BitVector() || VT.is512BitVector()) {
    // Get the desired 128-bit vector half.
    SDValue V = Extract128BitVector(N0, IdxVal, DAG, dl);

    // Insert the element into the desired half.
    unsigned NumEltsIn128 = 128 / EltVT.getSizeInBits();
    unsigned IdxIn128 = IdxVal - (IdxVal / NumEltsIn128) * NumEltsIn128;

    V = DAG.getNode(ISD::INSERT_VECTOR_ELT, dl, V.getValueType(), V, N1,
                    DAG.getConstant(IdxIn128, MVT::i32));

    // Insert the changed part back to the 256-bit vector
    return Insert128BitVector(N0, V, IdxVal, DAG, dl);
  }
  assert(VT.is128BitVector() && "Only 128-bit vector types should be left!");

  if (Subtarget->hasSSE41()) {
    if (EltVT.getSizeInBits() == 8 || EltVT.getSizeInBits() == 16) {
      unsigned Opc;
      if (VT == MVT::v8i16) {
        Opc = X86ISD::PINSRW;
      } else {
        assert(VT == MVT::v16i8);
        Opc = X86ISD::PINSRB;
      }

      // Transform it so it match pinsr{b,w} which expects a GR32 as its second
      // argument.
      if (N1.getValueType() != MVT::i32)
        N1 = DAG.getNode(ISD::ANY_EXTEND, dl, MVT::i32, N1);
      if (N2.getValueType() != MVT::i32)
        N2 = DAG.getIntPtrConstant(IdxVal);
      return DAG.getNode(Opc, dl, VT, N0, N1, N2);
    }

    if (EltVT == MVT::f32) {
      // Bits [7:6] of the constant are the source select.  This will always be
      //  zero here.  The DAG Combiner may combine an extract_elt index into
      //  these
      //  bits.  For example (insert (extract, 3), 2) could be matched by
      //  putting
      //  the '3' into bits [7:6] of X86ISD::INSERTPS.
      // Bits [5:4] of the constant are the destination select.  This is the
      //  value of the incoming immediate.
      // Bits [3:0] of the constant are the zero mask.  The DAG Combiner may
      //   combine either bitwise AND or insert of float 0.0 to set these bits.
      N2 = DAG.getIntPtrConstant(IdxVal << 4);
      // Create this as a scalar to vector..
      N1 = DAG.getNode(ISD::SCALAR_TO_VECTOR, dl, MVT::v4f32, N1);
      return DAG.getNode(X86ISD::INSERTPS, dl, VT, N0, N1, N2);
    }

    if (EltVT == MVT::i32 || EltVT == MVT::i64) {
      // PINSR* works with constant index.
      return Op;
    }
  }

  if (EltVT == MVT::i8)
    return SDValue();

  if (EltVT.getSizeInBits() == 16) {
    // Transform it so it match pinsrw which expects a 16-bit value in a GR32
    // as its second argument.
    if (N1.getValueType() != MVT::i32)
      N1 = DAG.getNode(ISD::ANY_EXTEND, dl, MVT::i32, N1);
    if (N2.getValueType() != MVT::i32)
      N2 = DAG.getIntPtrConstant(IdxVal);
    return DAG.getNode(X86ISD::PINSRW, dl, VT, N0, N1, N2);
  }
  return SDValue();
}

static SDValue LowerSCALAR_TO_VECTOR(SDValue Op, SelectionDAG &DAG) {
  SDLoc dl(Op);
  MVT OpVT = Op.getSimpleValueType();

  // If this is a 256-bit vector result, first insert into a 128-bit
  // vector and then insert into the 256-bit vector.
  if (!OpVT.is128BitVector()) {
    // Insert into a 128-bit vector.
    unsigned SizeFactor = OpVT.getSizeInBits()/128;
    MVT VT128 = MVT::getVectorVT(OpVT.getVectorElementType(),
                                 OpVT.getVectorNumElements() / SizeFactor);

    Op = DAG.getNode(ISD::SCALAR_TO_VECTOR, dl, VT128, Op.getOperand(0));

    // Insert the 128-bit vector.
    return Insert128BitVector(DAG.getUNDEF(OpVT), Op, 0, DAG, dl);
  }

  if (OpVT == MVT::v1i64 &&
      Op.getOperand(0).getValueType() == MVT::i64)
    return DAG.getNode(ISD::SCALAR_TO_VECTOR, dl, MVT::v1i64, Op.getOperand(0));

  SDValue AnyExt = DAG.getNode(ISD::ANY_EXTEND, dl, MVT::i32, Op.getOperand(0));
  assert(OpVT.is128BitVector() && "Expected an SSE type!");
  return DAG.getNode(ISD::BITCAST, dl, OpVT,
                     DAG.getNode(ISD::SCALAR_TO_VECTOR, dl, MVT::v4i32,AnyExt));
}

// Lower a node with an EXTRACT_SUBVECTOR opcode.  This may result in
// a simple subregister reference or explicit instructions to grab
// upper bits of a vector.
static SDValue LowerEXTRACT_SUBVECTOR(SDValue Op, const X86Subtarget *Subtarget,
                                      SelectionDAG &DAG) {
  SDLoc dl(Op);
  SDValue In =  Op.getOperand(0);
  SDValue Idx = Op.getOperand(1);
  unsigned IdxVal = cast<ConstantSDNode>(Idx)->getZExtValue();
  MVT ResVT   = Op.getSimpleValueType();
  MVT InVT    = In.getSimpleValueType();

  if (Subtarget->hasFp256()) {
    if (ResVT.is128BitVector() &&
        (InVT.is256BitVector() || InVT.is512BitVector()) &&
        isa<ConstantSDNode>(Idx)) {
      return Extract128BitVector(In, IdxVal, DAG, dl);
    }
    if (ResVT.is256BitVector() && InVT.is512BitVector() &&
        isa<ConstantSDNode>(Idx)) {
      return Extract256BitVector(In, IdxVal, DAG, dl);
    }
  }
  return SDValue();
}

// Lower a node with an INSERT_SUBVECTOR opcode.  This may result in a
// simple superregister reference or explicit instructions to insert
// the upper bits of a vector.
static SDValue LowerINSERT_SUBVECTOR(SDValue Op, const X86Subtarget *Subtarget,
                                     SelectionDAG &DAG) {
  if (Subtarget->hasFp256()) {
    SDLoc dl(Op.getNode());
    SDValue Vec = Op.getNode()->getOperand(0);
    SDValue SubVec = Op.getNode()->getOperand(1);
    SDValue Idx = Op.getNode()->getOperand(2);

    if ((Op.getNode()->getSimpleValueType(0).is256BitVector() ||
         Op.getNode()->getSimpleValueType(0).is512BitVector()) &&
        SubVec.getNode()->getSimpleValueType(0).is128BitVector() &&
        isa<ConstantSDNode>(Idx)) {
      unsigned IdxVal = cast<ConstantSDNode>(Idx)->getZExtValue();
      return Insert128BitVector(Vec, SubVec, IdxVal, DAG, dl);
    }

    if (Op.getNode()->getSimpleValueType(0).is512BitVector() &&
        SubVec.getNode()->getSimpleValueType(0).is256BitVector() &&
        isa<ConstantSDNode>(Idx)) {
      unsigned IdxVal = cast<ConstantSDNode>(Idx)->getZExtValue();
      return Insert256BitVector(Vec, SubVec, IdxVal, DAG, dl);
    }
  }
  return SDValue();
}

// ConstantPool, JumpTable, GlobalAddress, and ExternalSymbol are lowered as
// their target countpart wrapped in the X86ISD::Wrapper node. Suppose N is
// one of the above mentioned nodes. It has to be wrapped because otherwise
// Select(N) returns N. So the raw TargetGlobalAddress nodes, etc. can only
// be used to form addressing mode. These wrapped nodes will be selected
// into MOV32ri.
SDValue
X86TargetLowering::LowerConstantPool(SDValue Op, SelectionDAG &DAG) const {
  ConstantPoolSDNode *CP = cast<ConstantPoolSDNode>(Op);

  // In PIC mode (unless we're in RIPRel PIC mode) we add an offset to the
  // global base reg.
  unsigned char OpFlag = 0;
  unsigned WrapperKind = X86ISD::Wrapper;
  CodeModel::Model M = DAG.getTarget().getCodeModel();

  if (Subtarget->isPICStyleRIPRel() &&
      (M == CodeModel::Small || M == CodeModel::Kernel))
    WrapperKind = X86ISD::WrapperRIP;
  else if (Subtarget->isPICStyleGOT())
    OpFlag = X86II::MO_GOTOFF;
  else if (Subtarget->isPICStyleStubPIC())
    OpFlag = X86II::MO_PIC_BASE_OFFSET;

  SDValue Result = DAG.getTargetConstantPool(CP->getConstVal(), getPointerTy(),
                                             CP->getAlignment(),
                                             CP->getOffset(), OpFlag);
  SDLoc DL(CP);
  Result = DAG.getNode(WrapperKind, DL, getPointerTy(), Result);
  // With PIC, the address is actually $g + Offset.
  if (OpFlag) {
    Result = DAG.getNode(ISD::ADD, DL, getPointerTy(),
                         DAG.getNode(X86ISD::GlobalBaseReg,
                                     SDLoc(), getPointerTy()),
                         Result);
  }

  return Result;
}

SDValue X86TargetLowering::LowerJumpTable(SDValue Op, SelectionDAG &DAG) const {
  JumpTableSDNode *JT = cast<JumpTableSDNode>(Op);

  // In PIC mode (unless we're in RIPRel PIC mode) we add an offset to the
  // global base reg.
  unsigned char OpFlag = 0;
  unsigned WrapperKind = X86ISD::Wrapper;
  CodeModel::Model M = DAG.getTarget().getCodeModel();

  if (Subtarget->isPICStyleRIPRel() &&
      (M == CodeModel::Small || M == CodeModel::Kernel))
    WrapperKind = X86ISD::WrapperRIP;
  else if (Subtarget->isPICStyleGOT())
    OpFlag = X86II::MO_GOTOFF;
  else if (Subtarget->isPICStyleStubPIC())
    OpFlag = X86II::MO_PIC_BASE_OFFSET;

  SDValue Result = DAG.getTargetJumpTable(JT->getIndex(), getPointerTy(),
                                          OpFlag);
  SDLoc DL(JT);
  Result = DAG.getNode(WrapperKind, DL, getPointerTy(), Result);

  // With PIC, the address is actually $g + Offset.
  if (OpFlag)
    Result = DAG.getNode(ISD::ADD, DL, getPointerTy(),
                         DAG.getNode(X86ISD::GlobalBaseReg,
                                     SDLoc(), getPointerTy()),
                         Result);

  return Result;
}

SDValue
X86TargetLowering::LowerExternalSymbol(SDValue Op, SelectionDAG &DAG) const {
  const char *Sym = cast<ExternalSymbolSDNode>(Op)->getSymbol();

  // In PIC mode (unless we're in RIPRel PIC mode) we add an offset to the
  // global base reg.
  unsigned char OpFlag = 0;
  unsigned WrapperKind = X86ISD::Wrapper;
  CodeModel::Model M = DAG.getTarget().getCodeModel();

  if (Subtarget->isPICStyleRIPRel() &&
      (M == CodeModel::Small || M == CodeModel::Kernel)) {
    if (Subtarget->isTargetDarwin() || Subtarget->isTargetELF())
      OpFlag = X86II::MO_GOTPCREL;
    WrapperKind = X86ISD::WrapperRIP;
  } else if (Subtarget->isPICStyleGOT()) {
    OpFlag = X86II::MO_GOT;
  } else if (Subtarget->isPICStyleStubPIC()) {
    OpFlag = X86II::MO_DARWIN_NONLAZY_PIC_BASE;
  } else if (Subtarget->isPICStyleStubNoDynamic()) {
    OpFlag = X86II::MO_DARWIN_NONLAZY;
  }

  SDValue Result = DAG.getTargetExternalSymbol(Sym, getPointerTy(), OpFlag);

  SDLoc DL(Op);
  Result = DAG.getNode(WrapperKind, DL, getPointerTy(), Result);

  // With PIC, the address is actually $g + Offset.
  if (DAG.getTarget().getRelocationModel() == Reloc::PIC_ &&
      !Subtarget->is64Bit()) {
    Result = DAG.getNode(ISD::ADD, DL, getPointerTy(),
                         DAG.getNode(X86ISD::GlobalBaseReg,
                                     SDLoc(), getPointerTy()),
                         Result);
  }

  // For symbols that require a load from a stub to get the address, emit the
  // load.
  if (isGlobalStubReference(OpFlag))
    Result = DAG.getLoad(getPointerTy(), DL, DAG.getEntryNode(), Result,
                         MachinePointerInfo::getGOT(), false, false, false, 0);

  return Result;
}

SDValue
X86TargetLowering::LowerBlockAddress(SDValue Op, SelectionDAG &DAG) const {
  // Create the TargetBlockAddressAddress node.
  unsigned char OpFlags =
    Subtarget->ClassifyBlockAddressReference();
  CodeModel::Model M = DAG.getTarget().getCodeModel();
  const BlockAddress *BA = cast<BlockAddressSDNode>(Op)->getBlockAddress();
  int64_t Offset = cast<BlockAddressSDNode>(Op)->getOffset();
  SDLoc dl(Op);
  SDValue Result = DAG.getTargetBlockAddress(BA, getPointerTy(), Offset,
                                             OpFlags);

  if (Subtarget->isPICStyleRIPRel() &&
      (M == CodeModel::Small || M == CodeModel::Kernel))
    Result = DAG.getNode(X86ISD::WrapperRIP, dl, getPointerTy(), Result);
  else
    Result = DAG.getNode(X86ISD::Wrapper, dl, getPointerTy(), Result);

  // With PIC, the address is actually $g + Offset.
  if (isGlobalRelativeToPICBase(OpFlags)) {
    Result = DAG.getNode(ISD::ADD, dl, getPointerTy(),
                         DAG.getNode(X86ISD::GlobalBaseReg, dl, getPointerTy()),
                         Result);
  }

  return Result;
}

SDValue
X86TargetLowering::LowerGlobalAddress(const GlobalValue *GV, SDLoc dl,
                                      int64_t Offset, SelectionDAG &DAG) const {
  // Create the TargetGlobalAddress node, folding in the constant
  // offset if it is legal.
  unsigned char OpFlags =
      Subtarget->ClassifyGlobalReference(GV, DAG.getTarget());
  CodeModel::Model M = DAG.getTarget().getCodeModel();
  SDValue Result;
  if (OpFlags == X86II::MO_NO_FLAG &&
      X86::isOffsetSuitableForCodeModel(Offset, M)) {
    // A direct static reference to a global.
    Result = DAG.getTargetGlobalAddress(GV, dl, getPointerTy(), Offset);
    Offset = 0;
  } else {
    Result = DAG.getTargetGlobalAddress(GV, dl, getPointerTy(), 0, OpFlags);
  }

  if (Subtarget->isPICStyleRIPRel() &&
      (M == CodeModel::Small || M == CodeModel::Kernel))
    Result = DAG.getNode(X86ISD::WrapperRIP, dl, getPointerTy(), Result);
  else
    Result = DAG.getNode(X86ISD::Wrapper, dl, getPointerTy(), Result);

  // With PIC, the address is actually $g + Offset.
  if (isGlobalRelativeToPICBase(OpFlags)) {
    Result = DAG.getNode(ISD::ADD, dl, getPointerTy(),
                         DAG.getNode(X86ISD::GlobalBaseReg, dl, getPointerTy()),
                         Result);
  }

  // For globals that require a load from a stub to get the address, emit the
  // load.
  if (isGlobalStubReference(OpFlags))
    Result = DAG.getLoad(getPointerTy(), dl, DAG.getEntryNode(), Result,
                         MachinePointerInfo::getGOT(), false, false, false, 0);

  // If there was a non-zero offset that we didn't fold, create an explicit
  // addition for it.
  if (Offset != 0)
    Result = DAG.getNode(ISD::ADD, dl, getPointerTy(), Result,
                         DAG.getConstant(Offset, getPointerTy()));

  return Result;
}

SDValue
X86TargetLowering::LowerGlobalAddress(SDValue Op, SelectionDAG &DAG) const {
  const GlobalValue *GV = cast<GlobalAddressSDNode>(Op)->getGlobal();
  int64_t Offset = cast<GlobalAddressSDNode>(Op)->getOffset();
  return LowerGlobalAddress(GV, SDLoc(Op), Offset, DAG);
}

static SDValue
GetTLSADDR(SelectionDAG &DAG, SDValue Chain, GlobalAddressSDNode *GA,
           SDValue *InFlag, const EVT PtrVT, unsigned ReturnReg,
           unsigned char OperandFlags, bool LocalDynamic = false) {
  MachineFrameInfo *MFI = DAG.getMachineFunction().getFrameInfo();
  SDVTList NodeTys = DAG.getVTList(MVT::Other, MVT::Glue);
  SDLoc dl(GA);
  SDValue TGA = DAG.getTargetGlobalAddress(GA->getGlobal(), dl,
                                           GA->getValueType(0),
                                           GA->getOffset(),
                                           OperandFlags);

  X86ISD::NodeType CallType = LocalDynamic ? X86ISD::TLSBASEADDR
                                           : X86ISD::TLSADDR;

  if (InFlag) {
    SDValue Ops[] = { Chain,  TGA, *InFlag };
    Chain = DAG.getNode(CallType, dl, NodeTys, Ops);
  } else {
    SDValue Ops[]  = { Chain, TGA };
    Chain = DAG.getNode(CallType, dl, NodeTys, Ops);
  }

  // TLSADDR will be codegen'ed as call. Inform MFI that function has calls.
  MFI->setAdjustsStack(true);

  SDValue Flag = Chain.getValue(1);
  return DAG.getCopyFromReg(Chain, dl, ReturnReg, PtrVT, Flag);
}

// Lower ISD::GlobalTLSAddress using the "general dynamic" model, 32 bit
static SDValue
LowerToTLSGeneralDynamicModel32(GlobalAddressSDNode *GA, SelectionDAG &DAG,
                                const EVT PtrVT) {
  SDValue InFlag;
  SDLoc dl(GA);  // ? function entry point might be better
  SDValue Chain = DAG.getCopyToReg(DAG.getEntryNode(), dl, X86::EBX,
                                   DAG.getNode(X86ISD::GlobalBaseReg,
                                               SDLoc(), PtrVT), InFlag);
  InFlag = Chain.getValue(1);

  return GetTLSADDR(DAG, Chain, GA, &InFlag, PtrVT, X86::EAX, X86II::MO_TLSGD);
}

// Lower ISD::GlobalTLSAddress using the "general dynamic" model, 64 bit
static SDValue
LowerToTLSGeneralDynamicModel64(GlobalAddressSDNode *GA, SelectionDAG &DAG,
                                const EVT PtrVT) {
  return GetTLSADDR(DAG, DAG.getEntryNode(), GA, nullptr, PtrVT,
                    X86::RAX, X86II::MO_TLSGD);
}

static SDValue LowerToTLSLocalDynamicModel(GlobalAddressSDNode *GA,
                                           SelectionDAG &DAG,
                                           const EVT PtrVT,
                                           bool is64Bit) {
  SDLoc dl(GA);

  // Get the start address of the TLS block for this module.
  X86MachineFunctionInfo* MFI = DAG.getMachineFunction()
      .getInfo<X86MachineFunctionInfo>();
  MFI->incNumLocalDynamicTLSAccesses();

  SDValue Base;
  if (is64Bit) {
    Base = GetTLSADDR(DAG, DAG.getEntryNode(), GA, nullptr, PtrVT, X86::RAX,
                      X86II::MO_TLSLD, /*LocalDynamic=*/true);
  } else {
    SDValue InFlag;
    SDValue Chain = DAG.getCopyToReg(DAG.getEntryNode(), dl, X86::EBX,
        DAG.getNode(X86ISD::GlobalBaseReg, SDLoc(), PtrVT), InFlag);
    InFlag = Chain.getValue(1);
    Base = GetTLSADDR(DAG, Chain, GA, &InFlag, PtrVT, X86::EAX,
                      X86II::MO_TLSLDM, /*LocalDynamic=*/true);
  }

  // Note: the CleanupLocalDynamicTLSPass will remove redundant computations
  // of Base.

  // Build x@dtpoff.
  unsigned char OperandFlags = X86II::MO_DTPOFF;
  unsigned WrapperKind = X86ISD::Wrapper;
  SDValue TGA = DAG.getTargetGlobalAddress(GA->getGlobal(), dl,
                                           GA->getValueType(0),
                                           GA->getOffset(), OperandFlags);
  SDValue Offset = DAG.getNode(WrapperKind, dl, PtrVT, TGA);

  // Add x@dtpoff with the base.
  return DAG.getNode(ISD::ADD, dl, PtrVT, Offset, Base);
}

// Lower ISD::GlobalTLSAddress using the "initial exec" or "local exec" model.
static SDValue LowerToTLSExecModel(GlobalAddressSDNode *GA, SelectionDAG &DAG,
                                   const EVT PtrVT, TLSModel::Model model,
                                   bool is64Bit, bool isPIC) {
  SDLoc dl(GA);

  // Get the Thread Pointer, which is %gs:0 (32-bit) or %fs:0 (64-bit).
  Value *Ptr = Constant::getNullValue(Type::getInt8PtrTy(*DAG.getContext(),
                                                         is64Bit ? 257 : 256));

  SDValue ThreadPointer =
      DAG.getLoad(PtrVT, dl, DAG.getEntryNode(), DAG.getIntPtrConstant(0),
                  MachinePointerInfo(Ptr), false, false, false, 0);

  unsigned char OperandFlags = 0;
  // Most TLS accesses are not RIP relative, even on x86-64.  One exception is
  // initialexec.
  unsigned WrapperKind = X86ISD::Wrapper;
  if (model == TLSModel::LocalExec) {
    OperandFlags = is64Bit ? X86II::MO_TPOFF : X86II::MO_NTPOFF;
  } else if (model == TLSModel::InitialExec) {
    if (is64Bit) {
      OperandFlags = X86II::MO_GOTTPOFF;
      WrapperKind = X86ISD::WrapperRIP;
    } else {
      OperandFlags = isPIC ? X86II::MO_GOTNTPOFF : X86II::MO_INDNTPOFF;
    }
  } else {
    llvm_unreachable("Unexpected model");
  }

  // emit "addl x@ntpoff,%eax" (local exec)
  // or "addl x@indntpoff,%eax" (initial exec)
  // or "addl x@gotntpoff(%ebx) ,%eax" (initial exec, 32-bit pic)
  SDValue TGA =
      DAG.getTargetGlobalAddress(GA->getGlobal(), dl, GA->getValueType(0),
                                 GA->getOffset(), OperandFlags);
  SDValue Offset = DAG.getNode(WrapperKind, dl, PtrVT, TGA);

  if (model == TLSModel::InitialExec) {
    if (isPIC && !is64Bit) {
      Offset = DAG.getNode(ISD::ADD, dl, PtrVT,
                           DAG.getNode(X86ISD::GlobalBaseReg, SDLoc(), PtrVT),
                           Offset);
    }

    Offset = DAG.getLoad(PtrVT, dl, DAG.getEntryNode(), Offset,
                         MachinePointerInfo::getGOT(), false, false, false, 0);
  }

  // The address of the thread local variable is the add of the thread
  // pointer with the offset of the variable.
  return DAG.getNode(ISD::ADD, dl, PtrVT, ThreadPointer, Offset);
}

SDValue
X86TargetLowering::LowerGlobalTLSAddress(SDValue Op, SelectionDAG &DAG) const {

  GlobalAddressSDNode *GA = cast<GlobalAddressSDNode>(Op);
  const GlobalValue *GV = GA->getGlobal();

  if (Subtarget->isTargetELF()) {
    TLSModel::Model model = DAG.getTarget().getTLSModel(GV);

    switch (model) {
      case TLSModel::GeneralDynamic:
        if (Subtarget->is64Bit())
          return LowerToTLSGeneralDynamicModel64(GA, DAG, getPointerTy());
        return LowerToTLSGeneralDynamicModel32(GA, DAG, getPointerTy());
      case TLSModel::LocalDynamic:
        return LowerToTLSLocalDynamicModel(GA, DAG, getPointerTy(),
                                           Subtarget->is64Bit());
      case TLSModel::InitialExec:
      case TLSModel::LocalExec:
        return LowerToTLSExecModel(
            GA, DAG, getPointerTy(), model, Subtarget->is64Bit(),
            DAG.getTarget().getRelocationModel() == Reloc::PIC_);
    }
    llvm_unreachable("Unknown TLS model.");
  }

  if (Subtarget->isTargetDarwin()) {
    // Darwin only has one model of TLS.  Lower to that.
    unsigned char OpFlag = 0;
    unsigned WrapperKind = Subtarget->isPICStyleRIPRel() ?
                           X86ISD::WrapperRIP : X86ISD::Wrapper;

    // In PIC mode (unless we're in RIPRel PIC mode) we add an offset to the
    // global base reg.
    bool PIC32 = (DAG.getTarget().getRelocationModel() == Reloc::PIC_) &&
                 !Subtarget->is64Bit();
    if (PIC32)
      OpFlag = X86II::MO_TLVP_PIC_BASE;
    else
      OpFlag = X86II::MO_TLVP;
    SDLoc DL(Op);
    SDValue Result = DAG.getTargetGlobalAddress(GA->getGlobal(), DL,
                                                GA->getValueType(0),
                                                GA->getOffset(), OpFlag);
    SDValue Offset = DAG.getNode(WrapperKind, DL, getPointerTy(), Result);

    // With PIC32, the address is actually $g + Offset.
    if (PIC32)
      Offset = DAG.getNode(ISD::ADD, DL, getPointerTy(),
                           DAG.getNode(X86ISD::GlobalBaseReg,
                                       SDLoc(), getPointerTy()),
                           Offset);

    // Lowering the machine isd will make sure everything is in the right
    // location.
    SDValue Chain = DAG.getEntryNode();
    SDVTList NodeTys = DAG.getVTList(MVT::Other, MVT::Glue);
    SDValue Args[] = { Chain, Offset };
    Chain = DAG.getNode(X86ISD::TLSCALL, DL, NodeTys, Args);

    // TLSCALL will be codegen'ed as call. Inform MFI that function has calls.
    MachineFrameInfo *MFI = DAG.getMachineFunction().getFrameInfo();
    MFI->setAdjustsStack(true);

    // And our return value (tls address) is in the standard call return value
    // location.
    unsigned Reg = Subtarget->is64Bit() ? X86::RAX : X86::EAX;
    return DAG.getCopyFromReg(Chain, DL, Reg, getPointerTy(),
                              Chain.getValue(1));
  }

  if (Subtarget->isTargetKnownWindowsMSVC() ||
      Subtarget->isTargetWindowsGNU()) {
    // Just use the implicit TLS architecture
    // Need to generate someting similar to:
    //   mov     rdx, qword [gs:abs 58H]; Load pointer to ThreadLocalStorage
    //                                  ; from TEB
    //   mov     ecx, dword [rel _tls_index]: Load index (from C runtime)
    //   mov     rcx, qword [rdx+rcx*8]
    //   mov     eax, .tls$:tlsvar
    //   [rax+rcx] contains the address
    // Windows 64bit: gs:0x58
    // Windows 32bit: fs:__tls_array

    SDLoc dl(GA);
    SDValue Chain = DAG.getEntryNode();

    // Get the Thread Pointer, which is %fs:__tls_array (32-bit) or
    // %gs:0x58 (64-bit). On MinGW, __tls_array is not available, so directly
    // use its literal value of 0x2C.
    Value *Ptr = Constant::getNullValue(Subtarget->is64Bit()
                                        ? Type::getInt8PtrTy(*DAG.getContext(),
                                                             256)
                                        : Type::getInt32PtrTy(*DAG.getContext(),
                                                              257));

    SDValue TlsArray =
        Subtarget->is64Bit()
            ? DAG.getIntPtrConstant(0x58)
            : (Subtarget->isTargetWindowsGNU()
                   ? DAG.getIntPtrConstant(0x2C)
                   : DAG.getExternalSymbol("_tls_array", getPointerTy()));

    SDValue ThreadPointer =
        DAG.getLoad(getPointerTy(), dl, Chain, TlsArray,
                    MachinePointerInfo(Ptr), false, false, false, 0);

    // Load the _tls_index variable
    SDValue IDX = DAG.getExternalSymbol("_tls_index", getPointerTy());
    if (Subtarget->is64Bit())
      IDX = DAG.getExtLoad(ISD::ZEXTLOAD, dl, getPointerTy(), Chain,
                           IDX, MachinePointerInfo(), MVT::i32,
                           false, false, false, 0);
    else
      IDX = DAG.getLoad(getPointerTy(), dl, Chain, IDX, MachinePointerInfo(),
                        false, false, false, 0);

    SDValue Scale = DAG.getConstant(Log2_64_Ceil(TD->getPointerSize()),
                                    getPointerTy());
    IDX = DAG.getNode(ISD::SHL, dl, getPointerTy(), IDX, Scale);

    SDValue res = DAG.getNode(ISD::ADD, dl, getPointerTy(), ThreadPointer, IDX);
    res = DAG.getLoad(getPointerTy(), dl, Chain, res, MachinePointerInfo(),
                      false, false, false, 0);

    // Get the offset of start of .tls section
    SDValue TGA = DAG.getTargetGlobalAddress(GA->getGlobal(), dl,
                                             GA->getValueType(0),
                                             GA->getOffset(), X86II::MO_SECREL);
    SDValue Offset = DAG.getNode(X86ISD::Wrapper, dl, getPointerTy(), TGA);

    // The address of the thread local variable is the add of the thread
    // pointer with the offset of the variable.
    return DAG.getNode(ISD::ADD, dl, getPointerTy(), res, Offset);
  }

  llvm_unreachable("TLS not implemented for this target.");
}

/// LowerShiftParts - Lower SRA_PARTS and friends, which return two i32 values
/// and take a 2 x i32 value to shift plus a shift amount.
static SDValue LowerShiftParts(SDValue Op, SelectionDAG &DAG) {
  assert(Op.getNumOperands() == 3 && "Not a double-shift!");
  MVT VT = Op.getSimpleValueType();
  unsigned VTBits = VT.getSizeInBits();
  SDLoc dl(Op);
  bool isSRA = Op.getOpcode() == ISD::SRA_PARTS;
  SDValue ShOpLo = Op.getOperand(0);
  SDValue ShOpHi = Op.getOperand(1);
  SDValue ShAmt  = Op.getOperand(2);
  // X86ISD::SHLD and X86ISD::SHRD have defined overflow behavior but the
  // generic ISD nodes haven't. Insert an AND to be safe, it's optimized away
  // during isel.
  SDValue SafeShAmt = DAG.getNode(ISD::AND, dl, MVT::i8, ShAmt,
                                  DAG.getConstant(VTBits - 1, MVT::i8));
  SDValue Tmp1 = isSRA ? DAG.getNode(ISD::SRA, dl, VT, ShOpHi,
                                     DAG.getConstant(VTBits - 1, MVT::i8))
                       : DAG.getConstant(0, VT);

  SDValue Tmp2, Tmp3;
  if (Op.getOpcode() == ISD::SHL_PARTS) {
    Tmp2 = DAG.getNode(X86ISD::SHLD, dl, VT, ShOpHi, ShOpLo, ShAmt);
    Tmp3 = DAG.getNode(ISD::SHL, dl, VT, ShOpLo, SafeShAmt);
  } else {
    Tmp2 = DAG.getNode(X86ISD::SHRD, dl, VT, ShOpLo, ShOpHi, ShAmt);
    Tmp3 = DAG.getNode(isSRA ? ISD::SRA : ISD::SRL, dl, VT, ShOpHi, SafeShAmt);
  }

  // If the shift amount is larger or equal than the width of a part we can't
  // rely on the results of shld/shrd. Insert a test and select the appropriate
  // values for large shift amounts.
  SDValue AndNode = DAG.getNode(ISD::AND, dl, MVT::i8, ShAmt,
                                DAG.getConstant(VTBits, MVT::i8));
  SDValue Cond = DAG.getNode(X86ISD::CMP, dl, MVT::i32,
                             AndNode, DAG.getConstant(0, MVT::i8));

  SDValue Hi, Lo;
  SDValue CC = DAG.getConstant(X86::COND_NE, MVT::i8);
  SDValue Ops0[4] = { Tmp2, Tmp3, CC, Cond };
  SDValue Ops1[4] = { Tmp3, Tmp1, CC, Cond };

  if (Op.getOpcode() == ISD::SHL_PARTS) {
    Hi = DAG.getNode(X86ISD::CMOV, dl, VT, Ops0);
    Lo = DAG.getNode(X86ISD::CMOV, dl, VT, Ops1);
  } else {
    Lo = DAG.getNode(X86ISD::CMOV, dl, VT, Ops0);
    Hi = DAG.getNode(X86ISD::CMOV, dl, VT, Ops1);
  }

  SDValue Ops[2] = { Lo, Hi };
  return DAG.getMergeValues(Ops, dl);
}

SDValue X86TargetLowering::LowerSINT_TO_FP(SDValue Op,
                                           SelectionDAG &DAG) const {
  MVT SrcVT = Op.getOperand(0).getSimpleValueType();

  if (SrcVT.isVector())
    return SDValue();

  assert(SrcVT <= MVT::i64 && SrcVT >= MVT::i16 &&
         "Unknown SINT_TO_FP to lower!");

  // These are really Legal; return the operand so the caller accepts it as
  // Legal.
  if (SrcVT == MVT::i32 && isScalarFPTypeInSSEReg(Op.getValueType()))
    return Op;
  if (SrcVT == MVT::i64 && isScalarFPTypeInSSEReg(Op.getValueType()) &&
      Subtarget->is64Bit()) {
    return Op;
  }

  SDLoc dl(Op);
  unsigned Size = SrcVT.getSizeInBits()/8;
  MachineFunction &MF = DAG.getMachineFunction();
  int SSFI = MF.getFrameInfo()->CreateStackObject(Size, Size, false);
  SDValue StackSlot = DAG.getFrameIndex(SSFI, getPointerTy());
  SDValue Chain = DAG.getStore(DAG.getEntryNode(), dl, Op.getOperand(0),
                               StackSlot,
                               MachinePointerInfo::getFixedStack(SSFI),
                               false, false, 0);
  return BuildFILD(Op, SrcVT, Chain, StackSlot, DAG);
}

SDValue X86TargetLowering::BuildFILD(SDValue Op, EVT SrcVT, SDValue Chain,
                                     SDValue StackSlot,
                                     SelectionDAG &DAG) const {
  // Build the FILD
  SDLoc DL(Op);
  SDVTList Tys;
  bool useSSE = isScalarFPTypeInSSEReg(Op.getValueType());
  if (useSSE)
    Tys = DAG.getVTList(MVT::f64, MVT::Other, MVT::Glue);
  else
    Tys = DAG.getVTList(Op.getValueType(), MVT::Other);

  unsigned ByteSize = SrcVT.getSizeInBits()/8;

  FrameIndexSDNode *FI = dyn_cast<FrameIndexSDNode>(StackSlot);
  MachineMemOperand *MMO;
  if (FI) {
    int SSFI = FI->getIndex();
    MMO =
      DAG.getMachineFunction()
      .getMachineMemOperand(MachinePointerInfo::getFixedStack(SSFI),
                            MachineMemOperand::MOLoad, ByteSize, ByteSize);
  } else {
    MMO = cast<LoadSDNode>(StackSlot)->getMemOperand();
    StackSlot = StackSlot.getOperand(1);
  }
  SDValue Ops[] = { Chain, StackSlot, DAG.getValueType(SrcVT) };
  SDValue Result = DAG.getMemIntrinsicNode(useSSE ? X86ISD::FILD_FLAG :
                                           X86ISD::FILD, DL,
                                           Tys, Ops, SrcVT, MMO);

  if (useSSE) {
    Chain = Result.getValue(1);
    SDValue InFlag = Result.getValue(2);

    // FIXME: Currently the FST is flagged to the FILD_FLAG. This
    // shouldn't be necessary except that RFP cannot be live across
    // multiple blocks. When stackifier is fixed, they can be uncoupled.
    MachineFunction &MF = DAG.getMachineFunction();
    unsigned SSFISize = Op.getValueType().getSizeInBits()/8;
    int SSFI = MF.getFrameInfo()->CreateStackObject(SSFISize, SSFISize, false);
    SDValue StackSlot = DAG.getFrameIndex(SSFI, getPointerTy());
    Tys = DAG.getVTList(MVT::Other);
    SDValue Ops[] = {
      Chain, Result, StackSlot, DAG.getValueType(Op.getValueType()), InFlag
    };
    MachineMemOperand *MMO =
      DAG.getMachineFunction()
      .getMachineMemOperand(MachinePointerInfo::getFixedStack(SSFI),
                            MachineMemOperand::MOStore, SSFISize, SSFISize);

    Chain = DAG.getMemIntrinsicNode(X86ISD::FST, DL, Tys,
                                    Ops, Op.getValueType(), MMO);
    Result = DAG.getLoad(Op.getValueType(), DL, Chain, StackSlot,
                         MachinePointerInfo::getFixedStack(SSFI),
                         false, false, false, 0);
  }

  return Result;
}

// LowerUINT_TO_FP_i64 - 64-bit unsigned integer to double expansion.
SDValue X86TargetLowering::LowerUINT_TO_FP_i64(SDValue Op,
                                               SelectionDAG &DAG) const {
  // This algorithm is not obvious. Here it is what we're trying to output:
  /*
     movq       %rax,  %xmm0
     punpckldq  (c0),  %xmm0  // c0: (uint4){ 0x43300000U, 0x45300000U, 0U, 0U }
     subpd      (c1),  %xmm0  // c1: (double2){ 0x1.0p52, 0x1.0p52 * 0x1.0p32 }
     #ifdef __SSE3__
       haddpd   %xmm0, %xmm0
     #else
       pshufd   $0x4e, %xmm0, %xmm1
       addpd    %xmm1, %xmm0
     #endif
  */

  SDLoc dl(Op);
  LLVMContext *Context = DAG.getContext();

  // Build some magic constants.
  static const uint32_t CV0[] = { 0x43300000, 0x45300000, 0, 0 };
  Constant *C0 = ConstantDataVector::get(*Context, CV0);
  SDValue CPIdx0 = DAG.getConstantPool(C0, getPointerTy(), 16);

  SmallVector<Constant*,2> CV1;
  CV1.push_back(
    ConstantFP::get(*Context, APFloat(APFloat::IEEEdouble,
                                      APInt(64, 0x4330000000000000ULL))));
  CV1.push_back(
    ConstantFP::get(*Context, APFloat(APFloat::IEEEdouble,
                                      APInt(64, 0x4530000000000000ULL))));
  Constant *C1 = ConstantVector::get(CV1);
  SDValue CPIdx1 = DAG.getConstantPool(C1, getPointerTy(), 16);

  // Load the 64-bit value into an XMM register.
  SDValue XR1 = DAG.getNode(ISD::SCALAR_TO_VECTOR, dl, MVT::v2i64,
                            Op.getOperand(0));
  SDValue CLod0 = DAG.getLoad(MVT::v4i32, dl, DAG.getEntryNode(), CPIdx0,
                              MachinePointerInfo::getConstantPool(),
                              false, false, false, 16);
  SDValue Unpck1 = getUnpackl(DAG, dl, MVT::v4i32,
                              DAG.getNode(ISD::BITCAST, dl, MVT::v4i32, XR1),
                              CLod0);

  SDValue CLod1 = DAG.getLoad(MVT::v2f64, dl, CLod0.getValue(1), CPIdx1,
                              MachinePointerInfo::getConstantPool(),
                              false, false, false, 16);
  SDValue XR2F = DAG.getNode(ISD::BITCAST, dl, MVT::v2f64, Unpck1);
  SDValue Sub = DAG.getNode(ISD::FSUB, dl, MVT::v2f64, XR2F, CLod1);
  SDValue Result;

  if (Subtarget->hasSSE3()) {
    // FIXME: The 'haddpd' instruction may be slower than 'movhlps + addsd'.
    Result = DAG.getNode(X86ISD::FHADD, dl, MVT::v2f64, Sub, Sub);
  } else {
    SDValue S2F = DAG.getNode(ISD::BITCAST, dl, MVT::v4i32, Sub);
    SDValue Shuffle = getTargetShuffleNode(X86ISD::PSHUFD, dl, MVT::v4i32,
                                           S2F, 0x4E, DAG);
    Result = DAG.getNode(ISD::FADD, dl, MVT::v2f64,
                         DAG.getNode(ISD::BITCAST, dl, MVT::v2f64, Shuffle),
                         Sub);
  }

  return DAG.getNode(ISD::EXTRACT_VECTOR_ELT, dl, MVT::f64, Result,
                     DAG.getIntPtrConstant(0));
}

// LowerUINT_TO_FP_i32 - 32-bit unsigned integer to float expansion.
SDValue X86TargetLowering::LowerUINT_TO_FP_i32(SDValue Op,
                                               SelectionDAG &DAG) const {
  SDLoc dl(Op);
  // FP constant to bias correct the final result.
  SDValue Bias = DAG.getConstantFP(BitsToDouble(0x4330000000000000ULL),
                                   MVT::f64);

  // Load the 32-bit value into an XMM register.
  SDValue Load = DAG.getNode(ISD::SCALAR_TO_VECTOR, dl, MVT::v4i32,
                             Op.getOperand(0));

  // Zero out the upper parts of the register.
  Load = getShuffleVectorZeroOrUndef(Load, 0, true, Subtarget, DAG);

  Load = DAG.getNode(ISD::EXTRACT_VECTOR_ELT, dl, MVT::f64,
                     DAG.getNode(ISD::BITCAST, dl, MVT::v2f64, Load),
                     DAG.getIntPtrConstant(0));

  // Or the load with the bias.
  SDValue Or = DAG.getNode(ISD::OR, dl, MVT::v2i64,
                           DAG.getNode(ISD::BITCAST, dl, MVT::v2i64,
                                       DAG.getNode(ISD::SCALAR_TO_VECTOR, dl,
                                                   MVT::v2f64, Load)),
                           DAG.getNode(ISD::BITCAST, dl, MVT::v2i64,
                                       DAG.getNode(ISD::SCALAR_TO_VECTOR, dl,
                                                   MVT::v2f64, Bias)));
  Or = DAG.getNode(ISD::EXTRACT_VECTOR_ELT, dl, MVT::f64,
                   DAG.getNode(ISD::BITCAST, dl, MVT::v2f64, Or),
                   DAG.getIntPtrConstant(0));

  // Subtract the bias.
  SDValue Sub = DAG.getNode(ISD::FSUB, dl, MVT::f64, Or, Bias);

  // Handle final rounding.
  EVT DestVT = Op.getValueType();

  if (DestVT.bitsLT(MVT::f64))
    return DAG.getNode(ISD::FP_ROUND, dl, DestVT, Sub,
                       DAG.getIntPtrConstant(0));
  if (DestVT.bitsGT(MVT::f64))
    return DAG.getNode(ISD::FP_EXTEND, dl, DestVT, Sub);

  // Handle final rounding.
  return Sub;
}

SDValue X86TargetLowering::lowerUINT_TO_FP_vec(SDValue Op,
                                               SelectionDAG &DAG) const {
  SDValue N0 = Op.getOperand(0);
  MVT SVT = N0.getSimpleValueType();
  SDLoc dl(Op);

  assert((SVT == MVT::v4i8 || SVT == MVT::v4i16 ||
          SVT == MVT::v8i8 || SVT == MVT::v8i16) &&
         "Custom UINT_TO_FP is not supported!");

  MVT NVT = MVT::getVectorVT(MVT::i32, SVT.getVectorNumElements());
  return DAG.getNode(ISD::SINT_TO_FP, dl, Op.getValueType(),
                     DAG.getNode(ISD::ZERO_EXTEND, dl, NVT, N0));
}

SDValue X86TargetLowering::LowerUINT_TO_FP(SDValue Op,
                                           SelectionDAG &DAG) const {
  SDValue N0 = Op.getOperand(0);
  SDLoc dl(Op);

  if (Op.getValueType().isVector())
    return lowerUINT_TO_FP_vec(Op, DAG);

  // Since UINT_TO_FP is legal (it's marked custom), dag combiner won't
  // optimize it to a SINT_TO_FP when the sign bit is known zero. Perform
  // the optimization here.
  if (DAG.SignBitIsZero(N0))
    return DAG.getNode(ISD::SINT_TO_FP, dl, Op.getValueType(), N0);

  MVT SrcVT = N0.getSimpleValueType();
  MVT DstVT = Op.getSimpleValueType();
  if (SrcVT == MVT::i64 && DstVT == MVT::f64 && X86ScalarSSEf64)
    return LowerUINT_TO_FP_i64(Op, DAG);
  if (SrcVT == MVT::i32 && X86ScalarSSEf64)
    return LowerUINT_TO_FP_i32(Op, DAG);
  if (Subtarget->is64Bit() && SrcVT == MVT::i64 && DstVT == MVT::f32)
    return SDValue();

  // Make a 64-bit buffer, and use it to build an FILD.
  SDValue StackSlot = DAG.CreateStackTemporary(MVT::i64);
  if (SrcVT == MVT::i32) {
    SDValue WordOff = DAG.getConstant(4, getPointerTy());
    SDValue OffsetSlot = DAG.getNode(ISD::ADD, dl,
                                     getPointerTy(), StackSlot, WordOff);
    SDValue Store1 = DAG.getStore(DAG.getEntryNode(), dl, Op.getOperand(0),
                                  StackSlot, MachinePointerInfo(),
                                  false, false, 0);
    SDValue Store2 = DAG.getStore(Store1, dl, DAG.getConstant(0, MVT::i32),
                                  OffsetSlot, MachinePointerInfo(),
                                  false, false, 0);
    SDValue Fild = BuildFILD(Op, MVT::i64, Store2, StackSlot, DAG);
    return Fild;
  }

  assert(SrcVT == MVT::i64 && "Unexpected type in UINT_TO_FP");
  SDValue Store = DAG.getStore(DAG.getEntryNode(), dl, Op.getOperand(0),
                               StackSlot, MachinePointerInfo(),
                               false, false, 0);
  // For i64 source, we need to add the appropriate power of 2 if the input
  // was negative.  This is the same as the optimization in
  // DAGTypeLegalizer::ExpandIntOp_UNIT_TO_FP, and for it to be safe here,
  // we must be careful to do the computation in x87 extended precision, not
  // in SSE. (The generic code can't know it's OK to do this, or how to.)
  int SSFI = cast<FrameIndexSDNode>(StackSlot)->getIndex();
  MachineMemOperand *MMO =
    DAG.getMachineFunction()
    .getMachineMemOperand(MachinePointerInfo::getFixedStack(SSFI),
                          MachineMemOperand::MOLoad, 8, 8);

  SDVTList Tys = DAG.getVTList(MVT::f80, MVT::Other);
  SDValue Ops[] = { Store, StackSlot, DAG.getValueType(MVT::i64) };
  SDValue Fild = DAG.getMemIntrinsicNode(X86ISD::FILD, dl, Tys, Ops,
                                         MVT::i64, MMO);

  APInt FF(32, 0x5F800000ULL);

  // Check whether the sign bit is set.
  SDValue SignSet = DAG.getSetCC(dl,
                                 getSetCCResultType(*DAG.getContext(), MVT::i64),
                                 Op.getOperand(0), DAG.getConstant(0, MVT::i64),
                                 ISD::SETLT);

  // Build a 64 bit pair (0, FF) in the constant pool, with FF in the lo bits.
  SDValue FudgePtr = DAG.getConstantPool(
                             ConstantInt::get(*DAG.getContext(), FF.zext(64)),
                                         getPointerTy());

  // Get a pointer to FF if the sign bit was set, or to 0 otherwise.
  SDValue Zero = DAG.getIntPtrConstant(0);
  SDValue Four = DAG.getIntPtrConstant(4);
  SDValue Offset = DAG.getNode(ISD::SELECT, dl, Zero.getValueType(), SignSet,
                               Zero, Four);
  FudgePtr = DAG.getNode(ISD::ADD, dl, getPointerTy(), FudgePtr, Offset);

  // Load the value out, extending it from f32 to f80.
  // FIXME: Avoid the extend by constructing the right constant pool?
  SDValue Fudge = DAG.getExtLoad(ISD::EXTLOAD, dl, MVT::f80, DAG.getEntryNode(),
                                 FudgePtr, MachinePointerInfo::getConstantPool(),
                                 MVT::f32, false, false, false, 4);
  // Extend everything to 80 bits to force it to be done on x87.
  SDValue Add = DAG.getNode(ISD::FADD, dl, MVT::f80, Fild, Fudge);
  return DAG.getNode(ISD::FP_ROUND, dl, DstVT, Add, DAG.getIntPtrConstant(0));
}

std::pair<SDValue,SDValue>
X86TargetLowering:: FP_TO_INTHelper(SDValue Op, SelectionDAG &DAG,
                                    bool IsSigned, bool IsReplace) const {
  SDLoc DL(Op);

  EVT DstTy = Op.getValueType();

  if (!IsSigned && !isIntegerTypeFTOL(DstTy)) {
    assert(DstTy == MVT::i32 && "Unexpected FP_TO_UINT");
    DstTy = MVT::i64;
  }

  assert(DstTy.getSimpleVT() <= MVT::i64 &&
         DstTy.getSimpleVT() >= MVT::i16 &&
         "Unknown FP_TO_INT to lower!");

  // These are really Legal.
  if (DstTy == MVT::i32 &&
      isScalarFPTypeInSSEReg(Op.getOperand(0).getValueType()))
    return std::make_pair(SDValue(), SDValue());
  if (Subtarget->is64Bit() &&
      DstTy == MVT::i64 &&
      isScalarFPTypeInSSEReg(Op.getOperand(0).getValueType()))
    return std::make_pair(SDValue(), SDValue());

  // We lower FP->int64 either into FISTP64 followed by a load from a temporary
  // stack slot, or into the FTOL runtime function.
  MachineFunction &MF = DAG.getMachineFunction();
  unsigned MemSize = DstTy.getSizeInBits()/8;
  int SSFI = MF.getFrameInfo()->CreateStackObject(MemSize, MemSize, false);
  SDValue StackSlot = DAG.getFrameIndex(SSFI, getPointerTy());

  unsigned Opc;
  if (!IsSigned && isIntegerTypeFTOL(DstTy))
    Opc = X86ISD::WIN_FTOL;
  else
    switch (DstTy.getSimpleVT().SimpleTy) {
    default: llvm_unreachable("Invalid FP_TO_SINT to lower!");
    case MVT::i16: Opc = X86ISD::FP_TO_INT16_IN_MEM; break;
    case MVT::i32: Opc = X86ISD::FP_TO_INT32_IN_MEM; break;
    case MVT::i64: Opc = X86ISD::FP_TO_INT64_IN_MEM; break;
    }

  SDValue Chain = DAG.getEntryNode();
  SDValue Value = Op.getOperand(0);
  EVT TheVT = Op.getOperand(0).getValueType();
  // FIXME This causes a redundant load/store if the SSE-class value is already
  // in memory, such as if it is on the callstack.
  if (isScalarFPTypeInSSEReg(TheVT)) {
    assert(DstTy == MVT::i64 && "Invalid FP_TO_SINT to lower!");
    Chain = DAG.getStore(Chain, DL, Value, StackSlot,
                         MachinePointerInfo::getFixedStack(SSFI),
                         false, false, 0);
    SDVTList Tys = DAG.getVTList(Op.getOperand(0).getValueType(), MVT::Other);
    SDValue Ops[] = {
      Chain, StackSlot, DAG.getValueType(TheVT)
    };

    MachineMemOperand *MMO =
      MF.getMachineMemOperand(MachinePointerInfo::getFixedStack(SSFI),
                              MachineMemOperand::MOLoad, MemSize, MemSize);
    Value = DAG.getMemIntrinsicNode(X86ISD::FLD, DL, Tys, Ops, DstTy, MMO);
    Chain = Value.getValue(1);
    SSFI = MF.getFrameInfo()->CreateStackObject(MemSize, MemSize, false);
    StackSlot = DAG.getFrameIndex(SSFI, getPointerTy());
  }

  MachineMemOperand *MMO =
    MF.getMachineMemOperand(MachinePointerInfo::getFixedStack(SSFI),
                            MachineMemOperand::MOStore, MemSize, MemSize);

  if (Opc != X86ISD::WIN_FTOL) {
    // Build the FP_TO_INT*_IN_MEM
    SDValue Ops[] = { Chain, Value, StackSlot };
    SDValue FIST = DAG.getMemIntrinsicNode(Opc, DL, DAG.getVTList(MVT::Other),
                                           Ops, DstTy, MMO);
    return std::make_pair(FIST, StackSlot);
  } else {
    SDValue ftol = DAG.getNode(X86ISD::WIN_FTOL, DL,
      DAG.getVTList(MVT::Other, MVT::Glue),
      Chain, Value);
    SDValue eax = DAG.getCopyFromReg(ftol, DL, X86::EAX,
      MVT::i32, ftol.getValue(1));
    SDValue edx = DAG.getCopyFromReg(eax.getValue(1), DL, X86::EDX,
      MVT::i32, eax.getValue(2));
    SDValue Ops[] = { eax, edx };
    SDValue pair = IsReplace
      ? DAG.getNode(ISD::BUILD_PAIR, DL, MVT::i64, Ops)
      : DAG.getMergeValues(Ops, DL);
    return std::make_pair(pair, SDValue());
  }
}

static SDValue LowerAVXExtend(SDValue Op, SelectionDAG &DAG,
                              const X86Subtarget *Subtarget) {
  MVT VT = Op->getSimpleValueType(0);
  SDValue In = Op->getOperand(0);
  MVT InVT = In.getSimpleValueType();
  SDLoc dl(Op);

  // Optimize vectors in AVX mode:
  //
  //   v8i16 -> v8i32
  //   Use vpunpcklwd for 4 lower elements  v8i16 -> v4i32.
  //   Use vpunpckhwd for 4 upper elements  v8i16 -> v4i32.
  //   Concat upper and lower parts.
  //
  //   v4i32 -> v4i64
  //   Use vpunpckldq for 4 lower elements  v4i32 -> v2i64.
  //   Use vpunpckhdq for 4 upper elements  v4i32 -> v2i64.
  //   Concat upper and lower parts.
  //

  if (((VT != MVT::v16i16) || (InVT != MVT::v16i8)) &&
      ((VT != MVT::v8i32) || (InVT != MVT::v8i16)) &&
      ((VT != MVT::v4i64) || (InVT != MVT::v4i32)))
    return SDValue();

  if (Subtarget->hasInt256())
    return DAG.getNode(X86ISD::VZEXT, dl, VT, In);

  SDValue ZeroVec = getZeroVector(InVT, Subtarget, DAG, dl);
  SDValue Undef = DAG.getUNDEF(InVT);
  bool NeedZero = Op.getOpcode() == ISD::ZERO_EXTEND;
  SDValue OpLo = getUnpackl(DAG, dl, InVT, In, NeedZero ? ZeroVec : Undef);
  SDValue OpHi = getUnpackh(DAG, dl, InVT, In, NeedZero ? ZeroVec : Undef);

  MVT HVT = MVT::getVectorVT(VT.getVectorElementType(),
                             VT.getVectorNumElements()/2);

  OpLo = DAG.getNode(ISD::BITCAST, dl, HVT, OpLo);
  OpHi = DAG.getNode(ISD::BITCAST, dl, HVT, OpHi);

  return DAG.getNode(ISD::CONCAT_VECTORS, dl, VT, OpLo, OpHi);
}

static  SDValue LowerZERO_EXTEND_AVX512(SDValue Op,
                                        SelectionDAG &DAG) {
  MVT VT = Op->getSimpleValueType(0);
  SDValue In = Op->getOperand(0);
  MVT InVT = In.getSimpleValueType();
  SDLoc DL(Op);
  unsigned int NumElts = VT.getVectorNumElements();
  if (NumElts != 8 && NumElts != 16)
    return SDValue();

  if (VT.is512BitVector() && InVT.getVectorElementType() != MVT::i1)
    return DAG.getNode(X86ISD::VZEXT, DL, VT, In);

  EVT ExtVT = (NumElts == 8)? MVT::v8i64 : MVT::v16i32;
  const TargetLowering &TLI = DAG.getTargetLoweringInfo();
  // Now we have only mask extension
  assert(InVT.getVectorElementType() == MVT::i1);
  SDValue Cst = DAG.getTargetConstant(1, ExtVT.getScalarType());
  const Constant *C = (dyn_cast<ConstantSDNode>(Cst))->getConstantIntValue();
  SDValue CP = DAG.getConstantPool(C, TLI.getPointerTy());
  unsigned Alignment = cast<ConstantPoolSDNode>(CP)->getAlignment();
  SDValue Ld = DAG.getLoad(Cst.getValueType(), DL, DAG.getEntryNode(), CP,
                           MachinePointerInfo::getConstantPool(),
                           false, false, false, Alignment);

  SDValue Brcst = DAG.getNode(X86ISD::VBROADCASTM, DL, ExtVT, In, Ld);
  if (VT.is512BitVector())
    return Brcst;
  return DAG.getNode(X86ISD::VTRUNC, DL, VT, Brcst);
}

static SDValue LowerANY_EXTEND(SDValue Op, const X86Subtarget *Subtarget,
                               SelectionDAG &DAG) {
  if (Subtarget->hasFp256()) {
    SDValue Res = LowerAVXExtend(Op, DAG, Subtarget);
    if (Res.getNode())
      return Res;
  }

  return SDValue();
}

static SDValue LowerZERO_EXTEND(SDValue Op, const X86Subtarget *Subtarget,
                                SelectionDAG &DAG) {
  SDLoc DL(Op);
  MVT VT = Op.getSimpleValueType();
  SDValue In = Op.getOperand(0);
  MVT SVT = In.getSimpleValueType();

  if (VT.is512BitVector() || SVT.getVectorElementType() == MVT::i1)
    return LowerZERO_EXTEND_AVX512(Op, DAG);

  if (Subtarget->hasFp256()) {
    SDValue Res = LowerAVXExtend(Op, DAG, Subtarget);
    if (Res.getNode())
      return Res;
  }

  assert(!VT.is256BitVector() || !SVT.is128BitVector() ||
         VT.getVectorNumElements() != SVT.getVectorNumElements());
  return SDValue();
}

SDValue X86TargetLowering::LowerTRUNCATE(SDValue Op, SelectionDAG &DAG) const {
  SDLoc DL(Op);
  MVT VT = Op.getSimpleValueType();
  SDValue In = Op.getOperand(0);
  MVT InVT = In.getSimpleValueType();

  if (VT == MVT::i1) {
    assert((InVT.isInteger() && (InVT.getSizeInBits() <= 64)) &&
           "Invalid scalar TRUNCATE operation");
    if (InVT.getSizeInBits() >= 32)
      return SDValue();
    In = DAG.getNode(ISD::ANY_EXTEND, DL, MVT::i32, In);
    return DAG.getNode(ISD::TRUNCATE, DL, VT, In);
  }
  assert(VT.getVectorNumElements() == InVT.getVectorNumElements() &&
         "Invalid TRUNCATE operation");

  if (InVT.is512BitVector() || VT.getVectorElementType() == MVT::i1) {
    if (VT.getVectorElementType().getSizeInBits() >=8)
      return DAG.getNode(X86ISD::VTRUNC, DL, VT, In);

    assert(VT.getVectorElementType() == MVT::i1 && "Unexpected vector type");
    unsigned NumElts = InVT.getVectorNumElements();
    assert ((NumElts == 8 || NumElts == 16) && "Unexpected vector type");
    if (InVT.getSizeInBits() < 512) {
      MVT ExtVT = (NumElts == 16)? MVT::v16i32 : MVT::v8i64;
      In = DAG.getNode(ISD::SIGN_EXTEND, DL, ExtVT, In);
      InVT = ExtVT;
    }
    
    SDValue Cst = DAG.getTargetConstant(1, InVT.getVectorElementType());
    const Constant *C = (dyn_cast<ConstantSDNode>(Cst))->getConstantIntValue();
    SDValue CP = DAG.getConstantPool(C, getPointerTy());
    unsigned Alignment = cast<ConstantPoolSDNode>(CP)->getAlignment();
    SDValue Ld = DAG.getLoad(Cst.getValueType(), DL, DAG.getEntryNode(), CP,
                           MachinePointerInfo::getConstantPool(),
                           false, false, false, Alignment);
    SDValue OneV = DAG.getNode(X86ISD::VBROADCAST, DL, InVT, Ld);
    SDValue And = DAG.getNode(ISD::AND, DL, InVT, OneV, In);
    return DAG.getNode(X86ISD::TESTM, DL, VT, And, And);
  }

  if ((VT == MVT::v4i32) && (InVT == MVT::v4i64)) {
    // On AVX2, v4i64 -> v4i32 becomes VPERMD.
    if (Subtarget->hasInt256()) {
      static const int ShufMask[] = {0, 2, 4, 6, -1, -1, -1, -1};
      In = DAG.getNode(ISD::BITCAST, DL, MVT::v8i32, In);
      In = DAG.getVectorShuffle(MVT::v8i32, DL, In, DAG.getUNDEF(MVT::v8i32),
                                ShufMask);
      return DAG.getNode(ISD::EXTRACT_SUBVECTOR, DL, VT, In,
                         DAG.getIntPtrConstant(0));
    }

    SDValue OpLo = DAG.getNode(ISD::EXTRACT_SUBVECTOR, DL, MVT::v2i64, In,
                               DAG.getIntPtrConstant(0));
    SDValue OpHi = DAG.getNode(ISD::EXTRACT_SUBVECTOR, DL, MVT::v2i64, In,
                               DAG.getIntPtrConstant(2));
    OpLo = DAG.getNode(ISD::BITCAST, DL, MVT::v4i32, OpLo);
    OpHi = DAG.getNode(ISD::BITCAST, DL, MVT::v4i32, OpHi);
    static const int ShufMask[] = {0, 2, 4, 6};
    return DAG.getVectorShuffle(VT, DL, OpLo, OpHi, ShufMask);
  }

  if ((VT == MVT::v8i16) && (InVT == MVT::v8i32)) {
    // On AVX2, v8i32 -> v8i16 becomed PSHUFB.
    if (Subtarget->hasInt256()) {
      In = DAG.getNode(ISD::BITCAST, DL, MVT::v32i8, In);

      SmallVector<SDValue,32> pshufbMask;
      for (unsigned i = 0; i < 2; ++i) {
        pshufbMask.push_back(DAG.getConstant(0x0, MVT::i8));
        pshufbMask.push_back(DAG.getConstant(0x1, MVT::i8));
        pshufbMask.push_back(DAG.getConstant(0x4, MVT::i8));
        pshufbMask.push_back(DAG.getConstant(0x5, MVT::i8));
        pshufbMask.push_back(DAG.getConstant(0x8, MVT::i8));
        pshufbMask.push_back(DAG.getConstant(0x9, MVT::i8));
        pshufbMask.push_back(DAG.getConstant(0xc, MVT::i8));
        pshufbMask.push_back(DAG.getConstant(0xd, MVT::i8));
        for (unsigned j = 0; j < 8; ++j)
          pshufbMask.push_back(DAG.getConstant(0x80, MVT::i8));
      }
      SDValue BV = DAG.getNode(ISD::BUILD_VECTOR, DL, MVT::v32i8, pshufbMask);
      In = DAG.getNode(X86ISD::PSHUFB, DL, MVT::v32i8, In, BV);
      In = DAG.getNode(ISD::BITCAST, DL, MVT::v4i64, In);

      static const int ShufMask[] = {0,  2,  -1,  -1};
      In = DAG.getVectorShuffle(MVT::v4i64, DL,  In, DAG.getUNDEF(MVT::v4i64),
                                &ShufMask[0]);
      In = DAG.getNode(ISD::EXTRACT_SUBVECTOR, DL, MVT::v2i64, In,
                       DAG.getIntPtrConstant(0));
      return DAG.getNode(ISD::BITCAST, DL, VT, In);
    }

    SDValue OpLo = DAG.getNode(ISD::EXTRACT_SUBVECTOR, DL, MVT::v4i32, In,
                               DAG.getIntPtrConstant(0));

    SDValue OpHi = DAG.getNode(ISD::EXTRACT_SUBVECTOR, DL, MVT::v4i32, In,
                               DAG.getIntPtrConstant(4));

    OpLo = DAG.getNode(ISD::BITCAST, DL, MVT::v16i8, OpLo);
    OpHi = DAG.getNode(ISD::BITCAST, DL, MVT::v16i8, OpHi);

    // The PSHUFB mask:
    static const int ShufMask1[] = {0,  1,  4,  5,  8,  9, 12, 13,
                                   -1, -1, -1, -1, -1, -1, -1, -1};

    SDValue Undef = DAG.getUNDEF(MVT::v16i8);
    OpLo = DAG.getVectorShuffle(MVT::v16i8, DL, OpLo, Undef, ShufMask1);
    OpHi = DAG.getVectorShuffle(MVT::v16i8, DL, OpHi, Undef, ShufMask1);

    OpLo = DAG.getNode(ISD::BITCAST, DL, MVT::v4i32, OpLo);
    OpHi = DAG.getNode(ISD::BITCAST, DL, MVT::v4i32, OpHi);

    // The MOVLHPS Mask:
    static const int ShufMask2[] = {0, 1, 4, 5};
    SDValue res = DAG.getVectorShuffle(MVT::v4i32, DL, OpLo, OpHi, ShufMask2);
    return DAG.getNode(ISD::BITCAST, DL, MVT::v8i16, res);
  }

  // Handle truncation of V256 to V128 using shuffles.
  if (!VT.is128BitVector() || !InVT.is256BitVector())
    return SDValue();

  assert(Subtarget->hasFp256() && "256-bit vector without AVX!");

  unsigned NumElems = VT.getVectorNumElements();
  MVT NVT = MVT::getVectorVT(VT.getVectorElementType(), NumElems * 2);

  SmallVector<int, 16> MaskVec(NumElems * 2, -1);
  // Prepare truncation shuffle mask
  for (unsigned i = 0; i != NumElems; ++i)
    MaskVec[i] = i * 2;
  SDValue V = DAG.getVectorShuffle(NVT, DL,
                                   DAG.getNode(ISD::BITCAST, DL, NVT, In),
                                   DAG.getUNDEF(NVT), &MaskVec[0]);
  return DAG.getNode(ISD::EXTRACT_SUBVECTOR, DL, VT, V,
                     DAG.getIntPtrConstant(0));
}

SDValue X86TargetLowering::LowerFP_TO_SINT(SDValue Op,
                                           SelectionDAG &DAG) const {
  assert(!Op.getSimpleValueType().isVector());

  std::pair<SDValue,SDValue> Vals = FP_TO_INTHelper(Op, DAG,
    /*IsSigned=*/ true, /*IsReplace=*/ false);
  SDValue FIST = Vals.first, StackSlot = Vals.second;
  // If FP_TO_INTHelper failed, the node is actually supposed to be Legal.
  if (!FIST.getNode()) return Op;

  if (StackSlot.getNode())
    // Load the result.
    return DAG.getLoad(Op.getValueType(), SDLoc(Op),
                       FIST, StackSlot, MachinePointerInfo(),
                       false, false, false, 0);

  // The node is the result.
  return FIST;
}

SDValue X86TargetLowering::LowerFP_TO_UINT(SDValue Op,
                                           SelectionDAG &DAG) const {
  std::pair<SDValue,SDValue> Vals = FP_TO_INTHelper(Op, DAG,
    /*IsSigned=*/ false, /*IsReplace=*/ false);
  SDValue FIST = Vals.first, StackSlot = Vals.second;
  assert(FIST.getNode() && "Unexpected failure");

  if (StackSlot.getNode())
    // Load the result.
    return DAG.getLoad(Op.getValueType(), SDLoc(Op),
                       FIST, StackSlot, MachinePointerInfo(),
                       false, false, false, 0);

  // The node is the result.
  return FIST;
}

static SDValue LowerFP_EXTEND(SDValue Op, SelectionDAG &DAG) {
  SDLoc DL(Op);
  MVT VT = Op.getSimpleValueType();
  SDValue In = Op.getOperand(0);
  MVT SVT = In.getSimpleValueType();

  assert(SVT == MVT::v2f32 && "Only customize MVT::v2f32 type legalization!");

  return DAG.getNode(X86ISD::VFPEXT, DL, VT,
                     DAG.getNode(ISD::CONCAT_VECTORS, DL, MVT::v4f32,
                                 In, DAG.getUNDEF(SVT)));
}

static SDValue LowerFABS(SDValue Op, SelectionDAG &DAG) {
  LLVMContext *Context = DAG.getContext();
  SDLoc dl(Op);
  MVT VT = Op.getSimpleValueType();
  MVT EltVT = VT;
  unsigned NumElts = VT == MVT::f64 ? 2 : 4;
  if (VT.isVector()) {
    EltVT = VT.getVectorElementType();
    NumElts = VT.getVectorNumElements();
  }
  Constant *C;
  if (EltVT == MVT::f64)
    C = ConstantFP::get(*Context, APFloat(APFloat::IEEEdouble,
                                          APInt(64, ~(1ULL << 63))));
  else
    C = ConstantFP::get(*Context, APFloat(APFloat::IEEEsingle,
                                          APInt(32, ~(1U << 31))));
  C = ConstantVector::getSplat(NumElts, C);
  const TargetLowering &TLI = DAG.getTargetLoweringInfo();
  SDValue CPIdx = DAG.getConstantPool(C, TLI.getPointerTy());
  unsigned Alignment = cast<ConstantPoolSDNode>(CPIdx)->getAlignment();
  SDValue Mask = DAG.getLoad(VT, dl, DAG.getEntryNode(), CPIdx,
                             MachinePointerInfo::getConstantPool(),
                             false, false, false, Alignment);
  if (VT.isVector()) {
    MVT ANDVT = VT.is128BitVector() ? MVT::v2i64 : MVT::v4i64;
    return DAG.getNode(ISD::BITCAST, dl, VT,
                       DAG.getNode(ISD::AND, dl, ANDVT,
                                   DAG.getNode(ISD::BITCAST, dl, ANDVT,
                                               Op.getOperand(0)),
                                   DAG.getNode(ISD::BITCAST, dl, ANDVT, Mask)));
  }
  return DAG.getNode(X86ISD::FAND, dl, VT, Op.getOperand(0), Mask);
}

static SDValue LowerFNEG(SDValue Op, SelectionDAG &DAG) {
  LLVMContext *Context = DAG.getContext();
  SDLoc dl(Op);
  MVT VT = Op.getSimpleValueType();
  MVT EltVT = VT;
  unsigned NumElts = VT == MVT::f64 ? 2 : 4;
  if (VT.isVector()) {
    EltVT = VT.getVectorElementType();
    NumElts = VT.getVectorNumElements();
  }
  Constant *C;
  if (EltVT == MVT::f64)
    C = ConstantFP::get(*Context, APFloat(APFloat::IEEEdouble,
                                          APInt(64, 1ULL << 63)));
  else
    C = ConstantFP::get(*Context, APFloat(APFloat::IEEEsingle,
                                          APInt(32, 1U << 31)));
  C = ConstantVector::getSplat(NumElts, C);
  const TargetLowering &TLI = DAG.getTargetLoweringInfo();
  SDValue CPIdx = DAG.getConstantPool(C, TLI.getPointerTy());
  unsigned Alignment = cast<ConstantPoolSDNode>(CPIdx)->getAlignment();
  SDValue Mask = DAG.getLoad(VT, dl, DAG.getEntryNode(), CPIdx,
                             MachinePointerInfo::getConstantPool(),
                             false, false, false, Alignment);
  if (VT.isVector()) {
    MVT XORVT = MVT::getVectorVT(MVT::i64, VT.getSizeInBits()/64);
    return DAG.getNode(ISD::BITCAST, dl, VT,
                       DAG.getNode(ISD::XOR, dl, XORVT,
                                   DAG.getNode(ISD::BITCAST, dl, XORVT,
                                               Op.getOperand(0)),
                                   DAG.getNode(ISD::BITCAST, dl, XORVT, Mask)));
  }

  return DAG.getNode(X86ISD::FXOR, dl, VT, Op.getOperand(0), Mask);
}

static SDValue LowerFCOPYSIGN(SDValue Op, SelectionDAG &DAG) {
  const TargetLowering &TLI = DAG.getTargetLoweringInfo();
  LLVMContext *Context = DAG.getContext();
  SDValue Op0 = Op.getOperand(0);
  SDValue Op1 = Op.getOperand(1);
  SDLoc dl(Op);
  MVT VT = Op.getSimpleValueType();
  MVT SrcVT = Op1.getSimpleValueType();

  // If second operand is smaller, extend it first.
  if (SrcVT.bitsLT(VT)) {
    Op1 = DAG.getNode(ISD::FP_EXTEND, dl, VT, Op1);
    SrcVT = VT;
  }
  // And if it is bigger, shrink it first.
  if (SrcVT.bitsGT(VT)) {
    Op1 = DAG.getNode(ISD::FP_ROUND, dl, VT, Op1, DAG.getIntPtrConstant(1));
    SrcVT = VT;
  }

  // At this point the operands and the result should have the same
  // type, and that won't be f80 since that is not custom lowered.

  // First get the sign bit of second operand.
  SmallVector<Constant*,4> CV;
  if (SrcVT == MVT::f64) {
    const fltSemantics &Sem = APFloat::IEEEdouble;
    CV.push_back(ConstantFP::get(*Context, APFloat(Sem, APInt(64, 1ULL << 63))));
    CV.push_back(ConstantFP::get(*Context, APFloat(Sem, APInt(64, 0))));
  } else {
    const fltSemantics &Sem = APFloat::IEEEsingle;
    CV.push_back(ConstantFP::get(*Context, APFloat(Sem, APInt(32, 1U << 31))));
    CV.push_back(ConstantFP::get(*Context, APFloat(Sem, APInt(32, 0))));
    CV.push_back(ConstantFP::get(*Context, APFloat(Sem, APInt(32, 0))));
    CV.push_back(ConstantFP::get(*Context, APFloat(Sem, APInt(32, 0))));
  }
  Constant *C = ConstantVector::get(CV);
  SDValue CPIdx = DAG.getConstantPool(C, TLI.getPointerTy(), 16);
  SDValue Mask1 = DAG.getLoad(SrcVT, dl, DAG.getEntryNode(), CPIdx,
                              MachinePointerInfo::getConstantPool(),
                              false, false, false, 16);
  SDValue SignBit = DAG.getNode(X86ISD::FAND, dl, SrcVT, Op1, Mask1);

  // Shift sign bit right or left if the two operands have different types.
  if (SrcVT.bitsGT(VT)) {
    // Op0 is MVT::f32, Op1 is MVT::f64.
    SignBit = DAG.getNode(ISD::SCALAR_TO_VECTOR, dl, MVT::v2f64, SignBit);
    SignBit = DAG.getNode(X86ISD::FSRL, dl, MVT::v2f64, SignBit,
                          DAG.getConstant(32, MVT::i32));
    SignBit = DAG.getNode(ISD::BITCAST, dl, MVT::v4f32, SignBit);
    SignBit = DAG.getNode(ISD::EXTRACT_VECTOR_ELT, dl, MVT::f32, SignBit,
                          DAG.getIntPtrConstant(0));
  }

  // Clear first operand sign bit.
  CV.clear();
  if (VT == MVT::f64) {
    const fltSemantics &Sem = APFloat::IEEEdouble;
    CV.push_back(ConstantFP::get(*Context, APFloat(Sem,
                                                   APInt(64, ~(1ULL << 63)))));
    CV.push_back(ConstantFP::get(*Context, APFloat(Sem, APInt(64, 0))));
  } else {
    const fltSemantics &Sem = APFloat::IEEEsingle;
    CV.push_back(ConstantFP::get(*Context, APFloat(Sem,
                                                   APInt(32, ~(1U << 31)))));
    CV.push_back(ConstantFP::get(*Context, APFloat(Sem, APInt(32, 0))));
    CV.push_back(ConstantFP::get(*Context, APFloat(Sem, APInt(32, 0))));
    CV.push_back(ConstantFP::get(*Context, APFloat(Sem, APInt(32, 0))));
  }
  C = ConstantVector::get(CV);
  CPIdx = DAG.getConstantPool(C, TLI.getPointerTy(), 16);
  SDValue Mask2 = DAG.getLoad(VT, dl, DAG.getEntryNode(), CPIdx,
                              MachinePointerInfo::getConstantPool(),
                              false, false, false, 16);
  SDValue Val = DAG.getNode(X86ISD::FAND, dl, VT, Op0, Mask2);

  // Or the value with the sign bit.
  return DAG.getNode(X86ISD::FOR, dl, VT, Val, SignBit);
}

static SDValue LowerFGETSIGN(SDValue Op, SelectionDAG &DAG) {
  SDValue N0 = Op.getOperand(0);
  SDLoc dl(Op);
  MVT VT = Op.getSimpleValueType();

  // Lower ISD::FGETSIGN to (AND (X86ISD::FGETSIGNx86 ...) 1).
  SDValue xFGETSIGN = DAG.getNode(X86ISD::FGETSIGNx86, dl, VT, N0,
                                  DAG.getConstant(1, VT));
  return DAG.getNode(ISD::AND, dl, VT, xFGETSIGN, DAG.getConstant(1, VT));
}

// LowerVectorAllZeroTest - Check whether an OR'd tree is PTEST-able.
//
static SDValue LowerVectorAllZeroTest(SDValue Op, const X86Subtarget *Subtarget,
                                      SelectionDAG &DAG) {
  assert(Op.getOpcode() == ISD::OR && "Only check OR'd tree.");

  if (!Subtarget->hasSSE41())
    return SDValue();

  if (!Op->hasOneUse())
    return SDValue();

  SDNode *N = Op.getNode();
  SDLoc DL(N);

  SmallVector<SDValue, 8> Opnds;
  DenseMap<SDValue, unsigned> VecInMap;
  SmallVector<SDValue, 8> VecIns;
  EVT VT = MVT::Other;

  // Recognize a special case where a vector is casted into wide integer to
  // test all 0s.
  Opnds.push_back(N->getOperand(0));
  Opnds.push_back(N->getOperand(1));

  for (unsigned Slot = 0, e = Opnds.size(); Slot < e; ++Slot) {
    SmallVectorImpl<SDValue>::const_iterator I = Opnds.begin() + Slot;
    // BFS traverse all OR'd operands.
    if (I->getOpcode() == ISD::OR) {
      Opnds.push_back(I->getOperand(0));
      Opnds.push_back(I->getOperand(1));
      // Re-evaluate the number of nodes to be traversed.
      e += 2; // 2 more nodes (LHS and RHS) are pushed.
      continue;
    }

    // Quit if a non-EXTRACT_VECTOR_ELT
    if (I->getOpcode() != ISD::EXTRACT_VECTOR_ELT)
      return SDValue();

    // Quit if without a constant index.
    SDValue Idx = I->getOperand(1);
    if (!isa<ConstantSDNode>(Idx))
      return SDValue();

    SDValue ExtractedFromVec = I->getOperand(0);
    DenseMap<SDValue, unsigned>::iterator M = VecInMap.find(ExtractedFromVec);
    if (M == VecInMap.end()) {
      VT = ExtractedFromVec.getValueType();
      // Quit if not 128/256-bit vector.
      if (!VT.is128BitVector() && !VT.is256BitVector())
        return SDValue();
      // Quit if not the same type.
      if (VecInMap.begin() != VecInMap.end() &&
          VT != VecInMap.begin()->first.getValueType())
        return SDValue();
      M = VecInMap.insert(std::make_pair(ExtractedFromVec, 0)).first;
      VecIns.push_back(ExtractedFromVec);
    }
    M->second |= 1U << cast<ConstantSDNode>(Idx)->getZExtValue();
  }

  assert((VT.is128BitVector() || VT.is256BitVector()) &&
         "Not extracted from 128-/256-bit vector.");

  unsigned FullMask = (1U << VT.getVectorNumElements()) - 1U;

  for (DenseMap<SDValue, unsigned>::const_iterator
        I = VecInMap.begin(), E = VecInMap.end(); I != E; ++I) {
    // Quit if not all elements are used.
    if (I->second != FullMask)
      return SDValue();
  }

  EVT TestVT = VT.is128BitVector() ? MVT::v2i64 : MVT::v4i64;

  // Cast all vectors into TestVT for PTEST.
  for (unsigned i = 0, e = VecIns.size(); i < e; ++i)
    VecIns[i] = DAG.getNode(ISD::BITCAST, DL, TestVT, VecIns[i]);

  // If more than one full vectors are evaluated, OR them first before PTEST.
  for (unsigned Slot = 0, e = VecIns.size(); e - Slot > 1; Slot += 2, e += 1) {
    // Each iteration will OR 2 nodes and append the result until there is only
    // 1 node left, i.e. the final OR'd value of all vectors.
    SDValue LHS = VecIns[Slot];
    SDValue RHS = VecIns[Slot + 1];
    VecIns.push_back(DAG.getNode(ISD::OR, DL, TestVT, LHS, RHS));
  }

  return DAG.getNode(X86ISD::PTEST, DL, MVT::i32,
                     VecIns.back(), VecIns.back());
}

/// \brief return true if \c Op has a use that doesn't just read flags.
static bool hasNonFlagsUse(SDValue Op) {
  for (SDNode::use_iterator UI = Op->use_begin(), UE = Op->use_end(); UI != UE;
       ++UI) {
    SDNode *User = *UI;
    unsigned UOpNo = UI.getOperandNo();
    if (User->getOpcode() == ISD::TRUNCATE && User->hasOneUse()) {
      // Look pass truncate.
      UOpNo = User->use_begin().getOperandNo();
      User = *User->use_begin();
    }

    if (User->getOpcode() != ISD::BRCOND && User->getOpcode() != ISD::SETCC &&
        !(User->getOpcode() == ISD::SELECT && UOpNo == 0))
      return true;
  }
  return false;
}

/// Emit nodes that will be selected as "test Op0,Op0", or something
/// equivalent.
SDValue X86TargetLowering::EmitTest(SDValue Op, unsigned X86CC, SDLoc dl,
                                    SelectionDAG &DAG) const {
  if (Op.getValueType() == MVT::i1)
    // KORTEST instruction should be selected
    return DAG.getNode(X86ISD::CMP, dl, MVT::i32, Op,
                       DAG.getConstant(0, Op.getValueType()));

  // CF and OF aren't always set the way we want. Determine which
  // of these we need.
  bool NeedCF = false;
  bool NeedOF = false;
  switch (X86CC) {
  default: break;
  case X86::COND_A: case X86::COND_AE:
  case X86::COND_B: case X86::COND_BE:
    NeedCF = true;
    break;
  case X86::COND_G: case X86::COND_GE:
  case X86::COND_L: case X86::COND_LE:
  case X86::COND_O: case X86::COND_NO: {
    // Check if we really need to set the
    // Overflow flag. If NoSignedWrap is present
    // that is not actually needed.
    switch (Op->getOpcode()) {
    case ISD::ADD:
    case ISD::SUB:
    case ISD::MUL:
    case ISD::SHL: {
      const BinaryWithFlagsSDNode *BinNode =
          cast<BinaryWithFlagsSDNode>(Op.getNode());
      if (BinNode->hasNoSignedWrap())
        break;
    }
    default:
      NeedOF = true;
      break;
    }
    break;
  }
  }
  // See if we can use the EFLAGS value from the operand instead of
  // doing a separate TEST. TEST always sets OF and CF to 0, so unless
  // we prove that the arithmetic won't overflow, we can't use OF or CF.
  if (Op.getResNo() != 0 || NeedOF || NeedCF) {
    // Emit a CMP with 0, which is the TEST pattern.
    //if (Op.getValueType() == MVT::i1)
    //  return DAG.getNode(X86ISD::CMP, dl, MVT::i1, Op,
    //                     DAG.getConstant(0, MVT::i1));
    return DAG.getNode(X86ISD::CMP, dl, MVT::i32, Op,
                       DAG.getConstant(0, Op.getValueType()));
  }
  unsigned Opcode = 0;
  unsigned NumOperands = 0;

  // Truncate operations may prevent the merge of the SETCC instruction
  // and the arithmetic instruction before it. Attempt to truncate the operands
  // of the arithmetic instruction and use a reduced bit-width instruction.
  bool NeedTruncation = false;
  SDValue ArithOp = Op;
  if (Op->getOpcode() == ISD::TRUNCATE && Op->hasOneUse()) {
    SDValue Arith = Op->getOperand(0);
    // Both the trunc and the arithmetic op need to have one user each.
    if (Arith->hasOneUse())
      switch (Arith.getOpcode()) {
        default: break;
        case ISD::ADD:
        case ISD::SUB:
        case ISD::AND:
        case ISD::OR:
        case ISD::XOR: {
          NeedTruncation = true;
          ArithOp = Arith;
        }
      }
  }

  // NOTICE: In the code below we use ArithOp to hold the arithmetic operation
  // which may be the result of a CAST.  We use the variable 'Op', which is the
  // non-casted variable when we check for possible users.
  switch (ArithOp.getOpcode()) {
  case ISD::ADD:
    // Due to an isel shortcoming, be conservative if this add is likely to be
    // selected as part of a load-modify-store instruction. When the root node
    // in a match is a store, isel doesn't know how to remap non-chain non-flag
    // uses of other nodes in the match, such as the ADD in this case. This
    // leads to the ADD being left around and reselected, with the result being
    // two adds in the output.  Alas, even if none our users are stores, that
    // doesn't prove we're O.K.  Ergo, if we have any parents that aren't
    // CopyToReg or SETCC, eschew INC/DEC.  A better fix seems to require
    // climbing the DAG back to the root, and it doesn't seem to be worth the
    // effort.
    for (SDNode::use_iterator UI = Op.getNode()->use_begin(),
         UE = Op.getNode()->use_end(); UI != UE; ++UI)
      if (UI->getOpcode() != ISD::CopyToReg &&
          UI->getOpcode() != ISD::SETCC &&
          UI->getOpcode() != ISD::STORE)
        goto default_case;

    if (ConstantSDNode *C =
        dyn_cast<ConstantSDNode>(ArithOp.getNode()->getOperand(1))) {
      // An add of one will be selected as an INC.
      if (C->getAPIntValue() == 1 && !Subtarget->slowIncDec()) {
        Opcode = X86ISD::INC;
        NumOperands = 1;
        break;
      }

      // An add of negative one (subtract of one) will be selected as a DEC.
      if (C->getAPIntValue().isAllOnesValue() && !Subtarget->slowIncDec()) {
        Opcode = X86ISD::DEC;
        NumOperands = 1;
        break;
      }
    }

    // Otherwise use a regular EFLAGS-setting add.
    Opcode = X86ISD::ADD;
    NumOperands = 2;
    break;
  case ISD::SHL:
  case ISD::SRL:
    // If we have a constant logical shift that's only used in a comparison
    // against zero turn it into an equivalent AND. This allows turning it into
    // a TEST instruction later.
    if ((X86CC == X86::COND_E || X86CC == X86::COND_NE) && Op->hasOneUse() &&
        isa<ConstantSDNode>(Op->getOperand(1)) && !hasNonFlagsUse(Op)) {
      EVT VT = Op.getValueType();
      unsigned BitWidth = VT.getSizeInBits();
      unsigned ShAmt = Op->getConstantOperandVal(1);
      if (ShAmt >= BitWidth) // Avoid undefined shifts.
        break;
      APInt Mask = ArithOp.getOpcode() == ISD::SRL
                       ? APInt::getHighBitsSet(BitWidth, BitWidth - ShAmt)
                       : APInt::getLowBitsSet(BitWidth, BitWidth - ShAmt);
      if (!Mask.isSignedIntN(32)) // Avoid large immediates.
        break;
      SDValue New = DAG.getNode(ISD::AND, dl, VT, Op->getOperand(0),
                                DAG.getConstant(Mask, VT));
      DAG.ReplaceAllUsesWith(Op, New);
      Op = New;
    }
    break;

  case ISD::AND:
    // If the primary and result isn't used, don't bother using X86ISD::AND,
    // because a TEST instruction will be better.
    if (!hasNonFlagsUse(Op))
      break;
    // FALL THROUGH
  case ISD::SUB:
  case ISD::OR:
  case ISD::XOR:
    // Due to the ISEL shortcoming noted above, be conservative if this op is
    // likely to be selected as part of a load-modify-store instruction.
    for (SDNode::use_iterator UI = Op.getNode()->use_begin(),
           UE = Op.getNode()->use_end(); UI != UE; ++UI)
      if (UI->getOpcode() == ISD::STORE)
        goto default_case;

    // Otherwise use a regular EFLAGS-setting instruction.
    switch (ArithOp.getOpcode()) {
    default: llvm_unreachable("unexpected operator!");
    case ISD::SUB: Opcode = X86ISD::SUB; break;
    case ISD::XOR: Opcode = X86ISD::XOR; break;
    case ISD::AND: Opcode = X86ISD::AND; break;
    case ISD::OR: {
      if (!NeedTruncation && (X86CC == X86::COND_E || X86CC == X86::COND_NE)) {
        SDValue EFLAGS = LowerVectorAllZeroTest(Op, Subtarget, DAG);
        if (EFLAGS.getNode())
          return EFLAGS;
      }
      Opcode = X86ISD::OR;
      break;
    }
    }

    NumOperands = 2;
    break;
  case X86ISD::ADD:
  case X86ISD::SUB:
  case X86ISD::INC:
  case X86ISD::DEC:
  case X86ISD::OR:
  case X86ISD::XOR:
  case X86ISD::AND:
    return SDValue(Op.getNode(), 1);
  default:
  default_case:
    break;
  }

  // If we found that truncation is beneficial, perform the truncation and
  // update 'Op'.
  if (NeedTruncation) {
    EVT VT = Op.getValueType();
    SDValue WideVal = Op->getOperand(0);
    EVT WideVT = WideVal.getValueType();
    unsigned ConvertedOp = 0;
    // Use a target machine opcode to prevent further DAGCombine
    // optimizations that may separate the arithmetic operations
    // from the setcc node.
    switch (WideVal.getOpcode()) {
      default: break;
      case ISD::ADD: ConvertedOp = X86ISD::ADD; break;
      case ISD::SUB: ConvertedOp = X86ISD::SUB; break;
      case ISD::AND: ConvertedOp = X86ISD::AND; break;
      case ISD::OR:  ConvertedOp = X86ISD::OR;  break;
      case ISD::XOR: ConvertedOp = X86ISD::XOR; break;
    }

    if (ConvertedOp) {
      const TargetLowering &TLI = DAG.getTargetLoweringInfo();
      if (TLI.isOperationLegal(WideVal.getOpcode(), WideVT)) {
        SDValue V0 = DAG.getNode(ISD::TRUNCATE, dl, VT, WideVal.getOperand(0));
        SDValue V1 = DAG.getNode(ISD::TRUNCATE, dl, VT, WideVal.getOperand(1));
        Op = DAG.getNode(ConvertedOp, dl, VT, V0, V1);
      }
    }
  }

  if (Opcode == 0)
    // Emit a CMP with 0, which is the TEST pattern.
    return DAG.getNode(X86ISD::CMP, dl, MVT::i32, Op,
                       DAG.getConstant(0, Op.getValueType()));

  SDVTList VTs = DAG.getVTList(Op.getValueType(), MVT::i32);
  SmallVector<SDValue, 4> Ops;
  for (unsigned i = 0; i != NumOperands; ++i)
    Ops.push_back(Op.getOperand(i));

  SDValue New = DAG.getNode(Opcode, dl, VTs, Ops);
  DAG.ReplaceAllUsesWith(Op, New);
  return SDValue(New.getNode(), 1);
}

/// Emit nodes that will be selected as "cmp Op0,Op1", or something
/// equivalent.
SDValue X86TargetLowering::EmitCmp(SDValue Op0, SDValue Op1, unsigned X86CC,
                                   SDLoc dl, SelectionDAG &DAG) const {
  if (ConstantSDNode *C = dyn_cast<ConstantSDNode>(Op1)) {
    if (C->getAPIntValue() == 0)
      return EmitTest(Op0, X86CC, dl, DAG);

     if (Op0.getValueType() == MVT::i1)
       llvm_unreachable("Unexpected comparison operation for MVT::i1 operands");
  }
 
  if ((Op0.getValueType() == MVT::i8 || Op0.getValueType() == MVT::i16 ||
       Op0.getValueType() == MVT::i32 || Op0.getValueType() == MVT::i64)) {
    // Do the comparison at i32 if it's smaller, besides the Atom case. 
    // This avoids subregister aliasing issues. Keep the smaller reference 
    // if we're optimizing for size, however, as that'll allow better folding 
    // of memory operations.
    if (Op0.getValueType() != MVT::i32 && Op0.getValueType() != MVT::i64 &&
        !DAG.getMachineFunction().getFunction()->getAttributes().hasAttribute(
             AttributeSet::FunctionIndex, Attribute::MinSize) &&
        !Subtarget->isAtom()) {
      unsigned ExtendOp =
          isX86CCUnsigned(X86CC) ? ISD::ZERO_EXTEND : ISD::SIGN_EXTEND;
      Op0 = DAG.getNode(ExtendOp, dl, MVT::i32, Op0);
      Op1 = DAG.getNode(ExtendOp, dl, MVT::i32, Op1);
    }
    // Use SUB instead of CMP to enable CSE between SUB and CMP.
    SDVTList VTs = DAG.getVTList(Op0.getValueType(), MVT::i32);
    SDValue Sub = DAG.getNode(X86ISD::SUB, dl, VTs,
                              Op0, Op1);
    return SDValue(Sub.getNode(), 1);
  }
  return DAG.getNode(X86ISD::CMP, dl, MVT::i32, Op0, Op1);
}

/// Convert a comparison if required by the subtarget.
SDValue X86TargetLowering::ConvertCmpIfNecessary(SDValue Cmp,
                                                 SelectionDAG &DAG) const {
  // If the subtarget does not support the FUCOMI instruction, floating-point
  // comparisons have to be converted.
  if (Subtarget->hasCMov() ||
      Cmp.getOpcode() != X86ISD::CMP ||
      !Cmp.getOperand(0).getValueType().isFloatingPoint() ||
      !Cmp.getOperand(1).getValueType().isFloatingPoint())
    return Cmp;

  // The instruction selector will select an FUCOM instruction instead of
  // FUCOMI, which writes the comparison result to FPSW instead of EFLAGS. Hence
  // build an SDNode sequence that transfers the result from FPSW into EFLAGS:
  // (X86sahf (trunc (srl (X86fp_stsw (trunc (X86cmp ...)), 8))))
  SDLoc dl(Cmp);
  SDValue TruncFPSW = DAG.getNode(ISD::TRUNCATE, dl, MVT::i16, Cmp);
  SDValue FNStSW = DAG.getNode(X86ISD::FNSTSW16r, dl, MVT::i16, TruncFPSW);
  SDValue Srl = DAG.getNode(ISD::SRL, dl, MVT::i16, FNStSW,
                            DAG.getConstant(8, MVT::i8));
  SDValue TruncSrl = DAG.getNode(ISD::TRUNCATE, dl, MVT::i8, Srl);
  return DAG.getNode(X86ISD::SAHF, dl, MVT::i32, TruncSrl);
}

static bool isAllOnes(SDValue V) {
  ConstantSDNode *C = dyn_cast<ConstantSDNode>(V);
  return C && C->isAllOnesValue();
}

/// LowerToBT - Result of 'and' is compared against zero. Turn it into a BT node
/// if it's possible.
SDValue X86TargetLowering::LowerToBT(SDValue And, ISD::CondCode CC,
                                     SDLoc dl, SelectionDAG &DAG) const {
  SDValue Op0 = And.getOperand(0);
  SDValue Op1 = And.getOperand(1);
  if (Op0.getOpcode() == ISD::TRUNCATE)
    Op0 = Op0.getOperand(0);
  if (Op1.getOpcode() == ISD::TRUNCATE)
    Op1 = Op1.getOperand(0);

  SDValue LHS, RHS;
  if (Op1.getOpcode() == ISD::SHL)
    std::swap(Op0, Op1);
  if (Op0.getOpcode() == ISD::SHL) {
    if (ConstantSDNode *And00C = dyn_cast<ConstantSDNode>(Op0.getOperand(0)))
      if (And00C->getZExtValue() == 1) {
        // If we looked past a truncate, check that it's only truncating away
        // known zeros.
        unsigned BitWidth = Op0.getValueSizeInBits();
        unsigned AndBitWidth = And.getValueSizeInBits();
        if (BitWidth > AndBitWidth) {
          APInt Zeros, Ones;
          DAG.computeKnownBits(Op0, Zeros, Ones);
          if (Zeros.countLeadingOnes() < BitWidth - AndBitWidth)
            return SDValue();
        }
        LHS = Op1;
        RHS = Op0.getOperand(1);
      }
  } else if (Op1.getOpcode() == ISD::Constant) {
    ConstantSDNode *AndRHS = cast<ConstantSDNode>(Op1);
    uint64_t AndRHSVal = AndRHS->getZExtValue();
    SDValue AndLHS = Op0;

    if (AndRHSVal == 1 && AndLHS.getOpcode() == ISD::SRL) {
      LHS = AndLHS.getOperand(0);
      RHS = AndLHS.getOperand(1);
    }

    // Use BT if the immediate can't be encoded in a TEST instruction.
    if (!isUInt<32>(AndRHSVal) && isPowerOf2_64(AndRHSVal)) {
      LHS = AndLHS;
      RHS = DAG.getConstant(Log2_64_Ceil(AndRHSVal), LHS.getValueType());
    }
  }

  if (LHS.getNode()) {
    // If LHS is i8, promote it to i32 with any_extend.  There is no i8 BT
    // instruction.  Since the shift amount is in-range-or-undefined, we know
    // that doing a bittest on the i32 value is ok.  We extend to i32 because
    // the encoding for the i16 version is larger than the i32 version.
    // Also promote i16 to i32 for performance / code size reason.
    if (LHS.getValueType() == MVT::i8 ||
        LHS.getValueType() == MVT::i16)
      LHS = DAG.getNode(ISD::ANY_EXTEND, dl, MVT::i32, LHS);

    // If the operand types disagree, extend the shift amount to match.  Since
    // BT ignores high bits (like shifts) we can use anyextend.
    if (LHS.getValueType() != RHS.getValueType())
      RHS = DAG.getNode(ISD::ANY_EXTEND, dl, LHS.getValueType(), RHS);

    SDValue BT = DAG.getNode(X86ISD::BT, dl, MVT::i32, LHS, RHS);
    X86::CondCode Cond = CC == ISD::SETEQ ? X86::COND_AE : X86::COND_B;
    return DAG.getNode(X86ISD::SETCC, dl, MVT::i8,
                       DAG.getConstant(Cond, MVT::i8), BT);
  }

  return SDValue();
}

/// \brief - Turns an ISD::CondCode into a value suitable for SSE floating point
/// mask CMPs.
static int translateX86FSETCC(ISD::CondCode SetCCOpcode, SDValue &Op0,
                              SDValue &Op1) {
  unsigned SSECC;
  bool Swap = false;

  // SSE Condition code mapping:
  //  0 - EQ
  //  1 - LT
  //  2 - LE
  //  3 - UNORD
  //  4 - NEQ
  //  5 - NLT
  //  6 - NLE
  //  7 - ORD
  switch (SetCCOpcode) {
  default: llvm_unreachable("Unexpected SETCC condition");
  case ISD::SETOEQ:
  case ISD::SETEQ:  SSECC = 0; break;
  case ISD::SETOGT:
  case ISD::SETGT:  Swap = true; // Fallthrough
  case ISD::SETLT:
  case ISD::SETOLT: SSECC = 1; break;
  case ISD::SETOGE:
  case ISD::SETGE:  Swap = true; // Fallthrough
  case ISD::SETLE:
  case ISD::SETOLE: SSECC = 2; break;
  case ISD::SETUO:  SSECC = 3; break;
  case ISD::SETUNE:
  case ISD::SETNE:  SSECC = 4; break;
  case ISD::SETULE: Swap = true; // Fallthrough
  case ISD::SETUGE: SSECC = 5; break;
  case ISD::SETULT: Swap = true; // Fallthrough
  case ISD::SETUGT: SSECC = 6; break;
  case ISD::SETO:   SSECC = 7; break;
  case ISD::SETUEQ:
  case ISD::SETONE: SSECC = 8; break;
  }
  if (Swap)
    std::swap(Op0, Op1);

  return SSECC;
}

// Lower256IntVSETCC - Break a VSETCC 256-bit integer VSETCC into two new 128
// ones, and then concatenate the result back.
static SDValue Lower256IntVSETCC(SDValue Op, SelectionDAG &DAG) {
  MVT VT = Op.getSimpleValueType();

  assert(VT.is256BitVector() && Op.getOpcode() == ISD::SETCC &&
         "Unsupported value type for operation");

  unsigned NumElems = VT.getVectorNumElements();
  SDLoc dl(Op);
  SDValue CC = Op.getOperand(2);

  // Extract the LHS vectors
  SDValue LHS = Op.getOperand(0);
  SDValue LHS1 = Extract128BitVector(LHS, 0, DAG, dl);
  SDValue LHS2 = Extract128BitVector(LHS, NumElems/2, DAG, dl);

  // Extract the RHS vectors
  SDValue RHS = Op.getOperand(1);
  SDValue RHS1 = Extract128BitVector(RHS, 0, DAG, dl);
  SDValue RHS2 = Extract128BitVector(RHS, NumElems/2, DAG, dl);

  // Issue the operation on the smaller types and concatenate the result back
  MVT EltVT = VT.getVectorElementType();
  MVT NewVT = MVT::getVectorVT(EltVT, NumElems/2);
  return DAG.getNode(ISD::CONCAT_VECTORS, dl, VT,
                     DAG.getNode(Op.getOpcode(), dl, NewVT, LHS1, RHS1, CC),
                     DAG.getNode(Op.getOpcode(), dl, NewVT, LHS2, RHS2, CC));
}

static SDValue LowerIntVSETCC_AVX512(SDValue Op, SelectionDAG &DAG,
                                     const X86Subtarget *Subtarget) {
  SDValue Op0 = Op.getOperand(0);
  SDValue Op1 = Op.getOperand(1);
  SDValue CC = Op.getOperand(2);
  MVT VT = Op.getSimpleValueType();
  SDLoc dl(Op);

  assert(Op0.getValueType().getVectorElementType().getSizeInBits() >= 8 &&
         Op.getValueType().getScalarType() == MVT::i1 &&
         "Cannot set masked compare for this operation");

  ISD::CondCode SetCCOpcode = cast<CondCodeSDNode>(CC)->get();
  unsigned  Opc = 0;
  bool Unsigned = false;
  bool Swap = false;
  unsigned SSECC;
  switch (SetCCOpcode) {
  default: llvm_unreachable("Unexpected SETCC condition");
  case ISD::SETNE:  SSECC = 4; break;
  case ISD::SETEQ:  Opc = X86ISD::PCMPEQM; break;
  case ISD::SETUGT: SSECC = 6; Unsigned = true; break;
  case ISD::SETLT:  Swap = true; //fall-through
  case ISD::SETGT:  Opc = X86ISD::PCMPGTM; break;
  case ISD::SETULT: SSECC = 1; Unsigned = true; break;
  case ISD::SETUGE: SSECC = 5; Unsigned = true; break; //NLT
  case ISD::SETGE:  Swap = true; SSECC = 2; break; // LE + swap
  case ISD::SETULE: Unsigned = true; //fall-through
  case ISD::SETLE:  SSECC = 2; break;
  }

  if (Swap)
    std::swap(Op0, Op1);
  if (Opc)
    return DAG.getNode(Opc, dl, VT, Op0, Op1);
  Opc = Unsigned ? X86ISD::CMPMU: X86ISD::CMPM;
  return DAG.getNode(Opc, dl, VT, Op0, Op1,
                     DAG.getConstant(SSECC, MVT::i8));
}

/// \brief Try to turn a VSETULT into a VSETULE by modifying its second
/// operand \p Op1.  If non-trivial (for example because it's not constant)
/// return an empty value.
static SDValue ChangeVSETULTtoVSETULE(SDLoc dl, SDValue Op1, SelectionDAG &DAG)
{
  BuildVectorSDNode *BV = dyn_cast<BuildVectorSDNode>(Op1.getNode());
  if (!BV)
    return SDValue();

  MVT VT = Op1.getSimpleValueType();
  MVT EVT = VT.getVectorElementType();
  unsigned n = VT.getVectorNumElements();
  SmallVector<SDValue, 8> ULTOp1;

  for (unsigned i = 0; i < n; ++i) {
    ConstantSDNode *Elt = dyn_cast<ConstantSDNode>(BV->getOperand(i));
    if (!Elt || Elt->isOpaque() || Elt->getValueType(0) != EVT)
      return SDValue();

    // Avoid underflow.
    APInt Val = Elt->getAPIntValue();
    if (Val == 0)
      return SDValue();

    ULTOp1.push_back(DAG.getConstant(Val - 1, EVT));
  }

  return DAG.getNode(ISD::BUILD_VECTOR, dl, VT, ULTOp1);
}

static SDValue LowerVSETCC(SDValue Op, const X86Subtarget *Subtarget,
                           SelectionDAG &DAG) {
  SDValue Op0 = Op.getOperand(0);
  SDValue Op1 = Op.getOperand(1);
  SDValue CC = Op.getOperand(2);
  MVT VT = Op.getSimpleValueType();
  ISD::CondCode SetCCOpcode = cast<CondCodeSDNode>(CC)->get();
  bool isFP = Op.getOperand(1).getSimpleValueType().isFloatingPoint();
  SDLoc dl(Op);

  if (isFP) {
#ifndef NDEBUG
    MVT EltVT = Op0.getSimpleValueType().getVectorElementType();
    assert(EltVT == MVT::f32 || EltVT == MVT::f64);
#endif

    unsigned SSECC = translateX86FSETCC(SetCCOpcode, Op0, Op1);
    unsigned Opc = X86ISD::CMPP;
    if (Subtarget->hasAVX512() && VT.getVectorElementType() == MVT::i1) {
      assert(VT.getVectorNumElements() <= 16);
      Opc = X86ISD::CMPM;
    }
    // In the two special cases we can't handle, emit two comparisons.
    if (SSECC == 8) {
      unsigned CC0, CC1;
      unsigned CombineOpc;
      if (SetCCOpcode == ISD::SETUEQ) {
        CC0 = 3; CC1 = 0; CombineOpc = ISD::OR;
      } else {
        assert(SetCCOpcode == ISD::SETONE);
        CC0 = 7; CC1 = 4; CombineOpc = ISD::AND;
      }

      SDValue Cmp0 = DAG.getNode(Opc, dl, VT, Op0, Op1,
                                 DAG.getConstant(CC0, MVT::i8));
      SDValue Cmp1 = DAG.getNode(Opc, dl, VT, Op0, Op1,
                                 DAG.getConstant(CC1, MVT::i8));
      return DAG.getNode(CombineOpc, dl, VT, Cmp0, Cmp1);
    }
    // Handle all other FP comparisons here.
    return DAG.getNode(Opc, dl, VT, Op0, Op1,
                       DAG.getConstant(SSECC, MVT::i8));
  }

  // Break 256-bit integer vector compare into smaller ones.
  if (VT.is256BitVector() && !Subtarget->hasInt256())
    return Lower256IntVSETCC(Op, DAG);

  bool MaskResult = (VT.getVectorElementType() == MVT::i1);
  EVT OpVT = Op1.getValueType();
  if (Subtarget->hasAVX512()) {
    if (Op1.getValueType().is512BitVector() ||
        (Subtarget->hasBWI() && Subtarget->hasVLX()) ||
        (MaskResult && OpVT.getVectorElementType().getSizeInBits() >= 32))
      return LowerIntVSETCC_AVX512(Op, DAG, Subtarget);

    // In AVX-512 architecture setcc returns mask with i1 elements,
    // But there is no compare instruction for i8 and i16 elements in KNL.
    // We are not talking about 512-bit operands in this case, these
    // types are illegal.
    if (MaskResult &&
        (OpVT.getVectorElementType().getSizeInBits() < 32 &&
         OpVT.getVectorElementType().getSizeInBits() >= 8))
      return DAG.getNode(ISD::TRUNCATE, dl, VT,
                         DAG.getNode(ISD::SETCC, dl, OpVT, Op0, Op1, CC));
  }

  // We are handling one of the integer comparisons here.  Since SSE only has
  // GT and EQ comparisons for integer, swapping operands and multiple
  // operations may be required for some comparisons.
  unsigned Opc;
  bool Swap = false, Invert = false, FlipSigns = false, MinMax = false;
  bool Subus = false;

  switch (SetCCOpcode) {
  default: llvm_unreachable("Unexpected SETCC condition");
  case ISD::SETNE:  Invert = true;
  case ISD::SETEQ:  Opc = X86ISD::PCMPEQ; break;
  case ISD::SETLT:  Swap = true;
  case ISD::SETGT:  Opc = X86ISD::PCMPGT; break;
  case ISD::SETGE:  Swap = true;
  case ISD::SETLE:  Opc = X86ISD::PCMPGT;
                    Invert = true; break;
  case ISD::SETULT: Swap = true;
  case ISD::SETUGT: Opc = X86ISD::PCMPGT;
                    FlipSigns = true; break;
  case ISD::SETUGE: Swap = true;
  case ISD::SETULE: Opc = X86ISD::PCMPGT;
                    FlipSigns = true; Invert = true; break;
  }

  // Special case: Use min/max operations for SETULE/SETUGE
  MVT VET = VT.getVectorElementType();
  bool hasMinMax =
       (Subtarget->hasSSE41() && (VET >= MVT::i8 && VET <= MVT::i32))
    || (Subtarget->hasSSE2()  && (VET == MVT::i8));

  if (hasMinMax) {
    switch (SetCCOpcode) {
    default: break;
    case ISD::SETULE: Opc = X86ISD::UMIN; MinMax = true; break;
    case ISD::SETUGE: Opc = X86ISD::UMAX; MinMax = true; break;
    }

    if (MinMax) { Swap = false; Invert = false; FlipSigns = false; }
  }

  bool hasSubus = Subtarget->hasSSE2() && (VET == MVT::i8 || VET == MVT::i16);
  if (!MinMax && hasSubus) {
    // As another special case, use PSUBUS[BW] when it's profitable. E.g. for
    // Op0 u<= Op1:
    //   t = psubus Op0, Op1
    //   pcmpeq t, <0..0>
    switch (SetCCOpcode) {
    default: break;
    case ISD::SETULT: {
      // If the comparison is against a constant we can turn this into a
      // setule.  With psubus, setule does not require a swap.  This is
      // beneficial because the constant in the register is no longer
      // destructed as the destination so it can be hoisted out of a loop.
      // Only do this pre-AVX since vpcmp* is no longer destructive.
      if (Subtarget->hasAVX())
        break;
      SDValue ULEOp1 = ChangeVSETULTtoVSETULE(dl, Op1, DAG);
      if (ULEOp1.getNode()) {
        Op1 = ULEOp1;
        Subus = true; Invert = false; Swap = false;
      }
      break;
    }
    // Psubus is better than flip-sign because it requires no inversion.
    case ISD::SETUGE: Subus = true; Invert = false; Swap = true;  break;
    case ISD::SETULE: Subus = true; Invert = false; Swap = false; break;
    }

    if (Subus) {
      Opc = X86ISD::SUBUS;
      FlipSigns = false;
    }
  }

  if (Swap)
    std::swap(Op0, Op1);

  // Check that the operation in question is available (most are plain SSE2,
  // but PCMPGTQ and PCMPEQQ have different requirements).
  if (VT == MVT::v2i64) {
    if (Opc == X86ISD::PCMPGT && !Subtarget->hasSSE42()) {
      assert(Subtarget->hasSSE2() && "Don't know how to lower!");

      // First cast everything to the right type.
      Op0 = DAG.getNode(ISD::BITCAST, dl, MVT::v4i32, Op0);
      Op1 = DAG.getNode(ISD::BITCAST, dl, MVT::v4i32, Op1);

      // Since SSE has no unsigned integer comparisons, we need to flip the sign
      // bits of the inputs before performing those operations. The lower
      // compare is always unsigned.
      SDValue SB;
      if (FlipSigns) {
        SB = DAG.getConstant(0x80000000U, MVT::v4i32);
      } else {
        SDValue Sign = DAG.getConstant(0x80000000U, MVT::i32);
        SDValue Zero = DAG.getConstant(0x00000000U, MVT::i32);
        SB = DAG.getNode(ISD::BUILD_VECTOR, dl, MVT::v4i32,
                         Sign, Zero, Sign, Zero);
      }
      Op0 = DAG.getNode(ISD::XOR, dl, MVT::v4i32, Op0, SB);
      Op1 = DAG.getNode(ISD::XOR, dl, MVT::v4i32, Op1, SB);

      // Emulate PCMPGTQ with (hi1 > hi2) | ((hi1 == hi2) & (lo1 > lo2))
      SDValue GT = DAG.getNode(X86ISD::PCMPGT, dl, MVT::v4i32, Op0, Op1);
      SDValue EQ = DAG.getNode(X86ISD::PCMPEQ, dl, MVT::v4i32, Op0, Op1);

      // Create masks for only the low parts/high parts of the 64 bit integers.
      static const int MaskHi[] = { 1, 1, 3, 3 };
      static const int MaskLo[] = { 0, 0, 2, 2 };
      SDValue EQHi = DAG.getVectorShuffle(MVT::v4i32, dl, EQ, EQ, MaskHi);
      SDValue GTLo = DAG.getVectorShuffle(MVT::v4i32, dl, GT, GT, MaskLo);
      SDValue GTHi = DAG.getVectorShuffle(MVT::v4i32, dl, GT, GT, MaskHi);

      SDValue Result = DAG.getNode(ISD::AND, dl, MVT::v4i32, EQHi, GTLo);
      Result = DAG.getNode(ISD::OR, dl, MVT::v4i32, Result, GTHi);

      if (Invert)
        Result = DAG.getNOT(dl, Result, MVT::v4i32);

      return DAG.getNode(ISD::BITCAST, dl, VT, Result);
    }

    if (Opc == X86ISD::PCMPEQ && !Subtarget->hasSSE41()) {
      // If pcmpeqq is missing but pcmpeqd is available synthesize pcmpeqq with
      // pcmpeqd + pshufd + pand.
      assert(Subtarget->hasSSE2() && !FlipSigns && "Don't know how to lower!");

      // First cast everything to the right type.
      Op0 = DAG.getNode(ISD::BITCAST, dl, MVT::v4i32, Op0);
      Op1 = DAG.getNode(ISD::BITCAST, dl, MVT::v4i32, Op1);

      // Do the compare.
      SDValue Result = DAG.getNode(Opc, dl, MVT::v4i32, Op0, Op1);

      // Make sure the lower and upper halves are both all-ones.
      static const int Mask[] = { 1, 0, 3, 2 };
      SDValue Shuf = DAG.getVectorShuffle(MVT::v4i32, dl, Result, Result, Mask);
      Result = DAG.getNode(ISD::AND, dl, MVT::v4i32, Result, Shuf);

      if (Invert)
        Result = DAG.getNOT(dl, Result, MVT::v4i32);

      return DAG.getNode(ISD::BITCAST, dl, VT, Result);
    }
  }

  // Since SSE has no unsigned integer comparisons, we need to flip the sign
  // bits of the inputs before performing those operations.
  if (FlipSigns) {
    EVT EltVT = VT.getVectorElementType();
    SDValue SB = DAG.getConstant(APInt::getSignBit(EltVT.getSizeInBits()), VT);
    Op0 = DAG.getNode(ISD::XOR, dl, VT, Op0, SB);
    Op1 = DAG.getNode(ISD::XOR, dl, VT, Op1, SB);
  }

  SDValue Result = DAG.getNode(Opc, dl, VT, Op0, Op1);

  // If the logical-not of the result is required, perform that now.
  if (Invert)
    Result = DAG.getNOT(dl, Result, VT);

  if (MinMax)
    Result = DAG.getNode(X86ISD::PCMPEQ, dl, VT, Op0, Result);

  if (Subus)
    Result = DAG.getNode(X86ISD::PCMPEQ, dl, VT, Result,
                         getZeroVector(VT, Subtarget, DAG, dl));

  return Result;
}

SDValue X86TargetLowering::LowerSETCC(SDValue Op, SelectionDAG &DAG) const {

  MVT VT = Op.getSimpleValueType();

  if (VT.isVector()) return LowerVSETCC(Op, Subtarget, DAG);

  assert(((!Subtarget->hasAVX512() && VT == MVT::i8) || (VT == MVT::i1))
         && "SetCC type must be 8-bit or 1-bit integer");
  SDValue Op0 = Op.getOperand(0);
  SDValue Op1 = Op.getOperand(1);
  SDLoc dl(Op);
  ISD::CondCode CC = cast<CondCodeSDNode>(Op.getOperand(2))->get();

  // Optimize to BT if possible.
  // Lower (X & (1 << N)) == 0 to BT(X, N).
  // Lower ((X >>u N) & 1) != 0 to BT(X, N).
  // Lower ((X >>s N) & 1) != 0 to BT(X, N).
  if (Op0.getOpcode() == ISD::AND && Op0.hasOneUse() &&
      Op1.getOpcode() == ISD::Constant &&
      cast<ConstantSDNode>(Op1)->isNullValue() &&
      (CC == ISD::SETEQ || CC == ISD::SETNE)) {
    SDValue NewSetCC = LowerToBT(Op0, CC, dl, DAG);
    if (NewSetCC.getNode())
      return NewSetCC;
  }

  // Look for X == 0, X == 1, X != 0, or X != 1.  We can simplify some forms of
  // these.
  if (Op1.getOpcode() == ISD::Constant &&
      (cast<ConstantSDNode>(Op1)->getZExtValue() == 1 ||
       cast<ConstantSDNode>(Op1)->isNullValue()) &&
      (CC == ISD::SETEQ || CC == ISD::SETNE)) {

    // If the input is a setcc, then reuse the input setcc or use a new one with
    // the inverted condition.
    if (Op0.getOpcode() == X86ISD::SETCC) {
      X86::CondCode CCode = (X86::CondCode)Op0.getConstantOperandVal(0);
      bool Invert = (CC == ISD::SETNE) ^
        cast<ConstantSDNode>(Op1)->isNullValue();
      if (!Invert)
        return Op0;

      CCode = X86::GetOppositeBranchCondition(CCode);
      SDValue SetCC = DAG.getNode(X86ISD::SETCC, dl, MVT::i8,
                                  DAG.getConstant(CCode, MVT::i8),
                                  Op0.getOperand(1));
      if (VT == MVT::i1)
        return DAG.getNode(ISD::TRUNCATE, dl, MVT::i1, SetCC);
      return SetCC;
    }
  }
  if ((Op0.getValueType() == MVT::i1) && (Op1.getOpcode() == ISD::Constant) &&
      (cast<ConstantSDNode>(Op1)->getZExtValue() == 1) &&
      (CC == ISD::SETEQ || CC == ISD::SETNE)) {

    ISD::CondCode NewCC = ISD::getSetCCInverse(CC, true);
    return DAG.getSetCC(dl, VT, Op0, DAG.getConstant(0, MVT::i1), NewCC);
  }

  bool isFP = Op1.getSimpleValueType().isFloatingPoint();
  unsigned X86CC = TranslateX86CC(CC, isFP, Op0, Op1, DAG);
  if (X86CC == X86::COND_INVALID)
    return SDValue();

  SDValue EFLAGS = EmitCmp(Op0, Op1, X86CC, dl, DAG);
  EFLAGS = ConvertCmpIfNecessary(EFLAGS, DAG);
  SDValue SetCC = DAG.getNode(X86ISD::SETCC, dl, MVT::i8,
                              DAG.getConstant(X86CC, MVT::i8), EFLAGS);
  if (VT == MVT::i1)
    return DAG.getNode(ISD::TRUNCATE, dl, MVT::i1, SetCC);
  return SetCC;
}

// isX86LogicalCmp - Return true if opcode is a X86 logical comparison.
static bool isX86LogicalCmp(SDValue Op) {
  unsigned Opc = Op.getNode()->getOpcode();
  if (Opc == X86ISD::CMP || Opc == X86ISD::COMI || Opc == X86ISD::UCOMI ||
      Opc == X86ISD::SAHF)
    return true;
  if (Op.getResNo() == 1 &&
      (Opc == X86ISD::ADD ||
       Opc == X86ISD::SUB ||
       Opc == X86ISD::ADC ||
       Opc == X86ISD::SBB ||
       Opc == X86ISD::SMUL ||
       Opc == X86ISD::UMUL ||
       Opc == X86ISD::INC ||
       Opc == X86ISD::DEC ||
       Opc == X86ISD::OR ||
       Opc == X86ISD::XOR ||
       Opc == X86ISD::AND))
    return true;

  if (Op.getResNo() == 2 && Opc == X86ISD::UMUL)
    return true;

  return false;
}

static bool isTruncWithZeroHighBitsInput(SDValue V, SelectionDAG &DAG) {
  if (V.getOpcode() != ISD::TRUNCATE)
    return false;

  SDValue VOp0 = V.getOperand(0);
  unsigned InBits = VOp0.getValueSizeInBits();
  unsigned Bits = V.getValueSizeInBits();
  return DAG.MaskedValueIsZero(VOp0, APInt::getHighBitsSet(InBits,InBits-Bits));
}

SDValue X86TargetLowering::LowerSELECT(SDValue Op, SelectionDAG &DAG) const {
  bool addTest = true;
  SDValue Cond  = Op.getOperand(0);
  SDValue Op1 = Op.getOperand(1);
  SDValue Op2 = Op.getOperand(2);
  SDLoc DL(Op);
  EVT VT = Op1.getValueType();
  SDValue CC;

  // Lower fp selects into a CMP/AND/ANDN/OR sequence when the necessary SSE ops
  // are available. Otherwise fp cmovs get lowered into a less efficient branch
  // sequence later on.
  if (Cond.getOpcode() == ISD::SETCC &&
      ((Subtarget->hasSSE2() && (VT == MVT::f32 || VT == MVT::f64)) ||
       (Subtarget->hasSSE1() && VT == MVT::f32)) &&
      VT == Cond.getOperand(0).getValueType() && Cond->hasOneUse()) {
    SDValue CondOp0 = Cond.getOperand(0), CondOp1 = Cond.getOperand(1);
    int SSECC = translateX86FSETCC(
        cast<CondCodeSDNode>(Cond.getOperand(2))->get(), CondOp0, CondOp1);

    if (SSECC != 8) {
      if (Subtarget->hasAVX512()) {
        SDValue Cmp = DAG.getNode(X86ISD::FSETCC, DL, MVT::i1, CondOp0, CondOp1,
                                  DAG.getConstant(SSECC, MVT::i8));
        return DAG.getNode(X86ISD::SELECT, DL, VT, Cmp, Op1, Op2);
      }
      SDValue Cmp = DAG.getNode(X86ISD::FSETCC, DL, VT, CondOp0, CondOp1,
                                DAG.getConstant(SSECC, MVT::i8));
      SDValue AndN = DAG.getNode(X86ISD::FANDN, DL, VT, Cmp, Op2);
      SDValue And = DAG.getNode(X86ISD::FAND, DL, VT, Cmp, Op1);
      return DAG.getNode(X86ISD::FOR, DL, VT, AndN, And);
    }
  }

  if (Cond.getOpcode() == ISD::SETCC) {
    SDValue NewCond = LowerSETCC(Cond, DAG);
    if (NewCond.getNode())
      Cond = NewCond;
  }

  // (select (x == 0), -1, y) -> (sign_bit (x - 1)) | y
  // (select (x == 0), y, -1) -> ~(sign_bit (x - 1)) | y
  // (select (x != 0), y, -1) -> (sign_bit (x - 1)) | y
  // (select (x != 0), -1, y) -> ~(sign_bit (x - 1)) | y
  if (Cond.getOpcode() == X86ISD::SETCC &&
      Cond.getOperand(1).getOpcode() == X86ISD::CMP &&
      isZero(Cond.getOperand(1).getOperand(1))) {
    SDValue Cmp = Cond.getOperand(1);

    unsigned CondCode =cast<ConstantSDNode>(Cond.getOperand(0))->getZExtValue();

    if ((isAllOnes(Op1) || isAllOnes(Op2)) &&
        (CondCode == X86::COND_E || CondCode == X86::COND_NE)) {
      SDValue Y = isAllOnes(Op2) ? Op1 : Op2;

      SDValue CmpOp0 = Cmp.getOperand(0);
      // Apply further optimizations for special cases
      // (select (x != 0), -1, 0) -> neg & sbb
      // (select (x == 0), 0, -1) -> neg & sbb
      if (ConstantSDNode *YC = dyn_cast<ConstantSDNode>(Y))
        if (YC->isNullValue() &&
            (isAllOnes(Op1) == (CondCode == X86::COND_NE))) {
          SDVTList VTs = DAG.getVTList(CmpOp0.getValueType(), MVT::i32);
          SDValue Neg = DAG.getNode(X86ISD::SUB, DL, VTs,
                                    DAG.getConstant(0, CmpOp0.getValueType()),
                                    CmpOp0);
          SDValue Res = DAG.getNode(X86ISD::SETCC_CARRY, DL, Op.getValueType(),
                                    DAG.getConstant(X86::COND_B, MVT::i8),
                                    SDValue(Neg.getNode(), 1));
          return Res;
        }

      Cmp = DAG.getNode(X86ISD::CMP, DL, MVT::i32,
                        CmpOp0, DAG.getConstant(1, CmpOp0.getValueType()));
      Cmp = ConvertCmpIfNecessary(Cmp, DAG);

      SDValue Res =   // Res = 0 or -1.
        DAG.getNode(X86ISD::SETCC_CARRY, DL, Op.getValueType(),
                    DAG.getConstant(X86::COND_B, MVT::i8), Cmp);

      if (isAllOnes(Op1) != (CondCode == X86::COND_E))
        Res = DAG.getNOT(DL, Res, Res.getValueType());

      ConstantSDNode *N2C = dyn_cast<ConstantSDNode>(Op2);
      if (!N2C || !N2C->isNullValue())
        Res = DAG.getNode(ISD::OR, DL, Res.getValueType(), Res, Y);
      return Res;
    }
  }

  // Look past (and (setcc_carry (cmp ...)), 1).
  if (Cond.getOpcode() == ISD::AND &&
      Cond.getOperand(0).getOpcode() == X86ISD::SETCC_CARRY) {
    ConstantSDNode *C = dyn_cast<ConstantSDNode>(Cond.getOperand(1));
    if (C && C->getAPIntValue() == 1)
      Cond = Cond.getOperand(0);
  }

  // If condition flag is set by a X86ISD::CMP, then use it as the condition
  // setting operand in place of the X86ISD::SETCC.
  unsigned CondOpcode = Cond.getOpcode();
  if (CondOpcode == X86ISD::SETCC ||
      CondOpcode == X86ISD::SETCC_CARRY) {
    CC = Cond.getOperand(0);

    SDValue Cmp = Cond.getOperand(1);
    unsigned Opc = Cmp.getOpcode();
    MVT VT = Op.getSimpleValueType();

    bool IllegalFPCMov = false;
    if (VT.isFloatingPoint() && !VT.isVector() &&
        !isScalarFPTypeInSSEReg(VT))  // FPStack?
      IllegalFPCMov = !hasFPCMov(cast<ConstantSDNode>(CC)->getSExtValue());

    if ((isX86LogicalCmp(Cmp) && !IllegalFPCMov) ||
        Opc == X86ISD::BT) { // FIXME
      Cond = Cmp;
      addTest = false;
    }
  } else if (CondOpcode == ISD::USUBO || CondOpcode == ISD::SSUBO ||
             CondOpcode == ISD::UADDO || CondOpcode == ISD::SADDO ||
             ((CondOpcode == ISD::UMULO || CondOpcode == ISD::SMULO) &&
              Cond.getOperand(0).getValueType() != MVT::i8)) {
    SDValue LHS = Cond.getOperand(0);
    SDValue RHS = Cond.getOperand(1);
    unsigned X86Opcode;
    unsigned X86Cond;
    SDVTList VTs;
    switch (CondOpcode) {
    case ISD::UADDO: X86Opcode = X86ISD::ADD; X86Cond = X86::COND_B; break;
    case ISD::SADDO: X86Opcode = X86ISD::ADD; X86Cond = X86::COND_O; break;
    case ISD::USUBO: X86Opcode = X86ISD::SUB; X86Cond = X86::COND_B; break;
    case ISD::SSUBO: X86Opcode = X86ISD::SUB; X86Cond = X86::COND_O; break;
    case ISD::UMULO: X86Opcode = X86ISD::UMUL; X86Cond = X86::COND_O; break;
    case ISD::SMULO: X86Opcode = X86ISD::SMUL; X86Cond = X86::COND_O; break;
    default: llvm_unreachable("unexpected overflowing operator");
    }
    if (CondOpcode == ISD::UMULO)
      VTs = DAG.getVTList(LHS.getValueType(), LHS.getValueType(),
                          MVT::i32);
    else
      VTs = DAG.getVTList(LHS.getValueType(), MVT::i32);

    SDValue X86Op = DAG.getNode(X86Opcode, DL, VTs, LHS, RHS);

    if (CondOpcode == ISD::UMULO)
      Cond = X86Op.getValue(2);
    else
      Cond = X86Op.getValue(1);

    CC = DAG.getConstant(X86Cond, MVT::i8);
    addTest = false;
  }

  if (addTest) {
    // Look pass the truncate if the high bits are known zero.
    if (isTruncWithZeroHighBitsInput(Cond, DAG))
        Cond = Cond.getOperand(0);

    // We know the result of AND is compared against zero. Try to match
    // it to BT.
    if (Cond.getOpcode() == ISD::AND && Cond.hasOneUse()) {
      SDValue NewSetCC = LowerToBT(Cond, ISD::SETNE, DL, DAG);
      if (NewSetCC.getNode()) {
        CC = NewSetCC.getOperand(0);
        Cond = NewSetCC.getOperand(1);
        addTest = false;
      }
    }
  }

  if (addTest) {
    CC = DAG.getConstant(X86::COND_NE, MVT::i8);
    Cond = EmitTest(Cond, X86::COND_NE, DL, DAG);
  }

  // a <  b ? -1 :  0 -> RES = ~setcc_carry
  // a <  b ?  0 : -1 -> RES = setcc_carry
  // a >= b ? -1 :  0 -> RES = setcc_carry
  // a >= b ?  0 : -1 -> RES = ~setcc_carry
  if (Cond.getOpcode() == X86ISD::SUB) {
    Cond = ConvertCmpIfNecessary(Cond, DAG);
    unsigned CondCode = cast<ConstantSDNode>(CC)->getZExtValue();

    if ((CondCode == X86::COND_AE || CondCode == X86::COND_B) &&
        (isAllOnes(Op1) || isAllOnes(Op2)) && (isZero(Op1) || isZero(Op2))) {
      SDValue Res = DAG.getNode(X86ISD::SETCC_CARRY, DL, Op.getValueType(),
                                DAG.getConstant(X86::COND_B, MVT::i8), Cond);
      if (isAllOnes(Op1) != (CondCode == X86::COND_B))
        return DAG.getNOT(DL, Res, Res.getValueType());
      return Res;
    }
  }

  // X86 doesn't have an i8 cmov. If both operands are the result of a truncate
  // widen the cmov and push the truncate through. This avoids introducing a new
  // branch during isel and doesn't add any extensions.
  if (Op.getValueType() == MVT::i8 &&
      Op1.getOpcode() == ISD::TRUNCATE && Op2.getOpcode() == ISD::TRUNCATE) {
    SDValue T1 = Op1.getOperand(0), T2 = Op2.getOperand(0);
    if (T1.getValueType() == T2.getValueType() &&
        // Blacklist CopyFromReg to avoid partial register stalls.
        T1.getOpcode() != ISD::CopyFromReg && T2.getOpcode()!=ISD::CopyFromReg){
      SDVTList VTs = DAG.getVTList(T1.getValueType(), MVT::Glue);
      SDValue Cmov = DAG.getNode(X86ISD::CMOV, DL, VTs, T2, T1, CC, Cond);
      return DAG.getNode(ISD::TRUNCATE, DL, Op.getValueType(), Cmov);
    }
  }

  // X86ISD::CMOV means set the result (which is operand 1) to the RHS if
  // condition is true.
  SDVTList VTs = DAG.getVTList(Op.getValueType(), MVT::Glue);
  SDValue Ops[] = { Op2, Op1, CC, Cond };
  return DAG.getNode(X86ISD::CMOV, DL, VTs, Ops);
}

static SDValue LowerSIGN_EXTEND_AVX512(SDValue Op, SelectionDAG &DAG) {
  MVT VT = Op->getSimpleValueType(0);
  SDValue In = Op->getOperand(0);
  MVT InVT = In.getSimpleValueType();
  SDLoc dl(Op);

  unsigned int NumElts = VT.getVectorNumElements();
  if (NumElts != 8 && NumElts != 16)
    return SDValue();

  if (VT.is512BitVector() && InVT.getVectorElementType() != MVT::i1)
    return DAG.getNode(X86ISD::VSEXT, dl, VT, In);

  const TargetLowering &TLI = DAG.getTargetLoweringInfo();
  assert (InVT.getVectorElementType() == MVT::i1 && "Unexpected vector type");

  MVT ExtVT = (NumElts == 8) ? MVT::v8i64 : MVT::v16i32;
  Constant *C = ConstantInt::get(*DAG.getContext(),
    APInt::getAllOnesValue(ExtVT.getScalarType().getSizeInBits()));

  SDValue CP = DAG.getConstantPool(C, TLI.getPointerTy());
  unsigned Alignment = cast<ConstantPoolSDNode>(CP)->getAlignment();
  SDValue Ld = DAG.getLoad(ExtVT.getScalarType(), dl, DAG.getEntryNode(), CP,
                          MachinePointerInfo::getConstantPool(),
                          false, false, false, Alignment);
  SDValue Brcst = DAG.getNode(X86ISD::VBROADCASTM, dl, ExtVT, In, Ld);
  if (VT.is512BitVector())
    return Brcst;
  return DAG.getNode(X86ISD::VTRUNC, dl, VT, Brcst);
}

static SDValue LowerSIGN_EXTEND(SDValue Op, const X86Subtarget *Subtarget,
                                SelectionDAG &DAG) {
  MVT VT = Op->getSimpleValueType(0);
  SDValue In = Op->getOperand(0);
  MVT InVT = In.getSimpleValueType();
  SDLoc dl(Op);

  if (VT.is512BitVector() || InVT.getVectorElementType() == MVT::i1)
    return LowerSIGN_EXTEND_AVX512(Op, DAG);

  if ((VT != MVT::v4i64 || InVT != MVT::v4i32) &&
      (VT != MVT::v8i32 || InVT != MVT::v8i16) &&
      (VT != MVT::v16i16 || InVT != MVT::v16i8))
    return SDValue();

  if (Subtarget->hasInt256())
    return DAG.getNode(X86ISD::VSEXT, dl, VT, In);

  // Optimize vectors in AVX mode
  // Sign extend  v8i16 to v8i32 and
  //              v4i32 to v4i64
  //
  // Divide input vector into two parts
  // for v4i32 the shuffle mask will be { 0, 1, -1, -1} {2, 3, -1, -1}
  // use vpmovsx instruction to extend v4i32 -> v2i64; v8i16 -> v4i32
  // concat the vectors to original VT

  unsigned NumElems = InVT.getVectorNumElements();
  SDValue Undef = DAG.getUNDEF(InVT);

  SmallVector<int,8> ShufMask1(NumElems, -1);
  for (unsigned i = 0; i != NumElems/2; ++i)
    ShufMask1[i] = i;

  SDValue OpLo = DAG.getVectorShuffle(InVT, dl, In, Undef, &ShufMask1[0]);

  SmallVector<int,8> ShufMask2(NumElems, -1);
  for (unsigned i = 0; i != NumElems/2; ++i)
    ShufMask2[i] = i + NumElems/2;

  SDValue OpHi = DAG.getVectorShuffle(InVT, dl, In, Undef, &ShufMask2[0]);

  MVT HalfVT = MVT::getVectorVT(VT.getScalarType(),
                                VT.getVectorNumElements()/2);

  OpLo = DAG.getNode(X86ISD::VSEXT, dl, HalfVT, OpLo);
  OpHi = DAG.getNode(X86ISD::VSEXT, dl, HalfVT, OpHi);

  return DAG.getNode(ISD::CONCAT_VECTORS, dl, VT, OpLo, OpHi);
}

// Lower vector extended loads using a shuffle. If SSSE3 is not available we
// may emit an illegal shuffle but the expansion is still better than scalar
// code. We generate X86ISD::VSEXT for SEXTLOADs if it's available, otherwise
// we'll emit a shuffle and a arithmetic shift.
// TODO: It is possible to support ZExt by zeroing the undef values during
// the shuffle phase or after the shuffle.
static SDValue LowerExtendedLoad(SDValue Op, const X86Subtarget *Subtarget,
                                 SelectionDAG &DAG) {
  MVT RegVT = Op.getSimpleValueType();
  assert(RegVT.isVector() && "We only custom lower vector sext loads.");
  assert(RegVT.isInteger() &&
         "We only custom lower integer vector sext loads.");

  // Nothing useful we can do without SSE2 shuffles.
  assert(Subtarget->hasSSE2() && "We only custom lower sext loads with SSE2.");

  LoadSDNode *Ld = cast<LoadSDNode>(Op.getNode());
  SDLoc dl(Ld);
  EVT MemVT = Ld->getMemoryVT();
  const TargetLowering &TLI = DAG.getTargetLoweringInfo();
  unsigned RegSz = RegVT.getSizeInBits();

  ISD::LoadExtType Ext = Ld->getExtensionType();

  assert((Ext == ISD::EXTLOAD || Ext == ISD::SEXTLOAD)
         && "Only anyext and sext are currently implemented.");
  assert(MemVT != RegVT && "Cannot extend to the same type");
  assert(MemVT.isVector() && "Must load a vector from memory");

  unsigned NumElems = RegVT.getVectorNumElements();
  unsigned MemSz = MemVT.getSizeInBits();
  assert(RegSz > MemSz && "Register size must be greater than the mem size");

  if (Ext == ISD::SEXTLOAD && RegSz == 256 && !Subtarget->hasInt256()) {
    // The only way in which we have a legal 256-bit vector result but not the
    // integer 256-bit operations needed to directly lower a sextload is if we
    // have AVX1 but not AVX2. In that case, we can always emit a sextload to
    // a 128-bit vector and a normal sign_extend to 256-bits that should get
    // correctly legalized. We do this late to allow the canonical form of
    // sextload to persist throughout the rest of the DAG combiner -- it wants
    // to fold together any extensions it can, and so will fuse a sign_extend
    // of an sextload into a sextload targeting a wider value.
    SDValue Load;
    if (MemSz == 128) {
      // Just switch this to a normal load.
      assert(TLI.isTypeLegal(MemVT) && "If the memory type is a 128-bit type, "
                                       "it must be a legal 128-bit vector "
                                       "type!");
      Load = DAG.getLoad(MemVT, dl, Ld->getChain(), Ld->getBasePtr(),
                  Ld->getPointerInfo(), Ld->isVolatile(), Ld->isNonTemporal(),
                  Ld->isInvariant(), Ld->getAlignment());
    } else {
      assert(MemSz < 128 &&
             "Can't extend a type wider than 128 bits to a 256 bit vector!");
      // Do an sext load to a 128-bit vector type. We want to use the same
      // number of elements, but elements half as wide. This will end up being
      // recursively lowered by this routine, but will succeed as we definitely
      // have all the necessary features if we're using AVX1.
      EVT HalfEltVT =
          EVT::getIntegerVT(*DAG.getContext(), RegVT.getScalarSizeInBits() / 2);
      EVT HalfVecVT = EVT::getVectorVT(*DAG.getContext(), HalfEltVT, NumElems);
      Load =
          DAG.getExtLoad(Ext, dl, HalfVecVT, Ld->getChain(), Ld->getBasePtr(),
                         Ld->getPointerInfo(), MemVT, Ld->isVolatile(),
                         Ld->isNonTemporal(), Ld->isInvariant(),
                         Ld->getAlignment());
    }

    // Replace chain users with the new chain.
    assert(Load->getNumValues() == 2 && "Loads must carry a chain!");
    DAG.ReplaceAllUsesOfValueWith(SDValue(Ld, 1), Load.getValue(1));

    // Finally, do a normal sign-extend to the desired register.
    return DAG.getSExtOrTrunc(Load, dl, RegVT);
  }

  // All sizes must be a power of two.
  assert(isPowerOf2_32(RegSz * MemSz * NumElems) &&
         "Non-power-of-two elements are not custom lowered!");

  // Attempt to load the original value using scalar loads.
  // Find the largest scalar type that divides the total loaded size.
  MVT SclrLoadTy = MVT::i8;
  for (unsigned tp = MVT::FIRST_INTEGER_VALUETYPE;
       tp < MVT::LAST_INTEGER_VALUETYPE; ++tp) {
    MVT Tp = (MVT::SimpleValueType)tp;
    if (TLI.isTypeLegal(Tp) && ((MemSz % Tp.getSizeInBits()) == 0)) {
      SclrLoadTy = Tp;
    }
  }

  // On 32bit systems, we can't save 64bit integers. Try bitcasting to F64.
  if (TLI.isTypeLegal(MVT::f64) && SclrLoadTy.getSizeInBits() < 64 &&
      (64 <= MemSz))
    SclrLoadTy = MVT::f64;

  // Calculate the number of scalar loads that we need to perform
  // in order to load our vector from memory.
  unsigned NumLoads = MemSz / SclrLoadTy.getSizeInBits();

  assert((Ext != ISD::SEXTLOAD || NumLoads == 1) &&
         "Can only lower sext loads with a single scalar load!");

  unsigned loadRegZize = RegSz;
  if (Ext == ISD::SEXTLOAD && RegSz == 256)
    loadRegZize /= 2;

  // Represent our vector as a sequence of elements which are the
  // largest scalar that we can load.
  EVT LoadUnitVecVT = EVT::getVectorVT(
      *DAG.getContext(), SclrLoadTy, loadRegZize / SclrLoadTy.getSizeInBits());

  // Represent the data using the same element type that is stored in
  // memory. In practice, we ''widen'' MemVT.
  EVT WideVecVT =
      EVT::getVectorVT(*DAG.getContext(), MemVT.getScalarType(),
                       loadRegZize / MemVT.getScalarType().getSizeInBits());

  assert(WideVecVT.getSizeInBits() == LoadUnitVecVT.getSizeInBits() &&
         "Invalid vector type");

  // We can't shuffle using an illegal type.
  assert(TLI.isTypeLegal(WideVecVT) &&
         "We only lower types that form legal widened vector types");

  SmallVector<SDValue, 8> Chains;
  SDValue Ptr = Ld->getBasePtr();
  SDValue Increment =
      DAG.getConstant(SclrLoadTy.getSizeInBits() / 8, TLI.getPointerTy());
  SDValue Res = DAG.getUNDEF(LoadUnitVecVT);

  for (unsigned i = 0; i < NumLoads; ++i) {
    // Perform a single load.
    SDValue ScalarLoad =
        DAG.getLoad(SclrLoadTy, dl, Ld->getChain(), Ptr, Ld->getPointerInfo(),
                    Ld->isVolatile(), Ld->isNonTemporal(), Ld->isInvariant(),
                    Ld->getAlignment());
    Chains.push_back(ScalarLoad.getValue(1));
    // Create the first element type using SCALAR_TO_VECTOR in order to avoid
    // another round of DAGCombining.
    if (i == 0)
      Res = DAG.getNode(ISD::SCALAR_TO_VECTOR, dl, LoadUnitVecVT, ScalarLoad);
    else
      Res = DAG.getNode(ISD::INSERT_VECTOR_ELT, dl, LoadUnitVecVT, Res,
                        ScalarLoad, DAG.getIntPtrConstant(i));

    Ptr = DAG.getNode(ISD::ADD, dl, Ptr.getValueType(), Ptr, Increment);
  }

  SDValue TF = DAG.getNode(ISD::TokenFactor, dl, MVT::Other, Chains);

  // Bitcast the loaded value to a vector of the original element type, in
  // the size of the target vector type.
  SDValue SlicedVec = DAG.getNode(ISD::BITCAST, dl, WideVecVT, Res);
  unsigned SizeRatio = RegSz / MemSz;

  if (Ext == ISD::SEXTLOAD) {
    // If we have SSE4.1, we can directly emit a VSEXT node.
    if (Subtarget->hasSSE41()) {
      SDValue Sext = DAG.getNode(X86ISD::VSEXT, dl, RegVT, SlicedVec);
      DAG.ReplaceAllUsesOfValueWith(SDValue(Ld, 1), TF);
      return Sext;
    }

    // Otherwise we'll shuffle the small elements in the high bits of the
    // larger type and perform an arithmetic shift. If the shift is not legal
    // it's better to scalarize.
    assert(TLI.isOperationLegalOrCustom(ISD::SRA, RegVT) &&
           "We can't implement a sext load without an arithmetic right shift!");

    // Redistribute the loaded elements into the different locations.
    SmallVector<int, 16> ShuffleVec(NumElems * SizeRatio, -1);
    for (unsigned i = 0; i != NumElems; ++i)
      ShuffleVec[i * SizeRatio + SizeRatio - 1] = i;

    SDValue Shuff = DAG.getVectorShuffle(
        WideVecVT, dl, SlicedVec, DAG.getUNDEF(WideVecVT), &ShuffleVec[0]);

    Shuff = DAG.getNode(ISD::BITCAST, dl, RegVT, Shuff);

    // Build the arithmetic shift.
    unsigned Amt = RegVT.getVectorElementType().getSizeInBits() -
                   MemVT.getVectorElementType().getSizeInBits();
    Shuff =
        DAG.getNode(ISD::SRA, dl, RegVT, Shuff, DAG.getConstant(Amt, RegVT));

    DAG.ReplaceAllUsesOfValueWith(SDValue(Ld, 1), TF);
    return Shuff;
  }

  // Redistribute the loaded elements into the different locations.
  SmallVector<int, 16> ShuffleVec(NumElems * SizeRatio, -1);
  for (unsigned i = 0; i != NumElems; ++i)
    ShuffleVec[i * SizeRatio] = i;

  SDValue Shuff = DAG.getVectorShuffle(WideVecVT, dl, SlicedVec,
                                       DAG.getUNDEF(WideVecVT), &ShuffleVec[0]);

  // Bitcast to the requested type.
  Shuff = DAG.getNode(ISD::BITCAST, dl, RegVT, Shuff);
  DAG.ReplaceAllUsesOfValueWith(SDValue(Ld, 1), TF);
  return Shuff;
}

// isAndOrOfSingleUseSetCCs - Return true if node is an ISD::AND or
// ISD::OR of two X86ISD::SETCC nodes each of which has no other use apart
// from the AND / OR.
static bool isAndOrOfSetCCs(SDValue Op, unsigned &Opc) {
  Opc = Op.getOpcode();
  if (Opc != ISD::OR && Opc != ISD::AND)
    return false;
  return (Op.getOperand(0).getOpcode() == X86ISD::SETCC &&
          Op.getOperand(0).hasOneUse() &&
          Op.getOperand(1).getOpcode() == X86ISD::SETCC &&
          Op.getOperand(1).hasOneUse());
}

// isXor1OfSetCC - Return true if node is an ISD::XOR of a X86ISD::SETCC and
// 1 and that the SETCC node has a single use.
static bool isXor1OfSetCC(SDValue Op) {
  if (Op.getOpcode() != ISD::XOR)
    return false;
  ConstantSDNode *N1C = dyn_cast<ConstantSDNode>(Op.getOperand(1));
  if (N1C && N1C->getAPIntValue() == 1) {
    return Op.getOperand(0).getOpcode() == X86ISD::SETCC &&
      Op.getOperand(0).hasOneUse();
  }
  return false;
}

SDValue X86TargetLowering::LowerBRCOND(SDValue Op, SelectionDAG &DAG) const {
  bool addTest = true;
  SDValue Chain = Op.getOperand(0);
  SDValue Cond  = Op.getOperand(1);
  SDValue Dest  = Op.getOperand(2);
  SDLoc dl(Op);
  SDValue CC;
  bool Inverted = false;

  if (Cond.getOpcode() == ISD::SETCC) {
    // Check for setcc([su]{add,sub,mul}o == 0).
    if (cast<CondCodeSDNode>(Cond.getOperand(2))->get() == ISD::SETEQ &&
        isa<ConstantSDNode>(Cond.getOperand(1)) &&
        cast<ConstantSDNode>(Cond.getOperand(1))->isNullValue() &&
        Cond.getOperand(0).getResNo() == 1 &&
        (Cond.getOperand(0).getOpcode() == ISD::SADDO ||
         Cond.getOperand(0).getOpcode() == ISD::UADDO ||
         Cond.getOperand(0).getOpcode() == ISD::SSUBO ||
         Cond.getOperand(0).getOpcode() == ISD::USUBO ||
         Cond.getOperand(0).getOpcode() == ISD::SMULO ||
         Cond.getOperand(0).getOpcode() == ISD::UMULO)) {
      Inverted = true;
      Cond = Cond.getOperand(0);
    } else {
      SDValue NewCond = LowerSETCC(Cond, DAG);
      if (NewCond.getNode())
        Cond = NewCond;
    }
  }
#if 0
  // FIXME: LowerXALUO doesn't handle these!!
  else if (Cond.getOpcode() == X86ISD::ADD  ||
           Cond.getOpcode() == X86ISD::SUB  ||
           Cond.getOpcode() == X86ISD::SMUL ||
           Cond.getOpcode() == X86ISD::UMUL)
    Cond = LowerXALUO(Cond, DAG);
#endif

  // Look pass (and (setcc_carry (cmp ...)), 1).
  if (Cond.getOpcode() == ISD::AND &&
      Cond.getOperand(0).getOpcode() == X86ISD::SETCC_CARRY) {
    ConstantSDNode *C = dyn_cast<ConstantSDNode>(Cond.getOperand(1));
    if (C && C->getAPIntValue() == 1)
      Cond = Cond.getOperand(0);
  }

  // If condition flag is set by a X86ISD::CMP, then use it as the condition
  // setting operand in place of the X86ISD::SETCC.
  unsigned CondOpcode = Cond.getOpcode();
  if (CondOpcode == X86ISD::SETCC ||
      CondOpcode == X86ISD::SETCC_CARRY) {
    CC = Cond.getOperand(0);

    SDValue Cmp = Cond.getOperand(1);
    unsigned Opc = Cmp.getOpcode();
    // FIXME: WHY THE SPECIAL CASING OF LogicalCmp??
    if (isX86LogicalCmp(Cmp) || Opc == X86ISD::BT) {
      Cond = Cmp;
      addTest = false;
    } else {
      switch (cast<ConstantSDNode>(CC)->getZExtValue()) {
      default: break;
      case X86::COND_O:
      case X86::COND_B:
        // These can only come from an arithmetic instruction with overflow,
        // e.g. SADDO, UADDO.
        Cond = Cond.getNode()->getOperand(1);
        addTest = false;
        break;
      }
    }
  }
  CondOpcode = Cond.getOpcode();
  if (CondOpcode == ISD::UADDO || CondOpcode == ISD::SADDO ||
      CondOpcode == ISD::USUBO || CondOpcode == ISD::SSUBO ||
      ((CondOpcode == ISD::UMULO || CondOpcode == ISD::SMULO) &&
       Cond.getOperand(0).getValueType() != MVT::i8)) {
    SDValue LHS = Cond.getOperand(0);
    SDValue RHS = Cond.getOperand(1);
    unsigned X86Opcode;
    unsigned X86Cond;
    SDVTList VTs;
    // Keep this in sync with LowerXALUO, otherwise we might create redundant
    // instructions that can't be removed afterwards (i.e. X86ISD::ADD and
    // X86ISD::INC).
    switch (CondOpcode) {
    case ISD::UADDO: X86Opcode = X86ISD::ADD; X86Cond = X86::COND_B; break;
    case ISD::SADDO:
      if (ConstantSDNode *C = dyn_cast<ConstantSDNode>(RHS))
        if (C->isOne()) {
          X86Opcode = X86ISD::INC; X86Cond = X86::COND_O;
          break;
        }
      X86Opcode = X86ISD::ADD; X86Cond = X86::COND_O; break;
    case ISD::USUBO: X86Opcode = X86ISD::SUB; X86Cond = X86::COND_B; break;
    case ISD::SSUBO:
      if (ConstantSDNode *C = dyn_cast<ConstantSDNode>(RHS))
        if (C->isOne()) {
          X86Opcode = X86ISD::DEC; X86Cond = X86::COND_O;
          break;
        }
      X86Opcode = X86ISD::SUB; X86Cond = X86::COND_O; break;
    case ISD::UMULO: X86Opcode = X86ISD::UMUL; X86Cond = X86::COND_O; break;
    case ISD::SMULO: X86Opcode = X86ISD::SMUL; X86Cond = X86::COND_O; break;
    default: llvm_unreachable("unexpected overflowing operator");
    }
    if (Inverted)
      X86Cond = X86::GetOppositeBranchCondition((X86::CondCode)X86Cond);
    if (CondOpcode == ISD::UMULO)
      VTs = DAG.getVTList(LHS.getValueType(), LHS.getValueType(),
                          MVT::i32);
    else
      VTs = DAG.getVTList(LHS.getValueType(), MVT::i32);

    SDValue X86Op = DAG.getNode(X86Opcode, dl, VTs, LHS, RHS);

    if (CondOpcode == ISD::UMULO)
      Cond = X86Op.getValue(2);
    else
      Cond = X86Op.getValue(1);

    CC = DAG.getConstant(X86Cond, MVT::i8);
    addTest = false;
  } else {
    unsigned CondOpc;
    if (Cond.hasOneUse() && isAndOrOfSetCCs(Cond, CondOpc)) {
      SDValue Cmp = Cond.getOperand(0).getOperand(1);
      if (CondOpc == ISD::OR) {
        // Also, recognize the pattern generated by an FCMP_UNE. We can emit
        // two branches instead of an explicit OR instruction with a
        // separate test.
        if (Cmp == Cond.getOperand(1).getOperand(1) &&
            isX86LogicalCmp(Cmp)) {
          CC = Cond.getOperand(0).getOperand(0);
          Chain = DAG.getNode(X86ISD::BRCOND, dl, Op.getValueType(),
                              Chain, Dest, CC, Cmp);
          CC = Cond.getOperand(1).getOperand(0);
          Cond = Cmp;
          addTest = false;
        }
      } else { // ISD::AND
        // Also, recognize the pattern generated by an FCMP_OEQ. We can emit
        // two branches instead of an explicit AND instruction with a
        // separate test. However, we only do this if this block doesn't
        // have a fall-through edge, because this requires an explicit
        // jmp when the condition is false.
        if (Cmp == Cond.getOperand(1).getOperand(1) &&
            isX86LogicalCmp(Cmp) &&
            Op.getNode()->hasOneUse()) {
          X86::CondCode CCode =
            (X86::CondCode)Cond.getOperand(0).getConstantOperandVal(0);
          CCode = X86::GetOppositeBranchCondition(CCode);
          CC = DAG.getConstant(CCode, MVT::i8);
          SDNode *User = *Op.getNode()->use_begin();
          // Look for an unconditional branch following this conditional branch.
          // We need this because we need to reverse the successors in order
          // to implement FCMP_OEQ.
          if (User->getOpcode() == ISD::BR) {
            SDValue FalseBB = User->getOperand(1);
            SDNode *NewBR =
              DAG.UpdateNodeOperands(User, User->getOperand(0), Dest);
            assert(NewBR == User);
            (void)NewBR;
            Dest = FalseBB;

            Chain = DAG.getNode(X86ISD::BRCOND, dl, Op.getValueType(),
                                Chain, Dest, CC, Cmp);
            X86::CondCode CCode =
              (X86::CondCode)Cond.getOperand(1).getConstantOperandVal(0);
            CCode = X86::GetOppositeBranchCondition(CCode);
            CC = DAG.getConstant(CCode, MVT::i8);
            Cond = Cmp;
            addTest = false;
          }
        }
      }
    } else if (Cond.hasOneUse() && isXor1OfSetCC(Cond)) {
      // Recognize for xorb (setcc), 1 patterns. The xor inverts the condition.
      // It should be transformed during dag combiner except when the condition
      // is set by a arithmetics with overflow node.
      X86::CondCode CCode =
        (X86::CondCode)Cond.getOperand(0).getConstantOperandVal(0);
      CCode = X86::GetOppositeBranchCondition(CCode);
      CC = DAG.getConstant(CCode, MVT::i8);
      Cond = Cond.getOperand(0).getOperand(1);
      addTest = false;
    } else if (Cond.getOpcode() == ISD::SETCC &&
               cast<CondCodeSDNode>(Cond.getOperand(2))->get() == ISD::SETOEQ) {
      // For FCMP_OEQ, we can emit
      // two branches instead of an explicit AND instruction with a
      // separate test. However, we only do this if this block doesn't
      // have a fall-through edge, because this requires an explicit
      // jmp when the condition is false.
      if (Op.getNode()->hasOneUse()) {
        SDNode *User = *Op.getNode()->use_begin();
        // Look for an unconditional branch following this conditional branch.
        // We need this because we need to reverse the successors in order
        // to implement FCMP_OEQ.
        if (User->getOpcode() == ISD::BR) {
          SDValue FalseBB = User->getOperand(1);
          SDNode *NewBR =
            DAG.UpdateNodeOperands(User, User->getOperand(0), Dest);
          assert(NewBR == User);
          (void)NewBR;
          Dest = FalseBB;

          SDValue Cmp = DAG.getNode(X86ISD::CMP, dl, MVT::i32,
                                    Cond.getOperand(0), Cond.getOperand(1));
          Cmp = ConvertCmpIfNecessary(Cmp, DAG);
          CC = DAG.getConstant(X86::COND_NE, MVT::i8);
          Chain = DAG.getNode(X86ISD::BRCOND, dl, Op.getValueType(),
                              Chain, Dest, CC, Cmp);
          CC = DAG.getConstant(X86::COND_P, MVT::i8);
          Cond = Cmp;
          addTest = false;
        }
      }
    } else if (Cond.getOpcode() == ISD::SETCC &&
               cast<CondCodeSDNode>(Cond.getOperand(2))->get() == ISD::SETUNE) {
      // For FCMP_UNE, we can emit
      // two branches instead of an explicit AND instruction with a
      // separate test. However, we only do this if this block doesn't
      // have a fall-through edge, because this requires an explicit
      // jmp when the condition is false.
      if (Op.getNode()->hasOneUse()) {
        SDNode *User = *Op.getNode()->use_begin();
        // Look for an unconditional branch following this conditional branch.
        // We need this because we need to reverse the successors in order
        // to implement FCMP_UNE.
        if (User->getOpcode() == ISD::BR) {
          SDValue FalseBB = User->getOperand(1);
          SDNode *NewBR =
            DAG.UpdateNodeOperands(User, User->getOperand(0), Dest);
          assert(NewBR == User);
          (void)NewBR;

          SDValue Cmp = DAG.getNode(X86ISD::CMP, dl, MVT::i32,
                                    Cond.getOperand(0), Cond.getOperand(1));
          Cmp = ConvertCmpIfNecessary(Cmp, DAG);
          CC = DAG.getConstant(X86::COND_NE, MVT::i8);
          Chain = DAG.getNode(X86ISD::BRCOND, dl, Op.getValueType(),
                              Chain, Dest, CC, Cmp);
          CC = DAG.getConstant(X86::COND_NP, MVT::i8);
          Cond = Cmp;
          addTest = false;
          Dest = FalseBB;
        }
      }
    }
  }

  if (addTest) {
    // Look pass the truncate if the high bits are known zero.
    if (isTruncWithZeroHighBitsInput(Cond, DAG))
        Cond = Cond.getOperand(0);

    // We know the result of AND is compared against zero. Try to match
    // it to BT.
    if (Cond.getOpcode() == ISD::AND && Cond.hasOneUse()) {
      SDValue NewSetCC = LowerToBT(Cond, ISD::SETNE, dl, DAG);
      if (NewSetCC.getNode()) {
        CC = NewSetCC.getOperand(0);
        Cond = NewSetCC.getOperand(1);
        addTest = false;
      }
    }
  }

  if (addTest) {
    X86::CondCode X86Cond = Inverted ? X86::COND_E : X86::COND_NE;
    CC = DAG.getConstant(X86Cond, MVT::i8);
    Cond = EmitTest(Cond, X86Cond, dl, DAG);
  }
  Cond = ConvertCmpIfNecessary(Cond, DAG);
  return DAG.getNode(X86ISD::BRCOND, dl, Op.getValueType(),
                     Chain, Dest, CC, Cond);
}

// Lower dynamic stack allocation to _alloca call for Cygwin/Mingw targets.
// Calls to _alloca are needed to probe the stack when allocating more than 4k
// bytes in one go. Touching the stack at 4K increments is necessary to ensure
// that the guard pages used by the OS virtual memory manager are allocated in
// correct sequence.
SDValue
X86TargetLowering::LowerDYNAMIC_STACKALLOC(SDValue Op,
                                           SelectionDAG &DAG) const {
  MachineFunction &MF = DAG.getMachineFunction();
  bool SplitStack = MF.shouldSplitStack();
  bool Lower = (Subtarget->isOSWindows() && !Subtarget->isTargetMacho()) ||
               SplitStack;
  SDLoc dl(Op);

  if (!Lower) {
    const TargetLowering &TLI = DAG.getTargetLoweringInfo();
    SDNode* Node = Op.getNode();

    unsigned SPReg = TLI.getStackPointerRegisterToSaveRestore();
    assert(SPReg && "Target cannot require DYNAMIC_STACKALLOC expansion and"
        " not tell us which reg is the stack pointer!");
    EVT VT = Node->getValueType(0);
    SDValue Tmp1 = SDValue(Node, 0);
    SDValue Tmp2 = SDValue(Node, 1);
    SDValue Tmp3 = Node->getOperand(2);
    SDValue Chain = Tmp1.getOperand(0);

    // Chain the dynamic stack allocation so that it doesn't modify the stack
    // pointer when other instructions are using the stack.
    Chain = DAG.getCALLSEQ_START(Chain, DAG.getIntPtrConstant(0, true),
        SDLoc(Node));

    SDValue Size = Tmp2.getOperand(1);
    SDValue SP = DAG.getCopyFromReg(Chain, dl, SPReg, VT);
    Chain = SP.getValue(1);
    unsigned Align = cast<ConstantSDNode>(Tmp3)->getZExtValue();
    const TargetFrameLowering &TFI = *DAG.getSubtarget().getFrameLowering();
    unsigned StackAlign = TFI.getStackAlignment();
    Tmp1 = DAG.getNode(ISD::SUB, dl, VT, SP, Size); // Value
    if (Align > StackAlign)
      Tmp1 = DAG.getNode(ISD::AND, dl, VT, Tmp1,
          DAG.getConstant(-(uint64_t)Align, VT));
    Chain = DAG.getCopyToReg(Chain, dl, SPReg, Tmp1); // Output chain

    Tmp2 = DAG.getCALLSEQ_END(Chain, DAG.getIntPtrConstant(0, true),
        DAG.getIntPtrConstant(0, true), SDValue(),
        SDLoc(Node));

    SDValue Ops[2] = { Tmp1, Tmp2 };
    return DAG.getMergeValues(Ops, dl);
  }

  // Get the inputs.
  SDValue Chain = Op.getOperand(0);
  SDValue Size  = Op.getOperand(1);
  unsigned Align = cast<ConstantSDNode>(Op.getOperand(2))->getZExtValue();
  EVT VT = Op.getNode()->getValueType(0);

  bool Is64Bit = Subtarget->is64Bit();
  EVT SPTy = Is64Bit ? MVT::i64 : MVT::i32;

  if (SplitStack) {
    MachineRegisterInfo &MRI = MF.getRegInfo();

    if (Is64Bit) {
      // The 64 bit implementation of segmented stacks needs to clobber both r10
      // r11. This makes it impossible to use it along with nested parameters.
      const Function *F = MF.getFunction();

      for (Function::const_arg_iterator I = F->arg_begin(), E = F->arg_end();
           I != E; ++I)
        if (I->hasNestAttr())
          report_fatal_error("Cannot use segmented stacks with functions that "
                             "have nested arguments.");
    }

    const TargetRegisterClass *AddrRegClass =
      getRegClassFor(Subtarget->is64Bit() ? MVT::i64:MVT::i32);
    unsigned Vreg = MRI.createVirtualRegister(AddrRegClass);
    Chain = DAG.getCopyToReg(Chain, dl, Vreg, Size);
    SDValue Value = DAG.getNode(X86ISD::SEG_ALLOCA, dl, SPTy, Chain,
                                DAG.getRegister(Vreg, SPTy));
    SDValue Ops1[2] = { Value, Chain };
    return DAG.getMergeValues(Ops1, dl);
  } else {
    SDValue Flag;
    unsigned Reg = (Subtarget->is64Bit() ? X86::RAX : X86::EAX);

    Chain = DAG.getCopyToReg(Chain, dl, Reg, Size, Flag);
    Flag = Chain.getValue(1);
    SDVTList NodeTys = DAG.getVTList(MVT::Other, MVT::Glue);

    Chain = DAG.getNode(X86ISD::WIN_ALLOCA, dl, NodeTys, Chain, Flag);

    const X86RegisterInfo *RegInfo = static_cast<const X86RegisterInfo *>(
        DAG.getSubtarget().getRegisterInfo());
    unsigned SPReg = RegInfo->getStackRegister();
    SDValue SP = DAG.getCopyFromReg(Chain, dl, SPReg, SPTy);
    Chain = SP.getValue(1);

    if (Align) {
      SP = DAG.getNode(ISD::AND, dl, VT, SP.getValue(0),
                       DAG.getConstant(-(uint64_t)Align, VT));
      Chain = DAG.getCopyToReg(Chain, dl, SPReg, SP);
    }

    SDValue Ops1[2] = { SP, Chain };
    return DAG.getMergeValues(Ops1, dl);
  }
}

SDValue X86TargetLowering::LowerVASTART(SDValue Op, SelectionDAG &DAG) const {
  MachineFunction &MF = DAG.getMachineFunction();
  X86MachineFunctionInfo *FuncInfo = MF.getInfo<X86MachineFunctionInfo>();

  const Value *SV = cast<SrcValueSDNode>(Op.getOperand(2))->getValue();
  SDLoc DL(Op);

  if (!Subtarget->is64Bit() || Subtarget->isTargetWin64()) {
    // vastart just stores the address of the VarArgsFrameIndex slot into the
    // memory location argument.
    SDValue FR = DAG.getFrameIndex(FuncInfo->getVarArgsFrameIndex(),
                                   getPointerTy());
    return DAG.getStore(Op.getOperand(0), DL, FR, Op.getOperand(1),
                        MachinePointerInfo(SV), false, false, 0);
  }

  // __va_list_tag:
  //   gp_offset         (0 - 6 * 8)
  //   fp_offset         (48 - 48 + 8 * 16)
  //   overflow_arg_area (point to parameters coming in memory).
  //   reg_save_area
  SmallVector<SDValue, 8> MemOps;
  SDValue FIN = Op.getOperand(1);
  // Store gp_offset
  SDValue Store = DAG.getStore(Op.getOperand(0), DL,
                               DAG.getConstant(FuncInfo->getVarArgsGPOffset(),
                                               MVT::i32),
                               FIN, MachinePointerInfo(SV), false, false, 0);
  MemOps.push_back(Store);

  // Store fp_offset
  FIN = DAG.getNode(ISD::ADD, DL, getPointerTy(),
                    FIN, DAG.getIntPtrConstant(4));
  Store = DAG.getStore(Op.getOperand(0), DL,
                       DAG.getConstant(FuncInfo->getVarArgsFPOffset(),
                                       MVT::i32),
                       FIN, MachinePointerInfo(SV, 4), false, false, 0);
  MemOps.push_back(Store);

  // Store ptr to overflow_arg_area
  FIN = DAG.getNode(ISD::ADD, DL, getPointerTy(),
                    FIN, DAG.getIntPtrConstant(4));
  SDValue OVFIN = DAG.getFrameIndex(FuncInfo->getVarArgsFrameIndex(),
                                    getPointerTy());
  Store = DAG.getStore(Op.getOperand(0), DL, OVFIN, FIN,
                       MachinePointerInfo(SV, 8),
                       false, false, 0);
  MemOps.push_back(Store);

  // Store ptr to reg_save_area.
  FIN = DAG.getNode(ISD::ADD, DL, getPointerTy(),
                    FIN, DAG.getIntPtrConstant(8));
  SDValue RSFIN = DAG.getFrameIndex(FuncInfo->getRegSaveFrameIndex(),
                                    getPointerTy());
  Store = DAG.getStore(Op.getOperand(0), DL, RSFIN, FIN,
                       MachinePointerInfo(SV, 16), false, false, 0);
  MemOps.push_back(Store);
  return DAG.getNode(ISD::TokenFactor, DL, MVT::Other, MemOps);
}

SDValue X86TargetLowering::LowerVAARG(SDValue Op, SelectionDAG &DAG) const {
  assert(Subtarget->is64Bit() &&
         "LowerVAARG only handles 64-bit va_arg!");
  assert((Subtarget->isTargetLinux() ||
          Subtarget->isTargetDarwin()) &&
          "Unhandled target in LowerVAARG");
  assert(Op.getNode()->getNumOperands() == 4);
  SDValue Chain = Op.getOperand(0);
  SDValue SrcPtr = Op.getOperand(1);
  const Value *SV = cast<SrcValueSDNode>(Op.getOperand(2))->getValue();
  unsigned Align = Op.getConstantOperandVal(3);
  SDLoc dl(Op);

  EVT ArgVT = Op.getNode()->getValueType(0);
  Type *ArgTy = ArgVT.getTypeForEVT(*DAG.getContext());
  uint32_t ArgSize = getDataLayout()->getTypeAllocSize(ArgTy);
  uint8_t ArgMode;

  // Decide which area this value should be read from.
  // TODO: Implement the AMD64 ABI in its entirety. This simple
  // selection mechanism works only for the basic types.
  if (ArgVT == MVT::f80) {
    llvm_unreachable("va_arg for f80 not yet implemented");
  } else if (ArgVT.isFloatingPoint() && ArgSize <= 16 /*bytes*/) {
    ArgMode = 2;  // Argument passed in XMM register. Use fp_offset.
  } else if (ArgVT.isInteger() && ArgSize <= 32 /*bytes*/) {
    ArgMode = 1;  // Argument passed in GPR64 register(s). Use gp_offset.
  } else {
    llvm_unreachable("Unhandled argument type in LowerVAARG");
  }

  if (ArgMode == 2) {
    // Sanity Check: Make sure using fp_offset makes sense.
    assert(!DAG.getTarget().Options.UseSoftFloat &&
           !(DAG.getMachineFunction()
                .getFunction()->getAttributes()
                .hasAttribute(AttributeSet::FunctionIndex,
                              Attribute::NoImplicitFloat)) &&
           Subtarget->hasSSE1());
  }

  // Insert VAARG_64 node into the DAG
  // VAARG_64 returns two values: Variable Argument Address, Chain
  SmallVector<SDValue, 11> InstOps;
  InstOps.push_back(Chain);
  InstOps.push_back(SrcPtr);
  InstOps.push_back(DAG.getConstant(ArgSize, MVT::i32));
  InstOps.push_back(DAG.getConstant(ArgMode, MVT::i8));
  InstOps.push_back(DAG.getConstant(Align, MVT::i32));
  SDVTList VTs = DAG.getVTList(getPointerTy(), MVT::Other);
  SDValue VAARG = DAG.getMemIntrinsicNode(X86ISD::VAARG_64, dl,
                                          VTs, InstOps, MVT::i64,
                                          MachinePointerInfo(SV),
                                          /*Align=*/0,
                                          /*Volatile=*/false,
                                          /*ReadMem=*/true,
                                          /*WriteMem=*/true);
  Chain = VAARG.getValue(1);

  // Load the next argument and return it
  return DAG.getLoad(ArgVT, dl,
                     Chain,
                     VAARG,
                     MachinePointerInfo(),
                     false, false, false, 0);
}

static SDValue LowerVACOPY(SDValue Op, const X86Subtarget *Subtarget,
                           SelectionDAG &DAG) {
  // X86-64 va_list is a struct { i32, i32, i8*, i8* }.
  assert(Subtarget->is64Bit() && "This code only handles 64-bit va_copy!");
  SDValue Chain = Op.getOperand(0);
  SDValue DstPtr = Op.getOperand(1);
  SDValue SrcPtr = Op.getOperand(2);
  const Value *DstSV = cast<SrcValueSDNode>(Op.getOperand(3))->getValue();
  const Value *SrcSV = cast<SrcValueSDNode>(Op.getOperand(4))->getValue();
  SDLoc DL(Op);

  return DAG.getMemcpy(Chain, DL, DstPtr, SrcPtr,
                       DAG.getIntPtrConstant(24), 8, /*isVolatile*/false,
                       false,
                       MachinePointerInfo(DstSV), MachinePointerInfo(SrcSV));
}

// getTargetVShiftByConstNode - Handle vector element shifts where the shift
// amount is a constant. Takes immediate version of shift as input.
static SDValue getTargetVShiftByConstNode(unsigned Opc, SDLoc dl, MVT VT,
                                          SDValue SrcOp, uint64_t ShiftAmt,
                                          SelectionDAG &DAG) {
  MVT ElementType = VT.getVectorElementType();

  // Fold this packed shift into its first operand if ShiftAmt is 0.
  if (ShiftAmt == 0)
    return SrcOp;

  // Check for ShiftAmt >= element width
  if (ShiftAmt >= ElementType.getSizeInBits()) {
    if (Opc == X86ISD::VSRAI)
      ShiftAmt = ElementType.getSizeInBits() - 1;
    else
      return DAG.getConstant(0, VT);
  }

  assert((Opc == X86ISD::VSHLI || Opc == X86ISD::VSRLI || Opc == X86ISD::VSRAI)
         && "Unknown target vector shift-by-constant node");

  // Fold this packed vector shift into a build vector if SrcOp is a
  // vector of Constants or UNDEFs, and SrcOp valuetype is the same as VT.
  if (VT == SrcOp.getSimpleValueType() &&
      ISD::isBuildVectorOfConstantSDNodes(SrcOp.getNode())) {
    SmallVector<SDValue, 8> Elts;
    unsigned NumElts = SrcOp->getNumOperands();
    ConstantSDNode *ND;

    switch(Opc) {
    default: llvm_unreachable(nullptr);
    case X86ISD::VSHLI:
      for (unsigned i=0; i!=NumElts; ++i) {
        SDValue CurrentOp = SrcOp->getOperand(i);
        if (CurrentOp->getOpcode() == ISD::UNDEF) {
          Elts.push_back(CurrentOp);
          continue;
        }
        ND = cast<ConstantSDNode>(CurrentOp);
        const APInt &C = ND->getAPIntValue();
        Elts.push_back(DAG.getConstant(C.shl(ShiftAmt), ElementType));
      }
      break;
    case X86ISD::VSRLI:
      for (unsigned i=0; i!=NumElts; ++i) {
        SDValue CurrentOp = SrcOp->getOperand(i);
        if (CurrentOp->getOpcode() == ISD::UNDEF) {
          Elts.push_back(CurrentOp);
          continue;
        }
        ND = cast<ConstantSDNode>(CurrentOp);
        const APInt &C = ND->getAPIntValue();
        Elts.push_back(DAG.getConstant(C.lshr(ShiftAmt), ElementType));
      }
      break;
    case X86ISD::VSRAI:
      for (unsigned i=0; i!=NumElts; ++i) {
        SDValue CurrentOp = SrcOp->getOperand(i);
        if (CurrentOp->getOpcode() == ISD::UNDEF) {
          Elts.push_back(CurrentOp);
          continue;
        }
        ND = cast<ConstantSDNode>(CurrentOp);
        const APInt &C = ND->getAPIntValue();
        Elts.push_back(DAG.getConstant(C.ashr(ShiftAmt), ElementType));
      }
      break;
    }

    return DAG.getNode(ISD::BUILD_VECTOR, dl, VT, Elts);
  }

  return DAG.getNode(Opc, dl, VT, SrcOp, DAG.getConstant(ShiftAmt, MVT::i8));
}

// getTargetVShiftNode - Handle vector element shifts where the shift amount
// may or may not be a constant. Takes immediate version of shift as input.
static SDValue getTargetVShiftNode(unsigned Opc, SDLoc dl, MVT VT,
                                   SDValue SrcOp, SDValue ShAmt,
                                   SelectionDAG &DAG) {
  assert(ShAmt.getValueType() == MVT::i32 && "ShAmt is not i32");

  // Catch shift-by-constant.
  if (ConstantSDNode *CShAmt = dyn_cast<ConstantSDNode>(ShAmt))
    return getTargetVShiftByConstNode(Opc, dl, VT, SrcOp,
                                      CShAmt->getZExtValue(), DAG);

  // Change opcode to non-immediate version
  switch (Opc) {
    default: llvm_unreachable("Unknown target vector shift node");
    case X86ISD::VSHLI: Opc = X86ISD::VSHL; break;
    case X86ISD::VSRLI: Opc = X86ISD::VSRL; break;
    case X86ISD::VSRAI: Opc = X86ISD::VSRA; break;
  }

  // Need to build a vector containing shift amount
  // Shift amount is 32-bits, but SSE instructions read 64-bit, so fill with 0
  SDValue ShOps[4];
  ShOps[0] = ShAmt;
  ShOps[1] = DAG.getConstant(0, MVT::i32);
  ShOps[2] = ShOps[3] = DAG.getUNDEF(MVT::i32);
  ShAmt = DAG.getNode(ISD::BUILD_VECTOR, dl, MVT::v4i32, ShOps);

  // The return type has to be a 128-bit type with the same element
  // type as the input type.
  MVT EltVT = VT.getVectorElementType();
  EVT ShVT = MVT::getVectorVT(EltVT, 128/EltVT.getSizeInBits());

  ShAmt = DAG.getNode(ISD::BITCAST, dl, ShVT, ShAmt);
  return DAG.getNode(Opc, dl, VT, SrcOp, ShAmt);
}

/// \brief Return (vselect \p Mask, \p Op, \p PreservedSrc) along with the
/// necessary casting for \p Mask when lowering masking intrinsics.
static SDValue getVectorMaskingNode(SDValue Op, SDValue Mask,
                                    SDValue PreservedSrc, SelectionDAG &DAG) {
    EVT VT = Op.getValueType();
    EVT MaskVT = EVT::getVectorVT(*DAG.getContext(),
                                  MVT::i1, VT.getVectorNumElements());
    SDLoc dl(Op);

    assert(MaskVT.isSimple() && "invalid mask type");
    return DAG.getNode(ISD::VSELECT, dl, VT,
                       DAG.getNode(ISD::BITCAST, dl, MaskVT, Mask),
                       Op, PreservedSrc);
}

static unsigned getOpcodeForFMAIntrinsic(unsigned IntNo) {
    switch (IntNo) {
    default: llvm_unreachable("Impossible intrinsic");  // Can't reach here.
    case Intrinsic::x86_fma_vfmadd_ps:
    case Intrinsic::x86_fma_vfmadd_pd:
    case Intrinsic::x86_fma_vfmadd_ps_256:
    case Intrinsic::x86_fma_vfmadd_pd_256:
    case Intrinsic::x86_fma_mask_vfmadd_ps_512:
    case Intrinsic::x86_fma_mask_vfmadd_pd_512:
      return X86ISD::FMADD;
    case Intrinsic::x86_fma_vfmsub_ps:
    case Intrinsic::x86_fma_vfmsub_pd:
    case Intrinsic::x86_fma_vfmsub_ps_256:
    case Intrinsic::x86_fma_vfmsub_pd_256:
    case Intrinsic::x86_fma_mask_vfmsub_ps_512:
    case Intrinsic::x86_fma_mask_vfmsub_pd_512:
      return X86ISD::FMSUB;
    case Intrinsic::x86_fma_vfnmadd_ps:
    case Intrinsic::x86_fma_vfnmadd_pd:
    case Intrinsic::x86_fma_vfnmadd_ps_256:
    case Intrinsic::x86_fma_vfnmadd_pd_256:
    case Intrinsic::x86_fma_mask_vfnmadd_ps_512:
    case Intrinsic::x86_fma_mask_vfnmadd_pd_512:
      return X86ISD::FNMADD;
    case Intrinsic::x86_fma_vfnmsub_ps:
    case Intrinsic::x86_fma_vfnmsub_pd:
    case Intrinsic::x86_fma_vfnmsub_ps_256:
    case Intrinsic::x86_fma_vfnmsub_pd_256:
    case Intrinsic::x86_fma_mask_vfnmsub_ps_512:
    case Intrinsic::x86_fma_mask_vfnmsub_pd_512:
      return X86ISD::FNMSUB;
    case Intrinsic::x86_fma_vfmaddsub_ps:
    case Intrinsic::x86_fma_vfmaddsub_pd:
    case Intrinsic::x86_fma_vfmaddsub_ps_256:
    case Intrinsic::x86_fma_vfmaddsub_pd_256:
    case Intrinsic::x86_fma_mask_vfmaddsub_ps_512:
    case Intrinsic::x86_fma_mask_vfmaddsub_pd_512:
      return X86ISD::FMADDSUB;
    case Intrinsic::x86_fma_vfmsubadd_ps:
    case Intrinsic::x86_fma_vfmsubadd_pd:
    case Intrinsic::x86_fma_vfmsubadd_ps_256:
    case Intrinsic::x86_fma_vfmsubadd_pd_256:
    case Intrinsic::x86_fma_mask_vfmsubadd_ps_512:
    case Intrinsic::x86_fma_mask_vfmsubadd_pd_512:
      return X86ISD::FMSUBADD;
    }
}

static SDValue LowerINTRINSIC_WO_CHAIN(SDValue Op, SelectionDAG &DAG) {
  SDLoc dl(Op);
  unsigned IntNo = cast<ConstantSDNode>(Op.getOperand(0))->getZExtValue();

  const IntrinsicData* IntrData = GetIntrinsicWithoutChain(IntNo);
  if (IntrData) {
    switch(IntrData->Type) {
    case INTR_TYPE_1OP:
      return DAG.getNode(IntrData->Opc0, dl, Op.getValueType(), Op.getOperand(1));
    case INTR_TYPE_2OP:
      return DAG.getNode(IntrData->Opc0, dl, Op.getValueType(), Op.getOperand(1),
        Op.getOperand(2));
    case INTR_TYPE_3OP:
      return DAG.getNode(IntrData->Opc0, dl, Op.getValueType(), Op.getOperand(1),
        Op.getOperand(2), Op.getOperand(3));
    case COMI: { // Comparison intrinsics
      ISD::CondCode CC = (ISD::CondCode)IntrData->Opc1;
      SDValue LHS = Op.getOperand(1);
      SDValue RHS = Op.getOperand(2);
      unsigned X86CC = TranslateX86CC(CC, true, LHS, RHS, DAG);
      assert(X86CC != X86::COND_INVALID && "Unexpected illegal condition!");
      SDValue Cond = DAG.getNode(IntrData->Opc0, dl, MVT::i32, LHS, RHS);
      SDValue SetCC = DAG.getNode(X86ISD::SETCC, dl, MVT::i8,
                                  DAG.getConstant(X86CC, MVT::i8), Cond);
      return DAG.getNode(ISD::ZERO_EXTEND, dl, MVT::i32, SetCC);
    }
    case VSHIFT:
      return getTargetVShiftNode(IntrData->Opc0, dl, Op.getSimpleValueType(),
                                 Op.getOperand(1), Op.getOperand(2), DAG);
    default:
      break;
    }
  }

  switch (IntNo) {
  default: return SDValue();    // Don't custom lower most intrinsics.

  // Arithmetic intrinsics.
  case Intrinsic::x86_sse2_pmulu_dq:
  case Intrinsic::x86_avx2_pmulu_dq:
    return DAG.getNode(X86ISD::PMULUDQ, dl, Op.getValueType(),
                       Op.getOperand(1), Op.getOperand(2));

  case Intrinsic::x86_sse41_pmuldq:
  case Intrinsic::x86_avx2_pmul_dq:
    return DAG.getNode(X86ISD::PMULDQ, dl, Op.getValueType(),
                       Op.getOperand(1), Op.getOperand(2));

  case Intrinsic::x86_sse2_pmulhu_w:
  case Intrinsic::x86_avx2_pmulhu_w:
    return DAG.getNode(ISD::MULHU, dl, Op.getValueType(),
                       Op.getOperand(1), Op.getOperand(2));

  case Intrinsic::x86_sse2_pmulh_w:
  case Intrinsic::x86_avx2_pmulh_w:
    return DAG.getNode(ISD::MULHS, dl, Op.getValueType(),
                       Op.getOperand(1), Op.getOperand(2));

  // SSE/SSE2/AVX floating point max/min intrinsics.
  case Intrinsic::x86_sse_max_ps:
  case Intrinsic::x86_sse2_max_pd:
  case Intrinsic::x86_avx_max_ps_256:
  case Intrinsic::x86_avx_max_pd_256:
  case Intrinsic::x86_sse_min_ps:
  case Intrinsic::x86_sse2_min_pd:
  case Intrinsic::x86_avx_min_ps_256:
  case Intrinsic::x86_avx_min_pd_256: {
    unsigned Opcode;
    switch (IntNo) {
    default: llvm_unreachable("Impossible intrinsic");  // Can't reach here.
    case Intrinsic::x86_sse_max_ps:
    case Intrinsic::x86_sse2_max_pd:
    case Intrinsic::x86_avx_max_ps_256:
    case Intrinsic::x86_avx_max_pd_256:
      Opcode = X86ISD::FMAX;
      break;
    case Intrinsic::x86_sse_min_ps:
    case Intrinsic::x86_sse2_min_pd:
    case Intrinsic::x86_avx_min_ps_256:
    case Intrinsic::x86_avx_min_pd_256:
      Opcode = X86ISD::FMIN;
      break;
    }
    return DAG.getNode(Opcode, dl, Op.getValueType(),
                       Op.getOperand(1), Op.getOperand(2));
  }

  // AVX2 variable shift intrinsics
  case Intrinsic::x86_avx2_psllv_d:
  case Intrinsic::x86_avx2_psllv_q:
  case Intrinsic::x86_avx2_psllv_d_256:
  case Intrinsic::x86_avx2_psllv_q_256:
  case Intrinsic::x86_avx2_psrlv_d:
  case Intrinsic::x86_avx2_psrlv_q:
  case Intrinsic::x86_avx2_psrlv_d_256:
  case Intrinsic::x86_avx2_psrlv_q_256:
  case Intrinsic::x86_avx2_psrav_d:
  case Intrinsic::x86_avx2_psrav_d_256: {
    unsigned Opcode;
    switch (IntNo) {
    default: llvm_unreachable("Impossible intrinsic");  // Can't reach here.
    case Intrinsic::x86_avx2_psllv_d:
    case Intrinsic::x86_avx2_psllv_q:
    case Intrinsic::x86_avx2_psllv_d_256:
    case Intrinsic::x86_avx2_psllv_q_256:
      Opcode = ISD::SHL;
      break;
    case Intrinsic::x86_avx2_psrlv_d:
    case Intrinsic::x86_avx2_psrlv_q:
    case Intrinsic::x86_avx2_psrlv_d_256:
    case Intrinsic::x86_avx2_psrlv_q_256:
      Opcode = ISD::SRL;
      break;
    case Intrinsic::x86_avx2_psrav_d:
    case Intrinsic::x86_avx2_psrav_d_256:
      Opcode = ISD::SRA;
      break;
    }
    return DAG.getNode(Opcode, dl, Op.getValueType(),
                       Op.getOperand(1), Op.getOperand(2));
  }

  case Intrinsic::x86_sse2_packssdw_128:
  case Intrinsic::x86_sse2_packsswb_128:
  case Intrinsic::x86_avx2_packssdw:
  case Intrinsic::x86_avx2_packsswb:
    return DAG.getNode(X86ISD::PACKSS, dl, Op.getValueType(),
                       Op.getOperand(1), Op.getOperand(2));

  case Intrinsic::x86_sse2_packuswb_128:
  case Intrinsic::x86_sse41_packusdw:
  case Intrinsic::x86_avx2_packuswb:
  case Intrinsic::x86_avx2_packusdw:
    return DAG.getNode(X86ISD::PACKUS, dl, Op.getValueType(),
                       Op.getOperand(1), Op.getOperand(2));

  case Intrinsic::x86_ssse3_pshuf_b_128:
  case Intrinsic::x86_avx2_pshuf_b:
    return DAG.getNode(X86ISD::PSHUFB, dl, Op.getValueType(),
                       Op.getOperand(1), Op.getOperand(2));

  case Intrinsic::x86_sse2_pshuf_d:
    return DAG.getNode(X86ISD::PSHUFD, dl, Op.getValueType(),
                       Op.getOperand(1), Op.getOperand(2));

  case Intrinsic::x86_sse2_pshufl_w:
    return DAG.getNode(X86ISD::PSHUFLW, dl, Op.getValueType(),
                       Op.getOperand(1), Op.getOperand(2));

  case Intrinsic::x86_sse2_pshufh_w:
    return DAG.getNode(X86ISD::PSHUFHW, dl, Op.getValueType(),
                       Op.getOperand(1), Op.getOperand(2));

  case Intrinsic::x86_ssse3_psign_b_128:
  case Intrinsic::x86_ssse3_psign_w_128:
  case Intrinsic::x86_ssse3_psign_d_128:
  case Intrinsic::x86_avx2_psign_b:
  case Intrinsic::x86_avx2_psign_w:
  case Intrinsic::x86_avx2_psign_d:
    return DAG.getNode(X86ISD::PSIGN, dl, Op.getValueType(),
                       Op.getOperand(1), Op.getOperand(2));

  case Intrinsic::x86_avx2_permd:
  case Intrinsic::x86_avx2_permps:
    // Operands intentionally swapped. Mask is last operand to intrinsic,
    // but second operand for node/instruction.
    return DAG.getNode(X86ISD::VPERMV, dl, Op.getValueType(),
                       Op.getOperand(2), Op.getOperand(1));

  case Intrinsic::x86_avx512_mask_valign_q_512:
  case Intrinsic::x86_avx512_mask_valign_d_512:
    // Vector source operands are swapped.
    return getVectorMaskingNode(DAG.getNode(X86ISD::VALIGN, dl,
                                            Op.getValueType(), Op.getOperand(2),
                                            Op.getOperand(1),
                                            Op.getOperand(3)),
                                Op.getOperand(5), Op.getOperand(4), DAG);

  // ptest and testp intrinsics. The intrinsic these come from are designed to
  // return an integer value, not just an instruction so lower it to the ptest
  // or testp pattern and a setcc for the result.
  case Intrinsic::x86_sse41_ptestz:
  case Intrinsic::x86_sse41_ptestc:
  case Intrinsic::x86_sse41_ptestnzc:
  case Intrinsic::x86_avx_ptestz_256:
  case Intrinsic::x86_avx_ptestc_256:
  case Intrinsic::x86_avx_ptestnzc_256:
  case Intrinsic::x86_avx_vtestz_ps:
  case Intrinsic::x86_avx_vtestc_ps:
  case Intrinsic::x86_avx_vtestnzc_ps:
  case Intrinsic::x86_avx_vtestz_pd:
  case Intrinsic::x86_avx_vtestc_pd:
  case Intrinsic::x86_avx_vtestnzc_pd:
  case Intrinsic::x86_avx_vtestz_ps_256:
  case Intrinsic::x86_avx_vtestc_ps_256:
  case Intrinsic::x86_avx_vtestnzc_ps_256:
  case Intrinsic::x86_avx_vtestz_pd_256:
  case Intrinsic::x86_avx_vtestc_pd_256:
  case Intrinsic::x86_avx_vtestnzc_pd_256: {
    bool IsTestPacked = false;
    unsigned X86CC;
    switch (IntNo) {
    default: llvm_unreachable("Bad fallthrough in Intrinsic lowering.");
    case Intrinsic::x86_avx_vtestz_ps:
    case Intrinsic::x86_avx_vtestz_pd:
    case Intrinsic::x86_avx_vtestz_ps_256:
    case Intrinsic::x86_avx_vtestz_pd_256:
      IsTestPacked = true; // Fallthrough
    case Intrinsic::x86_sse41_ptestz:
    case Intrinsic::x86_avx_ptestz_256:
      // ZF = 1
      X86CC = X86::COND_E;
      break;
    case Intrinsic::x86_avx_vtestc_ps:
    case Intrinsic::x86_avx_vtestc_pd:
    case Intrinsic::x86_avx_vtestc_ps_256:
    case Intrinsic::x86_avx_vtestc_pd_256:
      IsTestPacked = true; // Fallthrough
    case Intrinsic::x86_sse41_ptestc:
    case Intrinsic::x86_avx_ptestc_256:
      // CF = 1
      X86CC = X86::COND_B;
      break;
    case Intrinsic::x86_avx_vtestnzc_ps:
    case Intrinsic::x86_avx_vtestnzc_pd:
    case Intrinsic::x86_avx_vtestnzc_ps_256:
    case Intrinsic::x86_avx_vtestnzc_pd_256:
      IsTestPacked = true; // Fallthrough
    case Intrinsic::x86_sse41_ptestnzc:
    case Intrinsic::x86_avx_ptestnzc_256:
      // ZF and CF = 0
      X86CC = X86::COND_A;
      break;
    }

    SDValue LHS = Op.getOperand(1);
    SDValue RHS = Op.getOperand(2);
    unsigned TestOpc = IsTestPacked ? X86ISD::TESTP : X86ISD::PTEST;
    SDValue Test = DAG.getNode(TestOpc, dl, MVT::i32, LHS, RHS);
    SDValue CC = DAG.getConstant(X86CC, MVT::i8);
    SDValue SetCC = DAG.getNode(X86ISD::SETCC, dl, MVT::i8, CC, Test);
    return DAG.getNode(ISD::ZERO_EXTEND, dl, MVT::i32, SetCC);
  }
  case Intrinsic::x86_avx512_kortestz_w:
  case Intrinsic::x86_avx512_kortestc_w: {
    unsigned X86CC = (IntNo == Intrinsic::x86_avx512_kortestz_w)? X86::COND_E: X86::COND_B;
    SDValue LHS = DAG.getNode(ISD::BITCAST, dl, MVT::v16i1, Op.getOperand(1));
    SDValue RHS = DAG.getNode(ISD::BITCAST, dl, MVT::v16i1, Op.getOperand(2));
    SDValue CC = DAG.getConstant(X86CC, MVT::i8);
    SDValue Test = DAG.getNode(X86ISD::KORTEST, dl, MVT::i32, LHS, RHS);
    SDValue SetCC = DAG.getNode(X86ISD::SETCC, dl, MVT::i1, CC, Test);
    return DAG.getNode(ISD::ZERO_EXTEND, dl, MVT::i32, SetCC);
  }

  case Intrinsic::x86_sse42_pcmpistria128:
  case Intrinsic::x86_sse42_pcmpestria128:
  case Intrinsic::x86_sse42_pcmpistric128:
  case Intrinsic::x86_sse42_pcmpestric128:
  case Intrinsic::x86_sse42_pcmpistrio128:
  case Intrinsic::x86_sse42_pcmpestrio128:
  case Intrinsic::x86_sse42_pcmpistris128:
  case Intrinsic::x86_sse42_pcmpestris128:
  case Intrinsic::x86_sse42_pcmpistriz128:
  case Intrinsic::x86_sse42_pcmpestriz128: {
    unsigned Opcode;
    unsigned X86CC;
    switch (IntNo) {
    default: llvm_unreachable("Impossible intrinsic");  // Can't reach here.
    case Intrinsic::x86_sse42_pcmpistria128:
      Opcode = X86ISD::PCMPISTRI;
      X86CC = X86::COND_A;
      break;
    case Intrinsic::x86_sse42_pcmpestria128:
      Opcode = X86ISD::PCMPESTRI;
      X86CC = X86::COND_A;
      break;
    case Intrinsic::x86_sse42_pcmpistric128:
      Opcode = X86ISD::PCMPISTRI;
      X86CC = X86::COND_B;
      break;
    case Intrinsic::x86_sse42_pcmpestric128:
      Opcode = X86ISD::PCMPESTRI;
      X86CC = X86::COND_B;
      break;
    case Intrinsic::x86_sse42_pcmpistrio128:
      Opcode = X86ISD::PCMPISTRI;
      X86CC = X86::COND_O;
      break;
    case Intrinsic::x86_sse42_pcmpestrio128:
      Opcode = X86ISD::PCMPESTRI;
      X86CC = X86::COND_O;
      break;
    case Intrinsic::x86_sse42_pcmpistris128:
      Opcode = X86ISD::PCMPISTRI;
      X86CC = X86::COND_S;
      break;
    case Intrinsic::x86_sse42_pcmpestris128:
      Opcode = X86ISD::PCMPESTRI;
      X86CC = X86::COND_S;
      break;
    case Intrinsic::x86_sse42_pcmpistriz128:
      Opcode = X86ISD::PCMPISTRI;
      X86CC = X86::COND_E;
      break;
    case Intrinsic::x86_sse42_pcmpestriz128:
      Opcode = X86ISD::PCMPESTRI;
      X86CC = X86::COND_E;
      break;
    }
    SmallVector<SDValue, 5> NewOps(Op->op_begin()+1, Op->op_end());
    SDVTList VTs = DAG.getVTList(Op.getValueType(), MVT::i32);
    SDValue PCMP = DAG.getNode(Opcode, dl, VTs, NewOps);
    SDValue SetCC = DAG.getNode(X86ISD::SETCC, dl, MVT::i8,
                                DAG.getConstant(X86CC, MVT::i8),
                                SDValue(PCMP.getNode(), 1));
    return DAG.getNode(ISD::ZERO_EXTEND, dl, MVT::i32, SetCC);
  }

  case Intrinsic::x86_sse42_pcmpistri128:
  case Intrinsic::x86_sse42_pcmpestri128: {
    unsigned Opcode;
    if (IntNo == Intrinsic::x86_sse42_pcmpistri128)
      Opcode = X86ISD::PCMPISTRI;
    else
      Opcode = X86ISD::PCMPESTRI;

    SmallVector<SDValue, 5> NewOps(Op->op_begin()+1, Op->op_end());
    SDVTList VTs = DAG.getVTList(Op.getValueType(), MVT::i32);
    return DAG.getNode(Opcode, dl, VTs, NewOps);
  }

  case Intrinsic::x86_fma_mask_vfmadd_ps_512:
  case Intrinsic::x86_fma_mask_vfmadd_pd_512:
  case Intrinsic::x86_fma_mask_vfmsub_ps_512:
  case Intrinsic::x86_fma_mask_vfmsub_pd_512:
  case Intrinsic::x86_fma_mask_vfnmadd_ps_512:
  case Intrinsic::x86_fma_mask_vfnmadd_pd_512:
  case Intrinsic::x86_fma_mask_vfnmsub_ps_512:
  case Intrinsic::x86_fma_mask_vfnmsub_pd_512:
  case Intrinsic::x86_fma_mask_vfmaddsub_ps_512:
  case Intrinsic::x86_fma_mask_vfmaddsub_pd_512:
  case Intrinsic::x86_fma_mask_vfmsubadd_ps_512:
  case Intrinsic::x86_fma_mask_vfmsubadd_pd_512: {
    auto *SAE = cast<ConstantSDNode>(Op.getOperand(5));
    if (SAE->getZExtValue() == X86::STATIC_ROUNDING::CUR_DIRECTION)
      return getVectorMaskingNode(DAG.getNode(getOpcodeForFMAIntrinsic(IntNo),
                                              dl, Op.getValueType(),
                                              Op.getOperand(1),
                                              Op.getOperand(2),
                                              Op.getOperand(3)),
                                  Op.getOperand(4), Op.getOperand(1), DAG);
    else
      return SDValue();
  }

  case Intrinsic::x86_fma_vfmadd_ps:
  case Intrinsic::x86_fma_vfmadd_pd:
  case Intrinsic::x86_fma_vfmsub_ps:
  case Intrinsic::x86_fma_vfmsub_pd:
  case Intrinsic::x86_fma_vfnmadd_ps:
  case Intrinsic::x86_fma_vfnmadd_pd:
  case Intrinsic::x86_fma_vfnmsub_ps:
  case Intrinsic::x86_fma_vfnmsub_pd:
  case Intrinsic::x86_fma_vfmaddsub_ps:
  case Intrinsic::x86_fma_vfmaddsub_pd:
  case Intrinsic::x86_fma_vfmsubadd_ps:
  case Intrinsic::x86_fma_vfmsubadd_pd:
  case Intrinsic::x86_fma_vfmadd_ps_256:
  case Intrinsic::x86_fma_vfmadd_pd_256:
  case Intrinsic::x86_fma_vfmsub_ps_256:
  case Intrinsic::x86_fma_vfmsub_pd_256:
  case Intrinsic::x86_fma_vfnmadd_ps_256:
  case Intrinsic::x86_fma_vfnmadd_pd_256:
  case Intrinsic::x86_fma_vfnmsub_ps_256:
  case Intrinsic::x86_fma_vfnmsub_pd_256:
  case Intrinsic::x86_fma_vfmaddsub_ps_256:
  case Intrinsic::x86_fma_vfmaddsub_pd_256:
  case Intrinsic::x86_fma_vfmsubadd_ps_256:
  case Intrinsic::x86_fma_vfmsubadd_pd_256:
    return DAG.getNode(getOpcodeForFMAIntrinsic(IntNo), dl, Op.getValueType(),
                       Op.getOperand(1), Op.getOperand(2), Op.getOperand(3));
  }
}

static SDValue getGatherNode(unsigned Opc, SDValue Op, SelectionDAG &DAG,
                              SDValue Src, SDValue Mask, SDValue Base,
                              SDValue Index, SDValue ScaleOp, SDValue Chain,
                              const X86Subtarget * Subtarget) {
  SDLoc dl(Op);
  ConstantSDNode *C = dyn_cast<ConstantSDNode>(ScaleOp);
  assert(C && "Invalid scale type");
  SDValue Scale = DAG.getTargetConstant(C->getZExtValue(), MVT::i8);
  EVT MaskVT = MVT::getVectorVT(MVT::i1,
                             Index.getSimpleValueType().getVectorNumElements());
  SDValue MaskInReg;
  ConstantSDNode *MaskC = dyn_cast<ConstantSDNode>(Mask);
  if (MaskC)
    MaskInReg = DAG.getTargetConstant(MaskC->getSExtValue(), MaskVT);
  else
    MaskInReg = DAG.getNode(ISD::BITCAST, dl, MaskVT, Mask);
  SDVTList VTs = DAG.getVTList(Op.getValueType(), MaskVT, MVT::Other);
  SDValue Disp = DAG.getTargetConstant(0, MVT::i32);
  SDValue Segment = DAG.getRegister(0, MVT::i32);
  if (Src.getOpcode() == ISD::UNDEF)
    Src = getZeroVector(Op.getValueType(), Subtarget, DAG, dl);
  SDValue Ops[] = {Src, MaskInReg, Base, Scale, Index, Disp, Segment, Chain};
  SDNode *Res = DAG.getMachineNode(Opc, dl, VTs, Ops);
  SDValue RetOps[] = { SDValue(Res, 0), SDValue(Res, 2) };
  return DAG.getMergeValues(RetOps, dl);
}

static SDValue getScatterNode(unsigned Opc, SDValue Op, SelectionDAG &DAG,
                               SDValue Src, SDValue Mask, SDValue Base,
                               SDValue Index, SDValue ScaleOp, SDValue Chain) {
  SDLoc dl(Op);
  ConstantSDNode *C = dyn_cast<ConstantSDNode>(ScaleOp);
  assert(C && "Invalid scale type");
  SDValue Scale = DAG.getTargetConstant(C->getZExtValue(), MVT::i8);
  SDValue Disp = DAG.getTargetConstant(0, MVT::i32);
  SDValue Segment = DAG.getRegister(0, MVT::i32);
  EVT MaskVT = MVT::getVectorVT(MVT::i1,
                             Index.getSimpleValueType().getVectorNumElements());
  SDValue MaskInReg;
  ConstantSDNode *MaskC = dyn_cast<ConstantSDNode>(Mask);
  if (MaskC)
    MaskInReg = DAG.getTargetConstant(MaskC->getSExtValue(), MaskVT);
  else
    MaskInReg = DAG.getNode(ISD::BITCAST, dl, MaskVT, Mask);
  SDVTList VTs = DAG.getVTList(MaskVT, MVT::Other);
  SDValue Ops[] = {Base, Scale, Index, Disp, Segment, MaskInReg, Src, Chain};
  SDNode *Res = DAG.getMachineNode(Opc, dl, VTs, Ops);
  return SDValue(Res, 1);
}

static SDValue getPrefetchNode(unsigned Opc, SDValue Op, SelectionDAG &DAG,
                               SDValue Mask, SDValue Base, SDValue Index,
                               SDValue ScaleOp, SDValue Chain) {
  SDLoc dl(Op);
  ConstantSDNode *C = dyn_cast<ConstantSDNode>(ScaleOp);
  assert(C && "Invalid scale type");
  SDValue Scale = DAG.getTargetConstant(C->getZExtValue(), MVT::i8);
  SDValue Disp = DAG.getTargetConstant(0, MVT::i32);
  SDValue Segment = DAG.getRegister(0, MVT::i32);
  EVT MaskVT =
    MVT::getVectorVT(MVT::i1, Index.getSimpleValueType().getVectorNumElements());
  SDValue MaskInReg;
  ConstantSDNode *MaskC = dyn_cast<ConstantSDNode>(Mask);
  if (MaskC)
    MaskInReg = DAG.getTargetConstant(MaskC->getSExtValue(), MaskVT);
  else
    MaskInReg = DAG.getNode(ISD::BITCAST, dl, MaskVT, Mask);
  //SDVTList VTs = DAG.getVTList(MVT::Other);
  SDValue Ops[] = {MaskInReg, Base, Scale, Index, Disp, Segment, Chain};
  SDNode *Res = DAG.getMachineNode(Opc, dl, MVT::Other, Ops);
  return SDValue(Res, 0);
}

// getReadPerformanceCounter - Handles the lowering of builtin intrinsics that
// read performance monitor counters (x86_rdpmc).
static void getReadPerformanceCounter(SDNode *N, SDLoc DL,
                              SelectionDAG &DAG, const X86Subtarget *Subtarget,
                              SmallVectorImpl<SDValue> &Results) {
  assert(N->getNumOperands() == 3 && "Unexpected number of operands!");
  SDVTList Tys = DAG.getVTList(MVT::Other, MVT::Glue);
  SDValue LO, HI;

  // The ECX register is used to select the index of the performance counter
  // to read.
  SDValue Chain = DAG.getCopyToReg(N->getOperand(0), DL, X86::ECX,
                                   N->getOperand(2));
  SDValue rd = DAG.getNode(X86ISD::RDPMC_DAG, DL, Tys, Chain);

  // Reads the content of a 64-bit performance counter and returns it in the
  // registers EDX:EAX.
  if (Subtarget->is64Bit()) {
    LO = DAG.getCopyFromReg(rd, DL, X86::RAX, MVT::i64, rd.getValue(1));
    HI = DAG.getCopyFromReg(LO.getValue(1), DL, X86::RDX, MVT::i64,
                            LO.getValue(2));
  } else {
    LO = DAG.getCopyFromReg(rd, DL, X86::EAX, MVT::i32, rd.getValue(1));
    HI = DAG.getCopyFromReg(LO.getValue(1), DL, X86::EDX, MVT::i32,
                            LO.getValue(2));
  }
  Chain = HI.getValue(1);

  if (Subtarget->is64Bit()) {
    // The EAX register is loaded with the low-order 32 bits. The EDX register
    // is loaded with the supported high-order bits of the counter.
    SDValue Tmp = DAG.getNode(ISD::SHL, DL, MVT::i64, HI,
                              DAG.getConstant(32, MVT::i8));
    Results.push_back(DAG.getNode(ISD::OR, DL, MVT::i64, LO, Tmp));
    Results.push_back(Chain);
    return;
  }

  // Use a buildpair to merge the two 32-bit values into a 64-bit one.
  SDValue Ops[] = { LO, HI };
  SDValue Pair = DAG.getNode(ISD::BUILD_PAIR, DL, MVT::i64, Ops);
  Results.push_back(Pair);
  Results.push_back(Chain);
}

// getReadTimeStampCounter - Handles the lowering of builtin intrinsics that
// read the time stamp counter (x86_rdtsc and x86_rdtscp). This function is
// also used to custom lower READCYCLECOUNTER nodes.
static void getReadTimeStampCounter(SDNode *N, SDLoc DL, unsigned Opcode,
                              SelectionDAG &DAG, const X86Subtarget *Subtarget,
                              SmallVectorImpl<SDValue> &Results) {
  SDVTList Tys = DAG.getVTList(MVT::Other, MVT::Glue);
  SDValue rd = DAG.getNode(Opcode, DL, Tys, N->getOperand(0));
  SDValue LO, HI;

  // The processor's time-stamp counter (a 64-bit MSR) is stored into the
  // EDX:EAX registers. EDX is loaded with the high-order 32 bits of the MSR
  // and the EAX register is loaded with the low-order 32 bits.
  if (Subtarget->is64Bit()) {
    LO = DAG.getCopyFromReg(rd, DL, X86::RAX, MVT::i64, rd.getValue(1));
    HI = DAG.getCopyFromReg(LO.getValue(1), DL, X86::RDX, MVT::i64,
                            LO.getValue(2));
  } else {
    LO = DAG.getCopyFromReg(rd, DL, X86::EAX, MVT::i32, rd.getValue(1));
    HI = DAG.getCopyFromReg(LO.getValue(1), DL, X86::EDX, MVT::i32,
                            LO.getValue(2));
  }
  SDValue Chain = HI.getValue(1);

  if (Opcode == X86ISD::RDTSCP_DAG) {
    assert(N->getNumOperands() == 3 && "Unexpected number of operands!");

    // Instruction RDTSCP loads the IA32:TSC_AUX_MSR (address C000_0103H) into
    // the ECX register. Add 'ecx' explicitly to the chain.
    SDValue ecx = DAG.getCopyFromReg(Chain, DL, X86::ECX, MVT::i32,
                                     HI.getValue(2));
    // Explicitly store the content of ECX at the location passed in input
    // to the 'rdtscp' intrinsic.
    Chain = DAG.getStore(ecx.getValue(1), DL, ecx, N->getOperand(2),
                         MachinePointerInfo(), false, false, 0);
  }

  if (Subtarget->is64Bit()) {
    // The EDX register is loaded with the high-order 32 bits of the MSR, and
    // the EAX register is loaded with the low-order 32 bits.
    SDValue Tmp = DAG.getNode(ISD::SHL, DL, MVT::i64, HI,
                              DAG.getConstant(32, MVT::i8));
    Results.push_back(DAG.getNode(ISD::OR, DL, MVT::i64, LO, Tmp));
    Results.push_back(Chain);
    return;
  }

  // Use a buildpair to merge the two 32-bit values into a 64-bit one.
  SDValue Ops[] = { LO, HI };
  SDValue Pair = DAG.getNode(ISD::BUILD_PAIR, DL, MVT::i64, Ops);
  Results.push_back(Pair);
  Results.push_back(Chain);
}

static SDValue LowerREADCYCLECOUNTER(SDValue Op, const X86Subtarget *Subtarget,
                                     SelectionDAG &DAG) {
  SmallVector<SDValue, 2> Results;
  SDLoc DL(Op);
  getReadTimeStampCounter(Op.getNode(), DL, X86ISD::RDTSC_DAG, DAG, Subtarget,
                          Results);
  return DAG.getMergeValues(Results, DL);
}


static SDValue LowerINTRINSIC_W_CHAIN(SDValue Op, const X86Subtarget *Subtarget,
                                      SelectionDAG &DAG) {
  unsigned IntNo = cast<ConstantSDNode>(Op.getOperand(1))->getZExtValue();

  const IntrinsicData* IntrData = GetIntrinsicWithChain(IntNo);
  if (!IntrData)
    return SDValue();

  SDLoc dl(Op);
  switch(IntrData->Type) {
  default:
    llvm_unreachable("Unknown Intrinsic Type");
    break;    
  case RDSEED:
  case RDRAND: {
    // Emit the node with the right value type.
    SDVTList VTs = DAG.getVTList(Op->getValueType(0), MVT::Glue, MVT::Other);
    SDValue Result = DAG.getNode(IntrData->Opc0, dl, VTs, Op.getOperand(0));

    // If the value returned by RDRAND/RDSEED was valid (CF=1), return 1.
    // Otherwise return the value from Rand, which is always 0, casted to i32.
    SDValue Ops[] = { DAG.getZExtOrTrunc(Result, dl, Op->getValueType(1)),
                      DAG.getConstant(1, Op->getValueType(1)),
                      DAG.getConstant(X86::COND_B, MVT::i32),
                      SDValue(Result.getNode(), 1) };
    SDValue isValid = DAG.getNode(X86ISD::CMOV, dl,
                                  DAG.getVTList(Op->getValueType(1), MVT::Glue),
                                  Ops);

    // Return { result, isValid, chain }.
    return DAG.getNode(ISD::MERGE_VALUES, dl, Op->getVTList(), Result, isValid,
                       SDValue(Result.getNode(), 2));
  }
  case GATHER: {
  //gather(v1, mask, index, base, scale);
    SDValue Chain = Op.getOperand(0);
    SDValue Src   = Op.getOperand(2);
    SDValue Base  = Op.getOperand(3);
    SDValue Index = Op.getOperand(4);
    SDValue Mask  = Op.getOperand(5);
    SDValue Scale = Op.getOperand(6);
    return getGatherNode(IntrData->Opc0, Op, DAG, Src, Mask, Base, Index, Scale, Chain,
                          Subtarget);
  }
  case SCATTER: {
  //scatter(base, mask, index, v1, scale);
    SDValue Chain = Op.getOperand(0);
    SDValue Base  = Op.getOperand(2);
    SDValue Mask  = Op.getOperand(3);
    SDValue Index = Op.getOperand(4);
    SDValue Src   = Op.getOperand(5);
    SDValue Scale = Op.getOperand(6);
    return getScatterNode(IntrData->Opc0, Op, DAG, Src, Mask, Base, Index, Scale, Chain);
  }
  case PREFETCH: {
    SDValue Hint = Op.getOperand(6);
    unsigned HintVal;
    if (dyn_cast<ConstantSDNode> (Hint) == nullptr ||
        (HintVal = dyn_cast<ConstantSDNode> (Hint)->getZExtValue()) > 1)
      llvm_unreachable("Wrong prefetch hint in intrinsic: should be 0 or 1");
    unsigned Opcode = (HintVal ? IntrData->Opc1 : IntrData->Opc0);
    SDValue Chain = Op.getOperand(0);
    SDValue Mask  = Op.getOperand(2);
    SDValue Index = Op.getOperand(3);
    SDValue Base  = Op.getOperand(4);
    SDValue Scale = Op.getOperand(5);
    return getPrefetchNode(Opcode, Op, DAG, Mask, Base, Index, Scale, Chain);
  }
  // Read Time Stamp Counter (RDTSC) and Processor ID (RDTSCP).
  case RDTSC: {
    SmallVector<SDValue, 2> Results;
    getReadTimeStampCounter(Op.getNode(), dl, IntrData->Opc0, DAG, Subtarget, Results);
    return DAG.getMergeValues(Results, dl);
  }
  // Read Performance Monitoring Counters.
  case RDPMC: {
    SmallVector<SDValue, 2> Results;
    getReadPerformanceCounter(Op.getNode(), dl, DAG, Subtarget, Results);
    return DAG.getMergeValues(Results, dl);
  }
  // XTEST intrinsics.
  case XTEST: {
    SDVTList VTs = DAG.getVTList(Op->getValueType(0), MVT::Other);
    SDValue InTrans = DAG.getNode(IntrData->Opc0, dl, VTs, Op.getOperand(0));
    SDValue SetCC = DAG.getNode(X86ISD::SETCC, dl, MVT::i8,
                                DAG.getConstant(X86::COND_NE, MVT::i8),
                                InTrans);
    SDValue Ret = DAG.getNode(ISD::ZERO_EXTEND, dl, Op->getValueType(0), SetCC);
    return DAG.getNode(ISD::MERGE_VALUES, dl, Op->getVTList(),
                       Ret, SDValue(InTrans.getNode(), 1));
  }
  // ADC/ADCX/SBB
  case ADX: {
    SmallVector<SDValue, 2> Results;
    SDVTList CFVTs = DAG.getVTList(Op->getValueType(0), MVT::Other);
    SDVTList VTs = DAG.getVTList(Op.getOperand(3)->getValueType(0), MVT::Other);
    SDValue GenCF = DAG.getNode(X86ISD::ADD, dl, CFVTs, Op.getOperand(2),
                                DAG.getConstant(-1, MVT::i8));
    SDValue Res = DAG.getNode(IntrData->Opc0, dl, VTs, Op.getOperand(3),
                              Op.getOperand(4), GenCF.getValue(1));
    SDValue Store = DAG.getStore(Op.getOperand(0), dl, Res.getValue(0),
                                 Op.getOperand(5), MachinePointerInfo(),
                                 false, false, 0);
    SDValue SetCC = DAG.getNode(X86ISD::SETCC, dl, MVT::i8,
                                DAG.getConstant(X86::COND_B, MVT::i8),
                                Res.getValue(1));
    Results.push_back(SetCC);
    Results.push_back(Store);
    return DAG.getMergeValues(Results, dl);
  }
  }
}

SDValue X86TargetLowering::LowerRETURNADDR(SDValue Op,
                                           SelectionDAG &DAG) const {
  MachineFrameInfo *MFI = DAG.getMachineFunction().getFrameInfo();
  MFI->setReturnAddressIsTaken(true);

  if (verifyReturnAddressArgumentIsConstant(Op, DAG))
    return SDValue();

  unsigned Depth = cast<ConstantSDNode>(Op.getOperand(0))->getZExtValue();
  SDLoc dl(Op);
  EVT PtrVT = getPointerTy();

  if (Depth > 0) {
    SDValue FrameAddr = LowerFRAMEADDR(Op, DAG);
    const X86RegisterInfo *RegInfo = static_cast<const X86RegisterInfo *>(
        DAG.getSubtarget().getRegisterInfo());
    SDValue Offset = DAG.getConstant(RegInfo->getSlotSize(), PtrVT);
    return DAG.getLoad(PtrVT, dl, DAG.getEntryNode(),
                       DAG.getNode(ISD::ADD, dl, PtrVT,
                                   FrameAddr, Offset),
                       MachinePointerInfo(), false, false, false, 0);
  }

  // Just load the return address.
  SDValue RetAddrFI = getReturnAddressFrameIndex(DAG);
  return DAG.getLoad(PtrVT, dl, DAG.getEntryNode(),
                     RetAddrFI, MachinePointerInfo(), false, false, false, 0);
}

SDValue X86TargetLowering::LowerFRAMEADDR(SDValue Op, SelectionDAG &DAG) const {
  MachineFrameInfo *MFI = DAG.getMachineFunction().getFrameInfo();
  MFI->setFrameAddressIsTaken(true);

  EVT VT = Op.getValueType();
  SDLoc dl(Op);  // FIXME probably not meaningful
  unsigned Depth = cast<ConstantSDNode>(Op.getOperand(0))->getZExtValue();
  const X86RegisterInfo *RegInfo = static_cast<const X86RegisterInfo *>(
      DAG.getSubtarget().getRegisterInfo());
  unsigned FrameReg = RegInfo->getFrameRegister(DAG.getMachineFunction());
  assert(((FrameReg == X86::RBP && VT == MVT::i64) ||
          (FrameReg == X86::EBP && VT == MVT::i32)) &&
         "Invalid Frame Register!");
  SDValue FrameAddr = DAG.getCopyFromReg(DAG.getEntryNode(), dl, FrameReg, VT);
  while (Depth--)
    FrameAddr = DAG.getLoad(VT, dl, DAG.getEntryNode(), FrameAddr,
                            MachinePointerInfo(),
                            false, false, false, 0);
  return FrameAddr;
}

// FIXME? Maybe this could be a TableGen attribute on some registers and
// this table could be generated automatically from RegInfo.
unsigned X86TargetLowering::getRegisterByName(const char* RegName,
                                              EVT VT) const {
  unsigned Reg = StringSwitch<unsigned>(RegName)
                       .Case("esp", X86::ESP)
                       .Case("rsp", X86::RSP)
                       .Default(0);
  if (Reg)
    return Reg;
  report_fatal_error("Invalid register name global variable");
}

SDValue X86TargetLowering::LowerFRAME_TO_ARGS_OFFSET(SDValue Op,
                                                     SelectionDAG &DAG) const {
  const X86RegisterInfo *RegInfo = static_cast<const X86RegisterInfo *>(
      DAG.getSubtarget().getRegisterInfo());
  return DAG.getIntPtrConstant(2 * RegInfo->getSlotSize());
}

SDValue X86TargetLowering::LowerEH_RETURN(SDValue Op, SelectionDAG &DAG) const {
  SDValue Chain     = Op.getOperand(0);
  SDValue Offset    = Op.getOperand(1);
  SDValue Handler   = Op.getOperand(2);
  SDLoc dl      (Op);

  EVT PtrVT = getPointerTy();
  const X86RegisterInfo *RegInfo = static_cast<const X86RegisterInfo *>(
      DAG.getSubtarget().getRegisterInfo());
  unsigned FrameReg = RegInfo->getFrameRegister(DAG.getMachineFunction());
  assert(((FrameReg == X86::RBP && PtrVT == MVT::i64) ||
          (FrameReg == X86::EBP && PtrVT == MVT::i32)) &&
         "Invalid Frame Register!");
  SDValue Frame = DAG.getCopyFromReg(DAG.getEntryNode(), dl, FrameReg, PtrVT);
  unsigned StoreAddrReg = (PtrVT == MVT::i64) ? X86::RCX : X86::ECX;

  SDValue StoreAddr = DAG.getNode(ISD::ADD, dl, PtrVT, Frame,
                                 DAG.getIntPtrConstant(RegInfo->getSlotSize()));
  StoreAddr = DAG.getNode(ISD::ADD, dl, PtrVT, StoreAddr, Offset);
  Chain = DAG.getStore(Chain, dl, Handler, StoreAddr, MachinePointerInfo(),
                       false, false, 0);
  Chain = DAG.getCopyToReg(Chain, dl, StoreAddrReg, StoreAddr);

  return DAG.getNode(X86ISD::EH_RETURN, dl, MVT::Other, Chain,
                     DAG.getRegister(StoreAddrReg, PtrVT));
}

SDValue X86TargetLowering::lowerEH_SJLJ_SETJMP(SDValue Op,
                                               SelectionDAG &DAG) const {
  SDLoc DL(Op);
  return DAG.getNode(X86ISD::EH_SJLJ_SETJMP, DL,
                     DAG.getVTList(MVT::i32, MVT::Other),
                     Op.getOperand(0), Op.getOperand(1));
}

SDValue X86TargetLowering::lowerEH_SJLJ_LONGJMP(SDValue Op,
                                                SelectionDAG &DAG) const {
  SDLoc DL(Op);
  return DAG.getNode(X86ISD::EH_SJLJ_LONGJMP, DL, MVT::Other,
                     Op.getOperand(0), Op.getOperand(1));
}

static SDValue LowerADJUST_TRAMPOLINE(SDValue Op, SelectionDAG &DAG) {
  return Op.getOperand(0);
}

SDValue X86TargetLowering::LowerINIT_TRAMPOLINE(SDValue Op,
                                                SelectionDAG &DAG) const {
  SDValue Root = Op.getOperand(0);
  SDValue Trmp = Op.getOperand(1); // trampoline
  SDValue FPtr = Op.getOperand(2); // nested function
  SDValue Nest = Op.getOperand(3); // 'nest' parameter value
  SDLoc dl (Op);

  const Value *TrmpAddr = cast<SrcValueSDNode>(Op.getOperand(4))->getValue();
  const TargetRegisterInfo *TRI = DAG.getSubtarget().getRegisterInfo();

  if (Subtarget->is64Bit()) {
    SDValue OutChains[6];

    // Large code-model.
    const unsigned char JMP64r  = 0xFF; // 64-bit jmp through register opcode.
    const unsigned char MOV64ri = 0xB8; // X86::MOV64ri opcode.

    const unsigned char N86R10 = TRI->getEncodingValue(X86::R10) & 0x7;
    const unsigned char N86R11 = TRI->getEncodingValue(X86::R11) & 0x7;

    const unsigned char REX_WB = 0x40 | 0x08 | 0x01; // REX prefix

    // Load the pointer to the nested function into R11.
    unsigned OpCode = ((MOV64ri | N86R11) << 8) | REX_WB; // movabsq r11
    SDValue Addr = Trmp;
    OutChains[0] = DAG.getStore(Root, dl, DAG.getConstant(OpCode, MVT::i16),
                                Addr, MachinePointerInfo(TrmpAddr),
                                false, false, 0);

    Addr = DAG.getNode(ISD::ADD, dl, MVT::i64, Trmp,
                       DAG.getConstant(2, MVT::i64));
    OutChains[1] = DAG.getStore(Root, dl, FPtr, Addr,
                                MachinePointerInfo(TrmpAddr, 2),
                                false, false, 2);

    // Load the 'nest' parameter value into R10.
    // R10 is specified in X86CallingConv.td
    OpCode = ((MOV64ri | N86R10) << 8) | REX_WB; // movabsq r10
    Addr = DAG.getNode(ISD::ADD, dl, MVT::i64, Trmp,
                       DAG.getConstant(10, MVT::i64));
    OutChains[2] = DAG.getStore(Root, dl, DAG.getConstant(OpCode, MVT::i16),
                                Addr, MachinePointerInfo(TrmpAddr, 10),
                                false, false, 0);

    Addr = DAG.getNode(ISD::ADD, dl, MVT::i64, Trmp,
                       DAG.getConstant(12, MVT::i64));
    OutChains[3] = DAG.getStore(Root, dl, Nest, Addr,
                                MachinePointerInfo(TrmpAddr, 12),
                                false, false, 2);

    // Jump to the nested function.
    OpCode = (JMP64r << 8) | REX_WB; // jmpq *...
    Addr = DAG.getNode(ISD::ADD, dl, MVT::i64, Trmp,
                       DAG.getConstant(20, MVT::i64));
    OutChains[4] = DAG.getStore(Root, dl, DAG.getConstant(OpCode, MVT::i16),
                                Addr, MachinePointerInfo(TrmpAddr, 20),
                                false, false, 0);

    unsigned char ModRM = N86R11 | (4 << 3) | (3 << 6); // ...r11
    Addr = DAG.getNode(ISD::ADD, dl, MVT::i64, Trmp,
                       DAG.getConstant(22, MVT::i64));
    OutChains[5] = DAG.getStore(Root, dl, DAG.getConstant(ModRM, MVT::i8), Addr,
                                MachinePointerInfo(TrmpAddr, 22),
                                false, false, 0);

    return DAG.getNode(ISD::TokenFactor, dl, MVT::Other, OutChains);
  } else {
    const Function *Func =
      cast<Function>(cast<SrcValueSDNode>(Op.getOperand(5))->getValue());
    CallingConv::ID CC = Func->getCallingConv();
    unsigned NestReg;

    switch (CC) {
    default:
      llvm_unreachable("Unsupported calling convention");
    case CallingConv::C:
    case CallingConv::X86_StdCall: {
      // Pass 'nest' parameter in ECX.
      // Must be kept in sync with X86CallingConv.td
      NestReg = X86::ECX;

      // Check that ECX wasn't needed by an 'inreg' parameter.
      FunctionType *FTy = Func->getFunctionType();
      const AttributeSet &Attrs = Func->getAttributes();

      if (!Attrs.isEmpty() && !Func->isVarArg()) {
        unsigned InRegCount = 0;
        unsigned Idx = 1;

        for (FunctionType::param_iterator I = FTy->param_begin(),
             E = FTy->param_end(); I != E; ++I, ++Idx)
          if (Attrs.hasAttribute(Idx, Attribute::InReg))
            // FIXME: should only count parameters that are lowered to integers.
            InRegCount += (TD->getTypeSizeInBits(*I) + 31) / 32;

        if (InRegCount > 2) {
          report_fatal_error("Nest register in use - reduce number of inreg"
                             " parameters!");
        }
      }
      break;
    }
    case CallingConv::X86_FastCall:
    case CallingConv::X86_ThisCall:
    case CallingConv::Fast:
      // Pass 'nest' parameter in EAX.
      // Must be kept in sync with X86CallingConv.td
      NestReg = X86::EAX;
      break;
    }

    SDValue OutChains[4];
    SDValue Addr, Disp;

    Addr = DAG.getNode(ISD::ADD, dl, MVT::i32, Trmp,
                       DAG.getConstant(10, MVT::i32));
    Disp = DAG.getNode(ISD::SUB, dl, MVT::i32, FPtr, Addr);

    // This is storing the opcode for MOV32ri.
    const unsigned char MOV32ri = 0xB8; // X86::MOV32ri's opcode byte.
    const unsigned char N86Reg = TRI->getEncodingValue(NestReg) & 0x7;
    OutChains[0] = DAG.getStore(Root, dl,
                                DAG.getConstant(MOV32ri|N86Reg, MVT::i8),
                                Trmp, MachinePointerInfo(TrmpAddr),
                                false, false, 0);

    Addr = DAG.getNode(ISD::ADD, dl, MVT::i32, Trmp,
                       DAG.getConstant(1, MVT::i32));
    OutChains[1] = DAG.getStore(Root, dl, Nest, Addr,
                                MachinePointerInfo(TrmpAddr, 1),
                                false, false, 1);

    const unsigned char JMP = 0xE9; // jmp <32bit dst> opcode.
    Addr = DAG.getNode(ISD::ADD, dl, MVT::i32, Trmp,
                       DAG.getConstant(5, MVT::i32));
    OutChains[2] = DAG.getStore(Root, dl, DAG.getConstant(JMP, MVT::i8), Addr,
                                MachinePointerInfo(TrmpAddr, 5),
                                false, false, 1);

    Addr = DAG.getNode(ISD::ADD, dl, MVT::i32, Trmp,
                       DAG.getConstant(6, MVT::i32));
    OutChains[3] = DAG.getStore(Root, dl, Disp, Addr,
                                MachinePointerInfo(TrmpAddr, 6),
                                false, false, 1);

    return DAG.getNode(ISD::TokenFactor, dl, MVT::Other, OutChains);
  }
}

SDValue X86TargetLowering::LowerFLT_ROUNDS_(SDValue Op,
                                            SelectionDAG &DAG) const {
  /*
   The rounding mode is in bits 11:10 of FPSR, and has the following
   settings:
     00 Round to nearest
     01 Round to -inf
     10 Round to +inf
     11 Round to 0

  FLT_ROUNDS, on the other hand, expects the following:
    -1 Undefined
     0 Round to 0
     1 Round to nearest
     2 Round to +inf
     3 Round to -inf

  To perform the conversion, we do:
    (((((FPSR & 0x800) >> 11) | ((FPSR & 0x400) >> 9)) + 1) & 3)
  */

  MachineFunction &MF = DAG.getMachineFunction();
  const TargetMachine &TM = MF.getTarget();
  const TargetFrameLowering &TFI = *TM.getSubtargetImpl()->getFrameLowering();
  unsigned StackAlignment = TFI.getStackAlignment();
  MVT VT = Op.getSimpleValueType();
  SDLoc DL(Op);

  // Save FP Control Word to stack slot
  int SSFI = MF.getFrameInfo()->CreateStackObject(2, StackAlignment, false);
  SDValue StackSlot = DAG.getFrameIndex(SSFI, getPointerTy());

  MachineMemOperand *MMO =
   MF.getMachineMemOperand(MachinePointerInfo::getFixedStack(SSFI),
                           MachineMemOperand::MOStore, 2, 2);

  SDValue Ops[] = { DAG.getEntryNode(), StackSlot };
  SDValue Chain = DAG.getMemIntrinsicNode(X86ISD::FNSTCW16m, DL,
                                          DAG.getVTList(MVT::Other),
                                          Ops, MVT::i16, MMO);

  // Load FP Control Word from stack slot
  SDValue CWD = DAG.getLoad(MVT::i16, DL, Chain, StackSlot,
                            MachinePointerInfo(), false, false, false, 0);

  // Transform as necessary
  SDValue CWD1 =
    DAG.getNode(ISD::SRL, DL, MVT::i16,
                DAG.getNode(ISD::AND, DL, MVT::i16,
                            CWD, DAG.getConstant(0x800, MVT::i16)),
                DAG.getConstant(11, MVT::i8));
  SDValue CWD2 =
    DAG.getNode(ISD::SRL, DL, MVT::i16,
                DAG.getNode(ISD::AND, DL, MVT::i16,
                            CWD, DAG.getConstant(0x400, MVT::i16)),
                DAG.getConstant(9, MVT::i8));

  SDValue RetVal =
    DAG.getNode(ISD::AND, DL, MVT::i16,
                DAG.getNode(ISD::ADD, DL, MVT::i16,
                            DAG.getNode(ISD::OR, DL, MVT::i16, CWD1, CWD2),
                            DAG.getConstant(1, MVT::i16)),
                DAG.getConstant(3, MVT::i16));

  return DAG.getNode((VT.getSizeInBits() < 16 ?
                      ISD::TRUNCATE : ISD::ZERO_EXTEND), DL, VT, RetVal);
}

static SDValue LowerCTLZ(SDValue Op, SelectionDAG &DAG) {
  MVT VT = Op.getSimpleValueType();
  EVT OpVT = VT;
  unsigned NumBits = VT.getSizeInBits();
  SDLoc dl(Op);

  Op = Op.getOperand(0);
  if (VT == MVT::i8) {
    // Zero extend to i32 since there is not an i8 bsr.
    OpVT = MVT::i32;
    Op = DAG.getNode(ISD::ZERO_EXTEND, dl, OpVT, Op);
  }

  // Issue a bsr (scan bits in reverse) which also sets EFLAGS.
  SDVTList VTs = DAG.getVTList(OpVT, MVT::i32);
  Op = DAG.getNode(X86ISD::BSR, dl, VTs, Op);

  // If src is zero (i.e. bsr sets ZF), returns NumBits.
  SDValue Ops[] = {
    Op,
    DAG.getConstant(NumBits+NumBits-1, OpVT),
    DAG.getConstant(X86::COND_E, MVT::i8),
    Op.getValue(1)
  };
  Op = DAG.getNode(X86ISD::CMOV, dl, OpVT, Ops);

  // Finally xor with NumBits-1.
  Op = DAG.getNode(ISD::XOR, dl, OpVT, Op, DAG.getConstant(NumBits-1, OpVT));

  if (VT == MVT::i8)
    Op = DAG.getNode(ISD::TRUNCATE, dl, MVT::i8, Op);
  return Op;
}

static SDValue LowerCTLZ_ZERO_UNDEF(SDValue Op, SelectionDAG &DAG) {
  MVT VT = Op.getSimpleValueType();
  EVT OpVT = VT;
  unsigned NumBits = VT.getSizeInBits();
  SDLoc dl(Op);

  Op = Op.getOperand(0);
  if (VT == MVT::i8) {
    // Zero extend to i32 since there is not an i8 bsr.
    OpVT = MVT::i32;
    Op = DAG.getNode(ISD::ZERO_EXTEND, dl, OpVT, Op);
  }

  // Issue a bsr (scan bits in reverse).
  SDVTList VTs = DAG.getVTList(OpVT, MVT::i32);
  Op = DAG.getNode(X86ISD::BSR, dl, VTs, Op);

  // And xor with NumBits-1.
  Op = DAG.getNode(ISD::XOR, dl, OpVT, Op, DAG.getConstant(NumBits-1, OpVT));

  if (VT == MVT::i8)
    Op = DAG.getNode(ISD::TRUNCATE, dl, MVT::i8, Op);
  return Op;
}

static SDValue LowerCTTZ(SDValue Op, SelectionDAG &DAG) {
  MVT VT = Op.getSimpleValueType();
  unsigned NumBits = VT.getSizeInBits();
  SDLoc dl(Op);
  Op = Op.getOperand(0);

  // Issue a bsf (scan bits forward) which also sets EFLAGS.
  SDVTList VTs = DAG.getVTList(VT, MVT::i32);
  Op = DAG.getNode(X86ISD::BSF, dl, VTs, Op);

  // If src is zero (i.e. bsf sets ZF), returns NumBits.
  SDValue Ops[] = {
    Op,
    DAG.getConstant(NumBits, VT),
    DAG.getConstant(X86::COND_E, MVT::i8),
    Op.getValue(1)
  };
  return DAG.getNode(X86ISD::CMOV, dl, VT, Ops);
}

// Lower256IntArith - Break a 256-bit integer operation into two new 128-bit
// ones, and then concatenate the result back.
static SDValue Lower256IntArith(SDValue Op, SelectionDAG &DAG) {
  MVT VT = Op.getSimpleValueType();

  assert(VT.is256BitVector() && VT.isInteger() &&
         "Unsupported value type for operation");

  unsigned NumElems = VT.getVectorNumElements();
  SDLoc dl(Op);

  // Extract the LHS vectors
  SDValue LHS = Op.getOperand(0);
  SDValue LHS1 = Extract128BitVector(LHS, 0, DAG, dl);
  SDValue LHS2 = Extract128BitVector(LHS, NumElems/2, DAG, dl);

  // Extract the RHS vectors
  SDValue RHS = Op.getOperand(1);
  SDValue RHS1 = Extract128BitVector(RHS, 0, DAG, dl);
  SDValue RHS2 = Extract128BitVector(RHS, NumElems/2, DAG, dl);

  MVT EltVT = VT.getVectorElementType();
  MVT NewVT = MVT::getVectorVT(EltVT, NumElems/2);

  return DAG.getNode(ISD::CONCAT_VECTORS, dl, VT,
                     DAG.getNode(Op.getOpcode(), dl, NewVT, LHS1, RHS1),
                     DAG.getNode(Op.getOpcode(), dl, NewVT, LHS2, RHS2));
}

static SDValue LowerADD(SDValue Op, SelectionDAG &DAG) {
  assert(Op.getSimpleValueType().is256BitVector() &&
         Op.getSimpleValueType().isInteger() &&
         "Only handle AVX 256-bit vector integer operation");
  return Lower256IntArith(Op, DAG);
}

static SDValue LowerSUB(SDValue Op, SelectionDAG &DAG) {
  assert(Op.getSimpleValueType().is256BitVector() &&
         Op.getSimpleValueType().isInteger() &&
         "Only handle AVX 256-bit vector integer operation");
  return Lower256IntArith(Op, DAG);
}

static SDValue LowerMUL(SDValue Op, const X86Subtarget *Subtarget,
                        SelectionDAG &DAG) {
  SDLoc dl(Op);
  MVT VT = Op.getSimpleValueType();

  // Decompose 256-bit ops into smaller 128-bit ops.
  if (VT.is256BitVector() && !Subtarget->hasInt256())
    return Lower256IntArith(Op, DAG);

  SDValue A = Op.getOperand(0);
  SDValue B = Op.getOperand(1);

  // Lower v4i32 mul as 2x shuffle, 2x pmuludq, 2x shuffle.
  if (VT == MVT::v4i32) {
    assert(Subtarget->hasSSE2() && !Subtarget->hasSSE41() &&
           "Should not custom lower when pmuldq is available!");

    // Extract the odd parts.
    static const int UnpackMask[] = { 1, -1, 3, -1 };
    SDValue Aodds = DAG.getVectorShuffle(VT, dl, A, A, UnpackMask);
    SDValue Bodds = DAG.getVectorShuffle(VT, dl, B, B, UnpackMask);

    // Multiply the even parts.
    SDValue Evens = DAG.getNode(X86ISD::PMULUDQ, dl, MVT::v2i64, A, B);
    // Now multiply odd parts.
    SDValue Odds = DAG.getNode(X86ISD::PMULUDQ, dl, MVT::v2i64, Aodds, Bodds);

    Evens = DAG.getNode(ISD::BITCAST, dl, VT, Evens);
    Odds = DAG.getNode(ISD::BITCAST, dl, VT, Odds);

    // Merge the two vectors back together with a shuffle. This expands into 2
    // shuffles.
    static const int ShufMask[] = { 0, 4, 2, 6 };
    return DAG.getVectorShuffle(VT, dl, Evens, Odds, ShufMask);
  }

  assert((VT == MVT::v2i64 || VT == MVT::v4i64 || VT == MVT::v8i64) &&
         "Only know how to lower V2I64/V4I64/V8I64 multiply");

  //  Ahi = psrlqi(a, 32);
  //  Bhi = psrlqi(b, 32);
  //
  //  AloBlo = pmuludq(a, b);
  //  AloBhi = pmuludq(a, Bhi);
  //  AhiBlo = pmuludq(Ahi, b);

  //  AloBhi = psllqi(AloBhi, 32);
  //  AhiBlo = psllqi(AhiBlo, 32);
  //  return AloBlo + AloBhi + AhiBlo;

  SDValue Ahi = getTargetVShiftByConstNode(X86ISD::VSRLI, dl, VT, A, 32, DAG);
  SDValue Bhi = getTargetVShiftByConstNode(X86ISD::VSRLI, dl, VT, B, 32, DAG);

  // Bit cast to 32-bit vectors for MULUDQ
  EVT MulVT = (VT == MVT::v2i64) ? MVT::v4i32 :
                                  (VT == MVT::v4i64) ? MVT::v8i32 : MVT::v16i32;
  A = DAG.getNode(ISD::BITCAST, dl, MulVT, A);
  B = DAG.getNode(ISD::BITCAST, dl, MulVT, B);
  Ahi = DAG.getNode(ISD::BITCAST, dl, MulVT, Ahi);
  Bhi = DAG.getNode(ISD::BITCAST, dl, MulVT, Bhi);

  SDValue AloBlo = DAG.getNode(X86ISD::PMULUDQ, dl, VT, A, B);
  SDValue AloBhi = DAG.getNode(X86ISD::PMULUDQ, dl, VT, A, Bhi);
  SDValue AhiBlo = DAG.getNode(X86ISD::PMULUDQ, dl, VT, Ahi, B);

  AloBhi = getTargetVShiftByConstNode(X86ISD::VSHLI, dl, VT, AloBhi, 32, DAG);
  AhiBlo = getTargetVShiftByConstNode(X86ISD::VSHLI, dl, VT, AhiBlo, 32, DAG);

  SDValue Res = DAG.getNode(ISD::ADD, dl, VT, AloBlo, AloBhi);
  return DAG.getNode(ISD::ADD, dl, VT, Res, AhiBlo);
}

SDValue X86TargetLowering::LowerWin64_i128OP(SDValue Op, SelectionDAG &DAG) const {
  assert(Subtarget->isTargetWin64() && "Unexpected target");
  EVT VT = Op.getValueType();
  assert(VT.isInteger() && VT.getSizeInBits() == 128 &&
         "Unexpected return type for lowering");

  RTLIB::Libcall LC;
  bool isSigned;
  switch (Op->getOpcode()) {
  default: llvm_unreachable("Unexpected request for libcall!");
  case ISD::SDIV:      isSigned = true;  LC = RTLIB::SDIV_I128;    break;
  case ISD::UDIV:      isSigned = false; LC = RTLIB::UDIV_I128;    break;
  case ISD::SREM:      isSigned = true;  LC = RTLIB::SREM_I128;    break;
  case ISD::UREM:      isSigned = false; LC = RTLIB::UREM_I128;    break;
  case ISD::SDIVREM:   isSigned = true;  LC = RTLIB::SDIVREM_I128; break;
  case ISD::UDIVREM:   isSigned = false; LC = RTLIB::UDIVREM_I128; break;
  }

  SDLoc dl(Op);
  SDValue InChain = DAG.getEntryNode();

  TargetLowering::ArgListTy Args;
  TargetLowering::ArgListEntry Entry;
  for (unsigned i = 0, e = Op->getNumOperands(); i != e; ++i) {
    EVT ArgVT = Op->getOperand(i).getValueType();
    assert(ArgVT.isInteger() && ArgVT.getSizeInBits() == 128 &&
           "Unexpected argument type for lowering");
    SDValue StackPtr = DAG.CreateStackTemporary(ArgVT, 16);
    Entry.Node = StackPtr;
    InChain = DAG.getStore(InChain, dl, Op->getOperand(i), StackPtr, MachinePointerInfo(),
                           false, false, 16);
    Type *ArgTy = ArgVT.getTypeForEVT(*DAG.getContext());
    Entry.Ty = PointerType::get(ArgTy,0);
    Entry.isSExt = false;
    Entry.isZExt = false;
    Args.push_back(Entry);
  }

  SDValue Callee = DAG.getExternalSymbol(getLibcallName(LC),
                                         getPointerTy());

  TargetLowering::CallLoweringInfo CLI(DAG);
  CLI.setDebugLoc(dl).setChain(InChain)
    .setCallee(getLibcallCallingConv(LC),
               static_cast<EVT>(MVT::v2i64).getTypeForEVT(*DAG.getContext()),
               Callee, std::move(Args), 0)
    .setInRegister().setSExtResult(isSigned).setZExtResult(!isSigned);

  std::pair<SDValue, SDValue> CallInfo = LowerCallTo(CLI);
  return DAG.getNode(ISD::BITCAST, dl, VT, CallInfo.first);
}

static SDValue LowerMUL_LOHI(SDValue Op, const X86Subtarget *Subtarget,
                             SelectionDAG &DAG) {
  SDValue Op0 = Op.getOperand(0), Op1 = Op.getOperand(1);
  EVT VT = Op0.getValueType();
  SDLoc dl(Op);

  assert((VT == MVT::v4i32 && Subtarget->hasSSE2()) ||
         (VT == MVT::v8i32 && Subtarget->hasInt256()));

  // PMULxD operations multiply each even value (starting at 0) of LHS with
  // the related value of RHS and produce a widen result.
  // E.g., PMULUDQ <4 x i32> <a|b|c|d>, <4 x i32> <e|f|g|h>
  // => <2 x i64> <ae|cg>
  //
  // In other word, to have all the results, we need to perform two PMULxD:
  // 1. one with the even values.
  // 2. one with the odd values.
  // To achieve #2, with need to place the odd values at an even position.
  //
  // Place the odd value at an even position (basically, shift all values 1
  // step to the left):
  const int Mask[] = {1, -1, 3, -1, 5, -1, 7, -1};
  // <a|b|c|d> => <b|undef|d|undef>
  SDValue Odd0 = DAG.getVectorShuffle(VT, dl, Op0, Op0, Mask);
  // <e|f|g|h> => <f|undef|h|undef>
  SDValue Odd1 = DAG.getVectorShuffle(VT, dl, Op1, Op1, Mask);

  // Emit two multiplies, one for the lower 2 ints and one for the higher 2
  // ints.
  MVT MulVT = VT == MVT::v4i32 ? MVT::v2i64 : MVT::v4i64;
  bool IsSigned = Op->getOpcode() == ISD::SMUL_LOHI;
  unsigned Opcode =
      (!IsSigned || !Subtarget->hasSSE41()) ? X86ISD::PMULUDQ : X86ISD::PMULDQ;
  // PMULUDQ <4 x i32> <a|b|c|d>, <4 x i32> <e|f|g|h>
  // => <2 x i64> <ae|cg>
  SDValue Mul1 = DAG.getNode(ISD::BITCAST, dl, VT,
                             DAG.getNode(Opcode, dl, MulVT, Op0, Op1));
  // PMULUDQ <4 x i32> <b|undef|d|undef>, <4 x i32> <f|undef|h|undef>
  // => <2 x i64> <bf|dh>
  SDValue Mul2 = DAG.getNode(ISD::BITCAST, dl, VT,
                             DAG.getNode(Opcode, dl, MulVT, Odd0, Odd1));

  // Shuffle it back into the right order.
  SDValue Highs, Lows;
  if (VT == MVT::v8i32) {
    const int HighMask[] = {1, 9, 3, 11, 5, 13, 7, 15};
    Highs = DAG.getVectorShuffle(VT, dl, Mul1, Mul2, HighMask);
    const int LowMask[] = {0, 8, 2, 10, 4, 12, 6, 14};
    Lows = DAG.getVectorShuffle(VT, dl, Mul1, Mul2, LowMask);
  } else {
    const int HighMask[] = {1, 5, 3, 7};
    Highs = DAG.getVectorShuffle(VT, dl, Mul1, Mul2, HighMask);
    const int LowMask[] = {0, 4, 2, 6};
    Lows = DAG.getVectorShuffle(VT, dl, Mul1, Mul2, LowMask);
  }

  // If we have a signed multiply but no PMULDQ fix up the high parts of a
  // unsigned multiply.
  if (IsSigned && !Subtarget->hasSSE41()) {
    SDValue ShAmt =
        DAG.getConstant(31, DAG.getTargetLoweringInfo().getShiftAmountTy(VT));
    SDValue T1 = DAG.getNode(ISD::AND, dl, VT,
                             DAG.getNode(ISD::SRA, dl, VT, Op0, ShAmt), Op1);
    SDValue T2 = DAG.getNode(ISD::AND, dl, VT,
                             DAG.getNode(ISD::SRA, dl, VT, Op1, ShAmt), Op0);

    SDValue Fixup = DAG.getNode(ISD::ADD, dl, VT, T1, T2);
    Highs = DAG.getNode(ISD::SUB, dl, VT, Highs, Fixup);
  }

  // The first result of MUL_LOHI is actually the low value, followed by the
  // high value.
  SDValue Ops[] = {Lows, Highs};
  return DAG.getMergeValues(Ops, dl);
}

static SDValue LowerScalarImmediateShift(SDValue Op, SelectionDAG &DAG,
                                         const X86Subtarget *Subtarget) {
  MVT VT = Op.getSimpleValueType();
  SDLoc dl(Op);
  SDValue R = Op.getOperand(0);
  SDValue Amt = Op.getOperand(1);

  // Optimize shl/srl/sra with constant shift amount.
  if (auto *BVAmt = dyn_cast<BuildVectorSDNode>(Amt)) {
    if (auto *ShiftConst = BVAmt->getConstantSplatNode()) {
      uint64_t ShiftAmt = ShiftConst->getZExtValue();

      if (VT == MVT::v2i64 || VT == MVT::v4i32 || VT == MVT::v8i16 ||
          (Subtarget->hasInt256() &&
           (VT == MVT::v4i64 || VT == MVT::v8i32 || VT == MVT::v16i16)) ||
          (Subtarget->hasAVX512() &&
           (VT == MVT::v8i64 || VT == MVT::v16i32))) {
        if (Op.getOpcode() == ISD::SHL)
          return getTargetVShiftByConstNode(X86ISD::VSHLI, dl, VT, R, ShiftAmt,
                                            DAG);
        if (Op.getOpcode() == ISD::SRL)
          return getTargetVShiftByConstNode(X86ISD::VSRLI, dl, VT, R, ShiftAmt,
                                            DAG);
        if (Op.getOpcode() == ISD::SRA && VT != MVT::v2i64 && VT != MVT::v4i64)
          return getTargetVShiftByConstNode(X86ISD::VSRAI, dl, VT, R, ShiftAmt,
                                            DAG);
      }

      if (VT == MVT::v16i8) {
        if (Op.getOpcode() == ISD::SHL) {
          // Make a large shift.
          SDValue SHL = getTargetVShiftByConstNode(X86ISD::VSHLI, dl,
                                                   MVT::v8i16, R, ShiftAmt,
                                                   DAG);
          SHL = DAG.getNode(ISD::BITCAST, dl, VT, SHL);
          // Zero out the rightmost bits.
          SmallVector<SDValue, 16> V(16,
                                     DAG.getConstant(uint8_t(-1U << ShiftAmt),
                                                     MVT::i8));
          return DAG.getNode(ISD::AND, dl, VT, SHL,
                             DAG.getNode(ISD::BUILD_VECTOR, dl, VT, V));
        }
        if (Op.getOpcode() == ISD::SRL) {
          // Make a large shift.
          SDValue SRL = getTargetVShiftByConstNode(X86ISD::VSRLI, dl,
                                                   MVT::v8i16, R, ShiftAmt,
                                                   DAG);
          SRL = DAG.getNode(ISD::BITCAST, dl, VT, SRL);
          // Zero out the leftmost bits.
          SmallVector<SDValue, 16> V(16,
                                     DAG.getConstant(uint8_t(-1U) >> ShiftAmt,
                                                     MVT::i8));
          return DAG.getNode(ISD::AND, dl, VT, SRL,
                             DAG.getNode(ISD::BUILD_VECTOR, dl, VT, V));
        }
        if (Op.getOpcode() == ISD::SRA) {
          if (ShiftAmt == 7) {
            // R s>> 7  ===  R s< 0
            SDValue Zeros = getZeroVector(VT, Subtarget, DAG, dl);
            return DAG.getNode(X86ISD::PCMPGT, dl, VT, Zeros, R);
          }

          // R s>> a === ((R u>> a) ^ m) - m
          SDValue Res = DAG.getNode(ISD::SRL, dl, VT, R, Amt);
          SmallVector<SDValue, 16> V(16, DAG.getConstant(128 >> ShiftAmt,
                                                         MVT::i8));
          SDValue Mask = DAG.getNode(ISD::BUILD_VECTOR, dl, VT, V);
          Res = DAG.getNode(ISD::XOR, dl, VT, Res, Mask);
          Res = DAG.getNode(ISD::SUB, dl, VT, Res, Mask);
          return Res;
        }
        llvm_unreachable("Unknown shift opcode.");
      }

      if (Subtarget->hasInt256() && VT == MVT::v32i8) {
        if (Op.getOpcode() == ISD::SHL) {
          // Make a large shift.
          SDValue SHL = getTargetVShiftByConstNode(X86ISD::VSHLI, dl,
                                                   MVT::v16i16, R, ShiftAmt,
                                                   DAG);
          SHL = DAG.getNode(ISD::BITCAST, dl, VT, SHL);
          // Zero out the rightmost bits.
          SmallVector<SDValue, 32> V(32,
                                     DAG.getConstant(uint8_t(-1U << ShiftAmt),
                                                     MVT::i8));
          return DAG.getNode(ISD::AND, dl, VT, SHL,
                             DAG.getNode(ISD::BUILD_VECTOR, dl, VT, V));
        }
        if (Op.getOpcode() == ISD::SRL) {
          // Make a large shift.
          SDValue SRL = getTargetVShiftByConstNode(X86ISD::VSRLI, dl,
                                                   MVT::v16i16, R, ShiftAmt,
                                                   DAG);
          SRL = DAG.getNode(ISD::BITCAST, dl, VT, SRL);
          // Zero out the leftmost bits.
          SmallVector<SDValue, 32> V(32,
                                     DAG.getConstant(uint8_t(-1U) >> ShiftAmt,
                                                     MVT::i8));
          return DAG.getNode(ISD::AND, dl, VT, SRL,
                             DAG.getNode(ISD::BUILD_VECTOR, dl, VT, V));
        }
        if (Op.getOpcode() == ISD::SRA) {
          if (ShiftAmt == 7) {
            // R s>> 7  ===  R s< 0
            SDValue Zeros = getZeroVector(VT, Subtarget, DAG, dl);
            return DAG.getNode(X86ISD::PCMPGT, dl, VT, Zeros, R);
          }

          // R s>> a === ((R u>> a) ^ m) - m
          SDValue Res = DAG.getNode(ISD::SRL, dl, VT, R, Amt);
          SmallVector<SDValue, 32> V(32, DAG.getConstant(128 >> ShiftAmt,
                                                         MVT::i8));
          SDValue Mask = DAG.getNode(ISD::BUILD_VECTOR, dl, VT, V);
          Res = DAG.getNode(ISD::XOR, dl, VT, Res, Mask);
          Res = DAG.getNode(ISD::SUB, dl, VT, Res, Mask);
          return Res;
        }
        llvm_unreachable("Unknown shift opcode.");
      }
    }
  }

  // Special case in 32-bit mode, where i64 is expanded into high and low parts.
  if (!Subtarget->is64Bit() &&
      (VT == MVT::v2i64 || (Subtarget->hasInt256() && VT == MVT::v4i64)) &&
      Amt.getOpcode() == ISD::BITCAST &&
      Amt.getOperand(0).getOpcode() == ISD::BUILD_VECTOR) {
    Amt = Amt.getOperand(0);
    unsigned Ratio = Amt.getSimpleValueType().getVectorNumElements() /
                     VT.getVectorNumElements();
    unsigned RatioInLog2 = Log2_32_Ceil(Ratio);
    uint64_t ShiftAmt = 0;
    for (unsigned i = 0; i != Ratio; ++i) {
      ConstantSDNode *C = dyn_cast<ConstantSDNode>(Amt.getOperand(i));
      if (!C)
        return SDValue();
      // 6 == Log2(64)
      ShiftAmt |= C->getZExtValue() << (i * (1 << (6 - RatioInLog2)));
    }
    // Check remaining shift amounts.
    for (unsigned i = Ratio; i != Amt.getNumOperands(); i += Ratio) {
      uint64_t ShAmt = 0;
      for (unsigned j = 0; j != Ratio; ++j) {
        ConstantSDNode *C =
          dyn_cast<ConstantSDNode>(Amt.getOperand(i + j));
        if (!C)
          return SDValue();
        // 6 == Log2(64)
        ShAmt |= C->getZExtValue() << (j * (1 << (6 - RatioInLog2)));
      }
      if (ShAmt != ShiftAmt)
        return SDValue();
    }
    switch (Op.getOpcode()) {
    default:
      llvm_unreachable("Unknown shift opcode!");
    case ISD::SHL:
      return getTargetVShiftByConstNode(X86ISD::VSHLI, dl, VT, R, ShiftAmt,
                                        DAG);
    case ISD::SRL:
      return getTargetVShiftByConstNode(X86ISD::VSRLI, dl, VT, R, ShiftAmt,
                                        DAG);
    case ISD::SRA:
      return getTargetVShiftByConstNode(X86ISD::VSRAI, dl, VT, R, ShiftAmt,
                                        DAG);
    }
  }

  return SDValue();
}

static SDValue LowerScalarVariableShift(SDValue Op, SelectionDAG &DAG,
                                        const X86Subtarget* Subtarget) {
  MVT VT = Op.getSimpleValueType();
  SDLoc dl(Op);
  SDValue R = Op.getOperand(0);
  SDValue Amt = Op.getOperand(1);

  if ((VT == MVT::v2i64 && Op.getOpcode() != ISD::SRA) ||
      VT == MVT::v4i32 || VT == MVT::v8i16 ||
      (Subtarget->hasInt256() &&
       ((VT == MVT::v4i64 && Op.getOpcode() != ISD::SRA) ||
        VT == MVT::v8i32 || VT == MVT::v16i16)) ||
       (Subtarget->hasAVX512() && (VT == MVT::v8i64 || VT == MVT::v16i32))) {
    SDValue BaseShAmt;
    EVT EltVT = VT.getVectorElementType();

    if (Amt.getOpcode() == ISD::BUILD_VECTOR) {
      unsigned NumElts = VT.getVectorNumElements();
      unsigned i, j;
      for (i = 0; i != NumElts; ++i) {
        if (Amt.getOperand(i).getOpcode() == ISD::UNDEF)
          continue;
        break;
      }
      for (j = i; j != NumElts; ++j) {
        SDValue Arg = Amt.getOperand(j);
        if (Arg.getOpcode() == ISD::UNDEF) continue;
        if (Arg != Amt.getOperand(i))
          break;
      }
      if (i != NumElts && j == NumElts)
        BaseShAmt = Amt.getOperand(i);
    } else {
      if (Amt.getOpcode() == ISD::EXTRACT_SUBVECTOR)
        Amt = Amt.getOperand(0);
      if (Amt.getOpcode() == ISD::VECTOR_SHUFFLE &&
               cast<ShuffleVectorSDNode>(Amt)->isSplat()) {
        SDValue InVec = Amt.getOperand(0);
        if (InVec.getOpcode() == ISD::BUILD_VECTOR) {
          unsigned NumElts = InVec.getValueType().getVectorNumElements();
          unsigned i = 0;
          for (; i != NumElts; ++i) {
            SDValue Arg = InVec.getOperand(i);
            if (Arg.getOpcode() == ISD::UNDEF) continue;
            BaseShAmt = Arg;
            break;
          }
        } else if (InVec.getOpcode() == ISD::INSERT_VECTOR_ELT) {
           if (ConstantSDNode *C =
               dyn_cast<ConstantSDNode>(InVec.getOperand(2))) {
             unsigned SplatIdx =
               cast<ShuffleVectorSDNode>(Amt)->getSplatIndex();
             if (C->getZExtValue() == SplatIdx)
               BaseShAmt = InVec.getOperand(1);
           }
        }
        if (!BaseShAmt.getNode())
          BaseShAmt = DAG.getNode(ISD::EXTRACT_VECTOR_ELT, dl, EltVT, Amt,
                                  DAG.getIntPtrConstant(0));
      }
    }

    if (BaseShAmt.getNode()) {
      if (EltVT.bitsGT(MVT::i32))
        BaseShAmt = DAG.getNode(ISD::TRUNCATE, dl, MVT::i32, BaseShAmt);
      else if (EltVT.bitsLT(MVT::i32))
        BaseShAmt = DAG.getNode(ISD::ZERO_EXTEND, dl, MVT::i32, BaseShAmt);

      switch (Op.getOpcode()) {
      default:
        llvm_unreachable("Unknown shift opcode!");
      case ISD::SHL:
        switch (VT.SimpleTy) {
        default: return SDValue();
        case MVT::v2i64:
        case MVT::v4i32:
        case MVT::v8i16:
        case MVT::v4i64:
        case MVT::v8i32:
        case MVT::v16i16:
        case MVT::v16i32:
        case MVT::v8i64:
          return getTargetVShiftNode(X86ISD::VSHLI, dl, VT, R, BaseShAmt, DAG);
        }
      case ISD::SRA:
        switch (VT.SimpleTy) {
        default: return SDValue();
        case MVT::v4i32:
        case MVT::v8i16:
        case MVT::v8i32:
        case MVT::v16i16:
        case MVT::v16i32:
        case MVT::v8i64:
          return getTargetVShiftNode(X86ISD::VSRAI, dl, VT, R, BaseShAmt, DAG);
        }
      case ISD::SRL:
        switch (VT.SimpleTy) {
        default: return SDValue();
        case MVT::v2i64:
        case MVT::v4i32:
        case MVT::v8i16:
        case MVT::v4i64:
        case MVT::v8i32:
        case MVT::v16i16:
        case MVT::v16i32:
        case MVT::v8i64:
          return getTargetVShiftNode(X86ISD::VSRLI, dl, VT, R, BaseShAmt, DAG);
        }
      }
    }
  }

  // Special case in 32-bit mode, where i64 is expanded into high and low parts.
  if (!Subtarget->is64Bit() &&
      (VT == MVT::v2i64 || (Subtarget->hasInt256() && VT == MVT::v4i64) ||
      (Subtarget->hasAVX512() && VT == MVT::v8i64)) &&
      Amt.getOpcode() == ISD::BITCAST &&
      Amt.getOperand(0).getOpcode() == ISD::BUILD_VECTOR) {
    Amt = Amt.getOperand(0);
    unsigned Ratio = Amt.getSimpleValueType().getVectorNumElements() /
                     VT.getVectorNumElements();
    std::vector<SDValue> Vals(Ratio);
    for (unsigned i = 0; i != Ratio; ++i)
      Vals[i] = Amt.getOperand(i);
    for (unsigned i = Ratio; i != Amt.getNumOperands(); i += Ratio) {
      for (unsigned j = 0; j != Ratio; ++j)
        if (Vals[j] != Amt.getOperand(i + j))
          return SDValue();
    }
    switch (Op.getOpcode()) {
    default:
      llvm_unreachable("Unknown shift opcode!");
    case ISD::SHL:
      return DAG.getNode(X86ISD::VSHL, dl, VT, R, Op.getOperand(1));
    case ISD::SRL:
      return DAG.getNode(X86ISD::VSRL, dl, VT, R, Op.getOperand(1));
    case ISD::SRA:
      return DAG.getNode(X86ISD::VSRA, dl, VT, R, Op.getOperand(1));
    }
  }

  return SDValue();
}

static SDValue LowerShift(SDValue Op, const X86Subtarget* Subtarget,
                          SelectionDAG &DAG) {
  MVT VT = Op.getSimpleValueType();
  SDLoc dl(Op);
  SDValue R = Op.getOperand(0);
  SDValue Amt = Op.getOperand(1);
  SDValue V;

  assert(VT.isVector() && "Custom lowering only for vector shifts!");
  assert(Subtarget->hasSSE2() && "Only custom lower when we have SSE2!");

  V = LowerScalarImmediateShift(Op, DAG, Subtarget);
  if (V.getNode())
    return V;

  V = LowerScalarVariableShift(Op, DAG, Subtarget);
  if (V.getNode())
      return V;

  if (Subtarget->hasAVX512() && (VT == MVT::v16i32 || VT == MVT::v8i64))
    return Op;
  // AVX2 has VPSLLV/VPSRAV/VPSRLV.
  if (Subtarget->hasInt256()) {
    if (Op.getOpcode() == ISD::SRL &&
        (VT == MVT::v2i64 || VT == MVT::v4i32 ||
         VT == MVT::v4i64 || VT == MVT::v8i32))
      return Op;
    if (Op.getOpcode() == ISD::SHL &&
        (VT == MVT::v2i64 || VT == MVT::v4i32 ||
         VT == MVT::v4i64 || VT == MVT::v8i32))
      return Op;
    if (Op.getOpcode() == ISD::SRA && (VT == MVT::v4i32 || VT == MVT::v8i32))
      return Op;
  }

  // If possible, lower this packed shift into a vector multiply instead of
  // expanding it into a sequence of scalar shifts.
  // Do this only if the vector shift count is a constant build_vector.
  if (Op.getOpcode() == ISD::SHL && 
      (VT == MVT::v8i16 || VT == MVT::v4i32 ||
       (Subtarget->hasInt256() && VT == MVT::v16i16)) &&
      ISD::isBuildVectorOfConstantSDNodes(Amt.getNode())) {
    SmallVector<SDValue, 8> Elts;
    EVT SVT = VT.getScalarType();
    unsigned SVTBits = SVT.getSizeInBits();
    const APInt &One = APInt(SVTBits, 1);
    unsigned NumElems = VT.getVectorNumElements();

    for (unsigned i=0; i !=NumElems; ++i) {
      SDValue Op = Amt->getOperand(i);
      if (Op->getOpcode() == ISD::UNDEF) {
        Elts.push_back(Op);
        continue;
      }

      ConstantSDNode *ND = cast<ConstantSDNode>(Op);
      const APInt &C = APInt(SVTBits, ND->getAPIntValue().getZExtValue());
      uint64_t ShAmt = C.getZExtValue();
      if (ShAmt >= SVTBits) {
        Elts.push_back(DAG.getUNDEF(SVT));
        continue;
      }
      Elts.push_back(DAG.getConstant(One.shl(ShAmt), SVT));
    }
    SDValue BV = DAG.getNode(ISD::BUILD_VECTOR, dl, VT, Elts);
    return DAG.getNode(ISD::MUL, dl, VT, R, BV);
  }

  // Lower SHL with variable shift amount.
  if (VT == MVT::v4i32 && Op->getOpcode() == ISD::SHL) {
    Op = DAG.getNode(ISD::SHL, dl, VT, Amt, DAG.getConstant(23, VT));

    Op = DAG.getNode(ISD::ADD, dl, VT, Op, DAG.getConstant(0x3f800000U, VT));
    Op = DAG.getNode(ISD::BITCAST, dl, MVT::v4f32, Op);
    Op = DAG.getNode(ISD::FP_TO_SINT, dl, VT, Op);
    return DAG.getNode(ISD::MUL, dl, VT, Op, R);
  }

  // If possible, lower this shift as a sequence of two shifts by
  // constant plus a MOVSS/MOVSD instead of scalarizing it.
  // Example:
  //   (v4i32 (srl A, (build_vector < X, Y, Y, Y>)))
  //
  // Could be rewritten as:
  //   (v4i32 (MOVSS (srl A, <Y,Y,Y,Y>), (srl A, <X,X,X,X>)))
  //
  // The advantage is that the two shifts from the example would be
  // lowered as X86ISD::VSRLI nodes. This would be cheaper than scalarizing
  // the vector shift into four scalar shifts plus four pairs of vector
  // insert/extract.
  if ((VT == MVT::v8i16 || VT == MVT::v4i32) &&
      ISD::isBuildVectorOfConstantSDNodes(Amt.getNode())) {
    unsigned TargetOpcode = X86ISD::MOVSS;
    bool CanBeSimplified;
    // The splat value for the first packed shift (the 'X' from the example).
    SDValue Amt1 = Amt->getOperand(0);
    // The splat value for the second packed shift (the 'Y' from the example).
    SDValue Amt2 = (VT == MVT::v4i32) ? Amt->getOperand(1) :
                                        Amt->getOperand(2);

    // See if it is possible to replace this node with a sequence of
    // two shifts followed by a MOVSS/MOVSD
    if (VT == MVT::v4i32) {
      // Check if it is legal to use a MOVSS.
      CanBeSimplified = Amt2 == Amt->getOperand(2) &&
                        Amt2 == Amt->getOperand(3);
      if (!CanBeSimplified) {
        // Otherwise, check if we can still simplify this node using a MOVSD.
        CanBeSimplified = Amt1 == Amt->getOperand(1) &&
                          Amt->getOperand(2) == Amt->getOperand(3);
        TargetOpcode = X86ISD::MOVSD;
        Amt2 = Amt->getOperand(2);
      }
    } else {
      // Do similar checks for the case where the machine value type
      // is MVT::v8i16.
      CanBeSimplified = Amt1 == Amt->getOperand(1);
      for (unsigned i=3; i != 8 && CanBeSimplified; ++i)
        CanBeSimplified = Amt2 == Amt->getOperand(i);

      if (!CanBeSimplified) {
        TargetOpcode = X86ISD::MOVSD;
        CanBeSimplified = true;
        Amt2 = Amt->getOperand(4);
        for (unsigned i=0; i != 4 && CanBeSimplified; ++i)
          CanBeSimplified = Amt1 == Amt->getOperand(i);
        for (unsigned j=4; j != 8 && CanBeSimplified; ++j)
          CanBeSimplified = Amt2 == Amt->getOperand(j);
      }
    }
    
    if (CanBeSimplified && isa<ConstantSDNode>(Amt1) &&
        isa<ConstantSDNode>(Amt2)) {
      // Replace this node with two shifts followed by a MOVSS/MOVSD.
      EVT CastVT = MVT::v4i32;
      SDValue Splat1 = 
        DAG.getConstant(cast<ConstantSDNode>(Amt1)->getAPIntValue(), VT);
      SDValue Shift1 = DAG.getNode(Op->getOpcode(), dl, VT, R, Splat1);
      SDValue Splat2 = 
        DAG.getConstant(cast<ConstantSDNode>(Amt2)->getAPIntValue(), VT);
      SDValue Shift2 = DAG.getNode(Op->getOpcode(), dl, VT, R, Splat2);
      if (TargetOpcode == X86ISD::MOVSD)
        CastVT = MVT::v2i64;
      SDValue BitCast1 = DAG.getNode(ISD::BITCAST, dl, CastVT, Shift1);
      SDValue BitCast2 = DAG.getNode(ISD::BITCAST, dl, CastVT, Shift2);
      SDValue Result = getTargetShuffleNode(TargetOpcode, dl, CastVT, BitCast2,
                                            BitCast1, DAG);
      return DAG.getNode(ISD::BITCAST, dl, VT, Result);
    }
  }

  if (VT == MVT::v16i8 && Op->getOpcode() == ISD::SHL) {
    assert(Subtarget->hasSSE2() && "Need SSE2 for pslli/pcmpeq.");

    // a = a << 5;
    Op = DAG.getNode(ISD::SHL, dl, VT, Amt, DAG.getConstant(5, VT));
    Op = DAG.getNode(ISD::BITCAST, dl, VT, Op);

    // Turn 'a' into a mask suitable for VSELECT
    SDValue VSelM = DAG.getConstant(0x80, VT);
    SDValue OpVSel = DAG.getNode(ISD::AND, dl, VT, VSelM, Op);
    OpVSel = DAG.getNode(X86ISD::PCMPEQ, dl, VT, OpVSel, VSelM);

    SDValue CM1 = DAG.getConstant(0x0f, VT);
    SDValue CM2 = DAG.getConstant(0x3f, VT);

    // r = VSELECT(r, psllw(r & (char16)15, 4), a);
    SDValue M = DAG.getNode(ISD::AND, dl, VT, R, CM1);
    M = getTargetVShiftByConstNode(X86ISD::VSHLI, dl, MVT::v8i16, M, 4, DAG);
    M = DAG.getNode(ISD::BITCAST, dl, VT, M);
    R = DAG.getNode(ISD::VSELECT, dl, VT, OpVSel, M, R);

    // a += a
    Op = DAG.getNode(ISD::ADD, dl, VT, Op, Op);
    OpVSel = DAG.getNode(ISD::AND, dl, VT, VSelM, Op);
    OpVSel = DAG.getNode(X86ISD::PCMPEQ, dl, VT, OpVSel, VSelM);

    // r = VSELECT(r, psllw(r & (char16)63, 2), a);
    M = DAG.getNode(ISD::AND, dl, VT, R, CM2);
    M = getTargetVShiftByConstNode(X86ISD::VSHLI, dl, MVT::v8i16, M, 2, DAG);
    M = DAG.getNode(ISD::BITCAST, dl, VT, M);
    R = DAG.getNode(ISD::VSELECT, dl, VT, OpVSel, M, R);

    // a += a
    Op = DAG.getNode(ISD::ADD, dl, VT, Op, Op);
    OpVSel = DAG.getNode(ISD::AND, dl, VT, VSelM, Op);
    OpVSel = DAG.getNode(X86ISD::PCMPEQ, dl, VT, OpVSel, VSelM);

    // return VSELECT(r, r+r, a);
    R = DAG.getNode(ISD::VSELECT, dl, VT, OpVSel,
                    DAG.getNode(ISD::ADD, dl, VT, R, R), R);
    return R;
  }

  // It's worth extending once and using the v8i32 shifts for 16-bit types, but
  // the extra overheads to get from v16i8 to v8i32 make the existing SSE
  // solution better.
  if (Subtarget->hasInt256() && VT == MVT::v8i16) {
    MVT NewVT = VT == MVT::v8i16 ? MVT::v8i32 : MVT::v16i16;
    unsigned ExtOpc =
        Op.getOpcode() == ISD::SRA ? ISD::SIGN_EXTEND : ISD::ZERO_EXTEND;
    R = DAG.getNode(ExtOpc, dl, NewVT, R);
    Amt = DAG.getNode(ISD::ANY_EXTEND, dl, NewVT, Amt);
    return DAG.getNode(ISD::TRUNCATE, dl, VT,
                       DAG.getNode(Op.getOpcode(), dl, NewVT, R, Amt));
    }

  // Decompose 256-bit shifts into smaller 128-bit shifts.
  if (VT.is256BitVector()) {
    unsigned NumElems = VT.getVectorNumElements();
    MVT EltVT = VT.getVectorElementType();
    EVT NewVT = MVT::getVectorVT(EltVT, NumElems/2);

    // Extract the two vectors
    SDValue V1 = Extract128BitVector(R, 0, DAG, dl);
    SDValue V2 = Extract128BitVector(R, NumElems/2, DAG, dl);

    // Recreate the shift amount vectors
    SDValue Amt1, Amt2;
    if (Amt.getOpcode() == ISD::BUILD_VECTOR) {
      // Constant shift amount
      SmallVector<SDValue, 4> Amt1Csts;
      SmallVector<SDValue, 4> Amt2Csts;
      for (unsigned i = 0; i != NumElems/2; ++i)
        Amt1Csts.push_back(Amt->getOperand(i));
      for (unsigned i = NumElems/2; i != NumElems; ++i)
        Amt2Csts.push_back(Amt->getOperand(i));

      Amt1 = DAG.getNode(ISD::BUILD_VECTOR, dl, NewVT, Amt1Csts);
      Amt2 = DAG.getNode(ISD::BUILD_VECTOR, dl, NewVT, Amt2Csts);
    } else {
      // Variable shift amount
      Amt1 = Extract128BitVector(Amt, 0, DAG, dl);
      Amt2 = Extract128BitVector(Amt, NumElems/2, DAG, dl);
    }

    // Issue new vector shifts for the smaller types
    V1 = DAG.getNode(Op.getOpcode(), dl, NewVT, V1, Amt1);
    V2 = DAG.getNode(Op.getOpcode(), dl, NewVT, V2, Amt2);

    // Concatenate the result back
    return DAG.getNode(ISD::CONCAT_VECTORS, dl, VT, V1, V2);
  }

  return SDValue();
}

static SDValue LowerXALUO(SDValue Op, SelectionDAG &DAG) {
  // Lower the "add/sub/mul with overflow" instruction into a regular ins plus
  // a "setcc" instruction that checks the overflow flag. The "brcond" lowering
  // looks for this combo and may remove the "setcc" instruction if the "setcc"
  // has only one use.
  SDNode *N = Op.getNode();
  SDValue LHS = N->getOperand(0);
  SDValue RHS = N->getOperand(1);
  unsigned BaseOp = 0;
  unsigned Cond = 0;
  SDLoc DL(Op);
  switch (Op.getOpcode()) {
  default: llvm_unreachable("Unknown ovf instruction!");
  case ISD::SADDO:
    // A subtract of one will be selected as a INC. Note that INC doesn't
    // set CF, so we can't do this for UADDO.
    if (ConstantSDNode *C = dyn_cast<ConstantSDNode>(RHS))
      if (C->isOne()) {
        BaseOp = X86ISD::INC;
        Cond = X86::COND_O;
        break;
      }
    BaseOp = X86ISD::ADD;
    Cond = X86::COND_O;
    break;
  case ISD::UADDO:
    BaseOp = X86ISD::ADD;
    Cond = X86::COND_B;
    break;
  case ISD::SSUBO:
    // A subtract of one will be selected as a DEC. Note that DEC doesn't
    // set CF, so we can't do this for USUBO.
    if (ConstantSDNode *C = dyn_cast<ConstantSDNode>(RHS))
      if (C->isOne()) {
        BaseOp = X86ISD::DEC;
        Cond = X86::COND_O;
        break;
      }
    BaseOp = X86ISD::SUB;
    Cond = X86::COND_O;
    break;
  case ISD::USUBO:
    BaseOp = X86ISD::SUB;
    Cond = X86::COND_B;
    break;
  case ISD::SMULO:
    BaseOp = X86ISD::SMUL;
    Cond = X86::COND_O;
    break;
  case ISD::UMULO: { // i64, i8 = umulo lhs, rhs --> i64, i64, i32 umul lhs,rhs
    SDVTList VTs = DAG.getVTList(N->getValueType(0), N->getValueType(0),
                                 MVT::i32);
    SDValue Sum = DAG.getNode(X86ISD::UMUL, DL, VTs, LHS, RHS);

    SDValue SetCC =
      DAG.getNode(X86ISD::SETCC, DL, MVT::i8,
                  DAG.getConstant(X86::COND_O, MVT::i32),
                  SDValue(Sum.getNode(), 2));

    return DAG.getNode(ISD::MERGE_VALUES, DL, N->getVTList(), Sum, SetCC);
  }
  }

  // Also sets EFLAGS.
  SDVTList VTs = DAG.getVTList(N->getValueType(0), MVT::i32);
  SDValue Sum = DAG.getNode(BaseOp, DL, VTs, LHS, RHS);

  SDValue SetCC =
    DAG.getNode(X86ISD::SETCC, DL, N->getValueType(1),
                DAG.getConstant(Cond, MVT::i32),
                SDValue(Sum.getNode(), 1));

  return DAG.getNode(ISD::MERGE_VALUES, DL, N->getVTList(), Sum, SetCC);
}

// Sign extension of the low part of vector elements. This may be used either
// when sign extend instructions are not available or if the vector element
// sizes already match the sign-extended size. If the vector elements are in
// their pre-extended size and sign extend instructions are available, that will
// be handled by LowerSIGN_EXTEND.
SDValue X86TargetLowering::LowerSIGN_EXTEND_INREG(SDValue Op,
                                                  SelectionDAG &DAG) const {
  SDLoc dl(Op);
  EVT ExtraVT = cast<VTSDNode>(Op.getOperand(1))->getVT();
  MVT VT = Op.getSimpleValueType();

  if (!Subtarget->hasSSE2() || !VT.isVector())
    return SDValue();

  unsigned BitsDiff = VT.getScalarType().getSizeInBits() -
                      ExtraVT.getScalarType().getSizeInBits();

  switch (VT.SimpleTy) {
    default: return SDValue();
    case MVT::v8i32:
    case MVT::v16i16:
      if (!Subtarget->hasFp256())
        return SDValue();
      if (!Subtarget->hasInt256()) {
        // needs to be split
        unsigned NumElems = VT.getVectorNumElements();

        // Extract the LHS vectors
        SDValue LHS = Op.getOperand(0);
        SDValue LHS1 = Extract128BitVector(LHS, 0, DAG, dl);
        SDValue LHS2 = Extract128BitVector(LHS, NumElems/2, DAG, dl);

        MVT EltVT = VT.getVectorElementType();
        EVT NewVT = MVT::getVectorVT(EltVT, NumElems/2);

        EVT ExtraEltVT = ExtraVT.getVectorElementType();
        unsigned ExtraNumElems = ExtraVT.getVectorNumElements();
        ExtraVT = EVT::getVectorVT(*DAG.getContext(), ExtraEltVT,
                                   ExtraNumElems/2);
        SDValue Extra = DAG.getValueType(ExtraVT);

        LHS1 = DAG.getNode(Op.getOpcode(), dl, NewVT, LHS1, Extra);
        LHS2 = DAG.getNode(Op.getOpcode(), dl, NewVT, LHS2, Extra);

        return DAG.getNode(ISD::CONCAT_VECTORS, dl, VT, LHS1, LHS2);
      }
      // fall through
    case MVT::v4i32:
    case MVT::v8i16: {
      SDValue Op0 = Op.getOperand(0);

      // This is a sign extension of some low part of vector elements without
      // changing the size of the vector elements themselves:
      // Shift-Left + Shift-Right-Algebraic.
      SDValue Shl = getTargetVShiftByConstNode(X86ISD::VSHLI, dl, VT, Op0,
                                               BitsDiff, DAG);
      return getTargetVShiftByConstNode(X86ISD::VSRAI, dl, VT, Shl, BitsDiff,
                                        DAG);
    }
  }
}

static SDValue LowerATOMIC_FENCE(SDValue Op, const X86Subtarget *Subtarget,
                                 SelectionDAG &DAG) {
  SDLoc dl(Op);
  AtomicOrdering FenceOrdering = static_cast<AtomicOrdering>(
    cast<ConstantSDNode>(Op.getOperand(1))->getZExtValue());
  SynchronizationScope FenceScope = static_cast<SynchronizationScope>(
    cast<ConstantSDNode>(Op.getOperand(2))->getZExtValue());

  // The only fence that needs an instruction is a sequentially-consistent
  // cross-thread fence.
  if (FenceOrdering == SequentiallyConsistent && FenceScope == CrossThread) {
    // Use mfence if we have SSE2 or we're on x86-64 (even if we asked for
    // no-sse2). There isn't any reason to disable it if the target processor
    // supports it.
    if (Subtarget->hasSSE2() || Subtarget->is64Bit())
      return DAG.getNode(X86ISD::MFENCE, dl, MVT::Other, Op.getOperand(0));

    SDValue Chain = Op.getOperand(0);
    SDValue Zero = DAG.getConstant(0, MVT::i32);
    SDValue Ops[] = {
      DAG.getRegister(X86::ESP, MVT::i32), // Base
      DAG.getTargetConstant(1, MVT::i8),   // Scale
      DAG.getRegister(0, MVT::i32),        // Index
      DAG.getTargetConstant(0, MVT::i32),  // Disp
      DAG.getRegister(0, MVT::i32),        // Segment.
      Zero,
      Chain
    };
    SDNode *Res = DAG.getMachineNode(X86::OR32mrLocked, dl, MVT::Other, Ops);
    return SDValue(Res, 0);
  }

  // MEMBARRIER is a compiler barrier; it codegens to a no-op.
  return DAG.getNode(X86ISD::MEMBARRIER, dl, MVT::Other, Op.getOperand(0));
}

static SDValue LowerCMP_SWAP(SDValue Op, const X86Subtarget *Subtarget,
                             SelectionDAG &DAG) {
  MVT T = Op.getSimpleValueType();
  SDLoc DL(Op);
  unsigned Reg = 0;
  unsigned size = 0;
  switch(T.SimpleTy) {
  default: llvm_unreachable("Invalid value type!");
  case MVT::i8:  Reg = X86::AL;  size = 1; break;
  case MVT::i16: Reg = X86::AX;  size = 2; break;
  case MVT::i32: Reg = X86::EAX; size = 4; break;
  case MVT::i64:
    assert(Subtarget->is64Bit() && "Node not type legal!");
    Reg = X86::RAX; size = 8;
    break;
  }
  SDValue cpIn = DAG.getCopyToReg(Op.getOperand(0), DL, Reg,
                                  Op.getOperand(2), SDValue());
  SDValue Ops[] = { cpIn.getValue(0),
                    Op.getOperand(1),
                    Op.getOperand(3),
                    DAG.getTargetConstant(size, MVT::i8),
                    cpIn.getValue(1) };
  SDVTList Tys = DAG.getVTList(MVT::Other, MVT::Glue);
  MachineMemOperand *MMO = cast<AtomicSDNode>(Op)->getMemOperand();
  SDValue Result = DAG.getMemIntrinsicNode(X86ISD::LCMPXCHG_DAG, DL, Tys,
                                           Ops, T, MMO);

  SDValue cpOut =
    DAG.getCopyFromReg(Result.getValue(0), DL, Reg, T, Result.getValue(1));
  SDValue EFLAGS = DAG.getCopyFromReg(cpOut.getValue(1), DL, X86::EFLAGS,
                                      MVT::i32, cpOut.getValue(2));
  SDValue Success = DAG.getNode(X86ISD::SETCC, DL, Op->getValueType(1),
                                DAG.getConstant(X86::COND_E, MVT::i8), EFLAGS);

  DAG.ReplaceAllUsesOfValueWith(Op.getValue(0), cpOut);
  DAG.ReplaceAllUsesOfValueWith(Op.getValue(1), Success);
  DAG.ReplaceAllUsesOfValueWith(Op.getValue(2), EFLAGS.getValue(1));
  return SDValue();
}

static SDValue LowerBITCAST(SDValue Op, const X86Subtarget *Subtarget,
                            SelectionDAG &DAG) {
  MVT SrcVT = Op.getOperand(0).getSimpleValueType();
  MVT DstVT = Op.getSimpleValueType();

  if (SrcVT == MVT::v2i32 || SrcVT == MVT::v4i16 || SrcVT == MVT::v8i8) {
    assert(Subtarget->hasSSE2() && "Requires at least SSE2!");
    if (DstVT != MVT::f64)
      // This conversion needs to be expanded.
      return SDValue();

    SDValue InVec = Op->getOperand(0);
    SDLoc dl(Op);
    unsigned NumElts = SrcVT.getVectorNumElements();
    EVT SVT = SrcVT.getVectorElementType();

    // Widen the vector in input in the case of MVT::v2i32.
    // Example: from MVT::v2i32 to MVT::v4i32.
    SmallVector<SDValue, 16> Elts;
    for (unsigned i = 0, e = NumElts; i != e; ++i)
      Elts.push_back(DAG.getNode(ISD::EXTRACT_VECTOR_ELT, dl, SVT, InVec,
                                 DAG.getIntPtrConstant(i)));

    // Explicitly mark the extra elements as Undef.
    SDValue Undef = DAG.getUNDEF(SVT);
    for (unsigned i = NumElts, e = NumElts * 2; i != e; ++i)
      Elts.push_back(Undef);

    EVT NewVT = EVT::getVectorVT(*DAG.getContext(), SVT, NumElts * 2);
    SDValue BV = DAG.getNode(ISD::BUILD_VECTOR, dl, NewVT, Elts);
    SDValue ToV2F64 = DAG.getNode(ISD::BITCAST, dl, MVT::v2f64, BV);
    return DAG.getNode(ISD::EXTRACT_VECTOR_ELT, dl, MVT::f64, ToV2F64,
                       DAG.getIntPtrConstant(0));
  }

  assert(Subtarget->is64Bit() && !Subtarget->hasSSE2() &&
         Subtarget->hasMMX() && "Unexpected custom BITCAST");
  assert((DstVT == MVT::i64 ||
          (DstVT.isVector() && DstVT.getSizeInBits()==64)) &&
         "Unexpected custom BITCAST");
  // i64 <=> MMX conversions are Legal.
  if (SrcVT==MVT::i64 && DstVT.isVector())
    return Op;
  if (DstVT==MVT::i64 && SrcVT.isVector())
    return Op;
  // MMX <=> MMX conversions are Legal.
  if (SrcVT.isVector() && DstVT.isVector())
    return Op;
  // All other conversions need to be expanded.
  return SDValue();
}

static SDValue LowerLOAD_SUB(SDValue Op, SelectionDAG &DAG) {
  SDNode *Node = Op.getNode();
  SDLoc dl(Node);
  EVT T = Node->getValueType(0);
  SDValue negOp = DAG.getNode(ISD::SUB, dl, T,
                              DAG.getConstant(0, T), Node->getOperand(2));
  return DAG.getAtomic(ISD::ATOMIC_LOAD_ADD, dl,
                       cast<AtomicSDNode>(Node)->getMemoryVT(),
                       Node->getOperand(0),
                       Node->getOperand(1), negOp,
                       cast<AtomicSDNode>(Node)->getMemOperand(),
                       cast<AtomicSDNode>(Node)->getOrdering(),
                       cast<AtomicSDNode>(Node)->getSynchScope());
}

static SDValue LowerATOMIC_STORE(SDValue Op, SelectionDAG &DAG) {
  SDNode *Node = Op.getNode();
  SDLoc dl(Node);
  EVT VT = cast<AtomicSDNode>(Node)->getMemoryVT();

  // Convert seq_cst store -> xchg
  // Convert wide store -> swap (-> cmpxchg8b/cmpxchg16b)
  // FIXME: On 32-bit, store -> fist or movq would be more efficient
  //        (The only way to get a 16-byte store is cmpxchg16b)
  // FIXME: 16-byte ATOMIC_SWAP isn't actually hooked up at the moment.
  if (cast<AtomicSDNode>(Node)->getOrdering() == SequentiallyConsistent ||
      !DAG.getTargetLoweringInfo().isTypeLegal(VT)) {
    SDValue Swap = DAG.getAtomic(ISD::ATOMIC_SWAP, dl,
                                 cast<AtomicSDNode>(Node)->getMemoryVT(),
                                 Node->getOperand(0),
                                 Node->getOperand(1), Node->getOperand(2),
                                 cast<AtomicSDNode>(Node)->getMemOperand(),
                                 cast<AtomicSDNode>(Node)->getOrdering(),
                                 cast<AtomicSDNode>(Node)->getSynchScope());
    return Swap.getValue(1);
  }
  // Other atomic stores have a simple pattern.
  return Op;
}

static SDValue LowerADDC_ADDE_SUBC_SUBE(SDValue Op, SelectionDAG &DAG) {
  EVT VT = Op.getNode()->getSimpleValueType(0);

  // Let legalize expand this if it isn't a legal type yet.
  if (!DAG.getTargetLoweringInfo().isTypeLegal(VT))
    return SDValue();

  SDVTList VTs = DAG.getVTList(VT, MVT::i32);

  unsigned Opc;
  bool ExtraOp = false;
  switch (Op.getOpcode()) {
  default: llvm_unreachable("Invalid code");
  case ISD::ADDC: Opc = X86ISD::ADD; break;
  case ISD::ADDE: Opc = X86ISD::ADC; ExtraOp = true; break;
  case ISD::SUBC: Opc = X86ISD::SUB; break;
  case ISD::SUBE: Opc = X86ISD::SBB; ExtraOp = true; break;
  }

  if (!ExtraOp)
    return DAG.getNode(Opc, SDLoc(Op), VTs, Op.getOperand(0),
                       Op.getOperand(1));
  return DAG.getNode(Opc, SDLoc(Op), VTs, Op.getOperand(0),
                     Op.getOperand(1), Op.getOperand(2));
}

static SDValue LowerFSINCOS(SDValue Op, const X86Subtarget *Subtarget,
                            SelectionDAG &DAG) {
  assert(Subtarget->isTargetDarwin() && Subtarget->is64Bit());

  // For MacOSX, we want to call an alternative entry point: __sincos_stret,
  // which returns the values as { float, float } (in XMM0) or
  // { double, double } (which is returned in XMM0, XMM1).
  SDLoc dl(Op);
  SDValue Arg = Op.getOperand(0);
  EVT ArgVT = Arg.getValueType();
  Type *ArgTy = ArgVT.getTypeForEVT(*DAG.getContext());

  TargetLowering::ArgListTy Args;
  TargetLowering::ArgListEntry Entry;

  Entry.Node = Arg;
  Entry.Ty = ArgTy;
  Entry.isSExt = false;
  Entry.isZExt = false;
  Args.push_back(Entry);

  bool isF64 = ArgVT == MVT::f64;
  // Only optimize x86_64 for now. i386 is a bit messy. For f32,
  // the small struct {f32, f32} is returned in (eax, edx). For f64,
  // the results are returned via SRet in memory.
  const char *LibcallName =  isF64 ? "__sincos_stret" : "__sincosf_stret";
  const TargetLowering &TLI = DAG.getTargetLoweringInfo();
  SDValue Callee = DAG.getExternalSymbol(LibcallName, TLI.getPointerTy());

  Type *RetTy = isF64
    ? (Type*)StructType::get(ArgTy, ArgTy, NULL)
    : (Type*)VectorType::get(ArgTy, 4);

  TargetLowering::CallLoweringInfo CLI(DAG);
  CLI.setDebugLoc(dl).setChain(DAG.getEntryNode())
    .setCallee(CallingConv::C, RetTy, Callee, std::move(Args), 0);

  std::pair<SDValue, SDValue> CallResult = TLI.LowerCallTo(CLI);

  if (isF64)
    // Returned in xmm0 and xmm1.
    return CallResult.first;

  // Returned in bits 0:31 and 32:64 xmm0.
  SDValue SinVal = DAG.getNode(ISD::EXTRACT_VECTOR_ELT, dl, ArgVT,
                               CallResult.first, DAG.getIntPtrConstant(0));
  SDValue CosVal = DAG.getNode(ISD::EXTRACT_VECTOR_ELT, dl, ArgVT,
                               CallResult.first, DAG.getIntPtrConstant(1));
  SDVTList Tys = DAG.getVTList(ArgVT, ArgVT);
  return DAG.getNode(ISD::MERGE_VALUES, dl, Tys, SinVal, CosVal);
}

/// LowerOperation - Provide custom lowering hooks for some operations.
///
SDValue X86TargetLowering::LowerOperation(SDValue Op, SelectionDAG &DAG) const {
  switch (Op.getOpcode()) {
  default: llvm_unreachable("Should not custom lower this!");
  case ISD::SIGN_EXTEND_INREG:  return LowerSIGN_EXTEND_INREG(Op,DAG);
  case ISD::ATOMIC_FENCE:       return LowerATOMIC_FENCE(Op, Subtarget, DAG);
  case ISD::ATOMIC_CMP_SWAP_WITH_SUCCESS:
    return LowerCMP_SWAP(Op, Subtarget, DAG);
  case ISD::ATOMIC_LOAD_SUB:    return LowerLOAD_SUB(Op,DAG);
  case ISD::ATOMIC_STORE:       return LowerATOMIC_STORE(Op,DAG);
  case ISD::BUILD_VECTOR:       return LowerBUILD_VECTOR(Op, DAG);
  case ISD::CONCAT_VECTORS:     return LowerCONCAT_VECTORS(Op, DAG);
  case ISD::VECTOR_SHUFFLE:     return LowerVECTOR_SHUFFLE(Op, DAG);
  case ISD::VSELECT:            return LowerVSELECT(Op, DAG);
  case ISD::EXTRACT_VECTOR_ELT: return LowerEXTRACT_VECTOR_ELT(Op, DAG);
  case ISD::INSERT_VECTOR_ELT:  return LowerINSERT_VECTOR_ELT(Op, DAG);
  case ISD::EXTRACT_SUBVECTOR:  return LowerEXTRACT_SUBVECTOR(Op,Subtarget,DAG);
  case ISD::INSERT_SUBVECTOR:   return LowerINSERT_SUBVECTOR(Op, Subtarget,DAG);
  case ISD::SCALAR_TO_VECTOR:   return LowerSCALAR_TO_VECTOR(Op, DAG);
  case ISD::ConstantPool:       return LowerConstantPool(Op, DAG);
  case ISD::GlobalAddress:      return LowerGlobalAddress(Op, DAG);
  case ISD::GlobalTLSAddress:   return LowerGlobalTLSAddress(Op, DAG);
  case ISD::ExternalSymbol:     return LowerExternalSymbol(Op, DAG);
  case ISD::BlockAddress:       return LowerBlockAddress(Op, DAG);
  case ISD::SHL_PARTS:
  case ISD::SRA_PARTS:
  case ISD::SRL_PARTS:          return LowerShiftParts(Op, DAG);
  case ISD::SINT_TO_FP:         return LowerSINT_TO_FP(Op, DAG);
  case ISD::UINT_TO_FP:         return LowerUINT_TO_FP(Op, DAG);
  case ISD::TRUNCATE:           return LowerTRUNCATE(Op, DAG);
  case ISD::ZERO_EXTEND:        return LowerZERO_EXTEND(Op, Subtarget, DAG);
  case ISD::SIGN_EXTEND:        return LowerSIGN_EXTEND(Op, Subtarget, DAG);
  case ISD::ANY_EXTEND:         return LowerANY_EXTEND(Op, Subtarget, DAG);
  case ISD::FP_TO_SINT:         return LowerFP_TO_SINT(Op, DAG);
  case ISD::FP_TO_UINT:         return LowerFP_TO_UINT(Op, DAG);
  case ISD::FP_EXTEND:          return LowerFP_EXTEND(Op, DAG);
  case ISD::LOAD:               return LowerExtendedLoad(Op, Subtarget, DAG);
  case ISD::FABS:               return LowerFABS(Op, DAG);
  case ISD::FNEG:               return LowerFNEG(Op, DAG);
  case ISD::FCOPYSIGN:          return LowerFCOPYSIGN(Op, DAG);
  case ISD::FGETSIGN:           return LowerFGETSIGN(Op, DAG);
  case ISD::SETCC:              return LowerSETCC(Op, DAG);
  case ISD::SELECT:             return LowerSELECT(Op, DAG);
  case ISD::BRCOND:             return LowerBRCOND(Op, DAG);
  case ISD::JumpTable:          return LowerJumpTable(Op, DAG);
  case ISD::VASTART:            return LowerVASTART(Op, DAG);
  case ISD::VAARG:              return LowerVAARG(Op, DAG);
  case ISD::VACOPY:             return LowerVACOPY(Op, Subtarget, DAG);
  case ISD::INTRINSIC_WO_CHAIN: return LowerINTRINSIC_WO_CHAIN(Op, DAG);
  case ISD::INTRINSIC_VOID:
  case ISD::INTRINSIC_W_CHAIN:  return LowerINTRINSIC_W_CHAIN(Op, Subtarget, DAG);
  case ISD::RETURNADDR:         return LowerRETURNADDR(Op, DAG);
  case ISD::FRAMEADDR:          return LowerFRAMEADDR(Op, DAG);
  case ISD::FRAME_TO_ARGS_OFFSET:
                                return LowerFRAME_TO_ARGS_OFFSET(Op, DAG);
  case ISD::DYNAMIC_STACKALLOC: return LowerDYNAMIC_STACKALLOC(Op, DAG);
  case ISD::EH_RETURN:          return LowerEH_RETURN(Op, DAG);
  case ISD::EH_SJLJ_SETJMP:     return lowerEH_SJLJ_SETJMP(Op, DAG);
  case ISD::EH_SJLJ_LONGJMP:    return lowerEH_SJLJ_LONGJMP(Op, DAG);
  case ISD::INIT_TRAMPOLINE:    return LowerINIT_TRAMPOLINE(Op, DAG);
  case ISD::ADJUST_TRAMPOLINE:  return LowerADJUST_TRAMPOLINE(Op, DAG);
  case ISD::FLT_ROUNDS_:        return LowerFLT_ROUNDS_(Op, DAG);
  case ISD::CTLZ:               return LowerCTLZ(Op, DAG);
  case ISD::CTLZ_ZERO_UNDEF:    return LowerCTLZ_ZERO_UNDEF(Op, DAG);
  case ISD::CTTZ:               return LowerCTTZ(Op, DAG);
  case ISD::MUL:                return LowerMUL(Op, Subtarget, DAG);
  case ISD::UMUL_LOHI:
  case ISD::SMUL_LOHI:          return LowerMUL_LOHI(Op, Subtarget, DAG);
  case ISD::SRA:
  case ISD::SRL:
  case ISD::SHL:                return LowerShift(Op, Subtarget, DAG);
  case ISD::SADDO:
  case ISD::UADDO:
  case ISD::SSUBO:
  case ISD::USUBO:
  case ISD::SMULO:
  case ISD::UMULO:              return LowerXALUO(Op, DAG);
  case ISD::READCYCLECOUNTER:   return LowerREADCYCLECOUNTER(Op, Subtarget,DAG);
  case ISD::BITCAST:            return LowerBITCAST(Op, Subtarget, DAG);
  case ISD::ADDC:
  case ISD::ADDE:
  case ISD::SUBC:
  case ISD::SUBE:               return LowerADDC_ADDE_SUBC_SUBE(Op, DAG);
  case ISD::ADD:                return LowerADD(Op, DAG);
  case ISD::SUB:                return LowerSUB(Op, DAG);
  case ISD::FSINCOS:            return LowerFSINCOS(Op, Subtarget, DAG);
  }
}

static void ReplaceATOMIC_LOAD(SDNode *Node,
                               SmallVectorImpl<SDValue> &Results,
                               SelectionDAG &DAG) {
  SDLoc dl(Node);
  EVT VT = cast<AtomicSDNode>(Node)->getMemoryVT();

  // Convert wide load -> cmpxchg8b/cmpxchg16b
  // FIXME: On 32-bit, load -> fild or movq would be more efficient
  //        (The only way to get a 16-byte load is cmpxchg16b)
  // FIXME: 16-byte ATOMIC_CMP_SWAP isn't actually hooked up at the moment.
  SDValue Zero = DAG.getConstant(0, VT);
  SDVTList VTs = DAG.getVTList(VT, MVT::i1, MVT::Other);
  SDValue Swap =
      DAG.getAtomicCmpSwap(ISD::ATOMIC_CMP_SWAP_WITH_SUCCESS, dl, VT, VTs,
                           Node->getOperand(0), Node->getOperand(1), Zero, Zero,
                           cast<AtomicSDNode>(Node)->getMemOperand(),
                           cast<AtomicSDNode>(Node)->getOrdering(),
                           cast<AtomicSDNode>(Node)->getOrdering(),
                           cast<AtomicSDNode>(Node)->getSynchScope());
  Results.push_back(Swap.getValue(0));
  Results.push_back(Swap.getValue(2));
}

/// ReplaceNodeResults - Replace a node with an illegal result type
/// with a new node built out of custom code.
void X86TargetLowering::ReplaceNodeResults(SDNode *N,
                                           SmallVectorImpl<SDValue>&Results,
                                           SelectionDAG &DAG) const {
  SDLoc dl(N);
  const TargetLowering &TLI = DAG.getTargetLoweringInfo();
  switch (N->getOpcode()) {
  default:
    llvm_unreachable("Do not know how to custom type legalize this operation!");
  case ISD::SIGN_EXTEND_INREG:
  case ISD::ADDC:
  case ISD::ADDE:
  case ISD::SUBC:
  case ISD::SUBE:
    // We don't want to expand or promote these.
    return;
  case ISD::SDIV:
  case ISD::UDIV:
  case ISD::SREM:
  case ISD::UREM:
  case ISD::SDIVREM:
  case ISD::UDIVREM: {
    SDValue V = LowerWin64_i128OP(SDValue(N,0), DAG);
    Results.push_back(V);
    return;
  }
  case ISD::FP_TO_SINT:
  case ISD::FP_TO_UINT: {
    bool IsSigned = N->getOpcode() == ISD::FP_TO_SINT;

    if (!IsSigned && !isIntegerTypeFTOL(SDValue(N, 0).getValueType()))
      return;

    std::pair<SDValue,SDValue> Vals =
        FP_TO_INTHelper(SDValue(N, 0), DAG, IsSigned, /*IsReplace=*/ true);
    SDValue FIST = Vals.first, StackSlot = Vals.second;
    if (FIST.getNode()) {
      EVT VT = N->getValueType(0);
      // Return a load from the stack slot.
      if (StackSlot.getNode())
        Results.push_back(DAG.getLoad(VT, dl, FIST, StackSlot,
                                      MachinePointerInfo(),
                                      false, false, false, 0));
      else
        Results.push_back(FIST);
    }
    return;
  }
  case ISD::UINT_TO_FP: {
    assert(Subtarget->hasSSE2() && "Requires at least SSE2!");
    if (N->getOperand(0).getValueType() != MVT::v2i32 ||
        N->getValueType(0) != MVT::v2f32)
      return;
    SDValue ZExtIn = DAG.getNode(ISD::ZERO_EXTEND, dl, MVT::v2i64,
                                 N->getOperand(0));
    SDValue Bias = DAG.getConstantFP(BitsToDouble(0x4330000000000000ULL),
                                     MVT::f64);
    SDValue VBias = DAG.getNode(ISD::BUILD_VECTOR, dl, MVT::v2f64, Bias, Bias);
    SDValue Or = DAG.getNode(ISD::OR, dl, MVT::v2i64, ZExtIn,
                             DAG.getNode(ISD::BITCAST, dl, MVT::v2i64, VBias));
    Or = DAG.getNode(ISD::BITCAST, dl, MVT::v2f64, Or);
    SDValue Sub = DAG.getNode(ISD::FSUB, dl, MVT::v2f64, Or, VBias);
    Results.push_back(DAG.getNode(X86ISD::VFPROUND, dl, MVT::v4f32, Sub));
    return;
  }
  case ISD::FP_ROUND: {
    if (!TLI.isTypeLegal(N->getOperand(0).getValueType()))
        return;
    SDValue V = DAG.getNode(X86ISD::VFPROUND, dl, MVT::v4f32, N->getOperand(0));
    Results.push_back(V);
    return;
  }
  case ISD::INTRINSIC_W_CHAIN: {
    unsigned IntNo = cast<ConstantSDNode>(N->getOperand(1))->getZExtValue();
    switch (IntNo) {
    default : llvm_unreachable("Do not know how to custom type "
                               "legalize this intrinsic operation!");
    case Intrinsic::x86_rdtsc:
      return getReadTimeStampCounter(N, dl, X86ISD::RDTSC_DAG, DAG, Subtarget,
                                     Results);
    case Intrinsic::x86_rdtscp:
      return getReadTimeStampCounter(N, dl, X86ISD::RDTSCP_DAG, DAG, Subtarget,
                                     Results);
    case Intrinsic::x86_rdpmc:
      return getReadPerformanceCounter(N, dl, DAG, Subtarget, Results);
    }
  }
  case ISD::READCYCLECOUNTER: {
    return getReadTimeStampCounter(N, dl, X86ISD::RDTSC_DAG, DAG, Subtarget,
                                   Results);
  }
  case ISD::ATOMIC_CMP_SWAP_WITH_SUCCESS: {
    EVT T = N->getValueType(0);
    assert((T == MVT::i64 || T == MVT::i128) && "can only expand cmpxchg pair");
    bool Regs64bit = T == MVT::i128;
    EVT HalfT = Regs64bit ? MVT::i64 : MVT::i32;
    SDValue cpInL, cpInH;
    cpInL = DAG.getNode(ISD::EXTRACT_ELEMENT, dl, HalfT, N->getOperand(2),
                        DAG.getConstant(0, HalfT));
    cpInH = DAG.getNode(ISD::EXTRACT_ELEMENT, dl, HalfT, N->getOperand(2),
                        DAG.getConstant(1, HalfT));
    cpInL = DAG.getCopyToReg(N->getOperand(0), dl,
                             Regs64bit ? X86::RAX : X86::EAX,
                             cpInL, SDValue());
    cpInH = DAG.getCopyToReg(cpInL.getValue(0), dl,
                             Regs64bit ? X86::RDX : X86::EDX,
                             cpInH, cpInL.getValue(1));
    SDValue swapInL, swapInH;
    swapInL = DAG.getNode(ISD::EXTRACT_ELEMENT, dl, HalfT, N->getOperand(3),
                          DAG.getConstant(0, HalfT));
    swapInH = DAG.getNode(ISD::EXTRACT_ELEMENT, dl, HalfT, N->getOperand(3),
                          DAG.getConstant(1, HalfT));
    swapInL = DAG.getCopyToReg(cpInH.getValue(0), dl,
                               Regs64bit ? X86::RBX : X86::EBX,
                               swapInL, cpInH.getValue(1));
    swapInH = DAG.getCopyToReg(swapInL.getValue(0), dl,
                               Regs64bit ? X86::RCX : X86::ECX,
                               swapInH, swapInL.getValue(1));
    SDValue Ops[] = { swapInH.getValue(0),
                      N->getOperand(1),
                      swapInH.getValue(1) };
    SDVTList Tys = DAG.getVTList(MVT::Other, MVT::Glue);
    MachineMemOperand *MMO = cast<AtomicSDNode>(N)->getMemOperand();
    unsigned Opcode = Regs64bit ? X86ISD::LCMPXCHG16_DAG :
                                  X86ISD::LCMPXCHG8_DAG;
    SDValue Result = DAG.getMemIntrinsicNode(Opcode, dl, Tys, Ops, T, MMO);
    SDValue cpOutL = DAG.getCopyFromReg(Result.getValue(0), dl,
                                        Regs64bit ? X86::RAX : X86::EAX,
                                        HalfT, Result.getValue(1));
    SDValue cpOutH = DAG.getCopyFromReg(cpOutL.getValue(1), dl,
                                        Regs64bit ? X86::RDX : X86::EDX,
                                        HalfT, cpOutL.getValue(2));
    SDValue OpsF[] = { cpOutL.getValue(0), cpOutH.getValue(0)};

    SDValue EFLAGS = DAG.getCopyFromReg(cpOutH.getValue(1), dl, X86::EFLAGS,
                                        MVT::i32, cpOutH.getValue(2));
    SDValue Success =
        DAG.getNode(X86ISD::SETCC, dl, MVT::i8,
                    DAG.getConstant(X86::COND_E, MVT::i8), EFLAGS);
    Success = DAG.getZExtOrTrunc(Success, dl, N->getValueType(1));

    Results.push_back(DAG.getNode(ISD::BUILD_PAIR, dl, T, OpsF));
    Results.push_back(Success);
    Results.push_back(EFLAGS.getValue(1));
    return;
  }
  case ISD::ATOMIC_SWAP:
  case ISD::ATOMIC_LOAD_ADD:
  case ISD::ATOMIC_LOAD_SUB:
  case ISD::ATOMIC_LOAD_AND:
  case ISD::ATOMIC_LOAD_OR:
  case ISD::ATOMIC_LOAD_XOR:
  case ISD::ATOMIC_LOAD_NAND:
  case ISD::ATOMIC_LOAD_MIN:
  case ISD::ATOMIC_LOAD_MAX:
  case ISD::ATOMIC_LOAD_UMIN:
  case ISD::ATOMIC_LOAD_UMAX:
    // Delegate to generic TypeLegalization. Situations we can really handle
    // should have already been dealt with by X86AtomicExpandPass.cpp.
    break;
  case ISD::ATOMIC_LOAD: {
    ReplaceATOMIC_LOAD(N, Results, DAG);
    return;
  }
  case ISD::BITCAST: {
    assert(Subtarget->hasSSE2() && "Requires at least SSE2!");
    EVT DstVT = N->getValueType(0);
    EVT SrcVT = N->getOperand(0)->getValueType(0);

    if (SrcVT != MVT::f64 ||
        (DstVT != MVT::v2i32 && DstVT != MVT::v4i16 && DstVT != MVT::v8i8))
      return;

    unsigned NumElts = DstVT.getVectorNumElements();
    EVT SVT = DstVT.getVectorElementType();
    EVT WiderVT = EVT::getVectorVT(*DAG.getContext(), SVT, NumElts * 2);
    SDValue Expanded = DAG.getNode(ISD::SCALAR_TO_VECTOR, dl,
                                   MVT::v2f64, N->getOperand(0));
    SDValue ToVecInt = DAG.getNode(ISD::BITCAST, dl, WiderVT, Expanded);

    if (ExperimentalVectorWideningLegalization) {
      // If we are legalizing vectors by widening, we already have the desired
      // legal vector type, just return it.
      Results.push_back(ToVecInt);
      return;
    }

    SmallVector<SDValue, 8> Elts;
    for (unsigned i = 0, e = NumElts; i != e; ++i)
      Elts.push_back(DAG.getNode(ISD::EXTRACT_VECTOR_ELT, dl, SVT,
                                   ToVecInt, DAG.getIntPtrConstant(i)));

    Results.push_back(DAG.getNode(ISD::BUILD_VECTOR, dl, DstVT, Elts));
  }
  }
}

const char *X86TargetLowering::getTargetNodeName(unsigned Opcode) const {
  switch (Opcode) {
  default: return nullptr;
  case X86ISD::BSF:                return "X86ISD::BSF";
  case X86ISD::BSR:                return "X86ISD::BSR";
  case X86ISD::SHLD:               return "X86ISD::SHLD";
  case X86ISD::SHRD:               return "X86ISD::SHRD";
  case X86ISD::FAND:               return "X86ISD::FAND";
  case X86ISD::FANDN:              return "X86ISD::FANDN";
  case X86ISD::FOR:                return "X86ISD::FOR";
  case X86ISD::FXOR:               return "X86ISD::FXOR";
  case X86ISD::FSRL:               return "X86ISD::FSRL";
  case X86ISD::FILD:               return "X86ISD::FILD";
  case X86ISD::FILD_FLAG:          return "X86ISD::FILD_FLAG";
  case X86ISD::FP_TO_INT16_IN_MEM: return "X86ISD::FP_TO_INT16_IN_MEM";
  case X86ISD::FP_TO_INT32_IN_MEM: return "X86ISD::FP_TO_INT32_IN_MEM";
  case X86ISD::FP_TO_INT64_IN_MEM: return "X86ISD::FP_TO_INT64_IN_MEM";
  case X86ISD::FLD:                return "X86ISD::FLD";
  case X86ISD::FST:                return "X86ISD::FST";
  case X86ISD::CALL:               return "X86ISD::CALL";
  case X86ISD::RDTSC_DAG:          return "X86ISD::RDTSC_DAG";
  case X86ISD::RDTSCP_DAG:         return "X86ISD::RDTSCP_DAG";
  case X86ISD::RDPMC_DAG:          return "X86ISD::RDPMC_DAG";
  case X86ISD::BT:                 return "X86ISD::BT";
  case X86ISD::CMP:                return "X86ISD::CMP";
  case X86ISD::COMI:               return "X86ISD::COMI";
  case X86ISD::UCOMI:              return "X86ISD::UCOMI";
  case X86ISD::CMPM:               return "X86ISD::CMPM";
  case X86ISD::CMPMU:              return "X86ISD::CMPMU";
  case X86ISD::SETCC:              return "X86ISD::SETCC";
  case X86ISD::SETCC_CARRY:        return "X86ISD::SETCC_CARRY";
  case X86ISD::FSETCC:             return "X86ISD::FSETCC";
  case X86ISD::CMOV:               return "X86ISD::CMOV";
  case X86ISD::BRCOND:             return "X86ISD::BRCOND";
  case X86ISD::RET_FLAG:           return "X86ISD::RET_FLAG";
  case X86ISD::REP_STOS:           return "X86ISD::REP_STOS";
  case X86ISD::REP_MOVS:           return "X86ISD::REP_MOVS";
  case X86ISD::GlobalBaseReg:      return "X86ISD::GlobalBaseReg";
  case X86ISD::Wrapper:            return "X86ISD::Wrapper";
  case X86ISD::WrapperRIP:         return "X86ISD::WrapperRIP";
  case X86ISD::PEXTRB:             return "X86ISD::PEXTRB";
  case X86ISD::PEXTRW:             return "X86ISD::PEXTRW";
  case X86ISD::INSERTPS:           return "X86ISD::INSERTPS";
  case X86ISD::PINSRB:             return "X86ISD::PINSRB";
  case X86ISD::PINSRW:             return "X86ISD::PINSRW";
  case X86ISD::PSHUFB:             return "X86ISD::PSHUFB";
  case X86ISD::ANDNP:              return "X86ISD::ANDNP";
  case X86ISD::PSIGN:              return "X86ISD::PSIGN";
  case X86ISD::BLENDV:             return "X86ISD::BLENDV";
  case X86ISD::BLENDI:             return "X86ISD::BLENDI";
  case X86ISD::SUBUS:              return "X86ISD::SUBUS";
  case X86ISD::HADD:               return "X86ISD::HADD";
  case X86ISD::HSUB:               return "X86ISD::HSUB";
  case X86ISD::FHADD:              return "X86ISD::FHADD";
  case X86ISD::FHSUB:              return "X86ISD::FHSUB";
  case X86ISD::UMAX:               return "X86ISD::UMAX";
  case X86ISD::UMIN:               return "X86ISD::UMIN";
  case X86ISD::SMAX:               return "X86ISD::SMAX";
  case X86ISD::SMIN:               return "X86ISD::SMIN";
  case X86ISD::FMAX:               return "X86ISD::FMAX";
  case X86ISD::FMIN:               return "X86ISD::FMIN";
  case X86ISD::FMAXC:              return "X86ISD::FMAXC";
  case X86ISD::FMINC:              return "X86ISD::FMINC";
  case X86ISD::FRSQRT:             return "X86ISD::FRSQRT";
  case X86ISD::FRCP:               return "X86ISD::FRCP";
  case X86ISD::TLSADDR:            return "X86ISD::TLSADDR";
  case X86ISD::TLSBASEADDR:        return "X86ISD::TLSBASEADDR";
  case X86ISD::TLSCALL:            return "X86ISD::TLSCALL";
  case X86ISD::EH_SJLJ_SETJMP:     return "X86ISD::EH_SJLJ_SETJMP";
  case X86ISD::EH_SJLJ_LONGJMP:    return "X86ISD::EH_SJLJ_LONGJMP";
  case X86ISD::EH_RETURN:          return "X86ISD::EH_RETURN";
  case X86ISD::TC_RETURN:          return "X86ISD::TC_RETURN";
  case X86ISD::FNSTCW16m:          return "X86ISD::FNSTCW16m";
  case X86ISD::FNSTSW16r:          return "X86ISD::FNSTSW16r";
  case X86ISD::LCMPXCHG_DAG:       return "X86ISD::LCMPXCHG_DAG";
  case X86ISD::LCMPXCHG8_DAG:      return "X86ISD::LCMPXCHG8_DAG";
  case X86ISD::LCMPXCHG16_DAG:     return "X86ISD::LCMPXCHG16_DAG";
  case X86ISD::VZEXT_MOVL:         return "X86ISD::VZEXT_MOVL";
  case X86ISD::VZEXT_LOAD:         return "X86ISD::VZEXT_LOAD";
  case X86ISD::VZEXT:              return "X86ISD::VZEXT";
  case X86ISD::VSEXT:              return "X86ISD::VSEXT";
  case X86ISD::VTRUNC:             return "X86ISD::VTRUNC";
  case X86ISD::VTRUNCM:            return "X86ISD::VTRUNCM";
  case X86ISD::VINSERT:            return "X86ISD::VINSERT";
  case X86ISD::VFPEXT:             return "X86ISD::VFPEXT";
  case X86ISD::VFPROUND:           return "X86ISD::VFPROUND";
  case X86ISD::VSHLDQ:             return "X86ISD::VSHLDQ";
  case X86ISD::VSRLDQ:             return "X86ISD::VSRLDQ";
  case X86ISD::VSHL:               return "X86ISD::VSHL";
  case X86ISD::VSRL:               return "X86ISD::VSRL";
  case X86ISD::VSRA:               return "X86ISD::VSRA";
  case X86ISD::VSHLI:              return "X86ISD::VSHLI";
  case X86ISD::VSRLI:              return "X86ISD::VSRLI";
  case X86ISD::VSRAI:              return "X86ISD::VSRAI";
  case X86ISD::CMPP:               return "X86ISD::CMPP";
  case X86ISD::PCMPEQ:             return "X86ISD::PCMPEQ";
  case X86ISD::PCMPGT:             return "X86ISD::PCMPGT";
  case X86ISD::PCMPEQM:            return "X86ISD::PCMPEQM";
  case X86ISD::PCMPGTM:            return "X86ISD::PCMPGTM";
  case X86ISD::ADD:                return "X86ISD::ADD";
  case X86ISD::SUB:                return "X86ISD::SUB";
  case X86ISD::ADC:                return "X86ISD::ADC";
  case X86ISD::SBB:                return "X86ISD::SBB";
  case X86ISD::SMUL:               return "X86ISD::SMUL";
  case X86ISD::UMUL:               return "X86ISD::UMUL";
  case X86ISD::INC:                return "X86ISD::INC";
  case X86ISD::DEC:                return "X86ISD::DEC";
  case X86ISD::OR:                 return "X86ISD::OR";
  case X86ISD::XOR:                return "X86ISD::XOR";
  case X86ISD::AND:                return "X86ISD::AND";
  case X86ISD::BEXTR:              return "X86ISD::BEXTR";
  case X86ISD::MUL_IMM:            return "X86ISD::MUL_IMM";
  case X86ISD::PTEST:              return "X86ISD::PTEST";
  case X86ISD::TESTP:              return "X86ISD::TESTP";
  case X86ISD::TESTM:              return "X86ISD::TESTM";
  case X86ISD::TESTNM:             return "X86ISD::TESTNM";
  case X86ISD::KORTEST:            return "X86ISD::KORTEST";
  case X86ISD::PACKSS:             return "X86ISD::PACKSS";
  case X86ISD::PACKUS:             return "X86ISD::PACKUS";
  case X86ISD::PALIGNR:            return "X86ISD::PALIGNR";
  case X86ISD::VALIGN:             return "X86ISD::VALIGN";
  case X86ISD::PSHUFD:             return "X86ISD::PSHUFD";
  case X86ISD::PSHUFHW:            return "X86ISD::PSHUFHW";
  case X86ISD::PSHUFLW:            return "X86ISD::PSHUFLW";
  case X86ISD::SHUFP:              return "X86ISD::SHUFP";
  case X86ISD::MOVLHPS:            return "X86ISD::MOVLHPS";
  case X86ISD::MOVLHPD:            return "X86ISD::MOVLHPD";
  case X86ISD::MOVHLPS:            return "X86ISD::MOVHLPS";
  case X86ISD::MOVLPS:             return "X86ISD::MOVLPS";
  case X86ISD::MOVLPD:             return "X86ISD::MOVLPD";
  case X86ISD::MOVDDUP:            return "X86ISD::MOVDDUP";
  case X86ISD::MOVSHDUP:           return "X86ISD::MOVSHDUP";
  case X86ISD::MOVSLDUP:           return "X86ISD::MOVSLDUP";
  case X86ISD::MOVSD:              return "X86ISD::MOVSD";
  case X86ISD::MOVSS:              return "X86ISD::MOVSS";
  case X86ISD::UNPCKL:             return "X86ISD::UNPCKL";
  case X86ISD::UNPCKH:             return "X86ISD::UNPCKH";
  case X86ISD::VBROADCAST:         return "X86ISD::VBROADCAST";
  case X86ISD::VBROADCASTM:        return "X86ISD::VBROADCASTM";
  case X86ISD::VEXTRACT:           return "X86ISD::VEXTRACT";
  case X86ISD::VPERMILP:           return "X86ISD::VPERMILP";
  case X86ISD::VPERM2X128:         return "X86ISD::VPERM2X128";
  case X86ISD::VPERMV:             return "X86ISD::VPERMV";
  case X86ISD::VPERMV3:            return "X86ISD::VPERMV3";
  case X86ISD::VPERMIV3:           return "X86ISD::VPERMIV3";
  case X86ISD::VPERMI:             return "X86ISD::VPERMI";
  case X86ISD::PMULUDQ:            return "X86ISD::PMULUDQ";
  case X86ISD::PMULDQ:             return "X86ISD::PMULDQ";
  case X86ISD::VASTART_SAVE_XMM_REGS: return "X86ISD::VASTART_SAVE_XMM_REGS";
  case X86ISD::VAARG_64:           return "X86ISD::VAARG_64";
  case X86ISD::WIN_ALLOCA:         return "X86ISD::WIN_ALLOCA";
  case X86ISD::MEMBARRIER:         return "X86ISD::MEMBARRIER";
  case X86ISD::SEG_ALLOCA:         return "X86ISD::SEG_ALLOCA";
  case X86ISD::WIN_FTOL:           return "X86ISD::WIN_FTOL";
  case X86ISD::SAHF:               return "X86ISD::SAHF";
  case X86ISD::RDRAND:             return "X86ISD::RDRAND";
  case X86ISD::RDSEED:             return "X86ISD::RDSEED";
  case X86ISD::FMADD:              return "X86ISD::FMADD";
  case X86ISD::FMSUB:              return "X86ISD::FMSUB";
  case X86ISD::FNMADD:             return "X86ISD::FNMADD";
  case X86ISD::FNMSUB:             return "X86ISD::FNMSUB";
  case X86ISD::FMADDSUB:           return "X86ISD::FMADDSUB";
  case X86ISD::FMSUBADD:           return "X86ISD::FMSUBADD";
  case X86ISD::PCMPESTRI:          return "X86ISD::PCMPESTRI";
  case X86ISD::PCMPISTRI:          return "X86ISD::PCMPISTRI";
  case X86ISD::XTEST:              return "X86ISD::XTEST";
  }
}

// isLegalAddressingMode - Return true if the addressing mode represented
// by AM is legal for this target, for a load/store of the specified type.
bool X86TargetLowering::isLegalAddressingMode(const AddrMode &AM,
                                              Type *Ty) const {
  // X86 supports extremely general addressing modes.
  CodeModel::Model M = getTargetMachine().getCodeModel();
  Reloc::Model R = getTargetMachine().getRelocationModel();

  // X86 allows a sign-extended 32-bit immediate field as a displacement.
  if (!X86::isOffsetSuitableForCodeModel(AM.BaseOffs, M, AM.BaseGV != nullptr))
    return false;

  if (AM.BaseGV) {
    unsigned GVFlags =
      Subtarget->ClassifyGlobalReference(AM.BaseGV, getTargetMachine());

    // If a reference to this global requires an extra load, we can't fold it.
    if (isGlobalStubReference(GVFlags))
      return false;

    // If BaseGV requires a register for the PIC base, we cannot also have a
    // BaseReg specified.
    if (AM.HasBaseReg && isGlobalRelativeToPICBase(GVFlags))
      return false;

    // If lower 4G is not available, then we must use rip-relative addressing.
    if ((M != CodeModel::Small || R != Reloc::Static) &&
        Subtarget->is64Bit() && (AM.BaseOffs || AM.Scale > 1))
      return false;
  }

  switch (AM.Scale) {
  case 0:
  case 1:
  case 2:
  case 4:
  case 8:
    // These scales always work.
    break;
  case 3:
  case 5:
  case 9:
    // These scales are formed with basereg+scalereg.  Only accept if there is
    // no basereg yet.
    if (AM.HasBaseReg)
      return false;
    break;
  default:  // Other stuff never works.
    return false;
  }

  return true;
}

bool X86TargetLowering::isVectorShiftByScalarCheap(Type *Ty) const {
  unsigned Bits = Ty->getScalarSizeInBits();

  // 8-bit shifts are always expensive, but versions with a scalar amount aren't
  // particularly cheaper than those without.
  if (Bits == 8)
    return false;

  // On AVX2 there are new vpsllv[dq] instructions (and other shifts), that make
  // variable shifts just as cheap as scalar ones.
  if (Subtarget->hasInt256() && (Bits == 32 || Bits == 64))
    return false;

  // Otherwise, it's significantly cheaper to shift by a scalar amount than by a
  // fully general vector.
  return true;
}

bool X86TargetLowering::isTruncateFree(Type *Ty1, Type *Ty2) const {
  if (!Ty1->isIntegerTy() || !Ty2->isIntegerTy())
    return false;
  unsigned NumBits1 = Ty1->getPrimitiveSizeInBits();
  unsigned NumBits2 = Ty2->getPrimitiveSizeInBits();
  return NumBits1 > NumBits2;
}

bool X86TargetLowering::allowTruncateForTailCall(Type *Ty1, Type *Ty2) const {
  if (!Ty1->isIntegerTy() || !Ty2->isIntegerTy())
    return false;

  if (!isTypeLegal(EVT::getEVT(Ty1)))
    return false;

  assert(Ty1->getPrimitiveSizeInBits() <= 64 && "i128 is probably not a noop");

  // Assuming the caller doesn't have a zeroext or signext return parameter,
  // truncation all the way down to i1 is valid.
  return true;
}

bool X86TargetLowering::isLegalICmpImmediate(int64_t Imm) const {
  return isInt<32>(Imm);
}

bool X86TargetLowering::isLegalAddImmediate(int64_t Imm) const {
  // Can also use sub to handle negated immediates.
  return isInt<32>(Imm);
}

bool X86TargetLowering::isTruncateFree(EVT VT1, EVT VT2) const {
  if (!VT1.isInteger() || !VT2.isInteger())
    return false;
  unsigned NumBits1 = VT1.getSizeInBits();
  unsigned NumBits2 = VT2.getSizeInBits();
  return NumBits1 > NumBits2;
}

bool X86TargetLowering::isZExtFree(Type *Ty1, Type *Ty2) const {
  // x86-64 implicitly zero-extends 32-bit results in 64-bit registers.
  return Ty1->isIntegerTy(32) && Ty2->isIntegerTy(64) && Subtarget->is64Bit();
}

bool X86TargetLowering::isZExtFree(EVT VT1, EVT VT2) const {
  // x86-64 implicitly zero-extends 32-bit results in 64-bit registers.
  return VT1 == MVT::i32 && VT2 == MVT::i64 && Subtarget->is64Bit();
}

bool X86TargetLowering::isZExtFree(SDValue Val, EVT VT2) const {
  EVT VT1 = Val.getValueType();
  if (isZExtFree(VT1, VT2))
    return true;

  if (Val.getOpcode() != ISD::LOAD)
    return false;

  if (!VT1.isSimple() || !VT1.isInteger() ||
      !VT2.isSimple() || !VT2.isInteger())
    return false;

  switch (VT1.getSimpleVT().SimpleTy) {
  default: break;
  case MVT::i8:
  case MVT::i16:
  case MVT::i32:
    // X86 has 8, 16, and 32-bit zero-extending loads.
    return true;
  }

  return false;
}

bool
X86TargetLowering::isFMAFasterThanFMulAndFAdd(EVT VT) const {
  if (!(Subtarget->hasFMA() || Subtarget->hasFMA4()))
    return false;

  VT = VT.getScalarType();

  if (!VT.isSimple())
    return false;

  switch (VT.getSimpleVT().SimpleTy) {
  case MVT::f32:
  case MVT::f64:
    return true;
  default:
    break;
  }

  return false;
}

bool X86TargetLowering::isNarrowingProfitable(EVT VT1, EVT VT2) const {
  // i16 instructions are longer (0x66 prefix) and potentially slower.
  return !(VT1 == MVT::i32 && VT2 == MVT::i16);
}

/// isShuffleMaskLegal - Targets can use this to indicate that they only
/// support *some* VECTOR_SHUFFLE operations, those with specific masks.
/// By default, if a target supports the VECTOR_SHUFFLE node, all mask values
/// are assumed to be legal.
bool
X86TargetLowering::isShuffleMaskLegal(const SmallVectorImpl<int> &M,
                                      EVT VT) const {
  if (!VT.isSimple())
    return false;

  MVT SVT = VT.getSimpleVT();

  // Very little shuffling can be done for 64-bit vectors right now.
  if (VT.getSizeInBits() == 64)
    return false;

  // If this is a single-input shuffle with no 128 bit lane crossings we can
  // lower it into pshufb.
  if ((SVT.is128BitVector() && Subtarget->hasSSSE3()) ||
      (SVT.is256BitVector() && Subtarget->hasInt256())) {
    bool isLegal = true;
    for (unsigned I = 0, E = M.size(); I != E; ++I) {
      if (M[I] >= (int)SVT.getVectorNumElements() ||
          ShuffleCrosses128bitLane(SVT, I, M[I])) {
        isLegal = false;
        break;
      }
    }
    if (isLegal)
      return true;
  }

  // FIXME: blends, shifts.
  return (SVT.getVectorNumElements() == 2 ||
          ShuffleVectorSDNode::isSplatMask(&M[0], VT) ||
          isMOVLMask(M, SVT) ||
          isMOVHLPSMask(M, SVT) ||
          isSHUFPMask(M, SVT) ||
          isPSHUFDMask(M, SVT) ||
          isPSHUFHWMask(M, SVT, Subtarget->hasInt256()) ||
          isPSHUFLWMask(M, SVT, Subtarget->hasInt256()) ||
          isPALIGNRMask(M, SVT, Subtarget) ||
          isUNPCKLMask(M, SVT, Subtarget->hasInt256()) ||
          isUNPCKHMask(M, SVT, Subtarget->hasInt256()) ||
          isUNPCKL_v_undef_Mask(M, SVT, Subtarget->hasInt256()) ||
          isUNPCKH_v_undef_Mask(M, SVT, Subtarget->hasInt256()) ||
          isBlendMask(M, SVT, Subtarget->hasSSE41(), Subtarget->hasInt256()));
}

bool
X86TargetLowering::isVectorClearMaskLegal(const SmallVectorImpl<int> &Mask,
                                          EVT VT) const {
  if (!VT.isSimple())
    return false;

  MVT SVT = VT.getSimpleVT();
  unsigned NumElts = SVT.getVectorNumElements();
  // FIXME: This collection of masks seems suspect.
  if (NumElts == 2)
    return true;
  if (NumElts == 4 && SVT.is128BitVector()) {
    return (isMOVLMask(Mask, SVT)  ||
            isCommutedMOVLMask(Mask, SVT, true) ||
            isSHUFPMask(Mask, SVT) ||
            isSHUFPMask(Mask, SVT, /* Commuted */ true));
  }
  return false;
}

//===----------------------------------------------------------------------===//
//                           X86 Scheduler Hooks
//===----------------------------------------------------------------------===//

/// Utility function to emit xbegin specifying the start of an RTM region.
static MachineBasicBlock *EmitXBegin(MachineInstr *MI, MachineBasicBlock *MBB,
                                     const TargetInstrInfo *TII) {
  DebugLoc DL = MI->getDebugLoc();

  const BasicBlock *BB = MBB->getBasicBlock();
  MachineFunction::iterator I = MBB;
  ++I;

  // For the v = xbegin(), we generate
  //
  // thisMBB:
  //  xbegin sinkMBB
  //
  // mainMBB:
  //  eax = -1
  //
  // sinkMBB:
  //  v = eax

  MachineBasicBlock *thisMBB = MBB;
  MachineFunction *MF = MBB->getParent();
  MachineBasicBlock *mainMBB = MF->CreateMachineBasicBlock(BB);
  MachineBasicBlock *sinkMBB = MF->CreateMachineBasicBlock(BB);
  MF->insert(I, mainMBB);
  MF->insert(I, sinkMBB);

  // Transfer the remainder of BB and its successor edges to sinkMBB.
  sinkMBB->splice(sinkMBB->begin(), MBB,
                  std::next(MachineBasicBlock::iterator(MI)), MBB->end());
  sinkMBB->transferSuccessorsAndUpdatePHIs(MBB);

  // thisMBB:
  //  xbegin sinkMBB
  //  # fallthrough to mainMBB
  //  # abortion to sinkMBB
  BuildMI(thisMBB, DL, TII->get(X86::XBEGIN_4)).addMBB(sinkMBB);
  thisMBB->addSuccessor(mainMBB);
  thisMBB->addSuccessor(sinkMBB);

  // mainMBB:
  //  EAX = -1
  BuildMI(mainMBB, DL, TII->get(X86::MOV32ri), X86::EAX).addImm(-1);
  mainMBB->addSuccessor(sinkMBB);

  // sinkMBB:
  // EAX is live into the sinkMBB
  sinkMBB->addLiveIn(X86::EAX);
  BuildMI(*sinkMBB, sinkMBB->begin(), DL,
          TII->get(TargetOpcode::COPY), MI->getOperand(0).getReg())
    .addReg(X86::EAX);

  MI->eraseFromParent();
  return sinkMBB;
}

// FIXME: When we get size specific XMM0 registers, i.e. XMM0_V16I8
// or XMM0_V32I8 in AVX all of this code can be replaced with that
// in the .td file.
static MachineBasicBlock *EmitPCMPSTRM(MachineInstr *MI, MachineBasicBlock *BB,
                                       const TargetInstrInfo *TII) {
  unsigned Opc;
  switch (MI->getOpcode()) {
  default: llvm_unreachable("illegal opcode!");
  case X86::PCMPISTRM128REG:  Opc = X86::PCMPISTRM128rr;  break;
  case X86::VPCMPISTRM128REG: Opc = X86::VPCMPISTRM128rr; break;
  case X86::PCMPISTRM128MEM:  Opc = X86::PCMPISTRM128rm;  break;
  case X86::VPCMPISTRM128MEM: Opc = X86::VPCMPISTRM128rm; break;
  case X86::PCMPESTRM128REG:  Opc = X86::PCMPESTRM128rr;  break;
  case X86::VPCMPESTRM128REG: Opc = X86::VPCMPESTRM128rr; break;
  case X86::PCMPESTRM128MEM:  Opc = X86::PCMPESTRM128rm;  break;
  case X86::VPCMPESTRM128MEM: Opc = X86::VPCMPESTRM128rm; break;
  }

  DebugLoc dl = MI->getDebugLoc();
  MachineInstrBuilder MIB = BuildMI(*BB, MI, dl, TII->get(Opc));

  unsigned NumArgs = MI->getNumOperands();
  for (unsigned i = 1; i < NumArgs; ++i) {
    MachineOperand &Op = MI->getOperand(i);
    if (!(Op.isReg() && Op.isImplicit()))
      MIB.addOperand(Op);
  }
  if (MI->hasOneMemOperand())
    MIB->setMemRefs(MI->memoperands_begin(), MI->memoperands_end());

  BuildMI(*BB, MI, dl,
    TII->get(TargetOpcode::COPY), MI->getOperand(0).getReg())
    .addReg(X86::XMM0);

  MI->eraseFromParent();
  return BB;
}

// FIXME: Custom handling because TableGen doesn't support multiple implicit
// defs in an instruction pattern
static MachineBasicBlock *EmitPCMPSTRI(MachineInstr *MI, MachineBasicBlock *BB,
                                       const TargetInstrInfo *TII) {
  unsigned Opc;
  switch (MI->getOpcode()) {
  default: llvm_unreachable("illegal opcode!");
  case X86::PCMPISTRIREG:  Opc = X86::PCMPISTRIrr;  break;
  case X86::VPCMPISTRIREG: Opc = X86::VPCMPISTRIrr; break;
  case X86::PCMPISTRIMEM:  Opc = X86::PCMPISTRIrm;  break;
  case X86::VPCMPISTRIMEM: Opc = X86::VPCMPISTRIrm; break;
  case X86::PCMPESTRIREG:  Opc = X86::PCMPESTRIrr;  break;
  case X86::VPCMPESTRIREG: Opc = X86::VPCMPESTRIrr; break;
  case X86::PCMPESTRIMEM:  Opc = X86::PCMPESTRIrm;  break;
  case X86::VPCMPESTRIMEM: Opc = X86::VPCMPESTRIrm; break;
  }

  DebugLoc dl = MI->getDebugLoc();
  MachineInstrBuilder MIB = BuildMI(*BB, MI, dl, TII->get(Opc));

  unsigned NumArgs = MI->getNumOperands(); // remove the results
  for (unsigned i = 1; i < NumArgs; ++i) {
    MachineOperand &Op = MI->getOperand(i);
    if (!(Op.isReg() && Op.isImplicit()))
      MIB.addOperand(Op);
  }
  if (MI->hasOneMemOperand())
    MIB->setMemRefs(MI->memoperands_begin(), MI->memoperands_end());

  BuildMI(*BB, MI, dl,
    TII->get(TargetOpcode::COPY), MI->getOperand(0).getReg())
    .addReg(X86::ECX);

  MI->eraseFromParent();
  return BB;
}

static MachineBasicBlock * EmitMonitor(MachineInstr *MI, MachineBasicBlock *BB,
                                       const TargetInstrInfo *TII,
                                       const X86Subtarget* Subtarget) {
  DebugLoc dl = MI->getDebugLoc();

  // Address into RAX/EAX, other two args into ECX, EDX.
  unsigned MemOpc = Subtarget->is64Bit() ? X86::LEA64r : X86::LEA32r;
  unsigned MemReg = Subtarget->is64Bit() ? X86::RAX : X86::EAX;
  MachineInstrBuilder MIB = BuildMI(*BB, MI, dl, TII->get(MemOpc), MemReg);
  for (int i = 0; i < X86::AddrNumOperands; ++i)
    MIB.addOperand(MI->getOperand(i));

  unsigned ValOps = X86::AddrNumOperands;
  BuildMI(*BB, MI, dl, TII->get(TargetOpcode::COPY), X86::ECX)
    .addReg(MI->getOperand(ValOps).getReg());
  BuildMI(*BB, MI, dl, TII->get(TargetOpcode::COPY), X86::EDX)
    .addReg(MI->getOperand(ValOps+1).getReg());

  // The instruction doesn't actually take any operands though.
  BuildMI(*BB, MI, dl, TII->get(X86::MONITORrrr));

  MI->eraseFromParent(); // The pseudo is gone now.
  return BB;
}

MachineBasicBlock *
X86TargetLowering::EmitVAARG64WithCustomInserter(
                   MachineInstr *MI,
                   MachineBasicBlock *MBB) const {
  // Emit va_arg instruction on X86-64.

  // Operands to this pseudo-instruction:
  // 0  ) Output        : destination address (reg)
  // 1-5) Input         : va_list address (addr, i64mem)
  // 6  ) ArgSize       : Size (in bytes) of vararg type
  // 7  ) ArgMode       : 0=overflow only, 1=use gp_offset, 2=use fp_offset
  // 8  ) Align         : Alignment of type
  // 9  ) EFLAGS (implicit-def)

  assert(MI->getNumOperands() == 10 && "VAARG_64 should have 10 operands!");
  assert(X86::AddrNumOperands == 5 && "VAARG_64 assumes 5 address operands");

  unsigned DestReg = MI->getOperand(0).getReg();
  MachineOperand &Base = MI->getOperand(1);
  MachineOperand &Scale = MI->getOperand(2);
  MachineOperand &Index = MI->getOperand(3);
  MachineOperand &Disp = MI->getOperand(4);
  MachineOperand &Segment = MI->getOperand(5);
  unsigned ArgSize = MI->getOperand(6).getImm();
  unsigned ArgMode = MI->getOperand(7).getImm();
  unsigned Align = MI->getOperand(8).getImm();

  // Memory Reference
  assert(MI->hasOneMemOperand() && "Expected VAARG_64 to have one memoperand");
  MachineInstr::mmo_iterator MMOBegin = MI->memoperands_begin();
  MachineInstr::mmo_iterator MMOEnd = MI->memoperands_end();

  // Machine Information
  const TargetInstrInfo *TII = MBB->getParent()->getSubtarget().getInstrInfo();
  MachineRegisterInfo &MRI = MBB->getParent()->getRegInfo();
  const TargetRegisterClass *AddrRegClass = getRegClassFor(MVT::i64);
  const TargetRegisterClass *OffsetRegClass = getRegClassFor(MVT::i32);
  DebugLoc DL = MI->getDebugLoc();

  // struct va_list {
  //   i32   gp_offset
  //   i32   fp_offset
  //   i64   overflow_area (address)
  //   i64   reg_save_area (address)
  // }
  // sizeof(va_list) = 24
  // alignment(va_list) = 8

  unsigned TotalNumIntRegs = 6;
  unsigned TotalNumXMMRegs = 8;
  bool UseGPOffset = (ArgMode == 1);
  bool UseFPOffset = (ArgMode == 2);
  unsigned MaxOffset = TotalNumIntRegs * 8 +
                       (UseFPOffset ? TotalNumXMMRegs * 16 : 0);

  /* Align ArgSize to a multiple of 8 */
  unsigned ArgSizeA8 = (ArgSize + 7) & ~7;
  bool NeedsAlign = (Align > 8);

  MachineBasicBlock *thisMBB = MBB;
  MachineBasicBlock *overflowMBB;
  MachineBasicBlock *offsetMBB;
  MachineBasicBlock *endMBB;

  unsigned OffsetDestReg = 0;    // Argument address computed by offsetMBB
  unsigned OverflowDestReg = 0;  // Argument address computed by overflowMBB
  unsigned OffsetReg = 0;

  if (!UseGPOffset && !UseFPOffset) {
    // If we only pull from the overflow region, we don't create a branch.
    // We don't need to alter control flow.
    OffsetDestReg = 0; // unused
    OverflowDestReg = DestReg;

    offsetMBB = nullptr;
    overflowMBB = thisMBB;
    endMBB = thisMBB;
  } else {
    // First emit code to check if gp_offset (or fp_offset) is below the bound.
    // If so, pull the argument from reg_save_area. (branch to offsetMBB)
    // If not, pull from overflow_area. (branch to overflowMBB)
    //
    //       thisMBB
    //         |     .
    //         |        .
    //     offsetMBB   overflowMBB
    //         |        .
    //         |     .
    //        endMBB

    // Registers for the PHI in endMBB
    OffsetDestReg = MRI.createVirtualRegister(AddrRegClass);
    OverflowDestReg = MRI.createVirtualRegister(AddrRegClass);

    const BasicBlock *LLVM_BB = MBB->getBasicBlock();
    MachineFunction *MF = MBB->getParent();
    overflowMBB = MF->CreateMachineBasicBlock(LLVM_BB);
    offsetMBB = MF->CreateMachineBasicBlock(LLVM_BB);
    endMBB = MF->CreateMachineBasicBlock(LLVM_BB);

    MachineFunction::iterator MBBIter = MBB;
    ++MBBIter;

    // Insert the new basic blocks
    MF->insert(MBBIter, offsetMBB);
    MF->insert(MBBIter, overflowMBB);
    MF->insert(MBBIter, endMBB);

    // Transfer the remainder of MBB and its successor edges to endMBB.
    endMBB->splice(endMBB->begin(), thisMBB,
                   std::next(MachineBasicBlock::iterator(MI)), thisMBB->end());
    endMBB->transferSuccessorsAndUpdatePHIs(thisMBB);

    // Make offsetMBB and overflowMBB successors of thisMBB
    thisMBB->addSuccessor(offsetMBB);
    thisMBB->addSuccessor(overflowMBB);

    // endMBB is a successor of both offsetMBB and overflowMBB
    offsetMBB->addSuccessor(endMBB);
    overflowMBB->addSuccessor(endMBB);

    // Load the offset value into a register
    OffsetReg = MRI.createVirtualRegister(OffsetRegClass);
    BuildMI(thisMBB, DL, TII->get(X86::MOV32rm), OffsetReg)
      .addOperand(Base)
      .addOperand(Scale)
      .addOperand(Index)
      .addDisp(Disp, UseFPOffset ? 4 : 0)
      .addOperand(Segment)
      .setMemRefs(MMOBegin, MMOEnd);

    // Check if there is enough room left to pull this argument.
    BuildMI(thisMBB, DL, TII->get(X86::CMP32ri))
      .addReg(OffsetReg)
      .addImm(MaxOffset + 8 - ArgSizeA8);

    // Branch to "overflowMBB" if offset >= max
    // Fall through to "offsetMBB" otherwise
    BuildMI(thisMBB, DL, TII->get(X86::GetCondBranchFromCond(X86::COND_AE)))
      .addMBB(overflowMBB);
  }

  // In offsetMBB, emit code to use the reg_save_area.
  if (offsetMBB) {
    assert(OffsetReg != 0);

    // Read the reg_save_area address.
    unsigned RegSaveReg = MRI.createVirtualRegister(AddrRegClass);
    BuildMI(offsetMBB, DL, TII->get(X86::MOV64rm), RegSaveReg)
      .addOperand(Base)
      .addOperand(Scale)
      .addOperand(Index)
      .addDisp(Disp, 16)
      .addOperand(Segment)
      .setMemRefs(MMOBegin, MMOEnd);

    // Zero-extend the offset
    unsigned OffsetReg64 = MRI.createVirtualRegister(AddrRegClass);
      BuildMI(offsetMBB, DL, TII->get(X86::SUBREG_TO_REG), OffsetReg64)
        .addImm(0)
        .addReg(OffsetReg)
        .addImm(X86::sub_32bit);

    // Add the offset to the reg_save_area to get the final address.
    BuildMI(offsetMBB, DL, TII->get(X86::ADD64rr), OffsetDestReg)
      .addReg(OffsetReg64)
      .addReg(RegSaveReg);

    // Compute the offset for the next argument
    unsigned NextOffsetReg = MRI.createVirtualRegister(OffsetRegClass);
    BuildMI(offsetMBB, DL, TII->get(X86::ADD32ri), NextOffsetReg)
      .addReg(OffsetReg)
      .addImm(UseFPOffset ? 16 : 8);

    // Store it back into the va_list.
    BuildMI(offsetMBB, DL, TII->get(X86::MOV32mr))
      .addOperand(Base)
      .addOperand(Scale)
      .addOperand(Index)
      .addDisp(Disp, UseFPOffset ? 4 : 0)
      .addOperand(Segment)
      .addReg(NextOffsetReg)
      .setMemRefs(MMOBegin, MMOEnd);

    // Jump to endMBB
    BuildMI(offsetMBB, DL, TII->get(X86::JMP_4))
      .addMBB(endMBB);
  }

  //
  // Emit code to use overflow area
  //

  // Load the overflow_area address into a register.
  unsigned OverflowAddrReg = MRI.createVirtualRegister(AddrRegClass);
  BuildMI(overflowMBB, DL, TII->get(X86::MOV64rm), OverflowAddrReg)
    .addOperand(Base)
    .addOperand(Scale)
    .addOperand(Index)
    .addDisp(Disp, 8)
    .addOperand(Segment)
    .setMemRefs(MMOBegin, MMOEnd);

  // If we need to align it, do so. Otherwise, just copy the address
  // to OverflowDestReg.
  if (NeedsAlign) {
    // Align the overflow address
    assert((Align & (Align-1)) == 0 && "Alignment must be a power of 2");
    unsigned TmpReg = MRI.createVirtualRegister(AddrRegClass);

    // aligned_addr = (addr + (align-1)) & ~(align-1)
    BuildMI(overflowMBB, DL, TII->get(X86::ADD64ri32), TmpReg)
      .addReg(OverflowAddrReg)
      .addImm(Align-1);

    BuildMI(overflowMBB, DL, TII->get(X86::AND64ri32), OverflowDestReg)
      .addReg(TmpReg)
      .addImm(~(uint64_t)(Align-1));
  } else {
    BuildMI(overflowMBB, DL, TII->get(TargetOpcode::COPY), OverflowDestReg)
      .addReg(OverflowAddrReg);
  }

  // Compute the next overflow address after this argument.
  // (the overflow address should be kept 8-byte aligned)
  unsigned NextAddrReg = MRI.createVirtualRegister(AddrRegClass);
  BuildMI(overflowMBB, DL, TII->get(X86::ADD64ri32), NextAddrReg)
    .addReg(OverflowDestReg)
    .addImm(ArgSizeA8);

  // Store the new overflow address.
  BuildMI(overflowMBB, DL, TII->get(X86::MOV64mr))
    .addOperand(Base)
    .addOperand(Scale)
    .addOperand(Index)
    .addDisp(Disp, 8)
    .addOperand(Segment)
    .addReg(NextAddrReg)
    .setMemRefs(MMOBegin, MMOEnd);

  // If we branched, emit the PHI to the front of endMBB.
  if (offsetMBB) {
    BuildMI(*endMBB, endMBB->begin(), DL,
            TII->get(X86::PHI), DestReg)
      .addReg(OffsetDestReg).addMBB(offsetMBB)
      .addReg(OverflowDestReg).addMBB(overflowMBB);
  }

  // Erase the pseudo instruction
  MI->eraseFromParent();

  return endMBB;
}

MachineBasicBlock *
X86TargetLowering::EmitVAStartSaveXMMRegsWithCustomInserter(
                                                 MachineInstr *MI,
                                                 MachineBasicBlock *MBB) const {
  // Emit code to save XMM registers to the stack. The ABI says that the
  // number of registers to save is given in %al, so it's theoretically
  // possible to do an indirect jump trick to avoid saving all of them,
  // however this code takes a simpler approach and just executes all
  // of the stores if %al is non-zero. It's less code, and it's probably
  // easier on the hardware branch predictor, and stores aren't all that
  // expensive anyway.

  // Create the new basic blocks. One block contains all the XMM stores,
  // and one block is the final destination regardless of whether any
  // stores were performed.
  const BasicBlock *LLVM_BB = MBB->getBasicBlock();
  MachineFunction *F = MBB->getParent();
  MachineFunction::iterator MBBIter = MBB;
  ++MBBIter;
  MachineBasicBlock *XMMSaveMBB = F->CreateMachineBasicBlock(LLVM_BB);
  MachineBasicBlock *EndMBB = F->CreateMachineBasicBlock(LLVM_BB);
  F->insert(MBBIter, XMMSaveMBB);
  F->insert(MBBIter, EndMBB);

  // Transfer the remainder of MBB and its successor edges to EndMBB.
  EndMBB->splice(EndMBB->begin(), MBB,
                 std::next(MachineBasicBlock::iterator(MI)), MBB->end());
  EndMBB->transferSuccessorsAndUpdatePHIs(MBB);

  // The original block will now fall through to the XMM save block.
  MBB->addSuccessor(XMMSaveMBB);
  // The XMMSaveMBB will fall through to the end block.
  XMMSaveMBB->addSuccessor(EndMBB);

  // Now add the instructions.
  const TargetInstrInfo *TII = MBB->getParent()->getSubtarget().getInstrInfo();
  DebugLoc DL = MI->getDebugLoc();

  unsigned CountReg = MI->getOperand(0).getReg();
  int64_t RegSaveFrameIndex = MI->getOperand(1).getImm();
  int64_t VarArgsFPOffset = MI->getOperand(2).getImm();

  if (!Subtarget->isTargetWin64()) {
    // If %al is 0, branch around the XMM save block.
    BuildMI(MBB, DL, TII->get(X86::TEST8rr)).addReg(CountReg).addReg(CountReg);
    BuildMI(MBB, DL, TII->get(X86::JE_4)).addMBB(EndMBB);
    MBB->addSuccessor(EndMBB);
  }

  // Make sure the last operand is EFLAGS, which gets clobbered by the branch
  // that was just emitted, but clearly shouldn't be "saved".
  assert((MI->getNumOperands() <= 3 ||
          !MI->getOperand(MI->getNumOperands() - 1).isReg() ||
          MI->getOperand(MI->getNumOperands() - 1).getReg() == X86::EFLAGS)
         && "Expected last argument to be EFLAGS");
  unsigned MOVOpc = Subtarget->hasFp256() ? X86::VMOVAPSmr : X86::MOVAPSmr;
  // In the XMM save block, save all the XMM argument registers.
  for (int i = 3, e = MI->getNumOperands() - 1; i != e; ++i) {
    int64_t Offset = (i - 3) * 16 + VarArgsFPOffset;
    MachineMemOperand *MMO =
      F->getMachineMemOperand(
          MachinePointerInfo::getFixedStack(RegSaveFrameIndex, Offset),
        MachineMemOperand::MOStore,
        /*Size=*/16, /*Align=*/16);
    BuildMI(XMMSaveMBB, DL, TII->get(MOVOpc))
      .addFrameIndex(RegSaveFrameIndex)
      .addImm(/*Scale=*/1)
      .addReg(/*IndexReg=*/0)
      .addImm(/*Disp=*/Offset)
      .addReg(/*Segment=*/0)
      .addReg(MI->getOperand(i).getReg())
      .addMemOperand(MMO);
  }

  MI->eraseFromParent();   // The pseudo instruction is gone now.

  return EndMBB;
}

// The EFLAGS operand of SelectItr might be missing a kill marker
// because there were multiple uses of EFLAGS, and ISel didn't know
// which to mark. Figure out whether SelectItr should have had a
// kill marker, and set it if it should. Returns the correct kill
// marker value.
static bool checkAndUpdateEFLAGSKill(MachineBasicBlock::iterator SelectItr,
                                     MachineBasicBlock* BB,
                                     const TargetRegisterInfo* TRI) {
  // Scan forward through BB for a use/def of EFLAGS.
  MachineBasicBlock::iterator miI(std::next(SelectItr));
  for (MachineBasicBlock::iterator miE = BB->end(); miI != miE; ++miI) {
    const MachineInstr& mi = *miI;
    if (mi.readsRegister(X86::EFLAGS))
      return false;
    if (mi.definesRegister(X86::EFLAGS))
      break; // Should have kill-flag - update below.
  }

  // If we hit the end of the block, check whether EFLAGS is live into a
  // successor.
  if (miI == BB->end()) {
    for (MachineBasicBlock::succ_iterator sItr = BB->succ_begin(),
                                          sEnd = BB->succ_end();
         sItr != sEnd; ++sItr) {
      MachineBasicBlock* succ = *sItr;
      if (succ->isLiveIn(X86::EFLAGS))
        return false;
    }
  }

  // We found a def, or hit the end of the basic block and EFLAGS wasn't live
  // out. SelectMI should have a kill flag on EFLAGS.
  SelectItr->addRegisterKilled(X86::EFLAGS, TRI);
  return true;
}

MachineBasicBlock *
X86TargetLowering::EmitLoweredSelect(MachineInstr *MI,
                                     MachineBasicBlock *BB) const {
  const TargetInstrInfo *TII = BB->getParent()->getSubtarget().getInstrInfo();
  DebugLoc DL = MI->getDebugLoc();

  // To "insert" a SELECT_CC instruction, we actually have to insert the
  // diamond control-flow pattern.  The incoming instruction knows the
  // destination vreg to set, the condition code register to branch on, the
  // true/false values to select between, and a branch opcode to use.
  const BasicBlock *LLVM_BB = BB->getBasicBlock();
  MachineFunction::iterator It = BB;
  ++It;

  //  thisMBB:
  //  ...
  //   TrueVal = ...
  //   cmpTY ccX, r1, r2
  //   bCC copy1MBB
  //   fallthrough --> copy0MBB
  MachineBasicBlock *thisMBB = BB;
  MachineFunction *F = BB->getParent();
  MachineBasicBlock *copy0MBB = F->CreateMachineBasicBlock(LLVM_BB);
  MachineBasicBlock *sinkMBB = F->CreateMachineBasicBlock(LLVM_BB);
  F->insert(It, copy0MBB);
  F->insert(It, sinkMBB);

  // If the EFLAGS register isn't dead in the terminator, then claim that it's
  // live into the sink and copy blocks.
  const TargetRegisterInfo *TRI =
      BB->getParent()->getSubtarget().getRegisterInfo();
  if (!MI->killsRegister(X86::EFLAGS) &&
      !checkAndUpdateEFLAGSKill(MI, BB, TRI)) {
    copy0MBB->addLiveIn(X86::EFLAGS);
    sinkMBB->addLiveIn(X86::EFLAGS);
  }

  // Transfer the remainder of BB and its successor edges to sinkMBB.
  sinkMBB->splice(sinkMBB->begin(), BB,
                  std::next(MachineBasicBlock::iterator(MI)), BB->end());
  sinkMBB->transferSuccessorsAndUpdatePHIs(BB);

  // Add the true and fallthrough blocks as its successors.
  BB->addSuccessor(copy0MBB);
  BB->addSuccessor(sinkMBB);

  // Create the conditional branch instruction.
  unsigned Opc =
    X86::GetCondBranchFromCond((X86::CondCode)MI->getOperand(3).getImm());
  BuildMI(BB, DL, TII->get(Opc)).addMBB(sinkMBB);

  //  copy0MBB:
  //   %FalseValue = ...
  //   # fallthrough to sinkMBB
  copy0MBB->addSuccessor(sinkMBB);

  //  sinkMBB:
  //   %Result = phi [ %FalseValue, copy0MBB ], [ %TrueValue, thisMBB ]
  //  ...
  BuildMI(*sinkMBB, sinkMBB->begin(), DL,
          TII->get(X86::PHI), MI->getOperand(0).getReg())
    .addReg(MI->getOperand(1).getReg()).addMBB(copy0MBB)
    .addReg(MI->getOperand(2).getReg()).addMBB(thisMBB);

  MI->eraseFromParent();   // The pseudo instruction is gone now.
  return sinkMBB;
}

MachineBasicBlock *
X86TargetLowering::EmitLoweredSegAlloca(MachineInstr *MI, MachineBasicBlock *BB,
                                        bool Is64Bit) const {
  MachineFunction *MF = BB->getParent();
  const TargetInstrInfo *TII = MF->getSubtarget().getInstrInfo();
  DebugLoc DL = MI->getDebugLoc();
  const BasicBlock *LLVM_BB = BB->getBasicBlock();

  assert(MF->shouldSplitStack());

  unsigned TlsReg = Is64Bit ? X86::FS : X86::GS;
  unsigned TlsOffset = Is64Bit ? 0x70 : 0x30;

  // BB:
  //  ... [Till the alloca]
  // If stacklet is not large enough, jump to mallocMBB
  //
  // bumpMBB:
  //  Allocate by subtracting from RSP
  //  Jump to continueMBB
  //
  // mallocMBB:
  //  Allocate by call to runtime
  //
  // continueMBB:
  //  ...
  //  [rest of original BB]
  //

  MachineBasicBlock *mallocMBB = MF->CreateMachineBasicBlock(LLVM_BB);
  MachineBasicBlock *bumpMBB = MF->CreateMachineBasicBlock(LLVM_BB);
  MachineBasicBlock *continueMBB = MF->CreateMachineBasicBlock(LLVM_BB);

  MachineRegisterInfo &MRI = MF->getRegInfo();
  const TargetRegisterClass *AddrRegClass =
    getRegClassFor(Is64Bit ? MVT::i64:MVT::i32);

  unsigned mallocPtrVReg = MRI.createVirtualRegister(AddrRegClass),
    bumpSPPtrVReg = MRI.createVirtualRegister(AddrRegClass),
    tmpSPVReg = MRI.createVirtualRegister(AddrRegClass),
    SPLimitVReg = MRI.createVirtualRegister(AddrRegClass),
    sizeVReg = MI->getOperand(1).getReg(),
    physSPReg = Is64Bit ? X86::RSP : X86::ESP;

  MachineFunction::iterator MBBIter = BB;
  ++MBBIter;

  MF->insert(MBBIter, bumpMBB);
  MF->insert(MBBIter, mallocMBB);
  MF->insert(MBBIter, continueMBB);

  continueMBB->splice(continueMBB->begin(), BB,
                      std::next(MachineBasicBlock::iterator(MI)), BB->end());
  continueMBB->transferSuccessorsAndUpdatePHIs(BB);

  // Add code to the main basic block to check if the stack limit has been hit,
  // and if so, jump to mallocMBB otherwise to bumpMBB.
  BuildMI(BB, DL, TII->get(TargetOpcode::COPY), tmpSPVReg).addReg(physSPReg);
  BuildMI(BB, DL, TII->get(Is64Bit ? X86::SUB64rr:X86::SUB32rr), SPLimitVReg)
    .addReg(tmpSPVReg).addReg(sizeVReg);
  BuildMI(BB, DL, TII->get(Is64Bit ? X86::CMP64mr:X86::CMP32mr))
    .addReg(0).addImm(1).addReg(0).addImm(TlsOffset).addReg(TlsReg)
    .addReg(SPLimitVReg);
  BuildMI(BB, DL, TII->get(X86::JG_4)).addMBB(mallocMBB);

  // bumpMBB simply decreases the stack pointer, since we know the current
  // stacklet has enough space.
  BuildMI(bumpMBB, DL, TII->get(TargetOpcode::COPY), physSPReg)
    .addReg(SPLimitVReg);
  BuildMI(bumpMBB, DL, TII->get(TargetOpcode::COPY), bumpSPPtrVReg)
    .addReg(SPLimitVReg);
  BuildMI(bumpMBB, DL, TII->get(X86::JMP_4)).addMBB(continueMBB);

  // Calls into a routine in libgcc to allocate more space from the heap.
  const uint32_t *RegMask = MF->getTarget()
                                .getSubtargetImpl()
                                ->getRegisterInfo()
                                ->getCallPreservedMask(CallingConv::C);
  if (Is64Bit) {
    BuildMI(mallocMBB, DL, TII->get(X86::MOV64rr), X86::RDI)
      .addReg(sizeVReg);
    BuildMI(mallocMBB, DL, TII->get(X86::CALL64pcrel32))
      .addExternalSymbol("__morestack_allocate_stack_space")
      .addRegMask(RegMask)
      .addReg(X86::RDI, RegState::Implicit)
      .addReg(X86::RAX, RegState::ImplicitDefine);
  } else {
    BuildMI(mallocMBB, DL, TII->get(X86::SUB32ri), physSPReg).addReg(physSPReg)
      .addImm(12);
    BuildMI(mallocMBB, DL, TII->get(X86::PUSH32r)).addReg(sizeVReg);
    BuildMI(mallocMBB, DL, TII->get(X86::CALLpcrel32))
      .addExternalSymbol("__morestack_allocate_stack_space")
      .addRegMask(RegMask)
      .addReg(X86::EAX, RegState::ImplicitDefine);
  }

  if (!Is64Bit)
    BuildMI(mallocMBB, DL, TII->get(X86::ADD32ri), physSPReg).addReg(physSPReg)
      .addImm(16);

  BuildMI(mallocMBB, DL, TII->get(TargetOpcode::COPY), mallocPtrVReg)
    .addReg(Is64Bit ? X86::RAX : X86::EAX);
  BuildMI(mallocMBB, DL, TII->get(X86::JMP_4)).addMBB(continueMBB);

  // Set up the CFG correctly.
  BB->addSuccessor(bumpMBB);
  BB->addSuccessor(mallocMBB);
  mallocMBB->addSuccessor(continueMBB);
  bumpMBB->addSuccessor(continueMBB);

  // Take care of the PHI nodes.
  BuildMI(*continueMBB, continueMBB->begin(), DL, TII->get(X86::PHI),
          MI->getOperand(0).getReg())
    .addReg(mallocPtrVReg).addMBB(mallocMBB)
    .addReg(bumpSPPtrVReg).addMBB(bumpMBB);

  // Delete the original pseudo instruction.
  MI->eraseFromParent();

  // And we're done.
  return continueMBB;
}

MachineBasicBlock *
X86TargetLowering::EmitLoweredWinAlloca(MachineInstr *MI,
                                        MachineBasicBlock *BB) const {
  const TargetInstrInfo *TII = BB->getParent()->getSubtarget().getInstrInfo();
  DebugLoc DL = MI->getDebugLoc();

  assert(!Subtarget->isTargetMacho());

  // The lowering is pretty easy: we're just emitting the call to _alloca.  The
  // non-trivial part is impdef of ESP.

  if (Subtarget->isTargetWin64()) {
    if (Subtarget->isTargetCygMing()) {
      // ___chkstk(Mingw64):
      // Clobbers R10, R11, RAX and EFLAGS.
      // Updates RSP.
      BuildMI(*BB, MI, DL, TII->get(X86::W64ALLOCA))
        .addExternalSymbol("___chkstk")
        .addReg(X86::RAX, RegState::Implicit)
        .addReg(X86::RSP, RegState::Implicit)
        .addReg(X86::RAX, RegState::Define | RegState::Implicit)
        .addReg(X86::RSP, RegState::Define | RegState::Implicit)
        .addReg(X86::EFLAGS, RegState::Define | RegState::Implicit);
    } else {
      // __chkstk(MSVCRT): does not update stack pointer.
      // Clobbers R10, R11 and EFLAGS.
      BuildMI(*BB, MI, DL, TII->get(X86::W64ALLOCA))
        .addExternalSymbol("__chkstk")
        .addReg(X86::RAX, RegState::Implicit)
        .addReg(X86::EFLAGS, RegState::Define | RegState::Implicit);
      // RAX has the offset to be subtracted from RSP.
      BuildMI(*BB, MI, DL, TII->get(X86::SUB64rr), X86::RSP)
        .addReg(X86::RSP)
        .addReg(X86::RAX);
    }
  } else {
    const char *StackProbeSymbol =
      Subtarget->isTargetKnownWindowsMSVC() ? "_chkstk" : "_alloca";

    BuildMI(*BB, MI, DL, TII->get(X86::CALLpcrel32))
      .addExternalSymbol(StackProbeSymbol)
      .addReg(X86::EAX, RegState::Implicit)
      .addReg(X86::ESP, RegState::Implicit)
      .addReg(X86::EAX, RegState::Define | RegState::Implicit)
      .addReg(X86::ESP, RegState::Define | RegState::Implicit)
      .addReg(X86::EFLAGS, RegState::Define | RegState::Implicit);
  }

  MI->eraseFromParent();   // The pseudo instruction is gone now.
  return BB;
}

MachineBasicBlock *
X86TargetLowering::EmitLoweredTLSCall(MachineInstr *MI,
                                      MachineBasicBlock *BB) const {
  // This is pretty easy.  We're taking the value that we received from
  // our load from the relocation, sticking it in either RDI (x86-64)
  // or EAX and doing an indirect call.  The return value will then
  // be in the normal return register.
  MachineFunction *F = BB->getParent();
  const X86InstrInfo *TII =
      static_cast<const X86InstrInfo *>(F->getSubtarget().getInstrInfo());
  DebugLoc DL = MI->getDebugLoc();

  assert(Subtarget->isTargetDarwin() && "Darwin only instr emitted?");
  assert(MI->getOperand(3).isGlobal() && "This should be a global");

  // Get a register mask for the lowered call.
  // FIXME: The 32-bit calls have non-standard calling conventions. Use a
  // proper register mask.
  const uint32_t *RegMask = F->getTarget()
                                .getSubtargetImpl()
                                ->getRegisterInfo()
                                ->getCallPreservedMask(CallingConv::C);
  if (Subtarget->is64Bit()) {
    MachineInstrBuilder MIB = BuildMI(*BB, MI, DL,
                                      TII->get(X86::MOV64rm), X86::RDI)
    .addReg(X86::RIP)
    .addImm(0).addReg(0)
    .addGlobalAddress(MI->getOperand(3).getGlobal(), 0,
                      MI->getOperand(3).getTargetFlags())
    .addReg(0);
    MIB = BuildMI(*BB, MI, DL, TII->get(X86::CALL64m));
    addDirectMem(MIB, X86::RDI);
    MIB.addReg(X86::RAX, RegState::ImplicitDefine).addRegMask(RegMask);
  } else if (F->getTarget().getRelocationModel() != Reloc::PIC_) {
    MachineInstrBuilder MIB = BuildMI(*BB, MI, DL,
                                      TII->get(X86::MOV32rm), X86::EAX)
    .addReg(0)
    .addImm(0).addReg(0)
    .addGlobalAddress(MI->getOperand(3).getGlobal(), 0,
                      MI->getOperand(3).getTargetFlags())
    .addReg(0);
    MIB = BuildMI(*BB, MI, DL, TII->get(X86::CALL32m));
    addDirectMem(MIB, X86::EAX);
    MIB.addReg(X86::EAX, RegState::ImplicitDefine).addRegMask(RegMask);
  } else {
    MachineInstrBuilder MIB = BuildMI(*BB, MI, DL,
                                      TII->get(X86::MOV32rm), X86::EAX)
    .addReg(TII->getGlobalBaseReg(F))
    .addImm(0).addReg(0)
    .addGlobalAddress(MI->getOperand(3).getGlobal(), 0,
                      MI->getOperand(3).getTargetFlags())
    .addReg(0);
    MIB = BuildMI(*BB, MI, DL, TII->get(X86::CALL32m));
    addDirectMem(MIB, X86::EAX);
    MIB.addReg(X86::EAX, RegState::ImplicitDefine).addRegMask(RegMask);
  }

  MI->eraseFromParent(); // The pseudo instruction is gone now.
  return BB;
}

MachineBasicBlock *
X86TargetLowering::emitEHSjLjSetJmp(MachineInstr *MI,
                                    MachineBasicBlock *MBB) const {
  DebugLoc DL = MI->getDebugLoc();
  MachineFunction *MF = MBB->getParent();
  const TargetInstrInfo *TII = MF->getSubtarget().getInstrInfo();
  MachineRegisterInfo &MRI = MF->getRegInfo();

  const BasicBlock *BB = MBB->getBasicBlock();
  MachineFunction::iterator I = MBB;
  ++I;

  // Memory Reference
  MachineInstr::mmo_iterator MMOBegin = MI->memoperands_begin();
  MachineInstr::mmo_iterator MMOEnd = MI->memoperands_end();

  unsigned DstReg;
  unsigned MemOpndSlot = 0;

  unsigned CurOp = 0;

  DstReg = MI->getOperand(CurOp++).getReg();
  const TargetRegisterClass *RC = MRI.getRegClass(DstReg);
  assert(RC->hasType(MVT::i32) && "Invalid destination!");
  unsigned mainDstReg = MRI.createVirtualRegister(RC);
  unsigned restoreDstReg = MRI.createVirtualRegister(RC);

  MemOpndSlot = CurOp;

  MVT PVT = getPointerTy();
  assert((PVT == MVT::i64 || PVT == MVT::i32) &&
         "Invalid Pointer Size!");

  // For v = setjmp(buf), we generate
  //
  // thisMBB:
  //  buf[LabelOffset] = restoreMBB
  //  SjLjSetup restoreMBB
  //
  // mainMBB:
  //  v_main = 0
  //
  // sinkMBB:
  //  v = phi(main, restore)
  //
  // restoreMBB:
  //  v_restore = 1

  MachineBasicBlock *thisMBB = MBB;
  MachineBasicBlock *mainMBB = MF->CreateMachineBasicBlock(BB);
  MachineBasicBlock *sinkMBB = MF->CreateMachineBasicBlock(BB);
  MachineBasicBlock *restoreMBB = MF->CreateMachineBasicBlock(BB);
  MF->insert(I, mainMBB);
  MF->insert(I, sinkMBB);
  MF->push_back(restoreMBB);

  MachineInstrBuilder MIB;

  // Transfer the remainder of BB and its successor edges to sinkMBB.
  sinkMBB->splice(sinkMBB->begin(), MBB,
                  std::next(MachineBasicBlock::iterator(MI)), MBB->end());
  sinkMBB->transferSuccessorsAndUpdatePHIs(MBB);

  // thisMBB:
  unsigned PtrStoreOpc = 0;
  unsigned LabelReg = 0;
  const int64_t LabelOffset = 1 * PVT.getStoreSize();
  Reloc::Model RM = MF->getTarget().getRelocationModel();
  bool UseImmLabel = (MF->getTarget().getCodeModel() == CodeModel::Small) &&
                     (RM == Reloc::Static || RM == Reloc::DynamicNoPIC);

  // Prepare IP either in reg or imm.
  if (!UseImmLabel) {
    PtrStoreOpc = (PVT == MVT::i64) ? X86::MOV64mr : X86::MOV32mr;
    const TargetRegisterClass *PtrRC = getRegClassFor(PVT);
    LabelReg = MRI.createVirtualRegister(PtrRC);
    if (Subtarget->is64Bit()) {
      MIB = BuildMI(*thisMBB, MI, DL, TII->get(X86::LEA64r), LabelReg)
              .addReg(X86::RIP)
              .addImm(0)
              .addReg(0)
              .addMBB(restoreMBB)
              .addReg(0);
    } else {
      const X86InstrInfo *XII = static_cast<const X86InstrInfo*>(TII);
      MIB = BuildMI(*thisMBB, MI, DL, TII->get(X86::LEA32r), LabelReg)
              .addReg(XII->getGlobalBaseReg(MF))
              .addImm(0)
              .addReg(0)
              .addMBB(restoreMBB, Subtarget->ClassifyBlockAddressReference())
              .addReg(0);
    }
  } else
    PtrStoreOpc = (PVT == MVT::i64) ? X86::MOV64mi32 : X86::MOV32mi;
  // Store IP
  MIB = BuildMI(*thisMBB, MI, DL, TII->get(PtrStoreOpc));
  for (unsigned i = 0; i < X86::AddrNumOperands; ++i) {
    if (i == X86::AddrDisp)
      MIB.addDisp(MI->getOperand(MemOpndSlot + i), LabelOffset);
    else
      MIB.addOperand(MI->getOperand(MemOpndSlot + i));
  }
  if (!UseImmLabel)
    MIB.addReg(LabelReg);
  else
    MIB.addMBB(restoreMBB);
  MIB.setMemRefs(MMOBegin, MMOEnd);
  // Setup
  MIB = BuildMI(*thisMBB, MI, DL, TII->get(X86::EH_SjLj_Setup))
          .addMBB(restoreMBB);

  const X86RegisterInfo *RegInfo = static_cast<const X86RegisterInfo *>(
      MF->getSubtarget().getRegisterInfo());
  MIB.addRegMask(RegInfo->getNoPreservedMask());
  thisMBB->addSuccessor(mainMBB);
  thisMBB->addSuccessor(restoreMBB);

  // mainMBB:
  //  EAX = 0
  BuildMI(mainMBB, DL, TII->get(X86::MOV32r0), mainDstReg);
  mainMBB->addSuccessor(sinkMBB);

  // sinkMBB:
  BuildMI(*sinkMBB, sinkMBB->begin(), DL,
          TII->get(X86::PHI), DstReg)
    .addReg(mainDstReg).addMBB(mainMBB)
    .addReg(restoreDstReg).addMBB(restoreMBB);

  // restoreMBB:
  BuildMI(restoreMBB, DL, TII->get(X86::MOV32ri), restoreDstReg).addImm(1);
  BuildMI(restoreMBB, DL, TII->get(X86::JMP_4)).addMBB(sinkMBB);
  restoreMBB->addSuccessor(sinkMBB);

  MI->eraseFromParent();
  return sinkMBB;
}

MachineBasicBlock *
X86TargetLowering::emitEHSjLjLongJmp(MachineInstr *MI,
                                     MachineBasicBlock *MBB) const {
  DebugLoc DL = MI->getDebugLoc();
  MachineFunction *MF = MBB->getParent();
  const TargetInstrInfo *TII = MF->getSubtarget().getInstrInfo();
  MachineRegisterInfo &MRI = MF->getRegInfo();

  // Memory Reference
  MachineInstr::mmo_iterator MMOBegin = MI->memoperands_begin();
  MachineInstr::mmo_iterator MMOEnd = MI->memoperands_end();

  MVT PVT = getPointerTy();
  assert((PVT == MVT::i64 || PVT == MVT::i32) &&
         "Invalid Pointer Size!");

  const TargetRegisterClass *RC =
    (PVT == MVT::i64) ? &X86::GR64RegClass : &X86::GR32RegClass;
  unsigned Tmp = MRI.createVirtualRegister(RC);
  // Since FP is only updated here but NOT referenced, it's treated as GPR.
  const X86RegisterInfo *RegInfo = static_cast<const X86RegisterInfo *>(
      MF->getSubtarget().getRegisterInfo());
  unsigned FP = (PVT == MVT::i64) ? X86::RBP : X86::EBP;
  unsigned SP = RegInfo->getStackRegister();

  MachineInstrBuilder MIB;

  const int64_t LabelOffset = 1 * PVT.getStoreSize();
  const int64_t SPOffset = 2 * PVT.getStoreSize();

  unsigned PtrLoadOpc = (PVT == MVT::i64) ? X86::MOV64rm : X86::MOV32rm;
  unsigned IJmpOpc = (PVT == MVT::i64) ? X86::JMP64r : X86::JMP32r;

  // Reload FP
  MIB = BuildMI(*MBB, MI, DL, TII->get(PtrLoadOpc), FP);
  for (unsigned i = 0; i < X86::AddrNumOperands; ++i)
    MIB.addOperand(MI->getOperand(i));
  MIB.setMemRefs(MMOBegin, MMOEnd);
  // Reload IP
  MIB = BuildMI(*MBB, MI, DL, TII->get(PtrLoadOpc), Tmp);
  for (unsigned i = 0; i < X86::AddrNumOperands; ++i) {
    if (i == X86::AddrDisp)
      MIB.addDisp(MI->getOperand(i), LabelOffset);
    else
      MIB.addOperand(MI->getOperand(i));
  }
  MIB.setMemRefs(MMOBegin, MMOEnd);
  // Reload SP
  MIB = BuildMI(*MBB, MI, DL, TII->get(PtrLoadOpc), SP);
  for (unsigned i = 0; i < X86::AddrNumOperands; ++i) {
    if (i == X86::AddrDisp)
      MIB.addDisp(MI->getOperand(i), SPOffset);
    else
      MIB.addOperand(MI->getOperand(i));
  }
  MIB.setMemRefs(MMOBegin, MMOEnd);
  // Jump
  BuildMI(*MBB, MI, DL, TII->get(IJmpOpc)).addReg(Tmp);

  MI->eraseFromParent();
  return MBB;
}

// Replace 213-type (isel default) FMA3 instructions with 231-type for
// accumulator loops. Writing back to the accumulator allows the coalescer
// to remove extra copies in the loop.   
MachineBasicBlock *
X86TargetLowering::emitFMA3Instr(MachineInstr *MI,
                                 MachineBasicBlock *MBB) const {
  MachineOperand &AddendOp = MI->getOperand(3);

  // Bail out early if the addend isn't a register - we can't switch these.
  if (!AddendOp.isReg())
    return MBB;

  MachineFunction &MF = *MBB->getParent();
  MachineRegisterInfo &MRI = MF.getRegInfo();

  // Check whether the addend is defined by a PHI:
  assert(MRI.hasOneDef(AddendOp.getReg()) && "Multiple defs in SSA?");
  MachineInstr &AddendDef = *MRI.def_instr_begin(AddendOp.getReg());
  if (!AddendDef.isPHI())
    return MBB;

  // Look for the following pattern:
  // loop:
  //   %addend = phi [%entry, 0], [%loop, %result]
  //   ...
  //   %result<tied1> = FMA213 %m2<tied0>, %m1, %addend

  // Replace with:
  //   loop:
  //   %addend = phi [%entry, 0], [%loop, %result]
  //   ...
  //   %result<tied1> = FMA231 %addend<tied0>, %m1, %m2

  for (unsigned i = 1, e = AddendDef.getNumOperands(); i < e; i += 2) {
    assert(AddendDef.getOperand(i).isReg());
    MachineOperand PHISrcOp = AddendDef.getOperand(i);
    MachineInstr &PHISrcInst = *MRI.def_instr_begin(PHISrcOp.getReg());
    if (&PHISrcInst == MI) {
      // Found a matching instruction.
      unsigned NewFMAOpc = 0;
      switch (MI->getOpcode()) {
        case X86::VFMADDPDr213r: NewFMAOpc = X86::VFMADDPDr231r; break;
        case X86::VFMADDPSr213r: NewFMAOpc = X86::VFMADDPSr231r; break;
        case X86::VFMADDSDr213r: NewFMAOpc = X86::VFMADDSDr231r; break;
        case X86::VFMADDSSr213r: NewFMAOpc = X86::VFMADDSSr231r; break;
        case X86::VFMSUBPDr213r: NewFMAOpc = X86::VFMSUBPDr231r; break;
        case X86::VFMSUBPSr213r: NewFMAOpc = X86::VFMSUBPSr231r; break;
        case X86::VFMSUBSDr213r: NewFMAOpc = X86::VFMSUBSDr231r; break;
        case X86::VFMSUBSSr213r: NewFMAOpc = X86::VFMSUBSSr231r; break;
        case X86::VFNMADDPDr213r: NewFMAOpc = X86::VFNMADDPDr231r; break;
        case X86::VFNMADDPSr213r: NewFMAOpc = X86::VFNMADDPSr231r; break;
        case X86::VFNMADDSDr213r: NewFMAOpc = X86::VFNMADDSDr231r; break;
        case X86::VFNMADDSSr213r: NewFMAOpc = X86::VFNMADDSSr231r; break;
        case X86::VFNMSUBPDr213r: NewFMAOpc = X86::VFNMSUBPDr231r; break;
        case X86::VFNMSUBPSr213r: NewFMAOpc = X86::VFNMSUBPSr231r; break;
        case X86::VFNMSUBSDr213r: NewFMAOpc = X86::VFNMSUBSDr231r; break;
        case X86::VFNMSUBSSr213r: NewFMAOpc = X86::VFNMSUBSSr231r; break;
        case X86::VFMADDPDr213rY: NewFMAOpc = X86::VFMADDPDr231rY; break;
        case X86::VFMADDPSr213rY: NewFMAOpc = X86::VFMADDPSr231rY; break;
        case X86::VFMSUBPDr213rY: NewFMAOpc = X86::VFMSUBPDr231rY; break;
        case X86::VFMSUBPSr213rY: NewFMAOpc = X86::VFMSUBPSr231rY; break;
        case X86::VFNMADDPDr213rY: NewFMAOpc = X86::VFNMADDPDr231rY; break;
        case X86::VFNMADDPSr213rY: NewFMAOpc = X86::VFNMADDPSr231rY; break;
        case X86::VFNMSUBPDr213rY: NewFMAOpc = X86::VFNMSUBPDr231rY; break;
        case X86::VFNMSUBPSr213rY: NewFMAOpc = X86::VFNMSUBPSr231rY; break;
        default: llvm_unreachable("Unrecognized FMA variant.");
      }

      const TargetInstrInfo &TII = *MF.getSubtarget().getInstrInfo();
      MachineInstrBuilder MIB =
        BuildMI(MF, MI->getDebugLoc(), TII.get(NewFMAOpc))
        .addOperand(MI->getOperand(0))
        .addOperand(MI->getOperand(3))
        .addOperand(MI->getOperand(2))
        .addOperand(MI->getOperand(1));
      MBB->insert(MachineBasicBlock::iterator(MI), MIB);
      MI->eraseFromParent();
    }
  }

  return MBB;
}

MachineBasicBlock *
X86TargetLowering::EmitInstrWithCustomInserter(MachineInstr *MI,
                                               MachineBasicBlock *BB) const {
  switch (MI->getOpcode()) {
  default: llvm_unreachable("Unexpected instr type to insert");
  case X86::TAILJMPd64:
  case X86::TAILJMPr64:
  case X86::TAILJMPm64:
    llvm_unreachable("TAILJMP64 would not be touched here.");
  case X86::TCRETURNdi64:
  case X86::TCRETURNri64:
  case X86::TCRETURNmi64:
    return BB;
  case X86::WIN_ALLOCA:
    return EmitLoweredWinAlloca(MI, BB);
  case X86::SEG_ALLOCA_32:
    return EmitLoweredSegAlloca(MI, BB, false);
  case X86::SEG_ALLOCA_64:
    return EmitLoweredSegAlloca(MI, BB, true);
  case X86::TLSCall_32:
  case X86::TLSCall_64:
    return EmitLoweredTLSCall(MI, BB);
  case X86::CMOV_GR8:
  case X86::CMOV_FR32:
  case X86::CMOV_FR64:
  case X86::CMOV_V4F32:
  case X86::CMOV_V2F64:
  case X86::CMOV_V2I64:
  case X86::CMOV_V8F32:
  case X86::CMOV_V4F64:
  case X86::CMOV_V4I64:
  case X86::CMOV_V16F32:
  case X86::CMOV_V8F64:
  case X86::CMOV_V8I64:
  case X86::CMOV_GR16:
  case X86::CMOV_GR32:
  case X86::CMOV_RFP32:
  case X86::CMOV_RFP64:
  case X86::CMOV_RFP80:
    return EmitLoweredSelect(MI, BB);

  case X86::FP32_TO_INT16_IN_MEM:
  case X86::FP32_TO_INT32_IN_MEM:
  case X86::FP32_TO_INT64_IN_MEM:
  case X86::FP64_TO_INT16_IN_MEM:
  case X86::FP64_TO_INT32_IN_MEM:
  case X86::FP64_TO_INT64_IN_MEM:
  case X86::FP80_TO_INT16_IN_MEM:
  case X86::FP80_TO_INT32_IN_MEM:
  case X86::FP80_TO_INT64_IN_MEM: {
    MachineFunction *F = BB->getParent();
    const TargetInstrInfo *TII = F->getSubtarget().getInstrInfo();
    DebugLoc DL = MI->getDebugLoc();

    // Change the floating point control register to use "round towards zero"
    // mode when truncating to an integer value.
    int CWFrameIdx = F->getFrameInfo()->CreateStackObject(2, 2, false);
    addFrameReference(BuildMI(*BB, MI, DL,
                              TII->get(X86::FNSTCW16m)), CWFrameIdx);

    // Load the old value of the high byte of the control word...
    unsigned OldCW =
      F->getRegInfo().createVirtualRegister(&X86::GR16RegClass);
    addFrameReference(BuildMI(*BB, MI, DL, TII->get(X86::MOV16rm), OldCW),
                      CWFrameIdx);

    // Set the high part to be round to zero...
    addFrameReference(BuildMI(*BB, MI, DL, TII->get(X86::MOV16mi)), CWFrameIdx)
      .addImm(0xC7F);

    // Reload the modified control word now...
    addFrameReference(BuildMI(*BB, MI, DL,
                              TII->get(X86::FLDCW16m)), CWFrameIdx);

    // Restore the memory image of control word to original value
    addFrameReference(BuildMI(*BB, MI, DL, TII->get(X86::MOV16mr)), CWFrameIdx)
      .addReg(OldCW);

    // Get the X86 opcode to use.
    unsigned Opc;
    switch (MI->getOpcode()) {
    default: llvm_unreachable("illegal opcode!");
    case X86::FP32_TO_INT16_IN_MEM: Opc = X86::IST_Fp16m32; break;
    case X86::FP32_TO_INT32_IN_MEM: Opc = X86::IST_Fp32m32; break;
    case X86::FP32_TO_INT64_IN_MEM: Opc = X86::IST_Fp64m32; break;
    case X86::FP64_TO_INT16_IN_MEM: Opc = X86::IST_Fp16m64; break;
    case X86::FP64_TO_INT32_IN_MEM: Opc = X86::IST_Fp32m64; break;
    case X86::FP64_TO_INT64_IN_MEM: Opc = X86::IST_Fp64m64; break;
    case X86::FP80_TO_INT16_IN_MEM: Opc = X86::IST_Fp16m80; break;
    case X86::FP80_TO_INT32_IN_MEM: Opc = X86::IST_Fp32m80; break;
    case X86::FP80_TO_INT64_IN_MEM: Opc = X86::IST_Fp64m80; break;
    }

    X86AddressMode AM;
    MachineOperand &Op = MI->getOperand(0);
    if (Op.isReg()) {
      AM.BaseType = X86AddressMode::RegBase;
      AM.Base.Reg = Op.getReg();
    } else {
      AM.BaseType = X86AddressMode::FrameIndexBase;
      AM.Base.FrameIndex = Op.getIndex();
    }
    Op = MI->getOperand(1);
    if (Op.isImm())
      AM.Scale = Op.getImm();
    Op = MI->getOperand(2);
    if (Op.isImm())
      AM.IndexReg = Op.getImm();
    Op = MI->getOperand(3);
    if (Op.isGlobal()) {
      AM.GV = Op.getGlobal();
    } else {
      AM.Disp = Op.getImm();
    }
    addFullAddress(BuildMI(*BB, MI, DL, TII->get(Opc)), AM)
                      .addReg(MI->getOperand(X86::AddrNumOperands).getReg());

    // Reload the original control word now.
    addFrameReference(BuildMI(*BB, MI, DL,
                              TII->get(X86::FLDCW16m)), CWFrameIdx);

    MI->eraseFromParent();   // The pseudo instruction is gone now.
    return BB;
  }
    // String/text processing lowering.
  case X86::PCMPISTRM128REG:
  case X86::VPCMPISTRM128REG:
  case X86::PCMPISTRM128MEM:
  case X86::VPCMPISTRM128MEM:
  case X86::PCMPESTRM128REG:
  case X86::VPCMPESTRM128REG:
  case X86::PCMPESTRM128MEM:
  case X86::VPCMPESTRM128MEM:
    assert(Subtarget->hasSSE42() &&
           "Target must have SSE4.2 or AVX features enabled");
    return EmitPCMPSTRM(MI, BB, BB->getParent()->getSubtarget().getInstrInfo());

  // String/text processing lowering.
  case X86::PCMPISTRIREG:
  case X86::VPCMPISTRIREG:
  case X86::PCMPISTRIMEM:
  case X86::VPCMPISTRIMEM:
  case X86::PCMPESTRIREG:
  case X86::VPCMPESTRIREG:
  case X86::PCMPESTRIMEM:
  case X86::VPCMPESTRIMEM:
    assert(Subtarget->hasSSE42() &&
           "Target must have SSE4.2 or AVX features enabled");
    return EmitPCMPSTRI(MI, BB, BB->getParent()->getSubtarget().getInstrInfo());

  // Thread synchronization.
  case X86::MONITOR:
    return EmitMonitor(MI, BB, BB->getParent()->getSubtarget().getInstrInfo(),
                       Subtarget);

  // xbegin
  case X86::XBEGIN:
    return EmitXBegin(MI, BB, BB->getParent()->getSubtarget().getInstrInfo());

  case X86::VASTART_SAVE_XMM_REGS:
    return EmitVAStartSaveXMMRegsWithCustomInserter(MI, BB);

  case X86::VAARG_64:
    return EmitVAARG64WithCustomInserter(MI, BB);

  case X86::EH_SjLj_SetJmp32:
  case X86::EH_SjLj_SetJmp64:
    return emitEHSjLjSetJmp(MI, BB);

  case X86::EH_SjLj_LongJmp32:
  case X86::EH_SjLj_LongJmp64:
    return emitEHSjLjLongJmp(MI, BB);

  case TargetOpcode::STACKMAP:
  case TargetOpcode::PATCHPOINT:
    return emitPatchPoint(MI, BB);

  case X86::VFMADDPDr213r:
  case X86::VFMADDPSr213r:
  case X86::VFMADDSDr213r:
  case X86::VFMADDSSr213r:
  case X86::VFMSUBPDr213r:
  case X86::VFMSUBPSr213r:
  case X86::VFMSUBSDr213r:
  case X86::VFMSUBSSr213r:
  case X86::VFNMADDPDr213r:
  case X86::VFNMADDPSr213r:
  case X86::VFNMADDSDr213r:
  case X86::VFNMADDSSr213r:
  case X86::VFNMSUBPDr213r:
  case X86::VFNMSUBPSr213r:
  case X86::VFNMSUBSDr213r:
  case X86::VFNMSUBSSr213r:
  case X86::VFMADDPDr213rY:
  case X86::VFMADDPSr213rY:
  case X86::VFMSUBPDr213rY:
  case X86::VFMSUBPSr213rY:
  case X86::VFNMADDPDr213rY:
  case X86::VFNMADDPSr213rY:
  case X86::VFNMSUBPDr213rY:
  case X86::VFNMSUBPSr213rY:
    return emitFMA3Instr(MI, BB);
  }
}

//===----------------------------------------------------------------------===//
//                           X86 Optimization Hooks
//===----------------------------------------------------------------------===//

void X86TargetLowering::computeKnownBitsForTargetNode(const SDValue Op,
                                                      APInt &KnownZero,
                                                      APInt &KnownOne,
                                                      const SelectionDAG &DAG,
                                                      unsigned Depth) const {
  unsigned BitWidth = KnownZero.getBitWidth();
  unsigned Opc = Op.getOpcode();
  assert((Opc >= ISD::BUILTIN_OP_END ||
          Opc == ISD::INTRINSIC_WO_CHAIN ||
          Opc == ISD::INTRINSIC_W_CHAIN ||
          Opc == ISD::INTRINSIC_VOID) &&
         "Should use MaskedValueIsZero if you don't know whether Op"
         " is a target node!");

  KnownZero = KnownOne = APInt(BitWidth, 0);   // Don't know anything.
  switch (Opc) {
  default: break;
  case X86ISD::ADD:
  case X86ISD::SUB:
  case X86ISD::ADC:
  case X86ISD::SBB:
  case X86ISD::SMUL:
  case X86ISD::UMUL:
  case X86ISD::INC:
  case X86ISD::DEC:
  case X86ISD::OR:
  case X86ISD::XOR:
  case X86ISD::AND:
    // These nodes' second result is a boolean.
    if (Op.getResNo() == 0)
      break;
    // Fallthrough
  case X86ISD::SETCC:
    KnownZero |= APInt::getHighBitsSet(BitWidth, BitWidth - 1);
    break;
  case ISD::INTRINSIC_WO_CHAIN: {
    unsigned IntId = cast<ConstantSDNode>(Op.getOperand(0))->getZExtValue();
    unsigned NumLoBits = 0;
    switch (IntId) {
    default: break;
    case Intrinsic::x86_sse_movmsk_ps:
    case Intrinsic::x86_avx_movmsk_ps_256:
    case Intrinsic::x86_sse2_movmsk_pd:
    case Intrinsic::x86_avx_movmsk_pd_256:
    case Intrinsic::x86_mmx_pmovmskb:
    case Intrinsic::x86_sse2_pmovmskb_128:
    case Intrinsic::x86_avx2_pmovmskb: {
      // High bits of movmskp{s|d}, pmovmskb are known zero.
      switch (IntId) {
        default: llvm_unreachable("Impossible intrinsic");  // Can't reach here.
        case Intrinsic::x86_sse_movmsk_ps:      NumLoBits = 4; break;
        case Intrinsic::x86_avx_movmsk_ps_256:  NumLoBits = 8; break;
        case Intrinsic::x86_sse2_movmsk_pd:     NumLoBits = 2; break;
        case Intrinsic::x86_avx_movmsk_pd_256:  NumLoBits = 4; break;
        case Intrinsic::x86_mmx_pmovmskb:       NumLoBits = 8; break;
        case Intrinsic::x86_sse2_pmovmskb_128:  NumLoBits = 16; break;
        case Intrinsic::x86_avx2_pmovmskb:      NumLoBits = 32; break;
      }
      KnownZero = APInt::getHighBitsSet(BitWidth, BitWidth - NumLoBits);
      break;
    }
    }
    break;
  }
  }
}

unsigned X86TargetLowering::ComputeNumSignBitsForTargetNode(
  SDValue Op,
  const SelectionDAG &,
  unsigned Depth) const {
  // SETCC_CARRY sets the dest to ~0 for true or 0 for false.
  if (Op.getOpcode() == X86ISD::SETCC_CARRY)
    return Op.getValueType().getScalarType().getSizeInBits();

  // Fallback case.
  return 1;
}

/// isGAPlusOffset - Returns true (and the GlobalValue and the offset) if the
/// node is a GlobalAddress + offset.
bool X86TargetLowering::isGAPlusOffset(SDNode *N,
                                       const GlobalValue* &GA,
                                       int64_t &Offset) const {
  if (N->getOpcode() == X86ISD::Wrapper) {
    if (isa<GlobalAddressSDNode>(N->getOperand(0))) {
      GA = cast<GlobalAddressSDNode>(N->getOperand(0))->getGlobal();
      Offset = cast<GlobalAddressSDNode>(N->getOperand(0))->getOffset();
      return true;
    }
  }
  return TargetLowering::isGAPlusOffset(N, GA, Offset);
}

/// isShuffleHigh128VectorInsertLow - Checks whether the shuffle node is the
/// same as extracting the high 128-bit part of 256-bit vector and then
/// inserting the result into the low part of a new 256-bit vector
static bool isShuffleHigh128VectorInsertLow(ShuffleVectorSDNode *SVOp) {
  EVT VT = SVOp->getValueType(0);
  unsigned NumElems = VT.getVectorNumElements();

  // vector_shuffle <4, 5, 6, 7, u, u, u, u> or <2, 3, u, u>
  for (unsigned i = 0, j = NumElems/2; i != NumElems/2; ++i, ++j)
    if (!isUndefOrEqual(SVOp->getMaskElt(i), j) ||
        SVOp->getMaskElt(j) >= 0)
      return false;

  return true;
}

/// isShuffleLow128VectorInsertHigh - Checks whether the shuffle node is the
/// same as extracting the low 128-bit part of 256-bit vector and then
/// inserting the result into the high part of a new 256-bit vector
static bool isShuffleLow128VectorInsertHigh(ShuffleVectorSDNode *SVOp) {
  EVT VT = SVOp->getValueType(0);
  unsigned NumElems = VT.getVectorNumElements();

  // vector_shuffle <u, u, u, u, 0, 1, 2, 3> or <u, u, 0, 1>
  for (unsigned i = NumElems/2, j = 0; i != NumElems; ++i, ++j)
    if (!isUndefOrEqual(SVOp->getMaskElt(i), j) ||
        SVOp->getMaskElt(j) >= 0)
      return false;

  return true;
}

/// PerformShuffleCombine256 - Performs shuffle combines for 256-bit vectors.
static SDValue PerformShuffleCombine256(SDNode *N, SelectionDAG &DAG,
                                        TargetLowering::DAGCombinerInfo &DCI,
                                        const X86Subtarget* Subtarget) {
  SDLoc dl(N);
  ShuffleVectorSDNode *SVOp = cast<ShuffleVectorSDNode>(N);
  SDValue V1 = SVOp->getOperand(0);
  SDValue V2 = SVOp->getOperand(1);
  EVT VT = SVOp->getValueType(0);
  unsigned NumElems = VT.getVectorNumElements();

  if (V1.getOpcode() == ISD::CONCAT_VECTORS &&
      V2.getOpcode() == ISD::CONCAT_VECTORS) {
    //
    //                   0,0,0,...
    //                      |
    //    V      UNDEF    BUILD_VECTOR    UNDEF
    //     \      /           \           /
    //  CONCAT_VECTOR         CONCAT_VECTOR
    //         \                  /
    //          \                /
    //          RESULT: V + zero extended
    //
    if (V2.getOperand(0).getOpcode() != ISD::BUILD_VECTOR ||
        V2.getOperand(1).getOpcode() != ISD::UNDEF ||
        V1.getOperand(1).getOpcode() != ISD::UNDEF)
      return SDValue();

    if (!ISD::isBuildVectorAllZeros(V2.getOperand(0).getNode()))
      return SDValue();

    // To match the shuffle mask, the first half of the mask should
    // be exactly the first vector, and all the rest a splat with the
    // first element of the second one.
    for (unsigned i = 0; i != NumElems/2; ++i)
      if (!isUndefOrEqual(SVOp->getMaskElt(i), i) ||
          !isUndefOrEqual(SVOp->getMaskElt(i+NumElems/2), NumElems))
        return SDValue();

    // If V1 is coming from a vector load then just fold to a VZEXT_LOAD.
    if (LoadSDNode *Ld = dyn_cast<LoadSDNode>(V1.getOperand(0))) {
      if (Ld->hasNUsesOfValue(1, 0)) {
        SDVTList Tys = DAG.getVTList(MVT::v4i64, MVT::Other);
        SDValue Ops[] = { Ld->getChain(), Ld->getBasePtr() };
        SDValue ResNode =
          DAG.getMemIntrinsicNode(X86ISD::VZEXT_LOAD, dl, Tys, Ops,
                                  Ld->getMemoryVT(),
                                  Ld->getPointerInfo(),
                                  Ld->getAlignment(),
                                  false/*isVolatile*/, true/*ReadMem*/,
                                  false/*WriteMem*/);

        // Make sure the newly-created LOAD is in the same position as Ld in
        // terms of dependency. We create a TokenFactor for Ld and ResNode,
        // and update uses of Ld's output chain to use the TokenFactor.
        if (Ld->hasAnyUseOfValue(1)) {
          SDValue NewChain = DAG.getNode(ISD::TokenFactor, dl, MVT::Other,
                             SDValue(Ld, 1), SDValue(ResNode.getNode(), 1));
          DAG.ReplaceAllUsesOfValueWith(SDValue(Ld, 1), NewChain);
          DAG.UpdateNodeOperands(NewChain.getNode(), SDValue(Ld, 1),
                                 SDValue(ResNode.getNode(), 1));
        }

        return DAG.getNode(ISD::BITCAST, dl, VT, ResNode);
      }
    }

    // Emit a zeroed vector and insert the desired subvector on its
    // first half.
    SDValue Zeros = getZeroVector(VT, Subtarget, DAG, dl);
    SDValue InsV = Insert128BitVector(Zeros, V1.getOperand(0), 0, DAG, dl);
    return DCI.CombineTo(N, InsV);
  }

  //===--------------------------------------------------------------------===//
  // Combine some shuffles into subvector extracts and inserts:
  //

  // vector_shuffle <4, 5, 6, 7, u, u, u, u> or <2, 3, u, u>
  if (isShuffleHigh128VectorInsertLow(SVOp)) {
    SDValue V = Extract128BitVector(V1, NumElems/2, DAG, dl);
    SDValue InsV = Insert128BitVector(DAG.getUNDEF(VT), V, 0, DAG, dl);
    return DCI.CombineTo(N, InsV);
  }

  // vector_shuffle <u, u, u, u, 0, 1, 2, 3> or <u, u, 0, 1>
  if (isShuffleLow128VectorInsertHigh(SVOp)) {
    SDValue V = Extract128BitVector(V1, 0, DAG, dl);
    SDValue InsV = Insert128BitVector(DAG.getUNDEF(VT), V, NumElems/2, DAG, dl);
    return DCI.CombineTo(N, InsV);
  }

  return SDValue();
}

/// \brief Combine an arbitrary chain of shuffles into a single instruction if
/// possible.
///
/// This is the leaf of the recursive combinine below. When we have found some
/// chain of single-use x86 shuffle instructions and accumulated the combined
/// shuffle mask represented by them, this will try to pattern match that mask
/// into either a single instruction if there is a special purpose instruction
/// for this operation, or into a PSHUFB instruction which is a fully general
/// instruction but should only be used to replace chains over a certain depth.
static bool combineX86ShuffleChain(SDValue Op, SDValue Root, ArrayRef<int> Mask,
                                   int Depth, bool HasPSHUFB, SelectionDAG &DAG,
                                   TargetLowering::DAGCombinerInfo &DCI,
                                   const X86Subtarget *Subtarget) {
  assert(!Mask.empty() && "Cannot combine an empty shuffle mask!");

  // Find the operand that enters the chain. Note that multiple uses are OK
  // here, we're not going to remove the operand we find.
  SDValue Input = Op.getOperand(0);
  while (Input.getOpcode() == ISD::BITCAST)
    Input = Input.getOperand(0);

  MVT VT = Input.getSimpleValueType();
  MVT RootVT = Root.getSimpleValueType();
  SDLoc DL(Root);

  // Just remove no-op shuffle masks.
  if (Mask.size() == 1) {
    DCI.CombineTo(Root.getNode(), DAG.getNode(ISD::BITCAST, DL, RootVT, Input),
                  /*AddTo*/ true);
    return true;
  }

  // Use the float domain if the operand type is a floating point type.
  bool FloatDomain = VT.isFloatingPoint();

  // If we don't have access to VEX encodings, the generic PSHUF instructions
  // are preferable to some of the specialized forms despite requiring one more
  // byte to encode because they can implicitly copy.
  //
  // IF we *do* have VEX encodings, than we can use shorter, more specific
  // shuffle instructions freely as they can copy due to the extra register
  // operand.
  if (Subtarget->hasAVX()) {
    // We have both floating point and integer variants of shuffles that dup
    // either the low or high half of the vector.
    if (Mask.equals(0, 0) || Mask.equals(1, 1)) {
      bool Lo = Mask.equals(0, 0);
      unsigned Shuffle = FloatDomain ? (Lo ? X86ISD::MOVLHPS : X86ISD::MOVHLPS)
                                     : (Lo ? X86ISD::UNPCKL : X86ISD::UNPCKH);
      if (Depth == 1 && Root->getOpcode() == Shuffle)
        return false; // Nothing to do!
      MVT ShuffleVT = FloatDomain ? MVT::v4f32 : MVT::v2i64;
      Op = DAG.getNode(ISD::BITCAST, DL, ShuffleVT, Input);
      DCI.AddToWorklist(Op.getNode());
      Op = DAG.getNode(Shuffle, DL, ShuffleVT, Op, Op);
      DCI.AddToWorklist(Op.getNode());
      DCI.CombineTo(Root.getNode(), DAG.getNode(ISD::BITCAST, DL, RootVT, Op),
                    /*AddTo*/ true);
      return true;
    }

    // FIXME: We should match UNPCKLPS and UNPCKHPS here.

    // For the integer domain we have specialized instructions for duplicating
    // any element size from the low or high half.
    if (!FloatDomain &&
        (Mask.equals(0, 0, 1, 1) || Mask.equals(2, 2, 3, 3) ||
         Mask.equals(0, 0, 1, 1, 2, 2, 3, 3) ||
         Mask.equals(4, 4, 5, 5, 6, 6, 7, 7) ||
         Mask.equals(0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7) ||
         Mask.equals(8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13, 14, 14, 15,
                     15))) {
      bool Lo = Mask[0] == 0;
      unsigned Shuffle = Lo ? X86ISD::UNPCKL : X86ISD::UNPCKH;
      if (Depth == 1 && Root->getOpcode() == Shuffle)
        return false; // Nothing to do!
      MVT ShuffleVT;
      switch (Mask.size()) {
      case 4: ShuffleVT = MVT::v4i32; break;
      case 8: ShuffleVT = MVT::v8i16; break;
      case 16: ShuffleVT = MVT::v16i8; break;
      };
      Op = DAG.getNode(ISD::BITCAST, DL, ShuffleVT, Input);
      DCI.AddToWorklist(Op.getNode());
      Op = DAG.getNode(Shuffle, DL, ShuffleVT, Op, Op);
      DCI.AddToWorklist(Op.getNode());
      DCI.CombineTo(Root.getNode(), DAG.getNode(ISD::BITCAST, DL, RootVT, Op),
                    /*AddTo*/ true);
      return true;
    }
  }

  // Don't try to re-form single instruction chains under any circumstances now
  // that we've done encoding canonicalization for them.
  if (Depth < 2)
    return false;

  // If we have 3 or more shuffle instructions or a chain involving PSHUFB, we
  // can replace them with a single PSHUFB instruction profitably. Intel's
  // manuals suggest only using PSHUFB if doing so replacing 5 instructions, but
  // in practice PSHUFB tends to be *very* fast so we're more aggressive.
  if ((Depth >= 3 || HasPSHUFB) && Subtarget->hasSSSE3()) {
    SmallVector<SDValue, 16> PSHUFBMask;
    assert(Mask.size() <= 16 && "Can't shuffle elements smaller than bytes!");
    int Ratio = 16 / Mask.size();
    for (unsigned i = 0; i < 16; ++i) {
      int M = Mask[i / Ratio] != SM_SentinelZero
                  ? Ratio * Mask[i / Ratio] + i % Ratio
                  : 255;
      PSHUFBMask.push_back(DAG.getConstant(M, MVT::i8));
    }
    Op = DAG.getNode(ISD::BITCAST, DL, MVT::v16i8, Input);
    DCI.AddToWorklist(Op.getNode());
    SDValue PSHUFBMaskOp =
        DAG.getNode(ISD::BUILD_VECTOR, DL, MVT::v16i8, PSHUFBMask);
    DCI.AddToWorklist(PSHUFBMaskOp.getNode());
    Op = DAG.getNode(X86ISD::PSHUFB, DL, MVT::v16i8, Op, PSHUFBMaskOp);
    DCI.AddToWorklist(Op.getNode());
    DCI.CombineTo(Root.getNode(), DAG.getNode(ISD::BITCAST, DL, RootVT, Op),
                  /*AddTo*/ true);
    return true;
  }

  // Failed to find any combines.
  return false;
}

/// \brief Fully generic combining of x86 shuffle instructions.
///
/// This should be the last combine run over the x86 shuffle instructions. Once
/// they have been fully optimized, this will recursively consider all chains
/// of single-use shuffle instructions, build a generic model of the cumulative
/// shuffle operation, and check for simpler instructions which implement this
/// operation. We use this primarily for two purposes:
///
/// 1) Collapse generic shuffles to specialized single instructions when
///    equivalent. In most cases, this is just an encoding size win, but
///    sometimes we will collapse multiple generic shuffles into a single
///    special-purpose shuffle.
/// 2) Look for sequences of shuffle instructions with 3 or more total
///    instructions, and replace them with the slightly more expensive SSSE3
///    PSHUFB instruction if available. We do this as the last combining step
///    to ensure we avoid using PSHUFB if we can implement the shuffle with
///    a suitable short sequence of other instructions. The PHUFB will either
///    use a register or have to read from memory and so is slightly (but only
///    slightly) more expensive than the other shuffle instructions.
///
/// Because this is inherently a quadratic operation (for each shuffle in
/// a chain, we recurse up the chain), the depth is limited to 8 instructions.
/// This should never be an issue in practice as the shuffle lowering doesn't
/// produce sequences of more than 8 instructions.
///
/// FIXME: We will currently miss some cases where the redundant shuffling
/// would simplify under the threshold for PSHUFB formation because of
/// combine-ordering. To fix this, we should do the redundant instruction
/// combining in this recursive walk.
static bool combineX86ShufflesRecursively(SDValue Op, SDValue Root,
                                          ArrayRef<int> RootMask,
                                          int Depth, bool HasPSHUFB,
                                          SelectionDAG &DAG,
                                          TargetLowering::DAGCombinerInfo &DCI,
                                          const X86Subtarget *Subtarget) {
  // Bound the depth of our recursive combine because this is ultimately
  // quadratic in nature.
  if (Depth > 8)
    return false;

  // Directly rip through bitcasts to find the underlying operand.
  while (Op.getOpcode() == ISD::BITCAST && Op.getOperand(0).hasOneUse())
    Op = Op.getOperand(0);

  MVT VT = Op.getSimpleValueType();
  if (!VT.isVector())
    return false; // Bail if we hit a non-vector.
  // FIXME: This routine should be taught about 256-bit shuffles, or a 256-bit
  // version should be added.
  if (VT.getSizeInBits() != 128)
    return false;

  assert(Root.getSimpleValueType().isVector() &&
         "Shuffles operate on vector types!");
  assert(VT.getSizeInBits() == Root.getSimpleValueType().getSizeInBits() &&
         "Can only combine shuffles of the same vector register size.");

  if (!isTargetShuffle(Op.getOpcode()))
    return false;
  SmallVector<int, 16> OpMask;
  bool IsUnary;
  bool HaveMask = getTargetShuffleMask(Op.getNode(), VT, OpMask, IsUnary);
  // We only can combine unary shuffles which we can decode the mask for.
  if (!HaveMask || !IsUnary)
    return false;

  assert(VT.getVectorNumElements() == OpMask.size() &&
         "Different mask size from vector size!");
  assert(((RootMask.size() > OpMask.size() &&
           RootMask.size() % OpMask.size() == 0) ||
          (OpMask.size() > RootMask.size() &&
           OpMask.size() % RootMask.size() == 0) ||
          OpMask.size() == RootMask.size()) &&
         "The smaller number of elements must divide the larger.");
  int RootRatio = std::max<int>(1, OpMask.size() / RootMask.size());
  int OpRatio = std::max<int>(1, RootMask.size() / OpMask.size());
  assert(((RootRatio == 1 && OpRatio == 1) ||
          (RootRatio == 1) != (OpRatio == 1)) &&
         "Must not have a ratio for both incoming and op masks!");

  SmallVector<int, 16> Mask;
  Mask.reserve(std::max(OpMask.size(), RootMask.size()));

  // Merge this shuffle operation's mask into our accumulated mask. Note that
  // this shuffle's mask will be the first applied to the input, followed by the
  // root mask to get us all the way to the root value arrangement. The reason
  // for this order is that we are recursing up the operation chain.
  for (int i = 0, e = std::max(OpMask.size(), RootMask.size()); i < e; ++i) {
    int RootIdx = i / RootRatio;
    if (RootMask[RootIdx] == SM_SentinelZero) {
      // This is a zero-ed lane, we're done.
      Mask.push_back(SM_SentinelZero);
      continue;
    }

    int RootMaskedIdx = RootMask[RootIdx] * RootRatio + i % RootRatio;
    int OpIdx = RootMaskedIdx / OpRatio;
    if (OpMask[OpIdx] == SM_SentinelZero) {
      // The incoming lanes are zero, it doesn't matter which ones we are using.
      Mask.push_back(SM_SentinelZero);
      continue;
    }

    // Ok, we have non-zero lanes, map them through.
    Mask.push_back(OpMask[OpIdx] * OpRatio +
                   RootMaskedIdx % OpRatio);
  }

  // See if we can recurse into the operand to combine more things.
  switch (Op.getOpcode()) {
    case X86ISD::PSHUFB:
      HasPSHUFB = true;
    case X86ISD::PSHUFD:
    case X86ISD::PSHUFHW:
    case X86ISD::PSHUFLW:
      if (Op.getOperand(0).hasOneUse() &&
          combineX86ShufflesRecursively(Op.getOperand(0), Root, Mask, Depth + 1,
                                        HasPSHUFB, DAG, DCI, Subtarget))
        return true;
      break;

    case X86ISD::UNPCKL:
    case X86ISD::UNPCKH:
      assert(Op.getOperand(0) == Op.getOperand(1) && "We only combine unary shuffles!");
      // We can't check for single use, we have to check that this shuffle is the only user.
      if (Op->isOnlyUserOf(Op.getOperand(0).getNode()) &&
          combineX86ShufflesRecursively(Op.getOperand(0), Root, Mask, Depth + 1,
                                        HasPSHUFB, DAG, DCI, Subtarget))
          return true;
      break;
  }

  // Minor canonicalization of the accumulated shuffle mask to make it easier
  // to match below. All this does is detect masks with squential pairs of
  // elements, and shrink them to the half-width mask. It does this in a loop
  // so it will reduce the size of the mask to the minimal width mask which
  // performs an equivalent shuffle.
  while (Mask.size() > 1 && canWidenShuffleElements(Mask)) {
    for (int i = 0, e = Mask.size() / 2; i < e; ++i)
      Mask[i] = Mask[2 * i] / 2;
    Mask.resize(Mask.size() / 2);
  }

  return combineX86ShuffleChain(Op, Root, Mask, Depth, HasPSHUFB, DAG, DCI,
                                Subtarget);
}

/// \brief Get the PSHUF-style mask from PSHUF node.
///
/// This is a very minor wrapper around getTargetShuffleMask to easy forming v4
/// PSHUF-style masks that can be reused with such instructions.
static SmallVector<int, 4> getPSHUFShuffleMask(SDValue N) {
  SmallVector<int, 4> Mask;
  bool IsUnary;
  bool HaveMask = getTargetShuffleMask(N.getNode(), N.getSimpleValueType(), Mask, IsUnary);
  (void)HaveMask;
  assert(HaveMask);

  switch (N.getOpcode()) {
  case X86ISD::PSHUFD:
    return Mask;
  case X86ISD::PSHUFLW:
    Mask.resize(4);
    return Mask;
  case X86ISD::PSHUFHW:
    Mask.erase(Mask.begin(), Mask.begin() + 4);
    for (int &M : Mask)
      M -= 4;
    return Mask;
  default:
    llvm_unreachable("No valid shuffle instruction found!");
  }
}

/// \brief Search for a combinable shuffle across a chain ending in pshufd.
///
/// We walk up the chain and look for a combinable shuffle, skipping over
/// shuffles that we could hoist this shuffle's transformation past without
/// altering anything.
static SDValue
combineRedundantDWordShuffle(SDValue N, MutableArrayRef<int> Mask,
                             SelectionDAG &DAG,
                             TargetLowering::DAGCombinerInfo &DCI) {
  assert(N.getOpcode() == X86ISD::PSHUFD &&
         "Called with something other than an x86 128-bit half shuffle!");
  SDLoc DL(N);

  // Walk up a single-use chain looking for a combinable shuffle. Keep a stack
  // of the shuffles in the chain so that we can form a fresh chain to replace
  // this one.
  SmallVector<SDValue, 8> Chain;
  SDValue V = N.getOperand(0);
  for (; V.hasOneUse(); V = V.getOperand(0)) {
    switch (V.getOpcode()) {
    default:
      return SDValue(); // Nothing combined!

    case ISD::BITCAST:
      // Skip bitcasts as we always know the type for the target specific
      // instructions.
      continue;

    case X86ISD::PSHUFD:
      // Found another dword shuffle.
      break;

    case X86ISD::PSHUFLW:
      // Check that the low words (being shuffled) are the identity in the
      // dword shuffle, and the high words are self-contained.
      if (Mask[0] != 0 || Mask[1] != 1 ||
          !(Mask[2] >= 2 && Mask[2] < 4 && Mask[3] >= 2 && Mask[3] < 4))
        return SDValue();

      Chain.push_back(V);
      continue;

    case X86ISD::PSHUFHW:
      // Check that the high words (being shuffled) are the identity in the
      // dword shuffle, and the low words are self-contained.
      if (Mask[2] != 2 || Mask[3] != 3 ||
          !(Mask[0] >= 0 && Mask[0] < 2 && Mask[1] >= 0 && Mask[1] < 2))
        return SDValue();

      Chain.push_back(V);
      continue;

    case X86ISD::UNPCKL:
    case X86ISD::UNPCKH:
      // For either i8 -> i16 or i16 -> i32 unpacks, we can combine a dword
      // shuffle into a preceding word shuffle.
      if (V.getValueType() != MVT::v16i8 && V.getValueType() != MVT::v8i16)
        return SDValue();

      // Search for a half-shuffle which we can combine with.
      unsigned CombineOp =
          V.getOpcode() == X86ISD::UNPCKL ? X86ISD::PSHUFLW : X86ISD::PSHUFHW;
      if (V.getOperand(0) != V.getOperand(1) ||
          !V->isOnlyUserOf(V.getOperand(0).getNode()))
        return SDValue();
      Chain.push_back(V);
      V = V.getOperand(0);
      do {
        switch (V.getOpcode()) {
        default:
          return SDValue(); // Nothing to combine.

        case X86ISD::PSHUFLW:
        case X86ISD::PSHUFHW:
          if (V.getOpcode() == CombineOp)
            break;

          Chain.push_back(V);

          // Fallthrough!
        case ISD::BITCAST:
          V = V.getOperand(0);
          continue;
        }
        break;
      } while (V.hasOneUse());
      break;
    }
    // Break out of the loop if we break out of the switch.
    break;
  }

  if (!V.hasOneUse())
    // We fell out of the loop without finding a viable combining instruction.
    return SDValue();

  // Merge this node's mask and our incoming mask.
  SmallVector<int, 4> VMask = getPSHUFShuffleMask(V);
  for (int &M : Mask)
    M = VMask[M];
  V = DAG.getNode(V.getOpcode(), DL, V.getValueType(), V.getOperand(0),
                  getV4X86ShuffleImm8ForMask(Mask, DAG));

  // Rebuild the chain around this new shuffle.
  while (!Chain.empty()) {
    SDValue W = Chain.pop_back_val();

    if (V.getValueType() != W.getOperand(0).getValueType())
      V = DAG.getNode(ISD::BITCAST, DL, W.getOperand(0).getValueType(), V);

    switch (W.getOpcode()) {
    default:
      llvm_unreachable("Only PSHUF and UNPCK instructions get here!");

    case X86ISD::UNPCKL:
    case X86ISD::UNPCKH:
      V = DAG.getNode(W.getOpcode(), DL, W.getValueType(), V, V);
      break;

    case X86ISD::PSHUFD:
    case X86ISD::PSHUFLW:
    case X86ISD::PSHUFHW:
      V = DAG.getNode(W.getOpcode(), DL, W.getValueType(), V, W.getOperand(1));
      break;
    }
  }
  if (V.getValueType() != N.getValueType())
    V = DAG.getNode(ISD::BITCAST, DL, N.getValueType(), V);

  // Return the new chain to replace N.
  return V;
}

/// \brief Search for a combinable shuffle across a chain ending in pshuflw or pshufhw.
///
/// We walk up the chain, skipping shuffles of the other half and looking
/// through shuffles which switch halves trying to find a shuffle of the same
/// pair of dwords.
static bool combineRedundantHalfShuffle(SDValue N, MutableArrayRef<int> Mask,
                                        SelectionDAG &DAG,
                                        TargetLowering::DAGCombinerInfo &DCI) {
  assert(
      (N.getOpcode() == X86ISD::PSHUFLW || N.getOpcode() == X86ISD::PSHUFHW) &&
      "Called with something other than an x86 128-bit half shuffle!");
  SDLoc DL(N);
  unsigned CombineOpcode = N.getOpcode();

  // Walk up a single-use chain looking for a combinable shuffle.
  SDValue V = N.getOperand(0);
  for (; V.hasOneUse(); V = V.getOperand(0)) {
    switch (V.getOpcode()) {
    default:
      return false; // Nothing combined!

    case ISD::BITCAST:
      // Skip bitcasts as we always know the type for the target specific
      // instructions.
      continue;

    case X86ISD::PSHUFLW:
    case X86ISD::PSHUFHW:
      if (V.getOpcode() == CombineOpcode)
        break;

      // Other-half shuffles are no-ops.
      continue;
    }
    // Break out of the loop if we break out of the switch.
    break;
  }

  if (!V.hasOneUse())
    // We fell out of the loop without finding a viable combining instruction.
    return false;

  // Combine away the bottom node as its shuffle will be accumulated into
  // a preceding shuffle.
  DCI.CombineTo(N.getNode(), N.getOperand(0), /*AddTo*/ true);

  // Record the old value.
  SDValue Old = V;

  // Merge this node's mask and our incoming mask (adjusted to account for all
  // the pshufd instructions encountered).
  SmallVector<int, 4> VMask = getPSHUFShuffleMask(V);
  for (int &M : Mask)
    M = VMask[M];
  V = DAG.getNode(V.getOpcode(), DL, MVT::v8i16, V.getOperand(0),
                  getV4X86ShuffleImm8ForMask(Mask, DAG));

  // Check that the shuffles didn't cancel each other out. If not, we need to
  // combine to the new one.
  if (Old != V)
    // Replace the combinable shuffle with the combined one, updating all users
    // so that we re-evaluate the chain here.
    DCI.CombineTo(Old.getNode(), V, /*AddTo*/ true);

  return true;
}

/// \brief Try to combine x86 target specific shuffles.
static SDValue PerformTargetShuffleCombine(SDValue N, SelectionDAG &DAG,
                                           TargetLowering::DAGCombinerInfo &DCI,
                                           const X86Subtarget *Subtarget) {
  SDLoc DL(N);
  MVT VT = N.getSimpleValueType();
  SmallVector<int, 4> Mask;

  switch (N.getOpcode()) {
  case X86ISD::PSHUFD:
  case X86ISD::PSHUFLW:
  case X86ISD::PSHUFHW:
    Mask = getPSHUFShuffleMask(N);
    assert(Mask.size() == 4);
    break;
  default:
    return SDValue();
  }

  // Nuke no-op shuffles that show up after combining.
  if (isNoopShuffleMask(Mask))
    return DCI.CombineTo(N.getNode(), N.getOperand(0), /*AddTo*/ true);

  // Look for simplifications involving one or two shuffle instructions.
  SDValue V = N.getOperand(0);
  switch (N.getOpcode()) {
  default:
    break;
  case X86ISD::PSHUFLW:
  case X86ISD::PSHUFHW:
    assert(VT == MVT::v8i16);
    (void)VT;

    if (combineRedundantHalfShuffle(N, Mask, DAG, DCI))
      return SDValue(); // We combined away this shuffle, so we're done.

    // See if this reduces to a PSHUFD which is no more expensive and can
    // combine with more operations.
    if (canWidenShuffleElements(Mask)) {
      int DMask[] = {-1, -1, -1, -1};
      int DOffset = N.getOpcode() == X86ISD::PSHUFLW ? 0 : 2;
      DMask[DOffset + 0] = DOffset + Mask[0] / 2;
      DMask[DOffset + 1] = DOffset + Mask[2] / 2;
      V = DAG.getNode(ISD::BITCAST, DL, MVT::v4i32, V);
      DCI.AddToWorklist(V.getNode());
      V = DAG.getNode(X86ISD::PSHUFD, DL, MVT::v4i32, V,
                      getV4X86ShuffleImm8ForMask(DMask, DAG));
      DCI.AddToWorklist(V.getNode());
      return DAG.getNode(ISD::BITCAST, DL, MVT::v8i16, V);
    }

    // Look for shuffle patterns which can be implemented as a single unpack.
    // FIXME: This doesn't handle the location of the PSHUFD generically, and
    // only works when we have a PSHUFD followed by two half-shuffles.
    if (Mask[0] == Mask[1] && Mask[2] == Mask[3] &&
        (V.getOpcode() == X86ISD::PSHUFLW ||
         V.getOpcode() == X86ISD::PSHUFHW) &&
        V.getOpcode() != N.getOpcode() &&
        V.hasOneUse()) {
      SDValue D = V.getOperand(0);
      while (D.getOpcode() == ISD::BITCAST && D.hasOneUse())
        D = D.getOperand(0);
      if (D.getOpcode() == X86ISD::PSHUFD && D.hasOneUse()) {
        SmallVector<int, 4> VMask = getPSHUFShuffleMask(V);
        SmallVector<int, 4> DMask = getPSHUFShuffleMask(D);
        int NOffset = N.getOpcode() == X86ISD::PSHUFLW ? 0 : 4;
        int VOffset = V.getOpcode() == X86ISD::PSHUFLW ? 0 : 4;
        int WordMask[8];
        for (int i = 0; i < 4; ++i) {
          WordMask[i + NOffset] = Mask[i] + NOffset;
          WordMask[i + VOffset] = VMask[i] + VOffset;
        }
        // Map the word mask through the DWord mask.
        int MappedMask[8];
        for (int i = 0; i < 8; ++i)
          MappedMask[i] = 2 * DMask[WordMask[i] / 2] + WordMask[i] % 2;
        const int UnpackLoMask[] = {0, 0, 1, 1, 2, 2, 3, 3};
        const int UnpackHiMask[] = {4, 4, 5, 5, 6, 6, 7, 7};
        if (std::equal(std::begin(MappedMask), std::end(MappedMask),
                       std::begin(UnpackLoMask)) ||
            std::equal(std::begin(MappedMask), std::end(MappedMask),
                       std::begin(UnpackHiMask))) {
          // We can replace all three shuffles with an unpack.
          V = DAG.getNode(ISD::BITCAST, DL, MVT::v8i16, D.getOperand(0));
          DCI.AddToWorklist(V.getNode());
          return DAG.getNode(MappedMask[0] == 0 ? X86ISD::UNPCKL
                                                : X86ISD::UNPCKH,
                             DL, MVT::v8i16, V, V);
        }
      }
    }

    break;

  case X86ISD::PSHUFD:
    if (SDValue NewN = combineRedundantDWordShuffle(N, Mask, DAG, DCI))
      return NewN;

    break;
  }

  return SDValue();
}

/// PerformShuffleCombine - Performs several different shuffle combines.
static SDValue PerformShuffleCombine(SDNode *N, SelectionDAG &DAG,
                                     TargetLowering::DAGCombinerInfo &DCI,
                                     const X86Subtarget *Subtarget) {
  SDLoc dl(N);
  SDValue N0 = N->getOperand(0);
  SDValue N1 = N->getOperand(1);
  EVT VT = N->getValueType(0);

  // Don't create instructions with illegal types after legalize types has run.
  const TargetLowering &TLI = DAG.getTargetLoweringInfo();
  if (!DCI.isBeforeLegalize() && !TLI.isTypeLegal(VT.getVectorElementType()))
    return SDValue();

  // Combine 256-bit vector shuffles. This is only profitable when in AVX mode
  if (Subtarget->hasFp256() && VT.is256BitVector() &&
      N->getOpcode() == ISD::VECTOR_SHUFFLE)
    return PerformShuffleCombine256(N, DAG, DCI, Subtarget);

  // During Type Legalization, when promoting illegal vector types,
  // the backend might introduce new shuffle dag nodes and bitcasts.
  //
  // This code performs the following transformation:
  // fold: (shuffle (bitcast (BINOP A, B)), Undef, <Mask>) ->
  //       (shuffle (BINOP (bitcast A), (bitcast B)), Undef, <Mask>)
  //
  // We do this only if both the bitcast and the BINOP dag nodes have
  // one use. Also, perform this transformation only if the new binary
  // operation is legal. This is to avoid introducing dag nodes that
  // potentially need to be further expanded (or custom lowered) into a
  // less optimal sequence of dag nodes.
  if (!DCI.isBeforeLegalize() && DCI.isBeforeLegalizeOps() &&
      N1.getOpcode() == ISD::UNDEF && N0.hasOneUse() &&
      N0.getOpcode() == ISD::BITCAST) {
    SDValue BC0 = N0.getOperand(0);
    EVT SVT = BC0.getValueType();
    unsigned Opcode = BC0.getOpcode();
    unsigned NumElts = VT.getVectorNumElements();
    
    if (BC0.hasOneUse() && SVT.isVector() &&
        SVT.getVectorNumElements() * 2 == NumElts &&
        TLI.isOperationLegal(Opcode, VT)) {
      bool CanFold = false;
      switch (Opcode) {
      default : break;
      case ISD::ADD :
      case ISD::FADD :
      case ISD::SUB :
      case ISD::FSUB :
      case ISD::MUL :
      case ISD::FMUL :
        CanFold = true;
      }

      unsigned SVTNumElts = SVT.getVectorNumElements();
      ShuffleVectorSDNode *SVOp = cast<ShuffleVectorSDNode>(N);
      for (unsigned i = 0, e = SVTNumElts; i != e && CanFold; ++i)
        CanFold = SVOp->getMaskElt(i) == (int)(i * 2);
      for (unsigned i = SVTNumElts, e = NumElts; i != e && CanFold; ++i)
        CanFold = SVOp->getMaskElt(i) < 0;

      if (CanFold) {
        SDValue BC00 = DAG.getNode(ISD::BITCAST, dl, VT, BC0.getOperand(0));
        SDValue BC01 = DAG.getNode(ISD::BITCAST, dl, VT, BC0.getOperand(1));
        SDValue NewBinOp = DAG.getNode(BC0.getOpcode(), dl, VT, BC00, BC01);
        return DAG.getVectorShuffle(VT, dl, NewBinOp, N1, &SVOp->getMask()[0]);
      }
    }
  }

  // Only handle 128 wide vector from here on.
  if (!VT.is128BitVector())
    return SDValue();

  // Combine a vector_shuffle that is equal to build_vector load1, load2, load3,
  // load4, <0, 1, 2, 3> into a 128-bit load if the load addresses are
  // consecutive, non-overlapping, and in the right order.
  SmallVector<SDValue, 16> Elts;
  for (unsigned i = 0, e = VT.getVectorNumElements(); i != e; ++i)
    Elts.push_back(getShuffleScalarElt(N, i, DAG, 0));

  SDValue LD = EltsFromConsecutiveLoads(VT, Elts, dl, DAG, true);
  if (LD.getNode())
    return LD;

  if (isTargetShuffle(N->getOpcode())) {
    SDValue Shuffle =
        PerformTargetShuffleCombine(SDValue(N, 0), DAG, DCI, Subtarget);
    if (Shuffle.getNode())
      return Shuffle;

    // Try recursively combining arbitrary sequences of x86 shuffle
    // instructions into higher-order shuffles. We do this after combining
    // specific PSHUF instruction sequences into their minimal form so that we
    // can evaluate how many specialized shuffle instructions are involved in
    // a particular chain.
    SmallVector<int, 1> NonceMask; // Just a placeholder.
    NonceMask.push_back(0);
    if (combineX86ShufflesRecursively(SDValue(N, 0), SDValue(N, 0), NonceMask,
                                      /*Depth*/ 1, /*HasPSHUFB*/ false, DAG,
                                      DCI, Subtarget))
      return SDValue(); // This routine will use CombineTo to replace N.
  }

  return SDValue();
}

/// PerformTruncateCombine - Converts truncate operation to
/// a sequence of vector shuffle operations.
/// It is possible when we truncate 256-bit vector to 128-bit vector
static SDValue PerformTruncateCombine(SDNode *N, SelectionDAG &DAG,
                                      TargetLowering::DAGCombinerInfo &DCI,
                                      const X86Subtarget *Subtarget)  {
  return SDValue();
}

/// XFormVExtractWithShuffleIntoLoad - Check if a vector extract from a target
/// specific shuffle of a load can be folded into a single element load.
/// Similar handling for VECTOR_SHUFFLE is performed by DAGCombiner, but
/// shuffles have been customed lowered so we need to handle those here.
static SDValue XFormVExtractWithShuffleIntoLoad(SDNode *N, SelectionDAG &DAG,
                                         TargetLowering::DAGCombinerInfo &DCI) {
  if (DCI.isBeforeLegalizeOps())
    return SDValue();

  SDValue InVec = N->getOperand(0);
  SDValue EltNo = N->getOperand(1);

  if (!isa<ConstantSDNode>(EltNo))
    return SDValue();

  EVT VT = InVec.getValueType();

  if (InVec.getOpcode() == ISD::BITCAST) {
    // Don't duplicate a load with other uses.
    if (!InVec.hasOneUse())
      return SDValue();
    EVT BCVT = InVec.getOperand(0).getValueType();
    if (BCVT.getVectorNumElements() != VT.getVectorNumElements())
      return SDValue();
    InVec = InVec.getOperand(0);
  }

  if (!isTargetShuffle(InVec.getOpcode()))
    return SDValue();

  // Don't duplicate a load with other uses.
  if (!InVec.hasOneUse())
    return SDValue();

  SmallVector<int, 16> ShuffleMask;
  bool UnaryShuffle;
  if (!getTargetShuffleMask(InVec.getNode(), VT.getSimpleVT(), ShuffleMask,
                            UnaryShuffle))
    return SDValue();

  // Select the input vector, guarding against out of range extract vector.
  unsigned NumElems = VT.getVectorNumElements();
  int Elt = cast<ConstantSDNode>(EltNo)->getZExtValue();
  int Idx = (Elt > (int)NumElems) ? -1 : ShuffleMask[Elt];
  SDValue LdNode = (Idx < (int)NumElems) ? InVec.getOperand(0)
                                         : InVec.getOperand(1);

  // If inputs to shuffle are the same for both ops, then allow 2 uses
  unsigned AllowedUses = InVec.getOperand(0) == InVec.getOperand(1) ? 2 : 1;

  if (LdNode.getOpcode() == ISD::BITCAST) {
    // Don't duplicate a load with other uses.
    if (!LdNode.getNode()->hasNUsesOfValue(AllowedUses, 0))
      return SDValue();

    AllowedUses = 1; // only allow 1 load use if we have a bitcast
    LdNode = LdNode.getOperand(0);
  }

  if (!ISD::isNormalLoad(LdNode.getNode()))
    return SDValue();

  LoadSDNode *LN0 = cast<LoadSDNode>(LdNode);

  if (!LN0 ||!LN0->hasNUsesOfValue(AllowedUses, 0) || LN0->isVolatile())
    return SDValue();

  EVT EltVT = N->getValueType(0);
  // If there's a bitcast before the shuffle, check if the load type and
  // alignment is valid.
  unsigned Align = LN0->getAlignment();
  const TargetLowering &TLI = DAG.getTargetLoweringInfo();
  unsigned NewAlign = TLI.getDataLayout()->getABITypeAlignment(
      EltVT.getTypeForEVT(*DAG.getContext()));

  if (NewAlign > Align || !TLI.isOperationLegalOrCustom(ISD::LOAD, EltVT))
    return SDValue();

  // All checks match so transform back to vector_shuffle so that DAG combiner
  // can finish the job
  SDLoc dl(N);

  // Create shuffle node taking into account the case that its a unary shuffle
  SDValue Shuffle = (UnaryShuffle) ? DAG.getUNDEF(VT) : InVec.getOperand(1);
  Shuffle = DAG.getVectorShuffle(InVec.getValueType(), dl,
                                 InVec.getOperand(0), Shuffle,
                                 &ShuffleMask[0]);
  Shuffle = DAG.getNode(ISD::BITCAST, dl, VT, Shuffle);
  return DAG.getNode(ISD::EXTRACT_VECTOR_ELT, dl, N->getValueType(0), Shuffle,
                     EltNo);
}

/// PerformEXTRACT_VECTOR_ELTCombine - Detect vector gather/scatter index
/// generation and convert it from being a bunch of shuffles and extracts
/// to a simple store and scalar loads to extract the elements.
static SDValue PerformEXTRACT_VECTOR_ELTCombine(SDNode *N, SelectionDAG &DAG,
                                         TargetLowering::DAGCombinerInfo &DCI) {
  SDValue NewOp = XFormVExtractWithShuffleIntoLoad(N, DAG, DCI);
  if (NewOp.getNode())
    return NewOp;

  SDValue InputVector = N->getOperand(0);

  // Detect whether we are trying to convert from mmx to i32 and the bitcast
  // from mmx to v2i32 has a single usage.
  if (InputVector.getNode()->getOpcode() == llvm::ISD::BITCAST &&
      InputVector.getNode()->getOperand(0).getValueType() == MVT::x86mmx &&
      InputVector.hasOneUse() && N->getValueType(0) == MVT::i32)
    return DAG.getNode(X86ISD::MMX_MOVD2W, SDLoc(InputVector),
                       N->getValueType(0),
                       InputVector.getNode()->getOperand(0));

  // Only operate on vectors of 4 elements, where the alternative shuffling
  // gets to be more expensive.
  if (InputVector.getValueType() != MVT::v4i32)
    return SDValue();

  // Check whether every use of InputVector is an EXTRACT_VECTOR_ELT with a
  // single use which is a sign-extend or zero-extend, and all elements are
  // used.
  SmallVector<SDNode *, 4> Uses;
  unsigned ExtractedElements = 0;
  for (SDNode::use_iterator UI = InputVector.getNode()->use_begin(),
       UE = InputVector.getNode()->use_end(); UI != UE; ++UI) {
    if (UI.getUse().getResNo() != InputVector.getResNo())
      return SDValue();

    SDNode *Extract = *UI;
    if (Extract->getOpcode() != ISD::EXTRACT_VECTOR_ELT)
      return SDValue();

    if (Extract->getValueType(0) != MVT::i32)
      return SDValue();
    if (!Extract->hasOneUse())
      return SDValue();
    if (Extract->use_begin()->getOpcode() != ISD::SIGN_EXTEND &&
        Extract->use_begin()->getOpcode() != ISD::ZERO_EXTEND)
      return SDValue();
    if (!isa<ConstantSDNode>(Extract->getOperand(1)))
      return SDValue();

    // Record which element was extracted.
    ExtractedElements |=
      1 << cast<ConstantSDNode>(Extract->getOperand(1))->getZExtValue();

    Uses.push_back(Extract);
  }

  // If not all the elements were used, this may not be worthwhile.
  if (ExtractedElements != 15)
    return SDValue();

  // Ok, we've now decided to do the transformation.
  SDLoc dl(InputVector);

  // Store the value to a temporary stack slot.
  SDValue StackPtr = DAG.CreateStackTemporary(InputVector.getValueType());
  SDValue Ch = DAG.getStore(DAG.getEntryNode(), dl, InputVector, StackPtr,
                            MachinePointerInfo(), false, false, 0);

  // Replace each use (extract) with a load of the appropriate element.
  for (SmallVectorImpl<SDNode *>::iterator UI = Uses.begin(),
       UE = Uses.end(); UI != UE; ++UI) {
    SDNode *Extract = *UI;

    // cOMpute the element's address.
    SDValue Idx = Extract->getOperand(1);
    unsigned EltSize =
        InputVector.getValueType().getVectorElementType().getSizeInBits()/8;
    uint64_t Offset = EltSize * cast<ConstantSDNode>(Idx)->getZExtValue();
    const TargetLowering &TLI = DAG.getTargetLoweringInfo();
    SDValue OffsetVal = DAG.getConstant(Offset, TLI.getPointerTy());

    SDValue ScalarAddr = DAG.getNode(ISD::ADD, dl, TLI.getPointerTy(),
                                     StackPtr, OffsetVal);

    // Load the scalar.
    SDValue LoadScalar = DAG.getLoad(Extract->getValueType(0), dl, Ch,
                                     ScalarAddr, MachinePointerInfo(),
                                     false, false, false, 0);

    // Replace the exact with the load.
    DAG.ReplaceAllUsesOfValueWith(SDValue(Extract, 0), LoadScalar);
  }

  // The replacement was made in place; don't return anything.
  return SDValue();
}

/// \brief Matches a VSELECT onto min/max or return 0 if the node doesn't match.
static std::pair<unsigned, bool>
matchIntegerMINMAX(SDValue Cond, EVT VT, SDValue LHS, SDValue RHS,
                   SelectionDAG &DAG, const X86Subtarget *Subtarget) {
  if (!VT.isVector())
    return std::make_pair(0, false);

  bool NeedSplit = false;
  switch (VT.getSimpleVT().SimpleTy) {
  default: return std::make_pair(0, false);
  case MVT::v32i8:
  case MVT::v16i16:
  case MVT::v8i32:
    if (!Subtarget->hasAVX2())
      NeedSplit = true;
    if (!Subtarget->hasAVX())
      return std::make_pair(0, false);
    break;
  case MVT::v16i8:
  case MVT::v8i16:
  case MVT::v4i32:
    if (!Subtarget->hasSSE2())
      return std::make_pair(0, false);
  }

  // SSE2 has only a small subset of the operations.
  bool hasUnsigned = Subtarget->hasSSE41() ||
                     (Subtarget->hasSSE2() && VT == MVT::v16i8);
  bool hasSigned = Subtarget->hasSSE41() ||
                   (Subtarget->hasSSE2() && VT == MVT::v8i16);

  ISD::CondCode CC = cast<CondCodeSDNode>(Cond.getOperand(2))->get();

  unsigned Opc = 0;
  // Check for x CC y ? x : y.
  if (DAG.isEqualTo(LHS, Cond.getOperand(0)) &&
      DAG.isEqualTo(RHS, Cond.getOperand(1))) {
    switch (CC) {
    default: break;
    case ISD::SETULT:
    case ISD::SETULE:
      Opc = hasUnsigned ? X86ISD::UMIN : 0; break;
    case ISD::SETUGT:
    case ISD::SETUGE:
      Opc = hasUnsigned ? X86ISD::UMAX : 0; break;
    case ISD::SETLT:
    case ISD::SETLE:
      Opc = hasSigned ? X86ISD::SMIN : 0; break;
    case ISD::SETGT:
    case ISD::SETGE:
      Opc = hasSigned ? X86ISD::SMAX : 0; break;
    }
  // Check for x CC y ? y : x -- a min/max with reversed arms.
  } else if (DAG.isEqualTo(LHS, Cond.getOperand(1)) &&
             DAG.isEqualTo(RHS, Cond.getOperand(0))) {
    switch (CC) {
    default: break;
    case ISD::SETULT:
    case ISD::SETULE:
      Opc = hasUnsigned ? X86ISD::UMAX : 0; break;
    case ISD::SETUGT:
    case ISD::SETUGE:
      Opc = hasUnsigned ? X86ISD::UMIN : 0; break;
    case ISD::SETLT:
    case ISD::SETLE:
      Opc = hasSigned ? X86ISD::SMAX : 0; break;
    case ISD::SETGT:
    case ISD::SETGE:
      Opc = hasSigned ? X86ISD::SMIN : 0; break;
    }
  }

  return std::make_pair(Opc, NeedSplit);
}

static SDValue
TransformVSELECTtoBlendVECTOR_SHUFFLE(SDNode *N, SelectionDAG &DAG,
                                      const X86Subtarget *Subtarget) {
  SDLoc dl(N);
  SDValue Cond = N->getOperand(0);
  SDValue LHS = N->getOperand(1);
  SDValue RHS = N->getOperand(2);

  if (Cond.getOpcode() == ISD::SIGN_EXTEND) {
    SDValue CondSrc = Cond->getOperand(0);
    if (CondSrc->getOpcode() == ISD::SIGN_EXTEND_INREG)
      Cond = CondSrc->getOperand(0);
  }

  MVT VT = N->getSimpleValueType(0);
  MVT EltVT = VT.getVectorElementType();
  unsigned NumElems = VT.getVectorNumElements();
  // There is no blend with immediate in AVX-512.
  if (VT.is512BitVector())
    return SDValue();

  if (!Subtarget->hasSSE41() || EltVT == MVT::i8)
    return SDValue();
  if (!Subtarget->hasInt256() && VT == MVT::v16i16)
    return SDValue();

  if (!ISD::isBuildVectorOfConstantSDNodes(Cond.getNode()))
    return SDValue();

  // A vselect where all conditions and data are constants can be optimized into
  // a single vector load by SelectionDAGLegalize::ExpandBUILD_VECTOR().
  if (ISD::isBuildVectorOfConstantSDNodes(LHS.getNode()) &&
      ISD::isBuildVectorOfConstantSDNodes(RHS.getNode()))
    return SDValue();

  unsigned MaskValue = 0;
  if (!BUILD_VECTORtoBlendMask(cast<BuildVectorSDNode>(Cond), MaskValue))
    return SDValue();

  SmallVector<int, 8> ShuffleMask(NumElems, -1);
  for (unsigned i = 0; i < NumElems; ++i) {
    // Be sure we emit undef where we can.
    if (Cond.getOperand(i)->getOpcode() == ISD::UNDEF)
      ShuffleMask[i] = -1;
    else
      ShuffleMask[i] = i + NumElems * ((MaskValue >> i) & 1);
  }

  return DAG.getVectorShuffle(VT, dl, LHS, RHS, &ShuffleMask[0]);
}

/// PerformSELECTCombine - Do target-specific dag combines on SELECT and VSELECT
/// nodes.
static SDValue PerformSELECTCombine(SDNode *N, SelectionDAG &DAG,
                                    TargetLowering::DAGCombinerInfo &DCI,
                                    const X86Subtarget *Subtarget) {
  SDLoc DL(N);
  SDValue Cond = N->getOperand(0);
  // Get the LHS/RHS of the select.
  SDValue LHS = N->getOperand(1);
  SDValue RHS = N->getOperand(2);
  EVT VT = LHS.getValueType();
  const TargetLowering &TLI = DAG.getTargetLoweringInfo();

  // If we have SSE[12] support, try to form min/max nodes. SSE min/max
  // instructions match the semantics of the common C idiom x<y?x:y but not
  // x<=y?x:y, because of how they handle negative zero (which can be
  // ignored in unsafe-math mode).
  if (Cond.getOpcode() == ISD::SETCC && VT.isFloatingPoint() &&
      VT != MVT::f80 && TLI.isTypeLegal(VT) &&
      (Subtarget->hasSSE2() ||
       (Subtarget->hasSSE1() && VT.getScalarType() == MVT::f32))) {
    ISD::CondCode CC = cast<CondCodeSDNode>(Cond.getOperand(2))->get();

    unsigned Opcode = 0;
    // Check for x CC y ? x : y.
    if (DAG.isEqualTo(LHS, Cond.getOperand(0)) &&
        DAG.isEqualTo(RHS, Cond.getOperand(1))) {
      switch (CC) {
      default: break;
      case ISD::SETULT:
        // Converting this to a min would handle NaNs incorrectly, and swapping
        // the operands would cause it to handle comparisons between positive
        // and negative zero incorrectly.
        if (!DAG.isKnownNeverNaN(LHS) || !DAG.isKnownNeverNaN(RHS)) {
          if (!DAG.getTarget().Options.UnsafeFPMath &&
              !(DAG.isKnownNeverZero(LHS) || DAG.isKnownNeverZero(RHS)))
            break;
          std::swap(LHS, RHS);
        }
        Opcode = X86ISD::FMIN;
        break;
      case ISD::SETOLE:
        // Converting this to a min would handle comparisons between positive
        // and negative zero incorrectly.
        if (!DAG.getTarget().Options.UnsafeFPMath &&
            !DAG.isKnownNeverZero(LHS) && !DAG.isKnownNeverZero(RHS))
          break;
        Opcode = X86ISD::FMIN;
        break;
      case ISD::SETULE:
        // Converting this to a min would handle both negative zeros and NaNs
        // incorrectly, but we can swap the operands to fix both.
        std::swap(LHS, RHS);
      case ISD::SETOLT:
      case ISD::SETLT:
      case ISD::SETLE:
        Opcode = X86ISD::FMIN;
        break;

      case ISD::SETOGE:
        // Converting this to a max would handle comparisons between positive
        // and negative zero incorrectly.
        if (!DAG.getTarget().Options.UnsafeFPMath &&
            !DAG.isKnownNeverZero(LHS) && !DAG.isKnownNeverZero(RHS))
          break;
        Opcode = X86ISD::FMAX;
        break;
      case ISD::SETUGT:
        // Converting this to a max would handle NaNs incorrectly, and swapping
        // the operands would cause it to handle comparisons between positive
        // and negative zero incorrectly.
        if (!DAG.isKnownNeverNaN(LHS) || !DAG.isKnownNeverNaN(RHS)) {
          if (!DAG.getTarget().Options.UnsafeFPMath &&
              !(DAG.isKnownNeverZero(LHS) || DAG.isKnownNeverZero(RHS)))
            break;
          std::swap(LHS, RHS);
        }
        Opcode = X86ISD::FMAX;
        break;
      case ISD::SETUGE:
        // Converting this to a max would handle both negative zeros and NaNs
        // incorrectly, but we can swap the operands to fix both.
        std::swap(LHS, RHS);
      case ISD::SETOGT:
      case ISD::SETGT:
      case ISD::SETGE:
        Opcode = X86ISD::FMAX;
        break;
      }
    // Check for x CC y ? y : x -- a min/max with reversed arms.
    } else if (DAG.isEqualTo(LHS, Cond.getOperand(1)) &&
               DAG.isEqualTo(RHS, Cond.getOperand(0))) {
      switch (CC) {
      default: break;
      case ISD::SETOGE:
        // Converting this to a min would handle comparisons between positive
        // and negative zero incorrectly, and swapping the operands would
        // cause it to handle NaNs incorrectly.
        if (!DAG.getTarget().Options.UnsafeFPMath &&
            !(DAG.isKnownNeverZero(LHS) || DAG.isKnownNeverZero(RHS))) {
          if (!DAG.isKnownNeverNaN(LHS) || !DAG.isKnownNeverNaN(RHS))
            break;
          std::swap(LHS, RHS);
        }
        Opcode = X86ISD::FMIN;
        break;
      case ISD::SETUGT:
        // Converting this to a min would handle NaNs incorrectly.
        if (!DAG.getTarget().Options.UnsafeFPMath &&
            (!DAG.isKnownNeverNaN(LHS) || !DAG.isKnownNeverNaN(RHS)))
          break;
        Opcode = X86ISD::FMIN;
        break;
      case ISD::SETUGE:
        // Converting this to a min would handle both negative zeros and NaNs
        // incorrectly, but we can swap the operands to fix both.
        std::swap(LHS, RHS);
      case ISD::SETOGT:
      case ISD::SETGT:
      case ISD::SETGE:
        Opcode = X86ISD::FMIN;
        break;

      case ISD::SETULT:
        // Converting this to a max would handle NaNs incorrectly.
        if (!DAG.isKnownNeverNaN(LHS) || !DAG.isKnownNeverNaN(RHS))
          break;
        Opcode = X86ISD::FMAX;
        break;
      case ISD::SETOLE:
        // Converting this to a max would handle comparisons between positive
        // and negative zero incorrectly, and swapping the operands would
        // cause it to handle NaNs incorrectly.
        if (!DAG.getTarget().Options.UnsafeFPMath &&
            !DAG.isKnownNeverZero(LHS) && !DAG.isKnownNeverZero(RHS)) {
          if (!DAG.isKnownNeverNaN(LHS) || !DAG.isKnownNeverNaN(RHS))
            break;
          std::swap(LHS, RHS);
        }
        Opcode = X86ISD::FMAX;
        break;
      case ISD::SETULE:
        // Converting this to a max would handle both negative zeros and NaNs
        // incorrectly, but we can swap the operands to fix both.
        std::swap(LHS, RHS);
      case ISD::SETOLT:
      case ISD::SETLT:
      case ISD::SETLE:
        Opcode = X86ISD::FMAX;
        break;
      }
    }

    if (Opcode)
      return DAG.getNode(Opcode, DL, N->getValueType(0), LHS, RHS);
  }

  EVT CondVT = Cond.getValueType();
  if (Subtarget->hasAVX512() && VT.isVector() && CondVT.isVector() &&
      CondVT.getVectorElementType() == MVT::i1) {
    // v16i8 (select v16i1, v16i8, v16i8) does not have a proper
    // lowering on KNL. In this case we convert it to
    // v16i8 (select v16i8, v16i8, v16i8) and use AVX instruction.
    // The same situation for all 128 and 256-bit vectors of i8 and i16.
    // Since SKX these selects have a proper lowering.
    EVT OpVT = LHS.getValueType();
    if ((OpVT.is128BitVector() || OpVT.is256BitVector()) &&
        (OpVT.getVectorElementType() == MVT::i8 ||
         OpVT.getVectorElementType() == MVT::i16) &&
        !(Subtarget->hasBWI() && Subtarget->hasVLX())) {
      Cond = DAG.getNode(ISD::SIGN_EXTEND, DL, OpVT, Cond);
      DCI.AddToWorklist(Cond.getNode());
      return DAG.getNode(N->getOpcode(), DL, OpVT, Cond, LHS, RHS);
    }
  }
  // If this is a select between two integer constants, try to do some
  // optimizations.
  if (ConstantSDNode *TrueC = dyn_cast<ConstantSDNode>(LHS)) {
    if (ConstantSDNode *FalseC = dyn_cast<ConstantSDNode>(RHS))
      // Don't do this for crazy integer types.
      if (DAG.getTargetLoweringInfo().isTypeLegal(LHS.getValueType())) {
        // If this is efficiently invertible, canonicalize the LHSC/RHSC values
        // so that TrueC (the true value) is larger than FalseC.
        bool NeedsCondInvert = false;

        if (TrueC->getAPIntValue().ult(FalseC->getAPIntValue()) &&
            // Efficiently invertible.
            (Cond.getOpcode() == ISD::SETCC ||  // setcc -> invertible.
             (Cond.getOpcode() == ISD::XOR &&   // xor(X, C) -> invertible.
              isa<ConstantSDNode>(Cond.getOperand(1))))) {
          NeedsCondInvert = true;
          std::swap(TrueC, FalseC);
        }

        // Optimize C ? 8 : 0 -> zext(C) << 3.  Likewise for any pow2/0.
        if (FalseC->getAPIntValue() == 0 &&
            TrueC->getAPIntValue().isPowerOf2()) {
          if (NeedsCondInvert) // Invert the condition if needed.
            Cond = DAG.getNode(ISD::XOR, DL, Cond.getValueType(), Cond,
                               DAG.getConstant(1, Cond.getValueType()));

          // Zero extend the condition if needed.
          Cond = DAG.getNode(ISD::ZERO_EXTEND, DL, LHS.getValueType(), Cond);

          unsigned ShAmt = TrueC->getAPIntValue().logBase2();
          return DAG.getNode(ISD::SHL, DL, LHS.getValueType(), Cond,
                             DAG.getConstant(ShAmt, MVT::i8));
        }

        // Optimize Cond ? cst+1 : cst -> zext(setcc(C)+cst.
        if (FalseC->getAPIntValue()+1 == TrueC->getAPIntValue()) {
          if (NeedsCondInvert) // Invert the condition if needed.
            Cond = DAG.getNode(ISD::XOR, DL, Cond.getValueType(), Cond,
                               DAG.getConstant(1, Cond.getValueType()));

          // Zero extend the condition if needed.
          Cond = DAG.getNode(ISD::ZERO_EXTEND, DL,
                             FalseC->getValueType(0), Cond);
          return DAG.getNode(ISD::ADD, DL, Cond.getValueType(), Cond,
                             SDValue(FalseC, 0));
        }

        // Optimize cases that will turn into an LEA instruction.  This requires
        // an i32 or i64 and an efficient multiplier (1, 2, 3, 4, 5, 8, 9).
        if (N->getValueType(0) == MVT::i32 || N->getValueType(0) == MVT::i64) {
          uint64_t Diff = TrueC->getZExtValue()-FalseC->getZExtValue();
          if (N->getValueType(0) == MVT::i32) Diff = (unsigned)Diff;

          bool isFastMultiplier = false;
          if (Diff < 10) {
            switch ((unsigned char)Diff) {
              default: break;
              case 1:  // result = add base, cond
              case 2:  // result = lea base(    , cond*2)
              case 3:  // result = lea base(cond, cond*2)
              case 4:  // result = lea base(    , cond*4)
              case 5:  // result = lea base(cond, cond*4)
              case 8:  // result = lea base(    , cond*8)
              case 9:  // result = lea base(cond, cond*8)
                isFastMultiplier = true;
                break;
            }
          }

          if (isFastMultiplier) {
            APInt Diff = TrueC->getAPIntValue()-FalseC->getAPIntValue();
            if (NeedsCondInvert) // Invert the condition if needed.
              Cond = DAG.getNode(ISD::XOR, DL, Cond.getValueType(), Cond,
                                 DAG.getConstant(1, Cond.getValueType()));

            // Zero extend the condition if needed.
            Cond = DAG.getNode(ISD::ZERO_EXTEND, DL, FalseC->getValueType(0),
                               Cond);
            // Scale the condition by the difference.
            if (Diff != 1)
              Cond = DAG.getNode(ISD::MUL, DL, Cond.getValueType(), Cond,
                                 DAG.getConstant(Diff, Cond.getValueType()));

            // Add the base if non-zero.
            if (FalseC->getAPIntValue() != 0)
              Cond = DAG.getNode(ISD::ADD, DL, Cond.getValueType(), Cond,
                                 SDValue(FalseC, 0));
            return Cond;
          }
        }
      }
  }

  // Canonicalize max and min:
  // (x > y) ? x : y -> (x >= y) ? x : y
  // (x < y) ? x : y -> (x <= y) ? x : y
  // This allows use of COND_S / COND_NS (see TranslateX86CC) which eliminates
  // the need for an extra compare
  // against zero. e.g.
  // (x - y) > 0 : (x - y) ? 0 -> (x - y) >= 0 : (x - y) ? 0
  // subl   %esi, %edi
  // testl  %edi, %edi
  // movl   $0, %eax
  // cmovgl %edi, %eax
  // =>
  // xorl   %eax, %eax
  // subl   %esi, $edi
  // cmovsl %eax, %edi
  if (N->getOpcode() == ISD::SELECT && Cond.getOpcode() == ISD::SETCC &&
      DAG.isEqualTo(LHS, Cond.getOperand(0)) &&
      DAG.isEqualTo(RHS, Cond.getOperand(1))) {
    ISD::CondCode CC = cast<CondCodeSDNode>(Cond.getOperand(2))->get();
    switch (CC) {
    default: break;
    case ISD::SETLT:
    case ISD::SETGT: {
      ISD::CondCode NewCC = (CC == ISD::SETLT) ? ISD::SETLE : ISD::SETGE;
      Cond = DAG.getSetCC(SDLoc(Cond), Cond.getValueType(),
                          Cond.getOperand(0), Cond.getOperand(1), NewCC);
      return DAG.getNode(ISD::SELECT, DL, VT, Cond, LHS, RHS);
    }
    }
  }

  // Early exit check
  if (!TLI.isTypeLegal(VT))
    return SDValue();

  // Match VSELECTs into subs with unsigned saturation.
  if (N->getOpcode() == ISD::VSELECT && Cond.getOpcode() == ISD::SETCC &&
      // psubus is available in SSE2 and AVX2 for i8 and i16 vectors.
      ((Subtarget->hasSSE2() && (VT == MVT::v16i8 || VT == MVT::v8i16)) ||
       (Subtarget->hasAVX2() && (VT == MVT::v32i8 || VT == MVT::v16i16)))) {
    ISD::CondCode CC = cast<CondCodeSDNode>(Cond.getOperand(2))->get();

    // Check if one of the arms of the VSELECT is a zero vector. If it's on the
    // left side invert the predicate to simplify logic below.
    SDValue Other;
    if (ISD::isBuildVectorAllZeros(LHS.getNode())) {
      Other = RHS;
      CC = ISD::getSetCCInverse(CC, true);
    } else if (ISD::isBuildVectorAllZeros(RHS.getNode())) {
      Other = LHS;
    }

    if (Other.getNode() && Other->getNumOperands() == 2 &&
        DAG.isEqualTo(Other->getOperand(0), Cond.getOperand(0))) {
      SDValue OpLHS = Other->getOperand(0), OpRHS = Other->getOperand(1);
      SDValue CondRHS = Cond->getOperand(1);

      // Look for a general sub with unsigned saturation first.
      // x >= y ? x-y : 0 --> subus x, y
      // x >  y ? x-y : 0 --> subus x, y
      if ((CC == ISD::SETUGE || CC == ISD::SETUGT) &&
          Other->getOpcode() == ISD::SUB && DAG.isEqualTo(OpRHS, CondRHS))
        return DAG.getNode(X86ISD::SUBUS, DL, VT, OpLHS, OpRHS);

      if (auto *OpRHSBV = dyn_cast<BuildVectorSDNode>(OpRHS))
        if (auto *OpRHSConst = OpRHSBV->getConstantSplatNode()) {
          if (auto *CondRHSBV = dyn_cast<BuildVectorSDNode>(CondRHS))
            if (auto *CondRHSConst = CondRHSBV->getConstantSplatNode())
              // If the RHS is a constant we have to reverse the const
              // canonicalization.
              // x > C-1 ? x+-C : 0 --> subus x, C
              if (CC == ISD::SETUGT && Other->getOpcode() == ISD::ADD &&
                  CondRHSConst->getAPIntValue() ==
                      (-OpRHSConst->getAPIntValue() - 1))
                return DAG.getNode(
                    X86ISD::SUBUS, DL, VT, OpLHS,
                    DAG.getConstant(-OpRHSConst->getAPIntValue(), VT));

          // Another special case: If C was a sign bit, the sub has been
          // canonicalized into a xor.
          // FIXME: Would it be better to use computeKnownBits to determine
          //        whether it's safe to decanonicalize the xor?
          // x s< 0 ? x^C : 0 --> subus x, C
          if (CC == ISD::SETLT && Other->getOpcode() == ISD::XOR &&
              ISD::isBuildVectorAllZeros(CondRHS.getNode()) &&
              OpRHSConst->getAPIntValue().isSignBit())
            // Note that we have to rebuild the RHS constant here to ensure we
            // don't rely on particular values of undef lanes.
            return DAG.getNode(
                X86ISD::SUBUS, DL, VT, OpLHS,
                DAG.getConstant(OpRHSConst->getAPIntValue(), VT));
        }
    }
  }

  // Try to match a min/max vector operation.
  if (N->getOpcode() == ISD::VSELECT && Cond.getOpcode() == ISD::SETCC) {
    std::pair<unsigned, bool> ret = matchIntegerMINMAX(Cond, VT, LHS, RHS, DAG, Subtarget);
    unsigned Opc = ret.first;
    bool NeedSplit = ret.second;

    if (Opc && NeedSplit) {
      unsigned NumElems = VT.getVectorNumElements();
      // Extract the LHS vectors
      SDValue LHS1 = Extract128BitVector(LHS, 0, DAG, DL);
      SDValue LHS2 = Extract128BitVector(LHS, NumElems/2, DAG, DL);

      // Extract the RHS vectors
      SDValue RHS1 = Extract128BitVector(RHS, 0, DAG, DL);
      SDValue RHS2 = Extract128BitVector(RHS, NumElems/2, DAG, DL);

      // Create min/max for each subvector
      LHS = DAG.getNode(Opc, DL, LHS1.getValueType(), LHS1, RHS1);
      RHS = DAG.getNode(Opc, DL, LHS2.getValueType(), LHS2, RHS2);

      // Merge the result
      return DAG.getNode(ISD::CONCAT_VECTORS, DL, VT, LHS, RHS);
    } else if (Opc)
      return DAG.getNode(Opc, DL, VT, LHS, RHS);
  }

  // Simplify vector selection if the selector will be produced by CMPP*/PCMP*.
  if (N->getOpcode() == ISD::VSELECT && Cond.getOpcode() == ISD::SETCC &&
      // Check if SETCC has already been promoted
      TLI.getSetCCResultType(*DAG.getContext(), VT) == CondVT &&
      // Check that condition value type matches vselect operand type
      CondVT == VT) { 

    assert(Cond.getValueType().isVector() &&
           "vector select expects a vector selector!");

    bool TValIsAllOnes = ISD::isBuildVectorAllOnes(LHS.getNode());
    bool FValIsAllZeros = ISD::isBuildVectorAllZeros(RHS.getNode());

    if (!TValIsAllOnes && !FValIsAllZeros) {
      // Try invert the condition if true value is not all 1s and false value
      // is not all 0s.
      bool TValIsAllZeros = ISD::isBuildVectorAllZeros(LHS.getNode());
      bool FValIsAllOnes = ISD::isBuildVectorAllOnes(RHS.getNode());

      if (TValIsAllZeros || FValIsAllOnes) {
        SDValue CC = Cond.getOperand(2);
        ISD::CondCode NewCC =
          ISD::getSetCCInverse(cast<CondCodeSDNode>(CC)->get(),
                               Cond.getOperand(0).getValueType().isInteger());
        Cond = DAG.getSetCC(DL, CondVT, Cond.getOperand(0), Cond.getOperand(1), NewCC);
        std::swap(LHS, RHS);
        TValIsAllOnes = FValIsAllOnes;
        FValIsAllZeros = TValIsAllZeros;
      }
    }

    if (TValIsAllOnes || FValIsAllZeros) {
      SDValue Ret;

      if (TValIsAllOnes && FValIsAllZeros)
        Ret = Cond;
      else if (TValIsAllOnes)
        Ret = DAG.getNode(ISD::OR, DL, CondVT, Cond,
                          DAG.getNode(ISD::BITCAST, DL, CondVT, RHS));
      else if (FValIsAllZeros)
        Ret = DAG.getNode(ISD::AND, DL, CondVT, Cond,
                          DAG.getNode(ISD::BITCAST, DL, CondVT, LHS));

      return DAG.getNode(ISD::BITCAST, DL, VT, Ret);
    }
  }

  // Try to fold this VSELECT into a MOVSS/MOVSD
  if (N->getOpcode() == ISD::VSELECT &&
      Cond.getOpcode() == ISD::BUILD_VECTOR && !DCI.isBeforeLegalize()) {
    if (VT == MVT::v4i32 || VT == MVT::v4f32 ||
        (Subtarget->hasSSE2() && (VT == MVT::v2i64 || VT == MVT::v2f64))) {
      bool CanFold = false;
      unsigned NumElems = Cond.getNumOperands();
      SDValue A = LHS;
      SDValue B = RHS;
      
      if (isZero(Cond.getOperand(0))) {
        CanFold = true;

        // fold (vselect <0,-1,-1,-1>, A, B) -> (movss A, B)
        // fold (vselect <0,-1> -> (movsd A, B)
        for (unsigned i = 1, e = NumElems; i != e && CanFold; ++i)
          CanFold = isAllOnes(Cond.getOperand(i));
      } else if (isAllOnes(Cond.getOperand(0))) {
        CanFold = true;
        std::swap(A, B);

        // fold (vselect <-1,0,0,0>, A, B) -> (movss B, A)
        // fold (vselect <-1,0> -> (movsd B, A)
        for (unsigned i = 1, e = NumElems; i != e && CanFold; ++i)
          CanFold = isZero(Cond.getOperand(i));
      }

      if (CanFold) {
        if (VT == MVT::v4i32 || VT == MVT::v4f32)
          return getTargetShuffleNode(X86ISD::MOVSS, DL, VT, A, B, DAG);
        return getTargetShuffleNode(X86ISD::MOVSD, DL, VT, A, B, DAG);
      }

      if (Subtarget->hasSSE2() && (VT == MVT::v4i32 || VT == MVT::v4f32)) {
        // fold (v4i32: vselect <0,0,-1,-1>, A, B) ->
        //      (v4i32 (bitcast (movsd (v2i64 (bitcast A)),
        //                             (v2i64 (bitcast B)))))
        //
        // fold (v4f32: vselect <0,0,-1,-1>, A, B) ->
        //      (v4f32 (bitcast (movsd (v2f64 (bitcast A)),
        //                             (v2f64 (bitcast B)))))
        //
        // fold (v4i32: vselect <-1,-1,0,0>, A, B) ->
        //      (v4i32 (bitcast (movsd (v2i64 (bitcast B)),
        //                             (v2i64 (bitcast A)))))
        //
        // fold (v4f32: vselect <-1,-1,0,0>, A, B) ->
        //      (v4f32 (bitcast (movsd (v2f64 (bitcast B)),
        //                             (v2f64 (bitcast A)))))

        CanFold = (isZero(Cond.getOperand(0)) &&
                   isZero(Cond.getOperand(1)) &&
                   isAllOnes(Cond.getOperand(2)) &&
                   isAllOnes(Cond.getOperand(3)));

        if (!CanFold && isAllOnes(Cond.getOperand(0)) &&
            isAllOnes(Cond.getOperand(1)) &&
            isZero(Cond.getOperand(2)) &&
            isZero(Cond.getOperand(3))) {
          CanFold = true;
          std::swap(LHS, RHS);
        }

        if (CanFold) {
          EVT NVT = (VT == MVT::v4i32) ? MVT::v2i64 : MVT::v2f64;
          SDValue NewA = DAG.getNode(ISD::BITCAST, DL, NVT, LHS);
          SDValue NewB = DAG.getNode(ISD::BITCAST, DL, NVT, RHS);
          SDValue Select = getTargetShuffleNode(X86ISD::MOVSD, DL, NVT, NewA,
                                                NewB, DAG);
          return DAG.getNode(ISD::BITCAST, DL, VT, Select);
        }
      }
    }
  }

  // If we know that this node is legal then we know that it is going to be
  // matched by one of the SSE/AVX BLEND instructions. These instructions only
  // depend on the highest bit in each word. Try to use SimplifyDemandedBits
  // to simplify previous instructions.
  if (N->getOpcode() == ISD::VSELECT && DCI.isBeforeLegalizeOps() &&
      !DCI.isBeforeLegalize() &&
      // We explicitly check against v8i16 and v16i16 because, although
      // they're marked as Custom, they might only be legal when Cond is a
      // build_vector of constants. This will be taken care in a later
      // condition.
      (TLI.isOperationLegalOrCustom(ISD::VSELECT, VT) && VT != MVT::v16i16 &&
       VT != MVT::v8i16)) {
    unsigned BitWidth = Cond.getValueType().getScalarType().getSizeInBits();

    // Don't optimize vector selects that map to mask-registers.
    if (BitWidth == 1)
      return SDValue();

    // Check all uses of that condition operand to check whether it will be
    // consumed by non-BLEND instructions, which may depend on all bits are set
    // properly.
    for (SDNode::use_iterator I = Cond->use_begin(),
                              E = Cond->use_end(); I != E; ++I)
      if (I->getOpcode() != ISD::VSELECT)
        // TODO: Add other opcodes eventually lowered into BLEND.
        return SDValue();

    assert(BitWidth >= 8 && BitWidth <= 64 && "Invalid mask size");
    APInt DemandedMask = APInt::getHighBitsSet(BitWidth, 1);

    APInt KnownZero, KnownOne;
    TargetLowering::TargetLoweringOpt TLO(DAG, DCI.isBeforeLegalize(),
                                          DCI.isBeforeLegalizeOps());
    if (TLO.ShrinkDemandedConstant(Cond, DemandedMask) ||
        TLI.SimplifyDemandedBits(Cond, DemandedMask, KnownZero, KnownOne, TLO))
      DCI.CommitTargetLoweringOpt(TLO);
  }

  // We should generate an X86ISD::BLENDI from a vselect if its argument
  // is a sign_extend_inreg of an any_extend of a BUILD_VECTOR of
  // constants. This specific pattern gets generated when we split a
  // selector for a 512 bit vector in a machine without AVX512 (but with
  // 256-bit vectors), during legalization:
  //
  // (vselect (sign_extend (any_extend (BUILD_VECTOR)) i1) LHS RHS)
  //
  // Iff we find this pattern and the build_vectors are built from
  // constants, we translate the vselect into a shuffle_vector that we
  // know will be matched by LowerVECTOR_SHUFFLEtoBlend.
  if (N->getOpcode() == ISD::VSELECT && !DCI.isBeforeLegalize()) {
    SDValue Shuffle = TransformVSELECTtoBlendVECTOR_SHUFFLE(N, DAG, Subtarget);
    if (Shuffle.getNode())
      return Shuffle;
  }

  return SDValue();
}

// Check whether a boolean test is testing a boolean value generated by
// X86ISD::SETCC. If so, return the operand of that SETCC and proper condition
// code.
//
// Simplify the following patterns:
// (Op (CMP (SETCC Cond EFLAGS) 1) EQ) or
// (Op (CMP (SETCC Cond EFLAGS) 0) NEQ)
// to (Op EFLAGS Cond)
//
// (Op (CMP (SETCC Cond EFLAGS) 0) EQ) or
// (Op (CMP (SETCC Cond EFLAGS) 1) NEQ)
// to (Op EFLAGS !Cond)
//
// where Op could be BRCOND or CMOV.
//
static SDValue checkBoolTestSetCCCombine(SDValue Cmp, X86::CondCode &CC) {
  // Quit if not CMP and SUB with its value result used.
  if (Cmp.getOpcode() != X86ISD::CMP &&
      (Cmp.getOpcode() != X86ISD::SUB || Cmp.getNode()->hasAnyUseOfValue(0)))
      return SDValue();

  // Quit if not used as a boolean value.
  if (CC != X86::COND_E && CC != X86::COND_NE)
    return SDValue();

  // Check CMP operands. One of them should be 0 or 1 and the other should be
  // an SetCC or extended from it.
  SDValue Op1 = Cmp.getOperand(0);
  SDValue Op2 = Cmp.getOperand(1);

  SDValue SetCC;
  const ConstantSDNode* C = nullptr;
  bool needOppositeCond = (CC == X86::COND_E);
  bool checkAgainstTrue = false; // Is it a comparison against 1?

  if ((C = dyn_cast<ConstantSDNode>(Op1)))
    SetCC = Op2;
  else if ((C = dyn_cast<ConstantSDNode>(Op2)))
    SetCC = Op1;
  else // Quit if all operands are not constants.
    return SDValue();

  if (C->getZExtValue() == 1) {
    needOppositeCond = !needOppositeCond;
    checkAgainstTrue = true;
  } else if (C->getZExtValue() != 0)
    // Quit if the constant is neither 0 or 1.
    return SDValue();

  bool truncatedToBoolWithAnd = false;
  // Skip (zext $x), (trunc $x), or (and $x, 1) node.
  while (SetCC.getOpcode() == ISD::ZERO_EXTEND ||
         SetCC.getOpcode() == ISD::TRUNCATE ||
         SetCC.getOpcode() == ISD::AND) {
    if (SetCC.getOpcode() == ISD::AND) {
      int OpIdx = -1;
      ConstantSDNode *CS;
      if ((CS = dyn_cast<ConstantSDNode>(SetCC.getOperand(0))) &&
          CS->getZExtValue() == 1)
        OpIdx = 1;
      if ((CS = dyn_cast<ConstantSDNode>(SetCC.getOperand(1))) &&
          CS->getZExtValue() == 1)
        OpIdx = 0;
      if (OpIdx == -1)
        break;
      SetCC = SetCC.getOperand(OpIdx);
      truncatedToBoolWithAnd = true;
    } else
      SetCC = SetCC.getOperand(0);
  }

  switch (SetCC.getOpcode()) {
  case X86ISD::SETCC_CARRY:
    // Since SETCC_CARRY gives output based on R = CF ? ~0 : 0, it's unsafe to
    // simplify it if the result of SETCC_CARRY is not canonicalized to 0 or 1,
    // i.e. it's a comparison against true but the result of SETCC_CARRY is not
    // truncated to i1 using 'and'.
    if (checkAgainstTrue && !truncatedToBoolWithAnd)
      break;
    assert(X86::CondCode(SetCC.getConstantOperandVal(0)) == X86::COND_B &&
           "Invalid use of SETCC_CARRY!");
    // FALL THROUGH
  case X86ISD::SETCC:
    // Set the condition code or opposite one if necessary.
    CC = X86::CondCode(SetCC.getConstantOperandVal(0));
    if (needOppositeCond)
      CC = X86::GetOppositeBranchCondition(CC);
    return SetCC.getOperand(1);
  case X86ISD::CMOV: {
    // Check whether false/true value has canonical one, i.e. 0 or 1.
    ConstantSDNode *FVal = dyn_cast<ConstantSDNode>(SetCC.getOperand(0));
    ConstantSDNode *TVal = dyn_cast<ConstantSDNode>(SetCC.getOperand(1));
    // Quit if true value is not a constant.
    if (!TVal)
      return SDValue();
    // Quit if false value is not a constant.
    if (!FVal) {
      SDValue Op = SetCC.getOperand(0);
      // Skip 'zext' or 'trunc' node.
      if (Op.getOpcode() == ISD::ZERO_EXTEND ||
          Op.getOpcode() == ISD::TRUNCATE)
        Op = Op.getOperand(0);
      // A special case for rdrand/rdseed, where 0 is set if false cond is
      // found.
      if ((Op.getOpcode() != X86ISD::RDRAND &&
           Op.getOpcode() != X86ISD::RDSEED) || Op.getResNo() != 0)
        return SDValue();
    }
    // Quit if false value is not the constant 0 or 1.
    bool FValIsFalse = true;
    if (FVal && FVal->getZExtValue() != 0) {
      if (FVal->getZExtValue() != 1)
        return SDValue();
      // If FVal is 1, opposite cond is needed.
      needOppositeCond = !needOppositeCond;
      FValIsFalse = false;
    }
    // Quit if TVal is not the constant opposite of FVal.
    if (FValIsFalse && TVal->getZExtValue() != 1)
      return SDValue();
    if (!FValIsFalse && TVal->getZExtValue() != 0)
      return SDValue();
    CC = X86::CondCode(SetCC.getConstantOperandVal(2));
    if (needOppositeCond)
      CC = X86::GetOppositeBranchCondition(CC);
    return SetCC.getOperand(3);
  }
  }

  return SDValue();
}

/// Optimize X86ISD::CMOV [LHS, RHS, CONDCODE (e.g. X86::COND_NE), CONDVAL]
static SDValue PerformCMOVCombine(SDNode *N, SelectionDAG &DAG,
                                  TargetLowering::DAGCombinerInfo &DCI,
                                  const X86Subtarget *Subtarget) {
  SDLoc DL(N);

  // If the flag operand isn't dead, don't touch this CMOV.
  if (N->getNumValues() == 2 && !SDValue(N, 1).use_empty())
    return SDValue();

  SDValue FalseOp = N->getOperand(0);
  SDValue TrueOp = N->getOperand(1);
  X86::CondCode CC = (X86::CondCode)N->getConstantOperandVal(2);
  SDValue Cond = N->getOperand(3);

  if (CC == X86::COND_E || CC == X86::COND_NE) {
    switch (Cond.getOpcode()) {
    default: break;
    case X86ISD::BSR:
    case X86ISD::BSF:
      // If operand of BSR / BSF are proven never zero, then ZF cannot be set.
      if (DAG.isKnownNeverZero(Cond.getOperand(0)))
        return (CC == X86::COND_E) ? FalseOp : TrueOp;
    }
  }

  SDValue Flags;

  Flags = checkBoolTestSetCCCombine(Cond, CC);
  if (Flags.getNode() &&
      // Extra check as FCMOV only supports a subset of X86 cond.
      (FalseOp.getValueType() != MVT::f80 || hasFPCMov(CC))) {
    SDValue Ops[] = { FalseOp, TrueOp,
                      DAG.getConstant(CC, MVT::i8), Flags };
    return DAG.getNode(X86ISD::CMOV, DL, N->getVTList(), Ops);
  }

  // If this is a select between two integer constants, try to do some
  // optimizations.  Note that the operands are ordered the opposite of SELECT
  // operands.
  if (ConstantSDNode *TrueC = dyn_cast<ConstantSDNode>(TrueOp)) {
    if (ConstantSDNode *FalseC = dyn_cast<ConstantSDNode>(FalseOp)) {
      // Canonicalize the TrueC/FalseC values so that TrueC (the true value) is
      // larger than FalseC (the false value).
      if (TrueC->getAPIntValue().ult(FalseC->getAPIntValue())) {
        CC = X86::GetOppositeBranchCondition(CC);
        std::swap(TrueC, FalseC);
        std::swap(TrueOp, FalseOp);
      }

      // Optimize C ? 8 : 0 -> zext(setcc(C)) << 3.  Likewise for any pow2/0.
      // This is efficient for any integer data type (including i8/i16) and
      // shift amount.
      if (FalseC->getAPIntValue() == 0 && TrueC->getAPIntValue().isPowerOf2()) {
        Cond = DAG.getNode(X86ISD::SETCC, DL, MVT::i8,
                           DAG.getConstant(CC, MVT::i8), Cond);

        // Zero extend the condition if needed.
        Cond = DAG.getNode(ISD::ZERO_EXTEND, DL, TrueC->getValueType(0), Cond);

        unsigned ShAmt = TrueC->getAPIntValue().logBase2();
        Cond = DAG.getNode(ISD::SHL, DL, Cond.getValueType(), Cond,
                           DAG.getConstant(ShAmt, MVT::i8));
        if (N->getNumValues() == 2)  // Dead flag value?
          return DCI.CombineTo(N, Cond, SDValue());
        return Cond;
      }

      // Optimize Cond ? cst+1 : cst -> zext(setcc(C)+cst.  This is efficient
      // for any integer data type, including i8/i16.
      if (FalseC->getAPIntValue()+1 == TrueC->getAPIntValue()) {
        Cond = DAG.getNode(X86ISD::SETCC, DL, MVT::i8,
                           DAG.getConstant(CC, MVT::i8), Cond);

        // Zero extend the condition if needed.
        Cond = DAG.getNode(ISD::ZERO_EXTEND, DL,
                           FalseC->getValueType(0), Cond);
        Cond = DAG.getNode(ISD::ADD, DL, Cond.getValueType(), Cond,
                           SDValue(FalseC, 0));

        if (N->getNumValues() == 2)  // Dead flag value?
          return DCI.CombineTo(N, Cond, SDValue());
        return Cond;
      }

      // Optimize cases that will turn into an LEA instruction.  This requires
      // an i32 or i64 and an efficient multiplier (1, 2, 3, 4, 5, 8, 9).
      if (N->getValueType(0) == MVT::i32 || N->getValueType(0) == MVT::i64) {
        uint64_t Diff = TrueC->getZExtValue()-FalseC->getZExtValue();
        if (N->getValueType(0) == MVT::i32) Diff = (unsigned)Diff;

        bool isFastMultiplier = false;
        if (Diff < 10) {
          switch ((unsigned char)Diff) {
          default: break;
          case 1:  // result = add base, cond
          case 2:  // result = lea base(    , cond*2)
          case 3:  // result = lea base(cond, cond*2)
          case 4:  // result = lea base(    , cond*4)
          case 5:  // result = lea base(cond, cond*4)
          case 8:  // result = lea base(    , cond*8)
          case 9:  // result = lea base(cond, cond*8)
            isFastMultiplier = true;
            break;
          }
        }

        if (isFastMultiplier) {
          APInt Diff = TrueC->getAPIntValue()-FalseC->getAPIntValue();
          Cond = DAG.getNode(X86ISD::SETCC, DL, MVT::i8,
                             DAG.getConstant(CC, MVT::i8), Cond);
          // Zero extend the condition if needed.
          Cond = DAG.getNode(ISD::ZERO_EXTEND, DL, FalseC->getValueType(0),
                             Cond);
          // Scale the condition by the difference.
          if (Diff != 1)
            Cond = DAG.getNode(ISD::MUL, DL, Cond.getValueType(), Cond,
                               DAG.getConstant(Diff, Cond.getValueType()));

          // Add the base if non-zero.
          if (FalseC->getAPIntValue() != 0)
            Cond = DAG.getNode(ISD::ADD, DL, Cond.getValueType(), Cond,
                               SDValue(FalseC, 0));
          if (N->getNumValues() == 2)  // Dead flag value?
            return DCI.CombineTo(N, Cond, SDValue());
          return Cond;
        }
      }
    }
  }

  // Handle these cases:
  //   (select (x != c), e, c) -> select (x != c), e, x),
  //   (select (x == c), c, e) -> select (x == c), x, e)
  // where the c is an integer constant, and the "select" is the combination
  // of CMOV and CMP.
  //
  // The rationale for this change is that the conditional-move from a constant
  // needs two instructions, however, conditional-move from a register needs
  // only one instruction.
  //
  // CAVEAT: By replacing a constant with a symbolic value, it may obscure
  //  some instruction-combining opportunities. This opt needs to be
  //  postponed as late as possible.
  //
  if (!DCI.isBeforeLegalize() && !DCI.isBeforeLegalizeOps()) {
    // the DCI.xxxx conditions are provided to postpone the optimization as
    // late as possible.

    ConstantSDNode *CmpAgainst = nullptr;
    if ((Cond.getOpcode() == X86ISD::CMP || Cond.getOpcode() == X86ISD::SUB) &&
        (CmpAgainst = dyn_cast<ConstantSDNode>(Cond.getOperand(1))) &&
        !isa<ConstantSDNode>(Cond.getOperand(0))) {

      if (CC == X86::COND_NE &&
          CmpAgainst == dyn_cast<ConstantSDNode>(FalseOp)) {
        CC = X86::GetOppositeBranchCondition(CC);
        std::swap(TrueOp, FalseOp);
      }

      if (CC == X86::COND_E &&
          CmpAgainst == dyn_cast<ConstantSDNode>(TrueOp)) {
        SDValue Ops[] = { FalseOp, Cond.getOperand(0),
                          DAG.getConstant(CC, MVT::i8), Cond };
        return DAG.getNode(X86ISD::CMOV, DL, N->getVTList (), Ops);
      }
    }
  }

  return SDValue();
}

static SDValue PerformINTRINSIC_WO_CHAINCombine(SDNode *N, SelectionDAG &DAG,
                                                const X86Subtarget *Subtarget) {
  unsigned IntNo = cast<ConstantSDNode>(N->getOperand(0))->getZExtValue();
  switch (IntNo) {
  default: return SDValue();
  // SSE/AVX/AVX2 blend intrinsics.
  case Intrinsic::x86_avx2_pblendvb:
  case Intrinsic::x86_avx2_pblendw:
  case Intrinsic::x86_avx2_pblendd_128:
  case Intrinsic::x86_avx2_pblendd_256:
    // Don't try to simplify this intrinsic if we don't have AVX2.
    if (!Subtarget->hasAVX2())
      return SDValue();
    // FALL-THROUGH
  case Intrinsic::x86_avx_blend_pd_256:
  case Intrinsic::x86_avx_blend_ps_256:
  case Intrinsic::x86_avx_blendv_pd_256:
  case Intrinsic::x86_avx_blendv_ps_256:
    // Don't try to simplify this intrinsic if we don't have AVX.
    if (!Subtarget->hasAVX())
      return SDValue();
    // FALL-THROUGH
  case Intrinsic::x86_sse41_pblendw:
  case Intrinsic::x86_sse41_blendpd:
  case Intrinsic::x86_sse41_blendps:
  case Intrinsic::x86_sse41_blendvps:
  case Intrinsic::x86_sse41_blendvpd:
  case Intrinsic::x86_sse41_pblendvb: {
    SDValue Op0 = N->getOperand(1);
    SDValue Op1 = N->getOperand(2);
    SDValue Mask = N->getOperand(3);

    // Don't try to simplify this intrinsic if we don't have SSE4.1.
    if (!Subtarget->hasSSE41())
      return SDValue();

    // fold (blend A, A, Mask) -> A
    if (Op0 == Op1)
      return Op0;
    // fold (blend A, B, allZeros) -> A
    if (ISD::isBuildVectorAllZeros(Mask.getNode()))
      return Op0;
    // fold (blend A, B, allOnes) -> B
    if (ISD::isBuildVectorAllOnes(Mask.getNode()))
      return Op1;
    
    // Simplify the case where the mask is a constant i32 value.
    if (ConstantSDNode *C = dyn_cast<ConstantSDNode>(Mask)) {
      if (C->isNullValue())
        return Op0;
      if (C->isAllOnesValue())
        return Op1;
    }

    return SDValue();
  }

  // Packed SSE2/AVX2 arithmetic shift immediate intrinsics.
  case Intrinsic::x86_sse2_psrai_w:
  case Intrinsic::x86_sse2_psrai_d:
  case Intrinsic::x86_avx2_psrai_w:
  case Intrinsic::x86_avx2_psrai_d:
  case Intrinsic::x86_sse2_psra_w:
  case Intrinsic::x86_sse2_psra_d:
  case Intrinsic::x86_avx2_psra_w:
  case Intrinsic::x86_avx2_psra_d: {
    SDValue Op0 = N->getOperand(1);
    SDValue Op1 = N->getOperand(2);
    EVT VT = Op0.getValueType();
    assert(VT.isVector() && "Expected a vector type!");

    if (isa<BuildVectorSDNode>(Op1))
      Op1 = Op1.getOperand(0);

    if (!isa<ConstantSDNode>(Op1))
      return SDValue();

    EVT SVT = VT.getVectorElementType();
    unsigned SVTBits = SVT.getSizeInBits();

    ConstantSDNode *CND = cast<ConstantSDNode>(Op1);
    const APInt &C = APInt(SVTBits, CND->getAPIntValue().getZExtValue());
    uint64_t ShAmt = C.getZExtValue();

    // Don't try to convert this shift into a ISD::SRA if the shift
    // count is bigger than or equal to the element size.
    if (ShAmt >= SVTBits)
      return SDValue();

    // Trivial case: if the shift count is zero, then fold this
    // into the first operand.
    if (ShAmt == 0)
      return Op0;

    // Replace this packed shift intrinsic with a target independent
    // shift dag node.
    SDValue Splat = DAG.getConstant(C, VT);
    return DAG.getNode(ISD::SRA, SDLoc(N), VT, Op0, Splat);
  }
  }
}

/// PerformMulCombine - Optimize a single multiply with constant into two
/// in order to implement it with two cheaper instructions, e.g.
/// LEA + SHL, LEA + LEA.
static SDValue PerformMulCombine(SDNode *N, SelectionDAG &DAG,
                                 TargetLowering::DAGCombinerInfo &DCI) {
  if (DCI.isBeforeLegalize() || DCI.isCalledByLegalizer())
    return SDValue();

  EVT VT = N->getValueType(0);
  if (VT != MVT::i64)
    return SDValue();

  ConstantSDNode *C = dyn_cast<ConstantSDNode>(N->getOperand(1));
  if (!C)
    return SDValue();
  uint64_t MulAmt = C->getZExtValue();
  if (isPowerOf2_64(MulAmt) || MulAmt == 3 || MulAmt == 5 || MulAmt == 9)
    return SDValue();

  uint64_t MulAmt1 = 0;
  uint64_t MulAmt2 = 0;
  if ((MulAmt % 9) == 0) {
    MulAmt1 = 9;
    MulAmt2 = MulAmt / 9;
  } else if ((MulAmt % 5) == 0) {
    MulAmt1 = 5;
    MulAmt2 = MulAmt / 5;
  } else if ((MulAmt % 3) == 0) {
    MulAmt1 = 3;
    MulAmt2 = MulAmt / 3;
  }
  if (MulAmt2 &&
      (isPowerOf2_64(MulAmt2) || MulAmt2 == 3 || MulAmt2 == 5 || MulAmt2 == 9)){
    SDLoc DL(N);

    if (isPowerOf2_64(MulAmt2) &&
        !(N->hasOneUse() && N->use_begin()->getOpcode() == ISD::ADD))
      // If second multiplifer is pow2, issue it first. We want the multiply by
      // 3, 5, or 9 to be folded into the addressing mode unless the lone use
      // is an add.
      std::swap(MulAmt1, MulAmt2);

    SDValue NewMul;
    if (isPowerOf2_64(MulAmt1))
      NewMul = DAG.getNode(ISD::SHL, DL, VT, N->getOperand(0),
                           DAG.getConstant(Log2_64(MulAmt1), MVT::i8));
    else
      NewMul = DAG.getNode(X86ISD::MUL_IMM, DL, VT, N->getOperand(0),
                           DAG.getConstant(MulAmt1, VT));

    if (isPowerOf2_64(MulAmt2))
      NewMul = DAG.getNode(ISD::SHL, DL, VT, NewMul,
                           DAG.getConstant(Log2_64(MulAmt2), MVT::i8));
    else
      NewMul = DAG.getNode(X86ISD::MUL_IMM, DL, VT, NewMul,
                           DAG.getConstant(MulAmt2, VT));

    // Do not add new nodes to DAG combiner worklist.
    DCI.CombineTo(N, NewMul, false);
  }
  return SDValue();
}

static SDValue PerformSHLCombine(SDNode *N, SelectionDAG &DAG) {
  SDValue N0 = N->getOperand(0);
  SDValue N1 = N->getOperand(1);
  ConstantSDNode *N1C = dyn_cast<ConstantSDNode>(N1);
  EVT VT = N0.getValueType();

  // fold (shl (and (setcc_c), c1), c2) -> (and setcc_c, (c1 << c2))
  // since the result of setcc_c is all zero's or all ones.
  if (VT.isInteger() && !VT.isVector() &&
      N1C && N0.getOpcode() == ISD::AND &&
      N0.getOperand(1).getOpcode() == ISD::Constant) {
    SDValue N00 = N0.getOperand(0);
    if (N00.getOpcode() == X86ISD::SETCC_CARRY ||
        ((N00.getOpcode() == ISD::ANY_EXTEND ||
          N00.getOpcode() == ISD::ZERO_EXTEND) &&
         N00.getOperand(0).getOpcode() == X86ISD::SETCC_CARRY)) {
      APInt Mask = cast<ConstantSDNode>(N0.getOperand(1))->getAPIntValue();
      APInt ShAmt = N1C->getAPIntValue();
      Mask = Mask.shl(ShAmt);
      if (Mask != 0)
        return DAG.getNode(ISD::AND, SDLoc(N), VT,
                           N00, DAG.getConstant(Mask, VT));
    }
  }

  // Hardware support for vector shifts is sparse which makes us scalarize the
  // vector operations in many cases. Also, on sandybridge ADD is faster than
  // shl.
  // (shl V, 1) -> add V,V
  if (auto *N1BV = dyn_cast<BuildVectorSDNode>(N1))
    if (auto *N1SplatC = N1BV->getConstantSplatNode()) {
      assert(N0.getValueType().isVector() && "Invalid vector shift type");
      // We shift all of the values by one. In many cases we do not have
      // hardware support for this operation. This is better expressed as an ADD
      // of two values.
      if (N1SplatC->getZExtValue() == 1)
        return DAG.getNode(ISD::ADD, SDLoc(N), VT, N0, N0);
    }

  return SDValue();
}

/// \brief Returns a vector of 0s if the node in input is a vector logical
/// shift by a constant amount which is known to be bigger than or equal
/// to the vector element size in bits.
static SDValue performShiftToAllZeros(SDNode *N, SelectionDAG &DAG,
                                      const X86Subtarget *Subtarget) {
  EVT VT = N->getValueType(0);

  if (VT != MVT::v2i64 && VT != MVT::v4i32 && VT != MVT::v8i16 &&
      (!Subtarget->hasInt256() ||
       (VT != MVT::v4i64 && VT != MVT::v8i32 && VT != MVT::v16i16)))
    return SDValue();

  SDValue Amt = N->getOperand(1);
  SDLoc DL(N);
  if (auto *AmtBV = dyn_cast<BuildVectorSDNode>(Amt))
    if (auto *AmtSplat = AmtBV->getConstantSplatNode()) {
      APInt ShiftAmt = AmtSplat->getAPIntValue();
      unsigned MaxAmount = VT.getVectorElementType().getSizeInBits();

      // SSE2/AVX2 logical shifts always return a vector of 0s
      // if the shift amount is bigger than or equal to
      // the element size. The constant shift amount will be
      // encoded as a 8-bit immediate.
      if (ShiftAmt.trunc(8).uge(MaxAmount))
        return getZeroVector(VT, Subtarget, DAG, DL);
    }

  return SDValue();
}

/// PerformShiftCombine - Combine shifts.
static SDValue PerformShiftCombine(SDNode* N, SelectionDAG &DAG,
                                   TargetLowering::DAGCombinerInfo &DCI,
                                   const X86Subtarget *Subtarget) {
  if (N->getOpcode() == ISD::SHL) {
    SDValue V = PerformSHLCombine(N, DAG);
    if (V.getNode()) return V;
  }

  if (N->getOpcode() != ISD::SRA) {
    // Try to fold this logical shift into a zero vector.
    SDValue V = performShiftToAllZeros(N, DAG, Subtarget);
    if (V.getNode()) return V;
  }

  return SDValue();
}

// CMPEQCombine - Recognize the distinctive  (AND (setcc ...) (setcc ..))
// where both setccs reference the same FP CMP, and rewrite for CMPEQSS
// and friends.  Likewise for OR -> CMPNEQSS.
static SDValue CMPEQCombine(SDNode *N, SelectionDAG &DAG,
                            TargetLowering::DAGCombinerInfo &DCI,
                            const X86Subtarget *Subtarget) {
  unsigned opcode;

  // SSE1 supports CMP{eq|ne}SS, and SSE2 added CMP{eq|ne}SD, but
  // we're requiring SSE2 for both.
  if (Subtarget->hasSSE2() && isAndOrOfSetCCs(SDValue(N, 0U), opcode)) {
    SDValue N0 = N->getOperand(0);
    SDValue N1 = N->getOperand(1);
    SDValue CMP0 = N0->getOperand(1);
    SDValue CMP1 = N1->getOperand(1);
    SDLoc DL(N);

    // The SETCCs should both refer to the same CMP.
    if (CMP0.getOpcode() != X86ISD::CMP || CMP0 != CMP1)
      return SDValue();

    SDValue CMP00 = CMP0->getOperand(0);
    SDValue CMP01 = CMP0->getOperand(1);
    EVT     VT    = CMP00.getValueType();

    if (VT == MVT::f32 || VT == MVT::f64) {
      bool ExpectingFlags = false;
      // Check for any users that want flags:
      for (SDNode::use_iterator UI = N->use_begin(), UE = N->use_end();
           !ExpectingFlags && UI != UE; ++UI)
        switch (UI->getOpcode()) {
        default:
        case ISD::BR_CC:
        case ISD::BRCOND:
        case ISD::SELECT:
          ExpectingFlags = true;
          break;
        case ISD::CopyToReg:
        case ISD::SIGN_EXTEND:
        case ISD::ZERO_EXTEND:
        case ISD::ANY_EXTEND:
          break;
        }

      if (!ExpectingFlags) {
        enum X86::CondCode cc0 = (enum X86::CondCode)N0.getConstantOperandVal(0);
        enum X86::CondCode cc1 = (enum X86::CondCode)N1.getConstantOperandVal(0);

        if (cc1 == X86::COND_E || cc1 == X86::COND_NE) {
          X86::CondCode tmp = cc0;
          cc0 = cc1;
          cc1 = tmp;
        }

        if ((cc0 == X86::COND_E  && cc1 == X86::COND_NP) ||
            (cc0 == X86::COND_NE && cc1 == X86::COND_P)) {
          // FIXME: need symbolic constants for these magic numbers.
          // See X86ATTInstPrinter.cpp:printSSECC().
          unsigned x86cc = (cc0 == X86::COND_E) ? 0 : 4;
          if (Subtarget->hasAVX512()) {
            SDValue FSetCC = DAG.getNode(X86ISD::FSETCC, DL, MVT::i1, CMP00,
                                         CMP01, DAG.getConstant(x86cc, MVT::i8));
            if (N->getValueType(0) != MVT::i1)
              return DAG.getNode(ISD::ZERO_EXTEND, DL, N->getValueType(0),
                                 FSetCC);
            return FSetCC;
          }
          SDValue OnesOrZeroesF = DAG.getNode(X86ISD::FSETCC, DL,
                                              CMP00.getValueType(), CMP00, CMP01,
                                              DAG.getConstant(x86cc, MVT::i8));

          bool is64BitFP = (CMP00.getValueType() == MVT::f64);
          MVT IntVT = is64BitFP ? MVT::i64 : MVT::i32;

          if (is64BitFP && !Subtarget->is64Bit()) {
            // On a 32-bit target, we cannot bitcast the 64-bit float to a
            // 64-bit integer, since that's not a legal type. Since
            // OnesOrZeroesF is all ones of all zeroes, we don't need all the
            // bits, but can do this little dance to extract the lowest 32 bits
            // and work with those going forward.
            SDValue Vector64 = DAG.getNode(ISD::SCALAR_TO_VECTOR, DL, MVT::v2f64,
                                           OnesOrZeroesF);
            SDValue Vector32 = DAG.getNode(ISD::BITCAST, DL, MVT::v4f32,
                                           Vector64);
            OnesOrZeroesF = DAG.getNode(ISD::EXTRACT_VECTOR_ELT, DL, MVT::f32,
                                        Vector32, DAG.getIntPtrConstant(0));
            IntVT = MVT::i32;
          }

          SDValue OnesOrZeroesI = DAG.getNode(ISD::BITCAST, DL, IntVT, OnesOrZeroesF);
          SDValue ANDed = DAG.getNode(ISD::AND, DL, IntVT, OnesOrZeroesI,
                                      DAG.getConstant(1, IntVT));
          SDValue OneBitOfTruth = DAG.getNode(ISD::TRUNCATE, DL, MVT::i8, ANDed);
          return OneBitOfTruth;
        }
      }
    }
  }
  return SDValue();
}

/// CanFoldXORWithAllOnes - Test whether the XOR operand is a AllOnes vector
/// so it can be folded inside ANDNP.
static bool CanFoldXORWithAllOnes(const SDNode *N) {
  EVT VT = N->getValueType(0);

  // Match direct AllOnes for 128 and 256-bit vectors
  if (ISD::isBuildVectorAllOnes(N))
    return true;

  // Look through a bit convert.
  if (N->getOpcode() == ISD::BITCAST)
    N = N->getOperand(0).getNode();

  // Sometimes the operand may come from a insert_subvector building a 256-bit
  // allones vector
  if (VT.is256BitVector() &&
      N->getOpcode() == ISD::INSERT_SUBVECTOR) {
    SDValue V1 = N->getOperand(0);
    SDValue V2 = N->getOperand(1);

    if (V1.getOpcode() == ISD::INSERT_SUBVECTOR &&
        V1.getOperand(0).getOpcode() == ISD::UNDEF &&
        ISD::isBuildVectorAllOnes(V1.getOperand(1).getNode()) &&
        ISD::isBuildVectorAllOnes(V2.getNode()))
      return true;
  }

  return false;
}

// On AVX/AVX2 the type v8i1 is legalized to v8i16, which is an XMM sized
// register. In most cases we actually compare or select YMM-sized registers
// and mixing the two types creates horrible code. This method optimizes
// some of the transition sequences.
static SDValue WidenMaskArithmetic(SDNode *N, SelectionDAG &DAG,
                                 TargetLowering::DAGCombinerInfo &DCI,
                                 const X86Subtarget *Subtarget) {
  EVT VT = N->getValueType(0);
  if (!VT.is256BitVector())
    return SDValue();

  assert((N->getOpcode() == ISD::ANY_EXTEND ||
          N->getOpcode() == ISD::ZERO_EXTEND ||
          N->getOpcode() == ISD::SIGN_EXTEND) && "Invalid Node");

  SDValue Narrow = N->getOperand(0);
  EVT NarrowVT = Narrow->getValueType(0);
  if (!NarrowVT.is128BitVector())
    return SDValue();

  if (Narrow->getOpcode() != ISD::XOR &&
      Narrow->getOpcode() != ISD::AND &&
      Narrow->getOpcode() != ISD::OR)
    return SDValue();

  SDValue N0  = Narrow->getOperand(0);
  SDValue N1  = Narrow->getOperand(1);
  SDLoc DL(Narrow);

  // The Left side has to be a trunc.
  if (N0.getOpcode() != ISD::TRUNCATE)
    return SDValue();

  // The type of the truncated inputs.
  EVT WideVT = N0->getOperand(0)->getValueType(0);
  if (WideVT != VT)
    return SDValue();

  // The right side has to be a 'trunc' or a constant vector.
  bool RHSTrunc = N1.getOpcode() == ISD::TRUNCATE;
  ConstantSDNode *RHSConstSplat = nullptr;
  if (auto *RHSBV = dyn_cast<BuildVectorSDNode>(N1))
    RHSConstSplat = RHSBV->getConstantSplatNode();
  if (!RHSTrunc && !RHSConstSplat)
    return SDValue();

  const TargetLowering &TLI = DAG.getTargetLoweringInfo();

  if (!TLI.isOperationLegalOrPromote(Narrow->getOpcode(), WideVT))
    return SDValue();

  // Set N0 and N1 to hold the inputs to the new wide operation.
  N0 = N0->getOperand(0);
  if (RHSConstSplat) {
    N1 = DAG.getNode(ISD::ZERO_EXTEND, DL, WideVT.getScalarType(),
                     SDValue(RHSConstSplat, 0));
    SmallVector<SDValue, 8> C(WideVT.getVectorNumElements(), N1);
    N1 = DAG.getNode(ISD::BUILD_VECTOR, DL, WideVT, C);
  } else if (RHSTrunc) {
    N1 = N1->getOperand(0);
  }

  // Generate the wide operation.
  SDValue Op = DAG.getNode(Narrow->getOpcode(), DL, WideVT, N0, N1);
  unsigned Opcode = N->getOpcode();
  switch (Opcode) {
  case ISD::ANY_EXTEND:
    return Op;
  case ISD::ZERO_EXTEND: {
    unsigned InBits = NarrowVT.getScalarType().getSizeInBits();
    APInt Mask = APInt::getAllOnesValue(InBits);
    Mask = Mask.zext(VT.getScalarType().getSizeInBits());
    return DAG.getNode(ISD::AND, DL, VT,
                       Op, DAG.getConstant(Mask, VT));
  }
  case ISD::SIGN_EXTEND:
    return DAG.getNode(ISD::SIGN_EXTEND_INREG, DL, VT,
                       Op, DAG.getValueType(NarrowVT));
  default:
    llvm_unreachable("Unexpected opcode");
  }
}

static SDValue PerformAndCombine(SDNode *N, SelectionDAG &DAG,
                                 TargetLowering::DAGCombinerInfo &DCI,
                                 const X86Subtarget *Subtarget) {
  EVT VT = N->getValueType(0);
  if (DCI.isBeforeLegalizeOps())
    return SDValue();

  SDValue R = CMPEQCombine(N, DAG, DCI, Subtarget);
  if (R.getNode())
    return R;

  // Create BEXTR instructions
  // BEXTR is ((X >> imm) & (2**size-1))
  if (VT == MVT::i32 || VT == MVT::i64) {
    SDValue N0 = N->getOperand(0);
    SDValue N1 = N->getOperand(1);
    SDLoc DL(N);

    // Check for BEXTR.
    if ((Subtarget->hasBMI() || Subtarget->hasTBM()) &&
        (N0.getOpcode() == ISD::SRA || N0.getOpcode() == ISD::SRL)) {
      ConstantSDNode *MaskNode = dyn_cast<ConstantSDNode>(N1);
      ConstantSDNode *ShiftNode = dyn_cast<ConstantSDNode>(N0.getOperand(1));
      if (MaskNode && ShiftNode) {
        uint64_t Mask = MaskNode->getZExtValue();
        uint64_t Shift = ShiftNode->getZExtValue();
        if (isMask_64(Mask)) {
          uint64_t MaskSize = CountPopulation_64(Mask);
          if (Shift + MaskSize <= VT.getSizeInBits())
            return DAG.getNode(X86ISD::BEXTR, DL, VT, N0.getOperand(0),
                               DAG.getConstant(Shift | (MaskSize << 8), VT));
        }
      }
    } // BEXTR

    return SDValue();
  }

  // Want to form ANDNP nodes:
  // 1) In the hopes of then easily combining them with OR and AND nodes
  //    to form PBLEND/PSIGN.
  // 2) To match ANDN packed intrinsics
  if (VT != MVT::v2i64 && VT != MVT::v4i64)
    return SDValue();

  SDValue N0 = N->getOperand(0);
  SDValue N1 = N->getOperand(1);
  SDLoc DL(N);

  // Check LHS for vnot
  if (N0.getOpcode() == ISD::XOR &&
      //ISD::isBuildVectorAllOnes(N0.getOperand(1).getNode()))
      CanFoldXORWithAllOnes(N0.getOperand(1).getNode()))
    return DAG.getNode(X86ISD::ANDNP, DL, VT, N0.getOperand(0), N1);

  // Check RHS for vnot
  if (N1.getOpcode() == ISD::XOR &&
      //ISD::isBuildVectorAllOnes(N1.getOperand(1).getNode()))
      CanFoldXORWithAllOnes(N1.getOperand(1).getNode()))
    return DAG.getNode(X86ISD::ANDNP, DL, VT, N1.getOperand(0), N0);

  return SDValue();
}

static SDValue PerformOrCombine(SDNode *N, SelectionDAG &DAG,
                                TargetLowering::DAGCombinerInfo &DCI,
                                const X86Subtarget *Subtarget) {
  if (DCI.isBeforeLegalizeOps())
    return SDValue();

  SDValue R = CMPEQCombine(N, DAG, DCI, Subtarget);
  if (R.getNode())
    return R;

  SDValue N0 = N->getOperand(0);
  SDValue N1 = N->getOperand(1);
  EVT VT = N->getValueType(0);

  // look for psign/blend
  if (VT == MVT::v2i64 || VT == MVT::v4i64) {
    if (!Subtarget->hasSSSE3() ||
        (VT == MVT::v4i64 && !Subtarget->hasInt256()))
      return SDValue();

    // Canonicalize pandn to RHS
    if (N0.getOpcode() == X86ISD::ANDNP)
      std::swap(N0, N1);
    // or (and (m, y), (pandn m, x))
    if (N0.getOpcode() == ISD::AND && N1.getOpcode() == X86ISD::ANDNP) {
      SDValue Mask = N1.getOperand(0);
      SDValue X    = N1.getOperand(1);
      SDValue Y;
      if (N0.getOperand(0) == Mask)
        Y = N0.getOperand(1);
      if (N0.getOperand(1) == Mask)
        Y = N0.getOperand(0);

      // Check to see if the mask appeared in both the AND and ANDNP and
      if (!Y.getNode())
        return SDValue();

      // Validate that X, Y, and Mask are BIT_CONVERTS, and see through them.
      // Look through mask bitcast.
      if (Mask.getOpcode() == ISD::BITCAST)
        Mask = Mask.getOperand(0);
      if (X.getOpcode() == ISD::BITCAST)
        X = X.getOperand(0);
      if (Y.getOpcode() == ISD::BITCAST)
        Y = Y.getOperand(0);

      EVT MaskVT = Mask.getValueType();

      // Validate that the Mask operand is a vector sra node.
      // FIXME: what to do for bytes, since there is a psignb/pblendvb, but
      // there is no psrai.b
      unsigned EltBits = MaskVT.getVectorElementType().getSizeInBits();
      unsigned SraAmt = ~0;
      if (Mask.getOpcode() == ISD::SRA) {
        if (auto *AmtBV = dyn_cast<BuildVectorSDNode>(Mask.getOperand(1)))
          if (auto *AmtConst = AmtBV->getConstantSplatNode())
            SraAmt = AmtConst->getZExtValue();
      } else if (Mask.getOpcode() == X86ISD::VSRAI) {
        SDValue SraC = Mask.getOperand(1);
        SraAmt  = cast<ConstantSDNode>(SraC)->getZExtValue();
      }
      if ((SraAmt + 1) != EltBits)
        return SDValue();

      SDLoc DL(N);

      // Now we know we at least have a plendvb with the mask val.  See if
      // we can form a psignb/w/d.
      // psign = x.type == y.type == mask.type && y = sub(0, x);
      if (Y.getOpcode() == ISD::SUB && Y.getOperand(1) == X &&
          ISD::isBuildVectorAllZeros(Y.getOperand(0).getNode()) &&
          X.getValueType() == MaskVT && Y.getValueType() == MaskVT) {
        assert((EltBits == 8 || EltBits == 16 || EltBits == 32) &&
               "Unsupported VT for PSIGN");
        Mask = DAG.getNode(X86ISD::PSIGN, DL, MaskVT, X, Mask.getOperand(0));
        return DAG.getNode(ISD::BITCAST, DL, VT, Mask);
      }
      // PBLENDVB only available on SSE 4.1
      if (!Subtarget->hasSSE41())
        return SDValue();

      EVT BlendVT = (VT == MVT::v4i64) ? MVT::v32i8 : MVT::v16i8;

      X = DAG.getNode(ISD::BITCAST, DL, BlendVT, X);
      Y = DAG.getNode(ISD::BITCAST, DL, BlendVT, Y);
      Mask = DAG.getNode(ISD::BITCAST, DL, BlendVT, Mask);
      Mask = DAG.getNode(ISD::VSELECT, DL, BlendVT, Mask, Y, X);
      return DAG.getNode(ISD::BITCAST, DL, VT, Mask);
    }
  }

  if (VT != MVT::i16 && VT != MVT::i32 && VT != MVT::i64)
    return SDValue();

  // fold (or (x << c) | (y >> (64 - c))) ==> (shld64 x, y, c)
  MachineFunction &MF = DAG.getMachineFunction();
  bool OptForSize = MF.getFunction()->getAttributes().
    hasAttribute(AttributeSet::FunctionIndex, Attribute::OptimizeForSize);

  // SHLD/SHRD instructions have lower register pressure, but on some
  // platforms they have higher latency than the equivalent
  // series of shifts/or that would otherwise be generated.
  // Don't fold (or (x << c) | (y >> (64 - c))) if SHLD/SHRD instructions
  // have higher latencies and we are not optimizing for size.
  if (!OptForSize && Subtarget->isSHLDSlow())
    return SDValue();

  if (N0.getOpcode() == ISD::SRL && N1.getOpcode() == ISD::SHL)
    std::swap(N0, N1);
  if (N0.getOpcode() != ISD::SHL || N1.getOpcode() != ISD::SRL)
    return SDValue();
  if (!N0.hasOneUse() || !N1.hasOneUse())
    return SDValue();

  SDValue ShAmt0 = N0.getOperand(1);
  if (ShAmt0.getValueType() != MVT::i8)
    return SDValue();
  SDValue ShAmt1 = N1.getOperand(1);
  if (ShAmt1.getValueType() != MVT::i8)
    return SDValue();
  if (ShAmt0.getOpcode() == ISD::TRUNCATE)
    ShAmt0 = ShAmt0.getOperand(0);
  if (ShAmt1.getOpcode() == ISD::TRUNCATE)
    ShAmt1 = ShAmt1.getOperand(0);

  SDLoc DL(N);
  unsigned Opc = X86ISD::SHLD;
  SDValue Op0 = N0.getOperand(0);
  SDValue Op1 = N1.getOperand(0);
  if (ShAmt0.getOpcode() == ISD::SUB) {
    Opc = X86ISD::SHRD;
    std::swap(Op0, Op1);
    std::swap(ShAmt0, ShAmt1);
  }

  unsigned Bits = VT.getSizeInBits();
  if (ShAmt1.getOpcode() == ISD::SUB) {
    SDValue Sum = ShAmt1.getOperand(0);
    if (ConstantSDNode *SumC = dyn_cast<ConstantSDNode>(Sum)) {
      SDValue ShAmt1Op1 = ShAmt1.getOperand(1);
      if (ShAmt1Op1.getNode()->getOpcode() == ISD::TRUNCATE)
        ShAmt1Op1 = ShAmt1Op1.getOperand(0);
      if (SumC->getSExtValue() == Bits && ShAmt1Op1 == ShAmt0)
        return DAG.getNode(Opc, DL, VT,
                           Op0, Op1,
                           DAG.getNode(ISD::TRUNCATE, DL,
                                       MVT::i8, ShAmt0));
    }
  } else if (ConstantSDNode *ShAmt1C = dyn_cast<ConstantSDNode>(ShAmt1)) {
    ConstantSDNode *ShAmt0C = dyn_cast<ConstantSDNode>(ShAmt0);
    if (ShAmt0C &&
        ShAmt0C->getSExtValue() + ShAmt1C->getSExtValue() == Bits)
      return DAG.getNode(Opc, DL, VT,
                         N0.getOperand(0), N1.getOperand(0),
                         DAG.getNode(ISD::TRUNCATE, DL,
                                       MVT::i8, ShAmt0));
  }

  return SDValue();
}

// Generate NEG and CMOV for integer abs.
static SDValue performIntegerAbsCombine(SDNode *N, SelectionDAG &DAG) {
  EVT VT = N->getValueType(0);

  // Since X86 does not have CMOV for 8-bit integer, we don't convert
  // 8-bit integer abs to NEG and CMOV.
  if (VT.isInteger() && VT.getSizeInBits() == 8)
    return SDValue();

  SDValue N0 = N->getOperand(0);
  SDValue N1 = N->getOperand(1);
  SDLoc DL(N);

  // Check pattern of XOR(ADD(X,Y), Y) where Y is SRA(X, size(X)-1)
  // and change it to SUB and CMOV.
  if (VT.isInteger() && N->getOpcode() == ISD::XOR &&
      N0.getOpcode() == ISD::ADD &&
      N0.getOperand(1) == N1 &&
      N1.getOpcode() == ISD::SRA &&
      N1.getOperand(0) == N0.getOperand(0))
    if (ConstantSDNode *Y1C = dyn_cast<ConstantSDNode>(N1.getOperand(1)))
      if (Y1C->getAPIntValue() == VT.getSizeInBits()-1) {
        // Generate SUB & CMOV.
        SDValue Neg = DAG.getNode(X86ISD::SUB, DL, DAG.getVTList(VT, MVT::i32),
                                  DAG.getConstant(0, VT), N0.getOperand(0));

        SDValue Ops[] = { N0.getOperand(0), Neg,
                          DAG.getConstant(X86::COND_GE, MVT::i8),
                          SDValue(Neg.getNode(), 1) };
        return DAG.getNode(X86ISD::CMOV, DL, DAG.getVTList(VT, MVT::Glue), Ops);
      }
  return SDValue();
}

// PerformXorCombine - Attempts to turn XOR nodes into BLSMSK nodes
static SDValue PerformXorCombine(SDNode *N, SelectionDAG &DAG,
                                 TargetLowering::DAGCombinerInfo &DCI,
                                 const X86Subtarget *Subtarget) {
  if (DCI.isBeforeLegalizeOps())
    return SDValue();

  if (Subtarget->hasCMov()) {
    SDValue RV = performIntegerAbsCombine(N, DAG);
    if (RV.getNode())
      return RV;
  }

  return SDValue();
}

/// PerformLOADCombine - Do target-specific dag combines on LOAD nodes.
static SDValue PerformLOADCombine(SDNode *N, SelectionDAG &DAG,
                                  TargetLowering::DAGCombinerInfo &DCI,
                                  const X86Subtarget *Subtarget) {
  LoadSDNode *Ld = cast<LoadSDNode>(N);
  EVT RegVT = Ld->getValueType(0);
  EVT MemVT = Ld->getMemoryVT();
  SDLoc dl(Ld);
  const TargetLowering &TLI = DAG.getTargetLoweringInfo();

  // On Sandybridge unaligned 256bit loads are inefficient.
  ISD::LoadExtType Ext = Ld->getExtensionType();
  unsigned Alignment = Ld->getAlignment();
  bool IsAligned = Alignment == 0 || Alignment >= MemVT.getSizeInBits()/8;
  if (RegVT.is256BitVector() && !Subtarget->hasInt256() &&
      !DCI.isBeforeLegalizeOps() && !IsAligned && Ext == ISD::NON_EXTLOAD) {
    unsigned NumElems = RegVT.getVectorNumElements();
    if (NumElems < 2)
      return SDValue();

    SDValue Ptr = Ld->getBasePtr();
    SDValue Increment = DAG.getConstant(16, TLI.getPointerTy());

    EVT HalfVT = EVT::getVectorVT(*DAG.getContext(), MemVT.getScalarType(),
                                  NumElems/2);
    SDValue Load1 = DAG.getLoad(HalfVT, dl, Ld->getChain(), Ptr,
                                Ld->getPointerInfo(), Ld->isVolatile(),
                                Ld->isNonTemporal(), Ld->isInvariant(),
                                Alignment);
    Ptr = DAG.getNode(ISD::ADD, dl, Ptr.getValueType(), Ptr, Increment);
    SDValue Load2 = DAG.getLoad(HalfVT, dl, Ld->getChain(), Ptr,
                                Ld->getPointerInfo(), Ld->isVolatile(),
                                Ld->isNonTemporal(), Ld->isInvariant(),
                                std::min(16U, Alignment));
    SDValue TF = DAG.getNode(ISD::TokenFactor, dl, MVT::Other,
                             Load1.getValue(1),
                             Load2.getValue(1));

    SDValue NewVec = DAG.getUNDEF(RegVT);
    NewVec = Insert128BitVector(NewVec, Load1, 0, DAG, dl);
    NewVec = Insert128BitVector(NewVec, Load2, NumElems/2, DAG, dl);
    return DCI.CombineTo(N, NewVec, TF, true);
  }

  return SDValue();
}

/// PerformSTORECombine - Do target-specific dag combines on STORE nodes.
static SDValue PerformSTORECombine(SDNode *N, SelectionDAG &DAG,
                                   const X86Subtarget *Subtarget) {
  StoreSDNode *St = cast<StoreSDNode>(N);
  EVT VT = St->getValue().getValueType();
  EVT StVT = St->getMemoryVT();
  SDLoc dl(St);
  SDValue StoredVal = St->getOperand(1);
  const TargetLowering &TLI = DAG.getTargetLoweringInfo();

  // If we are saving a concatenation of two XMM registers, perform two stores.
  // On Sandy Bridge, 256-bit memory operations are executed by two
  // 128-bit ports. However, on Haswell it is better to issue a single 256-bit
  // memory  operation.
  unsigned Alignment = St->getAlignment();
  bool IsAligned = Alignment == 0 || Alignment >= VT.getSizeInBits()/8;
  if (VT.is256BitVector() && !Subtarget->hasInt256() &&
      StVT == VT && !IsAligned) {
    unsigned NumElems = VT.getVectorNumElements();
    if (NumElems < 2)
      return SDValue();

    SDValue Value0 = Extract128BitVector(StoredVal, 0, DAG, dl);
    SDValue Value1 = Extract128BitVector(StoredVal, NumElems/2, DAG, dl);

    SDValue Stride = DAG.getConstant(16, TLI.getPointerTy());
    SDValue Ptr0 = St->getBasePtr();
    SDValue Ptr1 = DAG.getNode(ISD::ADD, dl, Ptr0.getValueType(), Ptr0, Stride);

    SDValue Ch0 = DAG.getStore(St->getChain(), dl, Value0, Ptr0,
                                St->getPointerInfo(), St->isVolatile(),
                                St->isNonTemporal(), Alignment);
    SDValue Ch1 = DAG.getStore(St->getChain(), dl, Value1, Ptr1,
                                St->getPointerInfo(), St->isVolatile(),
                                St->isNonTemporal(),
                                std::min(16U, Alignment));
    return DAG.getNode(ISD::TokenFactor, dl, MVT::Other, Ch0, Ch1);
  }

  // Optimize trunc store (of multiple scalars) to shuffle and store.
  // First, pack all of the elements in one place. Next, store to memory
  // in fewer chunks.
  if (St->isTruncatingStore() && VT.isVector()) {
    const TargetLowering &TLI = DAG.getTargetLoweringInfo();
    unsigned NumElems = VT.getVectorNumElements();
    assert(StVT != VT && "Cannot truncate to the same type");
    unsigned FromSz = VT.getVectorElementType().getSizeInBits();
    unsigned ToSz = StVT.getVectorElementType().getSizeInBits();

    // From, To sizes and ElemCount must be pow of two
    if (!isPowerOf2_32(NumElems * FromSz * ToSz)) return SDValue();
    // We are going to use the original vector elt for storing.
    // Accumulated smaller vector elements must be a multiple of the store size.
    if (0 != (NumElems * FromSz) % ToSz) return SDValue();

    unsigned SizeRatio  = FromSz / ToSz;

    assert(SizeRatio * NumElems * ToSz == VT.getSizeInBits());

    // Create a type on which we perform the shuffle
    EVT WideVecVT = EVT::getVectorVT(*DAG.getContext(),
            StVT.getScalarType(), NumElems*SizeRatio);

    assert(WideVecVT.getSizeInBits() == VT.getSizeInBits());

    SDValue WideVec = DAG.getNode(ISD::BITCAST, dl, WideVecVT, St->getValue());
    SmallVector<int, 8> ShuffleVec(NumElems * SizeRatio, -1);
    for (unsigned i = 0; i != NumElems; ++i)
      ShuffleVec[i] = i * SizeRatio;

    // Can't shuffle using an illegal type.
    if (!TLI.isTypeLegal(WideVecVT))
      return SDValue();

    SDValue Shuff = DAG.getVectorShuffle(WideVecVT, dl, WideVec,
                                         DAG.getUNDEF(WideVecVT),
                                         &ShuffleVec[0]);
    // At this point all of the data is stored at the bottom of the
    // register. We now need to save it to mem.

    // Find the largest store unit
    MVT StoreType = MVT::i8;
    for (unsigned tp = MVT::FIRST_INTEGER_VALUETYPE;
         tp < MVT::LAST_INTEGER_VALUETYPE; ++tp) {
      MVT Tp = (MVT::SimpleValueType)tp;
      if (TLI.isTypeLegal(Tp) && Tp.getSizeInBits() <= NumElems * ToSz)
        StoreType = Tp;
    }

    // On 32bit systems, we can't save 64bit integers. Try bitcasting to F64.
    if (TLI.isTypeLegal(MVT::f64) && StoreType.getSizeInBits() < 64 &&
        (64 <= NumElems * ToSz))
      StoreType = MVT::f64;

    // Bitcast the original vector into a vector of store-size units
    EVT StoreVecVT = EVT::getVectorVT(*DAG.getContext(),
            StoreType, VT.getSizeInBits()/StoreType.getSizeInBits());
    assert(StoreVecVT.getSizeInBits() == VT.getSizeInBits());
    SDValue ShuffWide = DAG.getNode(ISD::BITCAST, dl, StoreVecVT, Shuff);
    SmallVector<SDValue, 8> Chains;
    SDValue Increment = DAG.getConstant(StoreType.getSizeInBits()/8,
                                        TLI.getPointerTy());
    SDValue Ptr = St->getBasePtr();

    // Perform one or more big stores into memory.
    for (unsigned i=0, e=(ToSz*NumElems)/StoreType.getSizeInBits(); i!=e; ++i) {
      SDValue SubVec = DAG.getNode(ISD::EXTRACT_VECTOR_ELT, dl,
                                   StoreType, ShuffWide,
                                   DAG.getIntPtrConstant(i));
      SDValue Ch = DAG.getStore(St->getChain(), dl, SubVec, Ptr,
                                St->getPointerInfo(), St->isVolatile(),
                                St->isNonTemporal(), St->getAlignment());
      Ptr = DAG.getNode(ISD::ADD, dl, Ptr.getValueType(), Ptr, Increment);
      Chains.push_back(Ch);
    }

    return DAG.getNode(ISD::TokenFactor, dl, MVT::Other, Chains);
  }

  // Turn load->store of MMX types into GPR load/stores.  This avoids clobbering
  // the FP state in cases where an emms may be missing.
  // A preferable solution to the general problem is to figure out the right
  // places to insert EMMS.  This qualifies as a quick hack.

  // Similarly, turn load->store of i64 into double load/stores in 32-bit mode.
  if (VT.getSizeInBits() != 64)
    return SDValue();

  const Function *F = DAG.getMachineFunction().getFunction();
  bool NoImplicitFloatOps = F->getAttributes().
    hasAttribute(AttributeSet::FunctionIndex, Attribute::NoImplicitFloat);
  bool F64IsLegal = !DAG.getTarget().Options.UseSoftFloat && !NoImplicitFloatOps
                     && Subtarget->hasSSE2();
  if ((VT.isVector() ||
       (VT == MVT::i64 && F64IsLegal && !Subtarget->is64Bit())) &&
      isa<LoadSDNode>(St->getValue()) &&
      !cast<LoadSDNode>(St->getValue())->isVolatile() &&
      St->getChain().hasOneUse() && !St->isVolatile()) {
    SDNode* LdVal = St->getValue().getNode();
    LoadSDNode *Ld = nullptr;
    int TokenFactorIndex = -1;
    SmallVector<SDValue, 8> Ops;
    SDNode* ChainVal = St->getChain().getNode();
    // Must be a store of a load.  We currently handle two cases:  the load
    // is a direct child, and it's under an intervening TokenFactor.  It is
    // possible to dig deeper under nested TokenFactors.
    if (ChainVal == LdVal)
      Ld = cast<LoadSDNode>(St->getChain());
    else if (St->getValue().hasOneUse() &&
             ChainVal->getOpcode() == ISD::TokenFactor) {
      for (unsigned i = 0, e = ChainVal->getNumOperands(); i != e; ++i) {
        if (ChainVal->getOperand(i).getNode() == LdVal) {
          TokenFactorIndex = i;
          Ld = cast<LoadSDNode>(St->getValue());
        } else
          Ops.push_back(ChainVal->getOperand(i));
      }
    }

    if (!Ld || !ISD::isNormalLoad(Ld))
      return SDValue();

    // If this is not the MMX case, i.e. we are just turning i64 load/store
    // into f64 load/store, avoid the transformation if there are multiple
    // uses of the loaded value.
    if (!VT.isVector() && !Ld->hasNUsesOfValue(1, 0))
      return SDValue();

    SDLoc LdDL(Ld);
    SDLoc StDL(N);
    // If we are a 64-bit capable x86, lower to a single movq load/store pair.
    // Otherwise, if it's legal to use f64 SSE instructions, use f64 load/store
    // pair instead.
    if (Subtarget->is64Bit() || F64IsLegal) {
      EVT LdVT = Subtarget->is64Bit() ? MVT::i64 : MVT::f64;
      SDValue NewLd = DAG.getLoad(LdVT, LdDL, Ld->getChain(), Ld->getBasePtr(),
                                  Ld->getPointerInfo(), Ld->isVolatile(),
                                  Ld->isNonTemporal(), Ld->isInvariant(),
                                  Ld->getAlignment());
      SDValue NewChain = NewLd.getValue(1);
      if (TokenFactorIndex != -1) {
        Ops.push_back(NewChain);
        NewChain = DAG.getNode(ISD::TokenFactor, LdDL, MVT::Other, Ops);
      }
      return DAG.getStore(NewChain, StDL, NewLd, St->getBasePtr(),
                          St->getPointerInfo(),
                          St->isVolatile(), St->isNonTemporal(),
                          St->getAlignment());
    }

    // Otherwise, lower to two pairs of 32-bit loads / stores.
    SDValue LoAddr = Ld->getBasePtr();
    SDValue HiAddr = DAG.getNode(ISD::ADD, LdDL, MVT::i32, LoAddr,
                                 DAG.getConstant(4, MVT::i32));

    SDValue LoLd = DAG.getLoad(MVT::i32, LdDL, Ld->getChain(), LoAddr,
                               Ld->getPointerInfo(),
                               Ld->isVolatile(), Ld->isNonTemporal(),
                               Ld->isInvariant(), Ld->getAlignment());
    SDValue HiLd = DAG.getLoad(MVT::i32, LdDL, Ld->getChain(), HiAddr,
                               Ld->getPointerInfo().getWithOffset(4),
                               Ld->isVolatile(), Ld->isNonTemporal(),
                               Ld->isInvariant(),
                               MinAlign(Ld->getAlignment(), 4));

    SDValue NewChain = LoLd.getValue(1);
    if (TokenFactorIndex != -1) {
      Ops.push_back(LoLd);
      Ops.push_back(HiLd);
      NewChain = DAG.getNode(ISD::TokenFactor, LdDL, MVT::Other, Ops);
    }

    LoAddr = St->getBasePtr();
    HiAddr = DAG.getNode(ISD::ADD, StDL, MVT::i32, LoAddr,
                         DAG.getConstant(4, MVT::i32));

    SDValue LoSt = DAG.getStore(NewChain, StDL, LoLd, LoAddr,
                                St->getPointerInfo(),
                                St->isVolatile(), St->isNonTemporal(),
                                St->getAlignment());
    SDValue HiSt = DAG.getStore(NewChain, StDL, HiLd, HiAddr,
                                St->getPointerInfo().getWithOffset(4),
                                St->isVolatile(),
                                St->isNonTemporal(),
                                MinAlign(St->getAlignment(), 4));
    return DAG.getNode(ISD::TokenFactor, StDL, MVT::Other, LoSt, HiSt);
  }
  return SDValue();
}

/// isHorizontalBinOp - Return 'true' if this vector operation is "horizontal"
/// and return the operands for the horizontal operation in LHS and RHS.  A
/// horizontal operation performs the binary operation on successive elements
/// of its first operand, then on successive elements of its second operand,
/// returning the resulting values in a vector.  For example, if
///   A = < float a0, float a1, float a2, float a3 >
/// and
///   B = < float b0, float b1, float b2, float b3 >
/// then the result of doing a horizontal operation on A and B is
///   A horizontal-op B = < a0 op a1, a2 op a3, b0 op b1, b2 op b3 >.
/// In short, LHS and RHS are inspected to see if LHS op RHS is of the form
/// A horizontal-op B, for some already available A and B, and if so then LHS is
/// set to A, RHS to B, and the routine returns 'true'.
/// Note that the binary operation should have the property that if one of the
/// operands is UNDEF then the result is UNDEF.
static bool isHorizontalBinOp(SDValue &LHS, SDValue &RHS, bool IsCommutative) {
  // Look for the following pattern: if
  //   A = < float a0, float a1, float a2, float a3 >
  //   B = < float b0, float b1, float b2, float b3 >
  // and
  //   LHS = VECTOR_SHUFFLE A, B, <0, 2, 4, 6>
  //   RHS = VECTOR_SHUFFLE A, B, <1, 3, 5, 7>
  // then LHS op RHS = < a0 op a1, a2 op a3, b0 op b1, b2 op b3 >
  // which is A horizontal-op B.

  // At least one of the operands should be a vector shuffle.
  if (LHS.getOpcode() != ISD::VECTOR_SHUFFLE &&
      RHS.getOpcode() != ISD::VECTOR_SHUFFLE)
    return false;

  MVT VT = LHS.getSimpleValueType();

  assert((VT.is128BitVector() || VT.is256BitVector()) &&
         "Unsupported vector type for horizontal add/sub");

  // Handle 128 and 256-bit vector lengths. AVX defines horizontal add/sub to
  // operate independently on 128-bit lanes.
  unsigned NumElts = VT.getVectorNumElements();
  unsigned NumLanes = VT.getSizeInBits()/128;
  unsigned NumLaneElts = NumElts / NumLanes;
  assert((NumLaneElts % 2 == 0) &&
         "Vector type should have an even number of elements in each lane");
  unsigned HalfLaneElts = NumLaneElts/2;

  // View LHS in the form
  //   LHS = VECTOR_SHUFFLE A, B, LMask
  // If LHS is not a shuffle then pretend it is the shuffle
  //   LHS = VECTOR_SHUFFLE LHS, undef, <0, 1, ..., N-1>
  // NOTE: in what follows a default initialized SDValue represents an UNDEF of
  // type VT.
  SDValue A, B;
  SmallVector<int, 16> LMask(NumElts);
  if (LHS.getOpcode() == ISD::VECTOR_SHUFFLE) {
    if (LHS.getOperand(0).getOpcode() != ISD::UNDEF)
      A = LHS.getOperand(0);
    if (LHS.getOperand(1).getOpcode() != ISD::UNDEF)
      B = LHS.getOperand(1);
    ArrayRef<int> Mask = cast<ShuffleVectorSDNode>(LHS.getNode())->getMask();
    std::copy(Mask.begin(), Mask.end(), LMask.begin());
  } else {
    if (LHS.getOpcode() != ISD::UNDEF)
      A = LHS;
    for (unsigned i = 0; i != NumElts; ++i)
      LMask[i] = i;
  }

  // Likewise, view RHS in the form
  //   RHS = VECTOR_SHUFFLE C, D, RMask
  SDValue C, D;
  SmallVector<int, 16> RMask(NumElts);
  if (RHS.getOpcode() == ISD::VECTOR_SHUFFLE) {
    if (RHS.getOperand(0).getOpcode() != ISD::UNDEF)
      C = RHS.getOperand(0);
    if (RHS.getOperand(1).getOpcode() != ISD::UNDEF)
      D = RHS.getOperand(1);
    ArrayRef<int> Mask = cast<ShuffleVectorSDNode>(RHS.getNode())->getMask();
    std::copy(Mask.begin(), Mask.end(), RMask.begin());
  } else {
    if (RHS.getOpcode() != ISD::UNDEF)
      C = RHS;
    for (unsigned i = 0; i != NumElts; ++i)
      RMask[i] = i;
  }

  // Check that the shuffles are both shuffling the same vectors.
  if (!(A == C && B == D) && !(A == D && B == C))
    return false;

  // If everything is UNDEF then bail out: it would be better to fold to UNDEF.
  if (!A.getNode() && !B.getNode())
    return false;

  // If A and B occur in reverse order in RHS, then "swap" them (which means
  // rewriting the mask).
  if (A != C)
    CommuteVectorShuffleMask(RMask, NumElts);

  // At this point LHS and RHS are equivalent to
  //   LHS = VECTOR_SHUFFLE A, B, LMask
  //   RHS = VECTOR_SHUFFLE A, B, RMask
  // Check that the masks correspond to performing a horizontal operation.
  for (unsigned l = 0; l != NumElts; l += NumLaneElts) {
    for (unsigned i = 0; i != NumLaneElts; ++i) {
      int LIdx = LMask[i+l], RIdx = RMask[i+l];

      // Ignore any UNDEF components.
      if (LIdx < 0 || RIdx < 0 ||
          (!A.getNode() && (LIdx < (int)NumElts || RIdx < (int)NumElts)) ||
          (!B.getNode() && (LIdx >= (int)NumElts || RIdx >= (int)NumElts)))
        continue;

      // Check that successive elements are being operated on.  If not, this is
      // not a horizontal operation.
      unsigned Src = (i/HalfLaneElts); // each lane is split between srcs
      int Index = 2*(i%HalfLaneElts) + NumElts*Src + l;
      if (!(LIdx == Index && RIdx == Index + 1) &&
          !(IsCommutative && LIdx == Index + 1 && RIdx == Index))
        return false;
    }
  }

  LHS = A.getNode() ? A : B; // If A is 'UNDEF', use B for it.
  RHS = B.getNode() ? B : A; // If B is 'UNDEF', use A for it.
  return true;
}

/// PerformFADDCombine - Do target-specific dag combines on floating point adds.
static SDValue PerformFADDCombine(SDNode *N, SelectionDAG &DAG,
                                  const X86Subtarget *Subtarget) {
  EVT VT = N->getValueType(0);
  SDValue LHS = N->getOperand(0);
  SDValue RHS = N->getOperand(1);

  // Try to synthesize horizontal adds from adds of shuffles.
  if (((Subtarget->hasSSE3() && (VT == MVT::v4f32 || VT == MVT::v2f64)) ||
       (Subtarget->hasFp256() && (VT == MVT::v8f32 || VT == MVT::v4f64))) &&
      isHorizontalBinOp(LHS, RHS, true))
    return DAG.getNode(X86ISD::FHADD, SDLoc(N), VT, LHS, RHS);
  return SDValue();
}

/// PerformFSUBCombine - Do target-specific dag combines on floating point subs.
static SDValue PerformFSUBCombine(SDNode *N, SelectionDAG &DAG,
                                  const X86Subtarget *Subtarget) {
  EVT VT = N->getValueType(0);
  SDValue LHS = N->getOperand(0);
  SDValue RHS = N->getOperand(1);

  // Try to synthesize horizontal subs from subs of shuffles.
  if (((Subtarget->hasSSE3() && (VT == MVT::v4f32 || VT == MVT::v2f64)) ||
       (Subtarget->hasFp256() && (VT == MVT::v8f32 || VT == MVT::v4f64))) &&
      isHorizontalBinOp(LHS, RHS, false))
    return DAG.getNode(X86ISD::FHSUB, SDLoc(N), VT, LHS, RHS);
  return SDValue();
}

/// PerformFORCombine - Do target-specific dag combines on X86ISD::FOR and
/// X86ISD::FXOR nodes.
static SDValue PerformFORCombine(SDNode *N, SelectionDAG &DAG) {
  assert(N->getOpcode() == X86ISD::FOR || N->getOpcode() == X86ISD::FXOR);
  // F[X]OR(0.0, x) -> x
  // F[X]OR(x, 0.0) -> x
  if (ConstantFPSDNode *C = dyn_cast<ConstantFPSDNode>(N->getOperand(0)))
    if (C->getValueAPF().isPosZero())
      return N->getOperand(1);
  if (ConstantFPSDNode *C = dyn_cast<ConstantFPSDNode>(N->getOperand(1)))
    if (C->getValueAPF().isPosZero())
      return N->getOperand(0);
  return SDValue();
}

/// PerformFMinFMaxCombine - Do target-specific dag combines on X86ISD::FMIN and
/// X86ISD::FMAX nodes.
static SDValue PerformFMinFMaxCombine(SDNode *N, SelectionDAG &DAG) {
  assert(N->getOpcode() == X86ISD::FMIN || N->getOpcode() == X86ISD::FMAX);

  // Only perform optimizations if UnsafeMath is used.
  if (!DAG.getTarget().Options.UnsafeFPMath)
    return SDValue();

  // If we run in unsafe-math mode, then convert the FMAX and FMIN nodes
  // into FMINC and FMAXC, which are Commutative operations.
  unsigned NewOp = 0;
  switch (N->getOpcode()) {
    default: llvm_unreachable("unknown opcode");
    case X86ISD::FMIN:  NewOp = X86ISD::FMINC; break;
    case X86ISD::FMAX:  NewOp = X86ISD::FMAXC; break;
  }

  return DAG.getNode(NewOp, SDLoc(N), N->getValueType(0),
                     N->getOperand(0), N->getOperand(1));
}

/// PerformFANDCombine - Do target-specific dag combines on X86ISD::FAND nodes.
static SDValue PerformFANDCombine(SDNode *N, SelectionDAG &DAG) {
  // FAND(0.0, x) -> 0.0
  // FAND(x, 0.0) -> 0.0
  if (ConstantFPSDNode *C = dyn_cast<ConstantFPSDNode>(N->getOperand(0)))
    if (C->getValueAPF().isPosZero())
      return N->getOperand(0);
  if (ConstantFPSDNode *C = dyn_cast<ConstantFPSDNode>(N->getOperand(1)))
    if (C->getValueAPF().isPosZero())
      return N->getOperand(1);
  return SDValue();
}

/// PerformFANDNCombine - Do target-specific dag combines on X86ISD::FANDN nodes
static SDValue PerformFANDNCombine(SDNode *N, SelectionDAG &DAG) {
  // FANDN(x, 0.0) -> 0.0
  // FANDN(0.0, x) -> x
  if (ConstantFPSDNode *C = dyn_cast<ConstantFPSDNode>(N->getOperand(0)))
    if (C->getValueAPF().isPosZero())
      return N->getOperand(1);
  if (ConstantFPSDNode *C = dyn_cast<ConstantFPSDNode>(N->getOperand(1)))
    if (C->getValueAPF().isPosZero())
      return N->getOperand(1);
  return SDValue();
}

static SDValue PerformBTCombine(SDNode *N,
                                SelectionDAG &DAG,
                                TargetLowering::DAGCombinerInfo &DCI) {
  // BT ignores high bits in the bit index operand.
  SDValue Op1 = N->getOperand(1);
  if (Op1.hasOneUse()) {
    unsigned BitWidth = Op1.getValueSizeInBits();
    APInt DemandedMask = APInt::getLowBitsSet(BitWidth, Log2_32(BitWidth));
    APInt KnownZero, KnownOne;
    TargetLowering::TargetLoweringOpt TLO(DAG, !DCI.isBeforeLegalize(),
                                          !DCI.isBeforeLegalizeOps());
    const TargetLowering &TLI = DAG.getTargetLoweringInfo();
    if (TLO.ShrinkDemandedConstant(Op1, DemandedMask) ||
        TLI.SimplifyDemandedBits(Op1, DemandedMask, KnownZero, KnownOne, TLO))
      DCI.CommitTargetLoweringOpt(TLO);
  }
  return SDValue();
}

static SDValue PerformVZEXT_MOVLCombine(SDNode *N, SelectionDAG &DAG) {
  SDValue Op = N->getOperand(0);
  if (Op.getOpcode() == ISD::BITCAST)
    Op = Op.getOperand(0);
  EVT VT = N->getValueType(0), OpVT = Op.getValueType();
  if (Op.getOpcode() == X86ISD::VZEXT_LOAD &&
      VT.getVectorElementType().getSizeInBits() ==
      OpVT.getVectorElementType().getSizeInBits()) {
    return DAG.getNode(ISD::BITCAST, SDLoc(N), VT, Op);
  }
  return SDValue();
}

static SDValue PerformSIGN_EXTEND_INREGCombine(SDNode *N, SelectionDAG &DAG,
                                               const X86Subtarget *Subtarget) {
  EVT VT = N->getValueType(0);
  if (!VT.isVector())
    return SDValue();

  SDValue N0 = N->getOperand(0);
  SDValue N1 = N->getOperand(1);
  EVT ExtraVT = cast<VTSDNode>(N1)->getVT();
  SDLoc dl(N);

  // The SIGN_EXTEND_INREG to v4i64 is expensive operation on the
  // both SSE and AVX2 since there is no sign-extended shift right
  // operation on a vector with 64-bit elements.
  //(sext_in_reg (v4i64 anyext (v4i32 x )), ExtraVT) ->
  // (v4i64 sext (v4i32 sext_in_reg (v4i32 x , ExtraVT)))
  if (VT == MVT::v4i64 && (N0.getOpcode() == ISD::ANY_EXTEND ||
      N0.getOpcode() == ISD::SIGN_EXTEND)) {
    SDValue N00 = N0.getOperand(0);

    // EXTLOAD has a better solution on AVX2,
    // it may be replaced with X86ISD::VSEXT node.
    if (N00.getOpcode() == ISD::LOAD && Subtarget->hasInt256())
      if (!ISD::isNormalLoad(N00.getNode()))
        return SDValue();

    if (N00.getValueType() == MVT::v4i32 && ExtraVT.getSizeInBits() < 128) {
        SDValue Tmp = DAG.getNode(ISD::SIGN_EXTEND_INREG, dl, MVT::v4i32,
                                  N00, N1);
      return DAG.getNode(ISD::SIGN_EXTEND, dl, MVT::v4i64, Tmp);
    }
  }
  return SDValue();
}

static SDValue PerformSExtCombine(SDNode *N, SelectionDAG &DAG,
                                  TargetLowering::DAGCombinerInfo &DCI,
                                  const X86Subtarget *Subtarget) {
  if (!DCI.isBeforeLegalizeOps())
    return SDValue();

  if (!Subtarget->hasFp256())
    return SDValue();

  EVT VT = N->getValueType(0);
  if (VT.isVector() && VT.getSizeInBits() == 256) {
    SDValue R = WidenMaskArithmetic(N, DAG, DCI, Subtarget);
    if (R.getNode())
      return R;
  }

  return SDValue();
}

static SDValue PerformFMACombine(SDNode *N, SelectionDAG &DAG,
                                 const X86Subtarget* Subtarget) {
  SDLoc dl(N);
  EVT VT = N->getValueType(0);

  // Let legalize expand this if it isn't a legal type yet.
  if (!DAG.getTargetLoweringInfo().isTypeLegal(VT))
    return SDValue();

  EVT ScalarVT = VT.getScalarType();
  if ((ScalarVT != MVT::f32 && ScalarVT != MVT::f64) ||
      (!Subtarget->hasFMA() && !Subtarget->hasFMA4()))
    return SDValue();

  SDValue A = N->getOperand(0);
  SDValue B = N->getOperand(1);
  SDValue C = N->getOperand(2);

  bool NegA = (A.getOpcode() == ISD::FNEG);
  bool NegB = (B.getOpcode() == ISD::FNEG);
  bool NegC = (C.getOpcode() == ISD::FNEG);

  // Negative multiplication when NegA xor NegB
  bool NegMul = (NegA != NegB);
  if (NegA)
    A = A.getOperand(0);
  if (NegB)
    B = B.getOperand(0);
  if (NegC)
    C = C.getOperand(0);

  unsigned Opcode;
  if (!NegMul)
    Opcode = (!NegC) ? X86ISD::FMADD : X86ISD::FMSUB;
  else
    Opcode = (!NegC) ? X86ISD::FNMADD : X86ISD::FNMSUB;

  return DAG.getNode(Opcode, dl, VT, A, B, C);
}

static SDValue PerformZExtCombine(SDNode *N, SelectionDAG &DAG,
                                  TargetLowering::DAGCombinerInfo &DCI,
                                  const X86Subtarget *Subtarget) {
  // (i32 zext (and (i8  x86isd::setcc_carry), 1)) ->
  //           (and (i32 x86isd::setcc_carry), 1)
  // This eliminates the zext. This transformation is necessary because
  // ISD::SETCC is always legalized to i8.
  SDLoc dl(N);
  SDValue N0 = N->getOperand(0);
  EVT VT = N->getValueType(0);

  if (N0.getOpcode() == ISD::AND &&
      N0.hasOneUse() &&
      N0.getOperand(0).hasOneUse()) {
    SDValue N00 = N0.getOperand(0);
    if (N00.getOpcode() == X86ISD::SETCC_CARRY) {
      ConstantSDNode *C = dyn_cast<ConstantSDNode>(N0.getOperand(1));
      if (!C || C->getZExtValue() != 1)
        return SDValue();
      return DAG.getNode(ISD::AND, dl, VT,
                         DAG.getNode(X86ISD::SETCC_CARRY, dl, VT,
                                     N00.getOperand(0), N00.getOperand(1)),
                         DAG.getConstant(1, VT));
    }
  }

  if (N0.getOpcode() == ISD::TRUNCATE &&
      N0.hasOneUse() &&
      N0.getOperand(0).hasOneUse()) {
    SDValue N00 = N0.getOperand(0);
    if (N00.getOpcode() == X86ISD::SETCC_CARRY) {
      return DAG.getNode(ISD::AND, dl, VT,
                         DAG.getNode(X86ISD::SETCC_CARRY, dl, VT,
                                     N00.getOperand(0), N00.getOperand(1)),
                         DAG.getConstant(1, VT));
    }
  }
  if (VT.is256BitVector()) {
    SDValue R = WidenMaskArithmetic(N, DAG, DCI, Subtarget);
    if (R.getNode())
      return R;
  }

  return SDValue();
}

// Optimize x == -y --> x+y == 0
//          x != -y --> x+y != 0
static SDValue PerformISDSETCCCombine(SDNode *N, SelectionDAG &DAG,
                                      const X86Subtarget* Subtarget) {
  ISD::CondCode CC = cast<CondCodeSDNode>(N->getOperand(2))->get();
  SDValue LHS = N->getOperand(0);
  SDValue RHS = N->getOperand(1);
  EVT VT = N->getValueType(0);
  SDLoc DL(N);

  if ((CC == ISD::SETNE || CC == ISD::SETEQ) && LHS.getOpcode() == ISD::SUB)
    if (ConstantSDNode *C = dyn_cast<ConstantSDNode>(LHS.getOperand(0)))
      if (C->getAPIntValue() == 0 && LHS.hasOneUse()) {
        SDValue addV = DAG.getNode(ISD::ADD, SDLoc(N),
                                   LHS.getValueType(), RHS, LHS.getOperand(1));
        return DAG.getSetCC(SDLoc(N), N->getValueType(0),
                            addV, DAG.getConstant(0, addV.getValueType()), CC);
      }
  if ((CC == ISD::SETNE || CC == ISD::SETEQ) && RHS.getOpcode() == ISD::SUB)
    if (ConstantSDNode *C = dyn_cast<ConstantSDNode>(RHS.getOperand(0)))
      if (C->getAPIntValue() == 0 && RHS.hasOneUse()) {
        SDValue addV = DAG.getNode(ISD::ADD, SDLoc(N),
                                   RHS.getValueType(), LHS, RHS.getOperand(1));
        return DAG.getSetCC(SDLoc(N), N->getValueType(0),
                            addV, DAG.getConstant(0, addV.getValueType()), CC);
      }

  if (VT.getScalarType() == MVT::i1) {
    bool IsSEXT0 = (LHS.getOpcode() == ISD::SIGN_EXTEND) &&
      (LHS.getOperand(0).getValueType().getScalarType() ==  MVT::i1);
    bool IsVZero0 = ISD::isBuildVectorAllZeros(LHS.getNode());
    if (!IsSEXT0 && !IsVZero0)
      return SDValue();
    bool IsSEXT1 = (RHS.getOpcode() == ISD::SIGN_EXTEND) &&
      (RHS.getOperand(0).getValueType().getScalarType() ==  MVT::i1);
    bool IsVZero1 = ISD::isBuildVectorAllZeros(RHS.getNode());

    if (!IsSEXT1 && !IsVZero1)
      return SDValue();

    if (IsSEXT0 && IsVZero1) {
      assert(VT == LHS.getOperand(0).getValueType() && "Uexpected operand type");
      if (CC == ISD::SETEQ)
        return DAG.getNOT(DL, LHS.getOperand(0), VT);
      return LHS.getOperand(0);
    }
    if (IsSEXT1 && IsVZero0) {
      assert(VT == RHS.getOperand(0).getValueType() && "Uexpected operand type");
      if (CC == ISD::SETEQ)
        return DAG.getNOT(DL, RHS.getOperand(0), VT);
      return RHS.getOperand(0);
    }
  }

  return SDValue();
}

static SDValue PerformINSERTPSCombine(SDNode *N, SelectionDAG &DAG,
                                      const X86Subtarget *Subtarget) {
  SDLoc dl(N);
  MVT VT = N->getOperand(1)->getSimpleValueType(0);
  assert((VT == MVT::v4f32 || VT == MVT::v4i32) &&
         "X86insertps is only defined for v4x32");

  SDValue Ld = N->getOperand(1);
  if (MayFoldLoad(Ld)) {
    // Extract the countS bits from the immediate so we can get the proper
    // address when narrowing the vector load to a specific element.
    // When the second source op is a memory address, interps doesn't use
    // countS and just gets an f32 from that address.
    unsigned DestIndex =
        cast<ConstantSDNode>(N->getOperand(2))->getZExtValue() >> 6;
    Ld = NarrowVectorLoadToElement(cast<LoadSDNode>(Ld), DestIndex, DAG);
  } else
    return SDValue();

  // Create this as a scalar to vector to match the instruction pattern.
  SDValue LoadScalarToVector = DAG.getNode(ISD::SCALAR_TO_VECTOR, dl, VT, Ld);
  // countS bits are ignored when loading from memory on insertps, which
  // means we don't need to explicitly set them to 0.
  return DAG.getNode(X86ISD::INSERTPS, dl, VT, N->getOperand(0),
                     LoadScalarToVector, N->getOperand(2));
}

// Helper function of PerformSETCCCombine. It is to materialize "setb reg"
// as "sbb reg,reg", since it can be extended without zext and produces
// an all-ones bit which is more useful than 0/1 in some cases.
static SDValue MaterializeSETB(SDLoc DL, SDValue EFLAGS, SelectionDAG &DAG,
                               MVT VT) {
  if (VT == MVT::i8)
    return DAG.getNode(ISD::AND, DL, VT,
                       DAG.getNode(X86ISD::SETCC_CARRY, DL, MVT::i8,
                                   DAG.getConstant(X86::COND_B, MVT::i8), EFLAGS),
                       DAG.getConstant(1, VT));
  assert (VT == MVT::i1 && "Unexpected type for SECCC node");
  return DAG.getNode(ISD::TRUNCATE, DL, MVT::i1,
                     DAG.getNode(X86ISD::SETCC_CARRY, DL, MVT::i8,
                                 DAG.getConstant(X86::COND_B, MVT::i8), EFLAGS));
}

// Optimize  RES = X86ISD::SETCC CONDCODE, EFLAG_INPUT
static SDValue PerformSETCCCombine(SDNode *N, SelectionDAG &DAG,
                                   TargetLowering::DAGCombinerInfo &DCI,
                                   const X86Subtarget *Subtarget) {
  SDLoc DL(N);
  X86::CondCode CC = X86::CondCode(N->getConstantOperandVal(0));
  SDValue EFLAGS = N->getOperand(1);

  if (CC == X86::COND_A) {
    // Try to convert COND_A into COND_B in an attempt to facilitate
    // materializing "setb reg".
    //
    // Do not flip "e > c", where "c" is a constant, because Cmp instruction
    // cannot take an immediate as its first operand.
    //
    if (EFLAGS.getOpcode() == X86ISD::SUB && EFLAGS.hasOneUse() &&
        EFLAGS.getValueType().isInteger() &&
        !isa<ConstantSDNode>(EFLAGS.getOperand(1))) {
      SDValue NewSub = DAG.getNode(X86ISD::SUB, SDLoc(EFLAGS),
                                   EFLAGS.getNode()->getVTList(),
                                   EFLAGS.getOperand(1), EFLAGS.getOperand(0));
      SDValue NewEFLAGS = SDValue(NewSub.getNode(), EFLAGS.getResNo());
      return MaterializeSETB(DL, NewEFLAGS, DAG, N->getSimpleValueType(0));
    }
  }

  // Materialize "setb reg" as "sbb reg,reg", since it can be extended without
  // a zext and produces an all-ones bit which is more useful than 0/1 in some
  // cases.
  if (CC == X86::COND_B)
    return MaterializeSETB(DL, EFLAGS, DAG, N->getSimpleValueType(0));

  SDValue Flags;

  Flags = checkBoolTestSetCCCombine(EFLAGS, CC);
  if (Flags.getNode()) {
    SDValue Cond = DAG.getConstant(CC, MVT::i8);
    return DAG.getNode(X86ISD::SETCC, DL, N->getVTList(), Cond, Flags);
  }

  return SDValue();
}

// Optimize branch condition evaluation.
//
static SDValue PerformBrCondCombine(SDNode *N, SelectionDAG &DAG,
                                    TargetLowering::DAGCombinerInfo &DCI,
                                    const X86Subtarget *Subtarget) {
  SDLoc DL(N);
  SDValue Chain = N->getOperand(0);
  SDValue Dest = N->getOperand(1);
  SDValue EFLAGS = N->getOperand(3);
  X86::CondCode CC = X86::CondCode(N->getConstantOperandVal(2));

  SDValue Flags;

  Flags = checkBoolTestSetCCCombine(EFLAGS, CC);
  if (Flags.getNode()) {
    SDValue Cond = DAG.getConstant(CC, MVT::i8);
    return DAG.getNode(X86ISD::BRCOND, DL, N->getVTList(), Chain, Dest, Cond,
                       Flags);
  }

  return SDValue();
}

static SDValue performVectorCompareAndMaskUnaryOpCombine(SDNode *N,
                                                         SelectionDAG &DAG) {
  // Take advantage of vector comparisons producing 0 or -1 in each lane to
  // optimize away operation when it's from a constant.
  //
  // The general transformation is:
  //    UNARYOP(AND(VECTOR_CMP(x,y), constant)) -->
  //       AND(VECTOR_CMP(x,y), constant2)
  //    constant2 = UNARYOP(constant)

  // Early exit if this isn't a vector operation, the operand of the
  // unary operation isn't a bitwise AND, or if the sizes of the operations
  // aren't the same.
  EVT VT = N->getValueType(0);
  if (!VT.isVector() || N->getOperand(0)->getOpcode() != ISD::AND ||
      N->getOperand(0)->getOperand(0)->getOpcode() != ISD::SETCC ||
      VT.getSizeInBits() != N->getOperand(0)->getValueType(0).getSizeInBits())
    return SDValue();

  // Now check that the other operand of the AND is a constant. We could
  // make the transformation for non-constant splats as well, but it's unclear
  // that would be a benefit as it would not eliminate any operations, just
  // perform one more step in scalar code before moving to the vector unit.
  if (BuildVectorSDNode *BV =
          dyn_cast<BuildVectorSDNode>(N->getOperand(0)->getOperand(1))) {
    // Bail out if the vector isn't a constant.
    if (!BV->isConstant())
      return SDValue();

    // Everything checks out. Build up the new and improved node.
    SDLoc DL(N);
    EVT IntVT = BV->getValueType(0);
    // Create a new constant of the appropriate type for the transformed
    // DAG.
    SDValue SourceConst = DAG.getNode(N->getOpcode(), DL, VT, SDValue(BV, 0));
    // The AND node needs bitcasts to/from an integer vector type around it.
    SDValue MaskConst = DAG.getNode(ISD::BITCAST, DL, IntVT, SourceConst);
    SDValue NewAnd = DAG.getNode(ISD::AND, DL, IntVT,
                                 N->getOperand(0)->getOperand(0), MaskConst);
    SDValue Res = DAG.getNode(ISD::BITCAST, DL, VT, NewAnd);
    return Res;
  }

  return SDValue();
}

static SDValue PerformSINT_TO_FPCombine(SDNode *N, SelectionDAG &DAG,
                                        const X86TargetLowering *XTLI) {
  // First try to optimize away the conversion entirely when it's
  // conditionally from a constant. Vectors only.
  SDValue Res = performVectorCompareAndMaskUnaryOpCombine(N, DAG);
  if (Res != SDValue())
    return Res;

  // Now move on to more general possibilities.
  SDValue Op0 = N->getOperand(0);
  EVT InVT = Op0->getValueType(0);

  // SINT_TO_FP(v4i8) -> SINT_TO_FP(SEXT(v4i8 to v4i32))
  if (InVT == MVT::v8i8 || InVT == MVT::v4i8) {
    SDLoc dl(N);
    MVT DstVT = InVT == MVT::v4i8 ? MVT::v4i32 : MVT::v8i32;
    SDValue P = DAG.getNode(ISD::SIGN_EXTEND, dl, DstVT, Op0);
    return DAG.getNode(ISD::SINT_TO_FP, dl, N->getValueType(0), P);
  }

  // Transform (SINT_TO_FP (i64 ...)) into an x87 operation if we have
  // a 32-bit target where SSE doesn't support i64->FP operations.
  if (Op0.getOpcode() == ISD::LOAD) {
    LoadSDNode *Ld = cast<LoadSDNode>(Op0.getNode());
    EVT VT = Ld->getValueType(0);
    if (!Ld->isVolatile() && !N->getValueType(0).isVector() &&
        ISD::isNON_EXTLoad(Op0.getNode()) && Op0.hasOneUse() &&
        !XTLI->getSubtarget()->is64Bit() &&
        VT == MVT::i64) {
      SDValue FILDChain = XTLI->BuildFILD(SDValue(N, 0), Ld->getValueType(0),
                                          Ld->getChain(), Op0, DAG);
      DAG.ReplaceAllUsesOfValueWith(Op0.getValue(1), FILDChain.getValue(1));
      return FILDChain;
    }
  }
  return SDValue();
}

// Optimize RES, EFLAGS = X86ISD::ADC LHS, RHS, EFLAGS
static SDValue PerformADCCombine(SDNode *N, SelectionDAG &DAG,
                                 X86TargetLowering::DAGCombinerInfo &DCI) {
  // If the LHS and RHS of the ADC node are zero, then it can't overflow and
  // the result is either zero or one (depending on the input carry bit).
  // Strength reduce this down to a "set on carry" aka SETCC_CARRY&1.
  if (X86::isZeroNode(N->getOperand(0)) &&
      X86::isZeroNode(N->getOperand(1)) &&
      // We don't have a good way to replace an EFLAGS use, so only do this when
      // dead right now.
      SDValue(N, 1).use_empty()) {
    SDLoc DL(N);
    EVT VT = N->getValueType(0);
    SDValue CarryOut = DAG.getConstant(0, N->getValueType(1));
    SDValue Res1 = DAG.getNode(ISD::AND, DL, VT,
                               DAG.getNode(X86ISD::SETCC_CARRY, DL, VT,
                                           DAG.getConstant(X86::COND_B,MVT::i8),
                                           N->getOperand(2)),
                               DAG.getConstant(1, VT));
    return DCI.CombineTo(N, Res1, CarryOut);
  }

  return SDValue();
}

// fold (add Y, (sete  X, 0)) -> adc  0, Y
//      (add Y, (setne X, 0)) -> sbb -1, Y
//      (sub (sete  X, 0), Y) -> sbb  0, Y
//      (sub (setne X, 0), Y) -> adc -1, Y
static SDValue OptimizeConditionalInDecrement(SDNode *N, SelectionDAG &DAG) {
  SDLoc DL(N);

  // Look through ZExts.
  SDValue Ext = N->getOperand(N->getOpcode() == ISD::SUB ? 1 : 0);
  if (Ext.getOpcode() != ISD::ZERO_EXTEND || !Ext.hasOneUse())
    return SDValue();

  SDValue SetCC = Ext.getOperand(0);
  if (SetCC.getOpcode() != X86ISD::SETCC || !SetCC.hasOneUse())
    return SDValue();

  X86::CondCode CC = (X86::CondCode)SetCC.getConstantOperandVal(0);
  if (CC != X86::COND_E && CC != X86::COND_NE)
    return SDValue();

  SDValue Cmp = SetCC.getOperand(1);
  if (Cmp.getOpcode() != X86ISD::CMP || !Cmp.hasOneUse() ||
      !X86::isZeroNode(Cmp.getOperand(1)) ||
      !Cmp.getOperand(0).getValueType().isInteger())
    return SDValue();

  SDValue CmpOp0 = Cmp.getOperand(0);
  SDValue NewCmp = DAG.getNode(X86ISD::CMP, DL, MVT::i32, CmpOp0,
                               DAG.getConstant(1, CmpOp0.getValueType()));

  SDValue OtherVal = N->getOperand(N->getOpcode() == ISD::SUB ? 0 : 1);
  if (CC == X86::COND_NE)
    return DAG.getNode(N->getOpcode() == ISD::SUB ? X86ISD::ADC : X86ISD::SBB,
                       DL, OtherVal.getValueType(), OtherVal,
                       DAG.getConstant(-1ULL, OtherVal.getValueType()), NewCmp);
  return DAG.getNode(N->getOpcode() == ISD::SUB ? X86ISD::SBB : X86ISD::ADC,
                     DL, OtherVal.getValueType(), OtherVal,
                     DAG.getConstant(0, OtherVal.getValueType()), NewCmp);
}

/// PerformADDCombine - Do target-specific dag combines on integer adds.
static SDValue PerformAddCombine(SDNode *N, SelectionDAG &DAG,
                                 const X86Subtarget *Subtarget) {
  EVT VT = N->getValueType(0);
  SDValue Op0 = N->getOperand(0);
  SDValue Op1 = N->getOperand(1);

  // Try to synthesize horizontal adds from adds of shuffles.
  if (((Subtarget->hasSSSE3() && (VT == MVT::v8i16 || VT == MVT::v4i32)) ||
       (Subtarget->hasInt256() && (VT == MVT::v16i16 || VT == MVT::v8i32))) &&
      isHorizontalBinOp(Op0, Op1, true))
    return DAG.getNode(X86ISD::HADD, SDLoc(N), VT, Op0, Op1);

  return OptimizeConditionalInDecrement(N, DAG);
}

static SDValue PerformSubCombine(SDNode *N, SelectionDAG &DAG,
                                 const X86Subtarget *Subtarget) {
  SDValue Op0 = N->getOperand(0);
  SDValue Op1 = N->getOperand(1);

  // X86 can't encode an immediate LHS of a sub. See if we can push the
  // negation into a preceding instruction.
  if (ConstantSDNode *C = dyn_cast<ConstantSDNode>(Op0)) {
    // If the RHS of the sub is a XOR with one use and a constant, invert the
    // immediate. Then add one to the LHS of the sub so we can turn
    // X-Y -> X+~Y+1, saving one register.
    if (Op1->hasOneUse() && Op1.getOpcode() == ISD::XOR &&
        isa<ConstantSDNode>(Op1.getOperand(1))) {
      APInt XorC = cast<ConstantSDNode>(Op1.getOperand(1))->getAPIntValue();
      EVT VT = Op0.getValueType();
      SDValue NewXor = DAG.getNode(ISD::XOR, SDLoc(Op1), VT,
                                   Op1.getOperand(0),
                                   DAG.getConstant(~XorC, VT));
      return DAG.getNode(ISD::ADD, SDLoc(N), VT, NewXor,
                         DAG.getConstant(C->getAPIntValue()+1, VT));
    }
  }

  // Try to synthesize horizontal adds from adds of shuffles.
  EVT VT = N->getValueType(0);
  if (((Subtarget->hasSSSE3() && (VT == MVT::v8i16 || VT == MVT::v4i32)) ||
       (Subtarget->hasInt256() && (VT == MVT::v16i16 || VT == MVT::v8i32))) &&
      isHorizontalBinOp(Op0, Op1, true))
    return DAG.getNode(X86ISD::HSUB, SDLoc(N), VT, Op0, Op1);

  return OptimizeConditionalInDecrement(N, DAG);
}

/// performVZEXTCombine - Performs build vector combines
static SDValue performVZEXTCombine(SDNode *N, SelectionDAG &DAG,
                                        TargetLowering::DAGCombinerInfo &DCI,
                                        const X86Subtarget *Subtarget) {
  // (vzext (bitcast (vzext (x)) -> (vzext x)
  SDValue In = N->getOperand(0);
  while (In.getOpcode() == ISD::BITCAST)
    In = In.getOperand(0);

  if (In.getOpcode() != X86ISD::VZEXT)
    return SDValue();

  return DAG.getNode(X86ISD::VZEXT, SDLoc(N), N->getValueType(0),
                     In.getOperand(0));
}

SDValue X86TargetLowering::PerformDAGCombine(SDNode *N,
                                             DAGCombinerInfo &DCI) const {
  SelectionDAG &DAG = DCI.DAG;
  switch (N->getOpcode()) {
  default: break;
  case ISD::EXTRACT_VECTOR_ELT:
    return PerformEXTRACT_VECTOR_ELTCombine(N, DAG, DCI);
  case ISD::VSELECT:
  case ISD::SELECT:         return PerformSELECTCombine(N, DAG, DCI, Subtarget);
  case X86ISD::CMOV:        return PerformCMOVCombine(N, DAG, DCI, Subtarget);
  case ISD::ADD:            return PerformAddCombine(N, DAG, Subtarget);
  case ISD::SUB:            return PerformSubCombine(N, DAG, Subtarget);
  case X86ISD::ADC:         return PerformADCCombine(N, DAG, DCI);
  case ISD::MUL:            return PerformMulCombine(N, DAG, DCI);
  case ISD::SHL:
  case ISD::SRA:
  case ISD::SRL:            return PerformShiftCombine(N, DAG, DCI, Subtarget);
  case ISD::AND:            return PerformAndCombine(N, DAG, DCI, Subtarget);
  case ISD::OR:             return PerformOrCombine(N, DAG, DCI, Subtarget);
  case ISD::XOR:            return PerformXorCombine(N, DAG, DCI, Subtarget);
  case ISD::LOAD:           return PerformLOADCombine(N, DAG, DCI, Subtarget);
  case ISD::STORE:          return PerformSTORECombine(N, DAG, Subtarget);
  case ISD::SINT_TO_FP:     return PerformSINT_TO_FPCombine(N, DAG, this);
  case ISD::FADD:           return PerformFADDCombine(N, DAG, Subtarget);
  case ISD::FSUB:           return PerformFSUBCombine(N, DAG, Subtarget);
  case X86ISD::FXOR:
  case X86ISD::FOR:         return PerformFORCombine(N, DAG);
  case X86ISD::FMIN:
  case X86ISD::FMAX:        return PerformFMinFMaxCombine(N, DAG);
  case X86ISD::FAND:        return PerformFANDCombine(N, DAG);
  case X86ISD::FANDN:       return PerformFANDNCombine(N, DAG);
  case X86ISD::BT:          return PerformBTCombine(N, DAG, DCI);
  case X86ISD::VZEXT_MOVL:  return PerformVZEXT_MOVLCombine(N, DAG);
  case ISD::ANY_EXTEND:
  case ISD::ZERO_EXTEND:    return PerformZExtCombine(N, DAG, DCI, Subtarget);
  case ISD::SIGN_EXTEND:    return PerformSExtCombine(N, DAG, DCI, Subtarget);
  case ISD::SIGN_EXTEND_INREG:
    return PerformSIGN_EXTEND_INREGCombine(N, DAG, Subtarget);
  case ISD::TRUNCATE:       return PerformTruncateCombine(N, DAG,DCI,Subtarget);
  case ISD::SETCC:          return PerformISDSETCCCombine(N, DAG, Subtarget);
  case X86ISD::SETCC:       return PerformSETCCCombine(N, DAG, DCI, Subtarget);
  case X86ISD::BRCOND:      return PerformBrCondCombine(N, DAG, DCI, Subtarget);
  case X86ISD::VZEXT:       return performVZEXTCombine(N, DAG, DCI, Subtarget);
  case X86ISD::SHUFP:       // Handle all target specific shuffles
  case X86ISD::PALIGNR:
  case X86ISD::UNPCKH:
  case X86ISD::UNPCKL:
  case X86ISD::MOVHLPS:
  case X86ISD::MOVLHPS:
  case X86ISD::PSHUFB:
  case X86ISD::PSHUFD:
  case X86ISD::PSHUFHW:
  case X86ISD::PSHUFLW:
  case X86ISD::MOVSS:
  case X86ISD::MOVSD:
  case X86ISD::VPERMILP:
  case X86ISD::VPERM2X128:
  case ISD::VECTOR_SHUFFLE: return PerformShuffleCombine(N, DAG, DCI,Subtarget);
  case ISD::FMA:            return PerformFMACombine(N, DAG, Subtarget);
  case ISD::INTRINSIC_WO_CHAIN:
    return PerformINTRINSIC_WO_CHAINCombine(N, DAG, Subtarget);
  case X86ISD::INSERTPS:
    return PerformINSERTPSCombine(N, DAG, Subtarget);
  case ISD::BUILD_VECTOR: return PerformBUILD_VECTORCombine(N, DAG, Subtarget);
  }

  return SDValue();
}

/// isTypeDesirableForOp - Return true if the target has native support for
/// the specified value type and it is 'desirable' to use the type for the
/// given node type. e.g. On x86 i16 is legal, but undesirable since i16
/// instruction encodings are longer and some i16 instructions are slow.
bool X86TargetLowering::isTypeDesirableForOp(unsigned Opc, EVT VT) const {
  if (!isTypeLegal(VT))
    return false;
  if (VT != MVT::i16)
    return true;

  switch (Opc) {
  default:
    return true;
  case ISD::LOAD:
  case ISD::SIGN_EXTEND:
  case ISD::ZERO_EXTEND:
  case ISD::ANY_EXTEND:
  case ISD::SHL:
  case ISD::SRL:
  case ISD::SUB:
  case ISD::ADD:
  case ISD::MUL:
  case ISD::AND:
  case ISD::OR:
  case ISD::XOR:
    return false;
  }
}

/// IsDesirableToPromoteOp - This method query the target whether it is
/// beneficial for dag combiner to promote the specified node. If true, it
/// should return the desired promotion type by reference.
bool X86TargetLowering::IsDesirableToPromoteOp(SDValue Op, EVT &PVT) const {
  EVT VT = Op.getValueType();
  if (VT != MVT::i16)
    return false;

  bool Promote = false;
  bool Commute = false;
  switch (Op.getOpcode()) {
  default: break;
  case ISD::LOAD: {
    LoadSDNode *LD = cast<LoadSDNode>(Op);
    // If the non-extending load has a single use and it's not live out, then it
    // might be folded.
    if (LD->getExtensionType() == ISD::NON_EXTLOAD /*&&
                                                     Op.hasOneUse()*/) {
      for (SDNode::use_iterator UI = Op.getNode()->use_begin(),
             UE = Op.getNode()->use_end(); UI != UE; ++UI) {
        // The only case where we'd want to promote LOAD (rather then it being
        // promoted as an operand is when it's only use is liveout.
        if (UI->getOpcode() != ISD::CopyToReg)
          return false;
      }
    }
    Promote = true;
    break;
  }
  case ISD::SIGN_EXTEND:
  case ISD::ZERO_EXTEND:
  case ISD::ANY_EXTEND:
    Promote = true;
    break;
  case ISD::SHL:
  case ISD::SRL: {
    SDValue N0 = Op.getOperand(0);
    // Look out for (store (shl (load), x)).
    if (MayFoldLoad(N0) && MayFoldIntoStore(Op))
      return false;
    Promote = true;
    break;
  }
  case ISD::ADD:
  case ISD::MUL:
  case ISD::AND:
  case ISD::OR:
  case ISD::XOR:
    Commute = true;
    // fallthrough
  case ISD::SUB: {
    SDValue N0 = Op.getOperand(0);
    SDValue N1 = Op.getOperand(1);
    if (!Commute && MayFoldLoad(N1))
      return false;
    // Avoid disabling potential load folding opportunities.
    if (MayFoldLoad(N0) && (!isa<ConstantSDNode>(N1) || MayFoldIntoStore(Op)))
      return false;
    if (MayFoldLoad(N1) && (!isa<ConstantSDNode>(N0) || MayFoldIntoStore(Op)))
      return false;
    Promote = true;
  }
  }

  PVT = MVT::i32;
  return Promote;
}

//===----------------------------------------------------------------------===//
//                           X86 Inline Assembly Support
//===----------------------------------------------------------------------===//

namespace {
  // Helper to match a string separated by whitespace.
  bool matchAsmImpl(StringRef s, ArrayRef<const StringRef *> args) {
    s = s.substr(s.find_first_not_of(" \t")); // Skip leading whitespace.

    for (unsigned i = 0, e = args.size(); i != e; ++i) {
      StringRef piece(*args[i]);
      if (!s.startswith(piece)) // Check if the piece matches.
        return false;

      s = s.substr(piece.size());
      StringRef::size_type pos = s.find_first_not_of(" \t");
      if (pos == 0) // We matched a prefix.
        return false;

      s = s.substr(pos);
    }

    return s.empty();
  }
  const VariadicFunction1<bool, StringRef, StringRef, matchAsmImpl> matchAsm={};
}

static bool clobbersFlagRegisters(const SmallVector<StringRef, 4> &AsmPieces) {

  if (AsmPieces.size() == 3 || AsmPieces.size() == 4) {
    if (std::count(AsmPieces.begin(), AsmPieces.end(), "~{cc}") &&
        std::count(AsmPieces.begin(), AsmPieces.end(), "~{flags}") &&
        std::count(AsmPieces.begin(), AsmPieces.end(), "~{fpsr}")) {

      if (AsmPieces.size() == 3)
        return true;
      else if (std::count(AsmPieces.begin(), AsmPieces.end(), "~{dirflag}"))
        return true;
    }
  }
  return false;
}

bool X86TargetLowering::ExpandInlineAsm(CallInst *CI) const {
  InlineAsm *IA = cast<InlineAsm>(CI->getCalledValue());

  std::string AsmStr = IA->getAsmString();

  IntegerType *Ty = dyn_cast<IntegerType>(CI->getType());
  if (!Ty || Ty->getBitWidth() % 16 != 0)
    return false;

  // TODO: should remove alternatives from the asmstring: "foo {a|b}" -> "foo a"
  SmallVector<StringRef, 4> AsmPieces;
  SplitString(AsmStr, AsmPieces, ";\n");

  switch (AsmPieces.size()) {
  default: return false;
  case 1:
    // FIXME: this should verify that we are targeting a 486 or better.  If not,
    // we will turn this bswap into something that will be lowered to logical
    // ops instead of emitting the bswap asm.  For now, we don't support 486 or
    // lower so don't worry about this.
    // bswap $0
    if (matchAsm(AsmPieces[0], "bswap", "$0") ||
        matchAsm(AsmPieces[0], "bswapl", "$0") ||
        matchAsm(AsmPieces[0], "bswapq", "$0") ||
        matchAsm(AsmPieces[0], "bswap", "${0:q}") ||
        matchAsm(AsmPieces[0], "bswapl", "${0:q}") ||
        matchAsm(AsmPieces[0], "bswapq", "${0:q}")) {
      // No need to check constraints, nothing other than the equivalent of
      // "=r,0" would be valid here.
      return IntrinsicLowering::LowerToByteSwap(CI);
    }

    // rorw $$8, ${0:w}  -->  llvm.bswap.i16
    if (CI->getType()->isIntegerTy(16) &&
        IA->getConstraintString().compare(0, 5, "=r,0,") == 0 &&
        (matchAsm(AsmPieces[0], "rorw", "$$8,", "${0:w}") ||
         matchAsm(AsmPieces[0], "rolw", "$$8,", "${0:w}"))) {
      AsmPieces.clear();
      const std::string &ConstraintsStr = IA->getConstraintString();
      SplitString(StringRef(ConstraintsStr).substr(5), AsmPieces, ",");
      array_pod_sort(AsmPieces.begin(), AsmPieces.end());
      if (clobbersFlagRegisters(AsmPieces))
        return IntrinsicLowering::LowerToByteSwap(CI);
    }
    break;
  case 3:
    if (CI->getType()->isIntegerTy(32) &&
        IA->getConstraintString().compare(0, 5, "=r,0,") == 0 &&
        matchAsm(AsmPieces[0], "rorw", "$$8,", "${0:w}") &&
        matchAsm(AsmPieces[1], "rorl", "$$16,", "$0") &&
        matchAsm(AsmPieces[2], "rorw", "$$8,", "${0:w}")) {
      AsmPieces.clear();
      const std::string &ConstraintsStr = IA->getConstraintString();
      SplitString(StringRef(ConstraintsStr).substr(5), AsmPieces, ",");
      array_pod_sort(AsmPieces.begin(), AsmPieces.end());
      if (clobbersFlagRegisters(AsmPieces))
        return IntrinsicLowering::LowerToByteSwap(CI);
    }

    if (CI->getType()->isIntegerTy(64)) {
      InlineAsm::ConstraintInfoVector Constraints = IA->ParseConstraints();
      if (Constraints.size() >= 2 &&
          Constraints[0].Codes.size() == 1 && Constraints[0].Codes[0] == "A" &&
          Constraints[1].Codes.size() == 1 && Constraints[1].Codes[0] == "0") {
        // bswap %eax / bswap %edx / xchgl %eax, %edx  -> llvm.bswap.i64
        if (matchAsm(AsmPieces[0], "bswap", "%eax") &&
            matchAsm(AsmPieces[1], "bswap", "%edx") &&
            matchAsm(AsmPieces[2], "xchgl", "%eax,", "%edx"))
          return IntrinsicLowering::LowerToByteSwap(CI);
      }
    }
    break;
  }
  return false;
}

/// getConstraintType - Given a constraint letter, return the type of
/// constraint it is for this target.
X86TargetLowering::ConstraintType
X86TargetLowering::getConstraintType(const std::string &Constraint) const {
  if (Constraint.size() == 1) {
    switch (Constraint[0]) {
    case 'R':
    case 'q':
    case 'Q':
    case 'f':
    case 't':
    case 'u':
    case 'y':
    case 'x':
    case 'Y':
    case 'l':
      return C_RegisterClass;
    case 'a':
    case 'b':
    case 'c':
    case 'd':
    case 'S':
    case 'D':
    case 'A':
      return C_Register;
    case 'I':
    case 'J':
    case 'K':
    case 'L':
    case 'M':
    case 'N':
    case 'G':
    case 'C':
    case 'e':
    case 'Z':
      return C_Other;
    default:
      break;
    }
  }
  return TargetLowering::getConstraintType(Constraint);
}

/// Examine constraint type and operand type and determine a weight value.
/// This object must already have been set up with the operand type
/// and the current alternative constraint selected.
TargetLowering::ConstraintWeight
  X86TargetLowering::getSingleConstraintMatchWeight(
    AsmOperandInfo &info, const char *constraint) const {
  ConstraintWeight weight = CW_Invalid;
  Value *CallOperandVal = info.CallOperandVal;
    // If we don't have a value, we can't do a match,
    // but allow it at the lowest weight.
  if (!CallOperandVal)
    return CW_Default;
  Type *type = CallOperandVal->getType();
  // Look at the constraint type.
  switch (*constraint) {
  default:
    weight = TargetLowering::getSingleConstraintMatchWeight(info, constraint);
  case 'R':
  case 'q':
  case 'Q':
  case 'a':
  case 'b':
  case 'c':
  case 'd':
  case 'S':
  case 'D':
  case 'A':
    if (CallOperandVal->getType()->isIntegerTy())
      weight = CW_SpecificReg;
    break;
  case 'f':
  case 't':
  case 'u':
    if (type->isFloatingPointTy())
      weight = CW_SpecificReg;
    break;
  case 'y':
    if (type->isX86_MMXTy() && Subtarget->hasMMX())
      weight = CW_SpecificReg;
    break;
  case 'x':
  case 'Y':
    if (((type->getPrimitiveSizeInBits() == 128) && Subtarget->hasSSE1()) ||
        ((type->getPrimitiveSizeInBits() == 256) && Subtarget->hasFp256()))
      weight = CW_Register;
    break;
  case 'I':
    if (ConstantInt *C = dyn_cast<ConstantInt>(info.CallOperandVal)) {
      if (C->getZExtValue() <= 31)
        weight = CW_Constant;
    }
    break;
  case 'J':
    if (ConstantInt *C = dyn_cast<ConstantInt>(CallOperandVal)) {
      if (C->getZExtValue() <= 63)
        weight = CW_Constant;
    }
    break;
  case 'K':
    if (ConstantInt *C = dyn_cast<ConstantInt>(CallOperandVal)) {
      if ((C->getSExtValue() >= -0x80) && (C->getSExtValue() <= 0x7f))
        weight = CW_Constant;
    }
    break;
  case 'L':
    if (ConstantInt *C = dyn_cast<ConstantInt>(CallOperandVal)) {
      if ((C->getZExtValue() == 0xff) || (C->getZExtValue() == 0xffff))
        weight = CW_Constant;
    }
    break;
  case 'M':
    if (ConstantInt *C = dyn_cast<ConstantInt>(CallOperandVal)) {
      if (C->getZExtValue() <= 3)
        weight = CW_Constant;
    }
    break;
  case 'N':
    if (ConstantInt *C = dyn_cast<ConstantInt>(CallOperandVal)) {
      if (C->getZExtValue() <= 0xff)
        weight = CW_Constant;
    }
    break;
  case 'G':
  case 'C':
    if (dyn_cast<ConstantFP>(CallOperandVal)) {
      weight = CW_Constant;
    }
    break;
  case 'e':
    if (ConstantInt *C = dyn_cast<ConstantInt>(CallOperandVal)) {
      if ((C->getSExtValue() >= -0x80000000LL) &&
          (C->getSExtValue() <= 0x7fffffffLL))
        weight = CW_Constant;
    }
    break;
  case 'Z':
    if (ConstantInt *C = dyn_cast<ConstantInt>(CallOperandVal)) {
      if (C->getZExtValue() <= 0xffffffff)
        weight = CW_Constant;
    }
    break;
  }
  return weight;
}

/// LowerXConstraint - try to replace an X constraint, which matches anything,
/// with another that has more specific requirements based on the type of the
/// corresponding operand.
const char *X86TargetLowering::
LowerXConstraint(EVT ConstraintVT) const {
  // FP X constraints get lowered to SSE1/2 registers if available, otherwise
  // 'f' like normal targets.
  if (ConstraintVT.isFloatingPoint()) {
    if (Subtarget->hasSSE2())
      return "Y";
    if (Subtarget->hasSSE1())
      return "x";
  }

  return TargetLowering::LowerXConstraint(ConstraintVT);
}

/// LowerAsmOperandForConstraint - Lower the specified operand into the Ops
/// vector.  If it is invalid, don't add anything to Ops.
void X86TargetLowering::LowerAsmOperandForConstraint(SDValue Op,
                                                     std::string &Constraint,
                                                     std::vector<SDValue>&Ops,
                                                     SelectionDAG &DAG) const {
  SDValue Result;

  // Only support length 1 constraints for now.
  if (Constraint.length() > 1) return;

  char ConstraintLetter = Constraint[0];
  switch (ConstraintLetter) {
  default: break;
  case 'I':
    if (ConstantSDNode *C = dyn_cast<ConstantSDNode>(Op)) {
      if (C->getZExtValue() <= 31) {
        Result = DAG.getTargetConstant(C->getZExtValue(), Op.getValueType());
        break;
      }
    }
    return;
  case 'J':
    if (ConstantSDNode *C = dyn_cast<ConstantSDNode>(Op)) {
      if (C->getZExtValue() <= 63) {
        Result = DAG.getTargetConstant(C->getZExtValue(), Op.getValueType());
        break;
      }
    }
    return;
  case 'K':
    if (ConstantSDNode *C = dyn_cast<ConstantSDNode>(Op)) {
      if (isInt<8>(C->getSExtValue())) {
        Result = DAG.getTargetConstant(C->getZExtValue(), Op.getValueType());
        break;
      }
    }
    return;
  case 'N':
    if (ConstantSDNode *C = dyn_cast<ConstantSDNode>(Op)) {
      if (C->getZExtValue() <= 255) {
        Result = DAG.getTargetConstant(C->getZExtValue(), Op.getValueType());
        break;
      }
    }
    return;
  case 'e': {
    // 32-bit signed value
    if (ConstantSDNode *C = dyn_cast<ConstantSDNode>(Op)) {
      if (ConstantInt::isValueValidForType(Type::getInt32Ty(*DAG.getContext()),
                                           C->getSExtValue())) {
        // Widen to 64 bits here to get it sign extended.
        Result = DAG.getTargetConstant(C->getSExtValue(), MVT::i64);
        break;
      }
    // FIXME gcc accepts some relocatable values here too, but only in certain
    // memory models; it's complicated.
    }
    return;
  }
  case 'Z': {
    // 32-bit unsigned value
    if (ConstantSDNode *C = dyn_cast<ConstantSDNode>(Op)) {
      if (ConstantInt::isValueValidForType(Type::getInt32Ty(*DAG.getContext()),
                                           C->getZExtValue())) {
        Result = DAG.getTargetConstant(C->getZExtValue(), Op.getValueType());
        break;
      }
    }
    // FIXME gcc accepts some relocatable values here too, but only in certain
    // memory models; it's complicated.
    return;
  }
  case 'i': {
    // Literal immediates are always ok.
    if (ConstantSDNode *CST = dyn_cast<ConstantSDNode>(Op)) {
      // Widen to 64 bits here to get it sign extended.
      Result = DAG.getTargetConstant(CST->getSExtValue(), MVT::i64);
      break;
    }

    // In any sort of PIC mode addresses need to be computed at runtime by
    // adding in a register or some sort of table lookup.  These can't
    // be used as immediates.
    if (Subtarget->isPICStyleGOT() || Subtarget->isPICStyleStubPIC())
      return;

    // If we are in non-pic codegen mode, we allow the address of a global (with
    // an optional displacement) to be used with 'i'.
    GlobalAddressSDNode *GA = nullptr;
    int64_t Offset = 0;

    // Match either (GA), (GA+C), (GA+C1+C2), etc.
    while (1) {
      if ((GA = dyn_cast<GlobalAddressSDNode>(Op))) {
        Offset += GA->getOffset();
        break;
      } else if (Op.getOpcode() == ISD::ADD) {
        if (ConstantSDNode *C = dyn_cast<ConstantSDNode>(Op.getOperand(1))) {
          Offset += C->getZExtValue();
          Op = Op.getOperand(0);
          continue;
        }
      } else if (Op.getOpcode() == ISD::SUB) {
        if (ConstantSDNode *C = dyn_cast<ConstantSDNode>(Op.getOperand(1))) {
          Offset += -C->getZExtValue();
          Op = Op.getOperand(0);
          continue;
        }
      }

      // Otherwise, this isn't something we can handle, reject it.
      return;
    }

    const GlobalValue *GV = GA->getGlobal();
    // If we require an extra load to get this address, as in PIC mode, we
    // can't accept it.
    if (isGlobalStubReference(
            Subtarget->ClassifyGlobalReference(GV, DAG.getTarget())))
      return;

    Result = DAG.getTargetGlobalAddress(GV, SDLoc(Op),
                                        GA->getValueType(0), Offset);
    break;
  }
  }

  if (Result.getNode()) {
    Ops.push_back(Result);
    return;
  }
  return TargetLowering::LowerAsmOperandForConstraint(Op, Constraint, Ops, DAG);
}

std::pair<unsigned, const TargetRegisterClass*>
X86TargetLowering::getRegForInlineAsmConstraint(const std::string &Constraint,
                                                MVT VT) const {
  // First, see if this is a constraint that directly corresponds to an LLVM
  // register class.
  if (Constraint.size() == 1) {
    // GCC Constraint Letters
    switch (Constraint[0]) {
    default: break;
      // TODO: Slight differences here in allocation order and leaving
      // RIP in the class. Do they matter any more here than they do
      // in the normal allocation?
    case 'q':   // GENERAL_REGS in 64-bit mode, Q_REGS in 32-bit mode.
      if (Subtarget->is64Bit()) {
        if (VT == MVT::i32 || VT == MVT::f32)
          return std::make_pair(0U, &X86::GR32RegClass);
        if (VT == MVT::i16)
          return std::make_pair(0U, &X86::GR16RegClass);
        if (VT == MVT::i8 || VT == MVT::i1)
          return std::make_pair(0U, &X86::GR8RegClass);
        if (VT == MVT::i64 || VT == MVT::f64)
          return std::make_pair(0U, &X86::GR64RegClass);
        break;
      }
      // 32-bit fallthrough
    case 'Q':   // Q_REGS
      if (VT == MVT::i32 || VT == MVT::f32)
        return std::make_pair(0U, &X86::GR32_ABCDRegClass);
      if (VT == MVT::i16)
        return std::make_pair(0U, &X86::GR16_ABCDRegClass);
      if (VT == MVT::i8 || VT == MVT::i1)
        return std::make_pair(0U, &X86::GR8_ABCD_LRegClass);
      if (VT == MVT::i64)
        return std::make_pair(0U, &X86::GR64_ABCDRegClass);
      break;
    case 'r':   // GENERAL_REGS
    case 'l':   // INDEX_REGS
      if (VT == MVT::i8 || VT == MVT::i1)
        return std::make_pair(0U, &X86::GR8RegClass);
      if (VT == MVT::i16)
        return std::make_pair(0U, &X86::GR16RegClass);
      if (VT == MVT::i32 || VT == MVT::f32 || !Subtarget->is64Bit())
        return std::make_pair(0U, &X86::GR32RegClass);
      return std::make_pair(0U, &X86::GR64RegClass);
    case 'R':   // LEGACY_REGS
      if (VT == MVT::i8 || VT == MVT::i1)
        return std::make_pair(0U, &X86::GR8_NOREXRegClass);
      if (VT == MVT::i16)
        return std::make_pair(0U, &X86::GR16_NOREXRegClass);
      if (VT == MVT::i32 || !Subtarget->is64Bit())
        return std::make_pair(0U, &X86::GR32_NOREXRegClass);
      return std::make_pair(0U, &X86::GR64_NOREXRegClass);
    case 'f':  // FP Stack registers.
      // If SSE is enabled for this VT, use f80 to ensure the isel moves the
      // value to the correct fpstack register class.
      if (VT == MVT::f32 && !isScalarFPTypeInSSEReg(VT))
        return std::make_pair(0U, &X86::RFP32RegClass);
      if (VT == MVT::f64 && !isScalarFPTypeInSSEReg(VT))
        return std::make_pair(0U, &X86::RFP64RegClass);
      return std::make_pair(0U, &X86::RFP80RegClass);
    case 'y':   // MMX_REGS if MMX allowed.
      if (!Subtarget->hasMMX()) break;
      return std::make_pair(0U, &X86::VR64RegClass);
    case 'Y':   // SSE_REGS if SSE2 allowed
      if (!Subtarget->hasSSE2()) break;
      // FALL THROUGH.
    case 'x':   // SSE_REGS if SSE1 allowed or AVX_REGS if AVX allowed
      if (!Subtarget->hasSSE1()) break;

      switch (VT.SimpleTy) {
      default: break;
      // Scalar SSE types.
      case MVT::f32:
      case MVT::i32:
        return std::make_pair(0U, &X86::FR32RegClass);
      case MVT::f64:
      case MVT::i64:
        return std::make_pair(0U, &X86::FR64RegClass);
      // Vector types.
      case MVT::v16i8:
      case MVT::v8i16:
      case MVT::v4i32:
      case MVT::v2i64:
      case MVT::v4f32:
      case MVT::v2f64:
        return std::make_pair(0U, &X86::VR128RegClass);
      // AVX types.
      case MVT::v32i8:
      case MVT::v16i16:
      case MVT::v8i32:
      case MVT::v4i64:
      case MVT::v8f32:
      case MVT::v4f64:
        return std::make_pair(0U, &X86::VR256RegClass);
      case MVT::v8f64:
      case MVT::v16f32:
      case MVT::v16i32:
      case MVT::v8i64:
        return std::make_pair(0U, &X86::VR512RegClass);
      }
      break;
    }
  }

  // Use the default implementation in TargetLowering to convert the register
  // constraint into a member of a register class.
  std::pair<unsigned, const TargetRegisterClass*> Res;
  Res = TargetLowering::getRegForInlineAsmConstraint(Constraint, VT);

  // Not found as a standard register?
  if (!Res.second) {
    // Map st(0) -> st(7) -> ST0
    if (Constraint.size() == 7 && Constraint[0] == '{' &&
        tolower(Constraint[1]) == 's' &&
        tolower(Constraint[2]) == 't' &&
        Constraint[3] == '(' &&
        (Constraint[4] >= '0' && Constraint[4] <= '7') &&
        Constraint[5] == ')' &&
        Constraint[6] == '}') {

      Res.first = X86::FP0+Constraint[4]-'0';
      Res.second = &X86::RFP80RegClass;
      return Res;
    }

    // GCC allows "st(0)" to be called just plain "st".
    if (StringRef("{st}").equals_lower(Constraint)) {
      Res.first = X86::FP0;
      Res.second = &X86::RFP80RegClass;
      return Res;
    }

    // flags -> EFLAGS
    if (StringRef("{flags}").equals_lower(Constraint)) {
      Res.first = X86::EFLAGS;
      Res.second = &X86::CCRRegClass;
      return Res;
    }

    // 'A' means EAX + EDX.
    if (Constraint == "A") {
      Res.first = X86::EAX;
      Res.second = &X86::GR32_ADRegClass;
      return Res;
    }
    return Res;
  }

  // Otherwise, check to see if this is a register class of the wrong value
  // type.  For example, we want to map "{ax},i32" -> {eax}, we don't want it to
  // turn into {ax},{dx}.
  if (Res.second->hasType(VT))
    return Res;   // Correct type already, nothing to do.

  // All of the single-register GCC register classes map their values onto
  // 16-bit register pieces "ax","dx","cx","bx","si","di","bp","sp".  If we
  // really want an 8-bit or 32-bit register, map to the appropriate register
  // class and return the appropriate register.
  if (Res.second == &X86::GR16RegClass) {
    if (VT == MVT::i8 || VT == MVT::i1) {
      unsigned DestReg = 0;
      switch (Res.first) {
      default: break;
      case X86::AX: DestReg = X86::AL; break;
      case X86::DX: DestReg = X86::DL; break;
      case X86::CX: DestReg = X86::CL; break;
      case X86::BX: DestReg = X86::BL; break;
      }
      if (DestReg) {
        Res.first = DestReg;
        Res.second = &X86::GR8RegClass;
      }
    } else if (VT == MVT::i32 || VT == MVT::f32) {
      unsigned DestReg = 0;
      switch (Res.first) {
      default: break;
      case X86::AX: DestReg = X86::EAX; break;
      case X86::DX: DestReg = X86::EDX; break;
      case X86::CX: DestReg = X86::ECX; break;
      case X86::BX: DestReg = X86::EBX; break;
      case X86::SI: DestReg = X86::ESI; break;
      case X86::DI: DestReg = X86::EDI; break;
      case X86::BP: DestReg = X86::EBP; break;
      case X86::SP: DestReg = X86::ESP; break;
      }
      if (DestReg) {
        Res.first = DestReg;
        Res.second = &X86::GR32RegClass;
      }
    } else if (VT == MVT::i64 || VT == MVT::f64) {
      unsigned DestReg = 0;
      switch (Res.first) {
      default: break;
      case X86::AX: DestReg = X86::RAX; break;
      case X86::DX: DestReg = X86::RDX; break;
      case X86::CX: DestReg = X86::RCX; break;
      case X86::BX: DestReg = X86::RBX; break;
      case X86::SI: DestReg = X86::RSI; break;
      case X86::DI: DestReg = X86::RDI; break;
      case X86::BP: DestReg = X86::RBP; break;
      case X86::SP: DestReg = X86::RSP; break;
      }
      if (DestReg) {
        Res.first = DestReg;
        Res.second = &X86::GR64RegClass;
      }
    }
  } else if (Res.second == &X86::FR32RegClass ||
             Res.second == &X86::FR64RegClass ||
             Res.second == &X86::VR128RegClass ||
             Res.second == &X86::VR256RegClass ||
             Res.second == &X86::FR32XRegClass ||
             Res.second == &X86::FR64XRegClass ||
             Res.second == &X86::VR128XRegClass ||
             Res.second == &X86::VR256XRegClass ||
             Res.second == &X86::VR512RegClass) {
    // Handle references to XMM physical registers that got mapped into the
    // wrong class.  This can happen with constraints like {xmm0} where the
    // target independent register mapper will just pick the first match it can
    // find, ignoring the required type.

    if (VT == MVT::f32 || VT == MVT::i32)
      Res.second = &X86::FR32RegClass;
    else if (VT == MVT::f64 || VT == MVT::i64)
      Res.second = &X86::FR64RegClass;
    else if (X86::VR128RegClass.hasType(VT))
      Res.second = &X86::VR128RegClass;
    else if (X86::VR256RegClass.hasType(VT))
      Res.second = &X86::VR256RegClass;
    else if (X86::VR512RegClass.hasType(VT))
      Res.second = &X86::VR512RegClass;
  }

  return Res;
}

int X86TargetLowering::getScalingFactorCost(const AddrMode &AM,
                                            Type *Ty) const {
  // Scaling factors are not free at all.
  // An indexed folded instruction, i.e., inst (reg1, reg2, scale),
  // will take 2 allocations in the out of order engine instead of 1
  // for plain addressing mode, i.e. inst (reg1).
  // E.g.,
  // vaddps (%rsi,%drx), %ymm0, %ymm1
  // Requires two allocations (one for the load, one for the computation)
  // whereas:
  // vaddps (%rsi), %ymm0, %ymm1
  // Requires just 1 allocation, i.e., freeing allocations for other operations
  // and having less micro operations to execute.
  //
  // For some X86 architectures, this is even worse because for instance for
  // stores, the complex addressing mode forces the instruction to use the
  // "load" ports instead of the dedicated "store" port.
  // E.g., on Haswell:
  // vmovaps %ymm1, (%r8, %rdi) can use port 2 or 3.
  // vmovaps %ymm1, (%r8) can use port 2, 3, or 7.   
  if (isLegalAddressingMode(AM, Ty))
    // Scale represents reg2 * scale, thus account for 1
    // as soon as we use a second register.
    return AM.Scale != 0;
  return -1;
}

bool X86TargetLowering::isTargetFTOL() const {
  return Subtarget->isTargetKnownWindowsMSVC() && !Subtarget->is64Bit();
}
