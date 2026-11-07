/*
 * test_number.c - unit tests for src/runtime/number (parse and format).
 *
 * The reference for parsing is the C library's strtod in the "C" locale,
 * which glibc, macOS and the Windows UCRT implement with correct
 * rounding. A second pass repeats key checks under a locale whose decimal
 * separator is ',' when the system provides one.
 *
 * SPDX-License-Identifier: MIT
 */
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime/number.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            g_failures++;                                                    \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                  \
        }                                                                    \
    } while (0)

static unsigned long long g_seed = 0x9E3779B97F4A7C15ULL;

static unsigned long long rnd(void)
{
    /* xorshift64* */
    g_seed ^= g_seed >> 12;
    g_seed ^= g_seed << 25;
    g_seed ^= g_seed >> 27;
    return g_seed * 2685821657736338717ULL;
}

static double parse(const char *s)
{
    double v = -1.0;
    MflNumStatus st = mfl_number_parse(s, strlen(s), &v);
    return st == MFL_NUM_OK ? v : -1.0;
}

static void expect_format(double v, const char *want, int line)
{
    char buf[MFL_NUMBER_BUF];
    mfl_number_format(v, buf, sizeof buf);
    g_checks++;
    if (strcmp(buf, want) != 0) {
        g_failures++;
        fprintf(stderr, "%s:%d: format(%.17g) = '%s', want '%s'\n", __FILE__, line, v, buf, want);
    }
}

#define EXPECT_FORMAT(v, want) expect_format(v, want, __LINE__)

static void test_parse_basic(void)
{
    double v;
    CHECK(parse("0") == 0.0);
    CHECK(parse("000") == 0.0);
    CHECK(parse("0.0e5") == 0.0);
    CHECK(parse("20") == 20.0);
    CHECK(parse("0.8") == 0.8);
    CHECK(parse("1.5e-3") == 1.5e-3);
    CHECK(parse("6.02e23") == 6.02e23);
    CHECK(parse("1E+5") == 1e5);
    CHECK(parse("007") == 7.0);
    CHECK(parse("7850") == 7850.0);
    CHECK(parse("0.1") == 0.1);
    CHECK(parse("9007199254740993") == 9007199254740992.0); /* 2^53 + 1 rounds to even */
    CHECK(parse("1.7976931348623157e308") == 1.7976931348623157e308);
    CHECK(parse("4.9406564584124654e-324") == 4.9406564584124654e-324); /* denormal */
    CHECK(parse("123456789012345678901234567890") == 1.2345678901234568e29);
    CHECK(parse("0.000000000000000000000000000001") == 1e-30);

    CHECK(mfl_number_parse("1e400", 5, &v) == MFL_NUM_OVERFLOW);
    CHECK(mfl_number_parse("1e-400", 6, &v) == MFL_NUM_UNDERFLOW);
    CHECK(mfl_number_parse("", 0, &v) == MFL_NUM_INVALID);
    CHECK(mfl_number_parse(".5", 2, &v) == MFL_NUM_INVALID);
    CHECK(mfl_number_parse("5.", 2, &v) == MFL_NUM_INVALID);
    CHECK(mfl_number_parse("1e", 2, &v) == MFL_NUM_INVALID);
    CHECK(mfl_number_parse("1e+", 3, &v) == MFL_NUM_INVALID);
    CHECK(mfl_number_parse("1x", 2, &v) == MFL_NUM_INVALID);
    CHECK(mfl_number_parse("-1", 2, &v) == MFL_NUM_INVALID);
    CHECK(mfl_number_parse("1,5", 3, &v) == MFL_NUM_INVALID);
    /* Only `length` bytes are read. */
    CHECK(mfl_number_parse("2.5mm", 3, &v) == MFL_NUM_OK && v == 2.5);
}

/* Random literals: must equal strtod's correctly rounded result. */
static void test_parse_random(void)
{
    int i;
    int mismatches = 0;
    for (i = 0; i < 200000; i++) {
        char lit[64];
        unsigned long long r = rnd();
        int ndig = 1 + (int)(r % 22);       /* up to 22 significant digits */
        int point = (int)((r >> 8) % (unsigned)(ndig + 1));
        int exp = (int)((r >> 16) % 80) - 40;
        int use_exp = (int)((r >> 24) & 1);
        int k;
        int n = 0;
        double mine = 0.0;
        double ref;
        MflNumStatus st;

        lit[n++] = (char)('1' + (rnd() % 9));
        for (k = 1; k < ndig; k++) {
            if (k == point) {
                lit[n++] = '.';
            }
            lit[n++] = (char)('0' + (rnd() % 10));
        }
        if (use_exp) {
            n += snprintf(lit + n, sizeof lit - (size_t)n, "e%d", exp);
        }
        lit[n] = '\0';
        ref = strtod(lit, NULL);
        st = mfl_number_parse(lit, (size_t)n, &mine);
        if (st != MFL_NUM_OK || memcmp(&mine, &ref, sizeof mine) != 0) {
            if (mismatches++ < 5) {
                fprintf(stderr, "parse mismatch: '%s' -> %.17g, strtod %.17g\n", lit, mine, ref);
            }
        }
    }
    g_checks++;
    if (mismatches != 0) {
        g_failures++;
        fprintf(stderr, "test_parse_random: %d mismatches\n", mismatches);
    }
}

