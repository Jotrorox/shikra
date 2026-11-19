/* emit.c -- the naive C backend (spec §5 pass 7, §8.1; DESIGN.md §3, §7, §11 M0 part B).
 *
 * One translation unit: config #defines, rt.h, the generated code, the runtime sources.
 * Generated code: literals as static data (read-only flag set; symbols interned at
 * startup), the globals array (a GC root range), one C function per procedure with the
 * uniform signature, a static closure for every procedure without free variables, and
 * rt_program_main / rt_engine_raise (the program-mode contract, DESIGN.md §11 M0 part A).
 * Tail calls: self tail calls are gotos to the entry label; every other tail call is
 * `RT_MUSTTAIL return f(...)`. Every primitive call goes through the checked runtime
 * function named in prims.def. */
#include "ir.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static sbuf lits, code;    /* literal definitions; function bodies */
static int nlits, nsyms;
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
    sb_printf(&lits, "static rt_string L%d = {RT_HDR(RT_T_STRING, RT_F_RO%s, 0), %ld, %ld, ", k, ascii ? " | RT_F_ASCII" : "",
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
        sb_printf(&lits, "static rt_symbol L%d = {RT_HDR(RT_T_SYMBOL, 0, 1), %s};\n", k, r);
        r = fmt("((obj)((char *)&L%d + 5))", k);
        sb_printf(&symtab, "%s%s", nsyms++ ? ", " : "", r);
        ht_put(&symmap, arena_strdup(&compiler_arena, s->buf), (void *)r);
        return r;
    }
    if ((r = (const char *)pm_get(&litmap, x))) return r;
    if (rt_is_pair(x)) {
        const char *a = lit(RT_CAR(x)), *d = lit(RT_CDR(x));
        int k = nlits++;
        sb_printf(&lits, "static rt_pair L%d = {%s, %s};\n", k, a, d);
        r = fmt("((obj)((char *)&L%d + 1))", k);
    } else if (rt_is_string(x)) {
        r = lit_string(RT_STR(x)->buf, (size_t)RT_STR(x)->bytes);
    } else if (rt_is_flo(x)) {
        double d = RT_FLO(x);
        int k = nlits++;
        if (d != d) sb_printf(&lits, "static rt_flonum L%d = {RT_HDR(RT_T_FLONUM, 0, 8), G_NAN};\n", k);
        else if (d - d != 0) sb_printf(&lits, "static rt_flonum L%d = {RT_HDR(RT_T_FLONUM, 0, 8), %sG_INF};\n", k, d < 0 ? "-" : "");
        else sb_printf(&lits, "static rt_flonum L%d = {RT_HDR(RT_T_FLONUM, 0, 8), %a};\n", k, d);
        r = fmt("((obj)((char *)&L%d + 5))", k);
    } else if (rt_is_vector(x)) {
        const char **el;
        int k;
        n = RT_HLEN(x);
        el = NEWN(const char *, n);
        for (i = 0; i < n; i++) el[i] = lit(RT_VEC(x)[i]);
        k = nlits++;
        if (n == 0) sb_printf(&lits, "static struct { uintptr_t hdr; } L%d = {RT_HDR(RT_T_VECTOR, RT_F_RO, 0)};\n", k);
        else {
            sb_printf(&lits, "static struct { uintptr_t hdr; obj v[%ld]; } L%d = {RT_HDR(RT_T_VECTOR, RT_F_RO, %ld), {", (long)n, k, (long)n);
            for (i = 0; i < n; i++) sb_printf(&lits, "%s%s", i ? ", " : "", el[i]);
            sb_puts(&lits, "}};\n");
        }
        r = fmt("((obj)((char *)&L%d + 5))", k);
    } else if (rt_is_bytevector(x)) {
        int k = nlits++;
        n = RT_HLEN(x);
        if (n == 0) sb_printf(&lits, "static struct { uintptr_t hdr; } L%d = {RT_HDR(RT_T_BYTEVECTOR, RT_F_RO, 0)};\n", k);
        else {
            sb_printf(&lits, "static struct { uintptr_t hdr; unsigned char b[%ld]; } L%d = {RT_HDR(RT_T_BYTEVECTOR, RT_F_RO, %ld), {", (long)n, k, (long)n);
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
static void emit_call(ir_node *c, const char *dest, int ind) {
    const char *pre = c->u.call.tail ? "RT_MUSTTAIL return " : dest ? fmt("%s = ", dest) : "(void)";
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
            out(ind + 1, "n_ = rt_spread_values(RT_PROC(f_)->code(f_, 0, 0, 0, 0, 0), r_);");
            out(ind + 1, "f_ = %s;", x);
        }
        out(ind + 1, "RT_CALLABLE(f_);");
        out(ind + 1, "%sRT_PROC(f_)->code(f_, n_, r_[0], r_[1], r_[2], r_[3]);", pre);
        out(ind, "}");
        return;
    }
    if (c->u.call.known) {
        const char *self = atom(c->u.call.fn), *args = call_args(c->u.call.args, c->u.call.n, ind);
        out(ind, "%s%s(%s, %d%s);", pre, c->u.call.known->cname, self, c->u.call.n, args);
    } else {
        const char *f = atom(c->u.call.fn), *args;
        if (c->u.call.fn->kind == IR_CONST || c->u.call.fn->u.ref.check) { /* evaluate once */
            out(ind, "{");
            out(ind + 1, "obj f_ = %s;", f);
            out(ind + 1, "RT_CALLABLE(f_);");
            args = call_args(c->u.call.args, c->u.call.n, ind + 1);
            out(ind + 1, "%sRT_PROC(f_)->code(f_, %d%s);", pre, c->u.call.n, args);
            out(ind, "}");
            return;
        }
        out(ind, "RT_CALLABLE(%s);", f);
        args = call_args(c->u.call.args, c->u.call.n, ind);
        out(ind, "%sRT_PROC(%s)->code(%s, %d%s);", pre, f, f, c->u.call.n, args);
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

static void emit_jump(ir_join *j, ir_node **args, int ind) {
    int i, any = 0;
    if (j == cur->entry) { /* parallel assignment through temporaries */
        for (i = 0; i < j->n; i++) if (used(j->params[i])) any = 1;
        if (any) {
            out(ind, "{");
            for (i = 0; i < j->n; i++) if (used(j->params[i])) out(ind + 1, "obj t%d_ = %s;", i, atom(args[i]));
            for (i = 0; i < j->n; i++) if (used(j->params[i])) out(ind + 1, "%s = t%d_;", cvar(j->params[i]), i);
            out(ind + 1, "goto entry;");
            out(ind, "}");
        } else out(ind, "goto entry;");
        return;
    }
    for (i = 0; i < j->n; i++) if (used(j->params[i])) out(ind, "%s = %s;", cvar(j->params[i]), atom(args[i]));
    out(ind, "goto j%d;", j->id);
}

static void emit_term(ir_node *n, int ind) {
    int i;
    for (; n; ) {
        switch (n->kind) {
        case IR_LET:
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
        case IR_LETJ:
            emit_term(n->u.letj.body, ind);
            if (n->u.letj.j->njumps) out(ind - 1, "j%d:;", n->u.letj.j->id);
            n = n->u.letj.j->body;
            continue;
        case IR_IF:
            out(ind, "if (%s != RT_FALSE) {", atom(n->u.if_.test));
            emit_term(n->u.if_.then, ind + 1);
            out(ind, "} else {");
            emit_term(n->u.if_.els, ind + 1);
            out(ind, "}");
            return;
        case IR_JUMP: emit_jump(n->u.jump.j, n->u.jump.args, ind); return;
        case IR_RET: out(ind, "return %s;", atom(n->u.ret)); return;
        case IR_CALL: emit_call(n, NULL, ind); return;
        default: cerror(RT_FALSE, "internal error: bad term kind %d", n->kind);
        }
    }
}

/* Variables a function must declare: its parameters and every binder in its body. */
static void decl_var(ir_var *v) { if (used(v)) out(1, "obj %s = 0;", cvar(v)); }
static void decl_term(ir_node *n) {
    int i;
    for (; n; ) {
        switch (n->kind) {
        case IR_LET: decl_var(n->u.let.v); n = n->u.let.body; continue;
        case IR_FIX: for (i = 0; i < n->u.fix.n; i++) decl_var(n->u.fix.vars[i]); n = n->u.fix.body; continue;
        case IR_LETJ:
            for (i = 0; i < n->u.letj.j->n; i++) decl_var(n->u.letj.j->params[i]);
            decl_term(n->u.letj.body);
            n = n->u.letj.j->body;
            continue;
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

static void emit_function(ir_lambda *L, ir_program *p) {
    int i;
    cur = L;
    out(0, "%s", proc_comment(L));
    out(0, "static obj %s(obj self, intptr_t argc, obj a0, obj a1, obj a2, obj a3) {", L->cname);
    if (L->nclauses) { /* case-lambda dispatch on argc */
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
    for (i = 0; i < L->nparams; i++) decl_var(L->params[i]);
    decl_term(L->body);
    out(1, "(void)self; (void)argc; (void)a0; (void)a1; (void)a2; (void)a3;");
    if (L != p->main && !L->dispatch) {
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
        sb_printf(&protos, "static RT_UNUSED obj %s(obj, intptr_t, obj, obj, obj, obj);\n", L->cname);
        if (L->is_static && L != p->main && !L->dispatch)
            sb_printf(&closures, "static RT_UNUSED rt_proc %s = {RT_HDR(RT_T_PROC, 0, 0), %s};\n", closure_name(L), L->cname);
    }
    for (L = p->lambdas; L; L = L->next) emit_function(L, p);

    sb_puts(o_, "/* Generated by shikra (DESIGN.md §3): config, rt.h, program, runtime. */\n");
    sb_puts(o_, "#define SHIKRA_PROGRAM 1\n");
    if (o->unsafe) sb_puts(o_, "#define RT_UNSAFE 1\n");
    if (o->debug) sb_puts(o_, "#define RT_DEBUG 1\n");
    if (o->gc_bump) sb_puts(o_, "#define RT_GC_BUMP 1\n");
    if (o->unicode_full) sb_puts(o_, "#define RT_UNICODE_FULL 1\n");
    if (o->gc_stress) sb_puts(o_, "#define RT_GC_STRESS 1\n");
    append_embedded(o_, "src/rt.h");
    sb_puts(o_, "\n/* ==== program ==== */\n");
    sb_puts(o_, "#if defined(__GNUC__) || defined(__clang__)\n#define G_INF __builtin_inf()\n#define G_NAN __builtin_nan(\"\")\n"
                "#else\n#define G_INF (1e308 * 10.0)\n#define G_NAN (G_INF - G_INF)\n#endif\n");
    if (lits.len) sb_putn(o_, lits.s, lits.len);
    sb_printf(o_, "static obj rt_g[%d]; /* globals: a GC root range */\n", nglob + 1);
    if (nsyms) sb_printf(o_, "static obj rt_static_syms[%d] = {%s};\n", nsyms, symtab.s);
    sb_putn(o_, protos.s, protos.len);
    if (closures.len) sb_putn(o_, closures.s, closures.len);
    sb_putc(o_, '\n');
    sb_putn(o_, code.s, code.len);
    sb_puts(o_, "static void rt_program_main(void) {\n    int i;\n");
    sb_printf(o_, "    for (i = 0; i < %d; i++) rt_g[i] = RT_UNDEF;\n", nglob + 1);
    sb_printf(o_, "    rt_gc_add_roots(rt_g, rt_g + %d);\n", nglob + 1);
    if (nsyms) sb_printf(o_, "    for (i = 0; i < %d; i++) rt_intern_static(rt_static_syms[i]);\n", nsyms);
    sb_printf(o_, "    (void)%s(RT_FALSE, 0, 0, 0, 0, 0);\n}\n", p->main->cname);
    if (p->raise) /* runtime errors detected by primitives are raised by (scheme base) raise */
        sb_printf(o_, "static void rt_engine_raise(obj e) {\n    (void)%s(RT_SPROC(%s), 1, e, 0, 0, 0);\n    rt_uncaught(e);\n}\n",
                  p->raise->cname, closure_name(p->raise));
    else sb_puts(o_, "static void rt_engine_raise(obj e) { rt_uncaught(e); }\n");
    for (i = 0; rtfiles[i]; i++) append_embedded(o_, rtfiles[i]);
    sb_free(&protos);
    sb_free(&closures);
}
