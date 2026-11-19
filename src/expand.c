/* expand.c -- pass 3 (spec §5, DESIGN.md §6): hygienic syntax-rules expansion and the
 * lowering of derived syntax to core forms.
 *
 * Hygiene is explicit renaming: every symbol a template inserts becomes a fresh *alias*
 * (an uninterned symbol) that remembers the original identifier and the macro's
 * definition environment. Looking up an alias first searches the current environment
 * for the alias itself (it may have been bound by the expansion), then resolves the
 * original identifier in the macro's environment. Derived forms are lowered in C by
 * rewriting them into source forms whose keywords are aliases into (shikra primitives)
 * (kw()), so the rewrites are hygienic by the same mechanism and cannot be captured by
 * user bindings. */
#include "front.h"
#include <stdio.h>
#include <string.h>

obj S_define, S_lambda, S_if, S_quote, S_set, S_letrec_star, S_begin, S_case_lambda;

typedef struct { obj orig; frame *env; } alias_t;
static pmap aliases;        /* alias -> alias_t* */
static pmap globals_by_sym; /* global core symbol -> global* */
static pmap local_names;    /* local core symbol -> source name symbol */
static long local_counter;

static obj expand(obj x, frame *env);
static obj expand_body(obj body, frame *env, obj src);

/* ---- small datum helpers ---- */
obj sym(const char *s) { return rt_intern(s, strlen(s)); }
int list_len(obj x) {
    int n = 0;
    for (; rt_is_pair(x); x = RT_CDR(x)) n++;
    return x == RT_NIL ? n : -1;
}
obj L2(obj a, obj b) { return rt_cons(a, rt_cons(b, RT_NIL)); }
obj L3(obj a, obj b, obj c) { return rt_cons(a, L2(b, c)); }
obj L4(obj a, obj b, obj c, obj d) { return rt_cons(a, L3(b, c, d)); }
static obj L1(obj a) { return rt_cons(a, RT_NIL); }
obj append2(obj a, obj b) {
    if (!rt_is_pair(a)) return b;
    return rt_cons(RT_CAR(a), append2(RT_CDR(a), b));
}
#define CADR(x) RT_CAR(RT_CDR(x))
#define CDDR(x) RT_CDR(RT_CDR(x))
#define CADDR(x) RT_CAR(CDDR(x))
#define CDDDR(x) RT_CDR(CDDR(x))
#define CADDDR(x) RT_CAR(CDDDR(x))
static obj vec_list(obj v) { return rt_vector_to_list(v, RT_DEFAULT, RT_DEFAULT); }
const char *dstr(obj x) {
    obj p = rt_open_output_string();
    rt_print(strip_syntax(x), p, 1);
    return RT_STR(rt_get_output_string(p))->buf;
}

/* ---- identifiers, aliases, frames, bindings ---- */
int is_ident(obj x) { return rt_is_symbol(x); }
static alias_t *alias_of(obj x) { return rt_is_symbol(x) ? (alias_t *)pm_get(&aliases, x) : NULL; }
obj strip_id(obj x) {
    alias_t *a;
    while ((a = alias_of(x))) x = a->orig;
    return x;
}
static obj make_alias(obj id, frame *env) {
    obj s = rt_make_symbol(RT_PTR(strip_id(id), rt_symbol)->name);
    alias_t *a = NEW(alias_t);
    a->orig = id;
    a->env = env;
    pm_put(&aliases, s, a);
    return s;
}
obj front_alias(obj orig, frame *env) { return make_alias(orig, env); }
int front_alias_info(obj sym, obj *orig, frame **env) {
    alias_t *a = alias_of(sym);
    if (a) { *orig = a->orig; *env = a->env; }
    return a != NULL;
}
obj strip_syntax(obj x) {
    if (rt_is_symbol(x)) return strip_id(x);
    if (rt_is_pair(x)) {
        obj a = strip_syntax(RT_CAR(x)), d = strip_syntax(RT_CDR(x));
        return a == RT_CAR(x) && d == RT_CDR(x) ? x : rt_cons(a, d);
    }
    if (rt_is_vector(x)) {
        obj l = vec_list(x), s = strip_syntax(l);
        return s == l ? x : rt_list_to_vector(s);
    }
    return x;
}
frame *new_frame(frame *parent) {
    frame *f = NEW(frame);
    f->parent = parent;
    f->lib = parent ? parent->lib : NULL;
    return f;
}
binding *new_binding(int kind) {
    binding *b = NEW(binding);
    b->kind = kind;
    return b;
}
static binding *lookup_in(frame *env, obj id, frame **where) {
    for (;;) {
        frame *f;
        alias_t *a;
        for (f = env; f; f = f->parent) {
            binding *b = (binding *)pm_get(&f->map, id);
            if (b) {
                if (where) *where = f;
                if (f->lib && f->lib->imports) front_note_use(f->lib, id, b);
                return b;
            }
        }
        if (!(a = alias_of(id))) return NULL;
        env = a->env;
        id = a->orig;
    }
}
binding *lookup(frame *env, obj id) { return lookup_in(env, id, NULL); }

/* Two identifiers are free-identifier=? when they have the same binding, or are both
 * unbound with the same name. */
static int free_id_eq(frame *e1, obj a, frame *e2, obj b) {
    binding *ba = lookup(e1, a), *bb = lookup(e2, b);
    if (ba || bb) return ba == bb;
    return strip_id(a) == strip_id(b);
}
/* Does identifier x denote core keyword / auxiliary syntax k in env? */
static int is_kw(frame *env, obj x, int k) {
    binding *b;
    if (!is_ident(x)) return 0;
    if ((b = lookup(env, x))) return b->kind == B_CORE && b->id == k;
    return strip_id(x) == sym(keyword_names[k]);
}

/* Aliases into (shikra primitives): hygienic keywords and primitives for rewrites. */
static obj kwcache[NKEYWORDS];
static obj kw(int k) {
    if (!kwcache[k]) kwcache[k] = make_alias(sym(keyword_names[k]), core_env);
    return kwcache[k];
}
static obj pcache[NPRIMS_ALL];
static obj kp(int pi) {
    if (!pcache[pi]) pcache[pi] = make_alias(sym(prim_table[pi].name), core_env);
    return pcache[pi];
}
static obj fresh(const char *base) { return make_alias(sym(base), core_env); }
static obj kquote(obj d) { return L2(kw(K_QUOTE), d); }
static obj unspec_form(void) { return L3(kw(K_IF), RT_FALSE, RT_FALSE); }
static obj begin_of(obj body) { return list_len(body) == 1 ? RT_CAR(body) : rt_cons(kw(K_BEGIN), body); }

