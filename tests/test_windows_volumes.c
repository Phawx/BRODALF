/* Windows only: BRODALF on real FAT32 and exFAT volumes and on a network
 * share of this PC, where until now only stand-ins (bd_test_fs_name and
 * the like) were tested. The volumes and the share are made before the
 * test (on CI by .github/workflows/c-core.yml: small virtual disks made
 * with diskpart, and New-SmbShare) and named by these variables:
 *   BRODALF_TEST_FAT32, BRODALF_TEST_EXFAT  the volumes' roots, "F:\"
 *   BRODALF_TEST_FAT32_UNIT, ..._EXFAT_UNIT their allocation unit in bytes
 *                                           (optional: then it is checked)
 *   BRODALF_TEST_SHARE      a share of this PC, "\\localhost\Name", that the
 *                           test may remove and make again (administrator)
 *   BRODALF_TEST_SHARE_DIR  the folder it shares
 * Without them the checks are skipped, saying so; on CI (the CI variable
 * set) that is a failure. The volumes are written to (folders named
 * brodalf-test-*) and should be throwaway ones.
 *
 * On each volume: the file system BRODALF reads; free space as Windows
 * gives it, and the allocation unit; a backup to it and a restore from it
 * with modified times kept (to two seconds, as Windows stores them on
 * FAT32 and exFAT),
 * and a backup root on it. A file of 4 GiB planned out of a FAT32 drive,
 * never written, but not out of an exFAT one. A path near 260 characters
 * whose copy on the drive and restore are longer. A backup root on the
 * share found, then missing when the share is removed (nothing marked
 * deleted), and found again. */
#include "brodalf.h"
#include "internal.h"
#include "test_tree.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <lm.h>

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

static char top[1024];  /* the temp folder for the whole run (NTFS) */
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
    char *back = strrchr(dir, '\\');
    if (back && (!slash || back > slash)) slash = back;
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

static int file_equals(const char *path, const char *text)
{
    FILE *f = bd_fopen(path, "rb");
    if (!f) return 0;
    char buf[512] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    return n == strlen(text) && memcmp(buf, text, n) == 0;
}

static void quiet(void *ctx, const char *msg)
{
    (void)ctx;
    (void)msg;
}

static int on_ci(void)
{
    const char *ci = getenv("CI");
    return ci && *ci;
}

static void not_tried(const char *what, const char *why)
{
    if (on_ci()) {
        fprintf(stderr, "%s: not tried on CI, %s\n", what, why);
        failures++;
    } else {
        printf("%s: not tried, %s\n", what, why);
    }
}

/* "X:\" from "X:", "X:\" or "X:/"; NULL for anything else. */
static char *volume_root(const char *v)
{
    if (!v || !v[0] || v[1] != ':' || (v[2] && ((v[2] != '\\' && v[2] != '/') || v[3]))) return NULL;
    return bd_sprintf("%c:\\", v[0]);
}

static wchar_t *wide(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = malloc(sizeof(wchar_t) * (size_t)(n > 0 ? n : 1));
    if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    for (wchar_t *p = w; p && *p; p++) if (*p == L'/') *p = L'\\';
    return w;
}

static int64_t time_of(const char *path)
{
    bd_stat_t st;
    return bd_stat(path, &st) == 0 ? st.mtime_ns : -1;
}

/* Set by Windows itself, not through BRODALF. */
static void set_time(const char *path, int64_t ns)
{
    wchar_t *w = wide(path);
    HANDLE h = w ? CreateFileW(w, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, NULL)
                 : INVALID_HANDLE_VALUE;
    free(w);
    CHECK(h != INVALID_HANDLE_VALUE);
    if (h == INVALID_HANDLE_VALUE) return;
    uint64_t t = (uint64_t)(ns / 100) + 116444736000000000ULL;
    FILETIME ft;
    ft.dwLowDateTime = (DWORD)(t & 0xffffffffu);
    ft.dwHighDateTime = (DWORD)(t >> 32);
    CHECK(SetFileTime(h, NULL, NULL, &ft));
    CloseHandle(h);
}

static int64_t ns_abs(int64_t x) { return x < 0 ? -x : x; }

/* Free space and size as Windows gives them for root, and its allocation
 * unit. */
