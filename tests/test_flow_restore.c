/* The restore and search findings of the 0.14.0 flow review: each test
 * fails on 0.14.0 (see the notes at each one). */
#include "brodalf.h"
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

static int file_equals(const char *path, const char *text)
{
    FILE *f = bd_fopen(path, "rb");
    if (!f) return 0;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return strcmp(buf, text) == 0;
}

static void quiet(void *ctx, const char *msg)
{
    (void)ctx;
    (void)msg;
}

/* Everything the engine logged, to look for a line in. */
typedef struct { char text[16384]; size_t n; } log_keep;

static void keep_log(void *ctx, const char *msg)
{
    log_keep *k = ctx;
    int w = snprintf(k->text + k->n, sizeof(k->text) - k->n, "%s\n", msg);
    if (w > 0) k->n = k->n + (size_t)w < sizeof(k->text) ? k->n + (size_t)w : sizeof(k->text) - 1;
}

static void target_one(bd_catalog *cat)
{
    bd_target t;
    memset(&t, 0, sizeof(t));
    t.copies = 1;
    t.places = 1;
    REQUIRE_OK(bd_target_set(cat, &t), cat);
}

/* The same number of bytes, other contents, and the modified time it had. */
static void spoil_keep_time(const char *path, const char *text)
{
    bd_stat_t st;
    CHECK(bd_stat(path, &st) == 0);
    FILE *f = bd_fopen(path, "r+b");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    fputs(text, f);
    fflush(f);
    bd_set_times(f, st.mtime_ns, 0);
    fclose(f);
    bd_stat_t now;
    CHECK(bd_stat(path, &now) == 0 && now.size == st.size && now.mtime_ns == st.mtime_ns);
}

static int count_conflict(void *ctx, const bd_conflict_info *c)
{
    (void)c;
    (*(int *)ctx)++;
    return 0;
}

/* The version with a good copy. */
static int backed_up_version(void *ctx, const bd_copy_info *c)
{
    if (c->media_id <= 0 || !c->copy_state || strcmp(c->copy_state, "ok") != 0) return 0;
    *(int64_t *)ctx = c->version_id;
    return 1;
}

static void today(char *out, size_t cap)
{
    time_t now = time(NULL);
    struct tm tm;
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    strftime(out, cap, "%Y-%m-%d", &tm);
}

/* ---- D6: a deleted file restores, and "already there" is only what was compared */

