/* The gaps the 0.11.0 use-case review found: each test fails on 0.11.0. */
#include "brodalf.h"
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
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

/* At-risk files, with how many are online-only of each kind. */
typedef struct { int64_t n, never, changed; } risk_count;

static int risk_cb(void *ctx, const bd_risk_info *r)
{
    risk_count *c = ctx;
    c->n++;
    if (r->online_only == 1) c->never++;
    if (r->online_only == 2) c->changed++;
    return 0;
}

static risk_count at_risk(bd_catalog *cat)
{
    risk_count c = {0, 0, 0};
    bd_list_at_risk(cat, 0, risk_cb, &c, NULL);
    return c;
}

typedef struct { const char *name; int online_only; int state; } node_find;

static int node_cb(void *ctx, const bd_node_info *n)
{
    node_find *f = ctx;
    if (strcmp(n->name, f->name) != 0) return 0;
    f->online_only = n->online_only;
    f->state = (int)n->state;
    return 1;
}

static node_find node_of(bd_catalog *cat, int64_t src, const char *name)
{
    node_find f = {name, -1, -1};
    bd_list_children(cat, src, 0, node_cb, &f);
    return f;
}

static void target_one(bd_catalog *cat)
{
    bd_target t;
    memset(&t, 0, sizeof(t));
    t.copies = 1;
    t.places = 1;
    REQUIRE_OK(bd_target_set(cat, &t), cat);
}

/* ---- Data at risk 1 and convenience 3: online-only cloud files ----------- */

static void test_cloud_files(void)
{
    write_file(at("cloud/OneDrive/notes.txt"), "first notes");
    bd_mkdirs(at("cloud/drive"));
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("cloud/db.brodalf"), &cat), NULL);
    target_one(cat);
    int64_t src, drive;
    REQUIRE_OK(bd_source_add(cat, at("cloud/OneDrive"), &src), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_media_init(cat, at("cloud/drive"), "Blue", 0, &drive), cat);
    bd_backup_stats bs;
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(at_risk(cat).n == 0);

    /* Edited on another device: on this PC it is online-only, with another size. */
    write_file(at("cloud/OneDrive/notes.txt"), "notes edited on the laptop");
    bd_test_online_only = "notes.txt";
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_online_only == 1 && ss.files_cloud_changed == 1);
    risk_count r = at_risk(cat);
    CHECK(r.n == 1 && r.changed == 1); /* 0.11.0: backed up, not at risk */
    bd_risk_stats rk;
    REQUIRE_OK(bd_list_at_risk(cat, 0, NULL, NULL, &rk), cat);
    CHECK(rk.files_at_risk == 1 && rk.files_no_copy == 1);
    CHECK(node_of(cat, src, "notes.txt").online_only == 2);

    /* A cloud file never on this PC: at risk, and said why. */
    write_file(at("cloud/OneDrive/plan.txt"), "a plan");
    bd_test_online_only = ".txt";
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    r = at_risk(cat);
    CHECK(r.n == 2 && r.changed == 1 && r.never == 1);
    CHECK(node_of(cat, src, "plan.txt").online_only == 1);

    /* "Always keep on this device": the next scan reads both. */
    bd_test_online_only = NULL;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_cloud_changed == 0 && ss.files_changed == 1 && ss.files_new == 1);
    r = at_risk(cat);
    CHECK(r.n == 2 && r.changed == 0 && r.never == 0); /* not backed up yet */
    CHECK(node_of(cat, src, "notes.txt").online_only == 0);
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(at_risk(cat).n == 0);
    bd_catalog_close(cat);
}

/* ---- Data at risk 2 and convenience 5: links ----------------------------- */

typedef struct { int n; char rel[256], target[1024]; } link_list;

static int link_cb(void *ctx, const bd_link_info *l)
{
    link_list *k = ctx;
    if (k->n++ == 0) {
        snprintf(k->rel, sizeof(k->rel), "%s", l->rel_path);
        snprintf(k->target, sizeof(k->target), "%s", l->target);
    }
    return 0;
}

