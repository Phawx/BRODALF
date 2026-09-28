/* BRODALF core library.
 *
 * A catalog (.brodalf file) records every file and folder in the source
 * folders you protect, every version of each file BRODALF has seen, and every
 * place a copy of each version was written. It never holds file data.
 *
 * All strings are UTF-8. Paths may use '/' or '\\'. Functions return
 * BD_OK on success; on failure bd_catalog_error() describes the problem.
 */
#ifndef BRODALF_H
#define BRODALF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bd_catalog bd_catalog;

typedef enum {
    BD_OK = 0,
    BD_ERR_IO,
    BD_ERR_DB,
    BD_ERR_FORMAT,   /* not a .brodalf file, or a newer format */
    BD_ERR_LOCKED,   /* another BRODALF has this catalog open */
    BD_ERR_EXISTS,
    BD_ERR_NOT_FOUND,
    BD_ERR_INVALID,
    BD_ERR_NOMEM
} bd_status;

typedef void (*bd_log_fn)(void *ctx, const char *message);

/* ---- Catalog file ---------------------------------------------------- */

/* Create a new, empty catalog file. Fails with BD_ERR_EXISTS if the file is
 * already there. The catalog stays open and locked. */
bd_status bd_catalog_create(const char *path, bd_catalog **out);

/* Open an existing catalog. It is unpacked to a working copy in the temp
 * folder and a "<path>.lock" file is created next to it. */
bd_status bd_catalog_open(const char *path, bd_catalog **out);

/* Write the working copy back to the .brodalf file atomically. */
bd_status bd_catalog_save(bd_catalog *cat);

/* Close without saving, remove the working copy and the lock. */
void bd_catalog_close(bd_catalog *cat);

const char *bd_catalog_error(const bd_catalog *cat);
/* Why the last bd_catalog_create or bd_catalog_open failed. */
const char *bd_open_error(void);
const char *bd_catalog_uuid(const bd_catalog *cat);
const char *bd_status_name(bd_status status);

/* Progress for long jobs (scan, backup, check, restore), called at most
 * about ten times a second from the thread running the job. files_done and
 * bytes_done count up from zero; current is the file being worked on. */
typedef void (*bd_progress_fn)(void *ctx, const char *phase, int64_t files_done, int64_t bytes_done, const char *current);
void bd_catalog_set_progress(bd_catalog *cat, bd_progress_fn fn, void *ctx);

/* ---- Sources and scanning -------------------------------------------- */

bd_status bd_source_add(bd_catalog *cat, const char *folder, int64_t *out_source_id);

typedef struct {
    int64_t files_seen;
    int64_t dirs_seen;
    int64_t files_new;
    int64_t files_changed;   /* a new version was recorded */
    int64_t files_deleted;   /* in the catalog but no longer on disk */
    int64_t bytes_hashed;
    int64_t skipped_links;
    int64_t errors;
} bd_scan_stats;

/* Scan every source folder. New or changed files are hashed (BLAKE3) and get
 * a new version. Unreadable entries are logged and skipped. */
bd_status bd_scan(bd_catalog *cat, bd_scan_stats *stats, bd_log_fn log, void *log_ctx);

/* ---- Storage (media) ------------------------------------------------- */

/* Prepare a drive or folder as storage for this catalog. Writes
 * <root>/BRODALF/<catalog-uuid>/BRODALF.media with a new media ID. */
bd_status bd_media_init(bd_catalog *cat, const char *root, const char *label, int64_t *out_media_id);

/* Tell BRODALF a known drive is connected at root. Identifies it by its
 * BRODALF.media file (not by drive letter), records the mount and free
 * space, and runs a quick check of its copies. Files with a good copy on
 * connected storage stop being greyed out. */
typedef struct {
    int64_t copies;
    int64_t ok;
    int64_t missing;
    int64_t bad;
    int64_t rehashed;
} bd_check_stats;

bd_status bd_media_connect(bd_catalog *cat, const char *root, int64_t *out_media_id,
                           bd_check_stats *stats, bd_log_fn log, void *log_ctx);
void bd_media_disconnect(bd_catalog *cat, int64_t media_id);

/* Re-check every copy on a connected drive. full=0 checks existence, size
 * and modified time (rehashing only when the time differs); full=1 rehashes
 * every copy. */
bd_status bd_media_check(bd_catalog *cat, int64_t media_id, int full,
                         bd_check_stats *stats, bd_log_fn log, void *log_ctx);

/* ---- Backup and restore ---------------------------------------------- */

typedef struct {
    int64_t files_considered;
    int64_t files_copied;
    int64_t files_already_there;
    int64_t files_failed;
    int64_t versions_moved;   /* older copies moved into .versions */
    int64_t bytes_copied;
} bd_backup_stats;

