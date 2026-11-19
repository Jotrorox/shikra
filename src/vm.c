/* vm.c -- the interpreter's virtual machine (spec §5.6, DESIGN.md §11 M3).
 *
 * Its own value stack: Scheme calls never recurse on the C stack, tail calls reuse the frame,
 * and a continuation is a copy of the stack. Frame of a call (fp points at the first
 * argument; locals follow, temporaries above them):
 *
 *     fp[-4] caller's fp (fixnum: index into the stack)
 *     fp[-3] caller's pc (fixnum: offset into the caller's code)
 *     fp[-2] caller's template (raw pointer; 8-aligned, so it looks like a fixnum)
 *     fp[-1] the procedure being run (its closure: captured values)
 *     fp[0..nlocals) parameters, then let/letrec* variables
 *
 * Every stack slot below sp holds an obj or a fixnum-looking word, so the stack is scanned
 * precisely (a dynamic root range [stk, vm_sp)) and continuations copy it into a vector.
 * Primitives come from prims.def. A primitive's error unwinds through rt_raise_hook to
 * vm_run, which calls (scheme base)'s raise on top of the faulting frame; raise and
 * everything else of the control library is shared with compiled programs. */
#include "interp.h"
#include <stdlib.h>
#include <string.h>

#define SLACK 16                    /* always free above a frame: the raise call of an error */
#define MAXSTACK ((size_t)1 << 25)  /* slots: 256 MB */

void (*vm_abort)(void);
library *vm_interaction;
static int vm_repl;
static obj *stk, *stk_end;          /* the stack */
static obj *vm_sp = 0, *vm_fp;      /* saved registers (SYNC) */
static int32_t *vm_pc;
static tmpl *vm_t;
static obj vm_err = RT_FALSE;       /* the error object travelling from the raise hook */
static rt_jmpbuf *vm_jb;
static obj env_rtd = RT_FALSE;
static obj raise_cell;
static int32_t halt_code[] = {OP_HALT};
static tmpl halt_t;
static void (*saved_raise_hook)(obj);
static void (*saved_cerror_hook)(const char *, const char *);

typedef void (*vfn)(void);
static vfn prims[NPRIMS] = {
#define PRIM(name, who, cfn, arity, args, res, flags) (vfn)cfn,
#include "prims.def"
#undef PRIM
};

/* Procedure kinds, told apart by the code word. Neither is ever called from C. */
static obj vm_closure_code(obj self, intptr_t argc, obj a0, obj a1, obj a2, obj a3) {
    (void)self; (void)argc; (void)a0; (void)a1; (void)a2; (void)a3;
    rt_error(0, "interpreted procedure called from C", RT_NIL);
}
static obj vm_cont_code(obj self, intptr_t argc, obj a0, obj a1, obj a2, obj a3) {
    (void)self; (void)argc; (void)a0; (void)a1; (void)a2; (void)a3;
    rt_error(0, "interpreter continuation called from C", RT_NIL);
}
static const char *symname(obj s) { return RT_STR(RT_PTR(s, rt_symbol)->name)->buf; }

/* ---- uncaught errors ---- */
static void restore_hooks(void) { rt_raise_hook = saved_raise_hook; cerror_hook = saved_cerror_hook; }
static RT_NORETURN void uncaught(obj e) {
    if (!vm_repl) rt_uncaught(e); /* prints, flushes, exits 70 like a compiled program */
    if (RT_PORT(rt_stdout)->col) rt_port_putc(rt_stdout, '\n');
    rt_print_error(e);
    vm_abort();
    rt_exit(70);
}
static obj vm_uncaught_prim(obj e) { uncaught(e); }
void vm_reset(void) {
    vm_sp = vm_fp = stk;
    vm_err = RT_FALSE;
    restore_hooks();
}

