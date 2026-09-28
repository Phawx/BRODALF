/* Encryption: passphrase, encrypted drives, encrypted catalog file. */
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

static char base[1024];

static char *at(const char *rel)
{
    static char buf[8][2048];
    static int i = 0;
    i = (i + 1) % 8;
    snprintf(buf[i], sizeof(buf[i]), "%s/%s", base, rel);
    return buf[i];
}

static void write_bytes(const char *path, const void *data, size_t n)
{
    char *dir = bd_strdup(path);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = '\0'; bd_mkdirs(dir); }
    free(dir);
    FILE *f = bd_fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); exit(1); }
    if (n) fwrite(data, 1, n, f);
    fclose(f);
}

static void write_file(const char *path, const char *text)
{
    write_bytes(path, text, strlen(text));
}

static long read_all(const char *path, char *buf, size_t cap)
{
    FILE *f = bd_fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(buf, 1, cap, f);
    fclose(f);
    return (long)n;
}

static int file_equals(const char *path, const void *data, size_t n)
{
    static char buf[300000];
    long got = read_all(path, buf, sizeof(buf));
    return got == (long)n && memcmp(buf, data, n) == 0;
}

static int contains(const char *path, const char *needle)
{
    static char buf[300000];
    long n = read_all(path, buf, sizeof(buf));
    size_t k = strlen(needle);
    for (long i = 0; i + (long)k <= n; i++)
        if (memcmp(buf + i, needle, k) == 0) return 1;
    return 0;
}

static int exists(const char *path)
{
    bd_stat_t st;
    return bd_stat(path, &st) == 0;
}

typedef struct { const char *name; bd_node_state state; int64_t node_id; } find_ctx;

static int find_child(void *ctx, const bd_node_info *n)
{
    find_ctx *f = ctx;
    if (strcmp(n->name, f->name) != 0) return 0;
    f->state = n->state;
    f->node_id = n->node_id;
    return 1;
}

static bd_node_state state_of(bd_catalog *cat, int64_t source, const char *name)
{
    find_ctx f = {name, (bd_node_state)-1, 0};
    bd_list_children(cat, source, 0, find_child, &f);
    return f.state;
}

static void quiet(void *ctx, const char *msg)
{
    (void)ctx;
    (void)msg;
}

#define PASS "correct horse battery"
#define PASS2 "a brand new passphrase"

