/* The command line and backup findings of the 0.14.0 flow review: each
 * test fails on 0.14.0. */
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

/* A file of n bytes. */
static void write_size(const char *path, int n, char fill)
{
    char *text = malloc((size_t)n + 1);
    if (!text) exit(1);
    memset(text, fill, (size_t)n);
    text[n] = '\0';
    write_file(path, text);
    free(text);
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

static int64_t count_rows(bd_catalog *cat, const char *sql)
{
    sqlite3_stmt *q;
    int64_t n = -1;
    if (sqlite3_prepare_v2(cat->db, sql, -1, &q, NULL) == SQLITE_OK) {
        if (sqlite3_step(q) == SQLITE_ROW) n = sqlite3_column_int64(q, 0);
        sqlite3_finalize(q);
    }
    return n;
}

/* Every drive counts as a disk of its own. */
static int no_disk_known(const char *root, bd_drive_hw *hw)
{
    (void)root;
    memset(hw, 0, sizeof(*hw));
    return -1;
}

/* The roots left out, as bd_list_roots_left_out gives them. */
typedef struct { int n; int64_t id[8]; bd_root_out_why why[8]; int64_t gone[8]; } left_list;

static int collect_left(void *ctx, const bd_root_left_out *r)
{
    left_list *l = ctx;
    if (l->n < 8) {
        l->id[l->n] = r->source_id;
        l->why[l->n] = r->why;
        l->gone[l->n] = r->files_gone;
        l->n++;
    }
    return 0;
}

static left_list left_out(bd_catalog *cat)
{
    left_list l;
    memset(&l, 0, sizeof(l));
    int64_t count = -1;
    CHECK(bd_list_roots_left_out(cat, 0, collect_left, &l, &count) == BD_OK);
    CHECK(count == l.n);
    return l;
}

static int count_pending(void *ctx, const bd_pending_info *p)
{
    (void)p;
    (*(int64_t *)ctx)++;
    return 0;
}

/* D7: a root the scan held back, or whose folder cannot be found, is left
 * out of the backup, its preview and the Next backup list, with one line
 * saying why; the folder at its path is not read, so none of its files is
 * recorded as a new version of the root's. The hold outlasts the session. */
static void test_left_out_roots(void)
{
    make_files("lo/Documents", 20, "mine");
    make_files("lo/Pictures", 3, "pics");
    bd_mkdirs(at("lo/drive"));
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("lo/db.brodalf"), &cat), NULL);
    int64_t docs, pics, drive;
    REQUIRE_OK(bd_source_add(cat, at("lo/Documents"), &docs), cat);
    REQUIRE_OK(bd_source_add(cat, at("lo/Pictures"), &pics), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_media_init(cat, at("lo/drive"), "Drive", 0, &drive), cat);
    CHECK(left_out(cat).n == 0);

    /* Another folder at Documents' path, with one of its names. */
    char *mine = bd_strdup(at("lo/Documents-mine"));
    CHECK(bd_rename_noreplace(at("lo/Documents"), mine) == 0);
    make_files("lo/Documents/other", 25, "theirs");
    write_file(at("lo/Documents/f000.txt"), "someone else's file, longer than mine");
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.roots_emptied == 1);
    char versions_sql[200];
    snprintf(versions_sql, sizeof(versions_sql),
             "SELECT COUNT(*) FROM versions v JOIN nodes n ON n.id=v.node_id WHERE n.source_id=%lld AND n.rel_path='f000.txt'",
             (long long)docs);

    left_list l = left_out(cat);
    CHECK(l.n == 1 && l.id[0] == docs && l.why[0] == BD_ROOT_HELD && l.gone[0] == 19);
    bd_backup_preview_info pv;
    REQUIRE_OK(bd_backup_preview(cat, drive, 0, &pv), cat);
    CHECK(pv.files_to_copy == 3 && pv.roots_left_out == 1); /* 0.14.0: 23 */
    int64_t total = 0, listed = 0;
    REQUIRE_OK(bd_list_pending(cat, drive, 0, count_pending, &listed, &total), cat);
    CHECK(total == 3 && listed == 3); /* 0.14.0: 23 */

    bd_backup_stats bs;
    log_count lc = {"is left out of this backup", 0};
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, count_log, &lc), cat);
    CHECK(bs.files_copied == 3 && bs.roots_left_out == 1 && bs.files_rehashed == 0);
    CHECK(lc.n == 1);
    /* 0.14.0: the other f000.txt became version 2 of Documents' f000.txt. */
    CHECK(count_rows(cat, versions_sql) == 1);

    /* A new session: the hold is remembered. */
    REQUIRE_OK(bd_catalog_save(cat), cat);
    bd_catalog_close(cat);
    REQUIRE_OK(bd_catalog_open(at("lo/db.brodalf"), &cat), NULL);
    int64_t again;
    REQUIRE_OK(bd_media_connect(cat, at("lo/drive"), &again, quiet, NULL), cat);
    l = left_out(cat);
    CHECK(l.n == 1 && l.id[0] == docs && l.why[0] == BD_ROOT_HELD && l.gone[0] == 19);
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 0 && bs.roots_left_out == 1 && bs.files_rehashed == 0);
    CHECK(count_rows(cat, versions_sql) == 1);

    /* Its own files back, and scanned: backed up as usual. */
    CHECK(bd_rename_noreplace(at("lo/Documents"), at("lo/Documents-theirs")) == 0);
    CHECK(bd_rename_noreplace(mine, at("lo/Documents")) == 0);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(left_out(cat).n == 0);
    CHECK(count_rows(cat, "SELECT COUNT(*) FROM settings WHERE key LIKE 'held_root.%'") == 0);
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 20 && bs.roots_left_out == 0);

    /* A folder that is gone now, scanned or not, is left out: the backup
     * does not try each of its files and count them failed. */
    write_file(at("lo/Pictures/new.txt"), "a new picture");
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    char *pics_away = bd_strdup(at("lo/Pictures-away"));
    CHECK(bd_rename_noreplace(at("lo/Pictures"), pics_away) == 0);
    l = left_out(cat);
    CHECK(l.n == 1 && l.id[0] == pics && l.why[0] == BD_ROOT_NOT_FOUND);
    REQUIRE_OK(bd_backup_preview(cat, drive, 0, &pv), cat);
    CHECK(pv.files_to_copy == 0 && pv.roots_left_out == 1);
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 0 && bs.files_failed == 0 && bs.roots_left_out == 1); /* 0.14.0: 1 failed */
    REQUIRE_OK(bd_source_wait_for_disk(cat, pics, 1), cat);
    l = left_out(cat);
    CHECK(l.n == 1 && l.why[0] == BD_ROOT_WAITING);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.roots_waiting == 1);
    /* Back, but not scanned since it was missing: scan first. */
    CHECK(bd_rename_noreplace(pics_away, at("lo/Pictures")) == 0);
    l = left_out(cat);
    CHECK(l.n == 1 && l.why[0] == BD_ROOT_NOT_SCANNED);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(left_out(cat).n == 0);
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 1 && bs.roots_left_out == 0);
    free(mine);
    free(pics_away);
    bd_catalog_close(cat);
}

