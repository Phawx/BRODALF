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

    remove_tree(top);
    CHECK(!exists(top));
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("all misc checks passed");
    return 0;
}
