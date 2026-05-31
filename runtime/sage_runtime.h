// sage_runtime.h — SageTree Compiled Runtime
//
// Every compiled Sage binary links against this runtime.
// It is the single source of truth for the value system, memory management,
// GC hooks, and stdlib primitives in compiled code.
//
// Design principles:
//   - Zero cost when not used. No LilyBox overhead unless @sandbox is active.
//   - Same Value type as the interpreter. No second-class representation.
//   - All three memory modes: GC (default), @manual, hybrid (gc_disable/enable).
//   - Thread-safe. All GC paths are re-entrant.
//
// ─────────────────────────────────────────────────────────────────────────────

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Version
// ─────────────────────────────────────────────────────────────────────────────

#define SAGE_RT_VERSION_MAJOR 1
#define SAGE_RT_VERSION_MINOR 0
#define SAGE_RT_VERSION_PATCH 0
#define SAGE_RT_VERSION "1.0.0"

// ─────────────────────────────────────────────────────────────────────────────
// Value Types
//
// Mirrors interpreter's ValueType exactly. Both paths must stay in sync.
// ─────────────────────────────────────────────────────────────────────────────

typedef enum {
    SAGE_VAL_INT       = 0,  // 64-bit signed integer
    SAGE_VAL_FLOAT     = 1,  // IEEE 754 double
    SAGE_VAL_BOOL      = 2,
    SAGE_VAL_NIL       = 3,
    SAGE_VAL_STRING    = 4,
    SAGE_VAL_ARRAY     = 5,
    SAGE_VAL_DICT      = 6,
    SAGE_VAL_TUPLE     = 7,
    SAGE_VAL_FUNCTION  = 8,  // Compiled function pointer + closure
    SAGE_VAL_INSTANCE  = 9,  // Class/struct instance
    SAGE_VAL_CLASS     = 10, // Class definition
    SAGE_VAL_EXCEPTION = 11,
    SAGE_VAL_BYTES     = 12, // Binary-safe byte buffer
    SAGE_VAL_POINTER   = 13, // Raw memory pointer (@manual / FFI)
    SAGE_VAL_CLIB      = 14, // Loaded C library handle (FFI)
} SageValType;

// ─────────────────────────────────────────────────────────────────────────────
// Forward declarations
// ─────────────────────────────────────────────────────────────────────────────

typedef struct SageValue   SageValue;
typedef struct SageArray   SageArray;
typedef struct SageDict    SageDict;
typedef struct SageTuple   SageTuple;
typedef struct SageBytes   SageBytes;
typedef struct SageClosure SageClosure;
typedef struct SageClass   SageClass;
typedef struct SageInst    SageInst;
typedef struct SageGCHdr   SageGCHdr;

// ─────────────────────────────────────────────────────────────────────────────
// Heap objects (all GC-tracked unless in @manual mode)
// ─────────────────────────────────────────────────────────────────────────────

// GC header prepended to every heap allocation
struct SageGCHdr {
    uint32_t   flags;   // [31..24]=type  [7]=marked  [6]=pinned  [5]=manual
    uint32_t   size;    // payload bytes
    SageGCHdr* next;    // intrusive linked list of all GC objects
};

#define SAGE_GC_TYPE(h)    (((h)->flags >> 24) & 0xFF)
#define SAGE_GC_MARKED(h)  ((h)->flags & (1u << 7))
#define SAGE_GC_PINNED(h)  ((h)->flags & (1u << 6))
#define SAGE_GC_MANUAL(h)  ((h)->flags & (1u << 5))  // allocated in @manual block
#define SAGE_GC_PAYLOAD(h) ((void*)((h) + 1))
#define SAGE_GC_HEADER(p)  (((SageGCHdr*)(p)) - 1)

#define SAGE_GC_SET_MARK(h)   ((h)->flags |=  (1u << 7))
#define SAGE_GC_CLR_MARK(h)   ((h)->flags &= ~(1u << 7))
#define SAGE_GC_SET_PIN(h)    ((h)->flags |=  (1u << 6))
#define SAGE_GC_SET_MANUAL(h) ((h)->flags |=  (1u << 5))

// ─────────────────────────────────────────────────────────────────────────────
// SageValue — the universal value type
//
// Defined before all heap-object structs so SageDictSlot can embed it by value.
// All heap-object structs only need pointer-to-SageValue, which forward decls cover.
// ─────────────────────────────────────────────────────────────────────────────

