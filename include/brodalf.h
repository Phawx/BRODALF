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
    BD_ERR_NOMEM,
    BD_ERR_PASSPHRASE /* a passphrase is needed, or the one given is wrong */
} bd_status;

typedef void (*bd_log_fn)(void *ctx, const char *message);

/* ---- Catalog file ---------------------------------------------------- */

/* Create a new, empty catalog file. Fails with BD_ERR_EXISTS if the file is
 * already there. The catalog stays open and locked. */
bd_status bd_catalog_create(const char *path, bd_catalog **out);

/* Open an existing catalog. It is unpacked to a working copy in the temp
 * folder and a "<path>.lock" file is created next to it. An encrypted
 * catalog file fails with BD_ERR_PASSPHRASE; use bd_catalog_open_with. */
bd_status bd_catalog_open(const char *path, bd_catalog **out);

/* Open with a passphrase. For an encrypted catalog file the catalog comes
 * back unlocked; for a plain one the passphrase is ignored. */
bd_status bd_catalog_open_with(const char *path, const char *passphrase, bd_catalog **out);

/* 1 if the file at path is an encrypted catalog (needs a passphrase). */
int bd_catalog_file_needs_passphrase(const char *path);

/* Write the working copy back to the .brodalf file atomically. */
bd_status bd_catalog_save(bd_catalog *cat);

/* Close without saving, remove the working copy and the lock. */
void bd_catalog_close(bd_catalog *cat);

const char *bd_catalog_error(const bd_catalog *cat);
/* Why the last bd_catalog_create or bd_catalog_open failed. */
const char *bd_open_error(void);
const char *bd_catalog_uuid(const bd_catalog *cat);
const char *bd_status_name(bd_status status);

/* ---- Encryption ------------------------------------------------------ */

/* A catalog can have a passphrase. It protects a random master key that
 * encrypts file contents on encrypted drives (names stay readable) and,
 * optionally, the .brodalf file itself. Losing the passphrase means losing
 * access to everything encrypted with it. */
int bd_catalog_has_passphrase(bd_catalog *cat);
int bd_catalog_is_unlocked(bd_catalog *cat);
/* Set the first passphrase, or change it (the catalog must be unlocked).
 * At least 8 characters. Leaves the catalog unlocked. */
bd_status bd_catalog_set_passphrase(bd_catalog *cat, const char *passphrase);
/* BD_ERR_PASSPHRASE if wrong. */
bd_status bd_catalog_unlock(bd_catalog *cat, const char *passphrase);
/* Forget the master key until the next unlock. */
void bd_catalog_lock_key(bd_catalog *cat);
/* Encrypt the .brodalf file from the next save on. Needs an unlocked
 * catalog to turn on. */
bd_status bd_catalog_set_file_encrypted(bd_catalog *cat, int on);
int bd_catalog_file_encrypted(bd_catalog *cat);

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
 * <root>/BRODALF/<catalog-uuid>/BRODALF.media with a new media ID.
 * BD_MEDIA_ENCRYPTED makes every copy on it encrypted (the catalog must
 * have a passphrase and be unlocked); this cannot be changed later. */
#define BD_MEDIA_ENCRYPTED 1u
bd_status bd_media_init(bd_catalog *cat, const char *root, const char *label, unsigned flags, int64_t *out_media_id);

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
    int64_t skipped;   /* encrypted copies that need the passphrase to check */
} bd_check_stats;

bd_status bd_media_connect(bd_catalog *cat, const char *root, int64_t *out_media_id,
                           bd_check_stats *stats, bd_log_fn log, void *log_ctx);
void bd_media_disconnect(bd_catalog *cat, int64_t media_id);

/* Where a drive is kept, or what is written on it ("Box A", "top shelf").
 * Optional; "" or NULL clears it. */
bd_status bd_media_set_location(bd_catalog *cat, int64_t media_id, const char *location);

/* Rename a drive. The new name is also written to its BRODALF.media file
 * the next time the drive is plugged in. */
bd_status bd_media_rename(bd_catalog *cat, int64_t media_id, const char *label);

/* Re-check every copy on a connected drive. full=0 checks existence, size
 * and modified time (rehashing only when the time differs); full=1 rehashes
 * every copy. */
bd_status bd_media_check(bd_catalog *cat, int64_t media_id, int full,
                         bd_check_stats *stats, bd_log_fn log, void *log_ctx);

/* ---- Cloud storage --------------------------------------------------- */

/* OneDrive and Dropbox accounts work like drives. BRODALF keeps its files in
 * the service's app folder (Apps/BRODALF) and can see nothing else there.
 * Signing in happens once in the browser; the refresh token is saved in
 * Windows Credential Manager and the catalog records only the provider,
 * the account name and where the credential is. */
typedef enum { BD_CLOUD_ONEDRIVE = 1, BD_CLOUD_DROPBOX = 2 } bd_cloud_provider;
typedef struct bd_signin bd_signin;

