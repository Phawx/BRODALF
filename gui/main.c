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
    ID_SEARCH,
    ID_RESULTS,
    ID_PROGRESS,
    ID_PROGTEXT,
    /* The menu bar. */
    ID_M_ADD_FOLDER = 700,
    ID_M_SCAN,
    ID_M_RESTORE,
    ID_M_RESTORE_ASOF,
    ID_M_RISK,
    ID_M_EXIT,
    ID_M_ADD_DISK,
    ID_M_DRIVES,
    ID_M_SETTINGS,
    ID_M_SCHED_DAILY,
    ID_M_SCHED_WEEKLY,
    ID_M_SCHED_NEVER,
    ID_M_VERIFY_7,
    ID_M_VERIFY_30,
    ID_M_VERIFY_90,
    ID_M_VERIFY_NEVER,
    ID_M_GUARD_10,
    ID_M_GUARD_25,
    ID_M_GUARD_50,
    ID_M_GUARD_NEVER,
    ID_M_GUARD_STATUS,
    ID_M_IN_USE,
    ID_M_LOG,
    ID_M_REPORTS,
    ID_M_ABOUT,
    ID_MENU_SET_PASS = 910,
    ID_MENU_ENCRYPT_CATALOG,
    ID_MENU_LOCK,
    ID_MENU_AUTO_BACKUP,
    ID_MENU_CHECK_90,
    ID_MENU_CHECK_180,
    ID_MENU_CHECK_365,
    ID_MENU_CHECK_NEVER,
    ID_MENU_KEEP_0,
    ID_MENU_KEEP_1,
    ID_MENU_KEEP_2,
    ID_MENU_KEEP_3,
    ID_MENU_KEEP_4,
    ID_MENU_SKIP,
    ID_MENU_DRIVE_BASE = 1000, /* + media id, for the drive popup menus */
    ID_DRIVE_ACT_BASE = 20000, /* + media id * 8 + action, for the Local backups menu */
    ID_MENU_OTHER = 900
};

