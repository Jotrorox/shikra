/* analyze.c -- the flow analysis (spec §5 pass 5, §5.2) and what it enables (pass 6: deleting
 * proven checks, specialising primitives, unboxing; DESIGN.md §11 M4-2).
 *
 * One whole-program, monovariant analysis over the optimized IR: for every variable the set of
 * types that can reach it (a bit set, T_* in ir.h), which procedure it can hold (one lambda, or
 * "many"), which record type (allocation site of its %make-rtd) -- and so for every call site the
 * procedures that can reach it. Abstract interpretation: every reached procedure body is walked
 * with an environment of path facts (type refinements after type predicates and after checked
 * primitives that succeeded, order relations after comparisons), joining into variables, join
 * point parameters, procedure parameters and results, box contents, record fields and the three
 * port-parameter cells, until nothing changes (widening to "any" after WIDEN rounds). A call or
 * primitive whose result is empty never returns: the code after it is dead. A last, recording
 * walk annotates the IR (refined types of atoms, S_* flags of primitives) and rewrites it
 * (decided predicates and branches, dead code, calls that became known), and reports provably
 * wrong types. ir_represent chooses C representations after ir_finish. */
#include "ir.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { int t, r; ir_lambda *f; int lt; } aval; /* types, record site, procedure, types from local facts alone */
static ir_lambda many_lam;
#define MANYF (&many_lam)
#define WIDEN 30
#define LTSET (T_ANY + 1) /* mkav: lt is a type set */
static const aval NONE = {0, 0, NULL, 0};

static ir_program *P;
static aval *VT, *CONT, *RES, PC[3]; /* variables; box contents; procedure results; port cells */
static char *REACH, *ESC;            /* procedure reached; called by unknown code */
static ir_node **TDEF;               /* variable -> the primitive whose result it is (tests) */
static int nvars, nlams, njoins, changed, iter, widen, rec, final, stamp, structural, nwalk;
static int pccar, stdclose, handlers_set, flo_src;
static ir_lambda *cur;
static int uguard, lguard, inloop; /* inside branches of undecided ifs of user / library code */
static int hguard;                 /* after a handler was installed (a guard body, inlined) */
static pmap reported;
static void walk_lambda(ir_lambda *L);
static int *WALKED; /* procedure -> round it was walked in */

/* ---- the lattice ---- */
/* lt: the types this value can have judging by the procedure's own code (constants, primitive
 * results, its tests), i.e. without interprocedural flow; 1 = t, 0 = any (diagnostics). */
static aval mkav(int t, ir_lambda *f, int r, int lt) {
    aval a;
    a.t = t;
    a.f = t & T_PROC ? f : NULL;
    a.r = t & (T_RECORD | T_RTD) ? r : 0;
    a.lt = lt == 1 ? t : lt == 0 ? T_TOP : (lt & T_ANY) | t;
    return a;
}
static aval top(void) { return mkav(T_TOP, MANYF, -1, 0); }
static aval mask(int t) { return mkav(t, MANYF, -1, 1); }
int ir_const_type(obj x) {
    if (rt_is_fix(x)) return T_FIX;
    if (rt_is_char(x)) return T_CHAR;
    if (x == RT_TRUE) return T_TRUE;
    if (x == RT_FALSE) return T_FALSE;
    if (x == RT_NIL) return T_NULL;
    if (x == RT_EOF) return T_EOF;
    if (x == RT_UNSPEC) return T_UNSPEC;
    if (x == RT_UNDEF) return T_UNDEF;
    if (rt_is_pair(x)) return T_PAIR;
    if (rt_is_flo(x)) return T_FLO;
    if (rt_is_string(x)) return T_STRING;
    if (rt_is_symbol(x)) return T_SYMBOL;
    if (rt_is_vector(x)) return T_VECTOR;
    if (rt_is_bytevector(x)) return T_BYTEVECTOR;
    return T_OTHER;
}
int ir_atom_type(ir_node *a) {
    if (a->kind == IR_CONST) return ir_const_type(a->u.value);
    if (a->ty) return a->ty;
    return a->u.ref.v->type ? a->u.ref.v->type : T_TOP;
}
static void escape_lam(ir_lambda *L);
static ir_lambda *joinf(ir_lambda *a, ir_lambda *b) {
    if (!a || a == b) return b;
    if (!b) return a;
    if (a != MANYF) escape_lam(a); /* a call through the joined value is an unknown call */
    if (b != MANYF) escape_lam(b);
    return MANYF;
}
static aval join(aval a, aval b) {
    if (!a.t) return b;
    if (!b.t) return a;
    return mkav(a.t | b.t, joinf(a.f, b.f), !a.r ? b.r : !b.r || a.r == b.r ? a.r : -1, a.lt | b.lt | LTSET);
}
static aval meet(aval a, aval b) {
    int t = a.t & b.t;
    ir_lambda *f = a.f == MANYF ? b.f : b.f == MANYF || a.f == b.f ? a.f : NULL;
    int r = a.r == -1 ? b.r : b.r == -1 || a.r == b.r ? a.r : 0;
    if (!f) t &= ~T_PROC;
    if (!r) t &= ~(T_RECORD | T_RTD);
    return mkav(t, f, r, (a.lt & b.lt) | LTSET);
}
static aval minus(aval a, int t) { return mkav(a.t & ~t, a.f, a.r, (a.lt & ~t) | LTSET); }
static void flow(aval *d, aval s) {
    aval n;
    if (!s.t) return;
    n = join(*d, s);
    if (n.t == d->t && n.f == d->f && n.r == d->r && n.lt == d->lt) return;
    if (widen && d->t) { joinf(n.f, MANYF); n = join(n, top()); }
    *d = n;
    changed++;
}
static aval far(aval a) { a.lt = T_TOP; return a; }

/* ---- procedures: reached, escaped (unknown callers pass anything) ---- */
static void reach(ir_lambda *L) {
    int i;
    if (REACH[L->id]) return;
    REACH[L->id] = 1;
    changed = 1;
    if (L->nclauses) for (i = 0; i < L->nclauses; i++) reach(L->clauses[i]);
    else if (L->rest) flow(&VT[L->params[L->nreq]->id], mkav(T_PAIR | T_NULL, 0, 0, 0)); /* a list */
}
static void escape_lam(ir_lambda *L) {
    int i;
    if (ESC[L->id]) return;
    ESC[L->id] = 1;
    changed = 1;
    reach(L);
    if (L->nclauses) for (i = 0; i < L->nclauses; i++) escape_lam(L->clauses[i]);
    else for (i = 0; i < L->nreq; i++) flow(&VT[L->params[i]->id], top());
}
static void escape(aval a) { if (a.f && a.f != MANYF) escape_lam(a.f); }
static ir_lambda *resolve(ir_lambda *L, int n) {
    int i;
    if (L->nclauses) {
        for (i = 0; i < L->nclauses; i++) if (resolve(L->clauses[i], n)) return L->clauses[i];
        return NULL;
    }
    return n == L->nreq || (L->rest && n > L->nreq) ? L : NULL;
}

/* ---- path facts ---- */
enum { E_TYPE, E_REL };
enum { REL_LT, REL_LE, REL_GT, REL_GE };
typedef struct env { int kind, op; ir_var *v; aval a; ir_node *y; struct env *next; } env;
#define MAXENV 64
static int env_len(env *e) { int n = 0; for (; e; e = e->next) n++; return n; }
static int refinable(ir_var *v) { return !(v->flags & (V_ASSIGNED | V_BOXED)); }
static int has_flonum(obj x) { /* in a literal */
    intptr_t i;
    for (; rt_is_pair(x); x = RT_CDR(x)) if (has_flonum(RT_CAR(x))) return 1;
    if (rt_is_vector(x)) for (i = 0; i < RT_HLEN(x); i++) if (has_flonum(RT_VEC(x)[i])) return 1;
    return rt_is_flo(x);
}
static aval av_of(ir_node *a, env *e) {
    aval r;
    ir_var *v;
    if (a->kind == IR_CONST) {
        if (rec && !flo_src && has_flonum(a->u.value)) flo_src = 1;
        return mkav(ir_const_type(a->u.value), 0, 0, !(a->spec & N_FAR));
    }
    v = a->u.ref.v;
    if ((v->flags & V_GLOBAL) && v->known) r = mkav(T_PROC, v->known, 0, 0);
    else r = VT[v->id];
    if (a->u.ref.check) r.t &= ~T_UNDEF;
    if (refinable(v))
        for (; e; e = e->next)
            if (e->kind == E_TYPE && e->v == v) { r = meet(r, e->a); break; }
    if (v->owner != cur || (v->flags & V_FAR) || (a->spec & N_FAR)) r.lt = T_TOP;
    return mkav(r.t, r.f, r.r, r.lt | LTSET);
}
/* push a refinement of atom x to (its value meet m), using storage s */
static env *refine(env *e, env *s, ir_node *x, aval m) {
    if (x->kind != IR_REF || !refinable(x->u.ref.v) || env_len(e) >= MAXENV) return e;
    s->kind = E_TYPE;
    s->v = x->u.ref.v;
    s->a = meet(av_of(x, e), m);
    s->next = e;
    return s;
}
static env *relate(env *e, env *s, ir_node *x, int op, ir_node *y) {
    if (x->kind != IR_REF || !refinable(x->u.ref.v) || env_len(e) >= MAXENV) return e;
    if (y->kind == IR_REF && (!refinable(y->u.ref.v) || y->u.ref.check)) return e;
    s->kind = E_REL;
    s->v = x->u.ref.v;
    s->op = op;
    s->y = y;
    s->next = e;
    return s;
}

