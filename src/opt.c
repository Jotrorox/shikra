/* opt.c -- the structural optimizer (spec §5 pass 6, §8.1, §8.2, §9; DESIGN.md §11 M4-1).
 *
 * Passes over the ANF IR (ir.h), in rounds until nothing changes (budgets per -O level):
 *   census     liveness from the roots (program body, raise, --keep), def/use and call
 *              counts, owners of variables and join points, parents of procedures
 *   recursion  strongly connected components of the reference graph: a procedure that
 *              can reach itself is never inlined (termination)
 *   simplify   copy and constant propagation (top-level constants too), folding of pure
 *              primitives and of `if` on constants, dead bindings, join points with one
 *              jump, inlining of known calls (small, or the only call)
 *   contify    procedures always called with the same continuation (named let, do, local
 *              state machines, helpers called from one place) become join points
 *   unbox      boxes of assigned variables that are no longer captured become C locals
 * ir_tier1_failures checks the optimistic tier-1 call/cc sites; ir_finish computes escapes,
 * the tail-call graph, direct signatures, lambda lifting and the closure layout. */
#include "ir.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ir_program *P;
static int changed, round_no;
static int lim_small, lim_once;  /* inline a callee up to this size; the only call up to this size */
static long growth;              /* inlining budget left (nodes copied) */

/* ---- constructors (keep counts: never lower than the truth) ---- */
static int is_atom(ir_node *n) { return n->kind == IR_CONST || n->kind == IR_REF; }
static ir_node *mk_const(obj v) { ir_node *n = ir_new_node(IR_CONST, 0); n->u.value = v; return n; }
static ir_node *mk_ref(ir_var *v) { ir_node *n = ir_new_node(IR_REF, 0); n->u.ref.v = v; v->nrefs++; return n; }
static ir_node *mk_let(ir_var *v, ir_node *rhs, ir_node *body, obj loc) {
    ir_node *n = ir_new_node(IR_LET, loc);
    n->u.let.v = v;
    n->u.let.rhs = rhs;
    n->u.let.body = body;
    return n;
}
static ir_node *mk_ret(ir_node *a) { ir_node *n = ir_new_node(IR_RET, 0); n->u.ret = a; return n; }
static ir_node *mk_jump(ir_join *j, int n, ir_node **args, obj loc) {
    ir_node *x = ir_new_node(IR_JUMP, loc);
    x->u.jump.j = j;
    x->u.jump.n = n;
    x->u.jump.args = args;
    j->njumps++;
    return x;
}
static ir_node *mk_jump1(ir_join *j, ir_node *a, obj loc) {
    ir_node **v = NEWN(ir_node *, 1);
    v[0] = a;
    return mk_jump(j, 1, v, loc);
}
static ir_node *mk_letj(ir_join *j, ir_node *body, obj loc) {
    ir_node *n = ir_new_node(IR_LETJ, loc);
    n->u.letj.j = j;
    n->u.letj.body = body;
    return n;
}
static ir_lambda *real(ir_lambda *L) { return L->dispatch ? L->dispatch : L; }
static ir_lambda *resolve(ir_lambda *L, int n) {
    int i;
    if (L->nclauses) {
        for (i = 0; i < L->nclauses; i++) if (resolve(L->clauses[i], n)) return L->clauses[i];
        return NULL;
    }
    return n == L->nreq || (L->rest && n > L->nreq) ? L : NULL;
}

/* ---- census ---- */
static ir_lambda **work;
static int nwork, work_cap;
static void live(ir_lambda *L, ir_lambda *parent) {
    if (L->mark || L->dead) return;
    L->mark = 1;
    if (parent) L->parent = parent;
    if (nwork == work_cap) {
        ir_lambda **n = NEWN(ir_lambda *, work_cap * 2 + 64);
        if (work) memcpy(n, work, sizeof(ir_lambda *) * (size_t)nwork);
        work = n;
        work_cap = work_cap * 2 + 64;
    }
    work[nwork++] = L;
}
static void cen_atom(ir_node *a) {
    if (a->kind == IR_REF) {
        ir_var *v = a->u.ref.v;
        v->nrefs++;
        if ((v->flags & V_GLOBAL) && v->known) live(v->known, NULL);
    }
}
static void cen_term(ir_node *n, ir_lambda *L) {
    int i;
    for (; n; ) {
        switch (n->kind) {
        case IR_CONST: case IR_REF: cen_atom(n); return;
        case IR_PRIM: for (i = 0; i < n->u.prim.n; i++) cen_atom(n->u.prim.args[i]); return;
        case IR_CALL:
            if (n->u.call.fn) {
                if (n->u.call.known && n->u.call.fn->kind == IR_REF) {
                    n->u.call.fn->u.ref.v->nrefs++;
                    n->u.call.fn->u.ref.v->ncalls++;
                } else cen_atom(n->u.call.fn);
            }
            for (i = 0; i < n->u.call.n; i++) cen_atom(n->u.call.args[i]);
            if (n->u.call.known) live(n->u.call.known, NULL);
            return;
        case IR_LAMBDA: live(n->u.lam, L); return;
        case IR_SET: n->u.set.v->nsets++; cen_atom(n->u.set.val); return;
        case IR_LET: {
            ir_var *v = n->u.let.v;
            ir_node *r = n->u.let.rhs;
            if (v) v->owner = L;
            if (v && r->kind == IR_LAMBDA && !(v->flags & (V_ASSIGNED | V_BOXED))) {
                v->known = r->u.lam; /* temporaries holding lambdas are known callees too */
                if (!r->u.lam->self) r->u.lam->self = v;
            }
            cen_term(r, L);
            n = n->u.let.body;
            continue;
        }
        case IR_FIX:
            for (i = 0; i < n->u.fix.n; i++) { n->u.fix.vars[i]->owner = L; live(n->u.fix.lams[i], L); }
            n = n->u.fix.body;
            continue;
        case IR_LETJ: {
            ir_join *j;
            for (j = n->u.letj.j; j; j = j->next) {
                j->owner = L;
                for (i = 0; i < j->n; i++) j->params[i]->owner = L;
            }
            for (j = n->u.letj.j; j; j = j->next) cen_term(j->body, L);
            n = n->u.letj.body;
            continue;
        }
        case IR_JUMP:
            n->u.jump.j->njumps++;
            for (i = 0; i < n->u.jump.n; i++) cen_atom(n->u.jump.args[i]);
            return;
        case IR_IF: cen_atom(n->u.if_.test); cen_term(n->u.if_.then, L); n = n->u.if_.els; continue;
        case IR_RET: cen_atom(n->u.ret); return;
        }
    }
}
/* Join points are reset where they are declared; that always precedes their jumps in the
 * walk (jumps sit inside the LETJ, or in lambdas created inside it, walked later). */
static void reset_joins(ir_node *n) {
    ir_join *j;
    for (; n; ) {
        switch (n->kind) {
        case IR_LET: n = n->u.let.body; continue;
        case IR_FIX: n = n->u.fix.body; continue;
        case IR_LETJ:
            for (j = n->u.letj.j; j; j = j->next) { j->njumps = 0; reset_joins(j->body); }
            n = n->u.letj.body;
            continue;
        case IR_IF: reset_joins(n->u.if_.then); n = n->u.if_.els; continue;
        default: return;
        }
    }
}
static void census(void) {
    ir_lambda *L;
    int i;
    for (i = 0; i < P->nvars; i++) P->vars[i]->nrefs = P->vars[i]->nsets = P->vars[i]->ncalls = 0;
    for (L = P->lambdas; L; L = L->next) {
        L->mark = 0;
        if (L->entry) L->entry->njumps = 0;
        reset_joins(L->body);
    }
    nwork = 0;
    live(P->main, NULL);
    P->main->parent = NULL;
    for (L = P->lambdas; L; L = L->next) if (L->keep) live(L, NULL);
    for (i = 0; i < nwork; i++) {
        int k;
        L = work[i];
        if (L->nclauses) {
            for (k = 0; k < L->nclauses; k++) live(L->clauses[k], L);
            continue;
        }
        for (k = 0; k < L->nparams; k++) L->params[k]->owner = L;
        cen_term(L->body, L);
    }
}
#define LIVE(L) ((L)->mark && !(L)->dead)

/* ---- size and recursion ---- */
static int term_size(ir_node *n) {
    int s = 0, i;
    for (; n; ) {
        s++;
        switch (n->kind) {
        case IR_LET:
            if (n->u.let.rhs->kind == IR_LAMBDA) {
                ir_lambda *M = n->u.let.rhs->u.lam;
                if (M->nclauses) for (i = 0; i < M->nclauses; i++) s += term_size(M->clauses[i]->body);
                else s += term_size(M->body);
            } else if (n->u.let.rhs->kind == IR_CALL) s += 2 + n->u.let.rhs->u.call.n / 2;
            n = n->u.let.body;
            continue;
        case IR_FIX:
            for (i = 0; i < n->u.fix.n; i++) s += term_size(n->u.fix.lams[i]->body);
            n = n->u.fix.body;
            continue;
        case IR_LETJ: {
            ir_join *j;
            for (j = n->u.letj.j; j; j = j->next) s += term_size(j->body);
            n = n->u.letj.body;
            continue;
        }
        case IR_IF: s += term_size(n->u.if_.then); n = n->u.if_.els; continue;
        case IR_CALL: return s + 2 + n->u.call.n / 2;
        default: return s;
        }
    }
    return s;
}
static int lam_size(ir_lambda *L) {
    if (L->sround != round_no) { L->size = term_size(L->body); L->sround = round_no; }
    return L->size;
}

/* Tarjan's SCC over procedures; edges: known calls, references to known procedures, and the
 * procedures a body creates. sedge is shared with the tail-call graph of ir_finish. */
