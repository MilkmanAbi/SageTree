#ifndef SAGE_AOT_H
#define SAGE_AOT_H

#include "ast.h"
#include "jit.h"

// ============================================================================
// AOT (Ahead-of-Time) Compiler for Sage
//
// Generates optimized C code with type specialization.
// Can work independently (whole-program compile) or with JIT
// (AOT provides baseline, JIT reoptimizes hot paths).
//
// Modes:
//   AOT-only:  sage --aot file.sage -o binary
//   JIT-only:  sage --jit file.sage
//   Combined:  sage --aot --jit file.sage (AOT baseline + JIT reopt)
// ============================================================================

// Type inference context for AOT
typedef struct {
    char* name;
    JitTypeTag inferred_type;
} AotVarType;

typedef struct {
    AotVarType* vars;
    int count;
    int capacity;
} AotTypeEnv;

// AOT compiler state
typedef struct {
    char** lines;       // Output C source lines
    int line_count;
    int line_capacity;
    int indent;
    int next_temp;
    AotTypeEnv type_env;
    int opt_level;       // 0-3
    int emit_guards;     // If 1, emit type guards (for JIT interop)
    // Defer stack — deferred stmts collected per-function, emitted at function exit
    Stmt* defer_stack[64];
    int defer_count;
    // Known C function names (from STMT_PROC/CLASS declarations) — call directly, not via sage_rt_call_fn
    char known_procs[512][64];
    int known_proc_count;
    // Raw Sage names of top-level procs that participate in a recursive cycle
    // (direct self-recursion or mutual recursion). Bodies of these procs get a
    // runtime recursion-depth guard emitted; all other procs stay overhead-free.
    char recursive_procs[256][64];
    int recursive_proc_count;
    // Doc comments per top-level proc (raw name -> docstring) for compile-time doc().
    struct { char name[64]; char* doc; } proc_docs[256];
    int proc_doc_count;
    // Class/struct constructor names — always use SageValue params (no type specialization)
    char known_ctors[128][64];
    int known_ctor_count;
    // Known struct type names (raw Sage names) — for value-semantics copy detection
    char known_structs[64][64];
    int known_struct_count;
    // Known enum namespace names (raw Sage names) — to detect ADT constructor calls
    char known_enums[64][64];
    int known_enum_count;
    // Coroutine body flag — when 1, STMT_YIELD emits sage_rt_coro_yield instead of return
    int in_coro_body;
    char coro_var[32];   // name of the _co variable in scope
    // Current class context for direct super dispatch (set while compiling class methods)
    char current_class_cname[128];   // C-mangled current class name (e.g. "sg_Dog")
    char current_parent_cname[128];  // C-mangled parent class name (e.g. "sg_Animal")
    // ADT variant field registry — maps (enum_raw, variant_raw) -> ordered field names
    struct {
        char enum_raw[64];
        char variant_raw[64];
        char field_names[8][64];
        int  field_count;
    } adt_variants[128];
    int adt_variant_count;
    // Imported module names (deduplication — each module only compiled once)
    char imported_modules[64][128];
    int  imported_module_count;
    // Maps short module name → full C prefix (e.g. "atomic" → "sg_std_atomic_")
    struct { char short_name[64]; char full_prefix[128]; } mod_prefix_map[64];
    int mod_prefix_map_count;
    // Default parameter registry: maps proc_cname#idx -> default C expression string
    struct { char proc_cname[64]; int param_idx; char default_expr[256]; Expr* default_ast; } proc_defaults[512];
    int proc_default_count;
    // Set to 1 while compiling a module body — prevents recursive import processing
    int  in_module_body;
    // Registry of module procs for dict population in main()
    // Each entry: module_cname, proc_raw_name, wrapper_cname
    struct {
        char mod_cname[64];  // e.g. "sg_arrays"
        char proc_raw[64];   // e.g. "map"
        char wrap_cname[1024];// e.g. "_mwrap_sg_arrays_sg_map" or "@@expr" for vars
    } mod_procs[512];
    int mod_proc_count;
    // Current module prefix for name-mangling during module compilation
    char current_module_prefix[128];
    // Top-level program vars declared as file-scope C globals (before main)
    // so class methods and procs compiled before main() can reference them
    char global_vars[128][64];
    int  global_var_count;
    // Set to 1 while compiling a proc/method body — STMT_LET inside procs
    // should declare locals, not assign to globals
    int  in_proc_body;
    // Set to 1 while compiling a closure wrapper body (_sw_ functions)
    // so STMT_LET can #undef capture aliases before redeclaring same names
    int  in_closure_body;
} AotCompiler;

// Lifecycle
void aot_init(AotCompiler* aot, int opt_level);
void aot_free(AotCompiler* aot);

// Type inference
void aot_infer_types(AotCompiler* aot, Stmt* program);
JitTypeTag aot_get_var_type(AotCompiler* aot, const char* name);
void aot_set_var_type(AotCompiler* aot, const char* name, JitTypeTag type);

// Compilation
char* aot_compile_program(AotCompiler* aot, Stmt* program);
void aot_compile_stmt(AotCompiler* aot, Stmt* stmt);
char* aot_compile_expr(AotCompiler* aot, Expr* expr);

// Specialized emission (optimized paths for known types)
char* aot_emit_add_int(AotCompiler* aot, const char* left, const char* right);
char* aot_emit_add_string(AotCompiler* aot, const char* left, const char* right);
char* aot_emit_add_generic(AotCompiler* aot, const char* left, const char* right);

// Output
int aot_write_c_file(AotCompiler* aot, const char* path);
int aot_compile_to_binary(AotCompiler* aot, const char* c_path, const char* bin_path);

#endif // SAGE_AOT_H