int main(void)
{
    char tmp[512], id[37];
    bd_temp_dir(tmp, sizeof(tmp));
    bd_uuid_v4(id);
    snprintf(base, sizeof(base), "%s/brodalf-crypto-%.8s", tmp, id);
    for (char *p = base; *p; p++) if (*p == '\\') *p = '/';
    bd_mkdirs(base);
    printf("working in %s\n", base);

    /* Files around the 64 KiB chunk size, and an empty one. */
    static char big[200000], exact[65536];
    for (size_t i = 0; i < sizeof(big); i++) big[i] = (char)('a' + i % 23);
    for (size_t i = 0; i < sizeof(exact); i++) exact[i] = (char)('A' + i % 19);
    write_file(at("src/Docs/secret.txt"), "the plans are in the drawer");
    write_bytes(at("src/Docs/big.bin"), big, sizeof(big));
    write_bytes(at("src/Docs/exact.bin"), exact, sizeof(exact));
    write_bytes(at("src/Docs/empty.txt"), "", 0);
    bd_mkdirs(at("vault"));
    bd_mkdirs(at("plain"));

    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("c.brodalf"), &cat), NULL);
    int64_t src;
    REQUIRE_OK(bd_source_add(cat, at("src/Docs"), &src), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);

    /* An encrypted drive needs a passphrase first. */
    int64_t vault, plain;
    CHECK(bd_media_init(cat, at("vault"), "Vault", BD_MEDIA_ENCRYPTED, &vault) == BD_ERR_PASSPHRASE);
    CHECK(!bd_catalog_has_passphrase(cat));
    CHECK(bd_catalog_set_passphrase(cat, "short") == BD_ERR_INVALID);
    REQUIRE_OK(bd_catalog_set_passphrase(cat, PASS), cat);
    CHECK(bd_catalog_has_passphrase(cat) && bd_catalog_is_unlocked(cat));
    REQUIRE_OK(bd_media_init(cat, at("vault"), "Vault", BD_MEDIA_ENCRYPTED, &vault), cat);
    REQUIRE_OK(bd_media_init(cat, at("plain"), "Plain", 0, &plain), cat);

    /* Back up: names readable, contents not. */
    bd_backup_stats bs;
    REQUIRE_OK(bd_backup(cat, vault, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 4 && bs.files_failed == 0);
    char vdir[1200], p_secret[1400], p_big[1400], p_exact[1400];
    snprintf(vdir, sizeof(vdir), "vault/BRODALF/%s", bd_catalog_uuid(cat));
    snprintf(p_secret, sizeof(p_secret), "%s/Docs/secret.txt.bdenc", vdir);
    snprintf(p_big, sizeof(p_big), "%s/Docs/big.bin.bdenc", vdir);
    snprintf(p_exact, sizeof(p_exact), "%s/Docs/exact.bin.bdenc", vdir);
    CHECK(exists(at(p_secret)));
    CHECK(!contains(at(p_secret), "plans"));
    CHECK(!contains(at(p_big), "abcdefghij"));
    CHECK(state_of(cat, src, "secret.txt") == BD_STATE_AVAILABLE);
    bd_check_stats cs;
    REQUIRE_OK(bd_media_check(cat, vault, 1, &cs, quiet, NULL), cat);
    CHECK(cs.ok == 4 && cs.bad == 0);
    REQUIRE_OK(bd_backup(cat, plain, 0, &bs, quiet, NULL), cat);

    /* The catalog backup on an encrypted drive is encrypted. */
    REQUIRE_OK(bd_catalog_copy_to_media(cat, vault), cat);
    char p_catbak[1400];
    snprintf(p_catbak, sizeof(p_catbak), "%s/catalog-backup.brodalf", vdir);
    CHECK(bd_catalog_file_needs_passphrase(at(p_catbak)));
    CHECK(!bd_catalog_file_needs_passphrase(at("c.brodalf")));

    /* Reopen: the passphrase is remembered but the key is not. */
    REQUIRE_OK(bd_catalog_save(cat), cat);
    bd_catalog_close(cat);
    REQUIRE_OK(bd_catalog_open(at("c.brodalf"), &cat), NULL);
    CHECK(bd_catalog_has_passphrase(cat) && !bd_catalog_is_unlocked(cat));
    REQUIRE_OK(bd_media_connect(cat, at("vault"), &vault, &cs, quiet, NULL), cat);
    CHECK(cs.ok == 4);
    CHECK(state_of(cat, src, "secret.txt") == BD_STATE_AVAILABLE);
    CHECK(bd_media_check(cat, vault, 1, &cs, quiet, NULL) == BD_ERR_PASSPHRASE);

    /* A changed file cannot go to the vault while locked. */
    write_file(at("src/Docs/secret.txt"), "the plans moved to the safe");
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(bd_backup(cat, vault, 0, &bs, quiet, NULL) == BD_ERR_PASSPHRASE);
    CHECK(bd_catalog_unlock(cat, "wrong passphrase") == BD_ERR_PASSPHRASE);
    CHECK(!bd_catalog_is_unlocked(cat));
    REQUIRE_OK(bd_catalog_unlock(cat, PASS), cat);
    REQUIRE_OK(bd_backup(cat, vault, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 1 && bs.versions_moved == 1);
    char p_old[1400];
    snprintf(p_old, sizeof(p_old), "%s/.versions/Docs/secret.v1.txt.bdenc", vdir);
    CHECK(exists(at(p_old)));
    CHECK(!contains(at(p_old), "drawer"));

    /* Restore from the vault only (the plain drive is not connected). */
    bd_restore_stats rs;
    REQUIRE_OK(bd_restore(cat, 0, NULL, at("restored"), &rs, quiet, NULL), cat);
    CHECK(rs.files_restored == 4 && rs.files_failed == 0);
    CHECK(file_equals(at("restored/Docs/secret.txt"), "the plans moved to the safe", 27));
    CHECK(file_equals(at("restored/Docs/big.bin"), big, sizeof(big)));
    CHECK(file_equals(at("restored/Docs/exact.bin"), exact, sizeof(exact)));
    CHECK(file_equals(at("restored/Docs/empty.txt"), "", 0));

    /* Locked: restore says it needs the passphrase instead of failing. */
    bd_catalog_lock_key(cat);
    REQUIRE_OK(bd_restore(cat, 0, NULL, at("restored2"), &rs, quiet, NULL), cat);
    CHECK(rs.files_restored == 0 && rs.files_need_passphrase == 4);
    REQUIRE_OK(bd_catalog_unlock(cat, PASS), cat);

    /* Tampering and truncation are caught by a full check. */
    static char buf[300000];
    long n = read_all(at(p_big), buf, sizeof(buf));
    CHECK(n > 65536 + 100);
    buf[65536 + 60] ^= 1;
    write_bytes(at(p_big), buf, (size_t)n);
    n = read_all(at(p_exact), buf, sizeof(buf));
    CHECK(n == 40 + 16 + 65536 + 16); /* one full chunk and an empty last chunk */
    write_bytes(at(p_exact), buf, (size_t)(40 + 16 + 65536));
    REQUIRE_OK(bd_media_check(cat, vault, 1, &cs, quiet, NULL), cat);
    CHECK(cs.bad == 2);

    /* Encrypt the catalog file itself. */
    REQUIRE_OK(bd_catalog_set_file_encrypted(cat, 1), cat);
    REQUIRE_OK(bd_catalog_save(cat), cat);
    CHECK(bd_catalog_file_needs_passphrase(at("c.brodalf")));
    CHECK(!contains(at("c.brodalf"), "secret.txt"));
    bd_catalog_close(cat);
    CHECK(bd_catalog_open(at("c.brodalf"), &cat) == BD_ERR_PASSPHRASE);
    CHECK(bd_catalog_open_with(at("c.brodalf"), "wrong passphrase", &cat) == BD_ERR_PASSPHRASE);
    CHECK(!exists(at("c.brodalf.lock")));
    REQUIRE_OK(bd_catalog_open_with(at("c.brodalf"), PASS, &cat), NULL);
    CHECK(bd_catalog_is_unlocked(cat));
    CHECK(state_of(cat, src, "secret.txt") != (bd_node_state)-1);

    /* Change the passphrase: the old one stops working, copies still read. */
    REQUIRE_OK(bd_catalog_set_passphrase(cat, PASS2), cat);
    REQUIRE_OK(bd_catalog_save(cat), cat);
    bd_catalog_close(cat);
    CHECK(bd_catalog_open_with(at("c.brodalf"), PASS, &cat) == BD_ERR_PASSPHRASE);
    REQUIRE_OK(bd_catalog_open_with(at("c.brodalf"), PASS2, &cat), NULL);
    REQUIRE_OK(bd_media_connect(cat, at("vault"), &vault, &cs, quiet, NULL), cat);
    REQUIRE_OK(bd_restore(cat, 0, "secret.txt", at("restored3"), &rs, quiet, NULL), cat);
    CHECK(rs.files_restored == 1);
    CHECK(file_equals(at("restored3/Docs/secret.txt"), "the plans moved to the safe", 27));

    /* Turning catalog encryption off writes a plain file again. */
    REQUIRE_OK(bd_catalog_set_file_encrypted(cat, 0), cat);
    REQUIRE_OK(bd_catalog_save(cat), cat);
    CHECK(!bd_catalog_file_needs_passphrase(at("c.brodalf")));
    bd_catalog_close(cat);

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("all encryption checks passed");
    return 0;
}