static int scc_n;
static ir_lambda **sstack;
static int nsstack, sstack_cap;
static int tail_graph;            /* 0: reference graph; 1: tail calls only */
static ir_lambda unknown_node;    /* tail graph: "any escaping procedure" */
static void scc_visit(ir_lambda *L);
static void sedge(ir_lambda *L, ir_lambda *M) {
    if (!M || M->dead) return;
    if (M == L) { L->rec = 1; return; }
    if (!M->sidx) {
        scc_visit(M);
        if (M->slow < L->slow) L->slow = M->slow;
    } else if (M->onstack && M->sidx < L->slow) L->slow = M->sidx;
}
static void sedge_atom(ir_lambda *L, ir_node *a) {
    if (a->kind == IR_REF && a->u.ref.v->known) sedge(L, a->u.ref.v->known);
}
static void scc_term(ir_lambda *L, ir_node *n) {
    int i;
    for (; n; ) {
        switch (n->kind) {
        case IR_CONST: case IR_REF: if (!tail_graph) sedge_atom(L, n); return;
        case IR_PRIM: if (!tail_graph) for (i = 0; i < n->u.prim.n; i++) sedge_atom(L, n->u.prim.args[i]); return;
        case IR_CALL:
            if (tail_graph) {
                if (!n->u.call.tail) return;
                if (n->u.call.ctl == CTL_CALLCC) return; /* rt_call_cc calls from C, not a tail call */
                sedge(L, n->u.call.known && n->u.call.ctl == CTL_NONE ? n->u.call.known : &unknown_node);
                return;
            }
            if (n->u.call.known) sedge(L, n->u.call.known);
            else if (n->u.call.fn) sedge_atom(L, n->u.call.fn);
            for (i = 0; i < n->u.call.n; i++) sedge_atom(L, n->u.call.args[i]);
            return;
        case IR_LAMBDA: if (!tail_graph) sedge(L, n->u.lam); return;
        case IR_SET: if (!tail_graph) sedge_atom(L, n->u.set.val); return;
        case IR_LET: scc_term(L, n->u.let.rhs); n = n->u.let.body; continue;
        case IR_FIX:
            if (!tail_graph) for (i = 0; i < n->u.fix.n; i++) sedge(L, n->u.fix.lams[i]);
            n = n->u.fix.body;
            continue;
        case IR_LETJ: {
            ir_join *j;
            for (j = n->u.letj.j; j; j = j->next) scc_term(L, j->body);
            n = n->u.letj.body;
            continue;
        }
        case IR_JUMP: if (!tail_graph) for (i = 0; i < n->u.jump.n; i++) sedge_atom(L, n->u.jump.args[i]); return;
        case IR_IF: scc_term(L, n->u.if_.then); n = n->u.if_.els; continue;
        case IR_RET: if (!tail_graph) sedge_atom(L, n->u.ret); return;
        }
    }
}
static void scc_visit(ir_lambda *L) {
    int i;
    L->sidx = L->slow = ++scc_n;
    if (nsstack == sstack_cap) {
        ir_lambda **n = NEWN(ir_lambda *, sstack_cap * 2 + 64);
        if (sstack) memcpy(n, sstack, sizeof(ir_lambda *) * (size_t)nsstack);
        sstack = n;
        sstack_cap = sstack_cap * 2 + 64;
    }
    sstack[nsstack++] = L;
    L->onstack = 1;
    if (L == &unknown_node) {
        ir_lambda *M;
        for (M = P->lambdas; M; M = M->next) if (LIVE(M) && M->escapes) sedge(L, M);
    } else if (L->nclauses) {
        for (i = 0; i < L->nclauses; i++) sedge(L, L->clauses[i]);
    } else scc_term(L, L->body);
    if (L->slow == L->sidx) {
        ir_lambda *M;
        int n = 0;
        do { M = sstack[--nsstack]; M->onstack = 0; n++; } while (M != L);
        if (n > 1) { /* every member of a cycle can reach itself */
            for (i = nsstack; i < nsstack + n; i++) sstack[i]->rec = 1;
        }
    }
}
static void scc_all(int tail) {
    ir_lambda *L;
    tail_graph = tail;
    scc_n = 0;
    nsstack = 0;
    memset(&unknown_node, 0, sizeof unknown_node);
    for (L = P->lambdas; L; L = L->next) L->sidx = L->slow = L->onstack = L->rec = 0;
    for (L = P->lambdas; L; L = L->next) if (LIVE(L) && !L->sidx) scc_visit(L);
    if (tail) scc_visit(&unknown_node);
}

/* ---- copying a procedure body with fresh variables (inlining) ---- */
typedef struct { pmap vars, joins, lams; ir_join *ret; } copyctx;
static ir_var *cp_bind(copyctx *c, ir_var *v, ir_lambda *in) {
    ir_var *n = ir_new_var(v->name, in);
    n->flags = v->flags;
    n->type = v->type;
    pm_put(&c->vars, v, n);
    return n;
}
static ir_var *cp_var(copyctx *c, ir_var *v) { ir_var *n = (ir_var *)pm_get(&c->vars, v); return n ? n : v; }
static ir_node *cp_atom(copyctx *c, ir_node *a) {
    ir_node *r;
    if (a->kind == IR_CONST) return mk_const(a->u.value);
    r = mk_ref(cp_var(c, a->u.ref.v));
    r->u.ref.check = a->u.ref.check;
    return r;
}
static ir_node **cp_atoms(copyctx *c, ir_node **a, int n) {
    ir_node **r = NEWN(ir_node *, n);
    int i;
    for (i = 0; i < n; i++) r[i] = cp_atom(c, a[i]);
    return r;
}
static ir_join *cp_join(copyctx *c, ir_join *j) { ir_join *n = (ir_join *)pm_get(&c->joins, j); return n ? n : j; }
static ir_lambda *cp_lambda(copyctx *c, ir_lambda *L, ir_lambda *parent);
static ir_lambda *cp_shell(copyctx *c, ir_lambda *L, ir_lambda *parent);
static void cp_body(copyctx *c, ir_lambda *L);
static ir_node *cp_term(copyctx *c, ir_node *n, int toret, ir_lambda *in);
static ir_node *cp_rhs(copyctx *c, ir_node *r, ir_lambda *in) {
    ir_node *m;
    ir_lambda *k;
    switch (r->kind) {
    case IR_CONST: case IR_REF: return cp_atom(c, r);
    case IR_PRIM:
        m = ir_new_node(IR_PRIM, r->loc);
        m->u.prim = r->u.prim;
        m->u.prim.args = cp_atoms(c, r->u.prim.args, r->u.prim.n);
        return m;
    case IR_CALL:
        m = ir_new_node(IR_CALL, r->loc);
        m->u.call = r->u.call;
        m->u.call.fn = r->u.call.fn ? cp_atom(c, r->u.call.fn) : NULL;
        m->u.call.args = cp_atoms(c, r->u.call.args, r->u.call.n);
        if (r->u.call.known && (k = (ir_lambda *)pm_get(&c->lams, r->u.call.known))) m->u.call.known = k;
        return m;
    case IR_LAMBDA:
        m = ir_new_node(IR_LAMBDA, r->loc);
        m->u.lam = cp_lambda(c, r->u.lam, in);
        return m;
    case IR_SET:
        m = ir_new_node(IR_SET, r->loc);
        m->u.set.v = cp_var(c, r->u.set.v);
        m->u.set.val = cp_atom(c, r->u.set.val);
        return m;
    default: cerror(RT_FALSE, "internal error: copy of rhs kind %d", r->kind);
    }
}
static ir_join *cp_join_decl(copyctx *c, ir_join *j, ir_lambda *in) {
    ir_join *n = ir_new_join(j->n, in);
    int i;
    for (i = 0; i < j->n; i++) n->params[i] = cp_bind(c, j->params[i], in);
    n->site = j->site;
    pm_put(&c->joins, j, n);
    return n;
}
/* A copy is made in two steps, so that procedures of one FIX can call each other: the
 * shells (mapping, parameters, entry, clauses) first, then the bodies. */
