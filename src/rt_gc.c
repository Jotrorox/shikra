/* rt_gc.c -- allocation and collection (DESIGN.md §5).
 * M0 provides only the bump collector (--gc=bump semantics): memory comes from the OS
 * in chunks and is never freed. TODO(M2): the Immix collector goes here, selected by a
 * config macro, behind the same interface (rt_alloc / rt_cons fast paths in rt.h,
 * rt_alloc_slow, roots, immortal mode, rt_gc_collect). */
#ifndef SHIKRA_RT_H
#include "rt.h"
#endif

#define RT_CHUNK ((size_t)1 << 20)
#define RT_MAX_ROOTS 64

RT_VAR char *rt_hp, *rt_hlim, *rt_pp, *rt_plim;
static RT_UNUSED void *rt_gc_stack_base;
static RT_UNUSED int rt_gc_immortal, rt_gc_nroots;
static RT_UNUSED obj *rt_gc_roots[RT_MAX_ROOTS][2];

static void rt_oom(void) {
    static const char msg[] = "Error: out of memory\n";
    rt_sys_write(2, msg, sizeof msg - 1);
    rt_sys_exit(70);
}

RT_FN void *rt_alloc_slow(size_t bytes, int pair_stream) {
    char *p;
    if (bytes > RT_CHUNK / 4) { /* large object: own mapping, keep the current chunk */
        if (!(p = (char *)rt_sys_mem(bytes))) rt_oom();
        return p;
    }
    if (!(p = (char *)rt_sys_mem(RT_CHUNK))) rt_oom();
    if (pair_stream) { rt_pp = p + bytes; rt_plim = p + RT_CHUNK; }
    else { rt_hp = p + bytes; rt_hlim = p + RT_CHUNK; }
    return p;
}

RT_FN void *rt_alloc_raw(size_t nbytes) {
    char *p = (char *)rt_alloc(RT_HDR(RT_T_RAW, 0, nbytes), sizeof(uintptr_t) + nbytes);
    return p + sizeof(uintptr_t);
}

RT_FN void rt_gc_init(void *stack_base) { rt_gc_stack_base = stack_base; }

RT_FN void rt_gc_add_roots(obj *start, obj *end) {
    if (rt_gc_nroots == RT_MAX_ROOTS) rt_oom();
    rt_gc_roots[rt_gc_nroots][0] = start;
    rt_gc_roots[rt_gc_nroots][1] = end;
    rt_gc_nroots++;
}

RT_FN void rt_gc_set_immortal(int on) { rt_gc_immortal = on; }
RT_FN void rt_gc_collect(void) {}
