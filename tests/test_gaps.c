/* The gaps the 0.11.0 use-case review found: each test fails on 0.11.0. */
#include "brodalf.h"
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                        \
        }                                                                      \
    } while (0)

#define REQUIRE_OK(expr, cat)                                                  \
    do {                                                                       \
        bd_status _s = (expr);                                                 \
        if (_s != BD_OK) {                                                     \
            fprintf(stderr, "%s:%d: %s -> %s: %s\n", __FILE__, __LINE__, #expr, \
                    bd_status_name(_s), (cat) ? bd_catalog_error(cat) : bd_open_error()); \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

static char base[1024];

static char *at(const char *rel)
{
    static char buf[8][2048];
    static int i = 0;
    i = (i + 1) % 8;
    snprintf(buf[i], sizeof(buf[i]), "%s/%s", base, rel);
    return buf[i];
}

static void write_file(const char *path, const char *text)
{
    char *dir = bd_strdup(path);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = '\0'; bd_mkdirs(dir); }
    free(dir);
    FILE *f = bd_fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); exit(1); }
    fputs(text, f);
    fclose(f);
}

static int exists(const char *path)
{
    bd_stat_t st;
    return bd_stat(path, &st) == 0;
}

static void quiet(void *ctx, const char *msg)
{
    (void)ctx;
    (void)msg;
}

/* Lines the engine logged that hold ctx->want. */
typedef struct { const char *want; int n; } log_count;

static void count_log(void *ctx, const char *msg)
{
    log_count *c = ctx;
    if (strstr(msg, c->want)) c->n++;
}

typedef struct { int64_t id; int missing, waiting; int64_t gone, total; } root_info;

static int root_cb(void *ctx, const bd_source_info *s)
{
    root_info *r = ctx;
    if (s->source_id != r->id) return 0;
    r->missing = s->missing;
    r->gone = s->files_gone;
    r->total = s->files_total;
    r->waiting = s->wait_for_disk;
    return 1;
}

static root_info root_of(bd_catalog *cat, int64_t id)
{
    root_info r;
    memset(&r, 0, sizeof(r));
    r.id = id;
    bd_list_sources(cat, root_cb, &r);
    return r;
}

/* A folder of n small files, each with its own contents. */
static void make_files(const char *dir, int n, const char *tag)
{
    for (int i = 0; i < n; i++) {
        char rel[256], text[128];
        snprintf(rel, sizeof(rel), "%s/f%03d.txt", dir, i);
        snprintf(text, sizeof(text), "%s file %d", tag, i);
        write_file(at(rel), text);
    }
}

/* ---- Recovery 1: a new PC with the same user name ----------------------- */

