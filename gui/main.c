/* BRODALF for Windows: the ghost-tree window.
 *
 * Every file and folder in the protected folders is shown. Files are greyed
 * out until a correct copy is on a drive that is plugged in right now. Long
 * jobs (scan, backup, check, restore, looking for drives) run one at a time
 * on a worker thread; the window stays responsive and shows progress. */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <dbt.h>
#include <shlobj.h>
#include <shellapi.h>
#include <stdarg.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "brodalf.h"
#include "resource.h"

#define APP_NAME L"BRODALF"
#define REG_KEY L"Software\\BRODALF"

enum {
    ID_TREE = 100,
    ID_LIST,
    ID_LOG,
    ID_STATUS,
    ID_DETAIL,
    ID_DRIVES,
    ID_BTN_ADD,
    ID_BTN_SCAN,
    ID_BTN_BACKUP,
    ID_BTN_CHECK,
    ID_BTN_RESTORE,
    ID_BTN_SECURITY,
    ID_BTN_DRIVES,
    ID_BTN_RISK,
    ID_MENU_SET_PASS = 910,
    ID_MENU_ENCRYPT_CATALOG,
    ID_MENU_LOCK,
    ID_MENU_DRIVE_BASE = 1000, /* + media id, for the drive popup menus */
    ID_MENU_OTHER = 900,
    ID_MENU_ONEDRIVE,
    ID_MENU_DROPBOX
};

enum {
    WM_APP_LOG = WM_APP + 1,  /* lParam: malloc'd wide string */
    WM_APP_PROGRESS,          /* lParam: malloc'd wide string */
    WM_APP_DONE,              /* lParam: job* */
    WM_APP_OPEN_URL           /* lParam: malloc'd wide string */
};

/* ---- Small helpers ------------------------------------------------------ */

static wchar_t *widen(const char *s)
{
    if (!s) s = "";
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = malloc(sizeof(wchar_t) * (size_t)(n > 0 ? n : 1));
    if (!w) return NULL;
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    else w[0] = 0;
    return w;
}

static char *narrow(const wchar_t *w)
{
    if (!w) w = L"";
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = malloc((size_t)(n > 0 ? n : 1));
    if (!s) return NULL;
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    else s[0] = 0;
    return s;
}

static char *xstrdup(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

static void format_bytes(int64_t b, wchar_t *out, size_t n)
{
    const wchar_t *units[] = {L"bytes", L"KB", L"MB", L"GB", L"TB"};
    double v = (double)b;
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; u++; }
    if (u == 0) swprintf(out, n, L"%lld bytes", (long long)b);
    else swprintf(out, n, L"%.1f %ls", v, units[u]);
}

static void format_time_ms(int64_t ms, wchar_t *out, size_t n)
{
    if (ms <= 0) { swprintf(out, n, L"never"); return; }
    ULONGLONG t = (ULONGLONG)ms * 10000ULL + 116444736000000000ULL;
    FILETIME ft = {(DWORD)t, (DWORD)(t >> 32)}, local;
    SYSTEMTIME st;
    FileTimeToLocalFileTime(&ft, &local);
    FileTimeToSystemTime(&local, &st);
    swprintf(out, n, L"%04d-%02d-%02d %02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
}

/* ---- Globals ------------------------------------------------------------ */

static HINSTANCE g_inst;
static HWND g_main, g_tree, g_list, g_log, g_status, g_detail, g_drives;
#define N_BUTTONS 8
static HWND g_buttons[N_BUTTONS];
static HFONT g_font, g_font_italic, g_font_strike, g_font_bold;
static int g_dpi = 96;
static bd_catalog *g_cat;
static wchar_t g_cat_path[MAX_PATH * 2];
static volatile LONG g_busy;

static int S(int px) { return MulDiv(px, g_dpi, 96); }

/* ---- Jobs --------------------------------------------------------------- */

typedef enum { JOB_DRIVES, JOB_SCAN, JOB_BACKUP, JOB_CHECK, JOB_RESTORE } job_kind;

typedef struct job {
    job_kind kind;
    int64_t media_id;   /* backup/check: a connected drive, or 0 with root */
    char *root;         /* backup to a drive given by folder */
    char *label;        /* set up a new drive with this name first */
    char *location;     /* and say where it is kept */
    unsigned flags;     /* for the new drive: BD_MEDIA_ENCRYPTED */
    int provider;       /* sign in to this cloud and add it first (bd_cloud_provider) */
    int64_t source_id;  /* restore */
    char *rel;          /* restore */
    char *dest;         /* restore */
    int problems;
    bd_status status;
    wchar_t summary[512];
    struct job *next;
} job;

static job *g_queue;

static void job_free(job *j)
{
    if (!j) return;
    free(j->root);
    free(j->label);
    free(j->location);
    free(j->rel);
    free(j->dest);
    free(j);
}

static void post_text(UINT msg, const wchar_t *text)
{
    wchar_t *copy = _wcsdup(text);
    if (copy && !PostMessageW(g_main, msg, 0, (LPARAM)copy)) free(copy);
}

static void job_log(void *ctx, const char *message)
{
    job *j = ctx;
    j->problems++;
    wchar_t *w = widen(message);
    if (w) { post_text(WM_APP_LOG, w); free(w); }
}

static void job_progress(void *ctx, const char *phase, int64_t files, int64_t bytes, const char *current)
{
    (void)ctx;
    wchar_t size[64], line[1024];
    format_bytes(bytes, size, 64);
    wchar_t *p = widen(phase), *c = widen(current);
    if (strcmp(phase, "scan") == 0) swprintf(line, 1024, L"Scanning: %lld files, %ls checksummed. %ls", (long long)files, size, c);
    else if (strcmp(phase, "backup") == 0) swprintf(line, 1024, L"Backing up: %lld files copied, %ls. %ls", (long long)files, size, c);
    else if (strcmp(phase, "restore") == 0) swprintf(line, 1024, L"Restoring: %lld files, %ls. %ls", (long long)files, size, c);
    else swprintf(line, 1024, L"Checking the drive: %lld copies. %ls", (long long)files, c);
    free(p);
    free(c);
    post_text(WM_APP_PROGRESS, line);
}

/* Is root a drive for this catalog? Connects it (with a quick check). */
static int try_connect(job *j, const char *root, int64_t *media_id)
{
    bd_check_stats cs;
    bd_status s = bd_media_connect(g_cat, root, media_id, &cs, job_log, j);
    return s == BD_OK;
}

static int is_brodalf_drive(const char *root)
{
    char path[1024];
    size_t n = strlen(root);
    snprintf(path, sizeof(path), "%s%sBRODALF\\%s\\BRODALF.media", root, (n && root[n - 1] == '\\') ? "" : "\\", bd_catalog_uuid(g_cat));
    wchar_t *w = widen(path);
    int ok = w && GetFileAttributesW(w) != INVALID_FILE_ATTRIBUTES;
    free(w);
    return ok;
}

typedef struct { char *roots[256]; int n; } root_list;

static int offline_root_cb(void *ctx, const bd_media_info *info)
{
    root_list *l = ctx;
    if (!info->connected && strcmp(info->kind, "drive") == 0 && info->last_root && info->last_root[0] && l->n < 256)
        l->roots[l->n++] = xstrdup(info->last_root);
    return 0;
}

typedef struct { int64_t ids[64]; char *labels[64]; int n; } cloud_list;

static int offline_cloud_cb(void *ctx, const bd_media_info *info)
{
    cloud_list *l = ctx;
    if (!info->connected && strcmp(info->kind, "drive") != 0 && l->n < 64) {
        l->ids[l->n] = info->media_id;
        l->labels[l->n++] = xstrdup(info->label);
    }
    return 0;
}

static char *bd_sprintf_gui(const char *fmt, const char *a, const char *b)
{
    size_t n = strlen(fmt) + strlen(a) + strlen(b) + 1;
    char *out = malloc(n);
    if (out) snprintf(out, n, fmt, a, b);
    return out;
}

static void run_drives(job *j)
{
    int found = 0;
    /* Drive letters first: a drive may come back under a different letter. */
    DWORD mask = GetLogicalDrives();
    for (int i = 2; i < 26; i++) { /* skip A: and B: */
        if (!(mask & (1u << i))) continue;
        char root[4] = {(char)('A' + i), ':', '\\', 0};
        wchar_t wroot[4] = {(wchar_t)(L'A' + i), L':', L'\\', 0};
        UINT type = GetDriveTypeW(wroot);
        if (type == DRIVE_NO_ROOT_DIR || type == DRIVE_UNKNOWN || type == DRIVE_CDROM) continue;
        int64_t id;
        if (is_brodalf_drive(root) && try_connect(j, root, &id)) found++;
    }
    /* Then folders and shares used as storage, where they were last seen. */
    root_list l;
    l.n = 0;
    bd_list_media(g_cat, offline_root_cb, &l);
    for (int i = 0; i < l.n; i++) {
        int64_t id;
        if (l.roots[i] && is_brodalf_drive(l.roots[i]) && try_connect(j, l.roots[i], &id)) found++;
        free(l.roots[i]);
    }
    /* Cloud accounts, with their saved sign-ins. */
    cloud_list c;
    c.n = 0;
    bd_list_media(g_cat, offline_cloud_cb, &c);
    int clouds = 0;
    for (int i = 0; i < c.n; i++) {
        bd_check_stats cs;
        if (bd_cloud_connect(g_cat, c.ids[i], &cs, job_log, j) == BD_OK) {
            clouds++;
        } else {
            char *msg = bd_sprintf_gui("%s: %s", c.labels[i], bd_catalog_error(g_cat));
            if (msg) { job_log(j, msg); free(msg); }
        }
        free(c.labels[i]);
    }
    if (clouds) swprintf(j->summary, 512, L"Found %d BRODALF drive(s) and connected %d cloud account(s).", found, clouds);
    else swprintf(j->summary, 512, found ? L"Found %d BRODALF drive(s)." : L"No BRODALF drives are plugged in.", found);
}

typedef struct { int64_t ids[256]; char *roots[256]; int n; } media_snapshot;

static int snapshot_cb(void *ctx, const bd_media_info *info)
{
    media_snapshot *s = ctx;
    if (info->connected && strcmp(info->kind, "drive") == 0 && s->n < 256) {
        s->ids[s->n] = info->media_id;
        s->roots[s->n] = xstrdup(info->last_root);
        s->n++;
    }
    return 0;
}

/* Drop connected drives whose BRODALF folder is gone (drive unplugged). */
static void forget_missing_drives(void)
{
    media_snapshot s;
    s.n = 0;
    bd_list_media(g_cat, snapshot_cb, &s);
    for (int i = 0; i < s.n; i++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s%sBRODALF\\%s\\BRODALF.media", s.roots[i] ? s.roots[i] : "",
                 (s.roots[i] && s.roots[i][0] && s.roots[i][strlen(s.roots[i]) - 1] == '\\') ? "" : "\\",
                 bd_catalog_uuid(g_cat));
        wchar_t *w = widen(path);
        if (!w || GetFileAttributesW(w) == INVALID_FILE_ATTRIBUTES) bd_media_disconnect(g_cat, s.ids[i]);
        free(w);
        free(s.roots[i]);
    }
}