static ir_lambda *cp_shell(copyctx *c, ir_lambda *L, ir_lambda *parent) {
    ir_lambda *M = ir_new_lambda(L->name, parent, L->lib);
    int i;
    pm_put(&c->lams, L, M);
    M->nreq = L->nreq;
    M->rest = L->rest;
    M->nparams = L->nparams;
    M->params = NEWN(ir_var *, L->nparams);
    for (i = 0; i < L->nparams; i++) M->params[i] = cp_bind(c, L->params[i], M);
    M->self = L->self ? cp_var(c, L->self) : NULL;
    if (M->self && M->self != L->self) M->self->known = M;
    M->mark = 1; /* live (only meaningful until the next census) */
    M->rec = L->rec;
    if (L->entry) {
        M->entry = ir_new_join(M->nparams, M);
        M->entry->params = M->params;
        pm_put(&c->joins, L->entry, M->entry);
    }
    if (L->nclauses) {
        M->nclauses = L->nclauses;
        M->clauses = NEWN(ir_lambda *, L->nclauses);
        for (i = 0; i < L->nclauses; i++) {
            M->clauses[i] = cp_shell(c, L->clauses[i], M);
            M->clauses[i]->dispatch = M;
        }
    }
    return M;
}
static void cp_body(copyctx *c, ir_lambda *L) {
    ir_lambda *M = (ir_lambda *)pm_get(&c->lams, L);
    int i;
    if (L->nclauses) for (i = 0; i < L->nclauses; i++) cp_body(c, L->clauses[i]);
    else M->body = cp_term(c, L->body, 0, M);
}
static ir_lambda *cp_lambda(copyctx *c, ir_lambda *L, ir_lambda *parent) {
    ir_lambda *M = cp_shell(c, L, parent);
    cp_body(c, L);
    return M;
}
/* toret: RET a becomes a jump to c->ret, a tail call a call followed by that jump. */
static ir_node *cp_term(copyctx *c, ir_node *n, int toret, ir_lambda *in) {
    ir_node *m;
    int i;
    switch (n->kind) {
    case IR_LET: {
        ir_var *v = n->u.let.v ? cp_bind(c, n->u.let.v, in) : NULL;
        ir_node *r = cp_rhs(c, n->u.let.rhs, in);
        if (v && r->kind == IR_LAMBDA && n->u.let.v->known == n->u.let.rhs->u.lam) v->known = r->u.lam;
        return mk_let(v, r, cp_term(c, n->u.let.body, toret, in), n->loc);
    }
    case IR_FIX:
        m = ir_new_node(IR_FIX, n->loc);
        m->u.fix.n = n->u.fix.n;
        m->u.fix.vars = NEWN(ir_var *, n->u.fix.n);
        m->u.fix.lams = NEWN(ir_lambda *, n->u.fix.n);
        for (i = 0; i < n->u.fix.n; i++) m->u.fix.vars[i] = cp_bind(c, n->u.fix.vars[i], in);
        for (i = 0; i < n->u.fix.n; i++) {
            m->u.fix.lams[i] = cp_shell(c, n->u.fix.lams[i], in);
            m->u.fix.vars[i]->known = m->u.fix.lams[i];
            m->u.fix.lams[i]->self = m->u.fix.vars[i];
        }
        for (i = 0; i < n->u.fix.n; i++) cp_body(c, n->u.fix.lams[i]);
        m->u.fix.body = cp_term(c, n->u.fix.body, toret, in);
        return m;
    case IR_LETJ: {
        ir_join *j, *first = NULL, **pp = &first;
        for (j = n->u.letj.j; j; j = j->next) { *pp = cp_join_decl(c, j, in); pp = &(*pp)->next; }
        for (j = n->u.letj.j; j; j = j->next) cp_join(c, j)->body = cp_term(c, j->body, toret, in);
        return mk_letj(first, cp_term(c, n->u.letj.body, toret, in), n->loc);
    }
    case IR_JUMP: return mk_jump(cp_join(c, n->u.jump.j), n->u.jump.n, cp_atoms(c, n->u.jump.args, n->u.jump.n), n->loc);
    case IR_IF:
        m = ir_new_node(IR_IF, n->loc);
        m->u.if_.test = cp_atom(c, n->u.if_.test);
        m->u.if_.then = cp_term(c, n->u.if_.then, toret, in);
        m->u.if_.els = cp_term(c, n->u.if_.els, toret, in);
        return m;
    case IR_RET: return toret ? mk_jump1(c->ret, cp_atom(c, n->u.ret), 0) : mk_ret(cp_atom(c, n->u.ret));
    case IR_CALL:
        m = cp_rhs(c, n, in);
        if (toret) {
            ir_var *t = ir_new_var(sym("t"), in);
            m->u.call.tail = 0;
            return mk_let(t, m, mk_jump1(c->ret, mk_ref(t), n->loc), n->loc);
        }
        return m;
    default: cerror(RT_FALSE, "internal error: copy of term kind %d", n->kind);
    }
}

/* ---- simplification ---- */
static ir_lambda *cur;
/* The atom a reference reads: copy propagation through ->subst (never for a checked read). */
static ir_node *sub(ir_node *a) {
    ir_node *r = a;
    while (r->kind == IR_REF && !r->u.ref.check && r->u.ref.v->subst) r = r->u.ref.v->subst;
    if (r == a) return a;
    changed = 1;
    return r->kind == IR_CONST ? mk_const(r->u.value) : mk_ref(r->u.ref.v);
}
static void sub_rhs(ir_node *r) {
    int i;
    switch (r->kind) {
    case IR_PRIM: for (i = 0; i < r->u.prim.n; i++) r->u.prim.args[i] = sub(r->u.prim.args[i]); return;
    case IR_CALL:
        if (r->u.call.fn) r->u.call.fn = sub(r->u.call.fn);
        for (i = 0; i < r->u.call.n; i++) r->u.call.args[i] = sub(r->u.call.args[i]);
        if (!r->u.call.known && r->u.call.ctl == CTL_NONE && r->u.call.fn->kind == IR_REF) {
            ir_var *f = r->u.call.fn->u.ref.v;
            /* a call that became known; a wrong argument count stays a run-time error */
            if (f->known && !(f->flags & V_ASSIGNED) && !r->u.call.fn->u.ref.check && (r->u.call.known = resolve(f->known, r->u.call.n)))
                changed = 1;
        }
        return;
    case IR_SET: r->u.set.val = sub(r->u.set.val); return;
    default: return;
    }
}
static int fixc(ir_node *a, intptr_t *v) {
    if (a->kind != IR_CONST || !rt_is_fix(a->u.value)) return 0;
    *v = (intptr_t)a->u.value;
    return 1;
}
/* Folding: pure primitives of constants that cannot raise (an application that would raise
 * keeps its run-time behaviour: overflow, wrong types). */
static ir_node *fold(ir_node *p) {
    ir_node **a = p->u.prim.args;
    obj x, y = 0;
    intptr_t i, j, r;
    int k;
    for (k = 0; k < p->u.prim.n; k++) if (a[k]->kind != IR_CONST) return NULL;
    if (p->u.prim.n < 1 || p->u.prim.n > 2) return NULL;
    x = a[0]->u.value;
    if (p->u.prim.n == 2) y = a[1]->u.value;
    switch (p->u.prim.op) {
    case PI_rt_add: return fixc(a[0], &i) && fixc(a[1], &j) && !rt_add_ovf(i, j, &r) ? mk_const(RT_O(r)) : NULL;
    case PI_rt_sub: return fixc(a[0], &i) && fixc(a[1], &j) && !rt_sub_ovf(i, j, &r) ? mk_const(RT_O(r)) : NULL;
    case PI_rt_mul: return fixc(a[0], &i) && fixc(a[1], &j) && !rt_mul_ovf(RT_FIXVAL(x), j, &r) ? mk_const(RT_O(r)) : NULL;
    case PI_rt_lt: return fixc(a[0], &i) && fixc(a[1], &j) ? mk_const(RT_BOOL(i < j)) : NULL;
    case PI_rt_gt: return fixc(a[0], &i) && fixc(a[1], &j) ? mk_const(RT_BOOL(i > j)) : NULL;
    case PI_rt_num_le: return fixc(a[0], &i) && fixc(a[1], &j) ? mk_const(RT_BOOL(i <= j)) : NULL;
    case PI_rt_num_ge: return fixc(a[0], &i) && fixc(a[1], &j) ? mk_const(RT_BOOL(i >= j)) : NULL;
    case PI_rt_num_eq: return fixc(a[0], &i) && fixc(a[1], &j) ? mk_const(RT_BOOL(i == j)) : NULL;
    case PI_rt_eq_p: return mk_const(RT_BOOL(x == y));
    case PI_rt_eqv_p: return rt_is_flo(x) || rt_is_flo(y) ? NULL : mk_const(RT_BOOL(x == y));
    case PI_rt_car: return rt_is_pair(x) ? mk_const(RT_CAR(x)) : NULL;
    case PI_rt_cdr: return rt_is_pair(x) ? mk_const(RT_CDR(x)) : NULL;
    case PI_rt_char_to_integer: return rt_is_char(x) ? mk_const(RT_FIX(RT_CHARVAL(x))) : NULL;
    case PI_rt_null_p: return mk_const(rt_null_p(x));
    case PI_rt_pair_p: return mk_const(rt_pair_p(x));
    case PI_rt_boolean_p: return mk_const(rt_boolean_p(x));
    case PI_rt_symbol_p: return mk_const(rt_symbol_p(x));
    case PI_rt_char_p: return mk_const(rt_char_p(x));
    case PI_rt_string_p: return mk_const(rt_string_p(x));
    case PI_rt_vector_p: return mk_const(rt_vector_p(x));
    case PI_rt_procedure_p: return mk_const(rt_procedure_p(x));
    case PI_rt_number_p: return mk_const(rt_number_p(x));
    case PI_rt_exact_integer_p: return mk_const(rt_exact_integer_p(x));
    case PI_rt_eof_object_p: return mk_const(rt_eof_object_p(x));
    default: return NULL;
    }
}
static int removable(ir_node *r) {
    switch (r->kind) {
    case IR_CONST: case IR_LAMBDA: return 1;
    case IR_REF: return !r->u.ref.check;
    case IR_PRIM: return !(prim_table[r->u.prim.op].flags & (PF_FAIL | PF_EFFECT));
    case IR_SET: /* a store to a global that nobody reads */
        return (r->u.set.v->flags & V_GLOBAL) && r->u.set.v->nrefs == 0 && is_atom(r->u.set.val) && removable(r->u.set.val);
    default: return 0;
    }
}
/* LET v = atom: every reference to v can read the atom instead. */
static int propagatable(ir_var *v, ir_node *a) {
    ir_var *y;
    if (v->flags & (V_ASSIGNED | V_BOXED | V_GLOBAL)) return 0;
    if (a->kind == IR_CONST) return 1;
    if (a->u.ref.check) return 0;
    y = a->u.ref.v;
    if (y->flags & V_GLOBAL) return !(y->flags & V_ASSIGNED) && y->g && y->g->defined <= 1;
    return !(y->flags & V_ASSIGNED);
}
/* Top-level definitions never set! whose value is a constant or a known procedure. */
static void global_consts(ir_node *n) {
    for (; n; ) {
        switch (n->kind) {
        case IR_LET: {
            ir_node *r = n->u.let.rhs;
            if (r->kind == IR_SET) {
                ir_var *g = r->u.set.v;
                ir_node *a = r->u.set.val;
                if ((g->flags & V_GLOBAL) && !(g->flags & V_ASSIGNED) && g->g && g->g->defined == 1 && !g->known && !g->subst &&
                    (a->kind == IR_CONST || (!a->u.ref.check && (a->u.ref.v->flags & V_GLOBAL) &&
                                              (a->u.ref.v->known || a->u.ref.v->subst)))) {
                    g->subst = a->kind == IR_CONST ? mk_const(a->u.value) : mk_ref(a->u.ref.v);
                    changed = 1;
                }
            }
            n = n->u.let.body;
            continue;
        }
        case IR_FIX: n = n->u.fix.body; continue;
        case IR_LETJ: {
            ir_join *j;
            for (j = n->u.letj.j; j; j = j->next) global_consts(j->body);
            n = n->u.letj.body;
            continue;
        }
        case IR_IF: global_consts(n->u.if_.then); n = n->u.if_.els; continue;
        default: return;
        }
    }
}

