/*
 * check_heat_csv.c - independent checker of an exported heat conduction
 * run (CSV + JSON metadata), used by the CTest export tests. It does not
 * use the solver: it reads the files as a user would.
 *
 *   check_heat_csv <file.csv> <file.json> [options]
 *     --sine T0 A tol        compare every row with T0 + A sin(pi x/L) exp(-alpha pi^2 t/L^2)
 *                            (L and alpha read from the metadata); tol in K
 *     --value t x T tol      the row at time t and position x has temperature T (+- tol)
 *     --end T tol            every row at x = 0 has temperature T (prescribed end)
 *
 * Always checked: header, row count (= metadata rows = samples x nodes),
 * order (by time, then position), first time 0 and last time = end_time,
 * uniform positions from 0 to L, byte count and FNV-1a hash against the
 * metadata, and "export_complete": true.
 *
 * SPDX-License-Identifier: MIT
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;

#define FAIL(...)                       \
    do {                                \
        fprintf(stderr, "check_heat_csv: "); \
        fprintf(stderr, __VA_ARGS__);   \
        fprintf(stderr, "\n");          \
        g_fail = 1;                     \
    } while (0)

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    long n;
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc((size_t)n + 1);
    if (buf == NULL || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    buf[n] = '\0';
    *len = (size_t)n;
    return buf;
}

/* Number after "key": inside the object that follows "section" (or the
 * whole text if section is NULL). */
static double json_number(const char *json, const char *section, const char *key)
{
    const char *p = json;
    char pat[128];
    if (section != NULL) {
        snprintf(pat, sizeof pat, "\"%s\": {", section);
        p = strstr(json, pat);
        if (p == NULL) {
            FAIL("metadata has no section \"%s\"", section);
            return NAN;
        }
    }
    snprintf(pat, sizeof pat, "\"%s\": ", key);
    p = strstr(p, pat);
    if (p == NULL) {
        FAIL("metadata has no \"%s\"", key);
        return NAN;
    }
    return strtod(p + strlen(pat), NULL);
}

