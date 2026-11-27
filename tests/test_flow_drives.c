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

static int64_t count_rows(bd_catalog *cat, const char *sql, int64_t arg)
{
    sqlite3_stmt *q;
    int64_t n = -1;
    if (sqlite3_prepare_v2(cat->db, sql, -1, &q, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(q, 1, arg);
    if (sqlite3_step(q) == SQLITE_ROW) n = sqlite3_column_int64(q, 0);
    sqlite3_finalize(q);
    return n;
}

/* Takes a drive away (renames its folder), or stops the job, from the
 * progress callback, as if it were unplugged or Stop pressed halfway. */
typedef struct {
    bd_catalog *cat;
    const char *from, *to; /* NULL: stop the job instead */
    int64_t after_files;
    int fired, moved;
} vanish_at;

static void vanish_progress(void *ctx, const bd_progress *p)
{
    vanish_at *v = ctx;
    v->cat->progress_last_ms = 0; /* report every file */
    if (v->fired || p->done || strcmp(p->phase, "backup") != 0 || p->files_done < v->after_files) return;
    v->fired = 1;
    if (v->from) v->moved = bd_rename_noreplace(v->from, v->to) == 0;
    else bd_catalog_cancel(v->cat);
}

/* How many good copies on drive media_id the newest database copy on the
 * drive at root knows of: what recovering from that drive alone gives. */
static int64_t known_by_drive_copy(bd_catalog *cat, const char *root, int64_t media_id, const char *dest)
{
    revs_seen r = revisions_on(cat, root);
    if (!r.n || bd_catalog_recover(r.paths[0], dest) != BD_OK) return -1;
    bd_catalog *rc;
    if (bd_catalog_open(dest, &rc) != BD_OK) return -1;
    int64_t n = count_rows(rc, "SELECT COUNT(*) FROM copies WHERE media_id=? AND state='ok'", media_id);
    bd_catalog_close(rc);
    return n;
}

/* ---- R1: a backup that stops early, or loses one of its drives ------------ */

static void test_backup_stops_early(void)
{
    make_files("r1-src/Docs", 12, "r1");
    bd_mkdirs(at("r1/A"));
    bd_mkdirs(at("r1/B"));
    bd_mkdirs(at("r1/C"));
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("r1/db.brodalf"), &cat), NULL);
    int64_t src, ids[2], c;
    REQUIRE_OK(bd_source_add(cat, at("r1-src/Docs"), &src), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_media_init(cat, at("r1/A"), "A", 0, &ids[0]), cat);
    REQUIRE_OK(bd_media_init(cat, at("r1/B"), "B", 0, &ids[1]), cat);
    REQUIRE_OK(bd_media_init(cat, at("r1/C"), "C", 0, &c), cat);

    /* B goes halfway: A gets everything, and a database copy that knows it. */
    vanish_at va = {cat, at("r1/B"), at("r1/B-away"), 5, 0, 0};
    bd_backup_stats bs[2];
    bd_catalog_set_progress(cat, vanish_progress, &va);
    bd_status st = bd_backup_many(cat, ids, 2, 0, bs, quiet, NULL);
    bd_catalog_set_progress(cat, NULL, NULL);
    CHECK(va.fired && va.moved);
    CHECK(st == BD_ERR_DRIVE_GONE && strstr(bd_catalog_error(cat), "went on to the other drives") != NULL);
    CHECK(bs[0].files_copied == 12); /* 0.14.0: it stopped with B */
    CHECK(bs[1].files_copied < 12 && bs[1].catalog_revision == 0);
    CHECK(bs[0].catalog_revision > 0 && bs[0].catalog_copy_failed == 0); /* 0.14.0: no database copy at all */
    CHECK(count_rows(cat, "SELECT COUNT(*) FROM jobs WHERE media_id=? AND kind='backup' AND status='done'", ids[0]) == 1);
    CHECK(known_by_drive_copy(cat, at("r1/A"), ids[0], at("r1/from-A.brodalf")) == 12);
    CHECK(bd_rename_noreplace(at("r1/B-away"), at("r1/B")) == 0);

    /* Stopped: the drive gets a database copy that knows what was copied. */
    vanish_at stop = {cat, NULL, NULL, 4, 0, 0};
    bd_backup_stats one;
    bd_catalog_set_progress(cat, vanish_progress, &stop);
    st = bd_backup(cat, c, 0, &one, quiet, NULL);
    bd_catalog_set_progress(cat, NULL, NULL);
    CHECK(stop.fired && st == BD_ERR_CANCELLED);
    CHECK(one.files_copied > 0 && one.files_copied < 12);
    CHECK(one.catalog_revision == bd_catalog_revision(cat)); /* 0.14.0: 0, only the copy from setup */
    CHECK(revisions_on(cat, at("r1/C")).revs[0] == bd_catalog_revision(cat));
    CHECK(known_by_drive_copy(cat, at("r1/C"), c, at("r1/from-C.brodalf")) == one.files_copied);
    /* The status says which copy it holds. */
    bd_media_status ms;
    REQUIRE_OK(bd_media_status_get(cat, c, &ms), cat);
    CHECK(ms.catalog_revision == one.catalog_revision && ms.catalog_written_ms > 0 && ms.catalog_failed_ms == 0);
    bd_catalog_close(cat);
}

