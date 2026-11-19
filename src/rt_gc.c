/* rt_gc.c -- allocation and collection (spec §7, DESIGN.md §5 and "M2 as built").
 * One interface (the rt_alloc / rt_cons fast paths in rt.h, rt_alloc_slow, roots, immortal
 * mode, rt_gc_collect), two collectors chosen by a config macro:
 *   RT_GC_BUMP  --gc=bump: OS chunks, bump allocation, never frees (spec §7.1);
 *   default     an Immix-style mark-region collector: 32 KB blocks of 256-byte lines,
 *               side mark and object-start bitmaps, non-moving, single-threaded, no
 *               barriers. Conservative roots (C stack + registers), precise heap.
 * Nothing here runs unless the program allocates: rt_init only records the stack base and
 * root ranges, and the heap is set up by the first rt_alloc_slow. */
#ifndef SHIKRA_RT_H
#include "rt.h"
#endif

#define GC_MAXROOTS 64
RT_VAR char *rt_hp, *rt_hlim, *rt_pp, *rt_plim;
static RT_UNUSED void *gc_stack_base;
static RT_UNUSED int gc_immortal, gc_nroots;
static RT_UNUSED obj *gc_roots[GC_MAXROOTS][2];
static RT_UNUSED obj **gc_droots[GC_MAXROOTS][2]; /* ranges whose bounds are read at collection time */
static RT_UNUSED int gc_ndroots;
#ifdef SHIKRA_PROGRAM
static RT_UNUSED int gc_enabled = 1;
#else
static RT_UNUSED int gc_enabled; /* tool mode: compile commands never collect (DESIGN.md §5) */
#endif

static RT_UNUSED RT_NORETURN void gc_oom(void) {
    static const char msg[] = "Error: out of memory\n";
    rt_sys_write(2, msg, sizeof msg - 1);
    rt_sys_exit(70);
}

RT_FN void rt_gc_init(void *stack_base) { gc_stack_base = stack_base; }
RT_FN void rt_gc_add_roots(obj *start, obj *end) {
    if (gc_nroots == GC_MAXROOTS) gc_oom();
    gc_roots[gc_nroots][0] = start;
    gc_roots[gc_nroots][1] = end;
    gc_nroots++;
}
RT_FN void rt_gc_add_dynamic_roots(obj **lo, obj **hi) {
    if (gc_ndroots == GC_MAXROOTS) gc_oom();
    gc_droots[gc_ndroots][0] = lo;
    gc_droots[gc_ndroots][1] = hi;
    gc_ndroots++;
}
RT_FN void rt_gc_enable(int on) { gc_enabled = on; }
RT_FN void *rt_alloc_raw(size_t nbytes) {
    char *p = (char *)rt_alloc(RT_HDR(RT_T_RAW, 0, nbytes), sizeof(uintptr_t) + nbytes);
    return p + sizeof(uintptr_t);
}

#ifdef RT_GC_BUMP
/* ==== bump: never frees ============================================================== */
#define GC_CHUNK ((size_t)1 << 20)
RT_FN void *rt_alloc_slow(size_t bytes, int pair_stream) {
    char *p;
    if (bytes > GC_CHUNK / 4) { /* large object: own mapping, keep the current chunk */
        if (!(p = (char *)rt_sys_mem(bytes))) gc_oom();
        return p;
    }
    if (!(p = (char *)rt_sys_mem(GC_CHUNK))) gc_oom();
    if (pair_stream) { rt_pp = p + bytes; rt_plim = p + GC_CHUNK; }
    else { rt_hp = p + bytes; rt_hlim = p + GC_CHUNK; }
    return p;
}
RT_FN void rt_gc_set_immortal(int on) { gc_immortal = on; }
RT_FN void rt_gc_collect(void) {}

