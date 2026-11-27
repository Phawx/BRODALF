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

#include <stddef.h>
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
    BD_ERR_PASSPHRASE, /* a passphrase is needed, or the one given is wrong */
    BD_ERR_CANCELLED,  /* stopped by bd_catalog_cancel; what was done until then is kept */
    BD_ERR_DRIVE_GONE  /* a drive the job was using was unplugged, stopped answering or was
                          swapped for another: the job stopped, what it finished is kept,
                          and nothing on that drive was marked missing or damaged */
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
 * back unlocked, and it is unpacked into memory instead of the temp folder:
 * nothing of it is written to the disk unencrypted while it is open. For a
 * plain one the passphrase is ignored. */
bd_status bd_catalog_open_with(const char *path, const char *passphrase, bd_catalog **out);

/* Start again without the catalog file, from a copy of it a drive holds
 * (BRODALF\<catalog id>\catalog-r<revision>.brodalf, the highest
 * revision being the newest; drives written by 0.6.0 and older hold
 * catalog-backup.brodalf): copy it to dest_path,
 * which must not exist yet, then open dest_path as usual. Fails with
 * BD_ERR_FORMAT if backup_path is not a catalog; bd_open_error says why. */
bd_status bd_catalog_recover(const char *backup_path, const char *dest_path);

/* After opening a recovered catalog: look for paths in it that a restore
 * will not write (empty parts, ".", "..", absolute paths, and on Windows
 * ':' or '\\'), as a catalog from a drive may hold anything. Each is
 * logged (the first 20) and counted in *unsafe. Nothing is changed; a
 * restore never writes such a file, logs it and counts it as failed, and a
 * backup does not copy it. */
bd_status bd_check_stored_paths(bd_catalog *cat, bd_log_fn log, void *log_ctx, int64_t *unsafe);

/* The catalogs a drive holds copies of, to recover from. dir may be the
 * drive, its BRODALF folder, or one catalog's folder on it. For each
 * catalog (one folder per catalog id) fn gets its newest copy: the highest
 * catalog-r<revision>.brodalf, or catalog-backup.brodalf (revision 0) on a
 * drive written by 0.6.0 or older. Revisions of one catalog compare across
 * drives: the highest is the newest. BD_ERR_NOT_FOUND if there is none. */
typedef struct {
    const char *catalog_id;  /* the folder's name: the catalog's id */
    const char *path;        /* the newest copy in it */
    int64_t revision;        /* 0: catalog-backup.brodalf */
    int64_t saved_ms;        /* the copy's modified time */
} bd_catalog_copy_info;
typedef int (*bd_catalog_copy_fn)(void *ctx, const bd_catalog_copy_info *info);
bd_status bd_find_catalog_copies(const char *dir, bd_catalog_copy_fn fn, void *ctx);

/* Every copy of one catalog in a folder (one catalog's folder on a drive,
 * as bd_find_catalog_copies gives in info->path's folder), newest first:
 * to try the next older one when the newest will not open. */
bd_status bd_find_catalog_revisions(const char *folder, bd_catalog_copy_fn fn, void *ctx);

/* Read the whole catalog copy at path (one on a drive) through, as opening
 * it would, without keeping it: BD_OK if it reads, BD_ERR_FORMAT if it is
 * damaged or not a catalog, BD_ERR_PASSPHRASE if it is encrypted and cat
 * is locked (it was not read). An encrypted copy is read with cat's key. */
bd_status bd_catalog_copy_readable(bd_catalog *cat, const char *path);

/* 1 if path is a copy of a catalog on a backup drive: named like one
 * (catalog-r<revision>.brodalf, catalog-backup.brodalf) or inside a
 * BRODALF/<catalog id> folder. Opening it would save over it, so it is
 * recovered instead (bd_catalog_recover). */
int bd_path_is_drive_copy(const char *path);

/* 1 if the file at path is an encrypted catalog (needs a passphrase). */
int bd_catalog_file_needs_passphrase(const char *path);

/* Write the catalog back to the .brodalf file atomically, as a new
 * revision of the catalog, when anything changed since the catalog was
 * opened or last saved; with nothing to save it does nothing (no new
 * revision, the file is not rewritten) and returns BD_OK. Every committed
 * change to the catalog's own tables counts, however it was made, and so
 * does bd_catalog_mark_changed; a catalog file that is not there is always
 * written. A save that fails leaves the changes unsaved for the next one. */
bd_status bd_catalog_save(bd_catalog *cat);
/* 1 if bd_catalog_save has something to write, 0 if not. */
int bd_catalog_unsaved(bd_catalog *cat);
/* For a change kept outside the catalog's tables: the next bd_catalog_save
 * writes the file even if nothing in the database changed. */
void bd_catalog_mark_changed(bd_catalog *cat);
/* How many times the catalog has been saved: its revision. The copy put on
 * a drive carries it in its name, and copies are never overwritten, so a
 * drive keeps every revision it was given. */
int64_t bd_catalog_revision(bd_catalog *cat);

/* Close without saving, remove the working copy (if any) and the lock. */
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
/* A recovery key is a second way to unlock, for when the passphrase is
 * lost: 30 characters in groups of five ("ABCDE-FGHJK-..."), printed on the
 * recovery sheet. Making one needs the catalog unlocked and replaces any
 * earlier one; BRODALF keeps only the master key wrapped by it, never the
 * key itself. Changing the passphrase keeps it working. */
#define BD_RECOVERY_KEY_SIZE 36
bd_status bd_catalog_make_recovery_key(bd_catalog *cat, char out[BD_RECOVERY_KEY_SIZE]);
int bd_catalog_has_recovery_key(bd_catalog *cat);
/* Case, dashes and spaces do not matter. BD_ERR_PASSPHRASE if wrong. */
bd_status bd_catalog_unlock_recovery(bd_catalog *cat, const char *recovery_key);
/* 1 if the file at path is an encrypted catalog that a recovery key opens. */
int bd_catalog_file_has_recovery_key(const char *path);
/* Like bd_catalog_open_with, with the recovery key instead. */
bd_status bd_catalog_open_with_recovery(const char *path, const char *recovery_key, bd_catalog **out);
/* Write the recovery sheet, an HTML page to print: what to do when the catalog file is lost,
 * the backup drives and what each holds, the backup roots, and the
 * recovery key when one is given (from bd_catalog_make_recovery_key). */
bd_status bd_write_recovery_sheet(bd_catalog *cat, const char *path, const char *recovery_key);
/* Forget the master key until the next unlock. */
void bd_catalog_lock_key(bd_catalog *cat);
/* Encrypt the .brodalf file from the next save on. Needs an unlocked
 * catalog to turn on. Turned on, the open catalog moves into memory at
 * once and its working copy is deleted (BD_ERR_DB if a job keeps the
 * database busy for more than a few seconds); turned off, it stays in
 * memory until it is closed. */
bd_status bd_catalog_set_file_encrypted(bd_catalog *cat, int on);
int bd_catalog_file_encrypted(bd_catalog *cat);

/* Progress for long jobs (scan, backup, check, restore), called at most
 * about ten times a second from the thread running the job, and once more
 * with done set when the job ends. bytes_done counts every byte read so far
 * (hashed, copied or checked), so it gives the data rate. Totals are 0 while
 * not known; a scan's files_total is what the last scan found. */
typedef struct {
    const char *phase;     /* "scan", "backup", "check", "verify", "restore" */
    const char *current;   /* the file being worked on; "" between files */
    int64_t files_done;
    int64_t files_total;
    int64_t bytes_done;
    int64_t bytes_total;
    /* The file being worked on, for a bar of its own; 0 total when there
     * is no single file (a scan hashing several at once). */
    int64_t file_bytes_done;
    int64_t file_bytes_total;
    int done;
    /* 1 on a call made right after a backup recorded a group of copies in
     * the catalog. Such a call comes from the thread running the job, so
     * the callback may read the catalog in it (bd_backup_sets, to show the
     * full copies growing). Every other call has 0 and may come from a
     * thread reading files for the job: it must not touch the catalog. */
    int recorded;
} bd_progress;
typedef void (*bd_progress_fn)(void *ctx, const bd_progress *p);
void bd_catalog_set_progress(bd_catalog *cat, bd_progress_fn fn, void *ctx);

/* Ask the running job (scan, backup, check, read-back, restore) to stop.
 * Safe to call from any thread, including from the progress callback. The
 * job stops between files, or between blocks of a big file, and returns
 * BD_ERR_CANCELLED. Everything it finished stays recorded: a scan keeps
 * every file it had read, in the root it was in too (nothing there is
 * marked deleted, and that root is not counted as scanned until a scan of
 * it finishes; one whose folder showed none of the files the catalog knows
 * is left as it was), a backup keeps every copy it had finished, and a copy it was writing is
 * removed, never put in place. Each job clears the request when it
 * starts. */
void bd_catalog_cancel(bd_catalog *cat);

