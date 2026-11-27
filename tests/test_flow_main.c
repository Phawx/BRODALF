/* Flow review of 0.14.0, the coordinator's part: the recovery sheet and the
 * list of newest copies beside each database copy (D1: restoring by hand
 * from a plain drive), and which drive helps the files at risk (C8). */
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

#ifdef _WIN32
#define SEP "\\"
#else
#define SEP "/"
#endif

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

static char *read_all(const char *path)
{
    FILE *f = bd_fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 4096, n = 0;
    char *buf = malloc(cap);
    size_t got;
    while (buf && (got = fread(buf + n, 1, cap - n - 1, f)) > 0) {
        n += got;
        if (n + 1 == cap) {
            char *nb = realloc(buf, cap * 2);
            if (!nb) { free(buf); buf = NULL; break; }
            buf = nb;
            cap *= 2;
        }
    }
    fclose(f);
    if (buf) buf[n] = '\0';
    return buf;
}

static void quiet(void *ctx, const char *msg)
{
    (void)ctx;
    (void)msg;
}

/* The lists and database copies in a drive's BRODALF\<id> folder. */
typedef struct { int lists, copies; char newest[256]; } drive_files;

static int list_cb(void *ctx, const char *rel, const bd_stat_t *st)
{
    drive_files *d = ctx;
    if (!st->is_file || strchr(rel, '/')) return 0;
    if (bd_newest_list_name(rel)) {
        d->lists++;
        if (strcmp(rel, d->newest) > 0) snprintf(d->newest, sizeof(d->newest), "%s", rel);
    } else if (strncmp(rel, "catalog-r", 9) == 0) {
        d->copies++;
    }
    return 0;
}

static drive_files drive_dir_files(const char *dir)
{
    drive_files d;
    memset(&d, 0, sizeof(d));
    bd_walk(dir, list_cb, NULL, &d);
    return d;
}

static int has_line(const char *text, const char *line)
{
    size_t n = strlen(line);
    for (const char *p = text; (p = strstr(p, line)) != NULL; p++)
        if ((p == text || p[-1] == '\n') && (p[n] == '\r' || p[n] == '\n' || p[n] == '\0')) return 1;
    return 0;
}

/* The line for file in the list, or NULL. Returned in a static buffer. */
static const char *line_for(const char *text, const char *file)
{
    static char line[1024];
    size_t n = strlen(file);
    for (const char *p = text; (p = strstr(p, file)) != NULL; p++) {
        if ((p == text || p[-1] == '\n') && p[n] == '\t') {
            size_t k = strcspn(p, "\r\n");
            if (k >= sizeof(line)) k = sizeof(line) - 1;
            memcpy(line, p, k);
            line[k] = '\0';
            return line;
        }
    }
    return NULL;
}