static DWORD WINAPI worker(LPVOID arg)
{
    job *j = arg;
    bd_catalog_set_progress(g_cat, job_progress, j);
    bd_status s = BD_OK;
    wchar_t size[64];

    switch (j->kind) {
    case JOB_DRIVES:
        forget_missing_drives();
        run_drives(j);
        break;
    case JOB_SCAN: {
        bd_scan_stats st;
        s = bd_scan(g_cat, &st, job_log, j);
        format_bytes(st.bytes_hashed, size, 64);
        swprintf(j->summary, 512, L"Scan done: %lld files, %lld new, %lld changed, %lld deleted, %ls checksummed.",
                 (long long)st.files_seen, (long long)st.files_new, (long long)st.files_changed,
                 (long long)st.files_deleted, size);
        break;
    }
    case JOB_BACKUP: {
        int64_t id = j->media_id;
        if (j->provider) {
            bd_signin *si = NULL;
            const char *url;
            s = bd_cloud_signin_begin(g_cat, (bd_cloud_provider)j->provider, &si, &url);
            if (s == BD_OK) {
                wchar_t *w = widen(url);
                if (w) { post_text(WM_APP_OPEN_URL, w); free(w); }
                post_text(WM_APP_PROGRESS, L"Waiting for you to sign in with your browser...");
                s = bd_cloud_signin_finish(g_cat, si, 5 * 60 * 1000);
            }
            if (s == BD_OK) s = bd_cloud_add(g_cat, si, j->label, j->flags, &id);
            if (s == BD_OK) {
                wchar_t *acct = widen(bd_cloud_signin_account(si)), line[512];
                swprintf(line, 512, L"Signed in as %ls.", acct ? acct : L"");
                post_text(WM_APP_LOG, line);
                free(acct);
                j->provider = 0; /* a retry must not sign in again */
                free(j->label);
                j->label = NULL;
                j->media_id = id;
            }
            bd_cloud_signin_free(si);
        } else if (j->label) {
            s = bd_media_init(g_cat, j->root, j->label, j->flags, &id);
            if (s == BD_OK) {
                if (j->location && *j->location) bd_media_set_location(g_cat, id, j->location);
                free(j->label);
                j->label = NULL; /* a retry must not set it up again */
                j->media_id = id;
            }
        }
        else if (!id) s = try_connect(j, j->root, &id) ? BD_OK : BD_ERR_NOT_FOUND;
        if (s == BD_OK) {
            bd_backup_stats bs;
            s = bd_backup(g_cat, id, 0, &bs, job_log, j);
            if (s == BD_OK) s = bd_catalog_copy_to_media(g_cat, id);
            format_bytes(bs.bytes_copied, size, 64);
            swprintf(j->summary, 512, L"Backup done: %lld files copied (%ls), %lld already there, %lld failed, %lld older copies kept.",
                     (long long)bs.files_copied, size, (long long)bs.files_already_there, (long long)bs.files_failed,
                     (long long)bs.versions_moved);
        }
        break;
    }
    case JOB_CHECK: {
        bd_check_stats cs;
        s = bd_media_check(g_cat, j->media_id, 1, &cs, job_log, j);
        swprintf(j->summary, 512, L"Full check done: %lld copies, %lld good, %lld missing, %lld damaged.",
                 (long long)cs.copies, (long long)cs.ok, (long long)cs.missing, (long long)cs.bad);
        break;
    }
    case JOB_RESTORE: {
        bd_restore_stats rs;
        s = bd_restore(g_cat, j->source_id, j->rel, j->dest, &rs, job_log, j);
        format_bytes(rs.bytes_restored, size, 64);
        swprintf(j->summary, 512, L"Restore done: %lld files (%ls), %lld on drives that are not plugged in, %lld never backed up, %lld failed.",
                 (long long)rs.files_restored, size, (long long)rs.files_offline, (long long)rs.files_no_copy,
                 (long long)rs.files_failed);
        if (rs.files_need_passphrase) {
            size_t len = wcslen(j->summary);
            swprintf(j->summary + len, 512 - len, L" %lld need the passphrase.", (long long)rs.files_need_passphrase);
        }
        break;
    }
    }
    j->status = s;
    if (s != BD_OK) {
        wchar_t *e = widen(bd_catalog_error(g_cat));
        swprintf(j->summary, 512, L"Stopped: %ls", e ? e : L"unknown error");
        free(e);
    }
    if (bd_catalog_save_all(g_cat, job_log, j) != BD_OK) {
        wchar_t *e = widen(bd_catalog_error(g_cat));
        if (j->status == BD_OK) {
            j->status = BD_ERR_IO;
            swprintf(j->summary, 512, L"Could not save the catalog: %ls", e ? e : L"unknown error");
        } else {
            post_text(WM_APP_LOG, L"Could not save the catalog:");
            if (e) post_text(WM_APP_LOG, e);
        }
        free(e);
    }
    bd_catalog_set_progress(g_cat, NULL, NULL);
    PostMessageW(g_main, WM_APP_DONE, 0, (LPARAM)j);
    return 0;
}

static void set_busy(int busy)
{
    InterlockedExchange(&g_busy, busy);
    for (int i = 0; i < N_BUTTONS; i++) EnableWindow(g_buttons[i], !busy);
}

static void start_next_job(void)
{
    if (g_busy || !g_queue) return;
    job *j = g_queue;
    g_queue = j->next;
    j->next = NULL;
    set_busy(1);
    static const wchar_t *starting[] = {L"Looking for BRODALF drives...", L"Scanning your folders...",
                                        L"Backing up...", L"Checking every copy on the drive...", L"Restoring..."};
    SendMessageW(g_status, SB_SETTEXTW, 0, (LPARAM)starting[j->kind]);
    HANDLE h = CreateThread(NULL, 0, worker, j, 0, NULL);
    if (h) CloseHandle(h);
    else { set_busy(0); job_free(j); }
}

static void enqueue(job *j)
{
    /* Don't stack up drive scans; one pending is enough. */
    if (j->kind == JOB_DRIVES)
        for (job *q = g_queue; q; q = q->next)
            if (q->kind == JOB_DRIVES) { job_free(j); return; }
    job **tail = &g_queue;
    while (*tail) tail = &(*tail)->next;
    *tail = j;
    start_next_job();
}

static job *new_job(job_kind kind)
{
    job *j = calloc(1, sizeof(job));
    if (j) j->kind = kind;
    return j;
}

/* ---- Log pane ----------------------------------------------------------- */

static void log_append(const wchar_t *text)
{
    int len = GetWindowTextLengthW(g_log);
    if (len > 60000) { SendMessageW(g_log, EM_SETSEL, 0, 20000); SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)L""); len = GetWindowTextLengthW(g_log); }
    SendMessageW(g_log, EM_SETSEL, len, len);
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)text);
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
    char *u = narrow(text);
    if (u) { bd_applog("%s", u); free(u); }
}

/* ---- Errors ------------------------------------------------------------- */

/* Show an error, save a report file, and say how to post it as a GitHub
 * issue. Nothing is sent anywhere. */
static void report_error(HWND owner, const wchar_t *what)
{
    char *u = narrow(what);
    bd_applog("ERROR: %s", u ? u : "");
    char *path = bd_report_save(u);
    free(u);
    wchar_t *wpath = path ? widen(path) : NULL, *url = widen(bd_issues_url());
    wchar_t msg[4096];
    if (wpath)
        swprintf(msg, 4096,
                 L"%ls\n\nAn error report was saved to:\n%ls\n\n"
                 L"To let the BRODALF developers know, post it as a new issue at %ls. "
                 L"The report starts with step-by-step instructions.\n\nShow the report file now?",
                 what, wpath, url ? url : L"");
    else
        swprintf(msg, 4096, L"%ls\n\nThe error report could not be saved.", what);
    if (MessageBoxW(owner, msg, APP_NAME, MB_ICONERROR | (wpath ? MB_YESNO : MB_OK)) == IDYES) {
        wchar_t args[MAX_PATH * 2 + 16];
        swprintf(args, MAX_PATH * 2 + 16, L"/select,\"%ls\"", wpath);
        ShellExecuteW(owner, NULL, L"explorer.exe", args, NULL, SW_SHOWNORMAL);
    }
    free(path);
    free(wpath);
    free(url);
}

/* Last resort for a crash: record where it happened, then report it. */
static LONG WINAPI on_crash(EXCEPTION_POINTERS *ep)
{
    static volatile LONG once;
    if (InterlockedExchange(&once, 1)) return EXCEPTION_EXECUTE_HANDLER;
    void *addr = ep->ExceptionRecord->ExceptionAddress;
    HMODULE mod = NULL;
    wchar_t name[MAX_PATH] = L"?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)addr, &mod))
        GetModuleFileNameW(mod, name, MAX_PATH);
    const wchar_t *base = wcsrchr(name, L'\\');
    wchar_t what[512];
    swprintf(what, 512, L"BRODALF crashed (exception 0x%08lX in %ls at +0x%llx).",
             (unsigned long)ep->ExceptionRecord->ExceptionCode, base ? base + 1 : name,
             (unsigned long long)((uintptr_t)addr - (uintptr_t)mod));
    report_error(NULL, what);
    return EXCEPTION_EXECUTE_HANDLER;
}

/* ---- Tree --------------------------------------------------------------- */

typedef struct {
    int64_t source_id;
    int64_t node_id;   /* 0 for a source's top item */
    int is_dir;
    int loaded;
    bd_node_state state;
    char *rel;
} node_ref;

typedef struct {
    bd_node_info info;
    char *name, *rel, *label;
} child;

typedef struct { child *items; int n, cap; } child_list;

static int collect_child(void *ctx, const bd_node_info *info)
{
    child_list *l = ctx;
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 64;
        child *c = realloc(l->items, sizeof(child) * (size_t)cap);
        if (!c) return 1;
        l->items = c;
        l->cap = cap;
    }
    child *c = &l->items[l->n++];
    c->info = *info;
    c->name = xstrdup(info->name);
    c->rel = xstrdup(info->rel_path);
    c->label = xstrdup(info->offline_media_label);
    return 0;
}

static void item_text(const bd_node_info *n, const char *name, const char *label, wchar_t *out, size_t cap)
{
    wchar_t *wn = widen(name), *wl = widen(label);
    if (n->is_dir && n->state == BD_STATE_OFFLINE && label) {
        swprintf(out, cap, L"%ls   (0 of %lld available, copies on %ls)", wn, (long long)n->files_total, wl);
    } else if (n->is_dir) {
        swprintf(out, cap, L"%ls   (%lld of %lld available)", wn, (long long)n->files_available, (long long)n->files_total);
    } else if (n->state == BD_STATE_OFFLINE && label) {
        if (n->version_count > 1) swprintf(out, cap, L"%ls   v%d  on %ls", wn, n->version_no, wl);
        else swprintf(out, cap, L"%ls   on %ls", wn, wl);
    } else if (n->version_count > 1) {
        swprintf(out, cap, L"%ls   v%d", wn, n->version_no);
    } else {
        swprintf(out, cap, L"%ls", wn);
    }
    free(wn);
    free(wl);
}

static HTREEITEM insert_item(HTREEITEM parent, const wchar_t *text, node_ref *ref, int has_children)
{
    TVINSERTSTRUCTW tv;
    memset(&tv, 0, sizeof(tv));
    tv.hParent = parent;
    tv.hInsertAfter = TVI_LAST;
    tv.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_CHILDREN;
    tv.item.pszText = (LPWSTR)text;
    tv.item.lParam = (LPARAM)ref;
    tv.item.cChildren = has_children ? 1 : 0;
    return (HTREEITEM)SendMessageW(g_tree, TVM_INSERTITEMW, 0, (LPARAM)&tv);
}

static node_ref *item_ref(HTREEITEM h)
{
    TVITEMW it;
    memset(&it, 0, sizeof(it));
    it.mask = TVIF_PARAM;
    it.hItem = h;
    if (!h || !SendMessageW(g_tree, TVM_GETITEMW, 0, (LPARAM)&it)) return NULL;
    return (node_ref *)it.lParam;
}

static void load_children(HTREEITEM parent, node_ref *pref)
{
    child_list l = {0};
    bd_list_children(g_cat, pref->source_id, pref->node_id, collect_child, &l);
    for (int i = 0; i < l.n; i++) {
        child *c = &l.items[i];
        node_ref *r = calloc(1, sizeof(node_ref));
        if (r) {
            r->source_id = pref->source_id;
            r->node_id = c->info.node_id;
            r->is_dir = c->info.is_dir;
            r->state = c->info.state;
            r->rel = c->rel;
            c->rel = NULL;
        }
        wchar_t text[1024];
        item_text(&c->info, c->name, c->label, text, 1024);
        insert_item(parent, text, r, c->info.is_dir && (c->info.files_total > 0 || 1));
        free(c->name);
        free(c->rel);
        free(c->label);
    }
    free(l.items);
    pref->loaded = 1;
    if (l.n == 0) {
        TVITEMW it;
        memset(&it, 0, sizeof(it));
        it.mask = TVIF_CHILDREN;
        it.hItem = parent;
        it.cChildren = 0;
        SendMessageW(g_tree, TVM_SETITEMW, 0, (LPARAM)&it);
    }
}

