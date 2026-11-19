/* rt.h -- Shikra runtime: value representation and runtime API (spec §6, DESIGN.md §3-§5).
 *
 * Two lives (DESIGN.md §3):
 *   tool mode     rt_*.c are ordinary translation units of the shikra tool;
 *   program mode  SHIKRA_PROGRAM is defined and the emitter pastes config defines, this
 *                 header, the generated code and the rt_*.c texts into ONE translation
 *                 unit. Every runtime function and global is then static (RT_FN/RT_VAR).
 * Each rt_*.c starts with `#ifndef SHIKRA_RT_H / #include "rt.h"` so it works in both.
 *
 * Only freestanding headers are included here; libc is touched in rt_sys.c only. */
#ifndef SHIKRA_RT_H
#define SHIKRA_RT_H

/* Feature-test macros must precede every system header of the TU (rt_sys.c needs POSIX). */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif

#include <stddef.h>
#include <stdint.h>

typedef char rt_assert_64bit[(sizeof(void *) == 8 && sizeof(double) == 8 && sizeof(intptr_t) == 8) ? 1 : -1];

/* ---- compiler support, each with a portable fallback ---- */
#if defined(__GNUC__) || defined(__clang__)
#define RT_NORETURN __attribute__((noreturn))
#define RT_UNUSED __attribute__((unused))
#define RT_NOINLINE __attribute__((noinline))
#define RT_LIKELY(x) __builtin_expect(!!(x), 1)
#define RT_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define RT_NORETURN
#define RT_UNUSED
#define RT_NOINLINE
#define RT_LIKELY(x) (x)
#define RT_UNLIKELY(x) (x)
#endif

/* ---- linkage ----
 * RT_FN      every runtime function (declaration and definition)
 * RT_VAR     definition of a runtime global (in its rt_*.c)
 * RT_EXTVAR  declaration of a runtime global (here). In program mode it is a static
 *            tentative definition, which C99 allows to repeat within one TU. */
#ifdef SHIKRA_PROGRAM
#define RT_FN static RT_UNUSED
#define RT_VAR static RT_UNUSED
#define RT_EXTVAR static RT_UNUSED
#else
#define RT_FN
#define RT_VAR
#define RT_EXTVAR extern
#endif
#define RT_INLINE static inline RT_UNUSED

/* ---- values (spec §6) ----
 * obj is a pointer to an incomplete type, so static data such as (obj)((char *)&lit + 5)
 * or (obj)0x07 is a valid C99 address constant in initializers.
 *   ...0  fixnum (n << 1)       001 pair (two words, no header)
 *   011   procedure             101 other heap object (8-byte header)
 *   111   immediate: bits 3..7 subtype (0 constant, 1 char), payload from bit 8 */
typedef struct rt_obj_ *obj;
#define RT_W(x) ((uintptr_t)(x))
#define RT_O(w) ((obj)(uintptr_t)(w))
#define RT_TAG(x) (RT_W(x) & 7)

#define RT_FALSE ((obj)0x007)
#define RT_TRUE ((obj)0x107)
#define RT_NIL ((obj)0x207)
#define RT_EOF ((obj)0x307)
#define RT_UNSPEC ((obj)0x407)
#define RT_UNDEF ((obj)0x507)   /* letrec "not yet initialized" marker */
#define RT_DEFAULT ((obj)0x607) /* "default object" marker for optional arguments */
#define RT_BOOL(c) ((c) ? RT_TRUE : RT_FALSE)
#define RT_CHAR(cp) RT_O(((uintptr_t)(cp) << 8) | 0x0F)
#define RT_CHARVAL(x) ((int)(RT_W(x) >> 8))

#define RT_FIX(n) RT_O((uintptr_t)(intptr_t)(n) << 1)
#define RT_FIXVAL(x) ((intptr_t)(x) >> 1) /* arithmetic shift assumed */
#define RT_FIX_MAX (INTPTR_MAX >> 1)
#define RT_FIX_MIN (-RT_FIX_MAX - 1)

/* Header: 8 bits type | 8 bits flags | 48 bits length. */
#define RT_HDR(t, f, len) ((uintptr_t)(t) | ((uintptr_t)(f) << 8) | ((uintptr_t)(len) << 16))
#define RT_HDRREF(x) (*(uintptr_t *)((char *)(x) - 5))
#define RT_HTYPE(x) ((int)(RT_HDRREF(x) & 0xFF))
#define RT_HFLAGS(x) ((int)((RT_HDRREF(x) >> 8) & 0xFF))
#define RT_HLEN(x) ((intptr_t)(RT_HDRREF(x) >> 16))
#define RT_SETFLAGS(x, f) (RT_HDRREF(x) = (RT_HDRREF(x) & ~(uintptr_t)0xFF00) | ((uintptr_t)(f) << 8))
#define RT_PTR(x, T) ((T *)((char *)(x) - 5))
#define RT_TAGPTR(p) RT_O((char *)(p) + 5)

/* Heap object types. The collector (M2) derives scanning from the class:
 *   1..15   every word after the header is an obj; length = number of words
 *   16..31  raw, no obj fields; length = number of payload bytes
 *   32..    special fixed layout, scanned field by field (see each struct) */