/* D12: on a drive too small for everything, starred files go first, then
 * files no drive has yet, then smaller before bigger. */
static void test_copy_order(void)
{
    bd_catalog *cat;
    bd_mkdirs(at("ord"));
    REQUIRE_OK(bd_catalog_create(at("ord/db.brodalf"), &cat), NULL);
    int64_t src, d, e, f;
    /* Found first (lower ids): a big file and a middle-sized one. */
    write_size(at("ord/src/m/big.bin"), 2000, 'b');
    write_size(at("ord/src/m/middle.bin"), 90, 'm');
    REQUIRE_OK(bd_source_add(cat, at("ord/src"), &src), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    /* Found later: the starred file and small ones. */
    write_size(at("ord/src/s/star.bin"), 3000, 's');
    for (int i = 0; i < 5; i++) {
        char rel[64];
        snprintf(rel, sizeof(rel), "ord/src/s/small%d.txt", i);
        write_size(at(rel), 10, (char)('0' + i));
    }
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_star_set(cat, src, "s/star.bin", 1), cat);
    bd_mkdirs(at("ord/D"));
    bd_mkdirs(at("ord/E"));
    bd_mkdirs(at("ord/F"));
    REQUIRE_OK(bd_media_init(cat, at("ord/D"), "Drive D", 0, &d), cat);
    REQUIRE_OK(bd_media_init(cat, at("ord/E"), "Drive E", 0, &e), cat);
    REQUIRE_OK(bd_media_init(cat, at("ord/F"), "Drive F", 0, &f), cat);
    char path[1200];

    /* A small drive: the small files before the middle one. */
    bd_backup_stats bs;
    bd_test_free_bytes = 100;
    REQUIRE_OK(bd_backup(cat, e, 0, &bs, quiet, NULL), cat);
    bd_test_free_bytes = -1;
    CHECK(bs.files_copied == 5 && bs.files_no_room == 3); /* 0.14.0: 2, middle.bin took most of the room */
    snprintf(path, sizeof(path), "ord/E/BRODALF/%s/src/m/middle.bin", bd_catalog_uuid(cat));
    CHECK(!exists(at(path)));

    /* Room for the starred file and the small ones, not the others. */
    bd_test_free_bytes = 3050;
    REQUIRE_OK(bd_backup(cat, d, 0, &bs, quiet, NULL), cat);
    bd_test_free_bytes = -1;
    snprintf(path, sizeof(path), "ord/D/BRODALF/%s/src/s/star.bin", bd_catalog_uuid(cat));
    CHECK(exists(at(path))); /* 0.14.0: big.bin went first and the starred file did not fit */
    CHECK(bs.files_copied == 6 && bs.files_no_room == 2);

    /* What no drive has yet goes before files on other drives, even
     * smaller ones. */
    bd_test_free_bytes = 100;
    REQUIRE_OK(bd_backup(cat, f, 0, &bs, quiet, NULL), cat);
    bd_test_free_bytes = -1;
    snprintf(path, sizeof(path), "ord/F/BRODALF/%s/src/m/middle.bin", bd_catalog_uuid(cat));
    CHECK(exists(at(path)) && bs.files_copied == 2);
    bd_catalog_close(cat);
}

