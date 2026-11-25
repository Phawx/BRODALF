/* Tests for the smaller fixes that came out of the 0.9.0 reviews: the drive
 * status numbers, the mass-change warning, folders that cannot be read to
 * the end, stale working copies, saves during a long backup, nested backup
 * roots, FAT32 and big files, saves that change nothing, bit-rot results
 * that survive a reopen and the CSV export. Each test works in its own
 * folder under one temp folder, which is removed at the end. */
#include "brodalf.h"
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <sys/stat.h>
#include <sys/wait.h>
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

static char top[1024];  /* the temp folder for the whole run */
static char base[1100]; /* the current test's folder inside it */

static char *at(const char *rel)
{
    static char buf[8][2048];
    static int i = 0;
    i = (i + 1) % 8;
    snprintf(buf[i], sizeof(buf[i]), "%s/%s", base, rel);
    return buf[i];
}

static void begin(const char *name)
{
    snprintf(base, sizeof(base), "%s/%s", top, name);
    bd_mkdirs(base);
    printf("%s\n", name);
    fflush(stdout);
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

/* Remove a folder and everything in it: the walk lists parents before
 * what is inside them, so going through the list backwards empties each
 * folder before it is removed. */
typedef struct { char **paths; int *dirs; size_t n, cap; const char *root; } tree_list;

static int note_entry(void *ctx, const char *rel, const bd_stat_t *st)
{
    tree_list *t = ctx;
    if (t->n == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 64;
        t->paths = realloc(t->paths, t->cap * sizeof(*t->paths));
        t->dirs = realloc(t->dirs, t->cap * sizeof(*t->dirs));
        if (!t->paths || !t->dirs) exit(1);
    }
    t->paths[t->n] = bd_sprintf("%s/%s", t->root, rel);
    t->dirs[t->n] = st->is_dir && !st->is_link;
    t->n++;
    return 0;
}

static void remove_tree(const char *root)
{
#ifndef _WIN32
    /* A test may have left a folder unreadable. */
    chmod(root, 0755);
#endif
    tree_list t = {0};
    t.root = root;
    bd_walk(root, note_entry, NULL, &t);
    for (size_t i = t.n; i-- > 0;) {
        if (t.dirs[i]) bd_rmdir_empty(t.paths[i]);
        else bd_remove(t.paths[i]);
        free(t.paths[i]);
    }
    free(t.paths);
    free(t.dirs);
    bd_rmdir_empty(root);
}

/* A source folder with a few files, a catalog and a drive, in the current
 * test's folder. */
typedef struct { bd_catalog *cat; int64_t src, drive; } setup;

static setup make_setup(void)
{
    setup u;
    bd_mkdirs(at("src"));
    bd_mkdirs(at("drive"));
    REQUIRE_OK(bd_catalog_create(at("test.brodalf"), &u.cat), NULL);
    REQUIRE_OK(bd_source_add(u.cat, at("src"), &u.src), u.cat);
    REQUIRE_OK(bd_media_init(u.cat, at("drive"), "Drive", 0, &u.drive), u.cat);
    return u;
}

static void scan(bd_catalog *cat)
{
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
}

static bd_backup_stats backup(bd_catalog *cat, int64_t drive)
{
    bd_backup_stats bs;
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    return bs;
}

/* Item 1: the drive's numbers put each live file in exactly one group. */
static void test_drive_status(void)
{
    begin("status");
    write_file(at("src/a.txt"), "first");
    write_file(at("src/b.txt"), "second");
    setup u = make_setup();
    scan(u.cat);
    backup(u.cat, u.drive);
    bd_media_status ms;
    REQUIRE_OK(bd_media_status_get(u.cat, u.drive, &ms), u.cat);
    CHECK(ms.files_current == 2 && ms.bytes_current == 11);
    CHECK(ms.files_behind == 0 && ms.files_not_here == 0);

    write_file(at("src/a.txt"), "first, changed");
    write_file(at("src/c.txt"), "third");
    scan(u.cat);
    REQUIRE_OK(bd_media_status_get(u.cat, u.drive, &ms), u.cat);
    CHECK(ms.files_current == 1 && ms.bytes_current == 6);
    CHECK(ms.files_behind == 1 && ms.bytes_behind == 14);
    CHECK(ms.files_not_here == 1 && ms.bytes_not_here == 5);
    bd_catalog_close(u.cat);
}

static int count_changed(void *ctx, const bd_changed_info *c)
{
    int *n = ctx;
    n[0]++;
    if (c->deleted) n[1]++;
    return 0;
}

/* Item 2: the mass-change warning, for files encrypted in place and for
 * files encrypted to new names with the originals deleted. */
static void test_mass_change(void)
{
    char rel[64], text[64];
    for (int pass = 0; pass < 2; pass++) {
        begin(pass == 0 ? "mass-in-place" : "mass-rename");
        for (int i = 0; i < 60; i++) {
            snprintf(rel, sizeof(rel), "src/f%02d.txt", i);
            snprintf(text, sizeof(text), "file %d", i);
            write_file(at(rel), text);
        }
        setup u = make_setup();
        scan(u.cat);
        backup(u.cat, u.drive);
        bd_backup_preview_info pv;
        REQUIRE_OK(bd_backup_preview(u.cat, u.drive, 0, &pv), u.cat);
        CHECK(pv.files_held == 60 && pv.held_changed == 0 && !pv.mass_change);

        /* 20 of the 60 files, a third. */
        for (int i = 0; i < 20; i++) {
            snprintf(text, sizeof(text), "encrypted %d", i);
            if (pass == 0) {
                snprintf(rel, sizeof(rel), "src/f%02d.txt", i);
                write_file(at(rel), text);
            } else {
                snprintf(rel, sizeof(rel), "src/f%02d.txt.locked", i);
                write_file(at(rel), text);
                snprintf(rel, sizeof(rel), "src/f%02d.txt", i);
                CHECK(bd_remove(at(rel)) == 0);
            }
        }
        scan(u.cat);
        REQUIRE_OK(bd_backup_preview(u.cat, u.drive, 0, &pv), u.cat);
        CHECK(pv.files_held == 60);
        CHECK(pv.held_changed == 20);
        CHECK(pv.mass_change);
        int n[2] = {0, 0};
        REQUIRE_OK(bd_list_changed_on_media(u.cat, u.drive, count_changed, n), u.cat);
        CHECK(n[0] == 20);
        CHECK(n[1] == (pass == 0 ? 0 : 20));

        if (pass == 1) {
            /* Once the user has seen the deleted files they no longer count. */
            REQUIRE_OK(bd_deleted_ack(u.cat, 0), u.cat);
            REQUIRE_OK(bd_backup_preview(u.cat, u.drive, 0, &pv), u.cat);
            CHECK(pv.files_held == 40 && pv.held_changed == 0 && !pv.mass_change);
        }
        bd_catalog_close(u.cat);
    }
}

#ifndef _WIN32
/* Item 3: a folder that cannot be read to the end is reported, and what it
 * did not list is not marked deleted. */
static void test_walk_error(void)
{
    begin("walk-error");
    write_file(at("src/keep/a.txt"), "a");
    write_file(at("src/keep/b.txt"), "b");
    write_file(at("src/keep/c.txt"), "c");
    write_file(at("src/other.txt"), "d");
    setup u = make_setup();
    scan(u.cat);
    backup(u.cat, u.drive);

    bd_test_walk_fail = "keep";
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(u.cat, &ss, quiet, NULL), u.cat);
    bd_test_walk_fail = NULL;
    CHECK(ss.errors >= 1);
    CHECK(ss.files_deleted == 0);
    int64_t deleted = -1;
    REQUIRE_OK(bd_list_deleted(u.cat, 0, 0, NULL, NULL, &deleted), u.cat);
    CHECK(deleted == 0);

    /* Read in full again: still nothing deleted. */
    REQUIRE_OK(bd_scan(u.cat, &ss, quiet, NULL), u.cat);
    CHECK(ss.errors == 0 && ss.files_deleted == 0);
    bd_catalog_close(u.cat);
}
#endif

