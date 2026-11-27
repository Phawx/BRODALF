/* Backup roots that moved, and a database started again from a drive.
 *
 * What a backup root is, is its folder structure: the files the catalog
 * records at their paths inside it. Where the folder is (sources.path) is
 * only where it was last, so a root that is not there is looked for by its
 * files (bd_source_find) and pointed at where it is now
 * (bd_source_relocate). Only the path changes: the folder keeps its name on
 * the drives, its files keep their versions and copies, and the next scan
 * matches files by their path inside the folder as usual. Nothing ties a
 * catalog to the computer it is opened on.
 *
 * A database is started again from a drive's copy with bd_catalog_recover
 * (catalog.c). */
#include "internal.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

bd_status bd_source_relocate(bd_catalog *cat, int64_t source_id, const char *new_path)
{
    bd_stat_t st;
    if (!new_path || bd_stat(new_path, &st) != 0 || !st.is_dir)
        return bd_fail(cat, BD_ERR_NOT_FOUND, "%s is not a folder", new_path ? new_path : "");

    char *path = bd_full_path(new_path);
    if (!path) return BD_ERR_NOMEM;
    size_t len = strlen(path);
    while (len > 1 && (path[len - 1] == '/' || path[len - 1] == '\\') && !(len == 3 && path[1] == ':')) path[--len] = '\0';

    bd_status s = BD_OK;
    sqlite3_stmt *q;
    int found = 0;
    int active = 0;
    if (sqlite3_prepare_v2(cat->db, "SELECT active FROM sources WHERE id=?", -1, &q, NULL) != SQLITE_OK) {
        free(path);
        return bd_fail_db(cat, "source lookup");
    }
    sqlite3_bind_int64(q, 1, source_id);
    if (sqlite3_step(q) == SQLITE_ROW) {
        found = 1;
        active = sqlite3_column_int(q, 0);
    }
    sqlite3_finalize(q);
    if (!found) s = bd_fail(cat, BD_ERR_NOT_FOUND, "no such backup root");
    /* No other root may be this folder, however it is reached, and a root
     * that is backed up may not end up inside another or holding one. */
    int64_t same = 0;
    if (s == BD_OK) s = bd_root_check(cat, path, source_id, active, &same);
    if (s == BD_OK && same) s = bd_fail(cat, BD_ERR_EXISTS, "%s is already a backup root", path);

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
    /* The files there are other files on disk, with change times of their
     * own: the next scan records them rather than reading every file again.
     * Where the root was before is no longer worth a word. */
    if (s == BD_OK) {
        sqlite3_stmt *d;
        if (sqlite3_prepare_v2(cat->db, "DELETE FROM file_change_times WHERE node_id IN (SELECT id FROM nodes WHERE source_id=?)",
                               -1, &d, NULL) != SQLITE_OK) {
            s = bd_fail_db(cat, "change the folder");
        } else {
            sqlite3_bind_int64(d, 1, source_id);
            if (sqlite3_step(d) != SQLITE_DONE) s = bd_fail_db(cat, "change the folder");
            sqlite3_finalize(d);
        }
    }
    if (s == BD_OK) {
        char key[48];
        snprintf(key, sizeof(key), "moved_from.%lld", (long long)source_id);
        if (bd_setting_del(cat, key) != 0) s = bd_fail_db(cat, "change the folder");
    }
    free(path);
    return s;
}

/* ---- Finding a backup root by its files -------------------------------- */

static int is_sep(char c) { return c == '/' || c == '\\'; }

static int same_char(char a, char b, int win)
{
    if (!win) return a == b;
    if (is_sep(a) && is_sep(b)) return 1;
    return tolower((unsigned char)a) == tolower((unsigned char)b);
}

/* 1 if path names folder, or something inside it. */
static int under(const char *path, const char *folder, size_t len, int win)
{
    for (size_t i = 0; i < len; i++)
        if (!path[i] || !same_char(path[i], folder[i], win)) return 0;
    return path[len] == '\0' || is_sep(path[len]);
}

static int same_path(const char *a, const char *b, int win)
{
    size_t la = strlen(a), lb = strlen(b);
    while (la > 1 && is_sep(a[la - 1])) la--;
    while (lb > 1 && is_sep(b[lb - 1])) lb--;
    if (la != lb) return 0;
    for (size_t i = 0; i < la; i++)
        if (!same_char(a[i], b[i], win)) return 0;
    return 1;
}

typedef struct { char **v; size_t n, cap; int failed; } path_list;

