/* bytecode.c -- core forms -> bytecode for the interpreter (spec §5.6, DESIGN.md §11 M3).
 *
 * Input: the expander's core forms (DESIGN.md §6). Every local is a unique uninterned symbol,
 * so variable analysis needs no scopes: a symbol occurrence is a reference to its binder.
 * Variables live in frame slots of the procedure that binds them; closures are flat (each
 * captures the values of its free variables). A variable is *boxed* (its slot holds a box)
 * if it is ever assigned, so that copies of the VM stack made by continuations share the
 * location; a letrec* variable is also boxed if it may be read before its initialisation
 * ("early"; reads are then checked). letrec* lambdas that refer to each other within one
 * consecutive run of lambda bindings are created with placeholders and patched (OP_FIXCL)
 * instead. The front end has already rejected malformed forms. */
#include "interp.h"
#include <stdlib.h>
#include <string.h>

#define CADR(x) RT_CAR(RT_CDR(x))
#define CDDR(x) RT_CDR(RT_CDR(x))
#define CADDR(x) RT_CAR(CDDR(x))

typedef struct cfun cfun;
typedef struct { cfun *fn; int slot, boxed, checked; } var;
typedef struct { obj *syms; int n, cap; } flist; /* free variables; shared by case-lambda clauses */
struct cfun {
    int32_t *code;
    int n, cap;
    obj consts; /* reversed list */
    int nk;
    flist *fl;
    int nlocals, depth, maxdepth;
};
typedef struct { int clo, idx, target; } patch; /* OP_FIXCL operands */

static pmap vars;     /* local symbol -> var* (per unit) */
static pmap assigned; /* set! targets (per unit) */
static pmap primidx;  /* %primitive name symbol -> index + 1 */
static library *done; /* last library of front_order whose body was compiled */

static void comp(cfun *f, obj x, int tail);
static void comp_lambda(cfun *f, obj x, obj name, var **grp, int lo, int hi, patch **pv, int *np);

/* ---- emission ---- */
static void emit(cfun *f, int32_t w) {
    if (f->n == f->cap) {
        f->cap = f->cap ? 2 * f->cap : 64;
        f->code = (int32_t *)xrealloc(f->code, sizeof(int32_t) * (size_t)f->cap);
    }
    f->code[f->n++] = w;
}
static void op1(cfun *f, int op, int a) { emit(f, op); emit(f, a); }
static void op2(cfun *f, int op, int a, int b) { op1(f, op, a); emit(f, b); }
static void depth(cfun *f, int d) {
    f->depth += d;
    if (f->depth > f->maxdepth) f->maxdepth = f->depth;
}
static void push(cfun *f) { emit(f, OP_PUSH); depth(f, 1); }
static int konst(cfun *f, obj x) {
    f->consts = rt_cons(x, f->consts);
    return f->nk++;
}

static tmpl *finish(cfun *f, obj name, int nreq, int rest) {
    tmpl *t = (tmpl *)calloc(1, sizeof(tmpl));
    obj v = rt_vector_n(f->nk > 0 ? f->nk : 1, RT_FALSE), l;
    int i;
    for (i = f->nk - 1, l = f->consts; i >= 0; i--, l = RT_CDR(l)) RT_VEC(v)[i] = RT_CAR(l);
    if (!t) die("out of memory");
    t->code = f->code;
    t->k = RT_VEC(v);
    t->name = name;
    t->nreq = nreq;
    t->rest = rest;
    t->nlocals = f->nlocals;
    t->maxstack = f->maxdepth + 4;
    return t;
}

obj bc_cell(global *g) { /* called in IMM_PURE mode; a cell is mutable: scanned */
    if (!g->cell) {
        rt_gc_set_immortal(IMM_ROOTS);
        g->cell = rt_vector_n(3, RT_FALSE);
        RT_VEC(g->cell)[0] = RT_UNDEF;
        RT_VEC(g->cell)[1] = g->name;
        rt_gc_set_immortal(IMM_PURE);
    }
    return g->cell;
}

