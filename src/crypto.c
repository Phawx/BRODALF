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
/* What a key block may ask for. BRODALF writes 64 MiB and 3 passes (and
 * always uses one lane, which the block does not store); anything far
 * outside that was not written by it, and could make opening take hours
 * or all the memory there is. */
#define KDF_MIN_MEM_KIB 8192u    /* 8 MiB */
#define KDF_MAX_MEM_KIB 1048576u /* 1 GiB */
#define KDF_MIN_PASSES 1u
#define KDF_MAX_PASSES 16u

int bd_test_random_fail = 0;

/* Random bytes for keys, salts and nonces. Never use the buffer if this
 * fails: a predictable key or a repeated nonce gives the data away. */
static int random_bytes(void *buf, size_t n)
{
    if (bd_test_random_fail) return -1;
    return bd_random_bytes(buf, n);
}

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
    size_t fill, high; /* high: most plaintext the buffer has held */
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
    if (random_bytes(s->ad + 16, 24) != 0) {
        /* Nothing is written. */
        free(s->plain);
        free(s->cipher);
        free(s);
        return NULL;
    }
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
        if (s->fill > s->high) s->high = s->fill;
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
    crypto_wipe(s->plain, s->high); /* only what held plaintext: a small file is quick */
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
    size_t pos, len, high;
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
        if (plain_len > o->high) o->high = plain_len;
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
    crypto_wipe(o->plain, o->high);
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

enum { COPY_BUF = 1 << 20, COPY_RING = 3, COPY_THREAD_AFTER = 4 };

/* For a big copy, a writer thread writes (and seals) one block while this
 * thread reads and hashes the next. Blocks go through a ring of buffers in
 * order, so the copy holds exactly the bytes that were hashed. Small files
 * never get here: the thread starts after COPY_THREAD_AFTER full blocks. */
typedef struct {
    bd_sink *dst;
    unsigned char *buf[COPY_RING];
    size_t len[COPY_RING];
    int64_t produced, consumed; /* blocks handed over, blocks written */
    int eof, err;
    bd_mutex *m;
    bd_cond *cv;
    bd_thread *thread;
} copy_pipe;

static void pipe_writer(void *arg)
{
    copy_pipe *p = arg;
    bd_mutex_lock(p->m);
    for (;;) {
        while (p->consumed == p->produced && !p->eof) bd_cond_wait(p->cv, p->m);
        if (p->consumed == p->produced) break; /* all written */
        int i = (int)(p->consumed % COPY_RING);
        bd_mutex_unlock(p->m);
        int rc = bd_sink_write(p->dst, p->buf[i], p->len[i]);
        bd_mutex_lock(p->m);
        if (rc != 0) p->err = 1;
        else p->consumed++;
        bd_cond_broadcast(p->cv);
        if (p->err) break;
    }
    bd_mutex_unlock(p->m);
}

static void pipe_free(copy_pipe *p)
{
    for (int i = 0; i < COPY_RING; i++)
        if (p->buf[i]) { crypto_wipe(p->buf[i], COPY_BUF); free(p->buf[i]); }
    bd_cond_free(p->cv);
    bd_mutex_free(p->m);
}

static int pipe_start(copy_pipe *p, bd_sink *dst)
{
    memset(p, 0, sizeof(*p));
    p->dst = dst;
    int ok = (p->m = bd_mutex_new()) != NULL && (p->cv = bd_cond_new()) != NULL;
    for (int i = 0; ok && i < COPY_RING; i++) ok = (p->buf[i] = malloc(COPY_BUF)) != NULL;
    if (ok) ok = (p->thread = bd_thread_start(pipe_writer, p)) != NULL;
    if (!ok) pipe_free(p); /* carry on without the thread */
    return ok ? 0 : -1;
}

