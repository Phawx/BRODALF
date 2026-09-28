/* The .brodalf file: a 16-byte header followed by one zstd stream holding a
 * SQLite database.
 *
 *   bytes 0-7   magic "BRODALF\x1a"
 *   bytes 8-11  format version, little endian (currently 1)
 *   bytes 12-15 flags, little endian (bit 0 reserved for encryption)
 *
 * While open, the database lives in a working copy in the temp folder and a
 * "<file>.lock" file sits next to the catalog. */
#include "internal.h"

#include "zstd.h"

#include <stdlib.h>
#include <string.h>

#define BD_MAGIC "BRODALF\x1a"
#define BD_FORMAT_VERSION 1u
#define BD_SCHEMA_VERSION 1

static const char *SCHEMA_SQL =
    "CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);"
    "CREATE TABLE IF NOT EXISTS settings(key TEXT PRIMARY KEY, value TEXT NOT NULL);"
    "CREATE TABLE IF NOT EXISTS sources("
    "  id INTEGER PRIMARY KEY,"
    "  path TEXT NOT NULL UNIQUE,"
    "  name TEXT NOT NULL UNIQUE,"      /* folder name used on media */
    "  added_ms INTEGER NOT NULL,"
    "  last_scan_ms INTEGER);"
    "CREATE TABLE IF NOT EXISTS nodes("
    "  id INTEGER PRIMARY KEY,"
    "  source_id INTEGER NOT NULL REFERENCES sources(id) ON DELETE CASCADE,"
    "  parent_id INTEGER REFERENCES nodes(id) ON DELETE CASCADE,"
    "  rel_path TEXT NOT NULL,"         /* '/'-separated, relative to the source */
    "  name TEXT NOT NULL,"
    "  is_dir INTEGER NOT NULL,"
    "  size INTEGER NOT NULL DEFAULT 0,"
    "  mtime_ns INTEGER NOT NULL DEFAULT 0,"
    "  current_version_id INTEGER,"
    "  deleted INTEGER NOT NULL DEFAULT 0,"
    "  last_seen_ms INTEGER NOT NULL,"
    "  UNIQUE(source_id, rel_path));"
    "CREATE INDEX IF NOT EXISTS nodes_parent ON nodes(source_id, parent_id);"
    "CREATE TABLE IF NOT EXISTS versions("
    "  id INTEGER PRIMARY KEY,"
    "  node_id INTEGER NOT NULL REFERENCES nodes(id) ON DELETE CASCADE,"
    "  version_no INTEGER NOT NULL,"
    "  hash TEXT NOT NULL,"             /* BLAKE3, hex */
    "  size INTEGER NOT NULL,"
    "  mtime_ns INTEGER NOT NULL,"
    "  first_seen_ms INTEGER NOT NULL,"
    "  UNIQUE(node_id, hash), UNIQUE(node_id, version_no));"
    "CREATE TABLE IF NOT EXISTS media("
    "  id INTEGER PRIMARY KEY,"
    "  uuid TEXT NOT NULL UNIQUE,"      /* stored in BRODALF.media on the drive */
    "  kind TEXT NOT NULL DEFAULT 'drive',"  /* drive, onedrive, dropbox */
    "  label TEXT NOT NULL,"
    "  last_root TEXT,"
    "  total_bytes INTEGER,"
    "  free_bytes INTEGER,"
    "  added_ms INTEGER NOT NULL,"
    "  last_seen_ms INTEGER);"
    "CREATE TABLE IF NOT EXISTS cloud_accounts("
    "  id INTEGER PRIMARY KEY,"
    "  media_id INTEGER NOT NULL UNIQUE REFERENCES media(id) ON DELETE CASCADE,"
    "  provider TEXT NOT NULL,"
    "  username TEXT NOT NULL,"
    "  root_path TEXT NOT NULL,"
    "  credential_ref TEXT);"           /* name in Windows Credential Manager, never the secret */
    "CREATE TABLE IF NOT EXISTS copies("
    "  id INTEGER PRIMARY KEY,"
    "  version_id INTEGER NOT NULL REFERENCES versions(id) ON DELETE CASCADE,"
    "  media_id INTEGER NOT NULL REFERENCES media(id) ON DELETE CASCADE,"
    "  path_on_media TEXT NOT NULL,"    /* relative to BRODALF/<catalog uuid> */
    "  encrypted INTEGER NOT NULL DEFAULT 0,"
    "  stored_size INTEGER NOT NULL,"
    "  stored_mtime_ns INTEGER NOT NULL,"
    "  written_ms INTEGER NOT NULL,"
    "  last_quick_check_ms INTEGER,"
    "  last_full_check_ms INTEGER,"
    "  state TEXT NOT NULL DEFAULT 'ok' CHECK(state IN ('ok','missing','bad')),"
    "  UNIQUE(version_id, media_id), UNIQUE(media_id, path_on_media));"
    "CREATE INDEX IF NOT EXISTS copies_version ON copies(version_id);"
    "CREATE TABLE IF NOT EXISTS jobs("
    "  id INTEGER PRIMARY KEY,"
    "  kind TEXT NOT NULL,"
    "  media_id INTEGER REFERENCES media(id) ON DELETE SET NULL,"
    "  started_ms INTEGER NOT NULL,"
    "  finished_ms INTEGER,"
    "  status TEXT NOT NULL,"
    "  files_done INTEGER NOT NULL DEFAULT 0,"
    "  files_failed INTEGER NOT NULL DEFAULT 0,"
    "  bytes_done INTEGER NOT NULL DEFAULT 0,"
    "  note TEXT);";