/* ---- Backup roots and scanning ----------------------------------------
 *
 * A catalog backs up one or more backup roots (folders, called sources in
 * the code), everything inside them, kept as the folder structure they
 * have. Together they are the primary dataset. */

/* Add a backup root, or give the id of the one that is that folder already
 * (the same path, compared as the file system compares paths, or the same
 * folder reached another way: through a link, or by a mapped network
 * drive letter and its network path). A folder inside a backup root, or
 * holding one, is refused (BD_ERR_EXISTS, with a message naming that
 * root): its files would be backed up twice. Stopped roots
 * (bd_source_set_active 0) do not count. A folder given from the current
 * folder ("Documents", "../Photos") is kept as a full path. */
bd_status bd_source_add(bd_catalog *cat, const char *folder, int64_t *out_source_id);
/* The same; *is_new gets 1 when a new root was added, 0 when the folder
 * was a root already. */
bd_status bd_source_add_ex(bd_catalog *cat, const char *folder, int64_t *out_source_id, int *is_new);

/* The backup roots that are backed up inside folder (a folder bd_source_add
 * refuses because it holds them): fn gets each one's id and path, and
 * returns nonzero to stop. How many there are, or -1 on an error. */
typedef int (*bd_root_fn)(void *ctx, int64_t source_id, const char *path);
int bd_roots_inside(bd_catalog *cat, const char *folder, bd_root_fn fn, void *ctx);
/* Add folder as a backup root, and make every backup root inside it
 * (bd_roots_inside) part of it, in one step: their files and folders, with
 * their versions and copies, their stars, encrypted folders and own skip
 * lists, are the new root's, and those roots are no longer listed. In the
 * catalog only: nothing on a drive moves, and a copy keeps its place there,
 * so nothing is copied again. Otherwise as bd_source_add (a root inside it
 * only through a link or a mapped drive: refused as bd_source_add refuses
 * it). */
bd_status bd_source_add_holding(bd_catalog *cat, const char *folder, int64_t *out_source_id);

/* The active backup root that holds path (a file or folder), or NULL; free
 * it. A scan never backs up the open database or its side files, so a
 * database saved inside a root is not added to every backup again. */
char *bd_root_holding(bd_catalog *cat, const char *path);

/* How two folders lie, as bd_source_add compares backup roots (another
 * spelling, a link or a mapped drive on the way count as the same): 1 the
 * same folder, 2 a is inside b, 3 b is inside a, 0 neither. For the
 * folders chosen for a new database, before it exists. */
int bd_folders_overlap(const char *a, const char *b);

/* The top of the BRODALF backup drive (of any database) that path, a file
 * or folder there or to be made there, is on: the folder above it that
 * holds BRODALF/<id>/BRODALF.media; malloc'd, or NULL when it is on none.
 * A database kept there is lost with the drive. */
char *bd_backup_drive_holding(const char *path);

/* Point a backup root at where it is now, e.g. under another user name
 * or on another drive letter. The root keeps its name on the drives and
 * its files keep their versions and copies; the next scan finds them by
 * their path inside the root. new_path must be an existing folder that no
 * other backup root is, reached any way, and for a root that is backed up
 * not inside another or holding one (BD_ERR_EXISTS). It is kept as a full
 * path, as bd_source_add keeps it. */
bd_status bd_source_relocate(bd_catalog *cat, int64_t source_id, const char *new_path);

/* Look for a backup root by its files: the folder the catalog has for it
 * is only where it was last. Looked at: the same folder under this user's
 * home folder when it was under another user's, the same disk mounted for
 * this user, and on Windows the same folder on every other local drive
 * letter. A folder counts when at least half the files the catalog checks
 * (up to 64, the same ones each time) are in it at their path in the root
 * with their size; nothing about the computer is used. Gives the folder
 * holding the most of them (free it), how many of how many checked are
 * there, and how many folders counted (places; 0 and NULL when none).
 * Changes nothing. A scan points a root that is not there at the folder
 * without asking only when it is the one place, at least 15 of every 16
 * files checked are there, and the smallest few of them (up to 4) hold
 * the contents the catalog has for them; never a root that waits for its
 * disk (bd_source_wait_for_disk). Otherwise the caller offers the folders
 * (bd_source_find_all). */
char *bd_source_find(bd_catalog *cat, int64_t source_id, int64_t *found, int64_t *checked, int *places);

/* One folder that holds a missing backup root's files. */
typedef struct {
    const char *path;
    int64_t found;   /* of the files checked, how many are there */
    int64_t checked;
} bd_place_info;
typedef int (*bd_place_fn)(void *ctx, const bd_place_info *p);
/* Every folder bd_source_find counts, the one holding the most first; fn
 * returns nonzero to stop. How many were listed, or -1 out of memory. */
int bd_source_find_all(bd_catalog *cat, int64_t source_id, bd_place_fn fn, void *ctx);

/* Where a backup root was before a scan found its files somewhere else and
 * pointed it there (bd_scan_stats.roots_found), malloc'd; NULL when it was
 * not moved that way, or once the user chose where it is (bd_source_relocate,
 * bd_source_keep_place). *back (may be NULL) gets 1 when that folder is
 * there again and holds the root's files: the user should say which of
 * the two is the real one, because only one is backed up. */
char *bd_source_old_place(bd_catalog *cat, int64_t source_id, int *back);
/* The backup root stays where it is now: forget where it was before. */
bd_status bd_source_keep_place(bd_catalog *cat, int64_t source_id);

/* Stop backing up a root (active 0), or start again (1). A stopped root is
 * not scanned or backed up and does not count toward the copies you want;
 * its files, versions and copies stay in the catalog and still restore.
 * Starting a root again that is inside another root, or holds one, is
 * refused as bd_source_add refuses it (BD_ERR_EXISTS). */
bd_status bd_source_set_active(bd_catalog *cat, int64_t source_id, int active);

typedef struct {
    int64_t files_seen;
    int64_t dirs_seen;
    int64_t files_new;
    int64_t files_changed;   /* a new version was recorded (the checksum changed) */
    int64_t files_deleted;   /* in the catalog but no longer on disk (or newly left out) */
    int64_t files_moved;     /* moved or renamed: recognised by checksum, keeping their versions and copies */
    int64_t deleted_no_copy; /* of the files this scan found deleted, those no drive holds */
    int64_t bytes_hashed;
    int64_t skipped_links;
    int64_t errors;
    int64_t skipped;         /* files and folders left out by the skip lists */
    int64_t files_in_use;    /* another program has them open; see bd_list_in_use */
    int64_t files_online_only; /* cloud files not on this PC (OneDrive "online-only"): left alone, not downloaded */
    int64_t files_damaged;   /* bit-rot scan: contents changed but size and time did not; see bd_list_damaged */
    int64_t roots_missing;   /* backup roots not found (unplugged, moved): skipped, nothing marked deleted */
    int64_t roots_stopped;   /* backup roots no longer backed up: not scanned */
    /* Of roots_missing, those whose folder is there but is not the one the
     * catalog knows: a tenth or less of its files are in it as the catalog
     * has them (at their path, or moved inside it), and more are not there
     * at all than are there but changed. An empty folder made under the
     * same name, or another folder of files with the same path. Left as the
     * last scan saw them, nothing marked deleted. bd_source_info.files_gone
     * says how many files were not found. */
    int64_t roots_emptied;
    /* Of roots_missing, those waiting for their disk to be plugged in
     * (bd_source_wait_for_disk): the caller need not ask about them. */
    int64_t roots_waiting;
    /* Of files_online_only, those the catalog knew whose size or time
     * changed: edited elsewhere, so the newest version is only in the
     * cloud. Counted at risk until they are on this PC and scanned. */
    int64_t files_cloud_changed;
    /* Of skipped_links, those leading to a folder or file no backup root
     * holds: what is behind them is not backed up (bd_list_links). */
    int64_t links_not_followed;
    /* Backup roots whose folder was not there and were found by their
     * files somewhere else (bd_source_find), in one place only: pointed
     * there (bd_source_relocate) and scanned there. Not in roots_missing. */
    int64_t roots_found;
    /* Backup roots found that way earlier whose old folder is there again
     * holding their files (bd_source_old_place): the user should say which
     * is the real one. */
    int64_t roots_back;
} bd_scan_stats;

/* The skip lists: files and folders a scan leaves out. One list for the
 * catalog, and one more for each backup root; a file either one matches
 * is left out. One pattern per line, matched against names ignoring case,
 * with * and ? as wildcards. A pattern ending in / matches folders only
 * (and everything inside them). A pattern with a / elsewhere is matched
 * against the whole path inside the backup root, like "Photos/Exports/".
 * Blank lines and lines starting with # are ignored. Files already in the
 * catalog that are now left out are marked deleted, as if gone from their
 * folder, so they show among the deleted files for the user to keep or
 * forget. */
