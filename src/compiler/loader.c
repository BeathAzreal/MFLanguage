/*
 * loader.c - programs made of several files (import "path.mfl"). See
 * loader.h for the rules.
 *
 * SPDX-License-Identifier: MIT
 */
#include "compiler/loader.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { STATE_NEW, STATE_LOADING, STATE_DONE };

typedef struct {
    MflLoadResult  *res;
    MflReadSourceFn read;
    void           *read_ctx;
    size_t          limit;
    MflDiagnostic  *diags; /* loader's own diagnostics (unsorted) */
    char          **keys;  /* normalised path of each file (identity) */
    int            *state;
    int            *order; /* post-order: dependencies first */
    int             order_len;
    int             stack[MFL_LOAD_MAX_DEPTH + 2];
    int             depth;
} Loader;

unsigned long long mfl_fnv1a64(const char *data, size_t length)
{
    unsigned long long h = 14695981039346656037ULL;
    size_t i;
    for (i = 0; i < length; i++) {
        h ^= (unsigned char)data[i];
        h *= 1099511628211ULL;
    }
    return h;
}

int mfl_read_source_file(void *ctx, const char *path, char **data, size_t *length)
{
    FILE *f;
    char *buf = NULL;
    size_t len = 0;
    size_t cap = 0;
    (void)ctx;

    f = fopen(path, "rb");
    if (f == NULL) {
        return 0;
    }
    for (;;) {
        size_t n;
        if (len == cap) {
            size_t new_cap = cap == 0 ? 4096 : cap * 2;
            char *p = (char *)realloc(buf, new_cap);
            if (p == NULL) {
                free(buf);
                fclose(f);
                return 0;
            }
            buf = p;
            cap = new_cap;
        }
        n = fread(buf + len, 1, cap - len, f);
        len += n;
        if (n == 0) {
            break;
        }
    }
    if (ferror(f)) {
        free(buf);
        fclose(f);
        return 0;
    }
    fclose(f);
    *data = buf;
    *length = len;
    return 1;
}

/* ------------------------------------------------------------------------ */
/* Diagnostics                                                               */
/* ------------------------------------------------------------------------ */

static void diag(Loader *L, int file, int line, int column, const char *fmt, ...)
{
    char text[MFL_LOAD_MAX_PATH * 2 + 256];
    va_list ap;
    size_t n;
    MflDiagnostic *d;
    char *msg;

    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    n = strlen(text);
    d = (MflDiagnostic *)mfl_arena_alloc(&L->res->arena, sizeof *d);
    msg = (char *)mfl_arena_alloc(&L->res->arena, n + 1);
    if (d == NULL || msg == NULL) {
        L->res->out_of_memory = 1;
        return;
    }
    memcpy(msg, text, n + 1);
    d->file = file;
    d->line = line;
    d->column = column;
    d->message = msg;
    d->next = L->diags;
    L->diags = d;
}

/* ------------------------------------------------------------------------ */
/* Paths                                                                     */
/* ------------------------------------------------------------------------ */

static int is_sep(char ch) { return ch == '/' || ch == '\\'; }

/* Lexical normalisation: '.' segments dropped, '..' removes the previous
 * segment when there is one. Separators become '/'. Returns 0 if it does
 * not fit. */
static int normalise(const char *in, char *out, size_t size)
{
    char tmp[MFL_LOAD_MAX_PATH + 1];
    const char *segs[MFL_LOAD_MAX_PATH / 2 + 2];
    size_t lens[MFL_LOAD_MAX_PATH / 2 + 2];
    int n = 0;
    size_t prefix = 0;
    size_t pos = 0;
    size_t len = strlen(in);
    size_t i;
    int k;

    if (len > MFL_LOAD_MAX_PATH || size == 0) {
        return 0;
    }
    memcpy(tmp, in, len + 1);
    /* Keep a root: "/", "C:/" or "C:" */
    if (len >= 2 && tmp[1] == ':' && ((tmp[0] >= 'A' && tmp[0] <= 'Z') || (tmp[0] >= 'a' && tmp[0] <= 'z'))) {
        prefix = 2;
    }
    if (prefix < len && is_sep(tmp[prefix])) {
        prefix++;
    }
    i = prefix;
    while (i < len) {
        size_t start = i;
        while (i < len && !is_sep(tmp[i])) {
            i++;
        }
        if (i > start) {
            const char *s = tmp + start;
            size_t sl = i - start;
            if (sl == 1 && s[0] == '.') {
                /* skip */
            } else if (sl == 2 && s[0] == '.' && s[1] == '.') {
                if (n > 0 && !(lens[n - 1] == 2 && segs[n - 1][0] == '.' && segs[n - 1][1] == '.')) {
                    n--;
                } else if (prefix == 0) {
                    segs[n] = s;
                    lens[n] = sl;
                    n++;
                } /* "/.." is "/" */
            } else {
                segs[n] = s;
                lens[n] = sl;
                n++;
            }
        }
        i++;
    }
    if (prefix + 1 > size) {
        return 0;
    }
    for (i = 0; i < prefix; i++) {
        out[pos++] = is_sep(tmp[i]) ? '/' : tmp[i];
    }
    for (k = 0; k < n; k++) {
        if (pos + lens[k] + 2 > size) {
            return 0;
        }
        if (k > 0) {
            out[pos++] = '/';
        }
        memcpy(out + pos, segs[k], lens[k]);
        pos += lens[k];
    }
    if (pos == 0) {
        out[pos++] = '.';
    }
    out[pos] = '\0';
    return 1;
}

