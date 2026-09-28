/* Optional encryption.
 *
 * A catalog with a passphrase has a random 32-byte master key. The key is
 * stored in the catalog wrapped (XChaCha20-Poly1305) by a key derived from
 * the passphrase with Argon2id, so the passphrase can change without
 * re-encrypting anything. A wrong passphrase fails to unwrap.
 *
 * Encrypted files ("sealed" streams) look like this:
 *
 *   bytes 0-7    magic "BRDLFENC"
 *   bytes 8-11   stream version, little endian (1)
 *   bytes 12-15  chunk size, little endian (65536)
 *   bytes 16-39  random 24-byte nonce
 *   then chunks: 16-byte MAC + ciphertext
 *
 * Every chunk holds exactly chunk-size bytes of plaintext except the last,
 * which holds fewer (possibly none). Each chunk is authenticated with the
 * header and a "last chunk" flag, and the stream rekeys after every chunk,
 * so reordering, truncation and tampering are all detected. */
#include "internal.h"

#include "blake3.h"
#include "monocypher.h"

#include <stdlib.h>
#include <string.h>

#define SEAL_MAGIC "BRDLFENC"
#define SEAL_VERSION 1u
#define SEAL_CHUNK 65536u
#define SEAL_HEADER 40
#define SEAL_MAC 16

#define KDF_MEM_KIB 65536u /* 64 MiB */
#define KDF_PASSES 3u

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---- Sealed streams ---------------------------------------------------- */

struct bd_sealer {
    FILE *out;
    crypto_aead_ctx ctx;
    uint8_t ad[SEAL_HEADER + 1];
    uint8_t *plain, *cipher;
    size_t fill;
    int err;
};

static void seal_emit(bd_sealer *s, int last)
{
    uint8_t mac[SEAL_MAC];
    s->ad[SEAL_HEADER] = (uint8_t)last;
    crypto_aead_write(&s->ctx, s->cipher, mac, s->ad, sizeof(s->ad), s->plain, s->fill);
    if (fwrite(mac, 1, SEAL_MAC, s->out) != SEAL_MAC || fwrite(s->cipher, 1, s->fill, s->out) != s->fill) s->err = -2;
    s->fill = 0;
}

bd_sealer *bd_seal_begin(FILE *out, const uint8_t key[32])
{
    bd_sealer *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->plain = malloc(SEAL_CHUNK);
    s->cipher = malloc(SEAL_CHUNK);
    if (!s->plain || !s->cipher) { free(s->plain); free(s->cipher); free(s); return NULL; }
    s->out = out;
    memcpy(s->ad, SEAL_MAGIC, 8);
    put_u32(s->ad + 8, SEAL_VERSION);
    put_u32(s->ad + 12, SEAL_CHUNK);
    bd_random_bytes(s->ad + 16, 24);
    crypto_aead_init_x(&s->ctx, key, s->ad + 16);
    if (fwrite(s->ad, 1, SEAL_HEADER, out) != SEAL_HEADER) s->err = -2;
    return s;
}

int bd_seal_write(bd_sealer *s, const void *data, size_t n)
{
    const uint8_t *p = data;
    while (n > 0 && !s->err) {
        if (s->fill == SEAL_CHUNK) seal_emit(s, 0);
        size_t take = SEAL_CHUNK - s->fill;
        if (take > n) take = n;
        memcpy(s->plain + s->fill, p, take);
        s->fill += take;
        p += take;
        n -= take;
    }
    return s->err;
}

int bd_seal_end(bd_sealer *s)
{
    if (!s) return -2;
    if (!s->err) {
        /* A full buffer is never the last chunk; the last one is short. */
        if (s->fill == SEAL_CHUNK) seal_emit(s, 0);
        seal_emit(s, 1);
    }
    int err = s->err;
    crypto_wipe(&s->ctx, sizeof(s->ctx));
    crypto_wipe(s->plain, SEAL_CHUNK);
    free(s->plain);
    free(s->cipher);
    free(s);
    return err;
}

struct bd_opener {
    FILE *in;
    crypto_aead_ctx ctx;
    uint8_t ad[SEAL_HEADER + 1];
    uint8_t *plain, *cipher;
    size_t pos, len;
    int done;
};

