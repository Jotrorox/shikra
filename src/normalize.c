/* normalize.c -- the middle end (spec §5 passes 4 and 6; DESIGN.md §7, §11 M0 part B, M4-1).
 *
 *   1. tree shaking: keep the top-level forms reachable from the program body and from
 *      side-effecting top-level forms of the libraries (all user code is normalized so its
 *      diagnostics are complete; the optimizer drops what stays unreachable); unused user
 *      definitions are reported as warnings;
 *   2. variable analysis on the core forms: one ir_var per binder, assigned / captured;
 *   3. known top-level procedures (defined once by lambda / %case-lambda, never set!), and
 *      the first top-level form from which each can run (undefined-global checks);
 *   4. normalization of core forms to the ANF IR (ir.h): letrec* grouping, boxes,
 *      undefined checks, direct lambda application as let, known calls (arity checked
 *      at compile time), self tail calls as jumps to the procedure's entry join point,
 *      call-with-values with a literal receiver as a join point, tier 1 call/cc as jumps
 *      (optimistically also from inside lambdas: verified after optimization, else the
 *      site is rebuilt as tier 2);
 *   5. the optimizer (opt.c), then closure layout, static closures, C names. */
#include "ir.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ir_program *prog;
static int nvars, nlams, njoins;
static pmap varmap;        /* core variable symbol -> ir_var* */
static pmap primmap;       /* interned %name -> prim index + 1 */
static int *vscope;        /* var id -> binding lambda token (variable analysis) */
static int vscope_cap, lamtok;
static ir_lambda *cur_lam, *last_lam;
static library *cur_lib;
static int cur_form, in_static;
static obj UNSPEC_FORM;
static ir_var *new_var(obj name, ir_lambda *owner);
static pmap demoted;       /* tier-1 sites rebuilt as tier 2 (core form -> 1) */
static int cur_ff;         /* first_form of the known procedure being normalized */
static const ir_opts *opts;
/* (scheme base) procedures the middle end knows (M1 part A): calls to values,
 * call-with-values and apply become the control primitives; call/cc sites may be tier 1;
 * raise is the engine's raise entry. */
static global *g_values, *g_cwv, *g_apply, *g_callcc1, *g_callcc2, *g_raise, *g_travel;
static obj S_pvalues, S_pcwv, S_papply, S_pcallcc;
static pmap kctx;          /* tier-1 continuation variable symbol -> ir_join*, or &ret_ctx */
static ir_join ret_ctx;    /* "k returns from the procedure" */
static ir_node *dead_sink; /* code after a jump of a tier-1 k goes here and is dropped */

#define CADR(x) RT_CAR(RT_CDR(x))
#define CDDR(x) RT_CDR(RT_CDR(x))
#define CADDR(x) RT_CAR(CDDR(x))

/* ---- constructors ---- */
ir_var *ir_new_var(obj name, ir_lambda *owner) { return new_var(name, owner); }
static ir_var *new_var(obj name, ir_lambda *owner) {
    ir_var *v = NEW(ir_var);
    v->id = nvars++;
    v->name = name;
    v->owner = owner;
    v->def_index = -1;
    if (v->id >= vscope_cap) {
        int *n = NEWN(int, vscope_cap * 2 + 256);
        ir_var **nv = NEWN(ir_var *, vscope_cap * 2 + 256);
        if (vscope) memcpy(n, vscope, sizeof(int) * (size_t)vscope_cap);
        if (prog->vars) memcpy(nv, prog->vars, sizeof(ir_var *) * (size_t)vscope_cap);
        vscope = n;
        prog->vars = nv;
        vscope_cap = vscope_cap * 2 + 256;
    }
    prog->vars[v->id] = v;
    prog->nvars = nvars;
    return v;
}
static ir_node *node(int kind, obj loc) {
    ir_node *n = NEW(ir_node);
    n->kind = kind;
    n->loc = loc;
    return n;
}
ir_node *ir_new_node(int kind, obj loc) { return node(kind, loc); }
static ir_node *mk_const(obj v) { ir_node *n = node(IR_CONST, 0); n->u.value = v; return n; }
static ir_node *mk_ref(ir_var *v) { ir_node *n = node(IR_REF, 0); n->u.ref.v = v; return n; }
static ir_node *mk_prim(int op, int n, obj loc) {
    ir_node *p = node(IR_PRIM, loc);
    p->u.prim.op = op;
    p->u.prim.n = n;
    p->u.prim.args = NEWN(ir_node *, n);
    return p;
}
static ir_node *prim1(int op, ir_node *a) { ir_node *p = mk_prim(op, 1, 0); p->u.prim.args[0] = a; return p; }
static ir_node *prim2(int op, ir_node *a, ir_node *b) {
    ir_node *p = mk_prim(op, 2, 0);
    p->u.prim.args[0] = a;
    p->u.prim.args[1] = b;
    return p;
}
static ir_lambda *new_lambda(obj name, ir_lambda *parent) {
    ir_lambda *L = NEW(ir_lambda);
    L->id = nlams++;
    L->name = name;
    L->parent = parent;
    L->lib = cur_lib;
    if (last_lam) last_lam->next = L;
    else prog->lambdas = L;
    last_lam = L;
    return L;
}
static ir_join *new_join(int n) {
    ir_join *j = NEW(ir_join);
    j->id = njoins++;
    j->n = n;
    j->params = NEWN(ir_var *, n);
    j->owner = cur_lam;
    return j;
}
ir_join *ir_new_join(int n, ir_lambda *owner) { ir_join *j = new_join(n); j->owner = owner; return j; }
ir_lambda *ir_new_lambda(obj name, ir_lambda *parent, library *lib) {
    library *saved = cur_lib;
    ir_lambda *L;
    cur_lib = lib;
    L = new_lambda(name, parent);
    cur_lib = saved;
    return L;
}
static int is_atom(ir_node *n) { return n->kind == IR_CONST || n->kind == IR_REF; }
static int prim_index(obj head) {
    void *p = rt_is_symbol(head) ? pm_get(&primmap, head) : NULL;
    return p ? (int)((intptr_t)p - 1) : -1;
}
static int is_lambda_form(obj x) { return rt_is_pair(x) && (RT_CAR(x) == S_lambda || RT_CAR(x) == S_case_lambda); }
static int is_thunk(obj x) { return rt_is_pair(x) && RT_CAR(x) == S_lambda && CADR(x) == RT_NIL; }

/* ---- control forms the normalizer open-codes ---- */
static global *base_global(library *order, const char *name) {
    library *l;
    for (l = order; l; l = l->next)
        if (!strcmp(l->id, "scheme/base")) {
            binding *b = (binding *)pm_get(&l->env->map, sym(name));
            return b && b->kind == B_GLOBAL ? b->g : NULL;
        }
    return NULL;
}
/* Calls to (scheme base)'s values / call-with-values / apply (imported bindings are never
 * assigned) become %values / %call-with-values / %apply, in place, before tree shaking. */
static void rewrite_aliases(obj x) {
    obj l;
    global *g;
    if (!rt_is_pair(x) || RT_CAR(x) == S_quote) return;
    if ((g = front_global(RT_CAR(x)))) {
        int n = list_len(RT_CDR(x));
        if (g == g_values) RT_CAR(x) = S_pvalues;
        else if (g == g_cwv && n == 2) RT_CAR(x) = S_pcwv;
        else if (g == g_apply && n == 2) RT_CAR(x) = S_papply;
    }
    for (l = x; rt_is_pair(l); l = RT_CDR(l)) rewrite_aliases(RT_CAR(l));
}
/* (%call-with-values producer (lambda formals body)): the receiver becomes a join point
 * whose parameters are its formals; a (lambda () body) producer is inlined. Both lambdas
 * are then scopes of the enclosing procedure, like a let. */
static int cwv_inline(obj x) {
    obj c;
    if (!rt_is_pair(x) || RT_CAR(x) != S_pcwv || list_len(x) != 3) return 0;
    c = CADDR(x);
    return rt_is_pair(c) && RT_CAR(c) == S_lambda;
}
/* Tier 1 (spec §8.2): (call/cc (lambda (k) body)) where body only calls k, never crossing a
 * lambda (let-style direct applications, inlined call-with-values lambdas and nested tier-1
 * sites do not count as crossing). k is then a jump to the join point after the site (or a
 * return), with no runtime cost; no dynamic-wind, parameterize or handler can intervene. */
