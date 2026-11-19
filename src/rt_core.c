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
static RT_UNUSED obj core_argbuf[RT_ARGBUF];
static RT_UNUSED intptr_t core_argcap = RT_ARGBUF;
RT_VAR obj *rt_args = core_argbuf;
RT_VAR obj rt_winders = RT_NIL, rt_handlers = RT_NIL;
#ifndef RT_HAVE_MUSTTAIL
RT_VAR rt_code rt_tr_fn; /* the trampoline's pending tail call (rt.h) */
RT_VAR obj rt_tr_self, rt_tr_a[4];
RT_VAR intptr_t rt_tr_argc;
#endif
static RT_UNUSED obj *core_symtab, *core_symend; /* open addressing; 0 = empty slot */
static RT_UNUSED intptr_t core_symcount;
#if defined(SHIKRA_PROGRAM) && defined(RT_TIER3)
static char *core_base; /* rt_init's stack base: tier 3 copies the stack from a capture up to it */
#endif

RT_FN void rt_init(int argc, char **argv, void *stack_base) {
    rt_argc = argc;
    rt_argv = argv;
    /* Nothing is allocated here (the standard ports are static, the symbol table is made
     * on first use), so a program that never allocates links no collector (spec §9.1).
     * Every runtime global that holds an obj is a root range: */
    rt_gc_init(stack_base);
#ifdef RT_TIER3
    core_base = (char *)stack_base;
#endif
    rt_gc_add_dynamic_roots(&core_symtab, &core_symend);
    rt_gc_add_roots(core_argbuf, core_argbuf + RT_ARGBUF);
    rt_gc_add_roots(&rt_winders, &rt_winders + 1);
    rt_gc_add_roots(&rt_handlers, &rt_handlers + 1);
    rt_gc_add_roots(&rt_stdin, &rt_stdin + 1);
    rt_gc_add_roots(&rt_stdout, &rt_stdout + 1);
    rt_gc_add_roots(&rt_stderr, &rt_stderr + 1);
    rt_gc_add_roots(&rt_ports, &rt_ports + 1);
    rt_gc_add_roots(&rt_port_cells[0].car, &rt_port_cells[2].cdr + 1);
}

RT_FN void rt_exit(int code) {
    rt_flush_all();
    rt_sys_exit(code);
}

/* R7RS exit statuses: #f -> 1, an exact integer -> itself, anything else -> 0. */
RT_FN obj rt_exit_o(obj code) {
    rt_exit(code == RT_FALSE ? 1 : rt_is_fix(code) ? (int)RT_FIXVAL(code) : 0);
}

/* ---- system interface: (scheme process-context), (scheme time) ---- */
RT_FN obj rt_command_line(void) {
    obj l = RT_NIL;
    int i;
    for (i = rt_argc - 1; i >= 0; i--) l = rt_cons(rt_cstring(rt_argv[i]), l);
    return l;
}
RT_FN obj rt_get_environment_variable(obj name) {
    const char *v;
    RT_CHECK(rt_is_string(name), "get-environment-variable", "string", name);
    v = rt_sys_getenv(RT_STR(name)->buf);
    return v ? rt_cstring(v) : RT_FALSE;
}
RT_FN obj rt_get_environment_variables(void) {
    char **e = rt_sys_environ();
    obj l = RT_NIL, k;
    size_t n;
    for (; e && *e; e++) {
        for (n = 0; (*e)[n] && (*e)[n] != '='; n++) {}
        k = rt_make_string_n(*e, n);
        l = rt_cons(rt_cons(k, rt_cstring((*e)[n] ? *e + n + 1 : "")), l);
    }
    return rt_reverse(l);
}
RT_FN obj rt_current_second(void) { return rt_make_flonum((double)rt_sys_clock_ns(0) / 1e9); }
RT_FN obj rt_current_jiffy(void) { return RT_FIX(rt_sys_clock_ns(1)); } /* nanoseconds */

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
    rt_print_error(e);
    rt_exit(70);
}
RT_FN void rt_print_error(obj e) {
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
}

#ifdef RT_NO_HANDLERS
/* No exception handler can ever be installed in this program (the analysis found no
 * with-exception-handler or guard; DESIGN.md M4-2): a runtime error is uncaught, so it is
 * printed as rt_uncaught would and the program exits, with no error object, no irritant list
 * and no Scheme raise: the error path allocates nothing. */