obj front_local_name(obj s) { return (obj)pm_get(&local_names, s); }
obj front_make_local(obj srcname, long n) {
    char buf[32];
    obj s;
    sbuf b = {0};
    snprintf(buf, sizeof buf, ".%ld", n);
    sb_puts(&b, RT_STR(RT_PTR(srcname, rt_symbol)->name)->buf);
    sb_puts(&b, buf);
    s = rt_make_symbol(rt_cstring(b.s));
    sb_free(&b);
    pm_put(&local_names, s, srcname);
    return s;
}
static obj new_local_sym(obj id) { return front_make_local(strip_id(id), ++local_counter); }
long front_counter(long set) {
    if (set >= 0) local_counter = set;
    return local_counter;
}
global *new_global(library *lib, obj id) {
    global *g = NEW(global);
    sbuf b = {0};
    g->name = strip_id(id);
    g->lib = lib;
    sb_printf(&b, "%s:%s", lib->id, RT_STR(RT_PTR(g->name, rt_symbol)->name)->buf);
    g->sym = rt_make_symbol(rt_cstring(b.s));
    sb_free(&b);
    pm_put(&globals_by_sym, g->sym, g);
    if (lib->nglobals == lib->capglobals) {
        global **ng = NEWN(global *, lib->capglobals * 2 + 16);
        if (lib->nglobals) memcpy(ng, lib->globals, sizeof(global *) * (size_t)lib->nglobals);
        lib->globals = ng;
        lib->capglobals = lib->capglobals * 2 + 16;
    }
    g->index = lib->nglobals;
    lib->globals[lib->nglobals++] = g;
    return g;
}
global *front_global(obj s) { return rt_is_symbol(s) ? (global *)pm_get(&globals_by_sym, s) : NULL; }

static int need(obj x, int min, int max, const char *what) {
    int n = list_len(x);
    if (n < min || (max >= 0 && n > max)) cerror(x, "malformed %s", what);
    return n;
}
static obj bind_local(frame *f, obj id, obj src, const char *dupmsg) {
    binding *b;
    if (!is_ident(id)) cerror(src, "expected an identifier, got %s", dstr(id));
    if (pm_get(&f->map, id)) cerror(src, "%s %s", dupmsg, idname(id));
    b = new_binding(B_LOCAL);
    b->sym = new_local_sym(id);
    pm_put(&f->map, id, b);
    return b->sym;
}

/* ---- syntax-rules ---- */
typedef struct mb { obj var; int depth; obj val; struct mb *next; } mb;
static mb *mb_push(mb *next, obj var, int depth, obj val) {
    mb *b = NEW(mb);
    b->var = var; b->depth = depth; b->val = val; b->next = next;
    return b;
}
static mb *mb_find(mb *b, obj var) {
    for (; b; b = b->next) if (b->var == var) return b;
    return NULL;
}
static int memq_(obj x, obj l) {
    for (; rt_is_pair(l); l = RT_CDR(l)) if (RT_CAR(l) == x) return 1;
    return 0;
}
static int is_ellipsis(macro *m, obj x) { /* a literal has priority (R7RS 4.3.2) */
    if (!is_ident(x) || memq_(x, m->literals)) return 0;
    if (m->ellipsis) return x == m->ellipsis;
    return is_kw(m->env, x, K_ELLIPSIS);
}
static int is_underscore(macro *m, obj x) { return !memq_(x, m->literals) && is_kw(m->env, x, K_UNDERSCORE); }

static macro *make_macro(obj spec, frame *env, obj name) {
    binding *b;
    macro *m;
    obj rest, l;
    if (is_ident(spec) && (b = lookup(env, spec)) && b->kind == B_MACRO) return b->mac;
    if (!rt_is_pair(spec) || !is_kw(env, RT_CAR(spec), K_SYNTAX_RULES))
        cerror(spec, "unsupported macro transformer for %s: only syntax-rules is supported", idname(name));
    need(spec, 2, -1, "syntax-rules");
    m = NEW(macro);
    m->name = strip_id(name);
    m->env = env;
    rest = RT_CDR(spec);
    if (is_ident(RT_CAR(rest))) {
        m->ellipsis = RT_CAR(rest);
        rest = RT_CDR(rest);
        if (rest == RT_NIL) cerror(spec, "malformed syntax-rules");
    }
    m->literals = RT_CAR(rest);
    if (list_len(m->literals) < 0) cerror(spec, "malformed syntax-rules literals");
    for (l = m->literals; l != RT_NIL; l = RT_CDR(l))
        if (!is_ident(RT_CAR(l))) cerror(spec, "syntax-rules literal is not an identifier: %s", dstr(RT_CAR(l)));
    m->rules = RT_CDR(rest);
    for (l = m->rules; l != RT_NIL; l = RT_CDR(l))
        if (list_len(RT_CAR(l)) != 2 || !rt_is_pair(RT_CAR(RT_CAR(l)))) cerror(spec, "malformed syntax-rules rule %s", dstr(RT_CAR(l)));
    return m;
}

/* Pattern variables of p, with their ellipsis depth. */
static mb *pvars(macro *m, obj p, int depth, mb *acc) {
    if (is_ident(p)) {
        if (memq_(p, m->literals) || is_underscore(m, p) || is_ellipsis(m, p)) return acc;
        return mb_push(acc, p, depth, RT_NIL);
    }
    if (rt_is_pair(p)) {
        if (rt_is_pair(RT_CDR(p)) && is_ellipsis(m, CADR(p))) return pvars(m, CDDR(p), depth, pvars(m, RT_CAR(p), depth + 1, acc));
        return pvars(m, RT_CDR(p), depth, pvars(m, RT_CAR(p), depth, acc));
    }
    if (rt_is_vector(p)) return pvars(m, vec_list(p), depth, acc);
    return acc;
}

static int match(macro *m, obj p, obj f, frame *uenv, mb **bs) {
    if (is_ident(p)) {
        if (is_underscore(m, p)) return 1;
        if (memq_(p, m->literals)) return is_ident(f) && free_id_eq(uenv, f, m->env, p);
        *bs = mb_push(*bs, p, 0, f);
        return 1;
    }
    if (rt_is_pair(p)) {
        if (rt_is_pair(RT_CDR(p)) && is_ellipsis(m, CADR(p))) {
            obj sub = RT_CAR(p), tail = CDDR(p), q;
            int tmin = 0, nf = 0, k, i;
            mb **subs, *v;
            for (q = tail; rt_is_pair(q); q = RT_CDR(q)) tmin++;
            for (q = f; rt_is_pair(q); q = RT_CDR(q)) nf++;
            if ((k = nf - tmin) < 0) return 0;
            subs = NEWN(mb *, k);
            for (i = 0, q = f; i < k; i++, q = RT_CDR(q))
                if (!match(m, sub, RT_CAR(q), uenv, &subs[i])) return 0;
            for (v = pvars(m, sub, 0, NULL); v; v = v->next) {
                obj l = RT_NIL;
                for (i = k - 1; i >= 0; i--) l = rt_cons(mb_find(subs[i], v->var)->val, l);
                *bs = mb_push(*bs, v->var, v->depth + 1, l);
            }
            return match(m, tail, q, uenv, bs);
        }
        return rt_is_pair(f) && match(m, RT_CAR(p), RT_CAR(f), uenv, bs) && match(m, RT_CDR(p), RT_CDR(f), uenv, bs);
    }
    if (p == RT_NIL) return f == RT_NIL;
    if (rt_is_vector(p)) return rt_is_vector(f) && match(m, vec_list(p), vec_list(f), uenv, bs);
    return rt_equal(p, f);
}

