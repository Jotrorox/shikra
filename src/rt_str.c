/* rt_str.c -- strings (UTF-8 with an ASCII fast path, spec §3.2/§6.1) and chars.
 * A string is [hdr][chars][bytes][buf]; buf is a NUL-terminated UTF-8 buffer that may be
 * replaced by string-set! when a character's encoded width changes. RT_F_ASCII is kept
 * equal to (chars == bytes); on ASCII strings string-ref/set! are O(1). */
#ifndef SHIKRA_RT_H
#include "rt.h"
#endif

RT_FN int rt_utf8_encode(char *o, int c) {
    if (c < 0x80) { o[0] = (char)c; return 1; }
    if (c < 0x800) { o[0] = (char)(0xC0 | (c >> 6)); o[1] = (char)(0x80 | (c & 63)); return 2; }
    if (c < 0x10000) {
        o[0] = (char)(0xE0 | (c >> 12)); o[1] = (char)(0x80 | ((c >> 6) & 63)); o[2] = (char)(0x80 | (c & 63));
        return 3;
    }
    o[0] = (char)(0xF0 | (c >> 18)); o[1] = (char)(0x80 | ((c >> 12) & 63));
    o[2] = (char)(0x80 | ((c >> 6) & 63)); o[3] = (char)(0x80 | (c & 63));
    return 4;
}

/* Decodes one code point from s[0..n); malformed input yields U+FFFD and consumes 1 byte. */
RT_FN int rt_utf8_decode(const char *s, size_t n, size_t *len) {
    const unsigned char *u = (const unsigned char *)s;
    int c = u[0], need, i, min;
    *len = 1;
    if (c < 0x80) return c;
    if (c >= 0xC2 && c < 0xE0) { need = 1; c &= 0x1F; min = 0x80; }
    else if (c >= 0xE0 && c < 0xF0) { need = 2; c &= 0x0F; min = 0x800; }
    else if (c >= 0xF0 && c < 0xF5) { need = 3; c &= 0x07; min = 0x10000; }
    else return 0xFFFD;
    if ((size_t)need >= n) return 0xFFFD;
    for (i = 1; i <= need; i++) {
        if ((u[i] & 0xC0) != 0x80) return 0xFFFD;
        c = (c << 6) | (u[i] & 63);
    }
    if (c < min || c > 0x10FFFF || (c >= 0xD800 && c < 0xE000)) return 0xFFFD;
    *len = (size_t)need + 1;
    return c;
}

RT_FN size_t rt_utf8_count(const char *s, size_t n) {
    size_t k = 0, i = 0, l;
    while (i < n) {
        if ((unsigned char)s[i] < 0x80) i++;
        else { rt_utf8_decode(s + i, n - i, &l); i += l; }
        k++;
    }
    return k;
}

/* Fresh string with an uninitialised buffer of nbytes (+ NUL). */
static obj str_alloc(intptr_t chars, intptr_t nbytes) {
    char *buf = (char *)rt_alloc_raw((size_t)nbytes + 1);
    rt_string *s = (rt_string *)rt_alloc(RT_HDR(RT_T_STRING, chars == nbytes ? RT_F_ASCII : 0, 0), sizeof(rt_string));
    s->chars = chars;
    s->bytes = nbytes;
    s->buf = buf;
    buf[nbytes] = 0;
    return RT_TAGPTR(s);
}

RT_FN obj rt_make_string_n(const char *s, size_t n) {
    obj r = str_alloc((intptr_t)rt_utf8_count(s, n), (intptr_t)n);
    rt_memcpy(RT_STR(r)->buf, s, n);
    return r;
}
RT_FN obj rt_cstring(const char *s) { return rt_make_string_n(s, rt_strlen(s)); }

static void str_check_char(obj c, const char *who) { RT_CHECK(rt_is_char(c), who, "char", c); }

RT_FN obj rt_make_string(obj k, obj c) {
    char enc[4];
    intptr_t n, w, i;
    obj r;
    if (!rt_is_fix(k) || RT_FIXVAL(k) < 0 || RT_FIXVAL(k) > ((intptr_t)1 << 40)) rt_type_error("make-string", "valid length", k);
    str_check_char(c, "make-string");
    n = RT_FIXVAL(k);
    w = rt_utf8_encode(enc, RT_CHARVAL(c));
    r = str_alloc(n, n * w);
    for (i = 0; i < n; i++) rt_memcpy(RT_STR(r)->buf + i * w, enc, (size_t)w);
    return r;
}