enum {
    WM_APP_LOG = WM_APP + 1,  /* lParam: malloc'd wide string */
    WM_APP_PROGRESS,          /* lParam: malloc'd prog_msg */
    WM_APP_DONE               /* lParam: job* */
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
static HWND g_main, g_tree, g_list, g_log, g_status, g_detail, g_drives, g_search, g_results, g_progress, g_prog_text;
static HMENU g_menu_local;
#define N_BUTTONS 8
static HWND g_buttons[N_BUTTONS];
static HFONT g_font, g_font_italic, g_font_strike, g_font_bold;
static int g_dpi = 96;
static bd_catalog *g_cat;
static wchar_t g_cat_path[MAX_PATH * 2];
static volatile LONG g_busy;

static int S(int px) { return MulDiv(px, g_dpi, 96); }

/* ---- Jobs --------------------------------------------------------------- */

typedef enum { JOB_DRIVES, JOB_SCAN, JOB_BACKUP, JOB_CHECK, JOB_RESTORE, JOB_VERIFY, JOB_SHADOW } job_kind;

typedef struct job {
    job_kind kind;
    int64_t media_id;   /* backup/check: a connected drive, or 0 with root */
    char *root;         /* backup to a drive given by folder */
    char *label;        /* set up a new drive with this name first */
    char *location;     /* and say where it is kept */
    unsigned flags;     /* for the new drive: BD_MEDIA_ENCRYPTED */
    int64_t source_id;  /* restore */
    char *rel;          /* restore */
    char *dest;         /* restore */
    int auto_run;       /* drives: a drive was plugged in; backup: started by that */
    int startup;        /* drives: the first look when BRODALF opens */
    int64_t new_ids[16]; /* drives: drives connected by this job */
    int n_new;
    int64_t span[BD_MAX_CONTINUE]; /* backup: only what is missing from these full drives */
    int64_t no_room, bytes_no_room; /* backup: what did not fit */
    int64_t offline;    /* restore: files on drives not plugged in */
    int64_t as_of;      /* restore: the files as they were then (0: current) */
    int64_t in_use;     /* scan/backup: files another program had open */
    int guard;          /* scan: this scan paused backups */
    int from_shadow;    /* the last job using shadow copies: clean them up after */
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

/* What the progress bar shows: percent (-1: unknown, keep it moving) and a
 * line with the counts, the data rate and the file being worked on. */
typedef struct { int percent; wchar_t text[1024]; } prog_msg;

/* The data rate over the last few seconds: a ring of (time, bytes). */
#define RATE_SAMPLES 16
static struct { int64_t t[RATE_SAMPLES], b[RATE_SAMPLES]; int n, head; } g_rate;

static void rate_reset(void) { memset(&g_rate, 0, sizeof(g_rate)); }

static double rate_add(int64_t bytes)
{
    int64_t now = GetTickCount64();
    g_rate.t[g_rate.head] = now;
    g_rate.b[g_rate.head] = bytes;
    g_rate.head = (g_rate.head + 1) % RATE_SAMPLES;
    if (g_rate.n < RATE_SAMPLES) g_rate.n++;
    int oldest = g_rate.n < RATE_SAMPLES ? 0 : g_rate.head;
    int64_t dt = now - g_rate.t[oldest], db = bytes - g_rate.b[oldest];
    return dt >= 500 && db >= 0 ? (double)db * 1000.0 / (double)dt : -1.0;
}

static void job_progress(void *ctx, const bd_progress *p)
{
    (void)ctx;
    if (p->files_done == 0 && p->bytes_done == 0) rate_reset();
    prog_msg *m = calloc(1, sizeof(*m));
    if (!m) return;
    const char *ph = p->phase;
    const wchar_t *verb = strcmp(ph, "scan") == 0 ? L"Scanning" : strcmp(ph, "backup") == 0 ? L"Backing up"
                        : strcmp(ph, "restore") == 0 ? L"Restoring" : strcmp(ph, "verify") == 0 ? L"Verifying copies"
                        : L"Checking copies";
    const wchar_t *unit = strcmp(ph, "scan") == 0 ? L"files looked at" : strcmp(ph, "backup") == 0 ? L"files copied"
                        : strcmp(ph, "restore") == 0 ? L"files restored" : L"copies read";
    wchar_t done[64], total[64], rate[64] = L"", *c = widen(p->current);
    format_bytes(p->bytes_done, done, 64);
    double r = p->done ? -1.0 : rate_add(p->bytes_done);
    if (r >= 0) { format_bytes((int64_t)r, rate, 64); wcsncat(rate, L"/s", 63 - wcslen(rate)); }
    if (p->bytes_total > 0) {
        format_bytes(p->bytes_total, total, 64);
        m->percent = (int)(p->bytes_done * 100 / p->bytes_total);
        swprintf(m->text, 1024, L"%ls: %lld of %lld %ls, %ls of %ls%ls%ls. %ls", verb, (long long)p->files_done,
                 (long long)p->files_total, unit, done, total, *rate ? L", " : L"", rate, c ? c : L"");
    } else if (p->files_total > 0) {
        m->percent = (int)(p->files_done * 100 / p->files_total);
        swprintf(m->text, 1024, L"%ls: %lld of about %lld %ls, %ls read%ls%ls. %ls", verb, (long long)p->files_done,
                 (long long)p->files_total, unit, done, *rate ? L" at " : L"", rate, c ? c : L"");
    } else {
        m->percent = -1;
        swprintf(m->text, 1024, L"%ls: %lld %ls, %ls read%ls%ls. %ls", verb, (long long)p->files_done, unit, done,
                 *rate ? L" at " : L"", rate, c ? c : L"");
    }
    if (p->done) m->percent = 100;
    if (m->percent > 100) m->percent = 100;
    free(c);
    if (!PostMessageW(g_main, WM_APP_PROGRESS, 0, (LPARAM)m)) free(m);
}

/* The bar and its line when no job runs. */
static void show_progress(int percent, const wchar_t *text)
{
    LONG_PTR style = GetWindowLongPtrW(g_progress, GWL_STYLE);
    if (percent < 0) {
        if (!(style & PBS_MARQUEE)) SetWindowLongPtrW(g_progress, GWL_STYLE, style | PBS_MARQUEE);
        SendMessageW(g_progress, PBM_SETMARQUEE, TRUE, 50);
    } else {
        if (style & PBS_MARQUEE) {
            SendMessageW(g_progress, PBM_SETMARQUEE, FALSE, 0);
            SetWindowLongPtrW(g_progress, GWL_STYLE, style & ~PBS_MARQUEE);
        }
        SendMessageW(g_progress, PBM_SETRANGE32, 0, 100);
        SendMessageW(g_progress, PBM_SETPOS, percent, 0);
    }
    SetWindowTextW(g_prog_text, text);
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

typedef struct { int64_t ids[64]; int n; } id_list;

static int connected_drive_cb(void *ctx, const bd_media_info *m)
{
    id_list *l = ctx;
    if (m->connected && strcmp(m->kind, "drive") == 0 && l->n < 64) l->ids[l->n++] = m->media_id;
    return 0;
}

static int id_in(const id_list *l, int64_t id)
{
    for (int i = 0; i < l->n; i++)
        if (l->ids[i] == id) return 1;
    return 0;
}

static void run_drives(job *j)
{
    int found = 0;
    id_list before = {{0}, 0}, after = {{0}, 0};
    bd_list_media(g_cat, connected_drive_cb, &before);
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
    bd_list_media(g_cat, connected_drive_cb, &after);
    for (int i = 0; i < after.n && j->n_new < 16; i++)
        if (!id_in(&before, after.ids[i])) j->new_ids[j->n_new++] = after.ids[i];
    swprintf(j->summary, 512, found ? L"Found %d BRODALF drive(s)." : L"No BRODALF drives are plugged in.", found);
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

static bd_status run_shadow(job *j);
static void shadow_cleanup(void);

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
        if (st.skipped) {
            size_t len = wcslen(j->summary);
            swprintf(j->summary + len, 512 - len, L" %lld left out by the skip list.", (long long)st.skipped);
        }
        if (st.files_in_use) {
            size_t len = wcslen(j->summary);
            swprintf(j->summary + len, 512 - len, L" %lld in use by another program.", (long long)st.files_in_use);
        }
        j->in_use = st.files_in_use;
        j->guard = st.guard_tripped;
        break;
    }
    case JOB_BACKUP: {
        int64_t id = j->media_id;
        if (j->label) {
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
            bd_backup_opts bo;
            memset(&bo, 0, sizeof(bo));
            memcpy(bo.only_missing_from, j->span, sizeof(bo.only_missing_from));
            s = bd_backup_ex(g_cat, id, 0, &bo, &bs, job_log, j);
            if (s == BD_OK) s = bd_catalog_copy_to_media(g_cat, id);
            format_bytes(bs.bytes_copied, size, 64);
            swprintf(j->summary, 512, L"Backup done: %lld files copied (%ls), %lld already there, %lld failed, %lld older copies kept.",
                     (long long)bs.files_copied, size, (long long)bs.files_already_there, (long long)bs.files_failed,
                     (long long)bs.versions_moved);
            size_t len = wcslen(j->summary);
            if (bs.versions_pruned) {
                swprintf(j->summary + len, 512 - len, L" %lld old versions cleaned up.", (long long)bs.versions_pruned);
                len = wcslen(j->summary);
            }
            if (bs.files_moved) {
                swprintf(j->summary + len, 512 - len, L" %lld moved or renamed files were moved on the drive instead.", (long long)bs.files_moved);
                len = wcslen(j->summary);
            }
            if (bs.files_in_use) {
                swprintf(j->summary + len, 512 - len, L" %lld in use by another program.", (long long)bs.files_in_use);
                len = wcslen(j->summary);
            }
            if (bs.files_no_room) {
                format_bytes(bs.bytes_no_room, size, 64);
                swprintf(j->summary + len, 512 - len, L" The drive is full: %lld file%ls (%ls) did not fit.", (long long)bs.files_no_room,
                         bs.files_no_room == 1 ? L"" : L"s", size);
            }
            j->no_room = bs.files_no_room;
            j->bytes_no_room = bs.bytes_no_room;
            j->in_use = bs.files_in_use;
        }
        break;
    }
    case JOB_VERIFY: {
        bd_check_stats cs;
        int days = bd_option_get(g_cat, "verify_days");
        s = bd_media_verify(g_cat, j->media_id, days, &cs, job_log, j);
        if (cs.rehashed == 0 && s == BD_OK)
            swprintf(j->summary, 512, L"Verified: every copy on the drive was read back within the last %d days.", days);
        else
            swprintf(j->summary, 512, L"Verified: read back %lld copies, %lld good, %lld damaged, %lld missing.%ls",
                     (long long)cs.rehashed, (long long)cs.ok, (long long)cs.bad, (long long)cs.missing,
                     cs.bad || cs.missing ? L" The next backup to this drive writes them again." : L"");
        break;
    }
    case JOB_SHADOW:
        s = run_shadow(j);
        break;
    case JOB_CHECK: {
        bd_check_stats cs;
        s = bd_media_check(g_cat, j->media_id, 1, &cs, job_log, j);
        swprintf(j->summary, 512, L"Full check done: %lld copies, %lld good, %lld missing, %lld damaged.",
                 (long long)cs.copies, (long long)cs.ok, (long long)cs.missing, (long long)cs.bad);
        break;
    }
    case JOB_RESTORE: {
        bd_restore_stats rs;
        bd_restore_opts ro = {j->as_of};
        s = bd_restore_ex(g_cat, j->source_id, j->rel, j->dest, &ro, &rs, job_log, j);
        format_bytes(rs.bytes_restored, size, 64);
        swprintf(j->summary, 512, L"Restore done: %lld files (%ls), %lld already there, %lld on drives that are not plugged in, %lld never backed up, %lld failed.",
                 (long long)rs.files_restored, size, (long long)rs.files_already_there, (long long)rs.files_offline,
                 (long long)rs.files_no_copy, (long long)rs.files_failed);
        j->offline = rs.files_offline;
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
    if (j->from_shadow) shadow_cleanup();
    if (bd_catalog_save(g_cat) != BD_OK) {
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
    HMENU bar = GetMenu(g_main);
    if (bar) {
        for (int i = 0; i < GetMenuItemCount(bar); i++) EnableMenuItem(bar, (UINT)i, MF_BYPOSITION | (busy ? MF_GRAYED : MF_ENABLED));
        DrawMenuBar(g_main);
    }
}

static void start_next_job(void)
{
    if (g_busy || !g_queue) return;
    job *j = g_queue;
    g_queue = j->next;
    j->next = NULL;
    set_busy(1);
    static const wchar_t *starting[] = {L"Looking for BRODALF drives...", L"Scanning your folders...",
                                        L"Backing up...", L"Checking every copy on the drive...", L"Restoring...",
                                        L"Reading back the copies on the drive...", L"Reading files that are in use from a shadow copy..."};
    SendMessageW(g_status, SB_SETTEXTW, 0, (LPARAM)starting[j->kind]);
    show_progress(-1, starting[j->kind]);
    HANDLE h = CreateThread(NULL, 0, worker, j, 0, NULL);
    if (h) CloseHandle(h);
    else { set_busy(0); job_free(j); }
}

static void enqueue(job *j)
{
    /* Don't stack up drive scans; one pending is enough. */
    if (j->kind == JOB_DRIVES)
        for (job *q = g_queue; q; q = q->next)
            if (q->kind == JOB_DRIVES) { q->auto_run |= j->auto_run; job_free(j); return; }
    /* Likewise folder scans: one waiting to run covers this one. */
    if (j->kind == JOB_SCAN)
        for (job *q = g_queue; q; q = q->next)
            if (q->kind == JOB_SCAN) { job_free(j); return; }
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
        wchar_t bytes[64];
        format_bytes(l.items[i].bytes_total, bytes, 64);
        swprintf(text, 1024, L"%ls   %ls   [%ls]", counts, bytes, wp);
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
    if (strcmp(d->kind, "drive") != 0) return; /* a cloud account from an earlier version: the heading says so */
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
    if (strcmp(d->kind, "drive") != 0) wadd(t, L"%ls    Cloud account from an earlier version (not supported in this version)\r\n", indent);
    else if (d->connected) wadd(t, L"%ls    Plugged in now at %ls\r\n", indent, root);
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
        if (strcmp(d->kind, "drive") != 0) wcsncat(text, L"  - Cloud account from an earlier version (not supported in this version)", 599 - wcslen(text));
        else if (d->connected) wcsncat(text, L"  - plugged in", 599 - wcslen(text));
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
    if (strcmp(m->kind, "drive") != 0) return 0; /* a cloud account from an earlier version is not a drive to plug in */
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
    bd_guard_info g;
    if (bd_guard_get(g_cat, &g) == BD_OK && g.tripped) {
        wchar_t paused[1100];
        swprintf(paused, 1100, L"BACKUPS PAUSED (ransomware guard, see Settings).  %ls", d.text);
        wcscpy(d.text, paused);
    }
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
    if (strcmp(h->kind, "drive") != 0) return 0; /* a cloud account from an earlier version: nothing can be backed up to it */
    wchar_t *label = widen(h->label), *loc = widen(h->location), name[300], files[32], size[64];
    swprintf(name, 300, L"%ls%ls", label ? label : L"", h->connected ? L"  (plugged in)" : L"");
    swprintf(files, 32, L"%lld file%ls", (long long)h->files, h->files == 1 ? L"" : L"s");
    format_bytes(h->bytes, size, 64);
    const wchar_t *kind = L"Drive";
    wchar_t room[96] = L"unknown";
    if (h->free_bytes >= 0) {
        wchar_t fb[64];
        format_bytes(h->free_bytes, fb, 64);
        swprintf(room, 96, L"%ls free%ls", fb, h->fits ? L", room for all of it" : L", not enough");
    }
    const wchar_t *cells[] = {name, kind, loc && *loc ? loc : L"(not set)", files, size, room};
    lv_row(f->lv, cells, 6);
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
        static const wchar_t *help_cols[] = {L"Drive", L"Kind", L"Kept in", L"Would help", L"Size", L"Free space"};
        static const int help_w[] = {150, 60, 100, 80, 70, 190};
        static const wchar_t *file_cols[] = {L"File", L"Copies", L"Places", L"Size", L"Note"};
        static const int file_w[] = {280, 56, 56, 70, 200};
        lv_columns(GetDlgItem(dlg, IDC_RISK_HELP), help_cols, help_w, 6);
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
static wchar_t g_label_location[256];

static INT_PTR CALLBACK label_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG:
        SetDlgItemTextW(dlg, IDC_LABEL_EDIT, g_label_buf);
        g_label_location[0] = 0;
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

/* ---- Plug-in backups and check reminders --------------------------------- */

typedef struct { int64_t id; bd_media_info info; char label[256], location[256]; int found; } media_one;

static int media_one_cb(void *ctx, const bd_media_info *m)
{
    media_one *o = ctx;
    if (m->media_id != o->id) return 0;
    o->info = *m;
    snprintf(o->label, sizeof(o->label), "%s", m->label);
    snprintf(o->location, sizeof(o->location), "%s", m->location ? m->location : "");
    o->found = 1;
    return 1;
}

/* "Blue WD 4TB (kept in Box A)" */
static void drive_name(const char *label, const char *location, wchar_t *out, size_t cap)
{
    wchar_t *l = widen(label), *loc = widen(location);
    if (loc && *loc) swprintf(out, cap, L"%ls (kept in %ls)", l ? l : L"", loc);
    else swprintf(out, cap, L"%ls", l ? l : L"");
    free(l);
    free(loc);
}

static void enqueue_check(int64_t id)
{
    if (media_encryption(id).encrypted && !unlock_ui(L"This drive is encrypted. Enter the passphrase to check its copies.")) return;
    job *c = new_job(JOB_CHECK);
    if (!c) return;
    c->media_id = id;
    enqueue(c);
}

/* A drive that is plugged in and due for a full check: offer one. */
static void offer_check(int64_t id)
{
    media_one o;
    memset(&o, 0, sizeof(o));
    o.id = id;
    bd_list_media(g_cat, media_one_cb, &o);
    if (!o.found || !o.info.connected || !o.info.check_due) return;
    wchar_t name[600], since[64], msg[1200];
    drive_name(o.label, o.location, name, 600);
    format_time_ms(o.info.oldest_check_ms, since, 64);
    swprintf(msg, 1200,
             L"Some copies on %ls have not been read back since %ls.\n\n"
             L"A full check reads every copy on the drive to catch slow damage while other copies still exist. "
             L"It can take a while on a big drive.\n\nCheck it now?",
             name, since);
    if (MessageBoxW(g_main, msg, APP_NAME, MB_ICONQUESTION | MB_YESNO) == IDYES) enqueue_check(id);
}

typedef struct { wtext text; int64_t connected[64]; int n_connected, n; } due_list;

static int due_cb(void *ctx, const bd_media_info *m)
{
    due_list *d = ctx;
    if (!m->check_due || strcmp(m->kind, "drive") != 0) return 0; /* a cloud account from an earlier version cannot be checked */
    wchar_t name[600], since[64];
    drive_name(m->label, m->location, name, 600);
    format_time_ms(m->oldest_check_ms, since, 64);
    wadd(&d->text, L"    %ls: not read back since %ls%ls\n", name, since, m->connected ? L" (plugged in)" : L"");
    if (m->connected && d->n_connected < 64) d->connected[d->n_connected++] = m->media_id;
    d->n++;
    return 0;
}

/* When BRODALF opens: list the drives due for a full check. */
static void remind_checks(void)
{
    due_list d;
    memset(&d, 0, sizeof(d));
    bd_list_media(g_cat, due_cb, &d);
    if (!d.n) return;
    wtext msg = {NULL, 0, 0};
    wadd(&msg, L"%ls due for a full check, to catch slow damage to old copies:\n\n%ls\n",
         d.n == 1 ? L"This drive is" : L"These drives are", d.text.p);
    if (d.n_connected) {
        wadd(&msg, L"Check the plugged-in %ls now?", d.n_connected == 1 ? L"one" : L"ones");
        if (MessageBoxW(g_main, msg.p, APP_NAME, MB_ICONQUESTION | MB_YESNO) == IDYES)
            for (int i = 0; i < d.n_connected; i++) enqueue_check(d.connected[i]);
    } else {
        wadd(&msg, L"Plug each one in and BRODALF will offer to check it. You can change how often under Settings.");
        MessageBoxW(g_main, msg.p, APP_NAME, MB_ICONINFORMATION);
    }
    free(msg.p);
    free(d.text.p);
}

/* ---- Full drives and restores across drives ---------------------------- */

/* Drives that filled up, in order, and what is still waiting for room. */
static int64_t g_full[BD_MAX_CONTINUE];
static int g_full_n;
static int64_t g_full_left, g_full_left_bytes;

/* A restore that is waiting for more drives. */
static struct { int active; int64_t source_id; char *rel, *dest; } g_pending;

static int in_full_chain(int64_t id)
{
    for (int i = 0; i < g_full_n; i++)
        if (g_full[i] == id) return 1;
    return 0;
}

static void media_name(int64_t id, wchar_t *out, size_t cap)
{
    media_one o;
    memset(&o, 0, sizeof(o));
    o.id = id;
    bd_list_media(g_cat, media_one_cb, &o);
    drive_name(o.found ? o.label : "the drive", o.found ? o.location : "", out, cap);
}

typedef struct { int64_t best; int64_t best_free; } other_ctx;

static int other_drive_cb(void *ctx, const bd_media_info *m)
{
    other_ctx *o = ctx;
    if (!m->connected || in_full_chain(m->media_id)) return 0;
    if (!o->best || m->free_bytes > o->best_free) { o->best = m->media_id; o->best_free = m->free_bytes; }
    return 0;
}

/* Ask whether a drive should take just what did not fit on the full ones.
 * Fills j->span if so. */
static void ask_continue(job *j, int64_t id, const wchar_t *name)
{
    if (!g_full_n || (id && in_full_chain(id))) return;
    wchar_t full[600], size[64], msg[1600];
    media_name(g_full[g_full_n - 1], full, 600);
    format_bytes(g_full_left_bytes, size, 64);
    swprintf(msg, 1600,
             L"%ls filled up earlier and %lld files (%ls) were left over.\n\n"
             L"Put just those on %ls, so the drives together hold everything?\n\n"
             L"(No backs up everything that is missing from it.)",
             full, (long long)g_full_left, size, name);
    if (MessageBoxW(g_main, msg, APP_NAME, MB_ICONQUESTION | MB_YESNO) == IDYES)
        memcpy(j->span, g_full, sizeof(j->span));
}

static void after_backup(job *j)
{
    wchar_t name[600];
    media_name(j->media_id, name, 600);
    if (j->no_room > 0) {
        /* This drive joins the chain of full ones. */
        g_full_n = 0;
        for (int i = 0; i < BD_MAX_CONTINUE && j->span[i]; i++) g_full[g_full_n++] = j->span[i];
        if (g_full_n < BD_MAX_CONTINUE) g_full[g_full_n++] = j->media_id;
        g_full_left = j->no_room;
        g_full_left_bytes = j->bytes_no_room;
        wchar_t size[64], msg[1600];
        format_bytes(j->bytes_no_room, size, 64);
        other_ctx o = {0, 0};
        bd_list_media(g_cat, other_drive_cb, &o);
        if (o.best) {
            wchar_t other[600];
            media_name(o.best, other, 600);
            swprintf(msg, 1600, L"%ls is full. %lld file%ls (%ls) did not fit.\n\nPut %ls on %ls now?", name,
                     (long long)j->no_room, j->no_room == 1 ? L"" : L"s", size, j->no_room == 1 ? L"it" : L"them", other);
            if (MessageBoxW(g_main, msg, APP_NAME, MB_ICONQUESTION | MB_YESNO) == IDYES) {
                job *b = new_job(JOB_BACKUP);
                if (b) {
                    b->media_id = o.best;
                    memcpy(b->span, g_full, sizeof(b->span));
                    enqueue(b);
                }
            }
        } else {
            swprintf(msg, 1600,
                     L"%ls is full. %lld file%ls (%ls) did not fit.\n\n"
                     L"Plug in another backup drive, or set up a new one with Back up..., and BRODALF will offer to put %ls there.",
                     name, (long long)j->no_room, j->no_room == 1 ? L"" : L"s", size, j->no_room == 1 ? L"it" : L"them");
            MessageBoxW(g_main, msg, APP_NAME, MB_ICONINFORMATION);
        }
    } else if (j->span[0]) {
        wchar_t line[800];
        swprintf(line, 800, L"Everything left over from the full drive now has a copy on %ls.", name);
        log_append(line);
        g_full_n = 0;
    }
    if (j->auto_run) offer_check(j->media_id);
}

typedef struct { wtext t; int n; int any_offline; int64_t ids[64]; int nid; } plan_text;

static int plan_text_cb(void *ctx, const bd_restore_step *st)
{
    plan_text *p = ctx;
    wchar_t name[600], size[64];
    drive_name(st->label, st->location, name, 600);
    format_bytes(st->bytes, size, 64);
    wadd(&p->t, L"    %d. %ls: %lld file%ls (%ls)%ls\n", ++p->n, name, (long long)st->files, st->files == 1 ? L"" : L"s",
         size, st->connected ? L", plugged in" : L"");
    if (!st->connected) p->any_offline = 1;
    if (p->nid < 64) p->ids[p->nid++] = st->media_id;
    return 0;
}

static void clear_pending(void)
{
    g_pending.active = 0;
    free(g_pending.rel);
    free(g_pending.dest);
    g_pending.rel = g_pending.dest = NULL;
}

static void after_restore(job *j)
{
    if (j->offline <= 0) {
        if (g_pending.active) log_append(L"The restore is complete: every file that has a copy is back.");
        clear_pending();
        return;
    }
    if (!g_pending.active) {
        g_pending.active = 1;
        g_pending.source_id = j->source_id;
        g_pending.rel = xstrdup(j->rel);
        g_pending.dest = xstrdup(j->dest);
    }
    plan_text p;
    memset(&p, 0, sizeof(p));
    bd_restore_plan(g_cat, j->source_id, j->rel, j->dest, plan_text_cb, &p, NULL, NULL);
    if (p.t.p) {
        wtext msg = {NULL, 0, 0};
        wadd(&msg, L"%lld file%ls on drives that are not plugged in. Plug these in, one at a time; "
                   L"BRODALF restores each one's files when it arrives:\r\n%ls",
             (long long)j->offline, j->offline == 1 ? L" is" : L"s are", p.t.p);
        for (wchar_t *c = msg.p; c && *c; c++) if (*c == L'\n' && (c == msg.p || c[-1] != L'\r')) *c = L' ';
        log_append(msg.p);
        free(msg.p);
    }
    free(p.t.p);
}

/* A drive arrived while a restore waits for it: restore its part. */
static void resume_restore(const job *drives)
{
    if (!g_pending.active || !drives->n_new) return;
    plan_text p;
    memset(&p, 0, sizeof(p));
    bd_restore_plan(g_cat, g_pending.source_id, g_pending.rel, g_pending.dest, plan_text_cb, &p, NULL, NULL);
    free(p.t.p);
    int helps = 0;
    for (int i = 0; i < p.nid && !helps; i++)
        for (int k = 0; k < drives->n_new; k++)
            if (p.ids[i] == drives->new_ids[k]) helps = 1;
    if (!helps) return;
    for (int k = 0; k < drives->n_new; k++)
        if (media_encryption(drives->new_ids[k]).encrypted && !bd_catalog_is_unlocked(g_cat) &&
            !unlock_ui(L"The drive you plugged in is encrypted. Enter the passphrase to restore from it."))
            return;
    log_append(L"A drive the restore needs was plugged in. Restoring the files it holds.");
    job *r = new_job(JOB_RESTORE);
    if (!r) return;
    r->source_id = g_pending.source_id;
    r->rel = xstrdup(g_pending.rel);
    r->dest = xstrdup(g_pending.dest);
    enqueue(r);
}

/* A drive BRODALF knows was plugged in: read back the copies on it that
 * were not read back lately, so slow damage shows while other copies exist. */
static void verify_new_drives(const job *j)
{
    if (bd_option_get(g_cat, "verify_days") <= 0) return;
    for (int i = 0; i < j->n_new; i++) {
        media_one o;
        memset(&o, 0, sizeof(o));
        o.id = j->new_ids[i];
        bd_list_media(g_cat, media_one_cb, &o);
        if (!o.found || o.info.copies == 0) continue;
        if (o.info.encrypted && !bd_catalog_is_unlocked(g_cat)) {
            wchar_t name[600], line[800];
            drive_name(o.label, o.location, name, 600);
            swprintf(line, 800, L"%ls is encrypted; its copies are read back when the passphrase is entered.", name);
            log_append(line);
            continue;
        }
        job *v = new_job(JOB_VERIFY);
        if (!v) continue;
        v->media_id = o.id;
        enqueue(v);
    }
}

static void after_drives(job *j)
{
    if (j->startup) remind_checks();
    if (j->n_new) verify_new_drives(j);
    if (!j->n_new || !j->auto_run) return;
    resume_restore(j);
    if (bd_option_get(g_cat, "auto_backup") == 1) {
        /* Ask for any passphrase first, before jobs start running. */
        int64_t ids[16];
        int n = 0;
        for (int i = 0; i < j->n_new; i++) {
            if (media_encryption(j->new_ids[i]).encrypted && !bd_catalog_is_unlocked(g_cat) &&
                !unlock_ui(L"The drive you plugged in is encrypted. Enter the passphrase to back up to it.")) {
                offer_check(j->new_ids[i]);
                continue;
            }
            ids[n++] = j->new_ids[i];
        }
        if (!n) return;
        log_append(L"A backup drive was plugged in. Checking your folders for changes, then backing up to it. "
                   L"(Turn this off under Settings.)");
        job *jobs[16];
        for (int i = 0; i < n; i++) {
            jobs[i] = new_job(JOB_BACKUP);
            if (!jobs[i]) continue;
            jobs[i]->media_id = ids[i];
            jobs[i]->auto_run = 1;
            wchar_t name[600];
            media_name(ids[i], name, 600);
            ask_continue(jobs[i], ids[i], name);
        }
        enqueue(new_job(JOB_SCAN));
        for (int i = 0; i < n; i++)
            if (jobs[i]) enqueue(jobs[i]);
    } else {
        for (int i = 0; i < j->n_new; i++) {
            /* Even without automatic backups, a waiting overflow is worth offering. */
            if (g_full_n && !in_full_chain(j->new_ids[i])) {
                job *b = new_job(JOB_BACKUP);
                wchar_t name[600];
                media_name(j->new_ids[i], name, 600);
                if (b) {
                    b->media_id = j->new_ids[i];
                    ask_continue(b, j->new_ids[i], name);
                    if (b->span[0]) enqueue(b);
                    else job_free(b);
                }
            }
            offer_check(j->new_ids[i]);
        }
    }
}

typedef struct { int versions, days; const wchar_t *text; } keep_preset;
static const keep_preset KEEP_PRESETS[5] = {
    {0, 0, L"Keep every old version"},
    {10, 365, L"Keep the last 10, and everything from the past year"},
    {5, 365, L"Keep the last 5, and everything from the past year"},
    {3, 90, L"Keep the last 3, and everything from the past 3 months"},
    {2, 0, L"Keep only the version before the current one"},
};

/* The skip list, with the edit box's \r\n line ends. */
static wchar_t *skip_text_for_edit(const char *list)
{
    wtext t = {NULL, 0, 0};
    wchar_t *w = widen(list);
    for (wchar_t *c = w; c && *c; c++) {
        if (*c == L'\r') continue;
        if (*c == L'\n') wadd(&t, L"\r\n");
        else wadd(&t, L"%lc", *c);
    }
    free(w);
    return t.p ? t.p : _wcsdup(L"");
}

static INT_PTR CALLBACK skip_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG: {
        char *list = bd_skip_list_get(g_cat);
        wchar_t *w = skip_text_for_edit(list ? list : "");
        SetDlgItemTextW(dlg, IDC_SKIP_EDIT, w);
        free(w);
        free(list);
        SetFocus(GetDlgItem(dlg, IDC_SKIP_EDIT));
        SendDlgItemMessageW(dlg, IDC_SKIP_EDIT, EM_SETSEL, 0, 0);
        return FALSE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_SKIP_DEFAULT) {
            wchar_t *w = skip_text_for_edit(bd_skip_list_default());
            SetDlgItemTextW(dlg, IDC_SKIP_EDIT, w);
            free(w);
            return TRUE;
        }
        if (LOWORD(wp) == IDOK) {
            int n = GetWindowTextLengthW(GetDlgItem(dlg, IDC_SKIP_EDIT));
            wchar_t *w = calloc((size_t)n + 1, sizeof(wchar_t));
            if (!w) return TRUE;
            GetDlgItemTextW(dlg, IDC_SKIP_EDIT, w, n + 1);
            char *u = narrow(w);
            free(w);
            if (u) {
                char *o = u;
                for (char *c = u; *c; c++) if (*c != '\r') *o++ = *c;
                *o = '\0';
            }
            bd_status s = u ? bd_skip_list_set(g_cat, u) : BD_ERR_NOMEM;
            free(u);
            if (s == BD_OK) s = bd_catalog_save(g_cat);
            if (s != BD_OK) {
                wchar_t *e = widen(bd_catalog_error(g_cat));
                MessageBoxW(dlg, e ? e : L"Could not save the list.", APP_NAME, MB_ICONWARNING);
                free(e);
                return TRUE;
            }
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

static void cmd_skip(void)
{
    if (DialogBoxW(g_inst, MAKEINTRESOURCEW(IDD_SKIP), g_main, skip_proc) == IDOK) {
        log_append(L"The skip list changed. Scanning your folders again.");
        enqueue(new_job(JOB_SCAN));
    }
}

static void settings_command(int cmd);
static void apply_schedule(int days, int say);
static int guard_dialog(void);

static void cmd_security(void)
{
    int has = bd_catalog_has_passphrase(g_cat), unlocked = bd_catalog_is_unlocked(g_cat);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, ID_MENU_SET_PASS, has ? L"Change the passphrase..." : L"Set a passphrase...");
    AppendMenuW(m, MF_STRING | (has ? 0 : MF_GRAYED) | (bd_catalog_file_encrypted(g_cat) ? MF_CHECKED : 0),
                ID_MENU_ENCRYPT_CATALOG, L"Encrypt the catalog file");
    AppendMenuW(m, MF_STRING | (has && unlocked ? 0 : MF_GRAYED), ID_MENU_LOCK, L"Forget the passphrase until it is needed");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (bd_option_get(g_cat, "auto_backup") == 1 ? MF_CHECKED : 0), ID_MENU_AUTO_BACKUP,
                L"Back up as soon as a drive is plugged in");
    HMENU remind = CreatePopupMenu();
    int days = bd_option_get(g_cat, "check_days");
    AppendMenuW(remind, MF_STRING | (days == 90 ? MF_CHECKED : 0), ID_MENU_CHECK_90, L"Every 3 months");
    AppendMenuW(remind, MF_STRING | (days == 180 ? MF_CHECKED : 0), ID_MENU_CHECK_180, L"Every 6 months");
    AppendMenuW(remind, MF_STRING | (days == 365 ? MF_CHECKED : 0), ID_MENU_CHECK_365, L"Every year");
    AppendMenuW(remind, MF_STRING | (days == 0 ? MF_CHECKED : 0), ID_MENU_CHECK_NEVER, L"Never");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)remind, L"Remind me to check each drive");
    HMENU keep = CreatePopupMenu();
    int kv = bd_option_get(g_cat, "keep_versions"), kd = bd_option_get(g_cat, "keep_days");
    for (int i = 0; i < 5; i++)
        AppendMenuW(keep, MF_STRING | (KEEP_PRESETS[i].versions == kv && (kv == 0 || KEEP_PRESETS[i].days == kd) ? MF_CHECKED : 0),
                    (UINT_PTR)(ID_MENU_KEEP_0 + i), KEEP_PRESETS[i].text);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)keep, L"Old versions on each drive");
    AppendMenuW(m, MF_STRING, ID_MENU_SKIP, L"What to leave out...");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    HMENU sched = CreatePopupMenu();
    int sd = bd_option_get(g_cat, "schedule");
    AppendMenuW(sched, MF_STRING | (sd == 1 ? MF_CHECKED : 0), ID_M_SCHED_DAILY, L"Every day");
    AppendMenuW(sched, MF_STRING | (sd == 7 ? MF_CHECKED : 0), ID_M_SCHED_WEEKLY, L"Every week");
    AppendMenuW(sched, MF_STRING | (sd == 0 ? MF_CHECKED : 0), ID_M_SCHED_NEVER, L"Never");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)sched, L"Check my folders for changes while BRODALF is closed");
    HMENU verify = CreatePopupMenu();
    int vd = bd_option_get(g_cat, "verify_days");
    AppendMenuW(verify, MF_STRING | (vd == 7 ? MF_CHECKED : 0), ID_M_VERIFY_7, L"Copies not read back in a week");
    AppendMenuW(verify, MF_STRING | (vd == 30 ? MF_CHECKED : 0), ID_M_VERIFY_30, L"Copies not read back in a month");
    AppendMenuW(verify, MF_STRING | (vd == 90 ? MF_CHECKED : 0), ID_M_VERIFY_90, L"Copies not read back in 3 months");
    AppendMenuW(verify, MF_STRING | (vd == 0 ? MF_CHECKED : 0), ID_M_VERIFY_NEVER, L"Never");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)verify, L"Read back copies when a drive is plugged in");
    HMENU guard = CreatePopupMenu();
    int gp = bd_option_get(g_cat, "guard_percent");
    AppendMenuW(guard, MF_STRING | (gp == 10 ? MF_CHECKED : 0), ID_M_GUARD_10, L"When a tenth of the files change at once");
    AppendMenuW(guard, MF_STRING | (gp == 25 ? MF_CHECKED : 0), ID_M_GUARD_25, L"When a quarter of the files change at once");
    AppendMenuW(guard, MF_STRING | (gp == 50 ? MF_CHECKED : 0), ID_M_GUARD_50, L"When half of the files change at once");
    AppendMenuW(guard, MF_STRING | (gp == 0 ? MF_CHECKED : 0), ID_M_GUARD_NEVER, L"Never");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)guard, L"Pause backups (ransomware guard)");
    bd_guard_info gi;
    if (bd_guard_get(g_cat, &gi) == BD_OK && gi.tripped)
        AppendMenuW(m, MF_STRING, ID_M_GUARD_STATUS, L"Backups are paused by the guard...");
    RECT r;
    GetWindowRect(g_buttons[6], &r);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, r.left, r.bottom, 0, g_main, NULL);
    DestroyMenu(m);
    settings_command(cmd);
}