int mfl_normalise_path(const char *in, char *out, size_t size)
{
    return normalise(in, out, size);
}

/* Checks the text of an import string (without quotes). Returns NULL if
 * valid, or the reason. */
static const char *check_import_path(const char *p, size_t n)
{
    size_t i;
    if (n == 0) {
        return "the path is empty";
    }
    if (n > MFL_LOAD_MAX_PATH) {
        return "the path is too long";
    }
    for (i = 0; i < n; i++) {
        if (p[i] == '\\') {
            return "use '/' to separate directories (backslashes are not allowed)";
        }
        if ((unsigned char)p[i] < 0x20) {
            return "the path contains a control character";
        }
    }
    if (p[0] == '/' || (n >= 2 && p[1] == ':')) {
        return "absolute paths are not allowed; write the path relative to the importing file";
    }
    for (i = 0; i + 1 < n; i++) {
        if (p[i] == '/' && p[i + 1] == '/') {
            return "the path contains an empty directory name ('//')";
        }
    }
    if (p[n - 1] == '/') {
        return "the path names a directory, not a file";
    }
    if (n < 5 || memcmp(p + n - 4, ".mfl", 4) != 0) {
        return "the file name must end in .mfl";
    }
    return NULL;
}

/* Directory of `name` joined with `rel`, normalised, into out. */
static int resolve(const char *name, const char *rel, size_t rel_len, char *out, size_t size)
{
    char joined[MFL_LOAD_MAX_PATH * 2 + 2];
    size_t dir = 0;
    size_t i;
    for (i = 0; name[i] != '\0'; i++) {
        if (is_sep(name[i])) {
            dir = i + 1;
        }
    }
    if (dir + rel_len + 1 > sizeof joined) {
        return 0;
    }
    memcpy(joined, name, dir);
    memcpy(joined + dir, rel, rel_len);
    joined[dir + rel_len] = '\0';
    return normalise(joined, out, size);
}

/* ------------------------------------------------------------------------ */
/* Loading                                                                   */
/* ------------------------------------------------------------------------ */

static void tag_parse_diagnostics(MflParseResult *pr, int file)
{
    MflDiagnostic *d;
    for (d = pr->diagnostics; d != NULL; d = d->next) {
        d->file = file;
    }
}

static int add_edge(Loader *L, int from, int to)
{
    MflLoadedFile *f = &L->res->files[from];
    int *p;
    int k;
    for (k = 0; k < f->import_count; k++) {
        if (f->imports[k] == to) {
            return 1;
        }
    }
    p = (int *)realloc(f->imports, (size_t)(f->import_count + 1) * sizeof *p);
    if (p == NULL) {
        L->res->out_of_memory = 1;
        return 0;
    }
    f->imports = p;
    f->imports[f->import_count++] = to;
    return 1;
}

static void load_file(Loader *L, int idx);

