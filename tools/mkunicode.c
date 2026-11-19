/* mkunicode.c -- generates src/rt_unicode.c, the Unicode tables of --unicode=full (spec §3.1,
 * §9.1, §11.1; DESIGN.md "M5 as built"). Plain C99, no dependencies.
 *
 * Input: a directory with these files of the Unicode Character Database, version 18.0.0:
 *     https://www.unicode.org/Public/18.0.0/ucd/UnicodeData.txt
 *     https://www.unicode.org/Public/18.0.0/ucd/SpecialCasing.txt
 *     https://www.unicode.org/Public/18.0.0/ucd/CaseFolding.txt
 *     https://www.unicode.org/Public/18.0.0/ucd/PropList.txt
 *     https://www.unicode.org/Public/18.0.0/ucd/DerivedCoreProperties.txt
 * Usage:
 *     cc -std=c99 -O2 tools/mkunicode.c -o tools/mkunicode
 *     tools/mkunicode UCD-DIR > src/rt_unicode.c
 * The output is committed (like src/embed.c), so building Shikra never needs the UCD.
 *
 * What is generated (code points >= 0x80 only; ASCII is handled in rt_str.c):
 *   properties   Alphabetic, Uppercase, Lowercase (DerivedCoreProperties), White_Space
 *                (PropList), Numeric_Type=Decimal (general category Nd; R7RS 6.6)
 *   simple maps  upper and lower case (UnicodeData), case folding (CaseFolding C + S)
 *   full maps    unconditional upper and lower case mappings of SpecialCasing (ß -> SS,
 *                İ -> i̇ ...); full case folding (status F) is derived at run time as
 *                "simple-fold every character of the full upper case of the lower case",
 *                which this program verifies for every code point -- the few code points
 *                where that rule fails are emitted as explicit exceptions.
 * Final sigma (string-downcase) is handled in rt_str.c; language-specific mappings (tr, lt)
 * and the Turkic foldings (status T) are not used.
 *
 * Table format ("run tables", decoded linearly): a sequence of runs, each encoded as
 * varints (7 bits per byte, high bit = more): gap, count * 2 + (stride - 1) and, for maps, the
 * zigzag-encoded delta to add. A run covers count code points start, start + stride, ...
 * (stride 1 or 2); start = end of the previous run + 1 + gap, beginning at 0x80. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NCP 0x110000
enum { P_ALPHA, P_UPPER, P_LOWER, P_SPACE, P_DIGIT, NPROPS };
static unsigned char *prop[NPROPS];
static int *upper, *lower, *fold, *digit;
static int ufull[NCP][4], lfull[NCP][4]; /* [0] = count; huge but static: zero-filled bss */

