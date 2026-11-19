/* rt_io.c -- ports and the printer (write/display).
 * A port is a fixed-field object (rt_port in rt.h) over either an OS descriptor (with a
 * byte buffer) or an in-memory buffer (RT_PF_STRING: string and bytevector ports).
 * Textual ports decode/encode UTF-8 here; binary ports (RT_PF_BINARY) move raw bytes over
 * the same buffer. A port is textual or binary, never both; the standard ports are
 * textual. The printer writes datum labels for cycles (write, display) or for all shared
 * structure (write-shared); see "Datum labels" below. */
#ifndef SHIKRA_RT_H
#include "rt.h"
#endif

#define IO_BUFSZ 4096
#define IO_ANY 0x100 /* io_check: textual or binary */
/* The standard ports are static (not heap objects), so starting a program allocates
 * nothing; the collector ignores them (their name is #f, their buffers are static). */
static char io_inbuf[IO_BUFSZ], io_outbuf[IO_BUFSZ], io_errbuf[IO_BUFSZ];
static rt_port io_std[3] = {
    {RT_HDR(RT_T_PORT, RT_PF_IN | RT_PF_OPEN, 0), 0, io_inbuf, 0, 0, IO_BUFSZ, 1, 0, RT_FALSE},
    {RT_HDR(RT_T_PORT, RT_PF_OUT | RT_PF_OPEN, 0), 1, io_outbuf, 0, 0, IO_BUFSZ, 1, 0, RT_FALSE},
    {RT_HDR(RT_T_PORT, RT_PF_OUT | RT_PF_UNBUF | RT_PF_OPEN, 0), 2, io_errbuf, 0, 0, IO_BUFSZ, 1, 0, RT_FALSE}};
RT_VAR obj rt_stdin = (obj)((char *)&io_std[0] + 5), rt_stdout = (obj)((char *)&io_std[1] + 5),
           rt_stderr = (obj)((char *)&io_std[2] + 5);
RT_VAR obj rt_ports = RT_NIL;

