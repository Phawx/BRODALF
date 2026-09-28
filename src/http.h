/* A small HTTP client for the cloud backends. On Windows it uses WinHTTP
 * (HTTPS). Elsewhere only plain http:// works; that is enough for the tests,
 * which talk to local mock servers. */
#ifndef BD_HTTP_H
#define BD_HTTP_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    const char *method;
    const char *url;
    const char *const *headers; /* "Name: value" strings, NULL-terminated; may be NULL */
    const void *body;           /* request body in memory ... */
    size_t body_len;
    const char *body_file;      /* ... or a byte range of a file */
    int64_t file_offset;
    int64_t file_len;
    FILE *out;                  /* 2xx response bodies go here if set, otherwise to memory */
    int no_redirects;
} bd_http_req;

typedef struct {
    int status;
    char *body;                 /* NUL-terminated; error bodies always land here */
    size_t body_len;
    char location[2048];
    int retry_after;            /* seconds, 0 if absent */
} bd_http_resp;

/* 0 once a response arrived (any status), -1 on a network error. */
int bd_http(const bd_http_req *req, bd_http_resp *resp, char *err, size_t err_cap);
void bd_http_free(bd_http_resp *resp);

/* Percent-encode everything except unreserved characters (and '/' if keep_slash). */
char *bd_url_encode(const char *s, int keep_slash);

/* Wait for one browser redirect to http://localhost:<port>/?... and return
 * the query string (caller frees), answering with a short page. NULL on
 * timeout or error. */
char *bd_http_wait_redirect(int port, int timeout_ms, const char *page_html, char *err, size_t err_cap);

#endif
