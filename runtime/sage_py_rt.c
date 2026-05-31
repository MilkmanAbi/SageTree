// sage_py_rt.c — Python FFI for AOT-compiled SageTree programs.
//
// Mirrors src/sage_python.c (the interpreter's embed) but operates on the
// runtime's SageValue type. Arbitrary Python objects are wrapped behind a
// SAGE_VAL_POINTER that points at a small tagged box {PyObject* obj; int tag}
// (tag == SAGE_PY_TAG) so they can round-trip through Sage values.
//
// Everything is guarded by SAGE_HAS_PYRT. When Python isn't available the
// functions are still defined (so the runtime always links) but return nil.

#include "sage_runtime.h"
#include <string.h>
#include <stdio.h>

#ifdef SAGE_HAS_PYRT
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#define SAGE_PY_TAG 0x50590099  /* "PY" + marker */

typedef struct { void* obj; int tag; } SagePyBox;

static int py_initialized = 0;
static void py_ensure_init(void) {
    if (!py_initialized) { Py_Initialize(); py_initialized = 1; }
}

// Is this SageValue a wrapped PyObject?
static int sg_is_pyobj(SageValue v) {
    if (v.type != SAGE_VAL_POINTER || !v.as.pointer) return 0;
    SagePyBox* b = (SagePyBox*)v.as.pointer;
    return b->tag == SAGE_PY_TAG;
}
static PyObject* sg_get_pyobj(SageValue v) {
    if (!sg_is_pyobj(v)) return NULL;
    return (PyObject*)((SagePyBox*)v.as.pointer)->obj;
}
static SageValue sg_wrap_pyobj(PyObject* obj) {
    Py_INCREF(obj);
    SagePyBox* b = (SagePyBox*)sage_rt_alloc(sizeof(SagePyBox));
    b->obj = obj; b->tag = SAGE_PY_TAG;
    SageValue v; v.type = SAGE_VAL_POINTER; v.as.pointer = b;
    return v;
}

// ── SageValue → PyObject ─────────────────────────────────────────────────────
static PyObject* sage_to_py(SageValue val) {
    switch (val.type) {
        case SAGE_VAL_INT:    return PyLong_FromLongLong(val.as.integer);
        case SAGE_VAL_FLOAT:  return PyFloat_FromDouble(val.as.number);
        case SAGE_VAL_BOOL:   { PyObject* b = val.as.boolean ? Py_True : Py_False; Py_INCREF(b); return b; }
        case SAGE_VAL_NIL:    Py_RETURN_NONE;
        case SAGE_VAL_STRING: return PyUnicode_FromString(val.as.string ? val.as.string : "");
        case SAGE_VAL_ARRAY: {
            SageArray* a = val.as.array;
            PyObject* list = PyList_New(a->count);
            for (int i = 0; i < a->count; i++)
                PyList_SetItem(list, i, sage_to_py(a->elems[i]));
            return list;
        }
        case SAGE_VAL_POINTER:
            if (sg_is_pyobj(val)) { PyObject* o = sg_get_pyobj(val); Py_INCREF(o); return o; }
            Py_RETURN_NONE;
        default: Py_RETURN_NONE;
    }
}

