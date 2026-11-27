/* The gaps the 0.11.0 use-case review found: each test fails on 0.11.0. */
#include "brodalf.h"
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#else
#include <windows.h>
#include <direct.h>
#define chdir _chdir
#define getcwd _getcwd
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

typedef struct { int64_t id; int missing, waiting; int64_t gone, total; char path[2048]; } root_info;

static int root_cb(void *ctx, const bd_source_info *s)
{
    root_info *r = ctx;
    if (s->source_id != r->id) return 0;
    r->missing = s->missing;
    r->gone = s->files_gone;
    r->total = s->files_total;
    r->waiting = s->wait_for_disk;
    snprintf(r->path, sizeof(r->path), "%s", s->path ? s->path : "");
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
        {"disk-WIPE", "SanDisk Extreme", "32154D2A8F11", "good", 1000170586112},
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

/* ---- Data at risk 5: a backup drive that was wiped -------------------------- */

static void test_wiped_drive(void)
{
    make_files("wipe-src/Docs", 3, "wipe");
    bd_mkdirs(at("wipe/disk-WIPE-a"));
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("wipe/db.brodalf"), &cat), NULL);
    int64_t src, green, id = -1;
    REQUIRE_OK(bd_source_add(cat, at("wipe-src/Docs"), &src), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    target_one(cat);
    REQUIRE_OK(bd_media_init(cat, at("wipe/disk-WIPE-a"), "Green", 0, &green), cat);
    bd_backup_stats bs;
    REQUIRE_OK(bd_backup(cat, green, 0, &bs, quiet, NULL), cat);
    CHECK(at_risk(cat).n == 0);
    CHECK(bd_media_match_disk(cat, at("wipe/disk-WIPE-a"), &id) == BD_ERR_EXISTS);
    bd_media_disconnect(cat, green);

    /* The same disk, reformatted: empty, no BRODALF folder. */
    bd_mkdirs(at("wipe/disk-WIPE-b"));
    bd_mkdirs(at("wipe/disk-OTHER-c"));
    CHECK(bd_media_match_disk(cat, at("wipe/disk-WIPE-b"), &id) == BD_OK && id == green);
    CHECK(bd_media_match_disk(cat, at("wipe/disk-OTHER-c"), &id) == BD_ERR_NOT_FOUND && id == 0);
    CHECK(at_risk(cat).n == 0); /* nothing changes until the user says so */
    REQUIRE_OK(bd_media_erased(cat, green, at("wipe/disk-WIPE-b")), cat);
    CHECK(at_risk(cat).n == 3);
    CHECK(bd_media_erased(cat, green, at("wipe/disk-WIPE-b")) == BD_ERR_EXISTS);
    REQUIRE_OK(bd_backup(cat, green, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 3);
    CHECK(at_risk(cat).n == 0);
    bd_media_disconnect(cat, green);
    REQUIRE_OK(bd_media_connect(cat, at("wipe/disk-WIPE-b"), &id, quiet, NULL), cat);
    CHECK(id == green);
    bd_catalog_close(cat);
}

/* ---- Data at risk 6: a mass change reaching a new drive --------------------- */

static int changed_cb(void *ctx, const bd_changed_info *c)
{
    (void)c;
    (*(int *)ctx)++;
    return 0;
}

static void test_mass_change_new_drive(void)
{
    make_files("mass-src/Docs", 60, "mass");
    bd_mkdirs(at("mass/A"));
    bd_mkdirs(at("mass/B"));
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("mass/db.brodalf"), &cat), NULL);
    int64_t src, a, b;
    REQUIRE_OK(bd_source_add(cat, at("mass-src/Docs"), &src), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_media_init(cat, at("mass/A"), "A", 0, &a), cat);
    bd_backup_stats bs;
    REQUIRE_OK(bd_backup(cat, a, 0, &bs, quiet, NULL), cat);
    /* Half the files are encrypted in place. */
    for (int i = 0; i < 30; i++) {
        char rel[256];
        snprintf(rel, sizeof(rel), "mass-src/Docs/f%03d.txt", i);
        write_file(at(rel), "ENCRYPTED BY SOMEONE ELSE, MUCH LONGER THAN BEFORE");
    }
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_media_init(cat, at("mass/B"), "B", 0, &b), cat);
    bd_backup_preview_info pv;
    REQUIRE_OK(bd_backup_preview(cat, b, 0, &pv), cat);
    CHECK(pv.mass_change == 1); /* 0.11.0: 0, a new drive holds nothing to compare */
    CHECK(pv.held_everywhere == 1 && pv.files_held == 60 && pv.held_changed == 30);
    int n = 0;
    REQUIRE_OK(bd_list_changed_on_media(cat, 0, changed_cb, &n), cat);
    CHECK(n == 30);
    REQUIRE_OK(bd_backup_preview(cat, a, 0, &pv), cat);
    CHECK(pv.mass_change == 1 && pv.held_everywhere == 0);
    bd_catalog_close(cat);
}