enum {
    RT_T_VECTOR = 1, RT_T_RECORD, /* [rtd][fields...], length = 1 + nfields */
    RT_T_RTD,                     /* [name symbol][field-name vector] */
    RT_T_BOX, RT_T_VALUES, RT_T_ERROR, /* [message][irritants][loc]; flags = kind */
    RT_T_SYMBOL,                  /* [name string] */
    RT_T_PROMISE,                 /* [state pair] (scheme lazy) */
    RT_T_PARAMETER,               /* reserved (parameters are closures, DESIGN.md M1 part A) */
    RT_T_BYTEVECTOR = 16, RT_T_FLONUM, RT_T_RAW, RT_T_F64VECTOR, RT_T_BIGNUM, RT_T_CONT,
    RT_T_STRING = 32,             /* rt_string: only buf is traced (untagged, may be static) */
    RT_T_PORT,                    /* rt_port: buf (untagged) and name are traced */
    RT_T_PROC                     /* header sits one word BEFORE the code pointer */
};
/* Header flags */
#define RT_F_RO 0x01          /* read-only literal (strings, vectors, bytevectors; pairs: RT_LITPAIR) */
#define RT_F_ASCII 0x02       /* string: chars == bytes */
#define RT_F_PLACEHOLDER 0x04 /* box used by the reader for datum labels */
enum { RT_ERR_GENERAL = 0, RT_ERR_FILE = 1, RT_ERR_READ = 2 }; /* error-object kind (flags) */

typedef struct { obj car, cdr; } rt_pair;
typedef struct { uintptr_t hdr; double d; } rt_flonum;
typedef struct { uintptr_t hdr; obj v[]; } rt_vector; /* also record, values, rtd, error, box */
typedef struct { uintptr_t hdr; unsigned char b[]; } rt_bytevector;
typedef struct { uintptr_t hdr; intptr_t chars, bytes; char *buf; } rt_string; /* buf: NUL-terminated UTF-8 */
typedef struct { uintptr_t hdr; obj name; } rt_symbol;

/* Procedures (tag 011): the obj points at the code word; the header (type RT_T_PROC,
 * length = number of captured values) is the word before it. All compiled code shares
 * the uniform signature; arguments beyond a3 go through a runtime argument buffer. */
typedef obj (*rt_code)(obj self, intptr_t argc, obj a0, obj a1, obj a2, obj a3);
typedef struct { uintptr_t hdr; rt_code code; obj free[]; } rt_proc;
#define RT_PROC(x) ((rt_proc *)((char *)(x) - 3 - sizeof(uintptr_t)))
#define RT_PROC_OBJ(p) RT_O((char *)&(p)->code + 3)
#define RT_SPROC(c) ((obj)((char *)&(c).code + 3)) /* static closure: an address constant */

/* Ports: fixed fields. buf is a raw heap buffer (or static memory). */
typedef struct {
    uintptr_t hdr;      /* flags: RT_PF_* */
    intptr_t fd;        /* OS descriptor, -1 for string ports */
    char *buf;
    intptr_t pos, len, cap; /* input: unread bytes are buf[pos..len); output: buf[0..len) */
    intptr_t line, col; /* input: 1-based line, chars consumed on it; output (fd): col 0 at line start */
    obj name;           /* file name string or #f; used for read-error locations */
} rt_port;
#define RT_PF_IN 0x01
#define RT_PF_OUT 0x02
#define RT_PF_BINARY 0x04
#define RT_PF_OPEN 0x08
#define RT_PF_STRING 0x10
#define RT_PF_UNBUF 0x20   /* flush after every write (stderr) */
#define RT_PF_FOLDCASE 0x40 /* #!fold-case seen by the reader */

/* ---- C-level predicates (int) ---- */
RT_INLINE int rt_is_fix(obj x) { return (RT_W(x) & 1) == 0; }
RT_INLINE int rt_is_pair(obj x) { return RT_TAG(x) == 1; }
RT_INLINE int rt_is_proc(obj x) { return RT_TAG(x) == 3; }
RT_INLINE int rt_is_heap(obj x) { return RT_TAG(x) == 5; }
RT_INLINE int rt_is_type(obj x, int t) { return RT_TAG(x) == 5 && RT_HTYPE(x) == t; }
RT_INLINE int rt_is_char(obj x) { return (RT_W(x) & 0xFF) == 0x0F; }
RT_INLINE int rt_is_flo(obj x) { return rt_is_type(x, RT_T_FLONUM); }
RT_INLINE int rt_is_num(obj x) { return rt_is_fix(x) || rt_is_flo(x); }
RT_INLINE int rt_is_string(obj x) { return rt_is_type(x, RT_T_STRING); }
RT_INLINE int rt_is_symbol(obj x) { return rt_is_type(x, RT_T_SYMBOL); }
RT_INLINE int rt_is_vector(obj x) { return rt_is_type(x, RT_T_VECTOR); }
RT_INLINE int rt_is_bytevector(obj x) { return rt_is_type(x, RT_T_BYTEVECTOR); }
RT_INLINE int rt_is_port(obj x) { return rt_is_type(x, RT_T_PORT); }
#define RT_CAR(x) (((rt_pair *)((char *)(x) - 1))->car)
#define RT_CDR(x) (((rt_pair *)((char *)(x) - 1))->cdr)
#define RT_FLO(x) (RT_PTR(x, rt_flonum)->d)
#define RT_VEC(x) (RT_PTR(x, rt_vector)->v)
#define RT_STR(x) RT_PTR(x, rt_string)
#define RT_PORT(x) RT_PTR(x, rt_port)