/* ---- variables ---- */
static var *new_var(cfun *f, obj s, int boxed) {
    var *v = (var *)calloc(1, sizeof(var));
    if (!v) die("out of memory");
    v->fn = f;
    v->slot = f->nlocals++;
    v->boxed = boxed;
    pm_put(&vars, s, v);
    return v;
}
static int free_index(cfun *f, obj s) {
    flist *l = f->fl;
    int i;
    for (i = 0; i < l->n; i++) if (l->syms[i] == s) return i;
    if (l->n == l->cap) {
        l->cap = l->cap ? 2 * l->cap : 8;
        l->syms = (obj *)xrealloc(l->syms, sizeof(obj) * (size_t)l->cap);
    }
    l->syms[l->n] = s;
    return l->n++;
}
/* The variable's slot content itself (the box, if boxed): closure captures. */
static void load_raw(cfun *f, obj s, var *v) {
    if (v->fn == f) op1(f, OP_LREF, v->slot);
    else op1(f, OP_FREF, free_index(f, s));
}
static obj source_name(obj s) {
    obj n = front_local_name(s);
    return n ? n : s;
}
static void comp_ref(cfun *f, obj s) {
    var *v = (var *)pm_get(&vars, s);
    if (!v) {
        global *g = front_global(s);
        if (!g) die("interpreter: unknown variable in core form");
        op1(f, OP_GREF, konst(f, bc_cell(g)));
    } else if (!v->boxed) load_raw(f, s, v);
    else {
        int local = v->fn == f, i = local ? v->slot : free_index(f, s);
        if (v->checked) op2(f, local ? OP_LBREFC : OP_FBREFC, i, konst(f, source_name(s)));
        else op1(f, local ? OP_LBREF : OP_FBREF, i);
    }
}
static void comp_set(cfun *f, obj s) { /* value in acc */
    var *v = (var *)pm_get(&vars, s);
    if (!v) { op1(f, OP_GSET, konst(f, bc_cell(front_global(s)))); return; }
    if (v->fn == f) op1(f, OP_LBSET, v->slot); /* assigned => boxed */
    else op1(f, OP_FBSET, free_index(f, s));
}

/* ---- analysis helpers ---- */
static void collect_assigned(obj x) {
    while (rt_is_pair(x)) {
        obj h = RT_CAR(x);
        if (h == S_quote) return;
        if (h == S_set) pm_put(&assigned, CADR(x), (void *)1);
        collect_assigned(h);
        x = RT_CDR(x);
    }
}
/* Which variables of a letrec* group (grp: symbol -> index + 1) does x mention? */
static void mentions(obj x, pmap *grp, char *hit) {
    if (rt_is_symbol(x)) {
        intptr_t i = (intptr_t)pm_get(grp, x);
        if (i) hit[i - 1] = 1;
        return;
    }
    while (rt_is_pair(x)) {
        if (RT_CAR(x) == S_quote) return;
        mentions(RT_CAR(x), grp, hit);
        x = RT_CDR(x);
        if (rt_is_symbol(x)) mentions(x, grp, hit);
    }
}
static int is_lambda(obj x) { return rt_is_pair(x) && (RT_CAR(x) == S_lambda || RT_CAR(x) == S_case_lambda); }
static int prim_of(obj h) {
    if (!rt_is_symbol(h)) return -1;
    return (int)(intptr_t)pm_get(&primidx, h) - 1;
}

/* ---- forms ---- */
static void comp_seq(cfun *f, obj l, int tail) {
    for (; RT_CDR(l) != RT_NIL; l = RT_CDR(l)) comp(f, RT_CAR(l), 0);
    comp(f, RT_CAR(l), tail);
}

static void comp_call(cfun *f, obj x, int tail) {
    int n = 0;
    obj l;
    if (!tail) { emit(f, OP_FRAME); depth(f, 3); }
    for (l = x; l != RT_NIL; l = RT_CDR(l), n++) { comp(f, RT_CAR(l), 0); push(f); }
    op1(f, tail ? OP_TCALL : OP_CALL, n - 1);
    depth(f, -(n + (tail ? 0 : 3)));
}