static int tier1_site(obj x);
static int occurs(obj x, obj s) {
    if (x == s) return 1;
    if (!rt_is_pair(x) || RT_CAR(x) == S_quote) return 0;
    for (; rt_is_pair(x); x = RT_CDR(x)) if (occurs(RT_CAR(x), s)) return 1;
    return x == s;
}
static int k_crossed; /* k is called from inside a lambda: an optimistic site */
static int k_ok(obj x, obj k) {
    obj h, l;
    if (x == k) return 0;
    if (!rt_is_pair(x)) return 1;
    h = RT_CAR(x);
    if (h == S_quote) return 1;
    if (is_lambda_form(x)) {
        if (!occurs(x, k)) return 1;
        k_crossed = 1; /* only calls of k inside: fine if the lambda ends up in this C function */
        if (h == S_lambda) return k_ok(CADDR(x), k);
        for (l = RT_CDR(x); l != RT_NIL; l = RT_CDR(l)) if (!k_ok(CADR(RT_CAR(l)), k)) return 0;
        return 1;
    }
    if (cwv_inline(x))
        return (is_thunk(CADR(x)) ? k_ok(CADDR(CADR(x)), k) : k_ok(CADR(x), k)) && k_ok(CADDR(CADDR(x)), k);
    if (tier1_site(x)) return k_ok(CADDR(CADR(x)), k);
    if (h == k) l = RT_CDR(x);
    else if (rt_is_pair(h) && RT_CAR(h) == S_lambda) {
        if (!k_ok(CADDR(h), k)) return 0;
        l = RT_CDR(x);
    } else l = x;
    for (; rt_is_pair(l); l = RT_CDR(l)) if (!k_ok(RT_CAR(l), k)) return 0;
    return 1;
}
static pmap t1memo; /* site -> 1 (no) / 2 (yes) / 3 (yes, optimistic: k called inside a lambda) */
static int tier1_kind(obj x) {
    global *g;
    obj f;
    intptr_t m;
    if (!rt_is_pair(x) || !(g = front_global(RT_CAR(x))) || (g != g_callcc1 && g != g_callcc2) || list_len(x) != 2) return 0;
    if ((m = (intptr_t)pm_get(&t1memo, x))) return (int)m - 1;
    f = CADR(x);
    {
        int saved = k_crossed;
        k_crossed = 0;
        m = !pm_get(&demoted, x) && rt_is_pair(f) && RT_CAR(f) == S_lambda && list_len(CADR(f)) == 1 && k_ok(CADDR(f), RT_CAR(CADR(f)));
        if (m && k_crossed) m = 2;
        k_crossed = saved;
    }
    pm_put(&t1memo, x, (void *)(m + 1));
    return (int)m;
}
static int tier1_site(obj x) { return tier1_kind(x) != 0; }
/* Any other call of base's call/cc is %call/cc itself in compiled code: the compiled
 * continuation walks the winders on its own (rt_core.c), no escape procedure is needed. */
static int plain_callcc(obj x) {
    global *g;
    return rt_is_pair(x) && (g = front_global(RT_CAR(x))) && (g == g_callcc1 || g == g_callcc2) && list_len(x) == 2 && !tier1_site(x);
}

/* ---- 1. tree shaking ----
 * Roots: every form of the program and of user libraries (so the middle end's diagnostics
 * cover all user code), and every top-level form of a built-in library whose right-hand
 * side may have an effect. A definition is kept when its global is reachable from a root. */
typedef struct { obj form; global *g; library *lib; int root, keep; } topform;
static topform *tops;
static int ntops;
static pmap gmarked;  /* global* -> (void*)1 */
static global **gwork;
static int ngwork, gwork_cap;

static int pure_rhs(obj x) {
    obj l;
    int p;
    if (!rt_is_pair(x)) return 1;
    if (RT_CAR(x) == S_quote || is_lambda_form(x)) return 1;
    if ((p = prim_index(RT_CAR(x))) < 0 || p >= NPRIMS || !(prim_table[p].flags & PF_PURE)) return 0;
    for (l = RT_CDR(x); l != RT_NIL; l = RT_CDR(l)) if (!pure_rhs(RT_CAR(l))) return 0;
    return 1;
}
static void mark_global(global *g) {
    if (pm_get(&gmarked, g)) return;
    pm_put(&gmarked, g, (void *)1);
    if (ngwork == gwork_cap) {
        global **n = NEWN(global *, gwork_cap * 2 + 64);
        if (gwork) memcpy(n, gwork, sizeof(global *) * (size_t)ngwork);
        gwork = n;
        gwork_cap = gwork_cap * 2 + 64;
    }
    gwork[ngwork++] = g;
}
static void mark_refs(obj x) {
    global *g;
    while (rt_is_pair(x)) {
        if (RT_CAR(x) == S_quote) return;
        mark_refs(RT_CAR(x));
        x = RT_CDR(x);
    }
    if ((g = front_global(x))) mark_global(g);
}
static pmap defs; /* global* -> list of form indices (as fixnums) */
static void shake(library *order) {
    library *l;
    obj f;
    int i, n = 0, done = 0;
    for (l = order; l; l = l->next) n += list_len(l->core);
    tops = NEWN(topform, n);
    for (l = order; l; l = l->next)
        for (f = l->core; f != RT_NIL; f = RT_CDR(f)) {
            topform *t = &tops[ntops];
            t->form = RT_CAR(f);
            t->lib = l;
            if (!front_builtin(l)) t->root = 1; /* user code: always compiled (diagnostics) */
            if (rt_is_pair(t->form) && RT_CAR(t->form) == S_define) {
                obj prev;
                t->g = front_global(CADR(t->form));
                prev = (obj)pm_get(&defs, t->g);
                pm_put(&defs, t->g, (void *)rt_cons(RT_FIX(ntops), prev ? prev : RT_NIL));
                t->root |= !pure_rhs(CADDR(t->form));
            } else t->root = 1;
            ntops++;
        }
    for (i = 0; i < ntops; i++)
        if (tops[i].root) { tops[i].keep = 1; mark_refs(tops[i].form); }
    if (g_raise) mark_global(g_raise); /* the engine's raise entry (runtime errors) */
    while (done < ngwork) {
        global *g = gwork[done++];
        obj d = (obj)pm_get(&defs, g);
        for (; d && d != RT_NIL; d = RT_CDR(d)) {
            topform *t = &tops[RT_FIXVAL(RT_CAR(d))];
            if (!t->keep) { t->keep = 1; mark_refs(t->form); }
        }
    }
}

/* ---- 3b. the first top-level form from which each known procedure can run ----
 * Walks the top-level forms in order; a known procedure becomes reachable at the first form
 * that references it (directly or through procedures reachable from there). Its reads of
 * globals defined by earlier forms need no undefined check. raise can run at any time. */
static pmap ffmap; /* global* -> first form index + 1 */
static int known_def(topform *t) {
    return t->keep && t->g && t->g->defined == 1 && !t->g->assigned && is_lambda_form(CADDR(t->form));
}
static void ff_refs(obj x, int i);
static void ff_global(global *g, int i) {
    obj d;
    if (pm_get(&ffmap, g)) return;
    pm_put(&ffmap, g, (void *)(intptr_t)(i + 1));
    for (d = (obj)pm_get(&defs, g); d && d != RT_NIL; d = RT_CDR(d)) {
        topform *t = &tops[RT_FIXVAL(RT_CAR(d))];
        if (known_def(t)) ff_refs(CADDR(t->form), i);
    }
}
static void ff_refs(obj x, int i) {
    global *g;
    while (rt_is_pair(x)) {
        if (RT_CAR(x) == S_quote) return;
        ff_refs(RT_CAR(x), i);
        x = RT_CDR(x);
    }
    if ((g = front_global(x))) ff_global(g, i);
}
/* The program's definitions named by --keep (REPL ,c): roots that stay functions. */
static global *keep_global(library *order, int k) {
    library *l;
    binding *b;
    if (!opts->keep || !opts->keep[k]) return NULL;
    for (l = order; l; l = l->next)
        if (l->name == RT_FALSE && (b = (binding *)pm_get(&l->env->map, sym(opts->keep[k]))) && b->kind == B_GLOBAL) return b->g;
    return NULL;
}
static int nkeep(void) { int k = 0; while (opts->keep && opts->keep[k]) k++; return k; }
static void first_forms(library *order) {
    int i;
    if (g_raise) ff_global(g_raise, 0);
    for (i = 0; i < nkeep(); i++) if (keep_global(order, i)) ff_global(keep_global(order, i), 0);
    for (i = 0; i < ntops; i++)
        if (tops[i].keep && !known_def(&tops[i])) ff_refs(tops[i].g ? CADDR(tops[i].form) : tops[i].form, i);
}

/* Unused definitions (spec §11.3): a definition of the program or of a user library that
 * nothing reachable refers to. Reachable: the expressions of all code, the right-hand sides
 * of definitions that are impure (they run) or reachable, the exports of user libraries. */
static pmap wmarked;
static global **wwork;
static int nwwork, wwork_cap;
static void wmark(global *g) {
    if (!g || pm_get(&wmarked, g)) return;
    pm_put(&wmarked, g, (void *)1);
    if (nwwork == wwork_cap) {
        global **n = NEWN(global *, wwork_cap * 2 + 64);
        if (wwork) memcpy(n, wwork, sizeof(global *) * (size_t)nwwork);
        wwork = n;
        wwork_cap = wwork_cap * 2 + 64;
    }
    wwork[nwwork++] = g;
}
static void wmark_refs(obj x) {
    while (rt_is_pair(x)) {
        if (RT_CAR(x) == S_quote) return;
        wmark_refs(RT_CAR(x));
        x = RT_CDR(x);
    }
    wmark(front_global(x));
}
static void warn_unused(library *order) {
    library *l;
    int i, done = 0;
    for (l = order; l; l = l->next)
        if (!front_builtin(l) && l->name != RT_FALSE) {
            imp *e;
            for (e = l->exports; e; e = e->next) if (e->b->kind == B_GLOBAL) wmark(e->b->g);
        }
    for (i = 0; i < nkeep(); i++) wmark(keep_global(order, i));
    for (i = 0; i < ntops; i++)
        if (tops[i].keep) {
            if (!tops[i].g) wmark_refs(tops[i].form);
            else if (!pure_rhs(CADDR(tops[i].form))) wmark_refs(CADDR(tops[i].form));
        }
    while (done < nwwork) {
        obj d;
        for (d = (obj)pm_get(&defs, wwork[done++]); d && d != RT_NIL; d = RT_CDR(d)) wmark_refs(CADDR(tops[RT_FIXVAL(RT_CAR(d))].form));
    }
    for (i = 0; i < ntops; i++) {
        topform *t = &tops[i];
        if (t->g && !front_builtin(t->lib) && !t->g->nowarn && !pm_get(&wmarked, t->g))
            cwarning(t->g->defloc ? t->g->defloc : t->form, "unused definition %s", idname(t->g->name));
    }
}