static void put_u32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static uint32_t get_u32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bd_catalog *catalog_alloc(const char *path)
{
    bd_catalog *cat = calloc(1, sizeof(*cat));
    if (!cat) return NULL;
    cat->path = bd_strdup(path);
    cat->lock_path = bd_sprintf("%s.lock", path);
    char tmp[1024], id[37];
    bd_uuid_v4(id);
    if (bd_temp_dir(tmp, sizeof(tmp)) == 0) cat->work_path = bd_sprintf("%s%cbrodalf-%s.db", tmp, BD_SEP, id);
    if (!cat->path || !cat->lock_path || !cat->work_path) {
        free(cat->path);
        free(cat->lock_path);
        free(cat->work_path);
        free(cat);
        return NULL;
    }
    return cat;
}

static void remove_work_files(bd_catalog *cat)
{
    const char *suffixes[] = {"", "-wal", "-shm", "-journal", ".snapshot"};
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
        char *p = bd_sprintf("%s%s", cat->work_path, suffixes[i]);
        if (p) { bd_remove(p); free(p); }
    }
}

static void catalog_free(bd_catalog *cat, int owns_lock)
{
    if (!cat) return;
    if (cat->db) sqlite3_close(cat->db);
    remove_work_files(cat);
    if (owns_lock) bd_remove(cat->lock_path);
    free(cat->path);
    free(cat->lock_path);
    free(cat->work_path);
    free(cat);
}

static char g_open_error[1024];

/* Open and create free the catalog on failure, so keep its message. */
static bd_status fail_open(bd_catalog *cat, bd_status s, int owns_lock)
{
    if (cat->err[0]) snprintf(g_open_error, sizeof(g_open_error), "%s", cat->err);
    else snprintf(g_open_error, sizeof(g_open_error), "%s: %s", cat->path, bd_status_name(s));
    catalog_free(cat, owns_lock);
    return s;
}

const char *bd_open_error(void)
{
    return g_open_error;
}

static bd_status take_lock(bd_catalog *cat)
{
    int rc = bd_create_exclusive(cat->lock_path);
    if (rc == 1)
        return bd_fail(cat, BD_ERR_LOCKED,
                       "%s is open in another BRODALF window. If none is running, delete %s and try again.",
                       cat->path, cat->lock_path);
    if (rc != 0) return bd_fail(cat, BD_ERR_IO, "cannot create lock file %s", cat->lock_path);
    return BD_OK;
}