/* Byte offset of character index k (0 <= k <= chars). */
static intptr_t str_offset(rt_string *s, intptr_t k) {
    intptr_t i = 0;
    size_t l;
    if (s->chars == s->bytes) return k;
    while (k-- > 0) {
        if ((unsigned char)s->buf[i] < 0x80) i++;
        else { rt_utf8_decode(s->buf + i, (size_t)(s->bytes - i), &l); i += (intptr_t)l; }
    }
    return i;
}
static intptr_t str_index(obj s, obj k, intptr_t limit, const char *who) {
    RT_CHECK(rt_is_string(s), who, "string", s);
    if (!rt_is_fix(k) || RT_FIXVAL(k) < 0 || RT_FIXVAL(k) >= limit) rt_range_error(who, k);
    return RT_FIXVAL(k);
}

RT_FN obj rt_string_ref(obj s, obj k) {
    rt_string *p;
    intptr_t i = str_index(s, k, rt_is_string(s) ? RT_STR(s)->chars : 0, "string-ref");
    size_t l;
    p = RT_STR(s);
    if (p->chars == p->bytes) return RT_CHAR((unsigned char)p->buf[i]);
    i = str_offset(p, i);
    return RT_CHAR(rt_utf8_decode(p->buf + i, (size_t)(p->bytes - i), &l));
}

/* Make bytes [da, db) of s room for n bytes and return a pointer to them; a size change
 * moves the string to a new buffer (the old one stays valid for the caller). The character
 * count is unchanged, so RT_F_ASCII (chars == bytes) is recomputed. */
static char *str_gap(obj s, intptr_t da, intptr_t db, intptr_t n) {
    rt_string *p = RT_STR(s);
    if (n != db - da) {
        intptr_t nb = p->bytes - (db - da) + n;
        char *buf = (char *)rt_alloc_raw((size_t)nb + 1);
        p = RT_STR(s);
        rt_memcpy(buf, p->buf, (size_t)da);
        rt_memcpy(buf + da + n, p->buf + db, (size_t)(p->bytes - db));
        buf[nb] = 0;
        p->buf = buf;
        p->bytes = nb;
        RT_SETFLAGS(s, p->chars == nb ? RT_F_ASCII : 0);
    }
    return p->buf + da;
}
static void str_mut(obj s, const char *who) {
    RT_CHECK(rt_is_string(s) && !(RT_HFLAGS(s) & RT_F_RO), who, "mutable string", s);
}

RT_FN obj rt_string_set(obj s, obj k, obj c) {
    rt_string *p;
    intptr_t i, w;
    char enc[4];
    size_t l;
    str_mut(s, "string-set!");
    i = str_index(s, k, RT_STR(s)->chars, "string-set!");
    str_check_char(c, "string-set!");
    p = RT_STR(s);
    if (p->chars == p->bytes && RT_CHARVAL(c) < 0x80) { p->buf[i] = (char)RT_CHARVAL(c); return RT_UNSPEC; }
    i = str_offset(p, i);
    rt_utf8_decode(p->buf + i, (size_t)(p->bytes - i), &l);
    w = rt_utf8_encode(enc, RT_CHARVAL(c));
    rt_memcpy(str_gap(s, i, i + (intptr_t)l, w), enc, (size_t)w);
    return RT_UNSPEC;
}

static void str_range(obj s, obj start, obj end, intptr_t *a, intptr_t *b, const char *who) {
    RT_CHECK(rt_is_string(s), who, "string", s);
    *a = rt_is_fix(start) ? RT_FIXVAL(start) : -1;
    *b = rt_is_fix(end) ? RT_FIXVAL(end) : -1;
    if (*a < 0 || *a > RT_STR(s)->chars) rt_range_error(who, start);
    if (*b < *a || *b > RT_STR(s)->chars) rt_range_error(who, end);
}

