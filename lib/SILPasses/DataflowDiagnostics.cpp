//===-- DataflowDiagnostics.cpp - Emits diagnostics based on SIL analysis -===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/SILPasses/Passes.h"
#include "swift/SILPasses/Transforms.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/DiagnosticsSIL.h"
#include "swift/AST/Expr.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILLocation.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILVisitor.h"
using namespace swift;

template<typename...T, typename...U>
static void diagnose(ASTContext &Context, SourceLoc loc, Diag<T...> diag,
              U &&...args) {
  Context.Diags.diagnose(loc,
                         diag, std::forward<U>(args)...);
}

static void diagnoseMissingReturn(const UnreachableInst *UI,
                                  ASTContext &Context) {
  const SILBasicBlock *BB = UI->getParent();
  const SILFunction *F = BB->getParent();
  SILLocation FLoc = F->getLocation();

  Type ResTy;
  AnyFunctionType *T = 0;

  if (auto *FD = FLoc.getAsASTNode<FuncDecl>()) {
    ResTy = FD->getResultType();
    T = FD->getType()->castTo<AnyFunctionType>();
  } else if (auto *CE = FLoc.getAsASTNode<ClosureExpr>()) {
    ResTy = CE->getResultType();
    T = CE->getType()->castTo<AnyFunctionType>();
  } else {
    llvm_unreachable("unhandled case in MissingReturn");
  }

  // No action required if the function returns 'Void' or that the
  // function is marked 'noreturn'.
  if (ResTy->isVoid() || (T && T->isNoReturn()))
    return;

  SILLocation L = UI->getLoc();
  assert(L && ResTy);
  diagnose(Context,
           L.getEndSourceLoc(),
           diag::missing_return, ResTy,
           FLoc.isASTNode<ClosureExpr>() ? 1 : 0);
}

static void diagnoseNonExhaustiveSwitch(const UnreachableInst *UI,
                                        ASTContext &Context) {
  SILLocation L = UI->getLoc();
  assert(L);
  diagnose(Context,
           L.getEndSourceLoc(),
           diag::non_exhaustive_switch);
}

static void diagnoseUnreachable(const SILInstruction *I,
                                ASTContext &Context) {
  if (auto *UI = dyn_cast<UnreachableInst>(I)){
    SILLocation L = UI->getLoc();

    // Invalid location means that the instruction has been generated by SIL
    // passes, such as DCE. FIXME: we might want to just introduce a separate
    // instruction kind, instead of keeping this invarient.
    if (!L.hasASTLocation())
      return;

    // The most common case of getting an unreachable instruction is a
    // missing return statement. In this case, we know that the instruction
    // location will be the enclosing function.

    if (L.isASTNode<AbstractFunctionDecl>() || L.isASTNode<ClosureExpr>()) {
      diagnoseMissingReturn(UI, Context);
      return;
    }

    // A non-exhaustive switch would also produce an unreachable instruction.
    if (L.isASTNode<SwitchStmt>()) {
      diagnoseNonExhaustiveSwitch(UI, Context);
      return;
    }
  }
}

static void diagnoseReturn(const SILInstruction *I, ASTContext &Context) {
  auto *TI = dyn_cast<TermInst>(I);
  if (!TI || !(isa<BranchInst>(TI) || isa<ReturnInst>(TI)))
    return;

  const SILBasicBlock *BB = TI->getParent();
  const SILFunction *F = BB->getParent();
  SILLocation FLoc = F->getLocation();

  if (auto *FD = FLoc.getAsASTNode<FuncDecl>()) {
    if (AnyFunctionType *T = FD->getType()->castTo<AnyFunctionType>()) {

      // Warn if we reach a return inside a noreturn function.
      if (T->isNoReturn()) {
        SILLocation L = TI->getLoc();
        if (L.is<ReturnLocation>())
          diagnose(Context, L.getSourceLoc(), diag::return_from_noreturn);
        if (L.is<ImplicitReturnLocation>())
          diagnose(Context, L.getSourceLoc(), diag::return_from_noreturn);
      }
    }
  }
}

/// \brief Issue diagnostics whenever we see Builtin.static_report(1, ...).
static void diagnoseStaticReports(const SILInstruction *I,
                                  SILModule &M) {

  // Find out if we are dealing with Builtin.staticReport().
  if (const ApplyInst *AI = dyn_cast<ApplyInst>(I)) {
    if (const BuiltinFunctionRefInst *FR =
        dyn_cast<BuiltinFunctionRefInst>(AI->getCallee().getDef())) {
      const BuiltinInfo &B = FR->getBuiltinInfo();
      if (B.ID == BuiltinValueKind::StaticReport) {

        // Report diagnostic if the first argument has been folded to '1'.
        OperandValueArrayRef Args = AI->getArguments();
        IntegerLiteralInst *V = dyn_cast<IntegerLiteralInst>(Args[0]);
        if (!V || V->getValue() != 1)
          return;

        diagnose(M.getASTContext(),
                 I->getLoc().getSourceLoc(),
                 diag::static_report_error);
      }
    }
  }
}

class EmitDFDiagnostics : public SILFunctionTransform {
  virtual ~EmitDFDiagnostics() {}

  StringRef getName() override { return "Emit Dataflow Diagnostics"; }

  /// The entry point to the transformation.
  void run() {
    SILModule &M = getFunction()->getModule();
    for (auto &BB : *getFunction())
      for (auto &I : BB) {
        diagnoseUnreachable(&I, M.getASTContext());
        diagnoseReturn(&I, M.getASTContext());
        diagnoseStaticReports(&I, M);
      }
  }
};

SILTransform *swift::createEmitDFDiagnostics() {
  return new EmitDFDiagnostics();
}
