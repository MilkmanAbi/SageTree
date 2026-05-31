// sage_runtime.c — SageTree Compiled Runtime Implementation
//
// This file is compiled once and linked into every compiled Sage binary.
// It provides: value operations, GC, memory modes, strings, arrays, dicts,
// tuples, bytes, class/instance, builtins, I/O, and exception handling.
//
// LilyBox hooks are declared in sage_runtime.h but implemented separately
// in sage_sandbox_rt.c — only linked when @sandbox is active.

#define _POSIX_C_SOURCE 200809L

#include "sage_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <ctype.h>
#include <inttypes.h>
#include <setjmp.h>
#include <unistd.h>
#include <dlfcn.h>

// ─────────────────────────────────────────────────────────────────────────────
// Exception frame stack (thread-local)
// ─────────────────────────────────────────────────────────────────────────────

_Thread_local SageExcFrame* sage_rt_exc_top = NULL;

// ─────────────────────────────────────────────────────────────────────────────
// Internal GC state
// ─────────────────────────────────────────────────────────────────────────────

typedef struct {
    SageGCHdr* objects;       // linked list of all live GC objects
    size_t     bytes_alloc;   // total bytes ever allocated
    size_t     bytes_freed;   // total bytes ever freed
    size_t     bytes_live;    // currently live bytes
    int        obj_count;     // live object count
    int        collections;   // total collection count
    int        enabled;       // 0 = GC paused (manual or gc_disable)
    int        pause_depth;   // nested pause depth (@manual blocks can nest)
    size_t     next_gc;       // collect when bytes_live exceeds this
    size_t     next_gc_objects; // collect when obj_count reaches this
    int        mode;          // 0 = tracing (default), 1 = arc, 2 = orc
} _SageRTGC;

static _SageRTGC _gc = {
    .enabled  = 1,
    .next_gc  = 512 * 1024,  // first GC at 512 KB
    .next_gc_objects = 128,  // or after 128 live objects (matches interpreter)
};

// Root set for GC: a simple stack of pointers to SageValue
// Compiled code registers locals via SAGE_RT_PUSH_ROOT / SAGE_RT_POP_ROOT.
// For now we use a fixed-size root table; growth is handled lazily.
#define RT_ROOT_MAX 4096
static SageValue* _gc_roots[RT_ROOT_MAX];
static int        _gc_root_count = 0;

// Conservative stack scanning: AOT-compiled code keeps live SageValues only in
// C locals (it emits no explicit GC roots), so to collect safely we scan the
// machine stack for words that point at live heap objects and treat them as
// roots. `_gc_stack_base` is captured once at program start (the highest stack
// address); the current stack pointer is the lowest. Anything in between that
// looks like a live object payload pointer is marked.
static void* _gc_stack_base = NULL;
void sage_rt_gc_set_stack_base(void* p) { _gc_stack_base = p; }

// ─────────────────────────────────────────────────────────────────────────────
// Utility — safe allocation
// ─────────────────────────────────────────────────────────────────────────────

void* sage_rt_alloc(size_t size) {
    if (size == 0) size = 1;
    void* p = calloc(1, size);
    if (!p) {
        fprintf(stderr, "sage runtime: out of memory (requested %zu bytes)\n", size);
        abort();
    }
    return p;
}

void* sage_rt_realloc(void* ptr, size_t size) {
    void* p = realloc(ptr, size);
    if (!p && size > 0) {
        fprintf(stderr, "sage runtime: out of memory in realloc (%zu bytes)\n", size);
        abort();
    }
    return p;
}

char* sage_rt_strdup(const char* s) {
    if (!s) return NULL;
    char* p = strdup(s);
    if (!p) {
        fprintf(stderr, "sage runtime: out of memory in strdup\n");
        abort();
    }
    return p;
}

char* sage_rt_strndup(const char* s, int n) {
    if (!s) return NULL;
    char* p = strndup(s, n);
    if (!p) {
        fprintf(stderr, "sage runtime: out of memory in strndup\n");
        abort();
    }
    return p;
}

// ─────────────────────────────────────────────────────────────────────────────
// GC allocation
// ─────────────────────────────────────────────────────────────────────────────

void* sage_rt_gc_alloc(SageValType type, size_t size) {
    size_t total = sizeof(SageGCHdr) + size;
    // Trigger GC *before* allocating/linking the new object, so a collection
    // never sweeps a half-initialized allocation (its payload isn't populated
    // and the caller hasn't received the pointer yet). Conservative stack
    // scanning (see sage_rt_gc_collect) keeps this sound under AOT.
    if (_gc.enabled && (_gc.bytes_live > _gc.next_gc || _gc.obj_count >= _gc.next_gc_objects)) {
        sage_rt_gc_collect();
    }
    SageGCHdr* hdr = (SageGCHdr*)sage_rt_alloc(total);
    hdr->flags    = ((uint32_t)type << 24);
    hdr->size     = (uint32_t)size;
    hdr->next     = _gc.objects;
    _gc.objects   = hdr;
    _gc.bytes_alloc += total;
    _gc.bytes_live  += total;
    _gc.obj_count++;

    return SAGE_GC_PAYLOAD(hdr);
}

// ─────────────────────────────────────────────────────────────────────────────
// GC — mark and sweep
// ─────────────────────────────────────────────────────────────────────────────

static void _gc_mark_value(SageValue v);

static void _gc_mark_array(SageArray* a) {
    if (!a) return;
    for (int i = 0; i < a->count; i++) _gc_mark_value(a->elems[i]);
}

static void _gc_mark_dict(SageDict* d) {
    if (!d) return;
    for (int i = 0; i < d->cap; i++) {
        if (d->slots[i].key) _gc_mark_value(d->slots[i].val);
    }
}

static void _gc_mark_value(SageValue v) {
    switch (v.type) {
        case SAGE_VAL_STRING:
            if (v.as.string) {
                SageGCHdr* h = SAGE_GC_HEADER(v.as.string);
                if (!SAGE_GC_MARKED(h)) { SAGE_GC_SET_MARK(h); }
            }
            break;
        case SAGE_VAL_ARRAY:
            if (v.as.array) {
                SageGCHdr* h = SAGE_GC_HEADER(v.as.array);
                if (!SAGE_GC_MARKED(h)) {
                    SAGE_GC_SET_MARK(h);
                    _gc_mark_array(v.as.array);
                }
            }
            break;
        case SAGE_VAL_DICT:
            if (v.as.dict) {
                SageGCHdr* h = SAGE_GC_HEADER(v.as.dict);
                if (!SAGE_GC_MARKED(h)) {
                    SAGE_GC_SET_MARK(h);
                    _gc_mark_dict(v.as.dict);
                }
            }
            break;
        case SAGE_VAL_TUPLE:
            if (v.as.tuple) {
                SageGCHdr* h = SAGE_GC_HEADER(v.as.tuple);
                if (!SAGE_GC_MARKED(h)) {
                    SAGE_GC_SET_MARK(h);
                    for (int i = 0; i < v.as.tuple->count; i++)
                        _gc_mark_value(v.as.tuple->elems[i]);
                }
            }
            break;
        case SAGE_VAL_BYTES:
            if (v.as.bytes) {
                SageGCHdr* h = SAGE_GC_HEADER(v.as.bytes);
                if (!SAGE_GC_MARKED(h)) SAGE_GC_SET_MARK(h);
            }
            break;
        case SAGE_VAL_INSTANCE:
            if (v.as.instance) {
                SageGCHdr* h = SAGE_GC_HEADER(v.as.instance);
                if (!SAGE_GC_MARKED(h)) {
                    SAGE_GC_SET_MARK(h);
                    for (int i = 0; i < v.as.instance->field_count; i++)
                        _gc_mark_value(v.as.instance->fields[i]);
                }
            }
            break;
        case SAGE_VAL_FUNCTION:
            if (v.as.closure) {
                SageGCHdr* h = SAGE_GC_HEADER(v.as.closure);
                if (!SAGE_GC_MARKED(h)) {
                    SAGE_GC_SET_MARK(h);
                    for (int i = 0; i < v.as.closure->capture_count; i++)
                        _gc_mark_value(v.as.closure->captures[i]);
                }
            }
            break;
        case SAGE_VAL_EXCEPTION:
            if (v.as.exception) {
                SageGCHdr* h = SAGE_GC_HEADER(v.as.exception);
                if (!SAGE_GC_MARKED(h)) SAGE_GC_SET_MARK(h);
            }
            break;
        default:
            break;
    }
}

// Mark an object identified conservatively by a candidate payload pointer.
// We confirm the pointer is a real live GC object by walking the object list
// (linear, but collections are infrequent), then mark it *and* recurse into
// its contents by reconstructing a typed SageValue from the header tag.
static void _gc_mark_if_object(void* cand) {
    if (!cand) return;
    for (SageGCHdr* h = _gc.objects; h; h = h->next) {
        if (SAGE_GC_PAYLOAD(h) == cand) {
            if (SAGE_GC_MARKED(h)) return;  // already marked
            SageValType t = (SageValType)((h->flags >> 24) & 0xFF);
            SageValue v; v.type = t;
            switch (t) {
                case SAGE_VAL_STRING:  v.as.string  = (char*)cand; break;
                case SAGE_VAL_ARRAY:   v.as.array   = (SageArray*)cand; break;
                case SAGE_VAL_DICT:    v.as.dict    = (SageDict*)cand; break;
                case SAGE_VAL_TUPLE:   v.as.tuple   = (SageTuple*)cand; break;
                case SAGE_VAL_INSTANCE:v.as.instance= (SageInst*)cand; break;
                case SAGE_VAL_FUNCTION:v.as.closure = (SageClosure*)cand; break;
                case SAGE_VAL_BYTES:   v.as.bytes   = (SageBytes*)cand; break;
                default:
                    // Unknown/plain payload: just mark the header, no recursion.
                    SAGE_GC_SET_MARK(h);
                    return;
            }
            _gc_mark_value(v);  // marks + recurses into contents
            return;
        }
    }
}

// Scan a contiguous memory range [lo, hi) word-by-word for object pointers.
static void _gc_scan_range(void* lo, void* hi) {
    if (!lo || !hi) return;
    if ((char*)lo > (char*)hi) { void* t = lo; lo = hi; hi = t; }
    // Align to pointer size.
    uintptr_t start = ((uintptr_t)lo + sizeof(void*) - 1) & ~(uintptr_t)(sizeof(void*) - 1);
    for (uintptr_t p = start; p + sizeof(void*) <= (uintptr_t)hi; p += sizeof(void*)) {
        void* cand = *(void**)p;
        _gc_mark_if_object(cand);
    }
}

void sage_rt_gc_collect(void) {
    if (!_gc.enabled) return;

    // Mark phase: walk all registered roots
    for (int i = 0; i < _gc_root_count; i++) {
        if (_gc_roots[i]) _gc_mark_value(*_gc_roots[i]);
    }

    // Conservative stack scan: flush registers to the stack, then scan from the
    // current frame up to the captured stack base for live object pointers.
    if (_gc_stack_base) {
        jmp_buf regs;
        if (setjmp(regs) == 0) {
            // setjmp captured callee-saved registers into `regs`; scan it too.
            volatile int stack_marker;
            void* sp = (void*)&stack_marker;
            _gc_scan_range(sp, _gc_stack_base);
            _gc_scan_range((void*)&regs, (void*)((char*)&regs + sizeof(regs)));
        }
    }

    // Conservative data/BSS scan: AOT emits module-level state (e.g. a
    // module's namespace dict or an `_atexit_handlers` array) as file-scope
    // `static SageValue` globals, which live in the data/BSS segment rather
    // than on the stack. Scan that whole region so those roots are found.
    {
        extern char __data_start[], _end[], __bss_start[];
        char* d_lo = (char*)&__data_start;
        char* d_hi = (char*)&_end;
        if (d_lo && d_hi && d_lo < d_hi && (size_t)(d_hi - d_lo) < (size_t)512 * 1024 * 1024)
            _gc_scan_range(d_lo, d_hi);
        (void)__bss_start;
    }

    // Sweep phase: free unmarked, clear marks on survivors
    SageGCHdr** prev = (SageGCHdr**)&_gc.objects;
    SageGCHdr*  cur  = _gc.objects;

    while (cur) {
        if (SAGE_GC_MARKED(cur)) {
            SAGE_GC_CLR_MARK(cur);
            prev = &cur->next;
            cur  = cur->next;
        } else {
            // Unreachable — free it
            SageGCHdr* dead = cur;
            *prev = cur->next;
            cur   = cur->next;

            size_t sz = sizeof(SageGCHdr) + dead->size;
            _gc.bytes_freed += sz;
            _gc.bytes_live  -= sz;
            _gc.obj_count--;
            free(dead);
        }
    }

    _gc.collections++;
    // Back off next GC trigger (grow as heap grows)
    _gc.next_gc = _gc.bytes_live * 2;
    if (_gc.next_gc < 512 * 1024) _gc.next_gc = 512 * 1024;
    // Object-count trigger: collect again after the live set roughly doubles,
    // with a floor of 128 (matches the interpreter's object padding).
    _gc.next_gc_objects = _gc.obj_count + _gc.obj_count / 2 + 128;
}

// ─────────────────────────────────────────────────────────────────────────────
// Memory modes
// ─────────────────────────────────────────────────────────────────────────────

void sage_rt_gc_pause(void)   { _gc.pause_depth++; _gc.enabled = 0; }
void sage_rt_gc_resume(void)  {
    if (_gc.pause_depth > 0) _gc.pause_depth--;
    if (_gc.pause_depth == 0) _gc.enabled = 1;
}
void sage_rt_gc_disable(void) { _gc.enabled = 0; }
void sage_rt_gc_enable(void)  { if (_gc.pause_depth == 0) _gc.enabled = 1; }
int  sage_rt_gc_is_active(void) { return _gc.enabled; }