/* ---- 2. variable analysis ---- */
static ir_var *gvar(global *g) {
    ir_var *v = (ir_var *)g->mid;
    if (!v) {
        v = new_var(g->name, NULL);
        v->flags = V_GLOBAL;
        v->g = g;
        g->mid = v;
        pm_put(&varmap, g->sym, v);
    }
    return v;
}
static ir_var *var_of(obj s) {
    ir_var *v = (ir_var *)pm_get(&varmap, s);
    global *g;
    if (v) return v;
    if ((g = front_global(s))) return gvar(g);
    cerror(RT_FALSE, "internal error: unbound core variable %s", dstr(s));
}
static void bind_sym(obj s, int scope) {
    obj name = front_local_name(s);
    ir_var *v = new_var(name ? name : s, NULL);
    vscope[v->id] = scope;
    pm_put(&varmap, s, v);
}
static void bind_formals(obj f, int scope) {
    for (; rt_is_pair(f); f = RT_CDR(f)) bind_sym(RT_CAR(f), scope);
    if (f != RT_NIL) bind_sym(f, scope);
}
static void analyse(obj x, int lam) {
    obj h, l;
    if (rt_is_symbol(x)) {
        ir_var *v = var_of(x);
        if (!(v->flags & V_GLOBAL) && vscope[v->id] != lam) v->flags |= V_CAPTURED;
        return;
    }
    if (!rt_is_pair(x)) return;
    h = RT_CAR(x);
    if (h == S_quote) return;
    if (h == S_lambda) {
        int t = ++lamtok;
        bind_formals(CADR(x), t);
        analyse(CADDR(x), t);
        return;
    }
    if (h == S_case_lambda) {
        for (l = RT_CDR(x); l != RT_NIL; l = RT_CDR(l)) {
            int t = ++lamtok;
            bind_formals(RT_CAR(RT_CAR(l)), t);
            analyse(CADR(RT_CAR(l)), t);
        }
        return;
    }
    if (h == S_letrec_star) {
        for (l = CADR(x); l != RT_NIL; l = RT_CDR(l)) bind_sym(RT_CAR(RT_CAR(l)), lam);
        for (l = CADR(x); l != RT_NIL; l = RT_CDR(l)) analyse(CADR(RT_CAR(l)), lam);
        analyse(CADDR(x), lam);
        return;
    }
    if (h == S_set) {
        ir_var *v = var_of(CADR(x));
        v->flags |= V_ASSIGNED;
        analyse(CADR(x), lam);
        analyse(CADDR(x), lam);
        return;
    }
    if (h == S_define) { analyse(CADDR(x), lam); return; }
    if (cwv_inline(x)) {
        obj p = CADR(x), c = CADDR(x);
        analyse(is_thunk(p) ? CADDR(p) : p, lam);
        bind_formals(CADR(c), lam);
        analyse(CADDR(c), lam);
        return;
    }
    if (tier1_site(x)) {
        bind_formals(CADR(CADR(x)), lam);
        analyse(CADDR(CADR(x)), lam);
        return;
    }
    if (h == S_if || h == S_begin || prim_index(h) >= 0) {
        for (l = RT_CDR(x); l != RT_NIL; l = RT_CDR(l)) analyse(RT_CAR(l), lam);
        return;
    }
    if (rt_is_pair(h) && RT_CAR(h) == S_lambda) { /* ((lambda formals body) args): a let */
        for (l = RT_CDR(x); l != RT_NIL; l = RT_CDR(l)) analyse(RT_CAR(l), lam);
        bind_formals(CADR(h), lam);
        analyse(CADDR(h), lam);
        return;
    }
    for (l = x; l != RT_NIL; l = RT_CDR(l)) analyse(RT_CAR(l), lam);
}

/* ---- 4. normalization ---- */
typedef struct { ir_node **hole; } bld;
static void norm_tail(obj x, bld *b);
static ir_node *norm_rhs(obj x, bld *b);
static void norm_lambda_body(ir_lambda *L, obj x);

static void bind_let(bld *b, ir_var *v, ir_node *rhs, obj loc) {
    ir_node *n = node(IR_LET, loc);
    n->u.let.v = v;
    n->u.let.rhs = rhs;
    if (v) v->owner = cur_lam;
    *b->hole = n;
    b->hole = &n->u.let.body;
}
static ir_node *atomize(bld *b, ir_node *rhs, obj name) {
    ir_var *t;
    if (is_atom(rhs)) return rhs;
    t = new_var(name, cur_lam);
    bind_let(b, t, rhs, rhs->loc);
    return mk_ref(t);
}
static ir_node *norm_value(obj x, bld *b) { return atomize(b, norm_rhs(x, b), sym("t")); }
static void norm_effect(obj x, bld *b) {
    ir_node *r = norm_rhs(x, b);
    if (!is_atom(r)) bind_let(b, NULL, r, r->loc);
}
static void put_tail(bld *b, ir_node *n) { *b->hole = n; b->hole = NULL; }
static ir_node *mk_ret(ir_node *a) { ir_node *r = node(IR_RET, 0); r->u.ret = a; return r; }

/* A global read is checked unless its definition has run: in the program body when the
 * defining form comes earlier; in a known procedure (and the lambdas inside it) when the
 * defining form comes before the first top-level form from which the procedure can run. */
static int global_needs_check(ir_var *v) {
    if (v->known) return 0;
    return !(v->def_index >= 0 && v->def_index < (in_static ? cur_ff : cur_form));
}
/* Value of a variable reference: an atom, or a %box-ref (checked through a temporary). */
static ir_node *ref_value(obj s, bld *b) {
    ir_var *v = var_of(s);
    ir_node *r = mk_ref(v);
    if (v->flags & V_GLOBAL) { r->u.ref.check = global_needs_check(v); return r; }
    if (v->flags & V_BOXED) {
        if (!(v->flags & V_CHECK)) return prim1(PI_rt_box_ref, r);
        r = atomize(b, prim1(PI_rt_box_ref, r), v->name);
    }
    if (v->flags & V_CHECK) r->u.ref.check = 1;
    return r;
}

/* Arity: which procedure (clause) a call with n arguments reaches, or NULL. */
static ir_lambda *resolve_clause(ir_lambda *L, int n) {
    int i;
    if (L->nclauses) {
        for (i = 0; i < L->nclauses; i++)
            if (resolve_clause(L->clauses[i], n)) return L->clauses[i];
        return NULL;
    }
    return n == L->nreq || (L->rest && n > L->nreq) ? L : NULL;
}
static const char *arity_str(ir_lambda *L) {
    sbuf b = {0};
    const char *r;
    int i;
    if (L->nclauses) {
        for (i = 0; i < L->nclauses; i++) sb_printf(&b, "%s%s", i ? " or " : "", arity_str(L->clauses[i]));
    } else sb_printf(&b, L->rest ? "at least %d" : "%d", L->nreq);
    r = arena_strdup(&compiler_arena, b.s);
    sb_free(&b);
    return r;
}

/* Procedure shell: arity and parameters (bodies are normalized later, so calls can be
 * resolved before the callee's body exists). Boxed parameters get a raw parameter
 * variable; the body starts by boxing it. */
static ir_lambda *shell(obj x, obj name, ir_lambda *parent) {
    ir_lambda *L = new_lambda(name, parent);
    obj f, l;
    int i, n = 0;
    if (RT_CAR(x) == S_case_lambda) {
        L->nclauses = list_len(RT_CDR(x));
        L->clauses = NEWN(ir_lambda *, L->nclauses);
        for (i = 0, l = RT_CDR(x); l != RT_NIL; l = RT_CDR(l), i++) {
            ir_lambda *c = shell(rt_cons(S_lambda, RT_CAR(l)), name, L);
            c->dispatch = L;
            L->clauses[i] = c;
        }
        return L;
    }
    for (f = CADR(x); rt_is_pair(f); f = RT_CDR(f)) n++;
    L->nreq = n;
    L->rest = f != RT_NIL;
    L->nparams = n + L->rest;
    L->params = NEWN(ir_var *, L->nparams);
    for (i = 0, f = CADR(x); i < L->nparams; i++) {
        obj s = i < n ? RT_CAR(f) : f;
        ir_var *v = var_of(s);
        if (v->flags & V_BOXED) L->params[i] = new_var(v->name, L);
        else (L->params[i] = v)->owner = L;
        if (i < n) f = RT_CDR(f);
    }
    return L;
}
static ir_node *closure(obj x, obj name) {
    ir_lambda *L = shell(x, name, cur_lam);
    ir_node *n = node(IR_LAMBDA, x);
    norm_lambda_body(L, x);
    n->u.lam = L;
    return n;
}
static void bind_var(bld *b, ir_var *v, ir_node *rhs, obj loc) {
    if (v->flags & V_BOXED) { bind_let(b, v, prim1(PI_rt_make_box, atomize(b, rhs, v->name)), loc); return; }
    if (rhs->kind == IR_LAMBDA && !(v->flags & V_ASSIGNED)) {
        v->known = rhs->u.lam;
        rhs->u.lam->self = v;
        rhs->u.lam->name = v->name;
    }
    bind_let(b, v, rhs, loc);
}

