/* The 0.14.0 flow review's testing gaps: code that until now only ran on
 * a person's PC, run for real on Windows CI (an administrator there).
 *   - The disk's own description (bd_drive_hw_read) of the system disk and
 *     of the disk the tests run on, read from Windows, and the same as a
 *     backup drive on that disk gets when it is set up.
 *   - Copies of a file another program holds open with no sharing (as
 *     Outlook holds its .pst), taken out of a Volume Shadow Copy: directly
 *     (bd_shadow_copy_files), through the elevated helper's checks
 *     (bd_shadow_helper), and the command-line tool's way
 *     (bd_shadow_stage / bd_shadow_unstage), then scanned, backed up and
 *     restored.
 * A shadow copy needs administrator rights: without them the shadow part
 * is skipped, saying so, except on CI (the CI variable set), where it
 * fails. Elsewhere than Windows only what these functions answer there is
 * checked. Everything is made in one temp folder, removed at the end.
 * "test_flow_tests disk" or "test_flow_tests shadow" runs one of the two
 * parts (ctest's flow_tests and flow_shadow); with neither, both run. */
#include "brodalf.h"
#include "internal.h"
#include "test_tree.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sddl.h>
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

#ifdef _WIN32
static int file_equals(const char *path, const char *text)
{
    FILE *f = bd_fopen(path, "rb");
    if (!f) return 0;
    char buf[512] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    return n == strlen(text) && memcmp(buf, text, n) == 0;
}

static int on_ci(void)
{
    const char *ci = getenv("CI");
    return ci && *ci;
}

/* A check that cannot run here: skipped on a person's PC, saying so; a
 * failure on CI, where every check must run. */
static void not_tried(const char *what, const char *why)
{
    if (on_ci()) {
        fprintf(stderr, "%s: not tried on CI, %s\n", what, why);
        failures++;
    } else {
        printf("%s: not tried, %s\n", what, why);
    }
}

/* The path with '\' separators, as Windows writes it. */
static char *native(const char *path)
{
    char *s = bd_strdup(path);
    for (char *p = s; p && *p; p++) if (*p == '/') *p = '\\';
    return s;
}
#endif

/* ---- The disk's own description ----------------------------------------- */

static void print_hw(const char *root, int rc, const bd_drive_hw *hw)
{
    printf("  %s: rc %d, vendor \"%s\", model \"%s\", bus \"%s\", firmware \"%s\", disk %lld bytes, file system \"%s\","
           " volume \"%s\" %s, SMART %d, health \"%s\", %d C, %lld hours on, note \"%s\"\n",
           root, rc, hw->vendor, hw->model, hw->bus, hw->firmware, (long long)hw->disk_bytes, hw->filesystem,
           hw->volume_name, hw->volume_serial, hw->smart, hw->health, hw->temperature_c, (long long)hw->power_on_hours,
           hw->note);
}

#ifdef _WIN32
typedef struct { int64_t id; int found, uncounted; char model[128], bus[32]; } media_hw;

static int media_hw_cb(void *ctx, const bd_media_info *m)
{
    media_hw *w = ctx;
    if (m->media_id != w->id) return 0;
    w->found = 1;
    w->uncounted = m->uncounted;
    if (m->hw) {
        snprintf(w->model, sizeof(w->model), "%s", m->hw->model);
        snprintf(w->bus, sizeof(w->bus), "%s", m->hw->bus);
    }
    return 1;
}
#endif

static void test_drive_hw(void)
{
    begin("hw");
    bd_drive_hw hw;
#ifdef _WIN32
    /* The system disk: Windows always says what it is, if not its SMART
     * data (a virtual machine's disk has none to give). */
    char sys[8] = "C:\\";
    DWORD n = GetEnvironmentVariableA("SystemDrive", sys, sizeof(sys) - 1);
    if (n == 2) { sys[2] = '\\'; sys[3] = '\0'; }
    int rc = bd_drive_hw_read(sys, &hw);
    print_hw(sys, rc, &hw);
    CHECK(rc == 0);
    CHECK(hw.model[0] || hw.bus[0]);
    CHECK(strcmp(hw.filesystem, "NTFS") == 0);
    CHECK(hw.volume_serial[0] != '\0');
    CHECK(hw.disk_bytes > 0);

    /* The disk the tests run on, by a folder on it. */
    rc = bd_drive_hw_read(base, &hw);
    print_hw(base, rc, &hw);
    CHECK(rc == 0 && (hw.model[0] || hw.bus[0]) && hw.filesystem[0]);
    bd_drive_hw here = hw;

    /* A backup drive set up in a folder on that disk gets the same
     * description, and, the disk telling its serial number, is known to be
     * the same disk as the backup root beside it. */
    write_file(at("src/a.txt"), "a");
    bd_mkdirs(at("drive"));
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("c.brodalf"), &cat), NULL);
    int64_t src, drive;
    REQUIRE_OK(bd_source_add(cat, at("src"), &src), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_media_init(cat, at("drive"), "Same disk", 0, &drive), cat);
    media_hw w;
    memset(&w, 0, sizeof(w));
    w.id = drive;
    bd_list_media(cat, media_hw_cb, &w);
    CHECK(w.found && strcmp(w.model, here.model) == 0 && strcmp(w.bus, here.bus) == 0);
    if (here.serial[0]) CHECK(w.uncounted == 1);
    else printf("  this disk gives no serial number: the same-disk check is not made\n");
    bd_catalog_close(cat);