// ── PyObject → SageValue ─────────────────────────────────────────────────────
static SageValue py_to_sage(PyObject* obj) {
    if (obj == NULL || obj == Py_None) return sage_rt_nil();
    if (PyBool_Check(obj)) return sage_rt_bool(obj == Py_True);
    if (PyLong_Check(obj)) {
        long long v = PyLong_AsLongLong(obj);
        if (v == -1 && PyErr_Occurred()) { PyErr_Clear(); return sage_rt_nil(); }
        return sage_rt_int((int64_t)v);
    }
    if (PyFloat_Check(obj)) return sage_rt_float(PyFloat_AsDouble(obj));
    if (PyUnicode_Check(obj)) {
        const char* s = PyUnicode_AsUTF8(obj);
        return s ? sage_rt_string(s) : sage_rt_nil();
    }
    if (PyList_Check(obj)) {
        Py_ssize_t n = PyList_Size(obj);
        SageValue arr = sage_rt_array_new();
        for (Py_ssize_t i = 0; i < n; i++)
            sage_rt_array_push(arr, py_to_sage(PyList_GetItem(obj, i)));
        return arr;
    }
    if (PyTuple_Check(obj)) {
        Py_ssize_t n = PyTuple_Size(obj);
        SageValue arr = sage_rt_array_new();
        for (Py_ssize_t i = 0; i < n; i++)
            sage_rt_array_push(arr, py_to_sage(PyTuple_GetItem(obj, i)));
        return arr;
    }
    if (PyDict_Check(obj)) {
        SageValue d = sage_rt_dict_new();
        PyObject *key, *pval; Py_ssize_t pos = 0;
        while (PyDict_Next(obj, &pos, &key, &pval)) {
            const char* k = PyUnicode_AsUTF8(key);
            if (k) sage_rt_dict_set(d, sage_rt_string(k), py_to_sage(pval));
        }
        return d;
    }
    if (PySequence_Check(obj) && !PyBytes_Check(obj) && !PyByteArray_Check(obj)) {
        Py_ssize_t n = PySequence_Size(obj);
        if (n >= 0 && n < 1000000) {
            SageValue arr = sage_rt_array_new();
            for (Py_ssize_t i = 0; i < n; i++) {
                PyObject* it = PySequence_GetItem(obj, i);
                if (it) { sage_rt_array_push(arr, py_to_sage(it)); Py_DECREF(it); }
            }
            return arr;
        }
        PyErr_Clear();
    }
    // Arbitrary object → wrapped pointer
    return sg_wrap_pyobj(obj);
}

SageValue sage_rt_py_import(SageValue name) {
    if (!SAGE_IS_STRING(name) || !name.as.string) return sage_rt_nil();
    py_ensure_init();
    PyObject* mod = PyImport_ImportModule(name.as.string);
    if (!mod) { PyErr_Print(); return sage_rt_nil(); }
    SageValue r = py_to_sage(mod); Py_DECREF(mod); return r;
}

SageValue sage_rt_py_getattr(SageValue obj, SageValue name) {
    if (!SAGE_IS_STRING(name)) return sage_rt_nil();
    py_ensure_init();
    PyObject* po = sg_is_pyobj(obj) ? sg_get_pyobj(obj) : sage_to_py(obj);
    if (!po) return sage_rt_nil();
    PyObject* attr = PyObject_GetAttrString(po, name.as.string);
    if (!sg_is_pyobj(obj)) Py_DECREF(po);
    if (!attr) { PyErr_Clear(); return sage_rt_nil(); }
    SageValue r = py_to_sage(attr);
    if (!PyCallable_Check(attr)) Py_DECREF(attr);
    return r;
}

// python.call(obj, "method", ...args)  — argv holds the trailing args
SageValue sage_rt_py_call(SageValue obj, SageValue method, int argc, SageValue* argv) {
    if (!SAGE_IS_STRING(method)) return sage_rt_nil();
    py_ensure_init();
    PyObject* po = sg_is_pyobj(obj) ? sg_get_pyobj(obj) : sage_to_py(obj);
    if (!po) return sage_rt_nil();
    PyObject* m = PyObject_GetAttrString(po, method.as.string);
    if (!sg_is_pyobj(obj)) Py_DECREF(po);
    if (!m) { PyErr_Print(); return sage_rt_nil(); }
    PyObject* args = PyTuple_New(argc);
    for (int i = 0; i < argc; i++) PyTuple_SetItem(args, i, sage_to_py(argv[i]));
    PyObject* res = PyObject_CallObject(m, args);
    Py_DECREF(m); Py_DECREF(args);
    if (!res) { PyErr_Print(); return sage_rt_nil(); }
    SageValue r = py_to_sage(res); Py_DECREF(res); return r;
}

