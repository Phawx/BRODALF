/* Compressing a backup drive, when the person asks.
 *
 * BRODALF uses Windows' own file compression (NTFS): the copies stay
 * ordinary files that any Windows PC reads without BRODALF, and Windows
 * makes them smaller on the drive and whole again as they are read. It is
 * set on BRODALF's folder on the drive and on every folder and file in it;
 * a compressed folder makes the files later copied into it compressed
 * too, so backups after that need nothing more. Nothing here runs by
 * itself.
 *
 * An encrypted drive is left out: encrypted copies look like random data,
 * which cannot be made smaller. So are drives formatted exFAT or FAT32,
 * which cannot compress files, and every drive outside Windows. */
#include "internal.h"

#include <stdlib.h>
#include <string.h>

/* The drive's folder for this catalog, <root>/BRODALF/<catalog uuid>, when
 * it is plugged in. */
static char *drive_dir(bd_catalog *cat, int64_t media_id)
{
    sqlite3_stmt *q;
    char *dir = NULL;
    if (sqlite3_prepare_v2(cat->db, "SELECT root FROM temp.connected WHERE media_id=?", -1, &q, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_int64(q, 1, media_id);
    if (sqlite3_step(q) == SQLITE_ROW) {
        char *base = bd_path_join((const char *)sqlite3_column_text(q, 0), "BRODALF");
        if (base) dir = bd_path_join(base, bd_catalog_uuid(cat));
        free(base);
    }
    sqlite3_finalize(q);
    return dir;
}

static const char *cannot_why(bd_catalog *cat, int64_t media_id, const char *dir)
{
#ifndef _WIN32
    (void)cat;
    (void)media_id;
    (void)dir;
    return "Compressing a drive uses Windows' own file compression, so it works only on Windows.";
#else
    if (bd_media_encrypted(cat, media_id))
        return "This drive is encrypted. Encrypted copies look like random data, which cannot be made smaller.";
    if (!bd_fs_can_compress(dir))
        return "This drive cannot compress files: it is not formatted NTFS (exFAT and FAT32 drives cannot).";
    return NULL;
#endif
}

bd_status bd_media_compression(bd_catalog *cat, int64_t media_id, int *can, int *on, const char **why)
{
    *can = 0;
    *on = 0;
    if (why) *why = NULL;
    char *dir = drive_dir(cat, media_id);
    if (!dir) return bd_fail(cat, BD_ERR_NOT_FOUND, "that drive is not plugged in");
    const char *no = cannot_why(cat, media_id, dir);
    if (why) *why = no;
    *can = no == NULL;
    *on = bd_fs_compressed(dir) == 1;
    free(dir);
    return BD_OK;
}

typedef struct {
    bd_catalog *cat;
    const char *dir;
    int on, counting;
    int64_t total_files, total_bytes;
    bd_compress_stats *stats;
    bd_log_fn log;
    void *log_ctx;
    int cancelled;
} compress_walk;

static int compress_cb(void *ctx, const char *rel, const bd_stat_t *st)
{
    compress_walk *w = ctx;
    if (w->counting) {
        if (st->is_file) {
            w->total_files++;
            w->total_bytes += st->size;
        }
        return 0;
    }
    if (bd_cancelled(w->cat)) {
        w->cancelled = 1;
        return 1;
    }
    char *path = bd_path_join(w->dir, rel);
    if (!path) return 1;
    if (st->is_dir) {
        /* So the files later copied into it come out compressed (or not). */
        if (bd_fs_compressed(path) != w->on && bd_fs_set_compressed(path, w->on) != 0)
            bd_logf(w->log, w->log_ctx, "cannot change the folder %s", rel);
        free(path);
        return 0;
    }
    if (!st->is_file) {
        free(path);
        return 0;
    }
    bd_compress_stats *s = w->stats;
    s->files++;
    s->bytes += st->size;
    bd_report_file(w->cat, s->files, rel, st->size);
    if (bd_fs_compressed(path) != w->on) {
        if (bd_fs_set_compressed(path, w->on) == 0) {
            s->changed++;
        } else {
            s->failed++;
            bd_logf(w->log, w->log_ctx, "cannot %s %s (another program may have it open)", w->on ? "compress" : "uncompress", rel);
        }
    }
    bd_io_tick(st->size);
    int64_t room = bd_fs_size_on_disk(path);
    s->bytes_on_disk += room >= 0 ? room : st->size;
    free(path);
    return 0;
}

bd_status bd_media_set_compression(bd_catalog *cat, int64_t media_id, int on, bd_compress_stats *stats, bd_log_fn log,
                                   void *log_ctx)
{
    memset(stats, 0, sizeof(*stats));
    bd_cancel_reset(cat);
    char *dir = drive_dir(cat, media_id);
    if (!dir) return bd_fail(cat, BD_ERR_NOT_FOUND, "that drive is not plugged in");
    const char *no = cannot_why(cat, media_id, dir);
    /* Making a drive's files whole again is allowed wherever Windows can
     * compress files, even on an encrypted drive. */
    if (no && (on || !bd_fs_can_compress(dir))) {
        free(dir);
        return bd_fail(cat, BD_ERR_INVALID, "%s", no);
    }
    bd_status s = BD_OK;
    compress_walk w = {cat, dir, on ? 1 : 0, 1, 0, 0, stats, log, log_ctx, 0};
    bd_walk(dir, compress_cb, NULL, &w);
    bd_progress_begin(cat, on ? "compress" : "uncompress", w.total_files, w.total_bytes);
    int64_t job = bd_job_start(cat, on ? "compress" : "uncompress", media_id);
    if (bd_fs_set_compressed(dir, w.on) != 0) {
        s = bd_fail(cat, BD_ERR_IO, "cannot change BRODALF's folder on the drive");
    } else {
        w.counting = 0;
        if (bd_walk(dir, compress_cb, NULL, &w) != 0 && !w.cancelled) s = bd_fail(cat, BD_ERR_NOMEM, "out of memory");
        if (s == BD_OK && w.cancelled) s = bd_fail_cancelled(cat);
    }
    bd_report(cat, stats->files, NULL, 1);
    bd_progress_end(cat);
    bd_job_finish(cat, job, s == BD_ERR_CANCELLED ? "stopped" : s != BD_OK ? "failed" : stats->failed ? "partial" : "done",
                  stats->files, stats->failed, stats->bytes);
    if (s == BD_OK)
        bd_logf(log, log_ctx, "%s: %lld files, %lld changed, %lld could not be", on ? "compressed" : "uncompressed",
                (long long)stats->files, (long long)stats->changed, (long long)stats->failed);
    free(dir);
    return s;
}
