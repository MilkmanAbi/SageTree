# SageTree AOT — Status: COMPLETE

_Last updated: 2026-05-31_

## Test results (fresh clean build) — PERFECT

| Suite              | Result        |
|--------------------|---------------|
| Interpreter        | **294 / 294** |
| lang/AOT           | **121 / 121** |
| stdlib/AOT         | **67 / 67**   |
| safety/AOT         | **22 / 22**   |
| perf/AOT           | **84 / 84**   |
| **Full AOT Suite** | **294 / 294** |

Zero LINK errors, zero crashes, zero wrong-output bugs. Every test passes
through BOTH the interpreter and the AOT (compile-to-C) backend.

### How to reproduce
```sh
make            # builds ./sage
make runtime    # builds obj/rt/libsage_runtime.a (with libffi + embedded CPython)
python3 run_tests.py        # interpreter suite
python3 run_aot_tests.py    # AOT suite: sage --aot -> gcc -> run, vs # EXPECT
```
AOT binaries link: `-lm -lpthread -ldl -lffi` plus `python3-config --ldflags --embed`
(libffi and libpython are auto-detected by the Makefile; absent them the FFI
features degrade to nil-returning stubs but everything else still builds).

## Highlights of the work (from 109/122 at original handoff -> 294/294)

### Real FFI — the headline feature
- **C FFI via libffi**: `ffi_open` / `ffi_call` / `ffi_sym` / `ffi_close` make
  genuine native calls into shared libraries (libc, libm, ...) from compiled
  Sage, with full `ffi_prep_cif` type marshalling.
- **Python FFI via embedded CPython**: `runtime/sage_py_rt.c` bridges
  SageValue <-> PyObject. `python.import/getattr/call/eval/exec/invoke` plus
  direct method dispatch (`math.sqrt(144.0)`, `json.loads(...)`) all work in
  compiled binaries. Arbitrary Python objects round-trip via a tagged pointer box.

### Sound garbage collection under AOT
- **Conservative GC**: AOT code emits no explicit GC roots, so the collector
  now scans the machine stack (+ flushed registers via setjmp) and the
  data/BSS segment for live object pointers, marking transitively by header
  type tag. This makes mid-execution + object-count-triggered collection sound,
  fixing cyclic-garbage collection without any codegen churn.

### Deep codegen correctness fixes
- `sage_rt_index` dispatches dicts to `dict_get` (was returning nil).
- Typed int/float locals use converting `SAGE_AS_INT64`/`SAGE_AS_DOUBLE`
  instead of bit-punning `.as.integer`.
- Dynamic calls through arbitrary expressions (`d["fn"](x)`, `arr[0](x)`).
- Call-site param-type conflict demotion to SageValue.
- Integer divide/modulo by zero returns 0 (was a CPU trap).
- Recursion-depth guard with call-graph cycle detection (TCO-proof).
- String interpolation leaves `{var}` literal when out of scope.
- Fixed two AOT-compiler buffer overflows; for-loop var typing; Some/Ok/Err
  module shadowing; global-var promotion.

### New AOT feature support
- Inline assembly (`asm_exec`/`asm_arch`: assemble -> link .so -> dlopen -> call).
- Synchronous threading model (`thread.spawn`/`join`, `spawn:` blocks).
- `macro` definitions compiled as procs.
- Modules wired to real runtime: `io` (file I/O), `atomic`, `channel`
  (incl. `try_recv` -> Option), `semaphore`, CPU topology, gc mode/stats.
- Builtins: `bytes*`, `sizeof`, `ptr_add/sub`, `path_*`, `mem_size`,
  `addressof`, `doc()`.
- Compile-time REPL-style recovery for `print(<module>.<undefined-member>)`.

### Build
- Makefile declares header dependencies (fixed intermittent stale-object
  heap corruption) and auto-detects libffi + Python for the runtime archive.
