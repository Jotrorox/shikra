/* emit.c -- the C backend (spec §5 pass 7, §8.1; DESIGN.md §3, §7, §11 M0 part B, M4-1).
 *
 * One translation unit: config #defines, rt.h, the generated code, the runtime sources.
 * Generated code: literals as static const data with no startup code (read-only flag on
 * strings, vectors, bytevectors; every pair in rt_lit_pairs[] so set-car!/set-cdr! can
 * detect literals; symbols listed in rt_static_syms[], entered into the symbol table only
 * when the program creates one), the globals array (a GC root range), one C function per
 * procedure -- the uniform signature, or a direct one (obj f(params...)) for procedures the
 * optimizer proved are only called by known calls off any tail-call cycle -- a static closure
 * for every procedure without free variables, and rt_program_main / rt_engine_raise (the
 * program-mode contract, DESIGN.md §11 M0 part A). Join points are labels; jumps assign
 * their parameters (in parallel) and goto. Tail calls between uniform procedures are
 * RT_TAIL (musttail, or the trampoline: rt.h); every other call is a plain C call wrapped
 * in RT_NT (which runs pending trampolined tail calls). Primitives are the checked runtime
 * functions named in prims.def. */
#include "ir.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static sbuf lits, code;    /* literal definitions; function bodies */
static sbuf pairs;         /* initialisers of rt_lit_pairs[] (all literal pairs, spec §3.2) */
static int nlits, nsyms, npairs;
static pmap litmap;        /* heap literal datum -> C expression */
static htab symmap;        /* symbol name -> C expression */
static sbuf symtab;        /* initialiser list of static symbols */
static ir_lambda *cur;     /* function being emitted */
static int nglob;

static const char *fmt(const char *f, ...) UTIL_PRINTF(1, 2);
static const char *fmt(const char *f, ...) {
    va_list ap;
    char buf[512];
    int n;
    va_start(ap, f);
    n = vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    if (n < (int)sizeof buf) return arena_strdup(&compiler_arena, buf);
    {
        char *big = (char *)arena_alloc(&compiler_arena, (size_t)n + 1);
        va_start(ap, f);
        vsnprintf(big, (size_t)n + 1, f, ap);
        va_end(ap);
        return big;
    }
}
static void out(int ind, const char *f, ...) UTIL_PRINTF(2, 3);
static void out(int ind, const char *f, ...) {
    va_list ap;
    char buf[1024];
    int n;
    while (ind-- > 0) sb_puts(&code, "    ");
    va_start(ap, f);
    n = vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    if (n < (int)sizeof buf) sb_puts(&code, buf);
    else {
        char *big = (char *)xmalloc((size_t)n + 1);
        va_start(ap, f);
        vsnprintf(big, (size_t)n + 1, f, ap);
        va_end(ap);
        sb_puts(&code, big);
        free(big);
    }
    sb_putc(&code, '\n');
}

/* ---- literals: static data ---- */
static void c_string(sbuf *b, const char *s, size_t n) {
    size_t i;
    sb_putc(b, '"');
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') { sb_putc(b, '\\'); sb_putc(b, c); }
        else if (c >= 0x20 && c < 0x7F && c != '?') sb_putc(b, c);
        else sb_printf(b, "\\%03o", c);
    }
    sb_putc(b, '"');
}
static const char *lit_string(const char *s, size_t n) {
    int k = nlits++;
    int ascii = rt_utf8_count(s, n) == n;
    sb_printf(&lits, "static const rt_string L%d = {RT_HDR(RT_T_STRING, RT_F_RO%s, 0), %ld, %ld, ", k, ascii ? " | RT_F_ASCII" : "",
              (long)rt_utf8_count(s, n), (long)n);
    c_string(&lits, s, n);
    sb_puts(&lits, "};\n");
    return fmt("((obj)((char *)&L%d + 5))", k);
}
static const char *lit(obj x) {
    const char *r;
    intptr_t i, n;
    if (rt_is_fix(x)) {
        intptr_t v = RT_FIXVAL(x);
        return v >= -2147483647 && v <= 2147483647 ? fmt("RT_FIX(%ld)", (long)v) : fmt("RT_FIX(%lldLL)", (long long)v);
    }
    if (rt_is_char(x)) return fmt("RT_CHAR(0x%x)", RT_CHARVAL(x));
    if (x == RT_TRUE) return "RT_TRUE";
    if (x == RT_FALSE) return "RT_FALSE";
    if (x == RT_NIL) return "RT_NIL";
    if (x == RT_UNSPEC) return "RT_UNSPEC";
    if (x == RT_UNDEF) return "RT_UNDEF";
    if (x == RT_DEFAULT) return "RT_DEFAULT";
    if (x == RT_EOF) return "RT_EOF";
    if (rt_is_symbol(x)) {
        rt_string *s = RT_STR(RT_PTR(x, rt_symbol)->name);
        int k;
        if ((r = (const char *)ht_get(&symmap, s->buf))) return r;
        r = lit_string(s->buf, (size_t)s->bytes);
        k = nlits++;
        sb_printf(&lits, "static const rt_symbol L%d = {RT_HDR(RT_T_SYMBOL, 0, 1), %s};\n", k, r);
        r = fmt("((obj)((char *)&L%d + 5))", k);
        sb_printf(&symtab, "%s%s", nsyms++ ? ", " : "", r);
        ht_put(&symmap, arena_strdup(&compiler_arena, s->buf), (void *)r);
        return r;
    }
    if ((r = (const char *)pm_get(&litmap, x))) return r;
    if (rt_is_pair(x)) {
        const char *a = lit(RT_CAR(x)), *d = lit(RT_CDR(x));
        int k = npairs++;
        sb_printf(&pairs, "%s{%s, %s}", k ? ",\n    " : "", a, d);
        r = fmt("((obj)((char *)&rt_lit_pairs[%d] + 1))", k);
    } else if (rt_is_string(x)) {
        r = lit_string(RT_STR(x)->buf, (size_t)RT_STR(x)->bytes);
    } else if (rt_is_flo(x)) {
        double d = RT_FLO(x);
        int k = nlits++;
        if (d != d) sb_printf(&lits, "static const rt_flonum L%d = {RT_HDR(RT_T_FLONUM, 0, 8), G_NAN};\n", k);
        else if (d - d != 0) sb_printf(&lits, "static const rt_flonum L%d = {RT_HDR(RT_T_FLONUM, 0, 8), %sG_INF};\n", k, d < 0 ? "-" : "");
        else sb_printf(&lits, "static const rt_flonum L%d = {RT_HDR(RT_T_FLONUM, 0, 8), %a};\n", k, d);
        r = fmt("((obj)((char *)&L%d + 5))", k);
    } else if (rt_is_vector(x)) {
        const char **el;
        int k;
        n = RT_HLEN(x);
        el = NEWN(const char *, n);
        for (i = 0; i < n; i++) el[i] = lit(RT_VEC(x)[i]);
        k = nlits++;
        if (n == 0) sb_printf(&lits, "static const struct { uintptr_t hdr; } L%d = {RT_HDR(RT_T_VECTOR, RT_F_RO, 0)};\n", k);
        else {
            sb_printf(&lits, "static const struct { uintptr_t hdr; obj v[%ld]; } L%d = {RT_HDR(RT_T_VECTOR, RT_F_RO, %ld), {", (long)n, k, (long)n);
            for (i = 0; i < n; i++) sb_printf(&lits, "%s%s", i ? ", " : "", el[i]);
            sb_puts(&lits, "}};\n");
        }
        r = fmt("((obj)((char *)&L%d + 5))", k);
    } else if (rt_is_bytevector(x)) {
        int k = nlits++;
        n = RT_HLEN(x);
        if (n == 0) sb_printf(&lits, "static const struct { uintptr_t hdr; } L%d = {RT_HDR(RT_T_BYTEVECTOR, RT_F_RO, 0)};\n", k);
        else {
            sb_printf(&lits, "static const struct { uintptr_t hdr; unsigned char b[%ld]; } L%d = {RT_HDR(RT_T_BYTEVECTOR, RT_F_RO, %ld), {", (long)n, k, (long)n);
            for (i = 0; i < n; i++) sb_printf(&lits, "%s%d", i ? ", " : "", RT_PTR(x, rt_bytevector)->b[i]);
            sb_puts(&lits, "}};\n");
        }
        r = fmt("((obj)((char *)&L%d + 5))", k);
    } else cerror(RT_FALSE, "internal error: cannot emit literal %s", dstr(x));
    pm_put(&litmap, x, (void *)r);
    return r;
}

