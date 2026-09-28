/* OneDrive and Dropbox against tests/mock_cloud.py: sign in through a fake
 * browser, back up (small and chunked uploads), keep old versions, check,
 * restore, survive expired tokens and rate limits, reconnect from the saved
 * sign-in, sign out. Run as: mock_cloud.py test_cloud */
#include "brodalf.h"
#include "http.h"
#include "internal.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

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
static const char *mock;

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
    if (!f || fwrite(data, 1, n, f) != n) { fprintf(stderr, "cannot write %s\n", path); exit(1); }
    fclose(f);
}

static void write_file(const char *path, const char *text) { write_bytes(path, text, strlen(text)); }

static int same_file(const char *a, const char *b)
{
    FILE *fa = bd_fopen(a, "rb"), *fb = bd_fopen(b, "rb");
    int same = fa && fb;
    while (same) {
        char x[65536], y[65536];
        size_t na = fread(x, 1, sizeof(x), fa), nb = fread(y, 1, sizeof(y), fb);
        if (na != nb || memcmp(x, y, na) != 0) same = 0;
        if (na == 0) break;
    }
    if (fa) fclose(fa);
    if (fb) fclose(fb);
    return same;
}

/* GET a mock control URL; returns the body (caller frees). */
static char *control(const char *what)
{
    char url[1024], err[256];
    snprintf(url, sizeof(url), "%s/control/%s", mock, what);
    bd_http_req req = {"GET", url, NULL, NULL, 0, NULL, 0, 0, NULL, 1};
    bd_http_resp r;
    if (bd_http(&req, &r, err, sizeof(err)) != 0) { fprintf(stderr, "control %s: %s\n", what, err); exit(1); }
    return r.body;
}

static long stat_of(const char *key)
{
    char *s = control("stats");
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\": ", key);
    char *p = strstr(s, pat);
    long v = p ? atol(p + strlen(pat)) : -1;
    free(s);
    return v;
}

/* Plays the browser: opens the sign-in address and follows the redirect
 * back to BRODALF's loopback listener. */
static pid_t fake_browser(const char *url)
{
    pid_t pid = fork();
    if (pid == 0) {
        char err[256];
        for (int i = 0; i < 50; i++) {
            usleep(100000);
            bd_http_req req = {"GET", url, NULL, NULL, 0, NULL, 0, 0, NULL, 0};
            bd_http_resp r;
            if (bd_http(&req, &r, err, sizeof(err)) == 0) {
                int ok = r.status == 200 && r.body && strstr(r.body, "Signed in");
                bd_http_free(&r);
                _exit(ok ? 0 : 3);
            }
        }
        _exit(2);
    }
    return pid;
}

static bd_signin *sign_in(bd_catalog *cat, bd_cloud_provider prov)
{
    bd_signin *si;
    const char *url;
    REQUIRE_OK(bd_cloud_signin_begin(cat, prov, &si, &url), cat);
    CHECK(strstr(url, "code_challenge_method=S256") != NULL);
    pid_t pid = fake_browser(url);
    REQUIRE_OK(bd_cloud_signin_finish(cat, si, 15000), cat);
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    return si;
}

static void quiet(void *ctx, const char *msg)
{
    (void)ctx;
    if (getenv("TEST_VERBOSE")) printf("  %s\n", msg);
}

/* Size of BRODALF/<uuid>/<rel> (or its sealed .bdenc form) in a tree
 * listing, -1 when absent. */
static long tree_size(const char *tree, const char *uuid, const char *rel)
{
    char key[512];
    for (int sealed = 0; sealed < 2; sealed++) {
        snprintf(key, sizeof(key), "%s/%s%s\": ", uuid, rel, sealed ? BD_SEALED_SUFFIX : "");
        const char *hit = strstr(tree, key);
        if (hit) return atol(hit + strlen(key));
    }
    return -1;
}