/* Join points: the facts that hold at every jump to a join that is not a loop. */
typedef struct { env *e; int n, cap, stamp, walked, loop; } jinfo;
static jinfo *J;
static int same_atom(ir_node *a, ir_node *b) {
    return a->kind == b->kind && (a->kind == IR_CONST ? a->u.value == b->u.value : a->u.ref.v == b->u.ref.v);
}
static void note_jump(ir_join *j, env *e) {
    jinfo *ji = &J[j->id];
    int i, k;
    if (ji->loop) { ji->stamp = stamp; return; }
    if (ji->stamp != stamp) {
        ji->stamp = stamp;
        ji->n = 0;
        for (; e; e = e->next) {
            for (k = 0; k < ji->n; k++) /* the innermost fact about a variable wins */
                if (e->kind == E_TYPE && ji->e[k].kind == E_TYPE && ji->e[k].v == e->v) break;
            if (k < ji->n) continue;
            if (ji->n == ji->cap) {
                ji->cap = ji->cap * 2 + 8;
                ji->e = (env *)realloc(ji->e, sizeof(env) * (size_t)ji->cap);
                if (!ji->e) die("out of memory");
            }
            ji->e[ji->n++] = *e;
        }
        return;
    }
    for (i = k = 0; i < ji->n; i++) { /* keep the facts this jump has too */
        env *f, *x = &ji->e[i];
        for (f = e; f; f = f->next)
            if (f->kind == x->kind && f->v == x->v && (f->kind == E_TYPE || (f->op == x->op && same_atom(f->y, x->y)))) break;
        if (!f) continue;
        if (x->kind == E_TYPE) x->a = join(x->a, f->a);
        ji->e[k++] = *x;
    }
    ji->n = k;
}
static env *join_env(ir_join *j) {
    jinfo *ji = &J[j->id];
    int i;
    for (i = 0; i < ji->n; i++) ji->e[i].next = i + 1 < ji->n ? &ji->e[i + 1] : NULL;
    return ji->n ? ji->e : NULL;
}
static int jumps_to(ir_node *n, ir_join *j) {
    int i;
    for (; n; ) {
        switch (n->kind) {
        case IR_LET: n = n->u.let.body; continue;
        case IR_FIX: n = n->u.fix.body; continue;
        case IR_LETJ: {
            ir_join *k;
            for (k = n->u.letj.j; k; k = k->next) if (jumps_to(k->body, j)) return 1;
            n = n->u.letj.body;
            continue;
        }
        case IR_IF: if (jumps_to(n->u.if_.then, j)) return 1; n = n->u.if_.els; continue;
        case IR_JUMP: return n->u.jump.j == j;
        default: (void)i; return 0;
        }
    }
    return 0;
}

/* ---- record types: one per %make-rtd site that runs at most once; fields tracked ---- */
typedef struct { ir_node *site; int nf; aval *f; } rsite;
static rsite *RS;
static int nrs, rs_cap;
static aval fld_any; /* stored into a record of unknown type */
static int site_of(ir_node *p) {
    int i;
    for (i = 0; i < nrs; i++) if (RS[i].site == p) return i + 1;
    if (nrs == rs_cap) {
        rs_cap = rs_cap * 2 + 8;
        RS = (rsite *)realloc(RS, sizeof(rsite) * (size_t)rs_cap);
        if (!RS) die("out of memory");
    }
    RS[nrs].site = p;
    RS[nrs].nf = p->u.prim.args[1]->kind == IR_CONST && rt_is_vector(p->u.prim.args[1]->u.value) ? (int)RT_HLEN(p->u.prim.args[1]->u.value) : 0;
    RS[nrs].f = NEWN(aval, RS[nrs].nf + 1);
    return ++nrs;
}
static aval *field(int r, ir_node *k) {
    if (r <= 0 || k->kind != IR_CONST || !rt_is_fix(k->u.value) || RT_FIXVAL(k->u.value) >= RS[r - 1].nf) return NULL;
    return &RS[r - 1].f[RT_FIXVAL(k->u.value)];
}
/* fields a fresh record (LET v = %make-record) gets right after, in straight-line code: the
 * others keep the initial unspecified value */
static void fresh_record(ir_node *let, int r) {
    ir_var *v = let->u.let.v;
    ir_node *n;
    int k, nf = r > 0 ? RS[r - 1].nf : 0;
    char set[64] = {0};
    if (r <= 0) return;
    for (n = let->u.let.body; v && n && n->kind == IR_LET; n = n->u.let.body) {
        ir_node *x = n->u.let.rhs;
        if (x->kind == IR_PRIM && x->u.prim.op == PI_rt_record_set && x->u.prim.args[0]->kind == IR_REF &&
            x->u.prim.args[0]->u.ref.v == v && x->u.prim.args[2]->kind == IR_CONST && RT_FIXVAL(x->u.prim.args[2]->u.value) < 64)
            set[RT_FIXVAL(x->u.prim.args[2]->u.value)] = 1;
        else if (x->kind != IR_CONST && x->kind != IR_REF && x->kind != IR_PRIM) break;
    }
    for (k = 0; k < nf; k++) if (k >= 64 || !set[k]) flow(&RS[r - 1].f[k], mask(T_UNSPEC));
}