static void die(const char *m, const char *a) { fprintf(stderr, "mkunicode: %s %s\n", m, a ? a : ""); exit(1); }
static FILE *open_ucd(const char *dir, const char *name) {
    char path[4096];
    FILE *f;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    if (!(f = fopen(path, "r"))) die("cannot open", path);
    return f;
}
/* Splits a line at ';' into at most 16 trimmed fields (comment after '#' removed). */
static int fields(char *line, char **f) {
    int n = 0;
    char *p = strchr(line, '#'), *s;
    if (p) *p = 0;
    for (s = line; n < 16; s = p + 1) {
        char *e;
        p = strchr(s, ';');
        if (p) *p = 0;
        while (*s == ' ' || *s == '\t') s++;
        e = s + strlen(s);
        while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
        f[n++] = s;
        if (!p) break;
    }
    return n;
}
static int codes(const char *s, int *out) { /* "0053 0053" -> count */
    int n = 0;
    char *e;
    while (*s) {
        long v = strtol(s, &e, 16);
        if (e == s) break;
        if (n < 3) out[n] = (int)v;
        n++;
        s = e;
    }
    return n;
}
static void read_props(const char *dir, const char *name) {
    char line[1024], *f[16];
    FILE *in = open_ucd(dir, name);
    while (fgets(line, sizeof line, in)) {
        long lo, hi, c;
        int p;
        char *dots;
        if (fields(line, f) < 2 || !*f[0]) continue;
        if (!strcmp(f[1], "Alphabetic")) p = P_ALPHA;
        else if (!strcmp(f[1], "Uppercase")) p = P_UPPER;
        else if (!strcmp(f[1], "Lowercase")) p = P_LOWER;
        else if (!strcmp(f[1], "White_Space")) p = P_SPACE;
        else continue;
        lo = hi = strtol(f[0], 0, 16);
        if ((dots = strstr(f[0], ".."))) hi = strtol(dots + 2, 0, 16);
        for (c = lo; c <= hi; c++) prop[p][c] = 1;
    }
    fclose(in);
}
static void read_ucd(const char *dir) {
    char line[1024], *f[16];
    FILE *in = open_ucd(dir, "UnicodeData.txt");
    while (fgets(line, sizeof line, in)) {
        int c;
        if (fields(line, f) < 15) continue;
        c = (int)strtol(f[0], 0, 16);
        if (!strcmp(f[2], "Nd")) { prop[P_DIGIT][c] = 1; digit[c] = atoi(f[6]); }
        if (*f[12]) upper[c] = (int)strtol(f[12], 0, 16);
        if (*f[13]) lower[c] = (int)strtol(f[13], 0, 16);
    }
    fclose(in);
    in = open_ucd(dir, "SpecialCasing.txt");
    while (fgets(line, sizeof line, in)) { /* code; lower; title; upper; [condition;] */
        int c, n = fields(line, f);
        if (n < 4 || !*f[0] || (n > 4 && *f[4])) continue; /* conditional: skipped */
        c = (int)strtol(f[0], 0, 16);
        lfull[c][0] = codes(f[1], lfull[c] + 1);
        ufull[c][0] = codes(f[3], ufull[c] + 1);
        if (lfull[c][0] > 3 || ufull[c][0] > 3) die("full mapping longer than 3", f[0]);
    }
    fclose(in);
}
static int ffull[NCP][4]; /* CaseFolding status F */
static void read_folding(const char *dir) {
    char line[1024], *f[16];
    FILE *in = open_ucd(dir, "CaseFolding.txt");
    while (fgets(line, sizeof line, in)) {
        int c;
        if (fields(line, f) < 3 || !*f[0]) continue;
        c = (int)strtol(f[0], 0, 16);
        if (!strcmp(f[1], "C") || !strcmp(f[1], "S")) fold[c] = (int)strtol(f[2], 0, 16);
        else if (!strcmp(f[1], "F")) ffull[c][0] = codes(f[2], ffull[c] + 1);
    }
    fclose(in);
}

/* ---- output ---- */
static long total;
static void varint(unsigned long v, int *col) {
    do {
        unsigned b = v & 127;
        v >>= 7;
        printf("%u,", b | (v ? 128 : 0));
        total++;
        if (++*col % 24 == 0) printf("\n    ");
    } while (v);
}
/* Emits a run table for the members of set (map: members are code points with map[c] != c). */
static void run_table(const char *name, const unsigned char *set, const int *map) {
    long c = 0x80, pos = 0x80, n1, n2, k;
    int col = 0;
    long start = total;
#define MEMBER(x) ((x) < NCP && (map ? map[x] != (x) : set[x]))
#define DELTA(x) (map ? map[x] - (x) : 0)
    printf("static const unsigned char %s[] = {\n    ", name);
    for (;;) {
        while (c < NCP && !MEMBER(c)) c++;
        if (c >= NCP) break;
        for (n1 = 1; MEMBER(c + n1) && DELTA(c + n1) == DELTA(c); n1++) {}
        for (n2 = 1; MEMBER(c + 2 * n2) && DELTA(c + 2 * n2) == DELTA(c) && !MEMBER(c + 2 * n2 - 1); n2++) {}
        if (n2 < 3 || n2 <= n1) n2 = 0;
        varint((unsigned long)(c - pos), &col);
        varint(n2 ? (unsigned long)n2 * 2 + 1 : (unsigned long)n1 * 2, &col);
        if (map) { long d = DELTA(c); varint(d >= 0 ? (unsigned long)(2 * d) : (unsigned long)(-2 * d - 1), &col); }
        k = n2 ? c + 2 * (n2 - 1) : c + n1 - 1;
        pos = c = k + 1;
    }
    printf("};\n");
    fprintf(stderr, "mkunicode: %-12s %5ld bytes\n", name, total - start);
}
/* Full mappings: entries {code point, up to 3 BMP code points, 0-padded}. */
static void full_table(const char *name, int (*m)[4]) {
    int c, i, n = 0;
    printf("static const uni_full %s[] = {\n", name);
    for (c = 0x80; c < NCP; c++)
        if (m[c][0]) {
            printf("    {0x%04X, {", c);
            for (i = 1; i <= 3; i++) {
                if (i <= m[c][0] && m[c][i] > 0xFFFF) die("full mapping target beyond the BMP", name);
                printf("0x%04X%s", i <= m[c][0] ? m[c][i] : 0, i < 3 ? ", " : "}},\n");
            }
            n++;
        }
    if (!n) printf("    {0, {0, 0, 0}}\n");
    printf("};\n");
    total += 12L * (n ? n : 1);
    fprintf(stderr, "mkunicode: %-12s %5d bytes\n", name, 12 * n);
}