/* ---- Recovery 2: the newest catalog copy on a drive is damaged -------------- */

static void spoil(const char *path)
{
    FILE *f = bd_fopen(path, "r+b");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, n / 2, SEEK_SET);
    for (int i = 0; i < 8; i++) fputc(0x5A ^ i, f);
    fclose(f);
}

static int spoil_next = 0;
static void spoil_once(const char *path)
{
    if (spoil_next) {
        spoil_next = 0;
        spoil(path);
    }
}

typedef struct { char paths[8][2048]; int64_t revs[8]; int n; } revs_seen;

static int revs_cb(void *ctx, const bd_catalog_copy_info *c)
{
    revs_seen *r = ctx;
    if (r->n < 8) {
        snprintf(r->paths[r->n], sizeof(r->paths[0]), "%s", c->path);
        r->revs[r->n++] = c->revision;
    }
    return 0;
}

static void test_damaged_catalog_copy(void)
{
    make_files("dmg-src/Docs", 2, "dmg");
    bd_mkdirs(at("dmg/drive"));
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("dmg/db.brodalf"), &cat), NULL);
    int64_t src, drive;
    REQUIRE_OK(bd_source_add(cat, at("dmg-src/Docs"), &src), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_media_init(cat, at("dmg/drive"), "Blue", 0, &drive), cat);
    bd_backup_stats bs;
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    write_file(at("dmg-src/Docs/new.txt"), "a new file");
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    char dir[2048];
    snprintf(dir, sizeof(dir), "%s/BRODALF/%s", at("dmg/drive"), bd_catalog_uuid(cat));

    /* A copy that does not read back is written again as the next revision. */
    revs_seen before = {0}, after = {0};
    bd_find_catalog_revisions(dir, revs_cb, &before);
    write_file(at("dmg-src/Docs/newer.txt"), "a newer file");
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    spoil_next = 1;
    bd_test_spoil_catalog_copy = spoil_once;
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    bd_test_spoil_catalog_copy = NULL;
    bd_find_catalog_revisions(dir, revs_cb, &after);
    CHECK(after.n == before.n + 2); /* 0.11.0: + 1, and that one damaged */
    CHECK(after.n >= 2 && bd_catalog_copy_readable(cat, after.paths[0]) == BD_OK);
    CHECK(after.n >= 2 && bd_catalog_copy_readable(cat, after.paths[1]) == BD_ERR_FORMAT);

    /* Check drive reads the newest copy. */
    bd_check_stats cs;
    REQUIRE_OK(bd_media_check(cat, drive, &cs, quiet, NULL), cat);
    CHECK(cs.catalog_copy == 1 && cs.catalog_revision == after.revs[0]);
    spoil(after.paths[0]);
    REQUIRE_OK(bd_media_check(cat, drive, &cs, quiet, NULL), cat);
    CHECK(cs.catalog_copy == -1);
    /* The next backup puts a new revision there, though nothing changed. */
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    REQUIRE_OK(bd_media_check(cat, drive, &cs, quiet, NULL), cat);
    CHECK(cs.catalog_copy == 1 && cs.catalog_revision > after.revs[0]);
    revs_seen now = {0};
    bd_find_catalog_revisions(dir, revs_cb, &now);
    spoil(now.paths[0]); /* newest to oldest: spoiled, spoiled, spoiled by the hook, good */

    /* Leftovers from a copy that stopped. */
    char tmp[2200], fresh[2200];
    snprintf(tmp, sizeof(tmp), "%s/Docs/half.txt.brodalf-tmp", dir);
    snprintf(fresh, sizeof(fresh), "%s/Docs/now.txt.brodalf-tmp", dir);
    write_file(tmp, "half of a file");
    write_file(fresh, "being written");
    FILE *f = bd_fopen(tmp, "r+b");
    bd_set_mtime(f, (bd_now_ms() - 2 * 3600 * 1000) * 1000000);
    fclose(f);
    REQUIRE_OK(bd_media_check(cat, drive, &cs, quiet, NULL), cat);
    CHECK(cs.leftovers == 1); /* 0.11.0: nothing said */
    int64_t removed = 0;
    REQUIRE_OK(bd_media_remove_leftovers(cat, drive, &removed), cat);
    CHECK(removed == 1 && !exists(tmp) && exists(fresh));
    bd_catalog_close(cat);

    /* Recover: the newest copy does not open, the next older one does. */
    revs_seen all = {0};
    bd_find_catalog_revisions(dir, revs_cb, &all);
    int opened = -1;
    for (int i = 0; i < all.n && opened < 0; i++) {
        char dest[2048];
        snprintf(dest, sizeof(dest), "%s/recovered-%d.brodalf", at("dmg"), i);
        bd_catalog *rc = NULL;
        if (bd_catalog_recover(all.paths[i], dest) == BD_OK && bd_catalog_open(dest, &rc) == BD_OK) {
            opened = i;
            bd_catalog_close(rc);
        }
    }
    CHECK(opened == 3);

    /* Recovery 5: a copy on a drive is recognised as one. */
    CHECK(bd_path_is_drive_copy(all.paths[0]));
    CHECK(bd_path_is_drive_copy("E:\\BRODALF\\0f8e7a3c-1b2d-4c5e-9f60-7a8b9c0d1e2f\\catalog-old.brodalf"));
    CHECK(bd_path_is_drive_copy("E:\\Backups\\catalog-r00000012.brodalf"));
    CHECK(!bd_path_is_drive_copy(at("dmg/db.brodalf")));
    CHECK(!bd_path_is_drive_copy("C:\\Users\\Sam\\Documents\\catalog.brodalf"));
    CHECK(!bd_path_is_drive_copy("C:\\BRODALF\\notes\\home.brodalf"));
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
    log_count lc = {"Another folder with the same name", 0};
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