/* ---- primitives ---- */
static int letter(char c) {
    switch (c) {
    case 'f': return T_FIX;
    case 'd': return T_FLO;
    case 'n': return T_NUM;
    case 'c': return T_CHAR;
    case 'b': return T_BOOL;
    case 'p': return T_PAIR | T_PCELL;
    case 'e': return T_NULL;
    case 'l': return T_PAIR | T_NULL;
    case 's': return T_STRING;
    case 'y': return T_SYMBOL;
    case 'v': return T_VECTOR;
    case 'u': return T_BYTEVECTOR;
    case 'x': return T_PROC;
    case 'r': return T_RECORD;
    default: return T_TOP;
    }
}
static const char *type_name(int t) {
    static const char *const names[] = {"a fixnum", "a flonum", "a character", "#t", "#f", "a pair", "the empty list",
        "a string", "a symbol", "a vector", "a bytevector", "a procedure", "a record", "the eof object", "the unspecified value",
        "a port", "a port", "a port", "a port", "a pair", "a pair", "a pair", "a record type", "undefined", "an object"};
    int i;
    if ((t & ~T_BOOL) == 0 && t == T_BOOL) return "a boolean";
    for (i = 0; i < 25; i++) if (t & (1 << i)) return names[i];
    return "nothing";
}
static const char *letter_name(char c) {
    switch (c) {
    case 'f': return "an exact integer";
    case 'd': return "a flonum";
    case 'n': return "a number";
    case 'c': return "a character";
    case 'b': return "a boolean";
    case 'p': return "a pair";
    case 'e': return "the empty list";
    case 'l': return "a list";
    case 's': return "a string";
    case 'y': return "a symbol";
    case 'v': return "a vector";
    case 'u': return "a bytevector";
    case 'x': return "a procedure";
    case 'r': return "a record";
    default: return "?";
    }
}
/* Type predicates: the types for which the result is #t; exact = #f for every other type. */
static int pred_mask(int op, int *exact) {
    *exact = 1;
    switch (op) {
    case PI_rt_pair_p: return T_PAIR | T_PCELL;
    case PI_rt_null_p: return T_NULL;
    case PI_rt_boolean_p: return T_BOOL;
    case PI_rt_procedure_p: return T_PROC;
    case PI_rt_symbol_p: return T_SYMBOL;
    case PI_rt_char_p: return T_CHAR;
    case PI_rt_string_p: return T_STRING;
    case PI_rt_vector_p: return T_VECTOR;
    case PI_rt_bytevector_p: return T_BYTEVECTOR;
    case PI_rt_number_p: return T_NUM;
    case PI_rt_exact_integer_p: return T_FIX;
    case PI_rt_eof_object_p: return T_EOF;
    case PI_rt_port_p: return T_PORTS;
    case PI_rt_input_port_p: case PI_rt_output_port_p: case PI_rt_textual_port_p: case PI_rt_binary_port_p:
        *exact = 0;
        return T_PORTS;
    case PI_rt_error_object_p: case PI_rt_promise_p: case PI_rt_values_p: case PI_rt_file_error_p: case PI_rt_read_error_p:
        *exact = 0;
        return T_OTHER;
    default: return 0;
    }
}
static aval decide(int t, int m, int exact) {
    if (!(t & m)) return mkav(T_FALSE, 0, 0, T_BOOL);
    if (exact && !(t & ~m)) return mkav(T_TRUE, 0, 0, T_BOOL);
    return mask(T_BOOL);
}
static int singleton(int t) { return t == T_TRUE || t == T_FALSE || t == T_NULL || t == T_EOF || t == T_UNSPEC; }
/* an argument of a primitive that may be stored or passed on (its procedure escapes) */
static int leaks(int op, int i) {
    char c = op < NPRIMS ? prim_table[op].args[i] : 'a';
    if (c != 'a' && c != 'l' && c != 'p' && c != 'v' && c != 'r') return 0;
    if (op < NPRIMS && (prim_table[op].flags & PF_PURE)) return 0;
    switch (op) {
    case PI_rt_car: case PI_rt_cdr: case PI_rt_length: case PI_rt_list_p: case PI_rt_equal_p: case PI_rt_vector_ref:
    case PI_rt_vector_length: case PI_rt_record_ref: case PI_rt_box_ref: case PI_rt_display: case PI_rt_write:
    case PI_rt_write_shared: case PI_rt_write_simple: case PI_rt_newline: case PI_rt_write_char: case PI_rt_write_string:
    case PI_rt_close_port: case PI_rt_parameter_binding: case PI_rt_continuation_live_p: case PI_rt_values_check:
    case PI_rt_values_ref: case PI_rt_flush_output_port: case PI_rt_read_char: case PI_rt_peek_char: case PI_rt_read_line:
    case PI_rt_read_string: case PI_rt_vector_to_list:
        return 0;
    case PI_rt_set_car: case PI_rt_set_cdr: case PI_rt_vector_set: case PI_rt_record_set: return i > 0;
    default: return 1;
    }
}
static int arith_t(int a, int b) { return (a & T_FIX && b & T_FIX ? T_FIX : 0) | ((a | b) & T_FLO && a & T_NUM && b & T_NUM ? T_FLO : 0); }

/* ---- diagnostics (spec §11.3) ----
 * A provably wrong type is an error when the proof is local -- the value follows from constants
 * and primitive results of the same procedure body (standard library procedures inlined into it
 * count as primitives; a user procedure's parameter or result, even inlined, does not) -- and the
 * code runs whenever the body does (no undecided conditional on the way); otherwise a warning,
 * and the code still raises at run time. Library code guarded by its own test (a deliberate
 * error path) is not reported. Dead code is never reported. */
static int lib_code(ir_node *n) {
    obj file;
    intptr_t line, col;
    if (n->spec & N_LIB) return 1;
    return n->loc && rt_srcloc_lookup(n->loc, &file, &line, &col) && rt_is_string(file) && !strncmp(RT_STR(file)->buf, "<builtin>", 9);
}
/* a body that runs when called directly: not a procedure passed to other code, which may run it
 * under an exception handler (test-error style) */
static int body_runs(ir_lambda *L) {
    ir_lambda *R = L->dispatch ? L->dispatch : L;
    return L == P->main || R->g || (R->self && R->self->known == R && R->self->nrefs == R->self->ncalls);
}
static void diagnose(ir_node *n, int local, const char *fmt, const char *a, const char *b) {
    char msg[256];
    obj where = n->loc ? n->loc : RT_FALSE;
    int lib = lib_code(n);
    if (!final || (lib && lguard)) return;
    snprintf(msg, sizeof msg, fmt, a, b);
    if (local && !uguard && !lguard && !hguard && body_runs(cur)) cerror(where, "%s", msg);
    if (!P->warn || (n->loc && pm_get(&reported, n->loc))) return;
    if (n->loc) pm_put(&reported, n->loc, (void *)1);
    cwarning(where, "%s (an error when it runs)", msg);
}