/* Inline a known call: the callee's body with fresh variables, entered through a join point
 * (its loop, if it has one); in value position its returns jump to a join point holding the
 * rest of the caller. Never a procedure that can reach itself; small ones, or the only call. */
static void simp_term(ir_node **slot);
static int try_inline(ir_node **slot, ir_node *let, ir_node *c) {
    ir_lambda *L = c->u.call.known;
    ir_var *self;
    copyctx cx;
    ir_join *e, *k = NULL;
    ir_node **jargs, *pre = NULL, **pp = &pre, *t;
    int i, sz, once, n = c->u.call.n;
    if (!L || c->u.call.ctl != CTL_NONE || L->dead || L->nclauses || L->rec || L == cur || !L->body) return 0;
    if (L->dispatch && L->dispatch->rec) return 0;
    sz = lam_size(L);
    self = real(L)->self;
    once = !L->dispatch && !L->keep && self && self->nrefs == 1 && self->ncalls == 1;
    if (once ? sz > lim_once : sz > lim_small || growth < sz) return 0;
    if (!once) growth -= sz;
    if (getenv("SHIKRA_TRACE")) fprintf(stderr, "inline %s (size %d%s) into %s\n", idname(L->name), sz, once ? ", once" : "", idname(cur->name));
    memset(&cx, 0, sizeof cx);
    e = ir_new_join(L->nparams, cur);
    for (i = 0; i < L->nparams; i++) e->params[i] = cp_bind(&cx, L->params[i], cur);
    if (L->entry) pm_put(&cx.joins, L->entry, e);
    if (let) {
        k = ir_new_join(1, cur);
        k->params[0] = let->u.let.v ? let->u.let.v : ir_new_var(sym("_"), cur);
        k->body = let->u.let.body;
        let->u.let.body = NULL;
        cx.ret = k;
    }
    e->body = cp_term(&cx, L->body, k != NULL, cur);
    jargs = NEWN(ir_node *, L->nparams);
    for (i = 0; i < L->nreq; i++) jargs[i] = c->u.call.args[i];
    if (L->rest) { /* the rest list, consed in front of the jump */
        ir_node *l = mk_const(RT_NIL);
        for (i = n - 1; i >= L->nreq; i--) {
            ir_var *r = ir_new_var(sym("rest"), cur);
            ir_node *p = ir_new_node(IR_PRIM, c->loc);
            p->u.prim.op = PI_rt_cons;
            p->u.prim.n = 2;
            p->u.prim.args = NEWN(ir_node *, 2);
            p->u.prim.args[0] = c->u.call.args[i];
            p->u.prim.args[1] = l;
            *pp = mk_let(r, p, NULL, c->loc);
            pp = &(*pp)->u.let.body;
            l = mk_ref(r);
        }
        jargs[L->nreq] = l;
    }
    *pp = mk_jump(e, L->nparams, jargs, c->loc);
    t = mk_letj(e, pre, c->loc);
    *slot = k ? mk_letj(k, t, c->loc) : t;
    changed = 1;
    return 1;
}

static void simp_lambda(ir_lambda *L) {
    ir_lambda *saved = cur;
    int i;
    cur = L;
    if (L->nclauses) for (i = 0; i < L->nclauses; i++) simp_lambda(L->clauses[i]);
    else simp_term(&L->body);
    cur = saved;
}
static void simp_term(ir_node **slot) {
    int i;
    for (;;) {
        ir_node *n = *slot;
        switch (n->kind) {
        case IR_LET: {
            ir_var *v = n->u.let.v;
            ir_node *r = n->u.let.rhs;
            if (is_atom(r)) r = n->u.let.rhs = sub(r);
            else sub_rhs(r);
            if (r->kind == IR_PRIM) {
                ir_node *f = fold(r);
                if (f) { n->u.let.rhs = r = f; changed = 1; }
            }
            if (v && is_atom(r) && propagatable(v, r)) {
                v->subst = r;
                *slot = n->u.let.body;
                changed = 1;
                continue;
            }
            if ((!v || v->nrefs == 0) && removable(r)) {
                *slot = n->u.let.body;
                changed = 1;
                continue;
            }
            if (r->kind == IR_CALL && try_inline(slot, n, r)) continue;
            if (r->kind == IR_LAMBDA) simp_lambda(r->u.lam);
            slot = &n->u.let.body;
            continue;
        }
        case IR_FIX: {
            int k = 0;
            for (i = 0; i < n->u.fix.n; i++)
                if (n->u.fix.vars[i]->nrefs > 0) {
                    n->u.fix.vars[k] = n->u.fix.vars[i];
                    n->u.fix.lams[k++] = n->u.fix.lams[i];
                } else changed = 1;
            n->u.fix.n = k;
            if (!k) { *slot = n->u.fix.body; continue; }
            for (i = 0; i < k; i++) simp_lambda(n->u.fix.lams[i]);
            slot = &n->u.fix.body;
            continue;
        }
        case IR_LETJ: {
            ir_join *j, **pj;
            for (j = n->u.letj.j; j; j = j->next) j->state = j->njumps == 1 && j->owner == cur && !j->site; /* sites: verified later */
            simp_term(&n->u.letj.body);
            for (pj = &n->u.letj.j; (j = *pj); ) {
                if (j->state == 2 || j->njumps == 0) { *pj = j->next; changed = 1; continue; }
                j->state = 0;
                pj = &j->next;
            }
            if (!n->u.letj.j) { *slot = n->u.letj.body; return; }
            for (j = n->u.letj.j; j; j = j->next) simp_term(&j->body);
            return;
        }
        case IR_JUMP: {
            ir_join *j = n->u.jump.j;
            for (i = 0; i < n->u.jump.n; i++) n->u.jump.args[i] = sub(n->u.jump.args[i]);
            if (j->state == 1 && j->owner == cur) { /* the only jump: the join's body moves here */
                ir_node **s = slot;
                j->state = 2;
                for (i = 0; i < j->n; i++) {
                    *s = mk_let(j->params[i], n->u.jump.args[i], NULL, n->loc);
                    s = &(*s)->u.let.body;
                }
                *s = j->body;
                j->body = NULL;
                changed = 1;
                continue;
            }
            return;
        }
        case IR_IF: {
            ir_node *t = n->u.if_.test = sub(n->u.if_.test);
            if (t->kind == IR_CONST) {
                *slot = t->u.value != RT_FALSE ? n->u.if_.then : n->u.if_.els;
                changed = 1;
                continue;
            }
            simp_term(&n->u.if_.then);
            simp_term(&n->u.if_.els);
            return;
        }
        case IR_RET: n->u.ret = sub(n->u.ret); return;
        case IR_CALL:
            sub_rhs(n);
            if (try_inline(slot, NULL, n)) continue;
            return;
        default: return;
        }
    }
}

/* ---- contification ----
 * A group of procedures (the candidates of one FIX, a let-bound procedure, or a top-level
 * procedure) whose every reference is a known call, called inside the group only in tail
 * position and from outside only from one procedure Q, either always in tail position or
 * at a single call site, becomes join points of Q: their bodies run in Q's C function. */
