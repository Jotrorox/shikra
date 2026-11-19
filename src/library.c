/* library.c -- pass 2 (spec §4.3, §5; DESIGN.md §6): define-library, import sets, export,
 * include / include-ci / include-library-declarations, cond-expand and features, the
 * library graph (cycle detection, initialisation order), the hidden library
 * (shikra primitives), plus the front end's shared utilities: pointer maps, diagnostics
 * with source locations, reading source files (disk or embedded). */
#include "front.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

arena compiler_arena;
library *front_order;
frame *core_env;
obj loc_ctx;
static front_opts opts;
static htab libs; /* id -> library* */
static library *order_tail;
static obj features; /* list of symbols */
static library *loading[64]; /* libraries being loaded, innermost last (REPL error recovery) */
static int nloading;
void (*cerror_hook)(const char *loc, const char *msg);

const prim_info prim_table[NPRIMS_ALL] = {
#define PRIM(name, who, cfn, arity, args, res, flags) {name, who, #cfn, arity, args, res, flags},
#include "prims.def"
#undef PRIM
    {"%apply", "apply", 0, 2, "xl", 'a', PF_FAIL | PF_EFFECT},
    {"%values", "values", 0, -1, "", 'a', PF_ALLOC},
    {"%call-with-values", "call-with-values", 0, 2, "xx", 'a', PF_FAIL | PF_EFFECT},
    {"%call/cc", "call-with-current-continuation", 0, 1, "x", 'a', PF_FAIL | PF_EFFECT},
    {"%eval", "eval", 0, 2, "aa", 'a', PF_FAIL | PF_EFFECT},
    {"%environment", "environment", 0, 1, "l", 'a', PF_FAIL | PF_EFFECT},
    {"%interaction-environment", "interaction-environment", 0, 0, "", 'a', 0},
};

const char *const keyword_names[NKEYWORDS] = {
    "define", "lambda", "if", "quote", "set!", "begin", "let", "let*", "letrec", "letrec*",
    "cond", "case", "and", "or", "when", "unless", "do", "quasiquote", "define-syntax",
    "let-syntax", "letrec-syntax", "syntax-rules", "syntax-error", "define-record-type",
    "case-lambda", "let-values", "let*-values", "define-values", "include", "include-ci",
    "cond-expand", "delay", "delay-force", "%features",
    "else", "=>", "...", "_", "unquote", "unquote-splicing"};

/* ---- pointer map ---- */
static size_t pm_hash(const void *k) { return (size_t)(((uintptr_t)k >> 3) * (uintptr_t)0x9E3779B97F4A7C15u); }
void *pm_get(const pmap *m, const void *key) {
    size_t i;
    if (!m->cap) return NULL;
    for (i = pm_hash(key) & (m->cap - 1); m->k[i]; i = (i + 1) & (m->cap - 1))
        if (m->k[i] == key) return m->v[i];
    return NULL;
}
void pm_put(pmap *m, const void *key, void *val) {
    size_t i;
    if (2 * (m->n + 1) > m->cap) {
        size_t oc = m->cap, j;
        const void **ok = m->k;
        void **ov = m->v;
        m->cap = oc ? 2 * oc : 8;
        m->k = (const void **)calloc(m->cap, sizeof(void *));
        m->v = (void **)calloc(m->cap, sizeof(void *));
        if (!m->k || !m->v) die("out of memory");
        for (j = 0; j < oc; j++)
            if (ok[j]) {
                for (i = pm_hash(ok[j]) & (m->cap - 1); m->k[i]; i = (i + 1) & (m->cap - 1)) {}
                m->k[i] = ok[j];
                m->v[i] = ov[j];
            }
        free(ok);
        free(ov);
    }
    for (i = pm_hash(key) & (m->cap - 1); m->k[i] && m->k[i] != key; i = (i + 1) & (m->cap - 1)) {}
    if (!m->k[i]) m->n++;
    m->k[i] = key;
    m->v[i] = val;
}

