/* main.c -- the shikra driver (spec §11). Commands: build, run, repl, check, emit-c (size is
 * an M4 stub). Hidden commands:
 *   interp FILE [args]  run a program on the interpreter, like the compiled program (tests)
 *   debug-read FILE     the reader's datums
 *   debug-expand FILE   every top-level core form of the program and its libraries, with
 *                       renaming stripped (as the REPL's ,expand prints them)
 *   debug-core FILE     the same with unique names (locals "x.12", globals "lib/id:name")
 *   debug-ir FILE       the IR after tree shaking, normalization and optimization
 * Hidden flags: --gc-stress (collect at every allocation), --no-musttail (tail calls through
 * the trampoline), --keep NAME (keep a program definition as a function: REPL ,c), -w (no
 * warnings). */
#include "rt.h" /* first: it sets the POSIX feature-test macros */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include "util.h"
#include "front.h"
#include "ir.h"
#include "interp.h"

typedef enum {
    CMD_BUILD, CMD_RUN, CMD_REPL, CMD_CHECK, CMD_EMIT_C, CMD_SIZE, CMD_INTERP,
    CMD_DEBUG_READ, CMD_DEBUG_EXPAND, CMD_DEBUG_CORE, CMD_DEBUG_IR
} command;

typedef struct {
    command cmd;
    const char *input;      /* main.scm (or the binary for `size`) */
    const char *output;     /* -o */
    char opt;               /* -O: '0', '2' or 's' */
    int unsafe;             /* --unsafe */
    int gc_bump;            /* --gc=bump (default immix) */
    int unicode_full;       /* --unicode=full (default ascii) */
    int debug;              /* -g */
    int gc_stress;          /* --gc-stress (hidden, testing) */
    int no_musttail;        /* --no-musttail (hidden, testing): tail calls through the trampoline */
    int nowarn;             /* -w: no warnings */
    const char **keep;      /* --keep NAME (hidden, REPL ,c; repeatable): keep those definitions as functions */
    int nkeep;
    const char *cc;         /* --cc */
    const char *target;     /* --target, NULL = host */
    const char **incdirs;   /* -I, in order */
    int nincdirs;
    char **args;            /* run: arguments for the program */
    int nargs;
    int size_all, size_tsv; /* size: --all (every procedure), --tsv (machine-readable) */
    int no_cache;           /* --no-cache: expand every library from source */
} options;

int size_command(const char *path, int all, int tsv); /* size.c */

static const struct { const char *name; command cmd; } commands[] = {
    {"build", CMD_BUILD}, {"run", CMD_RUN}, {"repl", CMD_REPL}, {"check", CMD_CHECK},
    {"emit-c", CMD_EMIT_C}, {"size", CMD_SIZE}, {"interp", CMD_INTERP}, {"debug-read", CMD_DEBUG_READ},
    {"debug-expand", CMD_DEBUG_EXPAND}, {"debug-core", CMD_DEBUG_CORE}, {"debug-ir", CMD_DEBUG_IR},
    {0, CMD_BUILD}};

static void usage(FILE *f) {
    fputs("usage: shikra <command> [flags] [file]\n"
          "commands:\n"
          "  build main.scm -o app   compile a program to a native executable\n"
          "  run main.scm [args]     build at -O0 into a cache directory and run it\n"
          "  repl [main.scm]         start the interactive interpreter\n"
          "  check main.scm          expand and analyse only; report errors\n"
          "  emit-c main.scm         print the generated C\n"
          "  size app                break a binary down by library and procedure\n"
          "flags:\n"
          "  -O0 | -O2 | -Os         optimisation: fast build, speed (default), size\n"
          "  --unsafe                remove remaining run-time checks\n"
          "  --gc=immix|bump         collector (default immix)\n"
          "  --unicode=ascii|full    character tables (default ascii)\n"
          "  -I dir                  add a library search directory\n"
          "  --cc=CC                 C compiler to invoke (default cc)\n"
          "  --target=TRIPLE         cross-compilation target (default host)\n"
          "  -g                      add source locations to run-time errors\n"
          "  -o file                 output file\n", f);
}

/* Value of a flag given as "--name=value" or "--name value" (also "-Xvalue"/"-X value"). */
static const char *flag_value(int argc, char **argv, int *i, const char *name) {
    size_t n = strlen(name);
    if (strncmp(argv[*i], name, n) != 0) return NULL;
    if (argv[*i][n] == '=') return argv[*i] + n + 1;
    if (argv[*i][n] == 0) {
        if (*i + 1 >= argc) die("missing value for %s", name);
        return argv[++*i];
    }
    if (name[1] != '-') return argv[*i] + n; /* short flag: -Idir, -oapp */
    return NULL;
}

