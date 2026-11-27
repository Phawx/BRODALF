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

typedef struct { int64_t id; int missing, waiting; int64_t gone, total, last_scan_ms; char path[2048]; } root_info;

static int root_cb(void *ctx, const bd_source_info *s)
{
    root_info *r = ctx;
    if (s->source_id != r->id) return 0;
    r->missing = s->missing;
    r->gone = s->files_gone;
    r->total = s->files_total;
    r->waiting = s->wait_for_disk;
    r->last_scan_ms = s->last_scan_ms;
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

/* The same path, either slash. */
static int same_path(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a == '\\' ? '/' : *a, y = *b == '\\' ? '/' : *b;
        if (x != y) return 0;
    }
    return *a == *b;
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

/* D4: a missing root is pointed somewhere else without asking only on a
 * sure match, never while it waits for its disk, and the old folder
 * coming back is said. */
static void test_root_moved_only_when_sure(void)
{
    char *old_home = get_home();
    bd_mkdirs(at("mv/home/sam"));
    set_home(at("mv/home/sam"));
    make_files("mv/home/alex/Docs", 20, "alex");
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("mv/db.brodalf"), &cat), NULL);
    int64_t src;
    REQUIRE_OK(bd_source_add(cat, at("mv/home/alex/Docs"), &src), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_new == 20);
    char *where = bd_strdup(root_of(cat, src).path);
    char *away = bd_strdup(at("mv/unplugged"));
    CHECK(bd_rename_noreplace(at("mv/home/alex/Docs"), away) == 0);

    /* Waiting for its disk: an old copy elsewhere is not it. */
    make_files("mv/home/sam/Docs", 20, "alex");
    REQUIRE_OK(bd_source_wait_for_disk(cat, src, 1), cat);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.roots_found == 0 && ss.roots_waiting == 1 && ss.files_deleted == 0);
    CHECK(same_path(root_of(cat, src).path, where));
    REQUIRE_OK(bd_source_wait_for_disk(cat, src, 0), cat);

    /* A partial copy (12 of 20): offered, not taken; nothing deleted. */
    for (int i = 12; i < 20; i++) {
        char rel[64];
        snprintf(rel, sizeof(rel), "mv/home/sam/Docs/f%03d.txt", i);
        CHECK(bd_remove(at(rel)) == 0);
    }
    log_count lc = {"not enough to point the root there", 0};
    REQUIRE_OK(bd_scan(cat, &ss, count_log, &lc), cat);
    CHECK(ss.roots_found == 0 && ss.roots_missing == 1 && ss.files_deleted == 0);
    CHECK(lc.n == 1);
    CHECK(same_path(root_of(cat, src).path, where));
    CHECK(count_rows(cat, "SELECT COUNT(*) FROM nodes WHERE deleted=1") == 0);

    /* Every name and size, other contents: not taken either. */
    make_files("mv/home/sam/Docs", 20, "ALEX");
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.roots_found == 0 && ss.roots_missing == 1);
    CHECK(same_path(root_of(cat, src).path, where));

    /* A whole copy, contents and all: taken, and the old place kept. */
    make_files("mv/home/sam/Docs", 20, "alex");
    lc.want = "found it there by its files";
    lc.n = 0;
    REQUIRE_OK(bd_scan(cat, &ss, count_log, &lc), cat);
    CHECK(ss.roots_found == 1 && ss.roots_missing == 0 && ss.files_deleted == 0 && ss.roots_back == 0);
    CHECK(lc.n == 1);
    CHECK(strstr(root_of(cat, src).path, "sam") != NULL);
    int back = -1;
    char *old = bd_source_old_place(cat, src, &back);
    CHECK(old && same_path(old, where) && back == 0);
    free(old);

    /* The real folder is back: said, every scan, until the user chooses. */
    CHECK(bd_rename_noreplace(away, at("mv/home/alex/Docs")) == 0);
    lc.want = "is there again and holds its files";
    lc.n = 0;
    REQUIRE_OK(bd_scan(cat, &ss, count_log, &lc), cat);
    CHECK(ss.roots_back == 1 && lc.n == 1);
    old = bd_source_old_place(cat, src, &back);
    CHECK(old && back == 1);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.roots_back == 1);
    /* "That is the real one": pointed back, and no longer said. */
    REQUIRE_OK(bd_source_relocate(cat, src, old), cat);
    free(old);
    CHECK(bd_source_old_place(cat, src, NULL) == NULL);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.roots_back == 0 && ss.roots_missing == 0 && ss.files_deleted == 0);
    CHECK(same_path(root_of(cat, src).path, where));
    /* Or "keep it here" forgets the old place. */
    char key[48];
    snprintf(key, sizeof(key), "moved_from.%lld", (long long)src);
    CHECK(bd_setting_set(cat, key, at("mv/home/sam/Docs")) == 0);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.roots_back == 1);
    REQUIRE_OK(bd_source_keep_place(cat, src), cat);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.roots_back == 0);
    /* As does pointing it where it is (root-move to the same folder). */
    CHECK(bd_setting_set(cat, key, at("mv/home/sam/Docs")) == 0);
    REQUIRE_OK(bd_source_relocate(cat, src, where), cat);
    CHECK(bd_source_old_place(cat, src, NULL) == NULL);

    set_home(old_home);
    free(old_home);
    free(where);
    free(away);
    bd_catalog_close(cat);
}

typedef struct { int n; char path[4][2048]; int64_t found[4], checked[4]; } place_seen;

static int place_cb(void *ctx, const bd_place_info *p)
{
    place_seen *l = ctx;
    if (l->n < 4) {
        snprintf(l->path[l->n], sizeof(l->path[0]), "%s", p->path);
        l->found[l->n] = p->found;
        l->checked[l->n] = p->checked;
    }
    l->n++;
    return 0;
}

/* C7: every folder holding a missing root's files is listed, with how
 * many of the files checked each holds, most first. */
static void test_every_place_listed(void)
{
    /* A root kept as it was typed (0.13.0): looked for from the database's
     * folder and from the current one. */
    make_files("two/Docs", 10, "doc");
    make_files("two/cwd/Docs", 8, "doc");
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("two/db.brodalf"), &cat), NULL);
    int64_t src;
    REQUIRE_OK(bd_source_add(cat, at("two/Docs"), &src), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    char sql[128];
    snprintf(sql, sizeof(sql), "UPDATE sources SET path='Docs' WHERE id=%lld", (long long)src);
    CHECK(sqlite3_exec(cat->db, sql, NULL, NULL, NULL) == SQLITE_OK);
    char cwd[2048];
    CHECK(getcwd(cwd, sizeof(cwd)) != NULL);
    CHECK(chdir(at("two/cwd")) == 0);
    place_seen l;
    memset(&l, 0, sizeof(l));
    CHECK(bd_source_find_all(cat, src, place_cb, &l) == 2);
    CHECK(l.n == 2);
    CHECK(strstr(l.path[0], "cwd") == NULL && l.found[0] == 10 && l.checked[0] == 10);
    CHECK(strstr(l.path[1], "cwd") != NULL && l.found[1] == 8 && l.checked[1] == 10);
    /* Two places: the scan picks neither. */
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.roots_found == 0 && strcmp(root_of(cat, src).path, "Docs") == 0);
    CHECK(chdir(cwd) == 0);
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
    test_root_moved_only_when_sure();
    test_every_place_listed();

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("all flow scan checks passed\n");
    return 0;
}