/* ((lambda (v ...) body) arg ...) with matching counts: a let. */
static int let_form(obj x) {
    obj h = RT_CAR(x), fs, as;
    if (!rt_is_pair(h) || RT_CAR(h) != S_lambda) return 0;
    for (fs = CADR(h), as = RT_CDR(x); rt_is_pair(fs) && rt_is_pair(as); fs = RT_CDR(fs), as = RT_CDR(as)) {}
    return fs == RT_NIL && as == RT_NIL;
}
static void comp_let(cfun *f, obj x, int tail) {
    obj fs, as, h = RT_CAR(x);
    for (fs = CADR(h), as = RT_CDR(x); fs != RT_NIL; fs = RT_CDR(fs), as = RT_CDR(as)) {
        obj s = RT_CAR(fs), e = RT_CAR(as);
        int boxed = pm_get(&assigned, s) != NULL;
        var *v;
        if (is_lambda(e)) comp_lambda(f, e, boxed ? sym("lambda") : source_name(s), NULL, 0, 0, NULL, NULL);
        else comp(f, e, 0);
        if (boxed) emit(f, OP_BOX);
        v = new_var(f, s, boxed);
        op1(f, OP_LSET, v->slot);
    }
    comp(f, CADDR(h), tail);
}

static void comp_letrec(cfun *f, obj x, int tail) {
    obj bs = CADR(x), l;
    int n = list_len(bs), i, j, k;
    obj *syms = (obj *)xmalloc(sizeof(obj) * (size_t)n), *inits = (obj *)xmalloc(sizeof(obj) * (size_t)n);
    int *run = (int *)xmalloc(sizeof(int) * (size_t)n);
    char *early = (char *)calloc((size_t)n, 1), *hit = (char *)calloc((size_t)n, 1);
    var **vs = (var **)xmalloc(sizeof(var *) * (size_t)n);
    pmap grp = {0};
    for (i = 0, l = bs; i < n; i++, l = RT_CDR(l)) {
        syms[i] = RT_CAR(RT_CAR(l));
        inits[i] = CADR(RT_CAR(l));
        pm_put(&grp, syms[i], (void *)(intptr_t)(i + 1));
        run[i] = !is_lambda(inits[i]) ? -1 : i > 0 && run[i - 1] >= 0 ? run[i - 1] : i;
    }
    /* early: mentioned by the init of an earlier (or its own) binding, unless both are
     * lambdas of the same run (patched after the run's closures exist) */
    for (j = 0; j < n; j++) {
        memset(hit, 0, (size_t)n);
        mentions(inits[j], &grp, hit);
        for (i = j; i < n; i++)
            if (hit[i] && !(run[j] >= 0 && run[i] == run[j])) early[i] = 1;
    }
    for (i = 0; i < n; i++) {
        int boxed = early[i] || pm_get(&assigned, syms[i]) != NULL;
        vs[i] = new_var(f, syms[i], boxed);
        vs[i]->checked = early[i];
        if (boxed) op1(f, OP_MKBOX, vs[i]->slot);
    }
    for (i = 0; i < n; i = j) {
        if (run[i] < 0) {
            comp(f, inits[i], 0);
            if (vs[i]->boxed) op1(f, OP_LBSET, vs[i]->slot);
            else op1(f, OP_LSET, vs[i]->slot);
            j = i + 1;
            continue;
        }
        for (j = i; j < n && run[j] == run[i]; j++) {}
        {   /* the lambda run [i, j) */
            patch *pv = NULL;
            int np = 0;
            for (k = i; k < j; k++) {
                obj nm = pm_get(&assigned, syms[k]) ? sym("lambda") : source_name(syms[k]);
                comp_lambda(f, inits[k], nm, vs, k, j, &pv, &np);
                op1(f, vs[k]->boxed ? OP_LBSET : OP_LSET, vs[k]->slot);
            }
            for (k = 0; k < np; k++) { emit(f, OP_FIXCL); emit(f, pv[k].clo); emit(f, pv[k].idx); emit(f, pv[k].target); }
            free(pv);
        }
    }
    comp(f, CADDR(x), tail);
    free(syms); free(inits); free(run); free(early); free(hit); free(vs);
    free(grp.k); free(grp.v);
}

