/* An encrypted catalog is never on the disk unencrypted: while it is open
 * its database lives in memory, and nothing of it goes to a file but the
 * encrypted catalog itself. Checked three ways: SQLite opens no file at all
 * while an encrypted catalog is open (its file opening is counted), the
 * temp folder stays empty, and no file anywhere in the test's folders holds
 * a SQLite database or a name only the catalog knows. Also: turning
 * encryption on moves an open catalog into memory, a crash leaves nothing
 * behind, files written the 0.10.1 way still open (and 0.10.1's way opens
 * the new ones), an in-memory catalog grows past SQLite's memory database
 * limit, and a save taken while another thread writes is a whole database.
 * Everything is made in one temp folder, removed at the end. */
#include "brodalf.h"
#include "internal.h"
#include "test_tree.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
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

#define PASS "correct horse battery"
/* A file name: only the catalog knows it, so a file holding it holds the
 * catalog's contents unencrypted. */
#define SECRET_NAME "wombat-7f3a-unmistakable.txt"

static char top[1024];  /* everything the test makes */
static char tmp[1100];  /* the temp folder the catalogs see */
static char base[1100]; /* the current part's folder */

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

static void quiet(void *ctx, const char *msg)
{
    (void)ctx;
    (void)msg;
}

/* ---- Every file SQLite opens ------------------------------------------- */

/* The system's own file layer, which every SQLite file goes through
 * (in-memory databases hand theirs to it too): its open is counted. */
static int (*os_open)(sqlite3_vfs *, const char *, sqlite3_file *, int, int *);
static volatile long file_opens = 0;
static char last_open[600];

static int counting_open(sqlite3_vfs *vfs, const char *name, sqlite3_file *f, int flags, int *out)
{
    file_opens++;
    snprintf(last_open, sizeof(last_open), "%s (flags 0x%x)", name ? name : "a temporary file", (unsigned)flags);
    return os_open(vfs, name, f, flags, out);
}

static void count_opens_from_now(void)
{
    file_opens = 0;
    last_open[0] = '\0';
}

#define CHECK_NO_OPENS()                                                       \
    do {                                                                       \
        CHECK(file_opens == 0);                                                \
        if (file_opens) fprintf(stderr, "  SQLite opened %ld file(s), last %s\n", (long)file_opens, last_open); \
    } while (0)

/* ---- Files on the disk ---------------------------------------------------- */

static int count_cb(void *ctx, const char *rel, const bd_stat_t *st)
{
    (void)rel;
    if (!st->is_dir) (*(int *)ctx)++;
    return 0;
}

static int files_in(const char *dir)
{
    int n = 0;
    bd_walk(dir, count_cb, NULL, &n);
    return n;
}

static int holds(const unsigned char *buf, size_t n, const void *needle, size_t k)
{
    for (size_t i = 0; i + k <= n; i++)
        if (memcmp(buf + i, needle, k) == 0) return 1;
    return 0;
}

typedef struct { const char *root; int bad; } plain_scan;

/* A file holding a SQLite database (its header) or the secret name. */
static int plain_cb(void *ctx, const char *rel, const bd_stat_t *st)
{
    plain_scan *p = ctx;
    if (st->is_dir || st->is_link) return 0;
    char *path = bd_sprintf("%s/%s", p->root, rel);
    FILE *f = path ? bd_fopen(path, "rb") : NULL;
    if (f) {
        static unsigned char buf[1 << 22];
        size_t n = fread(buf, 1, sizeof(buf), f);
        fclose(f);
        if (holds(buf, n, "SQLite format 3", 16) || holds(buf, n, SECRET_NAME, strlen(SECRET_NAME))) {
            fprintf(stderr, "  readable catalog contents in %s\n", path);
            p->bad++;
        }
    }
    free(path);
    return 0;
}

/* How many files under dir hold any of the catalog unencrypted. */
static int plain_files(const char *dir)
{
    plain_scan p = {dir, 0};
    bd_walk(dir, plain_cb, NULL, &p);
    return p.bad;
}

/* No readable copy of the catalog anywhere the test reaches. */
#define CHECK_NOTHING_READABLE()                                               \
    do {                                                                       \
        CHECK(files_in(tmp) == 0);                                             \
        CHECK(plain_files(tmp) == 0);                                          \
        CHECK(plain_files(base) == 0);                                         \
    } while (0)

static int64_t count_rows(bd_catalog *cat, const char *sql)
{
    sqlite3_stmt *q;
    int64_t n = -1;
    if (sqlite3_prepare_v2(cat->db, sql, -1, &q, NULL) != SQLITE_OK) return -1;
    if (sqlite3_step(q) == SQLITE_ROW) n = sqlite3_column_int64(q, 0);
    sqlite3_finalize(q);
    return n;
}