typedef struct { int64_t source_id, node_id; } key;
typedef struct { key *k; int n, cap; } key_set;

static void keys_add(key_set *s, int64_t sid, int64_t nid)
{
    if (s->n == s->cap) {
        int cap = s->cap ? s->cap * 2 : 32;
        key *k = realloc(s->k, sizeof(key) * (size_t)cap);
        if (!k) return;
        s->k = k;
        s->cap = cap;
    }
    s->k[s->n].source_id = sid;
    s->k[s->n].node_id = nid;
    s->n++;
}

static int keys_has(const key_set *s, int64_t sid, int64_t nid)
{
    for (int i = 0; i < s->n; i++)
        if (s->k[i].source_id == sid && s->k[i].node_id == nid) return 1;
    return 0;
}

static void collect_expanded(HTREEITEM h, key_set *out)
{
    for (; h; h = TreeView_GetNextSibling(g_tree, h)) {
        if (TreeView_GetItemState(g_tree, h, TVIS_EXPANDED) & TVIS_EXPANDED) {
            node_ref *r = item_ref(h);
            if (r) keys_add(out, r->source_id, r->node_id);
            collect_expanded(TreeView_GetChild(g_tree, h), out);
        }
    }
}

static HTREEITEM g_reselect;

static void reexpand(HTREEITEM h, const key_set *expanded, key sel)
{
    for (; h; h = TreeView_GetNextSibling(g_tree, h)) {
        node_ref *r = item_ref(h);
        if (!r) continue;
        if (r->source_id == sel.source_id && r->node_id == sel.node_id) g_reselect = h;
        if (keys_has(expanded, r->source_id, r->node_id)) {
            TreeView_Expand(g_tree, h, TVE_EXPAND);
            reexpand(TreeView_GetChild(g_tree, h), expanded, sel);
        }
    }
}

typedef struct { bd_source_info *items; char **names, **paths, **labels; int n; } source_list;

static int collect_source(void *ctx, const bd_source_info *info)
{
    source_list *l = ctx;
    bd_source_info *ni = realloc(l->items, sizeof(bd_source_info) * (size_t)(l->n + 1));
    char **nn = realloc(l->names, sizeof(char *) * (size_t)(l->n + 1));
    if (ni) l->items = ni;
    if (nn) l->names = nn;
    char **np = realloc(l->paths, sizeof(char *) * (size_t)(l->n + 1));
    if (np) l->paths = np;
    char **nl = realloc(l->labels, sizeof(char *) * (size_t)(l->n + 1));
    if (nl) l->labels = nl;
    if (!ni || !nn || !np || !nl) return 1;
    l->items[l->n] = *info;
    l->names[l->n] = xstrdup(info->name);
    l->paths[l->n] = xstrdup(info->path);
    l->labels[l->n] = xstrdup(info->offline_media_label);
    l->n++;
    return 0;
}

static void rebuild_tree(void)
{
    key_set expanded = {0};
    key sel = {-1, -1};
    int first_build = TreeView_GetCount(g_tree) == 0;
    collect_expanded(TreeView_GetRoot(g_tree), &expanded);
    node_ref *sr = item_ref(TreeView_GetSelection(g_tree));
    if (sr) { sel.source_id = sr->source_id; sel.node_id = sr->node_id; }

    SendMessageW(g_tree, WM_SETREDRAW, FALSE, 0);
    TreeView_DeleteAllItems(g_tree);
    source_list l = {0};
    bd_list_sources(g_cat, collect_source, &l);
    for (int i = 0; i < l.n; i++) {
        node_ref *r = calloc(1, sizeof(node_ref));
        if (r) { r->source_id = l.items[i].source_id; r->is_dir = 1; r->state = l.items[i].state; r->rel = xstrdup(""); }
        bd_node_info as_folder;
        memset(&as_folder, 0, sizeof(as_folder));
        as_folder.is_dir = 1;
        as_folder.state = l.items[i].state;
        as_folder.files_total = l.items[i].files_total;
        as_folder.files_available = l.items[i].files_available;
        wchar_t counts[512], text[1024];
        item_text(&as_folder, l.names[i], l.labels[i], counts, 512);
        wchar_t *wp = widen(l.paths[i]);
        swprintf(text, 1024, L"%ls   [%ls]", counts, wp);
        HTREEITEM h = insert_item(TVI_ROOT, text, r, 1);
        if (first_build) keys_add(&expanded, r ? r->source_id : 0, 0);
        (void)h;
        free(wp);
        free(l.names[i]);
        free(l.paths[i]);
        free(l.labels[i]);
    }
    free(l.items);
    free(l.names);
    free(l.paths);
    free(l.labels);

    g_reselect = NULL;
    reexpand(TreeView_GetRoot(g_tree), &expanded, sel);
    if (g_reselect) TreeView_SelectItem(g_tree, g_reselect);
    free(expanded.k);
    SendMessageW(g_tree, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_tree, NULL, TRUE);
}

/* ---- Detail panel ------------------------------------------------------- */

static int add_copy_row(void *ctx, const bd_copy_info *c)
{
    int *row = ctx;
    wchar_t buf[512];
    LVITEMW it;
    memset(&it, 0, sizeof(it));
    it.mask = LVIF_TEXT;
    it.iItem = *row;
    swprintf(buf, 512, c->is_current ? L"v%d (current)" : L"v%d", c->version_no);
    it.pszText = buf;
    SendMessageW(g_list, LVM_INSERTITEMW, 0, (LPARAM)&it);

    wchar_t *where = widen(c->media_id ? c->media_label : "no copy yet"), where_buf[300];
    swprintf(where_buf, 300, L"%ls%ls", where ? where : L"", c->media_id && c->encrypted ? L" (encrypted)" : L"");
    it.iSubItem = 1;
    it.pszText = where_buf;
    SendMessageW(g_list, LVM_SETITEMTEXTW, *row, (LPARAM)&it);
    free(where);

    if (!c->media_id) swprintf(buf, 512, L"");
    else if (strcmp(c->copy_state, "ok") != 0) swprintf(buf, 512, strcmp(c->copy_state, "missing") == 0 ? L"Missing from drive" : L"Damaged");
    else swprintf(buf, 512, c->connected ? L"Good, plugged in" : L"Good, drive not plugged in");
    it.iSubItem = 2;
    it.pszText = buf;
    SendMessageW(g_list, LVM_SETITEMTEXTW, *row, (LPARAM)&it);

    wchar_t when[64];
    format_time_ms(c->media_id ? c->last_check_ms : 0, when, 64);
    it.iSubItem = 3;
    it.pszText = c->media_id ? when : L"";
    SendMessageW(g_list, LVM_SETITEMTEXTW, *row, (LPARAM)&it);

    wchar_t size[64];
    format_bytes(c->size, size, 64);
    it.iSubItem = 4;
    it.pszText = size;
    SendMessageW(g_list, LVM_SETITEMTEXTW, *row, (LPARAM)&it);

    wchar_t *path = widen(c->path_on_media);
    it.iSubItem = 5;
    it.pszText = path;
    SendMessageW(g_list, LVM_SETITEMTEXTW, *row, (LPARAM)&it);
    free(path);
    (*row)++;
    return 0;
}

static const wchar_t *state_words(bd_node_state s)
{
    switch (s) {
    case BD_STATE_AVAILABLE: return L"Available: a good copy is on a drive that is plugged in.";
    case BD_STATE_AVAILABLE_OLDER: return L"Changed since the last backup. Only an older version is on a plugged-in drive.";
    case BD_STATE_OFFLINE: return L"Backed up, but the drive is not plugged in.";
    case BD_STATE_NO_COPY: return L"Not backed up yet.";
    case BD_STATE_BAD: return L"The copy on the plugged-in drive is missing or damaged. Back up again to fix it.";
    case BD_STATE_DELETED: return L"Deleted from your folder. BRODALF still tracks its copies.";
    case BD_STATE_PARTIAL: return L"Some files inside are available.";
    }
    return L"";
}

/* A copy of what bd_list_media reports, kept past the callback. */
typedef struct {
    int64_t id;
    char label[256], kind[16], root[512], location[256];
    int connected, encrypted, has_hw;
    int64_t last_seen_ms, hw_read_ms, total, freeb, copies, added_ms;
    bd_drive_hw hw;
} drive_snap;

typedef struct { drive_snap *items; int n, cap; } drive_list;

static int drive_snap_cb(void *ctx, const bd_media_info *m)
{
    drive_list *l = ctx;
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 16;
        drive_snap *grown = realloc(l->items, sizeof(drive_snap) * (size_t)cap);
        if (!grown) return 1;
        l->items = grown;
        l->cap = cap;
    }
    drive_snap *d = &l->items[l->n++];
    memset(d, 0, sizeof(*d));
    d->id = m->media_id;
    snprintf(d->label, sizeof(d->label), "%s", m->label);
    snprintf(d->kind, sizeof(d->kind), "%s", m->kind);
    snprintf(d->root, sizeof(d->root), "%s", m->last_root ? m->last_root : "");
    snprintf(d->location, sizeof(d->location), "%s", m->location ? m->location : "");
    d->connected = m->connected;
    d->encrypted = m->encrypted;
    d->last_seen_ms = m->last_seen_ms;
    d->total = m->total_bytes;
    d->freeb = m->free_bytes;
    d->copies = m->copies;
    d->added_ms = m->added_ms;
    if (m->hw) { d->has_hw = 1; d->hw = *m->hw; d->hw_read_ms = m->hw_read_ms; }
    return 0;
}

static drive_list load_drives(void)
{
    drive_list l = {NULL, 0, 0};
    bd_list_media(g_cat, drive_snap_cb, &l);
    return l;
}

static const drive_snap *find_drive(const drive_list *l, int64_t id)
{
    for (int i = 0; i < l->n; i++)
        if (l->items[i].id == id) return &l->items[i];
    return NULL;
}

/* Growing wide-text buffer. */
typedef struct { wchar_t *p; size_t len, cap; } wtext;

static void wadd(wtext *t, const wchar_t *fmt, ...)
{
    wchar_t line[2048];
    va_list ap;
    va_start(ap, fmt);
    vswprintf(line, 2048, fmt, ap);
    va_end(ap);
    size_t n = wcslen(line);
    if (t->len + n + 1 > t->cap) {
        size_t cap = t->cap ? t->cap * 2 : 4096;
        while (cap < t->len + n + 1) cap *= 2;
        wchar_t *grown = realloc(t->p, cap * sizeof(wchar_t));
        if (!grown) return;
        t->p = grown;
        t->cap = cap;
    }
    wcscpy(t->p + t->len, line);
    t->len += n;
}

