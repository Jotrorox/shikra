/* repl.c -- `shikra repl [main.scm]` (spec §11.2) and the hidden batch runner
 * `shikra interp main.scm [args]` (DESIGN.md §11 M3), both on the interpreter (vm.c).
 *
 * The session is an open top level (front_repl_new; with a file, the program's own top level
 * reopened): unbound identifiers become globals, definitions may be redefined, and callers see
 * the new value (every global reference goes through its cell). Front-end work (reading,
 * expansion, bytecode) allocates immortal objects; user code runs with the collector on. */
#include "interp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static library *top;           /* the session's environment */
static obj recorded = RT_NIL;  /* imports and definitions, newest first (for ,c) */
static rt_jmpbuf *prompt_jb;
static interp_opts *opts;
static int echo;               /* stdin is not a terminal: echo input, so output reads as a session */

/* ---- errors return to the prompt ---- */
static RT_NORETURN void to_prompt(void) {
    rt_gc_set_immortal(0);
    front_repl_recover();
    rt_winders = rt_handlers = RT_NIL;
    rt_port_cells_reset(); /* parameterize of a port swaps the cell (control.scm) */
    vm_reset();
    rt_srcloc_enable(1);
    rt_sys_throw(prompt_jb);
}
static void fresh_line(void);
static void repl_raise(obj e) { fresh_line(); rt_print_error(e); to_prompt(); } /* reader errors */
static void repl_cerror(const char *loc, const char *msg) {
    fresh_line();
    rt_flush(rt_stdout);
    if (loc) { rt_port_puts(rt_stderr, loc); rt_port_puts(rt_stderr, ": "); }
    rt_port_puts(rt_stderr, "error: ");
    rt_port_puts(rt_stderr, msg);
    rt_port_putc(rt_stderr, '\n');
    to_prompt();
}
static void set_hooks(void) { rt_raise_hook = repl_raise; cerror_hook = repl_cerror; }
static void abort_entry(void) { set_hooks(); to_prompt(); }
static int protect(void (*fn)(void *), void *arg) {
    rt_jmpbuf jb, *saved = prompt_jb;
    int r;
    prompt_jb = &jb;
    r = rt_sys_catch(&jb, fn, arg);
    prompt_jb = saved;
    set_hooks();
    return r;
}

/* ---- output ---- */
static void fresh_line(void) { if (RT_PORT(rt_stdout)->col) rt_port_putc(rt_stdout, '\n'); }
static void print_value(obj v) {
    intptr_t i;
    if (v == RT_UNSPEC) return;
    if (rt_is_type(v, RT_T_VALUES)) {
        for (i = 0; i < RT_HLEN(v); i++) { fresh_line(); rt_print(RT_VEC(v)[i], rt_stdout, 1); rt_port_putc(rt_stdout, '\n'); }
        return;
    }
    fresh_line();
    rt_print(v, rt_stdout, 1);
    rt_port_putc(rt_stdout, '\n');
}
static void say(const char *a, const char *b, const char *c) {
    fresh_line();
    rt_port_puts(rt_stdout, a);
    if (b) rt_port_puts(rt_stdout, b);
    if (c) rt_port_puts(rt_stdout, c);
    rt_port_putc(rt_stdout, '\n');
}

/* ---- the session ---- */
static int is_head(obj x, const char *w) { return rt_is_pair(x) && RT_CAR(x) == sym(w); }
static void record(obj x) {
    if (is_head(x, "begin")) { for (x = RT_CDR(x); rt_is_pair(x); x = RT_CDR(x)) record(RT_CAR(x)); return; }
    if (is_head(x, "import") || is_head(x, "define") || is_head(x, "define-values") ||
        is_head(x, "define-record-type") || is_head(x, "define-syntax"))
        recorded = rt_cons(x, recorded);
}
static void run_unit(obj forms, int show) { /* caller is in immortal mode */
    tmpl *t = bc_unit(forms);
    obj v;
    rt_gc_set_immortal(0);
    v = vm_run(t);
    if (show) print_value(v);
}
static void eval_cb(void *p) {
    obj x = *(obj *)p, forms = RT_NIL;
    rt_gc_set_immortal(IMM_PURE);
    loc_ctx = RT_FALSE;
    if (is_head(x, "import")) front_repl_import(top, x);
    else forms = front_repl_expand(top, x);
    record(x);
    run_unit(forms, 1);
}
static void load_cb(void *p) {
    const char *file = (const char *)p;
    obj l;
    rt_gc_set_immortal(IMM_PURE);
    top = front_load_program(file);
    top->env->open = 1;
    vm_interaction = top;
    for (l = read_source(file, 0, RT_FALSE); rt_is_pair(l); l = RT_CDR(l)) record(RT_CAR(l));
    run_unit(RT_NIL, 0);
}
static void default_cb(void *p) {
    obj form;
    (void)p;
    rt_gc_set_immortal(IMM_PURE); /* first: the recorded (unscanned) list points to the form */
    form = vm_default_import_form();
    top = front_repl_new();
    vm_interaction = top;
    front_repl_import(top, form);
    record(form);
    run_unit(RT_NIL, 0);
}
static void read_cb(void *p) {
    obj *a = (obj *)p;
    rt_gc_set_immortal(IMM_PURE);
    a[1] = rt_read(a[0]);
    rt_gc_set_immortal(0);
}

