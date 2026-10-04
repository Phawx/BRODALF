/* The protection target (so many copies in so many places) and the files
 * that fall short of it. */
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_COPIES 2
#define DEFAULT_PLACES 2
#define MAX_COPIES 16

/* Where a medium is kept: its "kept in" text for a drive (all drives with
 * none set share ''), and a place of its own for each cloud account. */
#define PLACE(m) "(CASE WHEN " m ".kind<>'drive' THEN 'cloud:'||" m ".id ELSE lower(trim(COALESCE(" m ".location,''))) END)"

/* Every live file with its copies and places; ?3 limits it to one source. */
#define FILES_CTE                                                                                        \
    "WITH f AS (SELECT n.id, n.source_id, n.rel_path, n.size, n.current_version_id AS v,"                \
    "  (SELECT COUNT(DISTINCT c.media_id) FROM copies c"                                                 \
    "    WHERE c.version_id=n.current_version_id AND c.state='ok') AS cp,"                               \
    "  (SELECT COUNT(DISTINCT " PLACE("m") ") FROM copies c JOIN media m ON m.id=c.media_id"             \
    "    WHERE c.version_id=n.current_version_id AND c.state='ok') AS pl,"                               \
    "  EXISTS(SELECT 1 FROM copies c JOIN media m ON m.id=c.media_id"                                    \
    "    WHERE c.version_id=n.current_version_id AND c.state='ok'"                                       \
    "    AND m.kind='drive' AND trim(COALESCE(m.location,''))='') AS unk,"                               \
    "  EXISTS(SELECT 1 FROM copies c JOIN versions ov ON ov.id=c.version_id"                             \
    "    WHERE ov.node_id=n.id AND ov.id IS NOT n.current_version_id AND c.state='ok') AS older"         \
    "  FROM nodes n WHERE n.is_dir=0 AND n.deleted=0 AND (?3=0 OR n.source_id=?3)),"                     \
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
        return bd_fail_db(cat, "save the protection target");
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
    }
    if (!fn) return BD_OK;

    if (sqlite3_prepare_v2(cat->db,
                           FILES_CTE "SELECT r.id, r.source_id, s.name, r.rel_path, r.size, r.cp, r.pl, r.unk, r.older"
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
        if (fn(ctx, &info) != 0) break;
    }
    sqlite3_finalize(q);
    return BD_OK;
}

/* What a backup leaves free on a drive (as in backup.c). */
static int64_t reserve_for(int64_t total)
{
    const int64_t min = 16 * 1024 * 1024;
    return total / 200 > min ? total / 200 : min;
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
                                     " WHERE NOT EXISTS(SELECT 1 FROM copies c WHERE c.version_id=r.v"
                                     "   AND c.media_id=m.id AND c.state='ok')"
                                     " AND (r.cp<?1 OR NOT EXISTS(SELECT 1 FROM copies c JOIN media m2 ON m2.id=c.media_id"
                                     "   WHERE c.version_id=r.v AND c.state='ok' AND " PLACE("m2") "=" PLACE("m") "))"
                                     " GROUP BY m.id ORDER BY 6 DESC, 7 DESC, m.label COLLATE NOCASE",
                           -1, &q, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "work out which drive helps");
    bind_target(q, &t, 0);
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
        h.fits = h.free_bytes >= 0 && h.bytes + reserve_for(sqlite3_column_int64(q, 8)) <= h.free_bytes;
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