#else
/* ==== Immix ============================================================================
 * Heap: chunks of 32 KB-aligned blocks obtained from rt_sys_mem, each chunk with a side
 * array of block descriptors (kind, one state byte per line, start and mark bitmaps with
 * one bit per 8-byte granule). A block holds either pairs or headered objects. Spaces bump
 * through runs of free lines: headered and pair streams (the rt_hp / rt_pp globals), an
 * overflow space for medium objects that miss the current run (whole free blocks), and
 * immortal headered/pair spaces (whole zeroed blocks, never reclaimed). Requests above
 * GC_MEDIUM that miss the fast path get their own mapping (large object space).
 * Object starts: a newly claimed headered run is marked L_RUN; collection first walks
 * each such run by header sizes (it ends at a zero header word, written whenever a space
 * leaves a run) and sets the start bits, so the fast path records nothing. Sweeping keeps
 * the start bits of marked objects only. Line states: */
#define GC_BLOCK ((size_t)1 << 15)
#define GC_LINE ((size_t)256)
#define GC_NLINES (GC_BLOCK / GC_LINE)
#define GC_NWORDS (GC_BLOCK / 8 / 64)
#define GC_MEDIUM (GC_BLOCK / 4)
#define GC_FIRST 32 /* blocks in the first chunk: 1 MB */
#define GC_MAXCHUNKS 48
#define GC_LHDR 32  /* large object header, object follows */
#define GC_STACK0 256
#ifdef RT_GC_STRESS
#define GC_STACKMAX ((size_t)4096) /* stress builds exercise the mark-stack overflow rescan more */
#else
#define GC_STACKMAX ((size_t)1 << 20)
#endif
enum { GC_FREE = 0, GC_HDR = 1, GC_PAIR = 2, GC_IMM = 4, GC_PURE = 8 }; /* block kinds */
enum { L_FREE = 0, L_LIVE = 1, L_NEW = 2, L_RUN = 3, L_MARK = 4 }; /* line states (+ mark bit) */
enum { S_HDR, S_PAIR, S_OVF, S_IHDR, S_IPAIR, S_PHDR, S_PPAIR, S_N }; /* spaces */

typedef struct { unsigned char kind, line[GC_NLINES]; uintptr_t start[GC_NWORDS], mark[GC_NWORDS]; } gc_block;
typedef struct { char *base; size_t n; gc_block *b; } gc_chunk;
typedef struct { char *cur, *end; int ci, kind; intptr_t bi; } gc_space; /* bi -1: no current block */
typedef struct { size_t size, bytes; int mark, imm; } gc_large;          /* size: mapping, bytes: object */
typedef struct { obj *lo, *hi; } gc_range;

static gc_chunk gc_chunks[GC_MAXCHUNKS];
static int gc_nchunks, gc_ready, gc_ovf;
static uintptr_t gc_lo = ~(uintptr_t)0, gc_hi;
static size_t gc_nblocks, gc_claimed;
static gc_space gc_sp[S_N];
static gc_large **gc_larges;
static size_t gc_nlarge, gc_caplarge, gc_large_since, gc_large_limit;
static gc_range *gc_stack;
static size_t gc_top, gc_cap;
/* Stress builds (and the tool, for the interpreter) can collect every gc_stress-th allocation. */
#if defined(RT_GC_STRESS) || !defined(SHIKRA_PROGRAM)
#define GC_STRESS_ON 1
static intptr_t gc_stress, gc_countdown;
#endif

/* Size in bytes of the headered object at p (rt_alloc rounds to 8). */
static size_t gc_size(const char *p) {
    uintptr_t h = *(const uintptr_t *)p, t = h & 0xFF, n = h >> 16;
    if (t < 16) return 8 + 8 * n;
    if (t < 32) return (8 + n + 7) & ~(size_t)7;
    if (t == RT_T_STRING) return sizeof(rt_string);
    if (t == RT_T_PORT) return sizeof(rt_port);
    return sizeof(rt_proc) + 8 * n; /* RT_T_PROC */
}
static int gc_hibit(uintptr_t w) {
#if defined(__GNUC__) || defined(__clang__)
    return 63 - __builtin_clzll((unsigned long long)w);
#else
    int i = 0;
    while (w >>= 1) i++;
    return i;
#endif
}

