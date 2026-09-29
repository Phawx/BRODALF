/* The app log and error reports. A report is a text file the user can post
 * as a GitHub issue themselves: nothing is sent anywhere. */
#include "brodalf.h"
#include "internal.h"
#include "platform.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/utsname.h>
#include <unistd.h>
#endif

#ifndef BRODALF_VERSION
#define BRODALF_VERSION "0.0.0"
#endif
#ifndef BRODALF_BUILD_ID
#define BRODALF_BUILD_ID ""
#endif
#ifndef BRODALF_ISSUES_URL
#define BRODALF_ISSUES_URL "https://github.com/Phawx/BRODALF/issues"
#endif

#define LOG_LIMIT (1024 * 1024)
#define LOG_TAIL (32 * 1024)

static char *g_log_path;

const char *bd_version(void)
{
    static char v[64];
    if (!v[0]) {
        if (BRODALF_BUILD_ID[0]) snprintf(v, sizeof(v), "%s (%.7s)", BRODALF_VERSION, BRODALF_BUILD_ID);
        else snprintf(v, sizeof(v), "%s", BRODALF_VERSION);
    }
    return v;
}

/* ---- Environment ------------------------------------------------------- */

/* getenv as UTF-8, malloc'd, NULL if unset or empty. */
static char *env(const char *name)
{
#ifdef _WIN32
    wchar_t wname[64], buf[1024];
    MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, 64);
    DWORD n = GetEnvironmentVariableW(wname, buf, 1024);
    if (n == 0 || n >= 1024) return NULL;
    char out[3072];
    if (WideCharToMultiByte(CP_UTF8, 0, buf, -1, out, sizeof(out), NULL, NULL) <= 0) return NULL;
    return bd_strdup(out);
#else
    const char *v = getenv(name);
    return v && *v ? bd_strdup(v) : NULL;
#endif
}

char *bd_applog_default_path(void)
{
    char *p = env("BRODALF_LOG");
    if (p) return p;
#ifdef _WIN32
    char *base = env("LOCALAPPDATA");
    if (!base) base = env("APPDATA");
    if (!base) {
        char tmp[1024];
        if (bd_temp_dir(tmp, sizeof(tmp)) != 0) return NULL;
        base = bd_strdup(tmp);
    }
    p = bd_sprintf("%s\\BRODALF\\brodalf.log", base);
#else
    char *base = env("XDG_STATE_HOME");
    if (base) p = bd_sprintf("%s/brodalf/brodalf.log", base);
    else {
        base = env("HOME");
        p = base ? bd_sprintf("%s/.local/state/brodalf/brodalf.log", base) : bd_strdup("/tmp/brodalf.log");
    }
#endif
    free(base);
    return p;
}

static char *dir_of(const char *path)
{
    char *d = bd_strdup(path);
    if (!d) return NULL;
    char *cut = NULL;
    for (char *c = d; *c; c++)
        if (*c == '/' || *c == '\\') cut = c;
    if (cut) *cut = '\0';
    else d[0] = '\0';
    return d;
}

/* ---- App log ----------------------------------------------------------- */

bd_status bd_applog_open(const char *path)
{
    free(g_log_path);
    g_log_path = NULL;
    if (!path || !*path) return BD_OK;
    char *dir = dir_of(path);
    if (dir && dir[0]) bd_mkdirs(dir);
    free(dir);
    FILE *f = bd_fopen(path, "ab");
    if (!f) return BD_ERR_IO;
    fclose(f);
    g_log_path = bd_strdup(path);
    return g_log_path ? BD_OK : BD_ERR_NOMEM;
}

const char *bd_applog_path(void)
{
    return g_log_path;
}

void bd_applog(const char *fmt, ...)
{
    if (!g_log_path) return;
    bd_stat_t st;
    if (bd_stat(g_log_path, &st) == 0 && st.size > LOG_LIMIT) {
        char *old = bd_sprintf("%s.old", g_log_path);
        if (old) bd_rename_replace(g_log_path, old);
        free(old);
    }
    FILE *f = bd_fopen(g_log_path, "ab");
    if (!f) return;
    time_t now = time(NULL);
    struct tm tm;
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm);
    fprintf(f, "%s  ", stamp);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
#ifdef _WIN32
    fputs("\r\n", f);
#else
    fputc('\n', f);
#endif
    fclose(f);
}

