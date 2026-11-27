/* The recovery sheet: one self-contained HTML page to print and keep away
 * from the PC. It lists the backup drives (name, where kept, size, free
 * space, which backup roots they hold and how many files, last backup),
 * the backup roots, what to do when the catalog file is lost, and for an encrypted
 * catalog the recovery key it is given. Nothing is fetched: no scripts, no
 * images, no links. The passphrase is never written. */
#include "internal.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

static void put_html(FILE *f, const char *s)
{
    for (; s && *s; s++) {
        switch (*s) {
        case '&': fputs("&amp;", f); break;
        case '<': fputs("&lt;", f); break;
        case '>': fputs("&gt;", f); break;
        case '"': fputs("&quot;", f); break;
        case '\'': fputs("&#39;", f); break;
        default: fputc(*s, f);
        }
    }
}

static void put_bytes(FILE *f, int64_t b)
{
    static const char *units[] = {"bytes", "KB", "MB", "GB", "TB", "PB"};
    double v = (double)b;
    int u = 0;
    while (v >= 1000.0 && u < 5) { v /= 1000.0; u++; }
    if (u == 0) fprintf(f, "%lld bytes", (long long)b);
    else fprintf(f, "%.1f %s", v, units[u]);
}

static void put_time(FILE *f, int64_t ms)
{
    if (ms <= 0) { fputs("never", f); return; }
    time_t t = (time_t)(ms / 1000);
    struct tm tm;
#ifdef _WIN32
    if (localtime_s(&tm, &t) != 0) { fputs("unknown", f); return; }
#else
    if (!localtime_r(&t, &tm)) { fputs("unknown", f); return; }
#endif
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
    fputs(buf, f);
}

static const char *base_name(const char *path)
{
    const char *b = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\') b = p + 1;
    return b;
}

typedef struct {
    FILE *f;
    sqlite3_stmt *held, *last;
    int rows;
    bd_catalog *key_cat; /* set when the sheet holds a new recovery key */
} sheet_ctx;

static int drive_row(void *ctx, const bd_media_info *m)
{
    sheet_ctx *c = ctx;
    FILE *f = c->f;
    if (strcmp(m->kind, "drive") != 0) return 0; /* a cloud account from 0.3.0 cannot be recovered from */
    c->rows++;
    fputs("<tr><td><b>", f);
    put_html(f, m->label);
    fputs("</b>", f);
    if (m->encrypted) fputs("<br><small>encrypted</small>", f);
    if (m->lost_ms) {
        fputs("<br><small>marked ", f);
        put_html(f, m->lost_why);
        fputs("</small>", f);
    }
    /* Which drives' copies of the database open with the key on this sheet:
     * the others open with the key or passphrase of when they were written,
     * until their next backup. */
    int has = c->key_cat ? bd_media_copy_has_key(c->key_cat, m->media_id, 1) : -1;
    if (has == 1) fputs("<br><small>its database copy has this key</small>", f);
    else if (has == 0) fputs("<br><small>its database copy does not have this key yet: back up to it once</small>", f);
    fputs("</td><td>", f);
    if (m->location && *m->location) put_html(f, m->location);
    else fputs("<i>not set</i>", f);
    fputs("</td><td>", f);
    if (m->total_bytes > 0) put_bytes(f, m->total_bytes);
    else fputs("unknown", f);
    fputs("</td><td>", f);
    if (m->space_ms > 0) {
        put_bytes(f, m->free_bytes);
        fputs("<br><small>on ", f);
        put_time(f, m->space_ms);
        fputs("</small>", f);
    } else {
        fputs("unknown", f);
    }
    fputs("</td><td>", f);
    int any = 0;
    if (c->held) {
        sqlite3_reset(c->held);
        sqlite3_bind_int64(c->held, 1, m->media_id);
        while (sqlite3_step(c->held) == SQLITE_ROW) {
            if (any++) fputs("<br>", f);
            put_html(f, (const char *)sqlite3_column_text(c->held, 0));
            int64_t n = sqlite3_column_int64(c->held, 1);
            fprintf(f, ": %lld file%s (", (long long)n, n == 1 ? "" : "s");
            put_bytes(f, sqlite3_column_int64(c->held, 2));
            fputs(")", f);
        }
    }
    if (!any) fputs("<i>nothing yet</i>", f);
    fputs("</td><td>", f);
    int64_t last = 0;
    if (c->last) {
        sqlite3_reset(c->last);
        sqlite3_bind_int64(c->last, 1, m->media_id);
        if (sqlite3_step(c->last) == SQLITE_ROW) last = sqlite3_column_int64(c->last, 0);
    }
    put_time(f, last);
    fputs("</td></tr>\n", f);
    return 0;
}