bd_opener *bd_open_begin(FILE *in, const uint8_t key[32])
{
    bd_opener *o = calloc(1, sizeof(*o));
    if (!o) return NULL;
    if (fread(o->ad, 1, SEAL_HEADER, in) != SEAL_HEADER || memcmp(o->ad, SEAL_MAGIC, 8) != 0 ||
        get_u32(o->ad + 8) != SEAL_VERSION || get_u32(o->ad + 12) != SEAL_CHUNK) {
        free(o);
        return NULL;
    }
    o->plain = malloc(SEAL_CHUNK);
    o->cipher = malloc(SEAL_CHUNK + SEAL_MAC);
    if (!o->plain || !o->cipher) { free(o->plain); free(o->cipher); free(o); return NULL; }
    o->in = in;
    crypto_aead_init_x(&o->ctx, key, o->ad + 16);
    return o;
}

long bd_open_read(bd_opener *o, void *buf, size_t cap)
{
    if (o->pos == o->len) {
        if (o->done) return 0;
        size_t n = fread(o->cipher, 1, SEAL_CHUNK + SEAL_MAC, o->in);
        if (ferror(o->in)) return -1;
        if (n < SEAL_MAC) return -3; /* truncated */
        size_t plain_len = n - SEAL_MAC;
        int last = plain_len < SEAL_CHUNK;
        if (last && fgetc(o->in) != EOF) return -3;
        o->ad[SEAL_HEADER] = (uint8_t)last;
        if (crypto_aead_read(&o->ctx, o->plain, o->cipher, o->ad, sizeof(o->ad), o->cipher + SEAL_MAC, plain_len) != 0)
            return -3;
        o->pos = 0;
        o->len = plain_len;
        o->done = last;
        if (plain_len == 0) return 0;
    }
    size_t take = o->len - o->pos;
    if (take > cap) take = cap;
    memcpy(buf, o->plain + o->pos, take);
    o->pos += take;
    return (long)take;
}

void bd_open_end(bd_opener *o)
{
    if (!o) return;
    crypto_wipe(&o->ctx, sizeof(o->ctx));
    crypto_wipe(o->plain, SEAL_CHUNK);
    free(o->plain);
    free(o->cipher);
    free(o);
}

int bd_sink_write(bd_sink *k, const void *data, size_t n)
{
    if (k->sealer) return bd_seal_write(k->sealer, data, n);
    return fwrite(data, 1, n, k->file) == n ? 0 : -2;
}

long bd_source_read(bd_source *k, void *buf, size_t cap)
{
    if (k->opener) return bd_open_read(k->opener, buf, cap);
    size_t n = fread(buf, 1, cap, k->file);
    if (n == 0 && ferror(k->file)) return -1;
    return (long)n;
}

/* ---- Hash while copying ------------------------------------------------ */

int bd_hash_copy(const char *path, const uint8_t *src_key, FILE *copy_to, const uint8_t *dst_key,
                 char hex_out[BD_HASH_HEX_LEN + 1], int64_t *size_out)
{
    FILE *f = bd_fopen(path, "rb");
    if (!f) return -1;
    enum { BUF = 1 << 20 };
    unsigned char *buf = malloc(BUF);
    bd_source src = {f, NULL};
    bd_sink dst = {copy_to, NULL};
    int rc = 0;
    if (!buf) rc = -1;
    if (!rc && src_key && !(src.opener = bd_open_begin(f, src_key))) rc = -3;
    if (!rc && copy_to && dst_key && !(dst.sealer = bd_seal_begin(copy_to, dst_key))) rc = -2;

    blake3_hasher h;
    blake3_hasher_init(&h);
    int64_t total = 0;
    while (!rc) {
        long n = bd_source_read(&src, buf, BUF);
        if (n < 0) { rc = (int)n; break; }
        if (n == 0) break;
        blake3_hasher_update(&h, buf, (size_t)n);
        total += n;
        if (copy_to && bd_sink_write(&dst, buf, (size_t)n) != 0) rc = -2;
    }
    if (dst.sealer && bd_seal_end(dst.sealer) != 0 && !rc) rc = -2;
    bd_open_end(src.opener);
    if (buf) { crypto_wipe(buf, BUF); free(buf); }
    fclose(f);
    if (rc != 0) return rc;

    uint8_t out[BLAKE3_OUT_LEN];
    blake3_hasher_finalize(&h, out, BLAKE3_OUT_LEN);
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < BLAKE3_OUT_LEN; i++) {
        hex_out[i * 2] = hex[out[i] >> 4];
        hex_out[i * 2 + 1] = hex[out[i] & 15];
    }
    hex_out[BD_HASH_HEX_LEN] = '\0';
    if (size_out) *size_out = total;
    return 0;
}