static void gc_add_chunk(size_t n) {
    char *m = 0;
    gc_block *b = 0;
    if (gc_nchunks == GC_MAXCHUNKS || !(m = (char *)rt_sys_mem(n * GC_BLOCK + GC_BLOCK)) ||
        !(b = (gc_block *)rt_sys_mem(n * sizeof(gc_block))))
        gc_oom();
    m = (char *)(((uintptr_t)m + GC_BLOCK - 1) & ~(uintptr_t)(GC_BLOCK - 1));
    gc_chunks[gc_nchunks].base = m;
    gc_chunks[gc_nchunks].n = n;
    gc_chunks[gc_nchunks++].b = b;
    gc_nblocks += n;
    if ((uintptr_t)m < gc_lo) gc_lo = (uintptr_t)m;
    if ((uintptr_t)m + n * GC_BLOCK > gc_hi) gc_hi = (uintptr_t)m + n * GC_BLOCK;
}

static void gc_setup(void) {
    static const int kinds[S_N] = {GC_HDR, GC_PAIR, GC_HDR, GC_HDR | GC_IMM, GC_PAIR | GC_IMM,
                                   GC_HDR | GC_IMM | GC_PURE, GC_PAIR | GC_IMM | GC_PURE};
    int i;
    for (i = 0; i < S_N; i++) { gc_sp[i].kind = kinds[i]; gc_sp[i].bi = -1; }
    gc_add_chunk(GC_FIRST);
    if (!(gc_stack = (gc_range *)rt_sys_mem(GC_STACK0 * sizeof(gc_range)))) gc_oom();
    gc_cap = GC_STACK0;
    gc_large_limit = (size_t)4 << 20;
#ifdef GC_STRESS_ON
    { /* SHIKRA_GC_STRESS=N: collect every N-th allocation (stress builds: default 1; the tool,
       * whose interpreter enables collection: default 0; 0 turns stress off) */
        const char *e = rt_sys_getenv("SHIKRA_GC_STRESS");
#ifdef RT_GC_STRESS
        gc_stress = 1;
#endif
        if (e) for (gc_stress = 0; *e >= '0' && *e <= '9'; e++) gc_stress = gc_stress * 10 + (*e - '0');
        gc_countdown = gc_stress;
    }
#endif
    gc_ready = 1;
}

/* The fast-path globals mirror the active headered and pair spaces. */
static gc_space *gc_hs(void) { return &gc_sp[gc_immortal == 2 ? S_PHDR : gc_immortal ? S_IHDR : S_HDR]; }
static gc_space *gc_ps(void) { return &gc_sp[gc_immortal == 2 ? S_PPAIR : gc_immortal ? S_IPAIR : S_PAIR]; }
static void gc_sync_in(void) { gc_hs()->cur = rt_hp; gc_ps()->cur = rt_pp; }
static void gc_sync_out(void) {
    rt_hp = gc_hs()->cur; rt_hlim = gc_hs()->end;
    rt_pp = gc_ps()->cur; rt_plim = gc_ps()->end;
#ifdef GC_STRESS_ON
    if (gc_stress && gc_enabled && !gc_immortal) { rt_hlim = rt_hp; rt_plim = rt_pp; } /* every allocation takes the slow path */
#endif
}
RT_FN void rt_gc_set_immortal(int on) {
    if (gc_ready) gc_sync_in();
    gc_immortal = on;
    if (gc_ready) gc_sync_out();
}

/* ---- marking ---- */
static void gc_push(obj *lo, obj *hi) {
    if (lo >= hi) return;
    if (gc_top == gc_cap) { /* grow; when that fails the object stays marked but unscanned */
        size_t n = 2 * gc_cap;
        gc_range *s = n <= GC_STACKMAX ? (gc_range *)rt_sys_mem(n * sizeof(gc_range)) : 0;
        if (!s) { gc_ovf = 1; return; }
        rt_memcpy(s, gc_stack, gc_cap * sizeof(gc_range));
        rt_sys_free(gc_stack, gc_cap * sizeof(gc_range));
        gc_stack = s;
        gc_cap = n;
    }
    gc_stack[gc_top].lo = lo;
    gc_stack[gc_top++].hi = hi;
}
static void gc_mark_any(uintptr_t a);
static void gc_mark(obj x);
/* Push the obj fields of a marked object; raw pointers (string and port buffers) are
 * marked on the spot (they point to raw buffers or static data, so this cannot recurse). */
