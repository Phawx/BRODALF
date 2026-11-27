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
    void *lock;       /* bd_catalog_lock_take's handle while this catalog holds the lock */
    char *work_path;  /* unpacked SQLite working copy (a plain catalog) */
    char *owner_path; /* work_path + ".owner": names the process the working copy belongs to */
    /* An encrypted catalog is unpacked into memory, never to the disk:
     * cat->db is then an in-memory database (sqlite3_deserialize), and
     * work_path and owner_path name files that are not there. */
    int in_memory;
    void *owner;      /* bd_work_owner_begin's handle */
    char uuid[37];
    char err[1024];
    bd_progress_fn progress;
    void *progress_ctx;
    int64_t progress_last_ms;
    const char *prog_phase;
    char prog_current[512];
    int64_t prog_files, prog_files_total, prog_bytes, prog_bytes_total;
    int64_t prog_file_bytes, prog_file_total;
    uint8_t key[32];  /* master key while unlocked */
    int have_key;
    volatile long cancel; /* bd_catalog_cancel was called */
    /* Changes since the file was last written: changes counts commits that
     * wrote to the main database (the commit hook) and calls of
     * bd_catalog_mark_changed; saved_changes is what it was at the last
     * save that worked (or at open, when opening changed nothing). */
    int64_t changes, saved_changes;
    int open_upgraded; /* opening changed the database (schema upgrade, fix-ups) */
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
 * -1 for read errors, -2 for write errors, -4 when the job was cancelled
 * (checked between blocks). */
int bd_hash_file(const char *path, FILE *copy_to, char hex_out[BD_HASH_HEX_LEN + 1], int64_t *size_out);

/* Like bd_hash_file, but the source can be a sealed (encrypted) file when
 * src_key is set, and the copy is sealed when dst_key is set. The hash is
 * always of the plaintext. -3 means the sealed source is damaged or was
 * sealed with another key. */
int bd_hash_copy(const char *path, const uint8_t *src_key, FILE *copy_to, const uint8_t *dst_key,
                 char hex_out[BD_HASH_HEX_LEN + 1], int64_t *size_out);

/* Read a plain file once and write it to n copies at the same time (one
 * writer thread each for a big file), sealing copy i when dst_keys[i] is
 * set. The hash is of the file. A copy that cannot be written gets
 * failed[i] = 1 and the others carry on; sizes[i] is what copy i got.
 * 0 when the file was read to the end (check failed[]), -1 read error,
 * -2 every copy failed, -4 cancelled. */
int bd_hash_copy_multi(const char *path, FILE *const *copy_to, const uint8_t *const *dst_keys, int n,
                       char hex_out[BD_HASH_HEX_LEN + 1], int64_t *sizes, int *failed);

/* Streaming encryption (see crypto.c for the format). */
typedef struct bd_sealer bd_sealer;
typedef struct bd_opener bd_opener;
bd_sealer *bd_seal_begin(FILE *out, const uint8_t key[32]); /* NULL, nothing written: no memory or no random nonce */
int bd_seal_write(bd_sealer *s, const void *data, size_t n); /* 0 or -2 */
int bd_seal_end(bd_sealer *s);                                /* writes the last chunk, frees */
bd_opener *bd_open_begin(FILE *in, const uint8_t key[32]);   /* NULL: not a sealed stream */
long bd_open_read(bd_opener *o, void *buf, size_t cap);       /* bytes, 0 at end, -1 I/O, -3 damaged */
void bd_open_end(bd_opener *o);

/* Write to a file, sealed or not; read likewise. */
typedef struct { FILE *file; bd_sealer *sealer; } bd_sink;
typedef struct { FILE *file; bd_opener *opener; } bd_source;
int bd_sink_write(bd_sink *k, const void *data, size_t n);
long bd_source_read(bd_source *k, void *buf, size_t cap);

/* Tests only: pretend every drive has this much free space (-1: ask it). */
extern int64_t bd_test_free_bytes;
/* Tests only: a backup saves the catalog between batches this often (ms;
 * 0: every 5 minutes). */
extern int64_t bd_test_save_every_ms;
/* Tests only: an encrypted catalog is unpacked to the temp folder as 0.10.1
 * and earlier did, to write and read files the way they did. */
extern int bd_test_work_on_disk;
/* Tests only: every drive's file system has this name, and with
 * bd_test_fat_limit > 0 a FAT drive holds files up to that many bytes
 * (instead of 4 GiB less one). */