static obj inst(macro *m, obj t, mb *bs, pmap *ren, int ell);
static void tvars(obj t, mb *bs, mb **acc) {
    if (is_ident(t)) {
        mb *b = mb_find(bs, t);
        if (b && b->depth > 0 && !mb_find(*acc, t)) *acc = mb_push(*acc, t, b->depth, b->val);
    } else if (rt_is_pair(t)) {
        tvars(RT_CAR(t), bs, acc);
        tvars(RT_CDR(t), bs, acc);
    } else if (rt_is_vector(t)) tvars(vec_list(t), bs, acc);
}
/* Instances of template element e followed by k ellipses, as a list. */
static obj inst_ell(macro *m, obj e, int k, mb *bs, pmap *ren) {
    mb *vs = NULL, *v;
    int len = -1, i;
    obj out = RT_NIL;
    tvars(e, bs, &vs);
    if (!vs) cerror(RT_FALSE, "in macro %s: no pattern variable before ellipsis in template", idname(m->name));
    for (v = vs; v; v = v->next) {
        int n = list_len(v->val);
        if (len >= 0 && n != len) cerror(RT_FALSE, "in macro %s: pattern variables under one ellipsis matched different lengths", idname(m->name));
        len = n;
    }
    for (i = 0; i < len; i++) {
        mb *nbs = bs;
        for (v = vs; v; v = v->next) {
            obj l = v->val;
            int j;
            for (j = 0; j < i; j++) l = RT_CDR(l);
            nbs = mb_push(nbs, v->var, v->depth - 1, RT_CAR(l));
        }
        if (k == 1) out = rt_cons(inst(m, e, nbs, ren, 1), out);
        else {
            obj l = inst_ell(m, e, k - 1, nbs, ren);
            for (; l != RT_NIL; l = RT_CDR(l)) out = rt_cons(RT_CAR(l), out);
        }
    }
    return rt_reverse(out);
}
static obj inst(macro *m, obj t, mb *bs, pmap *ren, int ell) {
    if (is_ident(t)) {
        mb *b = mb_find(bs, t);
        obj a;
        if (b) {
            if (b->depth) cerror(RT_FALSE, "in macro %s: pattern variable %s used without ellipsis", idname(m->name), idname(t));
            return b->val;
        }
        if (!(a = (obj)pm_get(ren, t))) {
            a = make_alias(t, m->env);
            pm_put(ren, t, a);
        }
        return a;
    }
    if (rt_is_pair(t)) {
        obj out = RT_NIL, last = RT_NIL, x;
        if (ell && is_ellipsis(m, RT_CAR(t))) {
            if (!rt_is_pair(RT_CDR(t))) cerror(RT_FALSE, "in macro %s: malformed (... template)", idname(m->name));
            return inst(m, CADR(t), bs, ren, 0);
        }
        while (rt_is_pair(t)) {
            obj e = RT_CAR(t), rest = RT_CDR(t), items;
            int k = 0;
            while (ell && rt_is_pair(rest) && is_ellipsis(m, RT_CAR(rest))) { k++; rest = RT_CDR(rest); }
            items = k ? inst_ell(m, e, k, bs, ren) : L1(inst(m, e, bs, ren, ell));
            for (; items != RT_NIL; items = RT_CDR(items)) {
                x = L1(RT_CAR(items));
                if (last == RT_NIL) out = x;
                else RT_CDR(last) = x;
                last = x;
            }
            t = rest;
        }
        if (t != RT_NIL) {
            x = inst(m, t, bs, ren, ell);
            if (last == RT_NIL) return x;
            RT_CDR(last) = x;
        }
        return out;
    }
    if (rt_is_vector(t)) return rt_list_to_vector(inst(m, vec_list(t), bs, ren, ell));
    return t;
}

static obj expand_macro(macro *m, obj x, frame *uenv) {
    obj r;
    for (r = m->rules; r != RT_NIL; r = RT_CDR(r)) {
        obj pat = RT_CAR(RT_CAR(r)), tmpl = CADR(RT_CAR(r));
        mb *bs = NULL;
        if (match(m, RT_CDR(pat), RT_CDR(x), uenv, &bs)) {
            pmap ren = {0};
            return inst(m, tmpl, bs, &ren, 1);
        }
    }
    cerror(x, "no matching syntax-rules clause for %s", dstr(x));
}

/* ---- expressions ---- */
static obj expand_at(obj p, frame *env) { /* car of p, with p as location context */
    obj saved = loc_ctx, r;
    if (has_loc(p)) loc_ctx = p;
    r = expand(RT_CAR(p), env);
    loc_ctx = saved;
    return r;
}
static obj expand_list(obj l, frame *env) {
    obj out = RT_NIL;
    for (; rt_is_pair(l); l = RT_CDR(l)) out = rt_cons(expand_at(l, env), out);
    return rt_reverse(out);
}
static int self_evaluating(obj x) {
    return rt_is_num(x) || rt_is_string(x) || rt_is_char(x) || x == RT_TRUE || x == RT_FALSE || rt_is_vector(x) || rt_is_bytevector(x) ||
           rt_is_type(x, RT_T_F64VECTOR);
}

static binding *repl_autodefine(frame *env, obj id) {
    alias_t *a;
    frame *top;
    binding *b;
    while ((a = alias_of(id))) { env = a->env; id = a->orig; }
    for (top = env; top->parent; top = top->parent) {}
    if (!top->open) return NULL;
    b = new_binding(B_GLOBAL);
    b->g = new_global(top->lib, id);
    pm_put(&top->map, id, b);
    return b;
}
static obj expand_ref(obj id, frame *env) {
    binding *b = lookup(env, id);
    if (!b) b = repl_autodefine(env, id);
    if (!b) cerror(id, "unbound identifier: %s", idname(id));
    switch (b->kind) {
    case B_LOCAL: return b->sym;
    case B_GLOBAL: return b->g->sym;
    case B_PRIM: cerror(id, "primitive %s can only be applied, not used as a value", idname(id));
    default: cerror(id, "syntax keyword %s used as a variable", idname(id));
    }
}

static obj rename_formals(obj formals, frame *f, obj src) {
    obj a;
    if (formals == RT_NIL) return RT_NIL;
    if (is_ident(formals)) return bind_local(f, formals, src, "duplicate parameter");
    if (!rt_is_pair(formals)) cerror(src, "malformed parameter list");
    a = bind_local(f, RT_CAR(formals), src, "duplicate parameter");
    return rt_cons(a, rename_formals(RT_CDR(formals), f, src));
}
static obj expand_clause(obj formals, obj body, frame *env, obj src) {
    frame *f = new_frame(env);
    obj nf = rename_formals(formals, f, src);
    return L2(nf, expand_body(body, f, src));
}

