/*
 * heat_bar_host.c - a C host program running the same case as
 *
 *     mfl examples/thermal/bar_heated_end.mfl --csv bar_heated_end.csv
 *
 * through the public API only (include/mfl/mfl.h), in three ways:
 *
 *   A. the .mfl model run by the host (mfl_eval); its last heat.run() is
 *      read back with mfl_state_heat1d();
 *   B. the same bar configured directly from C (mfl_heat1d_*);
 *   C. optionally, the CSV exported by the CLI (path given as argument).
 *
 * It prints the final profile of each and checks that they agree within
 * TOLERANCE_K. All three use the same solver (modules/numerical/heat1d.c)
 * with the same doubles, so the expected difference is exactly 0 K; the
 * tolerance only allows for a CSV produced on another machine (the CSV
 * stores the shortest decimal that reads back to the same double, so it
 * round-trips exactly as well).
 *
 * It also shows error handling: an unstable time step is reported to the
 * host, which fixes it and runs again in the same state.
 *
 * Usage: mfl_heat_host [bar_heated_end.mfl [bar_heated_end.csv]]
 * Exit status: 0 if every comparison is within the tolerance.
 *
 * SPDX-License-Identifier: MIT
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mfl/mfl.h"

#define TOLERANCE_K 1e-9 /* documented in docs/heat-conduction.md, section 9 */

static void discard(void *user, const char *text, size_t length)
{
    (void)user;
    (void)text;
    (void)length;
}

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    char *buf = NULL;
    size_t cap = 0;
    size_t n;
    *len = 0;
    if (f == NULL) {
        return NULL;
    }
    for (;;) {
        if (*len + 4096 + 1 > cap) {
            char *p;
            cap = cap ? cap * 2 : 8192;
            p = (char *)realloc(buf, cap);
            if (p == NULL) {
                free(buf);
                fclose(f);
                return NULL;
            }
            buf = p;
        }
        n = fread(buf + *len, 1, 4096, f);
        *len += n;
        if (n == 0) {
            break;
        }
    }
    fclose(f);
    buf[*len] = '\0';
    return buf;
}

/* Final profile (rows with the last time) from a CLI CSV. Returns the
 * number of nodes read, 0 on failure. */
static size_t csv_final_profile(const char *path, double *T, size_t max)
{
    size_t len;
    char *csv = read_file(path, &len);
    const char *p;
    double last_t = -1;
    size_t n = 0;
    if (csv == NULL) {
        return 0;
    }
    p = strchr(csv, '\n'); /* skip the header */
    while (p != NULL && p[1] != '\0') {
        char *e;
        double t = strtod(p + 1, &e);
        double x = strtod(e + 1, &e);
        double k = strtod(e + 1, &e);
        (void)x;
        if (t != last_t) {
            last_t = t;
            n = 0;
        }
        if (n < max) {
            T[n++] = k;
        }
        p = strchr(e, '\n');
    }
    free(csv);
    return n;
}

#define CHECK_OK(call)                                                                   \
    do {                                                                                 \
        MflStatus st_ = (call);                                                          \
        if (st_ != MFL_OK) {                                                             \
            fprintf(stderr, "%s failed: %s: %s\n", #call, mfl_status_name(st_),          \
                    mfl_error_message(S));                                               \
            return 1;                                                                    \
        }                                                                                \
    } while (0)