/* ---- Keys ---------------------------------------------------------------- */

static int derive_kek(const char *pass, const uint8_t salt[16], uint32_t mem_kib, uint32_t passes, uint8_t kek[32])
{
    if (mem_kib < 8 || mem_kib > (1u << 22) || passes < 1 || passes > 64) return -1;
    void *work = malloc((size_t)mem_kib * 1024);
    if (!work) return -1;
    crypto_argon2_config config = {CRYPTO_ARGON2_ID, mem_kib, passes, 1};
    crypto_argon2_inputs inputs = {(const uint8_t *)pass, salt, (uint32_t)strlen(pass), 16};
    crypto_argon2(kek, 32, work, config, inputs, crypto_argon2_no_extras);
    crypto_wipe(work, (size_t)mem_kib * 1024);
    free(work);
    return 0;
}

/* Key block: salt(16) mem_kib(4) passes(4) nonce(24) mac(16) wrapped key(32). */
static void wrap_key(const uint8_t kek[32], const uint8_t key[32], uint8_t block[BD_KEY_BLOCK])
{
    bd_random_bytes(block + 24, 24);
    crypto_aead_lock(block + 64, block + 48, kek, block + 24, block, 24, key, 32);
}

static int unwrap_key(const char *pass, const uint8_t block[BD_KEY_BLOCK], uint8_t key[32])
{
    uint8_t kek[32];
    if (derive_kek(pass, block, get_u32(block + 16), get_u32(block + 20), kek) != 0) return -1;
    int rc = crypto_aead_unlock(key, block + 48, kek, block + 24, block, 24, block + 64, 32);
    crypto_wipe(kek, sizeof(kek));
    return rc == 0 ? 0 : -3;
}

static int make_key_block(const char *pass, const uint8_t key[32], uint8_t block[BD_KEY_BLOCK])
{
    uint8_t kek[32];
    bd_random_bytes(block, 16);
    put_u32(block + 16, KDF_MEM_KIB);
    put_u32(block + 20, KDF_PASSES);
    if (derive_kek(pass, block, KDF_MEM_KIB, KDF_PASSES, kek) != 0) return -1;
    wrap_key(kek, key, block);
    crypto_wipe(kek, sizeof(kek));
    return 0;
}

int bd_unwrap_key_block(const char *pass, const uint8_t block[BD_KEY_BLOCK], uint8_t key[32])
{
    return unwrap_key(pass, block, key);
}

static void to_hex(const uint8_t *b, size_t n, char *out)
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = hex[b[i] >> 4];
        out[i * 2 + 1] = hex[b[i] & 15];
    }
    out[n * 2] = '\0';
}

static int from_hex(const char *s, uint8_t *b, size_t n)
{
    if (!s || strlen(s) != n * 2) return -1;
    for (size_t i = 0; i < n; i++) {
        int v = 0;
        for (int j = 0; j < 2; j++) {
            char c = s[i * 2 + j];
            int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            if (d < 0) return -1;
            v = v * 16 + d;
        }
        b[i] = (uint8_t)v;
    }
    return 0;
}