int main(int argc, char **argv)
{
    size_t clen = 0;
    size_t jlen = 0;
    char *csv;
    char *json;
    const char *p;
    double L;
    double alpha;
    double end_time;
    double rows_meta;
    double nodes;
    double samples;
    size_t rows = 0;
    unsigned long long h = 14695981039346656037ULL;
    size_t i;
    double prev_t = -1;
    size_t in_group = 0;
    double max_sine_err = 0;
    int a;

    if (argc < 3) {
        fprintf(stderr, "usage: check_heat_csv <file.csv> <file.json> [options]\n");
        return 2;
    }
    csv = slurp(argv[1], &clen);
    json = slurp(argv[2], &jlen);
    if (csv == NULL || json == NULL) {
        fprintf(stderr, "check_heat_csv: cannot read %s\n", csv == NULL ? argv[1] : argv[2]);
        return 1;
    }
    if (strstr(json, "\"export_complete\": true") == NULL) {
        FAIL("metadata does not say export_complete: true");
    }
    L = json_number(json, "properties", "length");
    alpha = json_number(json, "properties", "diffusivity");
    end_time = json_number(json, "discretization", "end_time");
    nodes = json_number(json, "discretization", "nodes");
    samples = json_number(json, "sampling", "samples");
    rows_meta = json_number(json, "csv", "rows");
    if (json_number(json, "csv", "bytes") != (double)clen) {
        FAIL("metadata bytes %.0f != CSV size %lu", json_number(json, "csv", "bytes"),
             (unsigned long)clen);
    }
    for (i = 0; i < clen; i++) {
        h ^= (unsigned char)csv[i];
        h *= 1099511628211ULL;
    }
    {
        char hex[32];
        snprintf(hex, sizeof hex, "\"fnv1a64\": \"%016llx\"", h);
        if (strstr(json, hex) == NULL) {
            FAIL("CSV hash %016llx does not match the metadata", h);
        }
    }
    if (strncmp(csv, "time_s,position_m,temperature_K\n", 32) != 0) {
        FAIL("bad header");
    }
    p = strchr(csv, '\n');
    p = p != NULL ? p + 1 : csv + clen;
    while (*p != '\0') {
        char *e;
        double t = strtod(p, &e);
        double x;
        double T;
        if (*e != ',') {
            FAIL("row %lu: bad time", (unsigned long)rows + 1);
            break;
        }
        x = strtod(e + 1, &e);
        if (*e != ',') {
            FAIL("row %lu: bad position", (unsigned long)rows + 1);
            break;
        }
        T = strtod(e + 1, &e);
        if (*e != '\n') {
            FAIL("row %lu: bad temperature or line end", (unsigned long)rows + 1);
            break;
        }
        p = e + 1;
        rows++;
        if (rows == 1 && t != 0) {
            FAIL("first time is %.17g, not 0", t);
        }
        if (t != prev_t) {
            if (prev_t >= 0 && in_group != (size_t)nodes) {
                FAIL("time %.17g has %lu rows, not %.0f", prev_t, (unsigned long)in_group, nodes);
            }
            if (t < prev_t) {
                FAIL("times not increasing at row %lu", (unsigned long)rows);
            }
            in_group = 0;
            prev_t = t;
        }
        /* uniform positions, 0 .. L, in order */
        if (fabs(x - L * (double)in_group / (nodes - 1)) > 1e-12 * L) {
            FAIL("row %lu: position %.17g, expected node %lu of a uniform mesh", (unsigned long)rows,
                 x, (unsigned long)in_group);
        }
        if (in_group == (size_t)nodes - 1 && x != L) {
            FAIL("last position %.17g is not exactly L = %.17g", x, L);
        }
        in_group++;
        for (a = 3; a < argc; a++) {
            if (strcmp(argv[a], "--sine") == 0 && a + 3 < argc) {
                double T0 = atof(argv[a + 1]);
                double A = atof(argv[a + 2]);
                double pi = 3.14159265358979323846;
                double ref = T0 + A * sin(pi * x / L) * exp(-alpha * pi * pi * t / (L * L));
                double err = fabs(T - ref);
                if (err > max_sine_err) {
                    max_sine_err = err;
                }
            } else if (strcmp(argv[a], "--value") == 0 && a + 4 < argc) {
                if (fabs(t - atof(argv[a + 1])) < 1e-9 && fabs(x - atof(argv[a + 2])) < 1e-12 &&
                    fabs(T - atof(argv[a + 3])) > atof(argv[a + 4])) {
                    FAIL("T(t=%s, x=%s) = %.17g, expected %s +- %s", argv[a + 1], argv[a + 2], T,
                         argv[a + 3], argv[a + 4]);
                }
            } else if (strcmp(argv[a], "--end") == 0 && a + 2 < argc) {
                if (x == 0 && fabs(T - atof(argv[a + 1])) > atof(argv[a + 2])) {
                    FAIL("T(t=%.17g, x=0) = %.17g, expected %s", t, T, argv[a + 1]);
                }
            }
        }
    }
    if (in_group != (size_t)nodes) {
        FAIL("last time has %lu rows, not %.0f", (unsigned long)in_group, nodes);
    }
    if (prev_t != end_time) {
        FAIL("last time %.17g is not end_time %.17g", prev_t, end_time);
    }
    if ((double)rows != rows_meta || (double)rows != samples * nodes) {
        FAIL("%lu rows; metadata says %.0f rows, %.0f samples x %.0f nodes", (unsigned long)rows,
             rows_meta, samples, nodes);
    }
    for (a = 3; a < argc; a++) {
        if (strcmp(argv[a], "--sine") == 0 && a + 3 < argc) {
            double tol = atof(argv[a + 3]);
            printf("max |T - analytical| = %.3e K (tolerance %g K)\n", max_sine_err, tol);
            if (!(max_sine_err <= tol)) {
                FAIL("analytical error %.3e K above the tolerance %g K", max_sine_err, tol);
            }
        }
    }
    printf("%s: %lu rows, %.0f samples x %.0f nodes, t = 0 .. %.17g s, x = 0 .. %.17g m: %s\n",
           argv[1], (unsigned long)rows, samples, nodes, end_time, L, g_fail ? "FAILED" : "ok");
    free(csv);
    free(json);
    return g_fail ? 1 : 0;
}