#else
    /* Elsewhere: a description only where the system gives one. */
    int rc = bd_drive_hw_read(base, &hw);
    print_hw(base, rc, &hw);
    CHECK(rc == -1 || hw.model[0] || hw.serial[0]);
    CHECK(bd_drive_hw_read("/no/such/folder", &hw) == -1);
#endif
}

/* ---- Files in use, out of a shadow copy ---------------------------------- */

#define SNAPSHOT_TEXT "the mailbox, as the shadow copy saw it"

#ifdef _WIN32
typedef struct { int n; char path[2048]; } in_use_list;

static int in_use_cb(void *ctx, const char *path, int64_t size)
{
    in_use_list *l = ctx;
    (void)size;
    if (l->n++ == 0) snprintf(l->path, sizeof(l->path), "%s", path);
    return 0;
}

static int elevated(void)
{
    HANDLE t = NULL;
    TOKEN_ELEVATION e;
    DWORD n = 0;
    int yes = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t) &&
              GetTokenInformation(t, TokenElevation, &e, sizeof(e), &n) && e.TokenIsElevated;
    if (t) CloseHandle(t);
    return yes;
}

/* This account's SID as text ("S-1-5-21-..."), as the app hands it to the
 * helper. */
static char *my_sid(void)
{
    HANDLE t = NULL;
    unsigned char buf[256];
    DWORD n = 0;
    char *out = NULL, *s = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t) && GetTokenInformation(t, TokenUser, buf, sizeof(buf), &n) &&
        ConvertSidToStringSidA(((TOKEN_USER *)buf)->User.Sid, &s)) {
        out = bd_strdup(s);
        LocalFree(s);
    }
    if (t) CloseHandle(t);
    return out;
}

/* Held open as Outlook holds its mailbox: nobody else may even read it. */
static HANDLE hold_alone(const char *path)
{
    wchar_t w[2048];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 2048);
    for (wchar_t *c = w; *c; c++) if (*c == L'/') *c = L'\\';
    return CreateFileW(w, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
}
#endif

static void test_shadow_copy(void)
{
    begin("shadow");
#ifndef _WIN32
    /* Not Windows: no shadow copies, said so, and nothing in use to stage. */
    const char *one[1] = {"/tmp/x"};
    int ok[1] = {1};
    char err[256] = "";
    CHECK(bd_shadow_copy_files(one, 1, at("copies"), ok, err, sizeof(err)) == -1 && ok[0] == 0 && err[0]);
    write_file(at("src/a.txt"), "a");
    bd_catalog *c;
    REQUIRE_OK(bd_catalog_create(at("c.brodalf"), &c), NULL);
    int64_t s;
    REQUIRE_OK(bd_source_add(c, at("src"), &s), c);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(c, &ss, quiet, NULL), c);
    CHECK(ss.files_in_use == 0);
    char *dir = NULL;
    CHECK(bd_shadow_stage(c, &dir) == 0 && dir == NULL);
    bd_catalog_close(c);