static void test_deleted_and_compared(void)
{
    write_file(at("d6/Src/a.txt"), "alpha");
    write_file(at("d6/Src/gone/b.txt"), "bravo");
    write_file(at("d6/Src/gone/c.txt"), "charlie");
    bd_mkdirs(at("d6/drive"));
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("d6/db.brodalf"), &cat), NULL);
    target_one(cat);
    int64_t src, drive;
    REQUIRE_OK(bd_source_add(cat, at("d6/Src"), &src), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_media_init(cat, at("d6/drive"), "Red", 0, &drive), cat);
    bd_backup_stats bs;
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 3);
    CHECK(bd_remove(at("d6/Src/gone/b.txt")) == 0 && bd_remove(at("d6/Src/gone/c.txt")) == 0);
    bd_rmdir_empty(at("d6/Src/gone"));
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);

    /* The deleted folder: a plain restore has nothing in it; with its
     * deleted files, both. */
    bd_restore_counts rc;
    REQUIRE_OK(bd_restore_plan_counts(cat, src, "gone", at("d6/out"), NULL, NULL, NULL, &rc), cat);
    CHECK(rc.files == 0 && rc.files_checked == 0); /* nothing compared: not "already restored" */
    bd_restore_opts ro;
    memset(&ro, 0, sizeof(ro));
    ro.with_deleted = 1;
    REQUIRE_OK(bd_restore_plan_counts(cat, src, "", at("d6/out"), &ro, NULL, NULL, &rc), cat);
    CHECK(rc.files == 3 && rc.files_no_copy == 0);
    bd_restore_stats rs;
    REQUIRE_OK(bd_restore_ex(cat, src, "", at("d6/out"), &ro, &rs, quiet, NULL), cat);
    CHECK(rs.files_restored == 3);
    CHECK(file_equals(at("d6/out/a.txt"), "alpha") && file_equals(at("d6/out/gone/b.txt"), "bravo"));

    /* Written and read back: known to be there, while it stays the same. */
    REQUIRE_OK(bd_restore_plan_counts(cat, src, "", at("d6/out"), &ro, NULL, NULL, &rc), cat);
    CHECK(rc.files == 0 && rc.files_checked == 3);

    /* Another file of the same size, at the same time, where one goes in a
     * new folder: compared, not taken as there (0.14.0: the plan counted it
     * as already there by its size, and the restore never ran). */
    write_file(at("d6/out2/a.txt"), "ALPHA");
    int64_t total = -1, none = -1;
    REQUIRE_OK(bd_restore_plan(cat, src, "", at("d6/out2"), NULL, NULL, &total, &none), cat);
    CHECK(total == 1); /* 0.14.0: 0 */
    REQUIRE_OK(bd_restore_plan_counts(cat, src, "", at("d6/out2"), NULL, NULL, NULL, &rc), cat);
    CHECK(rc.files == 1 && rc.files_to_compare == 1 && rc.files_checked == 0);
    REQUIRE_OK(bd_restore(cat, src, "", at("d6/out2"), &rs, quiet, NULL), cat);
    CHECK(rs.files_conflict == 1 && rs.files_restored == 0 && file_equals(at("d6/out2/a.txt"), "ALPHA"));
    int conflicts = 0;
    REQUIRE_OK(bd_list_restore_conflicts(cat, count_conflict, &conflicts), cat);
    CHECK(conflicts == 1);
    /* A file of another size there needs no drive: it is left be. */
    write_file(at("d6/out3/a.txt"), "a much longer file");
    REQUIRE_OK(bd_restore_plan_counts(cat, src, "", at("d6/out3"), NULL, NULL, NULL, &rc), cat);
    CHECK(rc.files == 1 && rc.files_in_the_way == 1);
    bd_catalog_close(cat);
}

/* ---- R8: a copy here is damaged, a good one is on a drive not plugged in */

static void test_waits_for_other_drive(void)
{
    write_file(at("r8/Src/a.txt"), "the a file");
    write_file(at("r8/Src/b.txt"), "the b file");
    bd_mkdirs(at("r8/red"));
    bd_mkdirs(at("r8/blue"));
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("r8/db.brodalf"), &cat), NULL);
    int64_t src, red, blue;
    REQUIRE_OK(bd_source_add(cat, at("r8/Src"), &src), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_media_init(cat, at("r8/red"), "Red", 0, &red), cat);
    REQUIRE_OK(bd_media_init(cat, at("r8/blue"), "Blue", 0, &blue), cat);
    bd_backup_stats bs;
    REQUIRE_OK(bd_backup(cat, red, 0, &bs, quiet, NULL), cat);
    REQUIRE_OK(bd_backup(cat, blue, 0, &bs, quiet, NULL), cat);
    bd_media_disconnect(cat, blue);
    char path[1300];
    snprintf(path, sizeof(path), "r8/red/BRODALF/%s/Src/a.txt", bd_catalog_uuid(cat));
    write_file(at(path), "THE A FILE");

    log_keep lk;
    memset(&lk, 0, sizeof(lk));
    bd_restore_stats rs;
    REQUIRE_OK(bd_restore(cat, src, "", at("r8/out"), &rs, keep_log, &lk), cat);
    CHECK(rs.files_restored == 1 && rs.files_offline == 1 && rs.files_failed == 0); /* 0.14.0: failed 1, offline 0 */
    CHECK(strstr(lk.text, "plug in \"Blue\" to restore it") != NULL);
    CHECK(!exists(at("r8/out/a.txt")));
    /* The plan now asks for Blue. */
    int64_t total = 0, none = 0;
    REQUIRE_OK(bd_restore_plan(cat, src, "", at("r8/out"), NULL, NULL, &total, &none), cat);
    CHECK(total == 1 && none == 0);

    int64_t again = 0;
    REQUIRE_OK(bd_media_connect(cat, at("r8/blue"), &again, quiet, NULL), cat);
    CHECK(again == blue);
    REQUIRE_OK(bd_restore(cat, src, "", at("r8/out"), &rs, quiet, NULL), cat);
    CHECK(rs.files_restored == 1 && rs.files_already_there == 1 && rs.files_offline == 0);
    CHECK(file_equals(at("r8/out/a.txt"), "the a file"));
    bd_catalog_close(cat);
}