/* C1: without the passphrase, files in folders set to be encrypted wait
 * for it and everything else is backed up. */
static void test_passphrase_waits(void)
{
    write_file(at("pw/src/secret/a.txt"), "the secret text");
    write_file(at("pw/src/secret/b.txt"), "more secret text");
    write_file(at("pw/src/open/c.txt"), "open text");
    bd_mkdirs(at("pw/drive"));
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("pw/db.brodalf"), &cat), NULL);
    int64_t src, drive;
    REQUIRE_OK(bd_source_add(cat, at("pw/src"), &src), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_media_init(cat, at("pw/drive"), "Plain", 0, &drive), cat);
    REQUIRE_OK(bd_catalog_set_passphrase(cat, "a passphrase for the test"), cat);
    REQUIRE_OK(bd_encrypt_path_set(cat, src, "secret", 1), cat);
    bd_catalog_lock_key(cat);

    bd_backup_preview_info pv;
    REQUIRE_OK(bd_backup_preview(cat, drive, 0, &pv), cat);
    CHECK(pv.files_to_copy == 3 && pv.files_need_passphrase == 2);
    bd_backup_stats bs;
    log_count lc = {"wait for the passphrase", 0};
    /* 0.14.0: BD_ERR_PASSPHRASE, nothing backed up. */
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, count_log, &lc), cat);
    CHECK(bs.files_copied == 1 && bs.files_need_passphrase == 2 && bs.files_failed == 0);
    CHECK(lc.n == 1);
    char path[1200];
    snprintf(path, sizeof(path), "pw/drive/BRODALF/%s/src/secret/a.txt", bd_catalog_uuid(cat));
    CHECK(!exists(at(path)));
    char sealed[1300];
    snprintf(sealed, sizeof(sealed), "%s" BD_SEALED_SUFFIX, path);
    CHECK(!exists(at(sealed)));

    REQUIRE_OK(bd_catalog_unlock(cat, "a passphrase for the test"), cat);
    REQUIRE_OK(bd_backup_preview(cat, drive, 0, &pv), cat);
    CHECK(pv.files_to_copy == 2 && pv.files_need_passphrase == 0);
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 2 && bs.files_need_passphrase == 0);
    CHECK(exists(at(sealed)));
    bd_catalog_close(cat);
}

int main(void)
{
    bd_test_hw_read = no_disk_known;
    char tmp[512], id[37];
    bd_temp_dir(tmp, sizeof(tmp));
    bd_uuid_v4(id);
    snprintf(base, sizeof(base), "%s/brodalf-flow-cli-%.8s", tmp, id);
    for (char *p = base; *p; p++) if (*p == '\\') *p = '/';
    bd_mkdirs(base);
    printf("working in %s\n", base);

    test_left_out_roots();
    test_copy_order();
    test_passphrase_waits();

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("all flow-cli checks passed\n");
    return 0;
}