/* The value of a primitive application, with the facts it establishes when it returns. */
static aval prim(ir_node *p, env *e, env **out, env *s, ir_var *dst) {
    int op = p->u.prim.op, n = p->u.prim.n, i, ok, t;
    const prim_info *pi = &prim_table[op];
    aval a[6];
    ir_node **x = p->u.prim.args;
    for (i = 0; i < n && i < 6; i++) {
        a[i] = av_of(x[i], e);
        if (!a[i].t) return NONE; /* unreachable */
    }
    if (op == PI_VALUES) {
        if (n == 1) return a[0];
        for (i = 0; i < n; i++) escape(a[i]);
        return mkav(T_OTHER, 0, 0, 1);
    }
    if (rec) p->spec &= ~(S_OK(0) | S_OK(1) | S_OK(2) | S_OK(3) | S_OK(4) | S_OK(5) | S_NOOVF);
    for (i = 0; i < n && i < 6; i++) { /* checked arguments */
        int m = letter(pi->args[i]);
        if (op == PI_rt_record_ref || op == PI_rt_record_set) {
            if (i == 0 && (a[0].t & T_RECORD) && a[1].r > 0 && a[0].r == a[1].r && !(a[0].t & ~T_RECORD)) { if (rec) p->spec |= S_OK(0); }
            if (i == 0) {
                if (!(a[0].t & T_RECORD) || (a[0].r > 0 && a[1].r > 0 && a[0].r != a[1].r && !(a[0].t & ~T_RECORD))) {
                    if (rec) diagnose(p, !(a[0].lt & T_RECORD), "record accessor: expected a record of type %s, but the argument is always %s",
                                     a[1].r > 0 && rt_is_symbol(RS[a[1].r - 1].site->u.prim.args[0]->u.value) ? idname(RS[a[1].r - 1].site->u.prim.args[0]->u.value) : "?", type_name(a[0].t));
                    return NONE;
                }
                *out = refine(*out, s++, x[0], mkav(T_RECORD, MANYF, a[1].r > 0 ? a[1].r : -1, 1));
            }
            continue;
        }
        if (m == T_TOP) continue;
        ok = !(a[i].t & ~m);
        if (!(a[i].t & m)) {
            if (rec) {
                char want[64];
                snprintf(want, sizeof want, "%s: expected %s", pi->who, letter_name(pi->args[i]));
                diagnose(p, !(a[i].lt & m), "%s, but the argument is always %s", want, type_name(a[i].t));
            }
            return NONE; /* always raises */
        }
        if (ok && pi->args[i] != 'l' && rec) p->spec |= S_OK(i);
        if (!ok) *out = refine(*out, s++, x[i], mask(m));
    }
    for (i = 0; i < n; i++) if (leaks(op, i)) escape(a[i]);
    if (rec) /* the primitives that make a flonum out of no flonum (spec §9.1: float printer) */
        switch (op) {
        case PI_rt_div: case PI_rt_expt: case PI_rt_inexact: case PI_rt_exp: case PI_rt_log: case PI_rt_sin: case PI_rt_cos:
        case PI_rt_tan: case PI_rt_asin: case PI_rt_acos: case PI_rt_atan: case PI_rt_atan2: case PI_rt_sqrt:
        case PI_rt_current_second: case PI_rt_string_to_number: case PI_rt_read: flo_src = 1; break;
        default: break;
        }
    t = letter(pi->res);
    switch (op) {
    case PI_rt_add: case PI_rt_sub: case PI_rt_mul: case PI_rt_quotient: case PI_rt_remainder: case PI_rt_modulo:
    case PI_rt_floor_quotient:
        return mkav(arith_t(a[0].t, a[1].t), 0, 0, arith_t(a[0].lt & T_NUM, a[1].lt & T_NUM) | LTSET);
    case PI_rt_div:
        return mkav((a[0].t & T_FIX && a[1].t & T_FIX ? T_NUM : 0) | ((a[0].t | a[1].t) & T_FLO ? T_FLO : 0), 0, 0,
                    (a[0].lt & T_FIX && a[1].lt & T_FIX ? T_NUM : 0) | ((a[0].lt | a[1].lt) & T_FLO ? T_FLO : 0) | LTSET);
    case PI_rt_neg: case PI_rt_abs: case PI_rt_floor: case PI_rt_ceiling: case PI_rt_round: case PI_rt_truncate:
        return mkav(a[0].t & T_NUM, 0, 0, (a[0].lt & T_NUM) | LTSET);
    case PI_rt_expt: return mkav(T_NUM, 0, 0, 1);
    case PI_rt_exact_p: return decide(a[0].t, T_FIX, 1);
    case PI_rt_inexact_p: return decide(a[0].t, T_FLO, 1);
    case PI_rt_integer_p: return !(a[0].t & T_NUM) ? mask(T_FALSE) : !(a[0].t & ~T_FIX) ? mask(T_TRUE) : mask(T_BOOL);
    case PI_rt_list_p: return !(a[0].t & (T_PAIR | T_NULL)) ? mask(T_FALSE) : a[0].t == T_NULL ? mask(T_TRUE) : mask(T_BOOL);
    case PI_rt_eq_p: case PI_rt_eqv_p: case PI_rt_equal_p:
        if (!(a[0].t & a[1].t)) return mask(T_FALSE);
        if (op == PI_rt_eq_p && singleton(a[0].t) && a[0].t == a[1].t) return mask(T_TRUE);
        return mask(T_BOOL);
    case PI_rt_record_p:
        if (!(a[0].t & T_RECORD)) return mask(T_FALSE);
        if (a[1].r > 0 && a[0].r > 0 && !(a[0].t & ~T_RECORD)) return mask(a[0].r == a[1].r ? T_TRUE : T_FALSE);
        return mask(T_BOOL);
    case PI_rt_car: case PI_rt_cdr:
        if (a[0].t & T_PCELL) {
            aval v = a[0].t & T_PAIR ? top() : NONE;
            for (i = 0; i < 3; i++)
                if (a[0].t & (T_PCELL0 << i)) v = join(v, op == PI_rt_cdr ? PC[i] : pccar ? top() : mask(T_FALSE));
            return far(v);
        }
        return top();
    case PI_rt_set_car: case PI_rt_set_cdr:
        if (a[0].t & T_PCELL) {
            if (op == PI_rt_set_car) pccar = 1;
            else for (i = 0; i < 3; i++) if (a[0].t & (T_PCELL0 << i)) flow(&PC[i], far(a[1]));
            if (a[0].t & T_PAIR) escape(a[1]);
        }
        return mask(T_UNSPEC);
    case PI_rt_port_cell:
        return mask(x[0]->kind == IR_CONST && rt_is_fix(x[0]->u.value) && (uintptr_t)RT_FIXVAL(x[0]->u.value) < 3
                        ? T_PCELL0 << RT_FIXVAL(x[0]->u.value) : T_PCELL);
    case PI_rt_close_port: if (a[0].t & T_STDPORT) stdclose = 1; return mask(T_UNSPEC);
    case PI_rt_make_record: {
        aval v = mkav(T_RECORD, 0, a[0].r > 0 ? a[0].r : -1, 1);
        return v;
    }
    case PI_rt_make_rtd: return mkav(T_RTD, 0, 0, 1); /* the site: see walk */
    case PI_rt_record_ref: {
        aval *f = field(a[0].r, x[2]);
        return f && (a[0].t & ~T_RECORD) == 0 ? far(join(*f, fld_any)) : top();
    }
    case PI_rt_record_set: {
        aval *f = field(a[0].r, x[2]);
        escape(a[3]);
        if (f && (a[0].t & ~T_RECORD) == 0) flow(f, far(a[3]));
        else flow(&fld_any, far(a[3]));
        return mask(T_UNSPEC);
    }
    case PI_rt_make_box: if (dst) flow(&CONT[dst->id], far(a[0])); return mkav(T_OTHER, 0, 0, 1);
    case PI_rt_box_ref:
        return x[0]->kind == IR_REF && (x[0]->u.ref.v->flags & V_BOXED) ? far(CONT[x[0]->u.ref.v->id]) : top();
    case PI_rt_box_set:
        if (x[0]->kind == IR_REF && (x[0]->u.ref.v->flags & V_BOXED)) flow(&CONT[x[0]->u.ref.v->id], far(a[1]));
        else escape(a[1]);
        return mask(T_UNSPEC);
    case PI_rt_handlers_set: { /* installs a handler unless it restores or pops the current list */
        ir_node *d = x[0]->kind == IR_REF ? TDEF[x[0]->u.ref.v->id] : NULL;
        if (x[0]->kind == IR_REF && !(d && (d->u.prim.op == PI_rt_handlers_get || d->u.prim.op == PI_rt_cdr))) {
            if (rec) handlers_set = 1;
            hguard = 1;
        }
        return mask(T_UNSPEC);
    }
    case PI_rt_uncaught_o: case PI_rt_exit_o: return NONE; /* never return */
    case PI_rt_read_char: case PI_rt_peek_char: return mask(T_CHAR | T_EOF);
    case PI_rt_read_line: case PI_rt_read_string: return mask(T_STRING | T_EOF);
    case PI_rt_read_u8: case PI_rt_peek_u8: return mask(T_FIX | T_EOF);
    case PI_rt_string_to_number: return mask(T_NUM | T_FALSE);
    case PI_rt_digit_value: return mask(T_FIX | T_FALSE);
    case PI_rt_eof_object: return mask(T_EOF);
    case PI_rt_append2: return top();
    default: break;
    }
    {
        int ex, m = pred_mask(op, &ex);
        if (m) return decide(a[0].t, m, ex);
    }
    if (pi->res == 'a') return top();
    if (t == (T_PAIR | T_PCELL)) t = T_PAIR;
    return mkav(t, MANYF, -1, 1);
}

/* ---- calls ---- */
static aval call(ir_node *c, env *e) {
    int n = c->u.call.n, i;
    ir_lambda *K = c->u.call.known;
    aval f, r;
    if (c->u.call.ctl == CTL_CALLCC) {
        f = av_of(c->u.call.args[0], e);
        if (f.f && f.f != MANYF && (K = resolve(f.f, 1)) && !K->rest) {
            reach(K);
            flow(&VT[K->params[0]->id], mkav(T_PROC, MANYF, 0, 0));
        } else escape(f);
        return top();
    }
    if (c->u.call.ctl != CTL_NONE) { /* apply, call-with-values with an unknown receiver */
        for (i = 0; i < n; i++) escape(av_of(c->u.call.args[i], e));
        return top();
    }
    if (!K) {
        f = av_of(c->u.call.fn, e);
        if (!f.t) return NONE;
        if (!(f.t & T_PROC)) {
            if (rec) diagnose(c, !(f.lt & T_PROC), "not a procedure: the operator is always %s%s", type_name(f.t), "");
            return NONE;
        }
        if (f.f != MANYF) {
            K = resolve(f.f, n);
            if (!K && !(f.t & ~T_PROC)) {
                if (rec) diagnose(c, 0, "%s called with the wrong number of arguments%s", idname(f.f->name), "");
                return NONE;
            }
            if (K && rec && !(f.t & ~T_PROC) && c->u.call.fn->kind == IR_REF && !c->u.call.fn->u.ref.check) {
                c->u.call.known = K; /* the analysis found the only procedure that can be called here */
                c->spec |= N_DISC;
                structural = 1;
            }
        }
    }
    if (!K) {
        for (i = 0; i < n; i++) escape(av_of(c->u.call.args[i], e));
        return top();
    }
    reach(K);
    for (i = 0; i < n; i++) {
        aval a = av_of(c->u.call.args[i], e);
        if (i < K->nreq) flow(&VT[K->params[i]->id], far(a));
        else escape(a);
    }
    walk_lambda(K); /* its result for this round (no-op if walked or being walked) */
    r = RES[K->id];
    return far(r);
}

