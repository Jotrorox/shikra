/* rt_sys.c -- platform layer. The ONLY runtime file that calls libc or the OS
 * (DESIGN.md §3). Everything else goes through the rt_sys_* / rt_mem* functions. */
#ifndef SHIKRA_RT_H
#include "rt.h"
#endif

#if defined(SHIKRA_NOLIBC)
/* ==== Linux, no libc (M6; DESIGN.md "M6 Linux as built") ===========================
 * Raw system calls, an own _start and the mem* symbols the C compiler may call; the driver
 * links with -static -nostdlib -ffreestanding. The inline assembly (the system-call
 * instruction, _start, setjmp/longjmp for clang on arm64) is the only non-C99 code of the
 * runtime (DESIGN.md Deviations). Math comes from rt_math.c. */
#if !defined(__linux__) || !(defined(__x86_64__) || defined(__aarch64__)) || !(defined(__GNUC__) || defined(__clang__))
#error "SHIKRA_NOLIBC needs Linux on x86-64 or arm64 and a GCC-compatible compiler"
#endif
#if defined(__x86_64__)
enum { SYS_read = 0, SYS_write = 1, SYS_close = 3, SYS_mmap = 9, SYS_munmap = 11, SYS_clock_gettime = 228,
       SYS_exit_group = 231, SYS_openat = 257, SYS_unlinkat = 263, SYS_faccessat = 269, SYS_ppoll = 271 };
static long sys_call(long n, long a, long b, long c, long d, long e, long f) {
    long r;
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return r;
}
#else
enum { SYS_unlinkat = 35, SYS_faccessat = 48, SYS_openat = 56, SYS_close = 57, SYS_read = 63, SYS_write = 64,
       SYS_ppoll = 73, SYS_exit_group = 94, SYS_clock_gettime = 113, SYS_munmap = 215, SYS_mmap = 222 };
static long sys_call(long n, long a, long b, long c, long d, long e, long f) {
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    register long x4 __asm__("x4") = e;
    register long x5 __asm__("x5") = f;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory");
    return x0;
}
#endif
#define SYS3(n, a, b, c) sys_call(SYS_##n, (long)(a), (long)(b), (long)(c), 0, 0, 0)
#define SYS_AT_FDCWD (-100)
#define SYS_EINTR 4
static char **sys_envp;

RT_FN intptr_t rt_sys_write(int fd, const void *buf, size_t n) {
    size_t done = 0;
    while (done < n) {
        long r = SYS3(write, fd, (const char *)buf + done, n - done);
        if (r == -SYS_EINTR) continue;
        if (r < 0) return done ? (intptr_t)done : -1;
        done += (size_t)r;
    }
    return (intptr_t)done;
}
RT_FN intptr_t rt_sys_read(int fd, void *buf, size_t n) {
    long r;
    do r = SYS3(read, fd, buf, n); while (r == -SYS_EINTR);
    return r < 0 ? -1 : r;
}
RT_FN int rt_sys_open(const char *path, int mode) { /* O_WRONLY 01, O_CREAT 0100, O_TRUNC 01000, O_APPEND 02000 */
    long r = sys_call(SYS_openat, SYS_AT_FDCWD, (long)path, mode == RT_O_READ ? 0 : mode == RT_O_APPEND ? 02101 : 01101,
                      0666, 0, 0);
    return r < 0 ? -1 : (int)r;
}
RT_FN int rt_sys_close(int fd) { return SYS3(close, fd, 0, 0) < 0 ? -1 : 0; }
RT_FN int rt_sys_ready(int fd) {
    struct { int fd; short events, revents; } p;
    long ts[2] = {0, 0};
    p.fd = fd, p.events = 1 /* POLLIN */, p.revents = 0;
    return sys_call(SYS_ppoll, (long)&p, 1, (long)ts, 0, 8, 0) != 0;
}
RT_FN int rt_sys_exists(const char *path) { return sys_call(SYS_faccessat, SYS_AT_FDCWD, (long)path, 0, 0, 0, 0) == 0; }
RT_FN int rt_sys_remove(const char *path) { return SYS3(unlinkat, SYS_AT_FDCWD, path, 0) < 0 ? -1 : 0; }
RT_FN char **rt_sys_environ(void) { return sys_envp; }
RT_FN void rt_sys_exit(int code) {
    for (;;) SYS3(exit_group, code, 0, 0);
}
RT_FN void *rt_sys_mem(size_t n) { /* PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS */
    long r = sys_call(SYS_mmap, 0, (long)n, 3, 0x22, -1, 0);
    return r < 0 && r > -4096 ? 0 : (void *)r;
}
RT_FN void rt_sys_free(void *p, size_t n) { SYS3(munmap, p, n, 0); }
RT_FN const char *rt_sys_getenv(const char *name) {
    char **e;
    size_t i;
    for (e = sys_envp; e && *e; e++) {
        for (i = 0; name[i] && (*e)[i] == name[i]; i++) {}
        if (!name[i] && (*e)[i] == '=') return *e + i + 1;
    }
    return 0;
}
RT_FN int64_t rt_sys_clock_ns(int monotonic) {
    long ts[2] = {0, 0};
    SYS3(clock_gettime, monotonic ? 1 : 0, ts, 0); /* CLOCK_MONOTONIC 1, CLOCK_REALTIME 0 */
    return (int64_t)ts[0] * 1000000000 + ts[1];
}
RT_FN void rt_memcpy(void *d, const void *s, size_t n) { __builtin_memcpy(d, s, n); }
RT_FN void rt_memmove(void *d, const void *s, size_t n) { __builtin_memmove(d, s, n); }
RT_FN void rt_memset(void *d, int c, size_t n) { __builtin_memset(d, c, n); }
RT_FN int rt_memcmp(const void *a, const void *b, size_t n) { return __builtin_memcmp(a, b, n); }
RT_FN size_t rt_strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
/* The symbols themselves, for the calls the compiler emits (struct copies, the builtins
 * above). GCC must not turn these loops back into calls to themselves. */