static obj io_make(int flags, intptr_t fd, intptr_t cap, obj name) {
    char *buf = (char *)rt_alloc_raw((size_t)cap);
    rt_port *p = (rt_port *)rt_alloc(RT_HDR(RT_T_PORT, flags | RT_PF_OPEN, 0), sizeof(rt_port));
    p->fd = fd;
    p->buf = buf;
    p->pos = p->len = 0;
    p->cap = cap;
    p->line = 1;
    p->col = 0;
    p->name = name;
    return RT_TAGPTR(p);
}
RT_FN obj rt_make_fd_port(int fd, int flags, obj name) {
    obj p = io_make(flags, fd, IO_BUFSZ, name);
    if ((flags & RT_PF_OUT) && fd > 2) rt_ports = rt_cons(p, rt_ports);
    return p;
}
static obj io_input_mem(const void *data, intptr_t n, int flags) {
    obj p = io_make(RT_PF_IN | RT_PF_STRING | flags, -1, n + 1, RT_FALSE);
    rt_memcpy(RT_PORT(p)->buf, data, (size_t)n);
    RT_PORT(p)->len = n;
    return p;
}
RT_FN obj rt_open_input_string(obj s) {
    RT_CHECK(rt_is_string(s), "open-input-string", "string", s);
    return io_input_mem(RT_STR(s)->buf, RT_STR(s)->bytes, 0);
}
RT_FN obj rt_open_input_bytevector(obj bv) {
    RT_CHECK(rt_is_bytevector(bv), "open-input-bytevector", "bytevector", bv);
    return io_input_mem(RT_PTR(bv, rt_bytevector)->b, RT_HLEN(bv), RT_PF_BINARY);
}
RT_FN obj rt_open_output_string(void) { return io_make(RT_PF_OUT | RT_PF_STRING, -1, 64, RT_FALSE); }
RT_FN obj rt_open_output_bytevector(void) { return io_make(RT_PF_OUT | RT_PF_STRING | RT_PF_BINARY, -1, 64, RT_FALSE); }
static int io_is(obj port, int flags) {
    return rt_is_port(port) && (RT_HFLAGS(port) & (RT_PF_OUT | RT_PF_STRING | RT_PF_BINARY)) == flags;
}
RT_FN obj rt_get_output_string(obj port) {
    RT_CHECK(io_is(port, RT_PF_OUT | RT_PF_STRING), "get-output-string", "output string port", port);
    return rt_make_string_n(RT_PORT(port)->buf, (size_t)RT_PORT(port)->len);
}
RT_FN obj rt_get_output_bytevector(obj port) {
    obj bv;
    RT_CHECK(io_is(port, RT_PF_OUT | RT_PF_STRING | RT_PF_BINARY), "get-output-bytevector", "output bytevector port", port);
    bv = rt_make_bytevector(RT_FIX(RT_PORT(port)->len), RT_FIX(0));
    rt_memcpy(RT_PTR(bv, rt_bytevector)->b, RT_PORT(port)->buf, (size_t)RT_PORT(port)->len);
    return bv;
}
/* (scheme file): every failure raises a file error (file-error?) "who: cannot ... file" path. */
static RT_NORETURN void io_file_error(const char *who, const char *msg, obj path) {
    obj m = rt_open_output_string();
    rt_port_puts(m, who);
    rt_port_puts(m, msg);
    rt_raise(rt_make_error(RT_ERR_FILE, rt_get_output_string(m), rt_cons(path, RT_NIL), RT_FALSE));
}
static obj io_open_file(obj path, int mode, int binary, const char *who) {
    int fd;
    RT_CHECK(rt_is_string(path), who, "string", path);
    fd = rt_sys_open(RT_STR(path)->buf, mode);
    if (fd < 0) io_file_error(who, ": cannot open file", path);
    return rt_make_fd_port(fd, (mode == RT_O_READ ? RT_PF_IN : RT_PF_OUT) | binary, path);
}
RT_FN obj rt_open_input_file(obj path) { return io_open_file(path, RT_O_READ, 0, "open-input-file"); }
RT_FN obj rt_open_output_file(obj path) { return io_open_file(path, RT_O_WRITE, 0, "open-output-file"); }
RT_FN obj rt_open_binary_input_file(obj path) { return io_open_file(path, RT_O_READ, RT_PF_BINARY, "open-binary-input-file"); }
RT_FN obj rt_open_binary_output_file(obj path) { return io_open_file(path, RT_O_WRITE, RT_PF_BINARY, "open-binary-output-file"); }
RT_FN obj rt_file_exists_p(obj path) {
    RT_CHECK(rt_is_string(path), "file-exists?", "string", path);
    return RT_BOOL(rt_sys_exists(RT_STR(path)->buf));
}
RT_FN obj rt_delete_file(obj path) {
    RT_CHECK(rt_is_string(path), "delete-file", "string", path);
    if (rt_sys_remove(RT_STR(path)->buf) != 0) io_file_error("delete-file", ": cannot delete file", path);
    return RT_UNSPEC;
}

static void io_flush(rt_port *p) {
    if (p->fd >= 0 && p->len > 0) rt_sys_write((int)p->fd, p->buf, (size_t)p->len);
    if (p->fd >= 0) p->len = 0;
}
RT_FN void rt_flush(obj port) {
    if (rt_is_port(port) && (RT_HFLAGS(port) & RT_PF_OUT)) io_flush(RT_PORT(port));
}
RT_FN void rt_flush_all(void) {
    obj l;
    if (rt_stdout) rt_flush(rt_stdout);
    for (l = rt_ports; rt_is_pair(l); l = RT_CDR(l)) rt_flush(RT_CAR(l));
    if (rt_stderr) rt_flush(rt_stderr);
}
RT_FN obj rt_close_port(obj port) {
    rt_port *p;
    obj *l;
    RT_CHECK(rt_is_port(port), "close-port", "port", port);
    p = RT_PORT(port);
    if (!(RT_HFLAGS(port) & RT_PF_OPEN)) return RT_UNSPEC;
    if (RT_HFLAGS(port) & RT_PF_OUT) io_flush(p);
    if (p->fd > 2) rt_sys_close((int)p->fd);
    RT_SETFLAGS(port, RT_HFLAGS(port) & ~RT_PF_OPEN);
    for (l = &rt_ports; rt_is_pair(*l); l = &RT_CDR(*l))
        if (RT_CAR(*l) == port) { *l = RT_CDR(*l); break; }
    return RT_UNSPEC;
}

