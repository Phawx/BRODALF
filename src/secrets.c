/* Saved sign-ins. On Windows, secrets go to Credential Manager as generic
 * credentials named "BRODALF/<name>". A secret longer than one credential
 * can hold is split over "BRODALF/<name>#1", "#2", ... Elsewhere (tests)
 * they are files readable only by the user, under $BRODALF_SECRETS_DIR or
 * ~/.config/brodalf/secrets. */
#include "internal.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincred.h>

#define PART 2048

static wchar_t *target(const char *name, int part)
{
    char *t = part ? bd_sprintf("BRODALF/%s#%d", name, part) : bd_sprintf("BRODALF/%s", name);
    if (!t) return NULL;
    int n = MultiByteToWideChar(CP_UTF8, 0, t, -1, NULL, 0);
    wchar_t *w = malloc(sizeof(wchar_t) * (size_t)n);
    if (w) MultiByteToWideChar(CP_UTF8, 0, t, -1, w, n);
    free(t);
    return w;
}

int bd_secret_delete(const char *name)
{
    for (int part = 0; part < 64; part++) {
        wchar_t *t = target(name, part);
        BOOL ok = t && CredDeleteW(t, CRED_TYPE_GENERIC, 0);
        free(t);
        if (!ok && part > 0) break;
    }
    return 0;
}

int bd_secret_set(const char *name, const char *value)
{
    bd_secret_delete(name);
    size_t len = strlen(value);
    for (int part = 0; part == 0 || (size_t)part * PART < len; part++) {
        size_t off = (size_t)part * PART, n = len - off < PART ? len - off : PART;
        wchar_t *t = target(name, part);
        if (!t) return -1;
        CREDENTIALW c;
        memset(&c, 0, sizeof(c));
        c.Type = CRED_TYPE_GENERIC;
        c.TargetName = t;
        c.Comment = L"BRODALF cloud sign-in";
        c.CredentialBlobSize = (DWORD)n;
        c.CredentialBlob = (LPBYTE)(value + off);
        c.Persist = CRED_PERSIST_LOCAL_MACHINE;
        c.UserName = L"BRODALF";
        BOOL ok = CredWriteW(&c, 0);
        free(t);
        if (!ok) return -1;
    }
    return 0;
}

char *bd_secret_get(const char *name)
{
    char *out = NULL;
    size_t len = 0;
    for (int part = 0; part < 64; part++) {
        wchar_t *t = target(name, part);
        PCREDENTIALW c = NULL;
        BOOL ok = t && CredReadW(t, CRED_TYPE_GENERIC, 0, &c);
        free(t);
        if (!ok) break;
        char *grown = realloc(out, len + c->CredentialBlobSize + 1);
        if (!grown) { CredFree(c); free(out); return NULL; }
        out = grown;
        memcpy(out + len, c->CredentialBlob, c->CredentialBlobSize);
        len += c->CredentialBlobSize;
        out[len] = '\0';
        int full = c->CredentialBlobSize == PART;
        CredFree(c);
        if (!full) break;
    }
    return out;
}

#else
#include <sys/stat.h>

static char *secret_path(const char *name)
{
    const char *dir = getenv("BRODALF_SECRETS_DIR");
    char *base = dir ? bd_strdup(dir) : NULL;
    if (!base) {
        const char *home = getenv("HOME");
        if (!home) return NULL;
        base = bd_sprintf("%s/.config/brodalf/secrets", home);
    }
    if (!base) return NULL;
    bd_mkdirs(base);
    chmod(base, 0700);
    char *safe = bd_strdup(name);
    for (char *p = safe; p && *p; p++) if (*p == '/' || *p == '\\') *p = '_';
    char *path = safe ? bd_sprintf("%s/%s", base, safe) : NULL;
    free(safe);
    free(base);
    return path;
}

int bd_secret_set(const char *name, const char *value)
{
    char *p = secret_path(name);
    if (!p) return -1;
    FILE *f = bd_fopen(p, "wb");
    int rc = -1;
    if (f) {
        chmod(p, 0600);
        rc = fputs(value, f) >= 0 ? 0 : -1;
        if (fclose(f) != 0) rc = -1;
    }
    free(p);
    return rc;
}

char *bd_secret_get(const char *name)
{
    char *p = secret_path(name);
    FILE *f = p ? bd_fopen(p, "rb") : NULL;
    free(p);
    if (!f) return NULL;
    char buf[16384];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return bd_strdup(buf);
}

int bd_secret_delete(const char *name)
{
    char *p = secret_path(name);
    if (p) bd_remove(p);
    free(p);
    return 0;
}
#endif