/* ---- checked fixnum arithmetic on intptr_t ---- */
#if defined(__has_builtin)
#if __has_builtin(__builtin_add_overflow) && __has_builtin(__builtin_mul_overflow)
#define RT_HAVE_OVF 1
#endif
#elif defined(__GNUC__) && __GNUC__ >= 5
#define RT_HAVE_OVF 1
#endif
#ifdef RT_HAVE_OVF
RT_INLINE int rt_add_ovf(intptr_t a, intptr_t b, intptr_t *r) { return __builtin_add_overflow(a, b, r); }
RT_INLINE int rt_sub_ovf(intptr_t a, intptr_t b, intptr_t *r) { return __builtin_sub_overflow(a, b, r); }
RT_INLINE int rt_mul_ovf(intptr_t a, intptr_t b, intptr_t *r) { return __builtin_mul_overflow(a, b, r); }
#else
RT_INLINE int rt_add_ovf(intptr_t a, intptr_t b, intptr_t *r) {
    if ((b > 0 && a > INTPTR_MAX - b) || (b < 0 && a < INTPTR_MIN - b)) return 1;
    *r = a + b; return 0;
}
RT_INLINE int rt_sub_ovf(intptr_t a, intptr_t b, intptr_t *r) {
    if ((b < 0 && a > INTPTR_MAX + b) || (b > 0 && a < INTPTR_MIN + b)) return 1;
    *r = a - b; return 0;
}
RT_INLINE int rt_mul_ovf(intptr_t a, intptr_t b, intptr_t *r) {
    if (a > 0 ? (b > 0 ? a > INTPTR_MAX / b : b < INTPTR_MIN / a)
              : (a < 0 && (b > 0 ? a < INTPTR_MIN / b : b < INTPTR_MAX / a))) return 1;
    *r = a * b; return 0;
}
#endif

/* ---- platform layer (rt_sys.c; the only file that touches libc / the OS) ---- */
enum { RT_O_READ = 0, RT_O_WRITE = 1, RT_O_APPEND = 2 };
RT_FN intptr_t rt_sys_write(int fd, const void *buf, size_t n); /* bytes written or -1 */
RT_FN intptr_t rt_sys_read(int fd, void *buf, size_t n);        /* 0 at EOF, -1 on error */
RT_FN int rt_sys_open(const char *path, int mode);              /* fd or -1 */
RT_FN int rt_sys_close(int fd);
RT_FN int rt_sys_ready(int fd);                      /* 1 if a read would not block */
RT_FN double rt_sys_pow(double x, double y);         /* libm pow: (scheme base) expt */
RT_FN RT_NORETURN void rt_sys_exit(int code);
RT_FN void *rt_sys_mem(size_t n);                    /* zeroed memory from the OS; NULL on failure */
RT_FN void rt_sys_free(void *p, size_t n);           /* return rt_sys_mem(n) memory (collector only) */
RT_FN const char *rt_sys_getenv(const char *name);   /* NULL if unset (stress builds read SHIKRA_GC_STRESS) */
/* Non-local exits (tier 2 continuations, spec §8.2): rt_sys_catch runs fn(arg) and returns 0
 * when it returns, 1 when rt_sys_throw(b) left it. __builtin_setjmp where the compiler
 * supports it (GCC; clang on x86 only), else sigsetjmp(b, 0) from <setjmp.h>, included by
 * rt_sys.c only (the size of sigjmp_buf is checked there). */
#if (defined(__GNUC__) && !defined(__clang__)) || (defined(__clang__) && (defined(__x86_64__) || defined(__i386__)))
#define RT_BUILTIN_SETJMP 1
typedef struct { void *w[5]; } rt_jmpbuf;
#else
typedef struct { union { void *p; double d; long double ld; } w[48]; } rt_jmpbuf;
#endif
RT_FN int rt_sys_catch(rt_jmpbuf *b, void (*fn)(void *), void *arg);
RT_FN RT_NORETURN void rt_sys_throw(rt_jmpbuf *b);
RT_FN int64_t rt_sys_clock_ns(int monotonic);        /* realtime or monotonic nanoseconds */
RT_FN void rt_memcpy(void *d, const void *s, size_t n);
RT_FN void rt_memmove(void *d, const void *s, size_t n);
RT_FN void rt_memset(void *d, int c, size_t n);
RT_FN int rt_memcmp(const void *a, const void *b, size_t n);
RT_FN size_t rt_strlen(const char *s);

/* ---- memory (rt_gc.c; spec §7, DESIGN.md §5 and "M2 as built") ----
 * Two bump streams: headerless pairs live in pair-only blocks (every 16-byte cell is an
 * object start), headered objects elsewhere. The inline fast paths are add, compare, store;
 * rt_alloc_slow refills the stream from free lines and may collect first. Collection
 * happens only inside allocation (or rt_gc_collect), so callers must initialise every field
 * of a fresh object before the next allocation. Roots: the C stack and registers
 * (conservative: any word pointing at or into an object), registered ranges, immortal
 * objects. A program that never allocates never references rt_alloc_slow, so the
 * collector is dropped by dead-code elimination (spec §9.1). */
RT_EXTVAR char *rt_hp, *rt_hlim; /* headered-object stream */
RT_EXTVAR char *rt_pp, *rt_plim; /* pair stream */
RT_FN void *rt_alloc_slow(size_t bytes, int pair_stream);
/* Allocate a headered object of `bytes` (header included); writes the header and
 * returns a pointer to it (tag it with RT_TAGPTR, or + 3 + 8 for procedures). */
