/* Copies of files that are in use, read from a Volume Shadow Copy.
 *
 * Windows can take a snapshot of a drive (a shadow copy) that no program
 * holds open. BRODALF takes one only for the drives holding files it could
 * not read, copies those files out of it, and lets Windows delete the
 * snapshot again. Taking a snapshot needs administrator rights, so the app
 * runs this in a separate, elevated copy of itself.
 *
 * The elevated helper trusts nothing the unelevated side can change after
 * the user said yes to Windows:
 *   - the list of files is checked against its BLAKE3 hash, which is on the
 *     helper's command line (fixed when Windows asked for permission), and
 *     read once;
 *   - every file must be inside one of the protected folders named in that
 *     list, written as a plain "X:\..." path with no "." or ".." parts and
 *     no ":" streams;
 *   - a file whose path leads somewhere else through a link or junction is
 *     refused (the path Windows opened must be the one asked for);
 *   - the copies go into a new folder the helper makes itself, which only
 *     administrators can write to and the user can read and delete, held
 *     open so it cannot be swapped for a link while the helper works.
 *
 * The VSS interfaces are C++ in the Windows SDK, so the two used here are
 * declared by hand, in the order of their vtables, and vssapi.dll is loaded
 * when needed. */
#include "brodalf.h"
#include "internal.h"

#include "blake3.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LIST_MAGIC "BRODALF shadow-copy list 1\n"
#define LIST_MAX_BYTES (16 * 1024 * 1024)

static void hash_hex(const void *data, size_t n, char out[65])
{
    uint8_t h[32];
    blake3_hasher b;
    blake3_hasher_init(&b);
    blake3_hasher_update(&b, data, n);
    blake3_hasher_finalize(&b, h, sizeof(h));
    for (int i = 0; i < 32; i++) snprintf(out + i * 2, 3, "%02x", h[i]);
}

/* "X:\a\b" with '\' separators and ASCII upper case, no trailing separator
 * ("X:" for a whole drive). -1 for anything else: relative, UNC or device
 * paths, ":" after the drive, empty, "." or ".." parts (or parts that are
 * only dots and spaces, which Windows shortens), control characters. */
