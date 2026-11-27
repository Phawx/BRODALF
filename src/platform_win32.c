#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "platform.h"

#include <windows.h>
#include <winioctl.h>
#include <winnetwk.h>
#include <werapi.h>
#include <bcrypt.h>
#include "blake3.h"
#include <io.h>
#include <fcntl.h>
#include <process.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* 100ns intervals between 1601-01-01 and 1970-01-01. */
#define BD_EPOCH_DIFF 116444736000000000LL

/* What Windows does to a path without the \\?\ prefix, and does not do
 * with it: a run of separators counts as one, a "." part goes, and ".."
 * takes away the part before it, never what keep covers (the "C:\" of a
 * drive path, or the "\\server\share\" of a network one). In place, on a
 * path with '\\' separators only. Without it, C:\a\b\..\c would name a
 * folder called ".." once prefixed. */
static void squeeze_dots(char *p, size_t keep)
{
    char *top = p + keep, *out = top, *in = top;
    while (*in) {
        while (*in == '\\') in++;
        if (!*in) break;
        char *e = in;
        while (*e && *e != '\\') e++;
        size_t seg = (size_t)(e - in);
        if (seg == 1 && in[0] == '.') {
            /* nothing */
        } else if (seg == 2 && in[0] == '.' && in[1] == '.') {
            while (out > top && out[-1] != '\\') out--;
            if (out > top) out--; /* the '\' before that part */
        } else {
            if (out > top) *out++ = '\\';
            memmove(out, in, seg);
            out += seg;
        }
        in = e;
    }
    *out = '\0';
}

/* UTF-8 -> UTF-16 with '\\' separators. Absolute paths get the long-path
 * prefix so paths longer than MAX_PATH work: \\?\ for a drive path
 * (C:\x becomes \\?\C:\x), \\?\UNC\ for a network path (\\server\share\x
 * becomes \\?\UNC\server\share\x). A path that already starts with \\?\ or
 * \\.\ is left as it is, and so is a relative one. Caller frees. */
static wchar_t *to_wide(const char *path)
{
    size_t len = strlen(path);
    char *tmp = malloc(len + 1);
    if (!tmp) return NULL;
    for (size_t i = 0; i <= len; i++) tmp[i] = path[i] == '/' ? '\\' : path[i];

    const char *from = tmp;
    const wchar_t *prefix = NULL;
    if (len >= 3 && tmp[1] == ':' && tmp[2] == '\\') {
        prefix = L"\\\\?\\";
        squeeze_dots(tmp, 3);
    } else if (len >= 3 && tmp[0] == '\\' && tmp[1] == '\\' && tmp[2] != '\\' &&
               !((tmp[2] == '?' || tmp[2] == '.') && tmp[3] == '\\')) {
        prefix = L"\\\\?\\UNC\\";
        from = tmp + 2; /* \\server\share -> \\?\UNC\server\share */
        /* "\\server\share\" stays as it is. */
        const char *share = strchr(from, '\\');
        const char *after = share ? strchr(share + 1, '\\') : NULL;
        if (after) squeeze_dots(tmp, (size_t)(after + 1 - tmp));
    }
    size_t plen = prefix ? wcslen(prefix) : 0;
    int n = MultiByteToWideChar(CP_UTF8, 0, from, -1, NULL, 0);
    if (n <= 0) { free(tmp); return NULL; }
    wchar_t *w = malloc(sizeof(wchar_t) * ((size_t)n + plen));
    if (!w) { free(tmp); return NULL; }
    if (prefix) wcscpy(w, prefix);
    MultiByteToWideChar(CP_UTF8, 0, from, -1, w + plen, n);
    free(tmp);
    return w;
}

/* A network path as 0.9.0 gave it, without the \\?\UNC\ prefix
 * (\\server\share\x), in place: for the volume queries, which only need
 * the share and are not documented for the prefixed form. */
static wchar_t *unprefix_unc(wchar_t *w)
{
    if (w && _wcsnicmp(w, L"\\\\?\\UNC\\", 8) == 0) {
        w[1] = L'\\';
        memmove(w + 2, w + 8, sizeof(wchar_t) * (wcslen(w + 8) + 1));
    }
    return w;
}