RT_INLINE void *rt_alloc(uintptr_t hdr, size_t bytes) {
    char *p = rt_hp;
    bytes = (bytes + 7) & ~(size_t)7;
    if (RT_UNLIKELY((size_t)(rt_hlim - p) < bytes)) p = (char *)rt_alloc_slow(bytes, 0);
    else rt_hp = p + bytes;
    *(uintptr_t *)p = hdr;
    return p;
}
RT_INLINE obj rt_cons(obj a, obj d) {
    char *p = rt_pp;
    if (RT_UNLIKELY((size_t)(rt_plim - p) < sizeof(rt_pair))) p = (char *)rt_alloc_slow(sizeof(rt_pair), 1);
    else rt_pp = p + sizeof(rt_pair);
    ((rt_pair *)p)->car = a;
    ((rt_pair *)p)->cdr = d;
    return RT_O(p + 1);
}
RT_FN void *rt_alloc_raw(size_t nbytes);           /* raw (unscanned) buffer; returns its data */
RT_FN void rt_gc_init(void *stack_base);           /* records the stack base only (no allocation) */
RT_FN void rt_gc_add_roots(obj *start, obj *end);  /* register a range of obj words as roots */
RT_FN void rt_gc_add_dynamic_roots(obj **lo, obj **hi); /* the range [*lo, *hi), read at each collection (VM stack) */
/* Immortal mode (the REPL front end): 1 = allocations are never reclaimed and are scanned as
 * roots; 2 = never reclaimed and NOT scanned ("pure": only for objects that point to nothing but
 * immortal objects, static data and interned symbols); 0 = off. */
RT_FN void rt_gc_set_immortal(int on);
RT_FN void rt_gc_enable(int on);                   /* automatic collection; off in tool mode until the REPL enables it */
RT_FN void rt_gc_collect(void);                    /* collect now (no-op under bump or before the first allocation) */

/* Literal pairs (spec §3.2): the generated code puts every quoted pair in one static const
 * array, so set-car!/set-cdr! detect a literal with one subtraction and one comparison.
 * RT_NLITPAIRS and RT_NSTATIC_SYMS are emitted before this header (program mode). */
#if defined(SHIKRA_PROGRAM) && RT_NLITPAIRS > 0
static const rt_pair rt_lit_pairs[RT_NLITPAIRS];
#define RT_LITPAIR(x) ((uintptr_t)((const char *)(x) - 1 - (const char *)rt_lit_pairs) < sizeof rt_lit_pairs)
#elif !defined(SHIKRA_PROGRAM)
/* Tool: the interpreter's quoted data are immortal heap pairs (DESIGN.md §11 M3). */
RT_FN int rt_gc_immortal_pair(obj x);
#define RT_LITPAIR(x) rt_gc_immortal_pair(x)
#else
#define RT_LITPAIR(x) 0
#endif
#if defined(SHIKRA_PROGRAM) && RT_NSTATIC_SYMS > 0
static const obj rt_static_syms[RT_NSTATIC_SYMS]; /* entered into the symbol table when it is created */
#endif

/* ---- initialisation and exit (rt_core.c) ---- */
RT_FN void rt_init(int argc, char **argv, void *stack_base);
RT_FN RT_NORETURN void rt_exit(int code); /* flushes the standard ports */
RT_EXTVAR int rt_argc;
RT_EXTVAR char **rt_argv;
#ifdef SHIKRA_PROGRAM
/* Provided by the generated code: the program body (rt_sys.c's main calls it). */
static void rt_program_main(void);
#endif

/* ---- error path (rt_core.c; DESIGN.md §3 "Engine boundary") ----
 * rt_error builds an error object and calls rt_raise, which hands it to the engine:
 *   program mode: rt_engine_raise (declared here, DEFINED BY THE GENERATED CODE);
 *   tool mode:    rt_raise_hook if set (it must not return, e.g. longjmp to the VM).
 * With no engine handler: rt_uncaught prints "Error: msg irritant ..." (or
 * "loc: error: msg ..." when the error carries a location) to stderr, exits 70. */
#ifdef SHIKRA_PROGRAM
static RT_NORETURN void rt_engine_raise(obj e);
#else
RT_EXTVAR void (*rt_raise_hook)(obj e);
#endif
RT_FN RT_NORETURN void rt_raise(obj e);
RT_FN RT_NORETURN void rt_uncaught(obj e);
RT_FN void rt_print_error(obj e); /* what rt_uncaught prints, without exiting (the REPL) */
RT_FN RT_NORETURN void rt_error(const char *who, const char *msg, obj irritants); /* "who: msg" */
RT_FN RT_NORETURN void rt_type_error(const char *who, const char *expected, obj x);
RT_FN RT_NORETURN void rt_range_error(const char *who, obj x);
RT_FN obj rt_make_error(int kind, obj message, obj irritants, obj loc);

