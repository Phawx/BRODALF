/* Starred files, the extra places they also go to, and folders set to be
 * encrypted. Both kinds of mark are a backup root and a path in it ("" for
 * the whole root); a folder's mark covers everything under it, so files
 * added there later are covered too. */
#include "store.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define LIVE_FILES " n.is_dir=0 AND n.deleted=0 AND " BD_SQL_ACTIVE("n")

/* '/' separators, no slash at either end. malloc'd. */
static char *clean_rel(const char *rel)
{
    char *p = bd_strdup(rel ? rel : "");
    if (!p) return NULL;
    for (char *c = p; *c; c++) if (*c == '\\') *c = '/';
    size_t n = strlen(p);
    while (n > 0 && p[n - 1] == '/') p[--n] = '\0';
    size_t lead = strspn(p, "/");
    if (lead) memmove(p, p + lead, strlen(p + lead) + 1);
    return p;
}

static int source_exists(bd_catalog *cat, int64_t source_id)
{
    sqlite3_stmt *q;
    int ok = 0;
    if (sqlite3_prepare_v2(cat->db, "SELECT 1 FROM sources WHERE id=?", -1, &q, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int64(q, 1, source_id);
    ok = sqlite3_step(q) == SQLITE_ROW;
    sqlite3_finalize(q);
    return ok;
}

static bd_status mark_set(bd_catalog *cat, const char *table, int64_t source_id, const char *rel_path, int on)
{
    if (!source_exists(cat, source_id)) return bd_fail(cat, BD_ERR_NOT_FOUND, "no such backup root");
    char *rel = clean_rel(rel_path);
    if (!rel) return BD_ERR_NOMEM;
    char sql[160];
    sqlite3_stmt *u;
    bd_status s = BD_OK;
    if (on) {
        int64_t node = 0;
        if (*rel && bd_find_node(cat, source_id, rel, &node) != BD_OK) {
            free(rel);
            return bd_fail(cat, BD_ERR_NOT_FOUND, "that file or folder is not in the catalog; scan first");
        }
        snprintf(sql, sizeof(sql), "INSERT OR IGNORE INTO %s(source_id, rel_path, added_ms) VALUES(?,?,?)", table);
    } else {
        snprintf(sql, sizeof(sql), "DELETE FROM %s WHERE source_id=? AND rel_path=?", table);
    }
    if (sqlite3_prepare_v2(cat->db, sql, -1, &u, NULL) != SQLITE_OK) {
        free(rel);
        return bd_fail_db(cat, "mark the file");
    }
    sqlite3_bind_int64(u, 1, source_id);
    sqlite3_bind_text(u, 2, rel, -1, SQLITE_STATIC);
    if (on) sqlite3_bind_int64(u, 3, bd_now_ms());
    if (sqlite3_step(u) != SQLITE_DONE) s = bd_fail_db(cat, "mark the file");
    sqlite3_finalize(u);
    free(rel);
    return s;
}

static int mark_get(bd_catalog *cat, const char *table, int64_t source_id, const char *rel_path)
{
    char *rel = clean_rel(rel_path);
    if (!rel) return 0;
    char sql[320];
    snprintf(sql, sizeof(sql),
             "SELECT MIN(CASE WHEN rel_path=?2 THEN 1 ELSE 2 END) FROM %s WHERE source_id=?1"
             " AND (rel_path='' OR rel_path=?2 OR substr(?2,1,length(rel_path)+1)=rel_path||'/')",
             table);
    sqlite3_stmt *q;
    int r = 0;
    if (sqlite3_prepare_v2(cat->db, sql, -1, &q, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(q, 1, source_id);
        sqlite3_bind_text(q, 2, rel, -1, SQLITE_STATIC);
        if (sqlite3_step(q) == SQLITE_ROW && sqlite3_column_type(q, 0) != SQLITE_NULL) r = sqlite3_column_int(q, 0);
        sqlite3_finalize(q);
    }
    free(rel);
    return r;
}

static bd_status mark_list(bd_catalog *cat, const char *table, bd_mark_fn fn, void *ctx)
{
    char sql[1400];
    snprintf(sql, sizeof(sql),
             "SELECT k.source_id, s.name, k.rel_path, k.added_ms,"
             " CASE WHEN k.rel_path='' THEN 1 ELSE COALESCE(m.is_dir,0) END,"
             " k.rel_path='' OR (m.id IS NOT NULL AND m.deleted=0),"
             " (SELECT COUNT(*) FROM nodes n WHERE n.source_id=k.source_id AND" LIVE_FILES
             "   AND (k.rel_path='' OR n.rel_path=k.rel_path OR substr(n.rel_path,1,length(k.rel_path)+1)=k.rel_path||'/')),"
             " (SELECT COALESCE(SUM(v.size),0) FROM nodes n JOIN versions v ON v.id=n.current_version_id"
             "   WHERE n.source_id=k.source_id AND" LIVE_FILES
             "   AND (k.rel_path='' OR n.rel_path=k.rel_path OR substr(n.rel_path,1,length(k.rel_path)+1)=k.rel_path||'/'))"
             " FROM %s k JOIN sources s ON s.id=k.source_id"
             " LEFT JOIN nodes m ON m.source_id=k.source_id AND m.rel_path=k.rel_path"
             " ORDER BY s.name COLLATE NOCASE, k.rel_path COLLATE NOCASE",
             table);
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(cat->db, sql, -1, &q, NULL) != SQLITE_OK) return bd_fail_db(cat, "list marked files");
    while (sqlite3_step(q) == SQLITE_ROW) {
        bd_mark_info m;
        m.source_id = sqlite3_column_int64(q, 0);
        m.source_name = (const char *)sqlite3_column_text(q, 1);
        m.rel_path = (const char *)sqlite3_column_text(q, 2);
        m.added_ms = sqlite3_column_int64(q, 3);
        m.is_dir = sqlite3_column_int(q, 4);
        m.found = sqlite3_column_int(q, 5);
        m.files = sqlite3_column_int64(q, 6);
        m.bytes = sqlite3_column_int64(q, 7);
        if (fn && fn(ctx, &m)) break;
    }
    sqlite3_finalize(q);
    return BD_OK;
}

bd_status bd_star_set(bd_catalog *cat, int64_t source_id, const char *rel_path, int on)
{
    return mark_set(cat, "stars", source_id, rel_path, on);
}

int bd_starred(bd_catalog *cat, int64_t source_id, const char *rel_path)
{
    return mark_get(cat, "stars", source_id, rel_path);
}

bd_status bd_list_stars(bd_catalog *cat, bd_mark_fn fn, void *ctx)
{
    return mark_list(cat, "stars", fn, ctx);
}

bd_status bd_starred_totals(bd_catalog *cat, int64_t *files, int64_t *bytes)
{
    sqlite3_stmt *q;
    if (files) *files = 0;
    if (bytes) *bytes = 0;
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT COUNT(*), COALESCE(SUM(v.size),0) FROM nodes n JOIN versions v ON v.id=n.current_version_id"
                           " WHERE" LIVE_FILES " AND " BD_SQL_MARKED("stars", "n"),
                           -1, &q, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "count starred files");
    if (sqlite3_step(q) == SQLITE_ROW) {
        if (files) *files = sqlite3_column_int64(q, 0);
        if (bytes) *bytes = sqlite3_column_int64(q, 1);
    }
    sqlite3_finalize(q);
    return BD_OK;
}

bd_status bd_encrypt_path_set(bd_catalog *cat, int64_t source_id, const char *rel_path, int on)
{
    if (on && !bd_catalog_has_passphrase(cat))
        return bd_fail(cat, BD_ERR_PASSPHRASE, "set a passphrase first: encrypted copies are locked with it");
    return mark_set(cat, "encrypt_paths", source_id, rel_path, on);
}

int bd_encrypt_marked(bd_catalog *cat, int64_t source_id, const char *rel_path)
{
    return mark_get(cat, "encrypt_paths", source_id, rel_path);
}

bd_status bd_list_encrypt_paths(bd_catalog *cat, bd_mark_fn fn, void *ctx)
{
    return mark_list(cat, "encrypt_paths", fn, ctx);
}

bd_status bd_encrypt_stats_get(bd_catalog *cat, bd_encrypt_stats *out)
{
    memset(out, 0, sizeof(*out));
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT COUNT(*), COALESCE(SUM(v.size),0),"
                           " COALESCE(SUM((SELECT COUNT(*) FROM copies c WHERE c.version_id=v.id AND c.state='ok' AND c.encrypted<>0)),0),"
                           " COALESCE(SUM((SELECT COUNT(*) FROM copies c WHERE c.version_id=v.id AND c.state='ok' AND c.encrypted=0)),0),"
                           " COALESCE(SUM(NOT EXISTS(SELECT 1 FROM copies c WHERE c.version_id=v.id AND c.state='ok' AND c.encrypted<>0)),0)"
                           " FROM nodes n JOIN versions v ON v.id=n.current_version_id"
                           " WHERE" LIVE_FILES " AND " BD_SQL_MARKED("encrypt_paths", "n"),
                           -1, &q, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "count encrypted files");
    if (sqlite3_step(q) == SQLITE_ROW) {
        out->files = sqlite3_column_int64(q, 0);
        out->bytes = sqlite3_column_int64(q, 1);
        out->sealed_copies = sqlite3_column_int64(q, 2);
        out->plain_copies = sqlite3_column_int64(q, 3);
        out->files_waiting = sqlite3_column_int64(q, 4);
    }
    sqlite3_finalize(q);
    if (sqlite3_prepare_v2(cat->db, "SELECT COUNT(*) FROM media WHERE encrypted<>0 AND retired_ms IS NULL", -1, &q, NULL) == SQLITE_OK) {
        if (sqlite3_step(q) == SQLITE_ROW) out->drives_encrypted = sqlite3_column_int64(q, 0);
        sqlite3_finalize(q);
    }
    out->bytes_per_second = bd_crypto_speed();
    return BD_OK;
}