static void core_die_head(const char *who, const char *msg, const char *msg2) {
    rt_flush(rt_stdout);
    rt_port_write_fd(rt_stderr, "Error: ", 7);
    if (who) { rt_port_write_fd(rt_stderr, who, rt_strlen(who)); rt_port_write_fd(rt_stderr, ": ", 2); }
    rt_port_write_fd(rt_stderr, msg, rt_strlen(msg));
    if (msg2) rt_port_write_fd(rt_stderr, msg2, rt_strlen(msg2));
}
static RT_NORETURN void core_die(const char *who, const char *msg, const char *msg2, obj irritants) {
    core_die_head(who, msg, msg2);
    for (; rt_is_pair(irritants); irritants = RT_CDR(irritants)) {
        rt_port_putc(rt_stderr, ' ');
        rt_print(RT_CAR(irritants), rt_stderr, 1);
    }
    rt_port_write_fd(rt_stderr, "\n", 1);
    rt_exit(70);
}
#define CORE_DIE1(who, msg, msg2, x) do { obj i_[2]; i_[0] = (x); i_[1] = RT_NIL; core_die1(who, msg, msg2, i_); } while (0)
static RT_NORETURN void core_die1(const char *who, const char *msg, const char *msg2, obj *x) {
    core_die_head(who, msg, msg2);
    rt_port_putc(rt_stderr, ' ');
    rt_print(x[0], rt_stderr, 1);
    rt_port_write_fd(rt_stderr, "\n", 1);
    rt_exit(70);
}
RT_FN void rt_error(const char *who, const char *msg, obj irritants) { core_die(who, msg, 0, irritants); }
RT_FN void rt_type_error(const char *who, const char *expected, obj x) { CORE_DIE1(who, "expected ", expected, x); }
RT_FN void rt_range_error(const char *who, obj x) { CORE_DIE1(who, "index out of range", 0, x); }
/* Fixnum overflow of + - * (also from specialised code): the irritants are fixnums, printed by
 * the integer printer alone (so the section 12 program links no general printer). */
RT_FN void rt_fx_overflow(const char *who, obj a, obj b) {
    core_die_head(who, "fixnum overflow", 0);
    rt_port_write_fd(rt_stderr, " ", 1);
    rt_put_fixnum(a, rt_stderr);
    rt_port_write_fd(rt_stderr, " ", 1);
    rt_put_fixnum(b, rt_stderr);
    rt_port_write_fd(rt_stderr, "\n", 1);
    rt_exit(70);
}
#else
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
RT_FN void rt_fx_overflow(const char *who, obj a, obj b) { rt_error(who, "fixnum overflow", rt_cons(a, rt_cons(b, RT_NIL))); }
#endif

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
/* ---- address tables: entries [a, b, value] keyed by one or two object addresses, open
 * addressing over a raw heap buffer (write's datum labels, equal? on cycles). The buffer is
 * reached from the caller's C stack, so it survives a collection; the keyed objects stay
 * reachable from the caller's arguments, and the collector never moves objects. ---- */
RT_FN intptr_t *rt_addrtab_get(rt_addrtab *t, obj a, obj b, int insert) {
    uintptr_t *e, h;
    intptr_t i;
    if (insert && 2 * (t->n + 1) > t->cap) { /* grow and rehash */
        rt_addrtab o = *t;
        t->cap = o.cap ? 2 * o.cap : 64;
        t->n = 0;
        t->e = (uintptr_t *)rt_alloc_raw(3 * sizeof(uintptr_t) * (size_t)t->cap);
        rt_memset(t->e, 0, 3 * sizeof(uintptr_t) * (size_t)t->cap);
        for (i = 0; i < o.cap; i++)
            if (o.e[3 * i]) *rt_addrtab_get(t, RT_O(o.e[3 * i]), RT_O(o.e[3 * i + 1]), 1) = (intptr_t)o.e[3 * i + 2];
    }
    if (!t->cap) return 0;
    h = (RT_W(a) * 0x9E3779B97F4A7C15u) ^ (RT_W(b) * 0xC2B2AE3D27D4EB4Fu);
    for (i = (intptr_t)(h >> 24) & (t->cap - 1);; i = (i + 1) & (t->cap - 1)) {
        e = t->e + 3 * i;
        if (e[0] == RT_W(a) && e[1] == RT_W(b)) return (intptr_t *)&e[2];
        if (!e[0]) {
            if (!insert) return 0;
            e[0] = RT_W(a);
            e[1] = RT_W(b);
            t->n++;
            return (intptr_t *)&e[2];
        }
    }
}