extern const char *bd_test_fs_name;
extern int64_t bd_test_fat_limit;
/* The biggest file (as stored, sealing included) the drive can hold, or -1
 * for no known limit; root is where it is plugged in, or NULL. */
int64_t bd_drive_file_limit(bd_catalog *cat, int64_t media_id, const char *root);

/* SQL: where a drive is kept, its "kept in" text (all drives with none set
 * share ''). A cloud account left in a catalog by BRODALF 0.3.0 is a place
 * of its own, so the copies it holds still count the way they did. */
#define BD_SQL_PLACE(m) "(CASE WHEN " m ".kind<>'drive' THEN 'cloud:'||" m ".id ELSE lower(trim(COALESCE(" m ".location,''))) END)"
/* SQL: a copy counts toward the copies you want, unless its drive is
 * marked lost or broken (bd_media_mark_lost) or is an extra place for
 * starred files (a Dropbox folder, a share): those are a second line, not
 * one of the N full sets. */
#define BD_SQL_COUNTS(c) \
    "NOT EXISTS(SELECT 1 FROM media rm WHERE rm.id=" c ".media_id AND (rm.retired_ms IS NOT NULL OR rm.secondary<>0))" \
    " AND NOT EXISTS(SELECT 1 FROM temp.uncounted uc WHERE uc.media_id=" c ".media_id)"
/* Nor while it does not count by its disk (temp.uncounted, see
 * bd_refresh_uncounted): it is on the same physical disk as a backup root,
 * so one disk failure loses both, or its health data says it is failing. */
/* SQL: the physical disk a hardware reading h (media_hardware, or
 * temp.root_disks) names, or NULL when it says too little to tell. */
#define BD_SQL_HW_KEY(h)                                                                                  \
    "(CASE WHEN length(trim(" h ".serial))>=4 AND trim(" h ".serial,'0 ')<>''"                             \
    " THEN 'd:'||lower(trim(COALESCE(" h ".model,'')))||'|'||upper(trim(" h ".serial))||'|'||COALESCE(" h ".disk_bytes,0) END)"
/* SQL: the physical disk a drive is on, as last read: two drives that are
 * partitions of one disk share it, and count as one copy. A drive whose
 * disk cannot be told is a disk of its own. */
#define BD_SQL_DISK(mid) \
    "COALESCE((SELECT " BD_SQL_HW_KEY("dh") " FROM media_hardware dh WHERE dh.media_id=" mid "),'m:'||" mid ")"
/* How many distinct disks hold the copies c (an aggregate). */
#define BD_SQL_COPIES(c) "COUNT(DISTINCT " BD_SQL_DISK(c ".media_id") ")"
/* Work out again which drives do not count by their disk (temp.uncounted). */
void bd_refresh_uncounted(bd_catalog *cat);
/* For tests: stands in for bd_drive_hw_read when set. */
extern int (*bd_test_hw_read)(const char *root, bd_drive_hw *hw);
/* For tests: called on each catalog copy just written to a drive. */
extern void (*bd_test_spoil_catalog_copy)(const char *path);
/* SQL: a copy can still be restored from (its drive is not lost or broken),
 * extra places included. */
#define BD_SQL_USABLE(c) \
    "NOT EXISTS(SELECT 1 FROM media rm WHERE rm.id=" c ".media_id AND rm.retired_ms IS NOT NULL)"
/* SQL: the node n is starred, or is in a starred folder (tbl "stars"), or
 * set to be encrypted (tbl "encrypt_paths"). */
#define BD_SQL_MARKED(tbl, n)                                                                           \
    "EXISTS(SELECT 1 FROM " tbl " mk WHERE mk.source_id=" n ".source_id AND (mk.rel_path='' OR mk.rel_path=" n \
    ".rel_path OR substr(" n ".rel_path,1,length(mk.rel_path)+1)=mk.rel_path||'/'))"
/* SQL: the node's backup root is still backed up (bd_source_set_active). */
#define BD_SQL_ACTIVE(n) "NOT EXISTS(SELECT 1 FROM sources xs WHERE xs.id=" n ".source_id AND xs.active=0)"

/* 1 if the drive is marked lost or broken. */
int bd_media_lost(bd_catalog *cat, int64_t media_id);

/* A drive holding a copy never read back since it was written is due for
 * a check this many days after that copy was written (check_days, when
 * smaller). */
