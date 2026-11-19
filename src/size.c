/* size.c -- `shikra size app` (spec §9.3, §11): bytes per library and per procedure of a
 * binary built by `shikra build`, so that growth can be traced to its cause.
 *
 * The tool reads the executable itself (no nm/size/otool): Mach-O 64 and ELF64,
 * little-endian, x86-64 and arm64. Every byte of the file is attributed once:
 *   - bytes of a section with contents belong to the symbol at or below them (symbols of a
 *     section sorted by address; a symbol's size runs to the next symbol or the section end;
 *     bytes before the first symbol are the section's own);
 *   - everything else (headers, load commands, the symbol and string tables, segment
 *     padding, code signature, non-allocated ELF sections) is format overhead.
 * Symbols are mapped back to their origin through the emitter's naming contract (DESIGN.md
 * "M4-3 as built"): P_<library>__<procedure>[__<inner>...] (the library part never contains
 * "__"), P_init the program body, C_<...> static closures, rt_lits (the literals), rt_lit_pairs,
 * rt_static_syms, rt_g; runtime functions and variables by the runtime source file that
 * defines them (looked up in the runtime sources embedded in the tool). Procedures that were
 * inlined or contified have no code of their own: their bytes are in their host's. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "util.h"

typedef struct { const char *name; unsigned long long addr, size, off; int nobits, alloc; } sz_sect;
typedef struct { const char *name; unsigned long long addr; int sect; } sz_sym;
typedef struct { const char *group, *name; unsigned long long bytes; int kind, zf; } sz_item; /* kind: K_*; zf: zero-fill */
enum { K_LIB, K_RUNTIME, K_DATA, K_OTHER };

static unsigned char *fb;
static size_t flen;
static sz_sect *sects;
static int nsects;
static sz_sym *syms;
static size_t nsyms, capsyms;
static sz_item *items;
static size_t nitems, capitems;
static unsigned long long symtab_bytes; /* symbol + string tables, part of the overhead */

static unsigned long long rd(size_t off, int n) {
    unsigned long long v = 0;
    int i;
    if (off + (size_t)n > flen) die("size: truncated or malformed binary");
    for (i = n - 1; i >= 0; i--) v = v << 8 | fb[off + (size_t)i];
    return v;
}
static const char *rdstr(size_t off) {
    size_t e = off;
    if (off >= flen) return "";
    while (e < flen && fb[e]) e++;
    return e < flen ? (const char *)fb + off : "";
}
static void add_sym(const char *name, unsigned long long addr, int sect) {
    if (!*name || *name == '$') return; /* Arm mapping symbols ($x, $d) */
    if (nsyms == capsyms) { capsyms = capsyms ? 2 * capsyms : 256; syms = (sz_sym *)xrealloc(syms, capsyms * sizeof *syms); }
    syms[nsyms].name = name; syms[nsyms].addr = addr; syms[nsyms++].sect = sect;
}

/* ---- Mach-O 64 ---- */
static const char *parse_macho(void) {
    unsigned long long ncmds = rd(16, 4), cpu = rd(4, 4), i, j;
    size_t lc = 32;
    sects = (sz_sect *)xmalloc(256 * sizeof *sects);
    for (i = 0; i < ncmds; i++) {
        unsigned long long cmd = rd(lc, 4), csz = rd(lc + 4, 4);
        if (cmd == 0x19) { /* LC_SEGMENT_64 */
            unsigned long long ns = rd(lc + 64, 4);
            for (j = 0; j < ns && nsects < 255; j++) {
                size_t s = lc + 72 + (size_t)j * 80;
                char *nm = (char *)xmalloc(40);
                unsigned long long fl = rd(s + 64, 4) & 0xff;
                snprintf(nm, 40, "%.16s,%.16s", (const char *)fb + s + 16, (const char *)fb + s);
                sects[nsects].name = nm;
                sects[nsects].addr = rd(s + 32, 8);
                sects[nsects].size = rd(s + 40, 8);
                sects[nsects].off = rd(s + 48, 4);
                sects[nsects].nobits = fl == 1 || fl == 0xc || fl == 0x12; /* zerofill kinds */
                sects[nsects++].alloc = 1;
            }
        } else if (cmd == 2) { /* LC_SYMTAB */
            unsigned long long symoff = rd(lc + 8, 4), n = rd(lc + 12, 4), stroff = rd(lc + 16, 4), strsz = rd(lc + 20, 4);
            symtab_bytes = n * 16 + strsz;
            for (j = 0; j < n; j++) {
                size_t e = (size_t)(symoff + j * 16);
                unsigned type = (unsigned)rd(e + 4, 1), sect = (unsigned)rd(e + 5, 1);
                const char *name = rdstr((size_t)(stroff + rd(e, 4)));
                if ((type & 0xe0) || (type & 0x0e) != 0x0e || !sect) continue; /* stabs; not defined in a section */
                add_sym(name[0] == '_' ? name + 1 : name, rd(e + 8, 8), (int)sect - 1);
            }
        }
        lc += (size_t)csz;
    }
    return cpu == 0x0100000c ? "Mach-O arm64 (dynamically linked, libSystem)" : cpu == 0x01000007 ? "Mach-O x86-64 (dynamically linked, libSystem)" : "Mach-O (unknown CPU)";
}