/* ---- A backup root is its files, wherever they are ------------------- */

static void set_home(const char *dir)
{
#ifdef _WIN32
    SetEnvironmentVariableA("USERPROFILE", dir);
#else
    if (dir) setenv("HOME", dir, 1);
    else unsetenv("HOME");
#endif
}

static char *get_home(void)
{
#ifdef _WIN32
    char buf[2048];
    DWORD n = GetEnvironmentVariableA("USERPROFILE", buf, sizeof(buf));
    return n && n < sizeof(buf) ? bd_strdup(buf) : NULL;
#else
    const char *h = getenv("HOME");
    return h ? bd_strdup(h) : NULL;
#endif
}

static size_t list_len(char **v)
{
    size_t n = 0;
    while (v && v[n]) n++;
    return n;
}

static int listed(char **v, const char *want)
{
    for (size_t i = 0; v && v[i]; i++)
        if (strcmp(v[i], want) == 0) return 1;
    return 0;
}

static void list_free(char **v)
{
    for (size_t i = 0; v && v[i]; i++) free(v[i]);
    free(v);
}

static void test_root_candidates(void)
{
    char **v = bd_root_candidates("C:\\Users\\alex\\Documents", "C:\\Users\\sam", (1u << 2) | (1u << 3) | (1u << 4), 1);
    CHECK(list_len(v) == 3);
    CHECK(v && strcmp(v[0], "C:\\Users\\sam\\Documents") == 0);
    CHECK(listed(v, "D:\\Users\\alex\\Documents") && listed(v, "E:\\Users\\alex\\Documents"));
    list_free(v);
    /* The home folders on another drive, letter case ignored. */
    v = bd_root_candidates("d:/users/Alex/Pictures", "C:\\Users\\sam", 0, 1);
    CHECK(list_len(v) == 1 && strcmp(v[0], "C:\\Users\\sam/Pictures") == 0);
    list_free(v);
    /* This user's already, or not under a home folder: nothing. */
    v = bd_root_candidates("C:\\Users\\Sam\\Documents", "C:\\Users\\sam", 0, 1);
    CHECK(v && list_len(v) == 0);
    list_free(v);
    v = bd_root_candidates("\\\\server\\share\\Docs", "C:\\Users\\sam", ~0u, 1);
    CHECK(v && list_len(v) == 0);
    list_free(v);
    /* A whole drive, on another letter. */
    v = bd_root_candidates("D:\\", NULL, 1u << 5, 1);
    CHECK(list_len(v) == 1 && strcmp(v[0], "F:\\") == 0);
    list_free(v);
    v = bd_root_candidates("/home/alex/Pictures/2024", "/home/sam", 0, 0);
    CHECK(list_len(v) == 1 && strcmp(v[0], "/home/sam/Pictures/2024") == 0);
    list_free(v);
    v = bd_root_candidates("/media/alex/USB/Photos", "/home/sam/", 0, 0);
    CHECK(list_len(v) == 2 && listed(v, "/media/sam/USB/Photos") && listed(v, "/run/media/sam/USB/Photos"));
    list_free(v);
    v = bd_root_candidates("/home/sam/Docs", "/home/sam", 0, 0);
    CHECK(v && list_len(v) == 0);
    list_free(v);
    v = bd_root_candidates("/srv/data", "/home/sam", 0, 0);
    CHECK(v && list_len(v) == 0);
    list_free(v);
}