static void windows_space(const char *root, int64_t *free_b, int64_t *total_b, int64_t *unit)
{
    wchar_t *w = wide(root);
    ULARGE_INTEGER avail, total, fr;
    DWORD spc = 0, bps = 0, nfree = 0, nall = 0;
    *free_b = *total_b = *unit = -1;
    if (w && GetDiskFreeSpaceExW(w, &avail, &total, &fr)) {
        *free_b = (int64_t)avail.QuadPart;
        *total_b = (int64_t)total.QuadPart;
    }
    if (w && GetDiskFreeSpaceW(w, &spc, &bps, &nfree, &nall)) *unit = (int64_t)spc * bps;
    free(w);
}

typedef struct { const char *want; int n; } name_count;

static int count_named(void *ctx, const char *rel, const bd_stat_t *st)
{
    name_count *c = ctx;
    (void)st;
    if (strstr(rel, c->want)) c->n++;
    return 0;
}

static int files_named(const char *dir, const char *want)
{
    name_count c = {want, 0};
    bd_walk(dir, count_named, NULL, &c);
    return c.n;
}

/* Where BRODALF puts a root's copies on a drive: <drive>\BRODALF\<catalog>\<root's name>. */
static char *copies_dir(bd_catalog *cat, const char *drive_root, const char *root_name)
{
    size_t n = strlen(drive_root);
    const char *sep = n && (drive_root[n - 1] == '\\' || drive_root[n - 1] == '/') ? "" : "/";
    return bd_sprintf("%s%sBRODALF/%s/%s", drive_root, sep, bd_catalog_uuid(cat), root_name);
}

/* ---- One volume ----------------------------------------------------------- */

#define N_SMALL 20