/* ---- R2: room counted by allocation unit; a database copy that fails ------- */

static int spoil_all = 0;
static void spoil_every(const char *path)
{
    if (!spoil_all) return;
    FILE *f = bd_fopen(path, "r+b");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, n / 2, SEEK_SET);
    for (int i = 0; i < 8; i++) fputc(0x5A ^ i, f);
    fclose(f);
}

static void test_room_and_failed_catalog_copy(void)
{
    make_files("r2-src/Docs", 10, "r2");
    bd_mkdirs(at("r2/A"));
    bd_mkdirs(at("r2/B"));
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("r2/db.brodalf"), &cat), NULL);
    int64_t src, a, b;
    REQUIRE_OK(bd_source_add(cat, at("r2-src/Docs"), &src), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_media_init(cat, at("r2/A"), "A", 0, &a), cat);
    REQUIRE_OK(bd_media_init(cat, at("r2/B"), "B", 0, &b), cat);

    /* Each small file takes a whole 4 KB unit: four fit in 16 KB. */
    bd_test_free_bytes = 4 * 4096;
    bd_test_cluster_bytes = 4096;
    bd_backup_stats bs;
    REQUIRE_OK(bd_backup(cat, a, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 4 && bs.files_no_room == 6); /* 0.14.0: 10 copied, by their 12 bytes each */

    /* C16: a full drive does not read the files first. Every file changed
     * since the scan, which would read each one again. */
    for (int i = 0; i < 10; i++) {
        char rel[64];
        snprintf(rel, sizeof(rel), "r2-src/Docs/f%03d.txt", i);
        write_file(at(rel), "changed since the scan, and longer");
    }
    bd_test_free_bytes = 0;
    REQUIRE_OK(bd_backup(cat, b, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 0 && bs.files_no_room == 10);
    CHECK(bs.files_rehashed == 0); /* 0.14.0: 10, each read before its room was looked at */
    bd_test_free_bytes = -1;
    bd_test_cluster_bytes = 0;

    /* A database copy that fails is a problem for that drive. */
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_backup(cat, b, 0, &bs, quiet, NULL), cat);
    int64_t good = bs.catalog_revision;
    CHECK(good > 0 && !bs.catalog_copy_failed);
    write_file(at("r2-src/Docs/new.txt"), "a new file");
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    spoil_all = 1;
    bd_test_spoil_catalog_copy = spoil_every;
    log_count lc = {"could not be copied", 0};
    REQUIRE_OK(bd_backup(cat, b, 0, &bs, count_log, &lc), cat);
    spoil_all = 0;
    CHECK(bs.files_copied == 1 && bs.catalog_copy_failed == 1 && bs.catalog_revision == 0 && lc.n == 1);
    bd_media_status ms;
    REQUIRE_OK(bd_media_status_get(cat, b, &ms), cat);
    CHECK(ms.catalog_failed_ms > 0 && ms.catalog_failed_revision > good);
    /* The next backup puts one there, and the problem is gone. */
    REQUIRE_OK(bd_backup(cat, b, 0, &bs, quiet, NULL), cat);
    bd_test_spoil_catalog_copy = NULL;
    CHECK(bs.catalog_revision > good && !bs.catalog_copy_failed);
    REQUIRE_OK(bd_media_status_get(cat, b, &ms), cat);
    CHECK(ms.catalog_failed_ms == 0 && ms.catalog_revision == bs.catalog_revision);
    bd_catalog_close(cat);
}

/* ---- R3: a new passphrase or recovery key reaches the drives ------------- */

/* The whole of a small text file, or "" (free it). */
static char *slurp(const char *path)
{
    FILE *f = bd_fopen(path, "rb");
    char *buf = calloc(1, 1 << 20);
    if (f && buf) {
        size_t n = fread(buf, 1, (1 << 20) - 1, f);
        buf[n] = '\0';
    }
    if (f) fclose(f);
    return buf;
}