/* ((lambda formals body) args ...) binds like let; returns body. */
static obj norm_let(obj x, bld *b) {
    obj lam = RT_CAR(x), f = CADR(lam), a = RT_CDR(x);
    int nf = 0, na = list_len(a);
    for (; rt_is_pair(f); f = RT_CDR(f)) nf++;
    if (f == RT_NIL ? na != nf : na < nf)
        cerror(x, "procedure called with %d argument%s, but it takes %s%d", na, na == 1 ? "" : "s", f == RT_NIL ? "" : "at least ", nf);
    for (f = CADR(lam); rt_is_pair(f); f = RT_CDR(f), a = RT_CDR(a)) {
        obj saved = loc_ctx;
        if (has_loc(a)) loc_ctx = a;
        bind_var(b, var_of(RT_CAR(f)), norm_rhs(RT_CAR(a), b), a);
        loc_ctx = saved;
    }
    if (f != RT_NIL) { /* rest parameter: build the list */
        int n = list_len(a), i;
        ir_node **atoms = NEWN(ir_node *, n), *l = mk_const(RT_NIL);
        for (i = 0; i < n; i++, a = RT_CDR(a)) atoms[i] = norm_value(RT_CAR(a), b);
        for (i = n - 1; i >= 0; i--) l = atomize(b, prim2(PI_rt_cons, atoms[i], l), sym("rest"));
        bind_var(b, var_of(f), l, x);
    }
    return CADDR(lam);
}

/* letrec* (DESIGN.md §11): bindings are classified as simple (constants, references to
 * outer variables: hoisted), lambda (runs of consecutive lambdas become one FIX) or
 * complex (evaluated in order). A binding referenced by an earlier right-hand side (or
 * its own) is "early": bound to RT_UNDEF up front and assigned at its position; reads
 * of it that may happen before that are checked. */
enum { LR_SIMPLE, LR_LAMBDA, LR_COMPLEX };
static void first_refs(obj x, pmap *group, int k, int *first) {
    void *p;
    while (rt_is_pair(x)) {
        if (RT_CAR(x) == S_quote) return;
        first_refs(RT_CAR(x), group, k, first);
        x = RT_CDR(x);
    }
    if (rt_is_symbol(x) && (p = pm_get(group, x))) {
        int j = (int)((intptr_t)p - 1);
        if (first[j] > k) first[j] = k;
    }
}
static void norm_fix(bld *b, int n, ir_var **vs, obj *rhs, obj loc) {
    ir_node *fix = node(IR_FIX, loc);
    int i;
    fix->u.fix.n = n;
    fix->u.fix.vars = vs;
    fix->u.fix.lams = NEWN(ir_lambda *, n);
    for (i = 0; i < n; i++) {
        ir_lambda *L = shell(rhs[i], vs[i]->name, cur_lam);
        vs[i]->known = L;
        vs[i]->owner = cur_lam;
        L->self = vs[i];
        fix->u.fix.lams[i] = L;
    }
    *b->hole = fix;
    b->hole = &fix->u.fix.body;
    for (i = 0; i < n; i++) norm_lambda_body(fix->u.fix.lams[i], rhs[i]);
}
static void norm_letrec(obj x, bld *b) {
    obj bs = CADR(x), l;
    int n = list_len(bs), i, j, changed;
    ir_var **vs = NEWN(ir_var *, n);
    obj *rhs = NEWN(obj, n);
    int *kind = NEWN(int, n), *first = NEWN(int, n), *early = NEWN(int, n), *run = NEWN(int, n);
    pmap group = {0};
    for (i = 0, l = bs; i < n; i++, l = RT_CDR(l)) {
        vs[i] = var_of(RT_CAR(RT_CAR(l)));
        rhs[i] = CADR(RT_CAR(l));
        first[i] = n;
        pm_put(&group, RT_CAR(RT_CAR(l)), (void *)(intptr_t)(i + 1));
    }
    for (i = 0; i < n; i++) first_refs(rhs[i], &group, i, first);
    for (i = 0; i < n; i++) {
        obj r = rhs[i];
        if (is_lambda_form(r) && !(vs[i]->flags & V_ASSIGNED)) kind[i] = LR_LAMBDA;
        else if ((rt_is_pair(r) && RT_CAR(r) == S_quote) || (rt_is_symbol(r) && !pm_get(&group, r))) kind[i] = LR_SIMPLE;
        else kind[i] = LR_COMPLEX;
    }
    do { /* runs of lambdas; a lambda referenced before its run starts is complex */
        changed = 0;
        for (i = 0; i < n; i++) run[i] = kind[i] == LR_LAMBDA ? (i > 0 && kind[i - 1] == LR_LAMBDA ? run[i - 1] : i) : i;
        for (i = 0; i < n; i++) {
            early[i] = kind[i] == LR_LAMBDA ? first[i] < run[i] : kind[i] == LR_COMPLEX && first[i] <= i;
            if (kind[i] == LR_LAMBDA && early[i]) { kind[i] = LR_COMPLEX; changed = 1; }
        }
    } while (changed);
    for (i = 0; i < n; i++)
        if (kind[i] == LR_SIMPLE) bind_var(b, vs[i], norm_rhs(rhs[i], b), x);
    for (i = 0; i < n; i++)
        if (early[i]) {
            vs[i]->flags |= V_ASSIGNED | V_CHECK;
            if (vs[i]->flags & V_CAPTURED) vs[i]->flags |= V_BOXED;
            bind_let(b, vs[i], vs[i]->flags & V_BOXED ? prim1(PI_rt_make_box, mk_const(RT_UNDEF)) : mk_const(RT_UNDEF), x);
        }
    for (i = 0; i < n; i = j) {
        j = i + 1;
        if (kind[i] == LR_SIMPLE) continue;
        if (kind[i] == LR_LAMBDA) {
            while (j < n && kind[j] == LR_LAMBDA) j++;
            norm_fix(b, j - i, vs + i, rhs + i, x);
        } else if (early[i]) {
            ir_node *a = norm_value(rhs[i], b), *s;
            if (vs[i]->flags & V_BOXED) s = prim2(PI_rt_box_set, mk_ref(vs[i]), a);
            else { s = node(IR_SET, x); s->u.set.v = vs[i]; s->u.set.val = a; }
            bind_let(b, NULL, s, x);
            vs[i]->flags &= ~(unsigned)V_CHECK;
        } else bind_var(b, vs[i], norm_rhs(rhs[i], b), x);
    }
}

static ir_node *norm_call(obj x, bld *b, int tail) {
    int n = list_len(x) - 1, i;
    ir_node *fn = norm_value(RT_CAR(x), b), *c;
    ir_node **args = NEWN(ir_node *, n);
    ir_lambda *k = NULL;
    obj l;
    for (i = 0, l = RT_CDR(x); i < n; i++, l = RT_CDR(l)) {
        obj saved = loc_ctx;
        if (has_loc(l)) loc_ctx = l;
        args[i] = norm_value(RT_CAR(l), b);
        loc_ctx = saved;
    }
    if (fn->kind == IR_REF && fn->u.ref.v->known && !(fn->u.ref.v->flags & V_ASSIGNED)) {
        ir_lambda *L = fn->u.ref.v->known;
        if (!(k = resolve_clause(L, n)))
            cerror(x, "%s called with %d argument%s, but it takes %s", dstr(L->name), n, n == 1 ? "" : "s", arity_str(L));
        if (tail && k == cur_lam && !k->rest) { /* self tail call: jump to the entry */
            ir_node *j = node(IR_JUMP, x);
            if (!k->entry) {
                k->entry = NEW(ir_join);
                k->entry->id = njoins++;
                k->entry->n = k->nparams;
                k->entry->params = k->params;
                k->entry->owner = k;
            }
            j->u.jump.j = k->entry;
            j->u.jump.n = n;
            j->u.jump.args = args;
            return j;
        }
    }
    c = node(IR_CALL, x);
    c->u.call.fn = fn;
    c->u.call.n = n;
    c->u.call.args = args;
    c->u.call.known = k;
    c->u.call.tail = tail;
    return c;
}
static ir_node *norm_prim(obj x, bld *b, int tail) {
    int op = prim_index(RT_CAR(x)), n = list_len(x) - 1, i;
    ir_node *p;
    obj l;
    if (op == PI_CALLCC) {
        p = node(IR_CALL, x);
        p->u.call.ctl = CTL_CALLCC;
        p->u.call.n = 1;
        p->u.call.args = NEWN(ir_node *, 1);
        p->u.call.args[0] = norm_value(CADR(x), b);
        p->u.call.tail = tail;
        return p;
    }
    if (op == PI_APPLY || op == PI_CWV) {
        p = node(IR_CALL, x);
        p->u.call.ctl = op == PI_APPLY ? CTL_APPLY : CTL_CWV;
        p->u.call.n = 2;
        p->u.call.args = NEWN(ir_node *, 2);
        p->u.call.args[0] = norm_value(CADR(x), b);
        p->u.call.args[1] = norm_value(CADDR(x), b);
        p->u.call.tail = tail;
        return p;
    }
    if (op >= PI_EVAL) cerror(x, "%s is available only in the REPL", prim_table[op].name);
    p = mk_prim(op, n, x);
    for (i = 0, l = RT_CDR(x); i < n; i++, l = RT_CDR(l)) p->u.prim.args[i] = norm_value(RT_CAR(l), b);
    return p;
}