struct SageValue {
    SageValType type;
    union {
        int64_t    integer;   // SAGE_VAL_INT
        double     number;    // SAGE_VAL_FLOAT
        int        boolean;   // SAGE_VAL_BOOL
        char*      string;    // SAGE_VAL_STRING (GC-managed)
        SageArray* array;     // SAGE_VAL_ARRAY
        SageDict*  dict;      // SAGE_VAL_DICT
        SageTuple* tuple;     // SAGE_VAL_TUPLE
        SageBytes* bytes;     // SAGE_VAL_BYTES
        SageClosure* closure; // SAGE_VAL_FUNCTION
        SageInst*  instance;  // SAGE_VAL_INSTANCE
        SageClass* class_def; // SAGE_VAL_CLASS
        char*      exception; // SAGE_VAL_EXCEPTION (message)
        void*      pointer;   // SAGE_VAL_POINTER (@manual / FFI raw ptr)
        void*      clib;      // SAGE_VAL_CLIB (dlopen handle)
    } as;
};

// ─────────────────────────────────────────────────────────────────────────────
// Heap object structs (all use SageValue* or SageValue by value -- full def above)
// ─────────────────────────────────────────────────────────────────────────────

struct SageArray {
    SageValue* elems;
    int        count;
    int        cap;
};

struct SageTuple {
    SageValue* elems;
    int        count;
};

// Dict uses open-addressing with cached hash
typedef struct SageDictSlot {
    char*      key;
    int        key_len;
    uint32_t   hash;
    SageValue  val;   // embedded by value — requires SageValue fully defined above
} SageDictSlot;

struct SageDict {
    SageDictSlot* slots;
    int           count;
    int           cap;    // always power of 2
};

struct SageBytes {
    uint8_t* data;
    int      length;
    int      cap;
};

// Compiled closure: function pointer + captured variable table
struct SageClosure {
    SageValue  (*fn)(int argc, SageValue* argv, SageClosure* closure);
    SageValue*   captures;
    int          capture_count;
    const char** capture_names;  // for debugging
    void*        env;            // arbitrary environment pointer (for compiled closures)
};

// Class/struct definition (built at startup for compiled programs)
typedef struct SageMethod {
    const char* name;
    SageValue   (*fn)(SageInst* self, int argc, SageValue* argv);
} SageMethod;

struct SageClass {
    const char*  name;
    SageClass*   parent;
    SageMethod*  methods;
    int          method_count;
    const char** field_names;  // field name -> index map
    int          field_count;
    int          is_struct;    // 1 = value semantics (copy on assign)
};

struct SageInst {
    SageClass*    class_def;
    SageValue*    fields;       // dynamic field values
    const char**  field_names;  // dynamic field names (parallel to fields)
    int           field_count;  // number of dynamic fields
};
// ─────────────────────────────────────────────────────────────────────────────
// Value constructors (inline for zero-cost on hot paths)
// ─────────────────────────────────────────────────────────────────────────────

static inline SageValue sage_rt_int(int64_t v) {
    SageValue r; r.type = SAGE_VAL_INT; r.as.integer = v; return r;
}
static inline SageValue sage_rt_float(double v) {
    SageValue r; r.type = SAGE_VAL_FLOAT; r.as.number = v; return r;
}
static inline SageValue sage_rt_bool(int v) {
    SageValue r; r.type = SAGE_VAL_BOOL; r.as.boolean = (v != 0); return r;
}
static inline SageValue sage_rt_nil(void) {
    SageValue r; r.type = SAGE_VAL_NIL; r.as.integer = 0; return r;
}

// ─────────────────────────────────────────────────────────────────────────────
// Type check macros
// ─────────────────────────────────────────────────────────────────────────────

#define SAGE_IS_INT(v)      ((v).type == SAGE_VAL_INT)
#define SAGE_IS_FLOAT(v)    ((v).type == SAGE_VAL_FLOAT)
#define SAGE_IS_NUMERIC(v)  ((v).type == SAGE_VAL_INT || (v).type == SAGE_VAL_FLOAT)
#define SAGE_IS_BOOL(v)     ((v).type == SAGE_VAL_BOOL)
#define SAGE_IS_NIL(v)      ((v).type == SAGE_VAL_NIL)
#define SAGE_IS_STRING(v)   ((v).type == SAGE_VAL_STRING)
#define SAGE_IS_ARRAY(v)    ((v).type == SAGE_VAL_ARRAY)
#define SAGE_IS_DICT(v)     ((v).type == SAGE_VAL_DICT)
#define SAGE_IS_TUPLE(v)    ((v).type == SAGE_VAL_TUPLE)
#define SAGE_IS_BYTES(v)    ((v).type == SAGE_VAL_BYTES)
#define SAGE_IS_FUNCTION(v) ((v).type == SAGE_VAL_FUNCTION)
#define SAGE_IS_INSTANCE(v) ((v).type == SAGE_VAL_INSTANCE)
#define SAGE_IS_CLASS(v)    ((v).type == SAGE_VAL_CLASS)
#define SAGE_IS_EXCEPTION(v)((v).type == SAGE_VAL_EXCEPTION)
#define SAGE_IS_POINTER(v)  ((v).type == SAGE_VAL_POINTER)