#if defined(__GNUC__) && !defined(__clang__)
#define SYS_NOLOOPCALL __attribute__((optimize("no-tree-loop-distribute-patterns")))
#else
#define SYS_NOLOOPCALL
#endif
SYS_NOLOOPCALL void *memcpy(void *restrict d, const void *restrict s, size_t n) {
    unsigned char *p = (unsigned char *)d;
    const unsigned char *q = (const unsigned char *)s;
    while (n--) *p++ = *q++;
    return d;
}
SYS_NOLOOPCALL void *memmove(void *d, const void *s, size_t n) {
    unsigned char *p = (unsigned char *)d;
    const unsigned char *q = (const unsigned char *)s;
    if ((uintptr_t)p <= (uintptr_t)q) while (n--) *p++ = *q++;
    else while (n--) p[n] = q[n];
    return d;
}
SYS_NOLOOPCALL void *memset(void *d, int c, size_t n) {
    unsigned char *p = (unsigned char *)d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}
SYS_NOLOOPCALL int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *p = (const unsigned char *)a, *q = (const unsigned char *)b;
    for (; n; n--, p++, q++)
        if (*p != *q) return *p - *q;
    return 0;
}

/* Entry: the kernel leaves argc, argv[], NULL, envp[], NULL at the stack pointer; that address
 * is also the stack base for the collector's conservative scan. */
#ifdef SHIKRA_PROGRAM
__attribute__((used, noreturn)) void shikra_start(uintptr_t *sp);
__attribute__((used, noreturn)) void shikra_start(uintptr_t *sp) {
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    sys_envp = argv + argc + 1;
    rt_init(argc, argv, (void *)sp);
    rt_program_main();
    rt_exit(0);
}
#if defined(__x86_64__)
__asm__(".text\n.globl _start\n.type _start,@function\n_start:\n"
        "\txor %ebp, %ebp\n\tmov %rsp, %rdi\n\tand $-16, %rsp\n\tcall shikra_start\n\thlt\n");
#else
__asm__(".text\n.globl _start\n.type _start,%function\n_start:\n"
        "\tmov x29, #0\n\tmov x30, #0\n\tmov x0, sp\n\tbl shikra_start\n\tbrk #0\n");
#endif
#endif
#if !defined(RT_BUILTIN_SETJMP) /* clang on arm64 has no __builtin_setjmp: callee-saved x19-x30, sp, d8-d15 */
__attribute__((returns_twice)) int shikra_setjmp(rt_jmpbuf *b);
__attribute__((noreturn)) void shikra_longjmp(rt_jmpbuf *b);
__asm__(".text\n.globl shikra_setjmp\n.type shikra_setjmp,%function\nshikra_setjmp:\n"
        "\tstp x19, x20, [x0]\n\tstp x21, x22, [x0, #16]\n\tstp x23, x24, [x0, #32]\n\tstp x25, x26, [x0, #48]\n"
        "\tstp x27, x28, [x0, #64]\n\tstp x29, x30, [x0, #80]\n\tmov x2, sp\n\tstr x2, [x0, #96]\n"
        "\tstp d8, d9, [x0, #112]\n\tstp d10, d11, [x0, #128]\n\tstp d12, d13, [x0, #144]\n\tstp d14, d15, [x0, #160]\n"
        "\tmov w0, #0\n\tret\n"
        ".globl shikra_longjmp\n.type shikra_longjmp,%function\nshikra_longjmp:\n"
        "\tldp x19, x20, [x0]\n\tldp x21, x22, [x0, #16]\n\tldp x23, x24, [x0, #32]\n\tldp x25, x26, [x0, #48]\n"
        "\tldp x27, x28, [x0, #64]\n\tldp x29, x30, [x0, #80]\n\tldr x2, [x0, #96]\n\tmov sp, x2\n"
        "\tldp d8, d9, [x0, #112]\n\tldp d10, d11, [x0, #128]\n\tldp d12, d13, [x0, #144]\n\tldp d14, d15, [x0, #160]\n"
        "\tmov w0, #1\n\tret\n");