/* "WD Elements 25A3, 4.0 TB, USB" and the rest of what the disk said. */
static void add_drive_lines(wtext *t, const drive_snap *d, const wchar_t *indent)
{
    wchar_t when[64], a[64], b[64];
    if (strcmp(d->kind, "drive") != 0) {
        wchar_t *root = widen(d->root);
        wadd(t, L"%lsCloud storage: %ls\r\n", indent, root);
        free(root);
        return;
    }
    if (!d->has_hw) {
        wadd(t, L"%lsNo hardware details yet; they are read the next time the drive is plugged in.\r\n", indent);
        return;
    }
    const bd_drive_hw *h = &d->hw;
    wchar_t *vendor = widen(h->vendor), *model = widen(h->model), *bus = widen(h->bus), *serial = widen(h->serial);
    wchar_t *fw = widen(h->firmware), *vname = widen(h->volume_name), *vser = widen(h->volume_serial), *fs = widen(h->filesystem);
    wchar_t *health = widen(h->health), *note = widen(h->note);
    a[0] = 0;
    if (h->disk_bytes > 0) format_bytes(h->disk_bytes, a, 64);
    wchar_t name[256], line[512] = L"";
    swprintf(name, 256, L"%ls%ls%ls", vendor, *vendor && *model ? L" " : L"", model);
    const wchar_t *parts[3] = {name, a, bus};
    for (int i = 0; i < 3; i++)
        if (*parts[i]) swprintf(line + wcslen(line), 512 - wcslen(line), L"%ls%ls", *line ? L", " : L"", parts[i]);
    if (*line) wadd(t, L"%lsDisk: %ls\r\n", indent, line);
    if (*serial) wadd(t, L"%lsSerial number %ls%ls%ls\r\n", indent, serial, *fw ? L", firmware " : L"", fw);
    if (*vser) wadd(t, L"%lsVolume \"%ls\" (%ls, serial %ls)\r\n", indent, *vname ? vname : L"no name", fs, vser);
    if (h->smart) {
        wadd(t, L"%lsHealth: %ls", indent, *health ? health : L"unknown");
        if (h->temperature_c >= 0) wadd(t, L", %d \x00B0" L"C", h->temperature_c);
        if (h->power_on_hours >= 0) wadd(t, L", %lld hours powered on", (long long)h->power_on_hours);
        if (h->power_cycles >= 0) wadd(t, L", %lld power cycles", (long long)h->power_cycles);
        if (h->percent_used >= 0) wadd(t, L", %d%% worn", h->percent_used);
        wadd(t, L"\r\n");
        if (h->reallocated_sectors > 0 || h->pending_sectors > 0 || h->uncorrectable_sectors > 0)
            wadd(t, L"%ls%lld reallocated, %lld waiting to be reallocated, %lld unreadable sectors\r\n", indent,
                 (long long)(h->reallocated_sectors > 0 ? h->reallocated_sectors : 0),
                 (long long)(h->pending_sectors > 0 ? h->pending_sectors : 0),
                 (long long)(h->uncorrectable_sectors > 0 ? h->uncorrectable_sectors : 0));
    } else if (h->temperature_c >= 0) {
        wadd(t, L"%lsTemperature %d \x00B0" L"C\r\n", indent, h->temperature_c);
    }
    if (*note) wadd(t, L"%ls(%ls)\r\n", indent, note);
    format_time_ms(d->hw_read_ms, when, 64);
    wadd(t, L"%lsRead from the drive %ls\r\n", indent, when);
    if (d->total > 0) {
        format_bytes(d->freeb, a, 64);
        format_bytes(d->total, b, 64);
        wadd(t, L"%ls%ls free of %ls when last seen\r\n", indent, a, b);
    }
    free(vendor); free(model); free(bus); free(serial); free(fw); free(vname); free(vser); free(fs); free(health); free(note);
}

/* One drive: its name, where it is kept, whether it is plugged in. */
static void add_drive_heading(wtext *t, const drive_snap *d, const wchar_t *indent)
{
    wchar_t when[64];
    wchar_t *label = widen(d->label), *loc = widen(d->location), *root = widen(d->root);
    format_time_ms(d->last_seen_ms, when, 64);
    wadd(t, L"%ls%ls%ls\r\n", indent, label, d->encrypted ? L" (encrypted)" : L"");
    if (*loc) wadd(t, L"%ls    Kept in: %ls\r\n", indent, loc);
    if (d->connected) wadd(t, L"%ls    Plugged in now at %ls\r\n", indent, root);
    else wadd(t, L"%ls    Not plugged in. Last seen %ls%ls%ls\r\n", indent, when, *root ? L" at " : L"", root);
    free(label); free(loc); free(root);
}

typedef struct { bd_copy_info c; char hash[80], label[256], path[1024], state[16], location[256]; } copy_snap;
typedef struct { copy_snap *items; int n, cap; } copy_list;

static int copy_snap_cb(void *ctx, const bd_copy_info *c)
{
    copy_list *l = ctx;
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 16;
        copy_snap *grown = realloc(l->items, sizeof(copy_snap) * (size_t)cap);
        if (!grown) return 1;
        l->items = grown;
        l->cap = cap;
    }
    copy_snap *s = &l->items[l->n++];
    s->c = *c;
    snprintf(s->hash, sizeof(s->hash), "%s", c->hash ? c->hash : "");
    snprintf(s->label, sizeof(s->label), "%s", c->media_label ? c->media_label : "");
    snprintf(s->path, sizeof(s->path), "%s", c->path_on_media ? c->path_on_media : "");
    snprintf(s->state, sizeof(s->state), "%s", c->copy_state ? c->copy_state : "");
    snprintf(s->location, sizeof(s->location), "%s", c->media_location ? c->media_location : "");
    return 0;
}

/* Point the copied row at its own strings (the list may have moved). */
static const bd_copy_info *copy_of(copy_snap *s)
{
    s->c.hash = s->hash;
    s->c.media_label = s->label;
    s->c.path_on_media = s->path;
    s->c.copy_state = s->state;
    s->c.media_location = s->location;
    return &s->c;
}

static void format_ns(int64_t ns, wchar_t *out, size_t n)
{
    format_time_ms(ns / 1000000, out, n);
}

static void show_detail(void)
{
    SendMessageW(g_list, LVM_DELETEALLITEMS, 0, 0);
    node_ref *r = item_ref(TreeView_GetSelection(g_tree));
    if (!r) { SetWindowTextW(g_detail, L"Select a file to see what it is, its versions, and which drives hold it."); return; }
    wchar_t *rel = widen(r->rel && *r->rel ? r->rel : "(whole folder)");
    wtext t = {NULL, 0, 0};
    wadd(&t, L"%ls\r\n%ls\r\n", rel, state_words(r->state));
    free(rel);
    if (!r->is_dir) {
        copy_list cl = {NULL, 0, 0};
        bd_list_copies(g_cat, r->node_id, copy_snap_cb, &cl);
        drive_list dl = load_drives();
        int versions = 0, current = 0;
        for (int i = 0; i < cl.n; i++) {
            if (cl.items[i].c.version_no > versions) versions = cl.items[i].c.version_no;
            if (cl.items[i].c.is_current) current = cl.items[i].c.version_no;
        }
        for (int i = 0; i < cl.n; i++) {
            if (!cl.items[i].c.is_current) continue;
            const bd_copy_info *c = &cl.items[i].c;
            wchar_t size[64], mod[64], seen[64];
            format_bytes(c->size, size, 64);
            format_ns(c->mtime_ns, mod, 64);
            format_time_ms(c->first_seen_ms, seen, 64);
            wchar_t *hash = widen(cl.items[i].hash);
            wadd(&t, L"\r\nFile\r\n    Size %ls, modified %ls\r\n    Version %d of %d, first seen %ls\r\n    BLAKE3 %ls\r\n",
                 size, mod, current, versions, seen, hash);
            free(hash);
            break;
        }
        /* Where the current version is. */
        int any = 0;
        wadd(&t, L"\r\nCopies of this version\r\n");
        for (int i = 0; i < cl.n; i++) {
            const bd_copy_info *c = &cl.items[i].c;
            if (!c->is_current || !c->media_id) continue;
            const drive_snap *d = find_drive(&dl, c->media_id);
            if (!d) continue;
            any = 1;
            add_drive_heading(&t, d, L"    ");
            wchar_t *path = widen(cl.items[i].path);
            wadd(&t, L"        Copy %ls, at %ls\r\n",
                 strcmp(cl.items[i].state, "ok") == 0 ? L"good" : strcmp(cl.items[i].state, "missing") == 0 ? L"missing" : L"damaged",
                 path);
            free(path);
            add_drive_lines(&t, d, L"        ");
        }
        if (!any) wadd(&t, L"    None yet. Back up to put a copy on a drive.\r\n");
        /* Drives holding only older versions. */
        int header = 0;
        for (int i = 0; i < cl.n; i++) {
            const bd_copy_info *c = &cl.items[i].c;
            if (c->is_current || !c->media_id) continue;
            int dup = 0;
            for (int k = 0; k < i; k++) if (!cl.items[k].c.is_current && cl.items[k].c.media_id == c->media_id) dup = 1;
            if (dup) continue;
            const drive_snap *d = find_drive(&dl, c->media_id);
            if (!d) continue;
            if (!header) { wadd(&t, L"\r\nOlder versions are also on\r\n"); header = 1; }
            add_drive_heading(&t, d, L"    ");
        }
        for (int i = 0; i < cl.n; i++) {
            int row = i;
            add_copy_row(&row, copy_of(&cl.items[i]));
        }
        free(cl.items);
        free(dl.items);
    }
    SetWindowTextW(g_detail, t.p ? t.p : L"");
    free(t.p);
}

/* ---- Drive details dialog ------------------------------------------------ */

static void update_drives_label(void);

static const drive_snap *g_edit_drive;
static wchar_t g_edit_name[256], g_edit_location[256];

static INT_PTR CALLBACK drive_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG: {
        const drive_snap *d = g_edit_drive;
        wtext t = {NULL, 0, 0};
        wchar_t added[64], copies[32];
        add_drive_heading(&t, d, L"");
        format_time_ms(d->added_ms, added, 64);
        swprintf(copies, 32, L"%lld", (long long)d->copies);
        wadd(&t, L"    Set up %ls, %ls copies recorded\r\n\r\n", added, copies);
        add_drive_lines(&t, d, L"");
        SetDlgItemTextW(dlg, IDC_DRIVE_INFO, t.p ? t.p : L"");
        free(t.p);
        wchar_t *name = widen(d->label), *loc = widen(d->location);
        SetDlgItemTextW(dlg, IDC_DRIVE_NAME, name);
        SetDlgItemTextW(dlg, IDC_DRIVE_LOCATION, loc);
        free(name);
        free(loc);
        SetFocus(GetDlgItem(dlg, IDC_DRIVE_LOCATION));
        return FALSE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            GetDlgItemTextW(dlg, IDC_DRIVE_NAME, g_edit_name, 256);
            GetDlgItemTextW(dlg, IDC_DRIVE_LOCATION, g_edit_location, 256);
            if (!g_edit_name[0]) { MessageBeep(MB_ICONWARNING); return TRUE; }
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

static void cmd_drives(void)
{
    drive_list dl = load_drives();
    if (!dl.n) {
        MessageBoxW(g_main, L"No drives yet. Use Back up to set one up.", APP_NAME, MB_ICONINFORMATION);
        return;
    }
    HMENU m = CreatePopupMenu();
    for (int i = 0; i < dl.n; i++) {
        const drive_snap *d = &dl.items[i];
        wchar_t *label = widen(d->label), *loc = widen(d->location), text[600];
        swprintf(text, 600, L"%ls%ls%ls%ls", label, *loc ? L"  (" : L"", loc, *loc ? L")" : L"");
        if (d->connected) wcsncat(text, L"  - plugged in", 599 - wcslen(text));
        AppendMenuW(m, MF_STRING, (UINT_PTR)(ID_MENU_DRIVE_BASE + i), text);
        free(label);
        free(loc);
    }
    RECT r;
    GetWindowRect(g_buttons[5], &r);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, r.left, r.bottom, 0, g_main, NULL);
    DestroyMenu(m);
    if (cmd >= ID_MENU_DRIVE_BASE && cmd - ID_MENU_DRIVE_BASE < dl.n) {
        g_edit_drive = &dl.items[cmd - ID_MENU_DRIVE_BASE];
        if (DialogBoxW(g_inst, MAKEINTRESOURCEW(IDD_DRIVE), g_main, drive_proc) == IDOK) {
            char *name = narrow(g_edit_name), *loc = narrow(g_edit_location);
            int64_t id = g_edit_drive->id;
            bd_status st = BD_OK;
            if (name && strcmp(name, g_edit_drive->label) != 0) st = bd_media_rename(g_cat, id, name);
            if (st == BD_OK) st = bd_media_set_location(g_cat, id, loc);
            if (st == BD_OK) st = bd_catalog_save(g_cat);
            if (st != BD_OK) {
                wchar_t *e = widen(bd_catalog_error(g_cat));
                MessageBoxW(g_main, e, APP_NAME, MB_ICONWARNING);
                free(e);
            }
            free(name);
            free(loc);
            update_drives_label();
            rebuild_tree();
            show_detail();
        }
        g_edit_drive = NULL;
    }
    free(dl.items);
}