static int norm_path(const char *in, char *out, size_t cap)
{
    size_t n = strlen(in);
    if (n < 2 || n + 1 > cap || !isalpha((unsigned char)in[0]) || in[1] != ':') return -1;
    if (n > 2 && in[2] != '\\' && in[2] != '/') return -1;
    out[0] = (char)toupper((unsigned char)in[0]);
    out[1] = ':';
    size_t o = 2, i = 2;
    while (i < n) {
        while (i < n && (in[i] == '\\' || in[i] == '/')) i++; /* the separator before a part */
        if (i >= n) break;
        size_t start = i;
        int only_dots = 1;
        while (i < n && in[i] != '\\' && in[i] != '/') {
            unsigned char c = (unsigned char)in[i];
            if (c < 32 || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') return -1;
            if (c != '.' && c != ' ') only_dots = 0;
            i++;
        }
        if (only_dots) return -1;
        out[o++] = '\\';
        for (size_t k = start; k < i; k++) out[o++] = (char)toupper((unsigned char)in[k]);
    }
    out[o] = '\0';
    return 0;
}

int bd_shadow_path_allowed(const char *path, const char *const *folders, int n_folders)
{
    char p[4096], f[4096];
    if (!path || norm_path(path, p, sizeof(p)) != 0) return 0;
    for (int i = 0; i < n_folders; i++) {
        if (!folders[i] || norm_path(folders[i], f, sizeof(f)) != 0) continue;
        size_t fl = strlen(f);
        if (strncmp(p, f, fl) == 0 && p[fl] == '\\' && p[fl + 1]) return 1;
    }
    return 0;
}

static int has_newline(const char *s)
{
    return s && (strchr(s, '\n') || strchr(s, '\r'));
}

char *bd_shadow_list_format(const char *user_sid, const char *const *folders, int n_folders, const char *const *files,
                            int n_files, char hash_out[65])
{
    size_t len = strlen(LIST_MAGIC) + 1;
    if (!user_sid || has_newline(user_sid)) return NULL;
    len += strlen(user_sid) + 8;
    for (int i = 0; i < n_folders; i++) {
        if (has_newline(folders[i])) return NULL;
        len += strlen(folders[i]) + 9;
    }
    for (int i = 0; i < n_files; i++) {
        if (has_newline(files[i])) return NULL;
        len += strlen(files[i]) + 7;
    }
    char *t = malloc(len);
    if (!t) return NULL;
    size_t o = (size_t)snprintf(t, len, "%suser\t%s\n", LIST_MAGIC, user_sid);
    for (int i = 0; i < n_folders; i++) o += (size_t)snprintf(t + o, len - o, "folder\t%s\n", folders[i]);
    for (int i = 0; i < n_files; i++) o += (size_t)snprintf(t + o, len - o, "file\t%s\n", files[i]);
    hash_hex(t, o, hash_out);
    return t;
}

static int push(char ***arr, int *n, const char *s, size_t len)
{
    char **g = realloc(*arr, sizeof(char *) * (size_t)(*n + 1));
    if (!g) return -1;
    *arr = g;
    char *c = malloc(len + 1);
    if (!c) return -1;
    memcpy(c, s, len);
    c[len] = '\0';
    g[(*n)++] = c;
    return 0;
}

int bd_shadow_list_parse(const char *text, size_t len, const char *expected_hash, bd_shadow_list *out)
{
    memset(out, 0, sizeof(*out));
    char hash[65];
    hash_hex(text, len, hash);
    if (!expected_hash || strlen(expected_hash) != 64) return -2;
    int diff = 0;
    for (int i = 0; i < 64; i++) diff |= tolower((unsigned char)expected_hash[i]) ^ hash[i];
    if (diff) return -2;
    size_t ml = strlen(LIST_MAGIC);
    if (len < ml || memcmp(text, LIST_MAGIC, ml) != 0) return -1;
    size_t i = ml;
    while (i < len) {
        size_t e = i;
        while (e < len && text[e] != '\n') e++;
        if (e == len) break; /* every line ends with a newline; a cut-off one is ignored */
        const char *line = text + i;
        size_t n = e - i;
        int rc = 0;
        if (n > 5 && memcmp(line, "user\t", 5) == 0 && !out->user_sid) {
            char **one = NULL;
            int k = 0;
            rc = push(&one, &k, line + 5, n - 5);
            if (rc == 0) out->user_sid = one[0];
            free(one);
        } else if (n > 7 && memcmp(line, "folder\t", 7) == 0) {
            rc = push(&out->folders, &out->n_folders, line + 7, n - 7);
        } else if (n > 5 && memcmp(line, "file\t", 5) == 0) {
            rc = push(&out->files, &out->n_files, line + 5, n - 5);
        }
        if (rc != 0) { bd_shadow_list_free(out); return -1; }
        i = e + 1;
    }
    if (!out->user_sid) { bd_shadow_list_free(out); return -1; }
    return 0;
}

void bd_shadow_list_free(bd_shadow_list *l)
{
    if (!l) return;
    free(l->user_sid);
    for (int i = 0; i < l->n_folders; i++) free(l->folders[i]);
    for (int i = 0; i < l->n_files; i++) free(l->files[i]);
    free(l->folders);
    free(l->files);
    memset(l, 0, sizeof(*l));
}

static char *read_all(const char *path, size_t *len)
{
    FILE *f = bd_fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 65536, n = 0;
    char *buf = malloc(cap);
    while (buf) {
        if (n == cap) {
            if (cap >= LIST_MAX_BYTES) { free(buf); buf = NULL; break; }
            char *g = realloc(buf, cap * 2);
            if (!g) { free(buf); buf = NULL; break; }
            buf = g;
            cap *= 2;
        }
        size_t got = fread(buf + n, 1, cap - n, f);
        n += got;
        if (got == 0) break;
    }
    fclose(f);
    *len = n;
    return buf;
}

static int shadow_copy(const char *const *paths, int n, const char *dest_dir, const char *reader_sid, int *ok, char *err,
                       size_t err_len, const char *result_path);

int bd_shadow_copy_files(const char *const *paths, int n, const char *dest_dir, int *ok, char *err, size_t err_len)
{
    return shadow_copy(paths, n, dest_dir, NULL, ok, err, err_len, NULL);
}

int bd_shadow_helper(const char *out_dir, const char *list_path, const char *list_hash, char *err, size_t err_len)
{
    if (err && err_len) err[0] = '\0';
    size_t len = 0;
    char *text = read_all(list_path, &len); /* read once; only this copy is used */
    if (!text) {
        if (err && err_len) snprintf(err, err_len, "cannot read the list of files");
        return -1;
    }
    bd_shadow_list l;
    int rc = bd_shadow_list_parse(text, len, list_hash, &l);
    free(text);
    if (rc != 0) {
        if (err && err_len)
            snprintf(err, err_len, rc == -2 ? "the list of files was changed after Windows asked for permission; nothing was copied"
                                            : "the list of files is damaged");
        return -1;
    }
    const char **allowed = calloc((size_t)(l.n_files ? l.n_files : 1), sizeof(char *));
    int *ok = calloc((size_t)(l.n_files ? l.n_files : 1), sizeof(int));
    int got = -1;
    if (allowed && ok) {
        /* A path outside the protected folders is refused; it stays in the
         * list (as NULL) so the numbering of the copies does not change. */
        int refused = 0;
        for (int i = 0; i < l.n_files; i++) {
            if (bd_shadow_path_allowed(l.files[i], (const char *const *)l.folders, l.n_folders)) allowed[i] = l.files[i];
            else { refused++; bd_applog("shadow copy helper: refused %s (not inside a protected folder)", l.files[i]); }
        }
        char *result = bd_sprintf("%s%cresult.txt", out_dir, BD_SEP);
        got = result ? shadow_copy(allowed, l.n_files, out_dir, l.user_sid, ok, err, err_len, result) : -1;
        free(result);
        if (refused) bd_applog("shadow copy helper: %d file(s) outside the protected folders refused", refused);
    } else if (err && err_len) {
        snprintf(err, err_len, "out of memory");
    }
    free(allowed);
    free(ok);
    bd_shadow_list_free(&l);
    return got;
}

#ifndef _WIN32

static int shadow_copy(const char *const *paths, int n, const char *dest_dir, const char *reader_sid, int *ok, char *err,
                       size_t err_len, const char *result_path)
{
    (void)paths; (void)dest_dir; (void)reader_sid; (void)result_path;
    for (int i = 0; ok && i < n; i++) ok[i] = 0;
    if (err && err_len) snprintf(err, err_len, "shadow copies are a Windows feature");
    return -1;
}

#else

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <aclapi.h>
#include <objbase.h>
#include <sddl.h>
#include <stdlib.h>

#define MAX_VOLUMES 16
#define VSS_CTX_BACKUP 0
#define VSS_BT_COPY 5
#define VSS_S_ASYNC_FINISHED ((HRESULT)0x0004230AL)

typedef GUID VSS_ID;
static const VSS_ID no_provider; /* the default provider */

typedef struct {
    VSS_ID m_SnapshotId;
    VSS_ID m_SnapshotSetId;
    LONG m_lSnapshotsCount;
    WCHAR *m_pwszSnapshotDeviceObject;
    WCHAR *m_pwszOriginalVolumeName;
    WCHAR *m_pwszOriginatingMachine;
    WCHAR *m_pwszServiceMachine;
    WCHAR *m_pwszExposedName;
    WCHAR *m_pwszExposedPath;
    VSS_ID m_ProviderId;
    LONG m_lSnapshotAttributes;
    LONGLONG m_tsCreationTimestamp;
    int m_eStatus;
} snapshot_prop;

typedef struct vss_async vss_async;
typedef struct {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(vss_async *, REFIID, void **);
    ULONG(STDMETHODCALLTYPE *AddRef)(vss_async *);
    ULONG(STDMETHODCALLTYPE *Release)(vss_async *);
    HRESULT(STDMETHODCALLTYPE *Cancel)(vss_async *);
    HRESULT(STDMETHODCALLTYPE *Wait)(vss_async *, DWORD);
    HRESULT(STDMETHODCALLTYPE *QueryStatus)(vss_async *, HRESULT *, INT *);
} vss_async_vtbl;
struct vss_async { const vss_async_vtbl *v; };

typedef struct vss vss;
typedef void *slot; /* methods BRODALF does not call */
typedef struct {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(vss *, REFIID, void **);
    ULONG(STDMETHODCALLTYPE *AddRef)(vss *);
    ULONG(STDMETHODCALLTYPE *Release)(vss *);
    slot GetWriterComponentsCount, GetWriterComponents;
    HRESULT(STDMETHODCALLTYPE *InitializeForBackup)(vss *, BSTR);
    HRESULT(STDMETHODCALLTYPE *SetBackupState)(vss *, unsigned char, unsigned char, int, unsigned char);
    slot InitializeForRestore, SetRestoreState;
    HRESULT(STDMETHODCALLTYPE *GatherWriterMetadata)(vss *, vss_async **);
    slot GetWriterMetadataCount, GetWriterMetadata, FreeWriterMetadata, AddComponent;
    HRESULT(STDMETHODCALLTYPE *PrepareForBackup)(vss *, vss_async **);
    slot AbortBackup, GatherWriterStatus, GetWriterStatusCount, FreeWriterStatus, GetWriterStatus, SetBackupSucceeded,
        SetBackupOptions, SetSelectedForRestore, SetRestoreOptions, SetAdditionalRestores, SetPreviousBackupStamp, SaveAsXML;
    HRESULT(STDMETHODCALLTYPE *BackupComplete)(vss *, vss_async **);
    slot AddAlternativeLocationMapping, AddRestoreSubcomponent, SetFileRestoreStatus, AddNewTarget, SetRangesFilePath,
        PreRestore, PostRestore;
    HRESULT(STDMETHODCALLTYPE *SetContext)(vss *, LONG);
    HRESULT(STDMETHODCALLTYPE *StartSnapshotSet)(vss *, VSS_ID *);
    HRESULT(STDMETHODCALLTYPE *AddToSnapshotSet)(vss *, WCHAR *, VSS_ID, VSS_ID *);
    HRESULT(STDMETHODCALLTYPE *DoSnapshotSet)(vss *, vss_async **);
    slot DeleteSnapshots, ImportSnapshots, BreakSnapshotSet;
    HRESULT(STDMETHODCALLTYPE *GetSnapshotProperties)(vss *, VSS_ID, snapshot_prop *);
} vss_vtbl;
struct vss { const vss_vtbl *v; };

typedef HRESULT(STDAPICALLTYPE *create_fn)(vss **);

static wchar_t *widen(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = malloc(sizeof(wchar_t) * (size_t)n);
    if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    for (wchar_t *p = w; *p; p++) if (*p == L'/') *p = L'\\';
    return w;
}

/* Run an asynchronous VSS step to the end. */
static HRESULT finish(HRESULT hr, vss_async *a)
{
    if (FAILED(hr) || !a) return FAILED(hr) ? hr : E_FAIL;
    HRESULT status = E_FAIL;
    hr = a->v->Wait(a, INFINITE);
    if (SUCCEEDED(hr)) hr = a->v->QueryStatus(a, &status, NULL);
    a->v->Release(a);
    if (FAILED(hr)) return hr;
    return status == VSS_S_ASYNC_FINISHED ? S_OK : (FAILED(status) ? status : E_FAIL);
}

static void say(char *err, size_t err_len, const char *what, HRESULT hr)
{
    if (!err || !err_len) return;
    if (hr == E_ACCESSDENIED)
        snprintf(err, err_len, "%s: Windows only lets an administrator take a shadow copy", what);
    else
        snprintf(err, err_len, "%s (Windows error 0x%08lx)", what, (unsigned long)hr);
}

/* "\\?\X:\a" or "\\?\GLOBALROOT\Device\..." without the prefix, for comparing
 * paths that Windows hands back. */
static const wchar_t *no_prefix(const wchar_t *p)
{
    if (wcsncmp(p, L"\\\\?\\GLOBALROOT", 14) == 0) return p + 14;
    if (wcsncmp(p, L"\\\\?\\", 4) == 0) return p + 4;
    return p;
}

/* 1 if the open handle h is the file or folder at expected (NT or DOS form,
 * as asked for), not something a link or junction led to. */
static int is_where_asked(HANDLE h, const wchar_t *expected, DWORD how)
{
    wchar_t got[MAX_PATH * 4];
    DWORD n = GetFinalPathNameByHandleW(h, got, MAX_PATH * 4, how | FILE_NAME_NORMALIZED);
    if (n == 0 || n >= MAX_PATH * 4) return 0;
    return _wcsicmp(no_prefix(got), no_prefix(expected)) == 0;
}

/* Make dest as a new folder only administrators can write to, which
 * reader_sid (the user who asked) can read and empty, and keep it open so
 * nobody can rename it or put a link in its place. A folder already there
 * is refused: it could be anyone's. */
static HANDLE make_private_dir(const wchar_t *dest, const char *reader_sid, char *err, size_t err_len)
{
    if (!reader_sid || !*reader_sid || strspn(reader_sid, "S-0123456789") != strlen(reader_sid)) {
        say(err, err_len, "the list does not say who asked", E_INVALIDARG);
        return INVALID_HANDLE_VALUE;
    }
    wchar_t sddl[256], *wsid = widen(reader_sid);
    if (!wsid) { say(err, err_len, "out of memory", E_OUTOFMEMORY); return INVALID_HANDLE_VALUE; }
    swprintf(sddl, 256, L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FRFXSD;;;%ls)", wsid);
    free(wsid);
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, FALSE};
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sa.lpSecurityDescriptor, NULL)) {
        say(err, err_len, "cannot set up a private folder for the copies", HRESULT_FROM_WIN32(GetLastError()));
        return INVALID_HANDLE_VALUE;
    }
    BOOL made = CreateDirectoryW(dest, &sa);
    DWORD e = GetLastError();
    LocalFree(sa.lpSecurityDescriptor);
    if (!made) {
        say(err, err_len, e == ERROR_ALREADY_EXISTS ? "the folder for the copies is already there; nothing was copied"
                                                    : "cannot make the folder for the copies",
            HRESULT_FROM_WIN32(e));
        return INVALID_HANDLE_VALUE;
    }
    /* No FILE_SHARE_DELETE: while this is open the folder cannot be renamed
     * or removed. Reparse points are opened, not followed, and refused. */
    HANDLE h = CreateFileW(dest, FILE_LIST_DIRECTORY | READ_CONTROL | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        say(err, err_len, "cannot open the folder for the copies", HRESULT_FROM_WIN32(GetLastError()));
        return INVALID_HANDLE_VALUE;
    }
    BY_HANDLE_FILE_INFORMATION fi;
    wchar_t full[MAX_PATH * 4], lng[MAX_PATH * 4];
    DWORD fl = GetFullPathNameW(dest, MAX_PATH * 4, full, NULL);
    /* %TEMP% often has short 8.3 names; Windows answers with long ones. */
    DWORD ll = fl > 0 && fl < MAX_PATH * 4 ? GetLongPathNameW(full, lng, MAX_PATH * 4) : 0;
    if (ll > 0 && ll < MAX_PATH * 4) wcscpy(full, lng);
    int good = GetFileInformationByHandle(h, &fi) && (fi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
               !(fi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && fl > 0 && fl < MAX_PATH * 4 &&
               is_where_asked(h, full, VOLUME_NAME_DOS);
    /* And it is the folder made here: its owner is the one this process gives
     * what it creates (administrators), not the user. */
    if (good) {
        PSID owner = NULL;
        PSECURITY_DESCRIPTOR sd = NULL;
        HANDLE tok = NULL;
        unsigned char buf[256];
        DWORD n = 0;
        good = GetSecurityInfo(h, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, NULL, NULL, NULL, &sd) == ERROR_SUCCESS &&
               OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok) &&
               GetTokenInformation(tok, TokenOwner, buf, sizeof(buf), &n) && owner &&
               EqualSid(owner, ((TOKEN_OWNER *)buf)->Owner);
        if (tok) CloseHandle(tok);
        if (sd) LocalFree(sd);
    }
    if (!good) {
        CloseHandle(h);
        say(err, err_len, "the folder for the copies is not the one BRODALF made; nothing was copied", E_ACCESSDENIED);
        return INVALID_HANDLE_VALUE;
    }
    return h;
}