/* ---- the walk ---- */
/* Facts of a chain of bindings live until the walk of the chain returns: a stack of chunks. */
#define ECHUNK 4096
typedef struct echunk { env e[ECHUNK]; struct echunk *next, *prev; } echunk;
static echunk *ech;
static int etop;
static env *ealloc(int n) {
    if (!ech || etop + n > ECHUNK) {
        if (ech && ech->next) ech = ech->next;
        else {
            echunk *c = (echunk *)calloc(1, sizeof(echunk));
            if (!c) die("out of memory");
            c->prev = ech;
            if (ech) ech->next = c;
            ech = c;
        }
        etop = 0;
    }
    etop += n;
    return ech->e + etop - n;
}
static void walk(ir_node **slot, env *e);
static void ret(aval a) {
    flow(&RES[cur->id], a);
    if (ESC[cur->id] || (cur->dispatch && ESC[cur->dispatch->id])) escape(a);
}
static ir_node *mkconst(obj v) { ir_node *n = ir_new_node(IR_CONST, 0); n->u.value = v; return n; }
/* After a call or primitive that never returns, the rest of the term is dead. */
static void cut(ir_node *let) {
    ir_node *r;
    if (!rec || !let->u.let.body || let->u.let.body->kind == IR_RET) return;
    r = ir_new_node(IR_RET, let->loc);
    r->u.ret = let->u.let.v ? ir_new_node(IR_REF, 0) : mkconst(RT_UNSPEC);
    if (let->u.let.v) { r->u.ret->u.ref.v = let->u.let.v; let->u.let.v->nrefs++; }
    let->u.let.body = r;
    structural = 1;
}
/* the constant a decided primitive folds to (no effect, no allocation, cannot fail) */
static int fold_const(ir_node *r, aval a, obj *v) {
    int op = r->u.prim.op, i;
    if (op >= NPRIMS || !singleton(a.t) || (prim_table[op].flags & (PF_EFFECT | PF_ALLOC))) return 0;
    for (i = 0; i < r->u.prim.n; i++)
        if (prim_table[op].args[i] != 'a' && !(r->spec & S_OK(i))) return 0;
    *v = a.t == T_TRUE ? RT_TRUE : a.t == T_FALSE ? RT_FALSE : a.t == T_NULL ? RT_NIL : a.t == T_EOF ? RT_EOF : RT_UNSPEC;
    return 1;
}
static void walk_if(ir_node **slot, env *e) {
    ir_node *n = *slot, *t = n->u.if_.test, *d;
    aval a = av_of(t, e);
    env s[8], *et = e, *ee = e;
    int ug = uguard, lg = lguard, k = 0;
    if (rec && t->kind == IR_REF) t->ty = a.t;
    if (!a.t) return;
    if (!(a.t & T_FALSE) || a.t == T_FALSE) { /* decided */
        ir_node **b = !(a.t & T_FALSE) ? &n->u.if_.then : &n->u.if_.els;
        if (rec) { *slot = *b; b = slot; structural = 1; }
        walk(b, e);
        return;
    }
    if (t->kind == IR_REF) {
        et = refine(et, &s[k++], t, minus(top(), T_FALSE));
        ee = refine(ee, &s[k++], t, mask(T_FALSE));
        d = refinable(t->u.ref.v) ? TDEF[t->u.ref.v->id] : NULL;
        if (d) {
            ir_node **x = d->u.prim.args;
            int op = d->u.prim.op, ex, m = op < NPRIMS ? pred_mask(op, &ex) : 0;
            if (m) {
                et = refine(et, &s[k++], x[0], mask(m));
                if (ex) ee = refine(ee, &s[k++], x[0], minus(top(), m));
            } else if (op == PI_rt_record_p) {
                et = refine(et, &s[k++], x[0], mkav(T_RECORD, MANYF, av_of(x[1], e).r > 0 ? av_of(x[1], e).r : -1, 1));
            } else if (op == PI_rt_eq_p || op == PI_rt_eqv_p) {
                aval a0 = av_of(x[0], e), a1 = av_of(x[1], e);
                et = refine(et, &s[k++], x[0], a1);
                et = refine(et, &s[k++], x[1], a0);
                if (singleton(a1.t)) ee = refine(ee, &s[k++], x[0], minus(top(), a1.t));
                else if (singleton(a0.t)) ee = refine(ee, &s[k++], x[1], minus(top(), a0.t));
            } else if (op == PI_rt_lt || op == PI_rt_gt || op == PI_rt_num_le || op == PI_rt_num_ge) {
                static const int rel[4] = {REL_LT, REL_GT, REL_LE, REL_GE}, neg[4] = {REL_GE, REL_LE, REL_GT, REL_LT}, flip[4] = {REL_GT, REL_GE, REL_LT, REL_LE};
                int i = op == PI_rt_lt ? 0 : op == PI_rt_gt ? 1 : op == PI_rt_num_le ? 2 : 3;
                et = relate(et, &s[k++], x[0], rel[i], x[1]);
                ee = relate(ee, &s[k++], x[0], neg[i], x[1]);
                et = relate(et, &s[k++], x[1], flip[rel[i]], x[0]);
                ee = relate(ee, &s[k++], x[1], flip[neg[i]], x[0]);
            }
        }
    }
    if (final) { if (lib_code(n)) lguard++; else uguard++; }
    walk(&n->u.if_.then, et);
    walk(&n->u.if_.els, ee);
    uguard = ug;
    lguard = lg;
}
static void walk_letj(ir_node *n, env *e) {
    ir_join *j;
    int progress = 0, ug = uguard, lg = lguard;
    int pass = 0, before = changed;
    for (j = n->u.letj.j; j; j = j->next) J[j->id].walked = 0;
    walk(&n->u.letj.body, e);
    do {
        if (progress && !rec && !inloop && pass < 4 && n->u.letj.j->next == NULL && J[n->u.letj.j->id].loop && changed != before) {
            /* a loop whose body changed something: walk it again now, not in the next round */
            J[n->u.letj.j->id].walked = 0;
            pass++;
            before = changed;
        }
        progress = 0;
        for (j = n->u.letj.j; j; j = j->next) {
            jinfo *ji = &J[j->id];
            if (ji->stamp != stamp || ji->walked) continue;
            ji->walked = progress = 1;
            uguard = ug;
            lguard = lg;
            inloop += ji->loop;
            walk(&j->body, ji->loop ? e : join_env(j));
            inloop -= ji->loop;
        }
    } while (progress);
    uguard = ug;
    lguard = lg;
}
/* (+ x c) / (- x c) cannot overflow: a relation established by a dominating comparison bounds x
 * (x < y with y a fixnum, or x <= K for a constant K, K - c ... in range). */