/* ---- Extra places -------------------------------------------------------- */

/* At least two words and 10 characters, so the place can be told apart
 * later from the name alone. */
static int good_description(const char *label)
{
    if (!label) return 0;
    int words = 0, in_word = 0;
    size_t chars = 0;
    for (const unsigned char *p = (const unsigned char *)label; *p; p++) {
        if (isspace(*p)) { in_word = 0; continue; }
        chars++;
        if (!in_word) { words++; in_word = 1; }
    }
    return words >= 2 && chars >= 9 && strlen(label) >= 10;
}

static bd_status set_limit(bd_catalog *cat, int64_t media_id, int64_t limit_bytes)
{
    sqlite3_stmt *u;
    if (sqlite3_prepare_v2(cat->db, "UPDATE media SET limit_bytes=? WHERE id=? AND secondary<>0", -1, &u, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "set the limit");
    if (limit_bytes > 0) sqlite3_bind_int64(u, 1, limit_bytes);
    else sqlite3_bind_null(u, 1);
    sqlite3_bind_int64(u, 2, media_id);
    int rc = sqlite3_step(u);
    sqlite3_finalize(u);
    if (rc != SQLITE_DONE) return bd_fail_db(cat, "set the limit");
    if (sqlite3_changes(cat->db) == 0) return bd_fail(cat, BD_ERR_INVALID, "that is not an extra place for starred files");
    return BD_OK;
}

bd_status bd_place_add(bd_catalog *cat, const char *folder, const char *label, int64_t limit_bytes, unsigned flags,
                       int64_t *out_media_id)
{
    if (!good_description(label))
        return bd_fail(cat, BD_ERR_INVALID,
                       "describe this place in a few words, like \"Dropbox folder on the laptop\", so you can tell later what it "
                       "is and where it lives");
    if (limit_bytes < 0) return bd_fail(cat, BD_ERR_INVALID, "the limit cannot be negative");
    int64_t id = 0;
    bd_status s = bd_media_init(cat, folder, label, (flags & BD_MEDIA_ENCRYPTED) | BD_MEDIA_SECONDARY, &id);
    if (s != BD_OK) return s;
    if (limit_bytes > 0 && (s = set_limit(cat, id, limit_bytes)) != BD_OK) return s;
    if (out_media_id) *out_media_id = id;
    return BD_OK;
}

bd_status bd_place_set_limit(bd_catalog *cat, int64_t media_id, int64_t limit_bytes)
{
    if (limit_bytes < 0) return bd_fail(cat, BD_ERR_INVALID, "the limit cannot be negative");
    return set_limit(cat, media_id, limit_bytes);
}

bd_status bd_place_status_get(bd_catalog *cat, int64_t media_id, bd_place_status *out)
{
    memset(out, 0, sizeof(*out));
    sqlite3_stmt *q;
    int encrypted = 0;
    if (sqlite3_prepare_v2(cat->db, "SELECT COALESCE(limit_bytes,0), encrypted FROM media WHERE id=?", -1, &q, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "read the place");
    sqlite3_bind_int64(q, 1, media_id);
    int found = sqlite3_step(q) == SQLITE_ROW;
    if (found) {
        out->limit_bytes = sqlite3_column_int64(q, 0);
        encrypted = sqlite3_column_int(q, 1);
    }
    sqlite3_finalize(q);
    if (!found) return bd_fail(cat, BD_ERR_NOT_FOUND, "no such place");
    if (sqlite3_prepare_v2(cat->db, "SELECT COALESCE(SUM(stored_size),0) FROM copies WHERE media_id=? AND state='ok'", -1, &q, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(q, 1, media_id);
        if (sqlite3_step(q) == SQLITE_ROW) out->used_bytes = sqlite3_column_int64(q, 0);
        sqlite3_finalize(q);
    }
    /* Every starred file, whether it is there, in the order a backup
     * takes them, so the limit is applied the same way. */
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT v.size, EXISTS(SELECT 1 FROM copies c WHERE c.version_id=v.id AND c.media_id=?1 AND c.state='ok'),"
                           " " BD_SQL_MARKED("encrypt_paths", "n")
                           " FROM nodes n JOIN versions v ON v.id=n.current_version_id"
                           " WHERE" LIVE_FILES " AND " BD_SQL_MARKED("stars", "n") " ORDER BY n.id",
                           -1, &q, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "read the starred files");
    sqlite3_bind_int64(q, 1, media_id);
    int64_t budget = out->limit_bytes > 0 ? out->limit_bytes - out->used_bytes : -1;
    while (sqlite3_step(q) == SQLITE_ROW) {
        int64_t size = sqlite3_column_int64(q, 0);
        int have = sqlite3_column_int(q, 1), sealed = encrypted || sqlite3_column_int(q, 2);
        out->starred_files++;
        out->starred_bytes += size;
        if (have) {
            out->have_files++;
            out->have_bytes += size;
            continue;
        }
        int64_t need = size + (sealed ? size / 1024 + 4096 : 0);
        if (budget >= 0 && need > budget) {
            out->over_limit_files++;
            out->over_limit_bytes += size;
            continue;
        }
        if (budget >= 0) budget -= need;
        out->waiting_files++;
        out->waiting_bytes += size;
    }
    sqlite3_finalize(q);
    return BD_OK;
}