/* ---- names and values ---- */
static const char *cvar(ir_var *v) {
    sbuf b = {0};
    const char *r;
    sb_puts(&b, "v_");
    c_mangle(&b, RT_STR(RT_PTR(v->name, rt_symbol)->name)->buf);
    sb_printf(&b, "_%d", v->id);
    r = arena_strdup(&compiler_arena, b.s);
    sb_free(&b);
    return r;
}
static const char *closure_name(ir_lambda *L) { return fmt("C%s", L->cname + 1); } /* P_x -> C_x */
static const char *cname_str(obj s) { return RT_STR(RT_PTR(s, rt_symbol)->name)->buf; }

/* ---- representations (M4-2): obj, a tagged fixnum in an intptr_t, an unboxed double, or a
 * struct of values (the result of a procedure returning several values to known receivers) ---- */
static int unsafe_;
static const char *ctype(int rep, int nvals) {
    if (rep == R_MV) return fmt("rt_mv%d", nvals);
    return rep == R_FIX ? "intptr_t" : rep == R_DBL ? "double" : "obj";
}
static int fn_rep(ir_lambda *L) { return L->direct ? L->rrep : R_OBJ; } /* what its C function returns */
static int free_here(ir_var *v) { /* read from the closure (or self): an obj */
    ir_lambda *me = cur->dispatch ? cur->dispatch : cur;
    int i;
    if (v == me->self && !(v->flags & V_ASSIGNED)) return 1;
    for (i = 0; i < cur->nfree; i++) if (cur->free[i] == v) return 1;
    return 0;
}
static int var_rep(ir_var *v) { return (v->flags & V_GLOBAL) || free_here(v) ? R_OBJ : v->rep; }
static const char *conv(const char *e, int from, int to) {
    if (from == to) return e;
    if (to == R_OBJ) return from == R_FIX ? fmt("(obj)(%s)", e) : fmt("rt_make_flonum(%s)", e);
    if (from == R_OBJ) return to == R_FIX ? fmt("(intptr_t)(%s)", e) : fmt("RT_FLO(%s)", e);
    return to == R_DBL ? fmt("(double)RT_FIXVAL(%s)", e) : fmt("(intptr_t)rt_make_flonum(%s)", e); /* dead code only */
}
static const char *dbl_lit(double d) {
    if (d != d) return "G_NAN";
    if (d - d != 0) return d < 0 ? "(-G_INF)" : "G_INF";
    return fmt("%a", d);
}

/* The value of variable v inside the function being emitted (in its own representation). */
static const char *val(ir_var *v) {
    ir_lambda *me = cur->dispatch ? cur->dispatch : cur;
    int i;
    if (v->flags & V_GLOBAL) return v->known ? fmt("RT_SPROC(%s)", closure_name(v->known)) : fmt("rt_g[%d]", v->slot);
    if (v == me->self && !(v->flags & V_ASSIGNED)) return "self";
    for (i = 0; i < cur->nfree; i++) if (cur->free[i] == v) return fmt("RT_PROC(self)->free[%d]", i);
    return cvar(v);
}
static const char *quoted_name(ir_var *v) {
    sbuf b = {0};
    const char *r;
    obj s = v->g ? v->g->name : v->name;
    c_string(&b, cname_str(s), strlen(cname_str(s)));
    r = arena_strdup(&compiler_arena, b.s);
    sb_free(&b);
    return r;
}
static int atom_rep(ir_node *n) { return n->kind == IR_CONST || n->u.ref.check ? R_OBJ : var_rep(n->u.ref.v); }
static const char *atom(ir_node *n) {
    if (n->kind == IR_CONST) return lit(n->u.value);
    if (n->u.ref.check && !unsafe_) return fmt("rt_chkdef(%s, %s)", val(n->u.ref.v), quoted_name(n->u.ref.v));
    return val(n->u.ref.v);
}
/* An atom in representation rep. */
static const char *atom_as(ir_node *n, int rep) {
    if (n->kind == IR_CONST) {
        obj x = n->u.value;
        if (rep == R_DBL && rt_is_flo(x)) return dbl_lit(RT_FLO(x));
        if (rep == R_DBL && rt_is_fix(x)) return dbl_lit((double)RT_FIXVAL(x));
        if (rep == R_FIX && rt_is_fix(x)) return fmt("(intptr_t)%s", lit(x));
    }
    return conv(atom(n), atom_rep(n), rep);
}
/* A proven number as a double (spec §6.2: unboxed). */
static int only(int t, int m) { return t && !(t & ~m); }
static const char *as_dbl(ir_node *n) {
    int t = ir_atom_type(n);
    if (n->kind == IR_CONST || only(t, T_FLO)) return atom_as(n, R_DBL);
    if (only(t, T_FIX)) return fmt("(double)RT_FIXVAL(%s)", atom_as(n, R_FIX));
    return fmt("rt_num_dbl(%s)", atom_as(n, R_OBJ));
}
static int used(ir_var *v) { return v && v->nrefs > 0; }
static void assign(const char *dest, int drep, const char *e, int erep, int ind, int pure) {
    if (dest) out(ind, "%s = %s;", dest, conv(e, erep, drep));
    else if (!pure) out(ind, "(void)(%s);", e);
}

