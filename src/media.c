/* Storage targets. Each drive (or folder) used by a catalog holds
 * <root>/BRODALF/<catalog uuid>/BRODALF.media, a small text file naming the
 * media ID. BRODALF recognises drives by that ID, never by drive letter. */
#include "store.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

int bd_media_file_read(const char *path, bd_media_file *mf)
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

static int media_file_put(const char *path, const bd_media_file *mf, int replace)
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
    if (rc == 0) rc = replace ? bd_rename_replace(tmp, path) : bd_rename_noreplace(tmp, path);
    if (rc != 0) bd_remove(tmp);
    free(tmp);
    return rc;
}

/* Never replaces an existing ID file. */
int bd_media_file_write(const char *path, const bd_media_file *mf)
{
    return media_file_put(path, mf, 0);
}

static bd_status bd_media_file_path(bd_catalog *cat, const char *root, char **out)
{
    char *dir = bd_media_catalog_dir(cat, root);
    *out = dir ? bd_path_join(dir, BD_MEDIA_FILE) : NULL;
    free(dir);
    return *out ? BD_OK : BD_ERR_NOMEM;
}

bd_status bd_media_record_connected(bd_catalog *cat, int64_t media_id, const char *root)
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
    if (have_space) bd_media_note_space(cat, media_id, total, freeb, "plugged in");

    if (sqlite3_prepare_v2(cat->db, "INSERT OR REPLACE INTO temp.connected(media_id, root) VALUES(?,?)", -1, &u, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "record connected drive");
    sqlite3_bind_int64(u, 1, media_id);
    sqlite3_bind_text(u, 2, root, -1, SQLITE_STATIC);
    rc = sqlite3_step(u);
    sqlite3_finalize(u);
    return rc == SQLITE_DONE ? BD_OK : bd_fail_db(cat, "record connected drive");
}