/* ---- ELF64 ---- */
static const char *parse_elf(void) {
    unsigned long long shoff = rd(0x28, 8), mach = rd(0x12, 2), shnum = rd(0x3c, 2), shstr = rd(0x3e, 2), i, j;
    size_t strbase;
    if (!shoff || !shnum || shstr >= shnum) die("size: ELF file without section headers");
    strbase = (size_t)rd((size_t)(shoff + shstr * 64 + 24), 8);
    sects = (sz_sect *)xmalloc((size_t)shnum * sizeof *sects);
    nsects = (int)shnum;
    for (i = 0; i < shnum; i++) {
        size_t s = (size_t)(shoff + i * 64);
        unsigned long long type = rd(s + 4, 4);
        sects[i].name = rdstr(strbase + (size_t)rd(s, 4));
        sects[i].addr = rd(s + 16, 8);
        sects[i].off = rd(s + 24, 8);
        sects[i].size = rd(s + 32, 8);
        sects[i].nobits = type == 8; /* SHT_NOBITS */
        sects[i].alloc = (rd(s + 8, 8) & 2) && type != 0; /* SHF_ALLOC */
    }
    for (i = 0; i < shnum; i++) {
        size_t s = (size_t)(shoff + i * 64);
        if (rd(s + 4, 4) == 2) { /* SHT_SYMTAB; its sh_link is the string table */
            size_t str = (size_t)sects[rd(s + 40, 4)].off;
            unsigned long long n = sects[i].size / 24;
            symtab_bytes += sects[i].size + sects[rd(s + 40, 4)].size;
            for (j = 1; j < n; j++) {
                size_t e = (size_t)(sects[i].off + j * 24);
                unsigned info = (unsigned)rd(e + 4, 1), shndx = (unsigned)rd(e + 6, 2);
                if ((info & 15) > 2 || !shndx || shndx >= shnum) continue; /* sections, files; undefined, abs */
                add_sym(rdstr(str + (size_t)rd(e, 4)), rd(e + 8, 8), (int)shndx);
            }
        }
    }
    return mach == 62 ? "ELF x86-64" : mach == 183 ? "ELF arm64" : "ELF (unknown CPU)";
}
/* The build mode (DESIGN.md "Integration"): an ELF program interpreter means dynamic linking;
 * shikra's own entry shikra_start a static executable without libc. */
static const char *elf_mode(void) {
    unsigned long long phoff = rd(0x20, 8), phnum = rd(0x38, 2), i;
    size_t k;
    for (i = 0; i < phnum; i++) if (rd((size_t)(phoff + i * 56), 4) == 3) return "hosted: dynamically linked, C library"; /* PT_INTERP */
    for (k = 0; k < nsyms; k++) if (!strcmp(syms[k].name, "shikra_start")) return "static, no libc";
    return "hosted: static, C library";
}