/* want: RT_PF_IN or RT_PF_OUT, plus RT_PF_BINARY (binary) or IO_ANY (either); else textual. */
static rt_port *io_check(obj port, int want, const char *who) {
    int dir = want & (RT_PF_IN | RT_PF_OUT), mask = dir | (want & IO_ANY ? 0 : RT_PF_BINARY);
    if (!rt_is_port(port) || (RT_HFLAGS(port) & mask) != (want & mask))
        rt_type_error(who, want & IO_ANY ? (dir == RT_PF_IN ? "input port" : "output port")
                         : want & RT_PF_BINARY ? (dir == RT_PF_IN ? "binary input port" : "binary output port")
                         : dir == RT_PF_IN ? "textual input port" : "textual output port", port);
    if (!(RT_HFLAGS(port) & RT_PF_OPEN)) rt_error(who, "port is closed", rt_cons(port, RT_NIL));
    return RT_PORT(port);
}
RT_FN obj rt_input_port_p(obj x) { return RT_BOOL(rt_is_port(x) && (RT_HFLAGS(x) & RT_PF_IN)); }
RT_FN obj rt_output_port_p(obj x) { return RT_BOOL(rt_is_port(x) && (RT_HFLAGS(x) & RT_PF_OUT)); }
RT_FN obj rt_textual_port_p(obj x) { return RT_BOOL(rt_is_port(x) && !(RT_HFLAGS(x) & RT_PF_BINARY)); }
RT_FN obj rt_binary_port_p(obj x) { return RT_BOOL(rt_is_port(x) && (RT_HFLAGS(x) & RT_PF_BINARY)); }
RT_FN obj rt_input_port_open_p(obj x) {
    RT_CHECK(rt_is_port(x) && (RT_HFLAGS(x) & RT_PF_IN), "input-port-open?", "input port", x);
    return RT_BOOL(RT_HFLAGS(x) & RT_PF_OPEN);
}
RT_FN obj rt_output_port_open_p(obj x) {
    RT_CHECK(rt_is_port(x) && (RT_HFLAGS(x) & RT_PF_OUT), "output-port-open?", "output port", x);
    return RT_BOOL(RT_HFLAGS(x) & RT_PF_OPEN);
}