#ifndef _WIN32
static int count_files(void *ctx, const char *rel, const bd_stat_t *st)
{
    (*(int *)ctx)++;
    return st->is_dir ? BD_WALK_SKIP : 0;
}

/* Item 4: working copies left by a BRODALF that is gone are removed when a
 * catalog is opened; those of a running one, those without a marker (made
 * by 0.9.0) and anything not named like a working copy stay. */
static void test_stale_work_files(void)
{
    begin("stale");
    bd_mkdirs(at("tmp"));
    const char *was = getenv("TMPDIR");
    char *saved = was ? bd_strdup(was) : NULL;
    setenv("TMPDIR", at("tmp"), 1);

    pid_t child = fork();
    if (child == 0) _exit(0);
    waitpid(child, NULL, 0);
    char dead[32], live[32];
    snprintf(dead, sizeof(dead), "%ld\n", (long)child);
    snprintf(live, sizeof(live), "%ld\n", (long)getpid());

    const char *gone = "tmp/brodalf-11111111-1111-4111-8111-111111111111.db";
    const char *running = "tmp/brodalf-22222222-2222-4222-8222-222222222222.db";
    const char *unknown = "tmp/brodalf-33333333-3333-4333-8333-333333333333.db";
    const char *lone = "tmp/brodalf-44444444-4444-4444-8444-444444444444.db";
    char p[256];
    const char *parts[] = {"", "-wal", "-shm", ".snapshot"};
    for (int i = 0; i < 4; i++) {
        snprintf(p, sizeof(p), "%s%s", gone, parts[i]);
        write_file(at(p), "x");
        snprintf(p, sizeof(p), "%s%s", running, parts[i]);
        write_file(at(p), "x");
        snprintf(p, sizeof(p), "%s%s", unknown, parts[i]);
        write_file(at(p), "x");
    }
    snprintf(p, sizeof(p), "%s.owner", gone);
    write_file(at(p), dead);
    snprintf(p, sizeof(p), "%s.owner", running);
    write_file(at(p), live);
    snprintf(p, sizeof(p), "%s.owner", lone);
    write_file(at(p), dead);
    /* Not BRODALF's names, whatever is beside them. */
    const char *others[] = {"tmp/brodalf-11111111-1111-4111-8111-111111111111.dbx",
                            "tmp/brodalf-11111111-1111-4111-8111-111111111111.db.old",
                            "tmp/brodalf-1111111-1111-4111-8111-1111111111111.db",
                            "tmp/brodalf-AAAAAAAA-1111-4111-8111-111111111111.db",
                            "tmp/brodalf-test.db", "tmp/other.db"};
    for (size_t i = 0; i < sizeof(others) / sizeof(others[0]); i++) write_file(at(others[i]), "x");
    bd_mkdirs(at("tmp/brodalf-55555555-5555-4555-8555-555555555555.db-wal"));

    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("test.brodalf"), &cat), NULL);
    for (int i = 0; i < 4; i++) {
        snprintf(p, sizeof(p), "%s%s", gone, parts[i]);
        CHECK(!exists(at(p)));
        snprintf(p, sizeof(p), "%s%s", running, parts[i]);
        CHECK(exists(at(p)));
        snprintf(p, sizeof(p), "%s%s", unknown, parts[i]);
        CHECK(exists(at(p)));
    }
    snprintf(p, sizeof(p), "%s.owner", gone);
    CHECK(!exists(at(p)));
    snprintf(p, sizeof(p), "%s.owner", running);
    CHECK(exists(at(p)));
    snprintf(p, sizeof(p), "%s.owner", lone);
    CHECK(!exists(at(p)));
    for (size_t i = 0; i < sizeof(others) / sizeof(others[0]); i++) CHECK(exists(at(others[i])));
    CHECK(exists(at("tmp/brodalf-55555555-5555-4555-8555-555555555555.db-wal")));

    /* This catalog's own working copy and marker are there while it is
     * open, and gone once it is closed. */
    int open_n = 0, closed_n = 0;
    bd_walk(at("tmp"), count_files, NULL, &open_n);
    bd_catalog_close(cat);
    bd_walk(at("tmp"), count_files, NULL, &closed_n);
    CHECK(open_n >= closed_n + 2);
    CHECK(closed_n == 5 + 4 + (int)(sizeof(others) / sizeof(others[0])) + 1);

    if (saved) setenv("TMPDIR", saved, 1);
    else unsetenv("TMPDIR");
    free(saved);
}
#endif

