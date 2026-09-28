/* HTTP client on WinHTTP (HTTPS, proxies and certificates handled by
 * Windows). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#include "http.h"
#include "internal.h"

#include <stdlib.h>
#include <string.h>

static HINTERNET g_session;

static wchar_t *wide(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = n > 0 ? malloc(sizeof(wchar_t) * (size_t)n) : NULL;
    if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static void win_err(char *err, size_t cap, const char *what)
{
    snprintf(err, cap, "%s (Windows error %lu)", what, (unsigned long)GetLastError());
}

static int once(const bd_http_req *req, bd_http_resp *resp, char *err, size_t err_cap)
{
    memset(resp, 0, sizeof(*resp));
    if (!g_session) {
        g_session = WinHttpOpen(L"BRODALF/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
        if (!g_session) /* before Windows 8.1 */
            g_session = WinHttpOpen(L"BRODALF/0.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
        if (!g_session) { win_err(err, err_cap, "cannot start WinHTTP"); return -1; }
        WinHttpSetTimeouts(g_session, 30000, 30000, 120000, 300000);
    }
    wchar_t *url = wide(req->url), *method = wide(req->method);
    HINTERNET conn = NULL, h = NULL;
    FILE *f = NULL;
    int rc = -1;
    if (!url || !method) goto done;

    URL_COMPONENTS uc;
    memset(&uc, 0, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256], path[4096];
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 4096;
    wchar_t extra[4096];
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = 4096;
    if (!WinHttpCrackUrl(url, 0, 0, &uc)) { win_err(err, err_cap, "bad URL"); goto done; }
    wchar_t full_path[8192];
    _snwprintf(full_path, 8192, L"%ls%ls", path, extra);
    full_path[8191] = 0;

    conn = WinHttpConnect(g_session, host, uc.nPort, 0);
    if (!conn) { win_err(err, err_cap, "cannot connect"); goto done; }
    h = WinHttpOpenRequest(conn, method, full_path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                           uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!h) { win_err(err, err_cap, "cannot open request"); goto done; }
    /* Redirects are followed by bd_http, without our headers. */
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(h, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
    for (const char *const *hd = req->headers; hd && *hd; hd++) {
        wchar_t *w = wide(*hd);
        if (w) { WinHttpAddRequestHeaders(h, w, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE); free(w); }
    }

    int64_t total = req->body_file ? req->file_len : (int64_t)req->body_len;
    if (total > 0xFFFFFFFFll) { snprintf(err, err_cap, "request body too large"); goto done; }
    if (!WinHttpSendRequest(h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, (DWORD)total, 0)) {
        win_err(err, err_cap, "cannot reach the server");
        goto done;
    }
    if (req->body_file) {
        f = bd_fopen(req->body_file, "rb");
        if (!f || _fseeki64(f, req->file_offset, SEEK_SET) != 0) { snprintf(err, err_cap, "cannot read %s", req->body_file); goto done; }
        static char chunk[1 << 16];
        int64_t left = req->file_len;
        while (left > 0) {
            size_t want = left < (int64_t)sizeof(chunk) ? (size_t)left : sizeof(chunk);
            size_t n = fread(chunk, 1, want, f);
            DWORD wrote = 0;
            if (n == 0 || !WinHttpWriteData(h, chunk, (DWORD)n, &wrote) || wrote != n) {
                win_err(err, err_cap, "upload interrupted");
                goto done;
            }
            left -= (int64_t)n;
        }
    } else if (req->body_len) {
        DWORD wrote = 0;
        if (!WinHttpWriteData(h, req->body, (DWORD)req->body_len, &wrote)) { win_err(err, err_cap, "upload interrupted"); goto done; }
    }
    if (!WinHttpReceiveResponse(h, NULL)) { win_err(err, err_cap, "no answer from the server"); goto done; }

    DWORD status = 0, len = sizeof(status);
    WinHttpQueryHeaders(h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &len,
                        WINHTTP_NO_HEADER_INDEX);
    resp->status = (int)status;
    wchar_t wbuf[2048];
    len = sizeof(wbuf);
    if (WinHttpQueryHeaders(h, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, wbuf, &len, WINHTTP_NO_HEADER_INDEX))
        WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, resp->location, (int)sizeof(resp->location), NULL, NULL);
    len = sizeof(wbuf);
    if (WinHttpQueryHeaders(h, WINHTTP_QUERY_RETRY_AFTER, WINHTTP_HEADER_NAME_BY_INDEX, wbuf, &len, WINHTTP_NO_HEADER_INDEX))
        resp->retry_after = _wtoi(wbuf);

    int to_file = req->out && status >= 200 && status < 300;
    size_t cap = 0;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(h, &avail)) { win_err(err, err_cap, "download interrupted"); goto done; }
        if (avail == 0) break;
        char *tmp = malloc(avail);
        DWORD got = 0;
        if (!tmp || !WinHttpReadData(h, tmp, avail, &got)) { free(tmp); win_err(err, err_cap, "download interrupted"); goto done; }
        if (to_file) {
            if (fwrite(tmp, 1, got, req->out) != got) { free(tmp); snprintf(err, err_cap, "cannot write the download"); goto done; }
        } else {
            if (resp->body_len + got + 1 > cap) {
                size_t ncap = cap ? cap * 2 : 8192;
                while (ncap < resp->body_len + got + 1) ncap *= 2;
                char *nb = realloc(resp->body, ncap);
                if (!nb) { free(tmp); goto done; }
                resp->body = nb;
                cap = ncap;
            }
            memcpy(resp->body + resp->body_len, tmp, got);
            resp->body_len += got;
        }
        free(tmp);
    }
    if (!resp->body) resp->body = bd_strdup("");
    else resp->body[resp->body_len] = '\0';
    rc = 0;

done:
    if (f) fclose(f);
    if (h) WinHttpCloseHandle(h);
    if (conn) WinHttpCloseHandle(conn);
    free(url);
    free(method);
    if (rc != 0) bd_http_free(resp);
    return rc;
}

int bd_http(const bd_http_req *req, bd_http_resp *resp, char *err, size_t err_cap)
{
    int rc = once(req, resp, err, err_cap);
    char url[2048];
    for (int hop = 0; rc == 0 && !req->no_redirects && resp->status >= 300 && resp->status < 400 && resp->location[0]; hop++) {
        if (hop == 5) { bd_http_free(resp); snprintf(err, err_cap, "too many redirects"); return -1; }
        /* A pre-signed download link needs no authorization header. */
        snprintf(url, sizeof(url), "%s", resp->location);
        bd_http_free(resp);
        bd_http_req follow = {"GET", url, NULL, NULL, 0, NULL, 0, 0, req->out, 0};
        rc = once(&follow, resp, err, err_cap);
    }
    return rc;
}