/* ---- R9: the plain copies first; the rest waits for the passphrase ------ */

static void test_passphrase_for_the_rest(void)
{
    write_file(at("r9/Src/a.txt"), "plain and sealed");
    write_file(at("r9/Src/b.txt"), "only plain");
    bd_mkdirs(at("r9/plain"));
    bd_mkdirs(at("r9/sealed"));
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("r9/db.brodalf"), &cat), NULL);
    REQUIRE_OK(bd_catalog_set_passphrase(cat, "a passphrase for the test"), cat);
    int64_t src, plain, sealed;
    REQUIRE_OK(bd_source_add(cat, at("r9/Src"), &src), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_media_init(cat, at("r9/plain"), "Plain", 0, &plain), cat);
    REQUIRE_OK(bd_media_init(cat, at("r9/sealed"), "Sealed", BD_MEDIA_ENCRYPTED, &sealed), cat);
    bd_backup_stats bs;
    REQUIRE_OK(bd_backup(cat, plain, 0, &bs, quiet, NULL), cat);
    REQUIRE_OK(bd_backup(cat, sealed, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 2);
    char path[1300];
    snprintf(path, sizeof(path), "r9/plain/BRODALF/%s/Src/a.txt", bd_catalog_uuid(cat));
    write_file(at(path), "PLAIN AND SEALED");

    /* Locked: b comes back; a's plain copy is damaged and its good copy is
     * encrypted, so it waits for the passphrase. */
    bd_catalog_lock_key(cat);
    bd_restore_stats rs;
    REQUIRE_OK(bd_restore(cat, src, "", at("r9/out"), &rs, quiet, NULL), cat);
    CHECK(rs.files_restored == 1 && rs.files_need_passphrase == 1 && rs.files_failed == 0); /* 0.14.0: failed 1 */
    CHECK(file_equals(at("r9/out/b.txt"), "only plain") && !exists(at("r9/out/a.txt")));
    REQUIRE_OK(bd_catalog_unlock(cat, "a passphrase for the test"), cat);
    REQUIRE_OK(bd_restore(cat, src, "", at("r9/out"), &rs, quiet, NULL), cat);
    CHECK(rs.files_restored == 1 && rs.files_already_there == 1 && rs.files_need_passphrase == 0);
    CHECK(file_equals(at("r9/out/a.txt"), "plain and sealed"));
    bd_catalog_close(cat);
}

/* ---- R10: where it goes cannot take it --------------------------------- */

