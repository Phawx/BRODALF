/* BRODALF for Windows: the ghost-tree window.
 *
 * Every file and folder in the backup roots is shown. Files are greyed out
 * until a correct copy is on a drive that is plugged in right now. Long jobs
 * (scan, backup, check, restore, looking for drives) run one at a time on a
 * worker thread, only when the user starts them; the window stays
 * responsive and shows progress for the file, the drive and every one of
 * the full copies the user wants. */
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

#define COBJMACROS
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <dbt.h>
#include <sddl.h>
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
    ID_BTN_STOP,
    /* The menu bar. */
    ID_M_ADD_FOLDER = 700,
    ID_M_SCAN,
    ID_M_RESTORE,
    ID_M_RISK,
    ID_M_EXIT,
    ID_M_ADD_DISK,
    ID_M_DRIVES,
    ID_M_SETTINGS,
    ID_M_VERIFY_7,
    ID_M_VERIFY_30,
    ID_M_VERIFY_90,
    ID_M_VERIFY_NEVER,
    ID_M_LOG,
    ID_M_REPORTS,
    ID_M_ABOUT,
    ID_M_RELOCATE,
    ID_M_SHEET,
    ID_M_DELETED,
    ID_M_STATUS_ALL,
    ID_M_IN_USE,
    ID_M_EXPORT,
    ID_M_SCAN_FIRST, /* a backup warning's "scan first" button */
    ID_M_BITROT,
    ID_M_LARGEST,
    ID_M_DAMAGED,
    ID_M_CONFLICTS,
    ID_M_NEW_DB,
    ID_M_OPEN_DB,
    ID_M_RECOVER_DB,
    ID_M_RESTORE_DELETED,
    /* Right-click menus (TrackPopupMenu with TPM_RETURNCMD). */
    ID_CTX_OPEN = 800,
    ID_CTX_RESTORE,
    ID_CTX_STOP_ROOT,
    ID_CTX_RESUME_ROOT,
    ID_CTX_ROOT_SKIP,
    ID_CTX_RELOCATE,
    ID_CTX_RESTORE_VERSION,
    ID_CTX_OPEN_VERSION,
    ID_MENU_SET_PASS = 910,
    ID_MENU_ENCRYPT_CATALOG,
    ID_MENU_LOCK,
    ID_MENU_CHECK_90,
    ID_MENU_CHECK_180,
    ID_MENU_CHECK_365,
    ID_MENU_CHECK_NEVER,
    ID_MENU_RESERVE_256,
    ID_MENU_RESERVE_512,
    ID_MENU_RESERVE_1024,
    ID_MENU_RESERVE_2048,
    ID_MENU_LOW_PRIORITY,
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
/* The bar for the file being copied, and one bar per full copy the user
 * wants (the target's copies): how far each set of copies has got. */
static HWND g_file_progress, g_file_text;
#define MAX_SET_BARS 6
static HWND g_set_bar[MAX_SET_BARS], g_set_text[MAX_SET_BARS];
static int g_set_count;
static HMENU g_menu_local;
#define N_BUTTONS 8
static HWND g_buttons[N_BUTTONS];
static HFONT g_font, g_font_italic, g_font_strike, g_font_bold;
static int g_dpi = 96;
static bd_catalog *g_cat;
static wchar_t g_cat_path[MAX_PATH * 2];
static volatile LONG g_busy;
/* Stop pressed: the running job is asked to stop, and closes the window
 * once it has when g_close_after is set. */
static HWND g_stop;
static volatile LONG g_stopping;
static int g_close_after, g_running_kind;
/* >0 while the UI thread is in the middle of a command or of what comes
 * after a job (its end-of-job prompts), perhaps waiting in a message box:
 * the catalog is the UI's then, so no job may start. Jobs asked for
 * meanwhile, such as a drive being plugged in, wait in the queue and start
 * when the UI is done. */
static int g_ui_depth;

static int S(int px) { return MulDiv(px, g_dpi, 96); }

/* ---- Jobs --------------------------------------------------------------- */

typedef enum { JOB_DRIVES, JOB_SCAN, JOB_BACKUP, JOB_CHECK, JOB_RESTORE, JOB_VERIFY } job_kind;
#define SUMMARY_LEN 768

/* A file a restore left alone because another file was already where it
 * would go. Collected over every part of one restore. */
typedef struct {
    wchar_t *where;     /* root and path */
    wchar_t *dest;
    int64_t want_size, want_mtime_ns, have_size, have_mtime_ns;
} conflict_row;

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
    int plugged_in;     /* drives: Windows said a drive was plugged in */
    int startup;        /* drives: the first look when BRODALF opens */
    DWORD arrived;      /* drives: the drive letters Windows said arrived */
    wchar_t other[8][4]; /* drives: of those, disks that are not BRODALF drives */
    int n_other;
    int bitrot;         /* scan: also re-read every unchanged file */
    int after_open;     /* scan: the scan that runs when a catalog is opened */
    int64_t then_backup; /* scan: then back up to this drive */
    int64_t new_ids[16]; /* drives: drives connected by this job */
    int n_new;
    int64_t no_room, bytes_no_room; /* backup: what did not fit */
    int64_t offline;    /* restore: files on drives not plugged in */
    int deleted;        /* restore: only files deleted from their folder */
    int64_t version_id; /* restore: this one version of a file, into dest */
    int n_picks;        /* restore: these files or folders instead of source_id/rel */
    int64_t *pick_src;
    char **pick_rel;
    char **pick_dest;   /* restore: where each pick goes (NULL: dest) */
    conflict_row *conf; /* restore: files left alone */
    int n_conf, cap_conf;
    int64_t in_use;     /* scan/backup: files another program had open */
    int64_t damaged;    /* scan: files changed without their size or time changing */
    int64_t found_deleted; /* scan: files gone from their folder */
    int64_t roots_missing; /* scan: backup roots not found */
    int problems;
    bd_status status;
    wchar_t summary[SUMMARY_LEN];
    struct job *next;
} job;

static job *g_queue;

static void start_backup(job *b, int offer_scan);
static void free_conflicts(conflict_row *c, int n);

static void job_free(job *j)
{
    if (!j) return;
    free(j->root);
    free(j->label);
    free(j->location);
    free(j->rel);
    free(j->dest);
    for (int i = 0; i < j->n_picks; i++) {
        free(j->pick_rel[i]);
        if (j->pick_dest) free(j->pick_dest[i]);
    }
    free(j->pick_rel);
    free(j->pick_dest);
    free(j->pick_src);
    free_conflicts(j->conf, j->n_conf);
    free(j);
}

static void free_conflicts(conflict_row *c, int n)
{
    for (int i = 0; i < n; i++) {
        free(c[i].where);
        free(c[i].dest);
    }
    free(c);
}