static void gc_scan(char *p, int pair) {
    uintptr_t h, t;
    if (pair) { gc_push((obj *)p, (obj *)p + 2); return; }
    h = *(uintptr_t *)p;
    t = h & 0xFF;
    if (t < 16) gc_push((obj *)p + 1, (obj *)p + 1 + (h >> 16));
    else if (t == RT_T_STRING) gc_mark_any((uintptr_t)((rt_string *)p)->buf);
    else if (t == RT_T_PORT) { gc_mark_any((uintptr_t)((rt_port *)p)->buf); gc_mark(((rt_port *)p)->name); }
    else if (t == RT_T_PROC) gc_push(((rt_proc *)p)->free, ((rt_proc *)p)->free + (h >> 16));
}
static void gc_mark_obj(gc_block *b, char *base, char *p, int pair) {
    size_t off = (size_t)(p - base), g = off >> 3, l, last;
    uintptr_t bit = (uintptr_t)1 << (g & 63);
    if (b->mark[g >> 6] & bit) return;
    b->mark[g >> 6] |= bit;
    last = (off + (pair ? sizeof(rt_pair) : gc_size(p)) - 1) / GC_LINE;
    for (l = off / GC_LINE; l <= last; l++) b->line[l] |= L_MARK;
    gc_scan(p, pair);
}
static gc_block *gc_find(uintptr_t p, char **base) {
    int i;
    if (p < gc_lo || p >= gc_hi) return 0;
    for (i = 0; i < gc_nchunks; i++) {
        uintptr_t off = p - (uintptr_t)gc_chunks[i].base;
        if (off < gc_chunks[i].n * GC_BLOCK) {
            *base = gc_chunks[i].base + (off & ~(uintptr_t)(GC_BLOCK - 1));
            return &gc_chunks[i].b[off / GC_BLOCK];
        }
    }
    return 0;
}
/* Large objects: binary search of the sorted table; interior: a may point into the object. */
static void gc_large_mark(uintptr_t a, int interior) {
    size_t lo = 0, hi = gc_nlarge, mid;
    gc_large *L;
    uintptr_t o;
    while (lo < hi) {
        mid = (lo + hi) / 2;
        if ((uintptr_t)gc_larges[mid] <= a) lo = mid + 1;
        else hi = mid;
    }
    if (!lo) return;
    L = gc_larges[lo - 1];
    o = (uintptr_t)L + GC_LHDR;
    if (interior ? a < o || a >= o + L->bytes : a != o) return;
    if (L->mark || L->imm) return;
    L->mark = 1;
    gc_scan((char *)o, 0);
}
/* Precise: x is a value from a root range or a heap field. Anything that is not a valid
 * object (static data, stale words in a pair cell) is ignored. */