static int setting_get(bd_catalog *cat, const char *key, char *out, size_t cap)
{
    sqlite3_stmt *q;
    int found = 0;
    if (sqlite3_prepare_v2(cat->db, "SELECT value FROM settings WHERE key=?", -1, &q, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(q, 1, key, -1, SQLITE_STATIC);
    if (sqlite3_step(q) == SQLITE_ROW) {
        snprintf(out, cap, "%s", (const char *)sqlite3_column_text(q, 0));
        found = 1;
    }
    sqlite3_finalize(q);
    return found;
}

static int setting_set(bd_catalog *cat, const char *key, const char *value)
{
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(cat->db, "INSERT OR REPLACE INTO settings(key, value) VALUES(?,?)", -1, &q, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(q, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_text(q, 2, value, -1, SQLITE_STATIC);
    int rc = sqlite3_step(q) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(q);
    return rc;
}

int bd_catalog_key_block(bd_catalog *cat, uint8_t block[BD_KEY_BLOCK])
{
    char hex[BD_KEY_BLOCK * 2 + 1];
    if (!setting_get(cat, "key_block", hex, sizeof(hex))) return -1;
    return from_hex(hex, block, BD_KEY_BLOCK);
}

int bd_catalog_has_passphrase(bd_catalog *cat)
{
    uint8_t block[BD_KEY_BLOCK];
    return bd_catalog_key_block(cat, block) == 0;
}

int bd_catalog_is_unlocked(bd_catalog *cat)
{
    return cat->have_key;
}

void bd_catalog_set_key(bd_catalog *cat, const uint8_t key[32])
{
    memcpy(cat->key, key, 32);
    cat->have_key = 1;
}

static bd_status store_block(bd_catalog *cat, const uint8_t block[BD_KEY_BLOCK])
{
    char hex[BD_KEY_BLOCK * 2 + 1];
    to_hex(block, BD_KEY_BLOCK, hex);
    return setting_set(cat, "key_block", hex) == 0 ? BD_OK : bd_fail_db(cat, "store the encryption key");
}

bd_status bd_catalog_set_passphrase(bd_catalog *cat, const char *passphrase)
{
    if (!passphrase || strlen(passphrase) < 8)
        return bd_fail(cat, BD_ERR_INVALID, "use a passphrase of at least 8 characters");
    if (bd_catalog_has_passphrase(cat) && !cat->have_key)
        return bd_fail(cat, BD_ERR_PASSPHRASE, "unlock the catalog with its current passphrase first");
    uint8_t key[32], block[BD_KEY_BLOCK];
    if (cat->have_key) memcpy(key, cat->key, 32); /* changing the passphrase keeps the key */
    else bd_random_bytes(key, 32);
    if (make_key_block(passphrase, key, block) != 0) {
        crypto_wipe(key, sizeof(key));
        return bd_fail(cat, BD_ERR_NOMEM, "not enough memory to derive the key");
    }
    bd_status s = store_block(cat, block);
    if (s == BD_OK) bd_catalog_set_key(cat, key);
    crypto_wipe(key, sizeof(key));
    return s;
}

bd_status bd_catalog_unlock(bd_catalog *cat, const char *passphrase)
{
    uint8_t block[BD_KEY_BLOCK], key[32];
    if (bd_catalog_key_block(cat, block) != 0) return bd_fail(cat, BD_ERR_INVALID, "this catalog has no passphrase");
    int rc = unwrap_key(passphrase ? passphrase : "", block, key);
    if (rc == -1) return bd_fail(cat, BD_ERR_NOMEM, "not enough memory to derive the key");
    if (rc != 0) return bd_fail(cat, BD_ERR_PASSPHRASE, "wrong passphrase");
    bd_catalog_set_key(cat, key);
    crypto_wipe(key, sizeof(key));
    return BD_OK;
}

void bd_catalog_lock_key(bd_catalog *cat)
{
    crypto_wipe(cat->key, sizeof(cat->key));
    cat->have_key = 0;
}

int bd_catalog_file_encrypted(bd_catalog *cat)
{
    char v[8];
    return setting_get(cat, "encrypt_catalog", v, sizeof(v)) && strcmp(v, "1") == 0;
}

bd_status bd_catalog_set_file_encrypted(bd_catalog *cat, int on)
{
    if (on && !cat->have_key) return bd_fail(cat, BD_ERR_PASSPHRASE, "set or enter the passphrase first");
    return setting_set(cat, "encrypt_catalog", on ? "1" : "0") == 0 ? BD_OK : bd_fail_db(cat, "change catalog encryption");
}

void bd_wipe(void *p, size_t n)
{
    crypto_wipe(p, n);
}