// Numeric coercion
#define SAGE_AS_DOUBLE(v) \
    ((v).type == SAGE_VAL_INT ? (double)(v).as.integer : (v).as.number)
#define SAGE_AS_INT64(v) \
    ((v).type == SAGE_VAL_INT ? (v).as.integer : (int64_t)(v).as.number)

// Forward declaration needed by inline arithmetic functions below
void sage_rt_fatal(const char* fmt, ...) __attribute__((noreturn));

// ─────────────────────────────────────────────────────────────────────────────
// Arithmetic (int-preserving, promotes to float on mixed)
// ─────────────────────────────────────────────────────────────────────────────

// Coroutine-based generator support (ucontext)
#define _XOPEN_SOURCE 700
#include <ucontext.h>
typedef struct SageCoroutine SageCoroutine;
typedef void (*SageCoroutineBody)(SageCoroutine*);
struct SageCoroutine {
    ucontext_t ctx_callee;
    ucontext_t ctx_caller;
    char* stack;
    int stack_size;
    SageValue yielded;       // value yielded to caller
    SageValue* argv;         // args passed to coroutine
    int argc;
    int done;
    SageCoroutineBody body;
};
SageCoroutine* sage_rt_coro_new(SageCoroutineBody body, int argc, SageValue* argv);
SageValue sage_rt_coro_next(SageCoroutine* co);
void sage_rt_coro_yield(SageCoroutine* co, SageValue val);

// Forward declaration needed for sage_rt_add string path
SageValue sage_rt_string_concat(SageValue a, SageValue b);

// sage_rt_str_hash: FNV-1a hash for string values
static inline uint64_t sage_rt_str_hash(SageValue v) {
    if (!SAGE_IS_STRING(v) || !v.as.string) return 0;
    const char* s = v.as.string;
    uint64_t h = 14695981039346656037ULL;
    while (*s) { h ^= (unsigned char)*s++; h *= 1099511628211ULL; }
    return h;
}

// Forward declaration for operator overload dispatch
SageValue sage_rt_method_call(SageValue obj, const char* method, int argc, SageValue* argv);

static inline SageValue sage_rt_add(SageValue a, SageValue b) {
    if (SAGE_IS_INT(a) && SAGE_IS_INT(b))
        return sage_rt_int(a.as.integer + b.as.integer);
    if (SAGE_IS_NUMERIC(a) && SAGE_IS_NUMERIC(b))
        return sage_rt_float(SAGE_AS_DOUBLE(a) + SAGE_AS_DOUBLE(b));
    // String concatenation via generic add (for runtime dispatch)
    if (SAGE_IS_STRING(a) && SAGE_IS_STRING(b))
        return sage_rt_string_concat(a, b);
    // Instance operator overload: __add__
    if (a.type == SAGE_VAL_INSTANCE)
        return sage_rt_method_call(a, "__add__", 1, (SageValue[]){b});
    return sage_rt_nil();
}
static inline SageValue sage_rt_sub(SageValue a, SageValue b) {
    if (SAGE_IS_INT(a) && SAGE_IS_INT(b))
        return sage_rt_int(a.as.integer - b.as.integer);
    // Instance operator overload: __sub__
    if (a.type == SAGE_VAL_INSTANCE)
        return sage_rt_method_call(a, "__sub__", 1, (SageValue[]){b});
    return sage_rt_float(SAGE_AS_DOUBLE(a) - SAGE_AS_DOUBLE(b));
}
// Forward declaration for use in sage_rt_mul string repetition
SageValue sage_rt_str_repeat(SageValue s, int64_t n);

static inline SageValue sage_rt_mul(SageValue a, SageValue b) {
    if (SAGE_IS_INT(a) && SAGE_IS_INT(b))
        return sage_rt_int(a.as.integer * b.as.integer);
    // string * int = string repetition
    if (SAGE_IS_STRING(a) && SAGE_IS_INT(b)) return sage_rt_str_repeat(a, b.as.integer);
    if (SAGE_IS_INT(a) && SAGE_IS_STRING(b)) return sage_rt_str_repeat(b, a.as.integer);
    return sage_rt_float(SAGE_AS_DOUBLE(a) * SAGE_AS_DOUBLE(b));
}
static inline SageValue sage_rt_div(SageValue a, SageValue b) {
    // int / int = truncating integer division (Sage spec).
    // Division by zero evaluates to 0 (matches interpreter semantics).
    if (SAGE_IS_INT(a) && SAGE_IS_INT(b)) {
        if (b.as.integer == 0) return sage_rt_int(0);
        return sage_rt_int(a.as.integer / b.as.integer);
    }
    double bd = SAGE_AS_DOUBLE(b);
    if (bd == 0.0) return sage_rt_int(0);
    return sage_rt_float(SAGE_AS_DOUBLE(a) / bd);
}
static inline SageValue sage_rt_mod(SageValue a, SageValue b) {
    if (SAGE_IS_INT(a) && SAGE_IS_INT(b)) {
        if (b.as.integer == 0) return sage_rt_int(0);
        return sage_rt_int(a.as.integer % b.as.integer);
    }
    double bd = SAGE_AS_DOUBLE(b);
    if (bd == 0.0) return sage_rt_int(0);
    return sage_rt_float(fmod(SAGE_AS_DOUBLE(a), bd));
}
static inline SageValue sage_rt_pow(SageValue a, SageValue b) {
    return sage_rt_float(pow(SAGE_AS_DOUBLE(a), SAGE_AS_DOUBLE(b)));
}
static inline SageValue sage_rt_neg(SageValue a) {
    if (SAGE_IS_INT(a)) return sage_rt_int(-a.as.integer);
    return sage_rt_float(-a.as.number);
}

