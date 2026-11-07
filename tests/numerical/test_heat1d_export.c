/*
 * test_heat1d_export.c - CSV/metadata export of modules/numerical/
 * heat1d_export.c with simulated file-system failures: a write that fails
 * at every possible byte, a failing close (flush), failing renames and
 * opens. After any failure the metadata file (the completion marker) must
 * not exist and no temporary file may be left; after success both files
 * exist and the temporary ones are gone. Also: the CSV uses '.' under a
 * locale with a decimal comma.
 *
 * SPDX-License-Identifier: MIT
 */
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "heat1d.h"
#include "heat1d_export.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        g_checks++;                                                                    \
        if (!(cond)) {                                                                 \
            g_failures++;                                                              \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
        }                                                                              \
    } while (0)

/* ------------------------------------------------------------------------ */
/* In-memory file system with failure injection                             */
/* ------------------------------------------------------------------------ */

#define MAX_FILES 8

typedef struct {
    char   name[256];
    char  *data;
    size_t len;
    int    used;
} MemFile;

typedef struct {
    MemFile files[MAX_FILES];
    long    write_budget;  /* bytes that can still be written; < 0: unlimited */
    int     fail_close;    /* close number (1-based) that fails; 0: none */
    int     fail_rename;   /* rename number (1-based) that fails; 0: none */
    int     fail_open;     /* open number (1-based) that fails; 0: none */
    int     closes;
    int     renames;
    int     opens;
} MemFs;

static MemFile *find(MemFs *fs, const char *name)
{
    int i;
    for (i = 0; i < MAX_FILES; i++) {
        if (fs->files[i].used && strcmp(fs->files[i].name, name) == 0) {
            return &fs->files[i];
        }
    }
    return NULL;
}

static void drop(MemFile *f)
{
    free(f->data);
    memset(f, 0, sizeof *f);
}

static void *m_open(void *ctx, const char *path)
{
    MemFs *fs = (MemFs *)ctx;
    MemFile *f = find(fs, path);
    int i;
    if (++fs->opens == fs->fail_open) {
        return NULL;
    }
    if (f != NULL) {
        drop(f);
    }
    for (i = 0; i < MAX_FILES; i++) {
        if (!fs->files[i].used) {
            fs->files[i].used = 1;
            snprintf(fs->files[i].name, sizeof fs->files[i].name, "%s", path);
            return &fs->files[i];
        }
    }
    return NULL;
}

static int m_write(void *ctx, void *file, const char *data, size_t n)
{
    MemFs *fs = (MemFs *)ctx;
    MemFile *f = (MemFile *)file;
    size_t k = n;
    char *p;
    int ok = 1;
    if (fs->write_budget >= 0 && (long)n > fs->write_budget) {
        k = (size_t)fs->write_budget; /* a partial write, then the failure */
        ok = 0;
    }
    p = (char *)realloc(f->data, f->len + k + 1);
    if (p == NULL) {
        return 0;
    }
    memcpy(p + f->len, data, k);
    f->data = p;
    f->len += k;
    if (fs->write_budget >= 0) {
        fs->write_budget -= (long)k;
    }
    return ok;
}

static int m_close(void *ctx, void *file)
{
    MemFs *fs = (MemFs *)ctx;
    (void)file;
    return ++fs->closes != fs->fail_close;
}

static int m_rename(void *ctx, const char *from, const char *to)
{
    MemFs *fs = (MemFs *)ctx;
    MemFile *f = find(fs, from);
    MemFile *t = find(fs, to);
    if (++fs->renames == fs->fail_rename || f == NULL) {
        return 0;
    }
    if (t != NULL) {
        drop(t);
    }
    snprintf(f->name, sizeof f->name, "%s", to);
    return 1;
}

static int m_remove(void *ctx, const char *path)
{
    MemFile *f = find((MemFs *)ctx, path);
    if (f == NULL) {
        return 0;
    }
    drop(f);
    return 1;
}

static void fs_reset(MemFs *fs)
{
    int i;
    for (i = 0; i < MAX_FILES; i++) {
        if (fs->files[i].used) {
            drop(&fs->files[i]);
        }
    }
    memset(fs, 0, sizeof *fs);
    fs->write_budget = -1;
}

static MflH1FileOps ops_for(MemFs *fs)
{
    MflH1FileOps o;
    o.open = m_open;
    o.write = m_write;
    o.close = m_close;
    o.rename = m_rename;
    o.remove = m_remove;
    o.ctx = fs;
    return o;
}

/* ------------------------------------------------------------------------ */

