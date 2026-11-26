/* Thin OS layer. All paths are UTF-8; '/' and '\\' are both accepted as
 * separators on input. Implemented in platform_posix.c and platform_win32.c. */
#ifndef BD_PLATFORM_H
#define BD_PLATFORM_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    int is_dir;
    int is_file;
    int is_link; /* symlink or reparse point; never followed */
    int online_only; /* Windows: a cloud file (OneDrive "online-only") whose data is not on this PC; reading it downloads it */
    int64_t size;
    int64_t mtime_ns; /* nanoseconds since the Unix epoch */
    int64_t btime_ns; /* created; 0 where the system does not say */
} bd_stat_t;

/* 0 on success, -1 if the path does not exist or cannot be read. */
int bd_stat(const char *path, bd_stat_t *st);

/* The same, telling a path that is not there from one that could not be
 * looked at: 0 found, 1 not there (POSIX ENOENT or ENOTDIR; Windows
 * ERROR_FILE_NOT_FOUND or ERROR_PATH_NOT_FOUND), -1 could not look (a
 * drive unplugged or not ready, an I/O error, no access, ...). A drive
 * that is gone as a whole can still give 1, so before taking a copy for
 * missing, check that its drive is there (bd_media_present). */
int bd_stat_ex(const char *path, bd_stat_t *st);

/* Create a directory and any missing parents. 0 on success. */
int bd_mkdirs(const char *path);

/* Rename, replacing the target if it exists. 0 on success. */
int bd_rename_replace(const char *from, const char *to);

/* Rename, failing if the target exists. 0 on success. */
int bd_rename_noreplace(const char *from, const char *to);

int bd_remove(const char *path);

/* Remove a directory only if it is empty. 0 on success. */
int bd_rmdir_empty(const char *path);

FILE *bd_fopen(const char *path, const char *mode);

/* Flush stdio buffers and force the data to stable storage. 0 on success. */
int bd_fsync(FILE *f);

/* Set an open file's modified time, after its last write (stdio buffers
 * are flushed first). 0 on success. */
int bd_set_mtime(FILE *f, int64_t mtime_ns);
/* The same, and the created time where the system keeps one (Windows);
 * btime_ns 0 leaves it alone. */
int bd_set_times(FILE *f, int64_t mtime_ns, int64_t btime_ns);

/* While a job runs: keep the PC from sleeping, and with low set run the
 * calling thread at background priority (lower disk and processor
 * priority on Windows). Calls nest; each begin needs an end. */
void bd_job_power_begin(int low);
void bd_job_power_end(void);
/* A helper thread of a job (scan hashing) at background priority. */
void bd_thread_low_priority(void);

/* Force closed files' data to stable storage (each is opened again for
 * that), all on one drive, with as few device cache flushes as the OS
 * allows: on Windows each file is written out without one and a single
 * full flush ends the group. failed[i] is set for each file that could not
 * be synced; returns how many. */
int bd_fsync_paths(char **paths, size_t n, int *failed);

/* Make renames and new names in a folder durable: fsync on the folder on
 * POSIX; a no-op on Windows, where renames are written through. */
int bd_fsync_dir(const char *path);

/* Create a file that must not already exist (used for lock files).
 * 0 on success, 1 if it already exists, -1 on other errors. */
int bd_create_exclusive(const char *path);

/* A catalog's working copy in the temp folder belongs to the process that
 * made it. bd_work_owner_begin writes a small marker file naming this
 * process (its ID), before the working copy is made; on Windows the marker
 * is also held open, without sharing, until bd_work_owner_end, which only
 * closes it (the caller removes the file). 0 on success; *handle may be
 * NULL either way.
 * bd_work_in_use: 1 if the working copy made of paths[0..n) (files that
 * are not there are ignored), with its marker at marker_path (which may not
 * be there either), may still be in use, 0 if its owner is gone and the
 * files may be removed. It errs toward 1. On Windows: in use if the marker
 * or any of the files cannot be opened with no sharing (an open handle
 * anywhere, the owner's SQLite connection included, stops that), and, for
 * a working copy with no marker (made by BRODALF 0.9.0 or before), if any
 * of its files was written in the last 10 minutes, which covers the moments
 * between unpacking the catalog and opening it, or between writing a
 * snapshot and reading it, when no handle is open. Elsewhere: in use unless
 * the marker names a process that no longer runs. */
int bd_work_owner_begin(const char *marker_path, void **handle);
void bd_work_owner_end(void *handle);
int bd_work_in_use(const char *marker_path, char **paths, size_t n);

/* Keep a second BRODALF from opening the same catalog. On Windows a named
 * mutex (Local\BRODALF-catalog-<hash of the full path>) plus the lock file
 * held open without sharing; both go away when the process ends, crash or
 * not. Elsewhere the lock file is created and must be removed on release.
 * 0 taken (*handle set), 1 another BRODALF on this computer has it, 2 the
 * lock file is held (on Windows: open elsewhere, maybe another computer),
 * -1 other errors. */
int bd_catalog_lock_take(const char *catalog_path, const char *lock_path, void **handle);
void bd_catalog_lock_release(void *handle, const char *lock_path);