/* equal? terminates on circular structures (R7RS 6.1). One pass: plain recursion (on cars and
 * vector elements; iterating on cdrs and last elements) while a budget of CORE_EQ_BUDGET pairs and
 * vectors lasts and nesting stays below CORE_EQ_DEPTH; from then on (in place, nothing is
 * redone) every pair of compound objects entered is recorded in an address table and a pair met
 * again counts as equal (a bisimulation: the greatest fixed point), which terminates because the
 * table only grows. Large acyclic data and DAGs stay on the fast path (M4-2: the M5 version
 * restarted from scratch and recorded everything once a deep car chain was seen). */
#define CORE_EQ_BUDGET 100000
#define CORE_EQ_DEPTH 10000
typedef struct { intptr_t budget; int hashing; rt_addrtab seen; } core_eq;
static int core_equal(obj a, obj b, core_eq *q, int depth) {
    for (;;) {
        if (rt_eqv(a, b)) return 1;
        if (rt_is_pair(a) ? !rt_is_pair(b) : !rt_is_heap(a) || !rt_is_heap(b) || RT_HTYPE(a) != RT_HTYPE(b)) return 0;
        if (rt_is_pair(a) || RT_HTYPE(a) == RT_T_VECTOR) {
            if (!q->hashing && (--q->budget < 0 || depth > CORE_EQ_DEPTH)) q->hashing = 1;
            if (q->hashing) {
                intptr_t *v = rt_addrtab_get(&q->seen, a, b, 1);
                if (*v) return 1;
                *v = 1;
            }
        }
        if (rt_is_pair(a)) {
            if (!core_equal(RT_CAR(a), RT_CAR(b), q, depth + 1)) return 0;
            a = RT_CDR(a); b = RT_CDR(b);
            continue;
        }
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
            for (i = 0; i < n - 1; i++) if (!core_equal(RT_VEC(a)[i], RT_VEC(b)[i], q, depth + 1)) return 0;
            a = RT_VEC(a)[n - 1]; b = RT_VEC(b)[n - 1];
            continue;
        }
        default: return 0;
        }
    }
}
RT_FN int rt_equal(obj a, obj b) {
    core_eq q;
    q.budget = CORE_EQ_BUDGET;
    q.hashing = 0;
    q.seen.e = 0;
    q.seen.cap = q.seen.n = 0;
    return core_equal(a, b, &q, 0);
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
/* The table lives in OS memory, registered as a dynamic root range [core_symtab, core_symend):
 * it must not be a heap object, because it may grow while the REPL front end allocates in
 * unscanned immortal mode, and then the mortal symbols in it would not be marked. */
static void core_symadd(obj sym);
static obj *core_symslot(const char *s, size_t n) {
    intptr_t mask, i;
    if (!core_symtab) { /* first use: create the table and enter the program's static symbols */
        if (!(core_symtab = (obj *)rt_sys_mem(256 * sizeof(obj)))) rt_error(0, "out of memory", RT_NIL);
        core_symend = core_symtab + 256;
#if defined(SHIKRA_PROGRAM) && RT_NSTATIC_SYMS > 0
        for (mask = 0; mask < RT_NSTATIC_SYMS; mask++) core_symadd(rt_static_syms[mask]);
#endif
    }
    mask = core_symend - core_symtab - 1, i = (intptr_t)(core_hash(s, n) & (uintptr_t)mask);
    for (;; i = (i + 1) & mask) {
        obj *slot = &core_symtab[i], name;
        if (!*slot) return slot;
        name = RT_PTR(*slot, rt_symbol)->name;
        if ((size_t)RT_STR(name)->bytes == n && rt_memcmp(RT_STR(name)->buf, s, n) == 0) return slot;
    }
}
static void core_symadd(obj sym) {
    obj name = RT_PTR(sym, rt_symbol)->name;
    intptr_t cap = core_symend - core_symtab;
    if (2 * (core_symcount + 1) > cap) { /* grow and rehash */
        obj *old = core_symtab, *t;
        intptr_t i;
        if (!(t = (obj *)rt_sys_mem(2 * (size_t)cap * sizeof(obj)))) rt_error(0, "out of memory", RT_NIL);
        core_symtab = t;
        core_symend = t + 2 * cap;
        for (i = 0; i < cap; i++)
            if (old[i]) {
                obj nm = RT_PTR(old[i], rt_symbol)->name;
                *core_symslot(RT_STR(nm)->buf, (size_t)RT_STR(nm)->bytes) = old[i];
            }
        rt_sys_free(old, (size_t)cap * sizeof(obj));
    }
    *core_symslot(RT_STR(name)->buf, (size_t)RT_STR(name)->bytes) = sym;
    core_symcount++;
}
RT_FN obj rt_intern(const char *s, size_t n) {
    obj *slot = core_symslot(s, n), name, sym;
    if (*slot) return *slot;
    name = rt_make_string_n(s, n);
    RT_SETFLAGS(name, RT_HFLAGS(name) | RT_F_RO);
    sym = rt_make_symbol(name);
    core_symadd(sym);
    return sym;
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
RT_FN intptr_t rt_check_range(obj start, obj end, intptr_t len, intptr_t *e, const char *who) {
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
    s = rt_check_range(start, end, RT_HLEN(v), &e, "vector->list");
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
    s = rt_check_range(start, end, RT_HLEN(bv), &e, "bytevector-copy");
    r = core_bytevector_n(e - s);
    rt_memcpy(RT_PTR(r, rt_bytevector)->b, RT_PTR(bv, rt_bytevector)->b + s, (size_t)(e - s));
    return r;
}
RT_FN obj rt_bytevector_copy_x(obj to, obj at, obj from, obj start, obj end) {
    intptr_t s, e, k;
    RT_CHECK(rt_is_bytevector(from), "bytevector-copy!", "bytevector", from);
    RT_CHECK(rt_is_bytevector(to) && !(RT_HFLAGS(to) & RT_F_RO), "bytevector-copy!", "mutable bytevector", to);
    s = rt_check_range(start, end, RT_HLEN(from), &e, "bytevector-copy!");
    k = rt_is_fix(at) ? RT_FIXVAL(at) : -1;
    if (k < 0 || k > RT_HLEN(to) - (e - s)) rt_range_error("bytevector-copy!", at);
    rt_memmove(RT_PTR(to, rt_bytevector)->b + k, RT_PTR(from, rt_bytevector)->b + s, (size_t)(e - s));
    return RT_UNSPEC;
}
RT_FN obj rt_make_box(obj x) {
    rt_vector *b = (rt_vector *)rt_alloc(RT_HDR(RT_T_BOX, 0, 1), 2 * sizeof(obj));
    b->v[0] = x;
    return RT_TAGPTR(b);
}

/* ---- promises: [state], state = (done? . value) or (#f . thunk) (R7RS 4.2.5) ---- */
RT_FN obj rt_make_promise(obj done, obj value) {
    obj st = rt_cons(done, value);
    rt_vector *p = (rt_vector *)rt_alloc(RT_HDR(RT_T_PROMISE, 0, 1), 2 * sizeof(obj));
    p->v[0] = st;
    return RT_TAGPTR(p);
}
RT_FN obj rt_promise_p(obj x) { return RT_BOOL(rt_is_type(x, RT_T_PROMISE)); }
RT_FN obj rt_promise_state(obj p) { RT_CHECK(rt_is_type(p, RT_T_PROMISE), "force", "promise", p); return RT_VEC(p)[0]; }
RT_FN obj rt_promise_set_state(obj p, obj st) {
    RT_CHECK(rt_is_type(p, RT_T_PROMISE), "force", "promise", p);
    RT_VEC(p)[0] = st;
    return RT_UNSPEC;
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

/* ---- compiled calling convention ---- */
RT_FN void rt_args_reserve(intptr_t n) {
    if (n > core_argcap) { /* the old buffer stays a root range: clear it */
        obj *b = (obj *)rt_sys_mem(sizeof(obj) * (size_t)(2 * n));
        if (!b) rt_error(0, "out of memory", RT_NIL);
        rt_gc_add_roots(b, b + 2 * n);
        rt_memset(rt_args, 0, sizeof(obj) * (size_t)core_argcap);
        rt_args = b;
        core_argcap = 2 * n;
    }
}
RT_FN obj rt_rest_list(intptr_t argc, intptr_t nreq, obj a0, obj a1, obj a2, obj a3) {
    obj r = RT_NIL;
    intptr_t i;
    for (i = argc - 1; i >= nreq; i--) r = rt_cons(i == 0 ? a0 : i == 1 ? a1 : i == 2 ? a2 : i == 3 ? a3 : rt_args[i - 4], r);
    return r;
}
#ifdef RT_NO_HANDLERS
RT_FN void rt_arity_error(const char *who, intptr_t argc) { CORE_DIE1(who, "wrong number of arguments", 0, RT_FIX(argc)); }
RT_FN void rt_not_procedure(obj f) { CORE_DIE1(0, "not a procedure", 0, f); }
#else
RT_FN void rt_arity_error(const char *who, intptr_t argc) {
    rt_error(who, "wrong number of arguments", rt_cons(RT_FIX(argc), RT_NIL));
}
RT_FN void rt_not_procedure(obj f) { rt_error(0, "not a procedure", rt_cons(f, RT_NIL)); }
#endif
RT_FN void rt_undefined_error(const char *name) { rt_error(name, "variable used before its definition", RT_NIL); }
RT_FN intptr_t rt_spread(obj args, obj *regs) {
    intptr_t n = rt_list_length(args, "apply"), i;
    if (n > 4) rt_args_reserve(n - 4);
    for (i = 0; i < n; i++, args = RT_CDR(args))
        if (i < 4) regs[i] = RT_CAR(args);
        else rt_args[i - 4] = RT_CAR(args);
    for (; i < 4; i++) regs[i] = RT_FALSE;
    return n;
}
RT_FN intptr_t rt_spread_values(obj v, obj *regs) {
    intptr_t n, i;
    if (!rt_is_type(v, RT_T_VALUES)) { regs[0] = v; regs[1] = regs[2] = regs[3] = RT_FALSE; return 1; }
    n = RT_HLEN(v);
    if (n > 4) rt_args_reserve(n - 4);
    for (i = 0; i < n; i++)
        if (i < 4) regs[i] = RT_VEC(v)[i];
        else rt_args[i - 4] = RT_VEC(v)[i];
    for (; i < 4; i++) regs[i] = RT_FALSE;
    return n;
}
RT_FN obj rt_append2(obj a, obj b) {
    obj rev = RT_NIL;
    rt_list_length(a, "append");
    for (; rt_is_pair(a); a = RT_CDR(a)) rev = rt_cons(RT_CAR(a), rev);
    for (; rt_is_pair(rev); rev = RT_CDR(rev)) b = rt_cons(RT_CAR(rev), b);
    return b;
}
RT_FN obj rt_uncaught_o(obj e) { rt_uncaught(e); }

/* ---- multiple values into a known receiver (call-with-values fallback) ---- */
static intptr_t core_nvalues(obj v) { return rt_is_type(v, RT_T_VALUES) ? RT_HLEN(v) : 1; }
RT_FN obj rt_values_check(obj v, obj nreq, obj rest) {
    intptr_t n = core_nvalues(v);
    if (rest == RT_FALSE ? n != RT_FIXVAL(nreq) : n < RT_FIXVAL(nreq))
        rt_error("call-with-values", "wrong number of values for the receiver", rt_cons(RT_FIX(n), RT_NIL));
    return RT_UNSPEC;
}
RT_FN obj rt_values_ref(obj v, obj i) { return rt_is_type(v, RT_T_VALUES) ? RT_VEC(v)[RT_FIXVAL(i)] : v; }
RT_FN obj rt_values_tail(obj v, obj i) {
    obj r = RT_NIL;
    intptr_t k;
    for (k = core_nvalues(v) - 1; k >= RT_FIXVAL(i); k--) r = rt_cons(rt_values_ref(v, RT_FIX(k)), r);
    return r;
}

/* ---- dynamic state: the winders list and the handler stack (spec §8.3, §8.4) ---- */
RT_FN obj rt_winders_get(void) { return rt_winders; }
RT_FN obj rt_winders_set(obj w) { rt_winders = w; return RT_UNSPEC; }
RT_FN obj rt_handlers_get(void) { return rt_handlers; }
RT_FN obj rt_handlers_set(obj h) { rt_handlers = h; return RT_UNSPEC; }
RT_FN obj rt_parameter_binding(obj cell) {
    obj w;
    RT_CHECK(rt_is_pair(cell), "parameter", "parameter cell", cell);
    for (w = rt_winders; rt_is_pair(w); w = RT_CDR(w))
        if (RT_CAR(RT_CAR(w)) == cell) return RT_CAR(w);
    return cell;
}

/* ---- the compiled engine's escapes: %call/cc, tiers 2 and 3 (spec §8.2) ----
 * A capture is a core_cont frame on the C stack (jump buffer, saved winders and handler
 * lists) plus a heap procedure k whose free[0] points at the frame (a raw word that looks
 * like a fixnum) while the frame is live on the current stack, else #f. core_conts lists the
 * live frames, innermost first, so a jump can mark every frame it discards as dead. Invoking
 * a live k first walks the winders back to the capture's (rt_engine_travel: after/before
 * thunks), so base's call/cc needs no escape procedure in compiled code (M4-1). A tier-2 k
 * (1 free slot) can only be invoked while live: the compiler proved it (escape analysis), and
 * a dead one raises (safe-mode guard). A tier-3 k (RT_TIER3, rt_call_cc3) also keeps the frame
 * address (free[1]) and a heap copy of the C stack from its frame to the stack base (free[2]):
 * invoked when dead, it is re-entered (DESIGN.md "Tier 3 continuations as built"). */
#ifdef SHIKRA_PROGRAM
typedef struct core_cont { rt_jmpbuf jb; struct core_cont *prev; obj winders, handlers, f, k, val; } core_cont;
static core_cont *core_conts;
#ifdef RT_TIER3
static RT_NORETURN void core_reenter(obj self, obj v);
#define CORE_DEAD(self, v) do { if ((RT_PROC(self)->hdr >> 16) > 1) core_reenter(self, v); \
        rt_error(0, "tier-2 continuation called after its extent (bug)", RT_NIL); } while (0)
#else
#define CORE_DEAD(self, v) rt_error(0, "tier-2 continuation called after its extent (bug)", RT_NIL)
#endif
static obj core_cont_code(obj self, intptr_t argc, obj a0, obj a1, obj a2, obj a3) {
    core_cont *c = (core_cont *)(void *)RT_PROC(self)->free[0], *t;
    obj v = argc == 1 ? a0 : rt_make_values(rt_rest_list(argc, 0, a0, a1, a2, a3));
    if ((obj)(void *)c == RT_FALSE) CORE_DEAD(self, v);
    if (c->winders != rt_winders) { /* after thunks may escape, or end the capture's extent */
        rt_engine_travel(c->winders);
        if ((obj)(void *)(c = (core_cont *)(void *)RT_PROC(self)->free[0]) == RT_FALSE) CORE_DEAD(self, v);
    }
    for (t = core_conts; t != c; t = t->prev) RT_PROC(t->k)->free[0] = RT_FALSE;
    core_conts = c;
    c->val = v;
    rt_winders = c->winders;
    rt_handlers = c->handlers;
    rt_sys_throw(&c->jb);
}
static void core_callcc_run(void *p) {
    core_cont *c = (core_cont *)p;
    c->val = RT_NT(RT_PROC(c->f)->code(c->f, 1, c->k, 0, 0, 0));
}
RT_FN obj rt_call_cc(obj f) {
    core_cont c;
    RT_CALLABLE(f);
    c.prev = core_conts;
    c.winders = rt_winders;
    c.handlers = rt_handlers;
    c.f = f;
    c.val = RT_UNSPEC;
    c.k = rt_make_proc(core_cont_code, 1);
    RT_PROC(c.k)->free[0] = (obj)(void *)&c;
    core_conts = &c;
    rt_sys_catch(&c.jb, core_callcc_run, &c);
    core_conts = c.prev;
    RT_PROC(c.k)->free[0] = RT_FALSE;
    return c.val;
}
#ifdef RT_TIER3
/* Tier 3. The stack grows down on every target (checked at each capture). The copy is a heap
 * object scanned conservatively, like the stack (rt_gc.c); lo is the address it came from. */
typedef struct { uintptr_t hdr; char *lo; uintptr_t w[]; } core_stack;
static RT_NOINLINE void core_callcc3_run(void *p) { /* never inlined into rt_sys_catch (GCC clones it) */
    core_cont *c = (core_cont *)p;
    char *lo = (char *)((uintptr_t)&p & ~(uintptr_t)15); /* in this frame: below rt_sys_catch's */
    size_t n = (size_t)(core_base - lo) & ~(size_t)7;
    core_stack *s;
    if (lo >= core_base || (char *)(c + 1) > core_base) /* the stack must grow down, from below the base */
        rt_error("call/cc", "cannot copy the C stack for a re-entrant continuation", RT_NIL);
    s = (core_stack *)rt_alloc(RT_HDR(RT_T_CONT, 0, sizeof(char *) + n), sizeof(core_stack) + n);
    s->lo = lo;
    rt_memcpy(s->w, lo, n); /* the jump buffer (registers) is in c, inside the copy */
    RT_PROC(c->k)->free[2] = RT_TAGPTR(s);
    c->val = RT_NT(RT_PROC(c->f)->code(c->f, 1, c->k, 0, 0, 0));
}
RT_FN obj rt_call_cc3(obj f) {
    core_cont c;
    RT_CALLABLE(f);
    c.prev = core_conts;
    c.winders = rt_winders;
    c.handlers = rt_handlers;
    c.f = f;
    c.val = RT_UNSPEC;
    c.k = rt_make_proc(core_cont_code, 3);
    RT_PROC(c.k)->free[0] = RT_PROC(c.k)->free[1] = (obj)(void *)&c;
    core_conts = &c;
    /* re-entered (or escaped to) with other winders: the before thunks run here, on the restored stack */
    if (rt_sys_catch(&c.jb, core_callcc3_run, &c) && c.winders != rt_winders) rt_engine_travel(c.winders);
    core_conts = c.prev;
    RT_PROC(c.k)->free[0] = RT_FALSE;
    return c.val;
}
/* Copy the stack back: the caller made room, so this frame lies below the copy's region. The
 * frames of the current stack die, those of the copy (tier-2 frames included) come back to
 * life, and the capture's rt_sys_catch returns 1. core_never keeps compilers from treating
 * this as noreturn, so the call below stays a call (a tail call would free the room). */
static volatile int core_never;
static RT_NOINLINE void core_rewind(obj self, obj v) {
    core_stack *s = RT_PTR(RT_PROC(self)->free[2], core_stack);
    core_cont *c, *t;
    if ((uintptr_t)&s >= (uintptr_t)s->lo) rt_sys_exit(70); /* cannot happen: room was made */
    for (t = core_conts; t; t = t->prev) RT_PROC(t->k)->free[0] = RT_FALSE;
    rt_memcpy(s->lo, s->w, (size_t)RT_HLEN(RT_TAGPTR(s)) - sizeof(char *));
    core_conts = c = (core_cont *)(void *)RT_PROC(self)->free[1];
    for (t = c; t; t = t->prev) RT_PROC(t->k)->free[0] = (obj)(void *)t;
    c->val = v;
    rt_handlers = c->handlers; /* rt_winders stays the common tail: rt_call_cc3 rewinds */
    if (!core_never) rt_sys_throw(&c->jb);
}
/* Re-entry: the after thunks run on the current stack, down to the winders the current and
 * the captured list share (R7RS 6.10; the frame image in the copy holds the captured lists),
 * then a variable-length array moves the stack pointer below the copy's region, if needed. */
static void core_reenter(obj self, obj v) {
    core_stack *s = RT_PTR(RT_PROC(self)->free[2], core_stack);
    core_cont *img = (core_cont *)(void *)((char *)s->w + ((char *)RT_PROC(self)->free[1] - s->lo));
    obj a = rt_winders, b = img->winders;
    intptr_t na = RT_FIXVAL(rt_length(a)), nb = RT_FIXVAL(rt_length(b));
    for (; na > nb; na--) a = RT_CDR(a);
    for (; nb > na; nb--) b = RT_CDR(b);
    for (; a != b; a = RT_CDR(a)) b = RT_CDR(b);
    if (a != rt_winders) rt_engine_travel(a);
    {
        volatile char room[(uintptr_t)&a > (uintptr_t)s->lo - 4096 ? (uintptr_t)&a - (uintptr_t)s->lo + 4096 : 1];
        room[0] = 0;
        core_rewind(self, v);
        if (room[0]) rt_sys_exit(70);
    }
    rt_sys_exit(70); /* not reached */
}
#endif
#endif
RT_FN obj rt_continuation_live_p(obj k) {
#ifdef SHIKRA_PROGRAM
    if (rt_is_proc(k) && RT_PROC(k)->code == core_cont_code) return RT_FALSE; /* it walks the winders itself */
#endif
    (void)k;
    return RT_TRUE;
}