static void test_volume(const char *label, const char *root, const char *want_fs, const char *unit_env, int64_t time_step)
{
    char name[64];
    snprintf(name, sizeof(name), "volume-%s", label);
    begin(name);

    /* The file system, as BRODALF reads it and as the disk tells it. */
    char fs[64] = "";
    CHECK(bd_fs_name(root, fs, sizeof(fs)) == 0);
    printf("  %s: BRODALF reads the file system as \"%s\"\n", root, fs);
    CHECK(strcmp(fs, want_fs) == 0);
    bd_drive_hw hw;
    int rc = bd_drive_hw_read(root, &hw);
    printf("  %s: rc %d, model \"%s\", bus \"%s\", file system \"%s\", volume \"%s\" %s, disk %lld bytes, note \"%s\"\n", root,
           rc, hw.model, hw.bus, hw.filesystem, hw.volume_name, hw.volume_serial, (long long)hw.disk_bytes, hw.note);
    CHECK(rc == 0 && strcmp(hw.filesystem, want_fs) == 0);

    /* Free space: what BRODALF reads is what Windows says. */
    int64_t total = -1, freeb = -1, wfree, wtotal, unit;
    CHECK(bd_disk_space(root, &total, &freeb) == 0);
    windows_space(root, &wfree, &wtotal, &unit);
    printf("  %s: %lld of %lld bytes free, allocation unit %lld bytes\n", root, (long long)freeb, (long long)total,
           (long long)unit);
    CHECK(total == wtotal && freeb == wfree && total > 100 * 1024 * 1024 && freeb > 0 && freeb <= total);
    CHECK(unit >= 512);
    /* A backup counts the room each copy takes there in these units (R2). */
    char *in_root = bd_sprintf("%sbrodalf-test-unit", root);
    bd_mkdirs(in_root);
    printf("  %s: BRODALF reads the allocation unit as %lld bytes\n", root, (long long)bd_fs_cluster_bytes(in_root));
    CHECK(bd_fs_cluster_bytes(root) == unit && bd_fs_cluster_bytes(in_root) == unit);
    remove_tree(in_root);
    free(in_root);
    const char *want_unit = getenv(unit_env);
    if (want_unit && *want_unit) CHECK(unit == strtoll(want_unit, NULL, 10));

    /* A backup to it and a restore from it. */
    const int64_t t_doc = 1700000000123456700LL, t_small = 1650000001500000000LL;
    write_file(at("Src/doc.txt"), "a document with its own modified time");
    set_time(at("Src/doc.txt"), t_doc);
    for (int i = 0; i < N_SMALL; i++) {
        char rel[64];
        snprintf(rel, sizeof(rel), "Src/small/s%02d.txt", i);
        write_file(at(rel), "x");
        set_time(at(rel), t_small + (int64_t)i * 1000000000LL);
    }
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("c.brodalf"), &cat), NULL);
    int64_t src, drive;
    REQUIRE_OK(bd_source_add(cat, at("Src"), &src), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_new == N_SMALL + 1);
    REQUIRE_OK(bd_media_init(cat, root, label, 0, &drive), cat);

    bd_backup_preview_info pv;
    REQUIRE_OK(bd_backup_preview(cat, drive, 0, &pv), cat);
    windows_space(root, &wfree, &wtotal, &unit);
    CHECK(pv.files_to_copy == N_SMALL + 1 && pv.files_too_big == 0 && pv.too_big_why == NULL && pv.bytes_short == 0);
    CHECK(pv.free_is_current && pv.free_bytes == wfree);
    int64_t before = wfree;
    bd_backup_stats bs;
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == N_SMALL + 1 && bs.files_failed == 0 && bs.files_too_big == 0);
    windows_space(root, &wfree, &wtotal, &unit);
    /* Each small file takes a whole allocation unit there, whatever its
     * size: what the preview counts as bytes to copy is less than what the
     * drive gives up (R2 counts it in units). */
    printf("  %s: the preview counted %lld bytes to copy; the drive gave up %lld bytes (%d files of 1 byte, units of %lld)\n",
           root, (long long)pv.bytes_to_copy, (long long)(before - wfree), N_SMALL, (long long)unit);
    CHECK(before - wfree >= (int64_t)N_SMALL * unit);

    /* The copies keep the time, as closely as the file system stores it. */
    char *dir = copies_dir(cat, root, "Src");
    char *copy_doc = bd_sprintf("%s/doc.txt", dir), *copy_small = bd_sprintf("%s/small/s07.txt", dir);
    int64_t d1 = time_of(copy_doc) - t_doc, d2 = time_of(copy_small) - (t_small + 7 * 1000000000LL);
    printf("  %s: copies' times off by %lld and %lld ns (it stores them in steps of %lld ns)\n", root, (long long)d1,
           (long long)d2, (long long)time_step);
    CHECK(ns_abs(d1) <= time_step && ns_abs(d2) <= time_step);

    /* Checked: every copy as recorded, whatever the file system did to its
     * time; and the next backup copies nothing again. */
    bd_check_stats cs;
    REQUIRE_OK(bd_media_check(cat, drive, &cs, quiet, NULL), cat);
    CHECK(cs.copies == N_SMALL + 1 && cs.ok == N_SMALL + 1 && cs.bad == 0 && cs.missing == 0);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_changed == 0 && ss.files_new == 0);
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 0 && bs.files_failed == 0);

    /* Restored to an NTFS folder: the times exactly as they were. */
    bd_restore_stats rs;
    REQUIRE_OK(bd_restore(cat, src, "", at("restored"), &rs, quiet, NULL), cat);
    CHECK(rs.files_failed == 0 && rs.files_restored == N_SMALL + 1);
    CHECK(file_equals(at("restored/doc.txt"), "a document with its own modified time"));
    CHECK(time_of(at("restored/doc.txt")) == t_doc);
    CHECK(time_of(at("restored/small/s07.txt")) == t_small + 7 * 1000000000LL);

    /* Restored onto the volume itself: as closely as it stores them. */
    char *here = bd_sprintf("%sbrodalf-test-restored", root);
    REQUIRE_OK(bd_restore(cat, src, "", here, &rs, quiet, NULL), cat);
    CHECK(rs.files_failed == 0 && rs.files_restored == N_SMALL + 1);
    char *here_doc = bd_sprintf("%s/doc.txt", here);
    CHECK(file_equals(here_doc, "a document with its own modified time"));
    CHECK(ns_abs(time_of(here_doc) - t_doc) <= time_step);
    bd_catalog_close(cat);

    /* A backup root on the volume, backed up to an NTFS folder and restored. */
    char *vroot = bd_sprintf("%sbrodalf-test-root", root);
    char *vfile = bd_sprintf("%s/Letters/letter.txt", vroot);
    write_file(vfile, "a letter on the volume");
    set_time(vfile, t_doc);
    int64_t on_volume = time_of(vfile);
    int64_t vsrc, ntfs;
    REQUIRE_OK(bd_catalog_create(at("on-volume.brodalf"), &cat), NULL);
    REQUIRE_OK(bd_source_add(cat, vroot, &vsrc), cat);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_new == 1 && ss.errors == 0);
    bd_mkdirs(at("ntfs-drive"));
    REQUIRE_OK(bd_media_init(cat, at("ntfs-drive"), "NTFS folder", 0, &ntfs), cat);
    REQUIRE_OK(bd_backup(cat, ntfs, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 1 && bs.files_failed == 0);
    REQUIRE_OK(bd_restore(cat, vsrc, "", at("from-volume"), &rs, quiet, NULL), cat);
    CHECK(rs.files_restored == 1 && file_equals(at("from-volume/Letters/letter.txt"), "a letter on the volume"));
    CHECK(time_of(at("from-volume/Letters/letter.txt")) == on_volume);
    bd_catalog_close(cat);

    remove_tree(here);
    remove_tree(vroot);
    char *brodalf_dir = bd_sprintf("%sBRODALF", root);
    remove_tree(brodalf_dir);
    free(brodalf_dir);
    free(dir);
    free(copy_doc);
    free(copy_small);
    free(here);
    free(here_doc);
    free(vroot);
    free(vfile);
}