/* ---- runtime components: which runtime source defines a name ---- */
static htab rtnames; /* name -> component label */
static const char *rt_component(const char *file, const char *section) {
    if (!strcmp(file, "src/rt_io.c")) return section && strstr(section, "printer") ? "printer" : "ports";
    if (!strcmp(file, "src/rt_num.c")) return section && strstr(section, "decimal") ? "float printing and parsing" : "numbers";
    if (!strcmp(file, "src/rt_gc.c")) return "collector";
    if (!strcmp(file, "src/rt_read.c")) return "reader";
    if (!strcmp(file, "src/rt_str.c")) return "strings and chars";
    if (!strcmp(file, "src/rt_sys.c")) return "system layer";
    if (!strcmp(file, "src/rt_unicode.c")) return "unicode tables";
    if (!strcmp(file, "src/rt_ext.c")) return "extensions (shikra ...)";
    if (!strcmp(file, "src/rt.h")) return "inline helpers (rt.h)";
    if (!section) return "startup and exit";
    if (strstr(section, "system interface")) return "system layer";
    if (strstr(section, "error") || strstr(section, "raise")) return "errors";
    if (strstr(section, "calling") || strstr(section, "dynamic") || strstr(section, "escape") || strstr(section, "values")) return "control";
    if (strstr(section, "symbol")) return "symbols";
    return "data structures";
}
static int is_idc(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }
static void scan_runtime(void) {
    static const char *const files[] = {"src/rt.h", "src/rt_sys.c", "src/rt_gc.c", "src/rt_core.c", "src/rt_str.c",
                                        "src/rt_num.c", "src/rt_io.c", "src/rt_read.c", "src/rt_unicode.c", "src/rt_ext.c", 0};
    int f;
    for (f = 0; files[f]; f++) {
        char *t = embed_text(files[f], NULL), *line, *next, *section = NULL;
        if (!t) continue;
        for (line = t; line && *line; line = next) {
            char *e, *p, *q;
            if ((next = strchr(line, '\n'))) *next++ = 0;
            if (!strncmp(line, "/* ---- ", 8) || !strncmp(line, "/* ==== ", 8)) { section = line + 8; continue; }
            if (!is_idc((unsigned char)line[0]) || !strncmp(line, "typedef", 7) || !strncmp(line, "RT_EXTVAR", 9) || !strncmp(line, "extern", 6)) continue;
            /* a definition at column 0: the identifier before the first '(' (a function with a
             * body on this line) or before '[', '=' or ';' (a variable) */
            for (e = line; *e && *e != '(' && *e != '[' && *e != '=' && *e != ';'; e++) {}
            if (!*e || (*e == '(' && !strchr(e, '{')) || (*e == ';' && strchr(line, '('))) continue;
            if (f == 0 && *e != '(') continue; /* rt.h: only its inline functions are defined there */
            for (q = e; q > line && q[-1] == ' '; q--) {}
            for (p = q; p > line && is_idc((unsigned char)p[-1]); p--) {}
            if (p == q || !strncmp(p, "RT_", 3)) continue;
            *q = 0;
            if (!ht_get(&rtnames, p)) ht_put(&rtnames, p, (void *)rt_component(files[f], section)); /* t is never freed */
        }
    }
}