/* Recover the newest copy on the drive at root and try to open it. */
static bd_status open_newest(bd_catalog *cat, const char *root, const char *dest, const char *secret, int recovery)
{
    char dir[2048], newest[2048] = "";
    snprintf(dir, sizeof(dir), "%s/BRODALF/%s", root, bd_catalog_uuid(cat));
    bd_find_catalog_revisions(dir, newest_cb, newest);
    if (!newest[0] || bd_catalog_recover(newest, dest) != BD_OK) return BD_ERR_NOT_FOUND;
    bd_catalog *c = NULL;
    bd_status s = recovery ? bd_catalog_open_with_recovery(dest, secret, &c) : bd_catalog_open_with(dest, secret, &c);
    if (c) bd_catalog_close(c);
    bd_remove(dest);
    return s;
}

static void test_key_reaches_drives(void)
{
    bd_catalog *cat;
    bd_mkdirs(at("r3/A"));
    bd_mkdirs(at("r3/B"));
    REQUIRE_OK(bd_catalog_create(at("r3/db.brodalf"), &cat), NULL);
    make_files("r3-src/Docs", 3, "r3");
    int64_t src, a, b;
    bd_scan_stats ss;
    bd_backup_stats bs;
    REQUIRE_OK(bd_source_add(cat, at("r3-src/Docs"), &src), cat);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_catalog_set_passphrase(cat, "first passphrase"), cat);
    REQUIRE_OK(bd_media_init(cat, at("r3/A"), "A", BD_MEDIA_ENCRYPTED, &a), cat);
    REQUIRE_OK(bd_media_init(cat, at("r3/B"), "B", BD_MEDIA_ENCRYPTED, &b), cat);
    REQUIRE_OK(bd_backup(cat, a, 0, &bs, quiet, NULL), cat);
    REQUIRE_OK(bd_backup(cat, b, 0, &bs, quiet, NULL), cat);
    CHECK(bd_media_copy_has_key(cat, a, 0) == 1);

    /* A new passphrase: no drive has it until a copy of the database goes
     * there, and one that got none still opens with the old one. */
    REQUIRE_OK(bd_catalog_set_passphrase(cat, "second passphrase"), cat);
    REQUIRE_OK(bd_catalog_save(cat), cat);
    CHECK(bd_media_copy_has_key(cat, a, 0) == 0);
    REQUIRE_OK(bd_catalog_copy_to_media(cat, a), cat);
    CHECK(bd_media_copy_has_key(cat, a, 0) == 1);
    CHECK(bd_media_copy_has_key(cat, b, 0) == 0);
    CHECK(open_newest(cat, at("r3/A"), at("r3/try.brodalf"), "second passphrase", 0) == BD_OK);
    CHECK(open_newest(cat, at("r3/B"), at("r3/try.brodalf"), "second passphrase", 0) == BD_ERR_PASSPHRASE);
    CHECK(open_newest(cat, at("r3/B"), at("r3/try.brodalf"), "first passphrase", 0) == BD_OK);

    /* A new recovery key, put on B only: the sheet says which drive has it. */
    char key[BD_RECOVERY_KEY_SIZE];
    REQUIRE_OK(bd_catalog_make_recovery_key(cat, key), cat);
    CHECK(bd_media_copy_has_key(cat, b, 1) == 0);
    REQUIRE_OK(bd_catalog_copy_to_media(cat, b), cat);
    CHECK(bd_media_copy_has_key(cat, b, 1) == 1);
    CHECK(bd_media_copy_has_key(cat, a, 1) == 0);
    CHECK(open_newest(cat, at("r3/B"), at("r3/try.brodalf"), key, 1) == BD_OK);
    CHECK(open_newest(cat, at("r3/A"), at("r3/try.brodalf"), key, 1) != BD_OK);
    REQUIRE_OK(bd_write_recovery_sheet(cat, at("r3/sheet.html"), key), cat);
    char *sheet = slurp(at("r3/sheet.html"));
    CHECK(sheet && strstr(sheet, "its database copy has this key"));
    CHECK(sheet && strstr(sheet, "does not have this key yet"));
    free(sheet);
    /* Without a key on it the sheet says nothing about keys. */
    REQUIRE_OK(bd_write_recovery_sheet(cat, at("r3/plain.html"), NULL), cat);
    sheet = slurp(at("r3/plain.html"));
    CHECK(sheet && !strstr(sheet, "this key"));
    free(sheet);

    /* With no drive plugged in, what was put on each is remembered. */
    REQUIRE_OK(bd_catalog_save(cat), cat);
    bd_catalog_close(cat);
    REQUIRE_OK(bd_catalog_open(at("r3/db.brodalf"), &cat), NULL);
    CHECK(bd_media_copy_has_key(cat, b, 1) == 1);
    CHECK(bd_media_copy_has_key(cat, a, 1) == 0);
    CHECK(bd_media_copy_has_key(cat, a, 0) == 1);
    CHECK(bd_media_copy_has_key(cat, b, 0) == 1);
    bd_catalog_close(cat);
}