static void test_links(void)
{
#ifdef _WIN32
    printf("links: skipped on Windows (making a link needs rights the test may not have)\n");
#else
    make_files("links/Games", 4, "save");
    make_files("links/Docs/sub", 1, "doc");
    write_file(at("links/Docs/a.txt"), "a");
    char *games = bd_strdup(at("links/Games"));
    CHECK(symlink(games, at("links/Docs/Games")) == 0);
    CHECK(symlink("sub", at("links/Docs/alias")) == 0); /* inside the root: its files are backed up anyway */
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("links/db.brodalf"), &cat), NULL);
    int64_t src, games_id;
    REQUIRE_OK(bd_source_add(cat, at("links/Docs"), &src), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.skipped_links == 2);
    CHECK(ss.links_not_followed == 1); /* 0.11.0: only logged */
    link_list k;
    memset(&k, 0, sizeof(k));
    REQUIRE_OK(bd_list_links(cat, link_cb, &k), cat);
    CHECK(k.n == 1 && strcmp(k.rel, "Games") == 0);
    char *real = bd_link_target(games, NULL);
    CHECK(real && strcmp(k.target, real) == 0);

    /* Choosing the link itself as a backup root says where it leads. */
    bd_status s = bd_source_add(cat, at("links/Docs/Games"), &games_id);
    CHECK(s == BD_ERR_INVALID && strstr(bd_catalog_error(cat), "is a link to") != NULL); /* 0.11.0: "is not a folder" */
    char *t = bd_path_link_target(at("links/Docs/Games"));
    CHECK(t && real && strcmp(t, real) == 0);
    CHECK(bd_path_link_target(at("links/Docs/a.txt")) == NULL);

    /* Backing up what it leads to: no longer listed. */
    REQUIRE_OK(bd_source_add(cat, t, &games_id), cat);
    memset(&k, 0, sizeof(k));
    REQUIRE_OK(bd_list_links(cat, link_cb, &k), cat);
    CHECK(k.n == 0);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.skipped_links == 2 && ss.links_not_followed == 0);
    free(t);
    free(real);
    free(games);
    bd_catalog_close(cat);
#endif
}

/* ---- Data at risk 3: a file that changes during every copy ------------- */

typedef struct { int n; char path[1024]; } in_use_list;

static int in_use_cb(void *ctx, const char *path, int64_t size)
{
    in_use_list *l = ctx;
    (void)size;
    if (l->n++ == 0) snprintf(l->path, sizeof(l->path), "%s", path);
    return 0;
}

/* New contents of the same size, with the old modified time: what a
 * program writing into an open file looks like to a backup. */
static void rewrite_same_time(const char *path, const char *text)
{
    bd_stat_t st;
    if (bd_stat(path, &st) != 0) { fprintf(stderr, "cannot stat %s\n", path); exit(1); }
    FILE *f = bd_fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); exit(1); }
    fputs(text, f);
    bd_set_mtime(f, st.mtime_ns);
    fclose(f);
}

static void test_changes_during_copy(void)
{
    write_file(at("vm/Machines/disk.vhdx"), "disk image AAAA");
    bd_mkdirs(at("vm/drive"));
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("vm/db.brodalf"), &cat), NULL);
    int64_t src, drive;
    REQUIRE_OK(bd_source_add(cat, at("vm/Machines"), &src), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_media_init(cat, at("vm/drive"), "Blue", 0, &drive), cat);

    /* Changed once while the backup copied it: scan again, as before. */
    rewrite_same_time(at("vm/Machines/disk.vhdx"), "disk image BBBB");
    bd_backup_stats bs;
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_failed == 1 && bs.files_in_use == 0);
    in_use_list l;
    memset(&l, 0, sizeof(l));
    bd_list_in_use(cat, in_use_cb, &l);
    CHECK(l.n == 0);

    /* Changed again on the next try: listed in use, so the window offers a shadow copy. */
    rewrite_same_time(at("vm/Machines/disk.vhdx"), "disk image CCCC");
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_failed == 1);
    CHECK(bs.files_in_use == 1); /* 0.11.0: 0, failed forever */
    memset(&l, 0, sizeof(l));
    bd_list_in_use(cat, in_use_cb, &l);
    CHECK(l.n == 1 && strstr(l.path, "disk.vhdx") != NULL);
    bd_catalog_close(cat);
}

/* ---- Data at risk 4: two copies on one disk -------------------------------- */

/* The test folders all sit on one disk: a folder's name says which disk
 * it stands for. */
static int fake_hw(const char *root, bd_drive_hw *hw)
{
    memset(hw, 0, sizeof(*hw));
    hw->disk_bytes = -1;
    static const struct { const char *mark, *model, *serial, *health; int64_t bytes; } disks[] = {
        {"disk-SAME", "WD Elements", "WX51A2345", "good", 2000398934016},
        {"disk-OTHER", "Seagate Expansion", "NAB1C2D3", "good", 4000787030016},
        {"disk-ROOT", "Samsung SSD 980", "S64DNX0R123", "good", 1000204886016},
        {"disk-FAIL", "Toshiba Canvio", "Z8T4P1QXA", "failing", 1000204886016},
    };
    for (size_t i = 0; i < sizeof(disks) / sizeof(disks[0]); i++) {
        if (!strstr(root, disks[i].mark)) continue;
        snprintf(hw->model, sizeof(hw->model), "%s", disks[i].model);
        snprintf(hw->serial, sizeof(hw->serial), "%s", disks[i].serial);
        snprintf(hw->health, sizeof(hw->health), "%s", disks[i].health);
        hw->disk_bytes = disks[i].bytes;
        return 0;
    }
    return -1;
}