/* Append to a job's summary. */
static void say(job *j, const wchar_t *fmt, ...)
{
    size_t len = wcslen(j->summary);
    if (len + 1 >= SUMMARY_LEN) return;
    va_list ap;
    va_start(ap, fmt);
    vswprintf(j->summary + len, SUMMARY_LEN - len, fmt, ap);
    va_end(ap);
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

/* What the progress bars show: percent (-1: unknown, keep it moving) and a
 * line with the counts, the data rate and the time left; and for the file
 * being worked on, its own percent (-1: no bar) and name. */
typedef struct { int percent, file_percent; wchar_t text[1024], file[600]; } prog_msg;

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

/* "about 3 minutes left" from what is left and the rate. */
static void format_left(int64_t bytes_left, double rate, wchar_t *out, size_t n)
{
    out[0] = 0;
    if (rate <= 0 || bytes_left <= 0) return;
    double secs = (double)bytes_left / rate;
    if (secs < 60) swprintf(out, n, L", less than a minute left");
    else if (secs < 3600) swprintf(out, n, L", about %d minute%ls left", (int)(secs / 60 + 0.5), (int)(secs / 60 + 0.5) == 1 ? L"" : L"s");
    else swprintf(out, n, L", about %.1f hours left", secs / 3600.0);
}

static void job_progress(void *ctx, const bd_progress *p)
{
    (void)ctx;
    /* Each core job clears the request when it starts; ask again. */
    if (g_stopping) bd_catalog_cancel(g_cat);
    if (p->files_done == 0 && p->bytes_done == 0) rate_reset();
    prog_msg *m = calloc(1, sizeof(*m));
    if (!m) return;
    const char *ph = p->phase;
    const wchar_t *verb = strcmp(ph, "scan") == 0 ? L"Scanning" : strcmp(ph, "backup") == 0 ? L"Backing up"
                        : strcmp(ph, "restore") == 0 ? L"Restoring" : strcmp(ph, "verify") == 0 ? L"Reading back copies"
                        : L"Checking copies";
    const wchar_t *unit = strcmp(ph, "scan") == 0 ? L"files looked at" : strcmp(ph, "backup") == 0 ? L"files copied"
                        : strcmp(ph, "restore") == 0 ? L"files restored" : L"copies read";
    wchar_t done[64], total[64], rate[64] = L"", left[64] = L"", *c = widen(p->current);
    format_bytes(p->bytes_done, done, 64);
    double r = p->done ? -1.0 : rate_add(p->bytes_done);
    if (r >= 0) { format_bytes((int64_t)r, rate, 64); wcsncat(rate, L"/s", 63 - wcslen(rate)); }
    if (p->bytes_total > 0) {
        format_bytes(p->bytes_total, total, 64);
        format_left(p->bytes_total - p->bytes_done, r, left, 64);
        m->percent = (int)(p->bytes_done * 100 / p->bytes_total);
        swprintf(m->text, 1024, L"%ls: %lld of %lld %ls, %ls of %ls%ls%ls%ls.", verb, (long long)p->files_done,
                 (long long)p->files_total, unit, done, total, *rate ? L", " : L"", rate, left);
    } else if (p->files_total > 0) {
        m->percent = (int)(p->files_done * 100 / p->files_total);
        swprintf(m->text, 1024, L"%ls: %lld of about %lld %ls, %ls read%ls%ls.", verb, (long long)p->files_done,
                 (long long)p->files_total, unit, done, *rate ? L" at " : L"", rate);
    } else {
        m->percent = -1;
        swprintf(m->text, 1024, L"%ls: %lld %ls, %ls read%ls%ls.", verb, (long long)p->files_done, unit, done,
                 *rate ? L" at " : L"", rate);
    }
    if (p->done) m->percent = 100;
    if (m->percent > 100) m->percent = 100;
    m->file_percent = -1;
    if (c && *c) {
        if (p->file_bytes_total > 0) {
            wchar_t fd[64], ft[64];
            format_bytes(p->file_bytes_done, fd, 64);
            format_bytes(p->file_bytes_total, ft, 64);
            m->file_percent = (int)(p->file_bytes_done * 100 / p->file_bytes_total);
            if (m->file_percent > 100) m->file_percent = 100;
            swprintf(m->file, 600, L"%ls (%ls of %ls)", c, fd, ft);
        } else {
            swprintf(m->file, 600, L"%ls", c);
        }
    }
    free(c);
    if (!PostMessageW(g_main, WM_APP_PROGRESS, 0, (LPARAM)m)) free(m);
}

static void set_bar(HWND bar, int percent)
{
    LONG_PTR style = GetWindowLongPtrW(bar, GWL_STYLE);
    if (percent < 0) {
        if (!(style & PBS_MARQUEE)) SetWindowLongPtrW(bar, GWL_STYLE, style | PBS_MARQUEE);
        SendMessageW(bar, PBM_SETMARQUEE, TRUE, 50);
    } else {
        if (style & PBS_MARQUEE) {
            SendMessageW(bar, PBM_SETMARQUEE, FALSE, 0);
            SetWindowLongPtrW(bar, GWL_STYLE, style & ~PBS_MARQUEE);
        }
        SendMessageW(bar, PBM_SETRANGE32, 0, 100);
        SendMessageW(bar, PBM_SETPOS, percent, 0);
    }
}

/* The file bar: percent -1 with text clears it. */
static void show_file_progress(int percent, const wchar_t *text)
{
    SendMessageW(g_file_progress, PBM_SETRANGE32, 0, 100);
    SendMessageW(g_file_progress, PBM_SETPOS, percent < 0 ? 0 : percent, 0);
    SetWindowTextW(g_file_text, text ? text : L"");
}

/* The bar and its line. */
static void show_progress(int percent, const wchar_t *text)
{
    set_bar(g_progress, percent);
    SetWindowTextW(g_prog_text, text);
}

/* Is root a drive for this catalog? Connects it: reads its BRODALF.media
 * file, free space and health data, and checks no copies. */
static int try_connect(job *j, const char *root, int64_t *media_id)
{
    return bd_media_connect(g_cat, root, media_id, job_log, j) == BD_OK;
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
        if (is_brodalf_drive(root)) {
            if (try_connect(j, root, &id)) found++;
        } else if ((j->arrived & (1u << i)) && j->n_other < 8 && (type == DRIVE_REMOVABLE || type == DRIVE_FIXED)) {
            /* A disk that was just plugged in and is not one of this
             * catalog's drives: offer to set it up. */
            wcscpy(j->other[j->n_other++], wroot);
        }
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
    swprintf(j->summary, SUMMARY_LEN, found ? L"Found %d BRODALF drive(s)." : L"No BRODALF drives are plugged in.", found);
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

static int conflict_cb(void *ctx, const bd_conflict_info *c)
{
    job *j = ctx;
    if (j->n_conf >= 100000) return 1;
    if (j->n_conf == j->cap_conf) {
        int cap = j->cap_conf ? j->cap_conf * 2 : 64;
        conflict_row *n = realloc(j->conf, sizeof(*n) * (size_t)cap);
        if (!n) return 1;
        j->conf = n;
        j->cap_conf = cap;
    }
    conflict_row *r = &j->conf[j->n_conf++];
    size_t len = strlen(c->source_name) + strlen(c->rel_path) + 4;
    char *where = malloc(len);
    if (where) snprintf(where, len, "%s\\%s", c->source_name, c->rel_path);
    r->where = widen(where);
    free(where);
    r->dest = widen(c->dest);
    r->want_size = c->want_size;
    r->want_mtime_ns = c->want_mtime_ns;
    r->have_size = c->have_size;
    r->have_mtime_ns = c->have_mtime_ns;
    return 0;
}

static void collect_conflicts(job *j)
{
    bd_list_restore_conflicts(g_cat, conflict_cb, j);
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
        bd_scan_opts so;
        memset(&st, 0, sizeof(st));
        memset(&so, 0, sizeof(so));
        so.bitrot = j->bitrot;
        s = bd_scan_ex(g_cat, &so, &st, job_log, j);
        format_bytes(st.bytes_hashed, size, 64);
        swprintf(j->summary, SUMMARY_LEN, L"%ls done: %lld files, %lld new, %lld changed, %lld moved or renamed, %lld deleted, %ls read.",
                 j->bitrot ? L"Bit-rot scan" : L"Scan", (long long)st.files_seen, (long long)st.files_new,
                 (long long)st.files_changed, (long long)st.files_moved, (long long)st.files_deleted, size);
        if (st.deleted_no_copy) say(j, L" %lld of the deleted files were never backed up.", (long long)st.deleted_no_copy);
        if (st.skipped) say(j, L" %lld left out by the skip lists.", (long long)st.skipped);
        if (st.files_in_use) say(j, L" %lld in use by another program.", (long long)st.files_in_use);
        if (st.files_online_only) say(j, L" %lld online-only cloud files left alone (not downloaded).", (long long)st.files_online_only);
        if (st.files_damaged)
            say(j, L" %lld file%ls changed without their size or time changing: they may be damaged.", (long long)st.files_damaged,
                st.files_damaged == 1 ? L"" : L"s");
        if (st.roots_missing) say(j, L" %lld backup root%ls could not be found.", (long long)st.roots_missing, st.roots_missing == 1 ? L"" : L"s");
        if (st.roots_stopped) say(j, L" %lld stopped root%ls left alone.", (long long)st.roots_stopped, st.roots_stopped == 1 ? L"" : L"s");
        j->in_use = st.files_in_use;
        j->damaged = st.files_damaged;
        j->found_deleted = st.files_deleted;
        j->roots_missing = st.roots_missing;
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
        if (s == BD_OK && g_stopping) s = BD_ERR_CANCELLED;
        if (s == BD_OK) {
            bd_backup_stats bs;
            memset(&bs, 0, sizeof(bs));
            /* Copies everything missing from this drive, then saves the
             * catalog and puts its newest revision on the drive. */
            s = bd_backup(g_cat, id, 0, &bs, job_log, j);
            format_bytes(bs.bytes_copied, size, 64);
            swprintf(j->summary, SUMMARY_LEN, L"Backup done: %lld files copied (%ls), %lld already there, %lld failed.",
                     (long long)bs.files_copied, size, (long long)bs.files_already_there, (long long)bs.files_failed);
            if (bs.copies_alongside)
                say(j, L" %lld went alongside an older copy (under .versions on the drive): nothing there was overwritten.",
                    (long long)bs.copies_alongside);
            if (bs.files_rehashed) say(j, L" %lld changed since the scan and were read again first.", (long long)bs.files_rehashed);
            if (bs.files_in_use) say(j, L" %lld in use by another program.", (long long)bs.files_in_use);
            if (bs.files_online_only) say(j, L" %lld online-only cloud files not copied.", (long long)bs.files_online_only);
            if (bs.files_no_room) {
                format_bytes(bs.bytes_no_room, size, 64);
                say(j, L" The drive is full: %lld file%ls (%ls) did not fit and wait for another drive.", (long long)bs.files_no_room,
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
        memset(&cs, 0, sizeof(cs));
        int days = bd_option_get(g_cat, "verify_days");
        s = bd_media_verify(g_cat, j->media_id, days, &cs, job_log, j);
        if (cs.rehashed == 0 && s == BD_OK)
            swprintf(j->summary, SUMMARY_LEN, L"Read back: every copy on the drive was read back within the last %d days.", days);
        else
            swprintf(j->summary, SUMMARY_LEN, L"Read back %lld copies: %lld good, %lld damaged, %lld missing.%ls",
                     (long long)cs.rehashed, (long long)cs.ok, (long long)cs.bad, (long long)cs.missing,
                     cs.bad || cs.missing ? L" The next backup to this drive writes them again." : L"");
        if (cs.skipped) say(j, L" %lld encrypted copies need the passphrase to check.", (long long)cs.skipped);
        break;
    }
    case JOB_CHECK: {
        bd_check_stats cs;
        memset(&cs, 0, sizeof(cs));
        s = bd_media_check(g_cat, j->media_id, &cs, job_log, j);
        swprintf(j->summary, SUMMARY_LEN, L"Check done: %lld copies read, %lld good, %lld missing, %lld damaged.%ls",
                 (long long)cs.copies, (long long)cs.ok, (long long)cs.missing,
                 (long long)cs.bad, cs.bad || cs.missing ? L" The next backup to this drive writes them again." : L"");
        if (cs.skipped) say(j, L" %lld encrypted copies need the passphrase to check.", (long long)cs.skipped);
        break;
    }
    case JOB_RESTORE: {
        bd_restore_stats rs;
        bd_restore_opts ro;
        memset(&rs, 0, sizeof(rs));
        memset(&ro, 0, sizeof(ro));
        ro.deleted_only = j->deleted;
        if (j->version_id) {
            s = bd_restore_version(g_cat, j->version_id, j->dest, &rs, job_log, j);
            if (s == BD_OK) collect_conflicts(j);
        } else if (!j->n_picks) {
            s = bd_restore_ex(g_cat, j->source_id, j->rel, j->dest, &ro, &rs, job_log, j);
            if (s == BD_OK) collect_conflicts(j);
        }
        for (int i = 0; i < j->n_picks && s == BD_OK; i++) {
            bd_restore_stats one;
            memset(&one, 0, sizeof(one));
            const char *to = j->pick_dest && j->pick_dest[i] ? j->pick_dest[i] : j->dest;
            s = bd_restore_ex(g_cat, j->pick_src[i], j->pick_rel[i], to, &ro, &one, job_log, j);
            /* Each restore starts its list of conflicts afresh: keep them all. */
            if (s == BD_OK) collect_conflicts(j);
            rs.files_restored += one.files_restored;
            rs.files_offline += one.files_offline;
            rs.files_no_copy += one.files_no_copy;
            rs.files_failed += one.files_failed;
            rs.files_need_passphrase += one.files_need_passphrase;
            rs.bytes_restored += one.bytes_restored;
            rs.files_already_there += one.files_already_there;
            rs.files_conflict += one.files_conflict;
        }
        format_bytes(rs.bytes_restored, size, 64);
        swprintf(j->summary, SUMMARY_LEN, L"Restore done: %lld files (%ls), %lld already there, %lld on drives that are not plugged in, %lld never backed up, %lld failed.",
                 (long long)rs.files_restored, size, (long long)rs.files_already_there, (long long)rs.files_offline,
                 (long long)rs.files_no_copy, (long long)rs.files_failed);
        if (rs.files_conflict)
            say(j, L" %lld left alone because another file was already there: nothing was overwritten.", (long long)rs.files_conflict);
        j->offline = rs.files_offline;
        if (rs.files_need_passphrase) say(j, L" %lld need the passphrase.", (long long)rs.files_need_passphrase);
        break;
    }
    }
    j->status = s;
    if (s == BD_ERR_CANCELLED) {
        swprintf(j->summary, SUMMARY_LEN, L"Stopped. What was done until then is kept.");
    } else if (s != BD_OK) {
        wchar_t *e = widen(bd_catalog_error(g_cat));
        swprintf(j->summary, SUMMARY_LEN, L"Stopped: %ls", e ? e : L"unknown error");
        free(e);
    }
    if (bd_catalog_save(g_cat) != BD_OK) {
        wchar_t *e = widen(bd_catalog_error(g_cat));
        if (j->status == BD_OK) {
            j->status = BD_ERR_IO;
            swprintf(j->summary, SUMMARY_LEN, L"Could not save the database: %ls", e ? e : L"unknown error");
        } else {
            post_text(WM_APP_LOG, L"Could not save the database:");
            if (e) post_text(WM_APP_LOG, e);
        }
        free(e);
    }
    bd_catalog_set_progress(g_cat, NULL, NULL);
    PostMessageW(g_main, WM_APP_DONE, 0, (LPARAM)j);
    return 0;
}

static void update_menus(void);

/* The buttons work only with a database open and no job running. */
static void set_busy(int busy)
{
    InterlockedExchange(&g_busy, busy);
    for (int i = 0; i < N_BUTTONS; i++) EnableWindow(g_buttons[i], !busy && g_cat);
    EnableWindow(g_stop, busy);
    ShowWindow(g_stop, busy ? SW_SHOW : SW_HIDE);
    update_menus();
}

static void start_next_job(void)
{
    if (g_busy || g_ui_depth || !g_queue) return;
    job *j = g_queue;
    g_queue = j->next;
    j->next = NULL;
    InterlockedExchange(&g_stopping, 0);
    g_running_kind = j->kind;
    set_busy(1);
    static const wchar_t *starting[] = {L"Looking for BRODALF drives...", L"Scanning your backup roots...",
                                        L"Backing up...", L"Checking every copy on the drive...", L"Restoring...",
                                        L"Reading back copies not read lately..."};
    const wchar_t *what = j->kind == JOB_SCAN && j->bitrot ? L"Bit-rot scan: reading every file again..." : starting[j->kind];
    SendMessageW(g_status, SB_SETTEXTW, 0, (LPARAM)what);
    show_progress(-1, what);
    show_file_progress(-1, L"");
    HANDLE h = CreateThread(NULL, 0, worker, j, 0, NULL);
    if (h) CloseHandle(h);
    else { set_busy(0); job_free(j); }
}

/* Ask the running job to stop, and drop the ones waiting after it. */
static void stop_job(void)
{
    if (!g_busy) return;
    InterlockedExchange(&g_stopping, 1);
    bd_catalog_cancel(g_cat);
    while (g_queue) {
        job *q = g_queue;
        g_queue = q->next;
        job_free(q);
    }
    EnableWindow(g_stop, FALSE);
    SendMessageW(g_status, SB_SETTEXTW, 0, (LPARAM)L"Stopping...");
    show_progress(-1, L"Stopping...");
}

static void enqueue(job *j)
{
    if (!j) return;
    /* Don't stack up drive scans; one pending is enough. */
    if (j->kind == JOB_DRIVES)
        for (job *q = g_queue; q; q = q->next)
            if (q->kind == JOB_DRIVES) {
                q->plugged_in |= j->plugged_in;
                q->arrived |= j->arrived;
                job_free(j);
                return;
            }
    /* Likewise folder scans: one waiting to run covers this one. */
    if (j->kind == JOB_SCAN)
        for (job *q = g_queue; q; q = q->next)
            if (q->kind == JOB_SCAN && q->bitrot >= j->bitrot) {
                if (j->then_backup && !q->then_backup) q->then_backup = j->then_backup;
                q->after_open |= j->after_open;
                job_free(j);
                return;
            }
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
    if (!g_cat) {
        TreeView_DeleteAllItems(g_tree);
        return;
    }
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
        swprintf(text, 1024, L"%ls   %ls   [%ls]%ls", counts, bytes, wp,
                 l.items[i].missing ? L"   - not found: unplugged, moved or renamed?"
                 : !l.items[i].active ? L"   - no longer backed up" : L"");
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

/* What each row of the versions list is, for its right-click menu. */
typedef struct { int64_t version_id; int connected; wchar_t *full; } copy_row;
static copy_row *g_copy_rows;
static int g_n_copy_rows;

static void clear_copy_rows(void)
{
    for (int i = 0; i < g_n_copy_rows; i++) free(g_copy_rows[i].full);
    free(g_copy_rows);
    g_copy_rows = NULL;
    g_n_copy_rows = 0;
}

static int add_copy_row(void *ctx, const bd_copy_info *c)
{
    int *row = ctx;
    wchar_t buf[512];
    LVITEMW it;
    memset(&it, 0, sizeof(it));
    it.mask = LVIF_TEXT | LVIF_PARAM;
    it.iItem = *row;
    it.lParam = *row;
    swprintf(buf, 512, c->is_current ? L"v%d (current)" : L"v%d", c->version_no);
    it.pszText = buf;
    SendMessageW(g_list, LVM_INSERTITEMW, 0, (LPARAM)&it);
    it.mask = LVIF_TEXT;

    wchar_t *where = widen(c->media_id ? c->media_label : "no copy yet"), where_buf[300];
    swprintf(where_buf, 300, L"%ls%ls", where ? where : L"", c->media_id && c->encrypted ? L" (encrypted)" : L"");
    it.iSubItem = 1;
    it.pszText = where_buf;
    SendMessageW(g_list, LVM_SETITEMTEXTW, *row, (LPARAM)&it);
    free(where);

    if (!c->media_id) swprintf(buf, 512, L"");
    else if (strcmp(c->copy_state, "ok") != 0) swprintf(buf, 512, strcmp(c->copy_state, "missing") == 0 ? L"Missing from drive" : L"Damaged");
    else if (c->media_lost) swprintf(buf, 512, L"Drive marked lost or broken: does not count");
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
    int64_t last_seen_ms, hw_read_ms, total, freeb, copies, added_ms, space_ms;
    int64_t lost_ms, oldest_check_ms, bytes_held, reserve_bytes;
    char lost_why[16];
    int check_due;
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
    d->space_ms = m->space_ms;
    d->lost_ms = m->lost_ms;
    snprintf(d->lost_why, sizeof(d->lost_why), "%s", m->lost_why ? m->lost_why : "");
    d->oldest_check_ms = m->oldest_check_ms;
    d->check_due = m->check_due;
    d->bytes_held = m->bytes_held;
    d->reserve_bytes = m->reserve_bytes;
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

/* What the drive's own health data says about it, or NULL. */
static const wchar_t *replace_advice(const drive_snap *d)
{
    if (!d->has_hw || d->lost_ms) return NULL;
    const bd_drive_hw *h = &d->hw;
    if (strcmp(h->health, "failing") == 0)
        return L"The drive says it is failing. Set up another drive and back up to it; then mark this one broken.";
    if (h->percent_used >= 90) return L"The drive is nearly worn out. Plan to set up another drive.";
    if (strcmp(h->health, "warning") == 0 || h->pending_sectors > 0 || h->uncorrectable_sectors > 0)
        return L"The drive reports problems reading some of its sectors. Plan to set up another drive.";
    return NULL;
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
    const wchar_t *advice = replace_advice(d);
    if (advice) wadd(t, L"%ls%ls\r\n", indent, advice);
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
    if (d->lost_ms) {
        wchar_t lost[64];
        format_time_ms(d->lost_ms, lost, 64);
        wadd(t, L"%ls    Marked %ls %ls: its copies no longer count and nothing is backed up to it\r\n", indent,
             strcmp(d->lost_why, "broken") == 0 ? L"broken" : L"lost", lost);
    } else if (strcmp(d->kind, "drive") == 0 && d->copies) {
        wchar_t held[64], checked[64];
        format_bytes(d->bytes_held, held, 64);
        format_time_ms(d->oldest_check_ms, checked, 64);
        wadd(t, L"%ls    Holds %ls of good copies; the oldest was last read back %ls%ls\r\n", indent, held, checked,
             d->check_due ? L" (due for a check)" : L"");
    }
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

/* Where a copy is on a plugged-in drive: <root>\BRODALF\<catalog id>\<path>. */
static wchar_t *copy_full_path(const drive_snap *d, const char *path_on_media)
{
    size_t n = strlen(d->root) + strlen(path_on_media) + 64;
    char *p = malloc(n);
    if (!p) return NULL;
    size_t rl = strlen(d->root);
    snprintf(p, n, "%s%sBRODALF\\%s\\%s", d->root, rl && d->root[rl - 1] == '\\' ? "" : "\\", bd_catalog_uuid(g_cat), path_on_media);
    for (char *c = p; *c; c++) if (*c == '/') *c = '\\';
    wchar_t *w = widen(p);
    free(p);
    return w;
}

static void show_detail(void)
{
    SendMessageW(g_list, LVM_DELETEALLITEMS, 0, 0);
    clear_copy_rows();
    if (!g_cat) { SetWindowTextW(g_detail, L""); return; }
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
        g_copy_rows = calloc((size_t)(cl.n ? cl.n : 1), sizeof(copy_row));
        for (int i = 0; i < cl.n && g_copy_rows; i++) {
            int row = i;
            const bd_copy_info *c = copy_of(&cl.items[i]);
            add_copy_row(&row, c);
            g_copy_rows[i].version_id = c->version_id;
            g_copy_rows[i].connected = c->media_id && c->connected;
            const drive_snap *d = c->media_id ? find_drive(&dl, c->media_id) : NULL;
            if (d && c->connected && strcmp(c->copy_state, "ok") == 0 && c->path_on_media && *c->path_on_media)
                g_copy_rows[i].full = copy_full_path(d, c->path_on_media);
            g_n_copy_rows = i + 1;
        }
        wadd(&t, L"\r\nRight-click a version below to restore it or to open it on its drive.\r\n");
        free(cl.items);
        free(dl.items);
    }
    SetWindowTextW(g_detail, t.p ? t.p : L"");
    free(t.p);
}

/* ---- Drive details dialog ------------------------------------------------ */

static void update_drives_label(void);

static void media_name(int64_t id, wchar_t *out, size_t cap);

/* How a job ended, in words. */
static const wchar_t *job_end_words(const char *status, int64_t finished_ms)
{
    if (!finished_ms || strcmp(status, "running") == 0) return L"did not finish (BRODALF was closed during it)";
    if (strcmp(status, "done") == 0) return L"finished";
    if (strcmp(status, "partial") == 0) return L"finished with problems";
    if (strcmp(status, "stopped") == 0) return L"stopped";
    return L"failed";
}

/* Where a drive stands, from the catalog: what is on it as it is now, what
 * is behind, its last backup and its last check. */
static void add_drive_status(wtext *t, int64_t id, const wchar_t *indent)
{
    bd_media_status ms;
    if (bd_media_status_get(g_cat, id, &ms) != BD_OK) return;
    wchar_t a[64], when[64];
    wadd(t, L"%lsWhere it stands\r\n", indent);
    format_bytes(ms.bytes_current, a, 64);
    wadd(t, L"%ls    %lld files as they are now (%ls)\r\n", indent, (long long)ms.files_current, a);
    if (ms.files_behind) {
        format_bytes(ms.bytes_behind, a, 64);
        wadd(t, L"%ls    %lld changed since (%ls): only an older version is on it\r\n", indent, (long long)ms.files_behind, a);
    }
    if (ms.files_not_here) {
        format_bytes(ms.bytes_not_here, a, 64);
        wadd(t, L"%ls    %lld not on it at all (%ls)\r\n", indent, (long long)ms.files_not_here, a);
    }
    if (ms.deleted_kept)
        wadd(t, L"%ls    %lld deleted from their folders, still kept on it\r\n", indent, (long long)ms.deleted_kept);
    if (ms.old_versions) {
        format_bytes(ms.old_bytes, a, 64);
        wadd(t, L"%ls    %lld older versions kept on it (%ls)\r\n", indent, (long long)ms.old_versions, a);
    }
    if (ms.last_backup_ms) {
        format_time_ms(ms.last_backup_ms, when, 64);
        wadd(t, L"%ls    Last backup %ls: %ls, %lld files copied", indent, when,
             job_end_words(ms.last_backup_status, ms.last_backup_end_ms), (long long)ms.last_backup_files);
        if (ms.last_backup_failed) wadd(t, L", %lld failed", (long long)ms.last_backup_failed);
        wadd(t, L"\r\n");
    } else
        wadd(t, L"%ls    Never backed up to\r\n", indent);
    if (ms.last_check_ms) {
        format_time_ms(ms.last_check_ms, when, 64);
        wchar_t *kind = widen(ms.last_check_kind);
        wadd(t, L"%ls    Last %ls %ls: %ls\r\n", indent, kind ? kind : L"check", when, job_end_words(ms.last_check_status, ms.last_check_ms));
        free(kind);
    } else
        wadd(t, L"%ls    Its copies were never checked (Check drive...)\r\n", indent);
    if (ms.copies_bad)
        wadd(t, L"%ls    %lld copies were missing or damaged when last checked: the next backup to it writes them again\r\n", indent,
             (long long)ms.copies_bad);
    if (ms.copies_never_read)
        wadd(t, L"%ls    %lld copies were never read back since they were written (Check drive..., Read back)\r\n", indent,
             (long long)ms.copies_never_read);
}

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
        if (strcmp(d->kind, "drive") == 0) {
            add_drive_status(&t, d->id, L"");
            wadd(&t, L"\r\n");
        }
        add_drive_lines(&t, d, L"");
        if (d->lost_ms)
            wadd(&t, L"\r\nIts copies still restore if it turns up, but they no longer count toward your full copies, so the "
                     L"files it held are backed up again to your other drives. If you find it, choose Found it.\r\n");
        SetDlgItemTextW(dlg, IDC_DRIVE_INFO, t.p ? t.p : L"");
        SetDlgItemTextW(dlg, IDC_DRIVE_LOST, d->lost_ms ? L"Found it" : L"Mark lost or broken...");
        EnableWindow(GetDlgItem(dlg, IDC_DRIVE_LOST), strcmp(d->kind, "drive") == 0);
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
        if (LOWORD(wp) == IDC_DRIVE_LOST) {
            GetDlgItemTextW(dlg, IDC_DRIVE_NAME, g_edit_name, 256);
            GetDlgItemTextW(dlg, IDC_DRIVE_LOCATION, g_edit_location, 256);
            EndDialog(dlg, IDC_DRIVE_LOST);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static void edit_drive(int64_t id);

static void cmd_drives(void)
{
    drive_list dl = load_drives();
    if (!dl.n) {
        MessageBoxW(g_main, L"No drives yet. Use Back up to set one up.", APP_NAME, MB_ICONINFORMATION);
        free(dl.items);
        return;
    }
    HMENU m = CreatePopupMenu();
    for (int i = 0; i < dl.n; i++) {
        const drive_snap *d = &dl.items[i];
        wchar_t *label = widen(d->label), *loc = widen(d->location), text[600];
        swprintf(text, 600, L"%ls%ls%ls%ls", label, *loc ? L"  (" : L"", loc, *loc ? L")" : L"");
        if (strcmp(d->kind, "drive") != 0) wcsncat(text, L"  - Cloud account from an earlier version (not supported in this version)", 599 - wcslen(text));
        else if (d->connected) wcsncat(text, L"  - plugged in", 599 - wcslen(text));
        if (d->lost_ms) wcsncat(text, strcmp(d->lost_why, "broken") == 0 ? L"  - marked broken" : L"  - marked lost", 599 - wcslen(text));
        else if (replace_advice(d)) wcsncat(text, L"  - its health data warns", 599 - wcslen(text));
        else if (d->check_due) wcsncat(text, L"  - due for a check", 599 - wcslen(text));
        AppendMenuW(m, MF_STRING, (UINT_PTR)(ID_MENU_DRIVE_BASE + i), text);
        free(label);
        free(loc);
    }
    RECT r;
    GetWindowRect(g_buttons[5], &r);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, r.left, r.bottom, 0, g_main, NULL);
    DestroyMenu(m);
    if (cmd >= ID_MENU_DRIVE_BASE && cmd - ID_MENU_DRIVE_BASE < dl.n) edit_drive(dl.items[cmd - ID_MENU_DRIVE_BASE].id);
    free(dl.items);
}

/* ---- Drives label and the full-copy bars ------------------------------- */

static void layout(void);

/* One bar per full copy the user wants: how much of the files as they are
 * now has a good copy in that set. A new or changed file lowers them until
 * it is backed up again. */
static void update_set_bars(void)
{
    bd_set_progress sets[MAX_SET_BARS];
    int n = 0;
    memset(sets, 0, sizeof(sets));
    if (g_cat && bd_backup_sets(g_cat, sets, MAX_SET_BARS, &n) != BD_OK) n = 0;
    int shown = n < MAX_SET_BARS ? n : MAX_SET_BARS;
    for (int i = 0; i < MAX_SET_BARS; i++) {
        if (i >= shown) {
            ShowWindow(g_set_bar[i], SW_HIDE);
            ShowWindow(g_set_text[i], SW_HIDE);
            continue;
        }
        const bd_set_progress *s = &sets[i];
        int pct = s->bytes_total > 0 ? (int)(s->bytes_done * 100 / s->bytes_total)
                : s->files_total > 0 ? (int)(s->files_done * 100 / s->files_total) : 100;
        if (pct > 100) pct = 100;
        if (pct == 100 && s->files_done < s->files_total) pct = 99; /* never show 100% while a file is missing */
        wchar_t done[64], total[64], text[256];
        format_bytes(s->bytes_done, done, 64);
        format_bytes(s->bytes_total, total, 64);
        if (s->files_total == 0)
            swprintf(text, 256, L"Full copy %d of %d: nothing to back up yet", s->set, n);
        else if (s->files_done >= s->files_total)
            swprintf(text, 256, L"Full copy %d of %d: complete, %lld files (%ls)", s->set, n, (long long)s->files_total, total);
        else
            swprintf(text, 256, L"Full copy %d of %d: %d%%, %lld of %lld files (%ls of %ls)", s->set, n, pct,
                     (long long)s->files_done, (long long)s->files_total, done, total);
        set_bar(g_set_bar[i], pct);
        SetWindowTextW(g_set_text[i], text);
        ShowWindow(g_set_bar[i], SW_SHOW);
        ShowWindow(g_set_text[i], SW_SHOW);
    }
    if (shown != g_set_count) {
        g_set_count = shown;
        layout();
    }
}

/* The drives that are plugged in and what a backup to each would do now:
 * kept up to date beside the Back up button, so it needs no question
 * before it starts. */
static void update_drives_label(void)
{
    if (g_busy) return; /* the catalog belongs to the running job */
    if (!g_cat) {
        SetWindowTextW(g_drives, L"No database is open. Use File > New database, Open database or Recover from a drive.");
        update_set_bars();
        return;
    }
    drive_list dl = load_drives();
    wtext t = {NULL, 0, 0};
    int total = 0, connected = 0;
    for (int i = 0; i < dl.n; i++) {
        const drive_snap *d = &dl.items[i];
        if (strcmp(d->kind, "drive") != 0) continue; /* a cloud account from an earlier version is not a drive to plug in */
        total++;
        if (!d->connected) continue;
        wchar_t *l = widen(d->label), *root = widen(d->root);
        wadd(&t, L"%ls%ls (%ls): ", connected ? L"   " : L"Plugged in: ", l, root);
        connected++;
        free(l);
        free(root);
        if (d->lost_ms) {
            wadd(&t, L"marked %ls.", strcmp(d->lost_why, "broken") == 0 ? L"broken" : L"lost");
            continue;
        }
        bd_backup_preview_info pv;
        if (bd_backup_preview(g_cat, d->id, 0, &pv) != BD_OK) {
            wadd(&t, L"cannot tell what a backup would copy.");
            continue;
        }
        wchar_t size[64], shortb[64];
        format_bytes(pv.bytes_to_copy, size, 64);
        if (pv.files_to_copy == 0) wadd(&t, L"up to date.");
        else wadd(&t, L"%lld file%ls (%ls) to back up", (long long)pv.files_to_copy, pv.files_to_copy == 1 ? L"" : L"s", size);
        if (pv.files_to_copy && pv.bytes_short > 0) {
            format_bytes(pv.bytes_short, shortb, 64);
            wadd(&t, L", about %ls will not fit", shortb);
        }
        if (pv.files_to_copy) wadd(&t, L".");
        if (pv.mass_change) wadd(&t, L" Many files changed at once: look before backing up.");
        if (d->check_due) wadd(&t, L" Due for a check.");
    }
    if (total == 0) {
        free(t.p);
        t.p = _wcsdup(L"No backup drives yet. Use Back up to set one up.");
    } else if (connected == 0) {
        wchar_t none[256];
        swprintf(none, 256, L"None of your %d backup drive%ls are plugged in. Plug one in to back up.", total, total == 1 ? L"" : L"s");
        free(t.p);
        t.p = _wcsdup(none);
    }
    SetWindowTextW(g_drives, t.p ? t.p : L"");
    free(t.p);
    free(dl.items);
    /* The button says how many files are short of the target. */
    bd_risk_stats rs;
    wchar_t risk[64] = L"At risk...";
    if (bd_list_at_risk(g_cat, 0, NULL, NULL, &rs) == BD_OK && rs.files_at_risk > 0)
        swprintf(risk, 64, L"At risk (%lld)...", (long long)rs.files_at_risk);
    SetWindowTextW(g_buttons[7], risk);
    update_set_bars();
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

    wchar_t text[1024], size[64];
    format_bytes(st.bytes_at_risk, size, 64);
    if (st.files_total == 0)
        swprintf(text, 1024, L"No files yet. Add a folder and scan it first.");
    else if (st.files_at_risk == 0)
        swprintf(text, 1024, L"All %lld files have at least %d copies in %d different places.", (long long)st.files_total, t.copies, t.places);
    else
        swprintf(text, 1024, L"%lld of %lld files (%ls) %ls fewer than %d copies in %d different places. %lld %ls no copy of %ls current version at all.",
                 (long long)st.files_at_risk, (long long)st.files_total, size, st.files_at_risk == 1 ? L"has" : L"have", t.copies, t.places,
                 (long long)st.files_no_copy, st.files_no_copy == 1 ? L"has" : L"have", st.files_no_copy == 1 ? L"its" : L"their");
    if (st.deleted_kept || st.old_versions_kept) {
        size_t len = wcslen(text);
        swprintf(text + len, 1024 - len, L" Also kept: %lld deleted files (%lld short of the target) and %lld older versions (%lld short).",
                 (long long)st.deleted_kept, (long long)st.deleted_short, (long long)st.old_versions_kept, (long long)st.old_versions_short);
    }
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
                swprintf(m, 400, L"Plug in %ls first, then try again.", g_risk_media_name[row]);
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
            log_append(L"The number of full copies to keep changed.");
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

/* ---- Lists in a table: drives, largest files, damaged files, conflicts --- */

/* IDD_TABLE in one of four modes: the status of every drive (double-click
 * or Details... opens one), the largest files, the files a bit-rot scan
 * found changed by themselves (Keep as it is now records one as a new
 * version), and the files the last restore left alone. */
enum { TABLE_DRIVES, TABLE_LARGEST, TABLE_DAMAGED, TABLE_CONFLICTS, TABLE_CHANGED };
static int64_t g_table_media; /* changed: the drive */
static void drive_name(const char *label, const char *location, wchar_t *out, size_t cap);
static int g_table_mode;
static int64_t g_table_ids[256];
static int64_t *g_table_nodes; /* damaged: node id by row */
static int g_table_n, g_table_pick;
/* The files the last restore left alone, from its job. */
static conflict_row *g_conflicts;
static int g_n_conflicts;

static void table_count(int64_t n, int64_t bytes, wchar_t *out, size_t cap)
{
    wchar_t b[64];
    if (!n) { wcscpy(out, L"0"); return; }
    format_bytes(bytes, b, 64);
    swprintf(out, cap, L"%lld (%ls)", (long long)n, b);
}

static void table_drives(HWND lv)
{
    static const wchar_t *cols[] = {L"Drive", L"Plugged in", L"As it is now", L"Changed since", L"Not on it", L"Last backup", L"Last check", L"Problems"};
    static const int w[] = {130, 60, 100, 100, 100, 150, 150, 160};
    lv_columns(lv, cols, w, 8);
    drive_list dl = load_drives();
    g_table_n = 0;
    for (int i = 0; i < dl.n && g_table_n < 256; i++) {
        const drive_snap *d = &dl.items[i];
        if (strcmp(d->kind, "drive") != 0) continue;
        bd_media_status ms;
        if (bd_media_status_get(g_cat, d->id, &ms) != BD_OK) continue;
        wchar_t name[600], cur[96], behind[96], notit[96], backup[128] = L"Never", check[128] = L"Never", probs[160] = L"";
        drive_name(d->label, d->location, name, 600);
        table_count(ms.files_current, ms.bytes_current, cur, 96);
        table_count(ms.files_behind, ms.bytes_behind, behind, 96);
        table_count(ms.files_not_here, ms.bytes_not_here, notit, 96);
        if (ms.last_backup_ms) {
            wchar_t when[64];
            format_time_ms(ms.last_backup_ms, when, 64);
            const char *st = ms.last_backup_status;
            swprintf(backup, 128, L"%ls%ls", when, !ms.last_backup_end_ms ? L", did not finish" : strcmp(st, "done") == 0 ? L""
                                                   : strcmp(st, "partial") == 0 ? L", with problems" : strcmp(st, "stopped") == 0 ? L", stopped"
                                                   : L", failed");
        }
        if (ms.last_check_ms) {
            wchar_t when[64], *kind = widen(ms.last_check_kind);
            format_time_ms(ms.last_check_ms, when, 64);
            swprintf(check, 128, L"%ls, %ls", when, kind ? kind : L"check");
            free(kind);
        }
        if (ms.copies_bad) swprintf(probs, 160, L"%lld missing or damaged", (long long)ms.copies_bad);
        if (ms.copies_never_read) {
            size_t len = wcslen(probs);
            swprintf(probs + len, 160 - len, L"%ls%lld never read back", len ? L", " : L"", (long long)ms.copies_never_read);
        }
        if (d->lost_ms) {
            size_t len = wcslen(probs);
            swprintf(probs + len, 160 - len, L"%lsmarked %ls", len ? L", " : L"", strcmp(d->lost_why, "broken") == 0 ? L"broken" : L"lost");
        } else if (d->check_due) {
            size_t len = wcslen(probs);
            swprintf(probs + len, 160 - len, L"%lsdue for a check", len ? L", " : L"");
        }
        const wchar_t *cells[] = {name, d->connected ? L"Yes" : L"No", cur, behind, notit, backup, check, probs};
        lv_row(lv, cells, 8);
        g_table_ids[g_table_n++] = d->id;
    }
    free(dl.items);
    SetDlgItemTextW(GetParent(lv), IDC_TABLE_TEXT,
                    g_table_n ? L"Where each drive stands, from the database (the drives need not be plugged in). \"Changed since\": only an "
                                L"older version of the file is on the drive. Double-click a drive for its details."
                              : L"No drives yet. Use Back up to set one up.");
}

typedef struct { HWND lv; int rows; } table_rows;

static int table_largest_cb(void *ctx, const bd_risk_info *r)
{
    table_rows *t = ctx;
    wchar_t *src = widen(r->source_name), *rel = widen(r->rel_path), size[64], copies[32], places[32];
    format_bytes(r->size, size, 64);
    swprintf(copies, 32, L"%d", r->copies);
    swprintf(places, 32, L"%d", r->places);
    const wchar_t *cells[] = {size, src ? src : L"", rel ? rel : L"", copies, places};
    lv_row(t->lv, cells, 5);
    t->rows++;
    free(src);
    free(rel);
    return 0;
}

static void table_largest(HWND lv)
{
    static const wchar_t *cols[] = {L"Size", L"Backup root", L"File", L"Good copies", L"Places"};
    static const int w[] = {80, 110, 360, 80, 60};
    lv_columns(lv, cols, w, 5);
    table_rows t = {lv, 0};
    HCURSOR old = SetCursor(LoadCursor(NULL, IDC_WAIT));
    bd_list_largest(g_cat, 200, table_largest_cb, &t);
    SetCursor(old);
    SetDlgItemTextW(GetParent(lv), IDC_TABLE_TEXT,
                    t.rows ? L"The largest files in your backup roots, largest first (the top 200): where the space on your drives goes."
                           : L"No files yet. Add a backup root and scan it.");
}

typedef struct { HWND lv; int rows, cap; } damaged_rows;

static int table_damaged_cb(void *ctx, const bd_damaged_info *d)
{
    damaged_rows *t = ctx;
    if (t->rows == t->cap) {
        int cap = t->cap ? t->cap * 2 : 64;
        int64_t *n = realloc(g_table_nodes, sizeof(int64_t) * (size_t)cap);
        if (!n) return 1;
        g_table_nodes = n;
        t->cap = cap;
    }
    wchar_t *src = widen(d->source_name), *rel = widen(d->rel_path), size[64];
    format_bytes(d->size, size, 64);
    const wchar_t *cells[] = {src ? src : L"", rel ? rel : L"", size};
    lv_row(t->lv, cells, 3);
    g_table_nodes[t->rows++] = d->node_id;
    free(src);
    free(rel);
    return 0;
}

static void table_damaged(HWND lv)
{
    static const wchar_t *cols[] = {L"Backup root", L"File", L"Size"};
    static const int w[] = {120, 440, 80};
    lv_columns(lv, cols, w, 3);
    damaged_rows t = {lv, 0, 0};
    free(g_table_nodes);
    g_table_nodes = NULL;
    bd_list_damaged(g_cat, table_damaged_cb, &t);
    g_table_n = t.rows;
    SetDlgItemTextW(GetParent(lv), IDC_TABLE_TEXT,
                    t.rows ? L"These files changed although their size and modified time did not: that is what a failing disk or bit rot "
                             L"looks like. Their copies on your drives are the good ones: restore a file to get it back. If you changed "
                             L"one on purpose, pick it and choose Keep as it is now."
                           : L"No damaged files. A bit-rot scan (Database > Bit-rot scan) reads every file again to look for them.");
}

static void table_conflicts(HWND lv)
{
    static const wchar_t *cols[] = {L"File", L"Was going to", L"Backup has", L"Already there"};
    static const int w[] = {260, 240, 150, 150};
    lv_columns(lv, cols, w, 4);
    for (int i = 0; i < g_n_conflicts; i++) {
        const conflict_row *c = &g_conflicts[i];
        wchar_t want[160], have[160], sz[64], when[64];
        format_bytes(c->want_size, sz, 64);
        format_ns(c->want_mtime_ns, when, 64);
        swprintf(want, 160, L"%ls, %ls", sz, when);
        if (c->have_size < 0) wcscpy(have, L"a folder");
        else {
            format_bytes(c->have_size, sz, 64);
            format_ns(c->have_mtime_ns, when, 64);
            swprintf(have, 160, L"%ls, %ls", sz, when);
        }
        const wchar_t *cells[] = {c->where ? c->where : L"", c->dest ? c->dest : L"", want, have};
        lv_row(lv, cells, 4);
    }
    g_table_n = g_n_conflicts;
    SetDlgItemTextW(GetParent(lv), IDC_TABLE_TEXT,
                    g_n_conflicts ? L"The last restore left these alone because another file was already where each would go: nothing "
                                    L"was overwritten. Compare them, move or rename the one that is there, and restore again."
                                  : L"The last restore left nothing alone.");
}

static int table_changed_cb(void *ctx, const bd_changed_info *c)
{
    table_rows *t = ctx;
    if (t->rows >= 5000) return 1;
    wchar_t *src = widen(c->source_name), *rel = widen(c->rel_path), was[96], now[96], when[64], a[64];
    format_bytes(c->size_on_drive, a, 64);
    swprintf(was, 96, L"v%lld, %ls", (long long)c->version_on_drive, a);
    format_bytes(c->size_now, a, 64);
    swprintf(now, 96, L"v%lld, %ls", (long long)c->version_now, a);
    format_time_ms(c->changed_ms, when, 64);
    wchar_t where[1200];
    swprintf(where, 1200, L"%ls\\%ls", src ? src : L"", rel ? rel : L"");
    const wchar_t *cells[] = {where, was, now, when};
    lv_row(t->lv, cells, 4);
    t->rows++;
    free(src);
    free(rel);
    return 0;
}

static void table_changed(HWND lv)
{
    static const wchar_t *cols[] = {L"File", L"On the drive", L"Now", L"Changed"};
    static const int w[] = {380, 110, 110, 120};
    lv_columns(lv, cols, w, 4);
    table_rows t = {lv, 0};
    bd_list_changed_on_media(g_cat, g_table_media, table_changed_cb, &t);
    SetDlgItemTextW(GetParent(lv), IDC_TABLE_TEXT,
                    L"These files changed since their copy on this drive was made (the first 5000). If you did not change them, "
                    L"do not trust them as they are: restore them from the drive.");
}

static INT_PTR CALLBACK table_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG: {
        static const wchar_t *titles[] = {L"Status of every drive", L"Largest files", L"Damaged files", L"Left alone by the last restore",
                                          L"Changed since the copy on the drive"};
        static const wchar_t *buttons[] = {L"Details...", L"", L"Keep as it is now", L"Open in Explorer", L""};
        SetWindowTextW(dlg, titles[g_table_mode]);
        SetDlgItemTextW(dlg, IDC_TABLE_OPEN, buttons[g_table_mode]);
        ShowWindow(GetDlgItem(dlg, IDC_TABLE_OPEN), *buttons[g_table_mode] ? SW_SHOW : SW_HIDE);
        HWND lv = GetDlgItem(dlg, IDC_TABLE_LIST);
        g_table_n = 0;
        if (g_table_mode == TABLE_DRIVES) table_drives(lv);
        else if (g_table_mode == TABLE_LARGEST) table_largest(lv);
        else if (g_table_mode == TABLE_DAMAGED) table_damaged(lv);
        else if (g_table_mode == TABLE_CHANGED) table_changed(lv);
        else table_conflicts(lv);
        EnableWindow(GetDlgItem(dlg, IDC_TABLE_OPEN), g_table_n > 0);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_TABLE_OPEN) {
            HWND lv = GetDlgItem(dlg, IDC_TABLE_LIST);
            int row = ListView_GetNextItem(lv, -1, LVNI_SELECTED);
            if (row < 0 || row >= g_table_n) {
                MessageBoxW(dlg, L"Pick a row in the list first.", APP_NAME, MB_ICONINFORMATION);
                return TRUE;
            }
            if (g_table_mode == TABLE_DAMAGED) {
                if (MessageBoxW(dlg, L"Keep this file as it is now? It is recorded as a new version and backed up like any "
                                     L"change; the older copies on your drives stay as they are.",
                                APP_NAME, MB_YESNO | MB_ICONQUESTION) != IDYES)
                    return TRUE;
                if (bd_damaged_accept(g_cat, g_table_nodes[row]) != BD_OK) {
                    wchar_t *e = widen(bd_catalog_error(g_cat));
                    MessageBoxW(dlg, e ? e : L"Could not record it.", APP_NAME, MB_ICONWARNING);
                    free(e);
                    return TRUE;
                }
                bd_catalog_save(g_cat);
                ListView_DeleteAllItems(lv);
                while (ListView_DeleteColumn(lv, 0)) {}
                table_damaged(lv);
                EnableWindow(GetDlgItem(dlg, IDC_TABLE_OPEN), g_table_n > 0);
                return TRUE;
            }
            if (g_table_mode == TABLE_CONFLICTS) {
                wchar_t args[4096];
                swprintf(args, 4096, L"/select,\"%ls\"", g_conflicts[row].dest ? g_conflicts[row].dest : L"");
                ShellExecuteW(dlg, NULL, L"explorer.exe", args, NULL, SW_SHOWNORMAL);
                return TRUE;
            }
            g_table_pick = row;
            EndDialog(dlg, IDC_TABLE_OPEN);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL || LOWORD(wp) == IDOK) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    case WM_NOTIFY: {
        NMHDR *nh = (NMHDR *)lp;
        if ((g_table_mode == TABLE_DRIVES || g_table_mode == TABLE_CONFLICTS) && nh->idFrom == IDC_TABLE_LIST && nh->code == NM_DBLCLK)
            SendMessageW(dlg, WM_COMMAND, IDC_TABLE_OPEN, 0);
        return TRUE;
    }
    }
    return FALSE;
}

static void cmd_table(int mode)
{
    g_table_mode = mode;
    for (;;) {
        INT_PTR r = DialogBoxW(g_inst, MAKEINTRESOURCEW(IDD_TABLE), g_main, table_proc);
        if (r != IDC_TABLE_OPEN || mode != TABLE_DRIVES) break;
        edit_drive(g_table_ids[g_table_pick]);
    }
    if (mode == TABLE_DAMAGED) rebuild_tree();
}

static void show_changed(int64_t media_id)
{
    g_table_media = media_id;
    cmd_table(TABLE_CHANGED);
}

/* Every file and where it is, as a CSV file for a spreadsheet. */
static void cmd_export(void)
{
    wchar_t path[MAX_PATH * 2] = L"BRODALF files.csv";
    OPENFILENAMEW of;
    memset(&of, 0, sizeof(of));
    of.lStructSize = sizeof(of);
    of.hwndOwner = g_main;
    of.lpstrFilter = L"CSV file (*.csv)\0*.csv\0All files\0*.*\0";
    of.lpstrFile = path;
    of.nMaxFile = MAX_PATH * 2;
    of.lpstrDefExt = L"csv";
    of.lpstrTitle = L"Export the file list";
    of.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&of)) return;
    char *u = narrow(path);
    int64_t rows = 0;
    HCURSOR old = SetCursor(LoadCursor(NULL, IDC_WAIT));
    bd_status s = u ? bd_export_csv(g_cat, u, &rows) : BD_ERR_NOMEM;
    SetCursor(old);
    free(u);
    if (s != BD_OK) {
        wchar_t *e = widen(bd_catalog_error(g_cat)), m[1200];
        swprintf(m, 1200, L"Could not export the file list: %ls", e && *e ? e : L"the file could not be written.");
        MessageBoxW(g_main, m, APP_NAME, MB_ICONWARNING);
        free(e);
        return;
    }
    wchar_t line[MAX_PATH * 2 + 128];
    swprintf(line, MAX_PATH * 2 + 128, L"Exported %lld files to %ls.", (long long)rows, path);
    log_append(line);
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
static int g_pass_mode; /* IDD_PASSPHRASE: 0 passphrase, 1 also offer the recovery key, 2 typing the recovery key */

static INT_PTR CALLBACK pass_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG:
        SetDlgItemTextW(dlg, IDC_PASS_TEXT, g_pass_text);
        if (GetDlgItem(dlg, IDC_PASS_RECOVERY) && g_pass_mode != 1) ShowWindow(GetDlgItem(dlg, IDC_PASS_RECOVERY), SW_HIDE);
        if (g_pass_mode == 2) {
            /* A recovery key is long and read off paper: show what is typed. */
            SetWindowTextW(dlg, L"Enter the recovery key");
            SendDlgItemMessageW(dlg, IDC_PASS_EDIT, EM_SETPASSWORDCHAR, 0, 0);
        }
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
        if (LOWORD(wp) == IDC_PASS_RECOVERY) { EndDialog(dlg, IDC_PASS_RECOVERY); return TRUE; }
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
    g_pass_mode = 0;
    char *p = rc == IDOK ? narrow(g_pass_buf) : NULL;
    SecureZeroMemory(g_pass_buf, sizeof(g_pass_buf));
    return p;
}

/* The passphrase, or with allow_recovery the recovery key if the user
 * chooses "Use the recovery key" (*is_recovery set). NULL if cancelled. */
static char *ask_unlock_secret(HWND owner, const wchar_t *text, int allow_recovery, int *is_recovery)
{
    *is_recovery = 0;
    g_pass_text = text;
    g_pass_catalog = 0;
    g_pass_mode = allow_recovery ? 1 : 0;
    INT_PTR rc = DialogBoxW(g_inst, MAKEINTRESOURCEW(IDD_PASSPHRASE), owner, pass_proc);
    if (rc == IDC_PASS_RECOVERY) {
        g_pass_text = L"Type the recovery key from your recovery sheet. Dashes and spaces do not matter.";
        g_pass_mode = 2;
        rc = DialogBoxW(g_inst, MAKEINTRESOURCEW(IDD_PASSPHRASE), owner, pass_proc);
        *is_recovery = 1;
    }
    g_pass_mode = 0;
    char *p = rc == IDOK ? narrow(g_pass_buf) : NULL;
    SecureZeroMemory(g_pass_buf, sizeof(g_pass_buf));
    return p;
}

static int set_passphrase_ui(void);
static void cmd_sheet(int with_key);

/* After unlocking with the recovery key the passphrase may be lost. */
static void offer_new_passphrase(HWND owner)
{
    if (MessageBoxW(owner, L"Unlocked with the recovery key. If you no longer know the passphrase, set a new one now. "
                           L"The recovery key keeps working.\n\nSet a new passphrase now?",
                    APP_NAME, MB_YESNO | MB_ICONQUESTION) == IDYES)
        set_passphrase_ui();
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
        int recovery = 0;
        char *p = ask_unlock_secret(g_main, why, bd_catalog_has_recovery_key(g_cat), &recovery);
        if (!p) return 0;
        SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_WAIT));
        bd_status s = recovery ? bd_catalog_unlock_recovery(g_cat, p) : bd_catalog_unlock(g_cat, p);
        free_secret(p);
        if (s == BD_OK) {
            if (recovery) offer_new_passphrase(g_main);
            return 1;
        }
        MessageBoxW(g_main, recovery ? L"That recovery key is not right. Check it against the sheet and try again."
                                     : L"That passphrase is not right. Try again.",
                    APP_NAME, MB_ICONWARNING);
    }
}