/* The last LOG_TAIL bytes of the log, starting at a whole line. */
static char *log_tail(void)
{
    if (!g_log_path) return NULL;
    FILE *f = bd_fopen(g_log_path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    long start = size > LOG_TAIL ? size - LOG_TAIL : 0;
    fseek(f, start, SEEK_SET);
    char *buf = malloc((size_t)(size - start) + 1);
    size_t n = buf ? fread(buf, 1, (size_t)(size - start), f) : 0;
    fclose(f);
    if (!buf) return NULL;
    buf[n] = '\0';
    char *from = buf;
    if (start > 0) {
        char *nl = strchr(buf, '\n');
        from = nl ? nl + 1 : buf + n;
    }
    /* Drop carriage returns so the report reads the same everywhere. */
    char *w = buf;
    for (char *r = from; *r; r++)
        if (*r != '\r') *w++ = *r;
    *w = '\0';
    return buf;
}

/* ---- Redaction --------------------------------------------------------- */

typedef struct { char *s; size_t len, cap; } sbuf;

static void sb_add(sbuf *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 256;
        while (cap < b->len + n + 1) cap *= 2;
        char *p = realloc(b->s, cap);
        if (!p) return;
        b->s = p;
        b->cap = cap;
    }
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = '\0';
}

static void sb_str(sbuf *b, const char *s)
{
    sb_add(b, s, strlen(s));
}

static int ci_prefix(const char *s, const char *word, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (!s[i] || tolower((unsigned char)s[i]) != tolower((unsigned char)word[i])) return 0;
    return 1;
}

/* Replace every case-insensitive occurrence of word. Short words are left
 * alone: replacing "al" everywhere would wreck the report. */
static char *replace_word(char *text, const char *word, const char *with)
{
    if (!text || !word) return text;
    size_t n = strlen(word);
    if (n < 3) return text;
    sbuf b = {0};
    for (const char *p = text; *p;) {
        if (ci_prefix(p, word, n)) { sb_str(&b, with); p += n; }
        else sb_add(&b, p++, 1);
    }
    if (!b.s) b.s = bd_strdup("");
    free(text);
    return b.s;
}

static int email_char(char c)
{
    return isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-' || c == '+' || c == '%';
}

static int token_char(char c)
{
    return isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-' || c == '~' || c == '+' || c == '/' || c == '=';
}

/* user@example.com becomes <email>; "Bearer xyz" and "token=xyz" lose xyz. */
static char *replace_secrets(char *text)
{
    sbuf b = {0};
    size_t n = strlen(text);
    for (size_t i = 0; i < n;) {
        if (text[i] == '@' && i > 0 && email_char(text[i - 1]) && i + 1 < n && email_char(text[i + 1])) {
            size_t start = i;
            while (start > 0 && email_char(text[start - 1])) start--;
            size_t end = i + 1;
            int dot = 0;
            while (end < n && email_char(text[end])) { if (text[end] == '.') dot = 1; end++; }
            if (dot && b.len >= i - start) {
                b.len -= i - start;
                if (b.s) b.s[b.len] = '\0';
                sb_str(&b, "<email>");
                i = end;
                continue;
            }
        }
        static const char *keys[] = {"bearer ", "token=", "token\":\"", "code=", "code_verifier="};
        int hit = 0;
        for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
            size_t kn = strlen(keys[k]);
            if (ci_prefix(text + i, keys[k], kn)) {
                sb_add(&b, text + i, kn);
                i += kn;
                if (i < n && token_char(text[i])) {
                    while (i < n && token_char(text[i])) i++;
                    sb_str(&b, "<hidden>");
                }
                hit = 1;
                break;
            }
        }
        if (hit) continue;
        sb_add(&b, text + i, 1);
        i++;
    }
    if (!b.s) b.s = bd_strdup("");
    free(text);
    return b.s;
}

char *bd_redact(const char *text)
{
    char *s = bd_strdup(text ? text : "");
    if (!s) return NULL;
    s = replace_secrets(s);
#ifdef _WIN32
    const char *user_vars[] = {"USERNAME"}, *host_vars[] = {"COMPUTERNAME", "USERDOMAIN"};
#else
    const char *user_vars[] = {"USER"}, *host_vars[] = {"HOSTNAME"};
#endif
    for (size_t i = 0; i < sizeof(user_vars) / sizeof(user_vars[0]); i++) {
        char *v = env(user_vars[i]);
        /* "root" and "user" are not personal and appear in ordinary words. */
        if (v && strcmp(v, "root") != 0 && strcmp(v, "user") != 0) s = replace_word(s, v, "<user>");
        free(v);
    }
    for (size_t i = 0; i < sizeof(host_vars) / sizeof(host_vars[0]); i++) {
        char *v = env(host_vars[i]);
        if (v) s = replace_word(s, v, "<computer>");
        free(v);
    }
#ifndef _WIN32
    char host[256];
    if (gethostname(host, sizeof(host)) == 0) {
        host[sizeof(host) - 1] = '\0';
        if (strcmp(host, "localhost") != 0) s = replace_word(s, host, "<computer>");
    }
#endif
    return s;
}

