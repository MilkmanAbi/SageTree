# SageTree Makefile
# Build: make
# Clean: make clean
# Test:  make test

CC       := gcc
SAGE_VERSION := $(shell cat VERSION 2>/dev/null || echo "0.0.0")

CFLAGS   := -O2 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
            -Iinclude -Isrc/vm -Isrc/gc -Isrc/lilybox \
            -DSAGE_VERSION_STR='"$(SAGE_VERSION)"' \
            -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L
LDFLAGS  := -lm -lpthread -ldl

# Detect libcurl
CURL_LDFLAGS := $(shell pkg-config --libs libcurl 2>/dev/null)
ifneq ($(strip $(CURL_LDFLAGS)),)
    LDFLAGS += $(CURL_LDFLAGS)
    CFLAGS  += -DSAGE_HAS_CURL
endif

# Detect libffi
LIBFFI_CFLAGS  := $(shell pkg-config --cflags libffi 2>/dev/null)
LIBFFI_LDFLAGS := $(shell pkg-config --libs libffi 2>/dev/null)
ifneq ($(strip $(LIBFFI_LDFLAGS)),)
    CFLAGS  += $(LIBFFI_CFLAGS) -DSAGE_HAS_FFI
    LDFLAGS += $(LIBFFI_LDFLAGS)
endif

# Detect Python
PYTHON_CFLAGS  := $(shell python3-config --includes 2>/dev/null)
PYTHON_LDFLAGS := $(shell python3-config --ldflags --embed 2>/dev/null || python3-config --ldflags 2>/dev/null)
ifneq ($(strip $(PYTHON_CFLAGS)),)
    CFLAGS  += $(PYTHON_CFLAGS)
    LDFLAGS += $(PYTHON_LDFLAGS)
    PYTHON_SRC := src/sage_python.c
else
    CFLAGS += -DSAGE_NO_PYTHON
    PYTHON_SRC :=
endif

# ── Sources ───────────────────────────────────────────────────────────
CORE_SRC := \
    src/ast.c src/diagnostic.c src/env.c src/firefly.c src/gc.c \
    src/interpreter.c src/lexer.c src/module.c src/parser.c \
    src/sage_thread.c src/stdlib.c src/typecheck.c src/safety.c \
    src/value.c src/stubs.c src/net.c src/socket_module.c \
    src/sandbox_module.c \
    src/aot.c src/codegen.c src/compiler.c src/constfold.c \
    src/dce.c src/inline.c src/formatter.c src/linter.c \
    src/pass.c \
    $(PYTHON_SRC)

VM_SRC := src/vm/vm.c src/vm/bytecode.c src/vm/runtime.c src/vm/program.c
GC_SRC := src/gc/sagegc.c src/gc/env_hashmap.c
LB_SRC := src/lilybox/lilybox.c

MAIN_SRC := src/main.c

# ── Objects ───────────────────────────────────────────────────────────
CORE_OBJ := $(patsubst src/%.c, obj/%.o, $(CORE_SRC))
VM_OBJ   := $(patsubst src/vm/%.c, obj/vm/%.o, $(VM_SRC))
GC_OBJ   := $(patsubst src/gc/%.c, obj/gc_%.o, $(GC_SRC))
LB_OBJ   := $(patsubst src/lilybox/%.c, obj/lb_%.o, $(LB_SRC))
MAIN_OBJ := obj/main.o

ALL_OBJ  := $(CORE_OBJ) $(VM_OBJ) $(GC_OBJ) $(LB_OBJ) $(MAIN_OBJ)
TARGET   := sage

# ── Build rules ───────────────────────────────────────────────────────
all: $(TARGET)

$(TARGET): $(ALL_OBJ)
	$(CC) $(CFLAGS) -o $@ $(ALL_OBJ) $(LDFLAGS)
	@echo "  Built: $(TARGET) ($(SAGE_VERSION))"

obj/%.o: src/%.c $(wildcard include/*.h) | obj
	$(CC) $(CFLAGS) -c $< -o $@

obj/vm/%.o: src/vm/%.c | obj/vm
	$(CC) $(CFLAGS) -c $< -o $@

obj/gc_%.o: src/gc/%.c | obj
	$(CC) $(CFLAGS) -c $< -o $@

obj/lb_%.o: src/lilybox/%.c | obj
	$(CC) $(CFLAGS) -c $< -o $@

obj:
	@mkdir -p obj

obj/vm:
	@mkdir -p obj/vm

clean:
	rm -rf obj $(TARGET)
	@echo "  Cleaned."

test: $(TARGET)
	@python3 run_tests.py

.PHONY: all clean test

# ── Runtime (IP1-A: compiled Sage binary support) ─────────────────────
RT_CFLAGS := -O2 -Wall -Wextra -Wno-unused-parameter -std=c11 \
             -Iruntime -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE \
             $(LIBFFI_CFLAGS) $(if $(strip $(LIBFFI_LDFLAGS)),-DSAGE_HAS_FFI,)
# Python runtime FFI (optional): compiled into the runtime archive when the
# Python dev headers are present. Programs that use it must link Python too.
PYRT_INCLUDES := $(shell python3-config --includes 2>/dev/null)
ifneq ($(strip $(PYRT_INCLUDES)),)
    PYRT_CFLAGS := $(PYRT_INCLUDES) -DSAGE_HAS_PYRT
else
    PYRT_CFLAGS :=
endif
RT_SRC    := runtime/sage_runtime.c
RT_OBJ    := obj/rt/sage_runtime.o
PYRT_OBJ  := obj/rt/sage_py_rt.o
RT_LIB    := obj/rt/libsage_runtime.a

runtime: $(RT_LIB)
	@echo "  Built: $(RT_LIB)"

$(RT_LIB): $(RT_OBJ) $(PYRT_OBJ) | obj/rt
	ar rcs $@ $^

$(RT_OBJ): $(RT_SRC) runtime/sage_runtime.h | obj/rt
	$(CC) $(RT_CFLAGS) -c $< -o $@

$(PYRT_OBJ): runtime/sage_py_rt.c runtime/sage_runtime.h | obj/rt
	$(CC) -O2 -std=c11 -Iruntime -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE $(PYRT_CFLAGS) -c $< -o $@

obj/rt:
	@mkdir -p obj/rt

.PHONY: runtime

# ── Sandbox runtime (IP1-B: opt-in, linked only when @sandbox active) ─
SB_SRC  := runtime/sage_sandbox_rt.c
SB_OBJ  := obj/rt/sage_sandbox_rt.o
SB_LIB  := obj/rt/libsage_sandbox_rt.a

sandbox_rt: $(SB_LIB)
	@echo "  Built: $(SB_LIB)"

$(SB_LIB): $(SB_OBJ) | obj/rt
	ar rcs $@ $^

$(SB_OBJ): $(SB_SRC) runtime/sage_sandbox_rt.h runtime/sage_runtime.h | obj/rt
	$(CC) $(RT_CFLAGS) -c $< -o $@

.PHONY: sandbox_rt