/* ---- ,expand: in a scratch frame inside the session, so definitions do not take effect */
static void expand_cb(void *p) {
    obj x = *(obj *)p, forms;
    library *tmp;
    rt_gc_set_immortal(IMM_PURE);
    if (is_head(x, "import")) forms = rt_cons(x, RT_NIL);
    else {
        tmp = NEW(library);
        *tmp = *top;
        tmp->env = new_frame(top->env);
        tmp->env->open = 1;
        forms = front_repl_expand(tmp, x);
    }
    rt_gc_set_immortal(0);
    for (; rt_is_pair(forms); forms = RT_CDR(forms)) {
        fresh_line();
        unparse(RT_CAR(forms), rt_stdout, 0);
        rt_port_putc(rt_stdout, '\n');
    }
}

/* ---- ,c name: the session's imports and latest definitions as a program, compiled by
 * `shikra emit-c`; print the C functions of the definition (its procedure and the
 * procedures nested in it). */
static obj def_name(obj x) {
    if (!is_head(x, "define") || !rt_is_pair(RT_CDR(x))) return RT_FALSE;
    x = RT_CAR(RT_CDR(x));
    while (rt_is_pair(x)) x = RT_CAR(x); /* (define (f . args) ...) and curried forms */
    return x;
}
static int starts(const char *s, const char *p) { return !strncmp(s, p, strlen(p)); }
/* An import set of a REPL-only library ((scheme eval), possibly inside only/prefix/...). */
static int repl_only_set(obj spec) {
    while (rt_is_pair(spec) && rt_is_pair(RT_CDR(spec)) && rt_is_pair(RT_CAR(RT_CDR(spec))) &&
           (is_head(spec, "only") || is_head(spec, "except") || is_head(spec, "prefix") || is_head(spec, "rename")))
        spec = RT_CAR(RT_CDR(spec));
    return list_len(spec) == 2 && RT_CAR(spec) == sym("scheme") &&
           (RT_CAR(RT_CDR(spec)) == sym("eval") || RT_CAR(RT_CDR(spec)) == sym("load") || RT_CAR(RT_CDR(spec)) == sym("repl"));
}
static void show_c(obj name) {
    const char *nm = RT_STR(RT_PTR(name, rt_symbol)->name)->buf, *dir;
    obj l, defs = RT_NIL, port;
    char path[1024], want[600], base[600] = "", *out, *line, *next;
    sbuf text = {0}, got = {0};
    int fd, pfd[2], st, i, found = 0, printing = 0;
    pid_t pid;
    char **av;
    for (l = recorded; rt_is_pair(l); l = RT_CDR(l)) if (def_name(RT_CAR(l)) == name) found = 1;
    if (!found) { say(nm, " is not defined in this session (,c shows the C of a definition)", NULL); return; }
    for (l = recorded; rt_is_pair(l); l = RT_CDR(l)) { /* oldest first; latest definition per name */
        obj x = RT_CAR(l), m;
        int later = 0;
        if (def_name(x) != RT_FALSE)
            for (m = defs; rt_is_pair(m); m = RT_CDR(m)) if (def_name(RT_CAR(m)) == def_name(x)) later = 1;
        if (!later) defs = rt_cons(x, defs);
    }
    port = rt_open_output_string();
    for (l = defs; rt_is_pair(l); l = RT_CDR(l)) /* imports, without the REPL-only libraries */
        if (is_head(RT_CAR(l), "import")) {
            obj m;
            rt_port_puts(port, "(import");
            for (m = RT_CDR(RT_CAR(l)); rt_is_pair(m); m = RT_CDR(m))
                if (!repl_only_set(RT_CAR(m))) { rt_port_putc(port, ' '); rt_print(RT_CAR(m), port, 1); }
            rt_port_puts(port, ")\n");
        }
    for (l = defs; rt_is_pair(l); l = RT_CDR(l)) if (!is_head(RT_CAR(l), "import")) { rt_print(RT_CAR(l), port, 1); rt_port_putc(port, '\n'); }
    dir = getenv("TMPDIR");
    if (!dir || !*dir) dir = "/tmp";
    snprintf(path, sizeof path, "%s%sshikra-repl-XXXXXX", dir, dir[strlen(dir) - 1] == '/' ? "" : "/");
    if ((fd = mkstemp(path)) < 0) { say("cannot create a temporary file for ,c", NULL, NULL); return; }
    sb_puts(&text, RT_STR(rt_get_output_string(port))->buf);
    if (write(fd, text.s, text.len) != (ssize_t)text.len) { close(fd); unlink(path); say("cannot write ", path, NULL); return; }
    close(fd);
    av = (char **)xmalloc(sizeof(char *) * (size_t)(2 * opts->fo.nincdirs + 12));
    i = 0;
    av[i++] = (char *)opts->argv0;
    av[i++] = "emit-c";
    av[i++] = "-w";       /* no warnings: the session's imports are rarely all used */
    if (opts->fo.bignum) av[i++] = "--bignum";
    av[i++] = "--keep";   /* the definition stays a function even if inlined or unused */
    av[i++] = (char *)nm;
    av[i++] = "-I";
    av[i++] = (char *)(opts->fo.progdir ? opts->fo.progdir : ".");
    for (st = 0; st < opts->fo.nincdirs; st++) { av[i++] = "-I"; av[i++] = (char *)opts->fo.incdirs[st]; }
    av[i++] = path;
    av[i] = NULL;
    rt_flush(rt_stdout);
    if (pipe(pfd) != 0 || (pid = fork()) < 0) { unlink(path); say("cannot run the compiler for ,c", NULL, NULL); return; }
    if (pid == 0) {
        dup2(pfd[1], 1);
        close(pfd[0]);
        close(pfd[1]);
        execvp(av[0], av);
        _exit(127);
    }
    close(pfd[1]);
    for (;;) {
        char buf[8192];
        ssize_t n = read(pfd[0], buf, sizeof buf);
        if (n <= 0) break;
        sb_putn(&got, buf, (size_t)n);
    }
    close(pfd[0]);
    waitpid(pid, &st, 0);
    unlink(path);
    free(av);
    sb_free(&text);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0 || !got.s) {
        say("(,c compiles the session's imports and latest definitions as a program; the compiler rejected it)", NULL, NULL);
        sb_free(&got);
        return;
    }
    /* blocks: "/" "* main: NAME *" "/", "static obj P_main__X(...) {", ..., "}" */
    snprintf(want, sizeof want, "/* main: %s */", nm);
    for (out = got.s; out; out = next) {
        line = out;
        next = strchr(out, '\n');
        if (next) *next++ = 0;
        if (!base[0] && !strcmp(line, want) && next && starts(next, "static obj P_main__")) {
            char *e = strchr(next, '(');
            size_t n = e ? (size_t)(e - (next + 11)) : 0;
            if (n && n < sizeof base) {
                memcpy(base, next + 11, n);
                base[n] = 0;
                if (strstr(base + 8, "__")) base[0] = 0; /* nested in another definition */
            }
        }
    }
    if (!base[0]) {
        say(nm, " is not a procedure: the compiler computes its value in the program body", NULL);
        sb_free(&got);
        return;
    }
    fresh_line();
    for (out = got.s; out < got.s + got.len; out += strlen(out) + 1) {
        line = out;
        if (!printing && starts(line, "/* main: ") && out + strlen(out) + 1 < got.s + got.len) {
            char *fn = out + strlen(out) + 1;
            size_t bl = strlen(base);
            if (starts(fn, "static obj ") && starts(fn + 11, base) && (fn[11 + bl] == '(' || starts(fn + 11 + bl, "__")))
                printing = 1;
        }
        if (printing) {
            rt_port_puts(rt_stdout, line);
            rt_port_putc(rt_stdout, '\n');
            if (!strcmp(line, "}")) { printing = 0; rt_port_putc(rt_stdout, '\n'); }
        }
    }
    sb_free(&got);
}

