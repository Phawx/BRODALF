/* Backup to and restore from connected drives.
 *
 * On a drive, files live under BRODALF/<catalog uuid>/<source name>/<path>.
 * When a file changes, the new copy is written to a temp file and checked
 * against the catalog hash first. Only then is the previous copy moved to
 * .versions/<source name>/<folder>/<name>.v<N><ext> and the new copy renamed
 * into place, so a failed write never loses a good copy.
 *
 * On an encrypted drive each copy is sealed with the catalog's master key
 * and named <name>.bdenc; names stay readable. */
#include "store.h"

#include <stdlib.h>
#include <string.h>

#define BATCH 256

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

/* Move whatever sits at dest_rel into .versions. Tracked copies keep their
 * catalog row with the new path. Returns 0 on success. */
static int move_aside(bd_catalog *cat, bd_store *store, const char *dest_rel,
                      int64_t tracked_copy_id, int64_t tracked_version_no, bd_log_fn log, void *log_ctx)
{
    char tag[64];
    if (tracked_copy_id) snprintf(tag, sizeof(tag), "v%lld", (long long)tracked_version_no);
    else snprintf(tag, sizeof(tag), "untracked-%lld", (long long)bd_now_ms());

    char *vrel = versions_rel_path(dest_rel, tag);
    int rc = vrel ? store->ops->move(store, dest_rel, vrel, 0) : -1;
    if (rc == 1) {
        /* Already a file with that name in .versions: keep both. */
        char *alt_tag = bd_sprintf("%s-%lld", tag, (long long)bd_now_ms());
        free(vrel);
        vrel = alt_tag ? versions_rel_path(dest_rel, alt_tag) : NULL;
        free(alt_tag);
        rc = vrel ? store->ops->move(store, dest_rel, vrel, 0) : -1;
    }
    if (rc == 0 && tracked_copy_id) {
        sqlite3_stmt *u;
        if (sqlite3_prepare_v2(cat->db, "UPDATE copies SET path_on_media=? WHERE id=?", -1, &u, NULL) == SQLITE_OK) {
            sqlite3_bind_text(u, 1, vrel, -1, SQLITE_STATIC);
            sqlite3_bind_int64(u, 2, tracked_copy_id);
            if (sqlite3_step(u) != SQLITE_DONE) rc = -1;
            sqlite3_finalize(u);
        } else {
            rc = -1;
        }
    }
    if (rc == 0 && !tracked_copy_id) bd_logf(log, log_ctx, "moved an unknown file out of the way: %s -> %s", dest_rel, vrel);
    free(vrel);
    return rc == 0 ? 0 : -1;
}

static int record_copy(bd_catalog *cat, int64_t version_id, int64_t media_id, const char *rel, const bd_remote_stat *st)
{
    sqlite3_stmt *u;
    if (sqlite3_prepare_v2(cat->db,
                           "INSERT INTO copies(version_id, media_id, path_on_media, stored_size, stored_mtime_ns, stored_rev,"
                           " written_ms, last_quick_check_ms, state) VALUES(?,?,?,?,?,?,?,?,'ok')"
                           " ON CONFLICT(version_id, media_id) DO UPDATE SET path_on_media=excluded.path_on_media,"
                           " stored_size=excluded.stored_size, stored_mtime_ns=excluded.stored_mtime_ns,"
                           " stored_rev=excluded.stored_rev, written_ms=excluded.written_ms,"
                           " last_quick_check_ms=excluded.last_quick_check_ms, state='ok'",
                           -1, &u, NULL) != SQLITE_OK)
        return -1;
    int64_t now = bd_now_ms();
    sqlite3_bind_int64(u, 1, version_id);
    sqlite3_bind_int64(u, 2, media_id);
    sqlite3_bind_text(u, 3, rel, -1, SQLITE_STATIC);
    sqlite3_bind_int64(u, 4, st->size);
    sqlite3_bind_int64(u, 5, st->mtime_ns);
    sqlite3_bind_text(u, 6, st->rev, -1, SQLITE_STATIC);
    sqlite3_bind_int64(u, 7, now);
    sqlite3_bind_int64(u, 8, now);
    int rc = sqlite3_step(u) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(u);
    return rc;
}

/* Copy src to a temp file next to dest while hashing; keep the temp file
 * only if the hash matches. src_key decrypts the source, dst_key encrypts
 * the copy. Returns 0 and sets *tmp_out on success, 1 on hash mismatch or
 * a damaged encrypted source, -1 on I/O error. */
