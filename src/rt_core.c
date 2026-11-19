/* rt_core.c -- initialisation, the error path, and the core data objects
 * (pairs, symbols, vectors, bytevectors, boxes, records, error objects, values,
 * equivalence). All primitives are leaves: they never call Scheme code.
 * File-local statics carry a per-file prefix: the amalgamated program is one TU. */
#ifndef SHIKRA_RT_H
#include "rt.h"
#endif

RT_VAR int rt_argc;
RT_VAR char **rt_argv;
#ifndef SHIKRA_PROGRAM
RT_VAR void (*rt_raise_hook)(obj e);
#endif
static RT_UNUSED obj core_symtab; /* vector, open addressing; #f = empty slot */
static RT_UNUSED intptr_t core_symcount;

RT_FN void rt_init(int argc, char **argv, void *stack_base) {
    rt_argc = argc;
    rt_argv = argv;
    rt_gc_init(stack_base);
    rt_gc_add_roots(&core_symtab, &core_symtab + 1);
    core_symtab = rt_vector_n(256, RT_FALSE);
    rt_gc_add_roots(&rt_stdin, &rt_stdin + 1);
    rt_gc_add_roots(&rt_stdout, &rt_stdout + 1);
    rt_gc_add_roots(&rt_stderr, &rt_stderr + 1);
    rt_stdin = rt_make_fd_port(0, RT_PF_IN, RT_FALSE);
    rt_stdout = rt_make_fd_port(1, RT_PF_OUT, RT_FALSE);
    rt_stderr = rt_make_fd_port(2, RT_PF_OUT | RT_PF_UNBUF, RT_FALSE);
}

RT_FN void rt_exit(int code) {
    if (rt_stdout) rt_flush(rt_stdout);
    if (rt_stderr) rt_flush(rt_stderr);
    rt_sys_exit(code);
}

RT_FN obj rt_exit_o(obj code) {
    rt_exit(code == RT_TRUE || code == RT_DEFAULT ? 0 : rt_is_fix(code) ? (int)RT_FIXVAL(code) : 1);
}

/* ---- error path ---- */
RT_FN obj rt_make_error(int kind, obj message, obj irritants, obj loc) {
    rt_vector *e = (rt_vector *)rt_alloc(RT_HDR(RT_T_ERROR, kind, 3), 4 * sizeof(obj));
    e->v[0] = message;
    e->v[1] = irritants;
    e->v[2] = loc;
    return RT_TAGPTR(e);
}

RT_FN void rt_raise(obj e) {
#ifdef SHIKRA_PROGRAM
    rt_engine_raise(e);
#else
    if (rt_raise_hook) rt_raise_hook(e);
    rt_uncaught(e);
#endif
}

RT_FN void rt_uncaught(obj e) {
    obj p = rt_stderr;
    rt_flush(rt_stdout);
    if (rt_is_type(e, RT_T_ERROR)) {
        obj irr;
        if (RT_VEC(e)[2] != RT_FALSE) { rt_print(RT_VEC(e)[2], p, 0); rt_port_puts(p, ": error: "); }
        else rt_port_puts(p, "Error: ");
        rt_print(RT_VEC(e)[0], p, 0);
        for (irr = RT_VEC(e)[1]; rt_is_pair(irr); irr = RT_CDR(irr)) {
            rt_port_putc(p, ' ');
            rt_print(RT_CAR(irr), p, 1);
        }
    } else {
        rt_port_puts(p, "Error: uncaught exception: ");
        rt_print(e, p, 1);
    }
    rt_port_putc(p, '\n');
    rt_exit(70);
}

RT_FN void rt_error(const char *who, const char *msg, obj irritants) {
    obj m = rt_open_output_string();
    if (who) { rt_port_puts(m, who); rt_port_puts(m, ": "); }
    rt_port_puts(m, msg);
    rt_raise(rt_make_error(RT_ERR_GENERAL, rt_get_output_string(m), irritants, RT_FALSE));
}

RT_FN void rt_type_error(const char *who, const char *expected, obj x) {
    obj m = rt_open_output_string();
    rt_port_puts(m, "expected ");
    rt_port_puts(m, expected);
    rt_error(who, RT_STR(rt_get_output_string(m))->buf, rt_cons(x, RT_NIL));
}

RT_FN void rt_range_error(const char *who, obj x) { rt_error(who, "index out of range", rt_cons(x, RT_NIL)); }

