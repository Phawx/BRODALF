/* BLAKE3 checksums must not change with the SIMD paths: official test
 * vectors (BLAKE3 1.5.4, test_vectors/test_vectors.json, "hash" field, first
 * 32 bytes), hashed in one go, in odd-sized pieces and through
 * bd_hash_file; and every SIMD path the CPU has against the portable code. */
#include "brodalf.h"
#include "internal.h"

#include "blake3.h"
#include "blake3_impl.h"

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

/* The input is 0, 1, ..., 250, 0, 1, ... */
static const struct { size_t len; const char *hash; } vectors[] = {
    {0, "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262"},
    {1, "2d3adedff11b61f14c886e35afa036736dcd87a74d27b5c1510225d0f592e213"},
    {1023, "10108970eeda3eb932baac1428c7a2163b0e924c9a9e25b35bba72b28f70bd11"},
    {1024, "42214739f095a406f3fc83deb889744ac00df831c10daa55189b5d121c855af7"},
    {1025, "d00278ae47eb27b34faecf67b4fe263f82d5412916c1ffd97c8cb7fb814b8444"},
    {2048, "e776b6028c7cd22a4d0ba182a8bf62205d2ef576467e838ed6f2529b85fba24a"},
    {3072, "b98cb0ff3623be03326b373de6b9095218513e64f1ee2edd2525c7ad1e5cffd2"},
    {8192, "aae792484c8efe4f19e2ca7d371d8c467ffb10748d8a5a1ae579948f718a2a63"},
    {31744, "62b6960e1a44bcc1eb1a611a8d6235b6b4b78f32e7abc4fb4c6cdcce94895c47"},
    {102400, "bc3e3d41a1146b069abffad3c0d44860cf664390afce4d9661f7902e7943e085"},
};

static void to_hex(const uint8_t *b, size_t n, char *out)
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = hex[b[i] >> 4];
        out[i * 2 + 1] = hex[b[i] & 15];
    }
    out[n * 2] = '\0';
}

static void hash_hex(const uint8_t *data, size_t len, size_t piece, char out[65])
{
    blake3_hasher h;
    blake3_hasher_init(&h);
    for (size_t off = 0; off < len || (len == 0 && off == 0); off += piece) {
        size_t n = len - off < piece ? len - off : piece;
        blake3_hasher_update(&h, data + off, n);
        if (len == 0) break;
    }
    uint8_t digest[BLAKE3_OUT_LEN];
    blake3_hasher_finalize(&h, digest, sizeof(digest));
    to_hex(digest, sizeof(digest), out);
}

typedef void (*hash_many_fn)(const uint8_t *const *inputs, size_t num_inputs, size_t blocks, const uint32_t key[8],
                             uint64_t counter, bool increment_counter, uint8_t flags, uint8_t flags_start,
                             uint8_t flags_end, uint8_t *out);

/* 16 whole chunks at once, which is what the wide paths are for. */
static void same_as_portable(const char *name, hash_many_fn fn, const uint8_t *data)
{
    const uint8_t *inputs[16];
    for (int i = 0; i < 16; i++) inputs[i] = data + (size_t)i * BLAKE3_CHUNK_LEN;
    uint8_t want[16 * BLAKE3_OUT_LEN], got[16 * BLAKE3_OUT_LEN];
    for (int n = 1; n <= 16; n++) {
        blake3_hash_many_portable(inputs, (size_t)n, BLAKE3_CHUNK_LEN / BLAKE3_BLOCK_LEN, IV, 7, true, 0, CHUNK_START,
                                  CHUNK_END, want);
        memset(got, 0, sizeof(got));
        fn(inputs, (size_t)n, BLAKE3_CHUNK_LEN / BLAKE3_BLOCK_LEN, IV, 7, true, 0, CHUNK_START, CHUNK_END, got);
        if (memcmp(want, got, (size_t)n * BLAKE3_OUT_LEN) != 0) {
            fprintf(stderr, "%s differs from the portable code for %d inputs\n", name, n);
            failures++;
            return;
        }
    }
    printf("%s matches the portable code\n", name);
}

static void check_simd_paths(const uint8_t *data)
{
    (void)data;
#if defined(IS_X86) && (defined(__GNUC__) || defined(__clang__))
    __builtin_cpu_init();
#if !defined(BLAKE3_NO_SSE2)
    if (__builtin_cpu_supports("sse2")) same_as_portable("sse2", blake3_hash_many_sse2, data);
#endif
#if !defined(BLAKE3_NO_SSE41)
    if (__builtin_cpu_supports("sse4.1")) same_as_portable("sse4.1", blake3_hash_many_sse41, data);
#endif
#if !defined(BLAKE3_NO_AVX2)
    if (__builtin_cpu_supports("avx2")) same_as_portable("avx2", blake3_hash_many_avx2, data);
#endif
#if !defined(BLAKE3_NO_AVX512)
    if (__builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512vl"))
        same_as_portable("avx512", blake3_hash_many_avx512, data);
#endif
#endif
    printf("SIMD degree in use: %u\n", (unsigned)blake3_simd_degree());
}

int main(void)
{
    size_t max = 102400;
    uint8_t *data = malloc(max);
    if (!data) return 1;
    for (size_t i = 0; i < max; i++) data[i] = (uint8_t)(i % 251);

    char tmp[512], id[37];
    bd_temp_dir(tmp, sizeof(tmp));
    bd_uuid_v4(id);
    char *file = bd_sprintf("%s/brodalf-blake3-%.8s.bin", tmp, id);

    for (size_t v = 0; v < sizeof(vectors) / sizeof(vectors[0]); v++) {
        size_t len = vectors[v].len;
        char hex[65];
        hash_hex(data, len, len ? len : 1, hex);
        CHECK(strcmp(hex, vectors[v].hash) == 0);
        hash_hex(data, len, 1000, hex); /* pieces that do not line up with chunks */
        CHECK(strcmp(hex, vectors[v].hash) == 0);
        hash_hex(data, len, 1, hex);
        CHECK(strcmp(hex, vectors[v].hash) == 0);

        /* The app's own path: a file read in big blocks. */
        FILE *f = file ? bd_fopen(file, "wb") : NULL;
        CHECK(f != NULL);
        if (f) {
            if (len) fwrite(data, 1, len, f);
            fclose(f);
            char got[BD_HASH_HEX_LEN + 1];
            int64_t size = -1;
            CHECK(bd_hash_file(file, NULL, got, &size) == 0);
            CHECK(size == (int64_t)len);
            CHECK(strcmp(got, vectors[v].hash) == 0);
        }
    }
    if (file) bd_remove(file);
    free(file);

    uint8_t *chunks = malloc(16 * BLAKE3_CHUNK_LEN);
    if (!chunks) return 1;
    for (size_t i = 0; i < 16 * BLAKE3_CHUNK_LEN; i++) chunks[i] = (uint8_t)(i * 7 + (i >> 9));
    check_simd_paths(chunks);
    free(chunks);
    free(data);

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("all BLAKE3 checks passed");
    return 0;
}