static int copy_verified(const char *src, const uint8_t *src_key, const char *dest, const uint8_t *dst_key,
                         const char *expected, char **tmp_out, int64_t *bytes)
{
    *tmp_out = NULL;
    char *tmp = bd_sprintf("%s" BD_TMP_MARKER, dest);
    if (!tmp || ensure_parent_dir(dest) != 0) { free(tmp); return -1; }
    FILE *out = bd_fopen(tmp, "wb");
    if (!out) { free(tmp); return -1; }
    char hash[BD_HASH_HEX_LEN + 1];
    int rc = bd_hash_copy(src, src_key, out, dst_key, hash, bytes);
    if (rc == -3) rc = 1;
    if (rc == 0 && bd_fsync(out) != 0) rc = -1;
    if (fclose(out) != 0) rc = -1;
    if (rc == 0 && strcmp(hash, expected) != 0) rc = 1;
    if (rc != 0) { bd_remove(tmp); free(tmp); return rc; }
    *tmp_out = tmp;
    return 0;
}

/* Write src into the store's staging file for rel, hashing as it goes, and
 * upload it only if the hash matches. 0 on success (rel now holds the copy
 * and *st describes it), 1 on hash mismatch, -1 on error. */
static int put_verified(bd_store *store, const char *src, const uint8_t *key, const char *rel, const char *expected,
                        bd_remote_stat *st, int64_t *bytes)
{
    char *staged = store->ops->staging_path(store, rel);
    if (!staged) return -1;
    FILE *out = bd_fopen(staged, "wb");
    if (!out) { free(staged); return -1; }
    char hash[BD_HASH_HEX_LEN + 1];
    int rc = bd_hash_copy(src, NULL, out, key, hash, bytes);
    if (rc == 0 && bd_fsync(out) != 0) rc = -1;
    if (fclose(out) != 0) rc = -1;
    if (rc == 0 && strcmp(hash, expected) != 0) rc = 1;
    if (rc != 0) { bd_remove(staged); free(staged); return rc > 0 ? 1 : -1; }
    rc = store->ops->upload(store, staged, rel, st);
    free(staged);
    return rc == 0 ? 0 : -1;
}

static int64_t start_job(bd_catalog *cat, const char *kind, int64_t media_id)
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
    return id;
}