/* ---- derived syntax: rewrites into source forms over kw() aliases ---- */
static obj rw_let(obj x) {
    obj name = RT_FALSE, bs, body, vars = RT_NIL, inits = RT_NIL, lam;
    int n = need(x, 3, -1, "let");
    if (is_ident(CADR(x))) {
        if (n < 4) cerror(x, "malformed named let");
        name = CADR(x); bs = CADDR(x); body = CDDDR(x);
    } else { bs = CADR(x); body = CDDR(x); }
    if (list_len(bs) < 0) cerror(x, "malformed let bindings");
    for (; bs != RT_NIL; bs = RT_CDR(bs)) {
        obj b = RT_CAR(bs);
        if (list_len(b) != 2 || !is_ident(RT_CAR(b))) cerror(has_loc(bs) ? bs : x, "malformed let binding %s", dstr(b));
        vars = rt_cons(RT_CAR(b), vars);
        inits = rt_cons(CADR(b), inits);
    }
    lam = rt_cons(kw(K_LAMBDA), rt_cons(rt_reverse(vars), body));
    if (name != RT_FALSE) lam = L3(kw(K_LETREC_STAR), L1(L2(name, lam)), name);
    return rt_cons(lam, rt_reverse(inits));
}
static obj rw_letstar(obj x) {
    obj bs;
    need(x, 3, -1, "let*");
    bs = CADR(x);
    if (list_len(bs) < 0) cerror(x, "malformed let* bindings");
    if (bs == RT_NIL || RT_CDR(bs) == RT_NIL) return rt_cons(kw(K_LET), RT_CDR(x));
    return L3(kw(K_LET), L1(RT_CAR(bs)), rt_cons(kw(K_LETSTAR), rt_cons(RT_CDR(bs), CDDR(x))));
}
static obj rw_cond(obj cls, frame *env, obj src) {
    obj cl, test, body, rest, t;
    if (cls == RT_NIL) return RT_UNDEF; /* no clause left (RT_UNDEF never occurs in source) */
    cl = RT_CAR(cls);
    if (list_len(cl) < 1) cerror(has_loc(cls) ? cls : src, "malformed cond clause %s", dstr(cl));
    test = RT_CAR(cl);
    body = RT_CDR(cl);
    if (is_kw(env, test, K_ELSE)) {
        if (RT_CDR(cls) != RT_NIL) cerror(has_loc(cls) ? cls : src, "else clause must be the last cond clause");
        if (body == RT_NIL) cerror(src, "empty else clause");
        return begin_of(body);
    }
    rest = rw_cond(RT_CDR(cls), env, src);
    if (body == RT_NIL) {
        t = fresh("t");
        return L3(kw(K_LET), L1(L2(t, test)), rest != RT_UNDEF ? L4(kw(K_IF), t, t, rest) : t);
    }
    if (is_kw(env, RT_CAR(body), K_ARROW)) {
        obj call;
        if (list_len(body) != 2) cerror(has_loc(cls) ? cls : src, "malformed cond => clause");
        t = fresh("t");
        call = L2(CADR(body), t);
        return L3(kw(K_LET), L1(L2(t, test)), rest != RT_UNDEF ? L4(kw(K_IF), t, call, rest) : L3(kw(K_IF), t, call));
    }
    return rest != RT_UNDEF ? L4(kw(K_IF), test, begin_of(body), rest) : L3(kw(K_IF), test, begin_of(body));
}
static obj rw_case(obj x, frame *env) {
    obj k = fresh("key"), cls, chain = RT_UNDEF, out = RT_NIL;
    need(x, 3, -1, "case");
    for (cls = rt_reverse(CDDR(x)); cls != RT_NIL; cls = RT_CDR(cls)) {
        obj cl = RT_CAR(cls), body, res, test, d;
        if (list_len(cl) < 2) cerror(x, "malformed case clause %s", dstr(cl));
        body = RT_CDR(cl);
        if (is_kw(env, RT_CAR(body), K_ARROW)) {
            if (list_len(body) != 2) cerror(x, "malformed case => clause");
            res = L2(CADR(body), k);
        } else res = begin_of(body);
        if (is_kw(env, RT_CAR(cl), K_ELSE)) {
            if (chain != RT_UNDEF) cerror(x, "else clause must be the last case clause");
            chain = res;
            continue;
        }
        if (list_len(RT_CAR(cl)) < 0) cerror(x, "malformed case clause data %s", dstr(RT_CAR(cl)));
        out = RT_NIL;
        for (d = RT_CAR(cl); d != RT_NIL; d = RT_CDR(d)) out = rt_cons(L3(kp(PI_rt_eqv_p), k, kquote(RT_CAR(d))), out);
        test = out == RT_NIL ? RT_FALSE : RT_CDR(out) == RT_NIL ? RT_CAR(out) : rt_cons(kw(K_OR), rt_reverse(out));
        chain = chain != RT_UNDEF ? L4(kw(K_IF), test, res, chain) : L3(kw(K_IF), test, res);
    }
    return L3(kw(K_LET), L1(L2(k, CADR(x))), chain != RT_UNDEF ? chain : unspec_form());
}
static obj rw_do(obj x) {
    obj loop = fresh("loop"), specs, bs = RT_NIL, steps = RT_NIL, tc, then;
    need(x, 3, -1, "do");
    specs = CADR(x);
    tc = CADDR(x);
    if (list_len(specs) < 0 || list_len(tc) < 1) cerror(x, "malformed do");
    for (; specs != RT_NIL; specs = RT_CDR(specs)) {
        obj s = RT_CAR(specs);
        int n = list_len(s);
        if ((n != 2 && n != 3) || !is_ident(RT_CAR(s))) cerror(x, "malformed do binding %s", dstr(s));
        bs = rt_cons(L2(RT_CAR(s), CADR(s)), bs);
        steps = rt_cons(n == 3 ? CADDR(s) : RT_CAR(s), steps);
    }
    then = RT_CDR(tc) == RT_NIL ? unspec_form() : begin_of(RT_CDR(tc));
    return L4(kw(K_LET), loop, rt_reverse(bs),
              L4(kw(K_IF), RT_CAR(tc), then, rt_cons(kw(K_BEGIN), append2(CDDDR(x), L1(rt_cons(loop, rt_reverse(steps)))))));
}

/* quasiquote (R7RS 4.2.8), nested levels and vectors. */
static int is_kquote(obj f) { return rt_is_pair(f) && RT_CAR(f) == kw(K_QUOTE); }
static obj qq_cons(obj a, obj d) {
    if (is_kquote(a) && is_kquote(d)) return kquote(rt_cons(CADR(a), CADR(d)));
    return L3(kp(PI_rt_cons), a, d);
}
static obj qq_tag(const char *tag, obj e) { return qq_cons(kquote(sym(tag)), qq_cons(e, kquote(RT_NIL))); }
static obj qq(obj x, int d, frame *env) {
    if (rt_is_pair(x)) {
        obj h = RT_CAR(x);
        if (is_kw(env, h, K_UNQUOTE) && list_len(x) == 2) return d == 1 ? CADR(x) : qq_tag("unquote", qq(CADR(x), d - 1, env));
        if (is_kw(env, h, K_QUASIQUOTE) && list_len(x) == 2) return qq_tag("quasiquote", qq(CADR(x), d + 1, env));
        if (rt_is_pair(h) && is_kw(env, RT_CAR(h), K_UNQUOTE_SPLICING) && list_len(h) == 2) {
            obj rest = qq(RT_CDR(x), d, env);
            if (d == 1) return RT_CDR(x) == RT_NIL ? CADR(h) : L3(kp(PI_rt_append2), CADR(h), rest);
            return qq_cons(qq_tag("unquote-splicing", qq(CADR(h), d - 1, env)), rest);
        }
        return qq_cons(qq(h, d, env), qq(RT_CDR(x), d, env));
    }
    if (rt_is_vector(x)) {
        obj l = qq(vec_list(x), d, env);
        return is_kquote(l) ? kquote(x) : L2(kp(PI_rt_list_to_vector), l);
    }
    return kquote(x);
}

