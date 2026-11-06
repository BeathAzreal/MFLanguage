/*
 * number.h - locale-independent number parsing and formatting (internal).
 *
 * The C library's strtod/printf use the decimal separator of the current
 * locale (LC_NUMERIC). A host program running in pt_BR would read "0.8"
 * as 0 and print "0,8". MFL literals and output always use '.'.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MFL_NUMBER_H
#define MFL_NUMBER_H

#include <stddef.h>

typedef enum {
    MFL_NUM_OK,
    MFL_NUM_OVERFLOW,  /* magnitude too large for a double */
    MFL_NUM_UNDERFLOW, /* non-zero literal that rounds to zero */
    MFL_NUM_INVALID    /* not an MFL number literal, or too long */
} MflNumStatus;

/* Parses an MFL NUMBER literal: digits ('.' digits)? ([eE] [+-]? digits)?
 * The result is the correctly rounded double for literals with up to 19
 * significant digits and a decimal exponent within +-22 after scaling
 * (exact fast path); other literals use the C library's strtod with the
 * text adapted to the current locale. */
MflNumStatus mfl_number_parse(const char *text, size_t length, double *out);

/* Formats a double as the shortest decimal that parses back to the same
 * value: "480", "0.8", "1.5e-5", "6.02e23", "inf", "-inf", "nan".
 * Fixed notation is used for decimal exponents -4..15. buf should hold at
 * least MFL_NUMBER_BUF bytes. */
#define MFL_NUMBER_BUF 40
void mfl_number_format(double value, char *buf, size_t size);

#endif /* MFL_NUMBER_H */
