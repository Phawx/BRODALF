/* brodalf-cli: a command-line harness for the core library. The GUI is the
 * main way to use BRODALF; this exists to drive and test the engine. */
#include "brodalf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

static void usage(void)
{
    puts("usage: brodalf-cli <command> <catalog.brodalf> [arguments]\n"
         "\n"
         "  new      <catalog>                         create a new catalog\n"
         "  add      <catalog> <folder>...             add folders to protect\n"
         "  sources  <catalog>                         list protected folders\n"
         "  scan     <catalog>                         record files, sizes and checksums\n"
         "  drive    <catalog> <root> <label> [--encrypt] [--location TEXT]\n"
         "                                             set up a drive or folder as storage\n"
         "  backup   <catalog> <root> [--source NAME]  copy what is missing to a drive\n"
         "  check    <catalog> <root> [--full]         check the copies on a drive\n"
         "  tree     <catalog> [--drive ROOT]...       show the ghost tree\n"
         "  versions <catalog> <source> <path> [--drive ROOT]...\n"
         "                                             show versions and copies of a file\n"
         "  restore  <catalog> <dest> [--source NAME] [--path REL] --drive ROOT...\n"
         "                                             restore current versions from drives\n"
         "  cloud-add <catalog> onedrive|dropbox <label> [--encrypt]\n"
         "                                             sign in and use a cloud account as storage\n"
         "  cloud-signout <catalog> <label>            forget a cloud account's saved sign-in\n"
         "  drives   <catalog>                         list drives with make, model, serial, health, location\n"
         "  target   <catalog> [copies places]         show or set how many copies, in how many places\n"
         "  at-risk  <catalog> [--source NAME] [--all] files short of the target, and which drive helps\n"
         "  drive-location <catalog> <label> [text]    say where a drive is kept (no text clears it)\n"
         "  drive-rename <catalog> <label> <new label> rename a drive\n"
         "  passphrase <catalog>                       set or change the passphrase\n"
         "  encrypt-catalog <catalog> on|off           encrypt the .brodalf file itself\n"
         "\n"
         "--drive connects a drive for this command. Files are shown as available\n"
         "only when a correct copy is on a connected drive. Wherever a drive root\n"
         "goes, cloud:LABEL names a cloud account added with cloud-add.\n"
         "\n"
         "When something fails, an error report is saved with instructions for\n"
         "posting it as a GitHub issue. The app log is kept in the file named by\n"
         "BRODALF_LOG, or %LOCALAPPDATA%\\BRODALF\\brodalf.log (Windows) and\n"
         "~/.local/state/brodalf/brodalf.log (elsewhere).\n"
         "\n"
         "Passphrases are asked for on the terminal, or taken from the environment\n"
         "variables BRODALF_PASSPHRASE and (when setting one) BRODALF_NEW_PASSPHRASE.");
}

static void log_line(void *ctx, const char *msg)
{
    (void)ctx;
    printf("  %s\n", msg);
    bd_applog("%s", msg);
}

/* Print the error, save a report file and say how to post it. */
static int die(bd_catalog *cat, const char *what)
{
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
    return f.id;
}

static int print_source(void *ctx, const bd_source_info *info)
{
    (void)ctx;
    printf("%-20s %s\n", info->name, info->path);
    return 0;
}

typedef struct { const char *label; int64_t id; int cloud; } find_media_ctx;

static int find_any_media_cb(void *ctx, const bd_media_info *info)
{
    find_media_ctx *f = ctx;
    if (strcmp(info->label, f->label) == 0) { f->id = info->media_id; return 1; }
    return 0;
}

static int64_t media_id_by_label(bd_catalog *cat, const char *label)
{
    find_media_ctx f = {label, 0, 0};
    bd_list_media(cat, find_any_media_cb, &f);
    if (!f.id) fprintf(stderr, "brodalf: no drive labelled \"%s\"\n", label);
    return f.id;
}