/* ---- the stack ---- */
static void grow(size_t need) { /* room for need more slots above vm_sp */
    size_t used = (size_t)(vm_sp - stk), fpo = (size_t)(vm_fp - stk), cap = (size_t)(stk_end - stk), n = cap;
    obj *s;
    while (n - used < need) n *= 2;
    if (n > MAXSTACK) { /* not catchable: there is no room left to run a handler */
        rt_flush(rt_stdout);
        rt_port_puts(rt_stderr, "Error: stack overflow (interpreter stack exceeds 256 MB)\n");
        vm_sp = stk;
        if (vm_repl) { restore_hooks(); rt_winders = rt_handlers = RT_NIL; vm_abort(); }
        rt_exit(70);
    }
    if (!(s = (obj *)realloc(stk, n * sizeof(obj)))) die("out of memory");
    stk = s;
    stk_end = s + n;
    vm_sp = s + used;
    vm_fp = s + fpo;
}

/* ---- environments: records whose one field is a library* (8-aligned: a fixnum to the GC) */
static obj vm_make_env(library *lib) {
    obj r;
    if (env_rtd == RT_FALSE) {
        rt_gc_set_immortal(IMM_PURE);
        env_rtd = rt_make_rtd(sym("environment"), rt_vector_n(1, sym("library")));
        rt_gc_set_immortal(0);
    }
    r = rt_make_record(env_rtd);
    RT_VEC(r)[1] = (obj)(void *)lib;
    return r;
}
/* (import (scheme base) ...): the REPL's default environment (DESIGN.md §11 M3). */
obj vm_default_import_form(void) {
    static const char *const defaults[] = {"base", "case-lambda", "cxr", "write", "process-context", "eval", "load", "repl", 0};
    obj form = RT_NIL;
    int i;
    for (i = 0; defaults[i]; i++) form = rt_cons(L2(sym("scheme"), sym(defaults[i])), form);
    return rt_cons(sym("import"), rt_reverse(form));
}

/* An immortal copy of a datum handed to the front end (eval, load): the expander keeps
 * pointers to it in C memory the collector does not see. */
static obj icopy(obj x) {
    if (rt_is_pair(x)) {
        obj head = rt_cons(icopy(RT_CAR(x)), RT_NIL), last = head;
        for (x = RT_CDR(x); rt_is_pair(x); x = RT_CDR(x)) last = RT_CDR(last) = rt_cons(icopy(RT_CAR(x)), RT_NIL);
        RT_CDR(last) = icopy(x);
        return head;
    }
    if (rt_is_vector(x)) {
        intptr_t i, n = RT_HLEN(x);
        obj v = rt_vector_n(n, RT_FALSE);
        for (i = 0; i < n; i++) RT_VEC(v)[i] = icopy(RT_VEC(x)[i]);
        return v;
    }
    if (rt_is_string(x)) return rt_make_string_n(RT_STR(x)->buf, (size_t)RT_STR(x)->bytes);
    if (rt_is_flo(x)) return rt_make_flonum(RT_FLO(x));
    if (rt_is_bytevector(x)) return rt_bytevector_copy(x, RT_FIX(0), RT_FIX(RT_HLEN(x)));
    return x;
}

static obj closure0(tmpl *t) {
    obj f = rt_make_proc(vm_closure_code, 1);
    RT_PROC(f)->free[0] = (obj)(void *)t;
    return f;
}
static library *env_lib(obj env, const char *who) {
    if (!rt_is_type(env, RT_T_RECORD) || RT_VEC(env)[0] != env_rtd) rt_type_error(who, "environment", env);
    return (library *)(void *)RT_VEC(env)[1];
}
/* Front-end work from inside the VM: immortal allocation, source locations on; errors
 * (cerror_hook, rt_raise_hook) become Scheme errors raised at the instruction. */
