/* front.h -- the shared front end (spec §5 passes 2-3, DESIGN.md §6): library resolution
 * and hygienic expansion to core forms. Used by the compiler and (M3) the REPL.
 *
 * Datums are ordinary runtime objects (the reader's output). Identifiers are symbols:
 * interned symbols come from source text; *aliases* are uninterned symbols created by
 * macro expansion, each remembering the identifier it renames and the environment of the
 * macro that introduced it (explicit renaming). Core output (see DESIGN.md §11 "M0 part B")
 * uses interned symbols only for keywords and %primitive names, and uninterned symbols
 * for variables: locals "name.N", globals "lib/id:name". */
#ifndef SHIKRA_FRONT_H
#define SHIKRA_FRONT_H
#include "rt.h"
#include "util.h"

/* ---- pointer-keyed hash map (tool code) ---- */
typedef struct { const void **k; void **v; size_t n, cap; } pmap;
void *pm_get(const pmap *m, const void *key);
void pm_put(pmap *m, const void *key, void *val);

extern arena compiler_arena; /* all front/middle/back end memory */
#define NEW(T) ((T *)arena_alloc(&compiler_arena, sizeof(T)))
#define NEWN(T, n) ((T *)arena_alloc(&compiler_arena, sizeof(T) * (size_t)((n) > 0 ? (n) : 1)))

/* ---- primitives: every row of prims.def, then the engine-specific control forms ---- */
enum { PF_PURE = 1, PF_ALLOC = 2, PF_FAIL = 4, PF_EFFECT = 8 };
enum {
#define PRIM(name, who, cfn, arity, args, res, flags) PI_##cfn,
#include "prims.def"
#undef PRIM
    NPRIMS,
    PI_APPLY = NPRIMS, /* (%apply f list) */
    PI_VALUES,         /* (%values x ...) */
    PI_CWV,            /* (%call-with-values producer consumer) */
    PI_CALLCC,         /* (%call/cc f) */
    PI_EVAL,           /* (%eval expr env)            REPL only: the interpreter implements */
    PI_ENVIRONMENT,    /* (%environment import-sets)  these three; the compiler rejects them */
    PI_INTERENV,       /* (%interaction-environment)  (DESIGN.md §11 M3) */
    NPRIMS_ALL
};
typedef struct { const char *name, *who, *cfn; int arity; const char *args; char res; int flags; } prim_info;
extern const prim_info prim_table[NPRIMS_ALL]; /* arity -1: any number */

/* ---- environments ---- */
typedef struct library library;
typedef struct frame frame;
typedef struct macro macro;

/* Core keywords and auxiliary syntax (bound in the hidden library (shikra primitives)). */
enum {
    K_DEFINE, K_LAMBDA, K_IF, K_QUOTE, K_SET, K_BEGIN, K_LET, K_LETSTAR, K_LETREC, K_LETREC_STAR,
    K_COND, K_CASE, K_AND, K_OR, K_WHEN, K_UNLESS, K_DO, K_QUASIQUOTE, K_DEFINE_SYNTAX,
    K_LET_SYNTAX, K_LETREC_SYNTAX, K_SYNTAX_RULES, K_SYNTAX_ERROR, K_DEFINE_RECORD_TYPE,
    K_CASE_LAMBDA, K_LET_VALUES, K_LETSTAR_VALUES, K_DEFINE_VALUES, K_INCLUDE, K_INCLUDE_CI,
    K_COND_EXPAND, K_DELAY, K_DELAY_FORCE, K_FEATURES,
    K_ELSE, K_ARROW, K_ELLIPSIS, K_UNDERSCORE, K_UNQUOTE, K_UNQUOTE_SPLICING, NKEYWORDS
};
extern const char *const keyword_names[NKEYWORDS];

typedef struct global {
    obj sym;      /* unique uninterned symbol "lib/id:name" used in core forms */
    obj name;     /* the identifier's symbol as written */
    library *lib; /* defining library */
    int assigned; /* some set! targets it */
    int defined;  /* number of definitions seen */
    obj defloc;   /* the define form (diagnostics) */
    void *mid;    /* middle end: its ir_var */
    obj cell;     /* interpreter: its value cell (0 until first compiled) */
    int nowarn;   /* never reported as an unused definition (record procedures, hygienic names) */
} global;

enum { B_CORE, B_MACRO, B_GLOBAL, B_LOCAL, B_PRIM };
typedef struct binding {
    int kind;
    int id;      /* B_CORE: K_*; B_PRIM: PI_* */
    macro *mac;  /* B_MACRO */
    global *g;   /* B_GLOBAL */
    obj sym;     /* B_LOCAL: the unique local symbol */
} binding;

struct frame {
    frame *parent;
    pmap map;     /* identifier (symbol object) -> binding* */
    library *lib; /* top frames: the library */
    int open;     /* REPL top level: unbound variables become globals, redefinition allowed */
};