static obj rw_record(obj x) {
    obj tname, ctor, pred, specs, fields = RT_NIL, out = RT_NIL, l, rtdname;
    int nf = 0, i;
    obj *fname, *acc, *mod;
    need(x, 4, -1, "define-record-type");
    tname = CADR(x); ctor = CADDR(x); pred = RT_CAR(CDDDR(x)); specs = RT_CDR(CDDDR(x));
    if (!is_ident(tname)) cerror(x, "define-record-type: type name must be an identifier");
    nf = list_len(specs);
    fname = NEWN(obj, nf); acc = NEWN(obj, nf); mod = NEWN(obj, nf);
    for (i = 0, l = specs; i < nf; i++, l = RT_CDR(l)) {
        obj s = RT_CAR(l);
        int n = is_ident(s) ? 0 : list_len(s);
        acc[i] = mod[i] = RT_FALSE;
        if (is_ident(s)) fname[i] = s;
        else if (n >= 1 && n <= 3 && is_ident(RT_CAR(s))) {
            fname[i] = RT_CAR(s);
            if (n >= 2) acc[i] = CADR(s);
            if (n == 3) mod[i] = CADDR(s);
        } else cerror(x, "malformed record field spec %s", dstr(s));
        fields = rt_cons(strip_id(fname[i]), fields);
    }
    {   /* rtd name: <point> -> point */
        const char *s = idname(tname);
        size_t n = strlen(s);
        rtdname = n > 2 && s[0] == '<' && s[n - 1] == '>' ? rt_intern(s + 1, n - 2) : strip_id(tname);
    }
    out = rt_cons(L3(kw(K_DEFINE), tname, L3(kp(PI_rt_make_rtd), kquote(rtdname), kquote(rt_list_to_vector(rt_reverse(fields))))), out);
    if (ctor != RT_FALSE) {
        obj cname, args, params = RT_NIL, sets = RT_NIL, r = fresh("r");
        if (is_ident(ctor)) {
            cname = ctor;
            args = RT_NIL;
            for (i = nf - 1; i >= 0; i--) args = rt_cons(fname[i], args);
        } else if (list_len(ctor) >= 1 && is_ident(RT_CAR(ctor))) {
            cname = RT_CAR(ctor);
            args = RT_CDR(ctor);
        } else cerror(x, "malformed record constructor spec %s", dstr(ctor));
        for (l = args; l != RT_NIL; l = RT_CDR(l)) {
            obj p = fresh("field");
            for (i = 0; i < nf && strip_id(fname[i]) != strip_id(RT_CAR(l)); i++) {}
            if (i == nf) cerror(x, "record constructor argument %s is not a field", dstr(RT_CAR(l)));
            params = rt_cons(p, params);
            sets = rt_cons(rt_cons(kp(PI_rt_record_set), rt_cons(r, L3(tname, kquote(RT_FIX(i)), p))), sets);
        }
        out = rt_cons(L3(kw(K_DEFINE), cname,
                         L3(kw(K_LAMBDA), rt_reverse(params),
                            L3(kw(K_LET), L1(L2(r, L2(kp(PI_rt_make_record), tname))),
                               rt_cons(kw(K_BEGIN), append2(rt_reverse(sets), L1(r)))))),
                      out);
    }
    if (pred != RT_FALSE) {
        obj v = fresh("obj");
        if (!is_ident(pred)) cerror(x, "record predicate must be an identifier");
        out = rt_cons(L3(kw(K_DEFINE), pred, L3(kw(K_LAMBDA), L1(v), L3(kp(PI_rt_record_p), v, tname))), out);
    }
    for (i = 0; i < nf; i++) {
        obj r = fresh("r"), v = fresh("v");
        if (acc[i] != RT_FALSE)
            out = rt_cons(L3(kw(K_DEFINE), acc[i], L3(kw(K_LAMBDA), L1(r), L4(kp(PI_rt_record_ref), r, tname, kquote(RT_FIX(i))))), out);
        if (mod[i] != RT_FALSE)
            out = rt_cons(L3(kw(K_DEFINE), mod[i],
                             L3(kw(K_LAMBDA), L2(r, v), rt_cons(kp(PI_rt_record_set), rt_cons(r, L3(tname, kquote(RT_FIX(i)), v))))),
                          out);
    }
    return rt_cons(kw(K_BEGIN), rt_reverse(out));
}

/* formals -> fresh temporaries of the same shape; *pairs receives ((id temp) ...) */
static obj temp_formals(obj f, obj *pairs, obj src) {
    obj t;
    if (f == RT_NIL) return RT_NIL;
    if (is_ident(f)) { t = fresh("tmp"); *pairs = rt_cons(L2(f, t), *pairs); return t; }
    if (!rt_is_pair(f) || !is_ident(RT_CAR(f))) cerror(src, "malformed formals %s", dstr(f));
    t = fresh("tmp");
    *pairs = rt_cons(L2(RT_CAR(f), t), *pairs);
    return rt_cons(t, temp_formals(RT_CDR(f), pairs, src));
}
static obj thunk(obj e) { return L3(kw(K_LAMBDA), RT_NIL, e); }
static obj rw_let_values(obj x) {
    obj bs, pairs = RT_NIL, tfs = RT_NIL, inner, l;
    need(x, 3, -1, "let-values");
    bs = CADR(x);
    if (list_len(bs) < 0) cerror(x, "malformed let-values bindings");
    for (l = bs; l != RT_NIL; l = RT_CDR(l)) {
        if (list_len(RT_CAR(l)) != 2) cerror(x, "malformed let-values binding %s", dstr(RT_CAR(l)));
        tfs = rt_cons(temp_formals(RT_CAR(RT_CAR(l)), &pairs, x), tfs);
    }
    inner = rt_cons(kw(K_LET), rt_cons(rt_reverse(pairs), CDDR(x)));
    for (l = rt_reverse(bs); l != RT_NIL; l = RT_CDR(l), tfs = RT_CDR(tfs))
        inner = L3(kp(PI_CWV), thunk(CADR(RT_CAR(l))), L3(kw(K_LAMBDA), RT_CAR(tfs), inner));
    return inner;
}
static obj rw_letstar_values(obj x) {
    obj bs;
    need(x, 3, -1, "let*-values");
    bs = CADR(x);
    if (list_len(bs) < 0) cerror(x, "malformed let*-values bindings");
    if (bs == RT_NIL || RT_CDR(bs) == RT_NIL) return rt_cons(kw(K_LET_VALUES), RT_CDR(x));
    return L3(kw(K_LET_VALUES), L1(RT_CAR(bs)), rt_cons(kw(K_LETSTAR_VALUES), rt_cons(RT_CDR(bs), CDDR(x))));
}
static obj rw_define_values(obj x) {
    obj pairs = RT_NIL, tf, defs = RT_NIL, sets = RT_NIL, l;
    need(x, 3, 3, "define-values");
    tf = temp_formals(CADR(x), &pairs, x);
    for (l = rt_reverse(pairs); l != RT_NIL; l = RT_CDR(l)) {
        defs = rt_cons(L3(kw(K_DEFINE), RT_CAR(RT_CAR(l)), unspec_form()), defs);
        sets = rt_cons(L3(kw(K_SET), RT_CAR(RT_CAR(l)), CADR(RT_CAR(l))), sets);
    }
    if (sets == RT_NIL) sets = L1(unspec_form());
    return rt_cons(kw(K_BEGIN), append2(rt_reverse(defs), L1(L3(kp(PI_CWV), thunk(CADDR(x)), rt_cons(kw(K_LAMBDA), rt_cons(tf, rt_reverse(sets)))))));
}