/* Set the first passphrase, or change it. 1 on success. */
static int set_passphrase_ui(void)
{
    int changing = bd_catalog_has_passphrase(g_cat);
    if (changing && !unlock_ui(L"Enter the current passphrase first.")) return 0;
    char *p = ask_passphrase(g_main, IDD_NEW_PASSPHRASE,
                             L"The passphrase protects encrypted drives and, if you choose, this database file. "
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
    if (!changing && g_main &&
        MessageBoxW(g_main, L"Print a recovery sheet now? It lists your drives and holds a recovery key, which opens your "
                            L"encrypted backups if you ever forget the passphrase.",
                    APP_NAME, MB_YESNO | MB_ICONQUESTION) == IDYES)
        cmd_sheet(1);
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
    start_backup(j, 1);
}

/* ---- Drives: checks, backups, what happens after ---------------------------- */

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

/* Check every copy on a drive: that it is there, its size and time, and
 * its checksum. */
static void enqueue_check(int64_t id)
{
    if (media_encryption(id).encrypted && !unlock_ui(L"This drive is encrypted. Enter the passphrase to check its copies.")) return;
    job *c = new_job(JOB_CHECK);
    if (!c) return;
    c->media_id = id;
    enqueue(c);
}

static void enqueue_verify(int64_t id)
{
    if (media_encryption(id).encrypted && !unlock_ui(L"This drive is encrypted. Enter the passphrase to read its copies.")) return;
    job *v = new_job(JOB_VERIFY);
    if (!v) return;
    v->media_id = id;
    enqueue(v);
}

/* The user picked a plugged-in drive to check: every copy, or only the
 * copies not read back lately. Either way every copy read is checked for
 * its size, time and checksum. */
static void check_drive(int64_t id)
{
    media_one o;
    memset(&o, 0, sizeof(o));
    o.id = id;
    bd_list_media(g_cat, media_one_cb, &o);
    if (!o.found) return;
    wchar_t name[600], content[1400], since[64], verify[400];
    drive_name(o.label, o.location, name, 600);
    if (o.info.oldest_check_ms > 0) {
        format_time_ms(o.info.oldest_check_ms, since, 64);
        swprintf(content, 1400, L"Some copies on %ls have not been read back since %ls.%ls Checking changes nothing on the drive.", name,
                 since, o.info.check_due ? L" That is longer than Settings asks for, so a check is due." : L"");
    } else
        swprintf(content, 1400, L"Checking reads the copies on %ls and compares each one with its checksum. It changes nothing on the drive.", name);
    int days = bd_option_get(g_cat, "verify_days");
    if (days > 0)
        swprintf(verify, 400, L"Read back copies not read lately\nReads the copies not read back in the last %d days, and new ones. "
                              L"A full check spread over time.", days);
    else
        swprintf(verify, 400, L"Read back every copy\nThe same as checking every copy (Settings has read-back set to every copy).");
    TASKDIALOG_BUTTON b[2] = {
        {611, L"Check every copy\nReads every copy and compares its size, time and checksum. Takes a while on a big drive."},
        {612, verify},
    };
    TASKDIALOGCONFIG tc;
    memset(&tc, 0, sizeof(tc));
    tc.cbSize = sizeof(tc);
    tc.hwndParent = g_main;
    tc.hInstance = g_inst;
    tc.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
    tc.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    tc.pszWindowTitle = APP_NAME;
    tc.pszMainInstruction = L"How should BRODALF check the drive?";
    tc.pszContent = content;
    tc.cButtons = 2;
    tc.pButtons = b;
    tc.nDefaultButton = 611;
    int pressed = IDCANCEL;
    if (FAILED(TaskDialogIndirect(&tc, &pressed, NULL, NULL))) return;
    if (pressed == 611) enqueue_check(id);
    else if (pressed == 612) enqueue_verify(id);
}

/* A restore that is waiting for more drives. */
static struct { int active; int64_t source_id; char *rel, *dest; } g_pending;

static void media_name(int64_t id, wchar_t *out, size_t cap)
{
    media_one o;
    memset(&o, 0, sizeof(o));
    o.id = id;
    bd_list_media(g_cat, media_one_cb, &o);
    drive_name(o.found ? o.label : "the drive", o.found ? o.location : "", out, cap);
}

/* Back up to a drive. It starts straight away: what it will copy is shown
 * all the time beside the Back up button. Only two things ask first: many
 * files changed at once (what ransomware looks like), and nothing scanned
 * yet. Takes b (a JOB_BACKUP with media_id, or root for a BRODALF drive
 * picked by folder). offer_scan 0 right after the scan the user asked for. */
static void start_backup(job *b, int offer_scan)
{
    if (!b) return;
    if (b->label) { enqueue(b); return; } /* a new drive: there is nothing on it yet */
    if (!b->media_id && b->root) {
        int64_t id = 0;
        if (bd_media_connect(g_cat, b->root, &id, NULL, NULL) != BD_OK || !id) {
            wchar_t *e = widen(bd_catalog_error(g_cat)), m[1200];
            swprintf(m, 1200, L"BRODALF could not use that drive: %ls", e && *e ? e : L"it is not a BRODALF drive for this database.");
            MessageBoxW(g_main, m, APP_NAME, MB_ICONWARNING);
            free(e);
            job_free(b);
            return;
        }
        b->media_id = id;
        update_drives_label();
    }
    wchar_t name[600];
    media_name(b->media_id, name, 600);
    media_one o;
    memset(&o, 0, sizeof(o));
    o.id = b->media_id;
    bd_list_media(g_cat, media_one_cb, &o);
    if (o.found && o.info.lost_ms) {
        wchar_t m[1000];
        swprintf(m, 1000, L"%ls is marked %ls, so nothing is backed up to it. If it is back and working, open Drives..., choose it "
                          L"and press Found it.", name, strcmp(o.info.lost_why, "broken") == 0 ? L"broken" : L"lost");
        MessageBoxW(g_main, m, APP_NAME, MB_ICONINFORMATION);
        job_free(b);
        return;
    }
    for (;;) {
        bd_backup_preview_info pv;
        memset(&pv, 0, sizeof(pv));
        if (bd_backup_preview(g_cat, b->media_id, 0, &pv) != BD_OK) { enqueue(b); return; }
        int unscanned = !pv.last_scan_ms && offer_scan;
        if (!pv.mass_change && !unscanned) { enqueue(b); return; }
        wtext t = {NULL, 0, 0};
        TASKDIALOG_BUTTON btn[2];
        wchar_t title[700];
        if (pv.mass_change) {
            swprintf(title, 700, L"Many files changed at once. Back up to %ls?", name);
            wadd(&t, L"%lld of the %lld files whose copies on this drive were up to date have changed since. That many at once can "
                     L"mean ransomware encrypted your files.\n\nBacking up overwrites nothing: the copies already on the drive stay as "
                     L"they are and the changed files go alongside them. If you did not change these files, look at them first, and "
                     L"restore from this drive instead.",
                 (long long)pv.held_changed, (long long)pv.files_held);
            btn[0].nButtonID = 622;
            btn[0].pszButtonText = L"Show the changed files";
            btn[1].nButtonID = 620;
            btn[1].pszButtonText = L"Back up anyway\nThe copies already on the drive are kept.";
        } else {
            swprintf(title, 700, L"Scan your backup roots first?");
            wadd(&t, L"Your backup roots have not been scanned yet, so BRODALF does not know of any files to put on %ls.", name);
            btn[0].nButtonID = 621;
            btn[0].pszButtonText = L"Scan first, then back up";
            btn[1].nButtonID = 620;
            btn[1].pszButtonText = L"Back up anyway\nOnly the database is saved to the drive.";
        }
        TASKDIALOGCONFIG tc;
        memset(&tc, 0, sizeof(tc));
        tc.cbSize = sizeof(tc);
        tc.hwndParent = g_main;
        tc.hInstance = g_inst;
        tc.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
        tc.dwCommonButtons = TDCBF_CANCEL_BUTTON;
        tc.pszWindowTitle = APP_NAME;
        tc.pszMainIcon = pv.mass_change ? TD_WARNING_ICON : NULL;
        tc.pszMainInstruction = title;
        tc.pszContent = t.p ? t.p : L"";
        tc.cButtons = 2;
        tc.pButtons = btn;
        tc.nDefaultButton = pv.mass_change ? 622 : 621;
        int pressed = IDCANCEL;
        if (FAILED(TaskDialogIndirect(&tc, &pressed, NULL, NULL))) pressed = IDCANCEL;
        free(t.p);
        if (pressed == 620) { enqueue(b); return; }
        if (pressed == 621) {
            job *s = new_job(JOB_SCAN);
            if (s) {
                s->then_backup = b->media_id;
                enqueue(s);
            }
            job_free(b);
            return;
        }
        if (pressed != 622) { job_free(b); return; }
        show_changed(b->media_id);
    }
}

/* What did not fit waits for another drive: every drive gets whatever it
 * is missing, so any drive with room carries the backup on. */
static void after_backup(job *j)
{
    if (j->no_room <= 0) return;
    wchar_t name[600], size[64], msg[1600];
    media_name(j->media_id, name, 600);
    format_bytes(j->bytes_no_room, size, 64);
    drive_list dl = load_drives();
    const drive_snap *best = NULL;
    for (int i = 0; i < dl.n; i++) {
        const drive_snap *d = &dl.items[i];
        if (!d->connected || d->lost_ms || d->id == j->media_id || strcmp(d->kind, "drive") != 0) continue;
        if (!best || d->freeb > best->freeb) best = d;
    }
    if (best) {
        wchar_t other[600];
        int64_t other_id = best->id;
        drive_name(best->label, best->location, other, 600);
        swprintf(msg, 1600, L"%ls is full: %lld file%ls (%ls) did not fit. They wait for another drive.\n\nBack up to %ls now?", name,
                 (long long)j->no_room, j->no_room == 1 ? L"" : L"s", size, other);
        free(dl.items);
        if (MessageBoxW(g_main, msg, APP_NAME, MB_ICONQUESTION | MB_YESNO) == IDYES &&
            (!media_encryption(other_id).encrypted || unlock_ui(L"This drive is encrypted. Enter the passphrase to back up to it."))) {
            job *b = new_job(JOB_BACKUP);
            if (b) {
                b->media_id = other_id;
                start_backup(b, 0);
            }
        }
        return;
    }
    free(dl.items);
    swprintf(msg, 1600,
             L"%ls is full: %lld file%ls (%ls) did not fit. They wait for another drive.\n\n"
             L"Plug in another backup drive (or set up a new one with Back up... > Another drive or folder) and back up to it. "
             L"Each drive gets whatever it is missing.",
             name, (long long)j->no_room, j->no_room == 1 ? L"" : L"s", size);
    MessageBoxW(g_main, msg, APP_NAME, MB_ICONINFORMATION);
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
    /* What the restore left alone replaces the last restore's list. */
    free_conflicts(g_conflicts, g_n_conflicts);
    g_conflicts = j->conf;
    g_n_conflicts = j->n_conf;
    j->conf = NULL;
    j->n_conf = j->cap_conf = 0;
    if (g_n_conflicts > 0) {
        wchar_t msg[600];
        swprintf(msg, 600, L"%d file%ls left alone because another file was already where %ls would go. Nothing was overwritten.\n\n"
                           L"Show them, to compare?", g_n_conflicts, g_n_conflicts == 1 ? L" was" : L"s were",
                 g_n_conflicts == 1 ? L"it" : L"each");
        if (MessageBoxW(g_main, msg, APP_NAME, MB_ICONINFORMATION | MB_YESNO) == IDYES) cmd_table(TABLE_CONFLICTS);
    }
    if (j->deleted || j->version_id) {
        /* Not kept waiting: the user picks them again. */
        if (j->offline > 0) {
            wchar_t line[300];
            swprintf(line, 300, L"%lld file%ls on drives that are not plugged in. Plug one in, then restore again.", (long long)j->offline,
                     j->offline == 1 ? L" is" : L"s are");
            log_append(line);
        }
        return;
    }
    if (j->n_picks) return;
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
        wadd(&msg, L"%lld file%ls on drives that are not plugged in. Plug these in, one at a time, and press Restore... "
                   L"after each to continue:\r\n%ls",
             (long long)j->offline, j->offline == 1 ? L" is" : L"s are", p.t.p);
        for (wchar_t *c = msg.p; c && *c; c++) if (*c == L'\n' && (c == msg.p || c[-1] != L'\r')) *c = L' ';
        log_append(msg.p);
        free(msg.p);
    }
    free(p.t.p);
}

