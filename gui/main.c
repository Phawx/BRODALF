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
#define N_BUTTONS 6
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
            if (s == BD_OK) { free(j->label); j->label = NULL; j->media_id = id; } /* a retry must not set it up again */
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
        post_text(WM_APP_LOG, L"Could not save the catalog:");
        if (e) { post_text(WM_APP_LOG, e); free(e); }
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

static void show_detail(void)
{
    SendMessageW(g_list, LVM_DELETEALLITEMS, 0, 0);
    node_ref *r = item_ref(TreeView_GetSelection(g_tree));
    if (!r) { SetWindowTextW(g_detail, L"Select a file to see its versions and where each copy is."); return; }
    wchar_t *rel = widen(r->rel && *r->rel ? r->rel : "(whole folder)");
    wchar_t text[1200];
    if (r->is_dir) {
        swprintf(text, 1200, L"%ls\r\n%ls", rel, state_words(r->state));
    } else {
        swprintf(text, 1200, L"%ls\r\n%ls", rel, state_words(r->state));
        int row = 0;
        bd_list_copies(g_cat, r->node_id, add_copy_row, &row);
    }
    SetWindowTextW(g_detail, text);
    free(rel);
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

static INT_PTR CALLBACK label_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG:
        SetDlgItemTextW(dlg, IDC_LABEL_EDIT, g_label_buf);
        if (g_label_cloud) {
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
        MessageBoxW(g_main, e ? e : L"Could not set the passphrase.", APP_NAME, MB_ICONERROR);
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
            MessageBoxW(g_main, e ? e : L"Could not change it.", APP_NAME, MB_ICONERROR);
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
    int widths[N_BUTTONS] = {S(96), S(70), S(96), S(112), S(90), S(104)};
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
    MoveWindow(g_detail, rx, top, rw, S(44), TRUE);
    MoveWindow(g_list, rx, top + S(48), rw, body_h - S(48), TRUE);
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
    g_buttons[5] = make_button(L"Passphrase...", ID_BTN_SECURITY);

    g_drives = CreateWindowExW(0, WC_STATICW, L"", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, 0, 0, 10, 10, hwnd,
                               (HMENU)ID_DRIVES, g_inst, NULL);
    SendMessageW(g_drives, WM_SETFONT, (WPARAM)g_font, TRUE);

    g_tree = CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT |
                                 TVS_SHOWSELALWAYS | TVS_FULLROWSELECT,
                             0, 0, 10, 10, hwnd, (HMENU)ID_TREE, g_inst, NULL);
    SendMessageW(g_tree, WM_SETFONT, (WPARAM)g_font, TRUE);
    TreeView_SetExtendedStyle(g_tree, TVS_EX_DOUBLEBUFFER, TVS_EX_DOUBLEBUFFER);

    g_detail = CreateWindowExW(0, WC_STATICW, L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 0, 0, 10, 10, hwnd,
                               (HMENU)ID_DETAIL, g_inst, NULL);
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
                again->flags = j->flags;
                again->provider = j->provider;
                enqueue(again);
            }
        }
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
        MessageBoxW(NULL, e && e[0] ? e : L"The catalog could not be opened.", APP_NAME, MB_ICONERROR);
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
