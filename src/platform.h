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
} bd_stat_t;

/* 0 on success, -1 if the path does not exist or cannot be read. */
int bd_stat(const char *path, bd_stat_t *st);

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
 * are reported through err_cb and skipped. */
#define BD_WALK_SKIP 2
typedef int (*bd_walk_cb)(void *ctx, const char *rel_path, const bd_stat_t *st);
typedef void (*bd_walk_err_cb)(void *ctx, const char *path, const char *message);
int bd_walk(const char *root, bd_walk_cb cb, bd_walk_err_cb err_cb, void *ctx);

/* A copy of s with letter case folded the way the file system compares
 * names: Windows' own case mapping there; elsewhere (names are case
 * sensitive, so only tests use it) ASCII letters made lower case. Caller
 * frees; NULL when out of memory. */
char *bd_fold_case(const char *s);

int bd_random_bytes(void *buf, size_t n);
int64_t bd_now_ms(void);
void bd_sleep_ms(int ms);

/* Writes the system temp directory (no trailing separator). 0 on success. */
int bd_temp_dir(char *out, size_t out_len);

int bd_disk_space(const char *path, int64_t *total_bytes, int64_t *free_bytes);

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