/* ---- diagnostics ---- */
static int get_loc(obj x, obj *f, intptr_t *l, intptr_t *c) {
    if (rt_is_pair(x) && rt_srcloc_lookup(x, f, l, c)) return 1;
    return loc_ctx && rt_is_pair(loc_ctx) && rt_srcloc_lookup(loc_ctx, f, l, c);
}
int has_loc(obj x) {
    obj f;
    intptr_t l, c;
    return rt_is_pair(x) && rt_srcloc_lookup(x, &f, &l, &c);
}
obj setloc(obj dst, obj src) {
    obj f;
    intptr_t l, c;
    if (rt_is_pair(dst) && !has_loc(dst) && get_loc(src, &f, &l, &c)) rt_srcloc_set(dst, f, l, c);
    return dst;
}
const char *loc_file(obj x) {
    obj f;
    intptr_t l, c;
    return get_loc(x, &f, &l, &c) ? RT_STR(f)->buf : NULL;
}
void cerror(obj where, const char *fmt, ...) {
    obj f;
    intptr_t l, c;
    va_list ap;
    if (cerror_hook) {
        char loc[512], msg[1024];
        int located = get_loc(where, &f, &l, &c);
        if (located) snprintf(loc, sizeof loc, "%s:%ld:%ld", RT_STR(f)->buf, (long)l, (long)c);
        va_start(ap, fmt);
        vsnprintf(msg, sizeof msg, fmt, ap);
        va_end(ap);
        cerror_hook(located ? loc : NULL, msg);
    }
    rt_flush(rt_stdout);
    fflush(stdout);
    if (get_loc(where, &f, &l, &c)) fprintf(stderr, "%s:%ld:%ld: ", RT_STR(f)->buf, (long)l, (long)c);
    fputs("error: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}
sbuf *front_warn_capture;
void cwarning(obj where, const char *fmt, ...) {
    obj f;
    intptr_t l, c;
    va_list ap;
    char msg[1024];
    rt_flush(rt_stdout);
    fflush(stdout);
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (get_loc(where, &f, &l, &c)) fprintf(stderr, "%s:%ld:%ld: ", RT_STR(f)->buf, (long)l, (long)c);
    fprintf(stderr, "warning: %s\n", msg);
    if (front_warn_capture) { /* shikra run replays the warnings of a cached build */
        if (get_loc(where, &f, &l, &c)) sb_printf(front_warn_capture, "%s:%ld:%ld: ", RT_STR(f)->buf, (long)l, (long)c);
        sb_printf(front_warn_capture, "warning: %s\n", msg);
    }
}
const char *idname(obj id) { return RT_STR(RT_PTR(strip_id(id), rt_symbol)->name)->buf; }

/* A runtime error raised inside the compiler (reader errors, or a primitive applied to a
 * malformed datum) becomes a located compile error. */
static void compile_raise(obj e) {
    obj p = rt_open_output_string(), irr;
    if (rt_is_type(e, RT_T_ERROR)) {
        if (RT_VEC(e)[2] != RT_FALSE) { /* reader errors carry their own location */
            rt_flush(rt_stdout);
            fprintf(stderr, "%s: error: %s\n", RT_STR(RT_VEC(e)[2])->buf, RT_STR(RT_VEC(e)[0])->buf);
            exit(1);
        }
        rt_print(RT_VEC(e)[0], p, 0);
        for (irr = RT_VEC(e)[1]; rt_is_pair(irr); irr = RT_CDR(irr)) {
            rt_port_putc(p, ' ');
            rt_print(strip_syntax(RT_CAR(irr)), p, 1);
        }
    } else rt_print(e, p, 1);
    cerror(RT_FALSE, "%s", RT_STR(rt_get_output_string(p))->buf);
}
void compile_errors_init(void) { rt_raise_hook = compile_raise; }

/* ---- reading sources ---- */
obj read_source(const char *path, int foldcase, obj where) {
    size_t n;
    char *text;
    obj port, x, l = RT_NIL, last = RT_NIL;
    if (!strncmp(path, "<builtin>/", 10)) {
        sbuf b = {0};
        sb_printf(&b, "lib/%s", path + 10);
        text = embed_text(b.s, &n);
        sb_free(&b);
    } else text = read_file(path, &n);
    if (!text) cerror(where, "cannot read %s", path);
    cache_note_file(path, text, n);
    port = rt_open_input_string(rt_make_string_n(text, n));
    free(text);
    RT_PORT(port)->name = rt_cstring(path);
    if (foldcase) RT_SETFLAGS(port, RT_HFLAGS(port) | RT_PF_FOLDCASE);
    for (;;) { /* each list cell gets the position of its datum (top-level atoms too) */
        intptr_t line, col;
        int c;
        obj cell;
        while ((c = rt_port_peekc(port)) == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == ';')
            if (rt_port_getc(port) == ';') while ((c = rt_port_getc(port)) >= 0 && c != '\n') {}
        line = RT_PORT(port)->line;
        col = RT_PORT(port)->col + 1;
        if ((x = rt_read(port)) == RT_EOF) break;
        cell = rt_cons(x, RT_NIL);
        rt_srcloc_set(cell, RT_PORT(port)->name, line, col);
        if (l == RT_NIL) l = cell;
        else RT_CDR(last) = cell;
        last = cell;
    }
    return l;
}

/* Path of an included file, relative to the file containing `where`. */
const char *resolve_include(obj where, const char *name) {
    const char *base = loc_file(where), *slash;
    sbuf b = {0};
    if (name[0] == '/' || !base || !(slash = strrchr(base, '/'))) return arena_strdup(&compiler_arena, name);
    sb_putn(&b, base, (size_t)(slash - base + 1));
    sb_puts(&b, name);
    name = arena_strdup(&compiler_arena, b.s);
    sb_free(&b);
    return name;
}

/* ---- features and cond-expand (spec §3.4) ---- */
static void add_feature(const char *s) { features = rt_cons(sym(s), features); }
static void init_features(void) {
    const char *t = opts.target;
    int darwin, linux_, x64, a64;
#if defined(__APPLE__)
    darwin = 1, linux_ = 0;
#elif defined(__linux__)
    darwin = 0, linux_ = 1;
#else
    darwin = 0, linux_ = 0;
#endif
#if defined(__x86_64__) || defined(_M_X64)
    x64 = 1, a64 = 0;
#elif defined(__aarch64__) || defined(__arm64__)
    x64 = 0, a64 = 1;
#else
    x64 = 0, a64 = 0;
#endif
    if (t) {
        darwin = strstr(t, "darwin") || strstr(t, "apple") || strstr(t, "macos");
        linux_ = strstr(t, "linux") != NULL;
        x64 = !strncmp(t, "x86_64", 6) || !strncmp(t, "amd64", 5);
        a64 = !strncmp(t, "aarch64", 7) || !strncmp(t, "arm64", 5);
    }
    features = RT_NIL;
    add_feature("r7rs");
    add_feature("shikra");
    add_feature("ieee-float");
    if (opts.unicode_full) add_feature("full-unicode");
    if (darwin) { add_feature("darwin"); add_feature("macos"); }
    if (linux_) { add_feature("linux"); add_feature("gnu-linux"); }
    if (darwin || linux_) { add_feature("unix"); add_feature("posix"); }
    if (x64) add_feature("x86-64");
    if (a64) { add_feature("arm64"); add_feature("aarch64"); }
    if (x64 || a64) { add_feature("little-endian"); add_feature("lp64"); }
}

obj front_features(void) { return features; } /* (%features) expands to this list, quoted */

static const char *libname_id(obj name, obj where);
static int requirement(obj r) {
    obj h, l;
    if (is_ident(r)) {
        for (l = features; l != RT_NIL; l = RT_CDR(l)) if (RT_CAR(l) == strip_id(r)) return 1;
        return 0;
    }
    if (list_len(r) < 1 || !is_ident(RT_CAR(r))) cerror(r, "malformed cond-expand requirement");
    h = strip_id(RT_CAR(r));
    if (h == sym("and")) {
        for (l = RT_CDR(r); l != RT_NIL; l = RT_CDR(l)) if (!requirement(RT_CAR(l))) return 0;
        return 1;
    }
    if (h == sym("or")) {
        for (l = RT_CDR(r); l != RT_NIL; l = RT_CDR(l)) if (requirement(RT_CAR(l))) return 1;
        return 0;
    }
    if (h == sym("not") && list_len(r) == 2) return !requirement(RT_CAR(RT_CDR(r)));
    if (h == sym("library") && list_len(r) == 2) {
        int e = library_exists(strip_syntax(RT_CAR(RT_CDR(r))));
        cache_note_exists(libname_id(strip_syntax(RT_CAR(RT_CDR(r))), r), e);
        return e;
    }
    cerror(r, "malformed cond-expand requirement");
}
obj cond_expand_select(obj form) {
    obj cl;
    if (list_len(form) < 1) cerror(form, "malformed cond-expand");
    for (cl = RT_CDR(form); cl != RT_NIL; cl = RT_CDR(cl)) {
        obj c = RT_CAR(cl);
        if (list_len(c) < 1) cerror(form, "malformed cond-expand clause");
        if ((is_ident(RT_CAR(c)) && strip_id(RT_CAR(c)) == sym("else")) || requirement(RT_CAR(c))) return RT_CDR(c);
    }
    return RT_NIL;
}

/* ---- libraries ---- */
static library *new_library(obj name, const char *id, const char *path) {
    library *lib = NEW(library);
    lib->name = name;
    lib->id = id;
    lib->path = path;
    lib->env = new_frame(NULL);
    lib->env->lib = lib;
    lib->export_specs = lib->body = lib->core = RT_NIL;
    return lib;
}
static void add_to_order(library *lib) {
    if (order_tail) order_tail->next = lib;
    else front_order = lib;
    order_tail = lib;
}

/* (geometry vec) -> "geometry/vec" */
static const char *libname_id(obj name, obj where) {
    sbuf b = {0};
    const char *r;
    obj l;
    if (list_len(name) < 1) cerror(where, "malformed library name %s", dstr(name));
    for (l = name; l != RT_NIL; l = RT_CDR(l)) {
        obj c = strip_syntax(RT_CAR(l));
        if (b.len) sb_putc(&b, '/');
        if (rt_is_symbol(c)) sb_puts(&b, RT_STR(RT_PTR(c, rt_symbol)->name)->buf);
        else if (rt_is_fix(c) && RT_FIXVAL(c) >= 0) sb_printf(&b, "%ld", (long)RT_FIXVAL(c));
        else cerror(where, "malformed library name %s", dstr(name));
    }
    r = arena_strdup(&compiler_arena, b.s);
    sb_free(&b);
    return r;
}

static int file_readable(const char *p) {
    FILE *f = fopen(p, "rb");
    if (f) fclose(f);
    return f != NULL;
}
/* Search order (spec §4.3): program directory, each -I directory, the built-ins. */
static const char *find_library(const char *id) {
    sbuf b = {0};
    int i;
    char *t;
    for (i = -1; i < opts.nincdirs; i++) {
        const char *dir = i < 0 ? opts.progdir : opts.incdirs[i];
        b.len = 0;
        sb_printf(&b, "%s/%s.sld", dir && *dir ? dir : ".", id);
        if (file_readable(b.s)) { const char *r = arena_strdup(&compiler_arena, b.s); sb_free(&b); return r; }
    }
    b.len = 0;
    sb_printf(&b, "lib/%s.sld", id);
    t = embed_text(b.s, NULL);
    sb_free(&b);
    if (!t) return NULL;
    free(t);
    b.len = 0;
    sb_printf(&b, "<builtin>/%s.sld", id);
    return arena_strdup(&compiler_arena, b.s);
}
/* The evaluator libraries exist only in the interpreter (spec §3.1). */
static int repl_only(const char *id) {
    return !strcmp(id, "scheme/eval") || !strcmp(id, "scheme/load") || !strcmp(id, "scheme/repl");
}
int front_lib_exists(const char *id) {
    if (!opts.repl && repl_only(id)) return 0;
    return ht_get(&libs, id) || find_library(id);
}
int library_exists(obj name) { return front_lib_exists(libname_id(name, name)); }

static imp *new_imp(obj name, binding *b, obj loc, imp *next) {
    imp *i = NEW(imp);
    i->name = name; i->b = b; i->loc = loc; i->next = next;
    return i;
}
static library *load_library(obj name, obj where);

static int is_word(obj x, const char *w) { return is_ident(x) && strip_id(x) == sym(w); }

/* An import set -> list of (name, binding). *from receives the underlying library. */
static imp *import_set(obj spec, library **from) {
    imp *in, *out = NULL, *i, **pp;
    obj l, h;
    if (list_len(spec) >= 2 && rt_is_pair(RT_CAR(RT_CDR(spec))) && is_ident(h = RT_CAR(spec)) &&
        (is_word(h, "only") || is_word(h, "except") || is_word(h, "prefix") || is_word(h, "rename"))) {
        in = import_set(RT_CAR(RT_CDR(spec)), from);
        if (is_word(h, "prefix")) {
            obj p;
            if (list_len(spec) != 3 || !is_ident(p = RT_CAR(RT_CDR(RT_CDR(spec))))) cerror(spec, "malformed prefix import set");
            for (i = in; i; i = i->next) {
                sbuf b = {0};
                sb_printf(&b, "%s%s", idname(p), idname(i->name));
                out = new_imp(sym(b.s), i->b, spec, out);
                sb_free(&b);
            }
            return out;
        }
        for (l = RT_CDR(RT_CDR(spec)); rt_is_pair(l); l = RT_CDR(l)) { /* every named id must exist */
            obj id = RT_CAR(l), from_id = id;
            if (is_word(h, "rename")) {
                if (list_len(id) != 2 || !is_ident(RT_CAR(id)) || !is_ident(RT_CAR(RT_CDR(id)))) cerror(l, "malformed rename import set");
                from_id = RT_CAR(id);
            } else if (!is_ident(id)) cerror(l, "malformed %s import set", idname(h));
            for (i = in; i && i->name != strip_id(from_id); i = i->next) {}
            if (!i) cerror(l, "%s does not export %s", (*from)->name == RT_FALSE ? "the import set" : dstr((*from)->name), idname(from_id));
        }
        for (i = in; i; i = i->next) {
            obj newname = i->name;
            int named = 0;
            for (l = RT_CDR(RT_CDR(spec)); rt_is_pair(l); l = RT_CDR(l)) {
                obj id = RT_CAR(l);
                if (is_word(h, "rename") ? strip_id(RT_CAR(id)) == i->name : strip_id(id) == i->name) {
                    named = 1;
                    if (is_word(h, "rename")) newname = strip_id(RT_CAR(RT_CDR(id)));
                }
            }
            if (is_word(h, "only") ? named : is_word(h, "except") ? !named : 1) out = new_imp(newname, i->b, spec, out);
        }
        return out;
    }
    *from = load_library(spec, spec);
    cache_note_dep(*from);
    for (pp = &out, i = (*from)->exports; i; i = i->next, pp = &(*pp)->next) *pp = new_imp(i->name, i->b, spec, NULL);
    return out;
}

/* Unused-import warnings (spec §11.3): each import set of the program or of a user library
 * gets a record; every identifier it binds remembers it, and lookup marks it used. */
int front_builtin(library *lib) { return !strncmp(lib->path, "<builtin>", 9); }
void front_note_use(library *lib, obj id, binding *b) {
    imp_use *u;
    for (u = (imp_use *)pm_get(&lib->imported, id); u; u = u->next)
        if (u->b == b) u->rec->used = 1;
}
void front_add_import_use(library *lib, obj id, import_rec *rec, binding *b) {
    imp_use *u = NEW(imp_use);
    u->rec = rec;
    u->b = b;
    u->next = (imp_use *)pm_get(&lib->imported, id);
    pm_put(&lib->imported, id, u);
}
static void import_into(frame *env, obj spec) {
    library *from = NULL;
    imp *i = import_set(spec, &from);
    import_rec *rec = NULL;
    library *lib = env->lib;
    if (lib && !env->open && !front_builtin(lib)) {
        import_rec **pp = &lib->imports;
        while (*pp) pp = &(*pp)->next;
        *pp = rec = NEW(import_rec);
        rec->spec = spec;
    }
    for (; i; i = i->next) {
        binding *old = (binding *)pm_get(&env->map, i->name);
        if (old && old != i->b && !env->open) cerror(spec, "%s is imported twice with different bindings", idname(i->name));
        pm_put(&env->map, i->name, i->b);
        if (rec) front_add_import_use(lib, i->name, rec, i->b);
    }
}
void front_warn_imports(void) {
    library *l;
    import_rec *r;
    for (l = front_order; l; l = l->next)
        for (r = l->imports; r; r = r->next)
            if (!r->used) cwarning(r->spec, "unused import %s", dstr(r->spec));
}

static void resolve_exports(library *lib) {
    obj l;
    imp **pp = &lib->exports;
    for (l = lib->export_specs; l != RT_NIL; l = RT_CDR(l)) {
        obj s = RT_CAR(l), in = RT_CAR(s), ext = RT_CAR(RT_CDR(s)), where = RT_CDR(RT_CDR(s));
        binding *b = lookup(lib->env, in);
        if (!b) cerror(where, "exported identifier %s is not defined in library %s", idname(in), dstr(lib->name));
        *pp = new_imp(strip_id(ext), b, where, NULL);
        pp = &(*pp)->next;
    }
}

static void process_decls(library *lib, obj decls) {
    obj d, l;
    for (; rt_is_pair(decls); decls = RT_CDR(decls)) {
        obj saved = loc_ctx;
        d = RT_CAR(decls);
        if (has_loc(d)) loc_ctx = d;
        if (list_len(d) < 1 || !is_ident(RT_CAR(d))) cerror(d, "malformed library declaration");
        if (is_word(RT_CAR(d), "export")) {
            for (l = RT_CDR(d); l != RT_NIL; l = RT_CDR(l)) {
                obj s = RT_CAR(l), where = has_loc(l) ? l : d;
                if (is_ident(s)) lib->export_specs = rt_cons(rt_cons(s, rt_cons(s, where)), lib->export_specs);
                else if (list_len(s) == 3 && is_word(RT_CAR(s), "rename") && is_ident(RT_CAR(RT_CDR(s))) && is_ident(RT_CAR(RT_CDR(RT_CDR(s)))))
                    lib->export_specs = rt_cons(rt_cons(RT_CAR(RT_CDR(s)), rt_cons(RT_CAR(RT_CDR(RT_CDR(s))), where)), lib->export_specs);
                else cerror(where, "malformed export spec %s", dstr(s));
            }
        } else if (is_word(RT_CAR(d), "import")) {
            for (l = RT_CDR(d); l != RT_NIL; l = RT_CDR(l)) import_into(lib->env, RT_CAR(l));
        } else if (is_word(RT_CAR(d), "begin")) {
            lib->body = append2(lib->body, RT_CDR(d));
        } else if (is_word(RT_CAR(d), "include") || is_word(RT_CAR(d), "include-ci") || is_word(RT_CAR(d), "include-library-declarations")) {
            int ci = is_word(RT_CAR(d), "include-ci"), decl = is_word(RT_CAR(d), "include-library-declarations");
            for (l = RT_CDR(d); l != RT_NIL; l = RT_CDR(l)) {
                obj forms;
                if (!rt_is_string(RT_CAR(l))) cerror(d, "%s expects file names (strings)", idname(RT_CAR(d)));
                forms = read_source(resolve_include(d, RT_STR(RT_CAR(l))->buf), ci, d);
                if (decl) process_decls(lib, forms);
                else lib->body = append2(lib->body, forms);
            }
        } else if (is_word(RT_CAR(d), "cond-expand")) {
            process_decls(lib, cond_expand_select(d));
        } else cerror(d, "unknown library declaration %s", idname(RT_CAR(d)));
        loc_ctx = saved;
    }
}

static library *load_library(obj name, obj where) {
    const char *id = libname_id(name, where), *path;
    library *lib = (library *)ht_get(&libs, id);
    obj forms, dl = RT_FALSE, saved = loc_ctx;
    if (lib) {
        if (lib->state == 1) cerror(where, "library cycle: %s imports itself (directly or indirectly)", dstr(name));
        return lib;
    }
    if (!opts.repl && repl_only(id))
        cerror(where, "library %s exists only in the REPL (shikra repl): compiled programs cannot use eval, load or interaction-environment", dstr(name));
    if (nloading == 64) cerror(where, "libraries nested too deeply");
    if (!(path = find_library(id))) cerror(where, "library %s not found", dstr(name));
    lib = new_library(strip_syntax(name), id, path);
    lib->state = 1;
    ht_put(&libs, id, lib);
    loading[nloading++] = lib;
    if (!cache_load(lib)) { /* not in the expansion cache (or stale): read and expand it */
        for (forms = read_source(path, 0, where); forms != RT_NIL; forms = RT_CDR(forms)) {
            obj f = RT_CAR(forms);
            if (list_len(f) >= 2 && is_word(RT_CAR(f), "define-library") && rt_equal(strip_syntax(RT_CAR(RT_CDR(f))), strip_syntax(name))) dl = f;
        }
        if (dl == RT_FALSE) cerror(where, "%s does not define library %s", path, dstr(name));
        loc_ctx = dl;
        process_decls(lib, RT_CDR(RT_CDR(dl)));
        lib->export_specs = rt_reverse(lib->export_specs);
        cache_begin_expand(lib);
        expand_toplevel(lib, lib->body);
        resolve_exports(lib);
        cache_store(lib);
    }
    nloading--;
    lib->state = 2;
    add_to_order(lib);
    loc_ctx = saved;
    return lib;
}

/* The hidden library: core keywords, auxiliary syntax and every %primitive. */
static void make_primitive_library(void) {
    library *lib = new_library(L2(sym("shikra"), sym("primitives")), "shikra/primitives", "<builtin>");
    int i;
    imp **pp = &lib->exports;
    for (i = 0; i < NKEYWORDS + NPRIMS_ALL; i++) {
        binding *b = new_binding(i < NKEYWORDS ? B_CORE : B_PRIM);
        obj name = sym(i < NKEYWORDS ? keyword_names[i] : prim_table[i - NKEYWORDS].name);
        b->id = i < NKEYWORDS ? i : i - NKEYWORDS;
        pm_put(&lib->env->map, name, b);
        *pp = new_imp(name, b, RT_FALSE, NULL);
        pp = &(*pp)->next;
    }
    lib->state = 2;
    core_env = lib->env;
    front_core_lib = lib;
    ht_put(&libs, lib->id, lib);
}
library *front_core_lib;
library *front_library(const char *id) { return (library *)ht_get(&libs, id); }
library *front_loading(void) { return nloading ? loading[nloading - 1] : NULL; }
library *front_find_dep(const char *id) { /* (a b) from "a/b"; components of digits are numbers */
    obj name = RT_NIL;
    const char *e = id + strlen(id), *s;
    while (e > id) {
        for (s = e; s > id && s[-1] != '/'; s--) {}
        if (strspn(s, "0123456789") == (size_t)(e - s) && e - s < 15) name = rt_cons(RT_FIX(atol(s)), name);
        else name = rt_cons(rt_intern(s, (size_t)(e - s)), name);
        e = s > id ? s - 1 : s;
    }
    if (!library_exists(name)) return NULL;
    return load_library(name, RT_FALSE);
}

void front_init(const front_opts *o) {
    opts = *o;
    rt_srcloc_enable(1);
    S_define = sym("define"); S_lambda = sym("lambda"); S_if = sym("if"); S_quote = sym("quote");
    S_set = sym("set!"); S_letrec_star = sym("letrec*"); S_begin = sym("begin"); S_case_lambda = sym("%case-lambda");
    init_features();
    make_primitive_library();
    cache_init(o);
}

/* A program: import declarations first (cond-expand among them is spliced), then the body. */
library *front_load_program(const char *path) {
    obj forms = read_source(path, 0, RT_FALSE), l;
    library *prog = new_library(RT_FALSE, "main", path);
    prog->state = 1;
    while (rt_is_pair(forms)) {
        obj f = RT_CAR(forms);
        if (list_len(f) >= 1 && RT_CAR(f) == sym("import")) {
            loc_ctx = f;
            for (l = RT_CDR(f); l != RT_NIL; l = RT_CDR(l)) import_into(prog->env, RT_CAR(l));
            forms = RT_CDR(forms);
        } else if (list_len(f) >= 1 && RT_CAR(f) == sym("cond-expand")) {
            loc_ctx = f;
            forms = append2(cond_expand_select(f), RT_CDR(forms));
        } else break;
    }
    loc_ctx = RT_FALSE;
    expand_toplevel(prog, forms);
    prog->state = 2;
    add_to_order(prog);
    return prog;
}

/* ---- REPL entry points (used from M3) ---- */
library *front_repl_new(void) {
    library *lib = new_library(RT_FALSE, "repl", "<repl>");
    lib->env->open = 1;
    lib->state = 2;
    return lib;
}
void front_repl_import(library *top, obj form) {
    obj l;
    if (list_len(form) < 1) cerror(form, "malformed import");
    for (l = RT_CDR(form); l != RT_NIL; l = RT_CDR(l)) import_into(top->env, RT_CAR(l));
}
void front_repl_recover(void) {
    while (nloading > 0) ht_put(&libs, loading[--nloading]->id, NULL);
    loc_ctx = RT_FALSE;
}
obj front_repl_expand(library *top, obj form) {
    top->core = RT_NIL;
    expand_toplevel(top, rt_cons(form, RT_NIL));
    return top->core;
}