/* Start signing in: returns the address to open in a browser. The
 * provider sends the browser back to http://localhost:53682/. */
bd_status bd_cloud_signin_begin(bd_catalog *cat, bd_cloud_provider provider, bd_signin **out, const char **url_out);
/* Wait for the browser to come back, then finish signing in. */
bd_status bd_cloud_signin_finish(bd_catalog *cat, bd_signin *signin, int timeout_ms);
/* The account's name ("you@example.com"), once signed in. */
const char *bd_cloud_signin_account(const bd_signin *signin);
void bd_cloud_signin_free(bd_signin *signin);

/* Use the signed-in account as storage. flags as for bd_media_init. If the
 * account is already storage for this catalog, this signs it in again
 * (label and flags are ignored) and returns its media id. */
bd_status bd_cloud_add(bd_catalog *cat, bd_signin *signin, const char *label, unsigned flags, int64_t *out_media_id);

/* Connect a cloud account with its saved sign-in and run a quick check. */
bd_status bd_cloud_connect(bd_catalog *cat, int64_t media_id, bd_check_stats *stats, bd_log_fn log, void *log_ctx);

/* Forget the saved sign-in (the files in the cloud stay). */
bd_status bd_cloud_sign_out(bd_catalog *cat, int64_t media_id);

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
 * BRODALF/<catalog-uuid>/catalog-backup.brodalf. The copy is encrypted
 * when the catalog file or the drive is. */
bd_status bd_catalog_copy_to_media(bd_catalog *cat, int64_t media_id);

/* Save the catalog, then put a copy of it on every connected cloud account
 * (BRODALF/<catalog-uuid>/catalog-backup.brodalf there). A cloud copy that
 * fails is logged and does not fail the save. */
bd_status bd_catalog_save_all(bd_catalog *cat, bd_log_fn log, void *log_ctx);

typedef struct {
    int64_t files_restored;
    int64_t files_offline;    /* copies exist but no drive holding one is connected */
    int64_t files_no_copy;
    int64_t files_failed;
    int64_t files_need_passphrase; /* only on an encrypted drive, and the catalog is locked */
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
    int encrypted;           /* the copy is on an encrypted drive */
    const char *media_location; /* where that drive is kept; "" if not set */
} bd_copy_info;

/* What a drive says about itself, read whenever it is plugged in. Strings
 * are "" and numbers -1 when unknown. SMART data needs a drive and
 * connection that pass it through: many USB enclosures do not, and some
 * drives only answer when BRODALF runs as administrator (note says why). */
typedef struct {
    char vendor[64];
    char model[128];
    char serial[128];
    char firmware[32];
    char bus[32];            /* "USB", "SATA", "NVMe", ... */
    int64_t disk_bytes;      /* the whole disk, not just this volume */
    char volume_name[64];
    char volume_serial[16];  /* "1A2B-3C4D" */
    char filesystem[32];
    int smart;               /* 1 when health data was read */
    char health[16];         /* "good", "warning", "failing", or "" */
    int temperature_c;
    int64_t power_on_hours;
    int64_t power_cycles;
    int64_t reallocated_sectors;
    int64_t pending_sectors;
    int64_t uncorrectable_sectors;
    int percent_used;        /* SSD wear, 0-100+ */
    char note[160];
} bd_drive_hw;

typedef struct {
    int64_t media_id;
    const char *label;
    const char *kind;        /* "drive", "onedrive" or "dropbox" */
    const char *last_root;   /* where it was last seen, e.g. "E:\" */
    int connected;
    int64_t total_bytes;     /* 0 if unknown */
    int64_t free_bytes;
    int64_t last_seen_ms;
    int64_t copies;          /* copies BRODALF has recorded on it */
    int encrypted;
    const char *location;    /* where it is kept, e.g. "Box A"; "" if not set */
    const bd_drive_hw *hw;   /* last hardware reading, NULL if none */
    int64_t hw_read_ms;
    int64_t added_ms;
    /* The oldest good copy on it by when it was last fully checked (or
     * written, if never checked); 0 if it holds no copies. */
    int64_t oldest_check_ms;
    int check_due;           /* older than the check_days option */
} bd_media_info;

/* Read make, model, serial and SMART data for the disk holding root. 0 on
 * success (even if only some fields could be read), -1 if nothing is known
 * (not Windows, a network share, or the disk refused every query). */
int bd_drive_hw_read(const char *root, bd_drive_hw *out);

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

/* ---- Options ------------------------------------------------------------ */

/* Settings kept in the catalog:
 *   "auto_backup"  1 (default): scan and back up to a drive as soon as it
 *                  is plugged in; 0: only when asked.
 *   "check_days"   remind to run a full check on a drive whose oldest copy
 *                  was last read this many days ago (default 180; 0: never).
 * bd_option_get returns the default for an unset option and -1 for an
 * unknown name. */
int bd_option_get(bd_catalog *cat, const char *name);
bd_status bd_option_set(bd_catalog *cat, const char *name, int value);

