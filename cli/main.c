/* brodalf-cli: everything the window app does, from the command line. Each
 * command runs once, when you run it; nothing starts by itself. --json
 * prints the result as one JSON object for scripts. */
#include "brodalf.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <io.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <termios.h>
#include <unistd.h>
#endif

static void usage(void)
{
    puts("usage: brodalf-cli <command> <catalog.brodalf> [arguments] [--json]\n"
         "\n"
         "The catalog and its backup roots\n"
         "  new      <catalog> [folder]...             create a new catalog, with the folders to back up\n"
         "  add      <catalog> <folder>...             add backup roots (folders to back up)\n"
         "  roots    <catalog>                         list the backup roots\n"
         "  root-stop   <catalog> <root>               stop backing up a root (its copies stay and still restore)\n"
         "  root-resume <catalog> <root>               back a stopped root up again\n"
         "  root-move   <catalog> <root> <folder>      the root is somewhere else now (a new PC, another drive letter)\n"
         "  scan     <catalog> [--bitrot [--accept ROOT/PATH]...] [--shadow-copy]\n"
         "                                             record files, sizes and checksums; --bitrot also reads\n"
         "                                             unchanged files to find ones damaged on this PC, and\n"
         "                                             --accept records a listed one as changed on purpose.\n"
         "                                             --shadow-copy (Windows, as administrator): files in use\n"
         "                                             by another program are read from a shadow copy of\n"
         "                                             their drive instead of being skipped\n"
         "  deleted  <catalog> [--new] [--root NAME]   files deleted from their folders, and the drives holding them\n"
         "  deleted-ack    <catalog> <root> <path> | --all\n"
         "                                             mark deleted files as seen; they stay in the catalog\n"
         "  deleted-forget <catalog> <root> <path> | --all [--yes]\n"
         "                                             stop tracking deleted files (their copies stay on the drives)\n"
         "  skip     <catalog> [--root NAME] [--add PATTERN | --set FILE | --reset]\n"
         "                                             show or change what scans leave out (the catalog's list,\n"
         "                                             or one root's own list)\n"
         "\n"
         "Drives\n"
         "  drive    <catalog> <root> <label> [--encrypt [--hide-names]] [--location TEXT]\n"
         "                                             set up a drive or folder as a backup drive; --hide-names\n"
         "                                             also hides file and folder names on an encrypted drive\n"
         "                                             (a catalog with such a drive needs BRODALF 0.9.0 or newer)\n"
         "  drives   <catalog>                         every drive: make, model, serial, health, where it is kept\n"
         "  status   <catalog> [label]                 where each drive stands: what it holds, its last backup and check\n"
         "  drive-location <catalog> <label> [text]    say where a drive is kept (no text clears it)\n"
         "  drive-rename   <catalog> <label> <new label>\n"
         "  drive-lost     <catalog> <label> lost|broken [--yes]\n"
         "                                             its copies stop counting; what it held is backed up again\n"
         "  drive-found    <catalog> <label>           it turned up again: its copies count again\n"
         "\n"
         "Backup\n"
         "  preview  <catalog> <root>                  what a backup to the drive would do, doing none of it\n"
         "  changed  <catalog> <label>                 files whose contents changed since their copy on the drive\n"
         "  backup   <catalog> <root>... [--parallel] [--shadow-copy]\n"
         "                                             copy everything the drive does not hold yet; nothing on the\n"
         "                                             drive is overwritten, moved or deleted. With several drives,\n"
         "                                             one after another, or all at once with --parallel or the\n"
         "                                             parallel_copies option (each file is read once).\n"
         "                                             --shadow-copy (one drive): as for scan\n"
         "  pending  <catalog> [label] [--all]         what the next backup copies: to that drive, or every file\n"
         "                                             short of the copies you want (first 50; --all for all)\n"
         "  check    <catalog> <root>                  read every copy on the drive back and check it\n"
         "  verify   <catalog> <root> [--days N]       read back only copies not read back in N days\n"
         "  compress <catalog> <root> [on|off]        Windows' own file compression for BRODALF's folder on the drive:\n"
         "                                             say whether it is on, or turn it on or off (NTFS drives only)\n"
         "  sets     <catalog>                         progress toward each full set of backups\n"
         "  at-risk  <catalog> [--root NAME] [--all]   files short of the copies you want, and which drive helps\n"
         "  need     <catalog>                         what needs backing up, and the drive to plug in\n"
         "  largest  <catalog> [N]                     the N largest files (default 20)\n"
         "  target   <catalog> [copies places]         show or set how many copies, in how many places\n"
         "\n"
         "Starred files and extra places\n"
         "  star     <catalog> <root> [path]           star a file or folder (no path: the whole root)\n"
         "  unstar   <catalog> <root> [path]\n"
         "  stars    <catalog>                         the starred files and folders\n"
         "  place    <catalog> <folder> <description> [--limit SIZE] [--encrypt [--hide-names]]\n"
         "                                             a folder (a Dropbox or OneDrive folder, a network share)\n"
         "                                             that also gets the starred files; say what and where it is.\n"
         "                                             Its copies never count toward the copies you want\n"
         "  place-limit <catalog> <label> <SIZE|none>  copy there only up to SIZE in all (like 2GB)\n"
         "  places   <catalog>                         each extra place and how its starred files stand\n"
         "  (back up to an extra place with backup, like a drive)\n"
         "\n"
         "Encrypting chosen folders\n"
         "  encrypt-folder <catalog> <root> [path]     encrypt that file or folder's copies on every drive\n"
         "  plain-folder   <catalog> <root> [path]     new copies of it are plain files again\n"
         "  encrypted-folders <catalog>                the folders set to be encrypted, and what it costs in time\n"
         "\n"
         "Finding and restoring files\n"
         "  tree     <catalog> [--drive ROOT]...       the folder tree and what is available\n"
         "  search   <catalog> <words>...              find files and folders by name, and the drives holding them\n"
         "  versions <catalog> <root> <path> [--drive ROOT]...\n"
         "                                             every version of a file and its copies\n"
         "  restore  <catalog> <dest> [--root NAME] [--path REL] [--deleted] --drive ROOT...\n"
         "                                             restore files as they are now (or, with --deleted, the\n"
         "                                             deleted ones as last seen); nothing is overwritten\n"
         "  restore-version <catalog> <root> <path> <version> <dest folder> --drive ROOT...\n"
         "                                             restore one version of one file\n"
         "  restore-plan <catalog> [--root NAME] [--path REL] [--dest DIR] [--deleted] [--drive ROOT]...\n"
         "                                             which drives a restore needs, in order\n"
         "  export   <catalog> <file.csv>              every file and the drives holding it, for a spreadsheet\n"
         "\n"
         "A new PC, passphrases and settings\n"
         "  recover  <drive>... <new catalog> [--id ID]\n"
         "                                             start again from the newest catalog copy on the drives\n"
         "                                             (or give one catalog copy file instead of drives)\n"
         "  recovery-key <catalog> [--sheet FILE.html] make a recovery key (and print the recovery sheet)\n"
         "  sheet    <catalog> <file.html>             write the recovery sheet to print\n"
         "  passphrase <catalog>                       set or change the passphrase\n"
         "  encrypt-catalog <catalog> on|off           encrypt the .brodalf file itself\n"
         "  option   <catalog> [name value]            show or set options (run it to see them)\n"
         "\n"
         "BRODALF starts nothing by itself: every command runs once, when you run it.\n"
         "Ctrl+C stops a long command between files; what it finished is kept.\n"
         "\n"
         "--drive connects a drive for this command. Files are shown as available\n"
         "only when a correct copy is on a connected drive. A root is named as\n"
         "`roots` lists it. --json prints the result as JSON (messages go to stderr).\n"
         "\n"
         "When something fails, an error report is saved with instructions for\n"
         "posting it as a GitHub issue. The app log is kept in the file named by\n"
         "BRODALF_LOG, or %LOCALAPPDATA%\\BRODALF\\brodalf.log (Windows) and\n"
         "~/.local/state/brodalf/brodalf.log (elsewhere).\n"
         "\n"
         "Passphrases are asked for on the terminal, or taken from the environment\n"
         "variables BRODALF_PASSPHRASE and (when setting one) BRODALF_NEW_PASSPHRASE.");
}

/* ---- JSON output --------------------------------------------------------- */

static int g_json;
static int j_depth;
static int j_first[64] = {1};

static void j_sep(void)
{
    if (!j_first[j_depth]) putchar(',');
    j_first[j_depth] = 0;
}

static void j_text(const char *s)
{
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        if (*p == '"' || *p == '\\') { putchar('\\'); putchar(*p); }
        else if (*p == '\n') fputs("\\n", stdout);
        else if (*p == '\r') fputs("\\r", stdout);
        else if (*p == '\t') fputs("\\t", stdout);
        else if (*p < 0x20) printf("\\u%04x", *p);
        else putchar(*p);
    }
    putchar('"');
}

static void j_key(const char *k)
{
    j_sep();
    if (k) { j_text(k); putchar(':'); }
}

static void j_open(const char *k, int c)
{
    j_key(k);
    putchar(c);
    if (j_depth < 63) j_first[++j_depth] = 1;
}

static void j_close(int c)
{
    putchar(c);
    if (j_depth > 0) j_depth--;
    if (j_depth == 0) { putchar('\n'); j_first[0] = 1; }
}

#define J_OBJ(k) j_open(k, '{')
#define J_END() j_close('}')
#define J_ARR(k) j_open(k, '[')
#define J_END_ARR() j_close(']')

static void j_int(const char *k, int64_t v)
{
    j_key(k);
    printf("%lld", (long long)v);
}

static void j_str(const char *k, const char *v)
{
    j_key(k);
    j_text(v);
}

static void j_bool(const char *k, int v)
{
    j_key(k);
    fputs(v ? "true" : "false", stdout);
}

/* Messages for the person; with --json they go to stderr so stdout stays JSON. */
static FILE *msg_out(void) { return g_json ? stderr : stdout; }

/* ---- Progress on the terminal -------------------------------------------- */

static int g_progress_shown;

static int64_t now_ms(void)
{
#ifdef _WIN32
    return (int64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

static void clear_progress(void)
{
    if (!g_progress_shown) return;
    fprintf(stderr, "\r%79s\r", "");
    g_progress_shown = 0;
}

/* Sizes as Explorer shows them: 1 KB is 1024 bytes. */
static void human_bytes(int64_t n, char *out, size_t cap)
{
    const char *units[] = {"bytes", "KB", "MB", "GB", "TB", "PB"};
    double v = (double)n;
    int u = 0;
    while (v >= 1024 && u < 5) { v /= 1024; u++; }
    snprintf(out, cap, u ? "%.1f %s" : "%.0f %s", v, units[u]);
}

static void human_time(int64_t s, char *out, size_t cap)
{
    if (s < 90) snprintf(out, cap, "%lld s", (long long)s);
    else if (s < 90 * 60) snprintf(out, cap, "%lld min", (long long)(s + 30) / 60);
    else snprintf(out, cap, "%lld h %02lld min", (long long)s / 3600, (long long)(s % 3600) / 60);
}

typedef struct { int64_t start_ms, last_ms; const char *phase; } progress_state;

/* One line on stderr, rewritten in place: files and bytes done, the rate,
 * an estimate of the time left, and how far into a big file it is. */
static void on_progress(void *ctx, const bd_progress *p)
{
    progress_state *s = ctx;
    int64_t t = now_ms();
    if (p->done) { clear_progress(); s->phase = NULL; return; }
    if (!s->phase || strcmp(s->phase, p->phase) != 0) { s->phase = p->phase; s->start_ms = t; s->last_ms = 0; }
    if (t - s->last_ms < 250) return;
    s->last_ms = t;
    char line[256], done_s[32], total_s[32], rate_s[48] = "", left_s[48] = "";
    human_bytes(p->bytes_done, done_s, sizeof(done_s));
    human_bytes(p->bytes_total, total_s, sizeof(total_s));
    double secs = (double)(t - s->start_ms) / 1000.0;
    if (secs >= 2 && p->bytes_done > 0) {
        double rate = (double)p->bytes_done / secs;
        human_bytes((int64_t)rate, rate_s, sizeof(rate_s));
        strcat(rate_s, "/s");
        if (p->bytes_total > p->bytes_done && rate > 0) {
            char hs[24];
            human_time((int64_t)((double)(p->bytes_total - p->bytes_done) / rate) + 1, hs, sizeof(hs));
            snprintf(left_s, sizeof(left_s), ", about %s left", hs);
        }
    }
    if (p->files_total > 0)
        snprintf(line, sizeof(line), "%s: %lld of %lld files, %s of %s", p->phase, (long long)p->files_done,
                 (long long)p->files_total, done_s, total_s);
    else
        snprintf(line, sizeof(line), "%s: %lld files, %s", p->phase, (long long)p->files_done, done_s);
    if (rate_s[0]) snprintf(line + strlen(line), sizeof(line) - strlen(line), ", %s%s", rate_s, left_s);
    if (p->file_bytes_total > 64 * 1024 * 1024 && p->file_bytes_done < p->file_bytes_total)
        snprintf(line + strlen(line), sizeof(line) - strlen(line), ", this file %d%%",
                 (int)(p->file_bytes_done * 100 / p->file_bytes_total));
    line[79] = '\0';
    fprintf(stderr, "\r%-79s", line);
    fflush(stderr);
    g_progress_shown = 1;
}

static void log_line(void *ctx, const char *msg)
{
    (void)ctx;
    clear_progress();
    fprintf(msg_out(), "  %s\n", msg);
    bd_applog("%s", msg);
}

/* Print the error, save a report file and say how to post it. */
static int die(bd_catalog *cat, const char *what)
{
    clear_progress();
    const char *why = cat ? bd_catalog_error(cat) : bd_open_error();
    fprintf(stderr, "brodalf: %s: %s\n", what, why);
    char *line = malloc(strlen(what) + strlen(why) + 3);
    if (line) sprintf(line, "%s: %s", what, why);
    bd_applog("ERROR: %s", line ? line : what);
    char *report = bd_report_save(line ? line : what);
    if (report)
        fprintf(stderr, "An error report was saved to %s\n"
                        "To let the developers know, post it as a new issue at %s\n"
                        "(the report starts with step-by-step instructions).\n", report, bd_issues_url());
    free(report);
    free(line);
    if (cat) bd_catalog_close(cat);
    return 1;
}

/* A mistake in how the command was typed: say so, no report. */
static int refuse(bd_catalog *cat, const char *msg)
{
    clear_progress();
    fprintf(stderr, "brodalf: %s\n", msg);
    if (cat) bd_catalog_close(cat);
    return 1;
}

static int has_flag(int argc, char **argv, const char *name)
{
    for (int i = 0; i < argc; i++)
        if (strcmp(argv[i], name) == 0) return 1;
    return 0;
}

static const char *opt(int argc, char **argv, const char *name)
{
    for (int i = 0; i + 1 < argc; i++)
        if (strcmp(argv[i], name) == 0) return argv[i + 1];
    return NULL;
}

/* The arguments that are not flags (or a flag's value), from index 3 on. */
static int positional(int argc, char **argv, const char **out, int cap)
{
    static const char *const with_value[] = {"--drive", "--root", "--source", "--path", "--dest", "--location", "--days",
                                             "--add", "--set", "--accept", "--sheet", "--id"};
    int n = 0;
    for (int i = 3; i < argc; i++) {
        int takes = 0;
        for (size_t k = 0; k < sizeof(with_value) / sizeof(with_value[0]); k++)
            if (strcmp(argv[i], with_value[k]) == 0) takes = 1;
        if (takes) { i++; continue; }
        if (strncmp(argv[i], "--", 2) == 0) continue;
        if (n < cap) out[n++] = argv[i];
    }
    return n;
}

/* Read a line from the terminal without echo. */
static int read_secret(const char *prompt, char *buf, size_t cap)
{
    fprintf(stderr, "%s", prompt);
    fflush(stderr);
    buf[0] = '\0';
#ifdef _WIN32
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    int console = GetConsoleMode(in, &mode);
    if (console) {
        SetConsoleMode(in, mode & ~(DWORD)ENABLE_ECHO_INPUT);
        wchar_t wbuf[512];
        DWORD n = 0;
        BOOL ok = ReadConsoleW(in, wbuf, 511, &n, NULL);
        SetConsoleMode(in, mode);
        fputc('\n', stderr);
        if (!ok) return -1;
        while (n > 0 && (wbuf[n - 1] == L'\n' || wbuf[n - 1] == L'\r')) n--;
        wbuf[n] = 0;
        int len = WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, buf, (int)cap, NULL, NULL);
        SecureZeroMemory(wbuf, sizeof(wbuf));
        return len > 0 ? 0 : -1;
    }
#else
    struct termios old, quiet_t;
    int tty = tcgetattr(STDIN_FILENO, &old) == 0;
    if (tty) {
        quiet_t = old;
        quiet_t.c_lflag &= ~(tcflag_t)ECHO;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet_t);
    }
#endif
    char *got = fgets(buf, (int)cap, stdin);
#ifndef _WIN32
    if (tty) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
        fputc('\n', stderr);
    }