/* Takes p; leaves out path itself and what is listed already. */
static void list_add(path_list *l, char *p, const char *path, int win)
{
    if (!p) { l->failed = 1; return; }
    int dup = same_path(p, path, win);
    for (size_t i = 0; !dup && i < l->n; i++) dup = same_path(p, l->v[i], win);
    if (dup) { free(p); return; }
    if (l->n + 2 > l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 8;
        char **g = realloc(l->v, cap * sizeof(*g));
        if (!g) { free(p); l->failed = 1; return; }
        l->v = g;
        l->cap = cap;
    }
    l->v[l->n++] = p;
}

/* The part of path after the folder named by its first len bytes and one
 * more name, starting at a separator ("" when nothing follows); NULL when
 * no name follows. */
static const char *after_name(const char *path, size_t len)
{
    const char *p = path + len;
    if (!is_sep(*p)) return NULL;
    p++;
    if (!*p || is_sep(*p)) return NULL;
    while (*p && !is_sep(*p)) p++;
    return p;
}

char **bd_root_candidates(const char *path, const char *home, unsigned drives, int win)
{
    path_list l = {0};
    if (!path) path = "";
    /* Under another user's home folder: the same folder under this one's. */
    size_t hl = home ? strlen(home) : 0;
    while (hl > 1 && is_sep(home[hl - 1])) hl--;
    size_t pl = hl;
    while (pl > 0 && !is_sep(home[pl - 1])) pl--;
    const char *user = home ? home + pl : "";
    size_t ul = hl - pl;
    if (pl > 1) pl--; /* the separator */
    else pl = 0;
    if (home && pl > 0 && ul > 0) {
        /* On Windows the folder holding the home folders is on any drive. */
        int letters = win && pl >= 2 && home[1] == ':' && path[0] && path[1] == ':';
        size_t from = letters ? 2 : 0;
        if (under(path + from, home + from, pl - from, win)) {
            const char *rest = after_name(path, pl);
            if (rest) list_add(&l, bd_sprintf("%.*s%s", (int)hl, home, rest), path, win);
        }
    }
    /* A disk mounted for another user: mounted for this one. */
    if (!win && ul > 0) {
        static const char *const media[] = {"/media", "/run/media"};
        for (int i = 0; i < 2; i++) {
            size_t ml = strlen(media[i]);
            if (!under(path, media[i], ml, 0)) continue;
            const char *rest = after_name(path, ml);
            if (!rest || !*rest) continue;
            for (int j = 0; j < 2; j++) list_add(&l, bd_sprintf("%s/%.*s%s", media[j], (int)ul, user, rest), path, win);
        }
    }
    /* Another drive letter. */
    if (win && isalpha((unsigned char)path[0]) && path[1] == ':' && (path[2] == '\0' || is_sep(path[2]))) {
        int own = toupper((unsigned char)path[0]) - 'A';
        for (int i = 2; i < 26; i++)
            if ((drives & (1u << i)) && i != own) list_add(&l, bd_sprintf("%c:%s", 'A' + i, path[2] ? path + 2 : "\\"), path, win);
    }
    if (!l.v && !l.failed) {
        l.v = malloc(sizeof(*l.v));
        if (!l.v) l.failed = 1;
    }
    if (l.failed) {
        for (size_t i = 0; i < l.n; i++) free(l.v[i]);
        free(l.v);
        return NULL;
    }
    l.v[l.n] = NULL;
    return l.v;
}

/* This user's home folder, malloc'd; NULL if not known. */
static char *home_folder(void)
{
#ifdef _WIN32
    wchar_t buf[1024];
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", buf, 1024);
    if (n == 0 || n >= 1024) return NULL;
    char out[3072];
    if (WideCharToMultiByte(CP_UTF8, 0, buf, -1, out, sizeof(out), NULL, NULL) <= 0) return NULL;
    return bd_strdup(out);
#else
    const char *v = getenv("HOME");
    return v && *v ? bd_strdup(v) : NULL;
#endif
}

/* How many of the files the catalog knows in a root are in folder, at
 * their path in the root and with their size. checked gets how many were
 * looked at: up to 64 files with contents, picked by checksum so that the
 * same ones are looked at each time; a root with none is checked by the
 * names of its folders and empty files. */