const char *bd_skip_list_default(void);
/* The catalog's list (the default until one is set). malloc'd. */
char *bd_skip_list_get(bd_catalog *cat);
/* NULL goes back to the default; "" skips nothing. */
bd_status bd_skip_list_set(bd_catalog *cat, const char *patterns);
/* One backup root's own list ("" until one is set). malloc'd. */
char *bd_source_skip_get(bd_catalog *cat, int64_t source_id);
/* NULL or "" clears it. */
bd_status bd_source_skip_set(bd_catalog *cat, int64_t source_id, const char *patterns);
/* Leave one file (or folder) out of its root's scans from now on: its path
 * is added to that root's own list, as a line starting with / (a path from
 * the top of the root, so files of the same name elsewhere are still
 * backed up). Nothing is deleted: the next scan shows it among the deleted
 * files, and the copies already made stay on the drives and still restore. */
bd_status bd_leave_out(bd_catalog *cat, int64_t node_id);
/* 1 if rel_path (inside a backup root, '/'-separated) is skipped. */
int bd_skip_match(const char *patterns, const char *rel_path, int is_dir);

typedef struct {
    /* Bit-rot scan: also read every file whose size and time are
     * unchanged and compare its checksum with the catalog's. A file whose
     * contents changed anyway is listed (bd_list_damaged), not recorded as
     * a new version: its copies on the drives are the good ones. Reads
     * everything, so it takes as long as the first scan did. */
    int bitrot;
} bd_scan_opts;

/* Scan every backup root. New or changed files are hashed (BLAKE3) and get
 * a new version; a file whose checksum did not change is not a new
 * version, whatever its size or time say. Unreadable entries are logged
 * and skipped. A root that cannot be found is skipped without marking
 * anything deleted, and listed (bd_source_info.missing) so the user can
 * say where it is now (bd_source_relocate) or restore it. Before that, a
 * root that is not there is looked for by its files (bd_source_find): when
 * one folder surely holds them, the root is pointed there and scanned there
 * (bd_scan_stats.roots_found). Files moved or renamed inside or between
 * roots are recognised by checksum and keep their versions and copies;
 * nothing on any drive is moved. A root whose folder is there but holds
 * other files than the catalog knows (bd_scan_stats.roots_emptied) is
 * treated like one that is not there, and the log says where its files
 * are when another folder holds them. */
bd_status bd_scan(bd_catalog *cat, bd_scan_stats *stats, bd_log_fn log, void *log_ctx);
bd_status bd_scan_ex(bd_catalog *cat, const bd_scan_opts *opts, bd_scan_stats *stats, bd_log_fn log, void *log_ctx);
/* The user says the files missing from an emptied root really were
 * deleted: the next scan of it marks them deleted as usual (they stay in
 * the catalog with their copies). Good for one scan; until this database
 * is closed. */
bd_status bd_source_accept_gone(bd_catalog *cat, int64_t source_id);
/* The backup root is on a disk that is usually unplugged, or on a network
 * share that is often out of reach (on: 1): while it cannot be found,
 * scans skip it quietly (bd_scan_stats.roots_waiting).
 * The first scan that finds it again ends the wait. Kept in the catalog. */
bd_status bd_source_wait_for_disk(bd_catalog *cat, int64_t source_id, int on);

/* The links (symbolic links, junctions) the last scan found in the backup
 * roots and did not follow, whose target no backup root holds; the old
 * hidden "My Documents" kind are left out. Adding the target as a backup
 * root backs up what is behind one. */
typedef struct {
    int64_t source_id;
    const char *source_name;
    const char *rel_path;
    const char *target; /* where it leads, as a full path */
    int is_dir;
} bd_link_info;
typedef int (*bd_link_fn)(void *ctx, const bd_link_info *info);
bd_status bd_list_links(bd_catalog *cat, bd_link_fn fn, void *ctx);
/* Where the link at path leads (a full path, malloc'd), or NULL if path is
 * not a link or leads nowhere. */
char *bd_path_link_target(const char *path);

/* Files a bit-rot scan found changed without their size or time changing.
 * They are kept in the catalog, so they are still listed after it is
 * closed and opened again, until the user accepts the change
 * (bd_damaged_accept) or a later scan reads the file and finds it right
 * again (restored from a drive) or records it as a new version (it was
 * changed again, with its time). A file gone from its folder is not
 * listed. */
typedef struct {
    int64_t node_id;
    int64_t source_id;
    const char *source_name;
    const char *rel_path;
    int64_t size;
    const char *hash_on_record;
    const char *hash_found;
    int64_t found_ms;        /* when a bit-rot scan first found it like this */
    int64_t version_id;      /* the version on record, whose copies are the good contents (bd_restore_version) */
} bd_damaged_info;
typedef int (*bd_damaged_fn)(void *ctx, const bd_damaged_info *info);
bd_status bd_list_damaged(bd_catalog *cat, bd_damaged_fn fn, void *ctx);
/* The file as it is now is right after all (it was changed on purpose):
 * record it as a new version, to be backed up like any change, and stop
 * listing it. BD_ERR_NOT_FOUND if it is not listed. To get the good
 * contents back instead, restore the file. */
bd_status bd_damaged_accept(bd_catalog *cat, int64_t node_id);

/* ---- Files in use ------------------------------------------------------ */

/* Files another program has open so that nobody else can read them (an
 * Outlook .pst while Outlook runs, some databases), found by the last scan
 * or backup. They are skipped and listed here; the catalog keeps what it
 * knew about them. Closing the program and scanning again picks them up. */
typedef int (*bd_in_use_fn)(void *ctx, const char *path, int64_t size);
bd_status bd_list_in_use(bd_catalog *cat, bd_in_use_fn fn, void *ctx);

/* Read path from staged instead, for scans and backups, until cleared: a
 * copy of a file in use taken from a shadow copy. path is as listed by
 * bd_list_in_use. Nothing calls this by itself: the app offers it after a
 * scan or backup that met files in use, and the user chooses. */
bd_status bd_substitute_add(bd_catalog *cat, const char *path, const char *staged);
void bd_substitutes_clear(bd_catalog *cat);

/* Windows: copy files in use out of a Volume Shadow Copy of their drive (a
 * snapshot Windows takes that no program holds open), into dest_dir as
 * 1.bin, 2.bin, ... in order. Needs administrator rights. Returns how many
 * were copied (ok[i] set for each), or -1 with a reason in err. */
int bd_shadow_copy_files(const char *const *paths, int n, const char *dest_dir, int *ok, char *err, size_t err_len);

/* The app runs an elevated copy of itself to take the shadow copy. What it
 * hands over is a list file (who asked, the backup roots, the files) whose
 * BLAKE3 hash goes on the helper's command line, so nothing can change it
 * after Windows asked for permission. user_sid is the asking user's SID
 * ("S-1-5-21-..."). Returns the list's text (malloc'd) and its hash, or
 * NULL if a string holds a line break. */
char *bd_shadow_list_format(const char *user_sid, const char *const *folders, int n_folders, const char *const *files,
                            int n_files, char hash_out[65]);

typedef struct {
    char *user_sid;
    char **folders;
    int n_folders;
    char **files;
    int n_files;
} bd_shadow_list;

/* 0, -1 not a list, -2 the text does not match expected_hash. */
int bd_shadow_list_parse(const char *text, size_t len, const char *expected_hash, bd_shadow_list *out);
void bd_shadow_list_free(bd_shadow_list *list);

/* 1 if path is a plain "X:\..." path strictly inside one of the folders,
 * compared ignoring ASCII case; "." and ".." parts, ":" streams, UNC and
 * device paths never are. */
int bd_shadow_path_allowed(const char *path, const char *const *folders, int n_folders);

/* The elevated helper (brodalf.exe --shadow-copy <out dir> <list> <hash>):
 * reads the list once and checks its hash, refuses every file outside the
 * backup roots it names, makes out_dir as a new folder that only
 * administrators can write to and the asking user can read and empty, and
 * copies the files there as 1.bin, 2.bin, ... (by their place in the list).
 * out_dir/result.txt gets one line per file ("ok", "failed" or "refused")
 * and "error: ..." if something went wrong. Returns how many were copied,
 * or -1 with the reason in err. */
int bd_shadow_helper(const char *out_dir, const char *list_path, const char *list_hash, char *err, size_t err_len);

/* In this process instead (the command-line tool's --shadow-copy, run as
 * administrator): copy every file in use that the last scan or backup met
 * (bd_list_in_use) out of a shadow copy into a new folder in the temp
 * folder, and read those copies in their place (bd_substitute_add).
 * Returns how many are ready (0: none was in use), or -1 with the reason in
 * the catalog error. *dir_out is the folder, for bd_shadow_unstage. */
int bd_shadow_stage(bd_catalog *cat, char **dir_out);
/* Stop reading those copies, and delete them and their folder. */
void bd_shadow_unstage(bd_catalog *cat, const char *dir);

/* ---- Storage (media) ------------------------------------------------- */