#ifndef WC_ERR_INVALID_CHARS
#define WC_ERR_INVALID_CHARS 0x00000080 /* winnls.h, Windows Vista and later */
#endif

/* UTF-16 -> UTF-8. NULL when out of memory or when w is not valid UTF-16
 * (an unpaired surrogate, which NTFS allows in names): such a name is not
 * made up with U+FFFD in its place, since the made-up name would not open
 * the file. Caller frees. */
static char *to_utf8(const wchar_t *w)
{
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    char *s = malloc((size_t)n);
    if (s && WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w, -1, s, n, NULL, NULL) <= 0) {
        free(s);
        return NULL;
    }
    return s;
}

char *bd_link_target(const char *path, int *hidden_system)
{
    if (hidden_system) *hidden_system = 0;
    wchar_t *w = to_wide(path);
    if (!w) return NULL;
    DWORD attrs = GetFileAttributesW(w);
    if (hidden_system && attrs != INVALID_FILE_ATTRIBUTES)
        *hidden_system = (attrs & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) == (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
    /* Opening it follows every link on the way. */
    HANDLE h = CreateFileW(w, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    wchar_t got[MAX_PATH * 4];
    DWORD n = GetFinalPathNameByHandleW(h, got, MAX_PATH * 4, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(h);
    if (n == 0 || n >= MAX_PATH * 4) return NULL;
    wchar_t *p = got;
    if (_wcsnicmp(p, L"\\\\?\\UNC\\", 8) == 0) unprefix_unc(p);
    else if (wcsncmp(p, L"\\\\?\\", 4) == 0) p += 4;
    return to_utf8(p);
}

static int is_sep_w(char c) { return c == '/' || c == '\\'; }

int bd_path_is_full(const char *path)
{
    if (!path || !path[0]) return 0;
    if (is_sep_w(path[0]) && is_sep_w(path[1])) return 1;
    return path[1] == ':' && is_sep_w(path[2]);
}

char *bd_full_path(const char *path)
{
    /* A full path is kept as it was typed, as 0.13.0 and older kept every
     * path (its "." and ".." are dealt with wherever it is compared). */
    if (bd_path_is_full(path)) return _strdup(path);
    size_t len = strlen(path);
    char *tmp = malloc(len + 1);
    if (!tmp) return NULL;
    for (size_t i = 0; i <= len; i++) tmp[i] = path[i] == '/' ? '\\' : path[i];
    int n = MultiByteToWideChar(CP_UTF8, 0, tmp, -1, NULL, 0);
    wchar_t *w = n > 0 ? malloc(sizeof(wchar_t) * (size_t)n) : NULL;
    if (w) MultiByteToWideChar(CP_UTF8, 0, tmp, -1, w, n);
    free(tmp);
    if (!w) return NULL;
    DWORD need = GetFullPathNameW(w, 0, NULL, NULL);
    wchar_t *full = need ? malloc(sizeof(wchar_t) * need) : NULL;
    DWORD got = full ? GetFullPathNameW(w, need, full, NULL) : 0;
    free(w);
    char *out = got && got < need ? to_utf8(full) : NULL;
    free(full);
    return out ? out : _strdup(path);
}

char *bd_real_path(const char *path)
{
    if (!path || !path[0]) return NULL;
    if (path[1] == ':' && (is_sep_w(path[2]) || path[2] == '\0')) {
        wchar_t root[4] = {(wchar_t)path[0], L':', L'\\', 0};
        if (GetDriveTypeW(root) == DRIVE_REMOTE) {
            /* A mapped network drive: the network path Windows keeps for it,
             * also while it is not connected. */
            wchar_t dev[3] = {(wchar_t)path[0], L':', 0};
            wchar_t remote[MAX_PATH * 2];
            DWORD cap = MAX_PATH * 2;
            DWORD r = WNetGetConnectionW(dev, remote, &cap);
            if (r != NO_ERROR && r != ERROR_CONNECTION_UNAVAIL) return NULL;
            char *share = to_utf8(remote);
            if (!share) return NULL;
            size_t sl = strlen(share), rl = strlen(path + 2);
            char *out = malloc(sl + rl + 1);
            if (out) {
                memcpy(out, share, sl);
                memcpy(out + sl, path + 2, rl + 1);
            }
            free(share);
            return out;
        }
        /* A subst drive letter: the folder it stands for ("\\??\\C:\\x", or
         * "\\??\\UNC\\server\\share" for a network one). */
        wchar_t dev[3] = {(wchar_t)path[0], L':', 0}, target[MAX_PATH * 2];
        DWORD tn = QueryDosDeviceW(dev, target, MAX_PATH * 2);
        if (tn && wcsncmp(target, L"\\??\\", 4) == 0) {
            const wchar_t *t = target + 4;
            int unc = _wcsnicmp(t, L"UNC\\", 4) == 0;
            char *folder = to_utf8(unc ? t + 3 : t);
            if (!folder) return NULL;
            size_t fl = strlen(folder), rl = strlen(path + 2);
            char *joined = malloc(fl + rl + 2);
            if (joined) {
                size_t o = 0;
                if (unc) joined[o++] = '\\';
                memcpy(joined + o, folder, fl);
                memcpy(joined + o + fl, path + 2, rl + 1);
            }
            free(folder);
            if (!joined) return NULL;
            char *real = bd_real_path(joined);
            if (!real) return joined; /* a network folder, or one not there: as the subst says */
            free(joined);
            return real;
        }
    }
    if (is_sep_w(path[0]) && is_sep_w(path[1]) && !(path[2] == '?' && is_sep_w(path[3]) && path[4] && path[5] == ':'))
        return NULL; /* a network path: asking would go over the network */
    return bd_link_target(path, NULL);
}

static int64_t filetime_to_ns(FILETIME ft)
{
    int64_t t = ((int64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (t - BD_EPOCH_DIFF) * 100;
}

#define BD_RECALL_ON_OPEN 0x00040000u        /* FILE_ATTRIBUTE_RECALL_ON_OPEN */
#define BD_RECALL_ON_DATA_ACCESS 0x00400000u /* FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS */

/* Only a reparse point that stands in for another path (a symbolic link or
 * a junction: a "name surrogate") is a link to leave out of a scan. Every
 * other reparse point is an ordinary file or folder with extra plumbing
 * behind it, above all OneDrive and Dropbox placeholders, which carry the
 * attribute even when the file is fully downloaded. Treating those as
 * links would leave a OneDrive folder entirely unprotected. */
static void fill_from_attrs(DWORD attrs, DWORD reparse_tag, DWORD size_hi, DWORD size_lo, FILETIME mtime,
                            FILETIME btime, bd_stat_t *st)
{
    memset(st, 0, sizeof(*st));
    st->is_link = (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0 && IsReparseTagNameSurrogate(reparse_tag);
    st->is_dir = !st->is_link && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    st->is_file = !st->is_link && !st->is_dir;
    st->size = st->is_file ? (int64_t)(((uint64_t)size_hi << 32) | size_lo) : 0;
    st->mtime_ns = filetime_to_ns(mtime);
    st->btime_ns = (btime.dwHighDateTime | btime.dwLowDateTime) ? filetime_to_ns(btime) : 0;
    /* Opening one of these makes OneDrive (or another cloud app) download it. */
    st->online_only = st->is_file && (attrs & (BD_RECALL_ON_DATA_ACCESS | BD_RECALL_ON_OPEN | FILE_ATTRIBUTE_OFFLINE)) != 0;
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

/* Only "no such file" and "no such folder" mean the path is not there.
 * Everything else (ERROR_NOT_READY, ERROR_DEVICE_NOT_CONNECTED, an I/O
 * error, access denied, a share that is down) means it could not be
 * looked at. */
int bd_stat_ex(const char *path, bd_stat_t *st)
{
    wchar_t *w = to_wide(path);
    if (!w) return -1;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    BOOL ok = GetFileAttributesExW(w, GetFileExInfoStandard, &fa);
    DWORD err = ok ? ERROR_SUCCESS : GetLastError();
    DWORD tag = (ok && (fa.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) ? reparse_tag_of(w) : 0;
    free(w);
    if (!ok) return err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND ? 1 : -1;
    fill_from_attrs(fa.dwFileAttributes, tag, fa.nFileSizeHigh, fa.nFileSizeLow, fa.ftLastWriteTime, fa.ftCreationTime, st);
    return 0;
}

int bd_stat(const char *path, bd_stat_t *st)
{
    return bd_stat_ex(path, st) == 0 ? 0 : -1;
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

int bd_set_mtime(FILE *f, int64_t mtime_ns)
{
    if (fflush(f) != 0) return -1;
    HANDLE h = (HANDLE)_get_osfhandle(_fileno(f));
    if (h == INVALID_HANDLE_VALUE) return -1;
    int64_t t = mtime_ns / 100 + BD_EPOCH_DIFF;
    if (t < 0) return -1; /* before 1601: cannot be set */
    FILETIME ft;
    ft.dwLowDateTime = (DWORD)((uint64_t)t & 0xffffffffu);
    ft.dwHighDateTime = (DWORD)((uint64_t)t >> 32);
    /* Only the last-write time; creation and access times stay. */
    return SetFileTime(h, NULL, NULL, &ft) ? 0 : -1;
}

static int ns_to_filetime(int64_t ns, FILETIME *ft)
{
    int64_t t = ns / 100 + BD_EPOCH_DIFF;
    if (t < 0) return -1;
    ft->dwLowDateTime = (DWORD)((uint64_t)t & 0xffffffffu);
    ft->dwHighDateTime = (DWORD)((uint64_t)t >> 32);
    return 0;
}

int bd_set_times(FILE *f, int64_t mtime_ns, int64_t btime_ns)
{
    if (fflush(f) != 0) return -1;
    HANDLE h = (HANDLE)_get_osfhandle(_fileno(f));
    if (h == INVALID_HANDLE_VALUE) return -1;
    FILETIME mt, bt;
    if (ns_to_filetime(mtime_ns, &mt) != 0) return -1;
    int have_b = btime_ns > 0 && ns_to_filetime(btime_ns, &bt) == 0;
    return SetFileTime(h, have_b ? &bt : NULL, NULL, &mt) ? 0 : -1;
}

/* Jobs keep the PC awake and, with low_priority, step aside for other
 * programs. Counted, since a scan's helper threads and nested jobs share
 * the one process-wide setting. */
static volatile LONG power_depth = 0;

void bd_job_power_begin(int low)
{
    if (InterlockedIncrement(&power_depth) == 1)
        SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
    if (low) SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    else SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
}

void bd_job_power_end(void)
{
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
    if (InterlockedDecrement(&power_depth) <= 0) {
        power_depth = 0;
        SetThreadExecutionState(ES_CONTINUOUS);
    }
}

void bd_thread_low_priority(void)
{
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
}

/* NtFlushBuffersFileEx (Windows 8 and later) can write a file's data and
 * metadata to the drive without making the drive flush its own cache. */
typedef struct { void *status; ULONG_PTR information; } bd_io_status;
typedef LONG (WINAPI *flush_ex_fn)(HANDLE, ULONG, PVOID, ULONG, bd_io_status *);
#define BD_FLUSH_FLAGS_NO_SYNC 0x00000002

static HANDLE open_for_flush(const char *path)
{
    wchar_t *w = to_wide(path);
    if (!w) return INVALID_HANDLE_VALUE;
    /* FlushFileBuffers needs write access; it flushes everything cached for
     * the file, whichever handle wrote it. */
    HANDLE h = CreateFileW(w, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    free(w);
    return h;
}

int bd_fsync_paths(char **paths, size_t n, int *failed)
{
    static flush_ex_fn flush_ex;
    static int looked;
    if (!looked) {
        HMODULE nt = GetModuleHandleW(L"ntdll.dll");
        flush_ex = nt ? (flush_ex_fn)(void (*)(void))GetProcAddress(nt, "NtFlushBuffersFileEx") : NULL;
        looked = 1;
    }
    int bad = 0;
    size_t last_light = n; /* the last file written out without a cache flush */
    for (size_t i = 0; i < n; i++) {
        HANDLE h = open_for_flush(paths[i]);
        BOOL ok = h != INVALID_HANDLE_VALUE;
        int light = 0;
        if (ok) {
            bd_io_status io;
            memset(&io, 0, sizeof(io));
            light = flush_ex && n > 1 && flush_ex(h, BD_FLUSH_FLAGS_NO_SYNC, NULL, 0, &io) >= 0;
            if (!light) ok = FlushFileBuffers(h);
            if (!CloseHandle(h)) ok = FALSE;
        }
        failed[i] = !ok;
        bad += failed[i];
        if (ok && light) last_light = i;
    }
    if (last_light == n) return bad;
    /* One full flush makes the drive write out its cache, and with it every
     * file above. If it fails, none of them counts as synced. */
    HANDLE h = open_for_flush(paths[last_light]);
    BOOL ok = h != INVALID_HANDLE_VALUE && FlushFileBuffers(h);
    if (h != INVALID_HANDLE_VALUE && !CloseHandle(h)) ok = FALSE;
    if (!ok) {
        bad = 0;
        for (size_t i = 0; i < n; i++) { failed[i] = 1; bad++; }
    }
    return bad;
}

int bd_fsync_dir(const char *path)
{
    (void)path; /* MoveFileEx is called with MOVEFILE_WRITE_THROUGH */
    return 0;
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

int bd_work_owner_begin(const char *marker_path, void **handle)
{
    *handle = NULL;
    wchar_t *w = to_wide(marker_path);
    if (!w) return -1;
    /* Held open for the catalog's life; others may read it, nobody may
     * open it without sharing while this handle is open. */
    HANDLE h = CreateFileW(w, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return -1;
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%lu\n", (unsigned long)GetCurrentProcessId());
    DWORD put = 0;
    if (!WriteFile(h, buf, (DWORD)len, &put, NULL) || put != (DWORD)len) {
        CloseHandle(h);
        return -1;
    }
    *handle = h;
    return 0;
}

void bd_work_owner_end(void *handle)
{
    if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle((HANDLE)handle);
}

/* Open with no sharing and close again: 0 it opened (*write_ms its last
 * write), 2 it is not there, 1 anything else (in use, no access). */
static int open_alone(const char *path, int64_t *write_ms)
{
    wchar_t *w = to_wide(path);
    if (!w) return 1;
    HANDLE h = CreateFileW(w, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD err = GetLastError();
    free(w);
    if (h == INVALID_HANDLE_VALUE) return (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) ? 2 : 1;
    FILETIME ft;
    int ok = GetFileTime(h, NULL, NULL, &ft) != 0;
    CloseHandle(h);
    if (!ok) return 1;
    *write_ms = filetime_to_ns(ft) / 1000000;
    return 0;
}

int bd_work_in_use(const char *marker_path, char **paths, size_t n)
{
    int64_t when = 0, now = bd_now_ms();
    int marker = open_alone(marker_path, &when);
    if (marker == 1) return 1;
    for (size_t i = 0; i < n; i++) {
        int r = open_alone(paths[i], &when);
        if (r == 1) return 1;
        if (r == 0 && marker == 2 && now - when < 10 * 60 * 1000) return 1;
    }
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
        /* The error FindNextFileW stopped with, read before any other call
         * can change it: anything but ERROR_NO_MORE_FILES means the folder
         * was not read to the end. */
        DWORD werr = ERROR_NO_MORE_FILES;
        int bad_name = 0;
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            char *name = to_utf8(fd.cFileName);
            if (!name) {
                /* Reported against the folder, so the scan treats it as not
                 * read in full and marks nothing in it deleted. */
                if (err_cb && !bad_name) err_cb(ctx, pattern, "a name in this folder is not valid Unicode and cannot be read");
                bad_name = 1;
                continue;
            }
            char *child_rel = malloc(rlen + strlen(name) + 2);
            if (!child_rel) { free(name); rc = -1; stop = 1; break; }
            sprintf(child_rel, rlen ? "%s/%s" : "%s%s", rel, name);
            bd_stat_t st;
            fill_from_attrs(fd.dwFileAttributes, fd.dwReserved0, fd.nFileSizeHigh, fd.nFileSizeLow, fd.ftLastWriteTime, fd.ftCreationTime, &st);
            int r = cb(ctx, child_rel, &st);
            if (r == BD_WALK_SKIP) { /* leave the folder out */ }
            else if (r != 0) stop = 1;
            else if (st.is_dir && stack_push(&stack, child_rel) != 0) { rc = -1; stop = 1; }
            free(child_rel);
            free(name);
        } while (!stop && (FindNextFileW(h, &fd) || ((werr = GetLastError()), 0)));
        FindClose(h);
        if (!stop && werr != ERROR_NO_MORE_FILES && err_cb) {
            char msg[80];
            snprintf(msg, sizeof(msg), "the folder could not be read to the end (error %lu)", (unsigned long)werr);
            err_cb(ctx, pattern, msg);
        }
        free(pattern);
        free(rel);
    }
    while (stack.len > 0) free(stack.items[--stack.len]);
    free(stack.items);
    free(base);
    return rc;
}

char *bd_fold_case(const char *s)
{
    int wn = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (wn <= 0) return NULL;
    wchar_t *w = malloc((size_t)wn * 2 * sizeof(wchar_t));
    if (!w) return NULL;
    wchar_t *up = w + wn;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, wn);
    /* Without LCMAP_LINGUISTIC_CASING this is the file system's mapping,
     * one UTF-16 unit for one, so the length stays the same. */
    if (LCMapStringW(LOCALE_INVARIANT, LCMAP_UPPERCASE, w, wn, up, wn) == 0) { free(w); return NULL; }
    int n = WideCharToMultiByte(CP_UTF8, 0, up, -1, NULL, 0, NULL, NULL);
    char *r = n > 0 ? malloc((size_t)n) : NULL;
    if (r) WideCharToMultiByte(CP_UTF8, 0, up, -1, r, n, NULL, NULL);
    free(w);
    return r;
}

int bd_random_bytes(void *buf, size_t n)
{
    return BCRYPT_SUCCESS(BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG)) ? 0 : -1;
}

void bd_secret_lock(void *p, size_t n)
{
    VirtualLock(p, n);
}

void bd_secret_unlock(void *p, size_t n)
{
    VirtualUnlock(p, n);
}

void bd_no_crash_dumps_of_memory(void)
{
    WerSetFlags(WER_FAULT_REPORTING_FLAG_NOHEAP);
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
    wchar_t *w = unprefix_unc(to_wide(path));
    if (!w) return -1;
    ULARGE_INTEGER avail, total, freeb;
    BOOL ok = GetDiskFreeSpaceExW(w, &avail, &total, &freeb);
    free(w);
    if (!ok) return -1;
    *total_bytes = (int64_t)total.QuadPart;
    *free_bytes = (int64_t)avail.QuadPart;
    return 0;
}

/* ---- Compression -------------------------------------------------------- */

int bd_fs_can_compress(const char *path)
{
    wchar_t *w = unprefix_unc(to_wide(path)), vol[MAX_PATH + 8];
    if (!w) return 0;
    BOOL ok = GetVolumePathNameW(w, vol, MAX_PATH + 8);
    free(w);
    DWORD flags = 0;
    if (!ok || !GetVolumeInformationW(vol, NULL, 0, NULL, NULL, &flags, NULL, 0)) return 0;
    return (flags & FILE_FILE_COMPRESSION) != 0;
}

int bd_fs_name(const char *path, char *out, size_t cap)
{
    if (cap) out[0] = '\0';
    wchar_t *w = unprefix_unc(to_wide(path)), vol[MAX_PATH + 8], fs[MAX_PATH + 1];
    if (!w) return -1;
    BOOL ok = GetVolumePathNameW(w, vol, MAX_PATH + 8);
    free(w);
    if (!ok || !GetVolumeInformationW(vol, NULL, 0, NULL, NULL, NULL, fs, MAX_PATH + 1)) return -1;
    char *u = to_utf8(fs);
    if (!u) return -1;
    int rc = strlen(u) < cap ? 0 : -1;
    if (rc == 0) strcpy(out, u);
    free(u);
    return rc;
}

int bd_fs_compressed(const char *path)
{
    wchar_t *w = to_wide(path);
    if (!w) return -1;
    DWORD attrs = GetFileAttributesW(w);
    free(w);
    if (attrs == INVALID_FILE_ATTRIBUTES) return -1;
    return (attrs & FILE_ATTRIBUTE_COMPRESSED) != 0;
}

int bd_fs_set_compressed(const char *path, int on)
{
    wchar_t *w = to_wide(path);
    if (!w) return -1;
    /* A read-only file cannot be opened for writing: lift the flag while
     * its compression changes, then put it back. */
    DWORD attrs = GetFileAttributesW(w);
    int ro = attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY) && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
    if (ro) SetFileAttributesW(w, attrs & ~FILE_ATTRIBUTE_READONLY);
    HANDLE h = CreateFileW(w, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    int rc = -1;
    if (h != INVALID_HANDLE_VALUE) {
        USHORT format = on ? COMPRESSION_FORMAT_DEFAULT : COMPRESSION_FORMAT_NONE;
        DWORD got = 0;
        if (DeviceIoControl(h, FSCTL_SET_COMPRESSION, &format, sizeof(format), NULL, 0, &got, NULL)) rc = 0;
        CloseHandle(h);
    }
    if (ro) SetFileAttributesW(w, attrs);
    free(w);
    return rc;
}

int64_t bd_fs_size_on_disk(const char *path)
{
    wchar_t *w = to_wide(path);
    if (!w) return -1;
    DWORD hi = 0, lo = GetCompressedFileSizeW(w, &hi);
    DWORD err = lo == INVALID_FILE_SIZE ? GetLastError() : NO_ERROR;
    free(w);
    if (err != NO_ERROR) return -1;
    return (int64_t)(((uint64_t)hi << 32) | lo);
}

void bd_sleep_ms(int ms)
{
    Sleep((DWORD)ms);
}

/* ---- Threads ------------------------------------------------------------ */

struct bd_thread { HANDLE h; bd_thread_fn fn; void *arg; };
struct bd_mutex { CRITICAL_SECTION cs; };
struct bd_cond { CONDITION_VARIABLE cv; };

static unsigned __stdcall thread_main(void *p)
{
    bd_thread *t = p;
    t->fn(t->arg);
    return 0;
}

bd_thread *bd_thread_start(bd_thread_fn fn, void *arg)
{
    bd_thread *t = malloc(sizeof(*t));
    if (!t) return NULL;
    t->fn = fn;
    t->arg = arg;
    /* _beginthreadex, not CreateThread, so the C runtime sets the thread up. */
    uintptr_t h = _beginthreadex(NULL, 0, thread_main, t, 0, NULL);
    if (!h) { free(t); return NULL; }
    t->h = (HANDLE)h;
    return t;
}

void bd_thread_join(bd_thread *t)
{
    if (!t) return;
    WaitForSingleObject(t->h, INFINITE);
    CloseHandle(t->h);
    free(t);
}

bd_mutex *bd_mutex_new(void)
{
    bd_mutex *m = malloc(sizeof(*m));
    if (m) InitializeCriticalSection(&m->cs);
    return m;
}

void bd_mutex_free(bd_mutex *m)
{
    if (!m) return;
    DeleteCriticalSection(&m->cs);
    free(m);
}

void bd_mutex_lock(bd_mutex *m) { EnterCriticalSection(&m->cs); }
void bd_mutex_unlock(bd_mutex *m) { LeaveCriticalSection(&m->cs); }

bd_cond *bd_cond_new(void)
{
    bd_cond *c = malloc(sizeof(*c));
    if (c) InitializeConditionVariable(&c->cv);
    return c;
}

void bd_cond_free(bd_cond *c)
{
    free(c); /* condition variables need no cleanup */
}

void bd_cond_wait(bd_cond *c, bd_mutex *m) { SleepConditionVariableCS(&c->cv, &m->cs, INFINITE); }
void bd_cond_signal(bd_cond *c) { WakeConditionVariable(&c->cv); }
void bd_cond_broadcast(bd_cond *c) { WakeAllConditionVariable(&c->cv); }

unsigned bd_drive_letters(void)
{
    DWORD all = GetLogicalDrives();
    unsigned out = 0;
    for (int i = 2; i < 26; i++) {
        if (!(all & (1u << i))) continue;
        wchar_t r[4] = {(wchar_t)(L'A' + i), L':', L'\\', 0};
        UINT t = GetDriveTypeW(r);
        if (t == DRIVE_FIXED || t == DRIVE_REMOVABLE || t == DRIVE_RAMDISK) out |= 1u << i;
    }
    return out;
}

int bd_cpu_count(void)
{
    DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (n == 0) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        n = si.dwNumberOfProcessors;
    }
    return n > 0 ? (int)n : 1;
}

/* The catalog's mutex name: a hash of its full path, made the same however
 * the path was written (relative or not, short 8.3 names, case, / or \). */
static int catalog_mutex_name(const char *catalog_path, wchar_t *name, size_t cap)
{
    /* to_wide gives drive paths the \\?\ prefix and network paths the
     * \\?\UNC\ prefix, and leaves relative paths alone; GetFullPathNameW
     * keeps a prefix it is given. */
    wchar_t *w = to_wide(catalog_path);
    if (!w) return -1;
    DWORD n = GetFullPathNameW(w, 0, NULL, NULL);
    wchar_t *full = n ? malloc(sizeof(wchar_t) * n) : NULL;
    DWORD got = full ? GetFullPathNameW(w, n, full, NULL) : 0;
    free(w);
    if (!got || got >= n) { free(full); return -1; }
    DWORD ln = GetLongPathNameW(full, NULL, 0); /* only works when the file is there */
    wchar_t *lng = ln ? malloc(sizeof(wchar_t) * ln) : NULL;
    if (lng && (got = GetLongPathNameW(full, lng, ln)) > 0 && got < ln) { free(full); full = lng; }
    else free(lng);
    /* The name is of the path without its prefix, as 0.9.0 made it (it
     * gave network paths none), so that both keep each other out of the
     * same catalog: \\?\C:\x is C:\x, \\?\UNC\server\x is \\server\x. */
    wchar_t *p = full;
    if (_wcsnicmp(full, L"\\\\?\\UNC\\", 8) == 0) {
        p = full + 6;
        p[0] = L'\\';
    } else if (wcsncmp(full, L"\\\\?\\", 4) == 0) {
        p = full + 4;
    }
    CharUpperBuffW(p, (DWORD)wcslen(p));
    char *u = to_utf8(p);
    free(full);
    if (!u) return -1;
    uint8_t hash[16];
    blake3_hasher h;
    blake3_hasher_init(&h);
    blake3_hasher_update(&h, u, strlen(u));
    blake3_hasher_finalize(&h, hash, sizeof(hash));
    free(u);
    wchar_t hex[33];
    for (int i = 0; i < 16; i++) swprintf(hex + i * 2, 3, L"%02x", hash[i]);
    swprintf(name, cap, L"Local\\BRODALF-catalog-%ls", hex);
    return 0;
}

typedef struct { HANDLE mutex, file; } win_lock;

int bd_catalog_lock_take(const char *catalog_path, const char *lock_path, void **handle)
{
    *handle = NULL;
    wchar_t name[96];
    if (catalog_mutex_name(catalog_path, name, 96) != 0) return -1;
    /* Whoever holds a handle to the mutex has the catalog open: nobody else
     * opens it. Windows closes the handle when a process ends, so a crash
     * leaves nothing behind. Ownership is not used, so any thread may close. */
    HANDLE m = CreateMutexW(NULL, FALSE, name);
    DWORD err = GetLastError();
    if (!m) return err == ERROR_ACCESS_DENIED ? 1 : -1;
    if (err == ERROR_ALREADY_EXISTS) { CloseHandle(m); return 1; }
    /* The lock file, held open without sharing and deleted on close, keeps
     * out a BRODALF on another computer when the catalog is on a share. */
    wchar_t *wl = to_wide(lock_path);
    if (!wl) { CloseHandle(m); return -1; }
    HANDLE f = CreateFileW(wl, GENERIC_WRITE | DELETE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    err = GetLastError();
    free(wl);
    if (f == INVALID_HANDLE_VALUE) {
        CloseHandle(m);
        return err == ERROR_SHARING_VIOLATION || err == ERROR_LOCK_VIOLATION ? 2 : -1;
    }
    win_lock *l = malloc(sizeof(*l));
    if (!l) { CloseHandle(f); CloseHandle(m); return -1; }
    l->mutex = m;
    l->file = f;
    *handle = l;
    return 0;
}

void bd_catalog_lock_release(void *handle, const char *lock_path)
{
    (void)lock_path; /* deleted when its handle closes */
    win_lock *l = handle;
    if (!l) return;
    CloseHandle(l->file);
    CloseHandle(l->mutex);
    free(l);
}

#endif
