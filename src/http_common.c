/* HTTP helpers shared by both platforms: URL encoding and the one-shot
 * loopback listener that catches the browser redirect after sign-in. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET bd_socket;
#define BD_BAD_SOCKET INVALID_SOCKET
#define bd_closesocket closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int bd_socket;
#define BD_BAD_SOCKET (-1)
#define bd_closesocket close
#endif

#include "http.h"
#include "internal.h"

#include <stdlib.h>
#include <string.h>

void bd_http_free(bd_http_resp *resp)
{
    if (!resp) return;
    free(resp->body);
    resp->body = NULL;
    resp->body_len = 0;
}

char *bd_url_encode(const char *s, int keep_slash)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t n = strlen(s);
    char *out = malloc(n * 3 + 1), *o = out;
    if (!out) return NULL;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~' || (keep_slash && c == '/')) {
            *o++ = (char)c;
        } else {
            *o++ = '%';
            *o++ = hex[c >> 4];
            *o++ = hex[c & 15];
        }
    }
    *o = '\0';
    return out;
}

int bd_net_init(void)
{
#ifdef _WIN32
    static int done = 0;
    if (!done) {
        WSADATA w;
        if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return -1;
        done = 1;
    }
#endif
    return 0;
}

char *bd_http_wait_redirect(int port, int timeout_ms, const char *page_html, char *err, size_t err_cap)
{
    if (bd_net_init() != 0) { snprintf(err, err_cap, "networking is not available"); return NULL; }
    bd_socket ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls == BD_BAD_SOCKET) { snprintf(err, err_cap, "cannot open a socket"); return NULL; }
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(ls, 4) != 0) {
        snprintf(err, err_cap, "port %d is busy; close other sign-in windows and try again", port);
        bd_closesocket(ls);
        return NULL;
    }
    char *query = NULL;
    int64_t deadline = bd_now_ms() + timeout_ms;
    while (!query) {
        int64_t left = deadline - bd_now_ms();
        if (left <= 0) { snprintf(err, err_cap, "sign-in timed out"); break; }
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(ls, &rd);
        struct timeval tv = {(long)(left / 1000), (long)((left % 1000) * 1000)};
        if (select((int)ls + 1, &rd, NULL, NULL, &tv) <= 0) continue;
        bd_socket c = accept(ls, NULL, NULL);
        if (c == BD_BAD_SOCKET) continue;
        char buf[8192];
        int got = 0;
        while (got < (int)sizeof(buf) - 1) {
            int n = recv(c, buf + got, (int)sizeof(buf) - 1 - got, 0);
            if (n <= 0) break;
            got += n;
            buf[got] = '\0';
            if (strstr(buf, "\r\n\r\n")) break;
        }
        buf[got] = '\0';
        /* "GET /?code=...&state=... HTTP/1.1" */
        char *q = strncmp(buf, "GET ", 4) == 0 ? strchr(buf + 4, '?') : NULL;
        char *end = q ? strchr(q, ' ') : NULL;
        const char *reply_body = "Not found";
        if (q && end) {
            *end = '\0';
            query = bd_strdup(q + 1);
            reply_body = page_html;
        }
        char *resp = bd_sprintf("HTTP/1.1 %s\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %u\r\n"
                                "Connection: close\r\n\r\n%s",
                                query ? "200 OK" : "404 Not Found", (unsigned)strlen(reply_body), reply_body);
        if (resp) { send(c, resp, (int)strlen(resp), 0); free(resp); }
        bd_closesocket(c);
    }
    bd_closesocket(ls);
    return query;
}