#define SECRET_ROWS "SELECT count(*) FROM nodes WHERE name='" SECRET_NAME "'"

/* About 2 MB of settings: more than the in-memory limit this test sets
 * for SQLite (256 KiB), which the catalog has to lift. */
static void grow(bd_catalog *cat, const char *tag)
{
    char key[64], value[1100];
    memset(value, 'v', 1024);
    value[1024] = '\0';
    CHECK(bd_exec(cat, "BEGIN") == 0);
    for (int i = 0; i < 2000; i++) {
        snprintf(key, sizeof(key), "test.%s.%d", tag, i);
        CHECK(bd_setting_set(cat, key, value) == 0);
    }
    CHECK(bd_exec(cat, "COMMIT") == 0);
}

/* ---- The parts ------------------------------------------------------------ */

/* A plain catalog uses a working copy as before; turning encryption on
 * moves it into memory there and then, and from then on SQLite opens no
 * file, whatever the catalog does. */
static void test_switch_on(void)
{
    begin("switch");
    write_file(at("src/" SECRET_NAME), "ordinary text");
    write_file(at("src/other.txt"), "more ordinary text");
    bd_mkdirs(at("drive"));
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("c.brodalf"), &cat), NULL);
    CHECK(files_in(tmp) >= 1); /* the plain catalog's working copy */
    int64_t src;
    REQUIRE_OK(bd_source_add(cat, at("src"), &src), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(count_rows(cat, SECRET_ROWS) == 1);

    REQUIRE_OK(bd_catalog_set_passphrase(cat, PASS), cat);
    REQUIRE_OK(bd_catalog_set_file_encrypted(cat, 1), cat);
    CHECK(cat->in_memory);
    CHECK(files_in(tmp) == 0); /* the working copy and its marker are gone */
    count_opens_from_now();
    REQUIRE_OK(bd_catalog_save(cat), cat);
    CHECK(bd_catalog_file_needs_passphrase(at("c.brodalf")));
    CHECK(count_rows(cat, SECRET_ROWS) == 1);

    /* A drive, a backup, a change, another scan and backup, a check. */
    int64_t drive;
    REQUIRE_OK(bd_media_init(cat, at("drive"), "Drive", 0, &drive), cat);
    bd_backup_stats bs;
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 2);
    write_file(at("src/other.txt"), "changed text, a little longer");
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 1);
    bd_check_stats cs;
    REQUIRE_OK(bd_media_check(cat, drive, &cs, quiet, NULL), cat);
    CHECK(cs.bad == 0 && cs.missing == 0);
    grow(cat, "switch");
    REQUIRE_OK(bd_catalog_save(cat), cat);
    CHECK_NO_OPENS();
    CHECK_NOTHING_READABLE();
    bd_catalog_close(cat);
    CHECK_NO_OPENS();
    CHECK_NOTHING_READABLE();
}

/* Opened again: straight into memory, and still nothing on the disk. */
static void test_reopen(void)
{
    snprintf(base, sizeof(base), "%s/switch", top);
    printf("reopen\n");
    count_opens_from_now();
    bd_catalog *cat;
    CHECK(bd_catalog_open(at("c.brodalf"), &cat) == BD_ERR_PASSPHRASE);
    REQUIRE_OK(bd_catalog_open_with(at("c.brodalf"), PASS, &cat), NULL);
    CHECK(cat->in_memory);
    CHECK(files_in(tmp) == 0);
    CHECK(count_rows(cat, SECRET_ROWS) == 1);
    char v[32];
    CHECK(bd_setting_get(cat, "test.switch.1999", v, sizeof(v)));
    grow(cat, "reopen");
    REQUIRE_OK(bd_catalog_save(cat), cat);
    CHECK_NO_OPENS();
    CHECK_NOTHING_READABLE();
    /* Turned off, it stays in memory until it is closed, and is saved
     * plain; opened again, it has a working copy as any plain catalog. */
    REQUIRE_OK(bd_catalog_set_file_encrypted(cat, 0), cat);
    REQUIRE_OK(bd_catalog_save(cat), cat);
    CHECK(!bd_catalog_file_needs_passphrase(at("c.brodalf")));
    CHECK(files_in(tmp) == 0);
    bd_catalog_close(cat);
    REQUIRE_OK(bd_catalog_open(at("c.brodalf"), &cat), NULL);
    CHECK(!cat->in_memory && files_in(tmp) >= 1);
    CHECK(bd_setting_get(cat, "test.reopen.0", v, sizeof(v)));
    bd_catalog_close(cat);
    CHECK(files_in(tmp) == 0);
}