static void settings_command(int cmd)
{
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
    } else if (cmd == ID_MENU_AUTO_BACKUP) {
        int on = bd_option_get(g_cat, "auto_backup") != 1;
        if (bd_option_set(g_cat, "auto_backup", on) == BD_OK && bd_catalog_save(g_cat) == BD_OK)
            log_append(on ? L"BRODALF will back up to a drive as soon as it is plugged in."
                          : L"BRODALF will only back up when you press Back up.");
    } else if (cmd >= ID_MENU_KEEP_0 && cmd <= ID_MENU_KEEP_4) {
        const keep_preset *k = &KEEP_PRESETS[cmd - ID_MENU_KEEP_0];
        if (bd_option_set(g_cat, "keep_versions", k->versions) == BD_OK && bd_option_set(g_cat, "keep_days", k->days) == BD_OK &&
            bd_catalog_save(g_cat) == BD_OK) {
            wchar_t line[300];
            swprintf(line, 300, L"Old versions: %ls. Older ones are cleaned up the next time you back up to each drive.", k->text);
            log_append(line);
        }
    } else if (cmd == ID_MENU_SKIP) {
        cmd_skip();
    } else if (cmd >= ID_M_SCHED_DAILY && cmd <= ID_M_SCHED_NEVER) {
        static const int choice[] = {1, 7, 0};
        int d = choice[cmd - ID_M_SCHED_DAILY];
        if (bd_option_set(g_cat, "schedule", d) == BD_OK && bd_catalog_save(g_cat) == BD_OK) apply_schedule(d, 1);
    } else if (cmd >= ID_M_VERIFY_7 && cmd <= ID_M_VERIFY_NEVER) {
        static const int choice[] = {7, 30, 90, 0};
        int d = choice[cmd - ID_M_VERIFY_7];
        if (bd_option_set(g_cat, "verify_days", d) == BD_OK && bd_catalog_save(g_cat) == BD_OK) {
            wchar_t line[200];
            if (d) swprintf(line, 200, L"When a drive is plugged in, BRODALF reads back the copies on it not read back in %d days.", d);
            else wcscpy(line, L"BRODALF will not read back copies when a drive is plugged in (Check drive still does).");
            log_append(line);
        }
    } else if (cmd >= ID_M_GUARD_10 && cmd <= ID_M_GUARD_NEVER) {
        static const int choice[] = {10, 25, 50, 0};
        int p = choice[cmd - ID_M_GUARD_10];
        if (bd_option_set(g_cat, "guard_percent", p) == BD_OK && bd_catalog_save(g_cat) == BD_OK) {
            wchar_t line[200];
            if (p) swprintf(line, 200, L"Backups pause when one scan finds %d%% of the files changed or gone at once.", p);
            else wcscpy(line, L"The ransomware guard is off.");
            log_append(line);
        }
    } else if (cmd == ID_M_GUARD_STATUS) {
        guard_dialog();
    } else if (cmd >= ID_MENU_CHECK_90 && cmd <= ID_MENU_CHECK_NEVER) {
        static const int choice[] = {90, 180, 365, 0};
        int d = choice[cmd - ID_MENU_CHECK_90];
        if (bd_option_set(g_cat, "check_days", d) == BD_OK && bd_catalog_save(g_cat) == BD_OK) {
            wchar_t line[200];
            if (d) swprintf(line, 200, L"BRODALF will remind you to check a drive whose copies have not been read in %d days.", d);
            else wcscpy(line, L"BRODALF will not remind you to check drives.");
            log_append(line);
            update_drives_label();
        }
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
    if (!m->connected || strcmp(m->kind, "drive") != 0) return 0; /* a cloud account from an earlier version is never listed */
    wchar_t *l = widen(m->label), *root = widen(m->last_root), text[512], freeb[64];
    format_bytes(m->free_bytes, freeb, 64);
    swprintf(text, 512, L"%ls (%ls), %ls free%ls", l, root, freeb, m->encrypted ? L", encrypted" : L"");
    AppendMenuW(c->menu, MF_STRING, (UINT_PTR)(ID_MENU_DRIVE_BASE + m->media_id), text);
    c->count++;
    free(l);
    free(root);
    return 0;
}