#endif
    if (!got) return -1;
    buf[strcspn(buf, "\r\n")] = '\0';
    return 0;
}

/* Ask the person to type text back (a drive's name) before something that
 * matters. --yes skips it; without a terminal it is refused. */
static int confirm_typed(int argc, char **argv, const char *question, const char *expect)
{
    if (has_flag(argc, argv, "--yes")) return 1;
    if (!isatty(fileno(stdin))) {
        fprintf(stderr, "brodalf: %s\nAdd --yes to do it without a terminal.\n", question);
        return 0;
    }
    char buf[512];
    fprintf(stderr, "%s\nType \"%s\" to go ahead: ", question, expect);
    fflush(stderr);
    if (!fgets(buf, sizeof(buf), stdin)) return 0;
    buf[strcspn(buf, "\r\n")] = '\0';
    if (strcmp(buf, expect) != 0) { fprintf(stderr, "brodalf: nothing was changed\n"); return 0; }
    return 1;
}

static int get_passphrase(char *buf, size_t cap)
{
    const char *env = getenv("BRODALF_PASSPHRASE");
    if (env) { snprintf(buf, cap, "%s", env); return 0; }
    return read_secret("Passphrase: ", buf, cap);
}

static int get_new_passphrase(char *buf, size_t cap)
{
    const char *env = getenv("BRODALF_NEW_PASSPHRASE");
    if (env) { snprintf(buf, cap, "%s", env); return 0; }
    char again[512];
    if (read_secret("New passphrase (at least 8 characters): ", buf, cap) != 0 ||
        read_secret("Type it again: ", again, sizeof(again)) != 0)
        return -1;
    int same = strcmp(buf, again) == 0;
    memset(again, 0, sizeof(again));
    if (!same) { fprintf(stderr, "brodalf: the passphrases do not match\n"); return -1; }
    fprintf(stderr, "Write it down somewhere safe. Without it, encrypted copies cannot be read.\n");
    return 0;
}

/* Unlock the catalog if it has a passphrase and is locked. 0 on success. */
static int ensure_unlocked(bd_catalog *cat)
{
    if (!bd_catalog_has_passphrase(cat) || bd_catalog_is_unlocked(cat)) return 0;
    char pass[512];
    if (get_passphrase(pass, sizeof(pass)) != 0) return -1;
    bd_status s = bd_catalog_unlock(cat, pass);
    memset(pass, 0, sizeof(pass));
    if (s != BD_OK) { fprintf(stderr, "brodalf: %s\n", bd_catalog_error(cat)); return -1; }
    return 0;
}

/* ---- Looking things up ---------------------------------------------------- */

typedef struct { const char *name; int64_t id; } find_source_ctx;

static int find_source_cb(void *ctx, const bd_source_info *info)
{
    find_source_ctx *f = ctx;
    if (strcmp(info->name, f->name) == 0) { f->id = info->source_id; return 1; }
    return 0;
}

static int64_t source_id_by_name(bd_catalog *cat, const char *name)
{
    find_source_ctx f = {name, 0};
    bd_list_sources(cat, find_source_cb, &f);
    if (!f.id) fprintf(stderr, "brodalf: no backup root named \"%s\" (roots lists them)\n", name);
    return f.id;
}

/* --root NAME (or the older --source NAME): 0 for every root, -1 if unknown. */
static int64_t root_opt(bd_catalog *cat, int argc, char **argv)
{
    const char *name = opt(argc, argv, "--root");
    if (!name) name = opt(argc, argv, "--source");
    if (!name) return 0;
    int64_t id = source_id_by_name(cat, name);
    return id ? id : -1;
}

typedef struct { int64_t id; char label[256]; } label_ctx;

static int label_cb(void *ctx, const bd_media_info *m)
{
    label_ctx *l = ctx;
    if (m->media_id != l->id) return 0;
    snprintf(l->label, sizeof(l->label), "%s", m->label);
    return 1;
}

static const char *label_of(bd_catalog *cat, int64_t id)
{
    static label_ctx l;
    l.id = id;
    l.label[0] = '\0';
    bd_list_media(cat, label_cb, &l);
    return l.label;
}

typedef struct { const char *label; int64_t id; } find_media_ctx;

static int find_any_media_cb(void *ctx, const bd_media_info *info)
{
    find_media_ctx *f = ctx;
    if (strcmp(info->label, f->label) == 0) { f->id = info->media_id; return 1; }
    return 0;
}

static int64_t media_id_by_label(bd_catalog *cat, const char *label)
{
    find_media_ctx f = {label, 0};
    bd_list_media(cat, find_any_media_cb, &f);
    if (!f.id) fprintf(stderr, "brodalf: no drive named \"%s\" (drives lists them)\n", label);
    return f.id;
}

static void format_day(int64_t ms, char *out, size_t cap)
{
    time_t t = (time_t)(ms / 1000);
    struct tm *tm = localtime(&t);
    if (!tm || !strftime(out, cap, "%Y-%m-%d %H:%M", tm)) snprintf(out, cap, "?");
}

static void print_when(const char *what, int64_t ms)
{
    char when[32];
    format_day(ms, when, sizeof(when));
    printf("%s%s", what, when);
}

/* Connect a drive by its root. Nothing on it is read but its BRODALF.media file. */
static bd_status connect_target(bd_catalog *cat, const char *target, int64_t *id)
{
    return bd_media_connect(cat, target, id, log_line, NULL);
}

static int connect_drives(bd_catalog *cat, int argc, char **argv)
{
    for (int i = 0; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--drive") != 0) continue;
        int64_t id;
        if (connect_target(cat, argv[i + 1], &id) != BD_OK) {
            fprintf(stderr, "brodalf: %s\n", bd_catalog_error(cat));
            return -1;
        }
        fprintf(msg_out(), "connected %s as \"%s\"\n", argv[i + 1], label_of(cat, id));
    }
    return 0;
}

/* Ctrl+C asks the running job to stop between files. */
static bd_catalog *volatile g_cat;

static void on_interrupt(int sig)
{
    (void)sig;
    if (g_cat) bd_catalog_cancel(g_cat);
    signal(SIGINT, on_interrupt);
}

/* A job stopped by Ctrl+C: keep what it finished. */
static int stopped(bd_catalog *cat, const char *what)
{
    clear_progress();
    fprintf(msg_out(), "%s stopped; what it finished is kept\n", what);
    if (g_json) { J_OBJ(NULL); j_bool("stopped", 1); J_END(); }
    g_cat = NULL;
    if (bd_catalog_save(cat) != BD_OK) return die(cat, "cannot save catalog");
    bd_catalog_close(cat);
    return 3;
}

/* ---- Printing ------------------------------------------------------------ */

static int print_source(void *ctx, const bd_source_info *s)
{
    (void)ctx;
    if (g_json) {
        J_OBJ(NULL);
        j_int("id", s->source_id);
        j_str("name", s->name);
        j_str("path", s->path);
        j_int("files", s->files_total);
        j_int("bytes", s->bytes_total);
        j_bool("backed_up", s->active);
        j_bool("missing", s->missing);
        j_int("last_scan_ms", s->last_scan_ms);
        J_END();
        return 0;
    }
    char size[32], when[32] = "never";
    human_bytes(s->bytes_total, size, sizeof(size));
    if (s->last_scan_ms) format_day(s->last_scan_ms, when, sizeof(when));
    printf("%-20s %8lld files %-10s scanned %-16s %s%s%s\n", s->name, (long long)s->files_total, size, when, s->path,
           s->active ? "" : "  [stopped: not backed up]", s->missing ? "  [not found at the last scan]" : "");
    return 0;
}

static int print_plan_step(void *ctx, const bd_restore_step *st)
{
    int *n = ctx;
    ++*n;
    if (g_json) {
        J_OBJ(NULL);
        j_str("drive", st->label);
        j_str("kept_in", st->location);
        j_bool("connected", st->connected);
        j_int("files", st->files);
        j_int("bytes", st->bytes);
        J_END();
        return 0;
    }
    char size[32];
    human_bytes(st->bytes, size, sizeof(size));
    printf("  %d. %s%s%s%s: %lld %s (%s)%s\n", *n, st->label, st->location[0] ? " (kept in " : "", st->location,
           st->location[0] ? ")" : "", (long long)st->files, st->files == 1 ? "file" : "files", size,
           st->connected ? "  [connected]" : "");
    return 0;
}

static int print_search(void *ctx, const bd_search_info *r)
{
    (*(int *)ctx)++;
    if (g_json) {
        J_OBJ(NULL);
        j_str("root", r->source_name);
        j_str("path", r->rel_path);
        j_bool("folder", r->is_dir);
        j_int("bytes", r->size);
        j_str("state", bd_node_state_name(r->state));
        j_str("drives", r->where);
        J_END();
        return 0;
    }
    printf("%s/%s%s  [%s]  %s\n", r->source_name, r->rel_path, r->is_dir ? "/" : "", bd_node_state_name(r->state),
           r->where[0] ? r->where : "(no copies)");
    return 0;
}

typedef struct { int shown, limit; } risk_print;

static int print_risk(void *ctx, const bd_risk_info *r)
{
    risk_print *p = ctx;
    if (p->limit && p->shown >= p->limit) return 1;
    p->shown++;
    if (g_json) {
        J_OBJ(NULL);
        j_str("root", r->source_name);
        j_str("path", r->rel_path);
        j_int("bytes", r->size);
        j_int("copies", r->copies);
        j_int("places", r->places);
        j_bool("changed_since_backup", r->older_copies);
        j_bool("unknown_place", r->unknown_place);
        J_END();
        return 0;
    }
    char size[32];
    human_bytes(r->size, size, sizeof(size));
    if (r->copies == 0) printf("  no copy: ");
    else printf("  %d %s in %d %s: ", r->copies, r->copies == 1 ? "copy" : "copies", r->places, r->places == 1 ? "place" : "places");
    printf("%s/%s (%s)%s%s\n", r->source_name, r->rel_path, size, r->older_copies ? ", changed since its last backup" : "",
           r->unknown_place ? ", on a drive with no place set" : "");
    return 0;
}

static void risk_help_json(const char *key, const bd_risk_help *h)
{
    J_OBJ(key);
    j_str("drive", h->label);
    j_str("kept_in", h->location);
    j_bool("connected", h->connected);
    j_int("files", h->files);
    j_int("bytes", h->bytes);
    j_int("free_bytes", h->free_bytes);
    j_bool("fits", h->fits);
    J_END();
}

static int print_risk_help(void *ctx, const bd_risk_help *h)
{
    (void)ctx;
    if (g_json) { risk_help_json(NULL, h); return 0; }
    char size[32];
    human_bytes(h->bytes, size, sizeof(size));
    char free_s[32] = "unknown";
    if (h->free_bytes >= 0) human_bytes(h->free_bytes, free_s, sizeof(free_s));
    printf("  %s%s%s%s: would help %lld %s (%s), %s free%s%s\n", h->label, h->location[0] ? " (kept in " : "", h->location,
           h->location[0] ? ")" : "", (long long)h->files, h->files == 1 ? "file" : "files", size, free_s,
           h->fits ? ", room for all of it" : h->free_bytes >= 0 ? ", not enough room for all of it" : "",
           h->connected ? "  [connected]" : "");
    return 0;
}

static void hw_json(const bd_drive_hw *h)
{
    J_OBJ("disk");
    j_str("vendor", h->vendor);
    j_str("model", h->model);
    j_str("serial", h->serial);
    j_str("firmware", h->firmware);
    j_str("bus", h->bus);
    j_int("disk_bytes", h->disk_bytes);
    j_str("volume_name", h->volume_name);
    j_str("volume_serial", h->volume_serial);
    j_str("filesystem", h->filesystem);
    j_bool("smart", h->smart);
    j_str("health", h->health);
    j_int("temperature_c", h->temperature_c);
    j_int("power_on_hours", h->power_on_hours);
    j_int("power_cycles", h->power_cycles);
    j_int("reallocated_sectors", h->reallocated_sectors);
    j_int("pending_sectors", h->pending_sectors);
    j_int("uncorrectable_sectors", h->uncorrectable_sectors);
    j_int("percent_used", h->percent_used);
    j_str("note", h->note);
    J_END();
}