/* ---- Drives label ------------------------------------------------------- */

typedef struct { wchar_t text[1024]; int connected, total; } drives_text;

static int drive_label_cb(void *ctx, const bd_media_info *m)
{
    drives_text *d = ctx;
    d->total++;
    if (!m->connected) return 0;
    wchar_t *l = widen(m->label), *root = widen(m->last_root), part[256];
    swprintf(part, 256, L"%ls%ls (%ls)", d->connected ? L", " : L"", l, root);
    wcsncat(d->text, part, 1023 - wcslen(d->text));
    d->connected++;
    free(l);
    free(root);
    return 0;
}

static void update_drives_label(void)
{
    drives_text d;
    wcscpy(d.text, L"Plugged in: ");
    d.connected = d.total = 0;
    bd_list_media(g_cat, drive_label_cb, &d);
    if (d.total == 0) wcscpy(d.text, L"No backup drives yet. Use Back up to set one up.");
    else if (d.connected == 0) swprintf(d.text, 1024, L"None of your %d backup drive(s) are plugged in.", d.total);
    SetWindowTextW(g_drives, d.text);
    /* The button says how many files are short of the target. */
    bd_risk_stats rs;
    wchar_t risk[64] = L"At risk...";
    if (bd_list_at_risk(g_cat, 0, NULL, NULL, &rs) == BD_OK && rs.files_at_risk > 0)
        swprintf(risk, 64, L"At risk (%lld)...", (long long)rs.files_at_risk);
    SetWindowTextW(g_buttons[7], risk);
}

/* ---- Files at risk ------------------------------------------------------ */

#define RISK_ROWS 2000

static void lv_columns(HWND lv, const wchar_t **names, const int *widths, int n)
{
    ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT);
    for (int i = 0; i < n; i++) {
        LVCOLUMNW c;
        memset(&c, 0, sizeof(c));
        c.mask = LVCF_TEXT | LVCF_WIDTH;
        c.pszText = (wchar_t *)names[i];
        c.cx = S(widths[i]);
        ListView_InsertColumn(lv, i, &c);
    }
}

static void lv_row(HWND lv, const wchar_t **cells, int n)
{
    LVITEMW it;
    memset(&it, 0, sizeof(it));
    it.mask = LVIF_TEXT;
    it.iItem = ListView_GetItemCount(lv);
    it.pszText = (wchar_t *)cells[0];
    int row = (int)SendMessageW(lv, LVM_INSERTITEMW, 0, (LPARAM)&it);
    for (int i = 1; i < n; i++) {
        LVITEMW sub;
        memset(&sub, 0, sizeof(sub));
        sub.iSubItem = i;
        sub.pszText = (wchar_t *)cells[i];
        SendMessageW(lv, LVM_SETITEMTEXTW, (WPARAM)row, (LPARAM)&sub);
    }
}

typedef struct { HWND lv; int rows; } risk_fill_ctx;

/* Help rows: media ids and whether each is connected, by row. */
static int64_t g_risk_media[64];
static int g_risk_connected[64], g_risk_media_n;
static wchar_t g_risk_media_name[64][128];
static int64_t g_risk_pick;

static int risk_file_cb(void *ctx, const bd_risk_info *r)
{
    risk_fill_ctx *f = ctx;
    if (f->rows >= RISK_ROWS) return 1;
    f->rows++;
    wchar_t *src = widen(r->source_name), *rel = widen(r->rel_path), file[1200], copies[16], places[16], size[64], note[200];
    swprintf(file, 1200, L"%ls\\%ls", src ? src : L"", rel ? rel : L"");
    for (wchar_t *c = file; *c; c++) if (*c == L'/') *c = L'\\';
    swprintf(copies, 16, L"%d", r->copies);
    swprintf(places, 16, L"%d", r->places);
    format_bytes(r->size, size, 64);
    note[0] = 0;
    if (r->older_copies) wcscat(note, L"Changed since its last backup. ");
    if (r->unknown_place) wcscat(note, L"On a drive with no place set.");
    const wchar_t *cells[] = {file, copies, places, size, note};
    lv_row(f->lv, cells, 5);
    free(src);
    free(rel);
    return 0;
}

static int risk_help_cb(void *ctx, const bd_risk_help *h)
{
    risk_fill_ctx *f = ctx;
    wchar_t *label = widen(h->label), *loc = widen(h->location), name[300], files[32], size[64];
    swprintf(name, 300, L"%ls%ls", label ? label : L"", h->connected ? L"  (plugged in)" : L"");
    swprintf(files, 32, L"%lld file%ls", (long long)h->files, h->files == 1 ? L"" : L"s");
    format_bytes(h->bytes, size, 64);
    const wchar_t *kind = strcmp(h->kind, "drive") == 0 ? L"Drive" : strcmp(h->kind, "onedrive") == 0 ? L"OneDrive" : L"Dropbox";
    const wchar_t *cells[] = {name, kind, loc && *loc ? loc : L"(not set)", files, size};
    lv_row(f->lv, cells, 5);
    if (g_risk_media_n < 64) {
        g_risk_media[g_risk_media_n] = h->media_id;
        g_risk_connected[g_risk_media_n] = h->connected;
        swprintf(g_risk_media_name[g_risk_media_n], 128, L"%ls%ls%ls%ls", label ? label : L"", loc && *loc ? L" (kept in " : L"",
                 loc && *loc ? loc : L"", loc && *loc ? L")" : L"");
        g_risk_media_n++;
    }
    free(label);
    free(loc);
    f->rows++;
    return 0;
}

static void risk_fill(HWND dlg)
{
    bd_target t;
    bd_target_get(g_cat, &t);
    bd_risk_stats st;
    memset(&st, 0, sizeof(st));
    bd_list_at_risk(g_cat, 0, NULL, NULL, &st);

    wchar_t text[512], size[64];
    format_bytes(st.bytes_at_risk, size, 64);
    if (st.files_total == 0)
        swprintf(text, 512, L"No files yet. Add a folder and scan it first.");
    else if (st.files_at_risk == 0)
        swprintf(text, 512, L"All %lld files have at least %d copies in %d different places.", (long long)st.files_total, t.copies, t.places);
    else
        swprintf(text, 512, L"%lld of %lld files (%ls) %ls fewer than %d copies in %d different places. %lld %ls no copy of %ls current version at all.",
                 (long long)st.files_at_risk, (long long)st.files_total, size, st.files_at_risk == 1 ? L"has" : L"have", t.copies, t.places,
                 (long long)st.files_no_copy, st.files_no_copy == 1 ? L"has" : L"have", st.files_no_copy == 1 ? L"its" : L"their");
    SetDlgItemTextW(dlg, IDC_RISK_SUMMARY, text);
    EnableWindow(GetDlgItem(dlg, IDC_RISK_BACKUP), st.files_at_risk > 0);

    HWND help = GetDlgItem(dlg, IDC_RISK_HELP), files = GetDlgItem(dlg, IDC_RISK_FILES);
    ListView_DeleteAllItems(help);
    ListView_DeleteAllItems(files);
    risk_fill_ctx hc = {help, 0};
    g_risk_media_n = 0;
    if (st.files_at_risk) bd_list_risk_help(g_cat, risk_help_cb, &hc);
    if (st.files_at_risk && !hc.rows) {
        const wchar_t *cells[] = {L"None of your drives would help. Set up another drive, or one kept somewhere else.", L"", L"", L"", L""};
        lv_row(help, cells, 5);
    }
    risk_fill_ctx fc = {files, 0};
    if (st.files_at_risk) bd_list_at_risk(g_cat, 0, risk_file_cb, &fc, NULL);
    if (st.files_at_risk > fc.rows) {
        wchar_t more[128];
        swprintf(more, 128, L"... and %lld more", (long long)(st.files_at_risk - fc.rows));
        const wchar_t *cells[] = {more, L"", L"", L"", L""};
        lv_row(files, cells, 5);
    }
}

static INT_PTR CALLBACK risk_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG: {
        static const wchar_t *help_cols[] = {L"Drive", L"Kind", L"Kept in", L"Would help", L"Size"};
        static const int help_w[] = {170, 60, 120, 90, 80};
        static const wchar_t *file_cols[] = {L"File", L"Copies", L"Places", L"Size", L"Note"};
        static const int file_w[] = {280, 56, 56, 70, 200};
        lv_columns(GetDlgItem(dlg, IDC_RISK_HELP), help_cols, help_w, 5);
        lv_columns(GetDlgItem(dlg, IDC_RISK_FILES), file_cols, file_w, 5);
        bd_target t;
        bd_target_get(g_cat, &t);
        SetDlgItemInt(dlg, IDC_RISK_COPIES, (UINT)t.copies, FALSE);
        SetDlgItemInt(dlg, IDC_RISK_PLACES, (UINT)t.places, FALSE);
        risk_fill(dlg);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            /* Enter in one of the number boxes applies it. */
            HWND f = GetFocus();
            if (f == GetDlgItem(dlg, IDC_RISK_COPIES) || f == GetDlgItem(dlg, IDC_RISK_PLACES))
                wp = MAKEWPARAM(IDC_RISK_APPLY, BN_CLICKED);
        }
        if (LOWORD(wp) == IDC_RISK_BACKUP) {
            int row = ListView_GetNextItem(GetDlgItem(dlg, IDC_RISK_HELP), -1, LVNI_SELECTED);
            if (row < 0 || row >= g_risk_media_n) {
                MessageBoxW(dlg, L"Pick a drive in the list first.", APP_NAME, MB_ICONINFORMATION);
                return TRUE;
            }
            if (!g_risk_connected[row]) {
                wchar_t m[400];
                swprintf(m, 400, L"Plug in %ls first. BRODALF will find it on its own.", g_risk_media_name[row]);
                MessageBoxW(dlg, m, APP_NAME, MB_ICONINFORMATION);
                return TRUE;
            }
            g_risk_pick = g_risk_media[row];
            EndDialog(dlg, IDC_RISK_BACKUP);
            return TRUE;
        }
        if (LOWORD(wp) == IDC_RISK_APPLY) {
            bd_target t;
            t.copies = (int)GetDlgItemInt(dlg, IDC_RISK_COPIES, NULL, FALSE);
            t.places = (int)GetDlgItemInt(dlg, IDC_RISK_PLACES, NULL, FALSE);
            bd_status s = bd_target_set(g_cat, &t);
            if (s == BD_OK) s = bd_catalog_save(g_cat);
            if (s != BD_OK) {
                wchar_t *e = widen(bd_catalog_error(g_cat));
                MessageBoxW(dlg, e ? e : L"Could not change the target.", APP_NAME, MB_ICONWARNING);
                free(e);
                return TRUE;
            }
            log_append(L"Protection target changed.");
            risk_fill(dlg);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL || LOWORD(wp) == IDOK) { EndDialog(dlg, IDOK); return TRUE; }
        break;
    case WM_NOTIFY: {
        NMHDR *nh = (NMHDR *)lp;
        if (nh->idFrom == IDC_RISK_HELP && nh->code == NM_DBLCLK) SendMessageW(dlg, WM_COMMAND, IDC_RISK_BACKUP, 0);
        return TRUE;
    }
    }
    return FALSE;
}


/* ---- Folder and name prompts -------------------------------------------- */

static int CALLBACK browse_cb(HWND hwnd, UINT msg, LPARAM lp, LPARAM data)
{
    (void)lp;
    if (msg == BFFM_INITIALIZED && data) SendMessageW(hwnd, BFFM_SETSELECTIONW, TRUE, data);
    return 0;
}