/* The drives the waiting restore still needs. */
static int pending_needs(int64_t id)
{
    if (!g_pending.active) return 0;
    plan_text p;
    memset(&p, 0, sizeof(p));
    bd_restore_plan(g_cat, g_pending.source_id, g_pending.rel, g_pending.dest, plan_text_cb, &p, NULL, NULL);
    free(p.t.p);
    for (int i = 0; i < p.nid; i++)
        if (p.ids[i] == id) return 1;
    return 0;
}

/* The user chose to carry on with the restore that waits for drives. */
static void continue_restore(void)
{
    if (!g_pending.active) return;
    if (media_encryption(0).any_connected_encrypted && !bd_catalog_is_unlocked(g_cat) &&
        !unlock_ui(L"Some copies are on an encrypted drive. Enter the passphrase to restore them."))
        return;
    job *r = new_job(JOB_RESTORE);
    if (!r) return;
    r->source_id = g_pending.source_id;
    r->rel = xstrdup(g_pending.rel);
    r->dest = xstrdup(g_pending.dest);
    enqueue(r);
}

/* A line in the log for each drive just plugged in whose health data warns. */
static void advise_new_drives(const job *j)
{
    if (!j->n_new) return;
    drive_list dl = load_drives();
    for (int i = 0; i < j->n_new; i++) {
        const drive_snap *d = find_drive(&dl, j->new_ids[i]);
        const wchar_t *advice = d ? replace_advice(d) : NULL;
        if (!advice) continue;
        wchar_t name[600], line[900];
        drive_name(d->label, d->location, name, 600);
        swprintf(line, 900, L"%ls: %ls", name, advice);
        log_append(line);
    }
    free(dl.items);
}

/* Disks the user said never to ask about, by volume serial number. */
#define IGNORED_KEY REG_KEY L"\\IgnoredDisks"

static int disk_serial(const wchar_t *root, wchar_t *out, size_t cap)
{
    DWORD serial = 0;
    if (!GetVolumeInformationW(root, NULL, 0, &serial, NULL, NULL, NULL, 0)) return 0;
    swprintf(out, cap, L"%08lX", (unsigned long)serial);
    return 1;
}

static int disk_ignored(const wchar_t *serial)
{
    HKEY k;
    DWORD v = 0, size = sizeof(v);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, IGNORED_KEY, 0, KEY_READ, &k) != ERROR_SUCCESS) return 0;
    LONG r = RegQueryValueExW(k, serial, NULL, NULL, (BYTE *)&v, &size);
    RegCloseKey(k);
    return r == ERROR_SUCCESS && v;
}

static void disk_ignore(const wchar_t *serial)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, IGNORED_KEY, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) != ERROR_SUCCESS) return;
    DWORD v = 1;
    RegSetValueExW(k, serial, 0, REG_DWORD, (const BYTE *)&v, sizeof(v));
    RegCloseKey(k);
}

static void add_disk_at(char *root);

/* A disk that is not a BRODALF drive was plugged in: offer to set it up.
 * Nothing happens unless the user says so. */
static void offer_new_disk(const wchar_t *root)
{
    wchar_t serial[16] = L"", windir[MAX_PATH] = L"";
    if (GetWindowsDirectoryW(windir, MAX_PATH) && towupper(windir[0]) == towupper(root[0])) return; /* the system disk */
    if (disk_serial(root, serial, 16) && disk_ignored(serial)) return;
    wchar_t vname[MAX_PATH + 1] = L"", size[64] = L"", content[900];
    GetVolumeInformationW(root, vname, MAX_PATH + 1, NULL, NULL, NULL, NULL, 0);
    ULARGE_INTEGER total;
    if (GetDiskFreeSpaceExW(root, NULL, &total, NULL)) format_bytes((int64_t)total.QuadPart, size, 64);
    if (*vname) swprintf(content, 900, L"\"%ls\" (%ls) was plugged in. It is not one of your backup drives.", vname, root);
    else swprintf(content, 900, L"The disk at %ls was plugged in. It is not one of your backup drives.", root);
    if (*size) {
        size_t len = wcslen(content);
        swprintf(content + len, 900 - len, L" It holds %ls.", size);
    }
    TASKDIALOG_BUTTON b[3] = {
        {640, L"Set it up as a backup drive\nBRODALF makes its own folder on it and backs up to it. Nothing else on the disk is touched."},
        {641, L"Not now"},
        {642, L"Never ask about this disk"},
    };
    TASKDIALOGCONFIG tc;
    memset(&tc, 0, sizeof(tc));
    tc.cbSize = sizeof(tc);
    tc.hwndParent = g_main;
    tc.hInstance = g_inst;
    tc.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
    tc.pszWindowTitle = APP_NAME;
    tc.pszMainInstruction = L"Use this disk for backups?";
    tc.pszContent = content;
    tc.cButtons = *serial ? 3 : 2;
    tc.pButtons = b;
    tc.nDefaultButton = 641;
    int pressed = 641;
    if (FAILED(TaskDialogIndirect(&tc, &pressed, NULL, NULL))) return;
    if (pressed == 642) disk_ignore(serial);
    else if (pressed == 640) add_disk_at(narrow(root));
}

/* Drives offered a check this time BRODALF runs: offered once each. */
static int64_t g_check_offered[64];
static int g_n_check_offered;

static int check_offered(int64_t id)
{
    for (int i = 0; i < g_n_check_offered; i++)
        if (g_check_offered[i] == id) return 1;
    if (g_n_check_offered < 64) g_check_offered[g_n_check_offered++] = id;
    return 0;
}

/* Drives were found. BRODALF says so and may ask (a check that is due, a
 * new disk to set up): what to do with them is the user's choice. */
static void after_drives(job *j)
{
    int64_t due[16];
    int n_due = 0;
    for (int i = 0; i < j->n_new; i++) {
        media_one o;
        memset(&o, 0, sizeof(o));
        o.id = j->new_ids[i];
        bd_list_media(g_cat, media_one_cb, &o);
        if (!o.found) continue;
        wchar_t name[600], line[1400];
        drive_name(o.label, o.location, name, 600);
        swprintf(line, 1400, L"%ls is plugged in.", name);
        if (o.info.lost_ms)
            wcsncat(line, L" It is marked lost or broken, so its copies do not count: if it is back and working, open Drives..., "
                          L"choose it and press Found it.", 1399 - wcslen(line));
        else if (o.info.check_due && n_due < 16)
            due[n_due++] = o.id;
        if (g_pending.active && pending_needs(o.id))
            wcsncat(line, L" It holds files for the restore that is waiting: press Restore... to continue it.", 1399 - wcslen(line));
        log_append(line);
    }
    advise_new_drives(j);
    update_drives_label();
    for (int i = 0; i < n_due; i++) {
        if (check_offered(due[i])) continue;
        media_one o;
        memset(&o, 0, sizeof(o));
        o.id = due[i];
        bd_list_media(g_cat, media_one_cb, &o);
        wchar_t name[600], since[64], msg[1200];
        drive_name(o.label, o.location, name, 600);
        format_time_ms(o.info.oldest_check_ms, since, 64);
        swprintf(msg, 1200, L"%ls is due for a check: some of its copies have not been read back since %ls.\n\n"
                            L"Check every copy on it now? It changes nothing on the drive. (No: BRODALF does not ask again until "
                            L"it is opened again.)", name, since);
        if (MessageBoxW(g_main, msg, APP_NAME, MB_ICONQUESTION | MB_YESNO) == IDYES) enqueue_check(due[i]);
    }
    for (int i = 0; i < j->n_other; i++) offer_new_disk(j->other[i]);
}

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

/* The list being edited: 0 the one for every backup root, else one root's own. */
static int64_t g_skip_source;
static wchar_t g_skip_root_name[300];

static INT_PTR CALLBACK skip_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG: {
        char *list = g_skip_source ? bd_source_skip_get(g_cat, g_skip_source) : bd_skip_list_get(g_cat);
        wchar_t *w = skip_text_for_edit(list ? list : "");
        SetDlgItemTextW(dlg, IDC_SKIP_EDIT, w);
        free(w);
        free(list);
        if (g_skip_source) {
            wchar_t title[400], text[900];
            swprintf(title, 400, L"What to leave out in %ls", g_skip_root_name);
            SetWindowTextW(dlg, title);
            swprintf(text, 900, L"Scans of %ls leave out what matches these, as well as what the list for every backup root (Settings) "
                                L"leaves out. One per line: * matches anything, ? one character, a line ending in / matches folders. "
                                L"A file now left out shows among the deleted files, for you to keep or forget.", g_skip_root_name);
            SetDlgItemTextW(dlg, IDC_SKIP_TEXT, text);
            SetDlgItemTextW(dlg, IDC_SKIP_DEFAULT, L"Clear");
        } else {
            SetDlgItemTextW(dlg, IDC_SKIP_TEXT,
                            L"Scans of every backup root leave out files and folders that match these, one per line. * matches anything, "
                            L"? one character. End a line with / for folders only (and everything in them). A line with / in the middle, "
                            L"like Photos/Exports/, is a path inside a backup root. Lines starting with # are notes.");
        }
        SetFocus(GetDlgItem(dlg, IDC_SKIP_EDIT));
        SendDlgItemMessageW(dlg, IDC_SKIP_EDIT, EM_SETSEL, 0, 0);
        return FALSE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_SKIP_DEFAULT) {
            wchar_t *w = skip_text_for_edit(g_skip_source ? "" : bd_skip_list_default());
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
            bd_status s = !u ? BD_ERR_NOMEM : g_skip_source ? bd_source_skip_set(g_cat, g_skip_source, u) : bd_skip_list_set(g_cat, u);
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

/* Edit a skip list (source_id 0: the one for every root), then scan. */
static void cmd_skip(int64_t source_id, const wchar_t *root_name)
{
    g_skip_source = source_id;
    swprintf(g_skip_root_name, 300, L"%ls", root_name ? root_name : L"");
    if (DialogBoxW(g_inst, MAKEINTRESOURCEW(IDD_SKIP), g_main, skip_proc) == IDOK) {
        log_append(L"The skip list changed. Scanning your backup roots again.");
        enqueue(new_job(JOB_SCAN));
    }
}

static void settings_command(int cmd);

static void cmd_security(void)
{
    int has = bd_catalog_has_passphrase(g_cat), unlocked = bd_catalog_is_unlocked(g_cat);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, ID_MENU_SET_PASS, has ? L"Change the passphrase..." : L"Set a passphrase...");
    AppendMenuW(m, MF_STRING | (has ? 0 : MF_GRAYED) | (bd_catalog_file_encrypted(g_cat) ? MF_CHECKED : 0),
                ID_MENU_ENCRYPT_CATALOG, L"Encrypt the database file");
    AppendMenuW(m, MF_STRING | (has && unlocked ? 0 : MF_GRAYED), ID_MENU_LOCK, L"Forget the passphrase until it is needed");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    HMENU remind = CreatePopupMenu();
    int days = bd_option_get(g_cat, "check_days");
    AppendMenuW(remind, MF_STRING | (days == 90 ? MF_CHECKED : 0), ID_MENU_CHECK_90, L"Every 3 months");
    AppendMenuW(remind, MF_STRING | (days == 180 ? MF_CHECKED : 0), ID_MENU_CHECK_180, L"Every 6 months");
    AppendMenuW(remind, MF_STRING | (days == 365 ? MF_CHECKED : 0), ID_MENU_CHECK_365, L"Every year");
    AppendMenuW(remind, MF_STRING | (days == 0 ? MF_CHECKED : 0), ID_MENU_CHECK_NEVER, L"Never");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)remind, L"A plugged-in drive offers a check after");
    HMENU verify = CreatePopupMenu();
    int vd = bd_option_get(g_cat, "verify_days");
    AppendMenuW(verify, MF_STRING | (vd == 7 ? MF_CHECKED : 0), ID_M_VERIFY_7, L"Copies not read back in a week");
    AppendMenuW(verify, MF_STRING | (vd == 30 ? MF_CHECKED : 0), ID_M_VERIFY_30, L"Copies not read back in a month");
    AppendMenuW(verify, MF_STRING | (vd == 90 ? MF_CHECKED : 0), ID_M_VERIFY_90, L"Copies not read back in 3 months");
    AppendMenuW(verify, MF_STRING | (vd == 0 ? MF_CHECKED : 0), ID_M_VERIFY_NEVER, L"Every copy");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)verify, L"Check drive, Read back: which copies");
    HMENU reserve = CreatePopupMenu();
    int rmb = bd_option_get(g_cat, "reserve_mb");
    AppendMenuW(reserve, MF_STRING | (rmb == 256 ? MF_CHECKED : 0), ID_MENU_RESERVE_256, L"256 MB");
    AppendMenuW(reserve, MF_STRING | (rmb == 512 ? MF_CHECKED : 0), ID_MENU_RESERVE_512, L"512 MB");
    AppendMenuW(reserve, MF_STRING | (rmb == 1024 ? MF_CHECKED : 0), ID_MENU_RESERVE_1024, L"1 GB");
    AppendMenuW(reserve, MF_STRING | (rmb == 2048 ? MF_CHECKED : 0), ID_MENU_RESERVE_2048, L"2 GB");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)reserve, L"Space kept free on each drive for the database's copies");
    AppendMenuW(m, MF_STRING | (bd_option_get(g_cat, "low_priority") ? MF_CHECKED : 0), ID_MENU_LOW_PRIORITY,
                L"Let other programs come first while a job runs");
    AppendMenuW(m, MF_STRING, ID_MENU_SKIP, L"What to leave out in every backup root...");
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
        if (on && !unlock_ui(L"Enter the passphrase to encrypt the database file.")) return;
        bd_status s = bd_catalog_set_file_encrypted(g_cat, on);
        if (s == BD_OK) s = bd_catalog_save(g_cat);
        if (s != BD_OK) {
            wchar_t *e = widen(bd_catalog_error(g_cat));
            report_error(g_main, e && e[0] ? e : L"Could not change the database encryption.");
            free(e);
            return;
        }
        log_append(on ? L"The database file is now encrypted. BRODALF will ask for the passphrase when it opens."
                      : L"The database file is no longer encrypted.");
    } else if (cmd == ID_MENU_LOCK) {
        bd_catalog_lock_key(g_cat);
        log_append(L"Passphrase forgotten. BRODALF will ask for it again when it needs it.");
    } else if (cmd == ID_MENU_SKIP) {
        cmd_skip(0, NULL);
    } else if (cmd >= ID_M_VERIFY_7 && cmd <= ID_M_VERIFY_NEVER) {
        static const int choice[] = {7, 30, 90, 0};
        int d = choice[cmd - ID_M_VERIFY_7];
        if (bd_option_set(g_cat, "verify_days", d) == BD_OK && bd_catalog_save(g_cat) == BD_OK) {
            wchar_t line[200];
            if (d) swprintf(line, 200, L"Read back now reads the copies not read back in %d days, and new ones.", d);
            else wcscpy(line, L"Read back now reads every copy on the drive.");
            log_append(line);
        }
    } else if (cmd >= ID_MENU_CHECK_90 && cmd <= ID_MENU_CHECK_NEVER) {
        static const int choice[] = {90, 180, 365, 0};
        int d = choice[cmd - ID_MENU_CHECK_90];
        if (bd_option_set(g_cat, "check_days", d) == BD_OK && bd_catalog_save(g_cat) == BD_OK) {
            wchar_t line[200];
            if (d) swprintf(line, 200, L"A plugged-in drive whose copies have not been read back in %d days offers a check.", d);
            else wcscpy(line, L"Drives never offer a check by themselves.");
            log_append(line);
            update_drives_label();
        }
    } else if (cmd >= ID_MENU_RESERVE_256 && cmd <= ID_MENU_RESERVE_2048) {
        static const int choice[] = {256, 512, 1024, 2048};
        int mb = choice[cmd - ID_MENU_RESERVE_256];
        if (bd_option_set(g_cat, "reserve_mb", mb) == BD_OK && bd_catalog_save(g_cat) == BD_OK) {
            wchar_t line[200];
            swprintf(line, 200, L"Backups now keep %d MB free on each drive for the database's copies.", mb);
            log_append(line);
            update_drives_label();
        }
    } else if (cmd == ID_MENU_LOW_PRIORITY) {
        int on = !bd_option_get(g_cat, "low_priority");
        if (bd_option_set(g_cat, "low_priority", on) == BD_OK && bd_catalog_save(g_cat) == BD_OK)
            log_append(on ? L"Jobs now let other programs come first: they may take longer while the PC is busy."
                          : L"Jobs now run at normal speed, even while other programs are busy.");
    }
}

/* ---- Recovery sheet ---------------------------------------------------- */

/* Write the recovery sheet and open it for printing. with_key: 1 always
 * put a new recovery key on it, 0 ask (encrypted catalogs only). */