/* ---- classification of one symbol ---- */
static arena sz_arena;
static htab itemix; /* "kind zf group\001name" -> item index + 1 */
static int zf_now;  /* the current section is zero-fill (not in the file) */
static void add_item(int kind, const char *group, const char *name, unsigned long long bytes) {
    sbuf k = {0};
    size_t i;
    if (!bytes) return;
    sb_printf(&k, "%d %d %s\001%s", kind, zf_now, group, name);
    if ((i = (size_t)ht_get(&itemix, k.s))) { items[i - 1].bytes += bytes; sb_free(&k); return; }
    if (nitems == capitems) { capitems = capitems ? 2 * capitems : 256; items = (sz_item *)xrealloc(items, capitems * sizeof *items); }
    items[nitems].kind = kind; items[nitems].group = group; items[nitems].name = name;
    items[nitems].bytes = bytes; items[nitems].zf = zf_now;
    ht_put(&itemix, arena_strdup(&sz_arena, k.s), (void *)(nitems + 1));
    nitems++;
    sb_free(&k);
}
static const char *sect_label(const char *s) {
    if (strstr(s, "stubs") || strstr(s, "got") || strstr(s, "plt") || strstr(s, "symbol_ptr") || strstr(s, "dyn") || strstr(s, "rela") || strstr(s, "interp"))
        return "dynamic linking (stubs, GOT)";
    if (strstr(s, "unwind") || strstr(s, "eh_frame")) return "unwind tables";
    if (strstr(s, "cstring")) return "C strings (runtime messages, string literal text)";
    if (strstr(s, "text")) return "code without a symbol";
    if (strstr(s, "const") || strstr(s, "rodata")) return "constants without a symbol (C compiler tables)";
    return s;
}
static int shikra_seen;
static void classify(const char *raw, const char *sect, unsigned long long bytes) {
    char *n = arena_strdup(&sz_arena, raw), *dot, *sep;
    const char *comp;
    if ((dot = strchr(n, '.')) && dot != n) *dot = 0; /* clones and statics: f.cold, f.llvm.N, gc_oom.msg */
    if (!strcmp(n, "P_init") || !strcmp(n, "rt_program_main")) { shikra_seen = 1; add_item(K_LIB, "program", "(program body)", bytes); return; }
    if (!strcmp(n, "rt_engine_raise")) { add_item(K_LIB, "scheme_base", "raise (runtime entry)", bytes); return; }
    if (!strcmp(n, "rt_engine_travel")) { add_item(K_LIB, "shikra_dynamic", "travel-to! (runtime entry)", bytes); return; }
    if ((!strncmp(n, "P_", 2) || !strncmp(n, "C_", 2)) && (sep = strstr(n + 2, "__"))) {
        const char *lib = arena_strndup(&sz_arena, n + 2, (size_t)(sep - n - 2));
        shikra_seen = 1;
        if (!strcmp(lib, "main")) lib = "program";
        if (n[0] == 'C') {
            sbuf b = {0};
            sb_printf(&b, "%s (static closure)", sep + 2);
            add_item(K_LIB, lib, arena_strdup(&sz_arena, b.s), bytes);
            sb_free(&b);
        } else add_item(K_LIB, lib, sep + 2, bytes);
        return;
    }
    if ((n[0] == 'L' && n[1] >= '0' && n[1] <= '9') || !strcmp(n, "rt_lits")) { add_item(K_DATA, "literals (strings, vectors, flonums, symbols)", "", bytes); return; }
    if (!strcmp(n, "rt_lit_pairs")) { add_item(K_DATA, "literal pairs (rt_lit_pairs)", "", bytes); return; }
    if (!strcmp(n, "rt_static_syms")) { add_item(K_DATA, "static symbol list (rt_static_syms)", "", bytes); return; }
    if (!strcmp(n, "rt_g")) { add_item(K_DATA, "program globals (rt_g)", "", bytes); return; }
    if ((comp = (const char *)ht_get(&rtnames, n))) { shikra_seen |= !strncmp(n, "rt_", 3); add_item(K_RUNTIME, comp, n, bytes); return; }
    if (!strncmp(n, "rt_", 3)) { shikra_seen = 1; add_item(K_RUNTIME, "data structures", n, bytes); return; }
    if (!strncmp(n, "OUTLINED_FUNCTION_", 18)) { add_item(K_OTHER, "code sequences the C compiler outlined (shared)", "", bytes); return; }
    add_item(K_OTHER, strstr(sect, "text") ? "C library and startup code" : "C library and other data", n, bytes);
}

/* ---- attribution: every byte of every section with contents ---- */
static int sym_cmp(const void *a, const void *b) {
    const sz_sym *x = (const sz_sym *)a, *y = (const sz_sym *)b;
    if (x->sect != y->sect) return x->sect < y->sect ? -1 : 1;
    if (x->addr != y->addr) return x->addr < y->addr ? -1 : 1;
    return strcmp(x->name, y->name);
}
static void attribute(void) {
    size_t k = 0;
    int s;
    qsort(syms, nsyms, sizeof *syms, sym_cmp);
    for (s = 0; s < nsects; s++) {
        sz_sect *S = &sects[s];
        unsigned long long at = S->addr, end = S->addr + S->size;
        if (!S->alloc || !S->size) { while (k < nsyms && syms[k].sect <= s) k++; continue; }
        zf_now = S->nobits;
        while (k < nsyms && syms[k].sect < s) k++;
        for (; k < nsyms && syms[k].sect == s; k++) {
            unsigned long long a = syms[k].addr, next = end;
            size_t j;
            if (a < at || a >= end) continue; /* alias at the same address, or outside */
            if (a > at) add_item(K_OTHER, sect_label(S->name), "", a - at);
            for (j = k + 1; j < nsyms && syms[j].sect == s; j++) if (syms[j].addr > a) { next = syms[j].addr < end ? syms[j].addr : end; break; }
            classify(syms[k].name, S->name, next - a);
            at = next;
        }
        if (at < end) add_item(K_OTHER, sect_label(S->name), "", end - at);
    }
    zf_now = 0;
}