static void test_destination_cannot_write(void)
{
    write_file(at("r10/Src/a.txt"), "0123456789");
    write_file(at("r10/Src/b.txt"), "abcdefghij");
    write_file(at("r10/Src/c.txt"), "a file of twenty b.");
    bd_mkdirs(at("r10/red"));
    bd_mkdirs(at("r10/blue"));
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("r10/db.brodalf"), &cat), NULL);
    int64_t src, red, blue;
    REQUIRE_OK(bd_source_add(cat, at("r10/Src"), &src), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_media_init(cat, at("r10/red"), "Red", 0, &red), cat);
    REQUIRE_OK(bd_media_init(cat, at("r10/blue"), "Blue", 0, &blue), cat);
    bd_backup_stats bs;
    REQUIRE_OK(bd_backup(cat, red, 0, &bs, quiet, NULL), cat);
    REQUIRE_OK(bd_backup(cat, blue, 0, &bs, quiet, NULL), cat);

    /* Full after the first file: it stops, says so, and blames no copy
     * (0.14.0: every drive was tried for every file, and it went on). */
    log_keep lk;
    memset(&lk, 0, sizeof(lk));
    bd_restore_stats rs;
    bd_test_dest_room = 15;
    bd_status st = bd_restore(cat, src, "", at("r10/out"), &rs, keep_log, &lk);
    bd_test_dest_room = -1;
    CHECK(st == BD_ERR_IO && strstr(bd_catalog_error(cat), "is full") != NULL);
    CHECK(strstr(bd_catalog_error(cat), at("r10/out")) != NULL);
    CHECK(rs.files_restored == 1 && rs.files_failed == 1 && rs.files_cannot_write == 1);
    CHECK(strstr(lk.text, "the disk is full") != NULL && strstr(lk.text, "trying the next copy") == NULL);
    CHECK(strstr(lk.text, "no copy of") == NULL && strstr(lk.text, "from the copy on") == NULL);
    /* Room made: the same restore again brings the rest. */
    REQUIRE_OK(bd_restore(cat, src, "", at("r10/out"), &rs, quiet, NULL), cat);
    CHECK(rs.files_restored == 2 && rs.files_already_there == 1);

    /* Read-only: it stops at the first file, naming the reason. */
    bd_test_dest_read_only = 1;
    st = bd_restore(cat, src, "", at("r10/ro"), &rs, quiet, NULL);
    bd_test_dest_read_only = 0;
    CHECK(st == BD_ERR_IO && strstr(bd_catalog_error(cat), "read-only") != NULL);
    CHECK(rs.files_restored == 0 && rs.files_failed == 1 && rs.files_cannot_write == 1);

    /* FAT32 where it goes: a file too big for it is left out, the rest
     * comes back (0.14.0: no check, so a half-written file per drive). */
    bd_test_dest_fs_name = "FAT32";
    bd_test_fat_limit = 12;
    memset(&lk, 0, sizeof(lk));
    REQUIRE_OK(bd_restore(cat, src, "", at("r10/fat"), &rs, keep_log, &lk), cat);
    bd_test_dest_fs_name = NULL;
    bd_test_fat_limit = 0;
    CHECK(rs.files_restored == 2 && rs.files_failed == 1 && rs.files_cannot_write == 1);
    CHECK(strstr(lk.text, "too big") != NULL && strstr(lk.text, "FAT32") != NULL);
    CHECK(!exists(at("r10/fat/c.txt")) && file_equals(at("r10/fat/a.txt"), "0123456789"));
    bd_catalog_close(cat);
}

/* ---- C11: keep both ----------------------------------------------------- */

static void test_keep_both(void)
{
    write_file(at("c11/Src/a.txt"), "the backed-up a");
    write_file(at("c11/Src/README"), "read me");
    bd_mkdirs(at("c11/drive"));
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("c11/db.brodalf"), &cat), NULL);
    int64_t src, drive;
    REQUIRE_OK(bd_source_add(cat, at("c11/Src"), &src), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_media_init(cat, at("c11/drive"), "Red", 0, &drive), cat);
    bd_backup_stats bs;
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);

    write_file(at("c11/out/a.txt"), "someone else's a");
    write_file(at("c11/out/README"), "another read me");
    char day[16], beside[256], beside2[256], noext[256];
    today(day, sizeof(day));
    snprintf(beside, sizeof(beside), "c11/out/a (restored %s).txt", day);
    snprintf(beside2, sizeof(beside2), "c11/out/a (restored %s 2).txt", day);
    snprintf(noext, sizeof(noext), "c11/out/README (restored %s)", day);
    bd_restore_opts ro;
    memset(&ro, 0, sizeof(ro));
    ro.keep_both = 1;
    bd_restore_stats rs;
    REQUIRE_OK(bd_restore_ex(cat, src, "", at("c11/out"), &ro, &rs, quiet, NULL), cat);
    CHECK(rs.files_restored == 2 && rs.files_beside == 2 && rs.files_conflict == 0);
    CHECK(file_equals(at("c11/out/a.txt"), "someone else's a"));
    CHECK(file_equals(at(beside), "the backed-up a") && file_equals(at(noext), "read me"));
    /* Again: the one beside is the file, nothing new is written. */
    REQUIRE_OK(bd_restore_ex(cat, src, "", at("c11/out"), &ro, &rs, quiet, NULL), cat);
    CHECK(rs.files_restored == 0 && rs.files_already_there == 2 && !exists(at(beside2)));

    /* One older version into its own folder (0.14.0: always a conflict). */
    write_file(at("c11/Src/a.txt"), "the newer a, changed");
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    int64_t node = 0, vid = 0;
    REQUIRE_OK(bd_find_node(cat, src, "a.txt", &node), cat);
    bd_list_copies(cat, node, backed_up_version, &vid);
    REQUIRE_OK(bd_restore_version(cat, vid, at("c11/Src"), &rs, quiet, NULL), cat);
    CHECK(rs.files_conflict == 1 && rs.files_restored == 0);
    REQUIRE_OK(bd_restore_version_ex(cat, vid, at("c11/Src"), &ro, &rs, quiet, NULL), cat);
    snprintf(beside, sizeof(beside), "c11/Src/a (restored %s).txt", day);
    CHECK(rs.files_restored == 1 && rs.files_beside == 1);
    CHECK(file_equals(at(beside), "the backed-up a") && file_equals(at("c11/Src/a.txt"), "the newer a, changed"));
    bd_catalog_close(cat);
}