/* The buffer to read the next block into, or NULL once the writer failed. */
static unsigned char *pipe_slot(copy_pipe *p)
{
    bd_mutex_lock(p->m);
    while (p->produced - p->consumed == COPY_RING && !p->err) bd_cond_wait(p->cv, p->m);
    unsigned char *b = p->err ? NULL : p->buf[p->produced % COPY_RING];
    bd_mutex_unlock(p->m);
    return b;
}

static void pipe_push(copy_pipe *p, size_t n)
{
    bd_mutex_lock(p->m);
    p->len[p->produced % COPY_RING] = n;
    p->produced++;
    bd_cond_broadcast(p->cv);
    bd_mutex_unlock(p->m);
}

/* Let the writer finish what it was given. -2 if a write failed. */
static int pipe_finish(copy_pipe *p)
{
    bd_mutex_lock(p->m);
    p->eof = 1;
    bd_cond_broadcast(p->cv);
    bd_mutex_unlock(p->m);
    bd_thread_join(p->thread);
    int err = p->err;
    pipe_free(p);
    return err ? -2 : 0;
}

/* A whole block where there is one: a sealed source gives a chunk a time. */
static long read_block(bd_source *src, unsigned char *b, size_t cap)
{
    if (!src->opener) return bd_source_read(src, b, cap); /* fread fills it */
    size_t got = 0;
    while (got < cap) {
        long n = bd_source_read(src, b + got, cap - got);
        if (n < 0) return n;
        if (n == 0) break;
        got += (size_t)n;
    }
    return (long)got;
}

