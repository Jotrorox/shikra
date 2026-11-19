/* interp.h -- the interpreter (spec §5.6, §11.2; DESIGN.md §8 and §11 "M3 as built"):
 * core forms -> bytecode (bytecode.c) -> a VM with its own value stack (vm.c), driven by
 * the REPL and the batch runner (repl.c). Tool code only: never part of compiled programs. */
#ifndef SHIKRA_INTERP_H
#define SHIKRA_INTERP_H
#include "front.h"

/* A template is the compiled code of one lambda clause (or of a top-level unit). It lives
 * in C memory and is never freed; a closure stores its address in free[0] (8-aligned, so
 * it looks like a fixnum to the collector) and its captured values in free[1..nfree].
 * Constants are the elements of an immortal vector. */
typedef struct tmpl tmpl;
struct tmpl {
    int32_t *code;
    obj *k;           /* constants */
    obj name;         /* symbol: arity errors ("f: wrong number of arguments") */
    int nreq, rest;   /* required parameters; rest: slot nreq receives the rest list */
    int nlocals;      /* frame slots: parameters, then let and letrec* variables */
    int maxstack;     /* temporaries above the locals, at most */
    int nfree;        /* captured values */
    int nclauses;     /* case-lambda: clause templates tried in order (sharing nfree) */
    tmpl **clauses;
};

/* Instructions: one int32 opcode, then int32 operands. acc is the value register; "push"
 * means *sp++ = acc. k = constant index, i/j/m = frame slot or free-variable index. */
enum {
    OP_CONST,   /* k        acc = K[k] */
    OP_LREF,    /* i        acc = fp[i] */
    OP_LSET,    /* i        fp[i] = acc */
    OP_FREF,    /* i        acc = closure->free[1+i] */
    OP_LBREF,   /* i        acc = box contents of fp[i] */
    OP_FBREF,   /* i        acc = box contents of free[1+i] */
    OP_LBREFC,  /* i k      checked: undefined -> "K[k]: variable used before its definition" */
    OP_FBREFC,  /* i k */
    OP_LBSET,   /* i        box fp[i] := acc; acc = unspecified */
    OP_FBSET,   /* i */
    OP_BOX,     /*          acc = new box holding acc */
    OP_MKBOX,   /* i        fp[i] = new box holding the undefined marker (letrec*) */
    OP_GREF,    /* k        acc = value of global cell K[k] (checked) */
    OP_GSET,    /* k */
    OP_GDEF,    /* k        define: cell := acc; acc = unspecified */
    OP_PUSH,
    OP_JMP,     /* a        pc = code + a */
    OP_JMPF,    /* a        if acc is #f */
    OP_CLOSURE, /* k n      acc = closure of template K[k] over the n pushed values */
    OP_FIXCL,   /* i j m    letrec*: closure in fp[i] (or in its box): free[1+j] = fp[m] */
    OP_FRAME,   /*          push 3 slots for the return information of a non-tail call */
    OP_CALL,    /* n        [frame f a1..an] -> call f */
    OP_TCALL,   /* n        [f a1..an] -> replace the current frame, call f */
    OP_RET,
    OP_PRIM,    /* p n      acc = prim p of the n-1 pushed values and acc */
    OP_VALUES,  /* n        (%values ...) of the n-1 pushed values and acc */
    OP_SPREADL, /*          push the elements of the list in acc (apply); nargs = length */
    OP_SPREADV, /*          push the values in acc (call-with-values); nargs = count */
    OP_CALLV,   /*          OP_CALL with n = nargs */
    OP_TCALLV,
    OP_CWVSWAP, /*          [producer] acc=consumer -> [consumer frame producer] */
    OP_VCHECK,  /* n r      values in acc fit a literal receiver of n (r: or more) parameters */
    OP_CALLCC,  /*          [frame f] -> call f with the continuation of this call */
    OP_TCALLCC, /*          [f] -> tail call f with the current procedure's continuation */
    OP_EVAL,    /*          [frame expr] acc=env -> compile expr, call the result */
    OP_TEVAL,
    OP_ENV,     /*          acc = (environment . import sets in acc) */
    OP_INTERENV,
    OP_CCALL,   /* i n      acc = C function i of ffi.c (define-c) on the n-1 pushed values and acc */
    OP_HALT     /*          return acc to vm_run's caller */
};

/* Front-end work (reading, expansion, bytecode) allocates in the collector's unscanned
 * immortal mode: such objects point only to immortal objects, static data and interned
 * symbols. Objects that may later point to collectable ones (global cells, the copies of
 * data handed to eval) are allocated scanned-immortal (IMM_ROOTS) instead. */
#define IMM_ROOTS 1
#define IMM_PURE 2

/* bytecode.c */
tmpl *bc_unit(obj forms);   /* bodies of libraries loaded since the last unit, then the
                             * top-level core forms; caller is in immortal mode */
obj bc_cell(global *g);     /* a global's value cell: vector [value, name, defined?] */

/* vm.c */
void vm_init(int repl);     /* repl: uncaught errors return to the prompt (vm_abort) */
obj vm_run(tmpl *t);        /* run a top-level template; its value */
void vm_reset(void);        /* after an abort: empty stack, default hooks */
extern void (*vm_abort)(void);           /* REPL: called after an uncaught error was printed */
extern library *vm_interaction;          /* (interaction-environment); NULL: made on demand */
obj vm_default_import_form(void);        /* (import (scheme base) ...): the REPL defaults */

/* ffi.c: define-c in the interpreter (dlsym in the running process; DESIGN.md "Extensions as built") */
int ffi_resolve(obj desc, obj where);    /* index of the C function; cerror if the interpreter cannot call it */
obj ffi_call(int i, int n, const obj *args);

/* repl.c */
typedef struct { front_opts fo; const char *argv0; } interp_opts;
int repl_main(const char *file, interp_opts *o);
int interp_main(const char *file, interp_opts *o, int nargs, char **args);
#endif
