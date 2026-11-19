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

/* Type lattice (spec §5.2) as a bit set: the flow analysis (analyze.c, DESIGN.md M4-2).
 * 0 = no value (not analysed, or unreachable). The three standard ports and the three
 * static port-parameter cells (pairs) have bits of their own; T_UNDEF is the letrec marker. */
enum {
    T_FIX = 1 << 0, T_FLO = 1 << 1, T_CHAR = 1 << 2, T_TRUE = 1 << 3, T_FALSE = 1 << 4,
    T_PAIR = 1 << 5, T_NULL = 1 << 6, T_STRING = 1 << 7, T_SYMBOL = 1 << 8, T_VECTOR = 1 << 9,
    T_BYTEVECTOR = 1 << 10, T_PROC = 1 << 11, T_RECORD = 1 << 12, T_EOF = 1 << 13,
    T_UNSPEC = 1 << 14, T_PORT = 1 << 15, T_STDIN = 1 << 16, T_STDOUT = 1 << 17, T_STDERR = 1 << 18,
    T_PCELL0 = 1 << 19, T_PCELL1 = 1 << 20, T_PCELL2 = 1 << 21, T_RTD = 1 << 22, T_UNDEF = 1 << 23,
    T_OTHER = 1 << 24, /* boxes, promises, error objects, values objects, markers */
    T_F64 = 1 << 25,   /* f64vector (shikra f64vector) */
    T_ANY = (1 << 26) - 1, T_TOP = T_ANY & ~T_UNDEF,
    T_BOOL = T_TRUE | T_FALSE, T_NUM = T_FIX | T_FLO, T_PCELL = T_PCELL0 | T_PCELL1 | T_PCELL2,
    T_STDPORT = T_STDIN | T_STDOUT | T_STDERR, T_PORTS = T_PORT | T_STDPORT
};
/* C representation of a variable or of a direct procedure's result (analyze.c, emit.c) */
enum { R_OBJ, R_FIX, R_DBL, R_MV };
/* ir_node.spec: PRIM: S_OK(i) argument i is proven to have the checked type, S_NOOVF the
 * fixnum result cannot overflow; atoms: N_FAR the value came through a user procedure's
 * parameter or result (diagnostics: interprocedural); any node: N_LIB copied from a standard
 * library procedure (its location is the call's); CALL: N_DISC known only through the analysis
 * (the callee's free variables need not be in scope here: never inlined unless it has none);
 * CALL of CTL_CALLCC: N_TIER3 the continuation may be invoked after the call/cc returned. */
#define S_OK(i) (1 << (i))
enum { S_NOOVF = 1 << 6, N_FAR = 1 << 8, N_LIB = 1 << 9, N_DISC = 1 << 10, N_TIER3 = 1 << 11 };

enum {
    V_GLOBAL = 1,   /* top-level variable (C global or static closure) */
    V_ASSIGNED = 2, /* target of SET (or letrec initialisation after use) */
    V_CAPTURED = 4, /* referenced from a procedure other than its owner */
    V_BOXED = 8,    /* holds a box: assigned and captured */
    V_CHECK = 16,   /* may be read before initialisation: reads test for RT_UNDEF */
    V_FAR = 32      /* parameter or result of an inlined user procedure (diagnostics) */
};

struct ir_var {
    int id;
    obj name;          /* source name (symbol) */
    unsigned flags;
    int nrefs, nsets;  /* def/use counts, recomputed by ir_count / the optimizer's census */
    int ncalls;        /* references that are the callee of a known call (optimizer census) */
    ir_lambda *owner;  /* binding procedure; NULL for globals */
    ir_lambda *known;  /* bound once to this procedure and never assigned: known callee */
    global *g;         /* globals: the front-end global */
    int type;          /* T_* bits (flow analysis) */
    int rep;           /* R_* C representation (analyze.c ir_represent) */
    ir_node *def;      /* optimizer scratch: the %values primitive that binds it */
    int def_index;     /* globals: index of the defining top-level form, -1 if none */
    int slot;          /* globals: slot in the C globals array (emitter) */
    ir_node *subst;    /* optimizer: every reference reads this atom instead (copy propagation) */
    int mark;          /* scratch for passes */
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
    IR_LETJ,   /* term: declare join points u.letj.j (and its ->next chain: mutually
                  recursive, each visible in the others' bodies), then run u.letj.body */
    IR_JUMP,   /* tail: u.jump */
    IR_IF,     /* tail: u.if_ */
    IR_RET     /* tail: return atom u.ret */
};
/* IR_CALL flavours: (%apply f l), (%call-with-values p c), (%call/cc f) (tier 2: rt_call_cc) */
enum { CTL_NONE, CTL_APPLY, CTL_CWV, CTL_CALLCC };

struct ir_node {
    int kind;
    int ty, spec; /* analysis: refined T_* type of an atom at this use; S_* / N_* flags */
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
    int njumps;        /* recomputed by ir_count / the census; never lower than the real count */
    ir_lambda *owner;  /* the procedure whose C function holds the label */
    ir_join *next;     /* further join points of the same LETJ (mutually recursive group) */
    obj site;          /* optimistic tier-1 continuation: the call/cc core form, else 0 */
    int state;         /* optimizer scratch */
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
    int dead;              /* optimizer: inlined, contified or unreachable; not emitted */
    int keep;              /* never removed and always a first-class closure (raise, --keep) */
    int escapes;           /* its closure may reach unknown code (so it keeps the uniform signature) */
    int direct;            /* direct C signature: obj f(params...), no self, no argc (M4-1 rule) */
    int mark, low;         /* scratch for passes */
    int size, sround;      /* optimizer: node count with nested procedures, and its round */
    int sidx, slow, onstack, rec; /* optimizer: SCC numbering; rec = can reach itself */
    int onstack2;          /* optimizer scratch (merging of mutual tail recursion) */
    int rtype;             /* analysis: T_* type of its results */
    int rrep, nvals;       /* direct: C representation of the result (R_MV: nvals values) */
};

