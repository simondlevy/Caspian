#############################################################################
# CASPIAN - A Programmable Neuromorphic Computing Architecture
# Parker Mitchell, 2019
#############################################################################

# Applications
SH ?= bash
CC ?= gcc
CXX ?= g++
AR ?= ar
RANLIB ?= ranlib
PYTHON ?= python3
PIP ?= $(PYTHON) -m pip
VALGRIND ?= valgrind

# Directories
BIN = bin
DOCS = docs
INC = include
LIB = lib
OBJ = obj
SRC = src
STATIC_OBJ=obj_static
STATIC_SRC=src_static
EO = eo
UTILS = utils
TST = test
PYBINDINGS = bindings
PYBUILD = build
PYBUILD_BINDINGS = $(PYBUILD)/bindings
PYBUILD_SUFFIX := $(shell python3-config --extension-suffix || echo .cpython.so)

# Verilator support for uCaspian simulation
VCASPIAN ?= false

# FTDI USB support for uCaspian hardware
USB ?= false

# Framework Directories
ROOT          = ../..
ROOT_INCLUDE  = $(ROOT)/include

MKFILE_PATH := $(abspath $(lastword $(MAKEFILE_LIST)))
PROJECT_DIR := $(dir $(MKFILE_PATH))

# Compilation Flags
CFLAGS ?= -Wall -Wextra -pipe -O3 -g
CFLAGSBASE = $(CFLAGS) -std=c++14 -I$(INC) -I$(ROOT_INCLUDE)
LFLAGS = -lpthread -lftdi1

ifeq ($(VCASPIAN),true)
    CFLAGSBASE += -DWITH_VERILATOR
    CFLAGSBASE += -Iucaspian/sim/include -Iucaspian/vout -I/usr/local/share/verilator/include -I$(HOME)/oss-cad-suite/share/verilator/include
    LFLAGS += -lz
endif

ifeq ($(USB),true)
    CFLAGSBASE += -DWITH_USB
endif

## Targets
.PHONY: all clean test run_test python utils
all: $(LIB)/libcaspian.a library

clean:
	rm -rf $(BIN) $(OBJ) $(LIB) $(PYBUILD)

#########################
## Caspian library
LIBCASPIAN = $(LIB)/libcaspian.a
LIBFRAMEWORK = $(ROOT)/lib/libframework.a
PYLIBCASPIAN = $(PYBUILD)/caspian$(PYBUILD_SUFFIX)
PYFRAMEWORK  = $(ROOT)/build/neuro$(PYBUILD_SUFFIX)
LIBRARY=$(LIB)/libproc2.a
STATIC_LIB=$(LIB)/libproc1.a

$(LIBFRAMEWORK): $(ROOT_INCLUDE)/framework.hpp library
	$(MAKE) -C $(ROOT)

library: $(LIBRARY) $(STATIC_LIB)


$(PYFRAMEWORK):
	$(MAKE) -C $(ROOT) python

#########################
## Directories
$(BIN) $(OBJ) $(LIB) $(PYBUILD) $(PYBUILD_BINDINGS):
	mkdir -p $@

#########################
## Sources
HEADERS     = $(INC)/backend.hpp \
              $(INC)/constants.hpp \
	      $(INC)/network.hpp \
	      $(INC)/simulator.hpp \
	      $(INC)/ucaspian.hpp

TL_HEADERS  = $(INC)/processor.hpp \
              $(INC)/network_conversion.hpp

SOURCES     = $(SRC)/network.cpp \
	      $(SRC)/simulator.cpp

TL_SOURCES  = $(SRC)/processor.cpp \
              $(SRC)/network_conversion.cpp

ifeq ($(USB),true)
    SOURCES += $(SRC)/ucaspian.cpp
    LFLAGS += -lftdi1
endif

ifeq ($(VCASPIAN),true)
    SOURCES += $(SRC)/verilator_caspian.cpp
    V_OBJECTS := $(wildcard ucaspian/vout/V*.o) ucaspian/vout/verilated.o ucaspian/vout/verilated_fst_c.o
endif

OBJECTS    := $(patsubst $(SRC)/%.cpp,$(OBJ)/%.o,$(SOURCES))
TL_OBJECTS := $(patsubst $(SRC)/%.cpp,$(OBJ)/%.o,$(TL_SOURCES))

$(OBJECTS): $(OBJ)/%.o : $(SRC)/%.cpp $(HEADERS) | $(OBJ)
	$(CXX) $(CFLAGSBASE) -c $< -o $@

$(TL_OBJECTS): $(OBJ)/%.o : $(SRC)/%.cpp $(HEADERS) $(TL_HEADERS) $(ROOT_INCLUDE)/framework.hpp | $(OBJ)
	$(CXX) $(CFLAGSBASE) -c $< -o $@

$(LIBCASPIAN): $(OBJECTS) $(TL_OBJECTS) $(V_OBJECTS) | $(LIB)
	$(AR) r $@ $^
	$(RANLIB) $@

$(LIBRARY): obj/network.o obj/network_conversion.o obj/processor.o obj/simulator.o
	ar r $(LIBRARY) obj/network.o obj/network_conversion.o obj/processor.o obj/simulator.o
	ranlib $(LIBRARY)

$(STATIC_LIB): $(STATIC_OBJ)/static_proc.o
	ar r $(STATIC_LIB) $(STATIC_OBJ)/static_proc.o
	ranlib $(STATIC_LIB)