/* Prepare a drive or folder as storage for this catalog. Writes
 * <root>/BRODALF/<catalog-uuid>/BRODALF.media with a new media ID.
 * BD_MEDIA_ENCRYPTED makes every copy on it encrypted (the catalog must
 * have a passphrase and be unlocked); this cannot be changed later. */
#define BD_MEDIA_ENCRYPTED 1u
/* An extra place for starred files rather than one of the N copies (see
 * bd_place_add, which sets this). */
#define BD_MEDIA_SECONDARY 2u
/* With BD_MEDIA_ENCRYPTED only: hide file and folder names on the drive
 * too. Each name there becomes 24 letters and digits made from the real
 * one with the key; the real names are only in the catalog, whose copy on
 * the drive is encrypted. How many files and folders there are, and their
 * sizes, still show. Cannot be changed later. A catalog with such a drive
 * is saved as schema 9, which BRODALF 0.8.0 and older will not open. */
#define BD_MEDIA_HIDE_NAMES 4u
bd_status bd_media_init(bd_catalog *cat, const char *root, const char *label, unsigned flags, int64_t *out_media_id);

/* Tell BRODALF a known drive is connected at root. Identifies it by its
 * BRODALF.media file (not by drive letter) and records the mount and free
 * space, and reads the disk's health data. It checks no copies: that is
 * bd_media_check, run when the user asks. Files with a good copy on
 * connected storage stop being greyed out. */
typedef struct {
    int64_t copies;
    int64_t ok;
    int64_t missing;
    int64_t bad;
    int64_t rehashed;
    int64_t skipped;   /* encrypted copies that need the passphrase to check */
    /* bd_media_check only. The newest copy of the catalog on the drive:
     * 1 it reads, -1 it is damaged (the next backup puts a new revision
     * there), 0 not read (none there, or encrypted and locked). */
    int catalog_copy;
    int64_t catalog_revision;
    /* Half-written files BRODALF left on the drive (names ending in
     * .brodalf-tmp, from a copy that stopped, as at a power cut), older
     * than an hour: bd_media_remove_leftovers removes them. */
    int64_t leftovers, leftover_bytes;
} bd_check_stats;

bd_status bd_media_connect(bd_catalog *cat, const char *root, int64_t *out_media_id, bd_log_fn log, void *log_ctx);
void bd_media_disconnect(bd_catalog *cat, int64_t media_id);

/* Where a drive is kept, or what is written on it ("Box A", "top shelf").
 * Optional; "" or NULL clears it. */
bd_status bd_media_set_location(bd_catalog *cat, int64_t media_id, const char *location);

/* Rename a drive. The new name is also written to its BRODALF.media file
 * the next time the drive is plugged in. */
bd_status bd_media_rename(bd_catalog *cat, int64_t media_id, const char *label);

/* Check every copy on a connected drive: that it is there, with the right
 * size, and that its checksum is right (every copy is read back). A copy
 * found missing or damaged stops counting until a backup writes it again,
 * or a later check finds it there again as it was. An encrypted drive
 * needs the passphrase. A drive that stops answering or is unplugged
 * during the check is not taken for missing copies: the check stops with
 * BD_ERR_DRIVE_GONE, keeps the copies it found good, and marks nothing
 * missing or damaged. */
bd_status bd_media_check(bd_catalog *cat, int64_t media_id, bd_check_stats *stats, bd_log_fn log, void *log_ctx);
/* Remove the half-written files bd_media_check counted in leftovers: only
 * BRODALF's own .brodalf-tmp files in its folder on the drive, never a
 * copy. *removed is how many went. */
bd_status bd_media_remove_leftovers(bd_catalog *cat, int64_t media_id, int64_t *removed);

/* Read back and hash every copy on a connected drive that has not been
 * read back in max_age_days (0: every copy). Copies never read back since
 * they were written always count, and so do copies marked missing, in
 * case they are there again. As bd_media_check for a drive that goes. */
bd_status bd_media_verify(bd_catalog *cat, int64_t media_id, int max_age_days,
                          bd_check_stats *stats, bd_log_fn log, void *log_ctx);

/* Read back only the copies on a connected drive never read back since
 * they were written (bd_copy_info.read_back 0), such as everything the
 * last backup wrote. As bd_media_check for a drive that goes. */
bd_status bd_media_verify_unread(bd_catalog *cat, int64_t media_id, bd_check_stats *stats, bd_log_fn log, void *log_ctx);

/* ---- Compressing a backup drive ----------------------------------------- */

/* Windows' own file compression (NTFS) for BRODALF's folder on a backup
 * drive: an option the person turns on or off, never something BRODALF
 * does by itself. Copies stay ordinary files that any Windows PC reads;
 * Windows makes them smaller on the drive and whole again when read. */
typedef struct {
    int64_t files;         /* files looked at */
    int64_t changed;       /* files compressed (or made whole again) */
    int64_t failed;        /* files that could not be changed (in use, read-only drive) */
    int64_t bytes;         /* the size of every file looked at */
    int64_t bytes_on_disk; /* the room they take on the drive afterwards */
} bd_compress_stats;

/* For a plugged-in drive. *on: BRODALF's folder there is compressed, so
 * new copies are too. *why: when the drive cannot be compressed, a plain
 * sentence saying why (NULL when it can); not to be freed. Not plugged in:
 * BD_ERR_NOT_FOUND. */
bd_status bd_media_compression(bd_catalog *cat, int64_t media_id, int *can, int *on, const char **why);

/* Turn compression on or off for BRODALF's folder on a plugged-in drive
 * and every file already there. Takes about as long as reading and
 * writing every copy on the drive; can be stopped (bd_catalog_cancel), and
 * what was done until then stays. Turning it off needs room for the files
 * to grow back to their full size. */
bd_status bd_media_set_compression(bd_catalog *cat, int64_t media_id, int on, bd_compress_stats *stats, bd_log_fn log,
                                   void *log_ctx);

/* ---- Backup and restore ---------------------------------------------- */

typedef struct {
    int64_t files_considered;
    int64_t files_copied;
    int64_t files_already_there;
    int64_t files_failed;
    int64_t bytes_copied;
    int64_t files_no_room;    /* did not fit: they wait for another drive */
    int64_t bytes_no_room;
    int64_t copies_alongside; /* written under .versions because the usual place held an older version or a file BRODALF did not write */
    int64_t files_rehashed;   /* changed since the scan: read again, recorded as a new version and copied */
    int64_t files_in_use;     /* another program had them open (counted in files_failed too) */
    int64_t files_online_only; /* cloud files not on this PC: not copied, so as not to download them */
    /* Of files_no_room, those too big for the drive's file system: never
     * read or written, planned out up front. too_big_why says why, in a
     * sentence the window can show (BD_WHY_FAT32); NULL when none was. */
    int64_t files_too_big;
    int64_t bytes_too_big;
    const char *too_big_why;
    /* Of files_no_room (and not too big for the file system), those bigger
     * than every backup drive you have (bd_biggest_drive): no drive can
     * take them, so they wait for a bigger drive, or for you to leave them
     * out. */
    int64_t files_bigger_than_drives;
    int64_t bytes_bigger_than_drives;
    /* Backup roots left out of this backup (bd_list_roots_left_out): their
     * folder could not be found, or the last scan held them back. The same
     * number for every drive of a bd_backup_many. */
    int64_t roots_left_out;
    /* Files in folders set to be encrypted that were not copied because the
     * passphrase was not entered: they wait for it. Everything else was
     * backed up. */
    int64_t files_need_passphrase;
} bd_backup_stats;

/* The size of the biggest backup drive that counts (not lost or broken,
 * not an extra place), or -1 when there is none or one of them has never
 * said how big it is. */
int64_t bd_biggest_drive(bd_catalog *cat);

/* Why files of 4 GiB or more do not fit on a FAT32 drive (the file system
 * holds files up to 4 GiB less one byte; Windows' "FAT" for FAT12 and FAT16
 * too). Sealed copies count with their few extra bytes. */
#define BD_WHY_FAT32 "this drive is FAT32 and can't hold files of 4 GB or more; reformat it as exFAT or NTFS"

/* Copy the current version of every file that has no good copy on this
 * drive. The drive must be connected and not marked lost or broken.
 * source_id 0 means every backup root (the app always backs up all of
 * them: a catalog is one set of files to keep N copies of).
 *
 * Nothing on the drive is overwritten, moved or deleted. A file goes to
 * <root name>/<path>; when something else is already there (an older
 * version, a damaged copy, a file BRODALF did not write) the copy goes
 * alongside it under .versions instead. A file BRODALF did not write that
 * is exactly this version is recorded as its copy. A file that changed
 * since the last scan is read again and backed up as it is now, as a new
 * version. Files that would eat into the reserve (the reserve_mb option,
 * kept for the catalog's copies) wait for another drive and are counted in
 * files_no_room; smaller files after them are still copied. So are files
 * too big for the drive's file system (FAT32: 4 GiB or more), which are
 * not read at all and are counted in files_too_big as well. At the end the
 * catalog is saved and its newest revision added to the drive (see
 * bd_catalog_copy_to_media). */
