/* The list of newest copies written beside each database copy on a drive.
 *
 * A plain drive can be read without BRODALF: each backup root has a folder
 * there. That folder holds the first copy made of each file, though. A file
 * that changed has its newer copies under .versions, a moved or renamed
 * file keeps its old name, a file in a folder set to be encrypted is a
 * sealed .bdenc, and deleted files are mixed in with the rest. So every
 * database copy gets a plain text file beside it, newest-copies-r<N>.txt,
 * naming each file whose newest copy on that drive is not in its own place,
 * and where that copy is. Like the database copies, a new one is written
 * every time and none is ever overwritten.
 *
 * Drives that are encrypted or hide names get none: their copies can be
 * read only with BRODALF, and on a drive that hides names a list of names
 * would undo the hiding. */
#include "store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LIST_PREFIX "newest-copies-r"
#define LIST_SUFFIX ".txt"

int bd_newest_list_name(const char *name)
{
    size_t a = strlen(LIST_PREFIX), b = strlen(LIST_SUFFIX), n = strlen(name), tl = strlen(BD_TMP_MARKER), k;
    if (n > tl && strcmp(name + n - tl, BD_TMP_MARKER) == 0) n -= tl;
    if (n <= a + b) return 0;
    for (k = 0; k < a; k++) if ((name[k] | 0x20) != LIST_PREFIX[k]) return 0;
    for (k = 0; k < b; k++) if ((name[n - b + k] | 0x20) != LIST_SUFFIX[k]) return 0;
    /* r<digits>, then perhaps "-" and letters and digits, to go with a
     * database copy whose name has such a tag. */
    int digits = 0, tag = 0, dash = 0;
    for (k = a; k < n - b; k++) {
        char c = name[k];
        int alnum = (c >= '0' && c <= '9') || ((c | 0x20) >= 'a' && (c | 0x20) <= 'z');
        if (!dash && c >= '0' && c <= '9') digits++;
        else if (!dash && c == '-' && digits) dash = 1;
        else if (dash && alnum) tag++;
        else return 0;
    }
    return digits > 0 && (!dash || tag > 0);
}

static void put_time(FILE *f, int64_t ms)
{
    time_t t = (time_t)(ms / 1000);
    struct tm tm;
#ifdef _WIN32
    if (localtime_s(&tm, &t) != 0) { fputs("unknown", f); return; }
#else
    if (!localtime_r(&t, &tm)) { fputs("unknown", f); return; }
#endif
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
    fputs(buf, f);
}

/* A '/'-separated path as the PC writes it. Tabs and line breaks, which a
 * file name can hold on some systems, are shown as spaces so each file
 * stays on its own line. */
static void put_path(FILE *f, const char *p)
{
    for (; *p; p++) {
#ifdef _WIN32
        if (*p == '/') { fputc('\\', f); continue; }
#endif
        fputc(*p == '\t' || *p == '\r' || *p == '\n' ? ' ' : *p, f);
    }
}

typedef struct {
    int64_t id;
    char *name, *path;
    int active;
    int64_t files, here; /* current files; how many have a copy on this drive */
} list_root;

static void free_roots(list_root *r, size_t n)
{
    for (size_t i = 0; i < n; i++) { free(r[i].name); free(r[i].path); }
    free(r);
}

static void root_heading(FILE *f, const list_root *r)
{
    fputs("\r\n== ", f);
    put_path(f, r->name);
    fputs(" (from ", f);
    put_path(f, r->path);
    fputs(r->active ? ")" : ", no longer backed up)", f);
    fprintf(f, ": %lld file%s, %lld with a copy on this drive ==\r\n", (long long)r->files, r->files == 1 ? "" : "s",
            (long long)r->here);
}

static void root_end(FILE *f, const list_root *r, int listed)
{
    if (!listed && r->here) fputs("Every copy is in its own place.\r\n", f);
    int64_t missing = r->files - r->here;
    if (missing > 0)
        fprintf(f, "%lld file%s ha%s no copy on this drive: look on your other drives.\r\n", (long long)missing,
                missing == 1 ? "" : "s", missing == 1 ? "s" : "ve");
}