static char *pick_folder(HWND owner, const wchar_t *title)
{
    BROWSEINFOW bi;
    memset(&bi, 0, sizeof(bi));
    bi.hwndOwner = owner;
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    bi.lpfn = browse_cb;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return NULL;
    wchar_t path[MAX_PATH * 4];
    char *out = NULL;
    if (SHGetPathFromIDListEx(pidl, path, MAX_PATH * 4, 0)) out = narrow(path);
    CoTaskMemFree(pidl);
    return out;
}

static wchar_t g_label_buf[256];
static int g_label_encrypt;
static int g_label_cloud; /* naming a cloud account rather than a drive */
static wchar_t g_label_location[256];

static INT_PTR CALLBACK label_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG:
        SetDlgItemTextW(dlg, IDC_LABEL_EDIT, g_label_buf);
        g_label_location[0] = 0;
        if (g_label_cloud) {
            ShowWindow(GetDlgItem(dlg, IDC_LABEL_LOC_TEXT), SW_HIDE);
            ShowWindow(GetDlgItem(dlg, IDC_LABEL_LOCATION), SW_HIDE);
            SetWindowTextW(dlg, L"Add cloud storage");
            SetDlgItemTextW(dlg, IDC_LABEL_TEXT, L"Name this storage. Your browser opens next so you can sign in; BRODALF only sees its own Apps/BRODALF folder.");
            SetDlgItemTextW(dlg, IDC_LABEL_ENCRYPT, L"Encrypt the files stored there (needs a passphrase)");
            SetDlgItemTextW(dlg, IDOK, L"Sign in");
        }
        SendDlgItemMessageW(dlg, IDC_LABEL_EDIT, EM_SETSEL, 0, -1);
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            GetDlgItemTextW(dlg, IDC_LABEL_EDIT, g_label_buf, 256);
            if (!g_label_buf[0]) { MessageBeep(MB_ICONWARNING); return TRUE; }
            g_label_encrypt = IsDlgButtonChecked(dlg, IDC_LABEL_ENCRYPT) == BST_CHECKED;
            GetDlgItemTextW(dlg, IDC_LABEL_LOCATION, g_label_location, 256);
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

/* ---- Passphrase prompts ------------------------------------------------- */

static wchar_t g_pass_buf[512];
static const wchar_t *g_pass_text;
static int g_pass_catalog;

static INT_PTR CALLBACK pass_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG:
        SetDlgItemTextW(dlg, IDC_PASS_TEXT, g_pass_text);
        SetFocus(GetDlgItem(dlg, IDC_PASS_EDIT));
        return FALSE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            GetDlgItemTextW(dlg, IDC_PASS_EDIT, g_pass_buf, 512);
            if (GetDlgItem(dlg, IDC_PASS_EDIT2)) {
                wchar_t again[512];
                GetDlgItemTextW(dlg, IDC_PASS_EDIT2, again, 512);
                int same = wcscmp(again, g_pass_buf) == 0;
                SecureZeroMemory(again, sizeof(again));
                if (wcslen(g_pass_buf) < 8) {
                    MessageBoxW(dlg, L"Use at least 8 characters. A few unrelated words make a good passphrase.", APP_NAME, MB_ICONWARNING);
                    return TRUE;
                }
                if (!same) {
                    MessageBoxW(dlg, L"The two passphrases are different. Type them again.", APP_NAME, MB_ICONWARNING);
                    return TRUE;
                }
                g_pass_catalog = IsDlgButtonChecked(dlg, IDC_PASS_CATALOG) == BST_CHECKED;
            }
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

/* Ask for a passphrase; returns UTF-8 (caller wipes and frees) or NULL. */
static char *ask_passphrase(HWND owner, int dialog, const wchar_t *text)
{
    g_pass_text = text;
    g_pass_catalog = 0;
    INT_PTR rc = DialogBoxW(g_inst, MAKEINTRESOURCEW(dialog), owner, pass_proc);
    char *p = rc == IDOK ? narrow(g_pass_buf) : NULL;
    SecureZeroMemory(g_pass_buf, sizeof(g_pass_buf));
    return p;
}

static void free_secret(char *p)
{
    if (!p) return;
    SecureZeroMemory(p, strlen(p));
    free(p);
}

/* Make sure the key is available. Only while no job runs. 1 if unlocked. */
static int unlock_ui(const wchar_t *why)
{
    if (!bd_catalog_has_passphrase(g_cat) || bd_catalog_is_unlocked(g_cat)) return 1;
    for (;;) {
        char *p = ask_passphrase(g_main, IDD_PASSPHRASE, why);
        if (!p) return 0;
        SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_WAIT));
        bd_status s = bd_catalog_unlock(g_cat, p);
        free_secret(p);
        if (s == BD_OK) return 1;
        MessageBoxW(g_main, L"That passphrase is not right. Try again.", APP_NAME, MB_ICONWARNING);
    }
}

/* Set the first passphrase, or change it. 1 on success. */
static int set_passphrase_ui(void)
{
    int changing = bd_catalog_has_passphrase(g_cat);
    if (changing && !unlock_ui(L"Enter the current passphrase first.")) return 0;
    char *p = ask_passphrase(g_main, IDD_NEW_PASSPHRASE,
                             L"The passphrase protects encrypted drives and, if you choose, this catalog. "
                             L"Write it down somewhere safe: without it, encrypted copies cannot be read, by you or anyone.");
    if (!p) return 0;
    SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_WAIT));
    bd_status s = bd_catalog_set_passphrase(g_cat, p);
    free_secret(p);
    if (s == BD_OK && g_pass_catalog) s = bd_catalog_set_file_encrypted(g_cat, 1);
    if (s == BD_OK) s = bd_catalog_save(g_cat);
    if (s != BD_OK) {
        wchar_t *e = widen(bd_catalog_error(g_cat));
        report_error(g_main, e && e[0] ? e : L"Could not set the passphrase.");
        free(e);
        return 0;
    }
    log_append(changing ? L"Passphrase changed." : L"Passphrase set.");
    return 1;
}

typedef struct { int64_t id; int encrypted; int any_connected_encrypted; } enc_ctx;

static int enc_cb(void *ctx, const bd_media_info *m)
{
    enc_ctx *c = ctx;
    if (m->media_id == c->id) c->encrypted = m->encrypted;
    if (m->connected && m->encrypted) c->any_connected_encrypted = 1;
    return 0;
}

static enc_ctx media_encryption(int64_t media_id)
{
    enc_ctx c = {media_id, 0, 0};
    bd_list_media(g_cat, enc_cb, &c);
    return c;
}

static void cmd_risk(void)
{
    g_risk_pick = 0;
    INT_PTR r = DialogBoxW(g_inst, MAKEINTRESOURCEW(IDD_RISK), g_main, risk_proc);
    update_drives_label();
    if (r != IDC_RISK_BACKUP || g_risk_pick <= 0) return;
    if (media_encryption(g_risk_pick).encrypted && !unlock_ui(L"This drive is encrypted. Enter the passphrase to back up to it.")) return;
    job *j = new_job(JOB_BACKUP);
    if (!j) return;
    j->media_id = g_risk_pick;
    enqueue(j);
}

static void cmd_security(void)
{
    int has = bd_catalog_has_passphrase(g_cat), unlocked = bd_catalog_is_unlocked(g_cat);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, ID_MENU_SET_PASS, has ? L"Change the passphrase..." : L"Set a passphrase...");
    AppendMenuW(m, MF_STRING | (has ? 0 : MF_GRAYED) | (bd_catalog_file_encrypted(g_cat) ? MF_CHECKED : 0),
                ID_MENU_ENCRYPT_CATALOG, L"Encrypt the catalog file");
    AppendMenuW(m, MF_STRING | (has && unlocked ? 0 : MF_GRAYED), ID_MENU_LOCK, L"Forget the passphrase until it is needed");
    RECT r;
    GetWindowRect(g_buttons[5], &r);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, r.left, r.bottom, 0, g_main, NULL);
    DestroyMenu(m);
    if (cmd == ID_MENU_SET_PASS) {
        set_passphrase_ui();
    } else if (cmd == ID_MENU_ENCRYPT_CATALOG) {
        int on = !bd_catalog_file_encrypted(g_cat);
        if (on && !unlock_ui(L"Enter the passphrase to encrypt the catalog file.")) return;
        bd_status s = bd_catalog_set_file_encrypted(g_cat, on);
        if (s == BD_OK) s = bd_catalog_save(g_cat);
        if (s != BD_OK) {
            wchar_t *e = widen(bd_catalog_error(g_cat));
            report_error(g_main, e && e[0] ? e : L"Could not change the catalog encryption.");
            free(e);
            return;
        }
        log_append(on ? L"The catalog file is now encrypted. BRODALF will ask for the passphrase when it opens."
                      : L"The catalog file is no longer encrypted.");
    } else if (cmd == ID_MENU_LOCK) {
        bd_catalog_lock_key(g_cat);
        log_append(L"Passphrase forgotten. BRODALF will ask for it again when it needs it.");
    }
}

/* ---- Commands ----------------------------------------------------------- */

static void cmd_add_folder(void)
{
    char *folder = pick_folder(g_main, L"Choose a folder to protect. BRODALF will scan it and track every file inside.");
    if (!folder) return;
    int64_t id;
    if (bd_source_add(g_cat, folder, &id) != BD_OK) {
        wchar_t *e = widen(bd_catalog_error(g_cat));
        MessageBoxW(g_main, e, APP_NAME, MB_ICONWARNING);
        free(e);
    } else {
        wchar_t *w = widen(folder), line[1024];
        swprintf(line, 1024, L"Now protecting %ls", w);
        log_append(line);
        free(w);
        rebuild_tree();
        enqueue(new_job(JOB_SCAN));
    }
    free(folder);
}

typedef struct { HMENU menu; int count; } menu_ctx;

static int drive_menu_cb(void *ctx, const bd_media_info *m)
{
    menu_ctx *c = ctx;
    if (!m->connected) return 0;
    wchar_t *l = widen(m->label), *root = widen(m->last_root), text[512], freeb[64];
    format_bytes(m->free_bytes, freeb, 64);
    swprintf(text, 512, L"%ls (%ls), %ls free%ls", l, root, freeb, m->encrypted ? L", encrypted" : L"");
    AppendMenuW(c->menu, MF_STRING, (UINT_PTR)(ID_MENU_DRIVE_BASE + m->media_id), text);
    c->count++;
    free(l);
    free(root);
    return 0;
}

/* Returns a media id, 0 for "another drive or folder", -1 if cancelled,
 * CHOSE_ONEDRIVE or CHOSE_DROPBOX to add a cloud account. */