static bd_status open_db(bd_catalog *cat)
{
    if (sqlite3_open_v2(cat->work_path, &cat->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "cannot open working database");
    sqlite3_busy_timeout(cat->db, 5000);
    if (bd_exec(cat, "PRAGMA foreign_keys=ON; PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;") != 0)
        return bd_fail_db(cat, "cannot configure database");
    if (bd_exec(cat, SCHEMA_SQL) != 0) return bd_fail_db(cat, "cannot create catalog tables");
    if (bd_exec(cat, "CREATE TEMP TABLE IF NOT EXISTS connected(media_id INTEGER PRIMARY KEY, root TEXT NOT NULL);") != 0)
        return bd_fail_db(cat, "cannot create session tables");
    return BD_OK;
}

static int meta_get(bd_catalog *cat, const char *key, char *out, size_t out_len)
{
    sqlite3_stmt *st;
    int found = 0;
    if (sqlite3_prepare_v2(cat->db, "SELECT value FROM meta WHERE key=?", -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) {
        snprintf(out, out_len, "%s", (const char *)sqlite3_column_text(st, 0));
        found = 1;
    }
    sqlite3_finalize(st);
    return found;
}

static int meta_set(bd_catalog *cat, const char *key, const char *value)
{
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(cat->db, "INSERT OR REPLACE INTO meta(key,value) VALUES(?,?)", -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, value, -1, SQLITE_STATIC);
    int rc = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(st);
    return rc;
}

static bd_status decompress_to(bd_catalog *cat, FILE *in, const char *out_path)
{
    FILE *out = bd_fopen(out_path, "wb");
    if (!out) return bd_fail(cat, BD_ERR_IO, "cannot create working copy %s", out_path);
    ZSTD_DCtx *dctx = ZSTD_createDCtx();
    size_t in_cap = ZSTD_DStreamInSize(), out_cap = ZSTD_DStreamOutSize();
    void *ibuf = malloc(in_cap), *obuf = malloc(out_cap);
    bd_status status = BD_OK;
    size_t last = 1;
    if (!dctx || !ibuf || !obuf) { status = bd_fail(cat, BD_ERR_NOMEM, "out of memory"); goto done; }
    for (;;) {
        size_t n = fread(ibuf, 1, in_cap, in);
        if (n == 0) break;
        ZSTD_inBuffer ib = {ibuf, n, 0};
        while (ib.pos < ib.size) {
            ZSTD_outBuffer ob = {obuf, out_cap, 0};
            last = ZSTD_decompressStream(dctx, &ob, &ib);
            if (ZSTD_isError(last)) {
                status = bd_fail(cat, BD_ERR_FORMAT, "%s is damaged: %s", cat->path, ZSTD_getErrorName(last));
                goto done;
            }
            if (fwrite(obuf, 1, ob.pos, out) != ob.pos) {
                status = bd_fail(cat, BD_ERR_IO, "cannot write working copy %s", out_path);
                goto done;
            }
        }
    }
    if (ferror(in)) status = bd_fail(cat, BD_ERR_IO, "cannot read %s", cat->path);
    else if (last != 0) status = bd_fail(cat, BD_ERR_FORMAT, "%s is truncated", cat->path);
done:
    ZSTD_freeDCtx(dctx);
    free(ibuf);
    free(obuf);
    if (fclose(out) != 0 && status == BD_OK) status = bd_fail(cat, BD_ERR_IO, "cannot write working copy %s", out_path);
    return status;
}

static bd_status compress_to(bd_catalog *cat, const char *in_path, FILE *out)
{
    FILE *in = bd_fopen(in_path, "rb");
    if (!in) return bd_fail(cat, BD_ERR_IO, "cannot read snapshot %s", in_path);
    ZSTD_CCtx *cctx = ZSTD_createCCtx();
    size_t in_cap = ZSTD_CStreamInSize(), out_cap = ZSTD_CStreamOutSize();
    void *ibuf = malloc(in_cap), *obuf = malloc(out_cap);
    bd_status status = BD_OK;
    if (!cctx || !ibuf || !obuf) { status = bd_fail(cat, BD_ERR_NOMEM, "out of memory"); goto done; }
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, 9);
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_checksumFlag, 1);
    for (;;) {
        size_t n = fread(ibuf, 1, in_cap, in);
        int last = n < in_cap;
        if (last && ferror(in)) { status = bd_fail(cat, BD_ERR_IO, "cannot read snapshot %s", in_path); goto done; }
        ZSTD_EndDirective mode = last ? ZSTD_e_end : ZSTD_e_continue;
        ZSTD_inBuffer ib = {ibuf, n, 0};
        int finished;
        do {
            ZSTD_outBuffer ob = {obuf, out_cap, 0};
            size_t remaining = ZSTD_compressStream2(cctx, &ob, &ib, mode);
            if (ZSTD_isError(remaining)) {
                status = bd_fail(cat, BD_ERR_IO, "compression failed: %s", ZSTD_getErrorName(remaining));
                goto done;
            }
            if (fwrite(obuf, 1, ob.pos, out) != ob.pos) { status = bd_fail(cat, BD_ERR_IO, "cannot write catalog"); goto done; }
            finished = last ? (remaining == 0) : (ib.pos == ib.size);
        } while (!finished);
        if (last) break;
    }
done:
    ZSTD_freeCCtx(cctx);
    free(ibuf);
    free(obuf);
    fclose(in);
    return status;
}

bd_status bd_catalog_save(bd_catalog *cat)
{
    if (!cat || !cat->db) return BD_ERR_INVALID;
    char *snapshot = bd_sprintf("%s.snapshot", cat->work_path);
    char *saving = bd_sprintf("%s.saving", cat->path);
    bd_status status = BD_OK;
    FILE *out = NULL;
    if (!snapshot || !saving) { status = bd_fail(cat, BD_ERR_NOMEM, "out of memory"); goto done; }
    bd_remove(snapshot);

    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(cat->db, "VACUUM INTO ?", -1, &st, NULL) != SQLITE_OK) { status = bd_fail_db(cat, "cannot snapshot catalog"); goto done; }
    sqlite3_bind_text(st, 1, snapshot, -1, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) { status = bd_fail_db(cat, "cannot snapshot catalog"); goto done; }

    out = bd_fopen(saving, "wb");
    if (!out) { status = bd_fail(cat, BD_ERR_IO, "cannot write %s", saving); goto done; }
    unsigned char header[16];
    memcpy(header, BD_MAGIC, 8);
    put_u32(header + 8, BD_FORMAT_VERSION);
    put_u32(header + 12, 0);
    if (fwrite(header, 1, sizeof(header), out) != sizeof(header)) { status = bd_fail(cat, BD_ERR_IO, "cannot write %s", saving); goto done; }
    status = compress_to(cat, snapshot, out);
    if (status != BD_OK) goto done;
    if (bd_fsync(out) != 0) { status = bd_fail(cat, BD_ERR_IO, "cannot flush %s to disk", saving); goto done; }
    fclose(out);
    out = NULL;
    if (bd_rename_replace(saving, cat->path) != 0) status = bd_fail(cat, BD_ERR_IO, "cannot replace %s", cat->path);