static int64_t files_there(bd_catalog *cat, int64_t source_id, const char *folder, int64_t *checked)
{
    static const char *const sql[2] = {
        "SELECT n.rel_path, n.is_dir, n.size FROM nodes n JOIN versions v ON v.id=n.current_version_id"
        " WHERE n.source_id=? AND n.is_dir=0 AND n.deleted=0 AND n.size>0 ORDER BY v.hash, n.id LIMIT 64",
        "SELECT rel_path, is_dir, size FROM nodes WHERE source_id=? AND deleted=0 AND rel_path<>'' ORDER BY id LIMIT 64",
    };
    int64_t there = 0;
    *checked = 0;
    for (int k = 0; k < 2 && *checked == 0; k++) {
        sqlite3_stmt *q;
        if (sqlite3_prepare_v2(cat->db, sql[k], -1, &q, NULL) != SQLITE_OK) return 0;
        sqlite3_bind_int64(q, 1, source_id);
        while (sqlite3_step(q) == SQLITE_ROW) {
            const char *rel = (const char *)sqlite3_column_text(q, 0);
            int is_dir = sqlite3_column_int(q, 1);
            int64_t size = sqlite3_column_int64(q, 2);
            (*checked)++;
            char *p = rel ? bd_path_join(folder, rel) : NULL;
            bd_stat_t st;
            if (p && bd_stat(p, &st) == 0 && !st.is_link && st.is_dir == is_dir && (is_dir || size <= 0 || st.size == size))
                there++;
            free(p);
        }
        sqlite3_finalize(q);
    }
    return there;
}

/* The folders that hold a root's files, by the rule bd_source_find gives. */
typedef struct {
    char *path;
    int64_t found, checked;
} place;

typedef struct {
    place *v;
    size_t n, cap;
    int failed;
} place_list;

static void places_free(place_list *l)
{
    for (size_t i = 0; i < l->n; i++) free(l->v[i].path);
    free(l->v);
    memset(l, 0, sizeof(*l));
}

/* Every folder that holds the root's files, the one holding the most
 * first (as listed, among those holding as many). 0, or -1 when out of
 * memory. */
static int find_places(bd_catalog *cat, int64_t source_id, place_list *out)
{
    memset(out, 0, sizeof(*out));
    char *path = NULL;
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(cat->db, "SELECT path FROM sources WHERE id=?", -1, &q, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(q, 1, source_id);
        if (sqlite3_step(q) == SQLITE_ROW && sqlite3_column_text(q, 0))
            path = bd_strdup((const char *)sqlite3_column_text(q, 0));
        sqlite3_finalize(q);
    }
    char *home = path ? home_folder() : NULL;
#ifdef _WIN32
    const int win = 1;
    char **cand = path ? bd_root_candidates(path, home, bd_drive_letters(), 1) : NULL;
#else
    const int win = 0;
    char **cand = path ? bd_root_candidates(path, home, 0, 0) : NULL;
#endif
    /* A root kept as it was typed, from the current folder (by 0.13.0 and
     * older): from the database's folder, and from the current one. */
    char *rel[2] = {NULL, NULL};
    if (path && !bd_path_is_full(path)) {
        char *db = cat->path ? bd_full_path(cat->path) : NULL;
        size_t dl = db ? strlen(db) : 0;
        while (dl > 0 && !is_sep(db[dl - 1])) dl--;
        if (db && dl > 0) {
            db[dl] = '\0';
            char *j = bd_path_join(db, path);
            rel[0] = j ? bd_full_path(j) : NULL;
            free(j);
        }
        free(db);
        rel[1] = bd_full_path(path);
        if (rel[0] && rel[1] && same_path(rel[0], rel[1], win)) {
            free(rel[1]);
            rel[1] = NULL;
        }
    }
    size_t nc = 0;
    while (cand && cand[nc]) nc++;
    for (size_t i = 0; i < nc + 2 && !out->failed; i++) {
        const char *c = i < nc ? cand[i] : rel[i - nc];
        if (!c) continue;
        bd_stat_t st;
        if (bd_stat(c, &st) != 0 || !st.is_dir || st.is_link) continue;
        if (bd_root_overlaps(cat, c, source_id)) continue;
        int64_t ck = 0, th = files_there(cat, source_id, c, &ck);
        if (ck == 0 || th == 0 || th * 2 < ck) continue;
        if (out->n == out->cap) {
            size_t cap = out->cap ? out->cap * 2 : 8;
            place *v = realloc(out->v, cap * sizeof(*v));
            if (!v) { out->failed = 1; break; }
            out->v = v;
            out->cap = cap;
        }
        char *dup = bd_strdup(c);
        if (!dup) { out->failed = 1; break; }
        /* Kept in order, most found first. */
        size_t at = out->n;
        while (at > 0 && out->v[at - 1].found < th) at--;
        memmove(out->v + at + 1, out->v + at, (out->n - at) * sizeof(*out->v));
        out->v[at].path = dup;
        out->v[at].found = th;
        out->v[at].checked = ck;
        out->n++;
    }
    for (size_t i = 0; i < nc; i++) free(cand[i]);
    free(cand);
    free(rel[0]);
    free(rel[1]);
    free(home);
    free(path);
    if (out->failed) {
        places_free(out);
        return -1;
    }
    return 0;
}