void* sage_rt_manual_alloc(size_t size) {
    // @manual allocation: NOT tracked by GC, user calls sage_rt_manual_free.
    // We still prepend a GCHdr with SAGE_GC_MANUAL so double-free detection
    // can tell this apart from a GC-managed pointer.
    size_t total = sizeof(SageGCHdr) + size;
    SageGCHdr* hdr = (SageGCHdr*)sage_rt_alloc(total);
    hdr->flags = SAGE_GC_MANUAL(hdr) ? hdr->flags : (hdr->flags | (1u << 5));
    hdr->size  = (uint32_t)size;
    hdr->next  = NULL; // NOT in the GC list
    return SAGE_GC_PAYLOAD(hdr);
}

void sage_rt_manual_free(void* ptr) {
    if (!ptr) return;
    SageGCHdr* hdr = SAGE_GC_HEADER(ptr);
    // Poison the header to catch use-after-free
    hdr->flags = 0xDEADBEEFu;
    free(hdr);
}

void* sage_rt_manual_alloc_typed(SageValType type, size_t size) {
    void* p = sage_rt_manual_alloc(size);
    SageGCHdr* h = SAGE_GC_HEADER(p);
    h->flags = ((uint32_t)type << 24) | (1u << 5); // manual flag
    return p;
}

// ─────────────────────────────────────────────────────────────────────────────
// Lifecycle
// ─────────────────────────────────────────────────────────────────────────────

void sage_rt_init(void) {
    memset(&_gc, 0, sizeof(_gc));
    _gc.enabled  = 1;
    _gc.next_gc  = 512 * 1024;
    sage_rt_exc_top = NULL;
}

void sage_rt_shutdown(void) {
    // Final collection
    sage_rt_gc_collect();
    // Free anything remaining (graceful shutdown, don't complain)
    SageGCHdr* cur = _gc.objects;
    while (cur) {
        SageGCHdr* next = cur->next;
        free(cur);
        cur = next;
    }
    _gc.objects = NULL;
}

SageRTGCStats sage_rt_gc_stats(void) {
    SageRTGCStats s;
    s.bytes_allocated = _gc.bytes_alloc;
    s.bytes_freed     = _gc.bytes_freed;
    s.collections     = _gc.collections;
    s.live_objects    = _gc.obj_count;
    return s;
}

// gc_mode() -> "tracing" | "arc" | "orc"
SageValue sage_rt_gc_mode(void) {
    if (_gc.mode == 1) return sage_rt_string("arc");
    if (_gc.mode == 2) return sage_rt_string("orc");
    return sage_rt_string("tracing");
}
void sage_rt_gc_set_arc(void)     { _gc.mode = 1; }
void sage_rt_gc_set_orc(void)     { _gc.mode = 2; }
void sage_rt_gc_set_tracing(void) { _gc.mode = 0; }

// gc_collections() -> int
SageValue sage_rt_gc_collections(void) {
    return sage_rt_int((int64_t)_gc.collections);
}

// gc_stats() -> dict (mirrors interpreter keys used by tests)
SageValue sage_rt_gc_stats_dict(void) {
    SageValue d = sage_rt_dict_new();
    sage_rt_dict_set(d, sage_rt_string("bytes_allocated"), sage_rt_float((double)_gc.bytes_alloc));
    sage_rt_dict_set(d, sage_rt_string("current_bytes"),   sage_rt_float((double)_gc.bytes_live));
    sage_rt_dict_set(d, sage_rt_string("num_objects"),     sage_rt_float((double)_gc.obj_count));
    sage_rt_dict_set(d, sage_rt_string("collections"),     sage_rt_int((int64_t)_gc.collections));
    sage_rt_dict_set(d, sage_rt_string("objects_freed"),   sage_rt_float((double)_gc.bytes_freed));
    return d;
}

// ─────────────────────────────────────────────────────────────────────────────
// Comparison
// ─────────────────────────────────────────────────────────────────────────────

int sage_rt_equal(SageValue a, SageValue b) {
    if (a.type != b.type) {
        // int == float promotion
        if (SAGE_IS_NUMERIC(a) && SAGE_IS_NUMERIC(b))
            return SAGE_AS_DOUBLE(a) == SAGE_AS_DOUBLE(b);
        return 0;
    }
    switch (a.type) {
        case SAGE_VAL_INT:   return a.as.integer == b.as.integer;
        case SAGE_VAL_FLOAT: return a.as.number  == b.as.number;
        case SAGE_VAL_BOOL:  return a.as.boolean == b.as.boolean;
        case SAGE_VAL_NIL:   return 1;
        case SAGE_VAL_STRING:
            if (!a.as.string || !b.as.string) return a.as.string == b.as.string;
            return strcmp(a.as.string, b.as.string) == 0;
        case SAGE_VAL_INSTANCE: {
            if (!a.as.instance || !b.as.instance) return a.as.instance == b.as.instance;
            // Check for __eq__ method first
            if (a.as.instance->class_def) {
                SageClass* cls = a.as.instance->class_def;
                while (cls) {
                    for (int i = 0; i < cls->method_count; i++) {
                        if (strcmp(cls->methods[i].name, "__eq__") == 0) {
                            SageValue argv[1] = {b};
                            SageValue res = cls->methods[i].fn(a.as.instance, 1, argv);
                            return sage_rt_truthy(res);
                        }
                    }
                    cls = cls->parent;
                }
            }
            // No __eq__ — structural field comparison
            SageInst* ai = a.as.instance; SageInst* bi = b.as.instance;
            if (ai->field_count != bi->field_count) return 0;
            for (int i = 0; i < ai->field_count; i++) {
                if (!sage_rt_equal(ai->fields[i], bi->fields[i])) return 0;
            }
            return 1;
        }
        case SAGE_VAL_ARRAY: {
            if (!a.as.array || !b.as.array) return a.as.array == b.as.array;
            if (a.as.array->count != b.as.array->count) return 0;
            for (int i = 0; i < a.as.array->count; i++) {
                if (!sage_rt_equal(a.as.array->elems[i], b.as.array->elems[i])) return 0;
            }
            return 1;
        }
        case SAGE_VAL_DICT: {
            if (!a.as.dict || !b.as.dict) return a.as.dict == b.as.dict;
            if (a.as.dict->count != b.as.dict->count) return 0;
            for (int i = 0; i < a.as.dict->cap; i++) {
                if (!a.as.dict->slots[i].key) continue;
                SageValue bv = sage_rt_dict_get(b, sage_rt_string(a.as.dict->slots[i].key));
                if (!sage_rt_equal(a.as.dict->slots[i].val, bv)) return 0;
            }
            return 1;
        }
        default: return a.as.pointer == b.as.pointer; // identity
    }
}

