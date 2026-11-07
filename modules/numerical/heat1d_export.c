/*
 * heat1d_export.c - CSV and JSON metadata of a heat conduction run. See
 * heat1d_export.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "heat1d_export.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime/number.h"

/* Buffered output with byte count, hash and sticky failure. */
typedef struct {
    MflH1WriteFn       write;
    void              *ctx;
    char               buf[16384];
    size_t             len;
    size_t             bytes;
    unsigned long long hash;
    int                failed;
} Out;

static void out_init(Out *o, MflH1WriteFn write, void *ctx)
{
    o->write = write;
    o->ctx = ctx;
    o->len = 0;
    o->bytes = 0;
    o->hash = 14695981039346656037ULL;
    o->failed = 0;
}

static void out_flush(Out *o)
{
    if (!o->failed && o->len > 0 && !o->write(o->ctx, o->buf, o->len)) {
        o->failed = 1;
    }
    o->len = 0;
}

static void out_put(Out *o, const char *s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        o->hash ^= (unsigned char)s[i];
        o->hash *= 1099511628211ULL;
    }
    o->bytes += n;
    while (n > 0 && !o->failed) {
        size_t room = sizeof o->buf - o->len;
        size_t k = n < room ? n : room;
        memcpy(o->buf + o->len, s, k);
        o->len += k;
        s += k;
        n -= k;
        if (o->len == sizeof o->buf) {
            out_flush(o);
        }
    }
}

static void out_str(Out *o, const char *s) { out_put(o, s, strlen(s)); }

static void out_num(Out *o, double x)
{
    char b[MFL_NUMBER_BUF];
    mfl_number_format(x, b, sizeof b);
    out_str(o, b);
}

static void out_size(Out *o, size_t n)
{
    char b[32];
    snprintf(b, sizeof b, "%lu", (unsigned long)n);
    out_str(o, b);
}

/* JSON string with escapes. */
static void out_json_str(Out *o, const char *s)
{
    out_str(o, "\"");
    for (; *s != '\0'; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') {
            char e[2] = {'\\', (char)ch};
            out_put(o, e, 2);
        } else if (ch < 0x20) {
            char e[8];
            snprintf(e, sizeof e, "\\u%04x", ch);
            out_str(o, e);
        } else {
            out_put(o, s, 1);
        }
    }
    out_str(o, "\"");
}

static void out_hash(Out *o, unsigned long long h)
{
    char b[24];
    snprintf(b, sizeof b, "\"%016llx\"", h);
    out_str(o, b);
}

int mfl_h1_write_csv(const MflH1Result *r, MflH1WriteFn write, void *ctx, MflH1CsvInfo *info)
{
    Out *o = (Out *)malloc(sizeof *o);
    size_t s;
    size_t j;
    int ok;
    if (o == NULL) {
        return 0;
    }
    out_init(o, write, ctx);
    out_str(o, "time_s,position_m,temperature_K\n");
    for (s = 0; s < r->samples && !o->failed; s++) {
        char t[MFL_NUMBER_BUF];
        mfl_number_format(r->times[s], t, sizeof t);
        for (j = 0; j < r->nodes; j++) {
            out_str(o, t);
            out_str(o, ",");
            out_num(o, r->positions[j]);
            out_str(o, ",");
            out_num(o, r->T[s * r->nodes + j]);
            out_str(o, "\n");
        }
    }
    out_flush(o);
    ok = !o->failed;
    if (info != NULL) {
        info->rows = r->samples * r->nodes;
        info->bytes = o->bytes;
        info->hash = o->hash;
    }
    free(o);
    return ok;
}

static void key(Out *o, int indent, const char *k)
{
    static const char spaces[] = "        ";
    out_put(o, spaces, (size_t)indent * 2 < sizeof spaces - 1 ? (size_t)indent * 2 : sizeof spaces - 1);
    out_json_str(o, k);
    out_str(o, ": ");
}

static void kv_num(Out *o, int indent, const char *k, double v, int last)
{
    key(o, indent, k);
    out_num(o, v);
    out_str(o, last ? "\n" : ",\n");
}

static void kv_size(Out *o, int indent, const char *k, size_t v, int last)
{
    key(o, indent, k);
    out_size(o, v);
    out_str(o, last ? "\n" : ",\n");
}

static void kv_str(Out *o, int indent, const char *k, const char *v, int last)
{
    key(o, indent, k);
    out_json_str(o, v);
    out_str(o, last ? "\n" : ",\n");
}

