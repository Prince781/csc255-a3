#include "llvm/Pass.h"
#include "llvm/IR/Function.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Analysis/LoopPass.h"
#include "llvm/Analysis/LoopIterator.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Transforms/Utils/LoopUtils.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/IR/Dominators.h"
// #include "llvm/IR/TypeBuilder.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Transforms/Utils/SimplifyIndVar.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Verifier.h"

using namespace llvm;

namespace {
  struct A3 : public FunctionPass {
    static char ID;
    A3() : FunctionPass(ID) {}

    bool runOnFunction(Function &F) override {
      return false;
    }
  };
}

char A3::ID = 0;
static RegisterPass<A3> X("a3", "Induction Variable Pass",
							false /* Only looks at CFG */,
							false /* Analysis Pass */);
