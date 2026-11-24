/* Replacing a drive. The old drive is marked retired (media.retired_ms) and
 * the new one recorded as taking its place (media.replaced_by). From then
 * on nothing new goes to the old drive, and a copy on it stops counting
 * toward the protection target as soon as the new drive holds the same
 * version (BD_SQL_COUNTS). The copy itself is a backup to the new drive
 * limited to what needs the old one: current versions that have a good copy
 * on the old drive and would fall short of the target without it. */
#include "internal.h"

#include <string.h>

int bd_media_retired(bd_catalog *cat, int64_t media_id)
{
    sqlite3_stmt *q;
    int retired = 0;
    if (sqlite3_prepare_v2(cat->db, "SELECT retired_ms IS NOT NULL FROM media WHERE id=?", -1, &q, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int64(q, 1, media_id);
    if (sqlite3_step(q) == SQLITE_ROW) retired = sqlite3_column_int(q, 0);
    sqlite3_finalize(q);
    return retired;
}

static int media_exists(bd_catalog *cat, int64_t media_id)
{
    sqlite3_stmt *q;
    int found = 0;
    if (sqlite3_prepare_v2(cat->db, "SELECT 1 FROM media WHERE id=?", -1, &q, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int64(q, 1, media_id);
    found = sqlite3_step(q) == SQLITE_ROW;
    sqlite3_finalize(q);
    return found;
}

static bd_status set_retired(bd_catalog *cat, int64_t media_id, int retired, int64_t replacement_id)
{
    sqlite3_stmt *u;
    if (sqlite3_prepare_v2(cat->db,
                           retired ? "UPDATE media SET retired_ms=COALESCE(retired_ms, ?2), replaced_by=?3 WHERE id=?1"
                                   : "UPDATE media SET retired_ms=NULL, replaced_by=NULL WHERE id=?1",
                           -1, &u, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "change the drive");
    sqlite3_bind_int64(u, 1, media_id);
    if (retired) {
        sqlite3_bind_int64(u, 2, bd_now_ms());
        if (replacement_id) sqlite3_bind_int64(u, 3, replacement_id);
        else sqlite3_bind_null(u, 3);
    }
    bd_status s = sqlite3_step(u) == SQLITE_DONE ? BD_OK : bd_fail_db(cat, "change the drive");
    sqlite3_finalize(u);
    return s;
}

bd_status bd_media_retire(bd_catalog *cat, int64_t media_id, int64_t replacement_id)
{
    if (!media_exists(cat, media_id)) return bd_fail(cat, BD_ERR_NOT_FOUND, "no such drive");
    if (replacement_id) {
        if (replacement_id == media_id) return bd_fail(cat, BD_ERR_INVALID, "a drive cannot replace itself");
        if (!media_exists(cat, replacement_id)) return bd_fail(cat, BD_ERR_NOT_FOUND, "no such drive");
        if (bd_media_retired(cat, replacement_id))
            return bd_fail(cat, BD_ERR_INVALID, "that drive is being replaced itself; choose another one");
    }
    return set_retired(cat, media_id, 1, replacement_id);
}

bd_status bd_media_unretire(bd_catalog *cat, int64_t media_id)
{
    if (!media_exists(cat, media_id)) return bd_fail(cat, BD_ERR_NOT_FOUND, "no such drive");
    return set_retired(cat, media_id, 0, 0);
}

/* Good copies on old_id of versions with no good copy anywhere else, in
 * three groups: current versions, deleted files, older versions. */
static void count_only_old(bd_catalog *cat, int64_t old_id, bd_replace_stats *stats)
{
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT COALESCE(SUM(n.deleted=0 AND n.current_version_id=v.id),0),"
                           " COALESCE(SUM(n.deleted<>0 AND n.current_version_id=v.id),0),"
                           " COALESCE(SUM(n.current_version_id IS NOT v.id),0)"
                           " FROM copies c JOIN versions v ON v.id=c.version_id JOIN nodes n ON n.id=v.node_id"
                           " WHERE c.media_id=?1 AND c.state='ok'"
                           " AND NOT EXISTS(SELECT 1 FROM copies o WHERE o.version_id=v.id AND o.media_id<>?1 AND o.state='ok')",
                           -1, &q, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int64(q, 1, old_id);
    if (sqlite3_step(q) == SQLITE_ROW) {
        stats->files_only_old = sqlite3_column_int64(q, 0);
        stats->deleted_only_old = sqlite3_column_int64(q, 1);
        stats->versions_only_old = sqlite3_column_int64(q, 2);
    }
    sqlite3_finalize(q);
}

bd_status bd_media_replace(bd_catalog *cat, int64_t old_id, int64_t new_id, bd_replace_stats *stats,
                           bd_log_fn log, void *log_ctx)
{
    bd_replace_stats local;
    if (!stats) stats = &local;
    memset(stats, 0, sizeof(*stats));
    if (!new_id) return bd_fail(cat, BD_ERR_INVALID, "choose the drive that takes its place");
    /* Check what the copy needs before anything changes. */
    sqlite3_stmt *k;
    int connected = 0;
    if (sqlite3_prepare_v2(cat->db, "SELECT 1 FROM temp.connected WHERE media_id=?", -1, &k, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(k, 1, new_id);
        connected = sqlite3_step(k) == SQLITE_ROW;
        sqlite3_finalize(k);
    }
    if (!connected && media_exists(cat, new_id)) return bd_fail(cat, BD_ERR_NOT_FOUND, "plug in the drive that takes its place");
    if (connected && bd_media_encrypted(cat, new_id) && !cat->have_key)
        return bd_fail(cat, BD_ERR_PASSPHRASE, "enter the passphrase to back up to an encrypted drive");
    bd_status s = bd_media_retire(cat, old_id, new_id);
    if (s != BD_OK) return s;

    /* What needs the old drive: short of the target once it no longer counts. */
    bd_target t;
    bd_target_get(cat, &t);
    sqlite3_stmt *q;
    if (bd_exec(cat, "DELETE FROM temp.replace_need") != 0 ||
        sqlite3_prepare_v2(cat->db,
                           "INSERT INTO temp.replace_need(version_id)"
                           " SELECT n.current_version_id FROM nodes n"
                           " WHERE n.is_dir=0 AND n.deleted=0 AND n.current_version_id IS NOT NULL"
                           " AND EXISTS(SELECT 1 FROM copies c WHERE c.version_id=n.current_version_id"
                           "   AND c.media_id=?1 AND c.state='ok')"
                           " AND ((SELECT COUNT(DISTINCT c.media_id) FROM copies c WHERE c.version_id=n.current_version_id"
                           "   AND c.state='ok' AND c.media_id<>?1) < ?2"
                           "  OR (SELECT COUNT(DISTINCT " BD_SQL_PLACE("m") ") FROM copies c JOIN media m ON m.id=c.media_id"
                           "   WHERE c.version_id=n.current_version_id AND c.state='ok' AND c.media_id<>?1) < ?3)",
                           -1, &q, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "work out what is only on the old drive");
    sqlite3_bind_int64(q, 1, old_id);
    sqlite3_bind_int(q, 2, t.copies);
    sqlite3_bind_int(q, 3, t.places);
    int rc = sqlite3_step(q);
    stats->files_needed = rc == SQLITE_DONE ? sqlite3_changes(cat->db) : 0;
    sqlite3_finalize(q);
    if (rc != SQLITE_DONE) return bd_fail_db(cat, "work out what is only on the old drive");
    bd_logf(log, log_ctx, "%lld file(s) need the drive being replaced", (long long)stats->files_needed);

    bd_backup_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.replacing = old_id;
    s = stats->files_needed ? bd_backup_ex(cat, new_id, 0, &opts, &stats->backup, log, log_ctx) : BD_OK;
    bd_exec(cat, "DELETE FROM temp.replace_need");
    count_only_old(cat, old_id, stats);
    return s;
}