static void cmd_sheet(int with_key)
{
    char key[BD_RECOVERY_KEY_SIZE] = "";
    if (bd_catalog_has_passphrase(g_cat) && !with_key) {
        TASKDIALOG_BUTTON b[2] = {
            {600, L"Put a new recovery key on it\nIt opens your encrypted backups if the passphrase is lost. "
                  L"The key on any earlier sheet stops working."},
            {601, L"Leave the recovery key off"},
        };
        TASKDIALOGCONFIG tc;
        memset(&tc, 0, sizeof(tc));
        tc.cbSize = sizeof(tc);
        tc.hwndParent = g_main;
        tc.hInstance = g_inst;
        tc.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
        tc.dwCommonButtons = TDCBF_CANCEL_BUTTON;
        tc.pszWindowTitle = APP_NAME;
        tc.pszMainInstruction = L"Put a recovery key on the sheet?";
        tc.pszContent = L"Your backups are encrypted. The sheet lists your drives and what is on them. It never shows the passphrase.";
        tc.cButtons = 2;
        tc.pButtons = b;
        int pressed = IDCANCEL;
        if (FAILED(TaskDialogIndirect(&tc, &pressed, NULL, NULL)) || (pressed != 600 && pressed != 601)) return;
        with_key = pressed == 600;
    }
    if (with_key) {
        if (!unlock_ui(L"Enter the passphrase to make a recovery key.")) return;
        bd_status s = bd_catalog_make_recovery_key(g_cat, key);
        if (s == BD_OK) s = bd_catalog_save(g_cat);
        if (s != BD_OK) {
            SecureZeroMemory(key, sizeof(key));
            wchar_t *e = widen(bd_catalog_error(g_cat));
            report_error(g_main, e && e[0] ? e : L"Could not make a recovery key.");
            free(e);
            return;
        }
        /* The catalog backups on the drives plugged in now learn the key too;
         * the others at their next backup. */
        id_list l = {{0}, 0};
        bd_list_media(g_cat, connected_drive_cb, &l);
        for (int i = 0; i < l.n; i++)
            if (bd_catalog_copy_to_media(g_cat, l.ids[i]) != BD_OK) {
                wchar_t *e = widen(bd_catalog_error(g_cat));
                if (e) log_append(e);
                free(e);
            }
        log_append(L"Made a new recovery key. Drives that are not plugged in learn it at their next backup.");
    }

    wchar_t path[MAX_PATH * 2] = L"BRODALF recovery sheet.html";
    OPENFILENAMEW of;
    memset(&of, 0, sizeof(of));
    of.lStructSize = sizeof(of);
    of.hwndOwner = g_main;
    of.lpstrFilter = L"Web page (*.html)\0*.html\0";
    of.lpstrFile = path;
    of.nMaxFile = MAX_PATH * 2;
    of.lpstrDefExt = L"html";
    of.lpstrTitle = L"Save the recovery sheet";
    of.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    int saved = GetSaveFileNameW(&of);
    bd_status s = BD_OK;
    if (saved) {
        char *p8 = narrow(path);
        s = p8 ? bd_write_recovery_sheet(g_cat, p8, key[0] ? key : NULL) : BD_ERR_NOMEM;
        free(p8);
    }
    SecureZeroMemory(key, sizeof(key));
    if (!saved) {
        if (with_key)
            MessageBoxW(g_main, L"The new recovery key was not saved anywhere, so it cannot be used. Make the sheet again "
                                L"to get one.", APP_NAME, MB_ICONINFORMATION);
        return;
    }
    if (s != BD_OK) {
        wchar_t *e = widen(bd_catalog_error(g_cat));
        report_error(g_main, e && e[0] ? e : L"Could not write the recovery sheet.");
        free(e);
        return;
    }
    ShellExecuteW(g_main, L"open", path, NULL, NULL, SW_SHOWNORMAL);
    if (with_key)
        MessageBoxW(g_main, L"The sheet is open in your browser. Print it and keep it somewhere safe, away from the PC and "
                            L"the drives. Then delete the file: anyone who finds the recovery key can read your encrypted "
                            L"backups.", APP_NAME, MB_ICONINFORMATION);
}

/* ---- A protected folder that is somewhere else (a new PC) ---------------- */

static int is_folder(const wchar_t *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static int folder_is_empty(const wchar_t *dir)
{
    wchar_t pat[MAX_PATH * 4 + 4];
    size_t n = wcslen(dir);
    if (swprintf(pat, MAX_PATH * 4 + 4, n && dir[n - 1] == L'\\' ? L"%ls*" : L"%ls\\*", dir) < 0) return 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 1;
    int empty = 1;
    do {
        if (wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0) { empty = 0; break; }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return empty;
}

/* X:\Users\<name on the old PC>\rest is often %USERPROFILE%\rest here. */
static int guess_new_place(const wchar_t *old, wchar_t *out, size_t cap)
{
    if (wcslen(old) < 10 || old[1] != L':' || _wcsnicmp(old + 2, L"\\Users\\", 7) != 0) return 0;
    const wchar_t *rest = wcschr(old + 9, L'\\');
    wchar_t profile[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
    if (!n || n >= MAX_PATH) return 0;
    if (swprintf(out, cap, L"%ls%ls", profile, rest ? rest : L"") < 0) return 0;
    return _wcsicmp(out, old) != 0 && is_folder(out);
}

typedef struct { int64_t id; wchar_t *name, *path; int active, missing; } source_place;
typedef struct { source_place *items; int n, cap; } source_places;

static int source_place_cb(void *ctx, const bd_source_info *info)
{
    source_places *l = ctx;
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 16;
        source_place *g = realloc(l->items, (size_t)cap * sizeof(*g));
        if (!g) return 1;
        l->items = g;
        l->cap = cap;
    }
    source_place *p = &l->items[l->n];
    p->id = info->source_id;
    p->name = widen(info->name);
    p->path = widen(info->path);
    p->active = info->active;
    p->missing = info->missing;
    if (!p->name || !p->path) { free(p->name); free(p->path); return 1; }
    l->n++;
    return 0;
}

static void source_places_free(source_places *l)
{
    for (int i = 0; i < l->n; i++) { free(l->items[i].name); free(l->items[i].path); }
    free(l->items);
    memset(l, 0, sizeof(*l));
}

enum { PLACE_GUESS = 300, PLACE_PICK, PLACE_LATER };

/* Ask where a protected folder is now and point the catalog there. 1 if it
 * was changed. Nothing is scanned here; the caller saves and scans. */
static int relocate_source(const source_place *sp)
{
    wchar_t guess[MAX_PATH * 4], guess_btn[MAX_PATH * 4 + 96], head[512], body[MAX_PATH * 4 + 512];
    int have_guess = guess_new_place(sp->path, guess, MAX_PATH * 4);
    int here = is_folder(sp->path);
    swprintf(head, 512, L"Where is \"%ls\" on this PC?", sp->name);
    if (here)
        swprintf(body, MAX_PATH * 4 + 512, L"BRODALF backs up %ls. If you moved this backup root, choose where it is now. Its files "
                                            L"keep their copies and history.", sp->path);
    else
        swprintf(body, MAX_PATH * 4 + 512, L"BRODALF backed up %ls, which the last scan could not find. If it is somewhere else, "
                                            L"for example under another user name or on another drive letter, choose where it is "
                                            L"now. Its files keep their copies and history.", sp->path);
    for (;;) {
        TASKDIALOG_BUTTON b[3];
        int n = 0;
        if (have_guess) {
            swprintf(guess_btn, MAX_PATH * 4 + 96, L"Use %ls\nThe same folder in your user folder on this PC", guess);
            b[n].nButtonID = PLACE_GUESS;
            b[n++].pszButtonText = guess_btn;
        }
        b[n].nButtonID = PLACE_PICK;
        b[n++].pszButtonText = L"Choose where it is now...";
        b[n].nButtonID = PLACE_LATER;
        b[n++].pszButtonText = here ? L"Leave it where it is"
                                    : L"I cannot find it\nBRODALF offers to restore it from your backup drives instead.";
        TASKDIALOGCONFIG tc;
        memset(&tc, 0, sizeof(tc));
        tc.cbSize = sizeof(tc);
        tc.hwndParent = g_main;
        tc.hInstance = g_inst;
        tc.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
        tc.pszWindowTitle = APP_NAME;
        tc.pszMainInstruction = head;
        tc.pszContent = body;
        tc.cButtons = (UINT)n;
        tc.pButtons = b;
        int pressed = IDCANCEL;
        if (FAILED(TaskDialogIndirect(&tc, &pressed, NULL, NULL))) return 0;

        wchar_t *chosen = NULL;
        if (pressed == PLACE_GUESS) chosen = _wcsdup(guess);
        else if (pressed == PLACE_PICK) {
            char *f = pick_folder(g_main, L"Choose where this backup root is now.");
            if (!f) continue;
            chosen = widen(f);
            free(f);
        } else return 0;
        if (!chosen) return 0;

        if (folder_is_empty(chosen)) {
            wchar_t warn[MAX_PATH * 4 + 512];
            swprintf(warn, MAX_PATH * 4 + 512, L"%ls is empty. If BRODALF backs up this folder, every file of \"%ls\" will look "
                                                L"deleted.\n\nTo get the files back instead, choose No and restore the backup root "
                                                L"from your drives.\n\nUse this empty folder anyway?",
                     chosen, sp->name);
            if (MessageBoxW(g_main, warn, APP_NAME, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) { free(chosen); continue; }
        }
        char *p = narrow(chosen);
        bd_status s = p ? bd_source_relocate(g_cat, sp->id, p) : BD_ERR_NOMEM;
        free(p);
        if (s == BD_OK) {
            wchar_t line[MAX_PATH * 4 + 128];
            swprintf(line, MAX_PATH * 4 + 128, L"Backup root \"%ls\" is now at %ls", sp->name, chosen);
            log_append(line);
            free(chosen);
            return 1;
        }
        free(chosen);
        wchar_t *e = widen(bd_catalog_error(g_cat));
        MessageBoxW(g_main, e ? e : L"That folder cannot be used.", APP_NAME, MB_ICONWARNING);
        free(e);
    }
}

static void places_changed(void)
{
    if (bd_catalog_save(g_cat) != BD_OK) {
        wchar_t *e = widen(bd_catalog_error(g_cat));
        report_error(g_main, e ? e : L"The database could not be saved.");
        free(e);
    }
    rebuild_tree();
    show_detail();
}

/* After a scan that could not find some backup roots: ask where each is
 * now, and if it cannot be found, offer to restore it from the drives
 * (the restore says which drives to plug in). */
static void restore_selection(int whole, int64_t source_id);

static void ask_missing_roots(void)
{
    source_places l;
    memset(&l, 0, sizeof(l));
    bd_list_sources(g_cat, source_place_cb, &l);
    int changed = 0;
    for (int i = 0; i < l.n; i++) {
        const source_place *sp = &l.items[i];
        if (!sp->active || !sp->missing || is_folder(sp->path)) continue;
        if (relocate_source(sp)) { changed = 1; continue; }
        wchar_t q[MAX_PATH * 4 + 400];
        swprintf(q, MAX_PATH * 4 + 400, L"Restore \"%ls\" from your backup drives?\n\nBRODALF shows which drives to plug in, then puts the "
                                         L"files in a folder you choose. Nothing in that folder is overwritten.\n\nNo: nothing changes, "
                                         L"and its files are not marked deleted. The next scan asks again.", sp->name);
        if (MessageBoxW(g_main, q, APP_NAME, MB_ICONQUESTION | MB_YESNO) == IDYES) restore_selection(1, sp->id);
    }
    source_places_free(&l);
    if (changed) {
        places_changed();
        enqueue(new_job(JOB_SCAN));
    }
}

static void relocate_one(const source_place *sp)
{
    if (relocate_source(sp)) {
        places_changed();
        enqueue(new_job(JOB_SCAN));
    }
}

static void cmd_relocate_one(int64_t source_id)
{
    source_places l;
    memset(&l, 0, sizeof(l));
    bd_list_sources(g_cat, source_place_cb, &l);
    for (int i = 0; i < l.n; i++)
        if (l.items[i].id == source_id) relocate_one(&l.items[i]);
    source_places_free(&l);
}

static void cmd_relocate(void)
{
    node_ref *r = item_ref(TreeView_GetSelection(g_tree));
    source_places l;
    memset(&l, 0, sizeof(l));
    bd_list_sources(g_cat, source_place_cb, &l);
    const source_place *sp = NULL;
    for (int i = 0; i < l.n; i++)
        if ((r && l.items[i].id == r->source_id) || (!r && l.n == 1)) sp = &l.items[i];
    if (!sp)
        MessageBoxW(g_main, L"Select the backup root in the list first.", APP_NAME, MB_ICONINFORMATION);
    else
        relocate_one(sp);
    source_places_free(&l);
}

/* ---- Commands ----------------------------------------------------------- */

static void cmd_add_folder(void)
{
    char *folder = pick_folder(g_main, L"Choose a folder to back up. BRODALF will scan it and keep track of every file inside.");
    if (!folder) return;
    int64_t id;
    if (bd_source_add(g_cat, folder, &id) != BD_OK) {
        wchar_t *e = widen(bd_catalog_error(g_cat));
        MessageBoxW(g_main, e, APP_NAME, MB_ICONWARNING);
        free(e);
    } else {
        wchar_t *w = widen(folder), line[1024];
        swprintf(line, 1024, L"New backup root: %ls", w);
        log_append(line);
        free(w);
        rebuild_tree();
        enqueue(new_job(JOB_SCAN));
    }
    free(folder);
}

typedef struct { HMENU menu; int count, greyed_lost; } menu_ctx;

static int drive_menu_cb(void *ctx, const bd_media_info *m)
{
    menu_ctx *c = ctx;
    if (!m->connected || strcmp(m->kind, "drive") != 0) return 0; /* a cloud account from an earlier version is never listed */
    wchar_t *l = widen(m->label), *root = widen(m->last_root), text[512], freeb[64];
    format_bytes(m->free_bytes, freeb, 64);
    swprintf(text, 512, L"%ls (%ls), %ls free%ls%ls", l, root, freeb, m->encrypted ? L", encrypted" : L"",
             m->lost_ms ? L" - marked lost or broken" : L"");
    /* A drive marked lost or broken takes no backups: shown greyed. */
    AppendMenuW(c->menu, MF_STRING | (m->lost_ms && c->greyed_lost ? MF_GRAYED : 0), (UINT_PTR)(ID_MENU_DRIVE_BASE + m->media_id), text);
    c->count++;
    free(l);
    free(root);
    return 0;
}

/* Returns a media id, 0 for "another drive or folder", -1 if cancelled. */
static int64_t choose_drive(HWND button, int allow_other)
{
    menu_ctx c = {CreatePopupMenu(), 0, allow_other};
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
    start_backup(j, 1);
}

static void add_disk(void)
{
    char *root = pick_folder(g_main, L"Choose the drive (or a folder on it) to back up to.");
    if (root) add_disk_at(root);
}

/* Set up a disk (or a folder on one) as backup storage and back up to it.
 * Takes root. */
static void add_disk_at(char *root)
{
    if (!root) return;
    job *j = new_job(JOB_BACKUP);
    if (!j) { free(root); return; }
    {
        j->root = root;
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
            enqueue(j);
            return;
        }
    }
    /* A drive BRODALF already knows, picked by folder. */
    start_backup(j, 1);
}

static void cmd_check(void)
{
    int64_t id = choose_drive(g_buttons[3], 0);
    if (id > 0) check_drive(id);
}

/* How many backup roots the database has. */
static int count_roots_cb(void *ctx, const bd_source_info *s)
{
    (void)s;
    (*(int *)ctx)++;
    return 0;
}

static int root_count(void)
{
    int n = 0;
    bd_list_sources(g_cat, count_roots_cb, &n);
    return n;
}

/* Restore what is selected in the tree (or everything, with whole set, or
 * one root with whole and source_id), as it is now. */
static void restore_selection(int whole, int64_t source_id)
{
    node_ref all = {0}, *r = whole ? &all : item_ref(TreeView_GetSelection(g_tree));
    all.source_id = source_id;
    if (!r) {
        MessageBoxW(g_main, L"Select a backup root, a folder inside it, or a file to restore.", APP_NAME, MB_ICONINFORMATION);
        return;
    }
    if (media_encryption(0).any_connected_encrypted && !unlock_ui(L"Some copies are on an encrypted drive. Enter the passphrase to restore them."))
        return;
    /* With one root its files go straight into the folder chosen; with
     * several, each root gets a folder of its own there. */
    int flat = r->source_id || root_count() <= 1;
    char *dest = pick_folder(g_main, flat ? L"Choose where to put the restored files. Folders inside keep their structure; nothing there is overwritten."
                                          : L"Choose where to put the restored files. Each backup root gets a folder named after it; nothing there is overwritten.");
    if (!dest) return;

    /* Which drives this needs, in order. */
    plan_text p;
    memset(&p, 0, sizeof(p));
    int64_t total = 0, none = 0;
    bd_restore_plan(g_cat, r->source_id, r->rel, dest, plan_text_cb, &p, &total, &none);
    if (total == 0) {
        free(p.t.p);
        free(dest);
        MessageBoxW(g_main, L"Everything here is already restored in that folder.", APP_NAME, MB_ICONINFORMATION);
        return;
    }
    if (p.n > 1 || p.any_offline || none) {
        wtext msg = {NULL, 0, 0};
        wadd(&msg, L"Restoring %lld file%ls needs these drives:\n\n%ls", (long long)total, total == 1 ? L"" : L"s",
             p.t.p ? p.t.p : L"    (none)\n");
        if (none) wadd(&msg, L"\n%lld file%ls no copy on any drive.\n", (long long)none, none == 1 ? L" has" : L"s have");
        if (p.any_offline)
            wadd(&msg, L"\nBRODALF restores what the plugged-in drives hold now. Then plug in each other drive and press "
                       L"Restore... again to continue with its files.\n");
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
    clear_pending();
    enqueue(j);
}

static void cmd_restore(void)
{
    if (g_pending.active) {
        /* A restore waits for drives: carry on with it, or start another. */
        wchar_t content[1200], *dest = widen(g_pending.dest);
        plan_text p;
        memset(&p, 0, sizeof(p));
        bd_restore_plan(g_cat, g_pending.source_id, g_pending.rel, g_pending.dest, plan_text_cb, &p, NULL, NULL);
        swprintf(content, 1200, L"A restore into %ls is waiting for these drives:\n\n%ls", dest ? dest : L"",
                 p.t.p ? p.t.p : L"    (none: it can finish now)\n");
        free(dest);
        free(p.t.p);
        TASKDIALOG_BUTTON b[2] = {
            {630, L"Continue the restore\nRestores what the drives plugged in now hold."},
            {631, L"Restore something else\nThe waiting restore is forgotten."},
        };
        TASKDIALOGCONFIG tc;
        memset(&tc, 0, sizeof(tc));
        tc.cbSize = sizeof(tc);
        tc.hwndParent = g_main;
        tc.hInstance = g_inst;
        tc.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
        tc.dwCommonButtons = TDCBF_CANCEL_BUTTON;
        tc.pszWindowTitle = APP_NAME;
        tc.pszMainInstruction = L"Continue the restore that is waiting?";
        tc.pszContent = content;
        tc.cButtons = 2;
        tc.pButtons = b;
        int pressed = IDCANCEL;
        if (FAILED(TaskDialogIndirect(&tc, &pressed, NULL, NULL))) return;
        if (pressed == 630) { continue_restore(); return; }
        if (pressed != 631) return;
    }
    restore_selection(0, 0);
}

/* ---- Deleted files -------------------------------------------------------- */

/* Files gone from their backup roots stay in the database until the user
 * says: keep tracking them (they stay listed, with the drives that hold
 * them), forget them, or restore them. */

#define DELETED_ROWS 5000

/* The list's rows, by row; and what the user chose to restore. */
typedef struct { int64_t source_id, node_id; char *rel, *root; } del_row;
static del_row *g_del, *g_del_pick;
static int g_del_n, g_del_npick;

static void del_rows_free(del_row *r, int n)
{
    for (int i = 0; i < n; i++) {
        free(r[i].rel);
        free(r[i].root);
    }
    free(r);
}

static int deleted_cb(void *ctx, const bd_deleted_info *d)
{
    HWND lv = ctx;
    if (g_del_n >= DELETED_ROWS) return 1;
    char *rel = xstrdup(d->rel_path);
    if (!rel) return 1;
    g_del[g_del_n].source_id = d->source_id;
    g_del[g_del_n].node_id = d->node_id;
    g_del[g_del_n].rel = rel;
    g_del[g_del_n].root = xstrdup(d->source_name);
    g_del_n++;
    wchar_t *src = widen(d->source_name), *wrel = widen(d->rel_path), *drv = widen(d->drives);
    wchar_t file[1200], when[64], size[64], on[600];
    swprintf(file, 1200, L"%ls\\%ls", src ? src : L"", wrel ? wrel : L"");
    for (wchar_t *c = file; *c; c++) if (*c == L'/') *c = L'\\';
    format_time_ms(d->last_seen_ms, when, 64);
    format_bytes(d->size, size, 64);
    if (d->backed_up) swprintf(on, 600, L"%ls%ls", drv ? drv : L"", d->connected ? L"  (plugged in)" : L"");
    else wcscpy(on, L"Never backed up");
    const wchar_t *cells[] = {d->acknowledged ? L"" : L"New", file, when, size, on};
    lv_row(lv, cells, 5);
    free(src);
    free(wrel);
    free(drv);
    return 0;
}

/* The chosen rows, or the folders they were in, without repeats or picks
 * already inside another pick. */
static int deleted_pick(HWND lv, int folders)
{
    int sel = ListView_GetSelectedCount(lv);
    if (sel <= 0) return 0;
    g_del_pick = calloc((size_t)sel, sizeof(del_row));
    if (!g_del_pick) return 0;
    g_del_npick = 0;
    for (int row = ListView_GetNextItem(lv, -1, LVNI_SELECTED); row >= 0 && g_del_npick < sel;
         row = ListView_GetNextItem(lv, row, LVNI_SELECTED)) {
        if (row >= g_del_n) continue;
        char *rel = xstrdup(g_del[row].rel);
        if (!rel) continue;
        if (folders) {
            char *slash = strrchr(rel, '/');
            if (slash) *slash = '\0'; else rel[0] = '\0'; /* "": the whole backup root */
        }
        g_del_pick[g_del_npick].source_id = g_del[row].source_id;
        g_del_pick[g_del_npick].node_id = g_del[row].node_id;
        g_del_pick[g_del_npick].rel = rel;
        g_del_pick[g_del_npick].root = xstrdup(g_del[row].root);
        g_del_npick++;
    }
    if (folders) {
        int n = 0;
        for (int i = 0; i < g_del_npick; i++) {
            del_row *a = &g_del_pick[i];
            int covered = 0;
            for (int k = 0; k < g_del_npick && !covered; k++) {
                del_row *b = &g_del_pick[k];
                if (k == i || !b->rel || a->source_id != b->source_id) continue;
                size_t bl = strlen(b->rel);
                if (strcmp(a->rel, b->rel) == 0) covered = k < i; /* keep the first of the same */
                else covered = !bl || (strncmp(a->rel, b->rel, bl) == 0 && a->rel[bl] == '/');
            }
            if (covered) {
                free(a->rel);
                free(a->root);
                a->rel = a->root = NULL;
            }
        }
        for (int i = 0; i < g_del_npick; i++)
            if (g_del_pick[i].rel) g_del_pick[n++] = g_del_pick[i];
        g_del_npick = n;
    }
    return g_del_npick;
}

static void deleted_fill(HWND dlg)
{
    HWND lv = GetDlgItem(dlg, IDC_DEL_LIST);
    ListView_DeleteAllItems(lv);
    del_rows_free(g_del, g_del_n);
    g_del_n = 0;
    int64_t total = 0, fresh = 0;
    bd_list_deleted(g_cat, 0, 0, NULL, NULL, &total);
    bd_list_deleted(g_cat, 0, 1, NULL, NULL, &fresh);
    g_del = calloc(DELETED_ROWS, sizeof(del_row));
    SendMessageW(lv, WM_SETREDRAW, FALSE, 0);
    if (g_del) bd_list_deleted(g_cat, 0, 0, deleted_cb, lv, NULL);
    SendMessageW(lv, WM_SETREDRAW, TRUE, 0);
    wchar_t text[700];
    if (total == 0)
        swprintf(text, 700, L"No files have been deleted from your backup roots. When one is, it shows here with the drives that "
                            L"still hold it, until you choose what to do with it.");
    else
        swprintf(text, 700, L"%lld file%ls deleted from your backup roots, most recently deleted first%ls; %lld %ls new. Select files to "
                            L"restore them, keep tracking them (they stay in the database and on your drives), or forget them "
                            L"(BRODALF stops tracking them; nothing on the drives is deleted).",
                 (long long)total, total == 1 ? L"" : L"s", total > g_del_n ? L" (only the first ones are listed)" : L"",
                 (long long)fresh, fresh == 1 ? L"is" : L"are");
    SetDlgItemTextW(dlg, IDC_DEL_SUMMARY, text);
    EnableWindow(GetDlgItem(dlg, IDC_DEL_FILES), g_del_n > 0);
    EnableWindow(GetDlgItem(dlg, IDC_DEL_FOLDERS), g_del_n > 0);
    EnableWindow(GetDlgItem(dlg, IDC_DEL_ACK), fresh > 0);
    EnableWindow(GetDlgItem(dlg, IDC_DEL_FORGET), g_del_n > 0);
}

static INT_PTR CALLBACK deleted_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG: {
        static const wchar_t *cols[] = {L"", L"File", L"Last seen", L"Size", L"On drives"};
        static const int widths[] = {40, 300, 110, 70, 180};
        lv_columns(GetDlgItem(dlg, IDC_DEL_LIST), cols, widths, 5);
        deleted_fill(dlg);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_DEL_FILES || LOWORD(wp) == IDC_DEL_FOLDERS) {
            if (!deleted_pick(GetDlgItem(dlg, IDC_DEL_LIST), LOWORD(wp) == IDC_DEL_FOLDERS)) {
                del_rows_free(g_del_pick, g_del_npick);
                g_del_pick = NULL;
                g_del_npick = 0;
                MessageBoxW(dlg, L"Select one or more files in the list first.", APP_NAME, MB_ICONINFORMATION);
                return TRUE;
            }
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDC_DEL_ACK) {
            /* The selected files, or every new one when none is selected. */
            HWND lv = GetDlgItem(dlg, IDC_DEL_LIST);
            bd_status s = BD_OK;
            if (ListView_GetSelectedCount(lv) <= 0) s = bd_deleted_ack(g_cat, 0);
            for (int row = ListView_GetNextItem(lv, -1, LVNI_SELECTED); row >= 0 && s == BD_OK;
                 row = ListView_GetNextItem(lv, row, LVNI_SELECTED))
                if (row < g_del_n) s = bd_deleted_ack(g_cat, g_del[row].node_id);
            if (s == BD_OK) s = bd_catalog_save(g_cat);
            if (s != BD_OK) {
                wchar_t *e = widen(bd_catalog_error(g_cat));
                MessageBoxW(dlg, e ? e : L"Could not record that.", APP_NAME, MB_ICONWARNING);
                free(e);
            }
            deleted_fill(dlg);
            return TRUE;
        }
        if (LOWORD(wp) == IDC_DEL_FORGET) {
            HWND lv = GetDlgItem(dlg, IDC_DEL_LIST);
            int sel = ListView_GetSelectedCount(lv);
            if (sel <= 0) {
                MessageBoxW(dlg, L"Select the files to forget first.", APP_NAME, MB_ICONINFORMATION);
                return TRUE;
            }
            wchar_t q[700];
            swprintf(q, 700, L"Forget %d deleted file%ls?\n\nBRODALF stops tracking %ls and %ls copies, so it can no longer restore "
                             L"%ls. The copies stay on your drives: BRODALF never deletes anything there. This cannot be undone.",
                     sel, sel == 1 ? L"" : L"s", sel == 1 ? L"it" : L"them", sel == 1 ? L"its" : L"their", sel == 1 ? L"it" : L"them");
            if (MessageBoxW(dlg, q, APP_NAME, MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES) return TRUE;
            bd_status s = BD_OK;
            int64_t gone = 0;
            for (int row = ListView_GetNextItem(lv, -1, LVNI_SELECTED); row >= 0 && s == BD_OK;
                 row = ListView_GetNextItem(lv, row, LVNI_SELECTED)) {
                int64_t one = 0;
                if (row < g_del_n) s = bd_deleted_forget(g_cat, g_del[row].node_id, &one);
                gone += one;
            }
            if (s == BD_OK) s = bd_catalog_save(g_cat);
            if (s != BD_OK) {
                wchar_t *e = widen(bd_catalog_error(g_cat));
                MessageBoxW(dlg, e ? e : L"Could not forget them.", APP_NAME, MB_ICONWARNING);
                free(e);
            } else {
                wchar_t line[200];
                swprintf(line, 200, L"Forgot %lld deleted file%ls. Their copies stay on the drives.", (long long)gone, gone == 1 ? L"" : L"s");
                log_append(line);
            }
            deleted_fill(dlg);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL || LOWORD(wp) == IDOK) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    case WM_NOTIFY: {
        NMHDR *nh = (NMHDR *)lp;
        if (nh->idFrom == IDC_DEL_LIST && nh->code == NM_DBLCLK) SendMessageW(dlg, WM_COMMAND, IDC_DEL_FILES, 0);
        return TRUE;
    }
    }
    return FALSE;
}

/* Sums the plan of several restores by drive. */
typedef struct { int64_t id[64], files[64], bytes[64]; int connected[64]; wchar_t name[64][300]; int n, other; } del_plan;

static int del_plan_cb(void *ctx, const bd_restore_step *st)
{
    del_plan *p = ctx;
    int i = 0;
    while (i < p->n && p->id[i] != st->media_id) i++;
    if (i == p->n) {
        if (p->n == 64) { p->other = 1; return 0; }
        p->n++;
        p->id[i] = st->media_id;
        p->files[i] = p->bytes[i] = 0;
        p->connected[i] = st->connected;
        drive_name(st->label, st->location, p->name[i], 300);
    }
    p->files[i] += st->files;
    p->bytes[i] += st->bytes;
    return 0;
}

/* dest\<root name> when picks come from several roots, as a whole restore does. */
static char *pick_dest(const char *dest, const char *root, int several)
{
    if (!several || !root) return xstrdup(dest);
    size_t n = strlen(dest) + strlen(root) + 3;
    char *p = malloc(n);
    if (p) snprintf(p, n, "%s%s%s", dest, dest[0] && dest[strlen(dest) - 1] == '\\' ? "" : "\\", root);
    return p;
}

static void cmd_deleted(void)
{
    g_del_pick = NULL;
    g_del_npick = 0;
    INT_PTR r = DialogBoxW(g_inst, MAKEINTRESOURCEW(IDD_DELETED), g_main, deleted_proc);
    del_rows_free(g_del, g_del_n);
    g_del = NULL;
    g_del_n = 0;
    rebuild_tree();
    del_row *picks = g_del_pick;
    int n = g_del_npick;
    g_del_pick = NULL;
    g_del_npick = 0;
    if (r != IDOK || n <= 0) { del_rows_free(picks, n); return; }

    char *dest = NULL;
    del_plan *p = calloc(1, sizeof(del_plan));
    if (!p) goto out;
    if (media_encryption(0).any_connected_encrypted && !unlock_ui(L"Some copies are on an encrypted drive. Enter the passphrase to restore them."))
        goto out;
    int several = 0;
    for (int i = 1; i < n; i++) if (picks[i].source_id != picks[0].source_id) several = 1;
    dest = pick_folder(g_main, several ? L"Choose where to put the restored files. Each backup root gets a folder named after it; nothing there is overwritten."
                                       : L"Choose where to put the restored files. Folders inside keep their structure; nothing there is overwritten.");
    if (!dest) goto out;

    int64_t total = 0;
    bd_restore_opts ro;
    memset(&ro, 0, sizeof(ro));
    ro.deleted_only = 1;
    for (int i = 0; i < n; i++) {
        int64_t t = 0;
        char *d = pick_dest(dest, picks[i].root, several);
        if (d) bd_restore_plan_ex(g_cat, picks[i].source_id, picks[i].rel, d, &ro, del_plan_cb, p, &t, NULL);
        free(d);
        total += t;
    }
    if (total == 0) {
        MessageBoxW(g_main, L"Everything selected is already restored in that folder, or has no copy on any drive.", APP_NAME, MB_ICONINFORMATION);
        goto out;
    }
    int offline = p->other;
    for (int i = 0; i < p->n; i++) if (!p->connected[i]) offline = 1;
    if (offline || p->n > 1) {
        wtext msg = {NULL, 0, 0};
        wadd(&msg, L"Restoring %lld deleted file%ls needs these drives:\n\n", (long long)total, total == 1 ? L"" : L"s");
        for (int i = 0; i < p->n; i++) {
            wchar_t size[64];
            format_bytes(p->bytes[i], size, 64);
            wadd(&msg, L"    %d. %ls: %lld file%ls (%ls)%ls\n", i + 1, p->name[i], (long long)p->files[i],
                 p->files[i] == 1 ? L"" : L"s", size, p->connected[i] ? L", plugged in" : L"");
        }
        if (offline)
            wadd(&msg, L"\nBRODALF restores what the plugged-in drives hold now. For the rest, plug in another drive "
                       L"and restore them again from Deleted files.\n");
        int go = MessageBoxW(g_main, msg.p, APP_NAME, MB_ICONINFORMATION | MB_OKCANCEL) == IDOK;
        free(msg.p);
        if (!go) goto out;
    }
    job *j = new_job(JOB_RESTORE);
    if (!j) goto out;
    j->pick_src = calloc((size_t)n, sizeof(int64_t));
    j->pick_rel = calloc((size_t)n, sizeof(char *));
    j->pick_dest = calloc((size_t)n, sizeof(char *));
    if (!j->pick_src || !j->pick_rel || !j->pick_dest) { job_free(j); goto out; }
    for (int i = 0; i < n; i++) {
        j->pick_src[i] = picks[i].source_id;
        j->pick_rel[i] = picks[i].rel;
        j->pick_dest[i] = pick_dest(dest, picks[i].root, several);
        picks[i].rel = NULL;
        j->n_picks = i + 1;
    }
    j->deleted = 1;
    j->dest = dest;
    dest = NULL;
    log_append(L"Restoring deleted files.");
    enqueue(j);
out:
    free(p);
    free(dest);
    del_rows_free(picks, n);
}

/* Restore one version of a file (from the versions list) into a folder the
 * user picks, under the file's own name. */
static void restore_version(int64_t version_id)
{
    if (media_encryption(0).any_connected_encrypted && !unlock_ui(L"Some copies are on an encrypted drive. Enter the passphrase to restore them."))
        return;
    char *dest = pick_folder(g_main, L"Choose the folder to put this version in. It keeps the file's own name; nothing there is overwritten.");
    if (!dest) return;
    job *j = new_job(JOB_RESTORE);
    if (!j) { free(dest); return; }
    j->version_id = version_id;
    j->dest = dest;
    enqueue(j);
}

/* ---- Files in use ------------------------------------------------------------ */

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

/* The files another program had open at the last scan or backup, and what
 * to do about them. say_none: also say when there are none. */
static void show_in_use(int say_none)
{
    in_use_list l = {NULL, 0, 0, 0};
    bd_list_in_use(g_cat, in_use_cb, &l);
    if (!l.n) {
        if (say_none) MessageBoxW(g_main, L"No files were in use by another program at the last scan or backup.", APP_NAME, MB_ICONINFORMATION);
        return;
    }
    wtext msg = {NULL, 0, 0};
    wchar_t size[64];
    format_bytes(l.bytes, size, 64);
    wadd(&msg, L"%d file%ls (%ls) %ls in use by another program, so BRODALF could not read %ls:\n\n", l.n, l.n == 1 ? L"" : L"s",
         size, l.n == 1 ? L"is" : L"are", l.n == 1 ? L"it" : L"them");
    for (int i = 0; i < l.n && i < 10; i++) {
        wchar_t *w = widen(l.paths[i]);
        wadd(&msg, L"    %ls\n", w ? w : L"");
        free(w);
    }
    if (l.n > 10) wadd(&msg, L"    ... and %d more\n", l.n - 10);
    wadd(&msg, L"\nClose the program that has %ls open (an email program often keeps its mail files open), then press "
               L"Scan folders, and Back up if you were backing up.", l.n == 1 ? L"it" : L"them");
    MessageBoxW(g_main, msg.p, APP_NAME, MB_ICONINFORMATION);
    free(msg.p);
    in_use_free(&l);
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
    int lost = m->lost_ms != 0;
    format_bytes(m->free_bytes, freeb, 64);
    format_time_ms(m->space_ms, when, 64);
    if (lost) swprintf(text, 700, L"Marked %ls: its copies do not count", m->lost_why && strcmp(m->lost_why, "broken") == 0 ? L"broken" : L"lost");
    else if (m->connected) swprintf(text, 700, L"Plugged in now at %ls, %ls free", root, freeb);
    else if (m->space_ms > 0) swprintf(text, 700, L"Not plugged in. %ls free when last seen (%ls)", freeb, when);
    else swprintf(text, 700, L"Not plugged in");
    AppendMenuW(sub, MF_STRING | MF_GRAYED, 0, text);
    AppendMenuW(sub, MF_SEPARATOR, 0, NULL);
    AppendMenuW(sub, MF_STRING | (m->connected && !lost ? 0 : MF_GRAYED), base + 0, L"Back up to it now");
    AppendMenuW(sub, MF_STRING | (m->connected ? 0 : MF_GRAYED), base + 2, L"Check every copy on it");
    AppendMenuW(sub, MF_STRING | (m->connected ? 0 : MF_GRAYED), base + 1, L"Read back copies not read lately");
    AppendMenuW(sub, MF_STRING | (m->connected ? 0 : MF_GRAYED), base + 6, L"Open in Explorer");
    AppendMenuW(sub, MF_SEPARATOR, 0, NULL);
    AppendMenuW(sub, MF_STRING, base + 3, L"Where it stands, name, where it is kept, hardware details...");
    AppendMenuW(sub, MF_STRING, base + 5, lost ? L"Found it: count its copies again" : L"Mark it lost or broken...");
    swprintf(text, 700, L"%ls%ls%ls%ls%ls", label, *loc ? L"  (" : L"", loc, *loc ? L")" : L"",
             lost ? L"  - marked lost or broken" : m->connected ? L"  - plugged in" : L"");
    AppendMenuW(f->local, MF_POPUP, (UINT_PTR)sub, text);
    f->n_local++;
    free(label); free(loc); free(root);
    return 0;
}

/* Fill the Local backups menu from the database, each time it opens. */
static void fill_storage_menus(void)
{
    clear_menu(g_menu_local);
    if (!g_cat) return;
    fill_menus f = {g_menu_local, 0};
    bd_list_media(g_cat, fill_menu_cb, &f);
    if (!f.n_local) AppendMenuW(g_menu_local, MF_STRING | MF_GRAYED, 0, L"No backup disks yet");
    AppendMenuW(g_menu_local, MF_SEPARATOR, 0, NULL);
    AppendMenuW(g_menu_local, MF_STRING, ID_M_ADD_DISK, L"Set up an external or removable disk...");
    AppendMenuW(g_menu_local, MF_STRING, ID_M_DRIVES, L"All disks...");
    AppendMenuW(g_menu_local, MF_STRING, ID_M_STATUS_ALL, L"Status of every disk...");
    AppendMenuW(g_menu_local, MF_STRING, ID_M_RISK, L"What needs backing up, and which disk to plug in...");
}

static void create_menu_bar(HWND hwnd)
{
    HMENU bar = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, ID_M_NEW_DB, L"New database...");
    AppendMenuW(file, MF_STRING, ID_M_OPEN_DB, L"Open database...");
    AppendMenuW(file, MF_STRING, ID_M_RECOVER_DB, L"Recover the database from a backup drive...");
    AppendMenuW(file, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file, MF_STRING, ID_M_SHEET, L"Print a recovery sheet...");
    AppendMenuW(file, MF_STRING, ID_M_EXPORT, L"Export the file list (CSV)...");
    AppendMenuW(file, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file, MF_STRING, ID_M_SETTINGS, L"Settings...");
    AppendMenuW(file, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file, MF_STRING, ID_M_EXIT, L"Exit");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)file, L"&File");
    HMENU db = CreatePopupMenu();
    AppendMenuW(db, MF_STRING, ID_M_ADD_FOLDER, L"Add a backup root...");
    AppendMenuW(db, MF_STRING, ID_M_SCAN, L"Scan the backup roots for changes now");
    AppendMenuW(db, MF_STRING, ID_M_BITROT, L"Bit-rot scan: read every file again...");
    AppendMenuW(db, MF_STRING, ID_M_RELOCATE, L"Change where a backup root is...");
    AppendMenuW(db, MF_SEPARATOR, 0, NULL);
    AppendMenuW(db, MF_STRING, ID_M_DELETED, L"Deleted files...");
    AppendMenuW(db, MF_STRING, ID_M_DAMAGED, L"Damaged files (found by a bit-rot scan)...");
    AppendMenuW(db, MF_STRING, ID_M_IN_USE, L"Files that were in use at the last scan...");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)db, L"&Database");
    g_menu_local = CreatePopupMenu();
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)g_menu_local, L"&Local backups");
    HMENU rest = CreatePopupMenu();
    AppendMenuW(rest, MF_STRING, ID_M_RESTORE, L"Restore what is selected...");
    AppendMenuW(rest, MF_STRING, ID_M_RESTORE_DELETED, L"Restore deleted files...");
    AppendMenuW(rest, MF_STRING, ID_M_CONFLICTS, L"Files the last restore left alone...");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)rest, L"&Restore");
    HMENU view = CreatePopupMenu();
    AppendMenuW(view, MF_STRING, ID_M_STATUS_ALL, L"Status of every drive...");
    AppendMenuW(view, MF_STRING, ID_M_RISK, L"Files short of your copies...");
    AppendMenuW(view, MF_STRING, ID_M_LARGEST, L"Largest files...");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)view, L"&View");
    HMENU help = CreatePopupMenu();
    AppendMenuW(help, MF_STRING, ID_M_LOG, L"Show the app log");
    AppendMenuW(help, MF_STRING, ID_M_REPORTS, L"Show saved error reports");
    AppendMenuW(help, MF_STRING, ID_M_ABOUT, L"About BRODALF");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)help, L"&Help");
    SetMenu(hwnd, bar);
}

