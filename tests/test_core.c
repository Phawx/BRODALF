/* End-to-end test of the core: scan, back up, go offline, change a file,
 * keep the old version, detect a missing copy, restore. */
#include "brodalf.h"
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
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

static int file_equals(const char *path, const char *text)
{
    FILE *f = bd_fopen(path, "rb");
    if (!f) return 0;
    char buf[256] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    return n == strlen(text) && memcmp(buf, text, n) == 0;
}

static int exists(const char *path)
{
    bd_stat_t st;
    return bd_stat(path, &st) == 0;
}

typedef struct {
    const char *name;
    bd_node_state state;
    int version_no;
    char label[128];
    int64_t node_id;
    int found;
} find_ctx;

static int find_child(void *ctx, const bd_node_info *n)
{
    find_ctx *f = ctx;
    if (strcmp(n->name, f->name) != 0) return 0;
    f->state = n->state;
    f->version_no = n->version_no;
    f->node_id = n->node_id;
    snprintf(f->label, sizeof(f->label), "%s", n->offline_media_label ? n->offline_media_label : "");
    f->found = 1;
    return 1;
}

static find_ctx lookup(bd_catalog *cat, int64_t source, int64_t parent, const char *name)
{
    find_ctx f;
    memset(&f, 0, sizeof(f));
    f.name = name;
    bd_list_children(cat, source, parent, find_child, &f);
    return f;
}

static int count_copies(void *ctx, const bd_copy_info *c)
{
    if (c->media_id && strcmp(c->copy_state, "ok") == 0) (*(int *)ctx)++;
    return 0;
}

typedef struct { bd_node_state state; int64_t total, available; char label[128]; } source_totals;

static int get_totals(void *ctx, const bd_source_info *info)
{
    source_totals *t = ctx;
    t->state = info->state;
    t->total = info->files_total;
    t->available = info->files_available;
    snprintf(t->label, sizeof(t->label), "%s", info->offline_media_label ? info->offline_media_label : "");
    return 1;
}

static source_totals totals(bd_catalog *cat)
{
    source_totals t;
    memset(&t, 0, sizeof(t));
    bd_list_sources(cat, get_totals, &t);
    return t;
}

static void quiet(void *ctx, const char *msg)
{
    (void)ctx;
    (void)msg;
}

typedef struct {
    char location[128];
    int has_hw;
    bd_drive_hw hw;
    int64_t want;
} media_snapshot_t;

static int snap_media(void *ctx, const bd_media_info *m)
{
    media_snapshot_t *s = ctx;
    if (m->media_id != s->want) return 0;
    snprintf(s->location, sizeof(s->location), "%s", m->location ? m->location : "(null)");
    s->has_hw = m->hw != NULL;
    if (m->hw) s->hw = *m->hw;
    return 1;
}

static media_snapshot_t media_of(bd_catalog *cat, int64_t id)
{
    media_snapshot_t s;
    memset(&s, 0, sizeof(s));
    s.want = id;
    bd_list_media(cat, snap_media, &s);
    return s;
}

static void count_log(void *ctx, const char *msg)
{
    if (strstr(msg, "different disk")) (*(int *)ctx)++;
}

static void set_env(const char *name, const char *value)
{
#ifdef _WIN32
    _putenv_s(name, value);
    wchar_t wn[64], wv[256];
    MultiByteToWideChar(CP_UTF8, 0, name, -1, wn, 64);
    MultiByteToWideChar(CP_UTF8, 0, value, -1, wv, 256);
    SetEnvironmentVariableW(wn, wv);
#else
    setenv(name, value, 1);
#endif
}