/* Writes the list into f. */
static bd_status write_list(bd_catalog *cat, int64_t media_id, FILE *f)
{
    bd_status s = BD_OK;
    sqlite3_stmt *q = NULL;
    list_root *roots = NULL;
    size_t nroots = 0, cap = 0;

    if (sqlite3_prepare_v2(cat->db,
                           "SELECT s.id, s.name, s.path, s.active,"
                           " (SELECT COUNT(*) FROM nodes n WHERE n.source_id=s.id AND n.is_dir=0 AND n.deleted=0),"
                           " (SELECT COUNT(*) FROM nodes n WHERE n.source_id=s.id AND n.is_dir=0 AND n.deleted=0 AND EXISTS("
                           "   SELECT 1 FROM versions v JOIN copies k ON k.version_id=v.id"
                           "   WHERE v.node_id=n.id AND k.media_id=?1 AND k.state='ok'))"
                           " FROM sources s ORDER BY s.name COLLATE NOCASE, s.id",
                           -1, &q, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "list backup roots");
    sqlite3_bind_int64(q, 1, media_id);
    int rc;
    while ((rc = sqlite3_step(q)) == SQLITE_ROW) {
        if (nroots == cap) {
            size_t nc = cap ? cap * 2 : 8;
            list_root *nr = realloc(roots, nc * sizeof(*nr));
            if (!nr) { s = BD_ERR_NOMEM; break; }
            roots = nr;
            cap = nc;
        }
        list_root *r = &roots[nroots];
        r->id = sqlite3_column_int64(q, 0);
        r->name = bd_strdup((const char *)sqlite3_column_text(q, 1));
        r->path = bd_strdup((const char *)sqlite3_column_text(q, 2));
        r->active = sqlite3_column_int(q, 3);
        r->files = sqlite3_column_int64(q, 4);
        r->here = sqlite3_column_int64(q, 5);
        nroots++;
        if (!r->name || !r->path) { s = BD_ERR_NOMEM; break; }
    }
    if (s == BD_OK && rc != SQLITE_DONE) s = bd_fail_db(cat, "list backup roots");
    sqlite3_finalize(q);
    q = NULL;
    if (s != BD_OK) { free_roots(roots, nroots); return s == BD_ERR_NOMEM ? bd_fail(cat, s, "out of memory") : s; }
    if (!nroots) {
        fputs("\r\nThere are no backup roots yet.\r\n", f);
        free_roots(roots, nroots);
        return BD_OK;
    }

    /* The newest copy on this drive of each file that has one: with one
     * MAX() in the query, SQLite takes the other columns from that row. */
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT n.source_id, n.rel_path, n.deleted, n.current_version_id, n.last_seen_ms,"
                           " k.path_on_media, k.encrypted, v.id, v.mtime_ns, MAX(v.version_no)"
                           " FROM copies k JOIN versions v ON v.id=k.version_id JOIN nodes n ON n.id=v.node_id"
                           " JOIN sources s ON s.id=n.source_id"
                           " WHERE k.media_id=? AND k.state='ok' AND n.is_dir=0"
                           " GROUP BY v.node_id ORDER BY s.name COLLATE NOCASE, s.id, n.deleted, n.rel_path",
                           -1, &q, NULL) != SQLITE_OK) {
        free_roots(roots, nroots);
        return bd_fail_db(cat, "list newest copies");
    }
    sqlite3_bind_int64(q, 1, media_id);
    size_t at = 0;          /* the root being written */
    int started = 0;        /* its heading is out */
    int listed = 0;         /* lines under it */
    int deleted_part = 0;   /* its "deleted from the PC" part has begun */
    while ((rc = sqlite3_step(q)) == SQLITE_ROW) {
        int64_t sid = sqlite3_column_int64(q, 0);
        while (at < nroots && roots[at].id != sid) {
            if (!started) root_heading(f, &roots[at]);
            root_end(f, &roots[at], listed);
            at++;
            started = listed = deleted_part = 0;
        }
        if (at == nroots) break; /* cannot happen: every root is in the list */
        const list_root *r = &roots[at];
        if (!started) { root_heading(f, r); started = 1; }
        const char *rel = (const char *)sqlite3_column_text(q, 1);
        int deleted = sqlite3_column_int(q, 2);
        int64_t cur = sqlite3_column_type(q, 3) == SQLITE_NULL ? 0 : sqlite3_column_int64(q, 3);
        int64_t seen = sqlite3_column_int64(q, 4);
        const char *copy = (const char *)sqlite3_column_text(q, 5);
        int sealed = sqlite3_column_int(q, 6) != 0;
        int64_t vid = sqlite3_column_int64(q, 7);
        int64_t mtime_ns = sqlite3_column_int64(q, 8);
        if (!rel || !copy) continue;

        size_t nl = strlen(r->name), vl = strlen(BD_VERSIONS_DIR);
        int own = strncmp(copy, r->name, nl) == 0 && copy[nl] == '/' && strcmp(copy + nl + 1, rel) == 0;
        if (sealed && !own) {
            size_t cl = strlen(copy), sl = strlen(BD_SEALED_SUFFIX);
            own = cl > sl && strcmp(copy + cl - sl, BD_SEALED_SUFFIX) == 0 && strncmp(copy, r->name, nl) == 0 &&
                  copy[nl] == '/' && strlen(rel) == cl - sl - nl - 1 && strncmp(copy + nl + 1, rel, cl - sl - nl - 1) == 0;
        }
        int in_versions = strncmp(copy, BD_VERSIONS_DIR, vl) == 0 && copy[vl] == '/';
        int older = !deleted && vid != cur;
        if (!deleted && own && !sealed && !older) continue; /* in its own place: nothing to say */

        if (deleted && !deleted_part) {
            fputs("Deleted from the PC (their copies are still here, mixed in with the rest):\r\n", f);
            deleted_part = 1;
        }
        put_path(f, rel);
        fputc('\t', f);
        put_path(f, copy);
        fputc('\t', f);
        int parts = 0;
        if (deleted) {
            fputs("deleted, last seen ", f);
            put_time(f, seen);
            parts++;
        } else if (older) {
            fputs("an older version: the file changed since, and its newest copy is not on this drive", f);
            parts++;
        } else if (in_versions) {
            fputs("a newer version", f);
            parts++;
        } else if (!own) {
            fputs("moved or renamed: the copy has the old name", f);
            parts++;
        }
        if (sealed) {
            fputs(parts ? "; sealed, only BRODALF can open it" : "sealed, only BRODALF can open it", f);
            parts++;
        }
        if (!deleted && mtime_ns > 0) {
            fputs(parts ? "; modified " : "modified ", f);
            put_time(f, mtime_ns / 1000000);
        }
        fputs("\r\n", f);
        listed++;
    }
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) s = bd_fail_db(cat, "list newest copies");
    sqlite3_finalize(q);
    for (; s == BD_OK && at < nroots; at++) {
        if (!started) root_heading(f, &roots[at]);
        root_end(f, &roots[at], listed);
        started = listed = 0;
    }
    free_roots(roots, nroots);
    return s;
}

