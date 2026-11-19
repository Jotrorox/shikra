/* cache.c -- the expansion cache (spec §4.2 "Expanded libraries are cached on disk", §14).
 *
 * One file per library and configuration in <cache>/expand/: the library's expanded core
 * forms, its top-level frame (bindings: its globals and syntax-rules macros, imports by
 * export name), exports, globals, import records (unused-import warnings) and the source
 * locations of every pair, plus what it changed in other libraries while it was expanded
 * (assigned flags, used imports) and how many local symbols it numbered. An entry is used
 * only if everything that could change it is unchanged:
 *   - the file name: hash of the library id, its source path, the compiler build (the tool
 *     executable's path, size and modification time, and the build date), the feature set
 *     (target, --unicode) and the format version;
 *   - inside: the hash of every source file the library read (the .sld, includes), the
 *     answers of its cond-expand (library ...) tests, and the fingerprint of every library it
 *     imported (their own sources and dependencies, transitively), checked after loading
 *     them in the recorded order -- the order a fresh expansion would load them in;
 *   - a checksum over the whole file (truncated or corrupted files are ignored).
 * Anything unexpected while reading makes the entry a miss and the library is expanded
 * from source (and the entry rewritten). Entries are written to a temporary file and
 * renamed, so concurrent builds never see a partial entry. A library whose state cannot be
 * written (a macro defined in a local scope, an unknown kind of datum) is simply not cached.
 * Output with a warm cache is identical to a cold one: the entry restores exactly the state
 * a fresh expansion produces (local symbols renumbered from the current counter). */
#include "front.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#define CACHE_VERSION "shikra-expand-1"
typedef unsigned long long u64;
static const char *dir;
static u64 build_id, feat_id;

static u64 fnv(u64 h, const void *p, size_t n) {
    const unsigned char *b = (const unsigned char *)p;
    while (n--) h = (h ^ *b++) * 1099511628211ull;
    return h;
}
#define FNV0 14695981039346656037ull

/* ---- what a library does while it is loaded fresh ---- */
typedef struct { const char *path; u64 hash; } c_file;
typedef struct { const char *id; int r; } c_exists;
typedef struct {
    c_file files[64]; int nfiles;
    library *deps[64]; int ndeps;
    c_exists ex[16]; int nex;
    int overflow;           /* too much to record: not cached */
    long base;              /* local symbol counter when its expansion started */
    global **gsnap; int ngsnap;           /* globals of other libraries not yet assigned */
    import_rec **rsnap; int nrsnap;       /* import records of other libraries not yet used */
} c_rec;
static pmap recs;   /* library -> c_rec */
static pmap fprint; /* library -> u64 * fingerprint */

static c_rec *rec_of(library *l) {
    c_rec *r;
    if (!l || !dir) return NULL;
    if (!(r = (c_rec *)pm_get(&recs, l))) { r = NEW(c_rec); pm_put(&recs, l, r); }
    return r;
}
static c_rec prog; /* what the program itself read and asked (shikra run's key) */
void cache_note_file(const char *path, const char *text, size_t n) {
    c_rec *r = front_loading() ? rec_of(front_loading()) : dir ? &prog : NULL;
    if (!r) return;
    if (r->nfiles == 64) { r->overflow = 1; return; }
    r->files[r->nfiles].path = arena_strdup(&compiler_arena, path);
    r->files[r->nfiles++].hash = fnv(FNV0, text, n);
}
void cache_note_dep(library *dep) {
    c_rec *r = rec_of(front_loading());
    if (!r) return;
    if (r->ndeps == 64) { r->overflow = 1; return; }
    r->deps[r->ndeps++] = dep;
}
void cache_note_exists(const char *id, int e) {
    c_rec *r = front_loading() ? rec_of(front_loading()) : dir ? &prog : NULL;
    if (!r) return;
    if (r->nex == 16) { r->overflow = 1; return; }
    r->ex[r->nex].id = id;
    r->ex[r->nex++].r = e;
}
static u64 fingerprint(library *l) {
    u64 *f = (u64 *)pm_get(&fprint, l);
    return f ? *f : 0;
}
static void set_fingerprint(library *l, u64 v) {
    u64 *f = NEW(u64);
    *f = v;
    pm_put(&fprint, l, f);
}

