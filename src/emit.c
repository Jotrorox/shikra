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

/* The value of variable v inside the function being emitted. */
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
static const char *atom(ir_node *n) {
    if (n->kind == IR_CONST) return lit(n->u.value);
    if (n->u.ref.check) return fmt("rt_chkdef(%s, %s)", val(n->u.ref.v), quoted_name(n->u.ref.v));
    return val(n->u.ref.v);
}
static int used(ir_var *v) { return v && v->nrefs > 0; }

/* ---- calls ---- */
/* Argument registers for a call: stores of arguments beyond a3, then "a0, a1, a2, a3". */
static const char *call_args(ir_node **args, int n, int ind) {
    sbuf b = {0};
    const char *r;
    int i;
    if (n - 4 > RT_ARGBUF) out(ind, "rt_args_reserve(%d);", n - 4);
    for (i = 4; i < n; i++) out(ind, "rt_args[%d] = %s;", i - 4, atom(args[i]));
    for (i = 0; i < 4; i++) sb_printf(&b, ", %s", i < n ? atom(args[i]) : "0");
    r = arena_strdup(&compiler_arena, b.s);
    sb_free(&b);
    return r;
}
/* Emit "dest = <call>;", "(void)<call>;" or a tail call. */
static const char *nt(const char *call) { return fmt("RT_NT(%s)", call); }
static void put_call(ir_node *c, const char *dest, int ind, const char *fn, const char *self, const char *argc, const char *args) {
    if (c->u.call.tail) out(ind, "%s(%s, %s, %s%s);", cur->direct ? "RT_DTAIL" : "RT_TAIL", fn, self, argc, args);
    else out(ind, "%s%s;", dest ? fmt("%s = ", dest) : "(void)", nt(fmt("%s(%s, %s%s)", fn, self, argc, args)));
}
static void emit_call(ir_node *c, const char *dest, int ind) {
    if (c->u.call.ctl == CTL_CALLCC) { /* tier 2: the runtime keeps the frame (not a tail call) */
        out(ind, "%srt_call_cc(%s);", c->u.call.tail ? "return " : dest ? fmt("%s = ", dest) : "(void)", atom(c->u.call.args[0]));
        return;
    }
    if (c->u.call.ctl != CTL_NONE) {
        const char *f = atom(c->u.call.args[0]), *x = atom(c->u.call.args[1]);
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
        put_call(c, dest, ind + 1, "RT_PROC(f_)->code", "f_", "n_", ", r_[0], r_[1], r_[2], r_[3]");
        out(ind, "}");
        return;
    }
    if (c->u.call.known && c->u.call.known->direct) { /* direct signature: arguments as they are */
        sbuf b = {0};
        int i;
        const char *call;
        for (i = 0; i < c->u.call.n; i++) sb_printf(&b, "%s%s", i ? ", " : "", atom(c->u.call.args[i]));
        call = fmt("%s(%s)", c->u.call.known->cname, b.s ? b.s : "");
        sb_free(&b);
        if (c->u.call.tail) out(ind, "return %s;", call);
        else out(ind, "%s%s;", dest ? fmt("%s = ", dest) : "(void)", nt(call));
        return;
    }
    if (c->u.call.known) {
        const char *self = ir_call_self(c) == 1 ? atom(c->u.call.fn) : "RT_FALSE", *args = call_args(c->u.call.args, c->u.call.n, ind);
        put_call(c, dest, ind, c->u.call.known->cname, self, fmt("%d", c->u.call.n), args);
    } else {
        const char *f = atom(c->u.call.fn), *args;
        if (c->u.call.fn->kind == IR_CONST || c->u.call.fn->u.ref.check) { /* evaluate once */
            out(ind, "{");
            out(ind + 1, "obj f_ = %s;", f);
            out(ind + 1, "RT_CALLABLE(f_);");
            args = call_args(c->u.call.args, c->u.call.n, ind + 1);
            put_call(c, dest, ind + 1, "RT_PROC(f_)->code", "f_", fmt("%d", c->u.call.n), args);
            out(ind, "}");
            return;
        }
        out(ind, "RT_CALLABLE(%s);", f);
        args = call_args(c->u.call.args, c->u.call.n, ind);
        put_call(c, dest, ind, fmt("RT_PROC(%s)->code", f), f, fmt("%d", c->u.call.n), args);
    }
}

static void emit_closure(ir_lambda *M, const char *dest, int ind, int fill) {
    int i;
    if (M->is_static) {
        if (!fill) out(ind, "%s = RT_SPROC(%s);", dest, closure_name(M));
        return;
    }
    if (!fill) out(ind, "%s = rt_make_proc(%s, %d);", dest, M->cname, M->nfree);
    else for (i = 0; i < M->nfree; i++) out(ind, "RT_PROC(%s)->free[%d] = %s;", dest, i, val(M->free[i]));
}