static int64_t file_mtime(const char *path)
{
    bd_stat_t st;
    return bd_stat(path, &st) == 0 ? st.mtime_ns : -1;
}

/* Item 8: a save with nothing to save leaves the file and the revision
 * alone; every kind of change still gets saved. */
static void test_save_only_changes(void)
{
    begin("save");
    bd_mkdirs(at("src"));
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("test.brodalf"), &cat), NULL);
    int64_t rev = bd_catalog_revision(cat), t = file_mtime(at("test.brodalf"));
    CHECK(rev >= 1 && t > 0);
    CHECK(!bd_catalog_unsaved(cat));
    REQUIRE_OK(bd_catalog_save(cat), cat);
    CHECK(bd_catalog_revision(cat) == rev && file_mtime(at("test.brodalf")) == t);

    /* A change through the API. */
    int64_t src;
    REQUIRE_OK(bd_source_add(cat, at("src"), &src), cat);
    CHECK(bd_catalog_unsaved(cat));
    REQUIRE_OK(bd_catalog_save(cat), cat);
    CHECK(bd_catalog_revision(cat) == rev + 1);
    rev++;

    /* Only the session's TEMP tables: nothing to save. */
    CHECK(bd_exec(cat, "INSERT INTO temp.missing_sources(source_id) VALUES(1); DELETE FROM temp.missing_sources") == 0);
    CHECK(!bd_catalog_unsaved(cat));

    /* A DELETE without WHERE on a table no foreign key involves, which
     * SQLite runs without the update hook. */
    CHECK(bd_exec(cat, "INSERT INTO settings VALUES('test_a', '1'), ('test_b', '2')") == 0);
    REQUIRE_OK(bd_catalog_save(cat), cat);
    CHECK(bd_catalog_revision(cat) == ++rev);
    CHECK(bd_exec(cat, "DELETE FROM settings") == 0);
    CHECK(bd_catalog_unsaved(cat));
    REQUIRE_OK(bd_catalog_save(cat), cat);
    CHECK(bd_catalog_revision(cat) == ++rev);

    /* A schema change. */
    CHECK(bd_exec(cat, "CREATE INDEX IF NOT EXISTS test_only ON space_log(event)") == 0);
    CHECK(bd_catalog_unsaved(cat));
    REQUIRE_OK(bd_catalog_save(cat), cat);
    CHECK(bd_catalog_revision(cat) == ++rev);
    CHECK(bd_exec(cat, "DROP INDEX test_only") == 0);
    REQUIRE_OK(bd_catalog_save(cat), cat);
    CHECK(bd_catalog_revision(cat) == ++rev);

    /* A change kept outside the database. */
    bd_catalog_mark_changed(cat);
    CHECK(bd_catalog_unsaved(cat));
    REQUIRE_OK(bd_catalog_save(cat), cat);
    CHECK(bd_catalog_revision(cat) == ++rev);

    /* A save that fails leaves the change unsaved for the next one. */
    CHECK(bd_exec(cat, "INSERT INTO settings VALUES('test_only', '1')") == 0);
    bd_mkdirs(at("test.brodalf.saving"));
    CHECK(bd_catalog_save(cat) != BD_OK);
    bd_rmdir_empty(at("test.brodalf.saving"));
    CHECK(bd_catalog_unsaved(cat));
    REQUIRE_OK(bd_catalog_save(cat), cat);
    CHECK(!bd_catalog_unsaved(cat));
    rev = bd_catalog_revision(cat);

    /* The file gone: written again. */
    CHECK(bd_remove(at("test.brodalf")) == 0);
    CHECK(bd_catalog_unsaved(cat));
    REQUIRE_OK(bd_catalog_save(cat), cat);
    CHECK(exists(at("test.brodalf")) && bd_catalog_revision(cat) == ++rev);
    bd_catalog_close(cat);

    /* Opened and saved with nothing changed: the same file. */
    t = file_mtime(at("test.brodalf"));
    REQUIRE_OK(bd_catalog_open(at("test.brodalf"), &cat), NULL);
    CHECK(!bd_catalog_unsaved(cat));
    REQUIRE_OK(bd_catalog_save(cat), cat);
    CHECK(bd_catalog_revision(cat) == rev && file_mtime(at("test.brodalf")) == t);
    /* The setting written before the failed save is in the file. */
    char v[8] = "";
    CHECK(bd_setting_get(cat, "test_only", v, sizeof(v)) && strcmp(v, "1") == 0);
    bd_catalog_close(cat);
}

