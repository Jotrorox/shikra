/* rt_read.c -- the datum reader (R7RS §7.1.2 lexical syntax), shared by the compiler,
 * the REPL and compiled programs that use (scheme read). Leaf code: no Scheme calls.
 * Tool mode additionally records file:line:column for every pair it creates. */
#ifndef SHIKRA_RT_H
#include "rt.h"
#endif

#define RD_CLOSE ((obj)0x707) /* internal tokens, never escape rt_read */
#define RD_DOT ((obj)0x807)

typedef struct {
    obj port;
    obj tok;            /* scratch output string port for tokens and strings */
    obj labels;         /* alist (n . placeholder box) for #n= / #n# */
    int patch;          /* a label was defined: patch placeholders afterwards */
    intptr_t line, col; /* start of the item last returned by rd_item */
} rd_t;

/* ---- source locations (tool mode) ---- */
#ifndef SHIKRA_PROGRAM
typedef struct { obj key, file; intptr_t line, col; } rd_loc;
static rd_loc *rd_locs;
static size_t rd_nlocs, rd_caplocs;
static int rd_locon;
static size_t rd_lochash(obj k) { return (size_t)((RT_W(k) >> 4) * 0x9E3779B97F4A7C15u); }
RT_FN void rt_srcloc_enable(int on) { rd_locon = on; }
RT_FN void rt_srcloc_set(obj pair, obj file, intptr_t line, intptr_t col) {
    size_t i;
    if (2 * (rd_nlocs + 1) > rd_caplocs) {
        rd_loc *old = rd_locs;
        size_t oldcap = rd_caplocs, j;
        rd_caplocs = oldcap ? 2 * oldcap : 4096;
        rd_locs = (rd_loc *)rt_sys_mem(rd_caplocs * sizeof(rd_loc));
        if (!rd_locs) rt_error("read", "out of memory", RT_NIL);
        for (j = 0; j < oldcap; j++)
            if (old[j].key) {
                for (i = rd_lochash(old[j].key) & (rd_caplocs - 1); rd_locs[i].key; i = (i + 1) & (rd_caplocs - 1)) {}
                rd_locs[i] = old[j];
            }
    }
    for (i = rd_lochash(pair) & (rd_caplocs - 1); rd_locs[i].key && rd_locs[i].key != pair; i = (i + 1) & (rd_caplocs - 1)) {}
    if (!rd_locs[i].key) rd_nlocs++;
    rd_locs[i].key = pair;
    rd_locs[i].file = file;
    rd_locs[i].line = line;
    rd_locs[i].col = col;
}
RT_FN int rt_srcloc_lookup(obj pair, obj *file, intptr_t *line, intptr_t *col) {
    size_t i;
    if (!rd_caplocs) return 0;
    for (i = rd_lochash(pair) & (rd_caplocs - 1); rd_locs[i].key; i = (i + 1) & (rd_caplocs - 1))
        if (rd_locs[i].key == pair) {
            *file = rd_locs[i].file; *line = rd_locs[i].line; *col = rd_locs[i].col;
            return 1;
        }
    return 0;
}
#define RD_LOC(r, p, l, c) \
    do { if (rd_locon && rt_is_string(RT_PORT((r)->port)->name)) rt_srcloc_set(p, RT_PORT((r)->port)->name, l, c); } while (0)
#else
#define RD_LOC(r, p, l, c) ((void)(l), (void)(c))
#endif

static RT_NORETURN void rd_error(rd_t *r, intptr_t line, intptr_t col, const char *msg) {
    obj loc = RT_FALSE, name = RT_PORT(r->port)->name;
    if (rt_is_string(name)) {
        loc = rt_open_output_string();
        rt_print(name, loc, 0);
        rt_port_putc(loc, ':');
        rt_print(RT_FIX(line), loc, 0);
        rt_port_putc(loc, ':');
        rt_print(RT_FIX(col), loc, 0);
        loc = rt_get_output_string(loc);
    }
    rt_raise(rt_make_error(RT_ERR_READ, rt_cstring(msg), RT_NIL, loc));
}

