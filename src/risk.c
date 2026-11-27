/* The backup target (so many copies in so many places), progress toward
 * each full set of backups, and the files that fall short. */
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_COPIES 2
#define DEFAULT_PLACES 2
#define MAX_COPIES 16

#define PLACE(m) BD_SQL_PLACE(m)

/* Every live file with its copies and places; ?3 limits it to one source.
 * "older" means the file changed since its last backup: no good copy of the
 * current version anywhere, but an older version has one. A cloud file
 * edited elsewhere (temp.online_only, changed) has a newer version than the
 * catalog's current one, only in the cloud: it counts as changed since its
 * last backup, with no copies of that version. "oo" is 1 for a cloud file
 * never on this PC, 2 for one changed in the cloud. */
#define FILES_CTE                                                                                        \
    "WITH g AS (SELECT n.id, n.source_id, n.rel_path, n.size, n.current_version_id AS v,"                \
    "  COALESCE((SELECT o.changed+1 FROM temp.online_only o WHERE o.node_id=n.id),0) AS oo,"             \
    "  (SELECT " BD_SQL_COPIES("c") " FROM copies c"                                                 \
    "    WHERE c.version_id=n.current_version_id AND c.state='ok' AND " BD_SQL_COUNTS("c") ") AS cp0,"    \
    "  (SELECT COUNT(DISTINCT " PLACE("m") ") FROM copies c JOIN media m ON m.id=c.media_id"             \
    "    WHERE c.version_id=n.current_version_id AND c.state='ok' AND " BD_SQL_COUNTS("c") ") AS pl0,"    \
    "  EXISTS(SELECT 1 FROM copies c JOIN media m ON m.id=c.media_id"                                    \
    "    WHERE c.version_id=n.current_version_id AND c.state='ok' AND " BD_SQL_COUNTS("c")                \
    "    AND m.kind='drive' AND trim(COALESCE(m.location,''))='') AS unk0,"                              \
    "  (NOT EXISTS(SELECT 1 FROM copies c WHERE c.version_id=n.current_version_id AND c.state='ok')"     \
    "   AND EXISTS(SELECT 1 FROM copies c JOIN versions ov ON ov.id=c.version_id"                        \
    "    WHERE ov.node_id=n.id AND ov.id IS NOT n.current_version_id AND c.state='ok')) AS older0"       \
    "  FROM nodes n WHERE n.is_dir=0 AND n.deleted=0 AND (?3=0 OR n.source_id=?3)"                       \
    "  AND " BD_SQL_ACTIVE("n") "),"                                                                     \
    " f AS (SELECT id, source_id, rel_path, size, v, oo,"                                                \
    "  CASE WHEN oo=2 THEN 0 ELSE cp0 END AS cp, CASE WHEN oo=2 THEN 0 ELSE pl0 END AS pl,"              \
    "  CASE WHEN oo=2 THEN 0 ELSE unk0 END AS unk,"                                                      \
    "  CASE WHEN oo=2 THEN EXISTS(SELECT 1 FROM copies c JOIN versions ov ON ov.id=c.version_id"         \
    "    WHERE ov.node_id=g.id AND c.state='ok') ELSE older0 END AS older FROM g),"                       \
    " r AS (SELECT * FROM f WHERE cp<?1 OR pl<?2) "

bd_status bd_target_get(bd_catalog *cat, bd_target *out)
{
    char v[32];
    out->copies = DEFAULT_COPIES;
    out->places = DEFAULT_PLACES;
    if (bd_setting_get(cat, "target_copies", v, sizeof(v))) out->copies = atoi(v);
    if (bd_setting_get(cat, "target_places", v, sizeof(v))) out->places = atoi(v);
    if (out->copies < 1 || out->copies > MAX_COPIES) out->copies = DEFAULT_COPIES;
    if (out->places < 1) out->places = 1;
    if (out->places > out->copies) out->places = out->copies;
    return BD_OK;
}