static void gc_mark(obj x) {
    uintptr_t w = RT_W(x), p;
    char *base;
    gc_block *b;
    size_t g;
    switch (w & 7) {
    case 1: p = w - 1; break;
    case 5: p = w - 5; break;
    case 3: p = w - 3 - sizeof(uintptr_t); break;
    default: return;
    }
    if (!(b = gc_find(p, &base))) { gc_large_mark(p, 0); return; }
    g = (p - (uintptr_t)base) >> 3;
    if (b->kind == GC_PAIR) {
        if ((w & 7) == 1 && !(p & 15) && b->line[(p - (uintptr_t)base) / GC_LINE]) gc_mark_obj(b, base, (char *)p, 1);
    } else if (b->kind == GC_HDR && (w & 7) != 1 && (b->start[g >> 6] >> (g & 63) & 1))
        gc_mark_obj(b, base, (char *)p, 0);
}
/* Conservative: a is any word; it keeps the object it points at or into alive. */
static void gc_mark_any(uintptr_t a) {
    char *base, *p;
    gc_block *b;
    size_t off, g, i;
    uintptr_t w;
    if (!(b = gc_find(a, &base))) { if (gc_nlarge) gc_large_mark(a, 1); return; }
    off = a - (uintptr_t)base;
    if (b->kind == GC_PAIR) {
        if (b->line[off / GC_LINE]) gc_mark_obj(b, base, base + (off & ~(size_t)15), 1);
        return;
    }
    if (b->kind != GC_HDR) return; /* free or immortal */
    g = off >> 3;
    i = g >> 6;
    w = b->start[i] & (~(uintptr_t)0 >> (63 - (g & 63))); /* starts at or below g */
    while (!w) {
        if (!i) return;
        w = b->start[--i];
    }
    p = base + (i * 64 + (size_t)gc_hibit(w)) * 8;
    if (a < (uintptr_t)p + gc_size(p)) gc_mark_obj(b, base, p, 0);
}
static void gc_drain(void) {
    while (gc_top) {
        obj *lo = gc_stack[gc_top - 1].lo, *hi = gc_stack[--gc_top].hi;
        if (hi - lo > 64) { gc_push(lo + 64, hi); hi = lo + 64; } /* long vectors in slices */
        while (hi > lo) gc_mark(*--hi);                         /* first field popped first */
    }
}
/* After a mark-stack overflow: rescan every marked object until a pass overflows no more.
 * Descending address order: objects are usually allocated after what they point to, so
 * the unscanned objects a pass leaves behind mostly lie ahead of it in the same pass. */
static void gc_rescan(void) {
    int c;
    size_t j, k;
    while (gc_ovf) {
        gc_ovf = 0;
        for (k = gc_nlarge; k-- > 0;)
            if (gc_larges[k]->mark) { gc_scan((char *)gc_larges[k] + GC_LHDR, 0); gc_drain(); }
        for (c = gc_nchunks; c-- > 0;)
            for (j = gc_chunks[c].n; j-- > 0;) {
                gc_block *b = &gc_chunks[c].b[j];
                if (b->kind == GC_FREE || (b->kind & GC_IMM)) continue;
                for (k = GC_BLOCK / 8; k-- > 0;)
                    if (b->mark[k >> 6] >> (k & 63) & 1) {
                        gc_scan(gc_chunks[c].base + j * GC_BLOCK + k * 8, b->kind == GC_PAIR);
                        gc_drain();
                    }
            }
    }
}

