/* Backup to and restore from connected drives.
 *
 * On a drive, files live under BRODALF/<catalog uuid>/<root name>/<path>,
 * plain files in their folders, readable without BRODALF. Nothing on a
 * drive is ever overwritten, moved or deleted: a new version of a file
 * whose older version is already at that path goes alongside it, under
 * .versions/<root name>/<folder>/<name>.v<N><ext>. Every copy is written
 * to a temp name, checked against the catalog hash, synced, and only then
 * given its name, so a failed write never leaves a file that looks good.
 *
 * On an encrypted drive each copy is sealed with the catalog's master key
 * and named <name>.bdenc; names stay readable. */
#include "store.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BATCH 256
/* Copies are synced to the drive in groups of up to BATCH files or about
 * this much data, whichever comes first. */
#define SYNC_BYTES (64 * 1024 * 1024)

int64_t bd_test_free_bytes = -1;
int64_t bd_test_save_every_ms = 0;

/* How often a long backup saves the catalog between batches. */
#define SAVE_EVERY_MS (5 * 60 * 1000)

const char *bd_test_fs_name = NULL;
int64_t bd_test_fat_limit = 0;

/* FAT32 holds files up to 4 GiB less one byte, as does the FAT before it
 * (Windows calls FAT12 and FAT16 "FAT"); exFAT has no such limit. */
#define FAT_MAX_FILE 4294967295LL

static int fs_is_fat(const char *fs)
{
    char up[8];
    size_t i = 0;
    for (; fs[i] && i < sizeof(up) - 1; i++) up[i] = (char)(fs[i] >= 'a' && fs[i] <= 'z' ? fs[i] - 'a' + 'A' : fs[i]);
    up[i] = '\0';
    return strncmp(up, "FAT", 3) == 0 || strcmp(up, "VFAT") == 0 || strcmp(up, "MSDOS") == 0;
}

/* The file system is read now when the drive is plugged in, else taken as
 * last seen (media_hardware, which Windows fills when it is plugged in). */
int64_t bd_drive_file_limit(bd_catalog *cat, int64_t media_id, const char *root)
{
    char fs[64] = "";
    if (bd_test_fs_name) {
        snprintf(fs, sizeof(fs), "%s", bd_test_fs_name);
    } else if (!root || bd_fs_name(root, fs, sizeof(fs)) != 0) {
        fs[0] = '\0';
        sqlite3_stmt *q;
        if (sqlite3_prepare_v2(cat->db, "SELECT filesystem FROM media_hardware WHERE media_id=?", -1, &q, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(q, 1, media_id);
            if (sqlite3_step(q) == SQLITE_ROW && sqlite3_column_text(q, 0))
                snprintf(fs, sizeof(fs), "%s", (const char *)sqlite3_column_text(q, 0));
            sqlite3_finalize(q);
        }
    }
    if (!fs_is_fat(fs)) return -1;
    return bd_test_fat_limit > 0 ? bd_test_fat_limit : FAT_MAX_FILE;
}

typedef struct {
    int64_t node_id, node_size, node_mtime;
    int64_t version_id, version_no, version_size;
    char *rel, *source_path, *source_name, *hash, *orig;
    char *read_from; /* a copy from a shadow copy to read instead of orig, or NULL */
    int starred, encrypt;
} backup_item;

static void free_items(backup_item *items, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        free(items[i].orig);
        free(items[i].read_from);
        free(items[i].rel);
        free(items[i].source_path);
        free(items[i].source_name);
        free(items[i].hash);
    }
}

/* "<dir>/<stem>.v<N><ext>" under .versions, or with a tag instead of vN.
 * A .bdenc suffix stays last: "a.txt.bdenc" becomes "a.v1.txt.bdenc". */
static char *versions_rel_path(const char *dest_rel, const char *tag)
{
    size_t len = strlen(dest_rel), slen = strlen(BD_SEALED_SUFFIX);
    if (len > slen && strcmp(dest_rel + len - slen, BD_SEALED_SUFFIX) == 0) {
        char *plain = bd_strdup(dest_rel);
        if (!plain) return NULL;
        plain[len - slen] = '\0';
        char *inner = versions_rel_path(plain, tag);
        free(plain);
        char *r = inner ? bd_sprintf("%s%s", inner, BD_SEALED_SUFFIX) : NULL;
        free(inner);
        return r;
    }
    char *dir = bd_rel_dirname(dest_rel);
    const char *base = bd_rel_basename(dest_rel);
    const char *dot = strrchr(base, '.');
    if (dot == base) dot = NULL; /* ".bashrc" has no extension */
    size_t stem_len = dot ? (size_t)(dot - base) : strlen(base);
    char *stem = malloc(stem_len + 1);
    if (!dir || !stem) { free(dir); free(stem); return NULL; }
    memcpy(stem, base, stem_len);
    stem[stem_len] = '\0';
    char *r = *dir ? bd_sprintf("%s/%s/%s.%s%s", BD_VERSIONS_DIR, dir, stem, tag, dot ? dot : "")
                   : bd_sprintf("%s/%s.%s%s", BD_VERSIONS_DIR, stem, tag, dot ? dot : "");
    free(dir);
    free(stem);
    return r;
}

static int ensure_parent_dir(const char *path)
{
    char *p = bd_strdup(path);
    if (!p) return -1;
    char *slash = strrchr(p, '/');
    char *bslash = strrchr(p, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
    int rc = 0;
    if (slash) { *slash = '\0'; rc = bd_mkdirs(p); }
    free(p);
    return rc;
}

/* Is this path on the drive one a catalog row already gives to a copy? */
static int path_claimed(bd_catalog *cat, int64_t media_id, const char *rel)
{
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(cat->db, "SELECT 1 FROM copies WHERE media_id=? AND path_on_media=?", -1, &q, NULL) != SQLITE_OK)
        return 1;
    sqlite3_bind_int64(q, 1, media_id);
    sqlite3_bind_text(q, 2, rel, -1, SQLITE_STATIC);
    int taken = sqlite3_step(q) == SQLITE_ROW;
    sqlite3_finalize(q);
    return taken;
}

/* The path a copy has on the drive: as it is, or with the names hidden
 * (names.c) on a drive set up that way. NULL when out of memory. */
static char *on_drive(const bd_store *store, const char *plain)
{
    return store->hide_names ? bd_hide_rel(store->names_key, plain) : bd_strdup(plain);
}

/* Where a copy goes when its usual place on the drive holds something
 * else (an older version, or a file BRODALF does not know): next to it
 * under .versions, as <dir>/<stem>.v<N><ext>, or .v<N>-2 and so on when
 * that name is taken too. Nothing on a drive is ever overwritten, moved or
 * deleted. dest_rel is the plain path; what comes back is as on the
 * drive. NULL when the drive cannot be read. */
static char *alongside_path(bd_catalog *cat, bd_store *store, const char *dest_rel, int64_t version_no)
{
    for (int k = 1; k < 1000; k++) {
        char tag[48];
        if (k == 1) snprintf(tag, sizeof(tag), "v%lld", (long long)version_no);
        else snprintf(tag, sizeof(tag), "v%lld-%d", (long long)version_no, k);
        char *plain = versions_rel_path(dest_rel, tag);
        char *rel = plain ? on_drive(store, plain) : NULL;
        free(plain);
        if (!rel) return NULL;
        bd_remote_stat st;
        int r = store->ops->stat(store, rel, &st);
        if (r == 1 && !path_claimed(cat, store->media_id, rel)) return rel;
        free(rel);
        if (r < 0) return NULL;
    }
    return NULL;
}

static int record_copy(bd_catalog *cat, int64_t version_id, int64_t media_id, const char *rel, const bd_remote_stat *st,
                       int sealed)
{
    sqlite3_stmt *u;
    if (sqlite3_prepare_v2(cat->db,
                           "INSERT INTO copies(version_id, media_id, path_on_media, stored_size, stored_mtime_ns,"
                           " written_ms, last_quick_check_ms, state, encrypted) VALUES(?,?,?,?,?,?,?,'ok',?)"
                           " ON CONFLICT(version_id, media_id) DO UPDATE SET path_on_media=excluded.path_on_media,"
                           " stored_size=excluded.stored_size, stored_mtime_ns=excluded.stored_mtime_ns,"
                           " written_ms=excluded.written_ms, last_full_check_ms=NULL,"
                           " last_quick_check_ms=excluded.last_quick_check_ms, state='ok', encrypted=excluded.encrypted",
                           -1, &u, NULL) != SQLITE_OK)
        return -1;
    int64_t now = bd_now_ms();
    sqlite3_bind_int64(u, 1, version_id);
    sqlite3_bind_int64(u, 2, media_id);
    sqlite3_bind_text(u, 3, rel, -1, SQLITE_STATIC);
    sqlite3_bind_int64(u, 4, st->size);
    sqlite3_bind_int64(u, 5, st->mtime_ns);
    sqlite3_bind_int64(u, 6, now);
    sqlite3_bind_int64(u, 7, now);
    sqlite3_bind_int(u, 8, sealed ? 1 : 0);
    int rc = sqlite3_step(u) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(u);
    return rc;
}

int64_t bd_test_dest_room = -1;
int bd_test_dest_read_only = 0;

/* Copy src (size bytes) to a temp file next to dest while hashing; keep
 * the temp file only if the hash matches. src_key decrypts the source,
 * dst_key encrypts the copy. Returns 0 and sets *tmp_out on success, 1 on
 * hash mismatch or a damaged encrypted source, -1 when the source cannot
 * be read, -2 when dest's folder cannot be written (*werr: the errno that
 * says why, 0 when the system did not say), -4 if the job was stopped
 * (the temp file is removed in every case but success). */
static int copy_verified(const char *src, const uint8_t *src_key, const char *dest, const uint8_t *dst_key,
                         const char *expected, int64_t size, int64_t mtime_ns, int64_t btime_ns, char **tmp_out,
                         int64_t *bytes, int *werr)
{
    *tmp_out = NULL;
    *werr = 0;
    char *tmp = bd_sprintf("%s" BD_TMP_MARKER, dest);
    if (!tmp) return -1;
    errno = 0;
    if (bd_test_dest_read_only || ensure_parent_dir(dest) != 0) {
        *werr = bd_test_dest_read_only ? EROFS : errno;
        free(tmp);
        return -2;
    }
    errno = 0;
    FILE *out = bd_fopen(tmp, "wb");
    if (!out) { *werr = errno; free(tmp); return -2; }
    char hash[BD_HASH_HEX_LEN + 1];
    int rc;
    if (bd_test_dest_room >= 0 && size > bd_test_dest_room) {
        bd_test_dest_room = 0;
        rc = -2;
        errno = ENOSPC;
    } else {
        if (bd_test_dest_room >= 0) bd_test_dest_room -= size;
        rc = bd_hash_copy(src, src_key, out, dst_key, hash, bytes);
    }
    if (rc == -2) *werr = errno;
    if (rc == -3) rc = 1;
    /* The original's modified and created times; a file that cannot take
     * them is still restored. */
    if (rc == 0 && mtime_ns > 0) bd_set_times(out, mtime_ns, btime_ns);
    errno = 0;
    if (rc == 0 && bd_fsync(out) != 0) { rc = -2; *werr = errno; }
    errno = 0;
    if (fclose(out) != 0 && rc == 0) { rc = -2; *werr = errno; }
    if (rc == 0 && strcmp(hash, expected) != 0) rc = 1;
    if (rc != 0) { bd_remove(tmp); free(tmp); return rc; }
    *tmp_out = tmp;
    return 0;
}