/* Copy one file out of the snapshot through handles, refusing a path that
 * a link or junction leads elsewhere, and never replacing a file. */
static int copy_out(const wchar_t *from, const wchar_t *to)
{
    HANDLE in = CreateFileW(from, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                            FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (in == INVALID_HANDLE_VALUE) return 0;
    BY_HANDLE_FILE_INFORMATION fi;
    if (!GetFileInformationByHandle(in, &fi) || (fi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        !is_where_asked(in, from, VOLUME_NAME_NT)) {
        CloseHandle(in);
        return 0;
    }
    HANDLE out = CreateFileW(to, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out == INVALID_HANDLE_VALUE) { CloseHandle(in); return 0; }
    enum { CHUNK = 1 << 20 };
    char *buf = malloc(CHUNK);
    int ok = buf != NULL;
    while (ok) {
        DWORD got = 0, put = 0;
        if (!ReadFile(in, buf, CHUNK, &got, NULL)) { ok = 0; break; } /* never keep a short copy */
        if (got == 0) break;
        if (!WriteFile(out, buf, got, &put, NULL) || put != got) ok = 0;
    }
    free(buf);
    if (!FlushFileBuffers(out)) ok = 0;
    CloseHandle(out);
    CloseHandle(in);
    if (!ok) DeleteFileW(to);
    return ok;
}

static void write_result(const char *result_path, int n, const int *ok, const char *const *paths, const char *err)
{
    if (!result_path) return;
    FILE *out = bd_fopen(result_path, "wb");
    if (!out) return;
    for (int i = 0; i < n; i++) fprintf(out, "%s\n", ok[i] ? "ok" : paths[i] ? "failed" : "refused");
    if (err && err[0]) fprintf(out, "error: %s\n", err);
    fclose(out);
}

/* paths[i] may be NULL (refused by the caller): it is skipped. With
 * reader_sid, dest_dir is made new and private (see make_private_dir) and
 * the result file is written into it while it is still held. */
static int shadow_copy(const char *const *paths, int n, const char *dest_dir, const char *reader_sid, int *ok, char *err,
                       size_t err_len, const char *result_path)
{
    if (err && err_len) err[0] = '\0';
    for (int i = 0; i < n; i++) ok[i] = 0;
    HANDLE held = INVALID_HANDLE_VALUE;
    wchar_t *wdest = widen(dest_dir);
    if (!wdest) { say(err, err_len, "out of memory", E_OUTOFMEMORY); write_result(result_path, n, ok, paths, err); return -1; }
    if (reader_sid) {
        held = make_private_dir(wdest, reader_sid, err, err_len);
        if (held == INVALID_HANDLE_VALUE) { free(wdest); return -1; } /* no result file: the folder may not be ours */
    }
    if (n <= 0) {
        write_result(result_path, n, ok, paths, err);
        if (held != INVALID_HANDLE_VALUE) CloseHandle(held);
        free(wdest);
        return 0;
    }

    wchar_t **wpaths = calloc((size_t)n, sizeof(wchar_t *));
    wchar_t **wvol = calloc((size_t)n, sizeof(wchar_t *)); /* each path's volume, e.g. "C:\" */
    wchar_t *volumes[MAX_VOLUMES];
    VSS_ID ids[MAX_VOLUMES];
    snapshot_prop props[MAX_VOLUMES];
    int nvol = 0, copied = 0, have_props = 0;
    memset(props, 0, sizeof(props));
    vss *b = NULL;
    HMODULE lib = NULL;
    HRESULT hr = S_OK;
    int com = 0;
    if (!wpaths || !wvol) { say(err, err_len, "out of memory", E_OUTOFMEMORY); goto done; }

    for (int i = 0; i < n; i++) {
        wchar_t vol[MAX_PATH + 1];
        if (!paths[i]) continue;
        wpaths[i] = widen(paths[i]);
        if (!wpaths[i] || !GetVolumePathNameW(wpaths[i], vol, MAX_PATH + 1)) continue;
        size_t vl = wcslen(vol);
        if (vl < 3 || vol[1] != L':' || _wcsnicmp(wpaths[i], vol, vl) != 0) continue; /* only "X:\..." on a local volume */
        int k = 0;
        while (k < nvol && _wcsicmp(volumes[k], vol) != 0) k++;
        if (k == nvol) {
            if (nvol == MAX_VOLUMES) continue;
            volumes[nvol] = _wcsdup(vol);
            if (!volumes[nvol]) continue;
            nvol++;
        }
        wvol[i] = volumes[k];
    }
    if (!nvol) { say(err, err_len, "none of the files is on a local drive", E_INVALIDARG); goto done; }

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    com = SUCCEEDED(hr);
    CoInitializeSecurity(NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_PKT_PRIVACY, RPC_C_IMP_LEVEL_IMPERSONATE, NULL,
                         EOAC_DYNAMIC_CLOAKING, NULL);
    lib = LoadLibraryExW(L"vssapi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    create_fn create = lib ? (create_fn)(void (*)(void))GetProcAddress(lib, "CreateVssBackupComponentsInternal") : NULL;
    if (!create) { say(err, err_len, "the Volume Shadow Copy service is not available", E_NOTIMPL); goto done; }

    vss_async *a = NULL;
    VSS_ID set_id;
    if (FAILED(hr = create(&b)) || !b) { say(err, err_len, "cannot start a shadow copy", hr); b = NULL; goto done; }
    if (FAILED(hr = b->v->InitializeForBackup(b, NULL)) || FAILED(hr = b->v->SetContext(b, VSS_CTX_BACKUP)) ||
        FAILED(hr = b->v->SetBackupState(b, 0, 0, VSS_BT_COPY, 0))) {
        say(err, err_len, "cannot prepare a shadow copy", hr);
        goto done;
    }
    a = NULL;
    if (FAILED(hr = finish(b->v->GatherWriterMetadata(b, &a), a))) { say(err, err_len, "cannot ask programs to get ready", hr); goto done; }
    if (FAILED(hr = b->v->StartSnapshotSet(b, &set_id))) { say(err, err_len, "cannot start a shadow copy", hr); goto done; }
    for (int k = 0; k < nvol; k++) {
        if (FAILED(hr = b->v->AddToSnapshotSet(b, volumes[k], no_provider, &ids[k]))) {
            say(err, err_len, "this drive cannot have a shadow copy", hr);
            goto done;
        }
    }
    a = NULL;
    if (FAILED(hr = finish(b->v->PrepareForBackup(b, &a), a))) { say(err, err_len, "cannot prepare a shadow copy", hr); goto done; }
    a = NULL;
    if (FAILED(hr = finish(b->v->DoSnapshotSet(b, &a), a))) { say(err, err_len, "Windows could not take the shadow copy", hr); goto done; }
    for (int k = 0; k < nvol; k++) {
        if (FAILED(hr = b->v->GetSnapshotProperties(b, ids[k], &props[k]))) { say(err, err_len, "cannot find the shadow copy", hr); goto done; }
        have_props = k + 1;
    }

    if (held == INVALID_HANDLE_VALUE) CreateDirectoryW(wdest, NULL);
    for (int i = 0; i < n; i++) {
        if (!wvol[i]) continue;
        int k = 0;
        while (k < nvol && volumes[k] != wvol[i]) k++;
        const wchar_t *dev = props[k].m_pwszSnapshotDeviceObject;
        const wchar_t *inside = wpaths[i] + wcslen(wvol[i]); /* after "C:\" */
        if (!dev) continue;
        size_t len = wcslen(dev) + wcslen(inside) + 2;
        wchar_t *from = malloc(sizeof(wchar_t) * len);
        wchar_t to[MAX_PATH * 2];
        if (!from) continue;
        swprintf(from, len, L"%ls\\%ls", dev, inside);
        swprintf(to, MAX_PATH * 2, L"%ls\\%d.bin", wdest, i + 1);
        if (copy_out(from, to)) { ok[i] = 1; copied++; }
        free(from);
    }
    a = NULL;
    finish(b->v->BackupComplete(b, &a), a);

done:
    for (int k = 0; k < have_props; k++) {
        CoTaskMemFree(props[k].m_pwszSnapshotDeviceObject);
        CoTaskMemFree(props[k].m_pwszOriginalVolumeName);
        CoTaskMemFree(props[k].m_pwszOriginatingMachine);
        CoTaskMemFree(props[k].m_pwszServiceMachine);
        CoTaskMemFree(props[k].m_pwszExposedName);
        CoTaskMemFree(props[k].m_pwszExposedPath);
    }
    if (b) b->v->Release(b); /* a backup-context snapshot is deleted here */
    if (lib) FreeLibrary(lib);
    if (com) CoUninitialize();
    for (int k = 0; k < nvol; k++) free(volumes[k]);
    for (int i = 0; wpaths && i < n; i++) free(wpaths[i]);
    free(wpaths);
    free(wvol);
    if (copied == 0 && err && err_len && !err[0]) snprintf(err, err_len, "no file could be copied from the shadow copy");
    write_result(result_path, n, ok, paths, err);
    if (held != INVALID_HANDLE_VALUE) CloseHandle(held);
    free(wdest);
    return copied > 0 || !err || !err[0] ? copied : -1;
}

#endif