int main(int argc, char **argv)
{
    const char *model_path = argc > 1 ? argv[1] : "examples/thermal/bar_heated_end.mfl";
    const char *csv_path = argc > 2 ? argv[2] : NULL;
    MflConfig cfg;
    MflState *S;
    MflHeat1D *h;
    const MflHeat1D *a;
    char *model;
    size_t len;
    size_t j;
    size_t nodes;
    size_t last;
    double cli[64];
    size_t cli_nodes = 0;
    double max_ab = 0;
    double max_ac = 0;
    double t_hit = 0;
    int reached = 0;
    int ok = 1;

    printf("MFL %s: 1D heat conduction from a C host\n", mfl_version());
    cfg.output = discard; /* the model's print() output is not needed here */
    cfg.user = NULL;
    cfg.memory_limit = 0;
    S = mfl_state_new(&cfg);
    if (S == NULL) {
        return 1;
    }

    /* A. The .mfl model, with its import of materials/carbon_steel.mfl. */
    model = read_file(model_path, &len);
    if (model == NULL) {
        fprintf(stderr, "cannot read %s\n", model_path);
        mfl_state_free(S);
        return 1;
    }
    mfl_set_file_imports(S, 1);
    CHECK_OK(mfl_eval(S, model, len, model_path));
    free(model);
    a = mfl_state_heat1d(S);
    if (a == NULL) {
        fprintf(stderr, "the model did not run a simulation\n");
        return 1;
    }

    /* B. The same bar from C. */
    h = mfl_heat1d_new(S);
    if (h == NULL) {
        return 1;
    }
    CHECK_OK(mfl_heat1d_set(h, "length", 100, "mm"));
    CHECK_OK(mfl_heat1d_set(h, "conductivity", 45, "W/(m*K)"));
    CHECK_OK(mfl_heat1d_set(h, "density", 7850, "kg/m^3"));
    CHECK_OK(mfl_heat1d_set(h, "specific_heat", 460, "J/(kg*K)"));
    CHECK_OK(mfl_heat1d_set_nodes(h, 21));
    CHECK_OK(mfl_heat1d_set_initial_temperature(h, 20, "degC"));
    CHECK_OK(mfl_heat1d_set_boundary_temperature(h, MFL_SIDE_LEFT, 500, "degC"));
    CHECK_OK(mfl_heat1d_set_boundary_insulated(h, MFL_SIDE_RIGHT));
    CHECK_OK(mfl_heat1d_set(h, "end_time", 10, "min"));
    CHECK_OK(mfl_heat1d_set_sample_every(h, 20));

    /* An unstable step is reported to the host, which can fix it. */
    CHECK_OK(mfl_heat1d_set(h, "time_step", 2, "s"));
    if (mfl_heat1d_run(h) != MFL_ERR_SIMULATION) {
        fprintf(stderr, "expected MFL_ERR_SIMULATION for a 2 s time step\n");
        return 1;
    }
    printf("time_step = 2 s rejected: %.70s...\n", mfl_error_message(S));
    CHECK_OK(mfl_heat1d_set(h, "time_step", 0.5, "s"));
    CHECK_OK(mfl_heat1d_run(h));

    nodes = mfl_heat1d_node_count(h);
    last = mfl_heat1d_sample_count(h) - 1;
    if (nodes != mfl_heat1d_node_count(a) || last + 1 != mfl_heat1d_sample_count(a)) {
        fprintf(stderr, "different sizes: %lu x %lu vs %lu x %lu\n", (unsigned long)(last + 1),
                (unsigned long)nodes, (unsigned long)mfl_heat1d_sample_count(a),
                (unsigned long)mfl_heat1d_node_count(a));
        return 1;
    }
    if (csv_path != NULL) {
        cli_nodes = csv_final_profile(csv_path, cli, sizeof cli / sizeof cli[0]);
        if (cli_nodes != nodes) {
            fprintf(stderr, "cannot read %lu nodes from %s\n", (unsigned long)nodes, csv_path);
            return 1;
        }
    }

    {
        double t_end;
        CHECK_OK(mfl_heat1d_time(h, last, "min", &t_end));
        printf("final profile at t = %g min (%lu samples x %lu nodes)\n", t_end,
               (unsigned long)(last + 1), (unsigned long)nodes);
    }
    printf("  x [mm]   C API [degC]   model [degC]%s\n", csv_path != NULL ? "     CLI CSV [degC]" : "");
    for (j = 0; j < nodes; j++) {
        double x;
        double tb;
        double ta;
        CHECK_OK(mfl_heat1d_position(h, j, "mm", &x));
        CHECK_OK(mfl_heat1d_temperature(h, last, j, "degC", &tb));
        CHECK_OK(mfl_heat1d_temperature(a, last, j, "degC", &ta));
        if (fabs(mfl_heat1d_profile(a, last)[j] - mfl_heat1d_profile(h, last)[j]) > max_ab) {
            max_ab = fabs(mfl_heat1d_profile(a, last)[j] - mfl_heat1d_profile(h, last)[j]);
        }
        if (csv_path != NULL && fabs(cli[j] - mfl_heat1d_profile(h, last)[j]) > max_ac) {
            max_ac = fabs(cli[j] - mfl_heat1d_profile(h, last)[j]);
        }
        if (j % 4 == 0) {
            printf("  %6.1f   %12.6f   %12.6f", x, tb, ta);
            if (csv_path != NULL) {
                printf("   %16.6f", cli[j] - 273.15);
            }
            printf("\n");
        }
    }
    CHECK_OK(mfl_heat1d_first_time_reaching(h, 10, 150, "degC", "s", &t_hit, &reached));
    if (reached) {
        printf("center (node 10) reaches 150 degC at the sample t = %g s\n", t_hit);
    }
    CHECK_OK(mfl_heat1d_first_time_reaching(h, 10, 600, "degC", "s", &t_hit, &reached));
    printf("center reaches 600 degC: %s\n", reached ? "yes" : "no, not within the simulated time");

    printf("max |C API - model| = %g K (tolerance %g K)\n", max_ab, TOLERANCE_K);
    ok = max_ab <= TOLERANCE_K;
    if (csv_path != NULL) {
        printf("max |C API - CLI CSV| = %g K (tolerance %g K)\n", max_ac, TOLERANCE_K);
        ok = ok && max_ac <= TOLERANCE_K;
    }
    printf("%s\n", ok ? "same results: OK" : "RESULTS DIFFER");

    mfl_heat1d_free(h); /* host-owned: before the state */
    mfl_state_free(S);  /* also frees the model's simulation (a) */
    return ok ? 0 : 1;
}