/* ---- core objects (rt_core.c) ---- */
RT_FN obj rt_list_p(obj x);
RT_FN obj rt_length(obj x);
RT_FN intptr_t rt_list_length(obj x, const char *who); /* checked proper list */
RT_FN obj rt_reverse(obj x);
RT_FN obj rt_eqv_p(obj a, obj b);
RT_FN obj rt_equal_p(obj a, obj b);
RT_FN int rt_eqv(obj a, obj b);
RT_FN int rt_equal(obj a, obj b);
RT_FN obj rt_intern(const char *s, size_t nbytes);
RT_FN obj rt_make_symbol(obj name); /* uninterned */
RT_FN obj rt_string_to_symbol(obj s);
RT_FN obj rt_symbol_to_string(obj s);
RT_FN obj rt_make_vector(obj k, obj fill);
RT_FN obj rt_vector_n(intptr_t n, obj fill); /* unchecked n */
RT_FN obj rt_list_to_vector(obj l);
RT_FN obj rt_vector_to_list(obj v, obj start, obj end);
RT_FN intptr_t rt_check_range(obj start, obj end, intptr_t len, intptr_t *e, const char *who); /* -> start */
RT_FN obj rt_make_bytevector(obj k, obj fill);
RT_FN obj rt_bytevector_copy(obj bv, obj start, obj end);
RT_FN obj rt_bytevector_copy_x(obj to, obj at, obj from, obj start, obj end);
RT_FN obj rt_make_box(obj v);
RT_FN obj rt_make_promise(obj done, obj value);
RT_FN obj rt_promise_p(obj x);
RT_FN obj rt_promise_state(obj p);
RT_FN obj rt_promise_set_state(obj p, obj state);
RT_FN obj rt_make_rtd(obj name, obj fields);
RT_FN obj rt_make_record(obj rtd);
RT_FN obj rt_record_p(obj x, obj rtd);
RT_FN RT_NORETURN void rt_record_error(obj x, obj rtd);
RT_FN obj rt_make_error_object(obj kind, obj message, obj irritants);
RT_FN obj rt_error_object_p(obj x);
RT_FN obj rt_error_object_message(obj x);
RT_FN obj rt_error_object_irritants(obj x);
RT_FN obj rt_file_error_p(obj x);
RT_FN obj rt_read_error_p(obj x);
RT_FN obj rt_make_values(obj list);
RT_FN obj rt_values_n(intptr_t n, const obj *v);
RT_FN obj rt_values_to_list(obj v);
RT_FN obj rt_make_proc(rt_code code, intptr_t nfree); /* free slots set to #f */
/* Address tables (write's datum labels, equal? on cycles): entries keyed by one or two object
 * addresses (b = 0 for one), value 0 when inserted; NULL if absent and !insert. Zero-initialise. */
typedef struct { uintptr_t *e; intptr_t cap, n; } rt_addrtab;
RT_FN intptr_t *rt_addrtab_get(rt_addrtab *t, obj a, obj b, int insert);

/* ---- compiled calling convention (rt_core.c; spec §8.1, DESIGN.md §4, §11 M0 part B) ----
 * Every compiled procedure: obj f(obj self, intptr_t argc, obj a0, obj a1, obj a2, obj a3).
 * Arguments beyond a3 are stored by the caller in rt_args[0..argc-5] right before the call
 * and copied out by the callee at entry, before anything else can call. Tail calls are
 * `RT_MUSTTAIL return f(...);`. */
#ifdef __has_attribute
#if __has_attribute(musttail)
#define RT_MUSTTAIL __attribute__((musttail))
#endif
#endif
#ifndef RT_MUSTTAIL
/* TODO(M4): trampoline fallback (spec §8.1). Until then compilers without musttail rely on
 * sibling-call optimisation, which -O2 performs for these calls but -O0 does not. */
#define RT_MUSTTAIL
#endif
#define RT_ARGBUF 64
RT_EXTVAR obj *rt_args;
RT_FN void rt_args_reserve(intptr_t nextra); /* room for nextra arguments beyond a3 */
RT_FN obj rt_rest_list(intptr_t argc, intptr_t nreq, obj a0, obj a1, obj a2, obj a3);
RT_FN RT_NORETURN void rt_arity_error(const char *who, intptr_t argc);
RT_FN RT_NORETURN void rt_not_procedure(obj f);
RT_FN RT_NORETURN void rt_undefined_error(const char *name);
/* Spread a list (apply) or a values object (call-with-values) into a0..a3 + rt_args. */
RT_FN intptr_t rt_spread(obj args, obj *regs);
RT_FN intptr_t rt_spread_values(obj v, obj *regs);
RT_FN obj rt_append2(obj a, obj b);
RT_FN obj rt_uncaught_o(obj e);
/* Multiple values spread into a known receiver (normalize.c, call-with-values fallback). */
RT_FN obj rt_values_check(obj v, obj nreq, obj rest); /* raises unless v has nreq (or more) values */
RT_FN obj rt_values_ref(obj v, obj i);
RT_FN obj rt_values_tail(obj v, obj i);               /* list of the values from i on */

/* ---- dynamic state (rt_core.c; spec §8.3, §8.4; DESIGN.md §11 M1 part A) ----
 * Both are Scheme lists, innermost first, read and written by lib/ through %primitives.
 * rt_winders: (before . after) frames of dynamic-wind (car is a procedure) and
 *             (cell . value) frames of parameterize (car is the parameter's cell, a pair
 *             (converter . global-value)).
 * rt_handlers: exception handler procedures. */
RT_EXTVAR obj rt_winders, rt_handlers;
RT_FN obj rt_winders_get(void);
RT_FN obj rt_winders_set(obj w);
RT_FN obj rt_handlers_get(void);
RT_FN obj rt_handlers_set(obj h);
RT_FN obj rt_parameter_binding(obj cell); /* innermost frame for cell, else cell; cdr = value */
RT_FN obj rt_continuation_live_p(obj k);  /* #f only for a dead tier-2 escape */
#ifdef SHIKRA_PROGRAM
/* The compiled engine's %call/cc (tier 2): calls f with an escape procedure that restores
 * the winders and handler lists and jumps back here with its arguments as the result. */
RT_FN obj rt_call_cc(obj f);
#endif
RT_INLINE obj rt_chkdef(obj v, const char *name) {
    if (RT_UNLIKELY(v == RT_UNDEF)) rt_undefined_error(name);
    return v;
}
#define RT_CALLABLE(f) do { if (RT_UNLIKELY(!rt_is_proc(f))) rt_not_procedure(f); } while (0)