/* ---- identity of the compiler build and of the configuration ---- */
void cache_init(const front_opts *o) {
    char exe[4096];
    struct stat st;
    obj f;
    int ok = 0;
    if (!o->cache_dir || o->repl) return; /* the REPL and `shikra interp` expand without it */
    {
        sbuf b = {0};
        sb_printf(&b, "%s/expand", o->cache_dir);
        mkdir(b.s, 0777);
        dir = arena_strdup(&compiler_arena, b.s);
        sb_free(&b);
    }
#ifdef __APPLE__
    {
        uint32_t sz = sizeof exe;
        ok = _NSGetExecutablePath(exe, &sz) == 0;
    }
#else
    {
        ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        if ((ok = n > 0)) exe[n] = 0;
    }
#endif
    build_id = fnv(FNV0, CACHE_VERSION " " __DATE__ " " __TIME__, sizeof(CACHE_VERSION " " __DATE__ " " __TIME__));
    if (ok && stat(exe, &st) == 0) {
        u64 v[3];
        v[0] = (u64)st.st_size;
        v[1] = (u64)st.st_mtime;
        v[2] = (u64)st.st_ino;
        build_id = fnv(fnv(build_id, exe, strlen(exe)), v, sizeof v);
    }
    feat_id = FNV0;
    for (f = front_features(); f != RT_NIL; f = RT_CDR(f)) {
        const char *s = RT_STR(RT_PTR(RT_CAR(f), rt_symbol)->name)->buf;
        feat_id = fnv(feat_id, s, strlen(s) + 1);
    }
}
static const char *entry_path(library *l, int tmp) {
    sbuf b = {0}, k = {0};
    const char *r;
    u64 h;
    sb_printf(&k, "%s\n%s\n", l->id, l->path);
    h = fnv(fnv(fnv(FNV0, k.s, k.len), &build_id, sizeof build_id), &feat_id, sizeof feat_id);
    sb_printf(&b, "%s/%016llx", dir, h);
    if (tmp) sb_printf(&b, ".%ld.tmp", (long)getpid());
    r = arena_strdup(&compiler_arena, b.s);
    sb_free(&b);
    sb_free(&k);
    return r;
}
static int source_hash(const char *path, u64 *h) {
    size_t n;
    char *t;
    if (!strncmp(path, "<builtin>/", 10)) {
        sbuf b = {0};
        sb_printf(&b, "lib/%s", path + 10);
        t = embed_text(b.s, &n);
        sb_free(&b);
    } else t = read_file(path, &n);
    if (!t) return 0;
    *h = fnv(FNV0, t, n);
    free(t);
    return 1;
}

/* ---- writing ---- */
static sbuf W;
static int wfail;   /* the library cannot be written */
static library *wlib;
static pmap wobj, wbind, wmac, xbind, xmac; /* object/binding/macro -> index+1; foreign binding/macro -> imp */
static long nwobj, nwbind, nwmac, wbase;
static htab wfiles; /* location file name -> index+1 */
static long nwfiles;

static void put_u8(int c) { sb_putc(&W, c); }
static void put_uv(u64 v) {
    while (v >= 0x80) { sb_putc(&W, (int)(v & 0x7f) | 0x80); v >>= 7; }
    sb_putc(&W, (int)v);
}
static void put_sv(long long v) { put_uv(v < 0 ? ((u64)(-(v + 1)) << 1) | 1 : (u64)v << 1); }
static void put_str(const char *s, size_t n) { put_uv(n); sb_putn(&W, s, n); }
static void put_cstr(const char *s) { put_str(s, strlen(s)); }
static void put_u64(u64 v) { int i; for (i = 0; i < 8; i++) sb_putc(&W, (int)(v >> (8 * i)) & 0xff); }
static const char *symname(obj s) { return RT_STR(RT_PTR(s, rt_symbol)->name)->buf; }