static void finish_job(bd_catalog *cat, int64_t job_id, const char *status, int64_t done, int64_t failed, int64_t bytes)
{
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

static void backup_one(bd_catalog *cat, bd_store *store, const uint8_t *key, const backup_item *it,
                       bd_backup_stats *stats, bd_log_fn log, void *log_ctx)
{
    int64_t media_id = store->media_id;
    bd_report(cat, "backup", stats->files_copied, stats->bytes_copied, it->rel, 0);
    char *src = bd_path_join(it->source_path, it->rel);
    char *dest_rel = bd_sprintf("%s/%s%s", it->source_name, it->rel, key ? BD_SEALED_SUFFIX : "");
    char *tmp_rel = dest_rel ? bd_sprintf("%s" BD_TMP_MARKER, dest_rel) : NULL;
    if (!src || !tmp_rel) { stats->files_failed++; goto done; }

    bd_stat_t sst;
    if (bd_stat(src, &sst) != 0 || !sst.is_file) {
        stats->files_failed++;
        bd_logf(log, log_ctx, "source file missing, scan again: %s", src);
        goto done;
    }
    if (sst.size != it->node_size || sst.mtime_ns != it->node_mtime) {
        stats->files_failed++;
        bd_logf(log, log_ctx, "changed since the last scan, scan again: %s", src);
        goto done;
    }

    /* What is at the destination now? */
    int64_t tracked_copy = 0, tracked_version = 0, tracked_version_no = 0;
    bd_remote_stat dst_st;
    int found = store->ops->stat(store, dest_rel, &dst_st);
    if (found < 0) {
        stats->files_failed++;
        bd_logf(log, log_ctx, "cannot look at %s: %s", dest_rel, store->err);
        goto done;
    }
    int dest_exists = found == 0;
    if (dest_exists) {
        sqlite3_stmt *q;
        if (sqlite3_prepare_v2(cat->db,
                               "SELECT c.id, v.id, v.version_no FROM copies c JOIN versions v ON v.id=c.version_id"
                               " WHERE c.media_id=? AND c.path_on_media=?",
                               -1, &q, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(q, 1, media_id);
            sqlite3_bind_text(q, 2, dest_rel, -1, SQLITE_STATIC);
            if (sqlite3_step(q) == SQLITE_ROW) {
                tracked_copy = sqlite3_column_int64(q, 0);
                tracked_version = sqlite3_column_int64(q, 1);
                tracked_version_no = sqlite3_column_int64(q, 2);
            }
            sqlite3_finalize(q);
        }
        if (!tracked_copy) {
            /* Probably written by a backup whose catalog was never saved.
             * If it is exactly this version, adopt it instead of copying. */
            char hash[BD_HASH_HEX_LEN + 1];
            char *local = NULL;
            int is_temp = 0;
            int same = bd_store_fetch(store, dest_rel, &local, &is_temp) == 0 &&
                       bd_hash_copy(local, key, NULL, NULL, hash, NULL) == 0 && strcmp(hash, it->hash) == 0;
            bd_store_release(local, is_temp);
            if (same) {
                if (record_copy(cat, it->version_id, media_id, dest_rel, &dst_st) == 0) stats->files_already_there++;
                else stats->files_failed++;
                goto done;
            }
        }
    }

    if (!dest_exists) {
        /* Rows for other versions that claim this path point at a file that
         * is gone from the drive; drop them so the new copy can be recorded. */
        sqlite3_stmt *d;
        if (sqlite3_prepare_v2(cat->db, "DELETE FROM copies WHERE media_id=? AND path_on_media=? AND version_id<>?", -1, &d, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(d, 1, media_id);
            sqlite3_bind_text(d, 2, dest_rel, -1, SQLITE_STATIC);
            sqlite3_bind_int64(d, 3, it->version_id);
            sqlite3_step(d);
            if (sqlite3_changes(cat->db) > 0) bd_logf(log, log_ctx, "an older copy of %s is gone from the drive", dest_rel);
            sqlite3_finalize(d);
        }
    }

    /* The new copy goes to <dest>.brodalf-tmp first. */
    int64_t bytes = 0;
    bd_remote_stat new_st;
    int rc = put_verified(store, src, key, tmp_rel, it->hash, &new_st, &bytes);
    if (rc == 1) {
        stats->files_failed++;
        bd_logf(log, log_ctx, "changed while copying, scan again: %s", src);
        goto done;
    }
    if (rc != 0) {
        stats->files_failed++;
        bd_logf(log, log_ctx, "cannot copy %s to the drive%s%s", src, store->err[0] ? ": " : "", store->err);
        store->ops->remove(store, tmp_rel);
        goto done;
    }

    /* The new copy is safe in the temp file. Keep the old one if it is
     * another version (or unknown); replace it if it is a damaged copy of
     * this same version. */
    if (dest_exists && tracked_version != it->version_id) {
        if (move_aside(cat, store, dest_rel, tracked_copy, tracked_version_no, log, log_ctx) != 0) {
            stats->files_failed++;
            bd_logf(log, log_ctx, "cannot move the previous copy of %s into .versions; left it in place", dest_rel);
            store->ops->remove(store, tmp_rel);
            goto done;
        }
        if (tracked_copy) stats->versions_moved++;
    }
    if (store->ops->move(store, tmp_rel, dest_rel, 1) != 0) {
        stats->files_failed++;
        bd_logf(log, log_ctx, "cannot finish writing %s: %s", dest_rel, store->err);
        store->ops->remove(store, tmp_rel);
        goto done;
    }
    if (store->ops->stat(store, dest_rel, &new_st) != 0 ||
        record_copy(cat, it->version_id, media_id, dest_rel, &new_st) != 0) {
        stats->files_failed++;
        bd_logf(log, log_ctx, "copied %s but could not record it: %s", dest_rel, sqlite3_errmsg(cat->db));
        goto done;
    }
    stats->files_copied++;
    stats->bytes_copied += bytes;

done:
    free(src);
    free(dest_rel);
    free(tmp_rel);
}

bd_status bd_backup(bd_catalog *cat, int64_t media_id, int64_t source_id,
                    bd_backup_stats *stats, bd_log_fn log, void *log_ctx)
{
    bd_backup_stats local;
    if (!stats) stats = &local;
    memset(stats, 0, sizeof(*stats));

    const uint8_t *key = NULL;
    if (bd_media_encrypted(cat, media_id)) {
        if (!cat->have_key) return bd_fail(cat, BD_ERR_PASSPHRASE, "enter the passphrase to back up to an encrypted drive");
        key = cat->key;
    }
    bd_store *store = bd_store_open(cat, media_id);
    if (!store) return BD_ERR_NOT_FOUND;

    int64_t job = start_job(cat, "backup", media_id);
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT n.id, n.rel_path, n.size, n.mtime_ns, s.path, s.name, v.id, v.hash, v.version_no, v.size,"
                           " EXISTS(SELECT 1 FROM copies c WHERE c.version_id=v.id AND c.media_id=?1 AND c.state='ok')"
                           " FROM nodes n JOIN sources s ON s.id=n.source_id JOIN versions v ON v.id=n.current_version_id"
                           " WHERE n.is_dir=0 AND n.deleted=0 AND (?2=0 OR n.source_id=?2) AND n.id>?3"
                           " ORDER BY n.id LIMIT 256",
                           -1, &q, NULL) != SQLITE_OK) {
        bd_store_close(store);
        return bd_fail_db(cat, "list files to back up");
    }

    backup_item items[BATCH];
    int64_t last_id = 0;
    for (;;) {
        size_t n = 0, rows = 0;
        sqlite3_reset(q);
        sqlite3_bind_int64(q, 1, media_id);
        sqlite3_bind_int64(q, 2, source_id);
        sqlite3_bind_int64(q, 3, last_id);
        while (sqlite3_step(q) == SQLITE_ROW) {
            rows++;
            last_id = sqlite3_column_int64(q, 0);
            stats->files_considered++;
            if (sqlite3_column_int(q, 10)) { stats->files_already_there++; continue; }
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
        if (rows == 0) break;
        for (size_t i = 0; i < n; i++) backup_one(cat, store, key, &items[i], stats, log, log_ctx);
        free_items(items, n);
    }
    sqlite3_finalize(q);

    int64_t total_bytes = 0, free_bytes = 0;
    if (store->ops->space(store, &total_bytes, &free_bytes) == 0) {
        sqlite3_stmt *u;
        if (sqlite3_prepare_v2(cat->db, "UPDATE media SET total_bytes=?, free_bytes=? WHERE id=?", -1, &u, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(u, 1, total_bytes);
            sqlite3_bind_int64(u, 2, free_bytes);
            sqlite3_bind_int64(u, 3, media_id);
            sqlite3_step(u);
            sqlite3_finalize(u);
        }
    }
    bd_store_close(store);
    bd_report(cat, "backup", stats->files_copied, stats->bytes_copied, NULL, 1);
    finish_job(cat, job, stats->files_failed ? "partial" : "done", stats->files_copied, stats->files_failed, stats->bytes_copied);
    return BD_OK;
}

bd_status bd_restore(bd_catalog *cat, int64_t source_id, const char *rel_prefix, const char *dest_root,
                     bd_restore_stats *stats, bd_log_fn log, void *log_ctx)
{
    bd_restore_stats local;
    if (!stats) stats = &local;
    memset(stats, 0, sizeof(*stats));
    if (!rel_prefix) rel_prefix = "";
    char *prefix = bd_strdup(rel_prefix);
    if (!prefix) return BD_ERR_NOMEM;
    for (char *p = prefix; *p; p++) if (*p == '\\') *p = '/';
    size_t plen = strlen(prefix);
    while (plen > 0 && prefix[plen - 1] == '/') prefix[--plen] = '\0';

    sqlite3_stmt *q = NULL, *find = NULL, *any = NULL;
    bd_store *store = NULL;
    bd_status s = BD_OK;
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT n.id, n.rel_path, n.is_dir, s.name, v.id, v.hash FROM nodes n"
                           " JOIN sources s ON s.id=n.source_id LEFT JOIN versions v ON v.id=n.current_version_id"
                           " WHERE n.deleted=0 AND (?1=0 OR n.source_id=?1) AND n.id>?3"
                           " AND (?2='' OR n.rel_path=?2 OR substr(n.rel_path,1,length(?2)+1)=?2||'/')"
                           " ORDER BY n.id LIMIT 256",
                           -1, &q, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(cat->db,
                           "SELECT c.path_on_media, c.media_id, m.encrypted FROM copies c JOIN temp.connected k ON k.media_id=c.media_id"
                           " JOIN media m ON m.id=c.media_id WHERE c.version_id=? AND c.state='ok'"
                           " ORDER BY m.encrypted, m.kind='drive' DESC LIMIT 1",
                           -1, &find, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(cat->db, "SELECT 1 FROM copies WHERE version_id=? AND state='ok' LIMIT 1", -1, &any, NULL) != SQLITE_OK) {
        s = bd_fail_db(cat, "prepare restore");
        goto done;
    }

    typedef struct { int64_t id, version_id; int is_dir; char *rel, *source_name, *hash; } item;
    item items[BATCH];
    int64_t last_id = 0;
    for (;;) {
        size_t n = 0;
        sqlite3_reset(q);
        sqlite3_bind_int64(q, 1, source_id);
        sqlite3_bind_text(q, 2, prefix, -1, SQLITE_STATIC);
        sqlite3_bind_int64(q, 3, last_id);
        while (n < BATCH && sqlite3_step(q) == SQLITE_ROW) {
            item *it = &items[n++];
            it->id = last_id = sqlite3_column_int64(q, 0);
            it->rel = bd_strdup((const char *)sqlite3_column_text(q, 1));
            it->is_dir = sqlite3_column_int(q, 2);
            it->source_name = bd_strdup((const char *)sqlite3_column_text(q, 3));
            it->version_id = sqlite3_column_int64(q, 4);
            it->hash = sqlite3_column_type(q, 5) == SQLITE_NULL ? NULL : bd_strdup((const char *)sqlite3_column_text(q, 5));
        }
        sqlite3_reset(q);
        if (n == 0) break;

        for (size_t i = 0; i < n; i++) {
            item *it = &items[i];
            char *rel_out = bd_sprintf("%s/%s", it->source_name, it->rel);
            char *dest = rel_out ? bd_path_join(dest_root, rel_out) : NULL;
            free(rel_out);
            if (!dest) { stats->files_failed++; continue; }
            bd_report(cat, "restore", stats->files_restored, stats->bytes_restored, it->rel, 0);
            if (it->is_dir) {
                bd_mkdirs(dest);
                free(dest);
                continue;
            }
            if (!it->version_id || !it->hash) { stats->files_no_copy++; free(dest); continue; }

            sqlite3_reset(find);
            sqlite3_bind_int64(find, 1, it->version_id);
            if (sqlite3_step(find) != SQLITE_ROW) {
                sqlite3_reset(any);
                sqlite3_bind_int64(any, 1, it->version_id);
                if (sqlite3_step(any) == SQLITE_ROW) stats->files_offline++;
                else stats->files_no_copy++;
                free(dest);
                continue;
            }
            int sealed = sqlite3_column_int(find, 2);
            int64_t media_id = sqlite3_column_int64(find, 1);
            char *rel = bd_strdup((const char *)sqlite3_column_text(find, 0));
            sqlite3_reset(find);
            if (sealed && !cat->have_key) {
                stats->files_need_passphrase++;
                free(rel);
                free(dest);
                continue;
            }
            if (!store || store->media_id != media_id) {
                bd_store_close(store);
                store = bd_store_open(cat, media_id);
            }
            char *src = NULL;
            int src_temp = 0;
            if (!rel || !store || bd_store_fetch(store, rel, &src, &src_temp) != 0) {
                stats->files_failed++;
                bd_logf(log, log_ctx, "cannot read %s%s%s", rel ? rel : it->rel, store && store->err[0] ? ": " : "",
                        store ? store->err : "");
                free(rel);
                free(dest);
                continue;
            }

            char *tmp = NULL;
            int64_t bytes = 0;
            int rc = copy_verified(src, sealed ? cat->key : NULL, dest, NULL, it->hash, &tmp, &bytes);
            if (rc == 0 && bd_rename_replace(tmp, dest) != 0) { bd_remove(tmp); rc = -1; }
            if (rc == 0) {
                stats->files_restored++;
                stats->bytes_restored += bytes;
            } else {
                stats->files_failed++;
                bd_logf(log, log_ctx, rc == 1 ? "copy on the drive is damaged: %s" : "cannot restore %s", rel);
            }
            free(tmp);
            bd_store_release(src, src_temp);
            free(rel);
            free(dest);
        }
        for (size_t i = 0; i < n; i++) {
            free(items[i].rel);
            free(items[i].source_name);
            free(items[i].hash);
        }
    }
done:
    sqlite3_finalize(q);
    sqlite3_finalize(find);
    sqlite3_finalize(any);
    bd_store_close(store);
    free(prefix);
    return s;
}