/* ---- input ---- */
/* Make at least `need` unread bytes available if possible; returns how many are. */
static intptr_t io_ensure(rt_port *p, intptr_t need) {
    while (p->len - p->pos < need && p->fd >= 0) {
        intptr_t r;
        if (p->fd == 0 && rt_stdout) rt_flush(rt_stdout); /* prompts appear before reads */
        if (p->pos > 0) {
            rt_memmove(p->buf, p->buf + p->pos, (size_t)(p->len - p->pos));
            p->len -= p->pos;
            p->pos = 0;
        }
        r = rt_sys_read((int)p->fd, p->buf + p->len, (size_t)(p->cap - p->len));
        if (r <= 0) break;
        p->len += r;
    }
    return p->len - p->pos;
}
static int io_peek(rt_port *p, size_t *len) {
    intptr_t avail = io_ensure(p, 1);
    unsigned char c;
    *len = 1;
    if (avail <= 0) return -1;
    c = (unsigned char)p->buf[p->pos];
    if (c < 0x80) return c;
    avail = io_ensure(p, c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2);
    return rt_utf8_decode(p->buf + p->pos, (size_t)avail, len);
}
RT_FN int rt_port_peekc(obj port) { size_t l; return io_peek(RT_PORT(port), &l); }
RT_FN int rt_port_getc(obj port) {
    rt_port *p = RT_PORT(port);
    size_t l;
    int c = io_peek(p, &l);
    if (c < 0) return c;
    p->pos += (intptr_t)l;
    if (c == '\n') { p->line++; p->col = 0; }
    else p->col++;
    return c;
}
RT_FN obj rt_read_char(obj port) {
    int c;
    io_check(port, RT_PF_IN, "read-char");
    c = rt_port_getc(port);
    return c < 0 ? RT_EOF : RT_CHAR(c);
}
RT_FN obj rt_peek_char(obj port) {
    int c;
    io_check(port, RT_PF_IN, "peek-char");
    c = rt_port_peekc(port);
    return c < 0 ? RT_EOF : RT_CHAR(c);
}
RT_FN obj rt_read_line(obj port) {
    obj buf;
    int c;
    io_check(port, RT_PF_IN, "read-line");
    if ((c = rt_port_getc(port)) < 0) return RT_EOF;
    buf = rt_open_output_string();
    for (; c >= 0 && c != '\n'; c = rt_port_getc(port)) rt_port_putc(buf, c);
    if (RT_PORT(buf)->len > 0 && RT_PORT(buf)->buf[RT_PORT(buf)->len - 1] == '\r') RT_PORT(buf)->len--;
    return rt_get_output_string(buf);
}
RT_FN obj rt_read_string(obj k, obj port) {
    intptr_t n = rt_is_fix(k) ? RT_FIXVAL(k) : -1;
    obj buf;
    int c;
    if (n < 0) rt_type_error("read-string", "nonnegative exact integer", k);
    io_check(port, RT_PF_IN, "read-string");
    if (n == 0) return rt_make_string_n("", 0);
    if ((c = rt_port_getc(port)) < 0) return RT_EOF;
    buf = rt_open_output_string();
    do rt_port_putc(buf, c);
    while (--n > 0 && (c = rt_port_getc(port)) >= 0);
    return rt_get_output_string(buf);
}
/* A read will not block: buffered data, an in-memory port, or the OS says so (data or EOF). */
static obj io_ready(rt_port *p) { return RT_BOOL(p->pos < p->len || p->fd < 0 || rt_sys_ready((int)p->fd)); }
RT_FN obj rt_char_ready_p(obj port) { return io_ready(io_check(port, RT_PF_IN, "char-ready?")); }
RT_FN obj rt_u8_ready_p(obj port) { return io_ready(io_check(port, RT_PF_IN | RT_PF_BINARY, "u8-ready?")); }
RT_FN obj rt_read_u8(obj port) {
    rt_port *p = io_check(port, RT_PF_IN | RT_PF_BINARY, "read-u8");
    return io_ensure(p, 1) <= 0 ? RT_EOF : RT_FIX((unsigned char)p->buf[p->pos++]);
}
RT_FN obj rt_peek_u8(obj port) {
    rt_port *p = io_check(port, RT_PF_IN | RT_PF_BINARY, "peek-u8");
    return io_ensure(p, 1) <= 0 ? RT_EOF : RT_FIX((unsigned char)p->buf[p->pos]);
}
/* Reads end - start bytes, or as many as come before end of file; eof if none came. */
RT_FN obj rt_read_bytevector_x(obj bv, obj port, obj start, obj end) {
    intptr_t s, e, n = 0, avail;
    rt_port *p;
    RT_CHECK(rt_is_bytevector(bv) && !(RT_HFLAGS(bv) & RT_F_RO), "read-bytevector!", "mutable bytevector", bv);
    s = rt_check_range(start, end, RT_HLEN(bv), &e, "read-bytevector!");
    p = io_check(port, RT_PF_IN | RT_PF_BINARY, "read-bytevector!");
    while (n < e - s && (avail = io_ensure(p, 1)) > 0) {
        if (avail > e - s - n) avail = e - s - n;
        rt_memcpy(RT_PTR(bv, rt_bytevector)->b + s + n, p->buf + p->pos, (size_t)avail);
        p->pos += avail;
        n += avail;
    }
    return n == 0 && e > s ? RT_EOF : RT_FIX(n);
}