static ir_node *mk_jump(ir_join *j, int n, ir_node **args, obj loc) {
    ir_node *jn = node(IR_JUMP, loc);
    jn->u.jump.j = j;
    jn->u.jump.n = n;
    jn->u.jump.args = args;
    return jn;
}
static ir_node *mk_letj(bld *b, ir_join *j, obj loc) { /* *hole = LETJ j; hole -> its body */
    ir_node *n = node(IR_LETJ, loc);
    n->u.letj.j = j;
    *b->hole = n;
    b->hole = &n->u.letj.body;
    return n;
}

/* ---- tier 1 call/cc ---- */
static int is_kcall(obj x) { return rt_is_pair(x) && rt_is_symbol(RT_CAR(x)) && pm_get(&kctx, RT_CAR(x)); }
/* (k arg ...): jump to k's join point (or return) with the value(s); the rest of the
 * current term is dead. */
static void kjump(obj x, bld *b) {
    ir_join *j = (ir_join *)pm_get(&kctx, RT_CAR(x));
    int n = list_len(x) - 1, i;
    ir_node *v;
    obj l;
    if (n == 1) v = norm_value(CADR(x), b);
    else {
        ir_node *p = mk_prim(PI_VALUES, n, x);
        for (i = 0, l = RT_CDR(x); i < n; i++, l = RT_CDR(l)) p->u.prim.args[i] = norm_value(RT_CAR(l), b);
        v = atomize(b, p, sym("vals"));
    }
    if (j == &ret_ctx) put_tail(b, mk_ret(v));
    else {
        ir_node **a = NEWN(ir_node *, 1);
        a[0] = v;
        put_tail(b, mk_jump(j, 1, a, x));
    }
    b->hole = &dead_sink;
}
/* A tier-1 site: in tail position k returns; otherwise k jumps to a join point whose one
 * parameter is the site's value (packed values when k gets several). */
static ir_node *norm_callcc1(obj x, bld *b, int tail) {
    obj f = CADR(x), body = CADDR(f);
    ir_join *j;
    ir_node **a;
    bld bb;
    if (tail && tier1_kind(x) == 2) { /* optimistic: k may be called from a lambda, so a join */
        j = new_join(1);
        j->params[0] = new_var(sym("k"), cur_lam);
        j->site = x;
        mk_letj(b, j, x);
        pm_put(&kctx, RT_CAR(CADR(f)), j);
        norm_tail(body, b);
        j->body = mk_ret(mk_ref(j->params[0]));
        return NULL;
    }
    if (tail) {
        pm_put(&kctx, RT_CAR(CADR(f)), &ret_ctx);
        norm_tail(body, b);
        return NULL;
    }
    j = new_join(1);
    j->params[0] = new_var(sym("k"), cur_lam);
    if (tier1_kind(x) == 2) j->site = x;
    bb = *b;
    mk_letj(&bb, j, x);
    pm_put(&kctx, RT_CAR(CADR(f)), j);
    a = NEWN(ir_node *, 1);
    a[0] = norm_value(body, &bb);
    put_tail(&bb, mk_jump(j, 1, a, x));
    b->hole = &j->body;
    return mk_ref(j->params[0]);
}

/* ---- call-with-values with a literal receiver: a join point (spec §8.5) ----
 * (%call-with-values (lambda () body) (lambda formals rbody)): every tail of body that is
 * (values a ...) jumps straight to the receiver's join point with the atoms, no allocation;
 * other tails spread the (possibly packed) value with %values-check / %values-ref.
 * TODO(M4): known procedures returning several values could return a small C struct and
 * feed mv_spread directly; this is where that hooks in. */
typedef struct { ir_join *j; int nreq, rest; } mvctx;
static void mv_jump(bld *b, mvctx *m, ir_node **atoms, int n, obj loc) {
    ir_node **args = NEWN(ir_node *, m->nreq + m->rest), *l = mk_const(RT_NIL);
    int i;
    if (m->rest ? n < m->nreq : n != m->nreq)
        cerror(loc, "the receiver takes %s%d value%s, but %d %s produced", m->rest ? "at least " : "", m->nreq,
               m->nreq == 1 ? "" : "s", n, n == 1 ? "is" : "are");
    for (i = 0; i < m->nreq; i++) args[i] = atoms[i];
    if (m->rest) {
        for (i = n - 1; i >= m->nreq; i--) l = atomize(b, prim2(PI_rt_cons, atoms[i], l), sym("rest"));
        args[m->nreq] = l;
    }
    put_tail(b, mk_jump(m->j, m->nreq + m->rest, args, loc));
}
static void mv_spread(bld *b, mvctx *m, ir_node *v, obj loc) {
    ir_node **args = NEWN(ir_node *, m->nreq + m->rest), *chk = mk_prim(PI_rt_values_check, 3, loc);
    int i;
    chk->u.prim.args[0] = v;
    chk->u.prim.args[1] = mk_const(RT_FIX(m->nreq));
    chk->u.prim.args[2] = mk_const(RT_BOOL(m->rest));
    bind_let(b, NULL, chk, loc);
    if (m->nreq == 1 && !m->rest) args[0] = v; /* checked: a single value is itself */
    else for (i = 0; i < m->nreq; i++) args[i] = atomize(b, prim2(PI_rt_values_ref, v, mk_const(RT_FIX(i))), sym("val"));
    if (m->rest) args[m->nreq] = atomize(b, prim2(PI_rt_values_tail, v, mk_const(RT_FIX(m->nreq))), sym("rest"));
    put_tail(b, mk_jump(m->j, m->nreq + m->rest, args, loc));
}
static void norm_mv(obj x, bld *b, mvctx *m) {
    obj h, l;
    if (rt_is_pair(x)) {
        h = RT_CAR(x);
        if (h == S_if) {
            ir_node *test = norm_value(CADR(x), b), *ifn;
            obj els = list_len(x) == 4 ? RT_CAR(RT_CDR(CDDR(x))) : UNSPEC_FORM;
            bld bt, be;
            if (test->kind == IR_CONST) { norm_mv(test->u.value != RT_FALSE ? CADDR(x) : els, b, m); return; }
            ifn = node(IR_IF, x);
            ifn->u.if_.test = test;
            put_tail(b, ifn);
            bt.hole = &ifn->u.if_.then;
            norm_mv(CADDR(x), &bt, m);
            be.hole = &ifn->u.if_.els;
            norm_mv(els, &be, m);
            return;
        }
        if (h == S_begin) {
            for (l = RT_CDR(x); RT_CDR(l) != RT_NIL; l = RT_CDR(l)) norm_effect(RT_CAR(l), b);
            norm_mv(RT_CAR(l), b, m);
            return;
        }
        if (h == S_letrec_star) { norm_letrec(x, b); norm_mv(CADDR(x), b, m); return; }
        if (rt_is_pair(h) && RT_CAR(h) == S_lambda) { norm_mv(norm_let(x, b), b, m); return; }
        if (h == S_pvalues) {
            int n = list_len(x) - 1, i;
            ir_node **atoms = NEWN(ir_node *, n);
            for (i = 0, l = RT_CDR(x); i < n; i++, l = RT_CDR(l)) atoms[i] = norm_value(RT_CAR(l), b);
            mv_jump(b, m, atoms, n, x);
            return;
        }
    }
    mv_spread(b, m, norm_value(x, b), x);
}
static ir_node *norm_cwv(obj x, bld *b, int tail) {
    obj p = CADR(x), c = CADDR(x), f;
    ir_join *jk = NULL;
    ir_node **a;
    mvctx m;
    bld pb, cb;
    int i;
    m.nreq = 0;
    for (f = CADR(c); rt_is_pair(f); f = RT_CDR(f)) m.nreq++;
    m.rest = f != RT_NIL;
    m.j = new_join(m.nreq + m.rest);
    for (i = 0, f = CADR(c); i < m.j->n; i++) { /* boxed formals get a raw parameter */
        ir_var *v = var_of(i < m.nreq ? RT_CAR(f) : f);
        if (v->flags & V_BOXED) m.j->params[i] = new_var(v->name, cur_lam);
        else (m.j->params[i] = v)->owner = cur_lam;
        if (i < m.nreq) f = RT_CDR(f);
    }
    if (!tail) {
        jk = new_join(1);
        jk->params[0] = new_var(sym("v"), cur_lam);
        mk_letj(b, jk, x);
    }
    pb = *b;
    mk_letj(&pb, m.j, x);
    if (is_thunk(p)) norm_mv(CADDR(p), &pb, &m);
    else mv_spread(&pb, &m, norm_value(setloc(rt_cons(p, RT_NIL), x), &pb), x);
    cb.hole = &m.j->body;
    for (i = 0, f = CADR(c); i < m.j->n; i++) {
        ir_var *v = var_of(i < m.nreq ? RT_CAR(f) : f);
        if (v->flags & V_BOXED) bind_let(&cb, v, prim1(PI_rt_make_box, mk_ref(m.j->params[i])), x);
        if (i < m.nreq) f = RT_CDR(f);
    }
    if (tail) { norm_tail(CADDR(c), &cb); return NULL; }
    a = NEWN(ir_node *, 1);
    a[0] = norm_value(CADDR(c), &cb);
    put_tail(&cb, mk_jump(jk, 1, a, x));
    b->hole = &jk->body;
    return mk_ref(jk->params[0]);
}

