/* Plain-HTTP client for non-Windows builds. It exists so the cloud code can
 * be tested against local mock servers; HTTPS is not supported here. */
#include "http.h"
#include "internal.h"

#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct { char *data; size_t len, cap; } buf_t;

static int buf_add(buf_t *b, const void *p, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 4096;
        while (cap < b->len + n + 1) cap *= 2;
        char *d = realloc(b->data, cap);
        if (!d) return -1;
        b->data = d;
        b->cap = cap;
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

static int send_all(int fd, const void *p, size_t n)
{
    const char *c = p;
    while (n > 0) {
        ssize_t w = send(fd, c, n, 0);
        if (w <= 0) return -1;
        c += w;
        n -= (size_t)w;
    }
    return 0;
}

static int once(const bd_http_req *req, const char *url, bd_http_resp *resp, char *err, size_t err_cap)
{
    if (strncmp(url, "http://", 7) != 0) {
        snprintf(err, err_cap, "HTTPS is only supported in the Windows build");
        return -1;
    }
    const char *hp = url + 7, *path = strchr(hp, '/');
    if (!path) path = hp + strlen(hp);
    char host[256], port[16] = "80";
    size_t hl = (size_t)(path - hp);
    if (hl >= sizeof(host)) return -1;
    memcpy(host, hp, hl);
    host[hl] = '\0';
    char host_header[256];
    snprintf(host_header, sizeof(host_header), "%s", host);
    char *colon = strchr(host, ':');
    if (colon) { *colon = '\0'; snprintf(port, sizeof(port), "%s", colon + 1); }

    struct addrinfo hints, *ai;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &ai) != 0) { snprintf(err, err_cap, "cannot resolve %s", host); return -1; }
    int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0 || connect(fd, ai->ai_addr, ai->ai_addrlen) != 0) {
        freeaddrinfo(ai);
        if (fd >= 0) close(fd);
        snprintf(err, err_cap, "cannot connect to %s", host);
        return -1;
    }
    freeaddrinfo(ai);

    int64_t body_len = req->body_file ? req->file_len : (int64_t)req->body_len;
    buf_t head = {0};
    char line[4096];
    snprintf(line, sizeof(line), "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\nContent-Length: %lld\r\n",
             req->method, *path ? path : "/", host_header, (long long)body_len);
    buf_add(&head, line, strlen(line));
    for (const char *const *h = req->headers; h && *h; h++) {
        buf_add(&head, *h, strlen(*h));
        buf_add(&head, "\r\n", 2);
    }
    buf_add(&head, "\r\n", 2);
    int rc = head.data ? send_all(fd, head.data, head.len) : -1;
    free(head.data);
    if (rc == 0 && req->body_file) {
        FILE *f = bd_fopen(req->body_file, "rb");
        if (!f || fseek(f, (long)req->file_offset, SEEK_SET) != 0) rc = -1;
        char chunk[65536];
        int64_t left = req->file_len;
        while (rc == 0 && left > 0) {
            size_t want = left < (int64_t)sizeof(chunk) ? (size_t)left : sizeof(chunk);
            size_t n = fread(chunk, 1, want, f);
            if (n == 0 || send_all(fd, chunk, n) != 0) rc = -1;
            left -= (int64_t)n;
        }
        if (f) fclose(f);
    } else if (rc == 0 && req->body_len) {
        rc = send_all(fd, req->body, req->body_len);
    }
    if (rc != 0) { close(fd); snprintf(err, err_cap, "cannot send the request"); return -1; }

    /* Read the whole response (Connection: close). */
    buf_t in = {0};
    char chunk[65536];
    ssize_t n;
    while ((n = recv(fd, chunk, sizeof(chunk), 0)) > 0)
        if (buf_add(&in, chunk, (size_t)n) != 0) { close(fd); free(in.data); return -1; }
    close(fd);
    char *sep = in.data ? strstr(in.data, "\r\n\r\n") : NULL;
    if (!sep || sscanf(in.data, "HTTP/%*s %d", &resp->status) != 1) {
        free(in.data);
        snprintf(err, err_cap, "bad response from %s", host);
        return -1;
    }
    *sep = '\0';
    char *body = sep + 4;
    size_t blen = in.len - (size_t)(body - in.data);
    int chunked = 0;
    for (char *h = strstr(in.data, "\r\n"); h; h = strstr(h, "\r\n")) {
        h += 2;
        if (strncasecmp(h, "Location:", 9) == 0) {
            const char *v = h + 9;
            while (*v == ' ') v++;
            size_t l = strcspn(v, "\r\n");
            if (l >= sizeof(resp->location)) l = sizeof(resp->location) - 1;
            memcpy(resp->location, v, l);
            resp->location[l] = '\0';
        } else if (strncasecmp(h, "Retry-After:", 12) == 0) {
            resp->retry_after = atoi(h + 12);
        } else if (strncasecmp(h, "Transfer-Encoding:", 18) == 0 && strstr(h, "chunked")) {
            chunked = 1;
        }
    }
    buf_t out = {0};
    if (chunked) {
        char *p = body, *end = body + blen;
        while (p < end) {
            size_t sz = strtoul(p, NULL, 16);
            char *nl = strstr(p, "\r\n");
            if (!nl || sz == 0) break;
            p = nl + 2;
            if (p + sz > end) break;
            buf_add(&out, p, sz);
            p += sz + 2;
        }
    } else {
        buf_add(&out, body, blen);
    }
    free(in.data);
    int ok = resp->status >= 200 && resp->status < 300;
    if (ok && req->out) {
        if (out.len && fwrite(out.data, 1, out.len, req->out) != out.len) rc = -1;
        free(out.data);
    } else {
        resp->body = out.data ? out.data : bd_strdup("");
        resp->body_len = out.len;
    }
    return rc;
}

int bd_http(const bd_http_req *req, bd_http_resp *resp, char *err, size_t err_cap)
{
    memset(resp, 0, sizeof(*resp));
    int rc = once(req, req->url, resp, err, err_cap);
    char url[2048];
    for (int hop = 0; rc == 0 && !req->no_redirects && resp->status >= 300 && resp->status < 400 && resp->location[0]; hop++) {
        if (hop == 5) { snprintf(err, err_cap, "too many redirects"); return -1; }
        /* Follow as a plain GET without the original headers: a pre-signed
         * download link needs no authorization. */
        snprintf(url, sizeof(url), "%s", resp->location);
        bd_http_free(resp);
        memset(resp, 0, sizeof(*resp));
        bd_http_req follow = {"GET", url, NULL, NULL, 0, NULL, 0, 0, req->out, 0};
        rc = once(&follow, url, resp, err, err_cap);
    }
    return rc;
}
