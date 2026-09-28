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
#endif

static void usage(void)
{
    puts("usage: brodalf-cli <command> <catalog.brodalf> [arguments]\n"
         "\n"
         "  new      <catalog>                         create a new catalog\n"
         "  add      <catalog> <folder>...             add folders to protect\n"
         "  sources  <catalog>                         list protected folders\n"
         "  scan     <catalog>                         record files, sizes and checksums\n"
         "  drive    <catalog> <root> <label>          set up a drive or folder as storage\n"
         "  backup   <catalog> <root> [--source NAME]  copy what is missing to a drive\n"
         "  check    <catalog> <root> [--full]         check the copies on a drive\n"
         "  tree     <catalog> [--drive ROOT]...       show the ghost tree\n"
         "  versions <catalog> <source> <path> [--drive ROOT]...\n"
         "                                             show versions and copies of a file\n"
         "  restore  <catalog> <dest> [--source NAME] [--path REL] --drive ROOT...\n"
         "                                             restore current versions from drives\n"
         "\n"
         "--drive connects a drive for this command. Files are shown as available\n"
         "only when a correct copy is on a connected drive.");
}

static void log_line(void *ctx, const char *msg)
{
    (void)ctx;
    printf("  %s\n", msg);
}

static int die(bd_catalog *cat, const char *what)
{
    fprintf(stderr, "brodalf: %s: %s\n", what, cat ? bd_catalog_error(cat) : bd_open_error());
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

static int connect_drives(bd_catalog *cat, int argc, char **argv)
{
    for (int i = 0; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--drive") != 0) continue;
        bd_check_stats cs;
        int64_t id;
        if (bd_media_connect(cat, argv[i + 1], &id, &cs, log_line, NULL) != BD_OK) {
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
    if (c->media_id) printf("  on %s (%s%s) %s", c->media_label, c->copy_state, c->connected ? ", connected" : ", offline", c->path_on_media);
    else printf("  no copy");
    printf("\n");
    return 0;
}

static int run(int argc, char **argv)
{
    if (argc < 3) { usage(); return argc < 2 ? 1 : (strcmp(argv[1], "help") == 0 ? 0 : 1); }
    const char *cmd = argv[1], *path = argv[2];
    bd_catalog *cat = NULL;

    if (strcmp(cmd, "new") == 0) {
        if (bd_catalog_create(path, &cat) != BD_OK) return die(NULL, "cannot create catalog");
        printf("created %s (catalog %s)\n", path, bd_catalog_uuid(cat));
        bd_catalog_close(cat);
        return 0;
    }

    if (bd_catalog_open(path, &cat) != BD_OK) return die(NULL, "cannot open catalog");
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
        if (bd_media_init(cat, argv[3], argv[4], &id) != BD_OK) return die(cat, "cannot set up drive");
        printf("set up %s as \"%s\"\n", argv[3], argv[4]);
        save = 1;
    } else if (strcmp(cmd, "backup") == 0 || strcmp(cmd, "check") == 0) {
        if (argc < 4) { usage(); bd_catalog_close(cat); return 1; }
        int64_t media_id;
        bd_check_stats cs;
        if (bd_media_connect(cat, argv[3], &media_id, &cs, log_line, NULL) != BD_OK) return die(cat, "cannot use drive");
        if (strcmp(cmd, "check") == 0) {
            int full = has_flag(argc, argv, "--full");
            if (full && bd_media_check(cat, media_id, 1, &cs, log_line, NULL) != BD_OK) return die(cat, "check failed");
            printf("%s check: %lld copies, %lld ok, %lld missing, %lld damaged\n", full ? "full" : "quick",
                   (long long)cs.copies, (long long)cs.ok, (long long)cs.missing, (long long)cs.bad);
        } else {
            int64_t source_id = 0;
            const char *name = opt(argc, argv, "--source");
            if (name && !(source_id = source_id_by_name(cat, name))) {
                fprintf(stderr, "brodalf: no protected folder named %s\n", name);
                bd_catalog_close(cat);
                return 1;
            }
            bd_backup_stats bs;
            if (bd_backup(cat, media_id, source_id, &bs, log_line, NULL) != BD_OK) return die(cat, "backup failed");
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

    int rc = 0;
    if (save && bd_catalog_save(cat) != BD_OK) {
        fprintf(stderr, "brodalf: cannot save catalog: %s\n", bd_catalog_error(cat));
        rc = 1;
    }
    bd_catalog_close(cat);
    return rc;
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