$(STATIC_OBJ)/static_proc.o: $(STATIC_SRC)/static_proc.cpp
	$(CC) $(CFLAGSBASE) -c $(STATIC_SRC)/static_proc.cpp -o $@

## flto flag compiler version specific fix.
# '-flto=auto' introduced since GCC 10.1:
# * https://gcc.gnu.org/onlinedocs/gcc-9.5.0/gcc/Optimize-Options.html#Optimize-Options (no)
# * https://gcc.gnu.org/onlinedocs/gcc-10.1.0/gcc/Optimize-Options.html#Optimize-Options (yes)
# Since GCC 12.1, the abundance of a parameter produces a warning if compiling multiple targets.
FLTOFLAGS = 
ifeq ($(CXX), g++)
	CXXMAJORVERSION = $(shell g++ --version | grep ^g++ | sed 's/^.* //g' | sed 's/\..*//')
	ifeq ($(shell test $(CXXMAJORVERSION) -ge 10; echo $$?),0)
		FLTOFLAGS = -flto=auto
	endif
endif

#########################
## Python support
PYBUILD_FLAGS := $(shell python3 -m pybind11 --includes) -I$(INC) -I$(ROOT_INCLUDE) -I$(PYBINDINGS) -I$(ROOT)/$(PYBINDINGS) -std=c++14 $(FLTOFLAGS) -fPIC -O3 -fvisibility=hidden
PYBUILD_LFLAGS = -shared

ifeq ($(USB),true)
    PYBUILD_FLAGS += -DWITH_USB
    PYBUILD_LFLAGS += -lftdi1
endif

ifeq ($(VCASPIAN),true)
    PYBUILD_FLAGS += -Iucaspian/sim/include -Iucaspian/vout -I/usr/local/share/verilator/include -I$(HOME)/oss-cad-suite/share/verilator/include -DWITH_VERILATOR
endif

# Patch symbol linkage issues for Mac OS
OS := $(strip $(shell uname -s))

ifeq ($(OS),Darwin)
    PYBUILD_LFLAGS += -undefined dynamic_lookup
endif

python: $(PYLIBCASPIAN)

BINDING_SOURCES := $(PYBINDINGS)/backend.cpp \
                   $(PYBINDINGS)/network.cpp \
		   $(PYBINDINGS)/bindings.cpp \
		   $(PYBINDINGS)/processor.cpp \
		   $(PYBINDINGS)/fast_infer.cpp

BINDING_OBJECTS := $(patsubst $(PYBINDINGS)/%.cpp,$(PYBUILD_BINDINGS)/%.o,$(BINDING_SOURCES))

PYBUILD_OBJECTS := $(patsubst $(SRC)/%.cpp,$(PYBUILD)/%.o,$(SOURCES)) \
		   $(patsubst $(SRC)/%.cpp,$(PYBUILD)/%.o,$(TL_SOURCES))

PYBUILD_TL_OBJECTS := $(wildcard $(ROOT)/$(PYBUILD)/*.o)

$(BINDING_OBJECTS): $(PYBUILD_BINDINGS)/%.o : $(PYBINDINGS)/%.cpp $(HEADERS) $(TL_HEADERS) $(ROOT_INCLUDE)/framework.hpp | $(PYBUILD_BINDINGS)
	$(CXX) $(PYBUILD_FLAGS) -c $< -o $@

$(PYBUILD_OBJECTS): $(PYBUILD)/%.o : $(SRC)/%.cpp $(HEADERS) $(TL_HEADERS) $(ROOT_INCLUDE)/framework.hpp | $(PYBUILD)
	$(CXX) $(PYBUILD_FLAGS) -c $< -o $@

$(PYLIBCASPIAN): $(PYBUILD_OBJECTS) $(BINDING_OBJECTS) $(PYFRAMEWORK) $(V_OBJECTS)
	$(CXX) $(PYBUILD_FLAGS) $(PYBUILD_OBJECTS) $(BINDING_OBJECTS) $(PYBUILD_TL_OBJECTS) $(V_OBJECTS) -o $@ $(PYBUILD_LFLAGS)

#########################
## Testing
TEST_SRC  := $(wildcard $(TST)/*.cpp)
TEST_OBJ  := $(patsubst $(TST)/%.cpp,$(OBJ)/%.o,$(TEST_SRC))
TEST_EXEC  = $(BIN)/test

$(TEST_OBJ): $(OBJ)/%.o : $(TST)/%.cpp $(HEADERS) $(TL_HEADERS) | $(OBJ)
	$(CXX) $(CFLAGSBASE) -c $< -o $@

$(TEST_EXEC): $(TEST_OBJ) $(LIBCASPIAN) $(LIBFRAMEWORK) | $(BIN)
	$(CXX) $(CFLAGSBASE) $(TEST_OBJ) -o $(TEST_EXEC) $(LIBCASPIAN) $(LIBFRAMEWORK) $(LFLAGS)

test: $(TEST_EXEC)

run_test: $(TEST_EXEC)
	$(VALGRIND) $(TEST_EXEC)

#########################
## Utilities
UTILITIES  = $(BIN)/pass_bench \
             $(BIN)/all_to_all_bench \
             $(BIN)/prune \
             $(BIN)/echo \
             $(BIN)/network_convert

$(UTILITIES): $(BIN)/% : $(UTILS)/%.cpp $(LIBCASPIAN) $(LIBFRAMEWORK) | $(BIN)
	$(CXX) $(CFLAGSBASE) $< -o $@ $(LIBCASPIAN) $(LIBFRAMEWORK) $(LFLAGS)

utils: $(UTILITIES)