// Direct invoke of a wrapped callable: callable(...args)
SageValue sage_rt_py_invoke(SageValue callable, int argc, SageValue* argv) {
    py_ensure_init();
    PyObject* c = sg_is_pyobj(callable) ? sg_get_pyobj(callable) : sage_to_py(callable);
    if (!c || !PyCallable_Check(c)) { if (!sg_is_pyobj(callable) && c) Py_DECREF(c); return sage_rt_nil(); }
    PyObject* args = PyTuple_New(argc);
    for (int i = 0; i < argc; i++) PyTuple_SetItem(args, i, sage_to_py(argv[i]));
    PyObject* res = PyObject_CallObject(c, args);
    Py_DECREF(args);
    if (!sg_is_pyobj(callable)) Py_DECREF(c);
    if (!res) { PyErr_Print(); return sage_rt_nil(); }
    SageValue r = py_to_sage(res); Py_DECREF(res); return r;
}

SageValue sage_rt_py_eval(SageValue code) {
    if (!SAGE_IS_STRING(code)) return sage_rt_nil();
    py_ensure_init();
    PyObject* main_mod = PyImport_AddModule("__main__");
    PyObject* globals = PyModule_GetDict(main_mod);
    PyObject* res = PyRun_String(code.as.string, Py_eval_input, globals, globals);
    if (!res) { PyErr_Print(); return sage_rt_nil(); }
    SageValue r = py_to_sage(res); Py_DECREF(res); return r;
}

SageValue sage_rt_py_exec(SageValue code) {
    if (!SAGE_IS_STRING(code)) return sage_rt_bool(0);
    py_ensure_init();
    int ok = PyRun_SimpleString(code.as.string);
    return sage_rt_bool(ok == 0);
}

// math.sqrt(x) style: getattr(obj, name) then invoke with args.
SageValue sage_rt_py_method(SageValue obj, const char* name, int argc, SageValue* argv) {
    py_ensure_init();
    PyObject* po = sg_is_pyobj(obj) ? sg_get_pyobj(obj) : sage_to_py(obj);
    if (!po) return sage_rt_nil();
    PyObject* m = PyObject_GetAttrString(po, name);
    if (!sg_is_pyobj(obj)) Py_DECREF(po);
    if (!m) { PyErr_Clear(); return sage_rt_nil(); }
    PyObject* args = PyTuple_New(argc);
    for (int i = 0; i < argc; i++) PyTuple_SetItem(args, i, sage_to_py(argv[i]));
    PyObject* res = PyObject_CallObject(m, args);
    Py_DECREF(m); Py_DECREF(args);
    if (!res) { PyErr_Print(); return sage_rt_nil(); }
    SageValue r = py_to_sage(res); Py_DECREF(res); return r;
}

int sage_rt_py_is_obj(SageValue v) { return sg_is_pyobj(v); }

#else  /* no Python */

SageValue sage_rt_py_import(SageValue name) { (void)name; return sage_rt_nil(); }
SageValue sage_rt_py_getattr(SageValue o, SageValue n) { (void)o;(void)n; return sage_rt_nil(); }
SageValue sage_rt_py_call(SageValue o, SageValue m, int argc, SageValue* argv) { (void)o;(void)m;(void)argc;(void)argv; return sage_rt_nil(); }
SageValue sage_rt_py_invoke(SageValue c, int argc, SageValue* argv) { (void)c;(void)argc;(void)argv; return sage_rt_nil(); }
SageValue sage_rt_py_eval(SageValue c) { (void)c; return sage_rt_nil(); }
SageValue sage_rt_py_exec(SageValue c) { (void)c; return sage_rt_bool(0); }
SageValue sage_rt_py_method(SageValue o, const char* n, int argc, SageValue* argv) { (void)o;(void)n;(void)argc;(void)argv; return sage_rt_nil(); }
int sage_rt_py_is_obj(SageValue v) { (void)v; return 0; }

#endif