bd_status bd_backup(bd_catalog *cat, int64_t media_id, int64_t source_id,
                    bd_backup_stats *stats, bd_log_fn log, void *log_ctx);

/* Back up to several connected drives (at most 16) in one pass: each file
 * is read from its folder once and written to every drive that needs it at
 * the same time. Everything else is as bd_backup, drive by drive: stats
 * gets one entry per drive, in the order of media_ids. A drive that fails
 * to write a file does not stop the others. This reads the backup roots
 * once instead of once per drive, but writes to the drives at once, which
 * loads the PC (its USB bus, memory and processor) more while it runs, and
 * runs at the pace of the slowest drive. The app does this only when the
 * "parallel_copies" option is on; otherwise it backs up one drive after
 * another. */
bd_status bd_backup_many(bd_catalog *cat, const int64_t *media_ids, int n, int64_t source_id,
                         bd_backup_stats *stats, bd_log_fn log, void *log_ctx);

/* What a backup to this drive would do, without doing any of it: shown
 * all the time beside the Backup button. Reads only the catalog, and the
 * drive's free space when it is plugged in. */
typedef struct {
    int64_t files_to_copy;     /* no good copy of the file as it is now on this drive */
    int64_t bytes_to_copy;
    int64_t files_new;         /* of those, none of its versions is on the drive */
    int64_t files_changed;     /* of those, an older version is: the new one goes alongside it */
    int64_t free_bytes;        /* now when plugged in, else as last seen; -1 unknown */
    int free_is_current;       /* read just now */
    int64_t reserve_bytes;     /* kept free on every drive for the catalog's copies */
    int64_t bytes_short;       /* about how much would not fit, too big files included; 0: it all fits (or the space is unknown and none is too big) */
    int64_t last_scan_ms;      /* the oldest of the roots' last scans: a backup copies what that found */
    /* Files whose good copy is on this drive, and of those how many have
     * changed contents (checksum) since or are gone from their folder.
     * A large share at once is what ransomware encrypting the folders
     * looks like, in place or to new names with the originals deleted:
     * show the list (bd_list_changed_on_media) before backing that up. A
     * deleted file counts until the user acknowledges it in the deleted
     * list (bd_deleted_ack); a moved file does not count. */
    int64_t files_held;
    int64_t held_changed;
    int64_t held_deleted;      /* of held_changed, those gone from their folder (the rest changed contents) */
    int mass_change;           /* held_changed is a quarter or more of files_held (at least 50 files) */
    /* The drive holds fewer than 50 files (a new one, say), so files_held
     * and the rest are worked out over every drive: files with a good copy
     * on any drive. bd_list_changed_on_media with media_id 0 lists them. */
    int held_everywhere;
    /* Of files_to_copy, those too big for the drive's file system (FAT32:
     * 4 GiB or more): they will not fit and are in bytes_short. The file
     * system is read now when the drive is plugged in, else as last seen.
     * too_big_why says why, in a sentence the window can show
     * (BD_WHY_FAT32); NULL when none is too big. */
    int64_t files_too_big;
    int64_t bytes_too_big;
    const char *too_big_why;
    /* Backup roots this backup leaves out (bd_list_roots_left_out); their
     * files are not in files_to_copy, and last_scan_ms leaves them out. */
    int64_t roots_left_out;
    /* Of files_to_copy, those that need the passphrase and it is not
     * entered: every file for an encrypted drive, else those in folders set
     * to be encrypted. A backup to a plain drive copies the others and
     * leaves these waiting (files_need_passphrase); one to an encrypted
     * drive asks for the passphrase first. */
    int64_t files_need_passphrase;
} bd_backup_preview_info;

bd_status bd_backup_preview(bd_catalog *cat, int64_t media_id, int64_t source_id, bd_backup_preview_info *out);

/* Backup roots a backup leaves out, and why. A backup copies only what a
 * scan found in a root's own folder: a root whose folder cannot be found
 * now, or that the last scan held back because most of the files it knows
 * there were not (another folder with the same name, say), is left out of
 * the backup, its preview and the Next backup list until a scan finds it
 * again. Nothing of it is marked deleted and nothing is read from the
 * folder at its path. A hold is remembered in the catalog (it outlasts the
 * session); a folder not found is looked for again each time. */
typedef enum {
    BD_ROOT_NOT_FOUND = 1, /* its folder cannot be found */
    BD_ROOT_WAITING,       /* set to wait for its disk (bd_source_wait_for_disk), which is not plugged in */
    BD_ROOT_HELD,          /* the last scan found most of its files gone from the folder there: left as it was */
    BD_ROOT_NOT_SCANNED    /* it could not be found at the last scan and is there now: scan it first */
} bd_root_out_why;
typedef struct {
    int64_t source_id;
    const char *name;
    const char *path;
    bd_root_out_why why;
    int64_t files_gone; /* BD_ROOT_HELD: how many of its files the scan did not find there */
} bd_root_left_out;
typedef int (*bd_root_left_out_fn)(void *ctx, const bd_root_left_out *root);
/* source_id 0: every root still backed up. fn may be NULL; *count (may be
 * NULL) gets how many are left out. fn returning non-zero stops the list. */
bd_status bd_list_roots_left_out(bd_catalog *cat, int64_t source_id, bd_root_left_out_fn fn, void *ctx, int64_t *count);
/* Why, in a few English words ("its folder cannot be found"). */
const char *bd_root_out_text(bd_root_out_why why);

/* The files a backup would copy, one by one, for the Next backup tab.
 * media_id > 0: what the next backup to that drive copies. media_id 0:
 * every live file that does not yet have the copies you want. For each:
 * how many good copies of the file as it is now count toward the target
 * (set k has the file when copies >= k). At most limit rows (0: 5000), by
 * root and path; *total (may be NULL) gets how many there are in all. */
typedef struct {
    int64_t node_id;
    int64_t source_id;
    const char *source_name;
    const char *rel_path;
    int64_t size;
    int64_t mtime_ns;
    int copies;              /* good copies of the file as it is now that count */
    int places;              /* different places they are in */
    int backed_up;           /* copies and places meet the target */
    int is_new;              /* no version of it is on that drive yet (media_id > 0) */
    int starred;
    int encrypt;             /* in a folder set to be encrypted */
    int too_big;             /* media_id > 0: too big for that drive's file system, so it will not fit (BD_WHY_FAT32) */
} bd_pending_info;
typedef int (*bd_pending_fn)(void *ctx, const bd_pending_info *info);
bd_status bd_list_pending(bd_catalog *cat, int64_t media_id, int limit, bd_pending_fn fn, void *ctx, int64_t *total);

/* The files counted in held_changed: the drive holds an older version
 * than the file's contents now, or the file is gone from its folder and
 * not yet acknowledged (deleted set; version_now and size_now are then
 * its last version, and changed_ms when that was first seen). */
typedef struct {
    int64_t node_id;
    const char *source_name;
    const char *rel_path;
    int64_t version_on_drive;
    int64_t version_now;
    int64_t size_on_drive;
    int64_t size_now;
    int64_t changed_ms;        /* when the scan first saw the new contents */
    int deleted;               /* gone from its folder (see above) */
    int64_t version_id_on_drive; /* the version the drive holds, to restore it (bd_restore_version) */
} bd_changed_info;
typedef int (*bd_changed_fn)(void *ctx, const bd_changed_info *info);
bd_status bd_list_changed_on_media(bd_catalog *cat, int64_t media_id, bd_changed_fn fn, void *ctx); /* media_id 0: every drive */

/* Save the catalog and put a copy of it on the drive as
 * BRODALF/<catalog-uuid>/catalog-r<revision>.brodalf, unless that revision
 * is already there. Older revisions on the drive are never overwritten.
 * The copy is encrypted when the catalog file or the drive is. Done after
 * every backup and when a drive is set up. */
bd_status bd_catalog_copy_to_media(bd_catalog *cat, int64_t media_id);

/* A drive that is lost or broken (why: "lost" or "broken"). Its copies no
 * longer count toward the copies you want, so the progress toward N full
 * backups drops and the files it held are backed up again to other drives.
 * Nothing is backed up to it. Its records stay, so it can be found again. */
bd_status bd_media_mark_lost(bd_catalog *cat, int64_t media_id, const char *why);
/* It turned up again (or was marked by mistake): its copies count again. */
bd_status bd_media_found(bd_catalog *cat, int64_t media_id);

/* A disk at root that is not set up for this catalog, but whose model,
 * serial number and size match a known drive that is not plugged in now
 * and not marked lost or broken: most likely that drive, wiped or
 * reformatted. *media_id is that drive. BD_ERR_NOT_FOUND if none matches
 * or the disk says nothing about itself, BD_ERR_EXISTS if it is set up for
 * this catalog. Nothing is changed. */
