#include "llvm/Pass.h"
#include "llvm/IR/Function.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/Dominators.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Transforms/Scalar/IndVarSimplify.h"
#include <iostream>
#include <string>

using namespace llvm;

namespace {
  struct A3 : public FunctionPass {
    static char ID;
    A3() : FunctionPass(ID) {
    }

    // getInductionVariable function is from LLVM source code
    // lib/Transforms/Scalar/LoopInterchange.cpp
    PHINode *getInductionVariable(Loop *L, ScalarEvolution *SE) {
      PHINode *InnerIndexVar = L->getCanonicalInductionVariable();
      if (InnerIndexVar)
        return InnerIndexVar;
      if (L->getLoopLatch() == nullptr || L->getLoopPredecessor() == nullptr)
        return nullptr;
      for (BasicBlock::iterator I = L->getHeader()->begin(); isa<PHINode>(I); ++I) {
        PHINode *PhiVar = cast<PHINode>(I);
        Type *PhiTy = PhiVar->getType();
        if (!PhiTy->isIntegerTy() && !PhiTy->isFloatingPointTy() &&
            !PhiTy->isPointerTy())
          return nullptr;
        const SCEVAddRecExpr *AddRec =
            dyn_cast<SCEVAddRecExpr>(SE->getSCEV(PhiVar));
        if (!AddRec || !AddRec->isAffine())
          continue;
        const SCEV *Step = AddRec->getStepRecurrence(*SE);
        if (!isa<SCEVConstant>(Step))
          continue;
        // Found the induction variable.
        // FIXME: Handle loops with more than one induction variable. Note that,
        // currently, legality makes sure we have only one induction variable.
        return PhiVar;
      }
      return nullptr;
    }

    void printIVs(Loop *L, ScalarEvolution *SE, std::string prefix = "") {
        PHINode *ivar = getInductionVariable(L, SE);
        if (!ivar)
            std::cerr << prefix << "no induction variable for " << (std::string) L->getName() << "\n";
        else
            std::cerr << prefix << "found induction variable: " << (std::string) ivar->getName() << "\n";
            const SCEV *E = SE->getSCEV(ivar);
            if (E){
              E->print(errs());
              errs() << "\r\n";
            }
        for (Loop *SL : L->getSubLoops())
            printIVs(SL, SE, prefix + " ");
    }

    void printSCEVRec(const SCEV *E) {
        if (isa<SCEVAddRecExpr>(E)) {
            const SCEVAddRecExpr *RE = dyn_cast<SCEVAddRecExpr>(E);
            errs() << "  loop:";
            RE->getLoop()->print(errs());
            errs() << "\r\n";
            errs() << "  loop (start): ";
            if (isa<SCEVAddRecExpr>(RE->getStart())) {
                dyn_cast<SCEVAddRecExpr>(RE->getStart())->getLoop()->print(errs());
            }
            errs() << "\r\n";
        } else
            errs() << "  (no loop for SCEV)\r\n";
    }

    bool runOnFunction(Function &F) override {
      DominatorTree DT(F);
      LoopInfo LI(DT);
      AssumptionCache AC(F);
      TargetLibraryInfoImpl TLII;
      TargetLibraryInfo TLI(TLII);
      ScalarEvolution SE(F, TLI, AC, DT, LI);

      std::cerr << "In function " << (std::string) F.getName() << "...\n";

      for (Loop *L : LI){
        printIVs(L, &SE);
        for (Loop::block_iterator j = L->block_begin(); ; j++){
          if (j == L->block_end())
            break;
          BasicBlock *B = *j;
          for (BasicBlock::iterator k = B->begin(); ; k++){
            if (k == B->end())
              break;
            Instruction &I = *k;
            //errs() << I.getOpcodeName() << "  " << I.getOpcode() << "\r\n";
            if (I.getOpcode() == 33){
              // ArrayRef
              errs() << "New getelementptr\r\n";
              
              llvm::Use *operands = I.getOperandList();
              for(Instruction::op_iterator ii = I.op_begin(); ii != I.op_end(); ii++){
                
                llvm::Use &U = *ii;
                const SCEV *E = SE.getSCEV(U.get());
                errs() << "  op: ";
                E->print(errs());
                errs() << "\r\n  ";
                printSCEVRec(E);
                //errs() << U.get()->getValueID() << " " << U.get()->getName() << "\r\n";
              }
            }
            else if (I.getOpcode() == 32){
              // Store
              errs() << "Store\r\n";
              llvm::Use *operands = I.getOperandList();
              for(Instruction::op_iterator ii = I.op_begin(); ii != I.op_end(); ii++){
                
                llvm::Use &U = *ii;
                const SCEV *E = SE.getSCEV(U.get());
                E->print(errs());
                errs() << "\r\n";
                printSCEVRec(E);
                //errs() << U.get()->getValueID() << " " << U.get()->getName() << "\r\n";
              }
            }
            else if (I.getOpcode() == 31){
              // Load
              errs() << "Load\r\n";
              llvm::Use *operands = I.getOperandList();
              for(Instruction::op_iterator ii = I.op_begin(); ii != I.op_end(); ii++){
                
                llvm::Use &U = *ii;
                const SCEV *E = SE.getSCEV(U.get());
                E->print(errs());
                errs() << "\r\n";
                printSCEVRec(E);
                //errs() << U.get()->getValueID() << " " << U.get()->getName() << "\r\n";
              }
            }
            else{
              // errs() << I.getOpcode() << ": ";
              // I.print(errs());
              // errs() << "\r\n";
            }
          }
        }
      }
      return false;
    }

    void print(llvm::raw_ostream &O, const Module *M) const override {
        O << "this is a test\n";
        std::cerr << "this is a test\n";
    }

    void getAnalysisUsage(AnalysisUsage &AU) const override {
      AU.addRequired<ScalarEvolutionWrapperPass>();
    }
  };
}

char A3::ID = 0;
static RegisterPass<A3> X("a3", "Induction Variable Pass",
							false /* Only looks at CFG */,
							false /* Analysis Pass */);