/* Other files at the root's path are not the root, whatever the PC. */
static void test_same_path_other_files(void)
{
    make_files("same/Documents", 20, "mine");
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("same/db.brodalf"), &cat), NULL);
    int64_t src;
    REQUIRE_OK(bd_source_add(cat, at("same/Documents"), &src), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_new == 20);

    /* Another folder of files at the same path: other names, and one name
     * the catalog knows with other contents. */
    char *mine = bd_strdup(at("same/Documents-mine"));
    CHECK(bd_rename_noreplace(at("same/Documents"), mine) == 0);
    make_files("same/Documents/other", 25, "theirs");
    write_file(at("same/Documents/f000.txt"), "someone else's f000");
    log_count lc = {"Another folder with the same name", 0};
    REQUIRE_OK(bd_scan(cat, &ss, count_log, &lc), cat);
    CHECK(ss.files_deleted == 0); /* 0.12.0: 19 */
    CHECK(ss.roots_missing == 1 && ss.roots_emptied == 1);
    CHECK(ss.files_new == 0 && ss.files_changed == 0);
    CHECK(lc.n == 1);
    root_info r = root_of(cat, src);
    CHECK(r.missing && r.gone == 19 && r.total == 20);

    /* Its own files back at the path: scanned as usual. */
    char *theirs = bd_strdup(at("same/Documents-theirs"));
    CHECK(bd_rename_noreplace(at("same/Documents"), theirs) == 0);
    CHECK(bd_rename_noreplace(mine, at("same/Documents")) == 0);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.roots_missing == 0 && ss.files_deleted == 0 && ss.files_new == 0 && ss.files_changed == 0);
    CHECK(!root_of(cat, src).missing);

    /* Every file edited and one deleted: changed, not another folder. */
    for (int i = 1; i < 20; i++) {
        char rel[64], text[64];
        snprintf(rel, sizeof(rel), "same/Documents/f%03d.txt", i);
        snprintf(text, sizeof(text), "edited file %d", i);
        write_file(at(rel), text);
    }
    bd_remove(at("same/Documents/f000.txt"));
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.roots_emptied == 0);
    CHECK(ss.files_changed == 19 && ss.files_deleted == 1);
    free(mine);
    free(theirs);
    bd_catalog_close(cat);
}