/* ---- calls ---- */
/* Argument registers for a call: stores of arguments beyond a3, then "a0, a1, a2, a3". */
static const char *call_args(ir_node **args, int n, int ind) {
    sbuf b = {0};
    const char *r;
    int i;
    if (n - 4 > RT_ARGBUF) out(ind, "rt_args_reserve(%d);", n - 4);
    for (i = 4; i < n; i++) out(ind, "rt_args[%d] = %s;", i - 4, atom_as(args[i], R_OBJ));
    for (i = 0; i < 4; i++) sb_printf(&b, ", %s", i < n ? atom_as(args[i], R_OBJ) : "0");
    r = arena_strdup(&compiler_arena, b.s);
    sb_free(&b);
    return r;
}
/* Emit "dest = <call>;", "(void)<call>;" or a tail call (uniform callee: obj result). A direct
 * procedure that returns an unboxed value or values never trampolines: its tail calls to uniform
 * procedures are plain calls (it is on no tail-call cycle, M4-1). */
static const char *nt(const char *call) { return fmt("RT_NT(%s)", call); }
static void put_call(ir_node *c, const char *dest, int drep, int ind, const char *fn, const char *self, const char *argc, const char *args) {
    const char *call = fmt("%s(%s, %s%s)", fn, self, argc, args);
    if (!c->u.call.tail) assign(dest, drep, nt(call), R_OBJ, ind, 0);
    else if (!cur->direct) out(ind, "RT_TAIL(%s, %s, %s%s);", fn, self, argc, args);
    else if (cur->rrep == R_OBJ) out(ind, "RT_DTAIL(%s, %s, %s%s);", fn, self, argc, args);
    else out(ind, "return %s;", conv(nt(call), R_OBJ, cur->rrep));
}
static void emit_call(ir_node *c, const char *dest, int drep, int ind) {
    if (c->u.call.ctl == CTL_CALLCC) { /* tiers 2 and 3: the runtime keeps the frame (not a tail call) */
        const char *e = fmt(c->spec & N_TIER3 ? "rt_call_cc3(%s)" : "rt_call_cc(%s)", atom_as(c->u.call.args[0], R_OBJ));
        if (c->u.call.tail) out(ind, "return %s;", conv(e, R_OBJ, fn_rep(cur)));
        else assign(dest, drep, e, R_OBJ, ind, 0);
        return;
    }
    if (c->u.call.ctl != CTL_NONE) {
        const char *f = atom_as(c->u.call.args[0], R_OBJ), *x = atom_as(c->u.call.args[1], R_OBJ);
        out(ind, "{");
        out(ind + 1, "obj r_[4], f_;");
        out(ind + 1, "intptr_t n_;");
        if (c->u.call.ctl == CTL_APPLY) {
            out(ind + 1, "n_ = rt_spread(%s, r_);", x);
            out(ind + 1, "f_ = %s;", f);
        } else { /* call-with-values: call the producer, spread its values into the consumer */
            out(ind + 1, "f_ = %s;", f);
            out(ind + 1, "RT_CALLABLE(f_);");
            out(ind + 1, "n_ = rt_spread_values(RT_NT(RT_PROC(f_)->code(f_, 0, 0, 0, 0, 0)), r_);");
            out(ind + 1, "f_ = %s;", x);
        }
        out(ind + 1, "RT_CALLABLE(f_);");
        put_call(c, dest, drep, ind + 1, "RT_PROC(f_)->code", "f_", "n_", ", r_[0], r_[1], r_[2], r_[3]");
        out(ind, "}");
        return;
    }
    if (c->u.call.known && c->u.call.known->direct) { /* direct (private) signature: arguments as they are */
        ir_lambda *D = c->u.call.known;
        sbuf b = {0};
        int i;
        const char *call;
        for (i = 0; i < c->u.call.n; i++) sb_printf(&b, "%s%s", i ? ", " : "", atom_as(c->u.call.args[i], D->params[i]->rep));
        call = fmt("%s(%s)", D->cname, b.s ? b.s : "");
        sb_free(&b);
        if (D->rrep == R_OBJ) call = nt(call);
        if (c->u.call.tail) out(ind, "return %s;", conv(call, D->rrep, fn_rep(cur)));
        else if (D->rrep == R_MV) out(ind, dest ? "%s = %s;" : "%s(void)%s;", dest ? dest : "", call);
        else assign(dest, drep, call, D->rrep, ind, 0);
        return;
    }
    if (c->u.call.known) {
        const char *self = ir_call_self(c) == 1 ? atom_as(c->u.call.fn, R_OBJ) : "RT_FALSE", *args = call_args(c->u.call.args, c->u.call.n, ind);
        put_call(c, dest, drep, ind, c->u.call.known->cname, self, fmt("%d", c->u.call.n), args);
    } else {
        const char *f = atom_as(c->u.call.fn, R_OBJ), *args;
        if (c->u.call.fn->kind == IR_CONST || (c->u.call.fn->u.ref.check && !unsafe_)) { /* evaluate once */
            out(ind, "{");
            out(ind + 1, "obj f_ = %s;", f);
            out(ind + 1, "RT_CALLABLE(f_);");
            args = call_args(c->u.call.args, c->u.call.n, ind + 1);
            put_call(c, dest, drep, ind + 1, "RT_PROC(f_)->code", "f_", fmt("%d", c->u.call.n), args);
            out(ind, "}");
            return;
        }
        out(ind, "RT_CALLABLE(%s);", f);
        args = call_args(c->u.call.args, c->u.call.n, ind);
        put_call(c, dest, drep, ind, fmt("RT_PROC(%s)->code", f), f, fmt("%d", c->u.call.n), args);
    }
}

