#include "llvm/Pass.h"
#include "llvm/IR/Function.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/Dominators.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Analysis/ScalarEvolutionExpander.h"
#include "llvm/Transforms/Scalar/IndVarSimplify.h"
#include "llvm/Support/CommandLine.h"
#include <iostream>
#include <fstream>
#include <string>
#include <map>

using namespace llvm;

static cl::opt<std::string> OutputFilename("ilpoutput", cl::desc("Output .ilp filename"), cl::value_desc("filename"));

namespace {
  struct A3 : public FunctionPass {
    static char ID;

    A3() : FunctionPass(ID) {
    }

    // getInductionVariable function is from LLVM source code
    // lib/Transforms/Scalar/LoopInterchange.cpp
    PHINode *getInductionVariable(const Loop *L, ScalarEvolution *SE) {
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
            errs() << prefix << "no induction variable for " << (std::string) L->getName() << "\n";
        else
            errs() << prefix << "found induction variable: " << (std::string) ivar->getName() << "\n";
            const SCEV *E = SE->getSCEV(ivar);
            if (E){
              int64_t start_value = 0;
              int64_t end_value = 0;
              if (isa<SCEVNAryExpr>(E)){
                const SCEVNAryExpr *AE = dyn_cast<SCEVNAryExpr>(E);
                const SCEVConstant *CE = dyn_cast<SCEVConstant>(AE->getOperand(0));
                start_value = CE->getValue()->getZExtValue();
              }
              const SCEVConstant *EP = dyn_cast<SCEVConstant>(SE->getSCEVAtScope(E, L->getParentLoop()));
              end_value = EP->getValue()->getZExtValue();
              EP->print(errs());
              errs() << "\r\n";
              errs() << "Start value: " << start_value << "\r\n";
              errs() << "End value: " << end_value << "\r\n";
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

    // recursively follow the dependences and save all getelementptr instructions we encounter
    void getGEPs(Instruction *I, SmallVectorImpl<GetElementPtrInst *> &geps_list, std::set<Instruction *> &seen) {
      if (seen.find(I) != seen.end())
        return;
      seen.insert(I);
      Use *operands = I->getOperandList();
      if (isa<GetElementPtrInst>(I))
        geps_list.push_back(dyn_cast<GetElementPtrInst>(I));

      for (Instruction::op_iterator ii = I->op_begin(); ii != I->op_end(); ii++) {
        Use &U = *ii;
        Instruction *PI = dyn_cast<Instruction>(U.get());
        if (PI)
          getGEPs(PI, geps_list, seen);
      }
    }

    const SCEV *getGEPThirdArg(GetElementPtrInst *I, ScalarEvolution *SE) {
      int n = 0;
      Instruction::op_iterator ii;
      for (ii = I->op_begin(); ii != I->op_end() && n < 2; ii++, n++)
        ;
      if (ii == I->op_end())
        return nullptr;
      llvm::Use &U = *ii;
      return SE->getSCEV(U.get());
    }

    struct Coeff {
      const SCEV *indvar;
      const SCEVConstant *coeff;
    };

    /// taken from lib/Analysis/ScalarEvolutionExpander.cpp:
    /// FactorOutConstant - Test if S is divisible by Factor, using signed
    /// division. If so, update S with Factor divided out and return true.
    /// S need not be evenly divisible if a reasonable remainder can be
    /// computed.
    /// TODO: When ScalarEvolution gets a SCEVSDivExpr, this can be made
    /// unnecessary; in its place, just signed-divide Ops[i] by the scale and
    /// check to see if the divide was folded.
    bool FactorOutConstant(const SCEV *&S, const SCEV *&Remainder,
                                  const SCEV *Factor, ScalarEvolution &SE) {
      // Everything is divisible by one.
      if (Factor->isOne())
        return true;

      // x/x == 1.
      if (S == Factor) {
        S = SE.getConstant(S->getType(), 1);
        return true;
      }

      // For a Constant, check for a multiple of the given factor.
      if (const SCEVConstant *C = dyn_cast<SCEVConstant>(S)) {
        // 0/x == 0.
        if (C->isZero())
          return true;
        // Check for divisibility.
        if (const SCEVConstant *FC = dyn_cast<SCEVConstant>(Factor)) {
          ConstantInt *CI =
              ConstantInt::get(SE.getContext(), C->getAPInt().sdiv(FC->getAPInt()));
          // If the quotient is zero and the remainder is non-zero, reject
          // the value at this scale. It will be considered for subsequent
          // smaller scales.
          if (!CI->isZero()) {
            const SCEV *Div = SE.getConstant(CI);
            S = Div;
            Remainder = SE.getAddExpr(
                Remainder, SE.getConstant(C->getAPInt().srem(FC->getAPInt())));
            return true;
          }
        }
      }

      // In a Mul, check if there is a constant operand which is a multiple
      // of the given factor.
      if (const SCEVMulExpr *M = dyn_cast<SCEVMulExpr>(S)) {
        // Size is known, check if there is a constant operand which is a multiple
        // of the given factor. If so, we can factor it.
        const SCEVConstant *FC = cast<SCEVConstant>(Factor);
        if (const SCEVConstant *C = dyn_cast<SCEVConstant>(M->getOperand(0)))
          if (!C->getAPInt().srem(FC->getAPInt())) {
            SmallVector<const SCEV *, 4> NewMulOps(M->op_begin(), M->op_end());
            NewMulOps[0] = SE.getConstant(C->getAPInt().sdiv(FC->getAPInt()));
            S = SE.getMulExpr(NewMulOps);
            return true;
          }
      }

      // In an AddRec, check if both start and step are divisible.
      if (const SCEVAddRecExpr *A = dyn_cast<SCEVAddRecExpr>(S)) {
        const SCEV *Step = A->getStepRecurrence(SE);
        const SCEV *StepRem = SE.getConstant(Step->getType(), 0);
        if (!FactorOutConstant(Step, StepRem, Factor, SE))
          return false;
        if (!StepRem->isZero())
          return false;
        const SCEV *Start = A->getStart();
        if (!FactorOutConstant(Start, Remainder, Factor, SE))
          return false;
        S = SE.getAddRecExpr(Start, Step, A->getLoop(),
                            A->getNoWrapFlags(SCEV::FlagNW));
        return true;
      }

      return false;
    }

    void compareToIndVars(const SCEV *offset, ScalarEvolution *SE, std::list<Coeff> &coeffs, const SCEVConstant *&cnst) {
      while (isa<SCEVAddRecExpr>(offset)) {
        const SCEVAddRecExpr *RE = dyn_cast<SCEVAddRecExpr>(offset);
        // (1) get corresponding induction variable for outermost SCEV
        const SCEVAddRecExpr *IV = dyn_cast<SCEVAddRecExpr>(SE->getSCEV(getInductionVariable(RE->getLoop(), SE)));
        // (2) divide step of outermost SCEV by step of indvar SCEV
        const SCEV *factor = RE->getStepRecurrence(*SE);  // initial value = RE_Step; after, (RE_Step = k * IV_Step) => (factor == k)
        const SCEV *IV_Step = IV->getStepRecurrence(*SE);
        const SCEV *Step_rem = nullptr;

        assert(FactorOutConstant(factor, Step_rem, IV_Step, *SE) && "SE and its corresponding IV should have divisible steps");
        errs() << " factored out ";
        factor->print(errs());
        errs() << "\r\n";

        assert(isa<SCEVConstant>(factor) && "factor is not a constant");
        coeffs.push_back(Coeff { dyn_cast<SCEV>(IV), dyn_cast<SCEVConstant>(factor) });

        // (3) peel off a layer
        offset = RE->getStart();

        // (4) subtract factor*(start of IV) from the offset
        offset = SE->getAddExpr(offset, SE->getNegativeSCEV(SE->getMulExpr(factor, IV->getStart())));
      }
      cnst = dyn_cast<SCEVConstant>(offset);
      errs() << "    constant:";
      if (!cnst)
        errs() << " (none)";
      else
        cnst->print(errs());
      errs() << "\r\n";
    }

    bool runOnFunction(Function &F) override {
      DominatorTree DT(F);
      LoopInfo LI(DT);
      AssumptionCache AC(F);
      TargetLibraryInfoImpl TLII;
      TargetLibraryInfo TLI(TLII);
      ScalarEvolution SE(F, TLI, AC, DT, LI);

      errs() << "In function " << (std::string) F.getName() << "...\n";

      SetVector<Instruction *> loads_stores;

      for (Loop *L : LI){
        printIVs(L, &SE);
        for (Loop::block_iterator j = L->block_begin(); ; j++){
          if (j == L->block_end())
            break;
          BasicBlock *B = *j;
          for (BasicBlock::iterator k = B->begin(); ; k++){
            if (k == B->end())
              break;
            Instruction *I = &*k;
            //errs() << I.getOpcodeName() << "  " << I.getOpcode() << "\r\n";
            if (I->getOpcode() == 33){
              // ArrayRef
              errs() << "New getelementptr\r\n";
              
              llvm::Use *operands = I->getOperandList();
              for(Instruction::op_iterator ii = I->op_begin(); ii != I->op_end(); ii++){
                llvm::Use &U = *ii;
                const SCEV *E = SE.getSCEV(U.get());
                errs() << "  op: ";
                E->print(errs());
                errs() << "\r\n  ";
                printSCEVRec(E);
                //errs() << U.get()->getValueID() << " " << U.get()->getName() << "\r\n";
              }
            }
            else if (I->getOpcode() == 32){
              // Store
              errs() << "Store\r\n";
              llvm::Use *operands = I->getOperandList();
              for(Instruction::op_iterator ii = I->op_begin(); ii != I->op_end(); ii++){
                
                llvm::Use &U = *ii;
                const SCEV *E = SE.getSCEV(U.get());
                E->print(errs());
                errs() << "\r\n";
                printSCEVRec(E);
                //errs() << U.get()->getValueID() << " " << U.get()->getName() << "\r\n";
              }
              loads_stores.insert(I);
              SmallVector<GetElementPtrInst *, 4> geps_list;
              std::set<Instruction *> seen;
              getGEPs(I, geps_list, seen);
              errs() << "store instruction depends on these getelementptr instructions:\r\n";
              for (GetElementPtrInst *inst : geps_list) {
                inst->print(errs());
                errs() << "\r\n";
                errs() << "    with SCEV: ";
                const SCEV *scev = getGEPThirdArg(inst, &SE);
                assert(scev && "no SCEV!");
                scev->print(errs());
                errs() << "\r\n";
                errs() << "    turned into an index function (coefficients):";
                std::list<Coeff> expr_coeffs;
                const SCEVConstant *expr_const = nullptr;
                compareToIndVars(scev, &SE, expr_coeffs, expr_const);
              }
            }
            else if (I->getOpcode() == 31){
              // Load
              errs() << "Load\r\n";
              llvm::Use *operands = I->getOperandList();
              for(Instruction::op_iterator ii = I->op_begin(); ii != I->op_end(); ii++){
                
                llvm::Use &U = *ii;
                const SCEV *E = SE.getSCEV(U.get());
                E->print(errs());
                errs() << "\r\n";
                printSCEVRec(E);
                //errs() << U.get()->getValueID() << " " << U.get()->getName() << "\r\n";
              }
              loads_stores.insert(I);
              SmallVector<GetElementPtrInst *, 4> geps_list;
              std::set<Instruction *> seen;
              getGEPs(I, geps_list, seen);
              errs() << "load instruction depends on these getelementptr instructions:\r\n";
              for (GetElementPtrInst *inst : geps_list) {
                inst->print(errs());
                errs() << "\r\n";
                errs() << "    with SCEV: ";
                const SCEV *scev = getGEPThirdArg(inst, &SE);
                assert(scev && "no SCEV!");
                scev->print(errs());
                errs() << "\r\n";
                errs() << "    turned into an index function (coefficients):";
                std::list<Coeff> expr_coeffs;
                const SCEVConstant *expr_const = nullptr;
                compareToIndVars(scev, &SE, expr_coeffs, expr_const);
                errs() << "\r\n";
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
      std::ofstream Output(OutputFilename.c_str());
      if (Output.good()){
        Output << "Placeholder for ilp";
        Output.close();
      }
      return false;
    }

    void print(llvm::raw_ostream &O, const Module *M) const override {
        O << "this is a test\n";
        errs() << "this is a test\n";
    }

    void getAnalysisUsage(AnalysisUsage &AU) const override {
      AU.addRequired<LoopInfoWrapperPass>();
      AU.addRequired<ScalarEvolutionWrapperPass>();
    }
  };
}

char A3::ID = 0;
static RegisterPass<A3> X("a3", "Induction Variable Pass",
							false /* Only looks at CFG */,
							false /* Analysis Pass */);