static void handle_import(Loader *L, int idx, const MflStmt *s)
{
    MflLoadResult *R = L->res;
    const char *text = s->name.start + 1; /* without the quotes */
    size_t n = s->name.length >= 2 ? s->name.length - 2 : 0;
    char path[MFL_LOAD_MAX_PATH + 1];
    const char *why = check_import_path(text, n);
    int k;
    int found = -1;

    if (why != NULL) {
        diag(L, idx, s->line, s->column, "invalid import path \"%.*s\": %s",
             n > 200 ? 200 : (int)n, text, why);
        return;
    }
    if (!resolve(L->keys[idx], text, n, path, sizeof path)) {
        diag(L, idx, s->line, s->column, "import path \"%.*s\" is too long", n > 200 ? 200 : (int)n,
             text);
        return;
    }
    for (k = 0; k < R->file_count; k++) {
        if (strcmp(L->keys[k], path) == 0) {
            found = k;
            break;
        }
    }
    if (found >= 0) {
        if (L->state[found] == STATE_LOADING) {
            char chain[MFL_LOAD_MAX_PATH * 2];
            size_t pos = 0;
            int d;
            int start = 0;
            for (d = 0; d < L->depth; d++) {
                if (L->stack[d] == found) {
                    start = d;
                    break;
                }
            }
            chain[0] = '\0';
            for (d = start; d < L->depth && pos < sizeof chain - 1; d++) {
                int w = snprintf(chain + pos, sizeof chain - pos, "%s -> ", R->files[L->stack[d]].name);
                pos += w > 0 ? (size_t)w : 0;
            }
            if (pos < sizeof chain - 1) {
                snprintf(chain + pos, sizeof chain - pos, "%s", R->files[found].name);
            }
            diag(L, idx, s->line, s->column, "import cycle: %s (a file cannot import itself, "
                                             "directly or through other files)", chain);
            return;
        }
        add_edge(L, idx, found);
        return;
    }
    if (L->read == NULL) {
        diag(L, idx, s->line, s->column,
             "cannot import \"%.*s\": file imports are not enabled here (embedding API: call "
             "mfl_set_file_imports(S, 1))",
             n > 200 ? 200 : (int)n, text);
        return;
    }
    if (R->file_count >= MFL_LOAD_MAX_FILES) {
        diag(L, idx, s->line, s->column, "too many imported files (limit %d)", MFL_LOAD_MAX_FILES);
        return;
    }
    if (L->depth >= MFL_LOAD_MAX_DEPTH) {
        diag(L, idx, s->line, s->column, "imports are nested too deeply (limit %d)",
             MFL_LOAD_MAX_DEPTH);
        return;
    }
    {
        MflLoadedFile *f = &R->files[R->file_count];
        char *data = NULL;
        size_t len = 0;
        if (!L->read(L->read_ctx, path, &data, &len)) {
            diag(L, idx, s->line, s->column, "cannot read the imported file \"%.*s\" (looked for '%s')",
                 n > 200 ? 200 : (int)n, text, path);
            return;
        }
        memset(f, 0, sizeof *f);
        f->name = (char *)malloc(strlen(path) + 1);
        L->keys[R->file_count] = (char *)malloc(strlen(path) + 1);
        if (f->name == NULL || L->keys[R->file_count] == NULL) {
            free(data);
            free(f->name);
            free(L->keys[R->file_count]);
            f->name = NULL;
            L->keys[R->file_count] = NULL;
            R->out_of_memory = 1;
            return;
        }
        strcpy(f->name, path);
        strcpy(L->keys[R->file_count], path);
        f->source = data;
        f->length = len;
        f->owned = 1;
        f->hash = mfl_fnv1a64(data, len);
        L->state[R->file_count] = STATE_NEW;
        k = R->file_count++;
        if (!add_edge(L, idx, k)) {
            return;
        }
        load_file(L, k);
    }
}

static void load_file(Loader *L, int idx)
{
    MflLoadResult *R = L->res;
    const MflStmt *s;

    L->state[idx] = STATE_LOADING;
    L->stack[L->depth++] = idx;
    mfl_parse(R->files[idx].source, R->files[idx].length, L->limit, &R->files[idx].parse);
    if (R->files[idx].parse.out_of_memory) {
        R->out_of_memory = 1;
    }
    tag_parse_diagnostics(&R->files[idx].parse, idx);
    for (s = R->files[idx].parse.program.first; s != NULL && !R->out_of_memory; s = s->next) {
        if (s->kind == MFL_STMT_IMPORT && s->import_path) {
            handle_import(L, idx, s);
        }
    }
    L->depth--;
    L->state[idx] = STATE_DONE;
    L->order[L->order_len++] = idx;
}

static void mark_visible(MflLoadResult *R, int from, int to)
{
    int k;
    int n = R->file_count;
    if (R->visible[from * n + to]) {
        return;
    }
    R->visible[from * n + to] = 1;
    for (k = 0; k < R->files[to].import_count; k++) {
        mark_visible(R, from, R->files[to].imports[k]);
    }
}

static int diag_before(const MflDiagnostic *a, const MflDiagnostic *b)
{
    if (a->file != b->file) {
        return a->file < b->file;
    }
    if (a->line != b->line) {
        return a->line < b->line;
    }
    return a->column < b->column;
}

static void insert_sorted(MflDiagnostic **list, MflDiagnostic *d)
{
    MflDiagnostic **at = list;
    while (*at != NULL && !diag_before(d, *at)) {
        at = &(*at)->next;
    }
    d->next = *at;
    *at = d;
}