static void emit_closure(ir_lambda *M, const char *dest, int ind, int fill) {
    int i;
    if (M->is_static) {
        if (!fill) out(ind, "%s = RT_SPROC(%s);", dest, closure_name(M));
        return;
    }
    if (!fill) out(ind, "%s = rt_make_proc(%s, %d);", dest, M->cname, M->nfree);
    else for (i = 0; i < M->nfree; i++) out(ind, "RT_PROC(%s)->free[%d] = %s;", dest, i, conv(val(M->free[i]), var_rep(M->free[i]), R_OBJ));
}

/* ---- primitives: the checked runtime function, or a specialised form when the analysis proved
 * the types (S_OK flags, refined atom types): tagged fixnum arithmetic, double arithmetic on
 * unboxed operands, accesses without type checks, the writers a proven type needs. ---- */
#define EQABLE (T_FIX | T_CHAR | T_BOOL | T_NULL | T_EOF | T_UNSPEC | T_SYMBOL | T_PROC | T_PORTS | T_RECORD | T_RTD | T_OTHER | T_UNDEF)
static int small_fix(ir_node *n) { /* a fixnum constant a double represents exactly */
    return n->kind == IR_CONST && rt_is_fix(n->u.value) && RT_FIXVAL(n->u.value) <= ((intptr_t)1 << 53) && RT_FIXVAL(n->u.value) >= -((intptr_t)1 << 53);
}
static const char *prim_call(ir_node *r) {
    const prim_info *p = &prim_table[r->u.prim.op];
    sbuf b = {0};
    const char *s;
    int i;
    sb_printf(&b, "%s(", p->cfn);
    for (i = 0; i < r->u.prim.n; i++) sb_printf(&b, "%s%s", i ? ", " : "", atom_as(r->u.prim.args[i], R_OBJ));
    sb_putc(&b, ')');
    s = arena_strdup(&compiler_arena, b.s);
    sb_free(&b);
    return s;
}
static int std_port(ir_node *p) { return only(ir_atom_type(p), T_STDOUT | T_STDERR); } /* open: prog_->std_open */
static ir_program *prog_;
/* the specialised expression and its representation, or NULL (generic) */
static const char *special(ir_node *r, int *erep) {
    ir_node **a = r->u.prim.args;
    int op = r->u.prim.op, sp = r->spec, n = r->u.prim.n;
    int t0 = n > 0 ? ir_atom_type(a[0]) : 0, t1 = n > 1 ? ir_atom_type(a[1]) : 0;
    int ff = only(t0, T_FIX) && only(t1, T_FIX), fl = only(t0, T_NUM) && only(t1, T_NUM) && ((t0 | t1) & T_FLO) && (only(t0, T_FLO) || only(t1, T_FLO));
    int fcmp = (only(t0, T_FLO) && only(t1, T_FLO)) || (only(t0, T_FLO) && small_fix(a[1 < n ? 1 : 0])) || (only(t1, T_FLO) && small_fix(a[0]));
    static const char *const ops[] = {"+", "-", "*", "/", "<", ">", "<=", ">=", "=="};
    int k = op == PI_rt_add ? 0 : op == PI_rt_sub ? 1 : op == PI_rt_mul ? 2 : op == PI_rt_div ? 3 : op == PI_rt_lt ? 4 : op == PI_rt_gt ? 5
          : op == PI_rt_num_le ? 6 : op == PI_rt_num_ge ? 7 : op == PI_rt_num_eq ? 8 : -1;
    *erep = R_OBJ;
    if (k >= 0 && k <= 2 && ff) {
        *erep = R_FIX;
        if (sp & S_NOOVF) return k == 2 ? fmt("RT_FIXVAL(%s) * %s", atom_as(a[0], R_FIX), atom_as(a[1], R_FIX))
                                        : fmt("%s %s %s", atom_as(a[0], R_FIX), ops[k], atom_as(a[1], R_FIX));
        return fmt("rt_fx%s(%s, %s)", k == 0 ? "add" : k == 1 ? "sub" : "mul", atom_as(a[0], R_FIX), atom_as(a[1], R_FIX));
    }
    if (k >= 0 && k <= 3 && fl) { *erep = R_DBL; return fmt("%s %s %s", as_dbl(a[0]), ops[k], as_dbl(a[1])); }
    if (k >= 4 && ff) return fmt("RT_BOOL(%s %s %s)", atom_as(a[0], R_FIX), ops[k], atom_as(a[1], R_FIX));
    if (k >= 4 && fcmp) return fmt("RT_BOOL(%s %s %s)", as_dbl(a[0]), ops[k], as_dbl(a[1]));
    switch (op) {
    case PI_rt_quotient: case PI_rt_remainder: case PI_rt_modulo:
        if (!ff) return NULL;
        *erep = R_FIX;
        return fmt("rt_fx%s(%s, %s)", op == PI_rt_quotient ? "quo" : op == PI_rt_remainder ? "rem" : "mod", atom_as(a[0], R_FIX), atom_as(a[1], R_FIX));
    case PI_rt_neg: if (only(t0, T_FLO)) { *erep = R_DBL; return fmt("-%s", as_dbl(a[0])); } return NULL;
    case PI_rt_abs: if (only(t0, T_FLO)) { *erep = R_DBL; return fmt("rt_flabs(%s)", as_dbl(a[0])); } return NULL;
    case PI_rt_sqrt: case PI_rt_exp: case PI_rt_log: case PI_rt_sin: case PI_rt_cos: case PI_rt_tan: case PI_rt_asin: case PI_rt_acos:
        if (!only(t0, T_NUM)) return NULL;
        *erep = R_DBL;
        return fmt("rt_sys_%s(%s)", prim_table[op].who, as_dbl(a[0]));
    case PI_rt_atan: if (!only(t0, T_NUM)) return NULL; *erep = R_DBL; return fmt("rt_sys_atan2(%s, 1.0)", as_dbl(a[0]));
    case PI_rt_atan2: if (!only(t0, T_NUM) || !only(t1, T_NUM)) return NULL; *erep = R_DBL; return fmt("rt_sys_atan2(%s, %s)", as_dbl(a[0]), as_dbl(a[1]));
    case PI_rt_inexact: if (!only(t0, T_NUM)) return NULL; *erep = R_DBL; return as_dbl(a[0]);
    case PI_rt_exact: case PI_rt_floor: case PI_rt_ceiling: case PI_rt_round: case PI_rt_truncate:
        if (!only(t0, T_FIX)) return NULL;
        *erep = R_FIX;
        return atom_as(a[0], R_FIX);
    case PI_rt_exact_p: case PI_rt_inexact_p:
        return only(t0, T_NUM) ? fmt("RT_BOOL(%srt_is_fix(%s))", op == PI_rt_exact_p ? "" : "!", atom_as(a[0], R_OBJ)) : NULL;
    case PI_rt_car: case PI_rt_cdr:
        return sp & S_OK(0) ? fmt("RT_C%sR(%s)", op == PI_rt_car ? "A" : "D", atom_as(a[0], R_OBJ)) : NULL;
    case PI_rt_set_car: case PI_rt_set_cdr:
        return sp & S_OK(0) ? fmt("rt_set_c%sr_u(%s, %s)", op == PI_rt_set_car ? "a" : "d", atom_as(a[0], R_OBJ), atom_as(a[1], R_OBJ)) : NULL;
    case PI_rt_vector_ref: case PI_rt_string_ref: case PI_rt_bytevector_u8_ref:
        if ((sp & (S_OK(0) | S_OK(1))) != (S_OK(0) | S_OK(1))) return NULL;
        return fmt("%s_u(%s, %s)", prim_table[op].cfn, atom_as(a[0], R_OBJ), atom_as(a[1], R_OBJ));
    case PI_rt_vector_set:
        if ((sp & (S_OK(0) | S_OK(1))) != (S_OK(0) | S_OK(1))) return NULL;
        return fmt("rt_vector_set_u(%s, %s, %s)", atom_as(a[0], R_OBJ), atom_as(a[1], R_OBJ), atom_as(a[2], R_OBJ));
    case PI_rt_vector_length: case PI_rt_bytevector_length:
        if (!(sp & S_OK(0))) return NULL;
        *erep = R_FIX;
        return fmt("(intptr_t)RT_FIX(RT_HLEN(%s))", atom_as(a[0], R_OBJ));
    case PI_rt_string_length:
        if (!(sp & S_OK(0))) return NULL;
        *erep = R_FIX;
        return fmt("(intptr_t)RT_FIX(RT_STR(%s)->chars)", atom_as(a[0], R_OBJ));
    case PI_rt_char_to_integer:
        if (!(sp & S_OK(0))) return NULL;
        *erep = R_FIX;
        return fmt("(intptr_t)RT_FIX(RT_CHARVAL(%s))", atom_as(a[0], R_OBJ));
    case PI_rt_record_ref:
        return sp & S_OK(0) ? fmt("RT_VEC(%s)[%ld]", atom_as(a[0], R_OBJ), (long)RT_FIXVAL(a[2]->u.value) + 1) : NULL;
    case PI_rt_record_set:
        return sp & S_OK(0) ? fmt("(RT_VEC(%s)[%ld] = %s, RT_UNSPEC)", atom_as(a[0], R_OBJ), (long)RT_FIXVAL(a[2]->u.value) + 1, atom_as(a[3], R_OBJ)) : NULL;
    case PI_rt_box_ref: return fmt("RT_VEC(%s)[0]", atom_as(a[0], R_OBJ)); /* boxes are the compiler's own */
    case PI_rt_box_set: return fmt("(RT_VEC(%s)[0] = %s, RT_UNSPEC)", atom_as(a[0], R_OBJ), atom_as(a[1], R_OBJ));
    case PI_rt_eqv_p: case PI_rt_equal_p:
        if (op == PI_rt_eqv_p ? !(t0 & T_FLO) || !(t1 & T_FLO) : only(t0, EQABLE) || only(t1, EQABLE))
            return fmt("RT_BOOL(%s == %s)", atom_as(a[0], R_OBJ), atom_as(a[1], R_OBJ));
        return NULL;
    case PI_rt_display: case PI_rt_write: case PI_rt_write_char: case PI_rt_write_string: {
        const char *x = atom_as(a[0], R_OBJ), *p = atom_as(a[1], R_OBJ);
        int sp_ = prog_->std_open && std_port(a[1]);
        if (sp_ && op != PI_rt_write && only(t0, T_STRING)) return fmt("rt_put_string(%s, %s)", x, p);
        if (sp_ && op != PI_rt_write && only(t0, T_CHAR)) return fmt("rt_put_char(%s, %s)", x, p);
        if (sp_ && only(t0, T_FIX) && op != PI_rt_write_char && op != PI_rt_write_string) return fmt("rt_put_fixnum(%s, %s)", x, p);
        if ((op == PI_rt_display || op == PI_rt_write) && t0 && !(t0 & (T_PAIR | T_VECTOR | T_PCELL)))
            return fmt("rt_%s_flat(%s, %s)", op == PI_rt_display ? "display" : "write", x, p);
        return NULL;
    }
    case PI_rt_newline:
        return prog_->std_open && std_port(a[0]) ? fmt("rt_put_newline(%s)", atom_as(a[0], R_OBJ)) : NULL;
    default: return NULL;
    }
}
static void emit_prim(ir_node *r, const char *dest, int drep, int ind) {
    const prim_info *p = &prim_table[r->u.prim.op];
    int i, erep;
    const char *e;
    if (r->u.prim.op == PI_VALUES) {
        sbuf b = {0};
        if (drep == R_MV) return; /* returned in a struct (M4-2): no heap object */
        if (r->u.prim.n == 1) { assign(dest, drep, atom_as(r->u.prim.args[0], R_OBJ), R_OBJ, ind, 1); return; } /* one value is itself */
        if (r->u.prim.n == 0) sb_puts(&b, "rt_values_n(0, 0)");
        else {
            sb_printf(&b, "rt_values_n(%d, (obj[]){", r->u.prim.n);
            for (i = 0; i < r->u.prim.n; i++) sb_printf(&b, "%s%s", i ? ", " : "", atom_as(r->u.prim.args[i], R_OBJ));
            sb_puts(&b, "})");
        }
        assign(dest, drep, b.s, R_OBJ, ind, 1);
        sb_free(&b);
        return;
    }
    if ((r->u.prim.op == PI_rt_values_check || r->u.prim.op == PI_rt_values_ref) && r->u.prim.args[0]->kind == IR_REF &&
        r->u.prim.args[0]->u.ref.v->rep == R_MV) { /* spreading a values struct */
        if (r->u.prim.op == PI_rt_values_ref) assign(dest, drep, fmt("%s.v[%ld]", cvar(r->u.prim.args[0]->u.ref.v), (long)RT_FIXVAL(r->u.prim.args[1]->u.value)), R_OBJ, ind, 1);
        else if (dest) out(ind, "%s = RT_UNSPEC;", dest);
        return;
    }
    if (!dest && (p->flags & PF_PURE)) return;
    if (!(e = special(r, &erep))) { e = prim_call(r); erep = R_OBJ; }
    assign(dest, drep, e, erep, ind, (p->flags & PF_PURE) != 0);
}