/* ---- strings and chars (rt_str.c) ---- */
RT_FN int rt_utf8_encode(char *out, int cp);   /* returns 1..4 */
RT_FN int rt_utf8_decode(const char *s, size_t n, size_t *len); /* code point, or 0xFFFD */
RT_FN size_t rt_utf8_count(const char *s, size_t n);
RT_FN obj rt_make_string_n(const char *s, size_t nbytes); /* copies; counts chars */
RT_FN obj rt_cstring(const char *s);
RT_FN obj rt_make_string(obj k, obj ch);
RT_FN obj rt_string_ref(obj s, obj k);
RT_FN obj rt_string_set(obj s, obj k, obj ch);
RT_FN obj rt_substring(obj s, obj start, obj end);
RT_FN obj rt_string_copy_x(obj to, obj at, obj from, obj start, obj end);
RT_FN obj rt_string_fill_x(obj s, obj c, obj start, obj end);
RT_FN obj rt_string_append(obj a, obj b);
RT_FN obj rt_string_compare(obj a, obj b); /* -1, 0, 1 (code point order) */
RT_FN obj rt_string_to_list(obj s, obj start, obj end);
RT_FN obj rt_list_to_string(obj l);
RT_FN obj rt_string_to_utf8(obj s);
RT_FN obj rt_utf8_to_string(obj bv);
RT_FN obj rt_integer_to_char(obj n);

/* ---- numbers (rt_num.c) ---- */
RT_FN obj rt_make_flonum(double d);
RT_FN obj rt_add2(obj a, obj b);
RT_FN obj rt_sub2(obj a, obj b);
RT_FN obj rt_mul2(obj a, obj b);
RT_FN obj rt_div(obj a, obj b);
RT_FN int rt_numcmp(obj a, obj b, const char *who); /* -1 0 1, or 2 if unordered (NaN) */
RT_FN obj rt_num_eq(obj a, obj b);
RT_FN obj rt_num_le(obj a, obj b);
RT_FN obj rt_num_ge(obj a, obj b);
RT_FN obj rt_lt2(obj a, obj b);
RT_FN obj rt_gt2(obj a, obj b);
RT_FN obj rt_quotient(obj a, obj b);
RT_FN obj rt_remainder(obj a, obj b);
RT_FN obj rt_modulo(obj a, obj b);
RT_FN obj rt_floor_quotient(obj a, obj b);
RT_FN obj rt_neg(obj x);
RT_FN obj rt_abs(obj x);
RT_FN obj rt_expt(obj a, obj b);
RT_FN obj rt_exact(obj x);
RT_FN obj rt_inexact(obj x);
RT_FN obj rt_floor(obj x);
RT_FN obj rt_ceiling(obj x);
RT_FN obj rt_round(obj x);
RT_FN obj rt_truncate(obj x);
RT_FN obj rt_integer_p(obj x);
RT_FN obj rt_nan_p(obj x);
RT_FN obj rt_infinite_p(obj x);
RT_FN obj rt_finite_p(obj x);
RT_FN obj rt_number_to_string(obj n, obj radix);
RT_FN obj rt_string_to_number(obj s, obj radix);
/* Parse number syntax (R7RS 7.1.1, Shikra subset). Returns 0 and sets *out on success,
 * -1 if the text is not a number, -2 if it is a number Shikra cannot represent. */
RT_FN int rt_parse_number(const char *s, size_t n, int radix, obj *out);
RT_FN size_t rt_format_double(double d, char *out); /* >= 32 bytes; no NUL */

/* Generic arithmetic: inline fixnum fast path, out-of-line general case. */
RT_INLINE obj rt_add(obj a, obj b) {
    intptr_t r;
    if (RT_LIKELY(rt_is_fix(a) && rt_is_fix(b)) && !rt_add_ovf((intptr_t)a, (intptr_t)b, &r)) return RT_O(r);
    return rt_add2(a, b);
}
RT_INLINE obj rt_sub(obj a, obj b) {
    intptr_t r;
    if (RT_LIKELY(rt_is_fix(a) && rt_is_fix(b)) && !rt_sub_ovf((intptr_t)a, (intptr_t)b, &r)) return RT_O(r);
    return rt_sub2(a, b);
}
RT_INLINE obj rt_mul(obj a, obj b) {
    intptr_t r;
    if (RT_LIKELY(rt_is_fix(a) && rt_is_fix(b)) && !rt_mul_ovf(RT_FIXVAL(a), (intptr_t)b, &r)) return RT_O(r);
    return rt_mul2(a, b);
}
RT_INLINE obj rt_lt(obj a, obj b) {
    if (RT_LIKELY(rt_is_fix(a) && rt_is_fix(b))) return RT_BOOL((intptr_t)a < (intptr_t)b);
    return rt_lt2(a, b);
}
RT_INLINE obj rt_gt(obj a, obj b) {
    if (RT_LIKELY(rt_is_fix(a) && rt_is_fix(b))) return RT_BOOL((intptr_t)a > (intptr_t)b);
    return rt_gt2(a, b);
}

