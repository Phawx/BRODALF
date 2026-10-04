#ifndef _WIN32

#define _GNU_SOURCE
#include "platform.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/random.h>
#endif

static char *dup_native(const char *path)
{
    char *p = strdup(path);
    if (!p) return NULL;
    for (char *s = p; *s; s++)
        if (*s == '\\') *s = '/';
    return p;
}

static void fill_stat(const struct stat *sb, bd_stat_t *st)
{
    memset(st, 0, sizeof(*st));
    st->is_dir = S_ISDIR(sb->st_mode);
    st->is_file = S_ISREG(sb->st_mode);
    st->is_link = S_ISLNK(sb->st_mode);
    st->size = st->is_file ? (int64_t)sb->st_size : 0;
#if defined(__APPLE__)
    st->mtime_ns = (int64_t)sb->st_mtimespec.tv_sec * 1000000000LL + sb->st_mtimespec.tv_nsec;
#else
    st->mtime_ns = (int64_t)sb->st_mtim.tv_sec * 1000000000LL + sb->st_mtim.tv_nsec;
#endif
}

int bd_stat(const char *path, bd_stat_t *st)
{
    struct stat sb;
    char *p = dup_native(path);
    if (!p) return -1;
    int rc = lstat(p, &sb);
    free(p);
    if (rc != 0) return -1;
    fill_stat(&sb, st);
    return 0;
}

int bd_mkdirs(const char *path)
{
    char *p = dup_native(path);
    if (!p) return -1;
    size_t len = strlen(p);
    while (len > 1 && p[len - 1] == '/') p[--len] = '\0';
    for (char *s = p + 1; *s; s++) {
        if (*s != '/') continue;
        *s = '\0';
        if (mkdir(p, 0777) != 0 && errno != EEXIST) { free(p); return -1; }
        *s = '/';
    }
    int rc = (mkdir(p, 0777) == 0 || errno == EEXIST) ? 0 : -1;
    struct stat sb;
    if (rc == 0 && (stat(p, &sb) != 0 || !S_ISDIR(sb.st_mode))) rc = -1;
    free(p);
    return rc;
}

int bd_rename_replace(const char *from, const char *to)
{
    char *a = dup_native(from), *b = dup_native(to);
    int rc = (a && b) ? rename(a, b) : -1;
    free(a);
    free(b);
    return rc == 0 ? 0 : -1;
}

int bd_rename_noreplace(const char *from, const char *to)
{
    char *a = dup_native(from), *b = dup_native(to);
    int rc = -1;
    if (a && b) {
        /* link() fails if the target exists, which gives no-replace semantics
         * on filesystems that support hard links; fall back to a check. */
        if (link(a, b) == 0) {
            rc = unlink(a) == 0 ? 0 : -1;
        } else if (errno != EEXIST) {
            struct stat sb;
            if (lstat(b, &sb) != 0) rc = rename(a, b) == 0 ? 0 : -1;
        }
    }
    free(a);
    free(b);
    return rc;
}

int bd_remove(const char *path)
{
    char *p = dup_native(path);
    int rc = p ? remove(p) : -1;
    free(p);
    return rc == 0 ? 0 : -1;
}

int bd_rmdir_empty(const char *path)
{
    char *p = dup_native(path);
    int rc = p ? rmdir(p) : -1;
    free(p);
    return rc == 0 ? 0 : -1;
}

FILE *bd_fopen(const char *path, const char *mode)
{
    char *p = dup_native(path);
    FILE *f = p ? fopen(p, mode) : NULL;
    free(p);
    return f;
}

int bd_fsync(FILE *f)
{
    if (fflush(f) != 0) return -1;
    return fsync(fileno(f)) == 0 ? 0 : -1;
}

int bd_create_exclusive(const char *path)
{
    char *p = dup_native(path);
    if (!p) return -1;
    int fd = open(p, O_WRONLY | O_CREAT | O_EXCL, 0666);
    free(p);
    if (fd < 0) return errno == EEXIST ? 1 : -1;
    close(fd);
    return 0;
}

typedef struct {
    char **items;
    size_t len, cap;
} str_stack;