/* App log, redaction and the prefilled issue link. */
static void test_report(void)
{
#ifdef _WIN32
    set_env("USERNAME", "Zqxwolf");
#else
    set_env("USER", "Zqxwolf");
#endif
    char *r = bd_redact("C:\\Users\\zqxwolf\\Photos: signed in as jane.doe@example.com, Authorization: Bearer abc.DEF-123 ok");
    CHECK(r && strstr(r, "C:\\Users\\<user>\\Photos") && strstr(r, "signed in as <email>,") && strstr(r, "Bearer <hidden> ok"));
    CHECK(r && !strstr(r, "zqxwolf") && !strstr(r, "jane") && !strstr(r, "abc.DEF"));
    free(r);

    char log[2048];
    snprintf(log, sizeof(log), "%s", at("logs/brodalf.log"));
    REQUIRE_OK(bd_applog_open(log), NULL);
    CHECK(exists(log));
    bd_applog("first line from /home/Zqxwolf/x");
    for (int i = 0; i < 5000; i++) bd_applog("line %d", i);
    char *body = bd_report_body("Backup failed: disk full");
    CHECK(body && strstr(body, "Backup failed: disk full") && strstr(body, "Recent log") && strstr(body, "line 4999\n"));
    CHECK(body && strstr(body, bd_version()) && !strstr(body, "Zqxwolf"));
    free(body);

    char *saved = bd_report_save("Backup failed: disk full");
    CHECK(saved != NULL);
    if (saved) {
        FILE *f = fopen(saved, "rb");
        char text[4096] = "";
        if (f) { text[fread(text, 1, sizeof(text) - 1, f)] = '\0'; fclose(f); }
        CHECK(strstr(text, "https://github.com/Phawx/BRODALF/issues") && strstr(text, "Backup failed: disk full"));
        CHECK(strstr(saved, "reports") && strstr(saved, "brodalf-error-"));
        free(saved);
    }

    /* A long log moves to .old once it passes 1 MB. */
    char big[1001];
    memset(big, 'x', 1000);
    big[1000] = '\0';
    for (int i = 0; i < 1100; i++) bd_applog("%s", big);
    char old[2100];
    snprintf(old, sizeof(old), "%s.old", log);
    CHECK(exists(old));
    bd_applog_open(NULL);
    CHECK(bd_applog_path() == NULL);
}

