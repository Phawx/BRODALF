#include "internal.h"

#include "blake3.h"

#include <stdlib.h>
#include <string.h>

/* The catalog whose job is running, for byte counts from the hashing loop.
 * One job runs at a time. */
static bd_catalog *g_io_cat;

bd_status bd_fail(bd_catalog *cat, bd_status status, const char *fmt, ...)
{
    if (cat) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(cat->err, sizeof(cat->err), fmt, ap);
        va_end(ap);
    }
    return status;
}

bd_status bd_fail_db(bd_catalog *cat, const char *what)
{
    return bd_fail(cat, BD_ERR_DB, "%s: %s", what, cat && cat->db ? sqlite3_errmsg(cat->db) : "no database");
}

void bd_logf(bd_log_fn log, void *ctx, const char *fmt, ...)
{
    if (!log) return;
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log(ctx, buf);
}

char *bd_strdup(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

char *bd_sprintf(const char *fmt, ...)
{
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) { va_end(ap2); return NULL; }
    char *s = malloc((size_t)n + 1);
    if (s) vsnprintf(s, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return s;
}

char *bd_path_join(const char *a, const char *b)
{
    if (!b || !*b) return bd_strdup(a);
    size_t la = strlen(a);
    while (la > 0 && (a[la - 1] == '/' || a[la - 1] == '\\')) la--;
    while (*b == '/' || *b == '\\') b++;
    size_t lb = strlen(b);
    char *s = malloc(la + lb + 2);
    if (!s) return NULL;
    memcpy(s, a, la);
    s[la] = '/';
    memcpy(s + la + 1, b, lb + 1);
    return s;
}

char *bd_rel_dirname(const char *rel)
{
    const char *slash = strrchr(rel, '/');
    if (!slash) return bd_strdup("");
    size_t n = (size_t)(slash - rel);
    char *d = malloc(n + 1);
    if (!d) return NULL;
    memcpy(d, rel, n);
    d[n] = '\0';
    return d;
}

const char *bd_rel_basename(const char *rel)
{
    const char *slash = strrchr(rel, '/');
    return slash ? slash + 1 : rel;
}

int bd_hash_file(const char *path, FILE *copy_to, char hex_out[BD_HASH_HEX_LEN + 1], int64_t *size_out)
{
    return bd_hash_copy(path, NULL, copy_to, NULL, hex_out, size_out);
}

void bd_uuid_v4(char out[37])
{
    unsigned char b[16];
    if (bd_random_bytes(b, sizeof(b)) != 0) {
        /* Should not happen; fall back to time so we never return garbage. */
        int64_t t = bd_now_ms();
        for (int i = 0; i < 16; i++) b[i] = (unsigned char)(t >> ((i % 8) * 8)) ^ (unsigned char)(i * 37);
    }
    b[6] = (unsigned char)((b[6] & 0x0f) | 0x40);
    b[8] = (unsigned char)((b[8] & 0x3f) | 0x80);
    snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

char *bd_media_catalog_dir(const bd_catalog *cat, const char *root)
{
    char *m = bd_path_join(root, BD_MEDIA_DIR);
    if (!m) return NULL;
    char *d = bd_path_join(m, cat->uuid);
    free(m);
    return d;
}

char *bd_connected_root(bd_catalog *cat, int64_t media_id)
{
    sqlite3_stmt *st;
    char *root = NULL;
    if (sqlite3_prepare_v2(cat->db, "SELECT root FROM temp.connected WHERE media_id=?", -1, &st, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_int64(st, 1, media_id);
    if (sqlite3_step(st) == SQLITE_ROW) root = bd_strdup((const char *)sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    return root;
}

int bd_exec(bd_catalog *cat, const char *sql)
{
    return sqlite3_exec(cat->db, sql, NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
}

void bd_catalog_set_progress(bd_catalog *cat, bd_progress_fn fn, void *ctx)
{
    cat->progress = fn;
    cat->progress_ctx = ctx;
    if (!fn && g_io_cat == cat) g_io_cat = NULL;
}


static void emit(bd_catalog *cat, int done)
{
    bd_progress p;
    p.phase = cat->prog_phase ? cat->prog_phase : "";
    p.current = cat->prog_current;
    p.files_done = cat->prog_files;
    p.files_total = cat->prog_files_total;
    p.bytes_done = cat->prog_bytes;
    p.bytes_total = cat->prog_bytes_total;
    p.done = done;
    cat->progress(cat->progress_ctx, &p);
}

void bd_progress_begin(bd_catalog *cat, const char *phase, int64_t files_total, int64_t bytes_total)
{
    cat->prog_phase = phase;
    cat->prog_current[0] = '\0';
    cat->prog_files = cat->prog_bytes = 0;
    cat->prog_files_total = files_total;
    cat->prog_bytes_total = bytes_total;
    cat->progress_last_ms = 0;
    g_io_cat = cat->progress ? cat : NULL;
    if (cat->progress) emit(cat, 0);
}

void bd_report(bd_catalog *cat, int64_t files_done, const char *current, int force)
{
    cat->prog_files = files_done;
    snprintf(cat->prog_current, sizeof(cat->prog_current), "%s", current ? current : "");
    if (!cat->progress) return;
    int64_t now = bd_now_ms();
    if (!force && now - cat->progress_last_ms < 100) return;
    cat->progress_last_ms = now;
    emit(cat, 0);
}

void bd_progress_end(bd_catalog *cat)
{
    cat->prog_current[0] = '\0';
    if (cat->progress) emit(cat, 1);
    if (g_io_cat == cat) g_io_cat = NULL;
}

void bd_io_tick(int64_t bytes)
{
    bd_catalog *cat = g_io_cat;
    if (!cat) return;
    cat->prog_bytes += bytes;
    int64_t now = bd_now_ms();
    if (now - cat->progress_last_ms < 100) return;
    cat->progress_last_ms = now;
    emit(cat, 0);
}

const char *bd_status_name(bd_status s)
{
    switch (s) {
    case BD_OK: return "ok";
    case BD_ERR_IO: return "I/O error";
    case BD_ERR_DB: return "database error";
    case BD_ERR_FORMAT: return "not a BRODALF catalog";
    case BD_ERR_LOCKED: return "catalog is locked";
    case BD_ERR_EXISTS: return "already exists";
    case BD_ERR_NOT_FOUND: return "not found";
    case BD_ERR_INVALID: return "invalid argument";
    case BD_ERR_NOMEM: return "out of memory";
    case BD_ERR_PASSPHRASE: return "passphrase needed or wrong";
    case BD_ERR_GUARD: return "backups paused by the ransomware guard";
    }
    return "unknown error";
}

const char *bd_node_state_name(bd_node_state s)
{
    switch (s) {
    case BD_STATE_AVAILABLE: return "available";
    case BD_STATE_AVAILABLE_OLDER: return "older version available";
    case BD_STATE_OFFLINE: return "offline";
    case BD_STATE_NO_COPY: return "no copy";
    case BD_STATE_BAD: return "copy missing or damaged";
    case BD_STATE_DELETED: return "deleted from source";
    case BD_STATE_PARTIAL: return "partly available";
    }
    return "unknown";
}