/* ---- pairs and lists ---- */
RT_FN intptr_t rt_list_length(obj x, const char *who) {
    intptr_t n = 0;
    obj slow = x;
    while (rt_is_pair(x)) {
        x = RT_CDR(x);
        if (++n % 2 == 0) {
            slow = RT_CDR(slow);
            if (slow == x && rt_is_pair(x)) rt_error(who, "circular list", RT_NIL);
        }
    }
    if (x != RT_NIL) rt_type_error(who, "proper list", x);
    return n;
}
RT_FN obj rt_length(obj x) { return RT_FIX(rt_list_length(x, "length")); }
RT_FN obj rt_list_p(obj x) {
    obj slow = x;
    for (;;) {
        if (!rt_is_pair(x)) return RT_BOOL(x == RT_NIL);
        x = RT_CDR(x);
        if (!rt_is_pair(x)) return RT_BOOL(x == RT_NIL);
        x = RT_CDR(x);
        slow = RT_CDR(slow);
        if (x == slow) return RT_FALSE;
    }
}
RT_FN obj rt_reverse(obj x) {
    obj r = RT_NIL;
    for (; rt_is_pair(x); x = RT_CDR(x)) r = rt_cons(RT_CAR(x), r);
    if (x != RT_NIL) rt_type_error("reverse", "proper list", x);
    return r;
}

/* ---- equivalence ---- */
RT_FN int rt_eqv(obj a, obj b) {
    if (a == b) return 1;
    if (rt_is_flo(a) && rt_is_flo(b)) { /* same IEEE bits */
        double x = RT_FLO(a), y = RT_FLO(b);
        return rt_memcmp(&x, &y, sizeof x) == 0;
    }
    return 0;
}
/* TODO(M5): R7RS requires equal? to terminate on circular structures. */
RT_FN int rt_equal(obj a, obj b) {
    for (;;) {
        if (rt_eqv(a, b)) return 1;
        if (rt_is_pair(a) && rt_is_pair(b)) {
            if (!rt_equal(RT_CAR(a), RT_CAR(b))) return 0;
            a = RT_CDR(a); b = RT_CDR(b);
            continue;
        }
        if (!rt_is_heap(a) || !rt_is_heap(b) || RT_HTYPE(a) != RT_HTYPE(b)) return 0;
        switch (RT_HTYPE(a)) {
        case RT_T_STRING:
            return RT_STR(a)->bytes == RT_STR(b)->bytes &&
                   rt_memcmp(RT_STR(a)->buf, RT_STR(b)->buf, (size_t)RT_STR(a)->bytes) == 0;
        case RT_T_BYTEVECTOR:
            return RT_HLEN(a) == RT_HLEN(b) &&
                   rt_memcmp(RT_PTR(a, rt_bytevector)->b, RT_PTR(b, rt_bytevector)->b, (size_t)RT_HLEN(a)) == 0;
        case RT_T_VECTOR: {
            intptr_t i, n = RT_HLEN(a);
            if (n != RT_HLEN(b)) return 0;
            if (n == 0) return 1;
            for (i = 0; i < n - 1; i++) if (!rt_equal(RT_VEC(a)[i], RT_VEC(b)[i])) return 0;
            a = RT_VEC(a)[n - 1]; b = RT_VEC(b)[n - 1];
            continue;
        }
        default: return 0;
        }
    }
}
RT_FN obj rt_eqv_p(obj a, obj b) { return RT_BOOL(rt_eqv(a, b)); }
RT_FN obj rt_equal_p(obj a, obj b) { return RT_BOOL(rt_equal(a, b)); }

/* ---- symbols ---- */
static uintptr_t core_hash(const char *s, size_t n) {
    uintptr_t h = 14695981039346656037u;
    while (n--) h = (h ^ (unsigned char)*s++) * 1099511628211u;
    return h;
}
RT_FN obj rt_make_symbol(obj name) {
    rt_symbol *s = (rt_symbol *)rt_alloc(RT_HDR(RT_T_SYMBOL, 0, 1), sizeof(rt_symbol));
    s->name = name;
    return RT_TAGPTR(s);
}
static obj *core_symslot(const char *s, size_t n) {
    intptr_t mask = RT_HLEN(core_symtab) - 1, i = (intptr_t)(core_hash(s, n) & (uintptr_t)mask);
    for (;; i = (i + 1) & mask) {
        obj *slot = &RT_VEC(core_symtab)[i], name;
        if (*slot == RT_FALSE) return slot;
        name = RT_PTR(*slot, rt_symbol)->name;
        if ((size_t)RT_STR(name)->bytes == n && rt_memcmp(RT_STR(name)->buf, s, n) == 0) return slot;
    }
}
static void core_symadd(obj sym) {
    obj name = RT_PTR(sym, rt_symbol)->name;
    if (2 * (core_symcount + 1) > RT_HLEN(core_symtab)) { /* grow and rehash */
        obj old = core_symtab;
        intptr_t i;
        core_symtab = rt_vector_n(2 * RT_HLEN(old), RT_FALSE);
        for (i = 0; i < RT_HLEN(old); i++) {
            obj s = RT_VEC(old)[i];
            if (s != RT_FALSE) {
                obj nm = RT_PTR(s, rt_symbol)->name;
                *core_symslot(RT_STR(nm)->buf, (size_t)RT_STR(nm)->bytes) = s;
            }
        }
    }
    *core_symslot(RT_STR(name)->buf, (size_t)RT_STR(name)->bytes) = sym;
    core_symcount++;
}
RT_FN obj rt_intern(const char *s, size_t n) {
    obj *slot = core_symslot(s, n), name, sym;
    if (*slot != RT_FALSE) return *slot;
    name = rt_make_string_n(s, n);
    RT_SETFLAGS(name, RT_HFLAGS(name) | RT_F_RO);
    sym = rt_make_symbol(name);
    core_symadd(sym);
    return sym;
}
RT_FN void rt_intern_static(obj sym) {
    obj name = RT_PTR(sym, rt_symbol)->name;
    if (*core_symslot(RT_STR(name)->buf, (size_t)RT_STR(name)->bytes) == RT_FALSE) core_symadd(sym);
}
RT_FN obj rt_string_to_symbol(obj s) {
    RT_CHECK(rt_is_string(s), "string->symbol", "string", s);
    return rt_intern(RT_STR(s)->buf, (size_t)RT_STR(s)->bytes);
}
RT_FN obj rt_symbol_to_string(obj s) {
    RT_CHECK(rt_is_symbol(s), "symbol->string", "symbol", s);
    return RT_PTR(s, rt_symbol)->name;
}

