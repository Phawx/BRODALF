/* Storage backends. Backup, check and restore talk to a drive through this
 * interface, which is kept general enough for other kinds of storage. Paths
 * ("rel") are '/'-separated and relative to BRODALF/<catalog uuid> on that
 * storage. */
#ifndef BD_STORE_H
#define BD_STORE_H

#include "internal.h"

typedef struct {
    int64_t size;
    int64_t mtime_ns;
} bd_remote_stat;

typedef struct bd_store bd_store;

typedef struct {
    /* 0 found, 1 not found, -1 error (message in store->err). */
    int (*stat)(bd_store *s, const char *rel, bd_remote_stat *st);
    /* Path to read rel directly (local drives), or NULL. */
    char *(*local_path)(bd_store *s, const char *rel);
    /* Download rel into a local file. 0, 1 not found, -1 error. */
    int (*download)(bd_store *s, const char *rel, const char *local_dest);
    /* A local path to write a file before it is uploaded as rel. */
    char *(*staging_path)(bd_store *s, const char *rel);
    /* Put the staged file at rel, replacing what is there, and stat it.
     * The staged file is gone afterwards either way. 0 or -1. */
    int (*upload)(bd_store *s, const char *staged, const char *rel, bd_remote_stat *st);
    /* Move within the storage. 0, 1 if "to" exists and !replace, -1 error. */
    int (*move)(bd_store *s, const char *from, const char *to, int replace);
    /* 0 (also when already gone) or -1. */
    int (*remove)(bd_store *s, const char *rel);
    /* 0 or -1. */
    int (*space)(bd_store *s, int64_t *total, int64_t *free_bytes);
    void (*close)(bd_store *s);
} bd_store_ops;

struct bd_store {
    const bd_store_ops *ops;
    bd_catalog *cat;
    int64_t media_id;
    char err[512];
    void *impl;
};

/* A store for a connected drive, or NULL with the reason in the catalog
 * error. Close with bd_store_close. */
bd_store *bd_store_open(bd_catalog *cat, int64_t media_id);
void bd_store_close(bd_store *s);

/* A local file holding rel's bytes: the file itself on a drive, or a
 * downloaded temp copy (then *is_temp is set). 0, 1 not found, -1 error. */
int bd_store_fetch(bd_store *s, const char *rel, char **path_out, int *is_temp);
void bd_store_release(char *path, int is_temp);

/* A unique file name in the temp folder. */
char *bd_temp_file(const char *tag);

/* Local drives. */
bd_store *bd_store_local(bd_catalog *cat, int64_t media_id, const char *root);

#endif
