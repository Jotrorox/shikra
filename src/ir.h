/* ir.h -- the middle-end IR (spec §5.1, DESIGN.md §7; details in DESIGN.md §11 M0 part B).
 *
 * A-normal form with join points. A procedure body is a *term*: a chain of bindings
 * (LET, FIX, LETJ) ending in a *tail* (RET, CALL with tail=1, JUMP, IF of two terms).
 * Every operand of PRIM, CALL, SET, JUMP, RET and IF's test is an *atom* (CONST or REF),
 * so every intermediate value has a name. Loops and merges are join points: labelled
 * blocks with parameters, entered only by JUMP from inside the term that declares them
 * (or from inside their own body: a loop). A procedure's entry join point
 * (ir_lambda.entry, parameters = the procedure's parameters) is the target of
 * self tail calls.
 *
 * Invariants: every ir_var is bound exactly once (parameter, LET, FIX, join parameter)
 * and has a unique id; ir_var.owner is the procedure that binds it (NULL for globals).
 * Variables are never both captured and assigned: such variables are boxed by the
 * normalizer (V_BOXED; reads and writes are %box-ref / %box-set! primitives).
 * All memory comes from compiler_arena. One tagged union, one switch per pass. */
#ifndef SHIKRA_IR_H
#define SHIKRA_IR_H
#include "front.h"

typedef struct ir_var ir_var;
typedef struct ir_node ir_node;
typedef struct ir_lambda ir_lambda;
typedef struct ir_join ir_join;

/* Type lattice (spec §5.2) as a bit set, for the M4 flow analysis. 0 = not analysed. */
enum {
    T_FIX = 1 << 0, T_FLO = 1 << 1, T_CHAR = 1 << 2, T_BOOL = 1 << 3, T_PAIR = 1 << 4,
    T_NULL = 1 << 5, T_STRING = 1 << 6, T_SYMBOL = 1 << 7, T_VECTOR = 1 << 8,
    T_BYTEVECTOR = 1 << 9, T_PROC = 1 << 10, T_RECORD = 1 << 11, T_OTHER = 1 << 12,
    T_ANY = (1 << 13) - 1
};

enum {
    V_GLOBAL = 1,   /* top-level variable (C global or static closure) */
    V_ASSIGNED = 2, /* target of SET (or letrec initialisation after use) */
    V_CAPTURED = 4, /* referenced from a procedure other than its owner */
    V_BOXED = 8,    /* holds a box: assigned and captured */
    V_CHECK = 16    /* may be read before initialisation: reads test for RT_UNDEF */
};

struct ir_var {
    int id;
    obj name;          /* source name (symbol) */
    unsigned flags;
    int nrefs, nsets;  /* def/use counts, recomputed by ir_count */
    ir_lambda *owner;  /* binding procedure; NULL for globals */
    ir_lambda *known;  /* bound once to this procedure and never assigned: known callee */
    global *g;         /* globals: the front-end global */
    int type;          /* M4: T_* bits */
    int def_index;     /* globals: index of the defining top-level form, -1 if none */
    int slot;          /* globals: slot in the C globals array (emitter) */
};

enum ir_kind {
    IR_CONST,  /* atom: u.value */
    IR_REF,    /* atom: u.ref (check: test for RT_UNDEF on read) */
    IR_PRIM,   /* rhs: u.prim, op indexes prim_table (prims.def row or PI_VALUES) */
    IR_CALL,   /* rhs or tail: u.call */
    IR_LAMBDA, /* rhs: closure creation, u.lam */
    IR_SET,    /* rhs: u.set, value unspecified (unboxed local or global) */
    IR_LET,    /* term: u.let; v NULL = evaluate rhs for effect */
    IR_FIX,    /* term: mutually recursive closures, u.fix */
    IR_LETJ,   /* term: declare join point u.letj.j, then run u.letj.body */
    IR_JUMP,   /* tail: u.jump */
    IR_IF,     /* tail: u.if_ */
    IR_RET     /* tail: return atom u.ret */
};
/* IR_CALL flavours: (%apply f l), (%call-with-values p c), (%call/cc f) (tier 2: rt_call_cc) */
enum { CTL_NONE, CTL_APPLY, CTL_CWV, CTL_CALLCC };

struct ir_node {
    int kind;
    obj loc; /* source pair for diagnostics, or 0 */
    union {
        obj value;
        struct { ir_var *v; int check; } ref;
        struct { int op; int n; ir_node **args; } prim;
        struct {
            ir_node *fn;       /* atom; CTL_*: unused (operands in args) */
            int n;
            ir_node **args;
            ir_lambda *known;  /* resolved callee (case-lambda: the clause), or NULL */
            int tail, ctl;
        } call;
        ir_lambda *lam;
        struct { ir_var *v; ir_node *val; } set;
        struct { ir_var *v; ir_node *rhs, *body; } let;
        struct { int n; ir_var **vars; ir_lambda **lams; ir_node *body; } fix;
        struct { ir_join *j; ir_node *body; } letj;
        struct { ir_join *j; int n; ir_node **args; } jump;
        struct { ir_node *test, *then, *els; } if_;
        ir_node *ret;
    } u;
};

struct ir_join {
    int id;
    int n;
    ir_var **params;
    ir_node *body;
    int njumps;        /* recomputed by ir_count */
    ir_lambda *owner;
};

struct ir_lambda {
    int id;
    obj name;              /* procedure name for messages ("fib", "lambda") */
    const char *cname;     /* C function name, e.g. P_main__fib */
    int nreq, rest;        /* arity: nreq required, rest = 1 if a rest list follows */
    int nparams;           /* nreq + rest */
    ir_var **params;
    ir_node *body;
    ir_join *entry;        /* self-tail-call target: params = params, body = body */
    int nfree;             /* closure layout: captured variables, in slot order */
    ir_var **free;
    ir_lambda *parent;     /* lexically enclosing procedure (NULL: top level) */
    int nclauses;          /* case-lambda: > 0, the clauses below; body unused */
    ir_lambda **clauses;
    ir_lambda *dispatch;   /* clause -> its case-lambda */
    ir_var *self;          /* variable bound to this procedure, if any */
    int is_static;         /* no free variables: a static closure constant */
    global *g;             /* known top-level procedure: its global */
    library *lib;          /* library whose code creates it (C names, shikra size) */
    ir_lambda *next;       /* all procedures, in creation order */
    int reached;           /* closure layout reached it (others were dead code) */
};

typedef struct {
    ir_lambda *lambdas;    /* every procedure (clauses included), creation order */
    ir_lambda *main;       /* program body: library initialisation, then the program */
    ir_var **globals;      /* every reachable global */
    int nglobals;
    ir_var **vars;         /* every variable, indexed by id */
    int nvars;
    ir_lambda *raise;      /* (scheme base) raise: the engine's raise entry, or NULL */
} ir_program;

/* Middle end (normalize.c): tree shaking, normalization to IR, closure layout.
 * Reports the compile-time diagnostics of the middle end (arity of known calls). */
ir_program *ir_build(library *order);
void ir_count(ir_program *p);
void ir_print(ir_program *p, obj port);
void c_mangle(sbuf *b, const char *scheme_name); /* readable C identifier characters */

/* Back end (emit.c): the complete C translation unit (DESIGN.md §3). */
typedef struct { int unsafe, debug, gc_bump, unicode_full, gc_stress; } emit_opts;
void emit_c(ir_program *p, const emit_opts *o, sbuf *out);
#endif
