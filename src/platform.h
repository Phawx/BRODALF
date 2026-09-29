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

FILE *bd_fopen(const char *path, const char *mode);

/* Flush stdio buffers and force the data to stable storage. 0 on success. */
int bd_fsync(FILE *f);

/* Create a file that must not already exist (used for lock files).
 * 0 on success, 1 if it already exists, -1 on other errors. */
int bd_create_exclusive(const char *path);

/* Directory walk. The callback gets the path relative to the root, joined
 * with '/'. Return BD_WALK_SKIP for a folder to leave out what is inside it,
 * any other non-zero value to stop the walk. Entries that cannot be read
 * are reported through err_cb and skipped. */
#define BD_WALK_SKIP 2
typedef int (*bd_walk_cb)(void *ctx, const char *rel_path, const bd_stat_t *st);
typedef void (*bd_walk_err_cb)(void *ctx, const char *path, const char *message);
int bd_walk(const char *root, bd_walk_cb cb, bd_walk_err_cb err_cb, void *ctx);

int bd_random_bytes(void *buf, size_t n);
int64_t bd_now_ms(void);
void bd_sleep_ms(int ms);

/* Writes the system temp directory (no trailing separator). 0 on success. */
int bd_temp_dir(char *out, size_t out_len);

int bd_disk_space(const char *path, int64_t *total_bytes, int64_t *free_bytes);

/* Native separator for building paths. */
#ifdef _WIN32
#define BD_SEP '\\'
#else
#define BD_SEP '/'
#endif

#endif