static int source_row(void *ctx, const bd_source_info *s)
{
    sheet_ctx *c = ctx;
    FILE *f = c->f;
    c->rows++;
    fputs("<tr><td><b>", f);
    put_html(f, s->name);
    fputs("</b></td><td>", f);
    put_html(f, s->path);
    fprintf(f, "</td><td>%lld</td><td>", (long long)s->files_total);
    put_bytes(f, s->bytes_total);
    fputs("</td></tr>\n", f);
    return 0;
}

static const char STYLE[] =
    "body{font-family:\"Segoe UI\",Arial,sans-serif;color:#111;background:#fff;margin:24px auto;max-width:900px;"
    "padding:0 16px;font-size:14px;line-height:1.4}"
    "h1{font-size:24px;margin:0 0 4px}h2{font-size:17px;margin:24px 0 8px;border-bottom:1px solid #999}"
    ".meta{color:#444;margin:0}table{border-collapse:collapse;width:100%}"
    "th,td{border:1px solid #999;padding:6px 8px;text-align:left;vertical-align:top}th{background:#eee}"
    "small{color:#444}.key{font-family:Consolas,\"Courier New\",monospace;font-size:22px;letter-spacing:1px;"
    "border:2px solid #111;padding:10px 14px;display:inline-block;margin:4px 0}"
    ".foot{color:#666;font-size:12px;margin-top:24px}"
    "@media print{body{margin:0;max-width:none}h2{break-after:avoid}tr{break-inside:avoid}}";

