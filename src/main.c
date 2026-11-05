/*
 * main.c - `mfl` command-line driver (scaffold).
 *
 * The lexer, parser, compiler and VM are not implemented yet, so this
 * driver can only report its version. It exits with status 2 when asked
 * to run a script, to make that limitation explicit.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "mfl/mfl.h"

static void usage(FILE *out)
{
    fprintf(out,
            "usage: mfl [--version | --help] [script.mfl]\n"
            "\n"
            "MFL %s - pre-alpha scaffold. Script execution is not implemented yet.\n",
            mfl_version());
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage(stderr);
        return 1;
    }
    if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0) {
        printf("mfl %s\n", mfl_version());
        return 0;
    }
    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        usage(stdout);
        return 0;
    }
    fprintf(stderr, "mfl: cannot run '%s': the interpreter is not implemented yet (v%s).\n",
            argv[1], mfl_version());
    return 2;
}