static int bound_of(ir_var *v, env *e, int upper, intptr_t *k) {
    for (; e; e = e->next)
        if (e->kind == E_REL && e->v == v && e->y->kind == IR_CONST && rt_is_fix(e->y->u.value)) {
            intptr_t y = RT_FIXVAL(e->y->u.value);
            if (upper && (e->op == REL_LT || e->op == REL_LE)) { *k = e->op == REL_LT ? y - 1 : y; return 1; }
            if (!upper && (e->op == REL_GT || e->op == REL_GE)) { *k = e->op == REL_GT ? y + 1 : y; return 1; }
        }
    return 0;
}
static int no_overflow(ir_node *x, intptr_t c, env *e) {
    env *f;
    intptr_t k;
    if (c == 0) return 1;
    if (x->kind != IR_REF) return 0;
    if (bound_of(x->u.ref.v, e, c > 0, &k)) return c > 0 ? k <= RT_FIX_MAX - c : k >= RT_FIX_MIN - c;
    for (f = e; f; f = f->next) /* x < y (x > y) with y a fixnum variable: x + 1 (x - 1) fits */
        if (f->kind == E_REL && f->v == x->u.ref.v && f->y->kind == IR_REF && (av_of(f->y, e).t & ~T_FIX) == 0) {
            if (c == 1 && f->op == REL_LT) return 1;
            if (c == -1 && f->op == REL_GT) return 1;
            if (bound_of(f->y->u.ref.v, e, c > 0, &k)) { /* one step of transitivity */
                if (c > 0 && (f->op == REL_LT || f->op == REL_LE) && k - (f->op == REL_LT) <= RT_FIX_MAX - c) return 1;
                if (c < 0 && (f->op == REL_GT || f->op == REL_GE) && k + (f->op == REL_GT) >= RT_FIX_MIN - c) return 1;
            }
        }
    return 0;
}
static void noovf(ir_node *p, env *e) {
    ir_node **x = p->u.prim.args;
    int op = p->u.prim.op;
    intptr_t c;
    if ((op != PI_rt_add && op != PI_rt_sub) || !(p->spec & S_OK(0)) || !(p->spec & S_OK(1))) return;
    if ((ir_atom_type(x[0]) & ~T_FIX) || (ir_atom_type(x[1]) & ~T_FIX)) return;
    if (x[1]->kind == IR_CONST && rt_is_fix(x[1]->u.value)) {
        c = RT_FIXVAL(x[1]->u.value);
        if (op == PI_rt_sub) { if (c == RT_FIX_MIN) return; c = -c; }
        if (no_overflow(x[0], c, e)) p->spec |= S_NOOVF;
    } else if (op == PI_rt_add && x[0]->kind == IR_CONST && rt_is_fix(x[0]->u.value) && no_overflow(x[1], RT_FIXVAL(x[0]->u.value), e))
        p->spec |= S_NOOVF;
}
static int small_result(int op) { /* lengths, character codes, bytes */
    return op == PI_rt_length || op == PI_rt_string_length || op == PI_rt_vector_length || op == PI_rt_bytevector_length ||
           op == PI_rt_char_to_integer || op == PI_rt_bytevector_u8_ref;
}
static void annotate(ir_node *r, env *e) { /* refined types of the atoms of an rhs */
    int i;
    ir_node **a = r->kind == IR_PRIM ? r->u.prim.args : r->kind == IR_CALL ? r->u.call.args : NULL;
    int n = r->kind == IR_PRIM ? r->u.prim.n : r->kind == IR_CALL ? r->u.call.n : 0;
    for (i = 0; i < n; i++) if (a[i]->kind == IR_REF) a[i]->ty = av_of(a[i], e).t;
    if (r->kind == IR_REF) r->ty = av_of(r, e).t;
    if (r->kind == IR_SET && r->u.set.val->kind == IR_REF) r->u.set.val->ty = av_of(r->u.set.val, e).t;
}
static void walk1(ir_node **slot, env *e);
static void walk(ir_node **slot, env *e) {
    echunk *c = ech;
    int t = etop, h = hguard;
    walk1(slot, e);
    ech = c;
    etop = t;
    hguard = h;
}
static void walk1(ir_node **slot, env *e) {
    int i;
    nwalk++;
    for (;;) {
        ir_node *n = *slot;
        switch (n->kind) {
        case IR_LET: {
            ir_var *v = n->u.let.v;
            ir_node *r = n->u.let.rhs;
            env *s = ealloc(8), *e2 = e;
            aval a;
            if (rec) annotate(r, e);
            switch (r->kind) {
            case IR_CONST: case IR_REF: a = av_of(r, e); break;
            case IR_LAMBDA: a = mkav(T_PROC, r->u.lam, 0, 1); break;
            case IR_SET:
                flow(&VT[r->u.set.v->id], far(av_of(r->u.set.val, e)));
                a = mask(T_UNSPEC);
                break;
            case IR_CALL: a = call(r, e); break;
            default:
                a = prim(r, e, &e2, s, v);
                if (r->u.prim.op == PI_rt_make_rtd && a.t) a.r = cur == P->main && !inloop ? site_of(r) : -1;
                if (r->u.prim.op == PI_rt_make_record && a.t) fresh_record(n, a.r);
                if (rec && a.t) noovf(r, e);
                break;
            }
            if (!a.t) { cut(n); return; }
            if (v) {
                flow(&VT[v->id], a);
                TDEF[v->id] = r->kind == IR_PRIM && refinable(v) ? r : NULL;
                if (r->kind == IR_PRIM && small_result(r->u.prim.op) && refinable(v) && env_len(e2) < MAXENV - 2) { /* 0 <= v < 2^48 */
                    static ir_node *lo, *hi;
                    if (!lo) { lo = mkconst(RT_FIX(-1)); hi = mkconst(RT_FIX((intptr_t)1 << 48)); }
                    s[6].kind = s[7].kind = E_REL;
                    s[6].v = s[7].v = v;
                    s[6].op = REL_GT, s[6].y = lo, s[6].next = e2;
                    s[7].op = REL_LT, s[7].y = hi, s[7].next = &s[6];
                    e2 = &s[7];
                }
                if (rec && r->kind == IR_PRIM) {
                    obj c;
                    if (fold_const(r, a, &c)) { n->u.let.rhs = mkconst(c); structural = 1; }
                }
            }
            e = e2;
            slot = &n->u.let.body;
            continue;
        }
        case IR_FIX:
            for (i = 0; i < n->u.fix.n; i++) flow(&VT[n->u.fix.vars[i]->id], mkav(T_PROC, n->u.fix.lams[i], 0, 1));
            slot = &n->u.fix.body;
            continue;
        case IR_LETJ: walk_letj(n, e); return;
        case IR_JUMP:
            for (i = 0; i < n->u.jump.n; i++) {
                aval a = av_of(n->u.jump.args[i], e);
                if (rec && n->u.jump.args[i]->kind == IR_REF) n->u.jump.args[i]->ty = a.t;
                flow(&VT[n->u.jump.j->params[i]->id], a);
            }
            note_jump(n->u.jump.j, e);
            return;
        case IR_IF: walk_if(slot, e); return;
        case IR_RET:
            if (rec && n->u.ret->kind == IR_REF) n->u.ret->ty = av_of(n->u.ret, e).t;
            ret(av_of(n->u.ret, e));
            return;
        case IR_CALL:
            if (rec) annotate(n, e);
            ret(call(n, e));
            return;
        default: return;
        }
    }
}

/* ---- the driver ---- */
static void ids(void) {
    ir_lambda *L;
    nlams = njoins = 0;
    for (L = P->lambdas; L; L = L->next) {
        if (L->id >= nlams) nlams = L->id + 1;
        if (L->entry && L->entry->id >= njoins) njoins = L->entry->id + 1;
    }
}
static void scan_joins(ir_node *n) {
    for (; n; ) {
        switch (n->kind) {
        case IR_LET: n = n->u.let.body; continue;
        case IR_FIX: n = n->u.fix.body; continue;
        case IR_LETJ: {
            ir_join *j;
            for (j = n->u.letj.j; j; j = j->next) {
                if (j->id >= njoins) njoins = j->id + 1;
                scan_joins(j->body);
            }
            n = n->u.letj.body;
            continue;
        }
        case IR_IF: scan_joins(n->u.if_.then); n = n->u.if_.els; continue;
        default: return;
        }
    }
}
static void mark_loops(ir_node *n) {
    for (; n; ) {
        switch (n->kind) {
        case IR_LET: n = n->u.let.body; continue;
        case IR_FIX: n = n->u.fix.body; continue;
        case IR_LETJ: {
            ir_join *j;
            for (j = n->u.letj.j; j; j = j->next) {
                J[j->id].loop = n->u.letj.j->next != NULL || jumps_to(j->body, j);
                mark_loops(j->body);
            }
            n = n->u.letj.body;
            continue;
        }
        case IR_IF: mark_loops(n->u.if_.then); n = n->u.if_.els; continue;
        default: return;
        }
    }
}
/* A procedure is walked once per round; a call of one not yet walked in this round walks it
 * first (depth first), so a chain of calls does not cost a round per call. */
static void walk_lambda(ir_lambda *L) {
    ir_lambda *c = cur;
    int ug = uguard, lg = lguard, il = inloop, hg = hguard;
    if (L->dead || !REACH[L->id] || L->nclauses || !L->body || WALKED[L->id] == stamp) return;
    WALKED[L->id] = stamp;
    cur = L;
    uguard = lguard = inloop = hguard = 0;
    walk(&L->body, NULL);
    if (ESC[L->id]) escape(RES[L->id]);
    cur = c;
    uguard = ug, lguard = lg, inloop = il, hguard = hg;
}
static void walk_all(void) {
    ir_lambda *L;
    stamp++;
    for (L = P->lambdas; L; L = L->next) walk_lambda(L);
}
int ir_analyze(ir_program *p, int last) {
    ir_lambda *L;
    int i;
    P = p;
    nvars = p->nvars;
    ids();
    for (L = P->lambdas; L; L = L->next) if (!L->dead && L->body) scan_joins(L->body);
    VT = NEWN(aval, nvars);
    CONT = NEWN(aval, nvars);
    TDEF = NEWN(ir_node *, nvars);
    RES = NEWN(aval, nlams);
    REACH = NEWN(char, nlams);
    WALKED = NEWN(int, nlams);
    ESC = NEWN(char, nlams);
    J = NEWN(jinfo, njoins + 1);
    if (!ech) ealloc(0);
    while (ech->prev) ech = ech->prev;
    etop = 0;
    nrs = 0;
    fld_any = NONE;
    pccar = stdclose = handlers_set = flo_src = structural = nwalk = 0;
    PC[0] = mask(T_STDIN), PC[1] = mask(T_STDOUT), PC[2] = mask(T_STDERR);
    for (i = 0; i < 3; i++) PC[i].lt = T_TOP;
    for (L = P->lambdas; L; L = L->next) if (!L->dead && L->body) mark_loops(L->body);
    reach(P->main);
    for (L = P->lambdas; L; L = L->next) if (!L->dead && L->keep) escape_lam(L);
    rec = widen = 0;
    for (iter = 1;; iter++) {
        changed = 0;
        widen = iter > WIDEN;
        walk_all();
        if (!changed) break;
    }
    rec = 1;
    final = last;
    walk_all();
    rec = final = 0;
    for (i = 0; i < nvars; i++) P->vars[i]->type = VT[i].t;
    for (L = P->lambdas; L; L = L->next) L->rtype = RES[L->id].t;
    p->no_handlers = !handlers_set;
    p->no_flonums = !flo_src;
    p->std_open = !stdclose && !(PC[1].t & ~(T_STDOUT | T_STDERR)) && !(PC[2].t & ~(T_STDOUT | T_STDERR));
    if (getenv("SHIKRA_TRACE"))
        fprintf(stderr, "analysis: %d iterations, %d term walks, %d vars, %d procedures%s\n", iter, nwalk, nvars, nlams, structural ? ", rewrote" : "");
    for (i = 0; i <= njoins; i++) free(J[i].e);
    free(RS);
    RS = NULL;
    rs_cap = 0;
    return structural;
}