static void test_newest_list(void)
{
    bd_catalog *cat;
    bd_mkdirs(at("list"));
    REQUIRE_OK(bd_catalog_create(at("list/db.brodalf"), &cat), NULL);
    write_file(at("list/Docs/a.txt"), "one");
    write_file(at("list/Docs/b.txt"), "bee");
    write_file(at("list/Docs/c.txt"), "sea");
    write_file(at("list/Docs/sub/d.txt"), "dee");
    int64_t src, drive;
    REQUIRE_OK(bd_source_add(cat, at("list/Docs"), &src), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    bd_mkdirs(at("list/drive"));
    REQUIRE_OK(bd_media_init(cat, at("list/drive"), "Blue", 0, &drive), cat);
    char dir[1200];
    snprintf(dir, sizeof(dir), "%s", at("list/drive/BRODALF"));
    snprintf(dir + strlen(dir), sizeof(dir) - strlen(dir), "/%s", bd_catalog_uuid(cat));
    /* Setting up the drive put a database copy on it, and a list beside it. */
    drive_files d = drive_dir_files(dir);
    CHECK(d.lists == 1 && d.copies == 1);

    bd_backup_stats bs;
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 4);
    d = drive_dir_files(dir);
    CHECK(d.lists == 2 && d.lists == d.copies);
    char first[1400];
    snprintf(first, sizeof(first), "%s/%s", dir, d.newest);
    char *before = read_all(first);
    CHECK(before && strstr(before, "every copy is in its own place") == NULL);
    CHECK(before && strstr(before, "Every copy is in its own place.") != NULL);

    /* A change, a rename and a delete; then a folder set to be encrypted. */
    write_file(at("list/Docs/a.txt"), "two, longer");
    char from[1200], to[1200];
    snprintf(from, sizeof(from), "%s", at("list/Docs/b.txt"));
    snprintf(to, sizeof(to), "%s", at("list/Docs/b2.txt"));
    CHECK(bd_rename_noreplace(from, to) == 0);
    bd_remove(at("list/Docs/c.txt"));
    write_file(at("list/Docs/secret/e.txt"), "sealed text");
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_catalog_set_passphrase(cat, "a passphrase for the test"), cat);
    REQUIRE_OK(bd_encrypt_path_set(cat, src, "secret", 1), cat);
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 2); /* a.txt's new version and e.txt; b2.txt keeps b.txt's copy */

    d = drive_dir_files(dir);
    CHECK(d.lists == 3 && d.lists == d.copies);
    char newest[1400];
    snprintf(newest, sizeof(newest), "%s/%s", dir, d.newest);
    char *text = read_all(newest);
    CHECK(text != NULL);
    if (text) {
        const char *l;
        l = line_for(text, "a.txt");
        CHECK(l && strncmp(l, "a.txt\t.versions" SEP "Docs" SEP "a.v2.txt\ta newer version", 40) == 0);
        l = line_for(text, "b2.txt");
        CHECK(l && strncmp(l, "b2.txt\tDocs" SEP "b.txt\tmoved or renamed", 31) == 0);
        l = line_for(text, "secret" SEP "e.txt");
        CHECK(l && strstr(l, "\tDocs" SEP "secret" SEP "e.txt.bdenc\tsealed") != NULL);
        l = line_for(text, "c.txt");
        CHECK(l && strstr(l, "\tDocs" SEP "c.txt\tdeleted, last seen ") != NULL);
        CHECK(line_for(text, "sub" SEP "d.txt") == NULL); /* in its own place: not listed */
        CHECK(strstr(text, "Deleted from the PC") != NULL);
        /* The deleted file comes after the others. */
        CHECK(strstr(text, "c.txt\t") > strstr(text, "secret" SEP "e.txt\t"));
        CHECK(strstr(text, "== Docs (from ") != NULL && strstr(text, "4 files, 4 with a copy on this drive ==") != NULL);
        CHECK(strstr(text, "Drive \"Blue\"") != NULL);
    }
    /* The list from before is still there as it was. */
    char *again = read_all(first);
    CHECK(before && again && strcmp(before, again) == 0);
    free(again);
    free(before);

    /* A file whose newest copy is not on this drive: an older one is named,
     * and a file with no copy here is counted. */
    write_file(at("list/Docs/sub/d.txt"), "dee, changed");
    write_file(at("list/Docs/new.txt"), "new");
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    int64_t red;
    bd_mkdirs(at("list/red"));
    REQUIRE_OK(bd_media_init(cat, at("list/red"), "Red", 0, &red), cat);
    char reddir[1200];
    snprintf(reddir, sizeof(reddir), "%s/%s", at("list/red/BRODALF"), bd_catalog_uuid(cat));
    REQUIRE_OK(bd_catalog_copy_to_media(cat, drive), cat);
    d = drive_dir_files(dir);
    snprintf(newest, sizeof(newest), "%s/%s", dir, d.newest);
    free(text);
    text = read_all(newest);
    CHECK(text != NULL);
    if (text) {
        const char *l = line_for(text, "sub" SEP "d.txt");
        CHECK(l && strstr(l, "\tDocs" SEP "sub" SEP "d.txt\tan older version") != NULL);
        CHECK(has_line(text, "1 file has no copy on this drive: look on your other drives."));
    }
    free(text);
    d = drive_dir_files(reddir);
    CHECK(d.lists == 1);

    /* With nothing changed, the copy and its list are already there, and
     * neither is written again. */
    d = drive_dir_files(dir);
    char *same = read_all(newest);
    REQUIRE_OK(bd_catalog_copy_to_media(cat, drive), cat);
    drive_files d2 = drive_dir_files(dir);
    CHECK(d2.copies == d.copies && d2.lists == d.lists);
    char *same2 = read_all(newest);
    CHECK(same && same2 && strcmp(same, same2) == 0);
    free(same);
    free(same2);

    /* An encrypted drive gets no list: only BRODALF can read it anyway. */
    int64_t enc;
    bd_mkdirs(at("list/enc"));
    REQUIRE_OK(bd_media_init(cat, at("list/enc"), "Locked", BD_MEDIA_ENCRYPTED, &enc), cat);
    REQUIRE_OK(bd_backup(cat, enc, 0, &bs, quiet, NULL), cat);
    char encdir[1200];
    snprintf(encdir, sizeof(encdir), "%s/%s", at("list/enc/BRODALF"), bd_catalog_uuid(cat));
    d = drive_dir_files(encdir);
    CHECK(d.lists == 0 && d.copies >= 1);

    /* A backup root with a list's name gets another name on the drives. */
    write_file(at("list/newest-copies-r00000001.txt/x.txt"), "x");
    int64_t odd;
    REQUIRE_OK(bd_source_add(cat, at("list/newest-copies-r00000001.txt"), &odd), cat);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    char oddpath[1400];
    snprintf(oddpath, sizeof(oddpath), "%s/_newest-copies-r00000001.txt/x.txt", dir);
    bd_stat_t st;
    CHECK(bd_stat(oddpath, &st) == 0);
    CHECK(bd_newest_list_name("newest-copies-r00000012.txt") && bd_newest_list_name("NEWEST-COPIES-R00000012-2.TXT") &&
          bd_newest_list_name("newest-copies-r00000012.txt" BD_TMP_MARKER) && !bd_newest_list_name("newest-copies-r.txt") &&
          !bd_newest_list_name("newest-copies-r12-.txt") && bd_newest_list_name("newest-copies-r12-0a1b2c3d.txt") && !bd_newest_list_name("newest-copies-rx.txt"));

    bd_catalog_close(cat);
}