bd_status bd_write_recovery_sheet(bd_catalog *cat, const char *path, const char *recovery_key)
{
    FILE *f = bd_fopen(path, "wb");
    if (!f) return bd_fail(cat, BD_ERR_IO, "cannot write %s", path);
    sheet_ctx c = {f, NULL, NULL, 0, recovery_key && *recovery_key && bd_catalog_has_passphrase(cat) ? cat : NULL};
    sqlite3_prepare_v2(cat->db,
                       "SELECT s.name, COUNT(*), COALESCE(SUM(v.size),0) FROM copies k"
                       " JOIN versions v ON v.id=k.version_id JOIN nodes n ON n.id=v.node_id JOIN sources s ON s.id=n.source_id"
                       " WHERE k.media_id=? AND k.state='ok' AND n.deleted=0 AND n.current_version_id=v.id"
                       " GROUP BY s.id ORDER BY s.name COLLATE NOCASE",
                       -1, &c.held, NULL);
    sqlite3_prepare_v2(cat->db, "SELECT MAX(finished_ms) FROM jobs WHERE media_id=? AND kind='backup' AND finished_ms IS NOT NULL",
                       -1, &c.last, NULL);

    fputs("<!DOCTYPE html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">"
          "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
          "<title>BRODALF recovery sheet</title><style>", f);
    fputs(STYLE, f);
    fputs("</style></head><body>\n<h1>BRODALF recovery sheet</h1>\n<p class=\"meta\">Catalog <b>", f);
    put_html(f, base_name(cat->path));
    fputs("</b>, kept at ", f);
    put_html(f, cat->path);
    fputs("<br>Catalog id ", f);
    put_html(f, cat->uuid);
    fputs(". Printed ", f);
    put_time(f, bd_now_ms());
    fputs(".</p>\n", f);

    int encrypted = bd_catalog_has_passphrase(cat);
    fputs("<h2>If the catalog file is lost</h2>\n<ol>"
          "<li>Install BRODALF on any computer: BRODALF ", f);
    put_html(f, bd_version());
    fputs(" or newer, from https://github.com/Phawx/BRODALF/releases</li>"
          "<li>Plug in one of the drives below. The one backed up to most recently is best.</li>"
          "<li>Start BRODALF and choose <b>Recover the database from a backup drive...</b> It copies the catalog from the drive, "
          "then finds your files on every drive you plug in.</li>",
          f);
    if (encrypted)
        fputs("<li>When it asks for the passphrase, type it, or choose <b>Use the recovery key</b> and type the key "
              "below.</li>",
              f);
    fputs("<li>BRODALF looks for each backup root by its files. One it does not find you can restore with "
          "<b>Restore</b>, or point at where it is now with <b>Change where a root is...</b></li></ol>\n"
          "<p>Without BRODALF: a drive that is not encrypted holds your files as plain files and folders, in "
          "<b>BRODALF\\", f);
    put_html(f, cat->uuid);
    fputs("</b> on the drive, one folder per backup root. Copy them off as they are; older versions are in the "
          ".versions folder there. An encrypted drive, or one that hides names, can be read only with BRODALF, "
          "so keep a copy of its installer with this sheet.</p>\n",
          f);

    if (encrypted) {
        fputs("<h2>Recovery key</h2>\n", f);
        if (recovery_key && *recovery_key) {
            fputs("<p class=\"key\">", f);
            put_html(f, recovery_key);
            fputs("</p>\n<p>This key opens the catalog and every encrypted drive, like the passphrase. Anyone who has it can "
                  "read your encrypted backups, so keep this sheet somewhere safe, away from the drives and the PC. "
                  "Making a new recovery sheet replaces this key.</p>\n"
                  "<p>To recover with this key, use a drive whose database copy has it: the list of drives below says "
                  "which. Each drive gets one at its next backup. A copy written before this key opens with the "
                  "passphrase or key the database had then.</p>\n",
                  f);
        } else {
            fputs("<p>This catalog is encrypted. Without the passphrase or a recovery key the encrypted copies cannot be "
                  "read. This sheet has no recovery key in it.</p>\n",
                  f);
        }
    }

    fputs("<h2>Backup drives</h2>\n<table><tr><th>Drive</th><th>Where it is kept</th><th>Size</th><th>Free</th>"
          "<th>What is on it</th><th>Last backup</th></tr>\n",
          f);
    c.rows = 0;
    bd_status s = bd_list_media(cat, drive_row, &c);
    if (s == BD_OK && !c.rows) fputs("<tr><td colspan=\"6\"><i>No backup drives yet.</i></td></tr>\n", f);
    fputs("</table>\n<h2>Backup roots</h2>\n<table><tr><th>Name on the drives</th><th>Folder on this PC</th>"
          "<th>Files</th><th>Size</th></tr>\n",
          f);
    c.rows = 0;
    if (s == BD_OK) s = bd_list_sources(cat, source_row, &c);
    if (s == BD_OK && !c.rows) fputs("<tr><td colspan=\"4\"><i>No backup roots yet.</i></td></tr>\n", f);
    fputs("</table>\n<p class=\"foot\">Made by BRODALF ", f);
    put_html(f, bd_version());
    fputs(".</p>\n</body></html>\n", f);

    sqlite3_finalize(c.held);
    sqlite3_finalize(c.last);
    int bad = ferror(f);
    if (fclose(f) != 0) bad = 1;
    if (s == BD_OK && bad) s = bd_fail(cat, BD_ERR_IO, "cannot write %s", path);
    if (s != BD_OK) bd_remove(path);
    return s;
}