typedef struct { int64_t id; int uncounted; char same[64]; } media_find;

static int media_cb(void *ctx, const bd_media_info *m)
{
    media_find *f = ctx;
    if (m->media_id != f->id) return 0;
    f->uncounted = m->uncounted;
    snprintf(f->same, sizeof(f->same), "%s", m->same_disk_as ? m->same_disk_as : "");
    return 1;
}

static media_find media_of(bd_catalog *cat, int64_t id)
{
    media_find f;
    memset(&f, 0, sizeof(f));
    f.id = id;
    bd_list_media(cat, media_cb, &f);
    return f;
}

static int64_t copies_of_first(bd_catalog *cat)
{
    bd_set_progress sets[4];
    int n = 0;
    bd_backup_sets(cat, sets, 4, &n);
    return sets[1].files_done; /* files with a second copy */
}

static void test_one_disk(void)
{
    make_files("disk-ROOT/Docs", 3, "doc");
    bd_mkdirs(at("onedisk/disk-SAME-p1"));
    bd_mkdirs(at("onedisk/disk-SAME-p2"));
    bd_mkdirs(at("onedisk/disk-OTHER"));
    bd_mkdirs(at("onedisk/disk-ROOT-backup"));
    bd_mkdirs(at("onedisk/disk-FAIL"));
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("onedisk/db.brodalf"), &cat), NULL);
    bd_target t;
    memset(&t, 0, sizeof(t));
    t.copies = 2;
    t.places = 1;
    REQUIRE_OK(bd_target_set(cat, &t), cat);
    int64_t src, p1, p2, other, own, fail;
    REQUIRE_OK(bd_source_add(cat, at("disk-ROOT/Docs"), &src), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_media_init(cat, at("onedisk/disk-SAME-p1"), "Blue 1", 0, &p1), cat);
    REQUIRE_OK(bd_media_init(cat, at("onedisk/disk-SAME-p2"), "Blue 2", 0, &p2), cat);
    bd_backup_stats bs;
    REQUIRE_OK(bd_backup(cat, p1, 0, &bs, quiet, NULL), cat);
    REQUIRE_OK(bd_backup(cat, p2, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 3);

    /* Two partitions of one disk: one copy, not two. */
    CHECK(at_risk(cat).n == 3); /* 0.11.0: 0 */
    CHECK(copies_of_first(cat) == 0);
    CHECK(strcmp(media_of(cat, p1).same, "Blue 2") == 0);
    CHECK(media_of(cat, p1).uncounted == 0);
    REQUIRE_OK(bd_media_init(cat, at("onedisk/disk-OTHER"), "Red", 0, &other), cat);
    REQUIRE_OK(bd_backup(cat, other, 0, &bs, quiet, NULL), cat);
    CHECK(at_risk(cat).n == 0);
    CHECK(media_of(cat, other).same[0] == 0);

    /* A "drive" on the PC's own disk, where the files are: does not count. */
    REQUIRE_OK(bd_media_init(cat, at("onedisk/disk-ROOT-backup"), "Spare partition", 0, &own), cat);
    CHECK(media_of(cat, own).uncounted == 1);
    bd_media_mark_lost(cat, other, "lost");
    REQUIRE_OK(bd_backup(cat, own, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 3);
    CHECK(at_risk(cat).n == 3); /* 0.11.0: 0, Blue and Spare partition counted as two */

    /* A drive whose health data says it is failing: does not count. */
    REQUIRE_OK(bd_media_init(cat, at("onedisk/disk-FAIL"), "Old", 0, &fail), cat);
    CHECK(media_of(cat, fail).uncounted == 2);
    REQUIRE_OK(bd_backup(cat, fail, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 3);
    CHECK(at_risk(cat).n == 3); /* 0.11.0: 0 */
    bd_catalog_close(cat);
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
    bd_test_hw_read = fake_hw;

    test_new_pc_empty_folders();
    test_root_waits_for_disk();
    test_cloud_files();
    test_links();
    test_changes_during_copy();
    test_one_disk();

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("all gap checks passed\n");
    return 0;
}