/* ---- output ---- */
typedef struct { int kind; const char *group; unsigned long long bytes; } sz_group;
static int item_cmp(const void *a, const void *b) {
    const sz_item *x = (const sz_item *)a, *y = (const sz_item *)b;
    return x->bytes != y->bytes ? (x->bytes > y->bytes ? -1 : 1) : strcmp(x->name, y->name);
}
static int group_cmp(const void *a, const void *b) {
    const sz_group *x = (const sz_group *)a, *y = (const sz_group *)b;
    if (x->kind != y->kind) return x->kind < y->kind ? -1 : 1;
    return x->bytes != y->bytes ? (x->bytes > y->bytes ? -1 : 1) : strcmp(x->group, y->group);
}
static const char *num(unsigned long long v) { /* 12,345 */
    static char buf[8][32];
    static int r;
    char t[32], *o = buf[r = (r + 1) % 8];
    int n = snprintf(t, sizeof t, "%llu", v), i, j = 0;
    for (i = 0; i < n; i++) { if (i && (n - i) % 3 == 0) o[j++] = ','; o[j++] = t[i]; }
    o[j] = 0;
    return o;
}
static const char *lib_title(const char *g) { /* scheme_base -> (scheme base) for the built-in libraries */
    static const char *const builtin[] = {"scheme_base", "scheme_case_lambda", "scheme_char", "scheme_cxr", "scheme_file", "scheme_inexact",
        "scheme_lazy", "scheme_process_context", "scheme_read", "scheme_time", "scheme_write", "shikra_dynamic", 0};
    int i;
    if (!strcmp(g, "program")) return "program";
    for (i = 0; builtin[i]; i++)
        if (!strcmp(g, builtin[i])) {
            char *t = arena_strdup(&sz_arena, g), *p = strchr(t, '_');
            sbuf b = {0};
            *p = ' ';
            if (!strcmp(p + 1, "case_lambda")) strcpy(p + 1, "case-lambda");
            if (!strcmp(p + 1, "process_context")) strcpy(p + 1, "process-context");
            sb_printf(&b, "(%s)", t);
            return arena_strdup(&sz_arena, b.s);
        }
    return g; /* a user library: its mangled id (library name parts joined by '_') */
}