/* ---- representations (after ir_finish; spec §6.2, §4.1, §8.5) ----
 * A variable proven to hold only fixnums is a C intptr_t holding the tagged word (conversions
 * to obj are free). One proven to hold only flonums is a C double unless that costs boxing: a
 * flonum box is made where a double flows into a generic position (an obj variable or
 * parameter, a uniform procedure's argument or result, a closure, a non-float primitive);
 * the variable stays obj if it has two such uses, or one and a boxed source (so a value is
 * never unboxed and boxed again). Direct procedures get private signatures: their parameters
 * and their result in these representations; a direct procedure whose every return is
 * (values a b ...) of the same count, received only by known receivers that spread them,
 * returns a struct of n values (R_MV). */
static int *BOX, *SRC, *LBOX, *LSRC;
static int only_t(int t, int m) { return t && !(t & ~m); }
static int small_fixc(ir_node *n) {
    return n->kind == IR_CONST && rt_is_fix(n->u.value) && RT_FIXVAL(n->u.value) <= ((intptr_t)1 << 53) && RT_FIXVAL(n->u.value) >= -((intptr_t)1 << 53);
}
/* argument i is consumed as a double by the specialised form (the same conditions as emit.c's
 * special(): a double used anywhere else is boxed) */
static int float_slot(ir_node *r, int i) {
    int op = r->u.prim.op, n = r->u.prim.n, t0, t1;
    ir_node **a = r->u.prim.args;
    t0 = n > 0 ? ir_atom_type(a[0]) : 0;
    t1 = n > 1 ? ir_atom_type(a[1]) : 0;
    (void)i;
    switch (op) {
    case PI_rt_add: case PI_rt_sub: case PI_rt_mul: case PI_rt_div:
        return only_t(t0, T_NUM) && only_t(t1, T_NUM) && ((t0 | t1) & T_FLO) && (only_t(t0, T_FLO) || only_t(t1, T_FLO));
    case PI_rt_lt: case PI_rt_gt: case PI_rt_num_le: case PI_rt_num_ge: case PI_rt_num_eq:
        return (only_t(t0, T_FLO) && only_t(t1, T_FLO)) || (only_t(t0, T_FLO) && small_fixc(a[1])) || (only_t(t1, T_FLO) && small_fixc(a[0]));
    case PI_rt_neg: case PI_rt_abs: return only_t(t0, T_FLO);
    case PI_rt_sqrt: case PI_rt_exp: case PI_rt_log: case PI_rt_sin: case PI_rt_cos:
    case PI_rt_tan: case PI_rt_asin: case PI_rt_acos: case PI_rt_atan: case PI_rt_inexact:
        return only_t(t0, T_NUM);
    case PI_rt_atan2: return only_t(t0, T_NUM) && only_t(t1, T_NUM);
    default: return 0;
    }
}
static int raw_dbl(ir_node *r) { /* the specialised form yields a raw double */
    int op = r->u.prim.op;
    if (op == PI_rt_lt || op == PI_rt_gt || op == PI_rt_num_le || op == PI_rt_num_ge || op == PI_rt_num_eq) return 0;
    return float_slot(r, 0);
}
static int here_obj(ir_var *v) { /* read as an obj in cur: a global, or from the closure */
    int i;
    if (v->flags & V_GLOBAL) return 1;
    for (i = 0; i < cur->nfree; i++) if (cur->free[i] == v) return 1;
    return cur->self == v || (cur->dispatch && cur->dispatch->self == v);
}
static int is_dbl(ir_node *a) { return a->kind == IR_REF && !a->u.ref.check && a->u.ref.v->rep == R_DBL && !here_obj(a->u.ref.v); }
static void use_obj(ir_node *a) { if (is_dbl(a)) BOX[a->u.ref.v->id]++; } /* a double needed as an obj */
static void transfer(ir_node *a, int to_rep, int *src) { /* atom a flows into a slot of representation to_rep */
    if (to_rep == R_DBL) {
        if (a->kind == IR_REF && !is_dbl(a)) *src = 1;
    } else use_obj(a);
}
static int rep_of_call(ir_node *c) { return c->u.call.known && c->u.call.known->direct && c->u.call.ctl == CTL_NONE ? c->u.call.known->rrep : R_OBJ; }
static void cost_call(ir_node *c, int *dsrc, int drep) {
    int i;
    ir_lambda *D = c->u.call.known;
    if (D && D->direct && c->u.call.ctl == CTL_NONE)
        for (i = 0; i < c->u.call.n; i++) transfer(c->u.call.args[i], D->params[i]->rep, &SRC[D->params[i]->id]);
    else {
        for (i = 0; i < c->u.call.n; i++) use_obj(c->u.call.args[i]);
        if (c->u.call.fn) use_obj(c->u.call.fn);
    }
    if (drep == R_DBL && rep_of_call(c) != R_DBL) *dsrc = 1;
    if (drep != R_DBL && rep_of_call(c) == R_DBL && D) LBOX[D->id]++;
}
static void cost(ir_node *n) {
    int i;
    for (; n; ) {
        switch (n->kind) {
        case IR_LET: {
            ir_var *v = n->u.let.v;
            ir_node *r = n->u.let.rhs;
            int vrep = v ? v->rep : R_OBJ, *vsrc = v ? &SRC[v->id] : &i;
            switch (r->kind) {
            case IR_CONST: break;
            case IR_REF: transfer(r, vrep, vsrc); break;
            case IR_SET: transfer(r->u.set.val, (r->u.set.v->flags & V_GLOBAL) ? R_OBJ : r->u.set.v->rep, &SRC[r->u.set.v->id]); break;
            case IR_CALL: cost_call(r, vsrc, vrep); break;
            case IR_PRIM:
                for (i = 0; i < r->u.prim.n; i++) if (!float_slot(r, i)) use_obj(r->u.prim.args[i]);
                if (vrep == R_DBL && !raw_dbl(r)) *vsrc = 1;
                break;
            case IR_LAMBDA: {
                ir_lambda *M = r->u.lam;
                for (i = 0; i < M->nfree; i++) if (M->free[i]->rep == R_DBL && !here_obj(M->free[i])) BOX[M->free[i]->id]++;
                break;
            }
            default: break;
            }
            n = n->u.let.body;
            continue;
        }
        case IR_FIX: {
            int k;
            for (k = 0; k < n->u.fix.n; k++)
                for (i = 0; i < n->u.fix.lams[k]->nfree; i++) {
                    ir_var *f = n->u.fix.lams[k]->free[i];
                    if (f->rep == R_DBL && !here_obj(f)) BOX[f->id]++;
                }
            n = n->u.fix.body;
            continue;
        }
        case IR_LETJ: {
            ir_join *j;
            for (j = n->u.letj.j; j; j = j->next) cost(j->body);
            n = n->u.letj.body;
            continue;
        }
        case IR_JUMP:
            for (i = 0; i < n->u.jump.n; i++) transfer(n->u.jump.args[i], n->u.jump.j->params[i]->rep, &SRC[n->u.jump.j->params[i]->id]);
            return;
        case IR_IF: use_obj(n->u.if_.test); cost(n->u.if_.then); n = n->u.if_.els; continue;
        case IR_RET:
            if (cur->direct && cur->rrep == R_DBL) { if (n->u.ret->kind == IR_REF && !is_dbl(n->u.ret)) LSRC[cur->id] = 1; }
            else use_obj(n->u.ret);
            return;
        case IR_CALL: {
            int rr = cur->direct ? cur->rrep : R_OBJ;
            cost_call(n, &LSRC[cur->id], rr);
            return;
        }
        default: return;
        }
    }
}

