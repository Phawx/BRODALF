/* Storage targets. Each drive (or folder) used by a catalog holds
 * <root>/BRODALF/<catalog uuid>/BRODALF.media, a small text file naming the
 * media ID. BRODALF recognises drives by that ID, never by drive letter. */
#include "internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    char media_uuid[37];
    char catalog_uuid[37];
    char label[256];
    int encrypted;
} media_file;

static int read_media_file(const char *path, media_file *mf)
{
    memset(mf, 0, sizeof(*mf));
    FILE *f = bd_fopen(path, "rb");
    if (!f) return -1;
    char line[512];
    int magic = 0;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *v = eq + 1;
        if (strcmp(line, "brodalf_media") == 0) magic = 1;
        else if (strcmp(line, "media_uuid") == 0) snprintf(mf->media_uuid, sizeof(mf->media_uuid), "%s", v);
        else if (strcmp(line, "catalog_uuid") == 0) snprintf(mf->catalog_uuid, sizeof(mf->catalog_uuid), "%s", v);
        else if (strcmp(line, "label") == 0) snprintf(mf->label, sizeof(mf->label), "%s", v);
        else if (strcmp(line, "encrypted") == 0) mf->encrypted = strcmp(v, "1") == 0;
    }
    fclose(f);
    return (magic && strlen(mf->media_uuid) == 36) ? 0 : -1;
}

static int write_media_file(const char *path, const media_file *mf)
{
    char *tmp = bd_sprintf("%s" BD_TMP_MARKER, path);
    if (!tmp) return -1;
    FILE *f = bd_fopen(tmp, "wb");
    if (!f) { free(tmp); return -1; }
    fprintf(f,
            "brodalf_media=1\n"
            "media_uuid=%s\n"
            "catalog_uuid=%s\n"
            "label=%s\n"
            "encrypted=%d\n"
            "created_ms=%lld\n"
            "# This drive holds BRODALF backups. Do not delete this file.\n",
            mf->media_uuid, mf->catalog_uuid, mf->label, mf->encrypted, (long long)bd_now_ms());
    int rc = bd_fsync(f);
    if (fclose(f) != 0) rc = -1;
    if (rc == 0) rc = bd_rename_noreplace(tmp, path);
    if (rc != 0) bd_remove(tmp);
    free(tmp);
    return rc;
}

static bd_status media_file_path(bd_catalog *cat, const char *root, char **out)
{
    char *dir = bd_media_catalog_dir(cat, root);
    *out = dir ? bd_path_join(dir, BD_MEDIA_FILE) : NULL;
    free(dir);
    return *out ? BD_OK : BD_ERR_NOMEM;
}

