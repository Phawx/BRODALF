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

static void put_le32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static void hex_of(const uint8_t *b, size_t n, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = digits[b[i] >> 4];
        out[i * 2 + 1] = digits[b[i] & 15];
    }
    out[n * 2] = '\0';
}

static int count_tmp_cb(void *ctx, const char *rel, const bd_stat_t *st)
{
    size_t n = strlen(rel), k = strlen(BD_TMP_MARKER);
    if (st->is_file && n > k && strcmp(rel + n - k, BD_TMP_MARKER) == 0) (*(int *)ctx)++;
    return 0;
}

#define PASS "correct horse battery"
#define PASS2 "a brand new passphrase"

static int key_shape_ok(const char *k)
{
    if (strlen(k) != BD_RECOVERY_KEY_SIZE - 1) return 0;
    for (int i = 0; k[i]; i++) {
        if (i % 6 == 5) { if (k[i] != '-') return 0; continue; }
        if (!strchr("23456789ABCDEFGHJKLMNPQRSTUVWXYZ", k[i])) return 0;
    }
    return 1;
}

/* The recovery key: a second way to unlock, in the catalog and in the
 * header of an encrypted catalog file, and the recovery sheet that prints it. */
static void test_recovery_key(void)
{
    write_file(at("rk/src/note.txt"), "recover me");
    bd_mkdirs(at("rk/vault"));
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("rk/k.brodalf"), &cat), NULL);
    char key[BD_RECOVERY_KEY_SIZE], key2[BD_RECOVERY_KEY_SIZE];
    CHECK(bd_catalog_make_recovery_key(cat, key) == BD_ERR_INVALID); /* no passphrase yet */
    CHECK(!bd_catalog_has_recovery_key(cat));
    REQUIRE_OK(bd_catalog_set_passphrase(cat, "first passphrase"), cat);
    int64_t src, vault;
    REQUIRE_OK(bd_source_add(cat, at("rk/src"), &src), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_media_init(cat, at("rk/vault"), "Vault", BD_MEDIA_ENCRYPTED, &vault), cat);
    bd_backup_stats bs;
    REQUIRE_OK(bd_backup(cat, vault, 0, &bs, quiet, NULL), cat);
    bd_catalog_lock_key(cat);
    CHECK(bd_catalog_make_recovery_key(cat, key) == BD_ERR_PASSPHRASE); /* locked */
    REQUIRE_OK(bd_catalog_unlock(cat, "first passphrase"), cat);
    REQUIRE_OK(bd_catalog_make_recovery_key(cat, key), cat);
    CHECK(key_shape_ok(key) && bd_catalog_has_recovery_key(cat));

    /* Either way unlocks; case, dashes and spaces do not matter; a wrong key does not. */
    bd_catalog_lock_key(cat);
    char typed[64];
    snprintf(typed, sizeof(typed), " %s ", key);
    for (char *p = typed; *p; p++) {
        if (*p == '-') *p = ' ';
        else if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
    }
    REQUIRE_OK(bd_catalog_unlock_recovery(cat, typed), cat);
    CHECK(bd_catalog_is_unlocked(cat));
    bd_catalog_lock_key(cat);
    char wrong[BD_RECOVERY_KEY_SIZE];
    memcpy(wrong, key, sizeof(wrong));
    wrong[0] = wrong[0] == '2' ? '3' : '2';
    CHECK(bd_catalog_unlock_recovery(cat, wrong) == BD_ERR_PASSPHRASE && !bd_catalog_is_unlocked(cat));
    CHECK(bd_catalog_unlock_recovery(cat, "too short") == BD_ERR_PASSPHRASE);
    CHECK(bd_catalog_unlock_recovery(cat, "first passphrase") == BD_ERR_PASSPHRASE);
    CHECK(bd_catalog_unlock(cat, key) == BD_ERR_PASSPHRASE); /* not a passphrase */
    REQUIRE_OK(bd_catalog_unlock(cat, "first passphrase"), cat);

    /* Changing the passphrase keeps the recovery key. */
    REQUIRE_OK(bd_catalog_set_passphrase(cat, "second passphrase"), cat);
    bd_catalog_lock_key(cat);
    REQUIRE_OK(bd_catalog_unlock_recovery(cat, key), cat);

    /* An encrypted catalog file opens with either; the passphrase is never in it. */
    REQUIRE_OK(bd_catalog_set_file_encrypted(cat, 1), cat);
    REQUIRE_OK(bd_catalog_save(cat), cat);
    REQUIRE_OK(bd_catalog_copy_to_media(cat, vault), cat);
    bd_catalog_close(cat);
    CHECK(!contains(at("rk/k.brodalf"), "second passphrase") && !contains(at("rk/k.brodalf"), key));
    CHECK(bd_catalog_open_with_recovery(at("rk/k.brodalf"), wrong, &cat) == BD_ERR_PASSPHRASE);
    REQUIRE_OK(bd_catalog_open_with_recovery(at("rk/k.brodalf"), key, &cat), NULL);
    CHECK(bd_catalog_is_unlocked(cat));
    bd_catalog_close(cat);
    REQUIRE_OK(bd_catalog_open_with(at("rk/k.brodalf"), "second passphrase", &cat), NULL);
    bd_catalog_close(cat);

    /* On a new PC: the catalog backup on the drive opens with the recovery key. */
    char backup[1400];
    REQUIRE_OK(bd_catalog_open_with(at("rk/k.brodalf"), "second passphrase", &cat), NULL);
    snprintf(backup, sizeof(backup), "rk/vault/BRODALF/%s/catalog-backup.brodalf", bd_catalog_uuid(cat));
    bd_catalog_close(cat);
    REQUIRE_OK(bd_catalog_recover(at(backup), at("rk/new.brodalf")), NULL);
    REQUIRE_OK(bd_catalog_open_with_recovery(at("rk/new.brodalf"), key, &cat), NULL);
    bd_check_stats cs;
    REQUIRE_OK(bd_media_connect(cat, at("rk/vault"), &vault, quiet, NULL), cat);
    bd_restore_stats rs;
    REQUIRE_OK(bd_restore(cat, src, NULL, at("rk/out"), &rs, quiet, NULL), cat);
    CHECK(rs.files_restored == 1 && file_equals(at("rk/out/src/note.txt"), "recover me", 10));

    /* The sheet: drives, folders, the key; never the passphrase. */
    REQUIRE_OK(bd_media_set_location(cat, vault, "Top <shelf> & co"), cat);
    REQUIRE_OK(bd_write_recovery_sheet(cat, at("rk/sheet.html"), key), cat);
    CHECK(contains(at("rk/sheet.html"), key) && contains(at("rk/sheet.html"), "Vault"));
    CHECK(contains(at("rk/sheet.html"), "Top &lt;shelf&gt; &amp; co") && !contains(at("rk/sheet.html"), "<shelf>"));
    CHECK(contains(at("rk/sheet.html"), "src: 1 file"));
    CHECK(!contains(at("rk/sheet.html"), "passphrase\"") && !contains(at("rk/sheet.html"), "second passphrase"));
    REQUIRE_OK(bd_write_recovery_sheet(cat, at("rk/sheet2.html"), NULL), cat);
    CHECK(!contains(at("rk/sheet2.html"), key) && contains(at("rk/sheet2.html"), "no recovery key"));

    /* A new key replaces the old one. */
    REQUIRE_OK(bd_catalog_make_recovery_key(cat, key2), cat);
    CHECK(strcmp(key, key2) != 0);
    bd_catalog_lock_key(cat);
    CHECK(bd_catalog_unlock_recovery(cat, key) == BD_ERR_PASSPHRASE);
    REQUIRE_OK(bd_catalog_unlock_recovery(cat, key2), cat);
    bd_catalog_close(cat);
}

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
    REQUIRE_OK(bd_media_connect(cat, at("vault"), &vault, quiet, NULL), cat);
    REQUIRE_OK(bd_media_check(cat, vault, 0, &cs, quiet, NULL), cat);
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
    REQUIRE_OK(bd_media_connect(cat, at("vault"), &vault, quiet, NULL), cat);
    REQUIRE_OK(bd_restore(cat, 0, "secret.txt", at("restored3"), &rs, quiet, NULL), cat);
    CHECK(rs.files_restored == 1);
    CHECK(file_equals(at("restored3/Docs/secret.txt"), "the plans moved to the safe", 27));

    /* Turning catalog encryption off writes a plain file again. */
    REQUIRE_OK(bd_catalog_set_file_encrypted(cat, 0), cat);
    REQUIRE_OK(bd_catalog_save(cat), cat);
    CHECK(!bd_catalog_file_needs_passphrase(at("c.brodalf")));
    bd_catalog_close(cat);
    test_recovery_key();

    /* No random numbers for a new key: nothing is made or stored. */
    REQUIRE_OK(bd_catalog_create(at("r.brodalf"), &cat), NULL);
    bd_test_random_fail = 1;
    CHECK(bd_catalog_set_passphrase(cat, PASS) == BD_ERR_IO);
    CHECK(!bd_catalog_has_passphrase(cat) && !bd_catalog_is_unlocked(cat));
    bd_test_random_fail = 0;
    bd_catalog_close(cat);

    /* Nor for a new salt: the old key block stays. */
    REQUIRE_OK(bd_catalog_open(at("c.brodalf"), &cat), NULL);
    REQUIRE_OK(bd_catalog_unlock(cat, PASS2), cat);
    uint8_t before[BD_KEY_BLOCK], after[BD_KEY_BLOCK];
    CHECK(bd_catalog_key_block(cat, before) == 0);
    bd_test_random_fail = 1;
    CHECK(bd_catalog_set_passphrase(cat, "yet another passphrase") == BD_ERR_IO);
    CHECK(bd_catalog_key_block(cat, after) == 0 && memcmp(before, after, sizeof(before)) == 0);
    CHECK(bd_catalog_is_unlocked(cat));

    /* Nor for a sealed copy's nonce: those copies fail and leave nothing
     * on the drive (secret.txt changed; big.bin and exact.bin were found
     * damaged above and are copied again). */
    REQUIRE_OK(bd_media_connect(cat, at("vault"), &vault, quiet, NULL), cat);
    write_file(at("src/Docs/secret.txt"), "the plans are gone");
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_backup(cat, vault, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 0 && bs.files_failed == 3 && bs.versions_moved == 0);
    int tmp_files = 0;
    bd_walk(at("vault"), count_tmp_cb, NULL, &tmp_files);
    CHECK(tmp_files == 0);

    /* Nor for saving the catalog encrypted: the file on disk stays as it was. */
    REQUIRE_OK(bd_catalog_set_file_encrypted(cat, 1), cat);
    static char saved[300000];
    long saved_n = read_all(at("c.brodalf"), saved, sizeof(saved));
    CHECK(saved_n > 0 && saved_n < (long)sizeof(saved));
    CHECK(bd_catalog_save(cat) == BD_ERR_IO);
    CHECK(file_equals(at("c.brodalf"), saved, (size_t)saved_n) && !exists(at("c.brodalf.saving")));
    bd_test_random_fail = 0;
    REQUIRE_OK(bd_backup(cat, vault, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 3 && bs.files_failed == 0 && bs.versions_moved == 1);
    REQUIRE_OK(bd_media_check(cat, vault, 1, &cs, quiet, NULL), cat);
    CHECK(cs.bad == 0 && cs.missing == 0 && cs.ok == cs.copies);

    /* Argon2 settings far from what BRODALF writes (64 MiB, 3 passes) are
     * refused before any work: a damaged or crafted block could otherwise
     * ask for 4 GiB or hours. */
    static const uint32_t bad[][2] = {{1u << 22, 3}, {2u << 20, 3}, {1024, 3}, {65536, 0}, {65536, 17}, {65536, 1000000}};
    uint8_t key[32], blk[BD_KEY_BLOCK];
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        memcpy(blk, before, sizeof(blk));
        put_le32(blk + 16, bad[i][0]);
        put_le32(blk + 20, bad[i][1]);
        CHECK(bd_unwrap_key_block(PASS2, blk, key) == -2);
    }
    CHECK(bd_unwrap_key_block(PASS2, before, key) == 0); /* the app's own settings */
    char hex[BD_KEY_BLOCK * 2 + 1], good_hex[BD_KEY_BLOCK * 2 + 1];
    hex_of(before, sizeof(before), good_hex);
    memcpy(blk, before, sizeof(blk));
    put_le32(blk + 16, 1u << 22);
    hex_of(blk, sizeof(blk), hex);
    CHECK(bd_setting_set(cat, "key_block", hex) == 0);
    bd_catalog_lock_key(cat);
    CHECK(bd_catalog_unlock(cat, PASS2) == BD_ERR_FORMAT);
    CHECK(strstr(bd_catalog_error(cat), "not ones BRODALF writes") != NULL);
    CHECK(!bd_catalog_is_unlocked(cat));
    CHECK(bd_setting_set(cat, "key_block", good_hex) == 0);
    REQUIRE_OK(bd_catalog_unlock(cat, PASS2), cat);

    /* The same in an encrypted catalog file's header. */
    REQUIRE_OK(bd_catalog_save(cat), cat);
    bd_catalog_close(cat);
    long cn = read_all(at("c.brodalf"), saved, sizeof(saved));
    CHECK(cn > 16 + BD_KEY_BLOCK);
    put_le32((uint8_t *)saved + 16 + 20, 64); /* passes */
    write_bytes(at("c.brodalf"), saved, (size_t)cn);
    CHECK(bd_catalog_open_with(at("c.brodalf"), PASS2, &cat) == BD_ERR_FORMAT);
    CHECK(strstr(bd_open_error(), "not ones BRODALF writes") != NULL);
    CHECK(!exists(at("c.brodalf.lock")));
    put_le32((uint8_t *)saved + 16 + 20, 3);
    write_bytes(at("c.brodalf"), saved, (size_t)cn);
    REQUIRE_OK(bd_catalog_open_with(at("c.brodalf"), PASS2, &cat), NULL);
    bd_catalog_close(cat);

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("all encryption checks passed");
    return 0;
}