typedef struct csite { ir_node **slot, *node, *call; ir_lambda *in; struct csite *next; } csite;
static pmap sites;   /* ir_lambda* -> csite list */
static pmap binds;   /* ir_lambda* -> csite (slot and node of its FIX or LET binding, in = owner) */
static void add_site(pmap *m, ir_lambda *L, ir_node **slot, ir_node *node, ir_node *call, ir_lambda *in) {
    csite *s = NEW(csite);
    s->slot = slot;
    s->node = node;
    s->call = call;
    s->in = in;
    s->next = (csite *)pm_get(m, L);
    pm_put(m, L, s);
}
/* Every live procedure's body is walked once (nested procedures are walked on their own). */
static void site_walk(ir_node **slot, ir_lambda *in) {
    int i;
    for (;;) {
        ir_node *n = *slot;
        switch (n->kind) {
        case IR_LET: {
            ir_node *r = n->u.let.rhs;
            if (r->kind == IR_CALL && r->u.call.known && r->u.call.ctl == CTL_NONE) add_site(&sites, r->u.call.known, slot, n, r, in);
            if (r->kind == IR_LAMBDA) add_site(&binds, r->u.lam, slot, n, NULL, in);
            slot = &n->u.let.body;
            continue;
        }
        case IR_FIX:
            for (i = 0; i < n->u.fix.n; i++) add_site(&binds, n->u.fix.lams[i], slot, n, NULL, in);
            slot = &n->u.fix.body;
            continue;
        case IR_LETJ: {
            ir_join *j;
            for (j = n->u.letj.j; j; j = j->next) site_walk(&j->body, in);
            slot = &n->u.letj.body;
            continue;
        }
        case IR_IF: site_walk(&n->u.if_.then, in); slot = &n->u.if_.els; continue;
        case IR_CALL:
            if (n->u.call.known && n->u.call.ctl == CTL_NONE) add_site(&sites, n->u.call.known, slot, n, n, in);
            return;
        default: return;
        }
    }
}
static void collect_sites(void) {
    ir_lambda *L;
    memset(&sites, 0, sizeof sites);
    memset(&binds, 0, sizeof binds);
    for (L = P->lambdas; L; L = L->next) if (LIVE(L) && !L->nclauses && L->body) site_walk(&L->body, L);
}
static int nsites(ir_lambda *L) {
    csite *s;
    int n = 0;
    for (s = (csite *)pm_get(&sites, L); s; s = s->next) n++;
    return n;
}
static int site_valid(csite *s) { return *s->slot == s->node; }
static int candidate(ir_lambda *L) {
    ir_var *v = L->self;
    csite *s;
    if (!LIVE(L) || L->nclauses || L->dispatch || L->rest || L->keep || L == P->main || !v || v->known != L) return 0;
    if (v->nrefs != v->ncalls || !(s = (csite *)pm_get(&sites, L)) || nsites(L) != v->ncalls) return 0;
    for (; s; s = s->next) if (s->call->u.call.n != L->nreq || !site_valid(s)) return 0;
    return 1;
}
static int in_group(ir_lambda **g, int n, ir_lambda *L) {
    int i;
    for (i = 0; i < n; i++) if (g[i] == L) return 1;
    return 0;
}
/* Returns and tail calls of a contified body continue at join k (single non-tail site). */
static void retarget(ir_node **slot, ir_join *k, ir_lambda *in) {
    for (;;) {
        ir_node *n = *slot;
        switch (n->kind) {
        case IR_LET: slot = &n->u.let.body; continue;
        case IR_FIX: slot = &n->u.fix.body; continue;
        case IR_LETJ: {
            ir_join *j;
            for (j = n->u.letj.j; j; j = j->next) retarget(&j->body, k, in);
            slot = &n->u.letj.body;
            continue;
        }
        case IR_IF: retarget(&n->u.if_.then, k, in); slot = &n->u.if_.els; continue;
        case IR_RET: *slot = mk_jump1(k, n->u.ret, n->loc); return;
        case IR_CALL: {
            ir_var *t = ir_new_var(sym("t"), in);
            n->u.call.tail = 0;
            *slot = mk_let(t, n, mk_jump1(k, mk_ref(t), n->loc), n->loc);
            return;
        }
        default: return;
        }
    }
}
static ir_node *call_jump(ir_lambda *L, ir_node *call) {
    return mk_jump(L->entry, call->u.call.n, call->u.call.args, call->loc);
}
static int inside(ir_lambda *Q, ir_lambda **g, int ng) {
    for (; Q; Q = Q->parent) if (in_group(g, ng, Q)) return 1;
    return 0;
}
static int contify_group(ir_lambda **g, int ng, csite *bind) {
    ir_lambda *Q = NULL;
    csite *s, *single = NULL;
    int i, next = 0, alltail = 1;
    ir_join *chain = NULL, **pp = &chain;
    ir_node **place = NULL;
    for (i = 0; i < ng; i++)
        for (s = (csite *)pm_get(&sites, g[i]); s; s = s->next) {
            if (s->in->dead) return 0; /* moved by an earlier contification: next round */
            if (in_group(g, ng, s->in)) {
                if (!s->call->u.call.tail) return 0;
                continue;
            }
            if (Q && s->in != Q) return 0;
            Q = s->in;
            next++;
            single = s;
            if (!s->call->u.call.tail) alltail = 0;
        }
    if (!Q || (!alltail && next != 1) || inside(Q, g, ng)) return 0;
    if (bind && (!site_valid(bind) || bind->in->dead)) return 0;
    { static int cnt; if (getenv("SHIKRA_CMAX") && ++cnt > atoi(getenv("SHIKRA_CMAX"))) return 0; }
    if (getenv("SHIKRA_TRACE")) fprintf(stderr, "contify %s (+%d) into %s (%d sites%s)\n", idname(g[0]->name), ng - 1, idname(Q->name), next, alltail ? ", tail" : "");
    for (i = 0; i < ng; i++) {
        ir_lambda *L = g[i];
        if (!L->entry) {
            L->entry = ir_new_join(L->nparams, Q);
            L->entry->params = L->params;
        }
        L->entry->owner = Q;
        L->entry->next = NULL;
        *pp = L->entry;
        pp = &L->entry->next;
    }
    /* 1. calls inside the group become jumps (a site may be a body itself: bodies move after) */
    for (i = 0; i < ng; i++)
        for (s = (csite *)pm_get(&sites, g[i]); s; s = s->next)
            if (in_group(g, ng, s->in)) *s->slot = call_jump(g[i], s->call);
    for (i = 0; i < ng; i++) g[i]->entry->body = g[i]->body;
    /* 2. the outside calls */
    if (next == 1) { /* one call site: the join points go there */
        ir_lambda *L = single->call->u.call.known;
        ir_node *n = single->node;
        if (n->kind == IR_CALL) *single->slot = mk_letj(chain, call_jump(L, n), n->loc);
        else {
            ir_join *k = ir_new_join(1, Q);
            k->params[0] = n->u.let.v ? n->u.let.v : ir_new_var(sym("_"), Q);
            k->body = n->u.let.body;
            n->u.let.body = NULL;
            for (i = 0; i < ng; i++) retarget(&g[i]->entry->body, k, Q);
            *single->slot = mk_letj(k, mk_letj(chain, call_jump(L, single->call), n->loc), n->loc);
        }
    } else
        for (i = 0; i < ng; i++)
            for (s = (csite *)pm_get(&sites, g[i]); s; s = s->next)
                if (!in_group(g, ng, s->in)) *s->slot = call_jump(g[i], s->call);
    /* 3. the binding goes away */
    if (bind) {
        ir_node *b = bind->node;
        if (b->kind == IR_LET) {
            *bind->slot = b->u.let.body;
            b->u.let.body = NULL;
            place = bind->slot;
        } else {
            int k = 0;
            for (i = 0; i < b->u.fix.n; i++)
                if (!in_group(g, ng, b->u.fix.lams[i])) {
                    b->u.fix.vars[k] = b->u.fix.vars[i];
                    b->u.fix.lams[k++] = b->u.fix.lams[i];
                }
            b->u.fix.n = k;
            if (k) place = &b->u.fix.body;
            else {
                *bind->slot = b->u.fix.body;
                b->u.fix.body = NULL;
                place = bind->slot;
            }
        }
    }
    /* 4. several tail call sites in Q: the join points where the procedures were bound (if
     * that is in Q), else at Q's entry */
    if (next > 1) {
        if (!bind || bind->in != Q) place = &Q->body;
        *place = mk_letj(chain, *place, 0);
    }
    for (i = 0; i < ng; i++) {
        g[i]->dead = 1;
        g[i]->body = NULL;
    }
    changed = 1;
    return 1;
}
static void contify(void) {
    ir_lambda *L;
    pmap done_fix = {0};
    collect_sites();
    for (L = P->lambdas; L; L = L->next) {
        csite *b;
        ir_lambda *g[64];
        int ng = 0, i;
        if (L->dead || !candidate(L)) continue;
        b = (csite *)pm_get(&binds, L);
        if (b && b->node->kind == IR_FIX) {
            ir_node *f = b->node;
            if (pm_get(&done_fix, f)) continue; /* each FIX is tried as a group once */
            pm_put(&done_fix, f, (void *)1);
            for (i = 0; i < f->u.fix.n && ng < 64; i++)
                if (candidate(f->u.fix.lams[i])) g[ng++] = f->u.fix.lams[i];
        } else if (!b && !L->g) continue; /* a lambda without a binding we know */
        else g[ng++] = L;
        if (!contify_group(g, ng, b) && ng > 1) /* the group as a whole failed: try members alone */
            for (i = 0; i < ng; i++) if (!g[i]->dead && candidate(g[i]) && site_valid(b)) contify_group(&g[i], 1, b);
    }
}

/* ---- boxes no longer captured ---- */
static void box_uses(ir_node *n, ir_lambda *L, int count) {
    int i;
    for (; n; ) {
        switch (n->kind) {
        case IR_LET: {
            ir_node *r = n->u.let.rhs;
            if (r->kind == IR_PRIM && (r->u.prim.op == PI_rt_box_ref || r->u.prim.op == PI_rt_box_set) && r->u.prim.args[0]->kind == IR_REF) {
                ir_var *v = r->u.prim.args[0]->u.ref.v;
                if (count) { if (v->owner == L) v->ncalls++; }
                else if (v->flags & V_BOXED && v->mark) {
                    if (r->u.prim.op == PI_rt_box_ref) n->u.let.rhs = r->u.prim.args[0];
                    else {
                        ir_node *s = ir_new_node(IR_SET, r->loc);
                        s->u.set.v = v;
                        s->u.set.val = r->u.prim.args[1];
                        n->u.let.rhs = s;
                    }
                }
            } else if (!count && r->kind == IR_PRIM && r->u.prim.op == PI_rt_make_box && n->u.let.v && (n->u.let.v->flags & V_BOXED) && n->u.let.v->mark)
                n->u.let.rhs = r->u.prim.args[0];
            n = n->u.let.body;
            continue;
        }
        case IR_FIX: n = n->u.fix.body; continue;
        case IR_LETJ: {
            ir_join *j;
            for (j = n->u.letj.j; j; j = j->next) box_uses(j->body, L, count);
            n = n->u.letj.body;
            continue;
        }
        case IR_IF: box_uses(n->u.if_.then, L, count); n = n->u.if_.els; continue;
        default: (void)i; return;
        }
    }
}
static void unbox(void) {
    ir_lambda *L;
    int i, any = 0;
    for (i = 0; i < P->nvars; i++) P->vars[i]->ncalls = 0, P->vars[i]->mark = 0;
    for (L = P->lambdas; L; L = L->next) if (LIVE(L) && !L->nclauses) box_uses(L->body, L, 1);
    for (i = 0; i < P->nvars; i++) {
        ir_var *v = P->vars[i];
        if ((v->flags & V_BOXED) && v->nrefs > 0 && v->ncalls == v->nrefs) { v->mark = 1; any = 1; }
    }
    if (!any) return;
    for (L = P->lambdas; L; L = L->next) if (LIVE(L) && !L->nclauses) box_uses(L->body, L, 0);
    for (i = 0; i < P->nvars; i++) {
        ir_var *v = P->vars[i];
        if (v->mark) {
            v->flags = (v->flags & ~(unsigned)(V_BOXED | V_CAPTURED)) | V_ASSIGNED;
            v->mark = 0;
            changed = 1;
        }
    }
}