/* Returns a media id, 0 for "another drive or folder", -1 if cancelled. */
static int64_t choose_drive(HWND button, int allow_other)
{
    menu_ctx c = {CreatePopupMenu(), 0};
    bd_list_media(g_cat, drive_menu_cb, &c);
    if (allow_other) {
        if (c.count) AppendMenuW(c.menu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(c.menu, MF_STRING, ID_MENU_OTHER, L"Another drive or folder...");
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
    if (cmd >= ID_MENU_DRIVE_BASE) return cmd - ID_MENU_DRIVE_BASE;
    return -1;
}

static void add_disk(void);

static void cmd_backup(void)
{
    int64_t id = choose_drive(g_buttons[2], 1);
    if (id < 0) return;
    if (id == 0) { add_disk(); return; }
    job *j = new_job(JOB_BACKUP);
    if (!j) return;
    j->media_id = id;
    if (media_encryption(id).encrypted && !unlock_ui(L"This drive is encrypted. Enter the passphrase to back up to it.")) {
        job_free(j);
        return;
    }
    ask_continue(j, id, L"it");
    enqueue(j);
}

/* Set up a disk (or a folder on one) as backup storage and back up to it. */
static void add_disk(void)
{
    job *j = new_job(JOB_BACKUP);
    if (!j) return;
    {
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

/* Restore what is selected in the tree (or everything, with whole set) as it
 * is now, or as it was at as_of. */
static void restore_selection(int whole, int64_t as_of)
{
    node_ref all = {0}, *r = whole ? &all : item_ref(TreeView_GetSelection(g_tree));
    if (!r) {
        MessageBoxW(g_main, L"Select a protected folder, a folder inside it, or a file to restore.", APP_NAME, MB_ICONINFORMATION);
        return;
    }
    if (media_encryption(0).any_connected_encrypted && !unlock_ui(L"Some copies are on an encrypted drive. Enter the passphrase to restore them."))
        return;
    char *dest = pick_folder(g_main, L"Choose where to put the restored files. They go into a subfolder named after the protected folder.");
    if (!dest) return;

    /* Which drives this needs, in order. */
    plan_text p;
    memset(&p, 0, sizeof(p));
    int64_t total = 0, none = 0;
    bd_restore_opts ro = {as_of};
    bd_restore_plan_ex(g_cat, r->source_id, r->rel, dest, &ro, plan_text_cb, &p, &total, &none);
    if (total == 0) {
        free(p.t.p);
        free(dest);
        MessageBoxW(g_main, as_of ? L"BRODALF knew of no files here at that time." : L"Everything here is already restored in that folder.",
                    APP_NAME, MB_ICONINFORMATION);
        return;
    }
    if (p.n > 1 || p.any_offline || none) {
        wtext msg = {NULL, 0, 0};
        wadd(&msg, L"Restoring %lld file%ls needs these drives:\n\n%ls", (long long)total, total == 1 ? L"" : L"s",
             p.t.p ? p.t.p : L"    (none)\n");
        if (none) wadd(&msg, L"\n%lld file%ls no copy on any drive.\n", (long long)none, none == 1 ? L" has" : L"s have");
        if (p.any_offline)
            wadd(&msg, L"\nBRODALF restores what the plugged-in drives hold now. Then plug in each other drive; "
                       L"its files are restored as soon as it arrives, while BRODALF is open.\n");
        int go = MessageBoxW(g_main, msg.p, APP_NAME, MB_ICONINFORMATION | MB_OKCANCEL) == IDOK;
        free(msg.p);
        if (!go) { free(p.t.p); free(dest); return; }
    }
    free(p.t.p);
    job *j = new_job(JOB_RESTORE);
    if (!j) { free(dest); return; }
    j->source_id = r->source_id;
    j->rel = xstrdup(r->rel ? r->rel : "");
    j->dest = dest;
    j->as_of = as_of;
    clear_pending();
    if (as_of) {
        wchar_t when[64], line[200];
        format_time_ms(as_of, when, 64);
        swprintf(line, 200, L"Restoring the files as they were at %ls.", when);
        log_append(line);
    }
    enqueue(j);
}

static void cmd_restore(void) { restore_selection(0, 0); }

/* ---- Restore as of a date ------------------------------------------------ */

static int64_t g_asof_ms;

static INT_PTR CALLBACK asof_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG:
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            SYSTEMTIME st, utc;
            FILETIME ft;
            if (DateTime_GetSystemtime(GetDlgItem(dlg, IDC_ASOF_DATE), &st) != GDT_VALID) return TRUE;
            st.wHour = 23; st.wMinute = 59; st.wSecond = 59; st.wMilliseconds = 999;
            TzSpecificLocalTimeToSystemTime(NULL, &st, &utc);
            SystemTimeToFileTime(&utc, &ft);
            ULONGLONG t = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
            g_asof_ms = (int64_t)((t - 116444736000000000ULL) / 10000ULL);
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

static void cmd_restore_as_of(void)
{
    node_ref *r = item_ref(TreeView_GetSelection(g_tree));
    if (!r) {
        MessageBoxW(g_main, L"Select a protected folder, a folder inside it, or a file to restore.", APP_NAME, MB_ICONINFORMATION);
        return;
    }
    g_asof_ms = 0;
    if (DialogBoxW(g_inst, MAKEINTRESOURCEW(IDD_ASOF), g_main, asof_proc) != IDOK || !g_asof_ms) return;
    restore_selection(0, g_asof_ms);
}

/* ---- The ransomware guard ------------------------------------------------- */

static INT_PTR CALLBACK guard_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG: {
        bd_guard_info g;
        bd_guard_get(g_cat, &g);
        wchar_t when[64], before[64], text[1200];
        format_time_ms(g.tripped_ms, when, 64);
        format_time_ms(g.before_ms, before, 64);
        swprintf(text, 1200,
                 L"The scan at %ls found %lld of %lld files changed and %lld gone, all at once. "
                 L"That is what ransomware encrypting your files looks like, so BRODALF has paused backups: "
                 L"the copies made before are kept exactly as they are, and old versions are not cleaned up.\n\n"
                 L"If your files look wrong, restore them as they were at %ls (the scan before). "
                 L"If you made the changes yourself, tell BRODALF so and backups carry on.",
                 when, (long long)g.files_changed, (long long)g.files_total, (long long)g.files_deleted, before);
        SetDlgItemTextW(dlg, IDC_GUARD_TEXT, text);
        SetFocus(GetDlgItem(dlg, IDCANCEL));
        return FALSE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_GUARD_RESTORE || LOWORD(wp) == IDC_GUARD_MINE || LOWORD(wp) == IDCANCEL) {
            EndDialog(dlg, LOWORD(wp));
            return TRUE;
        }
        break;
    }
    return FALSE;
}

/* Returns 1 when the user said the changes were theirs and backups carry on. */
static int guard_dialog(void)
{
    bd_guard_info g;
    if (bd_guard_get(g_cat, &g) != BD_OK || !g.tripped) return 0;
    INT_PTR r = DialogBoxW(g_inst, MAKEINTRESOURCEW(IDD_GUARD), g_main, guard_proc);
    if (r == IDC_GUARD_RESTORE) {
        restore_selection(1, g.before_ms);
    } else if (r == IDC_GUARD_MINE) {
        if (bd_guard_clear(g_cat) == BD_OK && bd_catalog_save(g_cat) == BD_OK) {
            log_append(L"Backups carry on; the changes were yours.");
            update_drives_label();
            return 1;
        }
        update_drives_label();
    } else {
        log_append(L"Backups stay paused. Decide under Settings > Backups are paused by the guard.");
    }
    return 0;
}

/* ---- Files in use and shadow copies --------------------------------------- */

static wchar_t g_shadow_dir[MAX_PATH];

typedef struct { char **paths; int n, cap; int64_t bytes; } in_use_list;

static int in_use_cb(void *ctx, const char *path, int64_t size)
{
    in_use_list *l = ctx;
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 16;
        char **grown = realloc(l->paths, sizeof(char *) * (size_t)cap);
        if (!grown) return 1;
        l->paths = grown;
        l->cap = cap;
    }
    l->paths[l->n++] = xstrdup(path);
    l->bytes += size;
    return 0;
}

static void in_use_free(in_use_list *l)
{
    for (int i = 0; i < l->n; i++) free(l->paths[i]);
    free(l->paths);
}

static void shadow_cleanup(void)
{
    bd_substitutes_clear(g_cat);
    if (!g_shadow_dir[0]) return;
    wchar_t pattern[MAX_PATH + 8];
    swprintf(pattern, MAX_PATH + 8, L"%ls\\*", g_shadow_dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            wchar_t f[MAX_PATH * 2];
            swprintf(f, MAX_PATH * 2, L"%ls\\%ls", g_shadow_dir, fd.cFileName);
            DeleteFileW(f);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(g_shadow_dir);
    g_shadow_dir[0] = 0;
}

/* Run an elevated copy of BRODALF that copies the files in use out of a
 * shadow copy, then read from those copies. Worker thread. */
static bd_status run_shadow(job *j)
{
    in_use_list l = {NULL, 0, 0, 0};
    bd_list_in_use(g_cat, in_use_cb, &l);
    if (!l.n) { swprintf(j->summary, 512, L"No files are in use any more."); return BD_OK; }
    shadow_cleanup();
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    swprintf(g_shadow_dir, MAX_PATH, L"%lsbrodalf-shadow-%lu", tmp, (unsigned long)GetCurrentProcessId());
    CreateDirectoryW(g_shadow_dir, NULL);
    wchar_t list[MAX_PATH + 16], params[MAX_PATH * 2 + 64], exe[MAX_PATH];
    swprintf(list, MAX_PATH + 16, L"%ls\\list.txt", g_shadow_dir);
    FILE *f = _wfopen(list, L"wb");
    if (!f) { in_use_free(&l); swprintf(j->summary, 512, L"Could not write the list of files in use."); return BD_ERR_IO; }
    for (int i = 0; i < l.n; i++) fprintf(f, "%s\n", l.paths[i]);
    fclose(f);
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    swprintf(params, MAX_PATH * 2 + 64, L"--shadow-copy \"%ls\" \"%ls\"", g_shadow_dir, list);
    post_text(WM_APP_LOG, L"Asking Windows for administrator permission to take a shadow copy...");
    SHELLEXECUTEINFOW sei;
    memset(&sei, 0, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    sei.lpParameters = params;
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) {
        DWORD err = GetLastError();
        in_use_free(&l);
        if (err == ERROR_CANCELLED) {
            swprintf(j->summary, 512, L"Without administrator permission the files in use stay as they were last backed up.");
            return BD_OK;
        }
        swprintf(j->summary, 512, L"Could not start the shadow copy helper (Windows error %lu).", (unsigned long)err);
        return BD_ERR_IO;
    }
    WaitForSingleObject(sei.hProcess, INFINITE);
    CloseHandle(sei.hProcess);

    /* result.txt: one line per file, "ok" or "failed", then "error: ...". */
    wchar_t result[MAX_PATH + 16];
    swprintf(result, MAX_PATH + 16, L"%ls\\result.txt", g_shadow_dir);
    f = _wfopen(result, L"rb");
    int got = 0;
    char line[2048], err[1024] = "";
    for (int i = 0; f && fgets(line, sizeof(line), f);) {
        line[strcspn(line, "\r\n")] = 0;
        if (strncmp(line, "error: ", 7) == 0) { snprintf(err, sizeof(err), "%s", line + 7); continue; }
        if (i < l.n && strcmp(line, "ok") == 0) {
            char staged[MAX_PATH * 3];
            char *dir = narrow(g_shadow_dir);
            snprintf(staged, sizeof(staged), "%s\\%d.bin", dir ? dir : "", i + 1);
            free(dir);
            if (bd_substitute_add(g_cat, l.paths[i], staged) == BD_OK) got++;
        }
        i++;
    }
    if (f) fclose(f);
    in_use_free(&l);
    if (!got) {
        wchar_t *e = widen(err);
        swprintf(j->summary, 512, L"The shadow copy did not work: %ls", e && *e ? e : L"no reason was given");
        free(e);
        shadow_cleanup();
        return BD_OK;
    }
    swprintf(j->summary, 512, L"Read %d of %d files in use from a shadow copy. Scanning them now.", got, l.n);
    return BD_OK;
}

/* The shadow copies are ready: scan, then back up where the files were going. */
static void after_shadow(job *j)
{
    if (!g_shadow_dir[0]) return;
    job *scan = new_job(JOB_SCAN);
    job *back = j->media_id ? new_job(JOB_BACKUP) : NULL;
    if (back) {
        back->media_id = j->media_id;
        back->from_shadow = 1;
    } else if (scan) {
        scan->from_shadow = 1;
    }
    if (scan) enqueue(scan);
    if (back) enqueue(back);
}

/* After a scan or backup that met files in use: offer to read them from a
 * shadow copy. */
static void offer_shadow(const job *j)
{
    in_use_list l = {NULL, 0, 0, 0};
    bd_list_in_use(g_cat, in_use_cb, &l);
    if (!l.n) return;
    wtext msg = {NULL, 0, 0};
    wchar_t size[64];
    format_bytes(l.bytes, size, 64);
    wadd(&msg, L"%d file%ls (%ls) %ls in use by another program, so BRODALF could not read %ls:\n\n", l.n, l.n == 1 ? L"" : L"s",
         size, l.n == 1 ? L"is" : L"are", l.n == 1 ? L"it" : L"them");
    for (int i = 0; i < l.n && i < 6; i++) {
        wchar_t *w = widen(l.paths[i]);
        wadd(&msg, L"    %ls\n", w ? w : L"");
        free(w);
    }
    if (l.n > 6) wadd(&msg, L"    ... and %d more\n", l.n - 6);
    wadd(&msg, L"\nWindows can take a shadow copy of the drive, a snapshot no program holds open, and BRODALF can read the files "
               L"from that. Windows will ask for administrator permission.\n\nRead them from a shadow copy now?");
    int yes = MessageBoxW(g_main, msg.p, APP_NAME, MB_ICONQUESTION | MB_YESNO) == IDYES;
    free(msg.p);
    in_use_free(&l);
    if (!yes) return;
    job *s = new_job(JOB_SHADOW);
    if (!s) return;
    s->media_id = j->kind == JOB_BACKUP ? j->media_id : 0;
    enqueue(s);
}

/* ---- Scheduled checks ------------------------------------------------------ */

static void task_name(wchar_t *out, size_t cap)
{
    const wchar_t *base = wcsrchr(g_cat_path, L'\\');
    swprintf(out, cap, L"BRODALF - %ls", base ? base + 1 : g_cat_path);
}

/* Tell Windows Task Scheduler to run "brodalf.exe --check <catalog>" every
 * day or week, or forget the task. */
static void apply_schedule(int days, int say)
{
    wchar_t name[MAX_PATH + 32], exe[MAX_PATH], args[MAX_PATH * 3 + 128];
    task_name(name, MAX_PATH + 32);
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (days > 0)
        swprintf(args, MAX_PATH * 3 + 128, L"/Create /F /SC %ls /TN \"%ls\" /ST 12:00 /TR \"\\\"%ls\\\" --check \\\"%ls\\\"\"",
                 days >= 7 ? L"WEEKLY" : L"DAILY", name, exe, g_cat_path);
    else
        swprintf(args, MAX_PATH * 3 + 128, L"/Delete /F /TN \"%ls\"", name);
    SHELLEXECUTEINFOW sei;
    memset(&sei, 0, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
    sei.lpFile = L"schtasks.exe";
    sei.lpParameters = args;
    sei.nShow = SW_HIDE;
    DWORD code = 1;
    if (ShellExecuteExW(&sei)) {
        WaitForSingleObject(sei.hProcess, 30000);
        GetExitCodeProcess(sei.hProcess, &code);
        CloseHandle(sei.hProcess);
    }
    char *u = narrow(args);
    bd_applog("schtasks %s -> %lu", u ? u : "", (unsigned long)code);
    free(u);
    if (!say) return;
    if (code != 0)
        log_append(days > 0 ? L"Windows did not accept the scheduled task (see the app log). BRODALF only checks your folders while it is open."
                            : L"The scheduled task could not be removed (see the app log).");
    else if (days > 0) {
        wchar_t line[300];
        swprintf(line, 300, L"Windows will run BRODALF every %ls around noon to check your folders. It only shows itself when "
                            L"something needs backing up.", days >= 7 ? L"week" : L"day");
        log_append(line);
    } else
        log_append(L"BRODALF checks your folders only while it is open.");
}

/* ---- The menu bar ----------------------------------------------------------- */

static void clear_menu(HMENU m)
{
    while (GetMenuItemCount(m) > 0) DeleteMenu(m, 0, MF_BYPOSITION);
}

typedef struct { HMENU local; int n_local; } fill_menus;

static int fill_menu_cb(void *ctx, const bd_media_info *m)
{
    fill_menus *f = ctx;
    if (strcmp(m->kind, "drive") != 0) return 0; /* a cloud account from an earlier version: nothing can be done with it */
    wchar_t *label = widen(m->label), *loc = widen(m->location), *root = widen(m->last_root), text[700], freeb[64], when[64];
    HMENU sub = CreatePopupMenu();
    UINT base = (UINT)(ID_DRIVE_ACT_BASE + m->media_id * 8);
    format_bytes(m->free_bytes, freeb, 64);
    format_time_ms(m->space_ms, when, 64);
    if (m->connected) swprintf(text, 700, L"Plugged in now at %ls, %ls free", root, freeb);
    else if (m->space_ms > 0) swprintf(text, 700, L"Not plugged in. %ls free when last seen (%ls)", freeb, when);
    else swprintf(text, 700, L"Not plugged in");
    AppendMenuW(sub, MF_STRING | MF_GRAYED, 0, text);
    AppendMenuW(sub, MF_SEPARATOR, 0, NULL);
    AppendMenuW(sub, MF_STRING | (m->connected ? 0 : MF_GRAYED), base + 0, L"Back up to it now");
    AppendMenuW(sub, MF_STRING | (m->connected ? 0 : MF_GRAYED), base + 1, L"Read back copies not checked lately");
    AppendMenuW(sub, MF_STRING | (m->connected ? 0 : MF_GRAYED), base + 2, L"Full check (read back every copy)");
    AppendMenuW(sub, MF_STRING, base + 3, L"Name, where it is kept, hardware details...");
    swprintf(text, 700, L"%ls%ls%ls%ls%ls", label, *loc ? L"  (" : L"", loc, *loc ? L")" : L"", m->connected ? L"  - plugged in" : L"");
    AppendMenuW(f->local, MF_POPUP, (UINT_PTR)sub, text);
    f->n_local++;
    free(label); free(loc); free(root);
    return 0;
}

/* Fill the Local backups menu from the catalog, each time it opens. */
static void fill_storage_menus(void)
{
    clear_menu(g_menu_local);
    fill_menus f = {g_menu_local, 0};
    bd_list_media(g_cat, fill_menu_cb, &f);
    if (!f.n_local) AppendMenuW(g_menu_local, MF_STRING | MF_GRAYED, 0, L"No backup disks yet");
    AppendMenuW(g_menu_local, MF_SEPARATOR, 0, NULL);
    AppendMenuW(g_menu_local, MF_STRING, ID_M_ADD_DISK, L"Add an external or removable disk...");
    AppendMenuW(g_menu_local, MF_STRING, ID_M_RISK, L"What needs backing up, and which disk to plug in...");
    AppendMenuW(g_menu_local, MF_STRING, ID_M_DRIVES, L"All disks...");
}

static void create_menu_bar(HWND hwnd)
{
    HMENU bar = CreateMenu();
    HMENU cat = CreatePopupMenu();
    AppendMenuW(cat, MF_STRING, ID_M_ADD_FOLDER, L"Add a folder to protect...");
    AppendMenuW(cat, MF_STRING, ID_M_SCAN, L"Check the folders for changes now");
    AppendMenuW(cat, MF_SEPARATOR, 0, NULL);
    AppendMenuW(cat, MF_STRING, ID_M_RESTORE, L"Restore what is selected...");
    AppendMenuW(cat, MF_STRING, ID_M_RESTORE_ASOF, L"Restore what is selected as it was on a date...");
    AppendMenuW(cat, MF_SEPARATOR, 0, NULL);
    AppendMenuW(cat, MF_STRING, ID_M_EXIT, L"Exit");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)cat, L"&Catalog");
    g_menu_local = CreatePopupMenu();
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)g_menu_local, L"&Local backups");
    HMENU help = CreatePopupMenu();
    AppendMenuW(help, MF_STRING, ID_M_IN_USE, L"Read files that are in use from a shadow copy...");
    AppendMenuW(help, MF_SEPARATOR, 0, NULL);
    AppendMenuW(help, MF_STRING, ID_M_LOG, L"Show the app log");
    AppendMenuW(help, MF_STRING, ID_M_REPORTS, L"Show saved error reports");
    AppendMenuW(help, MF_STRING, ID_M_ABOUT, L"About BRODALF");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)help, L"&Help");
    SetMenu(hwnd, bar);
}