/* ---- output ---- */
RT_FN void rt_port_write(obj port, const char *s, size_t n) {
    rt_port *p = RT_PORT(port);
    if (RT_HFLAGS(port) & RT_PF_STRING) {
        if ((size_t)(p->cap - p->len) < n) {
            intptr_t cap = 2 * p->cap > p->len + (intptr_t)n ? 2 * p->cap : p->len + (intptr_t)n;
            char *nb = (char *)rt_alloc_raw((size_t)cap);
            p = RT_PORT(port);
            rt_memcpy(nb, p->buf, (size_t)p->len);
            p->buf = nb;
            p->cap = cap;
        }
        rt_memcpy(p->buf + p->len, s, n);
        p->len += (intptr_t)n;
        return;
    }
    if (RT_HFLAGS(port) & RT_PF_UNBUF && port != rt_stdout && rt_stdout) rt_flush(rt_stdout);
    if ((size_t)(p->cap - p->len) < n) io_flush(p);
    if ((intptr_t)n >= p->cap) rt_sys_write((int)p->fd, s, n);
    else { rt_memcpy(p->buf + p->len, s, n); p->len += (intptr_t)n; }
    if (RT_HFLAGS(port) & RT_PF_UNBUF) io_flush(p);
    if (n) p->col = s[n - 1] != '\n'; /* output: 0 at the start of a line (the REPL's fresh line) */
}
RT_FN void rt_port_putc(obj port, int c) {
    char b[4];
    rt_port_write(port, b, (size_t)rt_utf8_encode(b, c));
}
RT_FN void rt_port_puts(obj port, const char *s) { rt_port_write(port, s, rt_strlen(s)); }

/* ---- printer ---- */
static void io_fix(obj port, intptr_t v) {
    char buf[24], *p = buf + sizeof buf;
    uintptr_t u = v < 0 ? (uintptr_t)0 - (uintptr_t)v : (uintptr_t)v;
    do { *--p = (char)('0' + u % 10); u /= 10; } while (u);
    if (v < 0) *--p = '-';
    rt_port_write(port, p, (size_t)(buf + sizeof buf - p));
}
static void io_hex(obj port, int c) {
    char buf[12], *p = buf + sizeof buf;
    do { *--p = "0123456789abcdef"[c & 15]; c >>= 4; } while (c);
    rt_port_write(port, p, (size_t)(buf + sizeof buf - p));
}
static const char *const io_charnames[] = {"alarm", "backspace", "delete", "escape", "newline",
                                           "null", "return", "space", "tab", 0};
static const int io_charcodes[] = {7, 8, 127, 27, 10, 0, 13, 32, 9};

/* Escaped text for "strings" (q = '"') and |symbols| (q = '|'). */
static void io_escaped(obj port, const char *s, intptr_t n, char q) {
    intptr_t i, run = 0;
    rt_port_putc(port, q);
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        const char *esc = c == (unsigned char)q ? (q == '"' ? "\\\"" : "\\|") : c == '\\' ? "\\\\"
                        : c == '\n' ? "\\n" : c == '\t' ? "\\t" : c == '\r' ? "\\r" : c == 7 ? "\\a" : c == 8 ? "\\b" : 0;
        if (!esc && c >= 0x20 && c != 0x7F) continue;
        rt_port_write(port, s + run, (size_t)(i - run));
        run = i + 1;
        if (esc) rt_port_puts(port, esc);
        else { rt_port_puts(port, "\\x"); io_hex(port, c); rt_port_putc(port, ';'); }
    }
    rt_port_write(port, s + run, (size_t)(n - run));
    rt_port_putc(port, q);
}
/* A symbol is written without bars unless it would read back as something else: a number,
 * or what a reader with complex numbers takes for one (+i, -i, and anything beginning with
 * +inf.0, -inf.0, +nan.0 or -nan.0, in any case). */