int mfl_load(const char *source, size_t length, const char *name, MflReadSourceFn read,
             void *read_ctx, size_t memory_limit, MflLoadResult *out)
{
    Loader L;
    MflStmt *tail = NULL;
    int i;
    int k;

    memset(out, 0, sizeof *out);
    memset(&L, 0, sizeof L);
    mfl_arena_init(&out->arena, memory_limit);
    L.res = out;
    L.read = read;
    L.read_ctx = read_ctx;
    L.limit = memory_limit;
    if (name == NULL) {
        name = "<script>";
    }
    out->files = (MflLoadedFile *)calloc(MFL_LOAD_MAX_FILES, sizeof *out->files);
    L.keys = (char **)calloc(MFL_LOAD_MAX_FILES, sizeof *L.keys);
    L.state = (int *)calloc(MFL_LOAD_MAX_FILES, sizeof *L.state);
    L.order = (int *)calloc(MFL_LOAD_MAX_FILES, sizeof *L.order);
    if (out->files == NULL || L.keys == NULL || L.state == NULL || L.order == NULL) {
        out->out_of_memory = 1;
        goto done;
    }
    out->files[0].name = (char *)malloc(strlen(name) + 1);
    L.keys[0] = (char *)malloc(MFL_LOAD_MAX_PATH + 1);
    if (out->files[0].name == NULL || L.keys[0] == NULL) {
        out->out_of_memory = 1;
        goto done;
    }
    strcpy(out->files[0].name, name);
    if (!normalise(name, L.keys[0], MFL_LOAD_MAX_PATH + 1)) {
        snprintf(L.keys[0], MFL_LOAD_MAX_PATH + 1, "%s", "<main>");
    }
    out->files[0].source = source;
    out->files[0].length = length;
    out->files[0].hash = mfl_fnv1a64(source != NULL ? source : "", source != NULL ? length : 0);
    out->file_count = 1;
    load_file(&L, 0);
    if (out->out_of_memory) {
        goto done;
    }

    /* Visibility: a file sees itself and everything it imports, directly or
     * through other files. */
    out->visible = (unsigned char *)calloc((size_t)out->file_count * (size_t)out->file_count, 1);
    out->names = (const char **)calloc((size_t)out->file_count, sizeof *out->names);
    if (out->visible == NULL || out->names == NULL) {
        out->out_of_memory = 1;
        goto done;
    }
    for (i = 0; i < out->file_count; i++) {
        mark_visible(out, i, i);
        out->names[i] = out->files[i].name;
    }

    /* One program: files in dependency order (main last). */
    for (k = 0; k < L.order_len; k++) {
        MflStmt *s;
        int f = L.order[k];
        for (s = out->files[f].parse.program.first; s != NULL; s = s->next) {
            s->file = f;
            if (tail == NULL) {
                out->program.first = s;
            } else {
                tail->next = s;
            }
            tail = s;
            out->program.count++;
        }
    }
    out->program.file_count = out->file_count;
    out->program.file_names = out->names;
    out->program.visible = out->visible;

done:
    /* Diagnostics of every file and of the loader, sorted. */
    {
        MflDiagnostic *sorted = NULL;
        MflDiagnostic *d;
        MflDiagnostic *next;
        for (d = L.diags; d != NULL; d = next) {
            next = d->next;
            insert_sorted(&sorted, d);
        }
        if (out->files != NULL) {
            for (i = 0; i < out->file_count; i++) {
                for (d = out->files[i].parse.diagnostics; d != NULL; d = next) {
                    next = d->next;
                    insert_sorted(&sorted, d);
                }
                out->files[i].parse.diagnostics = NULL;
            }
        }
        out->diagnostics = sorted;
        for (d = sorted; d != NULL; d = d->next) {
            out->diagnostic_count++;
        }
    }
    if (L.keys != NULL) {
        for (i = 0; i < MFL_LOAD_MAX_FILES; i++) {
            free(L.keys[i]);
        }
    }
    free(L.keys);
    free(L.state);
    free(L.order);
    return !out->out_of_memory && out->diagnostic_count == 0;
}

void mfl_load_free(MflLoadResult *result)
{
    int i;
    if (result->files != NULL) {
        for (i = 0; i < result->file_count; i++) {
            MflLoadedFile *f = &result->files[i];
            mfl_parse_free(&f->parse);
            if (f->owned) {
                free((char *)f->source);
            }
            free(f->name);
            free(f->imports);
        }
    }
    free(result->files);
    free(result->names);
    free(result->visible);
    mfl_arena_free(&result->arena);
    memset(result, 0, sizeof *result);
}