int bd_hash_copy(const char *path, const uint8_t *src_key, FILE *copy_to, const uint8_t *dst_key,
                 char hex_out[BD_HASH_HEX_LEN + 1], int64_t *size_out)
{
    FILE *f = bd_fopen(path, "rb");
    if (!f) return -1;
    unsigned char *buf = malloc(COPY_BUF);
    bd_source src = {f, NULL};
    bd_sink dst = {copy_to, NULL};
    int rc = 0;
    if (!buf) rc = -1;
    if (!rc && src_key && !(src.opener = bd_open_begin(f, src_key))) rc = -3;
    if (!rc && copy_to && dst_key && !(dst.sealer = bd_seal_begin(copy_to, dst_key))) rc = -2;

    blake3_hasher h;
    blake3_hasher_init(&h);
    int64_t total = 0;
    size_t high = 0; /* most of buf that was used: wiping all 1 MiB per small file was most of a backup's time */
    int blocks = 0, piped = 0;
    copy_pipe pipe;
    while (!rc) {
        if (bd_cancel_pending()) { rc = -4; break; }
        unsigned char *b = piped ? pipe_slot(&pipe) : buf;
        if (!b) break; /* the writer failed; pipe_finish says so */
        long n = read_block(&src, b, COPY_BUF);
        if (n < 0) { rc = (int)n; break; }
        if (n == 0) break;
        if (b == buf && (size_t)n > high) high = (size_t)n;
        blake3_hasher_update(&h, b, (size_t)n);
        total += n;
        bd_io_tick(n);
        if (!copy_to) continue;
        if (piped) { pipe_push(&pipe, (size_t)n); continue; }
        if (bd_sink_write(&dst, b, (size_t)n) != 0) { rc = -2; break; }
        if (++blocks == COPY_THREAD_AFTER && n == COPY_BUF && pipe_start(&pipe, &dst) == 0) piped = 1;
    }
    /* A failed write was of an earlier block than any failed read, so it is
     * what the serial loop would have stopped at. */
    if (piped && pipe_finish(&pipe) != 0) rc = -2;
    if (dst.sealer && bd_seal_end(dst.sealer) != 0 && !rc) rc = -2;
    bd_open_end(src.opener);
    if (buf) { crypto_wipe(buf, high); free(buf); }
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

/* ---- One read, several copies ------------------------------------------- */

enum { MULTI_MAX = 16 };

/* Like copy_pipe, with one writer thread per copy. A block's buffer is
 * reused only once every copy still being written has written it. */
typedef struct multi_pipe multi_pipe;
typedef struct {
    multi_pipe *p;
    int index;
} multi_writer;

struct multi_pipe {
    bd_sink *dst;
    int n;
    unsigned char *buf[COPY_RING];
    size_t len[COPY_RING];
    int64_t produced;
    int64_t consumed[MULTI_MAX];
    int err[MULTI_MAX];
    int eof;
    bd_mutex *m;
    bd_cond *cv;
    bd_thread *thread[MULTI_MAX];
    multi_writer w[MULTI_MAX];
};

static void multi_writer_run(void *arg)
{
    multi_writer *w = arg;
    multi_pipe *p = w->p;
    int i = w->index;
    bd_mutex_lock(p->m);
    for (;;) {
        while (p->consumed[i] == p->produced && !p->eof) bd_cond_wait(p->cv, p->m);
        if (p->consumed[i] == p->produced) break;
        int slot = (int)(p->consumed[i] % COPY_RING);
        bd_mutex_unlock(p->m);
        int rc = bd_sink_write(&p->dst[i], p->buf[slot], p->len[slot]);
        bd_mutex_lock(p->m);
        if (rc != 0) p->err[i] = 1;
        else p->consumed[i]++;
        bd_cond_broadcast(p->cv);
        if (p->err[i]) break;
    }
    bd_mutex_unlock(p->m);
}

/* Blocks every copy still being written has written; -1 if none is left. */
static int64_t multi_low(const multi_pipe *p)
{
    int64_t low = -1;
    for (int i = 0; i < p->n; i++)
        if (!p->err[i] && (low < 0 || p->consumed[i] < low)) low = p->consumed[i];
    return low;
}

static void multi_free(multi_pipe *p)
{
    for (int i = 0; i < COPY_RING; i++)
        if (p->buf[i]) { crypto_wipe(p->buf[i], COPY_BUF); free(p->buf[i]); }
    bd_cond_free(p->cv);
    bd_mutex_free(p->m);
}

/* Let the writers finish; their failures land in failed[]. */
static void multi_finish(multi_pipe *p, int *failed)
{
    bd_mutex_lock(p->m);
    p->eof = 1;
    bd_cond_broadcast(p->cv);
    bd_mutex_unlock(p->m);
    for (int i = 0; i < p->n; i++)
        if (p->thread[i]) bd_thread_join(p->thread[i]);
    for (int i = 0; i < p->n; i++)
        if (p->err[i]) failed[i] = 1;
    multi_free(p);
}

/* Copies that already failed get no writer. */
static int multi_start(multi_pipe *p, bd_sink *dst, int n, const int *failed)
{
    memset(p, 0, sizeof(*p));
    p->dst = dst;
    p->n = n;
    int ok = (p->m = bd_mutex_new()) != NULL && (p->cv = bd_cond_new()) != NULL;
    for (int i = 0; ok && i < COPY_RING; i++) ok = (p->buf[i] = malloc(COPY_BUF)) != NULL;
    if (!ok) { multi_free(p); return -1; }
    for (int i = 0; i < n; i++) {
        p->w[i].p = p;
        p->w[i].index = i;
        if (failed[i]) { p->err[i] = 1; continue; }
        if (!(p->thread[i] = bd_thread_start(multi_writer_run, &p->w[i]))) {
            /* Stop the ones that started; the copy carries on without threads. */
            int none[MULTI_MAX] = {0};
            multi_finish(p, none);
            return -1;
        }
    }
    return 0;
}

static unsigned char *multi_slot(multi_pipe *p)
{
    bd_mutex_lock(p->m);
    int64_t low;
    while ((low = multi_low(p)) >= 0 && p->produced - low == COPY_RING) bd_cond_wait(p->cv, p->m);
    unsigned char *b = low < 0 ? NULL : p->buf[p->produced % COPY_RING];
    bd_mutex_unlock(p->m);
    return b;
}

static void multi_push(multi_pipe *p, size_t n)
{
    bd_mutex_lock(p->m);
    p->len[p->produced % COPY_RING] = n;
    p->produced++;
    bd_cond_broadcast(p->cv);
    bd_mutex_unlock(p->m);
}

int bd_hash_copy_multi(const char *path, FILE *const *copy_to, const uint8_t *const *dst_keys, int n,
                       char hex_out[BD_HASH_HEX_LEN + 1], int64_t *sizes, int *failed)
{
    if (n < 1 || n > MULTI_MAX) return -1;
    for (int i = 0; i < n; i++) { failed[i] = 0; sizes[i] = 0; }
    if (n == 1) {
        int rc = bd_hash_copy(path, NULL, copy_to[0], dst_keys[0], hex_out, &sizes[0]);
        if (rc == -2) { failed[0] = 1; return -2; }
        return rc;
    }
    FILE *f = bd_fopen(path, "rb");
    if (!f) return -1;
    unsigned char *buf = malloc(COPY_BUF);
    bd_sink dst[MULTI_MAX];
    bd_source src = {f, NULL};
    int rc = buf ? 0 : -1;
    for (int i = 0; i < n; i++) {
        dst[i].file = copy_to[i];
        dst[i].sealer = NULL;
        if (!rc && dst_keys[i] && !(dst[i].sealer = bd_seal_begin(copy_to[i], dst_keys[i]))) failed[i] = 1;
    }
    blake3_hasher h;
    blake3_hasher_init(&h);
    int64_t total = 0;
    size_t high = 0;
    int blocks = 0, piped = 0;
    multi_pipe pipe;
    while (!rc) {
        if (bd_cancel_pending()) { rc = -4; break; }
        unsigned char *b = piped ? multi_slot(&pipe) : buf;
        if (!b) break; /* every writer failed */
        long got = read_block(&src, b, COPY_BUF);
        if (got < 0) { rc = (int)got; break; }
        if (got == 0) break;
        if (b == buf && (size_t)got > high) high = (size_t)got;
        blake3_hasher_update(&h, b, (size_t)got);
        total += got;
        bd_io_tick(got);
        if (piped) { multi_push(&pipe, (size_t)got); continue; }
        int left = 0;
        for (int i = 0; i < n; i++) {
            if (failed[i]) continue;
            if (bd_sink_write(&dst[i], b, (size_t)got) != 0) failed[i] = 1;
            else left++;
        }
        if (!left) break;
        if (++blocks == COPY_THREAD_AFTER && got == COPY_BUF && multi_start(&pipe, dst, n, failed) == 0) piped = 1;
    }
    if (piped) multi_finish(&pipe, failed);
    int left = 0;
    for (int i = 0; i < n; i++) {
        if (dst[i].sealer && bd_seal_end(dst[i].sealer) != 0) failed[i] = 1;
        if (!failed[i]) { sizes[i] = total; left++; }
    }
    if (buf) { crypto_wipe(buf, high); free(buf); }
    fclose(f);
    if (rc != 0) return rc;
    if (!left) return -2;

    uint8_t out[BLAKE3_OUT_LEN];
    blake3_hasher_finalize(&h, out, BLAKE3_OUT_LEN);
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < BLAKE3_OUT_LEN; i++) {
        hex_out[i * 2] = hex[out[i] >> 4];
        hex_out[i * 2 + 1] = hex[out[i] & 15];
    }
    hex_out[BD_HASH_HEX_LEN] = '\0';
    return 0;
}