/* ---- input: an entry continues until its parentheses balance ---- */
typedef struct { int depth, str, bar, block, content; } scan_state;
static void scan(scan_state *s, const char *p) {
    for (; *p; p++) {
        char c = *p;
        if (s->str) { if (c == '\\' && p[1]) p++; else if (c == '"') s->str = 0; continue; }
        if (s->bar) { if (c == '\\' && p[1]) p++; else if (c == '|') s->bar = 0; continue; }
        if (s->block) {
            if (c == '|' && p[1] == '#') { s->block--; p++; }
            else if (c == '#' && p[1] == '|') { s->block++; p++; }
            continue;
        }
        if (c == ';') return;
        if (c == '#' && p[1] == '|') { s->block = 1; p++; continue; }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f') continue;
        s->content = 1;
        if (c == '#' && p[1] == '\\') { p += 2; if (!*p) return; continue; } /* the char after #\ */
        if (c == '"') s->str = 1;
        else if (c == '|') s->bar = 1;
        else if (c == '(' || c == '[') s->depth++;
        else if (c == ')' || c == ']') s->depth--;
    }
}
/* 1: an entry (possibly blank) in b; 0: end of input. */
static int read_entry(sbuf *b) {
    scan_state s;
    memset(&s, 0, sizeof s);
    b->len = 0;
    for (;;) {
        obj line = rt_read_line(rt_stdin);
        const char *l;
        if (line == RT_EOF) return s.content;
        l = RT_STR(line)->buf;
        if (echo) { rt_port_puts(rt_stdout, l); rt_port_putc(rt_stdout, '\n'); }
        sb_puts(b, l);
        sb_putc(b, '\n');
        scan(&s, l);
        if (!s.content || (!s.str && !s.bar && !s.block && s.depth <= 0)) return 1;
    }
}
static obj read_one(const char *text) { /* first datum of text, or eof; errors abort the entry */
    obj a[2];
    rt_gc_set_immortal(IMM_PURE);
    a[0] = rt_open_input_string(rt_cstring(text));
    rt_gc_set_immortal(0);
    if (protect(read_cb, a)) return 0;
    return a[1];
}
static void meta(const char *text) {
    const char *w = text + 1, *e = w;
    obj x;
    while (*e && *e != ' ' && *e != '\t' && *e != '\n') e++;
    if ((size_t)(e - w) == 4 && !strncmp(w, "quit", 4)) rt_exit(0);
    if ((size_t)(e - w) == 6 && !strncmp(w, "expand", 6)) {
        if ((x = read_one(e)) && x != RT_EOF) protect(expand_cb, &x);
        else if (x) say("usage: ,expand EXPR", NULL, NULL);
        return;
    }
    if ((size_t)(e - w) == 1 && *w == 'c') {
        if ((x = read_one(e)) && rt_is_symbol(x)) show_c(x);
        else if (x) say("usage: ,c NAME", NULL, NULL);
        return;
    }
    say("unknown command (commands: ,expand EXPR  ,c NAME  ,quit)", NULL, NULL);
}
static void entry(const char *text) {
    obj a[2];
    const char *p = text;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p == ',') { meta(p); return; }
    rt_gc_set_immortal(IMM_PURE);
    a[0] = rt_open_input_string(rt_cstring(text));
    rt_gc_set_immortal(0);
    for (;;) {
        if (protect(read_cb, a) || a[1] == RT_EOF) return;
        if (protect(eval_cb, &a[1])) return; /* an error drops the rest of the entry */
    }
}

