/* Cloud storage: OneDrive (Microsoft Graph) and Dropbox.
 *
 * Signing in is OAuth 2.0 with PKCE and a loopback redirect: BRODALF opens
 * the provider's page in the browser, the provider sends the browser back
 * to http://localhost:53682/ with a code, and BRODALF trades the code for
 * tokens. No client secret is involved. The refresh token is saved with
 * bd_secret_set (Credential Manager on Windows); access tokens live only in
 * memory.
 *
 * Both providers are used in "app folder" mode, so BRODALF sees only
 * Apps/BRODALF. Inside it the layout is the same as on a drive:
 * BRODALF/<catalog uuid>/...
 *
 * For tests, BRODALF_CLOUD_TEST_URL points every endpoint at a local mock
 * server (see tests/mock_cloud.py). */
#include "store.h"
#include "http.h"

#include "cJSON.h"
#include "monocypher.h"

#include <stdlib.h>
#include <string.h>

#ifndef BRODALF_ONEDRIVE_CLIENT_ID
#define BRODALF_ONEDRIVE_CLIENT_ID ""
#endif
#ifndef BRODALF_DROPBOX_CLIENT_ID
#define BRODALF_DROPBOX_CLIENT_ID ""
#endif

#define REDIRECT_PORT 53682
#define ONEDRIVE_SCOPE "offline_access Files.ReadWrite.AppFolder User.Read"
#define SIMPLE_UPLOAD_MAX (4 * 1024 * 1024)
#define ONEDRIVE_CHUNK (10 * 1024 * 1024) /* a multiple of 320 KiB */
#define DROPBOX_CHUNK (8 * 1024 * 1024)

/* ---- Providers and endpoints -------------------------------------------- */

typedef struct {
    bd_cloud_provider id;
    const char *kind;  /* media.kind */
    const char *title; /* for people */
    char authorize[512], token[512], api[512], content[512];
    char client_id[256];
} provider;

static int load_provider(bd_cloud_provider id, provider *p, char *err, size_t cap)
{
    memset(p, 0, sizeof(*p));
    p->id = id;
    const char *test = getenv("BRODALF_CLOUD_TEST_URL");
    const char *env_id;
    if (id == BD_CLOUD_ONEDRIVE) {
        p->kind = "onedrive";
        p->title = "OneDrive";
        env_id = getenv("BRODALF_ONEDRIVE_CLIENT_ID");
        snprintf(p->client_id, sizeof(p->client_id), "%s", env_id ? env_id : BRODALF_ONEDRIVE_CLIENT_ID);
        if (test) {
            snprintf(p->authorize, sizeof(p->authorize), "%s/ms/authorize", test);
            snprintf(p->token, sizeof(p->token), "%s/ms/token", test);
            snprintf(p->api, sizeof(p->api), "%s/graph/v1.0", test);
        } else {
            snprintf(p->authorize, sizeof(p->authorize), "https://login.microsoftonline.com/common/oauth2/v2.0/authorize");
            snprintf(p->token, sizeof(p->token), "https://login.microsoftonline.com/common/oauth2/v2.0/token");
            snprintf(p->api, sizeof(p->api), "https://graph.microsoft.com/v1.0");
        }
    } else if (id == BD_CLOUD_DROPBOX) {
        p->kind = "dropbox";
        p->title = "Dropbox";
        env_id = getenv("BRODALF_DROPBOX_CLIENT_ID");
        snprintf(p->client_id, sizeof(p->client_id), "%s", env_id ? env_id : BRODALF_DROPBOX_CLIENT_ID);
        if (test) {
            snprintf(p->authorize, sizeof(p->authorize), "%s/db/authorize", test);
            snprintf(p->token, sizeof(p->token), "%s/db/token", test);
            snprintf(p->api, sizeof(p->api), "%s/dbapi/2", test);
            snprintf(p->content, sizeof(p->content), "%s/dbcontent/2", test);
        } else {
            snprintf(p->authorize, sizeof(p->authorize), "https://www.dropbox.com/oauth2/authorize");
            snprintf(p->token, sizeof(p->token), "https://api.dropboxapi.com/oauth2/token");
            snprintf(p->api, sizeof(p->api), "https://api.dropboxapi.com/2");
            snprintf(p->content, sizeof(p->content), "https://content.dropboxapi.com/2");
        }
    } else {
        snprintf(err, cap, "unknown cloud provider");
        return -1;
    }
    if (!p->client_id[0]) {
        snprintf(err, cap, "this build of BRODALF has no %s app ID yet", p->title);
        return -1;
    }
    return 0;
}

static int provider_by_kind(const char *kind, bd_cloud_provider *out)
{
    if (strcmp(kind, "onedrive") == 0) { *out = BD_CLOUD_ONEDRIVE; return 0; }
    if (strcmp(kind, "dropbox") == 0) { *out = BD_CLOUD_DROPBOX; return 0; }
    return -1;
}

static int redirect_port(void)
{
    const char *p = getenv("BRODALF_OAUTH_PORT");
    return p ? atoi(p) : REDIRECT_PORT;
}

/* ---- Small helpers -------------------------------------------------------- */

/* SHA-256, only for the PKCE challenge. */
typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t fill; } sha256_ctx;

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(sha256_ctx *c, const uint8_t *p)
{
    uint32_t w[64], a, b, cc, d, e, f, g, h;
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void sha256(const void *data, size_t n, uint8_t out[32])
{
    sha256_ctx c = {{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19}, 0, {0}, 0};
    const uint8_t *p = data;
    c.len = (uint64_t)n * 8;
    while (n >= 64) { sha256_block(&c, p); p += 64; n -= 64; }
    uint8_t last[128] = {0};
    memcpy(last, p, n);
    last[n] = 0x80;
    size_t total = n + 1 + 8 <= 64 ? 64 : 128;
    for (int i = 0; i < 8; i++) last[total - 1 - i] = (uint8_t)(c.len >> (8 * i));
    sha256_block(&c, last);
    if (total == 128) sha256_block(&c, last + 64);
    for (int i = 0; i < 8; i++) {
        out[i * 4] = (uint8_t)(c.h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(c.h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c.h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)c.h[i];
    }
}

static void base64url(const uint8_t *in, size_t n, char *out)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = t[(v >> 18) & 63];
        out[o++] = t[(v >> 12) & 63];
        if (i + 1 < n) out[o++] = t[(v >> 6) & 63];
        if (i + 2 < n) out[o++] = t[v & 63];
    }
    out[o] = '\0';
}

