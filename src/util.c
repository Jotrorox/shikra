/* util.c -- arena, string buffer, hash table, file and embed helpers (tool code). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "util.h"
#include "embed.h"

void die(const char *fmt, ...) {
    va_list ap;
    fflush(stdout);
    fputs("shikra: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) die("out of memory");
    return p;
}
void *xrealloc(void *p, size_t n) {
    p = realloc(p, n ? n : 1);
    if (!p) die("out of memory");
    return p;
}

/* ---- arena ---- */
struct arena_block { arena_block *next; };
#define ARENA_BLOCK ((size_t)1 << 20)
void *arena_alloc(arena *a, size_t n) {
    char *p;
    n = (n + 15) & ~(size_t)15;
    if ((size_t)(a->end - a->p) < n || !a->p) {
        size_t sz = n + 16 > ARENA_BLOCK ? n + 16 : ARENA_BLOCK;
        arena_block *b = (arena_block *)xmalloc(sz);
        b->next = a->head;
        a->head = b;
        a->p = (char *)b + 16;
        a->end = (char *)b + sz;
    }
    p = a->p;
    a->p += n;
    memset(p, 0, n);
    return p;
}
char *arena_strndup(arena *a, const char *s, size_t n) {
    char *d = (char *)arena_alloc(a, n + 1);
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}
char *arena_strdup(arena *a, const char *s) { return arena_strndup(a, s, strlen(s)); }

/* ---- string buffer ---- */
static void sb_grow(sbuf *b, size_t extra) {
    if (b->len + extra + 1 > b->cap) {
        b->cap = (b->len + extra + 1) * 2;
        b->s = (char *)xrealloc(b->s, b->cap);
    }
}
void sb_putn(sbuf *b, const char *s, size_t n) {
    sb_grow(b, n);
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = 0;
}
void sb_putc(sbuf *b, int c) { char ch = (char)c; sb_putn(b, &ch, 1); }
void sb_puts(sbuf *b, const char *s) { sb_putn(b, s, strlen(s)); }
void sb_printf(sbuf *b, const char *fmt, ...) {
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(0, 0, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    sb_grow(b, (size_t)n);
    va_start(ap, fmt);
    vsnprintf(b->s + b->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
}
void sb_free(sbuf *b) { free(b->s); b->s = 0; b->len = b->cap = 0; }

/* ---- hash table ---- */
static size_t ht_hash(const char *s) {
    size_t h = 14695981039346656037u;
    while (*s) h = (h ^ (unsigned char)*s++) * 1099511628211u;
    return h;
}
static htab_entry *ht_slot(htab_entry *e, size_t cap, const char *key) {
    size_t i = ht_hash(key) & (cap - 1);
    while (e[i].key && strcmp(e[i].key, key) != 0) i = (i + 1) & (cap - 1);
    return &e[i];
}
void *ht_get(const htab *h, const char *key) {
    return h->cap ? ht_slot(h->e, h->cap, key)->val : NULL;
}
void ht_put(htab *h, const char *key, void *val) {
    htab_entry *s;
    if (2 * (h->n + 1) > h->cap) {
        size_t ncap = h->cap ? 2 * h->cap : 64, i;
        htab_entry *ne = (htab_entry *)xmalloc(ncap * sizeof *ne);
        memset(ne, 0, ncap * sizeof *ne);
        for (i = 0; i < h->cap; i++)
            if (h->e[i].key) *ht_slot(ne, ncap, h->e[i].key) = h->e[i];
        free(h->e);
        h->e = ne;
        h->cap = ncap;
    }
    s = ht_slot(h->e, h->cap, key);
    if (!s->key) h->n++;
    s->key = key;
    s->val = val;
}

/* ---- files ---- */
char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    sbuf b = {0};
    char tmp[8192];
    size_t n;
    if (!f) return NULL;
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) sb_putn(&b, tmp, n);
    fclose(f);
    if (!b.s) sb_putn(&b, "", 0);
    if (len) *len = b.len;
    return b.s;
}

char *embed_text(const char *name, size_t *len) {
    const embed_file *f;
    for (f = embed_files; f->name; f++)
        if (strcmp(f->name, name) == 0) {
            sbuf b = {0};
            const char *const *c;
            sb_putn(&b, "", 0);
            for (c = f->chunks; *c; c++) sb_puts(&b, *c);
            if (len) *len = b.len;
            return b.s;
        }
    return NULL;
}
