/* What each drive holds and when it was last backed up to or checked, and
 * a list of every file to open in a spreadsheet. All of it only reads the
 * catalog. */
#include "internal.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void copy_text(char *dst, size_t cap, const unsigned char *src)
{
    snprintf(dst, cap, "%s", src ? (const char *)src : "");
}

bd_status bd_media_status_get(bd_catalog *cat, int64_t media_id, bd_media_status *out)
{
    memset(out, 0, sizeof(*out));
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(cat->db, "SELECT 1 FROM media WHERE id=?", -1, &q, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "read the drive");
    sqlite3_bind_int64(q, 1, media_id);
    int found = sqlite3_step(q) == SQLITE_ROW;
    sqlite3_finalize(q);
    if (!found) return bd_fail(cat, BD_ERR_NOT_FOUND, "no such drive");

    /* Live files: the file as it is now is here (2), only an older version
     * is (1), or nothing of it is (0). Each file is put in one of the three
     * once; the look for an older version runs only when the current one
     * is not here, and goes from the file's own versions to their copies on
     * this drive through the (version_id, media_id) index, so the time
     * grows with the number of files, not with files times copies. */
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT st, COUNT(*), COALESCE(SUM(size),0) FROM (SELECT n.size,"
                           "  CASE WHEN EXISTS(SELECT 1 FROM copies c WHERE c.version_id=n.current_version_id"
                           "    AND c.media_id=?1 AND c.state='ok') THEN 2"
                           "   WHEN EXISTS(SELECT 1 FROM versions v WHERE v.node_id=n.id AND EXISTS("
                           "    SELECT 1 FROM copies c WHERE c.version_id=v.id AND c.media_id=?1 AND c.state='ok')) THEN 1"
                           "   ELSE 0 END AS st"
                           "  FROM nodes n WHERE n.is_dir=0 AND n.deleted=0 AND n.current_version_id IS NOT NULL)"
                           " GROUP BY st",
                           -1, &q, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "count the files on the drive");
    sqlite3_bind_int64(q, 1, media_id);
    int rc;
    while ((rc = sqlite3_step(q)) == SQLITE_ROW) {
        int64_t files = sqlite3_column_int64(q, 1), bytes = sqlite3_column_int64(q, 2);
        switch (sqlite3_column_int(q, 0)) {
        case 2: out->files_current = files; out->bytes_current = bytes; break;
        case 1: out->files_behind = files; out->bytes_behind = bytes; break;
        default: out->files_not_here = files; out->bytes_not_here = bytes; break;
        }
    }
    sqlite3_finalize(q);
    if (rc != SQLITE_DONE) return bd_fail_db(cat, "count the files on the drive");

    if (sqlite3_prepare_v2(cat->db,
                           "SELECT (SELECT COUNT(*) FROM nodes n WHERE n.is_dir=0 AND n.deleted=1 AND EXISTS("
                           "   SELECT 1 FROM copies c WHERE c.version_id=n.current_version_id AND c.media_id=?1 AND c.state='ok')),"
                           " (SELECT COUNT(*) FROM copies c JOIN versions v ON v.id=c.version_id JOIN nodes n ON n.id=v.node_id"
                           "   WHERE c.media_id=?1 AND c.state='ok' AND v.id IS NOT n.current_version_id AND ?2=?2),"
                           " (SELECT COALESCE(SUM(c.stored_size),0) FROM copies c JOIN versions v ON v.id=c.version_id"
                           "   JOIN nodes n ON n.id=v.node_id"
                           "   WHERE c.media_id=?1 AND c.state='ok' AND v.id IS NOT n.current_version_id),"
                           " (SELECT COUNT(*) FROM copies WHERE media_id=?1 AND state<>'ok'),"
                           " (SELECT COUNT(*) FROM copies WHERE media_id=?1 AND state='ok' AND last_full_check_ms IS NULL)",
                           -1, &q, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "count the copies on the drive");
    sqlite3_bind_int64(q, 1, media_id);
    sqlite3_bind_text(q, 2, BD_VERSIONS_DIR, -1, SQLITE_STATIC);
    if (sqlite3_step(q) == SQLITE_ROW) {
        out->deleted_kept = sqlite3_column_int64(q, 0);
        out->old_versions = sqlite3_column_int64(q, 1);
        out->old_bytes = sqlite3_column_int64(q, 2);
        out->copies_bad = sqlite3_column_int64(q, 3);
        out->copies_never_read = sqlite3_column_int64(q, 4);
    }
    sqlite3_finalize(q);

    if (sqlite3_prepare_v2(cat->db,
                           "SELECT started_ms, COALESCE(finished_ms,0), status, files_done, files_failed FROM jobs"
                           " WHERE kind='backup' AND media_id=? ORDER BY started_ms DESC, id DESC LIMIT 1",
                           -1, &q, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "read the drive's last backup");
    sqlite3_bind_int64(q, 1, media_id);
    if (sqlite3_step(q) == SQLITE_ROW) {
        out->last_backup_ms = sqlite3_column_int64(q, 0);
        out->last_backup_end_ms = sqlite3_column_int64(q, 1);
        copy_text(out->last_backup_status, sizeof(out->last_backup_status), sqlite3_column_text(q, 2));
        out->last_backup_files = sqlite3_column_int64(q, 3);
        out->last_backup_failed = sqlite3_column_int64(q, 4);
    }
    sqlite3_finalize(q);

    if (sqlite3_prepare_v2(cat->db,
                           "SELECT finished_ms, kind, status FROM jobs WHERE media_id=? AND finished_ms IS NOT NULL"
                           " AND kind IN ('check','quick check','full check','read back') ORDER BY finished_ms DESC, id DESC LIMIT 1",
                           -1, &q, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "read the drive's last check");
    sqlite3_bind_int64(q, 1, media_id);
    if (sqlite3_step(q) == SQLITE_ROW) {
        out->last_check_ms = sqlite3_column_int64(q, 0);
        copy_text(out->last_check_kind, sizeof(out->last_check_kind), sqlite3_column_text(q, 1));
        copy_text(out->last_check_status, sizeof(out->last_check_status), sqlite3_column_text(q, 2));
    }
    sqlite3_finalize(q);
    return BD_OK;
}

/* ---- The file list as CSV ----------------------------------------------- */

/* Days since 1970-01-01 to a calendar date (Howard Hinnant's algorithm). */
static void civil_from_days(int64_t z, int *y, int *m, int *d)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yy = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yy + (*m <= 2));
}

void bd_format_utc(int64_t ms, char *out, size_t cap)
{
    if (ms <= 0) { snprintf(out, cap, "%s", ""); return; }
    int64_t secs = ms / 1000;
    int64_t days = secs / 86400, rem = secs % 86400;
    int y, mo, d;
    civil_from_days(days, &y, &mo, &d);
    snprintf(out, cap, "%04d-%02d-%02d %02d:%02d:%02d", y, mo, d, (int)(rem / 3600), (int)(rem / 60 % 60),
             (int)(rem % 60));
}

/* One quoted field. A text that a spreadsheet would read as a formula
 * gets a leading ' so it shows as text. A path (is_path) is written with
 * Windows' separators on Windows; any other text, like a drive's label or
 * where it is kept ("shelf 2/3"), is written as it is. */
static int put_field(FILE *f, const char *s, int is_path, int last)
{
    if (!s) s = "";
    if (fputc('"', f) == EOF) return -1;
    if (s[0] == '=' || s[0] == '+' || s[0] == '-' || s[0] == '@' || s[0] == '\t' || s[0] == '\r')
        fputc('\'', f);
    for (const char *p = s; *p; p++) {
#ifdef _WIN32
        char c = is_path && *p == '/' ? '\\' : *p;
#else
        (void)is_path;
        char c = *p;
#endif
        if (c == '"') fputc('"', f);
        fputc(c, f);
    }
    fputc('"', f);
    return fputs(last ? "\r\n" : ",", f) == EOF ? -1 : 0;
}

static int put_number(FILE *f, int64_t v, int last)
{
    return fprintf(f, "%lld%s", (long long)v, last ? "\r\n" : ",") < 0 ? -1 : 0;
}

bd_status bd_export_csv(bd_catalog *cat, const char *path, int64_t *out_rows)
{
    if (out_rows) *out_rows = 0;
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT s.path, n.rel_path, n.size, n.mtime_ns, n.deleted,"
                           " (SELECT COUNT(*) FROM versions v WHERE v.node_id=n.id),"
                           " (SELECT COUNT(DISTINCT c.media_id) FROM copies c WHERE c.version_id=n.current_version_id"
                           "   AND c.state='ok'),"
                           " (SELECT group_concat(lbl, '; ') FROM (SELECT DISTINCT m.label ||"
                           "   CASE WHEN trim(COALESCE(m.location,''))<>'' THEN ' (' || trim(m.location) || ')' ELSE '' END AS lbl"
                           "   FROM copies c JOIN media m ON m.id=c.media_id"
                           "   WHERE c.version_id=n.current_version_id AND c.state='ok' ORDER BY m.label COLLATE NOCASE))"
                           " FROM nodes n JOIN sources s ON s.id=n.source_id WHERE n.is_dir=0"
                           " ORDER BY s.name COLLATE NOCASE, n.rel_path COLLATE NOCASE",
                           -1, &q, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "list the files");
    FILE *f = bd_fopen(path, "wb");
    if (!f) {
        sqlite3_finalize(q);
        return bd_fail(cat, BD_ERR_IO, "cannot write %s", path);
    }
    /* A byte order mark, so Excel reads the names as UTF-8. */
    int rc = fputs("\xEF\xBB\xBF", f) == EOF ? -1 : 0;
    if (rc == 0)
        rc = fputs("\"Folder\",\"File\",\"Size (bytes)\",\"Modified (UTC)\",\"Deleted from the folder\","
                   "\"Versions\",\"Drives with the file as it is now\",\"On drives\"\r\n",
                   f) == EOF ? -1 : 0;
    int64_t rows = 0;
    while (rc == 0 && sqlite3_step(q) == SQLITE_ROW) {
        char when[32];
        int64_t mtime_ns = sqlite3_column_int64(q, 3);
        bd_format_utc(mtime_ns > 0 ? mtime_ns / 1000000 : 0, when, sizeof(when));
        int deleted = sqlite3_column_int(q, 4);
        rc = put_field(f, (const char *)sqlite3_column_text(q, 0), 1, 0);
        if (rc == 0) rc = put_field(f, (const char *)sqlite3_column_text(q, 1), 1, 0);
        if (rc == 0) rc = put_number(f, sqlite3_column_int64(q, 2), 0);
        if (rc == 0) rc = put_field(f, when, 0, 0);
        if (rc == 0) rc = put_field(f, deleted ? "yes" : "no", 0, 0);
        if (rc == 0) rc = put_number(f, sqlite3_column_int64(q, 5), 0);
        if (rc == 0) rc = put_field(f, (const char *)sqlite3_column_text(q, 7), 0, 0);
        if (rc == 0) rc = put_number(f, sqlite3_column_int64(q, 6), 1);
        rows++;
    }
    sqlite3_finalize(q);
    if (fclose(f) != 0) rc = -1;
    if (rc != 0) {
        bd_remove(path);
        return bd_fail(cat, BD_ERR_IO, "cannot write %s", path);
    }
    if (out_rows) *out_rows = rows;
    return BD_OK;
}