/* Optimisation levels (-O): budgets in opt.c. */
enum { OPT_0, OPT_S, OPT_2 };

typedef struct {
    ir_lambda *lambdas;    /* every procedure (clauses included), creation order */
    ir_lambda *main;       /* program body: library initialisation, then the program */
    ir_var **globals;      /* every reachable global */
    int nglobals;
    ir_var **vars;         /* every variable, indexed by id */
    int nvars;
    ir_lambda *raise;      /* (scheme base) raise: the engine's raise entry, or NULL */
    ir_lambda *travel;     /* (shikra dynamic) travel-to!: tier-2 continuations' winders walk, or NULL */
    int opt;               /* OPT_* */
    int unsafe;            /* --unsafe (spec §3.3) */
    long growth;           /* inlining budget left (nodes), across the optimizer's runs */
    int nopt;              /* runs of the optimizer so far */
    int no_handlers;       /* no exception handler can be installed: errors print and exit */
    int no_flonums;        /* no flonum can exist at run time (no float printer, §9.1) */
    int std_open;          /* the standard ports are never closed (unchecked writes to them) */
    int tier3;             /* call/cc sites classified tier 3 (spec §8.2, §9.1: stack copying linked) */
    int warn;              /* report warnings */
} ir_program;

/* Options of the middle end. */
typedef struct {
    int opt;               /* OPT_* */
    int warn;              /* report unused definitions and imports (warnings) */
    const char **keep;     /* program definitions to keep as functions (REPL ,c), NULL-terminated */
    int unsafe;            /* --unsafe */
} ir_opts;

/* Middle end (normalize.c): tree shaking, normalization to IR, optimisation (opt.c),
 * closure layout. Reports the compile-time diagnostics of the middle end (arity of known
 * calls) and, with warn, unused definitions. */
ir_program *ir_build(library *order, const ir_opts *o);
void ir_count(ir_program *p);
void ir_print(ir_program *p, obj port);
void c_mangle(sbuf *b, const char *scheme_name); /* readable C identifier characters */

/* Shared constructors (normalize.c) for the passes. */
ir_var *ir_new_var(obj name, ir_lambda *owner);
ir_node *ir_new_node(int kind, obj loc);
ir_lambda *ir_new_lambda(obj name, ir_lambda *parent, library *lib);
ir_join *ir_new_join(int n, ir_lambda *owner);
int ir_call_self(ir_node *call); /* known call: 0 no self argument (direct), 1 the callee atom, 2 #f */

/* Optimizer (opt.c, DESIGN.md §11 M4-1). ir_optimize runs the structural passes;
 * ir_tier1_failures lists the optimistic tier-1 call/cc sites (core forms) whose
 * continuation is still reached from another C function (they are rebuilt as tier 2);
 * ir_finish computes escapes, direct signatures, lambda lifting and the closure layout. */
void ir_optimize(ir_program *p);
obj ir_tier1_failures(ir_program *p);
void ir_finish(ir_program *p);

/* Flow analysis and the passes it enables (analyze.c, DESIGN.md §11 M4-2). ir_analyze computes
 * types and reaching procedures, then rewrites: decided predicates and branches fold, code after
 * a call or primitive that cannot return is cut, calls whose operator is one procedure become
 * known, primitives get S_* flags; reports provably wrong types. Returns 1 if the IR changed
 * structurally (the optimizer then runs again). ir_represent (after ir_finish) chooses C
 * representations: unboxed fixnums and flonums, private signatures, values structs. */
int ir_analyze(ir_program *p, int last);
void ir_represent(ir_program *p);
int ir_atom_type(ir_node *a); /* T_* type of an atom at its use (T_TOP if unknown) */
/* call/cc sites (spec §8.2): after the final ir_analyze, an escape analysis of every %call/cc
 * continuation marks the sites whose k may be invoked after the call/cc returned (N_TIER3,
 * p->tier3). Assigned C locals of procedures that may be on the stack during a tier-3 capture
 * must live in boxes (a copied frame would bring back old values): returns 1 if it added such
 * variables to the normalizer's boxing set (the IR must then be rebuilt), else 0. */
int ir_classify_callcc(ir_program *p);
/* Normalizer: variables named in this set are boxed when assigned, and the optimizer leaves their
 * boxes alone (filled by ir_classify_callcc). */
extern pmap ir_forcebox;
int ir_const_type(obj x);
/* define-c (PI_CCALL, spec §10): the type letters of a %c-call's arguments ("a" for the
 * descriptor, then one per C argument) and the descriptor's C types */
const char *ir_ccall_letters(ir_node *p);
enum { FT_INT, FT_LONG, FT_DOUBLE, FT_FLOAT, FT_BOOL, FT_CHAR, FT_STRING, FT_POINTER, FT_VOID };
int ir_ffi_type(obj sym);              /* FT_*, -1 if unknown */
int ir_ccall_type(ir_node *p, int i);  /* FT_* of C argument i (i = -1: the result) */

/* Back end (emit.c): the complete C translation unit (DESIGN.md §3). */
typedef struct { int unsafe, debug, gc_bump, unicode_full, gc_stress, no_musttail; } emit_opts;
void emit_c(ir_program *p, const emit_opts *o, sbuf *out);
#endif