static bd_status record_connected(bd_catalog *cat, int64_t media_id, const char *root)
{
    int64_t total = 0, freeb = 0;
    int have_space = bd_disk_space(root, &total, &freeb) == 0;
    sqlite3_stmt *u;
    if (sqlite3_prepare_v2(cat->db,
                           "UPDATE media SET last_root=?, last_seen_ms=?, total_bytes=COALESCE(?,total_bytes),"
                           " free_bytes=COALESCE(?,free_bytes) WHERE id=?",
                           -1, &u, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "update drive");
    sqlite3_bind_text(u, 1, root, -1, SQLITE_STATIC);
    sqlite3_bind_int64(u, 2, bd_now_ms());
    if (have_space) { sqlite3_bind_int64(u, 3, total); sqlite3_bind_int64(u, 4, freeb); }
    else { sqlite3_bind_null(u, 3); sqlite3_bind_null(u, 4); }
    sqlite3_bind_int64(u, 5, media_id);
    int rc = sqlite3_step(u);
    sqlite3_finalize(u);
    if (rc != SQLITE_DONE) return bd_fail_db(cat, "update drive");

    if (sqlite3_prepare_v2(cat->db, "INSERT OR REPLACE INTO temp.connected(media_id, root) VALUES(?,?)", -1, &u, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "record connected drive");
    sqlite3_bind_int64(u, 1, media_id);
    sqlite3_bind_text(u, 2, root, -1, SQLITE_STATIC);
    rc = sqlite3_step(u);
    sqlite3_finalize(u);
    return rc == SQLITE_DONE ? BD_OK : bd_fail_db(cat, "record connected drive");
}

static int64_t media_id_for_uuid(bd_catalog *cat, const char *uuid)
{
    sqlite3_stmt *q;
    int64_t id = 0;
    if (sqlite3_prepare_v2(cat->db, "SELECT id FROM media WHERE uuid=?", -1, &q, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(q, 1, uuid, -1, SQLITE_STATIC);
    if (sqlite3_step(q) == SQLITE_ROW) id = sqlite3_column_int64(q, 0);
    sqlite3_finalize(q);
    return id;
}

static int64_t insert_media(bd_catalog *cat, const char *uuid, const char *label, int encrypted)
{
    sqlite3_stmt *ins;
    if (sqlite3_prepare_v2(cat->db, "INSERT INTO media(uuid, kind, label, added_ms, encrypted) VALUES(?, 'drive', ?, ?, ?)", -1, &ins, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_text(ins, 1, uuid, -1, SQLITE_STATIC);
    sqlite3_bind_text(ins, 2, label, -1, SQLITE_STATIC);
    sqlite3_bind_int64(ins, 3, bd_now_ms());
    sqlite3_bind_int(ins, 4, encrypted);
    int64_t id = sqlite3_step(ins) == SQLITE_DONE ? sqlite3_last_insert_rowid(cat->db) : 0;
    sqlite3_finalize(ins);
    return id;
}

bd_status bd_media_init(bd_catalog *cat, const char *root, const char *label, unsigned flags, int64_t *out_media_id)
{
    int encrypted = (flags & BD_MEDIA_ENCRYPTED) != 0;
    if (encrypted && !cat->have_key)
        return bd_fail(cat, BD_ERR_PASSPHRASE, bd_catalog_has_passphrase(cat) ? "enter the passphrase to set up an encrypted drive"
                                                                               : "set a passphrase to set up an encrypted drive");
    bd_stat_t st;
    if (bd_stat(root, &st) != 0 || !st.is_dir) return bd_fail(cat, BD_ERR_NOT_FOUND, "%s is not a drive or folder", root);
    if (!label || !*label) return bd_fail(cat, BD_ERR_INVALID, "give the drive a label");

    char *path;
    if (media_file_path(cat, root, &path) != BD_OK) return BD_ERR_NOMEM;
    media_file mf;
    if (read_media_file(path, &mf) == 0) {
        free(path);
        return bd_fail(cat, BD_ERR_EXISTS, "%s is already set up for this catalog as \"%s\"", root, mf.label);
    }

    char *dir = bd_media_catalog_dir(cat, root);
    if (!dir || bd_mkdirs(dir) != 0) {
        free(dir);
        free(path);
        return bd_fail(cat, BD_ERR_IO, "cannot create the BRODALF folder on %s", root);
    }
    free(dir);

    memset(&mf, 0, sizeof(mf));
    bd_uuid_v4(mf.media_uuid);
    snprintf(mf.catalog_uuid, sizeof(mf.catalog_uuid), "%s", cat->uuid);
    snprintf(mf.label, sizeof(mf.label), "%s", label);
    for (char *p = mf.label; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
    mf.encrypted = encrypted;

    int64_t id = insert_media(cat, mf.media_uuid, mf.label, encrypted);
    if (!id) { free(path); return bd_fail_db(cat, "add drive"); }
    if (write_media_file(path, &mf) != 0) {
        free(path);
        sqlite3_stmt *d;
        if (sqlite3_prepare_v2(cat->db, "DELETE FROM media WHERE id=?", -1, &d, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(d, 1, id);
            sqlite3_step(d);
            sqlite3_finalize(d);
        }
        return bd_fail(cat, BD_ERR_IO, "cannot write the drive ID file on %s", root);
    }
    free(path);
    bd_status s = record_connected(cat, id, root);
    if (s == BD_OK && out_media_id) *out_media_id = id;
    return s;
}

bd_status bd_media_connect(bd_catalog *cat, const char *root, int64_t *out_media_id,
                           bd_check_stats *stats, bd_log_fn log, void *log_ctx)
{
    char *path;
    if (media_file_path(cat, root, &path) != BD_OK) return BD_ERR_NOMEM;
    media_file mf;
    int rc = read_media_file(path, &mf);
    free(path);
    if (rc != 0) return bd_fail(cat, BD_ERR_NOT_FOUND, "%s has no BRODALF drive ID for this catalog", root);
    if (strcmp(mf.catalog_uuid, cat->uuid) != 0)
        return bd_fail(cat, BD_ERR_INVALID, "the drive ID on %s belongs to a different catalog", root);

    int64_t id = media_id_for_uuid(cat, mf.media_uuid);
    if (!id) {
        /* The drive knows this catalog but the catalog lost the drive, for
         * example after restoring an older catalog backup. Re-register it. */
        id = insert_media(cat, mf.media_uuid, mf.label[0] ? mf.label : "Recovered drive", mf.encrypted);
        if (!id) return bd_fail_db(cat, "re-register drive");
        bd_logf(log, log_ctx, "re-registered drive \"%s\"", mf.label);
    }
    bd_status s = record_connected(cat, id, root);
    if (s != BD_OK) return s;
    if (out_media_id) *out_media_id = id;
    return bd_media_check(cat, id, 0, stats, log, log_ctx);
}

void bd_media_disconnect(bd_catalog *cat, int64_t media_id)
{
    sqlite3_stmt *d;
    if (sqlite3_prepare_v2(cat->db, "DELETE FROM temp.connected WHERE media_id=?", -1, &d, NULL) != SQLITE_OK) return;
    sqlite3_bind_int64(d, 1, media_id);
    sqlite3_step(d);
    sqlite3_finalize(d);
}

bd_status bd_media_check(bd_catalog *cat, int64_t media_id, int full,
                         bd_check_stats *stats, bd_log_fn log, void *log_ctx)
{
    bd_check_stats local;
    if (!stats) stats = &local;
    memset(stats, 0, sizeof(*stats));

    char *root = bd_connected_root(cat, media_id);
    if (!root) return bd_fail(cat, BD_ERR_NOT_FOUND, "that drive is not connected");
    char *dir = bd_media_catalog_dir(cat, root);
    free(root);
    if (!dir) return BD_ERR_NOMEM;

    int encrypted = bd_media_encrypted(cat, media_id);
    const uint8_t *key = encrypted ? cat->key : NULL;
    if (encrypted && full && !cat->have_key) {
        free(dir);
        return bd_fail(cat, BD_ERR_PASSPHRASE, "enter the passphrase to check an encrypted drive");
    }

    sqlite3_stmt *q = NULL, *u = NULL;
    bd_status s = BD_OK;
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT c.id, c.path_on_media, c.stored_size, c.stored_mtime_ns, v.hash"
                           " FROM copies c JOIN versions v ON v.id=c.version_id WHERE c.media_id=?",
                           -1, &q, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(cat->db,
                           "UPDATE copies SET state=?, stored_mtime_ns=?, last_quick_check_ms=?,"
                           " last_full_check_ms=CASE WHEN ? THEN ? ELSE last_full_check_ms END WHERE id=?",
                           -1, &u, NULL) != SQLITE_OK) {
        s = bd_fail_db(cat, "prepare drive check");
        goto done;
    }
    if (bd_exec(cat, "BEGIN") != 0) { s = bd_fail_db(cat, "begin drive check"); goto done; }
    sqlite3_bind_int64(q, 1, media_id);
    int64_t now = bd_now_ms();
    while (sqlite3_step(q) == SQLITE_ROW) {
        int64_t copy_id = sqlite3_column_int64(q, 0);
        const char *rel = (const char *)sqlite3_column_text(q, 1);
        int64_t stored_size = sqlite3_column_int64(q, 2);
        int64_t stored_mtime = sqlite3_column_int64(q, 3);
        const char *expected = (const char *)sqlite3_column_text(q, 4);
        stats->copies++;
        bd_report(cat, full ? "full check" : "check", stats->copies, 0, rel, 0);

        char *path = bd_path_join(dir, rel);
        bd_stat_t st;
        const char *state = "ok";
        int hashed = 0;
        int64_t mtime = stored_mtime;
        if (!path || bd_stat(path, &st) != 0 || !st.is_file) {
            state = "missing";
        } else if (st.size != stored_size) {
            state = "bad";
        } else if (encrypted && !cat->have_key && st.mtime_ns != stored_mtime) {
            /* Touched since it was written, but reading it needs the key.
             * Leave its state alone until a check with the passphrase. */
            free(path);
            stats->skipped++;
            continue;
        } else if (full || st.mtime_ns != stored_mtime) {
            char hash[BD_HASH_HEX_LEN + 1];
            hashed = 1;
            stats->rehashed++;
            if (bd_hash_copy(path, key, NULL, NULL, hash, NULL) != 0 || strcmp(hash, expected) != 0) state = "bad";
            else mtime = st.mtime_ns;
        }
        free(path);
        if (strcmp(state, "ok") == 0) stats->ok++;
        else if (strcmp(state, "missing") == 0) { stats->missing++; bd_logf(log, log_ctx, "missing on drive: %s", rel); }
        else { stats->bad++; bd_logf(log, log_ctx, "damaged on drive: %s", rel); }

        sqlite3_reset(u);
        sqlite3_bind_text(u, 1, state, -1, SQLITE_STATIC);
        sqlite3_bind_int64(u, 2, mtime);
        sqlite3_bind_int64(u, 3, now);
        sqlite3_bind_int(u, 4, hashed);
        sqlite3_bind_int64(u, 5, now);
        sqlite3_bind_int64(u, 6, copy_id);
        if (sqlite3_step(u) != SQLITE_DONE) { s = bd_fail_db(cat, "record drive check"); break; }
    }
    if (s == BD_OK && bd_exec(cat, "COMMIT") != 0) s = bd_fail_db(cat, "commit drive check");
    if (s != BD_OK) bd_exec(cat, "ROLLBACK");
done:
    sqlite3_finalize(q);
    sqlite3_finalize(u);
    free(dir);
    return s;
}

int bd_media_encrypted(bd_catalog *cat, int64_t media_id)
{
    sqlite3_stmt *q;
    int enc = 0;
    if (sqlite3_prepare_v2(cat->db, "SELECT encrypted FROM media WHERE id=?", -1, &q, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int64(q, 1, media_id);
    if (sqlite3_step(q) == SQLITE_ROW) enc = sqlite3_column_int(q, 0);
    sqlite3_finalize(q);
    return enc;
}

/* The catalog backup on an encrypted drive is always encrypted. */
bd_status bd_catalog_copy_to_media(bd_catalog *cat, int64_t media_id)
{
    bd_status s = bd_catalog_save(cat);
    if (s != BD_OK) return s;
    int encrypt = bd_catalog_file_encrypted(cat) || bd_media_encrypted(cat, media_id);
    char *root = bd_connected_root(cat, media_id);
    if (!root) return bd_fail(cat, BD_ERR_NOT_FOUND, "that drive is not connected");
    char *dir = bd_media_catalog_dir(cat, root);
    free(root);
    char *dst = dir ? bd_path_join(dir, "catalog-backup.brodalf") : NULL;
    free(dir);
    if (!dst) return BD_ERR_NOMEM;
    s = bd_catalog_save_to(cat, dst, encrypt);
    free(dst);
    return s;
}