static void put_frame(frame *f) {
    if (f == core_env) put_u8('C');
    else if (f && !f->parent && f->lib && f == f->lib->env && f->lib->name != RT_FALSE) { put_u8('T'); put_cstr(f->lib->id); }
    else wfail = 1; /* a local scope */
}
static int seen(pmap *m, const void *p, long *n) { /* index of p, registering it if new (then -1) */
    long i = (long)pm_get(m, p);
    if (i) return (int)(i - 1);
    pm_put(m, p, (void *)(++*n));
    return -1;
}
static void put_loc(obj x) {
    obj f;
    intptr_t l, c;
    if (rt_srcloc_lookup(x, &f, &l, &c) && rt_is_string(f)) {
        long i = (long)ht_get(&wfiles, RT_STR(f)->buf);
        if (!i) { /* a new file name: 1, then the name */
            ht_put(&wfiles, RT_STR(f)->buf, (void *)(i = ++nwfiles));
            put_uv(1);
            put_cstr(RT_STR(f)->buf);
        } else put_uv((u64)i + 1);
        put_uv((u64)l);
        put_uv((u64)c);
    } else put_uv(0);
}
static void put_obj(obj x) {
    int i;
    if (wfail) return;
    if (rt_is_fix(x)) { put_u8('i'); put_sv((long long)RT_FIXVAL(x)); return; }
    if ((RT_W(x) & 7) == 7) { put_u8('m'); put_u64((u64)RT_W(x)); return; } /* immediates */
    if ((i = seen(&wobj, x, &nwobj)) >= 0) { put_u8('r'); put_uv((u64)i); return; }
    if (rt_is_pair(x)) { /* the cdr chain iteratively, cars recursively */
        for (;;) {
            put_u8('p');
            put_loc(x);
            put_obj(RT_CAR(x));
            x = RT_CDR(x);
            if (!rt_is_pair(x) || pm_get(&wobj, x)) { put_obj(x); return; }
            seen(&wobj, x, &nwobj);
        }
    }
    if (rt_is_symbol(x)) {
        obj orig, n;
        frame *env;
        global *g;
        if (front_alias_info(x, &orig, &env)) { put_u8('a'); put_obj(orig); put_frame(env); }
        else if ((n = front_local_name(x))) {
            const char *s = strrchr(symname(x), '.');
            long k = s ? atol(s + 1) : 0;
            if (k <= wbase) { wfail = 1; return; } /* another library's local */
            put_u8('l'); put_obj(n); put_uv((u64)(k - wbase));
        } else if ((g = front_global(x))) { put_u8('g'); put_cstr(g->lib->id); put_uv((u64)g->index); }
        else if (rt_intern(symname(x), (size_t)RT_STR(RT_PTR(x, rt_symbol)->name)->bytes) == x) {
            put_u8('y');
            put_str(symname(x), (size_t)RT_STR(RT_PTR(x, rt_symbol)->name)->bytes);
        }
        else wfail = 1;
        return;
    }
    if (rt_is_string(x)) { put_u8('s'); put_u8(RT_HFLAGS(x) & RT_F_RO); put_str(RT_STR(x)->buf, (size_t)RT_STR(x)->bytes); return; }
    if (rt_is_flo(x)) { double d = RT_FLO(x); u64 v; memcpy(&v, &d, 8); put_u8('d'); put_u64(v); return; }
    if (rt_is_vector(x)) {
        intptr_t k, n = RT_HLEN(x);
        put_u8('v'); put_u8(RT_HFLAGS(x) & RT_F_RO); put_uv((u64)n);
        for (k = 0; k < n; k++) put_obj(RT_VEC(x)[k]);
        return;
    }
    if (rt_is_bytevector(x)) { put_u8('b'); put_u8(RT_HFLAGS(x) & RT_F_RO); put_str((const char *)RT_PTR(x, rt_bytevector)->b, (size_t)RT_HLEN(x)); return; }
    wfail = 1;
}
static void put_macro(macro *m) {
    imp *x;
    int i;
    if ((i = seen(&wmac, m, &nwmac)) >= 0) { put_u8('r'); put_uv((u64)i); return; }
    if ((x = (imp *)pm_get(&xmac, m))) { put_u8('X'); put_cstr(((library *)x->loc)->id); put_obj(x->name); return; }
    put_u8('N');
    put_obj(m->name);
    put_obj(m->ellipsis ? m->ellipsis : RT_UNSPEC);
    put_obj(m->literals);
    put_obj(m->rules);
    put_frame(m->env);
}
static void put_binding(binding *b) {
    imp *x;
    int i;
    if ((i = seen(&wbind, b, &nwbind)) >= 0) { put_u8('r'); put_uv((u64)i); return; }
    if ((x = (imp *)pm_get(&xbind, b))) { put_u8('I'); put_cstr(((library *)x->loc)->id); put_obj(x->name); }
    else if (b->kind == B_GLOBAL && b->g->lib == wlib) { put_u8('G'); put_uv((u64)b->g->index); }
    else if (b->kind == B_MACRO) { put_u8('M'); put_macro(b->mac); }
    else wfail = 1;
}
static void index_exports(library *l) { /* foreign bindings and macros by (library, export name) */
    imp *i;
    for (i = l->exports; i; i = i->next) {
        imp *x = NEW(imp);
        x->name = i->name;
        x->loc = (obj)(void *)l;
        if (!pm_get(&xbind, i->b)) pm_put(&xbind, i->b, x);
        if (i->b->kind == B_MACRO && !pm_get(&xmac, i->b->mac)) pm_put(&xmac, i->b->mac, x);
    }
}

