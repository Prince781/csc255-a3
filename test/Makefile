#===- test/Makefile -------------------------*- Makefile -*-===##

# Relative path to the top of the source tree.
#
LEVEL=..
BUILD=../build

LLVM_BIN_DIR=$(LLVM_HOME)/bin
CLANG_BIN_DIR=$(LLVM_HOME)/bin

OPT= $(LLVM_BIN_DIR)/opt
LLC= $(LLVM_BIN_DIR)/llc
CC= $(CLANG_BIN_DIR)/clang
DIS= $(LLVM_BIN_DIR)/llvm-dis

all: check

check: test.c
	$(CC) -O0 -Xclang -disable-O0-optnone -c -emit-llvm test.c -o test.bc
	$(OPT) -load $(LEVEL)/build/skeleton/libSkeletonPass.so -a3 --debug-pass=Structure < test.bc > test.bc.opt
	$(LLC) -filetype=obj test.bc.opt -o test.o
	$(CC) test.o -o test

clean:
	rm -f *.bc *.opt *.o *.ll test