int size_command(const char *path, int all, int tsv) {
    static const char *const heads[] = {"", "(runtime)", "static data", "other"};
    const char *fmt_name;
    unsigned long long infile = 0, zero = 0, ovh;
    sz_group *gs;
    size_t ng = 0, i, j;
    int kind, limit = all ? 1 << 30 : 12;
    if (!(fb = (unsigned char *)read_file(path, &flen))) die("size: cannot read %s", path);
    if (flen >= 32 && rd(0, 4) == 0xfeedfacfu) fmt_name = parse_macho();
    else if (flen >= 64 && !memcmp(fb, "\177ELF", 4) && fb[4] == 2 && fb[5] == 1) {
        sbuf b = {0};
        fmt_name = parse_elf();
        sb_printf(&b, "%s (%s)", fmt_name, elf_mode());
        fmt_name = arena_strdup(&sz_arena, b.s);
        sb_free(&b);
    }
    else if (flen >= 4 && (rd(0, 4) == 0xbebafecau || rd(0, 4) == 0xcafebabeu)) die("size: %s is a universal (fat) binary; give one architecture", path);
    else die("size: %s is not a 64-bit little-endian Mach-O or ELF executable", path);
    if (!nsyms) die("size: %s has no symbol table (stripped?); shikra size needs the symbols that shikra build keeps", path);
    scan_runtime();
    attribute();
    if (!shikra_seen) die("size: %s has no Shikra symbols: it was stripped, or it was not built by shikra build", path);
    for (i = 0; i < nitems; i++) { if (items[i].zf) zero += items[i].bytes; else infile += items[i].bytes; }
    ovh = flen > infile ? flen - infile : 0;
    qsort(items, nitems, sizeof *items, item_cmp);
    if (tsv) {
        printf("kind\tgroup\tname\tbytes\tin_file\n");
        for (i = 0; i < nitems; i++)
            printf("%s\t%s\t%s\t%llu\t%s\n", items[i].kind == K_LIB ? "library" : items[i].kind == K_RUNTIME ? "runtime" : items[i].kind == K_DATA ? "data" : "other",
                   items[i].kind == K_LIB ? lib_title(items[i].group) : items[i].group, items[i].name, items[i].bytes, items[i].zf ? "no" : "yes");
        printf("overhead\tformat overhead\theaders, load commands, symbol and string tables, padding, signature\t%llu\tyes\n", ovh);
        printf("total\tfile\t%s\t%llu\tyes\n", path, (unsigned long long)flen);
        return 0;
    }
    gs = (sz_group *)xmalloc((nitems + 1) * sizeof *gs);
    for (i = 0; i < nitems; i++) {
        if (items[i].zf) continue;
        for (j = 0; j < ng && !(gs[j].kind == items[i].kind && !strcmp(gs[j].group, items[i].group)); j++) {}
        if (j == ng) { gs[ng].kind = items[i].kind; gs[ng].group = items[i].group; gs[ng++].bytes = 0; }
        gs[j].bytes += items[i].bytes;
    }
    qsort(gs, ng, sizeof *gs, group_cmp);
    printf("%s: %s, %s bytes\n\n", path, fmt_name, num(flen));
    printf("%-60s %10s %6s\n", "", "bytes", "%");
    for (kind = K_LIB; kind <= K_OTHER; kind++) {
        unsigned long long sub = 0;
        for (j = 0; j < ng; j++) if (gs[j].kind == kind) sub += gs[j].bytes;
        if (!sub) continue;
        if (kind != K_LIB) printf("%-60s %10s %5.1f%%\n", heads[kind], num(sub), 100.0 * (double)sub / (double)flen);
        for (j = 0; j < ng; j++) {
            int shown = 0, more = 0;
            unsigned long long rest = 0;
            if (gs[j].kind != kind) continue;
            printf("%s%-*.*s %10s %5.1f%%\n", kind == K_LIB ? "" : "  ", kind == K_LIB ? 60 : 58, kind == K_LIB ? 60 : 58,
                   kind == K_LIB ? lib_title(gs[j].group) : gs[j].group, num(gs[j].bytes), 100.0 * (double)gs[j].bytes / (double)flen);
            for (i = 0; i < nitems; i++) {
                if (items[i].zf || items[i].kind != kind || strcmp(items[i].group, gs[j].group)) continue;
                if (!*items[i].name) continue;
                if (shown++ < limit) printf("%s  %-*.*s %10s\n", kind == K_LIB ? "" : "  ", kind == K_LIB ? 58 : 56, kind == K_LIB ? 58 : 56, items[i].name, num(items[i].bytes));
                else { more++; rest += items[i].bytes; }
            }
            if (more) printf("%s  %-*s %10s\n", kind == K_LIB ? "" : "  ", kind == K_LIB ? 58 : 56,
                             kind == K_LIB ? "... more procedures (--all lists them)" : "... more (--all lists them)", num(rest));
        }
    }
    printf("%-60s %10s %5.1f%%\n", "format overhead", num(ovh), 100.0 * (double)ovh / (double)flen);
    printf("  %-58s %10s\n", "symbol and string tables (strip removes them)", num(symtab_bytes));
    printf("  %-58s %10s\n", "headers, load commands, segment padding, signature", num(ovh > symtab_bytes ? ovh - symtab_bytes : 0));
    printf("%-60s %10s\n", "total = file size", num(infile + ovh));
    if (zero) printf("\nnot in the file: %s bytes of zero-filled data (globals, buffers, collector state)\n", num(zero));
    printf("Procedures that were inlined or turned into loops (contified) have no code of their own;\n"
           "their bytes are counted in the procedure they were inlined into.\n");
    return 0;
}