static int stack_closure(ir_node *n);
/* Emit an rhs into dest, a variable of representation drep (dest NULL: for effect). */
static void emit_rhs(ir_node *r, const char *dest, int drep, int ind) {
    switch (r->kind) {
    case IR_CONST: case IR_REF:
        if (dest) out(ind, "%s = %s;", dest, atom_as(r, drep));
        else if (r->kind == IR_REF && r->u.ref.check && !unsafe_) out(ind, "(void)%s;", atom(r));
        return;
    case IR_PRIM: emit_prim(r, dest, drep, ind); return;
    case IR_CALL: emit_call(r, dest, drep, ind); return;
    case IR_LAMBDA:
        if (dest) { emit_closure(r->u.lam, dest, ind, 0); emit_closure(r->u.lam, dest, ind, 1); }
        return;
    case IR_SET: {
        ir_var *v = r->u.set.v;
        if (v->flags & V_GLOBAL) out(ind, "rt_g[%d] = %s;", v->slot, atom_as(r->u.set.val, R_OBJ));
        else if (used(v)) out(ind, "%s = %s;", val(v), atom_as(r->u.set.val, var_rep(v)));
        if (dest) out(ind, "%s = %s;", dest, conv("RT_UNSPEC", R_OBJ, drep));
        return;
    }
    default: cerror(RT_FALSE, "internal error: bad rhs kind %d", r->kind);
    }
}