static int rd_space(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
static int rd_delim(int c) { return c < 0 || rd_space(c) || c == '(' || c == ')' || c == '"' || c == ';' || c == '|'; }
static void rd_tokreset(rd_t *r) { RT_PORT(r->tok)->len = 0; }
static char *rd_tokbuf(rd_t *r) { return RT_PORT(r->tok)->buf; }
static intptr_t rd_toklen(rd_t *r) { return RT_PORT(r->tok)->len; }
/* Token: c followed by every char up to a delimiter. */
static void rd_token(rd_t *r, int c) {
    rd_tokreset(r);
    rt_port_putc(r->tok, c);
    while (!rd_delim(rt_port_peekc(r->port))) rt_port_putc(r->tok, rt_port_getc(r->port));
}
static int rd_tokis(rd_t *r, const char *s) {
    size_t n = rt_strlen(s);
    return (size_t)rd_toklen(r) == n && rt_memcmp(rd_tokbuf(r), s, n) == 0;
}
static void rd_fold(rd_t *r) {
    intptr_t i;
    if (!(RT_HFLAGS(r->port) & RT_PF_FOLDCASE)) return;
    for (i = 0; i < rd_toklen(r); i++)
        if (rd_tokbuf(r)[i] >= 'A' && rd_tokbuf(r)[i] <= 'Z') rd_tokbuf(r)[i] += 'a' - 'A';
}

/* \x<hex>; after the "\x" has been consumed. */
static int rd_hexesc(rd_t *r, intptr_t el, intptr_t ec) {
    int c, v = 0, n = 0;
    while ((c = rt_port_getc(r->port)) != ';') {
        int d = c >= '0' && c <= '9' ? c - '0' : (c | 0x20) >= 'a' && (c | 0x20) <= 'f' ? (c | 0x20) - 'a' + 10 : -1;
        if (d < 0 || ++n > 6) rd_error(r, el, ec, "bad \\x escape");
        v = v * 16 + d;
    }
    if (!n || v > 0x10FFFF || (v >= 0xD800 && v < 0xE000)) rd_error(r, el, ec, "bad \\x escape");
    return v;
}
/* Body of "..." or |...| after the opening quote; result left in r->tok. */
static void rd_quoted(rd_t *r, int q, intptr_t line, intptr_t col) {
    int c;
    rd_tokreset(r);
    for (;;) {
        if ((c = rt_port_getc(r->port)) < 0) rd_error(r, line, col, q == '"' ? "unterminated string" : "unterminated |symbol|");
        if (c == q) return;
        if (c == '\\') {
            intptr_t el = RT_PORT(r->port)->line, ec = RT_PORT(r->port)->col; /* the backslash */
            switch (c = rt_port_getc(r->port)) {
            case 'a': c = 7; break;
            case 'b': c = 8; break;
            case 't': c = 9; break;
            case 'n': c = 10; break;
            case 'r': c = 13; break;
            case '"': case '\\': case '|': break;
            case 'x': case 'X': c = rd_hexesc(r, el, ec); break;
            default: /* \ <intraline whitespace>* <newline> <intraline whitespace>* */
                while (c == ' ' || c == '\t') c = rt_port_getc(r->port);
                if (c == '\r' && rt_port_peekc(r->port) == '\n') c = rt_port_getc(r->port);
                if (c != '\n' || q != '"') rd_error(r, el, ec, "bad escape");
                while ((c = rt_port_peekc(r->port)) == ' ' || c == '\t') rt_port_getc(r->port);
                continue;
            }
        }
        rt_port_putc(r->tok, c);
    }
}

static obj rd_item(rd_t *r);
static obj rd_datum(rd_t *r, const char *what) {
    obj x = rd_item(r);
    if (x == RD_CLOSE || x == RD_DOT || x == RT_EOF) rd_error(r, r->line, r->col, what);
    return x;
}
static obj rd_list(rd_t *r, intptr_t line, intptr_t col, int dots) {
    obj head = RT_NIL, tail = RT_NIL, x, p;
    for (;;) {
        x = rd_item(r);
        if (x == RD_CLOSE) return head;
        if (x == RT_EOF) rd_error(r, line, col, "unterminated list");
        if (x == RD_DOT) {
            if (!dots || head == RT_NIL) rd_error(r, r->line, r->col, "unexpected '.'");
            RT_CDR(tail) = rd_datum(r, "bad dotted list");
            if (rd_item(r) != RD_CLOSE) rd_error(r, r->line, r->col, "expected ')' after dotted tail");
            return head;
        }
        p = rt_cons(x, RT_NIL);
        if (head == RT_NIL) { RD_LOC(r, p, line, col); head = p; }
        else { RD_LOC(r, p, r->line, r->col); RT_CDR(tail) = p; }
        tail = p;
    }
}
static obj rd_abbrev(rd_t *r, const char *name, intptr_t line, intptr_t col) {
    obj x = rd_datum(r, "missing datum after quote"), p = rt_cons(x, RT_NIL);
    RD_LOC(r, p, r->line, r->col);
    p = rt_cons(rt_intern(name, rt_strlen(name)), p);
    RD_LOC(r, p, line, col);
    return p;
}
static obj rd_char(rd_t *r, intptr_t line, intptr_t col) {
    int c = rt_port_getc(r->port), i;
    size_t l;
    static const char *const names[] = {"alarm", "backspace", "delete", "escape", "newline", "null", "return", "space", "tab", 0};
    static const int codes[] = {7, 8, 127, 27, 10, 0, 13, 32, 9};
    if (c < 0) rd_error(r, line, col, "unexpected end of file in character");
    if (rd_delim(c) || rd_delim(rt_port_peekc(r->port))) return RT_CHAR(c);
    rd_token(r, c);
    if (rt_utf8_decode(rd_tokbuf(r), (size_t)rd_toklen(r), &l) == c && (intptr_t)l == rd_toklen(r)) return RT_CHAR(c);
    rd_fold(r);
    for (i = 0; names[i]; i++) if (rd_tokis(r, names[i])) return RT_CHAR(codes[i]);
    if (rd_tokbuf(r)[0] == 'x' && rd_toklen(r) <= 7) {
        int v = 0;
        for (i = 1; i < rd_toklen(r); i++) {
            int ch = rd_tokbuf(r)[i] | 0x20, d = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : -1;
            if (d < 0) break;
            v = v * 16 + d;
        }
        if (i == rd_toklen(r) && v <= 0x10FFFF && !(v >= 0xD800 && v < 0xE000)) return RT_CHAR(v);
    }
    rd_error(r, line, col, "unknown character name");
}
static obj rd_number_or_symbol(rd_t *r, intptr_t line, intptr_t col) {
    obj x;
    char *b = rd_tokbuf(r);
    int c = b[0];
    if ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.' || c == '#') {
        int rc = rt_parse_number(b, (size_t)rd_toklen(r), 10, &x);
        if (rc == 0) return x;
        if (rc == -2) rd_error(r, line, col, "number cannot be represented (no bignums or rationals)");
        if (c == '#') rd_error(r, line, col, "bad # syntax");
    }
    rd_fold(r);
    return rt_intern(rd_tokbuf(r), (size_t)rd_toklen(r));
}
static obj rd_label(rd_t *r, intptr_t n) {
    obj l;
    for (l = r->labels; l != RT_NIL; l = RT_CDR(l))
        if (RT_CAR(RT_CAR(l)) == RT_FIX(n)) return RT_CDR(RT_CAR(l));
    return RT_FALSE;
}

