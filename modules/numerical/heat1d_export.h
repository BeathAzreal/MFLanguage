/*
 * heat1d_export.h - CSV and JSON metadata of a 1D heat conduction run
 * (v0.3). See docs/heat-conduction.md section "Export".
 *
 * CSV: header "time_s,position_m,temperature_K", then one row per sample
 * and node, ordered by time and then by position; numbers are the
 * shortest decimal that reads back to the same double, always with '.'
 * (independent of the locale); lines end in "\n".
 *
 * Completeness: mfl_h1_export() first removes an old metadata file, writes
 * both files under temporary names (<name>.partial), then renames the CSV
 * and finally the metadata. The metadata file is the completion marker: an
 * export is complete exactly when the metadata file exists (it records the
 * number of rows, the size and an FNV-1a hash of the CSV). After a failure
 * the temporary files are removed and no metadata file exists.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MFL_NUMERICAL_HEAT1D_EXPORT_H
#define MFL_NUMERICAL_HEAT1D_EXPORT_H

#include <stddef.h>

#include "heat1d.h"

/* Receives output; returns 1 on success, 0 if the data could not be written. */
typedef int (*MflH1WriteFn)(void *ctx, const char *data, size_t length);

typedef struct {
    const char               *mfl_version;
    const char *const        *files;  /* source files of the program (main first), or NULL */
    const unsigned long long *hashes; /* FNV-1a 64 of each file */
    size_t                    file_count;
} MflH1Provenance;

typedef struct {
    size_t             rows;  /* data rows (header excluded) */
    size_t             bytes; /* total bytes, header included */
    unsigned long long hash;  /* FNV-1a 64 of all bytes */
} MflH1CsvInfo;

/* Writes the CSV. Returns 1 on success, 0 when a write failed. */
int mfl_h1_write_csv(const MflH1Result *r, MflH1WriteFn write, void *ctx, MflH1CsvInfo *info);

/* Writes the JSON metadata. csv_file is the CSV's name as referenced from
 * the metadata (normally the file name only). Returns 1 / 0 as above. */
int mfl_h1_write_metadata(const MflH1Result *r, const MflH1Provenance *prov, const char *csv_file,
                          const MflH1CsvInfo *csv, MflH1WriteFn write, void *ctx);

/* File operations used by mfl_h1_export(); NULL members use stdio. Tests
 * replace them to simulate failures. Each returns NULL / 0 on failure. */
typedef struct {
    void *(*open)(void *ctx, const char *path); /* for writing, binary */
    int (*write)(void *ctx, void *file, const char *data, size_t length);
    int (*close)(void *ctx, void *file); /* includes the final flush */
    int (*rename)(void *ctx, const char *from, const char *to);
    int (*remove)(void *ctx, const char *path);
    void *ctx;
} MflH1FileOps;

/* Exports csv_path and json_path (see the rules above). ops may be NULL.
 * Returns 1 on success; 0 with the reason in msg. */
int mfl_h1_export(const MflH1Result *r, const char *csv_path, const char *json_path,
                  const MflH1Provenance *prov, const MflH1FileOps *ops, char *msg,
                  size_t msg_size);

#endif /* MFL_NUMERICAL_HEAT1D_EXPORT_H */