/* ---- D8: a cloned drive ---------------------------------------------------- */

/* Stands in for reading the disk: a folder's name says which disk it is on. */
static int fake_hw(const char *root, bd_drive_hw *hw)
{
    memset(hw, 0, sizeof(*hw));
    hw->disk_bytes = -1;
    static const struct { const char *mark, *model, *serial; int64_t bytes; } disks[] = {
        {"disk-ORIG", "WD Elements", "WX51A2345", 2000398934016},
        {"disk-CLONE", "WD Elements", "WX77B9876", 2000398934016},
        {"disk-NEW", "Seagate Expansion", "NAB1C2D3", 4000787030016},
        {"disk-SHARED", "USB Bridge", "000000001234", 1000204886016},
        {"disk-BLANK", "Card Reader", "", 64021856256},
        {"disk-BRIDGE2", "USB Bridge", "000000001234", 500107862016},
    };
    for (size_t i = 0; i < sizeof(disks) / sizeof(disks[0]); i++) {
        if (!strstr(root, disks[i].mark)) continue;
        snprintf(hw->model, sizeof(hw->model), "%s", disks[i].model);
        snprintf(hw->serial, sizeof(hw->serial), "%s", disks[i].serial);
        snprintf(hw->health, sizeof(hw->health), "good");
        hw->disk_bytes = disks[i].bytes;
        return 0;
    }
    return -1;
}

typedef struct { int64_t id; int uncounted; int found; } uncounted_find;

static int uncounted_cb(void *ctx, const bd_media_info *m)
{
    uncounted_find *f = ctx;
    if (m->media_id != f->id) return 0;
    f->uncounted = m->uncounted;
    f->found = 1;
    return 1;
}

static int uncounted_of(bd_catalog *cat, int64_t id)
{
    uncounted_find f = {id, -1, 0};
    bd_list_media(cat, uncounted_cb, &f);
    return f.uncounted;
}

static int64_t no_copy(bd_catalog *cat)
{
    bd_risk_stats rs;
    memset(&rs, 0, sizeof(rs));
    if (bd_list_at_risk(cat, 0, NULL, NULL, &rs) != BD_OK) return -1;
    return rs.files_no_copy;
}

/* Copy a whole drive folder, as a clone of the disk would be. */
typedef struct { const char *from, *to; } tree_copy;

static int tree_copy_cb(void *ctx, const char *rel, const bd_stat_t *st)
{
    tree_copy *t = ctx;
    char *src = bd_path_join(t->from, rel), *dst = bd_path_join(t->to, rel);
    if (st->is_dir) bd_mkdirs(dst);
    else {
        char *dir = bd_strdup(dst);
        char *slash = strrchr(dir, '/');
        char *bslash = strrchr(dir, '\\');
        if (bslash && (!slash || bslash > slash)) slash = bslash;
        if (slash) { *slash = '\0'; bd_mkdirs(dir); }
        free(dir);
        copy_file(src, dst);
    }
    free(src);
    free(dst);
    return 0;
}