static void front_begin(void) { rt_gc_set_immortal(IMM_PURE); rt_srcloc_enable(1); loc_ctx = RT_FALSE; }
static obj rooted_copy(obj x) { /* the copy may hold collectable objects (procedures...) */
    rt_gc_set_immortal(IMM_ROOTS);
    x = icopy(x);
    rt_gc_set_immortal(IMM_PURE);
    return x;
}
static void front_end(void) { rt_srcloc_enable(0); rt_gc_set_immortal(0); }
static obj eval_closure(obj expr, obj env) {
    library *lib = env_lib(env, "eval");
    obj forms = RT_NIL;
    tmpl *t;
    front_begin();
    expr = rooted_copy(expr);
    if (rt_is_pair(expr) && RT_CAR(expr) == sym("import")) front_repl_import(lib, expr);
    else forms = front_repl_expand(lib, expr);
    t = bc_unit(forms); /* also the bodies of libraries this loaded */
    front_end();
    return closure0(t);
}
static obj environment(obj specs) {
    library *lib;
    rt_list_length(specs, "environment");
    front_begin();
    lib = front_repl_new();
    front_repl_import(lib, rt_cons(sym("import"), rooted_copy(specs)));
    front_end();
    return vm_make_env(lib); /* its libraries' bodies run with the next unit (eval) */
}
static obj interaction(void) {
    if (!vm_interaction) {
        front_begin();
        vm_interaction = front_repl_new();
        front_repl_import(vm_interaction, vm_default_import_form());
        front_end();
    }
    return vm_make_env(vm_interaction);
}

/* Value after a continuation or values: one value is itself. */
static obj snapshot(obj *base, obj fpw, obj pcw, obj tw) {
    intptr_t n = base - stk, i;
    obj v = rt_vector_n(n + 5, RT_FALSE), k;
    RT_VEC(v)[0] = fpw;
    RT_VEC(v)[1] = pcw;
    RT_VEC(v)[2] = tw;
    RT_VEC(v)[3] = rt_winders;
    RT_VEC(v)[4] = rt_handlers;
    for (i = 0; i < n; i++) RT_VEC(v)[5 + i] = stk[i];
    k = rt_make_proc(vm_cont_code, 1);
    RT_PROC(k)->free[0] = v;
    return k;
}

/* ---- the dispatch loop ---- */
#define SYNC() (vm_sp = sp, vm_fp = fp, vm_pc = pc, vm_t = t)
#define GROW(need) do { if ((size_t)(stk_end - sp) < (size_t)(need)) { SYNC(); grow(need); sp = vm_sp; fp = vm_fp; } } while (0)
typedef obj (*f0)(void);
typedef obj (*f1)(obj);
typedef obj (*f2)(obj, obj);
typedef obj (*f3)(obj, obj, obj);
typedef obj (*f4)(obj, obj, obj, obj);
typedef obj (*f5)(obj, obj, obj, obj, obj);