static void boundary(Out *o, const MflH1Config *c, MflH1Side side, int last)
{
    key(o, 2, side == MFL_H1_LEFT ? "left" : "right");
    if (c->boundary[side] == MFL_H1_BC_TEMPERATURE) {
        out_str(o, "{\"kind\": \"temperature\", \"temperature\": ");
        out_num(o, c->boundary_temperature[side]);
        out_str(o, ", \"position\": ");
        out_num(o, side == MFL_H1_LEFT ? 0.0 : c->length);
        out_str(o, "}");
    } else if (c->boundary[side] == MFL_H1_BC_CONVECTION) {
        out_str(o, "{\"kind\": \"convection\", \"coefficient\": ");
        out_num(o, c->boundary_coefficient[side]);
        out_str(o, ", \"ambient_temperature\": ");
        out_num(o, c->boundary_temperature[side]);
        out_str(o, ", \"coefficient_unit\": \"W/(m^2*K)\", \"ambient_temperature_unit\": \"K\"");
        out_str(o, ", \"position\": ");
        out_num(o, side == MFL_H1_LEFT ? 0.0 : c->length);
        out_str(o, "}");
    } else {
        out_str(o, "{\"kind\": \"insulated\", \"position\": ");
        out_num(o, side == MFL_H1_LEFT ? 0.0 : c->length);
        out_str(o, "}");
    }
    out_str(o, last ? "\n" : ",\n");
}

int mfl_h1_write_metadata(const MflH1Result *r, const MflH1Provenance *prov, const char *csv_file,
                          const MflH1CsvInfo *csv, MflH1WriteFn write, void *ctx)
{
    const MflH1Config *c = &r->config;
    Out *o = (Out *)malloc(sizeof *o);
    size_t i;
    int ok;
    int p;
    if (o == NULL) {
        return 0;
    }
    out_init(o, write, ctx);
    out_str(o, "{\n");
    kv_str(o, 1, "format", "mfl-heat1d-metadata", 0);
    kv_num(o, 1, "format_version", 1, 0);
    kv_str(o, 1, "mfl_version", prov != NULL && prov->mfl_version != NULL ? prov->mfl_version : "", 0);
    kv_str(o, 1, "model",
           "transient heat conduction in a homogeneous 1D bar with constant properties: "
           "rho*cp*dT/dt = k*d2T/dx2, alpha = k/(rho*cp)", 0);
    kv_str(o, 1, "method",
           c->method == MFL_H1_BACKWARD_EULER
           ? "backward Euler on a uniform mesh; tridiagonal solve; Robin end half-volumes"
           : (c->boundary[0] == MFL_H1_BC_CONVECTION || c->boundary[1] == MFL_H1_BC_CONVECTION)
           ? "explicit finite differences (FTCS) on a uniform mesh; Robin end half-volumes; "
             "time_step <= dx^2/(2 alpha (1 + h dx/k)) at convective ends"
           : "explicit finite differences (FTCS) on a uniform mesh; insulated ends by a mirror "
           "node; stable for Fourier number alpha*dt/dx^2 <= 1/2", 0);
    kv_str(o, 1, "scope",
           "verification of a 1D bar model; moving heat sources and 2D/3D welding simulation are "
           "outside this version", 0);
    key(o, 1, "units");
    out_str(o, "{\"time\": \"s\", \"position\": \"m\", \"temperature\": \"K\", \"length\": \"m\", "
               "\"conductivity\": \"W/(m*K)\", \"density\": \"kg/m^3\", \"specific_heat\": "
               "\"J/(kg*K)\", \"diffusivity\": \"m^2/s\", \"temperatures_are\": \"absolute\"},\n");

    key(o, 1, "properties");
    out_str(o, "{\n");
    kv_num(o, 2, "length", c->length, 0);
    kv_num(o, 2, "conductivity", c->conductivity, 0);
    kv_num(o, 2, "density", c->density, 0);
    kv_num(o, 2, "specific_heat", c->specific_heat, 0);
    kv_num(o, 2, "diffusivity", r->alpha, 1);
    out_str(o, "  },\n");

    key(o, 1, "as_given");
    out_str(o, "{\n");
    {
        int first = 1;
        for (p = 0; p < MFL_H1_P_COUNT_; p++) {
            if (c->given[p][0] == '\0') {
                continue;
            }
            if (!first) {
                out_str(o, ",\n");
            }
            first = 0;
            key(o, 2, mfl_h1_param_name((MflH1Param)p));
            out_json_str(o, c->given[p]);
        }
        out_str(o, "\n  },\n");
    }

    key(o, 1, "initial_condition");
    if (c->initial == MFL_H1_INIT_UNIFORM) {
        out_str(o, "{\"kind\": \"uniform\", \"temperature\": ");
        out_num(o, c->initial_uniform);
        out_str(o, "},\n");
    } else {
        out_str(o, "{\"kind\": \"profile\", \"note\": \"one value per node, in node order; the "
                   "data are recorded here\", \"temperature\": [");
        for (i = 0; i < c->initial_count; i++) {
            if (i > 0) {
                out_str(o, ", ");
            }
            out_num(o, c->initial_profile[i]);
        }
        out_str(o, "]},\n");
    }

    key(o, 1, "boundaries");
    out_str(o, "{\n");
    boundary(o, c, MFL_H1_LEFT, 0);
    boundary(o, c, MFL_H1_RIGHT, 0);
    kv_str(o, 2, "at_initial_time",
           "a prescribed temperature replaces the initial value of its end node at t = 0", 1);
    out_str(o, "  },\n");

    key(o, 1, "discretization");
    out_str(o, "{\n");
    kv_size(o, 2, "nodes", r->nodes, 0);
    kv_num(o, 2, "dx", r->dx, 0);
    kv_num(o, 2, "time_step", c->time_step, 0);
    kv_num(o, 2, "end_time", c->end_time, 0);
    kv_size(o, 2, "steps", r->steps, 0);
    kv_num(o, 2, "last_step", r->last_step, 0);
    kv_num(o, 2, "fourier_number", r->fourier, 0);
    if (c->method == MFL_H1_BACKWARD_EULER) {
        key(o, 2, "max_stable_time_step");
        out_str(o, "null,\n");
        kv_num(o, 2, "ftcs_time_step_limit", r->max_time_step, 0);
    } else {
        kv_num(o, 2, "max_stable_time_step", r->max_time_step, 0);
    }
    kv_str(o, 2, "end_time_rule",
           "full steps of time_step, then one shorter step ending exactly at end_time when it "
           "falls between steps", 1);
    out_str(o, "  },\n");

    key(o, 1, "sampling");
    out_str(o, "{\n");
    kv_size(o, 2, "every_steps", c->sample_every != 0 ? c->sample_every : 1, 0);
    kv_size(o, 2, "samples", r->samples, 0);
    kv_str(o, 2, "rule", "the initial state, every n-th step and always the final state", 1);
    out_str(o, "  },\n");

    key(o, 1, "csv");
    out_str(o, "{\n");
    kv_str(o, 2, "file", csv_file != NULL ? csv_file : "", 0);
    key(o, 2, "columns");
    out_str(o, "[\"time_s\", \"position_m\", \"temperature_K\"],\n");
    kv_str(o, 2, "order", "by time, then by position", 0);
    kv_size(o, 2, "rows", csv != NULL ? csv->rows : 0, 0);
    kv_size(o, 2, "bytes", csv != NULL ? csv->bytes : 0, 0);
    key(o, 2, "fnv1a64");
    out_hash(o, csv != NULL ? csv->hash : 0);
    out_str(o, "\n  },\n");

    key(o, 1, "sources");
    out_str(o, "[");
    if (prov != NULL) {
        for (i = 0; i < prov->file_count; i++) {
            out_str(o, i > 0 ? ",\n    " : "\n    ");
            out_str(o, "{\"file\": ");
            out_json_str(o, prov->files[i]);
            out_str(o, ", \"fnv1a64\": ");
            out_hash(o, prov->hashes != NULL ? prov->hashes[i] : 0);
            out_str(o, "}");
        }
        if (prov->file_count > 0) {
            out_str(o, "\n  ");
        }
    }
    out_str(o, "],\n");
    kv_str(o, 1, "completeness",
           "this file is written last, after the CSV is complete; an export without it is "
           "incomplete", 0);
    key(o, 1, "export_complete");
    out_str(o, "true\n}\n");
    out_flush(o);
    ok = !o->failed;
    free(o);
    return ok;
}