/* ---- 4 GiB and more ----------------------------------------------------- */

/* A file of size bytes that takes no room (sparse), on NTFS. */
static int make_sparse(const char *path, int64_t size)
{
    wchar_t *w = wide(path);
    HANDLE h = w ? CreateFileW(w, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL) : INVALID_HANDLE_VALUE;
    free(w);
    if (h == INVALID_HANDLE_VALUE) return -1;
    DWORD got = 0;
    LARGE_INTEGER end;
    end.QuadPart = size;
    int ok = DeviceIoControl(h, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &got, NULL) && SetFilePointerEx(h, end, NULL, FILE_BEGIN) &&
             SetEndOfFile(h);
    CloseHandle(h);
    return ok ? 0 : -1;
}

static void test_too_big(const char *fat, const char *exfat)
{
    begin("too-big");
    const int64_t four_gib = 4294967296LL;
    write_file(at("Big/a.txt"), "small");
    CHECK(make_sparse(at("Big/video.mkv"), four_gib) == 0);
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("c.brodalf"), &cat), NULL);
    int64_t src, fat_drive, exfat_drive;
    REQUIRE_OK(bd_source_add(cat, at("Big"), &src), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_new == 2 && ss.errors == 0);

    /* FAT32: planned out, said why, never written. */
    char *fat_root = bd_sprintf("%sbrodalf-test-big", fat);
    bd_mkdirs(fat_root);
    REQUIRE_OK(bd_media_init(cat, fat_root, "FAT32 stick", 0, &fat_drive), cat);
    bd_backup_preview_info pv;
    REQUIRE_OK(bd_backup_preview(cat, fat_drive, 0, &pv), cat);
    CHECK(pv.files_to_copy == 2 && pv.files_too_big == 1 && pv.bytes_too_big == four_gib);
    CHECK(pv.too_big_why && strcmp(pv.too_big_why, BD_WHY_FAT32) == 0);
    int64_t before, after, total, unit;
    windows_space(fat, &before, &total, &unit);
    bd_backup_stats bs;
    REQUIRE_OK(bd_backup(cat, fat_drive, 0, &bs, quiet, NULL), cat);
    windows_space(fat, &after, &total, &unit);
    CHECK(bs.files_copied == 1 && bs.files_failed == 0 && bs.files_too_big == 1 && bs.bytes_too_big == four_gib);
    CHECK(bs.too_big_why && strcmp(bs.too_big_why, BD_WHY_FAT32) == 0);
    CHECK(files_named(fat_root, "video") == 0);
    CHECK(before - after < 16 * 1024 * 1024); /* nothing of it went there */

    /* exFAT holds files that big: not too big there, only more than this
     * small volume has room for. */
    char *exfat_root = bd_sprintf("%sbrodalf-test-big", exfat);
    bd_mkdirs(exfat_root);
    REQUIRE_OK(bd_media_init(cat, exfat_root, "exFAT stick", 0, &exfat_drive), cat);
    REQUIRE_OK(bd_backup_preview(cat, exfat_drive, 0, &pv), cat);
    CHECK(pv.files_to_copy == 2 && pv.files_too_big == 0 && pv.too_big_why == NULL && pv.bytes_short > 0);
    REQUIRE_OK(bd_backup(cat, exfat_drive, 0, &bs, quiet, NULL), cat);
    printf("  exFAT: %lld copied, %lld with no room, %lld failed, %lld too big\n", (long long)bs.files_copied,
           (long long)bs.files_no_room, (long long)bs.files_failed, (long long)bs.files_too_big);
    CHECK(bs.files_copied == 1 && bs.files_too_big == 0 && bs.too_big_why == NULL && bs.files_no_room == 1 &&
          bs.files_failed == 0);
    CHECK(files_named(exfat_root, "video") == 0);
    bd_catalog_close(cat);
    remove_tree(fat_root);
    remove_tree(exfat_root);
    free(fat_root);
    free(exfat_root);
}