static bd_status write_file(bd_catalog *cat, int64_t media_id, const char *label, const char *catalog_name, const char *path)
{
    FILE *f = bd_fopen(path, "wb");
    if (!f) return bd_fail(cat, BD_ERR_IO, "cannot write %s", path);
    fputs("\xEF\xBB\xBF", f); /* UTF-8, so Notepad shows every name right */
    fputs("BRODALF: where the newest copy of each file is on this drive\r\n\r\nDrive \"", f);
    put_path(f, label ? label : "");
    fputs("\", database ", f);
    put_path(f, cat->uuid);
    fprintf(f, ", revision %lld, written ", (long long)bd_catalog_revision(cat));
    put_time(f, bd_now_ms());
    fputs(".\r\nIt goes with the copy of the database beside it named ", f);
    put_path(f, catalog_name);
    fputs(".\r\n\r\n"
          "To get your files back without BRODALF, copy each backup root's folder off this drive as it is.\r\n"
          "Those folders hold the first copy made of each file. Every file named below has its newest copy\r\n"
          "on this drive somewhere else: take that copy instead, and give it the name in the first column.\r\n"
          "\r\n"
          "- A file that changed has its newer copies in the .versions folder, named like report.v3.docx.\r\n"
          "- A file that was moved or renamed keeps its old name here.\r\n"
          "- A copy ending in .bdenc is sealed: only BRODALF can open it, with the passphrase or the recovery key.\r\n"
          "- Files deleted from the PC are still on the drive, mixed in with the rest. They are listed after\r\n"
          "  each backup root's other files.\r\n"
          "- Every backup writes a new list next to its copy of the database: use the list with the highest\r\n"
          "  number.\r\n"
          "\r\n"
          "Each line is the file in its backup root, a tab, its newest copy here (starting from this folder),\r\n"
          "a tab, and what it is.\r\n",
          f);
    bd_status s = write_list(cat, media_id, f);
    fflush(f);
    if (s == BD_OK && (ferror(f) || bd_fsync(f) != 0)) s = bd_fail(cat, BD_ERR_IO, "cannot write %s", path);
    if (fclose(f) != 0 && s == BD_OK) s = bd_fail(cat, BD_ERR_IO, "cannot write %s", path);
    return s;
}

