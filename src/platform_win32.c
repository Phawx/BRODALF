#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "platform.h"

#include <windows.h>
#include <bcrypt.h>
#include <io.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* 100ns intervals between 1601-01-01 and 1970-01-01. */
#define BD_EPOCH_DIFF 116444736000000000LL

/* UTF-8 -> UTF-16 with '\\' separators. Absolute drive paths get the
 * \\?\ prefix so paths longer than MAX_PATH work. Caller frees. */
static wchar_t *to_wide(const char *path)
{
    size_t len = strlen(path);
    char *tmp = malloc(len + 1);
    if (!tmp) return NULL;
    for (size_t i = 0; i <= len; i++) tmp[i] = path[i] == '/' ? '\\' : path[i];

    int prefix = (len >= 3 && tmp[1] == ':' && tmp[2] == '\\');
    int n = MultiByteToWideChar(CP_UTF8, 0, tmp, -1, NULL, 0);
    if (n <= 0) { free(tmp); return NULL; }
    wchar_t *w = malloc(sizeof(wchar_t) * (size_t)(n + (prefix ? 4 : 0)));
    if (!w) { free(tmp); return NULL; }
    wchar_t *dst = w;
    if (prefix) { wcscpy(w, L"\\\\?\\"); dst = w + 4; }
    MultiByteToWideChar(CP_UTF8, 0, tmp, -1, dst, n);
    free(tmp);
    return w;
}

static char *to_utf8(const wchar_t *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    char *s = malloc((size_t)n);
    if (s) WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}

