/* The drives findings of the 0.14.0 flow review: each test fails on 0.14.0
 * where that can be shown without a real PC. */
#include "brodalf.h"
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#else
#include <windows.h>
#endif

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

static void copy_file(const char *from, const char *to)
{
    FILE *in = bd_fopen(from, "rb"), *out = bd_fopen(to, "wb");
    if (!in || !out) { fprintf(stderr, "cannot copy %s\n", from); exit(1); }
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, n, out);
    fclose(in);
    fclose(out);
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

static void set_old(const char *path, int64_t hours)
{
    FILE *f = bd_fopen(path, "r+b");
    if (!f) return;
    bd_set_mtime(f, (bd_now_ms() - hours * 3600 * 1000) * 1000000);
    fclose(f);
}

typedef struct { char paths[16][2048]; int64_t revs[16]; int n; } revs_seen;

static int revs_cb(void *ctx, const bd_catalog_copy_info *c)
{
    revs_seen *r = ctx;
    if (r->n < 16) {
        snprintf(r->paths[r->n], sizeof(r->paths[0]), "%s", c->path);
        r->revs[r->n++] = c->revision;
    }
    return 0;
}

static revs_seen revisions_on(bd_catalog *cat, const char *root)
{
    char dir[2048];
    snprintf(dir, sizeof(dir), "%s/BRODALF/%s", root, bd_catalog_uuid(cat));
    revs_seen r;
    memset(&r, 0, sizeof(r));
    bd_find_catalog_revisions(dir, revs_cb, &r);
    return r;
}

static int newest_cb(void *ctx, const bd_catalog_copy_info *c)
{
    snprintf((char *)ctx, 2048, "%s", c->path);
    return 1;
}

/* ---- D3: two copies of one database reach the same revision --------------- */

static void test_two_copies_same_revision(void)
{
    make_files("d3-src/Docs", 2, "d3");
    bd_mkdirs(at("d3/A"));
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("d3/db.brodalf"), &cat), NULL);
    int64_t src, a;
    REQUIRE_OK(bd_source_add(cat, at("d3-src/Docs"), &src), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_media_init(cat, at("d3/A"), "A", 0, &a), cat);
    bd_backup_stats bs;
    REQUIRE_OK(bd_backup(cat, a, 0, &bs, quiet, NULL), cat);
    bd_catalog_close(cat);
    /* An older copy of the database, as when it is recovered from a drive
     * that missed the last backups. */
    copy_file(at("d3/db.brodalf"), at("d3/older.brodalf"));

    REQUIRE_OK(bd_catalog_open(at("d3/db.brodalf"), &cat), NULL);
    int64_t id;
    REQUIRE_OK(bd_media_connect(cat, at("d3/A"), &id, quiet, NULL), cat);
    write_file(at("d3-src/Docs/later.txt"), "a later file");
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_backup(cat, a, 0, &bs, quiet, NULL), cat);
    int64_t newest = bd_catalog_revision(cat);
    revs_seen r1 = revisions_on(cat, at("d3/A"));
    /* The same database copied again with nothing changed: no second copy. */
    REQUIRE_OK(bd_catalog_copy_to_media(cat, a), cat);
    CHECK(revisions_on(cat, at("d3/A")).n == r1.n);
    CHECK(!bd_media_copy_clash(cat, a, NULL));
    bd_catalog_close(cat);
    set_old(r1.paths[0], 1);

    /* The older copy sees that the drive has a newer one. */
    bd_catalog *old;
    REQUIRE_OK(bd_catalog_open(at("d3/older.brodalf"), &old), NULL);
    REQUIRE_OK(bd_media_connect(old, at("d3/A"), &id, quiet, NULL), old);
    bd_drive_catalog dc;
    REQUIRE_OK(bd_media_catalog_compare(old, a, &dc), old);
    CHECK(dc.newer == 1 && dc.revision == newest && dc.separate == 0);
    CHECK(dc.open_revision < newest && dc.open_saved_ms > 0 && dc.written_ms > 0);
    CHECK(dc.path && strstr(dc.path, "catalog-r") != NULL);
    free(dc.path);

    /* Used on, it reaches the same revision: its copy goes beside the
     * other, which stays as it was. */
    while (bd_catalog_revision(old) < newest - 1) {
        bd_catalog_mark_changed(old);
        REQUIRE_OK(bd_catalog_save(old), old);
    }
    bd_catalog_mark_changed(old);
    REQUIRE_OK(bd_catalog_copy_to_media(old, a), old);
    CHECK(bd_catalog_revision(old) == newest);
    revs_seen r2 = revisions_on(old, at("d3/A"));
    CHECK(r2.n == r1.n + 1); /* 0.14.0: the same, its copy left out without a word */
    CHECK(r2.n >= 2 && r2.revs[0] == newest && r2.revs[1] == newest);
    CHECK(r2.n >= 1 && strchr(strrchr(r2.paths[0], '/') + 10, '-') != NULL && strcmp(r2.paths[0], r1.paths[0]) != 0);
    CHECK(exists(r1.paths[0]));
    int64_t rev = 0;
    CHECK(bd_media_copy_clash(old, a, &rev) == 1 && rev == newest);
    REQUIRE_OK(bd_media_catalog_compare(old, a, &dc), old);
    CHECK(dc.separate == 1 && dc.newer == 0 && dc.revision == newest);
    free(dc.path);
    /* Copied again unchanged: no third copy. */
    REQUIRE_OK(bd_catalog_copy_to_media(old, a), old);
    CHECK(revisions_on(old, at("d3/A")).n == r2.n);
    /* Recover takes the one written last; both open. */
    char found[2048] = "";
    bd_find_catalog_copies(at("d3/A"), newest_cb, found);
    CHECK(strcmp(found, r2.paths[0]) == 0);
    CHECK(bd_catalog_copy_readable(old, r2.paths[0]) == BD_OK);
    CHECK(bd_catalog_copy_readable(old, r2.paths[1]) == BD_OK);
    bd_catalog_close(old);
    char dest[2048];
    snprintf(dest, sizeof(dest), "%s", at("d3/recovered.brodalf"));
    REQUIRE_OK(bd_catalog_recover(r2.paths[0], dest), NULL);
    REQUIRE_OK(bd_catalog_open(dest, &old), NULL);
    CHECK(bd_catalog_revision(old) == newest);
    bd_catalog_close(old);

    /* The names. */
    CHECK(bd_path_is_drive_copy("E:\\Backups\\catalog-r00000012-1a2b3c4d.brodalf")); /* 0.14.0: no */
    CHECK(bd_path_is_drive_copy("E:\\Backups\\CATALOG-R00000012-1A2B3C4D.BRODALF"));
    CHECK(!bd_path_is_drive_copy("E:\\Backups\\catalog-r00000012-.brodalf"));
    CHECK(!bd_path_is_drive_copy("E:\\Backups\\catalog-r-1a2b3c4d.brodalf"));
    CHECK(!bd_path_is_drive_copy("E:\\Backups\\catalog-r12-xyz.brodalf"));
    CHECK(!bd_path_is_drive_copy("E:\\Backups\\catalog-r12-1a-2b.brodalf"));
}

int main(void)
{
    char tmp[512], id[37];
    bd_temp_dir(tmp, sizeof(tmp));
    bd_uuid_v4(id);
    snprintf(base, sizeof(base), "%s/brodalf-flow-drives-%.8s", tmp, id);
    for (char *p = base; *p; p++) if (*p == '\\') *p = '/';
    bd_mkdirs(base);
    printf("working in %s\n", base);

    test_two_copies_same_revision();

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("all flow drives checks passed\n");
    return 0;
}