static int print_drive(void *ctx, const bd_media_info *m)
{
    (void)ctx;
    if (g_json) {
        J_OBJ(NULL);
        j_int("id", m->media_id);
        j_str("label", m->label);
        j_str("kept_in", m->location);
        j_str("last_seen_at", m->last_root);
        j_int("last_seen_ms", m->last_seen_ms);
        j_bool("connected", m->connected);
        j_bool("encrypted", m->encrypted);
        j_bool("hide_names", m->hide_names);
        j_bool("extra_place", m->secondary);
        j_int("limit_bytes", m->limit_bytes);
        j_int("total_bytes", m->total_bytes);
        j_int("free_bytes", m->free_bytes);
        j_int("space_read_ms", m->space_ms);
        j_int("reserve_bytes", m->reserve_bytes);
        j_int("copies", m->copies);
        j_int("bytes_held", m->bytes_held);
        j_int("oldest_check_ms", m->oldest_check_ms);
        j_bool("check_due", m->check_due);
        j_str("lost", m->lost_ms ? m->lost_why : "");
        j_int("lost_ms", m->lost_ms);
        if (m->hw) hw_json(m->hw);
        J_END();
        return 0;
    }
    printf("%s%s%s", m->label, m->connected ? "  (connected)" : "",
           m->hide_names ? "  [encrypted, names hidden]" : m->encrypted ? "  [encrypted]" : "");
    if (m->secondary) printf("  [extra place: starred files only; its copies do not count]");
    if (m->lost_ms) printf("  [marked %s: its copies do not count]", m->lost_why);
    printf("\n");
    if (m->location && *m->location) printf("  kept in:   %s\n", m->location);
    printf("  last seen: at %s\n", m->last_root && m->last_root[0] ? m->last_root : "?");
    if (m->space_ms > 0) {
        char free_s[32], total_s[32], when[32], res[32];
        human_bytes(m->free_bytes, free_s, sizeof(free_s));
        human_bytes(m->total_bytes, total_s, sizeof(total_s));
        human_bytes(m->reserve_bytes, res, sizeof(res));
        format_day(m->space_ms, when, sizeof(when));
        printf("  space:     %s free of %s (read %s); %s kept free for the catalog\n", free_s, total_s, when, res);
    }
    if (m->oldest_check_ms > 0) {
        char when[32];
        format_day(m->oldest_check_ms, when, sizeof(when));
        when[10] = '\0';
        printf("  checked:   oldest copy last read back %s%s\n", when, m->check_due ? "  (a check is due)" : "");
    }
    const bd_drive_hw *h = m->hw;
    if (h) {
        char size[32] = "";
        if (h->disk_bytes > 0) human_bytes(h->disk_bytes, size, sizeof(size));
        char name[200];
        snprintf(name, sizeof(name), "%s%s%s", h->vendor, h->vendor[0] && h->model[0] ? " " : "", h->model);
        const char *parts[3] = {name, size, h->bus};
        char line[300] = "";
        for (int i = 0; i < 3; i++)
            if (parts[i][0]) snprintf(line + strlen(line), sizeof(line) - strlen(line), "%s%s", line[0] ? ", " : "", parts[i]);
        if (line[0]) printf("  disk:      %s\n", line);
        if (h->serial[0]) printf("  serial:    %s%s%s\n", h->serial, h->firmware[0] ? ", firmware " : "", h->firmware);
        if (h->volume_serial[0] || h->volume_name[0])
            printf("  volume:    %s %s %s\n", h->volume_name[0] ? h->volume_name : "(no name)", h->volume_serial, h->filesystem);
        if (h->smart) {
            printf("  health:    %s", h->health[0] ? h->health : "unknown");
            if (h->temperature_c >= 0) printf(", %d C", h->temperature_c);
            if (h->power_on_hours >= 0) printf(", %lld hours on", (long long)h->power_on_hours);
            if (h->power_cycles >= 0) printf(", %lld power cycles", (long long)h->power_cycles);
            if (h->reallocated_sectors >= 0) printf(", %lld reallocated", (long long)h->reallocated_sectors);
            if (h->pending_sectors >= 0) printf(", %lld pending", (long long)h->pending_sectors);
            if (h->uncorrectable_sectors >= 0) printf(", %lld uncorrectable", (long long)h->uncorrectable_sectors);
            if (h->percent_used >= 0) printf(", %d%% worn", h->percent_used);
            printf("\n");
        }
        if (h->note[0]) printf("  note:      %s\n", h->note);
    }
    char held[32];
    human_bytes(m->bytes_held, held, sizeof(held));
    printf("  copies:    %lld (%s)\n\n", (long long)m->copies, held);
    return 0;
}

static const char *marker(bd_node_state s)
{
    switch (s) {
    case BD_STATE_AVAILABLE: return "   ";
    case BD_STATE_AVAILABLE_OLDER: return " ~ ";
    case BD_STATE_PARTIAL: return " + ";
    case BD_STATE_OFFLINE: return " . ";
    case BD_STATE_NO_COPY: return " . ";
    case BD_STATE_BAD: return " ! ";
    case BD_STATE_DELETED: return " x ";
    }
    return " ? ";
}

typedef struct { bd_catalog *cat; int depth; } tree_ctx;

static int print_node(void *ctx, const bd_node_info *n)
{
    tree_ctx *t = ctx;
    tree_ctx child = {t->cat, t->depth + 1};
    if (g_json) {
        J_OBJ(NULL);
        j_str("name", n->name);
        j_bool("folder", n->is_dir);
        j_str("state", bd_node_state_name(n->state));
        if (n->is_dir) {
            j_int("files", n->files_total);
            j_int("files_available", n->files_available);
        } else {
            j_int("bytes", n->size);
            j_int("version", n->version_no);
            j_int("versions", n->version_count);
        }
        if (n->offline_media_label) j_str("plug_in", n->offline_media_label);
        if (n->is_dir) {
            J_ARR("children");
            bd_list_children(t->cat, n->source_id, n->node_id, print_node, &child);
            J_END_ARR();
        }
        J_END();
        return 0;
    }
    printf("%s%*s%s%s", marker(n->state), t->depth * 2, "", n->name, n->is_dir ? "/" : "");
    if (n->is_dir) printf("  (%lld of %lld available)", (long long)n->files_available, (long long)n->files_total);
    if (!n->is_dir && n->version_count > 1) printf("  v%d", n->version_no);
    if (n->state != BD_STATE_AVAILABLE && n->state != BD_STATE_PARTIAL) printf("  [%s", bd_node_state_name(n->state));
    if (n->state == BD_STATE_OFFLINE && n->offline_media_label) printf(": plug in %s", n->offline_media_label);
    if (n->state != BD_STATE_AVAILABLE && n->state != BD_STATE_PARTIAL) printf("]");
    printf("\n");
    if (n->is_dir) bd_list_children(t->cat, n->source_id, n->node_id, print_node, &child);
    return 0;
}

static int print_tree_source(void *ctx, const bd_source_info *info)
{
    bd_catalog *cat = ctx;
    tree_ctx t = {cat, 1};
    if (g_json) {
        J_OBJ(NULL);
        j_str("root", info->name);
        j_str("path", info->path);
        j_int("files", info->files_total);
        j_int("files_available", info->files_available);
        J_ARR("children");
        bd_list_children(cat, info->source_id, 0, print_node, &t);
        J_END_ARR();
        J_END();
        return 0;
    }
    printf("%s  (%s)  %lld of %lld available", info->name, info->path, (long long)info->files_available,
           (long long)info->files_total);
    if (info->offline_media_label) printf(", copies on %s", info->offline_media_label);
    printf("\n");
    bd_list_children(cat, info->source_id, 0, print_node, &t);
    return 0;
}

static int print_copy(void *ctx, const bd_copy_info *c)
{
    (void)ctx;
    if (g_json) {
        J_OBJ(NULL);
        j_int("version", c->version_no);
        j_bool("current", c->is_current);
        j_int("bytes", c->size);
        j_str("checksum", c->hash);
        j_int("first_seen_ms", c->first_seen_ms);
        if (c->media_id) {
            j_str("drive", c->media_label);
            j_str("path_on_drive", c->path_on_media);
            j_str("copy", c->copy_state);
            j_bool("connected", c->connected);
            j_bool("encrypted", c->encrypted);
            j_bool("drive_lost", c->media_lost);
            j_str("kept_in", c->media_location);
        }
        J_END();
        return 0;
    }
    printf("v%-3d %s %12lld bytes  %.16s...", c->version_no, c->is_current ? "current" : "       ", (long long)c->size, c->hash);
    if (c->media_id)
        printf("  on %s (%s%s%s%s%s%s) %s", c->media_label, c->copy_state, c->connected ? ", connected" : ", offline",
               c->encrypted ? ", encrypted" : "", c->media_lost ? ", drive marked lost" : "",
               c->media_location[0] ? ", kept in " : "", c->media_location, c->path_on_media);
    else printf("  no copy");
    printf("\n");
    return 0;
}

typedef struct { int version_no; int64_t version_id; } find_version_ctx;

static int find_version_cb(void *ctx, const bd_copy_info *c)
{
    find_version_ctx *f = ctx;
    if (c->version_no == f->version_no) { f->version_id = c->version_id; return 1; }
    return 0;
}

static void print_preview(const bd_backup_preview_info *p)
{
    if (g_json) {
        J_OBJ(NULL);
        j_int("files_to_copy", p->files_to_copy);
        j_int("bytes_to_copy", p->bytes_to_copy);
        j_int("files_new", p->files_new);
        j_int("files_changed", p->files_changed);
        j_int("free_bytes", p->free_bytes);
        j_bool("free_is_current", p->free_is_current);
        j_int("reserve_bytes", p->reserve_bytes);
        j_int("bytes_short", p->bytes_short);
        j_int("last_scan_ms", p->last_scan_ms);
        j_int("files_held", p->files_held);
        j_int("held_changed", p->held_changed);
        j_bool("mass_change", p->mass_change);
        j_int("files_too_big", p->files_too_big);
        j_int("bytes_too_big", p->bytes_too_big);
        if (p->too_big_why) j_str("too_big_why", p->too_big_why);
        J_END();
        return;
    }
    char size[32], free_s[32];
    human_bytes(p->bytes_to_copy, size, sizeof(size));
    if (p->last_scan_ms > 0) {
        print_when("from the folder scan of ", p->last_scan_ms);
        puts(" (scan first to include changes since)");
    } else {
        puts("the folders have not been scanned yet: scan first");
    }
    printf("to copy: %lld files (%s): %lld new to this drive, %lld changed since their copy here\n",
           (long long)p->files_to_copy, size, (long long)p->files_new, (long long)p->files_changed);
    if (p->files_changed) puts("changed files go alongside their older copies, under .versions; nothing is overwritten");
    if (p->free_bytes >= 0) {
        char res[32];
        human_bytes(p->free_bytes, free_s, sizeof(free_s));
        human_bytes(p->reserve_bytes, res, sizeof(res));
        printf("free on the drive: %s%s, of which %s stays free for the catalog\n", free_s,
               p->free_is_current ? "" : " (as last seen)", res);
    }
    if (p->bytes_short > 0) {
        human_bytes(p->bytes_short, size, sizeof(size));
        printf("about %s will not fit: those files wait for another drive\n", size);
    }
    if (p->files_too_big) {
        human_bytes(p->bytes_too_big, size, sizeof(size));
        printf("of those, %lld file%s (%s) will not fit because %s\n", (long long)p->files_too_big,
               p->files_too_big == 1 ? "" : "s", size, p->too_big_why);
    }
    if (p->mass_change)
        printf("WARNING: %lld of the %lld files this drive holds have different contents now or are gone\n"
               "from their folder. That is what ransomware encrypting your files looks like. If you did not\n"
               "expect it, look at the list first\n"
               "(changed <catalog> <label>) and restore from this drive instead of backing up.\n",
               (long long)p->held_changed, (long long)p->files_held);
}

typedef struct { bd_catalog *cat; int64_t only; int shown; } status_ctx;

static int print_status(void *ctx, const bd_media_info *m)
{
    status_ctx *sc = ctx;
    if (sc->only && m->media_id != sc->only) return 0;
    bd_media_status st;
    if (bd_media_status_get(sc->cat, m->media_id, &st) != BD_OK) return 0;
    sc->shown++;
    if (g_json) {
        J_OBJ(NULL);
        j_str("drive", m->label);
        j_str("kept_in", m->location);
        j_bool("connected", m->connected);
        j_str("lost", m->lost_ms ? m->lost_why : "");
        j_int("files_current", st.files_current);
        j_int("bytes_current", st.bytes_current);
        j_int("files_behind", st.files_behind);
        j_int("bytes_behind", st.bytes_behind);
        j_int("files_not_here", st.files_not_here);
        j_int("bytes_not_here", st.bytes_not_here);
        j_int("deleted_kept", st.deleted_kept);
        j_int("old_versions", st.old_versions);
        j_int("old_bytes", st.old_bytes);
        j_int("copies_bad", st.copies_bad);
        j_int("copies_never_read", st.copies_never_read);
        j_int("last_backup_ms", st.last_backup_ms);
        j_int("last_backup_end_ms", st.last_backup_end_ms);
        j_str("last_backup_status", st.last_backup_status);
        j_int("last_backup_files", st.last_backup_files);
        j_int("last_check_ms", st.last_check_ms);
        j_str("last_check_kind", st.last_check_kind);
        j_str("last_check_status", st.last_check_status);
        J_END();
        return 0;
    }
    char a[32], b[32], c[32];
    printf("%s%s%s%s%s", m->label, m->location[0] ? " (kept in " : "", m->location, m->location[0] ? ")" : "",
           m->connected ? "  [connected]" : "");
    if (m->lost_ms) printf("  [marked %s]", m->lost_why);
    printf("\n");
    human_bytes(st.bytes_current, a, sizeof(a));
    human_bytes(st.bytes_behind, b, sizeof(b));
    human_bytes(st.bytes_not_here, c, sizeof(c));
    printf("  up to date:    %lld files (%s)\n", (long long)st.files_current, a);
    printf("  changed since: %lld files (%s) have only an older version here\n", (long long)st.files_behind, b);
    printf("  not on it:     %lld files (%s)\n", (long long)st.files_not_here, c);
    if (st.deleted_kept || st.old_versions) {
        human_bytes(st.old_bytes, a, sizeof(a));
        printf("  also holds:    %lld deleted files, %lld older versions (%s)\n", (long long)st.deleted_kept,
               (long long)st.old_versions, a);
    }
    if (st.last_backup_ms) {
        print_when("  last backup:   ", st.last_backup_ms);
        if (!st.last_backup_end_ms) printf(", did not finish (BRODALF closed during it)");
        else if (strcmp(st.last_backup_status, "stopped") == 0) printf(", stopped partway");
        else if (strcmp(st.last_backup_status, "partial") == 0) printf(", some files failed");
        printf(": %lld copied\n", (long long)st.last_backup_files);
    } else {
        puts("  last backup:   never");
    }
    if (st.last_check_ms) {
        print_when("  last check:    ", st.last_check_ms);
        printf(" (%s, %s)\n", st.last_check_kind, st.last_check_status);
    } else {
        puts("  last check:    never");
    }
    if (st.copies_bad) printf("  problems:      %lld copies missing or damaged when last checked\n", (long long)st.copies_bad);
    if (st.copies_never_read) printf("  never read back: %lld copies\n", (long long)st.copies_never_read);
    putchar('\n');
    return 0;
}