RT_FN int rt_sys_catch(rt_jmpbuf *b, void (*fn)(void *), void *arg) {
    if (shikra_setjmp(b)) return 1;
    fn(arg);
    return 0;
}
RT_FN void rt_sys_throw(rt_jmpbuf *b) { shikra_longjmp(b); }
#endif
#else
/* ==== POSIX via libc / libSystem (macOS, and Linux with libc) ===================== */
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <poll.h>
#ifdef RT_SYS_LIBM
#include <math.h>
#endif
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

RT_FN intptr_t rt_sys_write(int fd, const void *buf, size_t n) {
    size_t done = 0;
    while (done < n) {
        ssize_t r = write(fd, (const char *)buf + done, n - done);
        if (r < 0) return done ? (intptr_t)done : -1;
        done += (size_t)r;
    }
    return (intptr_t)done;
}
RT_FN intptr_t rt_sys_read(int fd, void *buf, size_t n) { return (intptr_t)read(fd, buf, n); }
RT_FN int rt_sys_open(const char *path, int mode) {
    if (mode == RT_O_READ) return open(path, O_RDONLY);
    return open(path, O_WRONLY | O_CREAT | (mode == RT_O_APPEND ? O_APPEND : O_TRUNC), 0666);
}
RT_FN int rt_sys_close(int fd) { return close(fd); }
RT_FN int rt_sys_ready(int fd) {
    struct pollfd p;
    p.fd = fd;
    p.events = POLLIN;
    p.revents = 0;
    return poll(&p, 1, 0) != 0; /* readable, at end of file, or in error: a read will not block */
}
/* Math: (scheme base) expt on flonums, (scheme inexact). One function per libm function, so a
 * program links only those it uses; without libm (RT_SYS_LIBM unset: everything but macOS)
 * rt_math.c defines exactly these. atan of one argument is atan2(x, 1). */
#ifdef RT_SYS_LIBM
RT_FN double rt_sys_pow(double x, double y) { return pow(x, y); }
RT_FN double rt_sys_exp(double x) { return exp(x); }
RT_FN double rt_sys_log(double x) { return log(x); }
RT_FN double rt_sys_sin(double x) { return sin(x); }
RT_FN double rt_sys_cos(double x) { return cos(x); }
RT_FN double rt_sys_tan(double x) { return tan(x); }
RT_FN double rt_sys_asin(double x) { return asin(x); }
RT_FN double rt_sys_acos(double x) { return acos(x); }
RT_FN double rt_sys_atan2(double y, double x) { return atan2(y, x); }
RT_FN double rt_sys_sqrt(double x) { return sqrt(x); }
#endif
/* Files and the environment ((scheme file), (scheme process-context)). */
RT_FN int rt_sys_exists(const char *path) { struct stat st; return stat(path, &st) == 0; }
RT_FN int rt_sys_remove(const char *path) { return unlink(path); }
extern char **environ;
RT_FN char **rt_sys_environ(void) { return environ; }
RT_FN void rt_sys_exit(int code) { exit(code); }
RT_FN void *rt_sys_mem(size_t n) {
    void *p = mmap(0, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? 0 : p;
}
RT_FN void rt_sys_free(void *p, size_t n) { munmap(p, n); }
RT_FN const char *rt_sys_getenv(const char *name) { return getenv(name); }
RT_FN int64_t rt_sys_clock_ns(int monotonic) {
    struct timespec ts;
    clock_gettime(monotonic ? CLOCK_MONOTONIC : CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}
RT_FN void rt_memcpy(void *d, const void *s, size_t n) { memcpy(d, s, n); }
RT_FN void rt_memmove(void *d, const void *s, size_t n) { memmove(d, s, n); }
RT_FN void rt_memset(void *d, int c, size_t n) { memset(d, c, n); }
RT_FN int rt_memcmp(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }
RT_FN size_t rt_strlen(const char *s) { return strlen(s); }

#ifdef SHIKRA_PROGRAM
int main(int argc, char **argv) {
    rt_init(argc, argv, (void *)&argc);
    rt_program_main();
    rt_exit(0);
}
#endif
#endif

/* Non-local exits. The setjmp lives in rt_sys_catch, whose frame stays active while fn
 * runs; returning 1 after a throw restores the caller's callee-saved registers. */
#if defined(RT_BUILTIN_SETJMP)
RT_FN int rt_sys_catch(rt_jmpbuf *b, void (*fn)(void *), void *arg) {
    if (__builtin_setjmp(b->w)) return 1;
    fn(arg);
    return 0;
}
RT_FN void rt_sys_throw(rt_jmpbuf *b) { __builtin_longjmp(b->w, 1); }
#elif !defined(SHIKRA_NOLIBC)
#include <setjmp.h>
typedef char sys_jmpbuf_fits[sizeof(sigjmp_buf) <= sizeof(rt_jmpbuf) ? 1 : -1];
RT_FN int rt_sys_catch(rt_jmpbuf *b, void (*fn)(void *), void *arg) {
    if (sigsetjmp(*(sigjmp_buf *)(void *)b, 0)) return 1; /* 0: no signal-mask system call */
    fn(arg);
    return 0;
}
RT_FN void rt_sys_throw(rt_jmpbuf *b) { siglongjmp(*(sigjmp_buf *)(void *)b, 1); }
#endif