char *bd_source_find(bd_catalog *cat, int64_t source_id, int64_t *found, int64_t *checked, int *places)
{
    place_list l;
    char *best = NULL;
    if (find_places(cat, source_id, &l) == 0 && l.n > 0) {
        best = l.v[0].path;
        l.v[0].path = NULL;
    }
    if (found) *found = best ? l.v[0].found : 0;
    if (checked) *checked = best ? l.v[0].checked : 0;
    if (places) *places = best ? (int)l.n : 0;
    places_free(&l);
    return best;
}

int bd_source_find_all(bd_catalog *cat, int64_t source_id, bd_place_fn fn, void *ctx)
{
    place_list l;
    if (find_places(cat, source_id, &l) != 0) return -1;
    int n = 0;
    for (size_t i = 0; i < l.n; i++) {
        bd_place_info p = {l.v[i].path, l.v[i].found, l.v[i].checked};
        n++;
        if (fn && fn(ctx, &p)) break;
    }
    places_free(&l);
    return n;
}

/* A sure match, for a root pointed at a folder without asking: of the
 * files checked (the same up to 64 as files_there), at least 15 of 16 are
 * in folder at their path with their size, and the smallest few of those
 * (up to SURE_READ) hold the contents the catalog has for them. */
#define SURE_READ 4
int bd_source_place_sure(bd_catalog *cat, int64_t source_id, const char *folder, int64_t *found, int64_t *checked)
{
    sqlite3_stmt *q;
    *found = *checked = 0;
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT n.rel_path, n.size, v.hash FROM nodes n JOIN versions v ON v.id=n.current_version_id"
                           " WHERE n.source_id=? AND n.is_dir=0 AND n.deleted=0 AND n.size>0 ORDER BY v.hash, n.id LIMIT 64",
                           -1, &q, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_int64(q, 1, source_id);
    struct { char *path; int64_t size; char hash[BD_HASH_HEX_LEN + 1]; } read[SURE_READ];
    int nread = 0;
    while (sqlite3_step(q) == SQLITE_ROW) {
        const char *rel = (const char *)sqlite3_column_text(q, 0);
        const char *hash = (const char *)sqlite3_column_text(q, 2);
        int64_t size = sqlite3_column_int64(q, 1);
        (*checked)++;
        char *p = rel ? bd_path_join(folder, rel) : NULL;
        bd_stat_t st;
        if (!p || !hash || bd_stat(p, &st) != 0 || st.is_link || !st.is_file || st.size != size) {
            free(p);
            continue;
        }
        (*found)++;
        /* Keep the smallest, to be read. */
        int at = nread;
        if (nread == SURE_READ) {
            at = 0;
            for (int i = 1; i < nread; i++)
                if (read[i].size > read[at].size) at = i;
            if (read[at].size <= size) at = -1;
            else free(read[at].path);
        } else {
            nread++;
        }
        if (at < 0) {
            free(p);
            continue;
        }
        read[at].path = p;
        read[at].size = size;
        snprintf(read[at].hash, sizeof(read[at].hash), "%s", hash);
    }
    sqlite3_finalize(q);
    int sure = *checked > 0 && *found * 16 >= *checked * 15 && nread > 0;
    for (int i = 0; i < nread; i++) {
        char hex[BD_HASH_HEX_LEN + 1];
        int64_t got = 0;
        if (sure && (bd_hash_file(read[i].path, NULL, hex, &got) != 0 || strcmp(hex, read[i].hash) != 0)) sure = 0;
        free(read[i].path);
    }
    return sure;
}