/* ---- C13: between passes, and kept at close ---------------------------- */

static void test_between_passes(void)
{
    write_file(at("c13/Src/a.txt"), "first file");
    write_file(at("c13/Src/b.txt"), "second file");
    bd_mkdirs(at("c13/drive"));
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("c13/db.brodalf"), &cat), NULL);
    int64_t src, drive;
    REQUIRE_OK(bd_source_add(cat, at("c13/Src"), &src), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_media_init(cat, at("c13/drive"), "Red", 0, &drive), cat);
    bd_backup_stats bs;
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);

    /* The first pass compares b by its checksum: another file of its size
     * and time is not taken for it. */
    write_file(at("c13/out/b.txt"), "second file");
    bd_restore_stats rs;
    REQUIRE_OK(bd_restore(cat, src, "", at("c13/out"), &rs, quiet, NULL), cat);
    CHECK(rs.files_restored == 1 && rs.files_already_there == 1);
    /* The next pass takes what it found there by size and modified time,
     * without reading it again: changed bytes, same size and time, pass
     * (0.14.0 read every file again, so it was a conflict). */
    spoil_keep_time(at("c13/out/a.txt"), "FIRST");
    REQUIRE_OK(bd_restore(cat, src, "", at("c13/out"), &rs, quiet, NULL), cat);
    CHECK(rs.files_already_there == 2 && rs.files_conflict == 0);
    bd_restore_opts ro;
    memset(&ro, 0, sizeof(ro));
    REQUIRE_OK(bd_restore_pending_set(cat, src, "", at("c13/out"), &ro, 0), cat);
    REQUIRE_OK(bd_catalog_save(cat), cat);
    bd_catalog_close(cat);

    /* Kept in the database: the restore that waits, and what is there. */
    REQUIRE_OK(bd_catalog_open(at("c13/db.brodalf"), &cat), NULL);
    bd_restore_pending p;
    REQUIRE_OK(bd_restore_pending_get(cat, &p), cat);
    CHECK(p.source_id == src && strcmp(p.rel_prefix, "") == 0 && strcmp(p.dest, at("c13/out")) == 0 && p.saved_ms > 0);
    bd_restore_pending_free(&p);
    bd_restore_counts rc;
    REQUIRE_OK(bd_restore_plan_counts(cat, src, "", at("c13/out"), NULL, NULL, NULL, &rc), cat);
    CHECK(rc.files == 0 && rc.files_checked == 2);
    /* A restore to another place forgets it (the drive is not plugged in
     * since the database was opened: nothing comes back there). */
    REQUIRE_OK(bd_restore(cat, src, "", at("c13/other"), &rs, quiet, NULL), cat);
    CHECK(rs.files_offline == 2);
    REQUIRE_OK(bd_restore_plan_counts(cat, src, "", at("c13/out"), NULL, NULL, NULL, &rc), cat);
    CHECK(rc.files == 2 && rc.files_to_compare == 2 && rc.files_checked == 0);
    REQUIRE_OK(bd_restore(cat, src, "", at("c13/out"), &rs, quiet, NULL), cat);
    CHECK(rs.files_conflict == 1 && rs.files_already_there == 1); /* a is compared again */
    REQUIRE_OK(bd_restore_plan_counts(cat, src, "", at("c13/out"), NULL, NULL, NULL, &rc), cat);
    CHECK(rc.files == 1 && rc.files_checked == 1);
    /* Done with it: forgotten, and what it found with it. */
    REQUIRE_OK(bd_restore_pending_clear(cat), cat);
    CHECK(bd_restore_pending_get(cat, &p) == BD_ERR_NOT_FOUND);
    REQUIRE_OK(bd_restore_plan_counts(cat, src, "", at("c13/out"), NULL, NULL, NULL, &rc), cat);
    CHECK(rc.files_checked == 0 && rc.files_to_compare == 2);
    bd_catalog_close(cat);
}