/* A root not where it was last is looked for by its files. */
static void test_root_found_by_its_files(void)
{
    char *old_home = get_home();
    bd_mkdirs(at("roam/home/sam"));
    set_home(at("roam/home/sam"));
    make_files("roam/home/alex/Documents", 12, "alex");
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("roam/db.brodalf"), &cat), NULL);
    int64_t src;
    REQUIRE_OK(bd_source_add(cat, at("roam/home/alex/Documents"), &src), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_new == 12 && ss.roots_found == 0);

    /* Opened with another user name: Documents is under this user's home
     * folder now, with all its files. */
    char *to = bd_strdup(at("roam/home/sam/Documents"));
    CHECK(bd_rename_noreplace(at("roam/home/alex/Documents"), to) == 0);
    int64_t found = 0, checked = 0;
    int places = 0;
    char *p = bd_source_find(cat, src, &found, &checked, &places);
    CHECK(p && places == 1 && found == 12 && checked == 12);
    free(p);
    log_count lc = {"found it there by its files", 0};
    REQUIRE_OK(bd_scan(cat, &ss, count_log, &lc), cat); /* 0.12.0: missing */
    CHECK(ss.roots_found == 1 && ss.roots_missing == 0);
    CHECK(ss.files_deleted == 0 && ss.files_new == 0);
    CHECK(lc.n == 1);
    CHECK(!root_of(cat, src).missing);
    char *holder = bd_root_holding(cat, at("roam/home/sam/Documents/f001.txt"));
    CHECK(holder && strstr(holder, "sam"));
    free(holder);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.roots_found == 0 && ss.roots_missing == 0 && ss.files_deleted == 0);

    /* A folder of the same name holding other files is not taken for it. */
    char *away = bd_strdup(at("roam/elsewhere"));
    CHECK(bd_rename_noreplace(to, away) == 0);
    bd_mkdirs(at("roam/home/kim"));
    set_home(at("roam/home/kim"));
    make_files("roam/home/kim/Documents", 12, "kim");
    p = bd_source_find(cat, src, &found, &checked, &places);
    CHECK(!p && places == 0);
    free(p);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.roots_found == 0 && ss.roots_missing == 1 && ss.files_deleted == 0);
    CHECK(root_of(cat, src).missing);

    /* Its own files there after all: found again. */
    char *kims = bd_strdup(at("roam/kims"));
    CHECK(bd_rename_noreplace(at("roam/home/kim/Documents"), kims) == 0);
    CHECK(bd_rename_noreplace(away, at("roam/home/kim/Documents")) == 0);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.roots_found == 1 && ss.roots_missing == 0 && ss.files_deleted == 0);
    set_home(old_home);
    free(old_home);
    free(to);
    free(away);
    free(kims);
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

/* ---- Convenience 1: the database saved inside a backup root -------------- */

static void test_database_in_root(void)
{
    write_file(at("inroot/Documents/letter.txt"), "a letter");
    bd_mkdirs(at("inroot/drive"));
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("inroot/Documents/db.brodalf"), &cat), NULL);
    target_one(cat);
    int64_t src, drive;
    REQUIRE_OK(bd_source_add(cat, at("inroot/Documents"), &src), cat);
    REQUIRE_OK(bd_catalog_save(cat), cat);
    write_file(at("inroot/Documents/db.brodalf.saving"), "a save cut short");
    char *holder = bd_root_holding(cat, at("inroot/Documents/db.brodalf"));
    CHECK(holder && strstr(holder, "inroot/Documents"));
    free(holder);
    CHECK(bd_root_holding(cat, at("inroot/elsewhere.brodalf")) == NULL);

    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_new == 1); /* 0.11.0: the database, its lock and the cut-short save too */
    CHECK(node_of(cat, src, "letter.txt").state >= 0);
    CHECK(node_of(cat, src, "db.brodalf").state < 0);
    CHECK(node_of(cat, src, "db.brodalf.lock").state < 0);
    CHECK(node_of(cat, src, "db.brodalf.saving").state < 0);
    REQUIRE_OK(bd_media_init(cat, at("inroot/drive"), "Blue", 0, &drive), cat);
    bd_backup_stats bs;
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 1);
    bd_catalog_close(cat);
}

/* ---- Convenience 2 and 4: a file bigger than every drive; older versions -- */

typedef struct { int64_t node; char rel[256]; } first_file;

static int first_largest(void *ctx, const bd_risk_info *r)
{
    first_file *f = ctx;
    f->node = r->node_id;
    snprintf(f->rel, sizeof(f->rel), "%s", r->rel_path);
    return 1;
}

typedef struct { int n; char rel[4][256]; int64_t versions[4], bytes[4]; } space_rows;

static int space_cb(void *ctx, const bd_version_space *v)
{
    space_rows *r = ctx;
    if (r->n < 4) {
        snprintf(r->rel[r->n], sizeof(r->rel[0]), "%s", v->rel_path);
        r->versions[r->n] = v->old_versions;
        r->bytes[r->n] = v->old_bytes;
    }
    r->n++;
    return 0;
}