static void human_bytes(int64_t n, char *out, size_t cap)
{
    const char *units[] = {"bytes", "KB", "MB", "GB", "TB", "PB"};
    double v = (double)n;
    int u = 0;
    while (v >= 1000 && u < 5) { v /= 1000; u++; }
    snprintf(out, cap, u ? "%.1f %s" : "%.0f %s", v, units[u]);
}

typedef struct { int shown, limit; } risk_print;

static int print_risk(void *ctx, const bd_risk_info *r)
{
    risk_print *p = ctx;
    if (p->limit && p->shown >= p->limit) return 1;
    p->shown++;
    char size[32];
    human_bytes(r->size, size, sizeof(size));
    if (r->copies == 0) printf("  no copy: ");
    else printf("  %d %s in %d %s: ", r->copies, r->copies == 1 ? "copy" : "copies", r->places, r->places == 1 ? "place" : "places");
    printf("%s/%s (%s)%s%s\n", r->source_name, r->rel_path, size,
           r->older_copies ? ", changed since its last backup" : "",
           r->unknown_place ? ", on a drive with no place set" : "");
    return 0;
}

static int print_risk_help(void *ctx, const bd_risk_help *h)
{
    (void)ctx;
    char size[32];
    human_bytes(h->bytes, size, sizeof(size));
    printf("  %s%s%s%s: would help %lld %s (%s)%s\n", h->label, h->location[0] ? " (kept in " : "", h->location,
           h->location[0] ? ")" : "", (long long)h->files, h->files == 1 ? "file" : "files", size, h->connected ? "  [connected]" : "");
    return 0;
}

static int print_drive(void *ctx, const bd_media_info *m)
{
    (void)ctx;
    printf("%s%s%s\n", m->label, m->connected ? "  (connected)" : "", m->encrypted ? "  [encrypted]" : "");
    if (m->location && *m->location) printf("  kept in:   %s\n", m->location);
    printf("  kind:      %s, last seen at %s\n", m->kind, m->last_root ? m->last_root : "?");
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
    printf("  copies:    %lld\n\n", (long long)m->copies);
    return 0;
}

static int find_media_cb(void *ctx, const bd_media_info *info)
{
    find_media_ctx *f = ctx;
    if (strcmp(info->label, f->label) == 0 && strcmp(info->kind, "drive") != 0) { f->id = info->media_id; return 1; }
    return 0;
}

static void open_browser(const char *url)
{
#ifdef _WIN32
    int n = MultiByteToWideChar(CP_UTF8, 0, url, -1, NULL, 0);
    wchar_t *w = malloc(sizeof(wchar_t) * (size_t)n);
    if (w) {
        MultiByteToWideChar(CP_UTF8, 0, url, -1, w, n);
        ShellExecuteW(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL);
        free(w);
    }
#else
    (void)url; /* the address is printed; open it by hand */
#endif
}

/* Connect a drive root, or a cloud account given as cloud:LABEL. */
static bd_status connect_target(bd_catalog *cat, const char *target, int64_t *id, bd_check_stats *cs)
{
    if (strncmp(target, "cloud:", 6) != 0) return bd_media_connect(cat, target, id, cs, log_line, NULL);
    find_media_ctx f = {target + 6, 0, 0};
    bd_list_media(cat, find_media_cb, &f);
    if (!f.id) {
        fprintf(stderr, "brodalf: no cloud storage labelled \"%s\"\n", target + 6);
        return BD_ERR_NOT_FOUND;
    }
    *id = f.id;
    bd_status s = bd_cloud_connect(cat, f.id, cs, log_line, NULL);
    if (s != BD_OK) fprintf(stderr, "brodalf: %s\n", bd_catalog_error(cat));
    return s;
}

