# cs-255-llvm-pass-skeleton
The given code is a skeleton of a llvm pass using llvm-8 on cycle machine.

## Prepration
0. Add LLVM binaries to your path (optional)
```
# Add in your .bash_profile or other shell config
export PATH=/localdisk/cs255/llvm-project/bin:$PATH
```

1. Add `LLVM_HOME` in your environment:
```
export LLVM_HOME=/localdisk/cs255/llvm-project
```

2. Use CMake and Make to compile your pass to runtime lib
```
mkdir build
cd build
cmake ..
make
```

3. Use your pass
- One liner: `clang -Xclang -load -Xclang build/skeleton/libSkeletonPass.so test.cpp`
- Inside the `test/Makefile`, you can find out the way that use `opt` to
use your own pass.

## Hints
1. To do dependence testing, your first step is to find loops in the program.
	- Check how to use other llvm pass in your own pass
	- http://llvm.org/doxygen/classllvm_1_1Loop.html
	- https://llvm.org/doxygen/classllvm_1_1LoopInfo.html

2. Get induction variables in the loops you found.
	- TODO

## Reference
- https://www.cs.cornell.edu/~asampson/blog/clangpass.html
- https://github.com/abenkhadra/llvm-pass-tutorial