static const char *runtime_code =
"/* Returns the index of c within its run of table t, or -1; *delta receives a map's delta. */\n"
"static unsigned long uni_v(const unsigned char **p) {\n"
"    unsigned long v = 0;\n"
"    int s = 0;\n"
"    do v |= (unsigned long)(**p & 127) << s, s += 7;\n"
"    while (*(*p)++ & 128);\n"
"    return v;\n"
"}\n"
"static long uni_find(const unsigned char *t, size_t n, long c, long *delta) {\n"
"    const unsigned char *end = t + n;\n"
"    long pos = 0x80, start, cnt, st;\n"
"    unsigned long z;\n"
"    while (t < end) {\n"
"        start = pos + (long)uni_v(&t);\n"
"        cnt = (long)uni_v(&t);\n"
"        st = 1 + (cnt & 1);\n"
"        cnt >>= 1;\n"
"        if (delta) { z = uni_v(&t); *delta = z & 1 ? -(long)(z >> 1) - 1 : (long)(z >> 1); }\n"
"        if (c < start) return -1;\n"
"        if (c <= start + (cnt - 1) * st) return (c - start) % st ? -1 : (c - start) / st;\n"
"        pos = start + (cnt - 1) * st + 1;\n"
"    }\n"
"    return -1;\n"
"}\n"
"#define UNI_T(t) t, sizeof t\n"
"/* p: RT_UNI_ALPHA, _UPPER, _LOWER, _SPACE: 1 or 0; RT_UNI_DIGIT: the digit value or -1 */\n"
"RT_FN int rt_uni_prop(int c, int p) {\n"
"    long i = p == RT_UNI_ALPHA ? uni_find(UNI_T(uni_alpha), c, 0) : p == RT_UNI_UPPER ? uni_find(UNI_T(uni_upper), c, 0)\n"
"           : p == RT_UNI_LOWER ? uni_find(UNI_T(uni_lower), c, 0) : p == RT_UNI_SPACE ? uni_find(UNI_T(uni_space), c, 0)\n"
"           : uni_find(UNI_T(uni_digit), c, 0);\n"
"    return p == RT_UNI_DIGIT ? (i < 0 ? -1 : (int)(i % 10)) : i >= 0;\n"
"}\n"
"/* Simple case mapping, m: 0 upper, 1 lower, 2 fold. */\n"
"RT_FN int rt_uni_map(int c, int m) {\n"
"    long d = 0;\n"
"    long i = m == 0 ? uni_find(UNI_T(uni_tou), c, &d) : m == 1 ? uni_find(UNI_T(uni_tol), c, &d) : uni_find(UNI_T(uni_tof), c, &d);\n"
"    return i < 0 ? c : (int)(c + d);\n"
"}\n"
"/* Full mapping (m: 0 upper, 1 lower, 2 fold exception) into out[3]: the count, 0 if none. */\n"
"RT_FN int rt_uni_full(int c, int m, int *out) {\n"
"    const uni_full *t = m == 0 ? uni_fullu : m == 1 ? uni_fulll : uni_fullf;\n"
"    size_t i, n = (m == 0 ? sizeof uni_fullu : m == 1 ? sizeof uni_fulll : sizeof uni_fullf) / sizeof *t;\n"
"    int k;\n"
"    for (i = 0; i < n; i++)\n"
"        if (t[i].c == (unsigned)c) {\n"
"            for (k = 0; k < 3 && t[i].o[k]; k++) out[k] = t[i].o[k];\n"
"            return k;\n"
"        }\n"
"    return 0;\n"
"}\n";

