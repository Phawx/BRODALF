/* Moving to a new PC: the catalog is copied from a drive with
 * bd_catalog_recover (catalog.c), then backup roots whose paths do
 * not exist here (another user name, another drive letter) are pointed at
 * where they are now. Only the path changes: the folder keeps its name on
 * the drives, its files keep their versions and copies, and the next scan
 * matches files by their path inside the folder as usual. */
#include "internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

bd_status bd_source_relocate(bd_catalog *cat, int64_t source_id, const char *new_path)
{
    bd_stat_t st;
    if (!new_path || bd_stat(new_path, &st) != 0 || !st.is_dir)
        return bd_fail(cat, BD_ERR_NOT_FOUND, "%s is not a folder", new_path ? new_path : "");

    char *path = bd_strdup(new_path);
    if (!path) return BD_ERR_NOMEM;
    size_t len = strlen(path);
    while (len > 1 && (path[len - 1] == '/' || path[len - 1] == '\\') && !(len == 3 && path[1] == ':')) path[--len] = '\0';

    bd_status s = BD_OK;
    sqlite3_stmt *q;
    int found = 0;
    if (sqlite3_prepare_v2(cat->db, "SELECT id FROM sources WHERE id=? OR path=?", -1, &q, NULL) != SQLITE_OK) {
        free(path);
        return bd_fail_db(cat, "source lookup");
    }
    sqlite3_bind_int64(q, 1, source_id);
    sqlite3_bind_text(q, 2, path, -1, SQLITE_STATIC);
    while (s == BD_OK && sqlite3_step(q) == SQLITE_ROW) {
        if (sqlite3_column_int64(q, 0) == source_id) found = 1;
        else s = bd_fail(cat, BD_ERR_EXISTS, "%s is already a backup root", path);
    }
    sqlite3_finalize(q);
    if (s == BD_OK && !found) s = bd_fail(cat, BD_ERR_NOT_FOUND, "no such backup root");

    if (s == BD_OK) {
        sqlite3_stmt *u;
        if (sqlite3_prepare_v2(cat->db, "UPDATE sources SET path=? WHERE id=?", -1, &u, NULL) != SQLITE_OK) {
            s = bd_fail_db(cat, "change the folder");
        } else {
            sqlite3_bind_text(u, 1, path, -1, SQLITE_STATIC);
            sqlite3_bind_int64(u, 2, source_id);
            if (sqlite3_step(u) != SQLITE_DONE) s = bd_fail_db(cat, "change the folder");
            sqlite3_finalize(u);
        }
    }
    free(path);
    return s;
}

/* ---- Finding the catalog's copies on a drive ------------------------------ */

typedef struct {
    char *best;          /* path of the newest copy */
    int64_t revision;    /* its revision; -1 none yet */
    int64_t mtime_ns;
    const char *folder;
} newest_copy;

/* catalog-r<digits>.brodalf gives its revision, catalog-backup.brodalf 0;
 * -1 for anything else. */
static int64_t copy_revision(const char *name)
{
    static const char pre[] = "catalog-r", post[] = ".brodalf", old[] = "catalog-backup.brodalf";
    size_t n = strlen(name), a = sizeof(pre) - 1, b = sizeof(post) - 1, k;
    if (n == sizeof(old) - 1) {
        for (k = 0; k < n && (name[k] | 0x20) == old[k]; k++) {}
        if (k == n) return 0;
    }
    if (n <= a + b) return -1;
    for (k = 0; k < a; k++) if ((name[k] | 0x20) != pre[k]) return -1;
    for (k = 0; k < b; k++) if ((name[n - b + k] | 0x20) != post[k]) return -1;
    int64_t r = 0;
    for (k = a; k < n - b; k++) {
        if (name[k] < '0' || name[k] > '9' || r > INT64_MAX / 10 - 10) return -1;
        r = r * 10 + (name[k] - '0');
    }
    return r;
}