#else
    if (!elevated()) {
        not_tried("shadow copies", "this needs to run as administrator");
        return;
    }
    write_file(at("src/Mail/mail.pst"), SNAPSHOT_TEXT);
    write_file(at("src/note.txt"), "a note");
    write_file(at("elsewhere/secret.txt"), "not inside the backup root");
    bd_mkdirs(at("drive"));
    HANDLE held = hold_alone(at("src/Mail/mail.pst"));
    CHECK(held != INVALID_HANDLE_VALUE);
    FILE *f = bd_fopen(at("src/Mail/mail.pst"), "rb");
    CHECK(f == NULL); /* it really cannot be read now */
    if (f) fclose(f);

    /* Straight out of a shadow copy. */
    char *mail = native(at("src/Mail/mail.pst"));
    const char *paths[1] = {mail};
    int ok[1] = {0};
    char err[512] = "";
    int got = bd_shadow_copy_files(paths, 1, at("direct"), ok, err, sizeof(err));
    if (got != 1) fprintf(stderr, "  bd_shadow_copy_files: %d, %s\n", got, err);
    CHECK(got == 1 && ok[0] == 1);
    CHECK(file_equals(at("direct/1.bin"), SNAPSHOT_TEXT));

    /* The elevated helper's way: the list checked against its hash, a file
     * outside the backup roots refused, the copies in a folder of its own
     * that this account may read. */
    char *sid = my_sid(), hash[65];
    CHECK(sid != NULL);
    char *root = native(at("src")), *outside = native(at("elsewhere/secret.txt"));
    const char *folders[1] = {root};
    const char *files[2] = {mail, outside};
    char *list = sid ? bd_shadow_list_format(sid, folders, 1, files, 2, hash) : NULL;
    CHECK(list != NULL);
    if (list) {
        write_file(at("list.txt"), list);
        err[0] = '\0';
        got = bd_shadow_helper(at("helper"), at("list.txt"), hash, err, sizeof(err));
        if (got != 1) fprintf(stderr, "  bd_shadow_helper: %d, %s\n", got, err);
        CHECK(got == 1);
        CHECK(file_equals(at("helper/1.bin"), SNAPSHOT_TEXT));
        CHECK(!exists(at("helper/2.bin")));
        CHECK(file_equals(at("helper/result.txt"), "ok\nrefused\n"));
        /* A list changed after its hash was taken: nothing is copied. */
        write_file(at("list.txt"), "BRODALF shadow-copy list 1\nchanged\n");
        CHECK(bd_shadow_helper(at("helper2"), at("list.txt"), hash, err, sizeof(err)) == -1);
        CHECK(!exists(at("helper2/1.bin")));
    }
    free(list);
    free(sid);
    free(root);
    free(outside);

    /* The command-line tool's way: a scan meets the file in use, the copy
     * from a shadow copy stands in for it in a scan, a backup and so in a
     * restore, and then goes. */
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("c.brodalf"), &cat), NULL);
    int64_t src, drive;
    REQUIRE_OK(bd_source_add(cat, at("src"), &src), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_in_use == 1 && ss.files_new == 1);
    in_use_list iu;
    memset(&iu, 0, sizeof(iu));
    REQUIRE_OK(bd_list_in_use(cat, in_use_cb, &iu), cat);
    CHECK(iu.n == 1 && strstr(iu.path, "mail.pst"));
    char *dir = NULL;
    got = bd_shadow_stage(cat, &dir);
    if (got != 1) fprintf(stderr, "  bd_shadow_stage: %d, %s\n", got, bd_catalog_error(cat));
    CHECK(got == 1 && dir != NULL);
    if (dir) {
        char *staged = bd_sprintf("%s/1.bin", dir);
        CHECK(file_equals(staged, SNAPSHOT_TEXT));
        free(staged);
    }
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_in_use == 0 && ss.files_new == 1 && ss.errors == 0);
    REQUIRE_OK(bd_media_init(cat, at("drive"), "Drive", 0, &drive), cat);
    bd_backup_stats bs;
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 2 && bs.files_failed == 0 && bs.files_in_use == 0);
    bd_shadow_unstage(cat, dir);
    CHECK(dir && !exists(dir));
    free(dir);
    bd_restore_stats rs;
    REQUIRE_OK(bd_restore(cat, src, "", at("restored"), &rs, quiet, NULL), cat);
    CHECK(rs.files_failed == 0 && rs.files_restored == 2);
    CHECK(file_equals(at("restored/Mail/mail.pst"), SNAPSHOT_TEXT));
    /* Still held, and unchanged since it was read from the shadow copy: a
     * scan has no need to read it. A bit-rot scan reads every file, and
     * without the copy it is in use again. */
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_in_use == 0 && ss.errors == 0);
    bd_scan_opts so;
    memset(&so, 0, sizeof(so));
    so.bitrot = 1;
    REQUIRE_OK(bd_scan_ex(cat, &so, &ss, quiet, NULL), cat);
    CHECK(ss.files_in_use == 1);
    bd_catalog_close(cat);
    if (held != INVALID_HANDLE_VALUE) CloseHandle(held);
    free(mail);
#endif
}

int main(int argc, char **argv)
{
    const char *only = argc > 1 ? argv[1] : "";
    if (*only && strcmp(only, "disk") != 0 && strcmp(only, "shadow") != 0) {
        fprintf(stderr, "usage: test_flow_tests [disk|shadow]\n");
        return 2;
    }
    char tmp[512], id[37];
    bd_temp_dir(tmp, sizeof(tmp));
    bd_uuid_v4(id);
    setvbuf(stdout, NULL, _IONBF, 0); /* what it prints, in order with the failed checks */
    snprintf(top, sizeof(top), "%s/brodalf-flow-tests-%.8s", tmp, id);
    for (char *p = top; *p; p++) if (*p == '\\') *p = '/';
    bd_mkdirs(top);
    printf("working in %s\n", top);

    if (strcmp(only, "shadow") != 0) test_drive_hw();
    if (strcmp(only, "disk") != 0) test_shadow_copy();

    remove_tree(top);
    CHECK(!exists(top));
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("all flow-tests checks passed");
    return 0;
}