/* ---- A path near 260 characters ------------------------------------------- */

static void test_long_path(const char *fat, const char *exfat)
{
    begin("long-path");
    /* The root at the top of the FAT32 volume, the file deep in it: its own
     * path stays under 260 characters, its copy on the exFAT drive and its
     * restore into the temp folder do not. */
    char *root = bd_sprintf("%sL", fat);
    char rel[400] = "";
    for (int i = 0; i < 6; i++) {
        char part[48];
        snprintf(part, sizeof(part), "a-folder-with-quite-a-long-name-%02d/", i);
        strcat(rel, part);
    }
    strcat(rel, "and-a-file-with-a-long-name-too.txt");
    char *file = bd_sprintf("%s\\%s", root, rel);
    size_t file_len = strlen(file);
    printf("  the file's own path is %u characters\n", (unsigned)file_len);
    CHECK(file_len >= 240 && file_len < 260);
    write_file(file, "deep down");

    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("c.brodalf"), &cat), NULL);
    int64_t src, drive;
    REQUIRE_OK(bd_source_add(cat, root, &src), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_new == 1 && ss.errors == 0);
    char *drive_root = bd_sprintf("%sbrodalf-test-long", exfat);
    bd_mkdirs(drive_root);
    REQUIRE_OK(bd_media_init(cat, drive_root, "Long", 0, &drive), cat);
    bd_backup_stats bs;
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 1 && bs.files_failed == 0);
    /* Counted as a copy whose path there is longer than Explorer opens (R12). */
    printf("  copies with a long path there: %lld after the backup\n", (long long)bs.long_paths);
    CHECK(bs.long_paths == 1);
    char *dir = copies_dir(cat, drive_root, "L");
    char *copy = bd_sprintf("%s/%s", dir, rel);
    printf("  its copy's path is %u characters\n", (unsigned)strlen(copy));
    CHECK(strlen(copy) > 260);
    CHECK(file_equals(copy, "deep down"));
    bd_check_stats cs;
    REQUIRE_OK(bd_media_check(cat, drive, &cs, quiet, NULL), cat);
    CHECK(cs.copies == 1 && cs.ok == 1 && cs.long_paths == 1);

    bd_restore_stats rs;
    REQUIRE_OK(bd_restore(cat, src, "", at("restored"), &rs, quiet, NULL), cat);
    char *back = bd_sprintf("%s/%s", at("restored"), rel);
    printf("  restored to a path of %u characters\n", (unsigned)strlen(back));
    CHECK(strlen(back) > 260);
    CHECK(rs.files_restored == 1 && rs.files_failed == 0 && file_equals(back, "deep down"));
    bd_catalog_close(cat);
    remove_tree(root);
    remove_tree(drive_root);
    free(root);
    free(file);
    free(drive_root);
    free(dir);
    free(copy);
    free(back);
}

/* ---- A backup root on a share --------------------------------------------- */

typedef struct { int64_t id; int missing; } root_state;

static int root_state_cb(void *ctx, const bd_source_info *s)
{
    root_state *r = ctx;
    if (s->source_id == r->id) r->missing = s->missing;
    return 0;
}

static int root_missing(bd_catalog *cat, int64_t id)
{
    root_state r = {id, -1};
    bd_list_sources(cat, root_state_cb, &r);
    return r.missing;
}

/* Waits up to 20 seconds for path to be reachable (want 1) or not (0). */
static int wait_reachable(const char *path, int want)
{
    wchar_t *w = wide(path);
    int now = -1;
    for (int i = 0; w && i < 80; i++) {
        now = GetFileAttributesW(w) != INVALID_FILE_ATTRIBUTES;
        if (now == want) break;
        Sleep(250);
    }
    free(w);
    return now == want;
}