/* ---- How fast this PC encrypts ------------------------------------------- */

int64_t bd_crypto_speed(void)
{
    static int64_t measured = 0;
    if (measured > 0) return measured;
    enum { SAMPLE = 4 << 20 };
    uint8_t key[32], nonce[24], mac[16], ad[8] = {0};
    unsigned char *buf = malloc(SAMPLE);
    if (!buf) return 0;
    memset(buf, 0x5a, SAMPLE);
    memset(key, 7, sizeof(key));
    memset(nonce, 9, sizeof(nonce));
    int64_t start = bd_now_ms(), bytes = 0, now;
    /* About a quarter of a second of sealing in 64 KiB chunks, the size
     * the copy format uses. */
    do {
        for (size_t off = 0; off < SAMPLE; off += 65536)
            crypto_aead_lock(buf + off, mac, key, nonce, ad, sizeof(ad), buf + off, 65536);
        bytes += SAMPLE;
        now = bd_now_ms();
    } while (now - start < 250 && bytes < ((int64_t)4 << 30));
    free(buf);
    crypto_wipe(key, sizeof(key));
    int64_t ms = now - start;
    if (ms < 1) ms = 1;
    measured = bytes * 1000 / ms;
    return measured;
}

/* ---- Keys ---------------------------------------------------------------- */

