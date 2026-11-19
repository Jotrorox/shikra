/* rt_unit.c -- C-level tests of runtime primitives in tool mode.
 * Built by tests/run.sh: cc -std=c99 -Isrc tests/unit/rt_unit.c src/rt_*.c */
#include "rt.h"
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

static int fails, checks;
static jmp_buf catch_buf;
static obj caught;
static void hook(obj e) { caught = e; longjmp(catch_buf, 1); }

#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
/* Evaluate expression e expecting a raised error whose message contains msg. */
#define RAISES(e, msg) do { \
    caught = 0; \
    if (!setjmp(catch_buf)) { (void)(e); } \
    checks++; \
    if (!caught || !strstr(RT_STR(RT_VEC(caught)[0])->buf, msg)) { fails++; printf("FAIL %s:%d: %s does not raise '%s'\n", __FILE__, __LINE__, #e, msg); } \
} while (0)

static obj S(const char *s) { return rt_cstring(s); }
static obj rd(const char *s) { return rt_read(rt_open_input_string(S(s))); }
static int wr_is(obj x, const char *expect) {
    obj p = rt_open_output_string();
    rt_write(x, p);
    return strcmp(RT_STR(rt_get_output_string(p))->buf, expect) == 0;
}
static int num_is(obj x, const char *expect) { return strcmp(RT_STR(rt_number_to_string(x, RT_FIX(10)))->buf, expect) == 0; }

int main(int argc, char **argv) {
    obj x, y, port, file;
    intptr_t line, col;
    (void)argc;
    rt_init(argc, argv, (void *)&argc);
    rt_raise_hook = hook;

    /* values and fixnum arithmetic */
    CHECK(RT_FIXVAL(RT_FIX(-5)) == -5);
    CHECK(rt_add(RT_FIX(2), RT_FIX(3)) == RT_FIX(5));
    CHECK(rt_mul(RT_FIX(-4), RT_FIX(5)) == RT_FIX(-20));
    RAISES(rt_add(RT_FIX(RT_FIX_MAX), RT_FIX(1)), "fixnum overflow");
    RAISES(rt_mul(RT_FIX(RT_FIX_MAX / 2 + 1), RT_FIX(2)), "fixnum overflow");
    RAISES(rt_sub(RT_FIX(RT_FIX_MIN), RT_FIX(1)), "fixnum overflow");
    RAISES(rt_quotient(RT_FIX(RT_FIX_MIN), RT_FIX(-1)), "fixnum overflow");
    CHECK(rt_div(RT_FIX(6), RT_FIX(3)) == RT_FIX(2));
    CHECK(num_is(rt_div(RT_FIX(1), RT_FIX(4)), "0.25"));
    CHECK(num_is(rt_div(RT_FIX(1), RT_FIX(3)), "0.3333333333333333"));
    RAISES(rt_div(RT_FIX(1), RT_FIX(0)), "division by zero");
    CHECK(rt_modulo(RT_FIX(-7), RT_FIX(2)) == RT_FIX(1));
    CHECK(rt_remainder(RT_FIX(-7), RT_FIX(2)) == RT_FIX(-1));
    CHECK(rt_quotient(RT_FIX(-7), RT_FIX(2)) == RT_FIX(-3));
    CHECK(rt_floor_quotient(RT_FIX(-7), RT_FIX(2)) == RT_FIX(-4));
    CHECK(num_is(rt_modulo(rt_make_flonum(-7.0), RT_FIX(2)), "1.0"));
    RAISES(rt_modulo(rt_make_flonum(1.5), RT_FIX(2)), "expected integer");
    CHECK(rt_lt(RT_FIX(1), rt_make_flonum(1.5)) == RT_TRUE);
    CHECK(rt_num_eq(RT_FIX(2), rt_make_flonum(2.0)) == RT_TRUE);
    CHECK(rt_num_eq(RT_FIX(RT_FIX_MAX), rt_make_flonum(4611686018427387904.0)) == RT_FALSE);
    CHECK(rt_lt(rd("+nan.0"), RT_FIX(1)) == RT_FALSE && rt_num_ge(rd("+nan.0"), RT_FIX(1)) == RT_FALSE);
    CHECK(rt_nan_p(rd("+nan.0")) == RT_TRUE && rt_infinite_p(rd("-inf.0")) == RT_TRUE);
    RAISES(rt_lt(RT_FIX(1), S("a")), "<: expected number");
    CHECK(rt_exact(rt_make_flonum(3.0)) == RT_FIX(3));
    RAISES(rt_exact(rt_make_flonum(2.5)), "no exact representation");
    CHECK(num_is(rt_round(rt_make_flonum(2.5)), "2.0"));
    CHECK(num_is(rt_round(rt_make_flonum(3.5)), "4.0"));
    CHECK(num_is(rt_round(rt_make_flonum(-2.5)), "-2.0"));
    CHECK(num_is(rt_floor(rt_make_flonum(-2.5)), "-3.0"));
    CHECK(num_is(rt_ceiling(rt_make_flonum(-2.5)), "-2.0"));
    CHECK(num_is(rt_truncate(rt_make_flonum(-2.7)), "-2.0"));
    CHECK(rt_integer_p(rt_make_flonum(2.0)) == RT_TRUE && rt_integer_p(rt_make_flonum(2.1)) == RT_FALSE);
    CHECK(strcmp(RT_STR(rt_number_to_string(RT_FIX(-255), RT_FIX(16)))->buf, "-ff") == 0);
    CHECK(strcmp(RT_STR(rt_number_to_string(RT_FIX(5), RT_FIX(2)))->buf, "101") == 0);
    CHECK(rt_string_to_number(S("ff"), RT_FIX(16)) == RT_FIX(255));
    CHECK(rt_string_to_number(S("abc"), RT_FIX(10)) == RT_FALSE);
    CHECK(rt_string_to_number(S("-4611686018427387904"), RT_FIX(10)) == RT_FIX(RT_FIX_MIN));
    CHECK(rt_string_to_number(S("4611686018427387904"), RT_FIX(10)) == RT_FALSE);
    {   /* float printing round-trips through the parser for a spread of values */
        static const double v[] = {0.1, 0.2, 0.3, 1.0 / 3, 2.0 / 3, 123.456, 1e22, 1e23, 1.7976931348623157e308,
                                   2.2250738585072014e-308, 6.02214076e23, 3.141592653589793, 100.0, 1e-5};
        size_t i;
        for (i = 0; i < sizeof v / sizeof v[0]; i++) {
            obj s = rt_number_to_string(rt_make_flonum(v[i]), RT_FIX(10)), back = rt_string_to_number(s, RT_FIX(10));
            checks++;
            if (!rt_is_flo(back) || RT_FLO(back) != v[i]) { fails++; printf("FAIL float round trip %.17g -> %s\n", v[i], RT_STR(s)->buf); }
        }
        CHECK(num_is(rt_make_flonum(0.1), "0.1"));
        CHECK(num_is(rt_make_flonum(1e22), "1e22"));
        CHECK(num_is(rt_make_flonum(123.456), "123.456"));
    }

    /* strings: UTF-8, ASCII fast path, mutation with width change */
    x = S("h\xc3\xa9llo");                    /* "héllo" */
    CHECK(rt_string_length(x) == RT_FIX(5) && RT_STR(x)->bytes == 6 && !(RT_HFLAGS(x) & RT_F_ASCII));
    CHECK(rt_string_ref(x, RT_FIX(1)) == RT_CHAR(0xE9));
    CHECK(rt_string_ref(x, RT_FIX(4)) == RT_CHAR('o'));
    y = S("abc");
    CHECK(RT_HFLAGS(y) & RT_F_ASCII);
    rt_string_set(y, RT_FIX(1), RT_CHAR(0x3BB));  /* λ */
    CHECK(RT_STR(y)->bytes == 4 && wr_is(y, "\"a\xce\xbb" "c\"") && !(RT_HFLAGS(y) & RT_F_ASCII));
    rt_string_set(y, RT_FIX(1), RT_CHAR('b'));
    CHECK(RT_STR(y)->bytes == 3 && (RT_HFLAGS(y) & RT_F_ASCII) && wr_is(y, "\"abc\""));
    RAISES(rt_string_ref(y, RT_FIX(3)), "string-ref: index out of range");
    RAISES(rt_string_set(rt_symbol_to_string(rt_intern("k", 1)), RT_FIX(0), RT_CHAR('x')), "mutable string");
    CHECK(wr_is(rt_substring(x, RT_FIX(1), RT_FIX(3)), "\"\xc3\xa9l\""));
    CHECK(wr_is(rt_string_append(x, y), "\"h\xc3\xa9lloabc\""));
    CHECK(rt_string_compare(S("abc"), S("abd")) == RT_FIX(-1));
    CHECK(rt_string_compare(S("ab"), S("ab")) == RT_FIX(0));
    CHECK(rt_string_compare(S("b"), S("ab")) == RT_FIX(1));
    CHECK(wr_is(rt_string_to_list(x, RT_FIX(0), RT_FIX(2)), "(#\\h #\\\xc3\xa9)"));
    CHECK(rt_equal(rt_list_to_string(rt_string_to_list(x, RT_FIX(0), RT_FIX(5))), x));
    CHECK(wr_is(rt_make_string(RT_FIX(3), RT_CHAR(0x3BB)), "\"\xce\xbb\xce\xbb\xce\xbb\""));
    CHECK(rt_equal(rt_utf8_to_string(rt_string_to_utf8(x)), x));
    CHECK(wr_is(S("a\x01" "b"), "\"a\\x1;b\""));
    RAISES(rt_integer_to_char(RT_FIX(0xD800)), "Unicode scalar value");

    /* symbols */
    CHECK(rt_intern("foo", 3) == rt_string_to_symbol(S("foo")));
    CHECK(rt_make_symbol(S("foo")) != rt_intern("foo", 3));
    {
        int i;
        char b[16];
        for (i = 0; i < 2000; i++) { sprintf(b, "s%d", i); rt_intern(b, strlen(b)); }
        CHECK(rt_intern("s1234", 5) == rt_string_to_symbol(S("s1234")));
    }
    CHECK(wr_is(rt_intern("", 0), "||") && wr_is(rt_intern("1", 1), "|1|") && wr_is(rt_intern("a|b", 3), "|a\\|b|"));
    CHECK(wr_is(rt_intern("+", 1), "+") && wr_is(rt_intern("...", 3), "...") && wr_is(rt_intern(".", 1), "|.|"));

    /* lists, vectors, bytevectors, equality */
    x = rd("(1 2 3)");
    CHECK(rt_length(x) == RT_FIX(3) && rt_list_p(x) == RT_TRUE);
    RAISES(rt_length(rd("(1 . 2)")), "length: expected proper list");
    RAISES(rt_car(RT_FIX(5)), "car: expected pair");
    CHECK(wr_is(rt_reverse(x), "(3 2 1)"));
    CHECK(rt_equal(rd("(a #(1 \"x\" 2.0) #u8(1 2))"), rd("(a #(1 \"x\" 2.0) #u8(1 2))")));
    CHECK(!rt_equal(rd("(a #(1 2))"), rd("(a #(1 3))")));
    CHECK(rt_eqv(rt_make_flonum(1.5), rt_make_flonum(1.5)) && !rt_eqv(rt_make_flonum(0.0), rt_make_flonum(-0.0)));
    CHECK(!rt_eqv(S("a"), S("a")));
    x = rt_make_vector(RT_FIX(3), RT_FIX(0));
    rt_vector_set(x, RT_FIX(2), RT_TRUE);
    CHECK(wr_is(x, "#(0 0 #t)") && wr_is(rt_vector_to_list(x, RT_FIX(1), RT_FIX(3)), "(0 #t)"));
    RAISES(rt_vector_ref(x, RT_FIX(3)), "vector-ref: index out of range");
    RAISES(rt_vector_ref(x, RT_FIX(-1)), "vector-ref: index out of range");
    x = rt_make_bytevector(RT_FIX(2), RT_FIX(7));
    rt_bytevector_u8_set(x, RT_FIX(0), RT_FIX(255));
    CHECK(wr_is(x, "#u8(255 7)") && wr_is(rt_bytevector_copy(x, RT_FIX(1), RT_FIX(2)), "#u8(7)"));
    RAISES(rt_bytevector_u8_set(x, RT_FIX(0), RT_FIX(256)), "expected byte");

    /* boxes, records, error objects, values */
    x = rt_make_box(RT_FIX(1));
    rt_box_set(x, RT_FIX(2));
    CHECK(rt_box_ref(x) == RT_FIX(2));
    y = rt_make_rtd(rt_intern("point", 5), rd("#(x y)"));
    x = rt_make_record(y);
    rt_record_set(x, y, RT_FIX(1), RT_FIX(9));
    CHECK(rt_record_ref(x, y, RT_FIX(1)) == RT_FIX(9) && rt_record_p(x, y) == RT_TRUE && wr_is(x, "#<point>"));
    RAISES(rt_record_ref(RT_FIX(1), y, RT_FIX(0)), "expected record of type point");
    x = rt_make_error_object(RT_FIX(RT_ERR_FILE), S("m"), RT_NIL);
    CHECK(rt_file_error_p(x) == RT_TRUE && rt_read_error_p(x) == RT_FALSE && rt_error_object_p(x) == RT_TRUE);
    RAISES(rt_open_input_file(S("/nonexistent/x")), "open-input-file: cannot open file");
    CHECK(caught && rt_file_error_p(caught) == RT_TRUE);
    x = rt_make_values(rd("(1 2)"));
    CHECK(rt_values_p(x) == RT_TRUE && wr_is(rt_values_to_list(x), "(1 2)"));

    /* ports */
    port = rt_open_input_string(S("ab\xce\xbb\nline2\r\n"));
    CHECK(rt_peek_char(port) == RT_CHAR('a') && rt_read_char(port) == RT_CHAR('a'));
    CHECK(wr_is(rt_read_line(port), "\"b\xce\xbb\""));
    CHECK(wr_is(rt_read_line(port), "\"line2\""));
    CHECK(rt_read_line(port) == RT_EOF && rt_read_char(port) == RT_EOF);
    port = rt_open_output_string();
    rt_write_string(S("x="), port);
    rt_display(rd("(\"s\" #\\c 1.5)"), port);
    rt_write_char(RT_CHAR(0x3BB), port);
    CHECK(strcmp(RT_STR(rt_get_output_string(port))->buf, "x=(s c 1.5)\xce\xbb") == 0);
    CHECK(wr_is(rd("(#\\x7 #\\x0 #\\delete #\\x1f)"), "(#\\alarm #\\null #\\delete #\\x1f)"));
    RAISES(rt_write(RT_FIX(1), rt_stdin), "write: expected textual output port");

    /* reader: errors, labels, locations */
    RAISES(rd("(1 2"), "unterminated list");
    CHECK(caught && rt_read_error_p(caught) == RT_TRUE);
    x = rd("#0=(a . #0#)");
    CHECK(rt_is_pair(x) && RT_CDR(x) == x);
    x = rd("#1=#(1 #1#)");
    CHECK(rt_is_vector(x) && RT_VEC(x)[1] == x);
    x = rd("(#5=(p) #5#)");
    CHECK(RT_CAR(x) == RT_CAR(RT_CDR(x)));
    rt_srcloc_enable(1);
    {
        FILE *f = fopen("tests/tmp/loc.scm", "w");
        fputs("; header\n(define (f x)\n  (+ x 1))\n", f);
        fclose(f);
    }
    port = rt_open_input_file(S("tests/tmp/loc.scm"));
    x = rt_read(port);
    CHECK(rt_srcloc_lookup(x, &file, &line, &col) && line == 2 && col == 1 && wr_is(file, "\"tests/tmp/loc.scm\""));
    y = RT_CAR(RT_CDR(RT_CDR(x)));          /* (+ x 1) */
    CHECK(rt_srcloc_lookup(y, &file, &line, &col) && line == 3 && col == 3);
    y = RT_CDR(RT_CDR(y));                  /* (1) */
    CHECK(rt_srcloc_lookup(y, &file, &line, &col) && line == 3 && col == 8);
    CHECK(rt_read(port) == RT_EOF);

    /* open file output ports are flushed by rt_exit (run.sh checks the file afterwards);
     * closing one takes it off the list */
    port = rt_open_output_file(S("tests/tmp/closed.txt"));
    CHECK(rt_is_pair(rt_ports) && RT_CAR(rt_ports) == port);
    rt_close_port(port);
    CHECK(rt_ports == RT_NIL);
    port = rt_open_output_file(S("tests/tmp/flush.txt"));
    rt_write_string(S("flushed at exit\n"), port);
    CHECK(rt_ports != RT_NIL);

    printf("rt_unit: %d checks, %d failures\n", checks, fails);
    rt_exit(fails != 0);
}