/* One clause: parameters in slots 0..; boxed (assigned) parameters are boxed on entry. */
static tmpl *comp_clause(flist *fl, obj formals, obj body, obj name) {
    cfun c;
    int nreq = 0, rest = 0;
    obj p;
    tmpl *t;
    memset(&c, 0, sizeof c);
    c.consts = RT_NIL;
    c.fl = fl;
    for (p = formals; rt_is_pair(p); p = RT_CDR(p)) nreq++;
    rest = p != RT_NIL;
    for (p = formals;; p = RT_CDR(p)) {
        obj s = rt_is_pair(p) ? RT_CAR(p) : p;
        var *v;
        if (s == RT_NIL) break;
        v = new_var(&c, s, pm_get(&assigned, s) != NULL);
        if (v->boxed) { op1(&c, OP_LREF, v->slot); emit(&c, OP_BOX); op1(&c, OP_LSET, v->slot); }
        if (!rt_is_pair(p)) break;
    }
    comp(&c, body, 1);
    t = finish(&c, name, nreq, rest);
    return t;
}

/* Closure creation. With grp: a letrec* run whose variables grp[lo..hi) are not created yet
 * (lo is this lambda's own index); captures of them are placeholders recorded in *pv. */
static void comp_lambda(cfun *f, obj x, obj name, var **grp, int lo, int hi, patch **pv, int *np) {
    flist *fl = (flist *)calloc(1, sizeof(flist));
    tmpl *t;
    int i;
    if (!fl) die("out of memory");
    if (RT_CAR(x) == S_lambda) t = comp_clause(fl, CADR(x), CADDR(x), name);
    else {
        obj l;
        int n = list_len(RT_CDR(x));
        t = (tmpl *)calloc(1, sizeof(tmpl));
        if (!t) die("out of memory");
        t->clauses = (tmpl **)xmalloc(sizeof(tmpl *) * (size_t)n);
        for (i = 0, l = RT_CDR(x); i < n; i++, l = RT_CDR(l)) t->clauses[i] = comp_clause(fl, RT_CAR(RT_CAR(l)), CADR(RT_CAR(l)), name);
        t->nclauses = n;
        t->name = name;
        for (i = 0; i < n; i++) t->clauses[i]->nfree = fl->n;
    }
    t->nfree = fl->n;
    for (i = 0; i < fl->n; i++) {
        obj s = fl->syms[i];
        var *v = (var *)pm_get(&vars, s);
        int g = -1, m;
        if (grp)
            for (m = lo; m < hi; m++) if (grp[m] == v && !v->boxed) g = m;
        if (g >= 0) { /* not created yet: placeholder, patched after the run */
            op1(f, OP_CONST, konst(f, RT_FALSE));
            *pv = (patch *)xrealloc(*pv, sizeof(patch) * (size_t)(*np + 1));
            (*pv)[*np].clo = grp[lo]->slot;
            (*pv)[*np].idx = i;
            (*pv)[(*np)++].target = v->slot;
        } else load_raw(f, s, v);
        push(f);
    }
    op2(f, OP_CLOSURE, konst(f, (obj)(void *)t), fl->n);
    depth(f, -fl->n);
}

