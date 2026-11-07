/*
 * mfl_wasm.c - MFL in the browser (WebAssembly, target wasm32-wasi,
 * reactor module). Build: wasm/build.sh; JavaScript side: wasm/mfl.js.
 *
 * The same pipeline as the `mfl` command (loader, checker, compiler, VM,
 * the math/numerical/heat modules), with two differences:
 *
 *   - files come from an in-memory file system filled by JavaScript
 *     (mfl_wasm_add_file), so `import "file.mfl"` works without a disk;
 *   - print() output and error messages go to the imported JavaScript
 *     function env.mfl_host_write(stream, text, length) (stream 1:
 *     output, 2: errors), and the results of the last heat.run() are read
 *     directly from WebAssembly memory (times, positions, temperatures)
 *     or as the CSV and JSON text the `mfl` command would export.
 *
 * One module instance holds one program state; it is not shared between
 * threads (the browser runs it on one thread).
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler/checker.h"
#include "compiler/compiler.h"
#include "compiler/loader.h"
#include "heat1d_export.h"
#include "mfl/mfl.h"
#include "modules/natives.h"
#include "vm/vm.h"

#define EXPORT(name) __attribute__((export_name(#name)))

__attribute__((import_module("env"), import_name("mfl_host_write"))) extern void
mfl_host_write(int stream, const char *text, size_t length);

/* ------------------------------------------------------------------------ */
/* In-memory files                                                           */
/* ------------------------------------------------------------------------ */

typedef struct {
    char  *path;
    char  *data;
    size_t length;
} VFile;

static VFile *g_files;
static size_t g_file_count;

static VFile *find_file(const char *path)
{
    size_t i;
    for (i = 0; i < g_file_count; i++) {
        if (strcmp(g_files[i].path, path) == 0) {
            return &g_files[i];
        }
    }
    return NULL;
}

EXPORT(mfl_wasm_alloc) void *mfl_wasm_alloc(size_t n) { return malloc(n ? n : 1); }

EXPORT(mfl_wasm_free) void mfl_wasm_free(void *p) { free(p); }

EXPORT(mfl_wasm_version) const char *mfl_wasm_version(void) { return mfl_version(); }

/* Adds or replaces a file (copied). path is NUL-terminated, data is not.
 * Returns 1 on success, 0 when memory runs out. */
EXPORT(mfl_wasm_add_file) int mfl_wasm_add_file(const char *path, const char *data, size_t length)
{
    VFile *f = find_file(path);
    char *copy = (char *)malloc(length + 1);
    if (copy == NULL) {
        return 0;
    }
    memcpy(copy, data, length);
    copy[length] = '\0';
    if (f == NULL) {
        VFile *p = (VFile *)realloc(g_files, (g_file_count + 1) * sizeof *p);
        char *name = (char *)malloc(strlen(path) + 1);
        if (p == NULL || name == NULL) {
            free(copy);
            free(name);
            if (p != NULL) {
                g_files = p;
            }
            return 0;
        }
        g_files = p;
        strcpy(name, path);
        f = &g_files[g_file_count++];
        f->path = name;
    } else {
        free(f->data);
    }
    f->data = copy;
    f->length = length;
    return 1;
}

EXPORT(mfl_wasm_clear_files) void mfl_wasm_clear_files(void)
{
    size_t i;
    for (i = 0; i < g_file_count; i++) {
        free(g_files[i].path);
        free(g_files[i].data);
    }
    free(g_files);
    g_files = NULL;
    g_file_count = 0;
}

static int read_vfile(void *ctx, const char *path, char **data, size_t *length)
{
    VFile *f = find_file(path);
    char *copy;
    (void)ctx;
    if (f == NULL) {
        return 0;
    }
    copy = (char *)malloc(f->length + 1);
    if (copy == NULL) {
        return 0;
    }
    memcpy(copy, f->data, f->length + 1);
    *data = copy;
    *length = f->length;
    return 1;
}

/* ------------------------------------------------------------------------ */
/* Output                                                                    */
/* ------------------------------------------------------------------------ */

static void write_out(void *ctx, const char *data, size_t length)
{
    (void)ctx;
    mfl_host_write(1, data, length);
}