RT_FN obj rt_substring(obj s, obj start, obj end) {
    intptr_t a, b, oa, ob;
    obj r;
    str_range(s, start, end, &a, &b, "substring");
    oa = str_offset(RT_STR(s), a);
    ob = str_offset(RT_STR(s), b);
    r = str_alloc(b - a, ob - oa);
    rt_memcpy(RT_STR(r)->buf, RT_STR(s)->buf + oa, (size_t)(ob - oa));
    return r;
}

/* string-copy! and string-fill! replace a character range in place; the bytes are moved
 * once (variable-width replacement), so both are linear, also for non-ASCII strings. */
RT_FN obj rt_string_copy_x(obj to, obj at, obj from, obj start, obj end) {
    intptr_t a, b, k, oa, ob, da;
    str_range(from, start, end, &a, &b, "string-copy!");
    str_mut(to, "string-copy!");
    k = rt_is_fix(at) ? RT_FIXVAL(at) : -1;
    if (k < 0 || k > RT_STR(to)->chars - (b - a)) rt_range_error("string-copy!", at);
    oa = str_offset(RT_STR(from), a);
    ob = str_offset(RT_STR(from), b);
    da = str_offset(RT_STR(to), k);
    {
        const char *src = RT_STR(from)->buf + oa; /* stays valid if str_gap moves `to` */
        rt_memmove(str_gap(to, da, str_offset(RT_STR(to), k + b - a), ob - oa), src, (size_t)(ob - oa));
    }
    return RT_UNSPEC;
}
RT_FN obj rt_string_fill_x(obj s, obj c, obj start, obj end) {
    intptr_t a, b, i, w;
    char enc[4], *d;
    str_range(s, start, end, &a, &b, "string-fill!");
    str_mut(s, "string-fill!");
    str_check_char(c, "string-fill!");
    w = rt_utf8_encode(enc, RT_CHARVAL(c));
    d = str_gap(s, str_offset(RT_STR(s), a), str_offset(RT_STR(s), b), (b - a) * w);
    for (i = 0; i < b - a; i++) rt_memcpy(d + i * w, enc, (size_t)w);
    return RT_UNSPEC;
}

RT_FN obj rt_string_append(obj a, obj b) {
    obj r;
    RT_CHECK(rt_is_string(a), "string-append", "string", a);
    RT_CHECK(rt_is_string(b), "string-append", "string", b);
    r = str_alloc(RT_STR(a)->chars + RT_STR(b)->chars, RT_STR(a)->bytes + RT_STR(b)->bytes);
    rt_memcpy(RT_STR(r)->buf, RT_STR(a)->buf, (size_t)RT_STR(a)->bytes);
    rt_memcpy(RT_STR(r)->buf + RT_STR(a)->bytes, RT_STR(b)->buf, (size_t)RT_STR(b)->bytes);
    return r;
}

/* Byte order of UTF-8 equals code point order. */
RT_FN obj rt_string_compare(obj a, obj b) {
    intptr_t na, nb;
    int c;
    RT_CHECK(rt_is_string(a), "string-compare", "string", a);
    RT_CHECK(rt_is_string(b), "string-compare", "string", b);
    na = RT_STR(a)->bytes; nb = RT_STR(b)->bytes;
    c = rt_memcmp(RT_STR(a)->buf, RT_STR(b)->buf, (size_t)(na < nb ? na : nb));
    if (c == 0) c = na < nb ? -1 : na > nb;
    return RT_FIX(c < 0 ? -1 : c > 0);
}

RT_FN obj rt_string_to_list(obj s, obj start, obj end) {
    intptr_t a, b, i, j;
    obj r = RT_NIL;
    str_range(s, start, end, &a, &b, "string->list");
    /* collect forward into a reversed list, then reverse in place */
    for (i = str_offset(RT_STR(s), a), j = a; j < b; j++) {
        size_t l;
        r = rt_cons(RT_CHAR(rt_utf8_decode(RT_STR(s)->buf + i, (size_t)(RT_STR(s)->bytes - i), &l)), r);
        i += (intptr_t)l;
    }
    {
        obj prev = RT_NIL;
        while (r != RT_NIL) { obj n = RT_CDR(r); RT_CDR(r) = prev; prev = r; r = n; }
        return prev;
    }
}