static void run_provider(bd_cloud_provider prov, const char *name, const char *tree_key, const char *expect_account,
                         int encrypt)
{
    printf("== %s%s\n", name, encrypt ? " (encrypted)" : "");
    char dir[64];
    snprintf(dir, sizeof(dir), "%s", name);

    /* Source: two small files and one that needs a chunked upload. */
    size_t big_n = 12 * 1024 * 1024 + 12345;
    unsigned char *big = malloc(big_n);
    uint32_t x = 12345;
    for (size_t i = 0; i < big_n; i++) { x = x * 1103515245u + 12345u; big[i] = (unsigned char)(x >> 16); }
    char p[512];
    snprintf(p, sizeof(p), "%s/src/Docs/big.bin", dir);
    write_bytes(at(p), big, big_n);
    free(big);
    snprintf(p, sizeof(p), "%s/src/Docs/a.txt", dir);
    write_file(at(p), "hello");
    snprintf(p, sizeof(p), "%s/src/Docs/sub/caf\xc3\xa9 \xe2\x98\x95.txt", dir);
    write_file(at(p), "unicode name");

    bd_catalog *cat;
    snprintf(p, sizeof(p), "%s/test.brodalf", dir);
    char cat_path[1024];
    snprintf(cat_path, sizeof(cat_path), "%s", at(p));
    REQUIRE_OK(bd_catalog_create(cat_path, &cat), NULL);
    int64_t src;
    snprintf(p, sizeof(p), "%s/src/Docs", dir);
    REQUIRE_OK(bd_source_add(cat, at(p), &src), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_new == 3);

    /* Sign in and add the account as storage. */
    bd_signin *si = sign_in(cat, prov);
    CHECK(strcmp(bd_cloud_signin_account(si), expect_account) == 0);
    int64_t media;
    if (encrypt) REQUIRE_OK(bd_catalog_set_passphrase(cat, "correct horse"), cat);
    REQUIRE_OK(bd_cloud_add(cat, si, "My Cloud", encrypt ? BD_MEDIA_ENCRYPTED : 0, &media), cat);
    bd_cloud_signin_free(si);

    /* A second sign-in to the same account is recognised. */
    si = sign_in(cat, prov);
    CHECK(bd_cloud_add(cat, si, "Again", 0, NULL) == BD_ERR_EXISTS);
    bd_cloud_signin_free(si);

    /* Back up, with the API throttling us once along the way. */
    free(control("throttle?n=1"));
    long chunks_before = stat_of("chunks");
    bd_backup_stats bs;
    REQUIRE_OK(bd_backup(cat, media, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 3);
    CHECK(bs.files_failed == 0);
    CHECK(stat_of("throttled") >= 1);
    CHECK(stat_of("chunks") - chunks_before >= 2);

    char q[256];
    snprintf(q, sizeof(q), "tree?provider=%s&p=BRODALF/%s/", tree_key, bd_catalog_uuid(cat));
    char *tree = control(q);
    const char *uuid = bd_catalog_uuid(cat);
    /* Sealed copies carry a 40-byte header and a 16-byte tag per 64 KiB. */
    CHECK(tree_size(tree, uuid, "Docs/a.txt") == (encrypt ? 5 + 56 : 5));
    CHECK(tree_size(tree, uuid, "Docs/big.bin") == (encrypt ? 12595257 + 40 + 193 * 16 : 12595257));
    CHECK(tree_size(tree, uuid, "Docs/sub/caf\\u00e9 \\u2615.txt") == (encrypt ? 12 + 56 : 12));
    CHECK(strstr(tree, "brodalf-tmp") == NULL);
    free(tree);

    /* Expired access tokens are refreshed without fuss. */
    free(control("expire"));
    long refresh_before = stat_of("refresh");
    bd_check_stats cs;
    REQUIRE_OK(bd_media_check(cat, media, 0, &cs, quiet, NULL), cat);
    CHECK(cs.copies == 3 && cs.ok == 3);
    CHECK(stat_of("refresh") == refresh_before + 1);

    /* Full check downloads and hashes everything. */
    REQUIRE_OK(bd_media_check(cat, media, 1, &cs, quiet, NULL), cat);
    CHECK(cs.copies == 3 && cs.ok == 3 && cs.bad == 0);

    /* Change a file: the old copy moves into .versions. */
    snprintf(p, sizeof(p), "%s/src/Docs/a.txt", dir);
    write_file(at(p), "hello, version two");
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_backup(cat, media, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 1 && bs.versions_moved == 1);
    tree = control(q);
    CHECK(tree_size(tree, uuid, "Docs/a.txt") == (encrypt ? 18 + 56 : 18));
    CHECK(strstr(tree, ".versions/") != NULL);
    free(tree);

    /* Save, close, reopen: the saved sign-in reconnects (Microsoft has
     * rotated the refresh token by now, so this also proves we kept it). */
    REQUIRE_OK(bd_catalog_copy_to_media(cat, media), cat);
    bd_catalog_close(cat);
    REQUIRE_OK(bd_catalog_open(cat_path, &cat), NULL);
    if (encrypt) REQUIRE_OK(bd_catalog_unlock(cat, "correct horse"), cat);
    REQUIRE_OK(bd_cloud_connect(cat, media, &cs, quiet, NULL), cat);
    CHECK(cs.copies == 4 && cs.ok == 4); /* includes the old version in .versions */

    /* Damage a copy in the cloud: the quick check sees the hash change. */
    snprintf(q, sizeof(q), "corrupt?provider=%s&p=BRODALF/%s/Docs/sub/caf%%C3%%A9%%20%%E2%%98%%95.txt%s", tree_key,
             bd_catalog_uuid(cat), encrypt ? BD_SEALED_SUFFIX : "");
    free(control(q));
    REQUIRE_OK(bd_media_check(cat, media, 0, &cs, quiet, NULL), cat);
    CHECK(cs.bad == 1 && cs.ok == 3);
    REQUIRE_OK(bd_backup(cat, media, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 1);

    /* Restore everything and compare. */
    bd_restore_stats rs;
    snprintf(p, sizeof(p), "%s/restore", dir);
    char restore_root[1024];
    snprintf(restore_root, sizeof(restore_root), "%s", at(p));
    REQUIRE_OK(bd_restore(cat, src, NULL, restore_root, &rs, quiet, NULL), cat);
    CHECK(rs.files_restored == 3 && rs.files_failed == 0);
    char a1[1100], a2[1100];
    snprintf(a1, sizeof(a1), "%s/src/Docs/big.bin", dir);
    snprintf(a2, sizeof(a2), "%s/restore/Docs/big.bin", dir);
    CHECK(same_file(at(a1), at(a2)));
    snprintf(a1, sizeof(a1), "%s/src/Docs/a.txt", dir);
    snprintf(a2, sizeof(a2), "%s/restore/Docs/a.txt", dir);
    CHECK(same_file(at(a1), at(a2)));

    /* Signing in again to the same account finds it already set up. */
    si = sign_in(cat, prov);
    CHECK(bd_cloud_add(cat, si, "Again", 0, NULL) == BD_ERR_EXISTS);
    bd_cloud_signin_free(si);

    /* Sign out: the saved sign-in is gone, reconnecting asks to sign in. */
    REQUIRE_OK(bd_cloud_sign_out(cat, media), cat);
    CHECK(bd_cloud_connect(cat, media, &cs, quiet, NULL) == BD_ERR_PASSPHRASE);
    bd_catalog_close(cat);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    mock = getenv("BRODALF_CLOUD_TEST_URL");
    if (!mock) {
        fprintf(stderr, "run this through tests/mock_cloud.py\n");
        return 77;
    }
    char tmp[512], id[37];
    bd_temp_dir(tmp, sizeof(tmp));
    bd_uuid_v4(id);
    snprintf(base, sizeof(base), "%s/brodalf-cloud-%.8s", tmp, id);
    bd_mkdirs(base);
    printf("working in %s\n", base);
    setenv("BRODALF_SECRETS_DIR", at("secrets"), 1);
    char port[16];
    snprintf(port, sizeof(port), "%d", 40000 + (int)(getpid() % 20000));
    setenv("BRODALF_OAUTH_PORT", port, 1);

    run_provider(BD_CLOUD_ONEDRIVE, "onedrive", "ms", "tester@outlook.com", 0);
    run_provider(BD_CLOUD_DROPBOX, "dropbox", "db", "tester@example.com", 1);

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("all cloud checks passed\n");
    return 0;
}