/* A jump assigns the join's parameters in parallel (through temporaries when an argument
 * reads another parameter of the same join: loops), then jumps. */
static void emit_jump(ir_join *j, ir_node **args, int ind, int fall) {
    int i, k, par = 0;
    for (i = 0; i < j->n; i++)
        if (used(j->params[i]))
            for (k = 0; k < j->n; k++)
                if (k != i && used(j->params[k]) && args[k]->kind == IR_REF && args[k]->u.ref.v == j->params[i]) par = 1;
    if (par) {
        out(ind, "{");
        for (i = 0; i < j->n; i++)
            if (used(j->params[i])) out(ind + 1, "%s t%d_ = %s;", ctype(j->params[i]->rep, 0), i, atom_as(args[i], j->params[i]->rep));
        for (i = 0; i < j->n; i++) if (used(j->params[i])) out(ind + 1, "%s = t%d_;", cvar(j->params[i]), i);
    } else
        for (i = 0; i < j->n; i++)
            if (used(j->params[i]) && !(args[i]->kind == IR_REF && args[i]->u.ref.v == j->params[i]))
                out(ind, "%s = %s;", cvar(j->params[i]), atom_as(args[i], j->params[i]->rep));
    if (fall) {} /* the label follows */
    else if (j == cur->entry) out(ind + par, "goto entry;");
    else out(ind + par, "goto j%d;", j->id);
    if (par) out(ind, "}");
}

static void emit_ret(ir_node *a, int ind) {
    int rr = fn_rep(cur), i;
    if (rr == R_MV && a->kind == IR_REF && a->u.ref.v->def) { /* (values x ...) into the result struct */
        ir_node *d = a->u.ref.v->def;
        sbuf b = {0};
        for (i = 0; i < d->u.prim.n; i++) sb_printf(&b, "%s%s", i ? ", " : "", atom_as(d->u.prim.args[i], R_OBJ));
        out(ind, "{ rt_mv%d r_ = {{%s}}; return r_; }", cur->nvals, b.s);
        sb_free(&b);
        return;
    }
    if (rr == R_MV) { out(ind, "return %s;", cvar(a->u.ref.v)); return; }
    out(ind, "return %s;", atom_as(a, rr));
}

static void emit_term(ir_node *n, int ind) {
    int i;
    for (; n; ) {
        switch (n->kind) {
        case IR_LET:
            if (stack_closure(n)) {
                ir_lambda *M = n->u.let.rhs->u.lam;
                int id = n->u.let.v->id;
                out(ind, "sc_%d.hdr = RT_HDR(RT_T_PROC, 0, %d);", id, M->nfree);
                out(ind, "sc_%d.code = %s;", id, M->cname);
                for (i = 0; i < M->nfree; i++) out(ind, "sc_%d.free[%d] = %s;", id, i, conv(val(M->free[i]), var_rep(M->free[i]), R_OBJ));
                out(ind, "%s = (obj)((char *)&sc_%d.code + 3);", cvar(n->u.let.v), id);
                n = n->u.let.body;
                continue;
            }
            if (n->u.let.v && n->u.let.v->rep == R_MV && n->u.let.rhs->kind == IR_CALL) emit_call(n->u.let.rhs, cvar(n->u.let.v), R_MV, ind);
            else emit_rhs(n->u.let.rhs, used(n->u.let.v) ? cvar(n->u.let.v) : NULL, n->u.let.v ? n->u.let.v->rep : R_OBJ, ind);
            n = n->u.let.body;
            continue;
        case IR_FIX:
            for (i = 0; i < n->u.fix.n; i++)
                if (used(n->u.fix.vars[i])) emit_closure(n->u.fix.lams[i], cvar(n->u.fix.vars[i]), ind, 0);
            for (i = 0; i < n->u.fix.n; i++)
                if (used(n->u.fix.vars[i])) emit_closure(n->u.fix.lams[i], cvar(n->u.fix.vars[i]), ind, 1);
            n = n->u.fix.body;
            continue;
        case IR_LETJ: {
            ir_join *j;
            ir_node *b = n->u.letj.body;
            int fell = b->kind == IR_JUMP && b->u.jump.j == n->u.letj.j && b->u.jump.j != cur->entry;
            if (fell) emit_jump(b->u.jump.j, b->u.jump.args, ind, 1); /* falls into its label */
            else emit_term(b, ind);
            for (j = n->u.letj.j; j; j = j->next) {
                if (j->njumps > (fell && j == n->u.letj.j)) out(ind - 1, "j%d:;", j->id);
                if (j->next) emit_term(j->body, ind);
                else n = j->body;
            }
            continue;
        }
        case IR_IF:
            out(ind, "if (%s != RT_FALSE) {", atom_as(n->u.if_.test, R_OBJ));
            emit_term(n->u.if_.then, ind + 1);
            out(ind, "} else {");
            emit_term(n->u.if_.els, ind + 1);
            out(ind, "}");
            return;
        case IR_JUMP: emit_jump(n->u.jump.j, n->u.jump.args, ind, 0); return;
        case IR_RET: emit_ret(n->u.ret, ind); return;
        case IR_CALL: emit_call(n, NULL, R_OBJ, ind); return;
        default: cerror(RT_FALSE, "internal error: bad term kind %d", n->kind);
        }
    }
}