static void parse_args(int argc, char **argv, options *o) {
    int i, k;
    const char *v;
    memset(o, 0, sizeof *o);
    o->opt = '2';
    o->cc = "cc";
    o->incdirs = (const char **)xmalloc(sizeof(char *) * (size_t)argc);
    o->keep = (const char **)xmalloc(sizeof(char *) * (size_t)(argc + 1));
    if (argc < 2) { usage(stderr); exit(2); }
    if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help") || !strcmp(argv[1], "help")) { usage(stdout); exit(0); }
    for (k = 0; commands[k].name && strcmp(commands[k].name, argv[1]); k++) {}
    if (!commands[k].name) { fprintf(stderr, "shikra: unknown command '%s'\n", argv[1]); usage(stderr); exit(2); }
    o->cmd = commands[k].cmd;
    if (o->cmd == CMD_RUN) o->opt = '0';
    for (i = 2; i < argc; i++) {
        const char *a = argv[i];
        if ((o->cmd == CMD_RUN || o->cmd == CMD_INTERP) && o->input) { o->args = argv + i; o->nargs = argc - i; break; }
        if (!strcmp(a, "-O0") || !strcmp(a, "-O2") || !strcmp(a, "-Os")) o->opt = a[2];
        else if (!strcmp(a, "-O")) {
            if (i + 1 >= argc || strlen(argv[i + 1]) != 1 || !strchr("02s", argv[i + 1][0])) die("-O takes 0, 2 or s");
            o->opt = argv[++i][0];
        } else if (!strcmp(a, "--unsafe")) o->unsafe = 1;
        else if (!strcmp(a, "-g")) o->debug = 1;
        else if (!strcmp(a, "--gc-stress")) o->gc_stress = 1;
        else if (!strcmp(a, "--no-musttail")) o->no_musttail = 1;
        else if (!strcmp(a, "-w")) o->nowarn = 1;
        else if (!strcmp(a, "--no-cache")) o->no_cache = 1;
        else if (o->cmd == CMD_SIZE && !strcmp(a, "--all")) o->size_all = 1;
        else if (o->cmd == CMD_SIZE && !strcmp(a, "--tsv")) o->size_tsv = 1;
        else if ((v = flag_value(argc, argv, &i, "--keep"))) o->keep[o->nkeep++] = v;
        else if ((v = flag_value(argc, argv, &i, "--gc"))) {
            if (!strcmp(v, "bump")) o->gc_bump = 1;
            else if (!strcmp(v, "immix")) o->gc_bump = 0;
            else die("--gc takes immix or bump");
        } else if ((v = flag_value(argc, argv, &i, "--unicode"))) {
            if (!strcmp(v, "full")) o->unicode_full = 1;
            else if (!strcmp(v, "ascii")) o->unicode_full = 0;
            else die("--unicode takes ascii or full");
        } else if ((v = flag_value(argc, argv, &i, "--cc"))) o->cc = v;
        else if ((v = flag_value(argc, argv, &i, "--target"))) o->target = v;
        else if ((v = flag_value(argc, argv, &i, "-I"))) o->incdirs[o->nincdirs++] = v;
        else if ((v = flag_value(argc, argv, &i, "-o"))) o->output = v;
        else if (a[0] == '-' && a[1]) die("unknown flag '%s'", a);
        else if (o->input) die("more than one input file ('%s' and '%s')", o->input, a);
        else o->input = a;
    }
    if (!o->input && o->cmd != CMD_REPL) die("%s: missing input file", argv[1]);
}

static int debug_read(const char *path) {
    obj port, x;
    rt_srcloc_enable(1);
    port = rt_open_input_file(rt_cstring(path));
    while ((x = rt_read(port)) != RT_EOF) {
        rt_write(x, rt_stdout);
        rt_newline(rt_stdout);
    }
    rt_exit(0);
}

static char *dir_of(const char *path) {
    const char *s = strrchr(path, '/');
    char *d;
    if (!s) return arena_strdup(&compiler_arena, ".");
    d = arena_strndup(&compiler_arena, path, (size_t)(s - path));
    return *d ? d : arena_strdup(&compiler_arena, "/");
}