bd_status bd_media_match_disk(bd_catalog *cat, const char *root, int64_t *media_id);
/* The user said the disk at root is that drive, erased. Every copy
 * recorded on it is marked missing, so the files are backed up again and
 * those copies stop counting; its ID file is written back on the disk (a
 * new file: BD_ERR_EXISTS if one is there), so it keeps its name and
 * place; and it is recorded as plugged in at root. */
bd_status bd_media_erased(bd_catalog *cat, int64_t media_id, const char *root);

typedef struct {
    int64_t files_restored;
    int64_t files_offline;    /* copies exist but no drive holding one is connected */
    int64_t files_no_copy;
    int64_t files_failed;
    int64_t files_need_passphrase; /* only on an encrypted drive, and the catalog is locked */
    int64_t bytes_restored;
    int64_t files_already_there;   /* the destination already holds this exact version */
    int64_t files_conflict;   /* another file was already at the destination: left alone, see bd_list_restore_conflicts */
    int64_t files_older_version; /* restored as an older version: the newest was never backed up (or has no good
                                    copy left), so the latest backed-up state came back; the log names each one */
} bd_restore_stats;

/* Restore the latest backed-up state of every file in a backup root (0:
 * every root; optionally only under rel_prefix) into dest_root, from
 * connected drives: its current version, or when that was never backed up
 * (it changed after the last backup, say by ransomware), the newest version
 * that was (counted in files_older_version, and logged). With one root (chosen, or the catalog's only one) its files go
 * straight into dest_root as dest_root/<path>; with several, each root's
 * into dest_root/<root name>/<path>. Folder structure is kept as it is.
 * Every restored file is checked against its checksum and gets its
 * original modified (and, on Windows, created) time. Nothing is ever
 * overwritten: a file already at its destination with the right contents
 * is left as it is, so the same restore can be run again as each drive
 * holding the rest arrives; any other file there is left alone and listed
 * (bd_list_restore_conflicts). */
bd_status bd_restore(bd_catalog *cat, int64_t source_id, const char *rel_prefix, const char *dest_root,
                     bd_restore_stats *stats, bd_log_fn log, void *log_ctx);

typedef struct {
    /* Only files deleted from their folder, each as it was when last seen.
     * See bd_list_deleted. */
    int deleted_only;
} bd_restore_opts;

bd_status bd_restore_ex(bd_catalog *cat, int64_t source_id, const char *rel_prefix, const char *dest_root,
                        const bd_restore_opts *opts, bd_restore_stats *stats, bd_log_fn log, void *log_ctx);

/* Restore one version of a file (from the file's details) into
 * dest_folder, under the file's own name. Never overwrites, as above. */
bd_status bd_restore_version(bd_catalog *cat, int64_t version_id, const char *dest_folder, bd_restore_stats *stats,
                             bd_log_fn log, void *log_ctx);

/* The files the last restore left alone because another file was there,
 * to compare: what the backup has against what is there. */
typedef struct {
    const char *source_name;
    const char *rel_path;
    const char *dest;
    int64_t want_size;
    int64_t want_mtime_ns;
    int64_t have_size;      /* -1: a folder is there */
    int64_t have_mtime_ns;
    int64_t version_id;     /* the version it was going to put there, to restore it elsewhere (bd_restore_version) */
} bd_conflict_info;
typedef int (*bd_conflict_fn)(void *ctx, const bd_conflict_info *info);
bd_status bd_list_restore_conflicts(bd_catalog *cat, bd_conflict_fn fn, void *ctx);

/* Which drives a restore needs, in the order to plug them in: drives that
 * are already plugged in first, then each drive that holds the most of
 * what is still missing. Drives marked lost or broken are left out. Files
 * already at dest_root (when given) are not counted. files_no_copy is set
 * to the files no drive holds. */
typedef struct {
    int64_t media_id;
    const char *label;
    const char *kind;
    const char *location;  /* "" if not set */
    int connected;
    int64_t files;         /* files this drive supplies in the plan */
    int64_t bytes;
} bd_restore_step;

typedef int (*bd_restore_step_fn)(void *ctx, const bd_restore_step *step);

bd_status bd_restore_plan(bd_catalog *cat, int64_t source_id, const char *rel_prefix, const char *dest_root,
                          bd_restore_step_fn fn, void *ctx, int64_t *files_total, int64_t *files_no_copy);
bd_status bd_restore_plan_ex(bd_catalog *cat, int64_t source_id, const char *rel_prefix, const char *dest_root,
                             const bd_restore_opts *opts, bd_restore_step_fn fn, void *ctx, int64_t *files_total,
                             int64_t *files_no_copy);

/* Whether a restore fits where it goes, asked before it starts: what the
 * files it would restore take (bytes_needed: those with a copy on a drive,
 * not already at dest_root, as bd_restore_plan counts them; version_id > 0
 * for one version, as bd_restore_version) and the free space at dest_root,
 * or at the nearest folder above it that is there (bytes_free; -1 if it
 * cannot be read). A restore that does not fit stops with errors once the
 * disk is full; nothing there is overwritten either way. */
bd_status bd_restore_room(bd_catalog *cat, int64_t source_id, const char *rel_prefix, const char *dest_root,
                          const bd_restore_opts *opts, int64_t version_id, int64_t *bytes_needed, int64_t *bytes_free);

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
    int64_t bytes_total;     /* size of the current version of every live file */
    int active;              /* 0: no longer backed up (bd_source_set_active) */
    int missing;             /* the last scan could not find it: ask where it is now, or offer to restore it */
    int64_t last_scan_ms;
    /* Where its files' copies are: the drives and extra places holding a
     * good copy of the current version of any live file in it, names
     * sorted, ", " between them ("" if none; a drive marked lost or broken
     * is named only while it is plugged in), and those of them plugged in
     * now. */
    const char *copies_on;
    const char *copies_on_plugged;
    int64_t files_backed_up; /* live files with a good copy of the current version on a drive that counts */
    /* missing, but the folder is there: so many of its files were not
     * (bd_scan_stats.roots_emptied). 0 when the folder could not be found. */
    int64_t files_gone;
    /* The user said it is on a disk that is usually unplugged: while it
     * cannot be found, scans skip it without asking (bd_source_wait_for_disk). */
    int wait_for_disk;
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
    int64_t mtime_ns;        /* files: the current version's time; folders: as last scanned; 0 if not known */
    /* Where its copies are, so a database opened on a PC without the files
     * still says where each one is: the drives and extra places holding a
     * good copy of the current version (folders: of any file inside, or of
     * any file at all in a deleted folder), names sorted, ", " between
     * them, "" if none. A drive marked lost or broken is named only while
     * it is plugged in. */
    const char *copies_on;
    const char *copies_on_plugged; /* those of them plugged in now, the same way */
    const char *older_on;    /* files with no good copy of the current version: where an older one is; "" if none */
    int64_t files_backed_up; /* folders: files inside with a good copy of the current version on a drive that counts */
    int64_t subfolders;      /* folders: how many folders are directly in it, deleted ones too */
    /* Files the last scan found online-only (a cloud file not on this PC):
     * 1 never read, 2 changed in the cloud since its last backup, so the
     * newest version is not backed up (bd_risk_info.online_only). */
    int online_only;
} bd_node_info;