/* argc >= 0: enter the procedure at vm_fp[-1] with argc arguments; else resume at vm_pc. */
static obj run(intptr_t argc) {
    obj *sp = vm_sp, *fp = vm_fp, *k = 0, acc = RT_UNSPEC, f;
    int32_t *pc = vm_pc;
    tmpl *t = vm_t;
    intptr_t n, i, nargs = 0;
    if (argc >= 0) goto enter;
    k = t->k;
    for (;;) {
        switch (*pc++) {
        case OP_CONST: acc = k[*pc++]; break;
        case OP_LREF: acc = fp[*pc++]; break;
        case OP_LSET: fp[*pc++] = acc; break;
        case OP_FREF: acc = RT_PROC(fp[-1])->free[1 + *pc++]; break;
        case OP_LBREF: acc = RT_VEC(fp[*pc++])[0]; break;
        case OP_FBREF: acc = RT_VEC(RT_PROC(fp[-1])->free[1 + *pc++])[0]; break;
        case OP_LBREFC: case OP_FBREFC:
            acc = RT_VEC(pc[-1] == OP_LBREFC ? fp[pc[0]] : RT_PROC(fp[-1])->free[1 + pc[0]])[0];
            pc += 2;
            if (acc == RT_UNDEF) { SYNC(); rt_undefined_error(symname(k[pc[-1]])); }
            break;
        case OP_LBSET: RT_VEC(fp[*pc++])[0] = acc; acc = RT_UNSPEC; break;
        case OP_FBSET: RT_VEC(RT_PROC(fp[-1])->free[1 + *pc++])[0] = acc; acc = RT_UNSPEC; break;
        case OP_BOX: SYNC(); acc = rt_make_box(acc); break;
        case OP_MKBOX: SYNC(); f = rt_make_box(RT_UNDEF); fp[*pc++] = f; break;
        case OP_GREF: {
            obj c = k[*pc++];
            acc = RT_VEC(c)[0];
            if (acc == RT_UNDEF) {
                SYNC();
                if (RT_VEC(c)[2] != RT_FALSE) rt_undefined_error(symname(RT_VEC(c)[1]));
                rt_error(symname(RT_VEC(c)[1]), "unbound variable", RT_NIL);
            }
            break;
        }
        case OP_GSET: {
            obj c = k[*pc++];
            if (RT_VEC(c)[0] == RT_UNDEF && RT_VEC(c)[2] == RT_FALSE) { SYNC(); rt_error(symname(RT_VEC(c)[1]), "unbound variable", RT_NIL); }
            RT_VEC(c)[0] = acc;
            acc = RT_UNSPEC;
            break;
        }
        case OP_GDEF: {
            obj c = k[*pc++];
            RT_VEC(c)[0] = acc;
            RT_VEC(c)[2] = RT_TRUE;
            acc = RT_UNSPEC;
            break;
        }
        case OP_PUSH: *sp++ = acc; break;
        case OP_JMP: pc = t->code + *pc; break;
        case OP_JMPF: if (acc == RT_FALSE) pc = t->code + *pc; else pc++; break;
        case OP_CLOSURE: {
            tmpl *c = (tmpl *)(void *)k[pc[0]];
            n = pc[1];
            pc += 2;
            SYNC();
            f = rt_make_proc(vm_closure_code, n + 1);
            RT_PROC(f)->free[0] = (obj)(void *)c;
            for (i = 0; i < n; i++) RT_PROC(f)->free[1 + i] = sp[i - n];
            sp -= n;
            acc = f;
            break;
        }
        case OP_FIXCL:
            f = fp[pc[0]];
            if (rt_is_type(f, RT_T_BOX)) f = RT_VEC(f)[0];
            RT_PROC(f)->free[1 + pc[1]] = fp[pc[2]];
            pc += 3;
            break;
        case OP_FRAME: sp[0] = sp[1] = sp[2] = RT_FIX(0); sp += 3; break;
        case OP_CALL: n = *pc++; goto call;
        case OP_CALLV: n = nargs;
        call:
            sp[-n - 4] = RT_FIX(fp - stk);
            sp[-n - 3] = RT_FIX(pc - t->code);
            sp[-n - 2] = (obj)(void *)t;
            fp = sp - n;
            argc = n;
            goto enter;
        case OP_TCALL: n = *pc++; goto tcall;
        case OP_TCALLV: n = nargs;
        tcall:
            memmove(fp - 1, sp - n - 1, sizeof(obj) * (size_t)(n + 1));
            sp = fp + n;
            argc = n;
            goto enter;
        case OP_RET: {
            obj *b = fp - 4;
            t = (tmpl *)(void *)b[2];
            pc = t->code + RT_FIXVAL(b[1]);
            fp = stk + RT_FIXVAL(b[0]);
            sp = b;
            k = t->k;
            break;
        }
        case OP_PRIM: {
            vfn p = prims[pc[0]];
            n = pc[1];
            pc += 2;
            SYNC();
            switch (n) {
            case 0: acc = ((f0)p)(); break;
            case 1: acc = ((f1)p)(acc); break;
            case 2: acc = ((f2)p)(sp[-1], acc); break;
            case 3: acc = ((f3)p)(sp[-2], sp[-1], acc); break;
            case 4: acc = ((f4)p)(sp[-3], sp[-2], sp[-1], acc); break;
            default: acc = ((f5)p)(sp[-4], sp[-3], sp[-2], sp[-1], acc); break;
            }
            if (n > 1) sp -= n - 1;
            break;
        }
        case OP_VALUES:
            n = *pc++;
            if (n == 1) break;
            if (n > 0) *sp++ = acc;
            SYNC();
            acc = rt_values_n(n, sp - n);
            sp -= n;
            break;
        case OP_SPREADL:
            SYNC();
            nargs = rt_list_length(acc, "apply");
            GROW(nargs + SLACK);
            for (; rt_is_pair(acc); acc = RT_CDR(acc)) *sp++ = RT_CAR(acc);
            break;
        case OP_SPREADV:
            if (rt_is_type(acc, RT_T_VALUES)) {
                nargs = RT_HLEN(acc);
                GROW(nargs + SLACK);
                for (i = 0; i < nargs; i++) *sp++ = RT_VEC(acc)[i];
            } else { *sp++ = acc; nargs = 1; }
            break;
        case OP_VCHECK:
            pc += 2;
            if ((rt_is_type(acc, RT_T_VALUES) ? RT_HLEN(acc) : 1) != pc[-2] || pc[-1]) {
                SYNC();
                rt_values_check(acc, RT_FIX(pc[-2]), RT_BOOL(pc[-1]));
            }
            break;
        case OP_CWVSWAP:
            f = sp[-1];
            sp[-1] = acc;
            sp[0] = sp[1] = sp[2] = RT_FIX(0);
            sp[3] = f;
            sp += 4;
            break;
        case OP_CALLCC:
            SYNC();
            f = snapshot(sp - 4, RT_FIX(fp - stk), RT_FIX(pc - t->code), (obj)(void *)t);
            *sp++ = f;
            n = 1;
            goto call;
        case OP_TCALLCC:
            SYNC();
            f = snapshot(fp - 4, fp[-4], fp[-3], fp[-2]);
            *sp++ = f;
            n = 1;
            goto tcall;
        case OP_EVAL: case OP_TEVAL:
            SYNC();
            f = eval_closure(sp[-1], acc);
            sp[-1] = f;
            n = 0;
            if (pc[-1] == OP_EVAL) goto call;
            goto tcall;
        case OP_ENV: SYNC(); acc = environment(acc); break;
        case OP_INTERENV: SYNC(); acc = interaction(); break;
        case OP_HALT: SYNC(); return acc;
        default: die("interpreter: bad opcode %d", (int)pc[-1]);
        }
        continue;
    enter: /* the callee is fp[-1], its argc arguments at fp[0..] */
        f = fp[-1];
        if (RT_UNLIKELY(!rt_is_proc(f))) { SYNC(); rt_not_procedure(f); }
        if (RT_PROC(f)->code == vm_cont_code) { /* restore the captured stack */
            obj v = argc == 1 ? fp[0] : (sp = fp + argc, SYNC(), rt_values_n(argc, fp)), s = RT_PROC(f)->free[0];
            n = RT_HLEN(s) - 5;
            sp = stk;
            fp = stk;
            GROW(n + SLACK);
            memcpy(stk, &RT_VEC(s)[5], sizeof(obj) * (size_t)n);
            sp = stk + n;
            fp = stk + RT_FIXVAL(RT_VEC(s)[0]);
            t = (tmpl *)(void *)RT_VEC(s)[2];
            pc = t->code + RT_FIXVAL(RT_VEC(s)[1]);
            k = t->k;
            rt_winders = RT_VEC(s)[3];
            rt_handlers = RT_VEC(s)[4];
            acc = v;
            continue;
        }
        {
            tmpl *c = (tmpl *)(void *)RT_PROC(f)->free[0];
            if (c->nclauses) {
                for (i = 0; i < c->nclauses; i++) {
                    tmpl *d = c->clauses[i];
                    if (d->rest ? argc >= d->nreq : argc == d->nreq) break;
                }
                if (i == c->nclauses) { sp = fp + argc; SYNC(); rt_arity_error(symname(c->name), argc); }
                c = c->clauses[i];
            } else if (c->rest ? argc < c->nreq : argc != c->nreq) { sp = fp + argc; SYNC(); rt_arity_error(symname(c->name), argc); }
            sp = fp + argc;
            GROW((argc > c->nlocals ? 0 : c->nlocals - argc) + c->maxstack + SLACK);
            if (c->rest) {
                obj r = RT_NIL;
                SYNC();
                for (i = argc - 1; i >= c->nreq; i--) r = rt_cons(fp[i], r);
                fp[c->nreq] = r;
                i = c->nreq + 1;
            } else i = argc;
            for (; i < c->nlocals; i++) fp[i] = RT_UNDEF;
            sp = fp + c->nlocals;
            t = c;
            pc = t->code;
            k = t->k;
        }
    }
}