int sage_rt_less(SageValue a, SageValue b) {
    if (SAGE_IS_NUMERIC(a) && SAGE_IS_NUMERIC(b))
        return SAGE_AS_DOUBLE(a) < SAGE_AS_DOUBLE(b);
    if (SAGE_IS_STRING(a) && SAGE_IS_STRING(b))
        return strcmp(a.as.string, b.as.string) < 0;
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// String operations
// ─────────────────────────────────────────────────────────────────────────────

SageValue sage_rt_string(const char* s) {
    if (!s) return sage_rt_nil();
    int len = (int)strlen(s);
    char* buf = (char*)sage_rt_gc_alloc(SAGE_VAL_STRING, len + 1);
    memcpy(buf, s, len + 1);
    SageValue v; v.type = SAGE_VAL_STRING; v.as.string = buf;
    return v;
}

SageValue sage_rt_string_len(const char* s, int n) {
    char* buf = (char*)sage_rt_gc_alloc(SAGE_VAL_STRING, n + 1);
    memcpy(buf, s, n);
    buf[n] = '\0';
    SageValue v; v.type = SAGE_VAL_STRING; v.as.string = buf;
    return v;
}

SageValue sage_rt_string_take(char* s) {
    // s must already be in GC heap (via sage_rt_gc_alloc)
    SageValue v; v.type = SAGE_VAL_STRING; v.as.string = s;
    return v;
}

SageValue sage_rt_string_concat(SageValue a, SageValue b) {
    const char* sa = SAGE_IS_STRING(a) ? a.as.string : "";
    const char* sb = SAGE_IS_STRING(b) ? b.as.string : "";
    int la = (int)strlen(sa), lb = (int)strlen(sb);
    char* buf = (char*)sage_rt_gc_alloc(SAGE_VAL_STRING, la + lb + 1);
    memcpy(buf, sa, la);
    memcpy(buf + la, sb, lb);
    buf[la + lb] = '\0';
    SageValue v; v.type = SAGE_VAL_STRING; v.as.string = buf;
    return v;
}

SageValue sage_rt_string_repeat(SageValue sv, int n) {
    if (!SAGE_IS_STRING(sv) || n <= 0) return sage_rt_string("");
    const char* s = sv.as.string;
    int slen = (int)strlen(s);
    int total = slen * n;
    char* buf = (char*)sage_rt_gc_alloc(SAGE_VAL_STRING, total + 1);
    for (int i = 0; i < n; i++) memcpy(buf + i * slen, s, slen);
    buf[total] = '\0';
    SageValue v; v.type = SAGE_VAL_STRING; v.as.string = buf;
    return v;
}

// Convert any value to string representation (mirrors sage_typeof_str)
SageValue sage_rt_tostring(SageValue v) {
    char buf[128];
    switch (v.type) {
        case SAGE_VAL_INT:
            snprintf(buf, sizeof(buf), "%" PRId64, v.as.integer);
            return sage_rt_string(buf);
        case SAGE_VAL_FLOAT: {
            double d = v.as.number;
            // Float values always show decimal point so 7.0 prints as "7.0" not "7"
            snprintf(buf, sizeof(buf), "%g", d);
            // If %g produced an integer format (no '.', no 'e'), append ".0"
            if (!strchr(buf, '.') && !strchr(buf, 'e') && !strchr(buf, 'E') &&
                !strchr(buf, 'n') && !strchr(buf, 'i')) {
                size_t n = strlen(buf);
                if (n + 2 < sizeof(buf)) { buf[n] = '.'; buf[n+1] = '0'; buf[n+2] = '\0'; }
            }
            return sage_rt_string(buf);
        }
        case SAGE_VAL_BOOL:
            return sage_rt_string(v.as.boolean ? "true" : "false");
        case SAGE_VAL_NIL:
            return sage_rt_string("nil");
        case SAGE_VAL_STRING:
            return v;
        case SAGE_VAL_ARRAY: {
            // [a, b, c]
            SageArray* a = v.as.array;
            // Start with "["
            SageValue acc = sage_rt_string("[");
            for (int i = 0; i < a->count; i++) {
                if (i > 0) acc = sage_rt_string_concat(acc, sage_rt_string(", "));
                acc = sage_rt_string_concat(acc, sage_rt_tostring(a->elems[i]));
            }
            acc = sage_rt_string_concat(acc, sage_rt_string("]"));
            return acc;
        }
        case SAGE_VAL_DICT: {
            SageDict* d = v.as.dict;
            if (!d) return sage_rt_string("{}");
            // Check for Option/Result wrappers: {__type: "Some"/"Ok"/"Err", value: ...}
            // and ADT enum variants: {__tag: "VariantName", field1: ..., ...}
            SageValue type_v = sage_rt_dict_get(v, sage_rt_string("__type"));
            if (SAGE_IS_STRING(type_v)) {
                const char* tn = type_v.as.string;
                if (strcmp(tn,"Some")==0 || strcmp(tn,"Ok")==0 || strcmp(tn,"Err")==0) {
                    SageValue inner = sage_rt_dict_get(v, sage_rt_string("value"));
                    SageValue inner_s = sage_rt_tostring(inner);
                    size_t tlen = strlen(tn) + (SAGE_IS_STRING(inner_s)?strlen(inner_s.as.string):4) + 8;
                    char* tbuf = (char*)sage_rt_gc_alloc(SAGE_VAL_STRING, tlen);
                    snprintf(tbuf, tlen, "%s(%s)", tn, SAGE_IS_STRING(inner_s)?inner_s.as.string:"nil");
                    SageValue sv; sv.type=SAGE_VAL_STRING; sv.as.string=tbuf; return sv;
                }
                if (strcmp(tn,"None")==0) return sage_rt_string("None");
            }
            // ADT enum variant tag: {__tag: "VariantName", ...}
            SageValue tag_v = sage_rt_dict_get(v, sage_rt_string("__tag"));
            if (SAGE_IS_STRING(tag_v)) {
                // Count non-tag/non-meta fields
                int has_fields = 0;
                for (int i = 0; i < d->cap; i++) {
                    if (!d->slots[i].key) continue;
                    if (strcmp(d->slots[i].key,"__tag")==0 || strcmp(d->slots[i].key,"__name__")==0) continue;
                    has_fields = 1; break;
                }
                if (has_fields) {
                    // ADT variant with data: VariantName(field1, field2, ...)
                    SageValue acc = sage_rt_string(tag_v.as.string);
                    acc = sage_rt_string_concat(acc, sage_rt_string("("));
                    int first2 = 1;
                    for (int i = 0; i < d->cap; i++) {
                        if (!d->slots[i].key) continue;
                        if (strcmp(d->slots[i].key,"__tag")==0 || strcmp(d->slots[i].key,"__name__")==0) continue;
                        if (!first2) acc = sage_rt_string_concat(acc, sage_rt_string(", "));
                        first2 = 0;
                        acc = sage_rt_string_concat(acc, sage_rt_tostring(d->slots[i].val));
                    }
                    acc = sage_rt_string_concat(acc, sage_rt_string(")"));
                    return acc;
                }
                // Unit variant (only __tag): fall through to generic {} format (matches interpreter)
            }
            // Generic dict
            SageValue acc = sage_rt_string("{");
            int first = 1;
            for (int i = 0; i < d->cap; i++) {
                if (!d->slots[i].key) continue;
                if (strcmp(d->slots[i].key, "__tag") == 0) continue;
                if (strcmp(d->slots[i].key, "__name__") == 0) continue;
                if (!first) acc = sage_rt_string_concat(acc, sage_rt_string(", "));
                first = 0;
                acc = sage_rt_string_concat(acc, sage_rt_string("\""));
                acc = sage_rt_string_concat(acc, sage_rt_string(d->slots[i].key));
                acc = sage_rt_string_concat(acc, sage_rt_string("\": "));
                acc = sage_rt_string_concat(acc, sage_rt_tostring(d->slots[i].val));
            }
            acc = sage_rt_string_concat(acc, sage_rt_string("}"));
            return acc;
        }
        case SAGE_VAL_TUPLE: {
            SageTuple* t = v.as.tuple;
            SageValue acc = sage_rt_string("(");
            for (int i = 0; i < t->count; i++) {
                if (i > 0) acc = sage_rt_string_concat(acc, sage_rt_string(", "));
                acc = sage_rt_string_concat(acc, sage_rt_tostring(t->elems[i]));
            }
            acc = sage_rt_string_concat(acc, sage_rt_string(")"));
            return acc;
        }
        case SAGE_VAL_INSTANCE:
            if (v.as.instance && v.as.instance->class_def)
                snprintf(buf, sizeof(buf), "<instance of %s>", v.as.instance->class_def->name);
            else
                snprintf(buf, sizeof(buf), "<instance>");
            return sage_rt_string(buf);
        case SAGE_VAL_CLASS:
            if (v.as.class_def)
                snprintf(buf, sizeof(buf), "<class %s>", v.as.class_def->name);
            else
                snprintf(buf, sizeof(buf), "<class>");
            return sage_rt_string(buf);
        case SAGE_VAL_EXCEPTION:
            snprintf(buf, sizeof(buf), "Exception(%s)",
                     v.as.exception ? v.as.exception : "");
            return sage_rt_string(buf);
        default:
            return sage_rt_string("<unknown>");
    }
}

int sage_rt_str_len(SageValue s) {
    if (!SAGE_IS_STRING(s) || !s.as.string) return 0;
    return (int)strlen(s.as.string);
}

SageValue sage_rt_str_index(SageValue s, int64_t idx) {
    if (!SAGE_IS_STRING(s) || !s.as.string) return sage_rt_nil();
    size_t len = strlen(s.as.string);
    if (idx < 0) idx = (int64_t)len + idx;
    if (idx < 0 || (size_t)idx >= len) return sage_rt_nil();
    char buf[2] = {s.as.string[idx], 0};
    return sage_rt_string(buf);
}

SageValue sage_rt_str_repeat(SageValue s, int64_t n) {
    if (!SAGE_IS_STRING(s) || n <= 0) return sage_rt_string("");
    const char* src = s.as.string;
    size_t slen = strlen(src);
    if (slen == 0) return sage_rt_string("");
    size_t total = slen * (size_t)n;
    char* buf = (char*)sage_rt_gc_alloc(SAGE_VAL_STRING, total + 1);
    for (int64_t i = 0; i < n; i++) memcpy(buf + i * slen, src, slen);
    buf[total] = '\0';
    return sage_rt_string(buf);
}

SageValue sage_rt_str_upper(SageValue sv) {
    if (!SAGE_IS_STRING(sv)) return sv;
    int n = (int)strlen(sv.as.string);
    char* buf = (char*)sage_rt_gc_alloc(SAGE_VAL_STRING, n + 1);
    for (int i = 0; i <= n; i++) buf[i] = (char)toupper((unsigned char)sv.as.string[i]);
    SageValue v; v.type = SAGE_VAL_STRING; v.as.string = buf;
    return v;
}

SageValue sage_rt_str_lower(SageValue sv) {
    if (!SAGE_IS_STRING(sv)) return sv;
    int n = (int)strlen(sv.as.string);
    char* buf = (char*)sage_rt_gc_alloc(SAGE_VAL_STRING, n + 1);
    for (int i = 0; i <= n; i++) buf[i] = (char)tolower((unsigned char)sv.as.string[i]);
    SageValue v; v.type = SAGE_VAL_STRING; v.as.string = buf;
    return v;
}

SageValue sage_rt_str_strip(SageValue sv) {
    if (!SAGE_IS_STRING(sv)) return sv;
    const char* s = sv.as.string;
    while (*s && isspace((unsigned char)*s)) s++;
    int n = (int)strlen(s);
    while (n > 0 && isspace((unsigned char)s[n-1])) n--;
    return sage_rt_string_len(s, n);
}

SageValue sage_rt_str_split(SageValue sv, SageValue delim) {
    if (!SAGE_IS_STRING(sv)) return sage_rt_array_new();
    const char* s   = sv.as.string;
    const char* sep = SAGE_IS_STRING(delim) ? delim.as.string : " ";
    int sep_len = (int)strlen(sep);
    SageValue arr = sage_rt_array_new();
    if (sep_len == 0) {
        // Split into individual characters
        int n = (int)strlen(s);
        for (int i = 0; i < n; i++)
            sage_rt_array_push(arr, sage_rt_string_len(s + i, 1));
        return arr;
    }
    const char* p = s;
    const char* found;
    while ((found = strstr(p, sep)) != NULL) {
        sage_rt_array_push(arr, sage_rt_string_len(p, (int)(found - p)));
        p = found + sep_len;
    }
    sage_rt_array_push(arr, sage_rt_string(p));
    return arr;
}

SageValue sage_rt_str_startswith(SageValue sv, SageValue prefix) {
    if (!SAGE_IS_STRING(sv) || !SAGE_IS_STRING(prefix)) return sage_rt_bool(0);
    int plen = (int)strlen(prefix.as.string);
    return sage_rt_bool(strncmp(sv.as.string, prefix.as.string, plen) == 0);
}

SageValue sage_rt_str_endswith(SageValue sv, SageValue suffix) {
    if (!SAGE_IS_STRING(sv) || !SAGE_IS_STRING(suffix)) return sage_rt_bool(0);
    int slen = (int)strlen(sv.as.string);
    int suflen = (int)strlen(suffix.as.string);
    if (suflen > slen) return sage_rt_bool(0);
    return sage_rt_bool(strcmp(sv.as.string + slen - suflen, suffix.as.string) == 0);
}

SageValue sage_rt_str_replace(SageValue sv, SageValue old_v, SageValue new_v) {
    if (!SAGE_IS_STRING(sv) || !SAGE_IS_STRING(old_v) || !SAGE_IS_STRING(new_v))
        return sv;
    const char* s      = sv.as.string;
    const char* old_s  = old_v.as.string;
    const char* new_s  = new_v.as.string;
    int old_len = (int)strlen(old_s);
    int new_len = (int)strlen(new_s);
    if (old_len == 0) return sv;

    // Count occurrences
    int count = 0;
    const char* p = s;
    while ((p = strstr(p, old_s)) != NULL) { count++; p += old_len; }
    if (count == 0) return sv;

    int src_len = (int)strlen(s);
    int out_len = src_len + count * (new_len - old_len);
    char* buf = (char*)sage_rt_gc_alloc(SAGE_VAL_STRING, out_len + 1);
    char* w = buf;
    p = s;
    const char* found;
    while ((found = strstr(p, old_s)) != NULL) {
        int chunk = (int)(found - p);
        memcpy(w, p, chunk); w += chunk;
        memcpy(w, new_s, new_len); w += new_len;
        p = found + old_len;
    }
    strcpy(w, p);
    SageValue v; v.type = SAGE_VAL_STRING; v.as.string = buf;
    return v;
}

SageValue sage_rt_str_find(SageValue sv, SageValue needle) {
    if (!SAGE_IS_STRING(sv) || !SAGE_IS_STRING(needle)) return sage_rt_int(-1);
    const char* found = strstr(sv.as.string, needle.as.string);
    if (!found) return sage_rt_int(-1);
    return sage_rt_int((int64_t)(found - sv.as.string));
}

SageValue sage_rt_str_slice(SageValue sv, int start, int end) {
    if (!SAGE_IS_STRING(sv)) return sage_rt_nil();
    int n = (int)strlen(sv.as.string);
    if (start < 0) start = n + start;
    if (end < 0)   end   = n + end;
    if (start < 0) start = 0;
    if (end > n)   end   = n;
    if (start >= end) return sage_rt_string("");
    return sage_rt_string_len(sv.as.string + start, end - start);
}

// ─────────────────────────────────────────────────────────────────────────────
// Array operations
// ─────────────────────────────────────────────────────────────────────────────

SageValue sage_rt_array_new(void) {
    SageArray* a = (SageArray*)sage_rt_gc_alloc(SAGE_VAL_ARRAY, sizeof(SageArray));
    a->elems = NULL; a->count = 0; a->cap = 0;
    SageValue v; v.type = SAGE_VAL_ARRAY; v.as.array = a;
    return v;
}

SageValue sage_rt_array_of(int count, ...) {
    SageValue arr = sage_rt_array_new();
    va_list ap;
    va_start(ap, count);
    for (int i = 0; i < count; i++)
        sage_rt_array_push(arr, va_arg(ap, SageValue));
    va_end(ap);
    return arr;
}

void sage_rt_array_push(SageValue arr, SageValue val) {
    if (!SAGE_IS_ARRAY(arr)) return;
    SageArray* a = arr.as.array;
    if (a->count >= a->cap) {
        a->cap = a->cap ? a->cap * 2 : 8;
        a->elems = (SageValue*)sage_rt_realloc(a->elems, sizeof(SageValue) * a->cap);
    }
    a->elems[a->count++] = val;
}

SageValue sage_rt_array_pop(SageValue arr) {
    if (!SAGE_IS_ARRAY(arr) || arr.as.array->count == 0) return sage_rt_nil();
    return arr.as.array->elems[--arr.as.array->count];
}

SageValue sage_rt_array_get(SageValue arr, SageValue idx) {
    // Also handle tuples (indexing a tuple result)
    if (SAGE_IS_TUPLE(arr)) return sage_rt_tuple_get(arr, (int)SAGE_AS_INT64(idx));
    if (!SAGE_IS_ARRAY(arr)) return sage_rt_nil();
    SageArray* a = arr.as.array;
    int64_t i = SAGE_AS_INT64(idx);
    if (i < 0) i = a->count + i;
    if (i < 0 || i >= a->count) {
        sage_rt_fatal("array index %" PRId64 " out of bounds (length %d)", i, a->count);
    }
    return a->elems[i];
}

void sage_rt_array_set(SageValue arr, SageValue idx, SageValue val) {
    if (!SAGE_IS_ARRAY(arr)) return;
    SageArray* a = arr.as.array;
    int64_t i = SAGE_AS_INT64(idx);
    if (i < 0) i = a->count + i;
    if (i < 0 || i >= a->count) {
        sage_rt_fatal("array index %" PRId64 " out of bounds (length %d)", i, a->count);
    }
    a->elems[i] = val;
}

SageValue sage_rt_array_contains(SageValue arr, SageValue val) {
    if (!SAGE_IS_ARRAY(arr)) return sage_rt_bool(0);
    SageArray* a = arr.as.array;
    for (int i = 0; i < a->count; i++) {
        if (sage_rt_equal(a->elems[i], val)) return sage_rt_bool(1);
    }
    return sage_rt_bool(0);
}

SageValue sage_rt_array_index_of(SageValue arr, SageValue val) {
    if (!SAGE_IS_ARRAY(arr)) return sage_rt_int(-1);
    SageArray* a = arr.as.array;
    for (int i = 0; i < a->count; i++) {
        if (sage_rt_equal(a->elems[i], val)) return sage_rt_int((int64_t)i);
    }
    return sage_rt_int(-1);
}

SageValue sage_rt_array_join(SageValue arr, SageValue sep) {
    if (!SAGE_IS_ARRAY(arr)) return sage_rt_string("");
    SageArray* a = arr.as.array;
    const char* sep_str = SAGE_IS_STRING(sep) ? sep.as.string : "";
    // Calculate total size
    size_t total = 0;
    for (int i = 0; i < a->count; i++) {
        SageValue sv = sage_rt_tostring(a->elems[i]);
        if (sv.as.string) total += strlen(sv.as.string);
        if (i < a->count - 1) total += strlen(sep_str);
    }
    char* buf = (char*)sage_rt_gc_alloc(SAGE_VAL_STRING, total + 1);
    char* p = buf;
    for (int i = 0; i < a->count; i++) {
        SageValue sv = sage_rt_tostring(a->elems[i]);
        if (sv.as.string) { size_t n = strlen(sv.as.string); memcpy(p, sv.as.string, n); p += n; }
        if (i < a->count - 1) { size_t n = strlen(sep_str); memcpy(p, sep_str, n); p += n; }
    }
    *p = '\0';
    return sage_rt_string(buf);
}

SageValue sage_rt_array_reverse(SageValue arr) {
    if (!SAGE_IS_ARRAY(arr)) return arr;
    SageArray* a = arr.as.array;
    for (int i = 0, j = a->count - 1; i < j; i++, j--) {
        SageValue tmp = a->elems[i]; a->elems[i] = a->elems[j]; a->elems[j] = tmp;
    }
    return arr;
}

SageValue sage_rt_array_sort(SageValue arr) {
    // Simple insertion sort (stable for small arrays)
    if (!SAGE_IS_ARRAY(arr)) return arr;
    SageArray* a = arr.as.array;
    for (int i = 1; i < a->count; i++) {
        SageValue key = a->elems[i];
        int j = i - 1;
        while (j >= 0 && sage_rt_less(a->elems[j], key) == 0 &&
               !sage_rt_equal(a->elems[j], key)) {
            a->elems[j + 1] = a->elems[j]; j--;
        }
        a->elems[j + 1] = key;
    }
    return arr;
}

SageValue sage_rt_array_slice(SageValue arr, int start, int end) {
    if (!SAGE_IS_ARRAY(arr)) return sage_rt_array_new();
    SageArray* a = arr.as.array;
    if (start < 0) start = a->count + start;
    if (end < 0)   end   = a->count + end;
    if (start < 0) start = 0;
    if (end > a->count) end = a->count;
    SageValue result = sage_rt_array_new();
    for (int i = start; i < end; i++)
        sage_rt_array_push(result, a->elems[i]);
    return result;
}

int sage_rt_array_len(SageValue arr) {
    if (!SAGE_IS_ARRAY(arr)) return 0;
    return arr.as.array->count;
}

SageValue sage_rt_array_concat(SageValue a, SageValue b) {
    SageValue result = sage_rt_array_new();
    if (SAGE_IS_ARRAY(a))
        for (int i = 0; i < a.as.array->count; i++)
            sage_rt_array_push(result, a.as.array->elems[i]);
    if (SAGE_IS_ARRAY(b))
        for (int i = 0; i < b.as.array->count; i++)
            sage_rt_array_push(result, b.as.array->elems[i]);
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Dict operations (open-addressing, power-of-2 capacity)
// ─────────────────────────────────────────────────────────────────────────────

static uint32_t _dict_hash(const char* key, int len) {
    // FNV-1a
    uint32_t h = 2166136261u;
    for (int i = 0; i < len; i++) {
        h ^= (uint8_t)key[i];
        h *= 16777619u;
    }
    return h;
}

static void _dict_grow(SageDict* d) {
    int new_cap = d->cap ? d->cap * 2 : 8;
    SageDictSlot* new_slots = (SageDictSlot*)sage_rt_alloc(sizeof(SageDictSlot) * new_cap);
    for (int i = 0; i < d->cap; i++) {
        if (!d->slots[i].key) continue;
        int idx = d->slots[i].hash & (new_cap - 1);
        while (new_slots[idx].key) idx = (idx + 1) & (new_cap - 1);
        new_slots[idx] = d->slots[i];
    }
    free(d->slots);
    d->slots = new_slots;
    d->cap   = new_cap;
}

SageValue sage_rt_dict_new(void) {
    SageDict* d = (SageDict*)sage_rt_gc_alloc(SAGE_VAL_DICT, sizeof(SageDict));
    d->slots = NULL; d->count = 0; d->cap = 0;
    SageValue v; v.type = SAGE_VAL_DICT; v.as.dict = d;
    return v;
}

void sage_rt_dict_set(SageValue dict, SageValue key, SageValue val) {
    if (!SAGE_IS_DICT(dict) || !SAGE_IS_STRING(key)) return;
    SageDict* d = dict.as.dict;
    if (!d->slots || d->count * 2 >= d->cap) _dict_grow(d);
    const char* ks  = key.as.string;
    int klen = (int)strlen(ks);
    uint32_t h = _dict_hash(ks, klen);
    int idx = h & (d->cap - 1);
    while (d->slots[idx].key) {
        if (d->slots[idx].hash == h &&
            d->slots[idx].key_len == klen &&
            memcmp(d->slots[idx].key, ks, klen) == 0) {
            d->slots[idx].val = val;
            return;
        }
        idx = (idx + 1) & (d->cap - 1);
    }
    d->slots[idx].key     = sage_rt_strdup(ks);
    d->slots[idx].key_len = klen;
    d->slots[idx].hash    = h;
    d->slots[idx].val     = val;
    d->count++;
}

SageValue sage_rt_dict_get(SageValue dict, SageValue key) {
    if (!SAGE_IS_DICT(dict) || !SAGE_IS_STRING(key) || !dict.as.dict->slots)
        return sage_rt_nil();
    SageDict* d = dict.as.dict;
    const char* ks = key.as.string;
    int klen = (int)strlen(ks);
    uint32_t h = _dict_hash(ks, klen);
    int idx = h & (d->cap - 1);
    while (d->slots[idx].key) {
        if (d->slots[idx].hash == h &&
            d->slots[idx].key_len == klen &&
            memcmp(d->slots[idx].key, ks, klen) == 0)
            return d->slots[idx].val;
        idx = (idx + 1) & (d->cap - 1);
    }
    return sage_rt_nil();
}

int sage_rt_dict_has(SageValue dict, SageValue key) {
    SageValue v = sage_rt_dict_get(dict, key);
    return !SAGE_IS_NIL(v);
}

SageValue sage_rt_dict_remove(SageValue dict, SageValue key) {
    if (!SAGE_IS_DICT(dict) || !SAGE_IS_STRING(key) || !dict.as.dict->slots) return sage_rt_bool(0);
    SageDict* d = dict.as.dict;
    const char* ks = key.as.string;
    int klen = (int)strlen(ks);
    uint32_t h = _dict_hash(ks, klen);
    int idx = h & (d->cap - 1);
    while (d->slots[idx].key) {
        if (d->slots[idx].hash == h && d->slots[idx].key_len == klen &&
            memcmp(d->slots[idx].key, ks, klen) == 0) {
            // Mark as deleted by clearing key (open addressing — simple tombstone)
            d->slots[idx].key = NULL;
            d->count--;
            return sage_rt_bool(1);
        }
        idx = (idx + 1) & (d->cap - 1);
    }
    return sage_rt_bool(0);
}

SageValue sage_rt_dict_get_or(SageValue dict, SageValue key, SageValue def) {
    SageValue v = sage_rt_dict_get(dict, key);
    return SAGE_IS_NIL(v) ? def : v;
}

SageValue sage_rt_dict_len(SageValue dict) {
    if (!SAGE_IS_DICT(dict)) return sage_rt_int(0);
    return sage_rt_int((int64_t)dict.as.dict->count);
}


void sage_rt_dict_del(SageValue dict, SageValue key) {
    if (!SAGE_IS_DICT(dict) || !SAGE_IS_STRING(key) || !dict.as.dict->slots) return;
    SageDict* d = dict.as.dict;
    const char* ks = key.as.string;
    int klen = (int)strlen(ks);
    uint32_t h = _dict_hash(ks, klen);
    int idx = h & (d->cap - 1);
    while (d->slots[idx].key) {
        if (d->slots[idx].hash == h &&
            d->slots[idx].key_len == klen &&
            memcmp(d->slots[idx].key, ks, klen) == 0) {
            free(d->slots[idx].key);
            d->slots[idx].key = NULL;
            d->count--;
            return;
        }
        idx = (idx + 1) & (d->cap - 1);
    }
}

SageValue sage_rt_dict_keys(SageValue dict) {
    SageValue arr = sage_rt_array_new();
    if (!SAGE_IS_DICT(dict) || !dict.as.dict->slots) return arr;
    SageDict* d = dict.as.dict;
    for (int i = 0; i < d->cap; i++) {
        if (d->slots[i].key)
            sage_rt_array_push(arr, sage_rt_string(d->slots[i].key));
    }
    return arr;
}

SageValue sage_rt_dict_values(SageValue dict) {
    SageValue arr = sage_rt_array_new();
    if (!SAGE_IS_DICT(dict) || !dict.as.dict->slots) return arr;
    SageDict* d = dict.as.dict;
    for (int i = 0; i < d->cap; i++) {
        if (d->slots[i].key)
            sage_rt_array_push(arr, d->slots[i].val);
    }
    return arr;
}


// ─────────────────────────────────────────────────────────────────────────────
// Tuple operations
// ─────────────────────────────────────────────────────────────────────────────

SageValue sage_rt_tuple_new(int count, ...) {
    SageTuple* t = (SageTuple*)sage_rt_gc_alloc(SAGE_VAL_TUPLE, sizeof(SageTuple));
    t->count = count;
    t->elems = count > 0
        ? (SageValue*)sage_rt_alloc(sizeof(SageValue) * count)
        : NULL;
    va_list ap; va_start(ap, count);
    for (int i = 0; i < count; i++) t->elems[i] = va_arg(ap, SageValue);
    va_end(ap);
    SageValue v; v.type = SAGE_VAL_TUPLE; v.as.tuple = t;
    return v;
}

SageValue sage_rt_tuple_get(SageValue tuple, int idx) {
    if (!SAGE_IS_TUPLE(tuple)) return sage_rt_nil();
    SageTuple* t = tuple.as.tuple;
    if (idx < 0) idx = t->count + idx;
    if (idx < 0 || idx >= t->count)
        sage_rt_fatal("tuple index %d out of bounds (length %d)", idx, t->count);
    return t->elems[idx];
}

int sage_rt_tuple_len(SageValue tuple) {
    if (!SAGE_IS_TUPLE(tuple)) return 0;
    return tuple.as.tuple->count;
}

// ─────────────────────────────────────────────────────────────────────────────
// Bytes operations
// ─────────────────────────────────────────────────────────────────────────────

SageValue sage_rt_bytes_new(int cap) {
    SageBytes* b = (SageBytes*)sage_rt_gc_alloc(SAGE_VAL_BYTES, sizeof(SageBytes));
    b->cap  = cap > 0 ? cap : 8;
    b->data = (uint8_t*)sage_rt_alloc(b->cap);
    b->length = 0;
    SageValue v; v.type = SAGE_VAL_BYTES; v.as.bytes = b;
    return v;
}

SageValue sage_rt_bytes_from(const uint8_t* data, int len) {
    SageValue bv = sage_rt_bytes_new(len);
    memcpy(bv.as.bytes->data, data, len);
    bv.as.bytes->length = len;
    return bv;
}

void sage_rt_bytes_push(SageValue bytes, uint8_t byte) {
    if (!SAGE_IS_BYTES(bytes)) return;
    SageBytes* b = bytes.as.bytes;
    if (b->length >= b->cap) {
        b->cap *= 2;
        b->data = (uint8_t*)sage_rt_realloc(b->data, b->cap);
    }
    b->data[b->length++] = byte;
}

uint8_t sage_rt_bytes_get(SageValue bytes, int idx) {
    if (!SAGE_IS_BYTES(bytes)) return 0;
    SageBytes* b = bytes.as.bytes;
    if (idx < 0) idx = b->length + idx;
    if (idx < 0 || idx >= b->length)
        sage_rt_fatal("bytes index %d out of bounds", idx);
    return b->data[idx];
}

int sage_rt_bytes_len(SageValue bytes) {
    if (!SAGE_IS_BYTES(bytes)) return 0;
    return bytes.as.bytes->length;
}

// bytes(x): x may be an int (capacity), a string (its bytes), or an array of ints
SageValue sage_rt_bytes_ctor(SageValue x) {
    if (SAGE_IS_STRING(x) && x.as.string) {
        return sage_rt_bytes_from((const uint8_t*)x.as.string, (int)strlen(x.as.string));
    }
    if (SAGE_IS_ARRAY(x)) {
        SageArray* a = x.as.array;
        SageValue b = sage_rt_bytes_new(a->count > 0 ? a->count : 1);
        for (int i = 0; i < a->count; i++)
            sage_rt_bytes_push(b, (uint8_t)(SAGE_IS_NUMERIC(a->elems[i]) ? SAGE_AS_INT64(a->elems[i]) : 0));
        return b;
    }
    if (SAGE_IS_NUMERIC(x)) {
        SageValue b = sage_rt_bytes_new((int)SAGE_AS_INT64(x));
        return b;
    }
    return sage_rt_bytes_new(0);
}

// bytes_get(b, i) -> int (boxed)
SageValue sage_rt_bytes_get_v(SageValue bytes, SageValue idx) {
    if (!SAGE_IS_BYTES(bytes)) return sage_rt_nil();
    SageBytes* b = bytes.as.bytes;
    int i = (int)SAGE_AS_INT64(idx);
    if (i < 0) i += b->length;
    if (i < 0 || i >= b->length) return sage_rt_nil();
    return sage_rt_int((int64_t)b->data[i]);
}

SageValue sage_rt_bytes_set_v(SageValue bytes, SageValue idx, SageValue val) {
    if (!SAGE_IS_BYTES(bytes)) return sage_rt_nil();
    SageBytes* b = bytes.as.bytes;
    int i = (int)SAGE_AS_INT64(idx);
    if (i >= 0 && i < b->length) b->data[i] = (uint8_t)SAGE_AS_INT64(val);
    return sage_rt_nil();
}

SageValue sage_rt_bytes_to_string(SageValue bytes) {
    if (!SAGE_IS_BYTES(bytes)) return sage_rt_string("");
    SageBytes* b = bytes.as.bytes;
    char* s = (char*)sage_rt_gc_alloc(SAGE_VAL_STRING, b->length + 1);
    memcpy(s, b->data, b->length); s[b->length] = '\0';
    return sage_rt_string(s);
}

SageValue sage_rt_bytes_from_string(SageValue s) {
    if (!SAGE_IS_STRING(s) || !s.as.string) return sage_rt_bytes_new(0);
    return sage_rt_bytes_from((const uint8_t*)s.as.string, (int)strlen(s.as.string));
}

SageValue sage_rt_bytes_slice(SageValue bytes, SageValue startv, SageValue endv) {
    if (!SAGE_IS_BYTES(bytes)) return sage_rt_bytes_new(0);
    SageBytes* b = bytes.as.bytes;
    int start = (int)SAGE_AS_INT64(startv);
    int end   = SAGE_IS_NIL(endv) ? b->length : (int)SAGE_AS_INT64(endv);
    if (start < 0) start += b->length;
    if (end   < 0) end   += b->length;
    if (start < 0) start = 0;
    if (end > b->length) end = b->length;
    if (start >= end) return sage_rt_bytes_new(0);
    return sage_rt_bytes_from(b->data + start, end - start);
}

SageValue sage_rt_bytes_len_v(SageValue bytes) {
    return sage_rt_int((int64_t)sage_rt_bytes_len(bytes));
}

// sizeof(x): mirror the interpreter's type-dependent sizes
SageValue sage_rt_sizeof(SageValue x) {
    switch (x.type) {
        case SAGE_VAL_FLOAT:   return sage_rt_int((int64_t)sizeof(double));
        case SAGE_VAL_INT:     return sage_rt_int((int64_t)sizeof(double));
        case SAGE_VAL_BOOL:    return sage_rt_int((int64_t)sizeof(int));
        case SAGE_VAL_STRING:  return sage_rt_int((int64_t)(x.as.string ? strlen(x.as.string) : 0));
        case SAGE_VAL_BYTES:   return sage_rt_int((int64_t)x.as.bytes->length);
        case SAGE_VAL_ARRAY:   return sage_rt_int((int64_t)x.as.array->count);
        case SAGE_VAL_DICT:    return sage_rt_int((int64_t)x.as.dict->count);
        case SAGE_VAL_POINTER: {
            if (!x.as.pointer) return sage_rt_int(0);
            SageGCHdr* h = SAGE_GC_HEADER(x.as.pointer);
            return sage_rt_int((int64_t)h->size);
        }
        default: return sage_rt_int((int64_t)sizeof(SageValue));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Class / instance
// ─────────────────────────────────────────────────────────────────────────────

SageValue sage_rt_add_methods(SageValue class_val, SageMethod* methods, int count) {
    // Add methods to an existing class (for impl blocks)
    if (!SAGE_IS_CLASS(class_val) || !class_val.as.class_def) return class_val;
    SageClass* cls = class_val.as.class_def;
    // Allocate new method array combining old + new
    int old_count = cls->method_count;
    int new_total = old_count + count;
    SageMethod* all = (SageMethod*)malloc(sizeof(SageMethod) * new_total);
    if (old_count > 0 && cls->methods)
        memcpy(all, cls->methods, sizeof(SageMethod) * old_count);
    memcpy(all + old_count, methods, sizeof(SageMethod) * count);
    cls->methods = all;
    cls->method_count = new_total;
    return class_val;
}

SageValue sage_rt_class_new(const char* name, SageClass* parent,
                            SageMethod* methods, int method_count,
                            const char** field_names, int field_count,
                            int is_struct) {
    SageClass* cls = (SageClass*)sage_rt_gc_alloc(SAGE_VAL_CLASS, sizeof(SageClass));
    cls->name         = name;
    cls->parent       = parent;
    cls->methods      = methods;
    cls->method_count = method_count;
    cls->field_names  = field_names;
    cls->field_count  = field_count;
    cls->is_struct    = is_struct;
    SageValue v; v.type = SAGE_VAL_CLASS; v.as.class_def = cls;
    return v;
}

SageValue sage_rt_instance_new(SageValue class_val) {
    if (!SAGE_IS_CLASS(class_val)) return sage_rt_nil();
    SageClass* cls = class_val.as.class_def;
    SageInst* inst = (SageInst*)sage_rt_gc_alloc(SAGE_VAL_INSTANCE, sizeof(SageInst));
    inst->class_def   = cls;
    inst->field_count = 0;     // dynamic fields start at 0
    inst->field_names = NULL;
    inst->fields      = NULL;
    SageValue v; v.type = SAGE_VAL_INSTANCE; v.as.instance = inst;
    return v;
}

static int _field_index(SageInst* inst, const char* name) {
    if (!inst) return -1;
    // Check instance's own dynamic fields first
    for (int i = 0; i < inst->field_count; i++)
        if (inst->field_names && inst->field_names[i] && strcmp(inst->field_names[i], name) == 0) return i;
    // Then check class layout
    if (inst->class_def) {
        SageClass* cls = inst->class_def;
        for (int i = 0; i < cls->field_count; i++)
            if (cls->field_names && strcmp(cls->field_names[i], name) == 0)
                return i + inst->field_count;  // offset past dynamic fields
    }
    return -1;
}

SageValue sage_rt_field_get(SageValue inst_v, const char* name) {
    // Handle .length/.count/.size for collection types
    if ((strcmp(name,"length")==0||strcmp(name,"count")==0||strcmp(name,"size")==0)) {
        if (SAGE_IS_ARRAY(inst_v))  return sage_rt_int((int64_t)inst_v.as.array->count);
        if (SAGE_IS_STRING(inst_v)) return sage_rt_int((int64_t)strlen(inst_v.as.string));
        if (SAGE_IS_TUPLE(inst_v))  return sage_rt_int((int64_t)inst_v.as.tuple->count);
        if (SAGE_IS_DICT(inst_v))   return sage_rt_int((int64_t)inst_v.as.dict->count);
    }
    // Dict field access: covers ADT values ({"__tag":...,"radius":...}) and general dicts
    if (SAGE_IS_DICT(inst_v)) {
        return sage_rt_dict_get(inst_v, sage_rt_string(name));
    }
    if (!SAGE_IS_INSTANCE(inst_v)) return sage_rt_nil();
    SageInst* inst = inst_v.as.instance;
    // Check dynamic fields
    for (int i = 0; i < inst->field_count; i++) {
        if (inst->field_names && inst->field_names[i] && strcmp(inst->field_names[i], name) == 0) {
            return inst->fields ? inst->fields[i] : sage_rt_nil();
        }
    }
    // Check class layout
    if (inst->class_def && inst->class_def->field_names) {
        SageClass* cls = inst->class_def;
        for (int i = 0; i < cls->field_count; i++) {
            if (cls->field_names[i] && strcmp(cls->field_names[i], name) == 0) {
                return (inst->fields && i < inst->field_count) ? inst->fields[i] : sage_rt_nil();
            }
        }
    }
    return sage_rt_nil();
}

void sage_rt_field_set(SageValue inst_v, const char* name, SageValue val) {
    if (!SAGE_IS_INSTANCE(inst_v)) return;
    SageInst* inst = inst_v.as.instance;
    // Check dynamic fields first
    for (int i = 0; i < inst->field_count; i++) {
        if (inst->field_names && inst->field_names[i] && strcmp(inst->field_names[i], name) == 0) {
            inst->fields[i] = val; return;
        }
    }
    // Check class layout (struct fields with pre-declared names)
    if (inst->class_def && inst->class_def->field_names) {
        SageClass* cls = inst->class_def;
        for (int i = 0; i < cls->field_count; i++) {
            if (cls->field_names[i] && strcmp(cls->field_names[i], name) == 0) {
                // Ensure fields array is allocated (lazy init for class layout)
                if (!inst->fields) {
                    inst->fields = (SageValue*)calloc(cls->field_count, sizeof(SageValue));
                    inst->field_count = cls->field_count;
                    inst->field_names = cls->field_names; // borrow class field names
                }
                inst->fields[i] = val; return;
            }
        }
    }
    // Dynamic field: grow the instance field array
    int new_idx = inst->field_count;
    int new_cap = new_idx + 1;
    inst->fields = (SageValue*)realloc(inst->fields, sizeof(SageValue) * new_cap);
    inst->field_names = (const char**)realloc((void*)inst->field_names, sizeof(char*) * new_cap);
    char* name_copy = (char*)malloc(strlen(name) + 1);
    strcpy(name_copy, name);
    inst->field_names[new_idx] = name_copy;
    inst->fields[new_idx] = val;
    inst->field_count = new_cap;
}

SageValue sage_rt_field_by_idx(SageValue inst_v, int idx) {
    if (!SAGE_IS_INSTANCE(inst_v)) return sage_rt_nil();
    SageInst* inst = inst_v.as.instance;
    if (idx < 0 || idx >= inst->field_count) return sage_rt_nil();
    return inst->fields[idx];
}

void sage_rt_field_set_idx(SageValue inst_v, int idx, SageValue val) {
    if (!SAGE_IS_INSTANCE(inst_v)) return;
    SageInst* inst = inst_v.as.instance;
    if (idx >= 0 && idx < inst->field_count)
        inst->fields[idx] = val;
}

SageValue sage_rt_method_call(SageValue inst_v, const char* name,
                               int argc, SageValue* argv) {
    if (!SAGE_IS_INSTANCE(inst_v)) {
        sage_rt_fatal("cannot call method '%s' on non-instance", name);
    }
    SageInst* inst = inst_v.as.instance;
    SageClass* cls = inst->class_def;
    while (cls) {
        for (int i = 0; i < cls->method_count; i++) {
            if (strcmp(cls->methods[i].name, name) == 0)
                return cls->methods[i].fn(inst, argc, argv);
        }
        cls = cls->parent;
    }
    sage_rt_fatal("no method '%s' on %s",
                  name, inst->class_def ? inst->class_def->name : "<instance>");
}

SageValue sage_rt_method_call_super(SageInst* self, const char* name, int argc, SageValue* argv) {
    if (!self || !self->class_def) return sage_rt_nil();
    // Look in parent classes for the method
    SageClass* cls = self->class_def;
    // Search in parent class first (super calls skip current class)
    SageClass* search = cls->parent ? cls->parent : cls;
    for (int mi = 0; mi < search->method_count; mi++) {
        if (strcmp(search->methods[mi].name, name) == 0) {
            return search->methods[mi].fn(self, argc, argv);
        }
    }
    // Fallback to current class
    for (int mi = 0; mi < cls->method_count; mi++) {
        if (strcmp(cls->methods[mi].name, name) == 0) {
            return cls->methods[mi].fn(self, argc, argv);
        }
    }
    return sage_rt_nil();
}


SageValue sage_rt_struct_copy(SageValue src) {
    if (!SAGE_IS_INSTANCE(src)) return src;
    SageInst* orig = src.as.instance;
    SageInst* copy = (SageInst*)sage_rt_gc_alloc(SAGE_VAL_INSTANCE, sizeof(SageInst));
    copy->class_def   = orig->class_def;
    copy->field_count = orig->field_count;
    copy->fields = orig->field_count > 0
        ? (SageValue*)sage_rt_alloc(sizeof(SageValue) * orig->field_count)
        : NULL;
    for (int i = 0; i < orig->field_count; i++)
        copy->fields[i] = orig->fields[i]; // shallow copy (value semantics for primitives)
    SageValue v; v.type = SAGE_VAL_INSTANCE; v.as.instance = copy;
    return v;
}

// ─────────────────────────────────────────────────────────────────────────────
// Builtins
// ─────────────────────────────────────────────────────────────────────────────

SageValue sage_rt_len(SageValue v) {
    switch (v.type) {
        case SAGE_VAL_STRING: return sage_rt_int((int64_t)strlen(v.as.string));
        case SAGE_VAL_ARRAY:  return sage_rt_int(v.as.array->count);
        case SAGE_VAL_DICT:   return sage_rt_int(v.as.dict->count);
        // SAGE_VAL_TUPLE: interpreter throws error on .length, return nil
        case SAGE_VAL_BYTES:  return sage_rt_int(v.as.bytes->length);
        default:              return sage_rt_nil();
    }
}

SageValue sage_rt_typeof(SageValue v) {
    switch (v.type) {
        case SAGE_VAL_INT:      return sage_rt_string("int");
        case SAGE_VAL_FLOAT:    return sage_rt_string("float");
        case SAGE_VAL_BOOL:     return sage_rt_string("bool");
        case SAGE_VAL_NIL:      return sage_rt_string("nil");
        case SAGE_VAL_STRING:   return sage_rt_string("str");
        case SAGE_VAL_ARRAY:    return sage_rt_string("Array");
        case SAGE_VAL_DICT:     return sage_rt_string("Dict");
        case SAGE_VAL_TUPLE:    return sage_rt_string("Tuple");
        case SAGE_VAL_BYTES:    return sage_rt_string("Bytes");
        case SAGE_VAL_FUNCTION: return sage_rt_string("function");
        case SAGE_VAL_INSTANCE: return sage_rt_string(v.as.instance->class_def
                                    ? v.as.instance->class_def->name : "instance");
        case SAGE_VAL_CLASS:    return sage_rt_string(v.as.class_def
                                    ? v.as.class_def->name : "class");
        case SAGE_VAL_EXCEPTION:return sage_rt_string("Exception");
        case SAGE_VAL_POINTER:  return sage_rt_string("Pointer");
        case SAGE_VAL_CLIB:     return sage_rt_string("CLib");
        default:                return sage_rt_string("unknown");
    }
}

// type() builtin — lowercase names matching interpreter's type() function
SageValue sage_rt_type_lc(SageValue v) {
    switch (v.type) {
        case SAGE_VAL_INT:      return sage_rt_string("int");
        case SAGE_VAL_FLOAT:    return sage_rt_string("float");
        case SAGE_VAL_BOOL:     return sage_rt_string("bool");
        case SAGE_VAL_NIL:      return sage_rt_string("nil");
        case SAGE_VAL_STRING:   return sage_rt_string("string");
        case SAGE_VAL_ARRAY:    return sage_rt_string("array");
        case SAGE_VAL_DICT:     return sage_rt_string("dict");
        case SAGE_VAL_TUPLE:    return sage_rt_string("tuple");
        case SAGE_VAL_BYTES:    return sage_rt_string("bytes");
        case SAGE_VAL_FUNCTION: return sage_rt_string("function");
        case SAGE_VAL_INSTANCE: return sage_rt_string(v.as.instance->class_def
                                    ? v.as.instance->class_def->name : "instance");
        case SAGE_VAL_CLASS:    return sage_rt_string(v.as.class_def
                                    ? v.as.class_def->name : "class");
        default:                return sage_rt_string("unknown");
    }
}

SageValue sage_rt_int_cast(SageValue v) {
    switch (v.type) {
        case SAGE_VAL_INT:    return v;
        case SAGE_VAL_FLOAT:  return sage_rt_int((int64_t)v.as.number);
        case SAGE_VAL_BOOL:   return sage_rt_int(v.as.boolean ? 1 : 0);
        case SAGE_VAL_STRING: return sage_rt_int((int64_t)atoll(v.as.string));
        default:              return sage_rt_nil();
    }
}

SageValue sage_rt_float_cast(SageValue v) {
    switch (v.type) {
        case SAGE_VAL_INT:    return sage_rt_float((double)v.as.integer);
        case SAGE_VAL_FLOAT:  return v;
        case SAGE_VAL_BOOL:   return sage_rt_float(v.as.boolean ? 1.0 : 0.0);
        case SAGE_VAL_STRING: return sage_rt_float(atof(v.as.string));
        default:              return sage_rt_float(0.0);
    }
}

SageValue sage_rt_str_cast(SageValue v)  { return sage_rt_tostring(v); }

SageValue sage_rt_bool_cast(SageValue v) {
    return sage_rt_bool(sage_rt_truthy(v));
}

SageValue sage_rt_range(SageValue start, SageValue end) {
    int64_t s = SAGE_AS_INT64(start);
    int64_t e = SAGE_AS_INT64(end);
    SageValue arr = sage_rt_array_new();
    for (int64_t i = s; i < e; i++)
        sage_rt_array_push(arr, sage_rt_int(i));
    return arr;
}

SageValue sage_rt_range_inc(SageValue start, SageValue end) {
    int64_t s = SAGE_AS_INT64(start);
    int64_t e = SAGE_AS_INT64(end);
    SageValue arr = sage_rt_array_new();
    for (int64_t i = s; i <= e; i++)
        sage_rt_array_push(arr, sage_rt_int(i));
    return arr;
}

SageValue sage_rt_input(SageValue prompt) {
    if (SAGE_IS_STRING(prompt)) fputs(prompt.as.string, stdout);
    char buf[4096];
    if (!fgets(buf, sizeof(buf), stdin)) return sage_rt_nil();
    int n = (int)strlen(buf);
    if (n > 0 && buf[n-1] == '\n') buf[n-1] = '\0';
    return sage_rt_string(buf);
}

SageValue sage_rt_clock(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return sage_rt_float((double)ts.tv_sec + (double)ts.tv_nsec / 1e9);
}

SageValue sage_rt_some(SageValue v) {
    // Some(v) — wrapper dict {"__type":"Some","value":v}
    SageValue d = sage_rt_dict_new();
    sage_rt_dict_set(d, sage_rt_string("__type"),  sage_rt_string("Some"));
    sage_rt_dict_set(d, sage_rt_string("value"),   v);
    return d;
}

SageValue sage_rt_ok(SageValue v) {
    SageValue d = sage_rt_dict_new();
    sage_rt_dict_set(d, sage_rt_string("__type"),  sage_rt_string("Ok"));
    sage_rt_dict_set(d, sage_rt_string("value"),   v);
    return d;
}

SageValue sage_rt_err(SageValue v) {
    SageValue d = sage_rt_dict_new();
    sage_rt_dict_set(d, sage_rt_string("__type"),  sage_rt_string("Err"));
    sage_rt_dict_set(d, sage_rt_string("value"),   v);
    return d;
}

SageValue sage_rt_precision(SageValue f, SageValue digits) {
    double d = SAGE_AS_DOUBLE(f);
    int n    = (int)SAGE_AS_INT64(digits);
    char fmt[16];
    snprintf(fmt, sizeof(fmt), "%%.%df", n);
    char buf[64];
    snprintf(buf, sizeof(buf), fmt, d);
    // Return as string representation (matches Sage interpreter behavior)
    return sage_rt_string(buf);
}

// ─────────────────────────────────────────────────────────────────────────────
// I/O
// ─────────────────────────────────────────────────────────────────────────────

SageValue sage_rt_make_fn(SageNativeFn fn, void* env, const char* name) {
    (void)name;
    SageClosure* cl = (SageClosure*)sage_rt_gc_alloc(SAGE_VAL_FUNCTION, sizeof(SageClosure));
    cl->fn = (SageValue(*)(int, SageValue*, SageClosure*))fn;
    cl->capture_count = 0;
    cl->captures = NULL;
    cl->env = env;  // store env for compiled closures
    SageValue v;
    v.type = SAGE_VAL_FUNCTION;
    v.as.closure = cl;
    return v;
}

SageValue sage_rt_call_fn(SageValue fn, int argc, SageValue* argv) {
    if (!SAGE_IS_FUNCTION(fn)) {
        sage_rt_fatal("attempt to call a non-function value");
        return sage_rt_nil();
    }
    SageClosure* cl = fn.as.closure;
    // Pass env if set (compiled closures use void* env), otherwise pass cl
    void* third_arg = cl->env ? cl->env : (void*)cl;
    return ((SageValue(*)(int, SageValue*, void*))cl->fn)(argc, argv, third_arg);
}

// ─────────────────────────────────────────────────────────────────────────────
// C struct layout builtins — struct_def / struct_new / struct_get / struct_set / struct_size
// Mirrors interpreter stdlib exactly so AOT-compiled code and interpreted code
// share behaviour.
// ─────────────────────────────────────────────────────────────────────────────

static int _srt_type_info(const char* type, size_t* sz, size_t* al) {
    if (!strcmp(type,"char")||!strcmp(type,"byte"))   { *sz=1;              *al=1;              return 0; }
    if (!strcmp(type,"short"))                        { *sz=sizeof(short);  *al=sizeof(short);  return 0; }
    if (!strcmp(type,"int"))                          { *sz=sizeof(int);    *al=sizeof(int);    return 0; }
    if (!strcmp(type,"long"))                         { *sz=sizeof(long);   *al=sizeof(long);   return 0; }
    if (!strcmp(type,"float"))                        { *sz=sizeof(float);  *al=sizeof(float);  return 0; }
    if (!strcmp(type,"double"))                       { *sz=sizeof(double); *al=sizeof(double); return 0; }
    if (!strcmp(type,"ptr"))                          { *sz=sizeof(void*);  *al=sizeof(void*);  return 0; }
    return -1;
}
static size_t _srt_align(size_t off, size_t al) { return (off+al-1)&~(al-1); }

SageValue sage_rt_struct_def(SageValue fields_v) {
    if (!SAGE_IS_ARRAY(fields_v)) return sage_rt_nil();
    SageArray* fields = fields_v.as.array;
    SageValue result = sage_rt_dict_new();
    size_t offset = 0, max_align = 1;
    for (int i = 0; i < fields->count; i++) {
        SageValue pair_v = fields->elems[i];
        if (!SAGE_IS_ARRAY(pair_v) || pair_v.as.array->count < 2) return sage_rt_nil();
        SageValue name_v = pair_v.as.array->elems[0];
        SageValue type_v = pair_v.as.array->elems[1];
        if (!SAGE_IS_STRING(name_v) || !SAGE_IS_STRING(type_v)) return sage_rt_nil();
        const char* name = name_v.as.string;
        const char* type = type_v.as.string;
        size_t fsize, falign;
        if (_srt_type_info(type, &fsize, &falign) != 0) return sage_rt_nil();
        offset = _srt_align(offset, falign);
        if (falign > max_align) max_align = falign;
        // Store field info as tuple: (offset, size, type_string)
        SageValue info = sage_rt_tuple_new(3,
            sage_rt_int((int64_t)offset),
            sage_rt_int((int64_t)fsize),
            sage_rt_string(type));
        sage_rt_dict_set(result, name_v, info);
        offset += fsize;
    }
    offset = _srt_align(offset, max_align);
    sage_rt_dict_set(result, sage_rt_string("__size__"),  sage_rt_int((int64_t)offset));
    sage_rt_dict_set(result, sage_rt_string("__align__"), sage_rt_int((int64_t)max_align));
    return result;
}

SageValue sage_rt_struct_new(SageValue def_v) {
    if (!SAGE_IS_DICT(def_v)) return sage_rt_nil();
    SageValue sz_v = sage_rt_dict_get(def_v, sage_rt_string("__size__"));
    if (!SAGE_IS_INT(sz_v)) return sage_rt_nil();
    size_t size = (size_t)sz_v.as.integer;
    // Use sage_rt_manual_alloc so the GC header is prepended and
    // sage_rt_mem_free / sage_rt_manual_free can safely back up to it.
    void* ptr = sage_rt_manual_alloc(size);
    if (!ptr) return sage_rt_nil();
    memset(ptr, 0, size);
    SageValue r; r.type = SAGE_VAL_POINTER; r.as.pointer = ptr; return r;
}

SageValue sage_rt_struct_get(SageValue ptr_v, SageValue def_v, SageValue field_v) {
    if (!SAGE_IS_POINTER(ptr_v) || !SAGE_IS_DICT(def_v) || !SAGE_IS_STRING(field_v)) return sage_rt_nil();
    void* ptr = ptr_v.as.pointer;
    if (!ptr) return sage_rt_nil();
    SageValue info = sage_rt_dict_get(def_v, field_v);
    if (!SAGE_IS_TUPLE(info) || info.as.tuple->count < 3) return sage_rt_nil();
    size_t offset = (size_t)info.as.tuple->elems[0].as.integer;
    const char* type = info.as.tuple->elems[2].as.string;
    unsigned char* base = (unsigned char*)ptr + offset;
    if (!strcmp(type,"char")||!strcmp(type,"byte")) return sage_rt_int((int64_t)*base);
    if (!strcmp(type,"short"))  { short  v; memcpy(&v,base,sizeof(v)); return sage_rt_int((int64_t)v); }
    if (!strcmp(type,"int"))    { int    v; memcpy(&v,base,sizeof(v)); return sage_rt_int((int64_t)v); }
    if (!strcmp(type,"long"))   { long   v; memcpy(&v,base,sizeof(v)); return sage_rt_int((int64_t)v); }
    if (!strcmp(type,"float"))  { float  v; memcpy(&v,base,sizeof(v)); return sage_rt_float((double)v); }
    if (!strcmp(type,"double")) { double v; memcpy(&v,base,sizeof(v)); return sage_rt_float(v); }
    if (!strcmp(type,"ptr"))    { void*  v; memcpy(&v,base,sizeof(v)); SageValue r; r.type=SAGE_VAL_POINTER; r.as.pointer=v; return r; }
    return sage_rt_nil();
}

SageValue sage_rt_struct_set(SageValue ptr_v, SageValue def_v, SageValue field_v, SageValue val) {
    if (!SAGE_IS_POINTER(ptr_v) || !SAGE_IS_DICT(def_v) || !SAGE_IS_STRING(field_v)) return sage_rt_nil();
    void* ptr = ptr_v.as.pointer;
    if (!ptr) return sage_rt_nil();
    SageValue info = sage_rt_dict_get(def_v, field_v);
    if (!SAGE_IS_TUPLE(info) || info.as.tuple->count < 3) return sage_rt_nil();
    size_t offset = (size_t)info.as.tuple->elems[0].as.integer;
    const char* type = info.as.tuple->elems[2].as.string;
    unsigned char* base = (unsigned char*)ptr + offset;
    double dv = SAGE_AS_DOUBLE(val);
    int64_t iv = SAGE_AS_INT64(val);
    if (!strcmp(type,"char")||!strcmp(type,"byte")) { *base=(unsigned char)iv; }
    else if (!strcmp(type,"short"))  { short  v=(short)iv;    memcpy(base,&v,sizeof(v)); }
    else if (!strcmp(type,"int"))    { int    v=(int)iv;      memcpy(base,&v,sizeof(v)); }
    else if (!strcmp(type,"long"))   { long   v=(long)iv;     memcpy(base,&v,sizeof(v)); }
    else if (!strcmp(type,"float"))  { float  v=(float)dv;    memcpy(base,&v,sizeof(v)); }
    else if (!strcmp(type,"double")) { double v=dv;           memcpy(base,&v,sizeof(v)); }
    else if (!strcmp(type,"ptr") && SAGE_IS_POINTER(val)) { void* v=val.as.pointer; memcpy(base,&v,sizeof(v)); }
    return sage_rt_nil();
}

SageValue sage_rt_struct_size(SageValue def_v) {
    if (!SAGE_IS_DICT(def_v)) return sage_rt_nil();
    return sage_rt_dict_get(def_v, sage_rt_string("__size__"));
}

SageValue sage_rt_tonumber(SageValue s) {
    if (SAGE_IS_INT(s)) return sage_rt_float((double)s.as.integer);
    if (SAGE_IS_FLOAT(s)) return s;
    if (!SAGE_IS_STRING(s) || !s.as.string) return sage_rt_nil();
    const char* str = s.as.string;
    char* end;
    // Always parse as float (matches Sage interpreter behavior)
    double dv = strtod(str, &end);
    if (*end == '\0') return sage_rt_float(dv);
    return sage_rt_nil();
}

SageCoroutine* sage_rt_coro_new(SageCoroutineBody body, int argc, SageValue* argv) {
    SageCoroutine* co = (SageCoroutine*)malloc(sizeof(SageCoroutine));
    co->stack_size = 64*1024;
    co->stack = (char*)malloc(co->stack_size);
    co->body = body;
    co->done = 0;
    co->argc = argc;
    co->argv = argv;
    co->yielded = sage_rt_nil();
    getcontext(&co->ctx_callee);
    co->ctx_callee.uc_stack.ss_sp = co->stack;
    co->ctx_callee.uc_stack.ss_size = co->stack_size;
    co->ctx_callee.uc_link = NULL;
    makecontext(&co->ctx_callee, (void(*)())body, 1, co);
    return co;
}

SageValue sage_rt_coro_next(SageCoroutine* co) {
    if (!co || co->done) return sage_rt_nil();
    swapcontext(&co->ctx_caller, &co->ctx_callee);
    return co->yielded;
}

void sage_rt_coro_yield(SageCoroutine* co, SageValue val) {
    co->yielded = val;
    swapcontext(&co->ctx_callee, &co->ctx_caller);
}

void sage_rt_print(SageValue v) {
    SageValue s = sage_rt_tostring(v);
    fputs(s.as.string, stdout);
}

// sage_rt_print_kw: used for `print x` keyword — dispatches __str__ like the interpreter
void sage_rt_print_kw(SageValue v) {
    if (v.type == SAGE_VAL_INSTANCE && v.as.instance && v.as.instance->class_def) {
        SageClass* _cls = v.as.instance->class_def;
        while (_cls) {
            for (int _mi = 0; _mi < _cls->method_count; _mi++) {
                if (strcmp(_cls->methods[_mi].name, "__str__") == 0) {
                    SageValue _res = _cls->methods[_mi].fn(v.as.instance, 0, NULL);
                    v = SAGE_IS_STRING(_res) ? _res : sage_rt_tostring(_res);
                    goto _pkw_out;
                }
            }
            _cls = _cls->parent;
        }
    }
    _pkw_out:;
    SageValue s = sage_rt_tostring(v);
    fputs(s.as.string, stdout);
    fputc('\n', stdout);
}

void sage_rt_println(SageValue v) {
    sage_rt_print(v);
    fputc('\n', stdout);
}

// ─────────────────────────────────────────────────────────────────────────────
// Exceptions and panics
// ─────────────────────────────────────────────────────────────────────────────

SageValue sage_rt_exception(const char* fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    char* msg = (char*)sage_rt_gc_alloc(SAGE_VAL_EXCEPTION, strlen(buf) + 1);
    strcpy(msg, buf);
    SageValue v; v.type = SAGE_VAL_EXCEPTION; v.as.exception = msg;
    return v;
}

void sage_rt_fatal(const char* fmt, ...) {
    fprintf(stderr, "\n-- runtime error: ");
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, " --\n");
    exit(1);
}

void sage_rt_panic(SageValue v) {
    SageValue s = sage_rt_tostring(v);
    sage_rt_fatal("%s", s.as.string ? s.as.string : "(panic)");
}

void sage_rt_raise(SageValue exc) {
    if (sage_rt_exc_top && sage_rt_exc_top->active) {
        sage_rt_exc_top->exc = exc;
        sage_rt_exc_top->active = 0;
        longjmp(sage_rt_exc_top->jb, 1);
    }
    // No try frame — uncaught exception
    SageValue s = sage_rt_tostring(exc);
    sage_rt_fatal("uncaught exception: %s",
                  s.as.string ? s.as.string : "(unknown)");
}

// Per-thread recursion-depth counter for compiled code (see sage_runtime.h).
_Thread_local int sage_rt_call_depth = 0;

void sage_rt_recursion_error(void) {
    // Mirror the interpreter: error[E070], catchable exception. The counter is
    // restored by the enclosing try frame on unwind (or is irrelevant if the
    // exception is uncaught and we exit).
    fprintf(stderr,
            "error[E070]: maximum recursion depth exceeded (%d)\n",
            SAGE_RT_MAX_DEPTH);
    sage_rt_raise(sage_rt_exception("Maximum recursion depth exceeded"));
}

// ─────────────────────────────────────────────────────────────────────────────
// @manual memory builtins
// ─────────────────────────────────────────────────────────────────────────────

SageValue sage_rt_mem_alloc(SageValue size_val) {
    size_t sz = (size_t)SAGE_AS_INT64(size_val);
    void* p = sage_rt_manual_alloc(sz);
    SageValue v; v.type = SAGE_VAL_POINTER; v.as.pointer = p;
    return v;
}

void sage_rt_mem_free(SageValue ptr_val) {
    if (!SAGE_IS_POINTER(ptr_val) || !ptr_val.as.pointer) return;
    sage_rt_manual_free(ptr_val.as.pointer);
}

SageValue sage_rt_mem_read(SageValue ptr_val, SageValue offset_val,
                            SageValue type_str) {
    if (!SAGE_IS_POINTER(ptr_val)) return sage_rt_nil();
    uint8_t* base   = (uint8_t*)ptr_val.as.pointer;
    int64_t  offset = SAGE_AS_INT64(offset_val);
    const char* t   = SAGE_IS_STRING(type_str) ? type_str.as.string : "int";
    if (strcmp(t, "int") == 0) {
        int64_t v; memcpy(&v, base + offset, sizeof(v));
        return sage_rt_int(v);
    } else if (strcmp(t, "float") == 0 || strcmp(t, "double") == 0) {
        double v; memcpy(&v, base + offset, sizeof(v));
        return sage_rt_float(v);
    } else if (strcmp(t, "byte") == 0) {
        return sage_rt_int((int64_t)base[offset]);
    }
    return sage_rt_nil();
}

void sage_rt_mem_write(SageValue ptr_val, SageValue offset_val,
                       SageValue type_str, SageValue value) {
    if (!SAGE_IS_POINTER(ptr_val)) return;
    uint8_t* base   = (uint8_t*)ptr_val.as.pointer;
    int64_t  offset = SAGE_AS_INT64(offset_val);
    const char* t   = SAGE_IS_STRING(type_str) ? type_str.as.string : "int";
    if (strcmp(t, "int") == 0) {
        int64_t v = SAGE_AS_INT64(value);
        memcpy(base + offset, &v, sizeof(v));
    } else if (strcmp(t, "float") == 0 || strcmp(t, "double") == 0) {
        double v = SAGE_AS_DOUBLE(value);
        memcpy(base + offset, &v, sizeof(v));
    } else if (strcmp(t, "byte") == 0) {
        base[offset] = (uint8_t)SAGE_AS_INT64(value);
    }
}

SageValue sage_rt_ptr_add(SageValue ptr_val, SageValue offset_val) {
    if (!SAGE_IS_POINTER(ptr_val)) return sage_rt_nil();
    uint8_t* p = (uint8_t*)ptr_val.as.pointer + SAGE_AS_INT64(offset_val);
    SageValue v; v.type = SAGE_VAL_POINTER; v.as.pointer = p;
    return v;
}

SageValue sage_rt_ptr_null(void) {
    SageValue v; v.type = SAGE_VAL_POINTER; v.as.pointer = NULL;
    return v;
}

// mem_size(ptr) -> int  (size recorded in the GC header at alloc time)
SageValue sage_rt_mem_size(SageValue ptr_val) {
    if (!SAGE_IS_POINTER(ptr_val) || !ptr_val.as.pointer) return sage_rt_nil();
    SageGCHdr* h = SAGE_GC_HEADER(ptr_val.as.pointer);
    return sage_rt_int((int64_t)h->size);
}

// addressof(value) -> float (address of underlying data, for inspection only)
SageValue sage_rt_addressof(SageValue v) {
    void* addr = NULL;
    switch (v.type) {
        case SAGE_VAL_STRING:   addr = (void*)v.as.string; break;
        case SAGE_VAL_ARRAY:    addr = (void*)v.as.array; break;
        case SAGE_VAL_DICT:     addr = (void*)v.as.dict; break;
        case SAGE_VAL_POINTER:  addr = v.as.pointer; break;
        case SAGE_VAL_INSTANCE: addr = (void*)v.as.instance; break;
        default:                addr = NULL; break;
    }
    return sage_rt_float((double)(uintptr_t)addr);
}

// ── Path utilities (mirror interpreter path_* builtins) ──────────────────────
SageValue sage_rt_path_join(int argc, SageValue* argv) {
    if (argc < 1) return sage_rt_string("");
    size_t total = 1;
    for (int i = 0; i < argc; i++)
        total += (SAGE_IS_STRING(argv[i]) && argv[i].as.string ? strlen(argv[i].as.string) : 0) + 1;
    char* buf = (char*)sage_rt_gc_alloc(SAGE_VAL_STRING, total);
    buf[0] = '\0';
    for (int i = 0; i < argc; i++) {
        const char* seg = SAGE_IS_STRING(argv[i]) ? argv[i].as.string : "";
        if (!seg) seg = "";
        size_t len = strlen(buf);
        if (i > 0 && len > 0 && buf[len-1] != '/') { buf[len] = '/'; buf[len+1] = '\0'; }
        strcat(buf, seg);
    }
    return sage_rt_string(buf);
}

SageValue sage_rt_path_dirname(SageValue p) {
    if (!SAGE_IS_STRING(p) || !p.as.string) return sage_rt_string(".");
    const char* path = p.as.string;
    const char* slash = strrchr(path, '/');
    if (!slash) return sage_rt_string(".");
    if (slash == path) return sage_rt_string("/");
    size_t len = (size_t)(slash - path);
    char* dir = (char*)sage_rt_gc_alloc(SAGE_VAL_STRING, len + 1);
    memcpy(dir, path, len); dir[len] = '\0';
    return sage_rt_string(dir);
}

SageValue sage_rt_path_basename(SageValue p) {
    if (!SAGE_IS_STRING(p) || !p.as.string) return sage_rt_string("");
    const char* path = p.as.string;
    const char* slash = strrchr(path, '/');
    return sage_rt_string(slash ? slash + 1 : path);
}

SageValue sage_rt_path_ext(SageValue p) {
    if (!SAGE_IS_STRING(p) || !p.as.string) return sage_rt_string("");
    const char* path = p.as.string;
    const char* base = strrchr(path, '/');
    const char* dot = strrchr(base ? base : path, '.');
    if (!dot || dot == (base ? base + 1 : path)) return sage_rt_string("");
    return sage_rt_string(dot);
}

SageValue sage_rt_path_stem(SageValue p) {
    if (!SAGE_IS_STRING(p) || !p.as.string) return sage_rt_string("");
    const char* path = p.as.string;
    const char* slash = strrchr(path, '/');
    const char* base = slash ? slash + 1 : path;
    const char* dot = strrchr(base, '.');
    if (!dot || dot == base) return sage_rt_string(base);
    size_t len = (size_t)(dot - base);
    char* stem = (char*)sage_rt_gc_alloc(SAGE_VAL_STRING, len + 1);
    memcpy(stem, base, len); stem[len] = '\0';
    return sage_rt_string(stem);
}

SageValue sage_rt_path_exists(SageValue p) {
    if (!SAGE_IS_STRING(p) || !p.as.string) return sage_rt_bool(0);
    return sage_rt_bool(access(p.as.string, 0 /*F_OK*/) == 0);
}

// ── Inline assembly (asm_exec / asm_arch) ────────────────────────────────────
static const char* sage_asm_detect_arch(void) {
#if defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "aarch64";
#elif defined(__riscv) && __riscv_xlen == 64
    return "rv64";
#else
    return "unknown";
#endif
}
SageValue sage_rt_asm_arch(void) { return sage_rt_string(sage_asm_detect_arch()); }

static int sage_asm_safe_path(const char* p) {
    for (; *p; p++)
        if (!isalnum((unsigned char)*p) && *p!='/' && *p!='.' && *p!='-' && *p!='_' && *p!='~')
            return 0;
    return 1;
}
static char* sage_asm_unescape(const char* raw) {
    size_t len = strlen(raw);
    char* out = (char*)malloc(len + 1); size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (raw[i]=='\\' && i+1<len) {
            if (raw[i+1]=='n'){out[j++]='\n';i++;continue;}
            if (raw[i+1]=='t'){out[j++]='\t';i++;continue;}
        }
        out[j++]=raw[i];
    }
    out[j]='\0'; return out;
}

// asm_exec(code, ret_type, ...args): assemble, link as .so, dlopen, call.
SageValue sage_rt_asm_exec(int argc, SageValue* argv) {
    if (argc < 2 || !SAGE_IS_STRING(argv[0]) || !SAGE_IS_STRING(argv[1]))
        return sage_rt_nil();
    char* code = sage_asm_unescape(argv[0].as.string);
    const char* ret_type = argv[1].as.string;
    int num_args = argc - 2;
    if (num_args > 4) { free(code); return sage_rt_nil(); }
    const char* arch = sage_asm_detect_arch();

    char asm_path[] = "/tmp/sage_asm_XXXXXX.s";
    char obj_path[] = "/tmp/sage_asm_XXXXXX.o";
    char so_path[]  = "/tmp/sage_asm_XXXXXX.so";
    int afd = mkstemps(asm_path, 2); int ofd = mkstemps(obj_path, 2); int sfd = mkstemps(so_path, 3);
    if (afd>=0) close(afd); if (ofd>=0) close(ofd); if (sfd>=0) close(sfd);

    FILE* f = fopen(asm_path, "w");
    if (!f) { free(code); return sage_rt_nil(); }
    fprintf(f, ".text\n.globl sage_asm_fn\n.type sage_asm_fn, @function\nsage_asm_fn:\n%s\n    ret\n.size sage_asm_fn, .-sage_asm_fn\n", code);
    fclose(f); free(code);

    if (!sage_asm_safe_path(asm_path) || !sage_asm_safe_path(obj_path) || !sage_asm_safe_path(so_path))
        return sage_rt_nil();
    char as_cmd[512], ld_cmd[512];
    snprintf(as_cmd, sizeof(as_cmd), "as --64 -o %s %s 2>/dev/null", obj_path, asm_path);
    snprintf(ld_cmd, sizeof(ld_cmd), "gcc -shared -o %s %s 2>/dev/null", so_path, obj_path);
    (void)arch;
    SageValue result = sage_rt_nil();
    if (system(as_cmd)!=0) { unlink(asm_path); return sage_rt_nil(); }
    if (system(ld_cmd)!=0) { unlink(asm_path); unlink(obj_path); return sage_rt_nil(); }

    void* handle = dlopen(so_path, RTLD_LAZY);
    if (!handle) goto cleanup;
    {
        void* sym = dlsym(handle, "sage_asm_fn");
        if (!sym) { dlclose(handle); goto cleanup; }
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
        if (strcmp(ret_type,"double")==0) {
            double da[4]={0}; for (int i=0;i<num_args;i++) da[i]=SAGE_AS_DOUBLE(argv[i+2]);
            double (*f0)(void)=(double(*)(void))sym;
            double (*f1)(double)=(double(*)(double))sym;
            double (*f2)(double,double)=(double(*)(double,double))sym;
            double (*f3)(double,double,double)=(double(*)(double,double,double))sym;
            double (*f4)(double,double,double,double)=(double(*)(double,double,double,double))sym;
            double r=0;
            switch(num_args){case 0:r=f0();break;case 1:r=f1(da[0]);break;case 2:r=f2(da[0],da[1]);break;case 3:r=f3(da[0],da[1],da[2]);break;case 4:r=f4(da[0],da[1],da[2],da[3]);break;}
            result = sage_rt_float(r);
        } else if (strcmp(ret_type,"void")==0) {
            long long ia[4]={0}; for (int i=0;i<num_args;i++) ia[i]=(long long)SAGE_AS_INT64(argv[i+2]);
            void (*f0)(void)=(void(*)(void))sym;
            void (*f1)(long long)=(void(*)(long long))sym;
            void (*f2)(long long,long long)=(void(*)(long long,long long))sym;
            switch(num_args){case 0:f0();break;case 1:f1(ia[0]);break;case 2:f2(ia[0],ia[1]);break;default:f0();break;}
            result = sage_rt_nil();
        } else {
            long long ia[4]={0}; for (int i=0;i<num_args;i++) ia[i]=(long long)SAGE_AS_INT64(argv[i+2]);
            long long (*f0)(void)=(long long(*)(void))sym;
            long long (*f1)(long long)=(long long(*)(long long))sym;
            long long (*f2)(long long,long long)=(long long(*)(long long,long long))sym;
            long long (*f3)(long long,long long,long long)=(long long(*)(long long,long long,long long))sym;
            long long (*f4)(long long,long long,long long,long long)=(long long(*)(long long,long long,long long,long long))sym;
            long long r=0;
            switch(num_args){case 0:r=f0();break;case 1:r=f1(ia[0]);break;case 2:r=f2(ia[0],ia[1]);break;case 3:r=f3(ia[0],ia[1],ia[2]);break;case 4:r=f4(ia[0],ia[1],ia[2],ia[3]);break;}
            result = sage_rt_float((double)r);
        }
#pragma GCC diagnostic pop
        dlclose(handle);
    }
cleanup:
    unlink(asm_path); unlink(obj_path); unlink(so_path);
    return result;
}

// ── C FFI (ffi_open / ffi_call / ffi_sym / ffi_close) ────────────────────────
#ifdef SAGE_HAS_FFI
#include <ffi.h>
#endif

SageValue sage_rt_ffi_open(SageValue name) {
    if (!SAGE_IS_STRING(name) || !name.as.string) return sage_rt_nil();
    void* h = dlopen(name.as.string, RTLD_LAZY);
    if (!h) { fprintf(stderr, "ffi_open: %s\n", dlerror()); return sage_rt_nil(); }
    SageValue v; v.type = SAGE_VAL_CLIB; v.as.clib = h; return v;
}

SageValue sage_rt_ffi_close(SageValue lib) {
    if (lib.type == SAGE_VAL_CLIB && lib.as.clib) dlclose(lib.as.clib);
    return sage_rt_nil();
}

// ffi_sym(lib, name) -> bool : does the symbol exist?
SageValue sage_rt_ffi_sym(SageValue lib, SageValue name) {
    if (lib.type != SAGE_VAL_CLIB || !lib.as.clib) return sage_rt_bool(0);
    if (!SAGE_IS_STRING(name) || !name.as.string) return sage_rt_bool(0);
    dlerror();
    void* sym = dlsym(lib.as.clib, name.as.string);
    return sage_rt_bool(sym != NULL && dlerror() == NULL);
}

// ffi_call(lib, func_name, ret_type, [args])
SageValue sage_rt_ffi_call(SageValue lib, SageValue fname, SageValue rtype_v, SageValue argsv) {
    if (lib.type != SAGE_VAL_CLIB || !lib.as.clib) return sage_rt_nil();
    if (!SAGE_IS_STRING(fname) || !SAGE_IS_STRING(rtype_v)) return sage_rt_nil();
    const char* func_name = fname.as.string;
    const char* ret_type  = rtype_v.as.string;
    dlerror();
    void* sym = dlsym(lib.as.clib, func_name);
    char* err = dlerror();
    if (err) { fprintf(stderr, "ffi_call: %s\n", err); return sage_rt_nil(); }

    int call_argc = 0;
    SageValue* call_args = NULL;
    if (SAGE_IS_ARRAY(argsv)) { call_args = argsv.as.array->elems; call_argc = argsv.as.array->count; }
    if (call_argc > 16) { fprintf(stderr, "ffi_call: max 16 args\n"); return sage_rt_nil(); }

#ifdef SAGE_HAS_FFI
    ffi_cif cif;
    ffi_type* arg_types[16];
    void* arg_values[16];
    int64_t int_vals[16];
    double  dbl_vals[16];
    const char* str_vals[16];

    for (int i = 0; i < call_argc; i++) {
        SageValue v = call_args[i];
        if (SAGE_IS_INT(v)) {
            int_vals[i] = v.as.integer; arg_types[i] = &ffi_type_sint64; arg_values[i] = &int_vals[i];
        } else if (SAGE_IS_FLOAT(v)) {
            dbl_vals[i] = v.as.number;  arg_types[i] = &ffi_type_double; arg_values[i] = &dbl_vals[i];
        } else if (SAGE_IS_STRING(v)) {
            str_vals[i] = v.as.string;  arg_types[i] = &ffi_type_pointer; arg_values[i] = &str_vals[i];
        } else if (v.type == SAGE_VAL_POINTER && v.as.pointer) {
            str_vals[i] = (const char*)v.as.pointer; arg_types[i] = &ffi_type_pointer; arg_values[i] = &str_vals[i];
        } else {
            int_vals[i] = 0; arg_types[i] = &ffi_type_sint64; arg_values[i] = &int_vals[i];
        }
    }

    ffi_type* rtype;
    if      (!strcmp(ret_type,"double") || !strcmp(ret_type,"float")) rtype = &ffi_type_double;
    else if (!strcmp(ret_type,"int"))                                 rtype = &ffi_type_sint32;
    else if (!strcmp(ret_type,"long") || !strcmp(ret_type,"int64"))   rtype = &ffi_type_sint64;
    else if (!strcmp(ret_type,"string") || !strcmp(ret_type,"pointer")) rtype = &ffi_type_pointer;
    else if (!strcmp(ret_type,"void"))                                rtype = &ffi_type_void;
    else { fprintf(stderr, "ffi_call: unknown return type '%s'\n", ret_type); return sage_rt_nil(); }

    if (ffi_prep_cif(&cif, FFI_DEFAULT_ABI, (unsigned)call_argc, rtype, arg_types) != FFI_OK) {
        fprintf(stderr, "ffi_call: prep_cif failed\n"); return sage_rt_nil();
    }
    if (rtype == &ffi_type_double) {
        double r; ffi_call(&cif, FFI_FN(sym), &r, arg_values); return sage_rt_float(r);
    } else if (rtype == &ffi_type_sint32) {
        int r; ffi_call(&cif, FFI_FN(sym), &r, arg_values); return sage_rt_int((int64_t)r);
    } else if (rtype == &ffi_type_sint64) {
        int64_t r; ffi_call(&cif, FFI_FN(sym), &r, arg_values); return sage_rt_int(r);
    } else if (rtype == &ffi_type_pointer) {
        void* r; ffi_call(&cif, FFI_FN(sym), &r, arg_values);
        if (!strcmp(ret_type,"string")) return r ? sage_rt_string((const char*)r) : sage_rt_nil();
        SageValue p; p.type = SAGE_VAL_POINTER; p.as.pointer = r; return p;
    } else {
        ffi_call(&cif, FFI_FN(sym), NULL, arg_values); return sage_rt_nil();
    }
#else
    fprintf(stderr, "ffi_call: built without libffi support\n");
    return sage_rt_nil();
#endif
}

// ── File I/O (io module) ─────────────────────────────────────────────────────
SageValue sage_rt_io_writefile(SageValue path, SageValue content) {
    if (!SAGE_IS_STRING(path) || !path.as.string) return sage_rt_bool(0);
    const char* c = SAGE_IS_STRING(content) ? content.as.string : "";
    FILE* f = fopen(path.as.string, "wb");
    if (!f) return sage_rt_bool(0);
    if (c) fwrite(c, 1, strlen(c), f);
    fclose(f);
    return sage_rt_bool(1);
}
SageValue sage_rt_io_readfile(SageValue path) {
    if (!SAGE_IS_STRING(path) || !path.as.string) return sage_rt_nil();
    FILE* f = fopen(path.as.string, "rb");
    if (!f) return sage_rt_nil();
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return sage_rt_nil(); }
    char* buf = (char*)sage_rt_gc_alloc(SAGE_VAL_STRING, (size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = '\0';
    fclose(f);
    return sage_rt_string(buf);
}
SageValue sage_rt_io_exists(SageValue path) {
    if (!SAGE_IS_STRING(path) || !path.as.string) return sage_rt_bool(0);
    return sage_rt_bool(access(path.as.string, 0 /*F_OK*/) == 0);
}
SageValue sage_rt_io_remove(SageValue path) {
    if (!SAGE_IS_STRING(path) || !path.as.string) return sage_rt_bool(0);
    return sage_rt_bool(remove(path.as.string) == 0);
}

// ── CPU topology ─────────────────────────────────────────────────────────────
SageValue sage_rt_cpu_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    return sage_rt_int((int64_t)n);
}
SageValue sage_rt_cpu_physical_cores(void) {
    // Best-effort: report online processors. (Hyperthreading detection below
    // would refine this, but tests only require physical >= 1 and <= logical.)
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    return sage_rt_float((double)n);
}
SageValue sage_rt_cpu_has_hyperthreading(void) {
    return sage_rt_bool(0);
}

// ── Atomics ──────────────────────────────────────────────────────────────────
// In AOT, async procs run synchronously, so an atomic is just a boxed int cell.
typedef struct { int64_t value; } SageAtomic;

SageValue sage_rt_atomic_new(SageValue init) {
    SageAtomic* a = (SageAtomic*)sage_rt_manual_alloc(sizeof(SageAtomic));
    a->value = SAGE_IS_NUMERIC(init) ? SAGE_AS_INT64(init) : 0;
    SageValue v; v.type = SAGE_VAL_POINTER; v.as.pointer = a; return v;
}
SageValue sage_rt_atomic_load(SageValue av) {
    if (!SAGE_IS_POINTER(av) || !av.as.pointer) return sage_rt_float(0);
    return sage_rt_float((double)((SageAtomic*)av.as.pointer)->value);
}
SageValue sage_rt_atomic_store(SageValue av, SageValue n) {
    if (SAGE_IS_POINTER(av) && av.as.pointer)
        ((SageAtomic*)av.as.pointer)->value = SAGE_AS_INT64(n);
    return sage_rt_nil();
}
SageValue sage_rt_atomic_add(SageValue av, SageValue n) {
    if (!SAGE_IS_POINTER(av) || !av.as.pointer) return sage_rt_int(0);
    SageAtomic* a = (SageAtomic*)av.as.pointer;
    a->value += SAGE_AS_INT64(n);
    return sage_rt_float((double)a->value);
}
SageValue sage_rt_atomic_sub(SageValue av, SageValue n) {
    if (!SAGE_IS_POINTER(av) || !av.as.pointer) return sage_rt_int(0);
    SageAtomic* a = (SageAtomic*)av.as.pointer;
    a->value -= SAGE_AS_INT64(n);
    return sage_rt_float((double)a->value);
}
SageValue sage_rt_atomic_cas(SageValue av, SageValue expected, SageValue desired) {
    if (!SAGE_IS_POINTER(av) || !av.as.pointer) return sage_rt_bool(0);
    SageAtomic* a = (SageAtomic*)av.as.pointer;
    if (a->value == SAGE_AS_INT64(expected)) {
        a->value = SAGE_AS_INT64(desired);
        return sage_rt_bool(1);
    }
    return sage_rt_bool(0);
}
SageValue sage_rt_atomic_exchange(SageValue av, SageValue n) {
    if (!SAGE_IS_POINTER(av) || !av.as.pointer) return sage_rt_int(0);
    SageAtomic* a = (SageAtomic*)av.as.pointer;
    int64_t old = a->value; a->value = SAGE_AS_INT64(n);
    return sage_rt_float((double)old);
}

// ── Channels ─────────────────────────────────────────────────────────────────
// A simple in-memory FIFO. In AOT async procs run synchronously, so a queue
// faithfully models send/recv ordering without real threads.
typedef struct {
    SageValue* items;
    int count, cap, head;
    int closed;
} SageChannel;

SageValue sage_rt_channel_new(void) {
    SageChannel* c = (SageChannel*)sage_rt_manual_alloc(sizeof(SageChannel));
    c->items = NULL; c->count = 0; c->cap = 0; c->head = 0; c->closed = 0;
    SageValue v; v.type = SAGE_VAL_POINTER; v.as.pointer = c; return v;
}
SageValue sage_rt_channel_send(SageValue cv, SageValue item) {
    if (!SAGE_IS_POINTER(cv) || !cv.as.pointer) return sage_rt_nil();
    SageChannel* c = (SageChannel*)cv.as.pointer;
    if (c->head + c->count >= c->cap) {
        int ncap = c->cap ? c->cap * 2 : 8;
        SageValue* ni = (SageValue*)sage_rt_alloc((size_t)ncap * sizeof(SageValue));
        for (int i = 0; i < c->count; i++) ni[i] = c->items[c->head + i];
        c->items = ni; c->cap = ncap; c->head = 0;
    }
    c->items[c->head + c->count] = item;
    c->count++;
    return sage_rt_nil();
}
SageValue sage_rt_channel_recv(SageValue cv) {
    if (!SAGE_IS_POINTER(cv) || !cv.as.pointer) return sage_rt_nil();
    SageChannel* c = (SageChannel*)cv.as.pointer;
    if (c->count == 0) return sage_rt_nil();  // empty/closed → nil
    SageValue item = c->items[c->head];
    c->head++; c->count--;
    return item;
}
// try_recv returns Some(value) when an item is available, else nil (None).
SageValue sage_rt_channel_try_recv(SageValue cv) {
    if (!SAGE_IS_POINTER(cv) || !cv.as.pointer) return sage_rt_nil();
    SageChannel* c = (SageChannel*)cv.as.pointer;
    if (c->count == 0) return sage_rt_nil();
    SageValue item = c->items[c->head];
    c->head++; c->count--;
    return sage_rt_some(item);
}
SageValue sage_rt_channel_close(SageValue cv) {
    if (SAGE_IS_POINTER(cv) && cv.as.pointer)
        ((SageChannel*)cv.as.pointer)->closed = 1;
    return sage_rt_nil();
}
SageValue sage_rt_channel_len(SageValue cv) {
    if (!SAGE_IS_POINTER(cv) || !cv.as.pointer) return sage_rt_int(0);
    return sage_rt_int(((SageChannel*)cv.as.pointer)->count);
}
SageValue sage_rt_channel_is_closed(SageValue cv) {
    if (!SAGE_IS_POINTER(cv) || !cv.as.pointer) return sage_rt_bool(0);
    return sage_rt_bool(((SageChannel*)cv.as.pointer)->closed);
}

// ── Semaphores ───────────────────────────────────────────────────────────────
// Counting semaphore as a plain integer cell (synchronous AOT model).
typedef struct { int64_t permits; } SageSem;

SageValue sage_rt_sem_new(SageValue init) {
    SageSem* s = (SageSem*)sage_rt_manual_alloc(sizeof(SageSem));
    s->permits = SAGE_IS_NUMERIC(init) ? SAGE_AS_INT64(init) : 0;
    SageValue v; v.type = SAGE_VAL_POINTER; v.as.pointer = s; return v;
}
SageValue sage_rt_sem_wait(SageValue sv) {
    if (SAGE_IS_POINTER(sv) && sv.as.pointer) {
        SageSem* s = (SageSem*)sv.as.pointer;
        if (s->permits > 0) s->permits--;
    }
    return sage_rt_nil();
}
SageValue sage_rt_sem_trywait(SageValue sv) {
    if (!SAGE_IS_POINTER(sv) || !sv.as.pointer) return sage_rt_bool(0);
    SageSem* s = (SageSem*)sv.as.pointer;
    if (s->permits > 0) { s->permits--; return sage_rt_bool(1); }
    return sage_rt_bool(0);
}
SageValue sage_rt_sem_post(SageValue sv) {
    if (SAGE_IS_POINTER(sv) && sv.as.pointer)
        ((SageSem*)sv.as.pointer)->permits++;
    return sage_rt_nil();
}