static void comp_prim(cfun *f, obj x, int p, int tail) {
    obj args = RT_CDR(x), l;
    int n = list_len(args);
    switch (p) {
    case PI_APPLY: case PI_CALLCC: case PI_EVAL:
        if (!tail) { emit(f, OP_FRAME); depth(f, 3); }
        comp(f, RT_CAR(args), 0);
        push(f);
        depth(f, 1); /* room for CALLCC's k */
        depth(f, -1);
        if (p == PI_CALLCC) emit(f, tail ? OP_TCALLCC : OP_CALLCC);
        else {
            comp(f, CADR(args), 0);
            if (p == PI_EVAL) emit(f, tail ? OP_TEVAL : OP_EVAL);
            else { emit(f, OP_SPREADL); emit(f, tail ? OP_TCALLV : OP_CALLV); }
        }
        depth(f, -(1 + (tail ? 0 : 3)));
        return;
    case PI_CWV:
        if (!tail) { emit(f, OP_FRAME); depth(f, 3); }
        comp(f, RT_CAR(args), 0);
        push(f);
        comp(f, CADR(args), 0);
        emit(f, OP_CWVSWAP);
        depth(f, 4);
        op1(f, OP_CALL, 0);
        depth(f, -4);
        if (rt_is_pair(CADR(args)) && RT_CAR(CADR(args)) == S_lambda) { /* as compiled: %values-check */
            obj fs = CADR(CADR(args));
            int nreq = 0;
            for (; rt_is_pair(fs); fs = RT_CDR(fs)) nreq++;
            op2(f, OP_VCHECK, nreq, fs != RT_NIL);
        }
        emit(f, OP_SPREADV);
        emit(f, tail ? OP_TCALLV : OP_CALLV);
        depth(f, -(1 + (tail ? 0 : 3)));
        return;
    case PI_ENVIRONMENT: comp(f, RT_CAR(args), 0); emit(f, OP_ENV); break;
    case PI_INTERENV: emit(f, OP_INTERENV); break;
    case PI_CCALL: { /* define-c: resolved now, i.e. when the definition is compiled */
        int i = ffi_resolve(CADR(RT_CAR(args)), x);
        n--;
        for (l = RT_CDR(args); l != RT_NIL; l = RT_CDR(l)) {
            comp(f, RT_CAR(l), 0);
            if (RT_CDR(l) != RT_NIL) push(f);
        }
        op2(f, OP_CCALL, i, n);
        if (n > 1) depth(f, -(n - 1));
        break;
    }
    default:
        for (l = args; l != RT_NIL; l = RT_CDR(l)) {
            comp(f, RT_CAR(l), 0);
            if (RT_CDR(l) != RT_NIL) push(f);
        }
        if (p == PI_VALUES) op1(f, OP_VALUES, n);
        else op2(f, OP_PRIM, p, n);
        if (n > 1) depth(f, -(n - 1));
    }
    if (tail) emit(f, OP_RET);
}

/* Literal constants are read-only (spec §3.2): strings, vectors and bytevectors get RT_F_RO;
 * literal pairs are immortal, which set-car!/set-cdr! check (RT_LITPAIR in tool mode). */
static obj literal(obj x) {
    obj y = x;
    for (; rt_is_pair(y); y = RT_CDR(y)) literal(RT_CAR(y));
    if (rt_is_string(y) || rt_is_vector(y) || rt_is_bytevector(y) || rt_is_type(y, RT_T_F64VECTOR)) {
        RT_SETFLAGS(y, RT_HFLAGS(y) | RT_F_RO);
        if (rt_is_vector(y)) {
            intptr_t i;
            for (i = 0; i < RT_HLEN(y); i++) literal(RT_VEC(y)[i]);
        }
    }
    return x;
}
static void comp(cfun *f, obj x, int tail) {
    obj h;
    int p;
    if (rt_is_symbol(x)) comp_ref(f, x);
    else if (!rt_is_pair(x)) op1(f, OP_CONST, konst(f, literal(x)));
    else if ((h = RT_CAR(x)) == S_quote) op1(f, OP_CONST, konst(f, literal(CADR(x))));
    else if (h == S_lambda || h == S_case_lambda) comp_lambda(f, x, sym("lambda"), NULL, 0, 0, NULL, NULL);
    else if (h == S_if) {
        int jf, je = 0;
        comp(f, CADR(x), 0);
        op1(f, OP_JMPF, 0);
        jf = f->n - 1;
        comp(f, CADDR(x), tail);
        if (!tail) { op1(f, OP_JMP, 0); je = f->n - 1; }
        f->code[jf] = f->n;
        if (RT_CDR(CDDR(x)) != RT_NIL) comp(f, RT_CAR(RT_CDR(CDDR(x))), tail);
        else { op1(f, OP_CONST, konst(f, RT_UNSPEC)); if (tail) emit(f, OP_RET); }
        if (!tail) f->code[je] = f->n;
        return;
    } else if (h == S_set) { comp(f, CADDR(x), 0); comp_set(f, CADR(x)); }
    else if (h == S_begin) { comp_seq(f, RT_CDR(x), tail); return; }
    else if (h == S_letrec_star) { comp_letrec(f, x, tail); return; }
    else if ((p = prim_of(h)) >= 0) { comp_prim(f, x, p, tail); return; }
    else if (let_form(x)) { comp_let(f, x, tail); return; }
    else { comp_call(f, x, tail); return; }
    if (tail) emit(f, OP_RET);
}

