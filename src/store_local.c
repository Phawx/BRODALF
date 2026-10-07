/* Storage on a drive or folder: plain file operations under
 * <root>/BRODALF/<catalog uuid>. */
#include "store.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    char *root;
    char *dir; /* <root>/BRODALF/<catalog uuid> */
} local_impl;

static char *full(bd_store *s, const char *rel)
{
    local_impl *l = s->impl;
    return bd_path_join(l->dir, rel);
}

static int ensure_parent(const char *path)
{
    char *p = bd_strdup(path);
    if (!p) return -1;
    char *slash = strrchr(p, '/');
    char *bslash = strrchr(p, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
    int rc = 0;
    if (slash) { *slash = '\0'; rc = bd_mkdirs(p); }
    free(p);
    return rc;
}

static int l_stat(bd_store *s, const char *rel, bd_remote_stat *st)
{
    char *p = full(s, rel);
    if (!p) return -1;
    bd_stat_t fs;
    int rc = bd_stat(p, &fs);
    free(p);
    memset(st, 0, sizeof(*st));
    if (rc != 0 || !fs.is_file) return 1;
    st->size = fs.size;
    st->mtime_ns = fs.mtime_ns;
    return 0;
}

static char *l_local_path(bd_store *s, const char *rel)
{
    return full(s, rel);
}

static int l_download(bd_store *s, const char *rel, const char *dest)
{
    (void)s; (void)rel; (void)dest;
    return -1; /* never needed: local_path is used instead */
}

static char *l_staging_path(bd_store *s, const char *rel)
{
    char *p = full(s, rel);
    if (p && ensure_parent(p) != 0) {
        snprintf(s->err, sizeof(s->err), "cannot create the folder for %s", p);
        free(p);
        return NULL;
    }
    return p;
}

static int l_upload(bd_store *s, const char *staged, const char *rel, bd_remote_stat *st)
{
    char *p = full(s, rel);
    if (!p) return -1;
    int rc = 0;
    if (strcmp(p, staged) != 0) {
        rc = ensure_parent(p) == 0 && bd_rename_replace(staged, p) == 0 ? 0 : -1;
        if (rc != 0) { bd_remove(staged); snprintf(s->err, sizeof(s->err), "cannot write %s", p); }
    }
    free(p);
    if (rc == 0 && l_stat(s, rel, st) != 0) rc = -1;
    return rc;
}

/* After a move, the folders it left empty go too, up to the catalog's own
 * folder on the drive, so a renamed tree does not leave husks behind. */
static void drop_empty_parents(bd_store *s, char *path)
{
    local_impl *l = s->impl;
    size_t keep = strlen(l->dir);
    for (;;) {
        char *slash = strrchr(path, '/');
        char *bslash = strrchr(path, '\\');
        if (bslash && (!slash || bslash > slash)) slash = bslash;
        if (!slash || (size_t)(slash - path) <= keep) return;
        *slash = '\0';
        if (bd_rmdir_empty(path) != 0) return;
    }
}

static int l_move(bd_store *s, const char *from, const char *to, int replace)
{
    char *a = full(s, from), *b = full(s, to);
    int rc = -1;
    bd_stat_t st;
    if (a && b && ensure_parent(b) == 0) {
        if (!replace && bd_stat(b, &st) == 0) rc = 1;
        else rc = (replace ? bd_rename_replace(a, b) : bd_rename_noreplace(a, b)) == 0 ? 0 : -1;
    }
    if (rc < 0) snprintf(s->err, sizeof(s->err), "cannot move %s to %s", from, to);
    else if (rc == 0) drop_empty_parents(s, a);
    free(a);
    free(b);
    return rc;
}

static int l_remove(bd_store *s, const char *rel)
{
    char *p = full(s, rel);
    if (!p) return -1;
    bd_stat_t st;
    int rc = bd_stat(p, &st) != 0 || bd_remove(p) == 0 ? 0 : -1;
    free(p);
    return rc;
}

static int l_space(bd_store *s, int64_t *total, int64_t *free_bytes)
{
    local_impl *l = s->impl;
    return bd_disk_space(l->root, total, free_bytes);
}

static void l_close(bd_store *s)
{
    local_impl *l = s->impl;
    if (l) { free(l->root); free(l->dir); free(l); }
}

static const bd_store_ops local_ops = {l_stat, l_local_path, l_download, l_staging_path, l_upload,
                                       l_move, l_remove, l_space, l_close};

bd_store *bd_store_local(bd_catalog *cat, int64_t media_id, const char *root)
{
    bd_store *s = calloc(1, sizeof(*s));
    local_impl *l = calloc(1, sizeof(*l));
    if (!s || !l) { free(s); free(l); return NULL; }
    l->root = bd_strdup(root);
    l->dir = bd_media_catalog_dir(cat, root);
    if (!l->root || !l->dir) { free(l->root); free(l->dir); free(l); free(s); return NULL; }
    s->ops = &local_ops;
    s->cat = cat;
    s->media_id = media_id;
    s->impl = l;
    return s;
}

/* ---- Shared helpers ---------------------------------------------------- */

char *bd_temp_file(const char *tag)
{
    char dir[1024], id[37];
    if (bd_temp_dir(dir, sizeof(dir)) != 0) return NULL;
    bd_uuid_v4(id);
    return bd_sprintf("%s%cbrodalf-%s-%s", dir, BD_SEP, tag, id);
}

int bd_store_fetch(bd_store *s, const char *rel, char **path_out, int *is_temp)
{
    *path_out = NULL;
    *is_temp = 0;
    char *p = s->ops->local_path(s, rel);
    if (p) {
        bd_stat_t st;
        if (bd_stat(p, &st) != 0 || !st.is_file) { free(p); return 1; }
        *path_out = p;
        return 0;
    }
    char *tmp = bd_temp_file("download");
    if (!tmp) return -1;
    int rc = s->ops->download(s, rel, tmp);
    if (rc != 0) { bd_remove(tmp); free(tmp); return rc; }
    *path_out = tmp;
    *is_temp = 1;
    return 0;
}

void bd_store_release(char *path, int is_temp)
{
    if (path && is_temp) bd_remove(path);
    free(path);
}

void bd_store_close(bd_store *s)
{
    if (!s) return;
    s->ops->close(s);
    free(s);
}