static int io_ci(const char *s, intptr_t n, const char *lit) {
    intptr_t i;
    for (i = 0; lit[i]; i++) if (i >= n || (s[i] | 0x20) != lit[i]) return 0;
    return 1;
}
static int io_sym_plain(const char *s, intptr_t n) {
    obj dummy;
    intptr_t i;
    if (n == 0 || s[0] == '#' || (n == 1 && s[0] == '.')) return 0;
    if (rt_parse_number(s, (size_t)n, 10, &dummy) != -1) return 0;
    if ((s[0] == '+' || s[0] == '-') && ((n == 2 && (s[1] | 0x20) == 'i') || io_ci(s + 1, n - 1, "inf.0") || io_ci(s + 1, n - 1, "nan.0")))
        return 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        const char *d;
        if (c <= 0x20 || c == 0x7F) return 0;
        for (d = "()\"';`|,\\[]{}"; *d; d++) if (c == (unsigned char)*d) return 0;
    }
    return 1;
}
static void io_char(obj port, int c, int write) {
    int i;
    if (!write) { rt_port_putc(port, c); return; }
    rt_port_puts(port, "#\\");
    for (i = 0; io_charnames[i]; i++)
        if (io_charcodes[i] == c) { rt_port_puts(port, io_charnames[i]); return; }
    if (c < 0x20 || c == 0x7F) { rt_port_putc(port, 'x'); io_hex(port, c); }
    else rt_port_putc(port, c);
}

/* Datum labels (R7RS 6.13.3). A depth-first pre-pass over the pairs and vectors reachable
 * from the datum marks, in an address table, every object that needs a label: one reached
 * again while it is still open on the path (a cycle: write, display) or reached again at all
 * (write-shared). The printer then writes #n= before the first occurrence of a marked object
 * and #n# for every later one. write-simple and the scalar fast paths skip the pre-pass. */
#define IO_SEEN 1
#define IO_OPEN 2  /* on the current depth-first path */
#define IO_LABEL 4 /* needs a label; label number + 1 from bit 3 on once written */
typedef struct { rt_addrtab t; intptr_t next; } io_labels;
static int io_compound(obj x) { return rt_is_pair(x) || (rt_is_vector(x) && RT_HLEN(x) > 0); }
static void io_scan(io_labels *L, obj x, int shared) {
    uintptr_t *st = 0, *ns; /* explicit stack of (object, next child index) */
    intptr_t sp = 0, cap = 0, i, n, *v;
    for (;;) {
        if (io_compound(x)) {
            v = rt_addrtab_get(&L->t, x, 0, 1);
            if (*v) { if (shared || (*v & IO_OPEN)) *v |= IO_LABEL; }
            else {
                *v = IO_SEEN | IO_OPEN;
                if (sp == cap) {
                    cap = cap ? 2 * cap : 256;
                    ns = (uintptr_t *)rt_alloc_raw(sizeof(uintptr_t) * (size_t)cap);
                    if (sp) rt_memcpy(ns, st, sizeof(uintptr_t) * (size_t)sp);
                    st = ns;
                }
                st[sp++] = RT_W(x);
                st[sp++] = 0;
            }
        }
        for (;;) { /* the next child of the innermost open object */
            if (!sp) return;
            x = RT_O(st[sp - 2]);
            i = (intptr_t)st[sp - 1]++;
            n = rt_is_pair(x) ? 2 : RT_HLEN(x);
            if (i < n) { x = rt_is_pair(x) ? (i ? RT_CDR(x) : RT_CAR(x)) : RT_VEC(x)[i]; break; }
            *rt_addrtab_get(&L->t, x, 0, 0) &= ~IO_OPEN;
            sp -= 2;
        }
    }
}
static intptr_t *io_marked(io_labels *L, obj x) {
    intptr_t *v = L ? rt_addrtab_get(&L->t, x, 0, 0) : 0;
    return v && (*v & IO_LABEL) ? v : 0;
}
/* Writes #n# and returns 1 if x already has a label; writes #n= first if it gets one now. */
static int io_label(io_labels *L, obj x, obj port) {
    intptr_t *v = io_marked(L, x);
    if (!v) return 0;
    rt_port_putc(port, '#');
    if (*v >> 3) { io_fix(port, (*v >> 3) - 1); rt_port_putc(port, '#'); return 1; }
    *v |= (L->next + 1) << 3;
    io_fix(port, L->next++);
    rt_port_putc(port, '=');
    return 0;
}