/* ---- Report ------------------------------------------------------------ */

static void os_name(char *out, size_t cap)
{
#ifdef _WIN32
    typedef LONG(WINAPI * rtl_get_version_fn)(OSVERSIONINFOW *);
    OSVERSIONINFOW v;
    memset(&v, 0, sizeof(v));
    v.dwOSVersionInfoSize = sizeof(v);
    rtl_get_version_fn fn = (rtl_get_version_fn)(void (*)(void))GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    if (fn && fn(&v) == 0)
        snprintf(out, cap, "Windows %lu.%lu build %lu", (unsigned long)v.dwMajorVersion, (unsigned long)v.dwMinorVersion, (unsigned long)v.dwBuildNumber);
    else
        snprintf(out, cap, "Windows");
    /* Wine says it is Windows; say so, since problems there are not real ones. */
    if (GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version")) {
        size_t n = strlen(out);
        snprintf(out + n, cap - n, " (Wine)");
    }
#else
    struct utsname u;
    if (uname(&u) == 0) snprintf(out, cap, "%s %s %s", u.sysname, u.release, u.machine);
    else snprintf(out, cap, "unknown");
#endif
}

/* The report with at most keep_log bytes of log (from the end). */
static char *build_body(const char *what, const char *log, size_t keep_log)
{
    char os[128];
    os_name(os, sizeof(os));
    sbuf b = {0};
    sb_str(&b, "**What went wrong**\n\n");
    sb_str(&b, what && *what ? what : "(BRODALF did not record an error message.)");
    sb_str(&b, "\n\n**What were you doing when it happened?**\n\n\n\n**Version:** BRODALF ");
    sb_str(&b, bd_version());
    sb_str(&b, " on ");
    sb_str(&b, os);
    sb_str(&b, "\n");
    size_t len = log ? strlen(log) : 0;
    if (len && keep_log) {
        const char *from = log;
        if (len > keep_log) {
            from = log + len - keep_log;
            const char *nl = strchr(from, '\n');
            from = nl ? nl + 1 : log + len;
        }
        if (*from) {
            sb_str(&b, "\n<details><summary>Recent log</summary>\n\n```\n");
            sb_str(&b, from);
            if (b.len && b.s[b.len - 1] != '\n') sb_str(&b, "\n");
            sb_str(&b, "```\n</details>\n");
        }
    }
    if (!b.s) return NULL;
    char *out = bd_redact(b.s);
    free(b.s);
    return out;
}

char *bd_report_body(const char *what_happened)
{
    char *log = log_tail();
    char *body = build_body(what_happened, log, LOG_TAIL);
    free(log);
    return body;
}

const char *bd_issues_url(void)
{
    return BRODALF_ISSUES_URL;
}

char *bd_report_save(const char *what_happened)
{
    char *body = bd_report_body(what_happened);
    if (!body) return NULL;
    char *dir = NULL;
    if (g_log_path) {
        char *logdir = dir_of(g_log_path);
        dir = logdir && logdir[0] ? bd_sprintf("%s%creports", logdir, BD_SEP) : NULL;
        free(logdir);
    }
    if (!dir) {
        char tmp[1024];
        if (bd_temp_dir(tmp, sizeof(tmp)) == 0) dir = bd_strdup(tmp);
    }
    if (!dir) { free(body); return NULL; }
    bd_mkdirs(dir);

    time_t now = time(NULL);
    struct tm tm;
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
    char *path = bd_sprintf("%s%cbrodalf-error-%s.txt", dir, BD_SEP, stamp);
    free(dir);
    FILE *f = path ? bd_fopen(path, "wb") : NULL;
    if (!f) { free(path); free(body); return NULL; }
    fprintf(f,
            "BRODALF error report\n"
            "====================\n"
            "\n"
            "To report this problem to the BRODALF developers:\n"
            "\n"
            "  1. Go to %s and click \"New issue\".\n"
            "     You need a free GitHub account.\n"
            "  2. Give it a short title, like \"Backup failed\".\n"
            "  3. Copy everything below the dashed line into the description,\n"
            "     or drag this file onto it.\n"
            "  4. Say what you were doing when it happened, then click \"Create\".\n"
            "\n"
            "Your user name, computer name, email addresses and sign-in tokens have\n"
            "been replaced below. File and folder names are still there, so read it\n"
            "over before you post it.\n"
            "\n"
            "--------------------------------------------------------------------------\n"
            "\n%s",
            BRODALF_ISSUES_URL, body);
    int bad = ferror(f);
    if (fclose(f) != 0) bad = 1;
    free(body);
    if (bad) { bd_remove(path); free(path); return NULL; }
    return path;
}
