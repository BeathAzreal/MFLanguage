/* number.c - locale-independent numbers. SPDX-License-Identifier: MIT */
#include "runtime/number.h"

#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LITERAL 400

static const double pow10_exact[23] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22,
};

/* strtod on a copy of the literal whose '.' is replaced by the locale's
 * decimal separator. */
static double strtod_any_locale(const char *text, size_t length)
{
    char buf[MAX_LITERAL + 16];
    const char *dp = localeconv()->decimal_point;
    size_t dpl = (dp != NULL && dp[0] != '\0') ? strlen(dp) : 1;
    size_t i;
    size_t n = 0;

    for (i = 0; i < length && n + dpl < sizeof buf - 1; i++) {
        if (text[i] == '.' && dp != NULL && dp[0] != '\0') {
            memcpy(buf + n, dp, dpl);
            n += dpl;
        } else {
            buf[n++] = text[i];
        }
    }
    buf[n] = '\0';
    return strtod(buf, NULL);
}

MflNumStatus mfl_number_parse(const char *text, size_t length, double *out)
{
    unsigned long long mant = 0;
    int sig_digits = 0;   /* significant digits accumulated in mant */
    int dropped = 0;      /* significant digits beyond 19 (not in mant) */
    long exp10 = 0;       /* decimal exponent applied to mant */
    int nonzero = 0;
    size_t i = 0;
    double value;

    if (length == 0 || length > MAX_LITERAL || text[0] < '0' || text[0] > '9') {
        return MFL_NUM_INVALID;
    }
    /* integer part */
    for (; i < length && text[i] >= '0' && text[i] <= '9'; i++) {
        int d = text[i] - '0';
        if (d != 0) {
            nonzero = 1;
        }
        if (mant == 0 && d == 0) {
            continue; /* leading zero */
        }
        if (sig_digits < 19) {
            mant = mant * 10 + (unsigned long long)d;
            sig_digits++;
        } else {
            dropped++;
            exp10++;
        }
    }
    /* fraction */
    if (i < length && text[i] == '.') {
        i++;
        if (i >= length || text[i] < '0' || text[i] > '9') {
            return MFL_NUM_INVALID;
        }
        for (; i < length && text[i] >= '0' && text[i] <= '9'; i++) {
            int d = text[i] - '0';
            if (d != 0) {
                nonzero = 1;
            }
            if (mant == 0 && d == 0) {
                exp10--;
                continue;
            }
            if (sig_digits < 19) {
                mant = mant * 10 + (unsigned long long)d;
                sig_digits++;
                exp10--;
            } else if (d != 0) {
                dropped++;
            }
        }
    }
    /* exponent */
    if (i < length && (text[i] == 'e' || text[i] == 'E')) {
        int neg = 0;
        long e = 0;
        i++;
        if (i < length && (text[i] == '+' || text[i] == '-')) {
            neg = text[i] == '-';
            i++;
        }
        if (i >= length || text[i] < '0' || text[i] > '9') {
            return MFL_NUM_INVALID;
        }
        for (; i < length && text[i] >= '0' && text[i] <= '9'; i++) {
            if (e < 100000) {
                e = e * 10 + (text[i] - '0');
            }
        }
        exp10 += neg ? -e : e;
    }
    if (i != length) {
        return MFL_NUM_INVALID;
    }
    if (!nonzero) {
        *out = 0.0;
        return MFL_NUM_OK;
    }

    /* Exact fast path (Clinger): mant and 10^|e| are exact doubles, so a
     * single IEEE multiplication or division is correctly rounded. */
    if (dropped == 0 && mant <= (1ULL << 53) && exp10 >= -22 && exp10 <= 22) {
        value = exp10 >= 0 ? (double)mant * pow10_exact[exp10]
                           : (double)mant / pow10_exact[-exp10];
    } else {
        value = strtod_any_locale(text, length);
    }
    if (isinf(value)) {
        return MFL_NUM_OVERFLOW;
    }
    if (value == 0.0) {
        return MFL_NUM_UNDERFLOW;
    }
    *out = value;
    return MFL_NUM_OK;
}

/* Significant digits and decimal exponent of value with p digits, read
 * from "%.*e" output without depending on the locale's separator. */
static int digits_of(double value, int p, char *digits, int *exp10)
{
    char tmp[64];
    int n = 0;
    const char *s = tmp;
    snprintf(tmp, sizeof tmp, "%.*e", p - 1, value);
    if (*s == '-') {
        s++;
    }
    while (*s != '\0' && *s != 'e' && *s != 'E') {
        if (*s >= '0' && *s <= '9') {
            digits[n++] = *s;
        }
        s++;
    }
    digits[n] = '\0';
    *exp10 = (*s == 'e' || *s == 'E') ? atoi(s + 1) : 0;
    return n;
}

/* Canonical MFL literal for digits/exponent, for the round-trip test. */
static void literal_of(const char *digits, int n, int exp10, char *buf, size_t size)
{
    if (n == 1) {
        snprintf(buf, size, "%ce%d", digits[0], exp10);
    } else {
        snprintf(buf, size, "%c.%se%d", digits[0], digits + 1, exp10);
    }
}

void mfl_number_format(double value, char *buf, size_t size)
{
    char digits[32];
    char lit[64];
    int n = 0;
    int exp10 = 0;
    int p;
    int neg;
    size_t pos = 0;

    if (size == 0) {
        return;
    }
    if (isnan(value)) {
        snprintf(buf, size, "nan");
        return;
    }
    if (isinf(value)) {
        snprintf(buf, size, value < 0 ? "-inf" : "inf");
        return;
    }
    neg = signbit(value) != 0;
    if (value == 0.0) {
        snprintf(buf, size, neg ? "-0" : "0");
        return;
    }

    /* Shortest p in 1..17 whose decimal form reads back exactly. */
    for (p = 1; p <= 17; p++) {
        double back = 0.0;
        n = digits_of(fabs(value), p, digits, &exp10);
        literal_of(digits, n, exp10, lit, sizeof lit);
        if (mfl_number_parse(lit, strlen(lit), &back) == MFL_NUM_OK && back == fabs(value)) {
            break;
        }
    }
    /* Trailing zeros carry no information. */
    while (n > 1 && digits[n - 1] == '0') {
        digits[--n] = '\0';
    }

#define PUT(ch)                      \
    do {                             \
        if (pos + 1 < size) {        \
            buf[pos++] = (char)(ch); \
        }                            \
    } while (0)

    if (neg) {
        PUT('-');
    }
    if (exp10 >= -4 && exp10 < 16) {
        int k;
        if (exp10 < 0) {
            PUT('0');
            PUT('.');
            for (k = 0; k < -exp10 - 1; k++) {
                PUT('0');
            }
            for (k = 0; k < n; k++) {
                PUT(digits[k]);
            }
        } else {
            for (k = 0; k <= exp10; k++) {
                PUT(k < n ? digits[k] : '0');
            }
            if (n > exp10 + 1) {
                PUT('.');
                for (k = exp10 + 1; k < n; k++) {
                    PUT(digits[k]);
                }
            }
        }
        buf[pos] = '\0';
    } else {
        int k;
        char e[16];
        PUT(digits[0]);
        if (n > 1) {
            PUT('.');
            for (k = 1; k < n; k++) {
                PUT(digits[k]);
            }
        }
        snprintf(e, sizeof e, "e%d", exp10);
        for (k = 0; e[k] != '\0'; k++) {
            PUT(e[k]);
        }
        buf[pos] = '\0';
    }
#undef PUT
}