/* ---- ports and printing (rt_io.c) ---- */
RT_EXTVAR obj rt_stdin, rt_stdout, rt_stderr;
RT_EXTVAR obj rt_ports; /* open OS-descriptor output ports other than stdout/stderr (flushed at exit) */
RT_FN obj rt_make_fd_port(int fd, int flags, obj name);
RT_FN obj rt_open_input_string(obj s);
RT_FN obj rt_open_output_string(void);
RT_FN obj rt_get_output_string(obj port);
RT_FN obj rt_open_input_bytevector(obj bv);
RT_FN obj rt_open_output_bytevector(void);
RT_FN obj rt_get_output_bytevector(obj port);
RT_FN obj rt_open_input_file(obj path);  /* raises a file error */
RT_FN obj rt_open_output_file(obj path);
RT_FN obj rt_close_port(obj port);
RT_FN int rt_port_getc(obj port);  /* next code point or -1; tracks line/col */
RT_FN int rt_port_peekc(obj port);
RT_FN void rt_port_write(obj port, const char *s, size_t n);
RT_FN void rt_port_putc(obj port, int cp);
RT_FN void rt_port_puts(obj port, const char *s);
RT_FN void rt_flush(obj port);
RT_FN void rt_flush_all(void);
#define RT_PRINT_WRITE 1  /* rt_print modes: external representation (else display) */
#define RT_PRINT_SHARED 2 /* datum labels for all shared pairs and vectors (else for cycles only) */
#define RT_PRINT_SIMPLE 4 /* no datum labels */
RT_FN void rt_print(obj x, obj port, int mode);
RT_FN obj rt_input_port_p(obj x);
RT_FN obj rt_output_port_p(obj x);
RT_FN obj rt_textual_port_p(obj x);
RT_FN obj rt_binary_port_p(obj x);
RT_FN obj rt_input_port_open_p(obj port);
RT_FN obj rt_output_port_open_p(obj port);
RT_FN obj rt_read_char(obj port);
RT_FN obj rt_peek_char(obj port);
RT_FN obj rt_read_line(obj port);
RT_FN obj rt_read_string(obj k, obj port);
RT_FN obj rt_char_ready_p(obj port);
RT_FN obj rt_read_u8(obj port);
RT_FN obj rt_peek_u8(obj port);
RT_FN obj rt_u8_ready_p(obj port);
RT_FN obj rt_read_bytevector_x(obj bv, obj port, obj start, obj end); /* count or eof */
RT_FN obj rt_write_u8(obj b, obj port);
RT_FN obj rt_write_bytevector(obj bv, obj port, obj start, obj end);
RT_FN obj rt_write_char(obj c, obj port);
RT_FN obj rt_write_string(obj s, obj port);
RT_FN obj rt_write(obj x, obj port);
RT_FN obj rt_write_shared(obj x, obj port);
RT_FN obj rt_write_simple(obj x, obj port);
RT_FN obj rt_display(obj x, obj port);
RT_FN obj rt_newline(obj port);
RT_FN obj rt_flush_output_port(obj port);
RT_FN obj rt_exit_o(obj code);

/* ---- reader (rt_read.c) ---- */
RT_FN obj rt_read(obj port); /* next datum or the eof object; raises read errors */
#ifndef SHIKRA_PROGRAM
/* Tool mode: source locations of pairs created by the reader from ports with a name. */
RT_FN void rt_srcloc_enable(int on);
RT_FN int rt_srcloc_lookup(obj pair, obj *file, intptr_t *line, intptr_t *col); /* 1 if found */
RT_FN void rt_srcloc_set(obj pair, obj file, intptr_t line, intptr_t col);
#endif