/* Reads one item; line and col receive its start. Comments are skipped here. */
static obj rd_item1(rd_t *r, intptr_t *line, intptr_t *col) {
    obj port = r->port, x;
    int c;
    for (;;) {
        while ((c = rt_port_peekc(port)) >= 0 && rd_space(c)) rt_port_getc(port);
        *line = RT_PORT(port)->line;
        *col = RT_PORT(port)->col + 1;
        switch (c = rt_port_getc(port)) {
        case -1: return RT_EOF;
        case ';': while ((c = rt_port_getc(port)) >= 0 && c != '\n') {} continue;
        case '(': return rd_list(r, *line, *col, 1);
        case ')': return RD_CLOSE;
        case '\'': return rd_abbrev(r, "quote", *line, *col);
        case '`': return rd_abbrev(r, "quasiquote", *line, *col);
        case ',':
            if (rt_port_peekc(port) == '@') { rt_port_getc(port); return rd_abbrev(r, "unquote-splicing", *line, *col); }
            return rd_abbrev(r, "unquote", *line, *col);
        case '"': rd_quoted(r, '"', *line, *col); return rt_get_output_string(r->tok);
        case '|': rd_quoted(r, '|', *line, *col); return rt_intern(rd_tokbuf(r), (size_t)rd_toklen(r));
        case '#': break;
        default:
            rd_token(r, c);
            if (rd_tokis(r, ".")) return RD_DOT;
            return rd_number_or_symbol(r, *line, *col);
        }
        /* '#' dispatch */
        c = rt_port_peekc(port);
        if (c == '|') { /* nested block comment */
            int depth = 1, prev = 0;
            rt_port_getc(port);
            while (depth > 0) {
                if ((c = rt_port_getc(port)) < 0) rd_error(r, *line, *col, "unterminated #| comment");
                if (prev == '|' && c == '#') { depth--; c = 0; }
                else if (prev == '#' && c == '|') { depth++; c = 0; }
                prev = c;
            }
            continue;
        }
        if (c == ';') { rt_port_getc(port); rd_datum(r, "missing datum after #;"); continue; }
        if (c == '(') {
            rt_port_getc(port);
            return rt_list_to_vector(rd_list(r, *line, *col, 0));
        }
        if (c == '\\') { rt_port_getc(port); return rd_char(r, *line, *col); }
        if (c == '!') {
            rt_port_getc(port);
            rd_token(r, '!');
            if (rd_tokis(r, "!fold-case")) RT_SETFLAGS(port, RT_HFLAGS(port) | RT_PF_FOLDCASE);
            else if (rd_tokis(r, "!no-fold-case")) RT_SETFLAGS(port, RT_HFLAGS(port) & ~RT_PF_FOLDCASE);
            else rd_error(r, *line, *col, "unknown #! directive");
            continue;
        }
        if (c >= '0' && c <= '9') { /* datum label */
            intptr_t n = 0;
            obj ph;
            while ((c = rt_port_getc(port)) >= '0' && c <= '9') if (n < 1000000) n = n * 10 + c - '0';
            if (c == '#') {
                if ((ph = rd_label(r, n)) == RT_FALSE) rd_error(r, *line, *col, "undefined datum label");
                return ph;
            }
            if (c != '=') rd_error(r, *line, *col, "bad datum label syntax");
            ph = rt_make_box(RT_UNDEF);
            RT_SETFLAGS(ph, RT_F_PLACEHOLDER);
            r->labels = rt_cons(rt_cons(RT_FIX(n), ph), r->labels);
            x = rd_datum(r, "missing datum after label");
            RT_VEC(ph)[0] = x;
            r->patch = 1;
            return x;
        }
        if (c < 0 || rd_delim(c)) rd_error(r, *line, *col, "bad # syntax");
        rd_token(r, '#');
        if (rd_tokis(r, "#t") || rd_tokis(r, "#true")) return RT_TRUE;
        if (rd_tokis(r, "#f") || rd_tokis(r, "#false")) return RT_FALSE;
        if (rd_tokis(r, "#u8") && rt_port_peekc(port) == '(') {
            obj l, v;
            intptr_t i = 0;
            rt_port_getc(port);
            l = rd_list(r, *line, *col, 0);
            v = rt_make_bytevector(RT_FIX(rt_list_length(l, "read")), RT_FIX(0));
            for (; l != RT_NIL; l = RT_CDR(l), i++) {
                x = RT_CAR(l);
                if (!rt_is_fix(x) || (uintptr_t)RT_FIXVAL(x) > 255) rd_error(r, *line, *col, "bytevector element is not a byte");
                RT_PTR(v, rt_bytevector)->b[i] = (unsigned char)RT_FIXVAL(x);
            }
            return v;
        }
#ifdef RT_HAVE_EXT
        if (rd_tokis(r, "#f64") && rt_port_peekc(port) == '(') { /* (shikra f64vector), SRFI 4 */
            obj l;
            rt_port_getc(port);
            l = rd_list(r, *line, *col, 0);
            for (x = l; x != RT_NIL; x = RT_CDR(x))
                if (!rt_is_num(RT_CAR(x))) rd_error(r, *line, *col, "f64vector element is not a real number");
            return rt_list_to_f64vector(l);
        }
#endif
        return rd_number_or_symbol(r, *line, *col);
    }
}
static obj rd_item(rd_t *r) {
    intptr_t line, col;
    obj x = rd_item1(r, &line, &col);
    r->line = line;
    r->col = col;
    return x;
}