/* ---- running a unit; errors ---- */
static obj raise_proc(void) { /* (scheme base)'s raise, once it is defined */
    if (!raise_cell) {
        library *l;
        for (l = front_order; l; l = l->next)
            if (!strcmp(l->id, "scheme/base")) {
                binding *b = (binding *)pm_get(&l->env->map, sym("raise"));
                if (b && b->kind == B_GLOBAL) {
                    rt_gc_set_immortal(IMM_PURE);
                    raise_cell = bc_cell(b->g);
                    rt_gc_set_immortal(0);
                }
            }
        if (!raise_cell) return 0;
    }
    return RT_VEC(raise_cell)[0] != RT_UNDEF ? RT_VEC(raise_cell)[0] : 0;
}
static void vm_hook(obj e) {
    vm_err = e;
    rt_gc_set_immortal(0);
    rt_srcloc_enable(0);
    front_repl_recover();
    rt_sys_throw(vm_jb);
}
static void vm_cerror(const char *loc, const char *msg) {
    obj m = rt_open_output_string();
    if (loc) { rt_port_puts(m, loc); rt_port_puts(m, ": "); }
    rt_port_puts(m, msg);
    rt_raise(rt_make_error(RT_ERR_GENERAL, rt_get_output_string(m), RT_NIL, RT_FALSE));
}
typedef struct { intptr_t argc; obj result; } runarg;
static void run_cb(void *p) { runarg *r = (runarg *)p; r->result = run(r->argc); }

