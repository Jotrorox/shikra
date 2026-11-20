/* ffi.c: C functions for ffi.scm, linked with `--link ffi.c`. */
#include <math.h>
#include <string.h>

/* Population standard deviation of n values, computed in C. Shikra passes numbers, strings
   and pointers; arrays go through a buffer the C side owns. */
static double values[1024];
static int count;

void stats_reset(void) { count = 0; }
int stats_add(double x) {
    if (count == 1024) return 0;
    values[count++] = x;
    return 1;
}
double stats_stddev(void) {
    double mean = 0, var = 0;
    if (count == 0) return 0;
    for (int i = 0; i < count; i++) mean += values[i];
    mean /= count;
    for (int i = 0; i < count; i++) var += (values[i] - mean) * (values[i] - mean);
    return sqrt(var / count);
}

/* A string result: Shikra copies it into a Scheme string, so a static buffer is fine. */
const char *shout(const char *s) {
    static char buf[256];
    size_t i = 0;
    for (; s[i] && i < sizeof buf - 2; i++) buf[i] = (s[i] >= 'a' && s[i] <= 'z') ? s[i] - 32 : s[i];
    buf[i++] = '!';
    buf[i] = 0;
    return buf;
}