/* (shikra assume), SRFI 145: (assume e msg ...) = (let ((v e)) (if v v (%assume-failed (list msg ...)))).
 * %assume-failed never returns, so the analysis refines after it as after an if on e; with
 * --unsafe it is not emitted (DESIGN.md "Extensions as built"). */
static obj rw_assume(obj x) {
    obj v = fresh("v"), msgs = kquote(RT_NIL), l;
    if (need(x, 2, -1, "assume") < 2) cerror(x, "malformed assume: (assume expression message ...)");
    for (l = rt_reverse(CDDR(x)); l != RT_NIL; l = RT_CDR(l)) msgs = L3(kp(PI_rt_cons), RT_CAR(l), msgs);
    return L3(kw(K_LET), L1(L2(v, CADR(x))), L4(kw(K_IF), v, v, L2(kp(PI_rt_assume_failed), msgs)));
}
/* (shikra ffi): (define-c name "c-name" (arg-type ...) result-type) defines name as
 * (lambda (a ...) (%c-call '(name "c-name" (arg-type ...) result-type) a ...)). */
static const char *const ffi_types[] = {"int", "long", "double", "float", "bool", "char", "string", "pointer", "void", 0};
static int ffi_type(obj t) {
    int i;
    t = strip_syntax(t);
    if (!rt_is_symbol(t)) return -1;
    for (i = 0; ffi_types[i]; i++) if (t == sym(ffi_types[i])) return i;
    return -1;
}
static obj rw_define_c(obj x, obj *name) {
    obj l, params = RT_NIL, types = RT_NIL, call;
    if (list_len(x) != 5 || !is_ident(CADR(x)) || !rt_is_string(CADDR(x)) || list_len(CADDDR(x)) < 0)
        cerror(x, "malformed define-c: (define-c name \"c-name\" (arg-type ...) result-type)");
    if (RT_STR(CADDR(x))->bytes == 0) cerror(x, "define-c: empty C function name");
    for (l = CADDDR(x); l != RT_NIL; l = RT_CDR(l)) {
        int t = ffi_type(RT_CAR(l));
        if (t < 0) cerror(x, "define-c: unknown argument type %s (int long double float bool char string pointer)", dstr(RT_CAR(l)));
        if (t == 8) cerror(x, "define-c: void is only allowed as the result type");
        types = rt_cons(strip_syntax(RT_CAR(l)), types);
        params = rt_cons(fresh("a"), params);
    }
    if (ffi_type(RT_CAR(RT_CDR(CDDDR(x)))) < 0)
        cerror(x, "define-c: unknown result type %s (int long double float bool char string pointer void)", dstr(RT_CAR(RT_CDR(CDDDR(x)))));
    if (list_len(params) > 16) cerror(x, "define-c: at most 16 arguments");
    params = rt_reverse(params);
    *name = CADR(x);
    call = rt_cons(kp(PI_CCALL), rt_cons(kquote(L4(strip_id(CADR(x)), strip_syntax(CADDR(x)), rt_reverse(types),
                                                     strip_syntax(RT_CAR(RT_CDR(CDDDR(x)))))), params));
    return L3(kw(K_LAMBDA), params, setloc(call, x));
}

static obj include_forms(obj x, int ci) {
    obj l, out = RT_NIL;
    need(x, 2, -1, ci ? "include-ci" : "include");
    for (l = RT_CDR(x); l != RT_NIL; l = RT_CDR(l)) {
        if (!rt_is_string(RT_CAR(l))) cerror(x, "include expects file names (strings)");
        out = append2(out, read_source(resolve_include(x, RT_STR(RT_CAR(l))->buf), ci, x));
    }
    return out;
}

static obj expand_set(obj x, frame *env) {
    frame *where = NULL;
    binding *b;
    obj id;
    need(x, 3, 3, "set!");
    id = CADR(x);
    if (!is_ident(id)) cerror(x, "set!: expected an identifier, got %s", dstr(id));
    b = lookup_in(env, id, &where);
    if (!b) b = repl_autodefine(env, id), where = NULL;
    if (!b) cerror(x, "set! of unbound identifier %s", idname(id));
    if (b->kind == B_LOCAL) return setloc(L3(S_set, b->sym, expand_at(CDDR(x), env)), x);
    if (b->kind != B_GLOBAL) cerror(x, "set!: %s is not a variable", idname(id));
    if (where && where->lib != b->g->lib) cerror(x, "set! on imported binding %s (from %s)", idname(id), dstr(b->g->lib->name));
    b->g->assigned = 1;
    return setloc(L3(S_set, b->g->sym, expand_at(CDDR(x), env)), x);
}