bd_status bd_newest_list_to_media(bd_catalog *cat, int64_t media_id, const char *catalog_name)
{
    sqlite3_stmt *q;
    int skip = 1;
    char *label = NULL;
    if (sqlite3_prepare_v2(cat->db, "SELECT encrypted, hide_names, label FROM media WHERE id=?", -1, &q, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "find drive");
    sqlite3_bind_int64(q, 1, media_id);
    if (sqlite3_step(q) == SQLITE_ROW) {
        skip = sqlite3_column_int(q, 0) != 0 || sqlite3_column_int(q, 1) != 0;
        label = bd_strdup((const char *)sqlite3_column_text(q, 2));
    }
    sqlite3_finalize(q);
    if (skip) { free(label); return BD_OK; }

    bd_store *store = bd_store_open(cat, media_id);
    if (!store) { free(label); return BD_ERR_NOT_FOUND; }
    bd_status s = BD_OK;
    /* Named as its copy of the database is, catalog-r<revision>[-<tag>]
     * .brodalf: a copy put beside another of the same revision (media.c)
     * gets a list of its own beside the other's. */
    char rel[96], tmp[128], plain[64];
    snprintf(plain, sizeof(plain), "catalog-r%08lld.brodalf", (long long)bd_catalog_revision(cat));
    const char *cn = catalog_name ? catalog_name : plain;
    size_t cl = strlen(cn), pl = strlen("catalog-r"), sl = strlen(".brodalf");
    if (cl > pl + sl && strncmp(cn, "catalog-r", pl) == 0 && strcmp(cn + cl - sl, ".brodalf") == 0 && cl - pl - sl < 40)
        snprintf(rel, sizeof(rel), LIST_PREFIX "%.*s" LIST_SUFFIX, (int)(cl - pl - sl), cn + pl);
    else
        snprintf(rel, sizeof(rel), LIST_PREFIX "%08lld" LIST_SUFFIX, (long long)bd_catalog_revision(cat));
    snprintf(tmp, sizeof(tmp), "%s" BD_TMP_MARKER, rel);
    bd_remote_stat st;
    int there = store->ops->stat(store, rel, &st);
    /* Already there: this revision's list was written before (the same
     * database, so the same list). Never written over. */
    if (there < 0) s = bd_fail(cat, BD_ERR_IO, "cannot write the list of newest copies: %s", store->err);
    char *staged = there == 1 ? store->ops->staging_path(store, tmp) : NULL;
    if (there == 1 && !staged) s = bd_fail(cat, BD_ERR_IO, "cannot write the list of newest copies: %s", store->err);
    if (staged) {
        s = write_file(cat, media_id, label, cn, staged);
        if (s == BD_OK && store->ops->upload(store, staged, tmp, &st) != 0)
            s = bd_fail(cat, BD_ERR_IO, "cannot write the list of newest copies: %s", store->err);
        /* Not replacing: a list that turned up meanwhile stays. */
        if (s == BD_OK && store->ops->move(store, tmp, rel, 0) < 0)
            s = bd_fail(cat, BD_ERR_IO, "cannot write the list of newest copies: %s", store->err);
        store->ops->remove(store, tmp);
        if (s != BD_OK) bd_remove(staged);
        free(staged);
    }
    bd_store_close(store);
    free(label);
    return s;
}