/* ---- collection ---- */
static void gc_run(void *arg) {
    uintptr_t here = 0, *sp, *top;
    size_t total = 0, nfree = 0, j, k, l, m, live_large = 0;
    int c;
    (void)arg;
    for (c = 0; c < S_N; c++) /* leave every run walkable */
        if (!(gc_sp[c].kind & GC_PAIR) && gc_sp[c].cur && gc_sp[c].cur < gc_sp[c].end) *(uintptr_t *)gc_sp[c].cur = 0;
    for (c = 0; c < gc_nchunks; c++) /* object starts of everything allocated since the last collection */
        for (j = 0; j < gc_chunks[c].n; j++) {
            gc_block *b = &gc_chunks[c].b[j];
            char *base = gc_chunks[c].base + j * GC_BLOCK;
            if (b->kind != GC_HDR) continue;
            for (l = 0; l < GC_NLINES; l = m) {
                char *a = base + l * GC_LINE, *end;
                for (m = l + 1; m < GC_NLINES && b->line[m] == L_NEW; m++) {}
                if (b->line[l] != L_RUN) continue;
                for (end = base + m * GC_LINE; a < end && *(uintptr_t *)a; a += gc_size(a)) {
                    k = (size_t)(a - base) >> 3;
                    b->start[k >> 6] |= (uintptr_t)1 << (k & 63);
                }
            }
        }
    /* roots: registered ranges, immortal objects, then the stack and the registers that
     * rt_sys_catch saved in its frame above this one */
    for (c = 0; c < gc_nroots; c++) {
        obj *r;
        for (r = gc_roots[c][0]; r < gc_roots[c][1]; r++) { gc_mark(*r); gc_drain(); }
    }
    for (c = 0; c < gc_ndroots; c++) {
        obj *r;
        for (r = *gc_droots[c][0]; r < *gc_droots[c][1]; r++) { gc_mark(*r); gc_drain(); }
    }
    for (c = 0; c < gc_nchunks; c++)
        for (j = 0; j < gc_chunks[c].n; j++) {
            gc_block *b = &gc_chunks[c].b[j];
            char *a = gc_chunks[c].base + j * GC_BLOCK, *end = a + GC_BLOCK;
            if (b->kind == (GC_PAIR | GC_IMM))
                for (; a < end; a += sizeof(rt_pair)) { gc_scan(a, 1); gc_drain(); }
            else if (b->kind == (GC_HDR | GC_IMM))
                for (; a < end && *(uintptr_t *)a; a += gc_size(a)) { gc_scan(a, 0); gc_drain(); }
        }
    for (k = 0; k < gc_nlarge; k++)
        if (gc_larges[k]->imm == 1) { gc_scan((char *)gc_larges[k] + GC_LHDR, 0); gc_drain(); }
    sp = (uintptr_t *)((uintptr_t)&here & ~(uintptr_t)7);
    top = (uintptr_t *)((uintptr_t)gc_stack_base & ~(uintptr_t)7);
    if (sp > top) { uintptr_t *t = sp; sp = top; top = t; }
    for (; sp <= top; sp++) { gc_mark_any(*sp); gc_drain(); }
    gc_rescan();
    /* sweep: lines without a marked object are free; start bits survive for marked objects */
    for (c = 0; c < gc_nchunks; c++)
        for (j = 0; j < gc_chunks[c].n; j++) {
            gc_block *b = &gc_chunks[c].b[j];
            size_t n = 0;
            total += GC_NLINES;
            if (b->kind == GC_FREE) { nfree += GC_NLINES; continue; }
            if (b->kind & GC_IMM) continue;
            for (l = 0; l < GC_NLINES; l++) {
                b->line[l] = b->line[l] & L_MARK ? L_LIVE : L_FREE;
                n += !b->line[l];
            }
            for (k = 0; k < GC_NWORDS; k++) { b->start[k] &= b->mark[k]; b->mark[k] = 0; }
            if (n == GC_NLINES) b->kind = GC_FREE;
            nfree += n;
        }
    for (j = k = 0; j < gc_nlarge; j++) {
        gc_large *L = gc_larges[j];
        if (L->mark || L->imm) { L->mark = 0; gc_larges[k++] = L; live_large += L->size; }
        else rt_sys_free(L, L->size);
    }
    gc_nlarge = k;
    for (c = 0; c < S_N; c++) { gc_sp[c].cur = gc_sp[c].end = 0; gc_sp[c].ci = 0; gc_sp[c].bi = -1; }
    gc_claimed = gc_large_since = 0;
    /* growth: keep at least half of the block heap free; large objects may allocate as much
     * as is live (at least the block heap size, at least 4 MB) before the next collection */
    if (2 * (total - nfree) > total) gc_add_chunk(gc_nblocks);
    gc_large_limit = live_large > gc_nblocks * GC_BLOCK ? live_large : gc_nblocks * GC_BLOCK;
    if (gc_large_limit < (size_t)4 << 20) gc_large_limit = (size_t)4 << 20;
}
/* rt_sys_catch saves the callee-saved registers on the stack before gc_run scans it. */
static void gc_collect_now(void) {
    rt_jmpbuf jb;
    rt_sys_catch(&jb, gc_run, 0);
}
#ifndef SHIKRA_PROGRAM
RT_FN int rt_gc_immortal_pair(obj x) {
    char *base;
    gc_block *b;
    if (!gc_ready) return 0;
    b = gc_find(RT_W(x) - 1, &base);
    return b && (b->kind & ~GC_PURE) == (GC_PAIR | GC_IMM);
}
#endif
RT_FN void rt_gc_collect(void) {
    if (!gc_ready) return;
    gc_sync_in();
    gc_collect_now();
    gc_sync_out();
}