static void test_new_pc_empty_folders(void)
{
    make_files("newpc/Documents", 30, "doc");
    bd_mkdirs(at("newpc/drive"));
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("newpc/db.brodalf"), &cat), NULL);
    int64_t src, drive;
    REQUIRE_OK(bd_source_add(cat, at("newpc/Documents"), &src), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_new == 30);
    REQUIRE_OK(bd_media_init(cat, at("newpc/drive"), "Blue", 0, &drive), cat);
    bd_backup_stats bs;
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 30);

    /* A few deleted on purpose: marked deleted as before. */
    bd_remove(at("newpc/Documents/f000.txt"));
    bd_remove(at("newpc/Documents/f001.txt"));
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_deleted == 2);
    CHECK(ss.roots_missing == 0 && ss.roots_emptied == 0);
    CHECK(!root_of(cat, src).missing);

    /* Everything moved into a subfolder: moves, not an emptied root. */
    bd_mkdirs(at("newpc/Documents/old"));
    for (int i = 2; i < 30; i++) {
        char a[64], b[64];
        snprintf(a, sizeof(a), "newpc/Documents/f%03d.txt", i);
        snprintf(b, sizeof(b), "newpc/Documents/old/f%03d.txt", i);
        char *from = bd_strdup(at(a));
        CHECK(bd_rename_noreplace(from, at(b)) == 0);
        free(from);
    }
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.roots_emptied == 0);
    CHECK(ss.files_moved == 28);

    /* A new PC: Windows made an empty Documents under the same name, with
     * one file of its own in it. */
    char *moved = bd_strdup(at("newpc/Documents-old-pc"));
    CHECK(bd_rename_noreplace(at("newpc/Documents"), moved) == 0);
    write_file(at("newpc/Documents/desktop.ini"), "[.ShellClassInfo]");
    log_count lc = {"A new PC", 0};
    REQUIRE_OK(bd_scan(cat, &ss, count_log, &lc), cat);
    CHECK(ss.files_deleted == 0); /* 0.11.0: 28 */
    CHECK(ss.roots_missing == 1 && ss.roots_emptied == 1);
    CHECK(ss.files_new == 0);
    CHECK(lc.n == 1);
    root_info r = root_of(cat, src);
    CHECK(r.missing && r.gone == 28 && r.total == 28);

    /* "Restore everything" brings every file back. */
    bd_restore_stats rs;
    REQUIRE_OK(bd_restore(cat, src, NULL, at("newpc/restored"), &rs, quiet, NULL), cat);
    CHECK(rs.files_restored == 28); /* 0.11.0: 0 */
    CHECK(exists(at("newpc/restored/old/f029.txt")));

    /* The user says the files really were deleted: the next scan marks them. */
    REQUIRE_OK(bd_source_accept_gone(cat, src), cat);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_deleted == 28);
    CHECK(ss.roots_missing == 0);
    CHECK(!root_of(cat, src).missing);
    free(moved);
    bd_catalog_close(cat);
}

/* ---- Recovery 6: a backup root on a disk that is usually unplugged ------ */

static void test_root_waits_for_disk(void)
{
    make_files("usb/Photos", 3, "photo");
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("usb/db.brodalf"), &cat), NULL);
    int64_t src;
    REQUIRE_OK(bd_source_add(cat, at("usb/Photos"), &src), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    char *away = bd_strdup(at("usb/Photos-unplugged"));

    /* Unplugged: missing, and asked about every time. */
    CHECK(bd_rename_noreplace(at("usb/Photos"), away) == 0);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.roots_missing == 1 && ss.roots_waiting == 0);

    /* "Skip it until the disk is plugged in": still missing, but waiting. */
    REQUIRE_OK(bd_source_wait_for_disk(cat, src, 1), cat);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.roots_missing == 1 && ss.roots_waiting == 1);
    CHECK(ss.files_deleted == 0);
    root_info r = root_of(cat, src);
    CHECK(r.missing && r.waiting);

    /* The wait is kept in the catalog. */
    REQUIRE_OK(bd_catalog_save(cat), cat);
    bd_catalog_close(cat);
    REQUIRE_OK(bd_catalog_open(at("usb/db.brodalf"), &cat), NULL);
    CHECK(root_of(cat, src).waiting);

    /* Plugged in again: scanned as usual, and the wait ends. */
    CHECK(bd_rename_noreplace(away, at("usb/Photos")) == 0);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.roots_missing == 0 && ss.roots_waiting == 0);
    CHECK(!root_of(cat, src).waiting);
    CHECK(bd_rename_noreplace(at("usb/Photos"), away) == 0);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.roots_missing == 1 && ss.roots_waiting == 0);
    free(away);
    bd_catalog_close(cat);
}

int main(void)
{
    char tmp[512], id[37];
    bd_temp_dir(tmp, sizeof(tmp));
    bd_uuid_v4(id);
    snprintf(base, sizeof(base), "%s/brodalf-gaps-%.8s", tmp, id);
    for (char *p = base; *p; p++) if (*p == '\\') *p = '/';
    bd_mkdirs(base);
    printf("working in %s\n", base);

    test_new_pc_empty_folders();
    test_root_waits_for_disk();

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("all gap checks passed\n");
    return 0;
}