typedef struct {
    int64_t version_id;      /* for bd_restore_version */
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
    int encrypted;           /* the copy is encrypted: its drive is, or its folder is set to be */
    const char *media_location; /* where that drive is kept; "" if not set */
    int media_lost;          /* that drive was marked lost or broken: the copy no longer counts */
    int read_back;           /* the copy has been read back from the drive and its checksum found right
                                since it was written; 0: not yet (bd_media_verify_unread reads those) */
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
    const char *kind;        /* "drive"; a catalog from 0.3.0 may still list an
                                "onedrive" or "dropbox" account, which this
                                version cannot connect to */
    const char *last_root;   /* where it was last seen, e.g. "E:\" */
    int connected;
    int64_t total_bytes;     /* 0 if unknown */
    int64_t free_bytes;      /* as last read: when plugged in, and before and after each backup */
    int64_t space_ms;        /* when the space was last read; 0 if never */
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
    /* The oldest good copy on it never read back since it was written, by
     * when it was written; 0 if every good copy was read back. */
    int64_t oldest_unread_ms;
    int check_due;           /* older than the check_days option, or it holds a good copy never
                                read back that was written 7 days ago (check_days if less) */
    int64_t lost_ms;         /* when it was marked lost or broken; 0 if not (bd_media_mark_lost) */
    const char *lost_why;    /* "lost" or "broken"; "" if not */
    int64_t bytes_held;      /* size of the good copies on it */
    int64_t reserve_bytes;   /* kept free on it for the catalog's copies (reserve_mb) */
    int secondary;           /* an extra place for starred files (bd_place_add) */
    int64_t limit_bytes;     /* an extra place: most it takes of starred files; 0 no limit */
    int hide_names;          /* file and folder names on it are hidden (BD_MEDIA_HIDE_NAMES) */
    /* Its copies do not count toward the copies you want: 1 it is on the
     * same physical disk as a backup root (a partition of the PC's own
     * disk), so one disk failure would lose both; 2 its health data says
     * the disk is failing. 0 they count. As last read: a drive's disk when
     * it was last plugged in, a root's disk at the first scan since the
     * catalog was opened. */
    int uncounted;
    /* Another drive on the same physical disk (two partitions of one disk):
     * its label, "" if none. Copies on both count as one. */
    const char *same_disk_as;
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

/* Files deleted from their folder since BRODALF last saw them, backed up or
 * not, most recently seen first. Nothing leaves the catalog by itself: the
 * user either acknowledges them (they stay listed, with the drives still
 * holding them) or asks to forget them. Restore them with bd_restore_ex and
 * deleted_only set. source_id 0 means every root; new_only lists only the
 * ones not acknowledged yet. */
typedef struct {
    int64_t node_id;
    int64_t source_id;
    const char *source_name;
    const char *rel_path;
    int64_t size;
    int64_t last_seen_ms;    /* the last scan that still found it */
    const char *drives;      /* names of the drives holding a good copy, ", " between them; "" if none.
                                Drives marked lost or broken are left out: nothing comes back from them */
    int connected;           /* one of them is plugged in */
    int backed_up;           /* its last version has a good copy on a drive that counts */
    int acknowledged;        /* the user has seen it (bd_deleted_ack) */
} bd_deleted_info;
typedef int (*bd_deleted_fn)(void *ctx, const bd_deleted_info *info);
/* fn may be NULL to only count; *count may be NULL. */
bd_status bd_list_deleted(bd_catalog *cat, int64_t source_id, int new_only, bd_deleted_fn fn, void *ctx, int64_t *count);
/* Mark a deleted file as seen; node_id 0 marks every deleted file. They stay
 * in the catalog. A file that comes back stops being deleted. */
bd_status bd_deleted_ack(bd_catalog *cat, int64_t node_id);
/* Forget a deleted file (or a deleted folder and everything in it): the
 * catalog stops tracking it and its copies. The copies stay on the drives;
 * BRODALF never deletes anything there. node_id 0 forgets every deleted
 * file. BD_ERR_INVALID if the file is not deleted. *forgotten may be NULL. */
bd_status bd_deleted_forget(bd_catalog *cat, int64_t node_id, int64_t *forgotten);

/* Every version of a file and every copy of each version. */
bd_status bd_list_copies(bd_catalog *cat, int64_t node_id, bd_copy_fn fn, void *ctx);

/* ---- Starred files and extra places ------------------------------------ */

/* Star a file or folder (rel_path "" stars a whole backup root). Starred
 * files go to the drives like every file, and also to the extra places
 * (bd_place_add): a Dropbox or OneDrive folder on this PC, a network
 * share, any folder. A starred folder covers everything in it, now and
 * later. BRODALF talks to no cloud service: the folder's own app syncs it. */
bd_status bd_star_set(bd_catalog *cat, int64_t source_id, const char *rel_path, int on);
/* 1 if rel_path is starred itself, 2 if a folder above it is, 0 if not. */
int bd_starred(bd_catalog *cat, int64_t source_id, const char *rel_path);

/* A starred file or folder, or one set to be encrypted. */
typedef struct {
    int64_t source_id;
    const char *source_name;
    const char *rel_path;    /* "" the whole backup root */
    int is_dir;
    int found;               /* still in the catalog (a renamed folder is not) */
    int64_t files;           /* live files it covers */
    int64_t bytes;
    int64_t added_ms;
} bd_mark_info;
typedef int (*bd_mark_fn)(void *ctx, const bd_mark_info *info);
bd_status bd_list_stars(bd_catalog *cat, bd_mark_fn fn, void *ctx);
/* Live starred files in all. */
bd_status bd_starred_totals(bd_catalog *cat, int64_t *files, int64_t *bytes);

/* Add a folder as an extra place for starred files. Like a drive it gets
 * BRODALF/<catalog id>/ with plain copies in their folders (and the
 * catalog's own copy), but its copies never count toward the copies you
 * want: it is a second line, for the files that matter most. label must
 * say what and where it is, in at least two words and 10 characters
 * ("Dropbox folder on the laptop"). limit_bytes > 0: copy starred files
 * there only up to that much in all, so a small free account does not
 * fill up; 0: no limit. flags may be BD_MEDIA_ENCRYPTED to encrypt every
 * copy there (a passphrase must be set and entered). */
bd_status bd_place_add(bd_catalog *cat, const char *folder, const char *label, int64_t limit_bytes, unsigned flags,
                       int64_t *out_media_id);
bd_status bd_place_set_limit(bd_catalog *cat, int64_t media_id, int64_t limit_bytes);

/* How an extra place stands for the starred files, from the catalog. */
typedef struct {
    int64_t starred_files, starred_bytes;   /* every live starred file */
    int64_t have_files, have_bytes;         /* of those, the file as it is now is there */
    int64_t used_bytes;                     /* every good copy there, older versions included */
    int64_t limit_bytes;                    /* 0: no limit */
    int64_t waiting_files, waiting_bytes;   /* the next backup to it would copy these */
    int64_t over_limit_files;               /* of those, these would not fit under the limit */
    int64_t over_limit_bytes;
} bd_place_status;
bd_status bd_place_status_get(bd_catalog *cat, int64_t media_id, bd_place_status *out);

/* ---- Encrypting chosen folders ------------------------------------------ */

/* Set a file or folder (rel_path "" a whole backup root) to be encrypted
 * on every drive and extra place it is copied to, also the ones that are
 * not encrypted drives. Needs a passphrase (bd_catalog_set_passphrase);
 * backups then need it entered. Names stay readable: a copy is
 * <name>.bdenc. Copies written before stay as they are, since nothing on
 * a drive is overwritten or deleted; new copies and new versions are
 * encrypted. Turning it off makes the next new copies plain. */
bd_status bd_encrypt_path_set(bd_catalog *cat, int64_t source_id, const char *rel_path, int on);
/* 1 set itself, 2 a folder above it is, 0 not. */
int bd_encrypt_marked(bd_catalog *cat, int64_t source_id, const char *rel_path);
bd_status bd_list_encrypt_paths(bd_catalog *cat, bd_mark_fn fn, void *ctx);

typedef struct {
    int64_t files, bytes;        /* live files set to be encrypted */
    int64_t sealed_copies;       /* good copies of them as they are now that are encrypted */
    int64_t plain_copies;        /* ... that are plain: written before they were set */
    int64_t files_waiting;       /* with no encrypted copy anywhere yet */
    int64_t drives_encrypted;    /* drives and places set up as encrypted, in use */
    int64_t bytes_per_second;    /* how fast this PC encrypts (bd_crypto_speed) */
} bd_encrypt_stats;
bd_status bd_encrypt_stats_get(bd_catalog *cat, bd_encrypt_stats *out);

/* How many bytes a second this PC encrypts, measured once (about a
 * quarter of a second) and then remembered. Encrypting runs beside the
 * writes to the drive, so it costs at most this; reading back to check
 * and restoring decrypt at about the same speed. 0 if unknown. */
int64_t bd_crypto_speed(void);
/* What encrypting bytes worth of files means for time and space on this
 * PC, as a few sentences for the app to show beside the choice. */
void bd_encrypt_cost_text(int64_t bytes, char *out, size_t cap);

/* ---- Drive status ---------------------------------------------------- */

/* Where a drive stands, from the catalog alone (the drive need not be
 * plugged in). Sizes are of the file as it is now. */
typedef struct {
    int64_t files_current;      /* the file as it is now is on this drive */
    int64_t bytes_current;
    int64_t files_behind;       /* changed since: only an older version is on it */
    int64_t bytes_behind;
    int64_t files_not_here;     /* no version of the file is on it */
    int64_t bytes_not_here;
    int64_t deleted_kept;       /* files deleted from their folder whose last version is on it */
    int64_t old_versions;       /* copies on it of versions that are no longer the file as it is now */
    int64_t old_bytes;
    int64_t copies_bad;         /* missing or damaged when last checked */
    int64_t copies_never_read;  /* good copies never read back since they were written */
    int64_t last_backup_ms;     /* when the last backup to it started; 0: never */
    int64_t last_backup_end_ms; /* 0: it never finished (BRODALF closed during it) */
    char last_backup_status[16];/* "done", "partial", "stopped", "failed", "running"; "" never */
    int64_t last_backup_files;  /* files it copied */
    int64_t last_backup_failed;
    int64_t last_check_ms;      /* when the last check of its copies finished; 0: never */
    char last_check_kind[16];   /* "check" or "read back" */
    char last_check_status[16];
} bd_media_status;

bd_status bd_media_status_get(bd_catalog *cat, int64_t media_id, bd_media_status *out);

/* Export every file in the catalog as a CSV file to open in a spreadsheet: its
 * folder, path, size, modified time (UTC), whether it was deleted from its
 * folder, how many versions BRODALF has seen, and which drives hold it as
 * it is now. UTF-8 with a byte order mark. *rows may be NULL. */
bd_status bd_export_csv(bd_catalog *cat, const char *path, int64_t *rows);

/* "2026-11-25 14:03:00" in UTC; "" for 0. */
void bd_format_utc(int64_t ms, char *out, size_t cap);

/* ---- Options ------------------------------------------------------------ */

/* Settings kept in the catalog. BRODALF starts nothing by itself; these
 * only shape what the user's own jobs do and what the app shows.
 *   "check_days"   a drive whose oldest copy was last read back this many
 *                  days ago shows as due for a check when it is plugged in
 *                  (default 180; 0: never). A copy never read back since
 *                  it was written makes it due 7 days after it was written
 *                  (or check_days, if less). The app only offers a check
 *                  the user can dismiss.
 *   "verify_days"  "read back copies not checked lately" reads the copies
 *                  not read back in this many days (default 30; 0: all).
 *   "reserve_mb"   space kept free on every drive so the newest catalog can
 *                  always be written there (default 512).
 *   "low_priority" 1: jobs run in the background so the PC stays usable,
 *                  0: full speed (default 1).
 *   "parallel_copies" 1: "Back up to every plugged-in drive" writes each
 *                  file to all of them at once (bd_backup_many); 0: one
 *                  drive after another (default 0).
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
    /* Drives holding a good copy of the current version (for
     * a folder: of any file inside), as "Label (kept in)" joined by "; ".
     * "" if none. Drives marked lost or broken are left out. */
    const char *where;
} bd_search_info;

