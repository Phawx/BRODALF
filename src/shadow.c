/* Copies of files that are in use, read from a Volume Shadow Copy.
 *
 * Windows can take a snapshot of a drive (a shadow copy) that no program
 * holds open. BRODALF takes one only for the drives holding files it could
 * not read, copies those files out of it, and lets Windows delete the
 * snapshot again. Taking a snapshot needs administrator rights, so the app
 * runs this in a separate, elevated copy of itself.
 *
 * The VSS interfaces are C++ in the Windows SDK, so the two used here are
 * declared by hand, in the order of their vtables, and vssapi.dll is loaded
 * when needed. */
#include "brodalf.h"

#include <stdio.h>
#include <string.h>

#ifndef _WIN32

int bd_shadow_copy_files(const char *const *paths, int n, const char *dest_dir, int *ok, char *err, size_t err_len)
{
    (void)paths; (void)dest_dir;
    for (int i = 0; ok && i < n; i++) ok[i] = 0;
    if (err && err_len) snprintf(err, err_len, "shadow copies are a Windows feature");
    return -1;
}

#else

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
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

int bd_shadow_copy_files(const char *const *paths, int n, const char *dest_dir, int *ok, char *err, size_t err_len)
{
    if (err && err_len) err[0] = '\0';
    for (int i = 0; i < n; i++) ok[i] = 0;
    if (n <= 0) return 0;

    wchar_t **wpaths = calloc((size_t)n, sizeof(wchar_t *));
    wchar_t **wvol = calloc((size_t)n, sizeof(wchar_t *)); /* each path's volume, e.g. "C:\" */
    wchar_t *volumes[MAX_VOLUMES];
    VSS_ID ids[MAX_VOLUMES];
    snapshot_prop props[MAX_VOLUMES];
    int nvol = 0, copied = 0, have_props = 0;
    memset(props, 0, sizeof(props));
    wchar_t *wdest = widen(dest_dir);
    vss *b = NULL;
    HMODULE lib = NULL;
    HRESULT hr = S_OK;
    int com = 0;
    if (!wpaths || !wvol || !wdest) { say(err, err_len, "out of memory", E_OUTOFMEMORY); goto done; }

    for (int i = 0; i < n; i++) {
        wchar_t vol[MAX_PATH + 1];
        wpaths[i] = widen(paths[i]);
        if (!wpaths[i] || !GetVolumePathNameW(wpaths[i], vol, MAX_PATH + 1)) continue;
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
    lib = LoadLibraryW(L"vssapi.dll");
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

    CreateDirectoryW(wdest, NULL);
    for (int i = 0; i < n; i++) {
        if (!wvol[i]) continue;
        int k = 0;
        while (k < nvol && volumes[k] != wvol[i]) k++;
        const wchar_t *dev = props[k].m_pwszSnapshotDeviceObject;
        const wchar_t *inside = wpaths[i] + wcslen(wvol[i]); /* after "C:\" */
        size_t len = wcslen(dev) + wcslen(inside) + 2;
        wchar_t *from = malloc(sizeof(wchar_t) * len);
        wchar_t to[MAX_PATH * 2];
        if (!from) continue;
        swprintf(from, len, L"%ls\\%ls", dev, inside);
        swprintf(to, MAX_PATH * 2, L"%ls\\%d.bin", wdest, i + 1);
        if (CopyFileW(from, to, FALSE)) { ok[i] = 1; copied++; }
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
    free(wdest);
    if (copied == 0 && err && err_len && !err[0]) snprintf(err, err_len, "no file could be copied from the shadow copy");
    return copied > 0 || !err || !err[0] ? copied : -1;
}

#endif