/* Copy the current version of every file that has no good copy on this
 * drive. The drive must be connected. source_id 0 means all sources. The
 * previous copy of a changed file is kept under .versions, never
 * overwritten. */
bd_status bd_backup(bd_catalog *cat, int64_t media_id, int64_t source_id,
                    bd_backup_stats *stats, bd_log_fn log, void *log_ctx);

/* Save the catalog and put a copy of it on the drive as
 * BRODALF/<catalog-uuid>/catalog-backup.brodalf. */
bd_status bd_catalog_copy_to_media(bd_catalog *cat, int64_t media_id);

typedef struct {
    int64_t files_restored;
    int64_t files_offline;    /* copies exist but no drive holding one is connected */
    int64_t files_no_copy;
    int64_t files_failed;
    int64_t bytes_restored;
} bd_restore_stats;

/* Restore the current version of every file in a source (optionally only
 * under rel_prefix) into dest_root/<source name>/..., from connected
 * storage. Every restored file is checked against its BLAKE3 hash. */
bd_status bd_restore(bd_catalog *cat, int64_t source_id, const char *rel_prefix, const char *dest_root,
                     bd_restore_stats *stats, bd_log_fn log, void *log_ctx);

/* ---- Ghost tree queries ---------------------------------------------- */

typedef enum {
    BD_STATE_AVAILABLE = 0,    /* current version has a good copy on connected storage */
    BD_STATE_AVAILABLE_OLDER,  /* only an older version is reachable */
    BD_STATE_OFFLINE,          /* copies exist, none on connected storage */
    BD_STATE_NO_COPY,          /* never backed up */
    BD_STATE_BAD,              /* the copy on connected storage is missing or damaged */
    BD_STATE_DELETED,          /* gone from the source folder, copies still tracked */
    BD_STATE_PARTIAL           /* folders only: some files inside are available */
} bd_node_state;

const char *bd_node_state_name(bd_node_state state);

typedef struct {
    int64_t source_id;
    const char *name;
    const char *path;
    bd_node_state state;     /* AVAILABLE, PARTIAL, OFFLINE or NO_COPY, as for folders */
    int64_t files_total;     /* live files in the source */
    int64_t files_available;
    const char *offline_media_label; /* OFFLINE: a drive to plug in */
} bd_source_info;

typedef struct {
    int64_t node_id;
    int64_t source_id;
    int is_dir;
    const char *name;
    const char *rel_path;
    int64_t size;
    bd_node_state state;     /* folders: AVAILABLE, PARTIAL, OFFLINE or NO_COPY from the files inside */
    int version_no;          /* 1-based number of the current version */
    int version_count;
    int64_t files_total;     /* folders only: live files inside, at any depth */
    int64_t files_available; /* folders only */
    const char *offline_media_label; /* OFFLINE files: a drive to plug in */
} bd_node_info;

typedef struct {
    int version_no;
    int is_current;
    const char *hash;
    int64_t size;
    int64_t mtime_ns;
    int64_t first_seen_ms;
    int64_t media_id;        /* 0 if this version has no copy */
    const char *media_label;
    const char *path_on_media;
    const char *copy_state;  /* "ok", "missing", "bad" */
    int connected;
    int64_t last_check_ms;
} bd_copy_info;

typedef struct {
    int64_t media_id;
    const char *label;
    const char *kind;        /* "drive" for now; "onedrive", "dropbox" later */
    const char *last_root;   /* where it was last seen, e.g. "E:\" */
    int connected;
    int64_t total_bytes;     /* 0 if unknown */
    int64_t free_bytes;
    int64_t last_seen_ms;
    int64_t copies;          /* copies BRODALF has recorded on it */
} bd_media_info;

typedef int (*bd_media_fn)(void *ctx, const bd_media_info *info);
bd_status bd_list_media(bd_catalog *cat, bd_media_fn fn, void *ctx);

typedef int (*bd_source_fn)(void *ctx, const bd_source_info *info);
typedef int (*bd_node_fn)(void *ctx, const bd_node_info *info);
typedef int (*bd_copy_fn)(void *ctx, const bd_copy_info *info);

bd_status bd_list_sources(bd_catalog *cat, bd_source_fn fn, void *ctx);

/* Children of a folder; parent_node_id 0 lists the top of the source.
 * Folders come first, then files, each sorted by name. */
bd_status bd_list_children(bd_catalog *cat, int64_t source_id, int64_t parent_node_id, bd_node_fn fn, void *ctx);

bd_status bd_find_node(bd_catalog *cat, int64_t source_id, const char *rel_path, int64_t *out_node_id);

/* Every version of a file and every copy of each version. */
bd_status bd_list_copies(bd_catalog *cat, int64_t node_id, bd_copy_fn fn, void *ctx);

#ifdef __cplusplus
}
#endif

#endif
