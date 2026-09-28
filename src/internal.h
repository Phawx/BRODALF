#ifndef BD_INTERNAL_H
#define BD_INTERNAL_H

#include "brodalf.h"
#include "platform.h"
#include "sqlite3.h"

#include <stdarg.h>
#include <stdio.h>

#define BD_HASH_HEX_LEN 64
#define BD_MEDIA_DIR "BRODALF"
#define BD_MEDIA_FILE "BRODALF.media"
#define BD_VERSIONS_DIR ".versions"
#define BD_TMP_MARKER ".brodalf-tmp"

struct bd_catalog {
    sqlite3 *db;
    char *path;       /* the .brodalf file */
    char *lock_path;
    char *work_path;  /* unpacked SQLite working copy */
    char uuid[37];
    char err[1024];
    bd_progress_fn progress;
    void *progress_ctx;
    int64_t progress_last_ms;
};

/* Record an error message on the catalog and return status. */
bd_status bd_fail(bd_catalog *cat, bd_status status, const char *fmt, ...);
bd_status bd_fail_db(bd_catalog *cat, const char *what);
void bd_logf(bd_log_fn log, void *ctx, const char *fmt, ...);

char *bd_strdup(const char *s);
char *bd_sprintf(const char *fmt, ...);
/* Join with '/' (platform functions accept either separator). */
char *bd_path_join(const char *a, const char *b);
/* Directory part of a '/'-joined relative path ("" if none). */
char *bd_rel_dirname(const char *rel);
const char *bd_rel_basename(const char *rel);

/* Hash a file with BLAKE3. If copy_to is non-NULL the bytes are also written
 * there. hex_out receives 64 hex chars plus NUL. 0 on success; on failure
 * -1 for read errors, -2 for write errors. */
int bd_hash_file(const char *path, FILE *copy_to, char hex_out[BD_HASH_HEX_LEN + 1], int64_t *size_out);

void bd_uuid_v4(char out[37]);

/* Media root directory for this catalog: <root>/BRODALF/<uuid>. */
char *bd_media_catalog_dir(const bd_catalog *cat, const char *root);
/* Root of a connected media, or NULL (caller frees). */
char *bd_connected_root(bd_catalog *cat, int64_t media_id);

int bd_exec(bd_catalog *cat, const char *sql);

/* Report progress, throttled unless force is set. */
void bd_report(bd_catalog *cat, const char *phase, int64_t files, int64_t bytes, const char *current, int force);

#endif
