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
#include <uxtheme.h>
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
#include <wctype.h>

#include "brodalf.h"
#include "resource.h"

#define COUNT_OF(a) ((int)(sizeof(a) / sizeof((a)[0])))

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
    ID_WARN_ICON,
    ID_WARN_TEXT,
    ID_BTN_READBACK,
    ID_HEADER,
    ID_WARNBAR,
    ID_JOBPANEL,
    ID_BTN_NEXT,
    ID_BTN_JOB1,
    ID_BTN_JOB2,
    ID_BTN_LOGTOGGLE,
    ID_WARN_BTN0, /* and the next 11: two buttons on each of six warnings */
    ID_TABS = 150,
    /* The controls on the tab pages. */
    ID_PAGE_FIRST = 200,
    ID_DB_ADD = ID_PAGE_FIRST,
    ID_DB_STAR,
    ID_DB_ENCRYPT,
    ID_DB_BITROT,
    ID_DB_RELOCATE,
    ID_DB_SKIP,
    ID_DB_DELETED,
    ID_DB_DAMAGED,
    ID_DB_IN_USE,
    ID_DB_LARGEST,
    ID_DB_EXPORT,
    ID_NEXT_FOR_LABEL,
    ID_NEXT_FOR,
    ID_NEXT_TEXT,
    ID_NEXT_BACKUP,
    ID_NEXT_ALL,
    ID_NEXT_CHANGED,
    ID_NEXT_RISK,
    ID_NEXT_REFRESH,
    ID_NEXT_LIST,
    ID_LOCAL_ADD,
    ID_LOCAL_BACKUP,
    ID_LOCAL_ALL,
    ID_LOCAL_CHECK,
    ID_LOCAL_VERIFY,
    ID_LOCAL_OPEN,
    ID_LOCAL_COMPRESS,
    ID_LOCAL_CHANGED,
    ID_LOCAL_EDIT,
    ID_LOCAL_LOST,
    ID_LOCAL_STATUS,
    ID_LOCAL_RISK,
    ID_LOCAL_PARALLEL,
    ID_LOCAL_LIST,
    ID_LOCAL_TEXT,
    ID_STAR_TEXT,
    ID_STAR_ADD,
    ID_STAR_REMOVE,
    ID_STAR_SHOW,
    ID_STAR_LIST,
    ID_PLACE_LABEL,
    ID_PLACE_ADD,
    ID_PLACE_BACKUP,
    ID_PLACE_LIMIT,
    ID_PLACE_OPEN,
    ID_PLACE_EDIT,
    ID_PLACE_LOST,
    ID_PLACE_LIST,
    ID_ENC_PASS,
    ID_ENC_LOCK,
    ID_ENC_KEY,
    ID_ENC_SHEET,
    ID_ENC_DRIVE,
    ID_ENC_CATALOG,
    ID_ENC_INTRO,
    ID_ENC_STATE,
    ID_ENC_LABEL,
    ID_ENC_ADD,
    ID_ENC_PICK,
    ID_ENC_REMOVE,
    ID_ENC_SHOW,
    ID_ENC_LIST,
    ID_REST_TEXT,
    ID_REST_SEL,
    ID_REST_ALL,
    ID_REST_DELETED,
    ID_REST_CONTINUE,
    ID_REST_PLAN,
    ID_REST_LABEL,
    ID_REST_OPEN,
    ID_REST_TABLE,
    ID_REST_LIST,
    ID_PAGE_LAST,
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
    ID_M_TAB_FIRST = 750, /* View: one per tab */
    ID_M_TAB_LAST = 755,
    ID_M_REFRESH,
    ID_M_BACKUP,
    ID_M_BACKUP_ALL,
    ID_M_CHECK,
    ID_M_ADD_PLACE,
    ID_M_STAR,
    ID_M_ENCRYPT,
    ID_M_THEME_FOLLOW, /* View > Appearance */
    ID_M_THEME_LIGHT,
    ID_M_THEME_DARK,
    ID_M_LANG_FOLLOW, /* View > Language */
    ID_M_LANG_FIRST,
    ID_M_LANG_LAST = ID_M_LANG_FIRST + 15,
    ID_M_FIND,     /* View: Ctrl+F, the search box */
    ID_M_NEXT_TAB, /* View: Ctrl+Tab */
    ID_M_PREV_TAB, /* View: Ctrl+Shift+Tab */
    ID_M_STOP,     /* Tools: Esc, the same as the Stop button */
    ID_M_LEGEND,   /* Help: what the list of files shows */
    /* Right-click menus (TrackPopupMenu with TPM_RETURNCMD). */
    ID_CTX_OPEN = 800,
    ID_CTX_RESTORE,
    ID_CTX_STOP_ROOT,
    ID_CTX_RESUME_ROOT,
    ID_CTX_ROOT_SKIP,
    ID_CTX_RELOCATE,
    ID_CTX_RESTORE_VERSION,
    ID_CTX_OPEN_VERSION,
    ID_CTX_STAR,
    ID_CTX_ENCRYPT,
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
    ID_MENU_PARALLEL,
    ID_MENU_UNIGNORE,
    ID_MENU_DRIVE_BASE = 1000, /* + media id, for the drive popup menus */
    ID_DRIVE_ACT_BASE = 20000, /* + media id * 8 + action, for the Local backups menu */
    ID_MENU_OTHER = 900,
    ID_MENU_EVERY = 901 /* Back up...: every plugged-in drive */
};

enum {
    WM_APP_LOG = WM_APP + 1,  /* lParam: malloc'd wide string */
    WM_APP_PROGRESS,          /* lParam: malloc'd prog_msg */
    WM_APP_DONE,              /* lParam: job* */
    WM_APP_KICK,              /* start the next job, once the command that asked for it is done */
    WM_APP_SETS               /* lParam: malloc'd sets_msg, the full copies read during a backup */
};

/* ---- Languages -----------------------------------------------------------
 *
 * The window app's text is written in English in this file. Everything the
 * person reads goes through T(), or TN() for a sentence with a count, which
 * gives it in the chosen language when gui/lang/<code>.po has it and in
 * English otherwise. The .po files are ordinary gettext files built into
 * brodalf.exe; tools/lang.py makes the template from this file and checks
 * every translation. A translation whose %-codes differ from the English
 * (in number, order or kind) is never used, since it would show the wrong
 * values: the English is shown instead. N_() only marks text in a table,
 * which is translated where it is shown. */

#define N_(s) (s)

typedef struct {
    const wchar_t *code, *name; /* the name in the language itself */
    int res;                    /* its .po file in brodalf.rc; 0 for English */
} lang_info;

static const lang_info g_langs[] = {
    {L"en", L"English", 0},
    {L"de", L"Deutsch", IDR_LANG_DE},
    {L"es", L"Espa\u00f1ol", IDR_LANG_ES},
};
#define N_LANGS ((int)(sizeof(g_langs) / sizeof(g_langs[0])))

typedef struct {
    wchar_t *id;     /* the English */
    wchar_t *str[2]; /* the translation; for a count, [0] for one and [1] for more */
} lang_entry;

static lang_entry *g_tr;  /* open addressing, a power of two long */
static size_t g_tr_cap;
static int g_plural_gt1;   /* "one" also for 0, as in French */
static int g_lang;         /* the language in use, in g_langs */
static int g_lang_choice;  /* what View > Language says: -1 like Windows */

static size_t lang_hash(const wchar_t *s)
{
    size_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (size_t)*s) * 16777619u;
    return h;
}

static const lang_entry *lang_find(const wchar_t *s)
{
    if (!g_tr || !s) return NULL;
    for (size_t i = lang_hash(s) & (g_tr_cap - 1);; i = (i + 1) & (g_tr_cap - 1)) {
        if (!g_tr[i].id) return NULL;
        if (wcscmp(g_tr[i].id, s) == 0) return &g_tr[i];
    }
}

/* Text in the chosen language. */
static const wchar_t *T(const wchar_t *s)
{
    const lang_entry *e = lang_find(s);
    return e ? e->str[0] : s;
}

/* A sentence with a count: one is the English for 1, many for any other n. */
static const wchar_t *TN(long long n, const wchar_t *one, const wchar_t *many)
{
    const lang_entry *e = lang_find(one);
    if (e && e->str[1]) return e->str[(g_plural_gt1 ? n > 1 : n != 1) ? 1 : 0];
    return n == 1 ? one : many;
}

/* The %-codes of a format, in order, as one string, to compare two. 0 when
 * they do not fit in out. */
static int lang_codes(const wchar_t *s, wchar_t *out, size_t n)
{
    size_t o = 0;
    for (; *s; s++) {
        if (*s != L'%') continue;
        const wchar_t *start = s++;
        while (*s && wcschr(L"-+ #0", *s)) s++;
        while (*s && (iswdigit(*s) || *s == L'*')) s++;
        if (*s == L'.') {
            s++;
            while (*s && (iswdigit(*s) || *s == L'*')) s++;
        }
        while (*s && wcschr(L"hlLIjztw0123456789", *s)) s++;
        if (!*s) s--; /* a % at the end */
        size_t len = (size_t)(s - start) + 1;
        if (o + len + 2 > n) return 0;
        memcpy(out + o, start, len * sizeof(wchar_t));
        o += len;
        out[o++] = L'|';
    }
    out[o] = 0;
    return 1;
}

static int lang_same_codes(const wchar_t *a, const wchar_t *b)
{
    wchar_t ca[256], cb[256];
    return lang_codes(a, ca, 256) && lang_codes(b, cb, 256) && wcscmp(ca, cb) == 0;
}

static wchar_t *lang_widen(const char *s, size_t len)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, (int)len, NULL, 0);
    wchar_t *w = malloc(sizeof(wchar_t) * (size_t)(n + 1));
    if (!w) return NULL;
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s, (int)len, w, n);
    w[n > 0 ? n : 0] = 0;
    return w;
}

static void lang_add(wchar_t *id, wchar_t *one, wchar_t *more)
{
    if (!g_tr) {
        g_tr_cap = 4096;
        g_tr = calloc(g_tr_cap, sizeof(*g_tr));
        if (!g_tr) return;
    }
    static size_t used;
    if ((used + 1) * 4 > g_tr_cap * 3) return; /* far more than BRODALF has */
    size_t i = lang_hash(id) & (g_tr_cap - 1);
    while (g_tr[i].id) {
        if (wcscmp(g_tr[i].id, id) == 0) return; /* the first one counts */
        i = (i + 1) & (g_tr_cap - 1);
    }
    g_tr[i].id = id;
    g_tr[i].str[0] = one;
    g_tr[i].str[1] = more;
    used++;
}

/* One string of a .po file, from its opening quote; the end of the line
 * is the end of the text. Appended to buf. */
typedef struct { char *p; size_t len, cap; } lang_buf;

static void lang_put(lang_buf *b, char c)
{
    if (b->len + 2 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 256;
        char *p = realloc(b->p, cap);
        if (!p) return;
        b->p = p;
        b->cap = cap;
    }
    b->p[b->len++] = c;
    b->p[b->len] = 0;
}

static void lang_quoted(lang_buf *b, const char *s, const char *end)
{
    if (s >= end || *s != '"') return;
    for (s++; s < end && *s != '"'; s++) {
        if (*s != '\\' || s + 1 >= end) {
            lang_put(b, *s);
            continue;
        }
        char c = *++s;
        lang_put(b, c == 'n' ? '\n' : c == 't' ? '\t' : c == 'r' ? '\r' : c);
    }
}

/* What one entry of the file says; used once it is complete. */
typedef struct {
    lang_buf id, plural, str[2];
    int fuzzy, has_ctx, seen_str;
} lang_po_entry;

static void lang_po_done(lang_po_entry *e)
{
    if (e->id.len == 0 && e->str[0].len) {
        /* The header: only the plural rule matters here. */
        const char *rule = strstr(e->str[0].p, "plural=");
        if (rule) {
            char compact[32];
            size_t o = 0;
            for (rule += 7; *rule && *rule != ';' && *rule != '\n' && o + 1 < sizeof(compact); rule++)
                if (*rule != ' ' && *rule != '(' && *rule != ')') compact[o++] = *rule;
            compact[o] = 0;
            g_plural_gt1 = strcmp(compact, "n>1") == 0;
        }
    } else if (e->id.len && e->str[0].len && !e->fuzzy && !e->has_ctx && (!e->plural.len || e->str[1].len)) {
        wchar_t *id = lang_widen(e->id.p, e->id.len), *one = lang_widen(e->str[0].p, e->str[0].len);
        wchar_t *many = e->plural.len ? lang_widen(e->plural.p, e->plural.len) : NULL;
        wchar_t *more = e->plural.len ? lang_widen(e->str[1].p, e->str[1].len) : NULL;
        int good = id && one && (!e->plural.len || (many && more)) && lang_same_codes(id, one) &&
                   (!e->plural.len || lang_same_codes(many, more));
        free(many);
        if (good) lang_add(id, one, more);
        else {
            free(id);
            free(one);
            free(more);
        }
    }
    e->id.len = e->plural.len = e->str[0].len = e->str[1].len = 0;
    e->fuzzy = e->has_ctx = e->seen_str = 0;
}

static int lang_word(const char *s, size_t k, const char *word)
{
    return strlen(word) == k && memcmp(s, word, k) == 0;
}

static void lang_parse(const char *p, size_t size)
{
    const char *end = p + size;
    lang_po_entry e;
    memset(&e, 0, sizeof(e));
    lang_buf *field = NULL;
    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        if (!eol) eol = end;
        const char *s = p, *line_end = eol;
        while (s < eol && (*s == ' ' || *s == '\t' || *s == '\r')) s++;
        while (line_end > s && (line_end[-1] == '\r' || line_end[-1] == ' ' || line_end[-1] == '\t')) line_end--;
        p = eol + 1;
        if (s >= line_end) continue;
        if (*s == '"') {
            if (field) lang_quoted(field, s, line_end);
            continue;
        }
        if (*s == '#') {
            /* Comments come before the entry they are about. */
            if (e.seen_str) lang_po_done(&e);
            field = NULL;
            if (line_end - s > 1 && s[1] == ',')
                for (const char *f = s; f + 5 <= line_end; f++)
                    if (memcmp(f, "fuzzy", 5) == 0) e.fuzzy = 1;
            continue;
        }
        size_t k = 0;
        while (s + k < line_end && s[k] != ' ' && s[k] != '\t' && s[k] != '"') k++;
        if ((lang_word(s, k, "msgid") || lang_word(s, k, "msgctxt")) && e.seen_str) lang_po_done(&e);
        field = NULL;
        if (lang_word(s, k, "msgctxt")) e.has_ctx = 1;
        else if (lang_word(s, k, "msgid")) field = &e.id;
        else if (lang_word(s, k, "msgid_plural")) field = &e.plural;
        else if (lang_word(s, k, "msgstr") || lang_word(s, k, "msgstr[0]")) field = &e.str[0];
        else if (lang_word(s, k, "msgstr[1]")) field = &e.str[1];
        if (k >= 6 && memcmp(s, "msgstr", 6) == 0) e.seen_str = 1;
        if (field) {
            const char *q = s + k;
            while (q < line_end && (*q == ' ' || *q == '\t')) q++;
            lang_quoted(field, q, line_end);
        }
    }
    lang_po_done(&e);
    free(e.id.p);
    free(e.plural.p);
    free(e.str[0].p);
    free(e.str[1].p);
}

/* The language Windows shows its own text in, if BRODALF has it. */
static int lang_windows(void)
{
    wchar_t name[LOCALE_NAME_MAX_LENGTH];
    if (LCIDToLocaleName(MAKELCID(GetUserDefaultUILanguage(), SORT_DEFAULT), name, LOCALE_NAME_MAX_LENGTH, 0) <= 0) return 0;
    for (int i = 0; i < N_LANGS; i++) {
        size_t len = wcslen(g_langs[i].code);
        if (_wcsnicmp(name, g_langs[i].code, len) == 0 && (name[len] == 0 || name[len] == L'-')) return i;
    }
    return 0;
}

/* At start, before any window: View > Language's choice, or Windows'. */
static void lang_load(void)
{
    wchar_t code[16];
    DWORD size = sizeof(code);
    g_lang_choice = -1;
    if (RegGetValueW(HKEY_CURRENT_USER, REG_KEY, L"Language", RRF_RT_REG_SZ, NULL, code, &size) == ERROR_SUCCESS) {
        for (int i = 0; i < N_LANGS; i++)
            if (_wcsicmp(code, g_langs[i].code) == 0) g_lang_choice = i;
    }
    g_lang = g_lang_choice >= 0 ? g_lang_choice : lang_windows();
    if (!g_langs[g_lang].res) return;
    HRSRC r = FindResourceW(NULL, MAKEINTRESOURCEW(g_langs[g_lang].res), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    HGLOBAL h = r ? LoadResource(NULL, r) : NULL;
    const char *data = h ? LockResource(h) : NULL;
    if (data) lang_parse(data, SizeofResource(NULL, r));
}

/* A dialog from brodalf.rc: its title and the text of its buttons, check
 * boxes and labels, in the chosen language. */
static BOOL CALLBACK lang_child(HWND w, LPARAM lp)
{
    (void)lp;
    wchar_t cls[16], text[1024];
    GetClassNameW(w, cls, 16);
    if (_wcsicmp(cls, L"Button") != 0 && _wcsicmp(cls, L"Static") != 0) return TRUE;
    if (GetWindowTextW(w, text, 1024) > 0) {
        const wchar_t *t = T(text);
        if (t != text) SetWindowTextW(w, t);
    }
    return TRUE;
}

static void lang_dialog(HWND dlg)
{
    if (!g_tr) return;
    wchar_t text[256];
    if (GetWindowTextW(dlg, text, 256) > 0) {
        const wchar_t *t = T(text);
        if (t != text) SetWindowTextW(dlg, t);
    }
    EnumChildWindows(dlg, lang_child, 0);
}

/* A file dialog's filter from pairs of a name (translated) and a pattern. */
static const wchar_t *file_filter(wchar_t *out, size_t n, int count, ...)
{
    va_list ap;
    size_t o = 0;
    va_start(ap, count);
    for (int i = 0; i < count; i++) {
        const wchar_t *s = va_arg(ap, const wchar_t *);
        if (i % 2 == 0) s = T(s);
        size_t len = wcslen(s);
        if (o + len + 2 >= n) break;
        memcpy(out + o, s, (len + 1) * sizeof(wchar_t));
        o += len + 1;
    }
    va_end(ap);
    out[o] = 0;
    return out;
}

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

/* The locale for numbers and dates: the user's own when it is in the
 * language shown (English in Britain: en-GB), else that language's. */
static const wchar_t *fmt_locale(void)
{
    static wchar_t user[LOCALE_NAME_MAX_LENGTH];
    static int have = -1;
    if (have < 0) have = GetUserDefaultLocaleName(user, LOCALE_NAME_MAX_LENGTH) > 0;
    const wchar_t *ui = g_langs[g_lang].code;
    size_t n = wcslen(ui);
    if (have && _wcsnicmp(user, ui, n) == 0 && (user[n] == L'-' || user[n] == 0)) return user;
    return ui;
}

/* "1.0 GB", not "1022.9 MB"; "315,9 MB" in German and Spanish. */
static void format_bytes(int64_t b, wchar_t *out, size_t n)
{
    const wchar_t *units[] = {T(L"bytes"), T(L"KB"), T(L"MB"), T(L"GB"), T(L"TB")};
    double v = (double)b;
    int u = 0;
    while (v >= 1000.0 && u < 4) { v /= 1024.0; u++; }
    if (u == 0) {
        swprintf(out, n, T(L"%lld bytes"), (long long)b);
        return;
    }
    swprintf(out, n, L"%.1f %ls", v, units[u]);
    wchar_t dec[8];
    if (GetLocaleInfoEx(fmt_locale(), LOCALE_SDECIMAL, dec, 8) == 2 && dec[0] != L'.') {
        wchar_t *dot = wcschr(out, L'.');
        if (dot) *dot = dec[0];
    }
}

/* A date and time as the language shown writes them: 26.11.2026 01:21. */
static void format_time_ms(int64_t ms, wchar_t *out, size_t n)
{
    if (ms <= 0) { swprintf(out, n, T(L"never")); return; }
    ULONGLONG t = (ULONGLONG)ms * 10000ULL + 116444736000000000ULL;
    FILETIME ft = {(DWORD)t, (DWORD)(t >> 32)}, local;
    SYSTEMTIME st;
    FileTimeToLocalFileTime(&ft, &local);
    FileTimeToSystemTime(&local, &st);
    wchar_t d[96], tm[64];
    if (!GetDateFormatEx(fmt_locale(), DATE_SHORTDATE, &st, NULL, d, 96, NULL)) swprintf(d, 96, L"%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
    if (!GetTimeFormatEx(fmt_locale(), TIME_NOSECONDS, &st, NULL, tm, 64)) swprintf(tm, 64, L"%02d:%02d", st.wHour, st.wMinute);
    swprintf(out, n, L"%ls %ls", d, tm);
}

/* ---- Globals ------------------------------------------------------------ */

static HINSTANCE g_inst;
static HWND g_main, g_tree, g_list, g_log, g_status, g_detail, g_search, g_results;
/* The bar for the file being copied, and one bar per full copy the user
 * wants (the target's copies): how far each set of copies has got. */
#define MAX_SET_BARS 6
static HMENU g_menu_local;
/* The tabs: each one a page (a child window over the tab control) that
 * holds that tab's controls. */
enum { TAB_DB, TAB_NEXT, TAB_LOCAL, TAB_STARRED, TAB_ENCRYPT, TAB_RESTORE, N_TABS };
static HWND g_tabs, g_page[N_TABS];
static int g_tab;
static HWND g_ctl[ID_PAGE_LAST - ID_PAGE_FIRST];
static char g_ctl_btn[ID_PAGE_LAST - ID_PAGE_FIRST]; /* a button, check box or list: works only with a database and no job */
#define CTL(id) g_ctl[(id) - ID_PAGE_FIRST]
static HMENU g_menu_view, g_menu_lang;
static const wchar_t *g_tab_names[N_TABS] = {N_(L"Primary Database"), N_(L"Next backup"), N_(L"Local backups"),
                                             N_(L"Starred files and extra places"), N_(L"Encryption"), N_(L"Restore")};
static int g_label_encrypt_default; /* the new-drive dialog starts with Encrypt ticked */
#define N_BUTTONS 8
static HWND g_buttons[N_BUTTONS];
/* With no database open, the only buttons that work: Open and New. */
static HWND g_nodb_open, g_nodb_new;
static HFONT g_font, g_font_italic, g_font_strike, g_font_bold;
static int g_dpi = 96;
static bd_catalog *g_cat;
static wchar_t g_cat_path[MAX_PATH * 2];
static volatile LONG g_busy;
/* Stop pressed: the running job is asked to stop, and closes the window
 * once it has when g_close_after is set. */
static HWND g_stop;
/* After a backup, beside the bar for each file: reads back what it just
 * wrote, when the person presses it (read_back_new). Nothing starts it by
 * itself; the next job, or closing the database, takes it away. */
static HWND g_readback;
static int64_t g_readback_ids[16]; /* the drives the backup wrote copies to */
static int g_n_readback;
static wchar_t g_readback_tip[2400];
/* Under the drives line: what needs looking at before a backup (many files
 * changed at once, too little room, a check due, a drive's health), with
 * a warning icon; shown only when there is something. A tooltip gives the
 * whole text of it, of the drives line and of Stop. */
static HWND g_tip;
static int64_t g_warn_changed; /* the first drive where many files changed at once: a click lists them */
static wchar_t g_tip_text[4096];
static volatile LONG g_stopping;
/* Windows is shutting down or signing out (WM_ENDSESSION): no job starts
 * any more, and the one that was running is waited for. */
static volatile LONG g_session_end;
static HANDLE g_worker_thread; /* the running (or last) job's thread */
static int g_close_after, g_running_kind;
/* >0 while the UI thread is in the middle of a command or of what comes
 * after a job (its end-of-job prompts), perhaps waiting in a message box:
 * the catalog is the UI's then, so no job may start. Jobs asked for
 * meanwhile, such as a drive being plugged in, wait in the queue and start
 * when the UI is done. */
static int g_ui_depth;
/* The keyboard: the shortcuts in the menus, the control that had the
 * focus when the window was last active, and the one that had it when a
 * job started (it gets it back when the job ends). */
static HACCEL g_accel;
static HWND g_focus, g_focus_job;

static int S(int px) { return MulDiv(px, g_dpi, 96); }

/* The window follows the DPI of the monitor it is on (the manifest asks
 * for per-monitor v2). Both are in Windows 10 1607 and later; before
 * that the window is at the system DPI, which g_dpi starts at. */
typedef UINT(WINAPI *dpi_for_window_fn)(HWND);
typedef BOOL(WINAPI *spi_for_dpi_fn)(UINT, UINT, PVOID, UINT, UINT);
static dpi_for_window_fn p_dpi_for_window;
static spi_for_dpi_fn p_spi_for_dpi;
static int g_sys_dpi = 96; /* the DPI SystemParametersInfo's sizes are for */

static void dpi_load(void)
{
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (!u) return;
    p_dpi_for_window = (dpi_for_window_fn)(void (*)(void))GetProcAddress(u, "GetDpiForWindow");
    p_spi_for_dpi = (spi_for_dpi_fn)(void (*)(void))GetProcAddress(u, "SystemParametersInfoForDpi");
}

static int window_dpi(HWND h)
{
    UINT d = p_dpi_for_window && h ? p_dpi_for_window(h) : 0;
    return d ? (int)d : g_dpi;
}

static void make_fonts(void);

static void page_buttons_state(void);
static void refresh_pages(void);
static void show_tab(int i);
static int last_tab(void);
static void layout_pages(void);
static void create_pages(HWND hwnd);
static void local_metrics(void);
static void draw_page_button(const DRAWITEMSTRUCT *di);
static int next_text_lines(void);
static int font_h(HFONT f);
static void lv_select(HWND lv, int row);
static wchar_t g_next_label[128];         /* Next backup: the drive chosen, by its name */
static int g_next_copy_col = -1, g_next_copy_n; /* where its Copy columns are */
static int g_next_text_lines;
static void local_fit_cols(void);
static int local_list_need(void);
static int enc_state_need(void);
/* The Local backups table's columns. */
static const wchar_t *const g_local_cols[6] = {N_(L"Drive"), N_(L"State"), N_(L"Copies on it"),
                                               N_(L"Space"), N_(L"Health"), N_(L"Last checked")};

static void register_page_class(void);
static void cmd_backup_all(int offer_scan);
static void cmd_backup_at(HWND anchor);
static void cmd_add_place(void);
static void toggle_star_selected(void);
static void toggle_encrypt_selected(void);
static void load_marks(void);
static int good_description(const wchar_t *s);
static void media_name(int64_t id, wchar_t *out, size_t cap);
static void job_dest_label(int64_t id, wchar_t *out, size_t cap, int *secondary);
static void post_live_sets(void);

/* What a running backup is writing to, for its progress line: "Backing up
 * to Blue (1 of 2)". Set by the worker before each drive, read by
 * job_progress; empty when not backing up. */
static wchar_t g_job_dest[400];

/* ---- The keyboard -------------------------------------------------------- *
 *
 * The message loop gives the main window the dialog keys (IsDialogMessage:
 * Enter, Esc, the & letters, the arrows between buttons) and the shortcuts
 * in the menus (an accelerator table). Tab and Shift+Tab are done here, in
 * the order the window reads: the buttons along the top, the tabs, what is
 * on the tab shown, then Stop and the log. */

static int usable(HWND h)
{
    return h && IsWindow(h) && IsWindowVisible(h) && IsWindowEnabled(h);
}

/* Give a control the focus, or keep it for when the window is active again. */
static void put_focus(HWND h)
{
    if (!usable(h)) return;
    if (GetActiveWindow() == g_main) SetFocus(h);
    else g_focus = h;
}

/* The same, when the focus was lost: on a control that was hidden or
 * turned off, or nowhere. */
static void keep_focus(HWND h)
{
    if (GetActiveWindow() == g_main) {
        HWND now = GetFocus();
        if (!usable(now) || !IsChild(g_main, now)) put_focus(h);
    } else if (!usable(g_focus)) {
        g_focus = usable(h) ? h : NULL;
    }
}

/* The focus moved by the keyboard: show where it is, and select the text
 * of a box you type in, as dialogs do. */
static void focus_control(HWND h)
{
    if (!usable(h)) return;
    SendMessageW(g_main, WM_CHANGEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS), 0);
    SetFocus(h);
    if (SendMessageW(h, WM_GETDLGCODE, 0, 0) & DLGC_HASSETSEL) SendMessageW(h, EM_SETSEL, 0, -1);
}

static void tab_stops_of(HWND parent, HWND *out, int *n, int cap)
{
    for (HWND c = GetWindow(parent, GW_CHILD); c && *n < cap; c = GetWindow(c, GW_HWNDNEXT))
        if ((GetWindowLongW(c, GWL_STYLE) & WS_TABSTOP) && usable(c)) out[(*n)++] = c;
}

static void tab_move(int back)
{
    HWND order[160];
    int n = 0;
    for (HWND c = GetWindow(g_main, GW_CHILD); c && n < 160; c = GetWindow(c, GW_HWNDNEXT)) {
        if (c == g_tabs) continue;
        int page = -1;
        for (int i = 0; i < N_TABS; i++)
            if (c == g_page[i]) page = i;
        if (page >= 0) {
            /* The tabs come just before the page they show. */
            if (page == g_tab) {
                if (usable(g_tabs)) order[n++] = g_tabs;
                tab_stops_of(c, order, &n, 160);
            }
            continue;
        }
        if ((GetWindowLongW(c, GWL_STYLE) & WS_TABSTOP) && usable(c)) order[n++] = c;
    }
    if (!n) return;
    HWND f = GetFocus();
    int at = -1;
    for (int i = 0; i < n && at < 0; i++)
        if (f && (order[i] == f || IsChild(order[i], f))) at = i;
    int next = back ? (at <= 0 ? n - 1 : at - 1) : (at + 1) % n;
    focus_control(order[next]);
}

/* Tab, unless the control with the focus uses it itself. */
static int tab_key(const MSG *m)
{
    if (m->message != WM_KEYDOWN || m->wParam != VK_TAB || GetKeyState(VK_CONTROL) < 0 || GetKeyState(VK_MENU) < 0) return 0;
    LRESULT code = SendMessageW(m->hwnd, WM_GETDLGCODE, m->wParam, (LPARAM)m);
    if (code & (DLGC_WANTTAB | DLGC_WANTALLKEYS | DLGC_WANTMESSAGE)) return 0;
    tab_move(GetKeyState(VK_SHIFT) < 0);
    return 1;
}

/* The details, the log and the other read-only text: the arrows, Page Up
 * and Page Down scroll them and Ctrl+A selects all of it, but Tab, Enter
 * and Esc are the window's, and the text is not selected when they get
 * the focus. */
static LRESULT CALLBACK read_only_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
    (void)id;
    (void)ref;
    if (msg == WM_GETDLGCODE)
        return DefSubclassProc(h, msg, wp, lp) & ~(LRESULT)(DLGC_WANTALLKEYS | DLGC_WANTTAB | DLGC_HASSETSEL);
    if (msg == WM_KEYDOWN && wp == 'A' && GetKeyState(VK_CONTROL) < 0) {
        SendMessageW(h, EM_SETSEL, 0, -1);
        return 0;
    }
    if (msg == WM_CHAR && wp == 1) return 0; /* Ctrl+A, done above */
    return DefSubclassProc(h, msg, wp, lp);
}

static void read_only_keys(HWND h)
{
    SetWindowSubclass(h, read_only_proc, 1, 0);
}

static HACCEL make_accels(void)
{
    static const ACCEL keys[] = {
        {FVIRTKEY | FCONTROL, 'N', ID_M_NEW_DB},
        {FVIRTKEY | FCONTROL, 'O', ID_M_OPEN_DB},
        {FVIRTKEY | FCONTROL, 'P', ID_M_SHEET},
        {FVIRTKEY | FCONTROL, 'F', ID_M_FIND},
        {FVIRTKEY | FCONTROL, 'B', ID_M_BACKUP},
        {FVIRTKEY, VK_F5, ID_M_SCAN},
        {FVIRTKEY | FCONTROL, VK_TAB, ID_M_NEXT_TAB},
        {FVIRTKEY | FCONTROL | FSHIFT, VK_TAB, ID_M_PREV_TAB},
        {FVIRTKEY | FCONTROL, VK_NEXT, ID_M_NEXT_TAB},
        {FVIRTKEY | FCONTROL, VK_PRIOR, ID_M_PREV_TAB},
        {FVIRTKEY | FCONTROL, '1', ID_M_TAB_FIRST + 0},
        {FVIRTKEY | FCONTROL, '2', ID_M_TAB_FIRST + 1},
        {FVIRTKEY | FCONTROL, '3', ID_M_TAB_FIRST + 2},
        {FVIRTKEY | FCONTROL, '4', ID_M_TAB_FIRST + 3},
        {FVIRTKEY | FCONTROL, '5', ID_M_TAB_FIRST + 4},
        {FVIRTKEY | FCONTROL, '6', ID_M_TAB_FIRST + 5},
    };
    return CreateAcceleratorTableW((LPACCEL)keys, (int)(sizeof(keys) / sizeof(keys[0])));
}

/* "Ctrl+Shift+Tab", in the words of the keyboard the language uses. */
static void key_text(wchar_t *out, size_t cap, int ctrl, int shift, const wchar_t *key)
{
    swprintf(out, cap, L"%ls%ls%ls%ls%ls", ctrl ? T(L"Ctrl") : L"", ctrl ? L"+" : L"", shift ? T(L"Shift") : L"", shift ? L"+" : L"",
             key);
}

/* A menu item with its shortcut at the right. */
static void menu_keyed(HMENU m, UINT id, const wchar_t *label, int ctrl, int shift, const wchar_t *key)
{
    wchar_t k[64], text[400];
    key_text(k, 64, ctrl, shift, key);
    swprintf(text, 400, L"%ls\t%ls", label, k);
    AppendMenuW(m, MF_STRING, id, text);
}

/* ---- Light and dark ---------------------------------------------------------- */

/* BRODALF follows Windows' "Choose your app mode" (Settings > Personalization
 * > Colors) unless the person picks Light or Dark under View > Appearance;
 * that choice is kept for this user in the registry. Dark covers the main
 * window, its tabs, lists, menus and BRODALF's own dialogs. Message boxes
 * and the question dialogs Windows draws itself stay light: Windows has no
 * dark version of them. High contrast always uses the system's colours.
 *
 * Windows has no documented way to make a classic desktop program dark:
 * the menus and scroll bars use what File Explorer uses (uxtheme.dll's
 * unnamed functions, looked up by number and only on Windows 10 1809 or
 * later), the rest is painted here. Where any of it is missing the window
 * simply stays light. */
enum { THEME_FOLLOW = 0, THEME_LIGHT = 1, THEME_DARK = 2 };
static int g_theme_choice; /* THEME_* */
static HMENU g_menu_look; /* View > Appearance */
static int g_dark;         /* dark now */
static HBRUSH g_br_bg, g_br_field, g_br_hot, g_br_border;
#define DARK_BG RGB(32, 32, 32)
#define DARK_FIELD RGB(25, 25, 25)
#define DARK_HOT RGB(62, 62, 62)
#define DARK_BORDER RGB(80, 80, 80)
#define DARK_TEXT RGB(230, 230, 230)
#define DARK_GRAY RGB(150, 150, 150)

typedef BOOL(WINAPI *allow_dark_window_fn)(HWND, BOOL);
typedef int(WINAPI *preferred_mode_fn)(int);
typedef void(WINAPI *uxtheme_void_fn)(void);
typedef HRESULT(WINAPI *dwm_set_fn)(HWND, DWORD, LPCVOID, DWORD);
static allow_dark_window_fn p_allow_dark_window;
static preferred_mode_fn p_preferred_mode; /* SetPreferredAppMode, or AllowDarkModeForApp on 1809 */
static uxtheme_void_fn p_refresh_policy, p_flush_menus;
static dwm_set_fn p_dwm_set;
static DWORD g_win_build;

/* Windows' high contrast is on: the window uses its colours only. */
static int g_hc;

/* The grey the tree and lists use for names with no copy at hand,
 * readable on either background. Each state also has its words in a chip
 * beside the name and in its tooltip (state_suffix), so the colour is
 * never all there is to it. In high contrast it is the theme's grey. */
static COLORREF theme_gray(void) { return g_hc ? GetSysColor(COLOR_GRAYTEXT) : g_dark ? DARK_GRAY : GetSysColor(COLOR_GRAYTEXT); }

static int high_contrast(void)
{
    HIGHCONTRASTW hc = {sizeof(hc), 0, NULL};
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON);
}

static int windows_apps_dark(void)
{
    DWORD v = 1, n = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme",
                     RRF_RT_REG_DWORD, NULL, &v, &n) != ERROR_SUCCESS)
        return 0;
    return v == 0;
}

/* What the choice comes to now. Dark needs Windows 10 1809 or later. */
static int theme_want_dark(void)
{
    if (high_contrast() || g_win_build < 17763 || !p_allow_dark_window) return 0;
    if (g_theme_choice == THEME_DARK) return 1;
    if (g_theme_choice == THEME_LIGHT) return 0;
    return windows_apps_dark();
}

static void theme_load(void)
{
    typedef void(WINAPI * nt_version_fn)(LPDWORD, LPDWORD, LPDWORD);
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    nt_version_fn ver = nt ? (nt_version_fn)(void (*)(void))GetProcAddress(nt, "RtlGetNtVersionNumbers") : NULL;
    DWORD major = 0, minor = 0, build = 0;
    if (ver) ver(&major, &minor, &build);
    g_win_build = major >= 10 ? build & 0x0FFFFFFF : 0;
    HMODULE ux = g_win_build >= 17763 ? LoadLibraryExW(L"uxtheme.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32) : NULL;
    if (ux) {
        p_refresh_policy = (uxtheme_void_fn)(void (*)(void))GetProcAddress(ux, MAKEINTRESOURCEA(104));
        p_allow_dark_window = (allow_dark_window_fn)(void (*)(void))GetProcAddress(ux, MAKEINTRESOURCEA(133));
        p_preferred_mode = (preferred_mode_fn)(void (*)(void))GetProcAddress(ux, MAKEINTRESOURCEA(135));
        p_flush_menus = (uxtheme_void_fn)(void (*)(void))GetProcAddress(ux, MAKEINTRESOURCEA(136));
    }
    HMODULE dwm = LoadLibraryExW(L"dwmapi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (dwm) p_dwm_set = (dwm_set_fn)(void (*)(void))GetProcAddress(dwm, "DwmSetWindowAttribute");
    DWORD v = 0, n = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, REG_KEY, L"Appearance", RRF_RT_REG_DWORD, NULL, &v, &n) == ERROR_SUCCESS && v <= THEME_DARK)
        g_theme_choice = (int)v;
    g_br_bg = CreateSolidBrush(DARK_BG);
    g_br_field = CreateSolidBrush(DARK_FIELD);
    g_br_hot = CreateSolidBrush(DARK_HOT);
    g_br_border = CreateSolidBrush(DARK_BORDER);
}

/* Menus (the popups) follow the app's mode. */
static void theme_app_mode(void)
{
    if (!p_preferred_mode) return;
    /* 1809 has AllowDarkModeForApp(BOOL); later builds SetPreferredAppMode
     * (2 force dark, 3 force light). */
    if (g_win_build < 18362) p_preferred_mode(g_dark);
    else p_preferred_mode(g_dark ? 2 : 3);
    if (p_refresh_policy) p_refresh_policy();
    if (p_flush_menus) p_flush_menus();
}

static void theme_title_bar(HWND h)
{
    if (!p_dwm_set) return;
    BOOL on = g_dark;
    /* DWMWA_USE_IMMERSIVE_DARK_MODE: 20 from Windows 10 20H1, 19 before. */
    p_dwm_set(h, g_win_build >= 18985 ? 20 : 19, &on, sizeof(on));
}

static LRESULT CALLBACK dark_tab_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref);
static LRESULT CALLBACK dark_status_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref);

/* A list's column headers: the dark header theme keeps black text, so
 * dark mode gives it light text as each header item is drawn. */
static LRESULT CALLBACK dark_lv_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
    (void)id;
    (void)ref;
    if (msg == WM_NOTIFY && g_dark) {
        NMHDR *nh = (NMHDR *)lp;
        if (nh->code == NM_CUSTOMDRAW && nh->hwndFrom == ListView_GetHeader(h)) {
            NMCUSTOMDRAW *cd = (NMCUSTOMDRAW *)lp;
            if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
                SetTextColor(cd->hdc, DARK_TEXT);
                return CDRF_DODEFAULT;
            }
        }
    }
    return DefSubclassProc(h, msg, wp, lp);
}

static void tree_metrics(void);

/* A text box's frame: the dark theme draws it white, so dark mode draws
 * it again in the border colour. */
static LRESULT CALLBACK dark_edit_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
    (void)id;
    (void)ref;
    if (msg == WM_NCPAINT && g_dark && !g_hc) {
        LRESULT r = DefSubclassProc(h, msg, wp, lp);
        LONG ex = GetWindowLongW(h, GWL_EXSTYLE), st = GetWindowLongW(h, GWL_STYLE);
        int bw = (ex & WS_EX_CLIENTEDGE) ? GetSystemMetrics(SM_CXEDGE) : (st & WS_BORDER) ? 1 : 0;
        if (bw > 0) {
            HDC dc = GetWindowDC(h);
            RECT rc;
            GetWindowRect(h, &rc);
            OffsetRect(&rc, -rc.left, -rc.top);
            FrameRect(dc, &rc, g_br_border);
            for (int i = 1; i < bw; i++) {
                InflateRect(&rc, -1, -1);
                FrameRect(dc, &rc, g_br_field);
            }
            ReleaseDC(h, dc);
        }
        return r;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

/* A push button that is turned off: the dark theme draws it like one that
 * is on, so dark mode draws it itself, flat, with grey text. */
static LRESULT CALLBACK dark_button_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
    (void)id;
    (void)ref;
    LONG kind = GetWindowLongW(h, GWL_STYLE) & BS_TYPEMASK;
    if (msg == WM_PAINT && g_dark && !g_hc && !IsWindowEnabled(h) && (kind == BS_PUSHBUTTON || kind == BS_DEFPUSHBUTTON)) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        HWND parent = GetParent(h);
        HBRUSH under = parent == g_main ? g_br_bg : (HBRUSH)SendMessageW(parent, WM_CTLCOLORBTN, (WPARAM)dc, (LPARAM)h);
        FillRect(dc, &rc, under ? under : g_br_bg);
        HBRUSH face = CreateSolidBrush(RGB(38, 38, 38));
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(61, 61, 61));
        HGDIOBJ ob = SelectObject(dc, face), op = SelectObject(dc, pen);
        RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, S(6), S(6));
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(face);
        DeleteObject(pen);
        wchar_t text[256];
        GetWindowTextW(h, text, 256);
        HFONT f = (HFONT)SendMessageW(h, WM_GETFONT, 0, 0);
        HGDIOBJ of = f ? SelectObject(dc, f) : NULL;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(110, 110, 110));
        DrawTextW(dc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_HIDEPREFIX);
        if (of) SelectObject(dc, of);
        EndPaint(h, &ps);
        return 0;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

static void theme_control(HWND h)
{
    wchar_t cls[64];
    if (!GetClassNameW(h, cls, 64)) return;
    if (p_allow_dark_window) p_allow_dark_window(h, g_dark);
    LONG style = GetWindowLongW(h, GWL_STYLE);
    if (_wcsicmp(cls, WC_BUTTONW) == 0) {
        LONG kind = style & BS_TYPEMASK;
        if (kind == BS_PUSHBUTTON || kind == BS_DEFPUSHBUTTON) {
            SetWindowTheme(h, g_dark ? L"DarkMode_Explorer" : NULL, NULL);
            SetWindowSubclass(h, dark_button_proc, 3, 0);
        }
        /* A themed check box or group box draws its text black whatever
         * colour it is given: unthemed it takes the colour. */
        else SetWindowTheme(h, g_dark ? L"" : NULL, g_dark ? L"" : NULL);
    } else if (_wcsicmp(cls, WC_EDITW) == 0) {
        SetWindowTheme(h, g_dark ? (style & ES_MULTILINE ? L"DarkMode_Explorer" : L"DarkMode_CFD") : NULL, NULL);
        if (style & ES_MULTILINE) {
            SetWindowSubclass(h, dark_edit_proc, 2, 0);
            RedrawWindow(h, NULL, NULL, RDW_FRAME | RDW_INVALIDATE);
        }
    } else if (_wcsicmp(cls, WC_COMBOBOXW) == 0) {
        SetWindowTheme(h, g_dark ? L"DarkMode_CFD" : NULL, NULL);
    } else if (_wcsicmp(cls, WC_TREEVIEWW) == 0) {
        SetWindowTheme(h, g_dark ? L"DarkMode_Explorer" : L"Explorer", NULL);
        TreeView_SetBkColor(h, g_dark ? DARK_FIELD : (COLORREF)-1);
        TreeView_SetTextColor(h, g_dark ? DARK_TEXT : (COLORREF)-1);
        tree_metrics();
    } else if (_wcsicmp(cls, WC_LISTVIEWW) == 0) {
        SetWindowTheme(h, g_dark ? L"DarkMode_Explorer" : NULL, NULL);
        HWND header = ListView_GetHeader(h);
        if (header) SetWindowTheme(header, g_dark ? L"DarkMode_ItemsView" : NULL, NULL);
        SetWindowSubclass(h, dark_lv_proc, 2, 0);
        if (header) InvalidateRect(header, NULL, TRUE);
        COLORREF bg = g_dark ? DARK_FIELD : GetSysColor(COLOR_WINDOW);
        ListView_SetBkColor(h, bg);
        ListView_SetTextBkColor(h, bg);
        ListView_SetTextColor(h, g_dark ? DARK_TEXT : GetSysColor(COLOR_WINDOWTEXT));
    } else if (_wcsicmp(cls, WC_SCROLLBARW) == 0 || _wcsicmp(cls, WC_LISTBOXW) == 0) {
        SetWindowTheme(h, g_dark ? L"DarkMode_Explorer" : NULL, NULL);
    } else if (_wcsicmp(cls, WC_TABCONTROLW) == 0) {
        SetWindowSubclass(h, dark_tab_proc, 1, 0);
    } else if (_wcsicmp(cls, STATUSCLASSNAMEW) == 0) {
        SetWindowSubclass(h, dark_status_proc, 1, 0);
    }
    InvalidateRect(h, NULL, TRUE);
}

static BOOL CALLBACK theme_child(HWND h, LPARAM lp)
{
    (void)lp;
    theme_control(h);
    return TRUE;
}

/* A window and everything in it, in the current mode. */
static void theme_window(HWND top)
{
    if (p_allow_dark_window) p_allow_dark_window(top, g_dark);
    theme_title_bar(top);
    EnumChildWindows(top, theme_child, 0);
    RedrawWindow(top, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

/* Colours for labels, text boxes and lists: 1 if handled (dark). */
static int theme_ctlcolor(UINT msg, WPARAM wp, LPARAM lp, LRESULT *out)
{
    if (!g_dark) return 0;
    HDC dc = (HDC)wp;
    HWND ctl = (HWND)lp;
    switch (msg) {
    case WM_CTLCOLORDLG:
        *out = (LRESULT)g_br_bg;
        return 1;
    case WM_CTLCOLORSTATIC: {
        wchar_t cls[16];
        int edit = GetClassNameW(ctl, cls, 16) && _wcsicmp(cls, WC_EDITW) == 0; /* a read-only text box */
        SetTextColor(dc, IsWindowEnabled(ctl) ? DARK_TEXT : DARK_GRAY);
        SetBkColor(dc, edit ? DARK_FIELD : DARK_BG);
        *out = (LRESULT)(edit ? g_br_field : g_br_bg);
        return 1;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        SetTextColor(dc, DARK_TEXT);
        SetBkColor(dc, DARK_FIELD);
        *out = (LRESULT)g_br_field;
        return 1;
    case WM_CTLCOLORBTN:
        SetTextColor(dc, DARK_TEXT);
        SetBkColor(dc, DARK_BG);
        *out = (LRESULT)g_br_bg;
        return 1;
    }
    return 0;
}

/* A dialog's own messages: put its text in the chosen language as it is
 * made, theme it as it is first shown, and colour its controls. Each
 * dialog procedure calls this first. */
static int theme_dialog(HWND dlg, UINT msg, WPARAM wp, LPARAM lp, INT_PTR *out)
{
    if (msg == WM_INITDIALOG) lang_dialog(dlg); /* before the dialog sets its own text */
    if (msg == WM_SHOWWINDOW && wp && g_dark && !GetPropW(dlg, L"BrodalfDark")) {
        SetPropW(dlg, L"BrodalfDark", (HANDLE)1);
        theme_window(dlg);
        return 0;
    }
    if (msg == WM_DESTROY) RemovePropW(dlg, L"BrodalfDark");
    LRESULT r;
    if (theme_ctlcolor(msg, wp, lp, &r)) {
        *out = (INT_PTR)r;
        return 1;
    }
    return 0;
}

/* The tabs, painted: the tab control has no dark look of its own. */
static LRESULT CALLBACK dark_tab_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
    (void)id;
    (void)ref;
    if (g_dark && msg == WM_ERASEBKGND) return 1;
    if (g_dark && msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        FillRect(dc, &rc, g_br_bg);
        RECT disp = rc;
        TabCtrl_AdjustRect(h, FALSE, &disp);
        InflateRect(&disp, S(3), S(3));
        FrameRect(dc, &disp, g_br_border);
        HFONT font = (HFONT)SendMessageW(h, WM_GETFONT, 0, 0);
        HGDIOBJ old = font ? SelectObject(dc, font) : NULL;
        SetBkMode(dc, TRANSPARENT);
        int n = TabCtrl_GetItemCount(h), sel = TabCtrl_GetCurSel(h);
        for (int i = 0; i < n; i++) {
            RECT r;
            if (!TabCtrl_GetItemRect(h, i, &r)) continue;
            if (i == sel) InflateRect(&r, S(2), S(2));
            FillRect(dc, &r, i == sel ? g_br_hot : g_br_bg);
            FrameRect(dc, &r, g_br_border);
            wchar_t text[128] = L"";
            TCITEMW it;
            memset(&it, 0, sizeof(it));
            it.mask = TCIF_TEXT;
            it.pszText = text;
            it.cchTextMax = 128;
            TabCtrl_GetItem(h, i, &it);
            SetTextColor(dc, i == sel ? DARK_TEXT : DARK_GRAY);
            DrawTextW(dc, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            /* The keyboard focus, as the light theme shows it (not after a
             * click, while Windows hides focus marks). */
            if (i == sel && GetFocus() == h && !(SendMessageW(h, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)) {
                RECT fr = r;
                InflateRect(&fr, -S(3), -S(3));
                SetTextColor(dc, DARK_TEXT);
                SetBkColor(dc, DARK_HOT);
                DrawFocusRect(dc, &fr);
            }
        }
        if (old) SelectObject(dc, old);
        EndPaint(h, &ps);
        return 0;
    }
    if (g_dark && (msg == WM_SETFOCUS || msg == WM_KILLFOCUS || msg == WM_UPDATEUISTATE)) {
        LRESULT r = DefSubclassProc(h, msg, wp, lp);
        InvalidateRect(h, NULL, FALSE);
        return r;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

/* The status bar, painted: its text is always black otherwise. */
static LRESULT CALLBACK dark_status_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
    (void)id;
    (void)ref;
    if (g_dark && msg == WM_ERASEBKGND) return 1;
    if (g_dark && msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        FillRect(dc, &rc, g_br_bg);
        RECT line = rc;
        line.bottom = line.top + 1;
        FillRect(dc, &line, g_br_border);
        HFONT font = (HFONT)SendMessageW(h, WM_GETFONT, 0, 0);
        HGDIOBJ old = font ? SelectObject(dc, font) : NULL;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, DARK_TEXT);
        int parts = (int)SendMessageW(h, SB_GETPARTS, 0, 0);
        for (int i = 0; i < parts && i < 8; i++) {
            RECT r;
            if (!SendMessageW(h, SB_GETRECT, (WPARAM)i, (LPARAM)&r)) continue;
            int len = LOWORD(SendMessageW(h, SB_GETTEXTLENGTHW, (WPARAM)i, 0));
            wchar_t *text = calloc((size_t)len + 1, sizeof(wchar_t));
            if (!text) continue;
            SendMessageW(h, SB_GETTEXTW, (WPARAM)i, (LPARAM)text);
            if (i > 0 && text[0]) {
                RECT sep = {r.left, r.top + S(4), r.left + 1, r.bottom - S(4)};
                FillRect(dc, &sep, g_br_border);
            }
            r.left += S(6);
            DrawTextW(dc, text, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            free(text);
        }
        if (old) SelectObject(dc, old);
        EndPaint(h, &ps);
        return 0;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

/* The menu bar, painted (Windows draws it light whatever the mode). These
 * are the messages and structures File Explorer's menu bar uses. */
#define WM_UAHDRAWMENU 0x0091
#define WM_UAHDRAWMENUITEM 0x0092
typedef struct { HMENU hmenu; HDC hdc; DWORD dwFlags; } uah_menu;
typedef struct { DWORD cx, cy; } uah_size;
typedef struct { DWORD rgcx[4]; DWORD fUpdateMaxWidths : 2; } uah_popup_metrics;
typedef struct { int iPosition; uah_size umim; uah_popup_metrics umpm; } uah_menu_item;
typedef struct { DRAWITEMSTRUCT dis; uah_menu um; uah_menu_item umi; } uah_draw_menu_item;

static int theme_menu_bar(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT *out)
{
    if (!g_dark) return 0;
    switch (msg) {
    case WM_UAHDRAWMENU: {
        uah_menu *um = (uah_menu *)lp;
        MENUBARINFO mbi;
        memset(&mbi, 0, sizeof(mbi));
        mbi.cbSize = sizeof(mbi);
        if (!GetMenuBarInfo(hwnd, OBJID_MENU, 0, &mbi)) return 0;
        RECT win;
        GetWindowRect(hwnd, &win);
        OffsetRect(&mbi.rcBar, -win.left, -win.top);
        FillRect(um->hdc, &mbi.rcBar, g_br_bg);
        *out = TRUE;
        return 1;
    }
    case WM_UAHDRAWMENUITEM: {
        uah_draw_menu_item *d = (uah_draw_menu_item *)lp;
        wchar_t text[128] = L"";
        MENUITEMINFOW mi;
        memset(&mi, 0, sizeof(mi));
        mi.cbSize = sizeof(mi);
        mi.fMask = MIIM_STRING;
        mi.dwTypeData = text;
        mi.cch = 127;
        GetMenuItemInfoW(d->um.hmenu, (UINT)d->umi.iPosition, TRUE, &mi);
        UINT state = d->dis.itemState;
        int lit = (state & (ODS_HOTLIGHT | ODS_SELECTED)) != 0;
        FillRect(d->um.hdc, &d->dis.rcItem, lit ? g_br_hot : g_br_bg);
        SetBkMode(d->um.hdc, TRANSPARENT);
        SetTextColor(d->um.hdc, state & (ODS_GRAYED | ODS_DISABLED | ODS_INACTIVE) ? DARK_GRAY : DARK_TEXT);
        DrawTextW(d->um.hdc, text, -1, &d->dis.rcItem,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | (state & ODS_NOACCEL ? DT_HIDEPREFIX : 0));
        *out = TRUE;
        return 1;
    }
    case WM_NCPAINT:
    case WM_NCACTIVATE: {
        /* Windows leaves a light line under the menu bar: paint over it. */
        *out = DefWindowProcW(hwnd, msg, wp, lp);
        MENUBARINFO mbi;
        memset(&mbi, 0, sizeof(mbi));
        mbi.cbSize = sizeof(mbi);
        if (GetMenuBarInfo(hwnd, OBJID_MENU, 0, &mbi)) {
            RECT client, win;
            GetClientRect(hwnd, &client);
            MapWindowPoints(hwnd, NULL, (POINT *)&client, 2);
            GetWindowRect(hwnd, &win);
            OffsetRect(&client, -win.left, -win.top);
            RECT line = client;
            line.bottom = line.top;
            line.top--;
            HDC dc = GetWindowDC(hwnd);
            FillRect(dc, &line, g_br_bg);
            ReleaseDC(hwnd, dc);
        }
        return 1;
    }
    }
    return 0;
}

/* Change to what the choice comes to now: the whole window, its menus
 * and the tree's colours. */
static void theme_apply(void)
{
    g_hc = high_contrast();
    g_dark = theme_want_dark();
    theme_app_mode();
    if (!g_main) return;
    theme_window(g_main);
    DrawMenuBar(g_main);
}

/* View > Language. It takes effect the next time BRODALF starts, since
 * most of the window's text is set once, when it is made. */
static void lang_choose(int choice)
{
    const wchar_t *code = choice >= 0 ? g_langs[choice].code : L"";
    RegSetKeyValueW(HKEY_CURRENT_USER, REG_KEY, L"Language", REG_SZ, code, (DWORD)((wcslen(code) + 1) * sizeof(wchar_t)));
    int now = choice >= 0 ? choice : lang_windows();
    g_lang_choice = choice;
    if (now != g_lang)
        MessageBoxW(g_main, T(L"BRODALF shows the new language the next time you open it."), APP_NAME, MB_ICONINFORMATION);
}

static void theme_choose(int choice)
{
    g_theme_choice = choice;
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) == ERROR_SUCCESS) {
        DWORD v = (DWORD)choice;
        RegSetValueExW(k, L"Appearance", 0, REG_DWORD, (const BYTE *)&v, sizeof(v));
        RegCloseKey(k);
    }
    theme_apply();
}

/* ---- Jobs --------------------------------------------------------------- */

/* JOB_REFRESH saves a change made in the window and reads the figures the
 * window shows again (see read_figures). It runs like the others, but has
 * no Stop: a save cannot be stopped half way. */
typedef enum { JOB_DRIVES, JOB_SCAN, JOB_BACKUP, JOB_CHECK, JOB_RESTORE, JOB_VERIFY, JOB_SHADOW, JOB_COMPRESS, JOB_REFRESH } job_kind;
#define SUMMARY_LEN 2048 /* room for the longer translations */

/* A file a restore left alone because another file was already where it
 * would go. Collected over every part of one restore. */
typedef struct {
    wchar_t *where;     /* root and path */
    wchar_t *dest;
    int64_t want_size, want_mtime_ns, have_size, have_mtime_ns;
    int64_t version_id; /* what it was going to put there */
} conflict_row;

typedef struct job {
    job_kind kind;
    int64_t media_id;   /* backup/check: a connected drive, or 0 with root */
    char *root;         /* backup to a drive given by folder */
    char *label;        /* set up a new drive with this name first */
    char *location;     /* and say where it is kept */
    unsigned flags;     /* for the new drive: BD_MEDIA_ENCRYPTED, BD_MEDIA_HIDE_NAMES */
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
    int then_all;       /* scan: then back up to every plugged-in drive */
    int64_t new_ids[16]; /* drives: drives connected by this job */
    int n_new;
    int64_t many[16];   /* backup: several drives and extra places in one job */
    int n_many, many_places;
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
    int from_shadow;    /* scan/backup: reads files in use from a shadow copy; the copy is removed after the last one */
    int compress_on;    /* compress: turn compression on (else off) */
    int just_written;   /* verify: only the copies not read back since they were written */
    int64_t copied, copied_to; /* backup to one drive: copies written, and to which drive */
    int64_t many_copied[16]; /* backup: copies written to each of many[] */
    int64_t damaged;    /* scan: files changed without their size or time changing */
    int64_t found_deleted; /* scan: files gone from their folder */
    int64_t roots_missing; /* scan: backup roots not found */
    int problems;
    int saving;         /* refresh: a change made in the window is saved */
    int skip_figures;   /* another job is waiting: it reads the figures after it */
    struct figures *figs; /* read after the job, for the window to show */
    /* What went wrong, counted as the summary counts it (files that failed
     * or did not fit, copies missing or damaged...), and of those the ones
     * no question after the job is about. */
    int64_t n_problems, n_tell;
    /* Every line the engine reported while the job ran, for "Show what
     * went wrong". */
    wchar_t **report;
    int n_report, cap_report;
    bd_status status;
    wchar_t summary[SUMMARY_LEN];
    struct job *next;
} job;

static job *g_queue;

static void start_backup(job *b, int offer_scan);
static bd_status run_shadow(job *j);
static void free_conflicts(conflict_row *c, int n);
struct figures;
static struct figures *read_figures(void);
/* ---- Questions ------------------------------------------------------------ */

/* A yes-or-no question as a task dialog. A first or last paragraph that is
 * a question becomes the main instruction, the rest the content, and the
 * buttons say what they do: yes and no may be NULL ("Yes" and "No", in the
 * window's language, where a message box showed Windows' own). Returns
 * IDYES or IDNO, as MessageBox did. */
static int ask_verbs(HWND owner, const wchar_t *text, UINT flags, const wchar_t *yes, const wchar_t *no)
{
    if (!text) text = L"";
    size_t n = wcslen(text);
    wchar_t *copy = malloc((n + 1) * sizeof(wchar_t));
    if (!copy) return MessageBoxW(owner, text, APP_NAME, flags);
    memcpy(copy, text, (n + 1) * sizeof(wchar_t));
    const wchar_t *main_text = NULL, *content = copy;
    wchar_t *first = wcsstr(copy, L"\n\n"), *last = NULL;
    for (wchar_t *q = first; q; q = wcsstr(q + 2, L"\n\n")) last = q;
    if (first && first - copy < 200 && first > copy && first[-1] == L'?') {
        *first = 0;
        main_text = copy;
        content = first + 2;
    } else if (last) {
        wchar_t *tail = last + 2;
        size_t tl = wcslen(tail);
        while (tl && (tail[tl - 1] == L' ' || tail[tl - 1] == L'\n')) tl--;
        if (tl > 0 && tl < 200 && tail[tl - 1] == L'?') {
            *last = 0;
            main_text = tail;
        }
    } else if (n > 0 && n < 200 && copy[n - 1] == L'?') {
        main_text = copy;
        content = NULL;
    }
    TASKDIALOG_BUTTON b[2] = {{IDYES, yes ? yes : T(L"Yes")}, {IDNO, no ? no : T(L"No")}};
    TASKDIALOGCONFIG tc;
    memset(&tc, 0, sizeof(tc));
    tc.cbSize = sizeof(tc);
    tc.hwndParent = owner;
    tc.hInstance = g_inst;
    tc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    tc.pszWindowTitle = APP_NAME;
    UINT icon = flags & MB_ICONMASK;
    tc.pszMainIcon = icon == MB_ICONWARNING ? TD_WARNING_ICON : icon == MB_ICONERROR ? TD_ERROR_ICON
                   : icon == MB_ICONINFORMATION ? TD_INFORMATION_ICON : NULL;
    tc.pszMainInstruction = main_text;
    tc.pszContent = content && content[0] ? content : NULL;
    tc.cButtons = 2;
    tc.pButtons = b;
    tc.nDefaultButton = (flags & MB_DEFMASK) == MB_DEFBUTTON2 ? IDNO : IDYES;
    int pressed = IDNO;
    HRESULT hr = TaskDialogIndirect(&tc, &pressed, NULL, NULL);
    free(copy);
    if (FAILED(hr)) return MessageBoxW(owner, text, APP_NAME, flags);
    return pressed == IDYES ? IDYES : IDNO;
}

static int ask_yn(HWND owner, const wchar_t *text, const wchar_t *caption, UINT flags)
{
    (void)caption;
    return ask_verbs(owner, text, flags, NULL, NULL);
}

static void figures_free(struct figures *f);

/* ---- Drawing: colours, chips, icons ------------------------------------------
 *
 * The status header, the warnings and the job panel are painted here, so
 * each state has its own colour, icon and word. The colours follow
 * Windows 11's own (its info bars): green for safe, blue for working,
 * amber for something to look at, red for something wrong, grey for
 * nothing yet. A colour is never all there is: every state also has its
 * words. High contrast uses the system's colours only. */
enum { TONE_NEUTRAL, TONE_SAFE, TONE_WORK, TONE_WARN, TONE_BAD };

static COLORREF tone_text(int t)
{
    if (g_hc) return GetSysColor(COLOR_WINDOWTEXT);
    switch (t) {
    case TONE_SAFE: return g_dark ? RGB(108, 203, 95) : RGB(15, 123, 15);
    case TONE_WORK: return g_dark ? RGB(96, 205, 255) : RGB(0, 95, 184);
    case TONE_WARN: return g_dark ? RGB(252, 225, 0) : RGB(157, 93, 0);
    case TONE_BAD: return g_dark ? RGB(255, 153, 164) : RGB(196, 43, 28);
    }
    return g_dark ? RGB(157, 157, 157) : RGB(95, 95, 95);
}

/* A bar's fill and an icon's disc. */
static COLORREF tone_fill(int t)
{
    if (g_hc) return GetSysColor(t == TONE_NEUTRAL ? COLOR_GRAYTEXT : COLOR_HIGHLIGHT);
    switch (t) {
    case TONE_SAFE: return g_dark ? RGB(108, 203, 95) : RGB(15, 123, 15);
    case TONE_WORK: return g_dark ? RGB(96, 205, 255) : RGB(0, 95, 184);
    case TONE_WARN: return g_dark ? RGB(230, 180, 0) : RGB(193, 140, 0);
    case TONE_BAD: return g_dark ? RGB(255, 120, 130) : RGB(196, 43, 28);
    }
    return g_dark ? RGB(120, 120, 120) : RGB(138, 138, 138);
}

/* What goes on a filled shape: white, or black on the light fills of dark mode. */
static COLORREF tone_on_fill(int t)
{
    if (g_hc) return GetSysColor(t == TONE_NEUTRAL ? COLOR_WINDOW : COLOR_HIGHLIGHTTEXT);
    return g_dark ? RGB(0, 0, 0) : RGB(255, 255, 255);
}

static COLORREF tone_tint(int t)
{
    if (g_hc) return GetSysColor(COLOR_WINDOW);
    switch (t) {
    case TONE_SAFE: return g_dark ? RGB(57, 61, 27) : RGB(223, 246, 221);
    case TONE_WORK: return g_dark ? RGB(31, 58, 77) : RGB(224, 238, 249);
    case TONE_WARN: return g_dark ? RGB(67, 53, 25) : RGB(255, 244, 206);
    case TONE_BAD: return g_dark ? RGB(68, 39, 38) : RGB(253, 231, 233);
    }
    return g_dark ? RGB(58, 58, 58) : RGB(235, 235, 235);
}

static COLORREF ui_bg(void) { return g_hc ? GetSysColor(COLOR_BTNFACE) : g_dark ? DARK_BG : GetSysColor(COLOR_BTNFACE); }
static COLORREF ui_card(void) { return g_hc ? GetSysColor(COLOR_WINDOW) : g_dark ? RGB(43, 43, 43) : RGB(255, 255, 255); }
static COLORREF ui_border(void) { return g_hc ? GetSysColor(COLOR_WINDOWTEXT) : g_dark ? RGB(61, 61, 61) : RGB(224, 224, 224); }
static COLORREF ui_text(void) { return g_hc ? GetSysColor(COLOR_WINDOWTEXT) : g_dark ? DARK_TEXT : RGB(26, 26, 26); }
static COLORREF ui_sub(void) { return g_hc ? GetSysColor(COLOR_GRAYTEXT) : g_dark ? RGB(170, 170, 170) : RGB(95, 95, 95); }
static COLORREF ui_track(void) { return g_hc ? GetSysColor(COLOR_BTNFACE) : g_dark ? RGB(58, 58, 58) : RGB(229, 229, 229); }
static COLORREF ui_link(void) { return g_hc ? GetSysColor(COLOR_HOTLIGHT) : g_dark ? RGB(96, 205, 255) : RGB(0, 95, 184); }

/* Half way between two colours. */
static COLORREF mix(COLORREF a, COLORREF b, int pct_b)
{
    int r = (GetRValue(a) * (100 - pct_b) + GetRValue(b) * pct_b) / 100;
    int g = (GetGValue(a) * (100 - pct_b) + GetGValue(b) * pct_b) / 100;
    int bl = (GetBValue(a) * (100 - pct_b) + GetBValue(b) * pct_b) / 100;
    return RGB(r, g, bl);
}

static void fill_color(HDC dc, const RECT *r, COLORREF c)
{
    SetDCBrushColor(dc, c);
    FillRect(dc, r, (HBRUSH)GetStockObject(DC_BRUSH));
}

/* A rounded rectangle; border CLR_INVALID for none. */
static void round_box(HDC dc, const RECT *r, int radius, COLORREF fill, COLORREF border)
{
    HGDIOBJ ob = SelectObject(dc, GetStockObject(DC_BRUSH)), op = SelectObject(dc, GetStockObject(border == CLR_INVALID ? NULL_PEN : DC_PEN));
    SetDCBrushColor(dc, fill);
    if (border != CLR_INVALID) SetDCPenColor(dc, border);
    RoundRect(dc, r->left, r->top, r->right + (border == CLR_INVALID ? 1 : 0), r->bottom + (border == CLR_INVALID ? 1 : 0), radius, radius);
    SelectObject(dc, ob);
    SelectObject(dc, op);
}

static int text_w(HDC dc, HFONT f, const wchar_t *s)
{
    HGDIOBJ o = SelectObject(dc, f);
    SIZE sz = {0, 0};
    GetTextExtentPoint32W(dc, s, (int)wcslen(s), &sz);
    SelectObject(dc, o);
    return sz.cx;
}

static void draw_str(HDC dc, HFONT f, COLORREF c, const wchar_t *s, RECT r, UINT flags)
{
    HGDIOBJ o = SelectObject(dc, f);
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, s, -1, &r, flags | DT_NOPREFIX);
    SelectObject(dc, o);
}

/* A chip: a word on a tinted, rounded background, at most maxw wide (0:
 * any), its words ending in "..." when they do not fit. Returns its width. */
static int draw_chip(HDC dc, int x, int y, int h, const wchar_t *s, int tone, int maxw)
{
    int w = text_w(dc, g_font, s) + S(12);
    if (maxw > 0 && w > maxw) w = maxw;
    RECT r = {x, y, x + w, y + h};
    if (g_hc) round_box(dc, &r, S(6), GetSysColor(COLOR_WINDOW), GetSysColor(COLOR_WINDOWTEXT));
    else round_box(dc, &r, S(6), tone_tint(tone), CLR_INVALID);
    RECT tr = {r.left + S(6), r.top, r.right - S(6), r.bottom};
    draw_str(dc, g_font, tone_text(tone), s, tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    return w;
}

enum { GLYPH_CHECK, GLYPH_BANG, GLYPH_CROSS, GLYPH_PLAY, GLYPH_PAUSE, GLYPH_INFO, GLYPH_DOTS };

/* A round state icon with its glyph drawn in lines, so no font is needed. */
static void draw_state_icon(HDC dc, int x, int y, int size, int tone, int glyph)
{
    RECT r = {x, y, x + size, y + size};
    HGDIOBJ ob = SelectObject(dc, GetStockObject(DC_BRUSH)), op = SelectObject(dc, GetStockObject(NULL_PEN));
    SetDCBrushColor(dc, tone_fill(tone));
    Ellipse(dc, r.left, r.top, r.right + 1, r.bottom + 1);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    int lw = size >= S(24) ? S(3) : S(2);
    if (lw < 1) lw = 1;
    LOGBRUSH lb = {BS_SOLID, tone_on_fill(tone), 0};
    HPEN pen = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND, (DWORD)lw, &lb, 0, NULL);
    op = SelectObject(dc, pen);
    int cx = x + size / 2, cy = y + size / 2, u = size / 4;
    switch (glyph) {
    case GLYPH_CHECK: {
        POINT p[3] = {{cx - u - u / 3, cy}, {cx - u / 4, cy + u * 2 / 3 + 1}, {cx + u + u / 3, cy - u * 2 / 3}};
        Polyline(dc, p, 3);
        break;
    }
    case GLYPH_BANG:
    case GLYPH_INFO: {
        int top = glyph == GLYPH_BANG ? cy - u - u / 3 : cy - u / 4, bot = glyph == GLYPH_BANG ? cy + u / 4 : cy + u + u / 3;
        int dot = glyph == GLYPH_BANG ? cy + u + u / 4 : cy - u - u / 4;
        MoveToEx(dc, cx, top, NULL);
        LineTo(dc, cx, bot);
        MoveToEx(dc, cx, dot, NULL);
        LineTo(dc, cx, dot);
        break;
    }
    case GLYPH_CROSS:
        MoveToEx(dc, cx - u, cy - u, NULL);
        LineTo(dc, cx + u, cy + u);
        MoveToEx(dc, cx + u, cy - u, NULL);
        LineTo(dc, cx - u, cy + u);
        break;
    case GLYPH_PLAY: {
        SelectObject(dc, GetStockObject(DC_BRUSH));
        SetDCBrushColor(dc, tone_on_fill(tone));
        POINT p[3] = {{cx - u / 2 - 1, cy - u}, {cx - u / 2 - 1, cy + u}, {cx + u, cy}};
        Polygon(dc, p, 3);
        break;
    }
    case GLYPH_PAUSE:
        MoveToEx(dc, cx - u / 2 - 1, cy - u, NULL);
        LineTo(dc, cx - u / 2 - 1, cy + u);
        MoveToEx(dc, cx + u / 2 + 1, cy - u, NULL);
        LineTo(dc, cx + u / 2 + 1, cy + u);
        break;
    case GLYPH_DOTS:
        for (int k = -1; k <= 1; k++) {
            MoveToEx(dc, cx + k * u, cy, NULL);
            LineTo(dc, cx + k * u, cy);
        }
        break;
    }
    SelectObject(dc, op);
    DeleteObject(pen);
}

/* A plain bar: a track and a fill, percent 0..100; -1 a moving piece. */
static void draw_bar(HDC dc, RECT r, int pct, int tone, int phase)
{
    round_box(dc, &r, S(4), ui_track(), CLR_INVALID);
    int w = r.right - r.left;
    if (pct < 0) {
        int seg = w / 4, x = r.left + (int)((long long)(w + seg) * (phase % 100) / 100) - seg;
        RECT f = {x < r.left ? r.left : x, r.top, x + seg > r.right ? r.right : x + seg, r.bottom};
        if (f.right > f.left) round_box(dc, &f, S(4), tone_fill(tone), CLR_INVALID);
        return;
    }
    if (pct > 100) pct = 100;
    RECT f = r;
    f.right = r.left + (int)((long long)w * pct / 100);
    if (f.right > f.left) round_box(dc, &f, S(4), tone_fill(tone), CLR_INVALID);
}

/* A drive or folder icon from Windows' own set, at size px. */
static HICON g_ico_drive, g_ico_folder, g_ico_cloud;
static void load_ui_icons(void)
{
    SHSTOCKICONINFO si;
    HICON *slot[3] = {&g_ico_drive, &g_ico_folder, &g_ico_cloud};
    SHSTOCKICONID ids[3] = {SIID_DRIVEREMOVE, SIID_FOLDER, SIID_DRIVENET};
    for (int i = 0; i < 3; i++) {
        if (*slot[i]) DestroyIcon(*slot[i]);
        *slot[i] = NULL;
        memset(&si, 0, sizeof(si));
        si.cbSize = sizeof(si);
        if (SUCCEEDED(SHGetStockIconInfo(ids[i], SHGSI_ICON | SHGSI_LARGEICON, &si))) *slot[i] = si.hIcon;
    }
}

/* ---- What the window shows at the top ---------------------------------------
 *
 * One record, read after every job and every change (update_drives_label)
 * and while a job runs (its progress), that the header, the warnings and
 * the job panel all draw from, so no two of them say different things. */
typedef struct {
    int64_t id;
    wchar_t name[128], where[200], path[400], chip[120], tip[900];
    int tone, connected, lost, secondary;
    int64_t total, freeb, held;
} hd_card;

#define HD_MAX_CARDS 24
static hd_card g_cards[HD_MAX_CARDS];
static int g_ncards;
static RECT g_card_rc[HD_MAX_CARDS + 1]; /* + the "more" card */
static int g_ncard_shown, g_card_more;

typedef struct { int64_t media_id, files; } hd_seg;
typedef struct {
    int set;
    int64_t files_done, files_total, bytes_done, bytes_total;
    int nseg;
    hd_seg seg[16];
} hd_bar;
static hd_bar g_hbars[MAX_SET_BARS];
static int g_nhbars;

static int g_target_copies = 2, g_target_places = 2;
static int g_verdict_tone = TONE_NEUTRAL, g_verdict_glyph = GLYPH_INFO;
static wchar_t g_verdict[400], g_verdict_sub[600];
static int g_next_kind; /* NEXT_*: what the header's button does */
static int64_t g_next_id;
enum { NEXT_NONE, NEXT_ADD_ROOT, NEXT_ADD_DRIVE, NEXT_BACKUP, NEXT_RISK, NEXT_SCAN };
static int64_t g_job_ids[16]; /* the drives the running job writes to or reads */
static int g_njob_ids;
static RECT g_goal_link;

/* The backup roots as last read: how many, which are missing, the last scan. */
static int g_nroots;
static int64_t g_last_scan_ms, g_last_backup_ms;
static wchar_t g_missing_name[4][200], g_missing_path[4][600];
static int g_nmissing;

static HWND g_header, g_warnbar, g_jobpanel, g_next_btn, g_log_btn;

/* The warnings, under the header: each with a title, a sentence and up
 * to two buttons. */
enum { WA_NONE, WA_CHANGED, WA_DAMAGED, WA_NEXT_TAB, WA_CHECK, WA_RELOCATE, WA_RESTORE_ROOT, WA_RISK, WA_LOCAL, WA_SCAN, WA_DELETED };
static int64_t g_gone_new; /* files gone from the backup roots since the user last looked */
typedef struct {
    int tone, glyph;
    wchar_t title[200], text[900], btn[2][120], key[80];
    int act[2];
    int64_t arg[2];
    int closable;
} warn_item;
#define MAX_WARNS 16
#define WARN_SHOWN 6
static warn_item g_warns[MAX_WARNS];
static int g_nwarn, g_warn_all;
static int g_nwarn_shown;
static RECT g_warn_rc[WARN_SHOWN], g_warn_x[WARN_SHOWN], g_warn_more;
static HWND g_warn_btns[WARN_SHOWN][2];
static wchar_t g_warn_hidden[32][80];
static int g_nwarn_hidden;

/* The job panel, under the tabs: what runs now, or how the last job ended. */
enum { JP_IDLE, JP_RUN, JP_OK, JP_STOPPED, JP_FAILED, JP_PROBLEM };
enum { JA_NONE, JA_REPORTS, JA_DAMAGED, JA_CONFLICTS, JA_CHECK, JA_BACKUP, JA_OPEN_DEST };
static int g_jp_state, g_jp_pct, g_jp_fpct = -1, g_jp_phase;
static wchar_t g_jp_title[600], g_jp_line[SUMMARY_LEN + 400], g_jp_file[700], g_jp_right[200];
static int g_jp_act[2];
static wchar_t g_jp_dest[600]; /* the folder the last restore went into */
static int64_t g_jp_arg[2];
static wchar_t g_jp_btn_text[2][120];
static HWND g_jp_btn[2];
static RECT g_jp_close;
static int g_log_open = 1;

static HFONT g_font_semi, g_font_title;

static void header_refresh(void);
static void header_layout(void);
static void warn_layout(void);
static void warn_tooltips(void);
static void jobpanel_layout(void);
static int header_height(int w);
static int warn_height(void);
static int jobpanel_height(int w);
static void jp_start(const wchar_t *what);
static void jp_stopping(void);
static void goal_text(wchar_t *out, size_t cap);
static void status_bar_refresh(void);
static void friendly_time(int64_t ms, wchar_t *out, size_t cap);
static void status_state(const wchar_t *s);
static void select_media_row(int64_t id, int secondary);
static void drive_popup(int64_t id, int x, int y);

/* Is the window's style set to show it (whether or not its parent shows)? */
static int is_shown(HWND h) { return h && (GetWindowLongW(h, GWL_STYLE) & WS_VISIBLE) != 0; }

/* The warnings are built afresh each time (update_drives_label). */
static void warn_begin(void) { g_nwarn = 0; }

static warn_item *warn_add(int tone, int glyph, const wchar_t *key)
{
    if (g_nwarn >= MAX_WARNS) return NULL;
    warn_item *w = &g_warns[g_nwarn++];
    memset(w, 0, sizeof(*w));
    w->tone = tone;
    w->glyph = glyph;
    w->closable = tone != TONE_BAD;
    swprintf(w->key, 80, L"%ls", key);
    return w;
}

static void warn_button(warn_item *w, int k, int act, int64_t arg, const wchar_t *fmt, ...)
{
    if (!w || k < 0 || k > 1) return;
    va_list ap;
    va_start(ap, fmt);
    vswprintf(w->btn[k], 120, fmt, ap);
    va_end(ap);
    w->act[k] = act;
    w->arg[k] = arg;
}

/* Red first, then amber, then the rest; in the order added within each. */
static void warn_sort(void)
{
    for (int i = 1; i < g_nwarn; i++) {
        warn_item t = g_warns[i];
        int rank = t.tone == TONE_BAD ? 0 : t.tone == TONE_WARN ? 1 : 2, j = i - 1;
        while (j >= 0) {
            int rj = g_warns[j].tone == TONE_BAD ? 0 : g_warns[j].tone == TONE_WARN ? 1 : 2;
            if (rj <= rank) break;
            g_warns[j + 1] = g_warns[j];
            j--;
        }
        g_warns[j + 1] = t;
    }
}


static void job_free(job *j)
{
    if (!j) return;
    figures_free(j->figs);
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
    for (int i = 0; i < j->n_report; i++) free(j->report[i]);
    free(j->report);
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
    j->summary[SUMMARY_LEN - 1] = 0; /* cut off, not lost, if it did not fit */
}

static void post_text(UINT msg, const wchar_t *text)
{
    wchar_t *copy = _wcsdup(text);
    if (copy && !PostMessageW(g_main, msg, 0, (LPARAM)copy)) free(copy);
}

#define REPORT_LINES 5000
/* A backup to several drives at once may report from more than one thread. */
static SRWLOCK g_report_lock = SRWLOCK_INIT;

/* The engine's log lines seen most, as full sentences in the language
 * shown, with Windows paths. A line not here keeps its words, with a
 * capital letter. */
static const struct { const char *pre, *post; const wchar_t *say; } g_core_lines[] = {
    {"scanning ", "", N_(L"Scanning %ls")},
    {"contents changed but size and time did not (damage?): ", "", N_(L"May be damaged: its contents changed, but its size and time did not: %ls")},
    {"something else is already at ", "; left alone", N_(L"Left alone, because something else is already at %ls")},
    {"backup root ", " is not reachable; skipped (nothing marked deleted)", N_(L"Backup root %ls cannot be reached, so it was skipped. Nothing in it was marked deleted.")},
    {"missing on drive: ", "", N_(L"Missing on the drive: %ls")},
    {"damaged on drive: ", "", N_(L"Damaged on the drive: %ls")},
    {"in use by another program, skipped: ", "", N_(L"In use by another program, so it was skipped: %ls")},
    {"moved or renamed, copies kept: ", "", N_(L"Moved or renamed; its copies are kept: %ls")},
    {"skipped link: ", "", N_(L"Skipped a link: %ls")},
    {"online-only, skipped: ", "", N_(L"Online only (not on this PC), so it was skipped: %ls")},
    {"online-only (not on this PC), not copied: ", "", N_(L"Online only (not on this PC), so it was not copied: %ls")},
    {"changed while copying, scan again: ", "", N_(L"Changed while it was being copied; scan again: %ls")},
    {"source file missing, scan again: ", "", N_(L"Gone from its folder while backing up; scan again: %ls")},
    {"an older copy is gone from the drive: ", "", N_(L"An older copy is gone from the drive: %ls")},
    {"the copy is there again as it was, so it counts again: ", "", N_(L"The copy is there again as it was, so it counts again: %ls")},
    {"backing up to ", " drives at once: each file is read once and written to all of them",
     N_(L"Backing up to %ls drives at once: each file is read once and written to all of them.")},
    {"encrypted copies are skipped: enter the passphrase to check them too", NULL,
     N_(L"Encrypted copies are skipped: enter the passphrase to check them too.")},
    {"files changed since the scan are read again and backed up as they are now", NULL,
     N_(L"Files changed since the scan are read again and backed up as they are now.")},
    {"files that are online-only (in the cloud, not on this PC) are skipped, not downloaded", NULL,
     N_(L"Files that are online only (in the cloud, not on this PC) are skipped, not downloaded.")},
    {"the drive is full; files that do not fit wait for another drive", NULL, N_(L"The drive is full. Files that do not fit wait for another drive.")},
    {"the drive stopped answering or was unplugged: the backup stops", NULL, N_(L"The drive stopped answering or was unplugged, so the backup stops.")},
    {"the drive stopped answering or was unplugged: nothing on it was marked missing or damaged", NULL,
     N_(L"The drive stopped answering or was unplugged. Nothing on it was marked missing or damaged.")},
};

static wchar_t *core_line(const char *message)
{
    size_t ml = strlen(message);
    for (size_t i = 0; i < sizeof(g_core_lines) / sizeof(g_core_lines[0]); i++) {
        const char *pre = g_core_lines[i].pre, *post = g_core_lines[i].post;
        size_t pl = strlen(pre);
        if (!post) {
            if (strcmp(message, pre) == 0) return _wcsdup(T(g_core_lines[i].say));
            continue;
        }
        size_t ql = strlen(post);
        if (ml < pl + ql || strncmp(message, pre, pl) != 0 || strcmp(message + ml - ql, post) != 0) continue;
        char *arg = malloc(ml - pl - ql + 1);
        if (!arg) break;
        memcpy(arg, message + pl, ml - pl - ql);
        arg[ml - pl - ql] = 0;
        wchar_t *wa = widen(arg);
        free(arg);
        if (!wa) break;
        for (wchar_t *c = wa; *c; c++) if (*c == L'/') *c = L'\\';
        size_t cap = wcslen(wa) + wcslen(T(g_core_lines[i].say)) + 8;
        wchar_t *out = malloc(cap * sizeof(wchar_t));
        if (out) swprintf(out, cap, T(g_core_lines[i].say), wa);
        free(wa);
        return out;
    }
    wchar_t *w = widen(message);
    if (w && w[0] >= L'a' && w[0] <= L'z') w[0] = (wchar_t)(w[0] - L'a' + L'A');
    return w;
}

static void job_log(void *ctx, const char *message)
{
    job *j = ctx;
    j->problems++;
    wchar_t *w = core_line(message);
    if (!w) return;
    post_text(WM_APP_LOG, w);
    AcquireSRWLockExclusive(&g_report_lock);
    if (j->n_report == j->cap_report && j->n_report < REPORT_LINES) {
        int cap = j->cap_report ? j->cap_report * 2 : 64;
        wchar_t **grown = realloc(j->report, sizeof(wchar_t *) * (size_t)cap);
        if (grown) {
            j->report = grown;
            j->cap_report = cap;
        }
    }
    if (j->n_report < j->cap_report) {
        j->report[j->n_report++] = w;
        w = NULL;
    }
    ReleaseSRWLockExclusive(&g_report_lock);
    free(w);
}

/* What the progress bars show: percent (-1: unknown, keep it moving) and a
 * line with the counts, the data rate and the time left; and for the file
 * being worked on, its own percent (-1: no bar) and name. */
typedef struct { int percent, file_percent; wchar_t title[400], text[1024], file[600]; } prog_msg;

/* A line for the bar, with no count: the bar keeps moving. */
static void post_busy_text(const wchar_t *text)
{
    prog_msg *m = calloc(1, sizeof(*m));
    if (!m) return;
    m->percent = -1;
    m->file_percent = -1;
    swprintf(m->text, 1024, L"%ls", text);
    if (!PostMessageW(g_main, WM_APP_PROGRESS, 0, (LPARAM)m)) free(m);
}

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
    if (secs < 60) swprintf(out, n, T(L", less than a minute left"));
    else if (secs < 3600) {
        int mins = (int)(secs / 60 + 0.5);
        swprintf(out, n, TN(mins, L", about %d minute left", L", about %d minutes left"), mins);
    }
    else swprintf(out, n, T(L", about %.1f hours left"), secs / 3600.0);
}

typedef struct { wchar_t names[300]; int n; } restore_from;

static int restore_from_cb(void *ctx, const bd_restore_step *st)
{
    restore_from *rf = ctx;
    if (!st->connected || rf->n >= 3) return 0;
    wchar_t *w = widen(st->label);
    if (w && *w && !wcsstr(rf->names, w)) {
        size_t len = wcslen(rf->names);
        swprintf(rf->names + len, 300 - len, L"%ls%ls", len ? L", " : L"", w);
        rf->n++;
    }
    free(w);
    return 0;
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
    const wchar_t *verb = strcmp(ph, "scan") == 0 ? T(L"Scanning") : strcmp(ph, "backup") == 0 ? T(L"Backing up")
                        : strcmp(ph, "restore") == 0 ? T(L"Restoring") : strcmp(ph, "verify") == 0 ? T(L"Checking copies")
                        : T(L"Checking copies");
    const wchar_t *unit = strcmp(ph, "scan") == 0 ? T(L"files looked at") : strcmp(ph, "backup") == 0 ? T(L"files copied")
                        : strcmp(ph, "restore") == 0 ? T(L"files restored") : T(L"copies read");
    /* Which drive, and how many there are: a backup to several drives
     * starts over for each, and the line says so. */
    if ((strcmp(ph, "backup") == 0 || strcmp(ph, "restore") == 0) && g_job_dest[0]) verb = g_job_dest;
    /* Copies were just recorded, on the job's own thread: the full-copy
     * bars grow with them, at most once a second. */
    if (p->recorded && strcmp(ph, "backup") == 0) post_live_sets();
    swprintf(m->title, 400, L"%ls", verb);
    wchar_t done[64], total[64], rate[64] = L"", left[64] = L"", *c = widen(p->current);
    format_bytes(p->bytes_done, done, 64);
    double r = p->done ? -1.0 : rate_add(p->bytes_done);
    if (r >= 0) { format_bytes((int64_t)r, rate, 64); wcsncat(rate, L"/s", 63 - wcslen(rate)); }
    if (p->bytes_total > 0) {
        format_bytes(p->bytes_total, total, 64);
        format_left(p->bytes_total - p->bytes_done, r, left, 64);
        m->percent = (int)(p->bytes_done * 100 / p->bytes_total);
        swprintf(m->text, 1024, T(L"%ls: %lld of %lld %ls, %ls of %ls%ls%ls%ls."), verb, (long long)p->files_done,
                 (long long)p->files_total, unit, done, total, *rate ? L", " : L"", rate, left);
    } else if (p->files_total > 0) {
        m->percent = (int)(p->files_done * 100 / p->files_total);
        swprintf(m->text, 1024, T(L"%ls: %lld of about %lld %ls, %ls read%ls%ls."), verb, (long long)p->files_done,
                 (long long)p->files_total, unit, done, *rate ? T(L" at ") : L"", rate);
    } else {
        m->percent = -1;
        swprintf(m->text, 1024, T(L"%ls: %lld %ls, %ls read%ls%ls."), verb, (long long)p->files_done, unit, done,
                 *rate ? T(L" at ") : L"", rate);
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
            swprintf(m->file, 600, T(L"%ls (%ls of %ls)"), c, fd, ft);
        } else {
            swprintf(m->file, 600, L"%ls", c);
        }
    }
    free(c);
    if (!PostMessageW(g_main, WM_APP_PROGRESS, 0, (LPARAM)m)) free(m);
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

/* Backup drives plugged in and counted (not extra places, not marked lost). */
static int count_plugged_cb(void *ctx, const bd_media_info *m)
{
    if (m->connected && !m->secondary && !m->lost_ms && strcmp(m->kind, "drive") == 0) ++*(int *)ctx;
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
    (void)found;
    int plugged = 0;
    bd_list_media(g_cat, count_plugged_cb, &plugged);
    swprintf(j->summary, SUMMARY_LEN, plugged ? TN(plugged, L"%d backup drive is plugged in.", L"%d backup drives are plugged in.") : T(L"No backup drive is plugged in."),
             plugged);
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
    for (wchar_t *q = r->where; q && *q; q++)
        if (*q == L'/') *q = L'\\';
    for (wchar_t *q = r->dest; q && *q; q++)
        if (*q == L'/') *q = L'\\';
    r->want_size = c->want_size;
    r->want_mtime_ns = c->want_mtime_ns;
    r->have_size = c->have_size;
    r->have_mtime_ns = c->have_mtime_ns;
    r->version_id = c->version_id;
    return 0;
}

static void collect_conflicts(job *j)
{
    bd_list_restore_conflicts(g_cat, conflict_cb, j);
}

/* Back up to several drives and extra places: all at once (each file read
 * once and written to every one) with the parallel_copies option, else
 * one after another. */
static bd_status run_backup_many(job *j)
{
    bd_backup_stats bs[16];
    memset(bs, 0, sizeof(bs));
    int parallel = bd_option_get(g_cat, "parallel_copies") > 0, done = 0;
    bd_status s = BD_OK, first_err = BD_OK;
    wchar_t label[300], line[700];
    if (parallel) {
        /* "Backing up to Blue and Red at once" */
        wchar_t names[300] = L"";
        for (int i = 0; i < j->n_many; i++) {
            job_dest_label(j->many[i], label, 300, NULL);
            size_t len = wcslen(names);
            swprintf(names + len, 300 - len, L"%ls%ls", i == 0 ? L"" : i == j->n_many - 1 ? T(L" and ") : L", ", label);
        }
        swprintf(g_job_dest, 400, T(L"Backing up to %ls at once"), names);
        swprintf(line, 700, L"%ls.", g_job_dest);
        post_text(WM_APP_LOG, line);
        s = bd_backup_many(g_cat, j->many, j->n_many, 0, bs, job_log, j);
        done = j->n_many;
    } else {
        for (int i = 0; i < j->n_many; i++) {
            if (g_stopping) { s = BD_ERR_CANCELLED; break; }
            /* Each drive starts the bar over: the line names it, and how many. */
            int place = 0;
            job_dest_label(j->many[i], label, 300, &place);
            swprintf(g_job_dest, 400, place ? T(L"Copying starred files to %ls (%d of %d)") : T(L"Backing up to %ls (%d of %d)"), label, i + 1,
                     j->n_many);
            swprintf(line, 700, L"%ls.", g_job_dest);
            post_text(WM_APP_LOG, line);
            bd_status one = bd_backup(g_cat, j->many[i], 0, &bs[i], job_log, j);
            done = i + 1;
            if (one == BD_ERR_CANCELLED || one == BD_ERR_PASSPHRASE) { s = one; break; }
            if (one != BD_OK && first_err == BD_OK) first_err = one;
        }
        if (s == BD_OK) s = first_err;
    }
    g_job_dest[0] = 0;
    int64_t copied = 0, bytes = 0, failed = 0, no_room = 0, no_room_bytes = 0, alongside = 0;
    wchar_t name[600], size[64];
    for (int i = 0; i < done; i++) {
        j->many_copied[i] = bs[i].files_copied;
        copied += bs[i].files_copied;
        bytes += bs[i].bytes_copied;
        failed += bs[i].files_failed;
        no_room += bs[i].files_no_room;
        no_room_bytes += bs[i].bytes_no_room;
        alongside += bs[i].copies_alongside;
        if (bs[i].files_in_use > j->in_use) j->in_use = bs[i].files_in_use;
        media_name(j->many[i], name, 600);
        format_bytes(bs[i].bytes_copied, size, 64);
        swprintf(line, 700, T(L"%ls: %lld files copied (%ls), %lld already there, %lld failed%ls."), name, (long long)bs[i].files_copied,
                 size, (long long)bs[i].files_already_there, (long long)bs[i].files_failed,
                 bs[i].files_no_room ? T(L", some did not fit") : L"");
        post_text(WM_APP_LOG, line);
    }
    int drives = j->n_many - j->many_places;
    wchar_t what[160];
    wchar_t some_drives[64], some_places[64];
    swprintf(some_drives, 64, TN(drives, L"%d drive", L"%d drives"), drives);
    swprintf(some_places, 64, TN(j->many_places, L"%d extra place", L"%d extra places"), j->many_places);
    if (drives && j->many_places) swprintf(what, 160, T(L"%ls and %ls"), some_drives, some_places);
    else wcscpy(what, drives ? some_drives : some_places);
    format_bytes(bytes, size, 64);
    swprintf(j->summary, SUMMARY_LEN, T(L"Backup to %ls %ls, %ls: %lld files copied (%ls) in all, %lld failed."), what,
             s == BD_ERR_CANCELLED ? T(L"stopped") : T(L"done"), parallel ? T(L"all at once") : T(L"one after another"), (long long)copied, size,
             (long long)failed);
    j->n_problems = j->n_tell = failed + no_room;
    if (alongside)
        say(j, T(L" %lld went alongside an older copy (under .versions): nothing there was overwritten."), (long long)alongside);
    if (no_room) {
        format_bytes(no_room_bytes, size, 64);
        say(j, TN(no_room, L" %lld copy (%ls) did not fit and waits for another drive.", L" %lld copies (%ls) did not fit and wait for another drive."),
            (long long)no_room, size);
    }
    return s;
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
        swprintf(j->summary, SUMMARY_LEN, T(L"%ls done: %lld files. Since the last scan: %lld new, %lld changed, %lld moved or renamed, %lld deleted; %ls read."),
                 j->bitrot ? T(L"Bit-rot scan") : T(L"Scan"), (long long)st.files_seen, (long long)st.files_new,
                 (long long)st.files_changed, (long long)st.files_moved, (long long)st.files_deleted, size);
        if (st.deleted_no_copy) say(j, T(L" %lld of the deleted files were never backed up."), (long long)st.deleted_no_copy);
        if (st.skipped) say(j, T(L" %lld left out by the skip lists."), (long long)st.skipped);
        if (st.files_in_use) say(j, T(L" %lld in use by another program."), (long long)st.files_in_use);
        if (st.files_online_only) say(j, T(L" %lld online-only cloud files left alone (not downloaded)."), (long long)st.files_online_only);
        if (st.files_damaged)
            say(j, TN(st.files_damaged, L" %lld file changed without its size or time changing: it may be damaged.",
                      L" %lld files changed without their size or time changing: they may be damaged."),
                (long long)st.files_damaged);
        if (st.roots_missing)
            say(j, TN(st.roots_missing, L" %lld backup root could not be found.", L" %lld backup roots could not be found."), (long long)st.roots_missing);
        if (st.roots_stopped)
            say(j, TN(st.roots_stopped, L" %lld stopped root left alone.", L" %lld stopped roots left alone."), (long long)st.roots_stopped);
        if (st.errors)
            say(j, TN(st.errors, L" %lld file or folder could not be read; the log says which.",
                      L" %lld files or folders could not be read; the log says which."),
                (long long)st.errors);
        /* Damaged files and missing roots each get a question after the scan. */
        j->n_problems = st.errors + st.files_damaged + st.roots_missing;
        j->n_tell = st.errors;
        j->in_use = st.files_in_use;
        j->damaged = st.files_damaged;
        j->found_deleted = st.files_deleted;
        j->roots_missing = st.roots_missing;
        break;
    }
    case JOB_BACKUP: {
        if (j->n_many) {
            s = run_backup_many(j);
            break;
        }
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
            wchar_t label[300];
            int place = 0;
            job_dest_label(id, label, 300, &place);
            swprintf(g_job_dest, 400, place ? T(L"Copying starred files to %ls") : T(L"Backing up to %ls"), label);
            s = bd_backup(g_cat, id, 0, &bs, job_log, j);
            g_job_dest[0] = 0;
            j->copied = bs.files_copied;
            j->copied_to = id;
            format_bytes(bs.bytes_copied, size, 64);
            swprintf(j->summary, SUMMARY_LEN, T(L"Backup done: %lld files copied (%ls), %lld already there, %lld failed."),
                     (long long)bs.files_copied, size, (long long)bs.files_already_there, (long long)bs.files_failed);
            if (bs.copies_alongside)
                say(j, T(L" %lld went alongside an older copy (under .versions on the drive): nothing there was overwritten."),
                    (long long)bs.copies_alongside);
            if (bs.files_rehashed) say(j, T(L" %lld changed since the scan and were read again first."), (long long)bs.files_rehashed);
            if (bs.files_in_use) say(j, T(L" %lld in use by another program."), (long long)bs.files_in_use);
            if (bs.files_online_only) say(j, T(L" %lld online-only cloud files not copied."), (long long)bs.files_online_only);
            if (bs.files_no_room > bs.files_too_big) {
                format_bytes(bs.bytes_no_room - bs.bytes_too_big, size, 64);
                say(j, TN(bs.files_no_room - bs.files_too_big, L" The drive is full: %lld file (%ls) did not fit and waits for another drive.",
                          L" The drive is full: %lld files (%ls) did not fit and wait for another drive."),
                    (long long)(bs.files_no_room - bs.files_too_big), size);
            }
            if (bs.files_too_big) {
                format_bytes(bs.bytes_too_big, size, 64);
                say(j, TN(bs.files_too_big, L" %lld file (%ls) is 4 GB or more and cannot go on this drive, because it is FAT32. "
                                            L"Reformat the drive as exFAT or NTFS, or back it up to another drive.",
                          L" %lld files (%ls) are 4 GB or more and cannot go on this drive, because it is FAT32. "
                          L"Reformat the drive as exFAT or NTFS, or back them up to another drive."),
                    (long long)bs.files_too_big, size);
            }
            /* "The drive is full" is asked about only for what did not fit
             * for want of room; files too big for FAT32 are said above. */
            j->no_room = bs.files_no_room - bs.files_too_big;
            j->bytes_no_room = bs.bytes_no_room - bs.bytes_too_big;
            j->in_use = bs.files_in_use;
            /* What did not fit gets its own question after the backup. */
            j->n_problems = bs.files_failed + bs.files_no_room;
            j->n_tell = bs.files_failed;
        }
        break;
    }
    case JOB_VERIFY: {
        bd_check_stats cs;
        memset(&cs, 0, sizeof(cs));
        int days = bd_option_get(g_cat, "verify_days");
        /* After a backup: only the copies never read back since they were written. */
        if (j->just_written) s = bd_media_verify_unread(g_cat, j->media_id, &cs, job_log, j);
        else s = bd_media_verify(g_cat, j->media_id, days, &cs, job_log, j);
        if (j->just_written) {
            /* After a backup: what it wrote (read_back_new). */
            wchar_t name[600];
            media_name(j->media_id, name, 600);
            if (cs.rehashed == 0 && s == BD_OK)
                swprintf(j->summary, SUMMARY_LEN, T(L"Checked what was just written to %ls: every copy on it had been checked already."), name);
            else
                swprintf(j->summary, SUMMARY_LEN, T(L"Checked what was just written to %ls: %lld copies, %lld good, %lld damaged, %lld missing.%ls"),
                         name, (long long)cs.rehashed, (long long)cs.ok, (long long)cs.bad, (long long)cs.missing,
                         cs.bad || cs.missing ? T(L" The next backup to this drive writes them again.") : L"");
        } else if (cs.rehashed == 0 && s == BD_OK)
            swprintf(j->summary, SUMMARY_LEN, T(L"Check done: every copy on the drive was checked within the last %d days."), days);
        else
            swprintf(j->summary, SUMMARY_LEN, T(L"Check done: %lld copies checked, %lld good, %lld damaged, %lld missing.%ls"),
                     (long long)cs.rehashed, (long long)cs.ok, (long long)cs.bad, (long long)cs.missing,
                     cs.bad || cs.missing ? T(L" The next backup to this drive writes them again.") : L"");
        if (cs.skipped) say(j, T(L" %lld encrypted copies need the passphrase to check."), (long long)cs.skipped);
        j->n_problems = j->n_tell = cs.bad + cs.missing;
        break;
    }
    case JOB_CHECK: {
        bd_check_stats cs;
        memset(&cs, 0, sizeof(cs));
        s = bd_media_check(g_cat, j->media_id, &cs, job_log, j);
        swprintf(j->summary, SUMMARY_LEN, T(L"Check done: %lld copies checked, %lld good, %lld damaged, %lld missing.%ls"),
                 (long long)cs.copies, (long long)cs.ok, (long long)cs.bad,
                 (long long)cs.missing, cs.bad || cs.missing ? T(L" The next backup to this drive writes them again.") : L"");
        if (cs.skipped) say(j, T(L" %lld encrypted copies need the passphrase to check."), (long long)cs.skipped);
        j->n_problems = j->n_tell = cs.bad + cs.missing;
        break;
    }
    case JOB_SHADOW:
        s = run_shadow(j);
        break;
    case JOB_REFRESH:
        /* Saved below, like after every job; then the figures are read. */
        swprintf(j->summary, SUMMARY_LEN, L"%ls", j->saving ? T(L"Your changes are saved.") : T(L"Ready."));
        break;
    case JOB_COMPRESS: {
        bd_compress_stats zs;
        memset(&zs, 0, sizeof(zs));
        s = bd_media_set_compression(g_cat, j->media_id, j->compress_on, &zs, job_log, j);
        wchar_t room[64];
        format_bytes(zs.bytes, size, 64);
        format_bytes(zs.bytes_on_disk, room, 64);
        swprintf(j->summary, SUMMARY_LEN, T(L"Compression %ls: %lld files; their %ls now take %ls on the drive."),
                 j->compress_on ? T(L"turned on") : T(L"turned off"), (long long)zs.files, size, room);
        if (zs.failed)
            say(j, T(L" %lld could not be changed (another program may have them open)."), (long long)zs.failed);
        if (j->compress_on) say(j, T(L" Copies made from now on are compressed too."));
        j->n_problems = j->n_tell = zs.failed;
        break;
    }
    case JOB_RESTORE: {
        bd_restore_stats rs;
        bd_restore_opts ro;
        memset(&rs, 0, sizeof(rs));
        memset(&ro, 0, sizeof(ro));
        ro.deleted_only = j->deleted;
        {
            restore_from rf;
            memset(&rf, 0, sizeof(rf));
            if (!j->version_id && !j->n_picks)
                bd_restore_plan_ex(g_cat, j->source_id, j->rel ? j->rel : "", j->dest, &ro, restore_from_cb, &rf, NULL, NULL);
            for (int i = 0; i < j->n_picks; i++)
                bd_restore_plan_ex(g_cat, j->pick_src[i], j->pick_rel[i] ? j->pick_rel[i] : "",
                                   j->pick_dest && j->pick_dest[i] ? j->pick_dest[i] : j->dest, &ro, restore_from_cb, &rf, NULL, NULL);
            wchar_t *dw = widen(j->dest);
            if (rf.names[0]) swprintf(g_job_dest, 400, T(L"Restoring from %ls into %ls"), rf.names, dw ? dw : L"");
            else swprintf(g_job_dest, 400, T(L"Restoring into %ls"), dw ? dw : L"");
            free(dw);
        }
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
            rs.files_older_version += one.files_older_version;
        }
        g_job_dest[0] = 0;
        format_bytes(rs.bytes_restored, size, 64);
        {
            /* Only what happened: the counts that are not zero. */
            wchar_t *dw = widen(j->dest);
            swprintf(j->summary, SUMMARY_LEN, TN(rs.files_restored, L"Restored %lld file (%ls) into %ls.", L"Restored %lld files (%ls) into %ls."),
                     (long long)rs.files_restored, size, dw ? dw : L"");
            free(dw);
        }
        if (rs.files_already_there)
            say(j, TN(rs.files_already_there, L" %lld was already there.", L" %lld were already there."), (long long)rs.files_already_there);
        if (rs.files_offline)
            say(j, TN(rs.files_offline, L" %lld is on drives that are not plugged in.", L" %lld are on drives that are not plugged in."),
                (long long)rs.files_offline);
        if (rs.files_no_copy)
            say(j, TN(rs.files_no_copy, L" %lld was never backed up.", L" %lld were never backed up."), (long long)rs.files_no_copy);
        if (rs.files_failed) say(j, TN(rs.files_failed, L" %lld failed.", L" %lld failed."), (long long)rs.files_failed);
        if (rs.files_conflict)
            say(j, TN(rs.files_conflict, L" %lld file left alone because another file was already there: nothing was overwritten.",
                      L" %lld files left alone because another file was already there: nothing was overwritten."),
                (long long)rs.files_conflict);
        if (rs.files_older_version)
            say(j, TN(rs.files_older_version,
                      L" %lld file came back as an older version, because its newest contents were never backed up (the log names it).",
                      L" %lld files came back as an older version, because their newest contents were never backed up (the log names them)."),
                (long long)rs.files_older_version);
        j->offline = rs.files_offline;
        if (rs.files_need_passphrase) say(j, T(L" %lld need the passphrase."), (long long)rs.files_need_passphrase);
        /* Files on drives that are not plugged in wait for them: not a problem. */
        j->n_problems = j->n_tell = rs.files_failed + rs.files_no_copy + rs.files_need_passphrase;
        break;
    }
    }
    j->status = s;
    if (s == BD_ERR_CANCELLED) {
        swprintf(j->summary, SUMMARY_LEN, T(L"Stopped. What was done until then is kept."));
    } else if (s != BD_OK) {
        wchar_t *e = widen(bd_catalog_error(g_cat));
        swprintf(j->summary, SUMMARY_LEN, T(L"Stopped: %ls"), e ? e : T(L"unknown error"));
        free(e);
    }
    if (bd_catalog_save(g_cat) != BD_OK) {
        wchar_t *e = widen(bd_catalog_error(g_cat));
        if (j->status == BD_OK) {
            j->status = BD_ERR_IO;
            swprintf(j->summary, SUMMARY_LEN, T(L"Could not save the database: %ls"), e ? e : T(L"unknown error"));
        } else {
            post_text(WM_APP_LOG, T(L"Could not save the database:"));
            if (e) post_text(WM_APP_LOG, e);
        }
        free(e);
    }
    bd_catalog_set_progress(g_cat, NULL, NULL);
    /* What the window shows of the drives and files, read now while the
     * catalog is still the worker's. A refresh keeps the bar and its line
     * for the job before it. */
    if (!j->skip_figures && !g_session_end) {
        if (j->kind != JOB_REFRESH) post_busy_text(T(L"Updating the figures for your drives and files..."));
        j->figs = read_figures();
    }
    PostMessageW(g_main, WM_APP_DONE, 0, (LPARAM)j);
    return 0;
}

static void update_menus(void);
static void layout(void);

static int g_refocus; /* Stop had the focus when it was hidden */

static HWND focus_now(void)
{
    return GetActiveWindow() == g_main ? GetFocus() : g_focus;
}

static void hide_stop(void)
{
    if (focus_now() == g_stop) g_refocus = 1;
    EnableWindow(g_stop, FALSE);
    ShowWindow(g_stop, SW_HIDE);
}

/* The offer to read back what a backup wrote: shown, or taken away. */
static void show_read_back(int on)
{
    if (!g_readback) return;
    if (!on) g_n_readback = 0;
    int was = (GetWindowLongW(g_readback, GWL_STYLE) & WS_VISIBLE) != 0;
    if (!on && focus_now() == g_readback) g_refocus = 1;
    ShowWindow(g_readback, on ? SW_SHOWNA : SW_HIDE);
    if (was != on) layout();
}

/* The buttons work only with a database open and no job running. */
static void set_busy(int busy)
{
    HWND f = focus_now();
    if (busy && f && f != g_stop && IsChild(g_main, f)) g_focus_job = f;
    if (!busy) hide_stop();
    InterlockedExchange(&g_busy, busy);
    if (!busy && g_main) ShutdownBlockReasonDestroy(g_main);
    for (int i = 0; i < N_BUTTONS; i++) EnableWindow(g_buttons[i], !busy && g_cat);
    if (g_readback) EnableWindow(g_readback, !busy && g_cat);
    int refresh = busy && g_running_kind == JOB_REFRESH; /* a save: it has no Stop */
    int was_shown = g_nodb_open && IsWindowVisible(g_nodb_open);
    if (g_nodb_open) {
        ShowWindow(g_nodb_open, g_cat ? SW_HIDE : SW_SHOW);
        ShowWindow(g_nodb_new, g_cat ? SW_HIDE : SW_SHOW);
        EnableWindow(g_nodb_open, !busy);
        EnableWindow(g_nodb_new, !busy);
        if (was_shown != !g_cat) layout();
    }
    if (busy && !refresh) {
        EnableWindow(g_stop, TRUE);
        ShowWindow(g_stop, SW_SHOW);
    }
    /* While a job runs the header says what it does, with no button. */
    if (g_next_btn) {
        ShowWindow(g_next_btn, (busy && !refresh) || g_next_kind == NEXT_NONE ? SW_HIDE : SW_SHOWNA);
        header_layout();
    }
    update_menus();
    page_buttons_state();
    /* A button that started the job is turned off while it runs: the focus
     * goes to Stop (Esc does the same), and back when the job ends. While
     * a change is saved it stays where it is, and comes back the same way. */
    if (busy) {
        if (!refresh) keep_focus(g_stop);
    }
    else if (g_refocus || !usable(focus_now())) {
        g_refocus = 0;
        if (usable(g_focus_job)) put_focus(g_focus_job);
        else put_focus(g_tab == TAB_DB && IsWindowVisible(g_tree) ? g_tree : g_tabs);
    }
}

/* What each kind of job is doing, for the status bar, for Windows' list of
 * what holds up a shutdown, and for the note after one cut a job short. */
static const wchar_t *g_job_doing[] = {N_(L"Looking for BRODALF drives..."), N_(L"Scanning your backup roots..."),
                                       N_(L"Backing up..."), N_(L"Checking every copy on the drive..."), N_(L"Restoring..."),
                                       N_(L"Checking copies not checked lately..."), N_(L"Reading files that are in use from a shadow copy..."),
                                       N_(L"Compressing the copies on the drive..."), N_(L"Saving your changes and updating the figures...")};

static void start_next_job(void)
{
    if (g_busy || g_ui_depth || !g_queue || g_session_end) return;
    job *j = g_queue;
    g_queue = j->next;
    j->next = NULL;
    InterlockedExchange(&g_stopping, 0);
    g_running_kind = j->kind;
    set_busy(1);
    /* A job of its own ends the offer to read back the last backup; a
     * save of a change made in the window does not. */
    if (j->kind != JOB_REFRESH) show_read_back(0);
    const wchar_t *what = j->kind == JOB_SCAN && j->bitrot      ? T(L"Bit-rot scan: reading every file again...")
                          : j->kind == JOB_COMPRESS && !j->compress_on ? T(L"Making the copies on the drive whole again...")
                          : j->kind == JOB_REFRESH && !j->saving ? T(L"Updating the figures for your drives and files...")
                                                                  : T(g_job_doing[j->kind]);
    wchar_t what_buf[700];
    if (j->kind == JOB_VERIFY && j->just_written) {
        wchar_t name[600];
        media_name(j->media_id, name, 600); /* the worker has not started yet */
        swprintf(what_buf, 700, T(L"Checking what was just written to %ls..."), name);
        what = what_buf;
    }
    /* The job panel says what a job does; a refresh, which it does not
     * show, says so in a word in the status bar. */
    status_state(j->kind != JOB_REFRESH ? L"" : j->saving ? T(L"Saving...") : T(L"Updating the figures..."));
    /* If Windows shuts down or signs out now, its list of what holds that
     * up says what BRODALF is doing (WM_ENDSESSION stops it and saves). */
    wchar_t reason[400];
    if (j->kind == JOB_REFRESH) swprintf(reason, 400, L"%ls", what);
    else swprintf(reason, 400, T(L"%ls Shutting down now stops it; what was done until then is kept."), what);
    ShutdownBlockReasonCreate(g_main, reason);
    /* A refresh shows in the status bar only: the job panel still shows
     * how the last job ended. */
    g_njob_ids = 0;
    if (j->n_many)
        for (int k = 0; k < j->n_many && k < 16; k++) g_job_ids[g_njob_ids++] = j->many[k];
    else if (j->media_id)
        g_job_ids[g_njob_ids++] = j->media_id;
    if (j->kind != JOB_REFRESH) jp_start(what);
    HANDLE h = CreateThread(NULL, 0, worker, j, 0, NULL);
    if (h) {
        if (g_worker_thread) CloseHandle(g_worker_thread);
        g_worker_thread = h;
    } else {
        set_busy(0);
        job_free(j);
    }
}

/* Ask the running job to stop, and drop the ones waiting after it. */
static void stop_job(void)
{
    if (!g_busy || g_running_kind == JOB_REFRESH) return; /* a save is not stopped */
    InterlockedExchange(&g_stopping, 1);
    bd_catalog_cancel(g_cat);
    /* A change waiting to be saved is still saved. */
    job *keep = NULL, **kept = &keep;
    while (g_queue) {
        job *q = g_queue;
        g_queue = q->next;
        q->next = NULL;
        if (q->kind == JOB_REFRESH) {
            q->skip_figures = 0; /* the job that would have read them is gone */
            *kept = q;
            kept = &q->next;
        } else
            job_free(q);
    }
    g_queue = keep;
    if (focus_now() == g_stop) g_refocus = 1;
    EnableWindow(g_stop, FALSE);
    keep_focus(g_tabs);
    update_menus();
    status_state(T(L"Stopping..."));
    jp_stopping();
}

static void enqueue(job *j)
{
    if (!j) return;
    /* One save waiting is enough. Any other job waiting runs after the
     * change and reads the figures itself. */
    if (j->kind == JOB_REFRESH) {
        for (job *q = g_queue; q; q = q->next)
            if (q->kind == JOB_REFRESH) {
                q->saving |= j->saving;
                job_free(j);
                return;
            }
        j->skip_figures = g_queue != NULL;
    } else {
        for (job *q = g_queue; q; q = q->next)
            if (q->kind == JOB_REFRESH) q->skip_figures = 1;
    }
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
                q->then_all |= j->then_all;
                q->from_shadow |= j->from_shadow;
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

/* Save a change made in the window. Saving a large catalog takes seconds,
 * so the worker does it, and then reads the figures the window shows
 * again; until then the window shows the figures from before. It starts
 * once the command that made the change is done, so the rest of the
 * command still has the catalog. If it cannot be saved, the window says
 * so when it is done. */
static bd_status ui_save(void)
{
    if (!g_cat) return BD_OK;
    job *j = new_job(JOB_REFRESH);
    if (!j) return bd_catalog_save(g_cat);
    j->saving = 1;
    g_ui_depth++; /* queued, not started here */
    enqueue(j);
    g_ui_depth--;
    PostMessageW(g_main, WM_APP_KICK, 0, 0);
    return BD_OK;
}

/* ---- Log pane ----------------------------------------------------------- */

/* The log pane holds up to LOG_LIMIT characters (an edit control takes
 * about 32,000 unless told otherwise, and then shows nothing new). Past
 * LOG_TRIM the oldest lines, up to the end of the line at LOG_CUT, make
 * room; the app log file keeps every line. */
#define LOG_LIMIT 2000000
#define LOG_TRIM 900000
#define LOG_CUT 300000

static wchar_t g_log_last[600]; /* the last line, to count it when it comes again */
static int g_log_last_at = -1, g_log_last_end = -1, g_log_last_n;

static void log_append(const wchar_t *text)
{
    int len = GetWindowTextLengthW(g_log);
    if (len > LOG_TRIM) {
        int cut = LOG_CUT;
        wchar_t *all = malloc(((size_t)len + 1) * sizeof(wchar_t));
        if (all) {
            int n = GetWindowTextW(g_log, all, len + 1);
            const wchar_t *nl = n > LOG_CUT ? wcsstr(all + LOG_CUT, L"\r\n") : NULL;
            if (nl) cut = (int)(nl - all) + 2;
            free(all);
        }
        wchar_t note[400];
        swprintf(note, 400, L"%ls\r\n", T(L"(Older lines were taken out of this pane to make room. The app log keeps them all: Help > Show the app log.)"));
        SendMessageW(g_log, EM_SETSEL, 0, cut);
        SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)note);
        len = GetWindowTextLengthW(g_log);
    }
    wchar_t stamp[48] = L"";
    GetTimeFormatEx(fmt_locale(), 0, NULL, NULL, stamp, 48);
    size_t tl = wcslen(text);
    if (len == g_log_last_end && g_log_last_at >= 0 && tl < 600 && wcscmp(text, g_log_last) == 0) {
        /* The same line again, with nothing between: "(x4)" on it. */
        g_log_last_n++;
        size_t cap = tl + 96;
        wchar_t *line = malloc(cap * sizeof(wchar_t));
        if (line) {
            swprintf(line, cap, L"%ls   %ls  (\x00d7%d)\r\n", stamp, text, g_log_last_n);
            SendMessageW(g_log, EM_SETSEL, g_log_last_at, len);
            SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)line);
            free(line);
        }
    } else {
        g_log_last_at = len;
        g_log_last_n = 1;
        if (tl < 600) wcscpy(g_log_last, text);
        else g_log_last[0] = 0;
        SendMessageW(g_log, EM_SETSEL, len, len);
        SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)stamp);
        SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)L"   ");
        SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)text);
        SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
    }
    g_log_last_end = GetWindowTextLengthW(g_log);
    char *u = narrow(text);
    if (u) { bd_applog("%s", u); free(u); }
}

/* The engine's log lines for work done on the window's own thread. */
static void ui_log(void *ctx, const char *message)
{
    (void)ctx;
    wchar_t *w = core_line(message);
    if (w) log_append(w);
    free(w);
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
                 T(L"%ls\n\nAn error report was saved to:\n%ls\n\n"
                 L"To let the BRODALF developers know, post it as a new issue at %ls. "
                 L"The report starts with step-by-step instructions.\n\nShow the report file now?"),
                 what, wpath, url ? url : L"");
    else
        swprintf(msg, 4096, T(L"%ls\n\nThe error report could not be saved."), what);
    if (MessageBoxW(owner, msg, APP_NAME, MB_ICONERROR | (wpath ? MB_YESNO : MB_OK)) == IDYES) {
        wchar_t args[MAX_PATH * 2 + 16];
        swprintf(args, MAX_PATH * 2 + 16, L"/select,\"%ls\"", wpath);
        ShellExecuteW(owner, NULL, L"explorer.exe", args, NULL, SW_SHOWNORMAL);
    }
    free(path);
    free(wpath);
    free(url);
}

static void show_app_log(HWND owner)
{
    wchar_t *w = widen(bd_applog_path());
    if (w) ShellExecuteW(owner, L"open", w, NULL, NULL, SW_SHOWNORMAL);
    free(w);
}

/* What went wrong, in plain words, when it is something that happens: a
 * drive unplugged or full, a file another program holds, a database open
 * in another window, a wrong passphrase. Only what looks like a fault in
 * BRODALF itself (BD_ERR_DB, BD_ERR_NOMEM, or a code it does not know)
 * gets an error report for the developers. what is the engine's own
 * description; drive_gone says the drive a job used went away. */
static void report_status(HWND owner, bd_status s, int drive_gone, const wchar_t *what)
{
    const wchar_t *title = NULL, *advice = NULL;
    PCWSTR icon = TD_WARNING_ICON;
    /* The engine says BD_ERR_DRIVE_GONE when a drive is unplugged or stops
     * answering during a job; job_drive_gone also catches an IO or
     * NOT_FOUND error while the drive's folder is gone. */
    switch (s) {
    case BD_ERR_DRIVE_GONE:
        drive_gone = 1;
        break;
    case BD_ERR_IO:
        title = T(L"BRODALF could not read or write a file or a drive.");
        advice = T(L"A drive may be full, unplugged or failing, or another program may have the file open. What was done until then "
                   L"is kept. Check the drive and try again; the app log says more.");
        break;
    case BD_ERR_NOT_FOUND:
        title = T(L"Something BRODALF needed was not there.");
        advice = T(L"A drive may have been unplugged, or a file or folder moved or renamed. Plug the drive in, or check where it is, "
                   L"and try again.");
        break;
    case BD_ERR_LOCKED:
        title = T(L"The database is open in another BRODALF window.");
        advice = T(L"Use that window, or close it and try again.");
        icon = TD_INFORMATION_ICON;
        break;
    case BD_ERR_EXISTS:
        title = T(L"Something with that name is already there.");
        advice = T(L"BRODALF never overwrites anything. Choose another name or place.");
        icon = TD_INFORMATION_ICON;
        break;
    case BD_ERR_FORMAT:
        title = T(L"This is not a BRODALF database, or a newer BRODALF made it.");
        advice = T(L"Choose a .brodalf file, or update BRODALF to open this one.");
        break;
    case BD_ERR_INVALID:
        title = T(L"BRODALF could not do that with what was chosen.");
        advice = T(L"Check what you chose and try again.");
        break;
    case BD_ERR_PASSPHRASE:
        title = T(L"The passphrase is needed, or the one given is not right.");
        advice = T(L"Enter the passphrase when BRODALF asks for it. The recovery key on your recovery sheet can be used instead.");
        break;
    case BD_ERR_CANCELLED:
        title = T(L"Stopped. What was done until then is kept.");
        icon = TD_INFORMATION_ICON;
        break;
    default:
        break; /* BD_ERR_DB, BD_ERR_NOMEM and codes this window does not know: a fault */
    }
    if (drive_gone && (s == BD_ERR_IO || s == BD_ERR_NOT_FOUND || s == BD_ERR_DRIVE_GONE)) {
        title = T(L"A drive was unplugged or stopped answering during the job.");
        advice = T(L"What was done until then is kept. Plug the drive back in and start the job again: a backup copies only what "
                   L"the drive is still missing.");
        icon = TD_WARNING_ICON;
    }
    if (!title) {
        report_error(owner, what);
        return;
    }
    char *u = narrow(what);
    bd_applog("problem: %s", u ? u : "");
    free(u);
    wchar_t content[3000];
    swprintf(content, 3000, L"%ls%ls%ls", what && *what ? what : L"", what && *what && advice ? L"\n\n" : L"", advice ? advice : L"");
    TASKDIALOG_BUTTON b[1] = {{660, T(L"Show the app log")}};
    TASKDIALOGCONFIG tc;
    memset(&tc, 0, sizeof(tc));
    tc.cbSize = sizeof(tc);
    tc.hwndParent = owner;
    tc.hInstance = g_inst;
    tc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
    tc.dwCommonButtons = TDCBF_CLOSE_BUTTON;
    tc.pszWindowTitle = APP_NAME;
    tc.pszMainIcon = icon;
    tc.pszMainInstruction = title;
    tc.pszContent = content;
    tc.cButtons = 1;
    tc.pButtons = b;
    tc.nDefaultButton = IDCLOSE;
    int pressed = IDCLOSE;
    if (FAILED(TaskDialogIndirect(&tc, &pressed, NULL, NULL))) MessageBoxW(owner, content, APP_NAME, MB_ICONWARNING);
    else if (pressed == 660) show_app_log(owner);
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
    swprintf(what, 512, T(L"BRODALF crashed (exception 0x%08lX in %ls at +0x%llx)."),
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
    /* What the tree paints beside the name (tree_custom_draw). */
    int root;              /* a backup root's own item */
    int missing, inactive; /* roots: not found at the last scan; no longer backed up */
    int64_t size;          /* files: the current version; roots: every live file */
    int64_t files_total, files_available; /* folders */
    int damaged;           /* how many files a bit-rot scan found damaged: in it, or it */
    int starred, encrypted;
    wchar_t *drive;        /* the drive that holds its copies, when it is not plugged in */
    wchar_t *path;         /* roots: where it is */
    wchar_t *tip;          /* all of it in words, for its tooltip and screen readers */
} node_ref;

static void free_node_ref(node_ref *r)
{
    if (!r) return;
    free(r->rel);
    free(r->drive);
    free(r->path);
    free(r->tip);
    free(r);
}

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

/* The words that say a name's state in its tooltip, as its chip, colour
 * and font do in the tree, for screen readers; NULL for a good copy on a
 * plugged-in drive, and for a folder whose count says it. */
static const wchar_t *state_suffix(const bd_node_info *n, const char *label)
{
    switch (n->state) {
    case BD_STATE_AVAILABLE_OLDER: return T(L"changed since backup");
    case BD_STATE_BAD: return T(L"copy damaged");
    case BD_STATE_DELETED: return T(L"deleted");
    case BD_STATE_NO_COPY: return n->is_dir ? NULL : T(L"not backed up");
    case BD_STATE_OFFLINE: return n->is_dir || label ? NULL : T(L"drive not plugged in"); /* else "on <drive>" says it */
    default: return NULL;
    }
}

/* What the tree shows, for the details pane and Help. */
static const wchar_t *tree_legend(void)
{
    return T(L"How the list of files shows each one: its name, then on the right its state and its size.\r\n"
             L"    No state: a good copy is on a drive that is plugged in.\r\n"
             L"    On <drive>, name in grey: its copy is on a drive that is not plugged in.\r\n"
             L"    Not backed up, name in grey italics: it has no copy yet.\r\n"
             L"    Changed since backup: only an older version is backed up.\r\n"
             L"    Copy damaged: the copy on the plugged-in drive is missing or damaged; back up again to fix it.\r\n"
             L"    Deleted, name struck through: gone from its folder; BRODALF still keeps track of its copies.\r\n"
             L"    May be damaged: a bit-rot scan found it changed without its size or time changing (Damaged files...).\r\n"
             L"    3 of 10 available, on a folder: how many files in it have a good copy on a plugged-in drive.\r\n"
             L"    \x2605 after a name: starred, also copied to your extra places. A lock: encrypted on every drive and extra place it is copied to.\r\n"
             L"    Point at a name for all of this in words. A file's versions are in its details.");
}

static void item_text(const bd_node_info *n, const char *name, const char *label, wchar_t *out, size_t cap)
{
    wchar_t *wn = widen(name), *wl = widen(label);
    if (n->is_dir && n->state == BD_STATE_OFFLINE && label) {
        swprintf(out, cap, T(L"%ls   (0 of %lld available, copies on %ls)"), wn, (long long)n->files_total, wl);
    } else if (n->is_dir) {
        swprintf(out, cap, T(L"%ls   (%lld of %lld available)"), wn, (long long)n->files_available, (long long)n->files_total);
    } else if (n->state == BD_STATE_OFFLINE && label) {
        if (n->version_count > 1) swprintf(out, cap, T(L"%ls   v%d  on %ls"), wn, n->version_no, wl);
        else swprintf(out, cap, T(L"%ls   on %ls"), wn, wl);
    } else if (n->version_count > 1) {
        swprintf(out, cap, T(L"%ls   v%d"), wn, n->version_no);
    } else {
        swprintf(out, cap, L"%ls", wn);
    }
    const wchar_t *words = state_suffix(n, label);
    size_t len = wcslen(out);
    if (words && len + 1 < cap) swprintf(out + len, cap - len, L"   - %ls", words);
    free(wn);
    free(wl);
}

/* Starred and encrypted files and folders, marked in the tree: read once
 * each time it is built, as there are few. */
typedef struct { int64_t source_id; char *rel; } mark_key;
typedef struct { mark_key *k; int n; } mark_keys;
static mark_keys g_star_keys, g_enc_keys, g_dmg_keys; /* g_dmg_keys: files a bit-rot scan found damaged */

static int mark_key_cb(void *ctx, const bd_mark_info *m)
{
    mark_keys *s = ctx;
    mark_key *grown = realloc(s->k, sizeof(mark_key) * (size_t)(s->n + 1));
    if (!grown) return 1;
    s->k = grown;
    s->k[s->n].source_id = m->source_id;
    s->k[s->n].rel = xstrdup(m->rel_path ? m->rel_path : "");
    s->n++;
    return 0;
}

static void free_marks(mark_keys *s)
{
    for (int i = 0; i < s->n; i++) free(s->k[i].rel);
    free(s->k);
    s->k = NULL;
    s->n = 0;
}

static int dmg_key_cb(void *ctx, const bd_damaged_info *d)
{
    mark_keys *s = ctx;
    mark_key *grown = realloc(s->k, sizeof(mark_key) * (size_t)(s->n + 1));
    if (!grown) return 1;
    s->k = grown;
    s->k[s->n].source_id = d->source_id;
    s->k[s->n].rel = xstrdup(d->rel_path ? d->rel_path : "");
    s->n++;
    return 0;
}

static void load_marks(void)
{
    free_marks(&g_star_keys);
    free_marks(&g_enc_keys);
    free_marks(&g_dmg_keys);
    if (!g_cat) return;
    bd_list_stars(g_cat, mark_key_cb, &g_star_keys);
    bd_list_encrypt_paths(g_cat, mark_key_cb, &g_enc_keys);
    bd_list_damaged(g_cat, dmg_key_cb, &g_dmg_keys);
}

/* How many damaged files are in the folder rel ("" the whole root), or 1
 * if rel is a damaged file. */
static int damaged_in(int64_t source_id, const char *rel, int is_dir)
{
    if (!rel) rel = "";
    size_t len = strlen(rel);
    int n = 0;
    for (int i = 0; i < g_dmg_keys.n; i++) {
        const mark_key *k = &g_dmg_keys.k[i];
        if (k->source_id != source_id || !k->rel) continue;
        if (!is_dir) n += strcmp(k->rel, rel) == 0;
        else n += len == 0 || (strncmp(k->rel, rel, len) == 0 && k->rel[len] == '/');
    }
    return n;
}

static int marks_has(const mark_keys *s, int64_t source_id, const char *rel)
{
    for (int i = 0; i < s->n; i++)
        if (s->k[i].source_id == source_id && s->k[i].rel && strcmp(s->k[i].rel, rel ? rel : "") == 0) return 1;
    return 0;
}

/* Whether it is starred, set to be encrypted, or found damaged, for the
 * marks the tree paints after its name. */
static void set_marks(node_ref *r)
{
    r->starred = marks_has(&g_star_keys, r->source_id, r->rel);
    r->encrypted = marks_has(&g_enc_keys, r->source_id, r->rel);
    r->damaged = damaged_in(r->source_id, r->rel, r->is_dir);
}

/* In a name's tooltip: a star when starred, [encrypted] when chosen,
 * and what a bit-rot scan found damaged (Damaged files... lists it). */
static void add_marks(wchar_t *text, size_t cap, int64_t source_id, const char *rel, int is_dir)
{
    size_t n = wcslen(text);
    int dmg = damaged_in(source_id, rel, is_dir);
    if (dmg && n < cap) {
        if (is_dir) swprintf(text + n, cap - n, TN(dmg, L"   - %d file may be damaged", L"   - %d files may be damaged"), dmg);
        else swprintf(text + n, cap - n, L"%ls", T(L"   - may be damaged (see Damaged files...)"));
        n = wcslen(text);
    }
    if (marks_has(&g_star_keys, source_id, rel) && n < cap) {
        swprintf(text + n, cap - n, T(L"   \x2605 starred"));
        n = wcslen(text);
    }
    if (marks_has(&g_enc_keys, source_id, rel) && n < cap) swprintf(text + n, cap - n, T(L"   [encrypted]"));
}

/* The tree's icons are Windows' own, as in Explorer: from the file's
 * extension only, so a file need not be there (or be plugged in) to get
 * its icon. Kept by extension, as asking is slow for thousands. */
typedef struct { wchar_t ext[16]; int icon; } ext_icon;
static ext_icon g_ext_icons[96];
static int g_n_ext_icons, g_icon_folder = -1, g_icon_folder_open = -1;

static int shell_icon(const wchar_t *name, DWORD attrs, UINT extra)
{
    SHFILEINFOW sfi;
    memset(&sfi, 0, sizeof(sfi));
    if (!SHGetFileInfoW(name, attrs, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES | extra))
        return -1;
    return sfi.iIcon;
}

static void tree_icons_init(void)
{
    SHFILEINFOW sfi;
    memset(&sfi, 0, sizeof(sfi));
    HIMAGELIST il = (HIMAGELIST)SHGetFileInfoW(L"folder", FILE_ATTRIBUTE_DIRECTORY, &sfi, sizeof(sfi),
                                               SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
    if (!il) return;
    TreeView_SetImageList(g_tree, il, TVSIL_NORMAL);
    g_icon_folder = sfi.iIcon;
    g_icon_folder_open = shell_icon(L"folder", FILE_ATTRIBUTE_DIRECTORY, SHGFI_OPENICON);
    if (g_icon_folder_open < 0) g_icon_folder_open = g_icon_folder;
}

static int file_icon(const wchar_t *name)
{
    const wchar_t *dot = wcsrchr(name, L'.');
    wchar_t ext[16] = L"";
    if (dot && (int)wcslen(dot) < COUNT_OF(ext)) {
        for (int i = 0; dot[i]; i++) ext[i] = (wchar_t)towlower(dot[i]);
        ext[wcslen(dot)] = 0;
    }
    for (int i = 0; i < g_n_ext_icons; i++)
        if (wcscmp(g_ext_icons[i].ext, ext) == 0) return g_ext_icons[i].icon;
    wchar_t probe[24];
    swprintf(probe, COUNT_OF(probe), L"file%ls", ext);
    int icon = shell_icon(probe, FILE_ATTRIBUTE_NORMAL, 0);
    if (g_n_ext_icons < (int)COUNT_OF(g_ext_icons)) {
        wcscpy(g_ext_icons[g_n_ext_icons].ext, ext);
        g_ext_icons[g_n_ext_icons].icon = icon;
        g_n_ext_icons++;
    }
    return icon;
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
    int dir = !ref || ref->is_dir, icon = dir ? g_icon_folder : file_icon(text);
    if (icon >= 0) {
        tv.item.mask |= TVIF_IMAGE | TVIF_SELECTEDIMAGE;
        tv.item.iImage = tv.item.iSelectedImage = icon;
#ifdef TVIF_EXPANDEDIMAGE
        if (dir && g_icon_folder_open >= 0) {
            tv.itemex.mask |= TVIF_EXPANDEDIMAGE;
            tv.itemex.iExpandedImage = g_icon_folder_open;
        }
#endif
    }
    /* A root's name in bold; a deleted file's icon faded. */
    if (ref && (ref->root || ref->state == BD_STATE_DELETED)) {
        tv.item.mask |= TVIF_STATE;
        tv.item.stateMask = TVIS_BOLD | TVIS_CUT;
        tv.item.state = ref->root ? TVIS_BOLD : TVIS_CUT;
    }
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
        wchar_t text[1024];
        item_text(&c->info, c->name, c->label, text, 1024);
        if (r) {
            r->source_id = pref->source_id;
            r->node_id = c->info.node_id;
            r->is_dir = c->info.is_dir;
            r->state = c->info.state;
            r->rel = c->rel;
            c->rel = NULL;
            r->size = c->info.size;
            r->files_total = c->info.files_total;
            r->files_available = c->info.files_available;
            if (c->label && *c->label && c->info.state == BD_STATE_OFFLINE) r->drive = widen(c->label);
            set_marks(r);
            add_marks(text, 1024, r->source_id, r->rel, r->is_dir);
            r->tip = _wcsdup(text);
        }
        wchar_t *name = widen(c->name);
        insert_item(parent, name ? name : L"", r, c->info.is_dir && (c->info.files_total > 0 || 1));
        free(name);
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
    load_marks();
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
        if (r) {
            r->source_id = l.items[i].source_id;
            r->is_dir = 1;
            r->root = 1;
            r->state = l.items[i].state;
            r->rel = xstrdup("");
            r->missing = l.items[i].missing;
            r->inactive = !l.items[i].active;
            r->size = l.items[i].bytes_total;
            r->files_total = l.items[i].files_total;
            r->files_available = l.items[i].files_available;
            r->path = widen(l.paths[i]);
            if (l.labels[i] && *l.labels[i] && l.items[i].state == BD_STATE_OFFLINE) r->drive = widen(l.labels[i]);
            set_marks(r);
        }
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
                 l.items[i].missing ? T(L"   - not found: unplugged, moved or renamed?")
                 : !l.items[i].active ? T(L"   - no longer backed up") : L"");
        add_marks(text, 1024, l.items[i].source_id, "", 1);
        if (r) r->tip = _wcsdup(text);
        wchar_t *wname = widen(l.names[i]);
        HTREEITEM h = insert_item(TVI_ROOT, wname ? wname : L"", r, 1);
        free(wname);
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
    swprintf(buf, 512, c->is_current ? T(L"v%d (current)") : T(L"v%d"), c->version_no);
    it.pszText = buf;
    SendMessageW(g_list, LVM_INSERTITEMW, 0, (LPARAM)&it);
    it.mask = LVIF_TEXT;

    wchar_t *where = c->media_id ? widen(c->media_label) : NULL, where_buf[300];
    swprintf(where_buf, 300, L"%ls%ls", where ? where : c->media_id ? L"" : T(L"no copy yet"),
             c->media_id && c->encrypted ? T(L" (encrypted)") : L"");
    it.iSubItem = 1;
    it.pszText = where_buf;
    SendMessageW(g_list, LVM_SETITEMTEXTW, *row, (LPARAM)&it);
    free(where);

    if (!c->media_id) swprintf(buf, 512, L"");
    else if (strcmp(c->copy_state, "ok") != 0) swprintf(buf, 512, strcmp(c->copy_state, "missing") == 0 ? T(L"Missing from drive") : T(L"Damaged"));
    else if (c->media_lost) swprintf(buf, 512, T(L"Drive marked lost or broken: does not count"));
    else if (!c->read_back)
        swprintf(buf, 512, c->connected ? T(L"Good, plugged in; not checked yet") : T(L"Good, drive not plugged in; not checked yet"));
    else swprintf(buf, 512, c->connected ? T(L"Good, plugged in") : T(L"Good, drive not plugged in"));
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
    case BD_STATE_AVAILABLE: return T(L"Available: a good copy is on a drive that is plugged in.");
    case BD_STATE_AVAILABLE_OLDER: return T(L"Changed since the last backup. Only an older version is on a plugged-in drive.");
    case BD_STATE_OFFLINE: return T(L"Backed up, but the drive is not plugged in.");
    case BD_STATE_NO_COPY: return T(L"Not backed up yet.");
    case BD_STATE_BAD: return T(L"The copy on the plugged-in drive is missing or damaged. Back up again to fix it.");
    case BD_STATE_DELETED: return T(L"Deleted from your folder. BRODALF still tracks its copies.");
    case BD_STATE_PARTIAL: return T(L"Some files inside are available.");
    }
    return L"";
}

/* A copy of what bd_list_media reports, kept past the callback. */
typedef struct {
    int64_t id;
    char label[256], kind[16], root[512], location[256];
    int connected, encrypted, has_hw;
    int hide_names; /* encrypted, and the names of the files and folders on it are hidden too */
    int64_t last_seen_ms, hw_read_ms, total, freeb, copies, added_ms, space_ms;
    int64_t lost_ms, oldest_check_ms, oldest_unread_ms, bytes_held, reserve_bytes;
    char lost_why[16];
    int check_due;
    int secondary;       /* an extra place for starred files */
    int64_t limit_bytes; /* its size limit; 0 none */
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
    d->hide_names = m->hide_names;
    d->last_seen_ms = m->last_seen_ms;
    d->total = m->total_bytes;
    d->freeb = m->free_bytes;
    d->copies = m->copies;
    d->added_ms = m->added_ms;
    d->space_ms = m->space_ms;
    d->lost_ms = m->lost_ms;
    snprintf(d->lost_why, sizeof(d->lost_why), "%s", m->lost_why ? m->lost_why : "");
    d->oldest_check_ms = m->oldest_check_ms;
    d->oldest_unread_ms = m->oldest_unread_ms;
    d->check_due = m->check_due;
    d->secondary = m->secondary;
    d->limit_bytes = m->limit_bytes;
    d->bytes_held = m->bytes_held;
    d->reserve_bytes = m->reserve_bytes;
    if (m->hw) { d->has_hw = 1; d->hw = *m->hw; d->hw_read_ms = m->hw_read_ms; }
    return 0;
}

static drive_list load_drives(void);

/* "Where it is kept": the places other drives already have, so two drives
 * in the same place get the same words (and count as one place). */
static void fill_places_combo(HWND combo, const wchar_t *current)
{
    drive_list dl = load_drives();
    for (int i = 0; i < dl.n; i++) {
        if (!dl.items[i].location[0]) continue;
        wchar_t *w = widen(dl.items[i].location);
        if (w && *w && SendMessageW(combo, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)w) == CB_ERR)
            SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)w);
        free(w);
    }
    free(dl.items);
    SendMessageW(combo, CB_SETCUEBANNER, 0, (LPARAM)T(L"For example: desk drawer at home"));
    SetWindowTextW(combo, current ? current : L"");
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

/* ---- Figures for the window ----------------------------------------------
 *
 * What a backup to each plugged-in drive would copy, where each drive and
 * extra place stands, how many files are short of their copies and how far
 * each full copy has got. On a large catalog these take seconds, so the
 * worker reads them at the end of every job (and of every save of a change
 * made in the window), while the catalog is still its own, and the window
 * shows what it read. The window never reads them itself.
 *
 * Why not a second connection that reads them while the window is idle:
 * a catalog can be open only once (its lock, a named mutex, lets one
 * BRODALF use it at a time, and opening it again says it is in use), and
 * an encrypted catalog exists decrypted only in this connection. So the
 * one connection is shared, and a job owns it while it runs, as before. */
typedef struct {
    int tried_pv, has_pv, has_ms, has_ps;
    bd_backup_preview_info pv; /* a plugged-in drive: what a backup to it would copy */
    bd_media_status ms;        /* a drive: where it stands */
    bd_place_status ps;        /* an extra place: its starred files */
} drive_fig;

typedef struct figures {
    drive_list dl; /* the drives as they were then */
    drive_fig *d;  /* d[i] is for dl.items[i] */
    int has_risk, has_sets, n_sets;
    bd_risk_stats risk;
    bd_set_progress sets[MAX_SET_BARS];
    wchar_t set_on[MAX_SET_BARS][160]; /* the drives each full copy is on: "Blue and Red" */
    int64_t n_damaged;                 /* files a bit-rot scan found damaged (Damaged files...) */
    bd_set_drive sd[96];               /* the drives each full copy is on, and how many files */
    int nsd;
    int has_target;
    bd_target target;                  /* the copies and places wanted */
    int n_roots, n_missing;            /* backup roots in use; those the last scan could not find */
    int64_t missing_id[4];
    wchar_t missing_name[4][200];
    int64_t last_scan_ms;
} figures;

/* The full copies, read during a backup (post_live_sets). */
typedef struct {
    int n;
    bd_set_progress sets[MAX_SET_BARS];
    wchar_t on[MAX_SET_BARS][160];
    bd_set_drive sd[96];
    int nsd;
} sets_msg;

static int count_damaged_cb(void *ctx, const bd_damaged_info *d)
{
    (void)d;
    ++*(int64_t *)ctx;
    return 0;
}

/* On the worker thread: the backup roots, those the last scan could not
 * find, and when they were last scanned. */
static int fig_source_cb(void *ctx, const bd_source_info *s)
{
    figures *f = ctx;
    if (!s->active) return 0;
    f->n_roots++;
    if (s->last_scan_ms > f->last_scan_ms) f->last_scan_ms = s->last_scan_ms;
    if (s->missing) {
        if (f->n_missing < 4) {
            wchar_t *w = widen(s->name ? s->name : "");
            f->missing_id[f->n_missing] = s->source_id;
            swprintf(f->missing_name[f->n_missing], 200, L"%ls", w ? w : L"");
            free(w);
        }
        f->n_missing++;
    }
    return 0;
}

/* On the worker thread: which drives each full copy is on, by name. */
static void read_set_names(wchar_t (*on)[160], const drive_list *dl, bd_set_drive *sd, int *nsd)
{
    for (int i = 0; i < MAX_SET_BARS; i++) on[i][0] = 0;
    int n = 0;
    *nsd = 0;
    if (bd_backup_set_drives(g_cat, sd, 96, &n) != BD_OK) return;
    if (n > 96) n = 96;
    *nsd = n;
    for (int k = 1; k <= MAX_SET_BARS; k++) {
        int total = 0, seen = 0;
        for (int i = 0; i < n; i++)
            if (sd[i].set == k) total++;
        for (int i = 0; i < n; i++) {
            if (sd[i].set != k) continue;
            const char *label = "?";
            for (int d = 0; d < dl->n; d++)
                if (dl->items[d].id == sd[i].media_id) label = dl->items[d].label;
            wchar_t *w = widen(label);
            size_t len = wcslen(on[k - 1]);
            const wchar_t *sep = seen == 0 ? L"" : seen == total - 1 ? T(L" and ") : L", ";
            swprintf(on[k - 1] + len, 160 - len, L"%ls%ls", sep, w ? w : L"");
            free(w);
            seen++;
        }
    }
}

static figures *g_figs; /* the window's: the last ones read */

static void figures_free(figures *f)
{
    if (!f) return;
    free(f->dl.items);
    free(f->d);
    free(f);
}

/* On the worker thread, with the catalog its own. */
static figures *read_figures(void)
{
    figures *f = calloc(1, sizeof(*f));
    if (!f) return NULL;
    f->dl = load_drives();
    if (f->dl.n) {
        f->d = calloc((size_t)f->dl.n, sizeof(drive_fig));
        if (!f->d) {
            figures_free(f);
            return NULL;
        }
    }
    for (int i = 0; i < f->dl.n; i++) {
        const drive_snap *d = &f->dl.items[i];
        drive_fig *g = &f->d[i];
        if (strcmp(d->kind, "drive") != 0) continue; /* a cloud account from an earlier version */
        if (d->secondary) {
            g->has_ps = bd_place_status_get(g_cat, d->id, &g->ps) == BD_OK;
        } else if (d->connected && !d->lost_ms) {
            g->tried_pv = 1;
            g->has_pv = bd_backup_preview(g_cat, d->id, 0, &g->pv) == BD_OK;
        }
        g->has_ms = bd_media_status_get(g_cat, d->id, &g->ms) == BD_OK;
    }
    f->has_risk = bd_list_at_risk(g_cat, 0, NULL, NULL, &f->risk) == BD_OK;
    f->has_sets = bd_backup_sets(g_cat, f->sets, MAX_SET_BARS, &f->n_sets) == BD_OK;
    if (f->has_sets) read_set_names(f->set_on, &f->dl, f->sd, &f->nsd);
    bd_list_damaged(g_cat, count_damaged_cb, &f->n_damaged);
    f->has_target = bd_target_get(g_cat, &f->target) == BD_OK;
    bd_list_sources(g_cat, fig_source_cb, f);
    return f;
}

/* On the worker thread, during a backup, right after copies were recorded:
 * the full copies as they are now, for the bars. Read at most once a
 * second, since a backup of many small files records often. */
static void post_live_sets(void)
{
    static ULONGLONG last;
    ULONGLONG now = GetTickCount64();
    if (last && now - last < 1000) return;
    last = now;
    sets_msg *m = calloc(1, sizeof(*m));
    if (!m) return;
    if (bd_backup_sets(g_cat, m->sets, MAX_SET_BARS, &m->n) != BD_OK) {
        free(m);
        return;
    }
    drive_list dl = load_drives();
    read_set_names(m->on, &dl, m->sd, &m->nsd);
    free(dl.items);
    if (!PostMessageW(g_main, WM_APP_SETS, 0, (LPARAM)m)) free(m);
}

/* What was read for a drive, or NULL if it was not there then (it is read
 * at the end of the job or save running now). */
static const drive_fig *fig_of(int64_t id)
{
    if (!g_figs) return NULL;
    for (int i = 0; i < g_figs->dl.n; i++)
        if (g_figs->dl.items[i].id == id) return &g_figs->d[i];
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
        return T(L"The drive says it is failing. Set up another drive and back up to it; then mark this one broken.");
    if (h->percent_used >= 90) return T(L"The drive is nearly worn out. Plan to set up another drive.");
    if (strcmp(h->health, "warning") == 0 || h->pending_sectors > 0 || h->uncorrectable_sectors > 0)
        return T(L"The drive reports problems reading some of its sectors. Plan to set up another drive.");
    return NULL;
}

/* "WD Elements 25A3, 4.0 TB, USB" and the rest of what the disk said. */
static void add_drive_lines(wtext *t, const drive_snap *d, const wchar_t *indent)
{
    wchar_t when[64], a[64], b[64];
    if (strcmp(d->kind, "drive") != 0) return; /* a cloud account from an earlier version: the heading says so */
    if (!d->has_hw) {
        wadd(t, T(L"%lsNo hardware details yet; they are read the next time the drive is plugged in.\r\n"), indent);
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
    if (*line) wadd(t, T(L"%lsDisk: %ls\r\n"), indent, line);
    if (*serial) wadd(t, T(L"%lsSerial number %ls%ls%ls\r\n"), indent, serial, *fw ? T(L", firmware ") : L"", fw);
    if (*vser) wadd(t, T(L"%lsVolume \"%ls\" (%ls, serial %ls)\r\n"), indent, *vname ? vname : T(L"no name"), fs, vser);
    if (h->smart) {
        wadd(t, T(L"%lsHealth: %ls"), indent, *health ? health : T(L"unknown"));
        if (h->temperature_c >= 0) wadd(t, L", %d \x00B0" L"C", h->temperature_c);
        if (h->power_on_hours >= 0) wadd(t, T(L", %lld hours powered on"), (long long)h->power_on_hours);
        if (h->power_cycles >= 0) wadd(t, T(L", %lld power cycles"), (long long)h->power_cycles);
        if (h->percent_used >= 0) wadd(t, T(L", %d%% worn"), h->percent_used);
        wadd(t, L"\r\n");
        if (h->reallocated_sectors > 0 || h->pending_sectors > 0 || h->uncorrectable_sectors > 0)
            wadd(t, T(L"%ls%lld reallocated, %lld waiting to be reallocated, %lld unreadable sectors\r\n"), indent,
                 (long long)(h->reallocated_sectors > 0 ? h->reallocated_sectors : 0),
                 (long long)(h->pending_sectors > 0 ? h->pending_sectors : 0),
                 (long long)(h->uncorrectable_sectors > 0 ? h->uncorrectable_sectors : 0));
    } else if (h->temperature_c >= 0) {
        wadd(t, T(L"%lsTemperature %d \x00B0" L"C\r\n"), indent, h->temperature_c);
    }
    if (*note) wadd(t, L"%ls(%ls)\r\n", indent, note);
    const wchar_t *advice = replace_advice(d);
    if (advice) wadd(t, L"%ls%ls\r\n", indent, advice);
    format_time_ms(d->hw_read_ms, when, 64);
    wadd(t, T(L"%lsRead from the drive %ls\r\n"), indent, when);
    if (d->total > 0) {
        format_bytes(d->freeb, a, 64);
        format_bytes(d->total, b, 64);
        wadd(t, T(L"%ls%ls free of %ls when last seen\r\n"), indent, a, b);
    }
    free(vendor); free(model); free(bus); free(serial); free(fw); free(vname); free(vser); free(fs); free(health); free(note);
}

/* One drive: its name, where it is kept, whether it is plugged in. */
static void add_drive_heading(wtext *t, const drive_snap *d, const wchar_t *indent)
{
    wchar_t when[64];
    wchar_t *label = widen(d->label), *loc = widen(d->location), *root = widen(d->root);
    format_time_ms(d->last_seen_ms, when, 64);
    wadd(t, L"%ls%ls%ls\r\n", indent, label,
         d->hide_names ? T(L" (encrypted, names hidden)") : d->encrypted ? T(L" (encrypted)") : L"");
    if (*loc) wadd(t, T(L"%ls    Kept in: %ls\r\n"), indent, loc);
    if (strcmp(d->kind, "drive") != 0) wadd(t, T(L"%ls    Cloud account from an earlier version (not supported in this version)\r\n"), indent);
    else if (d->connected) wadd(t, T(L"%ls    Plugged in now at %ls\r\n"), indent, root);
    else if (*root) wadd(t, T(L"%ls    Not plugged in. Last seen %ls at %ls\r\n"), indent, when, root);
    else wadd(t, T(L"%ls    Not plugged in. Last seen %ls\r\n"), indent, when);
    if (d->lost_ms) {
        wchar_t lost[64];
        format_time_ms(d->lost_ms, lost, 64);
        wadd(t, T(L"%ls    Marked %ls %ls: its copies no longer count and nothing is backed up to it\r\n"), indent,
             strcmp(d->lost_why, "broken") == 0 ? T(L"broken") : T(L"lost"), lost);
    } else if (strcmp(d->kind, "drive") == 0 && d->copies) {
        wchar_t held[64], checked[64];
        format_bytes(d->bytes_held, held, 64);
        /* A copy never read back is not "read back" when it was written. */
        if (d->oldest_unread_ms) {
            format_time_ms(d->oldest_unread_ms, checked, 64);
            wadd(t, T(L"%ls    Holds %ls of good copies; some were never checked since they were written (the oldest %ls)%ls\r\n"),
                 indent, held, checked, d->check_due ? T(L" (due for a check)") : L"");
        } else {
            format_time_ms(d->oldest_check_ms, checked, 64);
            wadd(t, T(L"%ls    Holds %ls of good copies; the oldest was last checked %ls%ls\r\n"), indent, held, checked,
                 d->check_due ? T(L" (due for a check)") : L"");
        }
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

/* For one file whose newest version has no good copy anywhere: which
 * version a restore gives instead. Empty when it gives the file as it is. */
static void version_note(const node_ref *r, wchar_t *out, size_t cap)
{
    out[0] = 0;
    if (!g_cat || !r || r->is_dir || !r->node_id) return;
    copy_list cl = {NULL, 0, 0};
    bd_list_copies(g_cat, r->node_id, copy_snap_cb, &cl);
    int cur = 0, best = 0;
    int64_t cur_size = 0, best_size = 0, best_mtime = 0;
    for (int i = 0; i < cl.n; i++) {
        const bd_copy_info *c = &cl.items[i].c;
        if (c->is_current) {
            cur = c->version_no;
            cur_size = c->size;
        }
        if (c->media_id && !c->media_lost && strcmp(cl.items[i].state, "ok") == 0 && c->version_no > best) {
            best = c->version_no;
            best_size = c->size;
            best_mtime = c->mtime_ns;
        }
    }
    free(cl.items);
    if (!cur || !best || best >= cur) return;
    wchar_t a[64], b[64], m[64];
    format_bytes(best_size, a, 64);
    format_bytes(cur_size, b, 64);
    format_ns(best_mtime, m, 64);
    swprintf(out, cap, T(L"You will get version %d, as it was on %ls (%ls). The newest version, %d (%ls), has no copy yet: back it up first to keep it."),
             best, m, a, cur, b);
}

/* The full name of what is selected: its backup root, then its path, the
 * way Windows writes paths. */
static void node_full_name(HTREEITEM h, const node_ref *r, wchar_t *out, size_t cap)
{
    HTREEITEM root = h, up;
    while (root && (up = TreeView_GetParent(g_tree, root)) != NULL) root = up;
    wchar_t name[300] = L"";
    TVITEMW it;
    memset(&it, 0, sizeof(it));
    it.mask = TVIF_TEXT;
    it.hItem = root;
    it.pszText = name;
    it.cchTextMax = 300;
    if (root) SendMessageW(g_tree, TVM_GETITEMW, 0, (LPARAM)&it);
    wchar_t *rel = widen(r->rel ? r->rel : "");
    if (rel && *rel) swprintf(out, cap, L"%ls\\%ls", name, rel);
    else swprintf(out, cap, L"%ls", name);
    free(rel);
    for (wchar_t *c = out; *c; c++)
        if (*c == L'/') *c = L'\\';
}

/* The versions list's columns as wide as what they hold. */
static void versions_fit_cols(void)
{
    for (int c = 0; c < 6; c++) ListView_SetColumnWidth(g_list, c, LVSCW_AUTOSIZE_USEHEADER);
}

static void show_detail(void)
{
    SendMessageW(g_list, LVM_DELETEALLITEMS, 0, 0);
    clear_copy_rows();
    if (!g_cat) { SetWindowTextW(g_detail, L""); return; }
    node_ref *r = item_ref(TreeView_GetSelection(g_tree));
    if (!r) {
        wchar_t text[2400];
        swprintf(text, 2400, L"%ls\r\n\r\n%ls", T(L"Select a file or folder to see its versions and which drives hold them."), tree_legend());
        SetWindowTextW(g_detail, text);
        return;
    }
    wchar_t full[1400];
    node_full_name(TreeView_GetSelection(g_tree), r, full, 1400);
    wtext t = {NULL, 0, 0};
    wadd(&t, L"%ls\r\n%ls\r\n", full, state_words(r->state));
    {
        const char *mr = r->rel ? r->rel : "";
        int st = bd_starred(g_cat, r->source_id, mr), en = bd_encrypt_marked(g_cat, r->source_id, mr);
        if (st)
            wadd(&t, L"%ls\r\n", st == 1 ? T(L"Starred: also backed up to your extra places")
                                         : T(L"Starred with its folder: also backed up to your extra places"));
        if (en)
            wadd(&t, L"%ls\r\n", en == 1 ? T(L"Encrypted on every drive and extra place it is copied to")
                                         : T(L"Encrypted with its folder, on every drive and extra place it is copied to"));
    }
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
            wadd(&t, T(L"\r\nFile\r\n    Size %ls, modified %ls\r\n    Version %d of %d, first seen %ls\r\n    Checksum (BLAKE3) %ls\r\n"),
                 size, mod, current, versions, seen, hash);
            free(hash);
            break;
        }
        /* Where the current version is. */
        int any = 0;
        wadd(&t, T(L"\r\nCopies of this version\r\n"));
        for (int i = 0; i < cl.n; i++) {
            const bd_copy_info *c = &cl.items[i].c;
            if (!c->is_current || !c->media_id) continue;
            const drive_snap *d = find_drive(&dl, c->media_id);
            if (!d) continue;
            any = 1;
            add_drive_heading(&t, d, L"    ");
            wchar_t *path = widen(cl.items[i].path);
            wadd(&t, T(L"        Copy %ls, at %ls\r\n"),
                 strcmp(cl.items[i].state, "ok") == 0 ? T(L"good") : strcmp(cl.items[i].state, "missing") == 0 ? T(L"missing") : T(L"damaged"),
                 path);
            free(path);
            add_drive_lines(&t, d, L"        ");
        }
        if (!any) wadd(&t, T(L"    None yet. Back up to put a copy on a drive.\r\n"));
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
            if (!header) { wadd(&t, T(L"\r\nOlder versions are also on\r\n")); header = 1; }
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
        wadd(&t, T(L"\r\nRight-click a version below to restore it or to open it on its drive.\r\n"));
        free(cl.items);
        free(dl.items);
        versions_fit_cols();
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
    if (!finished_ms || strcmp(status, "running") == 0) return T(L"did not finish (BRODALF was closed during it)");
    if (strcmp(status, "done") == 0) return T(L"finished");
    if (strcmp(status, "partial") == 0) return T(L"finished with problems");
    if (strcmp(status, "stopped") == 0) return T(L"stopped");
    return T(L"failed");
}

/* Where a drive stands, from the catalog: what is on it as it is now, what
 * is behind, its last backup and its last check. */
static void add_drive_status(wtext *t, int64_t id, const wchar_t *indent)
{
    const drive_fig *g = fig_of(id);
    if (!g) {
        wadd(t, T(L"%lsWhere it stands: being read...\r\n"), indent);
        return;
    }
    if (!g->has_ms) {
        wadd(t, T(L"%lsWhere it stands could not be read from the database.\r\n"), indent);
        return;
    }
    bd_media_status ms = g->ms;
    wchar_t a[64], when[64];
    wadd(t, T(L"%lsWhere it stands\r\n"), indent);
    format_bytes(ms.bytes_current, a, 64);
    wadd(t, T(L"%ls    %lld files as they are now (%ls)\r\n"), indent, (long long)ms.files_current, a);
    if (ms.files_behind) {
        format_bytes(ms.bytes_behind, a, 64);
        wadd(t, T(L"%ls    %lld changed since (%ls): only an older version is on it\r\n"), indent, (long long)ms.files_behind, a);
    }
    if (ms.files_not_here) {
        format_bytes(ms.bytes_not_here, a, 64);
        wadd(t, T(L"%ls    %lld not on it at all (%ls)\r\n"), indent, (long long)ms.files_not_here, a);
    }
    if (ms.deleted_kept)
        wadd(t, T(L"%ls    %lld deleted from their folders, still kept on it\r\n"), indent, (long long)ms.deleted_kept);
    if (ms.old_versions) {
        format_bytes(ms.old_bytes, a, 64);
        wadd(t, T(L"%ls    %lld older versions kept on it (%ls)\r\n"), indent, (long long)ms.old_versions, a);
    }
    if (ms.last_backup_ms) {
        format_time_ms(ms.last_backup_ms, when, 64);
        wadd(t, T(L"%ls    Last backup %ls: %ls, %lld files copied"), indent, when,
             job_end_words(ms.last_backup_status, ms.last_backup_end_ms), (long long)ms.last_backup_files);
        if (ms.last_backup_failed) wadd(t, T(L", %lld failed"), (long long)ms.last_backup_failed);
        wadd(t, L"\r\n");
    } else
        wadd(t, T(L"%ls    Never backed up to\r\n"), indent);
    if (ms.last_check_ms) {
        format_time_ms(ms.last_check_ms, when, 64);
        /* The kind is "check" (or "quick check", "full check" before 0.7.0) or "read back". */
        wadd(t, strcmp(ms.last_check_kind, "read back") == 0 ? T(L"%ls    Last check of the copies not checked lately %ls: %ls\r\n") : T(L"%ls    Last check %ls: %ls\r\n"),
             indent, when, job_end_words(ms.last_check_status, ms.last_check_ms));
    } else
        wadd(t, T(L"%ls    Its copies were never checked (Check drive...)\r\n"), indent);
    if (ms.copies_bad)
        wadd(t, T(L"%ls    %lld copies were missing or damaged when last checked: the next backup to it writes them again\r\n"), indent,
             (long long)ms.copies_bad);
    if (ms.copies_never_read)
        wadd(t, T(L"%ls    %lld copies were never checked since they were written (Check drive...)\r\n"), indent,
             (long long)ms.copies_never_read);
}

static const drive_snap *g_edit_drive;
static wchar_t g_edit_name[256], g_edit_location[256];

static INT_PTR CALLBACK drive_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    INT_PTR themed;
    if (theme_dialog(dlg, msg, wp, lp, &themed)) return themed;
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG: {
        const drive_snap *d = g_edit_drive;
        wtext t = {NULL, 0, 0};
        wchar_t added[64], copies[32];
        add_drive_heading(&t, d, L"");
        format_time_ms(d->added_ms, added, 64);
        swprintf(copies, 32, L"%lld", (long long)d->copies);
        wadd(&t, T(L"    Set up %ls, %ls copies recorded\r\n\r\n"), added, copies);
        if (strcmp(d->kind, "drive") == 0) {
            add_drive_status(&t, d->id, L"");
            wadd(&t, L"\r\n");
        }
        /* An extra place is a folder: the disk under it says nothing about it. */
        if (!d->secondary) add_drive_lines(&t, d, L"");
        if (d->lost_ms && d->secondary)
            wadd(&t, T(L"\r\nBRODALF no longer copies starred files there. Your full copies do not change: extra places never count "
                     L"toward them. If it is back, choose Found it.\r\n"));
        else if (d->lost_ms)
            wadd(&t, T(L"\r\nIts copies still restore if it turns up, but they no longer count toward your full copies, so the "
                     L"files it held are backed up again to your other drives. If you find it, choose Found it.\r\n"));
        SetDlgItemTextW(dlg, IDC_DRIVE_INFO, t.p ? t.p : L"");
        SetDlgItemTextW(dlg, IDC_DRIVE_LOST, d->lost_ms ? T(L"Found it") : d->secondary ? T(L"It is gone...") : T(L"Mark lost or broken..."));
        if (d->secondary) {
            /* An extra place, not a drive: its own words. */
            SetWindowTextW(dlg, T(L"Extra place details"));
            SetDlgItemTextW(dlg, IDC_DRIVE_NAME_TEXT, T(L"Description (at least two words, like \"Dropbox folder on the laptop\"):"));
            SetDlgItemTextW(dlg, IDC_DRIVE_LOC_TEXT, T(L"Where it is (optional):"));
        }
        EnableWindow(GetDlgItem(dlg, IDC_DRIVE_LOST), strcmp(d->kind, "drive") == 0);
        free(t.p);
        wchar_t *name = widen(d->label), *loc = widen(d->location);
        SetDlgItemTextW(dlg, IDC_DRIVE_NAME, name);
        fill_places_combo(GetDlgItem(dlg, IDC_DRIVE_LOCATION), loc);
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

/* A drive is needed and none is plugged in: if none was ever set up, say
 * how to set one up rather than to plug it in. */
static void no_drive_message(void)
{
    drive_list dl = load_drives();
    int any = 0;
    for (int i = 0; i < dl.n; i++)
        if (strcmp(dl.items[i].kind, "drive") == 0 && !dl.items[i].secondary) any = 1;
    free(dl.items);
    MessageBoxW(g_main, any ? T(L"Plug in a backup drive first.")
                            : T(L"You have no backup drives yet. Set one up with Local backups > Set up a drive..., or Back up > Set "
                                L"up an external or removable drive..."),
                APP_NAME, MB_ICONINFORMATION);
}

static void cmd_drives(void)
{
    drive_list dl = load_drives();
    if (!dl.n) {
        no_drive_message();
        free(dl.items);
        return;
    }
    HMENU m = CreatePopupMenu();
    for (int i = 0; i < dl.n; i++) {
        const drive_snap *d = &dl.items[i];
        wchar_t *label = widen(d->label), *loc = widen(d->location), text[600];
        swprintf(text, 600, L"%ls%ls%ls%ls", label, *loc ? L"  (" : L"", loc, *loc ? L")" : L"");
        if (d->secondary) wcsncat(text, T(L"  - extra place for starred files"), 599 - wcslen(text));
        if (strcmp(d->kind, "drive") != 0) wcsncat(text, T(L"  - Cloud account from an earlier version (not supported in this version)"), 599 - wcslen(text));
        else if (d->connected) wcsncat(text, T(L"  - plugged in"), 599 - wcslen(text));
        if (d->lost_ms) wcsncat(text, strcmp(d->lost_why, "broken") == 0 ? T(L"  - marked broken") : T(L"  - marked lost"), 599 - wcslen(text));
        else if (replace_advice(d)) wcsncat(text, T(L"  - its health data warns"), 599 - wcslen(text));
        else if (d->check_due) wcsncat(text, T(L"  - due for a check"), 599 - wcslen(text));
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

/* Set a control's text; 1 if it changed. */
static int set_text_changed(HWND h, const wchar_t *text)
{
    wchar_t old[4096];
    GetWindowTextW(h, old, 4096);
    if (wcscmp(old, text) == 0) return 0;
    SetWindowTextW(h, text);
    return 1;
}

/* What the header, the warnings and the status bar show: the drives as
 * they are now, and the figures read at the end of the last job. */
static void update_drives_label(void)
{
    if (g_busy) return; /* the catalog belongs to the running job */
    RECT hr = {0, 0, 0, 0}, wr = {0, 0, 0, 0};
    if (g_header) GetClientRect(g_header, &hr);
    if (g_warnbar && is_shown(g_warnbar)) GetClientRect(g_warnbar, &wr);
    warn_begin();
    g_warn_changed = 0;
    if (g_cat) {
        drive_list dl = load_drives();
        for (int i = 0; i < dl.n; i++) {
            const drive_snap *d = &dl.items[i];
            if (strcmp(d->kind, "drive") != 0 || d->secondary || d->lost_ms) continue;
            const drive_fig *g = fig_of(d->id);
            wchar_t *l = widen(d->label), key[80];
            const wchar_t *name = l ? l : L"";
            warn_item *w;
            if (d->connected && g && g->has_pv) {
                const bd_backup_preview_info *pv = &g->pv;
                /* Many files changed at once: changed and deleted apart, so
                 * the counts match Next backup's. */
                if (pv->mass_change && !g_warn_changed) {
                    long long del = (long long)pv->held_deleted, chg = (long long)(pv->held_changed - pv->held_deleted);
                    swprintf(key, 80, L"mass:%lld", (long long)d->id);
                    if ((w = warn_add(TONE_BAD, GLYPH_BANG, key)) != NULL) {
                        swprintf(w->title, 200, L"%ls", T(L"Many files changed at once"));
                        if (del > 0)
                            swprintf(w->text, 900,
                                     TN(del, L"%lld changed and %lld was deleted since the last backup to %ls. That can mean ransomware: look at them before you back up.",
                                        L"%lld changed and %lld were deleted since the last backup to %ls. That can mean ransomware: look at them before you back up."),
                                     chg, del, name);
                        else
                            swprintf(w->text, 900,
                                     TN(chg, L"%lld file changed since the last backup to %ls. That can mean ransomware: look at it before you back up.",
                                        L"%lld files changed since the last backup to %ls. That can mean ransomware: look at them before you back up."),
                                     chg, name);
                        warn_button(w, 0, WA_CHANGED, d->id, TN(chg + del, L"Review %lld file...", L"Review %lld files..."), chg + del);
                        g_warn_changed = d->id;
                    }
                }
                if (pv->files_to_copy && pv->bytes_short > 0) {
                    wchar_t shortb[64];
                    format_bytes(pv->bytes_short, shortb, 64);
                    swprintf(key, 80, L"short:%lld", (long long)d->id);
                    if ((w = warn_add(TONE_WARN, GLYPH_BANG, key)) != NULL) {
                        swprintf(w->title, 200, T(L"Not everything fits on %ls"), name);
                        swprintf(w->text, 900, T(L"About %ls will not fit on it. That waits for another drive."), shortb);
                        warn_button(w, 0, WA_NEXT_TAB, d->id, L"%ls", T(L"See what goes where"));
                    }
                }
                if (pv->files_too_big) {
                    swprintf(key, 80, L"fat32:%lld", (long long)d->id);
                    if ((w = warn_add(TONE_WARN, GLYPH_BANG, key)) != NULL) {
                        swprintf(w->title, 200, T(L"%ls cannot take files of 4 GB or more"), name);
                        swprintf(w->text, 900,
                                 TN(pv->files_too_big, L"It is formatted as FAT32, so %lld file cannot go on it. Reformat it as exFAT or NTFS.",
                                    L"It is formatted as FAT32, so %lld files cannot go on it. Reformat it as exFAT or NTFS."),
                                 (long long)pv->files_too_big);
                        warn_button(w, 0, WA_NEXT_TAB, d->id, L"%ls", T(L"See what goes where"));
                    }
                }
            }
            const wchar_t *advice = replace_advice(d);
            if (advice) {
                swprintf(key, 80, L"health:%lld", (long long)d->id);
                if ((w = warn_add(TONE_BAD, GLYPH_BANG, key)) != NULL) {
                    swprintf(w->title, 200, T(L"%ls: its health data warns"), name);
                    swprintf(w->text, 900, L"%ls", advice);
                    w->closable = 1;
                    warn_button(w, 0, WA_LOCAL, d->id, L"%ls", T(L"Details"));
                }
            }
            if (d->check_due) {
                swprintf(key, 80, L"due:%lld", (long long)d->id);
                if ((w = warn_add(TONE_NEUTRAL, GLYPH_INFO, key)) != NULL) {
                    swprintf(w->title, 200, T(L"%ls is due for a check"), name);
                    swprintf(w->text, 900, L"%ls",
                             d->connected ? T(L"A check reads every copy on it back, to find any that went bad.")
                                          : T(L"Plug it in, and a check reads every copy on it back, to find any that went bad."));
                    if (d->connected) warn_button(w, 0, WA_CHECK, d->id, T(L"Check %ls"), name);
                }
            }
            free(l);
        }
        free(dl.items);
        /* Damaged files in the backup roots: said here, not only in the log. */
        if (g_figs && g_figs->n_damaged > 0) {
            warn_item *w = warn_add(TONE_BAD, GLYPH_CROSS, L"damaged");
            if (w) {
                long long n = (long long)g_figs->n_damaged;
                swprintf(w->title, 200, TN(n, L"%lld file in your backup roots may be damaged", L"%lld files in your backup roots may be damaged"), n);
                swprintf(w->text, 900, L"%ls",
                         TN(n, L"A bit-rot scan found it changed without its size or time changing. You can restore it from a backup drive.",
                            L"A bit-rot scan found them changed without their size or time changing. You can restore them from a backup drive."));
                warn_button(w, 0, WA_DAMAGED, 0, L"%ls", TN(n, L"Show it", L"Show them"));
            }
        }
        /* Files gone from the backup roots since the user last looked. */
        if (g_gone_new > 0) {
            wchar_t key[80];
            swprintf(key, 80, L"gone:%lld", (long long)g_gone_new);
            warn_item *w = warn_add(TONE_WARN, GLYPH_BANG, key);
            if (w) {
                long long n = (long long)g_gone_new;
                swprintf(w->title, 200, TN(n, L"%lld file was deleted from your backup roots", L"%lld files were deleted from your backup roots"), n);
                swprintf(w->text, 900, L"%ls",
                         TN(n, L"Its copies are still on your drives. You can restore it, keep tracking it, or forget it.",
                            L"Their copies are still on your drives. You can restore them, keep tracking them, or forget them."));
                warn_button(w, 0, WA_DELETED, 0, L"%ls", TN(n, L"Review it...", L"Review them..."));
            }
        }
        /* A backup root the last scan could not find. */
        for (int i = 0; g_figs && i < g_figs->n_missing && i < 4; i++) {
            wchar_t key[80];
            swprintf(key, 80, L"missing:%lld", (long long)g_figs->missing_id[i]);
            warn_item *w = warn_add(TONE_BAD, GLYPH_CROSS, key);
            if (!w) break;
            swprintf(w->title, 200, T(L"%ls cannot be found"), g_figs->missing_name[i]);
            swprintf(w->text, 900, L"%ls",
                     T(L"The last scan could not find this backup root, so its files are not marked deleted. Say where it is now, or restore "
                       L"it from your backup drives."));
            warn_button(w, 0, WA_RELOCATE, g_figs->missing_id[i], L"%ls", T(L"Where is it now?..."));
            warn_button(w, 1, WA_RESTORE_ROOT, g_figs->missing_id[i], L"%ls", T(L"Restore it..."));
        }
        warn_sort();
    }
    header_refresh();
    status_bar_refresh();
    /* The button says how many files are short of the target; it is as
     * wide as its text. */
    wchar_t risk[64];
    wcscpy(risk, T(L"Files at ris&k..."));
    if (g_figs && g_figs->has_risk && g_figs->risk.files_at_risk > 0)
        swprintf(risk, 64, T(L"Files at ris&k (%lld)..."), (long long)g_figs->risk.files_at_risk);
    else if (g_figs && g_figs->has_risk)
        wcscpy(risk, T(L"Files at ris&k (none)..."));
    int changed = set_text_changed(g_buttons[7], risk);
    if (g_header && header_height(hr.right) != hr.bottom) changed = 1;
    if (g_warnbar && warn_height() != wr.bottom) changed = 1;
    if (changed) layout();
    else {
        header_layout();
        warn_layout();
        warn_tooltips();
    }
}

/* ---- Files at risk ------------------------------------------------------ */

#define RISK_ROWS 2000

/* ---- Tables sorted by a click on a column's header ------------------------ */

#ifndef SORT_DIGITSASNUMBERS
#define SORT_DIGITSASNUMBERS 0x00000008
#endif
#define LV_SORT_PROP L"BrodalfSort" /* (column + 1) * 2, plus 1 when descending; none when not sorted */
#define LV_RESORT (WM_APP + 90)     /* posted to a table after it is filled again */

/* Each row keeps, as its lParam, the place it was added at: that is where
 * its data is in the arrays the table was filled from, however the rows are
 * sorted. lv_index turns a row's place on the screen into that, lv_pos the
 * other way. */
static int lv_index(HWND lv, int pos)
{
    if (pos < 0) return -1;
    LVITEMW it;
    memset(&it, 0, sizeof(it));
    it.mask = LVIF_PARAM;
    it.iItem = pos;
    if (!SendMessageW(lv, LVM_GETITEMW, 0, (LPARAM)&it)) return pos;
    return (int)it.lParam;
}

static int lv_pos(HWND lv, int index)
{
    if (index < 0) return -1;
    LVFINDINFOW fi;
    memset(&fi, 0, sizeof(fi));
    fi.flags = LVFI_PARAM;
    fi.lParam = index;
    int pos = (int)SendMessageW(lv, LVM_FINDITEMW, (WPARAM)-1, (LPARAM)&fi);
    return pos >= 0 ? pos : index;
}

/* A cell that starts with a number, perhaps with a size after it: "12",
 * "45%", "1.5 GB" (as bytes), or a date by its year (the rest of a date
 * sorts as text, which works for 2026-11-25 14:03). */
static int lv_number(const wchar_t *s, double *out)
{
    while (*s == L' ') s++;
    if (!iswdigit(*s)) return 0;
    wchar_t *end = NULL;
    double v = wcstod(s, &end);
    if (!end || end == s) return 0;
    while (*end == L' ') end++;
    const wchar_t *units[] = {T(L"KB"), T(L"MB"), T(L"GB"), T(L"TB")};
    double scale = 1.0;
    for (int u = 0; u < 4; u++) {
        size_t k = wcslen(units[u]);
        scale *= 1024.0;
        if (k && _wcsnicmp(end, units[u], k) == 0 && !iswalpha(end[k])) {
            v *= scale;
            break;
        }
    }
    *out = v;
    return 1;
}

typedef struct { wchar_t **text; double *num; char *has_num; int n, desc; } lv_sort_data;

/* By the rows' lParams: numbers and sizes as numbers, after cells with no
 * number (such as "never"), the rest as text with digits read as numbers;
 * rows that compare the same stay in the order they came. */
static int CALLBACK lv_compare(LPARAM pa, LPARAM pb, LPARAM ctx)
{
    const lv_sort_data *d = (const lv_sort_data *)ctx;
    int a = (int)pa, b = (int)pb, r = 0;
    if (a >= 0 && a < d->n && b >= 0 && b < d->n) {
        if (d->has_num[a] && d->has_num[b]) r = d->num[a] < d->num[b] ? -1 : d->num[a] > d->num[b] ? 1 : 0;
        else if (d->has_num[a] != d->has_num[b]) r = d->has_num[a] ? 1 : -1;
        if (r == 0) {
            const wchar_t *x = d->text[a] ? d->text[a] : L"", *y = d->text[b] ? d->text[b] : L"";
            int c = CompareStringW(LOCALE_USER_DEFAULT, NORM_IGNORECASE | SORT_DIGITSASNUMBERS, x, -1, y, -1);
            r = c ? c - CSTR_EQUAL : _wcsicmp(x, y);
        }
    }
    if (r) return d->desc ? -r : r;
    return a < b ? -1 : a > b ? 1 : 0;
}

/* Sort the table as its header says, and show the arrow on that column. */
static void lv_sort(HWND lv)
{
    INT_PTR st = (INT_PTR)GetPropW(lv, LV_SORT_PROP);
    int col = (int)(st / 2) - 1, desc = (int)(st % 2);
    HWND header = ListView_GetHeader(lv);
    int ncol = header ? Header_GetItemCount(header) : 0;
    for (int i = 0; i < ncol; i++) {
        HDITEMW hi;
        memset(&hi, 0, sizeof(hi));
        hi.mask = HDI_FORMAT;
        if (!SendMessageW(header, HDM_GETITEMW, (WPARAM)i, (LPARAM)&hi)) continue;
        hi.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (i == col) hi.fmt |= desc ? HDF_SORTDOWN : HDF_SORTUP;
        SendMessageW(header, HDM_SETITEMW, (WPARAM)i, (LPARAM)&hi);
    }
    int n = ListView_GetItemCount(lv);
    if (col < 0 || col >= ncol || n < 2) return;
    lv_sort_data d;
    d.n = n;
    d.desc = desc;
    d.text = calloc((size_t)n, sizeof(wchar_t *));
    d.num = calloc((size_t)n, sizeof(double));
    d.has_num = calloc((size_t)n, 1);
    if (d.text && d.num && d.has_num) {
        wchar_t buf[512];
        for (int i = 0; i < n; i++) {
            LVITEMW it;
            memset(&it, 0, sizeof(it));
            it.mask = LVIF_TEXT | LVIF_PARAM;
            it.iItem = i;
            it.iSubItem = col;
            it.pszText = buf;
            it.cchTextMax = 512;
            buf[0] = 0;
            if (!SendMessageW(lv, LVM_GETITEMW, 0, (LPARAM)&it)) continue;
            if (it.lParam < 0 || it.lParam >= n || d.text[it.lParam]) continue;
            d.text[it.lParam] = _wcsdup(it.pszText ? it.pszText : L"");
            if (d.text[it.lParam]) d.has_num[it.lParam] = (char)lv_number(d.text[it.lParam], &d.num[it.lParam]);
        }
        ListView_SortItems(lv, lv_compare, (LPARAM)&d);
        int focus = ListView_GetNextItem(lv, -1, LVNI_FOCUSED);
        if (focus >= 0) ListView_EnsureVisible(lv, focus, FALSE);
    }
    if (d.text)
        for (int i = 0; i < n; i++) free(d.text[i]);
    free(d.text);
    free(d.num);
    free(d.has_num);
}

/* A click on a column's header sorts by it, a second click the other way.
 * A table filled again is sorted again the same way once it is filled. */
static LRESULT CALLBACK lv_sort_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
    (void)ref;
    if (msg == WM_NOTIFY) {
        const NMHDR *nh = (const NMHDR *)lp;
        if (nh && nh->hwndFrom == ListView_GetHeader(h) && (nh->code == HDN_ITEMCLICKW || nh->code == HDN_ITEMCLICKA)) {
            int col = ((const NMHEADERW *)lp)->iItem;
            INT_PTR st = (INT_PTR)GetPropW(h, LV_SORT_PROP);
            int desc = (int)(st / 2) - 1 == col ? !(int)(st % 2) : 0;
            if (col >= 0) {
                SetPropW(h, LV_SORT_PROP, (HANDLE)(INT_PTR)((col + 1) * 2 + desc));
                lv_sort(h);
            }
        }
    } else if (msg == LVM_DELETEALLITEMS) {
        LRESULT r = DefSubclassProc(h, msg, wp, lp);
        if (GetPropW(h, LV_SORT_PROP)) PostMessageW(h, LV_RESORT, 0, 0);
        return r;
    } else if (msg == LV_RESORT) {
        lv_sort(h);
        return 0;
    } else if (msg == WM_NCDESTROY) {
        RemovePropW(h, LV_SORT_PROP);
        RemoveWindowSubclass(h, lv_sort_proc, id);
    }
    return DefSubclassProc(h, msg, wp, lp);
}

static void lv_sortable(HWND lv)
{
    SetWindowSubclass(lv, lv_sort_proc, 7, 0);
}

static void lv_columns(HWND lv, const wchar_t **names, const int *widths, int n)
{
    lv_sortable(lv);
    ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT);
    /* Never narrower than its name, in any language. */
    HDC dc = GetDC(lv);
    HFONT f = (HFONT)SendMessageW(lv, WM_GETFONT, 0, 0);
    HGDIOBJ of = SelectObject(dc, f ? f : g_font);
    for (int i = 0; i < n; i++) {
        LVCOLUMNW c;
        memset(&c, 0, sizeof(c));
        c.mask = LVCF_TEXT | LVCF_WIDTH;
        c.pszText = (wchar_t *)T(names[i]); /* the tables mark their names with N_ */
        SIZE sz = {0, 0};
        GetTextExtentPoint32W(dc, c.pszText, (int)wcslen(c.pszText), &sz);
        int need = sz.cx + S(22);
        c.cx = S(widths[i]) > need ? S(widths[i]) : need;
        ListView_InsertColumn(lv, i, &c);
    }
    SelectObject(dc, of);
    ReleaseDC(lv, dc);
}

static void lv_row(HWND lv, const wchar_t **cells, int n)
{
    LVITEMW it;
    memset(&it, 0, sizeof(it));
    it.mask = LVIF_TEXT | LVIF_PARAM;
    it.iItem = ListView_GetItemCount(lv);
    it.lParam = it.iItem; /* where its data is: lv_index */
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
static wchar_t g_risk_label[64][64];
static int64_t g_risk_pick;

static int risk_file_cb(void *ctx, const bd_risk_info *r)
{
    risk_fill_ctx *f = ctx;
    if (f->rows >= RISK_ROWS) return 1;
    f->rows++;
    wchar_t *src = widen(r->source_name), *rel = widen(r->rel_path), file[1200], copies[16], places[16], size[64], note[200];
    swprintf(file, 1200, L"%ls\\%ls", src ? src : L"", rel ? rel : L"");
    for (wchar_t *c = file; *c; c++) if (*c == L'/') *c = L'\\';
    {
        bd_target t;
        bd_target_get(g_cat, &t);
        swprintf(copies, 16, T(L"%d of %d"), r->copies, t.copies);
        swprintf(places, 16, T(L"%d of %d"), r->places, t.places);
    }
    format_bytes(r->size, size, 64);
    note[0] = 0;
    if (r->older_copies) wcscat(note, T(L"Changed since its last backup. "));
    if (r->unknown_place) wcscat(note, T(L"On a drive with no place set."));
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
    swprintf(name, 300, L"%ls%ls", label ? label : L"", h->connected ? T(L"  (plugged in)") : L"");
    swprintf(files, 32, TN(h->files, L"%lld file", L"%lld files"), (long long)h->files);
    format_bytes(h->bytes, size, 64);
    wchar_t room[96];
    wcsncpy(room, T(L"unknown"), 95);
    room[95] = 0;
    if (h->free_bytes >= 0) {
        wchar_t fb[64];
        format_bytes(h->free_bytes, fb, 64);
        swprintf(room, 96, T(L"%ls free%ls"), fb, h->fits ? T(L", room for all of it") : T(L", not enough"));
    }
    const wchar_t *cells[] = {name, loc && *loc ? loc : T(L"(not set)"), files, size, room};
    lv_row(f->lv, cells, 5);
    if (g_risk_media_n < 64) {
        g_risk_media[g_risk_media_n] = h->media_id;
        g_risk_connected[g_risk_media_n] = h->connected;
        swprintf(g_risk_media_name[g_risk_media_n], 128, L"%ls%ls%ls%ls", label ? label : L"", loc && *loc ? T(L" (kept in ") : L"",
                 loc && *loc ? loc : L"", loc && *loc ? L")" : L"");
        swprintf(g_risk_label[g_risk_media_n], 64, L"%ls", label ? label : L"");
        g_risk_media_n++;
    }
    free(label);
    free(loc);
    f->rows++;
    return 0;
}

/* The button says which drive it backs up to; it is on only when the
 * selected drive is plugged in. */
static void risk_button(HWND dlg)
{
    HWND help = GetDlgItem(dlg, IDC_RISK_HELP), b = GetDlgItem(dlg, IDC_RISK_BACKUP);
    int row = lv_index(help, ListView_GetNextItem(help, -1, LVNI_SELECTED));
    wchar_t text[120];
    if (row >= 0 && row < g_risk_media_n && g_risk_connected[row]) {
        swprintf(text, 120, T(L"Back up to %ls"), g_risk_label[row]);
        EnableWindow(b, TRUE);
    } else {
        swprintf(text, 120, L"%ls", row >= 0 && row < g_risk_media_n ? T(L"Plug it in to back up") : T(L"Back up to it"));
        EnableWindow(b, FALSE);
    }
    SetWindowTextW(b, text);
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
        swprintf(text, 1024, T(L"No files yet. Add a folder and scan it first."));
    else if (st.files_at_risk == 0)
        swprintf(text, 1024, T(L"All %lld files have at least %d copies in %d different places."), (long long)st.files_total, t.copies, t.places);
    else {
        wchar_t short_of[600], none[300];
        swprintf(short_of, 600, TN(st.files_at_risk, L"%lld of %lld files (%ls) has fewer than %d copies in %d different places.",
                                   L"%lld of %lld files (%ls) have fewer than %d copies in %d different places."),
                 (long long)st.files_at_risk, (long long)st.files_total, size, t.copies, t.places);
        swprintf(none, 300, TN(st.files_no_copy, L"%lld has no copy of its current version at all.",
                               L"%lld have no copy of their current version at all."), (long long)st.files_no_copy);
        swprintf(text, 1024, L"%ls %ls", short_of, none);
    }
    if (st.deleted_kept || st.old_versions_kept) {
        size_t len = wcslen(text);
        swprintf(text + len, 1024 - len, T(L" Also kept: %lld deleted files (%lld short of the target) and %lld older versions (%lld short)."),
                 (long long)st.deleted_kept, (long long)st.deleted_short, (long long)st.old_versions_kept, (long long)st.old_versions_short);
    }
    SetDlgItemTextW(dlg, IDC_RISK_SUMMARY, text);

    HWND help = GetDlgItem(dlg, IDC_RISK_HELP), files = GetDlgItem(dlg, IDC_RISK_FILES);
    ListView_DeleteAllItems(help);
    ListView_DeleteAllItems(files);
    risk_fill_ctx hc = {help, 0};
    g_risk_media_n = 0;
    if (st.files_at_risk) bd_list_risk_help(g_cat, risk_help_cb, &hc);
    if (st.files_at_risk && !hc.rows) {
        const wchar_t *cells[] = {T(L"None of your drives would help. Set up another drive, or one kept somewhere else."), L"", L"", L"", L""};
        lv_row(help, cells, 5);
    }
    /* The first plugged-in drive is picked, so the button is ready. */
    for (int i = 0; i < g_risk_media_n; i++)
        if (g_risk_connected[i]) {
            lv_select(help, i);
            break;
        }
    risk_button(dlg);
    risk_fill_ctx fc = {files, 0};
    if (st.files_at_risk) bd_list_at_risk(g_cat, 0, risk_file_cb, &fc, NULL);
    if (st.files_at_risk > fc.rows) {
        wchar_t more[128];
        swprintf(more, 128, T(L"... and %lld more"), (long long)(st.files_at_risk - fc.rows));
        const wchar_t *cells[] = {more, L"", L"", L"", L""};
        lv_row(files, cells, 5);
    }
}

static INT_PTR CALLBACK risk_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    INT_PTR themed;
    if (theme_dialog(dlg, msg, wp, lp, &themed)) return themed;
    switch (msg) {
    case WM_INITDIALOG: {
        static const wchar_t *help_cols[] = {N_(L"Drive"), N_(L"Kept in"), N_(L"Would fix"), N_(L"Size"), N_(L"Free space")};
        static const int help_w[] = {150, 120, 80, 70, 200};
        static const wchar_t *file_cols[] = {N_(L"File"), N_(L"Copies"), N_(L"Places"), N_(L"Size"), N_(L"Note")};
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
            HWND help = GetDlgItem(dlg, IDC_RISK_HELP);
            int row = lv_index(help, ListView_GetNextItem(help, -1, LVNI_SELECTED));
            if (row < 0 || row >= g_risk_media_n) {
                MessageBoxW(dlg, T(L"Pick a drive in the list first."), APP_NAME, MB_ICONINFORMATION);
                return TRUE;
            }
            if (!g_risk_connected[row]) {
                wchar_t m[400];
                swprintf(m, 400, T(L"Plug in %ls first, then try again."), g_risk_media_name[row]);
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
            if (s == BD_OK) s = ui_save();
            if (s != BD_OK) {
                wchar_t *e = widen(bd_catalog_error(g_cat));
                MessageBoxW(dlg, e ? e : T(L"Could not change the target."), APP_NAME, MB_ICONWARNING);
                free(e);
                return TRUE;
            }
            log_append(T(L"The number of full copies to keep changed."));
            risk_fill(dlg);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL || LOWORD(wp) == IDOK) { EndDialog(dlg, IDOK); return TRUE; }
        break;
    case WM_NOTIFY: {
        NMHDR *nh = (NMHDR *)lp;
        if (nh->idFrom == IDC_RISK_HELP && nh->code == NM_DBLCLK) SendMessageW(dlg, WM_COMMAND, IDC_RISK_BACKUP, 0);
        if (nh->idFrom == IDC_RISK_HELP && nh->code == LVN_ITEMCHANGED) risk_button(dlg);
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
enum { TABLE_DRIVES, TABLE_LARGEST, TABLE_DAMAGED, TABLE_CONFLICTS, TABLE_CHANGED, TABLE_REPORT };
/* What the engine reported during the last job that had problems. */
static wchar_t **g_report;
static int g_n_report;
static int64_t g_table_media; /* changed: the drive */
static void drive_name(const char *label, const char *location, wchar_t *out, size_t cap);
static int g_table_mode;
static int64_t g_table_ids[256];
static int64_t *g_table_nodes; /* damaged: node id by row */
/* Damaged, changed and left-alone files: the version to restore, by row;
 * and the one picked to restore once the table is closed. */
static int64_t *g_table_vers, g_table_restore_vid;
static int g_table_nvers, g_table_cvers;

static void table_add_version(int64_t vid)
{
    if (g_table_nvers == g_table_cvers) {
        int cap = g_table_cvers ? g_table_cvers * 2 : 64;
        int64_t *grown = realloc(g_table_vers, sizeof(int64_t) * (size_t)cap);
        if (!grown) return;
        g_table_vers = grown;
        g_table_cvers = cap;
    }
    g_table_vers[g_table_nvers++] = vid;
}

static void restore_version(int64_t version_id);
static int g_table_n, g_table_pick;
/* The files the last restore left alone, from its job. */
static conflict_row *g_conflicts;
static int g_n_conflicts;

/* What a list window says inside its empty list. */
static wchar_t g_table_empty[400];
/* The changed files: the words on the button that backs them up ("" none),
 * and whether it was pressed. */
static wchar_t g_table_backup_text[200];
static int g_table_backup;
static int button_width(HWND b);

/* Every column as wide as what it holds (none over about half the list),
 * and the one that holds the paths takes the room left. */
static void lv_fit_all(HWND lv, int fill)
{
    HWND hd = ListView_GetHeader(lv);
    int n = hd ? Header_GetItemCount(hd) : 0;
    RECT rc;
    GetClientRect(lv, &rc);
    int room = rc.right - GetSystemMetrics(SM_CXVSCROLL), used = 0;
    for (int i = 0; i < n; i++) {
        ListView_SetColumnWidth(lv, i, LVSCW_AUTOSIZE_USEHEADER);
        int w = ListView_GetColumnWidth(lv, i);
        if (n > 1 && w > room * 55 / 100) {
            w = room * 55 / 100;
            ListView_SetColumnWidth(lv, i, w);
        }
        used += w;
    }
    if (fill >= 0 && fill < n && used < room) ListView_SetColumnWidth(lv, fill, ListView_GetColumnWidth(lv, fill) + room - used);
}

/* "Root\folder\file", with backslashes, as Explorer writes it. */
static void root_path(const wchar_t *root, const wchar_t *rel, wchar_t *out, size_t cap)
{
    swprintf(out, cap, L"%ls%ls%ls", root ? root : L"", root && *root && rel && *rel ? L"\\" : L"", rel ? rel : L"");
    for (wchar_t *q = out; *q; q++)
        if (*q == L'/') *q = L'\\';
}

static void table_count(int64_t n, int64_t bytes, wchar_t *out, size_t cap)
{
    wchar_t b[64];
    if (!n) { wcscpy(out, L"0"); return; }
    format_bytes(bytes, b, 64);
    swprintf(out, cap, L"%lld (%ls)", (long long)n, b);
}

static void table_drives(HWND lv)
{
    /* Problems comes early, so a drive marked broken never reads as healthy. */
    static const wchar_t *cols[] = {N_(L"Drive"), N_(L"Plugged in"), N_(L"Problems"), N_(L"As it is now"), N_(L"Changed since"), N_(L"Not on it"), N_(L"Last backup"), N_(L"Last check")};
    static const int w[] = {190, 70, 190, 90, 90, 90, 130, 130};
    lv_columns(lv, cols, w, 8);
    drive_list dl = load_drives();
    g_table_n = 0;
    for (int i = 0; i < dl.n && g_table_n < 256; i++) {
        const drive_snap *d = &dl.items[i];
        if (strcmp(d->kind, "drive") != 0) continue;
        /* From the figures read after the last job: reading them here would
         * hold up the window. */
        const drive_fig *g = fig_of(d->id);
        bd_media_status ms;
        memset(&ms, 0, sizeof(ms));
        int have = g && g->has_ms;
        if (have) ms = g->ms;
        wchar_t name[600], cur[96] = L"", behind[96] = L"", notit[96] = L"", backup[128] = L"", check[128] = L"", probs[160] = L"";
        if (have) {
            wcsncpy(backup, T(L"Never"), 127);
            backup[127] = 0;
            wcscpy(check, backup);
            table_count(ms.files_current, ms.bytes_current, cur, 96);
            table_count(ms.files_behind, ms.bytes_behind, behind, 96);
            table_count(ms.files_not_here, ms.bytes_not_here, notit, 96);
        } else
            wcscpy(probs, g ? T(L"could not be read") : T(L"being read..."));
        drive_name(d->label, d->location, name, 600);
        if (d->lost_ms) {
            /* Its copies no longer count: no figures that look like a healthy drive's. */
            size_t len = wcslen(name);
            swprintf(name + len, 600 - len, L" %ls", strcmp(d->lost_why, "broken") == 0 ? T(L"(marked broken)") : T(L"(marked lost)"));
            cur[0] = behind[0] = notit[0] = 0;
        } else if (d->secondary) {
            size_t len = wcslen(name);
            swprintf(name + len, 600 - len, L" %ls", T(L"(extra place)"));
        }
        if (ms.last_backup_ms) {
            wchar_t when[64];
            format_time_ms(ms.last_backup_ms, when, 64);
            const char *st = ms.last_backup_status;
            swprintf(backup, 128, L"%ls%ls", when, !ms.last_backup_end_ms ? T(L", did not finish") : strcmp(st, "done") == 0 ? L""
                                                   : strcmp(st, "partial") == 0 ? T(L", with problems") : strcmp(st, "stopped") == 0 ? T(L", stopped")
                                                   : T(L", failed"));
        }
        if (ms.last_check_ms) {
            wchar_t when[64];
            format_time_ms(ms.last_check_ms, when, 64);
            swprintf(check, 128, L"%ls, %ls", when, strcmp(ms.last_check_kind, "read back") == 0 ? T(L"copies not checked lately") : T(L"every copy"));
        }
        if (d->lost_ms) {
            swprintf(probs, 160, L"%ls", strcmp(d->lost_why, "broken") == 0 ? T(L"marked broken: its copies no longer count")
                                                                           : T(L"marked lost: its copies no longer count"));
        } else {
            if (ms.copies_bad) swprintf(probs, 160, T(L"%lld missing or damaged"), (long long)ms.copies_bad);
            if (ms.copies_never_read) {
                size_t len = wcslen(probs);
                swprintf(probs + len, 160 - len, T(L"%ls%lld never checked"), len ? L", " : L"", (long long)ms.copies_never_read);
            }
            if (d->check_due) {
                size_t len = wcslen(probs);
                swprintf(probs + len, 160 - len, T(L"%lsdue for a check"), len ? L", " : L"");
            }
        }
        const wchar_t *cells[] = {name, d->connected ? T(L"Yes") : T(L"No"), probs, cur, behind, notit, backup, check};
        lv_row(lv, cells, 8);
        g_table_ids[g_table_n++] = d->id;
    }
    free(dl.items);
    swprintf(g_table_empty, 400, L"%ls", T(L"No drives yet. Use Back up to set one up."));
    lv_fit_all(lv, 2);
    SetDlgItemTextW(GetParent(lv), IDC_TABLE_TEXT,
                    g_table_n ? T(L"Where each drive stands, from the database (the drives need not be plugged in). \"Changed since\": only an "
                                L"older version of the file is on the drive. Double-click a drive for its details.")
                              : T(L"Your backup drives are listed here once you have set one up."));
}

typedef struct { HWND lv; int rows; } table_rows;

static bd_target g_table_target;

static int table_largest_cb(void *ctx, const bd_risk_info *r)
{
    table_rows *t = ctx;
    wchar_t *src = widen(r->source_name), *rel = widen(r->rel_path), size[64], copies[32], places[32], file[1200];
    format_bytes(r->size, size, 64);
    swprintf(copies, 32, T(L"%d of %d"), r->copies, g_table_target.copies);
    swprintf(places, 32, T(L"%d of %d"), r->places, g_table_target.places);
    root_path(src, rel, file, 1200);
    const wchar_t *cells[] = {file, size, copies, places};
    lv_row(t->lv, cells, 4);
    t->rows++;
    free(src);
    free(rel);
    return 0;
}

static void table_largest(HWND lv)
{
    static const wchar_t *cols[] = {N_(L"File"), N_(L"Size"), N_(L"Good copies"), N_(L"Places")};
    static const int w[] = {420, 80, 90, 70};
    lv_columns(lv, cols, w, 4);
    table_rows t = {lv, 0};
    g_table_target.copies = 2;
    g_table_target.places = 2;
    bd_target_get(g_cat, &g_table_target);
    HCURSOR old = SetCursor(LoadCursor(NULL, IDC_WAIT));
    bd_list_largest(g_cat, 200, table_largest_cb, &t);
    SetCursor(old);
    swprintf(g_table_empty, 400, L"%ls", T(L"No files yet. Add a backup root and scan it."));
    lv_fit_all(lv, 0);
    SetDlgItemTextW(GetParent(lv), IDC_TABLE_TEXT,
                    t.rows ? T(L"The largest files in your backup roots, largest first (the top 200): where the space on your drives goes.")
                           : T(L"The largest files in your backup roots are listed here once they are scanned."));
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
    wchar_t *src = widen(d->source_name), *rel = widen(d->rel_path), size[64], file[1200], good[300] = L"";
    format_bytes(d->size, size, 64);
    root_path(src, rel, file, 1200);
    {
        copy_list cl = {NULL, 0, 0};
        bd_list_copies(g_cat, d->node_id, copy_snap_cb, &cl);
        for (int i = 0; i < cl.n; i++) {
            const copy_snap *c = &cl.items[i];
            if (c->c.version_id != d->version_id || !c->c.media_id || c->c.media_lost || strcmp(c->state, "ok") != 0) continue;
            wchar_t *w = widen(c->label);
            size_t len = wcslen(good);
            if (w && *w && !wcsstr(good, w) && len < 250) swprintf(good + len, 300 - len, L"%ls%ls", len ? L", " : L"", w);
            free(w);
        }
        free(cl.items);
        if (!good[0]) wcscpy(good, T(L"none"));
    }
    const wchar_t *cells[] = {file, size, good};
    lv_row(t->lv, cells, 3);
    g_table_nodes[t->rows++] = d->node_id;
    table_add_version(d->version_id);
    free(src);
    free(rel);
    return 0;
}

static void table_damaged(HWND lv)
{
    static const wchar_t *cols[] = {N_(L"File"), N_(L"Size"), N_(L"Good copy on")};
    static const int w[] = {440, 80, 140};
    lv_columns(lv, cols, w, 3);
    damaged_rows t = {lv, 0, 0};
    free(g_table_nodes);
    g_table_nodes = NULL;
    bd_list_damaged(g_cat, table_damaged_cb, &t);
    g_table_n = t.rows;
    swprintf(g_table_empty, 400, L"%ls", T(L"No damaged files."));
    lv_fit_all(lv, 0);
    SetDlgItemTextW(GetParent(lv), IDC_TABLE_TEXT,
                    t.rows ? T(L"These files changed although their size and modified time did not: that is what a failing drive or bit rot "
                             L"looks like. Their copies on your drives are the good ones: pick a file and press Restore the good copy... "
                             L"to get it back. If you changed one on purpose, pick it and choose Keep as it is now.")
                           : T(L"A bit-rot scan (Tools > Bit-rot scan) reads every file again to look for them."));
}

static void table_conflicts(HWND lv)
{
    static const wchar_t *cols[] = {N_(L"File"), N_(L"Was going to"), N_(L"Backup has"), N_(L"Already there")};
    static const int w[] = {260, 240, 150, 150};
    lv_columns(lv, cols, w, 4);
    for (int i = 0; i < g_n_conflicts; i++) {
        const conflict_row *c = &g_conflicts[i];
        wchar_t want[160], have[160], sz[64], when[64];
        format_bytes(c->want_size, sz, 64);
        format_ns(c->want_mtime_ns, when, 64);
        swprintf(want, 160, L"%ls, %ls", sz, when);
        if (c->have_size < 0) wcscpy(have, T(L"a folder"));
        else {
            format_bytes(c->have_size, sz, 64);
            format_ns(c->have_mtime_ns, when, 64);
            swprintf(have, 160, L"%ls, %ls", sz, when);
        }
        const wchar_t *cells[] = {c->where ? c->where : L"", c->dest ? c->dest : L"", want, have};
        lv_row(lv, cells, 4);
        table_add_version(c->version_id);
    }
    g_table_n = g_n_conflicts;
    swprintf(g_table_empty, 400, L"%ls", T(L"The last restore left nothing alone."));
    lv_fit_all(lv, 1);
    SetDlgItemTextW(GetParent(lv), IDC_TABLE_TEXT,
                    g_n_conflicts ? T(L"The last restore left these alone because another file was already where each would go: nothing "
                                    L"was overwritten. Compare them, then move or rename the one that is there and restore again, or pick "
                                    L"one and press Restore it to another folder...")
                                  : T(L"A file is left alone when a restore finds another file already where it would go."));
}

typedef struct { HWND lv; int rows; int64_t total, deleted, bytes; } changed_rows;

static int table_changed_cb(void *ctx, const bd_changed_info *c)
{
    changed_rows *t = ctx;
    t->total++;
    if (c->deleted) t->deleted++;
    else t->bytes += c->size_now;
    if (t->rows >= 5000) return 0; /* counted, not listed */
    wchar_t *src = widen(c->source_name), *rel = widen(c->rel_path), what[96], when[64], size[64], where[1200];
    format_bytes(c->deleted ? c->size_on_drive : c->size_now, size, 64);
    if (c->deleted) swprintf(what, 96, L"%ls", T(L"Deleted from its folder"));
    else swprintf(what, 96, T(L"Changed (version %lld, the drive has %lld)"), (long long)c->version_now, (long long)c->version_on_drive);
    format_time_ms(c->changed_ms, when, 64);
    root_path(src, rel, where, 1200);
    const wchar_t *cells[] = {where, what, when, size};
    lv_row(t->lv, cells, 4);
    table_add_version(c->version_id_on_drive);
    t->rows++;
    free(src);
    free(rel);
    return 0;
}

static void table_changed(HWND lv)
{
    static const wchar_t *cols[] = {N_(L"File"), N_(L"What happened"), N_(L"When"), N_(L"Size")};
    static const int w[] = {380, 200, 120, 80};
    lv_columns(lv, cols, w, 4);
    changed_rows t;
    memset(&t, 0, sizeof(t));
    t.lv = lv;
    bd_list_changed_on_media(g_cat, g_table_media, table_changed_cb, &t);
    g_table_n = t.rows;
    lv_fit_all(lv, 0);
    wchar_t name[600], text[2000], size[64];
    media_name(g_table_media, name, 600);
    format_bytes(t.bytes, size, 64);
    swprintf(g_table_empty, 400, T(L"Nothing changed since the copies on %ls were made."), name);
    int64_t changed = t.total - t.deleted;
    if (!t.total) text[0] = 0;
    else if (!t.deleted)
        swprintf(text, 2000, TN(changed, L"%lld file changed since its copy on %ls was made (%ls).", L"%lld files changed since their copies on %ls were made (%ls)."),
                 (long long)changed, name, size);
    else if (!changed)
        swprintf(text, 2000, TN(t.deleted, L"%lld file was deleted since its copy on %ls was made.", L"%lld files were deleted since their copies on %ls were made."),
                 (long long)t.deleted, name);
    else
        swprintf(text, 2000, T(L"%lld changed (%ls) and %lld deleted since their copies on %ls were made."), (long long)changed, size, (long long)t.deleted, name);
    if (t.total > t.rows) {
        size_t len = wcslen(text);
        swprintf(text + len, 2000 - len, T(L" The first %d are listed."), t.rows);
    }
    if (t.total) {
        size_t len = wcslen(text);
        swprintf(text + len, 2000 - len, L" %ls",
                 g_table_backup_text[0] ? T(L"If you made these changes, back them up. If you did not, do not trust them as they are: restore the "
                                            L"drive's copy, or a whole folder from the Restore tab.")
                                        : T(L"If you did not make these changes, do not trust them as they are: restore the drive's copy, or a "
                                            L"whole folder from the Restore tab."));
    }
    SetDlgItemTextW(GetParent(lv), IDC_TABLE_TEXT, text);
}

static void table_report(HWND lv)
{
    static const wchar_t *cols[] = {N_(L"What BRODALF reported")};
    static const int w[] = {720};
    lv_columns(lv, cols, w, 1);
    for (int i = 0; i < g_n_report; i++) {
        const wchar_t *cells[] = {g_report[i]};
        lv_row(lv, cells, 1);
    }
    g_table_n = g_n_report;
    swprintf(g_table_empty, 400, L"%ls", T(L"Nothing was reported."));
    lv_fit_all(lv, 0);
    SetDlgItemTextW(GetParent(lv), IDC_TABLE_TEXT,
                    T(L"Everything BRODALF reported while the job ran, in order. The app log keeps it too, with the time of each line."));
}

static INT_PTR CALLBACK table_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    INT_PTR themed;
    if (theme_dialog(dlg, msg, wp, lp, &themed)) return themed;
    switch (msg) {
    case WM_INITDIALOG: {
        static const wchar_t *titles[] = {N_(L"Status of every drive"), N_(L"Largest files"), N_(L"Damaged files"), N_(L"Left alone by the last restore"),
                                          N_(L"Changed since the copy on the drive"), N_(L"What went wrong")};
        static const wchar_t *buttons[] = {N_(L"Details..."), L"", N_(L"Keep as it is now"), N_(L"Open in Explorer"), L"", N_(L"Show the app log")};
        /* The lists that say to restore have a button that does it. */
        static const wchar_t *restores[] = {L"", L"", N_(L"Restore the good copy..."), N_(L"Restore it to another folder..."),
                                            N_(L"Restore the drive's copy..."), L""};
        SetWindowTextW(dlg, T(titles[g_table_mode]));
        SetDlgItemTextW(dlg, IDC_TABLE_OPEN, T(buttons[g_table_mode]));
        ShowWindow(GetDlgItem(dlg, IDC_TABLE_OPEN), *buttons[g_table_mode] ? SW_SHOW : SW_HIDE);
        SetDlgItemTextW(dlg, IDC_TABLE_RESTORE, *restores[g_table_mode] ? T(restores[g_table_mode]) : L"");
        ShowWindow(GetDlgItem(dlg, IDC_TABLE_RESTORE), *restores[g_table_mode] ? SW_SHOW : SW_HIDE);
        HWND lv = GetDlgItem(dlg, IDC_TABLE_LIST);
        g_table_n = 0;
        g_table_nvers = 0;
        g_table_empty[0] = 0;
        if (g_table_mode == TABLE_DRIVES) table_drives(lv);
        else if (g_table_mode == TABLE_LARGEST) table_largest(lv);
        else if (g_table_mode == TABLE_DAMAGED) table_damaged(lv);
        else if (g_table_mode == TABLE_CHANGED) table_changed(lv);
        else if (g_table_mode == TABLE_REPORT) table_report(lv);
        else table_conflicts(lv);
        EnableWindow(GetDlgItem(dlg, IDC_TABLE_OPEN), g_table_n > 0 || g_table_mode == TABLE_REPORT);
        EnableWindow(GetDlgItem(dlg, IDC_TABLE_RESTORE), g_table_nvers > 0);
        if (g_table_mode == TABLE_CHANGED && g_table_backup_text[0]) {
            /* "These changes are mine": back them up, first, then restore. */
            HWND ob = GetDlgItem(dlg, IDC_TABLE_OPEN), rb = GetDlgItem(dlg, IDC_TABLE_RESTORE);
            SetWindowTextW(ob, g_table_backup_text);
            ShowWindow(ob, SW_SHOW);
            RECT a, b;
            GetWindowRect(ob, &a);
            GetWindowRect(rb, &b);
            MapWindowPoints(NULL, dlg, (POINT *)&a, 2);
            MapWindowPoints(NULL, dlg, (POINT *)&b, 2);
            int wo = button_width(ob), wr = button_width(rb);
            if (wo < a.right - a.left) wo = a.right - a.left;
            if (wr < b.right - b.left) wr = b.right - b.left;
            MoveWindow(ob, a.left, a.top, wo, a.bottom - a.top, TRUE);
            MoveWindow(rb, a.left + wo + (b.left - a.right), b.top, wr, b.bottom - b.top, TRUE);
        }
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_TABLE_RESTORE) {
            /* Restored once the table is closed: a restore is a job. */
            HWND lv = GetDlgItem(dlg, IDC_TABLE_LIST);
            int row = lv_index(lv, ListView_GetNextItem(lv, -1, LVNI_SELECTED));
            if (row < 0 || row >= g_table_nvers || g_table_vers[row] <= 0) {
                MessageBoxW(dlg, T(L"Pick a row in the list first."), APP_NAME, MB_ICONINFORMATION);
                return TRUE;
            }
            g_table_restore_vid = g_table_vers[row];
            EndDialog(dlg, IDC_TABLE_RESTORE);
            return TRUE;
        }
        if (LOWORD(wp) == IDC_TABLE_OPEN && g_table_mode == TABLE_CHANGED) {
            g_table_backup = 1;
            EndDialog(dlg, IDC_TABLE_OPEN);
            return TRUE;
        }
        if (LOWORD(wp) == IDC_TABLE_OPEN && g_table_mode == TABLE_REPORT) {
            show_app_log(dlg);
            return TRUE;
        }
        if (LOWORD(wp) == IDC_TABLE_OPEN) {
            HWND lv = GetDlgItem(dlg, IDC_TABLE_LIST);
            int row = lv_index(lv, ListView_GetNextItem(lv, -1, LVNI_SELECTED));
            if (row < 0 || row >= g_table_n) {
                MessageBoxW(dlg, T(L"Pick a row in the list first."), APP_NAME, MB_ICONINFORMATION);
                return TRUE;
            }
            if (g_table_mode == TABLE_DAMAGED) {
                if (ask_verbs(dlg, T(L"Keep this file as it is now? It is recorded as a new version and backed up like any "
                                     L"change; the older copies on your drives stay as they are."), MB_YESNO | MB_ICONQUESTION, T(L"Keep it as it is"), T(L"Cancel")) != IDYES)
                    return TRUE;
                if (bd_damaged_accept(g_cat, g_table_nodes[row]) != BD_OK) {
                    wchar_t *e = widen(bd_catalog_error(g_cat));
                    MessageBoxW(dlg, e ? e : T(L"Could not record it."), APP_NAME, MB_ICONWARNING);
                    free(e);
                    return TRUE;
                }
                ui_save();
                ListView_DeleteAllItems(lv);
                while (ListView_DeleteColumn(lv, 0)) {}
                g_table_nvers = 0;
                table_damaged(lv);
                EnableWindow(GetDlgItem(dlg, IDC_TABLE_OPEN), g_table_n > 0);
                EnableWindow(GetDlgItem(dlg, IDC_TABLE_RESTORE), g_table_nvers > 0);
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
        if (nh->idFrom == IDC_TABLE_LIST && nh->code == LVN_GETEMPTYMARKUP && g_table_empty[0]) {
            /* An empty list says so inside itself. */
            NMLVEMPTYMARKUP *em = (NMLVEMPTYMARKUP *)lp;
            em->dwFlags = EMF_CENTERED;
            wcsncpy(em->szMarkup, g_table_empty, L_MAX_URL_LENGTH - 1);
            em->szMarkup[L_MAX_URL_LENGTH - 1] = 0;
            SetWindowLongPtrW(dlg, DWLP_MSGRESULT, TRUE);
            return TRUE;
        }
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
        if (r == IDC_TABLE_RESTORE) {
            restore_version(g_table_restore_vid);
            break;
        }
        if (r != IDC_TABLE_OPEN || mode != TABLE_DRIVES) break;
        edit_drive(g_table_ids[g_table_pick]);
    }
    if (mode == TABLE_DAMAGED) rebuild_tree();
}

/* The files changed since their copies on a drive were made. button names
 * the backup the window may start ("" for none); returns 1 when it was
 * pressed: the changes are the user's own. */
static int show_changed_as(int64_t media_id, const wchar_t *button)
{
    g_table_media = media_id;
    g_table_backup = 0;
    swprintf(g_table_backup_text, 200, L"%ls", button ? button : L"");
    cmd_table(TABLE_CHANGED);
    g_table_backup_text[0] = 0;
    return g_table_backup;
}

static int show_changed(int64_t media_id)
{
    wchar_t b[200] = L"";
    drive_list dl = load_drives();
    const drive_snap *d = find_drive(&dl, media_id);
    if (d && d->connected && !d->lost_ms) { /* not plugged in: nothing to back up to now */
        wchar_t *w = widen(d->label);
        swprintf(b, 200, T(L"Back up these changes to %ls"), w ? w : L"");
        free(w);
    }
    free(dl.items);
    return show_changed_as(media_id, b);
}

/* Back up to a drive after the user said its changed files are theirs:
 * the question about many changes at once is not asked again. */
static int64_t g_changes_ok;
static void drive_action(int64_t id, int action);

static void back_up_changes(int64_t media_id)
{
    g_changes_ok = media_id;
    drive_action(media_id, 0);
    g_changes_ok = 0;
}

/* ---- How a job ended ------------------------------------------------------ */

/* The first words about a job that finished with problems. They come before
 * its counts, so a backup where files failed does not read as a success. */
static void job_headline(const job *j, wchar_t *out, size_t cap)
{
    out[0] = 0;
    if (j->status != BD_OK || j->n_problems <= 0) return;
    long long n = (long long)j->n_problems;
    switch (j->kind) {
    case JOB_BACKUP:
        swprintf(out, cap, TN(n, L"Backup finished with %lld problem.", L"Backup finished with %lld problems."), n);
        break;
    case JOB_SCAN:
        swprintf(out, cap, TN(n, L"%ls finished with %lld problem.", L"%ls finished with %lld problems."),
                 j->bitrot ? T(L"Bit-rot scan") : T(L"Scan"), n);
        break;
    case JOB_CHECK:
        swprintf(out, cap, TN(n, L"Check finished with %lld problem.", L"Check finished with %lld problems."), n);
        break;
    case JOB_VERIFY:
        swprintf(out, cap, TN(n, L"Check finished with %lld problem.", L"Check finished with %lld problems."), n);
        break;
    case JOB_RESTORE:
        swprintf(out, cap, TN(n, L"Restore finished with %lld problem.", L"Restore finished with %lld problems."), n);
        break;
    case JOB_COMPRESS:
        swprintf(out, cap, TN(n, L"Changing the compression finished with %lld problem.",
                              L"Changing the compression finished with %lld problems."), n);
        break;
    default:
        swprintf(out, cap, TN(n, L"Finished with %lld problem.", L"Finished with %lld problems."), n);
        break;
    }
}

/* Did the drive the job used go away while it ran (unplugged, or no longer
 * answering)? Its folder is gone. */
static int job_drive_gone(const job *j)
{
    if (j->kind != JOB_BACKUP && j->kind != JOB_CHECK && j->kind != JOB_VERIFY && j->kind != JOB_COMPRESS) return 0;
    int gone = 0;
    drive_list dl = load_drives();
    for (int i = 0; i < dl.n && !gone; i++) {
        const drive_snap *d = &dl.items[i];
        int used = d->id == j->media_id;
        for (int k = 0; k < j->n_many && !used; k++) used = d->id == j->many[k];
        if (!used || !d->root[0]) continue;
        wchar_t *w = widen(d->root);
        if (w && GetFileAttributesW(w) == INVALID_FILE_ATTRIBUTES) gone = 1;
        free(w);
    }
    free(dl.items);
    return gone;
}

/* A job that had problems no question after it is about: say so, with a
 * way to see each one. */
static void show_job_problems(const job *j, const wchar_t *headline)
{
    TASKDIALOG_BUTTON b[1] = {{650, T(L"Show what went wrong")}};
    TASKDIALOGCONFIG tc;
    memset(&tc, 0, sizeof(tc));
    tc.cbSize = sizeof(tc);
    tc.hwndParent = g_main;
    tc.hInstance = g_inst;
    tc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
    tc.dwCommonButtons = TDCBF_CLOSE_BUTTON;
    tc.pszWindowTitle = APP_NAME;
    tc.pszMainIcon = TD_WARNING_ICON;
    tc.pszMainInstruction = headline;
    tc.pszContent = j->summary;
    tc.cButtons = 1;
    tc.pButtons = b;
    tc.nDefaultButton = 650;
    int pressed = IDCLOSE;
    if (FAILED(TaskDialogIndirect(&tc, &pressed, NULL, NULL))) return;
    if (pressed != 650) return;
    if (g_n_report) cmd_table(TABLE_REPORT);
    else show_app_log(g_main);
}

/* Every file and where it is, as a CSV file for a spreadsheet. */
static void cmd_export(void)
{
    wchar_t path[MAX_PATH * 2];
    wcscpy(path, T(L"BRODALF files.csv"));
    OPENFILENAMEW of;
    memset(&of, 0, sizeof(of));
    of.lStructSize = sizeof(of);
    of.hwndOwner = g_main;
    wchar_t filter_text[256];
    of.lpstrFilter = file_filter(filter_text, 256, 4, N_(L"CSV file (*.csv)"), L"*.csv", N_(L"All files"), L"*.*");
    of.lpstrFile = path;
    of.nMaxFile = MAX_PATH * 2;
    of.lpstrDefExt = L"csv";
    of.lpstrTitle = T(L"Export the file list");
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
        swprintf(m, 1200, T(L"Could not export the file list: %ls"), e && *e ? e : T(L"the file could not be written."));
        MessageBoxW(g_main, m, APP_NAME, MB_ICONWARNING);
        free(e);
        return;
    }
    wchar_t line[MAX_PATH * 2 + 128];
    swprintf(line, MAX_PATH * 2 + 128, T(L"Exported %lld files to %ls."), (long long)rows, path);
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
static int g_label_encrypt, g_label_hide;
static wchar_t g_label_location[256];

static INT_PTR CALLBACK label_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    INT_PTR themed;
    if (theme_dialog(dlg, msg, wp, lp, &themed)) return themed;
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG:
        SetDlgItemTextW(dlg, IDC_LABEL_EDIT, g_label_buf);
        g_label_location[0] = 0;
        fill_places_combo(GetDlgItem(dlg, IDC_LABEL_LOCATION), L"");
        CheckDlgButton(dlg, IDC_LABEL_ENCRYPT, g_label_encrypt_default ? BST_CHECKED : BST_UNCHECKED);
        EnableWindow(GetDlgItem(dlg, IDC_LABEL_HIDE), g_label_encrypt_default); /* hiding names needs encryption */
        SendDlgItemMessageW(dlg, IDC_LABEL_EDIT, EM_SETSEL, 0, -1);
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_LABEL_ENCRYPT) {
            int on = IsDlgButtonChecked(dlg, IDC_LABEL_ENCRYPT) == BST_CHECKED;
            EnableWindow(GetDlgItem(dlg, IDC_LABEL_HIDE), on);
            if (!on) CheckDlgButton(dlg, IDC_LABEL_HIDE, BST_UNCHECKED);
            return TRUE;
        }
        if (LOWORD(wp) == IDOK) {
            GetDlgItemTextW(dlg, IDC_LABEL_EDIT, g_label_buf, 256);
            if (!g_label_buf[0]) { MessageBeep(MB_ICONWARNING); return TRUE; }
            g_label_encrypt = IsDlgButtonChecked(dlg, IDC_LABEL_ENCRYPT) == BST_CHECKED;
            g_label_hide = g_label_encrypt && IsDlgButtonChecked(dlg, IDC_LABEL_HIDE) == BST_CHECKED;
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
static int g_pass_changing;     /* IDD_NEW_PASSPHRASE: changing the passphrase, not setting the first one */
static int g_pass_need_current; /* IDD_NEW_PASSPHRASE: also ask for the current one, which it checks */
static INT_PTR g_pass_rc;
/* With this set, IDD_PASSPHRASE tries what was typed before it closes: 1
 * when it may close (it worked, or failed in a way the caller reports), 0
 * to say in place that it is not right. */
typedef int (*pass_try_fn)(void *ctx, const char *secret, int recovery);
static pass_try_fn g_pass_try;
static void *g_pass_try_ctx;

static void free_secret(char *p);

/* Drop a dialog's rows from the control first_id down to (not including)
 * the row of next_id, and pull up what is below them. */
static void dialog_drop_rows(HWND dlg, int first_id, int next_id)
{
    HWND f = GetDlgItem(dlg, first_id), n = GetDlgItem(dlg, next_id);
    if (!f || !n) return;
    RECT a, b;
    GetWindowRect(f, &a);
    GetWindowRect(n, &b);
    POINT top = {a.left, a.top}, below = {b.left, b.top};
    ScreenToClient(dlg, &top);
    ScreenToClient(dlg, &below);
    int dy = below.y - top.y;
    if (dy <= 0) return;
    for (HWND c = GetWindow(dlg, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
        RECT r;
        GetWindowRect(c, &r);
        POINT pt = {r.left, r.top};
        ScreenToClient(dlg, &pt);
        if (pt.y >= below.y) SetWindowPos(c, NULL, pt.x, pt.y - dy, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        else if (pt.y >= top.y) ShowWindow(c, SW_HIDE);
    }
    RECT w;
    GetWindowRect(dlg, &w);
    SetWindowPos(dlg, NULL, w.left, w.top + dy / 2, w.right - w.left, w.bottom - w.top - dy, SWP_NOZORDER | SWP_NOACTIVATE);
}

/* A line under a field that says what is wrong, or nothing. */
static void pass_error(HWND dlg, int id, const wchar_t *text)
{
    wchar_t t[400];
    if (text && text[0]) swprintf(t, 400, L"\x2715 %ls", text);
    else t[0] = 0;
    SetDlgItemTextW(dlg, id, t);
}

/* The new passphrase's two rules, ticked as they are met; the button
 * waits for both. */
static void pass_rules(HWND dlg)
{
    wchar_t a[512], b[512];
    GetDlgItemTextW(dlg, IDC_PASS_EDIT, a, 512);
    GetDlgItemTextW(dlg, IDC_PASS_EDIT2, b, 512);
    int long_enough = wcslen(a) >= 8, same = a[0] && wcscmp(a, b) == 0;
    SecureZeroMemory(a, sizeof(a));
    SecureZeroMemory(b, sizeof(b));
    int current = !g_pass_need_current || GetWindowTextLengthW(GetDlgItem(dlg, IDC_PASS_CUR)) > 0;
    wchar_t t[200];
    swprintf(t, 200, L"%ls %ls", long_enough ? L"\x2713" : L"\x2022", T(L"At least 8 characters"));
    SetDlgItemTextW(dlg, IDC_PASS_RULE_LEN, t);
    swprintf(t, 200, L"%ls %ls", same ? L"\x2713" : L"\x2022", T(L"Both match"));
    SetDlgItemTextW(dlg, IDC_PASS_RULE_SAME, t);
    SetPropW(dlg, L"BrodalfRules", (HANDLE)(INT_PTR)((long_enough ? 1 : 0) | (same ? 2 : 0)));
    InvalidateRect(GetDlgItem(dlg, IDC_PASS_RULE_LEN), NULL, TRUE);
    InvalidateRect(GetDlgItem(dlg, IDC_PASS_RULE_SAME), NULL, TRUE);
    EnableWindow(GetDlgItem(dlg, IDOK), long_enough && same && current);
}

/* Show what is typed, or dots again. */
static void pass_show(HWND dlg)
{
    int show = IsDlgButtonChecked(dlg, IDC_PASS_SHOW) == BST_CHECKED;
    static const int ids[] = {IDC_PASS_EDIT, IDC_PASS_EDIT2, IDC_PASS_CUR};
    for (int i = 0; i < 3; i++) {
        HWND e = GetDlgItem(dlg, ids[i]);
        if (!e) continue;
        SendMessageW(e, EM_SETPASSWORDCHAR, show ? 0 : 0x25CF, 0);
        InvalidateRect(e, NULL, TRUE);
    }
}

static void pass_select(HWND dlg, int id)
{
    HWND e = GetDlgItem(dlg, id);
    SetFocus(e);
    SendMessageW(e, EM_SETSEL, 0, -1);
}

static INT_PTR CALLBACK pass_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_CTLCOLORSTATIC && !g_hc) {
        /* The rules and the errors in their colours. */
        int id = GetDlgCtrlID((HWND)lp);
        if (id == IDC_PASS_ERROR || id == IDC_PASS_CUR_ERROR || id == IDC_PASS_RULE_LEN || id == IDC_PASS_RULE_SAME) {
            LRESULT r;
            HDC dc = (HDC)wp;
            if (!theme_ctlcolor(msg, wp, lp, &r)) {
                SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
                r = (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
            }
            int met = (int)(INT_PTR)GetPropW(dlg, L"BrodalfRules");
            SetTextColor(dc, id == IDC_PASS_RULE_LEN ? ((met & 1) ? tone_text(TONE_SAFE) : theme_gray())
                             : id == IDC_PASS_RULE_SAME ? ((met & 2) ? tone_text(TONE_SAFE) : theme_gray())
                             : tone_text(TONE_BAD));
            return (INT_PTR)r;
        }
    }
    INT_PTR themed;
    if (theme_dialog(dlg, msg, wp, lp, &themed)) return themed;
    (void)lp;
    int is_new = GetDlgItem(dlg, IDC_PASS_EDIT2) != NULL;
    switch (msg) {
    case WM_INITDIALOG:
        SetDlgItemTextW(dlg, IDC_PASS_TEXT, g_pass_text);
        if (is_new) {
            if (g_pass_changing) {
                SetWindowTextW(dlg, T(L"Change the passphrase"));
                SetDlgItemTextW(dlg, IDC_PASS_NEW_LABEL, T(L"New passphrase:"));
                SetDlgItemTextW(dlg, IDOK, T(L"Change it"));
            }
            if (!g_pass_need_current || !bd_catalog_has_recovery_key(g_cat)) ShowWindow(GetDlgItem(dlg, IDC_PASS_RECOVERY), SW_HIDE);
            /* Already encrypted: nothing to tick. */
            if (g_pass_changing && bd_catalog_file_encrypted(g_cat)) dialog_drop_rows(dlg, IDC_PASS_CATALOG, IDC_PASS_RECOVERY);
            if (!g_pass_need_current) dialog_drop_rows(dlg, IDC_PASS_CUR_LABEL, IDC_PASS_NEW_LABEL);
            pass_rules(dlg);
            SetFocus(GetDlgItem(dlg, g_pass_need_current ? IDC_PASS_CUR : IDC_PASS_EDIT));
            return FALSE;
        }
        if (g_pass_mode != 1) ShowWindow(GetDlgItem(dlg, IDC_PASS_RECOVERY), SW_HIDE);
        if (g_pass_mode == 2) {
            /* A recovery key is long and read off paper: show what is typed. */
            SetWindowTextW(dlg, T(L"Enter the recovery key"));
            SendDlgItemMessageW(dlg, IDC_PASS_EDIT, EM_SETPASSWORDCHAR, 0, 0);
            ShowWindow(GetDlgItem(dlg, IDC_PASS_SHOW), SW_HIDE);
        }
        SetFocus(GetDlgItem(dlg, IDC_PASS_EDIT));
        return FALSE;
    case WM_COMMAND:
        if (HIWORD(wp) == EN_CHANGE) {
            if (LOWORD(wp) == IDC_PASS_EDIT && !is_new) pass_error(dlg, IDC_PASS_ERROR, NULL);
            if (LOWORD(wp) == IDC_PASS_CUR) pass_error(dlg, IDC_PASS_CUR_ERROR, NULL);
            if (is_new) pass_rules(dlg);
            return TRUE;
        }
        if (LOWORD(wp) == IDC_PASS_SHOW) {
            pass_show(dlg);
            return TRUE;
        }
        if (LOWORD(wp) == IDOK) {
            GetDlgItemTextW(dlg, IDC_PASS_EDIT, g_pass_buf, 512);
            if (is_new) {
                wchar_t again[512];
                GetDlgItemTextW(dlg, IDC_PASS_EDIT2, again, 512);
                int same = wcscmp(again, g_pass_buf) == 0;
                SecureZeroMemory(again, sizeof(again));
                if (wcslen(g_pass_buf) < 8 || !same) {
                    /* The ticks under the fields say which rule is not met. */
                    SecureZeroMemory(g_pass_buf, sizeof(g_pass_buf));
                    MessageBeep(MB_ICONWARNING);
                    return TRUE;
                }
                if (g_pass_need_current) {
                    wchar_t cur[512];
                    GetDlgItemTextW(dlg, IDC_PASS_CUR, cur, 512);
                    char *c = narrow(cur);
                    SecureZeroMemory(cur, sizeof(cur));
                    HCURSOR old = SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_WAIT));
                    bd_status st = bd_catalog_unlock(g_cat, c ? c : "");
                    SetCursor(old);
                    free_secret(c);
                    if (st != BD_OK) {
                        SecureZeroMemory(g_pass_buf, sizeof(g_pass_buf));
                        wchar_t *e = st == BD_ERR_PASSPHRASE ? NULL : widen(bd_catalog_error(g_cat));
                        pass_error(dlg, IDC_PASS_CUR_ERROR, e && e[0] ? e : T(L"That passphrase is not right."));
                        free(e);
                        pass_select(dlg, IDC_PASS_CUR);
                        return TRUE;
                    }
                }
                g_pass_catalog = IsWindowVisible(GetDlgItem(dlg, IDC_PASS_CATALOG)) && IsDlgButtonChecked(dlg, IDC_PASS_CATALOG) == BST_CHECKED;
            } else if (g_pass_try) {
                char *p = narrow(g_pass_buf);
                HCURSOR old = SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_WAIT));
                int done = g_pass_try(g_pass_try_ctx, p ? p : "", g_pass_mode == 2);
                SetCursor(old);
                free_secret(p);
                if (!done) {
                    SecureZeroMemory(g_pass_buf, sizeof(g_pass_buf));
                    pass_error(dlg, IDC_PASS_ERROR, g_pass_mode == 2 ? T(L"That recovery key is not right.")
                                                                     : T(L"That passphrase is not right."));
                    pass_select(dlg, IDC_PASS_EDIT);
                    return TRUE;
                }
            }
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        if (LOWORD(wp) == IDC_PASS_RECOVERY) { EndDialog(dlg, IDC_PASS_RECOVERY); return TRUE; }
        break;
    case WM_DESTROY:
        RemovePropW(dlg, L"BrodalfRules");
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
    g_pass_rc = rc;
    g_pass_mode = 0;
    char *p = rc == IDOK ? narrow(g_pass_buf) : NULL;
    SecureZeroMemory(g_pass_buf, sizeof(g_pass_buf));
    return p;
}

/* The recovery key, typed off the sheet. NULL if cancelled. */
static char *ask_recovery_key(HWND owner)
{
    g_pass_text = T(L"Type the recovery key from your recovery sheet. Dashes and spaces do not matter.");
    g_pass_mode = 2;
    INT_PTR rc = DialogBoxW(g_inst, MAKEINTRESOURCEW(IDD_PASSPHRASE), owner, pass_proc);
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
        *is_recovery = 1;
        return ask_recovery_key(owner);
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
    if (ask_verbs(owner, T(L"Unlocked with the recovery key. If you no longer know the passphrase, set a new one now. "
                           L"The recovery key keeps working.\n\nSet a new passphrase now?"), MB_YESNO | MB_ICONQUESTION, T(L"Set a new passphrase"), T(L"Not now")) == IDYES)
        set_passphrase_ui();
}

static void free_secret(char *p)
{
    if (!p) return;
    SecureZeroMemory(p, strlen(p));
    free(p);
}

static int unlock_try(void *ctx, const char *secret, int recovery)
{
    bd_status *s = ctx;
    *s = recovery ? bd_catalog_unlock_recovery(g_cat, secret) : bd_catalog_unlock(g_cat, secret);
    return *s != BD_ERR_PASSPHRASE;
}

/* Make sure the key is available. Only while no job runs. 1 if unlocked. */
static int unlock_ui(const wchar_t *why)
{
    if (!bd_catalog_has_passphrase(g_cat) || bd_catalog_is_unlocked(g_cat)) return 1;
    int recovery = 0;
    bd_status s = BD_ERR_PASSPHRASE;
    g_pass_try = unlock_try;
    g_pass_try_ctx = &s;
    char *p = ask_unlock_secret(g_main, why, bd_catalog_has_recovery_key(g_cat), &recovery);
    g_pass_try = NULL;
    if (!p) return 0;
    free_secret(p);
    if (s != BD_OK) {
        wchar_t *e = widen(bd_catalog_error(g_cat));
        report_status(g_main, s, 0, e && e[0] ? e : T(L"Could not unlock the encryption key."));
        free(e);
        return 0;
    }
    if (recovery) offer_new_passphrase(g_main);
    return 1;
}

/* Set the first passphrase, or change it. 1 on success. */
static int set_passphrase_ui(void)
{
    int changing = bd_catalog_has_passphrase(g_cat);
    /* One dialog: the current passphrase (when it is not entered yet), the
     * new one twice. */
    const wchar_t *text = changing ? T(L"The new passphrase replaces the current one. Write it down somewhere safe: without it, "
                                       L"encrypted copies cannot be read, by you or anyone.")
                                   : T(L"The passphrase protects encrypted drives and, if you choose, this database file. "
                                       L"Write it down somewhere safe: without it, encrypted copies cannot be read, by you or anyone.");
    g_pass_changing = changing;
    g_pass_need_current = changing && !bd_catalog_is_unlocked(g_cat);
    char *p = ask_passphrase(g_main, IDD_NEW_PASSPHRASE, text);
    if (!p && g_pass_rc == IDC_PASS_RECOVERY) {
        /* The current one is forgotten: the recovery key opens it, then the new one. */
        bd_status us = BD_ERR_PASSPHRASE;
        g_pass_try = unlock_try;
        g_pass_try_ctx = &us;
        char *k = ask_recovery_key(g_main);
        int typed = k != NULL;
        g_pass_try = NULL;
        free_secret(k);
        if (typed && us == BD_OK) {
            g_pass_need_current = 0;
            g_pass_changing = 1;
            p = ask_passphrase(g_main, IDD_NEW_PASSPHRASE, text);
        } else if (typed) {
            wchar_t *e = widen(bd_catalog_error(g_cat));
            report_status(g_main, us, 0, e && e[0] ? e : T(L"Could not unlock the encryption key."));
            free(e);
        }
    }
    g_pass_changing = g_pass_need_current = 0;
    if (!p) return 0;
    SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_WAIT));
    bd_status s = bd_catalog_set_passphrase(g_cat, p);
    free_secret(p);
    if (s == BD_OK && g_pass_catalog) s = bd_catalog_set_file_encrypted(g_cat, 1);
    if (s == BD_OK) s = bd_catalog_save(g_cat);
    if (s != BD_OK) {
        wchar_t *e = widen(bd_catalog_error(g_cat));
        report_status(g_main, s, 0, e && e[0] ? e : T(L"Could not set the passphrase."));
        free(e);
        return 0;
    }
    log_append(changing ? T(L"Passphrase changed.") : T(L"Passphrase set."));
    if (!changing && g_main &&
        ask_verbs(g_main, T(L"Print a recovery sheet now? It lists your drives and holds a recovery key, which opens your "
                            L"encrypted backups if you ever forget the passphrase."), MB_YESNO | MB_ICONQUESTION, T(L"Print it"), T(L"Not now")) == IDYES)
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
    if (media_encryption(g_risk_pick).encrypted && !unlock_ui(T(L"This drive is encrypted. Enter the passphrase to back up to it."))) return;
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

/* A drive's name alone, for a progress line ("Blue WD 4TB"), and whether
 * it is an extra place. On the worker thread. */
static void job_dest_label(int64_t id, wchar_t *out, size_t cap, int *secondary)
{
    media_one o;
    memset(&o, 0, sizeof(o));
    o.id = id;
    bd_list_media(g_cat, media_one_cb, &o);
    wchar_t *w = widen(o.found ? o.label : "?");
    swprintf(out, cap, L"%ls", w ? w : L"?");
    free(w);
    if (secondary) *secondary = o.found && o.info.secondary;
}

/* "Blue WD 4TB (kept in Box A)" */
static void drive_name(const char *label, const char *location, wchar_t *out, size_t cap)
{
    wchar_t *l = widen(label), *loc = widen(location);
    if (loc && *loc) swprintf(out, cap, T(L"%ls (kept in %ls)"), l ? l : L"", loc);
    else swprintf(out, cap, L"%ls", l ? l : L"");
    free(l);
    free(loc);
}

/* Check every copy on a drive: that it is there, its size and time, and
 * its checksum. */
static void enqueue_check(int64_t id)
{
    if (media_encryption(id).encrypted && !unlock_ui(T(L"This drive is encrypted. Enter the passphrase to check its copies."))) return;
    job *c = new_job(JOB_CHECK);
    if (!c) return;
    c->media_id = id;
    enqueue(c);
}

static void enqueue_verify(int64_t id)
{
    if (media_encryption(id).encrypted && !unlock_ui(T(L"This drive is encrypted. Enter the passphrase to read its copies."))) return;
    job *v = new_job(JOB_VERIFY);
    if (!v) return;
    v->media_id = id;
    enqueue(v);
}

/* A backup that wrote copies to drives still plugged in ended: offer, with
 * the button beside the bar, to read them back. It starts nothing. */
static void offer_read_back(const job *j)
{
    g_n_readback = 0;
    if (j->n_many) {
        for (int i = 0; i < j->n_many && i < 16; i++)
            if (j->many_copied[i] > 0) g_readback_ids[g_n_readback++] = j->many[i];
    } else if (j->copied > 0 && j->copied_to)
        g_readback_ids[g_n_readback++] = j->copied_to;
    /* Only drives that are still plugged in and not marked lost. */
    wtext names = {NULL, 0, 0};
    int n = 0;
    for (int i = 0; i < g_n_readback; i++) {
        media_one o;
        memset(&o, 0, sizeof(o));
        o.id = g_readback_ids[i];
        bd_list_media(g_cat, media_one_cb, &o);
        if (!o.found || !o.info.connected || o.info.lost_ms) continue;
        wchar_t name[600];
        drive_name(o.label, o.location, name, 600);
        wadd(&names, L"%ls%ls", n ? L", " : L"", name);
        g_readback_ids[n++] = o.id;
    }
    g_n_readback = n;
    if (n)
        swprintf(g_readback_tip, 2400,
                 TN(n, L"Checks the copies on %ls that were not checked since they were written: it reads each one and compares it with its "
                       L"checksum, to be sure it was written right. It changes nothing on the drive, and starts only when you press it.",
                    L"Checks the copies on %ls that were not checked since they were written: it reads each one and compares it with its "
                    L"checksum, to be sure it was written right. It changes nothing on the drives, and starts only when you press it."),
                 names.p ? names.p : L"");
    /* A read straight after the backup may come from what Windows still
     * holds in memory rather than from the drive itself. */
    if (n) {
        size_t used = wcslen(g_readback_tip);
        swprintf(g_readback_tip + used, 2400 - used, L"%ls",
                 T(L" Windows may still hold some of what was just written in memory. To read it from the drive itself, unplug "
                   L"the drive, plug it in again, and then press this."));
    }
    free(names.p);
    show_read_back(n > 0);
}

/* Read back was pressed: read back what the last backup wrote, one drive
 * after another. */
static void read_back_new(void)
{
    int64_t ids[16];
    int n = g_n_readback, declined = 0, queued = 0;
    memcpy(ids, g_readback_ids, sizeof(ids));
    show_read_back(0);
    for (int i = 0; i < n; i++) {
        media_one o;
        memset(&o, 0, sizeof(o));
        o.id = ids[i];
        bd_list_media(g_cat, media_one_cb, &o);
        if (!o.found || !o.info.connected) continue;
        /* An encrypted drive needs the passphrase: asked once. */
        if (media_encryption(ids[i]).encrypted &&
            (declined || !unlock_ui(T(L"This drive is encrypted. Enter the passphrase to read its copies.")))) {
            declined = 1;
            continue;
        }
        job *v = new_job(JOB_VERIFY);
        if (!v) continue;
        v->media_id = ids[i];
        v->just_written = 1;
        enqueue(v);
        queued++;
    }
    if (!queued && !declined)
        MessageBoxW(g_main, T(L"The drive is not plugged in any more. Plug it in, then press Check drive... to check its copies."), APP_NAME,
                    MB_ICONINFORMATION);
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
    if (o.info.oldest_unread_ms > 0) {
        format_time_ms(o.info.oldest_unread_ms, since, 64);
        swprintf(content, 1400, T(L"Some copies on %ls were never checked since they were written (the oldest %ls).%ls Checking changes "
                                L"nothing on the drive."), name,
                 since, o.info.check_due ? T(L" That is longer than Settings asks for, so a check is due.") : L"");
    } else if (o.info.oldest_check_ms > 0) {
        format_time_ms(o.info.oldest_check_ms, since, 64);
        swprintf(content, 1400, T(L"Some copies on %ls have not been checked since %ls.%ls Checking changes nothing on the drive."), name,
                 since, o.info.check_due ? T(L" That is longer than Settings asks for, so a check is due.") : L"");
    } else
        swprintf(content, 1400, T(L"Checking reads the copies on %ls and compares each one with its checksum. It changes nothing on the drive."), name);
    int days = bd_option_get(g_cat, "verify_days");
    if (days > 0)
        swprintf(verify, 400, T(L"Check copies not checked lately\nReads the copies not checked in the last %d days, and new ones. "
                              L"A full check spread over time."), days);
    else
        swprintf(verify, 400, T(L"Check copies not checked lately\nSettings has this set to every copy, so it is the same as checking every copy."));
    TASKDIALOG_BUTTON b[2] = {
        {611, T(L"Check every copy\nReads every copy and compares its size, time and checksum. Takes a while on a big drive.")},
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
    tc.pszMainInstruction = T(L"How should BRODALF check the drive?");
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
            swprintf(m, 1200, T(L"BRODALF could not use that drive: %ls"), e && *e ? e : T(L"it is not a BRODALF drive for this database."));
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
        swprintf(m, 1000, T(L"%ls is marked %ls, so nothing is backed up to it. If it is back and working, open Drives..., choose it "
                          L"and press Found it."), name, strcmp(o.info.lost_why, "broken") == 0 ? T(L"broken") : T(L"lost"));
        MessageBoxW(g_main, m, APP_NAME, MB_ICONINFORMATION);
        job_free(b);
        return;
    }
    for (;;) {
        bd_backup_preview_info pv;
        memset(&pv, 0, sizeof(pv));
        if (bd_backup_preview(g_cat, b->media_id, 0, &pv) != BD_OK) { enqueue(b); return; }
        if (g_changes_ok && g_changes_ok == b->media_id) pv.mass_change = 0;
        int unscanned = !pv.last_scan_ms && offer_scan;
        if (!pv.mass_change && !unscanned) { enqueue(b); return; }
        wtext t = {NULL, 0, 0};
        TASKDIALOG_BUTTON btn[2];
        wchar_t title[700];
        if (pv.mass_change) {
            swprintf(title, 700, T(L"Many files changed at once. Back up to %ls?"), name);
            if (pv.held_deleted > 0)
                wadd(&t, TN(pv.held_deleted, L"Of the %lld files whose copies on this drive were up to date, %lld have changed since and %lld "
                                             L"was deleted.",
                            L"Of the %lld files whose copies on this drive were up to date, %lld have changed since and %lld "
                            L"were deleted."),
                     (long long)pv.files_held, (long long)(pv.held_changed - pv.held_deleted), (long long)pv.held_deleted);
            else
                wadd(&t, T(L"%lld of the %lld files whose copies on this drive were up to date have changed since."),
                     (long long)pv.held_changed, (long long)pv.files_held);
            wadd(&t, L" %ls", T(L"That many at once can mean ransomware encrypted your files.\n\nBacking up overwrites nothing: the copies "
                               L"already on the drive stay as they are and the changed files go alongside them. If you did not change "
                               L"these files, look at them first, and restore from this drive instead."));
            btn[0].nButtonID = 622;
            btn[0].pszButtonText = T(L"Show the changed files");
            btn[1].nButtonID = 620;
            {
                /* What "anyway" copies. */
                static wchar_t anyway[400];
                wchar_t size[64], *dl = widen(o.label);
                format_bytes(pv.bytes_to_copy, size, 64);
                swprintf(anyway, 400, TN(pv.files_to_copy, L"Back up anyway: %lld file, %ls to %ls\nThe copies already on the drive are kept.",
                                         L"Back up anyway: %lld files, %ls to %ls\nThe copies already on the drive are kept."),
                         (long long)pv.files_to_copy, size, dl && *dl ? dl : name);
                free(dl);
                btn[1].pszButtonText = anyway;
            }
        } else {
            swprintf(title, 700, T(L"Scan your backup roots first?"));
            wadd(&t, T(L"Your backup roots have not been scanned yet, so BRODALF does not know of any files to put on %ls."), name);
            btn[0].nButtonID = 621;
            btn[0].pszButtonText = T(L"Scan first, then back up");
            btn[1].nButtonID = 620;
            btn[1].pszButtonText = T(L"Back up anyway\nOnly the database is saved to the drive.");
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
        if (show_changed(b->media_id)) { enqueue(b); return; }
    }
}

/* What did not fit waits for another drive: every drive gets whatever it
 * is missing, so any drive with room carries the backup on. */
static void after_backup(job *j)
{
    if (j->no_room <= 0 || j->n_many) return;
    wchar_t name[600], size[64], msg[1600];
    media_name(j->media_id, name, 600);
    format_bytes(j->bytes_no_room, size, 64);
    drive_list dl = load_drives();
    const drive_snap *self = find_drive(&dl, j->media_id), *best = NULL;
    if (self && self->secondary) { /* an extra place's size limit: the drives still get those files */
        free(dl.items);
        return;
    }
    for (int i = 0; i < dl.n; i++) {
        const drive_snap *d = &dl.items[i];
        if (!d->connected || d->lost_ms || d->secondary || d->id == j->media_id || strcmp(d->kind, "drive") != 0) continue;
        if (!best || d->freeb > best->freeb) best = d;
    }
    if (best) {
        wchar_t other[600];
        int64_t other_id = best->id;
        drive_name(best->label, best->location, other, 600);
        swprintf(msg, 1600, TN(j->no_room, L"%ls is full: %lld file (%ls) did not fit. It waits for another drive.\n\nBack up to %ls now?",
                               L"%ls is full: %lld files (%ls) did not fit. They wait for another drive.\n\nBack up to %ls now?"),
                 name, (long long)j->no_room, size, other);
        free(dl.items);
        if (ask_verbs(g_main, msg, MB_ICONQUESTION | MB_YESNO, T(L"Back up now"), T(L"Not now")) == IDYES &&
            (!media_encryption(other_id).encrypted || unlock_ui(T(L"This drive is encrypted. Enter the passphrase to back up to it.")))) {
            job *b = new_job(JOB_BACKUP);
            if (b) {
                b->media_id = other_id;
                start_backup(b, 0);
            }
        }
        return;
    }
    free(dl.items);
    wchar_t full[600];
    swprintf(full, 600, TN(j->no_room, L"%ls is full: %lld file (%ls) did not fit. It waits for another drive.",
                           L"%ls is full: %lld files (%ls) did not fit. They wait for another drive."),
             name, (long long)j->no_room, size);
    swprintf(msg, 1600, L"%ls\n\n%ls", full,
             T(L"Plug in another backup drive (or set up a new one with Back up... > Another drive or folder) and back up to it. "
               L"Each drive gets whatever it is missing."));
    MessageBoxW(g_main, msg, APP_NAME, MB_ICONINFORMATION);
}

typedef struct { wtext t; int n; int any_offline; int64_t ids[64]; int nid; } plan_text;

static int plan_text_cb(void *ctx, const bd_restore_step *st)
{
    plan_text *p = ctx;
    wchar_t name[600], size[64];
    drive_name(st->label, st->location, name, 600);
    format_bytes(st->bytes, size, 64);
    wadd(&p->t, TN(st->files, L"    %d. %ls: %lld file (%ls)%ls\n", L"    %d. %ls: %lld files (%ls)%ls\n"), ++p->n, name,
         (long long)st->files, size, st->connected ? T(L", plugged in") : L"");
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

/* The last restore, in words, for the Restore tab. */
static wchar_t g_last_restore[SUMMARY_LEN];
static int64_t g_last_restore_ms;

static void after_restore(job *j)
{
    {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        ULONGLONG t = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
        g_last_restore_ms = (int64_t)((t - 116444736000000000ULL) / 10000ULL);
        swprintf(g_last_restore, SUMMARY_LEN, L"%ls", j->summary);
    }
    /* What the restore left alone replaces the last restore's list. */
    free_conflicts(g_conflicts, g_n_conflicts);
    g_conflicts = j->conf;
    g_n_conflicts = j->n_conf;
    j->conf = NULL;
    j->n_conf = j->cap_conf = 0;
    /* No question: the job panel offers to compare them, and the Restore
     * tab lists them. */
    if (j->deleted || j->version_id) {
        /* Not kept waiting: the user picks them again. */
        if (j->offline > 0) {
            wchar_t line[300];
            swprintf(line, 300, TN(j->offline, L"%lld file is on drives that are not plugged in. Plug one in, then restore again.",
                                   L"%lld files are on drives that are not plugged in. Plug one in, then restore again."),
                     (long long)j->offline);
            log_append(line);
        }
        return;
    }
    if (j->n_picks) return;
    if (j->offline <= 0) {
        if (g_pending.active) log_append(T(L"The restore is complete: every file that has a copy is back."));
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
        wadd(&msg, TN(j->offline, L"%lld file is on drives that are not plugged in. Plug these in, one at a time, and press Restore... "
                                  L"after each to continue:\r\n%ls",
                      L"%lld files are on drives that are not plugged in. Plug these in, one at a time, and press Restore... "
                      L"after each to continue:\r\n%ls"),
             (long long)j->offline, p.t.p);
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
        !unlock_ui(T(L"Some copies are on an encrypted drive. Enter the passphrase to restore them.")))
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
    wchar_t vname[MAX_PATH + 1] = L"", size[64] = L"", used[64] = L"", content[900];
    GetVolumeInformationW(root, vname, MAX_PATH + 1, NULL, NULL, NULL, NULL, 0);
    ULARGE_INTEGER total, freeb;
    if (GetDiskFreeSpaceExW(root, NULL, &total, &freeb)) {
        format_bytes((int64_t)total.QuadPart, size, 64);
        format_bytes((int64_t)(total.QuadPart - freeb.QuadPart), used, 64);
    }
    /* Its name, its size and how much of it is used: what is on it already. */
    if (*vname && *size)
        swprintf(content, 900, T(L"%ls (%ls) holds %ls, of which %ls is used. It is not one of your backup drives."), vname, root, size, used);
    else if (*size)
        swprintf(content, 900, T(L"The drive at %ls holds %ls, of which %ls is used. It is not one of your backup drives."), root, size, used);
    else if (*vname)
        swprintf(content, 900, T(L"\"%ls\" (%ls) was plugged in. It is not one of your backup drives."), vname, root);
    else
        swprintf(content, 900, T(L"The drive at %ls was plugged in. It is not one of your backup drives."), root);
    TASKDIALOG_BUTTON b[3] = {
        {640, T(L"Set it up as a backup drive\nYou name it first. BRODALF then makes its own folder on it and backs up to it. "
                L"Nothing else on the drive is touched.")},
        {641, T(L"Not now")},
        {642, T(L"Never ask about this drive\nSettings... can ask about it again.")},
    };
    TASKDIALOGCONFIG tc;
    memset(&tc, 0, sizeof(tc));
    tc.cbSize = sizeof(tc);
    tc.hwndParent = g_main;
    tc.hInstance = g_inst;
    tc.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
    tc.pszWindowTitle = APP_NAME;
    tc.pszMainInstruction = T(L"Use this drive for backups?");
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
        if (o.info.secondary) swprintf(line, 1400, T(L"Extra place found: %ls."), name);
        else swprintf(line, 1400, T(L"%ls is plugged in."), name);
        if (o.info.lost_ms)
            wcsncat(line, T(L" It is marked lost or broken, so its copies do not count: if it is back and working, open Drives..., "
                          L"choose it and press Found it."), 1399 - wcslen(line));
        else if (o.info.check_due && n_due < 16)
            due[n_due++] = o.id;
        if (g_pending.active && pending_needs(o.id))
            wcsncat(line, T(L" It holds files for the restore that is waiting: press Restore... to continue it."), 1399 - wcslen(line));
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
        swprintf(msg, 1200, T(L"%ls is due for a check: some of its copies have not been checked since %ls.\n\n"
                            L"Check every copy on it now? It changes nothing on the drive. (No: BRODALF does not ask again until "
                            L"it is opened again.)"), name, since);
        if (ask_verbs(g_main, msg, MB_ICONQUESTION | MB_YESNO, T(L"Check it now"), T(L"Not now")) == IDYES) enqueue_check(due[i]);
    }
    for (int i = 0; i < j->n_other; i++) offer_new_disk(j->other[i]);
}

/* The skip list, with the edit box's \r\n line ends. */
/* The comments of the list BRODALF starts with, shown in the language
 * chosen (a comment line is only for reading; saving it translated
 * changes nothing that is left out). */
static const wchar_t *const g_skip_comments[] = {
    N_(L"Temporary and partly downloaded files"),
    N_(L"System files"),
    N_(L"Caches and things a program can download or build again"),
};

static wchar_t *skip_text_for_edit(const char *list)
{
    wtext t = {NULL, 0, 0};
    wchar_t *w = widen(list);
    for (wchar_t *line = w; line && *line;) {
        wchar_t *end = wcschr(line, L'\n');
        size_t len = end ? (size_t)(end - line) : wcslen(line);
        if (len && line[len - 1] == L'\r') len--;
        const wchar_t *shown = NULL;
        for (size_t i = 0; i < sizeof(g_skip_comments) / sizeof(g_skip_comments[0]); i++) {
            size_t cl = wcslen(g_skip_comments[i]);
            if (len == cl + 2 && wcsncmp(line, L"# ", 2) == 0 && wcsncmp(line + 2, g_skip_comments[i], cl) == 0)
                shown = T(g_skip_comments[i]);
        }
        if (shown) wadd(&t, L"# %ls", shown);
        else wadd(&t, L"%.*ls", (int)len, line);
        if (!end) break;
        wadd(&t, L"\r\n");
        line = end + 1;
    }
    free(w);
    return t.p ? t.p : _wcsdup(L"");
}

/* The list being edited: 0 the one for every backup root, else one root's own. */
static int64_t g_skip_source;
static wchar_t g_skip_root_name[300];

static INT_PTR CALLBACK skip_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    INT_PTR themed;
    if (theme_dialog(dlg, msg, wp, lp, &themed)) return themed;
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
            swprintf(title, 400, T(L"What to leave out in %ls"), g_skip_root_name);
            SetWindowTextW(dlg, title);
            swprintf(text, 900, T(L"Scans of %ls leave out what matches these, as well as what the list for every backup root (Settings) "
                                L"leaves out. One per line: * matches anything, ? one character, a line ending in / matches folders. "
                                L"A file now left out shows among the deleted files, for you to keep or forget."), g_skip_root_name);
            SetDlgItemTextW(dlg, IDC_SKIP_TEXT, text);
            SetDlgItemTextW(dlg, IDC_SKIP_DEFAULT, T(L"Clear"));
        } else {
            SetDlgItemTextW(dlg, IDC_SKIP_TEXT,
                            T(L"Scans of every backup root leave out files and folders that match these, one per line. * matches anything, "
                            L"? one character. End a line with / for folders only (and everything in them). A line with / in the middle, "
                            L"like Photos/Exports/, is a path inside a backup root. Lines starting with # are notes."));
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
            if (s == BD_OK) s = ui_save();
            if (s != BD_OK) {
                wchar_t *e = widen(bd_catalog_error(g_cat));
                MessageBoxW(dlg, e ? e : T(L"Could not save the list."), APP_NAME, MB_ICONWARNING);
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
        log_append(T(L"The skip list changed. Scanning your backup roots again."));
        enqueue(new_job(JOB_SCAN));
    }
}

static void settings_command(int cmd);

static void cmd_security(void)
{
    int has = bd_catalog_has_passphrase(g_cat), unlocked = bd_catalog_is_unlocked(g_cat);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, ID_MENU_SET_PASS, has ? T(L"Change the passphrase...") : T(L"Set a passphrase..."));
    AppendMenuW(m, MF_STRING | (has ? 0 : MF_GRAYED) | (bd_catalog_file_encrypted(g_cat) ? MF_CHECKED : 0),
                ID_MENU_ENCRYPT_CATALOG, T(L"Encrypt the database file"));
    AppendMenuW(m, MF_STRING | (has && unlocked ? 0 : MF_GRAYED), ID_MENU_LOCK, T(L"Forget the passphrase until it is needed"));
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    HMENU remind = CreatePopupMenu();
    int days = bd_option_get(g_cat, "check_days");
    AppendMenuW(remind, MF_STRING | (days == 90 ? MF_CHECKED : 0), ID_MENU_CHECK_90, T(L"Every 3 months"));
    AppendMenuW(remind, MF_STRING | (days == 180 ? MF_CHECKED : 0), ID_MENU_CHECK_180, T(L"Every 6 months"));
    AppendMenuW(remind, MF_STRING | (days == 365 ? MF_CHECKED : 0), ID_MENU_CHECK_365, T(L"Every year"));
    AppendMenuW(remind, MF_STRING | (days == 0 ? MF_CHECKED : 0), ID_MENU_CHECK_NEVER, T(L"Never"));
    AppendMenuW(m, MF_POPUP, (UINT_PTR)remind, T(L"A plugged-in drive offers a check after"));
    HMENU verify = CreatePopupMenu();
    int vd = bd_option_get(g_cat, "verify_days");
    AppendMenuW(verify, MF_STRING | (vd == 7 ? MF_CHECKED : 0), ID_M_VERIFY_7, T(L"Copies not checked in a week"));
    AppendMenuW(verify, MF_STRING | (vd == 30 ? MF_CHECKED : 0), ID_M_VERIFY_30, T(L"Copies not checked in a month"));
    AppendMenuW(verify, MF_STRING | (vd == 90 ? MF_CHECKED : 0), ID_M_VERIFY_90, T(L"Copies not checked in 3 months"));
    AppendMenuW(verify, MF_STRING | (vd == 0 ? MF_CHECKED : 0), ID_M_VERIFY_NEVER, T(L"Every copy"));
    AppendMenuW(m, MF_POPUP, (UINT_PTR)verify, T(L"Check copies not checked lately: which copies"));
    HMENU reserve = CreatePopupMenu();
    int rmb = bd_option_get(g_cat, "reserve_mb");
    AppendMenuW(reserve, MF_STRING | (rmb == 256 ? MF_CHECKED : 0), ID_MENU_RESERVE_256, T(L"256 MB"));
    AppendMenuW(reserve, MF_STRING | (rmb == 512 ? MF_CHECKED : 0), ID_MENU_RESERVE_512, T(L"512 MB"));
    AppendMenuW(reserve, MF_STRING | (rmb == 1024 ? MF_CHECKED : 0), ID_MENU_RESERVE_1024, T(L"1 GB"));
    AppendMenuW(reserve, MF_STRING | (rmb == 2048 ? MF_CHECKED : 0), ID_MENU_RESERVE_2048, T(L"2 GB"));
    AppendMenuW(m, MF_POPUP, (UINT_PTR)reserve, T(L"Space kept free on each drive for the database's copies"));
    AppendMenuW(m, MF_STRING | (bd_option_get(g_cat, "low_priority") ? MF_CHECKED : 0), ID_MENU_LOW_PRIORITY,
                T(L"Let other programs come first while a job runs"));
    AppendMenuW(m, MF_STRING | (bd_option_get(g_cat, "parallel_copies") > 0 ? MF_CHECKED : 0), ID_MENU_PARALLEL,
                T(L"Back up to several drives at once"));
    AppendMenuW(m, MF_STRING, ID_MENU_SKIP, T(L"What to leave out in every backup root..."));
    {
        DWORD n_ignored = 0;
        HKEY k;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, IGNORED_KEY, 0, KEY_READ, &k) == ERROR_SUCCESS) {
            RegQueryInfoKeyW(k, NULL, NULL, NULL, NULL, NULL, NULL, &n_ignored, NULL, NULL, NULL, NULL);
            RegCloseKey(k);
        }
        if (n_ignored) {
            wchar_t text[200];
            swprintf(text, 200, TN((int)n_ignored, L"Ask again about the %d drive you said never to ask about", L"Ask again about the %d drives you said never to ask about"),
                     (int)n_ignored);
            AppendMenuW(m, MF_STRING, ID_MENU_UNIGNORE, text);
        }
    }
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
        if (on && !unlock_ui(T(L"Enter the passphrase to encrypt the database file."))) return;
        bd_status s = bd_catalog_set_file_encrypted(g_cat, on);
        if (s == BD_OK) s = bd_catalog_save(g_cat);
        if (s != BD_OK) {
            wchar_t *e = widen(bd_catalog_error(g_cat));
            report_status(g_main, s, 0, e && e[0] ? e : T(L"Could not change the database encryption."));
            free(e);
            return;
        }
        log_append(on ? T(L"The database file is now encrypted. BRODALF will ask for the passphrase when it opens.")
                      : T(L"The database file is no longer encrypted."));
    } else if (cmd == ID_MENU_LOCK) {
        bd_catalog_lock_key(g_cat);
        log_append(T(L"Passphrase forgotten. BRODALF will ask for it again when it needs it."));
    } else if (cmd == ID_MENU_SKIP) {
        cmd_skip(0, NULL);
    } else if (cmd == ID_MENU_UNIGNORE) {
        RegDeleteKeyW(HKEY_CURRENT_USER, IGNORED_KEY);
        log_append(T(L"BRODALF asks again about every drive that is not a backup drive when it is plugged in."));
    } else if (cmd >= ID_M_VERIFY_7 && cmd <= ID_M_VERIFY_NEVER) {
        static const int choice[] = {7, 30, 90, 0};
        int d = choice[cmd - ID_M_VERIFY_7];
        if (bd_option_set(g_cat, "verify_days", d) == BD_OK && ui_save() == BD_OK) {
            wchar_t line[200];
            if (d) swprintf(line, 200, T(L"Check copies not checked lately now reads the copies not checked in %d days, and new ones."), d);
            else wcscpy(line, T(L"Check copies not checked lately now reads every copy on the drive."));
            log_append(line);
        }
    } else if (cmd >= ID_MENU_CHECK_90 && cmd <= ID_MENU_CHECK_NEVER) {
        static const int choice[] = {90, 180, 365, 0};
        int d = choice[cmd - ID_MENU_CHECK_90];
        if (bd_option_set(g_cat, "check_days", d) == BD_OK && ui_save() == BD_OK) {
            wchar_t line[200];
            if (d) swprintf(line, 200, T(L"A plugged-in drive whose copies have not been checked in %d days offers a check."), d);
            else wcscpy(line, T(L"Drives never offer a check by themselves."));
            log_append(line);
            update_drives_label();
        }
    } else if (cmd >= ID_MENU_RESERVE_256 && cmd <= ID_MENU_RESERVE_2048) {
        static const int choice[] = {256, 512, 1024, 2048};
        int mb = choice[cmd - ID_MENU_RESERVE_256];
        if (bd_option_set(g_cat, "reserve_mb", mb) == BD_OK && ui_save() == BD_OK) {
            wchar_t line[200];
            swprintf(line, 200, T(L"Backups now keep %d MB free on each drive for the database's copies."), mb);
            log_append(line);
            update_drives_label();
        }
    } else if (cmd == ID_MENU_LOW_PRIORITY) {
        int on = !bd_option_get(g_cat, "low_priority");
        if (bd_option_set(g_cat, "low_priority", on) == BD_OK && ui_save() == BD_OK)
            log_append(on ? T(L"Jobs now let other programs come first: they may take longer while the PC is busy.")
                          : T(L"Jobs now run at normal speed, even while other programs are busy."));
    } else if (cmd == ID_MENU_PARALLEL) {
        int on = bd_option_get(g_cat, "parallel_copies") <= 0;
        if (on &&
            MessageBoxW(g_main, T(L"Back up to several drives at once?\n\nWhen you back up to every plugged-in drive, BRODALF then reads "
                                L"each file once and writes it to all of them at the same time. With two or more drives that is "
                                L"quicker than one drive after another.\n\nWhat it means for your PC: it works harder while the "
                                L"backup runs. The USB ports, memory and processor are shared by every drive at once, so other "
                                L"programs may be slower, and the backup goes at the pace of the slowest drive.\n\nWhen it is off "
                                L"(as it starts), drives are backed up one after another."),
                        APP_NAME, MB_ICONQUESTION | MB_OKCANCEL) != IDOK)
            return;
        if (bd_option_set(g_cat, "parallel_copies", on) == BD_OK && ui_save() == BD_OK)
            log_append(on ? T(L"Backups to every plugged-in drive now write to all of them at once.")
                          : T(L"Backups to every plugged-in drive now go one drive after another."));
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
            {600, T(L"Put a new recovery key on it\nIt opens your encrypted backups if the passphrase is lost. "
                  L"The key on any earlier sheet stops working.")},
            {601, T(L"Leave the recovery key off")},
        };
        TASKDIALOGCONFIG tc;
        memset(&tc, 0, sizeof(tc));
        tc.cbSize = sizeof(tc);
        tc.hwndParent = g_main;
        tc.hInstance = g_inst;
        tc.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
        tc.dwCommonButtons = TDCBF_CANCEL_BUTTON;
        tc.pszWindowTitle = APP_NAME;
        tc.pszMainInstruction = T(L"Put a recovery key on the sheet?");
        tc.pszContent = T(L"Your backups are encrypted. The sheet lists your drives and what is on them. It never shows the passphrase.");
        tc.cButtons = 2;
        tc.pButtons = b;
        int pressed = IDCANCEL;
        if (FAILED(TaskDialogIndirect(&tc, &pressed, NULL, NULL)) || (pressed != 600 && pressed != 601)) return;
        with_key = pressed == 600;
    }
    if (with_key) {
        if (!unlock_ui(T(L"Enter the passphrase to make a recovery key."))) return;
        bd_status s = bd_catalog_make_recovery_key(g_cat, key);
        if (s == BD_OK) s = bd_catalog_save(g_cat);
        if (s != BD_OK) {
            SecureZeroMemory(key, sizeof(key));
            wchar_t *e = widen(bd_catalog_error(g_cat));
            report_status(g_main, s, 0, e && e[0] ? e : T(L"Could not make a recovery key."));
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
        log_append(T(L"Made a new recovery key. Drives that are not plugged in learn it at their next backup."));
    }

    wchar_t path[MAX_PATH * 2];
    wcscpy(path, T(L"BRODALF recovery sheet.html"));
    OPENFILENAMEW of;
    memset(&of, 0, sizeof(of));
    of.lStructSize = sizeof(of);
    of.hwndOwner = g_main;
    wchar_t filter_text[256];
    of.lpstrFilter = file_filter(filter_text, 256, 2, N_(L"Web page (*.html)"), L"*.html");
    of.lpstrFile = path;
    of.nMaxFile = MAX_PATH * 2;
    of.lpstrDefExt = L"html";
    of.lpstrTitle = T(L"Save the recovery sheet");
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
            MessageBoxW(g_main, T(L"The new recovery key was not saved anywhere, so it cannot be used. Make the sheet again "
                                L"to get one."), APP_NAME, MB_ICONINFORMATION);
        return;
    }
    if (s != BD_OK) {
        wchar_t *e = widen(bd_catalog_error(g_cat));
        report_status(g_main, s, 0, e && e[0] ? e : T(L"Could not write the recovery sheet."));
        free(e);
        return;
    }
    ShellExecuteW(g_main, L"open", path, NULL, NULL, SW_SHOWNORMAL);
    if (with_key)
        MessageBoxW(g_main, T(L"The sheet is open in your browser. Print it and keep it somewhere safe, away from the PC and "
                            L"the drives. Then delete the file: anyone who finds the recovery key can read your encrypted "
                            L"backups."), APP_NAME, MB_ICONINFORMATION);
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

enum { PLACE_GUESS = 300, PLACE_PICK, PLACE_LATER, PLACE_RESTORE };

/* Ask where a protected folder is now and point the catalog there. 1 if it
 * was changed, 2 if the user would rather restore it from the drives (only
 * offered when it cannot be found). Nothing is scanned here; the caller
 * saves and scans. */
static int relocate_source(const source_place *sp)
{
    wchar_t guess[MAX_PATH * 4], guess_btn[MAX_PATH * 4 + 96], head[512], body[MAX_PATH * 4 + 512];
    int have_guess = guess_new_place(sp->path, guess, MAX_PATH * 4);
    int here = is_folder(sp->path);
    if (here) swprintf(head, 512, T(L"Where is \"%ls\" on this PC?"), sp->name);
    else swprintf(head, 512, T(L"\"%ls\" cannot be found"), sp->name);
    if (here)
        swprintf(body, MAX_PATH * 4 + 512, T(L"BRODALF backs up %ls. If you moved this backup root, choose where it is now. Its files "
                                            L"keep their copies and history."), sp->path);
    else
        swprintf(body, MAX_PATH * 4 + 512, T(L"%ls was not there when BRODALF scanned. Nothing in it is marked deleted. If it is "
                                            L"somewhere else, for example under another user name or on another drive letter, choose "
                                            L"where it is now: its files keep their copies and history."), sp->path);
    for (;;) {
        TASKDIALOG_BUTTON b[4];
        int n = 0;
        if (have_guess) {
            swprintf(guess_btn, MAX_PATH * 4 + 96, T(L"Use %ls\nThe same folder in your user folder on this PC"), guess);
            b[n].nButtonID = PLACE_GUESS;
            b[n++].pszButtonText = guess_btn;
        }
        b[n].nButtonID = PLACE_PICK;
        b[n++].pszButtonText = here ? T(L"Choose where it is now...") : T(L"It is somewhere else: choose where...");
        if (!here) {
            b[n].nButtonID = PLACE_RESTORE;
            b[n++].pszButtonText = T(L"Restore it from my backup drives...\nBRODALF says which drives to plug in, then puts the files "
                                     L"in a folder you choose.");
        }
        b[n].nButtonID = PLACE_LATER;
        b[n++].pszButtonText = here ? T(L"Leave it where it is")
                                    : T(L"Leave it for now\nIts files are not marked deleted. The next scan asks again.");
        TASKDIALOGCONFIG tc;
        memset(&tc, 0, sizeof(tc));
        tc.cbSize = sizeof(tc);
        tc.hwndParent = g_main;
        tc.hInstance = g_inst;
        tc.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
        tc.pszWindowTitle = APP_NAME;
        if (!here) tc.pszMainIcon = TD_WARNING_ICON;
        tc.pszMainInstruction = head;
        tc.pszContent = body;
        tc.cButtons = (UINT)n;
        tc.pButtons = b;
        int pressed = IDCANCEL;
        if (FAILED(TaskDialogIndirect(&tc, &pressed, NULL, NULL))) return 0;

        wchar_t *chosen = NULL;
        if (pressed == PLACE_RESTORE) return 2;
        if (pressed == PLACE_GUESS) chosen = _wcsdup(guess);
        else if (pressed == PLACE_PICK) {
            char *f = pick_folder(g_main, T(L"Choose where this backup root is now."));
            if (!f) continue;
            chosen = widen(f);
            free(f);
        } else return 0;
        if (!chosen) return 0;

        if (folder_is_empty(chosen)) {
            wchar_t warn[MAX_PATH * 4 + 512];
            swprintf(warn, MAX_PATH * 4 + 512, T(L"%ls is empty. If BRODALF backs up this folder, every file of \"%ls\" will look "
                                                L"deleted.\n\nTo get the files back instead, choose No and restore the backup root "
                                                L"from your drives.\n\nUse this empty folder anyway?"),
                     chosen, sp->name);
            if (ask_verbs(g_main, warn, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2, T(L"Use it anyway"), T(L"Cancel")) != IDYES) { free(chosen); continue; }
        }
        char *p = narrow(chosen);
        bd_status s = p ? bd_source_relocate(g_cat, sp->id, p) : BD_ERR_NOMEM;
        free(p);
        if (s == BD_OK) {
            wchar_t line[MAX_PATH * 4 + 128];
            swprintf(line, MAX_PATH * 4 + 128, T(L"Backup root \"%ls\" is now at %ls"), sp->name, chosen);
            log_append(line);
            free(chosen);
            return 1;
        }
        free(chosen);
        wchar_t *e = widen(bd_catalog_error(g_cat));
        MessageBoxW(g_main, e ? e : T(L"That folder cannot be used."), APP_NAME, MB_ICONWARNING);
        free(e);
    }
}

static void places_changed(void)
{
    ui_save(); /* says so when it is done if it could not be saved */
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
        int r = relocate_source(sp);
        if (r == 1) changed = 1;
        else if (r == 2) restore_selection(1, sp->id);
    }
    source_places_free(&l);
    if (changed) {
        places_changed();
        enqueue(new_job(JOB_SCAN));
    }
}

static void relocate_one(const source_place *sp)
{
    int r = relocate_source(sp);
    if (r == 2) restore_selection(1, sp->id);
    else if (r == 1) {
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
        MessageBoxW(g_main, T(L"Select the backup root in the list first."), APP_NAME, MB_ICONINFORMATION);
    else
        relocate_one(sp);
    source_places_free(&l);
}

/* ---- Commands ----------------------------------------------------------- */

static void cmd_add_folder(void)
{
    char *folder = pick_folder(g_main, T(L"Choose a folder to back up. BRODALF will scan it and keep track of every file inside."));
    if (!folder) return;
    int64_t id;
    if (bd_source_add(g_cat, folder, &id) != BD_OK) {
        wchar_t *e = widen(bd_catalog_error(g_cat));
        MessageBoxW(g_main, e, APP_NAME, MB_ICONWARNING);
        free(e);
    } else {
        wchar_t *w = widen(folder), line[1024];
        swprintf(line, 1024, T(L"New backup root: %ls"), w);
        log_append(line);
        free(w);
        rebuild_tree();
        enqueue(new_job(JOB_SCAN));
    }
    free(folder);
}

typedef struct {
    HMENU menu;
    int count, greyed_lost, usable;
    int64_t to_copy;     /* backup menus: files every usable drive would get, together */
    wchar_t names[400];  /* and their names, in the order they are backed up */
} menu_ctx;

/* One drive in the Back up or Check menu, saying what choosing it does:
 * how much a backup would copy, or how many copies a check reads and when
 * they were last checked. The drive's folder goes on the right. */
static int drive_menu_cb(void *ctx, const bd_media_info *m)
{
    menu_ctx *c = ctx;
    if (!m->connected || strcmp(m->kind, "drive") != 0) return 0; /* a cloud account from an earlier version is never listed */
    wchar_t *l = widen(m->label), *root = widen(m->last_root), text[600], freeb[64], what[400];
    const drive_fig *g = fig_of(m->media_id);
    int backup = c->greyed_lost;
    format_bytes(m->free_bytes, freeb, 64);
    if (m->lost_ms) {
        swprintf(what, 400, L"%ls", strcmp(m->lost_why ? m->lost_why : "", "broken") == 0 ? T(L"marked broken") : T(L"marked lost"));
    } else if (!backup) {
        wchar_t when[96];
        if (g && g->has_ms && g->ms.last_check_ms) friendly_time(g->ms.last_check_ms, when, 96);
        swprintf(what, 400, TN(m->copies, L"%lld copy, last checked %ls", L"%lld copies, last checked %ls"), (long long)m->copies,
                 g && g->has_ms && g->ms.last_check_ms ? when : T(L"never"));
    } else if (m->secondary) {
        if (g && g->has_ps && g->ps.waiting_files > 0) {
            wchar_t size[64];
            format_bytes(g->ps.waiting_bytes, size, 64);
            swprintf(what, 400, TN(g->ps.waiting_files, L"extra place, %lld starred file to copy (%ls)", L"extra place, %lld starred files to copy (%ls)"),
                     (long long)g->ps.waiting_files, size);
            c->to_copy += g->ps.waiting_files;
        } else {
            swprintf(what, 400, L"%ls", T(L"extra place, its starred files are up to date"));
        }
    } else if (g && g->has_pv && g->pv.files_to_copy > 0) {
        wchar_t size[64];
        format_bytes(g->pv.bytes_to_copy, size, 64);
        swprintf(what, 400, TN(g->pv.files_to_copy, L"%lld file to copy, %ls (%ls free)", L"%lld files to copy, %ls (%ls free)"),
                 (long long)g->pv.files_to_copy, size, freeb);
        c->to_copy += g->pv.files_to_copy;
    } else if (g && g->has_pv) {
        swprintf(what, 400, T(L"up to date (%ls free)"), freeb);
    } else {
        swprintf(what, 400, T(L"%ls free"), freeb);
    }
    swprintf(text, 600, L"%ls: %ls%ls\t%ls", l, what, m->hide_names ? T(L", encrypted, names hidden") : m->encrypted ? T(L", encrypted") : L"",
             root ? root : L"");
    if (!m->lost_ms) {
        c->usable++;
        size_t len = wcslen(c->names);
        int parallel = bd_option_get(g_cat, "parallel_copies") > 0;
        if (len < 300) swprintf(c->names + len, 400 - len, L"%ls%ls", len ? (parallel ? L", " : T(L", then ")) : L"", l);
    }
    /* A drive marked lost or broken takes no backups: shown greyed. */
    AppendMenuW(c->menu, MF_STRING | (m->lost_ms && c->greyed_lost ? MF_GRAYED : 0), (UINT_PTR)(ID_MENU_DRIVE_BASE + m->media_id), text);
    c->count++;
    free(l);
    free(root);
    return 0;
}

/* Returns a media id, 0 for "another drive or folder", -1 if cancelled.
 * allow_other 2 also offers every plugged-in drive at once: -2. */
static int64_t choose_drive(HWND button, int allow_other)
{
    menu_ctx c;
    memset(&c, 0, sizeof(c));
    c.menu = CreatePopupMenu();
    c.greyed_lost = allow_other;
    bd_list_media(g_cat, drive_menu_cb, &c);
    if (allow_other == 2 && c.usable >= 2) {
        wchar_t every[600];
        if (bd_option_get(g_cat, "parallel_copies") > 0)
            swprintf(every, 600, TN(c.to_copy, L"Every plugged-in drive at once (%ls): %lld file to copy", L"Every plugged-in drive at once (%ls): %lld files to copy"),
                     c.names, (long long)c.to_copy);
        else
            swprintf(every, 600, TN(c.to_copy, L"Every plugged-in drive, one after another (%ls): %lld file to copy",
                                    L"Every plugged-in drive, one after another (%ls): %lld files to copy"),
                     c.names, (long long)c.to_copy);
        AppendMenuW(c.menu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(c.menu, MF_STRING, ID_MENU_EVERY, every);
    }
    if (allow_other) {
        if (c.count) AppendMenuW(c.menu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(c.menu, MF_STRING, ID_MENU_OTHER, T(L"Another drive or folder..."));
    } else if (!c.count) {
        DestroyMenu(c.menu);
        no_drive_message();
        return -1;
    }
    RECT r;
    GetWindowRect(button, &r);
    int cmd = TrackPopupMenu(c.menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, r.left, r.bottom, 0, g_main, NULL);
    DestroyMenu(c.menu);
    if (cmd == ID_MENU_OTHER) return 0;
    if (cmd == ID_MENU_EVERY) return -2;
    if (cmd >= ID_MENU_DRIVE_BASE) return cmd - ID_MENU_DRIVE_BASE;
    return -1;
}

static void add_disk(void);

static void cmd_backup(void)
{
    cmd_backup_at(g_buttons[2]);
}

static void add_disk(void)
{
    char *root = pick_folder(g_main, T(L"Choose the drive (or a folder on it) to back up to."));
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
            wchar_t *w = widen(j->root), vname[MAX_PATH + 1] = L"";
            const wchar_t *slash = w ? wcsrchr(w, L'\\') : NULL;
            int whole = !w || wcslen(w) <= 3 || !slash || !slash[1];
            /* A whole drive is called what its own label says, like "SPARE". */
            if (whole && w) {
                wchar_t vroot[8];
                swprintf(vroot, 8, L"%lc:\\", w[0]);
                GetVolumeInformationW(vroot, vname, MAX_PATH + 1, NULL, NULL, NULL, NULL, 0);
            }
            wcsncpy(g_label_buf, whole ? (vname[0] ? vname : T(L"Backup drive")) : slash + 1, 255);
            g_label_buf[255] = 0;
            free(w);
            if (DialogBoxW(g_inst, MAKEINTRESOURCEW(IDD_LABEL), g_main, label_proc) != IDOK) { job_free(j); return; }
            j->label = narrow(g_label_buf);
            j->location = narrow(g_label_location);
            if (g_label_encrypt) {
                int ok = bd_catalog_has_passphrase(g_cat) ? unlock_ui(T(L"Enter the passphrase to set up an encrypted drive."))
                                                          : set_passphrase_ui();
                if (!ok) { job_free(j); return; }
                j->flags = BD_MEDIA_ENCRYPTED | (g_label_hide ? BD_MEDIA_HIDE_NAMES : 0);
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
        MessageBoxW(g_main, T(L"Select a backup root, a folder inside it, or a file to restore."), APP_NAME, MB_ICONINFORMATION);
        return;
    }
    if (media_encryption(0).any_connected_encrypted && !unlock_ui(T(L"Some copies are on an encrypted drive. Enter the passphrase to restore them.")))
        return;
    /* With one root its files go straight into the folder chosen; with
     * several, each root gets a folder of its own there. */
    int flat = r->source_id || root_count() <= 1;
    char *dest = pick_folder(g_main, flat ? T(L"Choose where to put the restored files. Folders inside keep their structure; nothing there is overwritten.")
                                          : T(L"Choose where to put the restored files. Each backup root gets a folder named after it; nothing there is overwritten."));
    if (!dest) return;

    /* Which drives this needs, in order. */
    plan_text p;
    memset(&p, 0, sizeof(p));
    int64_t total = 0, none = 0;
    bd_restore_plan(g_cat, r->source_id, r->rel, dest, plan_text_cb, &p, &total, &none);
    if (total == 0) {
        free(p.t.p);
        free(dest);
        MessageBoxW(g_main, T(L"Everything here is already restored in that folder."), APP_NAME, MB_ICONINFORMATION);
        return;
    }
    wchar_t older[600];
    version_note(whole ? NULL : r, older, 600);
    if (p.n > 1 || p.any_offline || none || older[0]) {
        wtext msg = {NULL, 0, 0};
        wadd(&msg, TN(total, L"Restoring %lld file needs these drives:\n\n%ls", L"Restoring %lld files needs these drives:\n\n%ls"),
             (long long)total, p.t.p ? p.t.p : T(L"    (none)\n"));
        if (older[0]) wadd(&msg, L"\n%ls\n", older);
        if (none) wadd(&msg, TN(none, L"\n%lld file has no copy on any drive.\n", L"\n%lld files have no copy on any drive.\n"), (long long)none);
        if (p.any_offline)
            wadd(&msg, T(L"\nBRODALF restores what the plugged-in drives hold now. Then plug in each other drive and press "
                       L"Restore... again to continue with its files.\n"));
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
        swprintf(content, 1200, T(L"A restore into %ls is waiting for these drives:\n\n%ls"), dest ? dest : L"",
                 p.t.p ? p.t.p : T(L"    (none: it can finish now)\n"));
        free(dest);
        free(p.t.p);
        TASKDIALOG_BUTTON b[2] = {
            {630, T(L"Continue the restore\nRestores what the drives plugged in now hold.")},
            {631, T(L"Restore something else\nThe waiting restore is forgotten.")},
        };
        TASKDIALOGCONFIG tc;
        memset(&tc, 0, sizeof(tc));
        tc.cbSize = sizeof(tc);
        tc.hwndParent = g_main;
        tc.hInstance = g_inst;
        tc.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
        tc.dwCommonButtons = TDCBF_CANCEL_BUTTON;
        tc.pszWindowTitle = APP_NAME;
        tc.pszMainInstruction = T(L"Continue the restore that is waiting?");
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
    if (d->backed_up) swprintf(on, 600, L"%ls%ls", drv ? drv : L"", d->connected ? T(L"  (plugged in)") : L"");
    else wcscpy(on, T(L"Never backed up"));
    const wchar_t *cells[] = {d->acknowledged ? L"" : T(L"New"), file, when, size, on};
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
    for (int pos = ListView_GetNextItem(lv, -1, LVNI_SELECTED); pos >= 0 && g_del_npick < sel;
         pos = ListView_GetNextItem(lv, pos, LVNI_SELECTED)) {
        int row = lv_index(lv, pos);
        if (row < 0 || row >= g_del_n) continue;
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
        swprintf(text, 700, T(L"No files have been deleted from your backup roots. When one is, it shows here with the drives that "
                            L"still hold it, until you choose what to do with it."));
    else {
        wchar_t gone[300], fresh_text[100];
        swprintf(gone, 300, TN(total, L"%lld file deleted from your backup roots%ls.",
                               L"%lld files deleted from your backup roots, most recently deleted first%ls."),
                 (long long)total, total > g_del_n ? T(L" (only the first ones are listed)") : L"");
        swprintf(fresh_text, 100, TN(fresh, L"%lld is new.", L"%lld are new."), (long long)fresh);
        swprintf(text, 700, L"%ls %ls %ls", gone, fresh_text,
                 T(L"Select files to restore them, keep tracking them (they stay in the database and on your drives), or forget them "
                   L"(BRODALF stops tracking them; nothing on the drives is deleted)."));
    }
    SetDlgItemTextW(dlg, IDC_DEL_SUMMARY, text);
    EnableWindow(GetDlgItem(dlg, IDC_DEL_FILES), g_del_n > 0);
    EnableWindow(GetDlgItem(dlg, IDC_DEL_FOLDERS), g_del_n > 0);
    EnableWindow(GetDlgItem(dlg, IDC_DEL_ACK), fresh > 0);
    EnableWindow(GetDlgItem(dlg, IDC_DEL_FORGET), g_del_n > 0);
}

static INT_PTR CALLBACK deleted_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    INT_PTR themed;
    if (theme_dialog(dlg, msg, wp, lp, &themed)) return themed;
    switch (msg) {
    case WM_INITDIALOG: {
        static const wchar_t *cols[] = {L"", N_(L"File"), N_(L"Last seen"), N_(L"Size"), N_(L"On drives")};
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
                MessageBoxW(dlg, T(L"Select one or more files in the list first."), APP_NAME, MB_ICONINFORMATION);
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
            for (int pos = ListView_GetNextItem(lv, -1, LVNI_SELECTED); pos >= 0 && s == BD_OK;
                 pos = ListView_GetNextItem(lv, pos, LVNI_SELECTED)) {
                int row = lv_index(lv, pos);
                if (row >= 0 && row < g_del_n) s = bd_deleted_ack(g_cat, g_del[row].node_id);
            }
            if (s == BD_OK) s = ui_save();
            if (s != BD_OK) {
                wchar_t *e = widen(bd_catalog_error(g_cat));
                MessageBoxW(dlg, e ? e : T(L"Could not record that."), APP_NAME, MB_ICONWARNING);
                free(e);
            }
            deleted_fill(dlg);
            return TRUE;
        }
        if (LOWORD(wp) == IDC_DEL_FORGET) {
            HWND lv = GetDlgItem(dlg, IDC_DEL_LIST);
            int sel = ListView_GetSelectedCount(lv);
            if (sel <= 0) {
                MessageBoxW(dlg, T(L"Select the files to forget first."), APP_NAME, MB_ICONINFORMATION);
                return TRUE;
            }
            wchar_t q[700];
            swprintf(q, 700, TN(sel, L"Forget %d deleted file?\n\nBRODALF stops tracking it and its copies, so it can no longer restore "
                                     L"it. The copies stay on your drives: BRODALF never deletes anything there. This cannot be undone.",
                                L"Forget %d deleted files?\n\nBRODALF stops tracking them and their copies, so it can no longer restore "
                                L"them. The copies stay on your drives: BRODALF never deletes anything there. This cannot be undone."),
                     sel);
            if (ask_verbs(dlg, q, MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2, T(L"Forget"), T(L"Cancel")) != IDYES) return TRUE;
            bd_status s = BD_OK;
            int64_t gone = 0;
            for (int pos = ListView_GetNextItem(lv, -1, LVNI_SELECTED); pos >= 0 && s == BD_OK;
                 pos = ListView_GetNextItem(lv, pos, LVNI_SELECTED)) {
                int64_t one = 0;
                int row = lv_index(lv, pos);
                if (row >= 0 && row < g_del_n) s = bd_deleted_forget(g_cat, g_del[row].node_id, &one);
                gone += one;
            }
            if (s == BD_OK) s = ui_save();
            if (s != BD_OK) {
                wchar_t *e = widen(bd_catalog_error(g_cat));
                MessageBoxW(dlg, e ? e : T(L"Could not forget them."), APP_NAME, MB_ICONWARNING);
                free(e);
            } else {
                wchar_t line[200];
                swprintf(line, 200, TN(gone, L"Forgot %lld deleted file. Its copies stay on the drives.",
                                       L"Forgot %lld deleted files. Their copies stay on the drives."), (long long)gone);
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
    /* They have been looked at: the warning about them goes. */
    g_gone_new = 0;
    update_drives_label();
    del_row *picks = g_del_pick;
    int n = g_del_npick;
    g_del_pick = NULL;
    g_del_npick = 0;
    if (r != IDOK || n <= 0) { del_rows_free(picks, n); return; }

    char *dest = NULL;
    del_plan *p = calloc(1, sizeof(del_plan));
    if (!p) goto out;
    if (media_encryption(0).any_connected_encrypted && !unlock_ui(T(L"Some copies are on an encrypted drive. Enter the passphrase to restore them.")))
        goto out;
    int several = 0;
    for (int i = 1; i < n; i++) if (picks[i].source_id != picks[0].source_id) several = 1;
    dest = pick_folder(g_main, several ? T(L"Choose where to put the restored files. Each backup root gets a folder named after it; nothing there is overwritten.")
                                       : T(L"Choose where to put the restored files. Folders inside keep their structure; nothing there is overwritten."));
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
        MessageBoxW(g_main, T(L"Everything selected is already restored in that folder, or has no copy on any drive."), APP_NAME, MB_ICONINFORMATION);
        goto out;
    }
    int offline = p->other;
    for (int i = 0; i < p->n; i++) if (!p->connected[i]) offline = 1;
    if (offline || p->n > 1) {
        wtext msg = {NULL, 0, 0};
        wadd(&msg, TN(total, L"Restoring %lld deleted file needs these drives:\n\n", L"Restoring %lld deleted files needs these drives:\n\n"),
             (long long)total);
        for (int i = 0; i < p->n; i++) {
            wchar_t size[64];
            format_bytes(p->bytes[i], size, 64);
            wadd(&msg, TN(p->files[i], L"    %d. %ls: %lld file (%ls)%ls\n", L"    %d. %ls: %lld files (%ls)%ls\n"), i + 1, p->name[i],
                 (long long)p->files[i], size, p->connected[i] ? T(L", plugged in") : L"");
        }
        if (offline)
            wadd(&msg, T(L"\nBRODALF restores what the plugged-in drives hold now. For the rest, plug in another drive "
                       L"and restore them again from Deleted files.\n"));
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
    log_append(T(L"Restoring deleted files."));
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
    if (media_encryption(0).any_connected_encrypted && !unlock_ui(T(L"Some copies are on an encrypted drive. Enter the passphrase to restore them.")))
        return;
    char *dest = pick_folder(g_main, T(L"Choose the folder to put this version in. It keeps the file's own name; nothing there is overwritten."));
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

/* ---- Reading files in use from a shadow copy ------------------------------- */

/* Windows can take a shadow copy of a drive, a snapshot no program holds
 * open. Only an administrator can, so BRODALF starts a second copy of
 * itself as administrator (brodalf.exe --shadow-copy), which copies the
 * files in use out of the snapshot into a folder only administrators and
 * this user can read; the scan and backup then read those copies in place
 * of the locked files. Nothing here starts by itself: it is offered after
 * a scan or backup that met files in use, and the person says yes. */
static wchar_t g_shadow_dir[MAX_PATH];

/* Delete the files in dir (not in folders inside it) and dir itself. */
static void remove_flat_dir(const wchar_t *dir)
{
    wchar_t pattern[MAX_PATH + 8];
    swprintf(pattern, MAX_PATH + 8, L"%ls\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            wchar_t f[MAX_PATH * 2];
            swprintf(f, MAX_PATH * 2, L"%ls\\%ls", dir, fd.cFileName);
            DeleteFileW(f);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}

/* g_shadow_dir holds list.txt (written here) and out\, which the helper
 * makes itself so that only administrators can write to it. */
static void shadow_out_dir(wchar_t *out, size_t cap)
{
    swprintf(out, cap, L"%ls\\out", g_shadow_dir);
}

/* Forget the copies and delete them. Only with no job running. */
static void shadow_cleanup(void)
{
    if (g_cat) bd_substitutes_clear(g_cat);
    if (!g_shadow_dir[0]) return;
    wchar_t out[MAX_PATH + 8];
    shadow_out_dir(out, MAX_PATH + 8);
    remove_flat_dir(out);
    remove_flat_dir(g_shadow_dir);
    g_shadow_dir[0] = 0;
}

/* Some job still to run reads from the shadow copy. */
static int shadow_still_needed(void)
{
    for (job *q = g_queue; q; q = q->next)
        if (q->from_shadow) return 1;
    return 0;
}

/* This user's SID as text, for the helper to let it read the copies. */
static char *user_sid(void)
{
    HANDLE tok = NULL;
    unsigned char buf[256];
    DWORD n = 0;
    char *out = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok) && GetTokenInformation(tok, TokenUser, buf, sizeof(buf), &n)) {
        wchar_t *w = NULL;
        if (ConvertSidToStringSidW(((TOKEN_USER *)buf)->User.Sid, &w)) {
            out = narrow(w);
            LocalFree(w);
        }
    }
    if (tok) CloseHandle(tok);
    return out;
}

typedef struct { char **paths; int n, cap; } path_list;

static int source_path_cb(void *ctx, const bd_source_info *info)
{
    path_list *l = ctx;
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 8;
        char **grown = realloc(l->paths, sizeof(char *) * (size_t)cap);
        if (!grown) return 1;
        l->paths = grown;
        l->cap = cap;
    }
    l->paths[l->n++] = xstrdup(info->path);
    return 0;
}

/* Run an elevated copy of BRODALF that copies the files in use out of a
 * shadow copy, then read from those copies. Worker thread. The helper gets
 * the list's hash on its command line and checks every file against the
 * backup roots, so nothing changed after the permission prompt is used. */
static bd_status run_shadow(job *j)
{
    in_use_list l = {NULL, 0, 0, 0};
    bd_list_in_use(g_cat, in_use_cb, &l);
    if (!l.n) { swprintf(j->summary, SUMMARY_LEN, T(L"No files are in use any more.")); return BD_OK; }
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    swprintf(g_shadow_dir, MAX_PATH, L"%lsbrodalf-shadow-%lu", tmp, (unsigned long)GetCurrentProcessId());
    CreateDirectoryW(g_shadow_dir, NULL);
    wchar_t list[MAX_PATH + 16], out[MAX_PATH + 8], params[MAX_PATH * 2 + 160], exe[MAX_PATH];
    swprintf(list, MAX_PATH + 16, L"%ls\\list.txt", g_shadow_dir);
    shadow_out_dir(out, MAX_PATH + 8);

    path_list src = {NULL, 0, 0};
    bd_list_sources(g_cat, source_path_cb, &src);
    char *sid = user_sid(), hash[65];
    char *text = sid ? bd_shadow_list_format(sid, (const char *const *)src.paths, src.n, (const char *const *)l.paths, l.n, hash) : NULL;
    for (int i = 0; i < src.n; i++) free(src.paths[i]);
    free(src.paths);
    free(sid);
    FILE *f = text ? _wfopen(list, L"wb") : NULL;
    int written = f && fwrite(text, 1, strlen(text), f) == strlen(text);
    if (f && fclose(f) != 0) written = 0;
    free(text);
    if (!written) {
        in_use_free(&l);
        swprintf(j->summary, SUMMARY_LEN, T(L"Could not write the list of files in use."));
        return BD_ERR_IO;
    }
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t *whash = widen(hash);
    swprintf(params, MAX_PATH * 2 + 160, L"--shadow-copy \"%ls\" \"%ls\" %ls", out, list, whash ? whash : L"");
    free(whash);
    post_text(WM_APP_LOG, T(L"Asking Windows for administrator permission to take a shadow copy..."));
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
            swprintf(j->summary, SUMMARY_LEN, T(L"Without administrator permission the files in use stay as they were last backed up."));
            return BD_OK;
        }
        swprintf(j->summary, SUMMARY_LEN, T(L"Could not start the shadow copy helper (Windows error %lu)."), (unsigned long)err);
        return BD_ERR_IO;
    }
    WaitForSingleObject(sei.hProcess, INFINITE);
    CloseHandle(sei.hProcess);

    /* result.txt: one line per file, "ok", "failed" or "refused", then "error: ...". */
    wchar_t result[MAX_PATH + 24];
    swprintf(result, MAX_PATH + 24, L"%ls\\result.txt", out);
    f = _wfopen(result, L"rb");
    int got = 0, refused = 0;
    char line[2048], err[1024] = "";
    char *dir = narrow(out);
    for (int i = 0; f && fgets(line, sizeof(line), f);) {
        line[strcspn(line, "\r\n")] = 0;
        if (strncmp(line, "error: ", 7) == 0) { snprintf(err, sizeof(err), "%s", line + 7); continue; }
        if (i < l.n && strcmp(line, "refused") == 0) refused++;
        if (i < l.n && strcmp(line, "ok") == 0) {
            char staged[MAX_PATH * 3];
            snprintf(staged, sizeof(staged), "%s\\%d.bin", dir ? dir : "", i + 1);
            if (bd_substitute_add(g_cat, l.paths[i], staged) == BD_OK) got++;
        }
        i++;
    }
    free(dir);
    if (f) fclose(f);
    int total = l.n;
    in_use_free(&l);
    if (refused) {
        wchar_t line_w[200];
        swprintf(line_w, 200, TN(refused, L"%d file outside your backup roots was left alone.",
                                 L"%d files outside your backup roots were left alone."), refused);
        post_text(WM_APP_LOG, line_w);
    }
    if (!got) {
        wchar_t *e = widen(err);
        swprintf(j->summary, SUMMARY_LEN, T(L"The shadow copy did not work: %ls"), e && *e ? e : T(L"no reason was given (see the app log)"));
        free(e);
        return BD_OK;
    }
    j->in_use = got;
    swprintf(j->summary, SUMMARY_LEN, T(L"Read %d of %d files in use from a shadow copy. Scanning them now."), got, total);
    return BD_OK;
}

/* The copies are ready: scan, then back up where the files were going.
 * The last of the two removes the copies when it is done. */
static void after_shadow(job *j)
{
    if (!g_shadow_dir[0] || !j->in_use) return;
    job *scan = new_job(JOB_SCAN);
    job *back = j->media_id || j->n_many ? new_job(JOB_BACKUP) : NULL;
    if (scan) scan->from_shadow = 1;
    if (back) {
        back->media_id = j->media_id;
        memcpy(back->many, j->many, sizeof(j->many));
        back->n_many = j->n_many;
        back->many_places = j->many_places;
        back->from_shadow = 1;
    }
    if (scan) enqueue(scan);
    if (back) enqueue(back);
}

/* The files another program had open at the last scan or backup, and what
 * to do about them. say_none: also say when there are none. after: the
 * scan or backup that met them (NULL from the menu); its drives are where
 * a backup from the shadow copy goes. */
static void show_in_use(int say_none, const job *after)
{
    in_use_list l = {NULL, 0, 0, 0};
    bd_list_in_use(g_cat, in_use_cb, &l);
    if (!l.n) {
        if (say_none) MessageBoxW(g_main, T(L"No files were in use by another program at the last scan or backup."), APP_NAME, MB_ICONINFORMATION);
        return;
    }
    wtext msg = {NULL, 0, 0};
    wchar_t size[64];
    format_bytes(l.bytes, size, 64);
    wadd(&msg, TN(l.n, L"%d file (%ls) is in use by another program, so BRODALF could not read it:\n\n",
                  L"%d files (%ls) are in use by another program, so BRODALF could not read them:\n\n"),
         l.n, size);
    for (int i = 0; i < l.n && i < 10; i++) {
        wchar_t *w = widen(l.paths[i]);
        wadd(&msg, L"    %ls\n", w ? w : L"");
        free(w);
    }
    if (l.n > 10) wadd(&msg, T(L"    ... and %d more\n"), l.n - 10);
    int offer = !(after && after->from_shadow); /* a shadow copy just tried and could not read them */
    if (offer) {
        wadd(&msg, L"%ls",
             TN(l.n, L"\nYou can close the program that has it open (an email program often keeps its mail files open) and scan "
                     L"again. Or Windows can take a shadow copy of the drive, a snapshot no program holds open, and BRODALF can read "
                     L"it from that. Windows will ask for administrator permission.\n\nRead it from a shadow copy now?",
                L"\nYou can close the program that has them open (an email program often keeps its mail files open) and scan "
                L"again. Or Windows can take a shadow copy of the drive, a snapshot no program holds open, and BRODALF can read "
                L"them from that. Windows will ask for administrator permission.\n\nRead them from a shadow copy now?"));
    } else {
        wadd(&msg, L"%ls",
             TN(l.n, L"\nThe shadow copy could not read it either. Close the program that has it open, then press Scan now, "
                     L"and Back up if you were backing up.",
                L"\nThe shadow copy could not read them either. Close the program that has them open, then press Scan now, "
                L"and Back up if you were backing up."));
    }
    int yes = MessageBoxW(g_main, msg.p, APP_NAME, offer ? MB_ICONQUESTION | MB_YESNO : MB_ICONINFORMATION) == IDYES;
    free(msg.p);
    in_use_free(&l);
    if (!offer || !yes) return;
    job *s = new_job(JOB_SHADOW);
    if (!s) return;
    if (after && after->kind == JOB_BACKUP) {
        s->media_id = after->media_id;
        memcpy(s->many, after->many, sizeof(after->many));
        s->n_many = after->n_many;
        s->many_places = after->many_places;
    }
    enqueue(s);
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
    if (lost) swprintf(text, 700, T(L"Marked %ls: its copies do not count"), m->lost_why && strcmp(m->lost_why, "broken") == 0 ? T(L"broken") : T(L"lost"));
    else if (m->connected) swprintf(text, 700, T(L"Plugged in now at %ls, %ls free"), root, freeb);
    else if (m->space_ms > 0) swprintf(text, 700, T(L"Not plugged in. %ls free when last seen (%ls)"), freeb, when);
    else swprintf(text, 700, T(L"Not plugged in"));
    AppendMenuW(sub, MF_STRING | MF_GRAYED, 0, text);
    AppendMenuW(sub, MF_SEPARATOR, 0, NULL);
    AppendMenuW(sub, MF_STRING | (m->connected && !lost ? 0 : MF_GRAYED), base + 0, T(L"Back up to it now"));
    AppendMenuW(sub, MF_STRING | (m->connected ? 0 : MF_GRAYED), base + 2, T(L"Check every copy on it"));
    AppendMenuW(sub, MF_STRING | (m->connected ? 0 : MF_GRAYED), base + 1, T(L"Check copies not checked lately"));
    AppendMenuW(sub, MF_STRING | (m->connected ? 0 : MF_GRAYED), base + 6, T(L"Open in Explorer"));
    AppendMenuW(sub, MF_STRING | (m->connected ? 0 : MF_GRAYED), base + 4, T(L"Compress the copies on it..."));
    AppendMenuW(sub, MF_SEPARATOR, 0, NULL);
    AppendMenuW(sub, MF_STRING, base + 3, T(L"Where it stands, name, where it is kept, hardware details..."));
    AppendMenuW(sub, MF_STRING, base + 5, lost ? T(L"Found it: count its copies again") : T(L"Mark it lost or broken..."));
    swprintf(text, 700, L"%ls%ls%ls%ls%ls%ls", label, *loc ? L"  (" : L"", loc, *loc ? L")" : L"",
             m->secondary ? T(L"  - extra place for starred files") : L"",
             lost ? T(L"  - marked lost or broken") : m->connected ? (m->secondary ? T(L"  - found") : T(L"  - plugged in")) : L"");
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
    if (!f.n_local) AppendMenuW(g_menu_local, MF_STRING | MF_GRAYED, 0, T(L"No backup drives yet"));
    AppendMenuW(g_menu_local, MF_SEPARATOR, 0, NULL);
    AppendMenuW(g_menu_local, MF_STRING, ID_M_ADD_DISK, T(L"Set up an external or removable drive..."));
    AppendMenuW(g_menu_local, MF_STRING, ID_M_ADD_PLACE, T(L"Add an extra place for starred files..."));
    AppendMenuW(g_menu_local, MF_STRING, ID_M_BACKUP_ALL, T(L"Back up to every plugged-in drive"));
    AppendMenuW(g_menu_local, MF_STRING, ID_M_DRIVES, T(L"All drives..."));
    AppendMenuW(g_menu_local, MF_STRING, ID_M_STATUS_ALL, T(L"Status of every drive..."));
    AppendMenuW(g_menu_local, MF_STRING, ID_M_RISK, T(L"Files at risk, and which drive to plug in..."));
}

static void create_menu_bar(HWND hwnd)
{
    HMENU bar = CreateMenu();
    /* Each menu's & letters differ from each other in every language. */
    HMENU file = CreatePopupMenu();
    menu_keyed(file, ID_M_NEW_DB, T(L"&New database..."), 1, 0, L"N");
    menu_keyed(file, ID_M_OPEN_DB, T(L"&Open database..."), 1, 0, L"O");
    AppendMenuW(file, MF_STRING, ID_M_RECOVER_DB, T(L"&Recover the database from a backup drive..."));
    AppendMenuW(file, MF_SEPARATOR, 0, NULL);
    menu_keyed(file, ID_M_SHEET, T(L"&Print a recovery sheet..."), 1, 0, L"P");
    AppendMenuW(file, MF_STRING, ID_M_EXPORT, T(L"&Export the file list (CSV)..."));
    AppendMenuW(file, MF_SEPARATOR, 0, NULL);
    menu_keyed(file, ID_M_EXIT, T(L"E&xit"), 0, 0, L"Alt+F4");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)file, T(L"&File"));

    g_menu_view = CreatePopupMenu();
    static const wchar_t *tab_items[N_TABS] = {N_(L"&Primary Database"), N_(L"&Next backup"), N_(L"L&ocal backups"),
                                               N_(L"&Starred files and extra places"), N_(L"&Encryption"), N_(L"Res&tore")};
    for (int i = 0; i < N_TABS; i++) {
        wchar_t digit[4];
        swprintf(digit, 4, L"%d", i + 1);
        menu_keyed(g_menu_view, (UINT)(ID_M_TAB_FIRST + i), T(tab_items[i]), 1, 0, digit);
    }
    menu_keyed(g_menu_view, ID_M_NEXT_TAB, T(L"Ne&xt tab"), 1, 0, L"Tab");
    menu_keyed(g_menu_view, ID_M_PREV_TAB, T(L"Pre&vious tab"), 1, 1, L"Tab");
    AppendMenuW(g_menu_view, MF_SEPARATOR, 0, NULL);
    menu_keyed(g_menu_view, ID_M_FIND, T(L"&Find a file or folder"), 1, 0, L"F");
    AppendMenuW(g_menu_view, MF_STRING, ID_M_STATUS_ALL, T(L"Status of every &drive..."));
    AppendMenuW(g_menu_view, MF_STRING, ID_M_RISK, T(L"Files at ris&k, and which drive to plug in..."));
    AppendMenuW(g_menu_view, MF_STRING, ID_M_LARGEST, T(L"Lar&gest files..."));
    AppendMenuW(g_menu_view, MF_STRING, ID_M_DELETED, T(L"Deleted f&iles..."));
    AppendMenuW(g_menu_view, MF_STRING, ID_M_DAMAGED, T(L"Da&maged files (found by a bit-rot scan)..."));
    AppendMenuW(g_menu_view, MF_STRING, ID_M_IN_USE, T(L"Files that were in &use at the last scan..."));
    AppendMenuW(g_menu_view, MF_STRING, ID_M_CONFLICTS, T(L"Files t&he last restore left alone..."));
    AppendMenuW(g_menu_view, MF_SEPARATOR, 0, NULL);
    AppendMenuW(g_menu_view, MF_STRING, ID_M_REFRESH, T(L"&Refresh"));
    AppendMenuW(g_menu_view, MF_SEPARATOR, 0, NULL);
    HMENU look = g_menu_look = CreatePopupMenu();
    AppendMenuW(look, MF_STRING, ID_M_THEME_FOLLOW, T(L"Like Windows (its app mode, light or dark)"));
    AppendMenuW(look, MF_STRING, ID_M_THEME_LIGHT, T(L"Light"));
    AppendMenuW(look, MF_STRING, ID_M_THEME_DARK, T(L"Dark"));
    CheckMenuRadioItem(look, ID_M_THEME_FOLLOW, ID_M_THEME_DARK, ID_M_THEME_FOLLOW + (UINT)g_theme_choice, MF_BYCOMMAND);
    AppendMenuW(g_menu_view, MF_POPUP, (UINT_PTR)look, T(L"&Appearance"));
    HMENU lang = g_menu_lang = CreatePopupMenu();
    AppendMenuW(lang, MF_STRING, ID_M_LANG_FOLLOW, T(L"Like Windows (its display language)"));
    for (int i = 0; i < N_LANGS; i++) AppendMenuW(lang, MF_STRING, (UINT_PTR)(ID_M_LANG_FIRST + i), g_langs[i].name);
    CheckMenuRadioItem(lang, ID_M_LANG_FOLLOW, ID_M_LANG_FIRST + N_LANGS - 1,
                       g_lang_choice < 0 ? ID_M_LANG_FOLLOW : (UINT)(ID_M_LANG_FIRST + g_lang_choice), MF_BYCOMMAND);
    AppendMenuW(g_menu_view, MF_POPUP, (UINT_PTR)lang, T(L"&Language"));
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)g_menu_view, T(L"&View"));

    HMENU tools = CreatePopupMenu();
    AppendMenuW(tools, MF_STRING, ID_M_ADD_FOLDER, T(L"&Add a backup root..."));
    menu_keyed(tools, ID_M_SCAN, T(L"S&can the backup roots for changes now"), 0, 0, L"F5");
    AppendMenuW(tools, MF_STRING, ID_M_BITROT, T(L"&Bit-rot scan: read every file again..."));
    AppendMenuW(tools, MF_STRING, ID_M_RELOCATE, T(L"Change &where a backup root is..."));
    AppendMenuW(tools, MF_SEPARATOR, 0, NULL);
    menu_keyed(tools, ID_M_BACKUP, T(L"Back &up..."), 1, 0, L"B");
    AppendMenuW(tools, MF_STRING, ID_M_BACKUP_ALL, T(L"Back up to &every plugged-in drive"));
    AppendMenuW(tools, MF_STRING, ID_M_CHECK, T(L"Chec&k a drive..."));
    AppendMenuW(tools, MF_SEPARATOR, 0, NULL);
    AppendMenuW(tools, MF_STRING, ID_M_RESTORE, T(L"&Restore what is selected..."));
    AppendMenuW(tools, MF_STRING, ID_M_RESTORE_DELETED, T(L"Restore &deleted files..."));
    AppendMenuW(tools, MF_SEPARATOR, 0, NULL);
    AppendMenuW(tools, MF_STRING, ID_M_ADD_DISK, T(L"Set up an e&xternal or removable drive..."));
    AppendMenuW(tools, MF_STRING, ID_M_ADD_PLACE, T(L"Add an extra &place for starred files..."));
    AppendMenuW(tools, MF_STRING, ID_M_STAR, T(L"S&tar or unstar what is selected"));
    AppendMenuW(tools, MF_STRING, ID_M_ENCRYPT, T(L"E&ncrypt what is selected, or stop..."));
    g_menu_local = CreatePopupMenu();
    AppendMenuW(tools, MF_POPUP, (UINT_PTR)g_menu_local, T(L"Backup dr&ives and extra places"));
    AppendMenuW(tools, MF_SEPARATOR, 0, NULL);
    menu_keyed(tools, ID_M_STOP, T(L"Stop the running &job"), 0, 0, L"Esc");
    AppendMenuW(tools, MF_SEPARATOR, 0, NULL);
    AppendMenuW(tools, MF_STRING, ID_M_SETTINGS, T(L"&Settings..."));
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)tools, T(L"&Tools"));

    HMENU help = CreatePopupMenu();
    AppendMenuW(help, MF_STRING, ID_M_LOG, T(L"Show the app &log"));
    AppendMenuW(help, MF_STRING, ID_M_REPORTS, T(L"Show saved error &reports"));
    AppendMenuW(help, MF_STRING, ID_M_LEGEND, T(L"What the file list &shows"));
    AppendMenuW(help, MF_STRING, ID_M_ABOUT, T(L"&About BRODALF"));
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)help, T(L"&Help"));
    SetMenu(hwnd, bar);
}

/* File works with no database open (new, open, recover, exit); View
 * always switches tabs; Tools needs a database; Help always works. While
 * a job runs only View's tabs, Tools' Stop and Help do. */
static void update_menus(void)
{
    HMENU bar = GetMenu(g_main);
    if (!bar) return;
    int n = GetMenuItemCount(bar);
    for (int i = 0; i < n; i++) {
        int on = i == 0 ? !g_busy : i == 2 ? g_cat != NULL : 1;
        EnableMenuItem(bar, (UINT)i, MF_BYPOSITION | (on ? MF_ENABLED : MF_GRAYED));
    }
    /* Tools: while a job runs, only Stop. */
    HMENU tools = GetSubMenu(bar, 2);
    if (tools) {
        int k = GetMenuItemCount(tools);
        for (int i = 0; i < k; i++) {
            int on = (int)GetMenuItemID(tools, i) == ID_M_STOP ? g_busy && !g_stopping && g_running_kind != JOB_REFRESH : !g_busy && g_cat;
            EnableMenuItem(tools, (UINT)i, MF_BYPOSITION | (on ? MF_ENABLED : MF_GRAYED));
        }
    }
    HMENU file = GetSubMenu(bar, 0);
    if (file) {
        static const UINT need_cat[] = {ID_M_SHEET, ID_M_EXPORT};
        for (int i = 0; i < 2; i++) EnableMenuItem(file, need_cat[i], MF_BYCOMMAND | (g_cat ? MF_ENABLED : MF_GRAYED));
    }
    if (g_menu_view) {
        static const UINT views[] = {ID_M_STATUS_ALL, ID_M_RISK, ID_M_LARGEST, ID_M_DELETED, ID_M_DAMAGED, ID_M_IN_USE,
                                     ID_M_CONFLICTS, ID_M_REFRESH};
        for (int i = 0; i < 8; i++)
            EnableMenuItem(g_menu_view, views[i], MF_BYCOMMAND | (g_cat && !g_busy ? MF_ENABLED : MF_GRAYED));
        EnableMenuItem(g_menu_view, ID_M_FIND, MF_BYCOMMAND | (g_cat ? MF_ENABLED : MF_GRAYED));
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

/* A second confirmation for something that changes what counts: the
 * button that does it works only once the box is ticked. 1 if pressed. */
static HRESULT CALLBACK confirm_checked_cb(HWND h, UINT msg, WPARAM wp, LPARAM lp, LONG_PTR ref)
{
    (void)lp;
    (void)ref;
    if (msg == TDN_CREATED) SendMessageW(h, TDM_ENABLE_BUTTON, IDYES, FALSE);
    else if (msg == TDN_VERIFICATION_CLICKED) SendMessageW(h, TDM_ENABLE_BUTTON, IDYES, wp ? TRUE : FALSE);
    return S_OK;
}

static int confirm_checked(const wchar_t *question, const wchar_t *content, const wchar_t *yes, const wchar_t *check)
{
    TASKDIALOG_BUTTON b[1] = {{IDYES, yes}};
    TASKDIALOGCONFIG tc;
    memset(&tc, 0, sizeof(tc));
    tc.cbSize = sizeof(tc);
    tc.hwndParent = g_main;
    tc.hInstance = g_inst;
    tc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
    tc.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    tc.pszWindowTitle = APP_NAME;
    tc.pszMainIcon = TD_WARNING_ICON;
    tc.pszMainInstruction = question;
    tc.pszContent = content;
    tc.pszVerificationText = check;
    tc.cButtons = 1;
    tc.pButtons = b;
    tc.nDefaultButton = IDCANCEL;
    tc.pfCallback = confirm_checked_cb;
    int pressed = IDCANCEL;
    if (FAILED(TaskDialogIndirect(&tc, &pressed, NULL, NULL))) return 0;
    return pressed == IDYES;
}

/* Mark a drive lost or broken, after showing what it does to the full
 * copies: its copies stop counting, so the files it held need backing up
 * again to other drives. */
static void mark_lost_ui(const drive_snap *d)
{
    wchar_t name[600], head[700];
    drive_name(d->label, d->location, name, 600);
    swprintf(head, 700, T(L"What happened to %ls?"), name);
    if (d->secondary) {
        TASKDIALOG_BUTTON pb[2] = {
            {650, T(L"It is gone\nThe folder or the account is no longer there.")},
            {651, T(L"It is broken\nIts files cannot be read.")},
        };
        TASKDIALOGCONFIG pc;
        memset(&pc, 0, sizeof(pc));
        pc.cbSize = sizeof(pc);
        pc.hwndParent = g_main;
        pc.hInstance = g_inst;
        pc.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
        pc.dwCommonButtons = TDCBF_CANCEL_BUTTON;
        pc.pszWindowTitle = APP_NAME;
        pc.pszMainInstruction = head;
        pc.pszContent = T(L"BRODALF stops copying starred files there. Your full copies do not change: extra places never count toward "
                          L"them. Nothing there is deleted, and Found it undoes this.");
        pc.cButtons = 2;
        pc.pButtons = pb;
        int pp = IDCANCEL;
        if (FAILED(TaskDialogIndirect(&pc, &pp, NULL, NULL)) || (pp != 650 && pp != 651)) return;
        if (bd_media_mark_lost(g_cat, d->id, pp == 651 ? "broken" : "lost") != BD_OK) {
            wchar_t *e = widen(bd_catalog_error(g_cat));
            MessageBoxW(g_main, e ? e : T(L"The drive could not be marked."), APP_NAME, MB_ICONWARNING);
            free(e);
            return;
        }
        if (ui_save() == BD_OK) {
            wchar_t line[800];
            swprintf(line, 800, pp == 651 ? T(L"%ls is marked broken. BRODALF no longer copies starred files there.")
                                          : T(L"%ls is marked gone. BRODALF no longer copies starred files there."), name);
            log_append(line);
        }
        return;
    }
    wchar_t first[1200];
    swprintf(first, 1200, T(L"BRODALF stops counting its copies, so your full copies show what you really have, and backs up its files "
                          L"again to your other drives. Nothing on any drive is deleted, and you can undo this with Found it."));
    TASKDIALOG_BUTTON b[2] = {
        {650, T(L"It is lost\nYou do not know where it is.")},
        {651, T(L"It is broken\nIt no longer works, or its files cannot be read.")},
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
    tc.pszContent = first;
    tc.cButtons = 2;
    tc.pButtons = b;
    int pressed = IDCANCEL;
    if (FAILED(TaskDialogIndirect(&tc, &pressed, NULL, NULL)) || (pressed != 650 && pressed != 651)) return;
    const char *why = pressed == 651 ? "broken" : "lost";
    const wchar_t *wwhy = pressed == 651 ? T(L"broken") : T(L"lost");
    sets_snap before = read_sets();
    bd_status st = bd_media_mark_lost(g_cat, d->id, why);
    if (st != BD_OK) {
        wchar_t *e = widen(bd_catalog_error(g_cat));
        MessageBoxW(g_main, e ? e : T(L"The drive could not be marked."), APP_NAME, MB_ICONWARNING);
        free(e);
        return;
    }
    sets_snap after = read_sets();
    wtext t = {NULL, 0, 0};
    int changed = 0;
    int64_t again = 0;
    for (int i = 0; i < before.n || i < after.n; i++) {
        int64_t fa = i < before.n ? before.s[i].files_done : 0, fz = i < after.n ? after.s[i].files_done : 0;
        int64_t ft = i < after.n ? after.s[i].files_total : i < before.n ? before.s[i].files_total : 0;
        if (fa == fz) continue;
        changed = 1;
        if (fa - fz > again) again = fa - fz;
        wadd(&t, T(L"Full copy %d goes from %lld of %lld files to %lld of %lld.\n"), i + 1, (long long)fa, (long long)ft, (long long)fz,
             (long long)ft);
    }
    if (changed)
        wadd(&t, TN(again, L"%lld file will need to be backed up again, to another drive.\n",
                    L"%lld files will need to be backed up again, to another drive.\n"), (long long)again);
    else
        wadd(&t, L"%ls\n", T(L"No full copy changes: every file it held has enough good copies elsewhere."));
    wadd(&t, T(L"\nNothing on %ls is deleted. If you find it, choose Found it and its copies count again."), name);
    wchar_t q[800], yes[700];
    swprintf(q, 800, T(L"Stop counting the copies on %ls?"), name);
    swprintf(yes, 700, pressed == 651 ? T(L"Mark %ls as broken") : T(L"Mark %ls as lost"), name);
    int ok = confirm_checked(q, t.p ? t.p : L"", yes, T(L"I understand what this does"));
    free(t.p);
    if (!ok) {
        bd_media_found(g_cat, d->id);
        return;
    }
    if (ui_save() == BD_OK) {
        wchar_t line[800];
        swprintf(line, 800, T(L"%ls is marked %ls. Its copies no longer count; back up to your other drives to make up for it."), name, wwhy);
        log_append(line);
    }
}

static void mark_found_ui(const drive_snap *d)
{
    wchar_t name[600], line[800];
    drive_name(d->label, d->location, name, 600);
    if (bd_media_found(g_cat, d->id) == BD_OK && ui_save() == BD_OK) {
        if (d->secondary) swprintf(line, 800, T(L"%ls is back: BRODALF copies starred files there again."), name);
        else
            swprintf(line, 800, T(L"%ls is back: its copies count again. Check every copy on it next time it is plugged in, to be sure they are "
                                L"still good."), name);
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
        if (name && *name && strcmp(name, d->label) != 0) {
            if (d->secondary && !good_description(g_edit_name))
                MessageBoxW(g_main, T(L"An extra place keeps its old description: describe it in at least two words, saying what it is and "
                                    L"where it lives, like \"Dropbox folder on the laptop\"."), APP_NAME, MB_ICONINFORMATION);
            else
                st = bd_media_rename(g_cat, id, name);
        }
        if (st == BD_OK && loc && strcmp(loc, d->location) != 0) st = bd_media_set_location(g_cat, id, loc);
        if (st == BD_OK) st = ui_save();
        free(name);
        free(loc);
    }
    if (st == BD_OK && r == IDC_DRIVE_LOST) {
        if (d->lost_ms) mark_found_ui(d);
        else mark_lost_ui(d);
    }
    if (st != BD_OK) {
        wchar_t *e = widen(bd_catalog_error(g_cat));
        MessageBoxW(g_main, e ? e : T(L"The drive could not be changed."), APP_NAME, MB_ICONWARNING);
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

/* Windows' own file compression for a backup drive, when the person asks:
 * say what it does and costs, and turn it on or off. */
static void compress_drive_ui(int64_t id)
{
    int can = 0, on = 0;
    const char *why = NULL;
    if (g_busy) return;
    if (bd_media_compression(g_cat, id, &can, &on, &why) != BD_OK) {
        MessageBoxW(g_main, T(L"Plug the drive in first."), APP_NAME, MB_ICONINFORMATION);
        return;
    }
    wchar_t name[600];
    media_name(id, name, 600);
    wtext msg = {NULL, 0, 0};
    int yes;
    if (on) {
        wadd(&msg, T(L"Compression is on for %ls: Windows keeps BRODALF's copies there smaller, and makes them whole again as "
                   L"they are read.\n\nTurn it off? Every copy on the drive is made whole again, which needs room for them to "
                   L"grow back to their full size, and takes about as long as reading and writing every copy there."), name);
        yes = ask_verbs(g_main, msg.p, MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2, T(L"Turn it off"), T(L"Leave it on")) == IDYES;
    } else if (!can) {
        wchar_t *w = widen(why);
        wadd(&msg, T(L"%ls cannot be compressed. %ls"), name, w ? w : L"");
        free(w);
        MessageBoxW(g_main, msg.p, APP_NAME, MB_ICONINFORMATION);
        yes = 0;
    } else {
        wadd(&msg, T(L"Windows can compress the copies on %ls with its own file compression. They stay ordinary files that any "
                   L"Windows PC reads without BRODALF; they just take less room on the drive.\n\n"
                   L"Documents and similar files often shrink to half their size or less. Photos, videos, music and zipped "
                   L"files are compressed already and barely shrink.\n\n"
                   L"Compressing takes about as long as reading and writing every copy on the drive, and later backups to it "
                   L"are a little slower. You can turn it off again here.\n\nCompress the copies on this drive now?"), name);
        yes = ask_verbs(g_main, msg.p, MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2, T(L"Compress"), T(L"Cancel")) == IDYES;
    }
    free(msg.p);
    if (!yes) return;
    job *j = new_job(JOB_COMPRESS);
    if (!j) return;
    j->media_id = id;
    j->compress_on = !on;
    enqueue(j);
}

static void drive_action(int64_t id, int action)
{
    drive_list dl = load_drives();
    const drive_snap *d = find_drive(&dl, id);
    if (!d) { free(dl.items); return; }
    if (action == 0) {
        if (!d->lost_ms && (!d->encrypted || unlock_ui(T(L"This drive is encrypted. Enter the passphrase to back up to it.")))) {
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
    } else if (action == 4) {
        compress_drive_ui(id);
    } else if (action == 6 && d->connected) {
        wchar_t *root = widen(d->root);
        if (root) open_in_explorer(root);
        free(root);
    }
    free(dl.items);
}

enum { CHOICE_LAST = 100, CHOICE_OPEN, CHOICE_NEW, CHOICE_RECOVER, CHOICE_FIND };
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
    case ID_M_LEGEND:
        MessageBoxW(g_main, tree_legend(), APP_NAME, MB_ICONINFORMATION);
        return;
    case ID_M_ABOUT: {
        wchar_t *v = widen(bd_version()), msg[700];
        swprintf(msg, 700, T(L"BRODALF %ls\n\nKeeps track of what has been backed up from your backup roots, and to which drive, and "
                           L"whether each copy is still good. It does nothing until you press a button.\n\nhttps://github.com/Phawx/BRODALF"),
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
    case ID_M_IN_USE: show_in_use(1, NULL); break;
    case ID_M_STATUS_ALL: cmd_table(TABLE_DRIVES); break;
    case ID_M_EXPORT: cmd_export(); break;
    case ID_M_SETTINGS: cmd_security(); break;
    case ID_M_BACKUP: cmd_backup(); break;
    case ID_M_BACKUP_ALL: cmd_backup_all(1); break;
    case ID_M_CHECK: cmd_check(); break;
    case ID_M_ADD_PLACE: cmd_add_place(); break;
    case ID_M_STAR: toggle_star_selected(); break;
    case ID_M_ENCRYPT: toggle_encrypt_selected(); break;
    case ID_M_REFRESH: break; /* every tab is filled again after each command */
    }
}

/* A scan that also reads every unchanged file again, to find files damaged
 * on the PC's own disk (bit rot): their copies on the drives are then the
 * good ones. */
static void cmd_bitrot(void)
{
    if (ask_verbs(g_main, T(L"Look for damaged files on this PC?\n\nA bit-rot scan reads every file in your backup roots again and "
                            L"compares it with its checksum, to find files that changed although their size and time did not. It takes "
                            L"as long as the first scan did. Nothing is changed or backed up."),
                  MB_ICONQUESTION | MB_YESNO, T(L"Start the scan"), T(L"Cancel")) != IDYES)
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
    AppendMenuW(m, MF_STRING | (missing || r->state == BD_STATE_DELETED ? MF_GRAYED : 0), ID_CTX_OPEN, T(L"Open in Explorer"));
    AppendMenuW(m, MF_STRING, ID_CTX_RESTORE, T(L"Restore..."));
    {
        const char *mr = r->rel ? r->rel : "";
        int st = bd_starred(g_cat, r->source_id, mr), en = bd_encrypt_marked(g_cat, r->source_id, mr);
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        AppendMenuW(m, MF_STRING | (st == 2 ? MF_GRAYED : 0), ID_CTX_STAR,
                    st == 1 ? T(L"Unstar it") : st == 2 ? T(L"Starred with its folder") : T(L"Star it: also back it up to the extra places"));
        AppendMenuW(m, MF_STRING | (en == 2 ? MF_GRAYED : 0), ID_CTX_ENCRYPT,
                    en == 1 ? T(L"Stop encrypting it...") : en == 2 ? T(L"Encrypted with its folder") : T(L"Encrypt it on every drive..."));
    }
    if (is_root) {
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        AppendMenuW(m, MF_STRING, active ? ID_CTX_STOP_ROOT : ID_CTX_RESUME_ROOT,
                    active ? T(L"Stop backing up this root...") : T(L"Back up this root again"));
        AppendMenuW(m, MF_STRING, ID_CTX_ROOT_SKIP, T(L"What to leave out in this root..."));
        AppendMenuW(m, MF_STRING, ID_CTX_RELOCATE, T(L"Change where it is..."));
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
    } else if (cmd == ID_CTX_STAR) {
        toggle_star_selected();
    } else if (cmd == ID_CTX_ENCRYPT) {
        toggle_encrypt_selected();
    } else if (cmd == ID_CTX_RESTORE) {
        cmd_restore();
    } else if (cmd == ID_CTX_STOP_ROOT) {
        wchar_t q[900];
        swprintf(q, 900, T(L"Stop backing up %ls?\n\nScans leave it alone and nothing new from it is backed up. Everything BRODALF "
                         L"knows about it stays, and its copies on your drives stay and still restore. You can back it up again "
                         L"at any time."), root_name);
        if (ask_verbs(g_main, q, MB_ICONQUESTION | MB_YESNO, T(L"Stop backing it up"), T(L"Cancel")) == IDYES && bd_source_set_active(g_cat, r->source_id, 0) == BD_OK &&
            ui_save() == BD_OK) {
            wchar_t line[400];
            swprintf(line, 400, T(L"%ls is no longer backed up."), root_name);
            log_append(line);
            rebuild_tree();
            update_drives_label();
        }
    } else if (cmd == ID_CTX_RESUME_ROOT) {
        if (bd_source_set_active(g_cat, r->source_id, 1) == BD_OK && ui_save() == BD_OK) {
            wchar_t line[400];
            swprintf(line, 400, T(L"%ls is backed up again. Scanning it."), root_name);
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
    AppendMenuW(m, MF_STRING, ID_CTX_RESTORE_VERSION, T(L"Restore this version..."));
    AppendMenuW(m, MF_STRING | (c->full ? 0 : MF_GRAYED), ID_CTX_OPEN_VERSION, T(L"Open in Explorer (on its drive)"));
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
        swprintf(q, 600, TN(j->damaged, L"%lld file changed although its size and time did not. That usually means damage on this PC's disk "
                                        L"(bit rot), and the copies on your drives are the good ones.\n\nShow it now? You can restore it or "
                                        L"accept it as it is.",
                            L"%lld files changed although their size and time did not. That usually means damage on this PC's disk "
                            L"(bit rot), and the copies on your drives are the good ones.\n\nShow them now? You can restore them or "
                            L"accept them as they are."),
                 (long long)j->damaged);
        if (ask_verbs(g_main, q, MB_ICONWARNING | MB_YESNO, TN(j->damaged, L"Show it", L"Show them"), T(L"Not now")) == IDYES) cmd_table(TABLE_DAMAGED);
    }
    /* Files gone from the backup roots: a warning above the tabs says so,
     * with a button to look at them (it does not stop everything else). */
    int64_t gone = 0;
    if ((j->found_deleted || j->after_open) && bd_list_deleted(g_cat, 0, 1, NULL, NULL, &gone) == BD_OK) {
        g_gone_new = gone;
        update_drives_label();
    }
    if (j->then_backup) {
        /* The user chose to scan first: show what the backup does now. */
        job *b = new_job(JOB_BACKUP);
        if (b) {
            b->media_id = j->then_backup;
            start_backup(b, 0);
        }
    }
    if (j->then_all) cmd_backup_all(0);
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
    case BD_STATE_AVAILABLE: return T(L"Available");
    case BD_STATE_AVAILABLE_OLDER: return T(L"Changed since backup");
    case BD_STATE_OFFLINE: return T(L"Drive not plugged in");
    case BD_STATE_NO_COPY: return T(L"Not backed up");
    case BD_STATE_BAD: return T(L"Copy damaged");
    case BD_STATE_DELETED: return T(L"Deleted");
    case BD_STATE_PARTIAL: return T(L"Partly available");
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
    const wchar_t *cells[] = {shown, folder, short_state(r->state), where && *where ? where : T(L"(no copies)")};
    lv_row(g_results, cells, 4);
    free(name);
    free(src);
    free(rel);
    free(where);
    return 0;
}

/* The results' columns as wide as what they hold, within the list; where
 * each file has copies takes the rest. */
static void search_fit_cols(void)
{
    RECT rc;
    GetClientRect(g_results, &rc);
    int w = rc.right;
    static const int share[] = {38, 34, 20};
    for (int c = 0; c < 3; c++) {
        ListView_SetColumnWidth(g_results, c, LVSCW_AUTOSIZE_USEHEADER);
        int cap = w * share[c] / 100;
        if (ListView_GetColumnWidth(g_results, c) > cap) ListView_SetColumnWidth(g_results, c, cap);
    }
    ListView_SetColumnWidth(g_results, 3, LVSCW_AUTOSIZE_USEHEADER);
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
        show_detail(); /* the tree's own selection again */
        return;
    }
    if (g_busy) {
        /* The catalog belongs to the job; the search runs again when it ends. */
        const wchar_t *cells[] = {T(L"Search is available when the current job finishes."), L"", L"", L""};
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
        const wchar_t *cells[] = {T(L"No files or folders match."), L"", L"", L""};
        lv_row(g_results, cells, 4);
    }
    /* Until a result is picked, the details say what was found, not what
     * was selected before the search. */
    {
        wchar_t msg[700];
        if (!g_hit_n) swprintf(msg, 700, T(L"Nothing matches \"%ls\". Esc goes back to the folders."), p);
        else if (g_hit_n >= 500)
            swprintf(msg, 700, T(L"The first %d files and folders that match \"%ls\". Type more of the name to find fewer."), g_hit_n, p);
        else
            swprintf(msg, 700, TN(g_hit_n, L"%d file or folder matches \"%ls\". Select it to see where its copies are.",
                                  L"%d files and folders match \"%ls\". Select one to see where its copies are."),
                     g_hit_n, p);
        SetWindowTextW(g_detail, msg);
        ListView_DeleteAllItems(g_list);
    }
    search_fit_cols();
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

/* ---- Tabs ------------------------------------------------------------------ */

/* The window shows one tab at a time: the Primary Database (the backup
 * roots as a tree), what the next backup copies, the local backup drives,
 * starred files and extra places, encryption, and restoring. Each tab is a
 * page: a child window over the tab control that holds that tab's
 * controls and passes their messages on to the main window. A page is
 * filled from the database when it is shown and after each job and
 * command, never while a job runs (the catalog is the job's then). */

static int g_page_stale[N_TABS] = {1, 1, 1, 1, 1, 1};
static int g_filling; /* >0 while a page refills its lists: their notifications are not the user's */

static const int g_db_btns[] = {ID_DB_ADD, ID_DB_STAR, ID_DB_ENCRYPT, ID_DB_BITROT, ID_DB_RELOCATE, ID_DB_SKIP,
                                ID_DB_DELETED, ID_DB_DAMAGED, ID_DB_IN_USE, ID_DB_LARGEST, ID_DB_EXPORT};
static const int g_next_btns[] = {ID_NEXT_BACKUP, ID_NEXT_ALL, ID_NEXT_CHANGED, ID_NEXT_RISK, ID_NEXT_REFRESH};
static const int g_local_btns[] = {ID_LOCAL_BACKUP, ID_LOCAL_ADD, ID_LOCAL_ALL, ID_LOCAL_CHECK, ID_LOCAL_VERIFY, ID_LOCAL_OPEN,
                                   ID_LOCAL_COMPRESS, ID_LOCAL_CHANGED, ID_LOCAL_EDIT, ID_LOCAL_LOST, ID_LOCAL_STATUS, ID_LOCAL_RISK, ID_LOCAL_PARALLEL};
static const int g_star_btns[] = {ID_STAR_ADD, ID_STAR_REMOVE, ID_STAR_SHOW};
static const int g_place_btns[] = {ID_PLACE_ADD, ID_PLACE_BACKUP, ID_PLACE_LIMIT, ID_PLACE_OPEN, ID_PLACE_EDIT, ID_PLACE_LOST};
static const int g_enc_btns[] = {ID_ENC_PASS, ID_ENC_LOCK, ID_ENC_KEY, ID_ENC_SHEET, ID_ENC_DRIVE, ID_ENC_CATALOG};
static const int g_enc_btns2[] = {ID_ENC_ADD, ID_ENC_PICK, ID_ENC_REMOVE, ID_ENC_SHOW};
static const int g_rest_btns[] = {ID_REST_SEL, ID_REST_ALL, ID_REST_DELETED, ID_REST_CONTINUE};
static const int g_rest_btns2[] = {ID_REST_OPEN, ID_REST_TABLE};

/* What each row of a page's list is. */
typedef struct { int64_t id; int connected, lost, secondary; } media_row;
static media_row g_local_rows[256], g_place_rows[256], g_next_opts[65];
static int g_local_n, g_place_n, g_next_nopts;
static mark_key *g_next_rows, *g_star_rows, *g_enc_rows;
static int g_next_nrows, g_star_nrows, g_enc_nrows;
static int64_t g_next_media; /* the Next backup tab: 0 every file short of its copies, else a drive or extra place */

static void rows_free(mark_key **rows, int *n)
{
    for (int i = 0; i < *n; i++) free((*rows)[i].rel);
    free(*rows);
    *rows = NULL;
    *n = 0;
}

static void rows_add(mark_key **rows, int *n, int64_t source_id, const char *rel)
{
    mark_key *grown = realloc(*rows, sizeof(mark_key) * (size_t)(*n + 1));
    if (!grown) return;
    *rows = grown;
    grown[*n].source_id = source_id;
    grown[*n].rel = xstrdup(rel ? rel : "");
    (*n)++;
}

static LRESULT CALLBACK page_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
    case WM_NOTIFY:
    case WM_CONTEXTMENU:
        return SendMessageW(g_main, msg, wp, lp);
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT *di = (const DRAWITEMSTRUCT *)lp;
        if (di->CtlType == ODT_BUTTON) {
            draw_page_button(di);
            return TRUE;
        }
        break;
    }
    case WM_ERASEBKGND:
        if (g_dark) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect((HDC)wp, &rc, g_br_bg);
            return 1;
        }
        break;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN: {
        LRESULT r;
        if (theme_ctlcolor(msg, wp, lp, &r)) return r;
        break;
    }
    case WM_CTLCOLORSTATIC: {
        LRESULT r;
        if (theme_ctlcolor(msg, wp, lp, &r)) return r;
        /* Labels, check boxes and read-only text on the page's own colour. */
        HDC dc = (HDC)wp;
        SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        SetBkColor(dc, GetSysColor(COLOR_WINDOW));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void register_page_class(void)
{
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = page_proc;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"BrodalfPage";
    RegisterClassExW(&wc);
}

static HWND page_ctl(int page, const wchar_t *cls, const wchar_t *text, DWORD style, DWORD ex, int id)
{
    /* WS_CLIPSIBLINGS: a button moving across another while a row is laid
     * out again must not leave pieces of either drawn over the other. */
    HWND h = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | style, 0, 0, 10, 10, g_page[page],
                             (HMENU)(INT_PTR)id, g_inst, NULL);
    SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);
    if (id >= ID_PAGE_FIRST && id < ID_PAGE_LAST) CTL(id) = h;
    return h;
}

static void page_button(int page, const wchar_t *text, int id)
{
    page_ctl(page, WC_BUTTONW, text, WS_TABSTOP | BS_PUSHBUTTON, 0, id);
    g_ctl_btn[id - ID_PAGE_FIRST] = 1;
}

/* The tab's main action: filled with the accent colour. */
static void page_primary(int page, const wchar_t *text, int id)
{
    page_ctl(page, WC_BUTTONW, text, WS_TABSTOP | BS_OWNERDRAW, 0, id);
    g_ctl_btn[id - ID_PAGE_FIRST] = 1;
}

/* A button that stays down while what it says is on (Star it, Encrypt it).
 * Its words never change, so the buttons after it stay where they are. */
#define BTN_TOGGLE 2
static void page_toggle(int page, const wchar_t *text, int id)
{
    HWND h = page_ctl(page, WC_BUTTONW, text, WS_TABSTOP | BS_OWNERDRAW, 0, id);
    SetWindowLongPtrW(h, GWLP_USERDATA, BTN_TOGGLE);
    g_ctl_btn[id - ID_PAGE_FIRST] = 1;
}

static void set_toggle(int id, int on)
{
    HWND h = CTL(id);
    LONG_PTR v = BTN_TOGGLE | (on ? 1 : 0);
    if (GetWindowLongPtrW(h, GWLP_USERDATA) != v) {
        SetWindowLongPtrW(h, GWLP_USERDATA, v);
        InvalidateRect(h, NULL, FALSE);
    }
}

static void page_check(int page, const wchar_t *text, int id)
{
    page_ctl(page, WC_BUTTONW, text, WS_TABSTOP | BS_CHECKBOX, 0, id);
    g_ctl_btn[id - ID_PAGE_FIRST] = 1;
}

static void page_list(int page, int id, const wchar_t **cols, const int *widths, int n)
{
    HWND lv = page_ctl(page, WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, WS_EX_CLIENTEDGE, id);
    lv_sortable(lv);
    if (n) lv_columns(lv, cols, widths, n);
    ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
}

static void page_text(int page, int id)
{
    read_only_keys(page_ctl(page, WC_EDITW, L"", WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, WS_EX_CLIENTEDGE, id));
}

static void page_label(int page, int id, const wchar_t *text)
{
    page_ctl(page, WC_STATICW, text, SS_LEFT | SS_NOPREFIX, 0, id);
}

static void lv_clear_columns(HWND lv)
{
    while (ListView_DeleteColumn(lv, 0)) {}
    RemovePropW(lv, LV_SORT_PROP); /* other columns: not sorted */
}

/* The selected row of a page's table, as where its data is (lv_index). */
static int lv_selected(int id)
{
    return lv_index(CTL(id), (int)SendMessageW(CTL(id), LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED));
}

/* Select the row whose data is at row (lv_index). */
static void lv_select(HWND lv, int row)
{
    int pos = lv_pos(lv, row);
    if (pos < 0) return;
    ListView_SetItemState(lv, pos, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(lv, pos, FALSE);
}

/* Text for a read-only edit control, which wants \r\n between lines. */
static void set_edit_text(HWND ed, const wchar_t *text)
{
    size_t n = 0;
    for (const wchar_t *c = text; *c; c++) n += *c == L'\n' ? 2 : 1;
    wchar_t *out = malloc(sizeof(wchar_t) * (n + 1)), *o = out;
    if (!out) { SetWindowTextW(ed, text); return; }
    for (const wchar_t *c = text; *c; c++) {
        if (*c == L'\n' && (c == text || c[-1] != L'\r')) *o++ = L'\r';
        *o++ = *c;
    }
    *o = 0;
    SetWindowTextW(ed, out);
    free(out);
}

/* "under a second", "40 seconds", "12 minutes", "3.5 hours" */
static void format_duration(double s, wchar_t *out, size_t n)
{
    if (s < 1) swprintf(out, n, T(L"under a second"));
    else if (s < 90) {
        int v = (int)(s + 0.5);
        swprintf(out, n, TN(v, L"%d second", L"%d seconds"), v);
    } else if (s < 90 * 60) {
        int v = (int)(s / 60 + 0.5);
        swprintf(out, n, TN(v, L"%d minute", L"%d minutes"), v);
    } else if (s < 48 * 3600) swprintf(out, n, T(L"%.1f hours"), s / 3600);
    else swprintf(out, n, T(L"%.1f days"), s / 86400);
}

/* What encrypting costs in time, in the window's language: the engine's
 * bd_encrypt_cost_text says the same in English. */
static void encrypt_cost_text(int64_t bytes, wchar_t *out, size_t cap)
{
    int64_t bps = bd_crypto_speed();
    wchar_t speed[64], size[64], work[64], extra[64];
    if (!cap) return;
    if (bps <= 0) {
        swprintf(out, cap, L"%ls", T(L"Encrypting takes some extra time while each copy is written, and checking or restoring an encrypted "
                                     L"copy takes the same again. Encrypted copies need the passphrase to restore."));
        return;
    }
    /* The copy is sealed on its way to the drive, so the two overlap: a
     * backup takes about as long as the slower of writing and encrypting,
     * on a typical USB hard drive at about 120 MB a second. */
    const double drive = 120e6;
    double crypt_s = (double)bytes / (double)bps, write_s = (double)bytes / drive;
    format_bytes(bps, speed, 64);
    format_bytes(bytes, size, 64);
    /* "about under a second" reads badly: round a sub-second cost up. */
    format_duration(crypt_s < 1 ? 1 : crypt_s, work, 64);
    if (bytes <= 0)
        swprintf(out, cap, T(L"This PC encrypts about %ls a second. Encrypting runs while each copy is written, so on a USB hard drive a "
                           L"backup takes about as long as before. Checking and restoring an encrypted copy decrypt it at about the same "
                           L"speed. Encrypted copies take about 0.1%% more space and need the passphrase to restore."),
                 speed);
    else if (crypt_s <= write_s)
        swprintf(out, cap, T(L"This PC encrypts about %ls a second, so the %ls you picked take about %ls of extra work, done while each "
                           L"copy is written. On a USB hard drive a backup takes about as long as before; on a fast SSD it can take up "
                           L"to %ls longer. Checking and restoring them decrypt at about the same speed. Encrypted copies take about "
                           L"0.1%% more space and need the passphrase to restore."),
                 speed, size, work, work);
    else {
        format_duration(crypt_s - write_s < 1 ? 1 : crypt_s - write_s, extra, 64);
        swprintf(out, cap, T(L"This PC encrypts about %ls a second, slower than a USB hard drive writes, so the %ls you picked add about %ls "
                           L"to a full backup of them (%ls of work in all). Checking and restoring them decrypt at about the same "
                           L"speed. Encrypted copies take about 0.1%% more space and need the passphrase to restore."),
                 speed, size, extra, work);
    }
}

static int mv(HWND h, int x, int y, int w, int hh)
{
    MoveWindow(h, x, y, w > 0 ? w : 0, hh > 0 ? hh : 0, TRUE);
    return hh;
}

static HFONT font_of(HWND h)
{
    HFONT f = (HFONT)SendMessageW(h, WM_GETFONT, 0, 0);
    return f ? f : g_font;
}

/* A button as wide as its text in the language shown (the & of its
 * letter takes no room), with room either side; never narrower than a
 * usual button. */
static int button_width(HWND b)
{
    wchar_t text[200];
    int n = GetWindowTextW(b, text, 200);
    HDC dc = GetDC(b);
    HFONT old = (HFONT)SelectObject(dc, font_of(b));
    RECT r = {0, 0, 0, 0};
    DrawTextW(dc, text, n, &r, DT_CALCRECT | DT_SINGLELINE);
    SelectObject(dc, old);
    ReleaseDC(b, dc);
    int w = r.right + S(24);
    /* Drawn by BRODALF: the primary button's words are semibold, a toggle
     * has its star or lock before them. */
    if ((GetWindowLongW(b, GWL_STYLE) & BS_TYPEMASK) == BS_OWNERDRAW)
        w += (GetWindowLongPtrW(b, GWLP_USERDATA) & BTN_TOGGLE) ? S(24) : S(14);
    return w < S(64) ? S(64) : w;
}

/* How tall a label is with its text wrapped at width w. */
static int text_height(HWND h, int w)
{
    wchar_t text[4096];
    int n = GetWindowTextW(h, text, 4096);
    /* Measured a little narrower than the control: a static control can
     * break a line a few pixels sooner than DrawText measures, and a line
     * that wraps unmeasured is cut off or drawn over what is below. */
    RECT r = {0, 0, w > S(8) ? w - S(6) : w, 0};
    HDC dc = GetDC(h);
    HFONT old = (HFONT)SelectObject(dc, font_of(h));
    DrawTextW(dc, text, n, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
    ReleaseDC(h, dc);
    return r.bottom + S(2);
}

/* The height of one line of the window's text. */
static int line_height(void)
{
    HDC dc = GetDC(g_main);
    HFONT old = (HFONT)SelectObject(dc, g_font);
    TEXTMETRICW tm;
    memset(&tm, 0, sizeof(tm));
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    ReleaseDC(g_main, dc);
    return tm.tmHeight > 0 ? tm.tmHeight + tm.tmExternalLeading : S(16);
}

/* Lay buttons out left to right from (x, y), within w, onto more rows as
 * needed. Returns the height they take. */
static int flow(const int *ids, int n, int x, int y, int w)
{
    int cx = x, cy = y, bh = S(26), gap = S(6);
    /* All of a row's buttons move at once, so none is drawn half-way. */
    HDWP dw = BeginDeferWindowPos(n);
    for (int i = 0; i < n; i++) {
        HWND b = CTL(ids[i]);
        int bw = button_width(b);
        if (cx > x && cx + bw > x + w) {
            cx = x;
            cy += bh + gap;
        }
        if (dw) dw = DeferWindowPos(dw, b, NULL, cx, cy, bw, bh, SWP_NOZORDER | SWP_NOACTIVATE);
        else MoveWindow(b, cx, cy, bw, bh, TRUE);
        cx += bw + gap;
    }
    if (dw) EndDeferWindowPos(dw);
    return n ? cy + bh - y : 0;
}

static void layout_pages(void)
{
    if (!g_page[0]) return;
    RECT rc;
    GetClientRect(g_page[0], &rc);
    int w = rc.right, h = rc.bottom, pad = S(8), gap = S(6), iw = w - pad * 2, y, part;

    /* Primary Database: its buttons, then the tree (or the search
     * results) beside the details of what is selected and its versions. */
    y = pad + flow(g_db_btns, COUNT_OF(g_db_btns), pad, pad, iw) + gap;
    int body = h - y - pad, tree_w = (w - pad * 3) * 55 / 100, search_h = S(24), below = search_h + gap;
    mv(g_search, pad, y, tree_w, search_h);
    mv(g_tree, pad, y + below, tree_w, body - below);
    mv(g_results, pad, y + below, tree_w, body - below);
    int rx = pad * 2 + tree_w, rw = w - rx - pad;
    int list_h = (font_h(g_font) + S(8)) * 6; /* a header and five versions */
    if (list_h < body * 42 / 100) list_h = body * 42 / 100;
    if (list_h > body * 62 / 100) list_h = body * 62 / 100;
    int detail_h = body - list_h - gap;
    mv(g_detail, rx, y, rw, detail_h);
    mv(g_list, rx, y + detail_h + gap, rw, list_h);

    /* Next backup */
    int lw = button_width(CTL(ID_NEXT_FOR_LABEL)) - S(18);
    mv(CTL(ID_NEXT_FOR_LABEL), pad, pad + S(4), lw, S(20));
    MoveWindow(CTL(ID_NEXT_FOR), pad + lw + S(6), pad, iw - lw - S(6) < S(560) ? iw - lw - S(6) : S(560), S(320), TRUE);
    y = pad + S(28) + gap;
    {
        /* As tall as its lines, measured at the width it now has. */
        HWND ed = CTL(ID_NEXT_TEXT);
        mv(ed, pad, y, iw, S(92));
        g_next_text_lines = next_text_lines();
        y += mv(ed, pad, y, iw, g_next_text_lines * font_h(g_font) + S(10)) + gap;
    }
    y += flow(g_next_btns, COUNT_OF(g_next_btns), pad, y, iw) + gap;
    mv(CTL(ID_NEXT_LIST), pad, y, iw, h - y - pad);

    /* Local backups */
    y = pad + flow(g_local_btns, COUNT_OF(g_local_btns), pad, pad, iw) + gap;
    {
        int room = h - y - pad;
        int text_min = font_h(g_font) * 4 + S(12);
        part = local_list_need();
        if (part > room - text_min) part = room - text_min;
        if (part < room * 40 / 100) part = room * 40 / 100;
    }
    y += mv(CTL(ID_LOCAL_LIST), pad, y, iw, part) + gap;
    mv(CTL(ID_LOCAL_TEXT), pad, y, iw, h - y - pad);
    local_fit_cols();

    /* Starred files and extra places */
    y = pad + mv(CTL(ID_STAR_TEXT), pad, pad, iw, text_height(CTL(ID_STAR_TEXT), iw)) + gap;
    y += flow(g_star_btns, COUNT_OF(g_star_btns), pad, y, iw) + gap;
    part = (h - y - pad) * 38 / 100;
    y += mv(CTL(ID_STAR_LIST), pad, y, iw, part) + gap;
    y += mv(CTL(ID_PLACE_LABEL), pad, y, iw, text_height(CTL(ID_PLACE_LABEL), iw)) + S(2);
    y += flow(g_place_btns, COUNT_OF(g_place_btns), pad, y, iw) + gap;
    mv(CTL(ID_PLACE_LIST), pad, y, iw, h - y - pad);

    /* Encryption */
    y = pad + mv(CTL(ID_ENC_INTRO), pad, pad, iw, text_height(CTL(ID_ENC_INTRO), iw)) + gap;
    y += flow(g_enc_btns, COUNT_OF(g_enc_btns), pad, y, iw) + gap;
    {
        /* The table as tall as its rows, within reason; the folders get the rest. */
        int room = h - y - pad;
        part = enc_state_need();
        if (part > room * 60 / 100) part = room * 60 / 100;
        if (part < room * 30 / 100) part = room * 30 / 100;
    }
    y += mv(CTL(ID_ENC_STATE), pad, y, iw, part) + gap;
    lv_fit_all(CTL(ID_ENC_STATE), 2);
    y += mv(CTL(ID_ENC_LABEL), pad, y, iw, text_height(CTL(ID_ENC_LABEL), iw)) + S(2);
    y += flow(g_enc_btns2, COUNT_OF(g_enc_btns2), pad, y, iw) + gap;
    mv(CTL(ID_ENC_LIST), pad, y, iw, h - y - pad);

    /* Restore */
    y = pad + mv(CTL(ID_REST_TEXT), pad, pad, iw, text_height(CTL(ID_REST_TEXT), iw)) + gap;
    y += flow(g_rest_btns, COUNT_OF(g_rest_btns), pad, y, iw) + gap;
    part = (h - y - pad) * 40 / 100;
    y += mv(CTL(ID_REST_PLAN), pad, y, iw, part) + gap;
    y += mv(CTL(ID_REST_LABEL), pad, y, iw, text_height(CTL(ID_REST_LABEL), iw)) + S(2);
    y += flow(g_rest_btns2, COUNT_OF(g_rest_btns2), pad, y, iw) + gap;
    mv(CTL(ID_REST_LIST), pad, y, iw, h - y - pad);

    /* Whatever moved is drawn again whole: a button whose text got longer
     * pushes the rest of its row along, and a page drawn only in parts
     * shows pieces of the buttons where they were. */
    for (int i = 0; i < N_TABS; i++)
        if (g_page[i]) RedrawWindow(g_page[i], NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

/* The pages and their controls. The tree, search box, details and
 * versions list (g_tree...) are made by create_children, on page 0. */
static void create_pages(HWND hwnd)
{
    g_tabs = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP, 0, 0, 10, 10, hwnd,
                             (HMENU)ID_TABS, g_inst, NULL);
    SendMessageW(g_tabs, WM_SETFONT, (WPARAM)g_font, TRUE);
    for (int i = 0; i < N_TABS; i++) {
        TCITEMW it;
        memset(&it, 0, sizeof(it));
        it.mask = TCIF_TEXT;
        it.pszText = (LPWSTR)T(g_tab_names[i]);
        SendMessageW(g_tabs, TCM_INSERTITEMW, (WPARAM)i, (LPARAM)&it);
        g_page[i] = CreateWindowExW(WS_EX_CONTROLPARENT, L"BrodalfPage", L"", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 10, 10,
                                    hwnd, NULL, g_inst, NULL);
    }
    /* The tab control is drawn under its pages. */
    SetWindowPos(g_tabs, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    page_button(TAB_DB, T(L"Add backup root..."), ID_DB_ADD);
    page_toggle(TAB_DB, T(L"Star it"), ID_DB_STAR);
    page_toggle(TAB_DB, T(L"Encrypt it..."), ID_DB_ENCRYPT);
    page_button(TAB_DB, T(L"Bit-rot scan..."), ID_DB_BITROT);
    page_button(TAB_DB, T(L"Change where a root is..."), ID_DB_RELOCATE);
    page_button(TAB_DB, T(L"What to leave out..."), ID_DB_SKIP);
    page_button(TAB_DB, T(L"Deleted files..."), ID_DB_DELETED);
    page_button(TAB_DB, T(L"Damaged files..."), ID_DB_DAMAGED);
    page_button(TAB_DB, T(L"In use at the last scan..."), ID_DB_IN_USE);
    page_button(TAB_DB, T(L"Largest files..."), ID_DB_LARGEST);
    page_button(TAB_DB, T(L"Export the list (CSV)..."), ID_DB_EXPORT);

    page_label(TAB_NEXT, ID_NEXT_FOR_LABEL, T(L"What the next backup copies to:"));
    page_ctl(TAB_NEXT, WC_COMBOBOXW, L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, 0, ID_NEXT_FOR);
    g_ctl_btn[ID_NEXT_FOR - ID_PAGE_FIRST] = 1;
    page_text(TAB_NEXT, ID_NEXT_TEXT);
    page_primary(TAB_NEXT, T(L"Back up..."), ID_NEXT_BACKUP);
    page_button(TAB_NEXT, T(L"Back up to every plugged-in drive"), ID_NEXT_ALL);
    page_button(TAB_NEXT, T(L"Changed since its last backup..."), ID_NEXT_CHANGED);
    page_button(TAB_NEXT, T(L"Files at risk..."), ID_NEXT_RISK);
    page_button(TAB_NEXT, T(L"Refresh"), ID_NEXT_REFRESH);
    page_list(TAB_NEXT, ID_NEXT_LIST, NULL, NULL, 0);

    page_button(TAB_LOCAL, T(L"Set up a drive..."), ID_LOCAL_ADD);
    page_primary(TAB_LOCAL, T(L"Back up to it"), ID_LOCAL_BACKUP);
    page_button(TAB_LOCAL, T(L"Back up to every plugged-in drive"), ID_LOCAL_ALL);
    page_button(TAB_LOCAL, T(L"Check every copy"), ID_LOCAL_CHECK);
    page_button(TAB_LOCAL, T(L"Check copies not checked lately"), ID_LOCAL_VERIFY);
    page_button(TAB_LOCAL, T(L"Open in Explorer"), ID_LOCAL_OPEN);
    page_button(TAB_LOCAL, T(L"Compress copies..."), ID_LOCAL_COMPRESS);
    page_button(TAB_LOCAL, T(L"Changed since its last backup..."), ID_LOCAL_CHANGED);
    page_button(TAB_LOCAL, T(L"Name and where it is kept..."), ID_LOCAL_EDIT);
    page_button(TAB_LOCAL, T(L"Mark lost or broken..."), ID_LOCAL_LOST);
    page_button(TAB_LOCAL, T(L"Status of every drive..."), ID_LOCAL_STATUS);
    page_button(TAB_LOCAL, T(L"Files at risk..."), ID_LOCAL_RISK);
    page_check(TAB_LOCAL, T(L"Back up to several drives at once"), ID_LOCAL_PARALLEL);
    {
        static const int widths[] = {220, 170, 150, 170, 170, 160};
        page_list(TAB_LOCAL, ID_LOCAL_LIST, (const wchar_t **)g_local_cols, widths, 6);
        HWND lv = CTL(ID_LOCAL_LIST);
        ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_INFOTIP);
        ListView_EnableGroupView(lv, TRUE);
        local_metrics();
    }
    page_text(TAB_LOCAL, ID_LOCAL_TEXT);

    page_label(TAB_STARRED, ID_STAR_TEXT,
               T(L"Starred files are backed up to your drives like every file, and also to the extra places below: a second, "
               L"secondary backup of the files that matter most, such as your Dropbox, OneDrive or Google Drive folder on this PC, "
               L"a network share or a folder on another disk. BRODALF only copies files into the folder; that folder's own app does "
               L"any syncing. Copies in extra places never count toward your full copies."));
    page_button(TAB_STARRED, T(L"Star what is selected in Primary Database"), ID_STAR_ADD);
    page_button(TAB_STARRED, T(L"Unstar it"), ID_STAR_REMOVE);
    page_button(TAB_STARRED, T(L"Show it in Primary Database"), ID_STAR_SHOW);
    {
        static const wchar_t *cols[] = {N_(L"Starred"), N_(L"Backup root"), N_(L"Files"), N_(L"Size"), N_(L"Starred on"), N_(L"Note")};
        static const int widths[] = {300, 160, 70, 80, 120, 260};
        page_list(TAB_STARRED, ID_STAR_LIST, cols, widths, 6);
    }
    page_label(TAB_STARRED, ID_PLACE_LABEL, T(L"Extra places"));
    page_button(TAB_STARRED, T(L"Add an extra place..."), ID_PLACE_ADD);
    page_button(TAB_STARRED, T(L"Back up starred files to it"), ID_PLACE_BACKUP);
    page_button(TAB_STARRED, T(L"Change its size limit..."), ID_PLACE_LIMIT);
    page_button(TAB_STARRED, T(L"Open in Explorer"), ID_PLACE_OPEN);
    page_button(TAB_STARRED, T(L"Description and details..."), ID_PLACE_EDIT);
    page_button(TAB_STARRED, T(L"It is gone..."), ID_PLACE_LOST);
    {
        static const wchar_t *cols[] = {N_(L"Extra place"), N_(L"Folder"), N_(L"Found now"), N_(L"Size limit"), N_(L"Starred files there"),
                                        N_(L"Waiting"), N_(L"Over the limit"), N_(L"Space used"), N_(L"Encrypted")};
        static const int widths[] = {240, 220, 70, 80, 120, 110, 110, 80, 70};
        page_list(TAB_STARRED, ID_PLACE_LIST, cols, widths, 9);
    }

    page_button(TAB_ENCRYPT, T(L"Set a passphrase..."), ID_ENC_PASS);
    page_button(TAB_ENCRYPT, T(L"Forget the passphrase until it is needed"), ID_ENC_LOCK);
    page_button(TAB_ENCRYPT, T(L"Make a new recovery key..."), ID_ENC_KEY);
    page_button(TAB_ENCRYPT, T(L"Print the recovery sheet..."), ID_ENC_SHEET);
    page_button(TAB_ENCRYPT, T(L"Set up an encrypted drive..."), ID_ENC_DRIVE);
    page_check(TAB_ENCRYPT, T(L"Encrypt the database file"), ID_ENC_CATALOG);
    page_label(TAB_ENCRYPT, ID_ENC_INTRO, L"");
    {
        static const wchar_t *cols[] = {N_(L"What"), N_(L"State"), N_(L"What it means")};
        static const int widths[] = {260, 160, 640};
        page_list(TAB_ENCRYPT, ID_ENC_STATE, cols, widths, 3);
        ListView_SetExtendedListViewStyle(CTL(ID_ENC_STATE), LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    }
    page_label(TAB_ENCRYPT, ID_ENC_LABEL, T(L"Folders encrypted on every drive and extra place they are copied to:"));
    page_button(TAB_ENCRYPT, T(L"Encrypt what is selected in Primary Database"), ID_ENC_ADD);
    page_button(TAB_ENCRYPT, T(L"Choose a folder to encrypt..."), ID_ENC_PICK);
    page_button(TAB_ENCRYPT, T(L"Stop encrypting it..."), ID_ENC_REMOVE);
    page_button(TAB_ENCRYPT, T(L"Show it in Primary Database"), ID_ENC_SHOW);
    {
        static const wchar_t *cols[] = {N_(L"Encrypted"), N_(L"Backup root"), N_(L"Files"), N_(L"Size"), N_(L"Encrypting them takes"), N_(L"Chosen on")};
        static const int widths[] = {300, 160, 70, 80, 150, 120};
        page_list(TAB_ENCRYPT, ID_ENC_LIST, cols, widths, 6);
    }

    page_label(TAB_RESTORE, ID_REST_TEXT,
               T(L"Restoring copies files from your drives into a folder you choose. Nothing there is overwritten: a file already "
               L"where one would go is left alone and listed below, so you can compare the two."));
    page_primary(TAB_RESTORE, T(L"Restore what is selected in Primary Database..."), ID_REST_SEL);
    page_button(TAB_RESTORE, T(L"Restore everything..."), ID_REST_ALL);
    page_button(TAB_RESTORE, T(L"Restore deleted files..."), ID_REST_DELETED);
    page_button(TAB_RESTORE, T(L"Continue the restore that waits for drives"), ID_REST_CONTINUE);
    page_text(TAB_RESTORE, ID_REST_PLAN);
    page_label(TAB_RESTORE, ID_REST_LABEL, T(L"Files the last restore left alone"));
    page_button(TAB_RESTORE, T(L"Open in Explorer"), ID_REST_OPEN);
    page_button(TAB_RESTORE, T(L"Show them in a window..."), ID_REST_TABLE);
    {
        static const wchar_t *cols[] = {N_(L"File"), N_(L"Where it would go"), N_(L"Backed-up size"), N_(L"Backed-up modified"), N_(L"Size there"),
                                        N_(L"Modified there"), N_(L"Newer")};
        static const int widths[] = {260, 260, 90, 120, 80, 120, 140};
        page_list(TAB_RESTORE, ID_REST_LIST, cols, widths, 7);
    }
}

static void refresh_page(int i);

static void show_tab(int i)
{
    if (i < 0 || i >= N_TABS) return;
    g_tab = i;
    if (TabCtrl_GetCurSel(g_tabs) != i) TabCtrl_SetCurSel(g_tabs, i);
    ShowWindow(g_page[i], SW_SHOW);
    for (int k = 0; k < N_TABS; k++)
        if (k != i) ShowWindow(g_page[k], SW_HIDE);
    keep_focus(g_tabs); /* the focus was on the page now hidden */
    if (g_menu_view) CheckMenuRadioItem(g_menu_view, ID_M_TAB_FIRST, ID_M_TAB_FIRST + N_TABS - 1, ID_M_TAB_FIRST + i, MF_BYCOMMAND);
    DWORD v = (DWORD)i;
    RegSetKeyValueW(HKEY_CURRENT_USER, REG_KEY, L"LastTab", REG_DWORD, &v, sizeof(v));
    if (g_page_stale[i]) refresh_page(i);
}

/* The tab shown when BRODALF last closed. */
static int last_tab(void)
{
    DWORD v = 0, n = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, REG_KEY, L"LastTab", RRF_RT_REG_DWORD, NULL, &v, &n) != ERROR_SUCCESS || v >= N_TABS) return 0;
    return (int)v;
}

/* The Next backup text's lines, wrapped as it is now: two to five. */
static int next_text_lines(void)
{
    int n = (int)SendMessageW(CTL(ID_NEXT_TEXT), EM_GETLINECOUNT, 0, 0);
    return n < 2 ? 2 : n > 5 ? 5 : n;
}

static void set_text_if(int id, const wchar_t *text, int *changed)
{
    wchar_t old[1600];
    GetWindowTextW(CTL(id), old, 1600);
    if (wcscmp(old, text) != 0) {
        SetWindowTextW(CTL(id), text);
        *changed = 1;
    }
}

/* ---- Tab pages: what each one shows ---------------------------------------- */

/* "Pictures\2024" in "Photos", or the whole root. */
static void mark_name(const bd_mark_info *m, wchar_t *out, size_t cap)
{
    wchar_t *rel = widen(m->rel_path);
    if (rel && *rel) swprintf(out, cap, L"%ls", rel);
    else swprintf(out, cap, T(L"(the whole backup root)"));
    for (wchar_t *c = out; *c; c++) if (*c == L'/') *c = L'\\';
    free(rel);
}

/* Next backup ---------------------------------------------------------- */

#define NEXT_ROWS 2000
#define MAX_COPY_COLS 8
typedef struct { HWND lv; int sets, drive, goal_copies, goal_places; } next_fill;

static int next_row_cb(void *ctx, const bd_pending_info *p)
{
    next_fill *f = ctx;
    wchar_t *src = widen(p->source_name), *rel = widen(p->rel_path), file[1200], size[64], copies[48], places[48];
    swprintf(file, 1200, L"%ls\\%ls", src ? src : L"", rel ? rel : L"");
    for (wchar_t *c = file; *c; c++) if (*c == L'/') *c = L'\\';
    format_bytes(p->size, size, 64);
    swprintf(copies, 48, T(L"%d of %d"), p->copies, f->goal_copies);
    swprintf(places, 48, T(L"%d of %d"), p->places, f->goal_places);
    const wchar_t *cells[8 + MAX_COPY_COLS];
    int n = 0;
    cells[n++] = file;
    cells[n++] = size;
    if (f->drive) cells[n++] = p->too_big ? T(L"Too big for this drive (FAT32)") : p->is_new ? T(L"Not there yet") : T(L"An older version");
    cells[n++] = copies;
    cells[n++] = places;
    /* A tick where the file is in that full copy, a dash where it is not: never blank. */
    for (int k = 1; k <= f->sets; k++) cells[n++] = p->copies >= k ? L"\x2713" : L"\x2013";
    cells[n++] = p->starred ? L"\x2605" : L"";
    cells[n++] = p->encrypt ? T(L"Yes") : L"";
    lv_row(f->lv, cells, n);
    rows_add(&g_next_rows, &g_next_nrows, p->source_id, p->rel_path);
    free(src);
    free(rel);
    return 0;
}

static const media_row *next_opt(void)
{
    for (int i = 0; i < g_next_nopts; i++)
        if (g_next_opts[i].id == g_next_media) return &g_next_opts[i];
    return NULL;
}

static void fill_next(void)
{
    HWND combo = CTL(ID_NEXT_FOR), lv = CTL(ID_NEXT_LIST), ed = CTL(ID_NEXT_TEXT);
    g_filling++;
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    SendMessageW(lv, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(lv);
    lv_clear_columns(lv);
    rows_free(&g_next_rows, &g_next_nrows);
    g_next_nopts = 0;
    if (!g_cat) {
        SetWindowTextW(ed, L"");
        SendMessageW(lv, WM_SETREDRAW, TRUE, 0);
        g_filling--;
        return;
    }
    /* What to show it for: every file short of its copies, or one drive or extra place. */
    bd_target tg;
    memset(&tg, 0, sizeof(tg));
    tg.copies = 2;
    tg.places = 2;
    bd_target_get(g_cat, &tg);
    drive_list dl = load_drives();
    {
        wchar_t any[200];
        swprintf(any, 200, T(L"Any drive: every file short of %d copies in %d places"), tg.copies, tg.places);
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)any);
    }
    g_next_label[0] = 0;
    memset(&g_next_opts[0], 0, sizeof(g_next_opts[0]));
    g_next_nopts = 1;
    int sel = 0;
    const drive_fig *fig = NULL;
    for (int i = 0; i < dl.n && g_next_nopts < 65; i++) {
        const drive_snap *d = &dl.items[i];
        if (strcmp(d->kind, "drive") != 0 || d->lost_ms) continue;
        wchar_t item[700];
        wchar_t *lab = widen(d->label), *loc = widen(d->location);
        if (d->secondary) {
            swprintf(item, 700, T(L"%ls (extra place)"), lab ? lab : L"");
            wcscat_s(item, 700, L" \x00b7 ");
            wcscat_s(item, 700, T(L"starred files only"));
            if (!d->connected) {
                wcscat_s(item, 700, L" \x00b7 ");
                wcscat_s(item, 700, T(L"not found now"));
            }
        } else {
            swprintf(item, 700, L"%ls%ls%ls \x00b7 %ls", lab ? lab : L"", loc && *loc ? L" \x00b7 " : L"", loc && *loc ? loc : L"",
                     d->connected ? T(L"plugged in") : T(L"not plugged in"));
        }
        if (d->id == g_next_media) swprintf(g_next_label, 128, L"%ls", lab ? lab : L"");
        free(lab);
        free(loc);
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)item);
        media_row *o = &g_next_opts[g_next_nopts];
        o->id = d->id;
        o->connected = d->connected;
        o->lost = d->lost_ms != 0;
        o->secondary = d->secondary;
        if (d->id == g_next_media) {
            sel = g_next_nopts;
            fig = fig_of(d->id);
        }
        g_next_nopts++;
    }
    free(dl.items);
    if (!sel) g_next_media = 0; /* nothing chosen, or it is gone */
    SendMessageW(combo, CB_SETCURSEL, (WPARAM)sel, 0);
    const media_row *o = &g_next_opts[sel];

    int sets = tg.copies < 1 ? 1 : tg.copies > MAX_COPY_COLS ? MAX_COPY_COLS : tg.copies;
    int drive = g_next_media != 0;

    /* One column for each full copy you want: a tick when the file is in it. */
    const wchar_t *cols[8 + MAX_COPY_COLS];
    int widths[8 + MAX_COPY_COLS], n = 0;
    wchar_t setcol[MAX_COPY_COLS][24];
    cols[n] = T(L"File"), widths[n++] = 380;
    cols[n] = T(L"Size"), widths[n++] = 70;
    if (drive) cols[n] = o->secondary ? T(L"In this extra place") : T(L"On this drive"), widths[n++] = 120;
    cols[n] = T(L"Copies"), widths[n++] = 70;
    cols[n] = T(L"Places"), widths[n++] = 70;
    g_next_copy_col = n;
    g_next_copy_n = sets;
    for (int k = 0; k < sets; k++) {
        swprintf(setcol[k], 24, T(L"Copy %d"), k + 1);
        cols[n] = setcol[k], widths[n++] = 58;
    }
    cols[n] = T(L"Starred"), widths[n++] = 60;
    cols[n] = T(L"Encrypted"), widths[n++] = 70;
    lv_columns(lv, cols, widths, n);
    for (int k = 0; k < sets; k++) {
        LVCOLUMNW c;
        memset(&c, 0, sizeof(c));
        c.mask = LVCF_FMT;
        c.fmt = LVCFMT_CENTER;
        ListView_SetColumn(lv, g_next_copy_col + k, &c);
    }
    ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

    wtext t = {NULL, 0, 0};
    wchar_t a[64], b[64], when[64];
    if (!drive) {
        bd_risk_stats rs;
        memset(&rs, 0, sizeof(rs));
        /* The figures read after the last job, when there are some. */
        int have = g_figs && g_figs->has_risk;
        if (have) rs = g_figs->risk;
        else have = bd_list_at_risk(g_cat, 0, NULL, NULL, &rs) == BD_OK;
        if (have) {
            format_bytes(rs.bytes_at_risk, a, 64);
            if (root_count() == 0)
                wadd(&t, T(L"Nothing to back up yet. Add a backup root first."));
            else if (rs.files_at_risk == 0)
                wadd(&t, T(L"Every file has the %d copies in %d places you want. Nothing waits for a backup."), tg.copies, tg.places);
            else {
                wadd(&t, T(L"%lld of your %lld files (%ls) do not have the %d copies in %d places you want yet"),
                     (long long)rs.files_at_risk, (long long)rs.files_total, a, tg.copies, tg.places);
                if (rs.files_no_copy) wadd(&t, T(L", and %lld of them have no copy anywhere"), (long long)rs.files_no_copy);
                wadd(&t, L".");
            }
        }
        if (root_count() > 0) wadd(&t, T(L"\r\nEach Copy column is one full copy: \x2713 means the file is in it, \x2013 that it is not."));
    } else if (!o->secondary) {
        bd_backup_preview_info pv;
        memset(&pv, 0, sizeof(pv));
        int have = fig && fig->has_pv;
        if (have) pv = fig->pv;
        else have = bd_backup_preview(g_cat, g_next_media, 0, &pv) == BD_OK; /* a drive not plugged in: chosen just now */
        if (have) {
            if (!o->connected) wadd(&t, T(L"Plug in %ls to back up to it.\r\n"), g_next_label);
            format_bytes(pv.bytes_to_copy, a, 64);
            if (pv.files_to_copy == 0) wadd(&t, T(L"This drive is up to date: the next backup to it copies nothing."));
            else
                wadd(&t, TN(pv.files_to_copy, L"The next backup to this drive copies %lld file (%ls): %lld new, %lld changed. A changed file goes "
                                              L"alongside its older copy; nothing on the drive is overwritten.",
                            L"The next backup to this drive copies %lld files (%ls): %lld new, %lld changed. A changed file goes "
                            L"alongside its older copy; nothing on the drive is overwritten."),
                     (long long)pv.files_to_copy, a, (long long)pv.files_new, (long long)pv.files_changed);
            if (pv.free_bytes >= 0) {
                format_bytes(pv.free_bytes, b, 64);
                wadd(&t, T(L"\r\nFree on it: %ls%ls."), b, pv.free_is_current ? T(L" (read just now)") : T(L" (when it was last plugged in)"));
            }
            if (pv.bytes_short > 0) {
                format_bytes(pv.bytes_short, b, 64);
                wadd(&t, T(L" About %ls will not fit and waits for another drive."), b);
            }
            if (pv.files_too_big)
                wadd(&t, TN(pv.files_too_big, L" %lld file is 4 GB or more and cannot go on this drive, because it is FAT32; reformat it as "
                                              L"exFAT or NTFS to back it up here.",
                            L" %lld files are 4 GB or more and cannot go on this drive, because it is FAT32; reformat it as "
                            L"exFAT or NTFS to back them up here."),
                     (long long)pv.files_too_big);
            if (pv.last_scan_ms) {
                format_time_ms(pv.last_scan_ms, when, 64);
                wadd(&t, T(L"\r\nIt copies what the scans found; the oldest scan of your backup roots was %ls."), when);
            } else
                wadd(&t, T(L"\r\nYour backup roots have not been scanned yet, so there is nothing to copy. Press Scan now."));
            if (pv.mass_change && pv.held_deleted > 0)
                wadd(&t, TN(pv.held_deleted, L"\r\nWarning: of the %lld files whose copies on this drive were up to date, %lld have changed "
                                             L"and %lld was deleted at once. That can mean ransomware. Look at Changed since its last backup... before backing up.",
                            L"\r\nWarning: of the %lld files whose copies on this drive were up to date, %lld have changed "
                            L"and %lld were deleted at once. That can mean ransomware. Look at Changed since its last backup... before backing up."),
                     (long long)pv.files_held, (long long)(pv.held_changed - pv.held_deleted), (long long)pv.held_deleted);
            else if (pv.mass_change)
                wadd(&t, T(L"\r\nWarning: %lld of the %lld files whose copies on this drive were up to date have changed at once. That "
                         L"can mean ransomware. Look at Changed since its last backup... before backing up."),
                     (long long)pv.held_changed, (long long)pv.files_held);
        }
    } else {
        bd_place_status ps;
        memset(&ps, 0, sizeof(ps));
        int have = fig && fig->has_ps;
        if (have) ps = fig->ps;
        else have = bd_place_status_get(g_cat, g_next_media, &ps) == BD_OK;
        if (have) {
            format_bytes(ps.starred_bytes, a, 64);
            wadd(&t, L"%ls ", T(L"An extra place takes only starred files, as a second, secondary backup; its copies do not count toward "
                               L"your full copies."));
            wadd(&t, TN(ps.starred_files, L"%lld starred file (%ls) in all; %lld of them is there as it is now.",
                        L"%lld starred files (%ls) in all; %lld of them are there as they are now."),
                 (long long)ps.starred_files, a, (long long)ps.have_files);
            if (ps.waiting_files) {
                format_bytes(ps.waiting_bytes, b, 64);
                wadd(&t, T(L"\r\nThe next backup to it copies %lld (%ls)."), (long long)(ps.waiting_files - ps.over_limit_files), b);
            }
            if (ps.limit_bytes > 0) {
                format_bytes(ps.limit_bytes, b, 64);
                wadd(&t, T(L" It may take at most %ls of starred files."), b);
                if (ps.over_limit_files) {
                    format_bytes(ps.over_limit_bytes, a, 64);
                    wadd(&t, TN(ps.over_limit_files, L" %lld file (%ls) would go over that and is not copied there.",
                                L" %lld files (%ls) would go over that and are not copied there."),
                         (long long)ps.over_limit_files, a);
                }
            }
            if (ps.starred_files == 0) wadd(&t, T(L"\r\nNothing is starred yet: star files or folders on the Primary Database tab."));
            if (!o->connected) wadd(&t, T(L"\r\nThe folder was not found the last time BRODALF looked."));
        }
    }

    next_fill f = {lv, sets, drive, tg.copies, tg.places};
    int64_t total = 0;
    bd_list_pending(g_cat, g_next_media, NEXT_ROWS, next_row_cb, &f, &total);
    if (total > g_next_nrows)
        wadd(&t, T(L"\r\nThe list shows the first %d of %lld files, by backup root and path."), g_next_nrows, (long long)total);
    set_edit_text(ed, t.p ? t.p : L"");
    free(t.p);
    SendMessageW(lv, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(lv, NULL, TRUE);
    g_filling--;
    /* The text box is as tall as its lines: more room for the list. */
    if (next_text_lines() != g_next_text_lines) layout_pages();
}

/* The Copy columns: a green tick, a grey dash. */
static LRESULT next_custom_draw(NMLVCUSTOMDRAW *cd)
{
    switch (cd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT: return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT: return CDRF_NOTIFYSUBITEMDRAW;
    case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
        HWND lv = cd->nmcd.hdr.hwndFrom;
        int c = cd->iSubItem;
        if (g_next_copy_col >= 0 && c >= g_next_copy_col && c < g_next_copy_col + g_next_copy_n) {
            wchar_t v[8] = L"";
            ListView_GetItemText(lv, (int)cd->nmcd.dwItemSpec, c, v, 8);
            cd->clrText = g_hc ? GetSysColor(COLOR_WINDOWTEXT) : v[0] == 0x2713 ? tone_text(TONE_SAFE) : theme_gray();
        } else
            cd->clrText = ListView_GetTextColor(lv);
        return CDRF_NEWFONT;
    }
    }
    return CDRF_DODEFAULT;
}

/* Local backups --------------------------------------------------------- */

/* The drives' table: one row for each drive, in groups by whether it is
 * plugged in, each cell drawn by hand in two lines, what it is and then a
 * word more in grey. Each cell's own text says the same in words, for
 * sorting by a column and for screen readers. */
enum { LOCAL_PLUGGED = 1, LOCAL_UNPLUGGED, LOCAL_LOST };

typedef struct {
    int64_t id;
    int group, connected, lost, chip_tone, copies2_tone, space_tone, health_tone, checked2_tone;
    int64_t total, freeb, held, other, reserve;
    wchar_t name[128], where[200], chip[120], state2[200];
    wchar_t copies[96], copies2[96], space[128];
    wchar_t health[64], health2[128], checked[96], checked2[96];
    wchar_t tip[900];
} local_view;

static local_view g_local_view[256];
static int g_local_groups[3];
static HIMAGELIST g_local_il;
static int g_lv_h1, g_lv_h2;

static void friendly_time(int64_t ms, wchar_t *out, size_t cap);
static void card_state(hd_card *c, const drive_snap *d, const drive_fig *g);
static void card_tip(hd_card *c, const drive_snap *d, const drive_fig *g);
static int job_uses(int64_t id);
static const wchar_t *job_chip(void);
static int font_h(HFONT f);
static int draw_chip_fit(HDC dc, int x, int y, int h, const wchar_t *s, int tone, int max);
static const wchar_t *ui_locale(void);

/* 1234567 as "1,234,567", with the window's language's separator. */
static void group_digits(int64_t v, wchar_t *out, size_t cap)
{
    wchar_t sep[8] = L",", digits[32];
    if (!GetLocaleInfoEx(ui_locale(), LOCALE_STHOUSAND, sep, 8)) wcscpy(sep, L",");
    swprintf(digits, 32, L"%lld", (long long)(v < 0 ? -v : v));
    size_t n = wcslen(digits), o = 0;
    if (v < 0 && o + 1 < cap) out[o++] = L'-';
    for (size_t i = 0; i < n && o + 1 < cap; i++) {
        out[o++] = digits[i];
        size_t left = n - i - 1;
        if (left && left % 3 == 0)
            for (const wchar_t *s = sep; *s && o + 1 < cap; s++) out[o++] = *s;
    }
    out[o] = 0;
}

static void join_part(wchar_t *buf, size_t cap, const wchar_t *part)
{
    size_t len = wcslen(buf);
    if (!*part || len + 4 >= cap) return;
    swprintf(buf + len, cap - len, L"%ls%ls", len ? L" \x00B7 " : L"", part);
}

/* Rows tall enough for two lines: a table's rows are as tall as its
 * images, so it has a list of blank images that tall. */
static void local_metrics(void)
{
    HWND lv = CTL(ID_LOCAL_LIST);
    if (!lv) return;
    int h = font_h(g_font_semi) + font_h(g_font) + S(14);
    if (h < S(40)) h = S(40);
    HIMAGELIST il = ImageList_Create(1, h, ILC_COLOR32, 1, 0);
    ListView_SetImageList(lv, il, LVSIL_SMALL);
    if (g_local_il) ImageList_Destroy(g_local_il);
    g_local_il = il;
    HWND tip = ListView_GetToolTips(lv);
    if (tip) SendMessageW(tip, TTM_SETMAXTIPWIDTH, 0, S(480));
}

static void local_group_text(int id, wchar_t *out, size_t cap)
{
    int n = id >= 1 && id <= 3 ? g_local_groups[id - 1] : 0;
    swprintf(out, cap, id == LOCAL_PLUGGED ? T(L"Plugged in (%d)") : id == LOCAL_UNPLUGGED ? T(L"Not plugged in (%d)")
                                                                   : T(L"Lost or broken, copies not counted (%d)"),
             n);
}

/* What one row shows, from the drive and what the last job read. */
static void local_view_fill(local_view *v, const drive_snap *d)
{
    memset(v, 0, sizeof(*v));
    const drive_fig *g = fig_of(d->id);
    hd_card c;
    memset(&c, 0, sizeof(c));
    c.id = d->id;
    c.connected = d->connected;
    c.lost = d->lost_ms != 0;
    c.secondary = d->secondary;
    c.total = d->total;
    c.freeb = d->freeb;
    c.held = d->bytes_held;
    wchar_t *w = widen(d->label);
    swprintf(c.name, 128, L"%ls", w ? w : L"");
    free(w);
    w = widen(d->location);
    swprintf(c.where, 200, L"%ls", w ? w : L"");
    free(w);
    w = widen(d->root);
    swprintf(c.path, 400, L"%ls", w ? w : L"");
    free(w);
    card_state(&c, d, g);
    card_tip(&c, d, g);
    /* The card's last line says to click the card: not for a row. */
    wchar_t *last = NULL;
    for (wchar_t *p = wcsstr(c.tip, L"\r\n"); p; p = wcsstr(p + 2, L"\r\n")) last = p;
    if (last) *last = 0;

    v->id = d->id;
    v->connected = d->connected;
    v->lost = d->lost_ms != 0;
    v->group = v->lost ? LOCAL_LOST : d->connected ? LOCAL_PLUGGED : LOCAL_UNPLUGGED;
    swprintf(v->name, 128, L"%ls", c.name);
    swprintf(v->where, 200, L"%ls", c.where[0] ? c.where : T(L"Where it is kept: not set"));
    swprintf(v->chip, 120, L"%ls", c.chip);
    v->chip_tone = c.tone;
    swprintf(v->tip, 900, L"%ls", c.tip);

    wchar_t a[96], b[96];
    if (d->connected) swprintf(v->state2, 200, T(L"Plugged in at %ls"), c.path);
    else if (d->last_seen_ms > 0) {
        friendly_time(d->last_seen_ms, a, 96);
        swprintf(v->state2, 200, T(L"Last seen %ls"), a);
    } else
        swprintf(v->state2, 200, L"%ls", T(L"Not seen yet"));

    format_bytes(d->bytes_held, a, 96);
    swprintf(b, 96, TN(d->copies, L"%lld copy", L"%lld copies"), (long long)d->copies);
    swprintf(v->copies, 96, L"%ls \x00B7 %ls", b, a);
    if (g && g->has_ms && g->ms.copies_bad > 0) {
        swprintf(v->copies2, 96, T(L"%lld missing or damaged"), (long long)g->ms.copies_bad);
        v->copies2_tone = TONE_BAD;
    }

    v->total = d->total;
    v->freeb = d->freeb;
    v->held = d->bytes_held;
    v->reserve = d->reserve_bytes;
    v->other = d->total - d->freeb - d->bytes_held;
    if (v->other < 0) v->other = 0;
    if (d->total > 0) {
        format_bytes(d->freeb, a, 96);
        format_bytes(d->total, b, 96);
        swprintf(v->space, 128, T(L"%ls free of %ls"), a, b);
        int free_pct = (int)(d->freeb * 100 / d->total);
        v->space_tone = free_pct < 5 ? TONE_BAD : free_pct < 15 ? TONE_WARN : TONE_NEUTRAL;
    } else
        swprintf(v->space, 128, L"%ls", T(L"Size not known yet"));

    /* Health: a word, then what the drive said; nothing it did not say. */
    const bd_drive_hw *h = &d->hw;
    if (!d->has_hw) swprintf(v->health, 64, L"%ls", T(L"Not read yet"));
    else if (!h->smart || !h->health[0]) swprintf(v->health, 64, L"%ls", T(L"Not reported"));
    else if (strcmp(h->health, "failing") == 0) {
        swprintf(v->health, 64, L"%ls", T(L"Failing"));
        v->health_tone = TONE_BAD;
    } else if (strcmp(h->health, "warning") == 0 || replace_advice(d)) {
        swprintf(v->health, 64, L"%ls", T(L"Warning"));
        v->health_tone = TONE_WARN;
    } else {
        swprintf(v->health, 64, L"%ls", T(L"Good"));
        v->health_tone = TONE_SAFE;
    }
    if (d->has_hw) {
        if (h->temperature_c >= 0) {
            swprintf(a, 96, L"%d \x00B0" L"C", h->temperature_c);
            join_part(v->health2, 128, a);
        }
        if (h->power_on_hours > 0) {
            group_digits(h->power_on_hours, b, 96);
            swprintf(a, 96, T(L"%ls h"), b);
            join_part(v->health2, 128, a);
        }
        if (h->percent_used >= 0) {
            swprintf(a, 96, T(L"%d%% worn"), h->percent_used);
            join_part(v->health2, 128, a);
        }
    }

    /* When its copies were last read back to check them. */
    if (d->copies == 0) swprintf(v->checked, 96, L"%ls", T(L"No copies yet"));
    else {
        int64_t last = g && g->has_ms ? g->ms.last_check_ms : 0;
        if (last <= 0) last = d->oldest_check_ms;
        if (last > 0) {
            friendly_time(last, v->checked, 96);
            v->checked[0] = (wchar_t)towupper(v->checked[0]);
        } else
            swprintf(v->checked, 96, L"%ls", T(L"Never"));
        if (d->oldest_unread_ms) {
            swprintf(v->checked2, 96, L"%ls", T(L"Some copies never checked"));
            v->checked2_tone = TONE_WARN;
        } else if (d->check_due) {
            swprintf(v->checked2, 96, L"%ls", T(L"Due for a check"));
            v->checked2_tone = TONE_WARN;
        }
    }
}

/* Columns as wide as what they hold; what is left goes to all of them. */
static void local_fit_cols(void)
{
    HWND lv = CTL(ID_LOCAL_LIST);
    HWND hd = lv ? ListView_GetHeader(lv) : NULL;
    if (!hd || Header_GetItemCount(hd) != 6) return;
    RECT cr;
    GetClientRect(lv, &cr);
    int avail = cr.right - cr.left - S(2);
    if (!(GetWindowLongW(lv, GWL_STYLE) & WS_VSCROLL)) avail -= GetSystemMetrics(SM_CXVSCROLL);
    if (avail < S(240)) return;
    HDC dc = GetDC(lv);
    int need[6], pad = S(16);
    for (int c = 0; c < 6; c++) need[c] = text_w(dc, g_font, T(g_local_cols[c])) + S(28);
    for (int i = 0; i < g_local_n; i++) {
        const local_view *v = &g_local_view[i];
        int w0 = text_w(dc, g_font_semi, v->name), w1 = text_w(dc, g_font, v->where);
        int x = S(10) + S(24) + S(10) + (w0 > w1 ? w0 : w1) + S(8);
        if (x > need[0]) need[0] = x;
        w0 = text_w(dc, g_font, v->chip) + S(12);
        w1 = text_w(dc, g_font, v->state2) + S(2);
        x = pad + (w0 > w1 ? w0 : w1);
        if (x > need[1]) need[1] = x;
        w0 = text_w(dc, g_font, v->copies);
        w1 = text_w(dc, g_font, v->copies2);
        x = pad + (w0 > w1 ? w0 : w1);
        if (x > need[2]) need[2] = x;
        w0 = text_w(dc, g_font, v->space);
        x = pad + (w0 > S(120) ? w0 : S(120));
        if (x > need[3]) need[3] = x;
        w0 = text_w(dc, g_font, v->health);
        w1 = text_w(dc, g_font, v->health2);
        x = pad + (w0 > w1 ? w0 : w1);
        if (x > need[4]) need[4] = x;
        w0 = text_w(dc, g_font, v->checked);
        w1 = text_w(dc, g_font, v->checked2);
        x = pad + (w0 > w1 ? w0 : w1);
        if (x > need[5]) need[5] = x;
    }
    ReleaseDC(lv, dc);
    int sum = 0;
    for (int c = 0; c < 6; c++) sum += need[c];
    if (sum <= avail) {
        int extra = avail - sum;
        for (int c = 0; c < 6; c++) need[c] += extra / 6;
    } else {
        int over = sum - avail;
        for (int c = 0; c < 6; c++) {
            need[c] -= (int)((long long)over * need[c] / sum);
            if (need[c] < S(64)) need[c] = S(64);
        }
    }
    for (int c = 0; c < 6; c++) ListView_SetColumnWidth(lv, c, need[c]);
}

/* How tall the table wants to be: its header, groups and rows. */
static int local_list_need(void)
{
    HWND lv = CTL(ID_LOCAL_LIST);
    int groups = 0, rowh = S(40);
    for (int i = 0; i < 3; i++)
        if (g_local_groups[i]) groups++;
    RECT ir;
    if (ListView_GetItemCount(lv) > 0 && ListView_GetItemRect(lv, 0, &ir, LVIR_BOUNDS)) rowh = ir.bottom - ir.top;
    RECT hr = {0, 0, 0, 0};
    HWND hd = ListView_GetHeader(lv);
    if (hd) GetWindowRect(hd, &hr);
    int head = hr.bottom - hr.top;
    if (head <= 0) head = S(26);
    return head + groups * (font_h(g_font_semi) + S(22)) + g_local_n * rowh + S(18);
}

static COLORREF local_bg(void) { return g_hc ? GetSysColor(COLOR_WINDOW) : g_dark ? DARK_FIELD : GetSysColor(COLOR_WINDOW); }

/* Two lines in a cell, or one in its middle when there is no second. */
static void cell_lines(HDC dc, RECT r, HFONT f1, COLORREF c1, const wchar_t *s1, COLORREF c2, const wchar_t *s2)
{
    UINT fl = DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS;
    if (!s2 || !*s2) {
        draw_str(dc, f1, c1, s1, r, fl | DT_VCENTER);
        return;
    }
    int h = g_lv_h1 + S(2) + g_lv_h2, top = r.top + (r.bottom - r.top - h) / 2;
    RECT a = {r.left, top, r.right, top + g_lv_h1};
    RECT b = {r.left, top + g_lv_h1 + S(2), r.right, top + h};
    draw_str(dc, f1, c1, s1, a, fl);
    draw_str(dc, g_font, c2, s2, b, fl);
}

/* A drive's space: its copies, other files, free, and at the end what
 * BRODALF keeps free, hatched. */
static void space_bar(HDC dc, RECT r, const local_view *v)
{
    round_box(dc, &r, S(4), ui_track(), CLR_INVALID);
    int w = r.right - r.left;
    if (w <= 0 || v->total <= 0) return;
    HRGN clip = CreateRoundRectRgn(r.left, r.top, r.right + 1, r.bottom + 1, S(4), S(4));
    SelectClipRgn(dc, clip);
    int held_w = (int)((long long)w * v->held / v->total), other_w = (int)((long long)w * v->other / v->total);
    if (v->held > 0 && held_w < S(2)) held_w = S(2);
    int64_t res = v->reserve < v->freeb ? v->reserve : v->freeb;
    int res_w = res > 0 ? (int)((long long)w * res / v->total) : 0;
    if (res > 0 && res_w < S(2)) res_w = S(2);
    RECT a = {r.left, r.top, r.left + held_w, r.bottom};
    if (a.right > r.right) a.right = r.right;
    fill_color(dc, &a, g_hc ? GetSysColor(COLOR_HIGHLIGHT) : ui_link());
    RECT b = {a.right, r.top, a.right + other_w, r.bottom};
    if (b.right > r.right) b.right = r.right;
    fill_color(dc, &b, g_hc ? GetSysColor(COLOR_GRAYTEXT) : g_dark ? RGB(125, 125, 125) : RGB(165, 165, 165));
    if (res_w > 0) {
        RECT k = {r.right - res_w, r.top, r.right, r.bottom};
        HBRUSH hb = CreateHatchBrush(HS_BDIAGONAL, g_hc ? GetSysColor(COLOR_WINDOWTEXT) : g_dark ? RGB(150, 150, 150) : RGB(130, 130, 130));
        int om = SetBkMode(dc, OPAQUE);
        COLORREF ob = SetBkColor(dc, ui_track());
        FillRect(dc, &k, hb);
        SetBkColor(dc, ob);
        SetBkMode(dc, om);
        DeleteObject(hb);
    }
    SelectClipRgn(dc, NULL);
    DeleteObject(clip);
}

static void local_paint_row(NMLVCUSTOMDRAW *cd, int pos, const local_view *v)
{
    HWND lv = cd->nmcd.hdr.hwndFrom;
    HDC dc = cd->nmcd.hdc;
    RECT row;
    if (!ListView_GetItemRect(lv, pos, &row, LVIR_BOUNDS)) return;
    HWND hd = ListView_GetHeader(lv);
    if (!hd || Header_GetItemCount(hd) < 6) return;
    int rh = row.bottom - row.top;
    int sel = (ListView_GetItemState(lv, pos, LVIS_SELECTED) & LVIS_SELECTED) != 0, focus = GetFocus() == lv;
    COLORREF bg = local_bg();
    if (sel) {
        if (g_hc) bg = GetSysColor(COLOR_HIGHLIGHT);
        else if (focus) bg = g_dark ? RGB(0x26, 0x4F, 0x78) : RGB(0xCC, 0xE4, 0xF7);
        else bg = g_dark ? RGB(0x2E, 0x3B, 0x48) : RGB(0xE5, 0xF0, 0xFA);
    }
    fill_color(dc, &row, bg);
    if (sel && focus && !g_hc) {
        RECT ab = {row.left + S(1), row.top + S(6), row.left + S(4), row.bottom - S(6)};
        round_box(dc, &ab, S(3), ui_link(), CLR_INVALID);
    } else if (!sel) {
        RECT ln = {row.left + S(8), row.bottom - 1, row.right - S(8), row.bottom};
        fill_color(dc, &ln, g_hc ? GetSysColor(COLOR_GRAYTEXT) : ui_border());
    }
    int hc_sel = sel && g_hc;
    COLORREF text = hc_sel ? GetSysColor(COLOR_HIGHLIGHTTEXT) : v->lost ? theme_gray() : ui_text();
    COLORREF sub = hc_sel ? GetSysColor(COLOR_HIGHLIGHTTEXT) : ui_sub();
#define TONED(t) ((t) && !hc_sel ? tone_text(t) : sub)

    RECT cell[6];
    for (int c = 1; c < 6; c++) {
        ListView_GetSubItemRect(lv, pos, c, LVIR_BOUNDS, &cell[c]);
        cell[c].left += S(8);
        cell[c].right -= S(6);
        cell[c].top = row.top;
        cell[c].bottom = row.bottom;
    }
    cell[0] = row;
    cell[0].right = cell[1].left - S(8) - S(6);
    cell[0].left += S(10);

    /* The drive: its icon, marked plugged in or lost, its name, where it is kept. */
    int isz = S(24), iy = row.top + (rh - isz) / 2;
    if (g_ico_drive) DrawIconEx(dc, cell[0].left, iy, g_ico_drive, isz, isz, 0, NULL, DI_NORMAL);
    if (v->lost) draw_state_icon(dc, cell[0].left + isz - S(9), iy + isz - S(9), S(12), TONE_BAD, GLYPH_CROSS);
    else if (v->connected) draw_state_icon(dc, cell[0].left + isz - S(9), iy + isz - S(9), S(12), TONE_SAFE, GLYPH_CHECK);
    RECT c0 = cell[0];
    c0.left += isz + S(10);
    cell_lines(dc, c0, g_font_semi, text, v->name, sub, v->where);

    /* Its state, as the drive's card in the header says it. */
    int busy = job_uses(v->id);
    const wchar_t *chip = busy ? job_chip() : v->chip;
    int tone = busy ? TONE_WORK : v->chip_tone;
    int ch = g_lv_h2 + S(4), block = ch + S(3) + g_lv_h2;
    int top = v->state2[0] ? row.top + (rh - block) / 2 : row.top + (rh - ch) / 2;
    draw_chip_fit(dc, cell[1].left, top, ch, chip, tone, cell[1].right - cell[1].left);
    if (v->state2[0]) {
        RECT s2 = {cell[1].left + S(2), top + ch + S(3), cell[1].right, top + block};
        draw_str(dc, g_font, sub, v->state2, s2, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    cell_lines(dc, cell[2], g_font, text, v->copies, TONED(v->copies2_tone), v->copies2);

    if (v->total > 0) {
        int bw = cell[3].right - cell[3].left, bh = S(8);
        if (bw > S(160)) bw = S(160);
        int sblock = bh + S(5) + g_lv_h2, sy = row.top + (rh - sblock) / 2;
        RECT bar = {cell[3].left, sy, cell[3].left + bw, sy + bh};
        space_bar(dc, bar, v);
        RECT st = {cell[3].left, sy + bh + S(5), cell[3].right, sy + sblock};
        draw_str(dc, g_font, v->space_tone ? TONED(v->space_tone) : text, v->space, st, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    } else
        draw_str(dc, g_font, sub, v->space, cell[3], DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    cell_lines(dc, cell[4], g_font, TONED(v->health_tone), v->health, sub, v->health2);
    cell_lines(dc, cell[5], g_font, text, v->checked, TONED(v->checked2_tone), v->checked2);
#undef TONED
}

static void local_paint_group(NMLVCUSTOMDRAW *cd)
{
    HWND lv = cd->nmcd.hdr.hwndFrom;
    HDC dc = cd->nmcd.hdc;
    int id = (int)cd->nmcd.dwItemSpec;
    RECT r;
    if (!ListView_GetGroupRect(lv, id, LVGGR_HEADER, &r)) r = cd->rcText;
    fill_color(dc, &r, local_bg());
    wchar_t s[200];
    local_group_text(id, s, 200);
    RECT t = r;
    t.left += S(10);
    int tw = text_w(dc, g_font_semi, s);
    draw_str(dc, g_font_semi, id == LOCAL_LOST ? tone_text(TONE_BAD) : ui_text(), s, t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    int ly = (r.top + r.bottom) / 2;
    RECT ln = {t.left + tw + S(10), ly, r.right - S(10), ly + 1};
    if (ln.right > ln.left) fill_color(dc, &ln, g_hc ? GetSysColor(COLOR_GRAYTEXT) : ui_border());
}

static LRESULT local_custom_draw(NMLVCUSTOMDRAW *cd)
{
    switch (cd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
        if (cd->dwItemType == LVCDI_GROUP) {
            local_paint_group(cd);
            return CDRF_SKIPDEFAULT;
        } else {
            TEXTMETRICW tm;
            HGDIOBJ o = SelectObject(cd->nmcd.hdc, g_font_semi);
            GetTextMetricsW(cd->nmcd.hdc, &tm);
            g_lv_h1 = tm.tmHeight;
            SelectObject(cd->nmcd.hdc, g_font);
            GetTextMetricsW(cd->nmcd.hdc, &tm);
            g_lv_h2 = tm.tmHeight;
            SelectObject(cd->nmcd.hdc, o);
        }
        return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT: {
        int i = (int)cd->nmcd.lItemlParam;
        if (i < 0 || i >= g_local_n) return CDRF_DODEFAULT;
        local_paint_row(cd, (int)cd->nmcd.dwItemSpec, &g_local_view[i]);
        return CDRF_SKIPDEFAULT;
    }
    }
    return CDRF_DODEFAULT;
}

static void local_info_tip(NMLVGETINFOTIPW *tip)
{
    int i = lv_index(tip->hdr.hwndFrom, tip->iItem);
    if (i >= 0 && i < g_local_n && tip->pszText && tip->cchTextMax > 0) lstrcpynW(tip->pszText, g_local_view[i].tip, tip->cchTextMax);
}

static void local_detail(void)
{
    HWND ed = CTL(ID_LOCAL_TEXT);
    int r = lv_selected(ID_LOCAL_LIST);
    if (!g_cat) { SetWindowTextW(ed, L""); return; }
    if (r < 0 || r >= g_local_n) {
        SetWindowTextW(ed, g_local_n ? T(L"Select a drive to see where it stands, its space and its health.")
                                     : T(L"No backup drives yet. Set up a drive... makes one: an external or removable drive, or a "
                                       L"folder on one."));
        return;
    }
    drive_list dl = load_drives();
    const drive_snap *d = find_drive(&dl, g_local_rows[r].id);
    if (d) {
        wtext t = {NULL, 0, 0};
        wchar_t added[64];
        add_drive_heading(&t, d, L"");
        format_time_ms(d->added_ms, added, 64);
        wadd(&t, T(L"    Set up %ls, %lld copies recorded\r\n\r\n"), added, (long long)d->copies);
        add_drive_status(&t, d->id, L"");
        wadd(&t, L"\r\n");
        add_drive_lines(&t, d, L"");
        SetWindowTextW(ed, t.p ? t.p : L"");
        free(t.p);
    }
    free(dl.items);
}

static void fill_local(void)
{
    HWND lv = CTL(ID_LOCAL_LIST);
    int r = lv_selected(ID_LOCAL_LIST), sel = -1;
    int64_t keep = r >= 0 && r < g_local_n ? g_local_rows[r].id : 0;
    int was_n = g_local_n, was_groups[3];
    memcpy(was_groups, g_local_groups, sizeof(was_groups));
    g_filling++;
    SendMessageW(lv, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(lv);
    ListView_RemoveAllGroups(lv);
    ListView_EnableGroupView(lv, TRUE);
    g_local_n = 0;
    memset(g_local_groups, 0, sizeof(g_local_groups));
    if (g_cat) {
        drive_list dl = load_drives();
        static wchar_t names[256][160];
        for (int i = 0; i < dl.n && g_local_n < 256; i++) {
            const drive_snap *d = &dl.items[i];
            if (strcmp(d->kind, "drive") != 0 || d->secondary) continue;
            local_view_fill(&g_local_view[g_local_n], d);
            drive_name(d->label, d->location, names[g_local_n], 160);
            g_local_groups[g_local_view[g_local_n].group - 1]++;
            media_row *m = &g_local_rows[g_local_n];
            m->id = d->id;
            m->connected = d->connected;
            m->lost = d->lost_ms != 0;
            m->secondary = 0;
            if (d->id == keep) sel = g_local_n;
            g_local_n++;
        }
        free(dl.items);
        for (int gid = LOCAL_PLUGGED; gid <= LOCAL_LOST; gid++) {
            if (!g_local_groups[gid - 1]) continue;
            wchar_t head[200];
            local_group_text(gid, head, 200);
            LVGROUP gr;
            memset(&gr, 0, sizeof(gr));
            gr.cbSize = sizeof(gr);
            gr.mask = LVGF_HEADER | LVGF_GROUPID;
            gr.iGroupId = gid;
            gr.pszHeader = head;
            ListView_InsertGroup(lv, -1, &gr);
        }
        /* The cells' own text: the name first, as the drive is known
         * everywhere else, then each cell in words. */
        for (int i = 0; i < g_local_n; i++) {
            const local_view *v = &g_local_view[i];
            wchar_t health[200], checked[200];
            swprintf(health, 200, L"%ls%ls%ls", v->health, v->health2[0] ? L", " : L"", v->health2);
            swprintf(checked, 200, L"%ls%ls%ls", v->checked, v->checked2[0] ? L", " : L"", v->checked2);
            const wchar_t *cells[] = {names[i], v->chip, v->copies, v->space, health, checked};
            lv_row(lv, cells, 6);
            LVITEMW it;
            memset(&it, 0, sizeof(it));
            it.mask = LVIF_GROUPID;
            it.iItem = ListView_GetItemCount(lv) - 1;
            it.iGroupId = v->group;
            SendMessageW(lv, LVM_SETITEMW, 0, (LPARAM)&it);
        }
    }
    if (sel < 0 && g_local_n) sel = 0;
    lv_select(lv, sel);
    SendMessageW(lv, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(lv, NULL, TRUE);
    g_filling--;
    /* More or fewer rows: the table and the details share the room again. */
    if (g_local_n != was_n || memcmp(was_groups, g_local_groups, sizeof(was_groups)) != 0) layout_pages();
    else local_fit_cols();
    local_detail();
}

/* Starred files and extra places ---------------------------------------- */

typedef struct { HWND lv; } star_fill;

static int star_row_cb(void *ctx, const bd_mark_info *m)
{
    star_fill *f = ctx;
    rows_add(&g_star_rows, &g_star_nrows, m->source_id, m->rel_path);
    wchar_t what[1200], files[32], size[64], when[64], *src = widen(m->source_name);
    mark_name(m, what, 1200);
    swprintf(files, 32, L"%lld", (long long)m->files);
    format_bytes(m->bytes, size, 64);
    format_time_ms(m->added_ms, when, 64);
    const wchar_t *cells[] = {what, src ? src : L"", files, size, when,
                              m->found ? L"" : T(L"Not in the last scan: renamed, moved or deleted?")};
    lv_row(f->lv, cells, 6);
    free(src);
    return 0;
}

/* The row of rows that is (source_id, rel), or -1. */
static int row_of(const mark_key *rows, int n, int64_t source_id, const char *rel)
{
    for (int i = 0; i < n; i++)
        if (rows[i].source_id == source_id && strcmp(rows[i].rel, rel) == 0) return i;
    return -1;
}

static void fill_starred(void)
{
    HWND lv = CTL(ID_STAR_LIST), pl = CTL(ID_PLACE_LIST);
    int r = lv_selected(ID_STAR_LIST), pr = lv_selected(ID_PLACE_LIST), sel = -1, psel = -1;
    mark_key keep = {0, NULL};
    if (r >= 0 && r < g_star_nrows) { keep.source_id = g_star_rows[r].source_id; keep.rel = xstrdup(g_star_rows[r].rel); }
    int64_t keep_place = pr >= 0 && pr < g_place_n ? g_place_rows[pr].id : 0;
    g_filling++;
    SendMessageW(lv, WM_SETREDRAW, FALSE, 0);
    SendMessageW(pl, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(lv);
    ListView_DeleteAllItems(pl);
    rows_free(&g_star_rows, &g_star_nrows);
    g_place_n = 0;
    if (g_cat) {
        star_fill f = {lv};
        bd_list_stars(g_cat, star_row_cb, &f);
        if (keep.rel) sel = row_of(g_star_rows, g_star_nrows, keep.source_id, keep.rel);

        int64_t files = 0, bytes = 0;
        bd_starred_totals(g_cat, &files, &bytes);
        wchar_t size[64], label[400];
        format_bytes(bytes, size, 64);
        drive_list dl = load_drives();
        for (int i = 0; i < dl.n && g_place_n < 256; i++) {
            const drive_snap *d = &dl.items[i];
            if (strcmp(d->kind, "drive") != 0 || !d->secondary) continue;
            bd_place_status ps;
            memset(&ps, 0, sizeof(ps));
            const drive_fig *g = fig_of(d->id); /* read after the last job */
            if (g && g->has_ps) ps = g->ps;
            else bd_place_status_get(g_cat, d->id, &ps); /* added just now */
            wchar_t name[600], limit[64], there[96], waiting[96] = L"", over[96] = L"", used[64], a[64];
            drive_name(d->label, d->location, name, 600);
            wchar_t *root = widen(d->root);
            if (ps.limit_bytes > 0) format_bytes(ps.limit_bytes, limit, 64);
            else wcscpy(limit, T(L"No limit"));
            format_bytes(ps.have_bytes, a, 64);
            swprintf(there, 96, T(L"%lld of %lld (%ls)"), (long long)ps.have_files, (long long)ps.starred_files, a);
            if (ps.waiting_files > ps.over_limit_files) {
                format_bytes(ps.waiting_bytes - ps.over_limit_bytes, a, 64);
                swprintf(waiting, 96, L"%lld (%ls)", (long long)(ps.waiting_files - ps.over_limit_files), a);
            }
            if (ps.over_limit_files) {
                format_bytes(ps.over_limit_bytes, a, 64);
                swprintf(over, 96, L"%lld (%ls)", (long long)ps.over_limit_files, a);
            }
            format_bytes(ps.used_bytes, used, 64);
            const wchar_t *found = d->lost_ms ? (strcmp(d->lost_why, "broken") == 0 ? T(L"Marked broken") : T(L"Marked lost"))
                                 : d->connected ? T(L"Yes") : T(L"No");
            const wchar_t *cells[] = {name, root ? root : L"", found, limit, there, waiting, over, used, d->encrypted ? T(L"Yes") : T(L"No")};
            lv_row(pl, cells, 9);
            free(root);
            media_row *m = &g_place_rows[g_place_n];
            m->id = d->id;
            m->connected = d->connected;
            m->lost = d->lost_ms != 0;
            m->secondary = 1;
            if (d->id == keep_place) psel = g_place_n;
            g_place_n++;
        }
        free(dl.items);
        swprintf(label, 400, TN(files, L"Extra places: a second, secondary backup of your %lld starred file (%ls)%ls",
                                L"Extra places: a second, secondary backup of your %lld starred files (%ls)%ls"),
                 (long long)files, size, g_place_n ? L"" : T(L". None yet: Add an extra place... sets one up."));
        int changed = 0;
        set_text_if(ID_PLACE_LABEL, label, &changed);
        if (changed) layout_pages();
    }
    free(keep.rel);
    if (sel < 0 && g_star_nrows) sel = 0;
    if (psel < 0 && g_place_n) psel = 0;
    lv_select(lv, sel);
    lv_select(pl, psel);
    SendMessageW(lv, WM_SETREDRAW, TRUE, 0);
    SendMessageW(pl, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(lv, NULL, TRUE);
    InvalidateRect(pl, NULL, TRUE);
    g_filling--;
}

/* Encryption ------------------------------------------------------------ */

typedef struct { HWND lv; int64_t bps; } enc_fill;

/* The tab's table: what can be encrypted, and whether it is. By row: is it
 * on (drawn in colour), the drive it is about, what a double-click does. */
#define ENC_STATE_MAX 80
static int g_enc_state_n, g_enc_state_on[ENC_STATE_MAX], g_enc_state_cmd[ENC_STATE_MAX];
static int64_t g_enc_state_drive[ENC_STATE_MAX];

static void enc_state_row(HWND lv, const wchar_t *what, const wchar_t *state, int on, const wchar_t *means, int64_t drive, int cmd)
{
    if (g_enc_state_n >= ENC_STATE_MAX) return;
    const wchar_t *cells[] = {what, state, means};
    lv_row(lv, cells, 3);
    g_enc_state_on[g_enc_state_n] = on;
    g_enc_state_drive[g_enc_state_n] = drive;
    g_enc_state_cmd[g_enc_state_n] = cmd;
    g_enc_state_n++;
}

static int enc_state_need(void)
{
    HWND lv = CTL(ID_ENC_STATE);
    int rowh = line_height() + S(6);
    RECT ir;
    if (ListView_GetItemCount(lv) > 0 && ListView_GetItemRect(lv, 0, &ir, LVIR_BOUNDS)) rowh = ir.bottom - ir.top;
    RECT hr = {0, 0, 0, 0};
    HWND hd = ListView_GetHeader(lv);
    if (hd) GetWindowRect(hd, &hr);
    int head = hr.bottom - hr.top;
    if (head <= 0) head = S(26);
    return head + (g_enc_state_n ? g_enc_state_n : 4) * rowh + S(8);
}

/* The State column in colour: blue for what is on, grey for what is not. */
static LRESULT enc_state_draw(NMLVCUSTOMDRAW *cd)
{
    switch (cd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT: return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT: return CDRF_NOTIFYSUBITEMDRAW;
    case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
        HWND lv = cd->nmcd.hdr.hwndFrom;
        int item = (int)cd->nmcd.dwItemSpec, r = lv_index(lv, item);
        int selected = (ListView_GetItemState(lv, item, LVIS_SELECTED) & LVIS_SELECTED) != 0;
        if (cd->iSubItem == 1 && !g_hc && !selected && r >= 0 && r < g_enc_state_n)
            cd->clrText = g_enc_state_on[r] ? tone_text(TONE_WORK) : theme_gray();
        else
            cd->clrText = ListView_GetTextColor(lv);
        return CDRF_NEWFONT;
    }
    }
    return CDRF_DODEFAULT;
}

static int enc_row_cb(void *ctx, const bd_mark_info *m)
{
    enc_fill *f = ctx;
    rows_add(&g_enc_rows, &g_enc_nrows, m->source_id, m->rel_path);
    wchar_t what[1200], files[32], size[64], takes[64], when[64], *src = widen(m->source_name);
    mark_name(m, what, 1200);
    swprintf(files, 32, L"%lld", (long long)m->files);
    format_bytes(m->bytes, size, 64);
    if (f->bps > 0) format_duration((double)m->bytes / (double)f->bps, takes, 64);
    else wcscpy(takes, T(L"not measured yet"));
    format_time_ms(m->added_ms, when, 64);
    const wchar_t *cells[] = {what, src ? src : L"", files, size, takes, when};
    lv_row(f->lv, cells, 6);
    free(src);
    return 0;
}

static void fill_encrypt(void)
{
    HWND sl = CTL(ID_ENC_STATE), lv = CTL(ID_ENC_LIST);
    int r = lv_selected(ID_ENC_LIST), sel = -1, was_rows = g_enc_state_n;
    mark_key keep = {0, NULL};
    if (r >= 0 && r < g_enc_nrows) { keep.source_id = g_enc_rows[r].source_id; keep.rel = xstrdup(g_enc_rows[r].rel); }
    g_filling++;
    SendMessageW(lv, WM_SETREDRAW, FALSE, 0);
    SendMessageW(sl, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(lv);
    ListView_DeleteAllItems(sl);
    g_enc_state_n = 0;
    rows_free(&g_enc_rows, &g_enc_nrows);
    if (!g_cat) {
        SendMessageW(lv, WM_SETREDRAW, TRUE, 0);
        SendMessageW(sl, WM_SETREDRAW, TRUE, 0);
        free(keep.rel);
        g_filling--;
        return;
    }
    int has = bd_catalog_has_passphrase(g_cat), unlocked = bd_catalog_is_unlocked(g_cat);
    bd_encrypt_stats es;
    memset(&es, 0, sizeof(es));
    bd_encrypt_stats_get(g_cat, &es);
    enc_fill f = {lv, es.bytes_per_second};
    bd_list_encrypt_paths(g_cat, enc_row_cb, &f);
    if (keep.rel) sel = row_of(g_enc_rows, g_enc_nrows, keep.source_id, keep.rel);
    free(keep.rel);

    /* The table: the passphrase, this file, every drive and extra place. */
    if (!has)
        enc_state_row(sl, T(L"Passphrase"), T(L"Not set"), 0, T(L"Each kind of encryption needs one: Set a passphrase... above."), 0, ID_ENC_PASS);
    else {
        enc_state_row(sl, T(L"Passphrase"), unlocked ? T(L"Set, entered for now") : T(L"Set"), 1,
                      unlocked ? T(L"BRODALF can encrypt, and read encrypted copies, until you press Forget the passphrase or close it.")
                               : T(L"Not entered now: BRODALF asks for it when a job needs it."),
                      0, ID_ENC_PASS);
        int key = bd_catalog_has_recovery_key(g_cat);
        enc_state_row(sl, T(L"Recovery key"), key ? T(L"Made") : T(L"None yet"), key,
                      key ? T(L"It is on the recovery sheet, and opens your encrypted backups if the passphrase is ever forgotten.")
                          : T(L"Make a new recovery key... puts one on a recovery sheet to print."),
                      0, 0);
    }
    int file_enc = bd_catalog_file_encrypted(g_cat);
    enc_state_row(sl, T(L"This database file"), file_enc ? T(L"Encrypted") : T(L"Not encrypted"), file_enc,
                  file_enc ? T(L"The list of your files and where their copies are. BRODALF asks for the passphrase when it opens it.")
                           : T(L"The list of your files and where their copies are. Tick Encrypt the database file above to encrypt it."),
                  0, 0);
    drive_list dl = load_drives();
    int n_drives = 0;
    for (int i = 0; i < dl.n; i++) {
        const drive_snap *d = &dl.items[i];
        if (strcmp(d->kind, "drive") != 0) continue;
        wchar_t name[600], what[700];
        drive_name(d->label, d->location, name, 600);
        swprintf(what, 700, L"%ls%ls", name, d->secondary ? T(L" (extra place)") : L"");
        enc_state_row(sl, what, !d->encrypted ? T(L"Not encrypted") : d->hide_names ? T(L"Encrypted, names hidden") : T(L"Encrypted"),
                      d->encrypted,
                      !d->encrypted ? T(L"Plain copies, except the folders you chose below. It stays as it was set up.")
                      : d->hide_names ? T(L"Every copy on it is encrypted, and the names of its files and folders are hidden.")
                                      : T(L"Every copy on it is encrypted. Its file and folder names stay readable."),
                      d->id, 0);
        n_drives++;
    }
    free(dl.items);
    if (!n_drives)
        enc_state_row(sl, T(L"Drives and extra places"), T(L"No drives yet"), 0,
                      T(L"Set up an encrypted drive... above sets up a drive with every copy on it encrypted."), 0, ID_ENC_DRIVE);

    /* Above the buttons: that it is all optional, and the time it takes when
     * no folders are chosen; with folders, the time goes with them. */
    wchar_t a[64], cost[1600];
    encrypt_cost_text(g_enc_nrows ? es.bytes : 0, cost, 1600);
    wtext t = {NULL, 0, 0};
    wadd(&t, L"%ls", T(L"Nothing is encrypted unless you choose it, and every kind is optional: use none, any one of them, or all of them together."));
    if (!g_enc_nrows) wadd(&t, L" %ls", cost);
    int changed = 0;
    set_text_if(ID_ENC_INTRO, t.p ? t.p : L"", &changed);
    free(t.p);

    /* Over the folders' table: what is in them, how far it got, the time. */
    wtext lb = {NULL, 0, 0};
    wadd(&lb, L"%ls", T(L"Folders encrypted on every drive and extra place they are copied to:"));
    if (!g_enc_nrows)
        wadd(&lb, L" %ls", T(L"none yet. Choose a folder to encrypt just it, also on drives that are not encrypted. Everything else stays "
                             L"plain, which is quicker to back up and to restore."));
    else {
        format_bytes(es.bytes, a, 64);
        wadd(&lb, L" ");
        wadd(&lb, TN(es.files, L"%lld file (%ls), encrypted wherever it is copied. Its name stays readable, with .bdenc after it.",
                     L"%lld files (%ls), encrypted wherever they are copied. Their names stay readable, with .bdenc after them."),
             (long long)es.files, a);
        wchar_t sealed[200];
        swprintf(sealed, 200, TN(es.sealed_copies, L"%lld encrypted copy so far", L"%lld encrypted copies so far"), (long long)es.sealed_copies);
        wadd(&lb, L" %ls.", sealed);
        if (es.files_waiting) {
            wchar_t waiting[300];
            swprintf(waiting, 300, TN(es.files_waiting, L"%lld file still waits for a first encrypted copy, which the next backups make.",
                                      L"%lld files still wait for a first encrypted copy, which the next backups make."),
                     (long long)es.files_waiting);
            wadd(&lb, L" %ls", waiting);
        }
        if (es.plain_copies) {
            wadd(&lb, L" ");
            wadd(&lb, TN(es.plain_copies, L"%lld plain copy was made before you chose to encrypt them. It stays as it is, since BRODALF "
                                          L"never overwrites or deletes anything on a drive.",
                         L"%lld plain copies were made before you chose to encrypt them. They stay as they are, since BRODALF "
                         L"never overwrites or deletes anything on a drive."),
                 (long long)es.plain_copies);
        }
        wadd(&lb, L"\r\n%ls", cost);
    }
    set_text_if(ID_ENC_LABEL, lb.p ? lb.p : L"", &changed);
    free(lb.p);

    if (sel < 0 && g_enc_nrows) sel = 0;
    lv_select(lv, sel);
    SendMessageW(lv, WM_SETREDRAW, TRUE, 0);
    SendMessageW(sl, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(lv, NULL, TRUE);
    InvalidateRect(sl, NULL, TRUE);
    g_filling--;
    if (changed || was_rows != g_enc_state_n) layout_pages();
    else lv_fit_all(sl, 2);
}

/* Restore --------------------------------------------------------------- */

/* The backup root's path and the path inside it, as Explorer shows it. */
static wchar_t *where_text(int64_t source_id, const char *rel)
{
    source_list l = {0};
    bd_list_sources(g_cat, collect_source, &l);
    wchar_t *out = NULL;
    for (int i = 0; i < l.n; i++) {
        if (l.items[i].source_id == source_id && !out) {
            size_t n = strlen(l.paths[i]) + strlen(rel ? rel : "") + 4;
            char *p = malloc(n);
            if (p) {
                size_t pl = strlen(l.paths[i]);
                snprintf(p, n, "%s%s%s", l.paths[i], rel && *rel && pl && l.paths[i][pl - 1] != '\\' ? "\\" : "", rel ? rel : "");
                for (char *c = p; *c; c++) if (*c == '/') *c = '\\';
                out = widen(p);
                free(p);
            }
        }
        free(l.names[i]);
        free(l.paths[i]);
        free(l.labels[i]);
    }
    free(l.items);
    free(l.names);
    free(l.paths);
    free(l.labels);
    return out;
}

static void fill_restore(void)
{
    HWND ed = CTL(ID_REST_PLAN), lv = CTL(ID_REST_LIST);
    int keep = lv_selected(ID_REST_LIST);
    g_filling++;
    SendMessageW(lv, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(lv);
    if (!g_cat) {
        SetWindowTextW(ed, L"");
        SendMessageW(lv, WM_SETREDRAW, TRUE, 0);
        g_filling--;
        return;
    }
    wtext t = {NULL, 0, 0};
    node_ref *r = item_ref(TreeView_GetSelection(g_tree));
    if (!r)
        wadd(&t, T(L"Select a backup root, a folder or a file on the Primary Database tab to see which drives restoring it needs.\n"));
    else {
        wchar_t *what = where_text(r->source_id, r->rel);
        wadd(&t, T(L"Selected on the Primary Database tab: %ls\n"), what ? what : L"");
        free(what);
        wchar_t older[600];
        version_note(r, older, 600);
        if (older[0]) wadd(&t, L"%ls\n", older);
        plan_text p;
        memset(&p, 0, sizeof(p));
        int64_t total = 0, none = 0;
        bd_restore_plan(g_cat, r->source_id, r->rel ? r->rel : "", NULL, plan_text_cb, &p, &total, &none);
        if (total == 0 && none == 0) wadd(&t, T(L"    There is nothing in it to restore.\n"));
        else {
            if (p.n) wadd(&t, T(L"Restoring it takes these drives, in this order:\n%ls"), p.t.p);
            if (none) wadd(&t, TN(none, L"%lld file has no copy on any drive.\n", L"%lld files have no copy on any drive.\n"), (long long)none);
            if (p.any_offline)
                wadd(&t, T(L"BRODALF restores what the plugged-in drives hold first; then plug in each other drive and press Continue.\n"));
        }
        free(p.t.p);
    }
    if (g_last_restore[0]) {
        wchar_t when[96];
        friendly_time(g_last_restore_ms, when, 96);
        wadd(&t, T(L"\nLast restore, %ls: %ls\n"), when, g_last_restore);
    }
    if (g_pending.active) {
        wchar_t *dest = widen(g_pending.dest);
        plan_text p;
        memset(&p, 0, sizeof(p));
        bd_restore_plan(g_cat, g_pending.source_id, g_pending.rel, g_pending.dest, plan_text_cb, &p, NULL, NULL);
        wadd(&t, T(L"\nA restore into %ls is waiting for these drives:\n%ls"), dest ? dest : L"",
             p.t.p ? p.t.p : T(L"    (none: it can finish now; press Continue)\n"));
        free(p.t.p);
        free(dest);
    }
    set_edit_text(ed, t.p ? t.p : L"");
    free(t.p);

    wchar_t label[300];
    if (g_n_conflicts)
        swprintf(label, 300, T(L"Files the last restore left alone (%d), because another file was already where each would go. "
                             L"Nothing was overwritten:"), g_n_conflicts);
    else
        wcscpy(label, T(L"Files the last restore left alone: none."));
    int changed = 0;
    set_text_if(ID_REST_LABEL, label, &changed);
    if (changed) layout_pages();
    for (int i = 0; i < g_n_conflicts; i++) {
        const conflict_row *c = &g_conflicts[i];
        wchar_t ws[64], wm[64], hs[64], hm[64];
        format_bytes(c->want_size, ws, 64);
        format_ns(c->want_mtime_ns, wm, 64);
        if (c->have_size < 0) wcscpy(hs, T(L"a folder"));
        else format_bytes(c->have_size, hs, 64);
        format_ns(c->have_mtime_ns, hm, 64);
        const wchar_t *newer = c->want_mtime_ns > c->have_mtime_ns ? T(L"The backed-up copy")
                             : c->want_mtime_ns < c->have_mtime_ns ? T(L"The file there")
                             : c->want_size != c->have_size ? T(L"Same time, other size") : T(L"Same time and size");
        const wchar_t *cells[] = {c->where ? c->where : L"", c->dest ? c->dest : L"", ws, wm, hs, hm, newer};
        lv_row(lv, cells, 7);
    }
    lv_select(lv, keep >= 0 && keep < g_n_conflicts ? keep : g_n_conflicts ? 0 : -1);
    SendMessageW(lv, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(lv, NULL, TRUE);
    g_filling--;
}

/* ---- Tab pages: buttons and what they do ------------------------------------ */

/* Which buttons work now: only with a database open and no job running,
 * and some only with something chosen. */
static void page_buttons_state(void)
{
    if (!g_page[0]) return;
    int on = g_cat && !g_busy;
    for (int id = ID_PAGE_FIRST; id < ID_PAGE_LAST; id++)
        if (g_ctl_btn[id - ID_PAGE_FIRST] && CTL(id)) EnableWindow(CTL(id), on);
    if (!on) return;
    int changed = 0;
    node_ref *r = item_ref(TreeView_GetSelection(g_tree));
    const char *rel = r && r->rel ? r->rel : "";
    int st = r ? bd_starred(g_cat, r->source_id, rel) : 0, en = r ? bd_encrypt_marked(g_cat, r->source_id, rel) : 0;
    /* Down while it is starred or encrypted; down and greyed when that
     * comes from a folder above it. */
    set_toggle(ID_DB_STAR, st != 0);
    EnableWindow(CTL(ID_DB_STAR), r && st != 2);
    set_toggle(ID_DB_ENCRYPT, en != 0);
    EnableWindow(CTL(ID_DB_ENCRYPT), r && en != 2);
    wchar_t dmg[64];
    if (g_figs && g_figs->n_damaged > 0) swprintf(dmg, 64, T(L"Damaged files (%lld)..."), (long long)g_figs->n_damaged);
    else swprintf(dmg, 64, L"%ls", T(L"Damaged files..."));
    set_text_if(ID_DB_DAMAGED, dmg, &changed);

    const media_row *o = next_opt();
    {
        wchar_t go[200];
        if (g_next_media && g_next_label[0]) swprintf(go, 200, T(L"Back up to %ls"), g_next_label);
        else swprintf(go, 200, L"%ls", T(L"Back up..."));
        set_text_if(ID_NEXT_BACKUP, go, &changed);
    }
    EnableWindow(CTL(ID_NEXT_BACKUP), !g_next_media || (o && o->connected && !o->lost));
    EnableWindow(CTL(ID_NEXT_CHANGED), g_next_media && o && !o->secondary);

    int lr = lv_selected(ID_LOCAL_LIST);
    const media_row *m = lr >= 0 && lr < g_local_n ? &g_local_rows[lr] : NULL;
    EnableWindow(CTL(ID_LOCAL_BACKUP), m && m->connected && !m->lost);
    EnableWindow(CTL(ID_LOCAL_CHECK), m && m->connected);
    EnableWindow(CTL(ID_LOCAL_VERIFY), m && m->connected);
    EnableWindow(CTL(ID_LOCAL_OPEN), m && m->connected);
    EnableWindow(CTL(ID_LOCAL_COMPRESS), m && m->connected && !m->secondary);
    EnableWindow(CTL(ID_LOCAL_CHANGED), m != NULL);
    EnableWindow(CTL(ID_LOCAL_EDIT), m != NULL);
    EnableWindow(CTL(ID_LOCAL_LOST), m != NULL);
    set_text_if(ID_LOCAL_LOST, m && m->lost ? T(L"Found it") : T(L"Mark lost or broken..."), &changed);
    SendMessageW(CTL(ID_LOCAL_PARALLEL), BM_SETCHECK, bd_option_get(g_cat, "parallel_copies") > 0 ? BST_CHECKED : BST_UNCHECKED, 0);

    EnableWindow(CTL(ID_STAR_ADD), r && st == 0);
    int sr = lv_selected(ID_STAR_LIST), has_star = sr >= 0 && sr < g_star_nrows;
    EnableWindow(CTL(ID_STAR_REMOVE), has_star);
    EnableWindow(CTL(ID_STAR_SHOW), has_star);
    int pr = lv_selected(ID_PLACE_LIST);
    const media_row *p = pr >= 0 && pr < g_place_n ? &g_place_rows[pr] : NULL;
    EnableWindow(CTL(ID_PLACE_BACKUP), p && p->connected && !p->lost);
    EnableWindow(CTL(ID_PLACE_LIMIT), p != NULL);
    EnableWindow(CTL(ID_PLACE_OPEN), p && p->connected);
    EnableWindow(CTL(ID_PLACE_EDIT), p != NULL);
    EnableWindow(CTL(ID_PLACE_LOST), p != NULL);
    set_text_if(ID_PLACE_LOST, p && p->lost ? T(L"Found it") : T(L"It is gone..."), &changed);

    int has = bd_catalog_has_passphrase(g_cat);
    set_text_if(ID_ENC_PASS, has ? T(L"Change the passphrase...") : T(L"Set a passphrase..."), &changed);
    EnableWindow(CTL(ID_ENC_LOCK), has && bd_catalog_is_unlocked(g_cat));
    EnableWindow(CTL(ID_ENC_KEY), has);
    EnableWindow(CTL(ID_ENC_CATALOG), has);
    SendMessageW(CTL(ID_ENC_CATALOG), BM_SETCHECK, bd_catalog_file_encrypted(g_cat) ? BST_CHECKED : BST_UNCHECKED, 0);
    EnableWindow(CTL(ID_ENC_ADD), r && en == 0);
    int er = lv_selected(ID_ENC_LIST), has_enc = er >= 0 && er < g_enc_nrows;
    EnableWindow(CTL(ID_ENC_REMOVE), has_enc);
    EnableWindow(CTL(ID_ENC_SHOW), has_enc);

    EnableWindow(CTL(ID_REST_SEL), r != NULL);
    EnableWindow(CTL(ID_REST_CONTINUE), g_pending.active);
    int cr = lv_selected(ID_REST_LIST);
    EnableWindow(CTL(ID_REST_OPEN), cr >= 0 && cr < g_n_conflicts);
    EnableWindow(CTL(ID_REST_TABLE), g_n_conflicts > 0);
    if (changed) layout_pages();
}

static void refresh_page(int i)
{
    if (g_busy) { g_page_stale[i] = 1; return; } /* the catalog is the job's: filled when it ends */
    g_page_stale[i] = 0;
    switch (i) {
    case TAB_NEXT: fill_next(); break;
    case TAB_LOCAL: fill_local(); break;
    case TAB_STARRED: fill_starred(); break;
    case TAB_ENCRYPT: fill_encrypt(); break;
    case TAB_RESTORE: fill_restore(); break;
    }
    page_buttons_state();
}

/* Something may have changed: refill the tab on show, the others when
 * they are next shown. */
static void refresh_pages(void)
{
    for (int i = 0; i < N_TABS; i++) g_page_stale[i] = 1;
    if (g_page[0]) refresh_page(g_tab);
}

/* ---- Tab pages: commands ----------------------------------------------------- */

/* Go to a file or folder on the Primary Database tab. */
static void show_in_db(int64_t source_id, const char *rel)
{
    show_tab(TAB_DB);
    if (GetWindowTextLengthW(g_search) > 0) end_search();
    reveal_node(source_id, rel ? rel : "");
    SetFocus(g_tree);
}

/* Which backup root a folder is in, and its path there; 0 if none. */
static int path_to_source(const char *path, int64_t *source_id, char **rel)
{
    source_list l = {0};
    bd_list_sources(g_cat, collect_source, &l);
    int found = 0;
    size_t best = 0;
    for (int i = 0; i < l.n; i++) {
        const char *rp = l.paths[i] ? l.paths[i] : "";
        size_t n = strlen(rp);
        while (n && (rp[n - 1] == '\\' || rp[n - 1] == '/')) n--;
        if (n && n >= best && _strnicmp(path, rp, n) == 0 && (path[n] == 0 || path[n] == '\\' || path[n] == '/')) {
            const char *in = path + n;
            while (*in == '\\' || *in == '/') in++;
            free(*rel);
            *rel = xstrdup(in);
            if (*rel) {
                for (char *c = *rel; *c; c++) if (*c == '\\') *c = '/';
                size_t rl = strlen(*rel);
                while (rl && (*rel)[rl - 1] == '/') (*rel)[--rl] = 0;
            }
            *source_id = l.items[i].source_id;
            best = n;
            found = 1;
        }
        free(l.names[i]);
        free(l.paths[i]);
        free(l.labels[i]);
    }
    free(l.items);
    free(l.names);
    free(l.paths);
    free(l.labels);
    return found && *rel;
}

/* Save after a change; say why if it could not be made. 1 if saved. */
static int save_or_say(bd_status s, const wchar_t *what)
{
    if (s == BD_OK) s = ui_save();
    if (s == BD_OK) return 1;
    wchar_t *e = widen(bd_catalog_error(g_cat)), m[1400];
    swprintf(m, 1400, L"%ls\n\n%ls", what, e && *e ? e : L"");
    MessageBoxW(g_main, m, APP_NAME, MB_ICONWARNING);
    free(e);
    return 0;
}

/* Extra places that are not marked lost or broken. */
static int count_places(void)
{
    drive_list dl = load_drives();
    int n = 0;
    for (int i = 0; i < dl.n; i++)
        if (dl.items[i].secondary && !dl.items[i].lost_ms) n++;
    free(dl.items);
    return n;
}

static void cmd_add_place(void);

static void set_star(int64_t source_id, const char *rel, int on)
{
    bd_status s = bd_star_set(g_cat, source_id, rel, on);
    if (!save_or_say(s, on ? T(L"Could not star it.") : T(L"Could not unstar it."))) return;
    wchar_t *w = where_text(source_id, rel), line[1600];
    int places = count_places();
    if (on)
        swprintf(line, 1600, T(L"Starred %ls. %ls"), w ? w : L"",
                 places ? T(L"The next backup to each extra place copies it there.")
                        : T(L"Add an extra place on the Starred files tab to give it a second, secondary copy there."));
    else
        swprintf(line, 1600, T(L"Unstarred %ls. Its copies in extra places stay there; nothing new of it goes there."), w ? w : L"");
    free(w);
    log_append(line);
    load_marks();
    rebuild_tree();
    show_detail();
    if (on && !places &&
        ask_verbs(g_main, T(L"Starred files are copied to your extra places as a second, secondary backup, and you have none "
                            L"yet. Add one now, like your Dropbox or OneDrive folder on this PC?"), MB_ICONQUESTION | MB_YESNO, T(L"Add an extra place"), T(L"Not now")) == IDYES)
        cmd_add_place();
}

static void toggle_star_selected(void)
{
    node_ref *r = item_ref(TreeView_GetSelection(g_tree));
    if (!r) {
        MessageBoxW(g_main, T(L"Select a backup root, a folder or a file on the Primary Database tab first."), APP_NAME, MB_ICONINFORMATION);
        return;
    }
    const char *rel = r->rel ? r->rel : "";
    int st = bd_starred(g_cat, r->source_id, rel);
    if (st == 2) {
        MessageBoxW(g_main, T(L"It is already starred, with a folder above it. Unstar that folder on the Starred files tab to change it."),
                    APP_NAME, MB_ICONINFORMATION);
        return;
    }
    int64_t sid = r->source_id;
    char *keep = xstrdup(rel); /* the tree is rebuilt: r goes */
    if (keep) set_star(sid, keep, st == 0);
    free(keep);
}

typedef struct { int64_t source_id; const char *rel; int64_t files, bytes; } find_mark;

static int find_mark_cb(void *ctx, const bd_mark_info *m)
{
    find_mark *f = ctx;
    if (m->source_id == f->source_id && strcmp(m->rel_path, f->rel) == 0) {
        f->files = m->files;
        f->bytes = m->bytes;
    }
    return 0;
}

/* Encrypt a file or folder on every drive and extra place it goes to, or
 * stop, after saying what that means for time. */
static void set_encrypt(int64_t source_id, const char *rel, int on)
{
    wchar_t *w = where_text(source_id, rel);
    if (!on) {
        wchar_t q[1600];
        swprintf(q, 1600, T(L"Stop encrypting %ls?\n\nNew copies of it are then plain. The encrypted copies already on your drives stay "
                          L"as they are and still restore with the passphrase."), w ? w : L"");
        if (ask_verbs(g_main, q, MB_ICONQUESTION | MB_YESNO, T(L"Stop encrypting"), T(L"Cancel")) == IDYES &&
            save_or_say(bd_encrypt_path_set(g_cat, source_id, rel, 0), T(L"Could not change the encryption."))) {
            swprintf(q, 1600, T(L"New copies of %ls are plain from now on."), w ? w : L"");
            log_append(q);
        }
        free(w);
        load_marks();
        rebuild_tree();
        show_detail();
        return;
    }
    if (!bd_catalog_has_passphrase(g_cat) &&
        (ask_verbs(g_main, T(L"Encrypting needs a passphrase, and none is set yet. Without it, encrypted copies cannot be read, by you "
                             L"or anyone.\n\nSet one now?"), MB_ICONQUESTION | MB_YESNO, T(L"Set a passphrase"), T(L"Cancel")) != IDYES ||
         !set_passphrase_ui())) {
        free(w);
        return;
    }
    /* Set it to see what it covers; taken back if the user says no. */
    bd_status s = bd_encrypt_path_set(g_cat, source_id, rel, 1);
    if (s != BD_OK) {
        save_or_say(s, T(L"Could not encrypt it."));
        free(w);
        return;
    }
    find_mark fm = {source_id, rel, 0, 0};
    bd_list_encrypt_paths(g_cat, find_mark_cb, &fm);
    wchar_t cost[1600], size[64];
    encrypt_cost_text(fm.bytes, cost, 1600);
    format_bytes(fm.bytes, size, 64);
    wtext q = {NULL, 0, 0};
    wadd(&q, TN(fm.files, L"Encrypt %ls?\n\nIts %lld file (%ls), and any added to it later, are then encrypted on every drive and extra place "
                          L"they are copied to, also drives that are not encrypted. Everything else stays as it is. Names stay readable: a copy is "
                          L"the file's name with .bdenc after it.\n\n",
                L"Encrypt %ls?\n\nIts %lld files (%ls), and any added to it later, are then encrypted on every drive and extra place "
                L"they are copied to, also drives that are not encrypted. Everything else stays as it is. Names stay readable: a copy is "
                L"the file's name with .bdenc after it.\n\n"),
         w ? w : L"", (long long)fm.files, size);
    wadd(&q, T(L"What it means for time: %ls\n\n"), cost);
    wadd(&q, T(L"Copies already on your drives stay as they are, since BRODALF never overwrites or deletes anything there: new copies "
             L"and new versions are encrypted. Backing up, checking and restoring them need the passphrase entered."));
    int yes = ask_verbs(g_main, q.p ? q.p : L"", MB_ICONQUESTION | MB_YESNO, T(L"Encrypt"), T(L"Cancel")) == IDYES;
    free(q.p);
    if (!yes) bd_encrypt_path_set(g_cat, source_id, rel, 0);
    else if (save_or_say(BD_OK, T(L"Could not encrypt it."))) {
        wchar_t line[1600];
        swprintf(line, 1600, T(L"%ls is encrypted on every drive and extra place from now on (%lld files, %ls)."), w ? w : L"",
                 (long long)fm.files, size);
        log_append(line);
    }
    free(w);
    load_marks();
    rebuild_tree();
    show_detail();
}

static void toggle_encrypt_selected(void)
{
    node_ref *r = item_ref(TreeView_GetSelection(g_tree));
    if (!r) {
        MessageBoxW(g_main, T(L"Select a backup root, a folder or a file on the Primary Database tab first."), APP_NAME, MB_ICONINFORMATION);
        return;
    }
    const char *rel = r->rel ? r->rel : "";
    int en = bd_encrypt_marked(g_cat, r->source_id, rel);
    if (en == 2) {
        MessageBoxW(g_main, T(L"It is already encrypted, with a folder above it. Change that folder on the Encryption tab."), APP_NAME,
                    MB_ICONINFORMATION);
        return;
    }
    int64_t sid = r->source_id;
    char *keep = xstrdup(rel);
    if (keep) set_encrypt(sid, keep, en == 0);
    free(keep);
}

static void encrypt_picked_folder(void)
{
    char *folder = pick_folder(g_main, T(L"Choose a folder in one of your backup roots. Its files are encrypted on every drive and extra "
                                       L"place they are copied to."));
    if (!folder) return;
    int64_t sid = 0;
    char *rel = NULL;
    if (!path_to_source(folder, &sid, &rel))
        MessageBoxW(g_main, T(L"That folder is not in any of your backup roots, so BRODALF does not back it up. Choose a folder inside "
                            L"a backup root, or add it as a backup root first."), APP_NAME, MB_ICONINFORMATION);
    else if (bd_encrypt_marked(g_cat, sid, rel))
        MessageBoxW(g_main, T(L"It is already encrypted, or a folder above it is."), APP_NAME, MB_ICONINFORMATION);
    else
        set_encrypt(sid, rel, 1);
    free(rel);
    free(folder);
}

/* IDD_PLACE: a new extra place (mode 0), or the size limit of one (mode 1). */
static int g_place_mode, g_place_encrypt, g_place_hide;
static wchar_t g_place_text[1600], g_place_label[256];
static int64_t g_place_limit;

/* Says what and where it is: two words or more, 10 characters or more. */
static int good_description(const wchar_t *s)
{
    int words = 0, in_word = 0, chars = 0;
    for (; *s; s++) {
        if (iswspace(*s)) { in_word = 0; continue; }
        chars++;
        if (!in_word) { words++; in_word = 1; }
    }
    return words >= 2 && chars >= 9;
}

/* "2.5" GB as bytes; "" no limit (0). 0 if it is not a number. */
static int parse_gb(const wchar_t *s, int64_t *out)
{
    wchar_t buf[64];
    while (*s == L' ') s++;
    if (!*s) { *out = 0; return 1; }
    wcsncpy(buf, s, 63);
    buf[63] = 0;
    for (wchar_t *c = buf; *c; c++) if (*c == L',') *c = L'.';
    wchar_t *end = NULL;
    double v = wcstod(buf, &end);
    if (end == buf) return 0;
    while (*end == L' ') end++;
    if (*end && _wcsicmp(end, L"GB") != 0) return 0;
    if (!(v >= 0) || v > 1e7) return 0;
    *out = (int64_t)(v * 1073741824.0);
    return 1;
}

static INT_PTR CALLBACK place_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    INT_PTR themed;
    if (theme_dialog(dlg, msg, wp, lp, &themed)) return themed;
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG:
        SetDlgItemTextW(dlg, IDC_PLACE_TEXT, g_place_text);
        SetDlgItemTextW(dlg, IDC_PLACE_LABEL, g_place_label);
        if (g_place_limit > 0) {
            wchar_t gb[32];
            swprintf(gb, 32, L"%.1f", (double)g_place_limit / 1073741824.0);
            SetDlgItemTextW(dlg, IDC_PLACE_LIMIT, gb);
        }
        if (g_place_mode == 1) {
            SetWindowTextW(dlg, T(L"Size limit of an extra place"));
            SendDlgItemMessageW(dlg, IDC_PLACE_LABEL, EM_SETREADONLY, TRUE, 0);
            ShowWindow(GetDlgItem(dlg, IDC_PLACE_ENCRYPT), SW_HIDE);
            ShowWindow(GetDlgItem(dlg, IDC_PLACE_HIDE), SW_HIDE);
            SetDlgItemTextW(dlg, IDOK, T(L"Save"));
            SetFocus(GetDlgItem(dlg, IDC_PLACE_LIMIT));
            SendDlgItemMessageW(dlg, IDC_PLACE_LIMIT, EM_SETSEL, 0, -1);
        } else {
            CheckDlgButton(dlg, IDC_PLACE_ENCRYPT, g_place_encrypt ? BST_CHECKED : BST_UNCHECKED);
            EnableWindow(GetDlgItem(dlg, IDC_PLACE_HIDE), g_place_encrypt); /* hiding names needs encryption */
            SetFocus(GetDlgItem(dlg, IDC_PLACE_LABEL));
            SendDlgItemMessageW(dlg, IDC_PLACE_LABEL, EM_SETSEL, 0, -1);
        }
        return FALSE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_PLACE_ENCRYPT) {
            int on = IsDlgButtonChecked(dlg, IDC_PLACE_ENCRYPT) == BST_CHECKED;
            EnableWindow(GetDlgItem(dlg, IDC_PLACE_HIDE), on);
            if (!on) CheckDlgButton(dlg, IDC_PLACE_HIDE, BST_UNCHECKED);
            return TRUE;
        }
        if (LOWORD(wp) == IDOK) {
            wchar_t label[256], limit[64];
            int64_t bytes = 0;
            GetDlgItemTextW(dlg, IDC_PLACE_LABEL, label, 256);
            GetDlgItemTextW(dlg, IDC_PLACE_LIMIT, limit, 64);
            if (g_place_mode == 0 && !good_description(label)) {
                MessageBoxW(dlg, T(L"Describe it in at least two words, saying what it is and where it lives, like \"Dropbox folder "
                                 L"on the laptop\" or \"NAS share in the office\". It is how you tell your extra places apart later."),
                            APP_NAME, MB_ICONINFORMATION);
                SetFocus(GetDlgItem(dlg, IDC_PLACE_LABEL));
                return TRUE;
            }
            if (!parse_gb(limit, &bytes)) {
                MessageBoxW(dlg, T(L"Type the most it may take in GB, like 2 or 15.5, or leave it empty for no limit."), APP_NAME,
                            MB_ICONINFORMATION);
                SetFocus(GetDlgItem(dlg, IDC_PLACE_LIMIT));
                return TRUE;
            }
            wcsncpy(g_place_label, label, 255);
            g_place_label[255] = 0;
            g_place_limit = bytes;
            g_place_encrypt = g_place_mode == 0 && IsDlgButtonChecked(dlg, IDC_PLACE_ENCRYPT) == BST_CHECKED;
            g_place_hide = g_place_encrypt && IsDlgButtonChecked(dlg, IDC_PLACE_HIDE) == BST_CHECKED;
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

/* Add a folder as an extra place for starred files: a second, secondary
 * backup that never counts toward the full copies. */
static void cmd_add_place(void)
{
    char *folder = pick_folder(g_main, T(L"Choose the folder for a second copy of your starred files: your Dropbox, OneDrive or Google "
                                       L"Drive folder on this PC, a network share, or a folder on another disk."));
    if (!folder) return;
    if (is_brodalf_drive(folder)) {
        MessageBoxW(g_main, T(L"That folder is already one of your backup drives or extra places."), APP_NAME, MB_ICONINFORMATION);
        free(folder);
        return;
    }
    wchar_t *w = widen(folder), pc[MAX_COMPUTERNAME_LENGTH + 1] = L"";
    DWORD pcn = MAX_COMPUTERNAME_LENGTH + 1;
    if (!GetComputerNameW(pc, &pcn)) wcscpy(pc, T(L"this PC"));
    const wchar_t *slash = w ? wcsrchr(w, L'\\') : NULL;
    swprintf(g_place_label, 256, T(L"%ls folder on %ls"), slash && slash[1] ? slash + 1 : T(L"Backup"), pc);
    swprintf(g_place_text, 1600,
             T(L"Your starred files get a second, secondary copy in %ls. It is not one of your backup drives: its copies never count "
             L"toward the full copies you want. BRODALF only copies files into it, in a BRODALF folder; that folder's own app does any "
             L"syncing, and BRODALF talks to no cloud service. Nothing is copied until you back up to it."),
             w ? w : L"");
    free(w);
    g_place_mode = 0;
    g_place_limit = 0;
    g_place_encrypt = g_place_hide = 0;
    if (DialogBoxW(g_inst, MAKEINTRESOURCEW(IDD_PLACE), g_main, place_proc) != IDOK) {
        free(folder);
        return;
    }
    unsigned flags = 0;
    if (g_place_encrypt) {
        int ok = bd_catalog_has_passphrase(g_cat) ? unlock_ui(T(L"Enter the passphrase to encrypt the copies in the extra place."))
                                                  : set_passphrase_ui();
        if (!ok) { free(folder); return; }
        flags = BD_MEDIA_ENCRYPTED | (g_place_hide ? BD_MEDIA_HIDE_NAMES : 0);
    }
    char *label = narrow(g_place_label);
    int64_t id = 0;
    bd_status s = label ? bd_place_add(g_cat, folder, label, g_place_limit, flags, &id) : BD_ERR_NOMEM;
    free(label);
    int ok = save_or_say(s, T(L"Could not add the extra place."));
    if (ok) {
        wchar_t *wf = widen(folder), line[1400];
        swprintf(line, 1400, T(L"Extra place added: %ls, at %ls. Only starred files go there."), g_place_label, wf ? wf : L"");
        free(wf);
        log_append(line);
    }
    free(folder);
    update_drives_label();
    if (!ok) return;
    int64_t files = 0, bytes = 0;
    bd_starred_totals(g_cat, &files, &bytes);
    if (files > 0) {
        wchar_t size[64], q[600];
        format_bytes(bytes, size, 64);
        swprintf(q, 600, TN(files, L"Back up your %lld starred file (%ls) to it now?", L"Back up your %lld starred files (%ls) to it now?"),
                 (long long)files, size);
        if (ask_verbs(g_main, q, MB_ICONQUESTION | MB_YESNO, T(L"Back up now"), T(L"Not now")) == IDYES) drive_action(id, 0);
    } else
        MessageBoxW(g_main, T(L"Nothing is starred yet. Star files or folders on the Primary Database tab (Star it, or right-click), "
                            L"then back up to the extra place."), APP_NAME, MB_ICONINFORMATION);
}

static void cmd_place_limit(int64_t id)
{
    drive_list dl = load_drives();
    const drive_snap *d = find_drive(&dl, id);
    if (!d) { free(dl.items); return; }
    wchar_t *l = widen(d->label);
    swprintf(g_place_label, 256, L"%ls", l ? l : L"");
    free(l);
    g_place_limit = d->limit_bytes;
    free(dl.items);
    swprintf(g_place_text, 1600, T(L"How much of your starred files may go to this extra place in all, so a small free account does not "
                                 L"fill up. Files that would go over it are not copied there; your drives still get them. Copies "
                                 L"already there stay."));
    g_place_mode = 1;
    if (DialogBoxW(g_inst, MAKEINTRESOURCEW(IDD_PLACE), g_main, place_proc) != IDOK) return;
    if (save_or_say(bd_place_set_limit(g_cat, id, g_place_limit), T(L"Could not change the size limit."))) {
        wchar_t line[400], size[64];
        format_bytes(g_place_limit, size, 64);
        if (g_place_limit > 0) swprintf(line, 400, T(L"%ls may now take at most %ls of starred files."), g_place_label, size);
        else swprintf(line, 400, T(L"%ls now takes every starred file (no limit)."), g_place_label);
        log_append(line);
    }
}

static void cmd_backup_all(int offer_scan);

/* Back up...: pick a drive, every plugged-in drive, or another one. */
static void cmd_backup_at(HWND anchor)
{
    int64_t id = choose_drive(anchor, 2);
    if (id == -2) { cmd_backup_all(1); return; }
    if (id < 0) return;
    if (id == 0) { add_disk(); return; }
    drive_action(id, 0);
}

/* Back up to every plugged-in drive and extra place, one after another or
 * all at once (the parallel_copies option). */
static void cmd_backup_all(int offer_scan)
{
    drive_list dl = load_drives();
    int64_t ids[16];
    int n = 0, places = 0, encrypted = 0, left_out = 0;
    for (int i = 0; i < dl.n; i++) {
        const drive_snap *d = &dl.items[i];
        if (strcmp(d->kind, "drive") != 0 || !d->connected || d->lost_ms) continue;
        if (n == 16) { left_out++; continue; }
        ids[n++] = d->id;
        if (d->secondary) places++;
        if (d->encrypted) encrypted = 1;
    }
    free(dl.items);
    if (!n) {
        no_drive_message();
        return;
    }
    bd_encrypt_stats es;
    memset(&es, 0, sizeof(es));
    bd_encrypt_stats_get(g_cat, &es);
    if ((encrypted || es.files) && !unlock_ui(T(L"Some of what this backs up is encrypted. Enter the passphrase to back up."))) return;
    if (n == 1) {
        job *j = new_job(JOB_BACKUP);
        if (!j) return;
        j->media_id = ids[0];
        start_backup(j, offer_scan);
        return;
    }
    /* The same two questions a backup to one drive asks. */
    int unscanned = 0;
    int64_t first_changed = 0;
    wtext changed = {NULL, 0, 0};
    for (int i = 0; i < n; i++) {
        bd_backup_preview_info pv;
        memset(&pv, 0, sizeof(pv));
        if (bd_backup_preview(g_cat, ids[i], 0, &pv) != BD_OK) continue;
        if (!pv.last_scan_ms) unscanned = 1;
        if (pv.mass_change) {
            wchar_t name[600];
            media_name(ids[i], name, 600);
            if (!first_changed) first_changed = ids[i];
            if (pv.held_deleted > 0)
                wadd(&changed, TN(pv.held_deleted, L"    %ls: of %lld files, %lld changed and %lld was deleted\n",
                                  L"    %ls: of %lld files, %lld changed and %lld were deleted\n"),
                     name, (long long)pv.files_held, (long long)(pv.held_changed - pv.held_deleted), (long long)pv.held_deleted);
            else
                wadd(&changed, T(L"    %ls: %lld of %lld files changed\n"), name, (long long)pv.held_changed, (long long)pv.files_held);
        }
    }
    if (unscanned && offer_scan) {
        int r = MessageBoxW(g_main, T(L"Your backup roots have not been scanned yet, so BRODALF does not know of any files to back up.\n\n"
                                    L"Scan first, then back up to every plugged-in drive?"),
                            APP_NAME, MB_ICONQUESTION | MB_YESNOCANCEL);
        if (r == IDCANCEL) { free(changed.p); return; }
        if (r == IDYES) {
            job *s = new_job(JOB_SCAN);
            if (s) {
                s->then_all = 1;
                enqueue(s);
            }
            free(changed.p);
            return;
        }
    }
    /* The same question a backup to one drive asks, with the same way to
     * look at the changed files before anything is copied. */
    while (changed.p) {
        wtext q = {NULL, 0, 0};
        wadd(&q, T(L"Since their copies were made:\n\n%ls\nThat many at once can mean ransomware encrypted your files. Backing up "
                 L"overwrites nothing: the copies already on the drives stay as they are and the changed files go alongside them. "
                 L"If you did not change these files, look at them first, and restore from a drive instead."),
             changed.p);
        TASKDIALOG_BUTTON btn[2];
        btn[0].nButtonID = 622;
        btn[0].pszButtonText = T(L"Show the changed files");
        btn[1].nButtonID = 620;
        btn[1].pszButtonText = T(L"Back up anyway\nThe copies already on the drives are kept.");
        TASKDIALOGCONFIG tc;
        memset(&tc, 0, sizeof(tc));
        tc.cbSize = sizeof(tc);
        tc.hwndParent = g_main;
        tc.hInstance = g_inst;
        tc.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
        tc.dwCommonButtons = TDCBF_CANCEL_BUTTON;
        tc.pszWindowTitle = APP_NAME;
        tc.pszMainIcon = TD_WARNING_ICON;
        tc.pszMainInstruction = T(L"Many files changed at once. Back up to every plugged-in drive?");
        tc.pszContent = q.p ? q.p : L"";
        tc.cButtons = 2;
        tc.pButtons = btn;
        tc.nDefaultButton = 622;
        int pressed = IDCANCEL;
        if (FAILED(TaskDialogIndirect(&tc, &pressed, NULL, NULL))) pressed = IDCANCEL;
        free(q.p);
        if (pressed == 622) {
            if (show_changed_as(first_changed, T(L"Back up to every plugged-in drive"))) {
                free(changed.p);
                break;
            }
            continue;
        }
        free(changed.p);
        if (pressed != 620) return;
        break;
    }
    job *j = new_job(JOB_BACKUP);
    if (!j) return;
    memcpy(j->many, ids, sizeof(int64_t) * (size_t)n);
    j->n_many = n;
    j->many_places = places;
    if (left_out) {
        wchar_t line[200];
        swprintf(line, 200, TN(left_out, L"%d more plugged-in drive is left for the next backup: at most 16 at a time.",
                               L"%d more plugged-in drives are left for the next backup: at most 16 at a time."), left_out);
        log_append(line);
    }
    enqueue(j);
}

/* A button, check box or list on a tab page. */
static void page_command(int id, int code)
{
    int r = -1;
    switch (id) {
    case ID_DB_ADD: cmd_add_folder(); break;
    case ID_DB_STAR: toggle_star_selected(); break;
    case ID_DB_ENCRYPT: toggle_encrypt_selected(); break;
    case ID_DB_BITROT: cmd_bitrot(); break;
    case ID_DB_RELOCATE: cmd_relocate(); break;
    case ID_DB_SKIP: cmd_skip(0, NULL); break;
    case ID_DB_DELETED: cmd_deleted(); break;
    case ID_DB_DAMAGED: cmd_table(TABLE_DAMAGED); break;
    case ID_DB_IN_USE: show_in_use(1, NULL); break;
    case ID_DB_LARGEST: cmd_table(TABLE_LARGEST); break;
    case ID_DB_EXPORT: cmd_export(); break;

    case ID_NEXT_FOR:
        if (code == CBN_SELCHANGE) {
            int sel = (int)SendMessageW(CTL(ID_NEXT_FOR), CB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < g_next_nopts) g_next_media = g_next_opts[sel].id;
        }
        break;
    case ID_NEXT_BACKUP:
        if (g_next_media) drive_action(g_next_media, 0);
        else cmd_backup_at(CTL(ID_NEXT_BACKUP));
        break;
    case ID_NEXT_ALL:
    case ID_LOCAL_ALL: cmd_backup_all(1); break;
    case ID_NEXT_CHANGED: if (g_next_media && show_changed(g_next_media)) back_up_changes(g_next_media); break;
    case ID_NEXT_RISK:
    case ID_LOCAL_RISK: cmd_risk(); break;
    case ID_NEXT_REFRESH: break; /* every page is filled again after a command */

    case ID_LOCAL_ADD: add_disk(); break;
    case ID_LOCAL_STATUS: cmd_table(TABLE_DRIVES); break;
    case ID_LOCAL_PARALLEL: settings_command(ID_MENU_PARALLEL); break;
    case ID_LOCAL_BACKUP:
    case ID_LOCAL_CHECK:
    case ID_LOCAL_VERIFY:
    case ID_LOCAL_OPEN:
    case ID_LOCAL_COMPRESS:
    case ID_LOCAL_CHANGED:
    case ID_LOCAL_EDIT:
    case ID_LOCAL_LOST: {
        r = lv_selected(ID_LOCAL_LIST);
        if (r < 0 || r >= g_local_n) break;
        int64_t mid = g_local_rows[r].id;
        if (id == ID_LOCAL_BACKUP) drive_action(mid, 0);
        else if (id == ID_LOCAL_CHECK) drive_action(mid, 2);
        else if (id == ID_LOCAL_VERIFY) drive_action(mid, 1);
        else if (id == ID_LOCAL_OPEN) drive_action(mid, 6);
        else if (id == ID_LOCAL_COMPRESS) compress_drive_ui(mid);
        else if (id == ID_LOCAL_CHANGED) {
            if (show_changed(mid)) back_up_changes(mid);
        }
        else if (id == ID_LOCAL_EDIT) edit_drive(mid);
        else drive_action(mid, 5);
        break;
    }

    case ID_STAR_ADD: toggle_star_selected(); break;
    case ID_STAR_REMOVE:
    case ID_STAR_SHOW: {
        r = lv_selected(ID_STAR_LIST);
        if (r < 0 || r >= g_star_nrows) break;
        int64_t sid = g_star_rows[r].source_id;
        char *rel = xstrdup(g_star_rows[r].rel);
        if (rel) {
            if (id == ID_STAR_REMOVE) set_star(sid, rel, 0);
            else show_in_db(sid, rel);
        }
        free(rel);
        break;
    }
    case ID_PLACE_ADD: cmd_add_place(); break;
    case ID_PLACE_BACKUP:
    case ID_PLACE_LIMIT:
    case ID_PLACE_OPEN:
    case ID_PLACE_EDIT:
    case ID_PLACE_LOST: {
        r = lv_selected(ID_PLACE_LIST);
        if (r < 0 || r >= g_place_n) break;
        int64_t pid = g_place_rows[r].id;
        if (id == ID_PLACE_BACKUP) drive_action(pid, 0);
        else if (id == ID_PLACE_LIMIT) cmd_place_limit(pid);
        else if (id == ID_PLACE_OPEN) drive_action(pid, 6);
        else if (id == ID_PLACE_EDIT) edit_drive(pid);
        else drive_action(pid, 5);
        break;
    }

    case ID_ENC_PASS: set_passphrase_ui(); break;
    case ID_ENC_LOCK: settings_command(ID_MENU_LOCK); break;
    case ID_ENC_KEY: cmd_sheet(1); break;
    case ID_ENC_SHEET: cmd_sheet(0); break;
    case ID_ENC_DRIVE:
        g_label_encrypt_default = 1;
        add_disk();
        g_label_encrypt_default = 0;
        break;
    case ID_ENC_CATALOG: settings_command(ID_MENU_ENCRYPT_CATALOG); break;
    case ID_ENC_ADD: toggle_encrypt_selected(); break;
    case ID_ENC_PICK: encrypt_picked_folder(); break;
    case ID_ENC_REMOVE:
    case ID_ENC_SHOW: {
        r = lv_selected(ID_ENC_LIST);
        if (r < 0 || r >= g_enc_nrows) break;
        int64_t sid = g_enc_rows[r].source_id;
        char *rel = xstrdup(g_enc_rows[r].rel);
        if (rel) {
            if (id == ID_ENC_REMOVE) set_encrypt(sid, rel, 0);
            else show_in_db(sid, rel);
        }
        free(rel);
        break;
    }

    case ID_REST_SEL: cmd_restore(); break;
    case ID_REST_ALL:
        if (g_pending.active &&
            ask_yn(g_main, T(L"A restore is waiting for more drives. Restoring everything forgets it (what it restored stays).\n\n"
                                L"Restore everything instead?"), APP_NAME, MB_ICONQUESTION | MB_YESNO) != IDYES)
            break;
        restore_selection(1, 0);
        break;
    case ID_REST_DELETED: cmd_deleted(); break;
    case ID_REST_CONTINUE: continue_restore(); break;
    case ID_REST_TABLE: cmd_table(TABLE_CONFLICTS); break;
    case ID_REST_OPEN:
        r = lv_selected(ID_REST_LIST);
        if (r >= 0 && r < g_n_conflicts && g_conflicts[r].dest) show_in_explorer(g_conflicts[r].dest);
        break;
    }
}

/* Notifications from the tab control and the pages' lists. 1 if handled. */
static int page_notify(NMHDR *nh)
{
    int id = (int)nh->idFrom;
    if (nh->hwndFrom == g_tabs) {
        if (nh->code == TCN_SELCHANGE) show_tab(TabCtrl_GetCurSel(g_tabs));
        return 1;
    }
    if (id < ID_PAGE_FIRST || id >= ID_PAGE_LAST || nh->hwndFrom != CTL(id)) return 0;
    if (nh->code == LVN_ITEMCHANGED) {
        NMLISTVIEW *lv = (NMLISTVIEW *)nh;
        if (!g_filling && !g_busy && g_cat && (lv->uChanged & LVIF_STATE) && ((lv->uNewState ^ lv->uOldState) & LVIS_SELECTED)) {
            if (id == ID_LOCAL_LIST) local_detail();
            page_buttons_state();
        }
        return 1;
    }
    if (nh->code != NM_DBLCLK || g_busy || g_ui_depth || !g_cat) return 1;
    int r = lv_selected(id);
    g_ui_depth++;
    if (id == ID_NEXT_LIST && r >= 0 && r < g_next_nrows) {
        int64_t sid = g_next_rows[r].source_id;
        char *rel = xstrdup(g_next_rows[r].rel);
        if (rel) show_in_db(sid, rel);
        free(rel);
    } else if ((id == ID_STAR_LIST && r >= 0 && r < g_star_nrows) || (id == ID_ENC_LIST && r >= 0 && r < g_enc_nrows)) {
        const mark_key *k = id == ID_STAR_LIST ? &g_star_rows[r] : &g_enc_rows[r];
        int64_t sid = k->source_id;
        char *rel = xstrdup(k->rel);
        if (rel) show_in_db(sid, rel);
        free(rel);
    } else if (id == ID_LOCAL_LIST && r >= 0 && r < g_local_n) {
        edit_drive(g_local_rows[r].id);
    } else if (id == ID_PLACE_LIST && r >= 0 && r < g_place_n) {
        edit_drive(g_place_rows[r].id);
    } else if (id == ID_ENC_STATE && r >= 0 && r < g_enc_state_n) {
        if (g_enc_state_drive[r]) edit_drive(g_enc_state_drive[r]);
        else if (g_enc_state_cmd[r] == ID_ENC_PASS) set_passphrase_ui();
        else if (g_enc_state_cmd[r] == ID_ENC_DRIVE) {
            g_label_encrypt_default = 1;
            add_disk();
            g_label_encrypt_default = 0;
        }
    } else if (id == ID_REST_LIST && r >= 0 && r < g_n_conflicts && g_conflicts[r].dest) {
        show_in_explorer(g_conflicts[r].dest);
    }
    g_ui_depth--;
    start_next_job();
    refresh_pages();
    return 1;
}

/* ---- Layout and window procedure ---------------------------------------- */

/* ---- The status header, the warnings and the job panel ------------------
 *
 * Painted by hand, from what update_drives_label and the jobs read. The
 * header says in one sentence whether the files are safe and what to do
 * next, shows each full copy as a bar made of the drives it is on, and a
 * card for every drive and extra place. Under it the warnings, each with
 * its own buttons; under the tabs the job panel: what runs now, or how the
 * last job ended. */

static const wchar_t *ui_locale(void) { return g_langs[g_lang].code; }

static int64_t day_number(const SYSTEMTIME *st)
{
    SYSTEMTIME d = *st;
    d.wHour = d.wMinute = d.wSecond = d.wMilliseconds = 0;
    FILETIME ft;
    if (!SystemTimeToFileTime(&d, &ft)) return 0;
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (int64_t)(u.QuadPart / 864000000000ULL);
}

/* "today at 14:02", "yesterday at 09:10", "25.11.2026 at 14:02", in the
 * way the language in use writes dates and times. */
static void friendly_time(int64_t ms, wchar_t *out, size_t cap)
{
    if (ms <= 0) {
        swprintf(out, cap, L"%ls", T(L"never"));
        return;
    }
    ULONGLONG t = (ULONGLONG)ms * 10000ULL + 116444736000000000ULL;
    FILETIME ft = {(DWORD)t, (DWORD)(t >> 32)}, local;
    SYSTEMTIME st, now;
    FileTimeToLocalFileTime(&ft, &local);
    FileTimeToSystemTime(&local, &st);
    GetLocalTime(&now);
    wchar_t tm[64] = L"", dt[128] = L"";
    if (!GetTimeFormatEx(fmt_locale(), TIME_NOSECONDS, &st, NULL, tm, 64)) swprintf(tm, 64, L"%02d:%02d", st.wHour, st.wMinute);
    int64_t days = day_number(&now) - day_number(&st);
    if (days == 0) swprintf(out, cap, T(L"today at %ls"), tm);
    else if (days == 1) swprintf(out, cap, T(L"yesterday at %ls"), tm);
    else {
        if (!GetDateFormatEx(fmt_locale(), DATE_SHORTDATE, &st, NULL, dt, 128, NULL))
            swprintf(dt, 128, L"%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
        swprintf(out, cap, T(L"%ls at %ls"), dt, tm);
    }
}

/* Add a line to a tooltip's text. */
static void tip_line(wchar_t *buf, size_t cap, const wchar_t *fmt, ...)
{
    size_t len = wcslen(buf);
    if (len && len + 2 < cap) {
        wcscat(buf, L"\r\n");
        len += 2;
    }
    if (len + 1 >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    vswprintf(buf + len, cap - len, fmt, ap);
    va_end(ap);
    buf[cap - 1] = 0;
}

/* ---- What the header shows ---------------------------------------------- */

static void card_state(hd_card *c, const drive_snap *d, const drive_fig *g)
{
    c->tone = TONE_NEUTRAL;
    if (d->lost_ms) {
        c->tone = TONE_BAD;
        swprintf(c->chip, 120, L"%ls", strcmp(d->lost_why, "broken") == 0 ? T(L"Broken: not counted") : T(L"Lost: not counted"));
        return;
    }
    if (d->secondary) {
        if (!d->connected) {
            swprintf(c->chip, 120, L"%ls", T(L"Not found now"));
        } else if (g && g->has_ps) {
            int64_t wait = g->ps.waiting_files - g->ps.over_limit_files;
            if (wait > 0) {
                c->tone = TONE_WARN;
                swprintf(c->chip, 120, TN(wait, L"%lld starred file to copy", L"%lld starred files to copy"), (long long)wait);
            } else {
                c->tone = TONE_SAFE;
                swprintf(c->chip, 120, L"%ls", T(L"Up to date"));
            }
        } else {
            swprintf(c->chip, 120, L"%ls", T(L"Found"));
        }
        return;
    }
    if (replace_advice(d)) {
        c->tone = TONE_BAD;
        swprintf(c->chip, 120, L"%ls", T(L"Health warning"));
        return;
    }
    if (g && g->has_ms && g->ms.copies_bad > 0) {
        c->tone = TONE_BAD;
        swprintf(c->chip, 120, TN(g->ms.copies_bad, L"%lld bad copy", L"%lld bad copies"), (long long)g->ms.copies_bad);
        return;
    }
    if (!d->connected) {
        swprintf(c->chip, 120, L"%ls", T(L"Not plugged in"));
        return;
    }
    if (g && g->has_pv) {
        if (g->pv.files_to_copy > 0) {
            c->tone = TONE_WARN;
            swprintf(c->chip, 120, TN(g->pv.files_to_copy, L"%lld file to back up", L"%lld files to back up"), (long long)g->pv.files_to_copy);
        } else if (d->check_due) {
            c->tone = TONE_WARN;
            swprintf(c->chip, 120, L"%ls", T(L"Due for a check"));
        } else {
            c->tone = TONE_SAFE;
            swprintf(c->chip, 120, L"%ls", T(L"Up to date"));
        }
        return;
    }
    swprintf(c->chip, 120, L"%ls", T(L"Plugged in"));
}

static void card_tip(hd_card *c, const drive_snap *d, const drive_fig *g)
{
    wchar_t a[96], b[96];
    c->tip[0] = 0;
    tip_line(c->tip, 900, L"%ls", c->name);
    if (c->where[0]) tip_line(c->tip, 900, T(L"Kept in: %ls"), c->where);
    if (d->lost_ms)
        tip_line(c->tip, 900, L"%ls", strcmp(d->lost_why, "broken") == 0 ? T(L"Marked broken: its copies do not count.")
                                                                          : T(L"Marked lost: its copies do not count."));
    if (d->connected) tip_line(c->tip, 900, d->secondary ? T(L"Found at %ls") : T(L"Plugged in at %ls"), c->path);
    else {
        friendly_time(d->last_seen_ms, a, 96);
        tip_line(c->tip, 900, d->secondary ? T(L"Not found now. Last found %ls.") : T(L"Not plugged in. Last seen %ls."), a);
    }
    if (d->total > 0) {
        format_bytes(d->freeb, a, 96);
        format_bytes(d->total, b, 96);
        tip_line(c->tip, 900, d->connected ? T(L"%ls free of %ls") : T(L"%ls free of %ls when last seen"), a, b);
    }
    format_bytes(d->bytes_held, a, 96);
    tip_line(c->tip, 900, T(L"Copies on it: %ls"), a);
    if (g && g->has_ms) {
        friendly_time(g->ms.last_backup_end_ms, a, 96);
        friendly_time(g->ms.last_check_ms, b, 96);
        tip_line(c->tip, 900, T(L"Last backup: %ls. Last check: %ls."), a, b);
    }
    tip_line(c->tip, 900, L"%ls", c->chip);
    tip_line(c->tip, 900, L"%ls", d->secondary ? T(L"Click to see it on Starred files and extra places. Right-click for what you can do with it.")
                                               : T(L"Click to see it on Local backups. Right-click for what you can do with it."));
}

/* The full-copy bars, from the figures (also during a backup). */
static void header_bars_refresh(void)
{
    g_nhbars = 0;
    if (!g_cat || !g_figs || !g_figs->has_sets) return;
    int n = g_figs->n_sets < MAX_SET_BARS ? g_figs->n_sets : MAX_SET_BARS;
    for (int i = 0; i < n; i++) {
        hd_bar *b = &g_hbars[g_nhbars++];
        const bd_set_progress *s = &g_figs->sets[i];
        memset(b, 0, sizeof(*b));
        b->set = s->set;
        b->files_done = s->files_done;
        b->files_total = s->files_total;
        b->bytes_done = s->bytes_done;
        b->bytes_total = s->bytes_total;
        for (int k = 0; k < g_figs->nsd && b->nseg < 16; k++)
            if (g_figs->sd[k].set == s->set && g_figs->sd[k].files > 0) {
                b->seg[b->nseg].media_id = g_figs->sd[k].media_id;
                b->seg[b->nseg].files = g_figs->sd[k].files;
                b->nseg++;
            }
    }
}

static void header_refresh(void)
{
    g_ncards = 0;
    g_last_backup_ms = 0;
    figures *f = g_cat ? g_figs : NULL;
    int primaries = 0, plugged = 0;
    int64_t best_id = 0, best_files = 0, best_bytes = 0;
    wchar_t best_name[128] = L"";
    if (f) {
        for (int pass = 0; pass < 2; pass++)
            for (int i = 0; i < f->dl.n; i++) {
                const drive_snap *d = &f->dl.items[i];
                if (strcmp(d->kind, "drive") != 0) continue;
                if ((pass == 0) != (d->secondary == 0)) continue;
                const drive_fig *g = &f->d[i];
                if (g->has_ms && g->ms.last_backup_end_ms > g_last_backup_ms) g_last_backup_ms = g->ms.last_backup_end_ms;
                if (!d->secondary) {
                    primaries++;
                    if (d->connected && !d->lost_ms) plugged++;
                    if (d->connected && !d->lost_ms && g->has_pv && g->pv.files_to_copy > best_files) {
                        best_files = g->pv.files_to_copy;
                        best_bytes = g->pv.bytes_to_copy;
                        best_id = d->id;
                        wchar_t *w = widen(d->label);
                        swprintf(best_name, 128, L"%ls", w ? w : L"");
                        free(w);
                    }
                }
                if (g_ncards >= HD_MAX_CARDS) continue;
                hd_card *c = &g_cards[g_ncards++];
                memset(c, 0, sizeof(*c));
                c->id = d->id;
                c->connected = d->connected;
                c->lost = d->lost_ms != 0;
                c->secondary = d->secondary;
                c->total = d->total;
                c->freeb = d->freeb;
                c->held = d->bytes_held;
                wchar_t *w = widen(d->label);
                swprintf(c->name, 128, L"%ls", w ? w : L"");
                free(w);
                w = widen(d->location);
                swprintf(c->where, 200, L"%ls", w ? w : L"");
                free(w);
                w = widen(d->root);
                swprintf(c->path, 400, L"%ls", w ? w : L"");
                free(w);
                card_state(c, d, g);
                card_tip(c, d, g);
            }
        if (f->has_target) {
            g_target_copies = f->target.copies;
            g_target_places = f->target.places;
        }
    }
    header_bars_refresh();

    /* The verdict: one sentence, and the one thing to do next. */
    g_next_kind = NEXT_NONE;
    g_next_id = 0;
    g_verdict_sub[0] = 0;
    wchar_t btn[200] = L"";
    if (!g_cat) {
        g_verdict_tone = TONE_NEUTRAL;
        g_verdict_glyph = GLYPH_INFO;
        swprintf(g_verdict, 400, L"%ls", T(L"No database is open"));
        swprintf(g_verdict_sub, 600, L"%ls",
                 T(L"Open your database, or create a new one. To start again from a backup drive, use File > Recover the database "
                   L"from a backup drive."));
    } else if (!f) {
        g_verdict_tone = TONE_NEUTRAL;
        g_verdict_glyph = GLYPH_DOTS;
        swprintf(g_verdict, 400, L"%ls", T(L"Reading where your files stand..."));
    } else if (f->n_roots == 0) {
        g_verdict_tone = TONE_NEUTRAL;
        g_verdict_glyph = GLYPH_INFO;
        swprintf(g_verdict, 400, L"%ls", T(L"Nothing to back up yet"));
        swprintf(g_verdict_sub, 600, L"%ls",
                 T(L"Add a folder to back up. BRODALF keeps a list of every file in it and copies them to your backup drives."));
        swprintf(btn, 200, L"%ls", T(L"Add a folder to back up..."));
        g_next_kind = NEXT_ADD_ROOT;
    } else if (primaries == 0) {
        g_verdict_tone = TONE_WARN;
        g_verdict_glyph = GLYPH_BANG;
        swprintf(g_verdict, 400, L"%ls", T(L"No backup drive yet"));
        swprintf(g_verdict_sub, 600, L"%ls",
                 T(L"Your files have no copies yet. Set up a USB stick or another drive, and BRODALF copies every file to it."));
        swprintf(btn, 200, L"%ls", T(L"Set up a drive..."));
        g_next_kind = NEXT_ADD_DRIVE;
    } else if (!f->has_risk) {
        g_verdict_tone = TONE_NEUTRAL;
        g_verdict_glyph = GLYPH_INFO;
        swprintf(g_verdict, 400, L"%ls", T(L"BRODALF could not tell how many copies your files have"));
    } else if (f->risk.files_total == 0) {
        g_verdict_tone = TONE_NEUTRAL;
        g_verdict_glyph = GLYPH_INFO;
        swprintf(g_verdict, 400, L"%ls", T(L"No files found yet"));
        swprintf(g_verdict_sub, 600, L"%ls", T(L"The last scan found no files in your backup roots. Scan now looks again."));
        swprintf(btn, 200, L"%ls", T(L"Scan now"));
        g_next_kind = NEXT_SCAN;
    } else if (f->risk.files_at_risk == 0) {
        g_verdict_tone = TONE_SAFE;
        g_verdict_glyph = GLYPH_CHECK;
        if (g_target_places > 1)
            swprintf(g_verdict, 400, T(L"Every file has %d copies, in %d different places"), g_target_copies, g_target_places);
        else
            swprintf(g_verdict, 400, TN(g_target_copies, L"Every file has %d copy", L"Every file has %d copies"), g_target_copies);
        wchar_t lb[96], ls[96];
        friendly_time(g_last_backup_ms, lb, 96);
        friendly_time(f->last_scan_ms, ls, 96);
        swprintf(g_verdict_sub, 600, TN(f->risk.files_total, L"%lld file. Last backup %ls; last scan %ls.",
                                        L"%lld files. Last backup %ls; last scan %ls."),
                 (long long)f->risk.files_total, lb, ls);
    } else {
        int64_t n = f->risk.files_at_risk, none = f->risk.files_no_copy;
        g_verdict_tone = TONE_WARN;
        g_verdict_glyph = GLYPH_BANG;
        if (none >= n)
            swprintf(g_verdict, 400, TN(n, L"%lld file has no copy yet", L"%lld files have no copy yet"), (long long)n);
        else
            swprintf(g_verdict, 400, TN(n, L"%lld file does not have %d copies yet", L"%lld files do not have %d copies yet"), (long long)n,
                     g_target_copies);
        if (best_id) {
            wchar_t size[64];
            format_bytes(best_bytes, size, 64);
            swprintf(g_verdict_sub, 600, TN(best_files, L"A backup to %ls copies %lld file (%ls).", L"A backup to %ls copies %lld files (%ls)."),
                     best_name, (long long)best_files, size);
            swprintf(btn, 200, T(L"Back up to %ls"), best_name);
            g_next_kind = NEXT_BACKUP;
            g_next_id = best_id;
        } else {
            if (plugged == 0)
                swprintf(g_verdict_sub, 600, L"%ls",
                         TN(primaries, L"Your backup drive is not plugged in. Plug it in to back up.",
                            L"None of your backup drives are plugged in. Plug one in to back up."));
            else
                swprintf(g_verdict_sub, 600, L"%ls",
                         T(L"The drives plugged in already have them. Plug in another drive to give them their next copy."));
            swprintf(btn, 200, L"%ls", T(L"Which drive to plug in..."));
            g_next_kind = NEXT_RISK;
        }
        if (none > 0 && none < n) {
            size_t len = wcslen(g_verdict_sub);
            swprintf(g_verdict_sub + len, 600 - len, TN(none, L" %lld of them has no copy at all.", L" %lld of them have no copy at all."),
                     (long long)none);
        }
    }
    if (g_next_btn) {
        SetWindowTextW(g_next_btn, btn);
        ShowWindow(g_next_btn, g_next_kind != NEXT_NONE && !(g_busy && g_running_kind != JOB_REFRESH) ? SW_SHOWNA : SW_HIDE);
    }
    if (g_header) InvalidateRect(g_header, NULL, FALSE);
}

/* Is a job running that the header should show instead of the verdict? */
static int header_busy(void) { return g_busy && g_running_kind != JOB_REFRESH; }

/* ---- Header geometry ---------------------------------------------------- */

static RECT g_hd_icon, g_hd_title, g_hd_sub, g_hd_bars, g_hd_goal;
static int g_hd_label_w, g_hd_right_w, g_hd_bar_row, g_hd_compact;
static int g_card_tools; /* tooltips added for the cards */
static int g_card_hot = -1;

#define CARD_MORE HD_MAX_CARDS

static int font_h(HFONT f)
{
    HDC dc = GetDC(NULL);
    HGDIOBJ o = SelectObject(dc, f);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, o);
    ReleaseDC(NULL, dc);
    return tm.tmHeight;
}

/* The status bar holds quiet facts, each in its own part: the database,
 * the goal, the last scan, and whether a change is being saved. What is
 * running is in the job panel above it, so it is not said twice. */
static wchar_t g_sb_state[200];

static void status_state(const wchar_t *s)
{
    swprintf(g_sb_state, 200, L"%ls", s ? s : L"");
    status_bar_refresh();
}

static void status_bar_refresh(void)
{
    if (!g_status) return;
    wchar_t db[MAX_PATH * 2 + 64] = L"", goal[160] = L"", scan[160] = L"";
    if (g_cat) {
        const wchar_t *base = wcsrchr(g_cat_path, L'\\');
        swprintf(db, MAX_PATH * 2 + 64, T(L"Database: %ls"), base ? base + 1 : g_cat_path);
        if (g_target_places > 1) swprintf(goal, 160, T(L"Goal: %d copies in %d places"), g_target_copies, g_target_places);
        else swprintf(goal, 160, TN(g_target_copies, L"Goal: %d copy", L"Goal: %d copies"), g_target_copies);
        if (g_figs && g_figs->last_scan_ms) {
            wchar_t when[96];
            friendly_time(g_figs->last_scan_ms, when, 96);
            swprintf(scan, 160, T(L"Last scan %ls"), when);
        } else if (g_figs)
            swprintf(scan, 160, L"%ls", T(L"Not scanned yet"));
    } else
        swprintf(db, 64, L"%ls", T(L"No database open"));
    const wchar_t *txt[4] = {db, goal, scan, g_sb_state};
    /* Each part as wide as its text; the database has the rest. */
    int wpart[4] = {0, 0, 0, 0};
    HDC dc = GetDC(g_status);
    HFONT f = (HFONT)SendMessageW(g_status, WM_GETFONT, 0, 0);
    HGDIOBJ old = f ? SelectObject(dc, f) : NULL;
    for (int i = 1; i < 4; i++) {
        SIZE sz = {0, 0};
        if (txt[i][0]) GetTextExtentPoint32W(dc, txt[i], (int)wcslen(txt[i]), &sz);
        wpart[i] = txt[i][0] ? sz.cx + S(26) : 0;
    }
    if (old) SelectObject(dc, old);
    ReleaseDC(g_status, dc);
    RECT rc;
    GetClientRect(g_status, &rc);
    int parts[4];
    parts[3] = -1;
    parts[2] = rc.right - GetSystemMetrics(SM_CXVSCROLL) - wpart[3];
    parts[1] = parts[2] - wpart[2];
    parts[0] = parts[1] - wpart[1];
    if (parts[0] < S(120)) parts[0] = S(120);
    if (parts[1] < parts[0]) parts[1] = parts[0];
    if (parts[2] < parts[1]) parts[2] = parts[1];
    SendMessageW(g_status, SB_SETPARTS, 4, (LPARAM)parts);
    for (int i = 0; i < 4; i++) SendMessageW(g_status, SB_SETTEXTW, (WPARAM)i, (LPARAM)txt[i]);
}

/* A window that would reach past the screen's work area (under the
 * taskbar, say) is made to fit it. */
static void fit_to_work_area(HWND hwnd)
{
    MONITORINFO mi;
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) return;
    RECT wa = mi.rcWork, r;
    GetWindowRect(hwnd, &r);
    int w = r.right - r.left, h = r.bottom - r.top, x = r.left, y = r.top;
    if (w > wa.right - wa.left) w = wa.right - wa.left;
    if (h > wa.bottom - wa.top) h = wa.bottom - wa.top;
    if (x + w > wa.right) x = wa.right - w;
    if (y + h > wa.bottom) y = wa.bottom - h;
    if (x < wa.left) x = wa.left;
    if (y < wa.top) y = wa.top;
    if (x != r.left || y != r.top || w != r.right - r.left || h != r.bottom - r.top)
        SetWindowPos(hwnd, NULL, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

static void goal_text(wchar_t *out, size_t cap)
{
    if (g_target_places > 1)
        swprintf(out, cap, T(L"Goal: %d copies of every file, in %d different places."), g_target_copies, g_target_places);
    else
        swprintf(out, cap, TN(g_target_copies, L"Goal: %d copy of every file.", L"Goal: %d copies of every file."), g_target_copies);
}

/* Works out where everything in the header goes for width w; returns its
 * height. Also places its buttons. In a wide window the drive cards stand
 * in two columns on the right, beside the verdict and the bars, so the
 * header leaves the tabs more room. */
static int header_measure(int w, int place)
{
    int pad = S(14), x = pad, y = pad, gap = S(12);
    int th = font_h(g_font_title), line = font_h(g_font);
    int icon = S(32);
    int row_h = th + S(2) + line;
    if (row_h < icon) row_h = icon;
    int cw = w >= S(1400) ? S(260) : S(224), ch = S(64), cg = S(10);
    int side = g_cat && g_ncards > 0 && w >= S(1000);
    int side_w = side ? (g_ncards == 1 ? cw : cw * 2 + cg) : 0;
    int left_r = side ? w - pad - side_w - S(20) : w - pad;
    /* The buttons on the right of the verdict. */
    int bx = left_r;
    HWND btns[2] = {NULL, NULL};
    int nb = 0;
    if (!g_cat) {
        btns[nb++] = g_nodb_new;
        btns[nb++] = g_nodb_open;
    } else if (is_shown(g_next_btn)) {
        btns[nb++] = g_next_btn;
    }
    for (int i = 0; i < nb; i++) {
        int bw = button_width(btns[i]) + (btns[i] == g_next_btn ? S(16) : S(8));
        if (bw < S(120)) bw = S(120);
        bx -= bw;
        if (place) MoveWindow(btns[i], bx, y + (row_h - S(32)) / 2, bw, S(32), TRUE);
        bx -= S(8);
    }
    int text_x = x + icon + S(12);
    SetRect(&g_hd_icon, x, y + (row_h - icon) / 2, x + icon, y + (row_h - icon) / 2 + icon);
    SetRect(&g_hd_title, text_x, y, bx - S(8), y + th);
    SetRect(&g_hd_sub, text_x, y + th + S(2), bx - S(8), y + th + S(2) + line);
    y += row_h;
    if (!g_cat) return y + pad;

    /* The full-copy bars. */
    if (g_nhbars) {
        y += gap;
        HDC dc = GetDC(NULL);
        g_hd_label_w = 0;
        for (int i = 0; i < g_nhbars; i++) {
            wchar_t s[120];
            swprintf(s, 120, T(L"Full copy %d of %d"), g_hbars[i].set, g_nhbars);
            int tw = text_w(dc, g_font_semi, s);
            if (tw > g_hd_label_w) g_hd_label_w = tw;
        }
        ReleaseDC(NULL, dc);
        g_hd_right_w = S(260);
        if (g_hd_right_w > (left_r - x) / 3) g_hd_right_w = (left_r - x) / 3;
        g_hd_bar_row = S(24);
        SetRect(&g_hd_bars, x, y, left_r, y + g_hd_bar_row * g_nhbars);
        y += g_hd_bar_row * g_nhbars;
    } else
        SetRectEmpty(&g_hd_bars);
    /* The goal, with a link to change it. */
    if (!g_hd_compact || side) {
        y += S(4);
        SetRect(&g_hd_goal, x, y, left_r, y + line);
        y += line;
    } else
        SetRectEmpty(&g_hd_goal);

    /* The cards. */
    g_ncard_shown = 0;
    g_card_more = 0;
    if (side) {
        /* Beside the verdict: two to a row, two rows. */
        int per_row = g_ncards == 1 ? 1 : 2, room = per_row * 2, n = g_ncards, cx = w - pad - side_w;
        int shown = n <= room ? n : room - 1, used_rows = 0;
        for (int i = 0; i <= shown; i++) {
            if (i == shown && shown == n) break;
            int r = i / per_row, c = i % per_row;
            RECT *rc = &g_card_rc[i == shown ? CARD_MORE : i];
            SetRect(rc, cx + c * (cw + cg), pad + r * (ch + cg), cx + c * (cw + cg) + cw, pad + r * (ch + cg) + ch);
            if (r + 1 > used_rows) used_rows = r + 1;
        }
        g_ncard_shown = shown;
        g_card_more = n - shown;
        int bottom = pad + used_rows * ch + (used_rows - 1) * cg;
        if (bottom > y) y = bottom;
    } else if (g_ncards) {
        y += gap;
        int avail = w - pad * 2;
        int per_row = (avail + cg) / (cw + cg);
        if (per_row < 1) per_row = 1;
        /* Few enough for one row: each as wide as there is room for. */
        if (g_ncards <= per_row && g_ncards > 0) {
            int fit = (avail - cg * (g_ncards - 1)) / g_ncards;
            if (fit > S(320)) fit = S(320);
            if (fit > cw) cw = fit;
        }
        int rows = g_hd_compact ? 1 : 2, room = per_row * rows, n = g_ncards;
        int shown = n <= room ? n : room - 1;
        int used_rows = 0;
        for (int i = 0; i <= shown; i++) {
            if (i == shown && shown == n) break;
            int r = i / per_row, c = i % per_row;
            RECT *rc = &g_card_rc[i == shown ? CARD_MORE : i];
            SetRect(rc, x + c * (cw + cg), y + r * (ch + cg), x + c * (cw + cg) + cw, y + r * (ch + cg) + ch);
            if (r + 1 > used_rows) used_rows = r + 1;
        }
        g_ncard_shown = shown;
        g_card_more = n - shown;
        y += used_rows * ch + (used_rows - 1) * cg;
    }
    return y + pad;
}

static int header_height(int w)
{
    return header_measure(w, 0);
}

static void header_tooltips(void)
{
    if (!g_tip || !g_header) return;
    TOOLINFOW ti;
    for (int i = 0; i < g_card_tools; i++) {
        memset(&ti, 0, sizeof(ti));
        ti.cbSize = sizeof(ti);
        ti.hwnd = g_header;
        ti.uId = (UINT_PTR)(1000 + i);
        SendMessageW(g_tip, TTM_DELTOOLW, 0, (LPARAM)&ti);
    }
    g_card_tools = 0;
    for (int i = 0; i < g_ncard_shown; i++) {
        memset(&ti, 0, sizeof(ti));
        ti.cbSize = sizeof(ti);
        ti.uFlags = TTF_SUBCLASS;
        ti.hwnd = g_header;
        ti.uId = (UINT_PTR)(1000 + i);
        ti.rect = g_card_rc[i];
        ti.lpszText = LPSTR_TEXTCALLBACKW;
        SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        g_card_tools = i + 1;
    }
}

static void header_layout(void)
{
    if (!g_header) return;
    RECT rc;
    GetClientRect(g_header, &rc);
    header_measure(rc.right, 1);
    header_tooltips();
    InvalidateRect(g_header, NULL, FALSE);
}

/* ---- Header painting ------------------------------------------------------ */

static const hd_card *card_by_id(int64_t id)
{
    for (int i = 0; i < g_ncards; i++)
        if (g_cards[i].id == id) return &g_cards[i];
    return NULL;
}

static int job_uses(int64_t id)
{
    if (!header_busy() || !id) return 0;
    for (int i = 0; i < g_njob_ids; i++)
        if (g_job_ids[i] == id) return 1;
    return 0;
}

static const wchar_t *job_chip(void)
{
    switch (g_running_kind) {
    case JOB_BACKUP: return T(L"Backing up...");
    case JOB_CHECK:
    case JOB_VERIFY: return T(L"Checking...");
    case JOB_COMPRESS: return T(L"Compressing...");
    }
    return T(L"In use...");
}

static void paint_bar_row(HDC dc, const hd_bar *b, int y)
{
    int x = g_hd_bars.left, right = g_hd_bars.right, row = g_hd_bar_row;
    wchar_t label[120], info[300], a[64], t[64];
    swprintf(label, 120, T(L"Full copy %d of %d"), b->set, g_nhbars);
    RECT lr = {x, y, x + g_hd_label_w, y + row};
    draw_str(dc, g_font_semi, ui_text(), label, lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    int complete = b->files_total > 0 && b->files_done >= b->files_total;
    format_bytes(b->bytes_done, a, 64);
    format_bytes(b->bytes_total, t, 64);
    if (b->files_total == 0) swprintf(info, 300, L"%ls", T(L"Nothing to back up yet"));
    else if (complete) swprintf(info, 300, TN(b->files_total, L"Complete: %lld file (%ls)", L"Complete: %lld files (%ls)"), (long long)b->files_total, t);
    else swprintf(info, 300, T(L"%lld of %lld files (%ls of %ls)"), (long long)b->files_done, (long long)b->files_total, a, t);
    int bar_l = x + g_hd_label_w + S(12), bar_r = right - g_hd_right_w - S(12);
    RECT ir = {bar_r + S(12), y, right, y + row};
    draw_str(dc, g_font, complete ? tone_text(TONE_SAFE) : ui_sub(), info, ir, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (bar_r - bar_l < S(40)) return;
    int bh = S(16);
    RECT br = {bar_l, y + (row - bh) / 2, bar_r, y + (row - bh) / 2 + bh};
    round_box(dc, &br, S(6), ui_track(), CLR_INVALID);
    if (b->files_total <= 0) return;
    /* One piece per drive the copy is on, as wide as its share of the
     * files; a drive being written to now in blue, one that is not
     * plugged in paler. */
    int width = br.right - br.left, px = br.left;
    int64_t sum = 0;
    for (int i = 0; i < b->nseg; i++) sum += b->seg[i].files;
    if (sum > b->files_total) sum = b->files_total;
    int64_t acc = 0;
    HRGN clip = CreateRoundRectRgn(br.left, br.top, br.right + 1, br.bottom + 1, S(6), S(6));
    SelectClipRgn(dc, clip);
    for (int i = 0; i < b->nseg; i++) {
        acc += b->seg[i].files;
        if (acc > b->files_total) acc = b->files_total;
        int end = br.left + (int)((long long)width * acc / b->files_total);
        if (end <= px) continue;
        const hd_card *c = card_by_id(b->seg[i].media_id);
        int tone = job_uses(b->seg[i].media_id) && g_running_kind == JOB_BACKUP ? TONE_WORK : complete ? TONE_SAFE : TONE_WARN;
        COLORREF fill = tone_fill(tone);
        if (c && !c->connected && tone != TONE_WORK && !g_hc) fill = mix(fill, ui_card(), 35);
        RECT sr = {px, br.top, end, br.bottom};
        fill_color(dc, &sr, fill);
        if (i + 1 < b->nseg && end < br.right) {
            RECT sep = {end - S(1) > px ? end - S(1) : end, br.top, end + S(1), br.bottom};
            fill_color(dc, &sep, ui_card());
        }
        if (c && end - px > text_w(dc, g_font, c->name) + S(10)) {
            RECT nr = {px + S(6), br.top, end - S(4), br.bottom};
            draw_str(dc, g_font, tone_on_fill(tone), c->name, nr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        px = end;
    }
    SelectClipRgn(dc, NULL);
    DeleteObject(clip);
}

static void paint_card(HDC dc, int i)
{
    const RECT *r = &g_card_rc[i];
    COLORREF bg = g_hc ? GetSysColor(COLOR_WINDOW) : g_dark ? RGB(50, 50, 50) : RGB(250, 250, 250);
    COLORREF border = i == g_card_hot ? ui_link() : ui_border();
    round_box(dc, r, S(8), bg, border);
    if (i == CARD_MORE) {
        wchar_t s[80];
        swprintf(s, 80, TN(g_card_more, L"+%d more", L"+%d more"), g_card_more);
        RECT tr = {r->left, r->top + S(10), r->right, r->top + S(32)};
        draw_str(dc, g_font_semi, ui_text(), s, tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        RECT sr = {r->left, r->top + S(32), r->right, r->bottom - S(8)};
        draw_str(dc, g_font, ui_link(), T(L"See them all"), sr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    const hd_card *c = &g_cards[i];
    int x = r->left + S(10), y = r->top + S(10), isz = S(32);
    HICON ic = c->secondary ? (g_ico_folder ? g_ico_folder : g_ico_drive) : g_ico_drive;
    if (ic) DrawIconEx(dc, x, y + S(2), ic, isz, isz, 0, NULL, DI_NORMAL);
    if (c->lost) draw_state_icon(dc, x + isz - S(12), y + isz - S(10), S(14), TONE_BAD, GLYPH_CROSS);
    else if (c->connected && !c->secondary) draw_state_icon(dc, x + isz - S(12), y + isz - S(10), S(14), TONE_SAFE, GLYPH_CHECK);
    int tx = x + isz + S(10), tr_ = r->right - S(10);
    RECT nr = {tx, r->top + S(6), tr_, r->top + S(6) + font_h(g_font_semi)};
    draw_str(dc, g_font_semi, ui_text(), c->name, nr, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    /* The state as a chip, then where it is kept. */
    int chip_y = nr.bottom + S(3), chip_h = S(20);
    const wchar_t *chip = job_uses(c->id) ? job_chip() : c->chip;
    int tone = job_uses(c->id) ? TONE_WORK : c->tone;
    int cw = draw_chip(dc, tx, chip_y, chip_h, chip, tone, tr_ - tx);
    if (c->where[0] && tx + cw + S(30) < tr_) {
        RECT wr = {tx + cw + S(6), chip_y, tr_, chip_y + chip_h};
        draw_str(dc, g_font, ui_sub(), c->where, wr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    /* Its space: a thin bar of what is used. */
    if (c->total > 0) {
        RECT sb = {tx, r->bottom - S(10), tr_, r->bottom - S(6)};
        int pct = (int)((c->total - c->freeb) * 100 / c->total);
        int free_pct = 100 - pct;
        draw_bar(dc, sb, pct, free_pct < 5 ? TONE_BAD : free_pct < 15 ? TONE_WARN : TONE_NEUTRAL, 0);
    }
}

static void header_paint(HDC dc, const RECT *rc)
{
    fill_color(dc, rc, ui_bg());
    RECT card = *rc;
    round_box(dc, &card, S(10), ui_card(), ui_border());
    int busy = header_busy();
    int tone = busy ? (g_stopping ? TONE_WARN : TONE_WORK) : g_verdict_tone;
    int glyph = busy ? (g_stopping ? GLYPH_PAUSE : GLYPH_PLAY) : g_verdict_glyph;
    draw_state_icon(dc, g_hd_icon.left, g_hd_icon.top, g_hd_icon.right - g_hd_icon.left, tone, glyph);
    const wchar_t *title = busy && g_jp_title[0] ? g_jp_title : g_verdict;
    const wchar_t *sub = busy ? g_jp_line : g_verdict_sub;
    draw_str(dc, g_font_title, tone == TONE_NEUTRAL ? ui_text() : tone_text(tone), title, g_hd_title, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    draw_str(dc, g_font, ui_sub(), sub, g_hd_sub, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (!g_cat) return;
    for (int i = 0; i < g_nhbars; i++) paint_bar_row(dc, &g_hbars[i], g_hd_bars.top + i * g_hd_bar_row);
    if (!IsRectEmpty(&g_hd_goal)) {
        wchar_t goal[300];
        goal_text(goal, 300);
        RECT gr = g_hd_goal;
        draw_str(dc, g_font, ui_sub(), goal, gr, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        int gw = text_w(dc, g_font, goal) + S(8);
        const wchar_t *chg = T(L"Change...");
        SetRect(&g_goal_link, gr.left + gw, gr.top, gr.left + gw + text_w(dc, g_font, chg), gr.bottom);
        draw_str(dc, g_font, ui_link(), chg, g_goal_link, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    } else
        SetRectEmpty(&g_goal_link);
    for (int i = 0; i < g_ncard_shown; i++) paint_card(dc, i);
    if (g_card_more) paint_card(dc, CARD_MORE);
}

/* The header's main button: filled with the accent colour. */
static void draw_primary_button(const DRAWITEMSTRUCT *di, COLORREF under)
{
    HDC dc = di->hDC;
    RECT r = di->rcItem;
    int disabled = (di->itemState & ODS_DISABLED) != 0, pressed = (di->itemState & ODS_SELECTED) != 0;
    fill_color(dc, &r, under);
    COLORREF fill = g_hc ? GetSysColor(COLOR_HIGHLIGHT) : ui_link(), text = g_hc ? GetSysColor(COLOR_HIGHLIGHTTEXT) : g_dark ? RGB(0, 0, 0) : RGB(255, 255, 255);
    if (pressed && !g_hc) fill = mix(fill, g_dark ? RGB(255, 255, 255) : RGB(0, 0, 0), 15);
    if (disabled) {
        fill = ui_track();
        text = ui_sub();
    }
    round_box(dc, &r, S(8), fill, CLR_INVALID);
    wchar_t s[200];
    GetWindowTextW(di->hwndItem, s, 200);
    HGDIOBJ o = SelectObject(dc, g_font_semi);
    SetTextColor(dc, text);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, s, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | ((di->itemState & ODS_NOACCEL) ? DT_HIDEPREFIX : 0));
    SelectObject(dc, o);
    if ((di->itemState & ODS_FOCUS) && !(di->itemState & ODS_NOFOCUSRECT)) {
        RECT f = r;
        InflateRect(&f, -S(3), -S(3));
        SetTextColor(dc, text);
        DrawFocusRect(dc, &f);
    }
}

static void select_media_row(int64_t id, int secondary)
{
    show_tab(secondary ? TAB_STARRED : TAB_LOCAL);
    HWND lv = CTL(secondary ? ID_PLACE_LIST : ID_LOCAL_LIST);
    const media_row *rows = secondary ? g_place_rows : g_local_rows;
    int n = secondary ? g_place_n : g_local_n;
    for (int i = 0; i < n; i++)
        if (rows[i].id == id) {
            int pos = lv_pos(lv, i);
            ListView_SetItemState(lv, -1, 0, LVIS_SELECTED);
            ListView_SetItemState(lv, pos, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(lv, pos, FALSE);
            SetFocus(lv);
            break;
        }
}

/* The drive's own menu, as the Local backups menu has it. */
static void drive_popup(int64_t id, int x, int y)
{
    if (!g_cat || g_busy || g_ui_depth) return;
    HMENU tmp = CreatePopupMenu();
    fill_menus f = {tmp, 0};
    bd_list_media(g_cat, fill_menu_cb, &f);
    UINT want = (UINT)(ID_DRIVE_ACT_BASE + id * 8 + 3);
    HMENU sub = NULL;
    for (int i = 0; i < GetMenuItemCount(tmp) && !sub; i++) {
        HMENU s = GetSubMenu(tmp, i);
        if (s && GetMenuState(s, want, MF_BYCOMMAND) != (UINT)-1) sub = s;
    }
    int cmd = sub ? TrackPopupMenu(sub, TPM_RETURNCMD | TPM_RIGHTBUTTON, x, y, 0, g_main, NULL) : 0;
    DestroyMenu(tmp);
    if (cmd > 0) PostMessageW(g_main, WM_COMMAND, (WPARAM)cmd, 0);
}

static int card_at(int x, int y)
{
    POINT p = {x, y};
    for (int i = 0; i < g_ncard_shown; i++)
        if (PtInRect(&g_card_rc[i], p)) return i;
    if (g_card_more && PtInRect(&g_card_rc[CARD_MORE], p)) return CARD_MORE;
    return -1;
}

static void paint_buffered(HWND hwnd, HDC target, void (*fn)(HDC, const RECT *))
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;
    HDC mem = CreateCompatibleDC(target);
    HBITMAP bmp = CreateCompatibleBitmap(target, rc.right, rc.bottom);
    HGDIOBJ old = SelectObject(mem, bmp);
    fn(mem, &rc);
    BitBlt(target, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

/* A panel's buttons draw their corners from what is behind them. */
static int panel_common(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT *out, void (*paint)(HDC, const RECT *), COLORREF bg)
{
    switch (msg) {
    case WM_ERASEBKGND:
        *out = 1;
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        paint_buffered(hwnd, dc, paint);
        EndPaint(hwnd, &ps);
        *out = 0;
        return 1;
    }
    case WM_PRINTCLIENT: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        paint((HDC)wp, &rc);
        *out = 0;
        return 1;
    }
    case WM_CTLCOLORBTN:
        SetDCBrushColor((HDC)wp, bg);
        SetBkColor((HDC)wp, bg);
        SetTextColor((HDC)wp, ui_text());
        *out = (LRESULT)GetStockObject(DC_BRUSH);
        return 1;
    case WM_COMMAND:
        *out = SendMessageW(g_main, WM_COMMAND, wp, lp);
        return 1;
    }
    return 0;
}

static LRESULT CALLBACK header_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    LRESULT r;
    if (panel_common(hwnd, msg, wp, lp, &r, header_paint, ui_card())) return r;
    switch (msg) {
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT *di = (const DRAWITEMSTRUCT *)lp;
        if (di->CtlType == ODT_BUTTON) {
            draw_primary_button(di, ui_card());
            return TRUE;
        }
        break;
    }
    case WM_NOTIFY: {
        NMHDR *nh = (NMHDR *)lp;
        if (g_tip && nh->hwndFrom == g_tip && nh->code == TTN_GETDISPINFOW) {
            NMTTDISPINFOW *di = (NMTTDISPINFOW *)lp;
            int i = (int)nh->idFrom - 1000;
            di->lpszText = i >= 0 && i < g_ncards ? g_cards[i].tip : (LPWSTR)L"";
        }
        return 0;
    }
    case WM_SETCURSOR: {
        POINT p;
        GetCursorPos(&p);
        ScreenToClient(hwnd, &p);
        if (card_at(p.x, p.y) >= 0 || (!IsRectEmpty(&g_goal_link) && PtInRect(&g_goal_link, p))) {
            SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_HAND));
            return TRUE;
        }
        break;
    }
    case WM_MOUSEMOVE: {
        int hot = card_at((short)LOWORD(lp), (short)HIWORD(lp));
        if (hot != g_card_hot) {
            g_card_hot = hot;
            InvalidateRect(hwnd, NULL, FALSE);
            TRACKMOUSEEVENT t = {sizeof(t), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&t);
        }
        break;
    }
    case WM_MOUSELEAVE:
        if (g_card_hot != -1) {
            g_card_hot = -1;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_LBUTTONUP: {
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
        POINT p = {x, y};
        int i = card_at(x, y);
        if (i == CARD_MORE) show_tab(TAB_LOCAL);
        else if (i >= 0) select_media_row(g_cards[i].id, g_cards[i].secondary);
        else if (!IsRectEmpty(&g_goal_link) && PtInRect(&g_goal_link, p) && g_cat && !g_busy && !g_ui_depth)
            PostMessageW(g_main, WM_COMMAND, MAKEWPARAM(ID_BTN_RISK, BN_CLICKED), (LPARAM)g_buttons[7]);
        return 0;
    }
    case WM_RBUTTONUP: {
        int i = card_at((short)LOWORD(lp), (short)HIWORD(lp));
        if (i >= 0 && i != CARD_MORE) {
            POINT p = {(short)LOWORD(lp), (short)HIWORD(lp)};
            ClientToScreen(hwnd, &p);
            drive_popup(g_cards[i].id, p.x, p.y);
        }
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- The warnings ------------------------------------------------------------ */

static int warn_is_hidden(const wchar_t *key)
{
    for (int i = 0; i < g_nwarn_hidden; i++)
        if (wcscmp(g_warn_hidden[i], key) == 0) return 1;
    return 0;
}

static int g_warn_shown_idx[WARN_SHOWN];
static int g_warn_rows; /* how many show now */

static int warn_visible_count(void)
{
    int n = 0;
    for (int i = 0; i < g_nwarn; i++)
        if (!warn_is_hidden(g_warns[i].key)) n++;
    return n;
}

static int warn_height(void)
{
    int n = warn_visible_count();
    if (!n || !g_cat) return 0;
    int max = g_warn_all ? WARN_SHOWN : (g_hd_compact ? 1 : 2);
    int rows = n < max ? n : max;
    int h = rows * S(40) + (rows - 1) * S(4);
    if (n > (g_hd_compact ? 1 : 2)) h += S(24);
    return h;
}

static void warn_layout(void)
{
    if (!g_warnbar) return;
    RECT rc;
    GetClientRect(g_warnbar, &rc);
    int max = g_warn_all ? WARN_SHOWN : (g_hd_compact ? 1 : 2), row = 0;
    for (int i = 0; i < g_nwarn && row < max; i++) {
        if (warn_is_hidden(g_warns[i].key)) continue;
        g_warn_shown_idx[row] = i;
        int y = row * (S(40) + S(4));
        SetRect(&g_warn_rc[row], 0, y, rc.right, y + S(40));
        int bx = rc.right - S(10);
        if (g_warns[i].closable) {
            SetRect(&g_warn_x[row], bx - S(24), y + S(8), bx, y + S(32));
            bx -= S(30);
        } else
            SetRectEmpty(&g_warn_x[row]);
        for (int k = 1; k >= 0; k--) {
            HWND b = g_warn_btns[row][k];
            if (!g_warns[i].btn[k][0]) {
                ShowWindow(b, SW_HIDE);
                continue;
            }
            SetWindowTextW(b, g_warns[i].btn[k]);
            int bw = button_width(b) + S(8);
            bx -= bw;
            MoveWindow(b, bx, y + S(6), bw, S(28), TRUE);
            ShowWindow(b, SW_SHOWNA);
            bx -= S(6);
        }
        g_warn_rc[row].right = bx; /* the text stops before the buttons */
        row++;
    }
    for (int r = row; r < WARN_SHOWN; r++) {
        ShowWindow(g_warn_btns[r][0], SW_HIDE);
        ShowWindow(g_warn_btns[r][1], SW_HIDE);
    }
    g_warn_rows = row;
    int n = warn_visible_count(), lim = g_hd_compact ? 1 : 2;
    if (n > lim) {
        int y = row * (S(40) + S(4));
        SetRect(&g_warn_more, S(8), y, S(8) + S(320), y + S(22));
    } else
        SetRectEmpty(&g_warn_more);
    InvalidateRect(g_warnbar, NULL, FALSE);
}

static void warn_paint(HDC dc, const RECT *rc)
{
    fill_color(dc, rc, ui_bg());
    for (int r = 0; r < g_warn_rows; r++) {
        const warn_item *w = &g_warns[g_warn_shown_idx[r]];
        RECT row = g_warn_rc[r];
        row.right = rc->right;
        if (g_hc) round_box(dc, &row, S(8), GetSysColor(COLOR_WINDOW), GetSysColor(COLOR_WINDOWTEXT));
        else round_box(dc, &row, S(8), tone_tint(w->tone), mix(tone_tint(w->tone), tone_fill(w->tone), 25));
        int x = row.left + S(12), cy = row.top + (row.bottom - row.top) / 2;
        draw_state_icon(dc, x, cy - S(9), S(18), w->tone, w->glyph);
        x += S(28);
        int tw = text_w(dc, g_font_semi, w->title);
        int limit = g_warn_rc[r].right - S(8);
        RECT tr = {x, row.top, x + tw < limit ? x + tw : limit, row.bottom};
        draw_str(dc, g_font_semi, ui_text(), w->title, tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (tr.right + S(40) < limit && w->text[0]) {
            RECT xr = {tr.right + S(10), row.top, limit, row.bottom};
            draw_str(dc, g_font, ui_text(), w->text, xr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        if (!IsRectEmpty(&g_warn_x[r])) {
            RECT xr = g_warn_x[r];
            int cx = (xr.left + xr.right) / 2, cyy = (xr.top + xr.bottom) / 2, u = S(4);
            HPEN pen = CreatePen(PS_SOLID, S(1) > 0 ? S(1) : 1, ui_sub());
            HGDIOBJ op = SelectObject(dc, pen);
            MoveToEx(dc, cx - u, cyy - u, NULL);
            LineTo(dc, cx + u + 1, cyy + u + 1);
            MoveToEx(dc, cx + u, cyy - u, NULL);
            LineTo(dc, cx - u - 1, cyy + u + 1);
            SelectObject(dc, op);
            DeleteObject(pen);
        }
    }
    if (!IsRectEmpty(&g_warn_more)) {
        int n = warn_visible_count(), lim = g_hd_compact ? 1 : 2;
        wchar_t s[120];
        if (g_warn_all) swprintf(s, 120, L"%ls", T(L"Show fewer warnings"));
        else swprintf(s, 120, TN(n - lim, L"%d more warning", L"%d more warnings"), n - lim);
        RECT mr = g_warn_more;
        draw_str(dc, g_font, ui_link(), s, mr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        /* A chevron after it: down to show more, up to show fewer. */
        int x = mr.left + text_w(dc, g_font, s) + S(8), cy = (mr.top + mr.bottom) / 2, u = S(4);
        HPEN pen = CreatePen(PS_SOLID, S(1) > 0 ? S(1) : 1, ui_link());
        HGDIOBJ op = SelectObject(dc, pen);
        if (g_warn_all) {
            MoveToEx(dc, x, cy + u / 2, NULL);
            LineTo(dc, x + u, cy - u / 2);
            LineTo(dc, x + u * 2 + 1, cy + u / 2 + 1);
        } else {
            MoveToEx(dc, x, cy - u / 2, NULL);
            LineTo(dc, x + u, cy + u / 2);
            LineTo(dc, x + u * 2 + 1, cy - u / 2 - 1);
        }
        SelectObject(dc, op);
        DeleteObject(pen);
        g_warn_more.right = x + u * 2 + S(4);
    }
}

static void layout(void);

static LRESULT CALLBACK warn_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    LRESULT r;
    if (panel_common(hwnd, msg, wp, lp, &r, warn_paint, ui_bg())) return r;
    switch (msg) {
    case WM_SETCURSOR: {
        POINT p;
        GetCursorPos(&p);
        ScreenToClient(hwnd, &p);
        int hand = !IsRectEmpty(&g_warn_more) && PtInRect(&g_warn_more, p);
        for (int i = 0; i < g_warn_rows && !hand; i++) hand = !IsRectEmpty(&g_warn_x[i]) && PtInRect(&g_warn_x[i], p);
        if (hand) {
            SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_HAND));
            return TRUE;
        }
        break;
    }
    case WM_LBUTTONUP: {
        POINT p = {(short)LOWORD(lp), (short)HIWORD(lp)};
        if (!IsRectEmpty(&g_warn_more) && PtInRect(&g_warn_more, p)) {
            g_warn_all = !g_warn_all;
            layout();
            return 0;
        }
        for (int i = 0; i < g_warn_rows; i++)
            if (!IsRectEmpty(&g_warn_x[i]) && PtInRect(&g_warn_x[i], p) && g_nwarn_hidden < 32) {
                wcscpy(g_warn_hidden[g_nwarn_hidden++], g_warns[g_warn_shown_idx[i]].key);
                layout();
                return 0;
            }
        return 0;
    }
    case WM_NOTIFY: {
        NMHDR *nh = (NMHDR *)lp;
        if (g_tip && nh->hwndFrom == g_tip && nh->code == TTN_GETDISPINFOW) {
            NMTTDISPINFOW *di = (NMTTDISPINFOW *)lp;
            int i = (int)nh->idFrom - 2000;
            static wchar_t tip[1200];
            tip[0] = 0;
            if (i >= 0 && i < g_warn_rows) {
                const warn_item *w = &g_warns[g_warn_shown_idx[i]];
                swprintf(tip, 1200, L"%ls\r\n%ls", w->title, w->text);
            }
            di->lpszText = tip;
        }
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void warn_tooltips(void)
{
    static int tools;
    if (!g_tip || !g_warnbar) return;
    TOOLINFOW ti;
    for (int i = 0; i < tools; i++) {
        memset(&ti, 0, sizeof(ti));
        ti.cbSize = sizeof(ti);
        ti.hwnd = g_warnbar;
        ti.uId = (UINT_PTR)(2000 + i);
        SendMessageW(g_tip, TTM_DELTOOLW, 0, (LPARAM)&ti);
    }
    tools = 0;
    for (int i = 0; i < g_warn_rows; i++) {
        memset(&ti, 0, sizeof(ti));
        ti.cbSize = sizeof(ti);
        ti.uFlags = TTF_SUBCLASS;
        ti.hwnd = g_warnbar;
        ti.uId = (UINT_PTR)(2000 + i);
        ti.rect = g_warn_rc[i];
        ti.lpszText = LPSTR_TEXTCALLBACKW;
        SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        tools = i + 1;
    }
}

/* A warning's button was pressed. */
static void warn_action(int row, int k)
{
    if (row < 0 || row >= g_warn_rows || k < 0 || k > 1) return;
    const warn_item *w = &g_warns[g_warn_shown_idx[row]];
    int64_t arg = w->arg[k];
    switch (w->act[k]) {
    case WA_CHANGED: if (show_changed(arg)) back_up_changes(arg); break;
    case WA_DAMAGED: cmd_table(TABLE_DAMAGED); break;
    case WA_NEXT_TAB:
        g_next_media = arg;
        g_page_stale[TAB_NEXT] = 1;
        show_tab(TAB_NEXT);
        break;
    case WA_CHECK: enqueue_check(arg); break;
    case WA_RELOCATE: cmd_relocate_one(arg); break;
    case WA_RISK: cmd_risk(); break;
    case WA_LOCAL: select_media_row(arg, 0); break;
    case WA_SCAN: enqueue(new_job(JOB_SCAN)); break;
    case WA_RESTORE_ROOT: restore_selection(1, arg); break;
    case WA_DELETED: cmd_deleted(); break;
    }
}

/* ---- The job panel -------------------------------------------------------- */

static const wchar_t *g_job_done_title[] = {N_(L"Looked for your drives"), N_(L"Scan finished"), N_(L"Backup finished"),
                                            N_(L"Check finished"), N_(L"Restore finished"), N_(L"Check finished"),
                                            N_(L"Files in use read"), N_(L"Compression changed"), N_(L"Your changes are saved")};
static const wchar_t *g_job_stop_title[] = {N_(L"Looking for drives stopped"), N_(L"Scan stopped"), N_(L"Backup stopped"),
                                            N_(L"Check stopped"), N_(L"Restore stopped"), N_(L"Check stopped"),
                                            N_(L"Reading files in use stopped"), N_(L"Compressing stopped"), N_(L"Saving stopped")};
static const wchar_t *g_job_fail_title[] = {N_(L"Looking for drives failed"), N_(L"Scan failed"), N_(L"Backup failed"),
                                            N_(L"Check failed"), N_(L"Restore failed"), N_(L"Check failed"),
                                            N_(L"Reading files in use failed"), N_(L"Compressing failed"), N_(L"Saving failed")};

#define JP_TIMER 77

static int jobpanel_height(int w)
{
    (void)w;
    int line = font_h(g_font), pad = S(10);
    switch (g_jp_state) {
    case JP_RUN: return pad + S(28) + S(6) + S(8) + S(6) + line + (g_jp_file[0] ? S(4) + line : 0) + pad;
    case JP_IDLE: return S(44);
    case JP_OK: return pad + S(28) + pad; /* all went well: one line */
    }
    /* A result: its title and up to three lines of what happened. */
    HDC dc = GetDC(NULL);
    HGDIOBJ o = SelectObject(dc, g_font);
    RECT r = {0, 0, w - S(54), 0};
    DrawTextW(dc, g_jp_line, -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, o);
    ReleaseDC(NULL, dc);
    int th = r.bottom;
    if (th > line * 3) th = line * 3;
    if (th < line) th = line;
    return pad + S(28) + S(2) + th + pad;
}

static void jobpanel_layout(void)
{
    if (!g_jobpanel) return;
    RECT rc;
    GetClientRect(g_jobpanel, &rc);
    int pad = S(10), bx = rc.right - pad, y = pad, bh = S(28);
    if (g_jp_state >= JP_OK) {
        SetRect(&g_jp_close, bx - S(24), y + S(2), bx, y + S(26));
        bx -= S(30);
    } else
        SetRectEmpty(&g_jp_close);
    if (g_jp_state == JP_IDLE) y = (rc.bottom - bh) / 2;
    HWND order[5] = {g_log_btn, g_stop, g_readback, g_jp_btn[1], g_jp_btn[0]};
    for (int i = 0; i < 5; i++) {
        HWND b = order[i];
        if (!b) continue;
        int show = b == g_log_btn || (b == g_stop && g_jp_state == JP_RUN && g_running_kind != JOB_REFRESH) ||
                   (b == g_readback && g_n_readback > 0 && g_jp_state != JP_RUN) ||
                   ((b == g_jp_btn[0] || b == g_jp_btn[1]) && g_jp_state >= JP_OK && GetWindowTextLengthW(b) > 0);
        if (!show) {
            if (is_shown(b)) {
                if (GetFocus() == b) g_refocus = 1;
                ShowWindow(b, SW_HIDE);
            }
            continue;
        }
        int bw = button_width(b) + S(8);
        bx -= bw;
        MoveWindow(b, bx, y, bw, bh, TRUE);
        ShowWindow(b, SW_SHOWNA);
        bx -= S(6);
    }
    InvalidateRect(g_jobpanel, NULL, FALSE);
}

static void jobpanel_paint(HDC dc, const RECT *rc)
{
    fill_color(dc, rc, ui_bg());
    RECT card = *rc;
    round_box(dc, &card, S(10), ui_card(), ui_border());
    int pad = S(10), x = pad, y = pad, line = font_h(g_font);
    /* The right end of the text: where the first visible button starts. */
    int right = rc->right - pad;
    HWND all[5] = {g_log_btn, g_stop, g_readback, g_jp_btn[0], g_jp_btn[1]};
    for (int i = 0; i < 5; i++) {
        if (!is_shown(all[i])) continue;
        RECT br;
        GetWindowRect(all[i], &br);
        MapWindowPoints(NULL, g_jobpanel, (POINT *)&br, 2);
        if (br.left - S(10) < right) right = br.left - S(10);
    }
    if (!IsRectEmpty(&g_jp_close) && g_jp_close.left - S(10) < right) right = g_jp_close.left - S(10);
    if (g_jp_state == JP_IDLE) {
        RECT tr = {x + S(4), rc->top, right, rc->bottom};
        draw_str(dc, g_font, ui_sub(), T(L"Nothing is running."), tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    int tone = TONE_WORK, glyph = GLYPH_PLAY;
    switch (g_jp_state) {
    case JP_RUN: tone = g_stopping ? TONE_WARN : TONE_WORK; glyph = g_stopping ? GLYPH_PAUSE : GLYPH_PLAY; break;
    case JP_OK: tone = TONE_SAFE; glyph = GLYPH_CHECK; break;
    case JP_STOPPED: tone = TONE_WARN; glyph = GLYPH_PAUSE; break;
    case JP_FAILED: tone = TONE_BAD; glyph = GLYPH_CROSS; break;
    case JP_PROBLEM: tone = TONE_WARN; glyph = GLYPH_BANG; break;
    }
    draw_state_icon(dc, x, y + S(2), S(24), tone, glyph);
    int tx = x + S(34);
    if (g_jp_state == JP_RUN) {
        wchar_t pct[32] = L"";
        if (g_jp_pct >= 0) swprintf(pct, 32, L"%d%%", g_jp_pct);
        int pw = pct[0] ? text_w(dc, g_font_semi, pct) + S(8) : 0;
        RECT tr = {tx, y, right - pw, y + S(28)};
        draw_str(dc, g_font_semi, ui_text(), g_jp_title, tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (pct[0]) {
            RECT pr = {right - pw, y, right, y + S(28)};
            draw_str(dc, g_font_semi, tone_text(tone), pct, pr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }
        y += S(28) + S(6);
        RECT br = {tx, y, rc->right - pad, y + S(8)};
        draw_bar(dc, br, g_jp_pct, tone, g_jp_phase);
        y += S(8) + S(6);
        RECT lr = {tx, y, rc->right - pad, y + line};
        draw_str(dc, g_font, ui_sub(), g_jp_line, lr, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        y += line;
        if (g_jp_file[0]) {
            y += S(4);
            int fb = g_jp_fpct >= 0 ? S(140) : 0;
            RECT fr = {tx, y, rc->right - pad - (fb ? fb + S(10) : 0), y + line};
            draw_str(dc, g_font, ui_sub(), g_jp_file, fr, DT_LEFT | DT_SINGLELINE | DT_PATH_ELLIPSIS);
            if (fb) {
                RECT fbr = {rc->right - pad - fb, y + line / 2 - S(2), rc->right - pad, y + line / 2 + S(2)};
                draw_bar(dc, fbr, g_jp_fpct, TONE_WORK, 0);
            }
        }
        return;
    }
    RECT tr = {tx, y, right, y + S(28)};
    draw_str(dc, g_font_semi, tone == TONE_SAFE ? ui_text() : tone_text(tone), g_jp_title, tr,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (g_jp_state == JP_OK) {
        /* What it did, on the same line after the title. */
        int lx = tx + text_w(dc, g_font_semi, g_jp_title) + S(14);
        if (lx < right - S(40)) {
            RECT lr = {lx, y, right, y + S(28)};
            draw_str(dc, g_font, ui_sub(), g_jp_line, lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    } else {
        y += S(28) + S(2);
        RECT lr = {tx, y, rc->right - pad, rc->bottom - pad};
        draw_str(dc, g_font, ui_text(), g_jp_line, lr, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);
    }
    if (!IsRectEmpty(&g_jp_close)) {
        RECT xr = g_jp_close;
        int cx = (xr.left + xr.right) / 2, cy = (xr.top + xr.bottom) / 2, u = S(4);
        HPEN pen = CreatePen(PS_SOLID, S(1) > 0 ? S(1) : 1, ui_sub());
        HGDIOBJ op = SelectObject(dc, pen);
        MoveToEx(dc, cx - u, cy - u, NULL);
        LineTo(dc, cx + u + 1, cy + u + 1);
        MoveToEx(dc, cx + u, cy - u, NULL);
        LineTo(dc, cx - u - 1, cy + u + 1);
        SelectObject(dc, op);
        DeleteObject(pen);
    }
}

static LRESULT CALLBACK jobpanel_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    LRESULT r;
    if (panel_common(hwnd, msg, wp, lp, &r, jobpanel_paint, ui_card())) return r;
    switch (msg) {
    case WM_TIMER:
        if (wp == JP_TIMER) {
            g_jp_phase = (g_jp_phase + 2) % 100;
            if (g_jp_state == JP_RUN && g_jp_pct < 0) InvalidateRect(hwnd, NULL, FALSE);
            else KillTimer(hwnd, JP_TIMER);
        }
        return 0;
    case WM_SETCURSOR: {
        POINT p;
        GetCursorPos(&p);
        ScreenToClient(hwnd, &p);
        if (!IsRectEmpty(&g_jp_close) && PtInRect(&g_jp_close, p)) {
            SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_HAND));
            return TRUE;
        }
        break;
    }
    case WM_LBUTTONUP: {
        POINT p = {(short)LOWORD(lp), (short)HIWORD(lp)};
        if (!IsRectEmpty(&g_jp_close) && PtInRect(&g_jp_close, p)) {
            g_jp_state = JP_IDLE;
            SetWindowTextW(g_jp_btn[0], L"");
            SetWindowTextW(g_jp_btn[1], L"");
            layout();
        }
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* Looking for drives after one was plugged in leaves how the last job
 * ended as it was. */
typedef struct {
    int state, act[2];
    wchar_t title[600], line[SUMMARY_LEN + 400], btn[2][120];
} jp_snap;
static jp_snap g_jp_saved;

static void jp_save(void)
{
    g_jp_saved.state = g_jp_state;
    g_jp_saved.act[0] = g_jp_act[0];
    g_jp_saved.act[1] = g_jp_act[1];
    wcscpy(g_jp_saved.title, g_jp_title);
    wcscpy(g_jp_saved.line, g_jp_line);
    for (int k = 0; k < 2; k++) GetWindowTextW(g_jp_btn[k], g_jp_saved.btn[k], 120);
}

static void jp_restore(void)
{
    KillTimer(g_jobpanel, JP_TIMER);
    g_jp_state = g_jp_saved.state == JP_RUN ? JP_IDLE : g_jp_saved.state;
    g_jp_act[0] = g_jp_saved.act[0];
    g_jp_act[1] = g_jp_saved.act[1];
    wcscpy(g_jp_title, g_jp_saved.title);
    wcscpy(g_jp_line, g_jp_saved.line);
    g_jp_file[0] = 0;
    for (int k = 0; k < 2; k++) SetWindowTextW(g_jp_btn[k], g_jp_saved.btn[k]);
    layout();
}

/* A job starts: the panel shows it running. */
static void jp_start(const wchar_t *what)
{
    int was = g_jp_state;
    if (g_running_kind == JOB_DRIVES && was != JP_RUN) jp_save();
    g_jp_state = JP_RUN;
    g_jp_pct = -1;
    g_jp_fpct = -1;
    g_jp_file[0] = 0;
    g_jp_line[0] = 0;
    swprintf(g_jp_title, 600, L"%ls", what);
    SetWindowTextW(g_jp_btn[0], L"");
    SetWindowTextW(g_jp_btn[1], L"");
    SetTimer(g_jobpanel, JP_TIMER, 30, NULL);
    if (was != JP_RUN) layout();
    else jobpanel_layout();
    if (g_header) InvalidateRect(g_header, NULL, FALSE);
}

/* Progress from the worker. The line starts with what is being done, then
 * a colon: that part is the title. */
static void jp_progress(const prog_msg *m)
{
    if (g_jp_state != JP_RUN) return;
    const wchar_t *line = m->text;
    if (m->title[0]) {
        size_t n = wcslen(m->title);
        if (wcsncmp(m->text, m->title, n) == 0 && m->text[n] == L':' && m->text[n + 1] == L' ') line = m->text + n + 2;
        if (!g_stopping) swprintf(g_jp_title, 600, L"%ls", m->title);
    }
    swprintf(g_jp_line, SUMMARY_LEN + 400, L"%ls", line);
    int had_file = g_jp_file[0] != 0;
    swprintf(g_jp_file, 700, L"%ls", m->file);
    g_jp_fpct = m->file_percent;
    g_jp_pct = m->percent;
    if (g_jp_pct < 0) SetTimer(g_jobpanel, JP_TIMER, 30, NULL);
    if (had_file != (g_jp_file[0] != 0)) layout();
    else InvalidateRect(g_jobpanel, NULL, FALSE);
    if (g_header) InvalidateRect(g_header, NULL, FALSE);
}

static void jp_stopping(void)
{
    swprintf(g_jp_title, 600, L"%ls", T(L"Stopping..."));
    swprintf(g_jp_line, SUMMARY_LEN + 400, L"%ls", T(L"What was done until now is kept."));
    g_jp_pct = -1;
    SetTimer(g_jobpanel, JP_TIMER, 30, NULL);
    jobpanel_layout();
    if (g_header) InvalidateRect(g_header, NULL, FALSE);
}

/* A job ended: how, in a card with what to do about it. */
static void jp_result(const job *j, const wchar_t *headline, int stopped, int drive_gone)
{
    KillTimer(g_jobpanel, JP_TIMER);
    g_jp_file[0] = 0;
    g_jp_act[0] = g_jp_act[1] = JA_NONE;
    SetWindowTextW(g_jp_btn[0], L"");
    SetWindowTextW(g_jp_btn[1], L"");
    int k = (int)j->kind;
    if (k < 0 || k > JOB_REFRESH) k = JOB_REFRESH;
    swprintf(g_jp_line, SUMMARY_LEN + 400, L"%ls", j->summary);
    if (drive_gone) {
        wchar_t name[600];
        media_name(j->media_id, name, 600);
        g_jp_state = JP_FAILED;
        swprintf(g_jp_title, 600, T(L"%ls: %ls was unplugged or stopped answering"), T(g_job_stop_title[k]), name);
        swprintf(g_jp_line, SUMMARY_LEN + 400, L"%ls", T(L"What was done until then is kept. Plug the drive in again and start it again."));
    } else if (stopped) {
        g_jp_state = JP_STOPPED;
        swprintf(g_jp_title, 600, L"%ls", T(g_job_stop_title[k]));
    } else if (j->status != BD_OK) {
        g_jp_state = JP_FAILED;
        swprintf(g_jp_title, 600, L"%ls", T(g_job_fail_title[k]));
        g_jp_act[0] = JA_REPORTS;
        SetWindowTextW(g_jp_btn[0], T(L"Show the log"));
    } else if (j->kind == JOB_SCAN && j->roots_missing && g_figs && g_figs->n_missing > 0) {
        g_jp_state = JP_PROBLEM;
        if (g_figs->n_missing == 1) swprintf(g_jp_title, 600, T(L"Scan finished, but %ls cannot be found"), g_figs->missing_name[0]);
        else swprintf(g_jp_title, 600, TN(g_figs->n_missing, L"Scan finished, but %d backup root cannot be found",
                                          L"Scan finished, but %d backup roots cannot be found"), g_figs->n_missing);
        g_jp_act[0] = JA_REPORTS;
        SetWindowTextW(g_jp_btn[0], T(L"Show what went wrong"));
    } else if (j->n_problems > 0) {
        g_jp_state = JP_PROBLEM;
        swprintf(g_jp_title, 600, L"%ls", headline && headline[0] ? headline : T(g_job_done_title[k]));
        g_jp_act[0] = JA_REPORTS;
        SetWindowTextW(g_jp_btn[0], T(L"Show what went wrong"));
    } else {
        g_jp_state = JP_OK;
        swprintf(g_jp_title, 600, L"%ls", T(g_job_done_title[k]));
        if (j->kind == JOB_SCAN && j->bitrot && j->damaged > 0) {
            g_jp_act[0] = JA_DAMAGED;
            SetWindowTextW(g_jp_btn[0], T(L"Show the damaged files"));
        }
    }
    if (j->kind == JOB_RESTORE && j->status == BD_OK && !stopped && !drive_gone && j->dest && j->dest[0]) {
        wchar_t *dw = widen(j->dest);
        swprintf(g_jp_dest, 600, L"%ls", dw ? dw : L"");
        free(dw);
        int k2 = g_jp_act[0] == JA_NONE ? 0 : 1;
        g_jp_act[k2] = JA_OPEN_DEST;
        SetWindowTextW(g_jp_btn[k2], T(L"Open the folder"));
        if (k2 == 0 && j->n_conf > 0) {
            wchar_t b[120];
            swprintf(b, 120, TN(j->n_conf, L"Compare the %d left alone...", L"Compare the %d left alone..."), j->n_conf);
            g_jp_act[1] = JA_CONFLICTS;
            SetWindowTextW(g_jp_btn[1], b);
        }
        if (g_jp_state == JP_OK && j->n_conf > 0) g_jp_state = JP_PROBLEM; /* two lines: the summary says what was left alone */
    }
    layout();
}

static void jp_action(int k)
{
    if (k < 0 || k > 1) return;
    switch (g_jp_act[k]) {
    case JA_REPORTS:
        if (g_n_report) cmd_table(TABLE_REPORT);
        else show_app_log(g_main);
        break;
    case JA_DAMAGED: cmd_table(TABLE_DAMAGED); break;
    case JA_CONFLICTS: cmd_table(TABLE_CONFLICTS); break;
    case JA_OPEN_DEST:
        if (g_jp_dest[0]) show_in_explorer(g_jp_dest);
        break;
    }
}

/* The header's button: the one thing to do next. */
static void header_next(void)
{
    switch (g_next_kind) {
    case NEXT_ADD_ROOT: cmd_add_folder(); break;
    case NEXT_ADD_DRIVE: add_disk(); break;
    case NEXT_BACKUP: drive_action(g_next_id, 0); break;
    case NEXT_RISK: cmd_risk(); break;
    case NEXT_SCAN: enqueue(new_job(JOB_SCAN)); break;
    }
}

static void log_toggle(void)
{
    g_log_open = !g_log_open;
    DWORD v = (DWORD)g_log_open;
    RegSetKeyValueW(HKEY_CURRENT_USER, REG_KEY, L"LogOpen", REG_DWORD, &v, sizeof(v));
    SetWindowTextW(g_log_btn, g_log_open ? T(L"Hide the log") : T(L"Show the log"));
    layout();
}

static HWND panel_button(HWND parent, const wchar_t *text, int id, DWORD extra)
{
    HWND b = CreateWindowExW(0, WC_BUTTONW, text, WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON | extra, 0, 0, 10, 10, parent,
                             (HMENU)(INT_PTR)id, g_inst, NULL);
    SendMessageW(b, WM_SETFONT, (WPARAM)g_font, TRUE);
    return b;
}

static void register_panel_classes(void)
{
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpfnWndProc = header_proc;
    wc.lpszClassName = L"BrodalfHeader";
    RegisterClassExW(&wc);
    wc.lpfnWndProc = warn_proc;
    wc.lpszClassName = L"BrodalfWarnings";
    RegisterClassExW(&wc);
    wc.lpfnWndProc = jobpanel_proc;
    wc.lpszClassName = L"BrodalfJob";
    RegisterClassExW(&wc);
}

/* Made in create_children: the header, the warnings and the job panel,
 * with their buttons. */
static void create_panels(HWND hwnd)
{
    register_panel_classes();
    load_ui_icons();
    DWORD ex = WS_EX_CONTROLPARENT;
    g_header = CreateWindowExW(ex, L"BrodalfHeader", L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 10, 10, hwnd,
                               (HMENU)(INT_PTR)ID_HEADER, g_inst, NULL);
    g_warnbar = CreateWindowExW(ex, L"BrodalfWarnings", L"", WS_CHILD | WS_CLIPCHILDREN, 0, 0, 10, 10, hwnd,
                                (HMENU)(INT_PTR)ID_WARNBAR, g_inst, NULL);
    g_next_btn = panel_button(g_header, L"", ID_BTN_NEXT, BS_OWNERDRAW);
    g_nodb_open = panel_button(g_header, T(L"&Open a database..."), ID_M_OPEN_DB, 0);
    g_nodb_new = panel_button(g_header, T(L"&New database..."), ID_M_NEW_DB, 0);
    for (int r = 0; r < WARN_SHOWN; r++)
        for (int k = 0; k < 2; k++) g_warn_btns[r][k] = panel_button(g_warnbar, L"", ID_WARN_BTN0 + r * 2 + k, 0);
}

/* After the tabs, so Tab reaches it after them: the job panel and its
 * buttons. */
static void create_job_panel(HWND hwnd)
{
    g_jobpanel = CreateWindowExW(WS_EX_CONTROLPARENT, L"BrodalfJob", L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 10, 10, hwnd,
                                 (HMENU)(INT_PTR)ID_JOBPANEL, g_inst, NULL);
    g_stop = panel_button(g_jobpanel, T(L"Sto&p"), ID_BTN_STOP, 0);
    g_readback = panel_button(g_jobpanel, T(L"Check what was just &written"), ID_BTN_READBACK, 0);
    g_jp_btn[0] = panel_button(g_jobpanel, L"", ID_BTN_JOB1, 0);
    g_jp_btn[1] = panel_button(g_jobpanel, L"", ID_BTN_JOB2, 0);
    /* The log: open in a tall window, closed in a low one, until the
     * user says. */
    DWORD v = 0, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, REG_KEY, L"LogOpen", RRF_RT_REG_DWORD, NULL, &v, &sz) == ERROR_SUCCESS) g_log_open = v != 0;
    else {
        RECT wa;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        g_log_open = wa.bottom - wa.top >= S(900);
    }
    g_log_btn = panel_button(g_jobpanel, g_log_open ? T(L"Hide the log") : T(L"Show the log"), ID_BTN_LOGTOGGLE, 0);
    ShowWindow(g_log_btn, SW_SHOWNA);
    header_refresh();
}

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
    int pad = S(8), gap = S(8), bar = S(30);
    status_bar_refresh();

    /* The buttons, each as wide as its text, onto a second row (or more)
     * when the window is narrow. */
    int x = pad, y = pad;
    for (int i = 0; i < N_BUTTONS; i++) {
        int bw = button_width(g_buttons[i]);
        if (x > pad && x + bw > w - pad) {
            x = pad;
            y += bar + gap;
        }
        MoveWindow(g_buttons[i], x, y, bw, bar, TRUE);
        x += bw + gap;
    }
    int top = y + bar + gap, hw = w - pad * 2;

    /* A low window: one row of drives and of warnings, and no goal line
     * (the status bar has it), so the tabs keep room for their lists. */
    int jh = g_jobpanel ? jobpanel_height(hw) : 0;
    int log_h = g_log_open ? S(110) : 0, want = S(340), hh = 0, wh = 0, body_h = 0;
    for (int pass = 0; pass < 2; pass++) {
        g_hd_compact = pass == 1 || h < S(720);
        hh = g_header ? header_height(hw) : 0;
        wh = g_warnbar ? warn_height() : 0;
        body_h = h - top - (hh ? hh + gap : 0) - (wh ? wh + gap : 0) - gap - jh - (log_h ? log_h + gap : 0) - pad;
        if (body_h >= want || g_hd_compact) break;
    }
    if (g_header) {
        MoveWindow(g_header, pad, top, hw, hh, TRUE);
        header_layout();
        top += hh + gap;
    }
    if (g_warnbar) {
        if (wh) {
            MoveWindow(g_warnbar, pad, top, hw, wh, TRUE);
            ShowWindow(g_warnbar, SW_SHOWNA);
            warn_layout();
            warn_tooltips();
            top += wh + gap;
        } else
            ShowWindow(g_warnbar, SW_HIDE);
    }

    /* Then the log gives up some of its height. */
    if (log_h && body_h < want) {
        int take = want - body_h;
        if (take > log_h - S(56)) take = log_h - S(56);
        if (take > 0) {
            log_h -= take;
            body_h += take;
        }
    }
    if (body_h < S(80)) body_h = S(80);
    /* The tabs, each page over the tab control's display area. */
    MoveWindow(g_tabs, pad, top, hw, body_h, TRUE);
    RECT tr = {pad, top, w - pad, top + body_h};
    TabCtrl_AdjustRect(g_tabs, FALSE, &tr);
    for (int i = 0; i < N_TABS; i++) MoveWindow(g_page[i], tr.left, tr.top, tr.right - tr.left, tr.bottom - tr.top, TRUE);
    layout_pages();
    /* Under the tabs, what runs now or how the last job ended; then the log. */
    int py = top + body_h + gap;
    if (g_jobpanel) {
        MoveWindow(g_jobpanel, pad, py, hw, jh, TRUE);
        jobpanel_layout();
        py += jh + gap;
    }
    if (log_h) {
        MoveWindow(g_log, pad, py, hw, log_h, TRUE);
        if (!is_shown(g_log)) ShowWindow(g_log, SW_SHOWNA);
    } else if (is_shown(g_log)) {
        if (GetFocus() == g_log) SetFocus(g_tabs);
        ShowWindow(g_log, SW_HIDE);
    }
    /* What is above and below the tabs moves: paint those strips afresh,
     * so nothing is left of where a line was. */
    RECT above = {0, 0, w, top}, below = {0, top + body_h, w, h};
    RedrawWindow(g_main, &above, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    RedrawWindow(g_main, &below, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

/* The tree paints two columns on the right of each row: its state, as a
 * chip, and its size. After the name come a star when it is starred, a
 * lock when it is encrypted, and a root's path in grey. */
static int g_tree_state_w, g_tree_size_w;

/* The theme's parts and states for a tree row (vssym32.h). */
enum { TREE_PART_ITEM = 1, TREE_HOT = 2, TREE_SELECTED = 3, TREE_SELECTED_NOFOCUS = 5, TREE_HOT_SELECTED = 6 };

/* Rows tall enough for a chip, at any size of text. */
static void tree_metrics(void)
{
    if (!g_tree) return;
    int h = font_h(g_font) + S(10);
    if (h < S(24)) h = S(24);
    TreeView_SetItemHeight(g_tree, h);
}

static COLORREF tree_bg(void) { return g_hc ? GetSysColor(COLOR_WINDOW) : g_dark ? DARK_FIELD : GetSysColor(COLOR_WINDOW); }
static COLORREF tree_fg(void) { return g_hc ? GetSysColor(COLOR_WINDOWTEXT) : g_dark ? DARK_TEXT : GetSysColor(COLOR_WINDOWTEXT); }
static COLORREF star_color(void) { return g_hc ? GetSysColor(COLOR_WINDOWTEXT) : g_dark ? RGB(255, 214, 10) : RGB(193, 156, 0); }

/* A chip no wider than max: its words end in ... if they do not fit. */
static int draw_chip_fit(HDC dc, int x, int y, int h, const wchar_t *s, int tone, int max)
{
    int w = text_w(dc, g_font, s) + S(12);
    if (w > max) w = max;
    if (w < S(16)) return 0;
    RECT r = {x, y, x + w, y + h};
    if (g_hc) round_box(dc, &r, S(6), GetSysColor(COLOR_WINDOW), GetSysColor(COLOR_WINDOWTEXT));
    else round_box(dc, &r, S(6), tone_tint(tone), CLR_INVALID);
    RECT t = {x + S(6), y, x + w - S(6), y + h};
    draw_str(dc, g_font, tone_text(tone), s, t, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    return w;
}

/* A small padlock in lines, size s high. Returns its width. */
static int draw_lock(HDC dc, int x, int y, int s, COLORREF c)
{
    int bw = s * 3 / 4, bh = s * 9 / 16, by = y + s - bh, lw = S(3) / 2 < 1 ? 1 : S(3) / 2;
    int sx0 = x + bw / 5, sx1 = x + bw - bw / 5;
    HPEN pen = CreatePen(PS_SOLID, lw, c);
    HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, sx0, y, sx1 + 1, by + bh / 2, sx1 - sx0, sx1 - sx0);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(pen);
    RECT b = {x, by, x + bw, y + s};
    round_box(dc, &b, S(3), c, CLR_INVALID);
    return bw;
}

/* What the State column says for a row, and in which tone. */
static const wchar_t *tree_chip(const node_ref *r, wchar_t *buf, size_t cap, int *tone)
{
    *tone = TONE_NEUTRAL;
    if (r->root && r->missing) { *tone = TONE_BAD; return T(L"Cannot be found"); }
    if (r->root && r->inactive) return T(L"No longer backed up");
    if (!r->is_dir) {
        if (r->state == BD_STATE_BAD) { *tone = TONE_BAD; return T(L"Copy damaged"); }
        if (r->damaged && r->state != BD_STATE_DELETED) { *tone = TONE_WARN; return T(L"May be damaged"); }
        switch (r->state) {
        case BD_STATE_AVAILABLE_OLDER: *tone = TONE_WARN; return T(L"Changed since backup");
        case BD_STATE_NO_COPY: *tone = TONE_WARN; return T(L"Not backed up");
        case BD_STATE_DELETED: return T(L"Deleted");
        case BD_STATE_OFFLINE:
            if (!r->drive) return T(L"Drive not plugged in");
            swprintf(buf, cap, T(L"On %ls"), r->drive);
            return buf;
        default: return NULL;
        }
    }
    if (r->damaged) {
        *tone = TONE_WARN;
        swprintf(buf, cap, TN(r->damaged, L"%d may be damaged", L"%d may be damaged"), r->damaged);
        return buf;
    }
    if (r->state == BD_STATE_NO_COPY && r->files_total > 0) { *tone = TONE_WARN; return T(L"Not backed up"); }
    if (r->state == BD_STATE_OFFLINE && r->drive) {
        swprintf(buf, cap, T(L"On %ls"), r->drive);
        return buf;
    }
    if (r->files_total <= 0) return NULL;
    swprintf(buf, cap, T(L"%lld of %lld available"), (long long)r->files_available, (long long)r->files_total);
    return buf;
}

static void tree_post_paint(NMTVCUSTOMDRAW *cd, const node_ref *r)
{
    HDC dc = cd->nmcd.hdc;
    HTREEITEM h = (HTREEITEM)cd->nmcd.dwItemSpec;
    RECT row = cd->nmcd.rc, cr, tr;
    GetClientRect(g_tree, &cr);
    if (!TreeView_GetItemRect(g_tree, h, &tr, TRUE)) return;
    int rh = row.bottom - row.top, right = cr.right - S(8);
    int show_size = cr.right >= S(380), show_state = cr.right >= S(260);
    int size_l = show_size ? right - g_tree_size_w : right;
    int state_l = show_state ? size_l - (show_size ? S(12) : 0) - g_tree_state_w : size_l;
    if (show_state && state_l < tr.left + S(48)) { /* deep in the tree and narrow: the tooltip says it */
        show_state = 0;
        state_l = size_l;
    }
    int cols_l = state_l - S(8);
    if (cols_l < tr.left + S(24)) cols_l = tr.left + S(24);

    /* A name too long for the room left: cut where the columns start. */
    int sel = (cd->nmcd.uItemState & CDIS_SELECTED) != 0, hot = (cd->nmcd.uItemState & CDIS_HOT) != 0;
    COLORREF fg = tree_fg();
    if (!sel) {
        if (r->state == BD_STATE_OFFLINE || r->state == BD_STATE_NO_COPY || r->state == BD_STATE_DELETED) fg = theme_gray();
    }
    if (tr.right > cols_l) {
        int ell = text_w(dc, g_font, L"\x2026") + S(2);
        RECT clip = {cols_l - ell, row.top, row.right, row.bottom};
        fill_color(dc, &clip, tree_bg());
        if (sel || hot) {
            HTHEME th = OpenThemeData(g_tree, L"TreeView");
            if (th) {
                int st = sel ? (GetFocus() == g_tree ? (hot ? TREE_HOT_SELECTED : TREE_SELECTED) : TREE_SELECTED_NOFOCUS) : TREE_HOT;
                DrawThemeBackground(th, dc, TREE_PART_ITEM, st, &row, &clip);
                CloseThemeData(th);
            }
        }
        RECT er = {cols_l - ell, row.top, cols_l, row.bottom};
        draw_str(dc, g_font, fg, L"\x2026", er, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        tr.right = cols_l;
    }

    /* After the name: the star, the lock, and where a root is. */
    int x = tr.right + S(6), gh = font_h(g_font);
    if (r->starred && x + gh < cols_l) {
        RECT sr = {x, row.top, cols_l, row.bottom};
        draw_str(dc, g_font, star_color(), L"\x2605", sr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        x += text_w(dc, g_font, L"\x2605") + S(5);
    }
    if (r->encrypted && x + gh < cols_l) {
        int ls = gh * 3 / 4;
        x += draw_lock(dc, x, row.top + (rh - ls) / 2, ls, ui_link()) + S(6);
    }
    if (r->root && r->path && x + S(24) < cols_l) {
        RECT pr = {x + S(2), row.top, cols_l, row.bottom};
        draw_str(dc, g_font, theme_gray(), r->path, pr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);
    }

    /* The columns. */
    int ch = gh + S(4);
    if (ch > rh - S(2)) ch = rh - S(2);
    int cy = row.top + (rh - ch) / 2;
    if (show_state) {
        wchar_t buf[300];
        int tone;
        const wchar_t *chip = tree_chip(r, buf, COUNT_OF(buf), &tone);
        if (chip) draw_chip_fit(dc, state_l, cy, ch, chip, tone, g_tree_state_w);
    }
    if (show_size && r->size > 0 && (!r->is_dir || r->root)) {
        wchar_t bytes[64];
        format_bytes(r->size, bytes, 64);
        RECT zr = {size_l, row.top, right, row.bottom};
        draw_str(dc, g_font, sel ? tree_fg() : theme_gray(), bytes, zr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
}

static COLORREF page_bg(void) { return g_dark ? DARK_BG : GetSysColor(COLOR_WINDOW); }

/* Star it and Encrypt it: a star or a lock, then the words; filled with a
 * tint of the accent colour and edged with it while it is on. */
static void draw_toggle_button(const DRAWITEMSTRUCT *di, int on)
{
    HDC dc = di->hDC;
    RECT r = di->rcItem;
    int disabled = (di->itemState & ODS_DISABLED) != 0, pressed = (di->itemState & ODS_SELECTED) != 0;
    COLORREF under = page_bg();
    fill_color(dc, &r, under);
    COLORREF face = g_hc ? GetSysColor(COLOR_BTNFACE) : g_dark ? RGB(55, 55, 55) : RGB(253, 253, 253);
    COLORREF edge = g_hc ? GetSysColor(COLOR_WINDOWTEXT) : g_dark ? RGB(85, 85, 85) : RGB(204, 204, 204);
    if (on) {
        face = g_hc ? GetSysColor(COLOR_HIGHLIGHT) : mix(under, ui_link(), g_dark ? 22 : 12);
        if (!g_hc) edge = ui_link();
    }
    if (pressed && !g_hc) face = mix(face, g_dark ? RGB(255, 255, 255) : RGB(0, 0, 0), 8);
    RECT b = {r.left + S(1), r.top + S(1), r.right - S(1), r.bottom - S(1)};
    round_box(dc, &b, S(8), face, edge);
    COLORREF tc = g_hc && on ? GetSysColor(COLOR_HIGHLIGHTTEXT) : disabled ? ui_sub() : ui_text();
    wchar_t s[200];
    GetWindowTextW(di->hwndItem, s, 200);
    int gh = font_h(g_font), x = r.left + S(10);
    if (GetDlgCtrlID(di->hwndItem) == ID_DB_STAR) {
        RECT gr = {x, r.top, r.right, r.bottom};
        draw_str(dc, g_font, on && !g_hc ? star_color() : tc, on ? L"\x2605" : L"\x2606", gr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        x += text_w(dc, g_font, L"\x2605") + S(6);
    } else {
        int ls = gh * 3 / 4;
        x += draw_lock(dc, x, (r.top + r.bottom - ls) / 2, ls, on && !g_hc ? ui_link() : g_hc ? tc : ui_sub()) + S(7);
    }
    RECT tr = {x, r.top, r.right - S(6), r.bottom};
    draw_str(dc, g_font, tc, s, tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if ((di->itemState & ODS_FOCUS) && !(di->itemState & ODS_NOFOCUSRECT)) {
        RECT f = r;
        InflateRect(&f, -S(4), -S(4));
        SetTextColor(dc, tc);
        DrawFocusRect(dc, &f);
    }
}

/* The page buttons BRODALF draws: the tab's main action, and the toggles. */
static void draw_page_button(const DRAWITEMSTRUCT *di)
{
    LONG_PTR v = GetWindowLongPtrW(di->hwndItem, GWLP_USERDATA);
    if (v & BTN_TOGGLE) draw_toggle_button(di, (int)(v & 1));
    else draw_primary_button(di, page_bg());
}

static LRESULT tree_custom_draw(NMTVCUSTOMDRAW *cd)
{
    switch (cd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT: {
        /* The State column is as wide as its longest word, in this language. */
        const wchar_t *probe[] = {T(L"Changed since backup"), T(L"Not backed up"), T(L"Copy damaged"), T(L"May be damaged"),
                                  T(L"Cannot be found"), T(L"No longer backed up"), T(L"Drive not plugged in")};
        int w = S(120);
        for (int i = 0; i < COUNT_OF(probe); i++) {
            int t = text_w(cd->nmcd.hdc, g_font, probe[i]) + S(12);
            if (t > w) w = t;
        }
        g_tree_state_w = w;
        g_tree_size_w = text_w(cd->nmcd.hdc, g_font, L"999.9 MB") + S(6);
        return CDRF_NOTIFYITEMDRAW;
    }
    case CDDS_ITEMPREPAINT: {
        node_ref *r = (node_ref *)cd->nmcd.lItemlParam;
        if (!r) return CDRF_DODEFAULT;
        int sel = (cd->nmcd.uItemState & CDIS_SELECTED) != 0;
        HFONT font = NULL;
        switch (r->state) {
        case BD_STATE_OFFLINE:
            if (!sel) cd->clrText = theme_gray();
            break;
        case BD_STATE_NO_COPY:
            if (!sel) cd->clrText = theme_gray();
            if (!r->is_dir) font = g_font_italic;
            break;
        case BD_STATE_DELETED:
            if (!sel) cd->clrText = theme_gray();
            font = g_font_strike;
            break;
        default:
            break;
        }
        if (r->root && r->missing && !sel) cd->clrText = theme_gray();
        LRESULT ret = CDRF_NOTIFYPOSTPAINT;
        if (font && !r->root) {
            SelectObject(cd->nmcd.hdc, font);
            ret |= CDRF_NEWFONT;
        }
        return ret;
    }
    case CDDS_ITEMPOSTPAINT: {
        node_ref *r = (node_ref *)cd->nmcd.lItemlParam;
        if (r) tree_post_paint(cd, r);
        return CDRF_DODEFAULT;
    }
    }
    return CDRF_DODEFAULT;
}

/* A tooltip whose text is asked for when it shows (TTN_GETDISPINFOW). */
static void add_tip(HWND h)
{
    TOOLINFOW ti;
    memset(&ti, 0, sizeof(ti));
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = g_main;
    ti.uId = (UINT_PTR)h;
    ti.lpszText = LPSTR_TEXTCALLBACKW;
    SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}

static HWND make_button(const wchar_t *text, int id)
{
    HWND b = CreateWindowExW(0, WC_BUTTONW, text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 10, 10, g_main,
                             (HMENU)(INT_PTR)id, g_inst, NULL);
    SendMessageW(b, WM_SETFONT, (WPARAM)g_font, TRUE);
    return b;
}

static void create_children(HWND hwnd)
{
    g_main = hwnd;
    /* The & letters (Alt+letter) differ from each other and from the
     * menus' in every language. */
    g_buttons[0] = make_button(T(L"&Add backup root..."), ID_BTN_ADD);
    g_buttons[1] = make_button(T(L"&Scan now"), ID_BTN_SCAN);
    g_buttons[2] = make_button(T(L"&Back up..."), ID_BTN_BACKUP);
    g_buttons[3] = make_button(T(L"&Check drive..."), ID_BTN_CHECK);
    g_buttons[4] = make_button(T(L"&Restore..."), ID_BTN_RESTORE);
    g_buttons[5] = make_button(T(L"&Drives..."), ID_BTN_DRIVES);
    g_buttons[6] = make_button(T(L"Settin&gs..."), ID_BTN_SECURITY);
    g_buttons[7] = make_button(T(L"Files at ris&k..."), ID_BTN_RISK);
    /* What the files' state is, each drive, and what to look at first. */
    create_panels(hwnd);

    create_pages(hwnd);
    g_tree = CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASBUTTONS | TVS_LINESATROOT | TVS_SHOWSELALWAYS |
                                 TVS_FULLROWSELECT | TVS_INFOTIP | TVS_NOHSCROLL,
                             0, 0, 10, 10, g_page[TAB_DB], (HMENU)ID_TREE, g_inst, NULL);
    SendMessageW(g_tree, WM_SETFONT, (WPARAM)g_font, TRUE);
    TreeView_SetExtendedStyle(g_tree, TVS_EX_DOUBLEBUFFER, TVS_EX_DOUBLEBUFFER);
    tree_icons_init();
    tree_metrics();

    g_search = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 10, 10,
                               g_page[TAB_DB], (HMENU)ID_SEARCH, g_inst, NULL);
    SendMessageW(g_search, WM_SETFONT, (WPARAM)g_font, TRUE);
    SendMessageW(g_search, EM_SETCUEBANNER, TRUE, (LPARAM)T(L"Search files and folders by name"));
    /* Tab reaches the search box before the tree under it. */
    SetWindowPos(g_search, CTL(ID_DB_EXPORT), 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    g_results = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                WS_CHILD | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, 0, 0,
                                10, 10, g_page[TAB_DB], (HMENU)ID_RESULTS, g_inst, NULL);
    SendMessageW(g_results, WM_SETFONT, (WPARAM)g_font, TRUE);
    {
        static const wchar_t *cols[] = {N_(L"Name"), N_(L"Folder"), N_(L"State"), N_(L"Where it is")};
        static const int widths[] = {150, 130, 120, 320};
        lv_columns(g_results, cols, widths, 4);
    }

    g_detail = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, L"",
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 0, 0,
                               10, 10, g_page[TAB_DB], (HMENU)ID_DETAIL, g_inst, NULL);
    SendMessageW(g_detail, WM_SETFONT, (WPARAM)g_font, TRUE);
    read_only_keys(g_detail);

    g_list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_NOSORTHEADER,
                             0, 0, 10, 10, g_page[TAB_DB], (HMENU)ID_LIST, g_inst, NULL);
    SendMessageW(g_list, WM_SETFONT, (WPARAM)g_font, TRUE);
    ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    const wchar_t *cols[] = {T(L"Version"), T(L"Where"), T(L"State"), T(L"Last checked"), T(L"Size"), T(L"Path on drive")};
    int widths[] = {90, 130, 160, 120, 80, 260};
    for (int i = 0; i < 6; i++) {
        LVCOLUMNW c;
        memset(&c, 0, sizeof(c));
        c.mask = LVCF_TEXT | LVCF_WIDTH;
        c.pszText = (LPWSTR)cols[i];
        c.cx = S(widths[i]);
        SendMessageW(g_list, LVM_INSERTCOLUMNW, i, (LPARAM)&c);
    }

    create_job_panel(hwnd);

    g_log = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, L"",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 0, 0, 10, 10,
                            hwnd, (HMENU)ID_LOG, g_inst, NULL);
    SendMessageW(g_log, WM_SETFONT, (WPARAM)g_font, TRUE);
    SendMessageW(g_log, EM_SETLIMITTEXT, LOG_LIMIT, 0);
    read_only_keys(g_log);

    g_status = CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0, hwnd,
                               (HMENU)ID_STATUS, g_inst, NULL);
    SendMessageW(g_status, WM_SETFONT, (WPARAM)g_font, TRUE);

    /* The whole text of the drives line and the warnings, which a narrow
     * window cuts short, and what Stop does. */
    g_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT,
                            CW_USEDEFAULT, CW_USEDEFAULT, hwnd, NULL, g_inst, NULL);
    if (g_tip) {
        SendMessageW(g_tip, WM_SETFONT, (WPARAM)g_font, FALSE);
        SendMessageW(g_tip, TTM_SETMAXTIPWIDTH, 0, S(560));
        SendMessageW(g_tip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 30000);
        add_tip(g_stop);
        add_tip(g_readback);
    }
}

/* Esc stops the job that is running, or leaves the search results. Enter
 * presses the button with the focus, searches now in the search box, and
 * in a list does what a double-click does. */
static void enter_or_esc(int esc)
{
    HWND f = GetFocus();
    if (esc) {
        if (g_busy) {
            if (!g_stopping) stop_job();
        } else if ((f == g_search || f == g_results) && GetWindowTextLengthW(g_search) > 0) {
            end_search();
        }
        return;
    }
    if (!f || !IsChild(g_main, f)) return;
    if (SendMessageW(f, WM_GETDLGCODE, 0, 0) & (DLGC_DEFPUSHBUTTON | DLGC_UNDEFPUSHBUTTON)) {
        SendMessageW(f, BM_CLICK, 0, 0);
        return;
    }
    if (f == g_search) {
        KillTimer(g_main, ID_SEARCH_TIMER);
        run_search();
        return;
    }
    if (f == g_results) {
        if (g_hit_n) end_search();
        return;
    }
    wchar_t cls[64];
    int id = GetDlgCtrlID(f);
    if (id >= ID_PAGE_FIRST && id < ID_PAGE_LAST && f == CTL(id) && GetClassNameW(f, cls, 64) && wcscmp(cls, WC_LISTVIEWW) == 0) {
        NMHDR nh;
        nh.hwndFrom = f;
        nh.idFrom = (UINT_PTR)id;
        nh.code = NM_DBLCLK;
        page_notify(&nh);
    }
}

static BOOL CALLBACK syscolor_child(HWND h, LPARAM lp)
{
    (void)lp;
    SendMessageW(h, WM_SYSCOLORCHANGE, 0, 0);
    return TRUE;
}

/* ---- Moving to a monitor with another DPI --------------------------------- */

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

typedef struct { HFONT from[4], to[4]; int old_dpi; } refont;

/* Each control onto the new fonts, and a table's columns to the new size. */
static BOOL CALLBACK refont_child(HWND h, LPARAM lp)
{
    const refont *r = (const refont *)lp;
    HFONT f = (HFONT)SendMessageW(h, WM_GETFONT, 0, 0);
    for (int i = 0; f && i < 4; i++) {
        if (f == r->from[i]) {
            SendMessageW(h, WM_SETFONT, (WPARAM)r->to[i], FALSE);
            break;
        }
    }
    wchar_t cls[64];
    if (GetClassNameW(h, cls, 64) && _wcsicmp(cls, WC_LISTVIEWW) == 0 && r->old_dpi > 0) {
        HWND header = ListView_GetHeader(h);
        int n = header ? Header_GetItemCount(header) : 0;
        for (int i = 0; i < n; i++) ListView_SetColumnWidth(h, i, MulDiv(ListView_GetColumnWidth(h, i), g_dpi, r->old_dpi));
    }
    return TRUE;
}

/* The app's icon on the window, at the window's DPI (the class has it at
 * the system's). */
static void set_window_icons(HWND hwnd)
{
    int sys = g_sys_dpi > 0 ? g_sys_dpi : 96;
    int big_px = MulDiv(GetSystemMetrics(SM_CXICON), g_dpi, sys), small_px = MulDiv(GetSystemMetrics(SM_CXSMICON), g_dpi, sys);
    HICON b = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(IDI_BRODALF), IMAGE_ICON, big_px, big_px, 0);
    HICON s = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(IDI_BRODALF), IMAGE_ICON, small_px, small_px, 0);
    /* What was set before was set here too (WM_SETICON gives NULL the first time). */
    HICON old_b = b ? (HICON)SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)b) : NULL;
    HICON old_s = s ? (HICON)SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)s) : NULL;
    if (old_b) DestroyIcon(old_b);
    if (old_s) DestroyIcon(old_s);
}

/* WM_DPICHANGED: new fonts for every control, the warning icon at the new
 * size, the window at the size Windows suggests, and everything laid out
 * again. */
static void dpi_changed(HWND hwnd, int dpi, const RECT *want)
{
    if (dpi <= 0) dpi = 96;
    if (dpi != g_dpi) {
        refont r;
        r.old_dpi = g_dpi;
        r.from[0] = g_font;
        r.from[1] = g_font_italic;
        r.from[2] = g_font_strike;
        r.from[3] = g_font_bold;
        g_dpi = dpi;
        make_fonts();
        r.to[0] = g_font;
        r.to[1] = g_font_italic;
        r.to[2] = g_font_strike;
        r.to[3] = g_font_bold;
        EnumChildWindows(hwnd, refont_child, (LPARAM)&r);
        tree_metrics();
        local_metrics();
        if (g_tip) {
            SendMessageW(g_tip, WM_SETFONT, (WPARAM)g_font, FALSE);
            SendMessageW(g_tip, TTM_SETMAXTIPWIDTH, 0, S(560));
        }
        for (int i = 0; i < 4; i++)
            if (r.from[i]) DeleteObject(r.from[i]);
        set_window_icons(hwnd);
    }
    if (want)
        SetWindowPos(hwnd, NULL, want->left, want->top, want->right - want->left, want->bottom - want->top, SWP_NOZORDER | SWP_NOACTIVATE);
    layout();
    InvalidateRect(hwnd, NULL, TRUE);
}

static void close_database(void);

/* Windows is shutting down or signing out (WM_ENDSESSION). The running job
 * is stopped and waited for, so what it did is saved, and the database is
 * saved and closed, which also removes its working copy. The next start
 * says which job was cut short. */
static void end_session(void)
{
    InterlockedExchange(&g_session_end, 1);
    if (g_busy) {
        int kind = g_running_kind;
        bd_applog("Windows is shutting down or signing out: stopping the running job");
        ShutdownBlockReasonCreate(g_main, T(L"Stopping the job and saving the database..."));
        if (kind != JOB_REFRESH) {
            DWORD v = (DWORD)kind;
            RegSetKeyValueW(HKEY_CURRENT_USER, REG_KEY, L"Interrupted", REG_DWORD, &v, sizeof(v));
        }
        stop_job(); /* a save (JOB_REFRESH) is let finish */
        if (!g_worker_thread || WaitForSingleObject(g_worker_thread, 60000) != WAIT_OBJECT_0) {
            bd_applog("the job did not stop in time: the database could not be saved");
            return;
        }
        /* The job's end, which the window would have shown. */
        MSG m;
        while (PeekMessageW(&m, g_main, WM_APP_DONE, WM_APP_DONE, PM_REMOVE)) {
            job *j = (job *)m.lParam;
            char *s = narrow(j->summary);
            bd_applog("the job ended: %s", s ? s : "");
            free(s);
            job_free(j);
        }
    } else {
        bd_applog("Windows is shutting down or signing out");
    }
    /* With a dialog open its code still holds the database: only save. */
    if (g_ui_depth) {
        if (g_cat) bd_catalog_save(g_cat);
    } else {
        close_database();
    }
    ShutdownBlockReasonDestroy(g_main);
}

/* After a shutdown or sign-out cut a job short, the next start says so. */
static void say_interrupted(void)
{
    DWORD kind = 0, n = sizeof(kind);
    if (RegGetValueW(HKEY_CURRENT_USER, REG_KEY, L"Interrupted", RRF_RT_REG_DWORD, NULL, &kind, &n) != ERROR_SUCCESS) return;
    RegDeleteKeyValueW(HKEY_CURRENT_USER, REG_KEY, L"Interrupted");
    if (kind >= sizeof(g_job_doing) / sizeof(g_job_doing[0])) return;
    wchar_t text[600];
    swprintf(text, 600, T(L"Windows shut down or signed out while BRODALF was busy (%ls). BRODALF stopped it and kept what was done "
                          L"until then; start it again to finish it."),
             T(g_job_doing[kind]));
    log_append(text);
}

static LRESULT CALLBACK main_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    LRESULT themed;
    if (theme_menu_bar(hwnd, msg, wp, lp, &themed) || theme_ctlcolor(msg, wp, lp, &themed)) return themed;
    switch (msg) {
    case WM_ERASEBKGND:
        if (g_dark) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect((HDC)wp, &rc, g_br_bg);
            return 1;
        }
        break;
    case WM_SETTINGCHANGE:
        /* Windows' app mode or high contrast changed. High contrast always
         * re-colours the window: its colours are the theme's. */
        if (wp == SPI_SETHIGHCONTRAST || high_contrast() != g_hc) theme_apply();
        else if (g_theme_choice == THEME_FOLLOW && theme_want_dark() != g_dark) theme_apply();
        break;
    case WM_SYSCOLORCHANGE:
        /* The common controls need to hear of it too. */
        EnumChildWindows(hwnd, syscolor_child, 0);
        theme_apply();
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) {
            HWND f = GetFocus();
            if (f && IsChild(hwnd, f)) g_focus = f;
        }
        break; /* activating it sets the focus on the window: WM_SETFOCUS passes it on */
    case WM_SETFOCUS: {
        HWND f = usable(g_focus) ? g_focus : usable(g_focus_job) && !g_busy ? g_focus_job : NULL;
        if (!f) f = g_tab == TAB_DB && usable(g_tree) ? g_tree : g_tabs;
        if (usable(f)) SetFocus(f);
        return 0;
    }
    case WM_CREATE: {
        /* The monitor the window opens on may be at another DPI than the
         * fonts were made for. */
        int d = window_dpi(hwnd);
        if (d != g_dpi) {
            HFONT old[4] = {g_font, g_font_italic, g_font_strike, g_font_bold};
            g_dpi = d;
            make_fonts();
            for (int i = 0; i < 4; i++)
                if (old[i]) DeleteObject(old[i]);
        }
        set_window_icons(hwnd);
        create_children(hwnd);
        create_menu_bar(hwnd);
        show_tab(last_tab());
        set_busy(0);
        return 0;
    }
    case WM_DPICHANGED:
        dpi_changed(hwnd, (int)HIWORD(wp), (const RECT *)lp);
        return 0;
    case WM_INITMENUPOPUP:
        if ((HMENU)wp == g_menu_local && !g_busy) fill_storage_menus();
        return 0;
    case WM_SETCURSOR:
        /* Saving a change: the arrow says the window is working. */
        if (g_busy && g_running_kind == JOB_REFRESH && LOWORD(lp) == HTCLIENT) {
            SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_APPSTARTING));
            return TRUE;
        }
        break;
    case WM_APP_KICK:
        start_next_job();
        return 0;
    case WM_CONTEXTMENU:
        if (((HWND)wp == g_tree || (HWND)wp == g_list) && g_cat && !g_busy && !g_ui_depth) {
            int x = (int)(short)LOWORD(lp), y = (int)(short)HIWORD(lp);
            g_ui_depth++;
            if ((HWND)wp == g_tree) tree_context_menu(x, y);
            else list_context_menu(x, y);
            g_ui_depth--;
            start_next_job();
            refresh_pages();
            return 0;
        }
        break;
    case WM_SIZE:
        layout();
        return 0;
    case WM_GETMINMAXINFO: {
        /* The buttons go onto more rows, and the log gets lower, before
         * anything is cut off. */
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        mm->ptMinTrackSize.x = S(600);
        mm->ptMinTrackSize.y = S(520);
        return 0;
    }
    case WM_COMMAND:
        /* Enter and Esc (IsDialogMessage sends them as IDOK and IDCANCEL). */
        if (!lp && (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)) {
            enter_or_esc(LOWORD(wp) == IDCANCEL);
            return 0;
        }
        /* A shortcut does nothing when its menu item is greyed out. */
        if (!lp && HIWORD(wp) == 1) {
            UINT st = GetMenuState(GetMenu(hwnd), LOWORD(wp), MF_BYCOMMAND);
            if (st != (UINT)-1 && (st & (MF_GRAYED | MF_DISABLED))) return 0;
        }
        if (!lp && LOWORD(wp) == ID_M_STOP) {
            stop_job();
            return 0;
        }
        /* Help works while a job runs: the log and the reports are what
         * tells what went wrong. */
        if (!lp && (LOWORD(wp) == ID_M_LOG || LOWORD(wp) == ID_M_REPORTS || LOWORD(wp) == ID_M_LEGEND || LOWORD(wp) == ID_M_ABOUT)) {
            menu_command(LOWORD(wp));
            return 0;
        }
        if (!lp && (LOWORD(wp) == ID_M_NEXT_TAB || LOWORD(wp) == ID_M_PREV_TAB)) {
            show_tab((g_tab + (LOWORD(wp) == ID_M_NEXT_TAB ? 1 : N_TABS - 1)) % N_TABS);
            return 0;
        }
        /* The log opens and closes while a job runs too. */
        if (LOWORD(wp) == ID_BTN_LOGTOGGLE) {
            log_toggle();
            return 0;
        }
        if (!lp && LOWORD(wp) == ID_M_FIND) {
            if (g_cat) {
                show_tab(TAB_DB);
                focus_control(g_search);
            }
            return 0;
        }
        if (LOWORD(wp) == ID_SEARCH) {
            if (HIWORD(wp) == EN_CHANGE) SetTimer(hwnd, ID_SEARCH_TIMER, 250, NULL);
            return 0;
        }
        if (LOWORD(wp) == ID_BTN_STOP) {
            stop_job();
            return 0;
        }
        /* View > Language: the same. */
        if (!lp && LOWORD(wp) >= ID_M_LANG_FOLLOW && LOWORD(wp) < ID_M_LANG_FIRST + N_LANGS) {
            CheckMenuRadioItem(g_menu_lang, ID_M_LANG_FOLLOW, ID_M_LANG_FIRST + N_LANGS - 1, LOWORD(wp), MF_BYCOMMAND);
            lang_choose(LOWORD(wp) == ID_M_LANG_FOLLOW ? -1 : LOWORD(wp) - ID_M_LANG_FIRST);
            return 0;
        }
        /* View > Appearance: works with no database and while a job runs. */
        if (!lp && LOWORD(wp) >= ID_M_THEME_FOLLOW && LOWORD(wp) <= ID_M_THEME_DARK) {
            CheckMenuRadioItem(g_menu_look, ID_M_THEME_FOLLOW, ID_M_THEME_DARK, LOWORD(wp), MF_BYCOMMAND);
            theme_choose(LOWORD(wp) - ID_M_THEME_FOLLOW);
            return 0;
        }
        /* View: a tab. Works while a job runs; the tab is filled when it ends. */
        if (!lp && LOWORD(wp) >= ID_M_TAB_FIRST && LOWORD(wp) < ID_M_TAB_FIRST + N_TABS) {
            show_tab(LOWORD(wp) - ID_M_TAB_FIRST);
            return 0;
        }
        /* From a control only clicks, and choosing in the Next backup list. */
        if (lp && HIWORD(wp) != BN_CLICKED && !(LOWORD(wp) == ID_NEXT_FOR && HIWORD(wp) == CBN_SELCHANGE)) return 0;
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
        case ID_BTN_READBACK: read_back_new(); break;
        case ID_BTN_NEXT: header_next(); break;
        case ID_BTN_JOB1: jp_action(0); break;
        case ID_BTN_JOB2: jp_action(1); break;
        default:
            if (LOWORD(wp) >= ID_WARN_BTN0 && LOWORD(wp) < ID_WARN_BTN0 + WARN_SHOWN * 2)
                warn_action((LOWORD(wp) - ID_WARN_BTN0) / 2, (LOWORD(wp) - ID_WARN_BTN0) % 2);
            else if (LOWORD(wp) >= ID_PAGE_FIRST && LOWORD(wp) < ID_PAGE_LAST) page_command(LOWORD(wp), HIWORD(wp));
            else if (LOWORD(wp) >= ID_M_ADD_FOLDER && LOWORD(wp) < ID_MENU_OTHER) menu_command(LOWORD(wp));
            else if (LOWORD(wp) >= ID_DRIVE_ACT_BASE) menu_command(LOWORD(wp));
            break;
        }
        g_ui_depth--;
        start_next_job();
        refresh_pages();
        return 0;
    case WM_TIMER:
        if (wp == ID_SEARCH_TIMER) {
            KillTimer(hwnd, ID_SEARCH_TIMER);
            run_search();
        }
        return 0;
    case WM_NOTIFY: {
        NMHDR *nh = (NMHDR *)lp;
        if (g_tip && nh->hwndFrom == g_tip) {
            if (nh->code == TTN_GETDISPINFOW) {
                NMTTDISPINFOW *di = (NMTTDISPINFOW *)lp;
                HWND h = (HWND)nh->idFrom;
                g_tip_text[0] = 0;
                if (h == g_stop) {
                    swprintf(g_tip_text, 4096, L"%ls", T(L"Stop the running job (Esc). What was done until then is kept."));
                } else if (h == g_readback) {
                    swprintf(g_tip_text, 4096, L"%ls", g_readback_tip);
                }
                di->lpszText = g_tip_text;
            }
            return 0;
        }
        if (nh->idFrom == ID_NEXT_LIST && nh->hwndFrom == CTL(ID_NEXT_LIST) && nh->code == NM_CUSTOMDRAW)
            return next_custom_draw((NMLVCUSTOMDRAW *)lp);
        if (nh->idFrom == ID_ENC_STATE && nh->hwndFrom == CTL(ID_ENC_STATE) && nh->code == NM_CUSTOMDRAW)
            return enc_state_draw((NMLVCUSTOMDRAW *)lp);
        if (nh->idFrom == ID_LOCAL_LIST && nh->hwndFrom == CTL(ID_LOCAL_LIST)) {
            if (nh->code == NM_CUSTOMDRAW) return local_custom_draw((NMLVCUSTOMDRAW *)lp);
            if (nh->code == LVN_GETINFOTIPW) {
                local_info_tip((NMLVGETINFOTIPW *)lp);
                return 0;
            }
        }
        if (page_notify(nh)) return 0;
        if (nh->idFrom == ID_RESULTS) {
            if (nh->code == LVN_ITEMCHANGED) {
                NMLISTVIEW *lv = (NMLISTVIEW *)lp;
                int hit = lv->iItem >= 0 ? lv_index(g_results, lv->iItem) : -1;
                if ((lv->uNewState & LVIS_SELECTED) && !(lv->uOldState & LVIS_SELECTED) && hit >= 0 && hit < g_hit_n)
                    reveal_node(g_hits[hit].source_id, g_hits[hit].rel);
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
                g_page_stale[TAB_RESTORE] = 1;
                page_buttons_state();
                return 0;
            }
            if (nh->code == TVN_DELETEITEMW) {
                NMTREEVIEWW *tv = (NMTREEVIEWW *)lp;
                node_ref *r = (node_ref *)tv->itemOld.lParam;
                free_node_ref(r);
                return 0;
            }
            if (nh->code == TVN_GETINFOTIPW) {
                NMTVGETINFOTIPW *tip = (NMTVGETINFOTIPW *)lp;
                node_ref *r = (node_ref *)tip->lParam;
                if (r && r->tip && tip->pszText && tip->cchTextMax > 0) lstrcpynW(tip->pszText, r->tip, tip->cchTextMax);
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
    case WM_APP_SETS: {
        sets_msg *m = (sets_msg *)lp;
        if (!g_figs) g_figs = calloc(1, sizeof(*g_figs));
        if (g_figs && g_cat) {
            g_figs->has_sets = 1;
            g_figs->n_sets = m->n;
            memcpy(g_figs->sets, m->sets, sizeof(g_figs->sets));
            memcpy(g_figs->set_on, m->on, sizeof(g_figs->set_on));
            memcpy(g_figs->sd, m->sd, sizeof(g_figs->sd));
            g_figs->nsd = m->nsd;
            header_bars_refresh();
            if (g_header) InvalidateRect(g_header, NULL, FALSE);
        }
        free(m);
        return 0;
    }
    case WM_APP_PROGRESS: {
        prog_msg *m = (prog_msg *)lp;
        jp_progress(m);
        free(m);
        return 0;
    }
    case WM_APP_DONE: {
        job *j = (job *)lp;
        if (g_session_end) { /* the database is closed (end_session) */
            job_free(j);
            return 0;
        }
        int stopping = g_stopping != 0;
        if (j->figs) {
            figures_free(g_figs);
            g_figs = j->figs;
            j->figs = NULL;
        }
        if (j->kind == JOB_REFRESH) {
            /* A change was saved: the figures are new, the tree is as it was. */
            InterlockedExchange(&g_busy, 0);
            status_state(j->saving && j->status == BD_OK ? T(L"Saved") : L"");
            if (g_close_after) {
                job_free(j);
                DestroyWindow(hwnd);
                return 0;
            }
            g_ui_depth++;
            if (j->status != BD_OK) {
                log_append(j->summary);
                report_status(hwnd, j->status, 0, j->summary);
            }
            job_free(j);
            g_ui_depth--;
            if (g_close_after) {
                DestroyWindow(hwnd);
                return 0;
            }
            set_busy(0);
            update_drives_label();
            show_detail();
            refresh_pages();
            start_next_job();
            return 0;
        }
        /* The worker has let go of the catalog. Until the prompts below are
         * answered, the buttons stay disabled and nothing new starts. */
        g_ui_depth++;
        InterlockedExchange(&g_busy, 0);
        hide_stop();
        j->summary[SUMMARY_LEN - 1] = 0;
        /* Problems first: "Backup finished with 3 problems. Backup done: ..." */
        wchar_t headline[300], line[SUMMARY_LEN + 300];
        job_headline(j, headline, 300);
        swprintf(line, SUMMARY_LEN + 300, L"%ls%ls%ls", headline, headline[0] ? L" " : L"", j->summary);
        /* The job panel says how it ended, with what to do about it;
         * looking for drives leaves the last job's ending there. */
        int stopped = stopping || j->status == BD_ERR_CANCELLED;
        if (j->kind == JOB_DRIVES && j->status == BD_OK) jp_restore();
        else jp_result(j, headline, stopped, !stopped && j->status != BD_OK && job_drive_gone(j));
        status_state(L"");
        log_append(line);
        if (j->n_report && (j->n_problems > 0 || j->status != BD_OK)) {
            /* Kept for "Show what went wrong" until the next job with problems. */
            for (int i = 0; i < g_n_report; i++) free(g_report[i]);
            free(g_report);
            g_report = j->report;
            g_n_report = j->n_report;
            j->report = NULL;
            j->n_report = j->cap_report = 0;
        }
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
                MessageBoxW(hwnd, T(L"The scan was stopped before it finished, so BRODALF may not know about every new or changed "
                                  L"file yet. A backup now copies what the scans so far found.\n\nScan again when you can, so your "
                                  L"backups are as up to date as possible."),
                            APP_NAME, MB_ICONINFORMATION);
        } else {
            if (j->status == BD_ERR_PASSPHRASE && (j->kind == JOB_BACKUP || j->kind == JOB_CHECK || j->kind == JOB_VERIFY) &&
                unlock_ui(T(L"Some of what this needs is encrypted: an encrypted drive, or folders you chose to encrypt. Enter the "
                          L"passphrase to continue."))) {
                job *again = new_job(j->kind);
                if (again) {
                    again->media_id = j->media_id;
                    again->root = xstrdup(j->root);
                    again->label = xstrdup(j->label);
                    again->location = xstrdup(j->location);
                    again->flags = j->flags;
                    memcpy(again->many, j->many, sizeof(j->many));
                    again->n_many = j->n_many;
                    again->many_places = j->many_places;
                    enqueue(again);
                }
            } else if (j->status != BD_OK && j->status != BD_ERR_PASSPHRASE) {
                report_status(hwnd, j->status, job_drive_gone(j), j->summary);
            } else if (j->status == BD_OK && j->n_tell > 0) {
                show_job_problems(j, headline);
            }
            if (GetWindowTextLengthW(g_search) > 0) run_search();
            if (j->kind == JOB_DRIVES) after_drives(j);
            else if (j->kind == JOB_SCAN && j->status == BD_OK) after_scan(j);
            else if (j->kind == JOB_BACKUP && j->status == BD_OK) {
                after_backup(j);
                offer_read_back(j);
            }
            else if (j->kind == JOB_RESTORE && j->status == BD_OK) after_restore(j);
            else if (j->kind == JOB_SHADOW && j->status == BD_OK) after_shadow(j);
            /* A scan that leads to a backup from the shadow copy waits for the backup to ask. */
            if ((j->kind == JOB_SCAN || j->kind == JOB_BACKUP) && j->in_use && j->status == BD_OK &&
                !(j->from_shadow && shadow_still_needed()))
                show_in_use(0, j);
        }
        job_free(j);
        /* No job is running now: remove the shadow copy once nothing more reads it. */
        if (g_shadow_dir[0] && !shadow_still_needed()) shadow_cleanup();
        g_ui_depth--;
        if (g_close_after) { /* Close was answered while the questions above were up */
            DestroyWindow(hwnd);
            return 0;
        }
        set_busy(0);
        update_drives_label();
        refresh_pages();
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
    case WM_ENDSESSION:
        if (wp) end_session();
        return 0;
    case WM_CLOSE:
        if (g_busy && g_running_kind == JOB_REFRESH) {
            /* A change is being saved: close once it is. */
            g_close_after = 1;
            status_state(T(L"Closing once your changes are saved..."));
            return 0;
        }
        if (g_busy) {
            static const wchar_t *ask[] = {N_(L"Stop looking for drives and close?"), N_(L"Stop the scan and close?"),
                                           N_(L"Stop the backup and close?"), N_(L"Stop the check and close?"),
                                           N_(L"Stop the restore and close?"), N_(L"Stop checking the copies and close?"),
                                           N_(L"Stop reading the files in use and close?"),
                                           N_(L"Stop compressing the drive and close?")};
            if (g_close_after) return 0; /* already stopping; closes when the job returns */
            wchar_t text[256];
            swprintf(text, 256, T(L"%ls\n\nWhat was done until then is kept. BRODALF closes as soon as the job has stopped."),
                     T(ask[g_running_kind]));
            if (ask_verbs(hwnd, text, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2, T(L"Stop and close"), T(L"Keep it running")) == IDYES) {
                g_close_after = 1;
                if (g_busy) stop_job();
                else if (!g_ui_depth) DestroyWindow(hwnd); /* the job ended while the question was up */
                /* else the end-of-job questions are up: it closes once they are answered */
            }
            return 0;
        }
        if (g_ui_depth) {
            MessageBoxW(hwnd, T(L"BRODALF is still working. Close it when the status bar says the job is done."), APP_NAME, MB_ICONINFORMATION);
            return 0;
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (g_shadow_dir[0]) shadow_cleanup(); /* no job runs any more */
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- Opening, creating and recovering a database ------------------------ */

/* The database opened last: kept in %LOCALAPPDATA%\\BRODALF\\last-database.txt,
 * and in the registry, where BRODALF 0.8.0 and earlier kept it (so it is
 * still found after an upgrade). */
static void reg_get_last(wchar_t *out, DWORD cap)
{
    out[0] = 0;
    char *saved = bd_last_database();
    wchar_t *w = widen(saved);
    free(saved);
    if (w && w[0]) {
        wcsncpy(out, w, cap - 1);
        out[cap - 1] = 0;
        free(w);
        return;
    }
    free(w);
    DWORD bytes = cap * sizeof(wchar_t);
    if (RegGetValueW(HKEY_CURRENT_USER, REG_KEY, L"LastCatalog", RRF_RT_REG_SZ, NULL, out, &bytes) != ERROR_SUCCESS) out[0] = 0;
}

static void reg_set_last(const wchar_t *path)
{
    char *p = narrow(path);
    if (p && bd_last_database_set(p) != BD_OK) bd_applog("could not remember the last database in the app data folder");
    free(p);
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(k, L"LastCatalog", 0, REG_SZ, (const BYTE *)path, (DWORD)((wcslen(path) + 1) * sizeof(wchar_t)));
        RegCloseKey(k);
    }
}

static int open_dialog_in(wchar_t *out, DWORD cap, const wchar_t *dir)
{
    OPENFILENAMEW of;
    memset(&of, 0, sizeof(of));
    of.lStructSize = sizeof(of);
    of.hwndOwner = g_main;
    wchar_t filter_text[256];
    of.lpstrFilter = file_filter(filter_text, 256, 4, N_(L"BRODALF databases (*.brodalf)"), L"*.brodalf", N_(L"All files"), L"*.*");
    of.lpstrFile = out;
    of.nMaxFile = cap;
    of.lpstrDefExt = L"brodalf";
    out[0] = 0;
    of.lpstrTitle = T(L"Open a BRODALF database");
    of.lpstrInitialDir = dir && dir[0] ? dir : NULL;
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&of);
}

static int open_dialog(wchar_t *out, DWORD cap) { return open_dialog_in(out, cap, NULL); }

/* Where to keep a new database file. Never an existing file: nothing is
 * overwritten. */
static int save_dialog(wchar_t *out, DWORD cap, const wchar_t *title)
{
    for (;;) {
        OPENFILENAMEW of;
        memset(&of, 0, sizeof(of));
        of.lStructSize = sizeof(of);
        of.hwndOwner = g_main;
        wchar_t filter_text[256];
        of.lpstrFilter = file_filter(filter_text, 256, 2, N_(L"BRODALF databases (*.brodalf)"), L"*.brodalf");
        if (!out[0]) wcsncpy(out, T(L"My backups.brodalf"), cap - 1);
        of.lpstrFile = out;
        of.nMaxFile = cap;
        of.lpstrDefExt = L"brodalf";
        of.lpstrTitle = title;
        of.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetSaveFileNameW(&of)) return 0;
        if (GetFileAttributesW(out) == INVALID_FILE_ATTRIBUTES) return 1;
        MessageBoxW(g_main, T(L"A file with that name is already there. Choose another name, so nothing is overwritten."), APP_NAME,
                    MB_ICONINFORMATION);
    }
}

static int ask_database_choice(const wchar_t *last, const wchar_t *missing)
{
    wchar_t last_btn[MAX_PATH * 2 + 64], gone[MAX_PATH * 2 + 300];
    TASKDIALOG_BUTTON buttons[5];
    int n = 0;
    if (missing[0]) {
        buttons[n].nButtonID = CHOICE_FIND;
        buttons[n++].pszButtonText = T(L"Find it...\nLook for it in another folder or on another drive");
    }
    if (last[0]) {
        swprintf(last_btn, MAX_PATH * 2 + 64, T(L"Open the last database\n%ls"), last);
        buttons[n].nButtonID = CHOICE_LAST;
        buttons[n++].pszButtonText = last_btn;
    }
    buttons[n].nButtonID = CHOICE_OPEN;
    buttons[n++].pszButtonText = T(L"Open a database...\nA .brodalf file you made before");
    buttons[n].nButtonID = CHOICE_NEW;
    buttons[n++].pszButtonText = T(L"Create a new database...\nChoose the folders to back up, then where to keep the database file");
    buttons[n].nButtonID = CHOICE_RECOVER;
    buttons[n++].pszButtonText = T(L"Recover the database from a backup drive...\nOn a new PC, or after losing the database file: "
                                 L"start from the newest copy on your drives");

    TASKDIALOGCONFIG tc;
    memset(&tc, 0, sizeof(tc));
    tc.cbSize = sizeof(tc);
    tc.hwndParent = g_main;
    tc.hInstance = g_inst;
    tc.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
    tc.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    tc.pszWindowTitle = APP_NAME;
    tc.pszMainInstruction = T(L"Which database do you want to use?");
    tc.pszContent = T(L"A database records every file in your backup roots and which drives hold a copy of it. It never holds the "
                    L"files themselves.");
    if (missing[0]) {
        swprintf(gone, MAX_PATH * 2 + 300, T(L"%ls was moved, renamed or deleted, or the drive it is on is not connected."), missing);
        tc.pszMainIcon = TD_WARNING_ICON;
        tc.pszMainInstruction = T(L"Your last database cannot be found");
        tc.pszContent = gone;
    }
    tc.cButtons = (UINT)n;
    tc.pButtons = buttons;
    int pressed = IDCANCEL;
    if (FAILED(TaskDialogIndirect(&tc, &pressed, NULL, NULL))) return IDCANCEL;
    return pressed;
}

/* Open a database, asking for the passphrase if the file is encrypted.
 * A cancelled prompt returns BD_ERR_PASSPHRASE with no message. */
static int g_opened_by_recovery; /* offer a new passphrase once it is open */

typedef struct { const char *path; bd_catalog **out; bd_status s; } open_try_ctx;

static int open_try(void *ctx, const char *secret, int recovery)
{
    open_try_ctx *o = ctx;
    o->s = recovery ? bd_catalog_open_with_recovery(o->path, secret, o->out) : bd_catalog_open_with(o->path, secret, o->out);
    return o->s != BD_ERR_PASSPHRASE;
}

static bd_status open_maybe_encrypted(const char *path, bd_catalog **out)
{
    if (!bd_catalog_file_needs_passphrase(path)) return bd_catalog_open(path, out);
    int can_recover = bd_catalog_file_has_recovery_key(path);
    int recovery = 0;
    open_try_ctx o = {path, out, BD_ERR_PASSPHRASE};
    g_pass_try = open_try;
    g_pass_try_ctx = &o;
    char *p = ask_unlock_secret(g_main, T(L"This database is encrypted. Enter its passphrase to open it."), can_recover, &recovery);
    g_pass_try = NULL;
    if (!p) return BD_ERR_PASSPHRASE;
    free_secret(p);
    if (o.s == BD_OK && recovery) g_opened_by_recovery = 1;
    return o.s;
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
    else report_status(g_main, s, 0, why);
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
    figures_free(g_figs);
    g_figs = NULL;
    while (g_queue) {
        job *n = g_queue->next;
        job_free(g_queue);
        g_queue = n;
    }
    clear_pending();
    free_conflicts(g_conflicts, g_n_conflicts);
    g_conflicts = NULL;
    g_n_conflicts = 0;
    g_last_restore[0] = 0;
    g_sb_state[0] = 0;
    g_n_check_offered = 0;
    g_gone_new = 0;
    SetWindowTextW(g_search, L"");
    end_search();
    rebuild_tree();
    show_detail();
    refresh_pages();
    SetWindowTextW(g_main, APP_NAME);
    update_drives_label();
    show_read_back(0);
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
    swprintf(title, MAX_PATH * 2 + 32, T(L"%ls - BRODALF"), base ? base + 1 : g_cat_path);
    SetWindowTextW(g_main, title);
    set_busy(0);
    if (g_opened_by_recovery) {
        g_opened_by_recovery = 0;
        offer_new_passphrase(g_main);
    }
    update_drives_label();
    rebuild_tree();
    show_detail();
    refresh_pages();
    count_ctx c = {0};
    bd_list_sources(g_cat, count_source, &c);
    if (recovered)
        log_append(T(L"Database recovered from a backup drive. Scanning your backup roots to see whether they are on this PC "
                   L"and what changed since that copy was saved."));
    else if (created)
        log_append(T(L"New database. Scanning your backup roots for the first time. Nothing is backed up until you press Back up."));
    else
        log_append(T(L"Database opened. Scanning your backup roots to find what changed since the last scan. Nothing is backed "
                   L"up until you press Back up."));
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
        log_append(T(L"It has no backup roots yet: use Add backup root to choose what to back up."));
    }
}

/* Open the database at path in place of the one open now. 1 if it is open. */
static int recover_database_ui(void);
static wchar_t g_damaged_db[MAX_PATH * 2]; /* a database file found damaged: not offered as the last one */

/* A database file that cannot be read: say so, and offer the copy that
 * every backup drive keeps. CHOICE_RECOVER, CHOICE_OPEN or IDCANCEL. */
static int damaged_database(const wchar_t *path, const wchar_t *why)
{
    wchar_t body[MAX_PATH * 2 + 400];
    swprintf(body, MAX_PATH * 2 + 400, T(L"BRODALF cannot read %ls. Each of your backup drives keeps a copy of the database, and "
                                        L"BRODALF can recover the newest one."), path);
    TASKDIALOG_BUTTON b[2] = {
        {CHOICE_RECOVER, T(L"Recover it from a backup drive... (recommended)\nStart from the newest copy on your drives")},
        {CHOICE_OPEN, T(L"Open another database...")},
    };
    TASKDIALOGCONFIG tc;
    memset(&tc, 0, sizeof(tc));
    tc.cbSize = sizeof(tc);
    tc.hwndParent = g_main;
    tc.hInstance = g_inst;
    tc.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
    tc.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    tc.pszWindowTitle = APP_NAME;
    tc.pszMainIcon = TD_ERROR_ICON;
    tc.pszMainInstruction = T(L"This database file is damaged");
    tc.pszContent = body;
    tc.pszExpandedInformation = why;
    tc.pszExpandedControlText = T(L"Hide the details");
    tc.pszCollapsedControlText = T(L"Show the details");
    tc.cButtons = 2;
    tc.pButtons = b;
    int pressed = IDCANCEL;
    if (FAILED(TaskDialogIndirect(&tc, &pressed, NULL, NULL))) return IDCANCEL;
    return pressed;
}

static int try_open(const wchar_t *path)
{
    if (g_cat && _wcsicmp(path, g_cat_path) == 0) return 1;
    char *p = narrow(path);
    if (!p) return 0;
    bd_catalog *nc = NULL;
    bd_status s = open_maybe_encrypted(p, &nc);
    free(p);
    if (s == BD_ERR_PASSPHRASE) return 0; /* declined */
    if (s == BD_ERR_FORMAT && strstr(bd_open_error(), " is damaged")) {
        wchar_t *why = widen(bd_open_error());
        wcsncpy(g_damaged_db, path, MAX_PATH * 2 - 1);
        int c = damaged_database(path, why);
        free(why);
        if (c == CHOICE_RECOVER) return recover_database_ui();
        if (c == CHOICE_OPEN) {
            wchar_t other[MAX_PATH * 2] = L"";
            return open_dialog(other, MAX_PATH * 2) && try_open(other);
        }
        return 0;
    }
    if (s != BD_OK) {
        show_open_error(s, T(L"The database could not be opened."));
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
    INT_PTR themed;
    if (theme_dialog(dlg, msg, wp, lp, &themed)) return themed;
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
            char *folder = pick_folder(dlg, T(L"Choose a folder to back up (a backup root)."));
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
    if (r == IDOK && g_newdb.n > 0 && save_dialog(path, MAX_PATH * 2, T(L"Choose where to keep the new database file"))) {
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
                swprintf(msg, 2048, T(L"%ls was not added: %ls"), w ? w : L"", e ? e : L"");
                MessageBoxW(g_main, msg, APP_NAME, MB_ICONWARNING);
                free(e);
                free(w);
            }
            bd_catalog_save(g_cat);
            database_opened(path, 1, 0);
            ok = 1;
        } else {
            show_open_error(s, T(L"The database could not be created."));
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

/* A backup drive can be a folder (a share, or a folder on another disk),
 * which a look at the top of each drive does not reach: with a database
 * open, also look where each of its drives was last seen. */
static int known_root_cb(void *ctx, const bd_media_info *m)
{
    if (m->connected && !m->secondary && !m->lost_ms && m->last_root && *m->last_root)
        bd_find_catalog_copies(m->last_root, found_copy_cb, ctx);
    return 0;
}

static void find_everywhere(found_list *l)
{
    find_on_all_drives(l);
    if (g_cat && !g_busy) bd_list_media(g_cat, known_root_cb, l);
}

/* Nothing found: plug in a drive and look again, or choose the folder. */
#define FOUND_AGAIN 510
#define FOUND_CHOOSE 511
static int ask_where_found(void)
{
    TASKDIALOG_BUTTON b[2] = {{FOUND_AGAIN, NULL}, {FOUND_CHOOSE, NULL}};
    b[0].pszButtonText = T(L"Look again");
    b[1].pszButtonText = T(L"Choose the folder...");
    TASKDIALOGCONFIG tc;
    memset(&tc, 0, sizeof(tc));
    tc.cbSize = sizeof(tc);
    tc.hwndParent = g_main;
    tc.hInstance = g_inst;
    tc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
    tc.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    tc.pszWindowTitle = APP_NAME;
    tc.pszMainIcon = TD_INFORMATION_ICON;
    tc.pszMainInstruction = T(L"No copy of a BRODALF database was found.");
    tc.pszContent = g_cat ? T(L"BRODALF looked at the top of every drive plugged into this PC, and in the backup drives of the database "
                              L"that is open. Every backup puts the newest copy of the database in the BRODALF folder of its drive.\n\n"
                              L"Plug in one of your backup drives and press Look again. If you back up to a folder rather than a whole "
                              L"drive, press Choose the folder and pick it.")
                          : T(L"BRODALF looked at the top of every drive plugged into this PC. Every backup puts the newest copy of the "
                              L"database in the BRODALF folder of its drive.\n\nPlug in one of your backup drives and press Look again. "
                              L"If you back up to a folder rather than a whole drive, press Choose the folder and pick it.");
    tc.cButtons = 2;
    tc.pButtons = b;
    tc.nDefaultButton = FOUND_AGAIN;
    int pressed = IDCANCEL;
    if (FAILED(TaskDialogIndirect(&tc, &pressed, NULL, NULL))) return IDCANCEL;
    return pressed;
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
        if (l->items[i].revision > 0) swprintf(rev, 64, T(L"revision %lld"), (long long)l->items[i].revision);
        else swprintf(rev, 64, T(L"saved by BRODALF 0.6 or older"));
        swprintf(text[i], MAX_PATH * 2 + 160, T(L"Database saved %ls (%ls)\n%ls"), when, rev, wp ? wp : L"");
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
    tc.pszMainInstruction = T(L"Your drives hold copies of more than one database. Which one is yours?");
    tc.pszContent = T(L"Each is the newest copy found of that database.");
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
    find_everywhere(&l);
    while (!l.n) {
        int pressed = ask_where_found();
        if (pressed == FOUND_AGAIN) {
            find_everywhere(&l);
            continue;
        }
        if (pressed != FOUND_CHOOSE) return 0;
        char *dir = pick_folder(g_main, T(L"Choose the backup drive, or the folder that holds the copy of your database."));
        if (!dir) continue;
        bd_find_catalog_copies(dir, found_copy_cb, &l);
        free(dir);
        if (!l.n) MessageBoxW(g_main, T(L"No copy of a BRODALF database was found there."), APP_NAME, MB_ICONINFORMATION);
    }
    int pick = pick_found(&l), ok = 0;
    wchar_t dest[MAX_PATH * 2] = L"";
    if (pick >= 0 && save_dialog(dest, MAX_PATH * 2, T(L"Choose where to keep your database on this PC"))) {
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
            /* A database from a drive may hold anything: say if some stored
             * paths would lead outside the folder a restore writes to. */
            int64_t unsafe = 0;
            if (bd_check_stored_paths(g_cat, ui_log, NULL, &unsafe) == BD_OK && unsafe) {
                wchar_t msg[600];
                swprintf(msg, 600, TN(unsafe, L"%lld file in this database has a stored path that would lead outside the folder you restore to. "
                                              L"A restore leaves it out and lists it as failed; the log names it.",
                                      L"%lld files in this database have stored paths that would lead outside the folder you restore to. "
                                      L"A restore leaves them out and lists them as failed; the log names the first 20."),
                         (long long)unsafe);
                MessageBoxW(g_main, msg, APP_NAME, MB_ICONWARNING);
            }
        } else if (s != BD_ERR_PASSPHRASE) {
            show_open_error(s, T(L"The database could not be recovered."));
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
        wchar_t last[MAX_PATH * 2], missing[MAX_PATH * 2] = L"";
        reg_get_last(last, MAX_PATH * 2);
        /* The last database is gone: say so, and offer to look for it. */
        if (last[0] && GetFileAttributesW(last) == INVALID_FILE_ATTRIBUTES) {
            wcscpy(missing, last);
            last[0] = 0;
        }
        if (last[0] && ((g_cat && _wcsicmp(last, g_cat_path) == 0) || _wcsicmp(last, g_damaged_db) == 0)) last[0] = 0;
        if (asked) choice = ask_database_choice(last, missing);
        wchar_t path[MAX_PATH * 2] = L"";
        if (choice == CHOICE_LAST && last[0]) {
            if (try_open(last)) return;
        } else if (choice == CHOICE_FIND && missing[0]) {
            wchar_t dir[MAX_PATH * 2];
            wcscpy(dir, missing);
            wchar_t *cut = wcsrchr(dir, L'\\');
            if (cut) *cut = 0;
            if (GetFileAttributesW(dir) == INVALID_FILE_ATTRIBUTES) dir[0] = 0;
            if (open_dialog_in(path, MAX_PATH * 2, dir) && try_open(path)) return;
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

/* Windows' message font at g_dpi. */
static void make_fonts(void)
{
    NONCLIENTMETRICSW ncm;
    memset(&ncm, 0, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);
    LOGFONTW lf;
    if (p_spi_for_dpi && p_spi_for_dpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, (UINT)g_dpi)) {
        lf = ncm.lfMessageFont;
    } else {
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        lf = ncm.lfMessageFont;
        if (g_sys_dpi > 0 && g_sys_dpi != g_dpi) lf.lfHeight = MulDiv(lf.lfHeight, g_dpi, g_sys_dpi);
    }
    g_font = CreateFontIndirectW(&lf);
    lf.lfItalic = TRUE;
    g_font_italic = CreateFontIndirectW(&lf);
    lf.lfItalic = FALSE;
    lf.lfStrikeOut = TRUE;
    g_font_strike = CreateFontIndirectW(&lf);
    lf.lfStrikeOut = FALSE;
    lf.lfWeight = FW_BOLD;
    g_font_bold = CreateFontIndirectW(&lf);
    /* For the header, the warnings and the job panel. */
    lf.lfWeight = FW_SEMIBOLD;
    if (g_font_semi) DeleteObject(g_font_semi);
    g_font_semi = CreateFontIndirectW(&lf);
    lf.lfHeight = MulDiv(lf.lfHeight, 4, 3);
    if (g_font_title) DeleteObject(g_font_title);
    g_font_title = CreateFontIndirectW(&lf);
}

/* ---- Helper modes ------------------------------------------------------- */

/* brodalf.exe --shadow-copy <out dir> <list> <hash>: started as
 * administrator by the main window. The checks (the list's hash, the
 * backup roots, a private folder for the copies) are in bd_shadow_helper. */
static int shadow_helper(const wchar_t *out, const wchar_t *list, const wchar_t *hash)
{
    char *o = narrow(out), *l = narrow(list), *h = narrow(hash);
    char err[512] = "";
    int got = o && l && h ? bd_shadow_helper(o, l, h, err, sizeof(err)) : -1;
    bd_applog("shadow copy helper: %d copied%s%s", got < 0 ? 0 : got, err[0] ? ": " : "", err);
    free(o);
    free(l);
    free(h);
    return got < 0 ? 1 : 0;
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

    /* --shadow-copy is BRODALF asking itself, as administrator, to copy
     * files in use from a shadow copy, after the person said yes. It runs
     * before CoInitializeEx: VSS needs the multithreaded COM apartment and
     * hangs in a single-threaded one. */
    if (argc >= 2 && wcscmp(argv[1], L"--shadow-copy") == 0) {
        int rc = argc >= 5 ? shadow_helper(argv[2], argv[3], argv[4]) : 1;
        LocalFree(argv);
        return rc;
    }
    /* Otherwise the only argument is a catalog to open. Anything else that
     * starts with -- is not one, so BRODALF exits without opening a window. */
    if (argc >= 2 && wcsncmp(argv[1], L"--", 2) == 0) {
        bd_applog("unknown option on the command line; nothing done");
        LocalFree(argv);
        return 1;
    }
    lang_load(); /* before any text is shown */
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_TREEVIEW_CLASSES | ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES |
                                                 ICC_PROGRESS_CLASS | ICC_DATE_CLASSES | ICC_TAB_CLASSES};
    InitCommonControlsEx(&icc);
    theme_load();
    g_hc = high_contrast();
    g_dark = theme_want_dark();
    theme_app_mode();
    dpi_load();
    HDC screen = GetDC(NULL);
    g_dpi = g_sys_dpi = GetDeviceCaps(screen, LOGPIXELSY);
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
    /* The app's own icon, at the sizes the title bar and Alt+Tab want. */
    wc.hIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_BRODALF), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_BRODALF), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.lpszClassName = L"BrodalfMain";
    RegisterClassExW(&wc);
    register_page_class();

    int made_at = g_dpi;
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, APP_NAME, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, S(1240), S(820),
                                NULL, NULL, inst, NULL);
    if (!hwnd) { LocalFree(argv); return 1; }
    /* It opened on a monitor at another DPI (WM_CREATE took that on). */
    if (g_dpi != made_at) SetWindowPos(hwnd, NULL, 0, 0, S(1240), S(820), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    fit_to_work_area(hwnd);
    theme_window(hwnd);
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);
    update_drives_label();
    say_interrupted();

    /* A database named on the command line (a double-click) is tried
     * first; otherwise BRODALF asks which to use. With none open the
     * window stays, with only File and Help working. */
    g_ui_depth++;
    if (!(argc > 1 && argv[1][0] && try_open(argv[1]))) open_database_ui(0);
    g_ui_depth--;
    LocalFree(argv);
    start_next_job();

    /* The menus' shortcuts, then Tab and the other dialog keys, for the
     * main window and everything in it. Dialogs and message boxes have
     * their own loop. */
    g_accel = make_accels();
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (g_main && (m.hwnd == g_main || IsChild(g_main, m.hwnd))) {
            if (g_accel && TranslateAcceleratorW(g_main, g_accel, &m)) continue;
            if (tab_key(&m)) continue;
            if (IsDialogMessageW(g_main, &m)) continue;
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (g_accel) DestroyAcceleratorTable(g_accel);

    if (g_cat) {
        bd_catalog_save(g_cat);
        bd_catalog_close(g_cat);
    }
    CoUninitialize();
    return 0;
}