/* ---- units ---- */
static int is_define(obj x) { return rt_is_pair(x) && RT_CAR(x) == S_define; }
tmpl *bc_unit(obj forms) {
    cfun top;
    obj all = RT_NIL, l;
    pmap hoisted = {0};
    library *lib;
    tmpl *t;
    int i;
    if (!primidx.cap)
        for (i = 0; i < NPRIMS_ALL; i++) pm_put(&primidx, sym(prim_table[i].name), (void *)(intptr_t)(i + 1));
    free(vars.k); free(vars.v); memset(&vars, 0, sizeof vars);
    free(assigned.k); free(assigned.v); memset(&assigned, 0, sizeof assigned);
    for (lib = done ? done->next : front_order; lib; lib = lib->next) { /* newly loaded libraries */
        all = append2(all, lib->core);
        done = lib;
    }
    all = append2(all, forms);
    collect_assigned(all);
    memset(&top, 0, sizeof top);
    top.consts = RT_NIL;
    top.fl = (flist *)calloc(1, sizeof(flist));
    for (l = all; l != RT_NIL; l = RT_CDR(l)) { /* defined globals: "used before its definition" */
        obj x = RT_CAR(l);
        if (is_define(x)) RT_VEC(bc_cell(front_global(CADR(x))))[2] = RT_TRUE;
    }
    /* Top-level procedures defined once and never assigned exist from the start of the unit,
     * as in compiled programs (static closures; DESIGN.md §10). */
    for (l = all; l != RT_NIL; l = RT_CDR(l)) {
        obj x = RT_CAR(l);
        global *g;
        if (!is_define(x) || !is_lambda(CADDR(x))) continue;
        g = front_global(CADR(x));
        if (g->defined != 1 || g->assigned) continue;
        comp_lambda(&top, CADDR(x), g->name, NULL, 0, 0, NULL, NULL);
        op1(&top, OP_GDEF, konst(&top, bc_cell(g)));
        pm_put(&hoisted, x, (void *)1);
    }
    op1(&top, OP_CONST, konst(&top, RT_UNSPEC));
    for (l = all; l != RT_NIL; l = RT_CDR(l)) {
        obj x = RT_CAR(l);
        if (pm_get(&hoisted, x)) op1(&top, OP_CONST, konst(&top, RT_UNSPEC));
        else if (is_define(x)) {
            global *g = front_global(CADR(x));
            if (is_lambda(CADDR(x))) comp_lambda(&top, CADDR(x), g->name, NULL, 0, 0, NULL, NULL);
            else comp(&top, CADDR(x), 0);
            op1(&top, OP_GDEF, konst(&top, bc_cell(g)));
        } else comp(&top, x, 0);
    }
    emit(&top, OP_RET);
    t = finish(&top, sym("toplevel"), 0, 0);
    free(hoisted.k); free(hoisted.v);
    return t;
}