static void test_too_big_and_versions(void)
{
    char huge[101];
    memset(huge, 'x', 100);
    huge[100] = 0;
    write_file(at("big/Data/huge.bin"), huge);
    write_file(at("big/Data/sub/huge.bin"), "small one");
    write_file(at("big/Data/notes.txt"), "v1");
    bd_mkdirs(at("big/drive"));
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("big/db.brodalf"), &cat), NULL);
    target_one(cat);
    int64_t src, drive;
    REQUIRE_OK(bd_source_add(cat, at("big/Data"), &src), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_media_init(cat, at("big/drive"), "Blue", 0, &drive), cat);
    /* A drive of 50 bytes, 40 of them free. */
    sqlite3_exec(cat->db, "UPDATE media SET total_bytes=50", NULL, NULL, NULL);
    CHECK(bd_biggest_drive(cat) == 50);
    bd_backup_stats bs;
    bd_test_free_bytes = 40;
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 2 && bs.files_no_room == 1);
    CHECK(bs.files_bigger_than_drives == 1 && bs.bytes_bigger_than_drives == 100); /* 0.11.0: "waits for another drive" */

    /* Left out: only the file at the top of the root, not the one of the same name in sub. */
    first_file f = {0, ""};
    REQUIRE_OK(bd_list_largest(cat, 1, first_largest, &f), cat);
    CHECK(strcmp(f.rel, "huge.bin") == 0);
    REQUIRE_OK(bd_leave_out(cat, f.node), cat);
    REQUIRE_OK(bd_leave_out(cat, f.node), cat); /* twice: listed once */
    char *list = bd_source_skip_get(cat, src);
    CHECK(list && strcmp(list, "/huge.bin\n") == 0);
    CHECK(list && bd_skip_match(list, "huge.bin", 0) && !bd_skip_match(list, "sub/huge.bin", 0));
    free(list);
    CHECK(exists(at("big/Data/huge.bin"))); /* nothing deleted */
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_no_room == 0 && bs.files_bigger_than_drives == 0);

    /* A drive that never said its size: no "bigger than every drive". */
    int64_t other;
    bd_mkdirs(at("big/drive2"));
    REQUIRE_OK(bd_media_init(cat, at("big/drive2"), "Red", 0, &other), cat);
    sqlite3_exec(cat->db, "UPDATE media SET total_bytes=NULL WHERE label='Red'", NULL, NULL, NULL);
    CHECK(bd_biggest_drive(cat) == -1);

    /* Older versions: notes.txt changed twice, sub/huge.bin once. */
    bd_test_free_bytes = 1000;
    write_file(at("big/Data/notes.txt"), "version 2");
    write_file(at("big/Data/sub/huge.bin"), "small one, changed");
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    write_file(at("big/Data/notes.txt"), "version three");
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    bd_test_free_bytes = -1;
    space_rows r;
    memset(&r, 0, sizeof(r));
    REQUIRE_OK(bd_list_version_space(cat, 10, space_cb, &r), cat);
    CHECK(r.n == 2);
    CHECK(strcmp(r.rel[0], "notes.txt") == 0 || strcmp(r.rel[0], "sub/huge.bin") == 0);
    for (int i = 0; i < r.n && i < 4; i++) {
        if (strcmp(r.rel[i], "notes.txt") == 0) CHECK(r.versions[i] == 2 && r.bytes[i] == 2 + 9);
        else CHECK(strcmp(r.rel[i], "sub/huge.bin") == 0 && r.versions[i] == 1 && r.bytes[i] == 9);
    }
    CHECK(r.n == 2 && r.bytes[0] >= r.bytes[1]);
    bd_catalog_close(cat);
}

/* ---- Convenience 6: room for a restore ----------------------------------- */

static int first_version(void *ctx, const bd_copy_info *c)
{
    *(int64_t *)ctx = c->version_id;
    return 1;
}

static void test_restore_room(void)
{
    write_file(at("room/Data/a.txt"), "0123456789");
    write_file(at("room/Data/sub/b.txt"), "01234");
    bd_mkdirs(at("room/drive"));
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("room/db.brodalf"), &cat), NULL);
    target_one(cat);
    int64_t src, drive;
    REQUIRE_OK(bd_source_add(cat, at("room/Data"), &src), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_media_init(cat, at("room/drive"), "Blue", 0, &drive), cat);
    bd_backup_stats bs;
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);

    /* Into a folder not made yet: the free space of the disk it would be on. */
    int64_t need = -1, freeb = -2;
    REQUIRE_OK(bd_restore_room(cat, src, "", at("room/out/new/deeper"), NULL, 0, &need, &freeb), cat);
    CHECK(need == 15 && freeb > 0);
    bd_test_free_bytes = 12;
    REQUIRE_OK(bd_restore_room(cat, src, "", at("room/out"), NULL, 0, &need, &freeb), cat);
    CHECK(need == 15 && freeb == 12);
    REQUIRE_OK(bd_restore_room(cat, src, "sub", at("room/out"), NULL, 0, &need, &freeb), cat);
    CHECK(need == 5);
    bd_test_free_bytes = -1;
    bd_restore_stats rs;
    REQUIRE_OK(bd_restore(cat, src, "", at("room/out"), &rs, quiet, NULL), cat);
    REQUIRE_OK(bd_restore_room(cat, src, "", at("room/out"), NULL, 0, &need, &freeb), cat);
    CHECK(need == 0); /* already there */

    /* One version of one file. */
    int64_t node = 0, vid = 0;
    REQUIRE_OK(bd_find_node(cat, src, "a.txt", &node), cat);
    bd_list_copies(cat, node, first_version, &vid);
    REQUIRE_OK(bd_restore_room(cat, 0, NULL, at("room/one"), NULL, vid, &need, &freeb), cat);
    CHECK(vid > 0 && need == 10);
    bd_catalog_close(cat);
}

