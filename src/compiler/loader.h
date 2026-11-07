/*
 * loader.h - programs made of several files: `import "path.mfl"` (internal
 * API, v0.3). See docs/language-spec.md section 13.
 *
 * The loader parses the main file, then every file it imports (directly or
 * through other imports), each exactly once, and joins their top-level
 * statements into ONE program, dependencies first:
 *
 *     main.mfl:  import "materials/steel.mfl"      steel.mfl statements
 *                import numerical            ==>   main.mfl statements
 *                ...
 *
 * Every top-level statement records the file it came from (MflStmt.file),
 * so the checker can apply the visibility rules and every message names
 * the right file. Module imports (`import numerical`) are not resolved
 * here: they stay in the program and the checker handles them.
 *
 * Paths: an import path is relative to the directory of the file that
 * contains the import, uses '/' as separator, and must end in ".mfl".
 * Absolute paths, backslashes and empty segments are rejected. Paths are
 * normalised lexically ("a/./b/../c.mfl" is "a/c.mfl"; symbolic links are
 * not followed), and the normalised text identifies a file: importing the
 * same file twice (also through different importers) loads it once.
 *
 * Errors (reported at the import statement, as file:line:col): missing or
 * unreadable files, import cycles (with the chain of files), invalid paths,
 * more than MFL_LOAD_MAX_FILES files or MFL_LOAD_MAX_DEPTH nested imports,
 * and file imports when no reader was given (the embedding API disables
 * them unless the host enables them).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MFL_LOADER_H
#define MFL_LOADER_H

#include <stddef.h>

#include "parser/parser.h"
#include "runtime/arena.h"
#include "runtime/diagnostic.h"

#define MFL_LOAD_MAX_FILES 64
#define MFL_LOAD_MAX_DEPTH 32
#define MFL_LOAD_MAX_PATH 1024

/* Reads a whole file. On success returns 1 and sets *data to a buffer
 * allocated with malloc() (the loader frees it) and *length. Returns 0 if
 * the file cannot be read. */
typedef int (*MflReadSourceFn)(void *ctx, const char *path, char **data, size_t *length);

/* Standard reader: fopen()/fread() in binary mode. */
int mfl_read_source_file(void *ctx, const char *path, char **data, size_t *length);

typedef struct {
    char          *name;   /* path as shown in messages (main: as given) */
    const char    *source; /* main: the caller's buffer; others: owned */
    size_t         length;
    int            owned;
    MflParseResult parse;
    int           *imports; /* direct file imports (indices) */
    int            import_count;
    unsigned long long hash; /* FNV-1a 64 of the source text (provenance) */
} MflLoadedFile;

typedef struct {
    MflLoadedFile *files; /* files[0] is the main file */
    int            file_count;
    const char   **names; /* file_count names (for MflProgram.file_names) */
    unsigned char *visible;
    MflProgram     program; /* all files joined, dependencies first */
    MflArena       arena;   /* loader diagnostics */
    MflDiagnostic *diagnostics; /* parser and loader errors, sorted by file and position */
    int            diagnostic_count;
    int            out_of_memory;
} MflLoadResult;

/* Parses `name` (source[0 .. length), not copied: it must outlive the
 * result) and everything it imports. read may be NULL: file imports are
 * then errors. Returns 1 when every file parsed without errors. Always call
 * mfl_load_free() afterwards. */
int mfl_load(const char *source, size_t length, const char *name, MflReadSourceFn read,
             void *read_ctx, size_t memory_limit, MflLoadResult *out);

void mfl_load_free(MflLoadResult *result);

/* Lexical normalisation of a path, as used to identify files ('.' and
 * '..' segments resolved, separators become '/'). Returns 0 if it does not
 * fit in size bytes. */
int mfl_normalise_path(const char *in, char *out, size_t size);

/* FNV-1a 64-bit hash of a byte string. */
unsigned long long mfl_fnv1a64(const char *data, size_t length);

#endif /* MFL_LOADER_H */