static ir_node *norm_rhs(obj x, bld *b) {
    obj h, l;
    if (rt_is_symbol(x)) return ref_value(x, b);
    if (!rt_is_pair(x)) return mk_const(x);
    h = RT_CAR(x);
    if (h == S_quote) return mk_const(CADR(x));
    if (h == S_lambda || h == S_case_lambda) return closure(x, sym("lambda"));
    if (h == S_set) {
        ir_var *v = var_of(CADR(x));
        ir_node *a = norm_value(CADDR(x), b), *s;
        if (v->flags & V_BOXED) return prim2(PI_rt_box_set, mk_ref(v), a);
        s = node(IR_SET, x);
        s->u.set.v = v;
        s->u.set.val = a;
        return s;
    }
    if (h == S_begin) {
        for (l = RT_CDR(x); RT_CDR(l) != RT_NIL; l = RT_CDR(l)) norm_effect(RT_CAR(l), b);
        return norm_rhs(RT_CAR(l), b);
    }
    if (h == S_letrec_star) { norm_letrec(x, b); return norm_rhs(CADDR(x), b); }
    if (h == S_if) { /* value context: a join point with one parameter */
        ir_node *test = norm_value(CADR(x), b), *ifn, *letj;
        obj els = list_len(x) == 4 ? RT_CAR(RT_CDR(CDDR(x))) : UNSPEC_FORM;
        ir_join *j;
        bld bt, be;
        ir_node *jt, *je;
        if (test->kind == IR_CONST) return norm_rhs(test->u.value != RT_FALSE ? CADDR(x) : els, b);
        j = new_join(1);
        j->params[0] = new_var(sym("v"), cur_lam);
        letj = node(IR_LETJ, x);
        letj->u.letj.j = j;
        ifn = node(IR_IF, x);
        ifn->u.if_.test = test;
        letj->u.letj.body = ifn;
        *b->hole = letj;
        bt.hole = &ifn->u.if_.then;
        jt = node(IR_JUMP, x);
        jt->u.jump.j = j;
        jt->u.jump.n = 1;
        jt->u.jump.args = NEWN(ir_node *, 1);
        jt->u.jump.args[0] = norm_value(CADDR(x), &bt);
        put_tail(&bt, jt);
        be.hole = &ifn->u.if_.els;
        je = node(IR_JUMP, x);
        je->u.jump.j = j;
        je->u.jump.n = 1;
        je->u.jump.args = NEWN(ir_node *, 1);
        je->u.jump.args[0] = norm_value(els, &be);
        put_tail(&be, je);
        b->hole = &j->body;
        return mk_ref(j->params[0]);
    }
    if (h == S_define) cerror(x, "internal error: define in expression");
    if (is_kcall(x)) { kjump(x, b); return mk_const(RT_UNSPEC); }
    if (tier1_site(x)) return norm_callcc1(x, b, 0);
    if (plain_callcc(x)) return norm_prim(setloc(rt_cons(S_pcallcc, RT_CDR(x)), x), b, 0);
    if (cwv_inline(x)) return norm_cwv(x, b, 0);
    if (prim_index(h) >= 0) return norm_prim(x, b, 0);
    if (rt_is_pair(h) && RT_CAR(h) == S_lambda) return norm_rhs(norm_let(x, b), b);
    return norm_call(x, b, 0);
}

static void norm_tail(obj x, bld *b) {
    obj h, l;
    if (rt_is_pair(x)) {
        h = RT_CAR(x);
        if (h == S_if) {
            ir_node *test = norm_value(CADR(x), b), *ifn;
            obj els = list_len(x) == 4 ? RT_CAR(RT_CDR(CDDR(x))) : UNSPEC_FORM;
            bld bt, be;
            if (test->kind == IR_CONST) { norm_tail(test->u.value != RT_FALSE ? CADDR(x) : els, b); return; }
            ifn = node(IR_IF, x);
            ifn->u.if_.test = test;
            put_tail(b, ifn);
            bt.hole = &ifn->u.if_.then;
            norm_tail(CADDR(x), &bt);
            be.hole = &ifn->u.if_.els;
            norm_tail(els, &be);
            return;
        }
        if (h == S_begin) {
            for (l = RT_CDR(x); RT_CDR(l) != RT_NIL; l = RT_CDR(l)) norm_effect(RT_CAR(l), b);
            norm_tail(RT_CAR(l), b);
            return;
        }
        if (h == S_letrec_star) { norm_letrec(x, b); norm_tail(CADDR(x), b); return; }
        if (rt_is_pair(h) && RT_CAR(h) == S_lambda) { norm_tail(norm_let(x, b), b); return; }
        if (is_kcall(x)) { kjump(x, b); return; }
        if (tier1_site(x)) { norm_callcc1(x, b, 1); return; }
        if (plain_callcc(x)) { put_tail(b, norm_prim(setloc(rt_cons(S_pcallcc, RT_CDR(x)), x), b, 1)); return; }
        if (cwv_inline(x)) { norm_cwv(x, b, 1); return; }
        if (h != S_quote && h != S_lambda && h != S_case_lambda && h != S_set && h != S_define) {
            int p = prim_index(h);
            if (p < 0 || p == PI_APPLY || p == PI_CWV || p == PI_CALLCC) {
                put_tail(b, p < 0 ? norm_call(x, b, 1) : norm_prim(x, b, 1));
                return;
            }
        }
    }
    put_tail(b, mk_ret(atomize(b, norm_rhs(x, b), sym("t"))));
}

static void norm_lambda_body(ir_lambda *L, obj x) {
    ir_lambda *saved = cur_lam;
    bld bb;
    obj f;
    int i;
    if (L->nclauses) {
        obj l;
        for (i = 0, l = RT_CDR(x); i < L->nclauses; i++, l = RT_CDR(l)) norm_lambda_body(L->clauses[i], rt_cons(S_lambda, RT_CAR(l)));
        return;
    }
    cur_lam = L;
    bb.hole = &L->body;
    for (i = 0, f = CADR(x); i < L->nparams; i++) {
        obj s = i < L->nreq ? RT_CAR(f) : f;
        ir_var *v = var_of(s);
        if (v->flags & V_BOXED) bind_let(&bb, v, prim1(PI_rt_make_box, mk_ref(L->params[i])), x);
        if (i < L->nreq) f = RT_CDR(f);
    }
    norm_tail(CADDR(x), &bb);
    cur_lam = saved;
}

/* C names: P_<library>__<procedure>[__<inner>...], unique. */
void c_mangle(sbuf *b, const char *s) {
    static const char *const ops[][2] = {{"+", "plus"}, {"-", "minus"}, {"*", "times"}, {"/", "div"},
        {"<", "lt"}, {">", "gt"}, {"=", "eq"}, {"<=", "le"}, {">=", "ge"}, {"...", "ellipsis"}, {0, 0}};
    int i;
    for (i = 0; ops[i][0]; i++) if (!strcmp(s, ops[i][0])) { sb_puts(b, ops[i][1]); return; }
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) sb_putc(b, c);
        else if (c == '-' || c == '/' || c == '.' || c == '_') sb_putc(b, '_');
        else if (c == '?') sb_puts(b, "_p");
        else if (c == '!') sb_puts(b, "_x");
        else if (c == '*') sb_puts(b, "_star");
        else if (c == '<') sb_puts(b, "_lt");
        else if (c == '>') sb_puts(b, "_gt");
        else if (c == '=') sb_puts(b, "_eq");
        else if (c == '+') sb_puts(b, "_plus");
        else if (c == '%') sb_puts(b, "_pct");
        else sb_printf(b, "_%02x", c);
    }
}
static htab used_names;
static void name_lambda(ir_lambda *L) {
    sbuf b = {0};
    int k = 1;
    size_t len;
    if (L->cname) return;
    if (L == prog->main) sb_puts(&b, "P_init");
    else if (L->dispatch) {
        int i;
        name_lambda(L->dispatch);
        for (i = 0; L->dispatch->clauses[i] != L; i++) {}
        sb_printf(&b, "%s__%d", L->dispatch->cname, i + 1);
    } else {
        if (!L->parent || L->parent == prog->main) {
            sb_puts(&b, "P_");
            c_mangle(&b, L->lib ? L->lib->id : "main");
        } else {
            name_lambda(L->parent);
            sb_puts(&b, L->parent->cname);
        }
        sb_puts(&b, "__");
        c_mangle(&b, RT_STR(RT_PTR(L->name, rt_symbol)->name)->buf);
    }
    len = b.len;
    while (ht_get(&used_names, b.s)) { b.len = len; sb_printf(&b, "_%d", ++k); }
    L->cname = arena_strdup(&compiler_arena, b.s);
    ht_put(&used_names, L->cname, L);
    sb_free(&b);
}
/* C names: P_<library>__<procedure>[__<inner>...], unique; in creation order, so the
 * outermost procedure of a name gets it unsuffixed. */
