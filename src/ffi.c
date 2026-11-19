/* ffi.c -- define-c in the interpreter (spec §10; DESIGN.md "Extensions as built"). Tool code
 * only, never in compiled programs (they call the C function directly, emit.c).
 *
 * A (%c-call '(name "c-name" (arg-type ...) result-type) arg ...) is resolved when its definition
 * is compiled to bytecode: dlsym(RTLD_DEFAULT, "c-name") in the running shikra process (so the C
 * library and libm are available, a user's C file is not). The call goes through one plain C
 * function-pointer type with six integer-class and eight double parameters: on AArch64 (AAPCS64,
 * Apple's variant included) and x86-64 System V, integer and floating-point arguments are assigned
 * to their own register files in order, and unused argument registers are ignored by the callee,
 * so every non-variadic function with at most six int/long/bool/char/string/pointer arguments and
 * at most eight double arguments, in any order, returning one of those types or void, is called
 * correctly. float arguments or results, and other ABIs, are refused at definition time. */
#define _GNU_SOURCE 1 /* RTLD_DEFAULT (glibc) */
#include "interp.h"
#include "ir.h"
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#if (defined(__aarch64__) || defined(__x86_64__)) && !defined(_WIN32)
#define FFI_OK 1
#endif
typedef struct { void *fn; int n, t[16], rt; const char *who; } ffent;
static ffent *tab;
static int ntab, captab;
typedef long (*ffw)(long, long, long, long, long, long, double, double, double, double, double, double, double, double);
typedef double (*ffd)(long, long, long, long, long, long, double, double, double, double, double, double, double, double);

int ffi_resolve(obj desc, obj where) {
    obj l = RT_CAR(RT_CDR(RT_CDR(desc)));
    const char *cname = RT_STR(RT_CAR(RT_CDR(desc)))->buf;
    ffent e;
    int nw = 0, nd = 0;
    memset(&e, 0, sizeof e);
    e.who = RT_STR(RT_PTR(RT_CAR(desc), rt_symbol)->name)->buf;
    e.rt = ir_ffi_type(RT_CAR(RT_CDR(RT_CDR(RT_CDR(desc)))));
    for (; l != RT_NIL; l = RT_CDR(l)) {
        int t = ir_ffi_type(RT_CAR(l));
        e.t[e.n++] = t;
        if (t == FT_DOUBLE) nd++;
        else if (t == FT_FLOAT) nd = 100;
        else nw++;
    }
#ifndef FFI_OK
    nw = 100;
#endif
    if (nw > 6 || nd > 8 || e.rt == FT_FLOAT)
        cerror(where, "define-c: signature of %s not supported in the interpreter (at most 6 integer, character, string or "
                      "pointer arguments and 8 double arguments, no float); use shikra build", e.who);
    if (!(e.fn = dlsym(RTLD_DEFAULT, cname)))
        cerror(where, "define-c: C function %s is not available in the interpreter (only functions of the running process, "
                      "such as the C library's, are); use shikra build", cname);
    if (ntab == captab) {
        captab = captab * 2 + 16;
        tab = (ffent *)xrealloc(tab, sizeof(ffent) * (size_t)captab);
    }
    tab[ntab] = e;
    return ntab++;
}

obj ffi_call(int k, int n, const obj *a) {
    ffent *e = &tab[k];
    long w[6] = {0, 0, 0, 0, 0, 0};
    double d[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    int nw = 0, nd = 0, i;
    long r = 0;
    double rd = 0;
    (void)n;
    for (i = 0; i < e->n; i++) /* in order: the first wrong argument raises */
        switch (e->t[i]) {
        case FT_INT: w[nw++] = rt_ffi_int(a[i], e->who); break;
        case FT_LONG: w[nw++] = rt_ffi_long(a[i], e->who); break;
        case FT_DOUBLE: d[nd++] = rt_ffi_dbl(a[i], e->who); break;
        case FT_BOOL: w[nw++] = rt_ffi_bool(a[i], e->who); break;
        case FT_CHAR: w[nw++] = rt_ffi_char(a[i], e->who); break;
        case FT_STRING: w[nw++] = (long)(intptr_t)rt_ffi_str(a[i], e->who); break;
        default: w[nw++] = (long)(intptr_t)rt_ffi_ptr(a[i], e->who); break;
        }
    if (e->rt == FT_DOUBLE) rd = ((ffd)e->fn)(w[0], w[1], w[2], w[3], w[4], w[5], d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
    else r = ((ffw)e->fn)(w[0], w[1], w[2], w[3], w[4], w[5], d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
    switch (e->rt) {
    case FT_INT: return RT_FIX((int)r);
    case FT_LONG: return rt_ffi_rlong(r, e->who);
    case FT_DOUBLE: return rt_make_flonum(rd);
    case FT_BOOL: return RT_BOOL((r & 0xFF) != 0);
    case FT_CHAR: return RT_CHAR((unsigned char)r);
    case FT_STRING: return rt_ffi_rstr((const char *)(intptr_t)r);
    case FT_POINTER: return rt_ffi_rptr((void *)(intptr_t)r, e->who);
    default: return RT_UNSPEC;
    }
}