bd_status bd_target_set(bd_catalog *cat, const bd_target *t)
{
    if (!t || t->copies < 1 || t->copies > MAX_COPIES)
        return bd_fail(cat, BD_ERR_INVALID, "the number of copies must be between 1 and %d", MAX_COPIES);
    if (t->places < 1 || t->places > t->copies)
        return bd_fail(cat, BD_ERR_INVALID, "the number of places must be between 1 and the number of copies");
    char c[16], p[16];
    snprintf(c, sizeof(c), "%d", t->copies);
    snprintf(p, sizeof(p), "%d", t->places);
    if (bd_setting_set(cat, "target_copies", c) != 0 || bd_setting_set(cat, "target_places", p) != 0)
        return bd_fail_db(cat, "save the backup target");
    return BD_OK;
}

static void bind_target(sqlite3_stmt *q, const bd_target *t, int64_t source_id)
{
    sqlite3_bind_int(q, 1, t->copies);
    sqlite3_bind_int(q, 2, t->places);
    sqlite3_bind_int64(q, 3, source_id);
}

bd_status bd_list_at_risk(bd_catalog *cat, int64_t source_id, bd_risk_fn fn, void *ctx, bd_risk_stats *stats)
{
    bd_target t;
    bd_target_get(cat, &t);
    sqlite3_stmt *q;

    if (stats) {
        memset(stats, 0, sizeof(*stats));
        if (sqlite3_prepare_v2(cat->db,
                               FILES_CTE "SELECT (SELECT COUNT(*) FROM f), COUNT(*), COALESCE(SUM(cp=0),0),"
                                         " COALESCE(SUM(size),0) FROM r",
                               -1, &q, NULL) != SQLITE_OK)
            return bd_fail_db(cat, "count files at risk");
        bind_target(q, &t, source_id);
        if (sqlite3_step(q) == SQLITE_ROW) {
            stats->files_total = sqlite3_column_int64(q, 0);
            stats->files_at_risk = sqlite3_column_int64(q, 1);
            stats->files_no_copy = sqlite3_column_int64(q, 2);
            stats->bytes_at_risk = sqlite3_column_int64(q, 3);
        }
        sqlite3_finalize(q);
        /* Deleted files (their last version) and older versions. */
        if (sqlite3_prepare_v2(cat->db,
                               "WITH k AS (SELECT n.deleted AS del, v.id IS NOT n.current_version_id AS old,"
                               "  (SELECT " BD_SQL_COPIES("c") " FROM copies c WHERE c.version_id=v.id AND c.state='ok'"
                               "   AND " BD_SQL_COUNTS("c") ") AS cp"
                               "  FROM versions v JOIN nodes n ON n.id=v.node_id"
                               "  WHERE n.is_dir=0 AND (?2=0 OR n.source_id=?2)"
                               "  AND (v.id IS NOT n.current_version_id OR n.deleted=1))"
                               " SELECT COALESCE(SUM(del AND NOT old AND cp>0),0), COALESCE(SUM(del AND NOT old AND cp>0 AND cp<?1),0),"
                               " COALESCE(SUM(old AND cp>0),0), COALESCE(SUM(old AND cp>0 AND cp<?1),0) FROM k",
                               -1, &q, NULL) != SQLITE_OK)
            return bd_fail_db(cat, "count deleted files and old versions");
        sqlite3_bind_int(q, 1, t.copies);
        sqlite3_bind_int64(q, 2, source_id);
        if (sqlite3_step(q) == SQLITE_ROW) {
            stats->deleted_kept = sqlite3_column_int64(q, 0);
            stats->deleted_short = sqlite3_column_int64(q, 1);
            stats->old_versions_kept = sqlite3_column_int64(q, 2);
            stats->old_versions_short = sqlite3_column_int64(q, 3);
        }
        sqlite3_finalize(q);
    }
    if (!fn) return BD_OK;

    if (sqlite3_prepare_v2(cat->db,
                           FILES_CTE "SELECT r.id, r.source_id, s.name, r.rel_path, r.size, r.cp, r.pl, r.unk, r.older, r.oo"
                                     " FROM r JOIN sources s ON s.id=r.source_id"
                                     " ORDER BY r.cp, r.pl, s.name COLLATE NOCASE, r.rel_path COLLATE NOCASE",
                           -1, &q, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "list files at risk");
    bind_target(q, &t, source_id);
    while (sqlite3_step(q) == SQLITE_ROW) {
        bd_risk_info info;
        memset(&info, 0, sizeof(info));
        info.node_id = sqlite3_column_int64(q, 0);
        info.source_id = sqlite3_column_int64(q, 1);
        info.source_name = (const char *)sqlite3_column_text(q, 2);
        info.rel_path = (const char *)sqlite3_column_text(q, 3);
        info.size = sqlite3_column_int64(q, 4);
        info.copies = sqlite3_column_int(q, 5);
        info.places = sqlite3_column_int(q, 6);
        info.unknown_place = sqlite3_column_int(q, 7);
        info.older_copies = sqlite3_column_int(q, 8);
        info.online_only = sqlite3_column_int(q, 9);
        if (fn(ctx, &info) != 0) break;
    }
    sqlite3_finalize(q);
    return BD_OK;
}

bd_status bd_backup_sets(bd_catalog *cat, bd_set_progress *out, int cap, int *sets)
{
    bd_target t;
    bd_target_get(cat, &t);
    if (sets) *sets = t.copies;
    if (!out || cap <= 0) return BD_OK;
    int n = t.copies < cap ? t.copies : cap;
    memset(out, 0, sizeof(*out) * (size_t)n);
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT cp, COUNT(*), COALESCE(SUM(size),0) FROM ("
                           " SELECT n.size AS size, (SELECT " BD_SQL_COPIES("c") " FROM copies c"
                           "  WHERE c.version_id=n.current_version_id AND c.state='ok' AND " BD_SQL_COUNTS("c") ") AS cp"
                           " FROM nodes n WHERE n.is_dir=0 AND n.deleted=0 AND " BD_SQL_ACTIVE("n") ")"
                           " GROUP BY cp",
                           -1, &q, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "work out the backup sets");
    for (int k = 0; k < n; k++) out[k].set = k + 1;
    while (sqlite3_step(q) == SQLITE_ROW) {
        int cp = sqlite3_column_int(q, 0);
        int64_t files = sqlite3_column_int64(q, 1), bytes = sqlite3_column_int64(q, 2);
        for (int k = 0; k < n; k++) {
            out[k].files_total += files;
            out[k].bytes_total += bytes;
            if (cp >= k + 1) {
                out[k].files_done += files;
                out[k].bytes_done += bytes;
            }
        }
    }
    sqlite3_finalize(q);
    return BD_OK;
}

bd_status bd_backup_set_drives(bd_catalog *cat, bd_set_drive *out, int cap, int *n)
{
    if (n) *n = 0;
    if (!out || cap <= 0) return BD_OK;
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(cat->db,
                           "WITH h AS (SELECT DISTINCT n.id AS nid, c.media_id AS mid FROM nodes n"
                           "  JOIN copies c ON c.version_id=n.current_version_id AND c.state='ok' AND " BD_SQL_COUNTS("c")
                           "  WHERE n.is_dir=0 AND n.deleted=0 AND " BD_SQL_ACTIVE("n") "),"
                           " r AS (SELECT mid, ROW_NUMBER() OVER (PARTITION BY nid ORDER BY mid) AS k FROM h)"
                           " SELECT k, mid, COUNT(*) FROM r GROUP BY k, mid ORDER BY k, mid",
                           -1, &q, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "work out which drives make up each full copy");
    int got = 0;
    while (got < cap && sqlite3_step(q) == SQLITE_ROW) {
        out[got].set = sqlite3_column_int(q, 0);
        out[got].media_id = sqlite3_column_int64(q, 1);
        out[got].files = sqlite3_column_int64(q, 2);
        got++;
    }
    sqlite3_finalize(q);
    if (n) *n = got;
    return BD_OK;
}

bd_status bd_list_largest(bd_catalog *cat, int limit, bd_risk_fn fn, void *ctx)
{
    bd_target t;
    bd_target_get(cat, &t);
    sqlite3_stmt *q;
    if (limit <= 0) limit = 100;
    if (sqlite3_prepare_v2(cat->db,
                           FILES_CTE "SELECT f.id, f.source_id, s.name, f.rel_path, f.size, f.cp, f.pl, f.unk, f.older"
                                     " FROM f JOIN sources s ON s.id=f.source_id ORDER BY f.size DESC LIMIT ?4",
                           -1, &q, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "list the largest files");
    bind_target(q, &t, 0);
    sqlite3_bind_int(q, 4, limit);
    while (sqlite3_step(q) == SQLITE_ROW) {
        bd_risk_info info;
        memset(&info, 0, sizeof(info));
        info.node_id = sqlite3_column_int64(q, 0);
        info.source_id = sqlite3_column_int64(q, 1);
        info.source_name = (const char *)sqlite3_column_text(q, 2);
        info.rel_path = (const char *)sqlite3_column_text(q, 3);
        info.size = sqlite3_column_int64(q, 4);
        info.copies = sqlite3_column_int(q, 5);
        info.places = sqlite3_column_int(q, 6);
        info.unknown_place = sqlite3_column_int(q, 7);
        info.older_copies = sqlite3_column_int(q, 8);
        if (fn(ctx, &info) != 0) break;
    }
    sqlite3_finalize(q);
    return BD_OK;
}

bd_status bd_list_version_space(bd_catalog *cat, int limit, bd_version_space_fn fn, void *ctx)
{
    sqlite3_stmt *q;
    if (limit <= 0) limit = 100;
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT n.id, n.source_id, s.name, n.rel_path, COALESCE(cv.size, n.size),"
                           " COUNT(DISTINCT v.id), SUM(c.stored_size) AS used"
                           " FROM nodes n JOIN sources s ON s.id=n.source_id AND s.active=1"
                           " LEFT JOIN versions cv ON cv.id=n.current_version_id"
                           " JOIN versions v ON v.node_id=n.id AND v.id IS NOT n.current_version_id"
                           " JOIN copies c ON c.version_id=v.id AND c.state<>'missing'"
                           " WHERE n.is_dir=0 AND n.deleted=0"
                           " GROUP BY n.id ORDER BY used DESC, n.id LIMIT ?",
                           -1, &q, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "list the files whose older versions take the most space");
    sqlite3_bind_int(q, 1, limit);
    while (sqlite3_step(q) == SQLITE_ROW) {
        bd_version_space info;
        memset(&info, 0, sizeof(info));
        info.node_id = sqlite3_column_int64(q, 0);
        info.source_id = sqlite3_column_int64(q, 1);
        info.source_name = (const char *)sqlite3_column_text(q, 2);
        info.rel_path = (const char *)sqlite3_column_text(q, 3);
        info.size = sqlite3_column_int64(q, 4);
        info.old_versions = sqlite3_column_int64(q, 5);
        info.old_bytes = sqlite3_column_int64(q, 6);
        if (fn && fn(ctx, &info) != 0) break;
    }
    sqlite3_finalize(q);
    return BD_OK;
}

bd_status bd_list_risk_help(bd_catalog *cat, bd_risk_help_fn fn, void *ctx)
{
    bd_target t;
    bd_target_get(cat, &t);
    sqlite3_stmt *q;
    /* A medium helps a file when it holds no good copy of the current
     * version yet, and the file either needs more copies or is missing
     * the medium's place. */
    if (sqlite3_prepare_v2(cat->db,
                           FILES_CTE "SELECT m.id, m.label, m.kind, COALESCE(m.location,''),"
                                     " EXISTS(SELECT 1 FROM temp.connected k WHERE k.media_id=m.id),"
                                     " COUNT(*), COALESCE(SUM(r.size),0), m.free_bytes, COALESCE(m.total_bytes,0),"
                                     " (SELECT MAX(l.at_ms) FROM space_log l WHERE l.media_id=m.id)"
                                     " FROM media m JOIN r"
                                     " WHERE m.kind='drive' AND m.retired_ms IS NULL AND m.secondary=0 AND r.oo=0"
                                     " AND NOT EXISTS(SELECT 1 FROM copies c WHERE c.version_id=r.v"
                                     "   AND c.media_id=m.id AND c.state='ok')"
                                     " AND (r.cp<?1 OR NOT EXISTS(SELECT 1 FROM copies c JOIN media m2 ON m2.id=c.media_id"
                                     "   WHERE c.version_id=r.v AND c.state='ok' AND " BD_SQL_COUNTS("c")
                                     "   AND " PLACE("m2") "=" PLACE("m") "))"
                                     " GROUP BY m.id ORDER BY 6 DESC, 7 DESC, m.label COLLATE NOCASE",
                           -1, &q, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "work out which drive helps");
    bind_target(q, &t, 0);
    int64_t reserve = bd_reserve_bytes(cat);
    while (sqlite3_step(q) == SQLITE_ROW) {
        bd_risk_help h;
        memset(&h, 0, sizeof(h));
        h.media_id = sqlite3_column_int64(q, 0);
        h.label = (const char *)sqlite3_column_text(q, 1);
        h.kind = (const char *)sqlite3_column_text(q, 2);
        h.location = (const char *)sqlite3_column_text(q, 3);
        h.connected = sqlite3_column_int(q, 4);
        h.files = sqlite3_column_int64(q, 5);
        h.bytes = sqlite3_column_int64(q, 6);
        h.free_bytes = sqlite3_column_type(q, 7) == SQLITE_NULL ? -1 : sqlite3_column_int64(q, 7);
        h.space_ms = sqlite3_column_type(q, 9) == SQLITE_NULL ? 0 : sqlite3_column_int64(q, 9);
        h.fits = h.free_bytes >= 0 && h.bytes + reserve <= h.free_bytes;
        if (fn(ctx, &h) != 0) break;
    }
    sqlite3_finalize(q);
    return BD_OK;
}

typedef struct {
    bd_risk_help best;
    int have;
    char label[256], kind[16], location[256];
} suggest_ctx;

static int take_suggestion(suggest_ctx *s, const bd_risk_help *h)
{
    s->best = *h;
    s->have = 1;
    snprintf(s->label, sizeof(s->label), "%s", h->label ? h->label : "");
    snprintf(s->kind, sizeof(s->kind), "%s", h->kind ? h->kind : "");
    snprintf(s->location, sizeof(s->location), "%s", h->location ? h->location : "");
    s->best.label = s->label;
    s->best.kind = s->kind;
    s->best.location = s->location;
    return 0;
}

/* Rows come most helpful first: take the first that fits, else the one
 * with the most known room. */
static int suggest_cb(void *ctx, const bd_risk_help *h)
{
    suggest_ctx *s = ctx;
    if (s->have && s->best.fits) return 1;
    if (h->fits) return take_suggestion(s, h);
    if (!s->have || h->free_bytes > s->best.free_bytes) take_suggestion(s, h);
    return 0;
}

bd_status bd_suggest_drive(bd_catalog *cat, bd_risk_help *out)
{
    static suggest_ctx s;
    memset(&s, 0, sizeof(s));
    bd_status st = bd_list_risk_help(cat, suggest_cb, &s);
    if (st != BD_OK) return st;
    if (!s.have) return bd_fail(cat, BD_ERR_NOT_FOUND, "no drive would help");
    *out = s.best;
    return BD_OK;
}