#ifndef _WIN32
/* A BRODALF that ends without closing its encrypted catalog (a crash, a
 * power cut) leaves nothing readable: there was nothing on the disk. */
static void test_crash(void)
{
    begin("crash");
    write_file(at("src/" SECRET_NAME), "ordinary text");
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("c.brodalf"), &cat), NULL);
    int64_t src;
    REQUIRE_OK(bd_source_add(cat, at("src"), &src), cat);
    REQUIRE_OK(bd_catalog_set_passphrase(cat, PASS), cat);
    REQUIRE_OK(bd_catalog_set_file_encrypted(cat, 1), cat);
    REQUIRE_OK(bd_catalog_save(cat), cat);
    bd_catalog_close(cat);

    fflush(NULL);
    pid_t child = fork();
    if (child == 0) {
        bd_catalog *c;
        if (bd_catalog_open_with(at("c.brodalf"), PASS, &c) != BD_OK) _exit(2);
        bd_scan_stats ss;
        if (bd_scan(c, &ss, quiet, NULL) != BD_OK) _exit(3);
        grow(c, "crash");
        if (bd_catalog_save(c) != BD_OK) _exit(4);
        if (bd_setting_set(c, "test.after-save", "lost") != 0) _exit(5);
        _exit(files_in(tmp) == 0 ? 0 : 6); /* ends here, the catalog still open */
    }
    int status = 0;
    waitpid(child, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK_NOTHING_READABLE();
    /* What was saved is there; what came after the save is not. */
    remove(at("c.brodalf.lock")); /* left by the crash, as on any system but Windows */
    REQUIRE_OK(bd_catalog_open_with(at("c.brodalf"), PASS, &cat), NULL);
    char v[32];
    CHECK(count_rows(cat, SECRET_ROWS) == 1);
    CHECK(bd_setting_get(cat, "test.crash.1999", v, sizeof(v)));
    CHECK(!bd_setting_get(cat, "test.after-save", v, sizeof(v)));
    bd_catalog_close(cat);
}
#endif

/* Encrypted files written the way 0.10.1 wrote them (from a working copy
 * in WAL mode, VACUUM INTO) open into memory, and 0.10.1's way of opening
 * reads the files saved from memory. */
static void test_old_files(void)
{
    begin("old");
    write_file(at("src/" SECRET_NAME), "ordinary text");
    bd_test_work_on_disk = 1;
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("c.brodalf"), &cat), NULL);
    int64_t src;
    REQUIRE_OK(bd_source_add(cat, at("src"), &src), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_catalog_set_passphrase(cat, PASS), cat);
    REQUIRE_OK(bd_catalog_set_file_encrypted(cat, 1), cat);
    CHECK(!cat->in_memory && files_in(tmp) >= 1);
    REQUIRE_OK(bd_setting_set(cat, "test.written-by", "old") == 0 ? BD_OK : BD_ERR_DB, cat);
    REQUIRE_OK(bd_catalog_save(cat), cat);
    bd_catalog_close(cat);
    bd_test_work_on_disk = 0;
    CHECK(files_in(tmp) == 0);

    count_opens_from_now();
    REQUIRE_OK(bd_catalog_open_with(at("c.brodalf"), PASS, &cat), NULL);
    CHECK(cat->in_memory);
    char v[32] = "";
    CHECK(bd_setting_get(cat, "test.written-by", v, sizeof(v)) && strcmp(v, "old") == 0);
    CHECK(count_rows(cat, SECRET_ROWS) == 1);
    CHECK(count_rows(cat, "SELECT count(*) FROM pragma_integrity_check WHERE integrity_check='ok'") == 1);
    REQUIRE_OK(bd_setting_set(cat, "test.written-by", "new") == 0 ? BD_OK : BD_ERR_DB, cat);
    REQUIRE_OK(bd_catalog_save(cat), cat);
    bd_catalog_close(cat);
    CHECK_NO_OPENS();
    CHECK_NOTHING_READABLE();

    bd_test_work_on_disk = 1;
    REQUIRE_OK(bd_catalog_open_with(at("c.brodalf"), PASS, &cat), NULL);
    CHECK(!cat->in_memory);
    CHECK(bd_setting_get(cat, "test.written-by", v, sizeof(v)) && strcmp(v, "new") == 0);
    CHECK(count_rows(cat, SECRET_ROWS) == 1);
    CHECK(count_rows(cat, "SELECT count(*) FROM pragma_integrity_check WHERE integrity_check='ok'") == 1);
    bd_catalog_close(cat);
    bd_test_work_on_disk = 0;
}