void cache_begin_expand(library *lib) {
    c_rec *r = rec_of(lib);
    library *l;
    int n = 0, m = 0, i;
    import_rec *ir;
    if (!r) return;
    r->base = front_counter(-1);
    for (l = front_order; l; l = l->next) {
        n += l->nglobals;
        for (ir = l->imports; ir; ir = ir->next) m++;
    }
    r->gsnap = NEWN(global *, n);
    r->rsnap = NEWN(import_rec *, m);
    for (l = front_order; l; l = l->next) {
        for (i = 0; i < l->nglobals; i++) if (!l->globals[i]->assigned) r->gsnap[r->ngsnap++] = l->globals[i];
        for (ir = l->imports; ir; ir = ir->next) if (!ir->used) r->rsnap[r->nrsnap++] = ir;
    }
}

void cache_store(library *lib) {
    c_rec *r = rec_of(lib);
    library *l;
    u64 fp = FNV0;
    int i, n;
    size_t k;
    import_rec *ir;
    FILE *f;
    const char *tmp;
    if (!r) return;
    for (i = 0; i < r->nfiles; i++) fp = fnv(fnv(fp, &r->files[i].hash, 8), r->files[i].path, strlen(r->files[i].path) + 1);
    for (i = 0; i < r->ndeps; i++) { u64 d = fingerprint(r->deps[i]); fp = fnv(fp, &d, 8); }
    for (i = 0; i < r->nex; i++) fp = fnv(fp, &r->ex[i].r, sizeof(int));
    set_fingerprint(lib, fp);
    if (r->overflow) return;
    W.len = 0;
    wfail = 0;
    wlib = lib;
    wbase = r->base;
    memset(&wobj, 0, sizeof wobj); memset(&wbind, 0, sizeof wbind); memset(&wmac, 0, sizeof wmac);
    memset(&xbind, 0, sizeof xbind); memset(&xmac, 0, sizeof xmac); memset(&wfiles, 0, sizeof wfiles);
    nwobj = nwbind = nwmac = nwfiles = 0;
    index_exports(front_core_lib);
    for (l = front_order; l; l = l->next) index_exports(l);
    /* header: what the entry depends on */
    put_cstr(CACHE_VERSION);
    put_u64(build_id);
    put_u64(feat_id);
    put_cstr(lib->id);
    put_cstr(lib->path);
    put_uv((u64)r->nfiles);
    for (i = 0; i < r->nfiles; i++) { put_cstr(r->files[i].path); put_u64(r->files[i].hash); }
    put_uv((u64)r->nex);
    for (i = 0; i < r->nex; i++) { put_cstr(r->ex[i].id); put_u8(r->ex[i].r); }
    put_uv((u64)r->ndeps);
    for (i = 0; i < r->ndeps; i++) { put_cstr(r->deps[i]->id); put_u64(fingerprint(r->deps[i])); }
    put_u64(fp);
    /* payload: globals, frame, exports, core forms, import records, effects on others */
    put_uv((u64)lib->nglobals);
    for (i = 0; i < lib->nglobals; i++) {
        global *g = lib->globals[i];
        put_obj(g->name);
        put_u8(g->assigned | g->nowarn << 1 | (g->defloc != 0) << 2);
        put_uv((u64)g->defined);
        if (g->defloc) put_loc(g->defloc);
    }
    for (n = 0, k = 0; k < lib->env->map.cap; k++) n += lib->env->map.k[k] != NULL;
    put_uv((u64)n);
    for (k = 0; k < lib->env->map.cap; k++)
        if (lib->env->map.k[k]) { put_obj((obj)lib->env->map.k[k]); put_binding((binding *)lib->env->map.v[k]); }
    {   imp *e;
        for (n = 0, e = lib->exports; e; e = e->next) n++;
        put_uv((u64)n);
        for (e = lib->exports; e; e = e->next) { put_obj(e->name); put_binding(e->b); }
    }
    put_obj(lib->core);
    for (n = 0, ir = lib->imports; ir; ir = ir->next) n++;
    put_uv((u64)n);
    for (ir = lib->imports; ir; ir = ir->next) { put_obj(ir->spec); put_u8(ir->used); }
    for (n = 0, k = 0; k < lib->imported.cap; k++) n += lib->imported.k[k] != NULL;
    put_uv((u64)n);
    for (k = 0; k < lib->imported.cap; k++)
        if (lib->imported.k[k]) {
            imp_use *u;
            int c = 0;
            for (u = (imp_use *)lib->imported.v[k]; u; u = u->next) c++;
            put_obj((obj)lib->imported.k[k]);
            put_uv((u64)c);
            for (u = (imp_use *)lib->imported.v[k]; u; u = u->next) {
                for (n = 0, ir = lib->imports; ir && ir != u->rec; ir = ir->next) n++;
                put_uv((u64)n);
                put_binding(u->b);
            }
        }
    for (n = 0, i = 0; i < r->ngsnap; i++) n += r->gsnap[i]->assigned;
    put_uv((u64)n);
    for (i = 0; i < r->ngsnap; i++) if (r->gsnap[i]->assigned) { put_cstr(r->gsnap[i]->lib->id); put_uv((u64)r->gsnap[i]->index); }
    for (n = 0, i = 0; i < r->nrsnap; i++) n += r->rsnap[i]->used;
    put_uv((u64)n);
    for (i = 0; i < r->nrsnap; i++)
        if (r->rsnap[i]->used) { /* (library, position of the record) */
            for (l = front_order; l; l = l->next) {
                for (n = 0, ir = l->imports; ir && ir != r->rsnap[i]; ir = ir->next) n++;
                if (ir) { put_cstr(l->id); put_uv((u64)n); break; }
            }
        }
    put_uv((u64)(front_counter(-1) - r->base));
    if (wfail) return;
    put_u64(fnv(FNV0, W.s, W.len));
    tmp = entry_path(lib, 1);
    if (!(f = fopen(tmp, "wb"))) return;
    if (fwrite(W.s, 1, W.len, f) != W.len) { fclose(f); remove(tmp); return; }
    if (fclose(f) != 0 || rename(tmp, entry_path(lib, 0)) != 0) remove(tmp);
}