/* ---- top-level mutual tail recursion (spec §8.1) ----
 * Top-level procedures that only reach each other by known calls (they never escape) and
 * form a cycle of tail calls become one procedure whose body is their join points: it takes
 * a selector and the largest parameter list, its entry dispatches on the selector, and tail
 * calls inside the group are jumps. Every outside call passes the member's selector. */
static int mcand(ir_lambda *L) {
    ir_var *v = L->self;
    csite *s;
    if (!LIVE(L) || !L->g || L->parent || L->nclauses || L->dispatch || L->rest || L->keep || !v || v->known != L) return 0;
    if (v->nrefs != v->ncalls || nsites(L) != v->ncalls) return 0;
    for (s = (csite *)pm_get(&sites, L); s; s = s->next) if (s->call->u.call.n != L->nreq || !site_valid(s) || s->in->dead) return 0;
    return 1;
}
static ir_lambda **mstack;
static int nmstack, mcount;
static void mvisit(ir_lambda *L);
static void medge(ir_lambda *L, ir_node *n) {
    for (; n; ) {
        switch (n->kind) {
        case IR_LET: n = n->u.let.body; continue;
        case IR_FIX: n = n->u.fix.body; continue;
        case IR_LETJ: {
            ir_join *j;
            for (j = n->u.letj.j; j; j = j->next) medge(L, j->body);
            n = n->u.letj.body;
            continue;
        }
        case IR_IF: medge(L, n->u.if_.then); n = n->u.if_.els; continue;
        case IR_CALL: {
            ir_lambda *M = n->u.call.known;
            if (!n->u.call.tail || n->u.call.ctl != CTL_NONE || !M || !M->onstack2) return;
            if (!M->sidx) { mvisit(M); if (M->slow < L->slow) L->slow = M->slow; }
            else if (M->onstack && M->sidx < L->slow) L->slow = M->sidx;
            return;
        }
        default: return;
        }
    }
}
static void merge_group(ir_lambda **g, int ng) {
    ir_lambda *M;
    ir_join *chain = NULL, **pp = &chain;
    ir_node *disp, **dp = &disp;
    csite *s;
    int i, k = 0;
    char name[256];
    for (i = 0; i < ng; i++) if (g[i]->nparams > k) k = g[i]->nparams;
    snprintf(name, sizeof name, "%s-group", idname(g[0]->name));
    M = ir_new_lambda(sym(name), NULL, g[0]->lib);
    M->nreq = M->nparams = k + 1;
    M->params = NEWN(ir_var *, k + 1);
    for (i = 0; i <= k; i++) M->params[i] = ir_new_var(sym(i ? "a" : "which"), M);
    M->self = ir_new_var(M->name, NULL);
    M->self->known = M;
    for (i = 0; i < ng; i++) {
        ir_lambda *L = g[i];
        if (!L->entry) { L->entry = ir_new_join(L->nparams, M); L->entry->params = L->params; }
        L->entry->owner = M;
        L->entry->next = NULL;
        *pp = L->entry;
        pp = &L->entry->next;
    }
    for (i = 0; i < ng; i++) /* calls of members: tail calls inside the group jump, others call M */
        for (s = (csite *)pm_get(&sites, g[i]); s; s = s->next) {
            ir_node *c = s->call, **args;
            int j;
            if (c->u.call.tail && in_group(g, ng, s->in)) { *s->slot = call_jump(g[i], c); continue; }
            args = NEWN(ir_node *, k + 1);
            args[0] = mk_const(RT_FIX(i));
            for (j = 0; j < k; j++) args[j + 1] = j < c->u.call.n ? c->u.call.args[j] : mk_const(RT_FALSE);
            c->u.call.args = args;
            c->u.call.n = k + 1;
            c->u.call.known = M;
            c->u.call.fn = mk_ref(M->self);
        }
    for (i = 0; i < ng; i++) g[i]->entry->body = g[i]->body;
    for (i = 0; i < ng; i++) { /* the entry dispatches on the selector */
        ir_node **a = NEWN(ir_node *, g[i]->nparams), *jmp;
        int j;
        for (j = 0; j < g[i]->nparams; j++) a[j] = mk_ref(M->params[j + 1]);
        jmp = mk_jump(g[i]->entry, g[i]->nparams, a, 0);
        if (i < ng - 1) {
            ir_var *t = ir_new_var(sym("t"), M);
            ir_node *p = ir_new_node(IR_PRIM, 0), *f = ir_new_node(IR_IF, 0);
            p->u.prim.op = PI_rt_eq_p;
            p->u.prim.n = 2;
            p->u.prim.args = NEWN(ir_node *, 2);
            p->u.prim.args[0] = mk_ref(M->params[0]);
            p->u.prim.args[1] = mk_const(RT_FIX(i));
            f->u.if_.test = mk_ref(t);
            f->u.if_.then = jmp;
            *dp = mk_let(t, p, f, 0);
            dp = &f->u.if_.els;
        } else *dp = jmp;
    }
    M->body = mk_letj(chain, disp, 0);
    for (i = 0; i < ng; i++) { g[i]->dead = 1; g[i]->body = NULL; }
    if (getenv("SHIKRA_TRACE")) fprintf(stderr, "merge %s (%d procedures)\n", name, ng);
}
static void mvisit(ir_lambda *L) {
    L->sidx = L->slow = ++mcount;
    mstack[nmstack++] = L;
    L->onstack = 1;
    medge(L, L->body);
    if (L->slow == L->sidx) {
        int n = 0;
        ir_lambda *M;
        do { M = mstack[--nmstack]; M->onstack = 0; n++; } while (M != L);
        if (n > 1) merge_group(mstack + nmstack, n);
    }
}
static void merge_mutual(void) {
    ir_lambda *L;
    int n = 0;
    census();
    collect_sites();
    for (L = P->lambdas; L; L = L->next) { L->sidx = L->slow = L->onstack = 0; if ((L->onstack2 = mcand(L))) n++; }
    mstack = NEWN(ir_lambda *, n);
    nmstack = mcount = 0;
    for (L = P->lambdas; L; L = L->next) if (L->onstack2 && !L->sidx) mvisit(L);
    for (L = P->lambdas; L; L = L->next) L->onstack2 = 0;
}

/* debugging: a term chain must end in a tail */
static int bad_term(ir_node *n) {
    for (; n; ) {
        switch (n->kind) {
        case IR_LET: n = n->u.let.body; if (!n) return 1; continue;
        case IR_FIX: n = n->u.fix.body; if (!n) return 2; continue;
        case IR_LETJ: { ir_join *j; for (j = n->u.letj.j; j; j = j->next) if (!j->body || bad_term(j->body)) return 3; n = n->u.letj.body; if (!n) return 4; continue; }
        case IR_IF: if (!n->u.if_.then || !n->u.if_.els || bad_term(n->u.if_.then)) return 5; n = n->u.if_.els; continue;
        default: return 0;
        }
    }
    return 6;
}
static void check_all(const char *when) {
    ir_lambda *L;
    if (!getenv("SHIKRA_CHECK")) return;
    for (L = P->lambdas; L; L = L->next)
        if (LIVE(L) && !L->nclauses && (!L->body || bad_term(L->body))) fprintf(stderr, "BROKEN %s after %s (round %d)\n", idname(L->name), when, round_no);
}

/* ---- the driver ---- */
void ir_optimize(ir_program *p) {
    long total = 0;
    int max_rounds;
    ir_lambda *L;
    P = p;
    switch (p->opt) {
    case OPT_0: lim_small = 6; lim_once = 0; max_rounds = 3; break;
    case OPT_S: lim_small = 12; lim_once = 4000; max_rounds = 6; break;
    default: lim_small = 40; lim_once = 4000; max_rounds = 8; break;
    }
    if (getenv("SHIKRA_ROUNDS")) max_rounds = atoi(getenv("SHIKRA_ROUNDS")); /* debugging */
    if (getenv("SHIKRA_NOINLINE")) lim_small = lim_once = -1;
    census();
    for (L = P->lambdas; L; L = L->next) if (LIVE(L)) total += term_size(L->body);
    growth = p->opt == OPT_2 ? total + 20000 : p->opt == OPT_S ? total / 4 + 2000 : total / 8 + 500;
    for (round_no = 1; round_no <= max_rounds; round_no++) {
        changed = 0;
        census();
        scc_all(0);
        global_consts(P->main->body);
        check_all("start");
        for (L = P->lambdas; L; L = L->next)
            if (LIVE(L) && !L->parent) simp_lambda(L);
        check_all("simplify");
        census();
        if (!getenv("SHIKRA_NOCONTIFY")) contify();
        check_all("contify");
        census();
        if (!getenv("SHIKRA_NOUNBOX")) unbox();
        check_all("unbox");
        if (!changed) break;
    }
    merge_mutual();
    census();
}