static const char *cache_dir(void);
static void front_options(const options *o, front_opts *fo) {
    memset(fo, 0, sizeof *fo);
    if (!o->no_cache && o->cmd != CMD_REPL && o->cmd != CMD_INTERP) fo->cache_dir = cache_dir(); /* expansion cache */
    fo->progdir = o->input ? dir_of(o->input) : ".";
    fo->incdirs = o->incdirs;
    fo->nincdirs = o->nincdirs;
    fo->unicode_full = o->unicode_full;
    fo->target = o->target;
}
/* SHIKRA_TIME=1: the time of each phase on stderr (cache measurements, DESIGN.md "M4-3 as built"). */
static double phase_t0;
static void phase(const char *name) {
    struct timespec ts;
    double t;
    if (!getenv("SHIKRA_TIME")) return;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    t = (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
    if (name) fprintf(stderr, "shikra: %-6s %8.2f ms\n", name, t - phase_t0);
    phase_t0 = t;
}
static library *run_front(const options *o) {
    front_opts fo;
    library *prog;
    phase(NULL);
    front_options(o, &fo);
    compile_errors_init();
    front_init(&fo);
    prog = front_load_program(o->input);
    phase("front");
    return prog;
}

/* The middle end with the driver's options; warnings come after a successful build of the
 * IR, so diagnostics come first. */
static ir_program *middle(options *o) {
    ir_opts io;
    ir_program *p;
    io.opt = o->opt == '0' ? OPT_0 : o->opt == 's' ? OPT_S : OPT_2;
    io.warn = !o->nowarn;
    o->keep[o->nkeep] = NULL;
    io.keep = o->keep;
    io.unsafe = o->unsafe;
    p = ir_build(front_order, &io);
    if (io.warn) front_warn_imports();
    phase("middle");
    return p;
}
static void gen_c(options *o, sbuf *out) {
    emit_opts eo;
    ir_program *p;
    run_front(o);
    p = middle(o);
    eo.unsafe = o->unsafe;
    eo.debug = o->debug;
    eo.gc_bump = o->gc_bump;
    eo.unicode_full = o->unicode_full;
    eo.gc_stress = o->gc_stress;
    eo.no_musttail = o->no_musttail;
    emit_c(p, &eo, out);
    phase("emit");
}

static void mkdirs(const char *path) {
    char *p = arena_strdup(&compiler_arena, path), *s;
    for (s = p + 1; *s; s++)
        if (*s == '/') { *s = 0; mkdir(p, 0777); *s = '/'; }
    if (mkdir(p, 0777) != 0 && errno != EEXIST) die("cannot create directory %s", p);
}
static const char *cache_dir(void) {
    const char *x = getenv("XDG_CACHE_HOME"), *h = getenv("HOME"), *e = getenv("SHIKRA_CACHE_DIR");
    sbuf b = {0};
    const char *r;
    if (e && *e) sb_puts(&b, e);
    else if (x && *x) sb_printf(&b, "%s/shikra", x);
    else if (h && *h) sb_printf(&b, "%s/.cache/shikra", h);
    else sb_printf(&b, "/tmp/shikra-%ld", (long)getuid());
    mkdirs(b.s);
    r = arena_strdup(&compiler_arena, b.s);
    sb_free(&b);
    return r;
}
static void write_text(const char *path, const sbuf *s) {
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(s->s, 1, s->len, f) != s->len || fclose(f) != 0) die("cannot write %s", path);
}