// Bitwise (int only)
static inline SageValue sage_rt_band(SageValue a, SageValue b) {
    return sage_rt_int(SAGE_AS_INT64(a) & SAGE_AS_INT64(b));
}
static inline SageValue sage_rt_bor(SageValue a, SageValue b) {
    return sage_rt_int(SAGE_AS_INT64(a) | SAGE_AS_INT64(b));
}
static inline SageValue sage_rt_bxor(SageValue a, SageValue b) {
    return sage_rt_int(SAGE_AS_INT64(a) ^ SAGE_AS_INT64(b));
}
static inline SageValue sage_rt_bnot(SageValue a) {
    return sage_rt_int(~SAGE_AS_INT64(a));
}
static inline SageValue sage_rt_shl(SageValue a, SageValue b) {
    return sage_rt_int(SAGE_AS_INT64(a) << (SAGE_AS_INT64(b) & 63));
}
static inline SageValue sage_rt_shr(SageValue a, SageValue b) {
    return sage_rt_int(SAGE_AS_INT64(a) >> (SAGE_AS_INT64(b) & 63));
}

// ─────────────────────────────────────────────────────────────────────────────
// Comparison
// ─────────────────────────────────────────────────────────────────────────────

int        sage_rt_equal(SageValue a, SageValue b);
int        sage_rt_less(SageValue a, SageValue b);