RT_FN obj rt_list_to_string(obj l) {
    intptr_t n = rt_list_length(l, "list->string"), nb = 0, i = 0;
    char enc[4];
    obj x, r;
    for (x = l; x != RT_NIL; x = RT_CDR(x)) {
        str_check_char(RT_CAR(x), "list->string");
        nb += rt_utf8_encode(enc, RT_CHARVAL(RT_CAR(x)));
    }
    r = str_alloc(n, nb);
    for (x = l; x != RT_NIL; x = RT_CDR(x)) i += rt_utf8_encode(RT_STR(r)->buf + i, RT_CHARVAL(RT_CAR(x)));
    return r;
}

RT_FN obj rt_string_to_utf8(obj s) {
    obj r;
    RT_CHECK(rt_is_string(s), "string->utf8", "string", s);
    r = RT_TAGPTR(rt_alloc(RT_HDR(RT_T_BYTEVECTOR, 0, RT_STR(s)->bytes), sizeof(uintptr_t) + (size_t)RT_STR(s)->bytes));
    rt_memcpy(RT_PTR(r, rt_bytevector)->b, RT_STR(s)->buf, (size_t)RT_STR(s)->bytes);
    return r;
}

/* Malformed sequences become U+FFFD so that strings always hold valid UTF-8. */
RT_FN obj rt_utf8_to_string(obj bv) {
    const char *b;
    intptr_t n, i = 0, nb = 0, k = 0;
    char enc[4];
    size_t l;
    obj r;
    RT_CHECK(rt_is_bytevector(bv), "utf8->string", "bytevector", bv);
    b = (const char *)RT_PTR(bv, rt_bytevector)->b;
    n = RT_HLEN(bv);
    while (i < n) { nb += rt_utf8_encode(enc, rt_utf8_decode(b + i, (size_t)(n - i), &l)); i += (intptr_t)l; k++; }
    r = str_alloc(k, nb);
    b = (const char *)RT_PTR(bv, rt_bytevector)->b;
    for (i = 0, nb = 0; i < n; i += (intptr_t)l) nb += rt_utf8_encode(RT_STR(r)->buf + nb, rt_utf8_decode(b + i, (size_t)(n - i), &l));
    return r;
}

RT_FN obj rt_integer_to_char(obj n) {
    intptr_t c = rt_is_fix(n) ? RT_FIXVAL(n) : -1;
    if (c < 0 || c > 0x10FFFF || (c >= 0xD800 && c < 0xE000)) rt_type_error("integer->char", "Unicode scalar value", n);
    return RT_CHAR(c);
}

/* ---- (scheme char): character classes and case (R7RS 6.6, 6.7) ----
 * ASCII here; other characters use the Unicode tables (rt_unicode.c) when they are enabled
 * and are otherwise caseless, not alphabetic, not numeric and not whitespace. */
#ifdef SHIKRA_PROGRAM
#ifdef RT_UNICODE_FULL
#define STR_UNI 1
#endif
#else
RT_VAR int rt_unicode_full;
#define STR_UNI rt_unicode_full
#endif
static int str_prop(int c, int p) {
    if (c < 0x80)
        switch (p) {
        case RT_UNI_ALPHA: return (c | 0x20) >= 'a' && (c | 0x20) <= 'z';
        case RT_UNI_UPPER: return c >= 'A' && c <= 'Z';
        case RT_UNI_LOWER: return c >= 'a' && c <= 'z';
        case RT_UNI_SPACE: return c == ' ' || (c >= 9 && c <= 13);
        default: return c >= '0' && c <= '9' ? c - '0' : -1;
        }
#ifdef RT_HAVE_UNICODE
    if (STR_UNI) return rt_uni_prop(c, p);
#endif
    return p == RT_UNI_DIGIT ? -1 : 0;
}
static int str_map(int c, int m) { /* m: 0 upper, 1 lower, 2 fold */
    if (c < 0x80) return m == 0 ? (c >= 'a' && c <= 'z' ? c - 32 : c) : (c >= 'A' && c <= 'Z' ? c + 32 : c);
#ifdef RT_HAVE_UNICODE
    if (STR_UNI) return rt_uni_map(c, m);
#endif
    return c;
}
static int str_char(obj c, const char *who) { str_check_char(c, who); return RT_CHARVAL(c); }
RT_FN obj rt_char_upcase(obj c) { return RT_CHAR(str_map(str_char(c, "char-upcase"), 0)); }
RT_FN obj rt_char_downcase(obj c) { return RT_CHAR(str_map(str_char(c, "char-downcase"), 1)); }
RT_FN obj rt_char_foldcase(obj c) { return RT_CHAR(str_map(str_char(c, "char-foldcase"), 2)); }
RT_FN obj rt_char_alphabetic_p(obj c) { return RT_BOOL(str_prop(str_char(c, "char-alphabetic?"), RT_UNI_ALPHA)); }
RT_FN obj rt_char_numeric_p(obj c) { return RT_BOOL(str_prop(str_char(c, "char-numeric?"), RT_UNI_DIGIT) >= 0); }
RT_FN obj rt_char_whitespace_p(obj c) { return RT_BOOL(str_prop(str_char(c, "char-whitespace?"), RT_UNI_SPACE)); }
RT_FN obj rt_char_upper_case_p(obj c) { return RT_BOOL(str_prop(str_char(c, "char-upper-case?"), RT_UNI_UPPER)); }
RT_FN obj rt_char_lower_case_p(obj c) { return RT_BOOL(str_prop(str_char(c, "char-lower-case?"), RT_UNI_LOWER)); }
RT_FN obj rt_digit_value(obj c) {
    int v = str_prop(str_char(c, "digit-value"), RT_UNI_DIGIT);
    return v < 0 ? RT_FALSE : RT_FIX(v);
}
/* Full case conversion of one character into out[3] (Unicode: SpecialCasing, final sigma, full
 * folding = simple folding of the full upper case of the lower case; see tools/mkunicode.c). */