typedef int (*bd_search_fn)(void *ctx, const bd_search_info *info);

/* Files and folders whose path (within their source) contains every word
 * of text, ignoring case. Names that match come first. Deleted files are
 * included. At most limit results (0: 500). */
bd_status bd_search(bd_catalog *cat, const char *text, int limit, bd_search_fn fn, void *ctx);

/* ---- Backup sets and files at risk --------------------------------------- */

/* How many backups every file needs. A copy counts when BRODALF last
 * saw it good; each drive counts once. A place is what a drive's "kept in"
 * says (compared ignoring case and outer spaces); drives with nothing set
 * count together as one unknown place. */
typedef struct {
    int copies;  /* good copies of each file's current version (default 2) */
    int places;  /* how many different places they must be in (default 2; 1 turns this off) */
} bd_target;

bd_status bd_target_get(bd_catalog *cat, bd_target *out);
/* copies 1..16, places 1..copies. */
bd_status bd_target_set(bd_catalog *cat, const bd_target *target);

/* Progress toward each full set of backups: set k is done when every live
 * file in the backup roots, as the last scan found it, has at least k good
 * copies on drives that count. A new or changed file lowers every set until
 * it is backed up again. out gets one entry per set, up to cap; *sets gets
 * the target's copies. */
typedef struct {
    int set;               /* 1, 2, ... */
    int64_t files_done;
    int64_t files_total;
    int64_t bytes_done;
    int64_t bytes_total;
} bd_set_progress;
bd_status bd_backup_sets(bd_catalog *cat, bd_set_progress *out, int cap, int *sets);

/* Which drives make up each full copy, for naming them: a file's good
 * copies on drives that count are ranked by drive (oldest drive first), and
 * full copy k is on the drives that hold some file's k-th copy. out gets one
 * entry for each set and drive, ordered by set and then drive, up to cap;
 * *n how many. */
typedef struct {
    int set;          /* 1, 2, ... */
    int64_t media_id;
    int64_t files;    /* files whose k-th copy is on it */
} bd_set_drive;
bd_status bd_backup_set_drives(bd_catalog *cat, bd_set_drive *out, int cap, int *n);

typedef struct {
    int64_t node_id;
    int64_t source_id;
    const char *source_name;
    const char *rel_path;
    int64_t size;
    int copies;         /* good copies of the current version */
    int places;         /* different places they are in */
    int unknown_place;  /* one of those places is drives with no "kept in" set */
    int older_copies;   /* the current version has no good copy anywhere, but an
                           older version has: the file changed since its last backup */
    /* A cloud file that is online-only on this PC (OneDrive, Dropbox): 1
     * never on this PC, so never read; 2 changed in the cloud since its
     * last backup. Set it to "Always keep on this device" and scan again to
     * back it up. 0 for any other file. */
    int online_only;
} bd_risk_info;

typedef struct {
    int64_t files_total;    /* live files in the catalog (or the source) */
    int64_t files_at_risk;  /* short of the target */
    int64_t files_no_copy;  /* no good copy of the current version anywhere */
    int64_t bytes_at_risk;
    /* Files deleted from their folder, and older versions of files, whose
     * copies BRODALF still tracks: those with a good copy, and of them those
     * with fewer good copies than the target. */
    int64_t deleted_kept;
    int64_t deleted_short;
    int64_t old_versions_kept;
    int64_t old_versions_short;
    /* Backup roots that are backed up but have never finished a scan
     * (bd_source_info.last_scan_ms 0): the files they hold are not all in
     * the catalog yet, so the counts above say nothing of them. */
    int64_t roots_not_scanned;
} bd_risk_stats;

typedef int (*bd_risk_fn)(void *ctx, const bd_risk_info *info);

/* Live files whose current version falls short of the target, fewest copies
 * first. source_id 0 means every source. fn may be NULL to only count;
 * stats may be NULL. */
bd_status bd_list_at_risk(bd_catalog *cat, int64_t source_id, bd_risk_fn fn, void *ctx, bd_risk_stats *stats);

/* The largest live files in the backup roots, largest first, so the user can
 * see where the space goes. At most limit (0: 100). The copies and places
 * fields of each entry are filled as for bd_list_at_risk. */
bd_status bd_list_largest(bd_catalog *cat, int limit, bd_risk_fn fn, void *ctx);

typedef struct {
    int64_t node_id;
    int64_t source_id;
    const char *source_name;
    const char *rel_path;
    int64_t size;          /* the current version */
    int64_t old_versions;  /* older versions with a copy on a drive or extra place */
    int64_t old_bytes;     /* what their copies take there, all drives together */
} bd_version_space;

typedef int (*bd_version_space_fn)(void *ctx, const bd_version_space *info);

/* Live files whose older versions take the most space on your drives (every
 * change adds a whole copy, and none is ever removed), most first. At most
 * limit (0: 100); files with no older version on a drive are left out. */
bd_status bd_list_version_space(bd_catalog *cat, int limit, bd_version_space_fn fn, void *ctx);

typedef struct {
    int64_t media_id;
    const char *label;
    const char *kind;      /* always "drive" */
    const char *location;  /* "" if not set */
    int connected;
    int64_t files;         /* at-risk files a backup here would bring closer to the target */
    int64_t bytes;
    int64_t free_bytes;    /* as last read, -1 if never */
    int64_t space_ms;      /* when */
    int fits;              /* all of bytes fits in that free space */
} bd_risk_help;

typedef int (*bd_risk_help_fn)(void *ctx, const bd_risk_help *info);

/* Which drive to plug in next: for every drive, how many at-risk files a
 * backup to it would help (it has no copy yet and adds a copy the file needs
 * or a place it is missing). Most helpful first; drives that would not help
 * are left out. */
bd_status bd_list_risk_help(bd_catalog *cat, bd_risk_help_fn fn, void *ctx);

/* The one drive to suggest for what is short of the target, using the free
 * space each drive had when last seen: the most helpful drive with room for
 * all it would take, or failing that the one that can take the most.
 * BD_ERR_NOT_FOUND when nothing is short or no drive would help. The strings
 * in out stay valid until the next call. */
bd_status bd_suggest_drive(bd_catalog *cat, bd_risk_help *out);

/* ---- App log and error reports -------------------------------------- */

/* "0.2.0" or "0.2.0 (abc1234)" when the build knows its commit. */
const char *bd_version(void);

/* Where the app log goes by default: %LOCALAPPDATA%\BRODALF\brodalf.log on
 * Windows, $XDG_STATE_HOME/brodalf/brodalf.log (or ~/.local/state/...)
 * elsewhere. The environment variable BRODALF_LOG overrides it. malloc'd. */
char *bd_applog_default_path(void);

/* The database last opened in the window app, remembered for this user in
 * a small file next to the app log (%LOCALAPPDATA%\BRODALF\last-database.txt
 * on Windows), so the next start can offer it. Nothing else is kept there.
 * bd_last_database: the path, or NULL when none is remembered; it may no
 * longer be there. malloc'd. BRODALF_STATE overrides the folder. */
char *bd_last_database(void);
bd_status bd_last_database_set(const char *path);

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