static int print_deleted(void *ctx, const bd_deleted_info *d)
{
    (void)ctx;
    if (g_json) {
        J_OBJ(NULL);
        j_str("root", d->source_name);
        j_str("path", d->rel_path);
        j_int("bytes", d->size);
        j_int("last_seen_ms", d->last_seen_ms);
        j_str("drives", d->drives);
        j_bool("connected", d->connected);
        j_bool("backed_up", d->backed_up);
        j_bool("seen", d->acknowledged);
        J_END();
        return 0;
    }
    char size[32], when[32];
    human_bytes(d->size, size, sizeof(size));
    format_day(d->last_seen_ms, when, sizeof(when));
    printf("%s %s/%s (%s), last seen %s: %s\n", d->acknowledged ? " " : "*", d->source_name, d->rel_path, size, when,
           d->drives[0] ? d->drives : "on no drive (it was never backed up)");
    return 0;
}

static int print_changed(void *ctx, const bd_changed_info *c)
{
    (*(int *)ctx)++;
    if (g_json) {
        J_OBJ(NULL);
        j_str("root", c->source_name);
        j_str("path", c->rel_path);
        j_int("version_on_drive", c->version_on_drive);
        j_int("version_now", c->version_now);
        j_int("bytes_on_drive", c->size_on_drive);
        j_int("bytes_now", c->size_now);
        j_int("changed_ms", c->changed_ms);
        j_int("deleted", c->deleted);
        J_END();
        return 0;
    }
    if (c->deleted) {
        printf("  %s/%s: v%lld on the drive, deleted from its folder\n", c->source_name, c->rel_path,
               (long long)c->version_on_drive);
        return 0;
    }
    char when[32];
    format_day(c->changed_ms, when, sizeof(when));
    printf("  %s/%s: v%lld on the drive, v%lld now (%lld -> %lld bytes), changed %s\n", c->source_name, c->rel_path,
           (long long)c->version_on_drive, (long long)c->version_now, (long long)c->size_on_drive,
           (long long)c->size_now, when);
    return 0;
}

static int print_conflict(void *ctx, const bd_conflict_info *c)
{
    (*(int *)ctx)++;
    if (g_json) {
        J_OBJ(NULL);
        j_str("root", c->source_name);
        j_str("path", c->rel_path);
        j_str("dest", c->dest);
        j_int("backup_bytes", c->want_size);
        j_int("backup_mtime_ns", c->want_mtime_ns);
        j_int("there_bytes", c->have_size);
        j_int("there_mtime_ns", c->have_mtime_ns);
        J_END();
        return 0;
    }
    char want[32], have[32];
    format_day(c->want_mtime_ns / 1000000, want, sizeof(want));
    format_day(c->have_mtime_ns / 1000000, have, sizeof(have));
    if (c->have_size < 0) printf("  %s: a folder is there\n", c->dest);
    else
        printf("  %s: the backup has %lld bytes from %s, there is %lld bytes from %s\n", c->dest, (long long)c->want_size,
               want, (long long)c->have_size, have);
    return 0;
}

static int print_in_use(void *ctx, const char *path, int64_t size)
{
    (*(int *)ctx)++;
    if (g_json) {
        J_OBJ(NULL);
        j_str("path", path);
        j_int("bytes", size);
        J_END();
        return 0;
    }
    printf("  %s\n", path);
    return 0;
}

typedef struct { bd_catalog *cat; int argc; char **argv; int shown, accepted; } damaged_ctx;

static int print_damaged(void *ctx, const bd_damaged_info *d)
{
    damaged_ctx *dc = ctx;
    /* --accept Root/path: changed on purpose. */
    for (int i = 0; i + 1 < dc->argc; i++) {
        if (strcmp(dc->argv[i], "--accept") != 0) continue;
        char full[2048];
        snprintf(full, sizeof(full), "%s/%s", d->source_name, d->rel_path);
        if (strcmp(full, dc->argv[i + 1]) == 0 && bd_damaged_accept(dc->cat, d->node_id) == BD_OK) {
            dc->accepted++;
            fprintf(msg_out(), "recorded %s as changed on purpose: it is a new version now\n", full);
            return 0;
        }
    }
    dc->shown++;
    if (g_json) {
        J_OBJ(NULL);
        j_str("root", d->source_name);
        j_str("path", d->rel_path);
        j_int("bytes", d->size);
        j_str("checksum_on_record", d->hash_on_record);
        j_str("checksum_now", d->hash_found);
        J_END();
        return 0;
    }
    printf("  %s/%s\n", d->source_name, d->rel_path);
    return 0;
}

static void print_risk_stats(bd_catalog *cat, int64_t source_id, int all)
{
    bd_target t;
    bd_target_get(cat, &t);
    bd_risk_stats st;
    risk_print rp = {0, all ? 0 : 50};
    bd_list_at_risk(cat, source_id, NULL, NULL, &st);
    if (g_json) {
        J_OBJ(NULL);
        j_int("target_copies", t.copies);
        j_int("target_places", t.places);
        j_int("files_total", st.files_total);
        j_int("files_at_risk", st.files_at_risk);
        j_int("files_no_copy", st.files_no_copy);
        j_int("bytes_at_risk", st.bytes_at_risk);
        j_int("deleted_kept", st.deleted_kept);
        j_int("deleted_short", st.deleted_short);
        j_int("old_versions_kept", st.old_versions_kept);
        j_int("old_versions_short", st.old_versions_short);
        J_ARR("files");
        bd_list_at_risk(cat, source_id, print_risk, &rp, NULL);
        J_END_ARR();
        J_ARR("drives_that_help");
        bd_list_risk_help(cat, print_risk_help, NULL);
        J_END_ARR();
        J_END();
        return;
    }
    char size[32];
    human_bytes(st.bytes_at_risk, size, sizeof(size));
    printf("target: %d %s in %d %s\n", t.copies, t.copies == 1 ? "copy" : "copies", t.places, t.places == 1 ? "place" : "places");
    printf("%lld of %lld files are short of it (%s), %lld with no copy at all\n", (long long)st.files_at_risk,
           (long long)st.files_total, size, (long long)st.files_no_copy);
    if (st.deleted_kept)
        printf("%lld deleted files are kept on drives, %lld of them with fewer than %d copies\n",
               (long long)st.deleted_kept, (long long)st.deleted_short, t.copies);
    if (st.old_versions_kept)
        printf("%lld older versions are kept on drives, %lld of them with fewer than %d copies\n",
               (long long)st.old_versions_kept, (long long)st.old_versions_short, t.copies);
    if (st.files_at_risk) {
        bd_list_at_risk(cat, source_id, print_risk, &rp, NULL);
        if (rp.limit && st.files_at_risk > rp.shown)
            printf("  ... and %lld more (--all lists them all)\n", (long long)(st.files_at_risk - rp.shown));
        puts("back up to these next:");
        bd_list_risk_help(cat, print_risk_help, NULL);
    }
}

/* --shadow-copy: the files in use the step just met are copied out of a
 * shadow copy (this needs administrator rights), so the step can run again
 * and read those copies. 1 if any is ready (unstage afterwards), 0 if not. */
static int shadow_stage(bd_catalog *cat, int64_t in_use, char **dir)
{
    *dir = NULL;
    if (!in_use) return 0;
    fprintf(msg_out(), "%lld files are in use by another program: reading them from a shadow copy of their drive\n",
            (long long)in_use);
    int ready = bd_shadow_stage(cat, dir);
    if (ready < 0) {
        fprintf(stderr, "brodalf: no shadow copy: %s\n", bd_catalog_error(cat));
        return 0;
    }
    if (ready == 0) { bd_shadow_unstage(cat, *dir); free(*dir); *dir = NULL; return 0; }
    fprintf(msg_out(), "%d of them were copied from the shadow copy; reading those copies now\n", ready);
    return 1;
}

/* After a scan or a backup: files another program kept open. */
static void report_in_use(bd_catalog *cat, int64_t count)
{
    if (!count) return;
    int n = 0;
    if (g_json) {
        J_ARR("in_use");
        bd_list_in_use(cat, print_in_use, &n);
        J_END_ARR();
        return;
    }
#ifdef _WIN32
    printf("%lld files are in use by another program and were skipped; close it and run this again, or run this as\n"
           "administrator with --shadow-copy to read them from a shadow copy of their drive:\n", (long long)count);
#else
    printf("%lld files are in use by another program and were skipped; close it and run this again:\n", (long long)count);
#endif
    bd_list_in_use(cat, print_in_use, &n);
}

static void print_restore_stats(bd_catalog *cat, const bd_restore_stats *rs)
{
    int conflicts = 0;
    if (g_json) {
        J_OBJ(NULL);
        j_int("files_restored", rs->files_restored);
        j_int("bytes_restored", rs->bytes_restored);
        j_int("files_already_there", rs->files_already_there);
        j_int("files_offline", rs->files_offline);
        j_int("files_no_copy", rs->files_no_copy);
        j_int("files_need_passphrase", rs->files_need_passphrase);
        j_int("files_failed", rs->files_failed);
        j_int("files_conflict", rs->files_conflict);
        J_ARR("conflicts");
        bd_list_restore_conflicts(cat, print_conflict, &conflicts);
        J_END_ARR();
        J_END();
        return;
    }
    printf("restore: %lld restored (%lld bytes), %lld already there, %lld on drives that are not connected, "
           "%lld never backed up, %lld failed\n",
           (long long)rs->files_restored, (long long)rs->bytes_restored, (long long)rs->files_already_there,
           (long long)rs->files_offline, (long long)rs->files_no_copy, (long long)rs->files_failed);
    if (rs->files_need_passphrase)
        printf("%lld files are on encrypted drives and need the passphrase\n", (long long)rs->files_need_passphrase);
    if (rs->files_offline) puts("run the same restore again with the other drives (restore-plan says which); what is there is kept");
    if (rs->files_conflict) {
        printf("%lld files were left alone because another file is already where they would go:\n", (long long)rs->files_conflict);
        bd_list_restore_conflicts(cat, print_conflict, &conflicts);
        puts("nothing was overwritten; move those aside and run the restore again to get the backed-up ones");
    }
}

/* ---- Recovery on a new PC ------------------------------------------------ */

/* The newest copy across every drive given; more than one catalog needs --id. */
typedef struct { int n, catalogs; const char *want; char id[64]; char path[4096]; int64_t revision, saved_ms; } copies_found;

static int copies_cb(void *ctx, const bd_catalog_copy_info *c)
{
    copies_found *f = ctx;
    if (f->want && strcmp(f->want, c->catalog_id) != 0) return 0;
    if (f->n && strcmp(f->id, c->catalog_id) != 0) f->catalogs = 2;
    else if (!f->catalogs) f->catalogs = 1;
    f->n++;
    if (g_json) {
        J_OBJ(NULL);
        j_str("catalog_id", c->catalog_id);
        j_str("path", c->path);
        j_int("revision", c->revision);
        j_int("saved_ms", c->saved_ms);
        J_END();
    } else {
        char when[32];
        format_day(c->saved_ms, when, sizeof(when));
        fprintf(msg_out(), "  catalog %s: revision %lld, saved %s (%s)\n", c->catalog_id, (long long)c->revision, when, c->path);
    }
    if (f->n > 1 && (c->revision < f->revision || (c->revision == f->revision && c->saved_ms <= f->saved_ms))) return 0;
    snprintf(f->id, sizeof(f->id), "%s", c->catalog_id);
    snprintf(f->path, sizeof(f->path), "%s", c->path);
    f->revision = c->revision;
    f->saved_ms = c->saved_ms;
    return 0;
}

static int missing_root_cb(void *ctx, const bd_source_info *s)
{
    if (!s->missing) return 0;
    (*(int *)ctx)++;
    fprintf(msg_out(), "  %s was at %s, which is not on this PC\n", s->name, s->path);
    return 0;
}

