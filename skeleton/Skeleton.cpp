#include "llvm/Pass.h"
#include "llvm/IR/Function.h"
#include "llvm/Support/raw_ostream.h"

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