static obj expand_core(int k, obj x, frame *env) {
    obj l;
    int n;
    switch (k) {
    case K_QUOTE: need(x, 2, 2, "quote"); return setloc(L2(S_quote, strip_syntax(CADR(x))), x);
    case K_LAMBDA:
        need(x, 3, -1, "lambda");
        return setloc(rt_cons(S_lambda, expand_clause(CADR(x), CDDR(x), env, x)), x);
    case K_CASE_LAMBDA: {
        obj out = RT_NIL;
        if (need(x, 2, -1, "case-lambda") < 2) cerror(x, "case-lambda needs at least one clause");
        for (l = RT_CDR(x); l != RT_NIL; l = RT_CDR(l)) {
            if (list_len(RT_CAR(l)) < 2) cerror(x, "malformed case-lambda clause %s", dstr(RT_CAR(l)));
            out = rt_cons(expand_clause(RT_CAR(RT_CAR(l)), RT_CDR(RT_CAR(l)), env, has_loc(l) ? l : x), out);
        }
        return setloc(rt_cons(S_case_lambda, rt_reverse(out)), x);
    }
    case K_IF:
        need(x, 3, 4, "if");
        return setloc(rt_cons(S_if, expand_list(RT_CDR(x), env)), x);
    case K_SET: return expand_set(x, env);
    case K_BEGIN:
        if (need(x, 1, -1, "begin") == 1) cerror(x, "empty (begin) in expression context");
        return setloc(rt_cons(S_begin, expand_list(RT_CDR(x), env)), x);
    case K_LETREC: case K_LETREC_STAR: {
        frame *f = new_frame(env);
        obj bs, syms = RT_NIL, out = RT_NIL;
        need(x, 3, -1, keyword_names[k]);
        bs = CADR(x);
        if (list_len(bs) < 0) cerror(x, "malformed %s bindings", keyword_names[k]);
        for (l = bs; l != RT_NIL; l = RT_CDR(l)) {
            if (list_len(RT_CAR(l)) != 2) cerror(has_loc(l) ? l : x, "malformed binding %s", dstr(RT_CAR(l)));
            syms = rt_cons(bind_local(f, RT_CAR(RT_CAR(l)), x, "duplicate binding of"), syms);
        }
        for (l = bs, syms = rt_reverse(syms); l != RT_NIL; l = RT_CDR(l), syms = RT_CDR(syms)) {
            obj saved = loc_ctx;
            if (has_loc(l)) loc_ctx = l;
            out = rt_cons(L2(RT_CAR(syms), expand_at(RT_CDR(RT_CAR(l)), f)), out);
            loc_ctx = saved;
        }
        if (out == RT_NIL) return expand_body(CDDR(x), f, x);
        return setloc(L3(S_letrec_star, rt_reverse(out), expand_body(CDDR(x), f, x)), x);
    }
    case K_LET_SYNTAX: case K_LETREC_SYNTAX: {
        frame *f = new_frame(env);
        need(x, 3, -1, keyword_names[k]);
        if (list_len(CADR(x)) < 0) cerror(x, "malformed %s bindings", keyword_names[k]);
        for (l = CADR(x); l != RT_NIL; l = RT_CDR(l)) {
            obj b = RT_CAR(l);
            binding *nb = new_binding(B_MACRO);
            if (list_len(b) != 2 || !is_ident(RT_CAR(b))) cerror(x, "malformed %s binding", keyword_names[k]);
            nb->mac = make_macro(CADR(b), k == K_LET_SYNTAX ? env : f, RT_CAR(b));
            pm_put(&f->map, RT_CAR(b), nb);
        }
        return expand_body(CDDR(x), f, x);
    }
    case K_LET: return expand(setloc(rw_let(x), x), env);
    case K_LETSTAR: return expand(setloc(rw_letstar(x), x), env);
    case K_COND: {
        obj r;
        need(x, 1, -1, "cond");
        r = rw_cond(RT_CDR(x), env, x);
        return expand(setloc(r == RT_UNDEF ? unspec_form() : r, x), env);
    }
    case K_CASE: return expand(setloc(rw_case(x, env), x), env);
    case K_AND:
        n = need(x, 1, -1, "and");
        if (n == 1) return L2(S_quote, RT_TRUE);
        if (n == 2) return expand_at(RT_CDR(x), env);
        return expand(setloc(L4(kw(K_IF), CADR(x), rt_cons(kw(K_AND), CDDR(x)), RT_FALSE), x), env);
    case K_OR: {
        obj t = fresh("t");
        n = need(x, 1, -1, "or");
        if (n == 1) return L2(S_quote, RT_FALSE);
        if (n == 2) return expand_at(RT_CDR(x), env);
        return expand(setloc(L3(kw(K_LET), L1(L2(t, CADR(x))), L4(kw(K_IF), t, t, rt_cons(kw(K_OR), CDDR(x)))), x), env);
    }
    case K_WHEN: case K_UNLESS:
        need(x, 3, -1, keyword_names[k]);
        if (k == K_WHEN) return expand(setloc(L3(kw(K_IF), CADR(x), rt_cons(kw(K_BEGIN), CDDR(x))), x), env);
        return expand(setloc(L4(kw(K_IF), CADR(x), unspec_form(), rt_cons(kw(K_BEGIN), CDDR(x))), x), env);
    case K_DO: return expand(setloc(rw_do(x), x), env);
    case K_QUASIQUOTE: need(x, 2, 2, "quasiquote"); return expand(setloc(qq(CADR(x), 1, env), x), env);
    case K_LET_VALUES: return expand(setloc(rw_let_values(x), x), env);
    case K_LETSTAR_VALUES: return expand(setloc(rw_letstar_values(x), x), env);
    case K_INCLUDE: case K_INCLUDE_CI: {
        obj forms = include_forms(x, k == K_INCLUDE_CI);
        if (forms == RT_NIL) cerror(x, "included files are empty");
        return expand(rt_cons(kw(K_BEGIN), forms), env);
    }
    case K_COND_EXPAND: {
        obj body = cond_expand_select(x);
        return expand(setloc(body == RT_NIL ? unspec_form() : rt_cons(kw(K_BEGIN), body), x), env);
    }
    case K_SYNTAX_ERROR: {
        obj p = rt_open_output_string();
        if (need(x, 2, -1, "syntax-error") < 2 || !rt_is_string(CADR(x))) cerror(x, "malformed syntax-error");
        rt_print(CADR(x), p, 0);
        for (l = CDDR(x); l != RT_NIL; l = RT_CDR(l)) { rt_port_putc(p, ' '); rt_print(strip_syntax(RT_CAR(l)), p, 1); }
        cerror(x, "%s", RT_STR(rt_get_output_string(p))->buf);
    }
    case K_ASSUME: return expand(setloc(rw_assume(x), x), env);
    case K_DEFINE: case K_DEFINE_SYNTAX: case K_DEFINE_RECORD_TYPE: case K_DEFINE_VALUES: case K_DEFINE_C:
        cerror(x, "%s is only allowed at the top level or at the beginning of a body", keyword_names[k]);
    case K_SYNTAX_RULES: cerror(x, "syntax-rules is only valid as a macro transformer");
    case K_FEATURES: need(x, 1, 1, "%features"); return setloc(L2(S_quote, rt_reverse(front_features())), x);
    case K_DELAY: case K_DELAY_FORCE: /* (scheme lazy), R7RS 4.2.5 and its reference implementation:
         * (delay-force e) = (%make-promise #f (lambda () e)), (delay e) = (delay-force (%make-promise #t e)) */
        need(x, 2, 2, keyword_names[k]);
        return expand(setloc(L3(kp(PI_rt_make_promise), RT_FALSE,
                                thunk(k == K_DELAY ? L3(kp(PI_rt_make_promise), RT_TRUE, CADR(x)) : CADR(x))), x), env);
    default: cerror(x, "invalid use of auxiliary syntax %s", keyword_names[k]);
    }
}

static obj expand1(obj x, frame *env) {
    int guard = 0;
    for (;;) {
        binding *b;
        if (is_ident(x)) return expand_ref(x, env);
        if (!rt_is_pair(x)) {
            if (x == RT_NIL) cerror(x, "empty application ()");
            if (self_evaluating(x)) return L2(S_quote, strip_syntax(x));
            cerror(x, "invalid expression %s", dstr(x));
        }
        b = is_ident(RT_CAR(x)) ? lookup(env, RT_CAR(x)) : NULL;
        if (b && b->kind == B_MACRO) {
            if (++guard > 100000) cerror(x, "macro expansion does not terminate");
            x = setloc(expand_macro(b->mac, x, env), x);
            continue;
        }
        if (b && b->kind == B_CORE) return expand_core(b->id, x, env);
        if (b && b->kind == B_PRIM) {
            const prim_info *p = &prim_table[b->id];
            int n = list_len(x) - 1;
            if (n < 0) cerror(x, "malformed application of %s", p->name);
            if (p->arity >= 0 && n != p->arity) cerror(x, "%s expects %d argument%s, given %d", p->name, p->arity, p->arity == 1 ? "" : "s", n);
            return setloc(rt_cons(sym(p->name), expand_list(RT_CDR(x), env)), x);
        }
        if (list_len(x) < 0) cerror(x, "malformed application (improper list)");
        return setloc(expand_list(x, env), x);
    }
}
static obj expand(obj x, frame *env) {
    obj saved = loc_ctx, r;
    if (has_loc(x)) loc_ctx = x;
    r = expand1(x, env);
    loc_ctx = saved;
    return r;
}

/* ---- bodies and top level: scan definitions, then expand ---- */
typedef struct item { int def; obj sym, form, src, ctx; struct item *next; } item;
typedef struct { item *head, **tail; frame *f; library *top; int quiet; } scanner;