int main(void)
{
    char tmp[512], id[37];
    bd_temp_dir(tmp, sizeof(tmp));
    bd_uuid_v4(id);
    snprintf(base, sizeof(base), "%s/brodalf-test-%.8s", tmp, id);
    for (char *p = base; *p; p++) if (*p == '\\') *p = '/';
    bd_mkdirs(base);
    printf("working in %s\n", base);

    write_file(at("src/Photos/a.txt"), "hello");
    write_file(at("src/Photos/sub/b.txt"), "world");
    bd_mkdirs(at("drive"));

    /* New catalog, one source, first scan. */
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("test.brodalf"), &cat), NULL);
    int64_t src;
    REQUIRE_OK(bd_source_add(cat, at("src/Photos"), &src), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_new == 2);
    CHECK(ss.dirs_seen == 1);

    /* Everything starts greyed out. */
    CHECK(lookup(cat, src, 0, "a.txt").state == BD_STATE_NO_COPY);
    CHECK(lookup(cat, src, 0, "sub").state == BD_STATE_NO_COPY);
    CHECK(totals(cat).state == BD_STATE_NO_COPY && totals(cat).total == 2);

    /* A second window cannot open the same catalog. */
    bd_catalog *other = NULL;
    CHECK(bd_catalog_open(at("test.brodalf"), &other) == BD_ERR_LOCKED);

    /* Set up a drive and back up. */
    int64_t drive;
    REQUIRE_OK(bd_media_init(cat, at("drive"), "Test Drive", 0, &drive), cat);
    CHECK(bd_media_init(cat, at("drive"), "Again", 0, NULL) == BD_ERR_EXISTS);
    bd_backup_stats bs;
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 2);
    CHECK(bs.files_failed == 0);
    char drive_dir[1024];
    snprintf(drive_dir, sizeof(drive_dir), "drive/BRODALF/%s", bd_catalog_uuid(cat));
    char p1[1200];
    snprintf(p1, sizeof(p1), "%s/Photos/a.txt", drive_dir);
    CHECK(file_equals(at(p1), "hello"));
    CHECK(lookup(cat, src, 0, "a.txt").state == BD_STATE_AVAILABLE);
    CHECK(lookup(cat, src, 0, "sub").state == BD_STATE_AVAILABLE);

    /* Unplug: files grey out and name the drive to plug in. */
    bd_media_disconnect(cat, drive);
    find_ctx a = lookup(cat, src, 0, "a.txt");
    CHECK(a.state == BD_STATE_OFFLINE);
    CHECK(strcmp(a.label, "Test Drive") == 0);
    CHECK(lookup(cat, src, 0, "sub").state == BD_STATE_OFFLINE);
    CHECK(totals(cat).state == BD_STATE_OFFLINE && strcmp(totals(cat).label, "Test Drive") == 0);

    /* The catalog survives a save and reopen. */
    REQUIRE_OK(bd_catalog_save(cat), cat);
    bd_catalog_close(cat);
    REQUIRE_OK(bd_catalog_open(at("test.brodalf"), &cat), NULL);
    bd_check_stats cs;
    REQUIRE_OK(bd_media_connect(cat, at("drive"), &drive, &cs, quiet, NULL), cat);
    CHECK(cs.copies == 2 && cs.ok == 2);
    CHECK(lookup(cat, src, 0, "a.txt").state == BD_STATE_AVAILABLE);

    /* Change a file: only the old version is reachable until the next backup. */
    write_file(at("src/Photos/a.txt"), "hello, version two");
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_changed == 1);
    a = lookup(cat, src, 0, "a.txt");
    CHECK(a.state == BD_STATE_AVAILABLE_OLDER);
    CHECK(a.version_no == 2);
    CHECK(totals(cat).state == BD_STATE_PARTIAL && totals(cat).available == 1);
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 1);
    CHECK(bs.versions_moved == 1);
    CHECK(file_equals(at(p1), "hello, version two"));
    char pv[1200];
    snprintf(pv, sizeof(pv), "%s/.versions/Photos/a.v1.txt", drive_dir);
    CHECK(file_equals(at(pv), "hello"));
    int copies = 0;
    bd_list_copies(cat, a.node_id, count_copies, &copies);
    CHECK(copies == 2);

    /* Lose a copy on the drive: the quick check notices it. */
    char pb[1200];
    snprintf(pb, sizeof(pb), "%s/Photos/sub/b.txt", drive_dir);
    bd_remove(at(pb));
    REQUIRE_OK(bd_media_check(cat, drive, 0, &cs, quiet, NULL), cat);
    CHECK(cs.missing == 1);
    find_ctx sub = lookup(cat, src, 0, "sub");
    CHECK(sub.state == BD_STATE_NO_COPY || sub.state == BD_STATE_PARTIAL || sub.state == BD_STATE_OFFLINE);
    CHECK(lookup(cat, src, sub.node_id, "b.txt").state == BD_STATE_BAD);

    /* Damage a copy: the full check notices it. */
    write_file(at(pv), "HELLO");
    REQUIRE_OK(bd_media_check(cat, drive, 1, &cs, quiet, NULL), cat);
    CHECK(cs.bad == 1);

    /* The next backup rewrites the missing copy. */
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 1);
    CHECK(lookup(cat, src, sub.node_id, "b.txt").state == BD_STATE_AVAILABLE);

    /* The old copy of b vanishes, then b changes: the new copy still records. */
    bd_remove(at(pb));
    write_file(at("src/Photos/sub/b.txt"), "world, version two");
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 1);
    CHECK(bs.files_failed == 0);
    CHECK(lookup(cat, src, sub.node_id, "b.txt").state == BD_STATE_AVAILABLE);

    /* Restore everything and compare. */
    bd_restore_stats rs;
    REQUIRE_OK(bd_restore(cat, 0, NULL, at("restore"), &rs, quiet, NULL), cat);
    CHECK(rs.files_restored == 2);
    CHECK(file_equals(at("restore/Photos/a.txt"), "hello, version two"));
    CHECK(file_equals(at("restore/Photos/sub/b.txt"), "world, version two"));

    /* A backup whose catalog was never saved: the next run adopts the copy. */
    write_file(at("src/Photos/c.txt"), "unsaved");
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_catalog_save(cat), cat);
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 1);
    bd_catalog_close(cat); /* closed without saving */
    REQUIRE_OK(bd_catalog_open(at("test.brodalf"), &cat), NULL);
    REQUIRE_OK(bd_media_connect(cat, at("drive"), &drive, &cs, quiet, NULL), cat);
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 0);
    CHECK(bs.files_already_there == 3);
    CHECK(lookup(cat, src, 0, "c.txt").state == BD_STATE_AVAILABLE);

    /* A missing source folder must not mark its files deleted. */
    bd_rename_noreplace(at("src/Photos"), at("src/Photos-away"));
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_deleted == 0);
    bd_rename_noreplace(at("src/Photos-away"), at("src/Photos"));

    /* A real deletion is recorded but the copies stay tracked. */
    bd_remove(at("src/Photos/c.txt"));
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_deleted == 1);
    CHECK(lookup(cat, src, 0, "c.txt").state == BD_STATE_DELETED);

    /* Where a drive is kept, renaming, and what the disk said about itself. */
    REQUIRE_OK(bd_media_set_location(cat, drive, "Box A, top shelf"), cat);
    CHECK(strcmp(media_of(cat, drive).location, "Box A, top shelf") == 0);
    bd_drive_hw hw;
    memset(&hw, 0, sizeof(hw));
    snprintf(hw.vendor, sizeof(hw.vendor), "WD");
    snprintf(hw.model, sizeof(hw.model), "Elements 25A3");
    snprintf(hw.serial, sizeof(hw.serial), "WX11A1234567");
    snprintf(hw.bus, sizeof(hw.bus), "USB");
    hw.disk_bytes = 4000787030016LL;
    hw.smart = 1;
    snprintf(hw.health, sizeof(hw.health), "good");
    hw.temperature_c = 31;
    hw.power_on_hours = 2345;
    hw.power_cycles = 120;
    hw.reallocated_sectors = 0;
    hw.pending_sectors = -1;
    hw.uncorrectable_sectors = -1;
    hw.percent_used = -1;
    bd_media_store_hw(cat, drive, &hw, quiet, NULL);
    media_snapshot_t m = media_of(cat, drive);
    CHECK(m.has_hw && strcmp(m.hw.model, "Elements 25A3") == 0 && strcmp(m.hw.serial, "WX11A1234567") == 0);
    CHECK(m.hw.power_on_hours == 2345 && m.hw.temperature_c == 31 && m.hw.pending_sectors == -1);
    CHECK(strcmp(m.hw.health, "good") == 0 && m.hw.disk_bytes == 4000787030016LL);
    /* Same drive ID on another disk: logged. */
    snprintf(hw.serial, sizeof(hw.serial), "OTHER999");
    int logged = 0;
    bd_media_store_hw(cat, drive, &hw, count_log, &logged);
    CHECK(logged == 1);
    REQUIRE_OK(bd_media_set_location(cat, drive, ""), cat);
    CHECK(strcmp(media_of(cat, drive).location, "") == 0);
    REQUIRE_OK(bd_media_rename(cat, drive, "Blue WD 4TB"), cat);
    bd_media_disconnect(cat, drive);
    REQUIRE_OK(bd_media_connect(cat, at("drive"), &drive, &cs, quiet, NULL), cat);
    char pm[1200];
    snprintf(pm, sizeof(pm), "%s/BRODALF.media", drive_dir);
    bd_media_file mfile;
    CHECK(bd_media_file_read(at(pm), &mfile) == 0 && strcmp(mfile.label, "Blue WD 4TB") == 0);
    CHECK(bd_media_rename(cat, 9999, "x") == BD_ERR_NOT_FOUND);

    /* SMART parsing: an ATA attribute table and an NVMe health log. */
    unsigned char ata[512] = {0};
    const unsigned char attrs[][12] = {
        {5, 0x33, 0, 100, 100, 3, 0, 0, 0, 0, 0, 0},       /* 3 reallocated */
        {9, 0x32, 0, 95, 95, 0x39, 0x30, 0, 0, 0, 0, 0},   /* 12345 hours */
        {194, 0x22, 0, 110, 100, 36, 0, 0, 0, 0, 0, 0},    /* 36 C */
    };
    for (int i = 0; i < 3; i++) memcpy(ata + 2 + i * 12, attrs[i], 12);
    CHECK(bd_drive_hw_parse_ata(&hw, ata, 0) == 0);
    CHECK(hw.reallocated_sectors == 3 && hw.power_on_hours == 12345 && hw.temperature_c == 36);
    CHECK(strcmp(hw.health, "warning") == 0);
    CHECK(bd_drive_hw_parse_ata(&hw, ata, 1) == 0 && strcmp(hw.health, "failing") == 0);
    unsigned char nvme[512] = {0};
    nvme[1] = (unsigned char)(310 & 0xFF);
    nvme[2] = (unsigned char)(310 >> 8); /* 310 K = 37 C */
    nvme[5] = 4;                          /* 4% used */
    nvme[112] = 50;                       /* power cycles */
    nvme[128] = 0xE8;
    nvme[129] = 0x03;                     /* 1000 hours */
    bd_drive_hw_parse_nvme(&hw, nvme);
    CHECK(hw.temperature_c == 37 && hw.percent_used == 4 && hw.power_cycles == 50 && hw.power_on_hours == 1000);
    CHECK(strcmp(hw.health, "good") == 0);

    REQUIRE_OK(bd_catalog_copy_to_media(cat, drive), cat);
    char pc[1200];
    snprintf(pc, sizeof(pc), "%s/catalog-backup.brodalf", drive_dir);
    CHECK(exists(at(pc)));
    bd_catalog_close(cat);
    CHECK(!exists(at("test.brodalf.lock")));

    test_report();

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("all core checks passed");
    return 0;
}