static void small_run(MflH1Result *r)
{
    MflH1Config c;
    char msg[512];
    mfl_h1_config_init(&c);
    mfl_h1_set_positive(&c, MFL_H1_P_LENGTH, 0.02, "20 mm", msg, sizeof msg);
    mfl_h1_set_positive(&c, MFL_H1_P_CONDUCTIVITY, 45, NULL, msg, sizeof msg);
    mfl_h1_set_positive(&c, MFL_H1_P_DENSITY, 7850, NULL, msg, sizeof msg);
    mfl_h1_set_positive(&c, MFL_H1_P_SPECIFIC_HEAT, 460, NULL, msg, sizeof msg);
    mfl_h1_set_nodes(&c, 5, msg, sizeof msg);
    mfl_h1_set_initial_uniform(&c, 293.15, "20 degC", msg, sizeof msg);
    mfl_h1_set_boundary(&c, MFL_H1_LEFT, MFL_H1_BC_TEMPERATURE, 373.15, "100 degC", msg, sizeof msg);
    mfl_h1_set_boundary(&c, MFL_H1_RIGHT, MFL_H1_BC_INSULATED, 0, NULL, msg, sizeof msg);
    mfl_h1_set_positive(&c, MFL_H1_P_TIME_STEP, 0.5, NULL, msg, sizeof msg);
    mfl_h1_set_positive(&c, MFL_H1_P_END_TIME, 2.25, NULL, msg, sizeof msg);
    CHECK(mfl_h1_run(&c, r, msg, sizeof msg) == MFL_H1_OK);
    mfl_h1_config_free(&c);
}

static const char *const CSV = "out/run.csv";
static const char *const JSON = "out/run.json";

static int exists(MemFs *fs, const char *name) { return find(fs, name) != NULL; }

static void expect_incomplete(MemFs *fs)
{
    CHECK(!exists(fs, JSON));              /* no completion marker */
    CHECK(!exists(fs, "out/run.csv.partial"));
    CHECK(!exists(fs, "out/run.json.partial"));
}

static void test_success(const MflH1Result *r, size_t *total)
{
    MemFs fs;
    MflH1FileOps o;
    char msg[600];
    MemFile *c;
    memset(&fs, 0, sizeof fs);
    fs_reset(&fs);
    o = ops_for(&fs);
    CHECK(mfl_h1_export(r, CSV, JSON, NULL, &o, msg, sizeof msg) == 1);
    CHECK(exists(&fs, CSV) && exists(&fs, JSON));
    CHECK(!exists(&fs, "out/run.csv.partial") && !exists(&fs, "out/run.json.partial"));
    c = find(&fs, CSV);
    /* 6 samples (t = 0, 0.5, 1, 1.5, 2, 2.25) x 5 nodes + header */
    {
        size_t lines = 0;
        size_t i;
        for (i = 0; c != NULL && i < c->len; i++) {
            lines += c->data[i] == '\n';
        }
        CHECK(lines == 31);
        CHECK(c != NULL && strncmp(c->data, "time_s,position_m,temperature_K\n0,0,373.15\n0,0.005,293.15\n", 58) == 0);
        CHECK(c != NULL && strstr(c->data, "\n2.25,0.02,") != NULL); /* last time, last position */
    }
    CHECK(strstr(find(&fs, JSON)->data, "\"rows\": 30") != NULL);
    CHECK(strstr(find(&fs, JSON)->data, "\"export_complete\": true") != NULL);
    CHECK(strstr(find(&fs, JSON)->data, "\"length\": \"20 mm\"") != NULL);
    *total = c != NULL ? c->len + find(&fs, JSON)->len : 0;
    fs_reset(&fs);
}

static void test_write_failures(const MflH1Result *r, size_t total)
{
    MemFs fs;
    MflH1FileOps o;
    char msg[600];
    long budget;
    int failures = 0;
    memset(&fs, 0, sizeof fs);
    /* A failure at every byte of the CSV and of the metadata. */
    for (budget = 0; budget < (long)total; budget++) {
        fs_reset(&fs);
        o = ops_for(&fs);
        fs.write_budget = budget;
        if (mfl_h1_export(r, CSV, JSON, NULL, &o, msg, sizeof msg) == 0) {
            failures++;
            expect_incomplete(&fs);
            CHECK(!exists(&fs, CSV)); /* a partial CSV never gets the final name */
            CHECK(strstr(msg, "cannot write") != NULL);
        }
    }
    CHECK(failures == (int)total); /* every budget below the total size fails */
    fs_reset(&fs);
    o = ops_for(&fs);
    fs.write_budget = (long)total;
    CHECK(mfl_h1_export(r, CSV, JSON, NULL, &o, msg, sizeof msg) == 1); /* exactly enough */
    fs_reset(&fs);
}