/* ---- C16: a restore into a backup root or a drive's BRODALF folder ----- */

static void test_destination_check(void)
{
    write_file(at("c16/Docs/a.txt"), "a");
    write_file(at("c16/Pics/p.txt"), "p");
    bd_mkdirs(at("c16/drive"));
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("c16/db.brodalf"), &cat), NULL);
    int64_t docs, pics, drive;
    REQUIRE_OK(bd_source_add(cat, at("c16/Docs"), &docs), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_media_init(cat, at("c16/drive"), "Red", 0, &drive), cat);
    char *what = NULL;
    /* Back where they came from: fine. Anywhere else in a root: not. */
    CHECK(bd_restore_dest_check(cat, docs, at("c16/Docs"), &what) == 0 && !what);
    CHECK(bd_restore_dest_check(cat, 0, at("c16/Docs"), &what) == 0);
    CHECK(bd_restore_dest_check(cat, docs, at("c16/Docs/restored"), &what) == 1);
    CHECK(what && strcmp(what, at("c16/Docs")) == 0);
    free(what);
    REQUIRE_OK(bd_source_add(cat, at("c16/Pics"), &pics), cat);
    CHECK(bd_restore_dest_check(cat, 0, at("c16/Docs"), NULL) == 1); /* each into a folder named after its root */
    CHECK(bd_restore_dest_check(cat, docs, at("c16/Pics"), NULL) == 1);
    /* A drive: its BRODALF folder and what is in it, not the drive itself. */
    char path[1300];
    snprintf(path, sizeof(path), "c16/drive/BRODALF/%s", bd_catalog_uuid(cat));
    CHECK(bd_restore_dest_check(cat, docs, at(path), &what) == 2);
    free(what);
    snprintf(path, sizeof(path), "c16/drive/BRODALF/%s/Docs", bd_catalog_uuid(cat));
    CHECK(bd_restore_dest_check(cat, docs, at(path), NULL) == 2);
    CHECK(bd_restore_dest_check(cat, docs, at("c16/drive/BRODALF"), NULL) == 2);
    CHECK(bd_restore_dest_check(cat, docs, at("c16/drive"), NULL) == 0);
    CHECK(bd_restore_dest_check(cat, docs, at("c16/out"), NULL) == 0);
    bd_catalog_close(cat);
}

int main(void)
{
    char tmp[512], id[37];
    bd_temp_dir(tmp, sizeof(tmp));
    bd_uuid_v4(id);
    snprintf(base, sizeof(base), "%s/brodalf-flow-restore-%.8s", tmp, id);
    for (char *p = base; *p; p++) if (*p == '\\') *p = '/';
    bd_mkdirs(base);
    printf("working in %s\n", base);

    test_deleted_and_compared();
    test_waits_for_other_drive();
    test_passphrase_for_the_rest();
    test_destination_cannot_write();
    test_keep_both();
    test_between_passes();
    test_destination_check();

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("all flow restore checks passed\n");
    return 0;
}