static inline int sage_rt_truthy(SageValue v) {
    switch (v.type) {
        case SAGE_VAL_NIL:   return 0;
        case SAGE_VAL_BOOL:  return v.as.boolean;
        case SAGE_VAL_INT:   return v.as.integer != 0;
        case SAGE_VAL_FLOAT: return v.as.number != 0.0;
        case SAGE_VAL_STRING:return v.as.string && v.as.string[0] != '\0';
        // Arrays, dicts, tuples, instances, functions — always truthy (Sage spec §7)
        default:             return 1;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// GC — Lifecycle and allocation
//
// All GC-mode allocations go through these. @manual allocations bypass GC
// and use sage_rt_manual_alloc / sage_rt_manual_free directly.
// ─────────────────────────────────────────────────────────────────────────────

// Must be called at the start of main() in every compiled binary.
// Initialises the GC, the root set, and the memory mode stack.
void sage_rt_init(void);

// Must be called at the end of main() (or via atexit). Runs final GC,
// checks for leaks in @manual regions if SAGE_RT_LEAK_CHECK is defined.
void sage_rt_shutdown(void);

// GC-managed allocation. Returns zeroed memory. Aborts on OOM.
void* sage_rt_gc_alloc(SageValType type, size_t size);

// Trigger a GC collection (can be called from user code via gc.collect()).
void sage_rt_gc_collect(void);
void sage_rt_gc_set_stack_base(void* p);

// GC stats (for perf module)
typedef struct {
    size_t   bytes_allocated;
    size_t   bytes_freed;
    int      collections;
    int      live_objects;
} SageRTGCStats;
SageRTGCStats sage_rt_gc_stats(void);
SageValue sage_rt_gc_mode(void);
void sage_rt_gc_set_arc(void);
void sage_rt_gc_set_orc(void);
void sage_rt_gc_set_tracing(void);
SageValue sage_rt_gc_collections(void);
SageValue sage_rt_gc_stats_dict(void);

// ─────────────────────────────────────────────────────────────────────────────
// Memory modes
//
// GC mode is the default.
// @manual blocks call sage_rt_gc_pause() / sage_rt_gc_resume().
// Hybrid mode uses sage_rt_gc_disable() / sage_rt_gc_enable() inline.
// ─────────────────────────────────────────────────────────────────────────────

// @manual block entry/exit (GC paused for the duration)
void sage_rt_gc_pause(void);
void sage_rt_gc_resume(void);

// Hybrid mode: gc_disable() / gc_enable() in user code
void sage_rt_gc_disable(void);
void sage_rt_gc_enable(void);

// Query whether GC is currently running
int sage_rt_gc_is_active(void);

// @manual allocations — NOT tracked by GC. User is responsible.
void* sage_rt_manual_alloc(size_t size);
void  sage_rt_manual_free(void* ptr);
void* sage_rt_manual_alloc_typed(SageValType type, size_t size); // zeros + records type

// ─────────────────────────────────────────────────────────────────────────────
// String operations
// ─────────────────────────────────────────────────────────────────────────────

// All string constructors allocate GC-managed copies unless noted.
SageValue sage_rt_string(const char* s);           // copy into GC heap
SageValue sage_rt_string_take(char* s);            // take ownership (must be GC-alloc'd)
SageValue sage_rt_string_len(const char* s, int n);// copy n bytes
SageValue sage_rt_string_concat(SageValue a, SageValue b);
SageValue sage_rt_string_repeat(SageValue s, int n);

// Convert any value to its string representation
SageValue sage_rt_tostring(SageValue v);

// String methods (mirror interpreter's builtin_method_call)
SageValue sage_rt_str_upper(SageValue s);
SageValue sage_rt_str_index(SageValue s, int64_t idx);
SageValue sage_rt_str_repeat(SageValue s, int64_t n);
SageValue sage_rt_str_lower(SageValue s);
SageValue sage_rt_str_strip(SageValue s);
SageValue sage_rt_str_split(SageValue s, SageValue delim);
SageValue sage_rt_str_startswith(SageValue s, SageValue prefix);
SageValue sage_rt_str_endswith(SageValue s, SageValue suffix);
SageValue sage_rt_str_replace(SageValue s, SageValue old, SageValue new_s);
SageValue sage_rt_str_find(SageValue s, SageValue needle);
SageValue sage_rt_str_slice(SageValue s, int start, int end);
int       sage_rt_str_len(SageValue s);

// ─────────────────────────────────────────────────────────────────────────────
// Array operations
// ─────────────────────────────────────────────────────────────────────────────

SageValue sage_rt_array_new(void);
SageValue sage_rt_array_of(int count, ...);        // sage_rt_array_of(3, v1, v2, v3)
void      sage_rt_array_push(SageValue arr, SageValue val);
SageValue sage_rt_array_pop(SageValue arr);
SageValue sage_rt_array_get(SageValue arr, SageValue idx);
SageValue sage_rt_dict_get(SageValue dict, SageValue key);  /* fwd: used by sage_rt_index */
// Universal indexer: dispatches to array_get or str_index based on runtime type
static inline SageValue sage_rt_index(SageValue obj, SageValue idx) {
    if (SAGE_IS_STRING(obj)) {
        int64_t i = SAGE_AS_INT64(idx);
        if (i < 0) i += (int64_t)strlen(obj.as.string);
        if (i < 0 || i >= (int64_t)strlen(obj.as.string)) return sage_rt_nil();
        char buf[2] = {obj.as.string[i], '\0'};
        return sage_rt_string(buf);
    }
    if (SAGE_IS_DICT(obj)) return sage_rt_dict_get(obj, idx);
    return sage_rt_array_get(obj, idx);
}
SageValue sage_rt_array_contains(SageValue arr, SageValue val);
SageValue sage_rt_array_join(SageValue arr, SageValue sep);
SageValue sage_rt_array_index_of(SageValue arr, SageValue val);
SageValue sage_rt_array_slice(SageValue arr, int start, int end);
SageValue sage_rt_array_reverse(SageValue arr);
SageValue sage_rt_array_sort(SageValue arr);
void      sage_rt_array_set(SageValue arr, SageValue idx, SageValue val);
SageValue sage_rt_array_slice(SageValue arr, int start, int end);
int       sage_rt_array_len(SageValue arr);
SageValue sage_rt_array_concat(SageValue a, SageValue b);

// ─────────────────────────────────────────────────────────────────────────────
// Dict operations
// ─────────────────────────────────────────────────────────────────────────────

SageValue sage_rt_dict_new(void);
SageValue sage_rt_dict_get(SageValue dict, SageValue key);
void      sage_rt_dict_set(SageValue dict, SageValue key, SageValue val);
int       sage_rt_dict_has(SageValue dict, SageValue key);
SageValue sage_rt_dict_remove(SageValue dict, SageValue key);
SageValue sage_rt_dict_get_or(SageValue dict, SageValue key, SageValue def);
SageValue sage_rt_dict_len(SageValue dict);
void      sage_rt_dict_del(SageValue dict, SageValue key);
SageValue sage_rt_dict_keys(SageValue dict);
SageValue sage_rt_dict_values(SageValue dict);

// ─────────────────────────────────────────────────────────────────────────────
// Tuple operations
// ─────────────────────────────────────────────────────────────────────────────

SageValue sage_rt_tuple_new(int count, ...);
SageValue sage_rt_tuple_get(SageValue tuple, int idx);
int       sage_rt_tuple_len(SageValue tuple);

// ─────────────────────────────────────────────────────────────────────────────
// Bytes operations
// ─────────────────────────────────────────────────────────────────────────────

SageValue sage_rt_bytes_new(int cap);
SageValue sage_rt_bytes_from(const uint8_t* data, int len);
void      sage_rt_bytes_push(SageValue bytes, uint8_t byte);
uint8_t   sage_rt_bytes_get(SageValue bytes, int idx);
int       sage_rt_bytes_len(SageValue bytes);
SageValue sage_rt_bytes_ctor(SageValue x);
SageValue sage_rt_bytes_get_v(SageValue bytes, SageValue idx);
SageValue sage_rt_bytes_set_v(SageValue bytes, SageValue idx, SageValue val);
SageValue sage_rt_bytes_to_string(SageValue bytes);
SageValue sage_rt_bytes_from_string(SageValue s);
SageValue sage_rt_bytes_slice(SageValue bytes, SageValue startv, SageValue endv);
SageValue sage_rt_bytes_len_v(SageValue bytes);
SageValue sage_rt_sizeof(SageValue x);

// ─────────────────────────────────────────────────────────────────────────────
// Class / struct / instance
// ─────────────────────────────────────────────────────────────────────────────

// Register a class definition (called once at program startup)
SageValue sage_rt_add_methods(SageValue class_val, SageMethod* methods, int count);
SageValue sage_rt_class_new(const char* name, SageClass* parent,
                            SageMethod* methods, int method_count,
                            const char** field_names, int field_count,
                            int is_struct);

// Create a new instance (fields zero-initialised)
SageValue sage_rt_instance_new(SageValue class_val);

// Field access by name (linear scan; compiled code uses sage_rt_field_by_idx)
SageValue sage_rt_field_get(SageValue inst, const char* name);
void      sage_rt_field_set(SageValue inst, const char* name, SageValue val);

// Field access by precomputed index (O(1), used by compiled code after layout)
SageValue sage_rt_field_by_idx(SageValue inst, int idx);
void      sage_rt_field_set_idx(SageValue inst, int idx, SageValue val);

// Method lookup and dispatch
SageValue sage_rt_method_call(SageValue inst, const char* name,
                              int argc, SageValue* argv);

// Struct value-copy (deep copy when struct is assigned)
SageValue sage_rt_struct_copy(SageValue src);

// ─────────────────────────────────────────────────────────────────────────────
// Builtins (mirror interpreter built-ins)
// ─────────────────────────────────────────────────────────────────────────────

SageValue sage_rt_len(SageValue v);
SageValue sage_rt_typeof(SageValue v);             // returns str (capitalized: Array, Dict)
SageValue sage_rt_type_lc(SageValue v);            // returns str (lowercase: array, dict)
SageValue sage_rt_int_cast(SageValue v);
SageValue sage_rt_float_cast(SageValue v);
SageValue sage_rt_str_cast(SageValue v);
SageValue sage_rt_bool_cast(SageValue v);
SageValue sage_rt_range(SageValue start, SageValue end); // exclusive
SageValue sage_rt_range_inc(SageValue start, SageValue end); // inclusive ..=
SageValue sage_rt_input(SageValue prompt);
SageValue sage_rt_clock(void);
SageValue sage_rt_some(SageValue v);
SageValue sage_rt_ok(SageValue v);
SageValue sage_rt_err(SageValue v);
SageValue sage_rt_precision(SageValue f, SageValue digits);
SageValue sage_rt_tonumber(SageValue s);

// C struct layout builtins (mirrors interpreter struct_def / struct_new / etc.)
SageValue sage_rt_struct_def(SageValue fields);
SageValue sage_rt_struct_new(SageValue def);
SageValue sage_rt_struct_get(SageValue ptr, SageValue def, SageValue field);
SageValue sage_rt_struct_set(SageValue ptr, SageValue def, SageValue field, SageValue val);
SageValue sage_rt_struct_size(SageValue def);

// ─────────────────────────────────────────────────────────────────────────────
// I/O
// ─────────────────────────────────────────────────────────────────────────────

void sage_rt_print(SageValue v);
void sage_rt_print_kw(SageValue v);   // keyword print: dispatches __str__
void sage_rt_println(SageValue v);

// ─────────────────────────────────────────────────────────────────────────────
// Exceptions and panics
// ─────────────────────────────────────────────────────────────────────────────

// Create an exception value (used with raise)
SageValue sage_rt_exception(const char* fmt, ...);

// Panic — unrecoverable. Prints Firefly-style message and exits.
// Use for divide-by-zero, OOM, assertion failures, etc.
void sage_rt_fatal(const char* fmt, ...) __attribute__((noreturn));
void sage_rt_panic(SageValue v) __attribute__((noreturn));

// Try/catch state (setjmp-based exception handling in compiled code)
#include <setjmp.h>

typedef struct SageExcFrame {
    jmp_buf            jb;
    SageValue          exc;           // the caught value
    int                active;        // 1 = inside try block
    int                saved_depth;   // sage_rt_call_depth at try entry (restored on unwind)
    struct SageExcFrame* prev;
} SageExcFrame;

// Per-thread exception frame stack
extern _Thread_local SageExcFrame* sage_rt_exc_top;

// ─────────────────────────────────────────────────────────────────────────────
// Recursion-depth guard (compiled code)
//
// AOT-emitted procedures that participate in a recursive cycle increment a
// per-thread depth counter on entry and decrement it on every exit (via a
// scope-cleanup handler). When the counter exceeds SAGE_RT_MAX_DEPTH a
// catchable "Maximum recursion depth exceeded" exception is raised, mirroring
// the interpreter (error[E070], MAX_RECURSION_DEPTH=1000).
//
// Because the cleanup handler does NOT run when the stack is unwound by
// longjmp (i.e. a raised exception), SAGE_TRY/SAGE_CATCH — and the equivalent
// inline emission in aot.c — save and restore the counter across try frames.
// ─────────────────────────────────────────────────────────────────────────────
#ifndef SAGE_RT_MAX_DEPTH
#define SAGE_RT_MAX_DEPTH 1000
#endif
extern _Thread_local int sage_rt_call_depth;
// Raise the recursion-depth exception (never returns to the caller).
void sage_rt_recursion_error(void) __attribute__((noreturn));
// Scope-cleanup helper: decrement the depth counter when a guarded frame exits
// normally. Used via __attribute__((cleanup(sage_rt_depth_pop))).
static inline void sage_rt_depth_pop(int* _slot) { (void)_slot; sage_rt_call_depth--; }

// Macros for compiled try/catch/finally
//
//   SAGE_TRY(frame) { ... }
//   SAGE_CATCH(frame, varname) { ... use varname ... }
//   SAGE_FINALLY(frame) { ... }
//   SAGE_ENDTRY(frame)
//
#define SAGE_TRY(f) \
    do { \
        (f).prev = sage_rt_exc_top; \
        (f).active = 1; \
        (f).saved_depth = sage_rt_call_depth; \
        sage_rt_exc_top = &(f); \
        if (setjmp((f).jb) == 0)

#define SAGE_CATCH(f, var) \
        else { \
            (f).active = 0; \
            sage_rt_exc_top = (f).prev; \
            sage_rt_call_depth = (f).saved_depth; \
            SageValue var = (f).exc;

#define SAGE_FINALLY(f) \
        } { // finally block always runs

#define SAGE_ENDTRY(f) \
        } \
        sage_rt_exc_top = (f).prev; \
    } while(0)

// Raise: longjmp to nearest SAGE_TRY frame, or fatal if none
void sage_rt_raise(SageValue exc) __attribute__((noreturn));

// ─────────────────────────────────────────────────────────────────────────────
// @manual memory builtins
// (mem_alloc, mem_free, mem_read, mem_write, ptr_add as in interpreter)
// ─────────────────────────────────────────────────────────────────────────────

SageValue sage_rt_mem_alloc(SageValue size_val);
void      sage_rt_mem_free(SageValue ptr_val);
SageValue sage_rt_mem_read(SageValue ptr_val, SageValue offset_val,
                           SageValue type_str);
void      sage_rt_mem_write(SageValue ptr_val, SageValue offset_val,
                            SageValue type_str, SageValue value);
SageValue sage_rt_ptr_add(SageValue ptr_val, SageValue offset_val);
SageValue sage_rt_ptr_null(void);
SageValue sage_rt_mem_size(SageValue ptr_val);
SageValue sage_rt_addressof(SageValue v);

// Path utilities (match interpreter path_* builtins)
SageValue sage_rt_path_join(int argc, SageValue* argv);
SageValue sage_rt_path_dirname(SageValue p);
SageValue sage_rt_path_basename(SageValue p);
SageValue sage_rt_path_ext(SageValue p);
SageValue sage_rt_path_stem(SageValue p);
SageValue sage_rt_path_exists(SageValue p);

// CPU topology
SageValue sage_rt_cpu_count(void);
SageValue sage_rt_cpu_physical_cores(void);
SageValue sage_rt_cpu_has_hyperthreading(void);
// File I/O (io module)
SageValue sage_rt_io_writefile(SageValue path, SageValue content);
SageValue sage_rt_io_readfile(SageValue path);
SageValue sage_rt_io_exists(SageValue path);
SageValue sage_rt_io_remove(SageValue path);
// Inline assembly
SageValue sage_rt_asm_exec(int argc, SageValue* argv);
SageValue sage_rt_asm_arch(void);
// C FFI
SageValue sage_rt_ffi_open(SageValue name);
SageValue sage_rt_ffi_close(SageValue lib);
SageValue sage_rt_ffi_sym(SageValue lib, SageValue name);
SageValue sage_rt_ffi_call(SageValue lib, SageValue fname, SageValue rtype_v, SageValue argsv);
// Python FFI (defined in sage_py_rt.c)
SageValue sage_rt_py_import(SageValue name);
SageValue sage_rt_py_getattr(SageValue obj, SageValue name);
SageValue sage_rt_py_call(SageValue obj, SageValue method, int argc, SageValue* argv);
SageValue sage_rt_py_invoke(SageValue callable, int argc, SageValue* argv);
SageValue sage_rt_py_eval(SageValue code);
SageValue sage_rt_py_exec(SageValue code);
SageValue sage_rt_py_method(SageValue obj, const char* name, int argc, SageValue* argv);
int       sage_rt_py_is_obj(SageValue v);
// Atomics
SageValue sage_rt_atomic_new(SageValue init);
SageValue sage_rt_atomic_load(SageValue av);
SageValue sage_rt_atomic_store(SageValue av, SageValue n);
SageValue sage_rt_atomic_add(SageValue av, SageValue n);
SageValue sage_rt_atomic_sub(SageValue av, SageValue n);
SageValue sage_rt_atomic_cas(SageValue av, SageValue expected, SageValue desired);
SageValue sage_rt_atomic_exchange(SageValue av, SageValue n);
// Channels
SageValue sage_rt_channel_new(void);
SageValue sage_rt_channel_send(SageValue cv, SageValue item);
SageValue sage_rt_channel_recv(SageValue cv);
SageValue sage_rt_channel_try_recv(SageValue cv);
SageValue sage_rt_channel_close(SageValue cv);
SageValue sage_rt_channel_len(SageValue cv);
SageValue sage_rt_channel_is_closed(SageValue cv);
// Semaphores
SageValue sage_rt_sem_new(SageValue init);
SageValue sage_rt_sem_wait(SageValue sv);
SageValue sage_rt_sem_trywait(SageValue sv);
SageValue sage_rt_sem_post(SageValue sv);

// ─────────────────────────────────────────────────────────────────────────────
// LilyBox hook points (no-ops when sandbox not active)
//
// These are always declared. When the sandbox is NOT compiled in
// (SAGE_RT_NO_SANDBOX), they compile to empty inline stubs — zero overhead.
// When a compiled binary opts into @sandbox, sage_sandbox_rt.c is linked and
// these resolve to real permission checks.
// ─────────────────────────────────────────────────────────────────────────────

#ifndef SAGE_RT_NO_SANDBOX

// Called by the runtime before any sandboxable operation.
// Returns 1 if permitted, 0 if denied (caller should then raise E090-E097).
int sage_rt_sb_check_fs_read(const char* path);
int sage_rt_sb_check_fs_write(const char* path);
int sage_rt_sb_check_net(const char* host, int port);
int sage_rt_sb_check_exec(const char* cmd);
int sage_rt_sb_check_ffi(const char* lib);
int sage_rt_sb_check_env(const char* key);

// Called once at startup with the binary's embedded permission manifest.
void sage_rt_sb_init(const void* manifest);

#else
// Sandbox disabled — all checks are unconditional pass, zero cost
static inline int sage_rt_sb_check_fs_read(const char* p)   { (void)p; return 1; }
static inline int sage_rt_sb_check_fs_write(const char* p)  { (void)p; return 1; }
static inline int sage_rt_sb_check_net(const char* h, int p){ (void)h;(void)p; return 1; }
static inline int sage_rt_sb_check_exec(const char* c)      { (void)c; return 1; }
static inline int sage_rt_sb_check_ffi(const char* l)       { (void)l; return 1; }
static inline int sage_rt_sb_check_env(const char* k)       { (void)k; return 1; }
static inline void sage_rt_sb_init(const void* m)           { (void)m; }
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Utility
// ─────────────────────────────────────────────────────────────────────────────

// Safe malloc/strdup that abort on OOM (same as interpreter's SAGE_ALLOC)
void* sage_rt_alloc(size_t size);
void* sage_rt_realloc(void* ptr, size_t size);
char* sage_rt_strdup(const char* s);
char* sage_rt_strndup(const char* s, int n);

// null-coalescing helper: return left if not nil, else right
static inline SageValue sage_rt_nullcoal(SageValue left, SageValue right) {
    return SAGE_IS_NIL(left) ? right : left;
}

#ifdef __cplusplus
}
#endif

// Native function wrapping
typedef SageValue (*SageNativeFn)(int argc, SageValue* argv, void* env);
SageValue sage_rt_make_fn(SageNativeFn fn, void* env, const char* name);
SageValue sage_rt_call_fn(SageValue fn, int argc, SageValue* argv);