typedef struct imp { obj name; binding *b; obj loc; struct imp *next; } imp;
/* An import set of a user library or the program, for the unused-import warning. */
typedef struct import_rec { obj spec; int used; struct import_rec *next; } import_rec;

struct library {
    obj name;       /* (scheme base); #f for the program */
    const char *id; /* "scheme/base"; "main" for the program */
    const char *path; /* source file ("<builtin>/scheme/base.sld" for embedded ones) */
    int state;      /* 0 new, 1 loading, 2 done */
    frame *env;     /* top-level frame */
    obj export_specs; /* list of (internal-id external-name . loc) */
    imp *exports;   /* resolved after expansion */
    obj body;       /* top-level forms to expand (list) */
    obj core;       /* expanded top-level core forms, in order (list) */
    library *next;  /* initialisation order (front_order) */
    import_rec *imports; /* user libraries and the program: their import sets */
    pmap imported;  /* identifier -> chain of (import set, binding) it was imported by (library.c) */
};

/* ---- options and entry points ---- */
typedef struct {
    const char *progdir;   /* directory of the program (library search, first) */
    const char **incdirs;  /* -I directories */
    int nincdirs;
    int unicode_full;      /* feature full-unicode */
    const char *target;    /* target triple or NULL for the host (features) */
    int repl;              /* interpreter: (scheme eval), (scheme load), (scheme repl) exist */
} front_opts;

void front_init(const front_opts *o);
/* Read, resolve and expand the program and every library it needs. Returns the program;
 * front_order lists every library with code in initialisation order, program last. */
library *front_load_program(const char *path);
extern library *front_order;
global *front_global(obj sym);        /* global of a core-form variable symbol, or NULL */
obj front_local_name(obj sym);        /* source name of a local core symbol, or NULL */

/* REPL entry points (M3): an open top level where globals can be redefined and forms are
 * expanded one at a time against a persistent environment. */
library *front_repl_new(void);
void front_repl_import(library *top, obj import_form);
obj front_repl_expand(library *top, obj form); /* list of top-level core forms */
void front_repl_recover(void); /* after an error escaped the front end: forget half-loaded libraries */
/* When set, cerror calls it with the location ("file:line:col", or NULL) and the message
 * instead of printing and exiting; it must not return (REPL, eval). */
extern void (*cerror_hook)(const char *loc, const char *msg);

/* Unparse a core form: raw = 0 strips renaming and prints (quote x) as 'x (,expand);
 * raw = 1 prints unique names. */
void unparse(obj x, obj port, int raw);

/* ---- diagnostics: "file:line:col: error: msg" on stderr, exit 1 ---- */
UTIL_NORETURN void cerror(obj where, const char *fmt, ...) UTIL_PRINTF(2, 3);
void cwarning(obj where, const char *fmt, ...) UTIL_PRINTF(2, 3); /* "file:line:col: warning: msg" */
void front_note_use(library *lib, obj id, binding *b); /* an identifier resolved in lib's top frame */
void front_warn_imports(void);        /* unused import sets of the program and user libraries */
int front_builtin(library *lib);      /* a built-in (embedded) library */
extern obj loc_ctx;                   /* fallback location: innermost located form */
int has_loc(obj x);
obj setloc(obj dst, obj src);         /* give pair dst src's location (or loc_ctx's) unless it has one */
const char *loc_file(obj x);          /* file of x (or loc_ctx), NULL if unknown */
const char *idname(obj id);           /* printable name of an identifier */
const char *dstr(obj x);              /* written representation (syntax stripped) */
void compile_errors_init(void);       /* runtime errors in the compiler exit 1, located */

/* ---- shared internals (library.c <-> expand.c) ---- */
extern frame *core_env;               /* top frame of (shikra primitives) */
extern obj S_define, S_lambda, S_if, S_quote, S_set, S_letrec_star, S_begin, S_case_lambda;
obj sym(const char *s);
int list_len(obj x);                  /* -1 if not a proper list */
obj L2(obj a, obj b);
obj L3(obj a, obj b, obj c);
obj L4(obj a, obj b, obj c, obj d);
obj append2(obj a, obj b);
frame *new_frame(frame *parent);
binding *lookup(frame *env, obj id);
int is_ident(obj x);
obj strip_id(obj id);
obj strip_syntax(obj x);
void expand_toplevel(library *lib, obj forms);
obj cond_expand_select(obj form);     /* chosen clause body, or () */
obj front_features(void);              /* feature identifiers (spec §3.4), newest first */
obj read_source(const char *path, int foldcase, obj loc); /* list of datums */
const char *resolve_include(obj where, const char *name);
global *new_global(library *lib, obj id);
binding *new_binding(int kind);
int library_exists(obj name);
#endif