static void copy_file(const char *from, const char *to)
{
    FILE *in = bd_fopen(from, "rb"), *out = bd_fopen(to, "wb");
    if (!in || !out) { fprintf(stderr, "cannot copy %s\n", from); exit(1); }
    char buf[8192];
    for (size_t n; (n = fread(buf, 1, sizeof(buf), in)) > 0;) fwrite(buf, 1, n, out);
    fclose(in);
    fclose(out);
}

typedef struct { const char *part; int n; } part_count;

static int count_part(void *ctx, const char *rel, const bd_stat_t *st)
{
    part_count *c = ctx;
    if (st->is_file && strstr(rel, c->part)) c->n++;
    return 0;
}

static int files_under(const char *dir, const char *part)
{
    part_count c = {part, 0};
    bd_walk(dir, count_part, NULL, &c);
    return c.n;
}

/* Item 5: a copy put under .versions by a backup whose catalog was never
 * saved is recorded by the next backup instead of copied again. */
static void test_adopt_alongside(void)
{
    begin("adopt");
    write_file(at("src/a.txt"), "one");
    setup u = make_setup();
    scan(u.cat);
    CHECK(backup(u.cat, u.drive).files_copied == 1);
    write_file(at("src/a.txt"), "two, a bit longer");
    scan(u.cat);
    REQUIRE_OK(bd_catalog_save(u.cat), u.cat);
    copy_file(at("test.brodalf"), at("before.brodalf"));
    CHECK(backup(u.cat, u.drive).files_copied == 1);
    CHECK(files_under(at("drive"), "/.versions/") == 1);
    bd_catalog_close(u.cat);

    /* What that backup recorded is lost, as after a crash. */
    copy_file(at("before.brodalf"), at("test.brodalf"));
    REQUIRE_OK(bd_catalog_open(at("test.brodalf"), &u.cat), NULL);
    REQUIRE_OK(bd_media_connect(u.cat, at("drive"), &u.drive, quiet, NULL), u.cat);
    bd_backup_stats bs = backup(u.cat, u.drive);
    CHECK(bs.files_copied == 0 && bs.files_already_there == 1 && bs.files_failed == 0);
    CHECK(files_under(at("drive"), "/.versions/") == 1);
    bd_media_status ms;
    REQUIRE_OK(bd_media_status_get(u.cat, u.drive, &ms), u.cat);
    CHECK(ms.files_current == 1 && ms.old_versions == 1);

    /* A file there with other contents is left alone: the next version
     * goes beside it. */
    write_file(at("src/a.txt"), "three, longer again");
    scan(u.cat);
    REQUIRE_OK(bd_catalog_save(u.cat), u.cat);
    bd_catalog_close(u.cat);
    copy_file(at("test.brodalf"), at("before.brodalf"));
    REQUIRE_OK(bd_catalog_open(at("test.brodalf"), &u.cat), NULL);
    REQUIRE_OK(bd_media_connect(u.cat, at("drive"), &u.drive, quiet, NULL), u.cat);
    CHECK(backup(u.cat, u.drive).files_copied == 1);
    bd_catalog_close(u.cat);
    copy_file(at("before.brodalf"), at("test.brodalf"));
    /* Spoil the unrecorded copy: it no longer matches the version. */
    char rel[1200];
    REQUIRE_OK(bd_catalog_open(at("test.brodalf"), &u.cat), NULL);
    snprintf(rel, sizeof(rel), "drive/BRODALF/%s/.versions/src/a.v3.txt", bd_catalog_uuid(u.cat));
    if (exists(at(rel))) write_file(at(rel), "something else entirely");
    else CHECK(!"the copy of version 3 is where it was expected");
    REQUIRE_OK(bd_media_connect(u.cat, at("drive"), &u.drive, quiet, NULL), u.cat);
    bs = backup(u.cat, u.drive);
    CHECK(bs.files_copied == 1 && bs.files_already_there == 0);
    CHECK(files_under(at("drive"), "/.versions/") == 3);
    bd_catalog_close(u.cat);
}