static void test_cloned_drive(void)
{
    bd_catalog *cat;
    bd_mkdirs(at("d8/disk-ORIG"));
    REQUIRE_OK(bd_catalog_create(at("d8/db.brodalf"), &cat), NULL);
    make_files("d8-src/Docs", 4, "d8");
    int64_t src, a, id;
    bd_scan_stats ss;
    bd_backup_stats bs;
    bd_check_stats cs;
    REQUIRE_OK(bd_source_add(cat, at("d8-src/Docs"), &src), cat);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_media_init(cat, at("d8/disk-ORIG"), "A", 0, &a), cat);
    REQUIRE_OK(bd_backup(cat, a, 0, &bs, quiet, NULL), cat);
    CHECK(no_copy(cat) == 0);
    CHECK(uncounted_of(cat, a) == 0);

    /* The disk is cloned, and both are plugged in at once. */
    tree_copy tc = {at("d8/disk-ORIG"), at("d8/disk-CLONE")};
    bd_mkdirs(tc.to);
    CHECK(bd_walk(tc.from, tree_copy_cb, NULL, &tc) == 0);
    log_count lc = {"at once", 0};
    REQUIRE_OK(bd_media_connect(cat, at("d8/disk-CLONE"), &id, count_log, &lc), cat);
    CHECK(id == a);
    CHECK(lc.n == 1);
    char *root = NULL, *other = NULL;
    CHECK(bd_media_suspect(cat, a, &root, &other) == 1);
    CHECK(root && strstr(root, "disk-CLONE"));
    CHECK(other && strstr(other, "disk-ORIG"));
    free(root);
    free(other);
    CHECK(uncounted_of(cat, a) == 3);
    CHECK(no_copy(cat) == 4);
    /* A check with the other one still plugged in does not settle it. */
    REQUIRE_OK(bd_media_check(cat, a, &cs, quiet, NULL), cat);
    CHECK(bd_media_suspect(cat, a, NULL, NULL) == 1);
    /* The clone alone gets a new file, then the original comes back. */
    char *gone = bd_sprintf("%s-away", at("d8/disk-ORIG"));
    CHECK(rename(at("d8/disk-ORIG"), gone) == 0);
    REQUIRE_OK(bd_media_check(cat, a, &cs, quiet, NULL), cat);
    CHECK(cs.missing == 0 && cs.ok == 4);
    CHECK(bd_media_suspect(cat, a, NULL, NULL) == 0);
    CHECK(uncounted_of(cat, a) == 0);
    CHECK(no_copy(cat) == 0);
    write_file(at("d8-src/Docs/later.txt"), "backed up to the clone only");
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_backup(cat, a, 0, &bs, quiet, NULL), cat);
    CHECK(no_copy(cat) == 0);
    bd_media_disconnect(cat, a);
    CHECK(rename(at("d8/disk-CLONE"), at("d8/clone-away")) == 0);
    CHECK(rename(gone, at("d8/disk-ORIG")) == 0);
    free(gone);
    /* On the original, the serial number is not the one last seen. */
    REQUIRE_OK(bd_media_connect(cat, at("d8/disk-ORIG"), &id, quiet, NULL), cat);
    CHECK(id == a);
    CHECK(bd_media_suspect(cat, a, NULL, NULL) == 2);
    CHECK(no_copy(cat) == 5);
    /* Remembered when the database is opened again. */
    REQUIRE_OK(bd_catalog_save(cat), cat);
    bd_catalog_close(cat);
    REQUIRE_OK(bd_catalog_open(at("d8/db.brodalf"), &cat), NULL);
    REQUIRE_OK(bd_media_connect(cat, at("d8/disk-ORIG"), &id, quiet, NULL), cat);
    CHECK(bd_media_suspect(cat, a, NULL, NULL) == 2);
    CHECK(uncounted_of(cat, a) == 3);
    /* The check finds the file the original never got, and settles it. */
    REQUIRE_OK(bd_media_check(cat, a, &cs, quiet, NULL), cat);
    CHECK(cs.missing == 1);
    CHECK(bd_media_suspect(cat, a, NULL, NULL) == 0);
    CHECK(no_copy(cat) == 1);
    REQUIRE_OK(bd_backup(cat, a, 0, &bs, quiet, NULL), cat);
    CHECK(no_copy(cat) == 0);
    bd_catalog_close(cat);
}

/* ---- D9: a drive marked lost takes older versions and deleted files ------- */

typedef struct { int64_t id[8]; int64_t items[8]; int n; } impact_seen;

static int impact_cb(void *ctx, int64_t media_id, const char *label, int64_t items)
{
    (void)label;
    impact_seen *s = ctx;
    if (s->n < 8) {
        s->id[s->n] = media_id;
        s->items[s->n++] = items;
    }
    return 0;
}

