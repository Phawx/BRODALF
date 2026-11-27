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

#include <stdlib.h>
#include <string.h>

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

/* Copy src to a temp file next to dest while hashing; keep the temp file
 * only if the hash matches. src_key decrypts the source, dst_key encrypts
 * the copy. Returns 0 and sets *tmp_out on success, 1 on hash mismatch or
 * a damaged encrypted source, -1 on I/O error, -4 if the job was stopped
 * (the temp file is removed in every case but success). */
static int copy_verified(const char *src, const uint8_t *src_key, const char *dest, const uint8_t *dst_key,
                         const char *expected, int64_t mtime_ns, int64_t btime_ns, char **tmp_out, int64_t *bytes)
{
    *tmp_out = NULL;
    char *tmp = bd_sprintf("%s" BD_TMP_MARKER, dest);
    if (!tmp || ensure_parent_dir(dest) != 0) { free(tmp); return -1; }
    FILE *out = bd_fopen(tmp, "wb");
    if (!out) { free(tmp); return -1; }
    char hash[BD_HASH_HEX_LEN + 1];
    int rc = bd_hash_copy(src, src_key, out, dst_key, hash, bytes);
    if (rc == -3) rc = 1;
    /* The original's modified and created times; a file that cannot take
     * them is still restored. */
    if (rc == 0 && mtime_ns > 0) bd_set_times(out, mtime_ns, btime_ns);
    if (rc == 0 && bd_fsync(out) != 0) rc = -1;
    if (fclose(out) != 0) rc = -1;
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

/* The files a backup to one drive copies (nodes n, their current versions
 * v): no good copy on this drive (?1), and for an extra place only starred
 * files. ?2 backup root (0: all), ?3 after this node id. Roots no longer
 * backed up are left out. */
#define TODO_WHERE                                                                                      \
    " n.is_dir=0 AND n.deleted=0 AND (?2=0 OR n.source_id=?2) AND n.id>?3 AND " BD_SQL_ACTIVE("n")      \
    " AND NOT EXISTS(SELECT 1 FROM copies c WHERE c.version_id=v.id AND c.media_id=?1 AND c.state='ok')" \
    " AND (NOT EXISTS(SELECT 1 FROM media sm WHERE sm.id=?1 AND sm.secondary<>0) OR " BD_SQL_MARKED("stars", "n") ")"

/* The same for the drives in temp.bk_dests: what at least one of them
 * needs. ?1 backup root, ?2 after this node id. */
#define TODO_MANY_WHERE                                                                                 \
    " n.is_dir=0 AND n.deleted=0 AND (?1=0 OR n.source_id=?1) AND n.id>?2 AND " BD_SQL_ACTIVE("n")      \
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

    /* Folders set to be encrypted need the key even on a plain drive. */
    if (!cat->have_key) {
        sqlite3_stmt *q;
        int sealed_todo = 0;
        if (sqlite3_prepare_v2(cat->db,
                               "SELECT 1 FROM nodes n JOIN versions v ON v.id=n.current_version_id WHERE" TODO_MANY_WHERE
                               " AND " BD_SQL_MARKED("encrypt_paths", "n") " LIMIT 1",
                               -1, &q, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(q, 1, source_id);
            sqlite3_bind_int64(q, 2, 0);
            sealed_todo = sqlite3_step(q) == SQLITE_ROW;
            sqlite3_finalize(q);
        }
        if (sealed_todo) {
            free(dests);
            return bd_fail(cat, BD_ERR_PASSPHRASE, "enter the passphrase: some files to back up are in folders set to be encrypted");
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
    sqlite3_stmt *q, *have = NULL;
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT n.id, n.rel_path, n.size, n.mtime_ns, s.path, s.name, v.id, v.hash, v.version_no, v.size,"
                           " " BD_SQL_MARKED("stars", "n") ", " BD_SQL_MARKED("encrypt_paths", "n")
                           " FROM nodes n JOIN sources s ON s.id=n.source_id JOIN versions v ON v.id=n.current_version_id"
                           " WHERE" TODO_MANY_WHERE " ORDER BY n.id LIMIT 256",
                           -1, &q, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(cat->db, "SELECT 1 FROM copies WHERE version_id=? AND media_id=? AND state='ok'", -1, &have, NULL) != SQLITE_OK) {
        for (int i = 0; i < n; i++) { bd_job_finish(cat, dests[i].job, "failed", 0, 0, 0); dest_close(&dests[i]); }
        sqlite3_finalize(have);
        free(dests);
        return bd_fail_db(cat, "list files to back up");
    }

    /* Totals for the progress bar: each file is read once, whatever the
     * number of drives. */
    sqlite3_stmt *t;
    int64_t todo_files = 0, todo_bytes = 0;
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT COUNT(*), COALESCE(SUM(v.size),0) FROM nodes n JOIN versions v ON v.id=n.current_version_id"
                           " WHERE" TODO_MANY_WHERE,
                           -1, &t, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(t, 1, source_id);
        sqlite3_bind_int64(t, 2, 0);
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
    bd_exec(cat, "DELETE FROM temp.bk_dests");
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

    if (sqlite3_prepare_v2(cat->db, "SELECT MIN(COALESCE(last_scan_ms,0)) FROM sources WHERE (?1=0 OR id=?1) AND active<>0", -1, &q, NULL) == SQLITE_OK) {
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

/* The files a restore brings back: the current version of every file still
 * in its folder, or with ?4 only the files deleted from it, each as it was
 * when last seen. */
#define RESTORE_WHERE                                                                                       \
    " (CASE WHEN ?4<>0 THEN n.deleted<>0 ELSE n.deleted=0 END)"                                             \
    " AND (?1=0 OR n.source_id=?1) AND (?2='' OR n.rel_path=?2 OR substr(n.rel_path,1,length(?2)+1)=?2||'/')"

/* SQL: version vid can be restored from: it has a good copy on a drive not
 * marked lost or broken, or on one plugged in now. */
#define RESTORABLE(vid)                                                                                 \
    "EXISTS(SELECT 1 FROM copies rc WHERE rc.version_id=" vid " AND rc.state='ok' AND (" BD_SQL_USABLE("rc") \
    " OR EXISTS(SELECT 1 FROM temp.connected rk WHERE rk.media_id=rc.media_id)))"

/* SQL: the version of node n a restore brings back, the latest backed-up
 * state: the current version if it can be restored from, else the newest
 * other version that can (its newest contents were never backed up, as
 * when ransomware or a bad edit changed it after the last backup), else
 * the current version. */
#define RESTORE_VERSION                                                                                 \
    "(CASE WHEN " RESTORABLE("n.current_version_id") " THEN n.current_version_id ELSE"                  \
    " COALESCE((SELECT ov.id FROM versions ov WHERE ov.node_id=n.id AND " RESTORABLE("ov.id")            \
    "  ORDER BY ov.version_no DESC LIMIT 1), n.current_version_id) END)"

/* Is there a good copy of the version (?1), or of any version of the file
 * (?2), anywhere: then it is on drives not plugged in. */
#define ANY_COPY_SQL                                                                                    \
    "SELECT 1 FROM copies c JOIN versions v ON v.id=c.version_id WHERE c.state='ok' AND (c.version_id=?1 OR v.node_id=?2)" \
    " LIMIT 1"

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

typedef struct {
    int64_t version_id, size, mtime, btime;
    const char *hash, *source_name, *rel;
    int64_t node_id; /* 0: only this version counts in telling offline from never backed up */
} restore_file_item;

/* What one restore job keeps between files. */
typedef struct {
    bd_store *store;           /* the drive read last */
    sqlite3_stmt *find, *any;  /* FIND_COPY_SQL; is there a good copy anywhere */
    bd_copy_marks marks;       /* copies found missing or damaged, recorded at the end */
    char gone[128];            /* the drive that went, if one did */
} restore_run;

typedef struct {
    char *rel, *label;
    int64_t copy_id, media_id;
    int sealed;
} restore_copy;

/* Restore one version to dest. Its good copies on the connected drives and
 * extra places are tried in the order of FIND_COPY_SQL until one reads back
 * with the right checksum; one found damaged or missing on the way is
 * marked so (when the job ends, if its drive is still there) and the next
 * is tried. Never writes over anything: a file already there with the
 * right contents is left as it is (already there), any other file there
 * is left alone and listed (bd_list_restore_conflicts). 0 go on, 1 the
 * job was stopped, 2 a drive went (unplugged or not answering; its label
 * is in r->gone): the restore stops. */
static int restore_file(bd_catalog *cat, restore_run *r, const restore_file_item *it, const char *dest,
                        bd_restore_stats *stats, bd_log_fn log, void *log_ctx)
{
    bd_stat_t dst;
    if (bd_stat(dest, &dst) == 0) {
        if (dst.is_file && dst.size == it->size) {
            char have[BD_HASH_HEX_LEN + 1];
            if (bd_hash_file(dest, NULL, have, NULL) == 0 && strcmp(have, it->hash) == 0) {
                stats->files_already_there++;
                return 0;
            }
        }
        stats->files_conflict++;
        note_conflict(cat, it->version_id, it->source_name, it->rel, dest, it->size, it->mtime, &dst);
        bd_logf(log, log_ctx, "something else is already at %s; left alone", dest);
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
        return 0;
    }
    int result = 0, done = 0, tried = 0, locked = 0;
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
        int src_temp = 0, rc = -1;
        int64_t bytes = 0;
        int got = r->store ? bd_store_fetch(r->store, c->rel, &src, &src_temp) : -1;
        if (got == 0) {
            rc = copy_verified(src, c->sealed ? cat->key : NULL, dest, NULL, it->hash, it->mtime, it->btime, &tmp, &bytes);
            if (rc == 0 && bd_rename_noreplace(tmp, dest) != 0) {
                bd_remove(tmp);
                bd_stat_t now;
                if (bd_stat(dest, &now) == 0) {
                    stats->files_conflict++;
                    note_conflict(cat, it->version_id, it->source_name, it->rel, dest, it->size, it->mtime, &now);
                    rc = 2;
                } else {
                    rc = -1;
                }
            }
        }
        bd_store_release(src, src_temp);
        free(tmp);
        if (rc == 0) {
            stats->files_restored++;
            stats->bytes_restored += bytes;
            if (tried > 1) bd_logf(log, log_ctx, "restored %s from the copy on \"%s\"", dest, c->label);
            done = 1;
        } else if (rc == -4) {
            result = 1; /* stopped; the half-written temp file is gone */
            done = 1;
        } else if (rc == 2) {
            done = 1; /* something appeared at dest meanwhile: left alone */
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
            bd_logf(log, log_ctx, "cannot restore %s from the copy on \"%s\"%s%s%s", dest, c->label,
                    r->store && r->store->err[0] ? ": " : "", r->store ? r->store->err : "",
                    i + 1 < nc ? "; trying the next copy" : "");
        }
    }
    if (nomem) {
        stats->files_failed++;
        bd_logf(log, log_ctx, "out of memory: cannot restore %s", dest);
    } else if (!done) {
        if (tried) {
            stats->files_failed++;
            bd_logf(log, log_ctx, "no copy of %s could be read back right, so it was not restored", dest);
        } else if (locked) {
            stats->files_need_passphrase++;
        }
    }
    for (size_t i = 0; i < nc; i++) {
        free(copies[i].rel);
        free(copies[i].label);
    }
    free(copies);
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

static bd_status restore_run_begin(bd_catalog *cat, restore_run *r)
{
    memset(r, 0, sizeof(*r));
    if (sqlite3_prepare_v2(cat->db, FIND_COPY_SQL, -1, &r->find, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(cat->db, ANY_COPY_SQL, -1, &r->any, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "prepare restore");
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
    sqlite3_finalize(r->find);
    sqlite3_finalize(r->any);
    bd_store_close(r->store);
    r->find = r->any = NULL;
    r->store = NULL;
}

static bd_status restore_gone(bd_catalog *cat, const restore_run *r)
{
    return bd_fail(cat, BD_ERR_DRIVE_GONE,
                   "\"%s\" stopped answering or was unplugged during the restore. What was restored is kept, and nothing on"
                   " the drive was marked missing or damaged; plug it in again and run the same restore again",
                   r->gone);
}

bd_status bd_restore_ex(bd_catalog *cat, int64_t source_id, const char *rel_prefix, const char *dest_root,
                        const bd_restore_opts *opts, bd_restore_stats *stats, bd_log_fn log, void *log_ctx)
{
    int deleted_only = opts ? opts->deleted_only : 0;
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
    bd_status s = restore_run_begin(cat, &run);
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
        sqlite3_bind_int(t, 4, deleted_only);
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
    int cancelled = 0, gone = 0;
    while (!cancelled && !gone) {
        size_t n = 0;
        sqlite3_reset(q);
        sqlite3_bind_int64(q, 1, source_id);
        sqlite3_bind_text(q, 2, prefix, -1, SQLITE_STATIC);
        sqlite3_bind_int64(q, 3, last_id);
        sqlite3_bind_int(q, 4, deleted_only);
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

        for (size_t i = 0; i < n && !cancelled && !gone; i++) {
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
    }
    if (stats->files_conflict)
        bd_logf(log, log_ctx, "%lld %s left alone because something else was already there",
                (long long)stats->files_conflict, stats->files_conflict == 1 ? "file was" : "files were");
    bd_report(cat, files_seen, NULL, 1);
    bd_progress_end(cat);
    restore_run_end(cat, &run, log, log_ctx);
    bd_job_finish(cat, job,
                  gone ? "failed" : cancelled ? "stopped" : stats->files_failed || stats->files_offline ? "partial" : "done",
                  stats->files_restored, stats->files_failed, stats->bytes_restored);
    if (gone) s = restore_gone(cat, &run);
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
    bd_restore_stats local;
    if (!stats) stats = &local;
    memset(stats, 0, sizeof(*stats));
    bd_cancel_reset(cat);
    bd_exec(cat, "DELETE FROM temp.restore_conflicts");
    sqlite3_stmt *q = NULL;
    restore_run run;
    bd_status s = restore_run_begin(cat, &run);
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
    restore_run_end(cat, &run, log, log_ctx);
    bd_job_finish(cat, job,
                  r == 2 ? "failed" : r == 1 ? "stopped" : stats->files_restored || stats->files_already_there ? "done" : "partial",
                  stats->files_restored, stats->files_failed, stats->bytes_restored);
    if (r == 2) s = restore_gone(cat, &run);
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
    int deleted_only = opts ? opts->deleted_only : 0;
    int flat = restore_flat(cat, source_id);
    if (files_total) *files_total = 0;
    if (files_no_copy) *files_no_copy = 0;
    char *prefix = bd_strdup(rel_prefix ? rel_prefix : "");
    if (!prefix) return BD_ERR_NOMEM;
    for (char *p = prefix; *p; p++) if (*p == '\\') *p = '/';
    size_t plen = strlen(prefix);
    while (plen > 0 && prefix[plen - 1] == '/') prefix[--plen] = '\0';

    bd_status s = BD_OK;
    sqlite3_stmt *mq = NULL, *fq = NULL, *cq = NULL;
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

    sqlite3_bind_int64(fq, 1, source_id);
    sqlite3_bind_text(fq, 2, prefix, -1, SQLITE_STATIC);
    sqlite3_bind_int64(fq, 3, 0);
    sqlite3_bind_int(fq, 4, deleted_only);
    while (sqlite3_step(fq) == SQLITE_ROW) {
        int64_t size = sqlite3_column_int64(fq, 3);
        if (dest_root && *dest_root) {
            char *dest = restore_dest(dest_root, flat, (const char *)sqlite3_column_text(fq, 2),
                                      (const char *)sqlite3_column_text(fq, 1));
            bd_stat_t st;
            int there = dest && bd_stat(dest, &st) == 0 && st.is_file && st.size == size;
            free(dest);
            if (there) continue;
        }
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
        if (!any && files_no_copy) (*files_no_copy)++;
        nf++;
    }
    if (files_total) *files_total = (int64_t)nf;

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