static void name_lambdas(void) {
    ir_lambda *L;
    for (L = prog->lambdas; L; L = L->next) name_lambda(L);
}

/* ---- def/use counts ----
 * A jump argument, or the variable rhs of a LET, is a use only if the variable it is copied
 * into is used (the emitter drops such copies otherwise): these are counted in a fixpoint. */
typedef struct { ir_var *to; ir_node *atom; } pending;
static pending *pend;
static int npend, pend_cap;
static void add_pending(ir_var *to, ir_node *atom) {
    if (npend == pend_cap) {
        pending *np = NEWN(pending, pend_cap * 2 + 64);
        if (pend) memcpy(np, pend, sizeof(pending) * (size_t)npend);
        pend = np;
        pend_cap = pend_cap * 2 + 64;
    }
    pend[npend].to = to;
    pend[npend++].atom = atom;
}
static void count_node(ir_node *n) {
    int i;
    if (!n) return;
    switch (n->kind) {
    case IR_CONST: return;
    case IR_REF: n->u.ref.v->nrefs++; return;
    case IR_PRIM: for (i = 0; i < n->u.prim.n; i++) count_node(n->u.prim.args[i]); return;
    case IR_CALL:
        if (n->u.call.fn && ir_call_self(n) == 1) count_node(n->u.call.fn); /* else no self argument */
        for (i = 0; i < n->u.call.n; i++) count_node(n->u.call.args[i]);
        return;
    case IR_LAMBDA: {
        ir_lambda *L = n->u.lam;
        for (i = 0; i < L->nfree; i++) L->free[i]->nrefs++;
        return;
    }
    case IR_SET: n->u.set.v->nsets++; count_node(n->u.set.val); return;
    case IR_LET:
        if (n->u.let.v && n->u.let.rhs->kind == IR_REF && !n->u.let.rhs->u.ref.check) add_pending(n->u.let.v, n->u.let.rhs);
        else count_node(n->u.let.rhs);
        count_node(n->u.let.body);
        return;
    case IR_FIX: {
        int k;
        for (k = 0; k < n->u.fix.n; k++)
            for (i = 0; i < n->u.fix.lams[k]->nfree; i++) n->u.fix.lams[k]->free[i]->nrefs++;
        count_node(n->u.fix.body);
        return;
    }
    case IR_LETJ: {
        ir_join *j;
        for (j = n->u.letj.j; j; j = j->next) count_node(j->body);
        count_node(n->u.letj.body);
        return;
    }
    case IR_JUMP: /* arguments are counted later, only for parameters that are used */
        n->u.jump.j->njumps++;
        for (i = 0; i < n->u.jump.n; i++) add_pending(n->u.jump.j->params[i], n->u.jump.args[i]);
        return;
    case IR_IF: count_node(n->u.if_.test); count_node(n->u.if_.then); count_node(n->u.if_.els); return;
    case IR_RET: count_node(n->u.ret); return;
    }
}
static void reset_joins(ir_node *n) {
    for (; n; ) {
        switch (n->kind) {
        case IR_LET: n = n->u.let.body; continue;
        case IR_FIX: n = n->u.fix.body; continue;
        case IR_LETJ: {
            ir_join *j;
            for (j = n->u.letj.j; j; j = j->next) { j->njumps = 0; reset_joins(j->body); }
            n = n->u.letj.body;
            continue;
        }
        case IR_IF: reset_joins(n->u.if_.then); n = n->u.if_.els; continue;
        default: return;
        }
    }
}
void ir_count(ir_program *p) {
    ir_lambda *L;
    int i;
    for (i = 0; i < p->nvars; i++) p->vars[i]->nrefs = p->vars[i]->nsets = 0;
    for (L = p->lambdas; L; L = L->next) {
        if (L->entry) L->entry->njumps = 0;
        reset_joins(L->body);
    }
    npend = 0;
    for (L = p->lambdas; L; L = L->next) count_node(L->body);
    {   /* fixpoint: count a pending atom once its target variable is used */
        char changed = 1, *done = NEWN(char, npend);
        while (changed) {
            changed = 0;
            for (i = 0; i < npend; i++)
                if (!done[i] && pend[i].to->nrefs > 0) {
                    done[i] = 1;
                    count_node(pend[i].atom);
                    changed = 1;
                }
        }
    }
}

/* ---- driver ---- */
/* Forget the IR of a previous round (the core forms and the shaking results stay). */
static void reset_round(void) {
    int i;
    for (i = 0; i < ngwork; i++) gwork[i]->mid = NULL;
    for (i = 0; i < ntops; i++) if (tops[i].g) tops[i].g->mid = NULL;
    memset(&varmap, 0, sizeof varmap);
    memset(&kctx, 0, sizeof kctx);
    memset(&t1memo, 0, sizeof t1memo);
    nvars = nlams = njoins = lamtok = vscope_cap = 0;
    vscope = NULL;
    cur_lam = last_lam = NULL;
    cur_lib = NULL;
    cur_form = in_static = 0;
    dead_sink = NULL;
}
static void normalize_program(library *order) {
    int i, k;
    global *kg;
    ir_lambda *L;
    prog = NEW(ir_program);
    prog->opt = opts->opt;
    for (i = 0; i < ntops; i++) {
        if (!tops[i].keep) continue;
        if (tops[i].g) gvar(tops[i].g)->def_index = i;
        analyse(tops[i].form, 0);
    }
    for (i = 0; i < nvars; i++) { /* variables both assigned and captured live in boxes */
        ir_var *v = prog->vars[i];
        if (!(v->flags & V_GLOBAL) && (v->flags & V_ASSIGNED) && (v->flags & V_CAPTURED)) v->flags |= V_BOXED;
    }
    cur_lib = NULL;
    prog->main = new_lambda(sym("main"), NULL);
    /* 3. known top-level procedures: shells first, so every call can be resolved */
    for (i = 0; i < ntops; i++) {
        topform *t = &tops[i];
        global *g = t->g;
        if (known_def(t)) {
            ir_var *v = gvar(g);
            cur_lib = t->lib;
            L = shell(CADDR(t->form), g->name, NULL);
            L->g = g;
            L->self = v;
            v->known = L;
        }
    }
    /* 4. the program body, then every known procedure */
    {
        bld b;
        cur_lam = prog->main;
        b.hole = &prog->main->body;
        for (i = 0; i < ntops; i++) {
            topform *t = &tops[i];
            obj saved = loc_ctx;
            if (!t->keep) continue;
            cur_form = i;
            cur_lib = t->lib;
            if (has_loc(t->form)) loc_ctx = t->form;
            if (t->g) {
                ir_var *v = gvar(t->g);
                if (!v->known) {
                    ir_node *r = norm_rhs(CADDR(t->form), &b), *s;
                    if (r->kind == IR_LAMBDA) r->u.lam->name = t->g->name;
                    s = node(IR_SET, t->form);
                    s->u.set.v = v;
                    s->u.set.val = atomize(&b, r, t->g->name);
                    bind_let(&b, NULL, s, t->form);
                }
            } else norm_effect(t->form, &b);
            loc_ctx = saved;
        }
        put_tail(&b, mk_ret(mk_const(RT_UNSPEC)));
        in_static = 1;
        for (i = 0; i < ntops; i++) {
            topform *t = &tops[i];
            obj saved = loc_ctx;
            intptr_t ff;
            if (!t->keep || !t->g || !gvar(t->g)->known) continue;
            cur_lib = t->lib;
            ff = (intptr_t)pm_get(&ffmap, t->g);
            cur_ff = ff ? (int)ff - 1 : 0x7fffffff; /* never reached: dropped later */
            if (has_loc(t->form)) loc_ctx = t->form;
            norm_lambda_body(gvar(t->g)->known, CADDR(t->form));
            loc_ctx = saved;
        }
        in_static = 0;
    }
    /* globals: every reachable one */
    prog->globals = NEWN(ir_var *, ngwork);
    for (k = 0; k < ngwork; k++) prog->globals[prog->nglobals++] = gvar(gwork[k]);
    if (g_raise && gvar(g_raise)->known) (prog->raise = gvar(g_raise)->known)->keep = 1;
    if (g_travel && g_travel->mid && gvar(g_travel)->known) (prog->travel = gvar(g_travel)->known)->keep = 1;
    for (i = 0; i < nkeep(); i++)
        if ((kg = keep_global(order, i)) && gvar(kg)->known) gvar(kg)->known->keep = 1;
}