/* Variables a function must declare: its parameters and every binder in its body. */
static void decl_var(ir_var *v) {
    if (!used(v) || (v->rep == R_MV && v->def && v->def->kind == IR_PRIM)) return; /* (values ...) into a struct: no variable */
    out(1, "%s %s%s;", ctype(v->rep, v->slot), cvar(v), v->rep == R_MV ? "" : " = 0");
}
/* LET r = closure, used only as the receiver of the %call/cc right after it: rt_call_cc calls
 * it while this frame is live and nothing else can reach it, so it lives on the C stack
 * (no allocation per tier-2 capture; the conservative scan sees its free variables). */
static int stack_closure(ir_node *n) {
    ir_node *r = n->u.let.rhs, *c = n->u.let.body;
    ir_var *v = n->u.let.v;
    if (r->kind != IR_LAMBDA || r->u.lam->is_static || !v || v->nrefs != 1 || (v->flags & V_ASSIGNED)) return 0;
    if (c->kind == IR_LET) c = c->u.let.rhs;
    return c->kind == IR_CALL && c->u.call.ctl == CTL_CALLCC && c->u.call.args[0]->kind == IR_REF && c->u.call.args[0]->u.ref.v == v;
}
static void decl_term(ir_node *n) {
    int i;
    for (; n; ) {
        switch (n->kind) {
        case IR_LET:
            decl_var(n->u.let.v);
            if (stack_closure(n))
                out(1, "struct { uintptr_t hdr; rt_code code; obj free[%d]; } sc_%d;", n->u.let.rhs->u.lam->nfree, n->u.let.v->id);
            n = n->u.let.body;
            continue;
        case IR_FIX: for (i = 0; i < n->u.fix.n; i++) decl_var(n->u.fix.vars[i]); n = n->u.fix.body; continue;
        case IR_LETJ: {
            ir_join *j;
            for (j = n->u.letj.j; j; j = j->next) {
                for (i = 0; i < j->n; i++) decl_var(j->params[i]);
                if (j->next) decl_term(j->body);
            }
            decl_term(n->u.letj.body);
            for (j = n->u.letj.j; j->next; j = j->next) {}
            n = j->body;
            continue;
        }
        case IR_IF: decl_term(n->u.if_.then); n = n->u.if_.els; continue;
        default: return;
        }
    }
}

static const char *proc_comment(ir_lambda *L) {
    obj name = L->name;
    const char *lib = L->lib ? L->lib->id : "main";
    return fmt("/* %s: %s%s */", lib, cname_str(name), L->dispatch ? " (case-lambda clause)" : "");
}

/* "obj v_x_1, obj v_y_2" (names) or "obj, obj" (prototype); "void" without parameters. */
static const char *direct_params(ir_lambda *L, int names) {
    sbuf b = {0};
    const char *r;
    int i;
    for (i = 0; i < L->nparams; i++)
        sb_printf(&b, "%s%s%s%s", i ? ", " : "", ctype(L->params[i]->rep, 0), names ? " " : "", names ? cvar(L->params[i]) : "");
    r = arena_strdup(&compiler_arena, L->nparams ? b.s : "void");
    sb_free(&b);
    return r;
}
static void emit_function(ir_lambda *L, ir_program *p) {
    int i;
    cur = L;
    out(0, "%s", proc_comment(L));
    if (L->nclauses) { /* case-lambda dispatch on argc */
        out(0, "static obj %s(obj self, intptr_t argc, obj a0, obj a1, obj a2, obj a3) {", L->cname);
        for (i = 0; i < L->nclauses; i++) {
            ir_lambda *c = L->clauses[i];
            if (c->rest) out(1, "if (argc >= %d) RT_MUSTTAIL return %s(self, argc, a0, a1, a2, a3);", c->nreq, c->cname);
            else out(1, "if (argc == %d) RT_MUSTTAIL return %s(self, argc, a0, a1, a2, a3);", c->nreq, c->cname);
        }
        out(1, "rt_arity_error(\"%s\", argc);", cname_str(L->name));
        out(0, "}");
        out(0, "%s", "");
        return;
    }
    if (L->direct) { /* obj f(params...): the parameters are the C parameters */
        out(0, "static %s %s(%s) {", ctype(L->rrep, L->nvals), L->cname, direct_params(L, 1));
        for (i = 0; i < L->nparams; i++) {
            ir_var *v = L->params[i];
            if (!used(v)) out(1, "(void)%s;", cvar(v));
            v->nrefs++; /* declared: never again by decl_var */
        }
        decl_term(L->body);
        for (i = 0; i < L->nparams; i++) L->params[i]->nrefs--;
        if (L->entry && L->entry->njumps) out(0, "entry:;");
        emit_term(L->body, 1);
        out(0, "}");
        out(0, "%s", "");
        return;
    }
    out(0, "static obj %s(obj self, intptr_t argc, obj a0, obj a1, obj a2, obj a3) {", L->cname);
    for (i = 0; i < L->nparams; i++) decl_var(L->params[i]);
    decl_term(L->body);
    out(1, "(void)self; (void)argc; (void)a0; (void)a1; (void)a2; (void)a3;");
    if (L != p->main && !L->dispatch && L->escapes && !unsafe_) { /* unknown callers: check the count */
        sbuf b = {0};
        c_string(&b, cname_str(L->name), strlen(cname_str(L->name)));
        if (L->rest) out(1, "if (RT_UNLIKELY(argc < %d)) rt_arity_error(%s, argc);", L->nreq, b.s);
        else out(1, "if (RT_UNLIKELY(argc != %d)) rt_arity_error(%s, argc);", L->nreq, b.s);
        sb_free(&b);
    }
    for (i = 0; i < L->nreq; i++)
        if (used(L->params[i])) {
            if (i < 4) out(1, "%s = %s;", cvar(L->params[i]), conv(fmt("a%d", i), R_OBJ, L->params[i]->rep));
            else out(1, "%s = %s;", cvar(L->params[i]), conv(fmt("rt_args[%d]", i - 4), R_OBJ, L->params[i]->rep));
        }
    if (L->rest && used(L->params[L->nreq]))
        out(1, "%s = rt_rest_list(argc, %d, a0, a1, a2, a3);", cvar(L->params[L->nreq]), L->nreq);
    if (L->entry && L->entry->njumps) out(0, "entry:;");
    emit_term(L->body, 1);
    out(0, "}");
    out(0, "%s", "");
}