static int cmd_recover(int argc, char **argv)
{
    const char *from = argv[2];
    const char *pos[16];
    int npos = positional(argc, argv, pos, 16);
    if (npos < 1)
        return refuse(NULL, "recover needs the drive (or the catalog copy) and where to keep the new catalog");
    const char *dest = pos[npos - 1];
    copies_found f;
    memset(&f, 0, sizeof(f));
    f.want = opt(argc, argv, "--id");
    FILE *probe = fopen(from, "rb");
    int is_file = 0;
    if (probe) {
        /* A folder opens on some systems; only a file reads. */
        is_file = fgetc(probe) != EOF;
        fclose(probe);
    }
    if (g_json) J_OBJ(NULL);
    if (is_file) {
        if (npos > 1) { if (g_json) J_END(); return refuse(NULL, "give one catalog copy, or the drives to look on"); }
        snprintf(f.path, sizeof(f.path), "%s", from);
        f.n = 1;
    } else {
        if (g_json) J_ARR("found");
        for (int i = -1; i < npos - 1; i++) {
            const char *drive = i < 0 ? from : pos[i];
            if (!g_json) fprintf(msg_out(), "looking for catalog copies in %s\n", drive);
            bd_find_catalog_copies(drive, copies_cb, &f);
        }
        if (g_json) J_END_ARR();
        if (f.n == 0) {
            if (g_json) J_END();
            return refuse(NULL, "no catalog copy was found there: a backup drive keeps them as "
                                "BRODALF/<catalog id>/catalog-r<revision>.brodalf");
        }
        if (f.catalogs > 1) {
            if (g_json) J_END();
            return refuse(NULL, "these drives hold copies of more than one catalog: say which with --id ID");
        }
        if (!g_json && f.n > 1) fprintf(msg_out(), "using the newest, revision %lld\n", (long long)f.revision);
    }
    bd_status s = bd_catalog_recover(f.path, dest);
    if (s != BD_OK) {
        if (g_json) J_END();
        return die(NULL, "cannot recover the catalog");
    }
    bd_applog("recovered the catalog from %s to %s", f.path, dest);
    bd_catalog *cat = NULL;
    if (bd_catalog_file_needs_passphrase(dest)) {
        char pass[512];
        if (get_passphrase(pass, sizeof(pass)) != 0) { if (g_json) J_END(); return 1; }
        s = bd_catalog_open_with(dest, pass, &cat);
        memset(pass, 0, sizeof(pass));
    } else {
        s = bd_catalog_open(dest, &cat);
    }
    if (s != BD_OK) {
        if (g_json) J_END();
        return die(NULL, "the recovered catalog does not open");
    }
    /* A scan finds which roots are where they were; a missing one is left
     * as it was, nothing in it marked deleted. */
    bd_scan_stats st;
    memset(&st, 0, sizeof(st));
    bd_scan(cat, &st, log_line, NULL);
    if (g_json) {
        j_str("catalog", dest);
        j_str("from", f.path);
        j_int("roots_missing", st.roots_missing);
        J_END();
    } else {
        printf("recovered the catalog to %s from %s\n", dest, f.path);
    }
    int missing = 0;
    bd_list_sources(cat, missing_root_cb, &missing);
    if (missing) fprintf(msg_out(), "point those at where they are now with root-move, or restore them from the drives\n");
    if (bd_catalog_save(cat) != BD_OK) return die(cat, "cannot save catalog");
    bd_catalog_close(cat);
    return 0;
}

/* ---- Starred files, extra places, encrypted folders, pending ---------------- */

/* "2GB", "500 MB", "1.5T", "123" (bytes). -1 if not a size. */
static int64_t parse_size(const char *s)
{
    char *end;
    double v = strtod(s, &end);
    if (end == s || v < 0) return -1;
    while (*end == ' ') end++;
    double mul = 1;
    switch (*end) {
    case 'k': case 'K': mul = 1024.0; end++; break;
    case 'm': case 'M': mul = 1024.0 * 1024; end++; break;
    case 'g': case 'G': mul = 1024.0 * 1024 * 1024; end++; break;
    case 't': case 'T': mul = 1024.0 * 1024 * 1024 * 1024; end++; break;
    default: break;
    }
    if (*end == 'b' || *end == 'B') end++;
    if (*end) return -1;
    return (int64_t)(v * mul);
}

static int print_mark(void *ctx, const bd_mark_info *m)
{
    (void)ctx;
    char size[32];
    human_bytes(m->bytes, size, sizeof(size));
    if (g_json) {
        J_OBJ(NULL);
        j_str("root", m->source_name);
        j_str("path", m->rel_path);
        j_bool("folder", m->is_dir);
        j_bool("found", m->found);
        j_int("files", m->files);
        j_int("bytes", m->bytes);
        J_END();
        return 0;
    }
    printf("  %s/%s%s  %lld file%s (%s)%s\n", m->source_name, m->rel_path, m->is_dir && m->rel_path[0] ? "/" : "",
           (long long)m->files, m->files == 1 ? "" : "s", size, m->found ? "" : "  [no longer in the catalog: moved or deleted]");
    return 0;
}

typedef struct { bd_catalog *cat; int shown; } places_ctx;

static int print_place(void *ctx, const bd_media_info *m)
{
    places_ctx *pc = ctx;
    if (!m->secondary || m->lost_ms) return 0;
    bd_place_status st;
    if (bd_place_status_get(pc->cat, m->media_id, &st) != BD_OK) return 0;
    pc->shown++;
    if (g_json) {
        J_OBJ(NULL);
        j_str("place", m->label);
        j_str("folder", m->last_root);
        j_bool("connected", m->connected);
        j_bool("encrypted", m->encrypted);
        j_int("limit_bytes", st.limit_bytes);
        j_int("used_bytes", st.used_bytes);
        j_int("starred_files", st.starred_files);
        j_int("have_files", st.have_files);
        j_int("waiting_files", st.waiting_files);
        j_int("waiting_bytes", st.waiting_bytes);
        j_int("over_limit_files", st.over_limit_files);
        j_int("over_limit_bytes", st.over_limit_bytes);
        J_END();
        return 0;
    }
    char a[32], b[32];
    printf("%s%s%s\n", m->label, m->connected ? "  [connected]" : "", m->encrypted ? "  [encrypted]" : "");
    printf("  folder:   %s\n", m->last_root && m->last_root[0] ? m->last_root : "?");
    human_bytes(st.used_bytes, a, sizeof(a));
    if (st.limit_bytes > 0) {
        human_bytes(st.limit_bytes, b, sizeof(b));
        printf("  uses:     %s of the %s you allowed\n", a, b);
    } else {
        printf("  uses:     %s (no limit)\n", a);
    }
    printf("  starred:  %lld of %lld starred files are there as they are now\n", (long long)st.have_files,
           (long long)st.starred_files);
    if (st.waiting_files) {
        human_bytes(st.waiting_bytes, a, sizeof(a));
        printf("  waiting:  %lld file%s (%s) go there at its next backup\n", (long long)st.waiting_files,
               st.waiting_files == 1 ? "" : "s", a);
    }
    if (st.over_limit_files) {
        human_bytes(st.over_limit_bytes, a, sizeof(a));
        printf("  too big:  %lld starred file%s (%s) would go past the limit; raise it with place-limit\n",
               (long long)st.over_limit_files, st.over_limit_files == 1 ? "" : "s", a);
    }
    putchar('\n');
    return 0;
}

static int print_pending(void *ctx, const bd_pending_info *p)
{
    (void)ctx;
    char size[32];
    human_bytes(p->size, size, sizeof(size));
    if (g_json) {
        J_OBJ(NULL);
        j_str("root", p->source_name);
        j_str("path", p->rel_path);
        j_int("size", p->size);
        j_int("copies", p->copies);
        j_int("places", p->places);
        j_bool("backed_up", p->backed_up);
        j_bool("new", p->is_new);
        j_bool("starred", p->starred);
        j_bool("encrypt", p->encrypt);
        J_END();
        return 0;
    }
    printf("  %s/%s  %s, %d cop%s in %d place%s%s%s\n", p->source_name, p->rel_path, size, p->copies,
           p->copies == 1 ? "y" : "ies", p->places, p->places == 1 ? "" : "s", p->starred ? ", starred" : "",
           p->encrypt ? ", encrypted" : "");
    return 0;
}

/* The JSON fields or the lines for one drive's backup. */
static void print_backup_result(bd_catalog *cat, const char *label, const bd_backup_stats *bs)
{
    if (g_json) {
        J_OBJ(NULL);
        if (label) j_str("drive", label);
        j_int("files_copied", bs->files_copied);
        j_int("bytes_copied", bs->bytes_copied);
        j_int("files_already_there", bs->files_already_there);
        j_int("files_failed", bs->files_failed);
        j_int("files_no_room", bs->files_no_room);
        j_int("bytes_no_room", bs->bytes_no_room);
        j_int("copies_alongside", bs->copies_alongside);
        j_int("files_rehashed", bs->files_rehashed);
        j_int("files_online_only", bs->files_online_only);
        j_int("files_too_big", bs->files_too_big);
        j_int("bytes_too_big", bs->bytes_too_big);
        if (bs->too_big_why) j_str("too_big_why", bs->too_big_why);
        report_in_use(cat, bs->files_in_use);
        J_END();
        return;
    }
    char size[32];
    const char *pre = label ? label : "backup";
    human_bytes(bs->bytes_copied, size, sizeof(size));
    if (!bs->files_copied && !bs->files_failed && !bs->files_no_room)
        printf("%s: nothing to back up: it holds every file as it was at the last scan\n", pre);
    else
        printf("%s: %lld copied (%s), %lld already there, %lld failed\n", pre, (long long)bs->files_copied, size,
               (long long)bs->files_already_there, (long long)bs->files_failed);
    if (bs->copies_alongside)
        printf("%lld went alongside an older copy (under .versions), so nothing was overwritten\n",
               (long long)bs->copies_alongside);
    if (bs->files_rehashed)
        printf("%lld had changed since the scan and were backed up as they are now\n", (long long)bs->files_rehashed);
    report_in_use(cat, bs->files_in_use);
    if (bs->files_online_only)
        printf("%lld online-only files (not on this PC) were not copied, so as not to download them\n",
               (long long)bs->files_online_only);
    if (bs->files_no_room) {
        human_bytes(bs->bytes_no_room, size, sizeof(size));
        printf("%lld file%s (%s) did not fit (the drive is full or a place reached its limit) and wait\n",
               (long long)bs->files_no_room, bs->files_no_room == 1 ? "" : "s", size);
    }
    if (bs->files_too_big) {
        human_bytes(bs->bytes_too_big, size, sizeof(size));
        printf("of those, %lld file%s (%s) did not fit because %s\n", (long long)bs->files_too_big,
               bs->files_too_big == 1 ? "" : "s", size, bs->too_big_why);
    }
}

/* ---- Commands ------------------------------------------------------------ */

