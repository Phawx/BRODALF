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

typedef struct {
    int64_t node_id, node_size, node_mtime;
    int64_t version_id, version_no, version_size;
    char *rel, *source_path, *source_name, *hash;
} backup_item;

static void free_items(backup_item *items, size_t n)
{
    for (size_t i = 0; i < n; i++) {
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

/* Where a copy goes when its usual place on the drive holds something
 * else (an older version, or a file BRODALF does not know): next to it
 * under .versions, as <dir>/<stem>.v<N><ext>, or .v<N>-2 and so on when
 * that name is taken too. Nothing on a drive is ever overwritten, moved or
 * deleted. NULL when the drive cannot be read. */
static char *alongside_path(bd_catalog *cat, bd_store *store, const char *dest_rel, int64_t version_no)
{
    for (int k = 1; k < 1000; k++) {
        char tag[48];
        if (k == 1) snprintf(tag, sizeof(tag), "v%lld", (long long)version_no);
        else snprintf(tag, sizeof(tag), "v%lld-%d", (long long)version_no, k);
        char *rel = versions_rel_path(dest_rel, tag);
        if (!rel) return NULL;
        bd_remote_stat st;
        int r = store->ops->stat(store, rel, &st);
        if (r == 1 && !path_claimed(cat, store->media_id, rel)) return rel;
        free(rel);
        if (r < 0) return NULL;
    }
    return NULL;
}

static int record_copy(bd_catalog *cat, int64_t version_id, int64_t media_id, const char *rel, const bd_remote_stat *st)
{
    sqlite3_stmt *u;
    if (sqlite3_prepare_v2(cat->db,
                           "INSERT INTO copies(version_id, media_id, path_on_media, stored_size, stored_mtime_ns,"
                           " written_ms, last_quick_check_ms, state) VALUES(?,?,?,?,?,?,?,'ok')"
                           " ON CONFLICT(version_id, media_id) DO UPDATE SET path_on_media=excluded.path_on_media,"
                           " stored_size=excluded.stored_size, stored_mtime_ns=excluded.stored_mtime_ns,"
                           " written_ms=excluded.written_ms, last_full_check_ms=NULL,"
                           " last_quick_check_ms=excluded.last_quick_check_ms, state='ok'",
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

/* Write src into the store's staging file for rel, hashing as it goes, and
 * keep it only if the hash matches. It gets the source's modified time
 * (mtime_ns > 0). The file is not synced yet: that is done for a group of
 * copies at once (sync_group). 0 on success with the
 * staged local path in *staged_out, 1 on hash mismatch, 2 if the job was
 * stopped, -1 on error. */
static int stage_verified(bd_store *store, const char *src, const uint8_t *key, const char *rel, const char *expected,
                          int64_t mtime_ns, char **staged_out, int64_t *bytes)
{
    *staged_out = NULL;
    char *staged = store->ops->staging_path(store, rel);
    if (!staged) return -1;
    FILE *out = bd_fopen(staged, "wb");
    if (!out) { free(staged); return -1; }
    char hash[BD_HASH_HEX_LEN + 1];
    int rc = bd_hash_copy(src, NULL, out, key, hash, bytes);
    /* After the last write. The copy's own time on the drive is recorded
     * after the rename (record_copy), so the quick check compares like with
     * like whether this worked or not. */
    if (rc == 0 && mtime_ns > 0) bd_set_mtime(out, mtime_ns);
    if (fclose(out) != 0 && rc == 0) rc = -1;
    if (rc == 0 && strcmp(hash, expected) != 0) rc = 1;
    if (rc != 0) { bd_remove(staged); free(staged); return rc == -4 ? 2 : rc > 0 ? 1 : -1; }
    *staged_out = staged;
    return 0;
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
    char *orig, *dest_rel, *tmp_rel, *local_tmp;
    int failed, alongside;
    int64_t bytes;
} pending_copy;

static void pending_free(pending_copy *p)
{
    free(p->orig);
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

/* Check the file and write its copy to <dest>.brodalf-tmp. 1 if *p now
 * holds a copy to finish; 0 if the file is done with (failed, or already
 * on the drive). */
static int backup_start(bd_catalog *cat, bd_store *store, const uint8_t *key, backup_item *it, pending_copy *p,
                        int64_t files_done, bd_backup_stats *stats, bd_log_fn log, void *log_ctx)
{
    int64_t media_id = store->media_id;
    memset(p, 0, sizeof(*p));
    p->it = it;
    p->orig = bd_path_join(it->source_path, it->rel);
    const char *src = p->orig;
    char *main_rel = bd_sprintf("%s/%s%s", it->source_name, it->rel, key ? BD_SEALED_SUFFIX : "");
    if (!src || !main_rel) { free(main_rel); stats->files_failed++; goto drop; }

    bd_stat_t sst;
    if (bd_stat(src, &sst) != 0 || !sst.is_file) {
        free(main_rel);
        stats->files_failed++;
        bd_logf(log, log_ctx, "source file missing, scan again: %s", src);
        goto drop;
    }
    /* Reading a cloud file that is not on this PC would download it. */
    if (sst.online_only) {
        free(main_rel);
        stats->files_online_only++;
        bd_logf(log, log_ctx, "online-only (not on this PC), not copied: %s", src);
        goto drop;
    }
    if (sst.size != it->node_size || sst.mtime_ns != it->node_mtime) {
        int r = rehash_changed(cat, media_id, it, src, &sst, stats, log, log_ctx);
        if (r != 0) {
            free(main_rel);
            if (r > 0) stats->files_already_there++;
            else stats->files_failed++;
            goto drop;
        }
    }
    bd_report_file(cat, files_done, it->rel, it->version_size);

    /* What is at the usual place now, and what the catalog says is there. */
    bd_remote_stat dst_st;
    int found = store->ops->stat(store, main_rel, &dst_st);
    if (found < 0) {
        stats->files_failed++;
        bd_logf(log, log_ctx, "cannot look at %s: %s", main_rel, store->err);
        free(main_rel);
        goto drop;
    }
    int64_t row_copy = 0, row_version = 0;
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(cat->db, "SELECT id, version_id FROM copies WHERE media_id=? AND path_on_media=?", -1, &q, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(q, 1, media_id);
        sqlite3_bind_text(q, 2, main_rel, -1, SQLITE_STATIC);
        if (sqlite3_step(q) == SQLITE_ROW) {
            row_copy = sqlite3_column_int64(q, 0);
            row_version = sqlite3_column_int64(q, 1);
        }
        sqlite3_finalize(q);
    }

    int use_main = 0;
    if (found == 1) {
        if (row_copy && row_version != it->version_id) {
            /* The catalog says another version's copy is here, but it is
             * gone from the drive. Keep the record, marked missing; the
             * new copy goes alongside. */
            sqlite3_stmt *u;
            if (sqlite3_prepare_v2(cat->db, "UPDATE copies SET state='missing' WHERE id=? AND state<>'missing'", -1, &u, NULL) == SQLITE_OK) {
                sqlite3_bind_int64(u, 1, row_copy);
                sqlite3_step(u);
                if (sqlite3_changes(cat->db) > 0) bd_logf(log, log_ctx, "an older copy is gone from the drive: %s", main_rel);
                sqlite3_finalize(u);
            }
        } else {
            use_main = 1;
        }
    } else if (!row_copy && !tracked_by_case(cat, media_id, it, main_rel)) {
        /* A file the catalog does not know, probably written by a backup
         * whose catalog was never saved. If it is exactly this version,
         * adopt it instead of copying. */
        char hash[BD_HASH_HEX_LEN + 1];
        char *local = NULL;
        int is_temp = 0;
        int same = bd_store_fetch(store, main_rel, &local, &is_temp) == 0 &&
                   bd_hash_copy(local, key, NULL, NULL, hash, NULL) == 0 && strcmp(hash, it->hash) == 0;
        bd_store_release(local, is_temp);
        if (same) {
            if (record_copy(cat, it->version_id, media_id, main_rel, &dst_st) == 0) stats->files_already_there++;
            else stats->files_failed++;
            free(main_rel);
            goto drop;
        }
        bd_logf(log, log_ctx, "a file BRODALF did not write is at %s; it is left alone", main_rel);
    }
    if (use_main) {
        p->dest_rel = main_rel;
    } else {
        p->dest_rel = alongside_path(cat, store, main_rel, it->version_no);
        p->alongside = 1;
        free(main_rel);
        if (!p->dest_rel) {
            stats->files_failed++;
            bd_logf(log, log_ctx, "cannot find a free name on the drive for %s: %s", src, store->err);
            goto drop;
        }
    }
    p->tmp_rel = bd_sprintf("%s" BD_TMP_MARKER, p->dest_rel);
    if (!p->tmp_rel) { stats->files_failed++; goto drop; }

    /* The new copy goes to <dest>.brodalf-tmp first. */
    int rc = stage_verified(store, src, key, p->tmp_rel, it->hash, it->node_mtime, &p->local_tmp, &p->bytes);
    if (rc == 2) goto drop; /* stopped halfway: the partial copy is gone */
    if (rc == 1) {
        stats->files_failed++;
        bd_logf(log, log_ctx, "changed while copying, scan again: %s", src);
        goto drop;
    }
    if (rc != 0) {
        stats->files_failed++;
        if (bd_open_was_in_use()) {
            stats->files_in_use++;
            bd_note_in_use(cat, p->orig, it->node_size);
            bd_logf(log, log_ctx, "in use by another program, skipped: %s", src);
        } else {
            bd_logf(log, log_ctx, "cannot copy %s to the drive%s%s", src, store->err[0] ? ": " : "", store->err);
        }
        store->ops->remove(store, p->tmp_rel); /* only its own half-written temp file */
        goto drop;
    }
    return 1;

drop:
    pending_free(p);
    return 0;
}

/* The copy's data is on the drive: give it its name and record it. */
static void backup_finish(bd_catalog *cat, bd_store *store, pending_copy *p, bd_backup_stats *stats, bd_log_fn log,
                          void *log_ctx)
{
    const backup_item *it = p->it;
    const char *dest_rel = p->dest_rel, *tmp_rel = p->tmp_rel;
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
        if (mv == 1) bd_logf(log, log_ctx, "a file appeared at %s while copying; left alone, back up again", dest_rel);
        else bd_logf(log, log_ctx, "cannot finish writing %s: %s", dest_rel, store->err);
        store->ops->remove(store, tmp_rel);
        return;
    }
    if (store->ops->stat(store, dest_rel, &new_st) != 0 ||
        record_copy(cat, it->version_id, store->media_id, dest_rel, &new_st) != 0) {
        stats->files_failed++;
        bd_logf(log, log_ctx, "copied %s but could not record it: %s", dest_rel, sqlite3_errmsg(cat->db));
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

/* The files a backup copies (nodes n, their current versions v): no good
 * copy on this drive (?1). ?2 backup root (0: all), ?3 after this node id.
 * Roots no longer backed up are left out. */
#define TODO_WHERE                                                                                      \
    " n.is_dir=0 AND n.deleted=0 AND (?2=0 OR n.source_id=?2) AND n.id>?3 AND " BD_SQL_ACTIVE("n")      \
    " AND NOT EXISTS(SELECT 1 FROM copies c WHERE c.version_id=v.id AND c.media_id=?1 AND c.state='ok')"

int64_t bd_reserve_bytes(bd_catalog *cat)
{
    return (int64_t)bd_option_get(cat, "reserve_mb") * 1024 * 1024;
}

bd_status bd_backup(bd_catalog *cat, int64_t media_id, int64_t source_id,
                    bd_backup_stats *stats, bd_log_fn log, void *log_ctx)
{
    bd_backup_stats local;
    if (!stats) stats = &local;
    memset(stats, 0, sizeof(*stats));
    bd_cancel_reset(cat);

    if (bd_media_lost(cat, media_id))
        return bd_fail(cat, BD_ERR_INVALID, "this drive is marked lost or broken, so nothing is backed up to it");

    const uint8_t *key = NULL;
    if (bd_media_encrypted(cat, media_id)) {
        if (!cat->have_key) return bd_fail(cat, BD_ERR_PASSPHRASE, "enter the passphrase to back up to an encrypted drive");
        key = cat->key;
    }
    /* A root called .versions would land in the versions tree. */
    bd_status fs = bd_sources_fix_names(cat, log, log_ctx);
    if (fs != BD_OK) return fs;
    bd_store *store = bd_store_open(cat, media_id);
    if (!store) return BD_ERR_NOT_FOUND;

    /* How much fits, leaving the reserve free for the catalog's copies.
     * Unknown space means no limit. */
    int64_t space_total = 0, space_free = -1, reserve = 0;
    if (bd_test_free_bytes >= 0) space_free = bd_test_free_bytes;
    else if (store->ops->space(store, &space_total, &space_free) != 0) space_free = -1;
    else {
        reserve = bd_reserve_bytes(cat);
        bd_media_note_space(cat, media_id, space_total, space_free, "before backup");
    }

    int64_t job = bd_job_start(cat, "backup", media_id);
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT n.id, n.rel_path, n.size, n.mtime_ns, s.path, s.name, v.id, v.hash, v.version_no, v.size"
                           " FROM nodes n JOIN sources s ON s.id=n.source_id JOIN versions v ON v.id=n.current_version_id"
                           " WHERE" TODO_WHERE " ORDER BY n.id LIMIT 256",
                           -1, &q, NULL) != SQLITE_OK) {
        bd_job_finish(cat, job, "failed", 0, 0, 0);
        bd_store_close(store);
        return bd_fail_db(cat, "list files to back up");
    }

    /* Totals for the progress bar: what this drive does not hold yet. */
    sqlite3_stmt *t;
    int64_t todo_files = 0, todo_bytes = 0;
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT COUNT(*), COALESCE(SUM(v.size),0) FROM nodes n JOIN versions v ON v.id=n.current_version_id"
                           " WHERE" TODO_WHERE,
                           -1, &t, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(t, 1, media_id);
        sqlite3_bind_int64(t, 2, source_id);
        sqlite3_bind_int64(t, 3, 0);
        if (sqlite3_step(t) == SQLITE_ROW) {
            todo_files = sqlite3_column_int64(t, 0);
            todo_bytes = sqlite3_column_int64(t, 1);
        }
        sqlite3_finalize(t);
    }
    bd_progress_begin(cat, "backup", todo_files, todo_bytes);

    backup_item items[BATCH];
    pending_copy group[BATCH];
    int64_t last_id = 0;
    int cancelled = 0;
    while (!cancelled) {
        size_t n = 0;
        sqlite3_reset(q);
        sqlite3_bind_int64(q, 1, media_id);
        sqlite3_bind_int64(q, 2, source_id);
        sqlite3_bind_int64(q, 3, last_id);
        while (sqlite3_step(q) == SQLITE_ROW) {
            last_id = sqlite3_column_int64(q, 0);
            stats->files_considered++;
            backup_item *it = &items[n++];
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
        }
        sqlite3_reset(q);
        if (n == 0) break;
        /* One catalog transaction per batch; copies are synced in groups
         * (at most SYNC_BYTES of data) before they are renamed and recorded. */
        int in_txn = bd_exec(cat, "BEGIN") == 0;
        size_t ng = 0;
        int64_t group_bytes = 0;
        for (size_t i = 0; i < n; i++) {
            /* Stopping: what is written so far is still finished below. */
            if (bd_cancelled(cat)) { cancelled = 1; break; }
            if (space_free >= 0) {
                /* Sealed copies are a little bigger than the file. */
                int64_t need = items[i].version_size + (key ? items[i].version_size / 1024 + 4096 : 0);
                if (need > space_free - reserve) {
                    if (!stats->files_no_room)
                        bd_logf(log, log_ctx, "the drive is full; files that do not fit wait for another drive");
                    stats->files_no_room++;
                    stats->bytes_no_room += items[i].version_size;
                    continue;
                }
            }
            /* Copies waiting in the group count as done for the progress bar. */
            int64_t done = stats->files_copied + stats->files_failed + stats->files_no_room + (int64_t)ng;
            if (!backup_start(cat, store, key, &items[i], &group[ng], done, stats, log, log_ctx)) continue;
            if (space_free >= 0) space_free -= group[ng].bytes;
            group_bytes += group[ng].bytes;
            ng++;
            if (group_bytes >= SYNC_BYTES) {
                sync_group(cat, store, group, ng, stats, log, log_ctx);
                ng = 0;
                group_bytes = 0;
            }
        }
        /* A stop during the batch's last copy is still a stop. */
        if (!cancelled && bd_cancelled(cat)) cancelled = 1;
        sync_group(cat, store, group, ng, stats, log, log_ctx);
        if (in_txn && bd_exec(cat, "COMMIT") != 0) {
            bd_logf(log, log_ctx, "cannot record the copies: %s", sqlite3_errmsg(cat->db));
            bd_exec(cat, "ROLLBACK");
        }
        free_items(items, n);
    }
    sqlite3_finalize(q);

    int64_t total_bytes = 0, free_bytes = 0;
    if (store->ops->space(store, &total_bytes, &free_bytes) == 0)
        bd_media_note_space(cat, media_id, total_bytes, free_bytes, "after backup");
    bd_store_close(store);
    bd_report(cat, stats->files_copied, NULL, 1);
    bd_progress_end(cat);
    bd_job_finish(cat, job, cancelled ? "stopped" : stats->files_failed ? "partial" : "done", stats->files_copied,
               stats->files_failed, stats->bytes_copied);
    if (cancelled) return bd_fail_cancelled(cat);
    /* The drive gets the catalog as it is now, under a new name: what it
     * holds can be found from the drive alone. */
    if (bd_catalog_copy_to_media(cat, media_id) != BD_OK)
        bd_logf(log, log_ctx, "the catalog could not be copied to the drive: %s", bd_catalog_error(cat));
    return BD_OK;
}

/* ---- Before a backup: what it would do ---------------------------------- */

#define MASS_CHANGE_MIN_FILES 50

/* Files this drive holds (a good copy of some version of it) that are still
 * in their folder, and whether the newest version it holds is still the
 * file's contents: the checksum is the test, not size or time. */
#define HELD_CTE                                                                                        \
    "WITH h AS (SELECT v.node_id AS nid, MAX(v.version_no) AS vno FROM copies c"                        \
    " JOIN versions v ON v.id=c.version_id WHERE c.media_id=?1 AND c.state='ok' GROUP BY v.node_id)"

bd_status bd_backup_preview(bd_catalog *cat, int64_t media_id, int64_t source_id, bd_backup_preview_info *out)
{
    memset(out, 0, sizeof(*out));
    out->free_bytes = -1;
    sqlite3_stmt *q;

    /* What it copies, and of that what is changed: an older version of the
     * file is on the drive, so the new one goes alongside it. */
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT COUNT(*), COALESCE(SUM(t.size),0), COALESCE(SUM(t.older),0) FROM (SELECT v.size AS size,"
                           " EXISTS(SELECT 1 FROM copies oc JOIN versions ov ON ov.id=oc.version_id"
                           "   WHERE ov.node_id=n.id AND oc.media_id=?1) AS older"
                           " FROM nodes n JOIN versions v ON v.id=n.current_version_id WHERE" TODO_WHERE ") t",
                           -1, &q, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "work out what the backup would copy");
    sqlite3_bind_int64(q, 1, media_id);
    sqlite3_bind_int64(q, 2, source_id);
    sqlite3_bind_int64(q, 3, 0);
    if (sqlite3_step(q) == SQLITE_ROW) {
        out->files_to_copy = sqlite3_column_int64(q, 0);
        out->bytes_to_copy = sqlite3_column_int64(q, 1);
        out->files_changed = sqlite3_column_int64(q, 2);
        out->files_new = out->files_to_copy - out->files_changed;
    }
    sqlite3_finalize(q);

    /* How much of the drive held, and how much of that changed since. A
     * large share at once is what ransomware encrypting the folders looks
     * like: the user should see the list before backing that up. */
    if (sqlite3_prepare_v2(cat->db,
                           HELD_CTE " SELECT COUNT(*), COALESCE(SUM(hv.id IS NOT n.current_version_id),0)"
                           " FROM h JOIN nodes n ON n.id=h.nid JOIN versions hv ON hv.node_id=h.nid AND hv.version_no=h.vno"
                           " WHERE n.is_dir=0 AND n.deleted=0 AND (?2=0 OR n.source_id=?2) AND " BD_SQL_ACTIVE("n"),
                           -1, &q, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(q, 1, media_id);
        sqlite3_bind_int64(q, 2, source_id);
        if (sqlite3_step(q) == SQLITE_ROW) {
            out->files_held = sqlite3_column_int64(q, 0);
            out->held_changed = sqlite3_column_int64(q, 1);
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
    char *root = bd_connected_root(cat, media_id);
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
    if (free_bytes >= 0) {
        int64_t need = out->bytes_to_copy;
        if (bd_media_encrypted(cat, media_id)) need += need / 1024 + out->files_to_copy * 4096;
        int64_t room = free_bytes - out->reserve_bytes;
        out->bytes_short = need > room ? need - room : 0;
    }
    return BD_OK;
}

bd_status bd_list_changed_on_media(bd_catalog *cat, int64_t media_id, bd_changed_fn fn, void *ctx)
{
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(cat->db,
                           HELD_CTE " SELECT n.id, s.name, n.rel_path, hv.version_no, cv.version_no, hv.size, cv.size, cv.first_seen_ms"
                           " FROM h JOIN nodes n ON n.id=h.nid JOIN sources s ON s.id=n.source_id"
                           " JOIN versions hv ON hv.node_id=h.nid AND hv.version_no=h.vno"
                           " JOIN versions cv ON cv.id=n.current_version_id"
                           " WHERE n.is_dir=0 AND n.deleted=0 AND hv.id<>cv.id AND " BD_SQL_ACTIVE("n")
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
        if (fn(ctx, &c) != 0) break;
    }
    sqlite3_finalize(q);
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

static void note_conflict(bd_catalog *cat, const char *source_name, const char *rel, const char *dest, int64_t want_size,
                          int64_t want_mtime, const bd_stat_t *have)
{
    sqlite3_stmt *u;
    if (sqlite3_prepare_v2(cat->db,
                           "INSERT INTO temp.restore_conflicts(source_name, rel_path, dest, want_size, want_mtime_ns, have_size, have_mtime_ns)"
                           " VALUES(?,?,?,?,?,?,?)", -1, &u, NULL) != SQLITE_OK)
        return;
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
} restore_file_item;

/* Restore one version to dest. Never writes over anything: a file already
 * there with the right contents is left as it is (already there), any
 * other file there is left alone and listed (bd_list_restore_conflicts).
 * 1 when the job was stopped. */
static int restore_file(bd_catalog *cat, bd_store **store, sqlite3_stmt *find, sqlite3_stmt *any, const restore_file_item *it,
                        const char *dest, bd_restore_stats *stats, bd_log_fn log, void *log_ctx)
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
        note_conflict(cat, it->source_name, it->rel, dest, it->size, it->mtime, &dst);
        bd_logf(log, log_ctx, "something else is already at %s; left alone", dest);
        return 0;
    }
    sqlite3_reset(find);
    sqlite3_bind_int64(find, 1, it->version_id);
    if (sqlite3_step(find) != SQLITE_ROW) {
        sqlite3_reset(any);
        sqlite3_bind_int64(any, 1, it->version_id);
        if (sqlite3_step(any) == SQLITE_ROW) stats->files_offline++;
        else stats->files_no_copy++;
        sqlite3_reset(any);
        return 0;
    }
    int sealed = sqlite3_column_int(find, 2);
    int64_t media_id = sqlite3_column_int64(find, 1);
    char *rel = bd_strdup((const char *)sqlite3_column_text(find, 0));
    sqlite3_reset(find);
    if (sealed && !cat->have_key) {
        stats->files_need_passphrase++;
        free(rel);
        return 0;
    }
    if (!*store || (*store)->media_id != media_id) {
        bd_store_close(*store);
        *store = bd_store_open(cat, media_id);
    }
    char *src = NULL;
    int src_temp = 0;
    if (!rel || !*store || bd_store_fetch(*store, rel, &src, &src_temp) != 0) {
        stats->files_failed++;
        bd_logf(log, log_ctx, "cannot read %s%s%s", rel ? rel : it->rel, *store && (*store)->err[0] ? ": " : "",
                *store ? (*store)->err : "");
        free(rel);
        return 0;
    }
    char *tmp = NULL;
    int64_t bytes = 0;
    int stopped = 0;
    int rc = copy_verified(src, sealed ? cat->key : NULL, dest, NULL, it->hash, it->mtime, it->btime, &tmp, &bytes);
    if (rc == 0 && bd_rename_noreplace(tmp, dest) != 0) {
        bd_remove(tmp);
        bd_stat_t now;
        if (bd_stat(dest, &now) == 0) {
            stats->files_conflict++;
            note_conflict(cat, it->source_name, it->rel, dest, it->size, it->mtime, &now);
            rc = 2;
        } else {
            rc = -1;
        }
    }
    if (rc == 0) {
        stats->files_restored++;
        stats->bytes_restored += bytes;
    } else if (rc == -4) {
        stopped = 1; /* the half-written temp file is gone */
    } else if (rc != 2) {
        stats->files_failed++;
        bd_logf(log, log_ctx, rc == 1 ? "copy on the drive is damaged: %s" : "cannot restore %s", rel);
    }
    free(tmp);
    bd_store_release(src, src_temp);
    free(rel);
    return stopped;
}

#define FIND_COPY_SQL                                                                                   \
    "SELECT c.path_on_media, c.media_id, m.encrypted FROM copies c JOIN temp.connected k ON k.media_id=c.media_id" \
    " JOIN media m ON m.id=c.media_id WHERE c.version_id=? AND c.state='ok'"                            \
    " ORDER BY m.encrypted, m.kind='drive' DESC LIMIT 1"

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

    sqlite3_stmt *q = NULL, *find = NULL, *any = NULL;
    bd_store *store = NULL;
    bd_status s = BD_OK;
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT n.id, n.rel_path, n.is_dir, s.name, v.id, v.hash, v.size,"
                           " CASE WHEN n.deleted=0 THEN n.mtime_ns ELSE v.mtime_ns END, n.btime_ns FROM nodes n"
                           " JOIN sources s ON s.id=n.source_id LEFT JOIN versions v ON v.id=n.current_version_id"
                           " WHERE" RESTORE_WHERE " AND n.id>?3 ORDER BY n.id LIMIT 256",
                           -1, &q, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(cat->db, FIND_COPY_SQL, -1, &find, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(cat->db, "SELECT 1 FROM copies WHERE version_id=? AND state='ok' LIMIT 1", -1, &any, NULL) != SQLITE_OK) {
        s = bd_fail_db(cat, "prepare restore");
        goto done;
    }

    /* Totals for the progress bar. */
    sqlite3_stmt *t;
    int64_t total_files = 0, total_bytes = 0;
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT COUNT(*), COALESCE(SUM(v.size),0) FROM nodes n JOIN versions v ON v.id=n.current_version_id"
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

    typedef struct { int64_t id, version_id, size, mtime, btime; int is_dir; char *rel, *source_name, *hash; } item;
    item items[BATCH];
    int64_t last_id = 0;
    int cancelled = 0;
    while (!cancelled) {
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
        }
        sqlite3_reset(q);
        if (n == 0) break;

        for (size_t i = 0; i < n && !cancelled; i++) {
            item *it = &items[i];
            if (bd_cancelled(cat)) { cancelled = 1; break; }
            char *dest = it->rel ? restore_dest(dest_root, flat, it->source_name, it->rel) : NULL;
            if (!dest) { stats->files_failed++; continue; }
            if (it->is_dir) {
                bd_mkdirs(dest);
                free(dest);
                continue;
            }
            bd_report_file(cat, files_seen++, it->rel, it->size);
            if (!it->version_id || !it->hash) { stats->files_no_copy++; free(dest); continue; }
            restore_file_item rf = {it->version_id, it->size, it->mtime, it->btime, it->hash, it->source_name, it->rel};
            if (restore_file(cat, &store, find, any, &rf, dest, stats, log, log_ctx)) cancelled = 1;
            free(dest);
        }
        for (size_t i = 0; i < n; i++) {
            free(items[i].rel);
            free(items[i].source_name);
            free(items[i].hash);
        }
    }
    if (stats->files_conflict)
        bd_logf(log, log_ctx, "%lld files were left alone because something else was already there",
                (long long)stats->files_conflict);
    bd_report(cat, files_seen, NULL, 1);
    bd_progress_end(cat);
    bd_job_finish(cat, job, cancelled ? "stopped" : stats->files_failed || stats->files_offline ? "partial" : "done",
                  stats->files_restored, stats->files_failed, stats->bytes_restored);
    if (cancelled) s = bd_fail_cancelled(cat);
done:
    sqlite3_finalize(q);
    sqlite3_finalize(find);
    sqlite3_finalize(any);
    bd_store_close(store);
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
    sqlite3_stmt *q = NULL, *find = NULL, *any = NULL;
    bd_store *store = NULL;
    bd_status s = BD_OK;
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT v.hash, v.size, CASE WHEN v.id=n.current_version_id AND n.deleted=0 THEN n.mtime_ns ELSE v.mtime_ns END,"
                           " n.btime_ns, n.rel_path, s.name FROM versions v JOIN nodes n ON n.id=v.node_id"
                           " JOIN sources s ON s.id=n.source_id WHERE v.id=?", -1, &q, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(cat->db, FIND_COPY_SQL, -1, &find, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(cat->db, "SELECT 1 FROM copies WHERE version_id=? AND state='ok' LIMIT 1", -1, &any, NULL) != SQLITE_OK) {
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
    char *dest = bd_path_join(dest_folder, bd_rel_basename(rf.rel));
    if (!dest) { s = BD_ERR_NOMEM; goto done; }
    bd_mkdirs(dest_folder);
    bd_progress_begin(cat, "restore", 1, rf.size);
    bd_report_file(cat, 0, rf.rel, rf.size);
    int64_t job = bd_job_start(cat, "restore", 0);
    int stopped = restore_file(cat, &store, find, any, &rf, dest, stats, log, log_ctx);
    bd_report(cat, 1, NULL, 1);
    bd_progress_end(cat);
    bd_job_finish(cat, job, stopped ? "stopped" : stats->files_restored || stats->files_already_there ? "done" : "partial",
                  stats->files_restored, stats->files_failed, stats->bytes_restored);
    if (stopped) s = bd_fail_cancelled(cat);
    free(dest);
done:
    sqlite3_finalize(q);
    sqlite3_finalize(find);
    sqlite3_finalize(any);
    bd_store_close(store);
    return s;
}

bd_status bd_list_restore_conflicts(bd_catalog *cat, bd_conflict_fn fn, void *ctx)
{
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT source_name, rel_path, dest, want_size, want_mtime_ns, have_size, have_mtime_ns"
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
                           " JOIN versions v ON v.id=n.current_version_id"
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