static void io_print(obj x, obj port, int write, io_labels *L) {
    char buf[40];
    intptr_t i;
    if (rt_is_fix(x)) { io_fix(port, RT_FIXVAL(x)); return; }
    if (rt_is_char(x)) { io_char(port, RT_CHARVAL(x), write); return; }
    if (rt_is_pair(x)) {
        if (io_label(L, x, port)) return;
        rt_port_putc(port, '(');
        for (;;) {
            io_print(RT_CAR(x), port, write, L);
            x = RT_CDR(x);
            if (!rt_is_pair(x) || io_marked(L, x)) break;
            rt_port_putc(port, ' ');
        }
        if (x != RT_NIL) { rt_port_puts(port, " . "); io_print(x, port, write, L); }
        rt_port_putc(port, ')');
        return;
    }
    if (rt_is_proc(x)) { rt_port_puts(port, "#<procedure>"); return; }
    if (!rt_is_heap(x)) {
        rt_port_puts(port, x == RT_TRUE ? "#t" : x == RT_FALSE ? "#f" : x == RT_NIL ? "()" : x == RT_EOF ? "#<eof>"
                         : x == RT_UNSPEC ? "#<unspecified>" : x == RT_UNDEF ? "#<undefined>"
                         : x == RT_DEFAULT ? "#<default>" : "#<immediate>");
        return;
    }
    switch (RT_HTYPE(x)) {
    case RT_T_FLONUM: rt_port_write(port, buf, rt_format_double(RT_FLO(x), buf)); return;
    case RT_T_STRING:
        if (write) io_escaped(port, RT_STR(x)->buf, RT_STR(x)->bytes, '"');
        else rt_port_write(port, RT_STR(x)->buf, (size_t)RT_STR(x)->bytes);
        return;
    case RT_T_SYMBOL: {
        rt_string *s = RT_STR(RT_PTR(x, rt_symbol)->name);
        if (write && !io_sym_plain(s->buf, s->bytes)) io_escaped(port, s->buf, s->bytes, '|');
        else rt_port_write(port, s->buf, (size_t)s->bytes);
        return;
    }
    case RT_T_VECTOR:
        if (io_label(L, x, port)) return;
        rt_port_puts(port, "#(");
        for (i = 0; i < RT_HLEN(x); i++) {
            if (i) rt_port_putc(port, ' ');
            io_print(RT_VEC(x)[i], port, write, L);
        }
        rt_port_putc(port, ')');
        return;
    case RT_T_BYTEVECTOR:
        rt_port_puts(port, "#u8(");
        for (i = 0; i < RT_HLEN(x); i++) {
            if (i) rt_port_putc(port, ' ');
            io_fix(port, RT_PTR(x, rt_bytevector)->b[i]);
        }
        rt_port_putc(port, ')');
        return;
    case RT_T_RECORD: rt_port_puts(port, "#<"); io_print(RT_VEC(RT_VEC(x)[0])[0], port, 0, 0); rt_port_putc(port, '>'); return;
    case RT_T_RTD: rt_port_puts(port, "#<record-type "); io_print(RT_VEC(x)[0], port, 0, 0); rt_port_putc(port, '>'); return;
    case RT_T_ERROR: rt_port_puts(port, "#<error "); rt_print(RT_VEC(x)[0], port, 1); rt_port_putc(port, '>'); return;
    case RT_T_PORT: rt_port_puts(port, "#<port>"); return;
    case RT_T_PROMISE: rt_port_puts(port, "#<promise>"); return;
    case RT_T_BOX: rt_port_puts(port, "#<box>"); return;
    case RT_T_VALUES: rt_port_puts(port, "#<values>"); return;
    default: rt_port_puts(port, "#<object>"); return;
    }
}
/* mode: RT_PRINT_WRITE (else display), RT_PRINT_SHARED (label all shared structure),
 * RT_PRINT_SIMPLE (no labels: may not terminate on cycles). Without SHARED or SIMPLE only
 * cycles get labels, so display and write always terminate. */