/* With no database open only File (new, open, recover, exit) and Help
 * work; while a job runs only Help does. */
static void update_menus(void)
{
    HMENU bar = GetMenu(g_main);
    if (!bar) return;
    int n = GetMenuItemCount(bar);
    for (int i = 0; i < n; i++) {
        int on = !g_busy && (g_cat || i == 0 || i == n - 1);
        if (g_busy && i == n - 1) on = 1;
        EnableMenuItem(bar, (UINT)i, MF_BYPOSITION | (on ? MF_ENABLED : MF_GRAYED));
    }
    HMENU file = GetSubMenu(bar, 0);
    if (file) {
        static const UINT need_cat[] = {ID_M_SHEET, ID_M_EXPORT, ID_M_SETTINGS};
        for (int i = 0; i < 3; i++) EnableMenuItem(file, need_cat[i], MF_BYCOMMAND | (g_cat ? MF_ENABLED : MF_GRAYED));
    }
    DrawMenuBar(g_main);
}

/* ---- Lost and broken drives ----------------------------------------------- */

typedef struct { bd_set_progress s[MAX_SET_BARS]; int n; } sets_snap;

static sets_snap read_sets(void)
{
    sets_snap s;
    memset(&s, 0, sizeof(s));
    if (bd_backup_sets(g_cat, s.s, MAX_SET_BARS, &s.n) != BD_OK) s.n = 0;
    if (s.n > MAX_SET_BARS) s.n = MAX_SET_BARS;
    return s;
}

static int set_percent(const bd_set_progress *s)
{
    int pct = s->bytes_total > 0 ? (int)(s->bytes_done * 100 / s->bytes_total) : s->files_total > 0 ? (int)(s->files_done * 100 / s->files_total) : 100;
    if (pct >= 100 && s->files_done < s->files_total) pct = 99;
    return pct > 100 ? 100 : pct;
}

/* Mark a drive lost or broken, after showing what it does to the full
 * copies: its copies stop counting, so the files it held need backing up
 * again to other drives. */
static void mark_lost_ui(const drive_snap *d)
{
    wchar_t name[600], head[700];
    drive_name(d->label, d->location, name, 600);
    swprintf(head, 700, L"What happened to %ls?", name);
    TASKDIALOG_BUTTON b[2] = {
        {650, L"It is lost\nYou do not know where it is."},
        {651, L"It is broken\nIt no longer works, or its files cannot be read."},
    };
    TASKDIALOGCONFIG tc;
    memset(&tc, 0, sizeof(tc));
    tc.cbSize = sizeof(tc);
    tc.hwndParent = g_main;
    tc.hInstance = g_inst;
    tc.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
    tc.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    tc.pszWindowTitle = APP_NAME;
    tc.pszMainInstruction = head;
    tc.pszContent = L"BRODALF stops counting its copies, so your full copies show what you really have, and backs the files it "
                    L"held up again to your other drives. Nothing on any drive is deleted, and you can undo this with Found it.";
    tc.cButtons = 2;
    tc.pButtons = b;
    int pressed = IDCANCEL;
    if (FAILED(TaskDialogIndirect(&tc, &pressed, NULL, NULL)) || (pressed != 650 && pressed != 651)) return;
    const char *why = pressed == 651 ? "broken" : "lost";
    const wchar_t *wwhy = pressed == 651 ? L"broken" : L"lost";
    sets_snap before = read_sets();
    bd_status st = bd_media_mark_lost(g_cat, d->id, why);
    if (st != BD_OK) {
        wchar_t *e = widen(bd_catalog_error(g_cat));
        MessageBoxW(g_main, e ? e : L"The drive could not be marked.", APP_NAME, MB_ICONWARNING);
        free(e);
        return;
    }
    sets_snap after = read_sets();
    wtext t = {NULL, 0, 0};
    wadd(&t, L"Marking %ls %ls changes your full copies:\n\n", name, wwhy);
    int changed = 0;
    for (int i = 0; i < before.n || i < after.n; i++) {
        int a = i < before.n ? set_percent(&before.s[i]) : 0, z = i < after.n ? set_percent(&after.s[i]) : 0;
        int64_t fa = i < before.n ? before.s[i].files_done : 0, fz = i < after.n ? after.s[i].files_done : 0;
        if (fa != fz) changed = 1;
        wadd(&t, L"    Full copy %d: %d%% now, %d%% after (%lld files fewer)\n", i + 1, a, z, (long long)(fa - fz));
    }
    if (!changed) wadd(&t, L"    No change: every file it held has enough good copies elsewhere.\n");
    wadd(&t, L"\nMark it %ls?", wwhy);
    int ok = MessageBoxW(g_main, t.p ? t.p : L"", APP_NAME, MB_ICONWARNING | MB_YESNO) == IDYES;
    free(t.p);
    if (!ok) {
        bd_media_found(g_cat, d->id);
        return;
    }
    if (bd_catalog_save(g_cat) == BD_OK) {
        wchar_t line[800];
        swprintf(line, 800, L"%ls is marked %ls. Its copies no longer count; back up to your other drives to make up for it.", name, wwhy);
        log_append(line);
    }
}