/* ---- tier 1 (spec §8.2): optimistic sites must end up with every jump to their
 * continuation in the C function that declares it, and nothing between capture and jump may
 * change the dynamic state (inlined dynamic-wind, parameterize, with-exception-handler). ---- */
static obj t1_fails;
static void t1_fail(obj site) {
    obj l;
    for (l = t1_fails; l != RT_NIL; l = RT_CDR(l)) if (RT_CAR(l) == site) return;
    t1_fails = rt_cons(site, t1_fails);
}
static int dyn_change(ir_node *n) {
    for (; n; ) {
        switch (n->kind) {
        case IR_LET: {
            ir_node *r = n->u.let.rhs;
            if (r->kind == IR_PRIM && (r->u.prim.op == PI_rt_winders_set || r->u.prim.op == PI_rt_handlers_set)) return 1;
            n = n->u.let.body;
            continue;
        }
        case IR_FIX: n = n->u.fix.body; continue;
        case IR_LETJ: {
            ir_join *j;
            for (j = n->u.letj.j; j; j = j->next) if (dyn_change(j->body)) return 1;
            n = n->u.letj.body;
            continue;
        }
        case IR_IF: if (dyn_change(n->u.if_.then)) return 1; n = n->u.if_.els; continue;
        default: return 0;
        }
    }
    return 0;
}
static void t1_walk(ir_node *n, ir_lambda *L) {
    for (; n; ) {
        switch (n->kind) {
        case IR_LET: n = n->u.let.body; continue;
        case IR_FIX: n = n->u.fix.body; continue;
        case IR_LETJ: {
            ir_join *j;
            for (j = n->u.letj.j; j; j = j->next) {
                if (j->site && dyn_change(n->u.letj.body)) t1_fail(j->site);
                t1_walk(j->body, L);
            }
            n = n->u.letj.body;
            continue;
        }
        case IR_JUMP:
            if (n->u.jump.j->owner != L) {
                if (!n->u.jump.j->site && getenv("SHIKRA_TRACE")) fprintf(stderr, "BAD JUMP j%d\n", n->u.jump.j->id);
                else if (!n->u.jump.j->site) cerror(RT_FALSE, "internal error: jump to j%d (of %s) from %s", n->u.jump.j->id, n->u.jump.j->owner ? idname(n->u.jump.j->owner->name) : "?", idname(L->name));
                t1_fail(n->u.jump.j->site);
            }
            return;
        case IR_IF: t1_walk(n->u.if_.then, L); n = n->u.if_.els; continue;
        default: return;
        }
    }
}
obj ir_tier1_failures(ir_program *p) {
    ir_lambda *L;
    P = p;
    t1_fails = RT_NIL;
    census();
    for (L = P->lambdas; L; L = L->next) if (LIVE(L) && !L->nclauses) t1_walk(L->body, L);
    return t1_fails;
}

/* ---- calling conventions (spec §8.1; DESIGN.md M4-1 "calling conventions") ----
 * A procedure gets a direct C signature obj f(params...) when its closure never reaches
 * unknown code (every reference is a known call), it is not a case-lambda, and it is on no
 * cycle of the tail-call graph (known tail calls, unknown tail calls to every escaping
 * procedure). Calls into and out of it are plain C calls; since no cycle of tail calls
 * passes through it, such calls happen a bounded number of times in a row, so tail calls
 * stay proper. Its free variables become extra parameters (lambda lifting). */
int ir_call_self(ir_node *c) {
    ir_lambda *L = c->u.call.known, *R;
    if (!L || c->u.call.ctl != CTL_NONE) return 1;
    if (L->direct) return 0;
    R = real(L);
    return R->is_static && !R->escapes ? 2 : 1;
}

static int in_tree(ir_lambda *owner, ir_lambda *L) {
    for (; owner; owner = owner->parent) if (owner == L) return 1;
    return 0;
}
static int add_var(ir_lambda *L, ir_var *v) {
    int i;
    for (i = 0; i < L->nfree; i++) if (L->free[i] == v) return 0;
    if ((L->nfree & 7) == 0) {
        ir_var **n = NEWN(ir_var *, L->nfree + 8);
        if (L->nfree) memcpy(n, L->free, sizeof(ir_var *) * (size_t)L->nfree);
        L->free = n;
    }
    L->free[L->nfree++] = v;
    return 1;
}
/* Free variables of direct procedure L's tree (not counting nested direct procedures, whose
 * free variables reach L through their call sites). */
static int lift_changed;
static void lift_atom(ir_lambda *L, ir_node *a) {
    ir_var *v;
    if (a->kind != IR_REF) return;
    v = a->u.ref.v;
    if (!(v->flags & V_GLOBAL) && !in_tree(v->owner, L)) lift_changed |= add_var(L, v);
}
static void lift_term(ir_lambda *L, ir_node *n) {
    int i;
    for (; n; ) {
        switch (n->kind) {
        case IR_CONST: case IR_REF: lift_atom(L, n); return;
        case IR_PRIM: for (i = 0; i < n->u.prim.n; i++) lift_atom(L, n->u.prim.args[i]); return;
        case IR_CALL:
            if (n->u.call.known && n->u.call.known->direct) {
                ir_lambda *M = n->u.call.known;
                for (i = 0; i < M->nfree; i++)
                    if (!in_tree(M->free[i]->owner, L)) lift_changed |= add_var(L, M->free[i]);
            } else if (n->u.call.fn) lift_atom(L, n->u.call.fn);
            for (i = 0; i < n->u.call.n; i++) lift_atom(L, n->u.call.args[i]);
            return;
        case IR_LAMBDA:
            if (!n->u.lam->direct) {
                ir_lambda *M = n->u.lam;
                if (M->nclauses) for (i = 0; i < M->nclauses; i++) lift_term(L, M->clauses[i]->body);
                else lift_term(L, M->body);
            }
            return;
        case IR_SET: lift_atom(L, n->u.set.val); if (!(n->u.set.v->flags & V_GLOBAL) && !in_tree(n->u.set.v->owner, L)) lift_changed |= add_var(L, n->u.set.v); return;
        case IR_LET: lift_term(L, n->u.let.rhs); n = n->u.let.body; continue;
        case IR_FIX:
            for (i = 0; i < n->u.fix.n; i++) if (!n->u.fix.lams[i]->direct) lift_term(L, n->u.fix.lams[i]->body);
            n = n->u.fix.body;
            continue;
        case IR_LETJ: {
            ir_join *j;
            for (j = n->u.letj.j; j; j = j->next) lift_term(L, j->body);
            n = n->u.letj.body;
            continue;
        }
        case IR_JUMP: for (i = 0; i < n->u.jump.n; i++) lift_atom(L, n->u.jump.args[i]); return;
        case IR_IF: lift_atom(L, n->u.if_.test); lift_term(L, n->u.if_.then); n = n->u.if_.els; continue;
        case IR_RET: lift_atom(L, n->u.ret); return;
        }
    }
}
/* Renaming of lifted variables to the new parameters (through ->subst, scoped by order). */
static void ren_atom(ir_node *a) {
    if (a->kind == IR_REF && a->u.ref.v->subst) {
        ir_var *p = a->u.ref.v->subst->u.ref.v;
        a->u.ref.v = p;
    }
}
static void ren_term(ir_node *n) {
    int i;
    for (; n; ) {
        switch (n->kind) {
        case IR_CONST: case IR_REF: ren_atom(n); return;
        case IR_PRIM: for (i = 0; i < n->u.prim.n; i++) ren_atom(n->u.prim.args[i]); return;
        case IR_CALL:
            if (n->u.call.fn) ren_atom(n->u.call.fn);
            for (i = 0; i < n->u.call.n; i++) ren_atom(n->u.call.args[i]);
            return;
        case IR_LAMBDA:
            if (!n->u.lam->direct) {
                ir_lambda *M = n->u.lam;
                if (M->nclauses) for (i = 0; i < M->nclauses; i++) ren_term(M->clauses[i]->body);
                else ren_term(M->body);
            }
            return;
        case IR_SET:
            ren_atom(n->u.set.val);
            if (n->u.set.v->subst) n->u.set.v = n->u.set.v->subst->u.ref.v;
            return;
        case IR_LET: ren_term(n->u.let.rhs); n = n->u.let.body; continue;
        case IR_FIX:
            for (i = 0; i < n->u.fix.n; i++) if (!n->u.fix.lams[i]->direct) ren_term(n->u.fix.lams[i]->body);
            n = n->u.fix.body;
            continue;
        case IR_LETJ: {
            ir_join *j;
            for (j = n->u.letj.j; j; j = j->next) ren_term(j->body);
            n = n->u.letj.body;
            continue;
        }
        case IR_JUMP: for (i = 0; i < n->u.jump.n; i++) ren_atom(n->u.jump.args[i]); return;
        case IR_IF: ren_atom(n->u.if_.test); ren_term(n->u.if_.then); n = n->u.if_.els; continue;
        case IR_RET: ren_atom(n->u.ret); return;
        }
    }
}
static int depth(ir_lambda *L) { int d = 0; for (; L; L = L->parent) d++; return d; }