/* ---- inline checked primitives (named in prims.def) ---- */
#define RT_CHECK(c, who, ty, x) do { if (RT_UNLIKELY(!(c))) rt_type_error(who, ty, x); } while (0)
RT_INLINE obj rt_car(obj x) { RT_CHECK(rt_is_pair(x), "car", "pair", x); return RT_CAR(x); }
RT_INLINE obj rt_cdr(obj x) { RT_CHECK(rt_is_pair(x), "cdr", "pair", x); return RT_CDR(x); }
#ifdef RT_UNSAFE
#define RT_CHECK_MUTABLE_PAIR(x, who) ((void)0)
#else
#define RT_CHECK_MUTABLE_PAIR(x, who) do { if (RT_UNLIKELY(RT_LITPAIR(x))) rt_type_error(who, "mutable pair", x); } while (0)
#endif
RT_INLINE obj rt_set_car(obj x, obj v) {
    RT_CHECK(rt_is_pair(x), "set-car!", "pair", x);
    RT_CHECK_MUTABLE_PAIR(x, "set-car!");
    RT_CAR(x) = v;
    return RT_UNSPEC;
}
RT_INLINE obj rt_set_cdr(obj x, obj v) {
    RT_CHECK(rt_is_pair(x), "set-cdr!", "pair", x);
    RT_CHECK_MUTABLE_PAIR(x, "set-cdr!");
    RT_CDR(x) = v;
    return RT_UNSPEC;
}
RT_INLINE obj rt_pair_p(obj x) { return RT_BOOL(rt_is_pair(x)); }
RT_INLINE obj rt_null_p(obj x) { return RT_BOOL(x == RT_NIL); }
RT_INLINE obj rt_eq_p(obj a, obj b) { return RT_BOOL(a == b); }
RT_INLINE obj rt_boolean_p(obj x) { return RT_BOOL(x == RT_TRUE || x == RT_FALSE); }
RT_INLINE obj rt_symbol_p(obj x) { return RT_BOOL(rt_is_symbol(x)); }
RT_INLINE obj rt_procedure_p(obj x) { return RT_BOOL(rt_is_proc(x)); }
RT_INLINE obj rt_char_p(obj x) { return RT_BOOL(rt_is_char(x)); }
RT_INLINE obj rt_char_to_integer(obj c) { RT_CHECK(rt_is_char(c), "char->integer", "char", c); return RT_FIX(RT_CHARVAL(c)); }
RT_INLINE obj rt_string_p(obj x) { return RT_BOOL(rt_is_string(x)); }
RT_INLINE obj rt_string_length(obj s) { RT_CHECK(rt_is_string(s), "string-length", "string", s); return RT_FIX(RT_STR(s)->chars); }
RT_INLINE obj rt_vector_p(obj x) { return RT_BOOL(rt_is_vector(x)); }
RT_INLINE obj rt_vector_length(obj v) { RT_CHECK(rt_is_vector(v), "vector-length", "vector", v); return RT_FIX(RT_HLEN(v)); }
RT_INLINE obj rt_vector_ref(obj v, obj k) {
    RT_CHECK(rt_is_vector(v), "vector-ref", "vector", v);
    if (RT_UNLIKELY(!rt_is_fix(k) || (uintptr_t)RT_FIXVAL(k) >= (uintptr_t)RT_HLEN(v))) rt_range_error("vector-ref", k);
    return RT_VEC(v)[RT_FIXVAL(k)];
}
RT_INLINE obj rt_vector_set(obj v, obj k, obj x) {
    RT_CHECK(rt_is_vector(v) && !(RT_HFLAGS(v) & RT_F_RO), "vector-set!", "mutable vector", v);
    if (RT_UNLIKELY(!rt_is_fix(k) || (uintptr_t)RT_FIXVAL(k) >= (uintptr_t)RT_HLEN(v))) rt_range_error("vector-set!", k);
    RT_VEC(v)[RT_FIXVAL(k)] = x;
    return RT_UNSPEC;
}
RT_INLINE obj rt_bytevector_p(obj x) { return RT_BOOL(rt_is_bytevector(x)); }
RT_INLINE obj rt_bytevector_length(obj v) { RT_CHECK(rt_is_bytevector(v), "bytevector-length", "bytevector", v); return RT_FIX(RT_HLEN(v)); }
RT_INLINE obj rt_bytevector_u8_ref(obj v, obj k) {
    RT_CHECK(rt_is_bytevector(v), "bytevector-u8-ref", "bytevector", v);
    if (RT_UNLIKELY(!rt_is_fix(k) || (uintptr_t)RT_FIXVAL(k) >= (uintptr_t)RT_HLEN(v))) rt_range_error("bytevector-u8-ref", k);
    return RT_FIX(RT_PTR(v, rt_bytevector)->b[RT_FIXVAL(k)]);
}
RT_INLINE obj rt_bytevector_u8_set(obj v, obj k, obj b) {
    RT_CHECK(rt_is_bytevector(v) && !(RT_HFLAGS(v) & RT_F_RO), "bytevector-u8-set!", "mutable bytevector", v);
    if (RT_UNLIKELY(!rt_is_fix(k) || (uintptr_t)RT_FIXVAL(k) >= (uintptr_t)RT_HLEN(v))) rt_range_error("bytevector-u8-set!", k);
    if (RT_UNLIKELY(!rt_is_fix(b) || (uintptr_t)RT_FIXVAL(b) > 255)) rt_type_error("bytevector-u8-set!", "byte", b);
    RT_PTR(v, rt_bytevector)->b[RT_FIXVAL(k)] = (unsigned char)RT_FIXVAL(b);
    return RT_UNSPEC;
}
RT_INLINE obj rt_box_ref(obj b) { RT_CHECK(rt_is_type(b, RT_T_BOX), "box-ref", "box", b); return RT_VEC(b)[0]; }
RT_INLINE obj rt_box_set(obj b, obj v) { RT_CHECK(rt_is_type(b, RT_T_BOX), "box-set!", "box", b); RT_VEC(b)[0] = v; return RT_UNSPEC; }
RT_INLINE obj rt_record_ref(obj r, obj rtd, obj k) {
    if (RT_UNLIKELY(!rt_is_type(r, RT_T_RECORD) || RT_VEC(r)[0] != rtd)) rt_record_error(r, rtd);
    return RT_VEC(r)[1 + RT_FIXVAL(k)]; /* k is a compiler-generated constant */
}
RT_INLINE obj rt_record_set(obj r, obj rtd, obj k, obj v) {
    if (RT_UNLIKELY(!rt_is_type(r, RT_T_RECORD) || RT_VEC(r)[0] != rtd)) rt_record_error(r, rtd);
    RT_VEC(r)[1 + RT_FIXVAL(k)] = v;
    return RT_UNSPEC;
}
RT_INLINE obj rt_values_p(obj x) { return RT_BOOL(rt_is_type(x, RT_T_VALUES)); }
RT_INLINE obj rt_number_p(obj x) { return RT_BOOL(rt_is_num(x)); }
RT_INLINE obj rt_exact_p(obj x) { RT_CHECK(rt_is_num(x), "exact?", "number", x); return RT_BOOL(rt_is_fix(x)); }
RT_INLINE obj rt_inexact_p(obj x) { RT_CHECK(rt_is_num(x), "inexact?", "number", x); return RT_BOOL(rt_is_flo(x)); }
RT_INLINE obj rt_exact_integer_p(obj x) { return RT_BOOL(rt_is_fix(x)); }
RT_INLINE obj rt_port_p(obj x) { return RT_BOOL(rt_is_port(x)); }
RT_INLINE obj rt_eof_object(void) { return RT_EOF; }
RT_INLINE obj rt_eof_object_p(obj x) { return RT_BOOL(x == RT_EOF); }
RT_INLINE obj rt_stdin_port(void) { return rt_stdin; }
RT_INLINE obj rt_stdout_port(void) { return rt_stdout; }
RT_INLINE obj rt_stderr_port(void) { return rt_stderr; }

#endif /* SHIKRA_RT_H */