#define BD_UNREAD_CHECK_DAYS 7

/* 1 if the drive is still plugged in where it was connected and is the
 * same drive: its BRODALF.media is there and names this catalog and this
 * drive. 0 if it was unplugged, stopped answering or another drive took
 * its place: then what cannot be found on it is not missing. */
int bd_media_present(bd_catalog *cat, int64_t media_id);

/* Copies a job found missing or damaged. They are recorded when the job
 * ends, and only on drives still there then (bd_media_present), so a drive
 * that drops out halfway gets nothing marked. */
typedef struct {
    int64_t copy_id, media_id;
    int bad;       /* damaged; else missing */
    int read_back; /* it was read to the end: its last read-back is now */
} bd_copy_mark;
typedef struct {
    bd_copy_mark *v;
    size_t n, cap;
} bd_copy_marks;
/* 0, or -1 when out of memory. */
int bd_copy_marks_add(bd_copy_marks *m, int64_t copy_id, int64_t media_id, int bad, int read_back);
/* Record them and empty the list. How many were left out because their
 * drive is gone, or -1 on a database error. */
int64_t bd_copy_marks_apply(bd_catalog *cat, bd_copy_marks *m);
void bd_copy_marks_free(bd_copy_marks *m);

/* 1 if a path the catalog stores (a file's path in its backup root, or a
 * copy's on its drive; parts split by '/') stays inside the folder it is
 * put under: not empty, not absolute, no part empty, "." or "..", and with
 * the Windows rules also no ':' (a drive, or a stream) or '\\' in a part
 * and no part of only dots and spaces. A catalog recovered from a drive
 * may hold anything. bd_stored_path_safe uses the rules of the system it
 * runs on (on POSIX ':' and '\\' are ordinary characters in a name);
 * _as picks them, for tests. bd_stored_name_safe: one part, such as a
 * backup root's name. (recover.c) */
int bd_stored_path_safe(const char *rel);
int bd_stored_path_safe_as(const char *rel, int windows);
int bd_stored_name_safe(const char *name);

/* Record that a file now has these contents, as a scan does: the version
 * with this hash (a new one if none has it) becomes the file's current
 * version. 0 with *version_id and *version_no set, -1 on a database error. */
int bd_record_version(bd_catalog *cat, int64_t node_id, const char *hash, int64_t size, int64_t mtime_ns,
                      int64_t *version_id, int64_t *version_no);

/* Bytes a backup leaves free on every drive (the reserve_mb option): room
 * for the copies of the catalog that go there. */
int64_t bd_reserve_bytes(bd_catalog *cat);

/* What a scan found of a backup root's folder, for bd_list_roots_left_out:
 * files_gone > 0, the scan held the root back (most of its files were not
 * there; settings key held_root.<id>); 0, the scan finished it. Kept in the
 * catalog so a backup in a later session leaves the root out too. */
void bd_root_held_note(bd_catalog *cat, int64_t source_id, int64_t files_gone);

/* Catalog settings (the settings table). get: 1 if found. set: 0 on success. */
int bd_setting_get(bd_catalog *cat, const char *key, char *out, size_t cap);
int bd_setting_set(bd_catalog *cat, const char *key, const char *value);

/* The passphrase-wrapped master key, as stored in settings and in the
 * header of an encrypted .brodalf file. */
#define BD_KEY_BLOCK 96
int bd_catalog_key_block(bd_catalog *cat, uint8_t block[BD_KEY_BLOCK]);
/* -1 nomem, -2 the block's Argon2 settings are out of range, -3 wrong */
int bd_unwrap_key_block(const char *pass, const uint8_t block[BD_KEY_BLOCK], uint8_t key[32]);
#define BD_KEY_BLOCK_BAD "the passphrase settings in this catalog are not ones BRODALF writes; the file may be damaged"
/* Tests only: random bytes for keys, salts and nonces fail. */
extern int bd_test_random_fail;
/* The master key wrapped by the recovery key, when there is one. 0 or -1. */
int bd_catalog_recovery_block(bd_catalog *cat, uint8_t block[BD_KEY_BLOCK]);
int bd_unwrap_recovery_block(const char *recovery_key, const uint8_t block[BD_KEY_BLOCK], uint8_t key[32]); /* -1 nomem, -3 wrong */
void bd_catalog_set_key(bd_catalog *cat, const uint8_t key[32]);
/* Move an open catalog's database from its working copy in the temp folder
 * into memory, and remove the working copy (when the catalog file is to be
 * encrypted from now on). BD_OK if it is in memory already. */
