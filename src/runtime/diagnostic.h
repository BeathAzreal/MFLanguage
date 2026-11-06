/*
 * diagnostic.h - error messages with source positions (internal API).
 * Shared by the parser and the semantic checker.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MFL_DIAGNOSTIC_H
#define MFL_DIAGNOSTIC_H

typedef struct MflDiagnostic MflDiagnostic;

struct MflDiagnostic {
    int            line;
    int            column;
    const char    *message; /* owned by the arena of the producing stage */
    MflDiagnostic *next;
};

#endif /* MFL_DIAGNOSTIC_H */