/* ---- allocation ---- */
static int gc_next_block(gc_space *s) {
    while (s->ci < gc_nchunks) {
        gc_chunk *c = &gc_chunks[s->ci];
        gc_block *b;
        if (++s->bi >= (intptr_t)c->n) { s->ci++; s->bi = -1; continue; }
        b = &c->b[s->bi];
        if (b->kind == GC_FREE || (b->kind == s->kind && (s == &gc_sp[S_HDR] || s == &gc_sp[S_PAIR]))) {
            b->kind = (unsigned char)s->kind; /* overflow and immortal spaces take free blocks only */
            s->cur = s->end = c->base + (size_t)s->bi * GC_BLOCK;
            return 1;
        }
    }
    return 0;
}
/* Move space s to the next run of free lines that holds `bytes`, collecting or growing
 * the heap when there is none. */
static void gc_refill(gc_space *s, size_t bytes) {
    for (;;) {
        if (!(s->kind & GC_PAIR) && s->cur && s->cur < s->end) *(uintptr_t *)s->cur = 0; /* run terminator */
        if (s->bi >= 0 && s->ci < gc_nchunks) {
            gc_chunk *c = &gc_chunks[s->ci];
            gc_block *b = &c->b[s->bi];
            char *base = c->base + (size_t)s->bi * GC_BLOCK;
            size_t l = (size_t)(s->end - base) / GC_LINE, m;
            while (l < GC_NLINES && b->line[l]) l++;
            if (l < GC_NLINES) {
                for (m = l; m < GC_NLINES && !b->line[m]; m++) b->line[m] = L_NEW;
                if (s->kind == GC_HDR) b->line[l] = L_RUN;
                s->cur = base + l * GC_LINE;
                s->end = base + m * GC_LINE;
                if (s->kind & GC_IMM) rt_memset(s->cur, 0, (size_t)(s->end - s->cur));
                gc_claimed += m - l;
                if ((size_t)(s->end - s->cur) >= bytes) return;
                continue;
            }
        }
        if (gc_next_block(s)) continue;
        if (gc_enabled && gc_claimed) gc_collect_now(); /* resets every space */
        else gc_add_chunk(gc_nblocks);
    }
}
static void *gc_large_alloc(size_t bytes) {
    size_t size = bytes + GC_LHDR, i;
    gc_large *L;
    if (gc_enabled && gc_large_since > gc_large_limit) gc_collect_now();
    if (!(L = (gc_large *)rt_sys_mem(size))) gc_oom();
    L->size = size;
    L->bytes = bytes;
    L->mark = 0;
    L->imm = gc_immortal;
    gc_large_since += size;
    if (gc_nlarge == gc_caplarge) {
        size_t n = gc_caplarge ? 2 * gc_caplarge : 64;
        gc_large **t = (gc_large **)rt_sys_mem(n * sizeof(gc_large *));
        if (!t) gc_oom();
        if (gc_caplarge) {
            rt_memcpy(t, gc_larges, gc_nlarge * sizeof(gc_large *));
            rt_sys_free(gc_larges, gc_caplarge * sizeof(gc_large *));
        }
        gc_larges = t;
        gc_caplarge = n;
    }
    for (i = gc_nlarge; i > 0 && (uintptr_t)gc_larges[i - 1] > (uintptr_t)L; i--) gc_larges[i] = gc_larges[i - 1];
    gc_larges[i] = L;
    gc_nlarge++;
    return (char *)L + GC_LHDR;
}
RT_FN void *rt_alloc_slow(size_t bytes, int pair_stream) {
    gc_space *s;
    char *p;
    if (!gc_ready) gc_setup();
    gc_sync_in();
#ifdef GC_STRESS_ON
    if (gc_stress && gc_enabled && !gc_immortal && --gc_countdown <= 0) { gc_countdown = gc_stress; gc_collect_now(); }
#endif
    if (bytes > GC_MEDIUM) p = (char *)gc_large_alloc(bytes);
    else {
        s = pair_stream ? gc_ps() : gc_hs();
        if ((size_t)(s->end - s->cur) < bytes && !pair_stream && !gc_immortal && bytes > GC_LINE) s = &gc_sp[S_OVF];
        while ((size_t)(s->end - s->cur) < bytes) gc_refill(s, bytes);
        p = s->cur;
        s->cur += bytes;
    }
    gc_sync_out();
    return p;
}
#endif