/* ---- vectors, bytevectors, boxes ---- */
RT_FN obj rt_vector_n(intptr_t n, obj fill) {
    rt_vector *v = (rt_vector *)rt_alloc(RT_HDR(RT_T_VECTOR, 0, n), sizeof(obj) * (size_t)(n + 1));
    intptr_t i;
    for (i = 0; i < n; i++) v->v[i] = fill;
    return RT_TAGPTR(v);
}
static intptr_t core_count(obj k, const char *who) {
    if (!rt_is_fix(k) || RT_FIXVAL(k) < 0 || RT_FIXVAL(k) > ((intptr_t)1 << 40)) rt_type_error(who, "valid length", k);
    return RT_FIXVAL(k);
}
RT_FN obj rt_make_vector(obj k, obj fill) { return rt_vector_n(core_count(k, "make-vector"), fill); }
RT_FN obj rt_list_to_vector(obj l) {
    intptr_t i, n = rt_list_length(l, "list->vector");
    obj v = rt_vector_n(n, RT_FALSE);
    for (i = 0; i < n; i++, l = RT_CDR(l)) RT_VEC(v)[i] = RT_CAR(l);
    return v;
}
/* Validate start/end against len; returns start, sets *e. */
static intptr_t core_range(obj start, obj end, intptr_t len, intptr_t *e, const char *who) {
    intptr_t s = rt_is_fix(start) ? RT_FIXVAL(start) : -1;
    *e = end == RT_DEFAULT ? len : rt_is_fix(end) ? RT_FIXVAL(end) : -1;
    if (start == RT_DEFAULT) s = 0;
    if (s < 0 || s > len) rt_range_error(who, start);
    if (*e < s || *e > len) rt_range_error(who, end);
    return s;
}
RT_FN obj rt_vector_to_list(obj v, obj start, obj end) {
    intptr_t s, e;
    obj r = RT_NIL;
    RT_CHECK(rt_is_vector(v), "vector->list", "vector", v);
    s = core_range(start, end, RT_HLEN(v), &e, "vector->list");
    while (e > s) r = rt_cons(RT_VEC(v)[--e], r);
    return r;
}
static obj core_bytevector_n(intptr_t n) {
    return RT_TAGPTR(rt_alloc(RT_HDR(RT_T_BYTEVECTOR, 0, n), sizeof(uintptr_t) + (size_t)n));
}
RT_FN obj rt_make_bytevector(obj k, obj fill) {
    intptr_t n = core_count(k, "make-bytevector");
    obj v;
    if (!rt_is_fix(fill) || (uintptr_t)RT_FIXVAL(fill) > 255) rt_type_error("make-bytevector", "byte", fill);
    v = core_bytevector_n(n);
    rt_memset(RT_PTR(v, rt_bytevector)->b, (int)RT_FIXVAL(fill), (size_t)n);
    return v;
}
RT_FN obj rt_bytevector_copy(obj bv, obj start, obj end) {
    intptr_t s, e;
    obj r;
    RT_CHECK(rt_is_bytevector(bv), "bytevector-copy", "bytevector", bv);
    s = core_range(start, end, RT_HLEN(bv), &e, "bytevector-copy");
    r = core_bytevector_n(e - s);
    rt_memcpy(RT_PTR(r, rt_bytevector)->b, RT_PTR(bv, rt_bytevector)->b + s, (size_t)(e - s));
    return r;
}
RT_FN obj rt_make_box(obj x) {
    rt_vector *b = (rt_vector *)rt_alloc(RT_HDR(RT_T_BOX, 0, 1), 2 * sizeof(obj));
    b->v[0] = x;
    return RT_TAGPTR(b);
}

