/* Storage targets. Each drive (or folder) used by a catalog holds
 * <root>/BRODALF/<catalog uuid>/BRODALF.media, a small text file naming the
 * media ID. BRODALF recognises drives by that ID, never by drive letter. */
#include "store.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#ifdef _WIN32
#include <windows.h>
#endif

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
        else if (strcmp(line, "secondary") == 0) mf->secondary = strcmp(v, "1") == 0;
        else if (strcmp(line, "hide_names") == 0) mf->hide_names = strcmp(v, "1") == 0;
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
            "%s%s"
            "created_ms=%lld\n"
            "# This drive holds BRODALF backups. Do not delete this file.\n",
            mf->media_uuid, mf->catalog_uuid, mf->label, mf->encrypted, mf->secondary ? "secondary=1\n" : "",
            mf->hide_names ? "hide_names=1\n" : "", (long long)bd_now_ms());
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
    /* Where it was last, as a full path: found again from any folder. */
    char *full = bd_full_path(root);
    sqlite3_bind_text(u, 1, full ? full : root, -1, SQLITE_TRANSIENT);
    free(full);
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

static void media_set_hide_names(bd_catalog *cat, int64_t media_id)
{
    sqlite3_stmt *u;
    if (sqlite3_prepare_v2(cat->db, "UPDATE media SET hide_names=1 WHERE id=?", -1, &u, NULL) != SQLITE_OK) return;
    sqlite3_bind_int64(u, 1, media_id);
    sqlite3_step(u);
    sqlite3_finalize(u);
    bd_catalog_note_schema(cat);
}