ir_program *ir_build(library *order, const ir_opts *o) {
    int i;
    opts = o;
    UNSPEC_FORM = L2(S_quote, RT_UNSPEC);
    for (i = 0; i < NPRIMS_ALL; i++) pm_put(&primmap, sym(prim_table[i].name), (void *)(intptr_t)(i + 1));
    S_pvalues = sym("%values");
    S_pcwv = sym("%call-with-values");
    S_papply = sym("%apply");
    g_values = base_global(order, "values");
    g_cwv = base_global(order, "call-with-values");
    g_apply = base_global(order, "apply");
    g_callcc1 = base_global(order, "call-with-current-continuation");
    g_callcc2 = base_global(order, "call/cc");
    g_raise = base_global(order, "raise");
    S_pcallcc = sym("%call/cc");
    {
        library *l;
        binding *b;
        for (l = order; l; l = l->next)
            if (!strcmp(l->id, "shikra/dynamic") && (b = (binding *)pm_get(&l->env->map, sym("travel-to!"))) && b->kind == B_GLOBAL)
                g_travel = b->g;
    }
    {
        library *l;
        obj f;
        for (l = order; l; l = l->next)
            for (f = l->core; f != RT_NIL; f = RT_CDR(f)) rewrite_aliases(RT_CAR(f));
    }
    shake(order);
    first_forms(order);
    if (o->warn) warn_unused(order);
    /* Optimistic tier-1 sites whose continuation is still reached from another C function
     * after optimization are rebuilt as tier 2 (at most a few rounds: each demotes a site). */
    /* Then the flow analysis (analyze.c): while it rewrites the IR structurally (decided branches,
     * dead code, new known calls) the optimizer runs again (at most three times), then a last
     * analysis gives the final types and the diagnostics. */
    for (;;) {
        obj fails, l;
        int k;
        normalize_program(order);
        prog->unsafe = o->unsafe;
        prog->warn = o->warn;
        ir_optimize(prog);
        fails = ir_tier1_failures(prog);
        for (k = 0; fails == RT_NIL && k < 3 && !getenv("SHIKRA_NOANALYZE") && ir_analyze(prog, 0); k++) {
            ir_optimize(prog);
            fails = ir_tier1_failures(prog);
        }
        if (fails == RT_NIL) break;
        for (l = fails; l != RT_NIL; l = RT_CDR(l)) pm_put(&demoted, RT_CAR(l), (void *)1);
        reset_round();
    }
    if (!getenv("SHIKRA_NOANALYZE")) ir_analyze(prog, 1);
    ir_finish(prog);
    ir_represent(prog);
    name_lambdas();
    ir_count(prog);
    return prog;
}

/* ---- printer (shikra debug-ir) ---- */
static void pr_indent(obj port, int d) { while (d-- > 0) rt_port_puts(port, "  "); }
static void pr_var(ir_var *v, obj port) {
    char buf[24];
    if (v->flags & V_GLOBAL) { rt_print(v->g->sym, port, 1); return; }
    rt_print(v->name, port, 1);
    snprintf(buf, sizeof buf, ".%d", v->id);
    rt_port_puts(port, buf);
}
static void pr_atom(ir_node *n, obj port) {
    if (n->kind == IR_CONST) {
        if (rt_is_symbol(n->u.value) || rt_is_pair(n->u.value) || n->u.value == RT_NIL) rt_port_putc(port, '\'');
        rt_print(n->u.value, port, 1);
    } else {
        pr_var(n->u.ref.v, port);
        if (n->u.ref.check) rt_port_putc(port, '!');
    }
}
static void pr_args(ir_node **a, int n, obj port) {
    int i;
    for (i = 0; i < n; i++) { rt_port_putc(port, ' '); pr_atom(a[i], port); }
}
static void pr_term(ir_node *n, obj port, int d);
static void pr_rhs(ir_node *n, obj port) {
    switch (n->kind) {
    case IR_PRIM:
        rt_port_putc(port, '(');
        rt_port_puts(port, prim_table[n->u.prim.op].name);
        pr_args(n->u.prim.args, n->u.prim.n, port);
        rt_port_putc(port, ')');
        return;
    case IR_CALL:
        rt_port_puts(port, n->u.call.tail ? "(tailcall" : "(call");
        if (n->u.call.ctl == CTL_APPLY) rt_port_puts(port, "/apply");
        if (n->u.call.ctl == CTL_CWV) rt_port_puts(port, "/cwv");
        if (n->u.call.known) {
            rt_port_puts(port, " [");
            rt_port_puts(port, n->u.call.known->cname ? n->u.call.known->cname : "?dead");
            if (n->u.call.known->direct) rt_port_puts(port, " direct");
            rt_port_putc(port, ']');
        }
        if (n->u.call.fn) { rt_port_putc(port, ' '); pr_atom(n->u.call.fn, port); }
        pr_args(n->u.call.args, n->u.call.n, port);
        rt_port_putc(port, ')');
        return;
    case IR_LAMBDA: rt_port_puts(port, "(closure "); rt_port_puts(port, n->u.lam->cname); rt_port_putc(port, ')'); return;
    case IR_SET:
        rt_port_puts(port, "(set! ");
        pr_var(n->u.set.v, port);
        rt_port_putc(port, ' ');
        pr_atom(n->u.set.val, port);
        rt_port_putc(port, ')');
        return;
    default: pr_atom(n, port); return;
    }
}
static void pr_term(ir_node *n, obj port, int d) {
    int i;
    for (; n; ) {
        if (n->kind != IR_LETJ) pr_indent(port, d);
        switch (n->kind) {
        case IR_LET:
            rt_port_puts(port, "(let ");
            if (n->u.let.v) pr_var(n->u.let.v, port);
            else rt_port_putc(port, '_');
            rt_port_puts(port, " = ");
            pr_rhs(n->u.let.rhs, port);
            rt_port_puts(port, ")\n");
            n = n->u.let.body;
            continue;
        case IR_FIX:
            rt_port_puts(port, "(fix");
            for (i = 0; i < n->u.fix.n; i++) {
                rt_port_puts(port, " (");
                pr_var(n->u.fix.vars[i], port);
                rt_port_putc(port, ' ');
                rt_port_puts(port, n->u.fix.lams[i]->cname);
                rt_port_putc(port, ')');
            }
            rt_port_puts(port, ")\n");
            n = n->u.fix.body;
            continue;
        case IR_LETJ: {
            ir_join *j;
            char buf[32];
            pr_term(n->u.letj.body, port, d);
            for (j = n->u.letj.j; j; j = j->next) {
                pr_indent(port, d);
                snprintf(buf, sizeof buf, "j%d (", j->id);
                rt_port_puts(port, buf);
                for (i = 0; i < j->n; i++) { if (i) rt_port_putc(port, ' '); pr_var(j->params[i], port); }
                rt_port_puts(port, "):\n");
                if (j->next) pr_term(j->body, port, d + 1);
                else n = j->body;
            }
            continue;
        }
        case IR_IF:
            rt_port_puts(port, "(if ");
            pr_atom(n->u.if_.test, port);
            rt_port_puts(port, "\n");
            pr_term(n->u.if_.then, port, d + 2);
            pr_term(n->u.if_.els, port, d + 2);
            pr_indent(port, d);
            rt_port_puts(port, ")\n");
            return;
        case IR_JUMP: {
            char buf[32];
            snprintf(buf, sizeof buf, "(jump j%d", n->u.jump.j->id);
            rt_port_puts(port, buf);
            pr_args(n->u.jump.args, n->u.jump.n, port);
            rt_port_puts(port, ")\n");
            return;
        }
        case IR_RET: rt_port_puts(port, "(return "); pr_atom(n->u.ret, port); rt_port_puts(port, ")\n"); return;
        default: pr_rhs(n, port); rt_port_putc(port, '\n'); return;
        }
    }
}
void ir_print(ir_program *p, obj port) {
    ir_lambda *L;
    int i;
    for (L = p->lambdas; L; L = L->next) {
        rt_port_puts(port, "(proc ");
        rt_port_puts(port, L->cname);
        if (L->nclauses) {
            rt_port_puts(port, " case-lambda");
            for (i = 0; i < L->nclauses; i++) { rt_port_putc(port, ' '); rt_port_puts(port, L->clauses[i]->cname); }
        } else {
            rt_port_puts(port, " (");
            for (i = 0; i < L->nparams; i++) {
                if (i) rt_port_puts(port, i == L->nreq ? " . " : " ");
                pr_var(L->params[i], port);
            }
            rt_port_putc(port, ')');
        }
        if (L->nfree) {
            rt_port_puts(port, " free:");
            for (i = 0; i < L->nfree; i++) { rt_port_putc(port, ' '); pr_var(L->free[i], port); }
        }
        if (L->is_static) rt_port_puts(port, " static");
        if (L->direct) rt_port_puts(port, " direct");
        rt_port_putc(port, '\n');
        if (L->entry && L->entry->njumps) {
            char buf[32];
            snprintf(buf, sizeof buf, "  j%d (entry):\n", L->entry->id);
            rt_port_puts(port, buf);
        }
        pr_term(L->body, port, 1);
        rt_port_puts(port, ")\n");
    }
}