bd_status bd_catalog_keep_in_memory(bd_catalog *cat);
void bd_wipe(void *p, size_t n);

int bd_media_encrypted(bd_catalog *cat, int64_t media_id);

/* Save a hardware reading for a drive. Logs when the drive ID turns up on a
 * different disk than last time (a copied or moved BRODALF folder). */
/* SMART parsing, exposed for tests (src/drive_hw.c). */
int bd_drive_hw_parse_ata(bd_drive_hw *hw, const unsigned char data[512], int predicted_failure);
int bd_drive_hw_parse_nvme(bd_drive_hw *hw, const unsigned char log[512]);
/* 1 for the Windows bus types (STORAGE_BUS_TYPE) whose failure-prediction
 * answer holds an ATA SMART attribute table: ATA and SATA only. */
int bd_drive_hw_bus_is_ata(int bus_type);
void bd_media_store_hw(bd_catalog *cat, int64_t media_id, const bd_drive_hw *hw, bd_log_fn log, void *log_ctx);

/* The BRODALF.media file that identifies a drive. */
typedef struct {
    char media_uuid[37];
    char catalog_uuid[37];
    char label[256];
    int encrypted;
    int secondary; /* an extra place for starred files */
    int hide_names; /* file and folder names are hidden (names.c) */
} bd_media_file;
int bd_media_file_read(const char *path, bd_media_file *mf);
int bd_media_file_write(const char *path, const bd_media_file *mf);
bd_status bd_media_record_connected(bd_catalog *cat, int64_t media_id, const char *root);
int64_t bd_media_id_for_uuid(bd_catalog *cat, const char *uuid);
int64_t bd_media_insert(bd_catalog *cat, const char *uuid, const char *kind, const char *label, int encrypted);

/* Set meta schema_version to what the catalog needs (catalog.c). 0 or -1. */
int bd_catalog_note_schema(bd_catalog *cat);

/* The copy from a shadow copy to read in place of path, or NULL (scan.c). */
char *bd_substitute_for(bd_catalog *cat, const char *path);

/* Save a copy of the catalog to dest, encrypted or not. */
bd_status bd_catalog_save_to(bd_catalog *cat, const char *dest, int encrypt);

/* Encrypted copies on a drive carry this suffix. */
#define BD_SEALED_SUFFIX ".bdenc"

void bd_uuid_v4(char out[37]);

/* Hidden names (names.c): the key for one drive, and a path on it as it
 * is written there. NULL when out of memory. */
void bd_names_key(const uint8_t master[32], const char *media_uuid, uint8_t out[32]);
char *bd_hide_rel(const uint8_t names_key[32], const char *plain_rel);
/* 1 if the drive hides names; the key for it in out when so. */
int bd_media_hides_names(bd_catalog *cat, int64_t media_id, uint8_t out[32]);
/* newest.c: newest-copies-r<revision>.txt beside a drive's database copy,
 * naming each file whose newest copy there is not in its own place. Plain
 * drives only; nothing on the drive is overwritten. */
bd_status bd_newest_list_to_media(bd_catalog *cat, int64_t media_id);
/* 1 for such a list's name (or its temp name). */
int bd_newest_list_name(const char *name);

/* Media root directory for this catalog: <root>/BRODALF/<uuid>. */
char *bd_media_catalog_dir(const bd_catalog *cat, const char *root);
/* Root of a connected media, or NULL (caller frees). */
char *bd_connected_root(bd_catalog *cat, int64_t media_id);

int bd_exec(bd_catalog *cat, const char *sql);

/* Record a drive's space (media.total_bytes/free_bytes) and log it in
 * space_log with what was happening ("plugged in", "before backup"...). */
void bd_media_note_space(bd_catalog *cat, int64_t media_id, int64_t total, int64_t free_bytes, const char *event);

/* The jobs table, which drive status reads: a row when a job starts,
 * filled in when it ends ("done", "partial", "stopped", "failed"). While a
 * job runs the PC is kept from sleeping, and with the low_priority option
 * the thread runs at background priority (bd_job_power). */
int64_t bd_job_start(bd_catalog *cat, const char *kind, int64_t media_id);
void bd_job_finish(bd_catalog *cat, int64_t job_id, const char *status, int64_t done, int64_t failed, int64_t bytes);