/* JSON text that is pure ASCII (Dropbox-API-Arg is an HTTP header). */
static char *json_ascii(cJSON *j)
{
    char *raw = cJSON_PrintUnformatted(j);
    if (!raw) return NULL;
    size_t n = strlen(raw);
    char *out = malloc(n * 12 + 1), *o = out;
    const unsigned char *p = (const unsigned char *)raw;
    while (out && *p) {
        uint32_t cp;
        int len;
        if (*p < 0x80) { *o++ = (char)*p++; continue; }
        if ((*p & 0xE0) == 0xC0) { cp = *p & 0x1F; len = 1; }
        else if ((*p & 0xF0) == 0xE0) { cp = *p & 0x0F; len = 2; }
        else { cp = *p & 0x07; len = 3; }
        p++;
        for (int i = 0; i < len && (*p & 0xC0) == 0x80; i++) cp = cp << 6 | (*p++ & 0x3F);
        if (cp >= 0x10000) {
            cp -= 0x10000;
            o += sprintf(o, "\\u%04x\\u%04x", 0xD800 + (unsigned)(cp >> 10), 0xDC00 + (unsigned)(cp & 0x3FF));
        } else {
            o += sprintf(o, "\\u%04x", (unsigned)cp);
        }
    }
    if (out) *o = '\0';
    cJSON_free(raw);
    return out;
}

static char *form_value(const char *query, const char *key)
{
    size_t kl = strlen(key);
    for (const char *p = query; p && *p;) {
        const char *amp = strchr(p, '&');
        size_t len = amp ? (size_t)(amp - p) : strlen(p);
        if (len > kl && strncmp(p, key, kl) == 0 && p[kl] == '=') {
            char *v = malloc(len - kl);
            if (!v) return NULL;
            size_t o = 0;
            for (size_t i = kl + 1; i < len; i++) {
                if (p[i] == '%' && i + 2 < len) {
                    char hx[3] = {p[i + 1], p[i + 2], 0};
                    v[o++] = (char)strtol(hx, NULL, 16);
                    i += 2;
                } else {
                    v[o++] = p[i] == '+' ? ' ' : p[i];
                }
            }
            v[o] = '\0';
            return v;
        }
        p = amp ? amp + 1 : NULL;
    }
    return NULL;
}