/* 0, -1 out of memory, -2 settings out of range. */
static int derive_kek(const char *pass, const uint8_t salt[16], uint32_t mem_kib, uint32_t passes, uint8_t kek[32])
{
    if (mem_kib < KDF_MIN_MEM_KIB || mem_kib > KDF_MAX_MEM_KIB || passes < KDF_MIN_PASSES || passes > KDF_MAX_PASSES)
        return -2;
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
static int wrap_key(const uint8_t kek[32], const uint8_t key[32], uint8_t block[BD_KEY_BLOCK])
{
    if (random_bytes(block + 24, 24) != 0) return -4;
    crypto_aead_lock(block + 64, block + 48, kek, block + 24, block, 24, key, 32);
    return 0;
}

/* 0, -1 out of memory, -2 the block's settings are out of range, -3 wrong
 * passphrase. */
static int unwrap_key(const char *pass, const uint8_t block[BD_KEY_BLOCK], uint8_t key[32])
{
    uint8_t kek[32];
    int d = derive_kek(pass, block, get_u32(block + 16), get_u32(block + 20), kek);
    if (d != 0) return d;
    int rc = crypto_aead_unlock(key, block + 48, kek, block + 24, block, 24, block + 64, 32);
    crypto_wipe(kek, sizeof(kek));
    return rc == 0 ? 0 : -3;
}

/* 0, -1 out of memory, -4 no random numbers. */
static int make_key_block(const char *pass, const uint8_t key[32], uint8_t block[BD_KEY_BLOCK])
{
    uint8_t kek[32];
    if (random_bytes(block, 16) != 0) return -4;
    put_u32(block + 16, KDF_MEM_KIB);
    put_u32(block + 20, KDF_PASSES);
    if (derive_kek(pass, block, KDF_MEM_KIB, KDF_PASSES, kek) != 0) return -1;
    int rc = wrap_key(kek, key, block);
    crypto_wipe(kek, sizeof(kek));
    return rc;
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

int bd_setting_get(bd_catalog *cat, const char *key, char *out, size_t cap)
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

int bd_setting_set(bd_catalog *cat, const char *key, const char *value)
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
    if (!bd_setting_get(cat, "key_block", hex, sizeof(hex))) return -1;
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
    return bd_setting_set(cat, "key_block", hex) == 0 ? BD_OK : bd_fail_db(cat, "store the encryption key");
}

/* The revision the next save gets, under which: a copy of the database of
 * that revision or later has the new passphrase or recovery key, an older
 * one the one it had then (bd_media_copy_has_key). */
static void note_key_change(bd_catalog *cat, const char *which)
{
    char v[32];
    snprintf(v, sizeof(v), "%lld", (long long)(bd_catalog_revision(cat) + 1));
    bd_setting_set(cat, which, v);
}

bd_status bd_catalog_set_passphrase(bd_catalog *cat, const char *passphrase)
{
    if (!passphrase || strlen(passphrase) < 8)
        return bd_fail(cat, BD_ERR_INVALID, "use a passphrase of at least 8 characters");
    if (bd_catalog_has_passphrase(cat) && !cat->have_key)
        return bd_fail(cat, BD_ERR_PASSPHRASE, "unlock the catalog with its current passphrase first");
    uint8_t key[32], block[BD_KEY_BLOCK];
    int rc = 0;
    if (cat->have_key) memcpy(key, cat->key, 32); /* changing the passphrase keeps the key */
    else if (random_bytes(key, 32) != 0) rc = -4;
    if (rc == 0) rc = make_key_block(passphrase, key, block);
    if (rc != 0) {
        /* Nothing was stored; the catalog is as it was. */
        crypto_wipe(key, sizeof(key));
        crypto_wipe(block, sizeof(block));
        return rc == -4 ? bd_fail(cat, BD_ERR_IO, "cannot make a key: the system gave no random numbers; nothing was changed")
                        : bd_fail(cat, BD_ERR_NOMEM, "not enough memory to derive the key");
    }
    bd_status s = store_block(cat, block);
    if (s == BD_OK) {
        bd_catalog_set_key(cat, key);
        note_key_change(cat, "passphrase_rev");
    }
    crypto_wipe(key, sizeof(key));
    return s;
}

bd_status bd_catalog_unlock(bd_catalog *cat, const char *passphrase)
{
    uint8_t block[BD_KEY_BLOCK], key[32];
    if (bd_catalog_key_block(cat, block) != 0) return bd_fail(cat, BD_ERR_INVALID, "this catalog has no passphrase");
    int rc = unwrap_key(passphrase ? passphrase : "", block, key);
    if (rc == -1) return bd_fail(cat, BD_ERR_NOMEM, "not enough memory to derive the key");
    if (rc == -2) return bd_fail(cat, BD_ERR_FORMAT, BD_KEY_BLOCK_BAD);
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
    return bd_setting_get(cat, "encrypt_catalog", v, sizeof(v)) && strcmp(v, "1") == 0;
}

bd_status bd_catalog_set_file_encrypted(bd_catalog *cat, int on)
{
    if (on && !cat->have_key) return bd_fail(cat, BD_ERR_PASSPHRASE, "set or enter the passphrase first");
    /* From now on nothing of it goes to the disk unencrypted. Turned off,
     * it stays in memory until it is closed. */
    if (on) {
        bd_status s = bd_catalog_keep_in_memory(cat);
        if (s != BD_OK) return s;
    }
    return bd_setting_set(cat, "encrypt_catalog", on ? "1" : "0") == 0 ? BD_OK : bd_fail_db(cat, "change catalog encryption");
}

void bd_wipe(void *p, size_t n)
{
    crypto_wipe(p, n);
}

/* ---- Recovery key -------------------------------------------------------- *
 * A second way to unlock: 30 random characters from an alphabet with no
 * look-alikes (150 bits), shown once for the recovery sheet. The master key
 * is wrapped by it exactly as by the passphrase (Argon2id, then
 * XChaCha20-Poly1305) and stored as "recovery_block" beside "key_block"; an
 * encrypted catalog file carries both blocks in its header. Only the
 * wrapped key is kept, never the recovery key itself. */

static const char RECOVERY_ALPHABET[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ"; /* no 0, 1, I or O */
#define RECOVERY_CHARS 30
#define RECOVERY_GROUP 5

/* The 30 characters of a recovery key as typed: any case, with or without
 * dashes and spaces. 0, or -1 if it cannot be a recovery key. */
static int recovery_normalize(const char *in, char out[RECOVERY_CHARS + 1])
{
    size_t n = 0;
    for (; in && *in; in++) {
        char c = *in;
        if (c == '-' || c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (n == RECOVERY_CHARS || !strchr(RECOVERY_ALPHABET, c)) { crypto_wipe(out, n); return -1; }
        out[n++] = c;
    }
    out[n] = '\0';
    if (n == RECOVERY_CHARS) return 0;
    crypto_wipe(out, n);
    return -1;
}

int bd_unwrap_recovery_block(const char *recovery_key, const uint8_t block[BD_KEY_BLOCK], uint8_t key[32])
{
    char norm[RECOVERY_CHARS + 1];
    if (recovery_normalize(recovery_key, norm) != 0) return -3;
    int rc = unwrap_key(norm, block, key);
    crypto_wipe(norm, sizeof(norm));
    return rc;
}

int bd_catalog_recovery_block(bd_catalog *cat, uint8_t block[BD_KEY_BLOCK])
{
    char hex[BD_KEY_BLOCK * 2 + 1];
    if (!bd_setting_get(cat, "recovery_block", hex, sizeof(hex))) return -1;
    return from_hex(hex, block, BD_KEY_BLOCK);
}

int bd_catalog_has_recovery_key(bd_catalog *cat)
{
    uint8_t block[BD_KEY_BLOCK];
    return bd_catalog_recovery_block(cat, block) == 0;
}

bd_status bd_catalog_make_recovery_key(bd_catalog *cat, char out[BD_RECOVERY_KEY_SIZE])
{
    out[0] = '\0';
    if (!bd_catalog_has_passphrase(cat)) return bd_fail(cat, BD_ERR_INVALID, "set a passphrase first");
    if (!cat->have_key) return bd_fail(cat, BD_ERR_PASSPHRASE, "unlock the catalog with its passphrase first");
    uint8_t r[RECOVERY_CHARS], block[BD_KEY_BLOCK];
    char norm[RECOVERY_CHARS + 1];
    if (bd_random_bytes(r, sizeof(r)) != 0) return bd_fail(cat, BD_ERR_IO, "the system could not supply random numbers");
    for (int i = 0; i < RECOVERY_CHARS; i++) norm[i] = RECOVERY_ALPHABET[r[i] & 31]; /* 256 is a multiple of 32: no bias */
    norm[RECOVERY_CHARS] = '\0';
    crypto_wipe(r, sizeof(r));
    int mk = make_key_block(norm, cat->key, block);
    if (mk != 0) {
        crypto_wipe(norm, sizeof(norm));
        return mk == -4 ? bd_fail(cat, BD_ERR_IO, "the system could not supply random numbers")
                        : bd_fail(cat, BD_ERR_NOMEM, "not enough memory to derive the key");
    }
    char hex[BD_KEY_BLOCK * 2 + 1];
    to_hex(block, BD_KEY_BLOCK, hex);
    if (bd_setting_set(cat, "recovery_block", hex) != 0) {
        crypto_wipe(norm, sizeof(norm));
        return bd_fail_db(cat, "store the recovery key");
    }
    note_key_change(cat, "recovery_key_rev");
    size_t j = 0;
    for (int i = 0; i < RECOVERY_CHARS; i++) {
        if (i && i % RECOVERY_GROUP == 0) out[j++] = '-';
        out[j++] = norm[i];
    }
    out[j] = '\0';
    crypto_wipe(norm, sizeof(norm));
    return BD_OK;
}

bd_status bd_catalog_unlock_recovery(bd_catalog *cat, const char *recovery_key)
{
    uint8_t block[BD_KEY_BLOCK], key[32];
    if (bd_catalog_recovery_block(cat, block) != 0) return bd_fail(cat, BD_ERR_INVALID, "this catalog has no recovery key");
    int rc = bd_unwrap_recovery_block(recovery_key, block, key);
    if (rc == -1) return bd_fail(cat, BD_ERR_NOMEM, "not enough memory to derive the key");
    if (rc == -2) return bd_fail(cat, BD_ERR_FORMAT, "%s", BD_KEY_BLOCK_BAD);
    if (rc != 0) return bd_fail(cat, BD_ERR_PASSPHRASE, "wrong recovery key");
    bd_catalog_set_key(cat, key);
    crypto_wipe(key, sizeof(key));
    return BD_OK;
}