typedef struct { bd_catalog *cat; int saves; } save_watch;

static void stop_after_save(void *ctx, const char *msg)
{
    save_watch *w = ctx;
    if (strncmp(msg, "catalog saved", 13) == 0 && ++w->saves == 1) bd_catalog_cancel(w->cat);
}

/* Item 5: a long backup saves the catalog between batches, so what it
 * copied before it was cut short is in the file. */
static void test_periodic_save(void)
{
    begin("periodic");
    char rel[64];
    for (int i = 0; i < 600; i++) {
        snprintf(rel, sizeof(rel), "src/f%03d.txt", i);
        write_file(at(rel), rel);
    }
    setup u = make_setup();
    scan(u.cat);
    REQUIRE_OK(bd_catalog_save(u.cat), u.cat);
    bd_test_save_every_ms = 1;
    save_watch w = {u.cat, 0};
    bd_backup_stats bs;
    CHECK(bd_backup(u.cat, u.drive, 0, &bs, stop_after_save, &w) == BD_ERR_CANCELLED);
    bd_test_save_every_ms = 0;
    CHECK(w.saves == 1);
    CHECK(bs.files_copied == 256);
    /* Closed without a save, as if BRODALF had ended there. */
    bd_catalog_close(u.cat);
    REQUIRE_OK(bd_catalog_open(at("test.brodalf"), &u.cat), NULL);
    bd_media_status ms;
    REQUIRE_OK(bd_media_status_get(u.cat, u.drive, &ms), u.cat);
    CHECK(ms.files_current == 256 && ms.files_not_here == 600 - 256);
    CHECK(strcmp(ms.last_backup_status, "stopped") == 0 && ms.last_backup_files == 256);
    /* The rest is copied by the next backup, nothing twice. */
    REQUIRE_OK(bd_media_connect(u.cat, at("drive"), &u.drive, quiet, NULL), u.cat);
    bs = backup(u.cat, u.drive);
    CHECK(bs.files_copied == 600 - 256 && bs.files_failed == 0);
    CHECK(files_under(at("drive"), "/.versions/") == 0);
    bd_catalog_close(u.cat);
}