int bd_media_hides_names(bd_catalog *cat, int64_t media_id, uint8_t out[32])
{
    sqlite3_stmt *q;
    int hide = 0;
    if (sqlite3_prepare_v2(cat->db, "SELECT uuid, hide_names FROM media WHERE id=?", -1, &q, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int64(q, 1, media_id);
    if (sqlite3_step(q) == SQLITE_ROW && sqlite3_column_int(q, 1)) {
        hide = 1;
        if (out && cat->have_key) bd_names_key(cat->key, (const char *)sqlite3_column_text(q, 0), out);
        else if (out) memset(out, 0, 32);
    }
    sqlite3_finalize(q);
    return hide;
}

static void media_set_secondary(bd_catalog *cat, int64_t media_id)
{
    sqlite3_stmt *u;
    if (sqlite3_prepare_v2(cat->db, "UPDATE media SET secondary=1 WHERE id=?", -1, &u, NULL) != SQLITE_OK) return;
    sqlite3_bind_int64(u, 1, media_id);
    sqlite3_step(u);
    sqlite3_finalize(u);
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

/* A serial number that tells one disk from another (as BD_SQL_HW_KEY). */
static int serial_tells(const char *s)
{
    if (!s) return 0;
    while (*s == ' ') s++;
    size_t n = strlen(s);
    while (n && s[n - 1] == ' ') n--;
    if (n < 4) return 0;
    for (size_t i = 0; i < n; i++)
        if (s[i] != '0' && s[i] != ' ') return 1;
    return 0;
}

static int same_serial(const char *a, const char *b)
{
    while (*a == ' ') a++;
    while (*b == ' ') b++;
    size_t na = strlen(a), nb = strlen(b);
    while (na && a[na - 1] == ' ') na--;
    while (nb && b[nb - 1] == ' ') nb--;
    if (na != nb) return 0;
    for (size_t i = 0; i < na; i++) {
        char x = a[i], y = b[i];
        if (x >= 'a' && x <= 'z') x = (char)(x - 32);
        if (y >= 'a' && y <= 'z') y = (char)(y - 32);
        if (x != y) return 0;
    }
    return 1;
}

/* The drive's ID turned up where it should not (media_suspect): why 1 on
 * two disks at once, 2 on a disk with another serial number than last
 * time, as when it is cloned. Its copies stop counting until a check of
 * every copy on it (bd_media_check). */
static void note_suspect(bd_catalog *cat, int64_t media_id, int why, const char *root, const char *other_root)
{
    sqlite3_stmt *q;
    /* Two disks at once says more than a new serial number: it is kept. */
    if (sqlite3_prepare_v2(cat->db,
                           why == 1 ? "INSERT OR REPLACE INTO media_suspect(media_id, why, found_ms, root, other_root) VALUES(?,?,?,?,?)"
                                    : "INSERT OR IGNORE INTO media_suspect(media_id, why, found_ms, root, other_root) VALUES(?,?,?,?,?)",
                           -1, &q, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int64(q, 1, media_id);
    sqlite3_bind_int(q, 2, why);
    sqlite3_bind_int64(q, 3, bd_now_ms());
    text_or_null(q, 4, root);
    text_or_null(q, 5, other_root);
    sqlite3_step(q);
    sqlite3_finalize(q);
    bd_refresh_uncounted(cat);
}

static int same_root(const char *a, const char *b);
static int holds_media_id(bd_catalog *cat, const char *root, const char *uuid);

static void clear_suspect(bd_catalog *cat, int64_t media_id)
{
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(cat->db, "DELETE FROM media_suspect WHERE media_id=?", -1, &q, NULL) != SQLITE_OK) return;
    sqlite3_bind_int64(q, 1, media_id);
    sqlite3_step(q);
    int gone = sqlite3_changes(cat->db) > 0;
    sqlite3_finalize(q);
    if (gone) bd_refresh_uncounted(cat);
}

int bd_media_suspect(bd_catalog *cat, int64_t media_id, char **root, char **other_root)
{
    sqlite3_stmt *q;
    int why = 0;
    if (root) *root = NULL;
    if (other_root) *other_root = NULL;
    if (sqlite3_prepare_v2(cat->db, "SELECT why, root, other_root FROM media_suspect WHERE media_id=?", -1, &q, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_int64(q, 1, media_id);
    if (sqlite3_step(q) == SQLITE_ROW) {
        why = sqlite3_column_int(q, 0);
        if (root && sqlite3_column_text(q, 1)) *root = bd_strdup((const char *)sqlite3_column_text(q, 1));
        if (other_root && sqlite3_column_text(q, 2)) *other_root = bd_strdup((const char *)sqlite3_column_text(q, 2));
    }
    sqlite3_finalize(q);
    return why;
}

void bd_media_store_hw(bd_catalog *cat, int64_t media_id, const bd_drive_hw *hw, bd_log_fn log, void *log_ctx)
{
    sqlite3_stmt *q;
    if (hw->serial[0] &&
        sqlite3_prepare_v2(cat->db, "SELECT h.serial, h.model, m.label FROM media_hardware h JOIN media m ON m.id=h.media_id WHERE h.media_id=?",
                           -1, &q, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(q, 1, media_id);
        int other = 0;
        if (sqlite3_step(q) == SQLITE_ROW && sqlite3_column_text(q, 0) &&
            strcmp((const char *)sqlite3_column_text(q, 0), hw->serial) != 0) {
            bd_logf(log, log_ctx, "\"%s\" is on a different disk than last time (serial %s, was %s %s)",
                    (const char *)sqlite3_column_text(q, 2), hw->serial,
                    sqlite3_column_text(q, 1) ? (const char *)sqlite3_column_text(q, 1) : "",
                    (const char *)sqlite3_column_text(q, 0));
            other = serial_tells((const char *)sqlite3_column_text(q, 0)) && serial_tells(hw->serial) &&
                    !same_serial((const char *)sqlite3_column_text(q, 0), hw->serial);
            if (other)
                bd_logf(log, log_ctx, "\"%s\" may be a copy of the drive made by cloning it: its copies do not count until a "
                                      "check of every copy on it", (const char *)sqlite3_column_text(q, 2));
        }
        sqlite3_finalize(q);
        if (other) {
            char *root = bd_connected_root(cat, media_id);
            note_suspect(cat, media_id, 2, root, NULL);
            free(root);
        }
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

int (*bd_test_hw_read)(const char *root, bd_drive_hw *hw) = NULL;
void (*bd_test_spoil_catalog_copy)(const char *path) = NULL;

void bd_refresh_uncounted(bd_catalog *cat)
{
    bd_exec(cat, "DELETE FROM temp.uncounted;"
                 "INSERT OR IGNORE INTO temp.uncounted(media_id, why) SELECT m.id, 1 FROM media m WHERE m.kind='drive'"
                 " AND EXISTS(SELECT 1 FROM temp.root_disks rd WHERE " BD_SQL_HW_KEY("rd") "=" BD_SQL_DISK("m.id") ");"
                 "INSERT OR IGNORE INTO temp.uncounted(media_id, why) SELECT h.media_id, 2 FROM media_hardware h"
                 " WHERE h.health='failing';"
                 "INSERT OR IGNORE INTO temp.uncounted(media_id, why) SELECT media_id, 3 FROM media_suspect;");
}

static void read_hardware(bd_catalog *cat, int64_t media_id, const char *root, bd_log_fn log, void *log_ctx)
{
    bd_drive_hw hw;
    int rc = bd_test_hw_read ? bd_test_hw_read(root, &hw) : bd_drive_hw_read(root, &hw);
    if (rc == 0) {
        bd_media_store_hw(cat, media_id, &hw, log, log_ctx);
        bd_refresh_uncounted(cat);
    }
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

int64_t bd_media_named(bd_catalog *cat, const char *label, int64_t except_id)
{
    sqlite3_stmt *q;
    int64_t id = 0;
    if (!label) return 0;
    if (sqlite3_prepare_v2(cat->db, "SELECT id FROM media WHERE lower(trim(label))=lower(trim(?1)) AND id<>?2 ORDER BY id LIMIT 1", -1, &q,
                           NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_text(q, 1, label, -1, SQLITE_STATIC);
    sqlite3_bind_int64(q, 2, except_id);
    if (sqlite3_step(q) == SQLITE_ROW) id = sqlite3_column_int64(q, 0);
    sqlite3_finalize(q);
    return id;
}

/* BD_ERR_EXISTS when another drive or place already has the name. */
static bd_status name_free(bd_catalog *cat, const char *label, int64_t except_id)
{
    int64_t other = bd_media_named(cat, label, except_id);
    if (!other) return BD_OK;
    sqlite3_stmt *q;
    char name[256] = "";
    int lost = 0, place = 0;
    if (sqlite3_prepare_v2(cat->db, "SELECT label, retired_ms IS NOT NULL, secondary FROM media WHERE id=?", -1, &q, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(q, 1, other);
        if (sqlite3_step(q) == SQLITE_ROW) {
            snprintf(name, sizeof(name), "%s", (const char *)sqlite3_column_text(q, 0));
            lost = sqlite3_column_int(q, 1);
            place = sqlite3_column_int(q, 2);
        }
        sqlite3_finalize(q);
    }
    return bd_fail(cat, BD_ERR_EXISTS, "%s is already called \"%s\"%s: give this one another name, or rename that one first",
                   place ? "an extra place" : "another drive", name, lost ? " (marked lost or broken)" : "");
}

bd_status bd_media_rename(bd_catalog *cat, int64_t media_id, const char *label)
{
    if (!label || !*label) return bd_fail(cat, BD_ERR_INVALID, "give the drive a label");
    bd_status s = name_free(cat, label, media_id);
    if (s != BD_OK) return s;
    return set_text(cat, "UPDATE media SET label=? WHERE id=?", media_id, label);
}

bd_status bd_media_init(bd_catalog *cat, const char *root, const char *label, unsigned flags, int64_t *out_media_id)
{
    int encrypted = (flags & BD_MEDIA_ENCRYPTED) != 0, secondary = (flags & BD_MEDIA_SECONDARY) != 0;
    int hide_names = (flags & BD_MEDIA_HIDE_NAMES) != 0;
    if (hide_names && !encrypted) return bd_fail(cat, BD_ERR_INVALID, "names can only be hidden on an encrypted drive");
    if (encrypted && !cat->have_key)
        return bd_fail(cat, BD_ERR_PASSPHRASE, bd_catalog_has_passphrase(cat) ? "enter the passphrase to set up an encrypted drive"
                                                                               : "set a passphrase to set up an encrypted drive");
    bd_stat_t st;
    if (bd_stat(root, &st) != 0 || !st.is_dir) return bd_fail(cat, BD_ERR_NOT_FOUND, "%s is not a drive or folder", root);
    if (!label || !*label) return bd_fail(cat, BD_ERR_INVALID, "give the drive a label");
    bd_status taken = name_free(cat, label, 0);
    if (taken != BD_OK) return taken;

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
    mf.secondary = secondary;
    mf.hide_names = hide_names;

    int64_t id = bd_media_insert(cat, mf.media_uuid, "drive", mf.label, encrypted);
    if (!id) { free(path); return bd_fail_db(cat, "add drive"); }
    if (secondary) media_set_secondary(cat, id);
    if (hide_names) media_set_hide_names(cat, id);
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
    /* Every drive gets the latest catalog as soon as it is added. A copy
     * that fails here is put there by the next backup. */
    if (s == BD_OK) bd_catalog_copy_to_media(cat, id);
    return s;
}

int bd_media_lost(bd_catalog *cat, int64_t media_id)
{
    sqlite3_stmt *q;
    int lost = 0;
    if (sqlite3_prepare_v2(cat->db, "SELECT retired_ms IS NOT NULL FROM media WHERE id=?", -1, &q, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int64(q, 1, media_id);
    if (sqlite3_step(q) == SQLITE_ROW) lost = sqlite3_column_int(q, 0);
    sqlite3_finalize(q);
    return lost;
}

bd_status bd_media_mark_lost(bd_catalog *cat, int64_t media_id, const char *why)
{
    if (!why || (strcmp(why, "lost") != 0 && strcmp(why, "broken") != 0))
        return bd_fail(cat, BD_ERR_INVALID, "say whether the drive is lost or broken");
    sqlite3_stmt *u;
    if (sqlite3_prepare_v2(cat->db, "UPDATE media SET retired_ms=COALESCE(retired_ms, ?2), lost_why=?3 WHERE id=?1", -1, &u, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "mark the drive");
    sqlite3_bind_int64(u, 1, media_id);
    sqlite3_bind_int64(u, 2, bd_now_ms());
    sqlite3_bind_text(u, 3, why, -1, SQLITE_STATIC);
    int rc = sqlite3_step(u);
    sqlite3_finalize(u);
    if (rc != SQLITE_DONE) return bd_fail_db(cat, "mark the drive");
    if (sqlite3_changes(cat->db) == 0) return bd_fail(cat, BD_ERR_NOT_FOUND, "no such drive");
    return BD_OK;
}

bd_status bd_media_found(bd_catalog *cat, int64_t media_id)
{
    sqlite3_stmt *u;
    if (sqlite3_prepare_v2(cat->db, "UPDATE media SET retired_ms=NULL, lost_why=NULL, replaced_by=NULL WHERE id=?", -1, &u, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "mark the drive");
    sqlite3_bind_int64(u, 1, media_id);
    int rc = sqlite3_step(u);
    sqlite3_finalize(u);
    if (rc != SQLITE_DONE) return bd_fail_db(cat, "mark the drive");
    if (sqlite3_changes(cat->db) == 0) return bd_fail(cat, BD_ERR_NOT_FOUND, "no such drive");
    return BD_OK;
}

/* The older versions and deleted files with a good copy on the drive, and
 * what is left of them elsewhere (?1 the drive). */
#define IMPACT_CTE                                                                                                     \
    "WITH k AS (SELECT DISTINCT v.id AS vid, n.deleted AS del, v.id IS NOT n.current_version_id AS old,"             \
    "  (SELECT " BD_SQL_COPIES("c") " FROM copies c WHERE c.version_id=v.id AND c.state='ok' AND c.media_id<>?1"       \
    "   AND " BD_SQL_COUNTS("c") ") AS cp"                                                                              \
    "  FROM copies h JOIN versions v ON v.id=h.version_id JOIN nodes n ON n.id=v.node_id"                              \
    "  WHERE h.media_id=?1 AND h.state='ok' AND n.is_dir=0 AND (v.id IS NOT n.current_version_id OR n.deleted=1))"

bd_status bd_media_lost_impact(bd_catalog *cat, int64_t media_id, bd_lost_impact *out, bd_impact_drive_fn fn, void *ctx)
{
    memset(out, 0, sizeof(*out));
    bd_target t;
    bd_target_get(cat, &t);
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(cat->db,
                           IMPACT_CTE " SELECT COALESCE(SUM(old),0), COALESCE(SUM(del AND NOT old),0),"
                                      " COALESCE(SUM(old AND cp<?2),0), COALESCE(SUM(del AND NOT old AND cp<?2),0),"
                                      " COALESCE(SUM(old AND cp=0),0), COALESCE(SUM(del AND NOT old AND cp=0),0) FROM k",
                           -1, &q, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "count what the drive holds");
    sqlite3_bind_int64(q, 1, media_id);
    sqlite3_bind_int(q, 2, t.copies);
    if (sqlite3_step(q) == SQLITE_ROW) {
        out->old_versions = sqlite3_column_int64(q, 0);
        out->deleted = sqlite3_column_int64(q, 1);
        out->old_versions_short = sqlite3_column_int64(q, 2);
        out->deleted_short = sqlite3_column_int64(q, 3);
        out->old_versions_none = sqlite3_column_int64(q, 4);
        out->deleted_none = sqlite3_column_int64(q, 5);
    }
    sqlite3_finalize(q);
    out->copies_wanted = t.copies;
    if (!fn || !(out->old_versions + out->deleted)) return BD_OK;
    if (sqlite3_prepare_v2(cat->db,
                           IMPACT_CTE " SELECT m.id, m.label, COUNT(DISTINCT c.version_id) FROM copies c JOIN media m ON m.id=c.media_id"
                                      " WHERE c.state='ok' AND c.media_id<>?1 AND " BD_SQL_COUNTS("c")
                                      " AND c.version_id IN (SELECT vid FROM k) GROUP BY m.id ORDER BY 3 DESC, m.label",
                           -1, &q, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "find the other drives");
    sqlite3_bind_int64(q, 1, media_id);
    while (sqlite3_step(q) == SQLITE_ROW)
        if (fn(ctx, sqlite3_column_int64(q, 0), (const char *)sqlite3_column_text(q, 1), sqlite3_column_int64(q, 2))) break;
    sqlite3_finalize(q);
    return BD_OK;
}

/* SQL: a serial number known to be shared by more than one disk: recorded
 * for drives of another model or size. Two partitions of one disk share
 * model, serial and size. */
#define SERIAL_SHARED(serial, model, bytes)                                                                       \
    "(EXISTS(SELECT 1 FROM media_hardware sx WHERE upper(trim(sx.serial))=upper(trim(" serial "))"                  \
    "  AND (lower(trim(COALESCE(sx.model,'')))<>lower(trim(COALESCE(" model ",'')))"                                \
    "   OR (COALESCE(sx.disk_bytes,0)>0 AND COALESCE(" bytes ",0)>0 AND sx.disk_bytes<>" bytes ")))"                 \
    " OR EXISTS(SELECT 1 FROM media_hardware sx, media_hardware sy WHERE upper(trim(sx.serial))=upper(trim(" serial "))" \
    "  AND upper(trim(sy.serial))=upper(trim(" serial ")) AND sx.media_id<>sy.media_id"                             \
    "  AND (lower(trim(COALESCE(sx.model,'')))<>lower(trim(COALESCE(sy.model,'')))"                                 \
    "   OR (COALESCE(sx.disk_bytes,0)>0 AND COALESCE(sy.disk_bytes,0)>0 AND sx.disk_bytes<>sy.disk_bytes))))"
/* SQL: a serial number that tells one disk from another (as BD_SQL_HW_KEY). */
#define SERIAL_TELLS(serial) "(length(trim(" serial "))>=4 AND trim(" serial ",'0 ')<>'')"

/* 1 if the drive's last place, another folder than root, still holds its
 * ID file: it is there, only not plugged in as far as this session knows
 * (two folders or partitions of one disk read as the same hardware). */
static int still_there(bd_catalog *cat, int64_t id, const char *root)
{
    sqlite3_stmt *q;
    char uuid[64] = "", *last = NULL;
    if (sqlite3_prepare_v2(cat->db, "SELECT uuid, last_root FROM media WHERE id=?", -1, &q, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int64(q, 1, id);
    if (sqlite3_step(q) == SQLITE_ROW && sqlite3_column_text(q, 1)) {
        snprintf(uuid, sizeof(uuid), "%s", (const char *)sqlite3_column_text(q, 0));
        last = bd_strdup((const char *)sqlite3_column_text(q, 1));
    }
    sqlite3_finalize(q);
    int yes = last && *last && !same_root(last, root) && holds_media_id(cat, last, uuid);
    free(last);
    return yes;
}

int bd_media_maybe_erased(bd_catalog *cat, const char *root, const char *label, int64_t *ids, int max)
{
    if (max <= 0) return 0;
    int n = 0;
    int64_t hw_id = 0;
    if (bd_media_match_disk(cat, root, &hw_id) == BD_OK && hw_id && !still_there(cat, hw_id, root)) ids[n++] = hw_id;
    bd_drive_hw hw;
    int have_hw = (bd_test_hw_read ? bd_test_hw_read(root, &hw) : bd_drive_hw_read(root, &hw)) == 0;
    int64_t total = 0, freeb = 0;
    if (bd_disk_space(root, &total, &freeb) != 0) total = 0;
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT m.id FROM media m LEFT JOIN media_hardware h ON h.media_id=m.id"
                           " WHERE m.kind='drive' AND m.retired_ms IS NULL AND m.secondary=0 AND m.id<>?6"
                           " AND NOT EXISTS(SELECT 1 FROM temp.connected k WHERE k.media_id=m.id)"
                           " AND COALESCE(m.last_seen_ms,0)<?3"
                           " AND ((?1<>'' AND lower(trim(m.label))=lower(trim(?1)))"
                           "  OR (?2>0 AND m.total_bytes>0 AND abs(m.total_bytes-?2)*50<=?2)"
                           "  OR (?7>0 AND h.disk_bytes>0 AND h.disk_bytes=?7))"
                           /* A disk that tells itself apart is not a drive that was another disk. */
                           " AND NOT (?4<>'' AND " SERIAL_TELLS("?4") " AND h.serial IS NOT NULL AND " SERIAL_TELLS("h.serial")
                           "  AND upper(trim(h.serial))<>upper(trim(?4)) AND NOT " SERIAL_SHARED("?4", "?8", "?7") ")"
                           " ORDER BY (?1<>'' AND lower(trim(m.label))=lower(trim(?1))) DESC, COALESCE(m.last_seen_ms,0) DESC",
                           -1, &q, NULL) != SQLITE_OK) {
        bd_fail_db(cat, "compare the disk");
        return n;
    }
    sqlite3_bind_text(q, 1, label ? label : "", -1, SQLITE_STATIC);
    sqlite3_bind_int64(q, 2, total);
    sqlite3_bind_int64(q, 3, bd_now_ms() - (int64_t)BD_AWAY_DAYS * 24 * 3600 * 1000);
    sqlite3_bind_text(q, 4, have_hw ? hw.serial : "", -1, SQLITE_STATIC);
    sqlite3_bind_int64(q, 6, hw_id);
    sqlite3_bind_int64(q, 7, have_hw && hw.disk_bytes > 0 ? hw.disk_bytes : 0);
    sqlite3_bind_text(q, 8, have_hw ? hw.model : "", -1, SQLITE_STATIC);
    int64_t *maybe = NULL;
    int nm = 0, cap = 0;
    while (sqlite3_step(q) == SQLITE_ROW) {
        if (nm == cap) {
            int64_t *more = realloc(maybe, (size_t)(cap = cap ? cap * 2 : 16) * sizeof(*maybe));
            if (!more) break;
            maybe = more;
        }
        maybe[nm++] = sqlite3_column_int64(q, 0);
    }
    sqlite3_finalize(q);
    /* Read after the query: still_there runs queries of its own. */
    for (int i = 0; i < nm && n < max; i++)
        if (!still_there(cat, maybe[i], root)) ids[n++] = maybe[i];
    free(maybe);
    return n;
}

bd_status bd_media_match_disk(bd_catalog *cat, const char *root, int64_t *media_id)
{
    *media_id = 0;
    char *path;
    if (bd_media_file_path(cat, root, &path) != BD_OK) return BD_ERR_NOMEM;
    bd_media_file mf;
    int known = bd_media_file_read(path, &mf) == 0;
    free(path);
    if (known) return bd_fail(cat, BD_ERR_EXISTS, "%s is already set up for this catalog", root);
    bd_drive_hw hw;
    int rc = bd_test_hw_read ? bd_test_hw_read(root, &hw) : bd_drive_hw_read(root, &hw);
    if (rc != 0) return bd_fail(cat, BD_ERR_NOT_FOUND, "the disk at %s says nothing about itself", root);
    sqlite3_stmt *q;
    /* Not when the serial number is known to be shared: some USB adapters
     * give every disk in them the same one, so a new disk of the same size
     * would match. */
    if (sqlite3_prepare_v2(cat->db,
                           "WITH h(model, serial, disk_bytes) AS (SELECT ?1, ?2, ?3)"
                           " SELECT m.id FROM media m, h WHERE m.kind='drive' AND m.retired_ms IS NULL"
                           " AND NOT EXISTS(SELECT 1 FROM temp.connected k WHERE k.media_id=m.id)"
                           " AND " BD_SQL_HW_KEY("h") "=" BD_SQL_DISK("m.id") " AND NOT " SERIAL_SHARED("h.serial", "h.model", "h.disk_bytes") " ORDER BY m.id LIMIT 1",
                           -1, &q, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "compare the disk");
    sqlite3_bind_text(q, 1, hw.model, -1, SQLITE_STATIC);
    sqlite3_bind_text(q, 2, hw.serial, -1, SQLITE_STATIC);
    sqlite3_bind_int64(q, 3, hw.disk_bytes > 0 ? hw.disk_bytes : 0);
    if (sqlite3_step(q) == SQLITE_ROW) *media_id = sqlite3_column_int64(q, 0);
    sqlite3_finalize(q);
    return *media_id ? BD_OK : bd_fail(cat, BD_ERR_NOT_FOUND, "the disk at %s is not one of your drives", root);
}

int64_t bd_biggest_drive(bd_catalog *cat)
{
    sqlite3_stmt *q;
    int64_t big = -1;
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT COUNT(*), MIN(COALESCE(total_bytes,0)), MAX(total_bytes) FROM media"
                           " WHERE kind='drive' AND retired_ms IS NULL AND secondary=0",
                           -1, &q, NULL) != SQLITE_OK)
        return -1;
    if (sqlite3_step(q) == SQLITE_ROW && sqlite3_column_int64(q, 0) > 0 && sqlite3_column_int64(q, 1) > 0)
        big = sqlite3_column_int64(q, 2);
    sqlite3_finalize(q);
    return big;
}

bd_status bd_media_erased(bd_catalog *cat, int64_t media_id, const char *root)
{
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(cat->db, "SELECT uuid, label, encrypted, secondary, COALESCE(hide_names,0) FROM media WHERE id=? AND kind='drive'",
                           -1, &q, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "read the drive");
    sqlite3_bind_int64(q, 1, media_id);
    bd_media_file mf;
    memset(&mf, 0, sizeof(mf));
    int found = sqlite3_step(q) == SQLITE_ROW;
    if (found) {
        snprintf(mf.media_uuid, sizeof(mf.media_uuid), "%s", (const char *)sqlite3_column_text(q, 0));
        snprintf(mf.label, sizeof(mf.label), "%s", (const char *)sqlite3_column_text(q, 1));
        mf.encrypted = sqlite3_column_int(q, 2) != 0;
        mf.secondary = sqlite3_column_int(q, 3) != 0;
        mf.hide_names = sqlite3_column_int(q, 4) != 0;
    }
    sqlite3_finalize(q);
    if (!found) return bd_fail(cat, BD_ERR_NOT_FOUND, "no such drive");
    snprintf(mf.catalog_uuid, sizeof(mf.catalog_uuid), "%s", cat->uuid);
    bd_stat_t st;
    if (bd_stat(root, &st) != 0 || !st.is_dir) return bd_fail(cat, BD_ERR_NOT_FOUND, "%s is not a drive or folder", root);
    char *path;
    if (bd_media_file_path(cat, root, &path) != BD_OK) return BD_ERR_NOMEM;
    bd_media_file old;
    if (bd_media_file_read(path, &old) == 0) {
        free(path);
        return bd_fail(cat, BD_ERR_EXISTS, "%s is already set up for this catalog as \"%s\"", root, old.label);
    }
    char *dir = bd_media_catalog_dir(cat, root);
    int made = dir && bd_mkdirs(dir) == 0;
    free(dir);
    /* Never replaces a file: an ID file that is there already stops it. */
    if (!made || bd_media_file_write(path, &mf) != 0) {
        free(path);
        return bd_fail(cat, BD_ERR_IO, "cannot write the drive ID file on %s", root);
    }
    free(path);
    sqlite3_stmt *u;
    if (sqlite3_prepare_v2(cat->db, "UPDATE copies SET state='missing', last_quick_check_ms=?2 WHERE media_id=?1 AND state<>'missing'",
                           -1, &u, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "mark the copies missing");
    sqlite3_bind_int64(u, 1, media_id);
    sqlite3_bind_int64(u, 2, bd_now_ms());
    int rc = sqlite3_step(u);
    sqlite3_finalize(u);
    if (rc != SQLITE_DONE) return bd_fail_db(cat, "mark the copies missing");
    bd_status s = bd_media_record_connected(cat, media_id, root);
    if (s == BD_OK) read_hardware(cat, media_id, root, NULL, NULL);
    /* Every copy on it is marked missing: nothing recorded on it counts
     * that a backup has not put there again. */
    if (s == BD_OK) clear_suspect(cat, media_id);
    return s;
}

/* The same folder, reached by the same full path. */
static int same_root(const char *a, const char *b)
{
    char *fa = bd_full_path(a), *fb = bd_full_path(b);
    const char *x = fa ? fa : a, *y = fb ? fb : b;
    size_t nx = strlen(x), ny = strlen(y);
    while (nx > 1 && (x[nx - 1] == '/' || x[nx - 1] == '\\')) nx--;
    while (ny > 1 && (y[ny - 1] == '/' || y[ny - 1] == '\\')) ny--;
    int same = nx == ny;
    for (size_t i = 0; same && i < nx; i++) {
        char p = x[i], q = y[i];
        if (p == '\\') p = '/';
        if (q == '\\') q = '/';
#ifdef _WIN32
        if (p >= 'A' && p <= 'Z') p = (char)(p + 32);
        if (q >= 'A' && q <= 'Z') q = (char)(q + 32);
#endif
        same = p == q;
    }
    free(fa);
    free(fb);
    return same;
}

/* 1 if the ID file at root names the drive uuid. */
static int holds_media_id(bd_catalog *cat, const char *root, const char *uuid)
{
    char *path = NULL;
    bd_media_file mf;
    int yes = bd_media_file_path(cat, root, &path) == BD_OK && bd_media_file_read(path, &mf) == 0 &&
              strcmp(mf.catalog_uuid, cat->uuid) == 0 && strcmp(mf.media_uuid, uuid) == 0;
    free(path);
    return yes;
}

/* This user's home folder, as recover.c's home_folder reads it. */
static char *home_now(void)
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

char *bd_media_find_elsewhere(bd_catalog *cat, int64_t media_id, bd_log_fn log, void *log_ctx)
{
    sqlite3_stmt *q;
    char uuid[64] = "", label[256] = "", *last = NULL;
    if (sqlite3_prepare_v2(cat->db, "SELECT uuid, label, last_root FROM media WHERE id=? AND kind='drive' AND retired_ms IS NULL", -1, &q,
                           NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_int64(q, 1, media_id);
    if (sqlite3_step(q) == SQLITE_ROW && sqlite3_column_text(q, 2)) {
        snprintf(uuid, sizeof(uuid), "%s", (const char *)sqlite3_column_text(q, 0));
        snprintf(label, sizeof(label), "%s", (const char *)sqlite3_column_text(q, 1));
        last = bd_strdup((const char *)sqlite3_column_text(q, 2));
    }
    sqlite3_finalize(q);
    if (!last || !*last) {
        free(last);
        return NULL;
    }
    char *home = home_now(), *found = NULL;
#ifdef _WIN32
    /* Network drive letters too (bd_drive_letters leaves them out). */
    DWORD all = GetLogicalDrives();
    unsigned letters = 0;
    for (int i = 2; i < 26; i++) {
        wchar_t r[4] = {(wchar_t)(L'A' + i), L':', L'\\', 0};
        UINT t = (all & (1u << i)) ? GetDriveTypeW(r) : DRIVE_NO_ROOT_DIR;
        if (t == DRIVE_FIXED || t == DRIVE_REMOVABLE || t == DRIVE_REMOTE || t == DRIVE_RAMDISK) letters |= 1u << i;
    }
    char **cand = bd_root_candidates(last, home, letters, 1);
#else
    char **cand = bd_root_candidates(last, home, 0, 0);
#endif
    for (size_t i = 0; cand && cand[i]; i++) {
        if (!found && !same_root(cand[i], last) && holds_media_id(cat, cand[i], uuid)) found = bd_strdup(cand[i]);
        free(cand[i]);
    }
    free(cand);
    free(home);
    if (found)
        bd_logf(log, log_ctx, "\"%s\" is not at %s, where it was last, but at %s", label, last, found);
    free(last);
    return found;
}

bd_status bd_media_connect(bd_catalog *cat, const char *root, int64_t *out_media_id, bd_log_fn log, void *log_ctx)
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
        if (mf.secondary) media_set_secondary(cat, id);
        if (mf.hide_names) media_set_hide_names(cat, id);
        bd_logf(log, log_ctx, "re-registered drive \"%s\"", mf.label);
    }
    /* Plugged in somewhere else too, with the same ID file there: two disks
     * at once, one most likely a clone of the other. */
    char *was = bd_connected_root(cat, id);
    if (was && !same_root(was, root) && holds_media_id(cat, was, mf.media_uuid)) {
        bd_logf(log, log_ctx, "\"%s\" is plugged in at %s and at %s at once: one of the two disks is most likely a copy of the "
                              "other. Its copies do not count until a check of every copy on it, with the other one unplugged",
                mf.label, was, root);
        note_suspect(cat, id, 1, root, was);
    }
    free(was);
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
    return BD_OK;
}

void bd_media_disconnect(bd_catalog *cat, int64_t media_id)
{
    sqlite3_stmt *d;
    if (sqlite3_prepare_v2(cat->db, "DELETE FROM temp.connected WHERE media_id=?", -1, &d, NULL) != SQLITE_OK) return;
    sqlite3_bind_int64(d, 1, media_id);
    sqlite3_step(d);
    sqlite3_finalize(d);
}

int bd_media_present(bd_catalog *cat, int64_t media_id)
{
    char *root = bd_connected_root(cat, media_id);
    char *path = NULL;
    int same = 0;
    bd_media_file mf;
    if (root && bd_media_file_path(cat, root, &path) == BD_OK && bd_media_file_read(path, &mf) == 0 &&
        strcmp(mf.catalog_uuid, cat->uuid) == 0) {
        sqlite3_stmt *q;
        if (sqlite3_prepare_v2(cat->db, "SELECT 1 FROM media WHERE id=? AND uuid=?", -1, &q, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(q, 1, media_id);
            sqlite3_bind_text(q, 2, mf.media_uuid, -1, SQLITE_STATIC);
            same = sqlite3_step(q) == SQLITE_ROW;
            sqlite3_finalize(q);
        }
    }
    free(path);
    free(root);
    return same;
}

int bd_copy_marks_add(bd_copy_marks *m, int64_t copy_id, int64_t media_id, int bad, int read_back)
{
    if (m->n == m->cap) {
        size_t cap = m->cap ? m->cap * 2 : 64;
        bd_copy_mark *v = realloc(m->v, cap * sizeof(*v));
        if (!v) return -1;
        m->v = v;
        m->cap = cap;
    }
    bd_copy_mark *k = &m->v[m->n++];
    k->copy_id = copy_id;
    k->media_id = media_id;
    k->bad = bad;
    k->read_back = read_back;
    return 0;
}

int64_t bd_copy_marks_apply(bd_catalog *cat, bd_copy_marks *m)
{
    int64_t dropped = 0, now = bd_now_ms();
    int64_t seen[16];
    int there[16], nseen = 0;
    sqlite3_stmt *u = NULL;
    if (m->n && sqlite3_prepare_v2(cat->db,
                                   "UPDATE copies SET state=CASE WHEN ?1 THEN 'bad' ELSE 'missing' END, last_quick_check_ms=?2,"
                                   " last_full_check_ms=CASE WHEN ?3 THEN ?2 ELSE last_full_check_ms END WHERE id=?4",
                                   -1, &u, NULL) != SQLITE_OK)
        dropped = -1;
    for (size_t i = 0; dropped >= 0 && i < m->n; i++) {
        const bd_copy_mark *k = &m->v[i];
        /* Each drive is looked at once, now that the job is over. */
        int j = 0;
        while (j < nseen && seen[j] != k->media_id) j++;
        int ok;
        if (j < nseen) ok = there[j];
        else {
            ok = bd_media_present(cat, k->media_id);
            if (nseen < 16) { seen[nseen] = k->media_id; there[nseen++] = ok; }
        }
        if (!ok) { dropped++; continue; }
        sqlite3_reset(u);
        sqlite3_bind_int(u, 1, k->bad);
        sqlite3_bind_int64(u, 2, now);
        sqlite3_bind_int(u, 3, k->read_back);
        sqlite3_bind_int64(u, 4, k->copy_id);
        if (sqlite3_step(u) != SQLITE_DONE) dropped = -1;
    }
    sqlite3_finalize(u);
    bd_copy_marks_free(m);
    return dropped;
}

void bd_copy_marks_free(bd_copy_marks *m)
{
    free(m->v);
    memset(m, 0, sizeof(*m));
}

/* What a check reads back: every copy, the copies due (not read back since
 * due_before, or never, and every copy marked missing, in case it is
 * back), or only the copies never read back. */
enum { CHECK_EVERY, CHECK_DUE, CHECK_UNREAD };

static bd_status check_copies(bd_catalog *cat, int64_t media_id, int mode, int64_t due_before,
                              bd_check_stats *stats, bd_log_fn log, void *log_ctx);

typedef struct { char *path; } newest_rev;

static int newest_rev_cb(void *ctx, const bd_catalog_copy_info *c)
{
    newest_rev *n = ctx;
    n->path = bd_strdup(c->path);
    return 1;
}

/* Half-written files: BRODALF's own, from a copy that stopped. */
typedef struct { const char *dir; int64_t n, bytes, before_ns; int remove; } leftover_walk;

static int leftover_cb(void *ctx, const char *rel, const bd_stat_t *st)
{
    leftover_walk *w = ctx;
    size_t n = strlen(rel), m = sizeof(BD_TMP_MARKER) - 1;
    if (!st->is_file || n <= m || strcmp(rel + n - m, BD_TMP_MARKER) != 0 || st->mtime_ns > w->before_ns) return 0;
    if (w->remove) {
        char *p = bd_path_join(w->dir, rel);
        if (p && bd_remove(p) == 0) { w->n++; w->bytes += st->size; }
        free(p);
        return 0;
    }
    w->n++;
    w->bytes += st->size;
    return 0;
}

static void leftovers(bd_catalog *cat, int64_t media_id, int remove, int64_t *n, int64_t *bytes)
{
    *n = *bytes = 0;
    char *root = bd_connected_root(cat, media_id);
    char *dir = root ? bd_media_catalog_dir(cat, root) : NULL;
    if (dir) {
        leftover_walk w = {dir, 0, 0, (bd_now_ms() - 3600 * 1000) * 1000000, remove};
        bd_walk(dir, leftover_cb, NULL, &w);
        *n = w.n;
        *bytes = w.bytes;
    }
    free(dir);
    free(root);
}

bd_status bd_media_remove_leftovers(bd_catalog *cat, int64_t media_id, int64_t *removed)
{
    int64_t n = 0, bytes = 0;
    if (!bd_media_present(cat, media_id)) return bd_fail(cat, BD_ERR_NOT_FOUND, "that drive is not connected");
    leftovers(cat, media_id, 1, &n, &bytes);
    if (removed) *removed = n;
    return BD_OK;
}

/* How many characters Windows counts in a UTF-8 path (UTF-16 units). */
static int64_t utf16_length(const char *s)
{
    int64_t n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if ((*p & 0xC0) != 0x80) n++;
        if (*p >= 0xF0) n++; /* outside the Basic Multilingual Plane: two units */
    }
    return n;
}

int64_t bd_media_long_paths(bd_catalog *cat, int64_t media_id, const char *root)
{
    char *connected = root ? NULL : bd_connected_root(cat, media_id);
    if (!root && !connected) {
        sqlite3_stmt *r;
        if (sqlite3_prepare_v2(cat->db, "SELECT last_root FROM media WHERE id=?", -1, &r, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(r, 1, media_id);
            if (sqlite3_step(r) == SQLITE_ROW && sqlite3_column_text(r, 0))
                connected = bd_strdup((const char *)sqlite3_column_text(r, 0));
            sqlite3_finalize(r);
        }
    }
    const char *at = root ? root : connected;
    char *dir = at && *at ? bd_media_catalog_dir(cat, at) : NULL;
    free(connected);
    if (!dir) return 0;
    int64_t prefix = utf16_length(dir) + 1; /* BRODALF\<id>\ under the root, and the separator after it */
    free(dir);
    sqlite3_stmt *q;
    /* A UTF-8 path has at least as many bytes as UTF-16 units: the bytes
     * pick out the copies that may be too long, then each is counted. */
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT path_on_media FROM copies WHERE media_id=?1 AND state='ok' AND length(CAST(path_on_media AS BLOB))>?2",
                           -1, &q, NULL) != SQLITE_OK) {
        bd_fail_db(cat, "count the long paths");
        return -1;
    }
    sqlite3_bind_int64(q, 1, media_id);
    sqlite3_bind_int64(q, 2, BD_LONG_PATH - prefix);
    int64_t n = 0;
    while (sqlite3_step(q) == SQLITE_ROW)
        if (prefix + utf16_length((const char *)sqlite3_column_text(q, 0)) > BD_LONG_PATH) n++;
    sqlite3_finalize(q);
    return n;
}

void bd_log_long_paths(bd_log_fn log, void *log_ctx, int64_t n, const char *label)
{
    if (n <= 0) return;
    char on[300];
    if (label) snprintf(on, sizeof(on), "\"%s\"", label);
    else snprintf(on, sizeof(on), "the drive");
    if (n == 1)
        bd_logf(log, log_ctx, "1 copy on %s has a full path there of more than 259 characters: File Explorer and some "
                              "other programs may not open or copy it by hand, but BRODALF restores it as usual",
                on);
    else
        bd_logf(log, log_ctx, "%lld copies on %s have a full path there of more than 259 characters: File Explorer and some "
                              "other programs may not open or copy them by hand, but BRODALF restores them as usual",
                (long long)n, on);
}

bd_status bd_media_check(bd_catalog *cat, int64_t media_id, bd_check_stats *stats, bd_log_fn log, void *log_ctx)
{
    bd_check_stats local;
    if (!stats) stats = &local;
    bd_cancel_reset(cat);
    bd_status s = check_copies(cat, media_id, CHECK_EVERY, 0, stats, log, log_ctx);
    if (s != BD_OK) return s;
    /* Every copy on the disk plugged in now was read: what it holds is
     * known, so a drive whose ID turned up on another disk counts again,
     * unless that other disk is still plugged in with the same ID. */
    char *other = NULL;
    int why = bd_media_suspect(cat, media_id, NULL, &other);
    if (why) {
        char *now = bd_connected_root(cat, media_id);
        char uuid[64] = "";
        sqlite3_stmt *u;
        if (sqlite3_prepare_v2(cat->db, "SELECT uuid FROM media WHERE id=?", -1, &u, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(u, 1, media_id);
            if (sqlite3_step(u) == SQLITE_ROW) snprintf(uuid, sizeof(uuid), "%s", (const char *)sqlite3_column_text(u, 0));
            sqlite3_finalize(u);
        }
        int both = why == 1 && other && now && !same_root(other, now) && holds_media_id(cat, other, uuid);
        if (stats->skipped)
            bd_logf(log, log_ctx, "its copies count again once every copy on it is checked: enter the passphrase and check it again");
        else if (both)
            bd_logf(log, log_ctx, "the drive is still plugged in at %s too: unplug the one you do not keep as this drive, then check "
                                  "this one again", other);
        else {
            clear_suspect(cat, media_id);
            bd_logf(log, log_ctx, "every copy on the drive was checked, so what it holds is known: its copies count again");
        }
        free(now);
    }
    free(other);
    /* The newest copy of the catalog on the drive: the one Recover takes. */
    char *root = bd_connected_root(cat, media_id);
    char *dir = root ? bd_media_catalog_dir(cat, root) : NULL;
    newest_rev nr = {NULL};
    if (dir) bd_find_catalog_revisions(dir, newest_rev_cb, &nr);
    if (nr.path) {
        const char *name = nr.path + strlen(nr.path);
        while (name > nr.path && name[-1] != '/' && name[-1] != '\\') name--;
        stats->catalog_revision = 0;
        if (strncmp(name, "catalog-r", 9) == 0) stats->catalog_revision = strtoll(name + 9, NULL, 10);
        bd_status r = bd_catalog_copy_readable(cat, nr.path);
        if (r == BD_OK) {
            stats->catalog_copy = 1;
        } else if (r != BD_ERR_PASSPHRASE) {
            stats->catalog_copy = -1;
            bd_logf(log, log_ctx, "the newest copy of the catalog on the drive (revision %lld) is damaged: the next backup puts a new one there",
                    (long long)stats->catalog_revision);
            /* So that the next save is a new revision, under a new name. */
            bd_catalog_mark_changed(cat);
        }
    }
    free(nr.path);
    free(dir);
    free(root);
    leftovers(cat, media_id, 0, &stats->leftovers, &stats->leftover_bytes);
    if (stats->leftovers)
        bd_logf(log, log_ctx, "%lld half-written files from a copy that stopped are on the drive (%lld bytes); they can be removed",
                (long long)stats->leftovers, (long long)stats->leftover_bytes);
    stats->long_paths = bd_media_long_paths(cat, media_id, NULL);
    bd_log_long_paths(log, log_ctx, stats->long_paths, NULL);
    return BD_OK;
}

bd_status bd_media_verify(bd_catalog *cat, int64_t media_id, int max_age_days,
                          bd_check_stats *stats, bd_log_fn log, void *log_ctx)
{
    int64_t due = max_age_days > 0 ? bd_now_ms() - (int64_t)max_age_days * 24 * 3600 * 1000 : INT64_MAX;
    bd_cancel_reset(cat);
    return check_copies(cat, media_id, CHECK_DUE, due, stats, log, log_ctx);
}

bd_status bd_media_verify_unread(bd_catalog *cat, int64_t media_id, bd_check_stats *stats, bd_log_fn log, void *log_ctx)
{
    bd_cancel_reset(cat);
    return check_copies(cat, media_id, CHECK_UNREAD, 0, stats, log, log_ctx);
}

/* The copies a check reads (?1 drive, ?2 mode, ?3 due before). */
#define CHECK_WHERE                                                                                     \
    " c.media_id=?1 AND (?2=0 OR (?2=1 AND (c.state='missing' OR COALESCE(c.last_full_check_ms,0)<?3))" \
    " OR (?2=2 AND c.last_full_check_ms IS NULL))"

static bd_status check_copies(bd_catalog *cat, int64_t media_id, int mode, int64_t due_before,
                              bd_check_stats *stats, bd_log_fn log, void *log_ctx)
{
    bd_check_stats local;
    if (!stats) stats = &local;
    memset(stats, 0, sizeof(*stats));

    int encrypted = bd_media_encrypted(cat, media_id);
    if (encrypted && !cat->have_key)
        return bd_fail(cat, BD_ERR_PASSPHRASE, "enter the passphrase to check an encrypted drive");
    bd_store *store = bd_store_open(cat, media_id);
    if (!store) return BD_ERR_NOT_FOUND;
    int hide = bd_media_hides_names(cat, media_id, NULL);

    sqlite3_stmt *q = NULL, *u = NULL;
    bd_status s = BD_OK;
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT c.id, c.path_on_media, c.stored_size, c.stored_mtime_ns, v.hash, c.state, c.encrypted,"
                           " COALESCE(s.name || '/' || n.rel_path, c.path_on_media)"
                           " FROM copies c JOIN versions v ON v.id=c.version_id"
                           " LEFT JOIN nodes n ON n.id=v.node_id LEFT JOIN sources s ON s.id=n.source_id WHERE" CHECK_WHERE
                           " ORDER BY c.path_on_media",
                           -1, &q, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(cat->db,
                           "UPDATE copies SET state='ok', stored_mtime_ns=?, last_quick_check_ms=?, last_full_check_ms=? WHERE id=?",
                           -1, &u, NULL) != SQLITE_OK) {
        s = bd_fail_db(cat, "prepare drive check");
        goto done;
    }
    /* Totals for the progress bar. */
    sqlite3_stmt *t;
    int64_t total_files = 0, total_bytes = 0;
    if (sqlite3_prepare_v2(cat->db, "SELECT COUNT(*), COALESCE(SUM(c.stored_size),0) FROM copies c WHERE" CHECK_WHERE, -1, &t,
                           NULL) == SQLITE_OK) {
        sqlite3_bind_int64(t, 1, media_id);
        sqlite3_bind_int(t, 2, mode);
        sqlite3_bind_int64(t, 3, due_before);
        if (sqlite3_step(t) == SQLITE_ROW) {
            total_files = sqlite3_column_int64(t, 0);
            total_bytes = sqlite3_column_int64(t, 1);
        }
        sqlite3_finalize(t);
    }
    const char *what = mode == CHECK_EVERY ? "check" : "read-back";
    bd_progress_begin(cat, mode == CHECK_EVERY ? "check" : "verify", total_files, total_bytes);
    int64_t job = bd_job_start(cat, mode == CHECK_EVERY ? "check" : "read back", media_id);

    if (bd_exec(cat, "BEGIN") != 0) { s = bd_fail_db(cat, "begin drive check"); goto done; }
    sqlite3_bind_int64(q, 1, media_id);
    sqlite3_bind_int(q, 2, mode);
    sqlite3_bind_int64(q, 3, due_before);
    int64_t now = bd_now_ms();
    int cancelled = 0, gone = 0;
    /* Copies found missing or damaged are recorded at the end, and only if
     * the drive is still there then: a drive that drops out halfway leaves
     * every copy as it was. */
    bd_copy_marks marks = {NULL, 0, 0};
    while (sqlite3_step(q) == SQLITE_ROW) {
        if (bd_cancelled(cat)) { cancelled = 1; break; }
        int64_t copy_id = sqlite3_column_int64(q, 0);
        const char *rel = (const char *)sqlite3_column_text(q, 1);
        int64_t stored_size = sqlite3_column_int64(q, 2);
        const char *expected = (const char *)sqlite3_column_text(q, 4);
        /* What the user knows the file as: on a drive that hides names the
         * path there means nothing to them. */
        const char *shown = hide ? (const char *)sqlite3_column_text(q, 7) : rel;
        /* On a plain drive, copies of folders set to be encrypted are
         * sealed one by one. Without the passphrase they are left as they
         * were and counted as skipped. */
        int sealed = encrypted || sqlite3_column_int(q, 6);
        const uint8_t *key = sealed ? cat->key : NULL;
        if (sealed && !cat->have_key) {
            stats->copies++;
            stats->skipped++;
            if (stats->skipped == 1) bd_logf(log, log_ctx, "encrypted copies are skipped: enter the passphrase to check them too");
            continue;
        }
        stats->copies++;
        bd_report_file(cat, stats->copies, shown, stored_size);

        bd_remote_stat st;
        int found = store->ops->stat(store, rel, &st);
        int got = 0, hrc = 0, hashed = 0;
        char hash[BD_HASH_HEX_LEN + 1] = "";
        if (found == 0 && st.size == stored_size) {
            char *path = NULL;
            int is_temp = 0;
            hashed = 1;
            stats->rehashed++;
            got = bd_store_fetch(store, rel, &path, &is_temp);
            if (got == 0) hrc = bd_hash_copy(path, key, NULL, NULL, hash, NULL);
            bd_store_release(path, is_temp);
            if (hrc == -4) {
                /* Stopped halfway through reading it: leave it as it was. */
                stats->copies--;
                stats->rehashed--;
                cancelled = 1;
                break;
            }
        }
        int good = hashed && got == 0 && hrc == 0 && strcmp(hash, expected) == 0;
        if (!good && !bd_media_present(cat, media_id)) {
            /* Not there, or not readable, because the drive itself went
             * (or another one is there now): not this copy's fault. */
            stats->copies--;
            if (hashed) stats->rehashed--;
            gone = 1;
            break;
        }
        if (found < 0 || got < 0) {
            /* Could not look: leave the copy as it was. */
            bd_logf(log, log_ctx, "cannot check %s: %s", shown, store->err);
            stats->skipped++;
            continue;
        }
        if (good) {
            stats->ok++;
            sqlite3_reset(u);
            sqlite3_bind_int64(u, 1, st.mtime_ns);
            sqlite3_bind_int64(u, 2, now);
            sqlite3_bind_int64(u, 3, now);
            sqlite3_bind_int64(u, 4, copy_id);
            if (sqlite3_step(u) != SQLITE_DONE) { s = bd_fail_db(cat, "record drive check"); break; }
            continue;
        }
        int missing = found == 1 || got == 1;
        if (missing) { stats->missing++; bd_logf(log, log_ctx, "missing on drive: %s", shown); }
        else { stats->bad++; bd_logf(log, log_ctx, "damaged on drive: %s", shown); }
        if (bd_copy_marks_add(&marks, copy_id, media_id, !missing, hashed && !missing) != 0) {
            s = bd_fail(cat, BD_ERR_NOMEM, "out of memory");
            break;
        }
    }
    sqlite3_reset(q);
    if (s == BD_OK && !gone) {
        int64_t dropped = bd_copy_marks_apply(cat, &marks);
        if (dropped < 0) s = bd_fail_db(cat, "record drive check");
        else if (dropped > 0) gone = 1;
    }
    bd_copy_marks_free(&marks);
    if (s == BD_OK && gone) {
        stats->missing = stats->bad = 0;
        bd_logf(log, log_ctx, "the drive stopped answering or was unplugged: nothing on it was marked missing or damaged");
        s = bd_fail(cat, BD_ERR_DRIVE_GONE,
                    "the drive stopped answering or was unplugged during the %s, so nothing on it was marked missing or"
                    " damaged; plug it in again and run the %s again",
                    what, what);
    }
    /* Copies checked before a stop keep what was found. */
    if ((s == BD_OK || s == BD_ERR_DRIVE_GONE) && bd_exec(cat, "COMMIT") != 0) s = bd_fail_db(cat, "commit drive check");
    if (s != BD_OK && s != BD_ERR_DRIVE_GONE) bd_exec(cat, "ROLLBACK");
    if (s == BD_OK && cancelled) s = bd_fail_cancelled(cat);
    bd_report(cat, stats->copies, NULL, 1);
    bd_progress_end(cat);
    bd_job_finish(cat, job, s == BD_ERR_CANCELLED ? "stopped" : s != BD_OK ? "failed" : stats->bad || stats->missing ? "partial" : "done",
                  stats->copies, stats->bad + stats->missing, 0);
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

/* The catalog's copy on an encrypted drive is always encrypted. */
static bd_status put_catalog_copy(bd_catalog *cat, int64_t media_id, char *name_out, size_t name_cap);

/* Whether the copy rel on the drive was made by this database: 1 the same
 * save (its save id), or, saved by an older BRODALF, neither has one; 0
 * another copy of this database saved it; -1 it cannot be read to tell. */
static int copy_is_ours(bd_catalog *cat, int64_t media_id, const char *rel)
{
    char *root = bd_connected_root(cat, media_id);
    char *dir = root ? bd_media_catalog_dir(cat, root) : NULL;
    char *path = dir ? bd_path_join(dir, rel) : NULL;
    char mine[40], theirs[40];
    int have_mine = bd_catalog_save_id(cat, mine, sizeof(mine));
    bd_status s = path ? bd_catalog_copy_meta(cat, path, "save_id", theirs, sizeof(theirs)) : BD_ERR_NOMEM;
    int ours = s == BD_OK ? have_mine && strcmp(mine, theirs) == 0 : s == BD_ERR_NOT_FOUND ? !have_mine : -1;
    free(path);
    free(dir);
    free(root);
    return ours;
}

/* Copies this session put beside another copy of the same revision. */
static void note_copy_clash(bd_catalog *cat, int64_t media_id, long long rev, const char *name)
{
    sqlite3_stmt *q;
    bd_exec(cat, "CREATE TEMP TABLE IF NOT EXISTS copy_clash(media_id INTEGER PRIMARY KEY, revision INTEGER NOT NULL, name TEXT NOT NULL);");
    if (sqlite3_prepare_v2(cat->db, "INSERT OR REPLACE INTO temp.copy_clash(media_id, revision, name) VALUES(?,?,?)", -1, &q, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int64(q, 1, media_id);
    sqlite3_bind_int64(q, 2, rev);
    sqlite3_bind_text(q, 3, name, -1, SQLITE_TRANSIENT);
    sqlite3_step(q);
    sqlite3_finalize(q);
}

typedef struct {
    bd_drive_catalog *out;
    int64_t at_newest;
} compare_ctx;

/* Newest first: the first one and how many share its revision. */
static int compare_cb(void *ctx, const bd_catalog_copy_info *info)
{
    compare_ctx *c = ctx;
    if (!c->out->path) {
        c->out->path = bd_strdup(info->path);
        c->out->revision = info->revision;
        c->out->written_ms = info->saved_ms;
    } else if (info->revision != c->out->revision) {
        return 1;
    }
    c->at_newest++;
    return 0;
}

bd_status bd_media_catalog_compare(bd_catalog *cat, int64_t media_id, bd_drive_catalog *out)
{
    memset(out, 0, sizeof(*out));
    out->revision = -1;
    out->open_revision = bd_catalog_revision(cat);
    out->open_saved_ms = bd_catalog_saved_ms(cat);
    if (!out->open_saved_ms) {
        bd_stat_t st;
        if (bd_stat(cat->path, &st) == 0) out->open_saved_ms = st.mtime_ns / 1000000;
    }
    char *root = bd_connected_root(cat, media_id);
    if (!root) return bd_fail(cat, BD_ERR_NOT_FOUND, "that drive is not connected");
    char *dir = bd_media_catalog_dir(cat, root);
    compare_ctx c = {out, 0};
    if (dir) bd_find_catalog_revisions(dir, compare_cb, &c);
    free(dir);
    free(root);
    if (out->path) {
        out->newer = out->revision > out->open_revision;
        out->separate = c.at_newest > 1;
    }
    return BD_OK;
}

int bd_media_copy_clash(bd_catalog *cat, int64_t media_id, int64_t *revision)
{
    sqlite3_stmt *q;
    int found = 0;
    if (revision) *revision = 0;
    if (sqlite3_prepare_v2(cat->db, "SELECT revision FROM temp.copy_clash WHERE media_id=?", -1, &q, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int64(q, 1, media_id);
    if (sqlite3_step(q) == SQLITE_ROW) {
        found = 1;
        if (revision) *revision = sqlite3_column_int64(q, 0);
    }
    sqlite3_finalize(q);
    return found;
}

/* What became of the copies of the database put on the drives this
 * session; kept in media_catalog by bd_media_record_catalog_copies. */
#define COPY_RESULT_TABLE \
    "CREATE TEMP TABLE IF NOT EXISTS copy_result(media_id INTEGER PRIMARY KEY, revision INTEGER NOT NULL," \
    " at_ms INTEGER NOT NULL, ok INTEGER NOT NULL);"

static void note_copy_result(bd_catalog *cat, int64_t media_id, int64_t rev, int ok)
{
    sqlite3_stmt *q;
    bd_exec(cat, COPY_RESULT_TABLE);
    if (sqlite3_prepare_v2(cat->db, "INSERT OR REPLACE INTO temp.copy_result(media_id, revision, at_ms, ok) VALUES(?,?,?,?)", -1, &q,
                           NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int64(q, 1, media_id);
    sqlite3_bind_int64(q, 2, rev);
    sqlite3_bind_int64(q, 3, bd_now_ms());
    sqlite3_bind_int(q, 4, ok);
    sqlite3_step(q);
    sqlite3_finalize(q);
}

void bd_media_record_catalog_copies(bd_catalog *cat)
{
    if (!cat || !cat->db || !sqlite3_get_autocommit(cat->db)) return; /* not inside another transaction */
    sqlite3_stmt *q;
    bd_exec(cat, COPY_RESULT_TABLE);
    int any = 0;
    if (sqlite3_prepare_v2(cat->db, "SELECT 1 FROM temp.copy_result LIMIT 1", -1, &q, NULL) == SQLITE_OK) {
        any = sqlite3_step(q) == SQLITE_ROW;
        sqlite3_finalize(q);
    }
    if (!any) return;
    int was_saved = cat->changes == cat->saved_changes || (cat->copies_noted_at && cat->changes == cat->copies_noted_at);
    bd_exec(cat, "BEGIN;"
                 "INSERT OR IGNORE INTO media_catalog(media_id) SELECT media_id FROM temp.copy_result r"
                 " WHERE EXISTS(SELECT 1 FROM media m WHERE m.id=r.media_id);"
                 "UPDATE media_catalog SET"
                 " revision=(SELECT r.revision FROM temp.copy_result r WHERE r.media_id=media_catalog.media_id),"
                 " written_ms=(SELECT r.at_ms FROM temp.copy_result r WHERE r.media_id=media_catalog.media_id),"
                 " failed_ms=NULL, failed_revision=NULL"
                 " WHERE media_id IN (SELECT media_id FROM temp.copy_result WHERE ok<>0);"
                 "UPDATE media_catalog SET"
                 " failed_ms=(SELECT r.at_ms FROM temp.copy_result r WHERE r.media_id=media_catalog.media_id),"
                 " failed_revision=(SELECT r.revision FROM temp.copy_result r WHERE r.media_id=media_catalog.media_id)"
                 " WHERE media_id IN (SELECT media_id FROM temp.copy_result WHERE ok=0);"
                 "DELETE FROM temp.copy_result;"
                 "COMMIT;");
    if (sqlite3_get_autocommit(cat->db) == 0) bd_exec(cat, "ROLLBACK");
    cat->copies_noted_at = was_saved ? cat->changes : 0;
}

/* This database put its copy of revision rev on the drive: this session,
 * or recorded before (a record is always of an earlier revision than the
 * database holding it, so another copy of the database has none for its
 * later revisions). Nothing on the drive needs reading to tell. */
static int wrote_revision(bd_catalog *cat, int64_t media_id, int64_t rev)
{
    sqlite3_stmt *q;
    int yes = 0;
    bd_exec(cat, COPY_RESULT_TABLE);
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT 1 FROM temp.copy_result WHERE media_id=?1 AND revision=?2 AND ok<>0"
                           " UNION ALL SELECT 1 FROM media_catalog WHERE media_id=?1 AND revision=?2",
                           -1, &q, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(q, 1, media_id);
        sqlite3_bind_int64(q, 2, rev);
        yes = sqlite3_step(q) == SQLITE_ROW;
        sqlite3_finalize(q);
    }
    return yes;
}

/* The newest revision of the database known to be on the drive: put there
 * this session or recorded before, or, on a drive plugged in now, read
 * from the names of the copies there. 0 when none is known. */
static int64_t known_copy_revision(bd_catalog *cat, int64_t media_id)
{
    sqlite3_stmt *q;
    int64_t rev = 0;
    bd_exec(cat, COPY_RESULT_TABLE);
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT MAX(r) FROM (SELECT revision AS r FROM temp.copy_result WHERE media_id=?1 AND ok<>0"
                           " UNION ALL SELECT revision FROM media_catalog WHERE media_id=?1)",
                           -1, &q, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(q, 1, media_id);
        if (sqlite3_step(q) == SQLITE_ROW) rev = sqlite3_column_int64(q, 0);
        sqlite3_finalize(q);
    }
    char *root = bd_connected_root(cat, media_id);
    bd_drive_catalog dc;
    if (root && bd_media_catalog_compare(cat, media_id, &dc) == BD_OK) {
        if (dc.revision > rev) rev = dc.revision;
        free(dc.path);
    }
    free(root);
    return rev;
}

int bd_media_copy_has_key(bd_catalog *cat, int64_t media_id, int recovery)
{
    char v[32];
    int64_t since = bd_setting_get(cat, recovery ? "recovery_key_rev" : "passphrase_rev", v, sizeof(v)) ? atoll(v) : 0;
    int64_t rev = known_copy_revision(cat, media_id);
    if (rev <= 0) return -1;
    return rev >= since;
}

bd_status bd_catalog_copy_to_media(bd_catalog *cat, int64_t media_id)
{
    /* What the copies put on the drives were is no change to save as a
     * new revision of its own: it goes with the next save. */
    int only_noted = cat->copies_noted_at && cat->changes == cat->copies_noted_at && cat->saved_changes != cat->changes;
    bd_status s = only_noted ? BD_OK : bd_catalog_save(cat);
    char name[64] = "";
    if (s == BD_OK) s = put_catalog_copy(cat, media_id, name, sizeof(name));
    if (s == BD_ERR_FORMAT) {
        /* It did not read back right: once more, as the next revision. The
         * bad copy stays (nothing on a drive is overwritten); Recover tries
         * an older one when the newest will not open. */
        bd_catalog_mark_changed(cat);
        s = bd_catalog_save(cat);
        if (s == BD_OK) s = put_catalog_copy(cat, media_id, name, sizeof(name));
    }
    note_copy_result(cat, media_id, bd_catalog_revision(cat), s == BD_OK);
    /* Kept now, so it is there when the database is closed with nothing
     * else to save; it is no change of its own (see only_noted above). */
    bd_media_record_catalog_copies(cat);
    if (s == BD_OK) {
        /* Beside it, the plain list of where each file's newest copy is on
         * the drive (newest.c). It only helps: a list that cannot be
         * written leaves the database copy as good as it was. */
        char keep[sizeof(cat->err)];
        memcpy(keep, cat->err, sizeof(keep));
        if (bd_newest_list_to_media(cat, media_id, name[0] ? name : NULL) != BD_OK) memcpy(cat->err, keep, sizeof(keep));
    }
    return s;
}

static bd_status put_catalog_copy(bd_catalog *cat, int64_t media_id, char *name_out, size_t name_cap)
{
    bd_status s = BD_OK;
    int encrypt = bd_catalog_file_encrypted(cat) || bd_media_encrypted(cat, media_id);
    bd_store *store = bd_store_open(cat, media_id);
    if (!store) return BD_ERR_NOT_FOUND;
    /* A new name for every revision: older copies on the drive stay as
     * they are, never overwritten. */
    char rel[64], tmp[96];
    long long rev = (long long)bd_catalog_revision(cat);
    snprintf(rel, sizeof(rel), "catalog-r%08lld.brodalf", rev);
    bd_remote_stat st;
    int there = store->ops->stat(store, rel, &st);
    if (there == 0) {
        /* This revision is on the drive already: this database's own copy,
         * or one from another copy of this database that was used apart
         * from it and reached the same revision. That one stays; this one
         * goes beside it, under a name of its own. */
        int ours = wrote_revision(cat, media_id, rev) ? 1 : copy_is_ours(cat, media_id, rel);
        if (ours == 1) { bd_store_close(store); return BD_OK; }
        char id[40];
        if (!bd_catalog_save_id(cat, id, sizeof(id)) || strlen(id) < 8) bd_uuid_v4(id);
        snprintf(rel, sizeof(rel), "catalog-r%08lld-%.8s.brodalf", rev, id);
        if (ours == 0) note_copy_clash(cat, media_id, rev, rel);
        there = store->ops->stat(store, rel, &st);
        if (there == 0) { /* put there at an earlier clash */
            snprintf(name_out, name_cap, "%s", rel);
            bd_store_close(store);
            return BD_OK;
        }
    }
    snprintf(tmp, sizeof(tmp), "%s" BD_TMP_MARKER, rel);
    char *staged = there < 0 ? NULL : store->ops->staging_path(store, tmp);
    if (!staged) s = bd_fail(cat, BD_ERR_IO, "cannot copy the catalog: %s", store->err);
    else if ((s = bd_catalog_save_to(cat, staged, encrypt)) == BD_OK &&
             store->ops->upload(store, staged, tmp, &st) != 0)
        s = bd_fail(cat, BD_ERR_IO, "cannot copy the catalog: %s", store->err);
    else if (s == BD_OK && store->ops->move(store, tmp, rel, 0) != 0)
        s = bd_fail(cat, BD_ERR_IO, "cannot copy the catalog: %s", store->err);
    if (s != BD_OK) store->ops->remove(store, tmp);
    if (staged && s != BD_OK) bd_remove(staged);
    free(staged);
    bd_store_close(store);
    if (s == BD_OK) {
        /* Read it back as Recover would. */
        char *root = bd_connected_root(cat, media_id);
        char *dir = root ? bd_media_catalog_dir(cat, root) : NULL;
        char *path = dir ? bd_path_join(dir, rel) : NULL;
        if (path && bd_test_spoil_catalog_copy) bd_test_spoil_catalog_copy(path);
        bd_status r = path ? bd_catalog_copy_readable(cat, path) : BD_OK;
        if (r == BD_ERR_FORMAT || r == BD_ERR_NOT_FOUND)
            s = bd_fail(cat, BD_ERR_FORMAT, "the copy of the catalog on the drive did not read back right (revision %lld)",
                        (long long)bd_catalog_revision(cat));
        free(path);
        free(dir);
        free(root);
    }
    /* With s BD_OK the copy is on the drive, as rel, and reads back right. */
    if (s == BD_OK) snprintf(name_out, name_cap, "%s", rel);
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
    } else if (strcmp((const char *)sqlite3_column_text(q, 0), "drive") != 0) {
        /* A OneDrive or Dropbox account added by BRODALF 0.3.0. */
        bd_fail(cat, BD_ERR_INVALID, "cloud accounts are not supported in this version of BRODALF");
    } else if (sqlite3_column_type(q, 1) == SQLITE_NULL) {
        bd_fail(cat, BD_ERR_NOT_FOUND, "that drive is not connected");
    } else {
        s = bd_store_local(cat, media_id, (const char *)sqlite3_column_text(q, 1));
        if (!s) bd_fail(cat, BD_ERR_NOMEM, "out of memory");
    }
    sqlite3_finalize(q);
    return s;
}