int repl_main(const char *file, interp_opts *o) {
    sbuf b = {0};
    opts = o;
    rt_unicode_full = o->fo.unicode_full; /* --unicode=full: the tool always has the tables */
    o->fo.repl = 1;
    rt_gc_set_immortal(IMM_PURE);
    front_init(&o->fo);
    rt_gc_set_immortal(0);
    vm_init(1);
    vm_abort = abort_entry;
    set_hooks();
    rt_gc_add_roots(&recorded, &recorded + 1);
    echo = !isatty(0);
    if (!file || protect(load_cb, (void *)file)) {
        if (!top) protect(default_cb, NULL);
    }
    for (;;) {
        fresh_line();
        rt_port_puts(rt_stdout, "shikra> ");
        if (!read_entry(&b)) break;
        entry(b.s);
    }
    rt_port_putc(rt_stdout, '\n');
    rt_exit(0);
}

int interp_main(const char *file, interp_opts *o, int nargs, char **args) {
    char **av = (char **)xmalloc(sizeof(char *) * (size_t)(nargs + 2));
    int i;
    av[0] = (char *)file;
    for (i = 0; i < nargs; i++) av[i + 1] = args[i];
    av[nargs + 1] = NULL;
    rt_argc = nargs + 1;
    rt_argv = av;
    opts = o;
    rt_unicode_full = o->fo.unicode_full;
    o->fo.repl = 1;
    compile_errors_init(); /* front-end errors: diagnostics and exit 1, as for build */
    rt_gc_set_immortal(IMM_PURE);
    front_init(&o->fo);
    vm_init(0);
    front_load_program(file);
    {
        tmpl *t = bc_unit(RT_NIL);
        rt_gc_set_immortal(0);
        vm_run(t);
    }
    rt_exit(0);
}