static int64_t filetime_to_ns(FILETIME ft)
{
    int64_t t = ((int64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (t - BD_EPOCH_DIFF) * 100;
}

/* Only a reparse point that stands in for another path (a symbolic link or
 * a junction: a "name surrogate") is a link to leave out of a scan. Every
 * other reparse point is an ordinary file or folder with extra plumbing
 * behind it, above all OneDrive and Dropbox placeholders, which carry the
 * attribute even when the file is fully downloaded. Treating those as
 * links would leave a OneDrive folder entirely unprotected. */
static void fill_from_attrs(DWORD attrs, DWORD reparse_tag, DWORD size_hi, DWORD size_lo, FILETIME mtime, bd_stat_t *st)
{
    memset(st, 0, sizeof(*st));
    st->is_link = (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0 && IsReparseTagNameSurrogate(reparse_tag);
    st->is_dir = !st->is_link && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    st->is_file = !st->is_link && !st->is_dir;
    st->size = st->is_file ? (int64_t)(((uint64_t)size_hi << 32) | size_lo) : 0;
    st->mtime_ns = filetime_to_ns(mtime);
}

/* The reparse tag of a reparse point. GetFileAttributesEx does not report
 * it; FindFirstFile does, without opening the file. When it cannot be read
 * the point is taken for a symbolic link, as before. */
static DWORD reparse_tag_of(const wchar_t *w)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(w, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
    if (h == INVALID_HANDLE_VALUE) return IO_REPARSE_TAG_SYMLINK;
    FindClose(h);
    return (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ? fd.dwReserved0 : 0;
}

int bd_stat(const char *path, bd_stat_t *st)
{
    wchar_t *w = to_wide(path);
    if (!w) return -1;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    BOOL ok = GetFileAttributesExW(w, GetFileExInfoStandard, &fa);
    DWORD tag = (ok && (fa.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) ? reparse_tag_of(w) : 0;
    free(w);
    if (!ok) return -1;
    fill_from_attrs(fa.dwFileAttributes, tag, fa.nFileSizeHigh, fa.nFileSizeLow, fa.ftLastWriteTime, st);
    return 0;
}

int bd_mkdirs(const char *path)
{
    char *p = strdup(path);
    if (!p) return -1;
    size_t len = strlen(p);
    for (size_t i = 0; i < len; i++) if (p[i] == '/') p[i] = '\\';
    while (len > 3 && p[len - 1] == '\\') p[--len] = '\0';
    /* Skip the drive ("C:\") or UNC prefix ("\\server\share\"). */
    size_t start = 0;
    if (len >= 2 && p[1] == ':') start = 3;
    else if (len >= 2 && p[0] == '\\' && p[1] == '\\') {
        int seps = 0;
        for (start = 2; start < len && seps < 2; start++) if (p[start] == '\\') seps++;
    }
    for (size_t i = start; i <= len; i++) {
        if (p[i] != '\\' && p[i] != '\0') continue;
        char saved = p[i];
        p[i] = '\0';
        if (strlen(p) > 0) {
            wchar_t *w = to_wide(p);
            if (!w) { free(p); return -1; }
            if (!CreateDirectoryW(w, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
                free(w);
                free(p);
                return -1;
            }
            free(w);
        }
        p[i] = saved;
    }
    free(p);
    return 0;
}

static int do_move(const char *from, const char *to, DWORD flags)
{
    wchar_t *a = to_wide(from), *b = to_wide(to);
    BOOL ok = (a && b) ? MoveFileExW(a, b, flags) : FALSE;
    free(a);
    free(b);
    return ok ? 0 : -1;
}

int bd_rename_replace(const char *from, const char *to)
{
    return do_move(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

int bd_rename_noreplace(const char *from, const char *to)
{
    return do_move(from, to, MOVEFILE_WRITE_THROUGH);
}

int bd_remove(const char *path)
{
    wchar_t *w = to_wide(path);
    if (!w) return -1;
    BOOL ok = DeleteFileW(w);
    free(w);
    return ok ? 0 : -1;
}

int bd_rmdir_empty(const char *path)
{
    wchar_t *w = to_wide(path);
    if (!w) return -1;
    BOOL ok = RemoveDirectoryW(w); /* fails when anything is left inside */
    free(w);
    return ok ? 0 : -1;
}

#ifdef _MSC_VER
static __declspec(thread) int t_in_use;
#else
static __thread int t_in_use;
#endif

int bd_open_was_in_use(void)
{
    return t_in_use;
}

FILE *bd_fopen(const char *path, const char *mode)
{
    t_in_use = 0;
    wchar_t *w = to_wide(path);
    if (!w) return NULL;
    if (strcmp(mode, "rb") == 0) {
        /* Share everything, so a file another program is writing (a log, a
         * database) can still be read. */
        HANDLE h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        DWORD err = GetLastError();
        free(w);
        if (h == INVALID_HANDLE_VALUE) {
            t_in_use = err == ERROR_SHARING_VIOLATION || err == ERROR_LOCK_VIOLATION;
            return NULL;
        }
        int fd = _open_osfhandle((intptr_t)h, _O_RDONLY | _O_BINARY);
        if (fd < 0) { CloseHandle(h); return NULL; }
        FILE *f = _fdopen(fd, "rb");
        if (!f) _close(fd);
        return f;
    }
    wchar_t wmode[8];
    size_t i = 0;
    for (; mode[i] && i < 7; i++) wmode[i] = (wchar_t)mode[i];
    wmode[i] = 0;
    FILE *f = _wfopen(w, wmode);
    if (!f) t_in_use = GetLastError() == ERROR_SHARING_VIOLATION;
    free(w);
    return f;
}

int bd_fsync(FILE *f)
{
    if (fflush(f) != 0) return -1;
    HANDLE h = (HANDLE)_get_osfhandle(_fileno(f));
    if (h == INVALID_HANDLE_VALUE) return -1;
    return FlushFileBuffers(h) ? 0 : -1;
}

int bd_create_exclusive(const char *path)
{
    wchar_t *w = to_wide(path);
    if (!w) return -1;
    HANDLE h = CreateFileW(w, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD err = GetLastError();
    free(w);
    if (h == INVALID_HANDLE_VALUE) return (err == ERROR_FILE_EXISTS || err == ERROR_ALREADY_EXISTS) ? 1 : -1;
    CloseHandle(h);
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
    char *base = strdup(root);
    if (!base) return -1;
    size_t blen = strlen(base);
    while (blen > 3 && (base[blen - 1] == '\\' || base[blen - 1] == '/')) base[--blen] = '\0';

    str_stack stack = {0};
    int stop = 0, rc = 0;
    if (stack_push(&stack, "") != 0) { free(base); return -1; }

    while (stack.len > 0 && !stop) {
        char *rel = stack.items[--stack.len];
        size_t rlen = strlen(rel);
        char *pattern = malloc(blen + rlen + 4);
        if (!pattern) { free(rel); rc = -1; break; }
        sprintf(pattern, rlen ? "%s\\%s\\*" : "%s\\*", base, rel);
        wchar_t *wpat = to_wide(pattern);
        WIN32_FIND_DATAW fd;
        HANDLE h = wpat ? FindFirstFileExW(wpat, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH)
                        : INVALID_HANDLE_VALUE;
        free(wpat);
        if (h == INVALID_HANDLE_VALUE) {
            if (err_cb) err_cb(ctx, pattern, "cannot open folder");
            free(pattern);
            free(rel);
            continue;
        }
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            char *name = to_utf8(fd.cFileName);
            if (!name) continue;
            char *child_rel = malloc(rlen + strlen(name) + 2);
            if (!child_rel) { free(name); rc = -1; stop = 1; break; }
            sprintf(child_rel, rlen ? "%s/%s" : "%s%s", rel, name);
            bd_stat_t st;
            fill_from_attrs(fd.dwFileAttributes, fd.dwReserved0, fd.nFileSizeHigh, fd.nFileSizeLow, fd.ftLastWriteTime, &st);
            int r = cb(ctx, child_rel, &st);
            if (r == BD_WALK_SKIP) { /* leave the folder out */ }
            else if (r != 0) stop = 1;
            else if (st.is_dir && stack_push(&stack, child_rel) != 0) { rc = -1; stop = 1; }
            free(child_rel);
            free(name);
        } while (!stop && FindNextFileW(h, &fd));
        FindClose(h);
        free(pattern);
        free(rel);
    }
    while (stack.len > 0) free(stack.items[--stack.len]);
    free(stack.items);
    free(base);
    return rc;
}

int bd_random_bytes(void *buf, size_t n)
{
    return BCRYPT_SUCCESS(BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG)) ? 0 : -1;
}

int64_t bd_now_ms(void)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return filetime_to_ns(ft) / 1000000;
}

int bd_temp_dir(char *out, size_t out_len)
{
    wchar_t w[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH + 1, w);
    if (n == 0 || n > MAX_PATH) return -1;
    while (n > 3 && w[n - 1] == L'\\') w[--n] = 0;
    char *s = to_utf8(w);
    if (!s) return -1;
    if (strlen(s) + 1 > out_len) { free(s); return -1; }
    strcpy(out, s);
    free(s);
    return 0;
}

int bd_disk_space(const char *path, int64_t *total_bytes, int64_t *free_bytes)
{
    wchar_t *w = to_wide(path);
    if (!w) return -1;
    ULARGE_INTEGER avail, total, freeb;
    BOOL ok = GetDiskFreeSpaceExW(w, &avail, &total, &freeb);
    free(w);
    if (!ok) return -1;
    *total_bytes = (int64_t)total.QuadPart;
    *free_bytes = (int64_t)avail.QuadPart;
    return 0;
}

void bd_sleep_ms(int ms)
{
    Sleep((DWORD)ms);
}

#endif
