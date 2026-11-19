/* tests/cases/inc/ffi_user.c: C functions of ext_ffi_user.scm, linked with --link. */
#include <string.h>
static char buf[64];
double user_poly(double x, double a, double b, double c) { return (a * x + b) * x + c; }
long user_sum(int a, long b, char c, _Bool d) { return a + b + c + (d ? 1000 : 0); }
float user_halve(float x) { return x / 2; }
_Bool user_even(long n) { return n % 2 == 0; }
const char *user_greet(const char *who) { strcpy(buf, "hello, "); strncat(buf, who, 40); return buf; }
const char *user_null(void) { return 0; }
char user_next(char c) { return (char)(c + 1); }
void user_store(int x) { buf[0] = (char)x; }
int user_load(void) { return buf[0]; }
double user_many(double a, int b, double c, int d, double e, int f, double g) { return a + b + c + d + e + f + g; }
