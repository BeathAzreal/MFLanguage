/* Compare output text exactly, allowing rounding differences in finite numbers.
 * Both arguments have their line endings normalised by compare_output.cmake.
 * Tolerance: 1e-12 absolute + 1e-12 relative to the expected value.
 * SPDX-License-Identifier: MIT
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    const char *expected;
    const char *actual;
    if (argc != 3) {
        fprintf(stderr, "usage: compare_numeric_output <expected> <actual>\n");
        return 2;
    }
    expected = argv[1];
    actual = argv[2];
    while (*expected != '\0' && *actual != '\0') {
        char *expected_end;
        char *actual_end;
        double reference = strtod(expected, &expected_end);
        double value = strtod(actual, &actual_end);
        if (expected_end != expected || actual_end != actual) {
            if (expected_end == expected || actual_end == actual ||
                !isfinite(reference) || !isfinite(value) ||
                fabs(value - reference) > 1e-12 + 1e-12 * fabs(reference)) {
                fprintf(stderr, "numeric output differs near expected '%s', actual '%s'\n",
                        expected, actual);
                return 1;
            }
            expected = expected_end;
            actual = actual_end;
        } else {
            if (*expected != *actual) {
                fprintf(stderr, "output text differs near expected '%s', actual '%s'\n",
                        expected, actual);
                return 1;
            }
            ++expected;
            ++actual;
        }
    }
    if (*expected != *actual) {
        fprintf(stderr, "output length differs\n");
        return 1;
    }
    return 0;
}
