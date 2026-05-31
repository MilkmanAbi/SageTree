// aot.c — SageTree AOT Compiler (IP1-C rewrite)
// High-performance C code generation using sage_runtime.h
// Key: unboxed fast paths, range loop specialization, -O3 -flto output
#define _GNU_SOURCE
#include "aot.h"
#include "gc.h"
#include "lexer.h"
#include "parser.h"
#include "module.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>
// Forward declarations for closure capture helpers
static void _collect_free_vars(Expr* e, const char** caps, int* ncaps, int maxcaps, ProcStmt* ps);
static void _collect_free_vars_stmt(Stmt* s, const char** caps, int* ncaps, int maxcaps, ProcStmt* ps);
// Forward declarations for generator detection
static int _has_yield(Stmt* body);
static int _yields_are_sequential(Stmt* body);
// Forward declarations for proc emission (used by STMT_IMPORT before definition)
static void aot_emit_proc(AotCompiler* aot, Stmt* s);
static void aot_emit_nested_procs(AotCompiler* aot, Stmt* body);
// Forward declaration for recursion-cycle detection (used in aot_emit_proc before definition)
static int aot_is_recursive_proc(AotCompiler* aot, Token name);
// Forward declaration for expression compiler (used in aot_infer_types for default params)
static char* aot_expr(AotCompiler* aot, Expr* expr, JitTypeTag hint);


void aot_init(AotCompiler* aot, int opt_level) {
    memset(aot, 0, sizeof(AotCompiler));
    aot->opt_level   = opt_level;
    aot->emit_guards = 0;
}

void aot_free(AotCompiler* aot) {
    for (int i = 0; i < aot->line_count; i++) free(aot->lines[i]);
    free(aot->lines);
    for (int i = 0; i < aot->type_env.count; i++) free(aot->type_env.vars[i].name);
    free(aot->type_env.vars);
    memset(aot, 0, sizeof(AotCompiler));
}

static void aot_emit_raw(AotCompiler* aot, const char* s) {
    // Append to last line (no indent, no newline push)
    if (aot->line_count == 0) return;
    int last = aot->line_count - 1;
    size_t old_len = strlen(aot->lines[last]);
    size_t add_len = strlen(s);
    aot->lines[last] = realloc(aot->lines[last], old_len + add_len + 1);
    memcpy(aot->lines[last] + old_len, s, add_len + 1);
}

static void aot_emit(AotCompiler* aot, const char* fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int msg_len = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (msg_len < 0) { va_end(ap); return; }
    int indent = aot->indent * 4;
    char* line = malloc(indent + msg_len + 2);
    if (!line) { va_end(ap); return; }
    if (indent) memset(line, ' ', indent);
    vsnprintf(line + indent, msg_len + 1, fmt, ap);
    va_end(ap);
    if (aot->line_count >= aot->line_capacity) {
        aot->line_capacity = aot->line_capacity ? aot->line_capacity * 2 : 512;
        aot->lines = realloc(aot->lines, sizeof(char*) * aot->line_capacity);
    }
    aot->lines[aot->line_count++] = line;
}

static void aot_blank(AotCompiler* aot) { aot_emit(aot, ""); }

static char* aot_cname(const char* start, int len) {
    char* out = malloc(len + 4);
    out[0]='s'; out[1]='g'; out[2]='_';
    for (int i = 0; i < len; i++) {
        char c = start[i];
        out[i+3] = ((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_') ? c : '_';
    }
    out[len+3] = '\0';
    return out;
}
static char* aot_cname_tok(Token t) { return aot_cname(t.start, t.length); }

static char* aot_temp(AotCompiler* aot) {
    char buf[24]; snprintf(buf, sizeof(buf), "_t%d", aot->next_temp++);
    return strdup(buf);
}

static char* aot_escape(const char* s) {
    int n = (int)strlen(s);
    char* out = malloc(n*4+1);
    int j = 0;
    for (int i = 0; i < n; i++) {
        switch ((unsigned char)s[i]) {
            case '\n': out[j++]='\\'; out[j++]='n';  break;
            case '\r': out[j++]='\\'; out[j++]='r';  break;
            case '\t': out[j++]='\\'; out[j++]='t';  break;
            case '\\': out[j++]='\\'; out[j++]='\\'; break;
            case '"':  out[j++]='\\'; out[j++]='"';  break;
            default:
                if ((unsigned char)s[i] < 0x20)
                    j += snprintf(out+j, 6, "\\x%02x", (unsigned char)s[i]);
                else out[j++] = s[i];
        }
    }
    out[j] = '\0';
    return out;
}

void aot_set_var_type(AotCompiler* aot, const char* name, JitTypeTag type) {
    for (int i = 0; i < aot->type_env.count; i++) {
        if (strcmp(aot->type_env.vars[i].name, name) == 0) {
            if (type == JIT_TYPE_UNKNOWN) {
                // Setting to UNKNOWN: overwrite unconditionally (it's already the worst case)
                aot->type_env.vars[i].inferred_type = JIT_TYPE_UNKNOWN;
            } else if (aot->type_env.vars[i].inferred_type != type) {
                // Conflict between two concrete types: demote to UNKNOWN
                aot->type_env.vars[i].inferred_type = JIT_TYPE_UNKNOWN;
            }
            // else: same type, no change
            return;
        }
    }
    if (aot->type_env.count >= aot->type_env.capacity) {
        aot->type_env.capacity = aot->type_env.capacity ? aot->type_env.capacity*2 : 64;
        aot->type_env.vars = realloc(aot->type_env.vars, sizeof(AotVarType)*aot->type_env.capacity);
    }
    aot->type_env.vars[aot->type_env.count].name = strdup(name);
    aot->type_env.vars[aot->type_env.count].inferred_type = type;
    aot->type_env.count++;
}

// Force-set a variable's type, overwriting any existing entry.
// Used inside proc/method bodies so inner-scope assignments shadow
// outer-scope UNKNOWN entries (e.g. from the global pre-scan).
static void aot_force_var_type(AotCompiler* aot, const char* name, JitTypeTag type) {
    for (int i = 0; i < aot->type_env.count; i++) {
        if (strcmp(aot->type_env.vars[i].name, name) == 0) {
            aot->type_env.vars[i].inferred_type = type;
            return;
        }
    }
    if (aot->type_env.count >= aot->type_env.capacity) {
        aot->type_env.capacity = aot->type_env.capacity ? aot->type_env.capacity*2 : 64;
        aot->type_env.vars = realloc(aot->type_env.vars, sizeof(AotVarType)*aot->type_env.capacity);
    }
    aot->type_env.vars[aot->type_env.count].name = strdup(name);
    aot->type_env.vars[aot->type_env.count].inferred_type = type;
    aot->type_env.count++;
}

JitTypeTag aot_get_var_type(AotCompiler* aot, const char* name) {
    for (int i = 0; i < aot->type_env.count; i++)
        if (strcmp(aot->type_env.vars[i].name, name) == 0)
            return aot->type_env.vars[i].inferred_type;
    return JIT_TYPE_UNKNOWN;
}

// Returns 1 if `name` is a variable known to be in scope at compile time:
// present in the type env (params, locals, top-level lets) or registered as a
// file-scope global. A variable can legitimately have JIT_TYPE_UNKNOWN while
// still being in scope, so membership — not type — is what we test. Used to
// mirror the interpreter, which leaves an interpolation placeholder literal
// when the inner expression would throw (e.g. undefined variable).
static int aot_var_in_scope(AotCompiler* aot, const char* name) {
    for (int i = 0; i < aot->type_env.count; i++)
        if (strcmp(aot->type_env.vars[i].name, name) == 0) return 1;
    for (int i = 0; i < aot->global_var_count; i++) {
        // global_vars holds C-mangled names (e.g. "sg_foo"); compare both forms
        const char* g = aot->global_vars[i];
        if (strcmp(g, name) == 0) return 1;
        if (strncmp(g, "sg_", 3) == 0 && strcmp(g + 3, name) == 0) return 1;
    }
    return 0;
}

static JitTypeTag aot_infer_expr(AotCompiler* aot, Expr* expr) {
    if (!expr) return JIT_TYPE_NIL;
    switch (expr->type) {
        case EXPR_INT: return JIT_TYPE_INT;
        case EXPR_NUMBER:
            return JIT_TYPE_FLOAT;  // EXPR_NUMBER = float literal; EXPR_INT = integer
        case EXPR_STRING: return JIT_TYPE_STRING;
        case EXPR_BOOL:   return JIT_TYPE_BOOL;
        case EXPR_NIL:    return JIT_TYPE_NIL;
        case EXPR_ARRAY:  return JIT_TYPE_ARRAY;
        case EXPR_DICT:   return JIT_TYPE_DICT;
        case EXPR_TUPLE:  return JIT_TYPE_TUPLE;
        case EXPR_VARIABLE: {
            char name[256];
            int len = expr->as.variable.name.length < 255 ? expr->as.variable.name.length : 255;
            memcpy(name, expr->as.variable.name.start, len); name[len] = '\0';
            return aot_get_var_type(aot, name);
        }
        case EXPR_BINARY: {
            int op = expr->as.binary.op.type;
            // Unary ops stored as binary with NULL right
            if (op == TOKEN_NOT || op == TOKEN_TILDE) return JIT_TYPE_BOOL;
            if (!expr->as.binary.right) return JIT_TYPE_UNKNOWN;
            JitTypeTag L = aot_infer_expr(aot, expr->as.binary.left);
            JitTypeTag R = aot_infer_expr(aot, expr->as.binary.right);
            if (op==TOKEN_EQ||op==TOKEN_NEQ||op==TOKEN_GT||op==TOKEN_LT||op==TOKEN_GTE||op==TOKEN_LTE)
                return JIT_TYPE_BOOL;
            if (L==JIT_TYPE_INT && R==JIT_TYPE_INT) {
                if (op==TOKEN_PLUS||op==TOKEN_MINUS||op==TOKEN_STAR||op==TOKEN_PERCENT||
                    op==TOKEN_AMP||op==TOKEN_PIPE||op==TOKEN_CARET||op==TOKEN_LSHIFT||op==TOKEN_RSHIFT||
                    op==TOKEN_SLASH)
                    return JIT_TYPE_INT;
            }
            if ((L==JIT_TYPE_INT||L==JIT_TYPE_FLOAT) && (R==JIT_TYPE_INT||R==JIT_TYPE_FLOAT)) {
                if (op==TOKEN_PLUS||op==TOKEN_MINUS||op==TOKEN_STAR||op==TOKEN_SLASH)
                    return JIT_TYPE_FLOAT;
            }
            // String concat: sage_rt_string_concat returns SageValue, not const char*
            // Return UNKNOWN so STMT_LET declares sg_x as SageValue not const char*
            if (L==JIT_TYPE_STRING && R==JIT_TYPE_STRING && op==TOKEN_PLUS) return JIT_TYPE_UNKNOWN;
            return JIT_TYPE_UNKNOWN;
        }
        case EXPR_CALL: {
            if (expr->as.call.callee && expr->as.call.callee->type == EXPR_VARIABLE) {
                const char* n = expr->as.call.callee->as.variable.name.start;
                int nl = expr->as.call.callee->as.variable.name.length;
                #define BM(s) (nl==(int)strlen(s)&&memcmp(n,s,nl)==0)
                if (BM("len")||BM("int")) return JIT_TYPE_INT;
                if (BM("float")) return JIT_TYPE_FLOAT;
                if (BM("str")||BM("typeof")) return JIT_TYPE_STRING;
                if (BM("bool")) return JIT_TYPE_BOOL;
                if (BM("range")||BM("range_inc")) return JIT_TYPE_ARRAY;
                // Struct constructor → STRUCT type (value semantics, copy on assign)
                for (int _si=0; _si<aot->known_struct_count; _si++) {
                    if (nl==(int)strlen(aot->known_structs[_si]) &&
                        memcmp(n, aot->known_structs[_si], nl)==0)
                        return JIT_TYPE_STRUCT;
                }
                #undef BM
            }
            return JIT_TYPE_UNKNOWN;
        }
        default: return JIT_TYPE_UNKNOWN;
    }
}

static void aot_register_proc(AotCompiler* aot, const char* name);
static int aot_is_known_proc(AotCompiler* aot, const char* name, int len);
void aot_infer_types(AotCompiler* aot, Stmt* program) {
    for (Stmt* s = program; s; s = s->next) {
        if (s->type == STMT_LET && s->as.let.initializer) {
            JitTypeTag t = aot_infer_expr(aot, s->as.let.initializer);
            char name[256];
            int len = s->as.let.name.length < 255 ? s->as.let.name.length : 255;
            memcpy(name, s->as.let.name.start, len); name[len] = '\0';
            aot_set_var_type(aot, name, t);
        }
        // Register all known C-callable names (procs, class constructors)
        if (s->type == STMT_PROC || s->type == STMT_ASYNC_PROC) {
            ProcStmt* ps = (s->type==STMT_PROC)?&s->as.proc:&s->as.async_proc;
            char* cn = aot_cname_tok(ps->name);
            aot_register_proc(aot, cn);
            // Generators always take SageValue params (constructor wraps them) — treat as ctors
            if (ps->body && _has_yield(ps->body)) {
                if (aot->known_ctor_count < 128)
                    snprintf(aot->known_ctors[aot->known_ctor_count++], 64, "%s", cn);
            }
            // Register param types from type annotations (: String, : Int, etc.)
            if (ps->param_types) {
                for (int _pi=0; _pi<ps->param_count; _pi++) {
                    if (!ps->param_types[_pi]) continue;
                    Token type_name = ps->param_types[_pi]->name;
                    JitTypeTag pt = JIT_TYPE_UNKNOWN;
                    int tl = type_name.length;
                    const char* ts = type_name.start;
                    if (tl==3 && !memcmp(ts,"Int",3))     pt=JIT_TYPE_INT;
                    else if (tl==5 && !memcmp(ts,"Float",5))  pt=JIT_TYPE_FLOAT;
                    else if (tl==4 && !memcmp(ts,"Bool",4))   pt=JIT_TYPE_BOOL;
                    else if (tl==6 && !memcmp(ts,"String",6)) pt=JIT_TYPE_STRING;
                    if (pt != JIT_TYPE_UNKNOWN) {
                        // Register as procname#paramN → type
                        char key[280];
                        int fnl = ps->name.length < 255 ? ps->name.length : 255;
                        snprintf(key, sizeof(key), "%.*s#%d", fnl, ps->name.start, _pi);
                        if (aot_get_var_type(aot, key) == JIT_TYPE_UNKNOWN)
                            aot_set_var_type(aot, key, pt);
                    }
                }
            }
            // Register default parameter values for call-site filling
            if (ps->defaults) {
                for (int _di=0; _di<ps->param_count; _di++) {
                    if (!ps->defaults[_di]) continue;
                    if (aot->proc_default_count >= 512) break;
                    // Store the default expr pointer — will be compiled at call site
                    snprintf(aot->proc_defaults[aot->proc_default_count].proc_cname, 64, "%s", cn);
                    aot->proc_defaults[aot->proc_default_count].param_idx = _di;
                    // Mark as "needs compilation" with a sentinel
                    snprintf(aot->proc_defaults[aot->proc_default_count].default_expr, 256, "__DEFAULT_PENDING_%p_%d",
                             (void*)ps->defaults[_di], _di);
                    aot->proc_defaults[aot->proc_default_count].default_ast = ps->defaults[_di];
                    aot->proc_default_count++;
                }
            }
            free(cn);
        }
        if (s->type == STMT_CLASS) {
            char* cn = aot_cname_tok(s->as.class_stmt.name);
            aot_register_proc(aot, cn);
            if (aot->known_ctor_count < 128)
                snprintf(aot->known_ctors[aot->known_ctor_count++], 64, "%s", cn);
            free(cn);
        }
        if (s->type == STMT_ENUM) {
            char* en = aot_cname_tok(s->as.enum_stmt.name);
            aot_register_proc(aot, en);
            // Enum namespace vars are DICT type (for field access via sage_rt_dict_get)
            char raw_en[256]; int enl=s->as.enum_stmt.name.length<255?s->as.enum_stmt.name.length:255;
            memcpy(raw_en,s->as.enum_stmt.name.start,enl); raw_en[enl]='\0';
            aot_set_var_type(aot, raw_en, JIT_TYPE_DICT);
            free(en);
        }
        if (s->type == STMT_STRUCT) {
            char* sn = aot_cname_tok(s->as.struct_stmt.name);
            aot_register_proc(aot, sn);
            // Register as ctor (always SageValue params)
            if (aot->known_ctor_count < 128)
                snprintf(aot->known_ctors[aot->known_ctor_count++], 64, "%s", sn);
            // Register raw name for struct-copy tracking
            if (aot->known_struct_count < 64) {
                char sraw[64]; int sl=s->as.struct_stmt.name.length<63?s->as.struct_stmt.name.length:63;
                memcpy(sraw,s->as.struct_stmt.name.start,sl); sraw[sl]='\0';
                snprintf(aot->known_structs[aot->known_struct_count++], 64, "%s", sraw);
            }
            free(sn);
        }
    }
    // Second pass: register enum variants with field names for ADT pattern matching
    for (Stmt* s = program; s; s = s->next) {
        if (s->type == STMT_ENUM) {
            EnumStmt* es = &s->as.enum_stmt;
            char en_raw[64]; int enl=es->name.length<63?es->name.length:63;
            memcpy(en_raw,es->name.start,enl); en_raw[enl]='\0';
            // Register raw enum name
            if (aot->known_enum_count < 64)
                snprintf(aot->known_enums[aot->known_enum_count++], 64, "%s", en_raw);
            // Register variant field info
            for (int vi=0; vi<es->variant_count; vi++) {
                if (!es->variant_field_counts || es->variant_field_counts[vi]==0) continue;
                if (aot->adt_variant_count >= 128) break;
                int slot = aot->adt_variant_count++;
                snprintf(aot->adt_variants[slot].enum_raw, 64, "%s", en_raw);
                int vnl=es->variant_names[vi].length<63?es->variant_names[vi].length:63;
                memcpy(aot->adt_variants[slot].variant_raw, es->variant_names[vi].start, vnl);
                aot->adt_variants[slot].variant_raw[vnl]='\0';
                int nf=es->variant_field_counts[vi]; if(nf>8)nf=8;
                aot->adt_variants[slot].field_count=nf;
                for (int fi=0;fi<nf;fi++) {
                    int fl=es->variant_fields[vi][fi].length<63?es->variant_fields[vi][fi].length:63;
                    memcpy(aot->adt_variants[slot].field_names[fi], es->variant_fields[vi][fi].start, fl);
                    aot->adt_variants[slot].field_names[fi][fl]='\0';
                }
            }
        }
    }
}

static void aot_infer_body(AotCompiler* aot, Stmt* body) {
    for (Stmt* s = body; s; s = s->next) {
        if (s->type == STMT_LET && s->as.let.initializer) {
            JitTypeTag t = aot_infer_expr(aot, s->as.let.initializer);
            char name[256];
            int len = s->as.let.name.length < 255 ? s->as.let.name.length : 255;
            memcpy(name, s->as.let.name.start, len); name[len] = '\0';
            // Use force-set: inner-scope var declarations always shadow outer UNKNOWN entries
            // (e.g. global pre-scan sets outer x→UNKNOWN, inner proc's x should be STRING)
            aot_force_var_type(aot, name, t);
        } else if (s->type == STMT_BLOCK) {
            aot_infer_body(aot, s->as.block.statements);
        } else if (s->type == STMT_FOR) {
            // Infer the loop variable type from the iterable
            // range(a,b) and 0..N → INT loop var
            if (s->as.for_stmt.iterable) {
                JitTypeTag lt = JIT_TYPE_UNKNOWN;
                Expr* it = s->as.for_stmt.iterable;
                if (it->type == EXPR_RANGE) lt = JIT_TYPE_INT;
                else if (it->type == EXPR_CALL && it->as.call.callee &&
                         it->as.call.callee->type == EXPR_VARIABLE) {
                    int nl = it->as.call.callee->as.variable.name.length;
                    const char* ns = it->as.call.callee->as.variable.name.start;
                    if ((nl==5&&memcmp(ns,"range",5)==0)||(nl==8&&memcmp(ns,"range_in",8)==0))
                        lt = JIT_TYPE_INT;
                }
                if (lt != JIT_TYPE_UNKNOWN && s->as.for_stmt.variable.length > 0) {
                    char vn[256]; int vl=s->as.for_stmt.variable.length<255?s->as.for_stmt.variable.length:255;
                    memcpy(vn,s->as.for_stmt.variable.start,vl); vn[vl]='\0';
                    // Force-set: for-loop vars are always INT (or UNKNOWN if non-range) — always win over outer UNKNOWN
                    aot_force_var_type(aot, vn, lt);
                }
            }
            aot_infer_body(aot, s->as.for_stmt.body);
        } else if (s->type == STMT_WHILE) {
            aot_infer_body(aot, s->as.while_stmt.body);
        } else if (s->type == STMT_IF) {
            aot_infer_body(aot, s->as.if_stmt.then_branch);
            if (s->as.if_stmt.else_branch) aot_infer_body(aot, s->as.if_stmt.else_branch);
        }
    }
}

// Track known C-callable procs (not SageValue function vars)
static void aot_register_proc(AotCompiler* aot, const char* name) {
    if (aot->known_proc_count >= 512) return;
    snprintf(aot->known_procs[aot->known_proc_count++], 64, "%s", name);
}
static int aot_is_known_proc(AotCompiler* aot, const char* name, int len) {
    char buf[65]; int l = len < 64 ? len : 64;
    memcpy(buf, name, l); buf[l] = '\0';
    // Check raw name
    for (int i = 0; i < aot->known_proc_count; i++)
        if (strcmp(aot->known_procs[i], buf) == 0) return 1;
    // Check cname (sg_-prefixed)
    char cn[70] = "sg_";
    strncat(cn, buf, 63);
    for (int i = 0; i < aot->known_proc_count; i++)
        if (strcmp(aot->known_procs[i], cn) == 0) return 1;
    return 0;
}

static const char* jit_ctype(JitTypeTag t) {
    switch (t) {
        case JIT_TYPE_INT:    return "int64_t";
        case JIT_TYPE_FLOAT:  return "double";
        case JIT_TYPE_BOOL:   return "int";
        case JIT_TYPE_STRING: return "const char*";
        default:              return "SageValue";
    }
}
static int jit_is_unboxed(JitTypeTag t) {
    return t==JIT_TYPE_INT||t==JIT_TYPE_FLOAT||t==JIT_TYPE_BOOL||t==JIT_TYPE_STRING;
}

/* Forward declarations for call-site analysis (defined later in file) */
static JitTypeTag aot_param_type(AotCompiler* aot, const char* fname, int flen, int idx);

static char* aot_box(JitTypeTag from, const char* val) {
    char* out = malloc(strlen(val)+64);
    switch (from) {
        case JIT_TYPE_INT:    sprintf(out,"sage_rt_int(%s)",val);    break;
        case JIT_TYPE_FLOAT:  sprintf(out,"sage_rt_float(%s)",val);  break;
        case JIT_TYPE_BOOL:   sprintf(out,"sage_rt_bool(%s)",val);   break;
        case JIT_TYPE_STRING: sprintf(out,"sage_rt_string(%s)",val); break;
        default: strcpy(out,val); break;
    }
    return out;
}

// Returns a SageValue expression for `e`, boxing if the expr is an unboxed scalar.
// Use this whenever you need to pass to a runtime function expecting SageValue.
static char* aot_expr_boxed(AotCompiler* aot, Expr* e) {
    if (!e) return strdup("sage_rt_nil()");
    JitTypeTag t = aot_infer_expr(aot, e);
    if (jit_is_unboxed(t)) {
        if (e->type == EXPR_VARIABLE) {
            // Variable: get raw scalar and box it
            char* raw = aot_expr(aot, e, t);
            char* boxed = aot_box(t, raw);
            free(raw);
            return boxed;
        }
        // Other exprs: UNKNOWN hint auto-boxes binary ops etc.
        char* v = aot_expr(aot, e, JIT_TYPE_UNKNOWN);
        if (strncmp(v, "sage_rt_", 8) == 0 || strncmp(v, "({", 2) == 0 ||
            strncmp(v, "_caps->", 7) == 0)  // closure capture fields are SageValue
            return v;
        char* boxed = aot_box(t, v);
        free(v);
        return boxed;
    }
    // For STRING variables: aot_expr with UNKNOWN returns raw const char*, not SageValue.
    // Explicitly box them so callers always get a SageValue.
    if (t == JIT_TYPE_STRING && e->type == EXPR_VARIABLE) {
        char* raw = aot_expr(aot, e, JIT_TYPE_STRING);
        char* boxed = aot_box(JIT_TYPE_STRING, raw);
        free(raw);
        return boxed;
    }
    return aot_expr(aot, e, JIT_TYPE_UNKNOWN);
}

static char* aot_expr(AotCompiler* aot, Expr* expr, JitTypeTag hint) {
    if (!expr) return strdup("sage_rt_nil()");
    JitTypeTag inferred = aot_infer_expr(aot, expr);
    (void)inferred;

    switch (expr->type) {
        case EXPR_INT: {
            char buf[32];
            snprintf(buf,sizeof(buf),"INT64_C(%lld)",(long long)expr->as.int_val.value);
            if (hint==JIT_TYPE_INT) return strdup(buf);
            if (hint==JIT_TYPE_FLOAT) {
                // Return as raw double constant for float arithmetic
                char* b=malloc(48); sprintf(b,"(double)INT64_C(%lld)",(long long)expr->as.int_val.value); return b;
            }
            char* b=malloc(64); sprintf(b,"sage_rt_int(INT64_C(%lld))",(long long)expr->as.int_val.value); return b;
        }
        case EXPR_NUMBER: {
            double d = expr->as.number.value;
            if (hint==JIT_TYPE_FLOAT || hint==JIT_TYPE_INT) {
                char buf[32]; snprintf(buf,sizeof(buf),"%.17g",d); return strdup(buf);
            }
            char* b=malloc(64); sprintf(b,"sage_rt_float(%.17g)",d); return b;
        }
        case EXPR_STRING: {
            char* esc = aot_escape(expr->as.string.value);
            if (hint==JIT_TYPE_STRING) { char* b=malloc(strlen(esc)+4); sprintf(b,"\"%s\"",esc); free(esc); return b; }
            char* b=malloc(strlen(esc)+32); sprintf(b,"sage_rt_string(\"%s\")",esc); free(esc); return b;
        }
        case EXPR_BOOL:
            if (hint==JIT_TYPE_BOOL) return strdup(expr->as.boolean.value?"1":"0");
            return strdup(expr->as.boolean.value?"sage_rt_bool(1)":"sage_rt_bool(0)");
        case EXPR_NIL: return strdup("sage_rt_nil()");

        case EXPR_VARIABLE: {
            char name[256];
            int len = expr->as.variable.name.length<255?expr->as.variable.name.length:255;
            memcpy(name,expr->as.variable.name.start,len); name[len]='\0';
            // Handle None/nil keywords
            if (!strcmp(name,"None") || !strcmp(name,"nil")) return strdup("sage_rt_nil()");
            if (!strcmp(name,"True")) return hint==JIT_TYPE_BOOL ? strdup("1") : strdup("sage_rt_bool(1)");
            if (!strcmp(name,"False")) return hint==JIT_TYPE_BOOL ? strdup("0") : strdup("sage_rt_bool(0)");
            JitTypeTag vtype = aot_get_var_type(aot,name);
            char* cname = aot_cname(expr->as.variable.name.start, expr->as.variable.name.length);
            if (!jit_is_unboxed(vtype)) {
                // If this is a known proc being used as a value (not called directly),
                // wrap it as sage_rt_make_fn so it can be passed to higher-order functions.
                // Top-level procs have _mwrap_ adaptors emitted after their definitions.
                // Module-internal procs use _sw_ wrappers from aot_emit_nested_procs.
                // Only apply _mwrap_ for top-level procs (not when inside a module body).
                if (vtype == JIT_TYPE_UNKNOWN && !aot->in_module_body &&
                    aot_is_known_proc(aot, name, len)) {
                    // Only emit _mwrap_ reference if this proc has a real STMT_PROC definition
                    // (global stubs like sg_bytes don't have _mwrap_ wrappers)
                    // Check: does _mwrap_cname exist in the emitted code? Use known_procs as proxy.
                    char mwrap_name[80]; snprintf(mwrap_name,sizeof(mwrap_name),"_mwrap_%s",cname);
                    int has_mwrap = 0;
                    for(int _ki=0;_ki<aot->known_proc_count;_ki++)
                        if(strcmp(aot->known_procs[_ki],mwrap_name)==0){has_mwrap=1;break;}
                    if (has_mwrap) {
                        char* out = malloc(strlen(cname)*2+80);
                        sprintf(out, "sage_rt_make_fn((SageNativeFn)_mwrap_%s,NULL,\"%s\")", cname, name);
                        free(cname); return out;
                    }
                    return cname;
                }
                return cname;
            }
            // Caller wants same unboxed type, or any unboxed type (caller casts) — return raw name
            if (hint==vtype || hint==JIT_TYPE_UNKNOWN) return cname;
            if (jit_is_unboxed(hint)) return cname; // caller will cast (e.g. int->double)
            // Caller needs SageValue — box it
            char* boxed = aot_box(vtype, cname);
            free(cname); return boxed;
        }

        case EXPR_BINARY: {
            int op = expr->as.binary.op.type;
            // Unary ops stored as binary with NULL right (parser convention)
            if (op == TOKEN_NOT) {
                // `not expr` — right is NULL, operand is left
                char* operand = aot_expr(aot, expr->as.binary.left, JIT_TYPE_UNKNOWN);
                JitTypeTag ot = aot_infer_expr(aot, expr->as.binary.left);
                char* raw = (expr->as.binary.left->type == EXPR_VARIABLE && jit_is_unboxed(ot))
                    ? aot_box(ot, operand) : operand;
                char* out = malloc(strlen(raw) + 64);
                sprintf(out, "sage_rt_bool(!sage_rt_truthy(%s))", raw);
                if (raw != operand) free(raw);
                free(operand); return out;
            }
            if (op == TOKEN_TILDE) {
                // Bitwise NOT: ~expr
                char* operand = aot_expr(aot, expr->as.binary.left, JIT_TYPE_INT);
                JitTypeTag ot = aot_infer_expr(aot, expr->as.binary.left);
                char* out;
                if (ot == JIT_TYPE_INT) {
                    out = malloc(strlen(operand) + 32);
                    sprintf(out, "sage_rt_int(~(%s))", operand);
                } else {
                    char* raw = (expr->as.binary.left->type == EXPR_VARIABLE && jit_is_unboxed(ot))
                        ? aot_box(ot, operand) : operand;
                    out = malloc(strlen(raw) + 32);
                    sprintf(out, "sage_rt_bnot(%s)", raw);
                    if (raw != operand) free(raw);
                }
                free(operand); return out;
            }
            if (!expr->as.binary.right) return strdup("sage_rt_nil()"); // safety
            JitTypeTag L = aot_infer_expr(aot, expr->as.binary.left);
            JitTypeTag R = aot_infer_expr(aot, expr->as.binary.right);

            // -- Unboxed integer arithmetic --
            if (L==JIT_TYPE_INT && R==JIT_TYPE_INT) {
                char* lv = aot_expr(aot, expr->as.binary.left,  JIT_TYPE_INT);
                char* rv = aot_expr(aot, expr->as.binary.right, JIT_TYPE_INT);
                char* out = malloc(strlen(lv)+strlen(rv)+96);
                int is_cmp=0;
                switch(op) {
                    case TOKEN_PLUS:    sprintf(out,"((%s)+(%s))",lv,rv); break;
                    case TOKEN_MINUS:   sprintf(out,"((%s)-(%s))",lv,rv); break;
                    case TOKEN_STAR:    sprintf(out,"((%s)*(%s))",lv,rv); break;
                    case TOKEN_SLASH:   sprintf(out,"({int64_t _dn=(%s),_dd=(%s); _dd?(_dn/_dd):INT64_C(0);})",lv,rv); break;
                    case TOKEN_PERCENT: sprintf(out,"({int64_t _mn=(%s),_md=(%s); _md?(_mn%%_md):INT64_C(0);})",lv,rv); break;
                    case TOKEN_AMP:     sprintf(out,"((%s)&(%s))",lv,rv); break;
                    case TOKEN_PIPE:    sprintf(out,"((%s)|(%s))",lv,rv); break;
                    case TOKEN_CARET:   sprintf(out,"((%s)^(%s))",lv,rv); break;
                    case TOKEN_LSHIFT:  sprintf(out,"((%s)<<(int)(%s))",lv,rv); break;
                    case TOKEN_RSHIFT:  sprintf(out,"((%s)>>(int)(%s))",lv,rv); break;
                    case TOKEN_EQ:  is_cmp=1; sprintf(out,"((%s)==(%s))",lv,rv); break;
                    case TOKEN_NEQ: is_cmp=1; sprintf(out,"((%s)!=(%s))",lv,rv); break;
                    case TOKEN_GT:  is_cmp=1; sprintf(out,"((%s)>(%s))",lv,rv);  break;
                    case TOKEN_LT:  is_cmp=1; sprintf(out,"((%s)<(%s))",lv,rv);  break;
                    case TOKEN_GTE: is_cmp=1; sprintf(out,"((%s)>=(%s))",lv,rv); break;
                    case TOKEN_LTE: is_cmp=1; sprintf(out,"((%s)<=(%s))",lv,rv); break;
                    default: sprintf(out,"sage_rt_add(sage_rt_int(%s),sage_rt_int(%s))",lv,rv); break;
                }
                free(lv); free(rv);
                JitTypeTag ret_t = is_cmp ? JIT_TYPE_BOOL : JIT_TYPE_INT;
                if (hint==ret_t || (hint!=JIT_TYPE_UNKNOWN && jit_is_unboxed(hint))) return out;
                char* b = aot_box(ret_t,out); free(out); return b;
            }

            // -- Unboxed float arithmetic --
            if ((L==JIT_TYPE_FLOAT||L==JIT_TYPE_INT) && (R==JIT_TYPE_FLOAT||R==JIT_TYPE_INT)) {
                char* lv = aot_expr(aot, expr->as.binary.left,  JIT_TYPE_FLOAT);
                char* rv = aot_expr(aot, expr->as.binary.right, JIT_TYPE_FLOAT);
                if (L==JIT_TYPE_INT) { char* t=malloc(strlen(lv)+16); sprintf(t,"(double)(%s)",lv); free(lv); lv=t; }
                if (R==JIT_TYPE_INT) { char* t=malloc(strlen(rv)+16); sprintf(t,"(double)(%s)",rv); free(rv); rv=t; }
                char* out=malloc(strlen(lv)+strlen(rv)+32);
                int is_cmp=0;
                switch(op) {
                    case TOKEN_PLUS:    sprintf(out,"((%s)+(%s))",lv,rv); break;
                    case TOKEN_MINUS:   sprintf(out,"((%s)-(%s))",lv,rv); break;
                    case TOKEN_STAR:    sprintf(out,"((%s)*(%s))",lv,rv); break;
                    case TOKEN_SLASH:   sprintf(out,"((%s)/(%s))",lv,rv); break;
                    case TOKEN_PERCENT: sprintf(out,"fmod((%s),(%s))",lv,rv); break;
                    case TOKEN_EQ:  is_cmp=1; sprintf(out,"((%s)==(%s))",lv,rv); break;
                    case TOKEN_NEQ: is_cmp=1; sprintf(out,"((%s)!=(%s))",lv,rv); break;
                    case TOKEN_GT:  is_cmp=1; sprintf(out,"((%s)>(%s))",lv,rv);  break;
                    case TOKEN_LT:  is_cmp=1; sprintf(out,"((%s)<(%s))",lv,rv);  break;
                    case TOKEN_GTE: is_cmp=1; sprintf(out,"((%s)>=(%s))",lv,rv); break;
                    case TOKEN_LTE: is_cmp=1; sprintf(out,"((%s)<=(%s))",lv,rv); break;
                    default: sprintf(out,"(%s)+(%s)",lv,rv); break;
                }
                free(lv); free(rv);
                JitTypeTag ret_t = is_cmp ? JIT_TYPE_BOOL : JIT_TYPE_FLOAT;
                if (hint==ret_t || (hint!=JIT_TYPE_UNKNOWN && jit_is_unboxed(hint))) return out;
                char* b = aot_box(ret_t,out); free(out); return b;
            }

            // -- String concat: box raw string vars; pass SageValue exprs directly --
            // String concat: return SageValue (not JIT_TYPE_STRING to avoid const char* mismatch)
            // aot_infer_expr returns STRING for this, but that would cause STMT_LET to declare
            // sg_probe as const char* and then try to assign SageValue from sage_rt_string_concat.
            // The fix: the hint guides whether we box or not.
            if (L==JIT_TYPE_STRING && R==JIT_TYPE_STRING && op==TOKEN_PLUS) {
                char* lv = aot_expr_boxed(aot, expr->as.binary.left);
                char* rv = aot_expr_boxed(aot, expr->as.binary.right);
                char* out = malloc(strlen(lv)+strlen(rv)+48);
                sprintf(out,"sage_rt_string_concat(%s,%s)",lv,rv);
                free(lv); free(rv); return out;
            }

            // -- Logical --
            if (op==TOKEN_AND) {
                JitTypeTag lt=aot_infer_expr(aot,expr->as.binary.left);
                JitTypeTag rt2=aot_infer_expr(aot,expr->as.binary.right);
                char* lv=aot_expr(aot,expr->as.binary.left,JIT_TYPE_UNKNOWN);
                char* rv=aot_expr(aot,expr->as.binary.right,JIT_TYPE_UNKNOWN);
                // sage_rt_truthy expects SageValue — box raw scalars
                char* lv_b=(jit_is_unboxed(lt)&&strncmp(lv,"sage_rt_",8)!=0)?aot_box(lt,lv):lv;
                char* rv_b=(jit_is_unboxed(rt2)&&strncmp(rv,"sage_rt_",8)!=0)?aot_box(rt2,rv):rv;
                char* out=malloc(strlen(lv_b)*2+strlen(rv_b)+64);
                sprintf(out,"(sage_rt_truthy(%s)?(%s):(%s))",lv_b,rv_b,lv_b);
                if(lv_b!=lv){free(lv_b);} else {free(lv);}
                if(rv_b!=rv){free(rv_b);} else {free(rv);}
                return out;
            }
            if (op==TOKEN_OR) {
                JitTypeTag lt=aot_infer_expr(aot,expr->as.binary.left);
                JitTypeTag rt2=aot_infer_expr(aot,expr->as.binary.right);
                char* lv=aot_expr(aot,expr->as.binary.left,JIT_TYPE_UNKNOWN);
                char* rv=aot_expr(aot,expr->as.binary.right,JIT_TYPE_UNKNOWN);
                char* lv_b=(jit_is_unboxed(lt)&&strncmp(lv,"sage_rt_",8)!=0)?aot_box(lt,lv):lv;
                char* rv_b=(jit_is_unboxed(rt2)&&strncmp(rv,"sage_rt_",8)!=0)?aot_box(rt2,rv):rv;
                char* out=malloc(strlen(lv_b)*2+strlen(rv_b)+64);
                sprintf(out,"(sage_rt_truthy(%s)?(%s):(%s))",lv_b,lv_b,rv_b);
                if(lv_b!=lv){free(lv_b);} else {free(lv);}
                if(rv_b!=rv){free(rv_b);} else {free(rv);}
                return out;
            }

            // -- String repeat: "ha" * 3 or 3 * "ha" --
            if ((L==JIT_TYPE_STRING&&R==JIT_TYPE_INT || L==JIT_TYPE_INT&&R==JIT_TYPE_STRING) && op==TOKEN_STAR) {
                char*sv=aot_expr(aot,L==JIT_TYPE_STRING?expr->as.binary.left:expr->as.binary.right,JIT_TYPE_UNKNOWN);
                char*iv=aot_expr(aot,L==JIT_TYPE_INT?expr->as.binary.left:expr->as.binary.right,JIT_TYPE_INT);
                // Box string var if raw
                int snb=(L==JIT_TYPE_STRING?expr->as.binary.left:expr->as.binary.right)->type==EXPR_VARIABLE;
                char*svb=snb?aot_box(JIT_TYPE_STRING,sv):sv;
                size_t sz=strlen(svb)+strlen(iv)+256;
                char*out=malloc(sz);
                snprintf(out,sz,"sage_rt_str_repeat(%s,%s)",svb,iv);
                if(svb!=sv)free(svb); free(sv); free(iv); return out;
            }
            // -- Generic fallback --
            // For EQ/NEQ/GT/LT/GTE/LTE/band/bor/bxor/shl/shr: always need SageValue args
            // Use aot_expr_boxed to safely get boxed versions
            {
                char* lv = aot_expr_boxed(aot, expr->as.binary.left);
                char* rv = aot_expr_boxed(aot, expr->as.binary.right);
                char* out=malloc(strlen(lv)+strlen(rv)+64);
                switch(op) {
                    case TOKEN_PLUS:    sprintf(out,"sage_rt_add(%s,%s)",lv,rv); break;
                    case TOKEN_MINUS:   sprintf(out,"sage_rt_sub(%s,%s)",lv,rv); break;
                    case TOKEN_STAR:    sprintf(out,"sage_rt_mul(%s,%s)",lv,rv); break;
                    case TOKEN_SLASH:   sprintf(out,"sage_rt_div(%s,%s)",lv,rv); break;
                    case TOKEN_PERCENT: sprintf(out,"sage_rt_mod(%s,%s)",lv,rv); break;
                    case TOKEN_EQ:      sprintf(out,"sage_rt_bool(sage_rt_equal(%s,%s))",lv,rv); break;
                    case TOKEN_NEQ:     sprintf(out,"sage_rt_bool(!sage_rt_equal(%s,%s))",lv,rv); break;
                    case TOKEN_GT:      sprintf(out,"sage_rt_bool(sage_rt_less(%s,%s))",rv,lv); break;
                    case TOKEN_LT:      sprintf(out,"sage_rt_bool(sage_rt_less(%s,%s))",lv,rv); break;
                    case TOKEN_GTE:     sprintf(out,"sage_rt_bool(!sage_rt_less(%s,%s))",lv,rv); break;
                    case TOKEN_LTE:     sprintf(out,"sage_rt_bool(!sage_rt_less(%s,%s))",rv,lv); break;
                    case TOKEN_AMP:     sprintf(out,"sage_rt_band(%s,%s)",lv,rv); break;
                    case TOKEN_PIPE:    sprintf(out,"sage_rt_bor(%s,%s)",lv,rv); break;
                    case TOKEN_CARET:   sprintf(out,"sage_rt_bxor(%s,%s)",lv,rv); break;
                    case TOKEN_LSHIFT:  sprintf(out,"sage_rt_shl(%s,%s)",lv,rv); break;
                    case TOKEN_RSHIFT:  sprintf(out,"sage_rt_shr(%s,%s)",lv,rv); break;
                    default:            sprintf(out,"sage_rt_add(%s,%s)",lv,rv); break;
                }
                free(lv); free(rv); return out;
            }
        }

        case EXPR_NULLCOAL: {
            // Must pass SageValues to sage_rt_nullcoal — box raw vars
            char* lv=aot_expr(aot,expr->as.nullcoal.left,JIT_TYPE_UNKNOWN);
            char* rv=aot_expr(aot,expr->as.nullcoal.right,JIT_TYPE_UNKNOWN);
            JitTypeTag lt=aot_infer_expr(aot,expr->as.nullcoal.left);
            JitTypeTag rt=aot_infer_expr(aot,expr->as.nullcoal.right);
            char* lbs=(expr->as.nullcoal.left->type==EXPR_VARIABLE&&jit_is_unboxed(lt))?aot_box(lt,lv):lv;
            char* rbs=(expr->as.nullcoal.right->type==EXPR_VARIABLE&&jit_is_unboxed(rt))?aot_box(rt,rv):rv;
            char* out=malloc(strlen(lbs)+strlen(rbs)+48);
            sprintf(out,"sage_rt_nullcoal(%s,%s)",lbs,rbs);
            if(lbs!=lv)free(lbs); if(rbs!=rv)free(rbs);
            free(lv); free(rv); return out;
        }

        case EXPR_RANGE: {
            char* lo=aot_expr(aot,expr->as.range.low,JIT_TYPE_INT);
            char* hi=aot_expr(aot,expr->as.range.high,JIT_TYPE_INT);
            char* out=malloc(strlen(lo)+strlen(hi)+80);
            if (expr->as.range.inclusive)
                sprintf(out,"sage_rt_range_inc(sage_rt_int(%s),sage_rt_int(%s))",lo,hi);
            else
                sprintf(out,"sage_rt_range(sage_rt_int(%s),sage_rt_int(%s))",lo,hi);
            free(lo); free(hi); return out;
        }

        case EXPR_FORCE_UNWRAP: {
            char* inner=aot_expr(aot,expr->as.unwrap.operand,JIT_TYPE_UNKNOWN);
            char* out=malloc(strlen(inner)+512);
            sprintf(out,"({SageValue _fu=(%s);"
                    "if(SAGE_IS_NIL(_fu))sage_rt_fatal(\"force-unwrap on nil\");"
                    "if(SAGE_IS_DICT(_fu)){SageValue _ft=sage_rt_dict_get(_fu,sage_rt_string(\"__type\"));"
                    "if(SAGE_IS_STRING(_ft)&&(strcmp(_ft.as.string,\"Some\")==0||strcmp(_ft.as.string,\"Ok\")==0))"
                    "{_fu=sage_rt_dict_get(_fu,sage_rt_string(\"value\"));}} _fu;})",inner);
            free(inner); return out;
        }
        case EXPR_PROPAGATE: {
            char* inner=aot_expr(aot,expr->as.unwrap.operand,JIT_TYPE_UNKNOWN);
            char* out=malloc(strlen(inner)+1024);  // template is ~560 chars, need headroom
            sprintf(out,"({SageValue _pp=(%s);"
                    "if(SAGE_IS_NIL(_pp))return sage_rt_nil();"
                    "if(SAGE_IS_DICT(_pp)){SageValue _pt=sage_rt_dict_get(_pp,sage_rt_string(\"__type\"));"
                    "if(SAGE_IS_STRING(_pt)&&strcmp(_pt.as.string,\"Err\")==0)return _pp;"
                    "if(SAGE_IS_STRING(_pt)&&strcmp(_pt.as.string,\"Ok\")==0)"
                    "{_pp=sage_rt_dict_get(_pp,sage_rt_string(\"value\"));}"
                    "else if(SAGE_IS_STRING(_pt)&&strcmp(_pt.as.string,\"Some\")==0)"
                    "{_pp=sage_rt_dict_get(_pp,sage_rt_string(\"value\"));}"
                    "} _pp;})",inner);
            free(inner); return out;
        }

        case EXPR_ARRAY: {
            int n=expr->as.array.count;
            if (n==0) return strdup("sage_rt_array_new()");
            char* tmp=aot_temp(aot);
            size_t sz=strlen(tmp)+128;
            for(int i=0;i<n;i++){char*e=aot_expr(aot,expr->as.array.elements[i],JIT_TYPE_UNKNOWN);sz+=strlen(e)+128;free(e);}
            char* out=malloc(sz);
            int pos=sprintf(out,"({SageValue %s=sage_rt_array_new();",tmp);
            char* idx_tmp=aot_temp(aot);
            for(int i=0;i<n;i++){
                Expr* el=expr->as.array.elements[i];
                // Spread element: EXPR_BINARY(left=arr, op=TOKEN_DOTDOT, right=NULL)
                if (el->type==EXPR_BINARY && el->as.binary.op.type==TOKEN_DOTDOT && !el->as.binary.right) {
                    char*spread_arr=aot_expr(aot,el->as.binary.left,JIT_TYPE_UNKNOWN);
                    pos+=sprintf(out+pos,
                        "{ SageValue _sp=%s; for(int %s=0;%s<sage_rt_array_len(_sp);%s++) sage_rt_array_push(%s,sage_rt_array_get(_sp,sage_rt_int(%s))); }",
                        spread_arr,idx_tmp,idx_tmp,idx_tmp,tmp,idx_tmp);
                    free(spread_arr);
                } else {
                    char*e=aot_expr_boxed(aot,el);
                    pos+=sprintf(out+pos,"sage_rt_array_push(%s,%s);",tmp,e);
                    free(e);
                }
            }
            sprintf(out+pos,"%s;})",tmp);
            free(idx_tmp); free(tmp); return out;
        }

        case EXPR_DICT: {
            int n=expr->as.dict.count;
            if (n==0) return strdup("sage_rt_dict_new()");
            char* tmp=aot_temp(aot);
            size_t sz=strlen(tmp)+128;
            for(int i=0;i<n;i++){char*v=aot_expr(aot,expr->as.dict.values[i],JIT_TYPE_UNKNOWN);sz+=strlen(v)+strlen(expr->as.dict.keys[i])+80;free(v);}
            char* out=malloc(sz);
            int pos=sprintf(out,"({SageValue %s=sage_rt_dict_new();",tmp);
            for(int i=0;i<n;i++){
                char*ek=aot_escape(expr->as.dict.keys[i]);
                char*ev=aot_expr_boxed(aot,expr->as.dict.values[i]);  // always box for dict_set
                pos+=sprintf(out+pos,"sage_rt_dict_set(%s,sage_rt_string(\"%s\"),%s);",tmp,ek,ev);
                free(ek);free(ev);
            }
            sprintf(out+pos,"%s;})",tmp); free(tmp); return out;
        }

        case EXPR_TUPLE: {
            int n=expr->as.tuple.count;
            size_t sz=64;
            for(int i=0;i<n;i++){char*e=aot_expr(aot,expr->as.tuple.elements[i],JIT_TYPE_UNKNOWN);sz+=strlen(e)+8;free(e);}
            char* out=malloc(sz);
            int pos=sprintf(out,"sage_rt_tuple_new(%d",n);
            for(int i=0;i<n;i++){char*e=aot_expr(aot,expr->as.tuple.elements[i],JIT_TYPE_UNKNOWN);pos+=sprintf(out+pos,",%s",e);free(e);}
            sprintf(out+pos,")"); return out;
        }

        case EXPR_INDEX: {
            JitTypeTag objt=aot_infer_expr(aot,expr->as.index.array);
            JitTypeTag idxt=aot_infer_expr(aot,expr->as.index.index);
            // For TUPLE, use integer index directly with tuple_get
            if (objt == JIT_TYPE_TUPLE) {
                char* obj=aot_expr(aot,expr->as.index.array,JIT_TYPE_UNKNOWN);
                char* idx=aot_expr(aot,expr->as.index.index,JIT_TYPE_INT);
                char* out=malloc(strlen(obj)+strlen(idx)+64);
                sprintf(out,"sage_rt_tuple_get(%s,(int)(%s))",obj,idx);
                free(obj);free(idx); return out;
            }
            // For STRING, use str_index (supports negative indexing)
            if (objt == JIT_TYPE_STRING) {
                char* obj_raw=aot_expr(aot,expr->as.index.array,JIT_TYPE_UNKNOWN);
                int obj_is_raw=(expr->as.index.array->type==EXPR_VARIABLE);
                char* obj=obj_is_raw?aot_box(JIT_TYPE_STRING,obj_raw):obj_raw;
                char* idx_raw=aot_expr(aot,expr->as.index.index,JIT_TYPE_INT);
                JitTypeTag idx_t=aot_infer_expr(aot,expr->as.index.index);
                char *idx;
                // If index expr returns SageValue (UNKNOWN or call result), unbox safely
                if (idx_t==JIT_TYPE_UNKNOWN || strncmp(idx_raw,"sage_rt_",8)==0) {
                    idx=malloc(strlen(idx_raw)+32);
                    sprintf(idx,"(int64_t)SAGE_AS_INT64(%s)",idx_raw); free(idx_raw);
                } else { idx=idx_raw; }
                char* out=malloc(strlen(obj)+strlen(idx)+64);
                sprintf(out,"sage_rt_str_index(%s,%s)",obj,idx);
                if(obj!=obj_raw)free(obj); free(obj_raw); free(idx); return out;
            }
            char* obj=aot_expr(aot,expr->as.index.array,JIT_TYPE_UNKNOWN);
            // Index must be SageValue for array_get/dict_get — box if unboxed raw scalar
            char* idx_raw=aot_expr(aot,expr->as.index.index,JIT_TYPE_UNKNOWN);
            char* idx;
            if (jit_is_unboxed(idxt) && strncmp(idx_raw,"sage_rt_",8)!=0) {
                idx = aot_box(idxt, idx_raw);
                free(idx_raw);
            } else {
                idx = idx_raw;
            }
            char* out_idx=malloc(strlen(obj)+strlen(idx)+64);
            if (objt==JIT_TYPE_DICT || idxt==JIT_TYPE_STRING)
                sprintf(out_idx,"sage_rt_dict_get(%s,%s)",obj,idx);
            else if (objt==JIT_TYPE_UNKNOWN)
                // Unknown type at compile-time — use universal indexer (handles strings and arrays)
                sprintf(out_idx,"sage_rt_index(%s,%s)",obj,idx);
            else
                sprintf(out_idx,"sage_rt_array_get(%s,%s)",obj,idx);
            free(obj); free(idx); return out_idx;
        }
        case EXPR_INDEX_SET: {
            char* obj=aot_expr(aot,expr->as.index_set.array,JIT_TYPE_UNKNOWN);
            JitTypeTag _ot=aot_infer_expr(aot,expr->as.index_set.array);
            JitTypeTag _it=aot_infer_expr(aot,expr->as.index_set.index);
            char* idx_raw=aot_expr(aot,expr->as.index_set.index,JIT_TYPE_UNKNOWN);
            char* idx;
            if (jit_is_unboxed(_it) && strncmp(idx_raw,"sage_rt_",8)!=0)
                { idx=aot_box(_it,idx_raw); free(idx_raw); }
            else
                idx=idx_raw;
            char* val=aot_expr_boxed(aot,expr->as.index_set.value);
            // val appears twice in output
            char* out=malloc(strlen(obj)+strlen(idx)+strlen(val)*2+64);
            if (_ot==JIT_TYPE_DICT||_ot==JIT_TYPE_UNKNOWN)
                sprintf(out,"({sage_rt_dict_set(%s,%s,%s);%s;})",obj,idx,val,val);
            else
                sprintf(out,"({sage_rt_array_set(%s,%s,%s);%s;})",obj,idx,val,val);
            free(obj);free(idx);free(val); return out;
        }
        case EXPR_SLICE: {
            char* obj=aot_expr(aot,expr->as.slice.array,JIT_TYPE_UNKNOWN);
            char* s_raw=expr->as.slice.start?aot_expr(aot,expr->as.slice.start,JIT_TYPE_INT):strdup("0");
            char* e_raw=expr->as.slice.end?aot_expr(aot,expr->as.slice.end,JIT_TYPE_INT):strdup("-1");
            // Safely unbox: if a SageValue expr, wrap in SAGE_AS_INT64
            JitTypeTag s_t = expr->as.slice.start?aot_infer_expr(aot,expr->as.slice.start):JIT_TYPE_INT;
            JitTypeTag e_t = expr->as.slice.end?aot_infer_expr(aot,expr->as.slice.end):JIT_TYPE_INT;
            char *s, *e;
            if (s_t==JIT_TYPE_UNKNOWN || strncmp(s_raw,"sage_rt_",8)==0) {
                s=malloc(strlen(s_raw)+32); sprintf(s,"(int)SAGE_AS_INT64(%s)",s_raw); free(s_raw);
            } else { s=s_raw; }
            if (e_t==JIT_TYPE_UNKNOWN || strncmp(e_raw,"sage_rt_",8)==0) {
                e=malloc(strlen(e_raw)+32); sprintf(e,"(int)SAGE_AS_INT64(%s)",e_raw); free(e_raw);
            } else { e=e_raw; }
            char* out=malloc(strlen(obj)+strlen(s)+strlen(e)+64);
            sprintf(out,"sage_rt_array_slice(%s,%s,%s)",obj,s,e);
            free(obj);free(s);free(e); return out;
        }
        case EXPR_GET: {
            char* obj=aot_expr(aot,expr->as.get.object,JIT_TYPE_UNKNOWN);
            char* prop=aot_escape(expr->as.get.property.start);
            prop[expr->as.get.property.length]='\0';
            // Dict field access: d.key → sage_rt_dict_get(d, "key")
            JitTypeTag _get_ot = aot_infer_expr(aot, expr->as.get.object);
            if (_get_ot == JIT_TYPE_DICT) {
                char* out = malloc(strlen(obj)+strlen(prop)+64);
                sprintf(out, "sage_rt_dict_get(%s,sage_rt_string(\"%s\"))", obj, prop);
                free(obj); free(prop); return out;
            }
            // Fast path for common properties
            if (!strcmp(prop,"length")||!strcmp(prop,"count")||!strcmp(prop,"size")) {
                JitTypeTag ot=aot_infer_expr(aot,expr->as.get.object);
                // Only apply length fast path for collection types, not instance fields named "count"
                if (ot==JIT_TYPE_STRING||ot==JIT_TYPE_ARRAY||ot==JIT_TYPE_TUPLE||ot==JIT_TYPE_DICT) {
                    int obj_is_raw=(expr->as.get.object->type==EXPR_VARIABLE&&jit_is_unboxed(ot));
                    char* obj_sv=obj_is_raw?aot_box(ot,obj):obj;
                    char* out=malloc(strlen(obj_sv)+64);
                    if (ot==JIT_TYPE_STRING) sprintf(out,"sage_rt_int(sage_rt_str_len(%s))",obj_sv);
                        else sprintf(out,"sage_rt_len(%s)",obj_sv);
                    if(obj_sv!=obj)free(obj_sv);
                    free(obj);free(prop); return out;
                }
                // For unknown/instance types, fall through to field_get
            }
            char* out=malloc(strlen(obj)+strlen(prop)+48);
            sprintf(out,"sage_rt_field_get(%s,\"%s\")",obj,prop);
            free(obj);free(prop); return out;
        }
        case EXPR_SET: {
            if (expr->as.set.object) {
                char* obj=aot_expr(aot,expr->as.set.object,JIT_TYPE_UNKNOWN);
                char* val=aot_expr_boxed(aot,expr->as.set.value);
                char* prop=aot_escape(expr->as.set.property.start);
                prop[expr->as.set.property.length]='\0';
                // val appears twice in sprintf output
                char* out=malloc(strlen(obj)+strlen(prop)+strlen(val)*2+64);
                sprintf(out,"({sage_rt_field_set(%s,\"%s\",%s);%s;})",obj,prop,val,val);
                free(obj);free(val);free(prop); return out;
            }
            char* name=aot_cname(expr->as.set.property.start,expr->as.set.property.length);
            // For SageValue vars, the value must match the declared type
            char* val=aot_expr(aot,expr->as.set.value,JIT_TYPE_UNKNOWN);
            char* out=malloc(strlen(name)+strlen(val)+16);
            sprintf(out,"(%s=%s)",name,val);
            free(name);free(val); return out;
        }

        case EXPR_CALL: {
            int argc=expr->as.call.arg_count;
            // super.method(args) — EXPR_SUPER is the direct callee
            // Parser: super.init(x) → EXPR_CALL(callee=EXPR_SUPER{method=Token}, args)
            if (expr->as.call.callee && expr->as.call.callee->type == EXPR_SUPER) {
                Token meth = expr->as.call.callee->as.super_expr.method;
                char mn[256]; int ml=meth.length<255?meth.length:255;
                memcpy(mn, meth.start, ml); mn[ml]='\0';
                size_t total = 512;
                for(int i=0;i<argc;i++){char*a=aot_expr(aot,expr->as.call.args[i],JIT_TYPE_UNKNOWN);total+=strlen(a)+8;free(a);}
                char* out = malloc(total); int pos = 0;
                if (aot->current_parent_cname[0]) {
                    char* mn_cn = aot_cname(mn, strlen(mn));
                    if (argc > 0) {
                        char* ab = malloc(total); int ap = sprintf(ab,"(SageValue[]){");
                        for(int i=0;i<argc;i++){char*a=aot_expr(aot,expr->as.call.args[i],JIT_TYPE_UNKNOWN);ap+=sprintf(ab+ap,"%s%s",i?",":"",a);free(a);}
                        sprintf(ab+ap,"}");
                        pos = sprintf(out,"%s_%s(_self,%d,%s)", aot->current_parent_cname, mn_cn, argc, ab);
                        free(ab);
                    } else {
                        pos = sprintf(out,"%s_%s(_self,0,NULL)", aot->current_parent_cname, mn_cn);
                    }
                    free(mn_cn);
                } else {
                    if (argc > 0) {
                        char* ab = malloc(total); int ap = sprintf(ab,"(SageValue[]){");
                        for(int i=0;i<argc;i++){char*a=aot_expr(aot,expr->as.call.args[i],JIT_TYPE_UNKNOWN);ap+=sprintf(ab+ap,"%s%s",i?",":"",a);free(a);}
                        sprintf(ab+ap,"}");
                        pos = sprintf(out,"sage_rt_method_call_super(_self,\"%s\",%d,%s)",mn,argc,ab);
                        free(ab);
                    } else pos = sprintf(out,"sage_rt_method_call_super(_self,\"%s\",0,NULL)",mn);
                }
                (void)pos; return out;
            }
            if (expr->as.call.callee && expr->as.call.callee->type==EXPR_VARIABLE) {
                const char* raw=expr->as.call.callee->as.variable.name.start;
                int rawlen=expr->as.call.callee->as.variable.name.length;
                #define BM(s) (rawlen==(int)strlen(s)&&memcmp(raw,s,rawlen)==0)
                if (BM("print")&&argc==1){
                    JitTypeTag _at=aot_infer_expr(aot,expr->as.call.args[0]);
                    char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);
                    int _nb=(expr->as.call.args[0]->type==EXPR_VARIABLE&&jit_is_unboxed(_at));
                    char*_arg=_nb?aot_box(_at,a):a;
                    char*o=malloc(strlen(_arg)+64);
                    sprintf(o,"({sage_rt_print(%s);sage_rt_nil();})",_arg);
                    if(_arg!=a)free(_arg);free(a);return o;
                }
                if (BM("println")&&argc==1){
                    JitTypeTag _at=aot_infer_expr(aot,expr->as.call.args[0]);
                    char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);
                    int _nb=(expr->as.call.args[0]->type==EXPR_VARIABLE&&jit_is_unboxed(_at));
                    char*_arg=_nb?aot_box(_at,a):a;
                    char*o=malloc(strlen(_arg)+64);
                    sprintf(o,"({sage_rt_println(%s);sage_rt_nil();})",_arg);
                    if(_arg!=a)free(_arg);free(a);return o;
                }
                if (BM("len")&&argc==1){
                    JitTypeTag _lat=aot_infer_expr(aot,expr->as.call.args[0]);
                    char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);
                    char*o=malloc(strlen(a)+64);
                    // Tuples need special handling since sage_rt_len returns nil for them
                    if(_lat==JIT_TYPE_TUPLE)
                        snprintf(o,strlen(a)+64,"sage_rt_int(sage_rt_tuple_len(%s))",a);
                    else if(hint==JIT_TYPE_INT)
                        snprintf(o,strlen(a)+64,"sage_rt_len(%s).as.integer",a);
                    else
                        snprintf(o,strlen(a)+64,"sage_rt_len(%s)",a);
                    free(a);return o;
                }
                if (BM("int")&&argc==1){
                    JitTypeTag _at=aot_infer_expr(aot,expr->as.call.args[0]);
                    char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);
                    int _nb=(expr->as.call.args[0]->type==EXPR_VARIABLE&&jit_is_unboxed(_at));
                    char*_arg=_nb?aot_box(_at,a):a;
                    char*o=malloc(strlen(_arg)+64);
                    if(hint==JIT_TYPE_INT)sprintf(o,"sage_rt_int_cast(%s).as.integer",_arg);
                    else sprintf(o,"sage_rt_int_cast(%s)",_arg);
                    if(_arg!=a)free(_arg);free(a);return o;
                }
                if (BM("float")&&argc==1){
                    JitTypeTag _at=aot_infer_expr(aot,expr->as.call.args[0]);
                    char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);
                    int _nb=(expr->as.call.args[0]->type==EXPR_VARIABLE&&jit_is_unboxed(_at));
                    char*_arg=_nb?aot_box(_at,a):a;
                    char*o=malloc(strlen(_arg)+64);
                    if(hint==JIT_TYPE_FLOAT)sprintf(o,"sage_rt_float_cast(%s).as.number",_arg);
                    else sprintf(o,"sage_rt_float_cast(%s)",_arg);
                    if(_arg!=a)free(_arg);free(a);return o;
                }
                if (BM("str")&&argc==1){
                    JitTypeTag at=aot_infer_expr(aot,expr->as.call.args[0]);
                    char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);
                    // Only box bare unboxed variables — literals/expressions already return SageValue
                    int needs_box=(expr->as.call.args[0]->type==EXPR_VARIABLE&&jit_is_unboxed(at));
                    char*arg=needs_box?aot_box(at,a):a;
                    char*o=malloc(strlen(arg)+64);
                    if(hint==JIT_TYPE_STRING) sprintf(o,"sage_rt_str_cast(%s).as.string",arg);
                    else sprintf(o,"sage_rt_str_cast(%s)",arg);
                    if(arg!=a) { free(arg); } free(a); return o;
                }
                // contains(haystack, needle) — builtin string/array membership test
                if (BM("contains")&&argc==2){
                    char*a=aot_expr_boxed(aot,expr->as.call.args[0]);
                    char*b=aot_expr_boxed(aot,expr->as.call.args[1]);
                    char*o=malloc(strlen(a)*3+strlen(b)*2+200);
                    // For strings: use sage_rt_str_find; for arrays: use sage_rt_array_contains
                    sprintf(o,"(SAGE_IS_STRING(%s)?sage_rt_bool(sage_rt_str_find(%s,%s).as.integer>=0):sage_rt_bool(sage_rt_truthy(sage_rt_method_call(%s,\"contains\",1,(SageValue[]){%s}))))",a,a,b,a,b);
                    free(a);free(b);return o;
                }
                if (BM("hash")&&argc==1){
                    char*a=aot_expr_boxed(aot,expr->as.call.args[0]);
                    char*o=malloc(strlen(a)+200);
                    // hash() returns an integer hash: proper string hash for strings,
                    // pointer identity otherwise.
                    sprintf(o,"({SageValue _hv=%s;SAGE_IS_STRING(_hv)?sage_rt_int((int64_t)sage_rt_str_hash(_hv)):sage_rt_int((int64_t)(uintptr_t)_hv.as.string);})",a);
                    free(a);return o;
                }
                if (BM("typeof")&&argc==1){
                    JitTypeTag _at=aot_infer_expr(aot,expr->as.call.args[0]);
                    char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);
                    int _nb=(expr->as.call.args[0]->type==EXPR_VARIABLE&&jit_is_unboxed(_at));
                    char*_arg=_nb?aot_box(_at,a):a;
                    char*o=malloc(strlen(_arg)+32);sprintf(o,"sage_rt_typeof(%s)",_arg);
                    if(_arg!=a)free(_arg);free(a);return o;
                }
                // type() — lowercase names (array, dict, etc.) matching interpreter
                if (BM("type")&&argc==1){char*a=aot_expr_boxed(aot,expr->as.call.args[0]);char*o=malloc(strlen(a)+32);sprintf(o,"sage_rt_type_lc(%s)",a);free(a);return o;}
                // ord(char) → integer codepoint
                if (BM("ord")&&argc==1){char*a=aot_expr_boxed(aot,expr->as.call.args[0]);char*o=malloc(strlen(a)*3+128);sprintf(o,"sage_rt_int((int64_t)(unsigned char)((SAGE_IS_STRING(%s)&&%s.as.string)?%s.as.string[0]:0))",a,a,a);free(a);return o;}
                // chr(n) → single-char string
                if (BM("chr")&&argc==1){
                    char*a=aot_expr_boxed(aot,expr->as.call.args[0]);
                    char*o=malloc(strlen(a)+128);
                    sprintf(o,"({char _chr[2]={(char)SAGE_AS_INT64(%s),0};sage_rt_string(_chr);})",a);
                    free(a);return o;
                }
                // len() → array/string length
                if (BM("len")&&argc==1){char*a=aot_expr_boxed(aot,expr->as.call.args[0]);char*o=malloc(strlen(a)+32);sprintf(o,"sage_rt_len(%s)",a);free(a);return o;}
                if (BM("bool")&&argc==1){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);char*o=malloc(strlen(a)+32);sprintf(o,"sage_rt_bool_cast(%s)",a);free(a);return o;}
                if (BM("range")&&argc==2){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);char*b=aot_expr(aot,expr->as.call.args[1],JIT_TYPE_UNKNOWN);char*o=malloc(strlen(a)+strlen(b)+32);sprintf(o,"sage_rt_range(%s,%s)",a,b);free(a);free(b);return o;}
                if (BM("range")&&argc==1){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);char*o=malloc(strlen(a)+64);sprintf(o,"sage_rt_range(sage_rt_int(0),%s)",a);free(a);return o;}
                if (BM("range_inc")&&argc==2){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);char*b=aot_expr(aot,expr->as.call.args[1],JIT_TYPE_UNKNOWN);char*o=malloc(strlen(a)+strlen(b)+32);sprintf(o,"sage_rt_range_inc(%s,%s)",a,b);free(a);free(b);return o;}
                if (BM("clock")&&argc==0) return strdup("sage_rt_clock()");
                if (BM("input")){char*a=argc==1?aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN):strdup("sage_rt_nil()");char*o=malloc(strlen(a)+32);sprintf(o,"sage_rt_input(%s)",a);free(a);return o;}
                if (BM("gc_collect")&&argc==0) return strdup("({sage_rt_gc_collect();sage_rt_nil();})");
                if (BM("gc_disable")&&argc==0) return strdup("({sage_rt_gc_disable();sage_rt_nil();})");
                if (BM("gc_enable")&&argc==0)  return strdup("({sage_rt_gc_enable();sage_rt_nil();})");
                // Some/Ok/Err builtins — but only when no user/module proc of that
                // name shadows them. Inside e.g. lib/safety.sage, `Some` refers to the
                // module's own proc (sg_safety_sg_Some), not the runtime builtin, so we
                // must fall through to the normal call path in that case.
                {
                    int _shadowed = 0;
                    if (aot->current_module_prefix[0]) {
                        // Module procs are mangled as <module_prefix>sg_<rawname>
                        // (aot_cname_tok prepends sg_ to the raw name).
                        char _pfxname[200];
                        snprintf(_pfxname, sizeof(_pfxname), "%ssg_%.*s", aot->current_module_prefix, (int)rawlen, raw);
                        if (aot_is_known_proc(aot, _pfxname, (int)strlen(_pfxname))) _shadowed = 1;
                    }
                    if (!_shadowed) {
                        if (BM("Some")&&argc==1){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);char*o=malloc(strlen(a)+48);sprintf(o,"sage_rt_some(%s)",a);free(a);return o;}
                        if (BM("Ok")&&argc==1){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);char*o=malloc(strlen(a)+48);sprintf(o,"sage_rt_ok(%s)",a);free(a);return o;}
                        if (BM("Err")&&argc==1){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);char*o=malloc(strlen(a)+48);sprintf(o,"sage_rt_err(%s)",a);free(a);return o;}
                    }
                }
                // -- String/array builtins (bare function form) --
                // join() builtin
                // -- C struct layout builtins --
                // -- Atomic builtins (bare form) — real runtime support --
                if (BM("atomic_new")&&argc==1){char*a=aot_expr_boxed(aot,expr->as.call.args[0]);size_t sz=strlen(a)+48;char*o=malloc(sz);snprintf(o,sz,"sage_rt_atomic_new(%s)",a);free(a);return o;}
                if (BM("atomic_load")&&argc==1){char*a=aot_expr_boxed(aot,expr->as.call.args[0]);size_t sz=strlen(a)+48;char*o=malloc(sz);snprintf(o,sz,"sage_rt_atomic_load(%s)",a);free(a);return o;}
                if (BM("atomic_store")&&argc==2){char*a=aot_expr_boxed(aot,expr->as.call.args[0]);char*b=aot_expr_boxed(aot,expr->as.call.args[1]);size_t sz=strlen(a)+strlen(b)+48;char*o=malloc(sz);snprintf(o,sz,"sage_rt_atomic_store(%s,%s)",a,b);free(a);free(b);return o;}
                if (BM("atomic_add")&&argc==2){char*a=aot_expr_boxed(aot,expr->as.call.args[0]);char*b=aot_expr_boxed(aot,expr->as.call.args[1]);size_t sz=strlen(a)+strlen(b)+48;char*o=malloc(sz);snprintf(o,sz,"sage_rt_atomic_add(%s,%s)",a,b);free(a);free(b);return o;}
                if (BM("atomic_sub")&&argc==2){char*a=aot_expr_boxed(aot,expr->as.call.args[0]);char*b=aot_expr_boxed(aot,expr->as.call.args[1]);size_t sz=strlen(a)+strlen(b)+48;char*o=malloc(sz);snprintf(o,sz,"sage_rt_atomic_sub(%s,%s)",a,b);free(a);free(b);return o;}
                if (BM("atomic_cas")&&argc==3){char*a=aot_expr_boxed(aot,expr->as.call.args[0]);char*b=aot_expr_boxed(aot,expr->as.call.args[1]);char*c=aot_expr_boxed(aot,expr->as.call.args[2]);size_t sz=strlen(a)+strlen(b)+strlen(c)+48;char*o=malloc(sz);snprintf(o,sz,"sage_rt_atomic_cas(%s,%s,%s)",a,b,c);free(a);free(b);free(c);return o;}
                if (BM("atomic_exchange")&&argc==2){char*a=aot_expr_boxed(aot,expr->as.call.args[0]);char*b=aot_expr_boxed(aot,expr->as.call.args[1]);size_t sz=strlen(a)+strlen(b)+48;char*o=malloc(sz);snprintf(o,sz,"sage_rt_atomic_exchange(%s,%s)",a,b);free(a);free(b);return o;}
                // -- CPU topology builtins --
                if ((BM("cpu_count")||BM("cpu_logical_cores")||BM("smp_count"))&&argc==0) return strdup("sage_rt_cpu_count()");
                if (BM("cpu_physical_cores")&&argc==0) return strdup("sage_rt_cpu_physical_cores()");
                if (BM("cpu_has_hyperthreading")&&argc==0) return strdup("sage_rt_cpu_has_hyperthreading()");
                // -- Semaphore builtins (bare form) --
                if (BM("sem_new")&&argc==1){char*a=aot_expr_boxed(aot,expr->as.call.args[0]);size_t sz=strlen(a)+48;char*o=malloc(sz);snprintf(o,sz,"sage_rt_sem_new(%s)",a);free(a);return o;}
                if (BM("sem_wait")&&argc==1){char*a=aot_expr_boxed(aot,expr->as.call.args[0]);size_t sz=strlen(a)+48;char*o=malloc(sz);snprintf(o,sz,"sage_rt_sem_wait(%s)",a);free(a);return o;}
                if (BM("sem_post")&&argc==1){char*a=aot_expr_boxed(aot,expr->as.call.args[0]);size_t sz=strlen(a)+48;char*o=malloc(sz);snprintf(o,sz,"sage_rt_sem_post(%s)",a);free(a);return o;}
                if (BM("sem_trywait")&&argc==1){char*a=aot_expr_boxed(aot,expr->as.call.args[0]);size_t sz=strlen(a)+48;char*o=malloc(sz);snprintf(o,sz,"sage_rt_sem_trywait(%s)",a);free(a);return o;}
                // -- Threading / mutex stubs (no real threads in AOT) --
                if (BM("spawn")||BM("thread_spawn")||BM("thread_join")||BM("thread_id")||
                    BM("mutex_new")||BM("mutex_lock")||BM("mutex_unlock")||BM("mutex_try_lock")||
                    BM("semaphore_new")||BM("semaphore_wait")||BM("semaphore_signal")||
                    BM("rwlock_new")||BM("rwlock_read")||BM("rwlock_write")||BM("rwlock_release")||
                    BM("condvar_new")||BM("condvar_wait")||BM("condvar_signal")||BM("condvar_broadcast")) {
                    return strdup("sage_rt_nil()");
                }
                // -- Inline assembly stubs (asm_compile is cross-compile; not in AOT) --
                if (BM("asm_compile")) {
                    return strdup("sage_rt_nil()");
                }
                // -- Signal handling stubs --
                if (BM("signal_set")||BM("signal_raise")||BM("signal_ignore")) {
                    return strdup("sage_rt_nil()");
                }
                // -- Pthread/threading globals stubs --
                if (BM("threadpool_new")||BM("threadpool_submit")||BM("threadpool_shutdown")||
                    BM("thread_sleep")||BM("thread_yield")) {
                    return strdup("sage_rt_nil()");
                }
                // -- GC mode/stats builtins --
                if (BM("gc_mode")&&argc==0) return strdup("sage_rt_gc_mode()");
                if (BM("gc_set_arc")&&argc==0) return strdup("({sage_rt_gc_set_arc();sage_rt_nil();})");
                if (BM("gc_set_orc")&&argc==0) return strdup("({sage_rt_gc_set_orc();sage_rt_nil();})");
                if (BM("gc_set_tracing")&&argc==0) return strdup("({sage_rt_gc_set_tracing();sage_rt_nil();})");
                if (BM("gc_collections")&&argc==0) return strdup("sage_rt_gc_collections()");
                if (BM("gc_stats")&&argc==0) return strdup("sage_rt_gc_stats_dict()");
                if (BM("gc_alloc_count")&&argc==0) return strdup("sage_rt_gc_collections()");
                if (BM("addressof")&&argc==1){char*a=aot_expr_boxed(aot,expr->as.call.args[0]);size_t sz=strlen(a)+64;char*o=malloc(sz);snprintf(o,sz,"sage_rt_addressof(%s)",a);free(a);return o;}
                if (BM("mem_size")&&argc==1){char*a=aot_expr_boxed(aot,expr->as.call.args[0]);size_t sz=strlen(a)+64;char*o=malloc(sz);snprintf(o,sz,"sage_rt_mem_size(%s)",a);free(a);return o;}
                if (BM("mem_copy")||BM("mem_zero")) {
                    if (argc>=1) { char*a=aot_expr_boxed(aot,expr->as.call.args[0]); char*o=malloc(strlen(a)+32); sprintf(o,"sage_rt_nil()/*%s*/",a); free(a); return o; }
                    return strdup("sage_rt_nil()");
                }
                // -- Path utilities --
                if (BM("path_join")&&argc>=1){
                    // Build a SageValue[] arg array and call the variadic helper.
                    size_t cap=64; for(int i=0;i<argc;i++){char*a=aot_expr_boxed(aot,expr->as.call.args[i]);cap+=strlen(a)+8;free(a);}
                    char*o=malloc(cap); int pos=sprintf(o,"sage_rt_path_join(%d,(SageValue[]){",argc);
                    for(int i=0;i<argc;i++){char*a=aot_expr_boxed(aot,expr->as.call.args[i]);pos+=sprintf(o+pos,"%s%s",i?",":"",a);free(a);}
                    sprintf(o+pos,"})"); return o;
                }
                if (BM("path_dirname")&&argc==1){char*a=aot_expr_boxed(aot,expr->as.call.args[0]);size_t sz=strlen(a)+64;char*o=malloc(sz);snprintf(o,sz,"sage_rt_path_dirname(%s)",a);free(a);return o;}
                if (BM("path_basename")&&argc==1){char*a=aot_expr_boxed(aot,expr->as.call.args[0]);size_t sz=strlen(a)+64;char*o=malloc(sz);snprintf(o,sz,"sage_rt_path_basename(%s)",a);free(a);return o;}
                if (BM("path_ext")&&argc==1){char*a=aot_expr_boxed(aot,expr->as.call.args[0]);size_t sz=strlen(a)+64;char*o=malloc(sz);snprintf(o,sz,"sage_rt_path_ext(%s)",a);free(a);return o;}
                if (BM("path_stem")&&argc==1){char*a=aot_expr_boxed(aot,expr->as.call.args[0]);size_t sz=strlen(a)+64;char*o=malloc(sz);snprintf(o,sz,"sage_rt_path_stem(%s)",a);free(a);return o;}
                if (BM("path_exists")&&argc==1){char*a=aot_expr_boxed(aot,expr->as.call.args[0]);size_t sz=strlen(a)+64;char*o=malloc(sz);snprintf(o,sz,"sage_rt_path_exists(%s)",a);free(a);return o;}
                // -- Bytes builtins --
                if (BM("bytes")&&argc==1){char*a=aot_expr_boxed(aot,expr->as.call.args[0]);size_t sz=strlen(a)+48;char*o=malloc(sz);snprintf(o,sz,"sage_rt_bytes_ctor(%s)",a);free(a);return o;}
                if (BM("bytes")&&argc==0){return strdup("sage_rt_bytes_new(0)");}
                if (BM("bytes_len")&&argc==1){char*a=aot_expr_boxed(aot,expr->as.call.args[0]);size_t sz=strlen(a)+48;char*o=malloc(sz);snprintf(o,sz,"sage_rt_bytes_len_v(%s)",a);free(a);return o;}
                if (BM("bytes_get")&&argc==2){char*a=aot_expr_boxed(aot,expr->as.call.args[0]);char*b=aot_expr_boxed(aot,expr->as.call.args[1]);size_t sz=strlen(a)+strlen(b)+48;char*o=malloc(sz);snprintf(o,sz,"sage_rt_bytes_get_v(%s,%s)",a,b);free(a);free(b);return o;}
                if (BM("bytes_set")&&argc==3){char*a=aot_expr_boxed(aot,expr->as.call.args[0]);char*b=aot_expr_boxed(aot,expr->as.call.args[1]);char*c=aot_expr_boxed(aot,expr->as.call.args[2]);size_t sz=strlen(a)+strlen(b)+strlen(c)+48;char*o=malloc(sz);snprintf(o,sz,"sage_rt_bytes_set_v(%s,%s,%s)",a,b,c);free(a);free(b);free(c);return o;}
                if ((BM("bytes_to_string")||BM("bytes_to_str"))&&argc==1){char*a=aot_expr_boxed(aot,expr->as.call.args[0]);size_t sz=strlen(a)+48;char*o=malloc(sz);snprintf(o,sz,"sage_rt_bytes_to_string(%s)",a);free(a);return o;}
                if ((BM("bytes_from_string")||BM("bytes_from_str"))&&argc==1){char*a=aot_expr_boxed(aot,expr->as.call.args[0]);size_t sz=strlen(a)+48;char*o=malloc(sz);snprintf(o,sz,"sage_rt_bytes_from_string(%s)",a);free(a);return o;}
                if (BM("bytes_slice")&&argc>=2){char*a=aot_expr_boxed(aot,expr->as.call.args[0]);char*b=aot_expr_boxed(aot,expr->as.call.args[1]);char*c=argc>=3?aot_expr_boxed(aot,expr->as.call.args[2]):strdup("sage_rt_nil()");size_t sz=strlen(a)+strlen(b)+strlen(c)+48;char*o=malloc(sz);snprintf(o,sz,"sage_rt_bytes_slice(%s,%s,%s)",a,b,c);free(a);free(b);free(c);return o;}
                if (BM("bytes_push")&&argc==2){char*a=aot_expr_boxed(aot,expr->as.call.args[0]);char*b=aot_expr_boxed(aot,expr->as.call.args[1]);size_t sz=strlen(a)+strlen(b)+64;char*o=malloc(sz);snprintf(o,sz,"({sage_rt_bytes_push(%s,(uint8_t)SAGE_AS_INT64(%s));sage_rt_nil();})",a,b);free(a);free(b);return o;}
                // -- doc(fn): compile-time docstring lookup --
                if (BM("doc")&&argc==1 && expr->as.call.args[0]->type==EXPR_VARIABLE){
                    Token nt = expr->as.call.args[0]->as.variable.name;
                    const char* dstr = NULL; int found = 0;
                    for (int _di=0; _di<aot->proc_doc_count; _di++){
                        if ((int)strlen(aot->proc_docs[_di].name)==(int)nt.length &&
                            memcmp(aot->proc_docs[_di].name, nt.start, nt.length)==0){
                            dstr = aot->proc_docs[_di].doc; found = 1; break;
                        }
                    }
                    if (found){
                        if (!dstr) return strdup("sage_rt_nil()");
                        char* esc = aot_escape(dstr);
                        char* o = malloc(strlen(esc)+32);
                        sprintf(o, "sage_rt_string(\"%s\")", esc);
                        free(esc); return o;
                    }
                    return strdup("sage_rt_nil()");
                }
                // -- C FFI --
                if (BM("ffi_open")&&argc==1){char*a=aot_expr_boxed(aot,expr->as.call.args[0]);size_t sz=strlen(a)+48;char*o=malloc(sz);snprintf(o,sz,"sage_rt_ffi_open(%s)",a);free(a);return o;}
                if (BM("ffi_close")&&argc==1){char*a=aot_expr_boxed(aot,expr->as.call.args[0]);size_t sz=strlen(a)+48;char*o=malloc(sz);snprintf(o,sz,"sage_rt_ffi_close(%s)",a);free(a);return o;}
                if (BM("ffi_sym")&&argc==2){char*a=aot_expr_boxed(aot,expr->as.call.args[0]);char*b=aot_expr_boxed(aot,expr->as.call.args[1]);size_t sz=strlen(a)+strlen(b)+48;char*o=malloc(sz);snprintf(o,sz,"sage_rt_ffi_sym(%s,%s)",a,b);free(a);free(b);return o;}
                if (BM("ffi_call")&&argc>=3){
                    char*l=aot_expr_boxed(aot,expr->as.call.args[0]);
                    char*f=aot_expr_boxed(aot,expr->as.call.args[1]);
                    char*r=aot_expr_boxed(aot,expr->as.call.args[2]);
                    char*as_=(argc>=4)?aot_expr_boxed(aot,expr->as.call.args[3]):strdup("sage_rt_nil()");
                    size_t sz=strlen(l)+strlen(f)+strlen(r)+strlen(as_)+64;
                    char*o=malloc(sz); snprintf(o,sz,"sage_rt_ffi_call(%s,%s,%s,%s)",l,f,r,as_);
                    free(l);free(f);free(r);free(as_);return o;
                }
                // -- inline assembly --
                if (BM("asm_arch")&&argc==0) return strdup("sage_rt_asm_arch()");
                if (BM("asm_exec")&&argc>=2){
                    size_t cap=64; for(int i=0;i<argc;i++){char*a=aot_expr_boxed(aot,expr->as.call.args[i]);cap+=strlen(a)+8;free(a);}
                    char*o=malloc(cap); int pos=sprintf(o,"sage_rt_asm_exec(%d,(SageValue[]){",argc);
                    for(int i=0;i<argc;i++){char*a=aot_expr_boxed(aot,expr->as.call.args[i]);pos+=sprintf(o+pos,"%s%s",i?",":"",a);free(a);}
                    sprintf(o+pos,"})"); return o;
                }
                // -- sizeof --
                if (BM("sizeof")&&argc==1){char*a=aot_expr_boxed(aot,expr->as.call.args[0]);size_t sz=strlen(a)+48;char*o=malloc(sz);snprintf(o,sz,"sage_rt_sizeof(%s)",a);free(a);return o;}
                // -- pointer arithmetic --
                if (BM("ptr_add")&&argc==2){char*a=aot_expr_boxed(aot,expr->as.call.args[0]);char*b=aot_expr_boxed(aot,expr->as.call.args[1]);size_t sz=strlen(a)+strlen(b)+48;char*o=malloc(sz);snprintf(o,sz,"sage_rt_ptr_add(%s,%s)",a,b);free(a);free(b);return o;}
                if (BM("ptr_sub")&&argc==2){char*a=aot_expr_boxed(aot,expr->as.call.args[0]);char*b=aot_expr_boxed(aot,expr->as.call.args[1]);size_t sz=strlen(a)+strlen(b)+64;char*o=malloc(sz);snprintf(o,sz,"sage_rt_ptr_add(%s,sage_rt_int(-SAGE_AS_INT64(%s)))",a,b);free(a);free(b);return o;}
                // -- Python FFI stubs --
                if (BM("python_init")||BM("python_eval")||BM("python_call")||BM("python_import")) {
                    return strdup("sage_rt_nil()");
                }
                if (BM("struct_def")&&argc==1){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);size_t sz=strlen(a)+64;char*o=malloc(sz);snprintf(o,sz,"sage_rt_struct_def(%s)",a);free(a);return o;}
                if (BM("struct_new")&&argc==1){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);size_t sz=strlen(a)+64;char*o=malloc(sz);snprintf(o,sz,"sage_rt_struct_new(%s)",a);free(a);return o;}
                if (BM("struct_get")&&argc==3){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);char*b=aot_expr(aot,expr->as.call.args[1],JIT_TYPE_UNKNOWN);char*c=aot_expr(aot,expr->as.call.args[2],JIT_TYPE_UNKNOWN);size_t sz=strlen(a)+strlen(b)+strlen(c)+64;char*o=malloc(sz);snprintf(o,sz,"sage_rt_struct_get(%s,%s,%s)",a,b,c);free(a);free(b);free(c);return o;}
                if (BM("struct_set")&&argc==4){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);char*b=aot_expr(aot,expr->as.call.args[1],JIT_TYPE_UNKNOWN);char*c=aot_expr(aot,expr->as.call.args[2],JIT_TYPE_UNKNOWN);char*d=aot_expr(aot,expr->as.call.args[3],JIT_TYPE_UNKNOWN);size_t sz=strlen(a)+strlen(b)+strlen(c)+strlen(d)+64;char*o=malloc(sz);snprintf(o,sz,"({sage_rt_struct_set(%s,%s,%s,%s);sage_rt_nil();})",a,b,c,d);free(a);free(b);free(c);free(d);return o;}
                if (BM("struct_size")&&argc==1){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);size_t sz=strlen(a)+64;char*o=malloc(sz);snprintf(o,sz,"sage_rt_struct_size(%s)",a);free(a);return o;}
                // -- Manual memory builtins --
                if (BM("mem_alloc")&&argc==1){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);size_t sz=strlen(a)+64;char*o=malloc(sz);snprintf(o,sz,"sage_rt_mem_alloc(%s)",a);free(a);return o;}
                if (BM("mem_free")&&argc==1){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);size_t sz=strlen(a)+64;char*o=malloc(sz);snprintf(o,sz,"({sage_rt_mem_free(%s);sage_rt_nil();})",a);free(a);return o;}
                if (BM("mem_read")&&argc==3){char*a=aot_expr_boxed(aot,expr->as.call.args[0]);char*b=aot_expr_boxed(aot,expr->as.call.args[1]);char*c=aot_expr_boxed(aot,expr->as.call.args[2]);size_t sz=strlen(a)+strlen(b)+strlen(c)+64;char*o=malloc(sz);snprintf(o,sz,"sage_rt_mem_read(%s,%s,%s)",a,b,c);free(a);free(b);free(c);return o;}
                if (BM("mem_write")&&argc==4){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);char*b=aot_expr(aot,expr->as.call.args[1],JIT_TYPE_UNKNOWN);char*c=aot_expr(aot,expr->as.call.args[2],JIT_TYPE_UNKNOWN);char*d=aot_expr(aot,expr->as.call.args[3],JIT_TYPE_UNKNOWN);size_t sz=strlen(a)+strlen(b)+strlen(c)+strlen(d)+64;char*o=malloc(sz);snprintf(o,sz,"({sage_rt_mem_write(%s,%s,%s,%s);sage_rt_nil();})",a,b,c,d);free(a);free(b);free(c);free(d);return o;}
                if (BM("precision")&&argc==2){
                    char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);
                    char*b=aot_expr(aot,expr->as.call.args[1],JIT_TYPE_UNKNOWN);
                    JitTypeTag bt=aot_infer_expr(aot,expr->as.call.args[1]);
                    // sage_rt_precision(SageValue, SageValue) - box int if needed
                    int bnb=(expr->as.call.args[1]->type==EXPR_VARIABLE&&jit_is_unboxed(bt));
                    char*bb=bnb?aot_box(bt,b):b;
                    size_t sz=strlen(a)+strlen(bb)+64;char*o=malloc(sz);
                    snprintf(o,sz,"sage_rt_precision(%s,%s)",a,bb);
                    if(bb!=b)free(bb);free(a);free(b);return o;
                }
                if (BM("tonumber")&&argc==1){char*a=aot_expr_boxed(aot,expr->as.call.args[0]);size_t sz=strlen(a)+64;char*o=malloc(sz);snprintf(o,sz,"sage_rt_tonumber(%s)",a);free(a);return o;}
                if (BM("join")&&argc==2){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);char*b=aot_expr(aot,expr->as.call.args[1],JIT_TYPE_UNKNOWN);size_t sz=strlen(a)+strlen(b)+64;char*o=malloc(sz);snprintf(o,sz,"sage_rt_array_join(%s,%s)",a,b);free(a);free(b);return o;}
                if (BM("string_join")&&argc==2){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);char*b=aot_expr(aot,expr->as.call.args[1],JIT_TYPE_UNKNOWN);size_t sz=strlen(a)+strlen(b)+64;char*o=malloc(sz);snprintf(o,sz,"sage_rt_array_join(%s,%s)",a,b);free(a);free(b);return o;}
                // Dict builtins (bare function form)
                if (BM("dict_has")&&argc==2){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);char*b=aot_expr_boxed(aot,expr->as.call.args[1]);size_t sz=strlen(a)+strlen(b)+64;char*o=malloc(sz);snprintf(o,sz,"sage_rt_bool(sage_rt_dict_has(%s,%s))",a,b);free(a);free(b);return o;}
                if (BM("dict_delete")&&argc==2){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);char*b=aot_expr(aot,expr->as.call.args[1],JIT_TYPE_UNKNOWN);size_t sz=strlen(a)+strlen(b)+64;char*o=malloc(sz);snprintf(o,sz,"({sage_rt_dict_remove(%s,%s);sage_rt_nil();})",a,b);free(a);free(b);return o;}
                if (BM("dict_get")&&argc==2){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);char*b=aot_expr(aot,expr->as.call.args[1],JIT_TYPE_UNKNOWN);size_t sz=strlen(a)+strlen(b)+64;char*o=malloc(sz);snprintf(o,sz,"sage_rt_dict_get(%s,%s)",a,b);free(a);free(b);return o;}
                if (BM("dict_keys")&&argc==1){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);size_t sz=strlen(a)+64;char*o=malloc(sz);snprintf(o,sz,"sage_rt_dict_keys(%s)",a);free(a);return o;}
                if (BM("dict_values")&&argc==1){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);size_t sz=strlen(a)+64;char*o=malloc(sz);snprintf(o,sz,"sage_rt_dict_values(%s)",a);free(a);return o;}
                if (BM("dict_len")&&argc==1){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);size_t sz=strlen(a)+64;char*o=malloc(sz);snprintf(o,sz,"sage_rt_dict_len(%s)",a);free(a);return o;}
                // Slice/range builtins
                if (BM("slice")&&argc==3){
                    char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);
                    char*b_raw=aot_expr(aot,expr->as.call.args[1],JIT_TYPE_INT);
                    char*d_raw=aot_expr(aot,expr->as.call.args[2],JIT_TYPE_INT);
                    JitTypeTag bt=aot_infer_expr(aot,expr->as.call.args[1]);
                    JitTypeTag dt=aot_infer_expr(aot,expr->as.call.args[2]);
                    char *b, *d;
                    if (bt==JIT_TYPE_UNKNOWN||strncmp(b_raw,"sage_rt_",8)==0) {
                        b=malloc(strlen(b_raw)+32); sprintf(b,"(int)SAGE_AS_INT64(%s)",b_raw); free(b_raw);
                    } else b=b_raw;
                    if (dt==JIT_TYPE_UNKNOWN||strncmp(d_raw,"sage_rt_",8)==0) {
                        d=malloc(strlen(d_raw)+32); sprintf(d,"(int)SAGE_AS_INT64(%s)",d_raw); free(d_raw);
                    } else d=d_raw;
                    size_t sz=strlen(a)+strlen(b)+strlen(d)+64;
                    char*o=malloc(sz); snprintf(o,sz,"sage_rt_array_slice(%s,%s,%s)",a,b,d);
                    free(a);free(b);free(d);return o;
                }
                if (BM("upper")&&argc==1){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);size_t sz=strlen(a)+64;char*o=malloc(sz);snprintf(o,sz,"sage_rt_str_upper(%s)",a);free(a);return o;}
                if (BM("lower")&&argc==1){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);size_t sz=strlen(a)+64;char*o=malloc(sz);snprintf(o,sz,"sage_rt_str_lower(%s)",a);free(a);return o;}
                if (BM("strip")&&argc==1){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);size_t sz=strlen(a)+64;char*o=malloc(sz);snprintf(o,sz,"sage_rt_str_strip(%s)",a);free(a);return o;}
                if (BM("replace")&&argc==3){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);char*b=aot_expr(aot,expr->as.call.args[1],JIT_TYPE_UNKNOWN);char*d=aot_expr(aot,expr->as.call.args[2],JIT_TYPE_UNKNOWN);size_t sz=strlen(a)+strlen(b)+strlen(d)+64;char*o=malloc(sz);snprintf(o,sz,"sage_rt_str_replace(%s,%s,%s)",a,b,d);free(a);free(b);free(d);return o;}
                if (BM("split")&&argc==2){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);char*b=aot_expr(aot,expr->as.call.args[1],JIT_TYPE_UNKNOWN);size_t sz=strlen(a)+strlen(b)+64;char*o=malloc(sz);snprintf(o,sz,"sage_rt_str_split(%s,%s)",a,b);free(a);free(b);return o;}
                if (BM("next")&&argc==1){
                    char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);
                    size_t sz=strlen(a)+128;char*o=malloc(sz);
                    snprintf(o,sz,"sage_rt_call_fn(%s,0,NULL)",a);
                    free(a);return o;
                }
                if (BM("push")&&argc==2){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);char*b=aot_expr_boxed(aot,expr->as.call.args[1]);size_t sz=strlen(a)+strlen(b)+64;char*o=malloc(sz);snprintf(o,sz,"({sage_rt_array_push(%s,%s);sage_rt_nil();})",a,b);free(a);free(b);return o;}
                if (BM("append")&&argc==2){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);char*b=aot_expr_boxed(aot,expr->as.call.args[1]);size_t sz=strlen(a)+strlen(b)+64;char*o=malloc(sz);snprintf(o,sz,"({sage_rt_array_push(%s,%s);sage_rt_nil();})",a,b);free(a);free(b);return o;}
                if (BM("pop")&&argc==1){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);size_t sz=strlen(a)+64;char*o=malloc(sz);snprintf(o,sz,"sage_rt_array_pop(%s)",a);free(a);return o;}
                if (BM("abs")&&argc==1){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);size_t sz=strlen(a)*3+128;char*o=malloc(sz);snprintf(o,sz,"(SAGE_IS_INT(%s)?sage_rt_int(llabs(%s.as.integer)):sage_rt_float(fabs(%s.as.number)))",a,a,a);free(a);return o;}
                if (BM("min")&&argc==2){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);char*b=aot_expr(aot,expr->as.call.args[1],JIT_TYPE_UNKNOWN);size_t sz=strlen(a)+strlen(b)+128;char*o=malloc(sz);snprintf(o,sz,"(sage_rt_less(%s,%s)?(%s):(%s))",a,b,a,b);free(a);free(b);return o;}
                if (BM("max")&&argc==2){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);char*b=aot_expr(aot,expr->as.call.args[1],JIT_TYPE_UNKNOWN);size_t sz=strlen(a)+strlen(b)+128;char*o=malloc(sz);snprintf(o,sz,"(sage_rt_less(%s,%s)?(%s):(%s))",a,b,b,a);free(a);free(b);return o;}
                if (BM("print")&&argc>=1){
                    // multi-arg print with spaces
                    size_t total=64; for(int i=0;i<argc;i++){char*a=aot_expr(aot,expr->as.call.args[i],JIT_TYPE_UNKNOWN);total+=strlen(a)+32;free(a);}
                    char*o=malloc(total); int pos=sprintf(o,"({");
                    for(int i=0;i<argc;i++){char*a=aot_expr(aot,expr->as.call.args[i],JIT_TYPE_UNKNOWN);pos+=sprintf(o+pos,"if(%d)fputs(" ",stdout);sage_rt_print(%s);",i,a);free(a);}
                    sprintf(o+pos,"sage_rt_nil();})"); return o;
                }
                #undef BM
                // Only use sage_rt_call_fn for SageValue function vars (not known C procs)
                if (!aot_is_known_proc(aot, raw, rawlen)) {
                    char fname_buf[256]; int flen2=rawlen<255?rawlen:255;
                    memcpy(fname_buf,raw,flen2); fname_buf[flen2]='\0';
                    JitTypeTag cv = aot_get_var_type(aot, fname_buf);
                    // If type is unknown and it's not a known proc, treat as SageValue fn
                    if (cv == JIT_TYPE_UNKNOWN || cv == JIT_TYPE_MIXED) {
                        char* cname = aot_cname(raw, rawlen);
                        size_t total = strlen(cname) + 256;
                        for(int i=0;i<argc;i++){char*a=aot_expr(aot,expr->as.call.args[i],JIT_TYPE_UNKNOWN);total+=strlen(a)+8;free(a);}
                        char* out = malloc(total);
                        int pos = 0;
                        if(argc > 0){
                            char* argbuf = malloc(total);
                            int ap = sprintf(argbuf,"(SageValue[]){");
                            for(int i=0;i<argc;i++){
                                JitTypeTag at=aot_infer_expr(aot,expr->as.call.args[i]);
                                char*a=aot_expr(aot,expr->as.call.args[i],JIT_TYPE_UNKNOWN);
                                int nb=(expr->as.call.args[i]->type==EXPR_VARIABLE&&jit_is_unboxed(at));
                                char*ab=nb?aot_box(at,a):a;
                                ap+=sprintf(argbuf+ap,"%s%s",i?",":"",ab);
                                if(ab!=a)free(ab); free(a);
                            }
                            sprintf(argbuf+ap,"}");
                            pos=sprintf(out,"sage_rt_call_fn(%s,%d,%s)",cname,argc,argbuf);
                            free(argbuf);
                        } else {
                            pos=sprintf(out,"sage_rt_call_fn(%s,0,NULL)",cname);
                        }
                        (void)pos; free(cname); return out;
                    }
                }
                // User proc call — use inferred types BUT classes/structs always take SageValue
                // Detect if this is a class/struct constructor by checking known_procs source
                // (class/struct ctors always emit SageValue params, not raw typed ones)
                // Check if this is a class/struct constructor (always use SageValue params)
                int is_ctor = 0;
                {
                    char nbuf[256]; int nl=rawlen<255?rawlen:255;
                    memcpy(nbuf,raw,nl); nbuf[nl]='\0';
                    // Check both raw name ("Point") and cname ("sg_Point")
                    char cname_buf[260]="sg_"; strncat(cname_buf,nbuf,255);
                    for(int _ci=0;_ci<aot->known_ctor_count;_ci++){
                        if(strcmp(aot->known_ctors[_ci],nbuf)==0||
                           strcmp(aot->known_ctors[_ci],cname_buf)==0){ is_ctor=1; break; }
                    }
                }
                char* fname=aot_cname(raw,rawlen);
                // Inside module body: rewrite sg_procname to sg_MODULENAME_sg_procname
                // so recursive/cross-function calls within a module use the correct symbol
                if (aot->in_module_body && aot->current_module_prefix[0]) {
                    char prefixed[256]; snprintf(prefixed,sizeof(prefixed),"%s%s",aot->current_module_prefix,fname);
                    int found_pfx = 0;
                    for(int _ki=0;_ki<aot->known_proc_count;_ki++){
                        if(strcmp(aot->known_procs[_ki],prefixed)==0){ found_pfx=1; break; }
                    }
                    if (found_pfx) { free(fname); fname=strdup(prefixed); }
                }
                size_t total=strlen(fname)+32;
                // Count total params including defaulted ones for buffer sizing
                int emit_argc_pre = argc;
                for (int _di=0; _di<aot->proc_default_count; _di++) {
                    if (strcmp(aot->proc_defaults[_di].proc_cname, fname)==0 &&
                        aot->proc_defaults[_di].param_idx >= emit_argc_pre &&
                        aot->proc_defaults[_di].param_idx < 32) // safety cap
                        emit_argc_pre = aot->proc_defaults[_di].param_idx + 1;
                }
                for(int i=0;i<argc;i++){
                    JitTypeTag pt=is_ctor?JIT_TYPE_UNKNOWN:aot_param_type(aot,raw,rawlen,i);
                    JitTypeTag ah=jit_is_unboxed(pt)?pt:JIT_TYPE_UNKNOWN;
                    char*a=aot_expr(aot,expr->as.call.args[i],ah);
                    if(jit_is_unboxed(pt)){
                        JitTypeTag at=aot_infer_expr(aot,expr->as.call.args[i]);
                        if(!jit_is_unboxed(at)){char*bx=aot_box(pt,a);total+=strlen(bx);free(bx);}
                    }
                    total+=strlen(a)+4; free(a);
                }
                // Add space for default values
                for (int i=argc; i<emit_argc_pre; i++) total += 320;
                char* out=malloc(total);
                int pos=sprintf(out,"%s(",fname);
                // Determine total param count (provided + defaulted)
                int emit_argc = emit_argc_pre;
                for(int i=0;i<emit_argc;i++){
                    if (i < argc) {
                        JitTypeTag pt=is_ctor?JIT_TYPE_UNKNOWN:aot_param_type(aot,raw,rawlen,i);
                        JitTypeTag ah=(!is_ctor&&jit_is_unboxed(pt))?pt:JIT_TYPE_UNKNOWN;
                        char*a=aot_expr(aot,expr->as.call.args[i],ah);
                        char*fa=a;
                        if(!is_ctor && jit_is_unboxed(pt)){
                            // Typed param: box incoming SageValue
                            JitTypeTag at=aot_infer_expr(aot,expr->as.call.args[i]);
                            if(!jit_is_unboxed(at)){fa=aot_box(pt,a);}
                        } else if (!is_ctor && pt == JIT_TYPE_UNKNOWN) {
                            // Untyped param (SageValue expected): box any raw scalar.
                            // The arg must end up as a SageValue. If its inferred type
                            // is unboxed (int/float/bool) and the emitted C is a raw
                            // scalar (not already a sage_rt_* boxed expression), box it.
                            // This covers literals (identity(10)) and bare variables
                            // alike. Compound array/dict/tuple temps emit as ({...;})
                            // which is already a SageValue, so they are left as-is.
                            JitTypeTag at=aot_infer_expr(aot,expr->as.call.args[i]);
                            int already_boxed = (strncmp(a,"sage_rt_",8)==0 || strncmp(a,"({",2)==0);
                            if (jit_is_unboxed(at) && !already_boxed) fa = aot_box(at, a);
                        }
                        pos+=sprintf(out+pos,"%s%s",i>0?", ":"",fa);
                        if(fa!=a)free(fa); free(a);
                    } else {
                        // Missing arg — compile default with correct param type hint
                        char* compiled_dflt = NULL;
                        for (int _di=0; _di<aot->proc_default_count; _di++) {
                            if (strcmp(aot->proc_defaults[_di].proc_cname, fname)==0 &&
                                aot->proc_defaults[_di].param_idx == i) {
                                if (aot->proc_defaults[_di].default_ast) {
                                    JitTypeTag pt = is_ctor ? JIT_TYPE_UNKNOWN
                                                           : aot_param_type(aot,raw,rawlen,i);
                                    JitTypeTag ah = jit_is_unboxed(pt)?pt:JIT_TYPE_UNKNOWN;
                                    compiled_dflt = aot_expr(aot, aot->proc_defaults[_di].default_ast, ah);
                                } else {
                                    compiled_dflt = strdup(aot->proc_defaults[_di].default_expr);
                                }
                                break;
                            }
                        }
                        const char* dflt = compiled_dflt ? compiled_dflt : "sage_rt_nil()";
                        pos+=sprintf(out+pos,"%s%s",i>0?", ":"",dflt);
                        if (compiled_dflt) free(compiled_dflt);
                    }
                }
                sprintf(out+pos,")");
                free(fname); return out;
            }
            // -- Method call: obj.method(args) --
            if (expr->as.call.callee && expr->as.call.callee->type == EXPR_GET) {
                Expr* ge = expr->as.call.callee;
                // Handle super.method() calls — parser emits EXPR_SUPER as the direct callee
                if (ge->as.get.object && ge->as.get.object->type == EXPR_SUPER) {
                    char mn[256]; int ml=ge->as.get.property.length<255?ge->as.get.property.length:255;
                    memcpy(mn,ge->as.get.property.start,ml); mn[ml]='\0';
                    size_t total=512;
                    for(int i=0;i<argc;i++){char*a=aot_expr(aot,expr->as.call.args[i],JIT_TYPE_UNKNOWN);total+=strlen(a)+8;free(a);}
                    char* out=malloc(total); int pos=0;
                    // Static dispatch: call the parent's method directly by C name.
                    // This fixes multi-level inheritance: each level calls its own
                    // statically-resolved parent, so C(B(A)) works without MRO confusion.
                    if (aot->current_parent_cname[0]) {
                        char* mn_cn = aot_cname(mn, strlen(mn));
                        if(argc>0){
                            char* ab=malloc(total); int ap=sprintf(ab,"(SageValue[]){");
                            for(int i=0;i<argc;i++){char*a=aot_expr(aot,expr->as.call.args[i],JIT_TYPE_UNKNOWN);ap+=sprintf(ab+ap,"%s%s",i?",":"",a);free(a);}
                            sprintf(ab+ap,"}");
                            pos=sprintf(out,"%s_%s(_self,%d,%s)",aot->current_parent_cname,mn_cn,argc,ab);
                            free(ab);
                        } else {
                            pos=sprintf(out,"%s_%s(_self,0,NULL)",aot->current_parent_cname,mn_cn);
                        }
                        free(mn_cn);
                    } else {
                        // No known parent — fall back to runtime lookup
                        if(argc>0){
                            char* ab=malloc(total); int ap=sprintf(ab,"(SageValue[]){");
                            for(int i=0;i<argc;i++){char*a=aot_expr(aot,expr->as.call.args[i],JIT_TYPE_UNKNOWN);ap+=sprintf(ab+ap,"%s%s",i?",":"",a);free(a);}
                            sprintf(ab+ap,"}");
                            pos=sprintf(out,"sage_rt_method_call_super(_self,\"%s\",%d,%s)",mn,argc,ab);
                            free(ab);
                        } else pos=sprintf(out,"sage_rt_method_call_super(_self,\"%s\",0,NULL)",mn);
                    }
                    (void)pos; return out;
                }
                char* _mobj_raw = aot_expr(aot, ge->as.get.object, JIT_TYPE_UNKNOWN);
                JitTypeTag _mot = aot_infer_expr(aot, ge->as.get.object);
                // --- ADT constructor call: EnumName.VariantName(args) ---
                // Detect when object is a known enum namespace (DICT type from known_enums)
                if (_mot == JIT_TYPE_DICT && ge->as.get.object->type == EXPR_VARIABLE) {
                    char _en_raw[256]={0};
                    int _enl = ge->as.get.object->as.variable.name.length < 255
                             ? ge->as.get.object->as.variable.name.length : 255;
                    memcpy(_en_raw, ge->as.get.object->as.variable.name.start, _enl);
                    _en_raw[_enl] = '\0';
                    int _is_known_enum = 0;
                    for (int _ei=0; _ei<aot->known_enum_count; _ei++) {
                        if (strcmp(aot->known_enums[_ei], _en_raw)==0) { _is_known_enum=1; break; }
                    }
                    if (_is_known_enum) {
                        // Emit direct constructor call: sg_EnumName_sg_VariantName(arg0, arg1, ...)
                        char* _ecn = aot_cname(_en_raw, strlen(_en_raw));
                        char* _vcn = aot_cname(ge->as.get.property.start, ge->as.get.property.length);
                        size_t _adt_sz = strlen(_ecn)+strlen(_vcn)+64;
                        for(int _ai=0;_ai<argc;_ai++){char*_ta=aot_expr(aot,expr->as.call.args[_ai],JIT_TYPE_UNKNOWN);_adt_sz+=strlen(_ta)+8;free(_ta);}
                        char* _adt_out = malloc(_adt_sz);
                        int _adt_pos = snprintf(_adt_out, _adt_sz, "%s_%s(", _ecn, _vcn);
                        for(int _ai=0;_ai<argc;_ai++){
                            char*_ta=aot_expr(aot,expr->as.call.args[_ai],JIT_TYPE_UNKNOWN);
                            _adt_pos+=sprintf(_adt_out+_adt_pos,"%s%s",_ai?", ":"",_ta);
                            free(_ta);
                        }
                        if (argc==0) sprintf(_adt_out+_adt_pos,")");
                        else sprintf(_adt_out+_adt_pos,")");
                        free(_ecn); free(_vcn); free(_mobj_raw);
                        return _adt_out;
                    }
                }
                // ── Module direct call: module.proc(args) → sg_MODULE_sg_PROC(args) ─
                if (_mot == JIT_TYPE_DICT && ge->as.get.object->type == EXPR_VARIABLE) {
                    char _mod_raw[256]={0};
                    int _modl = ge->as.get.object->as.variable.name.length < 255
                              ? ge->as.get.object->as.variable.name.length : 255;
                    memcpy(_mod_raw, ge->as.get.object->as.variable.name.start, _modl);
                    _mod_raw[_modl] = '\0';
                    // Check if this is a known imported module
                    int _is_module = 0;
                    for (int _mi=0; _mi<aot->imported_module_count; _mi++) {
                        if (strcmp(aot->imported_modules[_mi], _mod_raw)==0) { _is_module=1; break; }
                    }
                    // python.* — native Python FFI (handled regardless of the
                    // normal module-import bookkeeping).
                    if (strcmp(_mod_raw,"python")==0) {
                        const char* pn = ge->as.get.property.start;
                        int pl = ge->as.get.property.length;
                        #define _PYM(s) ((int)strlen(s)==pl && memcmp(pn,s,pl)==0)
                        char* a0 = argc>0 ? aot_expr_boxed(aot,expr->as.call.args[0]) : strdup("sage_rt_nil()");
                        if (_PYM("import")) { size_t z=strlen(a0)+48; char*o=malloc(z); snprintf(o,z,"sage_rt_py_import(%s)",a0); free(a0); free(_mobj_raw); return o; }
                        if (_PYM("eval"))   { size_t z=strlen(a0)+48; char*o=malloc(z); snprintf(o,z,"sage_rt_py_eval(%s)",a0); free(a0); free(_mobj_raw); return o; }
                        if (_PYM("exec"))   { size_t z=strlen(a0)+48; char*o=malloc(z); snprintf(o,z,"sage_rt_py_exec(%s)",a0); free(a0); free(_mobj_raw); return o; }
                        if (_PYM("getattr")){ char* a1=argc>1?aot_expr_boxed(aot,expr->as.call.args[1]):strdup("sage_rt_nil()"); size_t z=strlen(a0)+strlen(a1)+48; char*o=malloc(z); snprintf(o,z,"sage_rt_py_getattr(%s,%s)",a0,a1); free(a0);free(a1);free(_mobj_raw); return o; }
                        if (_PYM("call")) {
                            // python.call(obj, method, ...args)
                            char* a1=argc>1?aot_expr_boxed(aot,expr->as.call.args[1]):strdup("sage_rt_nil()");
                            int extra=argc>2?argc-2:0;
                            size_t z=strlen(a0)+strlen(a1)+96;
                            char** ev=malloc(sizeof(char*)*(extra>0?extra:1));
                            for(int i=0;i<extra;i++){ev[i]=aot_expr_boxed(aot,expr->as.call.args[i+2]);z+=strlen(ev[i])+4;}
                            char*o=malloc(z); int p;
                            if(extra>0){
                                p=sprintf(o,"sage_rt_py_call(%s,%s,%d,(SageValue[]){",a0,a1,extra);
                                for(int i=0;i<extra;i++){p+=sprintf(o+p,"%s%s",i?",":"",ev[i]);free(ev[i]);}
                                sprintf(o+p,"})");
                            } else {
                                sprintf(o,"sage_rt_py_call(%s,%s,0,NULL)",a0,a1);
                            }
                            free(ev);free(a0);free(a1);free(_mobj_raw); return o;
                        }
                        if (_PYM("invoke")) {
                            int extra=argc>1?argc-1:0;
                            size_t z=strlen(a0)+96;
                            char** ev=malloc(sizeof(char*)*(extra>0?extra:1));
                            for(int i=0;i<extra;i++){ev[i]=aot_expr_boxed(aot,expr->as.call.args[i+1]);z+=strlen(ev[i])+4;}
                            char*o=malloc(z); int p;
                            if(extra>0){
                                p=sprintf(o,"sage_rt_py_invoke(%s,%d,(SageValue[]){",a0,extra);
                                for(int i=0;i<extra;i++){p+=sprintf(o+p,"%s%s",i?",":"",ev[i]);free(ev[i]);}
                                sprintf(o+p,"})");
                            } else {
                                sprintf(o,"sage_rt_py_invoke(%s,0,NULL)",a0);
                            }
                            free(ev);free(a0);free(_mobj_raw); return o;
                        }
                        free(a0);
                        #undef _PYM
                    }
                    if (_is_module) {
                        // thread.spawn(fn, ...args) — emit the (argc, SageValue[]) form
                        // its synchronous wrapper expects.
                        if (strcmp(_mod_raw,"thread")==0 &&
                            (int)ge->as.get.property.length==5 &&
                            memcmp(ge->as.get.property.start,"spawn",5)==0) {
                            size_t _ssz=96; for(int _ai=0;_ai<argc;_ai++){char*_ta=aot_expr_boxed(aot,expr->as.call.args[_ai]);_ssz+=strlen(_ta)+8;free(_ta);}
                            char* _so=malloc(_ssz);
                            int _sp=sprintf(_so,"sg_thread_sg_spawn(%d,(SageValue[]){",argc);
                            if(argc==0) _sp+=sprintf(_so+_sp,"sage_rt_nil()");
                            for(int _ai=0;_ai<argc;_ai++){char*_ta=aot_expr_boxed(aot,expr->as.call.args[_ai]);_sp+=sprintf(_so+_sp,"%s%s",_ai?",":"",_ta);free(_ta);}
                            sprintf(_so+_sp,"})");
                            free(_mobj_raw);
                            return _so;
                        }
                        // Look up the full C prefix for this short module name
                        const char* _full_pfx = NULL;
                        for (int _pmi=0; _pmi<aot->mod_prefix_map_count; _pmi++) {
                            if (strcmp(aot->mod_prefix_map[_pmi].short_name, _mod_raw)==0) {
                                _full_pfx = aot->mod_prefix_map[_pmi].full_prefix; break;
                            }
                        }
                        char* _fn_c = aot_cname(ge->as.get.property.start, ge->as.get.property.length);
                        size_t _msz = 64;
                        if (_full_pfx) _msz += strlen(_full_pfx)+strlen(_fn_c);
                        else _msz += strlen(_mod_raw)+strlen(_fn_c)+8;
                        for(int _ai=0;_ai<argc;_ai++){char*_ta=aot_expr(aot,expr->as.call.args[_ai],JIT_TYPE_UNKNOWN);_msz+=strlen(_ta)+8;free(_ta);}
                        char* _mout = malloc(_msz);
                        // Direct C call using full prefix: sg_std_atomic_sg_cas(args)
                        // full_pfx ends with _ and fn_c starts with sg_: prefix+fn_c = "sg_arrays_sg_"+sg_sort = sg_arrays_sg_sg_sort
                        // This matches the module compilation naming convention (mod_prefix + cname)
                        int _mp;
                        if (_full_pfx)
                            _mp = snprintf(_mout, _msz, "%s%s(", _full_pfx, _fn_c);
                        else
                            _mp = snprintf(_mout, _msz, "sg_%s_%s(", _mod_raw, _fn_c);
                        for(int _ai=0;_ai<argc;_ai++){
                            char*_ta=aot_expr_boxed(aot,expr->as.call.args[_ai]);
                            _mp+=sprintf(_mout+_mp, "%s%s", _ai?",":"", _ta);
                            free(_ta);
                        }
                        sprintf(_mout+_mp, ")");
                        free(_fn_c); free(_mobj_raw);
                        return _mout;
                    }
                }
                // Box raw string/int/float/bool variables — methods always expect SageValue
                char* _mobj = (ge->as.get.object->type == EXPR_VARIABLE && jit_is_unboxed(_mot))
                    ? aot_box(_mot, _mobj_raw) : _mobj_raw;
                char* _mn  = aot_escape(ge->as.get.property.start);
                _mn[ge->as.get.property.length] = '\0';
                /* Use safe buffer: strlen of all parts + 256 headroom */
                #define _MOUT(...) ({ size_t _sz = strlen(_mobj) + 256; __VA_ARGS__; char* _o = malloc(_sz); _o; })
                #define _MO(fmt,...) do { size_t _sz=strlen(_mobj)+256; if(argc>0){char*_ta=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);_sz+=strlen(_ta);free(_ta);} char*_o=malloc(_sz); sprintf(_o, fmt, ## __VA_ARGS__); free(_mobj);free(_mn); return _o; } while(0)
                /* Build a size-safe output buffer for method call results */
                size_t _mbufsz = strlen(_mobj) + 512;
                for(int _mi=0;_mi<argc;_mi++){char*_ta=aot_expr(aot,expr->as.call.args[_mi],JIT_TYPE_UNKNOWN);_mbufsz+=strlen(_ta)+8;free(_ta);}
                #undef _MO
                #define _MO1(fn) do{char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);char*o=malloc(strlen(_mobj)+strlen(a)+256);sprintf(o,fn"(%s,%s)",_mobj,a);free(_mobj);free(_mn);free(a);return o;}while(0)
                #define _MO0(fn) do{char*o=malloc(strlen(_mobj)+256);sprintf(o,fn"(%s)",_mobj);free(_mobj);free(_mn);return o;}while(0)
                if (!strcmp(_mn,"upper")&&argc==0)      _MO0("sage_rt_str_upper");
                if (!strcmp(_mn,"lower")&&argc==0)      _MO0("sage_rt_str_lower");
                if ((!strcmp(_mn,"strip")||!strcmp(_mn,"trim"))&&argc==0) _MO0("sage_rt_str_strip");
                if (!strcmp(_mn,"split")&&argc==1)      _MO1("sage_rt_str_split");
                if ((!strcmp(_mn,"startswith")||!strcmp(_mn,"starts_with"))&&argc==1) _MO1("sage_rt_str_startswith");
                if ((!strcmp(_mn,"endswith")||!strcmp(_mn,"ends_with"))&&argc==1) _MO1("sage_rt_str_endswith");
                if (!strcmp(_mn,"find")&&argc==1)       _MO1("sage_rt_str_find");
                if (!strcmp(_mn,"pop")&&argc==0)        _MO0("sage_rt_array_pop");
                if (!strcmp(_mn,"join")&&argc==1){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);size_t _cs=strlen(_mobj)+strlen(a)+256;char*o=malloc(_cs);snprintf(o,_cs,"sage_rt_array_join(%s,%s)",_mobj,a);free(_mobj);free(_mn);free(a);return o;}
                if ((!strcmp(_mn,"index_of")||!strcmp(_mn,"indexOf"))&&argc==1){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);size_t _cs=strlen(_mobj)+strlen(a)+256;char*o=malloc(_cs);snprintf(o,_cs,"sage_rt_array_index_of(%s,%s)",_mobj,a);free(_mobj);free(_mn);free(a);return o;}
                if (!strcmp(_mn,"reverse")&&argc==0){size_t _cs=strlen(_mobj)+256;char*o=malloc(_cs);snprintf(o,_cs,"sage_rt_array_reverse(%s)",_mobj);free(_mobj);free(_mn);return o;}
                if (!strcmp(_mn,"sort")&&argc==0){size_t _cs=strlen(_mobj)+256;char*o=malloc(_cs);snprintf(o,_cs,"sage_rt_array_sort(%s)",_mobj);free(_mobj);free(_mn);return o;}
                if (!strcmp(_mn,"slice")&&argc==2){
                    char*a_raw=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_INT);
                    char*b_raw=aot_expr(aot,expr->as.call.args[1],JIT_TYPE_INT);
                    JitTypeTag at2=aot_infer_expr(aot,expr->as.call.args[0]);
                    JitTypeTag bt2=aot_infer_expr(aot,expr->as.call.args[1]);
                    char *a2, *b2;
                    if (at2==JIT_TYPE_UNKNOWN||strncmp(a_raw,"sage_rt_",8)==0) {
                        a2=malloc(strlen(a_raw)+32); sprintf(a2,"(int)SAGE_AS_INT64(%s)",a_raw); free(a_raw);
                    } else a2=a_raw;
                    if (bt2==JIT_TYPE_UNKNOWN||strncmp(b_raw,"sage_rt_",8)==0) {
                        b2=malloc(strlen(b_raw)+32); sprintf(b2,"(int)SAGE_AS_INT64(%s)",b_raw); free(b_raw);
                    } else b2=b_raw;
                    size_t _cs=strlen(_mobj)+strlen(a2)+strlen(b2)+64;
                    char*o=malloc(_cs); snprintf(o,_cs,"sage_rt_array_slice(%s,%s,%s)",_mobj,a2,b2);
                    free(_mobj);free(_mn);free(a2);free(b2);return o;
                }
                if ((!strcmp(_mn,"get_or")||!strcmp(_mn,"get"))&&argc==2){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);char*b=aot_expr(aot,expr->as.call.args[1],JIT_TYPE_UNKNOWN);size_t _cs=strlen(_mobj)+strlen(a)+strlen(b)+256;char*o=malloc(_cs);snprintf(o,_cs,"sage_rt_dict_get_or(%s,%s,%s)",_mobj,a,b);free(_mobj);free(_mn);free(a);free(b);return o;}
                if (!strcmp(_mn,"keys")&&argc==0)       _MO0("sage_rt_dict_keys");
                if (!strcmp(_mn,"values")&&argc==0)     _MO0("sage_rt_dict_values");
                if (!strcmp(_mn,"contains_key")&&argc==1){ char*a=aot_expr_boxed(aot,expr->as.call.args[0]);size_t _cs=strlen(_mobj)+strlen(a)+256;char*o=malloc(_cs);snprintf(o,_cs,"sage_rt_bool(sage_rt_dict_has(%s,%s))",_mobj,a);free(_mobj);free(_mn);free(a);return o;}
                if (!strcmp(_mn,"remove")&&argc==1){ char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);size_t _cs=strlen(_mobj)+strlen(a)+256;char*o=malloc(_cs);snprintf(o,_cs,"({sage_rt_dict_remove(%s,%s);sage_rt_nil();})",_mobj,a);free(_mobj);free(_mn);free(a);return o;}
                if (!strcmp(_mn,"get")&&argc==2){ char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);char*b=aot_expr(aot,expr->as.call.args[1],JIT_TYPE_UNKNOWN);size_t _cs=strlen(_mobj)+strlen(a)+strlen(b)+256;char*o=malloc(_cs);snprintf(o,_cs,"sage_rt_dict_get_or(%s,%s,%s)",_mobj,a,b);free(_mobj);free(_mn);free(a);free(b);return o;}
                if (!strcmp(_mn,"get")&&argc==1){ char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);size_t _cs=strlen(_mobj)+strlen(a)+256;char*o=malloc(_cs);snprintf(o,_cs,"sage_rt_dict_get(%s,%s)",_mobj,a);free(_mobj);free(_mn);free(a);return o;}
                if (!strcmp(_mn,"contains")&&argc==1){
                    JitTypeTag _cot=aot_infer_expr(aot,ge->as.get.object);
                    char*a=aot_expr_boxed(aot,expr->as.call.args[0]);
                    size_t _csz=strlen(_mobj)*3+strlen(a)*2+256;
                    char*o=malloc(_csz);
                    if(_cot==JIT_TYPE_ARRAY||_cot==JIT_TYPE_DICT)
                        snprintf(o,_csz,"sage_rt_array_contains(%s,%s)",_mobj,a);
                    else if(_cot==JIT_TYPE_STRING)
                        snprintf(o,_csz,"sage_rt_bool(sage_rt_str_find(%s,%s).as.integer>=0)",_mobj,a);
                    else
                        // Unknown object type: dispatch at runtime — string uses find,
                        // anything else (arrays) uses array_contains.
                        snprintf(o,_csz,"(SAGE_IS_STRING(%s)?sage_rt_bool(sage_rt_str_find(%s,%s).as.integer>=0):sage_rt_array_contains(%s,%s))",_mobj,_mobj,a,_mobj,a);
                    free(_mobj);free(_mn);free(a);return o;
                }
                if (!strcmp(_mn,"replace")&&argc==2){char*a=aot_expr(aot,expr->as.call.args[0],JIT_TYPE_UNKNOWN);char*b=aot_expr(aot,expr->as.call.args[1],JIT_TYPE_UNKNOWN);size_t _cs=strlen(_mobj)+strlen(a)+strlen(b)+256;char*o=malloc(_cs);snprintf(o,_cs,"sage_rt_str_replace(%s,%s,%s)",_mobj,a,b);free(_mobj);free(_mn);free(a);free(b);return o;}
                if (!strcmp(_mn,"push")&&argc==1){char*a=aot_expr_boxed(aot,expr->as.call.args[0]);size_t _cs=strlen(_mobj)+strlen(a)+256;char*o=malloc(_cs);snprintf(o,_cs,"({sage_rt_array_push(%s,%s);sage_rt_nil();})",_mobj,a);free(_mobj);free(_mn);free(a);return o;}
                if (!strcmp(_mn,"length")&&argc==0){size_t _cs=strlen(_mobj)+256;char*o=malloc(_cs);snprintf(o,_cs,"sage_rt_int(sage_rt_str_len(%s))",_mobj);free(_mobj);free(_mn);return o;}
                #undef _MO0
                #undef _MO1
                #undef _MOUT
                /* _mbufsz is a local var, not a macro */
                // Generic method call via runtime. If the receiver is a wrapped
                // Python object at runtime, dispatch to the Python FFI instead
                // (handles math.sqrt(x), json.loads(s), etc.). The object is
                // evaluated once into a temp to avoid double side effects.
                size_t total=strlen(_mobj)+strlen(_mn)+256;
                for(int i=0;i<argc;i++){char*a=aot_expr(aot,expr->as.call.args[i],JIT_TYPE_UNKNOWN);total+=strlen(a)+4;free(a);}
                char* out=malloc(total);
                char* argbuf=NULL;
                if(argc>0){
                    char** aa=malloc(argc*sizeof(char*));
                    size_t al=32;
                    for(int i=0;i<argc;i++){aa[i]=aot_expr_boxed(aot,expr->as.call.args[i]);al+=strlen(aa[i])+4;}
                    argbuf=malloc(al);
                    int ap=sprintf(argbuf,"(SageValue[]){");
                    for(int i=0;i<argc;i++) ap+=sprintf(argbuf+ap,"%s%s",i?",":"",aa[i]);
                    sprintf(argbuf+ap,"}");
                    for(int i=0;i<argc;i++) free(aa[i]); free(aa);
                }
                if(argc>0)
                    sprintf(out,"({SageValue _mo=%s; sage_rt_py_is_obj(_mo)?sage_rt_py_method(_mo,\"%s\",%d,%s):sage_rt_method_call(_mo,\"%s\",%d,%s);})",
                            _mobj,_mn,argc,argbuf,_mn,argc,argbuf);
                else
                    sprintf(out,"({SageValue _mo=%s; sage_rt_py_is_obj(_mo)?sage_rt_py_method(_mo,\"%s\",0,NULL):sage_rt_method_call(_mo,\"%s\",0,NULL);})",
                            _mobj,_mn,_mn);
                if(argbuf)free(argbuf);
                if(_mobj!=_mobj_raw)free(_mobj_raw); free(_mobj); free(_mn); return out;
            }
            // Callee is an arbitrary expression (e.g. d["fn"](x), arr[0](x),
            // (cond ? f : g)(x)). Evaluate it to a SageValue function and call it.
            {
                char* callee = aot_expr_boxed(aot, expr->as.call.callee);
                size_t bufsz = strlen(callee) + 64;
                char* argbuf = NULL;
                if (argc > 0) {
                    size_t asz = 16;
                    char** av = malloc(sizeof(char*) * argc);
                    for (int i = 0; i < argc; i++) { av[i] = aot_expr_boxed(aot, expr->as.call.args[i]); asz += strlen(av[i]) + 4; }
                    argbuf = malloc(asz);
                    int p = sprintf(argbuf, "(SageValue[]){");
                    for (int i = 0; i < argc; i++) { p += sprintf(argbuf + p, "%s%s", i?",":"", av[i]); free(av[i]); }
                    sprintf(argbuf + p, "}");
                    free(av);
                    bufsz += strlen(argbuf);
                }
                char* out = malloc(bufsz);
                if (argc > 0) sprintf(out, "sage_rt_call_fn(%s,%d,%s)", callee, argc, argbuf);
                else          sprintf(out, "sage_rt_call_fn(%s,0,NULL)", callee);
                free(callee); if (argbuf) free(argbuf);
                return out;
            }
        }

        case EXPR_INTERP: {
            // Compile-time string interpolation: scan template, parse each {expr},
            // emit: sage_rt_string_concat(sage_rt_string_concat("literal", tostring(expr)), ...)
            const char* tmpl = expr->as.interp.template_str ? expr->as.interp.template_str : "";
            int tlen = expr->as.interp.template_len;
            if (tlen <= 0) tlen = (int)strlen(tmpl);

            // Build a list of parts: (is_literal, text/expr_src)
            // Then fold them into nested sage_rt_string_concat calls
            #define MAX_PARTS 64
            typedef struct { int is_lit; char* s; } InterpPart;
            InterpPart parts[MAX_PARTS]; int nparts = 0;

            int i = 0;
            while (i < tlen && nparts < MAX_PARTS) {
                if (tmpl[i] == '{' && (i == 0 || tmpl[i-1] != '\\')) {
                    i++; // skip {
                    char expr_src[4096]; int elen = 0; int depth = 1;
                    while (i < tlen && depth > 0) {
                        if (tmpl[i] == '{') depth++;
                        else if (tmpl[i] == '}') { depth--; if (depth == 0) break; }
                        if (elen < (int)sizeof(expr_src)-2) expr_src[elen++] = tmpl[i];
                        i++;
                    }
                    expr_src[elen] = '\0'; i++; // skip }
                    if (elen > 0) {
                        parts[nparts].is_lit = 0;
                        parts[nparts].s = strdup(expr_src);
                        nparts++;
                    }
                } else {
                    // Collect literal segment
                    char lit[4096]; int llen = 0;
                    while (i < tlen && !(tmpl[i] == '{' && (i == 0 || tmpl[i-1] != '\\'))) {
                        if (llen < (int)sizeof(lit)-2) lit[llen++] = tmpl[i];
                        i++;
                    }
                    if (llen > 0) {
                        lit[llen] = '\0';
                        parts[nparts].is_lit = 1;
                        parts[nparts].s = strdup(lit);
                        nparts++;
                    }
                }
            }

            if (nparts == 0) { return strdup("sage_rt_string(\"\")"); }

            // Compile each part to a C expression returning SageValue string
            char* compiled[MAX_PARTS];
            for (int pi = 0; pi < nparts; pi++) {
                if (parts[pi].is_lit) {
                    // Escape the literal and wrap in sage_rt_string
                    char* esc = aot_escape(parts[pi].s);
                    compiled[pi] = malloc(strlen(esc) + 32);
                    sprintf(compiled[pi], "sage_rt_string(\"%s\")", esc);
                    free(esc);
                } else {
                    // Parse the sub-expression and compile it
                    char* snip = malloc(strlen(parts[pi].s) + 4);
                    sprintf(snip, "%s\n", parts[pi].s);
                    LexerState  sl = lexer_get_state();
                    ParserState sp = parser_get_state();
                    init_lexer(snip, "<interp>");
                    parser_init();
                    Expr* sub = parse_expression_public();
                    lexer_set_state(sl);
                    parser_set_state(sp);
                    if (sub) {
                        // Mirror the interpreter: if the inner expression is a bare
                        // variable that isn't in scope, it would throw at runtime and
                        // the interpreter preserves the literal "{name}" text. Do the
                        // same here instead of emitting a reference to an undeclared C
                        // variable (which fails to compile).
                        if (sub->type == EXPR_VARIABLE) {
                            char vn[256];
                            int vl = sub->as.variable.name.length < 255 ? sub->as.variable.name.length : 255;
                            memcpy(vn, sub->as.variable.name.start, vl); vn[vl] = '\0';
                            if (!aot_var_in_scope(aot, vn) &&
                                !aot_is_known_proc(aot, vn, vl)) {
                                char* esc = aot_escape(parts[pi].s);
                                compiled[pi] = malloc(strlen(esc) + 40);
                                sprintf(compiled[pi], "sage_rt_string(\"{%s}\")", esc);
                                free(esc);
                                free(snip);
                                free(parts[pi].s);
                                continue;
                            }
                        }
                        // Always get a boxed SageValue for the sub-expression.
                        // aot_expr with UNKNOWN hint returns raw scalars for unboxed vars,
                        // so check inferred type and box explicitly if needed.
                        JitTypeTag st = aot_infer_expr(aot, sub);
                        char* sub_c;
                        if (jit_is_unboxed(st)) {
                            // Get raw scalar, then box it
                            char* raw_c = aot_expr(aot, sub, st);
                            sub_c = aot_box(st, raw_c);
                            free(raw_c);
                        } else {
                            sub_c = aot_expr(aot, sub, JIT_TYPE_UNKNOWN);
                        }
                        compiled[pi] = malloc(strlen(sub_c) + 32);
                        sprintf(compiled[pi], "sage_rt_str_cast(%s)", sub_c);
                        free(sub_c);
                    } else {
                        // Parse failed — emit as literal
                        char* esc = aot_escape(parts[pi].s);
                        compiled[pi] = malloc(strlen(esc) + 40);
                        sprintf(compiled[pi], "sage_rt_string(\"{%s}\")", esc);
                        free(esc);
                    }
                    free(snip);
                }
                free(parts[pi].s);
            }

            // Fold: concat(concat(part0, part1), part2) ...
            char* acc = compiled[0];
            for (int pi = 1; pi < nparts; pi++) {
                char* newacc = malloc(strlen(acc) + strlen(compiled[pi]) + 48);
                sprintf(newacc, "sage_rt_string_concat(%s, %s)", acc, compiled[pi]);
                free(acc); free(compiled[pi]);
                acc = newacc;
            }
            return acc;
        }

        case EXPR_AWAIT: {
            // await always produces a SageValue (box unboxed types)
            JitTypeTag at = aot_infer_expr(aot, expr->as.await.expression);
            char* av = aot_expr(aot, expr->as.await.expression, at);
            if (jit_is_unboxed(at)) {
                char* bv = aot_box(at, av); free(av); return bv;
            }
            return av;
        }
        case EXPR_COMPTIME:
            return aot_expr(aot,expr->as.await.expression,hint);
        case EXPR_SUPER:
            return strdup("_self"); // super → current instance in compiled mode
            // Note: super.method() calls are handled in the method call dispatch
        default:
            return strdup("sage_rt_nil()");
    }
}

char* aot_compile_expr(AotCompiler* aot, Expr* expr) {
    return aot_expr(aot, expr, JIT_TYPE_UNKNOWN);
}

static int is_range_for(Stmt* stmt, Expr** lo, Expr** hi, int* inclusive) {
    Expr* iter=stmt->as.for_stmt.iterable;
    if (!iter) return 0;
    if (iter->type==EXPR_RANGE){*lo=iter->as.range.low;*hi=iter->as.range.high;*inclusive=iter->as.range.inclusive;return 1;}
    if (iter->type==EXPR_CALL && iter->as.call.callee && iter->as.call.callee->type==EXPR_VARIABLE) {
        const char* n=iter->as.call.callee->as.variable.name.start;
        int nl=iter->as.call.callee->as.variable.name.length;
        if (nl==5&&memcmp(n,"range",5)==0) {
            if (iter->as.call.arg_count==2){*lo=iter->as.call.args[0];*hi=iter->as.call.args[1];*inclusive=0;return 1;}
            if (iter->as.call.arg_count==1){*lo=NULL;*hi=iter->as.call.args[0];*inclusive=0;return 1;}
        }
        if (nl==8&&memcmp(n,"range_inc",9)==0&&iter->as.call.arg_count==2){*lo=iter->as.call.args[0];*hi=iter->as.call.args[1];*inclusive=1;return 1;}
    }
    return 0;
}

// Returns a C expression guaranteed to be a scalar int (0 or 1) for use in if/while.
// Never returns a SageValue struct — always wraps in sage_rt_truthy when needed.
static char* compile_cond(AotCompiler* aot, Expr* expr) {
    if (!expr) return strdup("0");
    // Native comparison: both operands are unboxed scalars → result is already C int
    if (expr->type == EXPR_BINARY) {
        int op = expr->as.binary.op.type;
        int is_cmp = (op==TOKEN_EQ||op==TOKEN_NEQ||op==TOKEN_GT||op==TOKEN_LT||
                      op==TOKEN_GTE||op==TOKEN_LTE);
        if (is_cmp) {
            JitTypeTag L = aot_infer_expr(aot, expr->as.binary.left);
            JitTypeTag R = aot_infer_expr(aot, expr->as.binary.right);
            if ((L==JIT_TYPE_INT||L==JIT_TYPE_FLOAT||L==JIT_TYPE_BOOL) &&
                (R==JIT_TYPE_INT||R==JIT_TYPE_FLOAT||R==JIT_TYPE_BOOL)) {
                // Both unboxed — binary path returns a native C int
                return aot_expr(aot, expr, JIT_TYPE_BOOL);
            }
        }
    }
    // Bool literal → native
    if (expr->type == EXPR_BOOL) return strdup(expr->as.boolean.value ? "1" : "0");
    // Known bool variable → native
    if (expr->type == EXPR_VARIABLE) {
        char name[256];
        int len = expr->as.variable.name.length<255?expr->as.variable.name.length:255;
        memcpy(name,expr->as.variable.name.start,len); name[len]='\0';
        JitTypeTag vt = aot_get_var_type(aot, name);
        if (vt==JIT_TYPE_BOOL||vt==JIT_TYPE_INT) return aot_expr(aot, expr, vt);
    }
    // Everything else: evaluate as SageValue, test with sage_rt_truthy
    JitTypeTag raw_t = aot_infer_expr(aot, expr);
    char* raw = aot_expr(aot, expr, JIT_TYPE_UNKNOWN);
    // sage_rt_truthy expects SageValue — box if it's a raw scalar
    char* boxed_raw = (jit_is_unboxed(raw_t) && strncmp(raw,"sage_rt_",8)!=0)
        ? aot_box(raw_t, raw) : raw;
    char* out = malloc(strlen(boxed_raw)+32);
    sprintf(out, "sage_rt_truthy(%s)", boxed_raw);
    if (boxed_raw != raw) { free(boxed_raw); }
    free(raw); return out;
}

void aot_compile_stmt(AotCompiler* aot, Stmt* stmt) {
    if (!stmt) return;
    switch (stmt->type) {
        case STMT_PRINT: {
            Expr* _pe = stmt->as.print.expression;
            // Mirror the interpreter's REPL-style recovery for the specific
            // malformed pattern `print(<module>.<undefined-member> ...)`:
            // accessing a member that the module doesn't define is a runtime
            // error that goes to stderr, abandoning the statement (so nothing
            // reaches stdout) while execution continues. We detect it at compile
            // time by walking to the head module GET of the print argument.
            {
                Expr* head = _pe;
                while (head) {
                    if (head->type == EXPR_GET) { head = head->as.get.object; continue; }
                    if (head->type == EXPR_INDEX) { head = head->as.index.array; continue; }
                    if (head->type == EXPR_CALL) { head = head->as.call.callee; continue; }
                    break;
                }
                // head should now be `module.member` as an EXPR_GET on a VARIABLE
                if (_pe->type == EXPR_GET && _pe->as.get.object &&
                    _pe->as.get.object->type == EXPR_GET) {
                    Expr* mg = _pe->as.get.object;  // e.g. channel.q
                    if (mg->as.get.object && mg->as.get.object->type == EXPR_VARIABLE) {
                        char mod[128]; int ml = mg->as.get.object->as.variable.name.length;
                        if (ml < 128) {
                            memcpy(mod, mg->as.get.object->as.variable.name.start, ml); mod[ml]='\0';
                            int is_mod = 0;
                            for (int i=0;i<aot->imported_module_count;i++)
                                if (!strcmp(aot->imported_modules[i],mod)) { is_mod=1; break; }
                            if (is_mod) {
                                // Is `member` a known function/var of this module?
                                char mem[128]; int el = mg->as.get.property.length;
                                if (el < 128) {
                                    memcpy(mem, mg->as.get.property.start, el); mem[el]='\0';
                                    int known = 0;
                                    char want[256]; snprintf(want,sizeof(want),"sg_%s_sg_%s",mod,mem);
                                    if (aot_is_known_proc(aot, want, (int)strlen(want))) known=1;
                                    for (int i=0;!known && i<aot->mod_proc_count;i++)
                                        if (!strcmp(aot->mod_procs[i].proc_raw,mem)) { known=1; break; }
                                    if (!known) {
                                        aot_emit(aot,"fprintf(stderr,\"Runtime Error: Module attribute is not defined.\\n\");");
                                        break;  // skip the stdout print entirely
                                    }
                                }
                            }
                        }
                    }
                }
            }
            char* val = aot_expr_boxed(aot, _pe);
            // Use sage_rt_print_kw which dispatches __str__ like the interpreter's STMT_PRINT
            aot_emit(aot, "sage_rt_print_kw(%s);", val);
            free(val); break;
        }
        case STMT_LET: {
            char* name_raw = aot_cname_tok(stmt->as.let.name);
            // Determine the var's actual name (without sg_ prefix) for type_env
            char let_raw[256]; int lrl = stmt->as.let.name.length < 255 ? stmt->as.let.name.length : 255;
            memcpy(let_raw, stmt->as.let.name.start, lrl); let_raw[lrl] = '\0';
            // Check if this var was pre-declared as a file-scope global
            // (happens for top-level vars that need to be visible in class methods)
            // Only applies in top-level code (not inside proc/method bodies which
            // should declare their own locals even if the name matches a global)
            int _is_global = 0;
            if (!aot->in_proc_body) {
                for (int _gi = 0; _gi < aot->global_var_count; _gi++)
                    if (strcmp(aot->global_vars[_gi], name_raw) == 0) { _is_global = 1; break; }
            }
            if (_is_global) {
                // Already declared as static SageValue — just assign (always boxed)
                if (stmt->as.let.initializer) {
                    char* val = aot_expr_boxed(aot, stmt->as.let.initializer);
                    aot_emit(aot, "%s = %s;", name_raw, val); free(val);
                }
                // Register as UNKNOWN so references don't try to unbox it
                aot_set_var_type(aot, let_raw, JIT_TYPE_UNKNOWN);
                free(name_raw); break;
            }
            if (stmt->as.let.initializer) {
                JitTypeTag t = aot->in_coro_body ? JIT_TYPE_UNKNOWN
                                                 : aot_infer_expr(aot, stmt->as.let.initializer);
                // Register type in type_env so future references know the C type.
                // Inside proc bodies: force-set to shadow outer-scope UNKNOWN entries
                // (e.g. global pre-scan sets outer x→UNKNOWN, inner proc's x→STRING wins)
                if (aot->in_proc_body) aot_force_var_type(aot, let_raw, t);
                else aot_set_var_type(aot, let_raw, t);
                // If inside a closure body that has #define capture aliases,
                // undef this name before declaring local — prevents macro expansion conflict.
                // Safe even when no such #define exists (no-op undef).
                // Only do this inside closure wrappers (in_closure_body > 0) to avoid
                // polluting global/proc scope with gratuitous undefs.
                if (aot->in_closure_body)
                    aot_emit(aot,"#undef %s", name_raw);
                if (jit_is_unboxed(t)) {
                    char* val=aot_expr(aot,stmt->as.let.initializer,t);
                    // aot_expr may return a boxed SageValue even with an unboxed hint
                    // (e.g. `(b1 & 128) != 0` where operands are SageValue → returns
                    // sage_rt_bool(...)). If the declared C type is a raw scalar but the
                    // RHS is a boxed sage_rt_* expression that doesn't already end in a
                    // field extraction, extract the matching field.
                    size_t vl = strlen(val);
                    int ends_in_field =
                        (vl>=11 && strcmp(val+vl-11,".as.integer")==0) ||
                        (vl>=10 && strcmp(val+vl-10,".as.number")==0)  ||
                        (vl>=11 && strcmp(val+vl-11,".as.boolean")==0) ||
                        (vl>=10 && strcmp(val+vl-10,".as.string")==0);
                    int rhs_is_boxed = (strncmp(val,"sage_rt_",8)==0) && !ends_in_field;
                    if (rhs_is_boxed) {
                        const char* fld = (t==JIT_TYPE_INT)?"i":(t==JIT_TYPE_FLOAT)?"d":
                                          (t==JIT_TYPE_BOOL)?".as.boolean":
                                          (t==JIT_TYPE_STRING)?".as.string":"";
                        if (t==JIT_TYPE_INT)
                            aot_emit(aot,"%s %s = SAGE_AS_INT64(%s);",jit_ctype(t),name_raw,val);
                        else if (t==JIT_TYPE_FLOAT)
                            aot_emit(aot,"%s %s = SAGE_AS_DOUBLE(%s);",jit_ctype(t),name_raw,val);
                        else
                            aot_emit(aot,"%s %s = (%s)%s;",jit_ctype(t),name_raw,val,fld);
                    } else {
                        aot_emit(aot,"%s %s = %s;",jit_ctype(t),name_raw,val);
                    }
                    free(val);
                } else if (t == JIT_TYPE_STRUCT &&
                           stmt->as.let.initializer->type == EXPR_VARIABLE) {
                    char* val=aot_expr(aot,stmt->as.let.initializer,JIT_TYPE_UNKNOWN);
                    aot_emit(aot,"SageValue %s = sage_rt_struct_copy(%s);",name_raw,val); free(val);
                } else {
                    char* val=aot_expr_boxed(aot,stmt->as.let.initializer);
                    aot_emit(aot,"SageValue %s = %s;",name_raw,val); free(val);
                }
            } else aot_emit(aot,"SageValue %s = sage_rt_nil();",name_raw);
            free(name_raw); break;
        }
        case STMT_EXPRESSION: {
            Expr* e = stmt->as.expression;
            // Optimise: var = expr where var is a known unboxed type
            // (suppressed inside coroutine bodies — all vars are SageValue there)
            if (!aot->in_coro_body && e && e->type == EXPR_SET && e->as.set.object == NULL) {
                char name[256];
                int len = e->as.set.property.length < 255 ? e->as.set.property.length : 255;
                memcpy(name, e->as.set.property.start, len); name[len] = '\0';
                JitTypeTag vt = aot_get_var_type(aot, name);
                if (jit_is_unboxed(vt)) {
                    char* lhs = aot_cname(e->as.set.property.start, e->as.set.property.length);
                    JitTypeTag rhs_t = aot_infer_expr(aot, e->as.set.value);
                    char* rhs = aot_expr(aot, e->as.set.value, vt);
                    // If RHS is a method call or complex expr returning SageValue,
                    // extract the raw C value from it
                    // rhs_is_raw: aot_expr returned a raw C scalar (not SageValue)
                    // This is true when:
                    //   - unboxed variable (sg_x where x is int64_t/double/etc.)
                    //   - literal with matching typed hint (returns raw C value)
                    //   - binary op between same unboxed types (returns raw C value)
                    int rhs_is_raw = (
                        (e->as.set.value->type == EXPR_VARIABLE && jit_is_unboxed(rhs_t)) ||
                        (e->as.set.value->type == EXPR_INT   && vt == JIT_TYPE_INT)   ||
                        (e->as.set.value->type == EXPR_NUMBER&& vt == JIT_TYPE_FLOAT) ||
                        (e->as.set.value->type == EXPR_BOOL  && vt == JIT_TYPE_BOOL)  ||
                        // Binary numeric ops return raw ONLY when the C expression is truly raw
                        // (not when they've been boxed as sage_rt_int(...) because hint=UNKNOWN)
                        (e->as.set.value->type == EXPR_BINARY && jit_is_unboxed(rhs_t) && rhs_t == vt &&
                         rhs_t != JIT_TYPE_STRING && strncmp(rhs,"sage_rt_",8)!=0)
                    );
                    if (rhs_is_raw) {
                        aot_emit(aot, "%s = %s;", lhs, rhs);
                    } else {
                        // RHS returns SageValue — extract the right field
                        // rhs_looks_raw: true if rhs is a raw C scalar (not SageValue)
                        // Function calls and sage_rt_* expressions always return SageValue
                        // even when the inferred type is unboxed (e.g. STRING method calls)
                        int _rhs_is_call = (e->as.set.value->type == EXPR_CALL ||
                                            strncmp(rhs,"sage_rt_",8)==0 ||
                                            strncmp(rhs,"({",2)==0 ||
                                            rhs[0]=='(' || // (sage_rt_add(...)) pattern
                                            (e->as.set.value->type == EXPR_BINARY && rhs_t == JIT_TYPE_STRING));
                        int rhs_looks_raw = (jit_is_unboxed(rhs_t) && !_rhs_is_call);
                        if (rhs_looks_raw) {
                            // Already a raw scalar — direct assign
                            aot_emit(aot, "%s = %s;", lhs, rhs);
                        } else {
                            char* extract = malloc(strlen(rhs) + 64);
                            switch(vt) {
                                case JIT_TYPE_INT:
                                    // SAGE_AS_INT64 converts float→int correctly; raw
                                    // .as.integer would type-pun a float's bits.
                                    sprintf(extract, "SAGE_AS_INT64(%s)", rhs); break;
                                case JIT_TYPE_FLOAT:
                                    sprintf(extract, "SAGE_AS_DOUBLE(%s)", rhs); break;
                                case JIT_TYPE_BOOL:
                                    sprintf(extract, "(%s).as.boolean", rhs); break;
                                case JIT_TYPE_STRING:
                                    sprintf(extract, "(%s).as.string", rhs); break;
                                default:
                                    sprintf(extract, "%s", rhs); break;
                            }
                            aot_emit(aot, "%s = %s;", lhs, extract);
                            free(extract);
                        }
                    }
                    free(lhs); free(rhs); break;
                }
            }
            // Also handle += -= *= /= on unboxed vars
            char* val=aot_expr(aot,stmt->as.expression,JIT_TYPE_UNKNOWN);
            aot_emit(aot,"(void)(%s);",val); free(val); break;
        }
        case STMT_IF: {
            char* cond = compile_cond(aot, stmt->as.if_stmt.condition);
            aot_emit(aot,"if (%s) {",cond); free(cond);
            aot->indent++;
            for(Stmt*s=stmt->as.if_stmt.then_branch;s;s=s->next) aot_compile_stmt(aot,s);
            aot->indent--;
            if (stmt->as.if_stmt.else_branch) {
                aot_emit(aot,"} else {"); aot->indent++;
                for(Stmt*s=stmt->as.if_stmt.else_branch;s;s=s->next) aot_compile_stmt(aot,s);
                aot->indent--;
            }
            aot_emit(aot,"}"); break;
        }
        case STMT_WHILE: {
            char* cond = compile_cond(aot, stmt->as.while_stmt.condition);
            aot_emit(aot,"while (%s) {",cond); free(cond);
            aot->indent++;
            for(Stmt*s=stmt->as.while_stmt.body;s;s=s->next) aot_compile_stmt(aot,s);
            aot->indent--;
            aot_emit(aot,"}"); break;
        }
        case STMT_FOR: {
            char* var=aot_cname_tok(stmt->as.for_stmt.variable);
            Expr*lo=NULL,*hi=NULL; int inc=0;
            if (is_range_for(stmt,&lo,&hi,&inc)) {
                // Force register loop var as INT — override any prior UNKNOWN inference
                { char name[256]; int len=stmt->as.for_stmt.variable.length<255?stmt->as.for_stmt.variable.length:255;
                  memcpy(name,stmt->as.for_stmt.variable.start,len);name[len]='\0';
                  aot_force_var_type(aot,name,JIT_TYPE_INT); }
                char* lo_c_raw=lo?aot_expr(aot,lo,JIT_TYPE_INT):strdup("INT64_C(0)");
                JitTypeTag lo_t = lo ? aot_infer_expr(aot, lo) : JIT_TYPE_INT;
                char* lo_c;
                if (lo && (lo_t==JIT_TYPE_UNKNOWN || strncmp(lo_c_raw,"sage_rt_",8)==0)) {
                    lo_c = malloc(strlen(lo_c_raw)+32);
                    sprintf(lo_c,"(int64_t)SAGE_AS_INT64(%s)",lo_c_raw);
                    free(lo_c_raw);
                } else { lo_c = lo_c_raw; }
                char* hi_c_raw=aot_expr(aot,hi,JIT_TYPE_INT);
                // If hi_c is a SageValue expression (starts with sage_rt_) or 
                // a SageValue variable (UNKNOWN inferred type), unbox it
                JitTypeTag hi_t = aot_infer_expr(aot, hi);
                char* hi_c;
                if (hi_t == JIT_TYPE_UNKNOWN || hi_t == JIT_TYPE_INSTANCE ||
                    strncmp(hi_c_raw,"sage_rt_",8)==0 || strncmp(hi_c_raw,"({",2)==0) {
                    hi_c = malloc(strlen(hi_c_raw)+32);
                    sprintf(hi_c,"(int64_t)SAGE_AS_INT64(%s)",hi_c_raw);
                    free(hi_c_raw);
                } else {
                    hi_c = hi_c_raw;
                }
                const char* cmp=inc?"<=":"<";
                aot_emit(aot,"for (int64_t %s = %s; %s %s %s; %s++) {",var,lo_c,var,cmp,hi_c,var);
                free(lo_c); free(hi_c);
                aot->indent++;
                for(Stmt*s=stmt->as.for_stmt.body;s;s=s->next) aot_compile_stmt(aot,s);
                aot->indent--;
                aot_emit(aot,"}");
            } else {
                char* iter=aot_temp(aot); char* idx=aot_temp(aot);
                char* itv=aot_expr(aot,stmt->as.for_stmt.iterable,JIT_TYPE_UNKNOWN);
                aot_emit(aot,"{"); aot->indent++;
                aot_emit(aot,"SageValue %s = %s;",iter,itv);
                aot_emit(aot,"for (int64_t %s = 0; %s < sage_rt_array_len(%s); %s++) {",idx,idx,iter,idx);
                aot->indent++;
                aot_emit(aot,"SageValue %s = sage_rt_array_get(%s, sage_rt_int(%s));",var,iter,idx);
                // The loop variable is a fresh SageValue binding for the body. Force
                // its type to UNKNOWN so any stale outer inference (e.g. an outer
                // `var item = "..."`) doesn't mis-specialize uses inside the loop.
                // Save and restore around the body so the outer type is unaffected.
                char loopname[256];
                { int len=stmt->as.for_stmt.variable.length<255?stmt->as.for_stmt.variable.length:255;
                  memcpy(loopname,stmt->as.for_stmt.variable.start,len);loopname[len]='\0'; }
                JitTypeTag _saved_lt = aot_get_var_type(aot, loopname);
                int _was_in_scope = aot_var_in_scope(aot, loopname);
                aot_force_var_type(aot, loopname, JIT_TYPE_UNKNOWN);
                for(Stmt*s=stmt->as.for_stmt.body;s;s=s->next) aot_compile_stmt(aot,s);
                if (_was_in_scope) aot_force_var_type(aot, loopname, _saved_lt);
                aot->indent--;
                aot_emit(aot,"}"); aot->indent--;
                aot_emit(aot,"}");
                free(itv); free(iter); free(idx);
            }
            free(var); break;
        }
        case STMT_RETURN: {
            // Emit defers (LIFO) before returning
            for(int _di=aot->defer_count-1;_di>=0;_di--){
                aot_emit(aot,"{ /* defer */"); aot->indent++;
                for(Stmt*_ds=aot->defer_stack[_di]->as.defer.statement;_ds;_ds=_ds->next)
                    aot_compile_stmt(aot,_ds);
                aot->indent--; aot_emit(aot,"}");
            }
            if (stmt->as.ret.value) {
                // Always box the return value — all Sage procs return SageValue
                // But: in closure bodies, captured variables are SageValue via _caps->fields[N]
                // so we must NOT use the outer-inferred type for boxing
                char* val;
                if (aot->in_closure_body && stmt->as.ret.value->type == EXPR_VARIABLE) {
                    // Return captured var directly — it's already SageValue in the closure
                    val = aot_expr(aot, stmt->as.ret.value, JIT_TYPE_UNKNOWN);
                } else {
                    val = aot_expr_boxed(aot, stmt->as.ret.value);
                }
                aot_emit(aot, "return %s;", val);
                free(val);
            } else aot_emit(aot,"return sage_rt_nil();");
            break;
        }
        case STMT_BREAK:    aot_emit(aot,"break;");    break;
        case STMT_CONTINUE: aot_emit(aot,"continue;"); break;
        case STMT_BLOCK:
            // Compile block statements inline without C braces — keeps vars in scope.
            // Sage blocks from destructuring need outer-scope visibility.
            for(Stmt*s=stmt->as.block.statements;s;s=s->next) aot_compile_stmt(aot,s);
            break;
        case STMT_ANNOTATED_BLOCK: {
            BlockAnnotation ann=stmt->as.annotated_block.annotation;
            if (ann==BLOCK_ANNOT_MANUAL||ann==BLOCK_ANNOT_TRUSTED)
                aot_emit(aot,"sage_rt_gc_pause(); /* @%s */",ann==BLOCK_ANNOT_MANUAL?"manual":"trusted");
            aot_emit(aot,"{"); aot->indent++;
            for(Stmt*s=stmt->as.annotated_block.statements;s;s=s->next) aot_compile_stmt(aot,s);
            aot->indent--; aot_emit(aot,"}");
            if (ann==BLOCK_ANNOT_MANUAL||ann==BLOCK_ANNOT_TRUSTED) aot_emit(aot,"sage_rt_gc_resume();");
            break;
        }
        case STMT_MATCH: {
            JitTypeTag vt=aot_infer_expr(aot,stmt->as.match_stmt.value);
            /* Evaluate match value */
            // For INT match, use INT hint so literals come back as raw int64_t
            char* _mval_raw=aot_expr(aot,stmt->as.match_stmt.value,
                vt==JIT_TYPE_INT?JIT_TYPE_INT:JIT_TYPE_UNKNOWN);
            char* val;
            if (stmt->as.match_stmt.value->type==EXPR_VARIABLE&&jit_is_unboxed(vt)&&vt!=JIT_TYPE_INT)
                val=aot_box(vt,_mval_raw);
            else
                val=_mval_raw;
            char* tmp=aot_temp(aot);
            aot_emit(aot,"{ /* match */"); aot->indent++;
            if (vt==JIT_TYPE_INT) {
                aot_emit(aot,"int64_t %s = %s;",tmp,_mval_raw);
                // Check if any case is a range or wildcard — if so, use if-else not switch
                int has_range=0;
                for(int i=0;i<stmt->as.match_stmt.case_count&&!has_range;i++){
                    CaseClause*mc=stmt->as.match_stmt.cases[i];
                    if(!mc->pattern) has_range=1;
                    else if(mc->pattern->type==EXPR_RANGE) has_range=1;
                    else if(mc->pattern->type==EXPR_VARIABLE) has_range=1; // wildcard
                    else if(mc->guard) has_range=1; // guard — can't use switch
                }
                if(!has_range){
                    // Pure integer switch
                    aot_emit(aot,"switch (%s) {",tmp); aot->indent++;
                    for(int i=0;i<stmt->as.match_stmt.case_count;i++){
                        CaseClause*mc=stmt->as.match_stmt.cases[i];
                        char*pat=aot_expr(aot,mc->pattern,JIT_TYPE_INT);
                        aot_emit(aot,"case %s: {",pat); free(pat);
                        aot->indent++;
                        for(Stmt*s=mc->body;s;s=s->next) aot_compile_stmt(aot,s);
                        aot_emit(aot,"break;"); aot->indent--; aot_emit(aot,"}");
                    }
                    if(stmt->as.match_stmt.default_case){
                        aot_emit(aot,"default: {"); aot->indent++;
                        for(Stmt*s=stmt->as.match_stmt.default_case;s;s=s->next) aot_compile_stmt(aot,s);
                        aot_emit(aot,"break;"); aot->indent--; aot_emit(aot,"}");
                    }
                    aot->indent--; aot_emit(aot,"}"); // close switch
                } else {
                    // If-else chain (handles ranges, wildcards, mixed, guards)
                    for(int i=0;i<stmt->as.match_stmt.case_count;i++){
                        CaseClause*mc=stmt->as.match_stmt.cases[i];
                        const char*kw=(i==0)?"if":"} else if";
                        // Check for guard expression (case n if cond =>)
                        if(mc->guard){
                            char*gcond=compile_cond(aot,mc->guard);
                            if(mc->pattern && mc->pattern->type!=EXPR_VARIABLE){
                                // Pattern + guard: if (val == pattern && guard_cond)
                                char*pat=aot_expr(aot,mc->pattern,JIT_TYPE_INT);
                                aot_emit(aot,"%s (%s==%s && %s) {",kw,tmp,pat,gcond);
                                free(pat);
                            } else {
                                aot_emit(aot,"%s (%s) {",kw,gcond);
                                if(mc->pattern && mc->pattern->type==EXPR_VARIABLE){
                                    char*pn=aot_cname_tok(mc->pattern->as.variable.name);
                                    aot_emit(aot,"int64_t %s=%s;",pn,tmp); free(pn);
                                }
                            }
                            free(gcond); aot->indent++;
                        } else if(!mc->pattern || (mc->pattern->type==EXPR_VARIABLE &&
                                mc->pattern->as.variable.name.length==1 &&
                                mc->pattern->as.variable.name.start[0]=='_')){
                            aot_emit(aot,"%s",i==0?"{":"} else {"); aot->indent++;
                        } else if(mc->pattern->type==EXPR_RANGE){
                            char*ls=aot_expr(aot,mc->pattern->as.range.low,JIT_TYPE_INT);
                            char*hs=aot_expr(aot,mc->pattern->as.range.high,JIT_TYPE_INT);
                            aot_emit(aot,"%s (%s>=%s&&%s<=%s) {",kw,tmp,ls,tmp,hs);
                            free(ls);free(hs); aot->indent++;
                        } else if(mc->pattern->type==EXPR_VARIABLE){
                            // Bare variable pattern: bind and match all
                            char*pn=aot_cname_tok(mc->pattern->as.variable.name);
                            aot_emit(aot,"%s (1) { int64_t %s=%s;",kw,pn,tmp);
                            free(pn); aot->indent++;
                        } else {
                            char*pat=aot_expr(aot,mc->pattern,JIT_TYPE_INT);
                            aot_emit(aot,"%s (%s==%s) {",kw,tmp,pat);
                            free(pat); aot->indent++;
                        }
                        for(Stmt*s=mc->body;s;s=s->next) aot_compile_stmt(aot,s);
                        aot->indent--;
                    }
                    if(stmt->as.match_stmt.default_case){
                        aot_emit(aot,"%s",stmt->as.match_stmt.case_count>0?"} else {":"{"); aot->indent++;
                        for(Stmt*s=stmt->as.match_stmt.default_case;s;s=s->next) aot_compile_stmt(aot,s);
                        aot->indent--;
                    }
                    if(stmt->as.match_stmt.case_count>0||stmt->as.match_stmt.default_case)
                        aot_emit(aot,"}");
                }
            } else {
                aot_emit(aot,"SageValue %s = %s;",tmp,val);
                for(int i=0;i<stmt->as.match_stmt.case_count;i++){
                    CaseClause*c=stmt->as.match_stmt.cases[i];
                    if (!c->pattern) {
                        // Default/wildcard: treat as else
                        aot_emit(aot,"%s",i==0?"{":"} else {"); aot->indent++;
                        for(Stmt*s=c->body;s;s=s->next) aot_compile_stmt(aot,s);
                        aot->indent--;
                        continue;
                    }
                    if (!c->pattern) {
                        // skip: handled below
                        const char*kw2=i==0?"{":"} else {";
                        aot_emit(aot,"%s",kw2); aot->indent++;
                        for(Stmt*s=c->body;s;s=s->next) aot_compile_stmt(aot,s);
                        aot->indent--; continue;
                    }
                    if(c->guard){
                        char*gcond2=aot_expr(aot,c->guard,JIT_TYPE_UNKNOWN);
                        const char*kw2=i==0?"if":"} else if";
                        aot_emit(aot,"%s (sage_rt_truthy(%s)) {",kw2,gcond2);
                        free(gcond2); aot->indent++;
                        for(Stmt*s=c->body;s;s=s->next) aot_compile_stmt(aot,s);
                        aot->indent--; continue;
                    }
                    // ── ADT pattern: Some(v), Ok(v), Err(e) ─────────────────────────────
                    if (c->pattern && c->pattern->type == EXPR_CALL &&
                        c->pattern->as.call.callee &&
                        c->pattern->as.call.callee->type == EXPR_VARIABLE &&
                        c->pattern->as.call.arg_count >= 1) {
                        const char* _fn = c->pattern->as.call.callee->as.variable.name.start;
                        int _fl = c->pattern->as.call.callee->as.variable.name.length;
                        #define _OPTM(s) (_fl==(int)strlen(s)&&memcmp(_fn,s,_fl)==0)
                        if (_OPTM("Some")||_OPTM("Ok")||_OPTM("Err")) {
                            const char* _type_name = _OPTM("Some")?"Some":_OPTM("Ok")?"Ok":"Err";
                            const char* kw = i==0?"if":"} else if";
                            aot_emit(aot,"%s (SAGE_IS_DICT(%s) && sage_rt_equal(sage_rt_dict_get(%s,sage_rt_string(\"__type\")),sage_rt_string(\"%s\"))) {",
                                     kw, tmp, tmp, _type_name);
                            aot->indent++;
                            // Bind the inner value to each binding variable
                            for (int _bi=0; _bi<c->pattern->as.call.arg_count; _bi++) {
                                Expr* _barg = c->pattern->as.call.args[_bi];
                                if (_barg && _barg->type == EXPR_VARIABLE) {
                                    char* _bname = aot_cname_tok(_barg->as.variable.name);
                                    aot_emit(aot,"SageValue %s = sage_rt_dict_get(%s, sage_rt_string(\"value\"));",
                                             _bname, tmp);
                                    free(_bname);
                                }
                            }
                            for(Stmt*s=c->body;s;s=s->next) aot_compile_stmt(aot,s);
                            aot->indent--; continue;
                        }
                        #undef _OPTM
                    }
                    // ── ADT pattern: Enum.Variant(field1, field2, ...) ──────────────────
                    if (c->pattern && c->pattern->type == EXPR_CALL &&
                        c->pattern->as.call.callee &&
                        c->pattern->as.call.callee->type == EXPR_GET) {
                        Expr* _ge2 = c->pattern->as.call.callee;
                        if (_ge2->as.get.object && _ge2->as.get.object->type == EXPR_VARIABLE) {
                            char _en2[64]={0};
                            int _enl2 = _ge2->as.get.object->as.variable.name.length<63
                                      ? _ge2->as.get.object->as.variable.name.length:63;
                            memcpy(_en2, _ge2->as.get.object->as.variable.name.start, _enl2);
                            _en2[_enl2]='\0';
                            char _vn2[64]={0};
                            int _vnl2 = _ge2->as.get.property.length<63
                                      ? _ge2->as.get.property.length:63;
                            memcpy(_vn2, _ge2->as.get.property.start, _vnl2);
                            _vn2[_vnl2]='\0';
                            // Look up field names from registry
                            int _slot = -1;
                            for (int _ri=0; _ri<aot->adt_variant_count; _ri++) {
                                if (strcmp(aot->adt_variants[_ri].enum_raw,_en2)==0 &&
                                    strcmp(aot->adt_variants[_ri].variant_raw,_vn2)==0) {
                                    _slot=_ri; break;
                                }
                            }
                            const char* kw = i==0?"if":"} else if";
                            aot_emit(aot,"%s (SAGE_IS_DICT(%s) && sage_rt_equal(sage_rt_dict_get(%s,sage_rt_string(\"__tag\")),sage_rt_string(\"%s\"))) {",
                                     kw, tmp, tmp, _vn2);
                            aot->indent++;
                            // Bind each pattern arg to the corresponding field
                            int _nbinds = c->pattern->as.call.arg_count;
                            for (int _bi=0; _bi<_nbinds; _bi++) {
                                Expr* _barg = c->pattern->as.call.args[_bi];
                                if (!_barg || _barg->type != EXPR_VARIABLE) continue;
                                char* _bname = aot_cname_tok(_barg->as.variable.name);
                                const char* _fname = (_slot>=0 && _bi<aot->adt_variants[_slot].field_count)
                                    ? aot->adt_variants[_slot].field_names[_bi] : NULL;
                                if (_fname && _fname[0]) {
                                    aot_emit(aot,"SageValue %s = sage_rt_dict_get(%s, sage_rt_string(\"%s\"));",
                                             _bname, tmp, _fname);
                                } else {
                                    // Fallback: positional lookup not possible without registry
                                    aot_emit(aot,"SageValue %s = sage_rt_nil(); /* field %d not in registry */",
                                             _bname, _bi);
                                }
                                free(_bname);
                            }
                            for(Stmt*s=c->body;s;s=s->next) aot_compile_stmt(aot,s);
                            aot->indent--; continue;
                        }
                    }
                    // ── ADT unit variant: Enum.Variant (no args, EXPR_GET pattern) ──────
                    if (c->pattern && c->pattern->type == EXPR_GET &&
                        c->pattern->as.get.object &&
                        c->pattern->as.get.object->type == EXPR_VARIABLE) {
                        char _en3[64]={0};
                        int _enl3=c->pattern->as.get.object->as.variable.name.length<63
                                 ?c->pattern->as.get.object->as.variable.name.length:63;
                        memcpy(_en3,c->pattern->as.get.object->as.variable.name.start,_enl3);
                        _en3[_enl3]='\0';
                        int _is_enum3=0;
                        for(int _ei=0;_ei<aot->known_enum_count;_ei++){
                            if(strcmp(aot->known_enums[_ei],_en3)==0){_is_enum3=1;break;}
                        }
                        if (_is_enum3) {
                            char _vn3[64]={0};
                            int _vnl3=c->pattern->as.get.property.length<63
                                     ?c->pattern->as.get.property.length:63;
                            memcpy(_vn3,c->pattern->as.get.property.start,_vnl3);
                            _vn3[_vnl3]='\0';
                            const char* kw = i==0?"if":"} else if";
                            aot_emit(aot,"%s (SAGE_IS_DICT(%s) && sage_rt_equal(sage_rt_dict_get(%s,sage_rt_string(\"__tag\")),sage_rt_string(\"%s\"))) {",
                                     kw, tmp, tmp, _vn3);
                            aot->indent++;
                            for(Stmt*s=c->body;s;s=s->next) aot_compile_stmt(aot,s);
                            aot->indent--; continue;
                        }
                    }
                    char*pat=aot_expr(aot,c->pattern,JIT_TYPE_UNKNOWN);
                    const char*kw=i==0?"if":"} else if";
                    // Wildcard variable "_" → always-true condition
                    if (c->pattern->type==EXPR_VARIABLE &&
                        c->pattern->as.variable.name.length==1 &&
                        c->pattern->as.variable.name.start[0]=='_') {
                        aot_emit(aot,"%s (1) {",kw); free(pat); aot->indent++;
                        for(Stmt*s=c->body;s;s=s->next) aot_compile_stmt(aot,s);
                        aot->indent--;
                        continue;
                    }
                    if (c->pattern&&c->pattern->type==EXPR_STRING){
                        char*esc=aot_escape(c->pattern->as.string.value);
                        aot_emit(aot,"%s (SAGE_IS_STRING(%s)&&strcmp(%s.as.string,\"%s\")==0) {",kw,tmp,tmp,esc);
                        free(esc);
                    } else aot_emit(aot,"%s (sage_rt_equal(%s,%s)) {",kw,tmp,pat);
                    free(pat); aot->indent++;
                    for(Stmt*s=c->body;s;s=s->next) aot_compile_stmt(aot,s);
                    aot->indent--;
                }
                if (stmt->as.match_stmt.default_case){
                    aot_emit(aot,"%s",stmt->as.match_stmt.case_count>0?"} else {":"{");
                    aot->indent++;
                    for(Stmt*s=stmt->as.match_stmt.default_case;s;s=s->next) aot_compile_stmt(aot,s);
                    aot->indent--;
                }
                if (stmt->as.match_stmt.case_count>0||stmt->as.match_stmt.default_case) aot_emit(aot,"}");
            }
            aot->indent--; aot_emit(aot,"} /* end match */");
            if(val!=_mval_raw) free(_mval_raw); free(val); free(tmp); break;
        }
        case STMT_TRY: {
            char* frame=aot_temp(aot);
            aot_emit(aot,"{ SageExcFrame %s;",frame); aot->indent++;
            aot_emit(aot,"%s.prev=sage_rt_exc_top; %s.active=1; sage_rt_exc_top=&%s;",frame,frame,frame);
            aot_emit(aot,"%s.saved_depth=sage_rt_call_depth;",frame);
            aot_emit(aot,"if (setjmp(%s.jb)==0) {",frame); aot->indent++;
            for(Stmt*s=stmt->as.try_stmt.try_block;s;s=s->next) aot_compile_stmt(aot,s);
            aot->indent--; aot_emit(aot,"}");
            // Catch block runs when exception was raised (active==0 after raise)
            if (stmt->as.try_stmt.catch_count>0&&stmt->as.try_stmt.catches){
                CatchClause*cc=stmt->as.try_stmt.catches[0];
                char*excvar=aot_cname_tok(cc->exception_var);
                aot_emit(aot,"if (!%s.active) {",frame); aot->indent++;
                aot_emit(aot,"sage_rt_exc_top=%s.prev; sage_rt_call_depth=%s.saved_depth;",frame,frame);
                // Store exc in a block-scoped var to survive optimization
                aot_emit(aot,"{ SageValue %s=%s.exc;",excvar,frame);
                for(Stmt*s=cc->body;s;s=s->next) aot_compile_stmt(aot,s);
                aot_emit(aot,"}");
                aot->indent--; aot_emit(aot,"} else { sage_rt_exc_top=%s.prev; }",frame);
                free(excvar);
            } else {
                aot_emit(aot,"sage_rt_exc_top=%s.prev; sage_rt_call_depth=%s.saved_depth;",frame,frame);
            }
            // Finally block always runs after catch
            if (stmt->as.try_stmt.finally_block){
                aot_emit(aot,"{ /* finally */"); aot->indent++;
                for(Stmt*s=stmt->as.try_stmt.finally_block;s;s=s->next) aot_compile_stmt(aot,s);
                aot->indent--; aot_emit(aot,"}");
            }
            aot->indent--; aot_emit(aot,"}"); free(frame); break;
        }
        case STMT_RAISE: {
            char* val=aot_expr(aot,stmt->as.raise.exception,JIT_TYPE_UNKNOWN);
            aot_emit(aot,"sage_rt_raise(%s);",val); free(val); break;
        }
        case STMT_DEFER:
            // Push onto defer stack (LIFO — emitted before each return and at proc end)
            if (aot->defer_count < 64)
                aot->defer_stack[aot->defer_count++] = stmt;
            break;
        case STMT_YIELD:
            if (stmt->as.yield_stmt.value){
                JitTypeTag yt=aot_infer_expr(aot,stmt->as.yield_stmt.value);
                char*v=aot_expr(aot,stmt->as.yield_stmt.value,JIT_TYPE_UNKNOWN);
                int nb=(stmt->as.yield_stmt.value->type==EXPR_VARIABLE&&jit_is_unboxed(yt));
                char*bv=nb?aot_box(yt,v):v;
                if (aot->in_coro_body)
                    aot_emit(aot,"sage_rt_coro_yield(%s, %s);",aot->coro_var,bv);
                else
                    aot_emit(aot,"return %s; /* yield */",bv);
                if(bv!=v)free(bv); free(v);
            } else {
                if (aot->in_coro_body)
                    aot_emit(aot,"sage_rt_coro_yield(%s, sage_rt_nil());",aot->coro_var);
                else
                    aot_emit(aot,"return sage_rt_nil(); /* yield */");
            }
            break;
        case STMT_STRUCT: {
            StructStmt*ss=&stmt->as.struct_stmt;
            char* sname=aot_cname_tok(ss->name);
            char rawname[256]; int nl=ss->name.length<255?ss->name.length:255;
            memcpy(rawname,ss->name.start,nl); rawname[nl]='\0';
            // Global classval so impl blocks can find it
            aot_emit(aot,"static SageValue _%s_classval;",sname);
            // Constructor
            aot_emit(aot,"static SageValue %s(",sname);
            aot->indent++;
            for(int i=0;i<ss->field_count;i++){char*fn=aot_cname_tok(ss->field_names[i]);aot_emit(aot,"SageValue %s%s",fn,i<ss->field_count-1?",":"");free(fn);}
            if(ss->field_count==0) aot_emit(aot,"void");
            aot->indent--;
            aot_emit(aot,") {"); aot->indent++;
            aot_emit(aot,"SageValue _inst = sage_rt_instance_new(_%s_classval);",sname);
            for(int i=0;i<ss->field_count;i++){char*fn=aot_cname_tok(ss->field_names[i]);char esc[64];int el=ss->field_names[i].length<63?ss->field_names[i].length:63;memcpy(esc,ss->field_names[i].start,el);esc[el]='\0';aot_emit(aot,"sage_rt_field_set(_inst,\"%s\",%s);",esc,fn);free(fn);}
            aot_emit(aot,"return _inst;");
            aot->indent--; aot_emit(aot,"}"); aot_blank(aot); free(sname); break;
        }
        case STMT_ENUM: {
            EnumStmt*es=&stmt->as.enum_stmt;
            char*ename=aot_cname_tok(es->name);
            aot_emit(aot,"/* enum %.*s */",es->name.length,es->name.start);
            for(int i=0;i<es->variant_count;i++){
                char*vname=aot_cname_tok(es->variant_names[i]);
                int has_fields=(es->variant_field_counts&&es->variant_field_counts[i]>0);
                if (!has_fields){
                    aot_emit(aot,"static inline SageValue %s_%s(void){",ename,vname);
                    aot->indent++;
                    aot_emit(aot,"static SageValue _v={.type=SAGE_VAL_NIL};static int _init=0;");
                    aot_emit(aot,"if(!_init){_v=sage_rt_dict_new();sage_rt_dict_set(_v,sage_rt_string(\"__tag\"),sage_rt_string(\"%.*s\"));_init=1;}",es->variant_names[i].length,es->variant_names[i].start);
                    aot_emit(aot,"return _v;");
                    aot->indent--; aot_emit(aot,"}");
                } else {
                    int nf=es->variant_field_counts[i];
                    aot_emit(aot,"static SageValue %s_%s(",ename,vname);
                    aot->indent++;
                    for(int f=0;f<nf;f++){char*fn=aot_cname_tok(es->variant_fields[i][f]);aot_emit(aot,"SageValue %s%s",fn,f<nf-1?",":"");free(fn);}
                    aot->indent--;
                    aot_emit(aot,"){"); aot->indent++;
                    aot_emit(aot,"SageValue _v=sage_rt_dict_new();");
                    aot_emit(aot,"sage_rt_dict_set(_v,sage_rt_string(\"__tag\"),sage_rt_string(\"%.*s\"));",es->variant_names[i].length,es->variant_names[i].start);
                    for(int f=0;f<nf;f++){char*fn=aot_cname_tok(es->variant_fields[i][f]);char*esc=aot_escape(es->variant_fields[i][f].start);esc[es->variant_fields[i][f].length]='\0';aot_emit(aot,"sage_rt_dict_set(_v,sage_rt_string(\"%s\"),%s);",esc,fn);free(fn);free(esc);}
                    aot_emit(aot,"return _v;"); aot->indent--; aot_emit(aot,"}");
                }
                free(vname);
            }
            // Emit enum namespace variable: sg_Color = dict{"Red": sg_Color_sg_Red(), ...}
            aot_emit(aot,"static SageValue %s;",ename);
            aot_blank(aot); free(ename); break;
        }
        case STMT_CLASS: {
            ClassStmt*cs=&stmt->as.class_stmt;
            char*cname=aot_cname_tok(cs->name);
            // Count methods and fields
            int mcount=0;
            for(Stmt*m=cs->methods;m;m=m->next) if(m->type==STMT_PROC) mcount++;
            // Set super-dispatch context so method bodies can resolve super calls statically
            snprintf(aot->current_class_cname, sizeof(aot->current_class_cname), "%s", cname);
            if (cs->has_parent && cs->parent.length > 0) {
                char* pcn = aot_cname_tok(cs->parent);
                snprintf(aot->current_parent_cname, sizeof(aot->current_parent_cname), "%s", pcn);
                free(pcn);
            } else {
                aot->current_parent_cname[0] = '\0';
            }
            // Emit method implementations
            for(Stmt*m=cs->methods;m;m=m->next){
                if(m->type!=STMT_PROC) continue;
                char*mname=aot_cname_tok(m->as.proc.name);
                aot_emit(aot,"static SageValue %s_%s(SageInst* _self, int _argc, SageValue* _argv) {",cname,mname);
                aot->indent++;
                // Bind self
                aot_emit(aot,"SageValue sg_self; sg_self.type=SAGE_VAL_INSTANCE; sg_self.as.instance=_self;");
                // Bind params (skip 'self')
                int pi=0;
                for(int i=0;i<m->as.proc.param_count;i++){
                    if(m->as.proc.params[i].length==4&&memcmp(m->as.proc.params[i].start,"self",4)==0) continue;
                    char*pn=aot_cname_tok(m->as.proc.params[i]);
                    aot_emit(aot,"SageValue %s=(_argc>%d)?_argv[%d]:sage_rt_nil();",pn,pi,pi);
                    free(pn); pi++;
                }
                aot_infer_body(aot,m->as.proc.body);
                int _saved_ipb = aot->in_proc_body;
                aot->in_proc_body = 1;
                for(Stmt*bs=m->as.proc.body;bs;bs=bs->next) aot_compile_stmt(aot,bs);
                aot->in_proc_body = _saved_ipb;
                aot_emit(aot,"return sage_rt_nil();"); aot->indent--; aot_emit(aot,"}");
                free(mname);
            }
            // Emit method table
            aot_emit(aot,"static SageMethod _%s_methods[] = {",cname);
            aot->indent++;
            for(Stmt*m=cs->methods;m;m=m->next){
                if(m->type!=STMT_PROC) continue;
                // Get the raw method name (not cname-mangled for the string key)
                char mraw[256]; int ml=m->as.proc.name.length<255?m->as.proc.name.length:255;
                memcpy(mraw,m->as.proc.name.start,ml); mraw[ml]='\0';
                char*mname=aot_cname_tok(m->as.proc.name);
                aot_emit(aot,"{\"%s\", %s_%s},",mraw,cname,mname);
                free(mname);
            }
            aot->indent--; aot_emit(aot,"};");
            // Emit class definition (registered at startup)
            char rawname[256]; int nl=cs->name.length<255?cs->name.length:255;
            memcpy(rawname,cs->name.start,nl); rawname[nl]='\0';
            aot_emit(aot,"static SageClass _%s_class = { \"%s\", NULL, _%s_methods, %d, NULL, 0, 0 };",
                     cname,rawname,cname,mcount);
            aot_emit(aot,"static SageValue _%s_classval;",cname);
            // Count init params (excluding 'self') and find if init exists
            int init_params = 0;
            int has_init = 0;
            char init_cname[128]="";
            for(Stmt*m=cs->methods;m;m=m->next){
                if(m->type==STMT_PROC && m->as.proc.name.length==4 &&
                   memcmp(m->as.proc.name.start,"init",4)==0){
                    has_init=1;
                    char*mn=aot_cname_tok(m->as.proc.name);
                    snprintf(init_cname,sizeof(init_cname),"%s_%s",cname,mn);
                    free(mn);
                    for(int i=0;i<m->as.proc.param_count;i++){
                        if(m->as.proc.params[i].length==4&&memcmp(m->as.proc.params[i].start,"self",4)==0) continue;
                        init_params++;
                    }
                    break;
                }
            }
            // Count call-site args to get max params needed (covers inherited inits)
            int call_max = init_params;
            {   // scan all call sites for this class constructor
                char craw[256]; int crl=cs->name.length<255?cs->name.length:255;
                memcpy(craw,cs->name.start,crl); craw[crl]='\0';
                // check aot_param_type at indices beyond init_params
                for(int ci=init_params;ci<8;ci++){
                    JitTypeTag pt=aot_param_type(aot,cs->name.start,cs->name.length,ci);
                    if(pt==JIT_TYPE_UNKNOWN&&ci>call_max) break;
                    if(pt!=JIT_TYPE_UNKNOWN) call_max=ci+1;
                }
            }
            int ctor_params = call_max > init_params ? call_max : init_params;
            // Emit constructor
            aot_emit(aot,"static SageValue %s(",cname);
            for(int i=0;i<ctor_params;i++){
                char pbuf[32]; snprintf(pbuf,sizeof(pbuf),"SageValue _a%d%s",i,i<ctor_params-1?",":"");
                aot_emit_raw(aot, pbuf);
            }
            if(ctor_params==0) aot_emit_raw(aot,"void");
            aot_emit_raw(aot,") {\n");
            aot->indent++;
            aot_emit(aot,"SageValue _inst = sage_rt_instance_new(_%s_classval);",cname);
            if(has_init){
                if(ctor_params>0){
                    char call_buf[512]; int cpos=0;
                    cpos+=snprintf(call_buf+cpos,sizeof(call_buf)-cpos,
                        "{ SageValue _iargs[%d]={",ctor_params);
                    for(int i=0;i<ctor_params;i++)
                        cpos+=snprintf(call_buf+cpos,sizeof(call_buf)-cpos,"%s_a%d",i?",":"",i);
                    cpos+=snprintf(call_buf+cpos,sizeof(call_buf)-cpos,
                        "}; %s(_inst.as.instance,%d,_iargs); }",init_cname,ctor_params);
                    aot_emit(aot,"%s",call_buf);
                } else {
                    aot_emit(aot,"%s(_inst.as.instance,0,NULL);",init_cname);
                }
            } else if(ctor_params>0 && cs->has_parent && cs->parent.length>0){
                // No local init but has parent — call parent init with args
                char parent_cname[128];
                char* pcn = aot_cname_tok(cs->parent);
                snprintf(parent_cname, sizeof(parent_cname), "%s_sg_init", pcn);
                free(pcn);
                char call_buf[512]; int cpos=0;
                cpos+=snprintf(call_buf+cpos,sizeof(call_buf)-cpos,
                    "if (%s != NULL) { SageValue _piargs[%d]={",parent_cname,ctor_params);
                for(int i=0;i<ctor_params;i++)
                    cpos+=snprintf(call_buf+cpos,sizeof(call_buf)-cpos,"%s_a%d",i?",":"",i);
                cpos+=snprintf(call_buf+cpos,sizeof(call_buf)-cpos,
                    "}; %s(_inst.as.instance,%d,_piargs); }",parent_cname,ctor_params);
                // Use a simpler approach: just call it unconditionally
                char call_buf2[512];
                snprintf(call_buf2,sizeof(call_buf2),
                    "{ SageValue _piargs[%d]={",ctor_params);
                char* p2 = call_buf2+strlen(call_buf2);
                for(int i=0;i<ctor_params;i++)
                    p2+=sprintf(p2,"%s_a%d",i?",":"",i);
                p2+=sprintf(p2,"}; %s(_inst.as.instance,%d,_piargs); }",
                    parent_cname,ctor_params);
                aot_emit(aot,"%s",call_buf2);
            }
            aot_emit(aot,"return _inst;");
            aot->indent--; aot_emit(aot,"}");
            aot_blank(aot);
            // Clear super-dispatch context
            aot->current_class_cname[0] = '\0';
            aot->current_parent_cname[0] = '\0';
            free(cname); break;
        }
        case STMT_IMPL: {
            ImplStmt*is=&stmt->as.impl_stmt;
            char*tname=aot_cname_tok(is->target);
            // Count methods
            int impl_mc=0;
            for(Stmt*m=is->methods;m;m=m->next) if(m->type==STMT_PROC) impl_mc++;
            for(Stmt*m=is->methods;m;m=m->next){
                if(m->type!=STMT_PROC) continue;
                char*mname=aot_cname_tok(m->as.proc.name);
                aot_emit(aot,"static SageValue %s_%s(SageInst* _self, int _argc, SageValue* _argv) {",tname,mname);
                aot->indent++;
                aot_emit(aot,"SageValue sg_self; sg_self.type=SAGE_VAL_INSTANCE; sg_self.as.instance=_self;");
                int _mpi=0;
                for(int i=0;i<m->as.proc.param_count;i++){
                    if(m->as.proc.params[i].length==4&&memcmp(m->as.proc.params[i].start,"self",4)==0) continue;
                    char*pn=aot_cname_tok(m->as.proc.params[i]);
                    aot_emit(aot,"SageValue %s=(_argc>%d)?_argv[%d]:sage_rt_nil();",pn,_mpi,_mpi);
                    free(pn); _mpi++;
                }
                aot_infer_body(aot,m->as.proc.body);
                for(Stmt*bs=m->as.proc.body;bs;bs=bs->next) aot_compile_stmt(aot,bs);
                aot_emit(aot,"return sage_rt_nil();"); aot->indent--; aot_emit(aot,"}"); free(mname);
            }
            // Emit method table for impl
            if(impl_mc>0){
                aot_emit(aot,"static SageMethod _%s_impl_methods[] = {",tname);
                aot->indent++;
                for(Stmt*m=is->methods;m;m=m->next){
                    if(m->type!=STMT_PROC) continue;
                    char mraw[256]; int ml=m->as.proc.name.length<255?m->as.proc.name.length:255;
                    memcpy(mraw,m->as.proc.name.start,ml); mraw[ml]='\0';
                    char*mname=aot_cname_tok(m->as.proc.name);
                    aot_emit(aot,"{\"%s\",%s_%s},",mraw,tname,mname);
                    free(mname);
                }
                aot->indent--; aot_emit(aot,"};");
            }
            free(tname); aot_blank(aot); break;
        }
        case STMT_IMPORT: {
            const char* mname = stmt->as.import.module_name;
            if (!mname) break;
            // Do not recursively process imports inside a module body
            // (they would be emitted inside a function which is invalid C)
            // Exception: _math is a native preamble module — always allow it
            // Exception: regular modules already imported can be allowed (they're already compiled)
            if (aot->in_module_body) {
                if (strcmp(mname,"_math")!=0) {
                    // Check if it's already imported — if so, nothing to emit (skip silently)
                    int _already = 0;
                    for (int _i=0; _i<aot->imported_module_count; _i++)
                        if (strcmp(aot->imported_modules[_i], mname)==0) { _already=1; break; }
                    if (_already) break;
                    // Not imported yet: needs to be compiled at file scope — skip for now
                    // (the outer scan should have caught it; if not, it's a nested import)
                    break;
                }
            }
            // Extract short name: "std.argparse" → "argparse" (last dotted component)
            const char* short_name = strrchr(mname, '.');
            short_name = short_name ? short_name + 1 : mname;
            if (stmt->as.import.alias && stmt->as.import.alias[0])
                short_name = stmt->as.import.alias;

            // Check if already compiled
            int already2 = 0;
            for (int i=0; i<aot->imported_module_count; i++)
                if (strcmp(aot->imported_modules[i], mname)==0) { already2=1; break; }

            // Handle "from X import Y, Z" items for already-imported modules
            // (for fresh imports, this is handled after module compilation below)
            if (stmt->as.import.item_count > 0 && stmt->as.import.items && already2) {
                const char* _full_pfx_early = NULL;
                for (int _pmi=0; _pmi<aot->mod_prefix_map_count; _pmi++) {
                    if (strcmp(aot->mod_prefix_map[_pmi].short_name, short_name)==0) {
                        _full_pfx_early = aot->mod_prefix_map[_pmi].full_prefix; break;
                    }
                }
                if (_full_pfx_early) {
                    for (int _ii=0; _ii<stmt->as.import.item_count; _ii++) {
                        const char* item = stmt->as.import.items[_ii];
                        if (!item) continue;
                        char* item_c = aot_cname(item, strlen(item));
                        char pfx_fn2[256]; snprintf(pfx_fn2,sizeof(pfx_fn2),"%s%s",_full_pfx_early,item_c);
                        if (strcmp(item_c, pfx_fn2) != 0) {
                            aot_emit(aot, "#define %s %s", item_c, pfx_fn2);
                            aot_register_proc(aot, item_c);
                        }
                        free(item_c);
                    }
                }
            }

            if (already2) break;
            // Record
            if (aot->imported_module_count < 64)
                snprintf(aot->imported_modules[aot->imported_module_count++], 128, "%s", mname);

            // Resolve module path via global_module_cache
            // But first: check if this is a force-stubbed module (e.g. thread.sage uses
            // "spawn" which is a reserved keyword, causing parse failure)
            {
                static const char* _skip_sage[] = {
                    "thread","atomic","channel","gc",NULL
                };
                int _should_skip = 0;
                for(int _ki=0; _skip_sage[_ki]; _ki++)
                    if(strcmp(mname,_skip_sage[_ki])==0){_should_skip=1;break;}
                if(_should_skip){ break; }
            }
            char* path = resolve_module_path(global_module_cache, mname);
            if (!path) {
                // Special case: _math is a native interpreter module that maps to C <math.h>
                // Emit real C wrappers and #define aliases for from _math import *
                if (strcmp(mname, "_math") == 0) {
                    static const struct { const char* fn; const char* c_expr; } _mfns[] = {
                        {"sin",  "sage_rt_float(sin(sage_rt_to_float(_a)))"},
                        {"cos",  "sage_rt_float(cos(sage_rt_to_float(_a)))"},
                        {"tan",  "sage_rt_float(tan(sage_rt_to_float(_a)))"},
                        {"asin", "sage_rt_float(asin(sage_rt_to_float(_a)))"},
                        {"acos", "sage_rt_float(acos(sage_rt_to_float(_a)))"},
                        {"atan", "sage_rt_float(atan(sage_rt_to_float(_a)))"},
                        {"atan2","sage_rt_float(atan2(sage_rt_to_float(_a),sage_rt_to_float(_b)))"},
                        {"sqrt", "sage_rt_float(sqrt(sage_rt_to_float(_a)))"},
                        {"pow",  "sage_rt_float(pow(sage_rt_to_float(_a),sage_rt_to_float(_b)))"},
                        {"log",  "sage_rt_float(log(sage_rt_to_float(_a)))"},
                        {"log10","sage_rt_float(log10(sage_rt_to_float(_a)))"},
                        {"exp",  "sage_rt_float(exp(sage_rt_to_float(_a)))"},
                        {"floor","sage_rt_float(floor(sage_rt_to_float(_a)))"},
                        {"ceil", "sage_rt_float(ceil(sage_rt_to_float(_a)))"},
                        {"round","sage_rt_float(round(sage_rt_to_float(_a)))"},
                        {"fmod", "sage_rt_float(fmod(sage_rt_to_float(_a),sage_rt_to_float(_b)))"},
                        {"isnan","sage_rt_bool(isnan(sage_rt_to_float(_a)))"},
                        {"isinf","sage_rt_bool(isinf(sage_rt_to_float(_a)))"},
                        {NULL,NULL}
                    };
                    // Helper: sage_rt_to_float — extract double from SageValue
                    aot_emit(aot,"static inline double sage_rt_to_float(SageValue v){");
                    aot_emit(aot,"  if(v.type==SAGE_VAL_FLOAT)return v.as.number;");
                    aot_emit(aot,"  if(v.type==SAGE_VAL_INT)return(double)v.as.integer;");
                    aot_emit(aot,"  return 0.0;}");
                    for (int _mi=0; _mfns[_mi].fn; _mi++) {
                        const char* fn = _mfns[_mi].fn;
                        const char* expr = _mfns[_mi].c_expr;
                        int two_args = strstr(expr,"_b") != NULL;
                        if (two_args)
                            aot_emit(aot,"static SageValue sg__math_sg_%s(SageValue _a,SageValue _b){return %s;}",fn,expr);
                        else
                            aot_emit(aot,"static SageValue sg__math_sg_%s(SageValue _a){return %s;}",fn,expr);
                        // #define alias: when math.sage (prefix sg_math_) uses from _math import *,
                        // calls become sg_math_sg_sin — alias to our wrapper.
                        // SKIP functions that math.sage redefines (sqrt, floor, ceil, round)
                        // to avoid conflicting with its own sg_math_sg_sqrt etc.
                        static const char* _math_sage_overrides[] = {"sqrt","floor","ceil","round",NULL};
                        int _overridden = 0;
                        for (int _oi=0; _math_sage_overrides[_oi]; _oi++)
                            if (strcmp(fn, _math_sage_overrides[_oi])==0) { _overridden=1; break; }
                        if (!_overridden)
                            aot_emit(aot,"#define sg_math_sg_%s sg__math_sg_%s",fn,fn);
                        aot_register_proc(aot, fn);
                    }
                    // Constants as #defines (accessed as sg_pi etc. inside math.sage)
                    aot_emit(aot,"static SageValue _sg_math_pi = {0};");
                    aot_emit(aot,"#define sg_pi _sg_math_pi");
                    aot_emit(aot,"#define sg_math_sg_pi _sg_math_pi");
                    aot_emit(aot,"static SageValue _sg_math_e_val = {0};");
                    aot_emit(aot,"#define sg_e _sg_math_e_val");
                    aot_emit(aot,"#define sg_math_sg_e _sg_math_e_val");
                    aot_emit(aot,"static SageValue _sg_math_tau = {0};");
                    aot_emit(aot,"#define sg_tau _sg_math_tau");
                    aot_emit(aot,"#define sg_math_sg_tau _sg_math_tau");
                    // Register mod_prefix_map so from-import aliases work
                    if (aot->mod_prefix_map_count < 64) {
                        snprintf(aot->mod_prefix_map[aot->mod_prefix_map_count].short_name,64,"_math");
                        snprintf(aot->mod_prefix_map[aot->mod_prefix_map_count].full_prefix,128,"sg__math_sg_");
                        aot->mod_prefix_map_count++;
                    }
                    // Register pi, e, tau in mod_procs so they get set in main() dict init
                    // for math.pi / math.e / math.tau access via sage_rt_dict_get
                    if (aot->mod_proc_count+4 < 512) {
                        // Also initialize the static vars used inside math.sage procs
                        struct { const char* raw; const char* val; } _consts[] = {
                            {"pi","sage_rt_float(3.14159265358979323846)"},
                            {"e","sage_rt_float(2.71828182845904523536)"},
                            {"tau","sage_rt_float(6.28318530717958647692)"},
                            {NULL,NULL}
                        };
                        for (int _ci=0; _consts[_ci].raw; _ci++) {
                            snprintf(aot->mod_procs[aot->mod_proc_count].mod_cname,64,"sg_math");
                            snprintf(aot->mod_procs[aot->mod_proc_count].proc_raw,64,"%s",_consts[_ci].raw);
                            snprintf(aot->mod_procs[aot->mod_proc_count].wrap_cname,1024,"@@%s",_consts[_ci].val);
                            aot->mod_proc_count++;
                        }
                    }
                    break;
                }
                // Special case: string native module — map to sage_rt_str_* runtime functions
                if (strcmp(mname, "string") == 0) {
                    static const struct { const char* fn; const char* rt; int na; } _sfns[] = {
                        {"find",       NULL,                     2},  // custom: returns float like interpreter
                        {"rfind",      NULL,                     2},  // same as find
                        {"startswith", "sage_rt_str_startswith", 2},
                        {"endswith",   "sage_rt_str_endswith",   2},
                        {"contains",   "sage_rt_str_startswith", 2},  // approximate
                        {"reverse",    NULL,                     1},  // custom below
                        {"repeat",     NULL,                     2},  // custom below (needs int arg)
                        {NULL,NULL,0}
                    };
                    char _mcn_str[16]; snprintf(_mcn_str,sizeof(_mcn_str),"sg_string");
                    aot_emit(aot,"static SageValue %s = {0};", _mcn_str);
                    aot_set_var_type(aot, "string", JIT_TYPE_DICT);
                    if (aot->mod_prefix_map_count < 64) {
                        snprintf(aot->mod_prefix_map[aot->mod_prefix_map_count].short_name,64,"string");
                        // Use sg_string_ prefix (fn_c adds sg_ so result = sg_string_sg_find)
                        snprintf(aot->mod_prefix_map[aot->mod_prefix_map_count].full_prefix,128,"sg_string_");
                        aot->mod_prefix_map_count++;
                    }
                    for (int _sfi=0; _sfns[_sfi].fn; _sfi++) {
                        const char* fn = _sfns[_sfi].fn;
                        char sfn[64]; snprintf(sfn,sizeof(sfn),"sg_string_sg_%s",fn);
                        if (!_sfns[_sfi].rt) {
                            if (strcmp(fn,"reverse")==0) {
                                // Custom reverse: iterate chars
                                aot_emit(aot,"static SageValue %s(SageValue _a){",sfn);
                                aot_emit(aot,"  if(!SAGE_IS_STRING(_a))return _a;");
                                aot_emit(aot,"  int _l=strlen(_a.as.string);char*_r=(char*)malloc(_l+1);");
                                aot_emit(aot,"  for(int _i=0;_i<_l;_i++)_r[_i]=_a.as.string[_l-1-_i];_r[_l]=0;");
                                aot_emit(aot,"  return sage_rt_string(_r);}");
                            } else if (strcmp(fn,"find")==0||strcmp(fn,"rfind")==0) {
                                // find returns float (like interpreter int-as-float)
                                aot_emit(aot,"static SageValue %s(SageValue _a,SageValue _b){",sfn);
                                aot_emit(aot,"  SageValue _r=sage_rt_str_find(_a,_b);");
                                aot_emit(aot,"  return sage_rt_float((double)SAGE_AS_INT64(_r));}");
                            } else { // repeat — needs int arg
                                aot_emit(aot,"static SageValue %s(SageValue _a,SageValue _b){",sfn);
                                aot_emit(aot,"  return sage_rt_str_repeat(_a,(int64_t)SAGE_AS_INT64(_b));}");
                            }
                        } else if (_sfns[_sfi].na==2) {
                            aot_emit(aot,"static SageValue %s(SageValue _a,SageValue _b){return %s(_a,_b);}",sfn,_sfns[_sfi].rt);
                        } else {
                            aot_emit(aot,"static SageValue %s(SageValue _a){return %s(_a,sage_rt_nil());}",sfn,_sfns[_sfi].rt);
                        }
                        aot_register_proc(aot, sfn);
                        if (aot->mod_proc_count < 512) {
                            snprintf(aot->mod_procs[aot->mod_proc_count].mod_cname,64,"%s",_mcn_str);
                            snprintf(aot->mod_procs[aot->mod_proc_count].proc_raw,64,"%s",fn);
                            // Use single wrap (no @@) so dict init doesn't try to set static var
                            snprintf(aot->mod_procs[aot->mod_proc_count].wrap_cname,1024,
                                     "_mwrap_placeholder_%s",sfn);
                            // Actually use the direct fn pointer emission format
                            snprintf(aot->mod_procs[aot->mod_proc_count].wrap_cname,1024,"%s",sfn);
                            aot->mod_proc_count++;
                        }
                    }
                    break;
                }
                aot_emit(aot, "/* import %s: not found, using stubs */", mname);
                // Emit nil-returning stubs for common module functions
                // so module.method() calls compile even without real implementation
                static const struct { const char* mod; const char* fn; int nargs; } _stubs[] = {
                    {"thread","spawn",1},{"thread","join",1},{"thread","id",0},
                    {"thread","sleep",1},{"thread","yield",0},{"thread","mutex",0},
                    {"thread","lock",1},{"thread","unlock",1},{"thread","try_lock",1},
                    {"semaphore","new",1},{"semaphore","wait",1},{"semaphore","signal",1},
                    {"sem","new",1},{"sem","wait",1},{"sem","post",1},{"sem","destroy",1},
                    {"rwlock","new",0},{"rwlock","read",1},{"rwlock","write",1},{"rwlock","release",1},
                    {"signal","set",2},{"signal","raise",1},{"signal","ignore",1},
                    // atomic module (bare import atomic, not std.atomic)
                    {"atomic","new",1},{"atomic","load",1},{"atomic","store",2},
                    {"atomic","add",2},{"atomic","sub",2},{"atomic","cas",3},{"atomic","exchange",2},
                    // channel module
                    {"channel","new",0},{"channel","send",2},{"channel","recv",1},
                    {"channel","try_recv",1},{"channel","close",1},{"channel","select",1},
                    {"channel","is_closed",1},{"channel","len",1},
                    // socket/net module stubs
                    {"socket","connect",2},{"socket","send",2},{"socket","recv",1},
                    {"socket","close",1},{"socket","http_get",1},{"socket","http_post",2},
                    {"socket","bind",2},{"socket","listen",1},{"socket","accept",1},
                    // net module
                    {"net","get",1},{"net","post",2},{"net","fetch",1},
                    // io module
                    {"io","readfile",1},{"io","writefile",2},{"io","exists",1},
                    {"io","mkdir",1},{"io","listdir",1},{"io","remove",1},
                    // doc module
                    {"doc","tag",2},{"doc","get",1},{"doc","list",0},
                    // hash module
                    {"hash","md5",1},{"hash","sha256",1},{"hash","sha1",1},
                    // path utilities (hash_paths uses)
                    {"path","basename",1},{"path","dirname",1},{"path","join",2},
                    {"path","exists",1},{"path","ext",1},
                    // macro stubs
                    {"timed",0},
                    // python FFI
                    {"python","import",1},{"python","call",3},{"python","get",2},
                    {"python","getattr",2},{"python","setattr",3},
                    {"python","set",3},{"python","eval",1},{"python","exec",1},
                    // ffi module
                    {"ffi","open",1},{"ffi","close",1},{"ffi","sym",2},{"ffi","call",2},
                    // bytes module
                    {"bytes","new",1},{"bytes","get",2},{"bytes","set",3},
                    {"bytes","len",1},{"bytes","to_str",1},{"bytes","from_str",1},
                    // gc module
                    {"gc","collect",0},{"gc","disable",0},{"gc","enable",0},
                    {"gc","collections",0},{"gc","alloc_count",0},
                    // addressof/sizeof
                    {"addressof","of",1},{"ptr","add",2},{"sizeof","of",1},
                    // smp/cpu
                    {"smp","count",0},{"smp","id",0},{"cpu","count",0},
                    {"cpu","has_hyperthreading",0},
                    {NULL,NULL,0}
                };
                for (int _si=0; _stubs[_si].mod; _si++) {
                    if (strcmp(_stubs[_si].mod, short_name)!=0) continue;
                    char sfn[128]; snprintf(sfn,sizeof(sfn),"sg_%s_sg_%s",short_name,_stubs[_si].fn);
                    const char* m=_stubs[_si].mod; const char* f=_stubs[_si].fn;
                    const char* body = NULL;  // real-runtime body when available
                    if(!strcmp(m,"io")){
                        if(!strcmp(f,"writefile")) body="return sage_rt_io_writefile(_a,_b);";
                        else if(!strcmp(f,"readfile")) body="return sage_rt_io_readfile(_a);";
                        else if(!strcmp(f,"exists")) body="return sage_rt_io_exists(_a);";
                        else if(!strcmp(f,"remove")) body="return sage_rt_io_remove(_a);";
                    } else if(!strcmp(m,"path")){
                        if(!strcmp(f,"basename")) body="return sage_rt_path_basename(_a);";
                        else if(!strcmp(f,"dirname")) body="return sage_rt_path_dirname(_a);";
                        else if(!strcmp(f,"join")) body="return sage_rt_path_join(2,(SageValue[]){_a,_b});";
                        else if(!strcmp(f,"exists")) body="return sage_rt_path_exists(_a);";
                        else if(!strcmp(f,"ext")) body="return sage_rt_path_ext(_a);";
                    } else if(!strcmp(m,"gc")){
                        if(!strcmp(f,"collect")) body="sage_rt_gc_collect();return sage_rt_nil();";
                        else if(!strcmp(f,"disable")) body="sage_rt_gc_disable();return sage_rt_nil();";
                        else if(!strcmp(f,"enable")) body="sage_rt_gc_enable();return sage_rt_nil();";
                        else if(!strcmp(f,"collections")||!strcmp(f,"alloc_count")) body="return sage_rt_gc_collections();";
                    } else if(!strcmp(m,"addressof")&&!strcmp(f,"of")){ body="return sage_rt_addressof(_a);";
                    } else if(!strcmp(m,"sizeof")&&!strcmp(f,"of")){ body="return sage_rt_sizeof(_a);";
                    } else if(!strcmp(m,"ptr")&&!strcmp(f,"add")){ body="return sage_rt_ptr_add(_a,_b);";
                    } else if((!strcmp(m,"smp")&&!strcmp(f,"count"))||(!strcmp(m,"cpu")&&!strcmp(f,"count"))){ body="return sage_rt_cpu_count();";
                    } else if(!strcmp(m,"cpu")&&!strcmp(f,"has_hyperthreading")){ body="return sage_rt_cpu_has_hyperthreading();";
                    } else if(!strcmp(m,"bytes")){
                        if(!strcmp(f,"new")) body="return sage_rt_bytes_ctor(_a);";
                        else if(!strcmp(f,"get")) body="return sage_rt_bytes_get_v(_a,_b);";
                        else if(!strcmp(f,"set")) body="return sage_rt_bytes_set_v(_a,_b,_c);";
                        else if(!strcmp(f,"len")) body="return sage_rt_bytes_len_v(_a);";
                        else if(!strcmp(f,"to_str")) body="return sage_rt_bytes_to_string(_a);";
                        else if(!strcmp(f,"from_str")) body="return sage_rt_bytes_from_string(_a);";
                    }
                    if(body){
                        int na=_stubs[_si].nargs;
                        if(na==0) aot_emit(aot,"static SageValue %s(void){%s}",sfn,body);
                        else if(na==1) aot_emit(aot,"static SageValue %s(SageValue _a){(void)_a;%s}",sfn,body);
                        else if(na==2) aot_emit(aot,"static SageValue %s(SageValue _a,SageValue _b){(void)_a;(void)_b;%s}",sfn,body);
                        else aot_emit(aot,"static SageValue %s(SageValue _a,SageValue _b,SageValue _c){(void)_a;(void)_b;(void)_c;%s}",sfn,body);
                    }
                    else if (_stubs[_si].nargs==0) aot_emit(aot,"static SageValue %s(void){return sage_rt_nil();}",sfn);
                    else if (_stubs[_si].nargs==1) aot_emit(aot,"static SageValue %s(SageValue _a){(void)_a;return sage_rt_nil();}",sfn);
                    else if (_stubs[_si].nargs==2) aot_emit(aot,"static SageValue %s(SageValue _a,SageValue _b){(void)_a;(void)_b;return sage_rt_nil();}",sfn);
                    else aot_emit(aot,"static SageValue %s(SageValue _a,SageValue _b,SageValue _c){(void)_a;(void)_b;(void)_c;return sage_rt_nil();}",sfn);
                    aot_register_proc(aot, sfn);
                    // Register in mod_prefix_map and mod_procs for dict init
                    if (aot->mod_prefix_map_count < 64) {
                        snprintf(aot->mod_prefix_map[aot->mod_prefix_map_count].short_name,64,"%s",short_name);
                        char _pfx[64]; snprintf(_pfx,64,"sg_%s_",short_name);
                        snprintf(aot->mod_prefix_map[aot->mod_prefix_map_count].full_prefix,128,"%s",_pfx);
                        aot->mod_prefix_map_count++;
                    }
                    if (aot->mod_proc_count < 512) {
                        char* _mcn_stub=aot_cname(short_name,strlen(short_name));
                        snprintf(aot->mod_procs[aot->mod_proc_count].mod_cname,64,"%s",_mcn_stub);
                        snprintf(aot->mod_procs[aot->mod_proc_count].proc_raw,64,"%s",_stubs[_si].fn);
                        // Use function name directly (not @@ which would try to assign to function)
                        snprintf(aot->mod_procs[aot->mod_proc_count].wrap_cname,1024,"%s",sfn);
                        aot->mod_proc_count++;
                        free(_mcn_stub);
                    }
                }
                // Declare the namespace dict var
                char* _stub_mcn=aot_cname(short_name,strlen(short_name));
                aot_emit(aot,"static SageValue %s;",_stub_mcn);
                aot_set_var_type(aot,short_name,JIT_TYPE_DICT);
                free(_stub_mcn);
                break;
            }
            char* source = read_file(path);
            free(path);
            if (!source) { aot_emit(aot,"/* import %s: read failed */",mname); break; }

            // Parse module source
            LexerState sl = lexer_get_state();
            ParserState sp = parser_get_state();
            init_lexer(source, mname);
            parser_init();
            // parse_program parses ALL statements and links them — parse() only returns one
            extern Stmt* parse_program(const char* source, const char* input_path);
            Stmt* mod_ast = parse_program(source, mname);
            lexer_set_state(sl);
            parser_set_state(sp);
            // NOTE: do NOT free(source) here — token .start pointers reference it!
            // It will be freed after all emission is complete.
            if (!mod_ast) { free(source); aot_emit(aot,"/* import %s: parse failed */",mname); break; }

            // Scan module AST for nested imports (e.g. assert.sage has `import math` inside proc)
            // and process them before compiling this module so they're available at file scope
            {
                // Recursive scan using a stack (limited depth)
                Stmt* _sq[256]; int _sh=0, _st=0;
                for(Stmt*_s=mod_ast;_s&&_st<256;_s=_s->next) _sq[_st++]=_s;
                while(_sh<_st){
                    Stmt*_s=_sq[_sh++];
                    if(_s->type==STMT_IMPORT) {
                        const char* _imn = _s->as.import.module_name;
                        if(_imn && strcmp(_imn,"_math")!=0) {
                            // Process this nested import at file scope if not already done
                            int _ai=0;
                            for(int _i=0;_i<aot->imported_module_count;_i++)
                                if(strcmp(aot->imported_modules[_i],_imn)==0){_ai=1;break;}
                            if(!_ai) { int _saved_mb=aot->in_module_body; aot->in_module_body=0; aot_compile_stmt(aot,_s); aot->in_module_body=_saved_mb; }
                        }
                    }
                    // Recurse into proc bodies, blocks, etc.
                    if((_s->type==STMT_PROC||_s->type==STMT_ASYNC_PROC)){ProcStmt*_ps=(_s->type==STMT_PROC)?&_s->as.proc:&_s->as.async_proc;for(Stmt*_b=_ps->body;_b&&_st<256;_b=_b->next)_sq[_st++]=_b;}
                    if(_s->type==STMT_BLOCK)for(Stmt*_b=_s->as.block.statements;_b&&_st<256;_b=_b->next)_sq[_st++]=_b;
                    if(_s->type==STMT_IF){for(Stmt*_b=_s->as.if_stmt.then_branch;_b&&_st<256;_b=_b->next)_sq[_st++]=_b;for(Stmt*_b=_s->as.if_stmt.else_branch;_b&&_st<256;_b=_b->next)_sq[_st++]=_b;}
                }
            }

            // Set module prefix for name-mangling
            char saved_prefix[128];
            snprintf(saved_prefix, sizeof(saved_prefix), "%s", aot->current_module_prefix);
            // Build prefix: "arrays" -> "sg_arrays_"
            // Build C-safe module prefix: dots → underscores (std.argparse → sg_std_argparse_)
            char mod_prefix[128]; {
                int mpl = 0; mod_prefix[mpl++]='s'; mod_prefix[mpl++]='g'; mod_prefix[mpl++]='_';
                for (const char* mp = mname; *mp && mpl < 122; mp++, mpl++)
                    mod_prefix[mpl] = (*mp == '.' || *mp == '/') ? '_' : *mp;
                mod_prefix[mpl++]='_'; mod_prefix[mpl]='\0';
            }
            snprintf(aot->current_module_prefix, sizeof(aot->current_module_prefix), "%s", mod_prefix);
            aot->in_module_body = 1;
            // Register short_name → full C prefix mapping
            if (aot->mod_prefix_map_count < 64) {
                snprintf(aot->mod_prefix_map[aot->mod_prefix_map_count].short_name, 64, "%s", short_name);
                snprintf(aot->mod_prefix_map[aot->mod_prefix_map_count].full_prefix, 128, "%s", mod_prefix);
                aot->mod_prefix_map_count++;
            }

            // Type-infer module body in an isolated type env snapshot
            // (prevent outer scope variable types from contaminating module functions)
            int saved_type_count_mod = aot->type_env.count;
            aot_infer_types(aot, mod_ast);

            // Emit a section comment
            aot_emit(aot, "/* ── module %s ─────────────────────────── */", mname);

            // Pre-scan: if module imports _math (native), emit C math.h wrappers NOW
            // so they appear before the forward-decl loop and #defines resolve correctly
            for (Stmt* _pms = mod_ast; _pms; _pms = _pms->next) {
                if (_pms->type == STMT_IMPORT &&
                    _pms->as.import.module_name &&
                    strcmp(_pms->as.import.module_name, "_math") == 0) {
                    aot_compile_stmt(aot, _pms);  // emits wrappers + marks _math imported
                    break;
                }
            }

            // First pass: forward-declare all module procs (prevents implicit-int errors)
            for (Stmt* ms = mod_ast; ms; ms = ms->next) {
                if (ms->type != STMT_PROC && ms->type != STMT_ASYNC_PROC) continue;
                ProcStmt* ps2 = (ms->type==STMT_PROC)?&ms->as.proc:&ms->as.async_proc;
                char* _pn = aot_cname_tok(ps2->name);
                // Full name = mod_prefix + full pn (mod_prefix already ends with _)
                // e.g. "sg_std_argparse_" + "sg_create" = "sg_std_argparse_sg_create"
                aot_emit(aot,"static SageValue %s%s();",mod_prefix,_pn);
                free(_pn);
            }
            aot_blank(aot);

            // Pre-register ALL module procs before emitting any bodies
            for (Stmt* ms2 = mod_ast; ms2; ms2 = ms2->next) {
                if (ms2->type != STMT_PROC && ms2->type != STMT_ASYNC_PROC) continue;
                ProcStmt* ps2b = (ms2->type==STMT_PROC)?&ms2->as.proc:&ms2->as.async_proc;
                char* pn_pre = aot_cname_tok(ps2b->name);
                char full_pfx_pre[256]; snprintf(full_pfx_pre,sizeof(full_pfx_pre),"%s%s",mod_prefix,pn_pre);
                aot_register_proc(aot, pn_pre);
                aot_register_proc(aot, full_pfx_pre);
                free(pn_pre);
            }
            // Emit #define aliases for all module LET and comptime LET vars
            for (Stmt* ms2 = mod_ast; ms2; ms2 = ms2->next) {
                if (ms2->type == STMT_LET) {
                    char* _avn = aot_cname_tok(ms2->as.let.name);
                    aot_emit(aot, "#define %s %s%s", _avn, mod_prefix, _avn);
                    free(_avn);
                } else if (ms2->type == STMT_COMPTIME) {
                    Stmt* _cbody = ms2->as.comptime.body;
                    if (_cbody && _cbody->type == STMT_BLOCK)
                        _cbody = _cbody->as.block.statements;
                    for (Stmt* _cs = _cbody; _cs; _cs = _cs->next) {
                        if (_cs->type == STMT_LET) {
                            char* _avn = aot_cname_tok(_cs->as.let.name);
                            aot_emit(aot, "#define %s %s%s", _avn, mod_prefix, _avn);
                            free(_avn);
                        }
                    }
                }
            }

            // Emit all top-level procs, classes, structs, enums from the module
            // Each gets the module prefix prepended to its C name
            for (Stmt* ms = mod_ast; ms; ms = ms->next) {
                if (ms->type == STMT_PROC || ms->type == STMT_ASYNC_PROC) {
                    // Reset type env to pre-module state before each proc
                    // so one proc's variable types don't contaminate the next
                    int saved_for_proc = aot->type_env.count;
                    aot->type_env.count = saved_type_count_mod;
                    // Only emit as module proc (global-scoped)
                    aot_emit_proc(aot, ms);
                    aot->type_env.count = saved_for_proc;
                    // Also emit a SageNativeFn wrapper for dict registration
                    ProcStmt* ps = (ms->type==STMT_PROC)?&ms->as.proc:&ms->as.async_proc;
                    char* pn_c   = aot_cname_tok(ps->name);
                    int np = ps->param_count;
                    // mcn2 = C name of the full module path (for function name prefix)
                    char* _mcn2_tmp=aot_cname(mname,strlen(mname)); char mcn2[256]; snprintf(mcn2,sizeof(mcn2),"%s",_mcn2_tmp); free(_mcn2_tmp);
                    // mcn_short = C name for the short module name (namespace dict var, e.g. sg_argparse)
                    char* _mcn_sn=aot_cname(short_name,strlen(short_name)); char mcn_short[128]; snprintf(mcn_short,sizeof(mcn_short),"%s",_mcn_sn); free(_mcn_sn);
                    char wrap_name[256]; snprintf(wrap_name,sizeof(wrap_name),"_mwrap_%s_%s",mcn2,pn_c);
                    aot_emit(aot,"static SageValue %s(int _argc, SageValue* _argv, void* _env) {",wrap_name);
                    aot->indent++;
                    aot_emit(aot,"(void)_env;");
                    char arglist[512]; int ap=0;
                    for (int pi=0; pi<np; pi++)
                        ap+=snprintf(arglist+ap, sizeof(arglist)-ap, "%s(_argc>%d?_argv[%d]:sage_rt_nil())",
                                     pi?",":"", pi, pi);
                    arglist[ap]='\0';
                    aot_emit(aot,"return %s_%s(%s);", mcn2, pn_c, arglist);
                    aot->indent--; aot_emit(aot,"}");
                    aot_register_proc(aot, pn_c);
                    char full_pfx[256]; snprintf(full_pfx,sizeof(full_pfx),"%s_%s",mcn2,pn_c);
                    aot_register_proc(aot, full_pfx);
                    // Register in mod_procs for main() dict population
                    // Use mcn_short (short name C var) so dict is keyed correctly
                    if (aot->mod_proc_count < 512) {
                        char pn_raw[64]; int prl=ps->name.length<63?ps->name.length:63;
                        memcpy(pn_raw,ps->name.start,prl); pn_raw[prl]='\0';
                        snprintf(aot->mod_procs[aot->mod_proc_count].mod_cname, 64, "%s", mcn_short);
                        snprintf(aot->mod_procs[aot->mod_proc_count].proc_raw,  64, "%s", pn_raw);
                        snprintf(aot->mod_procs[aot->mod_proc_count].wrap_cname,1024, "%s", wrap_name);
                        aot->mod_proc_count++;
                    }
                    free(pn_c);
                } else if (ms->type == STMT_CLASS || ms->type == STMT_STRUCT ||
                           ms->type == STMT_ENUM  || ms->type == STMT_IMPL) {
                    // Skip nested imports inside module body — they are interpreter-only
                    aot_compile_stmt(aot, ms);
                } else if (ms->type == STMT_LET) {
                    // Module-level variable — emit as static file-scope var
                    // so it's accessible from within module proc bodies
                    char* vn = aot_cname_tok(ms->as.let.name);
                    if (ms->as.let.initializer) {
                        char* val = aot_expr(aot, ms->as.let.initializer, JIT_TYPE_UNKNOWN);
                        aot_emit(aot, "static SageValue %s%s = {0}; /* module var */", mod_prefix, vn);
                        // Also register in mod_procs for dict export in main()
                        if (aot->mod_proc_count < 512) {
                            char raw_vn[64]; int prl=ms->as.let.name.length<63?ms->as.let.name.length:63;
                            memcpy(raw_vn,ms->as.let.name.start,prl); raw_vn[prl]='\0';
                            char* _mcn_sn2=aot_cname(short_name,strlen(short_name));
                            snprintf(aot->mod_procs[aot->mod_proc_count].mod_cname, 64, "%s", _mcn_sn2);
                            snprintf(aot->mod_procs[aot->mod_proc_count].proc_raw, 64, "%s", raw_vn);
                            snprintf(aot->mod_procs[aot->mod_proc_count].wrap_cname, 1024, "@@%s", val);
                            aot->mod_proc_count++;
                            free(_mcn_sn2);
                        }
                        free(val);
                    }
                    free(vn);
                } else if (ms->type == STMT_COMPTIME) {
                    // Comptime block — emit inner LET vars as static file-scope statics
                    // Parser wraps the body in a STMT_BLOCK — unwrap it
                    Stmt* _cbody2 = ms->as.comptime.body;
                    if (_cbody2 && _cbody2->type == STMT_BLOCK)
                        _cbody2 = _cbody2->as.block.statements;
                    for (Stmt* _cs = _cbody2; _cs; _cs = _cs->next) {
                        if (_cs->type == STMT_LET && _cs->as.let.initializer) {
                            char* _cvn = aot_cname_tok(_cs->as.let.name);
                            char* _cval = aot_expr(aot, _cs->as.let.initializer, JIT_TYPE_UNKNOWN);
                            aot_emit(aot, "static SageValue %s%s = {0}; /* comptime */", mod_prefix, _cvn);
                            if (aot->mod_proc_count < 512) {
                                char _crv[64]; int _prl=_cs->as.let.name.length<63?_cs->as.let.name.length:63;
                                memcpy(_crv,_cs->as.let.name.start,_prl); _crv[_prl]='\0';
                                char* _cmsn=aot_cname(short_name,strlen(short_name));
                                snprintf(aot->mod_procs[aot->mod_proc_count].mod_cname,64,"%s",_cmsn);
                                snprintf(aot->mod_procs[aot->mod_proc_count].proc_raw,64,"%s",_crv);
                                snprintf(aot->mod_procs[aot->mod_proc_count].wrap_cname,1024,"@@%s",_cval);
                                aot->mod_proc_count++;
                                free(_cmsn);
                            }
                            free(_cval); free(_cvn);
                        }
                    }
                }
                // Skip everything else (STMT_IMPORT, bare statements, etc.)
            }

            // Declare the module namespace variable using the short name (last component)
            // e.g. "std.argparse" → var is "sg_argparse", accessed as argparse.create(...)
            // LET var #defines are intentionally left active after module compilation
            // so cross-module references to variables like sg_PI, sg_E work correctly.
            // Individual procs use the full prefixed name via the EXPR_CALL rewrite.
            char* mcn = aot_cname(short_name, strlen(short_name));
            aot_emit(aot, "static SageValue %s; /* module %s namespace */", mcn, mname);
            aot_blank(aot);

            // Register module var type as DICT using short_name (matches Sage source)
            aot_set_var_type(aot, short_name, JIT_TYPE_DICT);
            // Also register in imported_modules under short_name for direct-call detection
            {
                int already_sn = 0;
                for (int _i=0; _i<aot->imported_module_count; _i++)
                    if (strcmp(aot->imported_modules[_i], short_name)==0) { already_sn=1; break; }
                if (!already_sn && aot->imported_module_count < 64)
                    snprintf(aot->imported_modules[aot->imported_module_count++], 128, "%s", short_name);
            }

            free(mcn);
            // Now safe to free the module source — all tokens have been processed
            free(source);
            // Restore type env to pre-module state (module vars don't pollute outer scope)
            aot->type_env.count = saved_type_count_mod;
            // Re-register the module namespace variable as DICT in the outer scope
            aot_set_var_type(aot, short_name, JIT_TYPE_DICT);
            // Handle "from X import Y, Z" items — register as global aliases
            // This must happen even if the module was already imported (multiple from-import lines)
            if (stmt->as.import.item_count > 0 && stmt->as.import.items) {
                // Find the full C prefix for this module (may be freshly registered above)
                const char* _full_pfx_items = NULL;
                for (int _pmi=0; _pmi<aot->mod_prefix_map_count; _pmi++) {
                    if (strcmp(aot->mod_prefix_map[_pmi].short_name, short_name)==0) {
                        _full_pfx_items = aot->mod_prefix_map[_pmi].full_prefix; break;
                    }
                }
                if (_full_pfx_items) {
                    for (int _ii=0; _ii<stmt->as.import.item_count; _ii++) {
                        const char* item = stmt->as.import.items[_ii];
                        if (!item) continue;
                        char* item_c = aot_cname(item, strlen(item));
                        // Check if #define was already emitted (avoid duplicate defines)
                        // We track by checking if item_c is already the SAME as a prefixed name
                        // Always emit #define to ensure C code can resolve the symbol
                        // (known_procs registration is separate from C-level name resolution)
                        char pfx_fn[256]; snprintf(pfx_fn,sizeof(pfx_fn),"%s%s",_full_pfx_items,item_c);
                        // Only emit if the item name differs from the prefixed name
                        if (strcmp(item_c, pfx_fn) != 0) {
                            // Check not already emitted in this compilation unit
                            int _already_emitted = 0;
                            for (int _ki=0; _ki<aot->known_proc_count; _ki++) {
                                if (strcmp(aot->known_procs[_ki], pfx_fn)==0) { _already_emitted=1; break; }
                            }
                            // emit the define regardless — multiple identical #defines are OK in C
                            aot_emit(aot, "#define %s %s", item_c, pfx_fn);
                            if (!_already_emitted) aot_register_proc(aot, item_c);
                        }
                        free(item_c);
                    }
                }
            }

            // Restore prefix and module-body flag
            snprintf(aot->current_module_prefix, sizeof(aot->current_module_prefix), "%s", saved_prefix);
            aot->in_module_body = 0;
            break;
        }
        case STMT_SPAWN: {
            // Synchronous model: run the spawned block inline now.
            aot_emit(aot,"{ /* spawn block (synchronous) */"); aot->indent++;
            for(Stmt*s=stmt->as.spawn_stmt.body;s;s=s->next) aot_compile_stmt(aot,s);
            aot->indent--; aot_emit(aot,"}");
            break;
        }
        case STMT_COMPTIME: {
            // Parser wraps comptime body in a STMT_BLOCK — unwrap it so vars
            // are emitted inline (not scoped) and stay visible to subsequent stmts
            Stmt* _ct_body = stmt->as.comptime.body;
            if (_ct_body && _ct_body->type == STMT_BLOCK)
                _ct_body = _ct_body->as.block.statements;
            aot_infer_body(aot, _ct_body);
            for (Stmt* s = _ct_body; s; s = s->next) aot_compile_stmt(aot, s);
            break;
        }
        case STMT_PROC: case STMT_ASYNC_PROC: {
            // Nested proc: emit capture struct + make_fn
            ProcStmt* ps=(stmt->type==STMT_PROC)?&stmt->as.proc:&stmt->as.async_proc;
            char* pname=aot_cname_tok(ps->name);
            char wname[128]; snprintf(wname,sizeof(wname),"_sw_%s",pname);
            char sname[140]; snprintf(sname,sizeof(sname),"_cap_%s",pname);
            // Collect captures for this nested proc
            const char* caps[64]; int ncaps=0;
            _collect_free_vars_stmt(ps->body, caps, &ncaps, 64, ps);
            if(ncaps>0){
                // Allocate capture struct and fill it
                aot_emit(aot,"%s* _env_%s = (%s*)malloc(sizeof(%s));",sname,pname,sname,sname);
                for(int i=0;i<ncaps;i++){
                    // caps[i] is a null-terminated name
                    int cl=(int)strlen(caps[i]);
                    char* cn=aot_cname(caps[i],cl);
                    JitTypeTag ct=aot_get_var_type(aot,(char*)caps[i]);
                    if(jit_is_unboxed(ct)){
                        char* bx=aot_box(ct,cn);
                        aot_emit(aot,"_env_%s->fields[%d]=%s;",pname,i,bx);
                        free(bx);
                    } else {
                        aot_emit(aot,"_env_%s->fields[%d]=%s;",pname,i,cn);
                    }
                    free(cn);
                }
                aot_emit(aot,"SageValue %s=sage_rt_make_fn((SageNativeFn)%s,_env_%s,\"%.*s\");",
                    pname,wname,pname,ps->name.length,ps->name.start);
            } else {
                aot_emit(aot,"SageValue %s=sage_rt_make_fn((SageNativeFn)%s,NULL,\"%.*s\");",
                    pname,wname,ps->name.length,ps->name.start);
            }
            free(pname);
            break;
        }
        case STMT_TRAIT: {
            // Emit trait as a static SageValue dict: {__name__: "TraitName", __methods__: [...]}
            if (!stmt->as.trait_stmt.name.start) break;
            char* tname = aot_cname_tok(stmt->as.trait_stmt.name);
            char raw_tn[64]; int prl=stmt->as.trait_stmt.name.length<63?stmt->as.trait_stmt.name.length:63;
            memcpy(raw_tn,stmt->as.trait_stmt.name.start,prl); raw_tn[prl]='\0';
            aot_emit(aot,"static SageValue %s;",tname);
            aot_set_var_type(aot, raw_tn, JIT_TYPE_DICT);
            // Build method push calls
            char push_expr[512]=""; int pp=0;
            for(Stmt*ms=stmt->as.trait_stmt.methods;ms;ms=ms->next){
                if(ms->type==STMT_PROC||ms->type==STMT_ASYNC_PROC){
                    ProcStmt*ps2=(ms->type==STMT_PROC)?&ms->as.proc:&ms->as.async_proc;
                    char mn[64]; int ml=ps2->name.length<63?ps2->name.length:63;
                    memcpy(mn,ps2->name.start,ml); mn[ml]='\0';
                    pp+=snprintf(push_expr+pp,sizeof(push_expr)-pp,
                        "sage_rt_array_push(_a,sage_rt_string(\"%s\"));",mn);
                }
            }
            // Register in mod_procs for main() init via @@ expression
            if (aot->mod_proc_count < 512) {
                // Use the trait's var name as both mod_cname and proc_raw
                // The @@ initializer assigns directly to sg_TraitName
                snprintf(aot->mod_procs[aot->mod_proc_count].mod_cname,64,"__trait__");
                snprintf(aot->mod_procs[aot->mod_proc_count].proc_raw,64,"%s",raw_tn);
                snprintf(aot->mod_procs[aot->mod_proc_count].wrap_cname,1024,
                    "@@({SageValue _td=sage_rt_dict_new();"
                    "sage_rt_dict_set(_td,sage_rt_string(\"__name__\"),sage_rt_string(\"%s\"));"
                    "SageValue _tm=({SageValue _a=sage_rt_array_new();%s_a;});"
                    "sage_rt_dict_set(_td,sage_rt_string(\"__methods__\"),_tm);_td;})",
                    raw_tn, push_expr);
                aot->mod_proc_count++;
            }
            free(tname);
            break;
        }
        case STMT_MACRO_DEF: break;
        default: aot_emit(aot,"/* unhandled stmt %d */",stmt->type); break;
    }
}


// ── Call-site type analysis ───────────────────────────────────────────────────
// Scans AST for all calls to a named function, collects arg types.
// Stores inferred param types as "procname#paramN" in type_env.

// Scan expr tree for calls to fname and record arg types
static void aot_scan_expr_calls(AotCompiler* aot, const char* fname, int flen, Expr* e) {
    if (!e) return;
    if (e->type == EXPR_CALL) {
        if (e->as.call.callee && e->as.call.callee->type == EXPR_VARIABLE) {
            const char* cn = e->as.call.callee->as.variable.name.start;
            int cl = e->as.call.callee->as.variable.name.length;
            if (cl == flen && memcmp(cn, fname, flen) == 0) {
                for (int i = 0; i < e->as.call.arg_count; i++) {
                    JitTypeTag t = aot_infer_expr(aot, e->as.call.args[i]);
                    char key[280], seen[300];
                    snprintf(key, sizeof(key), "%.*s#%d", flen, fname, i);
                    snprintf(seen, sizeof(seen), "%.*s#%d$seen", flen, fname, i);
                    // Param type is the type all call sites agree on. If any call
                    // site passes a different (or unknown) type, the param must be
                    // a generic SageValue — otherwise the proc body, compiled for
                    // the first call's type, mismatches the boxed arg the other
                    // caller passes (e.g. add(3,4) then add(d["k"],1)).
                    if (aot_get_var_type(aot, seen) == JIT_TYPE_UNKNOWN) {
                        // First observation of this param.
                        aot_set_var_type(aot, seen, JIT_TYPE_BOOL); // mark seen (sentinel)
                        aot_set_var_type(aot, key, t);
                    } else {
                        JitTypeTag prev = aot_get_var_type(aot, key);
                        if (prev != t) aot_set_var_type(aot, key, JIT_TYPE_UNKNOWN);
                    }
                }
            }
        }
        // Scan args recursively
        for (int i = 0; i < e->as.call.arg_count; i++)
            aot_scan_expr_calls(aot, fname, flen, e->as.call.args[i]);
        aot_scan_expr_calls(aot, fname, flen, e->as.call.callee);
    } else {
        // Recurse into sub-expressions
        switch (e->type) {
            case EXPR_BINARY:
                aot_scan_expr_calls(aot, fname, flen, e->as.binary.left);
                aot_scan_expr_calls(aot, fname, flen, e->as.binary.right);
                break;
            case EXPR_GET:
                aot_scan_expr_calls(aot, fname, flen, e->as.get.object);
                break;
            case EXPR_SET:
                aot_scan_expr_calls(aot, fname, flen, e->as.set.object);
                aot_scan_expr_calls(aot, fname, flen, e->as.set.value);
                break;
            case EXPR_INDEX:
                aot_scan_expr_calls(aot, fname, flen, e->as.index.array);
                aot_scan_expr_calls(aot, fname, flen, e->as.index.index);
                break;
            default: break;
        }
    }
}

static void aot_scan_stmt_calls(AotCompiler* aot, const char* fname, int flen, Stmt* s);

static void aot_collect_calls(AotCompiler* aot, const char* fname, int flen,
                               Stmt* program) {
    aot_scan_stmt_calls(aot, fname, flen, program);
}

static void aot_scan_stmt_calls(AotCompiler* aot, const char* fname, int flen, Stmt* s) {
    for (; s; s = s->next) {
        Expr* e = NULL;
        switch (s->type) {
            case STMT_EXPRESSION: e = s->as.expression; break;
            case STMT_LET:        e = s->as.let.initializer; break;
            case STMT_PRINT:      e = s->as.print.expression; break;
            case STMT_RETURN:     e = s->as.ret.value; break;
            case STMT_IF:
                aot_scan_expr_calls(aot, fname, flen, s->as.if_stmt.condition);
                aot_scan_stmt_calls(aot, fname, flen, s->as.if_stmt.then_branch);
                aot_scan_stmt_calls(aot, fname, flen, s->as.if_stmt.else_branch);
                break;
            case STMT_WHILE:
                aot_scan_expr_calls(aot, fname, flen, s->as.while_stmt.condition);
                aot_scan_stmt_calls(aot, fname, flen, s->as.while_stmt.body);
                break;
            case STMT_FOR:
                aot_scan_stmt_calls(aot, fname, flen, s->as.for_stmt.body);
                break;
            case STMT_BLOCK:
                aot_scan_stmt_calls(aot, fname, flen, s->as.block.statements);
                break;
            case STMT_PROC: case STMT_ASYNC_PROC: {
                ProcStmt* ps=(s->type==STMT_PROC)?&s->as.proc:&s->as.async_proc;
                aot_scan_stmt_calls(aot, fname, flen, ps->body);
                break;
            }
            default: break;
        }
        if (e) aot_scan_expr_calls(aot, fname, flen, e);
    }
}
static JitTypeTag aot_param_type(AotCompiler* aot, const char* fname, int flen, int idx) {
    char key[280];
    snprintf(key, sizeof(key), "%.*s#%d", flen, fname, idx);
    return aot_get_var_type(aot, key);
}


static void aot_emit_nested_procs(AotCompiler* aot, Stmt* body);

// (forward decl)
// (forward decl moved to top of section)
static void _collect_free_vars(Expr* e, const char** caps, int* ncaps, int maxcaps, ProcStmt* ps) {
    if (!e) return;
    if (e->type == EXPR_VARIABLE) {
        int is_param = 0;
        for(int i=0;i<ps->param_count;i++){
            if(e->as.variable.name.length==ps->params[i].length &&
               memcmp(e->as.variable.name.start,ps->params[i].start,ps->params[i].length)==0){
                is_param=1; break;
            }
        }
        if(!is_param && *ncaps < maxcaps){
            int nl=(int)e->as.variable.name.length;
            char* namecopy=(char*)malloc(nl+1);
            memcpy(namecopy,e->as.variable.name.start,nl); namecopy[nl]='\0';
            // Exclude language builtins — they're not captures
            static const char* _builtins[] = {
                "str","int","float","bool","len","print","println","typeof","type",
                "range","range_inc","dict_has","array_push","nil","true","false",
                "None","Some","Ok","Err","assert","min","max","abs","ord","chr",
                "contains","input","open","close","read","write","exit","gc_disable",
                "gc_enable","gc_collect","gc_collections","ffi_open","ffi_close","ffi_call",
                NULL
            };
            int is_builtin = 0;
            for(int _bi=0; _builtins[_bi]; _bi++)
                if(strcmp(namecopy,_builtins[_bi])==0){is_builtin=1;break;}
            int dup=0;
            if(!is_builtin){
                for(int i=0;i<*ncaps;i++){
                    if(strcmp(caps[i],namecopy)==0){ dup=1; break; }
                }
                if(!dup && *ncaps < maxcaps) caps[(*ncaps)++] = namecopy;
                else free(namecopy);
            } else free(namecopy);
        }
        return;
    }
    switch(e->type){
        case EXPR_BINARY:
            _collect_free_vars(e->as.binary.left,caps,ncaps,maxcaps,ps);
            _collect_free_vars(e->as.binary.right,caps,ncaps,maxcaps,ps); break;
        case EXPR_CALL:
            _collect_free_vars(e->as.call.callee,caps,ncaps,maxcaps,ps);
            for(int i=0;i<e->as.call.arg_count;i++)
                _collect_free_vars(e->as.call.args[i],caps,ncaps,maxcaps,ps);
            break;
        case EXPR_GET: _collect_free_vars(e->as.get.object,caps,ncaps,maxcaps,ps); break;
        case EXPR_SET:
            _collect_free_vars(e->as.set.object,caps,ncaps,maxcaps,ps);
            _collect_free_vars(e->as.set.value,caps,ncaps,maxcaps,ps); break;
        case EXPR_PROPAGATE:
        case EXPR_FORCE_UNWRAP:
            if (e->as.unwrap.operand) _collect_free_vars(e->as.unwrap.operand,caps,ncaps,maxcaps,ps);
            break;
        case EXPR_NULLCOAL:
            if (e->as.nullcoal.left) _collect_free_vars(e->as.nullcoal.left,caps,ncaps,maxcaps,ps);
            if (e->as.nullcoal.right) _collect_free_vars(e->as.nullcoal.right,caps,ncaps,maxcaps,ps);
            break;
        case EXPR_INDEX:
            if (e->as.index.array) _collect_free_vars(e->as.index.array,caps,ncaps,maxcaps,ps);
            if (e->as.index.index) _collect_free_vars(e->as.index.index,caps,ncaps,maxcaps,ps);
            break;
        default: break;
    }
}

static void _collect_free_vars_stmt(Stmt* s, const char** caps, int* ncaps, int maxcaps, ProcStmt* ps) {
    // First pass: collect all locally-defined var names (from let/var statements)
    // so we can exclude them from the free-variable (capture) set
    const char* locals[128]; int nlocals = 0;
    for(Stmt* _ls=s; _ls; _ls=_ls->next) {
        if(_ls->type==STMT_LET && _ls->as.let.name.start && nlocals<128) {
            int nl = _ls->as.let.name.length;
            char* lname = (char*)malloc(nl+1);
            memcpy(lname, _ls->as.let.name.start, nl); lname[nl]='\0';
            locals[nlocals++] = lname;
        }
    }
    for(;s;s=s->next){
        Expr* e=NULL;
        switch(s->type){
            case STMT_EXPRESSION: e=s->as.expression; break;
            case STMT_LET: e=s->as.let.initializer; break;
            case STMT_RETURN: e=s->as.ret.value; break;
            case STMT_IF:
                if (s->as.if_stmt.condition) _collect_free_vars(s->as.if_stmt.condition,caps,ncaps,maxcaps,ps);
                if (s->as.if_stmt.then_branch) _collect_free_vars_stmt(s->as.if_stmt.then_branch,caps,ncaps,maxcaps,ps);
                if (s->as.if_stmt.else_branch) _collect_free_vars_stmt(s->as.if_stmt.else_branch,caps,ncaps,maxcaps,ps);
                break;
            case STMT_WHILE:
                if (s->as.while_stmt.condition) _collect_free_vars(s->as.while_stmt.condition,caps,ncaps,maxcaps,ps);
                if (s->as.while_stmt.body) _collect_free_vars_stmt(s->as.while_stmt.body,caps,ncaps,maxcaps,ps);
                break;
            case STMT_BLOCK:
                if (s->as.block.statements) _collect_free_vars_stmt(s->as.block.statements,caps,ncaps,maxcaps,ps);
                break;
            case STMT_FOR:
                if (s->as.for_stmt.body) _collect_free_vars_stmt(s->as.for_stmt.body,caps,ncaps,maxcaps,ps);
                break;
            default: break;
        }
        if(e) _collect_free_vars(e,caps,ncaps,maxcaps,ps);
    }
    // Remove any captured vars that are locally defined in this scope
    // NOTE: do NOT free caps[ci] here — the caller (aot_emit_one_nested_proc) owns the array
    // Freeing here causes double-free because recursive calls share the same caps array
    for(int li=0; li<nlocals; li++) {
        for(int ci=0; ci<*ncaps; ci++) {
            if(strcmp(caps[ci], locals[li])==0) {
                // Shift remaining caps down (caps[ci] ownership transfers to caller, not freed here)
                for(int ri=ci; ri<(*ncaps)-1; ri++) caps[ri]=caps[ri+1];
                caps[(*ncaps)-1] = NULL;  // clear the dangling slot
                (*ncaps)--;
                ci--;
            }
        }
        free((char*)locals[li]);
    }
}

static void aot_emit_one_nested_proc(AotCompiler* aot, Stmt* s) {
    ProcStmt* ps = (s->type==STMT_PROC)?&s->as.proc:&s->as.async_proc;
    char* pname = aot_cname_tok(ps->name);
    char wname[128]; snprintf(wname, sizeof(wname), "_sw_%s", pname);
    // Emit all nested procs within this proc first (recursive)
    aot_emit_nested_procs(aot, ps->body);
    // Collect free variables (captures)
    const char* caps[64]; int ncaps=0;
    _collect_free_vars_stmt(ps->body, caps, &ncaps, 64, ps);
    // Emit capture struct type
    char sname[140]; snprintf(sname,sizeof(sname),"_cap_%s",pname);
    if(ncaps>0){
        aot_emit(aot,"typedef struct { SageValue fields[%d]; } %s;",ncaps,sname);
        // Emit capture field name comments for debugging
        for(int i=0;i<ncaps;i++){
            char capname[256]; int cl=(int)strnlen(caps[i],255);
            memcpy(capname,caps[i],cl); capname[cl]='\0';
            aot_emit(aot,"// cap[%d] = %s",i,capname);
        }
    }
    // Emit this proc as a file-scope SageNativeFn wrapper
    aot_emit(aot, "static SageValue %s(int _argc, SageValue* _argv, void* _env) {", wname);
    aot->indent++;
    // Bind params
    for(int i = 0; i < ps->param_count; i++) {
        char* pn = aot_cname_tok(ps->params[i]);
        aot_emit(aot, "SageValue %s = (_argc > %d) ? _argv[%d] : sage_rt_nil();", pn, i, i);
        free(pn);
    }
    // Bind captures: use direct struct field access (always non-null when ncaps>0)
    if(ncaps>0){
        aot_emit(aot,"%s* _caps = (%s*)_env;",sname,sname);
        for(int i=0;i<ncaps;i++){
            int cl=(int)strlen(caps[i]);
            char* cn=aot_cname(caps[i],cl);
            // #define maps the captured name directly to the struct field
            // This makes assignments write-through to the shared struct
            aot_emit(aot,"#define %s (_caps->fields[%d])",cn,i);
            free(cn);
        }
    }
    aot_infer_body(aot, ps->body);
    // Compile body — STMT_LET inside will #undef capture aliases when redefining same name
    // (handled in STMT_LET by checking for matching capture #define)
    int _saved_cb = aot->in_closure_body;
    aot->in_closure_body = (ncaps > 0) ? 1 : _saved_cb;
    for(Stmt* bs = ps->body; bs; bs = bs->next) aot_compile_stmt(aot, bs);
    aot->in_closure_body = _saved_cb;
    // No explicit writeback needed - #define makes assignments write directly to struct
    // Undefine the macros to avoid polluting global scope
    if(ncaps>0){
        for(int i=0;i<ncaps;i++){
            int cl=(int)strlen(caps[i]);
            char* cn=aot_cname(caps[i],cl);
            aot_emit(aot,"#undef %s",cn);
            free(cn);
        }
    }
    aot_emit(aot, "return sage_rt_nil();");
    aot->indent--; aot_emit(aot, "}");
    free(pname);
}

static void aot_emit_nested_procs(AotCompiler* aot, Stmt* body) {
    for (Stmt* s = body; s; s = s->next) {
        if (s->type == STMT_PROC || s->type == STMT_ASYNC_PROC)
            aot_emit_one_nested_proc(aot, s);
        else if (s->type == STMT_BLOCK)
            aot_emit_nested_procs(aot, s->as.block.statements);
        else if (s->type == STMT_IF) {
            aot_emit_nested_procs(aot, s->as.if_stmt.then_branch);
            if (s->as.if_stmt.else_branch) aot_emit_nested_procs(aot, s->as.if_stmt.else_branch);
        }
        else if (s->type == STMT_WHILE) aot_emit_nested_procs(aot, s->as.while_stmt.body);
        else if (s->type == STMT_FOR)   aot_emit_nested_procs(aot, s->as.for_stmt.body);
    }
}


// Check if a body contains any yield statements (generator detection)
static int _has_yield(Stmt* body) {
    for (Stmt* s = body; s; s = s->next) {
        if (s->type == STMT_YIELD) return 1;
        if (s->type == STMT_BLOCK && _has_yield(s->as.block.statements)) return 1;
        if (s->type == STMT_IF) {
            if (_has_yield(s->as.if_stmt.then_branch)) return 1;
            if (_has_yield(s->as.if_stmt.else_branch)) return 1;
        }
        if (s->type == STMT_WHILE && _has_yield(s->as.while_stmt.body)) return 1;
        if (s->type == STMT_FOR && _has_yield(s->as.for_stmt.body)) return 1;
    }
    return 0;
}

// Collect yield values from body (flat list, in order) — includes nested blocks/loops
static int _collect_yields(Stmt* body, Expr** yields, int max) {
    int count = 0;
    for (Stmt* s = body; s && count < max; s = s->next) {
        if (s->type == STMT_YIELD) {
            if (s->as.yield_stmt.value) yields[count++] = s->as.yield_stmt.value;
        } else if (s->type == STMT_BLOCK)
            count += _collect_yields(s->as.block.statements, yields+count, max-count);
        else if (s->type == STMT_WHILE)
            count += _collect_yields(s->as.while_stmt.body, yields+count, max-count);
        else if (s->type == STMT_FOR)
            count += _collect_yields(s->as.for_stmt.body, yields+count, max-count);
        else if (s->type == STMT_IF) {
            count += _collect_yields(s->as.if_stmt.then_branch, yields+count, max-count);
            if (s->as.if_stmt.else_branch)
                count += _collect_yields(s->as.if_stmt.else_branch, yields+count, max-count);
        }
    }
    return count;
}

// Check if all yields are at the top level (no yields inside loops)
static int _yields_are_sequential(Stmt* body) {
    for (Stmt* s = body; s; s = s->next) {
        if (s->type == STMT_WHILE || s->type == STMT_FOR) {
            // Any yield inside a loop = needs coroutine
            Stmt* lbody = (s->type==STMT_WHILE) ? s->as.while_stmt.body : s->as.for_stmt.body;
            if (_has_yield(lbody)) return 0;
        }
        if (s->type == STMT_IF) {
            if (_has_yield(s->as.if_stmt.then_branch)) return 0;
            if (_has_yield(s->as.if_stmt.else_branch)) return 0;
        }
        // Recurse into blocks — a block wrapping a loop with yield is also non-sequential
        if (s->type == STMT_BLOCK) {
            if (!_yields_are_sequential(s->as.block.statements)) return 0;
        }
    }
    return 1;
}










static void aot_emit_proc(AotCompiler* aot, Stmt* s) {
    ProcStmt* ps = (s->type==STMT_PROC)?&s->as.proc:&s->as.async_proc;
    char* base_fname = aot_cname_tok(ps->name);
    char* fname;
    // When compiling a module, prefix all proc names with the module prefix
    if (aot->current_module_prefix[0]) {
        fname = malloc(strlen(aot->current_module_prefix) + strlen(base_fname) + 1);
        sprintf(fname, "%s%s", aot->current_module_prefix, base_fname);
        free(base_fname);
    } else {
        fname = base_fname;
    }

    // Emit all nested procs (closures) BEFORE this proc so they're declared
    aot_emit_nested_procs(aot, ps->body);

    // Save type env state to restore after proc (proc params are local)
    int saved_type_count = aot->type_env.count;
    // Set param types (override any existing entry to avoid cross-proc pollution)
    // Generic procs (with type params like [T]) always use SageValue params
    int is_generic = (ps->type_param_count > 0);
    for (int i = 0; i < ps->param_count; i++) {
        JitTypeTag pt = is_generic ? JIT_TYPE_UNKNOWN : aot_param_type(aot, ps->name.start, ps->name.length, i);
        if (pt != JIT_TYPE_UNKNOWN) {
            char pname[256];
            int len = ps->params[i].length<255?ps->params[i].length:255;
            memcpy(pname, ps->params[i].start, len); pname[len]='\0';
            // Find and update existing entry, or add new
            int found_param = 0;
            for (int j = 0; j < aot->type_env.count; j++) {
                if (strcmp(aot->type_env.vars[j].name, pname) == 0) {
                    aot->type_env.vars[j].inferred_type = pt;
                    found_param = 1; break;
                }
            }
            if (!found_param) aot_set_var_type(aot, pname, pt);
        }
    }
    aot_infer_body(aot, ps->body);


    // Check if generator — emit state machine or coroutine
    if (ps->body && _has_yield(ps->body)) {
        if (!_yields_are_sequential(ps->body)) {
            // Complex generator (yields inside loops): use ucontext coroutine
            char gname[128]; snprintf(gname, sizeof(gname), "_gen_%s", fname);
            int np = ps->param_count;
            // Coroutine body function
            aot_emit(aot, "static void %s_body(SageCoroutine* _co) {", gname);
            aot->indent++;
            for (int pi = 0; pi < np; pi++) {
                char* pn = aot_cname_tok(ps->params[pi]);
                aot_emit(aot, "SageValue %s = _co->argv[%d];", pn, pi); free(pn);
            }
            aot_infer_body(aot, ps->body);
            // Set coroutine context so STMT_YIELD → sage_rt_coro_yield(_co, val)
            aot->in_coro_body = 1;
            strcpy(aot->coro_var, "_co");
            // Temporarily suppress type specialization — all vars are SageValue inside
            // the coroutine body because params come from _co->argv (always boxed)
            int coro_saved_count = aot->type_env.count;
            aot->type_env.count = 0;
            int _saved_in_proc2 = aot->in_proc_body;
            aot->in_proc_body = 1;
            for (Stmt* bs = ps->body; bs; bs = bs->next) aot_compile_stmt(aot, bs);
            aot->in_proc_body = _saved_in_proc2;
            aot->in_coro_body = 0;
            aot->type_env.count = coro_saved_count;
            aot_emit(aot, "_co->done = 1;");
            aot->indent--; aot_emit(aot, "}");
            // Constructor
            aot_emit(aot, "static SageValue %s(", fname);
            aot->indent++;
            for (int pi = 0; pi < np; pi++) {
                char* pn = aot_cname_tok(ps->params[pi]);
                aot_emit(aot, "SageValue %s%s", pn, pi<np-1?",":""); free(pn);
            }
            if (np==0) aot_emit(aot,"void");
            aot->indent--; aot_emit(aot, ") {"); aot->indent++;
            if (np > 0) {
                aot_emit(aot, "SageValue* _argv = (SageValue*)malloc(%d*sizeof(SageValue));", np);
                for (int pi = 0; pi < np; pi++) {
                    char* pn = aot_cname_tok(ps->params[pi]);
                    aot_emit(aot, "_argv[%d] = %s;", pi, pn); free(pn);
                }
                aot_emit(aot, "SageCoroutine* _co = sage_rt_coro_new((SageCoroutineBody)%s_body,%d,_argv);",gname,np);
            } else {
                aot_emit(aot,"SageCoroutine* _co = sage_rt_coro_new((SageCoroutineBody)%s_body,0,NULL);",gname);
            }
            aot_emit(aot, "return sage_rt_make_fn((SageNativeFn)_coro_next_fn, _co, \"%s\");", fname);
            aot->indent--; aot_emit(aot, "}");
            aot_blank(aot);
            aot->type_env.count = saved_type_count;
            free(fname); return;
        }
        // Simple sequential generator: use state machine
        {
        char gname[128]; snprintf(gname, sizeof(gname), "_gen_%s", fname);
        Expr* yields[64]; int nyields = _collect_yields(ps->body, yields, 64);
        int np = ps->param_count;  // number of params (incl. self if any)
        // State struct: state index + params as SageValue fields
        aot_emit(aot, "typedef struct { int _state; SageValue params[%d]; } %s;", np > 0 ? np : 1, gname);
        // Next function
        aot_emit(aot, "static SageValue %s_next(int _argc, SageValue* _argv, void* _env) {", gname);
        aot->indent++;
        aot_emit(aot, "%s* _g = (%s*)_env;", gname, gname);
        aot_emit(aot, "if (!_g) return sage_rt_nil();");
        // Bind params from struct
        for (int pi = 0; pi < np; pi++) {
            char* pn = aot_cname_tok(ps->params[pi]);
            aot_emit(aot, "SageValue %s = _g->params[%d];", pn, pi);
            free(pn);
        }
        // Also set up local vars that were LETs in the body
        aot_infer_body(aot, ps->body);
        // Simple generator: sequential yields
        aot_emit(aot, "switch (_g->_state) {"); aot->indent++;
        for (int yi = 0; yi < nyields; yi++) {
            char* yv = aot_expr(aot, yields[yi], JIT_TYPE_UNKNOWN);
            JitTypeTag yt = aot_infer_expr(aot, yields[yi]);
            int ynb = (yields[yi]->type == EXPR_VARIABLE && jit_is_unboxed(yt));
            char* yvb = ynb ? aot_box(yt, yv) : yv;
            aot_emit(aot, "case %d: _g->_state = %d; return %s;", yi, yi+1, yvb);
            if (yvb != yv) free(yvb); free(yv);
        }
        aot_emit(aot, "default: return sage_rt_nil();");
        aot->indent--; aot_emit(aot, "}");
        aot_emit(aot, "return sage_rt_nil();");
        aot->indent--; aot_emit(aot, "}");
        // Constructor
        aot_emit(aot, "static SageValue %s(", fname);
        aot->indent++;
        for (int pi = 0; pi < np; pi++) {
            char* pn = aot_cname_tok(ps->params[pi]);
            aot_emit(aot, "SageValue %s%s", pn, pi < np-1 ? "," : "");
            free(pn);
        }
        if (np == 0) aot_emit(aot, "void");
        aot->indent--;
        aot_emit(aot, ") {"); aot->indent++;
        aot_emit(aot, "%s* _gs = (%s*)malloc(sizeof(%s));", gname, gname, gname);
        aot_emit(aot, "_gs->_state = 0;");
        for (int pi = 0; pi < np; pi++) {
            char* pn = aot_cname_tok(ps->params[pi]);
            aot_emit(aot, "_gs->params[%d] = %s;", pi, pn);
            free(pn);
        }
        aot_emit(aot, "return sage_rt_make_fn((SageNativeFn)%s_next, _gs, \"%s\");", gname, fname);
        aot->indent--; aot_emit(aot, "}");
        aot_blank(aot);
        aot->type_env.count = saved_type_count;
        free(fname); return;
    }
    }  // end if (_has_yield)

    // Regular proc — determine return type
    // For now emit SageValue return — future pass can specialise this
    aot_emit(aot, "static SageValue %s(", fname);
    aot->indent++;
    for (int i = 0; i < ps->param_count; i++) {
        JitTypeTag pt = is_generic ? JIT_TYPE_UNKNOWN : aot_param_type(aot, ps->name.start, ps->name.length, i);
        char* pn = aot_cname_tok(ps->params[i]);
        if (jit_is_unboxed(pt)) {
            aot_emit(aot, "%s %s%s", jit_ctype(pt), pn, i<ps->param_count-1?",":"");
        } else {
            aot_emit(aot, "SageValue %s%s", pn, i<ps->param_count-1?",":"");
        }
        free(pn);
    }
    aot->indent--;
    // (generator handled above)
    aot_emit(aot, ") {"); aot->indent++;
    int _saved_defer = aot->defer_count;
    aot->defer_count = 0;  // Reset defer stack for this function
    int _saved_in_proc = aot->in_proc_body;
    aot->in_proc_body = 1;
    // Recursion-depth guard — only for procs that can reach themselves. The
    // cleanup handler decrements on every normal exit; try frames restore the
    // counter on unwind. The post-increment read also defeats tail-call
    // optimisation of self-recursion (so infinite recursion is caught instead
    // of spinning forever).
    if (!aot->current_module_prefix[0] && aot_is_recursive_proc(aot, ps->name)) {
        aot_emit(aot, "int _sage_rec __attribute__((cleanup(sage_rt_depth_pop))) = ++sage_rt_call_depth;");
        aot_emit(aot, "if (_sage_rec > SAGE_RT_MAX_DEPTH) sage_rt_recursion_error();");
    }
    for(Stmt* bs = ps->body; bs; bs = bs->next) aot_compile_stmt(aot, bs);
    aot->in_proc_body = _saved_in_proc;
    // Emit pending defers at function end (LIFO)
    for(int _di=aot->defer_count-1;_di>=0;_di--){
        aot_emit(aot,"{ /* defer */"); aot->indent++;
        for(Stmt*_ds=aot->defer_stack[_di]->as.defer.statement;_ds;_ds=_ds->next)
            aot_compile_stmt(aot,_ds);
        aot->indent--; aot_emit(aot,"}");
    }
    aot->defer_count = _saved_defer;
    aot_emit(aot, "return sage_rt_nil();");
    aot->indent--;
    aot_emit(aot, "}"); aot_blank(aot); free(fname);
    // Restore type env to pre-proc state (proc-local types don't persist)
    aot->type_env.count = saved_type_count;
}



// Returns 1 if var_name appears as an EXPR_VARIABLE anywhere in the expr tree
static int _expr_refs_var(Expr* e, const char* var, int vlen) {
    if (!e) return 0;
    if (e->type == EXPR_VARIABLE) {
        if ((int)e->as.variable.name.length == vlen &&
            memcmp(e->as.variable.name.start, var, vlen) == 0) return 1;
    }
    switch (e->type) {
        case EXPR_BINARY: return _expr_refs_var(e->as.binary.left,var,vlen) || _expr_refs_var(e->as.binary.right,var,vlen);
        case EXPR_CALL: {
            if (_expr_refs_var(e->as.call.callee,var,vlen)) return 1;
            for (int i=0;i<e->as.call.arg_count;i++) if(_expr_refs_var(e->as.call.args[i],var,vlen)) return 1;
            return 0;
        }
        case EXPR_GET: return _expr_refs_var(e->as.get.object,var,vlen);
        case EXPR_SET:
            // Bare-variable assignment `x = v` is parsed as SET with object==NULL
            // and the target name in `property`. Count that as a use of `x`.
            if (e->as.set.object == NULL &&
                (int)e->as.set.property.length == vlen &&
                memcmp(e->as.set.property.start, var, vlen) == 0) return 1;
            return _expr_refs_var(e->as.set.object,var,vlen) || _expr_refs_var(e->as.set.value,var,vlen);
        case EXPR_INDEX: return _expr_refs_var(e->as.index.array,var,vlen) || _expr_refs_var(e->as.index.index,var,vlen);
        case EXPR_INDEX_SET: return _expr_refs_var(e->as.index_set.array,var,vlen) ||
                                    _expr_refs_var(e->as.index_set.index,var,vlen) ||
                                    _expr_refs_var(e->as.index_set.value,var,vlen);
        case EXPR_SLICE: return _expr_refs_var(e->as.slice.array,var,vlen) ||
                                _expr_refs_var(e->as.slice.start,var,vlen) ||
                                _expr_refs_var(e->as.slice.end,var,vlen);
        case EXPR_ARRAY: { for (int i=0;i<e->as.array.count;i++) if(_expr_refs_var(e->as.array.elements[i],var,vlen)) return 1; return 0; }
        case EXPR_TUPLE: { for (int i=0;i<e->as.tuple.count;i++) if(_expr_refs_var(e->as.tuple.elements[i],var,vlen)) return 1; return 0; }
        case EXPR_DICT:  { for (int i=0;i<e->as.dict.count;i++) if(_expr_refs_var(e->as.dict.values[i],var,vlen)) return 1; return 0; }
        case EXPR_RANGE: return _expr_refs_var(e->as.range.low,var,vlen) || _expr_refs_var(e->as.range.high,var,vlen);
        case EXPR_AWAIT: return _expr_refs_var(e->as.await.expression,var,vlen);
        case EXPR_FORCE_UNWRAP:
        case EXPR_PROPAGATE: return _expr_refs_var(e->as.unwrap.operand,var,vlen);
        case EXPR_NULLCOAL: return _expr_refs_var(e->as.nullcoal.left,var,vlen) || _expr_refs_var(e->as.nullcoal.right,var,vlen);
        case EXPR_OPTCHAIN: return _expr_refs_var(e->as.optchain.object,var,vlen);
        default: return 0;
    }
}
static int _stmt_refs_var(Stmt* s, const char* var, int vlen) {
    for (; s; s = s->next) {
        switch (s->type) {
            case STMT_EXPRESSION: if(_expr_refs_var(s->as.expression,var,vlen)) return 1; break;
            case STMT_LET: if(_expr_refs_var(s->as.let.initializer,var,vlen)) return 1; break;
            case STMT_RETURN: if(s->as.ret.value && _expr_refs_var(s->as.ret.value,var,vlen)) return 1; break;
            case STMT_IF: if(_expr_refs_var(s->as.if_stmt.condition,var,vlen)||_stmt_refs_var(s->as.if_stmt.then_branch,var,vlen)||_stmt_refs_var(s->as.if_stmt.else_branch,var,vlen)) return 1; break;
            case STMT_WHILE: if(_expr_refs_var(s->as.while_stmt.condition,var,vlen)||_stmt_refs_var(s->as.while_stmt.body,var,vlen)) return 1; break;
            case STMT_FOR: if(_expr_refs_var(s->as.for_stmt.iterable,var,vlen)||_stmt_refs_var(s->as.for_stmt.body,var,vlen)) return 1; break;
            case STMT_BLOCK: if(_stmt_refs_var(s->as.block.statements,var,vlen)) return 1; break;
            case STMT_PRINT: if(_expr_refs_var(s->as.print.expression,var,vlen)) return 1; break;
            case STMT_RAISE: if(_expr_refs_var(s->as.raise.exception,var,vlen)) return 1; break;
            case STMT_DEFER: if(_stmt_refs_var(s->as.defer.statement,var,vlen)) return 1; break;
            case STMT_MATCH:
                if(_expr_refs_var(s->as.match_stmt.value,var,vlen)) return 1;
                for(int i=0;i<s->as.match_stmt.case_count;i++)
                    if(s->as.match_stmt.cases && s->as.match_stmt.cases[i] && _stmt_refs_var(s->as.match_stmt.cases[i]->body,var,vlen)) return 1;
                if(_stmt_refs_var(s->as.match_stmt.default_case,var,vlen)) return 1;
                break;
            case STMT_TRY:
                if(_stmt_refs_var(s->as.try_stmt.try_block,var,vlen)) return 1;
                if(s->as.try_stmt.catches)
                    for(int i=0;i<s->as.try_stmt.catch_count;i++)
                        if(s->as.try_stmt.catches[i] && _stmt_refs_var(s->as.try_stmt.catches[i]->body,var,vlen)) return 1;
                if(_stmt_refs_var(s->as.try_stmt.finally_block,var,vlen)) return 1;
                break;
            default: break;
        }
    }
    return 0;
}
// Returns 1 if var_name appears in any class method body or top-level proc body
static int _var_used_before_main(Stmt* program, const char* var, int vlen) {
    for (Stmt* s = program; s; s = s->next) {
        if (s->type == STMT_CLASS) {
            // Scan all method bodies (methods is a linked list of STMT_PROC)
            for (Stmt* m = s->as.class_stmt.methods; m; m = m->next) {
                if (m->type == STMT_PROC || m->type == STMT_ASYNC_PROC) {
                    ProcStmt* ps = (m->type==STMT_PROC)?&m->as.proc:&m->as.async_proc;
                    if (_stmt_refs_var(ps->body, var, vlen)) return 1;
                }
            }
        }
        if (s->type == STMT_PROC || s->type == STMT_ASYNC_PROC) {
            ProcStmt* ps = (s->type==STMT_PROC)?&s->as.proc:&s->as.async_proc;
            if (_stmt_refs_var(ps->body, var, vlen)) return 1;
        }
    }
    return 0;
}

// ── Recursion-cycle detection ─────────────────────────────────────────────
// Returns 1 if expr `e` contains a direct call whose callee is the bare name
// `name` (length `len`). Walks the common nesting positions; missing an exotic
// position only risks under-detection (a genuinely recursive proc left
// unguarded), never mis-compilation.
static int _expr_calls_name(Expr* e, const char* name, int len) {
    if (!e) return 0;
    switch (e->type) {
        case EXPR_CALL: {
            Expr* c = e->as.call.callee;
            if (c && c->type == EXPR_VARIABLE &&
                (int)c->as.variable.name.length == len &&
                memcmp(c->as.variable.name.start, name, len) == 0) return 1;
            if (_expr_calls_name(e->as.call.callee, name, len)) return 1;
            for (int i = 0; i < e->as.call.arg_count; i++)
                if (_expr_calls_name(e->as.call.args[i], name, len)) return 1;
            return 0;
        }
        case EXPR_BINARY:
            return _expr_calls_name(e->as.binary.left, name, len) ||
                   _expr_calls_name(e->as.binary.right, name, len);
        case EXPR_GET:   return _expr_calls_name(e->as.get.object, name, len);
        case EXPR_SET:   return _expr_calls_name(e->as.set.object, name, len) ||
                                _expr_calls_name(e->as.set.value, name, len);
        case EXPR_INDEX: return _expr_calls_name(e->as.index.array, name, len) ||
                                _expr_calls_name(e->as.index.index, name, len);
        case EXPR_INDEX_SET:
            return _expr_calls_name(e->as.index_set.array, name, len) ||
                   _expr_calls_name(e->as.index_set.index, name, len) ||
                   _expr_calls_name(e->as.index_set.value, name, len);
        case EXPR_SLICE: return _expr_calls_name(e->as.slice.array, name, len) ||
                                _expr_calls_name(e->as.slice.start, name, len) ||
                                _expr_calls_name(e->as.slice.end, name, len);
        case EXPR_ARRAY: {
            for (int i = 0; i < e->as.array.count; i++)
                if (_expr_calls_name(e->as.array.elements[i], name, len)) return 1;
            return 0;
        }
        case EXPR_TUPLE: {
            for (int i = 0; i < e->as.tuple.count; i++)
                if (_expr_calls_name(e->as.tuple.elements[i], name, len)) return 1;
            return 0;
        }
        case EXPR_DICT: {
            for (int i = 0; i < e->as.dict.count; i++)
                if (_expr_calls_name(e->as.dict.values[i], name, len)) return 1;
            return 0;
        }
        case EXPR_RANGE: return _expr_calls_name(e->as.range.low, name, len) ||
                                _expr_calls_name(e->as.range.high, name, len);
        case EXPR_AWAIT: return _expr_calls_name(e->as.await.expression, name, len);
        case EXPR_FORCE_UNWRAP:
        case EXPR_PROPAGATE: return _expr_calls_name(e->as.unwrap.operand, name, len);
        case EXPR_NULLCOAL: return _expr_calls_name(e->as.nullcoal.left, name, len) ||
                                   _expr_calls_name(e->as.nullcoal.right, name, len);
        case EXPR_OPTCHAIN: return _expr_calls_name(e->as.optchain.object, name, len);
        default: return 0;
    }
}
static int _stmt_calls_name(Stmt* s, const char* name, int len) {
    for (; s; s = s->next) {
        switch (s->type) {
            case STMT_EXPRESSION: if (_expr_calls_name(s->as.expression, name, len)) return 1; break;
            case STMT_LET:        if (_expr_calls_name(s->as.let.initializer, name, len)) return 1; break;
            case STMT_RETURN:     if (s->as.ret.value && _expr_calls_name(s->as.ret.value, name, len)) return 1; break;
            case STMT_PRINT:      if (_expr_calls_name(s->as.print.expression, name, len)) return 1; break;
            case STMT_RAISE:      if (_expr_calls_name(s->as.raise.exception, name, len)) return 1; break;
            case STMT_IF:
                if (_expr_calls_name(s->as.if_stmt.condition, name, len)) return 1;
                if (_stmt_calls_name(s->as.if_stmt.then_branch, name, len)) return 1;
                if (_stmt_calls_name(s->as.if_stmt.else_branch, name, len)) return 1;
                break;
            case STMT_WHILE:
                if (_expr_calls_name(s->as.while_stmt.condition, name, len)) return 1;
                if (_stmt_calls_name(s->as.while_stmt.body, name, len)) return 1;
                break;
            case STMT_FOR:
                if (_expr_calls_name(s->as.for_stmt.iterable, name, len)) return 1;
                if (_stmt_calls_name(s->as.for_stmt.body, name, len)) return 1;
                break;
            case STMT_BLOCK:      if (_stmt_calls_name(s->as.block.statements, name, len)) return 1; break;
            case STMT_DEFER:      if (_stmt_calls_name(s->as.defer.statement, name, len)) return 1; break;
            case STMT_MATCH:
                if (_expr_calls_name(s->as.match_stmt.value, name, len)) return 1;
                for (int i = 0; i < s->as.match_stmt.case_count; i++)
                    if (s->as.match_stmt.cases && s->as.match_stmt.cases[i] &&
                        _stmt_calls_name(s->as.match_stmt.cases[i]->body, name, len)) return 1;
                if (_stmt_calls_name(s->as.match_stmt.default_case, name, len)) return 1;
                break;
            case STMT_TRY:
                if (_stmt_calls_name(s->as.try_stmt.try_block, name, len)) return 1;
                if (s->as.try_stmt.catches)
                    for (int i = 0; i < s->as.try_stmt.catch_count; i++)
                        if (s->as.try_stmt.catches[i] &&
                            _stmt_calls_name(s->as.try_stmt.catches[i]->body, name, len)) return 1;
                if (_stmt_calls_name(s->as.try_stmt.finally_block, name, len)) return 1;
                break;
            default: break;
        }
    }
    return 0;
}
// Build the call graph over top-level procs and mark every proc that can reach
// itself (direct self-recursion or a mutual-recursion cycle). Only those procs
// get a runtime depth guard, so leaf/non-recursive calls stay overhead-free.
static void aot_detect_recursion(AotCompiler* aot, Stmt* program) {
    enum { MAXP = 256 };
    char names[MAXP][64];
    Stmt* pstmt[MAXP];
    int np = 0;
    for (Stmt* s = program; s && np < MAXP; s = s->next) {
        if (s->type == STMT_PROC || s->type == STMT_ASYNC_PROC) {
            ProcStmt* ps = (s->type == STMT_PROC) ? &s->as.proc : &s->as.async_proc;
            int len = ps->name.length < 63 ? ps->name.length : 63;
            memcpy(names[np], ps->name.start, len); names[np][len] = '\0';
            pstmt[np] = s;
            np++;
        }
    }
    if (np == 0) return;
    unsigned char* reach = (unsigned char*)calloc((size_t)np * np, 1);
    if (!reach) return;
    for (int i = 0; i < np; i++) {
        ProcStmt* ps = (pstmt[i]->type == STMT_PROC) ? &pstmt[i]->as.proc : &pstmt[i]->as.async_proc;
        for (int j = 0; j < np; j++)
            if (_stmt_calls_name(ps->body, names[j], (int)strlen(names[j])))
                reach[i * np + j] = 1;
    }
    // Transitive closure (Floyd–Warshall over the reachability relation).
    for (int k = 0; k < np; k++)
        for (int i = 0; i < np; i++)
            if (reach[i * np + k])
                for (int j = 0; j < np; j++)
                    if (reach[k * np + j]) reach[i * np + j] = 1;
    for (int i = 0; i < np; i++)
        if (reach[i * np + i] && aot->recursive_proc_count < 256)
            snprintf(aot->recursive_procs[aot->recursive_proc_count++], 64, "%s", names[i]);
    free(reach);
}
static int aot_is_recursive_proc(AotCompiler* aot, Token name) {
    int len = name.length;
    for (int i = 0; i < aot->recursive_proc_count; i++)
        if ((int)strlen(aot->recursive_procs[i]) == len &&
            memcmp(aot->recursive_procs[i], name.start, len) == 0) return 1;
    return 0;
}

char* aot_compile_program(AotCompiler* aot, Stmt* program) {
    // Treat macro definitions as plain procs (matches interpreter semantics).
    // MacroDefStmt and ProcStmt share name/params/param_count/body, so we read
    // the macro fields into locals first, then overwrite the union as a proc.
    for (Stmt* s = program; s; s = s->next) {
        if (s->type == STMT_MACRO_DEF) {
            Token  m_name   = s->as.macro_def.name;
            Token* m_params = s->as.macro_def.params;
            int    m_pc     = s->as.macro_def.param_count;
            Stmt*  m_body   = s->as.macro_def.body;
            s->type = STMT_PROC;
            ProcStmt* p = &s->as.proc;
            p->name = m_name;
            p->params = m_params;
            p->param_types = NULL;
            p->defaults = NULL;
            p->param_count = m_pc;
            p->required_count = m_pc;
            p->return_type = NULL;
            p->doc = NULL;
            p->type_params = NULL;
            p->type_param_count = 0;
            p->body = m_body;
        }
    }
    aot_infer_types(aot,program);
    aot_detect_recursion(aot, program);
    // Record doc comments for compile-time doc() resolution.
    for (Stmt* s = program; s; s = s->next) {
        if ((s->type == STMT_PROC || s->type == STMT_ASYNC_PROC) && aot->proc_doc_count < 256) {
            ProcStmt* ps = (s->type==STMT_PROC)?&s->as.proc:&s->as.async_proc;
            int len = ps->name.length < 63 ? ps->name.length : 63;
            memcpy(aot->proc_docs[aot->proc_doc_count].name, ps->name.start, len);
            aot->proc_docs[aot->proc_doc_count].name[len] = '\0';
            aot->proc_docs[aot->proc_doc_count].doc = ps->doc;  // may be NULL
            aot->proc_doc_count++;
        }
    }
    aot_emit(aot,"/* Auto-generated by sage --aot */");
    aot_emit(aot,"#define _POSIX_C_SOURCE 200809L");
    aot_emit(aot,"#define _GNU_SOURCE");
    aot_emit(aot,"#include <stdint.h>");
    aot_emit(aot,"#include <stdio.h>");
    aot_emit(aot,"#include <stdlib.h>");
    aot_emit(aot,"#include <string.h>");
    aot_emit(aot,"#include <math.h>");
    aot_emit(aot,"#include <setjmp.h>");
    aot_emit(aot,"#include \"sage_runtime.h\"");
    aot_blank(aot);
    // Coroutine next-function stub — used by complex generators (yields inside loops)
    aot_emit(aot,"static SageValue _coro_next_fn(int _argc, SageValue* _argv, void* _env) {");
    aot_emit(aot,"    (void)_argc; (void)_argv;");
    aot_emit(aot,"    return sage_rt_coro_next((SageCoroutine*)_env);");
    aot_emit(aot,"}");
    aot_blank(aot);
    // ── Call-site type analysis first ──────────────────────────────────────
    for (Stmt* s = program; s; s = s->next) {
        if (s->type == STMT_PROC || s->type == STMT_ASYNC_PROC) {
            ProcStmt* ps = (s->type==STMT_PROC)?&s->as.proc:&s->as.async_proc;
            // Generic procs have dynamic (SageValue) params regardless of call-site
            // arg types — collecting per-call types here would wrongly specialize
            // them (e.g. identity(10) typing param 0 as INT, breaking identity("x")).
            if (ps->type_param_count > 0) continue;
            aot_collect_calls(aot, ps->name.start, ps->name.length, program);
        }
        // Also collect call-site info for class constructors
        if (s->type == STMT_CLASS) {
            aot_collect_calls(aot, s->as.class_stmt.name.start, s->as.class_stmt.name.length, program);
        }
        if (s->type == STMT_STRUCT) {
            aot_collect_calls(aot, s->as.struct_stmt.name.start, s->as.struct_stmt.name.length, program);
        }
    }

    // ── Forward-declare procs with typed signatures ──────────────────────────
    for (Stmt* s = program; s; s = s->next) {
        if (s->type == STMT_PROC || s->type == STMT_ASYNC_PROC) {
            ProcStmt* ps = (s->type==STMT_PROC)?&s->as.proc:&s->as.async_proc;
            // Skip generators — they emit their own constructor in aot_emit_proc
            if (ps->body && _has_yield(ps->body)) continue;
            char* name = aot_cname_tok(ps->name);
            aot_emit(aot, "static SageValue %s(", name);
            aot->indent++;
            int fwd_is_generic = (ps->type_param_count > 0);
            for (int i = 0; i < ps->param_count; i++) {
                JitTypeTag pt = fwd_is_generic ? JIT_TYPE_UNKNOWN : aot_param_type(aot, ps->name.start, ps->name.length, i);
                aot_emit(aot, "%s%s",
                    jit_is_unboxed(pt) ? jit_ctype(pt) : "SageValue",
                    i < ps->param_count-1 ? "," : "");
            }
            aot->indent--;
            aot_emit(aot, ");");
            free(name);
        }
    }
    aot_blank(aot);
    // Stub declarations for interpreter-only threading/asm modules
    // Only emit if the module can't be found (prevents conflicts with actual module imports)
    {
        // Modules that should always use stubs regardless of whether a .sage file exists.
        // Reason: the .sage file uses reserved keywords as proc names (e.g. thread.sage has
        // "proc spawn" but "spawn" is TOKEN_SPAWN), or are pure native-only modules.
        const char* _force_stub[] = {
            "thread","mutex","atomic","channel","sys","gc","python","ffi",
            "semaphore","rwlock","condvar","signal","socket","io",NULL
        };
        const char* _stub_names[] = {
            "thread","mutex","semaphore","rwlock","condvar","signal","asm","sys",
            "python","addressof","channel","socket","io","atomic","gc",NULL
        };
        for (int _si=0; _stub_names[_si]; _si++) {
            // Check if this is a force-stubbed module (ignore real .sage file)
            int _force = 0;
            for (int _fi=0; _force_stub[_fi]; _fi++)
                if (strcmp(_stub_names[_si], _force_stub[_fi])==0) { _force=1; break; }
            // Check if there's an actual importable module with this name
            char* _mod_path = (!_force && global_module_cache) ? resolve_module_path(global_module_cache, _stub_names[_si]) : NULL;
            if (!_mod_path) {
                // No real module — emit stub
                char _sv[32]; snprintf(_sv, sizeof(_sv), "sg_%s", _stub_names[_si]);
                aot_emit(aot,"static SageValue %s; /* interpreter-only stub */",_sv);
                aot_set_var_type(aot, _stub_names[_si], JIT_TYPE_DICT);
            } else {
                free(_mod_path);
            }
        }
    }
    // Pre-emit stubs for force-skipped native modules (thread, channel, atomic, gc)
    // These modules have .sage files but they use reserved keywords — we skip loading them
    // and emit stubs directly here before any import processing.
    {
        static const struct { const char* mod; const char* fn; int na; } _nat_stubs[] = {
            // thread module (spawn emitted separately as variadic)
            {"thread","join",1},
            {"thread","sleep",1},{"thread","yield",0},{"thread","mutex",0},
            {"thread","lock",1},{"thread","unlock",1},{"thread","try_lock",1},
            // channel module (skip channel.sage which also uses reserved words)
            {"channel","new",0},{"channel","send",2},{"channel","recv",1},
            {"channel","try_recv",1},{"channel","close",1},{"channel","is_closed",1},
            {"channel","len",1},{"channel","select",1},
            // atomic module
            {"atomic","new",1},{"atomic","load",1},{"atomic","store",2},
            {"atomic","add",2},{"atomic","sub",2},{"atomic","cas",3},{"atomic","exchange",2},
            // mutex module
            {"mutex","new",0},{"mutex","lock",1},{"mutex","unlock",1},{"mutex","try_lock",1},
            {NULL,NULL,0}
        };
        // Register native module vars as dict stubs
        static const char* _nat_mods[] = {"thread","channel","atomic","mutex",NULL};
        for(int _mi=0; _nat_mods[_mi]; _mi++){
            char _sv[32]; snprintf(_sv,sizeof(_sv),"sg_%s",_nat_mods[_mi]);
            aot_emit(aot,"static SageValue %s;",_sv);
            aot_set_var_type(aot,_nat_mods[_mi],JIT_TYPE_DICT);
        }
        // Emit stub functions. atomic/channel map to real runtime functions;
        // thread/mutex remain nil stubs (no real threading in AOT — async runs
        // synchronously, so these are not exercised concurrently).
        for(int _si=0; _nat_stubs[_si].mod; _si++){
            char sfn[128]; snprintf(sfn,sizeof(sfn),"sg_%s_sg_%s",_nat_stubs[_si].mod,_nat_stubs[_si].fn);
            int na=_nat_stubs[_si].na;
            const char* body = NULL;  // when set, the C body that returns a value
            const char* m=_nat_stubs[_si].mod; const char* f=_nat_stubs[_si].fn;
            if(!strcmp(m,"atomic")){
                if(!strcmp(f,"new"))      body="return sage_rt_atomic_new(_a);";
                else if(!strcmp(f,"load"))body="return sage_rt_atomic_load(_a);";
                else if(!strcmp(f,"store"))body="return sage_rt_atomic_store(_a,_b);";
                else if(!strcmp(f,"add")) body="return sage_rt_atomic_add(_a,_b);";
                else if(!strcmp(f,"sub")) body="return sage_rt_atomic_sub(_a,_b);";
                else if(!strcmp(f,"cas")) body="return sage_rt_atomic_cas(_a,_b,_c);";
                else if(!strcmp(f,"exchange"))body="return sage_rt_atomic_exchange(_a,_b);";
            } else if(!strcmp(m,"channel")){
                if(!strcmp(f,"new"))      body="return sage_rt_channel_new();";
                else if(!strcmp(f,"send"))body="return sage_rt_channel_send(_a,_b);";
                else if(!strcmp(f,"recv"))body="return sage_rt_channel_recv(_a);";
                else if(!strcmp(f,"try_recv"))body="return sage_rt_channel_try_recv(_a);";
                else if(!strcmp(f,"close"))body="return sage_rt_channel_close(_a);";
                else if(!strcmp(f,"is_closed"))body="return sage_rt_channel_is_closed(_a);";
                else if(!strcmp(f,"len")) body="return sage_rt_channel_len(_a);";
            } else if(!strcmp(m,"thread")){
                // Synchronous model: spawn runs the proc immediately and join
                // returns the already-computed result unchanged.
                if(!strcmp(f,"join")) body="return _a;";
            }
            if(!body) body = "return sage_rt_nil();";
            if(na==0) aot_emit(aot,"static SageValue %s(void){%s}",sfn,body);
            else if(na==1) aot_emit(aot,"static SageValue %s(SageValue _a){(void)_a;%s}",sfn,body);
            else if(na==2) aot_emit(aot,"static SageValue %s(SageValue _a,SageValue _b){(void)_a;(void)_b;%s}",sfn,body);
            else aot_emit(aot,"static SageValue %s(SageValue _a,SageValue _b,SageValue _c){(void)_a;(void)_b;(void)_c;%s}",sfn,body);
            aot_register_proc(aot,sfn);
        }
        // thread.id() needs to return a positive integer (like a thread id)
        aot_emit(aot,"static SageValue sg_thread_sg_id(void){return sage_rt_int(1);}");
        aot_register_proc(aot,"sg_thread_sg_id");
        // thread.spawn(fn, ...args): synchronous — call fn now, return its result
        // (a resolved "future"). join() then returns it unchanged.
        aot_emit(aot,"static SageValue sg_thread_sg_spawn(int _argc, SageValue* _argv){");
        aot_emit(aot,"    if(_argc<1) return sage_rt_nil();");
        aot_emit(aot,"    return sage_rt_call_fn(_argv[0], _argc-1, _argv+1);");
        aot_emit(aot,"}");
        aot_register_proc(aot,"sg_thread_sg_spawn");
    }
    // Global builtin function stubs for interpreter-only features (ffi, gc builtins, etc.)
    {
        static const struct { const char* fn; int nargs; } _gstubs[] = {
            // FFI bare functions  
            {"ffi_open",1},{"ffi_close",1},{"ffi_sym",2},
            // gc builtins  
            {"gc_disable",0},{"gc_enable",0},{"gc_collect",0},
            {"gc_collections",0},{"gc_alloc_count",0},
            // semaphore bare functions
            {"sem_new",1},{"sem_wait",1},{"sem_post",1},{"sem_destroy",1},{"sem_trywait",1},
            // memory/pointer functions
            {"ptr_add",2},{"ptr_sub",2},{"ptr_deref",1},
            // cpu/smp functions
            {"cpu_count",0},{"cpu_has_hyperthreading",0},{"cpu_physical_cores",0},
            {"cpu_logical_cores",0},{"smp_count",0},{"smp_id",0},
            // doc() builtin - returns docstring of a proc (always nil in AOT mode)
            {"doc",1},
            // path_* bare global functions
            {"path_join",3},{"path_dirname",1},{"path_basename",1},
            {"path_ext",1},{"path_stem",1},{"path_exists",1},
            // regex bare functions (stop_pos is a local var in regex module, not a function)
            {"re_match",2},{"re_find",2},{"re_replace",3},{"re_split",2},
            // sizeof bare
            {"sizeof",1},
            // timed macro stub
            {"timed",1},{"profile_start",1},{"profile_end",1},
            // bytes global functions
            {"bytes",1},{"bytes_len",1},{"bytes_get",2},{"bytes_set",3},
            {"bytes_to_string",1},{"bytes_slice",3},{"bytes_from_string",1},
            {NULL,0}
        };
        for (int _gi=0; _gstubs[_gi].fn; _gi++) {
            const char* fn = _gstubs[_gi].fn;
            int na = _gstubs[_gi].nargs;
            char sfn[64]; snprintf(sfn,sizeof(sfn),"sg_%s",fn);
            // Don't emit a nil stub for a name the user has defined as a proc
            // (e.g. a `macro timed(...)` rewritten to a proc) — that would
            // produce a conflicting redefinition.
            if (aot_is_known_proc(aot, sfn, (int)strlen(sfn))) continue;
            const char* body = NULL;
            if(!strcmp(fn,"sem_new"))          body="return sage_rt_sem_new(_a);";
            else if(!strcmp(fn,"sem_wait"))    body="return sage_rt_sem_wait(_a);";
            else if(!strcmp(fn,"sem_post"))    body="return sage_rt_sem_post(_a);";
            else if(!strcmp(fn,"sem_trywait")) body="return sage_rt_sem_trywait(_a);";
            else if(!strcmp(fn,"cpu_count")||!strcmp(fn,"cpu_logical_cores")||!strcmp(fn,"smp_count"))
                                               body="return sage_rt_cpu_count();";
            else if(!strcmp(fn,"cpu_physical_cores")) body="return sage_rt_cpu_physical_cores();";
            else if(!strcmp(fn,"cpu_has_hyperthreading")) body="return sage_rt_cpu_has_hyperthreading();";
            else if(!strcmp(fn,"gc_collections")||!strcmp(fn,"gc_alloc_count")) body="return sage_rt_gc_collections();";
            if(body){
                if(na==0) aot_emit(aot,"static SageValue %s(void){%s}",sfn,body);
                else aot_emit(aot,"static SageValue %s(SageValue _a){(void)_a;%s}",sfn,body);
                aot_register_proc(aot, sfn); aot_register_proc(aot, fn);
                continue;
            }
            if (na==0) aot_emit(aot,"static SageValue %s(void){return sage_rt_nil();}",sfn);
            else if(na==1) aot_emit(aot,"static SageValue %s(SageValue _a){(void)_a;return sage_rt_nil();}",sfn);
            else if(na==2) aot_emit(aot,"static SageValue %s(SageValue _a,SageValue _b){(void)_a;(void)_b;return sage_rt_nil();}",sfn);
            else if(na==3) aot_emit(aot,"static SageValue %s(SageValue _a,SageValue _b,SageValue _c){(void)_a;(void)_b;(void)_c;return sage_rt_nil();}",sfn);
            else aot_emit(aot,"static SageValue %s(SageValue _a,SageValue _b,SageValue _c,SageValue _d){(void)_a;(void)_b;(void)_c;(void)_d;return sage_rt_nil();}",sfn);
            aot_register_proc(aot, sfn);
            aot_register_proc(aot, fn);
        }
        // ffi_call is variadic — emit as SageNativeFn-compatible  
        aot_emit(aot,"static SageValue sg_ffi_call(SageValue _a,...){(void)_a;return sage_rt_nil();}");
        aot_register_proc(aot,"sg_ffi_call"); aot_register_proc(aot,"ffi_call");
    }
    // Pre-declare commonly-needed native module stub functions so modules that
    // import sys/socket/etc internally don't get implicit-int returns
    {
        static const struct { const char* cname; int na; } _fwdecls[] = {
            // sys module functions
            {"sg_sys_sg_args",0},{"sg_sys_sg_exit",1},{"sg_sys_sg_getenv",1},
            {"sg_sys_sg_setenv",2},{"sg_sys_sg_getcwd",0},{"sg_sys_sg_time",0},
            {"sg_sys_sg_clock",0},{"sg_sys_sg_sleep",1},
            // signal module (std.signal)
            {"sg_signal_sg_on",2},{"sg_signal_sg_emit",1},{"sg_signal_sg_off",2},
            // db module
            {"sg_std_db_sg_open",1},{"sg_std_db_sg_execute",2},{"sg_std_db_sg_query",2},
            {"sg_std_db_sg_close",1},{"sg_std_db_sg_fetch_one",1},{"sg_std_db_sg_fetch_all",1},
            {NULL,0}
        };
        for(int _fi=0; _fwdecls[_fi].cname; _fi++){
            const char* _cn = _fwdecls[_fi].cname;
            int _na = _fwdecls[_fi].na;
            if(_na==0) aot_emit(aot,"static SageValue %s(void){return sage_rt_nil();}",_cn);
            else if(_na==1) aot_emit(aot,"static SageValue %s(SageValue _a){(void)_a;return sage_rt_nil();}",_cn);
            else aot_emit(aot,"static SageValue %s(SageValue _a,SageValue _b){(void)_a;(void)_b;return sage_rt_nil();}",_cn);
            aot_register_proc(aot,_cn);
        }
    }
    // Note: _mwrap_ wrappers for global stubs (bytes, hash, doc, sizeof) are generated
    // lazily by the _mwrap_ generation pass for top-level procs, so no explicit emission needed here.
    // Recursively scan ALL stmts (including nested proc bodies) for imports
    // so imports inside proc bodies (e.g. assert.sage `import math`) are handled
    {
        // Use a stack-based iterative scan to find all STMT_IMPORT nodes
        Stmt* scan_queue[512]; int sq_head=0, sq_tail=0;
        for(Stmt*s=program;s;s=s->next) if(sq_tail<512) scan_queue[sq_tail++]=s;
        while(sq_head<sq_tail){
            Stmt* s=scan_queue[sq_head++];
            if(s->type==STMT_IMPORT) aot_compile_stmt(aot,s);
            // Recurse into proc/async proc bodies
            if((s->type==STMT_PROC||s->type==STMT_ASYNC_PROC)){
                ProcStmt* ps=(s->type==STMT_PROC)?&s->as.proc:&s->as.async_proc;
                for(Stmt*b=ps->body;b&&sq_tail<512;b=b->next) scan_queue[sq_tail++]=b;
            }
            if(s->type==STMT_BLOCK) for(Stmt*b=s->as.block.statements;b&&sq_tail<512;b=b->next) scan_queue[sq_tail++]=b;
            if(s->type==STMT_IF){for(Stmt*b=s->as.if_stmt.then_branch;b&&sq_tail<512;b=b->next)scan_queue[sq_tail++]=b;for(Stmt*b=s->as.if_stmt.else_branch;b&&sq_tail<512;b=b->next)scan_queue[sq_tail++]=b;}
            if(s->type==STMT_WHILE) for(Stmt*b=s->as.while_stmt.body;b&&sq_tail<512;b=b->next) scan_queue[sq_tail++]=b;
            if(s->type==STMT_FOR) for(Stmt*b=s->as.for_stmt.body;b&&sq_tail<512;b=b->next) scan_queue[sq_tail++]=b;
        }
    }
    aot_blank(aot);
    // Pre-declare top-level vars as file-scope C globals ONLY when they are
    // referenced in class method or top-level proc bodies (compiled before main).
    // Other vars stay as typed locals in main() for performance.
    for (Stmt* s = program; s; s = s->next) {
        if (s->type == STMT_LET && s->as.let.name.start) {
            const char* vstart = s->as.let.name.start;
            int vlen = s->as.let.name.length;
            if (!_var_used_before_main(program, vstart, vlen)) continue;
            char* vn = aot_cname_tok(s->as.let.name);
            char raw_n[64]; int rl = vlen<63?vlen:63;
            memcpy(raw_n, vstart, rl); raw_n[rl] = '\0';
            aot_emit(aot, "static SageValue %s = {0}; /* toplevel global */", vn);
            aot_set_var_type(aot, raw_n, JIT_TYPE_UNKNOWN);
            if (aot->global_var_count < 128)
                snprintf(aot->global_vars[aot->global_var_count++], 64, "%s", vn);
            free(vn);
        } else if (s->type == STMT_COMPTIME) {
            Stmt* _ct = s->as.comptime.body;
            if (_ct && _ct->type == STMT_BLOCK) _ct = _ct->as.block.statements;
            for (Stmt* cs = _ct; cs; cs = cs->next) {
                if (cs->type == STMT_LET && cs->as.let.name.start) {
                    const char* vstart2 = cs->as.let.name.start;
                    int vlen2 = cs->as.let.name.length;
                    if (!_var_used_before_main(program, vstart2, vlen2)) continue;
                    char* vn = aot_cname_tok(cs->as.let.name);
                    char raw_n2[64]; int rl2 = vlen2<63?vlen2:63;
                    memcpy(raw_n2, vstart2, rl2); raw_n2[rl2] = '\0';
                    aot_emit(aot, "static SageValue %s = {0}; /* toplevel global */", vn);
                    aot_set_var_type(aot, raw_n2, JIT_TYPE_UNKNOWN);
                    if (aot->global_var_count < 128)
                        snprintf(aot->global_vars[aot->global_var_count++], 64, "%s", vn);
                    free(vn);
                }
            }
        }
    }
    aot_blank(aot);
    // Forward-declare class constructors so methods can call them
    for(Stmt*s=program;s;s=s->next) {
        if(s->type==STMT_CLASS) {
            ClassStmt* cs = &s->as.class_stmt;
            char* cname = aot_cname_tok(cs->name);
            // Count init params — only look at this class's own init
            int np = -1;  // -1 = no init found
            for(Stmt*m=cs->methods;m;m=m->next)
                if(m->type==STMT_PROC && m->as.proc.name.length==4 &&
                   memcmp(m->as.proc.name.start,"init",4)==0) {
                    np = 0;
                    for(int i=0;i<m->as.proc.param_count;i++)
                        if(m->as.proc.params[i].length!=4||memcmp(m->as.proc.params[i].start,"self",4)!=0) np++;
                    break;
                }
            if (np < 0) {
                // No own init — skip forward decl (constructor comes from parent or is variadic)
                // The actual constructor will be emitted during class compilation
                free(cname); continue;
            }
            // Forward declare constructor with known param count
            char pbuf[512]=""; int pp=0;
            for(int i=0;i<np;i++) pp+=snprintf(pbuf+pp,sizeof(pbuf)-pp,"%sSageValue",i?",":"");
            if(np==0) aot_emit(aot,"static SageValue %s(void);",cname);
            else aot_emit(aot,"static SageValue %s(%s);",cname,pbuf);
            free(cname);
        }
    }
    aot_blank(aot);
    // Structs/enums/classes/impls/traits
    for(Stmt*s=program;s;s=s->next)
        if(s->type==STMT_STRUCT||s->type==STMT_ENUM||s->type==STMT_CLASS||s->type==STMT_IMPL||s->type==STMT_TRAIT)
            aot_compile_stmt(aot,s);
    aot_blank(aot);
    // Procs (aot_emit_proc calls aot_emit_nested_procs internally — no pre-pass needed)
    for(Stmt*s=program;s;s=s->next)
        if(s->type==STMT_PROC||s->type==STMT_ASYNC_PROC)
            aot_emit_proc(aot,s);
    // Emit SageNativeFn-compatible wrappers for top-level procs so they can be
    // passed as values to higher-order functions (arrays.map, arrays.filter, etc.)
    aot_blank(aot);
    for(Stmt*s=program;s;s=s->next) {
        if(s->type!=STMT_PROC && s->type!=STMT_ASYNC_PROC) continue;
        ProcStmt* ps=(s->type==STMT_PROC)?&s->as.proc:&s->as.async_proc;
        if(ps->body && _has_yield(ps->body)) continue; // generators have own wrappers
        char* fn=aot_cname_tok(ps->name);
        int np=ps->param_count;
        // Register _mwrap_ in known_procs so EXPR_VARIABLE can detect it exists
        {char mwn[128]; snprintf(mwn,sizeof(mwn),"_mwrap_%s",fn); aot_register_proc(aot,mwn);}
        aot_emit(aot,"static SageValue _mwrap_%s(int _argc, SageValue* _argv, void* _env) {",fn);
        aot->indent++;
        aot_emit(aot,"(void)_env;");
        // Build arg list, unboxing each parameter to match the actual proc signature
        // For generic procs [T], all params are SageValue — no unboxing
        int mwrap_is_generic = (ps->type_param_count > 0);
        char argbuf[1024]=""; int abpos=0;
        for(int i=0;i<np&&i<16;i++){
            JitTypeTag pt = mwrap_is_generic ? JIT_TYPE_UNKNOWN : aot_param_type(aot, ps->name.start, ps->name.length, i);
            char slot[80]; snprintf(slot,sizeof(slot),"(_argc>%d?_argv[%d]:sage_rt_nil())",i,i);
            char arg[128];
            if (pt==JIT_TYPE_INT)
                snprintf(arg,sizeof(arg),"SAGE_AS_INT64(%s)",slot);
            else if (pt==JIT_TYPE_FLOAT)
                snprintf(arg,sizeof(arg),"SAGE_AS_DOUBLE(%s)",slot);
            else if (pt==JIT_TYPE_BOOL)
                snprintf(arg,sizeof(arg),"sage_rt_truthy(%s)",slot);
            else if (pt==JIT_TYPE_STRING)
                snprintf(arg,sizeof(arg),"(SAGE_IS_STRING(%s)?%s.as.string:\"\")",slot,slot);
            else
                snprintf(arg,sizeof(arg),"%s",slot);
            abpos+=snprintf(argbuf+abpos,sizeof(argbuf)-abpos,"%s%s",i?",":"",arg);
        }
        aot_emit(aot,"return %s(%s);",fn,argbuf);
        aot->indent--;
        aot_emit(aot,"}");
        free(fn);
    }
    // Infer types for top-level code before emitting main
    aot_infer_body(aot, program);
    // main
    aot_emit(aot,"int main(int argc, char** argv) {");
    aot->indent++;
    aot_emit(aot,"(void)argc; (void)argv;");
    // Capture the stack base for the conservative GC stack scan. `argc` lives
    // near the top of main's frame, so its address approximates the base.
    aot_emit(aot,"sage_rt_gc_set_stack_base((void*)&argc);");
    aot_emit(aot,"sage_rt_init();");
    // ── Initialize imported module namespace dicts ────────────────────────
    {
        const char* cur_mod = "";
        for (int mi = 0; mi < aot->mod_proc_count; mi++) {
            const char* mcn = aot->mod_procs[mi].mod_cname;
            const char* wrap = aot->mod_procs[mi].wrap_cname;
            // Special case: trait definitions — assign directly to sg_TraitName (skip dict_new)
            if (strcmp(mcn,"__trait__")==0) {
                if (wrap[0]=='@'&&wrap[1]=='@') {
                    char trait_var[70]; snprintf(trait_var,sizeof(trait_var),"sg_%s",aot->mod_procs[mi].proc_raw);
                    aot_emit(aot,"%s = %s;",trait_var,wrap+2);
                }
                continue;
            }
            if (strcmp(mcn, cur_mod) != 0) {
                aot_emit(aot, "%s = sage_rt_dict_new();", mcn);
                cur_mod = aot->mod_procs[mi].mod_cname;
            }
            if (wrap[0] == '@' && wrap[1] == '@') {
                // Module variable — value expression follows @@
                const char* expr_str = wrap + 2;
                // Export to dict
                aot_emit(aot, "sage_rt_dict_set(%s, sage_rt_string(\"%s\"), %s);",
                         mcn, aot->mod_procs[mi].proc_raw, expr_str);
                // Also initialize the corresponding static var used inside module procs
                // We need to find which module this belongs to and set the prefixed var
                // Look up prefix from mod_prefix_map using mcn (strip sg_)
                for (int _pmi=0; _pmi<aot->mod_prefix_map_count; _pmi++) {
                    char tmp_mcn[64]; snprintf(tmp_mcn,sizeof(tmp_mcn),"sg_%s",aot->mod_prefix_map[_pmi].short_name);
                    if (strcmp(tmp_mcn, mcn)==0) {
                        // var name = prefix + sg_ + proc_raw
                        char* vn_c = aot_cname(aot->mod_procs[mi].proc_raw, strlen(aot->mod_procs[mi].proc_raw));
                        aot_emit(aot, "%s%s = %s; /* init module static var */",
                                 aot->mod_prefix_map[_pmi].full_prefix, vn_c, expr_str);
                        free(vn_c);
                        break;
                    }
                }
            } else if (wrap[0] == '@') {
                // Legacy single-@ variable reference
                const char* cvar = wrap + 1;
                aot_emit(aot, "sage_rt_dict_set(%s, sage_rt_string(\"%s\"), %s);",
                         mcn, aot->mod_procs[mi].proc_raw, cvar);
            } else {
                aot_emit(aot, "sage_rt_dict_set(%s, sage_rt_string(\"%s\"), sage_rt_make_fn((SageNativeFn)%s, NULL, \"%s\"));",
                         mcn, aot->mod_procs[mi].proc_raw, wrap,
                         aot->mod_procs[mi].proc_raw);
            }
        }
    }
    // Initialize interpreter-only stub module dicts with real values where possible
    // sys module: platform, version, args
    aot_emit(aot,"if(sage_rt_truthy(sg_sys)){} else {");
    aot_emit(aot,"  sg_sys=sage_rt_dict_new();");
    aot_emit(aot,"  sage_rt_dict_set(sg_sys,sage_rt_string(\"platform\"),sage_rt_string(\"linux\"));");
    aot_emit(aot,"  sage_rt_dict_set(sg_sys,sage_rt_string(\"version\"),sage_rt_string(\"%s\"));", SAGE_VERSION_STR ? SAGE_VERSION_STR : "0.2.0");
    aot_emit(aot,"  sage_rt_dict_set(sg_sys,sage_rt_string(\"os\"),sage_rt_string(\"linux\"));");
    aot_emit(aot,"}");
    // Register enums as namespace dicts
    for(Stmt*s=program;s;s=s->next){
        if(s->type==STMT_ENUM){
            EnumStmt*es=&s->as.enum_stmt;
            char*en=aot_cname_tok(es->name);
            char en_raw[256]; int enrl=es->name.length<255?es->name.length:255;
            memcpy(en_raw,es->name.start,enrl); en_raw[enrl]='\0';
            aot_emit(aot,"%s=sage_rt_dict_new();",en);
            // Store enum name so typeof() works
            aot_emit(aot,"sage_rt_dict_set(%s,sage_rt_string(\"__name__\"),sage_rt_string(\"%s\"));",en,en_raw);
            for(int i=0;i<es->variant_count;i++){
                char*vn=aot_cname_tok(es->variant_names[i]);
                char vraw[256]; int vl=es->variant_names[i].length<255?es->variant_names[i].length:255;
                memcpy(vraw,es->variant_names[i].start,vl); vraw[vl]='\0';
                int has_fields=(es->variant_field_counts&&es->variant_field_counts[i]>0);
                if(!has_fields){
                    // Simple/unit variant: store tag dict directly
                    aot_emit(aot,"sage_rt_dict_set(%s,sage_rt_string(\"%s\"),%s_%s());",en,vraw,en,vn);
                }
                // ADT variants with fields are called directly as sg_Enum_sg_Variant(args)
                // in the EXPR_CALL handler — no need to store in dict for AOT code
                free(vn);
            }
            free(en);
        }
    }
    // Register all class and struct definitions
    for(Stmt*s=program;s;s=s->next){
        if(s->type==STMT_CLASS){
            char*cn=aot_cname_tok(s->as.class_stmt.name);
            int mc=0;
            for(Stmt*m=s->as.class_stmt.methods;m;m=m->next) if(m->type==STMT_PROC) mc++;
            char rawname[256]; int nl=s->as.class_stmt.name.length<255?s->as.class_stmt.name.length:255;
            memcpy(rawname,s->as.class_stmt.name.start,nl); rawname[nl]='\0';
            // Set parent class if this class inherits
            if(s->as.class_stmt.has_parent && s->as.class_stmt.parent.length>0){
                char* pn=aot_cname_tok(s->as.class_stmt.parent);
                aot_emit(aot,"_%s_classval = sage_rt_class_new(\"%s\",_%s_classval.as.class_def,_%s_methods,%d,NULL,0,0);",
                     cn,rawname,pn,cn,mc);
                free(pn);
            } else {
                aot_emit(aot,"_%s_classval = sage_rt_class_new(\"%s\",NULL,_%s_methods,%d,NULL,0,0);",
                     cn,rawname,cn,mc);
            }
            free(cn);
        }
        if(s->type==STMT_STRUCT){
            StructStmt*ssr=&s->as.struct_stmt;
            char*cn=aot_cname_tok(ssr->name);
            char rawname[256]; int nl=ssr->name.length<255?ssr->name.length:255;
            memcpy(rawname,ssr->name.start,nl); rawname[nl]='\0';
            // Emit inline field name array for this registration
            if(ssr->field_count>0){
                aot_emit(aot,"{ static const char* _sfn[] = {");
                aot->indent++;
                for(int i=0;i<ssr->field_count;i++){
                    char esc[64]; int el=ssr->field_names[i].length<63?ssr->field_names[i].length:63;
                    memcpy(esc,ssr->field_names[i].start,el); esc[el]='\0';
                    aot_emit(aot,"\"%s\"%s",esc,i<ssr->field_count-1?",":"");
                }
                aot->indent--;
                aot_emit(aot,"};");
                aot_emit(aot,"_%s_classval=sage_rt_class_new(\"%s\",NULL,NULL,0,_sfn,%d,1); }",
                         cn,rawname,ssr->field_count);
            } else {
                aot_emit(aot,"_%s_classval=sage_rt_class_new(\"%s\",NULL,NULL,0,NULL,0,1);",cn,rawname);
            }
            free(cn);
        }
    }

    // Register impl methods (after class/struct classvals are created)
    for(Stmt*s=program;s;s=s->next){
        if(s->type==STMT_IMPL){
            ImplStmt*is=&s->as.impl_stmt;
            char*tn=aot_cname_tok(is->target);
            int mc=0; for(Stmt*m=is->methods;m;m=m->next) if(m->type==STMT_PROC) mc++;
            if(mc>0)
                aot_emit(aot,"_%s_classval = sage_rt_add_methods(_%s_classval,_%s_impl_methods,%d);",tn,tn,tn,mc);
            free(tn);
        }
    }
    aot_blank(aot);
    for(Stmt*s=program;s;s=s->next){
        if(s->type==STMT_PROC||s->type==STMT_ASYNC_PROC||
           s->type==STMT_STRUCT||s->type==STMT_ENUM||
           s->type==STMT_CLASS||s->type==STMT_IMPL||
           s->type==STMT_TRAIT||s->type==STMT_MACRO_DEF) continue;
        aot_compile_stmt(aot,s);
    }
    aot_blank(aot);
    aot_emit(aot,"sage_rt_shutdown();");
    aot_emit(aot,"return 0;");
    aot->indent--;
    aot_emit(aot,"}");
    // Join
    size_t total=0;
    for(int i=0;i<aot->line_count;i++) total+=strlen(aot->lines[i])+1;
    char* result=malloc(total+1);
    char* p=result;
    for(int i=0;i<aot->line_count;i++){size_t n=strlen(aot->lines[i]);memcpy(p,aot->lines[i],n);p+=n;*p++='\n';}
    *p='\0';
    return result;
}

int aot_write_c_file(AotCompiler* aot, const char* path) {
    FILE* f=fopen(path,"w");
    if(!f) return 0;
    for(int i=0;i<aot->line_count;i++){fputs(aot->lines[i],f);fputc('\n',f);}
    fclose(f); return 1;
}


int aot_compile_to_binary(AotCompiler* aot, const char* c_path, const char* bin_path) {
    (void)aot;
    // Find runtime dir
    const char* rt_dir = getenv("SAGE_RUNTIME_DIR");
    if (!rt_dir) rt_dir = "runtime"; // fallback for dev builds

    char rt_obj[512], rt_inc[512];
    snprintf(rt_obj,sizeof(rt_obj),"%s/libsage_runtime.a",rt_dir);
    snprintf(rt_inc,sizeof(rt_inc),"-I%s",rt_dir);

    const char* cc=getenv("CC")?getenv("CC"):"cc";
    pid_t pid=fork();
    if(pid<0) return 0;
    if(pid==0){
        execlp(cc,cc,"-std=c11","-O3","-march=native",
               "-fomit-frame-pointer","-funroll-loops","-ffast-math",
               rt_inc,c_path,rt_obj,"-o",bin_path,"-lm",(char*)NULL);
        _exit(127);
    }
    int status; waitpid(pid,&status,0);
    return WIFEXITED(status)&&WEXITSTATUS(status)==0;
}

char* aot_emit_add_int(AotCompiler* aot, const char* left, const char* right) {
    (void)aot;
    char* out=malloc(strlen(left)+strlen(right)+16);
    sprintf(out,"((%s)+(%s))",left,right); return out;
}
char* aot_emit_add_string(AotCompiler* aot, const char* left, const char* right) {
    (void)aot;
    char* out=malloc(strlen(left)+strlen(right)+80);
    sprintf(out,"sage_rt_string_concat(sage_rt_string(%s),sage_rt_string(%s)).as.string",left,right); return out;
}
char* aot_emit_add_generic(AotCompiler* aot, const char* left, const char* right) {
    (void)aot;
    char* out=malloc(strlen(left)+strlen(right)+32);
    sprintf(out,"sage_rt_add(%s,%s)",left,right); return out;
}