static int stack_push(str_stack *s, const char *v)
{
    if (s->len == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 64;
        char **n = realloc(s->items, cap * sizeof(char *));
        if (!n) return -1;
        s->items = n;
        s->cap = cap;
    }
    s->items[s->len] = strdup(v);
    if (!s->items[s->len]) return -1;
    s->len++;
    return 0;
}

int bd_walk(const char *root, bd_walk_cb cb, bd_walk_err_cb err_cb, void *ctx)
{
    char *base = dup_native(root);
    if (!base) return -1;
    size_t blen = strlen(base);
    while (blen > 1 && base[blen - 1] == '/') base[--blen] = '\0';

    str_stack stack = {0};
    int stop = 0, rc = 0;
    if (stack_push(&stack, "") != 0) { free(base); return -1; }

    while (stack.len > 0 && !stop) {
        char *rel = stack.items[--stack.len];
        size_t rlen = strlen(rel);
        char *dir_path = malloc(blen + rlen + 2);
        if (!dir_path) { free(rel); rc = -1; break; }
        sprintf(dir_path, rlen ? "%s/%s" : "%s", base, rel);

        DIR *d = opendir(dir_path);
        if (!d) {
            if (err_cb) err_cb(ctx, dir_path, strerror(errno));
            free(dir_path);
            free(rel);
            continue;
        }
        struct dirent *de;
        while (!stop && (de = readdir(d)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
            size_t nlen = strlen(de->d_name);
            char *child_rel = malloc(rlen + nlen + 2);
            char *child_path = malloc(strlen(dir_path) + nlen + 2);
            if (!child_rel || !child_path) { free(child_rel); free(child_path); rc = -1; stop = 1; break; }
            sprintf(child_rel, rlen ? "%s/%s" : "%s%s", rel, de->d_name);
            sprintf(child_path, "%s/%s", dir_path, de->d_name);

            struct stat sb;
            if (lstat(child_path, &sb) != 0) {
                if (err_cb) err_cb(ctx, child_path, strerror(errno));
            } else {
                bd_stat_t st;
                fill_stat(&sb, &st);
                int r = cb(ctx, child_rel, &st);
                if (r == BD_WALK_SKIP) { /* leave the folder out */ }
                else if (r != 0) stop = 1;
                else if (st.is_dir && stack_push(&stack, child_rel) != 0) { rc = -1; stop = 1; }
            }
            free(child_rel);
            free(child_path);
        }
        closedir(d);
        free(dir_path);
        free(rel);
    }
    while (stack.len > 0) free(stack.items[--stack.len]);
    free(stack.items);
    free(base);
    return rc;
}

int bd_random_bytes(void *buf, size_t n)
{
#if defined(__linux__)
    size_t got = 0;
    while (got < n) {
        ssize_t r = getrandom((char *)buf + got, n - got, 0);
        if (r < 0) { if (errno == EINTR) continue; break; }
        got += (size_t)r;
    }
    if (got == n) return 0;
#endif
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) return -1;
    size_t r = fread(buf, 1, n, f);
    fclose(f);
    return r == n ? 0 : -1;
}

int64_t bd_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int bd_temp_dir(char *out, size_t out_len)
{
    const char *t = getenv("TMPDIR");
    if (!t || !*t) t = "/tmp";
    size_t len = strlen(t);
    while (len > 1 && t[len - 1] == '/') len--;
    if (len + 1 > out_len) return -1;
    memcpy(out, t, len);
    out[len] = '\0';
    return 0;
}

int bd_disk_space(const char *path, int64_t *total_bytes, int64_t *free_bytes)
{
    struct statvfs sv;
    char *p = dup_native(path);
    int rc = p ? statvfs(p, &sv) : -1;
    free(p);
    if (rc != 0) return -1;
    *total_bytes = (int64_t)sv.f_blocks * (int64_t)sv.f_frsize;
    *free_bytes = (int64_t)sv.f_bavail * (int64_t)sv.f_frsize;
    return 0;
}

void bd_sleep_ms(int ms)
{
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

int bd_open_was_in_use(void)
{
    return 0;
}

#endif