static void test_format_basic(void)
{
    EXPECT_FORMAT(480.0, "480");
    EXPECT_FORMAT(0.8, "0.8");
    EXPECT_FORMAT(0.1, "0.1");
    EXPECT_FORMAT(0.1 + 0.2, "0.30000000000000004");
    EXPECT_FORMAT(1.0 / 3.0, "0.3333333333333333");
    EXPECT_FORMAT(2400.0, "2400");
    EXPECT_FORMAT(1e15, "1000000000000000");
    EXPECT_FORMAT(1e16, "1e16");
    EXPECT_FORMAT(1.2345678901234568e17, "1.2345678901234568e17");
    EXPECT_FORMAT(0.0001, "0.0001");
    EXPECT_FORMAT(0.000123, "0.000123");
    EXPECT_FORMAT(1.5e-5, "1.5e-5");
    EXPECT_FORMAT(6.02e23, "6.02e23");
    EXPECT_FORMAT(-12.5, "-12.5");
    EXPECT_FORMAT(0.0, "0");
    EXPECT_FORMAT(-0.0, "-0");
    EXPECT_FORMAT(1.7976931348623157e308, "1.7976931348623157e308");
    EXPECT_FORMAT(4.9406564584124654e-324, "5e-324");
    EXPECT_FORMAT(HUGE_VAL, "inf");
    EXPECT_FORMAT(-HUGE_VAL, "-inf");
    {
        volatile double zero = 0.0;
        EXPECT_FORMAT(zero / zero, "nan");
    }
}

/* Random doubles: output must parse back exactly, and be minimal. */
static void test_format_roundtrip(void)
{
    int i;
    int bad = 0;
    for (i = 0; i < 200000; i++) {
        unsigned long long bits = rnd();
        double v;
        char buf[MFL_NUMBER_BUF];
        double back;
        memcpy(&v, &bits, sizeof v);
        if (isnan(v) || isinf(v)) {
            continue;
        }
        if (i % 2 == 0) {
            v = ldexp((double)(bits >> 11), -(int)(bits % 80)); /* "ordinary" magnitudes */
        }
        mfl_number_format(v, buf, sizeof buf);
        back = strtod(buf, NULL);
        if (back != v || (v != 0.0 && strchr(buf, ',') != NULL)) {
            if (bad++ < 5) {
                fprintf(stderr, "format roundtrip: %.17g -> '%s' -> %.17g\n", v, buf, back);
            }
        }
        /* Minimal: no representation with fewer significant digits exists. */
        if (i < 20000 && v != 0.0) {
            char shorter[64];
            char digs[64];
            int nd = 0;
            int first = 0;
            const char *p;
            for (p = buf; *p != '\0' && *p != 'e'; p++) {
                if (*p >= '0' && *p <= '9' && nd < 63) {
                    digs[nd++] = *p;
                }
            }
            while (first < nd && digs[first] == '0') {
                first++; /* leading zeros are not significant */
            }
            while (nd > first && digs[nd - 1] == '0') {
                nd--; /* nor are trailing ones */
            }
            if (nd - first > 1) {
                snprintf(shorter, sizeof shorter, "%.*e", nd - first - 2, v);
                if (strtod(shorter, NULL) == v) {
                    if (bad++ < 5) {
                        fprintf(stderr, "format not minimal: %.17g -> '%s' (also '%s')\n", v, buf,
                                shorter);
                    }
                }
            }
        }
    }
    g_checks++;
    if (bad != 0) {
        g_failures++;
        fprintf(stderr, "test_format_roundtrip: %d problems\n", bad);
    }
}

/* Under a locale with ',' as decimal separator, nothing changes. */
static void test_comma_locale(void)
{
    static const char *const names[] = {"pt_BR.UTF-8", "pt_BR.utf8", "de_DE.UTF-8", "de_DE.utf8",
                                        "pt-BR",       "de-DE",      "Portuguese_Brazil.1252",
                                        "German_Germany.1252"};
    size_t i;
    const char *used = NULL;
    for (i = 0; i < sizeof names / sizeof names[0]; i++) {
        if (setlocale(LC_NUMERIC, names[i]) != NULL && strcmp(localeconv()->decimal_point, ",") == 0) {
            used = names[i];
            break;
        }
    }
    if (used == NULL) {
        setlocale(LC_NUMERIC, "C");
        printf("test_number: comma-locale test SKIPPED (no such locale installed)\n");
        return;
    }
    printf("test_number: comma-locale test ran with %s (decimal point ',')\n", used);
    CHECK(parse("0.8") == 0.8);
    CHECK(parse("1.5e-3") == 1.5e-3);
    CHECK(parse("123456789012345678901234567890") == 1.2345678901234568e29); /* strtod path */
    CHECK(parse("4.9406564584124654e-324") == 4.9406564584124654e-324);
    EXPECT_FORMAT(0.8, "0.8");
    EXPECT_FORMAT(12.461921905289394, "12.461921905289394");
    EXPECT_FORMAT(1.5e-5, "1.5e-5");
    EXPECT_FORMAT(1.2345678901234568e17, "1.2345678901234568e17");
    setlocale(LC_NUMERIC, "C");
}

int main(void)
{
    test_parse_basic();
    test_parse_random();
    test_format_basic();
    test_format_roundtrip();
    test_comma_locale();
    printf("test_number: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