static void add_item(scanner *s, int def, obj sym_, obj form, obj src) {
    item *it = NEW(item);
    it->def = def; it->sym = sym_; it->form = form; it->src = src; it->ctx = loc_ctx;
    *s->tail = it;
    s->tail = &it->next;
}
static obj define_name(scanner *s, obj id, obj src) {
    binding *old, *b;
    global *g;
    if (!is_ident(id)) cerror(src, "define: expected an identifier, got %s", dstr(id));
    if (!s->top) return bind_local(s->f, id, src, "duplicate definition of");
    old = (binding *)pm_get(&s->f->map, id);
    if (old && old->kind == B_GLOBAL && old->g->lib == s->top) {
        if (!s->f->open) cerror(src, "duplicate definition of %s", idname(id));
        old->g->defined++;
        return old->g->sym;
    }
    g = new_global(s->top, id);
    g->defined = 1;
    g->defloc = src;
    g->nowarn = s->quiet || id != strip_id(id); /* record procedures and macro-introduced names */
    b = new_binding(B_GLOBAL);
    b->g = g;
    pm_put(&s->f->map, id, b);
    return g->sym;
}
static void scan_form(scanner *s, obj x);
static void scan_list(scanner *s, obj l) {
    for (; rt_is_pair(l); l = RT_CDR(l)) {
        obj saved = loc_ctx;
        if (has_loc(l)) loc_ctx = l;
        scan_form(s, RT_CAR(l));
        loc_ctx = saved;
    }
}
static void scan_form(scanner *s, obj x) {
    obj saved = loc_ctx;
    binding *b = NULL;
    if (has_loc(x)) loc_ctx = x;
    for (;;) {
        b = rt_is_pair(x) && is_ident(RT_CAR(x)) ? lookup(s->f, RT_CAR(x)) : NULL;
        if (!b || b->kind != B_MACRO) break;
        x = setloc(expand_macro(b->mac, x, s->f), x);
    }
    if (b && b->kind == B_CORE) switch (b->id) {
        case K_BEGIN: need(x, 1, -1, "begin"); scan_list(s, RT_CDR(x)); goto done;
        case K_DEFINE: {
            int n = need(x, 2, -1, "define");
            obj target = CADR(x);
            if (rt_is_pair(target)) {
                if (n < 3) cerror(x, "malformed define: missing body");
                add_item(s, 1, define_name(s, RT_CAR(target), x), setloc(rt_cons(kw(K_LAMBDA), rt_cons(RT_CDR(target), CDDR(x))), x), x);
            } else {
                if (n > 3) cerror(x, "malformed define");
                add_item(s, 1, define_name(s, target, x), n == 3 ? CADDR(x) : unspec_form(), x);
            }
            goto done;
        }
        case K_DEFINE_SYNTAX: {
            binding *nb = new_binding(B_MACRO);
            if (need(x, 3, 3, "define-syntax") && !is_ident(CADR(x))) cerror(x, "define-syntax: expected an identifier");
            if (!s->top && pm_get(&s->f->map, CADR(x))) cerror(x, "duplicate definition of %s", idname(CADR(x)));
            nb->mac = make_macro(CADDR(x), s->f, CADR(x));
            pm_put(&s->f->map, CADR(x), nb);
            goto done;
        }
        case K_DEFINE_RECORD_TYPE: { /* its procedures are never reported as unused */
            int q = s->quiet;
            s->quiet = 1;
            scan_form(s, setloc(rw_record(x), x));
            s->quiet = q;
            goto done;
        }
        case K_DEFINE_VALUES: scan_form(s, setloc(rw_define_values(x), x)); goto done;
        case K_DEFINE_C: {
            obj name, lam = setloc(rw_define_c(x, &name), x);
            add_item(s, 1, define_name(s, name, x), lam, x);
            goto done;
        }
        case K_INCLUDE: case K_INCLUDE_CI: scan_list(s, include_forms(x, b->id == K_INCLUDE_CI)); goto done;
        case K_COND_EXPAND: scan_list(s, cond_expand_select(x)); goto done;
        default: break;
        }
    add_item(s, 0, NULL, x, x);
done:
    loc_ctx = saved;
}

static obj expand_item(item *it, frame *env) {
    obj saved = loc_ctx, r;
    loc_ctx = has_loc(it->src) ? it->src : it->ctx;
    r = expand(it->form, env);
    loc_ctx = saved;
    return r;
}

static obj expand_body(obj body, frame *env, obj src) {
    scanner s = {0};
    item *it, *lastdef = NULL;
    obj binds = RT_NIL, exprs = RT_NIL;
    s.head = NULL; s.tail = &s.head; s.f = new_frame(env); s.top = NULL;
    if (list_len(body) < 0) cerror(src, "malformed body");
    scan_list(&s, body);
    if (!s.head) cerror(src, "empty body");
    for (it = s.head; it; it = it->next) if (it->def) lastdef = it;
    if (lastdef && !lastdef->next) cerror(lastdef->src, "body ends with a definition (no expression follows)");
    for (it = s.head; it; it = it->next) {
        obj e = expand_item(it, s.f);
        if (lastdef && it == lastdef->next) lastdef = NULL; /* past the last definition */
        if (it->def) binds = rt_cons(setloc(L2(it->sym, e), it->src), binds);
        else if (lastdef) binds = rt_cons(L2(new_local_sym(sym("_")), e), binds);
        else exprs = rt_cons(e, exprs);
    }
    exprs = rt_reverse(exprs);
    exprs = RT_CDR(exprs) == RT_NIL ? RT_CAR(exprs) : rt_cons(S_begin, exprs);
    if (binds == RT_NIL) return exprs;
    return setloc(L3(S_letrec_star, rt_reverse(binds), exprs), src);
}

void expand_toplevel(library *lib, obj forms) {
    scanner s = {0};
    item *it;
    obj out = RT_NIL;
    s.head = NULL; s.tail = &s.head; s.f = lib->env; s.top = lib;
    scan_list(&s, forms);
    for (it = s.head; it; it = it->next) {
        obj e = expand_item(it, lib->env);
        out = rt_cons(it->def ? setloc(L3(S_define, it->sym, e), it->src) : e, out);
    }
    lib->core = append2(lib->core, rt_reverse(out));
}

/* ---- unparse (,expand / shikra debug-expand) ---- */
static void up(obj x, obj port, int raw) {
    if (rt_is_symbol(x)) {
        if (!raw) {
            global *g = front_global(x);
            obj n = g ? g->name : front_local_name(x);
            if (n) x = n;
        }
        rt_print(x, port, 1);
        return;
    }
    if (rt_is_pair(x)) {
        if (RT_CAR(x) == S_quote && list_len(x) == 2) {
            if (!self_evaluating(CADR(x))) rt_port_putc(port, '\'');
            rt_print(CADR(x), port, 1);
            return;
        }
        rt_port_putc(port, '(');
        for (;;) {
            up(RT_CAR(x), port, raw);
            x = RT_CDR(x);
            if (!rt_is_pair(x)) break;
            rt_port_putc(port, ' ');
        }
        if (x != RT_NIL) { rt_port_puts(port, " . "); up(x, port, raw); }
        rt_port_putc(port, ')');
        return;
    }
    rt_print(x, port, 1);
}
void unparse(obj x, obj port, int raw) { up(x, port, raw); }