static int stack_closure(ir_node *n);
/* Emit an rhs into dest (NULL: for effect). */
static void emit_rhs(ir_node *r, const char *dest, int ind) {
    int i;
    switch (r->kind) {
    case IR_CONST: case IR_REF:
        if (dest) out(ind, "%s = %s;", dest, atom(r));
        else if (r->kind == IR_REF && r->u.ref.check) out(ind, "(void)%s;", atom(r));
        return;
    case IR_PRIM: {
        const prim_info *p = &prim_table[r->u.prim.op];
        sbuf b = {0};
        if (!dest && (p->flags & PF_PURE)) return;
        if (r->u.prim.op == PI_VALUES) {
            if (r->u.prim.n == 1) sb_puts(&b, atom(r->u.prim.args[0])); /* one value is itself */
            else if (r->u.prim.n == 0) sb_puts(&b, "rt_values_n(0, 0)");
            else {
                sb_printf(&b, "rt_values_n(%d, (obj[]){", r->u.prim.n);
                for (i = 0; i < r->u.prim.n; i++) sb_printf(&b, "%s%s", i ? ", " : "", atom(r->u.prim.args[i]));
                sb_puts(&b, "})");
            }
        } else {
            sb_printf(&b, "%s(", p->cfn);
            for (i = 0; i < r->u.prim.n; i++) sb_printf(&b, "%s%s", i ? ", " : "", atom(r->u.prim.args[i]));
            sb_putc(&b, ')');
        }
        if (dest) out(ind, "%s = %s;", dest, b.s);
        else out(ind, "(void)%s;", b.s);
        sb_free(&b);
        return;
    }
    case IR_CALL: emit_call(r, dest, ind); return;
    case IR_LAMBDA:
        if (dest) { emit_closure(r->u.lam, dest, ind, 0); emit_closure(r->u.lam, dest, ind, 1); }
        return;
    case IR_SET: {
        ir_var *v = r->u.set.v;
        if (v->flags & V_GLOBAL) out(ind, "rt_g[%d] = %s;", v->slot, atom(r->u.set.val));
        else if (used(v)) out(ind, "%s = %s;", val(v), atom(r->u.set.val));
        if (dest) out(ind, "%s = RT_UNSPEC;", dest);
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
        for (i = 0; i < j->n; i++) if (used(j->params[i])) out(ind + 1, "obj t%d_ = %s;", i, atom(args[i]));
        for (i = 0; i < j->n; i++) if (used(j->params[i])) out(ind + 1, "%s = t%d_;", cvar(j->params[i]), i);
    } else
        for (i = 0; i < j->n; i++)
            if (used(j->params[i]) && !(args[i]->kind == IR_REF && args[i]->u.ref.v == j->params[i]))
                out(ind, "%s = %s;", cvar(j->params[i]), atom(args[i]));
    if (fall) {} /* the label follows */
    else if (j == cur->entry) out(ind + par, "goto entry;");
    else out(ind + par, "goto j%d;", j->id);
    if (par) out(ind, "}");
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
                for (i = 0; i < M->nfree; i++) out(ind, "sc_%d.free[%d] = %s;", id, i, val(M->free[i]));
                out(ind, "%s = (obj)((char *)&sc_%d.code + 3);", cvar(n->u.let.v), id);
                n = n->u.let.body;
                continue;
            }
            emit_rhs(n->u.let.rhs, used(n->u.let.v) ? cvar(n->u.let.v) : NULL, ind);
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
            out(ind, "if (%s != RT_FALSE) {", atom(n->u.if_.test));
            emit_term(n->u.if_.then, ind + 1);
            out(ind, "} else {");
            emit_term(n->u.if_.els, ind + 1);
            out(ind, "}");
            return;
        case IR_JUMP: emit_jump(n->u.jump.j, n->u.jump.args, ind, 0); return;
        case IR_RET: out(ind, "return %s;", atom(n->u.ret)); return;
        case IR_CALL: emit_call(n, NULL, ind); return;
        default: cerror(RT_FALSE, "internal error: bad term kind %d", n->kind);
        }
    }
}

/* Variables a function must declare: its parameters and every binder in its body. */
static void decl_var(ir_var *v) { if (used(v)) out(1, "obj %s = 0;", cvar(v)); }
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
    for (i = 0; i < L->nparams; i++) sb_printf(&b, "%sobj%s%s", i ? ", " : "", names ? " " : "", names ? cvar(L->params[i]) : "");
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
        out(0, "static obj %s(%s) {", L->cname, direct_params(L, 1));
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
    if (L != p->main && !L->dispatch && L->escapes) { /* unknown callers: check the count */
        sbuf b = {0};
        c_string(&b, cname_str(L->name), strlen(cname_str(L->name)));
        if (L->rest) out(1, "if (RT_UNLIKELY(argc < %d)) rt_arity_error(%s, argc);", L->nreq, b.s);
        else out(1, "if (RT_UNLIKELY(argc != %d)) rt_arity_error(%s, argc);", L->nreq, b.s);
        sb_free(&b);
    }
    for (i = 0; i < L->nreq; i++)
        if (used(L->params[i])) {
            if (i < 4) out(1, "%s = a%d;", cvar(L->params[i]), i);
            else out(1, "%s = rt_args[%d];", cvar(L->params[i]), i - 4);
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
    sbuf protos = {0}, closures = {0};
    for (i = 0; i < p->nglobals; i++)
        if (!p->globals[i]->known) p->globals[i]->slot = nglob++;
    for (L = p->lambdas; L; L = L->next) {
        if (L->direct) sb_printf(&protos, "static RT_UNUSED obj %s(%s);\n", L->cname, direct_params(L, 0));
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
    if (p->raise) /* runtime errors detected by primitives are raised by (scheme base) raise */
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