char *bd_source_old_place(bd_catalog *cat, int64_t source_id, int *back)
{
    if (back) *back = 0;
    char key[48], *val = NULL;
    snprintf(key, sizeof(key), "moved_from.%lld", (long long)source_id);
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(cat->db, "SELECT value FROM settings WHERE key=?", -1, &q, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_text(q, 1, key, -1, SQLITE_STATIC);
    if (sqlite3_step(q) == SQLITE_ROW && sqlite3_column_text(q, 0) && *sqlite3_column_text(q, 0))
        val = bd_strdup((const char *)sqlite3_column_text(q, 0));
    sqlite3_finalize(q);
    if (!val) return NULL;
    if (back) {
        bd_stat_t st;
        int64_t ck = 0, th = 0;
        if (bd_stat(val, &st) == 0 && st.is_dir && !st.is_link && !bd_root_overlaps(cat, val, source_id)) {
            th = files_there(cat, source_id, val, &ck);
            *back = ck > 0 && th > 0 && th * 2 >= ck;
        }
    }
    return val;
}

bd_status bd_source_keep_place(bd_catalog *cat, int64_t source_id)
{
    char key[48];
    snprintf(key, sizeof(key), "moved_from.%lld", (long long)source_id);
    return bd_setting_del(cat, key) == 0 ? BD_OK : bd_fail_db(cat, "keep the backup root where it is");
}

/* ---- Finding the catalog's copies on a drive ------------------------------ */

typedef struct {
    char *best;          /* path of the newest copy */
    int64_t revision;    /* its revision; -1 none yet */
    int64_t mtime_ns;
    const char *folder;
} newest_copy;

/* catalog-r<digits>.brodalf gives its revision, catalog-backup.brodalf 0;
 * -1 for anything else. catalog-r<digits>-<hex>.brodalf is a second copy
 * of that revision, from another copy of the database that was used apart
 * from this one (BRODALF 0.14.0 and older pass such names by). */
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
    size_t end = n - b, dash = end;
    for (k = a; k < end; k++) {
        if (name[k] == '-' && k > a && dash == end) { dash = k; continue; }
        if (dash < end) {
            int c = name[k] | 0x20;
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) || k - dash > 16) return -1;
            continue;
        }
        if (name[k] < '0' || name[k] > '9' || r > INT64_MAX / 10 - 10) return -1;
        r = r * 10 + (name[k] - '0');
    }
    if (dash == end - 1) return -1; /* nothing after the dash */
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
    /* Two copies of one revision: the one written last. */
    if (r < 0 || r < c->revision || (r == c->revision && st->mtime_ns < c->mtime_ns)) return 0;
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

typedef struct {
    struct rev { char *path; int64_t revision, mtime_ns; } *items;
    size_t n, cap;
    const char *folder;
} rev_list;

static int revs_cb(void *ctx, const char *rel, const bd_stat_t *st)
{
    rev_list *l = ctx;
    if (st->is_dir) return BD_WALK_SKIP;
    if (!st->is_file || strchr(rel, '/') || l->n >= 100000) return 0;
    int64_t r = copy_revision(rel);
    if (r < 0) return 0;
    if (l->n == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 32;
        struct rev *g = realloc(l->items, cap * sizeof(*g));
        if (!g) return BD_WALK_SKIP;
        l->items = g;
        l->cap = cap;
    }
    char *p = bd_path_join(l->folder, rel);
    if (!p) return 0;
    l->items[l->n].path = p;
    l->items[l->n].revision = r;
    l->items[l->n].mtime_ns = st->mtime_ns;
    l->n++;
    return 0;
}

static int rev_newer_first(const void *a, const void *b)
{
    const struct rev *x = a, *y = b;
    if (x->revision != y->revision) return x->revision > y->revision ? -1 : 1;
    return x->mtime_ns > y->mtime_ns ? -1 : x->mtime_ns < y->mtime_ns ? 1 : 0;
}

bd_status bd_find_catalog_revisions(const char *folder, bd_catalog_copy_fn fn, void *ctx)
{
    rev_list l = {NULL, 0, 0, folder};
    bd_walk(folder, revs_cb, quiet_err, &l);
    if (l.n) qsort(l.items, l.n, sizeof(l.items[0]), rev_newer_first);
    char *name = bd_strdup(folder);
    const char *last = name ? name : "";
    if (name) {
        size_t len = strlen(name);
        while (len > 0 && (name[len - 1] == '/' || name[len - 1] == '\\')) name[--len] = '\0';
        for (const char *p = name; *p; p++) if (*p == '/' || *p == '\\') last = p + 1;
    }
    int stop = 0;
    for (size_t i = 0; i < l.n; i++) {
        bd_catalog_copy_info info = {last, l.items[i].path, l.items[i].revision, l.items[i].mtime_ns / 1000000};
        if (!stop && fn && fn(ctx, &info)) stop = 1;
        free(l.items[i].path);
    }
    size_t n = l.n;
    free(l.items);
    free(name);
    return n ? BD_OK : BD_ERR_NOT_FOUND;
}