static int run(int argc, char **argv)
{
    g_json = has_flag(argc, argv, "--json");
    if (argc < 3) { usage(); return argc < 2 ? 1 : (strcmp(argv[1], "help") == 0 ? 0 : 1); }
    const char *cmd = argv[1], *path = argv[2];
    bd_catalog *cat = NULL;
    char *log_path = bd_applog_default_path();
    bd_applog_open(log_path);
    free(log_path);
    bd_applog("brodalf-cli %s: %s %s", bd_version(), cmd, path);
    const char *pos[20];
    int npos = positional(argc, argv, pos, 20);

    if (strcmp(cmd, "recover") == 0) return cmd_recover(argc, argv);

    if (strcmp(cmd, "new") == 0) {
        if (bd_catalog_create(path, &cat) != BD_OK) return die(NULL, "cannot create catalog");
        for (int i = 0; i < npos; i++) {
            int64_t id;
            if (bd_source_add(cat, pos[i], &id) != BD_OK) return die(cat, "cannot add the folder");
            fprintf(msg_out(), "backing up %s\n", pos[i]);
        }
        if (bd_catalog_save(cat) != BD_OK) return die(cat, "cannot save catalog");
        if (g_json) { J_OBJ(NULL); j_str("catalog", path); j_str("id", bd_catalog_uuid(cat)); J_END(); }
        else printf("created %s (catalog %s); %s\n", path, bd_catalog_uuid(cat), npos ? "scan it next" : "add folders to back up next");
        bd_catalog_close(cat);
        return 0;
    }

    if (bd_catalog_file_needs_passphrase(path)) {
        char pass[512];
        if (get_passphrase(pass, sizeof(pass)) != 0) return 1;
        bd_status s = bd_catalog_open_with(path, pass, &cat);
        memset(pass, 0, sizeof(pass));
        if (s != BD_OK) return die(NULL, "cannot open catalog");
    } else if (bd_catalog_open(path, &cat) != BD_OK) {
        return die(NULL, "cannot open catalog");
    }
    int save = 0;
    g_cat = cat;
    signal(SIGINT, on_interrupt);
    progress_state ps = {0, 0, NULL};
    if (!g_json && isatty(fileno(stderr))) bd_catalog_set_progress(cat, on_progress, &ps);

    if (strcmp(cmd, "add") == 0) {
        if (!npos) { usage(); bd_catalog_close(cat); return 1; }
        for (int i = 0; i < npos; i++) {
            int64_t id;
            if (bd_source_add(cat, pos[i], &id) != BD_OK) return die(cat, "cannot add the folder");
            fprintf(msg_out(), "backing up %s\n", pos[i]);
        }
        if (g_json) { J_OBJ(NULL); j_int("added", npos); J_END(); }
        save = 1;
    } else if (strcmp(cmd, "roots") == 0 || strcmp(cmd, "sources") == 0) {
        if (g_json) J_ARR(NULL);
        bd_list_sources(cat, print_source, NULL);
        if (g_json) J_END_ARR();
    } else if (strcmp(cmd, "root-stop") == 0 || strcmp(cmd, "root-resume") == 0) {
        if (npos < 1) { usage(); bd_catalog_close(cat); return 1; }
        int64_t id = source_id_by_name(cat, pos[0]);
        if (!id) { bd_catalog_close(cat); return 1; }
        int active = strcmp(cmd, "root-resume") == 0;
        if (bd_source_set_active(cat, id, active) != BD_OK) return die(cat, "cannot change the root");
        if (g_json) { J_OBJ(NULL); j_str("root", pos[0]); j_bool("backed_up", active); J_END(); }
        else if (active) printf("%s is backed up again\n", pos[0]);
        else printf("%s is no longer scanned or backed up; its copies stay on the drives and still restore\n", pos[0]);
        save = 1;
    } else if (strcmp(cmd, "root-move") == 0) {
        if (npos < 2) { usage(); bd_catalog_close(cat); return 1; }
        int64_t id = source_id_by_name(cat, pos[0]);
        if (!id) { bd_catalog_close(cat); return 1; }
        if (bd_source_relocate(cat, id, pos[1]) != BD_OK) return die(cat, "cannot move the root");
        if (g_json) { J_OBJ(NULL); j_str("root", pos[0]); j_str("path", pos[1]); J_END(); }
        else printf("%s is at %s now; it keeps its name, versions and copies (scan next)\n", pos[0], pos[1]);
        save = 1;
    } else if (strcmp(cmd, "scan") == 0) {
        bd_scan_stats st;
        bd_scan_opts so = {has_flag(argc, argv, "--bitrot")};
        bd_status s = bd_scan_ex(cat, &so, &st, log_line, NULL);
        char *shadow_dir = NULL;
        if (s == BD_OK && has_flag(argc, argv, "--shadow-copy") && shadow_stage(cat, st.files_in_use, &shadow_dir)) {
            s = bd_scan_ex(cat, &so, &st, log_line, NULL);
            bd_shadow_unstage(cat, shadow_dir);
            free(shadow_dir);
        }
        if (s == BD_ERR_CANCELLED) return stopped(cat, "scan");
        if (s != BD_OK) return die(cat, "scan failed");
        damaged_ctx dc = {cat, argc, argv, 0, 0};
        if (g_json) {
            J_OBJ(NULL);
            j_int("files_seen", st.files_seen);
            j_int("folders_seen", st.dirs_seen);
            j_int("files_new", st.files_new);
            j_int("files_changed", st.files_changed);
            j_int("files_moved", st.files_moved);
            j_int("files_deleted", st.files_deleted);
            j_int("deleted_no_copy", st.deleted_no_copy);
            j_int("bytes_hashed", st.bytes_hashed);
            j_int("skipped", st.skipped);
            j_int("skipped_links", st.skipped_links);
            j_int("errors", st.errors);
            j_int("files_online_only", st.files_online_only);
            j_int("files_damaged", st.files_damaged);
            j_int("roots_missing", st.roots_missing);
            j_int("roots_stopped", st.roots_stopped);
            if (st.files_damaged) {
                J_ARR("damaged");
                bd_list_damaged(cat, print_damaged, &dc);
                J_END_ARR();
            }
            report_in_use(cat, st.files_in_use);
            J_END();
        } else {
            printf("scanned %lld files in %lld folders: %lld new, %lld changed, %lld moved or renamed, %lld deleted, "
                   "%lld skipped, %lld errors\n",
                   (long long)st.files_seen, (long long)st.dirs_seen, (long long)st.files_new, (long long)st.files_changed,
                   (long long)st.files_moved, (long long)st.files_deleted, (long long)st.skipped, (long long)st.errors);
            if (st.roots_missing)
                printf("%lld backup roots were not found and were left as they were: plug in their drive, or say where\n"
                       "they are now with root-move (roots shows which)\n", (long long)st.roots_missing);
            if (st.files_deleted)
                printf("%lld files were deleted from their folders (%lld of them on no drive); they stay in the catalog:\n"
                       "see them with deleted, then keep them (deleted-ack) or forget them (deleted-forget)\n",
                       (long long)st.files_deleted, (long long)st.deleted_no_copy);
            report_in_use(cat, st.files_in_use);
            if (st.files_online_only)
                printf("%lld files are online-only (in the cloud, not on this PC) and were left alone\n",
                       (long long)st.files_online_only);
            if (st.files_damaged) {
                printf("%lld files changed without their size or modified time changing. That is what damage on this\n"
                       "PC's disk looks like; their copies on the drives are the good ones:\n", (long long)st.files_damaged);
                bd_list_damaged(cat, print_damaged, &dc);
                if (dc.shown)
                    puts("restore them from a drive, or if you changed one on purpose, scan --bitrot again with\n"
                         "--accept ROOT/PATH to record it as a new version");
            } else if (so.bitrot) {
                puts("no damaged files found");
            }
        }
        save = 1;
    } else if (strcmp(cmd, "deleted") == 0) {
        int64_t source_id = root_opt(cat, argc, argv);
        if (source_id < 0) { bd_catalog_close(cat); return 1; }
        int64_t count = 0;
        if (g_json) J_ARR(NULL);
        bd_status s = bd_list_deleted(cat, source_id, has_flag(argc, argv, "--new"), print_deleted, NULL, &count);
        if (g_json) J_END_ARR();
        if (s != BD_OK) return die(cat, "cannot list deleted files");
        if (!g_json) {
            if (!count) puts("no deleted files");
            else puts("(* not marked as seen yet) keep them listed with deleted-ack, or stop tracking them with\n"
                      "deleted-forget; restore them with restore --deleted");
        }
    } else if (strcmp(cmd, "deleted-ack") == 0 || strcmp(cmd, "deleted-forget") == 0) {
        int forget = strcmp(cmd, "deleted-forget") == 0, all = has_flag(argc, argv, "--all");
        int64_t node = 0;
        if (!all) {
            if (npos < 2) { usage(); bd_catalog_close(cat); return 1; }
            int64_t sid = source_id_by_name(cat, pos[0]);
            if (!sid) { bd_catalog_close(cat); return 1; }
            if (bd_find_node(cat, sid, pos[1], &node) != BD_OK) return die(cat, "cannot find the file");
        }
        if (forget && all) {
            int64_t count = 0;
            bd_list_deleted(cat, 0, 0, NULL, NULL, &count);
            char q[200];
            snprintf(q, sizeof(q), "Stop tracking all %lld deleted files? Their copies stay on the drives.", (long long)count);
            if (!confirm_typed(argc, argv, q, "forget")) { bd_catalog_close(cat); return 1; }
        }
        int64_t forgotten = 0;
        bd_status s = forget ? bd_deleted_forget(cat, node, &forgotten) : bd_deleted_ack(cat, node);
        if (s == BD_ERR_INVALID) return refuse(cat, "that file is not deleted");
        if (s != BD_OK) return die(cat, forget ? "cannot forget the deleted files" : "cannot mark the deleted files");
        if (g_json) { J_OBJ(NULL); j_int(forget ? "forgotten" : "marked", forget ? forgotten : 1); J_END(); }
        else if (forget) printf("stopped tracking %lld files; their copies stay on the drives\n", (long long)forgotten);
        else printf("marked as seen; they stay in the catalog with the drives that hold them\n");
        save = 1;
    } else if (strcmp(cmd, "drive") == 0) {
        if (npos < 2) { usage(); bd_catalog_close(cat); return 1; }
        int64_t id;
        int hide = has_flag(argc, argv, "--hide-names");
        int encrypt = has_flag(argc, argv, "--encrypt") || hide;
        if (encrypt && !bd_catalog_has_passphrase(cat)) {
            char pass[512];
            if (get_new_passphrase(pass, sizeof(pass)) != 0) { bd_catalog_close(cat); return 1; }
            bd_status s = bd_catalog_set_passphrase(cat, pass);
            memset(pass, 0, sizeof(pass));
            if (s != BD_OK) return die(cat, "cannot set passphrase");
        } else if (encrypt && ensure_unlocked(cat) != 0) {
            bd_catalog_close(cat);
            return 1;
        }
        if (bd_media_init(cat, pos[0], pos[1], (encrypt ? BD_MEDIA_ENCRYPTED : 0) | (hide ? BD_MEDIA_HIDE_NAMES : 0), &id) != BD_OK)
            return die(cat, "cannot set up drive");
        const char *location = opt(argc, argv, "--location");
        if (location && bd_media_set_location(cat, id, location) != BD_OK) return die(cat, "cannot set location");
        if (g_json) { J_OBJ(NULL); j_int("id", id); j_str("label", pos[1]); j_bool("encrypted", encrypt); j_bool("hide_names", hide); J_END(); }
        else printf("set up %s as \"%s\"%s\n", pos[0], pos[1], hide ? ", encrypted, with file and folder names hidden" : encrypt ? ", encrypted" : "");
        save = 1;
    } else if (strcmp(cmd, "option") == 0) {
        if (npos >= 2) {
            if (bd_option_set(cat, pos[0], atoi(pos[1])) != BD_OK) return refuse(cat, bd_catalog_error(cat));
            save = 1;
        } else if (npos == 1) { usage(); bd_catalog_close(cat); return 1; }
        static const char *const names[] = {"check_days", "verify_days", "reserve_mb", "low_priority", "parallel_copies"};
        static const char *const help[] = {
            "offer a check of a drive whose oldest copy was last read back this many days ago (0: never)",
            "verify reads back copies not read back in this many days (0: all)",
            "space kept free on every drive so the newest catalog always fits there",
            "1: run jobs in the background so the PC stays usable; 0: full speed",
            "1: a backup to several drives writes to all at once (each file is read once, but the PC's\n"
            "                       USB, memory and processor work harder and it goes at the slowest drive's pace);\n"
            "                       0: one drive after another"};
        if (g_json) J_OBJ(NULL);
        for (int i = 0; i < 5; i++) {
            if (g_json) j_int(names[i], bd_option_get(cat, names[i]));
            else printf("%-15s %-4d %s\n", names[i], bd_option_get(cat, names[i]), help[i]);
        }
        if (g_json) J_END();
    } else if (strcmp(cmd, "search") == 0) {
        if (npos < 1) { usage(); bd_catalog_close(cat); return 1; }
        char text[1024] = "";
        for (int i = 0; i < npos; i++)
            snprintf(text + strlen(text), sizeof(text) - strlen(text), "%s%s", i ? " " : "", pos[i]);
        int found = 0;
        if (g_json) J_ARR(NULL);
        bd_status s = bd_search(cat, text, 0, print_search, &found);
        if (g_json) J_END_ARR();
        if (s != BD_OK) return die(cat, "search failed");
        if (!found && !g_json) puts("nothing matches");
    } else if (strcmp(cmd, "target") == 0) {
        bd_target t;
        bd_target_get(cat, &t);
        if (npos >= 2) {
            t.copies = atoi(pos[0]);
            t.places = atoi(pos[1]);
            if (bd_target_set(cat, &t) != BD_OK) return refuse(cat, bd_catalog_error(cat));
            save = 1;
        } else if (npos == 1) { usage(); bd_catalog_close(cat); return 1; }
        if (g_json) { J_OBJ(NULL); j_int("copies", t.copies); j_int("places", t.places); J_END(); }
        else printf("target: %d %s of every file, in %d different %s\n", t.copies, t.copies == 1 ? "copy" : "copies",
                    t.places, t.places == 1 ? "place" : "places");
    } else if (strcmp(cmd, "sets") == 0) {
        bd_set_progress sets[16];
        int n = 0;
        if (bd_backup_sets(cat, sets, 16, &n) != BD_OK) return die(cat, "cannot work out the backup sets");
        if (g_json) J_ARR(NULL);
        for (int i = 0; i < n && i < 16; i++) {
            const bd_set_progress *p = &sets[i];
            if (g_json) {
                J_OBJ(NULL);
                j_int("set", p->set);
                j_int("files_done", p->files_done);
                j_int("files_total", p->files_total);
                j_int("bytes_done", p->bytes_done);
                j_int("bytes_total", p->bytes_total);
                J_END();
                continue;
            }
            char d[32], t[32];
            human_bytes(p->bytes_done, d, sizeof(d));
            human_bytes(p->bytes_total, t, sizeof(t));
            int pct = p->files_total ? (int)(p->files_done * 100 / p->files_total) : 100;
            char bar[21];
            for (int k = 0; k < 20; k++) bar[k] = k < pct / 5 ? '#' : '.';
            bar[20] = '\0';
            printf("backup %d: [%s] %3d%%  %lld of %lld files, %s of %s\n", p->set, bar, pct, (long long)p->files_done,
                   (long long)p->files_total, d, t);
        }
        if (g_json) J_END_ARR();
    } else if (strcmp(cmd, "at-risk") == 0) {
        int64_t source_id = root_opt(cat, argc, argv);
        if (source_id < 0) { bd_catalog_close(cat); return 1; }
        print_risk_stats(cat, source_id, has_flag(argc, argv, "--all"));
    } else if (strcmp(cmd, "largest") == 0) {
        int limit = npos ? atoi(pos[0]) : 20;
        if (limit <= 0) limit = 20;
        risk_print rp = {0, 0};
        if (g_json) J_ARR(NULL);
        bd_status s = bd_list_largest(cat, limit, print_risk, &rp);
        if (g_json) J_END_ARR();
        if (s != BD_OK) return die(cat, "cannot list the largest files");
    } else if (strcmp(cmd, "drives") == 0) {
        if (g_json) J_ARR(NULL);
        bd_list_media(cat, print_drive, NULL);
        if (g_json) J_END_ARR();
    } else if (strcmp(cmd, "drive-location") == 0 || strcmp(cmd, "drive-rename") == 0) {
        int rename = strcmp(cmd, "drive-rename") == 0;
        if (npos < (rename ? 2 : 1)) { usage(); bd_catalog_close(cat); return 1; }
        int64_t id = media_id_by_label(cat, pos[0]);
        if (!id) { bd_catalog_close(cat); return 1; }
        bd_status s = rename ? bd_media_rename(cat, id, pos[1]) : bd_media_set_location(cat, id, npos > 1 ? pos[1] : NULL);
        if (s != BD_OK) return die(cat, "cannot change the drive");
        if (g_json) { J_OBJ(NULL); j_bool("changed", 1); J_END(); }
        else if (rename) printf("renamed \"%s\" to \"%s\"\n", pos[0], pos[1]);
        else if (npos > 1) printf("\"%s\" is kept in: %s\n", pos[0], pos[1]);
        else printf("cleared where \"%s\" is kept\n", pos[0]);
        save = 1;
    } else if (strcmp(cmd, "drive-lost") == 0) {
        if (npos < 2 || (strcmp(pos[1], "lost") != 0 && strcmp(pos[1], "broken") != 0)) { usage(); bd_catalog_close(cat); return 1; }
        int64_t id = media_id_by_label(cat, pos[0]);
        if (!id) { bd_catalog_close(cat); return 1; }
        char q[600];
        snprintf(q, sizeof(q), "Mark \"%s\" as %s? Its copies will stop counting, and what it held will be backed up\n"
                               "again to your other drives. Nothing is deleted; drive-found undoes this.", pos[0], pos[1]);
        if (!confirm_typed(argc, argv, q, pos[0])) { bd_catalog_close(cat); return 1; }
        if (bd_media_mark_lost(cat, id, pos[1]) != BD_OK) return die(cat, "cannot mark the drive");
        if (g_json) { J_OBJ(NULL); j_str("drive", pos[0]); j_str("lost", pos[1]); J_END(); }
        else printf("\"%s\" is marked %s; sets shows what needs backing up again\n", pos[0], pos[1]);
        save = 1;
    } else if (strcmp(cmd, "drive-found") == 0) {
        if (npos < 1) { usage(); bd_catalog_close(cat); return 1; }
        int64_t id = media_id_by_label(cat, pos[0]);
        if (!id) { bd_catalog_close(cat); return 1; }
        if (bd_media_found(cat, id) != BD_OK) return die(cat, "cannot change the drive");
        if (g_json) { J_OBJ(NULL); j_str("drive", pos[0]); j_str("lost", ""); J_END(); }
        else printf("\"%s\" counts again; check it to be sure its copies are still good\n", pos[0]);
        save = 1;
    } else if (strcmp(cmd, "passphrase") == 0) {
        if (ensure_unlocked(cat) != 0) { bd_catalog_close(cat); return 1; }
        char pass[512];
        if (get_new_passphrase(pass, sizeof(pass)) != 0) { bd_catalog_close(cat); return 1; }
        bd_status s = bd_catalog_set_passphrase(cat, pass);
        memset(pass, 0, sizeof(pass));
        if (s != BD_OK) return die(cat, "cannot set passphrase");
        fprintf(msg_out(), "passphrase set\n");
        if (g_json) { J_OBJ(NULL); j_bool("passphrase", 1); J_END(); }
        save = 1;
    } else if (strcmp(cmd, "recovery-key") == 0 || strcmp(cmd, "sheet") == 0) {
        int make = strcmp(cmd, "recovery-key") == 0;
        const char *sheet = make ? opt(argc, argv, "--sheet") : (npos ? pos[0] : NULL);
        if (!make && !sheet) { usage(); bd_catalog_close(cat); return 1; }
        char key[BD_RECOVERY_KEY_SIZE] = "";
        if (make) {
            if (!bd_catalog_has_passphrase(cat)) return refuse(cat, "set a passphrase first: a recovery key is a second way to unlock it");
            if (ensure_unlocked(cat) != 0) { bd_catalog_close(cat); return 1; }
            if (bd_catalog_make_recovery_key(cat, key) != BD_OK) return die(cat, "cannot make a recovery key");
            save = 1;
        }
        if (sheet && bd_write_recovery_sheet(cat, sheet, key[0] ? key : NULL) != BD_OK)
            return die(cat, "cannot write the recovery sheet");
        if (g_json) {
            J_OBJ(NULL);
            if (make) j_str("recovery_key", key);
            if (sheet) j_str("sheet", sheet);
            J_END();
        } else {
            if (make) printf("recovery key: %s\nKeep it apart from the PC, with your drives. It replaces any earlier key.\n", key);
            if (sheet) printf("wrote the recovery sheet to %s; print it and keep it with your drives\n", sheet);
        }
        memset(key, 0, sizeof(key));
    } else if (strcmp(cmd, "encrypt-catalog") == 0) {
        if (npos < 1 || (strcmp(pos[0], "on") != 0 && strcmp(pos[0], "off") != 0)) { usage(); bd_catalog_close(cat); return 1; }
        int on = strcmp(pos[0], "on") == 0;
        if (on && !bd_catalog_has_passphrase(cat)) return refuse(cat, "set a passphrase first (brodalf-cli passphrase <catalog>)");
        if (on && ensure_unlocked(cat) != 0) { bd_catalog_close(cat); return 1; }
        if (bd_catalog_set_file_encrypted(cat, on) != BD_OK) return die(cat, "cannot change catalog encryption");
        if (g_json) { J_OBJ(NULL); j_bool("encrypted", on); J_END(); }
        else printf("the catalog file is %s\n", on ? "encrypted from now on" : "no longer encrypted");
        save = 1;
    } else if (strcmp(cmd, "compress") == 0) {
        if (npos < 1 || (npos >= 2 && strcmp(pos[1], "on") != 0 && strcmp(pos[1], "off") != 0)) {
            usage();
            bd_catalog_close(cat);
            return 1;
        }
        int64_t media_id;
        if (connect_target(cat, pos[0], &media_id) != BD_OK) return die(cat, "cannot use the drive");
        int can = 0, on = 0;
        const char *why = NULL;
        if (bd_media_compression(cat, media_id, &can, &on, &why) != BD_OK) return die(cat, "cannot look at the drive");
        if (npos < 2) {
            if (g_json) {
                J_OBJ(NULL);
                j_bool("can_compress", can);
                j_bool("compressed", on);
                if (why) j_str("why_not", why);
                J_END();
            } else {
                printf("compression is %s for BRODALF's folder on this drive\n", on ? "on" : "off");
                if (why) puts(why);
            }
        } else {
            bd_compress_stats cs;
            int want = strcmp(pos[1], "on") == 0;
            bd_status s = bd_media_set_compression(cat, media_id, want, &cs, log_line, NULL);
            if (s == BD_ERR_CANCELLED) return stopped(cat, "compression");
            if (s != BD_OK) return die(cat, want ? "cannot compress the drive" : "cannot uncompress the drive");
            char before[32], after[32];
            human_bytes(cs.bytes, before, sizeof(before));
            human_bytes(cs.bytes_on_disk, after, sizeof(after));
            if (g_json) {
                J_OBJ(NULL);
                j_bool("compressed", want);
                j_int("files", cs.files);
                j_int("changed", cs.changed);
                j_int("failed", cs.failed);
                j_int("bytes", cs.bytes);
                j_int("bytes_on_disk", cs.bytes_on_disk);
                J_END();
            } else {
                printf("compression %s: %lld files, %lld changed, %lld could not be; %s of copies take %s on the drive\n",
                       want ? "on" : "off", (long long)cs.files, (long long)cs.changed, (long long)cs.failed, before, after);
                if (want) puts("copies made from now on are compressed too");
            }
        }
    } else if (strcmp(cmd, "changed") == 0) {
        if (npos < 1) { usage(); bd_catalog_close(cat); return 1; }
        int64_t id = media_id_by_label(cat, pos[0]);
        if (!id) { bd_catalog_close(cat); return 1; }
        int n = 0;
        if (g_json) J_ARR(NULL);
        bd_status s = bd_list_changed_on_media(cat, id, print_changed, &n);
        if (g_json) J_END_ARR();
        if (s != BD_OK) return die(cat, "cannot list the changed files");
        if (!g_json && !n) puts("every file this drive holds is as it is now");
    } else if (strcmp(cmd, "backup") == 0 || strcmp(cmd, "check") == 0 || strcmp(cmd, "preview") == 0 ||
               strcmp(cmd, "verify") == 0) {
        if (npos < 1) { usage(); bd_catalog_close(cat); return 1; }
        int64_t media_id;
        if (connect_target(cat, pos[0], &media_id) != BD_OK) return die(cat, "cannot use the drive");
        if (strcmp(cmd, "check") == 0 || strcmp(cmd, "verify") == 0) {
            int verify = strcmp(cmd, "verify") == 0;
            const char *d = opt(argc, argv, "--days");
            int days = d ? atoi(d) : bd_option_get(cat, "verify_days");
            bd_check_stats cs;
            bd_status s = verify ? bd_media_verify(cat, media_id, days, &cs, log_line, NULL)
                                 : bd_media_check(cat, media_id, &cs, log_line, NULL);
            if (s == BD_ERR_PASSPHRASE && ensure_unlocked(cat) == 0)
                s = verify ? bd_media_verify(cat, media_id, days, &cs, log_line, NULL)
                           : bd_media_check(cat, media_id, &cs, log_line, NULL);
            if (s == BD_ERR_CANCELLED) return stopped(cat, verify ? "read-back" : "check");
            if (s != BD_OK) return die(cat, "check failed");
            if (g_json) {
                J_OBJ(NULL);
                j_int("copies", cs.copies);
                j_int("read_back", cs.rehashed);
                j_int("ok", cs.ok);
                j_int("missing", cs.missing);
                j_int("damaged", cs.bad);
                J_END();
            } else {
                printf("%s: read back %lld of %lld copies: %lld ok, %lld missing, %lld damaged\n", verify ? "verify" : "check",
                       (long long)cs.rehashed, (long long)cs.copies, (long long)cs.ok, (long long)cs.missing, (long long)cs.bad);
                if (cs.missing || cs.bad) puts("those copies no longer count; the next backup to this drive writes them again");
            }
        } else if (strcmp(cmd, "preview") == 0) {
            bd_backup_preview_info pv;
            if (bd_backup_preview(cat, media_id, 0, &pv) != BD_OK) return die(cat, "cannot preview the backup");
            print_preview(&pv);
        } else if (npos == 1) {
            bd_backup_stats bs;
            bd_status s = bd_backup(cat, media_id, 0, &bs, log_line, NULL);
            if (s == BD_ERR_PASSPHRASE && ensure_unlocked(cat) == 0) s = bd_backup(cat, media_id, 0, &bs, log_line, NULL);
            char *shadow_dir = NULL;
            if (s == BD_OK && has_flag(argc, argv, "--shadow-copy") && shadow_stage(cat, bs.files_in_use, &shadow_dir)) {
                /* A file never read has no version yet: the scan reads it first. */
                bd_scan_stats sst;
                bd_backup_stats first = bs;
                s = bd_scan(cat, &sst, log_line, NULL);
                if (s == BD_OK) s = bd_backup(cat, media_id, 0, &bs, log_line, NULL);
                bd_shadow_unstage(cat, shadow_dir);
                free(shadow_dir);
                if (s == BD_OK) {
                    /* One result for both runs: the second only retried the files in use. */
                    bs.files_considered = first.files_considered;
                    bs.files_copied += first.files_copied;
                    bs.bytes_copied += first.bytes_copied;
                    bs.files_already_there += first.files_already_there;
                    bs.files_failed += first.files_failed - first.files_in_use;
                    bs.files_no_room = first.files_no_room;
                    bs.bytes_no_room = first.bytes_no_room;
                    bs.files_too_big = first.files_too_big;
                    bs.bytes_too_big = first.bytes_too_big;
                    bs.too_big_why = first.too_big_why;
                    bs.copies_alongside += first.copies_alongside;
                    bs.files_rehashed += first.files_rehashed;
                    bs.files_online_only = first.files_online_only;
                }
            }
            if (s == BD_ERR_CANCELLED) {
                /* What it copied is on the drive: so is the catalog that knows it. */
                bd_catalog_copy_to_media(cat, media_id);
                return stopped(cat, "backup");
            }
            if (s != BD_OK) return die(cat, "backup failed");
            print_backup_result(cat, NULL, &bs);
        } else {
            /* Several drives: all at once, or one after another. */
            int64_t ids[16];
            bd_backup_stats bs[16];
            if (npos > 16) return refuse(cat, "back up to at most 16 drives at once");
            ids[0] = media_id;
            for (int i = 1; i < npos; i++)
                if (connect_target(cat, pos[i], &ids[i]) != BD_OK) return die(cat, "cannot use the drive");
            int parallel = has_flag(argc, argv, "--parallel") || bd_option_get(cat, "parallel_copies") == 1;
            if (parallel) {
                bd_status s = bd_backup_many(cat, ids, npos, 0, bs, log_line, NULL);
                if (s == BD_ERR_PASSPHRASE && ensure_unlocked(cat) == 0) s = bd_backup_many(cat, ids, npos, 0, bs, log_line, NULL);
                if (s == BD_ERR_CANCELLED) {
                    for (int i = 0; i < npos; i++) bd_catalog_copy_to_media(cat, ids[i]);
                    return stopped(cat, "backup");
                }
                if (s != BD_OK) return die(cat, "backup failed");
                if (g_json) J_ARR(NULL);
                for (int i = 0; i < npos; i++) print_backup_result(cat, label_of(cat, ids[i]), &bs[i]);
                if (g_json) J_END_ARR();
            } else {
                if (g_json) J_ARR(NULL);
                for (int i = 0; i < npos; i++) {
                    bd_status s = bd_backup(cat, ids[i], 0, &bs[i], log_line, NULL);
                    if (s == BD_ERR_PASSPHRASE && ensure_unlocked(cat) == 0) s = bd_backup(cat, ids[i], 0, &bs[i], log_line, NULL);
                    if (s == BD_ERR_CANCELLED) {
                        bd_catalog_copy_to_media(cat, ids[i]);
                        if (g_json) J_END_ARR();
                        return stopped(cat, "backup");
                    }
                    if (s != BD_OK) return die(cat, "backup failed");
                    print_backup_result(cat, label_of(cat, ids[i]), &bs[i]);
                }
                if (g_json) J_END_ARR();
            }
        }
        save = 1;
    } else if (strcmp(cmd, "star") == 0 || strcmp(cmd, "unstar") == 0 || strcmp(cmd, "encrypt-folder") == 0 ||
               strcmp(cmd, "plain-folder") == 0) {
        if (npos < 1) { usage(); bd_catalog_close(cat); return 1; }
        int star = cmd[0] == 's' || cmd[0] == 'u';
        int on = strcmp(cmd, "star") == 0 || strcmp(cmd, "encrypt-folder") == 0;
        int64_t source_id = source_id_by_name(cat, pos[0]);
        if (!source_id) { bd_catalog_close(cat); return 1; }
        const char *rel = npos > 1 ? pos[1] : "";
        if (!star && on && !bd_catalog_has_passphrase(cat)) {
            fprintf(msg_out(), "encrypted copies are locked with a passphrase: set one now\n");
            char pass[512];
            if (get_new_passphrase(pass, sizeof(pass)) != 0) { bd_catalog_close(cat); return 1; }
            bd_status ps = bd_catalog_set_passphrase(cat, pass);
            memset(pass, 0, sizeof(pass));
            if (ps != BD_OK) return die(cat, "cannot set passphrase");
        }
        bd_status s = star ? bd_star_set(cat, source_id, rel, on) : bd_encrypt_path_set(cat, source_id, rel, on);
        if (s == BD_ERR_NOT_FOUND || s == BD_ERR_INVALID || s == BD_ERR_PASSPHRASE) return refuse(cat, bd_catalog_error(cat));
        if (s != BD_OK) return die(cat, "cannot change it");
        if (g_json) {
            J_OBJ(NULL);
            j_str("root", pos[0]);
            j_str("path", rel);
            j_bool(star ? "starred" : "encrypt", on);
            J_END();
        } else if (star) {
            printf("%s %s/%s\n", on ? "starred" : "no longer starred:", pos[0], rel);
            if (on) puts("it goes to your extra places at their next backup (places lists them)");
            else puts("copies already in your extra places stay there");
        } else if (on) {
            bd_encrypt_stats es;
            char note[1024];
            printf("%s/%s is encrypted on every drive from its next copy on; copies made before stay as they are\n", pos[0], rel);
            if (bd_encrypt_stats_get(cat, &es) == BD_OK) {
                bd_encrypt_cost_text(es.bytes, note, sizeof(note));
                printf("%s\n", note);
            }
        } else {
            printf("new copies of %s/%s are plain files again; encrypted copies stay as they are\n", pos[0], rel);
        }
        save = 1;
    } else if (strcmp(cmd, "stars") == 0 || strcmp(cmd, "encrypted-folders") == 0) {
        int star = strcmp(cmd, "stars") == 0;
        if (g_json) { J_OBJ(NULL); J_ARR(star ? "stars" : "folders"); }
        else puts(star ? "Starred:" : "Set to be encrypted:");
        bd_status s = star ? bd_list_stars(cat, print_mark, NULL) : bd_list_encrypt_paths(cat, print_mark, NULL);
        if (g_json) J_END_ARR();
        if (s != BD_OK) return die(cat, "cannot list them");
        if (star) {
            int64_t files = 0, bytes = 0;
            bd_starred_totals(cat, &files, &bytes);
            char size[32];
            human_bytes(bytes, size, sizeof(size));
            if (g_json) { j_int("files", files); j_int("bytes", bytes); }
            else printf("%lld starred file%s in all (%s)\n", (long long)files, files == 1 ? "" : "s", size);
        } else {
            bd_encrypt_stats es;
            if (bd_encrypt_stats_get(cat, &es) != BD_OK) return die(cat, "cannot count them");
            char note[1024], size[32];
            bd_encrypt_cost_text(es.bytes, note, sizeof(note));
            human_bytes(es.bytes, size, sizeof(size));
            if (g_json) {
                j_int("files", es.files);
                j_int("bytes", es.bytes);
                j_int("encrypted_copies", es.sealed_copies);
                j_int("plain_copies", es.plain_copies);
                j_int("files_waiting", es.files_waiting);
                j_int("bytes_per_second", es.bytes_per_second);
                j_str("note", note);
            } else {
                printf("%lld file%s (%s): %lld encrypted copies; %lld with no encrypted copy yet\n", (long long)es.files,
                       es.files == 1 ? "" : "s", size, (long long)es.sealed_copies, (long long)es.files_waiting);
                if (es.plain_copies)
                    printf("%lld plain copies were made before they were set; they stay as they are\n", (long long)es.plain_copies);
                printf("%s\n", note);
            }
        }
        if (g_json) J_END();
    } else if (strcmp(cmd, "place") == 0) {
        if (npos < 2) { usage(); bd_catalog_close(cat); return 1; }
        const char *lim = opt(argc, argv, "--limit");
        int64_t limit = lim ? parse_size(lim) : 0, id;
        if (limit < 0) return refuse(cat, "--limit takes a size like 2GB or 500MB");
        int hide = has_flag(argc, argv, "--hide-names");
        int encrypt = has_flag(argc, argv, "--encrypt") || hide;
        if (encrypt && !bd_catalog_has_passphrase(cat)) {
            char pass[512];
            if (get_new_passphrase(pass, sizeof(pass)) != 0) { bd_catalog_close(cat); return 1; }
            bd_status ps = bd_catalog_set_passphrase(cat, pass);
            memset(pass, 0, sizeof(pass));
            if (ps != BD_OK) return die(cat, "cannot set passphrase");
        } else if (encrypt && ensure_unlocked(cat) != 0) {
            bd_catalog_close(cat);
            return 1;
        }
        bd_status s = bd_place_add(cat, pos[0], pos[1], limit, (encrypt ? BD_MEDIA_ENCRYPTED : 0) | (hide ? BD_MEDIA_HIDE_NAMES : 0), &id);
        if (s == BD_ERR_INVALID) return refuse(cat, bd_catalog_error(cat));
        if (s != BD_OK) return die(cat, "cannot set up the place");
        if (g_json) { J_OBJ(NULL); j_int("id", id); j_str("label", pos[1]); j_int("limit_bytes", limit); J_END(); }
        else {
            printf("set up %s as an extra place, \"%s\"%s\n", pos[0], pos[1], encrypt ? ", encrypted" : "");
            puts("it gets a second copy of your starred files (star adds them) when you back up to it;");
            puts("its copies never count toward the copies you want, since a synced folder is not a backup drive");
        }
        save = 1;
    } else if (strcmp(cmd, "place-limit") == 0) {
        if (npos < 2) { usage(); bd_catalog_close(cat); return 1; }
        int64_t id = media_id_by_label(cat, pos[0]);
        if (!id) { bd_catalog_close(cat); return 1; }
        int64_t limit = strcmp(pos[1], "none") == 0 ? 0 : parse_size(pos[1]);
        if (limit < 0) return refuse(cat, "give a size like 2GB, or none");
        if (bd_place_set_limit(cat, id, limit) != BD_OK) return refuse(cat, bd_catalog_error(cat));
        if (g_json) { J_OBJ(NULL); j_str("place", pos[0]); j_int("limit_bytes", limit); J_END(); }
        else if (limit) {
            char size[32];
            human_bytes(limit, size, sizeof(size));
            printf("\"%s\" gets starred files up to %s in all\n", pos[0], size);
        } else printf("\"%s\" has no limit now\n", pos[0]);
        save = 1;
    } else if (strcmp(cmd, "places") == 0) {
        places_ctx pc = {cat, 0};
        if (g_json) J_ARR(NULL);
        bd_list_media(cat, print_place, &pc);
        if (g_json) J_END_ARR();
        else if (!pc.shown) puts("no extra places yet: add one with place, then star files with star");
    } else if (strcmp(cmd, "pending") == 0) {
        int64_t media_id = 0, total = 0;
        if (npos >= 1) {
            media_id = media_id_by_label(cat, pos[0]);
            if (!media_id) { bd_catalog_close(cat); return 1; }
        }
        int all = has_flag(argc, argv, "--all");
        if (g_json) { J_OBJ(NULL); J_ARR("files"); }
        else if (media_id) printf("The next backup to \"%s\" copies:\n", pos[0]);
        else puts("Files short of the copies you want:");
        bd_status s = bd_list_pending(cat, media_id, all ? 2000000000 : 50, print_pending, NULL, &total);
        if (g_json) { J_END_ARR(); j_int("total", total); J_END(); }
        if (s != BD_OK) return die(cat, "cannot list what to back up");
        if (!g_json) {
            if (!total) puts("  nothing");
            else if (!all && total > 50) printf("  ... and %lld more (--all lists them all)\n", (long long)(total - 50));
        }
    } else if (strcmp(cmd, "tree") == 0) {
        if (connect_drives(cat, argc, argv) != 0) { bd_catalog_close(cat); return 1; }
        if (g_json) J_ARR(NULL);
        bd_list_sources(cat, print_tree_source, cat);
        if (g_json) J_END_ARR();
        else puts("\n(blank) available  ~ older version only  + partly available  . greyed out  ! damaged  x deleted");
        save = 1;
    } else if (strcmp(cmd, "versions") == 0) {
        if (npos < 2) { usage(); bd_catalog_close(cat); return 1; }
        if (connect_drives(cat, argc, argv) != 0) { bd_catalog_close(cat); return 1; }
        int64_t source_id = source_id_by_name(cat, pos[0]), node_id = 0;
        if (!source_id) { bd_catalog_close(cat); return 1; }
        if (bd_find_node(cat, source_id, pos[1], &node_id) != BD_OK) return die(cat, "cannot find the file");
        if (g_json) J_ARR(NULL);
        bd_list_copies(cat, node_id, print_copy, NULL);
        if (g_json) J_END_ARR();
        save = 1;
    } else if (strcmp(cmd, "restore") == 0 || strcmp(cmd, "restore-version") == 0) {
        int one = strcmp(cmd, "restore-version") == 0;
        if (npos < (one ? 4 : 1)) { usage(); bd_catalog_close(cat); return 1; }
        if (connect_drives(cat, argc, argv) != 0) { bd_catalog_close(cat); return 1; }
        if (ensure_unlocked(cat) != 0) { bd_catalog_close(cat); return 1; }
        bd_restore_stats rs;
        bd_status s;
        if (one) {
            int64_t source_id = source_id_by_name(cat, pos[0]), node_id = 0;
            if (!source_id) { bd_catalog_close(cat); return 1; }
            if (bd_find_node(cat, source_id, pos[1], &node_id) != BD_OK) return die(cat, "cannot find the file");
            find_version_ctx fv = {atoi(pos[2][0] == 'v' ? pos[2] + 1 : pos[2]), 0};
            bd_list_copies(cat, node_id, find_version_cb, &fv);
            if (!fv.version_id) return refuse(cat, "the file has no such version (versions lists them)");
            s = bd_restore_version(cat, fv.version_id, pos[3], &rs, log_line, NULL);
        } else {
            int64_t source_id = root_opt(cat, argc, argv);
            if (source_id < 0) { bd_catalog_close(cat); return 1; }
            bd_restore_opts ro = {has_flag(argc, argv, "--deleted")};
            s = bd_restore_ex(cat, source_id, opt(argc, argv, "--path"), pos[0], &ro, &rs, log_line, NULL);
        }
        if (s == BD_ERR_CANCELLED) return stopped(cat, "restore (run the same restore again to carry on)");
        if (s != BD_OK) return die(cat, "restore failed");
        print_restore_stats(cat, &rs);
        save = 1;
    } else if (strcmp(cmd, "restore-plan") == 0) {
        if (connect_drives(cat, argc, argv) != 0) { bd_catalog_close(cat); return 1; }
        int64_t source_id = root_opt(cat, argc, argv);
        if (source_id < 0) { bd_catalog_close(cat); return 1; }
        int64_t total = 0, none = 0;
        int steps = 0;
        bd_restore_opts ro = {has_flag(argc, argv, "--deleted")};
        if (g_json) { J_OBJ(NULL); J_ARR("drives"); }
        else puts("plug these in, in this order:");
        bd_status s = bd_restore_plan_ex(cat, source_id, opt(argc, argv, "--path"), opt(argc, argv, "--dest"), &ro,
                                         print_plan_step, &steps, &total, &none);
        if (g_json) { J_END_ARR(); j_int("files", total); j_int("files_no_copy", none); J_END(); }
        if (s != BD_OK) return die(cat, "cannot plan the restore");
        if (!g_json) {
            if (!steps) puts("  (nothing to plug in)");
            printf("%lld files to restore, %lld with no copy anywhere\n", (long long)total, (long long)none);
        }
        save = 1;
    } else if (strcmp(cmd, "need") == 0) {
        bd_risk_stats rs;
        if (bd_list_at_risk(cat, 0, NULL, NULL, &rs) != BD_OK) return die(cat, "cannot work out what needs backing up");
        bd_target t;
        bd_target_get(cat, &t);
        bd_risk_help h;
        int have = rs.files_at_risk && bd_suggest_drive(cat, &h) == BD_OK;
        if (g_json) {
            J_OBJ(NULL);
            j_int("files_total", rs.files_total);
            j_int("files_at_risk", rs.files_at_risk);
            j_int("bytes_at_risk", rs.bytes_at_risk);
            if (have) risk_help_json("plug_in", &h);
            J_END();
        } else {
            char size[32];
            human_bytes(rs.bytes_at_risk, size, sizeof(size));
            printf("%lld of %lld files (%s) have fewer than %d copies in %d places\n", (long long)rs.files_at_risk,
                   (long long)rs.files_total, size, t.copies, t.places);
            if (have) {
                printf("plug in next:\n");
                print_risk_help(NULL, &h);
            }
        }
    } else if (strcmp(cmd, "status") == 0) {
        int64_t only = 0;
        if (npos > 0 && !(only = media_id_by_label(cat, pos[0]))) { bd_catalog_close(cat); return 1; }
        status_ctx sc = {cat, only, 0};
        if (g_json) J_ARR(NULL);
        bd_status s = bd_list_media(cat, print_status, &sc);
        if (g_json) J_END_ARR();
        if (s != BD_OK) return die(cat, "cannot read the drives");
        if (!sc.shown && !g_json) puts("no drives yet (drive <catalog> <root> <label> sets one up)");
    } else if (strcmp(cmd, "export") == 0) {
        if (npos < 1) { usage(); bd_catalog_close(cat); return 1; }
        int64_t rows = 0;
        if (bd_export_csv(cat, pos[0], &rows) != BD_OK) return die(cat, "cannot export the file list");
        if (g_json) { J_OBJ(NULL); j_str("file", pos[0]); j_int("rows", rows); J_END(); }
        else printf("wrote %lld files to %s\n", (long long)rows, pos[0]);
    } else if (strcmp(cmd, "skip") == 0) {
        int64_t root = root_opt(cat, argc, argv);
        if (root < 0) { bd_catalog_close(cat); return 1; }
        char *list = root ? bd_source_skip_get(cat, root) : bd_skip_list_get(cat);
        bd_status st = BD_OK;
        const char *add = opt(argc, argv, "--add"), *file = opt(argc, argv, "--set");
        int reset = has_flag(argc, argv, "--reset");
        if (reset) {
            st = root ? bd_source_skip_set(cat, root, NULL) : bd_skip_list_set(cat, NULL);
        } else if (add && list) {
            char *more = malloc(strlen(list) + strlen(add) + 3);
            if (more) {
                size_t n = strlen(list);
                sprintf(more, "%s%s%s\n", list, n && list[n - 1] != '\n' ? "\n" : "", add);
                st = root ? bd_source_skip_set(cat, root, more) : bd_skip_list_set(cat, more);
                free(more);
            }
        } else if (file) {
            FILE *f = fopen(file, "rb");
            char buf[16384];
            size_t n = f ? fread(buf, 1, sizeof(buf) - 1, f) : 0;
            if (f) fclose(f);
            if (!f) { free(list); return refuse(cat, "cannot read that file"); }
            buf[n] = '\0';
            st = root ? bd_source_skip_set(cat, root, buf) : bd_skip_list_set(cat, buf);
        }
        if (st != BD_OK) { free(list); return die(cat, "cannot change the skip list"); }
        if (add || file || reset) {
            free(list);
            list = root ? bd_source_skip_get(cat, root) : bd_skip_list_get(cat);
            save = 1;
            fprintf(msg_out(), "files already in the catalog that this leaves out are marked deleted at the next scan\n");
        }
        if (g_json) { J_OBJ(NULL); j_str("patterns", list ? list : ""); J_END(); }
        else fputs(list ? list : "", stdout);
        free(list);
    } else {
        usage();
        bd_catalog_close(cat);
        return 1;
    }

    if (save && bd_catalog_save(cat) != BD_OK) return die(cat, "cannot save catalog");
    g_cat = NULL;
    bd_catalog_close(cat);
    return 0;
}

#ifdef _WIN32
int main(void)
{
    SetConsoleOutputCP(CP_UTF8);
    int argc;
    wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    char **argv = calloc((size_t)argc + 1, sizeof(char *));
    for (int i = 0; wargv && argv && i < argc; i++) {
        int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
        argv[i] = malloc((size_t)n);
        WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, argv[i], n, NULL, NULL);
    }
    LocalFree(wargv);
    return run(argc, argv);
}
#else
int main(int argc, char **argv)
{
    return run(argc, argv);
}
#endif