static void drive_action(int64_t id, int action)
{
    media_one o;
    memset(&o, 0, sizeof(o));
    o.id = id;
    bd_list_media(g_cat, media_one_cb, &o);
    if (!o.found) return;
    if (action == 0) {
        if (o.info.encrypted && !unlock_ui(L"This drive is encrypted. Enter the passphrase to back up to it.")) return;
        job *j = new_job(JOB_BACKUP);
        if (!j) return;
        j->media_id = id;
        ask_continue(j, id, L"it");
        enqueue(j);
    } else if (action == 1) {
        if (o.info.encrypted && !unlock_ui(L"This drive is encrypted. Enter the passphrase to read its copies.")) return;
        job *j = new_job(JOB_VERIFY);
        if (!j) return;
        j->media_id = id;
        enqueue(j);
    } else if (action == 2) {
        enqueue_check(id);
    } else if (action == 3) {
        drive_list dl = load_drives();
        const drive_snap *d = find_drive(&dl, id);
        if (d) {
            g_edit_drive = d;
            if (DialogBoxW(g_inst, MAKEINTRESOURCEW(IDD_DRIVE), g_main, drive_proc) == IDOK) {
                char *name = narrow(g_edit_name), *loc = narrow(g_edit_location);
                bd_status st = BD_OK;
                if (name && strcmp(name, d->label) != 0) st = bd_media_rename(g_cat, id, name);
                if (st == BD_OK) st = bd_media_set_location(g_cat, id, loc);
                if (st == BD_OK) st = bd_catalog_save(g_cat);
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
}

static void open_in_explorer(const wchar_t *path)
{
    ShellExecuteW(g_main, L"open", path, NULL, NULL, SW_SHOWNORMAL);
}

static void menu_command(int cmd)
{
    if (cmd >= ID_DRIVE_ACT_BASE) {
        drive_action((cmd - ID_DRIVE_ACT_BASE) / 8, (cmd - ID_DRIVE_ACT_BASE) % 8);
        return;
    }
    switch (cmd) {
    case ID_M_ADD_FOLDER: cmd_add_folder(); break;
    case ID_M_SCAN: enqueue(new_job(JOB_SCAN)); break;
    case ID_M_RESTORE: cmd_restore(); break;
    case ID_M_RESTORE_ASOF: cmd_restore_as_of(); break;
    case ID_M_RISK: cmd_risk(); break;
    case ID_M_EXIT: PostMessageW(g_main, WM_CLOSE, 0, 0); break;
    case ID_M_ADD_DISK: add_disk(); break;
    case ID_M_DRIVES: cmd_drives(); break;
    case ID_M_IN_USE: {
        in_use_list l = {NULL, 0, 0, 0};
        bd_list_in_use(g_cat, in_use_cb, &l);
        int n = l.n;
        in_use_free(&l);
        if (!n) {
            MessageBoxW(g_main, L"The last scan could read every file. Files in use are found by a scan; BRODALF offers the "
                                L"shadow copy right after one that meets any.", APP_NAME, MB_ICONINFORMATION);
            break;
        }
        job fake;
        memset(&fake, 0, sizeof(fake));
        fake.kind = JOB_SCAN;
        offer_shadow(&fake);
        break;
    }
    case ID_M_LOG: {
        wchar_t *w = widen(bd_applog_path());
        if (w) open_in_explorer(w);
        free(w);
        break;
    }
    case ID_M_REPORTS: {
        wchar_t *w = widen(bd_applog_path());
        if (w) {
            wchar_t *slash = wcsrchr(w, L'\\');
            if (slash) wcscpy(slash + 1, L"reports");
            CreateDirectoryW(w, NULL);
            open_in_explorer(w);
        }
        free(w);
        break;
    }
    case ID_M_ABOUT: {
        wchar_t *v = widen(bd_version()), msg[600];
        swprintf(msg, 600, L"BRODALF %ls\n\nKeeps track of which drive holds a copy of each of your files, and whether that copy is "
                           L"still good.\n\nhttps://github.com/Phawx/BRODALF", v ? v : L"");
        free(v);
        MessageBoxW(g_main, msg, APP_NAME, MB_ICONINFORMATION);
        break;
    }
    }
}

/* ---- Search ------------------------------------------------------------- */

#define ID_SEARCH_TIMER 1

typedef struct { int64_t source_id; char *rel; } search_hit;
static search_hit *g_hits;
static int g_hit_n, g_hit_cap;

static void clear_hits(void)
{
    for (int i = 0; i < g_hit_n; i++) free(g_hits[i].rel);
    g_hit_n = 0;
}

static const wchar_t *short_state(bd_node_state s)
{
    switch (s) {
    case BD_STATE_AVAILABLE: return L"Available";
    case BD_STATE_AVAILABLE_OLDER: return L"Changed since backup";
    case BD_STATE_OFFLINE: return L"Drive not plugged in";
    case BD_STATE_NO_COPY: return L"Not backed up";
    case BD_STATE_BAD: return L"Copy damaged";
    case BD_STATE_DELETED: return L"Deleted";
    case BD_STATE_PARTIAL: return L"Partly available";
    }
    return L"";
}

static int search_cb(void *ctx, const bd_search_info *r)
{
    (void)ctx;
    if (g_hit_n == g_hit_cap) {
        int cap = g_hit_cap ? g_hit_cap * 2 : 64;
        search_hit *h = realloc(g_hits, sizeof(search_hit) * (size_t)cap);
        if (!h) return 1;
        g_hits = h;
        g_hit_cap = cap;
    }
    g_hits[g_hit_n].source_id = r->source_id;
    g_hits[g_hit_n].rel = xstrdup(r->rel_path);
    g_hit_n++;

    wchar_t *name = widen(r->name), *src = widen(r->source_name), *rel = widen(r->rel_path), *where = widen(r->where);
    wchar_t shown[600], folder[1200];
    swprintf(shown, 600, L"%ls%ls", name ? name : L"", r->is_dir ? L"\\" : L"");
    /* The folder it is in: the source, then the path without the name. */
    swprintf(folder, 1200, L"%ls\\%ls", src ? src : L"", rel ? rel : L"");
    wchar_t *cut = wcsrchr(folder, L'/');
    if (cut) *cut = 0;
    else if ((cut = wcschr(folder, L'\\')) != NULL) cut[0] = 0;
    for (wchar_t *c = folder; *c; c++) if (*c == L'/') *c = L'\\';
    const wchar_t *cells[] = {shown, folder, short_state(r->state), where && *where ? where : L"(no copies)"};
    lv_row(g_results, cells, 4);
    free(name);
    free(src);
    free(rel);
    free(where);
    return 0;
}

static void run_search(void)
{
    wchar_t text[256];
    GetWindowTextW(g_search, text, 256);
    const wchar_t *p = text;
    while (*p == L' ') p++;
    SendMessageW(g_results, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_results);
    clear_hits();
    if (!*p) {
        SendMessageW(g_results, WM_SETREDRAW, TRUE, 0);
        ShowWindow(g_results, SW_HIDE);
        ShowWindow(g_tree, SW_SHOW);
        return;
    }
    char *u = narrow(text);
    if (u) bd_search(g_cat, u, 500, search_cb, NULL);
    free(u);
    if (!g_hit_n) {
        const wchar_t *cells[] = {L"No files or folders match.", L"", L"", L""};
        lv_row(g_results, cells, 4);
    }
    SendMessageW(g_results, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_results, NULL, TRUE);
    ShowWindow(g_tree, SW_HIDE);
    ShowWindow(g_results, SW_SHOW);
}

/* Open the tree down to a file or folder and select it, which also shows
 * its details. */
static void reveal_node(int64_t source_id, const char *rel)
{
    if (g_busy) return;
    HTREEITEM h = TreeView_GetRoot(g_tree);
    for (; h; h = TreeView_GetNextSibling(g_tree, h)) {
        node_ref *r = item_ref(h);
        if (r && r->source_id == source_id) break;
    }
    if (!h) return;
    size_t n = strlen(rel);
    for (size_t i = 1; i <= n; i++) {
        if (rel[i] != '/' && rel[i] != '\0') continue;
        node_ref *pr = item_ref(h);
        if (pr && !pr->loaded) load_children(h, pr);
        TreeView_Expand(g_tree, h, TVE_EXPAND);
        HTREEITEM c = TreeView_GetChild(g_tree, h);
        for (; c; c = TreeView_GetNextSibling(g_tree, c)) {
            node_ref *cr = item_ref(c);
            if (cr && cr->rel && strlen(cr->rel) == i && strncmp(cr->rel, rel, i) == 0) break;
        }
        if (!c) break;
        h = c;
    }
    TreeView_SelectItem(g_tree, h);
    TreeView_EnsureVisible(g_tree, h);
}

/* Leave the results for the tree, keeping what was picked selected. */
static void end_search(void)
{
    KillTimer(g_main, ID_SEARCH_TIMER);
    SetWindowTextW(g_search, L"");
    KillTimer(g_main, ID_SEARCH_TIMER);
    run_search();
    SetFocus(g_tree);
    HTREEITEM sel = TreeView_GetSelection(g_tree);
    if (sel) TreeView_EnsureVisible(g_tree, sel);
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
    int widths[N_BUTTONS] = {S(96), S(70), S(96), S(112), S(90), S(80), S(90), S(104)};
    for (int i = 0; i < N_BUTTONS; i++) {
        MoveWindow(g_buttons[i], x, pad, widths[i], bar, TRUE);
        x += widths[i] + S(6);
    }
    MoveWindow(g_drives, x + S(10), pad + S(7), w - x - S(10) - pad, bar - S(7), TRUE);

    int top = pad + bar + pad;
    int prog_h = S(20);
    int body_h = h - top - log_h - prog_h - pad * 3;
    int tree_w = (w - pad * 3) * 55 / 100, search_h = S(24), below = search_h + S(6);
    MoveWindow(g_search, pad, top, tree_w, search_h, TRUE);
    MoveWindow(g_tree, pad, top + below, tree_w, body_h - below, TRUE);
    MoveWindow(g_results, pad, top + below, tree_w, body_h - below, TRUE);
    int rx = pad * 2 + tree_w, rw = w - rx - pad;
    int detail_h = body_h * 58 / 100;
    MoveWindow(g_detail, rx, top, rw, detail_h, TRUE);
    MoveWindow(g_list, rx, top + detail_h + S(6), rw, body_h - detail_h - S(6), TRUE);
    int py = top + body_h + pad;
    MoveWindow(g_progress, pad, py, S(240), prog_h, TRUE);
    MoveWindow(g_prog_text, pad + S(248), py, w - pad * 2 - S(248), prog_h, TRUE);
    MoveWindow(g_log, pad, py + prog_h + pad, w - pad * 2, log_h, TRUE);
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
    g_buttons[6] = make_button(L"Settings...", ID_BTN_SECURITY);
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

    g_search = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 10, 10,
                               hwnd, (HMENU)ID_SEARCH, g_inst, NULL);
    SendMessageW(g_search, WM_SETFONT, (WPARAM)g_font, TRUE);
    SendMessageW(g_search, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search files and folders by name");
    g_results = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                WS_CHILD | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER, 0, 0,
                                10, 10, hwnd, (HMENU)ID_RESULTS, g_inst, NULL);
    SendMessageW(g_results, WM_SETFONT, (WPARAM)g_font, TRUE);
    {
        static const wchar_t *cols[] = {L"Name", L"Folder", L"State", L"Where it is"};
        static const int widths[] = {150, 130, 120, 320};
        lv_columns(g_results, cols, widths, 4);
    }

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

    g_progress = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE | PBS_SMOOTH, 0, 0, 10, 10, hwnd,
                                 (HMENU)ID_PROGRESS, g_inst, NULL);
    g_prog_text = CreateWindowExW(0, WC_STATICW, L"Ready.", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS | SS_CENTERIMAGE,
                                  0, 0, 10, 10, hwnd, (HMENU)ID_PROGTEXT, g_inst, NULL);
    SendMessageW(g_prog_text, WM_SETFONT, (WPARAM)g_font, TRUE);

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
        create_menu_bar(hwnd);
        return 0;
    case WM_INITMENUPOPUP:
        if ((HMENU)wp == g_menu_local) fill_storage_menus();
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
        if (LOWORD(wp) == ID_SEARCH) {
            if (HIWORD(wp) == EN_CHANGE) SetTimer(hwnd, ID_SEARCH_TIMER, 250, NULL);
            return 0;
        }
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
        default:
            if (LOWORD(wp) >= ID_M_ADD_FOLDER && LOWORD(wp) < ID_MENU_OTHER) menu_command(LOWORD(wp));
            else if (LOWORD(wp) >= ID_DRIVE_ACT_BASE) menu_command(LOWORD(wp));
            break;
        }
        return 0;
    case WM_TIMER:
        if (wp == ID_SEARCH_TIMER) {
            KillTimer(hwnd, ID_SEARCH_TIMER);
            run_search();
        }
        return 0;
    case WM_NOTIFY: {
        NMHDR *nh = (NMHDR *)lp;
        if (nh->idFrom == ID_RESULTS) {
            if (nh->code == LVN_ITEMCHANGED) {
                NMLISTVIEW *lv = (NMLISTVIEW *)lp;
                if ((lv->uNewState & LVIS_SELECTED) && !(lv->uOldState & LVIS_SELECTED) && lv->iItem >= 0 && lv->iItem < g_hit_n)
                    reveal_node(g_hits[lv->iItem].source_id, g_hits[lv->iItem].rel);
            } else if ((nh->code == NM_DBLCLK || nh->code == NM_RETURN) && g_hit_n) {
                end_search();
            }
            return 0;
        }
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
    case WM_APP_PROGRESS: {
        prog_msg *m = (prog_msg *)lp;
        SendMessageW(g_status, SB_SETTEXTW, 0, (LPARAM)m->text);
        show_progress(m->percent, m->text);
        free(m);
        return 0;
    }
    case WM_APP_DONE: {
        job *j = (job *)lp;
        set_busy(0);
        SendMessageW(g_status, SB_SETTEXTW, 0, (LPARAM)j->summary);
        show_progress(j->status == BD_OK ? 100 : 0, j->summary);
        log_append(j->summary);
        if (j->status == BD_ERR_GUARD) {
            /* The backup the guard stopped runs after all once the user owns the changes. */
            if (guard_dialog() && j->kind == JOB_BACKUP) {
                job *again = new_job(JOB_BACKUP);
                if (again) {
                    again->media_id = j->media_id;
                    again->root = xstrdup(j->root);
                    again->label = xstrdup(j->label);
                    again->location = xstrdup(j->location);
                    again->flags = j->flags;
                    enqueue(again);
                }
            }
        } else if (j->status == BD_ERR_PASSPHRASE && (j->kind == JOB_BACKUP || j->kind == JOB_CHECK || j->kind == JOB_VERIFY) &&
            unlock_ui(L"This drive is encrypted. Enter the passphrase to continue.")) {
            job *again = new_job(j->kind);
            if (again) {
                again->media_id = j->media_id;
                again->root = xstrdup(j->root);
                again->label = xstrdup(j->label);
                again->location = xstrdup(j->location);
                again->flags = j->flags;
                enqueue(again);
            }
        } else if (j->status != BD_OK && j->status != BD_ERR_PASSPHRASE)
            report_error(hwnd, j->summary);
        update_drives_label();
        rebuild_tree();
        show_detail();
        if (GetWindowTextLengthW(g_search) > 0) run_search();
        if (j->kind == JOB_DRIVES) after_drives(j);
        else if (j->kind == JOB_BACKUP && j->status == BD_OK) after_backup(j);
        else if (j->kind == JOB_RESTORE && j->status == BD_OK) after_restore(j);
        else if (j->kind == JOB_SHADOW && j->status == BD_OK) after_shadow(j);
        if (j->kind == JOB_SCAN && j->guard) guard_dialog();
        else if ((j->kind == JOB_SCAN || j->kind == JOB_BACKUP) && j->in_use && j->status == BD_OK) offer_shadow(j);
        job_free(j);
        start_next_job();
        return 0;
    }
    case WM_DEVICECHANGE:
        if (wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE) {
            DEV_BROADCAST_HDR *hdr = (DEV_BROADCAST_HDR *)lp;
            if (hdr && hdr->dbch_devicetype == DBT_DEVTYP_VOLUME) {
                job *dj = new_job(JOB_DRIVES);
                if (dj) {
                    dj->auto_run = wp == DBT_DEVICEARRIVAL;
                    enqueue(dj);
                }
            }
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

/* ---- Helper modes ------------------------------------------------------- */

/* brodalf.exe --shadow-copy <dir> <list>: run as administrator by the main
 * window. Copies the files named in list (UTF-8, one per line) out of a
 * shadow copy into dir as 1.bin, 2.bin, ... and writes dir\result.txt. */
static int shadow_helper(const wchar_t *dir, const wchar_t *list)
{
    char *paths[4096];
    int n = 0;
    FILE *f = _wfopen(list, L"rb");
    char line[4096];
    while (f && n < 4096 && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0]) paths[n++] = xstrdup(line);
    }
    if (f) fclose(f);
    int *ok = calloc((size_t)(n ? n : 1), sizeof(int));
    char err[512] = "";
    char *d = narrow(dir);
    int got = ok && d ? bd_shadow_copy_files((const char *const *)paths, n, d, ok, err, sizeof(err)) : -1;
    free(d);
    wchar_t result[MAX_PATH + 16];
    swprintf(result, MAX_PATH + 16, L"%ls\\result.txt", dir);
    FILE *out = _wfopen(result, L"wb");
    if (out) {
        for (int i = 0; i < n; i++) fprintf(out, "%s\n", ok && ok[i] ? "ok" : "failed");
        if (err[0]) fprintf(out, "error: %s\n", err);
        fclose(out);
    }
    bd_applog("shadow copy helper: %d of %d copied%s%s", got < 0 ? 0 : got, n, err[0] ? ": " : "", err);
    for (int i = 0; i < n; i++) free(paths[i]);
    free(ok);
    return got < 0 ? 1 : 0;
}


/* brodalf.exe --check <catalog>: run by Task Scheduler. Scans the folders
 * without a window, then speaks up only when something needs backing up (or
 * the guard tripped): a note saying which disk to plug in, with a button to
 * open BRODALF. Returns 1 to go on into the window with the catalog open. */
static int scheduled_check(const wchar_t *path)
{
    char *p = narrow(path);
    if (!p) return 0;
    if (bd_catalog_file_needs_passphrase(p)) {
        bd_applog("scheduled check: %s is encrypted and needs the passphrase; skipped", p);
        free(p);
        return 0;
    }
    bd_status s = bd_catalog_open(p, &g_cat);
    free(p);
    if (s != BD_OK) {
        bd_applog("scheduled check: cannot open the catalog (%s); skipped", bd_open_error());
        return 0;
    }
    wcsncpy(g_cat_path, path, MAX_PATH * 2 - 1);
    bd_scan_stats ss;
    bd_scan(g_cat, &ss, NULL, NULL);
    bd_risk_stats rs;
    memset(&rs, 0, sizeof(rs));
    bd_list_at_risk(g_cat, 0, NULL, NULL, &rs);
    bd_guard_info g;
    bd_guard_get(g_cat, &g);
    bd_catalog_save(g_cat);
    bd_applog("scheduled check: %lld files scanned, %lld at risk, guard %s", (long long)ss.files_seen, (long long)rs.files_at_risk,
              g.tripped ? "tripped" : "ok");
    if (!rs.files_at_risk && !g.tripped) {
        bd_catalog_close(g_cat);
        g_cat = NULL;
        return 0;
    }
    wtext msg = {NULL, 0, 0};
    bd_target t;
    bd_target_get(g_cat, &t);
    wchar_t size[64];
    format_bytes(rs.bytes_at_risk, size, 64);
    if (g.tripped)
        wadd(&msg, L"BRODALF has paused backups: a scan found %lld of %lld files changed or gone at once, which is what ransomware "
                   L"does. Open BRODALF to restore them as they were, or to say the changes are yours.\n\n",
             (long long)(g.files_changed + g.files_deleted), (long long)g.files_total);
    if (rs.files_at_risk) {
        wadd(&msg, L"%lld of %lld files (%ls) have fewer than %d copies in %d places.\n\n", (long long)rs.files_at_risk,
             (long long)rs.files_total, size, t.copies, t.places);
        bd_risk_help h;
        if (bd_suggest_drive(g_cat, &h) == BD_OK) {
            wchar_t name[600], fb[64];
            drive_name(h.label, h.location, name, 600);
            format_bytes(h.free_bytes >= 0 ? h.free_bytes : 0, fb, 64);
            if (h.connected) wadd(&msg, L"%ls is plugged in and has %ls free%ls.", name, fb, h.fits ? L", room for all of it" : L"");
            else if (h.free_bytes >= 0)
                wadd(&msg, L"Plug in %ls: it had %ls free when last seen%ls.", name, fb,
                     h.fits ? L", room for all of it" : L", not enough for all of it");
            else wadd(&msg, L"Plug in %ls.", name);
        } else {
            wadd(&msg, L"None of your disks would help; set up another one.");
        }
    }
    wadd(&msg, L"\n\nOpen BRODALF now?");
    int open = MessageBoxW(NULL, msg.p, APP_NAME, MB_ICONINFORMATION | MB_YESNO | MB_TOPMOST | MB_SETFOREGROUND) == IDYES;
    free(msg.p);
    if (!open) {
        bd_catalog_close(g_cat);
        g_cat = NULL;
        return 0;
    }
    reg_set_last(path);
    return 1;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    (void)prev;
    (void)cmd;
    g_inst = inst;
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    char *log_path = bd_applog_default_path();
    bd_applog_open(log_path);
    free(log_path);
    SetUnhandledExceptionFilter(on_crash);

    /* The shadow copy helper runs before CoInitializeEx: it needs the
     * multithreaded COM apartment, and VSS hangs in a single-threaded one. */
    if (argc >= 4 && wcscmp(argv[1], L"--shadow-copy") == 0) {
        int rc = shadow_helper(argv[2], argv[3]);
        LocalFree(argv);
        return rc;
    }
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_TREEVIEW_CLASSES | ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES |
                                                 ICC_PROGRESS_CLASS | ICC_DATE_CLASSES};
    InitCommonControlsEx(&icc);
    HDC screen = GetDC(NULL);
    g_dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(NULL, screen);
    make_fonts();
    bd_applog("BRODALF %s started", bd_version());

    int opened;
    if (argc >= 3 && wcscmp(argv[1], L"--check") == 0) opened = scheduled_check(argv[2]);
    else opened = open_catalog(argc > 1 && argv[1][0] ? argv[1] : NULL);
    LocalFree(argv);
    if (!opened) return 0;
    /* Keep the scheduled task pointing at this copy of BRODALF. */
    if (bd_option_get(g_cat, "schedule") > 0) apply_schedule(bd_option_get(g_cat, "schedule"), 0);

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
    job *first = new_job(JOB_DRIVES);
    if (first) {
        first->startup = 1;
        enqueue(first);
    }
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