/* Item 6: a backup root inside another, or holding one, is refused with a
 * message naming the root in the way. */
static void test_nested_roots(void)
{
    begin("nested");
    bd_mkdirs(at("src/A/B/C"));
    bd_mkdirs(at("src/AB"));
    bd_mkdirs(at("src/Case/sub"));
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("test.brodalf"), &cat), NULL);
    int64_t a = 0, id = 0;
    REQUIRE_OK(bd_source_add(cat, at("src/A"), &a), cat);

    CHECK(bd_source_add(cat, at("src/A/B"), &id) == BD_ERR_EXISTS);
    CHECK(strstr(bd_catalog_error(cat), at("src/A")) != NULL);
    CHECK(strstr(bd_catalog_error(cat), "inside") != NULL);
    CHECK(bd_source_add(cat, at("src/A/B/C/"), &id) == BD_ERR_EXISTS);
    CHECK(bd_source_add(cat, at("src"), &id) == BD_ERR_EXISTS);
    CHECK(strstr(bd_catalog_error(cat), at("src/A")) != NULL);
    CHECK(strstr(bd_catalog_error(cat), "holds") != NULL);
    CHECK(bd_source_add(cat, at("src/A/B/../../A/B"), &id) == BD_ERR_EXISTS);

    /* The same folder spelled another way is the same root. */
    id = 0;
    REQUIRE_OK(bd_source_add(cat, at("src/A/"), &id), cat);
    CHECK(id == a);
    id = 0;
    REQUIRE_OK(bd_source_add(cat, at("src//A/./B/.."), &id), cat);
    CHECK(id == a);

    /* A name that only starts the same is another folder. */
    int64_t ab = 0;
    REQUIRE_OK(bd_source_add(cat, at("src/AB"), &ab), cat);
    CHECK(ab != 0 && ab != a);

    /* Where names ignore case (Windows), so does the comparison. */
    int64_t c = 0;
    REQUIRE_OK(bd_source_add(cat, at("src/Case"), &c), cat);
    bd_test_fold_case = 1;
    CHECK(bd_source_add(cat, at("src/Case/sub"), &id) == BD_ERR_EXISTS);
    char upper[1200];
    snprintf(upper, sizeof(upper), "%s/src/CASE", base);
    bd_mkdirs(upper);
    id = 0;
    REQUIRE_OK(bd_source_add(cat, upper, &id), cat);
    CHECK(id == c);
    bd_test_fold_case = 0;

    /* A stopped root does not count, but cannot start again while a root
     * inside it is backed up. */
    REQUIRE_OK(bd_source_set_active(cat, a, 0), cat);
    int64_t b = 0;
    REQUIRE_OK(bd_source_add(cat, at("src/A/B"), &b), cat);
    CHECK(b != a);
    CHECK(bd_source_set_active(cat, a, 1) == BD_ERR_EXISTS);
    CHECK(strstr(bd_catalog_error(cat), at("src/A/B")) != NULL);
    REQUIRE_OK(bd_source_set_active(cat, b, 0), cat);
    REQUIRE_OK(bd_source_set_active(cat, a, 1), cat);
    bd_catalog_close(cat);
}

int main(void)
{
    char tmp[512], id[37];
    bd_temp_dir(tmp, sizeof(tmp));
    bd_uuid_v4(id);
    snprintf(top, sizeof(top), "%s/brodalf-misc-%.8s", tmp, id);
    for (char *p = top; *p; p++) if (*p == '\\') *p = '/';
    bd_mkdirs(top);
    printf("working in %s\n", top);

    test_drive_status();
    test_mass_change();
    test_save_only_changes();
    test_adopt_alongside();
    test_periodic_save();
    test_nested_roots();
#ifndef _WIN32
    test_walk_error();
    test_stale_work_files();
#endif

    remove_tree(top);
    CHECK(!exists(top));
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("all misc checks passed");
    return 0;
}