/* The C compiler command line (spec §11.1 flags; DESIGN.md §11 M0 part B). */
static char **cc_argv(const options *o, const char *cfile, const char *out) {
    char **av = (char **)xmalloc(sizeof(char *) * 64), *cc = arena_strdup(&compiler_arena, o->cc), *t;
    int n = 0, darwin;
    static char optflag[4] = "-O2";
    for (t = strtok(cc, " \t"); t && n < 40; t = strtok(NULL, " \t")) av[n++] = t;
    if (!n) die("--cc is empty");
    optflag[2] = o->opt;
#ifdef __APPLE__
    darwin = 1;
#else
    darwin = 0;
#endif
    if (o->target) {
        static char tflag[256];
        snprintf(tflag, sizeof tflag, "--target=%s", o->target);
        av[n++] = tflag;
        darwin = strstr(o->target, "darwin") || strstr(o->target, "apple");
    }
    av[n++] = "-std=c99";
    av[n++] = optflag;
    {   /* Apple clang splits every cold path (error calls) into its own function with its own
         * symbol: 15% of a large program, no measurable speed (DESIGN.md "M4-3 as built") */
        const char *base = strrchr(av[0], '/') ? strrchr(av[0], '/') + 1 : av[0];
        if ((darwin && !strcmp(base, "cc")) || !strncmp(base, "clang", 5)) { av[n++] = "-Xclang"; av[n++] = "-fno-split-cold-code"; }
    }
    av[n++] = "-ffunction-sections";
    av[n++] = "-fdata-sections";
    if (o->debug) av[n++] = "-g";
    av[n++] = darwin ? "-Wl,-dead_strip" : "-Wl,--gc-sections";
    av[n++] = "-o";
    av[n++] = (char *)out;
    av[n++] = (char *)cfile;
    if (!darwin) av[n++] = "-lm"; /* rt_sys_pow; libm is part of libSystem on macOS */
    av[n] = NULL;
    return av;
}
static int run_cc(const options *o, const char *cfile, const char *out) {
    char **av = cc_argv(o, cfile, out);
    pid_t pid;
    int st;
    fflush(stdout);
    if ((pid = fork()) < 0) die("fork failed");
    if (pid == 0) {
        execvp(av[0], av);
        fprintf(stderr, "shikra: cannot run C compiler '%s'\n", av[0]);
        _exit(127);
    }
    if (waitpid(pid, &st, 0) < 0) die("waitpid failed");
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

static const char *default_output(const char *input) {
    const char *b = strrchr(input, '/'), *dot;
    b = b ? b + 1 : input;
    dot = strrchr(b, '.');
    return dot && dot != b ? arena_strndup(&compiler_arena, b, (size_t)(dot - b)) : arena_strdup(&compiler_arena, "a.out");
}

static int cmd_build(options *o) {
    sbuf c = {0}, cf = {0};
    const char *out = o->output ? o->output : default_output(o->input);
    gen_c(o, &c);
    sb_printf(&cf, "%s/build-%ld.c", cache_dir(), (long)getpid());
    write_text(cf.s, &c);
    if (!run_cc(o, cf.s, out)) {
        fprintf(stderr, "shikra: C compilation failed; generated C kept in %s\n", cf.s);
        return 1;
    }
    remove(cf.s);
    return 0;
}

static int cmd_run(options *o) {
    sbuf c = {0}, bin = {0}, tmp = {0};
    unsigned long long h = 14695981039346656037ull;
    size_t i;
    char **av;
    const char *dir;
    gen_c(o, &c);
    sb_printf(&c, "/* cc: %s -O%c */\n", o->cc, o->opt); /* the key covers compiler and flags */
    for (i = 0; i < c.len; i++) h = (h ^ (unsigned char)c.s[i]) * 1099511628211ull;
    dir = cache_dir();
    sb_printf(&bin, "%s/run-%016llx", dir, h);
    if (access(bin.s, X_OK) != 0) { /* build under a private name, then rename atomically */
        sbuf cf = {0};
        sb_printf(&cf, "%s/run-%016llx-%ld.c", dir, h, (long)getpid());
        sb_printf(&tmp, "%s/run-%016llx-%ld.tmp", dir, h, (long)getpid());
        write_text(cf.s, &c);
        if (!run_cc(o, cf.s, tmp.s)) {
            fprintf(stderr, "shikra: C compilation failed; generated C kept in %s\n", cf.s);
            return 1;
        }
        remove(cf.s);
        if (rename(tmp.s, bin.s) != 0) die("cannot rename %s", tmp.s);
    }
    av = (char **)xmalloc(sizeof(char *) * (size_t)(o->nargs + 2));
    av[0] = (char *)o->input;
    for (i = 0; i < (size_t)o->nargs; i++) av[i + 1] = o->args[i];
    av[o->nargs + 1] = NULL;
    fflush(stdout);
    execv(bin.s, av);
    die("cannot execute %s", bin.s);
}

static void print_core(int raw) {
    library *l;
    obj f;
    for (l = front_order; l; l = l->next) {
        rt_port_puts(rt_stdout, ";; ");
        if (l->name == RT_FALSE) rt_port_puts(rt_stdout, "program");
        else rt_print(l->name, rt_stdout, 1);
        rt_port_putc(rt_stdout, '\n');
        for (f = l->core; f != RT_NIL; f = RT_CDR(f)) {
            unparse(RT_CAR(f), rt_stdout, raw);
            rt_port_putc(rt_stdout, '\n');
        }
    }
}

int main(int argc, char **argv) {
    options o;
    sbuf c = {0};
    parse_args(argc, argv, &o);
    rt_init(argc, argv, (void *)&argc);
    switch (o.cmd) {
    case CMD_DEBUG_READ: return debug_read(o.input);
    case CMD_DEBUG_EXPAND: case CMD_DEBUG_CORE:
        run_front(&o);
        print_core(o.cmd == CMD_DEBUG_CORE);
        rt_exit(0);
    case CMD_DEBUG_IR:
        run_front(&o);
        ir_print(middle(&o), rt_stdout);
        rt_exit(0);
    case CMD_CHECK:
        run_front(&o);
        middle(&o);
        return 0;
    case CMD_EMIT_C:
        gen_c(&o, &c);
        fwrite(c.s, 1, c.len, stdout);
        return 0;
    case CMD_BUILD: return cmd_build(&o);
    case CMD_RUN: return cmd_run(&o);
    case CMD_REPL: case CMD_INTERP: {
        interp_opts io;
        front_options(&o, &io.fo);
        io.argv0 = argv[0];
        if (o.cmd == CMD_REPL) return repl_main(o.input, &io);
        return interp_main(o.input, &io, o.nargs, o.args);
    }
    case CMD_SIZE: return size_command(o.input, o.size_all, o.size_tsv);
    }
    return 1;
}