static void test_share(const char *share, const char *share_dir)
{
    begin("share");
    const char *name = strrchr(share, '\\');
    CHECK(strncmp(share, "\\\\", 2) == 0 && name && name[1]);
    if (!name || !name[1]) return;
    name++;
    char *docs_dir = bd_sprintf("%s\\BrodalfDocs", share_dir);
    for (int i = 0; i < 3; i++) {
        char *p = bd_sprintf("%s\\doc%d.txt", docs_dir, i);
        write_file(p, "on the share");
        free(p);
    }
    char *root = bd_sprintf("%s\\BrodalfDocs", share);
    CHECK(wait_reachable(root, 1));

    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("c.brodalf"), &cat), NULL);
    int64_t src;
    REQUIRE_OK(bd_source_add(cat, root, &src), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_new == 3 && ss.roots_missing == 0 && ss.errors == 0);
    CHECK(root_missing(cat, src) == 0);

    /* The share removed, as when its PC is off: the root is missing, and
     * nothing in it is taken for deleted. */
    wchar_t *wname = wide(name);
    NET_API_STATUS del = wname ? NetShareDel(NULL, wname, 0) : NERR_NetNameNotFound;
    printf("  removing %s: %lu\n", share, (unsigned long)del);
    CHECK(del == NERR_Success);
    CHECK(wait_reachable(root, 0));
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.roots_missing == 1 && ss.files_deleted == 0);
    CHECK(root_missing(cat, src) == 1);

    /* Shared again: found, nothing changed. */
    wchar_t *wdir = wide(share_dir);
    SHARE_INFO_2 si;
    memset(&si, 0, sizeof(si));
    si.shi2_netname = wname;
    si.shi2_type = STYPE_DISKTREE;
    si.shi2_remark = (LPWSTR)L"BRODALF test share";
    si.shi2_permissions = ACCESS_ALL;
    si.shi2_max_uses = (DWORD)-1;
    si.shi2_path = wdir;
    DWORD bad = 0;
    NET_API_STATUS add = wname && wdir ? NetShareAdd(NULL, 2, (LPBYTE)&si, &bad) : NERR_NetNameNotFound;
    printf("  sharing %s again: %lu\n", share, (unsigned long)add);
    CHECK(add == NERR_Success);
    CHECK(wait_reachable(root, 1));
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.roots_missing == 0 && ss.files_deleted == 0 && ss.files_new == 0 && ss.files_changed == 0);
    CHECK(root_missing(cat, src) == 0);
    bd_catalog_close(cat);
    remove_tree(docs_dir);
    free(wname);
    free(wdir);
    free(docs_dir);
    free(root);
}

int main(void)
{
    char tmp[512], id[37];
    bd_temp_dir(tmp, sizeof(tmp));
    bd_uuid_v4(id);
    setvbuf(stdout, NULL, _IONBF, 0); /* what it prints, in order with the failed checks */
    snprintf(top, sizeof(top), "%s/brodalf-volumes-%.8s", tmp, id);
    for (char *p = top; *p; p++) if (*p == '\\') *p = '/';
    bd_mkdirs(top);
    printf("working in %s\n", top);

    char *fat = volume_root(getenv("BRODALF_TEST_FAT32")), *exfat = volume_root(getenv("BRODALF_TEST_EXFAT"));
    const char *share = getenv("BRODALF_TEST_SHARE"), *share_dir = getenv("BRODALF_TEST_SHARE_DIR");
    if (fat) test_volume("FAT32", fat, "FAT32", "BRODALF_TEST_FAT32_UNIT", 2000000000LL);
    else not_tried("a FAT32 volume", "BRODALF_TEST_FAT32 does not name one (\"F:\\\")");
    /* exFAT has a field for modified times to 10 ms, but Windows leaves it
     * empty: on CI its copies' times came out in steps of 2 seconds, as on
     * FAT32 (and BRODALF's check and rescan took them as they were). */
    if (exfat) test_volume("exFAT", exfat, "exFAT", "BRODALF_TEST_EXFAT_UNIT", 2000000000LL);
    else not_tried("an exFAT volume", "BRODALF_TEST_EXFAT does not name one");
    if (fat && exfat) {
        test_too_big(fat, exfat);
        test_long_path(fat, exfat);
    } else {
        not_tried("4 GiB files and long paths", "they need both volumes");
    }
    if (share && *share && share_dir && *share_dir) test_share(share, share_dir);
    else not_tried("a backup root on a share", "BRODALF_TEST_SHARE and BRODALF_TEST_SHARE_DIR are not both set");
    free(fat);
    free(exfat);

    remove_tree(top);
    CHECK(!exists(top));
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("all Windows volume checks passed");
    return 0;
}

#else
#include <stdio.h>
int main(void)
{
    puts("Windows only");
    return 0;
}
#endif
