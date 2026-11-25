#ifndef _WIN32

#define _GNU_SOURCE
#include "platform.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#ifdef __linux__
#include <sys/vfs.h>
#endif
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
    st->btime_ns = (int64_t)sb->st_birthtimespec.tv_sec * 1000000000LL + sb->st_birthtimespec.tv_nsec;
#else
    st->mtime_ns = (int64_t)sb->st_mtim.tv_sec * 1000000000LL + sb->st_mtim.tv_nsec;
#endif
}

int bd_stat_ex(const char *path, bd_stat_t *st)
{
    struct stat sb;
    char *p = dup_native(path);
    if (!p) return -1;
    int rc = lstat(p, &sb);
    int err = errno;
    free(p);
    if (rc != 0) return err == ENOENT || err == ENOTDIR ? 1 : -1;
    fill_stat(&sb, st);
    return 0;
}

int bd_stat(const char *path, bd_stat_t *st)
{
    return bd_stat_ex(path, st) == 0 ? 0 : -1;
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

int bd_set_mtime(FILE *f, int64_t mtime_ns)
{
    if (fflush(f) != 0) return -1;
    struct timespec ts[2];
    ts[0].tv_sec = 0;
    ts[0].tv_nsec = UTIME_OMIT; /* leave the access time alone */
    ts[1].tv_sec = (time_t)(mtime_ns / 1000000000LL);
    ts[1].tv_nsec = (long)(mtime_ns % 1000000000LL);
    if (ts[1].tv_nsec < 0) { ts[1].tv_sec--; ts[1].tv_nsec += 1000000000L; }
    return futimens(fileno(f), ts) == 0 ? 0 : -1;
}

int bd_set_times(FILE *f, int64_t mtime_ns, int64_t btime_ns)
{
    (void)btime_ns; /* no portable way to set a created time here */
    return bd_set_mtime(f, mtime_ns);
}

void bd_job_power_begin(int low) { (void)low; }
void bd_job_power_end(void) {}
void bd_thread_low_priority(void) {}

int bd_fsync_paths(char **paths, size_t n, int *failed)
{
    int bad = 0;
    for (size_t i = 0; i < n; i++) {
        char *p = dup_native(paths[i]);
        int fd = p ? open(p, O_WRONLY) : -1;
        free(p);
        int rc = fd < 0 ? -1 : fsync(fd);
        if (fd >= 0 && close(fd) != 0) rc = -1;
        failed[i] = rc != 0;
        bad += failed[i];
    }
    return bad;
}

int bd_fsync_dir(const char *path)
{
    char *p = dup_native(path);
    int fd = p ? open(p, O_RDONLY | O_DIRECTORY) : -1;
    free(p);
    if (fd < 0) return -1;
    int rc = fsync(fd);
    close(fd);
    return rc == 0 ? 0 : -1;
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

int bd_work_owner_begin(const char *marker_path, void **handle)
{
    *handle = NULL;
    char *p = dup_native(marker_path);
    if (!p) return -1;
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    free(p);
    if (fd < 0) return -1;
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%ld\n", (long)getpid());
    int ok = write(fd, buf, (size_t)len) == (ssize_t)len;
    if (close(fd) != 0) ok = 0;
    return ok ? 0 : -1;
}

void bd_work_owner_end(void *handle)
{
    (void)handle;
}

int bd_work_in_use(const char *marker_path, char **paths, size_t n)
{
    (void)paths;
    (void)n;
    char *p = dup_native(marker_path);
    if (!p) return 1;
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    free(p);
    if (fd < 0) return 1; /* no marker: the owner is not known */
    char buf[32];
    ssize_t got = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (got <= 0) return 1;
    buf[got] = '\0';
    char *end;
    errno = 0;
    long pid = strtol(buf, &end, 10);
    if (errno != 0 || end == buf || (*end != '\n' && *end != '\0') || pid <= 0 || (pid_t)pid != pid) return 1;
    if ((pid_t)pid == getpid()) return 1;
    if (kill((pid_t)pid, 0) == 0) return 1;
    return errno == ESRCH ? 0 : 1; /* EPERM: it runs, as another user */
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

const char *bd_test_walk_fail = NULL;

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
        /* readdir gives NULL at the end and on an error, telling them apart
         * only by errno, which everything in the loop may change: clear it
         * just before each call. */
        struct dirent *de = NULL;
        int seen = 0, read_err = 0;
        while (!stop) {
            errno = 0;
            if (bd_test_walk_fail && seen >= 1 && strcmp(rel, bd_test_walk_fail) == 0) {
                de = NULL;
                errno = EIO;
            } else {
                de = readdir(d);
            }
            if (!de) { read_err = errno; break; }
            seen++;
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
        /* What was read is kept; the folder is reported so the scan treats
         * it as not read in full and marks nothing in it deleted. */
        if (read_err && err_cb) err_cb(ctx, dir_path, strerror(read_err));
        free(dir_path);
        free(rel);
    }
    while (stack.len > 0) free(stack.items[--stack.len]);
    free(stack.items);
    free(base);
    return rc;
}

char *bd_fold_case(const char *s)
{
    char *r = strdup(s);
    if (!r) return NULL;
    for (char *p = r; *p; p++)
        if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
    return r;
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

/* File compression is Windows' own (NTFS): nothing to do elsewhere. */
int bd_fs_name(const char *path, char *out, size_t cap)
{
    if (cap) out[0] = '\0';
#ifdef __linux__
    char *p = dup_native(path);
    if (!p) return -1;
    struct statfs sf;
    int rc = statfs(p, &sf);
    free(p);
    if (rc != 0) return -1;
    const char *name = "other";
    if ((unsigned long)sf.f_type == 0x4d44ul) name = "FAT";              /* MSDOS_SUPER_MAGIC: FAT12, 16 or 32 */
    else if ((unsigned long)sf.f_type == 0x2011bab0ul) name = "exFAT";   /* EXFAT_SUPER_MAGIC */
    if (strlen(name) >= cap) return -1;
    strcpy(out, name);
    return 0;
#else
    (void)path;
    return -1;
#endif
}

int bd_fs_can_compress(const char *path)
{
    (void)path;
    return 0;
}

int bd_fs_compressed(const char *path)
{
    bd_stat_t st;
    return bd_stat(path, &st) == 0 ? 0 : -1;
}

int bd_fs_set_compressed(const char *path, int on)
{
    (void)path;
    (void)on;
    return -1;
}

int64_t bd_fs_size_on_disk(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (int64_t)st.st_blocks * 512;
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

/* ---- Threads ------------------------------------------------------------ */

struct bd_thread { pthread_t t; bd_thread_fn fn; void *arg; };
struct bd_mutex { pthread_mutex_t m; };
struct bd_cond { pthread_cond_t c; };

static void *thread_main(void *p)
{
    bd_thread *t = p;
    t->fn(t->arg);
    return NULL;
}

bd_thread *bd_thread_start(bd_thread_fn fn, void *arg)
{
    bd_thread *t = malloc(sizeof(*t));
    if (!t) return NULL;
    t->fn = fn;
    t->arg = arg;
    if (pthread_create(&t->t, NULL, thread_main, t) != 0) { free(t); return NULL; }
    return t;
}

void bd_thread_join(bd_thread *t)
{
    if (!t) return;
    pthread_join(t->t, NULL);
    free(t);
}

bd_mutex *bd_mutex_new(void)
{
    bd_mutex *m = malloc(sizeof(*m));
    if (m && pthread_mutex_init(&m->m, NULL) != 0) { free(m); m = NULL; }
    return m;
}

void bd_mutex_free(bd_mutex *m)
{
    if (!m) return;
    pthread_mutex_destroy(&m->m);
    free(m);
}

void bd_mutex_lock(bd_mutex *m) { pthread_mutex_lock(&m->m); }
void bd_mutex_unlock(bd_mutex *m) { pthread_mutex_unlock(&m->m); }

bd_cond *bd_cond_new(void)
{
    bd_cond *c = malloc(sizeof(*c));
    if (c && pthread_cond_init(&c->c, NULL) != 0) { free(c); c = NULL; }
    return c;
}

void bd_cond_free(bd_cond *c)
{
    if (!c) return;
    pthread_cond_destroy(&c->c);
    free(c);
}

void bd_cond_wait(bd_cond *c, bd_mutex *m) { pthread_cond_wait(&c->c, &m->m); }
void bd_cond_signal(bd_cond *c) { pthread_cond_signal(&c->c); }
void bd_cond_broadcast(bd_cond *c) { pthread_cond_broadcast(&c->c); }

int bd_cpu_count(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}

/* The lock file is the lock: a crash leaves it behind, and the user is
 * told it can be removed. */
static char posix_lock_token;

int bd_catalog_lock_take(const char *catalog_path, const char *lock_path, void **handle)
{
    (void)catalog_path;
    *handle = NULL;
    int rc = bd_create_exclusive(lock_path);
    if (rc == 0) *handle = &posix_lock_token;
    return rc;
}

void bd_catalog_lock_release(void *handle, const char *lock_path)
{
    if (handle) bd_remove(lock_path);
}

#endif
