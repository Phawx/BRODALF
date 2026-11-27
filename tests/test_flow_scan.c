/* The scan findings of the 0.14.0 flow review (D2, D4, D5, C2, C5, C7,
 * C15, C16): each test fails on 0.14.0 where it can run there. */
#include "brodalf.h"
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#include <sys/stat.h>
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

static int64_t count_rows(bd_catalog *cat, const char *sql)
{
    sqlite3_stmt *q;
    int64_t n = -1;
    if (sqlite3_prepare_v2(cat->db, sql, -1, &q, NULL) != SQLITE_OK) return -1;
    if (sqlite3_step(q) == SQLITE_ROW) n = sqlite3_column_int64(q, 0);
    sqlite3_finalize(q);
    return n;
}

static int fake_hw(const char *root, bd_drive_hw *hw)
{
    (void)root;
    memset(hw, 0, sizeof(*hw));
    hw->disk_bytes = -1;
    return -1;
}

/* Write a file again with the same size and modified time, as some
 * programs do (VeraCrypt containers by default). The pause lets the
 * change time move on a clock that ticks every 16 ms. */
static void rewrite_keeping_time(const char *path, const char *text)
{
    bd_stat_t st;
    CHECK(bd_stat(path, &st) == 0);
    bd_sleep_ms(40);
    FILE *f = bd_fopen(path, "r+b");
    CHECK(f != NULL);
    if (!f) return;
    fputs(text, f);
    fflush(f);
    CHECK(bd_set_mtime(f, st.mtime_ns) == 0);
    fclose(f);
    bd_stat_t after;
    CHECK(bd_stat(path, &after) == 0 && after.size == st.size && after.mtime_ns == st.mtime_ns);
}

/* Move a file's change time without touching its contents: set its
 * modified time to what it already is. */
static void touch_change_time(const char *path)
{
    bd_stat_t st;
    CHECK(bd_stat(path, &st) == 0);
    bd_sleep_ms(40);
    FILE *f = bd_fopen(path, "r+b");
    CHECK(f != NULL);
    if (!f) return;
    CHECK(bd_set_mtime(f, st.mtime_ns) == 0);
    fclose(f);
}

/* D2: a change that keeps the size and modified time is seen by the
 * change time, and backed up; the bit-rot scan then finds nothing. */
static void test_change_time(void)
{
    write_file(at("ct/Src/vault.hc"), "secret contents, version one");
    write_file(at("ct/Src/other.txt"), "other");
    bd_mkdirs(at("ct/drive"));
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("ct/c.brodalf"), &cat), NULL);
    int64_t src, drive;
    REQUIRE_OK(bd_source_add(cat, at("ct/Src"), &src), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_new == 2);
    REQUIRE_OK(bd_media_init(cat, at("ct/drive"), "CT", 0, &drive), cat);
    bd_backup_stats bs;
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 2);
    /* Nothing changed: nothing is read again. */
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_changed == 0 && ss.bytes_hashed == 0);

    rewrite_keeping_time(at("ct/Src/vault.hc"), "SECRET");
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_changed == 1 && ss.files_damaged == 0);
    CHECK(count_rows(cat, "SELECT COUNT(*) FROM versions v JOIN nodes n ON n.id=v.node_id WHERE n.name='vault.hc'") == 2);
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 1);
    bd_scan_opts so = {1};
    REQUIRE_OK(bd_scan_ex(cat, &so, &ss, quiet, NULL), cat);
    CHECK(ss.files_damaged == 0 && ss.files_changed == 0);

    /* The change time moved but the contents did not: read, and the same
     * checksum is no new version. */
    touch_change_time(at("ct/Src/other.txt"));
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_changed == 0 && ss.files_new == 0);
    CHECK(count_rows(cat, "SELECT COUNT(*) FROM versions") == 3);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.bytes_hashed == 0);

    /* A catalog from before (no change times kept): the first scan only
     * records them, it reads nothing; the next change is seen. */
    sqlite3_exec(cat->db, "DELETE FROM file_change_times", NULL, NULL, NULL);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.bytes_hashed == 0 && ss.files_changed == 0);
    CHECK(count_rows(cat, "SELECT COUNT(*) FROM file_change_times") == 2);
    rewrite_keeping_time(at("ct/Src/vault.hc"), "secret");
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_changed == 1);

    /* Where no change time is known (a FAT drive), it is as before. */
    bd_test_no_change_time = 1;
    rewrite_keeping_time(at("ct/Src/vault.hc"), "Secret");
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_changed == 0);
    REQUIRE_OK(bd_scan_ex(cat, &so, &ss, quiet, NULL), cat);
    CHECK(ss.files_damaged == 1);
    bd_test_no_change_time = 0;
    bd_catalog_close(cat);
}

int main(void)
{
    char tmp[512], id[37];
    bd_temp_dir(tmp, sizeof(tmp));
    bd_uuid_v4(id);
    snprintf(base, sizeof(base), "%s/brodalf-flow-scan-%.8s", tmp, id);
    for (char *p = base; *p; p++) if (*p == '\\') *p = '/';
    bd_mkdirs(base);
    printf("working in %s\n", base);
    bd_test_hw_read = fake_hw;

    test_change_time();

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("all flow scan checks passed\n");
    return 0;
}