/* ------------------------------------------------------------------------ */
/* Files                                                                     */
/* ------------------------------------------------------------------------ */

static void *std_open(void *ctx, const char *path)
{
    (void)ctx;
    return fopen(path, "wb");
}

static int std_write(void *ctx, void *f, const char *data, size_t n)
{
    (void)ctx;
    return fwrite(data, 1, n, (FILE *)f) == n;
}

static int std_close(void *ctx, void *f)
{
    int ok;
    (void)ctx;
    ok = fflush((FILE *)f) == 0 && !ferror((FILE *)f);
    return fclose((FILE *)f) == 0 && ok;
}

/* 1 if path is an existing regular file (not a directory). Portable C:
 * a directory either cannot be opened (Windows) or fails on the first read
 * (POSIX). */
static int is_regular_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    int bad;
    if (f == NULL) {
        return 0;
    }
    (void)fgetc(f);
    bad = ferror(f);
    fclose(f);
    return !bad;
}

/* Removes regular files only: remove() would also delete an empty
 * directory on POSIX systems. */
static int std_remove(void *ctx, const char *path)
{
    (void)ctx;
    return is_regular_file(path) && remove(path) == 0;
}

static int std_rename(void *ctx, const char *from, const char *to)
{
    (void)ctx;
    if (rename(from, to) == 0) { /* POSIX: replaces an existing file atomically */
        return 1;
    }
#ifdef _WIN32
    /* Windows: rename() does not replace an existing file. */
    if (is_regular_file(to) && remove(to) == 0 && rename(from, to) == 0) {
        return 1;
    }
#endif
    return 0;
}