/* ---- Search -------------------------------------------------------------- */

typedef struct {
    int64_t node_id;
    int64_t source_id;
    const char *source_name;
    const char *rel_path;
    const char *name;
    int is_dir;
    int64_t size;            /* files only */
    bd_node_state state;     /* as in the ghost tree */
    /* Drives and accounts holding a good copy of the current version (for
     * a folder: of any file inside), as "Label (kept in)" joined by "; ".
     * "" if none. */
    const char *where;
} bd_search_info;

typedef int (*bd_search_fn)(void *ctx, const bd_search_info *info);

/* Files and folders whose path (within their source) contains every word
 * of text, ignoring case. Names that match come first. Deleted files are
 * included. At most limit results (0: 500). */
bd_status bd_search(bd_catalog *cat, const char *text, int limit, bd_search_fn fn, void *ctx);

/* ---- Protection target and files at risk ------------------------------- */

/* How well every file should be protected. A copy counts when BRODALF last
 * saw it good; each drive or cloud account counts once. A place is what a
 * drive's "kept in" says (compared ignoring case and outer spaces); every
 * cloud account is a place of its own, and drives with nothing set count
 * together as one unknown place. */
typedef struct {
    int copies;  /* good copies of each file's current version (default 2) */
    int places;  /* how many different places they must be in (default 2; 1 turns this off) */
} bd_target;

bd_status bd_target_get(bd_catalog *cat, bd_target *out);
/* copies 1..16, places 1..copies. */
bd_status bd_target_set(bd_catalog *cat, const bd_target *target);

typedef struct {
    int64_t node_id;
    int64_t source_id;
    const char *source_name;
    const char *rel_path;
    int64_t size;
    int copies;         /* good copies of the current version */
    int places;         /* different places they are in */
    int unknown_place;  /* one of those places is drives with no "kept in" set */
    int older_copies;   /* copies exist, but only of older versions: changed since the last backup */
} bd_risk_info;

typedef struct {
    int64_t files_total;    /* live files in the catalog (or the source) */
    int64_t files_at_risk;  /* short of the target */
    int64_t files_no_copy;  /* no good copy of the current version anywhere */
    int64_t bytes_at_risk;
} bd_risk_stats;

typedef int (*bd_risk_fn)(void *ctx, const bd_risk_info *info);

/* Live files whose current version falls short of the target, fewest copies
 * first. source_id 0 means every source. fn may be NULL to only count;
 * stats may be NULL. */
bd_status bd_list_at_risk(bd_catalog *cat, int64_t source_id, bd_risk_fn fn, void *ctx, bd_risk_stats *stats);

typedef struct {
    int64_t media_id;
    const char *label;
    const char *kind;      /* "drive", "onedrive" or "dropbox" */
    const char *location;  /* "" if not set */
    int connected;
    int64_t files;         /* at-risk files a backup here would bring closer to the target */
    int64_t bytes;
} bd_risk_help;

typedef int (*bd_risk_help_fn)(void *ctx, const bd_risk_help *info);

/* Which drive to plug in next: for every drive and cloud account, how many
 * at-risk files a backup to it would help (it has no copy yet and adds a
 * copy the file needs or a place it is missing). Most helpful first;
 * drives that would not help are left out. */
bd_status bd_list_risk_help(bd_catalog *cat, bd_risk_help_fn fn, void *ctx);

/* ---- App log and error reports -------------------------------------- */

/* "0.2.0" or "0.2.0 (abc1234)" when the build knows its commit. */
const char *bd_version(void);

/* Where the app log goes by default: %LOCALAPPDATA%\BRODALF\brodalf.log on
 * Windows, $XDG_STATE_HOME/brodalf/brodalf.log (or ~/.local/state/...)
 * elsewhere. The environment variable BRODALF_LOG overrides it. malloc'd. */
char *bd_applog_default_path(void);

/* Start writing the app log to path (its folder is created). Each line gets
 * a timestamp. When the file passes about 1 MB it is moved to path.old and a
 * new one is started. Call these from one thread only. */
bd_status bd_applog_open(const char *path);
void bd_applog(const char *fmt, ...);
const char *bd_applog_path(void);

/* The text of a problem report: what happened, the BRODALF version and
 * operating system, and the end of the app log. The user's name, computer
 * name, email addresses and sign-in tokens are replaced before it is
 * returned. malloc'd. */
char *bd_report_body(const char *what_happened);

/* Save an error report to a new text file in a "reports" folder next to
 * the app log (or the temp folder). The file starts with instructions for
 * posting it as a GitHub issue by hand, followed by bd_report_body. Nothing
 * is sent anywhere. Returns the file's path (malloc'd) or NULL. */
char *bd_report_save(const char *what_happened);

/* The BRODALF issues page on GitHub. */
const char *bd_issues_url(void);

/* Replace the user's name, computer name, email addresses and tokens in text.
 * malloc'd. */
char *bd_redact(const char *text);

#ifdef __cplusplus
}
#endif

#endif