int bd_path_is_drive_copy(const char *path)
{
    if (!path) return 0;
    const char *name = path + strlen(path);
    while (name > path && name[-1] != '/' && name[-1] != '\\') name--;
    if (copy_revision(name) >= 0) return 1;
    /* .../BRODALF/<36-character catalog id>/... */
    for (const char *p = path; *p; p++) {
        if (p != path && p[-1] != '/' && p[-1] != '\\') continue;
        static const char dir[] = "brodalf";
        size_t k = 0;
        while (k < 7 && p[k] && (p[k] | 0x20) == dir[k]) k++;
        if (k < 7 || (p[7] != '/' && p[7] != '\\')) continue;
        const char *id = p + 8;
        size_t n = 0;
        while (id[n] && id[n] != '/' && id[n] != '\\') n++;
        if (n == 36 && id[8] == '-' && id[13] == '-' && id[18] == '-' && id[23] == '-' && id[n]) return 1;
    }
    return 0;
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

/* ---- Paths a catalog stores ------------------------------------------- */

int bd_stored_path_safe_as(const char *rel, int windows)
{
    if (!rel || !*rel || rel[0] == '/') return 0; /* empty, or absolute */
    const char *part = rel;
    for (const char *p = rel;; p++) {
        if (*p == '/' || *p == '\0') {
            size_t n = (size_t)(p - part);
            if (n == 0) return 0; /* an empty part: "a//b", or a slash at the end */
            if ((n == 1 && part[0] == '.') || (n == 2 && part[0] == '.' && part[1] == '.')) return 0;
            if (windows) {
                /* Windows reads a part of only dots and spaces as "." or "..". */
                size_t k = 0;
                while (k < n && (part[k] == '.' || part[k] == ' ')) k++;
                if (k == n) return 0;
            }
            if (!*p) return 1;
            part = p + 1;
        } else if (windows && (*p == ':' || *p == '\\')) {
            return 0; /* a drive ("C:"), a stream ("a:b"), or a second kind of separator */
        }
    }
}

int bd_stored_path_safe(const char *rel)
{
#ifdef _WIN32
    return bd_stored_path_safe_as(rel, 1);
#else
    return bd_stored_path_safe_as(rel, 0);
#endif
}

int bd_stored_name_safe(const char *name)
{
    return name && !strchr(name, '/') && bd_stored_path_safe(name);
}

bd_status bd_check_stored_paths(bd_catalog *cat, bd_log_fn log, void *log_ctx, int64_t *unsafe)
{
    if (unsafe) *unsafe = 0;
    sqlite3_stmt *q;
    /* 0: a backup root's name, 1: a file's or folder's path in its root,
     * 2: where a copy is on its drive. */
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT 0, name, name FROM sources"
                           " UNION ALL SELECT 1, n.rel_path, s.name || '/' || n.rel_path FROM nodes n JOIN sources s ON s.id=n.source_id"
                           " UNION ALL SELECT 2, c.path_on_media, m.label FROM copies c JOIN media m ON m.id=c.media_id",
                           -1, &q, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "look through the stored paths");
    int64_t n = 0;
    while (sqlite3_step(q) == SQLITE_ROW) {
        int kind = sqlite3_column_int(q, 0);
        const char *path = (const char *)sqlite3_column_text(q, 1);
        const char *shown = (const char *)sqlite3_column_text(q, 2);
        if (kind == 0 ? bd_stored_name_safe(path) : bd_stored_path_safe(path)) continue;
        if (++n <= 20) {
            if (kind == 0) bd_logf(log, log_ctx, "the backup root name \"%s\" is not a safe folder name: its files will not be restored", path ? path : "");
            else if (kind == 1) bd_logf(log, log_ctx, "the stored path \"%s\" could lead outside a restore folder: it will not be restored", shown ? shown : "");
            else bd_logf(log, log_ctx, "a copy on \"%s\" is recorded at \"%s\", which could lead outside its folder: it will not be read", shown ? shown : "", path ? path : "");
        }
    }
    sqlite3_finalize(q);
    if (n > 20) bd_logf(log, log_ctx, "and %lld more paths like those", (long long)(n - 20));
    if (unsafe) *unsafe = n;
    return BD_OK;
}