static void test_lost_drive_takes_old_versions(void)
{
    bd_catalog *cat;
    bd_mkdirs(at("d9/A"));
    bd_mkdirs(at("d9/B"));
    REQUIRE_OK(bd_catalog_create(at("d9/db.brodalf"), &cat), NULL);
    make_files("d9-src/Docs", 3, "d9");
    int64_t src, a, b;
    bd_scan_stats ss;
    bd_backup_stats bs;
    REQUIRE_OK(bd_source_add(cat, at("d9-src/Docs"), &src), cat);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_media_init(cat, at("d9/A"), "A", 0, &a), cat);
    REQUIRE_OK(bd_media_init(cat, at("d9/B"), "B", 0, &b), cat);
    bd_target tg;
    bd_target_get(cat, &tg);
    tg.copies = 2;
    REQUIRE_OK(bd_target_set(cat, &tg), cat);
    REQUIRE_OK(bd_backup(cat, a, 0, &bs, quiet, NULL), cat);
    REQUIRE_OK(bd_backup(cat, b, 0, &bs, quiet, NULL), cat);
    /* f000 changes twice, backed up to A only: A holds its two older
     * versions, B the first. f001 is deleted (on A and B); f003 is new,
     * backed up to A only, then deleted. */
    write_file(at("d9-src/Docs/f000.txt"), "d9 second contents");
    set_old(at("d9-src/Docs/f000.txt"), 3);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_backup(cat, a, 0, &bs, quiet, NULL), cat);
    write_file(at("d9-src/Docs/f000.txt"), "d9 third contents, longer");
    write_file(at("d9-src/Docs/f003.txt"), "d9 only on A");
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_backup(cat, a, 0, &bs, quiet, NULL), cat);
    bd_remove(at("d9-src/Docs/f001.txt"));
    bd_remove(at("d9-src/Docs/f003.txt"));
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);

    bd_lost_impact li;
    impact_seen seen;
    memset(&seen, 0, sizeof(seen));
    REQUIRE_OK(bd_media_lost_impact(cat, a, &li, impact_cb, &seen), cat);
    CHECK(li.old_versions == 2 && li.deleted == 2);
    CHECK(li.old_versions_short == 2 && li.deleted_short == 2);
    CHECK(li.old_versions_none == 1 && li.deleted_none == 1);
    CHECK(li.copies_wanted == 2);
    CHECK(seen.n == 1 && seen.id[0] == b && seen.items[0] == 2);
    /* The same once it is marked. */
    REQUIRE_OK(bd_media_mark_lost(cat, a, "lost"), cat);
    bd_lost_impact after;
    memset(&seen, 0, sizeof(seen));
    REQUIRE_OK(bd_media_lost_impact(cat, a, &after, impact_cb, &seen), cat);
    CHECK(after.old_versions == 2 && after.deleted == 2 && after.old_versions_none == 1 && after.deleted_none == 1);
    CHECK(seen.n == 1 && seen.id[0] == b);
    /* B holds what is left; marked lost too, nothing is left of them. */
    REQUIRE_OK(bd_media_lost_impact(cat, b, &li, impact_cb, &seen), cat);
    CHECK(li.old_versions == 1 && li.deleted == 1 && li.old_versions_none == 1 && li.deleted_none == 1);
    bd_catalog_close(cat);
}

/* ---- D10: a wiped drive set up again ---------------------------------------- */

static void set_seen_days_ago(bd_catalog *cat, int64_t id, int days)
{
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(cat->db, "UPDATE media SET last_seen_ms=?2 WHERE id=?1", -1, &q, NULL) != SQLITE_OK) { failures++; return; }
    sqlite3_bind_int64(q, 1, id);
    sqlite3_bind_int64(q, 2, bd_now_ms() - (int64_t)days * 24 * 3600 * 1000);
    CHECK(sqlite3_step(q) == SQLITE_DONE);
    sqlite3_finalize(q);
}

static int has_id(const int64_t *ids, int n, int64_t id)
{
    for (int i = 0; i < n; i++)
        if (ids[i] == id) return 1;
    return 0;
}

