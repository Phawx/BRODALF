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
/* Room left on a drive for its file system and the catalog copy. */
#define MIN_RESERVE (16 * 1024 * 1024)

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

/* A file that was moved or renamed in its folder shows up as a new file
 * with the content of one that is now deleted. When that deleted file's
 * copy is on this drive, unchanged since BRODALF last saw it good, move the
 * copy to the new name instead of copying the whole file again. Checked by
 * hash (the catalog's) and by the copy's size and time (or cloud content
 * hash) on the drive. 1 if the copy was moved. */
static int move_existing(bd_catalog *cat, bd_store *store, const backup_item *it, const uint8_t *key,
                         bd_backup_stats *stats, bd_log_fn log, void *log_ctx)
{
    char *dest_rel = bd_sprintf("%s/%s%s", it->source_name, it->rel, key ? BD_SEALED_SUFFIX : "");
    if (!dest_rel) return 0;
    int moved = 0;
    int64_t copy_id = 0, stored_size = 0, stored_mtime = 0;
    char *old_rel = NULL, *old_rev = NULL;
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT c.id, c.path_on_media, c.stored_size, c.stored_mtime_ns, COALESCE(c.stored_rev,'')"
                           " FROM copies c JOIN versions v ON v.id=c.version_id JOIN nodes n ON n.id=v.node_id"
                           " WHERE c.media_id=?1 AND c.state='ok' AND v.hash=?2 AND v.size=?3 AND n.deleted=1"
                           " AND v.id=n.current_version_id AND substr(c.path_on_media,1,length(?4)+1)<>?4||'/'"
                           " ORDER BY c.id LIMIT 1",
                           -1, &q, NULL) != SQLITE_OK) {
        free(dest_rel);
        return 0;
    }
    sqlite3_bind_int64(q, 1, store->media_id);
    sqlite3_bind_text(q, 2, it->hash, -1, SQLITE_STATIC);
    sqlite3_bind_int64(q, 3, it->version_size);
    sqlite3_bind_text(q, 4, BD_VERSIONS_DIR, -1, SQLITE_STATIC);
    if (sqlite3_step(q) == SQLITE_ROW) {
        copy_id = sqlite3_column_int64(q, 0);
        old_rel = bd_strdup((const char *)sqlite3_column_text(q, 1));
        stored_size = sqlite3_column_int64(q, 2);
        stored_mtime = sqlite3_column_int64(q, 3);
        old_rev = bd_strdup((const char *)sqlite3_column_text(q, 4));
    }
    sqlite3_finalize(q);
    if (!copy_id || !old_rel || !old_rev) goto done;

    bd_remote_stat st;
    if (store->ops->stat(store, dest_rel, &st) != 1) goto done; /* something is already there */
    if (store->ops->stat(store, old_rel, &st) != 0 || st.size != stored_size) goto done;
    if (store->is_local ? st.mtime_ns != stored_mtime : (st.rev[0] && strcmp(st.rev, old_rev) != 0)) goto done;
    if (store->ops->move(store, old_rel, dest_rel, 0) != 0) goto done;

    sqlite3_stmt *d, *u;
    int ok = 0;
    if (sqlite3_prepare_v2(cat->db, "DELETE FROM copies WHERE version_id=? AND media_id=?", -1, &d, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(d, 1, it->version_id);
        sqlite3_bind_int64(d, 2, store->media_id);
        ok = sqlite3_step(d) == SQLITE_DONE;
        sqlite3_finalize(d);
    }
    if (ok && sqlite3_prepare_v2(cat->db, "UPDATE copies SET version_id=?, path_on_media=? WHERE id=?", -1, &u, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(u, 1, it->version_id);
        sqlite3_bind_text(u, 2, dest_rel, -1, SQLITE_STATIC);
        sqlite3_bind_int64(u, 3, copy_id);
        ok = sqlite3_step(u) == SQLITE_DONE;
        sqlite3_finalize(u);
    } else {
        ok = 0;
    }
    if (!ok) {
        /* Put it back where the catalog says it is. */
        store->ops->move(store, dest_rel, old_rel, 0);
        goto done;
    }
    moved = 1;
    stats->files_moved++;
    stats->bytes_moved += it->version_size;
    if (stats->files_moved == 1) bd_logf(log, log_ctx, "moved or renamed files are moved on the drive, not copied again");

done:
    free(dest_rel);
    free(old_rel);
    free(old_rev);
    return moved;
}

static void backup_one(bd_catalog *cat, bd_store *store, const uint8_t *key, const backup_item *it,
                       bd_backup_stats *stats, bd_log_fn log, void *log_ctx)
{
    int64_t media_id = store->media_id;
    bd_report(cat, stats->files_copied + stats->files_moved + stats->files_failed + stats->files_no_room, it->rel, 0);
    char *orig = bd_path_join(it->source_path, it->rel);
    /* A file in use is read from its shadow copy, when one was taken. */
    char *staged = orig ? bd_substitute_for(cat, orig) : NULL;
    char *src = staged ? staged : orig;
    char *dest_rel = bd_sprintf("%s/%s%s", it->source_name, it->rel, key ? BD_SEALED_SUFFIX : "");
    char *tmp_rel = dest_rel ? bd_sprintf("%s" BD_TMP_MARKER, dest_rel) : NULL;
    if (!src || !tmp_rel) { stats->files_failed++; goto done; }

    bd_stat_t sst;
    if (bd_stat(src, &sst) != 0 || !sst.is_file) {
        stats->files_failed++;
        bd_logf(log, log_ctx, "source file missing, scan again: %s", src);
        goto done;
    }
    if (sst.size != it->node_size || (!staged && sst.mtime_ns != it->node_mtime)) {
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
        if (bd_open_was_in_use()) {
            stats->files_in_use++;
            bd_note_in_use(cat, orig, it->node_size);
            bd_logf(log, log_ctx, "in use by another program, not copied: %s", src);
        } else {
            bd_logf(log, log_ctx, "cannot copy %s to the drive%s%s", src, store->err[0] ? ": " : "", store->err);
        }
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
    free(orig);
    free(staged);
    free(dest_rel);
    free(tmp_rel);
}

bd_status bd_prune_versions(bd_catalog *cat, int64_t media_id, bd_prune_stats *stats, bd_log_fn log, void *log_ctx)
{
    bd_prune_stats local;
    if (!stats) stats = &local;
    memset(stats, 0, sizeof(*stats));
    int keep = bd_option_get(cat, "keep_versions"), days = bd_option_get(cat, "keep_days");
    if (keep <= 0) return BD_OK;
    bd_guard_info g;
    if (bd_guard_get(cat, &g) == BD_OK && g.tripped) {
        bd_logf(log, log_ctx, "old versions are kept while the changes BRODALF stopped at wait for a decision");
        return BD_OK;
    }

    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT c.id, c.path_on_media, v.size FROM copies c JOIN versions v ON v.id=c.version_id"
                           " JOIN nodes n ON n.id=v.node_id"
                           " WHERE c.media_id=?1 AND v.id IS NOT n.current_version_id"
                           " AND substr(c.path_on_media,1,length(?4)+1)=?4||'/'"
                           " AND (SELECT COUNT(*) FROM versions v2 WHERE v2.node_id=v.node_id AND v2.version_no>v.version_no)>=?2"
                           " AND COALESCE((SELECT MIN(v3.first_seen_ms) FROM versions v3"
                           "   WHERE v3.node_id=v.node_id AND v3.version_no>v.version_no),0)<=?3",
                           -1, &q, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "find old versions");
    sqlite3_bind_int64(q, 1, media_id);
    sqlite3_bind_int(q, 2, keep);
    sqlite3_bind_int64(q, 3, bd_now_ms() - (int64_t)days * 24 * 3600 * 1000);
    sqlite3_bind_text(q, 4, BD_VERSIONS_DIR, -1, SQLITE_STATIC);
    typedef struct { int64_t id, size; char *rel; } old_copy;
    old_copy *list = NULL;
    size_t n = 0, cap = 0;
    while (sqlite3_step(q) == SQLITE_ROW) {
        if (n == cap) {
            size_t nc = cap ? cap * 2 : 64;
            old_copy *nl = realloc(list, nc * sizeof(old_copy));
            if (!nl) break;
            list = nl;
            cap = nc;
        }
        list[n].id = sqlite3_column_int64(q, 0);
        list[n].rel = bd_strdup((const char *)sqlite3_column_text(q, 1));
        list[n].size = sqlite3_column_int64(q, 2);
        n++;
    }
    sqlite3_finalize(q);
    if (!n) { free(list); return BD_OK; }

    bd_store *store = bd_store_open(cat, media_id);
    sqlite3_stmt *d = NULL;
    if (!store || sqlite3_prepare_v2(cat->db, "DELETE FROM copies WHERE id=?", -1, &d, NULL) != SQLITE_OK) {
        for (size_t i = 0; i < n; i++) free(list[i].rel);
        free(list);
        bd_store_close(store);
        return store ? bd_fail_db(cat, "forget old versions") : BD_ERR_NOT_FOUND;
    }
    for (size_t i = 0; i < n; i++) {
        if (list[i].rel && store->ops->remove(store, list[i].rel) == 0) {
            sqlite3_reset(d);
            sqlite3_bind_int64(d, 1, list[i].id);
            sqlite3_step(d);
            stats->copies_removed++;
            stats->bytes_freed += list[i].size;
        } else {
            stats->failed++;
            bd_logf(log, log_ctx, "cannot remove the old version %s: %s", list[i].rel ? list[i].rel : "?", store->err);
        }
        free(list[i].rel);
    }
    free(list);
    sqlite3_finalize(d);
    bd_store_close(store);
    if (stats->copies_removed)
        bd_logf(log, log_ctx, "removed %lld old versions that the keep rule no longer needs", (long long)stats->copies_removed);
    return BD_OK;
}

/* ?4..?11: the drives whose copies count (the full ones), 0 ending the list. */
static void bind_continue(sqlite3_stmt *q, const bd_backup_opts *opts)
{
    for (int k = 0; k < BD_MAX_CONTINUE; k++) {
        int64_t id = opts ? opts->only_missing_from[k] : 0;
        if (k > 0 && opts && !opts->only_missing_from[k - 1]) id = 0; /* the list ended */
        sqlite3_bind_int64(q, 4 + k, id);
    }
}

bd_status bd_backup(bd_catalog *cat, int64_t media_id, int64_t source_id,
                    bd_backup_stats *stats, bd_log_fn log, void *log_ctx)
{
    return bd_backup_ex(cat, media_id, source_id, NULL, stats, log, log_ctx);
}

bd_status bd_backup_ex(bd_catalog *cat, int64_t media_id, int64_t source_id, const bd_backup_opts *opts,
                       bd_backup_stats *stats, bd_log_fn log, void *log_ctx)
{
    bd_backup_stats local;
    if (!stats) stats = &local;
    memset(stats, 0, sizeof(*stats));

    bd_guard_info g;
    if (!(opts && opts->ignore_guard) && bd_guard_get(cat, &g) == BD_OK && g.tripped)
        return bd_fail(cat, BD_ERR_GUARD,
                       "backups are paused: the last scan found %lld of %lld files changed or gone at once, which is what "
                       "ransomware does. Restore them as they were, or say the changes are yours",
                       (long long)(g.files_changed + g.files_deleted), (long long)g.files_total);

    const uint8_t *key = NULL;
    if (bd_media_encrypted(cat, media_id)) {
        if (!cat->have_key) return bd_fail(cat, BD_ERR_PASSPHRASE, "enter the passphrase to back up to an encrypted drive");
        key = cat->key;
    }
    bd_store *store = bd_store_open(cat, media_id);
    if (!store) return BD_ERR_NOT_FOUND;

    /* Make room first: old versions the keep rule no longer needs. */
    bd_prune_stats ps;
    if (bd_prune_versions(cat, media_id, &ps, log, log_ctx) == BD_OK) {
        stats->versions_pruned = ps.copies_removed;
        stats->bytes_pruned = ps.bytes_freed;
    }

    /* How much fits. Unknown space (some cloud accounts) means no limit. */
    int64_t space_total = 0, space_free = -1, reserve = 0;
    if (bd_test_free_bytes >= 0) space_free = bd_test_free_bytes;
    else if (store->ops->space(store, &space_total, &space_free) != 0) space_free = -1;
    else {
        reserve = space_total / 200 > MIN_RESERVE ? space_total / 200 : MIN_RESERVE;
        bd_media_note_space(cat, media_id, space_total, space_free, "before backup");
    }

    int64_t job = start_job(cat, "backup", media_id);
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT n.id, n.rel_path, n.size, n.mtime_ns, s.path, s.name, v.id, v.hash, v.version_no, v.size,"
                           " EXISTS(SELECT 1 FROM copies c WHERE c.version_id=v.id AND c.media_id=?1 AND c.state='ok')"
                           " FROM nodes n JOIN sources s ON s.id=n.source_id JOIN versions v ON v.id=n.current_version_id"
                           " WHERE n.is_dir=0 AND n.deleted=0 AND (?2=0 OR n.source_id=?2) AND n.id>?3"
                           " AND (?4=0 OR NOT EXISTS(SELECT 1 FROM copies c2 WHERE c2.version_id=v.id"
                           "   AND c2.media_id IN (?4,?5,?6,?7,?8,?9,?10,?11) AND c2.state='ok'))"
                           " ORDER BY n.id LIMIT 256",
                           -1, &q, NULL) != SQLITE_OK) {
        bd_store_close(store);
        return bd_fail_db(cat, "list files to back up");
    }
    bind_continue(q, opts);

    /* Totals for the progress bar: what this drive does not hold yet. */
    sqlite3_stmt *t;
    int64_t todo_files = 0, todo_bytes = 0;
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT COUNT(*), COALESCE(SUM(v.size),0) FROM nodes n JOIN versions v ON v.id=n.current_version_id"
                           " WHERE n.is_dir=0 AND n.deleted=0 AND (?2=0 OR n.source_id=?2) AND n.id>?3"
                           " AND NOT EXISTS(SELECT 1 FROM copies c WHERE c.version_id=v.id AND c.media_id=?1 AND c.state='ok')"
                           " AND (?4=0 OR NOT EXISTS(SELECT 1 FROM copies c2 WHERE c2.version_id=v.id"
                           "   AND c2.media_id IN (?4,?5,?6,?7,?8,?9,?10,?11) AND c2.state='ok'))",
                           -1, &t, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(t, 1, media_id);
        sqlite3_bind_int64(t, 2, source_id);
        sqlite3_bind_int64(t, 3, 0);
        bind_continue(t, opts);
        if (sqlite3_step(t) == SQLITE_ROW) {
            todo_files = sqlite3_column_int64(t, 0);
            todo_bytes = sqlite3_column_int64(t, 1);
        }
        sqlite3_finalize(t);
    }
    bd_progress_begin(cat, "backup", todo_files, todo_bytes);

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
        for (size_t i = 0; i < n; i++) {
            /* Moved or renamed: move the copy already on the drive. */
            if (move_existing(cat, store, &items[i], key, stats, log, log_ctx)) {
                bd_report(cat, stats->files_copied + stats->files_moved, items[i].rel, 0);
                continue;
            }
            if (space_free >= 0) {
                /* Sealed copies are a little bigger than the file. */
                int64_t need = items[i].version_size + (key ? items[i].version_size / 1024 + 4096 : 0);
                if (need > space_free - reserve) {
                    if (!stats->files_no_room)
                        bd_logf(log, log_ctx, "the drive is full; files that do not fit are left for another drive");
                    stats->files_no_room++;
                    stats->bytes_no_room += items[i].version_size;
                    continue;
                }
            }
            int64_t before = stats->bytes_copied;
            backup_one(cat, store, key, &items[i], stats, log, log_ctx);
            if (space_free >= 0) space_free -= stats->bytes_copied - before;
        }
        free_items(items, n);
    }
    sqlite3_finalize(q);

    int64_t total_bytes = 0, free_bytes = 0;
    if (store->ops->space(store, &total_bytes, &free_bytes) == 0)
        bd_media_note_space(cat, media_id, total_bytes, free_bytes, "after backup");
    bd_store_close(store);
    bd_report(cat, stats->files_copied + stats->files_moved, NULL, 1);
    bd_progress_end(cat);
    finish_job(cat, job, stats->files_failed ? "partial" : "done", stats->files_copied, stats->files_failed, stats->bytes_copied);
    return BD_OK;
}

