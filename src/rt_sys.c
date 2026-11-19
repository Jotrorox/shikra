/* rt_sys.c -- platform layer. The ONLY runtime file that calls libc or the OS
 * (DESIGN.md §3). Everything else goes through the rt_sys_* / rt_mem* functions. */
#ifndef SHIKRA_RT_H
#include "rt.h"
#endif

#if defined(SHIKRA_NOLIBC)
/* ==== Linux, no libc (M6) ==========================================================
 * TODO(M6): raw system calls (write/read/openat/close/exit_group/mmap/clock_gettime)
 * for x86-64 and arm64, plus an own _start that captures argc/argv and the stack base,
 * calls rt_init and rt_program_main. Only this branch changes; the API stays. */
#error "SHIKRA_NOLIBC (static Linux without libc) is not implemented yet (M6)"
#else
/* ==== POSIX via libc / libSystem (macOS, and Linux with libc) ===================== */
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
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
RT_FN void rt_sys_exit(int code) { exit(code); }
RT_FN void *rt_sys_mem(size_t n) {
    void *p = mmap(0, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? 0 : p;
}
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