/* Datum labels: every #n# reads as its placeholder box, so before patching the datum is a
 * tree; the walk replaces each placeholder by its value without descending into it. */
static int rd_isph(obj x) { return rt_is_type(x, RT_T_BOX) && (RT_HFLAGS(x) & RT_F_PLACEHOLDER); }
static obj rd_resolve(rd_t *r, obj x) {
    int n = 0;
    while (rd_isph(x)) {
        x = RT_VEC(x)[0];
        if (x == RT_UNDEF || ++n > 10000) rd_error(r, r->line, r->col, "datum label refers to itself");
    }
    return x;
}
static void rd_patch(rd_t *r, obj x) {
    intptr_t i;
    while (rt_is_pair(x)) {
        if (rd_isph(RT_CAR(x))) RT_CAR(x) = rd_resolve(r, RT_CAR(x));
        else rd_patch(r, RT_CAR(x));
        if (rd_isph(RT_CDR(x))) { RT_CDR(x) = rd_resolve(r, RT_CDR(x)); return; }
        x = RT_CDR(x);
    }
    if (rt_is_vector(x))
        for (i = 0; i < RT_HLEN(x); i++) {
            if (rd_isph(RT_VEC(x)[i])) RT_VEC(x)[i] = rd_resolve(r, RT_VEC(x)[i]);
            else rd_patch(r, RT_VEC(x)[i]);
        }
}

RT_FN obj rt_read(obj port) {
    rd_t r;
    obj x;
    if (!rt_is_port(port) || (RT_HFLAGS(port) & (RT_PF_IN | RT_PF_BINARY)) != RT_PF_IN)
        rt_type_error("read", "textual input port", port);
    if (!(RT_HFLAGS(port) & RT_PF_OPEN)) rt_error("read", "port is closed", rt_cons(port, RT_NIL));
    r.port = port;
    r.tok = rt_open_output_string();
    r.labels = RT_NIL;
    r.patch = 0;
    x = rd_item(&r);
    if (x == RD_CLOSE) rd_error(&r, r.line, r.col, "unexpected ')'");
    if (x == RD_DOT) rd_error(&r, r.line, r.col, "unexpected '.'");
    if (r.patch) { x = rd_resolve(&r, x); rd_patch(&r, x); }
    return x;
}
