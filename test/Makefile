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

check: test.bc.opt level2.bc.opt level3.bc.opt level4.bc.opt nonaffine3.bc.opt

%.bc.opt: %.bc $(LEVEL)/build/skeleton/libilpdep.so
	$(OPT) -load $(LEVEL)/build/skeleton/libilpdep.so $(shell cat $(LEVEL)/OPTARGS) -ilpdep -ilpoutput=output.ilp --debug-pass=Structure < $< > $@
	$(DIS) $@

%.bc: %.c
	$(CC) -O0 -Xclang -disable-O0-optnone -c -emit-llvm $< -o $@
	$(DIS) $@

%.o: %.bc.opt
	$(LLC) -filetype=obj $< -o $@

test: test.o

level2: level2.o

clean:
	rm -f *.bc *.opt *.o *.ll test