/* The run-time rule for full case folding (see the header): returns the count in out[3]. */
static int derived_fold(int c, int *out) {
    int n, i, l;
    if (lfull[c][0]) {
        for (i = 0; i < lfull[c][0]; i++) out[i] = fold[lfull[c][i + 1]] ? fold[lfull[c][i + 1]] : lfull[c][i + 1];
        return lfull[c][0];
    }
    l = lower[c] ? lower[c] : c;
    if (!(n = ufull[l][0])) { out[0] = fold[c] ? fold[c] : c; return 1; }
    for (i = 0; i < n; i++) out[i] = fold[ufull[l][i + 1]] ? fold[ufull[l][i + 1]] : ufull[l][i + 1];
    return n;
}

int main(int argc, char **argv) {
    static int fexc[NCP][4];
    int c, i, nexc = 0;
    if (argc != 2) die("usage: mkunicode UCD-DIR > src/rt_unicode.c", 0);
    for (i = 0; i < NPROPS; i++) prop[i] = (unsigned char *)calloc(NCP, 1);
    upper = (int *)calloc(NCP, sizeof(int));
    lower = (int *)calloc(NCP, sizeof(int));
    fold = (int *)calloc(NCP, sizeof(int));
    digit = (int *)calloc(NCP, sizeof(int));
    read_props(argv[1], "DerivedCoreProperties.txt");
    read_props(argv[1], "PropList.txt");
    read_ucd(argv[1]);
    read_folding(argv[1]);
    for (c = 0; c < NCP; c++) { /* identity where there is no mapping */
        if (!upper[c]) upper[c] = c;
        if (!lower[c]) lower[c] = c;
        if (!fold[c]) fold[c] = c;
        if (lfull[c][0] == 1 && lfull[c][1] == lower[c]) lfull[c][0] = 0; /* same as the simple map */
        if (ufull[c][0] == 1 && ufull[c][1] == upper[c]) ufull[c][0] = 0;
    }
    for (c = 0; c < NCP; c++) /* digits: runs of 0..9 starting at a run start */
        if (prop[P_DIGIT][c] && digit[c] != (prop[P_DIGIT][c - 1] ? (digit[c - 1] + 1) % 10 : 0)) die("unexpected Nd run", 0);
    for (c = 0x80; c < NCP; c++) { /* verify the derived full folding; keep the exceptions */
        int out[3], n = derived_fold(c, out), *want = ffull[c][0] ? ffull[c] + 1 : &fold[c];
        int nw = ffull[c][0] ? ffull[c][0] : 1;
        if (n != nw || memcmp(out, want, sizeof(int) * (size_t)n) != 0) {
            fexc[c][0] = nw;
            for (i = 0; i < nw; i++) fexc[c][i + 1] = want[i];
            nexc++;
        }
    }
    printf("/* rt_unicode.c -- GENERATED by tools/mkunicode.c from the Unicode Character Database 18.0.0\n"
           " * (https://www.unicode.org/Public/18.0.0/ucd/). Do not edit: see tools/mkunicode.c for the\n"
           " * table format and how to regenerate. Linked into a program only with --unicode=full\n"
           " * (spec §9.1); the tool always contains it (REPL and interpreter: --unicode=full). */\n"
           "#ifndef SHIKRA_RT_H\n#include \"rt.h\"\n#endif\n"
           "#if !defined(SHIKRA_PROGRAM) || defined(RT_UNICODE_FULL)\n"
           "typedef struct { unsigned int c; unsigned short o[3]; } uni_full;\n");
    run_table("uni_alpha", prop[P_ALPHA], 0);
    run_table("uni_upper", prop[P_UPPER], 0);
    run_table("uni_lower", prop[P_LOWER], 0);
    run_table("uni_space", prop[P_SPACE], 0);
    run_table("uni_digit", prop[P_DIGIT], 0);
    run_table("uni_tou", 0, upper);
    run_table("uni_tol", 0, lower);
    run_table("uni_tof", 0, fold);
    full_table("uni_fullu", ufull);
    full_table("uni_fulll", lfull);
    full_table("uni_fullf", fexc);
    fputs(runtime_code, stdout);
    printf("#endif\n");
    fprintf(stderr, "mkunicode: %ld bytes of tables, %d fold exceptions\n", total, nexc);
    return 0;
}