static void test_sheet_text(void)
{
    bd_catalog *cat;
    bd_mkdirs(at("sheet"));
    REQUIRE_OK(bd_catalog_create(at("sheet/db.brodalf"), &cat), NULL);
    REQUIRE_OK(bd_write_recovery_sheet(cat, at("sheet/sheet.html"), NULL), cat);
    char *text = read_all(at("sheet/sheet.html"));
    CHECK(text != NULL);
    if (text) {
        /* 0.14.0 said the older versions were in .versions: it is the other way round. */
        CHECK(strstr(text, "older versions are in the .versions folder") == NULL);
        CHECK(strstr(text, "first copy made of each file") != NULL);
        CHECK(strstr(text, "Newer versions") != NULL && strstr(text, ".versions") != NULL);
        CHECK(strstr(text, "newest-copies-r") != NULL);
        CHECK(strstr(text, "moved or renamed") != NULL && strstr(text, "deleted") != NULL);
        CHECK(strstr(text, ".bdenc") != NULL);
    }
    free(text);
    bd_catalog_close(cat);
}

/* ---- C8: which drive helps the files at risk ----------------------------- */

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

typedef struct { int n; int64_t ids[16]; } help_list;

static int help_cb(void *ctx, const bd_risk_help *h)
{
    help_list *l = ctx;
    if (l->n < 16) l->ids[l->n++] = h->media_id;
    return 0;
}

static int helps(const help_list *l, int64_t id)
{
    for (int i = 0; i < l->n; i++) if (l->ids[i] == id) return 1;
    return 0;
}

static void test_risk_help(void)
{
    for (int i = 0; i < 3; i++) {
        char rel[64], text[64];
        snprintf(rel, sizeof(rel), "help/disk-ROOT/Docs/f%d.txt", i);
        snprintf(text, sizeof(text), "doc %d", i);
        write_file(at(rel), text);
    }
    bd_mkdirs(at("help/disk-SAME-p1"));
    bd_mkdirs(at("help/disk-SAME-p2"));
    bd_mkdirs(at("help/disk-OTHER"));
    bd_mkdirs(at("help/disk-ROOT-backup"));
    bd_mkdirs(at("help/disk-FAIL"));
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("help/db.brodalf"), &cat), NULL);
    bd_target t;
    memset(&t, 0, sizeof(t));
    t.copies = 2;
    t.places = 1;
    REQUIRE_OK(bd_target_set(cat, &t), cat);
    int64_t src, p1, p2, other, own, fail;
    REQUIRE_OK(bd_source_add(cat, at("help/disk-ROOT/Docs"), &src), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_media_init(cat, at("help/disk-SAME-p1"), "Blue 1", 0, &p1), cat);
    REQUIRE_OK(bd_media_init(cat, at("help/disk-SAME-p2"), "Blue 2", 0, &p2), cat);
    REQUIRE_OK(bd_media_init(cat, at("help/disk-ROOT-backup"), "Spare partition", 0, &own), cat);
    REQUIRE_OK(bd_media_init(cat, at("help/disk-FAIL"), "Old", 0, &fail), cat);
    REQUIRE_OK(bd_media_init(cat, at("help/disk-OTHER"), "Red", 0, &other), cat);
    bd_backup_stats bs;
    REQUIRE_OK(bd_backup(cat, p1, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 3);

    /* Only Red would add a copy that counts: Blue 2 is the same disk as
     * Blue 1, the spare partition is the files' own disk, and Old is
     * failing. 0.14.0 suggested all four. */
    help_list l;
    memset(&l, 0, sizeof(l));
    REQUIRE_OK(bd_list_risk_help(cat, help_cb, &l), cat);
    CHECK(helps(&l, other));
    CHECK(!helps(&l, p2));
    CHECK(!helps(&l, own));
    CHECK(!helps(&l, fail));
    CHECK(!helps(&l, p1));
    bd_risk_help best;
    REQUIRE_OK(bd_suggest_drive(cat, &best), cat);
    CHECK(best.media_id == other);

    /* Once Red has them, nothing is short and no drive is suggested. */
    REQUIRE_OK(bd_backup(cat, other, 0, &bs, quiet, NULL), cat);
    memset(&l, 0, sizeof(l));
    REQUIRE_OK(bd_list_risk_help(cat, help_cb, &l), cat);
    CHECK(l.n == 0);
    CHECK(bd_suggest_drive(cat, &best) == BD_ERR_NOT_FOUND);
    bd_catalog_close(cat);
}

int main(void)
{
    char tmp[512], id[37];
    bd_temp_dir(tmp, sizeof(tmp));
    bd_uuid_v4(id);
    snprintf(base, sizeof(base), "%s/brodalf-flow-main-%.8s", tmp, id);
    for (char *p = base; *p; p++) if (*p == '\\') *p = '/';
    bd_mkdirs(base);
    printf("working in %s\n", base);

    bd_test_hw_read = fake_hw;

    test_newest_list();
    test_sheet_text();
    test_risk_help();

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("all flow review checks passed\n");
    return 0;
}