void bd_media_note_space(bd_catalog *cat, int64_t media_id, int64_t total, int64_t free_bytes, const char *event)
{
    sqlite3_stmt *u;
    int64_t now = bd_now_ms();
    if (sqlite3_prepare_v2(cat->db, "UPDATE media SET total_bytes=?, free_bytes=? WHERE id=?", -1, &u, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(u, 1, total);
        sqlite3_bind_int64(u, 2, free_bytes);
        sqlite3_bind_int64(u, 3, media_id);
        sqlite3_step(u);
        sqlite3_finalize(u);
    }
    if (sqlite3_prepare_v2(cat->db, "INSERT INTO space_log(media_id, at_ms, total_bytes, free_bytes, event) VALUES(?,?,?,?,?)",
                           -1, &u, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(u, 1, media_id);
        sqlite3_bind_int64(u, 2, now);
        sqlite3_bind_int64(u, 3, total);
        sqlite3_bind_int64(u, 4, free_bytes);
        sqlite3_bind_text(u, 5, event, -1, SQLITE_STATIC);
        sqlite3_step(u);
        sqlite3_finalize(u);
    }
}

int64_t bd_media_id_for_uuid(bd_catalog *cat, const char *uuid)
{
    sqlite3_stmt *q;
    int64_t id = 0;
    if (sqlite3_prepare_v2(cat->db, "SELECT id FROM media WHERE uuid=?", -1, &q, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(q, 1, uuid, -1, SQLITE_STATIC);
    if (sqlite3_step(q) == SQLITE_ROW) id = sqlite3_column_int64(q, 0);
    sqlite3_finalize(q);
    return id;
}

int64_t bd_media_insert(bd_catalog *cat, const char *uuid, const char *kind, const char *label, int encrypted)
{
    sqlite3_stmt *ins;
    if (sqlite3_prepare_v2(cat->db, "INSERT INTO media(uuid, kind, label, added_ms, encrypted) VALUES(?, ?, ?, ?, ?)", -1, &ins, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_text(ins, 1, uuid, -1, SQLITE_STATIC);
    sqlite3_bind_text(ins, 2, kind, -1, SQLITE_STATIC);
    sqlite3_bind_text(ins, 3, label, -1, SQLITE_STATIC);
    sqlite3_bind_int64(ins, 4, bd_now_ms());
    sqlite3_bind_int(ins, 5, encrypted);
    int64_t id = sqlite3_step(ins) == SQLITE_DONE ? sqlite3_last_insert_rowid(cat->db) : 0;
    sqlite3_finalize(ins);
    return id;
}

static void text_or_null(sqlite3_stmt *st, int col, const char *s)
{
    if (s && *s) sqlite3_bind_text(st, col, s, -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(st, col);
}

static void num_or_null(sqlite3_stmt *st, int col, int64_t v)
{
    if (v >= 0) sqlite3_bind_int64(st, col, v);
    else sqlite3_bind_null(st, col);
}

void bd_media_store_hw(bd_catalog *cat, int64_t media_id, const bd_drive_hw *hw, bd_log_fn log, void *log_ctx)
{
    sqlite3_stmt *q;
    if (hw->serial[0] &&
        sqlite3_prepare_v2(cat->db, "SELECT h.serial, h.model, m.label FROM media_hardware h JOIN media m ON m.id=h.media_id WHERE h.media_id=?",
                           -1, &q, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(q, 1, media_id);
        if (sqlite3_step(q) == SQLITE_ROW && sqlite3_column_text(q, 0) &&
            strcmp((const char *)sqlite3_column_text(q, 0), hw->serial) != 0)
            bd_logf(log, log_ctx, "\"%s\" is on a different disk than last time (serial %s, was %s %s)",
                    (const char *)sqlite3_column_text(q, 2), hw->serial,
                    sqlite3_column_text(q, 1) ? (const char *)sqlite3_column_text(q, 1) : "",
                    (const char *)sqlite3_column_text(q, 0));
        sqlite3_finalize(q);
    }
    if (sqlite3_prepare_v2(cat->db,
                           "INSERT OR REPLACE INTO media_hardware(media_id, read_ms, vendor, model, serial, firmware, bus, disk_bytes,"
                           " volume_name, volume_serial, filesystem, smart, health, temperature_c, power_on_hours, power_cycles,"
                           " reallocated, pending, uncorrectable, percent_used, note)"
                           " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                           -1, &q, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int64(q, 1, media_id);
    sqlite3_bind_int64(q, 2, bd_now_ms());
    text_or_null(q, 3, hw->vendor);
    text_or_null(q, 4, hw->model);
    text_or_null(q, 5, hw->serial);
    text_or_null(q, 6, hw->firmware);
    text_or_null(q, 7, hw->bus);
    num_or_null(q, 8, hw->disk_bytes);
    text_or_null(q, 9, hw->volume_name);
    text_or_null(q, 10, hw->volume_serial);
    text_or_null(q, 11, hw->filesystem);
    sqlite3_bind_int(q, 12, hw->smart);
    text_or_null(q, 13, hw->health);
    num_or_null(q, 14, hw->temperature_c);
    num_or_null(q, 15, hw->power_on_hours);
    num_or_null(q, 16, hw->power_cycles);
    num_or_null(q, 17, hw->reallocated_sectors);
    num_or_null(q, 18, hw->pending_sectors);
    num_or_null(q, 19, hw->uncorrectable_sectors);
    num_or_null(q, 20, hw->percent_used);
    text_or_null(q, 21, hw->note);
    sqlite3_step(q);
    sqlite3_finalize(q);
}

static void read_hardware(bd_catalog *cat, int64_t media_id, const char *root, bd_log_fn log, void *log_ctx)
{
    bd_drive_hw hw;
    if (bd_drive_hw_read(root, &hw) == 0) bd_media_store_hw(cat, media_id, &hw, log, log_ctx);
}

static bd_status set_text(bd_catalog *cat, const char *sql, int64_t media_id, const char *text)
{
    sqlite3_stmt *u;
    if (sqlite3_prepare_v2(cat->db, sql, -1, &u, NULL) != SQLITE_OK) return bd_fail_db(cat, "update drive");
    if (text && *text) sqlite3_bind_text(u, 1, text, -1, SQLITE_STATIC);
    else sqlite3_bind_null(u, 1);
    sqlite3_bind_int64(u, 2, media_id);
    int rc = sqlite3_step(u);
    sqlite3_finalize(u);
    if (rc != SQLITE_DONE) return bd_fail_db(cat, "update drive");
    return sqlite3_changes(cat->db) ? BD_OK : bd_fail(cat, BD_ERR_NOT_FOUND, "no such drive");
}

bd_status bd_media_set_location(bd_catalog *cat, int64_t media_id, const char *location)
{
    return set_text(cat, "UPDATE media SET location=? WHERE id=?", media_id, location);
}

bd_status bd_media_rename(bd_catalog *cat, int64_t media_id, const char *label)
{
    if (!label || !*label) return bd_fail(cat, BD_ERR_INVALID, "give the drive a label");
    return set_text(cat, "UPDATE media SET label=? WHERE id=?", media_id, label);
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
    if (bd_media_file_path(cat, root, &path) != BD_OK) return BD_ERR_NOMEM;
    bd_media_file mf;
    if (bd_media_file_read(path, &mf) == 0) {
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

    int64_t id = bd_media_insert(cat, mf.media_uuid, "drive", mf.label, encrypted);
    if (!id) { free(path); return bd_fail_db(cat, "add drive"); }
    if (bd_media_file_write(path, &mf) != 0) {
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
    bd_status s = bd_media_record_connected(cat, id, root);
    if (s == BD_OK) read_hardware(cat, id, root, NULL, NULL);
    if (s == BD_OK && out_media_id) *out_media_id = id;
    return s;
}

bd_status bd_media_connect(bd_catalog *cat, const char *root, int64_t *out_media_id,
                           bd_check_stats *stats, bd_log_fn log, void *log_ctx)
{
    char *path;
    if (bd_media_file_path(cat, root, &path) != BD_OK) return BD_ERR_NOMEM;
    bd_media_file mf;
    int rc = bd_media_file_read(path, &mf);
    free(path);
    if (rc != 0) return bd_fail(cat, BD_ERR_NOT_FOUND, "%s has no BRODALF drive ID for this catalog", root);
    if (strcmp(mf.catalog_uuid, cat->uuid) != 0)
        return bd_fail(cat, BD_ERR_INVALID, "the drive ID on %s belongs to a different catalog", root);

    int64_t id = bd_media_id_for_uuid(cat, mf.media_uuid);
    if (!id) {
        /* The drive knows this catalog but the catalog lost the drive, for
         * example after restoring an older catalog backup. Re-register it. */
        id = bd_media_insert(cat, mf.media_uuid, "drive", mf.label[0] ? mf.label : "Recovered drive", mf.encrypted);
        if (!id) return bd_fail_db(cat, "re-register drive");
        bd_logf(log, log_ctx, "re-registered drive \"%s\"", mf.label);
    }
    bd_status s = bd_media_record_connected(cat, id, root);
    if (s != BD_OK) return s;
    /* A rename in the app reaches the drive's ID file here. */
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(cat->db, "SELECT label FROM media WHERE id=?", -1, &q, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(q, 1, id);
        if (sqlite3_step(q) == SQLITE_ROW && strcmp((const char *)sqlite3_column_text(q, 0), mf.label) != 0) {
            snprintf(mf.label, sizeof(mf.label), "%s", (const char *)sqlite3_column_text(q, 0));
            char *p = NULL;
            if (bd_media_file_path(cat, root, &p) == BD_OK) media_file_put(p, &mf, 1);
            free(p);
        }
        sqlite3_finalize(q);
    }
    read_hardware(cat, id, root, log, log_ctx);
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

/* due_before > 0: only copies not read back since then (or never), each
 * rehashed. Otherwise every copy, rehashed when full is set. */
static bd_status check_copies(bd_catalog *cat, int64_t media_id, int full, int64_t due_before,
                              bd_check_stats *stats, bd_log_fn log, void *log_ctx);

bd_status bd_media_check(bd_catalog *cat, int64_t media_id, int full,
                         bd_check_stats *stats, bd_log_fn log, void *log_ctx)
{
    return check_copies(cat, media_id, full, 0, stats, log, log_ctx);
}

bd_status bd_media_verify(bd_catalog *cat, int64_t media_id, int max_age_days,
                          bd_check_stats *stats, bd_log_fn log, void *log_ctx)
{
    int64_t due = max_age_days > 0 ? bd_now_ms() - (int64_t)max_age_days * 24 * 3600 * 1000 : INT64_MAX;
    return check_copies(cat, media_id, 1, due, stats, log, log_ctx);
}

static bd_status check_copies(bd_catalog *cat, int64_t media_id, int full, int64_t due_before,
                              bd_check_stats *stats, bd_log_fn log, void *log_ctx)
{
    bd_check_stats local;
    if (!stats) stats = &local;
    memset(stats, 0, sizeof(*stats));

    int encrypted = bd_media_encrypted(cat, media_id);
    const uint8_t *key = encrypted ? cat->key : NULL;
    if (encrypted && full && !cat->have_key)
        return bd_fail(cat, BD_ERR_PASSPHRASE, "enter the passphrase to check an encrypted drive");
    bd_store *store = bd_store_open(cat, media_id);
    if (!store) return BD_ERR_NOT_FOUND;

    sqlite3_stmt *q = NULL, *u = NULL;
    bd_status s = BD_OK;
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT c.id, c.path_on_media, c.stored_size, c.stored_mtime_ns, v.hash, COALESCE(c.stored_rev,'')"
                           " FROM copies c JOIN versions v ON v.id=c.version_id WHERE c.media_id=?1"
                           " AND (?2=0 OR (c.state<>'missing' AND COALESCE(c.last_full_check_ms,0)<?2)) ORDER BY c.path_on_media",
                           -1, &q, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(cat->db,
                           "UPDATE copies SET state=?, stored_mtime_ns=?, stored_rev=?, last_quick_check_ms=?,"
                           " last_full_check_ms=CASE WHEN ? THEN ? ELSE last_full_check_ms END WHERE id=?",
                           -1, &u, NULL) != SQLITE_OK) {
        s = bd_fail_db(cat, "prepare drive check");
        goto done;
    }
    /* Totals for the progress bar. */
    sqlite3_stmt *t;
    int64_t total_files = 0, total_bytes = 0;
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT COUNT(*), COALESCE(SUM(c.stored_size),0) FROM copies c WHERE c.media_id=?1"
                           " AND (?2=0 OR (c.state<>'missing' AND COALESCE(c.last_full_check_ms,0)<?2))",
                           -1, &t, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(t, 1, media_id);
        sqlite3_bind_int64(t, 2, due_before);
        if (sqlite3_step(t) == SQLITE_ROW) {
            total_files = sqlite3_column_int64(t, 0);
            total_bytes = sqlite3_column_int64(t, 1);
        }
        sqlite3_finalize(t);
    }
    bd_progress_begin(cat, due_before ? "verify" : full ? "verify" : "check", total_files, full ? total_bytes : 0);

    if (bd_exec(cat, "BEGIN") != 0) { s = bd_fail_db(cat, "begin drive check"); goto done; }
    sqlite3_bind_int64(q, 1, media_id);
    sqlite3_bind_int64(q, 2, due_before);
    int64_t now = bd_now_ms();
    while (sqlite3_step(q) == SQLITE_ROW) {
        int64_t copy_id = sqlite3_column_int64(q, 0);
        const char *rel = (const char *)sqlite3_column_text(q, 1);
        int64_t stored_size = sqlite3_column_int64(q, 2);
        int64_t stored_mtime = sqlite3_column_int64(q, 3);
        const char *expected = (const char *)sqlite3_column_text(q, 4);
        char stored_rev[160];
        snprintf(stored_rev, sizeof(stored_rev), "%s", (const char *)sqlite3_column_text(q, 5));
        stats->copies++;
        bd_report(cat, stats->copies, rel, 0);

        bd_remote_stat st;
        int found = store->ops->stat(store, rel, &st);
        if (found < 0) {
            /* Could not ask (network trouble): leave the copy as it was. */
            bd_logf(log, log_ctx, "cannot check %s: %s", rel, store->err);
            stats->skipped++;
            continue;
        }
        const char *state = "ok";
        int hashed = 0;
        int64_t mtime = stored_mtime;
        const char *rev = stored_rev;
        /* Local drives: a changed time means reread. Cloud: a changed content hash. */
        int touched = store->is_local ? st.mtime_ns != stored_mtime : (st.rev[0] && strcmp(st.rev, stored_rev) != 0);
        if (found == 1) {
            state = "missing";
        } else if (st.size != stored_size) {
            state = "bad";
        } else if (encrypted && !cat->have_key && touched) {
            /* Touched since it was written, but reading it needs the key.
             * Leave its state alone until a check with the passphrase. */
            stats->skipped++;
            continue;
        } else if (full || touched) {
            char hash[BD_HASH_HEX_LEN + 1];
            char *path = NULL;
            int is_temp = 0;
            hashed = 1;
            stats->rehashed++;
            int got = bd_store_fetch(store, rel, &path, &is_temp);
            if (got < 0) {
                bd_logf(log, log_ctx, "cannot read %s: %s", rel, store->err);
                stats->skipped++;
                continue;
            }
            if (got != 0 || bd_hash_copy(path, key, NULL, NULL, hash, NULL) != 0 || strcmp(hash, expected) != 0) state = "bad";
            else { mtime = st.mtime_ns; if (st.rev[0]) rev = st.rev; }
            bd_store_release(path, is_temp);
        }
        if (strcmp(state, "ok") == 0) stats->ok++;
        else if (strcmp(state, "missing") == 0) { stats->missing++; bd_logf(log, log_ctx, "missing on drive: %s", rel); }
        else { stats->bad++; bd_logf(log, log_ctx, "damaged on drive: %s", rel); }

        sqlite3_reset(u);
        sqlite3_bind_text(u, 1, state, -1, SQLITE_STATIC);
        sqlite3_bind_int64(u, 2, mtime);
        sqlite3_bind_text(u, 3, rev, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(u, 4, now);
        sqlite3_bind_int(u, 5, hashed);
        sqlite3_bind_int64(u, 6, now);
        sqlite3_bind_int64(u, 7, copy_id);
        if (sqlite3_step(u) != SQLITE_DONE) { s = bd_fail_db(cat, "record drive check"); break; }
    }
    if (s == BD_OK && bd_exec(cat, "COMMIT") != 0) s = bd_fail_db(cat, "commit drive check");
    if (s != BD_OK) bd_exec(cat, "ROLLBACK");
    bd_report(cat, stats->copies, NULL, 1);
    bd_progress_end(cat);
done:
    sqlite3_finalize(q);
    sqlite3_finalize(u);
    bd_store_close(store);
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
static bd_status put_catalog_copy(bd_catalog *cat, int64_t media_id);

bd_status bd_catalog_copy_to_media(bd_catalog *cat, int64_t media_id)
{
    bd_status s = bd_catalog_save(cat);
    return s == BD_OK ? put_catalog_copy(cat, media_id) : s;
}

bd_status bd_catalog_save_all(bd_catalog *cat, bd_log_fn log, void *log_ctx)
{
    bd_status s = bd_catalog_save(cat);
    if (s != BD_OK) return s;
    int64_t ids[64];
    int n = 0;
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(cat->db, "SELECT m.id FROM media m JOIN temp.connected k ON k.media_id=m.id WHERE m.kind<>'drive'",
                           -1, &q, NULL) != SQLITE_OK)
        return BD_OK;
    while (n < 64 && sqlite3_step(q) == SQLITE_ROW) ids[n++] = sqlite3_column_int64(q, 0);
    sqlite3_finalize(q);
    for (int i = 0; i < n; i++) {
        if (put_catalog_copy(cat, ids[i]) != BD_OK && log) {
            char *msg = bd_sprintf("catalog not copied to the cloud: %s", bd_catalog_error(cat));
            if (msg) { log(log_ctx, msg); free(msg); }
        }
    }
    return BD_OK;
}

static bd_status put_catalog_copy(bd_catalog *cat, int64_t media_id)
{
    bd_status s = BD_OK;
    int encrypt = bd_catalog_file_encrypted(cat) || bd_media_encrypted(cat, media_id);
    bd_store *store = bd_store_open(cat, media_id);
    if (!store) return BD_ERR_NOT_FOUND;
    const char *rel = "catalog-backup.brodalf";
    char *staged = store->ops->staging_path(store, "catalog-backup.brodalf" BD_TMP_MARKER);
    bd_remote_stat st;
    if (!staged) s = bd_fail(cat, BD_ERR_IO, "cannot copy the catalog: %s", store->err);
    else if ((s = bd_catalog_save_to(cat, staged, encrypt)) == BD_OK &&
             store->ops->upload(store, staged, rel, &st) != 0)
        s = bd_fail(cat, BD_ERR_IO, "cannot copy the catalog: %s", store->err);
    if (staged && s != BD_OK) bd_remove(staged);
    free(staged);
    bd_store_close(store);
    return s;
}

bd_store *bd_store_open(bd_catalog *cat, int64_t media_id)
{
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(cat->db, "SELECT m.kind, k.root FROM media m LEFT JOIN temp.connected k ON k.media_id=m.id WHERE m.id=?",
                           -1, &q, NULL) != SQLITE_OK) {
        bd_fail_db(cat, "find drive");
        return NULL;
    }
    sqlite3_bind_int64(q, 1, media_id);
    bd_store *s = NULL;
    if (sqlite3_step(q) != SQLITE_ROW) {
        bd_fail(cat, BD_ERR_NOT_FOUND, "no such drive");
    } else if (sqlite3_column_type(q, 1) == SQLITE_NULL) {
        bd_fail(cat, BD_ERR_NOT_FOUND, "that drive is not connected");
    } else if (strcmp((const char *)sqlite3_column_text(q, 0), "drive") == 0) {
        s = bd_store_local(cat, media_id, (const char *)sqlite3_column_text(q, 1));
        if (!s) bd_fail(cat, BD_ERR_NOMEM, "out of memory");
    } else {
        s = bd_cloud_store_open(cat, media_id);
    }
    sqlite3_finalize(q);
    return s;
}