/* Only the files right in the folder: the backup roots beside them are not
 * walked into. */
static int newest_cb(void *ctx, const char *rel, const bd_stat_t *st)
{
    newest_copy *c = ctx;
    if (st->is_dir) return BD_WALK_SKIP;
    if (!st->is_file || strchr(rel, '/')) return 0;
    int64_t r = copy_revision(rel);
    if (r < 0 || r < c->revision) return 0;
    char *p = bd_path_join(c->folder, rel);
    if (!p) return 0;
    free(c->best);
    c->best = p;
    c->revision = r;
    c->mtime_ns = st->mtime_ns;
    return 0;
}

static void quiet_err(void *ctx, const char *path, const char *message)
{
    (void)ctx;
    (void)path;
    (void)message;
}

/* 1 if fn was called (a copy is in folder), -1 if fn asked to stop. */
static int report_folder(const char *folder, const char *id, bd_catalog_copy_fn fn, void *ctx)
{
    newest_copy c = {NULL, -1, 0, folder};
    bd_walk(folder, newest_cb, quiet_err, &c);
    if (!c.best) return 0;
    bd_catalog_copy_info info = {id, c.best, c.revision, c.mtime_ns / 1000000};
    int stop = fn ? fn(ctx, &info) : 0;
    free(c.best);
    return stop ? -1 : 1;
}

typedef struct {
    char **names;
    size_t n, cap;
} folder_list;

static int folders_cb(void *ctx, const char *rel, const bd_stat_t *st)
{
    folder_list *l = ctx;
    if (!st->is_dir) return 0;
    if (rel[0] != '.' && !strchr(rel, '/') && l->n < 4096) {
        if (l->n == l->cap) {
            size_t cap = l->cap ? l->cap * 2 : 16;
            char **g = realloc(l->names, cap * sizeof(*g));
            if (!g) return BD_WALK_SKIP;
            l->names = g;
            l->cap = cap;
        }
        if ((l->names[l->n] = bd_strdup(rel)) != NULL) l->n++;
    }
    return BD_WALK_SKIP;
}

/* fn for each subfolder of dir that holds a copy. Found count, or -1 if fn stopped. */
static int report_subfolders(const char *dir, bd_catalog_copy_fn fn, void *ctx)
{
    folder_list l = {NULL, 0, 0};
    bd_walk(dir, folders_cb, quiet_err, &l);
    int found = 0;
    for (size_t i = 0; i < l.n; i++) {
        char *sub = found >= 0 ? bd_path_join(dir, l.names[i]) : NULL;
        int r = sub ? report_folder(sub, l.names[i], fn, ctx) : 0;
        if (r < 0) found = -1;
        else if (found >= 0) found += r;
        free(sub);
        free(l.names[i]);
    }
    free(l.names);
    return found;
}

bd_status bd_find_catalog_copies(const char *dir, bd_catalog_copy_fn fn, void *ctx)
{
    bd_stat_t st;
    if (!dir || bd_stat(dir, &st) != 0 || !st.is_dir) return BD_ERR_NOT_FOUND;
    /* The catalog's own folder: named for it. */
    char *name = bd_strdup(dir);
    if (!name) return BD_ERR_NOMEM;
    size_t len = strlen(name);
    while (len > 0 && (name[len - 1] == '/' || name[len - 1] == '\\')) name[--len] = '\0';
    const char *last = name;
    for (const char *p = name; *p; p++) if (*p == '/' || *p == '\\') last = p + 1;
    int r = report_folder(dir, *last ? last : name, fn, ctx);
    free(name);
    if (r) return BD_OK;
    char *sub = bd_path_join(dir, "BRODALF");
    if (!sub) return BD_ERR_NOMEM;
    r = report_subfolders(sub, fn, ctx);
    free(sub);
    if (!r) r = report_subfolders(dir, fn, ctx);
    return r ? BD_OK : BD_ERR_NOT_FOUND;
}