static int connect_drives(bd_catalog *cat, int argc, char **argv)
{
    for (int i = 0; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--drive") != 0) continue;
        bd_check_stats cs;
        int64_t id;
        if (connect_target(cat, argv[i + 1], &id, &cs) != BD_OK) {
            fprintf(stderr, "brodalf: %s\n", bd_catalog_error(cat));
            return -1;
        }
        printf("connected %s: %lld copies, %lld ok, %lld missing, %lld damaged\n", argv[i + 1],
               (long long)cs.copies, (long long)cs.ok, (long long)cs.missing, (long long)cs.bad);
    }
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
    printf("%s%*s%s%s", marker(n->state), t->depth * 2, "", n->name, n->is_dir ? "/" : "");
    if (n->is_dir) printf("  (%lld of %lld available)", (long long)n->files_available, (long long)n->files_total);
    if (!n->is_dir && n->version_count > 1) printf("  v%d", n->version_no);
    if (n->state != BD_STATE_AVAILABLE && n->state != BD_STATE_PARTIAL) printf("  [%s", bd_node_state_name(n->state));
    if (n->state == BD_STATE_OFFLINE && n->offline_media_label) printf(": plug in %s", n->offline_media_label);
    if (n->state != BD_STATE_AVAILABLE && n->state != BD_STATE_PARTIAL) printf("]");
    printf("\n");
    if (n->is_dir) {
        tree_ctx child = {t->cat, t->depth + 1};
        bd_list_children(t->cat, n->source_id, n->node_id, print_node, &child);
    }
    return 0;
}

static int print_tree_source(void *ctx, const bd_source_info *info)
{
    bd_catalog *cat = ctx;
    printf("%s  (%s)  %lld of %lld available", info->name, info->path, (long long)info->files_available,
           (long long)info->files_total);
    if (info->offline_media_label) printf(", copies on %s", info->offline_media_label);
    printf("\n");
    tree_ctx t = {cat, 1};
    bd_list_children(cat, info->source_id, 0, print_node, &t);
    return 0;
}

static int print_copy(void *ctx, const bd_copy_info *c)
{
    (void)ctx;
    printf("v%-3d %s %12lld bytes  %.16s...", c->version_no, c->is_current ? "current" : "       ", (long long)c->size, c->hash);
    if (c->media_id) printf("  on %s (%s%s%s%s%s) %s", c->media_label, c->copy_state, c->connected ? ", connected" : ", offline",
                            c->encrypted ? ", encrypted" : "", c->media_location[0] ? ", kept in " : "", c->media_location,
                            c->path_on_media);
    else printf("  no copy");
    printf("\n");
    return 0;
}

