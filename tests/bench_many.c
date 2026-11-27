/* Many files: how long a scan, a second scan, a moved folder, a backup and
 * a check take, and that every number comes out right. The file count is
 * the first argument (default 20000); "bench_many 1000000" tries a million.
 * Run by ctest with a small count so it stays quick; run it by hand for
 * the big numbers. */
#include "brodalf.h"
#include "internal.h"
#include "test_tree.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond);      \
            failures++;                                                                   \
        }                                                                                 \
    } while (0)

#define REQUIRE_OK(expr, cat)                                                             \
    do {                                                                                  \
        bd_status _s = (expr);                                                            \
        if (_s != BD_OK) {                                                                \
            fprintf(stderr, "%s:%d: %s -> %s: %s\n", __FILE__, __LINE__, #expr,            \
                    bd_status_name(_s), (cat) ? bd_catalog_error(cat) : bd_open_error()); \
            exit(1);                                                                      \
        }                                                                                 \
    } while (0)

static char base[1024];

static char *at(const char *rel)
{
    static char buf[4][2048];
    static int i = 0;
    i = (i + 1) % 4;
    snprintf(buf[i], sizeof(buf[i]), "%s/%s", base, rel);
    return buf[i];
}

static void quiet(void *ctx, const char *msg)
{
    (void)ctx;
    (void)msg;
}

static int64_t t0;

static void lap(const char *what, int64_t files)
{
    int64_t ms = bd_now_ms() - t0;
    double rate = ms > 0 ? (double)files * 1000.0 / (double)ms : 0.0;
    printf("%-28s %8lld ms  %10.0f files/s\n", what, (long long)ms, rate);
    fflush(stdout);
    t0 = bd_now_ms();
}

/* The test folders all sit on one disk; tests not about disks treat every
 * drive as a disk of its own, as before copies were counted by disk. */
static int no_disk_known(const char *root, bd_drive_hw *hw)
{
    (void)root;
    memset(hw, 0, sizeof(*hw));
    return -1;
}

int main(int argc, char **argv)
{
    bd_test_hw_read = no_disk_known;
    long n = argc > 1 ? atol(argv[1]) : 20000;
    if (n < 100) n = 100;
    char tmp[512], id[37];
    bd_temp_dir(tmp, sizeof(tmp));
    bd_uuid_v4(id);
    snprintf(base, sizeof(base), "%s/brodalf-bench-%.8s", tmp, id);
    for (char *p = base; *p; p++) if (*p == '\\') *p = '/';
    bd_mkdirs(base);
    printf("%ld files in %s\n", n, base);

    /* 100 files to a folder, 100 folders to a folder above that. */
    t0 = bd_now_ms();
    char rel[256], text[96];
    for (long i = 0; i < n; i++) {
        if (i % 100 == 0) {
            snprintf(rel, sizeof(rel), "src/Root/d%03ld/e%03ld", (i / 10000) % 1000, (i / 100) % 100);
            bd_mkdirs(at(rel));
        }
        snprintf(rel, sizeof(rel), "src/Root/d%03ld/e%03ld/f%07ld.txt", (i / 10000) % 1000, (i / 100) % 100, i);
        int len = snprintf(text, sizeof(text), "file %ld of the benchmark", i);
        FILE *f = bd_fopen(at(rel), "wb");
        if (!f || fwrite(text, 1, (size_t)len, f) != (size_t)len) {
            fprintf(stderr, "cannot write %s\n", rel);
            return 1;
        }
        fclose(f);
    }
    lap("write the files", n);

    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("bench.brodalf"), &cat), NULL);
    int64_t src, drive;
    REQUIRE_OK(bd_source_add(cat, at("src/Root"), &src), cat);
    bd_scan_stats ss;
    t0 = bd_now_ms();
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    lap("first scan", n);
    CHECK(ss.files_new == n && ss.errors == 0);

    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    lap("scan again, nothing changed", n);
    CHECK(ss.files_new == 0 && ss.files_changed == 0 && ss.bytes_hashed == 0);

    REQUIRE_OK(bd_catalog_save(cat), cat);
    lap("save the catalog", n);

    /* A folder of 100 files moves: they keep their copies. */
    CHECK(bd_rename_noreplace(at("src/Root/d000/e000"), at("src/Root/d000/moved")) == 0);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    lap("scan after a move", n);
    CHECK(ss.files_moved == 100 && ss.files_new == 0 && ss.files_deleted == 0);

    bd_mkdirs(at("drive"));
    REQUIRE_OK(bd_media_init(cat, at("drive"), "Bench", 0, &drive), cat);
    bd_backup_preview_info pv;
    t0 = bd_now_ms();
    REQUIRE_OK(bd_backup_preview(cat, drive, 0, &pv), cat);
    lap("what a backup would do", n);
    CHECK(pv.files_to_copy == n);

    bd_set_progress sets[2];
    int nsets = 0;
    REQUIRE_OK(bd_backup_sets(cat, sets, 2, &nsets), cat);
    lap("backup sets", n);
    CHECK(nsets == 2 && sets[0].files_total == n && sets[0].files_done == 0);

    bd_backup_stats bs;
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    lap("back up", n);
    CHECK(bs.files_copied == n && bs.files_failed == 0);

    REQUIRE_OK(bd_backup_sets(cat, sets, 2, &nsets), cat);
    CHECK(sets[0].files_done == n && sets[1].files_done == 0);

    bd_media_status ms;
    t0 = bd_now_ms();
    REQUIRE_OK(bd_media_status_get(cat, drive, &ms), cat);
    lap("drive status", n);
    CHECK(ms.files_current == n && ms.files_behind == 0 && ms.files_not_here == 0);

    REQUIRE_OK(bd_backup_preview(cat, drive, 0, &pv), cat);
    lap("what a backup would do now", n);
    CHECK(pv.files_to_copy == 0 && pv.files_held == n);

    bd_check_stats cs;
    t0 = bd_now_ms();
    REQUIRE_OK(bd_media_check(cat, drive, &cs, quiet, NULL), cat);
    lap("check the drive", n);
    CHECK(cs.copies == n && cs.ok == n);

    bd_risk_stats rk;
    REQUIRE_OK(bd_list_at_risk(cat, 0, NULL, NULL, &rk), cat);
    lap("files short of the target", n);
    CHECK(rk.files_at_risk == n);

    bd_catalog_close(cat);
    remove_tree(base);
    bd_stat_t gone;
    CHECK(bd_stat(base, &gone) != 0);
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("all benchmark checks passed");
    return 0;
}