static void drop_direct(ir_node **slot) {
    int i, k;
    for (;;) {
        ir_node *n = *slot;
        switch (n->kind) {
        case IR_LET:
            if (n->u.let.rhs->kind == IR_LAMBDA && n->u.let.rhs->u.lam->direct) { *slot = n->u.let.body; continue; }
            slot = &n->u.let.body;
            continue;
        case IR_FIX:
            for (i = k = 0; i < n->u.fix.n; i++)
                if (!n->u.fix.lams[i]->direct) { n->u.fix.vars[k] = n->u.fix.vars[i]; n->u.fix.lams[k++] = n->u.fix.lams[i]; }
            if (!(n->u.fix.n = k)) { *slot = n->u.fix.body; continue; }
            slot = &n->u.fix.body;
            continue;
        case IR_LETJ: {
            ir_join *j;
            for (j = n->u.letj.j; j; j = j->next) drop_direct(&j->body);
            slot = &n->u.letj.body;
            continue;
        }
        case IR_IF: drop_direct(&n->u.if_.then); slot = &n->u.if_.els; continue;
        default: return;
        }
    }
}
/* Direct procedures: unused parameters dropped, the rest list built by the caller, free
 * variables appended as parameters; their bindings disappear. */
static void make_direct(void) {
    ir_lambda *L, **ds;
    int nd = 0, i, k;
    csite *s;
    for (L = P->lambdas; L; L = L->next) if (LIVE(L) && L->direct) nd++;
    if (!nd) return;
    ds = NEWN(ir_lambda *, nd);
    nd = 0;
    for (L = P->lambdas; L; L = L->next) if (LIVE(L) && L->direct) { ds[nd++] = L; L->nfree = 0; L->free = NULL; }
    do { /* free variables, to a fixpoint through calls of direct procedures */
        lift_changed = 0;
        for (i = 0; i < nd; i++) lift_term(ds[i], ds[i]->body);
    } while (lift_changed);
    collect_sites();
    for (i = 0; i < nd; i++) { /* call sites: rest list, unused parameters, lifted arguments */
        ir_lambda *D = ds[i];
        int keepall = D->entry && D->entry->njumps > 0, np = 0;
        char *drop = NEWN(char, D->nparams);
        ir_var **params;
        for (k = 0; k < D->nparams; k++) drop[k] = !keepall && D->params[k]->nrefs == 0;
        for (s = (csite *)pm_get(&sites, D); s; s = s->next) {
            ir_node *c = s->call, **args = NEWN(ir_node *, D->nparams + D->nfree), **pre = s->slot;
            int n = 0;
            for (k = 0; k < D->nreq; k++) if (!drop[k]) args[n++] = c->u.call.args[k];
            if (D->rest && !drop[D->nreq]) {
                ir_node *l = mk_const(RT_NIL);
                for (k = c->u.call.n - 1; k >= D->nreq; k--) {
                    ir_var *r = ir_new_var(sym("rest"), s->in);
                    ir_node *p = ir_new_node(IR_PRIM, c->loc);
                    p->u.prim.op = PI_rt_cons;
                    p->u.prim.n = 2;
                    p->u.prim.args = NEWN(ir_node *, 2);
                    p->u.prim.args[0] = c->u.call.args[k];
                    p->u.prim.args[1] = l;
                    *pre = mk_let(r, p, *pre, c->loc);
                    pre = &(*pre)->u.let.body;
                    l = mk_ref(r);
                }
                args[n++] = l;
            }
            for (k = 0; k < D->nfree; k++) args[n++] = mk_ref(D->free[k]);
            c->u.call.args = args;
            c->u.call.n = n;
        }
        params = NEWN(ir_var *, D->nparams + D->nfree);
        for (k = 0; k < D->nparams; k++) if (!drop[k]) params[np++] = D->params[k];
        for (k = 0; k < D->nfree; k++) params[np++] = ir_new_var(D->free[k]->name, D);
        if (D->entry) D->entry->params = D->params; /* self jumps keep passing the old parameters */
        D->params = params;
        D->nparams = np;
        D->nreq = np;
        D->rest = 0;
    }
    for (L = P->lambdas; L; L = L->next) /* the bindings (closure creations) go away */
        if (LIVE(L) && !L->nclauses && L->body) drop_direct(&L->body);
    /* rename the lifted variables inside each direct procedure, innermost first */
    for (i = 0; i < nd; i++)
        for (k = i + 1; k < nd; k++)
            if (depth(ds[k]) > depth(ds[i])) { L = ds[i]; ds[i] = ds[k]; ds[k] = L; }
    for (i = 0; i < nd; i++) {
        ir_lambda *D = ds[i];
        int base = D->nparams - D->nfree;
        if (!D->nfree) continue;
        for (k = 0; k < D->nfree; k++) {
            ir_node *r = ir_new_node(IR_REF, 0);
            r->u.ref.v = D->params[base + k];
            D->free[k]->subst = r;
        }
        ren_term(D->body);
        for (k = 0; k < D->nfree; k++) D->free[k]->subst = NULL;
    }
    for (i = 0; i < nd; i++) { ds[i]->nfree = 0; ds[i]->free = NULL; }
}

/* ---- closure layout: free variables per procedure (a case-lambda's clauses share the
 * union); a direct callee needs no closure, its own variable is not captured. ---- */
static void add_free(ir_lambda *L, ir_var *v) {
    ir_lambda *me = real(L);
    if ((v->flags & V_GLOBAL) || v->owner == L || (v == me->self && !(v->flags & V_ASSIGNED))) return;
    add_var(L, v);
}
static void free_lambda(ir_lambda *L);
static void free_sub(ir_lambda *L, ir_lambda *M) {
    int i;
    free_lambda(M);
    for (i = 0; i < M->nfree; i++) add_free(L, M->free[i]);
}
static void free_atom(ir_lambda *L, ir_node *a) { if (a->kind == IR_REF) add_free(L, a->u.ref.v); }
static void free_node(ir_lambda *L, ir_node *n) {
    int i;
    for (; n; ) {
        switch (n->kind) {
        case IR_CONST: case IR_REF: free_atom(L, n); return;
        case IR_PRIM: for (i = 0; i < n->u.prim.n; i++) free_atom(L, n->u.prim.args[i]); return;
        case IR_CALL:
            if (n->u.call.fn && (!n->u.call.known || ir_call_self(n) == 1)) free_atom(L, n->u.call.fn);
            for (i = 0; i < n->u.call.n; i++) free_atom(L, n->u.call.args[i]);
            return;
        case IR_LAMBDA: free_sub(L, n->u.lam); return;
        case IR_SET: add_free(L, n->u.set.v); free_atom(L, n->u.set.val); return;
        case IR_LET: free_node(L, n->u.let.rhs); n = n->u.let.body; continue;
        case IR_FIX:
            for (i = 0; i < n->u.fix.n; i++) free_sub(L, n->u.fix.lams[i]);
            n = n->u.fix.body;
            continue;
        case IR_LETJ: {
            ir_join *j;
            for (j = n->u.letj.j; j; j = j->next) free_node(L, j->body);
            n = n->u.letj.body;
            continue;
        }
        case IR_JUMP: for (i = 0; i < n->u.jump.n; i++) free_atom(L, n->u.jump.args[i]); return;
        case IR_IF: free_atom(L, n->u.if_.test); free_node(L, n->u.if_.then); n = n->u.if_.els; continue;
        case IR_RET: free_atom(L, n->u.ret); return;
        }
    }
}
static void free_lambda(ir_lambda *L) {
    int i;
    if (L->reached) return;
    L->reached = 1;
    if (L->nclauses) {
        for (i = 0; i < L->nclauses; i++) free_sub(L, L->clauses[i]);
        for (i = 0; i < L->nclauses; i++) { L->clauses[i]->nfree = L->nfree; L->clauses[i]->free = L->free; }
    } else free_node(L, L->body);
}

void ir_finish(ir_program *p) {
    ir_lambda *L, **pp;
    int i;
    P = p;
    for (i = 0; i < P->nvars; i++) P->vars[i]->subst = NULL;
    census();
    /* escapes: the closure may be called by unknown code */
    for (L = P->lambdas; L; L = L->next) {
        ir_lambda *R = real(L);
        L->escapes = LIVE(L) && L != P->main && (R->keep || !R->self || R->self->known != R);
        L->is_static = 0;
    }
    for (i = 0; i < P->nvars; i++) { /* a reference that is not the callee of a known call */
        ir_var *v = P->vars[i];
        if (v->known && v->nrefs > v->ncalls) v->known->escapes = 1;
    }
    for (L = P->lambdas; L; L = L->next) if (L->dispatch && L->dispatch->escapes) L->escapes = 1;
    /* the tail-call graph; direct signatures off every cycle */
    scc_all(1);
    for (L = P->lambdas; L; L = L->next)
        L->direct = !getenv("SHIKRA_NODIRECT") && LIVE(L) && L != P->main && !L->escapes && !L->rec && !L->nclauses && !L->dispatch && L->nparams <= 64;
    make_direct();
    census();
    /* closure layout */
    for (L = P->lambdas; L; L = L->next) { L->reached = 0; if (!L->nclauses && !L->dispatch) { L->nfree = 0; L->free = NULL; } }
    for (L = P->lambdas; L; L = L->next)
        if (LIVE(L) && (!L->parent || L->direct)) free_lambda(L);
        else if (LIVE(L) && L->dispatch && !LIVE(L->dispatch)) free_lambda(L->dispatch); /* clauses called directly */
    for (L = P->lambdas; L; L = L->next) {
        if (!L->reached) continue;
        if (L->direct && L->nfree) cerror(RT_FALSE, "internal error: direct procedure %s has free variables", idname(L->name));
        L->is_static = L->nfree == 0;
    }
    /* the procedures that remain, in creation order */
    for (pp = &P->lambdas, L = P->lambdas; L; L = L->next)
        if (LIVE(L) && L->reached) { *pp = L; pp = &L->next; }
    *pp = NULL;
}