static int run(int argc, char **argv)
{
    if (argc < 3) { usage(); return argc < 2 ? 1 : (strcmp(argv[1], "help") == 0 ? 0 : 1); }
    const char *cmd = argv[1], *path = argv[2];
    bd_catalog *cat = NULL;
    char *log_path = bd_applog_default_path();
    bd_applog_open(log_path);
    free(log_path);
    bd_applog("brodalf-cli %s: %s %s", bd_version(), cmd, path);

    if (strcmp(cmd, "new") == 0) {
        if (bd_catalog_create(path, &cat) != BD_OK) return die(NULL, "cannot create catalog");
        printf("created %s (catalog %s)\n", path, bd_catalog_uuid(cat));
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

    if (strcmp(cmd, "add") == 0) {
        for (int i = 3; i < argc; i++) {
            int64_t id;
            if (bd_source_add(cat, argv[i], &id) != BD_OK) return die(cat, "cannot add folder");
            printf("protecting %s\n", argv[i]);
        }
        save = 1;
    } else if (strcmp(cmd, "sources") == 0) {
        bd_list_sources(cat, print_source, NULL);
    } else if (strcmp(cmd, "scan") == 0) {
        bd_scan_stats st;
        if (bd_scan(cat, &st, log_line, NULL) != BD_OK) return die(cat, "scan failed");
        printf("scanned %lld files in %lld folders: %lld new, %lld changed, %lld deleted, %lld errors\n",
               (long long)st.files_seen, (long long)st.dirs_seen, (long long)st.files_new,
               (long long)st.files_changed, (long long)st.files_deleted, (long long)st.errors);
        save = 1;
    } else if (strcmp(cmd, "drive") == 0) {
        if (argc < 5) { usage(); bd_catalog_close(cat); return 1; }
        int64_t id;
        int encrypt = has_flag(argc, argv, "--encrypt");
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
        if (bd_media_init(cat, argv[3], argv[4], encrypt ? BD_MEDIA_ENCRYPTED : 0, &id) != BD_OK)
            return die(cat, "cannot set up drive");
        const char *location = opt(argc, argv, "--location");
        if (location && bd_media_set_location(cat, id, location) != BD_OK) return die(cat, "cannot set location");
        printf("set up %s as \"%s\"%s\n", argv[3], argv[4], encrypt ? ", encrypted" : "");
        save = 1;
    } else if (strcmp(cmd, "target") == 0) {
        bd_target t;
        bd_target_get(cat, &t);
        if (argc >= 5) {
            t.copies = atoi(argv[3]);
            t.places = atoi(argv[4]);
            if (bd_target_set(cat, &t) != BD_OK) {
                fprintf(stderr, "brodalf: %s\n", bd_catalog_error(cat));
                bd_catalog_close(cat);
                return 1;
            }
            save = 1;
        } else if (argc == 4) { usage(); bd_catalog_close(cat); return 1; }
        printf("target: %d %s of every file, in %d different %s\n", t.copies, t.copies == 1 ? "copy" : "copies",
               t.places, t.places == 1 ? "place" : "places");
    } else if (strcmp(cmd, "at-risk") == 0) {
        int64_t source_id = 0;
        const char *name = opt(argc, argv, "--source");
        if (name && !(source_id = source_id_by_name(cat, name))) {
            fprintf(stderr, "brodalf: no protected folder named %s\n", name);
            bd_catalog_close(cat);
            return 1;
        }
        bd_target t;
        bd_target_get(cat, &t);
        bd_risk_stats st;
        risk_print rp = {0, has_flag(argc, argv, "--all") ? 0 : 50};
        if (bd_list_at_risk(cat, source_id, NULL, NULL, &st) != BD_OK) return die(cat, "cannot list files at risk");
        char size[32];
        human_bytes(st.bytes_at_risk, size, sizeof(size));
        printf("target: %d %s in %d %s\n", t.copies, t.copies == 1 ? "copy" : "copies", t.places, t.places == 1 ? "place" : "places");
        printf("%lld of %lld files are short of it (%s), %lld with no copy at all\n", (long long)st.files_at_risk,
               (long long)st.files_total, size, (long long)st.files_no_copy);
        if (st.files_at_risk) {
            bd_list_at_risk(cat, source_id, print_risk, &rp, NULL);
            if (rp.limit && st.files_at_risk > rp.shown)
                printf("  ... and %lld more (--all lists them all)\n", (long long)(st.files_at_risk - rp.shown));
            puts("back up to these next:");
            bd_list_risk_help(cat, print_risk_help, NULL);
        }
    } else if (strcmp(cmd, "drives") == 0) {
        bd_list_media(cat, print_drive, NULL);
    } else if (strcmp(cmd, "drive-location") == 0 || strcmp(cmd, "drive-rename") == 0) {
        int rename = strcmp(cmd, "drive-rename") == 0;
        if (argc < (rename ? 5 : 4)) { usage(); bd_catalog_close(cat); return 1; }
        int64_t id = media_id_by_label(cat, argv[3]);
        if (!id) { bd_catalog_close(cat); return 1; }
        bd_status s = rename ? bd_media_rename(cat, id, argv[4]) : bd_media_set_location(cat, id, argc > 4 ? argv[4] : NULL);
        if (s != BD_OK) return die(cat, "cannot change the drive");
        if (rename) printf("renamed \"%s\" to \"%s\"\n", argv[3], argv[4]);
        else if (argc > 4) printf("\"%s\" is kept in: %s\n", argv[3], argv[4]);
        else printf("cleared where \"%s\" is kept\n", argv[3]);
        save = 1;
    } else if (strcmp(cmd, "cloud-add") == 0) {
        if (argc < 5) { usage(); bd_catalog_close(cat); return 1; }
        bd_cloud_provider prov = strcmp(argv[3], "onedrive") == 0 ? BD_CLOUD_ONEDRIVE
                                 : strcmp(argv[3], "dropbox") == 0 ? BD_CLOUD_DROPBOX : 0;
        if (!prov) { usage(); bd_catalog_close(cat); return 1; }
        int encrypt = has_flag(argc, argv, "--encrypt");
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
        bd_signin *si;
        const char *url;
        if (bd_cloud_signin_begin(cat, prov, &si, &url) != BD_OK) return die(cat, "cannot sign in");
        printf("Sign in with your browser. If it does not open, visit:\n\n  %s\n\n", url);
        fflush(stdout);
        open_browser(url);
        int64_t id;
        bd_status s = bd_cloud_signin_finish(cat, si, 5 * 60 * 1000);
        if (s == BD_OK) printf("signed in as %s\n", bd_cloud_signin_account(si));
        if (s == BD_OK) s = bd_cloud_add(cat, si, argv[4], encrypt ? BD_MEDIA_ENCRYPTED : 0, &id);
        bd_cloud_signin_free(si);
        if (s != BD_OK) return die(cat, "cannot add cloud storage");
        printf("added %s as \"%s\"%s; use it as cloud:%s\n", argv[3], argv[4], encrypt ? ", encrypted" : "", argv[4]);
        save = 1;
    } else if (strcmp(cmd, "cloud-signout") == 0) {
        if (argc < 4) { usage(); bd_catalog_close(cat); return 1; }
        find_media_ctx f = {argv[3], 0, 0};
        bd_list_media(cat, find_media_cb, &f);
        if (!f.id) { fprintf(stderr, "brodalf: no cloud storage labelled \"%s\"\n", argv[3]); bd_catalog_close(cat); return 1; }
        if (bd_cloud_sign_out(cat, f.id) != BD_OK) return die(cat, "cannot sign out");
        printf("signed out of \"%s\"; its files stay in the cloud\n", argv[3]);
        save = 1;
    } else if (strcmp(cmd, "passphrase") == 0) {
        if (ensure_unlocked(cat) != 0) { bd_catalog_close(cat); return 1; }
        char pass[512];
        if (get_new_passphrase(pass, sizeof(pass)) != 0) { bd_catalog_close(cat); return 1; }
        bd_status s = bd_catalog_set_passphrase(cat, pass);
        memset(pass, 0, sizeof(pass));
        if (s != BD_OK) return die(cat, "cannot set passphrase");
        puts("passphrase set");
        save = 1;
    } else if (strcmp(cmd, "encrypt-catalog") == 0) {
        if (argc < 4 || (strcmp(argv[3], "on") != 0 && strcmp(argv[3], "off") != 0)) { usage(); bd_catalog_close(cat); return 1; }
        int on = strcmp(argv[3], "on") == 0;
        if (on && !bd_catalog_has_passphrase(cat)) {
            fprintf(stderr, "brodalf: set a passphrase first (brodalf-cli passphrase %s)\n", path);
            bd_catalog_close(cat);
            return 1;
        }
        if (on && ensure_unlocked(cat) != 0) { bd_catalog_close(cat); return 1; }
        if (bd_catalog_set_file_encrypted(cat, on) != BD_OK) return die(cat, "cannot change catalog encryption");
        printf("the catalog file is %s\n", on ? "encrypted from now on" : "no longer encrypted");
        save = 1;
    } else if (strcmp(cmd, "backup") == 0 || strcmp(cmd, "check") == 0) {
        if (argc < 4) { usage(); bd_catalog_close(cat); return 1; }
        int64_t media_id;
        bd_check_stats cs;
        if (connect_target(cat, argv[3], &media_id, &cs) != BD_OK) return die(cat, "cannot use storage");
        if (strcmp(cmd, "check") == 0) {
            int full = has_flag(argc, argv, "--full");
            bd_status s = full ? bd_media_check(cat, media_id, 1, &cs, log_line, NULL) : BD_OK;
            if (s == BD_ERR_PASSPHRASE && ensure_unlocked(cat) == 0) s = bd_media_check(cat, media_id, 1, &cs, log_line, NULL);
            if (s != BD_OK) return die(cat, "check failed");
            printf("%s check: %lld copies, %lld ok, %lld missing, %lld damaged\n", full ? "full" : "quick",
                   (long long)cs.copies, (long long)cs.ok, (long long)cs.missing, (long long)cs.bad);
            if (cs.skipped) printf("%lld encrypted copies changed on the drive; run check --full to read them\n", (long long)cs.skipped);
        } else {
            int64_t source_id = 0;
            const char *name = opt(argc, argv, "--source");
            if (name && !(source_id = source_id_by_name(cat, name))) {
                fprintf(stderr, "brodalf: no protected folder named %s\n", name);
                bd_catalog_close(cat);
                return 1;
            }
            bd_backup_stats bs;
            bd_status s = bd_backup(cat, media_id, source_id, &bs, log_line, NULL);
            if (s == BD_ERR_PASSPHRASE && ensure_unlocked(cat) == 0) s = bd_backup(cat, media_id, source_id, &bs, log_line, NULL);
            if (s != BD_OK) return die(cat, "backup failed");
            printf("backup: %lld copied (%lld bytes), %lld already there, %lld failed, %lld older copies kept in .versions\n",
                   (long long)bs.files_copied, (long long)bs.bytes_copied, (long long)bs.files_already_there,
                   (long long)bs.files_failed, (long long)bs.versions_moved);
            if (bd_catalog_copy_to_media(cat, media_id) != BD_OK) fprintf(stderr, "brodalf: %s\n", bd_catalog_error(cat));
        }
        save = 1;
    } else if (strcmp(cmd, "tree") == 0) {
        if (connect_drives(cat, argc, argv) != 0) { bd_catalog_close(cat); return 1; }
        bd_list_sources(cat, print_tree_source, cat);
        puts("\n(blank) available  ~ older version only  + partly available  . greyed out  ! damaged  x deleted");
        save = 1;
    } else if (strcmp(cmd, "versions") == 0) {
        if (argc < 5) { usage(); bd_catalog_close(cat); return 1; }
        if (connect_drives(cat, argc, argv) != 0) { bd_catalog_close(cat); return 1; }
        int64_t source_id = source_id_by_name(cat, argv[3]), node_id = 0;
        if (!source_id) { fprintf(stderr, "brodalf: no protected folder named %s\n", argv[3]); bd_catalog_close(cat); return 1; }
        if (bd_find_node(cat, source_id, argv[4], &node_id) != BD_OK) return die(cat, "cannot find file");
        bd_list_copies(cat, node_id, print_copy, NULL);
        save = 1;
    } else if (strcmp(cmd, "restore") == 0) {
        if (argc < 4) { usage(); bd_catalog_close(cat); return 1; }
        if (connect_drives(cat, argc, argv) != 0) { bd_catalog_close(cat); return 1; }
        int64_t source_id = 0;
        const char *name = opt(argc, argv, "--source");
        if (name && !(source_id = source_id_by_name(cat, name))) {
            fprintf(stderr, "brodalf: no protected folder named %s\n", name);
            bd_catalog_close(cat);
            return 1;
        }
        if (ensure_unlocked(cat) != 0) { bd_catalog_close(cat); return 1; }
        bd_restore_stats rs;
        if (bd_restore(cat, source_id, opt(argc, argv, "--path"), argv[3], &rs, log_line, NULL) != BD_OK)
            return die(cat, "restore failed");
        printf("restore: %lld restored (%lld bytes), %lld on drives that are not connected, %lld never backed up, %lld failed\n",
               (long long)rs.files_restored, (long long)rs.bytes_restored, (long long)rs.files_offline,
               (long long)rs.files_no_copy, (long long)rs.files_failed);
        save = 1;
    } else {
        usage();
        bd_catalog_close(cat);
        return 1;
    }

    if (save && bd_catalog_save_all(cat, log_line, NULL) != BD_OK) return die(cat, "cannot save catalog");
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