static void test_wiped_drive_set_up_again(void)
{
    bd_catalog *cat;
    bd_mkdirs(at("d10"));
    REQUIRE_OK(bd_catalog_create(at("d10/db.brodalf"), &cat), NULL);
    make_files("d10-src/Docs", 3, "d10");
    int64_t src, stick, recent, bridge, bridge2, elements, id, ids[8];
    bd_scan_stats ss;
    bd_backup_stats bs;
    REQUIRE_OK(bd_source_add(cat, at("d10-src/Docs"), &src), cat);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    /* A stick in a card reader that gives no serial number. */
    bd_mkdirs(at("d10/disk-BLANK-1"));
    REQUIRE_OK(bd_media_init(cat, at("d10/disk-BLANK-1"), "Stick", 0, &stick), cat);
    REQUIRE_OK(bd_backup(cat, stick, 0, &bs, quiet, NULL), cat);
    bd_mkdirs(at("d10/recent"));
    REQUIRE_OK(bd_media_init(cat, at("d10/recent"), "Recent", 0, &recent), cat);
    bd_media_disconnect(cat, stick);
    bd_media_disconnect(cat, recent);
    set_seen_days_ago(cat, stick, BD_AWAY_DAYS + 10);
    CHECK(no_copy(cat) == 0);

    /* It is reformatted and comes back: its hardware says nothing. While
     * its old place still holds its ID, it is there, not wiped. */
    bd_mkdirs(at("d10/disk-BLANK-2"));
    CHECK(!has_id(ids, bd_media_maybe_erased(cat, at("d10/disk-BLANK-2"), "Stick", ids, 8), stick));
    CHECK(rename(at("d10/disk-BLANK-1"), at("d10/stick-before")) == 0);
    CHECK(bd_media_match_disk(cat, at("d10/disk-BLANK-2"), &id) == BD_ERR_NOT_FOUND);
    int n = bd_media_maybe_erased(cat, at("d10/disk-BLANK-2"), "Spare", ids, 8);
    CHECK(n == 1 && ids[0] == stick); /* about the same size (the same file system here) */
    CHECK(!has_id(ids, n, recent));   /* seen lately: not wiped, only not plugged in */
    /* By name too, whatever the case and spaces. */
    n = bd_media_maybe_erased(cat, at("d10/disk-BLANK-2"), " stick ", ids, 8);
    CHECK(n >= 1 && ids[0] == stick);
    /* A drive marked lost is not offered. */
    REQUIRE_OK(bd_media_mark_lost(cat, stick, "lost"), cat);
    CHECK(bd_media_maybe_erased(cat, at("d10/disk-BLANK-2"), "Stick", ids, 8) == 0);
    REQUIRE_OK(bd_media_found(cat, stick), cat);
    set_seen_days_ago(cat, stick, BD_AWAY_DAYS + 10);
    CHECK(bd_media_maybe_erased(cat, at("d10/disk-BLANK-2"), "Stick", ids, 8) == 1);
    /* Yes, it is that drive: its copies stop counting, and it is backed up
     * to again under its own name. */
    REQUIRE_OK(bd_media_erased(cat, stick, at("d10/disk-BLANK-2")), cat);
    CHECK(no_copy(cat) == 3);
    REQUIRE_OK(bd_backup(cat, stick, 0, &bs, quiet, NULL), cat);
    CHECK(no_copy(cat) == 0);
    /* Plugged in now: not offered for another disk. */
    bd_mkdirs(at("d10/disk-BLANK-3"));
    CHECK(!has_id(ids, bd_media_maybe_erased(cat, at("d10/disk-BLANK-3"), "Stick", ids, 8), stick));

    /* A disk whose serial number tells it apart is not an old drive, even
     * of the same size and name. */
    bd_mkdirs(at("d10/disk-ORIG"));
    REQUIRE_OK(bd_media_init(cat, at("d10/disk-ORIG"), "Elements", 0, &elements), cat);
    bd_media_disconnect(cat, elements);
    set_seen_days_ago(cat, elements, BD_AWAY_DAYS + 1);
    bd_mkdirs(at("d10/disk-NEW"));
    n = bd_media_maybe_erased(cat, at("d10/disk-NEW"), "Elements", ids, 8);
    CHECK(!has_id(ids, n, elements));
    /* The same disk wiped comes first, whatever it is called now. */
    char *away = bd_sprintf("%s-old", at("d10/disk-ORIG"));
    CHECK(rename(at("d10/disk-ORIG"), away) == 0);
    free(away);
    bd_mkdirs(at("d10/disk-ORIG"));
    CHECK(bd_media_match_disk(cat, at("d10/disk-ORIG"), &id) == BD_OK && id == elements);
    n = bd_media_maybe_erased(cat, at("d10/disk-ORIG"), "Stick", ids, 8);
    CHECK(n >= 1 && ids[0] == elements);

    /* A USB bridge that gives every disk in it the same serial number: a
     * new disk of the same size must not be taken for the old one. */
    bd_mkdirs(at("d10/disk-SHARED-a"));
    REQUIRE_OK(bd_media_init(cat, at("d10/disk-SHARED-a"), "Bridge A", 0, &bridge), cat);
    bd_media_disconnect(cat, bridge);
    bd_mkdirs(at("d10/disk-SHARED-b"));
    CHECK(bd_media_match_disk(cat, at("d10/disk-SHARED-b"), &id) == BD_OK && id == bridge); /* not known to be shared yet */
    bd_mkdirs(at("d10/disk-BRIDGE2"));
    REQUIRE_OK(bd_media_init(cat, at("d10/disk-BRIDGE2"), "Bridge B", 0, &bridge2), cat);
    bd_media_disconnect(cat, bridge2);
    CHECK(bd_media_match_disk(cat, at("d10/disk-SHARED-b"), &id) == BD_ERR_NOT_FOUND);
    n = bd_media_maybe_erased(cat, at("d10/disk-SHARED-b"), "New", ids, 8);
    CHECK(!has_id(ids, n, bridge)); /* seen lately, and the hardware no longer counts */
    set_seen_days_ago(cat, bridge, BD_AWAY_DAYS + 1);
    CHECK(rename(at("d10/disk-SHARED-a"), at("d10/bridge-before")) == 0);
    n = bd_media_maybe_erased(cat, at("d10/disk-SHARED-b"), "New", ids, 8);
    CHECK(has_id(ids, n, bridge)); /* by size alone, as for a disk that says nothing */
    bd_catalog_close(cat);
}