/* ---- reading: any inconsistency is a miss ---- */
static const unsigned char *R, *Rend;
static int rfail;
static library *rlib;
static obj *robj;
static long nrobj, caprobj, rbase;
static binding **rbind;
static long nrbind, caprbind;
static macro **rmac;
static long nrmac, caprmac;
static obj *rfiles;
static long nrfiles, caprfiles;

static int get_u8(void) { if (R >= Rend) { rfail = 1; return 0; } return *R++; }
static u64 get_uv(void) {
    u64 v = 0;
    int s = 0, c;
    do { c = get_u8(); v |= (u64)(c & 0x7f) << s; s += 7; } while ((c & 0x80) && !rfail && s < 64);
    return v;
}
static long long get_sv(void) { u64 v = get_uv(); return v & 1 ? -(long long)(v >> 1) - 1 : (long long)(v >> 1); }
static u64 get_u64(void) { u64 v = 0; int i; for (i = 0; i < 8; i++) v |= (u64)get_u8() << (8 * i); return v; }
static const char *get_str(size_t *n) {
    u64 len = get_uv();
    char *s;
    if (rfail || len > (u64)(Rend - R)) { rfail = 1; return ""; }
    s = (char *)arena_alloc(&compiler_arena, (size_t)len + 1);
    memcpy(s, R, (size_t)len);
    R += len;
    if (n) *n = (size_t)len;
    return s;
}
#define GROW(arr, n, cap, T) do { if ((n) == (cap)) { T *t_ = NEWN(T, (cap) * 2 + 64); if (n) memcpy(t_, arr, sizeof(T) * (size_t)(n)); arr = t_; cap = (cap) * 2 + 64; } } while (0)
static long reg(obj x) { GROW(robj, nrobj, caprobj, obj); robj[nrobj] = x; return nrobj++; }
static library *get_lib(void) {
    library *l = front_library(get_str(NULL));
    if (!l) rfail = 1;
    return l;
}
static frame *get_frame(void) {
    int c = get_u8();
    library *l;
    if (c == 'C') return core_env;
    if (c == 'T' && (l = get_lib())) return l->env;
    rfail = 1;
    return core_env;
}
static void get_loc(obj p) {
    u64 i = get_uv();
    intptr_t l, c;
    if (!i) return;
    if (i == 1) { GROW(rfiles, nrfiles, caprfiles, obj); rfiles[nrfiles++] = rt_cstring(get_str(NULL)); i = (u64)nrfiles + 1; }
    l = (intptr_t)get_uv();
    c = (intptr_t)get_uv();
    if (i - 2 >= (u64)nrfiles) { rfail = 1; return; }
    if (p != RT_FALSE) rt_srcloc_set(p, rfiles[i - 2], l, c);
}
static obj get_obj(void);
static obj get_atom(int t) {
    size_t n;
    const char *s;
    long i;
    obj x;
    switch (t) {
    case 'i': return RT_FIX((intptr_t)get_sv());
    case 'm': { u64 w = get_u64(); return (obj)(uintptr_t)w; }
    case 'r': { u64 k = get_uv(); if (k >= (u64)nrobj) { rfail = 1; return RT_FALSE; } return robj[k]; }
    case 'a': {
        obj orig;
        frame *env;
        i = reg(RT_FALSE);
        orig = get_obj();
        env = get_frame();
        if (rfail || !rt_is_symbol(orig)) { rfail = 1; return RT_FALSE; }
        return robj[i] = front_alias(orig, env);
    }
    case 'l': {
        obj src;
        i = reg(RT_FALSE);
        src = get_obj();
        n = (size_t)get_uv();
        if (rfail || !rt_is_symbol(src)) { rfail = 1; return RT_FALSE; }
        return robj[i] = front_make_local(src, rbase + (long)n);
    }
    case 'g': {
        library *l = get_lib();
        u64 k = get_uv();
        if (rfail || k >= (u64)l->nglobals) { rfail = 1; return RT_FALSE; }
        x = l->globals[k]->sym;
        reg(x);
        return x;
    }
    case 'y': s = get_str(&n); x = rt_intern(s, n); reg(x); return x;
    case 's': {
        int ro = get_u8();
        s = get_str(&n);
        x = rt_make_string_n(s, n);
        if (ro) RT_SETFLAGS(x, RT_HFLAGS(x) | RT_F_RO);
        reg(x);
        return x;
    }
    case 'd': { u64 v = get_u64(); double d; memcpy(&d, &v, 8); x = rt_make_flonum(d); reg(x); return x; }
    case 'v': {
        int ro = get_u8();
        u64 k, len = get_uv();
        if (rfail || len > (u64)(Rend - R)) { rfail = 1; return RT_FALSE; }
        x = rt_make_vector(RT_FIX((intptr_t)len), RT_FALSE);
        reg(x);
        for (k = 0; k < len && !rfail; k++) RT_VEC(x)[k] = get_obj();
        if (ro) RT_SETFLAGS(x, RT_HFLAGS(x) | RT_F_RO);
        return x;
    }
    case 'b': {
        int ro = get_u8();
        s = get_str(&n);
        x = rt_make_bytevector(RT_FIX((intptr_t)n), RT_FIX(0));
        memcpy(RT_PTR(x, rt_bytevector)->b, s, n);
        if (ro) RT_SETFLAGS(x, RT_HFLAGS(x) | RT_F_RO);
        reg(x);
        return x;
    }
    }
    rfail = 1;
    return RT_FALSE;
}
static obj get_obj(void) {
    int t = get_u8();
    obj head, prev = RT_FALSE;
    if (t != 'p') return get_atom(t);
    for (;;) { /* a cdr chain of new pairs */
        obj p = rt_cons(RT_FALSE, RT_NIL);
        if (prev == RT_FALSE) head = p;
        else RT_CDR(prev) = p;
        reg(p);
        get_loc(p);
        RT_CAR(p) = get_obj();
        prev = p;
        if (rfail) return RT_FALSE;
        if ((t = get_u8()) != 'p') { RT_CDR(prev) = get_atom(t); return head; }
    }
}
static binding *find_export(library *l, obj name) {
    imp *i;
    if (l) for (i = l->exports; i; i = i->next) if (i->name == name) return i->b;
    rfail = 1;
    return NULL;
}
static macro *get_macro(void) {
    int t = get_u8();
    macro *m;
    if (t == 'r') { u64 k = get_uv(); if (k >= (u64)nrmac) { rfail = 1; return NULL; } return rmac[k]; }
    if (t == 'X') { library *l = get_lib(); binding *b = find_export(l, get_obj()); if (!b || b->kind != B_MACRO) { rfail = 1; return NULL; } m = b->mac; }
    else if (t == 'N') {
        m = NEW(macro);
        m->name = get_obj();
        m->ellipsis = get_obj();
        if (m->ellipsis == RT_UNSPEC) m->ellipsis = 0;
        m->literals = get_obj();
        m->rules = get_obj();
        m->env = get_frame();
    } else { rfail = 1; return NULL; }
    GROW(rmac, nrmac, caprmac, macro *);
    rmac[nrmac++] = m;
    return m;
}
static binding *get_binding(void) {
    int t = get_u8();
    binding *b = NULL;
    if (t == 'r') { u64 k = get_uv(); if (k >= (u64)nrbind) { rfail = 1; return NULL; } return rbind[k]; }
    if (t == 'I') { library *l = get_lib(); b = find_export(l, get_obj()); }
    else if (t == 'G') {
        u64 k = get_uv();
        if (k >= (u64)rlib->nglobals) { rfail = 1; return NULL; }
        b = new_binding(B_GLOBAL);
        b->g = rlib->globals[k];
    } else if (t == 'M') { b = new_binding(B_MACRO); b->mac = get_macro(); }
    else rfail = 1;
    if (rfail) return NULL;
    GROW(rbind, nrbind, caprbind, binding *);
    rbind[nrbind++] = b;
    return b;
}