static void append_embedded(sbuf *o, const char *name) {
    size_t n;
    char *t = embed_text(name, &n);
    if (!t) die("internal error: runtime source %s is not embedded", name);
    sb_printf(o, "\n/* ==== %s ==== */\n", name);
    sb_putn(o, t, n);
    free(t);
}

void emit_c(ir_program *p, const emit_opts *o, sbuf *o_) {
    static const char *const rtfiles[] = {"src/rt_sys.c", "src/rt_gc.c", "src/rt_core.c", "src/rt_str.c",
                                          "src/rt_num.c", "src/rt_io.c", "src/rt_read.c", 0};
    ir_lambda *L;
    int i;
    unsigned mvs = 0;
    sbuf protos = {0}, closures = {0};
    for (i = 0; i < p->nglobals; i++)
        if (!p->globals[i]->known) p->globals[i]->slot = nglob++;
    unsafe_ = o->unsafe;
    prog_ = p;
    for (L = p->lambdas; L; L = L->next)
        if (L->direct && L->rrep == R_MV && !(mvs & (1u << L->nvals))) {
            mvs |= 1u << L->nvals;
            sb_printf(&protos, "typedef struct { obj v[%d]; } rt_mv%d; /* values returned to known receivers */\n", L->nvals, L->nvals);
        }
    for (L = p->lambdas; L; L = L->next) {
        if (L->direct) sb_printf(&protos, "static RT_UNUSED %s %s(%s);\n", ctype(L->rrep, L->nvals), L->cname, direct_params(L, 0));
        else sb_printf(&protos, "static RT_UNUSED obj %s(obj, intptr_t, obj, obj, obj, obj);\n", L->cname);
        if (L->is_static && L != p->main && !L->dispatch && !L->direct)
            sb_printf(&closures, "static RT_UNUSED const rt_proc %s = {RT_HDR(RT_T_PROC, 0, 0), %s};\n", closure_name(L), L->cname);
    }
    for (L = p->lambdas; L; L = L->next) emit_function(L, p);

    sb_puts(o_, "/* Generated by shikra (DESIGN.md §3): config, rt.h, program, runtime. */\n");
    sb_puts(o_, "#define SHIKRA_PROGRAM 1\n");
    if (o->unsafe) sb_puts(o_, "#define RT_UNSAFE 1\n");
    if (o->debug) sb_puts(o_, "#define RT_DEBUG 1\n");
    if (o->gc_bump) sb_puts(o_, "#define RT_GC_BUMP 1\n");
    if (o->unicode_full) sb_puts(o_, "#define RT_UNICODE_FULL 1\n");
    if (o->gc_stress) sb_puts(o_, "#define RT_GC_STRESS 1\n");
    if (o->no_musttail) sb_puts(o_, "#define SHIKRA_NO_MUSTTAIL 1\n");
    if (p->no_handlers) sb_puts(o_, "#define RT_NO_HANDLERS 1 /* no exception handler is ever installed */\n");
    if (p->no_flonums) sb_puts(o_, "#define RT_NO_FLONUMS 1 /* no flonum can exist: no float printer */\n");
    if (p->tier3) sb_printf(o_, "#define RT_TIER3 1 /* %d call/cc site(s) of tier 3: stack copying */\n", p->tier3);
    sb_printf(o_, "#define RT_NLITPAIRS %d\n#define RT_NSTATIC_SYMS %d\n", npairs, nsyms);
    append_embedded(o_, "src/rt.h");
    sb_puts(o_, "\n/* ==== program ==== */\n");
    sb_puts(o_, "#if defined(__GNUC__) || defined(__clang__)\n#define G_INF __builtin_inf()\n#define G_NAN __builtin_nan(\"\")\n"
                "#else\n#define G_INF (1e308 * 10.0)\n#define G_NAN (G_INF - G_INF)\n#endif\n");
    if (lits.len) sb_putn(o_, lits.s, lits.len);
    if (npairs) sb_printf(o_, "static const rt_pair rt_lit_pairs[%d] = {\n    %s};\n", npairs, pairs.s);
    sb_printf(o_, "static obj rt_g[%d]; /* globals: a GC root range */\n", nglob + 1);
    if (nsyms) /* entered into the symbol table only if the program creates it (spec §9.1) */
        sb_printf(o_, "static const obj rt_static_syms[%d] = {%s};\n", nsyms, symtab.s);
    sb_putn(o_, protos.s, protos.len);
    if (closures.len) sb_putn(o_, closures.s, closures.len);
    sb_putc(o_, '\n');
    sb_putn(o_, code.s, code.len);
    sb_puts(o_, "static void rt_program_main(void) {\n    int i;\n");
    sb_printf(o_, "    for (i = 0; i < %d; i++) rt_g[i] = RT_UNDEF;\n", nglob + 1);
    sb_printf(o_, "    rt_gc_add_roots(rt_g, rt_g + %d);\n", nglob + 1);
    sb_printf(o_, "    (void)RT_NT(%s(RT_FALSE, 0, 0, 0, 0, 0));\n}\n", p->main->cname);
    if (p->raise && !p->no_handlers) /* runtime errors detected by primitives are raised by (scheme base) raise */
        sb_printf(o_, "static void rt_engine_raise(obj e) {\n    (void)RT_NT(%s(RT_SPROC(%s), 1, e, 0, 0, 0));\n    rt_uncaught(e);\n}\n",
                  p->raise->cname, closure_name(p->raise));
    else sb_puts(o_, "static void rt_engine_raise(obj e) { rt_uncaught(e); }\n");
    if (p->travel) /* tier-2 continuations walk the winders with (shikra dynamic) travel-to! */
        sb_printf(o_, "static void rt_engine_travel(obj w) {\n    (void)RT_NT(%s(RT_SPROC(%s), 1, w, 0, 0, 0));\n}\n",
                  p->travel->cname, closure_name(p->travel));
    else sb_puts(o_, "static void rt_engine_travel(obj w) { rt_winders = w; }\n");
    for (i = 0; rtfiles[i]; i++) append_embedded(o_, rtfiles[i]);
    if (o->unicode_full) append_embedded(o_, "src/rt_unicode.c"); /* spec §9.1: only with --unicode=full */
    sb_free(&protos);
    sb_free(&closures);
}