static void test_other_failures(const MflH1Result *r)
{
    MemFs fs;
    MflH1FileOps o;
    char msg[600];
    int k;
    memset(&fs, 0, sizeof fs);
    /* close (flush) of the CSV or of the metadata fails */
    for (k = 1; k <= 2; k++) {
        fs_reset(&fs);
        o = ops_for(&fs);
        fs.fail_close = k;
        CHECK(mfl_h1_export(r, CSV, JSON, NULL, &o, msg, sizeof msg) == 0);
        expect_incomplete(&fs);
        CHECK(!exists(&fs, CSV));
    }
    /* rename of the CSV (1) or of the metadata (2) fails */
    for (k = 1; k <= 2; k++) {
        fs_reset(&fs);
        o = ops_for(&fs);
        fs.fail_rename = k;
        CHECK(mfl_h1_export(r, CSV, JSON, NULL, &o, msg, sizeof msg) == 0);
        CHECK(strstr(msg, "cannot rename") != NULL);
        expect_incomplete(&fs);
        CHECK(!exists(&fs, CSV)); /* removed again when its marker cannot be written */
    }
    /* opening either file fails */
    for (k = 1; k <= 2; k++) {
        fs_reset(&fs);
        o = ops_for(&fs);
        fs.fail_open = k;
        CHECK(mfl_h1_export(r, CSV, JSON, NULL, &o, msg, sizeof msg) == 0);
        expect_incomplete(&fs);
    }
    /* an old complete export is invalidated first: after a failed new export
     * its marker is gone, so the stale CSV is not mistaken for the new one */
    fs_reset(&fs);
    o = ops_for(&fs);
    CHECK(mfl_h1_export(r, CSV, JSON, NULL, &o, msg, sizeof msg) == 1);
    fs.write_budget = 10;
    CHECK(mfl_h1_export(r, CSV, JSON, NULL, &o, msg, sizeof msg) == 0);
    CHECK(!exists(&fs, JSON));
    fs_reset(&fs);
    /* invalid calls */
    o = ops_for(&fs);
    CHECK(mfl_h1_export(r, CSV, CSV, NULL, &o, msg, sizeof msg) == 0);
    CHECK(strstr(msg, "must be different files") != NULL);
    CHECK(mfl_h1_export(NULL, CSV, JSON, NULL, &o, msg, sizeof msg) == 0);
    CHECK(mfl_h1_export(r, "", JSON, NULL, &o, msg, sizeof msg) == 0);
    fs_reset(&fs);
}

typedef struct {
    char   buf[8192];
    size_t len;
} Sink;

static int to_sink(void *ctx, const char *d, size_t n)
{
    Sink *s = (Sink *)ctx;
    if (s->len + n >= sizeof s->buf) {
        return 0;
    }
    memcpy(s->buf + s->len, d, n);
    s->len += n;
    s->buf[s->len] = '\0';
    return 1;
}

static void test_locale(const MflH1Result *r)
{
    static const char *const names[] = {"pt_BR.UTF-8", "pt_BR.utf8", "de_DE.UTF-8", "de_DE.utf8",
                                        "Portuguese_Brazil.1252", "German_Germany.1252"};
    Sink a;
    Sink b;
    MflH1CsvInfo ia;
    MflH1CsvInfo ib;
    size_t i;
    const char *got = NULL;
    a.len = b.len = 0;
    CHECK(mfl_h1_write_csv(r, to_sink, &a, &ia));
    for (i = 0; i < sizeof names / sizeof names[0] && got == NULL; i++) {
        got = setlocale(LC_ALL, names[i]);
    }
    if (got == NULL) {
        printf("test_heat1d_export: no decimal-comma locale installed; locale check skipped\n");
        return;
    }
    CHECK(mfl_h1_write_csv(r, to_sink, &b, &ib));
    setlocale(LC_ALL, "C");
    CHECK(a.len == b.len && memcmp(a.buf, b.buf, a.len) == 0 && ia.hash == ib.hash);
    CHECK(strstr(b.buf, "373.15") != NULL);
    printf("test_heat1d_export: CSV identical under locale %s\n", got);
}

int main(void)
{
    MflH1Result r;
    size_t total = 0;
    small_run(&r);
    test_success(&r, &total);
    CHECK(total > 1000);
    test_write_failures(&r, total);
    test_other_failures(&r);
    test_locale(&r);
    mfl_h1_result_free(&r);
    printf("test_heat1d_export: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