int cache_load(library *lib) {
    const char *path;
    char *buf;
    size_t len, n;
    u64 i, cnt, fp;
    int ok = 0;
    if (!dir) return 0;
    path = entry_path(lib, 0);
    if (!(buf = read_file(path, &len)) || len < 8) { free(buf); return 0; }
    R = (const unsigned char *)buf;
    Rend = R + len - 8;
    rfail = 0;
    for (fp = 0, i = 0; i < 8; i++) fp |= (u64)Rend[i] << (8 * i);
    if (fnv(FNV0, buf, len - 8) != fp) goto miss;
    if (strcmp(get_str(NULL), CACHE_VERSION) || get_u64() != build_id || get_u64() != feat_id ||
        strcmp(get_str(NULL), lib->id) || strcmp(get_str(NULL), lib->path) || rfail)
        goto miss;
    for (cnt = get_uv(), i = 0; i < cnt && !rfail; i++) { /* every source file unchanged */
        const char *src = get_str(NULL);
        u64 h, want = get_u64();
        if (rfail) goto miss; /* built-in sources are part of the tool: the build id covers them */
        if (strncmp(src, "<builtin>/", 10) && (!source_hash(src, &h) || h != want)) goto miss;
    }
    for (cnt = get_uv(), i = 0; i < cnt && !rfail; i++) { /* every (library ...) test answered the same */
        const char *id = get_str(NULL);
        int e = get_u8();
        if (rfail || front_lib_exists(id) != e) goto miss;
    }
    for (cnt = get_uv(), i = 0; i < cnt && !rfail; i++) { /* imports, in order, with their fingerprints */
        const char *id = get_str(NULL);
        u64 want = get_u64();
        library *d;
        if (rfail) goto miss;
        if (!(d = front_library(id))) { /* loading it may read its own entry: keep our place */
            const unsigned char *r = R, *e = Rend;
            d = front_find_dep(id);
            R = r;
            Rend = e;
            rfail = 0;
        }
        if (!d || d->state != 2 || fingerprint(d) != want) goto miss;
        cache_note_dep(d);
    }
    fp = get_u64();
    if (rfail) goto miss;
    /* payload */
    rlib = lib;
    nrobj = nrbind = nrmac = nrfiles = 0;
    rbase = front_counter(-1);
    for (cnt = get_uv(), i = 0; i < cnt && !rfail; i++) {
        obj name = get_obj();
        int fl = get_u8();
        global *g;
        if (rfail || !rt_is_symbol(name)) goto miss;
        g = new_global(lib, name);
        g->assigned = fl & 1;
        g->nowarn = fl >> 1 & 1;
        g->defined = (int)get_uv();
        if (fl & 4) { g->defloc = rt_cons(RT_FALSE, RT_FALSE); get_loc(g->defloc); }
    }
    for (cnt = get_uv(), i = 0; i < cnt && !rfail; i++) {
        obj id = get_obj();
        binding *b = get_binding();
        if (rfail || !rt_is_symbol(id)) goto miss;
        pm_put(&lib->env->map, id, b);
    }
    {   imp **pp = &lib->exports;
        for (cnt = get_uv(), i = 0; i < cnt && !rfail; i++) {
            obj name = get_obj();
            binding *b = get_binding();
            if (rfail) goto miss;
            *pp = NEW(imp);
            (*pp)->name = name;
            (*pp)->b = b;
            (*pp)->loc = RT_FALSE;
            pp = &(*pp)->next;
        }
    }
    lib->core = get_obj();
    {   import_rec **pp = &lib->imports, **recv;
        cnt = get_uv();
        if (rfail || cnt > len) goto miss;
        recv = NEWN(import_rec *, cnt);
        for (i = 0; i < cnt && !rfail; i++) {
            *pp = recv[i] = NEW(import_rec);
            (*pp)->spec = get_obj();
            (*pp)->used = get_u8();
            pp = &(*pp)->next;
        }
        for (n = (size_t)get_uv(), i = 0; i < n && !rfail; i++) {
            obj id = get_obj();
            u64 c, j;
            for (c = get_uv(), j = 0; j < c && !rfail; j++) {
                u64 k = get_uv();
                binding *b = get_binding();
                if (rfail || k >= cnt) goto miss;
                front_add_import_use(lib, id, recv[k], b);
            }
        }
    }
    {   /* effects on other libraries: check them all, then apply */
        const unsigned char *at = R;
        int pass;
        for (pass = 0; pass < 2; pass++) {
            R = at;
            for (cnt = get_uv(), i = 0; i < cnt && !rfail; i++) {
                library *l = get_lib();
                u64 k = get_uv();
                if (rfail || k >= (u64)l->nglobals) goto miss;
                if (pass) l->globals[k]->assigned = 1;
            }
            for (cnt = get_uv(), i = 0; i < cnt && !rfail; i++) {
                library *l = get_lib();
                u64 k = get_uv();
                import_rec *ir;
                if (rfail) goto miss;
                for (ir = l->imports; ir && k; ir = ir->next) k--;
                if (!ir) goto miss;
                if (pass) ir->used = 1;
            }
            n = (size_t)get_uv();
            if (rfail || R != Rend) goto miss;
        }
        front_counter(rbase + (long)n);
    }
    set_fingerprint(lib, fp);
    ok = 1;
miss:
    if (!ok) { /* undo what a partial read added to the library */
        lib->nglobals = 0;
        lib->env->map.n = 0;
        if (lib->env->map.cap) memset(lib->env->map.k, 0, sizeof(void *) * lib->env->map.cap);
        lib->exports = NULL;
        lib->core = RT_NIL;
        lib->imports = NULL;
        memset(&lib->imported, 0, sizeof lib->imported);
        if (rec_of(lib)) memset(rec_of(lib), 0, sizeof(c_rec)); /* dependencies are noted again */
    }
    free(buf);
    return ok;
}

/* shikra run: a hash of everything the front end's result depends on (the program's sources
 * and cond-expand answers, the fingerprint of every library, compiler build, features), or 0
 * when there is no cache. Equal keys mean equal expanded programs, so equal C. */
unsigned long long cache_program_key(void) {
    library *l;
    u64 h = FNV0;
    int i;
    if (!dir || prog.overflow) return 0;
    h = fnv(fnv(h, &build_id, 8), &feat_id, 8);
    for (i = 0; i < prog.nfiles; i++) { h = fnv(h, prog.files[i].path, strlen(prog.files[i].path) + 1); h = fnv(h, &prog.files[i].hash, 8); }
    for (i = 0; i < prog.nex; i++) { h = fnv(h, prog.ex[i].id, strlen(prog.ex[i].id) + 1); h = fnv(h, &prog.ex[i].r, sizeof(int)); }
    for (l = front_order; l; l = l->next) {
        u64 f;
        if (l->name == RT_FALSE) continue; /* the program */
        if (!(f = fingerprint(l))) return 0;
        h = fnv(fnv(h, l->id, strlen(l->id) + 1), &f, 8);
    }
    return h;
}