/* ---- records ---- */
RT_FN obj rt_make_rtd(obj name, obj fields) {
    rt_vector *t;
    RT_CHECK(rt_is_symbol(name), "make-record-type", "symbol", name);
    RT_CHECK(rt_is_vector(fields), "make-record-type", "vector", fields);
    t = (rt_vector *)rt_alloc(RT_HDR(RT_T_RTD, 0, 2), 3 * sizeof(obj));
    t->v[0] = name;
    t->v[1] = fields;
    return RT_TAGPTR(t);
}
RT_FN obj rt_make_record(obj rtd) {
    intptr_t i, n;
    rt_vector *r;
    RT_CHECK(rt_is_type(rtd, RT_T_RTD), "make-record", "record type", rtd);
    n = RT_HLEN(RT_VEC(rtd)[1]);
    r = (rt_vector *)rt_alloc(RT_HDR(RT_T_RECORD, 0, n + 1), sizeof(obj) * (size_t)(n + 2));
    r->v[0] = rtd;
    for (i = 1; i <= n; i++) r->v[i] = RT_UNSPEC;
    return RT_TAGPTR(r);
}
RT_FN obj rt_record_p(obj x, obj rtd) { return RT_BOOL(rt_is_type(x, RT_T_RECORD) && RT_VEC(x)[0] == rtd); }
RT_FN void rt_record_error(obj x, obj rtd) {
    obj m = rt_open_output_string();
    rt_port_puts(m, "expected record of type ");
    rt_print(RT_VEC(rtd)[0], m, 0);
    rt_error("record accessor", RT_STR(rt_get_output_string(m))->buf, rt_cons(x, RT_NIL));
}

/* ---- error objects ---- */
RT_FN obj rt_make_error_object(obj kind, obj message, obj irritants) {
    RT_CHECK(rt_is_fix(kind) && (uintptr_t)RT_FIXVAL(kind) <= RT_ERR_READ, "make-error-object", "error kind", kind);
    return rt_make_error((int)RT_FIXVAL(kind), message, irritants, RT_FALSE);
}
RT_FN obj rt_error_object_p(obj x) { return RT_BOOL(rt_is_type(x, RT_T_ERROR)); }
RT_FN obj rt_error_object_message(obj x) {
    RT_CHECK(rt_is_type(x, RT_T_ERROR), "error-object-message", "error object", x);
    return RT_VEC(x)[0];
}
RT_FN obj rt_error_object_irritants(obj x) {
    RT_CHECK(rt_is_type(x, RT_T_ERROR), "error-object-irritants", "error object", x);
    return RT_VEC(x)[1];
}
RT_FN obj rt_file_error_p(obj x) { return RT_BOOL(rt_is_type(x, RT_T_ERROR) && RT_HFLAGS(x) == RT_ERR_FILE); }
RT_FN obj rt_read_error_p(obj x) { return RT_BOOL(rt_is_type(x, RT_T_ERROR) && RT_HFLAGS(x) == RT_ERR_READ); }

/* ---- multiple values (only for unknown receivers, spec §8.5) and procedures ---- */
RT_FN obj rt_values_n(intptr_t n, const obj *vals) {
    rt_vector *v = (rt_vector *)rt_alloc(RT_HDR(RT_T_VALUES, 0, n), sizeof(obj) * (size_t)(n + 1));
    intptr_t i;
    for (i = 0; i < n; i++) v->v[i] = vals[i];
    return RT_TAGPTR(v);
}
RT_FN obj rt_make_values(obj l) {
    obj v = rt_list_to_vector(l);
    RT_HDRREF(v) = (RT_HDRREF(v) & ~(uintptr_t)0xFF) | RT_T_VALUES;
    return v;
}
RT_FN obj rt_values_to_list(obj v) {
    intptr_t i;
    obj r = RT_NIL;
    RT_CHECK(rt_is_type(v, RT_T_VALUES), "values->list", "values", v);
    for (i = RT_HLEN(v); i > 0; i--) r = rt_cons(RT_VEC(v)[i - 1], r);
    return r;
}
RT_FN obj rt_make_proc(rt_code code, intptr_t nfree) {
    rt_proc *p = (rt_proc *)rt_alloc(RT_HDR(RT_T_PROC, 0, nfree), sizeof(rt_proc) + sizeof(obj) * (size_t)nfree);
    intptr_t i;
    p->code = code;
    for (i = 0; i < nfree; i++) p->free[i] = RT_FALSE;
    return RT_PROC_OBJ(p);
}