static void mark_found_ui(const drive_snap *d)
{
    wchar_t name[600], line[800];
    drive_name(d->label, d->location, name, 600);
    if (bd_media_found(g_cat, d->id) == BD_OK && bd_catalog_save(g_cat) == BD_OK) {
        swprintf(line, 800, L"%ls is back: its copies count again. Check every copy on it next time it is plugged in, to be sure they are "
                            L"still good.", name);
        log_append(line);
    }
}

/* The drive details dialog: name, where it is kept, lost or found. */
static void edit_drive(int64_t id)
{
    drive_list dl = load_drives();
    const drive_snap *d = find_drive(&dl, id);
    if (!d) { free(dl.items); return; }
    g_edit_drive = d;
    INT_PTR r = DialogBoxW(g_inst, MAKEINTRESOURCEW(IDD_DRIVE), g_main, drive_proc);
    g_edit_drive = NULL;
    bd_status st = BD_OK;
    if (r == IDOK || r == IDC_DRIVE_LOST) {
        char *name = narrow(g_edit_name), *loc = narrow(g_edit_location);
        if (name && *name && strcmp(name, d->label) != 0) st = bd_media_rename(g_cat, id, name);
        if (st == BD_OK && loc && strcmp(loc, d->location) != 0) st = bd_media_set_location(g_cat, id, loc);
        if (st == BD_OK) st = bd_catalog_save(g_cat);
        free(name);
        free(loc);
    }
    if (st == BD_OK && r == IDC_DRIVE_LOST) {
        if (d->lost_ms) mark_found_ui(d);
        else mark_lost_ui(d);
    }
    if (st != BD_OK) {
        wchar_t *e = widen(bd_catalog_error(g_cat));
        MessageBoxW(g_main, e ? e : L"The drive could not be changed.", APP_NAME, MB_ICONWARNING);
        free(e);
    }
    free(dl.items);
    update_drives_label();
    rebuild_tree();
    show_detail();
}

static void open_in_explorer(const wchar_t *path)
{
    ShellExecuteW(g_main, L"open", path, NULL, NULL, SW_SHOWNORMAL);
}

/* Explorer with the file or folder selected. */
static void show_in_explorer(const wchar_t *path)
{
    wchar_t *args = malloc(sizeof(wchar_t) * (wcslen(path) + 16));
    if (!args) return;
    swprintf(args, wcslen(path) + 16, L"/select,\"%ls\"", path);
    ShellExecuteW(g_main, NULL, L"explorer.exe", args, NULL, SW_SHOWNORMAL);
    free(args);
}

static void drive_action(int64_t id, int action)
{
    drive_list dl = load_drives();
    const drive_snap *d = find_drive(&dl, id);
    if (!d) { free(dl.items); return; }
    if (action == 0) {
        if (!d->lost_ms && (!d->encrypted || unlock_ui(L"This drive is encrypted. Enter the passphrase to back up to it."))) {
            job *j = new_job(JOB_BACKUP);
            if (j) {
                j->media_id = id;
                start_backup(j, 1);
            }
        }
    } else if (action == 1) {
        enqueue_verify(id);
    } else if (action == 2) {
        enqueue_check(id);
    } else if (action == 3) {
        edit_drive(id);
    } else if (action == 5) {
        if (d->lost_ms) mark_found_ui(d);
        else mark_lost_ui(d);
        update_drives_label();
        rebuild_tree();
        show_detail();
    } else if (action == 6 && d->connected) {
        wchar_t *root = widen(d->root);
        if (root) open_in_explorer(root);
        free(root);
    }
    free(dl.items);
}

enum { CHOICE_LAST = 100, CHOICE_OPEN, CHOICE_NEW, CHOICE_RECOVER };
static void open_database_ui(int choice);
static void cmd_bitrot(void);

static void menu_command(int cmd)
{
    if (cmd >= ID_DRIVE_ACT_BASE) {
        if (g_cat) drive_action((cmd - ID_DRIVE_ACT_BASE) / 8, (cmd - ID_DRIVE_ACT_BASE) % 8);
        return;
    }
    switch (cmd) {
    case ID_M_NEW_DB: open_database_ui(CHOICE_NEW); return;
    case ID_M_OPEN_DB: open_database_ui(CHOICE_OPEN); return;
    case ID_M_RECOVER_DB: open_database_ui(CHOICE_RECOVER); return;
    case ID_M_EXIT: PostMessageW(g_main, WM_CLOSE, 0, 0); return;
    case ID_M_LOG: {
        wchar_t *w = widen(bd_applog_path());
        if (w) open_in_explorer(w);
        free(w);
        return;
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
        return;
    }
    case ID_M_ABOUT: {
        wchar_t *v = widen(bd_version()), msg[700];
        swprintf(msg, 700, L"BRODALF %ls\n\nKeeps track of what has been backed up from your backup roots, and to which drive, and "
                           L"whether each copy is still good. It does nothing until you press a button.\n\nhttps://github.com/Phawx/BRODALF",
                 v ? v : L"");
        free(v);
        MessageBoxW(g_main, msg, APP_NAME, MB_ICONINFORMATION);
        return;
    }
    }
    if (!g_cat) return;
    switch (cmd) {
    case ID_M_ADD_FOLDER: cmd_add_folder(); break;
    case ID_M_SCAN: enqueue(new_job(JOB_SCAN)); break;
    case ID_M_BITROT: cmd_bitrot(); break;
    case ID_M_RELOCATE: cmd_relocate(); break;
    case ID_M_SHEET: cmd_sheet(0); break;
    case ID_M_DELETED:
    case ID_M_RESTORE_DELETED: cmd_deleted(); break;
    case ID_M_DAMAGED: cmd_table(TABLE_DAMAGED); break;
    case ID_M_LARGEST: cmd_table(TABLE_LARGEST); break;
    case ID_M_CONFLICTS: cmd_table(TABLE_CONFLICTS); break;
    case ID_M_RESTORE: cmd_restore(); break;
    case ID_M_RISK: cmd_risk(); break;
    case ID_M_ADD_DISK: add_disk(); break;
    case ID_M_DRIVES: cmd_drives(); break;
    case ID_M_IN_USE: show_in_use(1); break;
    case ID_M_STATUS_ALL: cmd_table(TABLE_DRIVES); break;
    case ID_M_EXPORT: cmd_export(); break;
    case ID_M_SETTINGS: cmd_security(); break;
    }
}

/* A scan that also reads every unchanged file again, to find files damaged
 * on the PC's own disk (bit rot): their copies on the drives are then the
 * good ones. */
static void cmd_bitrot(void)
{
    if (MessageBoxW(g_main, L"A bit-rot scan reads every file in your backup roots again and compares it with its checksum, to find "
                            L"files that changed although their size and time did not (damage on this PC's disk). It takes as long as "
                            L"the first scan did. Nothing is changed or backed up.\n\nStart it now?",
                    APP_NAME, MB_ICONQUESTION | MB_YESNO) != IDYES)
        return;
    job *j = new_job(JOB_SCAN);
    if (!j) return;
    j->bitrot = 1;
    enqueue(j);
}

/* ---- Right-click menus ------------------------------------------------- */

static void cmd_relocate_one(int64_t source_id);

/* Right-click on the tree: open the file or folder, restore it, and for a
 * backup root stop or resume backing it up, its own skip list, or where it
 * is now. */
static void tree_context_menu(int x, int y)
{
    HTREEITEM h;
    if (x == -1 && y == -1) {
        h = TreeView_GetSelection(g_tree);
        RECT r;
        if (!h || !TreeView_GetItemRect(g_tree, h, &r, TRUE)) return;
        POINT pt = {r.left, r.bottom};
        ClientToScreen(g_tree, &pt);
        x = pt.x;
        y = pt.y;
    } else {
        TVHITTESTINFO hit;
        memset(&hit, 0, sizeof(hit));
        hit.pt.x = x;
        hit.pt.y = y;
        ScreenToClient(g_tree, &hit.pt);
        h = TreeView_HitTest(g_tree, &hit);
        if (!h) return;
        TreeView_SelectItem(g_tree, h);
    }
    node_ref *r = item_ref(h);
    if (!r) return;
    /* The source's own path and state. */
    source_list l = {0};
    bd_list_sources(g_cat, collect_source, &l);
    char *root_path = NULL;
    wchar_t root_name[300] = L"";
    int active = 1, missing = 0;
    for (int i = 0; i < l.n; i++) {
        if (l.items[i].source_id == r->source_id) {
            root_path = xstrdup(l.paths[i]);
            wchar_t *w = widen(l.names[i]);
            swprintf(root_name, 300, L"%ls", w ? w : L"");
            free(w);
            active = l.items[i].active;
            missing = l.items[i].missing;
        }
        free(l.names[i]);
        free(l.paths[i]);
        free(l.labels[i]);
    }
    free(l.items);
    free(l.names);
    free(l.paths);
    free(l.labels);
    int is_root = r->node_id == 0;
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (missing || r->state == BD_STATE_DELETED ? MF_GRAYED : 0), ID_CTX_OPEN, L"Open in Explorer");
    AppendMenuW(m, MF_STRING, ID_CTX_RESTORE, L"Restore...");
    if (is_root) {
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        AppendMenuW(m, MF_STRING, active ? ID_CTX_STOP_ROOT : ID_CTX_RESUME_ROOT,
                    active ? L"Stop backing up this root..." : L"Back up this root again");
        AppendMenuW(m, MF_STRING, ID_CTX_ROOT_SKIP, L"What to leave out in this root...");
        AppendMenuW(m, MF_STRING, ID_CTX_RELOCATE, L"Change where it is...");
    }
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, x, y, 0, g_main, NULL);
    DestroyMenu(m);
    if (cmd == ID_CTX_OPEN && root_path) {
        size_t n = strlen(root_path) + strlen(r->rel ? r->rel : "") + 4;
        char *full = malloc(n);
        if (full) {
            snprintf(full, n, "%s%s%s", root_path, r->rel && *r->rel ? "\\" : "", r->rel ? r->rel : "");
            for (char *c = full; *c; c++) if (*c == '/') *c = '\\';
            wchar_t *w = widen(full);
            if (w) {
                if (r->is_dir) open_in_explorer(w);
                else show_in_explorer(w);
            }
            free(w);
            free(full);
        }
    } else if (cmd == ID_CTX_RESTORE) {
        cmd_restore();
    } else if (cmd == ID_CTX_STOP_ROOT) {
        wchar_t q[900];
        swprintf(q, 900, L"Stop backing up %ls?\n\nScans leave it alone and nothing new from it is backed up. Everything BRODALF "
                         L"knows about it stays, and its copies on your drives stay and still restore. You can back it up again "
                         L"at any time.", root_name);
        if (MessageBoxW(g_main, q, APP_NAME, MB_ICONQUESTION | MB_YESNO) == IDYES && bd_source_set_active(g_cat, r->source_id, 0) == BD_OK &&
            bd_catalog_save(g_cat) == BD_OK) {
            wchar_t line[400];
            swprintf(line, 400, L"%ls is no longer backed up.", root_name);
            log_append(line);
            rebuild_tree();
            update_drives_label();
        }
    } else if (cmd == ID_CTX_RESUME_ROOT) {
        if (bd_source_set_active(g_cat, r->source_id, 1) == BD_OK && bd_catalog_save(g_cat) == BD_OK) {
            wchar_t line[400];
            swprintf(line, 400, L"%ls is backed up again. Scanning it.", root_name);
            log_append(line);
            enqueue(new_job(JOB_SCAN));
        }
    } else if (cmd == ID_CTX_ROOT_SKIP) {
        cmd_skip(r->source_id, root_name);
    } else if (cmd == ID_CTX_RELOCATE) {
        cmd_relocate_one(r->source_id);
    }
    free(root_path);
}

/* Right-click on the versions list: restore that version, or open the
 * copy on its drive. */
static void list_context_menu(int x, int y)
{
    int row = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
    if (x != -1 || y != -1) {
        LVHITTESTINFO hit;
        memset(&hit, 0, sizeof(hit));
        hit.pt.x = x;
        hit.pt.y = y;
        ScreenToClient(g_list, &hit.pt);
        row = ListView_HitTest(g_list, &hit);
    } else {
        RECT rc;
        GetWindowRect(g_list, &rc);
        x = rc.left + S(20);
        y = rc.top + S(20);
    }
    if (row < 0 || row >= g_n_copy_rows) return;
    const copy_row *c = &g_copy_rows[row];
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, ID_CTX_RESTORE_VERSION, L"Restore this version...");
    AppendMenuW(m, MF_STRING | (c->full ? 0 : MF_GRAYED), ID_CTX_OPEN_VERSION, L"Open in Explorer (on its drive)");
    int64_t version_id = c->version_id;
    wchar_t *full = c->full ? _wcsdup(c->full) : NULL;
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, x, y, 0, g_main, NULL);
    DestroyMenu(m);
    if (cmd == ID_CTX_RESTORE_VERSION) restore_version(version_id);
    else if (cmd == ID_CTX_OPEN_VERSION && full) show_in_explorer(full);
    free(full);
}

/* ---- After a scan ------------------------------------------------------- */

/* What a scan found that needs the user: backup roots it could not find,
 * files that may be damaged on this PC, files gone from the roots; then
 * the backup the user asked for, if they chose to scan first. */