done:
    if (out) fclose(out);
    if (status != BD_OK && saving) bd_remove(saving);
    if (snapshot) { bd_remove(snapshot); free(snapshot); }
    free(saving);
    return status;
}

bd_status bd_catalog_create(const char *path, bd_catalog **out)
{
    *out = NULL;
    bd_stat_t st;
    g_open_error[0] = '\0';
    if (bd_stat(path, &st) == 0) {
        snprintf(g_open_error, sizeof(g_open_error), "%s already exists", path);
        return BD_ERR_EXISTS;
    }
    bd_catalog *cat = catalog_alloc(path);
    if (!cat) return BD_ERR_NOMEM;
    bd_status s = take_lock(cat);
    if (s != BD_OK) return fail_open(cat, s, 0);
    bd_remove(cat->work_path);
    s = open_db(cat);
    if (s == BD_OK) {
        char buf[32];
        bd_uuid_v4(cat->uuid);
        snprintf(buf, sizeof(buf), "%d", BD_SCHEMA_VERSION);
        char created[32];
        snprintf(created, sizeof(created), "%lld", (long long)bd_now_ms());
        if (meta_set(cat, "catalog_uuid", cat->uuid) != 0 || meta_set(cat, "schema_version", buf) != 0 ||
            meta_set(cat, "created_ms", created) != 0)
            s = bd_fail_db(cat, "cannot initialise catalog");
    }
    if (s == BD_OK) s = bd_catalog_save(cat);
    if (s != BD_OK) {
        bd_remove(path);
        return fail_open(cat, s, 1);
    }
    *out = cat;
    return BD_OK;
}

bd_status bd_catalog_open(const char *path, bd_catalog **out)
{
    *out = NULL;
    g_open_error[0] = '\0';
    bd_catalog *cat = catalog_alloc(path);
    if (!cat) return BD_ERR_NOMEM;
    FILE *in = bd_fopen(path, "rb");
    if (!in) return fail_open(cat, bd_fail(cat, BD_ERR_NOT_FOUND, "cannot open %s", path), 0);
    bd_status s = take_lock(cat);
    if (s != BD_OK) { fclose(in); return fail_open(cat, s, 0); }

    unsigned char header[16];
    if (fread(header, 1, sizeof(header), in) != sizeof(header) || memcmp(header, BD_MAGIC, 8) != 0) {
        fclose(in);
        return fail_open(cat, bd_fail(cat, BD_ERR_FORMAT, "%s is not a BRODALF catalog", path), 1);
    }
    uint32_t version = get_u32(header + 8), flags = get_u32(header + 12);
    if (version > BD_FORMAT_VERSION || (flags & 1u)) {
        fclose(in);
        return fail_open(cat, bd_fail(cat, BD_ERR_FORMAT, "%s uses a newer or encrypted format this build cannot read", path), 1);
    }
    s = decompress_to(cat, in, cat->work_path);
    fclose(in);
    if (s == BD_OK) s = open_db(cat);
    if (s == BD_OK && !meta_get(cat, "catalog_uuid", cat->uuid, sizeof(cat->uuid)))
        s = bd_fail(cat, BD_ERR_FORMAT, "%s has no catalog id", path);
    if (s == BD_OK) {
        char v[32];
        if (meta_get(cat, "schema_version", v, sizeof(v)) && atoi(v) > BD_SCHEMA_VERSION)
            s = bd_fail(cat, BD_ERR_FORMAT, "%s was made by a newer BRODALF", path);
    }
    if (s != BD_OK) return fail_open(cat, s, 1);
    *out = cat;
    return BD_OK;
}

void bd_catalog_close(bd_catalog *cat)
{
    catalog_free(cat, 1);
}

const char *bd_catalog_error(const bd_catalog *cat)
{
    return cat ? cat->err : "no catalog";
}

const char *bd_catalog_uuid(const bd_catalog *cat)
{
    return cat->uuid;
}