#define CHOSE_ONEDRIVE (-2)
#define CHOSE_DROPBOX (-3)
static int64_t choose_drive(HWND button, int allow_other)
{
    menu_ctx c = {CreatePopupMenu(), 0};
    bd_list_media(g_cat, drive_menu_cb, &c);
    if (allow_other) {
        if (c.count) AppendMenuW(c.menu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(c.menu, MF_STRING, ID_MENU_OTHER, L"Another drive or folder...");
        AppendMenuW(c.menu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(c.menu, MF_STRING, ID_MENU_ONEDRIVE, L"OneDrive...");
        AppendMenuW(c.menu, MF_STRING, ID_MENU_DROPBOX, L"Dropbox...");
    } else if (!c.count) {
        DestroyMenu(c.menu);
        MessageBoxW(g_main, L"Plug in a backup drive first.", APP_NAME, MB_ICONINFORMATION);
        return -1;
    }
    RECT r;
    GetWindowRect(button, &r);
    int cmd = TrackPopupMenu(c.menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, r.left, r.bottom, 0, g_main, NULL);
    DestroyMenu(c.menu);
    if (cmd == ID_MENU_OTHER) return 0;
    if (cmd == ID_MENU_ONEDRIVE) return CHOSE_ONEDRIVE;
    if (cmd == ID_MENU_DROPBOX) return CHOSE_DROPBOX;
    if (cmd >= ID_MENU_DRIVE_BASE) return cmd - ID_MENU_DRIVE_BASE;
    return -1;
}

static void cmd_add_cloud(bd_cloud_provider prov)
{
    wcscpy(g_label_buf, prov == BD_CLOUD_ONEDRIVE ? L"OneDrive" : L"Dropbox");
    g_label_cloud = 1;
    INT_PTR ok = DialogBoxW(g_inst, MAKEINTRESOURCEW(IDD_LABEL), g_main, label_proc);
    g_label_cloud = 0;
    if (ok != IDOK) return;
    job *j = new_job(JOB_BACKUP);
    if (!j) return;
    j->provider = prov;
    j->label = narrow(g_label_buf);
    if (g_label_encrypt) {
        int unlocked = bd_catalog_has_passphrase(g_cat) ? unlock_ui(L"Enter the passphrase to set up encrypted cloud storage.")
                                                        : set_passphrase_ui();
        if (!unlocked) { job_free(j); return; }
        j->flags = BD_MEDIA_ENCRYPTED;
    }
    enqueue(j);
}

static void cmd_backup(void)
{
    int64_t id = choose_drive(g_buttons[2], 1);
    if (id == CHOSE_ONEDRIVE || id == CHOSE_DROPBOX) { cmd_add_cloud(id == CHOSE_ONEDRIVE ? BD_CLOUD_ONEDRIVE : BD_CLOUD_DROPBOX); return; }
    if (id < 0) return;
    job *j = new_job(JOB_BACKUP);
    if (!j) return;
    if (id > 0) {
        j->media_id = id;
    } else {
        j->root = pick_folder(g_main, L"Choose the drive (or a folder on it) to back up to.");
        if (!j->root) { job_free(j); return; }
        if (!is_brodalf_drive(j->root)) {
            wchar_t *w = widen(j->root);
            const wchar_t *slash = w ? wcsrchr(w, L'\\') : NULL;
            wcsncpy(g_label_buf, (!w || wcslen(w) <= 3 || !slash || !slash[1]) ? L"Backup drive" : slash + 1, 255);
            g_label_buf[255] = 0;
            free(w);
            if (DialogBoxW(g_inst, MAKEINTRESOURCEW(IDD_LABEL), g_main, label_proc) != IDOK) { job_free(j); return; }
            j->label = narrow(g_label_buf);
            j->location = narrow(g_label_location);
            if (g_label_encrypt) {
                int ok = bd_catalog_has_passphrase(g_cat) ? unlock_ui(L"Enter the passphrase to set up an encrypted drive.")
                                                          : set_passphrase_ui();
                if (!ok) { job_free(j); return; }
                j->flags = BD_MEDIA_ENCRYPTED;
            }
        }
    }
    if (id > 0 && media_encryption(id).encrypted && !unlock_ui(L"This drive is encrypted. Enter the passphrase to back up to it.")) {
        job_free(j);
        return;
    }
    enqueue(j);
}

static void cmd_check(void)
{
    int64_t id = choose_drive(g_buttons[3], 0);
    if (id <= 0) return;
    if (media_encryption(id).encrypted && !unlock_ui(L"This drive is encrypted. Enter the passphrase to check its copies.")) return;
    job *j = new_job(JOB_CHECK);
    if (!j) return;
    j->media_id = id;
    enqueue(j);
}

static void cmd_restore(void)
{
    node_ref *r = item_ref(TreeView_GetSelection(g_tree));
    if (!r) {
        MessageBoxW(g_main, L"Select a protected folder, a folder inside it, or a file to restore.", APP_NAME, MB_ICONINFORMATION);
        return;
    }
    if (media_encryption(0).any_connected_encrypted && !unlock_ui(L"Some copies are on an encrypted drive. Enter the passphrase to restore them."))
        return;
    char *dest = pick_folder(g_main, L"Choose where to put the restored files. They go into a subfolder named after the protected folder.");
    if (!dest) return;
    job *j = new_job(JOB_RESTORE);
    if (!j) { free(dest); return; }
    j->source_id = r->source_id;
    j->rel = xstrdup(r->rel ? r->rel : "");
    j->dest = dest;
    enqueue(j);
}

/* ---- Layout and window procedure ---------------------------------------- */

static void layout(void)
{
    RECT rc;
    GetClientRect(g_main, &rc);
    SendMessageW(g_status, WM_SIZE, 0, 0);
    RECT sr;
    GetWindowRect(g_status, &sr);
    int status_h = sr.bottom - sr.top;
    int w = rc.right, h = rc.bottom - status_h;
    int pad = S(8), bar = S(30), log_h = S(110);

    int x = pad;
    int widths[N_BUTTONS] = {S(96), S(70), S(96), S(112), S(90), S(80), S(104), S(104)};
    for (int i = 0; i < N_BUTTONS; i++) {
        MoveWindow(g_buttons[i], x, pad, widths[i], bar, TRUE);
        x += widths[i] + S(6);
    }
    MoveWindow(g_drives, x + S(10), pad + S(7), w - x - S(10) - pad, bar - S(7), TRUE);

    int top = pad + bar + pad;
    int body_h = h - top - log_h - pad * 2;
    int tree_w = (w - pad * 3) * 55 / 100;
    MoveWindow(g_tree, pad, top, tree_w, body_h, TRUE);
    int rx = pad * 2 + tree_w, rw = w - rx - pad;
    int detail_h = body_h * 58 / 100;
    MoveWindow(g_detail, rx, top, rw, detail_h, TRUE);
    MoveWindow(g_list, rx, top + detail_h + S(6), rw, body_h - detail_h - S(6), TRUE);
    MoveWindow(g_log, pad, top + body_h + pad, w - pad * 2, log_h, TRUE);
}

static LRESULT tree_custom_draw(NMTVCUSTOMDRAW *cd)
{
    switch (cd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
        return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT: {
        node_ref *r = (node_ref *)cd->nmcd.lItemlParam;
        if (!r || (cd->nmcd.uItemState & CDIS_SELECTED)) return CDRF_DODEFAULT;
        HFONT font = NULL;
        switch (r->state) {
        case BD_STATE_AVAILABLE:
        case BD_STATE_PARTIAL:
            break;
        case BD_STATE_AVAILABLE_OLDER:
            cd->clrText = RGB(150, 100, 0);
            break;
        case BD_STATE_OFFLINE:
        case BD_STATE_NO_COPY:
            cd->clrText = GetSysColor(COLOR_GRAYTEXT);
            font = g_font_italic;
            break;
        case BD_STATE_BAD:
            cd->clrText = RGB(180, 30, 30);
            font = g_font_bold;
            break;
        case BD_STATE_DELETED:
            cd->clrText = GetSysColor(COLOR_GRAYTEXT);
            font = g_font_strike;
            break;
        }
        if (font) { SelectObject(cd->nmcd.hdc, font); return CDRF_NEWFONT; }
        return CDRF_DODEFAULT;
    }
    }
    return CDRF_DODEFAULT;
}

static HWND make_button(const wchar_t *text, int id)
{
    HWND b = CreateWindowExW(0, WC_BUTTONW, text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 10, 10, g_main,
                             (HMENU)(INT_PTR)id, g_inst, NULL);
    SendMessageW(b, WM_SETFONT, (WPARAM)g_font, TRUE);
    return b;
}

static void create_children(HWND hwnd)
{
    g_main = hwnd;
    g_buttons[0] = make_button(L"Add folder...", ID_BTN_ADD);
    g_buttons[1] = make_button(L"Scan", ID_BTN_SCAN);
    g_buttons[2] = make_button(L"Back up...", ID_BTN_BACKUP);
    g_buttons[3] = make_button(L"Check drive...", ID_BTN_CHECK);
    g_buttons[4] = make_button(L"Restore...", ID_BTN_RESTORE);
    g_buttons[5] = make_button(L"Drives...", ID_BTN_DRIVES);
    g_buttons[6] = make_button(L"Passphrase...", ID_BTN_SECURITY);
    g_buttons[7] = make_button(L"At risk...", ID_BTN_RISK);

    g_drives = CreateWindowExW(0, WC_STATICW, L"", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, 0, 0, 10, 10, hwnd,
                               (HMENU)ID_DRIVES, g_inst, NULL);
    SendMessageW(g_drives, WM_SETFONT, (WPARAM)g_font, TRUE);

    g_tree = CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT |
                                 TVS_SHOWSELALWAYS | TVS_FULLROWSELECT,
                             0, 0, 10, 10, hwnd, (HMENU)ID_TREE, g_inst, NULL);
    SendMessageW(g_tree, WM_SETFONT, (WPARAM)g_font, TRUE);
    TreeView_SetExtendedStyle(g_tree, TVS_EX_DOUBLEBUFFER, TVS_EX_DOUBLEBUFFER);

    g_detail = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, L"",
                               WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 0, 0, 10, 10,
                               hwnd, (HMENU)ID_DETAIL, g_inst, NULL);
    SendMessageW(g_detail, WM_SETFONT, (WPARAM)g_font, TRUE);

    g_list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_NOSORTHEADER,
                             0, 0, 10, 10, hwnd, (HMENU)ID_LIST, g_inst, NULL);
    SendMessageW(g_list, WM_SETFONT, (WPARAM)g_font, TRUE);
    ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    const wchar_t *cols[] = {L"Version", L"Where", L"State", L"Last checked", L"Size", L"Path on drive"};
    int widths[] = {90, 130, 160, 120, 80, 260};
    for (int i = 0; i < 6; i++) {
        LVCOLUMNW c;
        memset(&c, 0, sizeof(c));
        c.mask = LVCF_TEXT | LVCF_WIDTH;
        c.pszText = (LPWSTR)cols[i];
        c.cx = S(widths[i]);
        SendMessageW(g_list, LVM_INSERTCOLUMNW, i, (LPARAM)&c);
    }

    g_log = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, L"",
                            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 0, 0, 10, 10, hwnd,
                            (HMENU)ID_LOG, g_inst, NULL);
    SendMessageW(g_log, WM_SETFONT, (WPARAM)g_font, TRUE);

    g_status = CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0, hwnd,
                               (HMENU)ID_STATUS, g_inst, NULL);
    SendMessageW(g_status, WM_SETFONT, (WPARAM)g_font, TRUE);
}

