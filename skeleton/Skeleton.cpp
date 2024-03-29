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
#include "llvm/Support/raw_ostream.h"
#include <iostream>
#include <fstream>
#include <string>
#include <map>
#include <regex>

using namespace llvm;

static cl::opt<std::string> OutputFilename("ilpoutput", cl::desc("Output .ilp filename"), cl::value_desc("filename"), cl::Required);

namespace {
  struct A3 : public FunctionPass {
    static char ID;
    std::ofstream ilp_file;

    A3() : FunctionPass(ID), ilp_file(OutputFilename) {
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

    /// get the range of an induction variable (inclusive lower and upper bound)
    void IVgetRange(const SCEVAddRecExpr *IV, ScalarEvolution *SE, std::string &lower, std::string &upper) {
      const SCEVNAryExpr *AE = dyn_cast<SCEVNAryExpr>(IV);
      raw_string_ostream lsso(lower), usso(upper);

      if (isa<SCEVConstant>(AE->getOperand(0))){
        const SCEVConstant *CE = dyn_cast<SCEVConstant>(AE->getOperand(0));
        lsso << CE->getValue()->getSExtValue();
      } else{
        lower.clear(); // lower bound is Unknown
      }

      const SCEV *EP = SE->getSCEVAtScope(IV, IV->getLoop()->getParentLoop());
      if (isa<SCEVConstant>(EP)){
        const SCEVConstant *EPP = dyn_cast<SCEVConstant>(EP);
        usso << EPP->getValue()->getSExtValue() - 1;
      } else{
        upper.clear(); // upper bound is Unknown
      }
    }

    void printIVs(Loop *L, ScalarEvolution *SE, std::map<const SCEV *, PHINode *> &ivars, std::string prefix = "") {
        PHINode *ivar = getInductionVariable(L, SE);
        if (!ivar)
            errs() << prefix << "no induction variable for " << (std::string) L->getName() << "\n";
        else
            errs() << prefix << "found induction variable: " << (std::string) ivar->getName() << "\n";
          const SCEV *E = SE->getSCEV(ivar);
          if (E){
            ivars[E] = ivar;
            int64_t start_value = 0;
            int64_t end_value = 0;
            std::string start_value_str, end_value_str;
            if (isa<SCEVNAryExpr>(E)){
              const SCEVNAryExpr *AE = dyn_cast<SCEVNAryExpr>(E);
              if (isa<SCEVConstant>(AE->getOperand(0))){
                const SCEVConstant *CE = dyn_cast<SCEVConstant>(AE->getOperand(0));
                start_value = CE->getValue()->getSExtValue();
                start_value_str = std::to_string(start_value);
              } else{
                start_value_str = "u"; // u as an unknown variable. Maybe it should be infinity instead?
              }
              
            }
            // const SCEVConstant *EP = dyn_cast<SCEVConstant>(SE->getSCEVAtScope(E, L->getParentLoop()));
            // end_value = EP->getValue()->getSExtValue();
            const SCEV *EP = SE->getSCEVAtScope(E, L->getParentLoop());
            if (isa<SCEVConstant>(EP)){
              const SCEVConstant *EPP = dyn_cast<SCEVConstant>(EP);
              end_value = EPP->getValue()->getSExtValue();
              end_value_str = std::to_string(end_value);
            } else{
              end_value_str = "u"; // u as an unknown variable. Maybe it should be infinity instead? 
            }
            
            EP->print(errs());
            errs() << "\r\n";
            errs() << "Start value: " << start_value << "\r\n";
            errs() << "End value: " << end_value << "\r\n";
          }
        for (Loop *SL : L->getSubLoops())
            printIVs(SL, SE, ivars, prefix + " ");
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

    bool compareToIndVars(const SCEV *offset, ScalarEvolution *SE, std::list<Coeff> &coeffs, const SCEVConstant *&cnst, 
      std::map<const SCEV *, PHINode *> &ivars_map) {
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

        // not affine; abort
        if (!isa<SCEVConstant>(factor)) {
          coeffs.clear();
          return false;
        }
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


      printIndexFunction(coeffs, cnst, ivars_map);
      return true;
    }

    void printIndexFunction(std::list<Coeff> &coeffs, const SCEVConstant *scnst, std::map<const SCEV *, PHINode *> &ivars_map) {
      bool first = true;
      for (auto pair : coeffs) {
        if (!first)
          errs() << " + ";
        else
          first = false;
        pair.coeff->print(errs());
        errs() << "*" << ivars_map[pair.indvar]->getName();
      }
      if (scnst) {
        if (!first)
          errs() << " + ";
        scnst->print(errs());
      }
      errs() << "\r\n";
    }

    static std::string glpsolName(const std::string s) {
      return std::regex_replace(s, std::regex("[.]"), "_");
    }

    struct Equation {
      std::list<Coeff> coeffs;
      const SCEVConstant *cnst;

      std::string to_string(std::map<const SCEV *, PHINode *> &iv_map, std::string post, std::map<std::string, const SCEV *> &ivars_instanced) const {
        std::string s;
        raw_string_ostream stream(s);

        if (!post.empty())
          post = "_" + post;

        bool first = true;
        for (auto coeff : coeffs) {
          assert(iv_map.find(coeff.indvar) != iv_map.end() && "indvar SCEV doesn't map to an indvar");
          assert(coeff.coeff && "coeff cannot be null");
          std::string iname;   // instance name
          raw_string_ostream iname_stream(iname);

          if (!first)
            stream << " + ";
          else
            first = false;
          coeff.coeff->print(stream);
          iname_stream << iv_map[coeff.indvar]->getName() << post;
          iname_stream.flush();
          stream << "*" << glpsolName(iname);
          ivars_instanced[iname] = coeff.indvar;
        }

        if (cnst) {
          if (!first)
            stream << " + ";
          cnst->print(stream);
        }

        return s;
      }

      std::string to_string(std::map<const SCEV *, PHINode *> &iv_map) const {
        std::map<std::string, const SCEV *> dummy;
        return to_string(iv_map, "", dummy);
      }
    };

    struct ArrayAccess {
      Instruction *I;
      std::list<Equation> idxs;
    };

    bool runOnFunction(Function &F) override {
      LoopInfo &LI = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
      ScalarEvolution &SE = getAnalysis<ScalarEvolutionWrapperPass>().getSE();

      ilp_file << "/* " << (std::string) F.getName() << "() */\n";

      // (1) for each loop L
      // (2) populate SCEV -> IV map
      // (3) for each instruction in the loop
      // (4) if load/store, get base addr and save to ls_map

      for (Loop *L : LI) {
        std::map<const SCEV *, PHINode *> iv_map;
        std::map<const SCEV *, std::list<ArrayAccess>> base_map;

        printIVs(L, &SE, iv_map); // populate iv_map
        for (Loop::block_iterator li = L->block_begin(); li != L->block_end(); ++li) {
          BasicBlock *BB = *li;
          for (BasicBlock::iterator bi = BB->begin(); bi != BB->end(); ++bi) {
            Instruction *I = &*bi;

            if (isa<LoadInst>(I) || isa<StoreInst>(I)) {
              const SCEV *base = nullptr;
              SmallVector<GetElementPtrInst *, 4> geps_list;
              std::set<Instruction *> seen;

              getGEPs(I, geps_list, seen); // get all getelementptr instructions that this instruction depends on
              for (GetElementPtrInst *gep : geps_list){
                if (!isa<GetElementPtrInst>(gep->getOperand(0))){
                  base = SE.getSCEV(gep->getOperand(0));  // TODO: maybe use llvm::Value instead of SCEV here?
                  break;
                }
              }
              if (base_map.find(base) == base_map.end())
                base_map[base] = std::list<ArrayAccess>();
              
              ArrayAccess aa = { .I = I };
              bool isAffine = true;
              for (GetElementPtrInst *gep : geps_list) {
                // get an equation for each getelementptr instruction (index)
                const SCEV *scev = getGEPThirdArg(gep, &SE);
                std::list<Coeff> expr_coeffs;
                const SCEVConstant *expr_const = nullptr;
                if (!(isAffine = compareToIndVars(scev, &SE, expr_coeffs, expr_const, iv_map)))
                  break;
                aa.idxs.push_back(Equation { expr_coeffs, expr_const });
              }

              if (isAffine)
                base_map[base].push_back(aa);
              else
              {
                errs() << "ignoring ";
                I->print(errs());
                errs() << " because not all of its index functions are affine\r\n";
              }
            }
          }
        }

        // the instanced names of the induction variables
        // for example, if the induction variable is "indvar.iv",
        // then a possible instance of it would be "indvar.iv_a"
        std::map<std::string, const SCEV *> ivars_instanced;
        std::string eqns;
        raw_string_ostream eqns_s(eqns);
        size_t neqns = 0;
        for (auto pair : base_map) {
          // for debugging purposes
          pair.first->print(errs());
          errs() << ": \r\n";
          for (auto it = pair.second.begin(); it != pair.second.end(); it++) {
            errs() << "instruction: ";
            it->I->print(errs());
            errs() << "\r\nhas these index functions: ";
            bool first = true;
            for (auto eq : it->idxs) {
              if (!first)
                errs() << ", ";
              else
                first = false;
              errs() << eq.to_string(iv_map);
            }
            errs() << "\r\n";
          }

          errs() << "writing to ... " << OutputFilename << " for all references to " << pair.first << "\r\n";

          // time to print the ILP
          for (auto it = pair.second.begin(); it != pair.second.end(); it++) {
            for (auto it2 = std::next(it); it2 != pair.second.end(); it2++) {
              if (!(isa<StoreInst>(it->I) || isa<StoreInst>(it2->I)))
                continue;
              auto fidx_it = it->idxs.begin();    // f_i()
              auto gidx_it = it2->idxs.begin();   // g_i()

              for (; fidx_it != it->idxs.end() && gidx_it != it2->idxs.end(); fidx_it++,gidx_it++) {
                eqns_s << "s.t. eqn" << neqns << ": " << fidx_it->to_string(iv_map, "a", ivars_instanced) 
                << " = " << gidx_it->to_string(iv_map, "B", ivars_instanced) << ";\n";
                neqns++;
              }

              assert(fidx_it == it->idxs.end() && "references to same memory location have different number of index functions");
              assert(gidx_it == it2->idxs.end() && "references to same memory location have different number of index functions");
            }
          }
          eqns_s.flush();
        }

        // print ranges for induction variables
        for (auto pair : ivars_instanced) {
          std::string lower, upper;
          assert(isa<SCEVAddRecExpr>(pair.second) && "induction variable is not a SCEVAddRecExpr");
          IVgetRange(dyn_cast<SCEVAddRecExpr>(pair.second), &SE, lower, upper);
          ilp_file << "var " << glpsolName(pair.first);
          if (!lower.empty())
            ilp_file << " >= " << lower;
          if (!upper.empty())
            ilp_file << " <= " << upper;
          ilp_file << " integer;" << std::endl;
        }

        // just pick some variable to maximize; we only care whether a solution exists
        if (ivars_instanced.empty())
          continue;
        else
          ilp_file << std::endl;

        auto fpair = &*ivars_instanced.begin();
        ilp_file << "maximize keks: " << glpsolName(fpair->first) << ";" << std::endl << std::endl;
        ilp_file << eqns << std::endl;
        ilp_file << "solve;" << std::endl << std::endl;
        ilp_file << "display ";
        bool first = true;
        for (auto pair : ivars_instanced) {
          if (!first)
            ilp_file << ", ";
          else first = false;
          ilp_file << glpsolName(pair.first);
        }
        ilp_file << ";" << std::endl << std::endl;
        ilp_file << "end;" << std::endl;
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
static RegisterPass<A3> X("ilpdep", "Induction Variable Pass",
							false /* Only looks at CFG */,
							false /* Analysis Pass */);