typedef struct {
    const MflH1FileOps *ops;
    void               *file;
} FileSink;

static int file_sink(void *ctx, const char *data, size_t n)
{
    FileSink *s = (FileSink *)ctx;
    return s->ops->write(s->ops->ctx, s->file, data, n);
}

static const char *base_name(const char *path)
{
    const char *b = path;
    const char *p;
    for (p = path; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') {
            b = p + 1;
        }
    }
    return b;
}

static int fail(char *msg, size_t size, const char *fmt, const char *a, const char *b)
{
    if (msg != NULL && size > 0) {
        snprintf(msg, size, fmt, a, b);
    }
    return 0;
}

/* Writes one file through ops under `partial`. csv: write the CSV and set
 * info; else the metadata. */
static int write_file(const MflH1Result *r, const MflH1FileOps *ops, const char *partial, int csv,
                      const MflH1Provenance *prov, const char *csv_file, MflH1CsvInfo *info)
{
    FileSink sink;
    int ok;
    sink.ops = ops;
    sink.file = ops->open(ops->ctx, partial);
    if (sink.file == NULL) {
        return 0;
    }
    ok = csv ? mfl_h1_write_csv(r, file_sink, &sink, info)
             : mfl_h1_write_metadata(r, prov, csv_file, info, file_sink, &sink);
    if (!ops->close(ops->ctx, sink.file)) {
        ok = 0;
    }
    return ok;
}

int mfl_h1_export(const MflH1Result *r, const char *csv_path, const char *json_path,
                  const MflH1Provenance *prov, const MflH1FileOps *user_ops, char *msg,
                  size_t msg_size)
{
    MflH1FileOps ops;
    MflH1CsvInfo info;
    char *csv_tmp = NULL;
    char *json_tmp = NULL;
    int ok = 0;

    if (r == NULL || r->T == NULL || csv_path == NULL || json_path == NULL || csv_path[0] == '\0' ||
        json_path[0] == '\0') {
        return fail(msg, msg_size, "%s%s", "nothing to export (no simulation results or no file name)",
                    "");
    }
    if (strcmp(csv_path, json_path) == 0) {
        return fail(msg, msg_size, "the CSV and the metadata file must be different files ('%s')%s",
                    csv_path, "");
    }
    memset(&ops, 0, sizeof ops);
    if (user_ops != NULL) {
        ops = *user_ops;
    }
    if (ops.open == NULL) {
        ops.open = std_open;
    }
    if (ops.write == NULL) {
        ops.write = std_write;
    }
    if (ops.close == NULL) {
        ops.close = std_close;
    }
    if (ops.rename == NULL) {
        ops.rename = std_rename;
    }
    if (ops.remove == NULL) {
        ops.remove = std_remove;
    }
    csv_tmp = (char *)malloc(strlen(csv_path) + 9);
    json_tmp = (char *)malloc(strlen(json_path) + 9);
    if (csv_tmp == NULL || json_tmp == NULL) {
        free(csv_tmp);
        free(json_tmp);
        return fail(msg, msg_size, "out of memory while exporting '%s'%s", csv_path, "");
    }
    sprintf(csv_tmp, "%s.partial", csv_path);
    sprintf(json_tmp, "%s.partial", json_path);

    /* 1. The old marker goes first: from here on, no metadata = incomplete. */
    ops.remove(ops.ctx, json_path);
    /* 2. Both files under temporary names. */
    if (!write_file(r, &ops, csv_tmp, 1, prov, NULL, &info)) {
        fail(msg, msg_size, "cannot write '%s' (while exporting '%s')", csv_tmp, csv_path);
        goto cleanup;
    }
    if (!write_file(r, &ops, json_tmp, 0, prov, base_name(csv_path), &info)) {
        fail(msg, msg_size, "cannot write '%s' (while exporting '%s')", json_tmp, json_path);
        goto cleanup;
    }
    /* 3. CSV, then the marker. */
    if (!ops.rename(ops.ctx, csv_tmp, csv_path)) {
        fail(msg, msg_size, "cannot rename '%s' to '%s'", csv_tmp, csv_path);
        goto cleanup;
    }
    if (!ops.rename(ops.ctx, json_tmp, json_path)) {
        fail(msg, msg_size, "cannot rename '%s' to '%s'", json_tmp, json_path);
        ops.remove(ops.ctx, csv_path); /* without its marker the CSV would look complete */
        goto cleanup;
    }
    ok = 1;

cleanup:
    if (!ok) {
        ops.remove(ops.ctx, csv_tmp);
        ops.remove(ops.ctx, json_tmp);
    }
    free(csv_tmp);
    free(json_tmp);
    return ok;
}