static const char *jstr(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static int64_t jnum(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? (int64_t)v->valuedouble : 0;
}

static int64_t file_size(const char *path)
{
    bd_stat_t st;
    return bd_stat(path, &st) == 0 ? st.size : -1;
}

/* ---- Sessions --------------------------------------------------------------- */

typedef struct {
    char *path;
    char *id;
} folder_entry;

typedef struct cloud_session {
    struct cloud_session *next;
    int64_t media_id; /* 0 while only signed in */
    provider prov;
    char *access_token;
    int64_t expires_ms;
    char *refresh_token;
    char *secret_name; /* where the refresh token is saved, once it is */
    char account[256];
    folder_entry *folders; /* OneDrive folder ids we know */
    int n_folders;
    char err[512];
} cloud_session;

static void session_free(cloud_session *s)
{
    if (!s) return;
    free(s->access_token);
    if (s->refresh_token) { bd_wipe(s->refresh_token, strlen(s->refresh_token)); free(s->refresh_token); }
    free(s->secret_name);
    for (int i = 0; i < s->n_folders; i++) { free(s->folders[i].path); free(s->folders[i].id); }
    free(s->folders);
    free(s);
}

void bd_cloud_forget_all(bd_catalog *cat)
{
    cloud_session *s = cat->clouds;
    while (s) {
        cloud_session *n = s->next;
        session_free(s);
        s = n;
    }
    cat->clouds = NULL;
}

static cloud_session *find_session(bd_catalog *cat, int64_t media_id)
{
    for (cloud_session *s = cat->clouds; s; s = s->next)
        if (s->media_id == media_id) return s;
    return NULL;
}

static void keep_session(bd_catalog *cat, cloud_session *s)
{
    cloud_session **pp = (cloud_session **)&cat->clouds;
    while (*pp) {
        if ((*pp)->media_id == s->media_id) {
            cloud_session *old = *pp;
            *pp = old->next;
            session_free(old);
            continue;
        }
        pp = &(*pp)->next;
    }
    s->next = cat->clouds;
    cat->clouds = s;
}

/* Token endpoint call; fills access (and maybe a new refresh) token. */
static int token_request(cloud_session *s, const char *form)
{
    const char *hdr[] = {"Content-Type: application/x-www-form-urlencoded", "Accept: application/json", NULL};
    bd_http_req req = {"POST", s->prov.token, hdr, form, strlen(form), NULL, 0, 0, NULL, 1};
    bd_http_resp r;
    if (bd_http(&req, &r, s->err, sizeof(s->err)) != 0) return -1;
    cJSON *j = cJSON_Parse(r.body ? r.body : "");
    int rc = -1;
    if (r.status == 200 && j && jstr(j, "access_token")) {
        free(s->access_token);
        s->access_token = bd_strdup(jstr(j, "access_token"));
        int64_t expires = jnum(j, "expires_in");
        s->expires_ms = bd_now_ms() + (expires > 0 ? expires : 3600) * 1000;
        const char *refresh = jstr(j, "refresh_token");
        if (refresh && (!s->refresh_token || strcmp(refresh, s->refresh_token) != 0)) {
            if (s->refresh_token) { bd_wipe(s->refresh_token, strlen(s->refresh_token)); free(s->refresh_token); }
            s->refresh_token = bd_strdup(refresh);
            /* Microsoft hands out a new refresh token each time; keep it. */
            if (s->secret_name) bd_secret_set(s->secret_name, s->refresh_token);
        }
        rc = 0;
    } else {
        const char *why = j ? jstr(j, "error_description") : NULL;
        if (!why && j) why = jstr(j, "error");
        snprintf(s->err, sizeof(s->err), "%s sign-in failed (%d)%s%s", s->prov.title, r.status, why ? ": " : "", why ? why : "");
        if (r.status == 400 || r.status == 401) rc = -2; /* the saved sign-in is no longer valid */
    }
    cJSON_Delete(j);
    bd_http_free(&r);
    return rc;
}

static int refresh_access(cloud_session *s)
{
    if (!s->refresh_token) { snprintf(s->err, sizeof(s->err), "not signed in to %s", s->prov.title); return -2; }
    char *rt = bd_url_encode(s->refresh_token, 0), *cid = bd_url_encode(s->prov.client_id, 0);
    char *scope = bd_url_encode(ONEDRIVE_SCOPE, 0);
    char *form = rt && cid && scope
                     ? (s->prov.id == BD_CLOUD_ONEDRIVE
                            ? bd_sprintf("grant_type=refresh_token&refresh_token=%s&client_id=%s&scope=%s", rt, cid, scope)
                            : bd_sprintf("grant_type=refresh_token&refresh_token=%s&client_id=%s", rt, cid))
                     : NULL;
    int rc = form ? token_request(s, form) : -1;
    if (rt) { bd_wipe(rt, strlen(rt)); free(rt); }
    free(cid);
    free(scope);
    if (form) { bd_wipe(form, strlen(form)); free(form); }
    return rc;
}

/* An API call with the access token, refreshing it when it expires and
 * waiting out rate limits. extra: more headers. */
static int api(cloud_session *s, const char *method, const char *url, const char *const *extra, const char *json_body,
               const char *body_file, int64_t file_off, int64_t file_len, FILE *out, bd_http_resp *r)
{
    for (int attempt = 0; attempt < 6; attempt++) {
        if (!s->access_token || bd_now_ms() > s->expires_ms - 60000) {
            int rr = refresh_access(s);
            if (rr != 0) return -1;
        }
        const char *hdr[16];
        int n = 0;
        char *auth = bd_sprintf("Authorization: Bearer %s", s->access_token);
        hdr[n++] = auth;
        if (json_body) hdr[n++] = "Content-Type: application/json";
        for (const char *const *e = extra; e && *e && n < 14; e++) hdr[n++] = *e;
        hdr[n] = NULL;
        long start = out ? ftell(out) : 0;
        bd_http_req req = {method, url, hdr, json_body, json_body ? strlen(json_body) : 0, body_file, file_off, file_len, out, 0};
        int rc = auth ? bd_http(&req, r, s->err, sizeof(s->err)) : -1;
        if (auth) { bd_wipe(auth, strlen(auth)); free(auth); }
        if (rc != 0) {
            if (attempt < 2) { bd_sleep_ms(1000 * (attempt + 1)); if (out) fseek(out, start, SEEK_SET); continue; }
            return -1;
        }
        if (r->status == 401 && attempt == 0) {
            /* The token expired early or was revoked: refresh once. */
            bd_http_free(r);
            free(s->access_token);
            s->access_token = NULL;
            if (out) fseek(out, start, SEEK_SET);
            continue;
        }
        if (r->status == 429 || r->status == 503 || r->status == 502 || r->status == 504) {
            int wait = r->retry_after > 0 ? r->retry_after : 2 << attempt;
            if (wait > 60) wait = 60;
            bd_http_free(r);
            if (out) fseek(out, start, SEEK_SET);
            bd_sleep_ms(wait * 1000);
            continue;
        }
        return 0;
    }
    snprintf(s->err, sizeof(s->err), "%s is busy; try again later", s->prov.title);
    return -1;
}

static void api_error(cloud_session *s, const bd_http_resp *r, const char *what)
{
    cJSON *j = cJSON_Parse(r->body ? r->body : "");
    const char *msg = NULL;
    if (j) {
        const cJSON *e = cJSON_GetObjectItemCaseSensitive(j, "error");
        msg = cJSON_IsObject(e) ? jstr(e, "message") : jstr(j, "error_summary");
    }
    snprintf(s->err, sizeof(s->err), "%s: %s returned %d%s%s", what, s->prov.title, r->status, msg ? ", " : "", msg ? msg : "");
    cJSON_Delete(j);
}

/* ---- OneDrive (Microsoft Graph, app folder) ---------------------------------------- */

/* Paths inside the app folder: BRODALF/<catalog uuid>/<rel>. */
static char *od_item_url(cloud_session *s, const char *path, const char *suffix)
{
    char *enc = bd_url_encode(path, 1);
    char *u = enc ? bd_sprintf("%s/me/drive/special/approot:/%s:%s", s->prov.api, enc, suffix ? suffix : "") : NULL;
    free(enc);
    return u;
}

static void od_stat_from(const cJSON *item, bd_remote_stat *st)
{
    memset(st, 0, sizeof(*st));
    st->size = jnum(item, "size");
    const cJSON *file = cJSON_GetObjectItemCaseSensitive(item, "file");
    const cJSON *hashes = file ? cJSON_GetObjectItemCaseSensitive(file, "hashes") : NULL;
    const char *h = hashes ? jstr(hashes, "quickXorHash") : NULL;
    if (!h && hashes) h = jstr(hashes, "sha256Hash");
    if (!h && hashes) h = jstr(hashes, "sha1Hash");
    if (h) snprintf(st->rev, sizeof(st->rev), "%s", h);
}

static int od_stat(cloud_session *s, const char *path, bd_remote_stat *st, char **id_out)
{
    char *url = od_item_url(s, path, NULL);
    bd_http_resp r;
    int rc = url ? api(s, "GET", url, NULL, NULL, NULL, 0, 0, NULL, &r) : -1;
    free(url);
    if (rc != 0) return -1;
    if (r.status == 404) { bd_http_free(&r); return 1; }
    if (r.status != 200) { api_error(s, &r, path); bd_http_free(&r); return -1; }
    cJSON *j = cJSON_Parse(r.body);
    bd_http_free(&r);
    if (!j) { snprintf(s->err, sizeof(s->err), "unreadable answer from OneDrive"); return -1; }
    int is_file = cJSON_GetObjectItemCaseSensitive(j, "file") != NULL;
    if (st) od_stat_from(j, st);
    if (id_out) *id_out = jstr(j, "id") ? bd_strdup(jstr(j, "id")) : NULL;
    cJSON_Delete(j);
    return (st && !is_file) ? 1 : 0;
}

/* The id of a folder, creating it (and its parents) when missing. */
static char *od_folder(cloud_session *s, const char *path)
{
    for (int i = 0; i < s->n_folders; i++)
        if (strcmp(s->folders[i].path, path) == 0) return bd_strdup(s->folders[i].id);
    char *id = NULL;
    int rc = *path ? od_stat(s, path, NULL, &id) : 1;
    if (!*path) {
        /* The app folder itself. */
        char *url = bd_sprintf("%s/me/drive/special/approot", s->prov.api);
        bd_http_resp r;
        if (url && api(s, "GET", url, NULL, NULL, NULL, 0, 0, NULL, &r) == 0) {
            cJSON *j = r.status == 200 ? cJSON_Parse(r.body) : NULL;
            if (j && jstr(j, "id")) id = bd_strdup(jstr(j, "id"));
            else api_error(s, &r, "app folder");
            cJSON_Delete(j);
            bd_http_free(&r);
        }
        free(url);
        rc = id ? 0 : -1;
    } else if (rc == 1) {
        char *parent = bd_rel_dirname(path);
        char *pid = parent ? od_folder(s, parent) : NULL;
        free(parent);
        if (!pid) return NULL;
        char *url = bd_sprintf("%s/me/drive/items/%s/children", s->prov.api, pid);
        free(pid);
        cJSON *b = cJSON_CreateObject();
        cJSON_AddStringToObject(b, "name", bd_rel_basename(path));
        cJSON_AddItemToObject(b, "folder", cJSON_CreateObject());
        cJSON_AddStringToObject(b, "@microsoft.graph.conflictBehavior", "fail");
        char *body = cJSON_PrintUnformatted(b);
        cJSON_Delete(b);
        bd_http_resp r;
        if (url && body && api(s, "POST", url, NULL, body, NULL, 0, 0, NULL, &r) == 0) {
            if (r.status == 201 || r.status == 200) {
                cJSON *j = cJSON_Parse(r.body);
                if (j && jstr(j, "id")) id = bd_strdup(jstr(j, "id"));
                cJSON_Delete(j);
            } else if (r.status == 409) {
                od_stat(s, path, NULL, &id); /* made meanwhile */
            } else {
                api_error(s, &r, path);
            }
            bd_http_free(&r);
        }
        free(url);
        cJSON_free(body);
    }
    if (rc < 0 || !id) return NULL;
    folder_entry *f = realloc(s->folders, sizeof(folder_entry) * (size_t)(s->n_folders + 1));
    if (f) {
        s->folders = f;
        f[s->n_folders].path = bd_strdup(path);
        f[s->n_folders].id = bd_strdup(id);
        s->n_folders++;
    }
    return id;
}

static int od_download(cloud_session *s, const char *path, const char *dest)
{
    char *url = od_item_url(s, path, "/content");
    FILE *out = bd_fopen(dest, "wb");
    bd_http_resp r;
    int rc = url && out ? api(s, "GET", url, NULL, NULL, NULL, 0, 0, out, &r) : -1;
    free(url);
    if (out && fclose(out) != 0) rc = -1;
    if (rc != 0) return -1;
    rc = r.status == 200 ? 0 : r.status == 404 ? 1 : -1;
    if (rc < 0) api_error(s, &r, path);
    bd_http_free(&r);
    return rc;
}

static int od_upload(cloud_session *s, const char *staged, const char *path, bd_remote_stat *st)
{
    int64_t size = file_size(staged);
    if (size < 0) return -1;
    char *parent = bd_rel_dirname(path);
    char *pid = parent ? od_folder(s, parent) : NULL;
    free(parent);
    if (!pid) return -1;
    char *name = bd_url_encode(bd_rel_basename(path), 0);
    bd_http_resp r;
    int rc = -1;
    if (size <= SIMPLE_UPLOAD_MAX) {
        char *url = name ? bd_sprintf("%s/me/drive/items/%s:/%s:/content?@microsoft.graph.conflictBehavior=replace", s->prov.api, pid, name) : NULL;
        const char *hdr[] = {"Content-Type: application/octet-stream", NULL};
        if (url && api(s, "PUT", url, hdr, NULL, staged, 0, size, NULL, &r) == 0) {
            if (r.status == 200 || r.status == 201) {
                cJSON *j = cJSON_Parse(r.body);
                if (j) { od_stat_from(j, st); rc = 0; }
                cJSON_Delete(j);
            } else {
                api_error(s, &r, path);
            }
            bd_http_free(&r);
        }
        free(url);
    } else {
        char *url = name ? bd_sprintf("%s/me/drive/items/%s:/%s:/createUploadSession", s->prov.api, pid, name) : NULL;
        const char *body = "{\"item\":{\"@microsoft.graph.conflictBehavior\":\"replace\"}}";
        char *upload_url = NULL;
        if (url && api(s, "POST", url, NULL, body, NULL, 0, 0, NULL, &r) == 0) {
            cJSON *j = r.status == 200 ? cJSON_Parse(r.body) : NULL;
            if (j && jstr(j, "uploadUrl")) upload_url = bd_strdup(jstr(j, "uploadUrl"));
            else api_error(s, &r, path);
            cJSON_Delete(j);
            bd_http_free(&r);
        }
        free(url);
        /* The upload URL is pre-authorised: no Authorization header. */
        for (int64_t off = 0; upload_url && off < size;) {
            int64_t n = size - off < ONEDRIVE_CHUNK ? size - off : ONEDRIVE_CHUNK;
            char range[128];
            snprintf(range, sizeof(range), "Content-Range: bytes %lld-%lld/%lld", (long long)off, (long long)(off + n - 1), (long long)size);
            const char *hdr[] = {range, NULL};
            bd_http_req req = {"PUT", upload_url, hdr, NULL, 0, staged, off, n, NULL, 1};
            int tries = 0, sent = -1;
            while (tries++ < 4) {
                if (bd_http(&req, &r, s->err, sizeof(s->err)) != 0) { bd_sleep_ms(2000 * tries); continue; }
                if (r.status == 202) { sent = 0; bd_http_free(&r); break; }
                if (r.status == 200 || r.status == 201) {
                    cJSON *j = cJSON_Parse(r.body);
                    if (j) { od_stat_from(j, st); sent = 0; rc = 0; }
                    cJSON_Delete(j);
                    bd_http_free(&r);
                    break;
                }
                if (r.status >= 500 || r.status == 429) { bd_http_free(&r); bd_sleep_ms(2000 * tries); continue; }
                api_error(s, &r, path);
                bd_http_free(&r);
                break;
            }
            if (sent != 0) { rc = -1; break; }
            off += n;
        }
        if (upload_url && rc != 0) {
            bd_http_req cancel = {"DELETE", upload_url, NULL, NULL, 0, NULL, 0, 0, NULL, 1};
            char e[64];
            if (bd_http(&cancel, &r, e, sizeof(e)) == 0) bd_http_free(&r);
        }
        free(upload_url);
    }
    free(name);
    free(pid);
    return rc;
}

static int od_remove(cloud_session *s, const char *path)
{
    char *url = od_item_url(s, path, NULL);
    bd_http_resp r;
    int rc = url ? api(s, "DELETE", url, NULL, NULL, NULL, 0, 0, NULL, &r) : -1;
    free(url);
    if (rc != 0) return -1;
    rc = r.status == 204 || r.status == 200 || r.status == 404 ? 0 : -1;
    if (rc) api_error(s, &r, path);
    bd_http_free(&r);
    return rc;
}

static int od_move(cloud_session *s, const char *from, const char *to, int replace)
{
    bd_remote_stat st;
    int there = od_stat(s, to, &st, NULL);
    if (there < 0) return -1;
    if (there == 0) {
        if (!replace) return 1;
        if (od_remove(s, to) != 0) return -1;
    }
    char *parent = bd_rel_dirname(to);
    char *pid = parent ? od_folder(s, parent) : NULL;
    free(parent);
    if (!pid) return -1;
    char *url = od_item_url(s, from, NULL);
    cJSON *b = cJSON_CreateObject();
    cJSON *ref = cJSON_AddObjectToObject(b, "parentReference");
    cJSON_AddStringToObject(ref, "id", pid);
    cJSON_AddStringToObject(b, "name", bd_rel_basename(to));
    char *body = cJSON_PrintUnformatted(b);
    cJSON_Delete(b);
    free(pid);
    bd_http_resp r;
    int rc = url && body ? api(s, "PATCH", url, NULL, body, NULL, 0, 0, NULL, &r) : -1;
    free(url);
    cJSON_free(body);
    if (rc != 0) return -1;
    rc = r.status == 200 ? 0 : r.status == 409 ? 1 : -1;
    if (rc < 0) api_error(s, &r, from);
    bd_http_free(&r);
    return rc;
}

static int od_space(cloud_session *s, int64_t *total, int64_t *free_bytes)
{
    char *url = bd_sprintf("%s/me/drive", s->prov.api);
    bd_http_resp r;
    int rc = url ? api(s, "GET", url, NULL, NULL, NULL, 0, 0, NULL, &r) : -1;
    free(url);
    if (rc != 0) return -1;
    cJSON *j = r.status == 200 ? cJSON_Parse(r.body) : NULL;
    const cJSON *q = j ? cJSON_GetObjectItemCaseSensitive(j, "quota") : NULL;
    rc = q ? 0 : -1;
    if (q) { *total = jnum(q, "total"); *free_bytes = jnum(q, "remaining"); }
    cJSON_Delete(j);
    bd_http_free(&r);
    return rc;
}

static int od_account(cloud_session *s)
{
    char *url = bd_sprintf("%s/me", s->prov.api);
    bd_http_resp r;
    int rc = url ? api(s, "GET", url, NULL, NULL, NULL, 0, 0, NULL, &r) : -1;
    free(url);
    if (rc != 0) return -1;
    cJSON *j = r.status == 200 ? cJSON_Parse(r.body) : NULL;
    const char *name = j ? jstr(j, "userPrincipalName") : NULL;
    if (!name && j) name = jstr(j, "mail");
    if (!name && j) name = jstr(j, "displayName");
    rc = name ? 0 : -1;
    if (name) snprintf(s->account, sizeof(s->account), "%s", name);
    else api_error(s, &r, "account");
    cJSON_Delete(j);
    bd_http_free(&r);
    return rc;
}

/* ---- Dropbox (app folder) --------------------------------------------------------- */

static char *db_path(const char *path)
{
    return bd_sprintf("/%s", path);
}

static int db_rpc(cloud_session *s, const char *endpoint, cJSON *args, bd_http_resp *r)
{
    char *url = bd_sprintf("%s/%s", s->prov.api, endpoint);
    char *body = args ? cJSON_PrintUnformatted(args) : bd_strdup("null");
    int rc = url && body ? api(s, "POST", url, NULL, body, NULL, 0, 0, NULL, r) : -1;
    free(url);
    if (args) cJSON_free(body); else free(body);
    return rc;
}

static int db_not_found(const bd_http_resp *r)
{
    return r->status == 409 && r->body && strstr(r->body, "not_found");
}

static void db_stat_from(const cJSON *m, bd_remote_stat *st)
{
    memset(st, 0, sizeof(*st));
    st->size = jnum(m, "size");
    const char *h = jstr(m, "content_hash");
    if (h) snprintf(st->rev, sizeof(st->rev), "%s", h);
}

static int db_stat(cloud_session *s, const char *path, bd_remote_stat *st)
{
    char *p = db_path(path);
    cJSON *a = cJSON_CreateObject();
    cJSON_AddStringToObject(a, "path", p);
    free(p);
    bd_http_resp r;
    int rc = db_rpc(s, "files/get_metadata", a, &r);
    cJSON_Delete(a);
    if (rc != 0) return -1;
    if (db_not_found(&r)) { bd_http_free(&r); return 1; }
    if (r.status != 200) { api_error(s, &r, path); bd_http_free(&r); return -1; }
    cJSON *j = cJSON_Parse(r.body);
    bd_http_free(&r);
    const char *tag = j ? jstr(j, ".tag") : NULL;
    rc = tag && strcmp(tag, "file") == 0 ? 0 : 1;
    if (rc == 0) db_stat_from(j, st);
    cJSON_Delete(j);
    return rc;
}

static int db_content(cloud_session *s, const char *endpoint, cJSON *arg, const char *file, int64_t off, int64_t len,
                      FILE *out, bd_http_resp *r)
{
    char *a = json_ascii(arg);
    char *hdr_arg = a ? bd_sprintf("Dropbox-API-Arg: %s", a) : NULL;
    free(a);
    char *url = bd_sprintf("%s/%s", s->prov.content, endpoint);
    const char *hdr[] = {hdr_arg, "Content-Type: application/octet-stream", NULL};
    int rc = hdr_arg && url ? api(s, "POST", url, hdr, NULL, file, off, len, out, r) : -1;
    free(hdr_arg);
    free(url);
    return rc;
}

static int db_download(cloud_session *s, const char *path, const char *dest)
{
    char *p = db_path(path);
    cJSON *a = cJSON_CreateObject();
    cJSON_AddStringToObject(a, "path", p);
    free(p);
    FILE *out = bd_fopen(dest, "wb");
    bd_http_resp r;
    int rc = out ? db_content(s, "files/download", a, NULL, 0, 0, out, &r) : -1;
    cJSON_Delete(a);
    if (out && fclose(out) != 0) rc = -1;
    if (rc != 0) return -1;
    rc = r.status == 200 ? 0 : db_not_found(&r) ? 1 : -1;
    if (rc < 0) api_error(s, &r, path);
    bd_http_free(&r);
    return rc;
}

static int db_upload(cloud_session *s, const char *staged, const char *path, bd_remote_stat *st)
{
    int64_t size = file_size(staged);
    if (size < 0) return -1;
    char *p = db_path(path);
    cJSON *commit = cJSON_CreateObject();
    cJSON_AddStringToObject(commit, "path", p);
    cJSON_AddStringToObject(commit, "mode", "overwrite");
    cJSON_AddBoolToObject(commit, "mute", 1);
    free(p);
    bd_http_resp r;
    int rc = -1;
    if (size <= DROPBOX_CHUNK) {
        if (db_content(s, "files/upload", commit, staged, 0, size, NULL, &r) == 0) {
            cJSON *j = r.status == 200 ? cJSON_Parse(r.body) : NULL;
            if (j) { db_stat_from(j, st); rc = 0; }
            else api_error(s, &r, path);
            cJSON_Delete(j);
            bd_http_free(&r);
        }
        cJSON_Delete(commit);
        return rc;
    }
    /* Upload session: start with the first chunk, append, finish. */
    char session[256] = "";
    cJSON *a = cJSON_CreateObject();
    cJSON_AddBoolToObject(a, "close", 0);
    if (db_content(s, "files/upload_session/start", a, staged, 0, DROPBOX_CHUNK, NULL, &r) == 0) {
        cJSON *j = r.status == 200 ? cJSON_Parse(r.body) : NULL;
        if (j && jstr(j, "session_id")) snprintf(session, sizeof(session), "%s", jstr(j, "session_id"));
        else api_error(s, &r, path);
        cJSON_Delete(j);
        bd_http_free(&r);
    }
    cJSON_Delete(a);
    int64_t off = DROPBOX_CHUNK;
    while (session[0]) {
        int64_t n = size - off < DROPBOX_CHUNK ? size - off : DROPBOX_CHUNK;
        int last = off + n >= size;
        cJSON *arg = cJSON_CreateObject();
        cJSON *cursor = cJSON_AddObjectToObject(arg, "cursor");
        cJSON_AddStringToObject(cursor, "session_id", session);
        cJSON_AddNumberToObject(cursor, "offset", (double)off);
        if (last) cJSON_AddItemToObject(arg, "commit", cJSON_Duplicate(commit, 1));
        else cJSON_AddBoolToObject(arg, "close", 0);
        int got = db_content(s, last ? "files/upload_session/finish" : "files/upload_session/append_v2", arg, staged, off, n, NULL, &r);
        cJSON_Delete(arg);
        if (got != 0) break;
        if (r.status != 200) { api_error(s, &r, path); bd_http_free(&r); break; }
        if (last) {
            cJSON *j = cJSON_Parse(r.body);
            if (j) { db_stat_from(j, st); rc = 0; }
            cJSON_Delete(j);
            bd_http_free(&r);
            break;
        }
        bd_http_free(&r);
        off += n;
    }
    cJSON_Delete(commit);
    return rc;
}

static int db_remove(cloud_session *s, const char *path)
{
    char *p = db_path(path);
    cJSON *a = cJSON_CreateObject();
    cJSON_AddStringToObject(a, "path", p);
    free(p);
    bd_http_resp r;
    int rc = db_rpc(s, "files/delete_v2", a, &r);
    cJSON_Delete(a);
    if (rc != 0) return -1;
    rc = r.status == 200 || db_not_found(&r) ? 0 : -1;
    if (rc) api_error(s, &r, path);
    bd_http_free(&r);
    return rc;
}

static int db_move(cloud_session *s, const char *from, const char *to, int replace)
{
    if (replace && db_remove(s, to) != 0) return -1;
    char *pf = db_path(from), *pt = db_path(to);
    cJSON *a = cJSON_CreateObject();
    cJSON_AddStringToObject(a, "from_path", pf);
    cJSON_AddStringToObject(a, "to_path", pt);
    cJSON_AddBoolToObject(a, "autorename", 0);
    free(pf);
    free(pt);
    bd_http_resp r;
    int rc = db_rpc(s, "files/move_v2", a, &r);
    cJSON_Delete(a);
    if (rc != 0) return -1;
    rc = r.status == 200 ? 0 : (r.status == 409 && r.body && strstr(r.body, "conflict")) ? 1 : -1;
    if (rc < 0) api_error(s, &r, from);
    bd_http_free(&r);
    return rc;
}

static int db_space(cloud_session *s, int64_t *total, int64_t *free_bytes)
{
    bd_http_resp r;
    if (db_rpc(s, "users/get_space_usage", NULL, &r) != 0) return -1;
    cJSON *j = r.status == 200 ? cJSON_Parse(r.body) : NULL;
    const cJSON *alloc = j ? cJSON_GetObjectItemCaseSensitive(j, "allocation") : NULL;
    int rc = alloc ? 0 : -1;
    if (alloc) {
        *total = jnum(alloc, "allocated");
        *free_bytes = *total - jnum(j, "used");
    }
    cJSON_Delete(j);
    bd_http_free(&r);
    return rc;
}

static int db_account(cloud_session *s)
{
    bd_http_resp r;
    if (db_rpc(s, "users/get_current_account", NULL, &r) != 0) return -1;
    cJSON *j = r.status == 200 ? cJSON_Parse(r.body) : NULL;
    const char *email = j ? jstr(j, "email") : NULL;
    int rc = email ? 0 : -1;
    if (email) snprintf(s->account, sizeof(s->account), "%s", email);
    else api_error(s, &r, "account");
    cJSON_Delete(j);
    bd_http_free(&r);
    return rc;
}

/* ---- Store backend ------------------------------------------------------------ */

typedef struct {
    cloud_session *sess; /* owned by the catalog */
    char *base;          /* BRODALF/<catalog uuid> */
} cloud_impl;

static char *full_path(bd_store *st, const char *rel)
{
    cloud_impl *c = st->impl;
    return bd_sprintf("%s/%s", c->base, rel);
}

#define ONEDRIVE(st) (((cloud_impl *)(st)->impl)->sess->prov.id == BD_CLOUD_ONEDRIVE)
#define SESS(st) (((cloud_impl *)(st)->impl)->sess)

static int copy_err(bd_store *st, int rc)
{
    if (rc < 0) snprintf(st->err, sizeof(st->err), "%s", SESS(st)->err);
    return rc;
}

static int c_stat(bd_store *st, const char *rel, bd_remote_stat *out)
{
    char *p = full_path(st, rel);
    int rc = p ? (ONEDRIVE(st) ? od_stat(SESS(st), p, out, NULL) : db_stat(SESS(st), p, out)) : -1;
    free(p);
    return copy_err(st, rc);
}

static char *c_local_path(bd_store *st, const char *rel)
{
    (void)st;
    (void)rel;
    return NULL;
}

static int c_download(bd_store *st, const char *rel, const char *dest)
{
    char *p = full_path(st, rel);
    int rc = p ? (ONEDRIVE(st) ? od_download(SESS(st), p, dest) : db_download(SESS(st), p, dest)) : -1;
    free(p);
    return copy_err(st, rc);
}

static char *c_staging_path(bd_store *st, const char *rel)
{
    (void)st;
    (void)rel;
    return bd_temp_file("upload");
}

static int c_upload(bd_store *st, const char *staged, const char *rel, bd_remote_stat *out)
{
    char *p = full_path(st, rel);
    int rc = p ? (ONEDRIVE(st) ? od_upload(SESS(st), staged, p, out) : db_upload(SESS(st), staged, p, out)) : -1;
    free(p);
    bd_remove(staged);
    return copy_err(st, rc);
}

static int c_move(bd_store *st, const char *from, const char *to, int replace)
{
    char *a = full_path(st, from), *b = full_path(st, to);
    int rc = a && b ? (ONEDRIVE(st) ? od_move(SESS(st), a, b, replace) : db_move(SESS(st), a, b, replace)) : -1;
    free(a);
    free(b);
    return copy_err(st, rc);
}

static int c_remove(bd_store *st, const char *rel)
{
    char *p = full_path(st, rel);
    int rc = p ? (ONEDRIVE(st) ? od_remove(SESS(st), p) : db_remove(SESS(st), p)) : -1;
    free(p);
    return copy_err(st, rc);
}

static int c_space(bd_store *st, int64_t *total, int64_t *free_bytes)
{
    return ONEDRIVE(st) ? od_space(SESS(st), total, free_bytes) : db_space(SESS(st), total, free_bytes);
}

static void c_close(bd_store *st)
{
    cloud_impl *c = st->impl;
    if (c) { free(c->base); free(c); }
}

static const bd_store_ops cloud_ops = {c_stat, c_local_path, c_download, c_staging_path, c_upload,
                                       c_move, c_remove, c_space, c_close};

static bd_store *make_store(bd_catalog *cat, cloud_session *sess, int64_t media_id)
{
    bd_store *st = calloc(1, sizeof(*st));
    cloud_impl *c = calloc(1, sizeof(*c));
    if (!st || !c) { free(st); free(c); return NULL; }
    c->sess = sess;
    c->base = bd_sprintf(BD_MEDIA_DIR "/%s", cat->uuid);
    st->ops = &cloud_ops;
    st->cat = cat;
    st->media_id = media_id;
    st->impl = c;
    return st;
}

bd_store *bd_cloud_store_open(bd_catalog *cat, int64_t media_id)
{
    cloud_session *s = find_session(cat, media_id);
    if (!s) {
        bd_fail(cat, BD_ERR_NOT_FOUND, "that cloud account is not connected");
        return NULL;
    }
    bd_store *st = make_store(cat, s, media_id);
    if (!st) bd_fail(cat, BD_ERR_NOMEM, "out of memory");
    return st;
}

/* ---- Signing in ---------------------------------------------------------------- */

struct bd_signin {
    cloud_session *sess;
    char verifier[64];
    char state[32];
    char *url;
};

bd_status bd_cloud_signin_begin(bd_catalog *cat, bd_cloud_provider provider_id, bd_signin **out, const char **url_out)
{
    *out = NULL;
    bd_signin *si = calloc(1, sizeof(*si));
    cloud_session *s = calloc(1, sizeof(*s));
    if (!si || !s) { free(si); free(s); return bd_fail(cat, BD_ERR_NOMEM, "out of memory"); }
    si->sess = s;
    if (load_provider(provider_id, &s->prov, s->err, sizeof(s->err)) != 0) {
        bd_status st = bd_fail(cat, BD_ERR_INVALID, "%s", s->err);
        bd_cloud_signin_free(si);
        return st;
    }
    uint8_t rnd[32], digest[32];
    bd_random_bytes(rnd, sizeof(rnd));
    base64url(rnd, sizeof(rnd), si->verifier);
    bd_random_bytes(rnd, 16);
    base64url(rnd, 16, si->state);
    sha256(si->verifier, strlen(si->verifier), digest);
    char challenge[64];
    base64url(digest, sizeof(digest), challenge);

    char redirect[64];
    snprintf(redirect, sizeof(redirect), "http://localhost:%d/", redirect_port());
    char *r = bd_url_encode(redirect, 0), *cid = bd_url_encode(s->prov.client_id, 0), *scope = bd_url_encode(ONEDRIVE_SCOPE, 0);
    if (r && cid && scope) {
        if (provider_id == BD_CLOUD_ONEDRIVE)
            si->url = bd_sprintf("%s?client_id=%s&response_type=code&redirect_uri=%s&response_mode=query&scope=%s"
                                 "&code_challenge=%s&code_challenge_method=S256&state=%s&prompt=select_account",
                                 s->prov.authorize, cid, r, scope, challenge, si->state);
        else
            si->url = bd_sprintf("%s?client_id=%s&response_type=code&redirect_uri=%s&token_access_type=offline"
                                 "&code_challenge=%s&code_challenge_method=S256&state=%s",
                                 s->prov.authorize, cid, r, challenge, si->state);
    }
    free(r);
    free(cid);
    free(scope);
    if (!si->url) { bd_cloud_signin_free(si); return bd_fail(cat, BD_ERR_NOMEM, "out of memory"); }
    *out = si;
    *url_out = si->url;
    return BD_OK;
}

static const char *DONE_PAGE =
    "<!doctype html><meta charset=utf-8><title>BRODALF</title>"
    "<body style=\"font-family:Segoe UI,sans-serif;margin:3em\"><h2>Signed in</h2>"
    "<p>You can close this tab and go back to BRODALF.</p></body>";

bd_status bd_cloud_signin_finish(bd_catalog *cat, bd_signin *si, int timeout_ms)
{
    cloud_session *s = si->sess;
    char err[256];
    char *query = bd_http_wait_redirect(redirect_port(), timeout_ms, DONE_PAGE, err, sizeof(err));
    if (!query) return bd_fail(cat, BD_ERR_IO, "%s", err);
    char *code = form_value(query, "code"), *state = form_value(query, "state"), *error = form_value(query, "error_description");
    if (!error) error = form_value(query, "error");
    free(query);
    bd_status st = BD_OK;
    if (!code || !state || strcmp(state, si->state) != 0) {
        st = bd_fail(cat, BD_ERR_INVALID, "%s did not finish signing in%s%s", s->prov.title, error ? ": " : "", error ? error : "");
    } else {
        char redirect[64];
        snprintf(redirect, sizeof(redirect), "http://localhost:%d/", redirect_port());
        char *c = bd_url_encode(code, 0), *r = bd_url_encode(redirect, 0), *cid = bd_url_encode(s->prov.client_id, 0);
        char *form = c && r && cid ? bd_sprintf("grant_type=authorization_code&code=%s&redirect_uri=%s&client_id=%s&code_verifier=%s",
                                                c, r, cid, si->verifier)
                                   : NULL;
        if (!form || token_request(s, form) != 0) st = bd_fail(cat, BD_ERR_IO, "%s", s->err);
        else if (!s->refresh_token) st = bd_fail(cat, BD_ERR_IO, "%s gave no lasting sign-in", s->prov.title);
        else if ((s->prov.id == BD_CLOUD_ONEDRIVE ? od_account(s) : db_account(s)) != 0) st = bd_fail(cat, BD_ERR_IO, "%s", s->err);
        free(c);
        free(r);
        free(cid);
        free(form);
    }
    free(code);
    free(state);
    free(error);
    return st;
}

const char *bd_cloud_signin_account(const bd_signin *si)
{
    return si && si->sess ? si->sess->account : "";
}

void bd_cloud_signin_free(bd_signin *si)
{
    if (!si) return;
    session_free(si->sess);
    free(si->url);
    free(si);
}

/* ---- Cloud accounts as media ---------------------------------------------------- */

static char *display_root(const cloud_session *s)
{
    return bd_sprintf("%s: %s", s->prov.title, s->account);
}

/* Read BRODALF.media from the account. 0, 1 not there, -1 error. */
static int read_remote_media(bd_catalog *cat, cloud_session *s, bd_media_file *mf)
{
    bd_store *st = make_store(cat, s, 0);
    if (!st) return -1;
    char *tmp = NULL;
    int is_temp = 0;
    int rc = bd_store_fetch(st, BD_MEDIA_FILE, &tmp, &is_temp);
    if (rc == 0 && bd_media_file_read(tmp, mf) != 0) rc = -1;
    if (rc < 0) snprintf(s->err, sizeof(s->err), "%s", st->err[0] ? st->err : "cannot read the BRODALF.media file");
    bd_store_release(tmp, is_temp);
    bd_store_close(st);
    return rc;
}

bd_status bd_cloud_add(bd_catalog *cat, bd_signin *si, const char *label, unsigned flags, int64_t *out_media_id)
{
    cloud_session *s = si->sess;
    if (!s || !s->refresh_token) return bd_fail(cat, BD_ERR_INVALID, "sign in first");
    if (!label || !*label) return bd_fail(cat, BD_ERR_INVALID, "give the cloud storage a label");
    int encrypted = (flags & BD_MEDIA_ENCRYPTED) != 0;
    if (encrypted && !cat->have_key)
        return bd_fail(cat, BD_ERR_PASSPHRASE, bd_catalog_has_passphrase(cat) ? "enter the passphrase to set up encrypted storage"
                                                                               : "set a passphrase to set up encrypted storage");
    bd_media_file mf;
    int there = read_remote_media(cat, s, &mf);
    if (there < 0) return bd_fail(cat, BD_ERR_IO, "%s", s->err);
    if (there == 0 && strcmp(mf.catalog_uuid, cat->uuid) == 0 && bd_media_id_for_uuid(cat, mf.media_uuid))
        return bd_fail(cat, BD_ERR_EXISTS, "this %s account is already set up for this catalog as \"%s\"", s->prov.title, mf.label);

    if (there != 0) {
        memset(&mf, 0, sizeof(mf));
        bd_uuid_v4(mf.media_uuid);
        snprintf(mf.catalog_uuid, sizeof(mf.catalog_uuid), "%s", cat->uuid);
        snprintf(mf.label, sizeof(mf.label), "%s", label);
        for (char *p = mf.label; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
        mf.encrypted = encrypted;
    } /* else: the account knows this catalog but the catalog lost it; take it back as it was */

    char *secret = bd_sprintf("cloud-%s", mf.media_uuid);
    if (!secret || bd_secret_set(secret, s->refresh_token) != 0) {
        free(secret);
        return bd_fail(cat, BD_ERR_IO, "cannot save the sign-in in Credential Manager");
    }
    int64_t id = bd_media_insert(cat, mf.media_uuid, s->prov.kind, mf.label, mf.encrypted);
    sqlite3_stmt *ins = NULL;
    bd_status st = BD_OK;
    if (!id || sqlite3_prepare_v2(cat->db,
                                  "INSERT INTO cloud_accounts(media_id, provider, username, root_path, credential_ref) VALUES(?,?,?,?,?)",
                                  -1, &ins, NULL) != SQLITE_OK) {
        st = bd_fail_db(cat, "add cloud storage");
    } else {
        sqlite3_bind_int64(ins, 1, id);
        sqlite3_bind_text(ins, 2, s->prov.kind, -1, SQLITE_STATIC);
        sqlite3_bind_text(ins, 3, s->account, -1, SQLITE_STATIC);
        sqlite3_bind_text(ins, 4, "Apps/BRODALF", -1, SQLITE_STATIC);
        sqlite3_bind_text(ins, 5, secret, -1, SQLITE_STATIC);
        if (sqlite3_step(ins) != SQLITE_DONE) st = bd_fail_db(cat, "add cloud storage");
    }
    sqlite3_finalize(ins);

    if (st == BD_OK && there != 0) {
        /* Write BRODALF.media into the account. */
        bd_store *store = make_store(cat, s, id);
        char *staged = bd_temp_file("media");
        bd_remote_stat rs;
        if (!store || !staged || bd_media_file_write(staged, &mf) != 0 ||
            store->ops->upload(store, staged, BD_MEDIA_FILE, &rs) != 0)
            st = bd_fail(cat, BD_ERR_IO, "cannot write to %s: %s", s->prov.title, store ? store->err : "");
        if (staged) bd_remove(staged);
        free(staged);
        bd_store_close(store);
    }
    if (st != BD_OK) {
        sqlite3_stmt *d;
        if (id && sqlite3_prepare_v2(cat->db, "DELETE FROM media WHERE id=?", -1, &d, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(d, 1, id);
            sqlite3_step(d);
            sqlite3_finalize(d);
        }
        bd_secret_delete(secret);
        free(secret);
        return st;
    }

    /* The sign-in now belongs to this media. */
    si->sess = NULL;
    s->media_id = id;
    s->secret_name = secret;
    keep_session(cat, s);
    char *root = display_root(s);
    st = bd_media_record_connected(cat, id, root ? root : s->prov.title);
    free(root);
    if (st == BD_OK && out_media_id) *out_media_id = id;
    return st;
}

bd_status bd_cloud_connect(bd_catalog *cat, int64_t media_id, bd_check_stats *stats, bd_log_fn log, void *log_ctx)
{
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT a.provider, a.username, a.credential_ref, m.uuid FROM cloud_accounts a JOIN media m ON m.id=a.media_id"
                           " WHERE a.media_id=?",
                           -1, &q, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "find cloud account");
    sqlite3_bind_int64(q, 1, media_id);
    if (sqlite3_step(q) != SQLITE_ROW) {
        sqlite3_finalize(q);
        return bd_fail(cat, BD_ERR_NOT_FOUND, "no such cloud account");
    }
    bd_cloud_provider pid;
    cloud_session *s = calloc(1, sizeof(*s));
    char media_uuid[37];
    snprintf(media_uuid, sizeof(media_uuid), "%s", (const char *)sqlite3_column_text(q, 3));
    bd_status st = BD_OK;
    if (!s) st = bd_fail(cat, BD_ERR_NOMEM, "out of memory");
    else if (provider_by_kind((const char *)sqlite3_column_text(q, 0), &pid) != 0 ||
             load_provider(pid, &s->prov, s->err, sizeof(s->err)) != 0)
        st = bd_fail(cat, BD_ERR_INVALID, "%s", s->err[0] ? s->err : "unknown cloud provider");
    else {
        snprintf(s->account, sizeof(s->account), "%s", (const char *)sqlite3_column_text(q, 1));
        s->secret_name = bd_strdup((const char *)sqlite3_column_text(q, 2));
        s->media_id = media_id;
        s->refresh_token = s->secret_name ? bd_secret_get(s->secret_name) : NULL;
        if (!s->refresh_token) st = bd_fail(cat, BD_ERR_PASSPHRASE, "sign in to %s again (%s)", s->prov.title, s->account);
    }
    sqlite3_finalize(q);
    if (st == BD_OK) {
        int rr = refresh_access(s);
        if (rr == -2) st = bd_fail(cat, BD_ERR_PASSPHRASE, "sign in to %s again: %s", s->prov.title, s->err);
        else if (rr != 0) st = bd_fail(cat, BD_ERR_IO, "%s", s->err);
    }
    if (st == BD_OK) {
        bd_media_file mf;
        int got = read_remote_media(cat, s, &mf);
        if (got < 0) st = bd_fail(cat, BD_ERR_IO, "%s", s->err);
        else if (got == 1) st = bd_fail(cat, BD_ERR_NOT_FOUND, "the BRODALF folder is gone from %s (%s)", s->prov.title, s->account);
        else if (strcmp(mf.catalog_uuid, cat->uuid) != 0 || strcmp(mf.media_uuid, media_uuid) != 0)
            st = bd_fail(cat, BD_ERR_INVALID, "the BRODALF folder in %s belongs to something else", s->prov.title);
    }
    if (st != BD_OK) { session_free(s); return st; }
    keep_session(cat, s);
    char *root = display_root(s);
    st = bd_media_record_connected(cat, media_id, root ? root : s->prov.title);
    free(root);
    if (st != BD_OK) return st;
    return bd_media_check(cat, media_id, 0, stats, log, log_ctx);
}

bd_status bd_cloud_sign_out(bd_catalog *cat, int64_t media_id)
{
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(cat->db, "SELECT credential_ref FROM cloud_accounts WHERE media_id=?", -1, &q, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "find cloud account");
    sqlite3_bind_int64(q, 1, media_id);
    if (sqlite3_step(q) == SQLITE_ROW && sqlite3_column_text(q, 0)) bd_secret_delete((const char *)sqlite3_column_text(q, 0));
    sqlite3_finalize(q);
    bd_media_disconnect(cat, media_id);
    cloud_session **pp = (cloud_session **)&cat->clouds;
    while (*pp) {
        if ((*pp)->media_id == media_id) {
            cloud_session *old = *pp;
            *pp = old->next;
            session_free(old);
        } else {
            pp = &(*pp)->next;
        }
    }
    return BD_OK;
}