/* Saves taken while another thread writes: each one is a whole database. */
typedef struct { bd_catalog *cat; sqlite3_mutex *lock; int stop; } writer_ctx;

static int stopped(writer_ctx *w)
{
    sqlite3_mutex_enter(w->lock);
    int stop = w->stop;
    sqlite3_mutex_leave(w->lock);
    return stop;
}

static void writer(void *arg)
{
    writer_ctx *w = arg;
    char key[64], value[1100];
    memset(value, 'w', 1024);
    value[1024] = '\0';
    for (int round = 0; !stopped(w) && round < 400; round++) {
        bd_exec(w->cat, "BEGIN");
        for (int i = 0; i < 400; i++) {
            snprintf(key, sizeof(key), "test.thread.%d.%d", round % 3, i);
            bd_setting_set(w->cat, key, value);
        }
        bd_exec(w->cat, "COMMIT");
        bd_sleep_ms(1);
    }
}

static void test_threads(void)
{
    begin("threads");
    write_file(at("src/" SECRET_NAME), "ordinary text");
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("c.brodalf"), &cat), NULL);
    int64_t src;
    REQUIRE_OK(bd_source_add(cat, at("src"), &src), cat);
    REQUIRE_OK(bd_catalog_set_passphrase(cat, PASS), cat);
    REQUIRE_OK(bd_catalog_set_file_encrypted(cat, 1), cat);
    writer_ctx w = {cat, sqlite3_mutex_alloc(SQLITE_MUTEX_FAST), 0};
    bd_thread *t = bd_thread_start(writer, &w);
    CHECK(t != NULL);
    int saved = 0;
    for (int tries = 0; saved < 6 && tries < 5000; tries++) {
        char *dest = bd_sprintf("%s/snap-%d.brodalf", base, saved);
        if (bd_catalog_save_to(cat, dest, 1) == BD_OK) saved++;
        else bd_sleep_ms(1);
        free(dest);
    }
    sqlite3_mutex_enter(w.lock);
    w.stop = 1;
    sqlite3_mutex_leave(w.lock);
    bd_thread_join(t);
    sqlite3_mutex_free(w.lock);
    CHECK(saved == 6);
    bd_catalog_close(cat);
    for (int i = 0; i < saved; i++) {
        char *p = bd_sprintf("%s/snap-%d.brodalf", base, i);
        bd_catalog *c;
        REQUIRE_OK(bd_catalog_open_with(p, PASS, &c), NULL);
        CHECK(count_rows(c, "SELECT count(*) FROM pragma_integrity_check WHERE integrity_check='ok'") == 1);
        /* No half of a writer's transaction: whole rounds of 400 only. */
        CHECK(count_rows(c, "SELECT count(*) FROM settings WHERE key LIKE 'test.thread.%'") % 400 == 0);
        bd_catalog_close(c);
        free(p);
    }
    CHECK(files_in(tmp) == 0);
}

int main(void)
{
    /* SQLite's in-memory databases stop at 256 KiB here, unless the
     * catalog lifts the limit, as it must. */
    sqlite3_config(SQLITE_CONFIG_MEMDB_MAXSIZE, (sqlite3_int64)256 * 1024);
    sqlite3_initialize();
    sqlite3_vfs *vfs = sqlite3_vfs_find(NULL);
    os_open = vfs->xOpen;
    vfs->xOpen = counting_open;

    char sys_tmp[512], id[37];
    bd_temp_dir(sys_tmp, sizeof(sys_tmp));
    bd_uuid_v4(id);
    snprintf(top, sizeof(top), "%s/brodalf-memory-%.8s", sys_tmp, id);
    for (char *p = top; *p; p++) if (*p == '\\') *p = '/';
    snprintf(tmp, sizeof(tmp), "%s/tmp", top);
    bd_mkdirs(tmp);
    printf("working in %s\n", top);
    /* The catalogs' temp folder: its own, to see everything put there. */
#ifdef _WIN32
    char native[1100];
    snprintf(native, sizeof(native), "%s", tmp);
    for (char *p = native; *p; p++) if (*p == '/') *p = '\\';
    SetEnvironmentVariableA("TMP", native);
    SetEnvironmentVariableA("TEMP", native);
#else
    setenv("TMPDIR", tmp, 1);
#endif
    char check[1100];
    CHECK(bd_temp_dir(check, sizeof(check)) == 0);

    test_switch_on();
    test_reopen();
#ifndef _WIN32
    test_crash();
#endif
    test_old_files();
    test_threads();

    remove_tree(top);
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("all memory checks passed");
    return 0;
}