/* Files in use (scan.c): remember a file that could not be read because
 * another program has it open. */
void bd_note_in_use(bd_catalog *cat, const char *path, int64_t size);

/* Progress: begin a job's phase with its totals (0 if unknown), report the
 * files done and the file being worked on (throttled unless force), and end
 * it. Bytes are counted as they are read (bd_io_tick, from the hashing
 * loop), so they move during a big file too. */
void bd_progress_begin(bd_catalog *cat, const char *phase, int64_t files_total, int64_t bytes_total);
void bd_report(bd_catalog *cat, int64_t files_done, const char *current, int force);
/* bd_report for a file of this size that is about to be read: its own bar. */
void bd_report_file(bd_catalog *cat, int64_t files_done, const char *current, int64_t size);
void bd_progress_end(bd_catalog *cat);
/* From the thread running the job, right after copies were recorded: a
 * progress call with recorded set (see bd_progress). */
void bd_report_recorded(bd_catalog *cat);
void bd_io_tick(int64_t bytes);
/* Send this thread's bd_io_tick calls to fn instead (a hashing worker hands
 * its byte counts to the thread running the job). NULL undoes it. */
void bd_io_redirect(void (*fn)(void *ctx, int64_t bytes), void *ctx);

/* Cancelling (bd_catalog_cancel). A job calls bd_cancel_reset when it starts
 * and bd_cancelled between files; bd_hash_copy checks between blocks. */
void bd_cancel_reset(bd_catalog *cat);
int bd_cancelled(bd_catalog *cat);
int bd_cancel_pending(void); /* the running job's request, from any thread */
void bd_cancel_forget(bd_catalog *cat);
bd_status bd_fail_cancelled(bd_catalog *cat);

/* The key that says two paths name the same file: on Windows, the path
 * with letter case folded (names there ignore case); NULL elsewhere, where
 * the path itself is the key, and when out of memory. Caller frees. */
char *bd_path_key(const char *rel);
/* Tests only: fold case as on Windows. */
extern int bd_test_fold_case;

/* 1 if path is another backup root than except_id (stopped or not), is
 * inside an active one or holds one. */
int bd_root_overlaps(bd_catalog *cat, const char *path, int64_t except_id);
/* The same with a message (BD_ERR_EXISTS naming the root in the way), as
 * bd_source_add, bd_source_relocate and bd_source_set_active check; with
 * nesting 0 (a stopped root) only another root that is this folder counts.
 * *same gets the id of a root that is this very folder, reached any way,
 * or 0. */
bd_status bd_root_check(bd_catalog *cat, const char *path, int64_t except_id, int nesting, int64_t *same);

/* The places a backup root at path may be now, besides path itself: the
 * same folder under the home folder home ("C:\Users\Sam", "/home/sam")
 * when path is under another user's home folder next to it, the same disk
 * mounted for this user (/media/<user>/<disk>, /run/media/<user>/<disk>),
 * and with win, the same folder on each other drive letter in drives (a
 * bit per letter, A: as bit 0). win compares as Windows does, letter case
 * ignored and either slash a separator. A NULL-ended list, each entry and
 * the list malloc'd; NULL when out of memory. */
char **bd_root_candidates(const char *path, const char *home, unsigned drives, int win);

/* 1 if folder surely holds the backup root's files, as bd_source_find
 * says a root is pointed at one without asking (recover.c); found and
 * checked get how many of the files checked are there. Reads a few files. */
int bd_source_place_sure(bd_catalog *cat, int64_t source_id, const char *folder, int64_t *found, int64_t *checked);

/* Remove a catalog setting (scan.c). 0 on success, also when it was not set. */
int bd_setting_del(bd_catalog *cat, const char *key);
/* Tests: scan as on a drive whose files have no change time (FAT), so a
 * change made with the modified time put back goes unseen, as bit rot does. */
extern int bd_test_no_change_time;

/* Give a source whose name on the drives is one of BRODALF's own (the
 * .versions tree, BRODALF.media, the catalog copy) a name of its own; its
 * new copies go under that name. */
bd_status bd_sources_fix_names(bd_catalog *cat, bd_log_fn log, void *log_ctx);

/* Tests only: hashing threads for a scan (0: the default, -1: hash on the
 * scanning thread itself). */
extern int bd_test_scan_workers;
/* Tests: files whose path ends with this are taken for online-only cloud files. */
extern const char *bd_test_online_only;

#endif