bd_status bd_restore(bd_catalog *cat, int64_t source_id, const char *rel_prefix, const char *dest_root,
                     bd_restore_stats *stats, bd_log_fn log, void *log_ctx)
{
    return bd_restore_ex(cat, source_id, rel_prefix, dest_root, NULL, stats, log, log_ctx);
}

/* The files a restore brings back and the version of each: the current one,
 * or with ?4 (as of) the newest seen by then, for files that existed then
 * (including ones deleted since). */
#define RESTORE_VERSION                                                                                     \
    "(CASE WHEN ?4=0 THEN n.current_version_id ELSE (SELECT v2.id FROM versions v2 WHERE v2.node_id=n.id"  \
    " AND v2.first_seen_ms<=?4 ORDER BY v2.version_no DESC LIMIT 1) END)"
#define RESTORE_WHERE                                                                                       \
    " (CASE WHEN ?4=0 THEN n.deleted=0 ELSE (n.deleted=0 OR n.last_seen_ms>=?4) END)"                      \
    " AND (?1=0 OR n.source_id=?1) AND (?2='' OR n.rel_path=?2 OR substr(n.rel_path,1,length(?2)+1)=?2||'/')"

bd_status bd_restore_ex(bd_catalog *cat, int64_t source_id, const char *rel_prefix, const char *dest_root,
                        const bd_restore_opts *opts, bd_restore_stats *stats, bd_log_fn log, void *log_ctx)
{
    int64_t as_of = opts ? opts->as_of_ms : 0;
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
                           "SELECT n.id, n.rel_path, n.is_dir, s.name, v.id, v.hash, v.size FROM nodes n"
                           " JOIN sources s ON s.id=n.source_id LEFT JOIN versions v ON v.id=" RESTORE_VERSION
                           " WHERE" RESTORE_WHERE " AND n.id>?3 AND (n.is_dir=1 OR ?4=0 OR v.id IS NOT NULL)"
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
        sqlite3_bind_int64(t, 4, as_of);
        if (sqlite3_step(t) == SQLITE_ROW) {
            total_files = sqlite3_column_int64(t, 0);
            total_bytes = sqlite3_column_int64(t, 1);
        }
        sqlite3_finalize(t);
    }
    bd_progress_begin(cat, "restore", total_files, total_bytes);
    int64_t files_seen = 0;

    typedef struct { int64_t id, version_id, size; int is_dir; char *rel, *source_name, *hash; } item;
    item items[BATCH];
    int64_t last_id = 0;
    for (;;) {
        size_t n = 0;
        sqlite3_reset(q);
        sqlite3_bind_int64(q, 1, source_id);
        sqlite3_bind_text(q, 2, prefix, -1, SQLITE_STATIC);
        sqlite3_bind_int64(q, 3, last_id);
        sqlite3_bind_int64(q, 4, as_of);
        while (n < BATCH && sqlite3_step(q) == SQLITE_ROW) {
            item *it = &items[n++];
            it->id = last_id = sqlite3_column_int64(q, 0);
            it->rel = bd_strdup((const char *)sqlite3_column_text(q, 1));
            it->is_dir = sqlite3_column_int(q, 2);
            it->source_name = bd_strdup((const char *)sqlite3_column_text(q, 3));
            it->version_id = sqlite3_column_int64(q, 4);
            it->hash = sqlite3_column_type(q, 5) == SQLITE_NULL ? NULL : bd_strdup((const char *)sqlite3_column_text(q, 5));
            it->size = sqlite3_column_int64(q, 6);
        }
        sqlite3_reset(q);
        if (n == 0) break;

        for (size_t i = 0; i < n; i++) {
            item *it = &items[i];
            char *rel_out = bd_sprintf("%s/%s", it->source_name, it->rel);
            char *dest = rel_out ? bd_path_join(dest_root, rel_out) : NULL;
            free(rel_out);
            if (!dest) { stats->files_failed++; continue; }
            if (!it->is_dir) bd_report(cat, files_seen++, it->rel, 0);
            if (it->is_dir) {
                bd_mkdirs(dest);
                free(dest);
                continue;
            }
            if (!it->version_id || !it->hash) { stats->files_no_copy++; free(dest); continue; }
            /* Restored before (an earlier run, from another drive)? */
            bd_stat_t dst;
            if (bd_stat(dest, &dst) == 0 && dst.is_file && dst.size == it->size) {
                char have[BD_HASH_HEX_LEN + 1];
                if (bd_hash_file(dest, NULL, have, NULL) == 0 && strcmp(have, it->hash) == 0) {
                    stats->files_already_there++;
                    free(dest);
                    continue;
                }
            }

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
    bd_report(cat, files_seen, NULL, 1);
    bd_progress_end(cat);
done:
    sqlite3_finalize(q);
    sqlite3_finalize(find);
    sqlite3_finalize(any);
    bd_store_close(store);
    free(prefix);
    return s;
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
    int64_t as_of = opts ? opts->as_of_ms : 0;
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
                           " EXISTS(SELECT 1 FROM temp.connected k WHERE k.media_id=m.id) FROM media m ORDER BY m.id",
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
    sqlite3_bind_int64(fq, 4, as_of);
    while (sqlite3_step(fq) == SQLITE_ROW) {
        int64_t size = sqlite3_column_int64(fq, 3);
        if (dest_root && *dest_root) {
            char *out = bd_sprintf("%s/%s", (const char *)sqlite3_column_text(fq, 2), (const char *)sqlite3_column_text(fq, 1));
            char *dest = out ? bd_path_join(dest_root, out) : NULL;
            bd_stat_t st;
            int there = dest && bd_stat(dest, &st) == 0 && st.is_file && st.size == size;
            free(out);
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