/* A running job's counts so far, for a catalog saved before it ends. */
static void note_job_progress(bd_catalog *cat, int64_t job, const bd_backup_stats *st)
{
    sqlite3_stmt *u;
    if (!job || sqlite3_prepare_v2(cat->db, "UPDATE jobs SET files_done=?, files_failed=?, bytes_done=? WHERE id=? AND finished_ms IS NULL",
                                   -1, &u, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int64(u, 1, st->files_copied);
    sqlite3_bind_int64(u, 2, st->files_failed);
    sqlite3_bind_int64(u, 3, st->bytes_copied);
    sqlite3_bind_int64(u, 4, job);
    sqlite3_step(u);
    sqlite3_finalize(u);
}

int64_t bd_job_start(bd_catalog *cat, const char *kind, int64_t media_id)
{
    sqlite3_stmt *ins;
    if (sqlite3_prepare_v2(cat->db, "INSERT INTO jobs(kind, media_id, started_ms, status) VALUES(?,?,?,'running')", -1, &ins, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_text(ins, 1, kind, -1, SQLITE_STATIC);
    if (media_id) sqlite3_bind_int64(ins, 2, media_id);
    else sqlite3_bind_null(ins, 2);
    sqlite3_bind_int64(ins, 3, bd_now_ms());
    int64_t id = sqlite3_step(ins) == SQLITE_DONE ? sqlite3_last_insert_rowid(cat->db) : 0;
    sqlite3_finalize(ins);
    bd_job_power_begin(bd_option_get(cat, "low_priority"));
    /* There is no history to browse: only each drive's last backup and
     * check are shown, so older rows are dropped, never those. */
    if (id > 2000) {
        char sql[192];
        snprintf(sql, sizeof(sql),
                 "DELETE FROM jobs WHERE id<=%lld AND id NOT IN (SELECT MAX(id) FROM jobs GROUP BY media_id, kind)",
                 (long long)(id - 2000));
        bd_exec(cat, sql);
    }
    return id;
}

void bd_job_finish(bd_catalog *cat, int64_t job_id, const char *status, int64_t done, int64_t failed, int64_t bytes)
{
    bd_job_power_end();
    sqlite3_stmt *u;
    if (!job_id || sqlite3_prepare_v2(cat->db, "UPDATE jobs SET finished_ms=?, status=?, files_done=?, files_failed=?, bytes_done=? WHERE id=?", -1, &u, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int64(u, 1, bd_now_ms());
    sqlite3_bind_text(u, 2, status, -1, SQLITE_STATIC);
    sqlite3_bind_int64(u, 3, done);
    sqlite3_bind_int64(u, 4, failed);
    sqlite3_bind_int64(u, 5, bytes);
    sqlite3_bind_int64(u, 6, job_id);
    sqlite3_step(u);
    sqlite3_finalize(u);
}

/* A copy written and checked, waiting for its data to be synced before it
 * is renamed into place and recorded. */
typedef struct {
    backup_item *it;
    char *dest_rel, *tmp_rel, *local_tmp;
    const char *orig;
    FILE *out;  /* open while the copy is written */
    int failed, alongside, sealed;
    int64_t bytes;
} pending_copy;

static void pending_free(pending_copy *p)
{
    if (p->out) fclose(p->out);
    free(p->dest_rel);
    free(p->tmp_rel);
    free(p->local_tmp);
    memset(p, 0, sizeof(*p));
}

/* Where names ignore case (Windows), the file at dest_rel may be a copy of
 * this file under a name that differs only in case: it was renamed that
 * way. 1 if one of this file's copies on the drive has that name. */
static int tracked_by_case(bd_catalog *cat, int64_t media_id, const backup_item *it, const char *dest_rel)
{
    char *want = bd_path_key(dest_rel);
    sqlite3_stmt *q;
    int found = 0;
    if (!want) return 0;
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT c.path_on_media FROM copies c JOIN versions v ON v.id=c.version_id"
                           " WHERE v.node_id=? AND c.media_id=?",
                           -1, &q, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(q, 1, it->node_id);
        sqlite3_bind_int64(q, 2, media_id);
        while (!found && sqlite3_step(q) == SQLITE_ROW) {
            char *key = bd_path_key((const char *)sqlite3_column_text(q, 0));
            found = key && strcmp(key, want) == 0;
            free(key);
        }
        sqlite3_finalize(q);
    }
    free(want);
    return found;
}

/* A file that changed after the scan: read it now and record what it is
 * as a new version (as a scan would), so that version is what gets
 * copied. 0 ok, 1 it is already on this drive after all, -1 failed. */
static int rehash_changed(bd_catalog *cat, int64_t media_id, backup_item *it, const char *src, const bd_stat_t *sst,
                          bd_backup_stats *stats, bd_log_fn log, void *log_ctx)
{
    char hash[BD_HASH_HEX_LEN + 1];
    int64_t got = 0;
    if (bd_hash_file(src, NULL, hash, &got) != 0) {
        if (bd_open_was_in_use()) {
            stats->files_in_use++;
            bd_note_in_use(cat, src, sst->size);
            bd_logf(log, log_ctx, "in use by another program, skipped: %s", src);
        } else {
            bd_logf(log, log_ctx, "cannot read %s", src);
        }
        return -1;
    }
    int64_t vid = 0, vno = 0;
    if (bd_record_version(cat, it->node_id, hash, got, sst->mtime_ns, &vid, &vno) != 0) {
        bd_logf(log, log_ctx, "cannot record the new version of %s: %s", src, sqlite3_errmsg(cat->db));
        return -1;
    }
    stats->files_rehashed++;
    if (stats->files_rehashed == 1) bd_logf(log, log_ctx, "files changed since the scan are read again and backed up as they are now");
    char *h = bd_strdup(hash);
    if (!h) return -1;
    free(it->hash);
    it->hash = h;
    it->version_id = vid;
    it->version_no = vno;
    it->version_size = it->node_size = got;
    it->node_mtime = sst->mtime_ns;
    sqlite3_stmt *q;
    int there = 0;
    if (sqlite3_prepare_v2(cat->db, "SELECT 1 FROM copies WHERE version_id=? AND media_id=? AND state='ok'", -1, &q, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(q, 1, vid);
        sqlite3_bind_int64(q, 2, media_id);
        there = sqlite3_step(q) == SQLITE_ROW;
        sqlite3_finalize(q);
    }
    return there ? 1 : 0;
}

/* What happened to the file itself, before any drive. */
enum { PREP_OK, PREP_MISSING, PREP_ONLINE, PREP_FAILED, PREP_IN_USE };

/* Look at the file in its folder; read it again if it changed since the
 * scan (it->version_* then describe it as it is now). */
static int item_prepare(bd_catalog *cat, backup_item *it, int *rehashed, bd_log_fn log, void *log_ctx)
{
    *rehashed = 0;
    it->orig = bd_path_join(it->source_path, it->rel);
    if (!it->orig) return PREP_FAILED;
    bd_stat_t sst;
    if (bd_stat(it->orig, &sst) != 0 || !sst.is_file) {
        bd_logf(log, log_ctx, "source file missing, scan again: %s", it->orig);
        return PREP_MISSING;
    }
    /* Reading a cloud file that is not on this PC would download it. */
    if (sst.online_only) {
        bd_logf(log, log_ctx, "online-only (not on this PC), not copied: %s", it->orig);
        return PREP_ONLINE;
    }
    /* A file in use is read from its shadow copy, when the user had one taken. */
    free(it->read_from);
    it->read_from = bd_substitute_for(cat, it->orig);
    if (sst.size != it->node_size || sst.mtime_ns != it->node_mtime) {
        bd_backup_stats scratch;
        memset(&scratch, 0, sizeof(scratch));
        int r = rehash_changed(cat, 0, it, it->read_from ? it->read_from : it->orig, &sst, &scratch, log, log_ctx);
        if (r < 0) return scratch.files_in_use ? PREP_IN_USE : PREP_FAILED;
        *rehashed = 1;
    }
    return PREP_OK;
}

/* A copy of this version that an earlier backup put alongside under
 * .versions (alongside_path's names for it, in order) but never recorded,
 * because the catalog was not saved after it (a crash, a power cut, the
 * drive pulled). If one is there, not claimed by any copy the catalog
 * knows and with exactly this version's checksum, it is recorded instead
 * of copied again, as dest_prepare does for the usual place. Only looked
 * for when the drive holds some version of the file, as only then does a
 * copy go alongside. 1 if adopted (or adopting it failed: counted either
 * way), 0 to copy as usual. Nothing on the drive is touched. */
static int adopt_alongside(bd_catalog *cat, bd_store *store, const uint8_t *key, const backup_item *it,
                           bd_backup_stats *stats)
{
    int64_t media_id = store->media_id;
    sqlite3_stmt *q;
    int held = 0;
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT 1 FROM copies c JOIN versions v ON v.id=c.version_id"
                           " WHERE v.node_id=? AND c.media_id=? AND c.version_id<>? LIMIT 1",
                           -1, &q, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(q, 1, it->node_id);
        sqlite3_bind_int64(q, 2, media_id);
        sqlite3_bind_int64(q, 3, it->version_id);
        held = sqlite3_step(q) == SQLITE_ROW;
        sqlite3_finalize(q);
    }
    if (!held) return 0;
    char *main_rel = bd_sprintf("%s/%s%s", it->source_name, it->rel, key ? BD_SEALED_SUFFIX : "");
    if (!main_rel) return 0;
    int adopted = 0;
    for (int k = 1; k < 1000 && !adopted; k++) {
        char tag[48];
        if (k == 1) snprintf(tag, sizeof(tag), "v%lld", (long long)it->version_no);
        else snprintf(tag, sizeof(tag), "v%lld-%d", (long long)it->version_no, k);
        char *plain = versions_rel_path(main_rel, tag);
        char *rel = plain ? on_drive(store, plain) : NULL;
        free(plain);
        if (!rel) break;
        bd_remote_stat st;
        int r = store->ops->stat(store, rel, &st);
        if (r != 0) { free(rel); break; } /* the first free name: nothing was put beyond it */
        if (!path_claimed(cat, media_id, rel)) {
            char hash[BD_HASH_HEX_LEN + 1];
            char *local = NULL;
            int is_temp = 0;
            int same = bd_store_fetch(store, rel, &local, &is_temp) == 0 &&
                       bd_hash_copy(local, key, NULL, NULL, hash, NULL) == 0 && strcmp(hash, it->hash) == 0;
            bd_store_release(local, is_temp);
            if (same) {
                if (record_copy(cat, it->version_id, media_id, rel, &st, key != NULL) == 0) stats->files_already_there++;
                else stats->files_failed++;
                adopted = 1;
            }
        }
        free(rel);
    }
    free(main_rel);
    return adopted;
}

static int has_good_copy(bd_catalog *cat, int64_t version_id, int64_t media_id)
{
    sqlite3_stmt *q;
    int there = 0;
    if (sqlite3_prepare_v2(cat->db, "SELECT 1 FROM copies WHERE version_id=? AND media_id=? AND state='ok'", -1, &q, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(q, 1, version_id);
        sqlite3_bind_int64(q, 2, media_id);
        there = sqlite3_step(q) == SQLITE_ROW;
        sqlite3_finalize(q);
    }
    return there;
}

/* A copy marked missing is at rel again with its recorded size: if it
 * reads back as the version it holds (expected), it counts again. 1 if so. */
static int revive_copy(bd_catalog *cat, bd_store *store, int64_t copy_id, const char *rel, int64_t size, int sealed,
                       const char *expected, const char *shown, bd_log_fn log, void *log_ctx)
{
    if (sealed && !cat->have_key) return 0;
    bd_remote_stat st;
    if (store->ops->stat(store, rel, &st) != 0 || st.size != size) return 0;
    char hash[BD_HASH_HEX_LEN + 1];
    char *local = NULL;
    int is_temp = 0;
    int same = bd_store_fetch(store, rel, &local, &is_temp) == 0 &&
               bd_hash_copy(local, sealed ? cat->key : NULL, NULL, NULL, hash, NULL) == 0 && strcmp(hash, expected) == 0;
    bd_store_release(local, is_temp);
    if (!same) return 0;
    sqlite3_stmt *u;
    int64_t now = bd_now_ms();
    int back = 0;
    if (sqlite3_prepare_v2(cat->db,
                           "UPDATE copies SET state='ok', stored_mtime_ns=?, last_quick_check_ms=?, last_full_check_ms=?"
                           " WHERE id=? AND state='missing'",
                           -1, &u, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(u, 1, st.mtime_ns);
        sqlite3_bind_int64(u, 2, now);
        sqlite3_bind_int64(u, 3, now);
        sqlite3_bind_int64(u, 4, copy_id);
        back = sqlite3_step(u) == SQLITE_DONE && sqlite3_changes(cat->db) > 0;
        sqlite3_finalize(u);
    }
    if (back) bd_logf(log, log_ctx, "the copy is there again as it was, so it counts again: %s", shown);
    return back;
}

/* This version's copy on this drive was marked missing. If it is there
 * again as it was (the drive was unplugged, a folder was put back), it
 * counts again and no second copy is written. 1 if so. */
static int missing_copy_back(bd_catalog *cat, bd_store *store, const backup_item *it, bd_log_fn log, void *log_ctx)
{
    sqlite3_stmt *q;
    int back = 0;
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT id, path_on_media, stored_size, encrypted FROM copies"
                           " WHERE version_id=? AND media_id=? AND state='missing'",
                           -1, &q, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_int64(q, 1, it->version_id);
    sqlite3_bind_int64(q, 2, store->media_id);
    if (sqlite3_step(q) == SQLITE_ROW) {
        const char *rel = (const char *)sqlite3_column_text(q, 1);
        back = rel && revive_copy(cat, store, sqlite3_column_int64(q, 0), rel, sqlite3_column_int64(q, 2),
                                  sqlite3_column_int(q, 3), it->hash, store->hide_names ? it->orig : rel, log, log_ctx);
    }
    sqlite3_finalize(q);
    return back;
}

/* Work out where the copy goes on this drive and open its temp file
 * (<dest>.brodalf-tmp) for writing. 1 if *p is ready to be written; 0 if
 * the file is done with on this drive (failed, or already there). A copy
 * found gone from the drive is added to marks, to be recorded as missing
 * once the backup ends with the drive still there. */
static int dest_prepare(bd_catalog *cat, bd_store *store, const uint8_t *key, backup_item *it, pending_copy *p,
                        bd_copy_marks *marks, bd_backup_stats *stats, bd_log_fn log, void *log_ctx)
{
    int64_t media_id = store->media_id;
    memset(p, 0, sizeof(*p));
    p->it = it;
    p->orig = it->orig;
    p->sealed = key != NULL;
    const char *src = it->orig;
    if (missing_copy_back(cat, store, it, log, log_ctx)) {
        stats->files_already_there++;
        return 0;
    }
    char *main_rel = bd_sprintf("%s/%s%s", it->source_name, it->rel, key ? BD_SEALED_SUFFIX : "");
    char *main_disk = main_rel ? on_drive(store, main_rel) : NULL; /* the same, as on the drive */
    if (!main_disk) { free(main_rel); stats->files_failed++; goto drop; }

    /* What is at the usual place now, and what the catalog says is there. */
    bd_remote_stat dst_st;
    int found = store->ops->stat(store, main_disk, &dst_st);
    if (found < 0) {
        stats->files_failed++;
        bd_logf(log, log_ctx, "cannot look at %s: %s", main_rel, store->err);
        free(main_rel);
        free(main_disk);
        goto drop;
    }
    int64_t row_copy = 0, row_version = 0, row_size = 0;
    int row_missing = 0, row_sealed = 0;
    char row_hash[BD_HASH_HEX_LEN + 1] = "";
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT c.id, c.version_id, c.state='missing', c.stored_size, c.encrypted, v.hash FROM copies c"
                           " JOIN versions v ON v.id=c.version_id WHERE c.media_id=? AND c.path_on_media=?",
                           -1, &q, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(q, 1, media_id);
        sqlite3_bind_text(q, 2, main_disk, -1, SQLITE_STATIC);
        if (sqlite3_step(q) == SQLITE_ROW) {
            row_copy = sqlite3_column_int64(q, 0);
            row_version = sqlite3_column_int64(q, 1);
            row_missing = sqlite3_column_int(q, 2);
            row_size = sqlite3_column_int64(q, 3);
            row_sealed = sqlite3_column_int(q, 4);
            snprintf(row_hash, sizeof(row_hash), "%s", (const char *)sqlite3_column_text(q, 5));
        }
        sqlite3_finalize(q);
    }
    /* An older version's copy marked missing is back at its place: it
     * counts again (this version still goes alongside it). */
    if (found == 0 && row_copy && row_missing && row_version != it->version_id)
        revive_copy(cat, store, row_copy, main_disk, row_size, row_sealed, row_hash, main_rel, log, log_ctx);

    int use_main = 0;
    if (found == 1) {
        if (row_copy && row_version != it->version_id) {
            /* The catalog says another version's copy is here, but it is
             * gone from the drive. Keep the record, to be marked missing if
             * the drive is still there when the backup ends; the new copy
             * goes alongside. */
            if (!row_missing) {
                if (bd_copy_marks_add(marks, row_copy, media_id, 0, 0) != 0) {
                    stats->files_failed++;
                    free(main_rel);
                    free(main_disk);
                    goto drop;
                }
                bd_logf(log, log_ctx, "an older copy is gone from the drive: %s", main_rel);
            }
        } else {
            use_main = 1;
        }
    } else if (!row_copy && !tracked_by_case(cat, media_id, it, main_disk)) {
        /* A file the catalog does not know, probably written by a backup
         * whose catalog was never saved. If it is exactly this version,
         * adopt it instead of copying. */
        char hash[BD_HASH_HEX_LEN + 1];
        char *local = NULL;
        int is_temp = 0;
        int same = bd_store_fetch(store, main_disk, &local, &is_temp) == 0 &&
                   bd_hash_copy(local, key, NULL, NULL, hash, NULL) == 0 && strcmp(hash, it->hash) == 0;
        bd_store_release(local, is_temp);
        if (same) {
            if (record_copy(cat, it->version_id, media_id, main_disk, &dst_st, key != NULL) == 0) stats->files_already_there++;
            else stats->files_failed++;
            free(main_rel);
            free(main_disk);
            goto drop;
        }
        bd_logf(log, log_ctx, "a file BRODALF did not write is at %s; it is left alone", main_rel);
    }
    if (use_main) {
        p->dest_rel = main_disk;
        free(main_rel);
    } else {
        p->dest_rel = alongside_path(cat, store, main_rel, it->version_no);
        p->alongside = 1;
        free(main_rel);
        free(main_disk);
        if (!p->dest_rel) {
            stats->files_failed++;
            bd_logf(log, log_ctx, "cannot find a free name on the drive for %s: %s", src, store->err);
            goto drop;
        }
    }
    p->tmp_rel = bd_sprintf("%s" BD_TMP_MARKER, p->dest_rel);
    if (!p->tmp_rel || !(p->local_tmp = store->ops->staging_path(store, p->tmp_rel)) ||
        !(p->out = bd_fopen(p->local_tmp, "wb"))) {
        stats->files_failed++;
        bd_logf(log, log_ctx, "cannot copy %s to the drive%s%s", src, store->err[0] ? ": " : "", store->err);
        goto drop;
    }
    return 1;

drop:
    pending_free(p);
    return 0;
}

/* A file changed while it was being copied: how many times now. */
static int note_copy_changed(bd_catalog *cat, const char *path)
{
    sqlite3_stmt *q;
    int times = 1;
    if (sqlite3_prepare_v2(cat->db,
                           "INSERT INTO temp.copy_changed(path, times) VALUES(?1,1)"
                           " ON CONFLICT(path) DO UPDATE SET times=times+1 RETURNING times",
                           -1, &q, NULL) != SQLITE_OK)
        return times;
    sqlite3_bind_text(q, 1, path, -1, SQLITE_STATIC);
    if (sqlite3_step(q) == SQLITE_ROW) times = sqlite3_column_int(q, 0);
    sqlite3_finalize(q);
    return times;
}

/* The copy is written (rc and failed from bd_hash_copy_multi): give it the
 * file's modified time and close it, and keep it only if what was read
 * is the version being backed up. 1 if it waits to be synced, 0 dropped,
 * 2 dropped because the job was stopped. */
static int dest_written(bd_store *store, pending_copy *p, int rc, int failed, int64_t bytes, const char *hash,
                        bd_backup_stats *stats, int *in_use_noted, bd_catalog *cat, bd_log_fn log, void *log_ctx)
{
    const backup_item *it = p->it;
    /* After the last write. The copy's own time on the drive is recorded
     * after the rename (record_copy), so the quick check compares like with
     * like whether this worked or not. */
    if (rc == 0 && !failed && it->node_mtime > 0) bd_set_mtime(p->out, it->node_mtime);
    int closed = fclose(p->out) == 0;
    p->out = NULL;
    int keep = 0;
    if (rc == -4) {
        /* Stopped halfway: the partial copy is removed below. */
    } else if (rc == -1) {
        stats->files_failed++;
        if (bd_open_was_in_use()) {
            stats->files_in_use++;
            if (!*in_use_noted) {
                bd_note_in_use(cat, p->orig, it->node_size);
                bd_logf(log, log_ctx, "in use by another program, skipped: %s", p->orig);
                *in_use_noted = 1;
            }
        } else {
            bd_logf(log, log_ctx, "cannot read %s", p->orig);
        }
    } else if (failed || !closed) {
        stats->files_failed++;
        bd_logf(log, log_ctx, "cannot copy %s to the drive%s%s", p->orig, store->err[0] ? ": " : "", store->err);
    } else if (strcmp(hash, it->hash) != 0) {
        stats->files_failed++;
        /* Once is a file saved during the backup. Again and again is a file
         * another program keeps writing (a virtual machine's disk, a mail
         * store): like a file in use, a shadow copy can back it up. */
        if (!*in_use_noted) {
            if (note_copy_changed(cat, p->orig) >= 2) {
                bd_note_in_use(cat, p->orig, it->node_size);
                bd_logf(log, log_ctx, "changed while copying again, so another program keeps writing it: %s", p->orig);
                *in_use_noted = 3;
            } else {
                bd_logf(log, log_ctx, "changed while copying, scan again: %s", p->orig);
                *in_use_noted = 2;
            }
        }
        if (*in_use_noted == 3) stats->files_in_use++;
    } else {
        keep = 1;
    }
    if (!keep) {
        bd_remove(p->local_tmp);
        store->ops->remove(store, p->tmp_rel); /* only its own half-written temp file */
        pending_free(p);
        return rc == -4 ? 2 : 0;
    }
    p->bytes = bytes;
    return 1;
}

/* The copy's data is on the drive: give it its name and record it. */
static void backup_finish(bd_catalog *cat, bd_store *store, pending_copy *p, bd_backup_stats *stats, bd_log_fn log,
                          void *log_ctx)
{
    const backup_item *it = p->it;
    const char *dest_rel = p->dest_rel, *tmp_rel = p->tmp_rel;
    const char *shown = store->hide_names ? p->orig : dest_rel; /* hidden names mean nothing to the user */
    bd_remote_stat new_st;
    if (p->failed) {
        stats->files_failed++;
        bd_logf(log, log_ctx, "cannot write the copy of %s to the drive", p->orig);
        store->ops->remove(store, tmp_rel);
        return;
    }
    if (store->ops->upload(store, p->local_tmp, tmp_rel, &new_st) != 0) {
        stats->files_failed++;
        bd_logf(log, log_ctx, "cannot copy %s to the drive: %s", p->orig, store->err);
        store->ops->remove(store, tmp_rel);
        return;
    }
    /* Never over anything: if a file appeared at that name meanwhile, the
     * copy waits for the next backup. */
    int mv = store->ops->move(store, tmp_rel, dest_rel, 0);
    if (mv != 0) {
        stats->files_failed++;
        if (mv == 1) bd_logf(log, log_ctx, "a file appeared at %s while copying; left alone, back up again", shown);
        else bd_logf(log, log_ctx, "cannot finish writing %s: %s", shown, store->err);
        store->ops->remove(store, tmp_rel);
        return;
    }
    if (store->ops->stat(store, dest_rel, &new_st) != 0 ||
        record_copy(cat, it->version_id, store->media_id, dest_rel, &new_st, p->sealed) != 0) {
        stats->files_failed++;
        bd_logf(log, log_ctx, "copied %s but could not record it: %s", shown, sqlite3_errmsg(cat->db));
        return;
    }
    stats->files_copied++;
    stats->bytes_copied += p->bytes;
    if (p->alongside) stats->copies_alongside++;
}

/* Sync a group of written copies, then rename each into place and record
 * it, then sync the folders that got new names. Syncing many small files
 * together instead of one by one is much faster, and no copy is renamed
 * into place or recorded before its data is on the drive. */
static void sync_group(bd_catalog *cat, bd_store *store, pending_copy *group, size_t n, bd_backup_stats *stats,
                       bd_log_fn log, void *log_ctx)
{
    if (n == 0) return;
    char *paths[BATCH];
    int failed[BATCH];
    for (size_t i = 0; i < n; i++) paths[i] = group[i].local_tmp;
    bd_fsync_paths(paths, n, failed);
    for (size_t i = 0; i < n; i++) group[i].failed = failed[i];
    char *last_dir = NULL;
    for (size_t i = 0; i < n; i++) {
        backup_finish(cat, store, &group[i], stats, log, log_ctx);
        char *local = group[i].failed ? NULL : store->ops->local_path(store, group[i].dest_rel);
        char *dir = local ? bd_rel_dirname(local) : NULL;
        if (dir && (!last_dir || strcmp(dir, last_dir) != 0)) {
            bd_fsync_dir(dir);
            free(last_dir);
            last_dir = dir;
        } else {
            free(dir);
        }
        free(local);
        pending_free(&group[i]);
    }
    free(last_dir);
}

/* ---- Backup roots a backup leaves out ------------------------------------ */

/* A backup copies only what a scan found in a root's own folder. Roots the
 * last scan held back (most of the files it knows there were not: another
 * folder with the same name), or whose folder cannot be found now, are left
 * out: reading the folder at their path would record its other files as
 * new versions of the root's. The roots a backup leaves out go in
 * temp.bk_out, those a preview or the Next backup list leaves out in
 * temp.pv_out (so a preview while a backup runs changes nothing for it).
 * temp.bk_order holds what a backup copies, in the order it copies it. */
static int backup_temp_tables(bd_catalog *cat)
{
    return bd_exec(cat, "CREATE TEMP TABLE IF NOT EXISTS bk_out(source_id INTEGER PRIMARY KEY);"
                        "CREATE TEMP TABLE IF NOT EXISTS pv_out(source_id INTEGER PRIMARY KEY);"
                        "CREATE TEMP TABLE IF NOT EXISTS bk_order(seq INTEGER PRIMARY KEY, node_id INTEGER NOT NULL UNIQUE);");
}

void bd_root_held_note(bd_catalog *cat, int64_t source_id, int64_t files_gone)
{
    char key[48], now[24], want[24];
    snprintf(key, sizeof(key), "held_root.%lld", (long long)source_id);
    int had = bd_setting_get(cat, key, now, sizeof(now));
    if (files_gone > 0) {
        snprintf(want, sizeof(want), "%lld", (long long)files_gone);
        if (!had || strcmp(now, want) != 0) bd_setting_set(cat, key, want);
        return;
    }
    if (!had) return; /* nothing to change: the catalog is not touched */
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(cat->db, "DELETE FROM settings WHERE key=?", -1, &q, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(q, 1, key, -1, SQLITE_STATIC);
    sqlite3_step(q);
    sqlite3_finalize(q);
}

const char *bd_root_out_text(bd_root_out_why why)
{
    switch (why) {
    case BD_ROOT_NOT_FOUND: return "its folder cannot be found";
    case BD_ROOT_WAITING: return "it waits for its disk to be plugged in";
    case BD_ROOT_HELD: return "the last scan did not find most of its files in the folder there";
    case BD_ROOT_NOT_SCANNED: return "it was not found at the last scan; scan it first";
    }
    return "";
}

/* The roots left out, into temp table `table` when given (emptied first),
 * each to fn and, with log, one line each saying why. Returns how many, or
 * -1 when the catalog cannot be read. */
static int64_t roots_left_out(bd_catalog *cat, int64_t source_id, const char *table, bd_root_left_out_fn fn, void *ctx,
                              bd_log_fn log, void *log_ctx)
{
    sqlite3_stmt *q, *ins = NULL;
    if (table) {
        char *sql = bd_sprintf("DELETE FROM temp.%s", table);
        int bad = !sql || backup_temp_tables(cat) != 0 || bd_exec(cat, sql) != 0;
        free(sql);
        sql = bad ? NULL : bd_sprintf("INSERT OR IGNORE INTO temp.%s(source_id) VALUES(?)", table);
        if (!sql || sqlite3_prepare_v2(cat->db, sql, -1, &ins, NULL) != SQLITE_OK) bad = 1;
        free(sql);
        if (bad) { sqlite3_finalize(ins); return -1; }
    }
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT s.id, s.name, s.path, ms.source_id IS NOT NULL, COALESCE(ms.files_gone,0),"
                           " EXISTS(SELECT 1 FROM settings w WHERE w.key='wait_disk.'||s.id),"
                           " COALESCE((SELECT CAST(h.value AS INTEGER) FROM settings h WHERE h.key='held_root.'||s.id),0)"
                           " FROM sources s LEFT JOIN temp.missing_sources ms ON ms.source_id=s.id"
                           " WHERE s.active<>0 AND (?1=0 OR s.id=?1) ORDER BY s.name COLLATE NOCASE, s.id",
                           -1, &q, NULL) != SQLITE_OK) {
        sqlite3_finalize(ins);
        return -1;
    }
    sqlite3_bind_int64(q, 1, source_id);
    int64_t count = 0;
    int stop = 0;
    while (sqlite3_step(q) == SQLITE_ROW) {
        bd_root_left_out r;
        memset(&r, 0, sizeof(r));
        r.source_id = sqlite3_column_int64(q, 0);
        r.name = (const char *)sqlite3_column_text(q, 1);
        r.path = (const char *)sqlite3_column_text(q, 2);
        int in_session = sqlite3_column_int(q, 3), waits = sqlite3_column_int(q, 5);
        int64_t gone_now = sqlite3_column_int64(q, 4), gone_kept = sqlite3_column_int64(q, 6);
        bd_stat_t st;
        int there = r.path && bd_stat(r.path, &st) == 0 && st.is_dir;
        if (in_session && gone_now > 0) { r.why = BD_ROOT_HELD; r.files_gone = gone_now; }
        else if (in_session) r.why = there ? BD_ROOT_NOT_SCANNED : waits ? BD_ROOT_WAITING : BD_ROOT_NOT_FOUND;
        else if (gone_kept > 0) { r.why = BD_ROOT_HELD; r.files_gone = gone_kept; }
        else if (!there) r.why = waits ? BD_ROOT_WAITING : BD_ROOT_NOT_FOUND;
        else continue;
        count++;
        if (ins) {
            sqlite3_reset(ins);
            sqlite3_bind_int64(ins, 1, r.source_id);
            sqlite3_step(ins);
        }
        if (log) {
            if (r.why == BD_ROOT_HELD)
                bd_logf(log, log_ctx, "backup root %s is left out of this backup: the last scan did not find %lld of its files "
                                      "in the folder there. Say where it is now, or scan again",
                        r.path ? r.path : "", (long long)r.files_gone);
            else
                bd_logf(log, log_ctx, "backup root %s is left out of this backup: %s", r.path ? r.path : "", bd_root_out_text(r.why));
        }
        if (fn && !stop && fn(ctx, &r) != 0) stop = 1;
        if (stop && !ins) break;
    }
    sqlite3_finalize(q);
    sqlite3_finalize(ins);
    return count;
}

bd_status bd_list_roots_left_out(bd_catalog *cat, int64_t source_id, bd_root_left_out_fn fn, void *ctx, int64_t *count)
{
    int64_t n = roots_left_out(cat, source_id, NULL, fn, ctx, NULL, NULL);
    if (count) *count = n > 0 ? n : 0;
    return n < 0 ? bd_fail_db(cat, "list the backup roots left out") : BD_OK;
}

/* The files a backup to one drive copies (nodes n, their current versions
 * v): no good copy on this drive (?1), and for an extra place only starred
 * files. ?2 backup root (0: all), ?3 after this node id. Roots no longer
 * backed up, and roots left out (temp.pv_out), are left out. */
#define TODO_WHERE                                                                                      \
    " n.is_dir=0 AND n.deleted=0 AND (?2=0 OR n.source_id=?2) AND n.id>?3 AND " BD_SQL_ACTIVE("n")      \
    " AND NOT EXISTS(SELECT 1 FROM temp.pv_out lo WHERE lo.source_id=n.source_id)"                       \
    " AND NOT EXISTS(SELECT 1 FROM copies c WHERE c.version_id=v.id AND c.media_id=?1 AND c.state='ok')" \
    " AND (NOT EXISTS(SELECT 1 FROM media sm WHERE sm.id=?1 AND sm.secondary<>0) OR " BD_SQL_MARKED("stars", "n") ")"

/* The same for the drives in temp.bk_dests: what at least one of them
 * needs, the roots in temp.bk_out left out. ?1 backup root. */
#define TODO_MANY_COND                                                                                  \
    " n.is_dir=0 AND n.deleted=0 AND (?1=0 OR n.source_id=?1) AND " BD_SQL_ACTIVE("n")                  \
    " AND NOT EXISTS(SELECT 1 FROM temp.bk_out lo WHERE lo.source_id=n.source_id)"                       \
    " AND EXISTS(SELECT 1 FROM temp.bk_dests d WHERE (d.secondary=0 OR " BD_SQL_MARKED("stars", "n") ")" \
    "  AND NOT EXISTS(SELECT 1 FROM copies c WHERE c.version_id=v.id AND c.media_id=d.media_id AND c.state='ok'))"

int64_t bd_reserve_bytes(bd_catalog *cat)
{
    return (int64_t)bd_option_get(cat, "reserve_mb") * 1024 * 1024;
}

enum { MAX_DESTS = 16 };

/* One drive a backup writes to. */
typedef struct {
    int64_t media_id;
    bd_store *store;
    int encrypted, secondary;
    char label[128];
    int64_t space_free, reserve; /* free space -1: unknown, no limit */
    int64_t budget;              /* an extra place's limit left; -1: none */
    bd_backup_stats *stats;
    pending_copy group[BATCH];
    size_t ng;
    int64_t group_bytes;
    int64_t job;
    bd_copy_marks marks; /* copies found gone from it, recorded at the end */
    int gone;            /* it was unplugged or stopped answering: the backup stops */
    int64_t max_file;            /* the biggest copy its file system holds; -1: no limit */
    int too_big_said;
} backup_dest;

static void say_gone(backup_dest *d, int n, bd_log_fn log, void *log_ctx)
{
    d->gone = 1;
    if (n > 1) bd_logf(log, log_ctx, "\"%s\" stopped answering or was unplugged: the backup stops", d->label);
    else bd_logf(log, log_ctx, "the drive stopped answering or was unplugged: the backup stops");
}

/* Something failed on this drive (files_failed went up from before): if
 * the drive itself is gone, the backup stops there. 1 if it is gone. */
static int dest_gone(bd_catalog *cat, backup_dest *d, int64_t failed_before, int n, bd_log_fn log, void *log_ctx)
{
    if (!d->gone && d->stats->files_failed != failed_before && !bd_media_present(cat, d->media_id))
        say_gone(d, n, log, log_ctx);
    return d->gone;
}

static void dest_flush(bd_catalog *cat, backup_dest *d, int n, bd_log_fn log, void *log_ctx)
{
    if (d->ng && !d->gone && !bd_media_present(cat, d->media_id)) say_gone(d, n, log, log_ctx);
    if (d->gone) {
        /* Nothing more is named, recorded or removed on a drive that is
         * gone (another drive may be in its place). A temp file left on it
         * is written again by the next backup. */
        for (size_t i = 0; i < d->ng; i++) {
            d->stats->files_failed++;
            pending_free(&d->group[i]);
        }
    } else {
        int64_t before = d->stats->files_failed;
        size_t had = d->ng;
        sync_group(cat, d->store, d->group, d->ng, d->stats, log, log_ctx);
        dest_gone(cat, d, before, n, log, log_ctx);
        /* The copies are in the catalog now (in this batch's transaction):
         * the window can show the full copies growing. */
        if (had) bd_report_recorded(cat);
    }
    d->ng = 0;
    d->group_bytes = 0;
}

static void dest_close(backup_dest *d)
{
    for (size_t i = 0; i < d->ng; i++) pending_free(&d->group[i]);
    bd_copy_marks_free(&d->marks);
    bd_store_close(d->store);
}

bd_status bd_backup(bd_catalog *cat, int64_t media_id, int64_t source_id,
                    bd_backup_stats *stats, bd_log_fn log, void *log_ctx)
{
    return bd_backup_many(cat, &media_id, 1, source_id, stats, log, log_ctx);
}

bd_status bd_backup_many(bd_catalog *cat, const int64_t *media_ids, int n, int64_t source_id,
                         bd_backup_stats *stats, bd_log_fn log, void *log_ctx)
{
    bd_backup_stats local;
    if (n < 1 || n > MAX_DESTS) return bd_fail(cat, BD_ERR_INVALID, "back up to 1 to %d drives at once", MAX_DESTS);
    if (!stats) {
        if (n != 1) return bd_fail(cat, BD_ERR_INVALID, "give room for each drive's results");
        stats = &local;
    }
    for (int i = 0; i < n; i++) memset(&stats[i], 0, sizeof(stats[i]));
    for (int i = 0; i < n; i++)
        for (int k = 0; k < i; k++)
            if (media_ids[i] == media_ids[k]) return bd_fail(cat, BD_ERR_INVALID, "the same drive is listed twice");
    bd_cancel_reset(cat);

    backup_dest *dests = calloc((size_t)n, sizeof(*dests));
    if (!dests) return BD_ERR_NOMEM;
    bd_status s = BD_OK;
    int need_key = 0;
    for (int i = 0; i < n && s == BD_OK; i++) {
        backup_dest *d = &dests[i];
        d->media_id = media_ids[i];
        d->stats = &stats[i];
        d->space_free = -1;
        d->budget = -1;
        sqlite3_stmt *q;
        int known = 0;
        if (sqlite3_prepare_v2(cat->db,
                               "SELECT label, encrypted, secondary, COALESCE(limit_bytes,0), retired_ms IS NOT NULL,"
                               " (SELECT COALESCE(SUM(stored_size),0) FROM copies WHERE media_id=media.id AND state='ok')"
                               " FROM media WHERE id=?",
                               -1, &q, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(q, 1, d->media_id);
            if (sqlite3_step(q) == SQLITE_ROW) {
                known = 1;
                snprintf(d->label, sizeof(d->label), "%s", (const char *)sqlite3_column_text(q, 0));
                d->encrypted = sqlite3_column_int(q, 1);
                d->secondary = sqlite3_column_int(q, 2);
                int64_t limit = sqlite3_column_int64(q, 3);
                if (sqlite3_column_int(q, 4)) known = -1;
                if (d->secondary && limit > 0) {
                    d->budget = limit - sqlite3_column_int64(q, 5);
                    if (d->budget < 0) d->budget = 0;
                }
            }
            sqlite3_finalize(q);
        }
        if (known == 0) s = bd_fail(cat, BD_ERR_NOT_FOUND, "no such drive");
        else if (known < 0)
            s = n == 1 ? bd_fail(cat, BD_ERR_INVALID, "this drive is marked lost or broken, so nothing is backed up to it")
                       : bd_fail(cat, BD_ERR_INVALID, "\"%s\" is marked lost or broken, so nothing is backed up to it", d->label);
        else if (d->encrypted) need_key = 1;
    }
    if (s == BD_OK && need_key && !cat->have_key)
        s = bd_fail(cat, BD_ERR_PASSPHRASE, "enter the passphrase to back up to an encrypted drive");
    if (s != BD_OK) { free(dests); return s; }

    /* The drives this job writes to, for the query of what to copy. */
    sqlite3_stmt *ins;
    if (bd_exec(cat, "DELETE FROM temp.bk_dests") != 0 ||
        sqlite3_prepare_v2(cat->db, "INSERT INTO temp.bk_dests(media_id, secondary) VALUES(?,?)", -1, &ins, NULL) != SQLITE_OK) {
        free(dests);
        return bd_fail_db(cat, "list the drives to back up to");
    }
    for (int i = 0; i < n; i++) {
        sqlite3_reset(ins);
        sqlite3_bind_int64(ins, 1, dests[i].media_id);
        sqlite3_bind_int(ins, 2, dests[i].secondary);
        sqlite3_step(ins);
    }
    sqlite3_finalize(ins);

    /* Roots the last scan held back or that cannot be found are left out,
     * one line each. */
    int64_t left_out = roots_left_out(cat, source_id, "bk_out", NULL, NULL, log, log_ctx);
    if (left_out < 0) {
        free(dests);
        return bd_fail_db(cat, "list the backup roots left out");
    }
    for (int i = 0; i < n; i++) stats[i].roots_left_out = left_out;

    /* Folders set to be encrypted need the key even on a plain drive.
     * Without it, their files wait for it and everything else is backed
     * up. */
    if (!cat->have_key) {
        sqlite3_stmt *q;
        if (sqlite3_prepare_v2(cat->db,
                               "SELECT d.media_id, COUNT(*) FROM temp.bk_dests d, nodes n JOIN versions v ON v.id=n.current_version_id"
                               " WHERE n.is_dir=0 AND n.deleted=0 AND (?1=0 OR n.source_id=?1) AND " BD_SQL_ACTIVE("n")
                               " AND NOT EXISTS(SELECT 1 FROM temp.bk_out lo WHERE lo.source_id=n.source_id)"
                               " AND (d.secondary=0 OR " BD_SQL_MARKED("stars", "n") ")"
                               " AND NOT EXISTS(SELECT 1 FROM copies c WHERE c.version_id=v.id AND c.media_id=d.media_id AND c.state='ok')"
                               " AND " BD_SQL_MARKED("encrypt_paths", "n") " GROUP BY d.media_id",
                               -1, &q, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(q, 1, source_id);
            while (sqlite3_step(q) == SQLITE_ROW)
                for (int i = 0; i < n; i++)
                    if (dests[i].media_id == sqlite3_column_int64(q, 0)) stats[i].files_need_passphrase = sqlite3_column_int64(q, 1);
            sqlite3_finalize(q);
        }
        if (sqlite3_prepare_v2(cat->db,
                               "SELECT COUNT(*) FROM nodes n JOIN versions v ON v.id=n.current_version_id WHERE" TODO_MANY_COND
                               " AND " BD_SQL_MARKED("encrypt_paths", "n"),
                               -1, &q, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(q, 1, source_id);
            int64_t waiting = sqlite3_step(q) == SQLITE_ROW ? sqlite3_column_int64(q, 0) : 0;
            sqlite3_finalize(q);
            if (waiting == 1)
                bd_logf(log, log_ctx, "1 file in a folder set to be encrypted is not backed up: it waits for the passphrase");
            else if (waiting)
                bd_logf(log, log_ctx, "%lld files in folders set to be encrypted are not backed up: they wait for the passphrase",
                        (long long)waiting);
        }
    }

    /* A root called .versions would land in the versions tree. */
    bd_status fs = bd_sources_fix_names(cat, log, log_ctx);
    if (fs != BD_OK) { free(dests); return fs; }
    for (int i = 0; i < n; i++) {
        backup_dest *d = &dests[i];
        if (!(d->store = bd_store_open(cat, d->media_id))) {
            if (n > 1) bd_fail(cat, BD_ERR_NOT_FOUND, "\"%s\" is not plugged in", d->label);
            for (int k = 0; k < i; k++) bd_store_close(dests[k].store);
            free(dests);
            return BD_ERR_NOT_FOUND;
        }
        if (!bd_media_present(cat, d->media_id)) {
            if (n > 1)
                bd_fail(cat, BD_ERR_DRIVE_GONE, "\"%s\" is not answering or was unplugged; plug it in again", d->label);
            else
                bd_fail(cat, BD_ERR_DRIVE_GONE, "the drive is not answering or was unplugged; plug it in again");
            for (int k = 0; k <= i; k++) bd_store_close(dests[k].store);
            free(dests);
            return BD_ERR_DRIVE_GONE;
        }
        d->store->hide_names = bd_media_hides_names(cat, d->media_id, d->store->names_key);
        char *root = bd_connected_root(cat, d->media_id);
        d->max_file = bd_drive_file_limit(cat, d->media_id, root);
        free(root);
        /* How much fits, leaving the reserve free for the catalog's copies.
         * Unknown space means no limit. */
        int64_t space_total = 0;
        if (bd_test_free_bytes >= 0) d->space_free = bd_test_free_bytes;
        else if (d->store->ops->space(d->store, &space_total, &d->space_free) != 0) d->space_free = -1;
        else {
            d->reserve = bd_reserve_bytes(cat);
            bd_media_note_space(cat, d->media_id, space_total, d->space_free, "before backup");
        }
    }
    if (n > 1) bd_logf(log, log_ctx, "backing up to %d drives at once: each file is read once and written to all of them", n);

    for (int i = 0; i < n; i++) dests[i].job = bd_job_start(cat, "backup", dests[i].media_id);
    /* What to copy, in the order it is copied: starred files first, so on a
     * drive too small for everything the files marked as mattering most get
     * there; then files with no copy that counts anywhere yet, so a drive
     * that takes over where a full one stopped gets what the other lacks;
     * then smaller files before bigger ones, so the most files fit and one
     * big file does not hold up the rest. The order is fixed here,
     * so a file read again because it changed since the scan (a new
     * version) keeps its place. Without the passphrase, files in folders set
     * to be encrypted are left out (above). The loop below goes through it
     * by node id: each batch starts after the last node it got. */
    const char *plain_only = cat->have_key ? "" : " AND NOT " BD_SQL_MARKED("encrypt_paths", "n");
    char *order_sql = bd_sprintf("INSERT INTO temp.bk_order(node_id) SELECT n.id FROM nodes n"
                                 " JOIN versions v ON v.id=n.current_version_id WHERE" TODO_MANY_COND "%s"
                                 " ORDER BY " BD_SQL_MARKED("stars", "n") " DESC,"
                                 " EXISTS(SELECT 1 FROM copies oc WHERE oc.version_id=v.id AND oc.state='ok' AND " BD_SQL_COUNTS("oc") "),"
                                 " v.size, n.id",
                                 plain_only);
    char *item_sql = bd_sprintf("SELECT n.id, n.rel_path, n.size, n.mtime_ns, s.path, s.name, v.id, v.hash, v.version_no, v.size,"
                                " " BD_SQL_MARKED("stars", "n") ", " BD_SQL_MARKED("encrypt_paths", "n")
                                " FROM temp.bk_order o JOIN nodes n ON n.id=o.node_id JOIN sources s ON s.id=n.source_id"
                                " JOIN versions v ON v.id=n.current_version_id"
                                " WHERE o.seq>COALESCE((SELECT x.seq FROM temp.bk_order x WHERE x.node_id=?2),0) AND" TODO_MANY_COND
                                "%s ORDER BY o.seq LIMIT 256",
                                plain_only);
    sqlite3_stmt *q = NULL, *have = NULL, *fill = NULL;
    int ordered = order_sql && item_sql && bd_exec(cat, "DELETE FROM temp.bk_order") == 0 &&
                  sqlite3_prepare_v2(cat->db, order_sql, -1, &fill, NULL) == SQLITE_OK;
    if (ordered) {
        sqlite3_bind_int64(fill, 1, source_id);
        ordered = sqlite3_step(fill) == SQLITE_DONE;
    }
    sqlite3_finalize(fill);
    free(order_sql);
    if (!ordered || sqlite3_prepare_v2(cat->db, item_sql, -1, &q, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(cat->db, "SELECT 1 FROM copies WHERE version_id=? AND media_id=? AND state='ok'", -1, &have, NULL) != SQLITE_OK) {
        free(item_sql);
        bd_status r = bd_fail_db(cat, "list files to back up");
        for (int i = 0; i < n; i++) { bd_job_finish(cat, dests[i].job, "failed", 0, 0, 0); dest_close(&dests[i]); }
        sqlite3_finalize(q);
        sqlite3_finalize(have);
        bd_exec(cat, "DELETE FROM temp.bk_order");
        free(dests);
        return r;
    }
    free(item_sql);

    /* Totals for the progress bar: each file is read once, whatever the
     * number of drives. */
    sqlite3_stmt *t;
    int64_t todo_files = 0, todo_bytes = 0;
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT COUNT(*), COALESCE(SUM(v.size),0) FROM temp.bk_order o JOIN nodes n ON n.id=o.node_id"
                           " JOIN versions v ON v.id=n.current_version_id",
                           -1, &t, NULL) == SQLITE_OK) {
        if (sqlite3_step(t) == SQLITE_ROW) {
            todo_files = sqlite3_column_int64(t, 0);
            todo_bytes = sqlite3_column_int64(t, 1);
        }
        sqlite3_finalize(t);
    }
    bd_progress_begin(cat, "backup", todo_files, todo_bytes);

    backup_item items[BATCH];
    int64_t last_id = 0, items_done = 0, rehashed_total = 0, last_save_ms = bd_now_ms();
    int cancelled = 0, gone = 0;
    int full_said[MAX_DESTS] = {0}, limit_said[MAX_DESTS] = {0};
    int64_t biggest = bd_biggest_drive(cat); /* a file over it fits no drive you have */
    while (!cancelled && !gone) {
        size_t ni = 0;
        sqlite3_reset(q);
        sqlite3_bind_int64(q, 1, source_id);
        sqlite3_bind_int64(q, 2, last_id);
        while (sqlite3_step(q) == SQLITE_ROW) {
            last_id = sqlite3_column_int64(q, 0);
            backup_item *it = &items[ni++];
            memset(it, 0, sizeof(*it));
            it->node_id = last_id;
            it->rel = bd_strdup((const char *)sqlite3_column_text(q, 1));
            it->node_size = sqlite3_column_int64(q, 2);
            it->node_mtime = sqlite3_column_int64(q, 3);
            it->source_path = bd_strdup((const char *)sqlite3_column_text(q, 4));
            it->source_name = bd_strdup((const char *)sqlite3_column_text(q, 5));
            it->version_id = sqlite3_column_int64(q, 6);
            it->hash = bd_strdup((const char *)sqlite3_column_text(q, 7));
            it->version_no = sqlite3_column_int64(q, 8);
            it->version_size = sqlite3_column_int64(q, 9);
            it->starred = sqlite3_column_int(q, 10);
            it->encrypt = sqlite3_column_int(q, 11);
        }
        sqlite3_reset(q);
        if (ni == 0) break;
        /* One catalog transaction per batch; each drive's copies are synced
         * in groups (at most SYNC_BYTES of data) before they are renamed and
         * recorded. */
        int in_txn = bd_exec(cat, "BEGIN") == 0;
        for (size_t i = 0; i < ni; i++) {
            /* Stopping: what is written so far is still finished below. */
            if (bd_cancelled(cat)) { cancelled = 1; break; }
            backup_item *it = &items[i];
            items_done++;
            if (!it->rel || !it->source_path || !it->source_name || !it->hash) {
                for (int k = 0; k < n; k++) stats[k].files_failed++;
                continue;
            }
            /* Paths from a recovered catalog may hold anything: nothing is
             * read or written outside the root and its folder on the drive. */
            if (!bd_stored_path_safe(it->rel) || !bd_stored_name_safe(it->source_name)) {
                for (int k = 0; k < n; k++) stats[k].files_failed++;
                bd_logf(log, log_ctx, "not backed up: the path stored for \"%s/%s\" could lead outside its folder", it->source_name,
                        it->rel);
                continue;
            }
            /* Which drives still need it. */
            int want[MAX_DESTS], nw = 0;
            for (int k = 0; k < n; k++) {
                want[k] = !dests[k].secondary || it->starred;
                if (want[k] && n > 1) {
                    sqlite3_reset(have);
                    sqlite3_bind_int64(have, 1, it->version_id);
                    sqlite3_bind_int64(have, 2, dests[k].media_id);
                    if (sqlite3_step(have) == SQLITE_ROW) want[k] = 0;
                    sqlite3_reset(have);
                }
                if (want[k]) { nw++; stats[k].files_considered++; }
            }
            if (!nw) continue;
            int rehashed = 0;
            int prep = item_prepare(cat, it, &rehashed, log, log_ctx);
            if (prep != PREP_OK) {
                for (int k = 0; k < n; k++) {
                    if (!want[k]) continue;
                    if (prep == PREP_ONLINE) stats[k].files_online_only++;
                    else stats[k].files_failed++;
                    if (prep == PREP_IN_USE) stats[k].files_in_use++;
                }
                continue;
            }
            if (rehashed) {
                if (++rehashed_total == 1)
                    bd_logf(log, log_ctx, "files changed since the scan are read again and backed up as they are now");
                for (int k = 0; k < n; k++) {
                    if (!want[k]) continue;
                    stats[k].files_rehashed++;
                    /* As it is now, it may be on this drive already. */
                    if (has_good_copy(cat, it->version_id, dests[k].media_id)) {
                        stats[k].files_already_there++;
                        want[k] = 0;
                    }
                }
            }
            bd_report_file(cat, items_done - 1, it->rel, it->version_size);

            /* Room on each drive, then where the copy goes there. */
            FILE *outs[MAX_DESTS];
            const uint8_t *keys[MAX_DESTS];
            pending_copy *ps[MAX_DESTS];
            int which[MAX_DESTS], nr = 0;
            for (int k = 0; k < n; k++) {
                if (!want[k]) continue;
                backup_dest *d = &dests[k];
                const uint8_t *key = (d->encrypted || it->encrypt) ? cat->key : NULL;
                if (adopt_alongside(cat, d->store, key, it, &stats[k])) continue;
                /* Sealed copies are a little bigger than the file. */
                int64_t need = it->version_size + (key ? it->version_size / 1024 + 4096 : 0);
                if (d->max_file >= 0 && need > d->max_file) {
                    /* Planned out: never read, never half written. */
                    if (!d->too_big_said++) {
                        if (n > 1) bd_logf(log, log_ctx, "\"%s\": " BD_WHY_FAT32 ". Such files wait for another drive", d->label);
                        else bd_logf(log, log_ctx, BD_WHY_FAT32 ". Such files wait for another drive");
                    }
                    stats[k].files_no_room++;
                    stats[k].bytes_no_room += it->version_size;
                    stats[k].files_too_big++;
                    stats[k].bytes_too_big += it->version_size;
                    stats[k].too_big_why = BD_WHY_FAT32;
                    continue;
                }
                if (d->space_free >= 0 && need > d->space_free - d->reserve) {
                    if (biggest > 0 && need > biggest) {
                        /* No drive you have could take it. */
                        if (stats[k].files_bigger_than_drives < 20)
                            bd_logf(log, log_ctx, "%s is bigger than every backup drive you have: leave it out, or back it up to a bigger drive", it->rel);
                        stats[k].files_bigger_than_drives++;
                        stats[k].bytes_bigger_than_drives += it->version_size;
                    } else if (!full_said[k]++) {
                        if (n > 1) bd_logf(log, log_ctx, "\"%s\" is full; files that do not fit wait for another drive", d->label);
                        else bd_logf(log, log_ctx, "the drive is full; files that do not fit wait for another drive");
                    }
                    stats[k].files_no_room++;
                    stats[k].bytes_no_room += it->version_size;
                    continue;
                }
                if (d->budget >= 0 && need > d->budget) {
                    if (!limit_said[k]++)
                        bd_logf(log, log_ctx, "\"%s\" has reached the size you set for it; starred files over it are not copied there",
                                d->label);
                    stats[k].files_no_room++;
                    stats[k].bytes_no_room += it->version_size;
                    continue;
                }
                pending_copy *p = &d->group[d->ng];
                int64_t failed_before = stats[k].files_failed;
                if (!dest_prepare(cat, d->store, key, it, p, &d->marks, &stats[k], log, log_ctx)) {
                    if (dest_gone(cat, d, failed_before, n, log, log_ctx)) gone = 1;
                    continue;
                }
                outs[nr] = p->out;
                keys[nr] = key;
                ps[nr] = p;
                which[nr] = k;
                nr++;
            }
            if (!nr) continue;
            char hash[BD_HASH_HEX_LEN + 1] = "";
            int64_t sizes[MAX_DESTS];
            int failed[MAX_DESTS];
            int rc = bd_hash_copy_multi(it->read_from ? it->read_from : it->orig, outs, keys, nr, hash, sizes, failed);
            if (rc == -2) rc = 0; /* every copy failed: failed[] says so */
            int in_use_noted = 0;
            for (int j = 0; j < nr; j++) {
                backup_dest *d = &dests[which[j]];
                int64_t failed_before = d->stats->files_failed;
                int r = dest_written(d->store, ps[j], rc, failed[j], sizes[j], hash, d->stats, &in_use_noted, cat, log, log_ctx);
                if (r == 2) cancelled = 1;
                if (r == 0 && dest_gone(cat, d, failed_before, n, log, log_ctx)) gone = 1;
                if (r != 1) continue;
                pending_copy *p = ps[j];
                if (d->space_free >= 0) d->space_free -= p->bytes;
                if (d->budget >= 0) d->budget -= p->bytes + (p->sealed ? p->bytes / 1024 + 4096 : 0);
                d->group_bytes += p->bytes;
                d->ng++;
                if (d->group_bytes >= SYNC_BYTES || d->ng == BATCH) {
                    dest_flush(cat, d, n, log, log_ctx);
                    if (d->gone) gone = 1;
                }
            }
            if (cancelled || gone) break;
        }
        /* A stop during the batch's last copy is still a stop. */
        if (!cancelled && bd_cancelled(cat)) cancelled = 1;
        for (int k = 0; k < n; k++) {
            dest_flush(cat, &dests[k], n, log, log_ctx);
            if (dests[k].gone) gone = 1;
        }
        if (in_txn && bd_exec(cat, "COMMIT") != 0) {
            bd_logf(log, log_ctx, "cannot record the copies: %s", sqlite3_errmsg(cat->db));
            bd_exec(cat, "ROLLBACK");
        }
        free_items(items, ni);
        /* Every few minutes, between batches, when every copy made so far
         * is synced, in its place and recorded: save the catalog, so a
         * long backup that is cut short (a crash, a power cut) keeps what
         * it copied. The catalog's copy on the drive is still made at the
         * end only, so a drive does not fill with revisions. */
        int64_t every = bd_test_save_every_ms > 0 ? bd_test_save_every_ms : SAVE_EVERY_MS;
        if (!cancelled && bd_now_ms() - last_save_ms >= every) {
            int64_t copied = 0;
            for (int k = 0; k < n; k++) {
                note_job_progress(cat, dests[k].job, &stats[k]);
                copied += stats[k].files_copied;
            }
            if (bd_catalog_save(cat) == BD_OK)
                bd_logf(log, log_ctx, "catalog saved; %lld files copied so far", (long long)copied);
            else
                bd_logf(log, log_ctx, "the catalog could not be saved during the backup (it is saved at the end): %s",
                        bd_catalog_error(cat));
            last_save_ms = bd_now_ms();
        }
    }
    sqlite3_finalize(q);
    sqlite3_finalize(have);

    /* Older copies found gone, on the drives still there. */
    for (int k = 0; k < n; k++) {
        backup_dest *d = &dests[k];
        if (d->gone || !d->marks.n) continue;
        int txn = bd_exec(cat, "BEGIN") == 0;
        int64_t dropped = bd_copy_marks_apply(cat, &d->marks);
        if (dropped < 0) bd_logf(log, log_ctx, "cannot record the copies found gone: %s", sqlite3_errmsg(cat->db));
        if (txn) bd_exec(cat, dropped < 0 ? "ROLLBACK" : "COMMIT");
        if (dropped > 0) {
            say_gone(d, n, log, log_ctx);
            gone = 1;
        }
    }

    for (int k = 0; k < n; k++) {
        backup_dest *d = &dests[k];
        int64_t total_bytes = 0, free_bytes = 0;
        if (!d->gone && d->store->ops->space(d->store, &total_bytes, &free_bytes) == 0)
            bd_media_note_space(cat, d->media_id, total_bytes, free_bytes, "after backup");
        dest_close(d);
    }
    bd_report(cat, n == 1 ? stats[0].files_copied : items_done, NULL, 1);
    bd_progress_end(cat);
    for (int k = 0; k < n; k++)
        bd_job_finish(cat, dests[k].job,
                      dests[k].gone ? "failed" : cancelled || gone ? "stopped" : stats[k].files_failed ? "partial" : "done",
                      stats[k].files_copied, stats[k].files_failed, stats[k].bytes_copied);
    bd_exec(cat, "DELETE FROM temp.bk_dests; DELETE FROM temp.bk_order");
    if (gone) {
        const char *label = "";
        for (int k = 0; k < n; k++)
            if (dests[k].gone) { label = dests[k].label; break; }
        bd_status r = n > 1 ? bd_fail(cat, BD_ERR_DRIVE_GONE,
                                      "\"%s\" stopped answering or was unplugged during the backup. The copies finished"
                                      " before are kept and nothing on it was marked missing; plug it in again and back up again",
                                      label)
                            : bd_fail(cat, BD_ERR_DRIVE_GONE,
                                      "the drive stopped answering or was unplugged during the backup. The copies finished"
                                      " before are kept and nothing on it was marked missing; plug it in again and back up again");
        free(dests);
        return r;
    }
    if (cancelled) { free(dests); return bd_fail_cancelled(cat); }
    /* Each drive gets the catalog as it is now, under a new name: what it
     * holds can be found from the drive alone. */
    for (int k = 0; k < n; k++)
        if (bd_catalog_copy_to_media(cat, dests[k].media_id) != BD_OK) {
            if (n > 1) bd_logf(log, log_ctx, "the catalog could not be copied to \"%s\": %s", dests[k].label, bd_catalog_error(cat));
            else bd_logf(log, log_ctx, "the catalog could not be copied to the drive: %s", bd_catalog_error(cat));
        }
    free(dests);
    return BD_OK;
}

/* ---- Before a backup: what it would do ---------------------------------- */

#define MASS_CHANGE_MIN_FILES 50

/* Files this drive holds (a good copy of some version of it), and whether
 * the newest version it holds is still the file's contents: the checksum is
 * the test, not size or time. A held file that is gone from its folder
 * counts as changed until the user has seen it in the deleted list
 * (deleted_ack): encrypting files to new names and deleting the originals
 * leaves the drive's files deleted, not changed. A moved file keeps its
 * copies (the new node takes the old one's versions), so moves do not
 * count. */
#define HELD_CTE                                                                                        \
    "WITH h AS (SELECT v.node_id AS nid, MAX(v.version_no) AS vno FROM copies c"                        \
    " JOIN versions v ON v.id=c.version_id WHERE (?1=0 OR c.media_id=?1) AND c.state='ok' GROUP BY v.node_id)"

bd_status bd_backup_preview(bd_catalog *cat, int64_t media_id, int64_t source_id, bd_backup_preview_info *out)
{
    memset(out, 0, sizeof(*out));
    out->free_bytes = -1;
    sqlite3_stmt *q;
    int64_t left_out = roots_left_out(cat, source_id, "pv_out", NULL, NULL, NULL, NULL);
    if (left_out < 0) return bd_fail_db(cat, "list the backup roots left out");
    out->roots_left_out = left_out;
    char *root = bd_connected_root(cat, media_id);
    int64_t max_file = bd_drive_file_limit(cat, media_id, root);
    int drive_sealed = bd_media_encrypted(cat, media_id);

    /* What it copies, and of that what is changed: an older version of the
     * file is on the drive, so the new one goes alongside it. And what is
     * too big for the drive's file system (?4, -1 no limit), sealed copies
     * (?5: every copy on this drive) with their few extra bytes, as the
     * backup counts them. */
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT COUNT(*), COALESCE(SUM(t.size),0), COALESCE(SUM(t.older),0),"
                           " COALESCE(SUM(t.enc),0), COALESCE(SUM(t.enc*t.size),0),"
                           " COALESCE(SUM(t.big),0), COALESCE(SUM(t.big*t.size),0),"
                           " COALESCE(SUM(t.big*t.enc),0), COALESCE(SUM(t.big*t.enc*t.size),0) FROM (SELECT size, older, enc,"
                           "  ?4>=0 AND size + CASE WHEN ?5<>0 OR enc THEN size/1024 + 4096 ELSE 0 END > ?4 AS big FROM ("
                           " SELECT v.size AS size,"
                           " EXISTS(SELECT 1 FROM copies oc JOIN versions ov ON ov.id=oc.version_id"
                           "   WHERE ov.node_id=n.id AND oc.media_id=?1) AS older,"
                           " " BD_SQL_MARKED("encrypt_paths", "n") " AS enc"
                           " FROM nodes n JOIN versions v ON v.id=n.current_version_id WHERE" TODO_WHERE ")) t",
                           -1, &q, NULL) != SQLITE_OK) {
        free(root);
        return bd_fail_db(cat, "work out what the backup would copy");
    }
    sqlite3_bind_int64(q, 1, media_id);
    sqlite3_bind_int64(q, 2, source_id);
    sqlite3_bind_int64(q, 3, 0);
    sqlite3_bind_int64(q, 4, max_file);
    sqlite3_bind_int(q, 5, drive_sealed);
    int64_t sealed_files = 0, sealed_bytes = 0, big_sealed_files = 0, big_sealed_bytes = 0;
    if (sqlite3_step(q) == SQLITE_ROW) {
        out->files_to_copy = sqlite3_column_int64(q, 0);
        out->bytes_to_copy = sqlite3_column_int64(q, 1);
        out->files_changed = sqlite3_column_int64(q, 2);
        out->files_new = out->files_to_copy - out->files_changed;
        sealed_files = sqlite3_column_int64(q, 3);
        sealed_bytes = sqlite3_column_int64(q, 4);
        out->files_too_big = sqlite3_column_int64(q, 5);
        out->bytes_too_big = sqlite3_column_int64(q, 6);
        big_sealed_files = sqlite3_column_int64(q, 7);
        big_sealed_bytes = sqlite3_column_int64(q, 8);
    }
    sqlite3_finalize(q);
    if (out->files_too_big) out->too_big_why = BD_WHY_FAT32;
    if (!cat->have_key) out->files_need_passphrase = drive_sealed ? out->files_to_copy : sealed_files;

    /* How much of the drive held, and how much of that changed since. A
     * large share at once is what ransomware encrypting the folders looks
     * like: the user should see the list before backing that up. */
    /* A new or nearly empty drive holds too few to tell: then the same is
     * worked out over every drive, so a first backup to a new drive does
     * not copy encrypted files without a word. */
    if (sqlite3_prepare_v2(cat->db,
                           HELD_CTE " SELECT COUNT(*), COALESCE(SUM(n.deleted<>0 OR hv.id IS NOT n.current_version_id),0), COALESCE(SUM(n.deleted<>0),0)"
                           " FROM h JOIN nodes n ON n.id=h.nid JOIN versions hv ON hv.node_id=h.nid AND hv.version_no=h.vno"
                           " WHERE n.is_dir=0 AND (n.deleted=0 OR n.deleted_ack=0) AND (?2=0 OR n.source_id=?2)"
                           " AND " BD_SQL_ACTIVE("n"),
                           -1, &q, NULL) == SQLITE_OK) {
        for (int everywhere = 0; everywhere < 2; everywhere++) {
            sqlite3_reset(q);
            sqlite3_bind_int64(q, 1, everywhere ? 0 : media_id);
            sqlite3_bind_int64(q, 2, source_id);
            if (sqlite3_step(q) == SQLITE_ROW && (!everywhere || sqlite3_column_int64(q, 0) >= MASS_CHANGE_MIN_FILES)) {
                out->files_held = sqlite3_column_int64(q, 0);
                out->held_changed = sqlite3_column_int64(q, 1);
                out->held_deleted = sqlite3_column_int64(q, 2);
                out->held_everywhere = everywhere;
            }
            if (out->files_held >= MASS_CHANGE_MIN_FILES || !media_id) break;
        }
        sqlite3_finalize(q);
    }
    out->mass_change = out->files_held >= MASS_CHANGE_MIN_FILES && out->held_changed * 4 >= out->files_held;

    if (sqlite3_prepare_v2(cat->db, "SELECT MIN(COALESCE(last_scan_ms,0)) FROM sources WHERE (?1=0 OR id=?1) AND active<>0"
                                " AND id NOT IN (SELECT source_id FROM temp.pv_out)", -1, &q, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(q, 1, source_id);
        if (sqlite3_step(q) == SQLITE_ROW) out->last_scan_ms = sqlite3_column_int64(q, 0);
        sqlite3_finalize(q);
    }

    /* Room: read now when plugged in, else what was last seen. */
    int64_t total = 0, free_bytes = -1;
    bd_store *store = root ? bd_store_open(cat, media_id) : NULL;
    if (bd_test_free_bytes >= 0 && store) {
        free_bytes = bd_test_free_bytes;
        out->free_is_current = 1;
    } else if (store && store->ops->space(store, &total, &free_bytes) == 0) {
        out->free_is_current = 1;
    } else {
        free_bytes = -1;
        if (sqlite3_prepare_v2(cat->db, "SELECT total_bytes, free_bytes FROM media WHERE id=?", -1, &q, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(q, 1, media_id);
            if (sqlite3_step(q) == SQLITE_ROW && sqlite3_column_type(q, 1) != SQLITE_NULL) {
                total = sqlite3_column_int64(q, 0);
                free_bytes = sqlite3_column_int64(q, 1);
            }
            sqlite3_finalize(q);
        }
    }
    bd_store_close(store);
    free(root);
    out->free_bytes = free_bytes;
    out->reserve_bytes = bd_test_free_bytes >= 0 ? 0 : bd_reserve_bytes(cat);
    /* Files too big for the file system never fit, whatever the room; the
     * rest is weighed against the room. */
    out->bytes_short = out->bytes_too_big;
    if (free_bytes >= 0) {
        int64_t need = out->bytes_to_copy - out->bytes_too_big;
        /* Sealed copies are a little bigger than the file. */
        if (drive_sealed) need += need / 1024 + (out->files_to_copy - out->files_too_big) * 4096;
        else need += (sealed_bytes - big_sealed_bytes) / 1024 + (sealed_files - big_sealed_files) * 4096;
        int64_t room = free_bytes - out->reserve_bytes;
        out->bytes_short += need > room ? need - room : 0;
    }
    return BD_OK;
}

bd_status bd_list_changed_on_media(bd_catalog *cat, int64_t media_id, bd_changed_fn fn, void *ctx)
{
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(cat->db,
                           HELD_CTE " SELECT n.id, s.name, n.rel_path, hv.version_no, COALESCE(cv.version_no, hv.version_no),"
                           " hv.size, COALESCE(cv.size, hv.size), COALESCE(cv.first_seen_ms, 0), n.deleted<>0, hv.id"
                           " FROM h JOIN nodes n ON n.id=h.nid JOIN sources s ON s.id=n.source_id"
                           " JOIN versions hv ON hv.node_id=h.nid AND hv.version_no=h.vno"
                           " LEFT JOIN versions cv ON cv.id=n.current_version_id"
                           " WHERE n.is_dir=0 AND ((n.deleted=0 AND hv.id IS NOT cv.id) OR (n.deleted<>0 AND n.deleted_ack=0))"
                           " AND " BD_SQL_ACTIVE("n")
                           " ORDER BY s.name, n.rel_path",
                           -1, &q, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "list changed files");
    sqlite3_bind_int64(q, 1, media_id);
    while (sqlite3_step(q) == SQLITE_ROW) {
        bd_changed_info c;
        memset(&c, 0, sizeof(c));
        c.node_id = sqlite3_column_int64(q, 0);
        c.source_name = (const char *)sqlite3_column_text(q, 1);
        c.rel_path = (const char *)sqlite3_column_text(q, 2);
        c.version_on_drive = sqlite3_column_int64(q, 3);
        c.version_now = sqlite3_column_int64(q, 4);
        c.size_on_drive = sqlite3_column_int64(q, 5);
        c.size_now = sqlite3_column_int64(q, 6);
        c.changed_ms = sqlite3_column_int64(q, 7);
        c.deleted = sqlite3_column_int(q, 8);
        c.version_id_on_drive = sqlite3_column_int64(q, 9);
        if (fn(ctx, &c) != 0) break;
    }
    sqlite3_finalize(q);
    return BD_OK;
}

/* ---- The Next backup list ------------------------------------------------ */

#define PENDING_SELECT                                                                                  \
    "SELECT n.id AS id, n.source_id AS sid, s.name AS sname, n.rel_path AS rel, v.size AS size, n.mtime_ns AS mt," \
    " (SELECT " BD_SQL_COPIES("c") " FROM copies c WHERE c.version_id=v.id AND c.state='ok' AND " BD_SQL_COUNTS("c") ") AS cp," \
    " (SELECT COUNT(DISTINCT " BD_SQL_PLACE("m") ") FROM copies c JOIN media m ON m.id=c.media_id"      \
    "   WHERE c.version_id=v.id AND c.state='ok' AND " BD_SQL_COUNTS("c") ") AS pl,"                     \
    " NOT EXISTS(SELECT 1 FROM copies oc JOIN versions ov ON ov.id=oc.version_id"                       \
    "   WHERE ov.node_id=n.id AND oc.media_id=?1) AS is_new,"                                           \
    " " BD_SQL_MARKED("stars", "n") " AS st, " BD_SQL_MARKED("encrypt_paths", "n") " AS en"             \
    " FROM nodes n JOIN sources s ON s.id=n.source_id JOIN versions v ON v.id=n.current_version_id"

bd_status bd_list_pending(bd_catalog *cat, int64_t media_id, int limit, bd_pending_fn fn, void *ctx, int64_t *total)
{
    if (limit <= 0) limit = 5000;
    bd_target target;
    if (bd_target_get(cat, &target) != BD_OK) return BD_ERR_DB;
    int64_t max_file = -1;
    int drive_sealed = 0;
    if (media_id > 0) {
        /* What a backup to it copies leaves out the roots a backup leaves out. */
        if (roots_left_out(cat, 0, "pv_out", NULL, NULL, NULL, NULL) < 0) return bd_fail_db(cat, "list the backup roots left out");
        char *root = bd_connected_root(cat, media_id);
        max_file = bd_drive_file_limit(cat, media_id, root);
        free(root);
        drive_sealed = bd_media_encrypted(cat, media_id);
    }
    const char *inner = media_id > 0 ? PENDING_SELECT " WHERE" TODO_WHERE
                                     : PENDING_SELECT " WHERE n.is_dir=0 AND n.deleted=0 AND " BD_SQL_ACTIVE("n");
    const char *outer_where = " WHERE (?1<>0 OR cp<?4 OR pl<?5)";
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 0 && !total) continue;
        char *sql = pass == 0 ? bd_sprintf("SELECT COUNT(*) FROM (%s)%s", inner, outer_where)
                              : bd_sprintf("SELECT * FROM (%s)%s ORDER BY sname COLLATE NOCASE, rel COLLATE NOCASE LIMIT ?6", inner,
                                           outer_where);
        if (!sql) return BD_ERR_NOMEM;
        sqlite3_stmt *q;
        int rc = sqlite3_prepare_v2(cat->db, sql, -1, &q, NULL);
        free(sql);
        if (rc != SQLITE_OK) return bd_fail_db(cat, "list the files the next backup copies");
        sqlite3_bind_int64(q, 1, media_id > 0 ? media_id : 0);
        sqlite3_bind_int64(q, 2, 0);
        sqlite3_bind_int64(q, 3, 0);
        sqlite3_bind_int(q, 4, target.copies);
        sqlite3_bind_int(q, 5, target.places);
        sqlite3_bind_int(q, 6, limit);
        if (pass == 0) {
            *total = sqlite3_step(q) == SQLITE_ROW ? sqlite3_column_int64(q, 0) : 0;
            sqlite3_finalize(q);
            continue;
        }
        while (fn && sqlite3_step(q) == SQLITE_ROW) {
            bd_pending_info p;
            p.node_id = sqlite3_column_int64(q, 0);
            p.source_id = sqlite3_column_int64(q, 1);
            p.source_name = (const char *)sqlite3_column_text(q, 2);
            p.rel_path = (const char *)sqlite3_column_text(q, 3);
            p.size = sqlite3_column_int64(q, 4);
            p.mtime_ns = sqlite3_column_int64(q, 5);
            p.copies = sqlite3_column_int(q, 6);
            p.places = sqlite3_column_int(q, 7);
            p.is_new = media_id > 0 ? sqlite3_column_int(q, 8) : 0;
            p.starred = sqlite3_column_int(q, 9);
            p.encrypt = sqlite3_column_int(q, 10);
            p.backed_up = p.copies >= target.copies && p.places >= target.places;
            p.too_big = max_file >= 0 && p.size + (drive_sealed || p.encrypt ? p.size / 1024 + 4096 : 0) > max_file;
            if (fn(ctx, &p)) break;
        }
        sqlite3_finalize(q);
    }
    return BD_OK;
}

bd_status bd_restore(bd_catalog *cat, int64_t source_id, const char *rel_prefix, const char *dest_root,
                     bd_restore_stats *stats, bd_log_fn log, void *log_ctx)
{
    return bd_restore_ex(cat, source_id, rel_prefix, dest_root, NULL, stats, log, log_ctx);
}

/* The files a restore brings back (?4, restore_mode): 0 the latest
 * backed-up state of every file still in its folder; 1 only the files
 * deleted from it, each as it was when last seen; 2 both. */
#define RESTORE_WHERE                                                                                       \
    " (?4=2 OR (CASE WHEN ?4<>0 THEN n.deleted<>0 ELSE n.deleted=0 END))"                                   \
    " AND (?1=0 OR n.source_id=?1) AND (?2='' OR n.rel_path=?2 OR substr(n.rel_path,1,length(?2)+1)=?2||'/')"

static int restore_mode(const bd_restore_opts *opts)
{
    return !opts ? 0 : opts->deleted_only ? 1 : opts->with_deleted ? 2 : 0;
}

/* SQL: the version of node n a restore brings back (BD_SQL_RESTORE_VERSION,
 * which search shows the drives of too). */
#define RESTORE_VERSION BD_SQL_RESTORE_VERSION("n")

/* Is there a good copy of the version (?1), or of any version of the file
 * (?2), anywhere: then it is on drives not plugged in. */
#define ANY_COPY_SQL                                                                                    \
    "SELECT 1 FROM copies c JOIN versions v ON v.id=c.version_id WHERE c.state='ok' AND (c.version_id=?1 OR v.node_id=?2)" \
    " LIMIT 1"

/* The drives not plugged in, and not marked lost or broken, that hold a
 * good copy of the version (?1): a file whose copies here cannot be read
 * back right waits for one of them. */
#define ELSEWHERE_SQL                                                                                   \
    "SELECT DISTINCT m.label FROM copies c JOIN media m ON m.id=c.media_id WHERE c.version_id=?1"       \
    " AND c.state='ok' AND m.retired_ms IS NULL"                                                        \
    " AND NOT EXISTS(SELECT 1 FROM temp.connected k WHERE k.media_id=c.media_id) ORDER BY m.label LIMIT 3"

/* Kept in the catalog, in tables of their own that older versions leave
 * be: the files a restore found already where they go (compared by
 * checksum, or written and read back), with the size and modified time
 * each had then, for one place (the settings key restore_checked_for:
 * whether files go straight into the folder, and the folder); and the
 * restore that waits for more drives or the passphrase. */
#define RESTORE_TABLES                                                                                  \
    "CREATE TABLE IF NOT EXISTS restore_checked(node_id INTEGER PRIMARY KEY, version_id INTEGER NOT NULL," \
    " size INTEGER NOT NULL, mtime_ns INTEGER NOT NULL);"                                               \
    "CREATE TABLE IF NOT EXISTS restore_pending(id INTEGER PRIMARY KEY CHECK(id=1), source_id INTEGER NOT NULL," \
    " rel_path TEXT NOT NULL, dest TEXT NOT NULL, opts INTEGER NOT NULL, offer_root INTEGER NOT NULL,"  \
    " saved_ms INTEGER NOT NULL)"
#define CHECKED_FOR_SQL "SELECT 1 FROM settings WHERE key='restore_checked_for' AND value=?"
#define CHECKED_SQL "SELECT 1 FROM restore_checked WHERE node_id=?1 AND version_id=?2 AND size=?3 AND mtime_ns=?4"

/* One backup root (chosen, or the only one): its files go straight into
 * the folder chosen. Several: each into a folder named after its root. */
static int restore_flat(bd_catalog *cat, int64_t source_id)
{
    if (source_id) return 1;
    sqlite3_stmt *q;
    int n = 0;
    if (sqlite3_prepare_v2(cat->db, "SELECT COUNT(*) FROM sources", -1, &q, NULL) == SQLITE_OK) {
        if (sqlite3_step(q) == SQLITE_ROW) n = sqlite3_column_int(q, 0);
        sqlite3_finalize(q);
    }
    return n <= 1;
}

static char *restore_dest(const char *dest_root, int flat, const char *source_name, const char *rel)
{
    if (flat) return bd_path_join(dest_root, rel);
    char *rel_out = bd_sprintf("%s/%s", source_name, rel);
    char *dest = rel_out ? bd_path_join(dest_root, rel_out) : NULL;
    free(rel_out);
    return dest;
}

static void note_conflict(bd_catalog *cat, int64_t version_id, const char *source_name, const char *rel, const char *dest,
                          int64_t want_size, int64_t want_mtime, const bd_stat_t *have)
{
    sqlite3_stmt *u;
    if (sqlite3_prepare_v2(cat->db,
                           "INSERT INTO temp.restore_conflicts(source_name, rel_path, dest, want_size, want_mtime_ns, have_size, have_mtime_ns,"
                           " version_id) VALUES(?,?,?,?,?,?,?,?)", -1, &u, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int64(u, 8, version_id);
    sqlite3_bind_text(u, 1, source_name ? source_name : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(u, 2, rel ? rel : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(u, 3, dest, -1, SQLITE_STATIC);
    sqlite3_bind_int64(u, 4, want_size);
    sqlite3_bind_int64(u, 5, want_mtime);
    sqlite3_bind_int64(u, 6, have ? have->size : -1);
    sqlite3_bind_int64(u, 7, have ? have->mtime_ns : 0);
    sqlite3_step(u);
    sqlite3_finalize(u);
}

/* "1|<dest_root>": the place restore_checked's rows are for. */
static char *checked_place(int flat, const char *dest_root)
{
    return bd_sprintf("%d|%s", flat, dest_root ? dest_root : "");
}

typedef struct {
    int64_t version_id, size, mtime, btime;
    const char *hash, *source_name, *rel;
    int64_t node_id; /* 0: only this version counts in telling offline from never backed up */
} restore_file_item;

typedef struct {
    int64_t node_id, version_id, size, mtime_ns;
} restore_checked_row;

/* What one restore job keeps between files. */
typedef struct {
    bd_store *store;           /* the drive read last */
    sqlite3_stmt *find, *any;  /* FIND_COPY_SQL; is there a good copy anywhere */
    sqlite3_stmt *elsewhere;   /* ELSEWHERE_SQL */
    sqlite3_stmt *checked;     /* CHECKED_SQL; NULL: not looked at (one version, or the table cannot be made) */
    restore_checked_row *rows; /* found there or written, to note in restore_checked */
    size_t nrows, caprows;
    bd_copy_marks marks;       /* copies found missing or damaged, recorded at the end */
    char gone[128];            /* the drive that went, if one did */
    char *dest_root;           /* where the files go */
    int keep_both;             /* bd_restore_opts.keep_both */
    char day[16];              /* the day the restore started, for a name beside another file */
    int64_t max_file;          /* the biggest file the file system there holds, or -1 */
    int stop_errno;            /* the restore stopped, nothing can be written there: why */
} restore_run;

/* Is the file at dest (st) known to be the version restored: an earlier
 * run of this restore compared it by checksum or wrote it, and its size
 * and modified time are the same since. */
static int checked_has(restore_run *r, const restore_file_item *it, const bd_stat_t *st)
{
    if (!r->checked || !it->node_id) return 0;
    sqlite3_reset(r->checked);
    sqlite3_bind_int64(r->checked, 1, it->node_id);
    sqlite3_bind_int64(r->checked, 2, it->version_id);
    sqlite3_bind_int64(r->checked, 3, st->size);
    sqlite3_bind_int64(r->checked, 4, st->mtime_ns);
    int has = sqlite3_step(r->checked) == SQLITE_ROW;
    sqlite3_reset(r->checked);
    return has;
}

/* Note that dest holds the version now, with the size and modified time
 * it has; written by checked_flush. */
static void note_checked(restore_run *r, const restore_file_item *it, const char *dest)
{
    bd_stat_t st;
    if (!r->checked || !it->node_id || bd_stat(dest, &st) != 0 || !st.is_file) return;
    if (r->nrows == r->caprows) {
        size_t n = r->caprows ? r->caprows * 2 : 256;
        restore_checked_row *v = realloc(r->rows, n * sizeof(*v));
        if (!v) return; /* only not noted: compared again next time */
        r->rows = v;
        r->caprows = n;
    }
    restore_checked_row *w = &r->rows[r->nrows++];
    w->node_id = it->node_id;
    w->version_id = it->version_id;
    w->size = st.size;
    w->mtime_ns = st.mtime_ns;
}

static void checked_flush(bd_catalog *cat, restore_run *r)
{
    if (!r->nrows) return;
    sqlite3_stmt *u;
    if (sqlite3_prepare_v2(cat->db, "INSERT OR REPLACE INTO restore_checked(node_id, version_id, size, mtime_ns) VALUES(?,?,?,?)",
                           -1, &u, NULL) == SQLITE_OK) {
        int txn = sqlite3_get_autocommit(cat->db) && bd_exec(cat, "BEGIN") == 0;
        for (size_t i = 0; i < r->nrows; i++) {
            sqlite3_bind_int64(u, 1, r->rows[i].node_id);
            sqlite3_bind_int64(u, 2, r->rows[i].version_id);
            sqlite3_bind_int64(u, 3, r->rows[i].size);
            sqlite3_bind_int64(u, 4, r->rows[i].mtime_ns);
            sqlite3_step(u);
            sqlite3_reset(u);
        }
        if (txn && bd_exec(cat, "COMMIT") != 0) bd_exec(cat, "ROLLBACK");
        sqlite3_finalize(u);
    }
    r->nrows = 0;
}

/* Cut the last name off the path p, in place ("C:\a\b" to "C:\a", "/a"
 * to "/"). 0 when p is the top already. */
static int path_up(char *p)
{
    size_t n = strlen(p);
    while (n > 1 && (p[n - 1] == '/' || p[n - 1] == '\\') && !(n == 3 && p[1] == ':')) p[--n] = '\0';
    char *x = strrchr(p, '/'), *y = strrchr(p, '\\');
    char *cut = !x ? y : !y ? x : x > y ? x : y;
    if (!cut || cut[1] == '\0') return 0;
    if (cut == p || (cut == p + 2 && p[1] == ':')) cut[1] = '\0'; /* keep "/" or "C:\\" */
    else *cut = '\0';
    return 1;
}

/* path, or the nearest folder above it that is there; NULL if none is. */
static char *nearest_folder(const char *path)
{
    char *p = path && *path ? bd_strdup(path) : NULL;
    for (int tries = 0; p && tries < 256; tries++) {
        bd_stat_t st;
        if (bd_stat(p, &st) == 0 && st.is_dir) return p;
        if (!path_up(p)) break;
    }
    free(p);
    return NULL;
}

const char *bd_test_dest_fs_name = NULL;

/* The biggest file the file system where a restore writes holds, or -1
 * for no known limit (FAT32 holds files up to 4 GiB less one byte). */
static int64_t dest_file_limit(const char *dest_root)
{
    char fs[64] = "";
    if (bd_test_dest_fs_name) {
        snprintf(fs, sizeof(fs), "%s", bd_test_dest_fs_name);
    } else {
        char *dir = nearest_folder(dest_root);
        if (!dir || bd_fs_name(dir, fs, sizeof(fs)) != 0) fs[0] = '\0';
        free(dir);
    }
    if (!fs[0] || !fs_is_fat(fs)) return -1;
    return bd_test_fat_limit > 0 ? bd_test_fat_limit : FAT_MAX_FILE;
}

/* After a file could not be written where a restore writes: can a new file
 * be made there at all (in dest_root, or the nearest folder above it)? It
 * tells a folder or file that refuses it from a disk that refuses
 * everything. *werr gets why not. */
static int dest_writable(const char *dest_root, int *werr)
{
    *werr = 0;
    if (bd_test_dest_read_only) {
        *werr = EROFS;
        return 0;
    }
    char *dir = nearest_folder(dest_root);
    char *probe = dir ? bd_path_join(dir, "brodalf-write-test" BD_TMP_MARKER) : NULL;
    free(dir);
    if (!probe) return 1; /* cannot tell */
    errno = 0;
    FILE *f = bd_fopen(probe, "wb");
    int ok = f != NULL;
    if (!f) *werr = errno;
    if (f && fclose(f) != 0) {
        ok = 0;
        *werr = errno;
    }
    if (f) bd_remove(probe);
    free(probe);
    return ok;
}

static int disk_full(int e)
{
#ifdef EDQUOT
    if (e == EDQUOT) return 1;
#endif
    return e == ENOSPC;
}

/* Why a file could not be written where it goes, in plain words. */
static const char *dest_error_text(int e, char *buf, size_t cap)
{
    if (disk_full(e)) return "the disk is full";
    switch (e) {
    case EROFS: return "the disk is read-only";
    case EACCES:
    case EPERM: return "no permission to write there, or the disk is write-protected";
    case EFBIG: return "the file is too big for that disk's file system";
    case ENAMETOOLONG: return "the path there is too long";
    case EIO: return "the disk gave an error";
    case 0: return "the system did not say why";
    default: snprintf(buf, cap, "the system gave error %d", e); return buf;
    }
}

/* dest's name with " (restored <day>)" before its extension, and from the
 * second on a number after the day: "a.txt" as "a (restored 2026-11-27).txt",
 * then "a (restored 2026-11-27 2).txt". */
static char *beside_path(const char *dest, const char *day, int n)
{
    const char *name = dest;
    for (const char *p = dest; *p; p++)
        if (*p == '/' || *p == '\\') name = p + 1;
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name) dot = name + strlen(name);
    char num[16] = "";
    if (n > 1) snprintf(num, sizeof(num), " %d", n);
    return bd_sprintf("%.*s (restored %s%s)%s", (int)(dot - dest), dest, day, num, dot);
}

/* With keep_both, where a file goes beside another already at dest: the
 * first free name beside_path gives. *there is set instead when one of
 * them holds this very file already (an earlier run put it there). NULL:
 * none is free, or out of memory. */
static char *find_beside(const restore_run *r, const restore_file_item *it, const char *dest, int *there)
{
    *there = 0;
    for (int n = 1; n <= 99; n++) {
        char *b = beside_path(dest, r->day, n);
        if (!b) return NULL;
        bd_stat_t st;
        if (bd_stat(b, &st) != 0) return b;
        char have[BD_HASH_HEX_LEN + 1];
        if (st.is_file && st.size == it->size && bd_hash_file(b, NULL, have, NULL) == 0 && strcmp(have, it->hash) == 0) {
            *there = 1;
            free(b);
            return NULL;
        }
        free(b);
    }
    return NULL;
}

typedef struct {
    char *rel, *label;
    int64_t copy_id, media_id;
    int sealed;
} restore_copy;

/* Restore one version to dest. Its good copies on the connected drives and
 * extra places are tried in the order of FIND_COPY_SQL until one reads back
 * with the right checksum; one found damaged or missing on the way is
 * marked so (when the job ends, if its drive is still there) and the next
 * is tried. When none here reads back right, it waits for another drive
 * that holds a good copy (offline, the log names it) or for the
 * passphrase. Never writes over anything: a file already there with the
 * right contents is left as it is (already there), any other file there
 * is left alone and listed (bd_list_restore_conflicts), or with keep_both
 * this one is written beside it. A file that cannot be written where it
 * goes is the destination's fault, not the copy's: no other copy is tried.
 * 0 go on, 1 the job was stopped, 2 a drive went (unplugged or not
 * answering; its label is in r->gone), 3 nothing can be written where the
 * files go (the disk is full or read-only, r->stop_errno says why): the
 * restore stops. */
static int restore_file(bd_catalog *cat, restore_run *r, const restore_file_item *it, const char *dest,
                        bd_restore_stats *stats, bd_log_fn log, void *log_ctx)
{
    char *beside = NULL;
    const char *target = dest;
    bd_stat_t dst;
    if (bd_stat(dest, &dst) == 0) {
        if (dst.is_file && dst.size == it->size) {
            int same = checked_has(r, it, &dst);
            if (!same) {
                char have[BD_HASH_HEX_LEN + 1];
                same = bd_hash_file(dest, NULL, have, NULL) == 0 && strcmp(have, it->hash) == 0;
                if (same) note_checked(r, it, dest);
            }
            if (same) {
                stats->files_already_there++;
                return 0;
            }
        }
        if (r->keep_both) {
            int there = 0;
            beside = find_beside(r, it, dest, &there);
            if (there) {
                stats->files_already_there++;
                return 0;
            }
        }
        if (!beside) {
            stats->files_conflict++;
            note_conflict(cat, it->version_id, it->source_name, it->rel, dest, it->size, it->mtime, &dst);
            bd_logf(log, log_ctx, "something else is already at %s; left alone", dest);
            return 0;
        }
        target = beside;
    }
    if (r->max_file >= 0 && it->size > r->max_file) {
        stats->files_failed++;
        stats->files_cannot_write++;
        bd_logf(log, log_ctx,
                "%s was not restored: it is too big for where it goes, a disk formatted as FAT32, which holds files up to"
                " 4 GB. Restore it to a disk formatted as NTFS or exFAT",
                target);
        free(beside);
        return 0;
    }
    restore_copy *copies = NULL;
    size_t nc = 0, cap = 0;
    int nomem = 0;
    sqlite3_reset(r->find);
    sqlite3_bind_int64(r->find, 1, it->version_id);
    while (sqlite3_step(r->find) == SQLITE_ROW) {
        if (nc == cap) {
            size_t ncap = cap ? cap * 2 : 4;
            restore_copy *v = realloc(copies, ncap * sizeof(*v));
            if (!v) { nomem = 1; break; }
            copies = v;
            cap = ncap;
        }
        restore_copy *c = &copies[nc++];
        c->rel = bd_strdup((const char *)sqlite3_column_text(r->find, 0));
        c->media_id = sqlite3_column_int64(r->find, 1);
        c->sealed = sqlite3_column_int(r->find, 2);
        c->label = bd_strdup((const char *)sqlite3_column_text(r->find, 3));
        c->copy_id = sqlite3_column_int64(r->find, 4);
        if (!c->rel || !c->label) nomem = 1;
    }
    sqlite3_reset(r->find);
    if (nc == 0 && !nomem) {
        sqlite3_reset(r->any);
        sqlite3_bind_int64(r->any, 1, it->version_id);
        sqlite3_bind_int64(r->any, 2, it->node_id);
        if (sqlite3_step(r->any) == SQLITE_ROW) stats->files_offline++;
        else stats->files_no_copy++;
        sqlite3_reset(r->any);
        free(copies);
        free(beside);
        return 0;
    }
    int result = 0, done = 0, tried = 0, locked = 0, write_failed = 0, werr = 0;
    for (size_t i = 0; i < nc && !done && !nomem; i++) {
        restore_copy *c = &copies[i];
        if (c->sealed && !cat->have_key) { locked++; continue; }
        tried++;
        if (!bd_stored_path_safe(c->rel)) {
            bd_logf(log, log_ctx, "the copy on \"%s\" is recorded at a path that could lead outside its folder, so it is not read: %s",
                    c->label, it->rel);
            continue;
        }
        if (!r->store || r->store->media_id != c->media_id) {
            bd_store_close(r->store);
            r->store = bd_store_open(cat, c->media_id);
        }
        /* On a drive that hides names the path there means nothing to the user. */
        const char *shown = bd_media_hides_names(cat, c->media_id, NULL) ? it->rel : c->rel;
        char *src = NULL, *tmp = NULL;
        int src_temp = 0, rc = -1, w = 0;
        int64_t bytes = 0;
        int got = r->store ? bd_store_fetch(r->store, c->rel, &src, &src_temp) : -1;
        if (got == 0) {
            rc = copy_verified(src, c->sealed ? cat->key : NULL, target, NULL, it->hash, it->size, it->mtime, it->btime, &tmp,
                               &bytes, &w);
            if (rc == 0) {
                errno = 0;
                if (bd_rename_noreplace(tmp, target) != 0) {
                    w = errno;
                    bd_remove(tmp);
                    bd_stat_t now;
                    if (bd_stat(target, &now) == 0) {
                        stats->files_conflict++;
                        note_conflict(cat, it->version_id, it->source_name, it->rel, target, it->size, it->mtime, &now);
                        rc = 2;
                    } else {
                        rc = -2;
                    }
                }
            }
        }
        bd_store_release(src, src_temp);
        free(tmp);
        if (rc == 0) {
            stats->files_restored++;
            stats->bytes_restored += bytes;
            if (beside) {
                stats->files_beside++;
                bd_logf(log, log_ctx, "another file is already at %s, so it was restored beside it as %s", dest, target);
            } else {
                note_checked(r, it, target);
            }
            if (tried > 1) bd_logf(log, log_ctx, "restored %s from the copy on \"%s\"", target, c->label);
            done = 1;
        } else if (rc == -4) {
            result = 1; /* stopped; the half-written temp file is gone */
            done = 1;
        } else if (rc == 2) {
            done = 1; /* something appeared at dest meanwhile: left alone */
        } else if (rc == -2) {
            /* Where it goes cannot take it: another copy would not help. */
            write_failed = 1;
            werr = w;
            done = 1;
        } else if (!bd_media_present(cat, c->media_id)) {
            /* Not readable because the drive itself went (or another one
             * is there now): nothing is said about the copy. */
            snprintf(r->gone, sizeof(r->gone), "%s", c->label);
            result = 2;
            done = 1;
        } else if (got == 1 || rc == 1) {
            int missing = got == 1;
            bd_logf(log, log_ctx, "the copy on \"%s\" is %s: %s%s", c->label, missing ? "missing" : "damaged", shown,
                    i + 1 < nc ? "; trying the next copy" : "");
            /* Out of memory only means it is not marked this time. */
            bd_copy_marks_add(&r->marks, c->copy_id, c->media_id, !missing, !missing);
        } else {
            bd_logf(log, log_ctx, "cannot restore %s from the copy on \"%s\"%s%s%s", target, c->label,
                    r->store && r->store->err[0] ? ": " : "", r->store ? r->store->err : "",
                    i + 1 < nc ? "; trying the next copy" : "");
        }
    }
    if (nomem) {
        stats->files_failed++;
        bd_logf(log, log_ctx, "out of memory: cannot restore %s", target);
    } else if (write_failed) {
        stats->files_failed++;
        stats->files_cannot_write++;
        char buf[64];
        bd_logf(log, log_ctx, "cannot write %s: %s. It was not restored", target, dest_error_text(werr, buf, sizeof(buf)));
        int w2 = 0;
        if (disk_full(werr) || werr == EROFS) {
            r->stop_errno = werr;
            result = 3;
        } else if (!dest_writable(r->dest_root, &w2)) {
            r->stop_errno = w2 ? w2 : werr;
            result = 3;
        }
    } else if (!done) {
        /* A drive not plugged in may hold a good copy: then it waits for
         * that drive, named, rather than failing. */
        char where[400] = "";
        size_t nw = 0;
        if (!locked) {
            sqlite3_reset(r->elsewhere);
            sqlite3_bind_int64(r->elsewhere, 1, it->version_id);
            while (sqlite3_step(r->elsewhere) == SQLITE_ROW && nw < sizeof(where) - 1) {
                const char *label = (const char *)sqlite3_column_text(r->elsewhere, 0);
                int k = snprintf(where + nw, sizeof(where) - nw, "%s\"%s\"", nw ? " or " : "", label ? label : "");
                if (k < 0) break;
                nw = (size_t)k < sizeof(where) - nw ? nw + (size_t)k : sizeof(where) - 1;
            }
            sqlite3_reset(r->elsewhere);
        }
        if (locked) {
            stats->files_need_passphrase++;
            if (tried)
                bd_logf(log, log_ctx, "no plain copy of %s could be read back right; it waits for the passphrase, to be"
                        " restored from an encrypted copy", target);
        } else if (nw) {
            stats->files_offline++;
            bd_logf(log, log_ctx, "no copy of %s on the drives plugged in could be read back right; plug in %s to restore it",
                    target, where);
        } else if (tried) {
            stats->files_failed++;
            bd_logf(log, log_ctx, "no copy of %s could be read back right, so it was not restored", target);
        }
    }
    for (size_t i = 0; i < nc; i++) {
        free(copies[i].rel);
        free(copies[i].label);
    }
    free(copies);
    free(beside);
    return result;
}

/* A plain copy before an encrypted one (it needs no passphrase), and one on
 * a drive before one in an extra place; after those, the copy written
 * first. */
#define FIND_COPY_SQL                                                                                   \
    "SELECT c.path_on_media, c.media_id, (c.encrypted<>0 OR m.encrypted<>0) AS sealed, m.label, c.id"   \
    " FROM copies c JOIN temp.connected k ON k.media_id=c.media_id"                                     \
    " JOIN media m ON m.id=c.media_id WHERE c.version_id=? AND c.state='ok'"                            \
    " ORDER BY sealed, m.kind='drive' DESC, m.secondary, c.id"

/* place: checked_place, to use and note what is known to be there; NULL
 * for neither. */
static bd_status restore_run_begin(bd_catalog *cat, restore_run *r, const char *dest_root, const bd_restore_opts *opts,
                                   const char *place)
{
    memset(r, 0, sizeof(*r));
    r->max_file = -1;
    r->keep_both = opts ? opts->keep_both : 0;
    if (sqlite3_prepare_v2(cat->db, FIND_COPY_SQL, -1, &r->find, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(cat->db, ANY_COPY_SQL, -1, &r->any, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(cat->db, ELSEWHERE_SQL, -1, &r->elsewhere, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "prepare restore");
    r->dest_root = bd_strdup(dest_root ? dest_root : "");
    if (!r->dest_root) return BD_ERR_NOMEM;
    r->max_file = dest_file_limit(dest_root);
    time_t now = time(NULL);
    struct tm tm;
#ifdef _WIN32
    int have_tm = localtime_s(&tm, &now) == 0;
#else
    int have_tm = localtime_r(&now, &tm) != NULL;
#endif
    if (!have_tm || strftime(r->day, sizeof(r->day), "%Y-%m-%d", &tm) == 0) snprintf(r->day, sizeof(r->day), "today");
    /* What was found there counts for one place: a restore to another
     * forgets it first. Without the table it is only not used. */
    if (place && bd_exec(cat, RESTORE_TABLES) == 0) {
        sqlite3_stmt *q;
        int same = 0;
        if (sqlite3_prepare_v2(cat->db, CHECKED_FOR_SQL, -1, &q, NULL) == SQLITE_OK) {
            sqlite3_bind_text(q, 1, place, -1, SQLITE_STATIC);
            same = sqlite3_step(q) == SQLITE_ROW;
            sqlite3_finalize(q);
        }
        if (same || (bd_exec(cat, "DELETE FROM restore_checked") == 0 && bd_setting_set(cat, "restore_checked_for", place) == 0)) {
            if (sqlite3_prepare_v2(cat->db, CHECKED_SQL, -1, &r->checked, NULL) != SQLITE_OK) r->checked = NULL;
        }
    }
    return BD_OK;
}

/* Record the copies found missing or damaged, on the drives still there,
 * and let go of the rest. */
static void restore_run_end(bd_catalog *cat, restore_run *r, bd_log_fn log, void *log_ctx)
{
    if (r->marks.n) {
        int txn = bd_exec(cat, "BEGIN") == 0;
        int64_t dropped = bd_copy_marks_apply(cat, &r->marks);
        if (dropped < 0) bd_logf(log, log_ctx, "cannot record the copies found damaged or missing: %s", sqlite3_errmsg(cat->db));
        if (txn) bd_exec(cat, dropped < 0 ? "ROLLBACK" : "COMMIT");
    }
    bd_copy_marks_free(&r->marks);
    checked_flush(cat, r);
    sqlite3_finalize(r->find);
    sqlite3_finalize(r->any);
    sqlite3_finalize(r->elsewhere);
    sqlite3_finalize(r->checked);
    bd_store_close(r->store);
    free(r->rows);
    free(r->dest_root);
    r->find = r->any = r->elsewhere = r->checked = NULL;
    r->rows = NULL;
    r->nrows = r->caprows = 0;
    r->dest_root = NULL;
    r->store = NULL;
}

static bd_status restore_gone(bd_catalog *cat, const restore_run *r)
{
    return bd_fail(cat, BD_ERR_DRIVE_GONE,
                   "\"%s\" stopped answering or was unplugged during the restore. What was restored is kept, and nothing on"
                   " the drive was marked missing or damaged; plug it in again and run the same restore again",
                   r->gone);
}

/* The restore stopped: nothing can be written to where, for the reason e. */
static bd_status restore_cannot_write(bd_catalog *cat, const char *where, int e)
{
    char buf[64];
    if (disk_full(e))
        return bd_fail(cat, BD_ERR_IO,
                       "%s is full, so the restore stopped. What was restored is kept; make room there, or choose another"
                       " folder, and run the same restore again",
                       where);
    return bd_fail(cat, BD_ERR_IO, "%s cannot be written to (%s), so the restore stopped. What was restored is kept", where,
                   dest_error_text(e, buf, sizeof(buf)));
}

bd_status bd_restore_ex(bd_catalog *cat, int64_t source_id, const char *rel_prefix, const char *dest_root,
                        const bd_restore_opts *opts, bd_restore_stats *stats, bd_log_fn log, void *log_ctx)
{
    int mode = restore_mode(opts);
    bd_restore_stats local;
    if (!stats) stats = &local;
    memset(stats, 0, sizeof(*stats));
    bd_cancel_reset(cat);
    bd_exec(cat, "DELETE FROM temp.restore_conflicts");
    if (!rel_prefix) rel_prefix = "";
    char *prefix = bd_strdup(rel_prefix);
    if (!prefix) return BD_ERR_NOMEM;
    for (char *p = prefix; *p; p++) if (*p == '\\') *p = '/';
    size_t plen = strlen(prefix);
    while (plen > 0 && prefix[plen - 1] == '/') prefix[--plen] = '\0';
    int flat = restore_flat(cat, source_id);

    sqlite3_stmt *q = NULL;
    restore_run run;
    memset(&run, 0, sizeof(run));
    char *place = checked_place(flat, dest_root);
    bd_status s = place ? restore_run_begin(cat, &run, dest_root, opts, place) : BD_ERR_NOMEM;
    free(place);
    if (s != BD_OK) goto done;
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT n.id, n.rel_path, n.is_dir, s.name, v.id, v.hash, v.size,"
                           " CASE WHEN n.deleted=0 AND v.id=n.current_version_id THEN n.mtime_ns ELSE v.mtime_ns END,"
                           " n.btime_ns, v.id<>n.current_version_id,"
                           " EXISTS(SELECT 1 FROM copies xc WHERE xc.version_id=n.current_version_id),"
                           " strftime('%Y-%m-%d %H:%M', CASE WHEN v.mtime_ns>0 THEN v.mtime_ns/1000000000"
                           "  ELSE v.first_seen_ms/1000 END, 'unixepoch', 'localtime') FROM nodes n"
                           " JOIN sources s ON s.id=n.source_id LEFT JOIN versions v ON v.id=" RESTORE_VERSION
                           " WHERE" RESTORE_WHERE " AND n.id>?3 ORDER BY n.id LIMIT 256",
                           -1, &q, NULL) != SQLITE_OK) {
        s = bd_fail_db(cat, "prepare restore");
        goto done;
    }

    /* Totals for the progress bar. */
    sqlite3_stmt *t;
    int64_t total_files = 0, total_bytes = 0;
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT COUNT(*), COALESCE(SUM(v.size),0) FROM nodes n JOIN versions v ON v.id=" RESTORE_VERSION
                           " WHERE n.is_dir=0 AND" RESTORE_WHERE " AND ?3=?3",
                           -1, &t, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(t, 1, source_id);
        sqlite3_bind_text(t, 2, prefix, -1, SQLITE_STATIC);
        sqlite3_bind_int64(t, 3, 0);
        sqlite3_bind_int(t, 4, mode);
        if (sqlite3_step(t) == SQLITE_ROW) {
            total_files = sqlite3_column_int64(t, 0);
            total_bytes = sqlite3_column_int64(t, 1);
        }
        sqlite3_finalize(t);
    }
    bd_progress_begin(cat, "restore", total_files, total_bytes);
    int64_t files_seen = 0;
    int64_t job = bd_job_start(cat, "restore", 0);

    typedef struct {
        int64_t id, version_id, size, mtime, btime;
        int is_dir, older, newer_had_copies;
        char *rel, *source_name, *hash, *when;
    } item;
    item items[BATCH];
    int64_t last_id = 0;
    int cancelled = 0, gone = 0, full = 0;
    while (!cancelled && !gone && !full) {
        size_t n = 0;
        sqlite3_reset(q);
        sqlite3_bind_int64(q, 1, source_id);
        sqlite3_bind_text(q, 2, prefix, -1, SQLITE_STATIC);
        sqlite3_bind_int64(q, 3, last_id);
        sqlite3_bind_int(q, 4, mode);
        while (n < BATCH && sqlite3_step(q) == SQLITE_ROW) {
            item *it = &items[n++];
            it->id = last_id = sqlite3_column_int64(q, 0);
            it->rel = bd_strdup((const char *)sqlite3_column_text(q, 1));
            it->is_dir = sqlite3_column_int(q, 2);
            it->source_name = bd_strdup((const char *)sqlite3_column_text(q, 3));
            it->version_id = sqlite3_column_int64(q, 4);
            it->hash = sqlite3_column_type(q, 5) == SQLITE_NULL ? NULL : bd_strdup((const char *)sqlite3_column_text(q, 5));
            it->size = sqlite3_column_int64(q, 6);
            it->mtime = sqlite3_column_int64(q, 7);
            it->btime = sqlite3_column_int64(q, 8);
            it->older = sqlite3_column_int(q, 9);
            it->newer_had_copies = sqlite3_column_int(q, 10);
            it->when = sqlite3_column_type(q, 11) == SQLITE_NULL ? NULL : bd_strdup((const char *)sqlite3_column_text(q, 11));
        }
        sqlite3_reset(q);
        if (n == 0) break;

        for (size_t i = 0; i < n && !cancelled && !gone && !full; i++) {
            item *it = &items[i];
            if (bd_cancelled(cat)) { cancelled = 1; break; }
            /* A catalog recovered from a drive may hold anything: nothing
             * is written outside the folder chosen. */
            if (it->rel && it->source_name && (!bd_stored_path_safe(it->rel) || (!flat && !bd_stored_name_safe(it->source_name)))) {
                if (!it->is_dir) {
                    stats->files_failed++;
                    bd_report_file(cat, files_seen++, it->rel, it->size);
                }
                bd_logf(log, log_ctx, "not restored: the path stored for \"%s/%s\" could lead outside the folder you chose",
                        it->source_name, it->rel);
                continue;
            }
            char *dest = it->rel ? restore_dest(dest_root, flat, it->source_name, it->rel) : NULL;
            if (!dest) { stats->files_failed++; continue; }
            if (it->is_dir) {
                bd_mkdirs(dest);
                free(dest);
                continue;
            }
            bd_report_file(cat, files_seen++, it->rel, it->size);
            if (!it->version_id || !it->hash) { stats->files_no_copy++; free(dest); continue; }
            restore_file_item rf = {it->version_id, it->size, it->mtime, it->btime, it->hash, it->source_name, it->rel, it->id};
            int64_t restored_before = stats->files_restored;
            int r = restore_file(cat, &run, &rf, dest, stats, log, log_ctx);
            if (r == 1) cancelled = 1;
            else if (r == 2) gone = 1;
            else if (r == 3) full = 1;
            if (it->older && stats->files_restored > restored_before) {
                stats->files_older_version++;
                bd_logf(log, log_ctx, "%s: restored the version from %s; the newer one %s", dest, it->when ? it->when : "before",
                        it->newer_had_copies ? "has no good copy to restore from" : "was never backed up");
            }
            free(dest);
        }
        for (size_t i = 0; i < n; i++) {
            free(items[i].rel);
            free(items[i].source_name);
            free(items[i].hash);
            free(items[i].when);
        }
        checked_flush(cat, &run);
    }
    if (stats->files_conflict)
        bd_logf(log, log_ctx, "%lld %s left alone because something else was already there",
                (long long)stats->files_conflict, stats->files_conflict == 1 ? "file was" : "files were");
    bd_report(cat, files_seen, NULL, 1);
    bd_progress_end(cat);
    int stop_errno = run.stop_errno;
    restore_run_end(cat, &run, log, log_ctx);
    bd_job_finish(cat, job,
                  gone || full ? "failed" : cancelled ? "stopped"
                  : stats->files_failed || stats->files_offline || stats->files_need_passphrase ? "partial" : "done",
                  stats->files_restored, stats->files_failed, stats->bytes_restored);
    if (gone) s = restore_gone(cat, &run);
    else if (full) s = restore_cannot_write(cat, dest_root, stop_errno);
    else if (cancelled) s = bd_fail_cancelled(cat);
done:
    sqlite3_finalize(q);
    restore_run_end(cat, &run, log, log_ctx);
    free(prefix);
    return s;
}

bd_status bd_restore_version(bd_catalog *cat, int64_t version_id, const char *dest_folder, bd_restore_stats *stats,
                             bd_log_fn log, void *log_ctx)
{
    return bd_restore_version_ex(cat, version_id, dest_folder, NULL, stats, log, log_ctx);
}

bd_status bd_restore_version_ex(bd_catalog *cat, int64_t version_id, const char *dest_folder, const bd_restore_opts *opts,
                                bd_restore_stats *stats, bd_log_fn log, void *log_ctx)
{
    bd_restore_stats local;
    if (!stats) stats = &local;
    memset(stats, 0, sizeof(*stats));
    bd_cancel_reset(cat);
    bd_exec(cat, "DELETE FROM temp.restore_conflicts");
    sqlite3_stmt *q = NULL;
    restore_run run;
    bd_status s = restore_run_begin(cat, &run, dest_folder, opts, NULL);
    if (s != BD_OK) goto done;
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT v.hash, v.size, CASE WHEN v.id=n.current_version_id AND n.deleted=0 THEN n.mtime_ns ELSE v.mtime_ns END,"
                           " n.btime_ns, n.rel_path, s.name FROM versions v JOIN nodes n ON n.id=v.node_id"
                           " JOIN sources s ON s.id=n.source_id WHERE v.id=?", -1, &q, NULL) != SQLITE_OK) {
        s = bd_fail_db(cat, "prepare restore");
        goto done;
    }
    sqlite3_bind_int64(q, 1, version_id);
    if (sqlite3_step(q) != SQLITE_ROW) { s = bd_fail(cat, BD_ERR_NOT_FOUND, "no such version"); goto done; }
    restore_file_item rf;
    rf.version_id = version_id;
    rf.hash = (const char *)sqlite3_column_text(q, 0);
    rf.size = sqlite3_column_int64(q, 1);
    rf.mtime = sqlite3_column_int64(q, 2);
    rf.btime = sqlite3_column_int64(q, 3);
    rf.rel = (const char *)sqlite3_column_text(q, 4);
    rf.source_name = (const char *)sqlite3_column_text(q, 5);
    rf.node_id = 0;
    if (!rf.rel || !bd_stored_path_safe(rf.rel)) {
        stats->files_failed++;
        bd_logf(log, log_ctx, "not restored: the path stored for \"%s\" could lead outside the folder you chose",
                rf.rel ? rf.rel : "");
        goto done;
    }
    char *dest = bd_path_join(dest_folder, bd_rel_basename(rf.rel));
    if (!dest) { s = BD_ERR_NOMEM; goto done; }
    bd_mkdirs(dest_folder);
    bd_progress_begin(cat, "restore", 1, rf.size);
    bd_report_file(cat, 0, rf.rel, rf.size);
    int64_t job = bd_job_start(cat, "restore", 0);
    int r = restore_file(cat, &run, &rf, dest, stats, log, log_ctx);
    bd_report(cat, 1, NULL, 1);
    bd_progress_end(cat);
    int stop_errno = run.stop_errno;
    restore_run_end(cat, &run, log, log_ctx);
    bd_job_finish(cat, job,
                  r == 2 || r == 3 ? "failed" : r == 1 ? "stopped"
                  : stats->files_restored || stats->files_already_there ? "done" : "partial",
                  stats->files_restored, stats->files_failed, stats->bytes_restored);
    if (r == 2) s = restore_gone(cat, &run);
    else if (r == 3) s = restore_cannot_write(cat, dest_folder, stop_errno);
    else if (r == 1) s = bd_fail_cancelled(cat);
    free(dest);
done:
    sqlite3_finalize(q);
    restore_run_end(cat, &run, log, log_ctx);
    return s;
}

bd_status bd_list_restore_conflicts(bd_catalog *cat, bd_conflict_fn fn, void *ctx)
{
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT source_name, rel_path, dest, want_size, want_mtime_ns, have_size, have_mtime_ns, version_id"
                           " FROM temp.restore_conflicts ORDER BY id", -1, &q, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "list restore conflicts");
    while (sqlite3_step(q) == SQLITE_ROW) {
        bd_conflict_info c;
        c.source_name = (const char *)sqlite3_column_text(q, 0);
        c.rel_path = (const char *)sqlite3_column_text(q, 1);
        c.dest = (const char *)sqlite3_column_text(q, 2);
        c.want_size = sqlite3_column_int64(q, 3);
        c.want_mtime_ns = sqlite3_column_int64(q, 4);
        c.have_size = sqlite3_column_int64(q, 5);
        c.have_mtime_ns = sqlite3_column_int64(q, 6);
        c.version_id = sqlite3_column_int64(q, 7);
        if (fn(ctx, &c) != 0) break;
    }
    sqlite3_finalize(q);
    return BD_OK;
}

/* ---- Restore plan ------------------------------------------------------ */

typedef struct {
    int64_t id;
    char *label, *kind, *location;
    int connected;
    int used;
} plan_media;

static int add_step_bytes(void *ctx, const bd_restore_step *st)
{
    *(int64_t *)ctx += st->bytes;
    return 0;
}

/* Free space at path, or at the nearest folder above it that is there. */
static int64_t free_at(const char *path)
{
    if (bd_test_free_bytes >= 0) return bd_test_free_bytes;
    char *p = path ? bd_strdup(path) : NULL;
    int64_t total = 0, freeb = -1;
    for (int tries = 0; p && *p && tries < 256; tries++) {
        if (bd_disk_space(p, &total, &freeb) == 0) break;
        freeb = -1;
        size_t n = strlen(p);
        while (n > 1 && (p[n - 1] == '/' || p[n - 1] == '\\') && !(n == 3 && p[1] == ':')) p[--n] = '\0';
        char *x = strrchr(p, '/'), *y = strrchr(p, '\\');
        char *cut = !x ? y : !y ? x : x > y ? x : y;
        if (!cut || cut[1] == '\0') break; /* the top, and it cannot be read */
        if (cut == p || (cut == p + 2 && p[1] == ':')) cut[1] = '\0'; /* keep "/" or "C:\\" */
        else *cut = '\0';
    }
    free(p);
    return freeb;
}

bd_status bd_restore_room(bd_catalog *cat, int64_t source_id, const char *rel_prefix, const char *dest_root,
                          const bd_restore_opts *opts, int64_t version_id, int64_t *bytes_needed, int64_t *bytes_free)
{
    int64_t need = 0;
    bd_status s = BD_OK;
    if (version_id > 0) {
        sqlite3_stmt *q;
        if (sqlite3_prepare_v2(cat->db, "SELECT size FROM versions WHERE id=?", -1, &q, NULL) != SQLITE_OK)
            return bd_fail_db(cat, "find the version to restore");
        sqlite3_bind_int64(q, 1, version_id);
        if (sqlite3_step(q) == SQLITE_ROW) need = sqlite3_column_int64(q, 0);
        else s = bd_fail(cat, BD_ERR_NOT_FOUND, "no such version");
        sqlite3_finalize(q);
    } else {
        s = bd_restore_plan_ex(cat, source_id, rel_prefix, dest_root, opts, add_step_bytes, &need, NULL, NULL);
    }
    if (bytes_needed) *bytes_needed = need;
    if (bytes_free) *bytes_free = free_at(dest_root);
    return s;
}

bd_status bd_restore_plan(bd_catalog *cat, int64_t source_id, const char *rel_prefix, const char *dest_root,
                          bd_restore_step_fn fn, void *ctx, int64_t *files_total, int64_t *files_no_copy)
{
    return bd_restore_plan_ex(cat, source_id, rel_prefix, dest_root, NULL, fn, ctx, files_total, files_no_copy);
}

bd_status bd_restore_plan_ex(bd_catalog *cat, int64_t source_id, const char *rel_prefix, const char *dest_root,
                             const bd_restore_opts *opts, bd_restore_step_fn fn, void *ctx, int64_t *files_total,
                             int64_t *files_no_copy)
{
    bd_restore_counts counts;
    bd_status s = bd_restore_plan_counts(cat, source_id, rel_prefix, dest_root, opts, fn, ctx, &counts);
    if (files_total) *files_total = counts.files;
    if (files_no_copy) *files_no_copy = counts.files_no_copy;
    return s;
}

bd_status bd_restore_plan_counts(bd_catalog *cat, int64_t source_id, const char *rel_prefix, const char *dest_root,
                                 const bd_restore_opts *opts, bd_restore_step_fn fn, void *ctx, bd_restore_counts *counts)
{
    int mode = restore_mode(opts), keep_both = opts ? opts->keep_both : 0;
    int flat = restore_flat(cat, source_id);
    bd_restore_counts local;
    if (!counts) counts = &local;
    memset(counts, 0, sizeof(*counts));
    char *prefix = bd_strdup(rel_prefix ? rel_prefix : "");
    if (!prefix) return BD_ERR_NOMEM;
    for (char *p = prefix; *p; p++) if (*p == '\\') *p = '/';
    size_t plen = strlen(prefix);
    while (plen > 0 && prefix[plen - 1] == '/') prefix[--plen] = '\0';

    bd_status s = BD_OK;
    sqlite3_stmt *mq = NULL, *fq = NULL, *cq = NULL, *kq = NULL;
    plan_media *media = NULL;
    size_t nm = 0;
    int64_t *fsize = NULL;
    unsigned char *covered = NULL;
    size_t nf = 0, capf = 0;
    /* (file, medium) pairs: a good copy of the file's current version. */
    size_t *pf = NULL, *pm = NULL, np = 0, capp = 0;

    if (sqlite3_prepare_v2(cat->db,
                           "SELECT m.id, m.label, m.kind, COALESCE(trim(m.location),''),"
                           " EXISTS(SELECT 1 FROM temp.connected k WHERE k.media_id=m.id) AS on_now FROM media m"
                           " WHERE m.retired_ms IS NULL OR on_now ORDER BY m.id",
                           -1, &mq, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(cat->db,
                           "SELECT n.id, n.rel_path, s.name, v.size, v.id FROM nodes n JOIN sources s ON s.id=n.source_id"
                           " JOIN versions v ON v.id=" RESTORE_VERSION
                           " WHERE n.is_dir=0 AND" RESTORE_WHERE " AND ?3=?3 ORDER BY n.id",
                           -1, &fq, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(cat->db, "SELECT DISTINCT media_id FROM copies WHERE version_id=? AND state='ok'", -1, &cq, NULL) !=
            SQLITE_OK) {
        s = bd_fail_db(cat, "plan the restore");
        goto done;
    }
    while (sqlite3_step(mq) == SQLITE_ROW) {
        plan_media *nl = realloc(media, (nm + 1) * sizeof(plan_media));
        if (!nl) { s = BD_ERR_NOMEM; goto done; }
        media = nl;
        plan_media *m = &media[nm++];
        m->id = sqlite3_column_int64(mq, 0);
        m->label = bd_strdup((const char *)sqlite3_column_text(mq, 1));
        m->kind = bd_strdup((const char *)sqlite3_column_text(mq, 2));
        m->location = bd_strdup((const char *)sqlite3_column_text(mq, 3));
        m->connected = sqlite3_column_int(mq, 4);
        m->used = 0;
    }

    /* What an earlier run of this restore found there, when it was to the
     * same place (bd_restore_ex). */
    if (dest_root && *dest_root) {
        char *place = checked_place(flat, dest_root);
        sqlite3_stmt *pq;
        int same = 0;
        if (place && sqlite3_prepare_v2(cat->db, CHECKED_FOR_SQL, -1, &pq, NULL) == SQLITE_OK) {
            sqlite3_bind_text(pq, 1, place, -1, SQLITE_STATIC);
            same = sqlite3_step(pq) == SQLITE_ROW;
            sqlite3_finalize(pq);
        }
        free(place);
        if (same && sqlite3_prepare_v2(cat->db, CHECKED_SQL, -1, &kq, NULL) != SQLITE_OK) kq = NULL;
    }
    sqlite3_bind_int64(fq, 1, source_id);
    sqlite3_bind_text(fq, 2, prefix, -1, SQLITE_STATIC);
    sqlite3_bind_int64(fq, 3, 0);
    sqlite3_bind_int(fq, 4, mode);
    while (sqlite3_step(fq) == SQLITE_ROW) {
        int64_t size = sqlite3_column_int64(fq, 3);
        if (dest_root && *dest_root) {
            /* Something already where it goes: known to be this file, or
             * for the restore to compare (or leave be and list); it needs
             * a drive only to be written beside it. */
            char *dest = restore_dest(dest_root, flat, (const char *)sqlite3_column_text(fq, 2),
                                      (const char *)sqlite3_column_text(fq, 1));
            bd_stat_t st;
            int there = dest && bd_stat(dest, &st) == 0;
            free(dest);
            if (there) {
                int same_size = st.is_file && st.size == size;
                if (same_size && kq) {
                    sqlite3_reset(kq);
                    sqlite3_bind_int64(kq, 1, sqlite3_column_int64(fq, 0));
                    sqlite3_bind_int64(kq, 2, sqlite3_column_int64(fq, 4));
                    sqlite3_bind_int64(kq, 3, st.size);
                    sqlite3_bind_int64(kq, 4, st.mtime_ns);
                    int known = sqlite3_step(kq) == SQLITE_ROW;
                    sqlite3_reset(kq);
                    if (known) {
                        counts->files_checked++;
                        continue;
                    }
                }
                if (same_size) counts->files_to_compare++;
                else counts->files_in_the_way++;
                if (!keep_both) {
                    counts->files++;
                    continue;
                }
            }
        }
        counts->files++;
        if (nf == capf) {
            size_t nc = capf ? capf * 2 : 256;
            int64_t *ns = realloc(fsize, nc * sizeof(int64_t));
            if (ns) fsize = ns;
            unsigned char *ncv = realloc(covered, nc);
            if (ncv) covered = ncv;
            if (!ns || !ncv) { s = BD_ERR_NOMEM; goto done; }
            capf = nc;
        }
        fsize[nf] = size;
        covered[nf] = 0;
        sqlite3_reset(cq);
        sqlite3_bind_int64(cq, 1, sqlite3_column_int64(fq, 4));
        int any = 0;
        while (sqlite3_step(cq) == SQLITE_ROW) {
            int64_t mid = sqlite3_column_int64(cq, 0);
            size_t mi = 0;
            while (mi < nm && media[mi].id != mid) mi++;
            if (mi == nm) continue;
            if (np == capp) {
                size_t nc = capp ? capp * 2 : 256;
                size_t *a = realloc(pf, nc * sizeof(size_t));
                if (a) pf = a;
                size_t *b = realloc(pm, nc * sizeof(size_t));
                if (b) pm = b;
                if (!a || !b) { s = BD_ERR_NOMEM; goto done; }
                capp = nc;
            }
            pf[np] = nf;
            pm[np] = mi;
            np++;
            any = 1;
        }
        if (!any) counts->files_no_copy++;
        nf++;
    }

    /* Greedy: plugged-in drives first, then whichever drive supplies the
     * most of what is still missing. */
    int64_t *gain = calloc(nm ? nm : 1, sizeof(int64_t)), *gain_bytes = calloc(nm ? nm : 1, sizeof(int64_t));
    if (!gain || !gain_bytes) { free(gain); free(gain_bytes); s = BD_ERR_NOMEM; goto done; }
    for (int pass = 0; pass < 2; pass++) {
        for (;;) {
            memset(gain, 0, nm * sizeof(int64_t));
            memset(gain_bytes, 0, nm * sizeof(int64_t));
            for (size_t i = 0; i < np; i++)
                if (!covered[pf[i]] && !media[pm[i]].used) {
                    gain[pm[i]]++;
                    gain_bytes[pm[i]] += fsize[pf[i]];
                }
            size_t best = nm;
            for (size_t m = 0; m < nm; m++) {
                if (media[m].used || !gain[m] || (pass == 0 && !media[m].connected)) continue;
                if (best == nm || gain[m] > gain[best]) best = m;
            }
            if (best == nm) break;
            media[best].used = 1;
            for (size_t i = 0; i < np; i++)
                if (pm[i] == best) covered[pf[i]] = 1;
            bd_restore_step step;
            memset(&step, 0, sizeof(step));
            step.media_id = media[best].id;
            step.label = media[best].label ? media[best].label : "";
            step.kind = media[best].kind ? media[best].kind : "drive";
            step.location = media[best].location ? media[best].location : "";
            step.connected = media[best].connected;
            step.files = gain[best];
            step.bytes = gain_bytes[best];
            if (fn && fn(ctx, &step) != 0) { pass = 2; break; }
        }
    }
    free(gain);
    free(gain_bytes);

done:
    sqlite3_finalize(mq);
    sqlite3_finalize(fq);
    sqlite3_finalize(cq);
    sqlite3_finalize(kq);
    for (size_t i = 0; i < nm; i++) {
        free(media[i].label);
        free(media[i].kind);
        free(media[i].location);
    }
    free(media);
    free(fsize);
    free(covered);
    free(pf);
    free(pm);
    free(prefix);
    return s;
}

/* ---- The restore that waits -------------------------------------------- */

#define PENDING_DELETED_ONLY 1
#define PENDING_WITH_DELETED 2
#define PENDING_KEEP_BOTH 4

bd_status bd_restore_pending_set(bd_catalog *cat, int64_t source_id, const char *rel_prefix, const char *dest,
                                 const bd_restore_opts *opts, int64_t offer_root)
{
    if (!dest || !*dest) return bd_fail(cat, BD_ERR_INVALID, "no folder to restore to");
    if (bd_exec(cat, RESTORE_TABLES) != 0) return bd_fail_db(cat, "keep the restore that waits");
    sqlite3_stmt *u;
    if (sqlite3_prepare_v2(cat->db,
                           "INSERT OR REPLACE INTO restore_pending(id, source_id, rel_path, dest, opts, offer_root, saved_ms)"
                           " VALUES(1,?,?,?,?,?,?)", -1, &u, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "keep the restore that waits");
    int bits = !opts ? 0
                     : (opts->deleted_only ? PENDING_DELETED_ONLY : 0) | (opts->with_deleted ? PENDING_WITH_DELETED : 0) |
                           (opts->keep_both ? PENDING_KEEP_BOTH : 0);
    sqlite3_bind_int64(u, 1, source_id);
    sqlite3_bind_text(u, 2, rel_prefix ? rel_prefix : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(u, 3, dest, -1, SQLITE_STATIC);
    sqlite3_bind_int(u, 4, bits);
    sqlite3_bind_int64(u, 5, offer_root);
    sqlite3_bind_int64(u, 6, bd_now_ms());
    bd_status s = sqlite3_step(u) == SQLITE_DONE ? BD_OK : bd_fail_db(cat, "keep the restore that waits");
    sqlite3_finalize(u);
    return s;
}

bd_status bd_restore_pending_get(bd_catalog *cat, bd_restore_pending *out)
{
    memset(out, 0, sizeof(*out));
    sqlite3_stmt *q;
    /* No table: no restore ever waited in this database. */
    if (sqlite3_prepare_v2(cat->db, "SELECT source_id, rel_path, dest, opts, offer_root, saved_ms FROM restore_pending WHERE id=1",
                           -1, &q, NULL) != SQLITE_OK)
        return bd_fail(cat, BD_ERR_NOT_FOUND, "no restore waits");
    bd_status s = bd_fail(cat, BD_ERR_NOT_FOUND, "no restore waits");
    if (sqlite3_step(q) == SQLITE_ROW) {
        int bits = sqlite3_column_int(q, 3);
        out->source_id = sqlite3_column_int64(q, 0);
        out->rel_prefix = bd_strdup(sqlite3_column_text(q, 1) ? (const char *)sqlite3_column_text(q, 1) : "");
        out->dest = bd_strdup(sqlite3_column_text(q, 2) ? (const char *)sqlite3_column_text(q, 2) : "");
        out->opts.deleted_only = (bits & PENDING_DELETED_ONLY) != 0;
        out->opts.with_deleted = (bits & PENDING_WITH_DELETED) != 0;
        out->opts.keep_both = (bits & PENDING_KEEP_BOTH) != 0;
        out->offer_root = sqlite3_column_int64(q, 4);
        out->saved_ms = sqlite3_column_int64(q, 5);
        s = BD_OK;
        if (!out->rel_prefix || !out->dest) {
            bd_restore_pending_free(out);
            s = BD_ERR_NOMEM;
        }
    }
    sqlite3_finalize(q);
    return s;
}

void bd_restore_pending_free(bd_restore_pending *p)
{
    if (!p) return;
    free(p->rel_prefix);
    free(p->dest);
    memset(p, 0, sizeof(*p));
}

bd_status bd_restore_pending_clear(bd_catalog *cat)
{
    /* Tables never made have nothing to forget. */
    bd_exec(cat, "DELETE FROM restore_pending");
    bd_exec(cat, "DELETE FROM restore_checked");
    bd_exec(cat, "DELETE FROM settings WHERE key='restore_checked_for'");
    return BD_OK;
}

/* ---- Where a restore goes ---------------------------------------------- */

static int name_is(const char *path, const char *name)
{
    const char *base = path;
    for (const char *p = path; *p; p++)
        if ((*p == '/' || *p == '\\') && p[1]) base = p + 1;
    size_t n = strlen(name);
    for (size_t i = 0; i < n; i++) {
        char a = base[i], b = name[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
        if (b >= 'a' && b <= 'z') b = (char)(b - 'a' + 'A');
        if (a != b) return 0;
    }
    return base[n] == '\0' || ((base[n] == '/' || base[n] == '\\') && base[n + 1] == '\0');
}

static int is_file_at(const char *dir, const char *rel)
{
    char *p = bd_path_join(dir, rel);
    bd_stat_t st;
    int is = p && bd_stat(p, &st) == 0 && st.is_file;
    free(p);
    return is;
}

int bd_restore_dest_check(bd_catalog *cat, int64_t source_id, const char *dest, char **what)
{
    if (what) *what = NULL;
    if (!dest || !*dest) return 0;
    /* A backup root itself: fine when the files go back to their own
     * place in it. */
    int flat = restore_flat(cat, source_id);
    int64_t same = 0;
    if (bd_root_check(cat, dest, 0, 0, &same) == BD_OK && same) {
        sqlite3_stmt *q;
        int active = 0;
        char *path = NULL;
        if (sqlite3_prepare_v2(cat->db, "SELECT active, path FROM sources WHERE id=?", -1, &q, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(q, 1, same);
            if (sqlite3_step(q) == SQLITE_ROW) {
                active = sqlite3_column_int(q, 0);
                path = bd_strdup(sqlite3_column_text(q, 1) ? (const char *)sqlite3_column_text(q, 1) : dest);
            }
            sqlite3_finalize(q);
        }
        if (active && !(flat && (source_id == 0 || source_id == same))) {
            if (what) *what = path;
            else free(path);
            return 1;
        }
        free(path);
        if (active) return 0;
    }
    char *holder = bd_root_holding(cat, dest);
    if (holder) {
        if (what) *what = holder;
        else free(holder);
        return 1;
    }
    /* A drive's BRODALF folder, or a database's folder in it
     * (BRODALF\<id>\BRODALF.media), on any drive. */
    char *p = bd_strdup(dest);
    int found = 0;
    for (int tries = 0; p && !found && tries < 256; tries++) {
        char *mine = name_is(p, BD_MEDIA_DIR) ? bd_path_join(p, cat->uuid) : NULL;
        found = is_file_at(p, BD_MEDIA_FILE) || (mine && is_file_at(mine, BD_MEDIA_FILE));
        free(mine);
        if (!found && !path_up(p)) break;
    }
    if (found && what) *what = p;
    else free(p);
    return found ? 2 : 0;
}