obj vm_run(tmpl *top) {
    obj *sp;
    intptr_t argc = 0;
    saved_raise_hook = rt_raise_hook;
    saved_cerror_hook = cerror_hook;
    rt_raise_hook = vm_hook;
    cerror_hook = vm_cerror;
    rt_srcloc_enable(0);
    vm_sp = vm_fp = stk;
    vm_t = &halt_t;
    vm_pc = halt_code;
    grow(8 + SLACK);
    sp = stk;
    *sp++ = RT_FALSE;   /* the bottom frame's procedure (never read) */
    *sp++ = RT_FIX(1);  /* the top-level unit returns to OP_HALT */
    *sp++ = RT_FIX(0);
    *sp++ = (obj)(void *)&halt_t;
    *sp++ = closure0(top);
    vm_sp = vm_fp = sp;
    for (;;) {
        rt_jmpbuf jb;
        runarg ra;
        obj e, r;
        ra.argc = argc;
        vm_jb = &jb;
        if (!rt_sys_catch(&jb, run_cb, &ra)) {
            vm_sp = vm_fp = stk;
            restore_hooks();
            rt_srcloc_enable(1);
            return ra.result;
        }
        e = vm_err; /* call raise on top of the faulting frame; it returns (if at all) after it */
        vm_err = RT_FALSE;
        if (!(r = raise_proc())) uncaught(e);
        grow(5 + SLACK);
        sp = vm_sp;
        sp[0] = RT_FIX(vm_fp - stk);
        sp[1] = RT_FIX(vm_pc - vm_t->code);
        sp[2] = (obj)(void *)vm_t;
        sp[3] = r;
        sp[4] = e;
        vm_sp = sp + 5;
        vm_fp = sp + 4;
        argc = 1;
    }
}

void vm_init(int repl) {
    vm_repl = repl;
    halt_t.code = halt_code;
    halt_t.name = sym("halt");
    prims[PI_rt_uncaught_o] = (vfn)vm_uncaught_prim;
    stk = (obj *)xmalloc(sizeof(obj) * 16384);
    stk_end = stk + 16384;
    vm_sp = vm_fp = stk;
    rt_gc_add_dynamic_roots(&stk, &vm_sp);
    rt_gc_add_roots(&vm_err, &vm_err + 1);
    rt_gc_enable(1);
}