/* ---- Any folders: kept as full paths ----------------------------------- */

static void test_full_paths(void)
{
    write_file(at("full/work/docs/a.txt"), "alpha");
    bd_mkdirs(at("full/work/other"));
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("full/db.brodalf"), &cat), NULL);
    char was[4096];
    CHECK(getcwd(was, sizeof(was)) != NULL);
    CHECK(chdir(at("full/work")) == 0);
    int64_t id = 0, again = 0, other = 0;
    int is_new = 0;
    REQUIRE_OK(bd_source_add_ex(cat, "docs", &id, &is_new), cat); /* 0.13.0: kept as "docs" */
    CHECK(is_new == 1);
    is_new = 1;
    REQUIRE_OK(bd_source_add_ex(cat, "./other/../docs/", &again, &is_new), cat);
    CHECK(again == id && is_new == 0);
    REQUIRE_OK(bd_source_add(cat, "other", &other), cat);
    CHECK(chdir(was) == 0);
    root_info r = root_of(cat, id);
    CHECK(bd_path_is_full(r.path) && strstr(r.path, "docs") && !strstr(r.path, ".."));
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat); /* from another folder */
    CHECK(ss.files_new == 1 && ss.roots_missing == 0);

    /* A root moved to a folder typed from the current folder. */
    CHECK(chdir(at("full")) == 0);
    REQUIRE_OK(bd_source_relocate(cat, other, "work/other"), cat);
    CHECK(chdir(was) == 0);
    CHECK(bd_path_is_full(root_of(cat, other).path));

    /* A root kept as typed by 0.13.0 and older ("work/docs", typed in the
     * database's folder): found from there by its files and given its full
     * path, nothing marked deleted. */
    sqlite3_stmt *u;
    CHECK(sqlite3_prepare_v2(cat->db, "UPDATE sources SET path='work/docs' WHERE id=?", -1, &u, NULL) == SQLITE_OK);
    sqlite3_bind_int64(u, 1, id);
    CHECK(sqlite3_step(u) == SQLITE_DONE);
    sqlite3_finalize(u);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.roots_found == 1 && ss.roots_missing == 0 && ss.files_deleted == 0);
    r = root_of(cat, id);
    CHECK(bd_path_is_full(r.path) && strstr(r.path, "docs"));
    bd_catalog_close(cat);
}

/* ---- Any folders: one folder reached two ways --------------------------- */

#ifdef _WIN32
/* A drive letter that is not in use, from Z: down; 0 if none. */
static wchar_t free_letter(void)
{
    DWORD used = GetLogicalDrives();
    for (int i = 25; i >= 4; i--)
        if (!(used & (1u << i))) return (wchar_t)(L'A' + i);
    return 0;
}

static wchar_t *wide_of(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = malloc(sizeof(wchar_t) * (size_t)(n > 0 ? n : 1));
    if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    for (wchar_t *p = w; p && *p; p++) if (*p == L'/') *p = L'\\';
    return w;
}
#endif