static LRESULT CALLBACK main_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        create_children(hwnd);
        return 0;
    case WM_SIZE:
        layout();
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        mm->ptMinTrackSize.x = S(760);
        mm->ptMinTrackSize.y = S(480);
        return 0;
    }
    case WM_COMMAND:
        if (g_busy) return 0;
        switch (LOWORD(wp)) {
        case ID_BTN_ADD: cmd_add_folder(); break;
        case ID_BTN_SCAN: enqueue(new_job(JOB_SCAN)); break;
        case ID_BTN_DRIVES: cmd_drives(); break;
        case ID_BTN_RISK: cmd_risk(); break;
        case ID_BTN_BACKUP: cmd_backup(); break;
        case ID_BTN_CHECK: cmd_check(); break;
        case ID_BTN_RESTORE: cmd_restore(); break;
        case ID_BTN_SECURITY: cmd_security(); break;
        }
        return 0;
    case WM_NOTIFY: {
        NMHDR *nh = (NMHDR *)lp;
        if (nh->idFrom == ID_TREE) {
            if (nh->code == NM_CUSTOMDRAW) return tree_custom_draw((NMTVCUSTOMDRAW *)lp);
            if (nh->code == TVN_ITEMEXPANDINGW) {
                NMTREEVIEWW *tv = (NMTREEVIEWW *)lp;
                node_ref *r = (node_ref *)tv->itemNew.lParam;
                if (tv->action == TVE_EXPAND && r && !r->loaded) {
                    if (g_busy) return TRUE; /* the catalog is in use; try again when the job ends */
                    load_children(tv->itemNew.hItem, r);
                }
                return FALSE;
            }
            if (nh->code == TVN_SELCHANGEDW) {
                if (!g_busy) show_detail();
                return 0;
            }
            if (nh->code == TVN_DELETEITEMW) {
                NMTREEVIEWW *tv = (NMTREEVIEWW *)lp;
                node_ref *r = (node_ref *)tv->itemOld.lParam;
                if (r) { free(r->rel); free(r); }
                return 0;
            }
        }
        return 0;
    }
    case WM_APP_LOG: {
        wchar_t *text = (wchar_t *)lp;
        log_append(text);
        free(text);
        return 0;
    }
    case WM_APP_OPEN_URL: {
        wchar_t *url = (wchar_t *)lp;
        ShellExecuteW(g_main, L"open", url, NULL, NULL, SW_SHOWNORMAL);
        log_append(L"Sign in with your browser. If it did not open, visit:");
        log_append(url);
        free(url);
        return 0;
    }
    case WM_APP_PROGRESS: {
        wchar_t *text = (wchar_t *)lp;
        SendMessageW(g_status, SB_SETTEXTW, 0, (LPARAM)text);
        free(text);
        return 0;
    }
    case WM_APP_DONE: {
        job *j = (job *)lp;
        set_busy(0);
        SendMessageW(g_status, SB_SETTEXTW, 0, (LPARAM)j->summary);
        log_append(j->summary);
        if (j->status == BD_ERR_PASSPHRASE && (j->kind == JOB_BACKUP || j->kind == JOB_CHECK) &&
            unlock_ui(L"This drive is encrypted. Enter the passphrase to continue.")) {
            job *again = new_job(j->kind);
            if (again) {
                again->media_id = j->media_id;
                again->root = xstrdup(j->root);
                again->label = xstrdup(j->label);
                again->location = xstrdup(j->location);
                again->flags = j->flags;
                again->provider = j->provider;
                enqueue(again);
            }
        } else if (j->status != BD_OK && j->status != BD_ERR_PASSPHRASE)
            report_error(hwnd, j->summary);
        job_free(j);
        update_drives_label();
        rebuild_tree();
        show_detail();
        start_next_job();
        return 0;
    }
    case WM_DEVICECHANGE:
        if (wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE) {
            DEV_BROADCAST_HDR *hdr = (DEV_BROADCAST_HDR *)lp;
            if (hdr && hdr->dbch_devicetype == DBT_DEVTYP_VOLUME) enqueue(new_job(JOB_DRIVES));
        }
        return TRUE;
    case WM_CLOSE:
        if (g_busy) {
            MessageBoxW(hwnd, L"BRODALF is still working. Close it when the status bar says the job is done.", APP_NAME, MB_ICONINFORMATION);
            return 0;
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- Startup: pick or create a catalog ---------------------------------- */

static void reg_get_last(wchar_t *out, DWORD cap)
{
    out[0] = 0;
    DWORD bytes = cap * sizeof(wchar_t);
    if (RegGetValueW(HKEY_CURRENT_USER, REG_KEY, L"LastCatalog", RRF_RT_REG_SZ, NULL, out, &bytes) != ERROR_SUCCESS) out[0] = 0;
}

static void reg_set_last(const wchar_t *path)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(k, L"LastCatalog", 0, REG_SZ, (const BYTE *)path, (DWORD)((wcslen(path) + 1) * sizeof(wchar_t)));
        RegCloseKey(k);
    }
}

static int file_dialog(int save, wchar_t *out, DWORD cap)
{
    OPENFILENAMEW of;
    memset(&of, 0, sizeof(of));
    of.lStructSize = sizeof(of);
    of.lpstrFilter = L"BRODALF catalogs (*.brodalf)\0*.brodalf\0All files\0*.*\0";
    of.lpstrFile = out;
    of.nMaxFile = cap;
    of.lpstrDefExt = L"brodalf";
    if (save) {
        if (!out[0]) wcscpy(out, L"My files.brodalf");
        of.lpstrTitle = L"Create a new BRODALF catalog";
        of.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        return GetSaveFileNameW(&of);
    }
    out[0] = 0;
    of.lpstrTitle = L"Open a BRODALF catalog";
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&of);
}

enum { CHOICE_LAST = 100, CHOICE_OPEN, CHOICE_NEW };

static int ask_catalog_choice(const wchar_t *last)
{
    wchar_t last_btn[MAX_PATH * 2 + 64];
    TASKDIALOG_BUTTON buttons[3];
    int n = 0;
    if (last[0]) {
        swprintf(last_btn, MAX_PATH * 2 + 64, L"Open the last catalog\n%ls", last);
        buttons[n].nButtonID = CHOICE_LAST;
        buttons[n++].pszButtonText = last_btn;
    }
    buttons[n].nButtonID = CHOICE_OPEN;
    buttons[n++].pszButtonText = L"Open a catalog...\nA .brodalf file you made before";
    buttons[n].nButtonID = CHOICE_NEW;
    buttons[n++].pszButtonText = L"Create a new catalog...\nStart tracking a set of folders";

    TASKDIALOGCONFIG tc;
    memset(&tc, 0, sizeof(tc));
    tc.cbSize = sizeof(tc);
    tc.hInstance = g_inst;
    tc.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
    tc.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    tc.pszWindowTitle = APP_NAME;
    tc.pszMainInstruction = L"Which catalog do you want to use?";
    tc.pszContent = L"A catalog records every file you protect and every drive that holds a copy. It never holds the files themselves.";
    tc.cButtons = (UINT)n;
    tc.pButtons = buttons;
    int pressed = IDCANCEL;
    if (FAILED(TaskDialogIndirect(&tc, &pressed, NULL, NULL))) return IDCANCEL;
    return pressed;
}

static int ask_remove_lock(const wchar_t *path)
{
    wchar_t msg[MAX_PATH * 3];
    swprintf(msg, MAX_PATH * 3,
             L"%ls is marked as open in another BRODALF window.\n\nIf no other BRODALF window is open (for example after a crash), you can remove the mark and open it.",
             path);
    return MessageBoxW(NULL, msg, APP_NAME, MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) == IDYES;
}

/* Open a catalog, asking for the passphrase if the file is encrypted.
 * A cancelled prompt returns BD_ERR_PASSPHRASE with no message. */
static bd_status open_maybe_encrypted(const char *path)
{
    if (!bd_catalog_file_needs_passphrase(path)) return bd_catalog_open(path, &g_cat);
    for (;;) {
        char *p = ask_passphrase(NULL, IDD_PASSPHRASE, L"This catalog is encrypted. Enter its passphrase to open it.");
        if (!p) return BD_ERR_PASSPHRASE;
        bd_status s = bd_catalog_open_with(path, p, &g_cat);
        free_secret(p);
        if (s != BD_ERR_PASSPHRASE) return s;
        MessageBoxW(NULL, L"That passphrase is not right. Try again.", APP_NAME, MB_ICONWARNING);
    }
}

/* given: a catalog named on the command line (double-click), tried first. */
static int open_catalog(const wchar_t *given)
{
    wchar_t last[MAX_PATH * 2];
    reg_get_last(last, MAX_PATH * 2);
    if (last[0] && GetFileAttributesW(last) == INVALID_FILE_ATTRIBUTES) last[0] = 0;

    for (;;) {
        int choice = given ? CHOICE_LAST : ask_catalog_choice(last);
        wchar_t path[MAX_PATH * 2] = L"";
        int create = 0;
        const wchar_t *first = given;
        given = NULL;
        if (choice == CHOICE_LAST) wcsncpy(path, first ? first : last, MAX_PATH * 2 - 1);
        else if (choice == CHOICE_OPEN) { if (!file_dialog(0, path, MAX_PATH * 2)) continue; }
        else if (choice == CHOICE_NEW) { create = 1; if (!file_dialog(1, path, MAX_PATH * 2)) continue; }
        else return 0;

        char *p = narrow(path);
        bd_status s;
        if (create) {
            if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) DeleteFileW(path); /* the save dialog asked to replace it */
            s = bd_catalog_create(p, &g_cat);
        } else {
            s = open_maybe_encrypted(p);
            if (s == BD_ERR_LOCKED && ask_remove_lock(path)) {
                wchar_t lock[MAX_PATH * 2 + 8];
                swprintf(lock, MAX_PATH * 2 + 8, L"%ls.lock", path);
                DeleteFileW(lock);
                s = open_maybe_encrypted(p);
            } else if (s == BD_ERR_LOCKED || s == BD_ERR_PASSPHRASE) { /* declined */
                free(p);
                continue;
            }
        }
        free(p);
        if (s == BD_OK) {
            wcscpy(g_cat_path, path);
            reg_set_last(path);
            return 1;
        }
        wchar_t *e = widen(bd_open_error());
        const wchar_t *why = e && e[0] ? e : L"The catalog could not be opened.";
        /* Picking a file that is not a catalog is not a bug. */
        if (s == BD_ERR_FORMAT || s == BD_ERR_NOT_FOUND || s == BD_ERR_EXISTS) MessageBoxW(NULL, why, APP_NAME, MB_ICONERROR);
        else report_error(NULL, why);
        free(e);
    }
}

typedef struct { int n; } count_ctx;
static int count_source(void *ctx, const bd_source_info *info) { (void)info; ((count_ctx *)ctx)->n++; return 0; }

static void first_run_folders(void)
{
    count_ctx c = {0};
    bd_list_sources(g_cat, count_source, &c);
    if (c.n > 0) return;
    MessageBoxW(g_main, L"Next, choose the folders you want to back up. You can add more later with Add folder.", APP_NAME, MB_ICONINFORMATION);
    for (;;) {
        char *folder = pick_folder(g_main, L"Choose a folder to protect.");
        if (!folder) break;
        int64_t id;
        if (bd_source_add(g_cat, folder, &id) != BD_OK) {
            wchar_t *e = widen(bd_catalog_error(g_cat));
            MessageBoxW(g_main, e, APP_NAME, MB_ICONWARNING);
            free(e);
        }
        free(folder);
        if (MessageBoxW(g_main, L"Add another folder?", APP_NAME, MB_YESNO | MB_ICONQUESTION) != IDYES) break;
    }
}

static void make_fonts(void)
{
    NONCLIENTMETRICSW ncm;
    memset(&ncm, 0, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    LOGFONTW lf = ncm.lfMessageFont;
    g_font = CreateFontIndirectW(&lf);
    lf.lfItalic = TRUE;
    g_font_italic = CreateFontIndirectW(&lf);
    lf.lfItalic = FALSE;
    lf.lfStrikeOut = TRUE;
    g_font_strike = CreateFontIndirectW(&lf);
    lf.lfStrikeOut = FALSE;
    lf.lfWeight = FW_BOLD;
    g_font_bold = CreateFontIndirectW(&lf);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    (void)prev;
    (void)cmd;
    g_inst = inst;
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_TREEVIEW_CLASSES | ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    HDC screen = GetDC(NULL);
    g_dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(NULL, screen);
    make_fonts();

    char *log_path = bd_applog_default_path();
    bd_applog_open(log_path);
    free(log_path);
    bd_applog("BRODALF %s started", bd_version());
    SetUnhandledExceptionFilter(on_crash);

    int opened = open_catalog(argc > 1 && argv[1][0] ? argv[1] : NULL);
    LocalFree(argv);
    if (!opened) return 0;

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = main_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    wc.lpszClassName = L"BrodalfMain";
    RegisterClassExW(&wc);

    wchar_t title[MAX_PATH * 2 + 32];
    const wchar_t *base = wcsrchr(g_cat_path, L'\\');
    swprintf(title, MAX_PATH * 2 + 32, L"%ls - BRODALF", base ? base + 1 : g_cat_path);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, S(1180), S(760),
                                NULL, NULL, inst, NULL);
    if (!hwnd) return 1;
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    first_run_folders();
    update_drives_label();
    rebuild_tree();
    show_detail();
    log_append(L"Catalog opened. Looking for backup drives, then checking your folders for changes.");
    enqueue(new_job(JOB_DRIVES));
    enqueue(new_job(JOB_SCAN));

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    bd_catalog_save(g_cat);
    bd_catalog_close(g_cat);
    CoUninitialize();
    return 0;
}