/* Directory walk. The callback gets the path relative to the root, joined
 * with '/'. Return BD_WALK_SKIP for a folder to leave out what is inside it,
 * any other non-zero value to stop the walk. Entries that cannot be read
 * are reported through err_cb and skipped; so is a folder that cannot be
 * opened or read to the end, or that holds a name that cannot be turned
 * into UTF-8 (Windows), with the folder's path (on Windows its search
 * pattern, the path with a backslash and a star added). */
#define BD_WALK_SKIP 2
typedef int (*bd_walk_cb)(void *ctx, const char *rel_path, const bd_stat_t *st);
typedef void (*bd_walk_err_cb)(void *ctx, const char *path, const char *message);
int bd_walk(const char *root, bd_walk_cb cb, bd_walk_err_cb err_cb, void *ctx);
#ifndef _WIN32
/* Tests only: reading the folder with this path relative to the root ("" the
 * root itself) fails with EIO after its first entry. */
extern const char *bd_test_walk_fail;
#endif

/* A copy of s with letter case folded the way the file system compares
 * names: Windows' own case mapping there; elsewhere (names are case
 * sensitive, so only tests use it) ASCII letters made lower case. Caller
 * frees; NULL when out of memory. */
char *bd_fold_case(const char *s);

int bd_random_bytes(void *buf, size_t n);

/* Keep secret bytes (an encryption key) out of the swap file: lock the
 * memory they are in, until bd_secret_unlock. Best effort; nothing happens
 * when the system refuses. */
void bd_secret_lock(void *p, size_t n);
void bd_secret_unlock(void *p, size_t n);

/* This process holds a database that must not reach the disk unencrypted:
 * a crash report leaves out the memory it lives in (Windows: no heap in
 * Windows Error Reporting's dumps; elsewhere: no core file). Lasts until
 * the process ends. */
void bd_no_crash_dumps_of_memory(void);

int64_t bd_now_ms(void);
void bd_sleep_ms(int ms);

/* Writes the system temp directory (no trailing separator). 0 on success. */
int bd_temp_dir(char *out, size_t out_len);

int bd_disk_space(const char *path, int64_t *total_bytes, int64_t *free_bytes);

/* Windows' own file compression (NTFS). bd_fs_can_compress: 1 if the
 * volume holding path can compress files, 0 if not (exFAT, FAT32, any
 * other system). bd_fs_compressed: 1 if the file or folder is compressed,
 * 0 if not, -1 if it cannot be read. bd_fs_set_compressed: 0 or -1. A
 * folder set to compressed makes the files later made in it compressed;
 * files already in it stay as they are until set one by one.
 * bd_fs_size_on_disk: the room a file takes, or -1. */
int bd_fs_can_compress(const char *path);
/* The name of the file system holding path, as the system gives it
 * ("NTFS", "exFAT", "FAT32", "FAT" ...), into out. 0 on success, -1 if it
 * cannot be told. Elsewhere than Windows only Linux tells, and only "FAT"
 * (which may be FAT32 or older), "exFAT" or "other". */
int bd_fs_name(const char *path, char *out, size_t cap);
int bd_fs_compressed(const char *path);
int bd_fs_set_compressed(const char *path, int on);
int64_t bd_fs_size_on_disk(const char *path);

/* 1 if the last bd_fopen on this thread failed because another program has
 * the file open and does not let others read it (Windows sharing or lock
 * violation). Files are opened for reading with every sharing mode, so only
 * files locked against reading fail. */
int bd_open_was_in_use(void);

/* Threads, for the few places that overlap work (a big copy, hashing during
 * a scan). Objects are heap-allocated; the functions returning pointers give
 * NULL when the OS refuses, and callers then do the work on their own
 * thread instead. */
typedef struct bd_thread bd_thread;
typedef struct bd_mutex bd_mutex;
typedef struct bd_cond bd_cond;
typedef void (*bd_thread_fn)(void *arg);
bd_thread *bd_thread_start(bd_thread_fn fn, void *arg);
void bd_thread_join(bd_thread *t); /* waits, then frees */
bd_mutex *bd_mutex_new(void);
void bd_mutex_free(bd_mutex *m);
void bd_mutex_lock(bd_mutex *m);
void bd_mutex_unlock(bd_mutex *m);
bd_cond *bd_cond_new(void);
void bd_cond_free(bd_cond *c);
void bd_cond_wait(bd_cond *c, bd_mutex *m);
void bd_cond_signal(bd_cond *c);
void bd_cond_broadcast(bd_cond *c);
/* Logical processors, at least 1. */
int bd_cpu_count(void);

#ifdef _MSC_VER
#define BD_THREAD_LOCAL __declspec(thread)
#else
#define BD_THREAD_LOCAL __thread
#endif

/* A flag one thread sets and others read. */
#ifdef _MSC_VER
#include <intrin.h>
#define bd_flag_get(p) _InterlockedOr((volatile long *)(p), 0)
#define bd_flag_set(p, v) _InterlockedExchange((volatile long *)(p), (long)(v))
#else
#define bd_flag_get(p) __atomic_load_n((p), __ATOMIC_SEQ_CST)
#define bd_flag_set(p, v) __atomic_store_n((p), (long)(v), __ATOMIC_SEQ_CST)
#endif

/* Native separator for building paths. */
#ifdef _WIN32
#define BD_SEP '\\'
#else
#define BD_SEP '/'
#endif

#endif