static int str_case1(int c, int m, const char *s, intptr_t i, intptr_t n, int prev, int *out) {
#ifdef RT_HAVE_UNICODE
    if (c >= 0x80 && STR_UNI) {
        int k, j;
        size_t len;
        if (m == 1 && c == 0x3A3) { /* final sigma: after a cased letter, not before one */
            int next = i < n ? rt_utf8_decode(s + i, (size_t)(n - i), &len) : 0;
            out[0] = (str_prop(prev, RT_UNI_UPPER) || str_prop(prev, RT_UNI_LOWER)) &&
                     !(str_prop(next, RT_UNI_UPPER) || str_prop(next, RT_UNI_LOWER)) ? 0x3C2 : 0x3C3;
            return 1;
        }
        if (m < 2) return (k = rt_uni_full(c, m, out)) ? k : (out[0] = rt_uni_map(c, m), 1);
        if ((k = rt_uni_full(c, 2, out)) || (k = rt_uni_full(c, 1, out)) || (k = rt_uni_full(rt_uni_map(c, 1), 0, out))) {
            for (j = 0; j < k; j++) out[j] = str_map(out[j], 2);
            return k;
        }
    }
#else
    (void)s; (void)i; (void)n; (void)prev;
#endif
    out[0] = str_map(c, m);
    return 1;
}
static obj str_case(obj s, int m, const char *who) {
    obj out;
    intptr_t i;
    size_t l;
    int c, prev = 0, k, n, buf[3];
    RT_CHECK(rt_is_string(s), who, "string", s);
    if (RT_STR(s)->chars == RT_STR(s)->bytes) { /* ASCII */
        out = rt_make_string_n(RT_STR(s)->buf, (size_t)RT_STR(s)->bytes);
        for (i = 0; i < RT_STR(out)->bytes; i++) RT_STR(out)->buf[i] = (char)str_map((unsigned char)RT_STR(out)->buf[i], m);
        return out;
    }
    out = rt_open_output_string();
    for (i = 0; i < RT_STR(s)->bytes; i += (intptr_t)l, prev = c) {
        c = rt_utf8_decode(RT_STR(s)->buf + i, (size_t)(RT_STR(s)->bytes - i), &l);
        n = str_case1(c, m, RT_STR(s)->buf, i + (intptr_t)l, RT_STR(s)->bytes, prev, buf);
        for (k = 0; k < n; k++) rt_port_putc(out, buf[k]);
    }
    return rt_get_output_string(out);
}
RT_FN obj rt_string_upcase(obj s) { return str_case(s, 0, "string-upcase"); }
RT_FN obj rt_string_downcase(obj s) { return str_case(s, 1, "string-downcase"); }
RT_FN obj rt_string_foldcase(obj s) { return str_case(s, 2, "string-foldcase"); }