RT_FN void rt_print(obj x, obj port, int mode) {
    io_labels L;
    if (!io_compound(x) || (mode & RT_PRINT_SIMPLE)) { io_print(x, port, mode & RT_PRINT_WRITE, 0); return; }
    L.t.e = 0;
    L.t.cap = L.t.n = L.next = 0;
    io_scan(&L, x, mode & RT_PRINT_SHARED);
    io_print(x, port, mode & RT_PRINT_WRITE, &L);
}

RT_FN obj rt_write_char(obj c, obj port) {
    RT_CHECK(rt_is_char(c), "write-char", "char", c);
    io_check(port, RT_PF_OUT, "write-char");
    rt_port_putc(port, RT_CHARVAL(c));
    return RT_UNSPEC;
}
RT_FN obj rt_write_string(obj s, obj port) {
    RT_CHECK(rt_is_string(s), "write-string", "string", s);
    io_check(port, RT_PF_OUT, "write-string");
    rt_port_write(port, RT_STR(s)->buf, (size_t)RT_STR(s)->bytes);
    return RT_UNSPEC;
}
RT_FN obj rt_write_u8(obj b, obj port) {
    char c;
    if (!rt_is_fix(b) || (uintptr_t)RT_FIXVAL(b) > 255) rt_type_error("write-u8", "byte", b);
    io_check(port, RT_PF_OUT | RT_PF_BINARY, "write-u8");
    c = (char)RT_FIXVAL(b);
    rt_port_write(port, &c, 1);
    return RT_UNSPEC;
}
RT_FN obj rt_write_bytevector(obj bv, obj port, obj start, obj end) {
    intptr_t s, e;
    RT_CHECK(rt_is_bytevector(bv), "write-bytevector", "bytevector", bv);
    s = rt_check_range(start, end, RT_HLEN(bv), &e, "write-bytevector");
    io_check(port, RT_PF_OUT | RT_PF_BINARY, "write-bytevector");
    rt_port_write(port, (const char *)RT_PTR(bv, rt_bytevector)->b + s, (size_t)(e - s));
    return RT_UNSPEC;
}
RT_FN obj rt_write(obj x, obj port) { io_check(port, RT_PF_OUT, "write"); rt_print(x, port, RT_PRINT_WRITE); return RT_UNSPEC; }
RT_FN obj rt_write_shared(obj x, obj port) {
    io_check(port, RT_PF_OUT, "write-shared");
    rt_print(x, port, RT_PRINT_WRITE | RT_PRINT_SHARED);
    return RT_UNSPEC;
}
RT_FN obj rt_write_simple(obj x, obj port) {
    io_check(port, RT_PF_OUT, "write-simple");
    rt_print(x, port, RT_PRINT_WRITE | RT_PRINT_SIMPLE);
    return RT_UNSPEC;
}
/* Strings and chars also have their own primitives (rt_write_string, rt_write_char), so a call
 * whose argument type is known (M4) links neither the general printer nor the label pre-pass. */
RT_FN obj rt_display(obj x, obj port) {
    io_check(port, RT_PF_OUT, "display");
    if (rt_is_string(x)) rt_port_write(port, RT_STR(x)->buf, (size_t)RT_STR(x)->bytes);
    else if (rt_is_char(x)) rt_port_putc(port, RT_CHARVAL(x));
    else rt_print(x, port, 0);
    return RT_UNSPEC;
}
RT_FN obj rt_newline(obj port) { io_check(port, RT_PF_OUT, "newline"); rt_port_putc(port, '\n'); return RT_UNSPEC; }
RT_FN obj rt_flush_output_port(obj port) { io_check(port, RT_PF_OUT | IO_ANY, "flush-output-port"); rt_flush(port); return RT_UNSPEC; }