/* ---- R12: copies whose full path on the drive is long ---------------------- */

static void test_long_paths_counted(void)
{
    bd_catalog *cat;
    bd_mkdirs(at("r12/D"));
    REQUIRE_OK(bd_catalog_create(at("r12/db.brodalf"), &cat), NULL);
    bd_mkdirs(at("r12-src/Docs"));
    write_file(at("r12-src/Docs/short.txt"), "short");
    int64_t src, d;
    bd_scan_stats ss;
    bd_backup_stats bs;
    bd_check_stats cs;
    REQUIRE_OK(bd_source_add(cat, at("r12-src/Docs"), &src), cat);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_media_init(cat, at("r12/D"), "D", 0, &d), cat);
    REQUIRE_OK(bd_backup(cat, d, 0, &bs, quiet, NULL), cat);
    CHECK(bs.long_paths == 0);
    /* How long a copy's full path on the drive is, besides its name. */
    sqlite3_stmt *q;
    char rel[1024] = "";
    if (sqlite3_prepare_v2(cat->db, "SELECT path_on_media FROM copies WHERE media_id=?", -1, &q, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(q, 1, d);
        if (sqlite3_step(q) == SQLITE_ROW) snprintf(rel, sizeof(rel), "%s", (const char *)sqlite3_column_text(q, 0));
        sqlite3_finalize(q);
    }
    char *root = bd_connected_root(cat, d); /* as the count takes it */
    char *dir = root ? bd_media_catalog_dir(cat, root) : NULL;
    free(root);
    REQUIRE_OK(dir && strstr(rel, "short.txt") ? BD_OK : BD_ERR_NOT_FOUND, cat);
    int besides = (int)(strlen(dir) + 1 + strlen(rel) - strlen("short.txt"));
    free(dir);
    /* One copy whose full path is 259 characters, and one of 260. */
    char name[300], *path;
    for (int len = 259; len <= 260; len++) {
        int n = len - besides;
        CHECK(n > 8 && n < 250);
        if (n <= 8 || n >= 250) break;
        memset(name, len == 259 ? 'a' : 'b', (size_t)n - 4);
        memcpy(name + n - 4, ".txt", 5);
        path = bd_path_join(at("r12-src/Docs"), name);
        write_file(path, "long");
        free(path);
    }
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    log_count lc = {"more than 259 characters", 0};
    REQUIRE_OK(bd_backup(cat, d, 0, &bs, count_log, &lc), cat);
    CHECK(bs.files_copied == 2);
    CHECK(bs.long_paths == 1);
    CHECK(lc.n == 1);
    lc.n = 0;
    REQUIRE_OK(bd_media_check(cat, d, &cs, count_log, &lc), cat);
    CHECK(cs.ok == 3);
    CHECK(cs.long_paths == 1);
    CHECK(lc.n == 1);
    /* It is the full path that counts: under a longer root, both are long. */
    CHECK(bd_media_long_paths(cat, d, at("r12/D-at-a-longer-place")) == 2);
    bd_catalog_close(cat);
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
    /* The same on every system: no real disk is read. */
    bd_test_hw_read = fake_hw;

    test_two_copies_same_revision();
    test_backup_stops_early();
    test_room_and_failed_catalog_copy();
    test_key_reaches_drives();
    test_cloned_drive();
    test_lost_drive_takes_old_versions();
    test_wiped_drive_set_up_again();
    test_long_paths_counted();

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("all flow drives checks passed\n");
    return 0;
}