static void after_scan(job *j)
{
    if (j->roots_missing) ask_missing_roots();
    if (j->damaged) {
        wchar_t q[600];
        swprintf(q, 600, L"%lld file%ls changed although %ls size and time did not. That usually means damage on this PC's disk "
                         L"(bit rot), and the copies on your drives are the good ones.\n\nShow %ls now? You can restore %ls or "
                         L"accept %ls as they are.",
                 (long long)j->damaged, j->damaged == 1 ? L"" : L"s", j->damaged == 1 ? L"its" : L"their",
                 j->damaged == 1 ? L"it" : L"them", j->damaged == 1 ? L"it" : L"them", j->damaged == 1 ? L"it" : L"them");
        if (MessageBoxW(g_main, q, APP_NAME, MB_ICONWARNING | MB_YESNO) == IDYES) cmd_table(TABLE_DAMAGED);
    }
    int64_t gone = 0;
    if ((j->found_deleted || j->after_open) && bd_list_deleted(g_cat, 0, 1, NULL, NULL, &gone) == BD_OK && gone > 0) {
        wchar_t q[600];
        swprintf(q, 600, L"%lld file%ls gone from your backup roots since you last looked. BRODALF keeps tracking %ls and %ls "
                         L"copies on your drives.\n\nLook at %ls now? You can restore %ls, keep tracking %ls, or forget %ls.",
                 (long long)gone, gone == 1 ? L" is" : L"s are", gone == 1 ? L"it" : L"them", gone == 1 ? L"its" : L"their",
                 gone == 1 ? L"it" : L"them", gone == 1 ? L"it" : L"them", gone == 1 ? L"it" : L"them", gone == 1 ? L"it" : L"them");
        if (MessageBoxW(g_main, q, APP_NAME, MB_ICONQUESTION | MB_YESNO) == IDYES) cmd_deleted();
    }
    if (j->then_backup) {
        /* The user chose to scan first: show what the backup does now. */
        job *b = new_job(JOB_BACKUP);
        if (b) {
            b->media_id = j->then_backup;
            start_backup(b, 0);
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
    if (!g_cat) return;
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
    if (g_busy) {
        /* The catalog belongs to the job; the search runs again when it ends. */
        const wchar_t *cells[] = {L"Search is available when the current job finishes.", L"", L"", L""};
        lv_row(g_results, cells, 4);
        SendMessageW(g_results, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(g_results, NULL, TRUE);
        ShowWindow(g_tree, SW_HIDE);
        ShowWindow(g_results, SW_SHOW);
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
    if (!g_main || !g_status) return;
    RECT rc;
    GetClientRect(g_main, &rc);
    SendMessageW(g_status, WM_SIZE, 0, 0);
    RECT sr;
    GetWindowRect(g_status, &sr);
    int status_h = sr.bottom - sr.top;
    int w = rc.right, h = rc.bottom - status_h;
    int pad = S(8), bar = S(30), log_h = S(100);

    int x = pad;
    int widths[N_BUTTONS] = {S(124), S(84), S(84), S(104), S(84), S(80), S(84), S(104)};
    for (int i = 0; i < N_BUTTONS; i++) {
        MoveWindow(g_buttons[i], x, pad, widths[i], bar, TRUE);
        x += widths[i] + S(6);
    }
    MoveWindow(g_drives, x + S(10), pad + S(7), w - x - S(10) - pad, bar - S(7), TRUE);

    /* One bar per full copy the user wants, under the buttons. */
    int top = pad + bar + pad, row_h = S(18), bar_w = S(240);
    for (int i = 0; i < MAX_SET_BARS; i++) {
        if (i >= g_set_count) continue;
        MoveWindow(g_set_bar[i], pad, top, bar_w, row_h - S(4), TRUE);
        MoveWindow(g_set_text[i], pad + bar_w + S(8), top - S(1), w - pad * 2 - bar_w - S(8), row_h, TRUE);
        top += row_h + S(2);
    }
    if (g_set_count) top += pad - S(2);

    int prog_h = S(20);
    int body_h = h - top - log_h - prog_h * 2 - pad * 4;
    int tree_w = (w - pad * 3) * 55 / 100, search_h = S(24), below = search_h + S(6);
    MoveWindow(g_search, pad, top, tree_w, search_h, TRUE);
    MoveWindow(g_tree, pad, top + below, tree_w, body_h - below, TRUE);
    MoveWindow(g_results, pad, top + below, tree_w, body_h - below, TRUE);
    int rx = pad * 2 + tree_w, rw = w - rx - pad;
    int detail_h = body_h * 58 / 100;
    MoveWindow(g_detail, rx, top, rw, detail_h, TRUE);
    MoveWindow(g_list, rx, top + detail_h + S(6), rw, body_h - detail_h - S(6), TRUE);
    /* The job as a whole (for a backup: everything going to the plugged-in
     * drive), then the file being copied now. */
    int py = top + body_h + pad;
    MoveWindow(g_progress, pad, py, bar_w, prog_h, TRUE);
    MoveWindow(g_stop, pad + bar_w + S(8), py - S(2), S(64), prog_h + S(4), TRUE);
    MoveWindow(g_prog_text, pad + bar_w + S(80), py, w - pad * 2 - bar_w - S(80), prog_h, TRUE);
    py += prog_h + pad;
    MoveWindow(g_file_progress, pad, py, bar_w, prog_h, TRUE);
    MoveWindow(g_file_text, pad + bar_w + S(80), py, w - pad * 2 - bar_w - S(80), prog_h, TRUE);
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
    g_buttons[0] = make_button(L"Add backup root...", ID_BTN_ADD);
    g_buttons[1] = make_button(L"Scan now", ID_BTN_SCAN);
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
    g_stop = make_button(L"Stop", ID_BTN_STOP);
    ShowWindow(g_stop, SW_HIDE);
    g_file_progress = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE | PBS_SMOOTH, 0, 0, 10, 10, hwnd, NULL, g_inst, NULL);
    g_file_text = CreateWindowExW(0, WC_STATICW, L"", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_PATHELLIPSIS | SS_CENTERIMAGE, 0, 0,
                                  10, 10, hwnd, NULL, g_inst, NULL);
    SendMessageW(g_file_text, WM_SETFONT, (WPARAM)g_font, TRUE);
    for (int i = 0; i < MAX_SET_BARS; i++) {
        g_set_bar[i] = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | PBS_SMOOTH, 0, 0, 10, 10, hwnd, NULL, g_inst, NULL);
        g_set_text[i] = CreateWindowExW(0, WC_STATICW, L"", WS_CHILD | SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS | SS_CENTERIMAGE, 0, 0, 10, 10,
                                        hwnd, NULL, g_inst, NULL);
        SendMessageW(g_set_text[i], WM_SETFONT, (WPARAM)g_font, TRUE);
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
        create_menu_bar(hwnd);
        set_busy(0);
        return 0;
    case WM_INITMENUPOPUP:
        if ((HMENU)wp == g_menu_local && !g_busy) fill_storage_menus();
        return 0;
    case WM_CONTEXTMENU:
        if (((HWND)wp == g_tree || (HWND)wp == g_list) && g_cat && !g_busy && !g_ui_depth) {
            int x = (int)(short)LOWORD(lp), y = (int)(short)HIWORD(lp);
            g_ui_depth++;
            if ((HWND)wp == g_tree) tree_context_menu(x, y);
            else list_context_menu(x, y);
            g_ui_depth--;
            start_next_job();
            return 0;
        }
        break;
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
        if (LOWORD(wp) == ID_BTN_STOP) {
            stop_job();
            return 0;
        }
        if (g_busy || g_ui_depth) return 0;
        if (!g_cat && LOWORD(wp) < ID_M_ADD_FOLDER) return 0; /* the buttons need a database */
        g_ui_depth++; /* a job this asks for starts once it is done */
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
        g_ui_depth--;
        start_next_job();
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
        show_file_progress(m->file_percent, m->file);
        free(m);
        return 0;
    }
    case WM_APP_DONE: {
        job *j = (job *)lp;
        int stopping = g_stopping != 0;
        /* The worker has let go of the catalog. Until the prompts below are
         * answered, the buttons stay disabled and nothing new starts. */
        g_ui_depth++;
        InterlockedExchange(&g_busy, 0);
        EnableWindow(g_stop, FALSE);
        ShowWindow(g_stop, SW_HIDE);
        SendMessageW(g_status, SB_SETTEXTW, 0, (LPARAM)j->summary);
        show_progress(j->status == BD_OK ? 100 : 0, j->summary);
        show_file_progress(-1, L"");
        log_append(j->summary);
        if (g_close_after) {
            job_free(j);
            DestroyWindow(hwnd);
            return 0;
        }
        update_drives_label();
        rebuild_tree();
        show_detail();
        if (stopping) {
            /* Nothing follows a job the user stopped, but a stopped scan
             * leaves the backup working from what the last scans found. */
            if (j->kind == JOB_SCAN)
                MessageBoxW(hwnd, L"The scan was stopped before it finished, so BRODALF may not know about every new or changed "
                                  L"file yet. A backup now copies what the scans so far found.\n\nScan again when you can, so your "
                                  L"backups are as up to date as possible.",
                            APP_NAME, MB_ICONINFORMATION);
        } else {
            if (j->status == BD_ERR_PASSPHRASE && (j->kind == JOB_BACKUP || j->kind == JOB_CHECK || j->kind == JOB_VERIFY) &&
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
            if (GetWindowTextLengthW(g_search) > 0) run_search();
            if (j->kind == JOB_DRIVES) after_drives(j);
            else if (j->kind == JOB_SCAN && j->status == BD_OK) after_scan(j);
            else if (j->kind == JOB_BACKUP && j->status == BD_OK) after_backup(j);
            else if (j->kind == JOB_RESTORE && j->status == BD_OK) after_restore(j);
            if ((j->kind == JOB_SCAN || j->kind == JOB_BACKUP) && j->in_use && j->status == BD_OK) show_in_use(0);
        }
        job_free(j);
        g_ui_depth--;
        if (g_close_after) { /* Close was answered while the questions above were up */
            DestroyWindow(hwnd);
            return 0;
        }
        set_busy(0);
        update_drives_label();
        start_next_job();
        return 0;
    }
    case WM_DEVICECHANGE:
        if (wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE) {
            DEV_BROADCAST_HDR *hdr = (DEV_BROADCAST_HDR *)lp;
            if (hdr && hdr->dbch_devicetype == DBT_DEVTYP_VOLUME && g_cat) {
                job *dj = new_job(JOB_DRIVES);
                if (dj) {
                    dj->plugged_in = wp == DBT_DEVICEARRIVAL;
                    if (dj->plugged_in) dj->arrived = ((DEV_BROADCAST_VOLUME *)hdr)->dbcv_unitmask;
                    enqueue(dj);
                }
            }
        }
        return TRUE;
    case WM_CLOSE:
        if (g_busy) {
            static const wchar_t *ask[] = {L"Stop looking for drives and close?", L"Stop the scan and close?",
                                           L"Stop the backup and close?", L"Stop the check and close?",
                                           L"Stop the restore and close?", L"Stop reading back the copies and close?"};
            if (g_close_after) return 0; /* already stopping; closes when the job returns */
            wchar_t text[256];
            swprintf(text, 256, L"%ls\n\nWhat was done until then is kept. BRODALF closes as soon as the job has stopped.",
                     ask[g_running_kind]);
            if (MessageBoxW(hwnd, text, APP_NAME, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES) {
                g_close_after = 1;
                if (g_busy) stop_job();
                else if (!g_ui_depth) DestroyWindow(hwnd); /* the job ended while the question was up */
                /* else the end-of-job questions are up: it closes once they are answered */
            }
            return 0;
        }
        if (g_ui_depth) {
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

/* ---- Opening, creating and recovering a database ------------------------ */

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

static int open_dialog(wchar_t *out, DWORD cap)
{
    OPENFILENAMEW of;
    memset(&of, 0, sizeof(of));
    of.lStructSize = sizeof(of);
    of.hwndOwner = g_main;
    of.lpstrFilter = L"BRODALF databases (*.brodalf)\0*.brodalf\0All files\0*.*\0";
    of.lpstrFile = out;
    of.nMaxFile = cap;
    of.lpstrDefExt = L"brodalf";
    out[0] = 0;
    of.lpstrTitle = L"Open a BRODALF database";
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&of);
}

/* Where to keep a new database file. Never an existing file: nothing is
 * overwritten. */
static int save_dialog(wchar_t *out, DWORD cap, const wchar_t *title)
{
    for (;;) {
        OPENFILENAMEW of;
        memset(&of, 0, sizeof(of));
        of.lStructSize = sizeof(of);
        of.hwndOwner = g_main;
        of.lpstrFilter = L"BRODALF databases (*.brodalf)\0*.brodalf\0";
        if (!out[0]) wcsncpy(out, L"My backups.brodalf", cap - 1);
        of.lpstrFile = out;
        of.nMaxFile = cap;
        of.lpstrDefExt = L"brodalf";
        of.lpstrTitle = title;
        of.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetSaveFileNameW(&of)) return 0;
        if (GetFileAttributesW(out) == INVALID_FILE_ATTRIBUTES) return 1;
        MessageBoxW(g_main, L"A file with that name is already there. Choose another name, so nothing is overwritten.", APP_NAME,
                    MB_ICONINFORMATION);
    }
}

static int ask_database_choice(const wchar_t *last)
{
    wchar_t last_btn[MAX_PATH * 2 + 64];
    TASKDIALOG_BUTTON buttons[4];
    int n = 0;
    if (last[0]) {
        swprintf(last_btn, MAX_PATH * 2 + 64, L"Open the last database\n%ls", last);
        buttons[n].nButtonID = CHOICE_LAST;
        buttons[n++].pszButtonText = last_btn;
    }
    buttons[n].nButtonID = CHOICE_OPEN;
    buttons[n++].pszButtonText = L"Open a database...\nA .brodalf file you made before";
    buttons[n].nButtonID = CHOICE_NEW;
    buttons[n++].pszButtonText = L"Create a new database...\nChoose the folders to back up, then where to keep the database file";
    buttons[n].nButtonID = CHOICE_RECOVER;
    buttons[n++].pszButtonText = L"Recover the database from a backup drive...\nOn a new PC, or after losing the database file: "
                                 L"start from the newest copy on your drives";

    TASKDIALOGCONFIG tc;
    memset(&tc, 0, sizeof(tc));
    tc.cbSize = sizeof(tc);
    tc.hwndParent = g_main;
    tc.hInstance = g_inst;
    tc.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
    tc.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    tc.pszWindowTitle = APP_NAME;
    tc.pszMainInstruction = L"Which database do you want to use?";
    tc.pszContent = L"A database records every file in your backup roots and which drives hold a copy of it. It never holds the "
                    L"files themselves.";
    tc.cButtons = (UINT)n;
    tc.pButtons = buttons;
    int pressed = IDCANCEL;
    if (FAILED(TaskDialogIndirect(&tc, &pressed, NULL, NULL))) return IDCANCEL;
    return pressed;
}

/* Open a database, asking for the passphrase if the file is encrypted.
 * A cancelled prompt returns BD_ERR_PASSPHRASE with no message. */
static int g_opened_by_recovery; /* offer a new passphrase once it is open */

static bd_status open_maybe_encrypted(const char *path, bd_catalog **out)
{
    if (!bd_catalog_file_needs_passphrase(path)) return bd_catalog_open(path, out);
    int can_recover = bd_catalog_file_has_recovery_key(path);
    for (;;) {
        int recovery = 0;
        char *p = ask_unlock_secret(g_main, L"This database is encrypted. Enter its passphrase to open it.", can_recover, &recovery);
        if (!p) return BD_ERR_PASSPHRASE;
        bd_status s = recovery ? bd_catalog_open_with_recovery(path, p, out) : bd_catalog_open_with(path, p, out);
        free_secret(p);
        if (s == BD_OK && recovery) g_opened_by_recovery = 1;
        if (s != BD_ERR_PASSPHRASE) return s;
        MessageBoxW(g_main, recovery ? L"That recovery key is not right. Check it against the sheet and try again."
                                     : L"That passphrase is not right. Try again.",
                    APP_NAME, MB_ICONWARNING);
    }
}

static void show_open_error(bd_status s, const wchar_t *fallback)
{
    wchar_t *e = widen(bd_open_error());
    const wchar_t *why = e && e[0] ? e : fallback;
    /* Picking a file that is not a database is not a bug. Nor is one that
     * is open in another BRODALF window (a named mutex says so; Windows
     * drops it if that BRODALF crashed). */
    if (s == BD_ERR_LOCKED) MessageBoxW(g_main, why, APP_NAME, MB_ICONINFORMATION);
    else if (s == BD_ERR_FORMAT || s == BD_ERR_NOT_FOUND || s == BD_ERR_EXISTS) MessageBoxW(g_main, why, APP_NAME, MB_ICONERROR);
    else report_error(g_main, why);
    free(e);
}

/* Save and close what is open, and clear the window. */
static void close_database(void)
{
    if (!g_cat) return;
    bd_catalog_save(g_cat);
    bd_catalog_close(g_cat);
    g_cat = NULL;
    g_cat_path[0] = 0;
    while (g_queue) {
        job *n = g_queue->next;
        job_free(g_queue);
        g_queue = n;
    }
    clear_pending();
    free_conflicts(g_conflicts, g_n_conflicts);
    g_conflicts = NULL;
    g_n_conflicts = 0;
    g_n_check_offered = 0;
    SetWindowTextW(g_search, L"");
    end_search();
    rebuild_tree();
    show_detail();
    SetWindowTextW(g_main, APP_NAME);
    update_drives_label();
    set_busy(0);
}

typedef struct { int n; } count_ctx;
static int count_source(void *ctx, const bd_source_info *info) { (void)info; ((count_ctx *)ctx)->n++; return 0; }

/* A database is open: show it, look for its drives, and scan its backup
 * roots, as the user asked: opening a database always starts a scan, so
 * the backup works from where things are now. */
static void database_opened(const wchar_t *path, int created, int recovered)
{
    wcsncpy(g_cat_path, path, MAX_PATH * 2 - 1);
    g_cat_path[MAX_PATH * 2 - 1] = 0;
    reg_set_last(path);
    wchar_t title[MAX_PATH * 2 + 32];
    const wchar_t *base = wcsrchr(g_cat_path, L'\\');
    swprintf(title, MAX_PATH * 2 + 32, L"%ls - BRODALF", base ? base + 1 : g_cat_path);
    SetWindowTextW(g_main, title);
    set_busy(0);
    if (g_opened_by_recovery) {
        g_opened_by_recovery = 0;
        offer_new_passphrase(g_main);
    }
    update_drives_label();
    rebuild_tree();
    show_detail();
    count_ctx c = {0};
    bd_list_sources(g_cat, count_source, &c);
    if (recovered)
        log_append(L"Database recovered from a backup drive. Scanning your backup roots to see whether they are on this PC "
                   L"and what changed since that copy was saved.");
    else if (created)
        log_append(L"New database. Scanning your backup roots for the first time. Nothing is backed up until you press Back up.");
    else
        log_append(L"Database opened. Scanning your backup roots to find what changed since the last scan. Nothing is backed "
                   L"up until you press Back up.");
    job *d = new_job(JOB_DRIVES);
    if (d) {
        d->startup = 1;
        enqueue(d);
    }
    if (c.n > 0) {
        job *s = new_job(JOB_SCAN);
        if (s) {
            s->after_open = 1;
            enqueue(s);
        }
    } else {
        log_append(L"It has no backup roots yet: use Add backup root to choose what to back up.");
    }
}

/* Open the database at path in place of the one open now. 1 if it is open. */
static int try_open(const wchar_t *path)
{
    if (g_cat && _wcsicmp(path, g_cat_path) == 0) return 1;
    char *p = narrow(path);
    if (!p) return 0;
    bd_catalog *nc = NULL;
    bd_status s = open_maybe_encrypted(p, &nc);
    free(p);
    if (s == BD_ERR_PASSPHRASE) return 0; /* declined */
    if (s != BD_OK) {
        show_open_error(s, L"The database could not be opened.");
        return 0;
    }
    close_database();
    g_cat = nc;
    database_opened(path, 0, 0);
    return 1;
}

/* ---- A new database: the backup roots first, then the file ------------- */

static struct { char **paths; int n; } g_newdb;

static INT_PTR CALLBACK newdb_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG:
        EnableWindow(GetDlgItem(dlg, IDOK), FALSE);
        EnableWindow(GetDlgItem(dlg, IDC_NEWDB_REMOVE), FALSE);
        SetFocus(GetDlgItem(dlg, IDC_NEWDB_ADD));
        return FALSE;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_NEWDB_ADD: {
            char *folder = pick_folder(dlg, L"Choose a folder to back up (a backup root).");
            if (folder) {
                int dup = 0;
                for (int i = 0; i < g_newdb.n; i++)
                    if (_stricmp(g_newdb.paths[i], folder) == 0) dup = 1;
                char **grown = dup ? NULL : realloc(g_newdb.paths, sizeof(char *) * (size_t)(g_newdb.n + 1));
                if (grown) {
                    g_newdb.paths = grown;
                    g_newdb.paths[g_newdb.n++] = folder;
                    wchar_t *w = widen(folder);
                    SendDlgItemMessageW(dlg, IDC_NEWDB_LIST, LB_ADDSTRING, 0, (LPARAM)(w ? w : L""));
                    free(w);
                    folder = NULL;
                }
                free(folder);
            }
            EnableWindow(GetDlgItem(dlg, IDOK), g_newdb.n > 0);
            if (g_newdb.n > 0) SendMessageW(dlg, DM_SETDEFID, IDOK, 0);
            return TRUE;
        }
        case IDC_NEWDB_REMOVE: {
            HWND lb = GetDlgItem(dlg, IDC_NEWDB_LIST);
            for (int i = g_newdb.n - 1; i >= 0; i--) {
                if (SendMessageW(lb, LB_GETSEL, (WPARAM)i, 0) > 0) {
                    free(g_newdb.paths[i]);
                    memmove(&g_newdb.paths[i], &g_newdb.paths[i + 1], sizeof(char *) * (size_t)(g_newdb.n - i - 1));
                    g_newdb.n--;
                    SendMessageW(lb, LB_DELETESTRING, (WPARAM)i, 0);
                }
            }
            EnableWindow(GetDlgItem(dlg, IDOK), g_newdb.n > 0);
            EnableWindow(GetDlgItem(dlg, IDC_NEWDB_REMOVE), FALSE);
            return TRUE;
        }
        case IDC_NEWDB_LIST:
            if (HIWORD(wp) == LBN_SELCHANGE)
                EnableWindow(GetDlgItem(dlg, IDC_NEWDB_REMOVE), SendDlgItemMessageW(dlg, IDC_NEWDB_LIST, LB_GETSELCOUNT, 0, 0) > 0);
            return TRUE;
        case IDOK:
        case IDCANCEL:
            EndDialog(dlg, LOWORD(wp));
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static int new_database_ui(void)
{
    memset(&g_newdb, 0, sizeof(g_newdb));
    INT_PTR r = DialogBoxW(g_inst, MAKEINTRESOURCEW(IDD_NEWDB), g_main, newdb_proc);
    int ok = 0;
    wchar_t path[MAX_PATH * 2] = L"";
    if (r == IDOK && g_newdb.n > 0 && save_dialog(path, MAX_PATH * 2, L"Choose where to keep the new database file")) {
        char *p = narrow(path);
        bd_catalog *nc = NULL;
        bd_status s = p ? bd_catalog_create(p, &nc) : BD_ERR_NOMEM;
        free(p);
        if (s == BD_OK) {
            close_database();
            g_cat = nc;
            for (int i = 0; i < g_newdb.n; i++) {
                int64_t id;
                if (bd_source_add(g_cat, g_newdb.paths[i], &id) == BD_OK) continue;
                wchar_t *e = widen(bd_catalog_error(g_cat)), *w = widen(g_newdb.paths[i]), msg[2048];
                swprintf(msg, 2048, L"%ls was not added: %ls", w ? w : L"", e ? e : L"");
                MessageBoxW(g_main, msg, APP_NAME, MB_ICONWARNING);
                free(e);
                free(w);
            }
            bd_catalog_save(g_cat);
            database_opened(path, 1, 0);
            ok = 1;
        } else {
            show_open_error(s, L"The database could not be created.");
        }
    }
    for (int i = 0; i < g_newdb.n; i++) free(g_newdb.paths[i]);
    free(g_newdb.paths);
    memset(&g_newdb, 0, sizeof(g_newdb));
    return ok;
}

/* ---- Recover the database from a backup drive (a new PC) --------------- */

/* Every backup writes the newest revision of the database to the drive,
 * so the newest copy of each database is the highest revision on any
 * drive plugged in. */
#define MAX_FOUND 8
typedef struct { char id[64]; char *path; int64_t revision, saved_ms; } copy_found;
typedef struct { copy_found items[MAX_FOUND]; int n; } found_list;

static int found_copy_cb(void *ctx, const bd_catalog_copy_info *c)
{
    found_list *l = ctx;
    for (int i = 0; i < l->n; i++) {
        copy_found *f = &l->items[i];
        if (strcmp(f->id, c->catalog_id) != 0) continue;
        if (c->revision > f->revision || (c->revision == f->revision && c->saved_ms > f->saved_ms)) {
            char *p = xstrdup(c->path);
            if (p) {
                free(f->path);
                f->path = p;
                f->revision = c->revision;
                f->saved_ms = c->saved_ms;
            }
        }
        return 0;
    }
    if (l->n == MAX_FOUND || strlen(c->catalog_id) >= sizeof(l->items[0].id)) return 0;
    copy_found *f = &l->items[l->n];
    snprintf(f->id, sizeof(f->id), "%s", c->catalog_id);
    f->path = xstrdup(c->path);
    f->revision = c->revision;
    f->saved_ms = c->saved_ms;
    if (f->path) l->n++;
    return 0;
}

static void find_on_all_drives(found_list *l)
{
    DWORD mask = GetLogicalDrives();
    for (int i = 2; i < 26; i++) {
        if (!(mask & (1u << i))) continue;
        wchar_t wroot[4] = {(wchar_t)(L'A' + i), L':', L'\\', 0};
        char root[4] = {(char)('A' + i), ':', '\\', 0};
        UINT type = GetDriveTypeW(wroot);
        if (type == DRIVE_NO_ROOT_DIR || type == DRIVE_UNKNOWN || type == DRIVE_CDROM) continue;
        bd_find_catalog_copies(root, found_copy_cb, l);
    }
}

static void found_free(found_list *l)
{
    for (int i = 0; i < l->n; i++) free(l->items[i].path);
    l->n = 0;
}

/* Which database, when the drives hold copies of more than one. -1 cancelled. */
static int pick_found(const found_list *l)
{
    if (l->n == 1) return 0;
    TASKDIALOG_BUTTON b[MAX_FOUND];
    wchar_t text[MAX_FOUND][MAX_PATH * 2 + 160];
    for (int i = 0; i < l->n; i++) {
        wchar_t when[64], rev[64], *wp = widen(l->items[i].path);
        format_time_ms(l->items[i].saved_ms, when, 64);
        if (l->items[i].revision > 0) swprintf(rev, 64, L"revision %lld", (long long)l->items[i].revision);
        else swprintf(rev, 64, L"saved by BRODALF 0.6 or older");
        swprintf(text[i], MAX_PATH * 2 + 160, L"Database saved %ls (%ls)\n%ls", when, rev, wp ? wp : L"");
        free(wp);
        b[i].nButtonID = 400 + i;
        b[i].pszButtonText = text[i];
    }
    TASKDIALOGCONFIG tc;
    memset(&tc, 0, sizeof(tc));
    tc.cbSize = sizeof(tc);
    tc.hwndParent = g_main;
    tc.hInstance = g_inst;
    tc.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
    tc.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    tc.pszWindowTitle = APP_NAME;
    tc.pszMainInstruction = L"Your drives hold copies of more than one database. Which one is yours?";
    tc.pszContent = L"Each is the newest copy found of that database.";
    tc.cButtons = (UINT)l->n;
    tc.pButtons = b;
    int pressed = IDCANCEL;
    if (FAILED(TaskDialogIndirect(&tc, &pressed, NULL, NULL))) return -1;
    return pressed >= 400 && pressed < 400 + l->n ? pressed - 400 : -1;
}

/* Copy the newest copy of the database from a drive to this PC and open
 * it. The scan that follows finds which backup roots are here, and asks
 * about the others. */
static int recover_database_ui(void)
{
    found_list l;
    memset(&l, 0, sizeof(l));
    find_on_all_drives(&l);
    if (!l.n) {
        if (MessageBoxW(g_main, L"No copy of a BRODALF database was found on the drives that are plugged in.\n\nEvery backup puts "
                                L"the newest copy of the database on its drive. Plug in one of your backup drives, then press OK. "
                                L"If it is plugged in already, press OK to choose the folder that holds the copy.",
                        APP_NAME, MB_ICONINFORMATION | MB_OKCANCEL) != IDOK)
            return 0;
        find_on_all_drives(&l);
        if (!l.n) {
            char *dir = pick_folder(g_main, L"Choose the backup drive, or the folder on it that holds the copy of your database.");
            if (!dir) return 0;
            bd_find_catalog_copies(dir, found_copy_cb, &l);
            free(dir);
            if (!l.n) {
                MessageBoxW(g_main, L"No copy of a BRODALF database was found there.", APP_NAME, MB_ICONINFORMATION);
                return 0;
            }
        }
    }
    int pick = pick_found(&l), ok = 0;
    wchar_t dest[MAX_PATH * 2] = L"";
    if (pick >= 0 && save_dialog(dest, MAX_PATH * 2, L"Choose where to keep your database on this PC")) {
        char *d8 = narrow(dest);
        bd_catalog *nc = NULL;
        bd_status s = d8 ? bd_catalog_recover(l.items[pick].path, d8) : BD_ERR_NOMEM;
        if (s == BD_OK) {
            bd_applog("recovered the catalog from %s to %s", l.items[pick].path, d8);
            s = open_maybe_encrypted(d8, &nc);
            if (s != BD_OK) DeleteFileW(dest); /* only the copy just made; the drive's is untouched */
        }
        free(d8);
        if (s == BD_OK) {
            close_database();
            g_cat = nc;
            database_opened(dest, 0, 1);
            ok = 1;
        } else if (s != BD_ERR_PASSPHRASE) {
            show_open_error(s, L"The database could not be recovered.");
        }
    }
    found_free(&l);
    return ok;
}

/* choice: what the user picked in the File menu, or 0 to ask (when
 * BRODALF starts). Cancelling a step asks again only when BRODALF asked. */
static void open_database_ui(int choice)
{
    int asked = choice == 0;
    for (;;) {
        wchar_t last[MAX_PATH * 2];
        reg_get_last(last, MAX_PATH * 2);
        if (last[0] && (GetFileAttributesW(last) == INVALID_FILE_ATTRIBUTES || (g_cat && _wcsicmp(last, g_cat_path) == 0))) last[0] = 0;
        if (asked) choice = ask_database_choice(last);
        wchar_t path[MAX_PATH * 2] = L"";
        if (choice == CHOICE_LAST && last[0]) {
            if (try_open(last)) return;
        } else if (choice == CHOICE_OPEN) {
            if (open_dialog(path, MAX_PATH * 2) && try_open(path)) return;
        } else if (choice == CHOICE_NEW) {
            if (new_database_ui()) return;
        } else if (choice == CHOICE_RECOVER) {
            if (recover_database_ui()) return;
        } else {
            return; /* cancelled */
        }
        if (!asked) return;
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

    /* The only argument is a catalog to open. Anything that starts with
     * -- is not one, so BRODALF exits without opening a window. */
    if (argc >= 2 && wcsncmp(argv[1], L"--", 2) == 0) {
        bd_applog("unknown option on the command line; nothing done");
        LocalFree(argv);
        return 1;
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

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, APP_NAME, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, S(1180), S(760),
                                NULL, NULL, inst, NULL);
    if (!hwnd) { LocalFree(argv); return 1; }
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);
    update_drives_label();

    /* A database named on the command line (a double-click) is tried
     * first; otherwise BRODALF asks which to use. With none open the
     * window stays, with only File and Help working. */
    g_ui_depth++;
    if (!(argc > 1 && argv[1][0] && try_open(argv[1]))) open_database_ui(0);
    g_ui_depth--;
    LocalFree(argv);
    if (!g_cat) log_append(L"No database is open. Use File > New database, Open database or Recover the database from a backup drive.");
    start_next_job();

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    if (g_cat) {
        bd_catalog_save(g_cat);
        bd_catalog_close(g_cat);
    }
    CoUninitialize();
    return 0;
}