static void errorf(const char *fmt, const char *a, int line, int col, const char *msg)
{
    char buf[2048];
    int n = snprintf(buf, sizeof buf, fmt, a, line, col, msg);
    if (n > 0) {
        mfl_host_write(2, buf, (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1);
    }
}

static void error_text(const char *text) { mfl_host_write(2, text, strlen(text)); }

static void print_diagnostics(const MflLoadResult *ld, const MflDiagnostic *d)
{
    for (; d != NULL; d = d->next) {
        const char *name = d->file >= 0 && d->file < ld->file_count ? ld->names[d->file] : "?";
        errorf("%s:%d:%d: error: %s\n", name, d->line, d->column, d->message);
    }
}

/* ------------------------------------------------------------------------ */
/* Results of the last run                                                   */
/* ------------------------------------------------------------------------ */

static MflNativeSession g_session;
static int g_has_session;

typedef struct {
    char  *data;
    size_t length;
    size_t cap;
} Text;

static Text g_csv;
static Text g_meta;

static int text_write(void *ctx, const char *data, size_t length)
{
    Text *t = (Text *)ctx;
    if (t->length + length + 1 > t->cap) {
        size_t cap = t->cap ? t->cap : 4096;
        char *p;
        while (cap < t->length + length + 1) {
            cap *= 2;
        }
        p = (char *)realloc(t->data, cap);
        if (p == NULL) {
            return 0;
        }
        t->data = p;
        t->cap = cap;
    }
    memcpy(t->data + t->length, data, length);
    t->length += length;
    t->data[t->length] = '\0';
    return 1;
}

static void text_reset(Text *t)
{
    free(t->data);
    memset(t, 0, sizeof *t);
}

static void clear_results(void)
{
    if (g_has_session) {
        mfl_native_session_free(&g_session);
        g_has_session = 0;
    }
    text_reset(&g_csv);
    text_reset(&g_meta);
}

/* Exports the simulation into g_csv / g_meta, as `mfl --csv` would. */
static void export_texts(const MflLoadResult *ld, const char *csv_name)
{
    MflH1Provenance prov;
    MflH1CsvInfo info;
    unsigned long long hashes[MFL_LOAD_MAX_FILES];
    int i;
    for (i = 0; i < ld->file_count && i < MFL_LOAD_MAX_FILES; i++) {
        hashes[i] = ld->files[i].hash;
    }
    prov.mfl_version = mfl_version();
    prov.files = ld->names;
    prov.hashes = hashes;
    prov.file_count = (size_t)ld->file_count;
    if (!mfl_h1_write_csv(&g_session.result, text_write, &g_csv, &info) ||
        !mfl_h1_write_metadata(&g_session.result, &prov, csv_name, &info, text_write, &g_meta)) {
        text_reset(&g_csv);
        text_reset(&g_meta);
    }
}

/* Runs the file `path` of the in-memory file system, with its imports.
 * Returns the exit status of the `mfl` command: 0 success, 1 errors in the
 * program, 3 internal failure. */
EXPORT(mfl_wasm_run) int mfl_wasm_run(const char *path, unsigned long long max_steps)
{
    VFile *main_file = find_file(path);
    MflLoadResult ld;
    MflCheckResult chk;
    MflCompileResult comp;
    int status = 1;

    clear_results();
    if (main_file == NULL) {
        error_text("mfl: no such file in the browser file system\n");
        return 1;
    }
    if (!mfl_load(main_file->data, main_file->length, path, read_vfile, NULL, 0, &ld)) {
        print_diagnostics(&ld, ld.diagnostics);
        if (ld.out_of_memory) {
            error_text("mfl: out of memory\n");
            status = 3;
        }
        mfl_load_free(&ld);
        return status;
    }
    if (!mfl_check(&ld.program, 0, &chk)) {
        print_diagnostics(&ld, chk.diagnostics);
        status = chk.out_of_memory ? 3 : 1;
        mfl_check_free(&chk);
        mfl_load_free(&ld);
        return status;
    }
    if (!mfl_compile(&ld.program, &chk, 0, &comp)) {
        print_diagnostics(&ld, comp.diagnostics);
        status = comp.out_of_memory ? 3 : 1;
    } else {
        MflVmError err;
        MflVmLimits limits;
        MflVmStatus vs;
        limits.memory_limit = 0;
        limits.max_steps = max_steps;
        mfl_native_session_init(&g_session, 0);
        g_has_session = 1;
        vs = mfl_vm_execute_ex(&comp.chunk, NULL, 0, NULL, write_out, NULL, &limits, &g_session,
                               &err);
        if (vs == MFL_VM_OK) {
            status = 0;
            if (g_session.result.T != NULL) {
                export_texts(&ld, "simulation.csv");
            }
        } else if (vs == MFL_VM_RUNTIME_ERROR) {
            const char *name = err.file >= 0 && err.file < ld.file_count ? ld.names[err.file] : path;
            errorf("%s:%d:%d: runtime error: %s\n", name, err.line, err.column, err.message);
            status = 1;
        } else {
            error_text(vs == MFL_VM_OUT_OF_MEMORY ? "mfl: out of memory\n"
                                                  : "mfl: invalid bytecode\n");
            status = 3;
        }
    }
    mfl_compile_free(&comp);
    mfl_check_free(&chk);
    mfl_load_free(&ld);
    return status;
}

/* Last heat.run() of the last program (0 / NULL when there was none). */
static const MflH1Result *sim(void)
{
    return g_has_session && g_session.result.T != NULL ? &g_session.result : NULL;
}

EXPORT(mfl_wasm_sim_samples) int mfl_wasm_sim_samples(void) { return sim() ? (int)sim()->samples : 0; }
EXPORT(mfl_wasm_sim_nodes) int mfl_wasm_sim_nodes(void) { return sim() ? (int)sim()->nodes : 0; }
EXPORT(mfl_wasm_sim_times) const double *mfl_wasm_sim_times(void) { return sim() ? sim()->times : NULL; }
EXPORT(mfl_wasm_sim_positions) const double *mfl_wasm_sim_positions(void)
{
    return sim() ? sim()->positions : NULL;
}
/* samples x nodes temperatures in K, sample by sample. */
EXPORT(mfl_wasm_sim_temperatures) const double *mfl_wasm_sim_temperatures(void)
{
    return sim() ? sim()->T : NULL;
}
EXPORT(mfl_wasm_sim_csv) const char *mfl_wasm_sim_csv(void) { return g_csv.data; }
EXPORT(mfl_wasm_sim_csv_length) size_t mfl_wasm_sim_csv_length(void) { return g_csv.length; }
EXPORT(mfl_wasm_sim_metadata) const char *mfl_wasm_sim_metadata(void) { return g_meta.data; }
EXPORT(mfl_wasm_sim_metadata_length) size_t mfl_wasm_sim_metadata_length(void) { return g_meta.length; }

/* ------------------------------------------------------------------------ */
/* States (v0.4): the public C API, for hosts that keep a model loaded       */
/* ------------------------------------------------------------------------ */
/*
 * The functions below wrap include/mfl/mfl.h one to one, so a page can do
 * what a C host does: add sources, set named inputs with units, compile a
 * model once, run it many times and read named results in any unit. Each
 * state is independent; print() output goes to env.mfl_host_write(1, ...).
 */

static void state_output(void *user, const char *text, size_t length)
{
    (void)user;
    mfl_host_write(1, text, length);
}

EXPORT(mfl_wasm_state_new) MflState *mfl_wasm_state_new(void)
{
    MflConfig cfg;
    cfg.output = state_output;
    cfg.user = NULL;
    cfg.memory_limit = 0;
    return mfl_state_new(&cfg);
}

EXPORT(mfl_wasm_state_free) void mfl_wasm_state_free(MflState *S) { mfl_state_free(S); }

EXPORT(mfl_wasm_error) const char *mfl_wasm_error(MflState *S) { return mfl_error_message(S); }

EXPORT(mfl_wasm_status_name) const char *mfl_wasm_status_name(int status)
{
    return mfl_status_name((MflStatus)status);
}

EXPORT(mfl_wasm_add_source) int mfl_wasm_add_source(MflState *S, const char *path, const char *text,
                                                    size_t length)
{
    return (int)mfl_add_source(S, path, text, length);
}

EXPORT(mfl_wasm_set_number) int mfl_wasm_set_number(MflState *S, const char *name, double value,
                                                    const char *unit)
{
    return (int)mfl_set_number(S, name, value, unit);
}

EXPORT(mfl_wasm_clear_inputs) void mfl_wasm_clear_inputs(MflState *S) { mfl_clear_inputs(S); }

EXPORT(mfl_wasm_compile) MflScript *mfl_wasm_compile(MflState *S, const char *path)
{
    return mfl_script_compile_source(S, path);
}

EXPORT(mfl_wasm_script_free) void mfl_wasm_script_free(MflScript *sc) { mfl_script_free(sc); }

EXPORT(mfl_wasm_script_run) int mfl_wasm_script_run(MflState *S, MflScript *sc,
                                                    unsigned long long max_steps)
{
    mfl_set_instruction_limit(S, max_steps);
    return (int)mfl_script_run(S, sc);
}

EXPORT(mfl_wasm_get_number) int mfl_wasm_get_number(MflState *S, const char *name, const char *unit,
                                                    double *out)
{
    return (int)mfl_get_number(S, name, unit, out);
}

/* The last heat.run() of the last successful run on S (NULL if none). */
EXPORT(mfl_wasm_state_heat1d) const MflHeat1D *mfl_wasm_state_heat1d(MflState *S)
{
    return mfl_state_heat1d(S);
}

EXPORT(mfl_wasm_h1_samples) int mfl_wasm_h1_samples(const MflHeat1D *h)
{
    return (int)mfl_heat1d_sample_count(h);
}

EXPORT(mfl_wasm_h1_nodes) int mfl_wasm_h1_nodes(const MflHeat1D *h) { return (int)mfl_heat1d_node_count(h); }

EXPORT(mfl_wasm_h1_profile) const double *mfl_wasm_h1_profile(const MflHeat1D *h, int sample)
{
    return mfl_heat1d_profile(h, (size_t)sample);
}

EXPORT(mfl_wasm_h1_time) double mfl_wasm_h1_time(const MflHeat1D *h, int sample)
{
    double t = 0;
    return mfl_heat1d_time(h, (size_t)sample, "s", &t) == MFL_OK ? t : -1;
}

EXPORT(mfl_wasm_h1_position) double mfl_wasm_h1_position(const MflHeat1D *h, int node)
{
    double x = 0;
    return mfl_heat1d_position(h, (size_t)node, "m", &x) == MFL_OK ? x : -1;
}