static void test_same_folder_two_ways(void)
{
    make_files("ways/real/Photos", 3, "photo");
    bd_mkdirs(at("ways/real/Photos/sub"));
    make_files("ways/Music", 2, "music");
    /* The folder holding everything, reached another way: through a link
     * to it, or on Windows a subst drive letter standing for it. */
    char *other = NULL;
#ifndef _WIN32
    if (symlink(at("ways"), at("ways-link")) == 0) other = bd_strdup(at("ways-link"));
#else
    wchar_t letter = free_letter(), dev[3] = {letter, L':', 0};
    wchar_t *target = wide_of(at("ways"));
    if (letter && target && DefineDosDeviceW(0, dev, target)) other = bd_sprintf("%c:", (char)letter);
    free(target);
#endif
    if (!other) {
        printf("one folder reached two ways: not tried, no second way could be made\n");
        return;
    }
    char *holder = bd_sprintf("%s/real", other), *photos = bd_sprintf("%s/real/Photos", other),
         *sub = bd_sprintf("%s/real/Photos/sub", other);
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("ways/db.brodalf"), &cat), NULL);
    int64_t id = 0, again = 0, music = 0, x = 0;
    int is_new = 0;
    REQUIRE_OK(bd_source_add(cat, at("ways/real/Photos"), &id), cat);
    REQUIRE_OK(bd_source_add_ex(cat, photos, &again, &is_new), cat); /* 0.13.0: a second root */
    CHECK(again == id && is_new == 0);
    CHECK(bd_source_add(cat, sub, &x) == BD_ERR_EXISTS);
    CHECK(strstr(bd_catalog_error(cat), "another path") != NULL);
    CHECK(bd_source_add(cat, holder, &x) == BD_ERR_EXISTS); /* holds Photos */
    REQUIRE_OK(bd_source_add(cat, at("ways/Music"), &music), cat);
    CHECK(bd_source_relocate(cat, music, photos) == BD_ERR_EXISTS);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_new == 5);
    REQUIRE_OK(bd_catalog_save(cat), cat);
    bd_catalog_close(cat);

    /* The database opened the other way, kept inside the root: its files
     * are not backed up as the root's. */
    char *db2 = bd_sprintf("%s/real/Photos/inside.brodalf", other);
    REQUIRE_OK(bd_catalog_create(db2, &cat), NULL);
    REQUIRE_OK(bd_source_add(cat, at("ways/real/Photos"), &id), cat);
    REQUIRE_OK(bd_catalog_save(cat), cat);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_new == 3); /* 0.13.0: the database file too */
    bd_catalog_close(cat);
    free(db2);

#ifdef _WIN32
    DefineDosDeviceW(DDD_REMOVE_DEFINITION, dev, NULL);
    /* A mapped network drive and its network path: tried where Windows
     * lets a drive letter be mapped to this PC's own share of the drive. */
    wchar_t share[64], net[3] = {free_letter(), L':', 0};
    swprintf(share, 64, L"\\\\localhost\\%c$", base[0]);
    NETRESOURCEW nr;
    memset(&nr, 0, sizeof(nr));
    nr.dwType = RESOURCETYPE_DISK;
    nr.lpLocalName = net;
    nr.lpRemoteName = share;
    DWORD rc = net[0] ? WNetAddConnection2W(&nr, NULL, NULL, 0) : ERROR_NO_MORE_ITEMS;
    if (rc != NO_ERROR) {
        printf("mapped network drive: not tried, mapping %ls failed (%lu)\n", share, (unsigned long)rc);
    } else {
        make_files("ways/net/Docs", 2, "net");
        char *unc = bd_sprintf("\\\\localhost\\%c$%s", base[0], at("ways/net/Docs") + 2);
        char *mapped = bd_sprintf("%c:%s", (char)net[0], at("ways/net/Docs") + 2);
        REQUIRE_OK(bd_catalog_create(at("ways/net.brodalf"), &cat), NULL);
        int64_t a = 0, b = 0;
        REQUIRE_OK(bd_source_add(cat, unc, &a), cat);
        is_new = 1;
        REQUIRE_OK(bd_source_add_ex(cat, mapped, &b, &is_new), cat);
        CHECK(a == b && is_new == 0);
        printf("mapped network drive: %s and %s are one root\n", mapped, unc);
        bd_catalog_close(cat);
        free(unc);
        free(mapped);
        WNetCancelConnection2W(net, 0, TRUE);
    }
#endif
    free(holder);
    free(photos);
    free(sub);
    free(other);
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
    test_root_candidates();
    test_same_path_other_files();
    test_root_found_by_its_files();
    test_full_paths();
    test_same_folder_two_ways();
    test_root_waits_for_disk();
    test_cloud_files();
    test_links();
    test_changes_during_copy();
    test_one_disk();
    test_wiped_drive();
    test_mass_change_new_drive();
    test_damaged_catalog_copy();
    test_database_in_root();
    test_too_big_and_versions();
    test_restore_room();

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("all gap checks passed\n");
    return 0;
}