/* values structs: every return of L is (values x ...) of n values or a tail call of such a procedure */
static int mv_n(ir_lambda *L, ir_node *n, int *cnt) {
    for (; n; ) {
        switch (n->kind) {
        case IR_LET: n = n->u.let.body; continue;
        case IR_FIX: n = n->u.fix.body; continue;
        case IR_LETJ: {
            ir_join *j;
            for (j = n->u.letj.j; j; j = j->next) if (!mv_n(L, j->body, cnt)) return 0;
            n = n->u.letj.body;
            continue;
        }
        case IR_IF: if (!mv_n(L, n->u.if_.then, cnt)) return 0; n = n->u.if_.els; continue;
        case IR_RET: {
            ir_node *a = n->u.ret, *d;
            int k;
            if (a->kind != IR_REF || !(d = a->u.ref.v->def) || a->u.ref.v->owner != L) return 0;
            if (d->kind == IR_PRIM) k = d->u.prim.n;
            else if (d->kind == IR_CALL && d->u.call.known && d->u.call.known->nvals) k = d->u.call.known->nvals;
            else return 0;
            if (*cnt && *cnt != k) return 0;
            *cnt = k;
            return 1;
        }
        case IR_CALL:
            if (n->u.call.ctl != CTL_NONE || !n->u.call.known || !n->u.call.known->nvals) return 0;
            if (*cnt && *cnt != n->u.call.known->nvals) return 0;
            *cnt = n->u.call.known->nvals;
            return 1;
        default: return 0;
        }
    }
    return 0;
}
static int *MVREFS, *MVUSES;
static void mv_scan(ir_node *n, int def) { /* def: record (values) and known-call bindings; count uses */
    int i;
    ir_node **a = NULL;
    int na = 0;
    for (; n; ) {
        switch (n->kind) {
        case IR_LET: {
            ir_node *r = n->u.let.rhs;
            ir_var *v = n->u.let.v;
            if (def && v) {
                v->def = NULL;
                if (r->kind == IR_PRIM && r->u.prim.op == PI_VALUES && r->u.prim.n >= 2 && r->u.prim.n <= 16) v->def = r;
                if (r->kind == IR_CALL && r->u.call.known && r->u.call.known->direct && !r->u.call.tail && r->u.call.ctl == CTL_NONE) v->def = r;
            }
            if (!def && r->kind == IR_PRIM && (r->u.prim.op == PI_rt_values_check || r->u.prim.op == PI_rt_values_ref) && r->u.prim.args[0]->kind == IR_REF)
                MVUSES[r->u.prim.args[0]->u.ref.v->id]++;
            if (r->kind == IR_PRIM) a = r->u.prim.args, na = r->u.prim.n;
            else if (r->kind == IR_CALL) a = r->u.call.args, na = r->u.call.n;
            else if (r->kind == IR_REF) a = &n->u.let.rhs, na = 1;
            else if (r->kind == IR_SET) a = &r->u.set.val, na = 1;
            else na = 0;
            if (!def) for (i = 0; i < na; i++) if (a[i]->kind == IR_REF) MVREFS[a[i]->u.ref.v->id]++;
            if (!def && r->kind == IR_CALL && r->u.call.fn && r->u.call.fn->kind == IR_REF) MVREFS[r->u.call.fn->u.ref.v->id]++;
            n = n->u.let.body;
            continue;
        }
        case IR_FIX: n = n->u.fix.body; continue;
        case IR_LETJ: {
            ir_join *j;
            for (j = n->u.letj.j; j; j = j->next) mv_scan(j->body, def);
            n = n->u.letj.body;
            continue;
        }
        case IR_IF: if (!def && n->u.if_.test->kind == IR_REF) MVREFS[n->u.if_.test->u.ref.v->id] += 1000; mv_scan(n->u.if_.then, def); n = n->u.if_.els; continue;
        case IR_JUMP: if (!def) for (i = 0; i < n->u.jump.n; i++) if (n->u.jump.args[i]->kind == IR_REF) MVREFS[n->u.jump.args[i]->u.ref.v->id] += 1000; return;
        case IR_CALL: if (!def) for (i = 0; i < n->u.call.n; i++) if (n->u.call.args[i]->kind == IR_REF) MVREFS[n->u.call.args[i]->u.ref.v->id] += 1000; return;
        default: return; /* RET: allowed for (values) and call results of a values procedure */
        }
    }
}
static int mv_receivers_ok(ir_lambda *D, ir_program *p) { /* every non-tail call of D only spreads its values */
    int i;
    for (i = 0; i < p->nvars; i++) {
        ir_var *v = p->vars[i];
        if (v->def && v->def->kind == IR_CALL && v->def->u.call.known == D && MVREFS[v->id] != MVUSES[v->id]) return 0;
    }
    return 1;
}
static void values_structs(ir_program *p) {
    ir_lambda *L;
    int i, again = 1;
    MVREFS = NEWN(int, p->nvars);
    MVUSES = NEWN(int, p->nvars);
    for (L = p->lambdas; L; L = L->next) { L->nvals = 0; if (L->body && !L->nclauses) mv_scan(L->body, 1); }
    for (L = p->lambdas; L; L = L->next) if (L->body && !L->nclauses) mv_scan(L->body, 0);
    for (L = p->lambdas; L; L = L->next) if (L->direct && L != p->main) L->nvals = -1; /* candidates */
    while (again) {
        again = 0;
        for (L = p->lambdas; L; L = L->next) {
            int n = 0;
            if (!L->nvals) continue;
            if (L->nvals < 0) L->nvals = 0; /* not yet known: a tail call into it is not enough */
            if (!mv_n(L, L->body, &n) || !mv_receivers_ok(L, p)) { if (L->nvals) again = 1; L->nvals = 0; continue; }
            if (L->nvals != n) again = 1;
            L->nvals = n;
        }
    }
    for (L = p->lambdas; L; L = L->next)
        if (L->nvals > 0) {
            L->rrep = R_MV;
            for (i = 0; i < p->nvars; i++) {
                ir_var *v = p->vars[i];
                if (v->def && ((v->def->kind == IR_CALL && v->def->u.call.known == L) || (v->def->kind == IR_PRIM && v->owner == L))) {
                    v->rep = R_MV;
                    v->slot = L->nvals;
                }
            }
        }
    for (i = 0; i < p->nvars; i++) if (p->vars[i]->rep != R_MV) p->vars[i]->def = NULL;
}

void ir_represent(ir_program *p) {
    ir_lambda *L;
    int i, again = 1, nl;
    P = p;
    nl = 0;
    for (L = p->lambdas; L; L = L->next) if (L->id >= nl) nl = L->id + 1;
    for (i = 0; i < p->nvars; i++) {
        ir_var *v = p->vars[i];
        v->rep = R_OBJ;
        if ((v->flags & (V_GLOBAL | V_BOXED)) || getenv("SHIKRA_NOREPS")) continue;
        if (v->type == T_FIX) v->rep = R_FIX;
        else if (v->type == T_FLO) v->rep = R_DBL;
    }
    for (L = p->lambdas; L; L = L->next) {
        L->rrep = R_OBJ;
        L->nvals = 0;
        if (L->direct && !getenv("SHIKRA_NOREPS")) L->rrep = L->rtype == T_FIX ? R_FIX : L->rtype == T_FLO ? R_DBL : R_OBJ;
        for (i = 0; i < L->nparams && !L->direct; i++) /* a uniform procedure's parameters arrive boxed */
            if (L->params[i]->rep == R_DBL) L->params[i]->rep = R_OBJ;
    }
    BOX = NEWN(int, p->nvars);
    SRC = NEWN(int, p->nvars);
    LBOX = NEWN(int, nl);
    LSRC = NEWN(int, nl);
    while (again) { /* demote doubles that would be boxed more than once (monotone: terminates) */
        again = 0;
        memset(BOX, 0, sizeof(int) * (size_t)p->nvars);
        memset(SRC, 0, sizeof(int) * (size_t)p->nvars);
        memset(LBOX, 0, sizeof(int) * (size_t)nl);
        memset(LSRC, 0, sizeof(int) * (size_t)nl);
        for (L = p->lambdas; L; L = L->next) if (L->body && !L->nclauses) { cur = L; cost(L->body); }
        for (i = 0; i < p->nvars; i++) {
            ir_var *v = p->vars[i];
            if (v->rep == R_DBL && (BOX[i] >= 2 || (BOX[i] && SRC[i]))) { v->rep = R_OBJ; again = 1; }
        }
        for (L = p->lambdas; L; L = L->next)
            if (L->rrep == R_DBL && (LBOX[L->id] >= 2 || (LBOX[L->id] && LSRC[L->id]))) { L->rrep = R_OBJ; again = 1; }
    }
    if (!getenv("SHIKRA_NOREPS")) values_structs(p);
    else for (i = 0; i < p->nvars; i++) p->vars[i]->def = NULL;
}
