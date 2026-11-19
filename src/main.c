/* main.c -- the shikra driver (spec §11). M0 part A: commands and flags are parsed;
 * the commands themselves are stubs. Hidden: `shikra debug-read FILE`. */
#include "rt.h" /* first: it sets the POSIX feature-test macros */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "util.h"

typedef enum { CMD_BUILD, CMD_RUN, CMD_REPL, CMD_CHECK, CMD_EMIT_C, CMD_SIZE, CMD_DEBUG_READ } command;

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
    const char *cc;         /* --cc */
    const char *target;     /* --target, NULL = host */
    const char **incdirs;   /* -I, in order */
    int nincdirs;
} options;

static const struct { const char *name; command cmd; } commands[] = {
    {"build", CMD_BUILD}, {"run", CMD_RUN}, {"repl", CMD_REPL}, {"check", CMD_CHECK},
    {"emit-c", CMD_EMIT_C}, {"size", CMD_SIZE}, {"debug-read", CMD_DEBUG_READ}, {0, CMD_BUILD}};

static void usage(FILE *f) {
    fputs("usage: shikra <command> [flags] [file]\n"
          "commands:\n"
          "  build main.scm -o app   compile a program to a native executable\n"
          "  run main.scm            build at -O0 into a cache directory and run it\n"
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
    if (argc < 2) { usage(stderr); exit(2); }
    if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help") || !strcmp(argv[1], "help")) { usage(stdout); exit(0); }
    for (k = 0; commands[k].name && strcmp(commands[k].name, argv[1]); k++) {}
    if (!commands[k].name) { fprintf(stderr, "shikra: unknown command '%s'\n", argv[1]); usage(stderr); exit(2); }
    o->cmd = commands[k].cmd;
    for (i = 2; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-O0") || !strcmp(a, "-O2") || !strcmp(a, "-Os")) o->opt = a[2];
        else if (!strcmp(a, "-O")) {
            if (i + 1 >= argc || strlen(argv[i + 1]) != 1 || !strchr("02s", argv[i + 1][0])) die("-O takes 0, 2 or s");
            o->opt = argv[++i][0];
        } else if (!strcmp(a, "--unsafe")) o->unsafe = 1;
        else if (!strcmp(a, "-g")) o->debug = 1;
        else if (!strcmp(a, "--gc-stress")) o->gc_stress = 1;
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

int main(int argc, char **argv) {
    options o;
    parse_args(argc, argv, &o);
    rt_init(argc, argv, (void *)&argc);
    if (o.cmd == CMD_DEBUG_READ) return debug_read(o.input);
    fprintf(stderr, "shikra: '%s' is not implemented yet\n", argv[1]);
    return 1;
}
