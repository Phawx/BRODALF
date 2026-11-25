/* Hidden names on an encrypted drive.
 *
 * A drive set up with BD_MEDIA_HIDE_NAMES (only an encrypted one) keeps
 * every file and folder under BRODALF/<catalog uuid>/ under a name made
 * from the real one with a key, so someone holding the drive sees how
 * many files and folders there are and how big, but not what they are
 * called. Each part of a path becomes 24 letters and digits:
 *
 *   name = base32(BLAKE3-keyed(drive key, folder path above it || 0 || name))[0..15 bytes]
 *
 * The drive key comes from the catalog's master key and the drive's ID,
 * so two drives never share names. The same file always gets the same
 * name, which is what lets a backup find what is already on the drive.
 * The real names are kept in the catalog, and its copy on the drive is
 * encrypted like everything else there. A file's name keeps .bdenc at
 * the end; folders have no ending. */
#include "internal.h"

#include "blake3.h"

#include <stdlib.h>
#include <string.h>

#define NAME_BYTES 15 /* 120 bits, 24 base32 characters */

void bd_names_key(const uint8_t master[32], const char *media_uuid, uint8_t out[32])
{
    static const char ctx[] = "BRODALF hidden names v1";
    blake3_hasher h;
    blake3_hasher_init_keyed(&h, master);
    blake3_hasher_update(&h, ctx, sizeof(ctx)); /* with its 0 */
    blake3_hasher_update(&h, media_uuid, strlen(media_uuid));
    blake3_hasher_finalize(&h, out, 32);
}

static void base32(const uint8_t *in, size_t n, char *out)
{
    static const char abc[] = "abcdefghijklmnopqrstuvwxyz234567";
    uint32_t buf = 0;
    int bits = 0;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        buf = (buf << 8) | in[i];
        bits += 8;
        while (bits >= 5) {
            out[o++] = abc[(buf >> (bits - 5)) & 31];
            bits -= 5;
        }
    }
    if (bits > 0) out[o++] = abc[(buf << (5 - bits)) & 31];
    out[o] = '\0';
}

char *bd_hide_rel(const uint8_t names_key[32], const char *plain_rel)
{
    size_t len = strlen(plain_rel), slen = strlen(BD_SEALED_SUFFIX);
    int sealed = len > slen && strcmp(plain_rel + len - slen, BD_SEALED_SUFFIX) == 0;
    if (sealed) len -= slen;
    /* Each part becomes 24 characters and a slash. */
    size_t parts = 1;
    for (size_t i = 0; i < len; i++) if (plain_rel[i] == '/') parts++;
    char *out = malloc(parts * 25 + slen + 1);
    if (!out) return NULL;
    size_t o = 0, start = 0;
    while (start <= len) {
        size_t end = start;
        while (end < len && plain_rel[end] != '/') end++;
        blake3_hasher h;
        uint8_t mac[NAME_BYTES];
        blake3_hasher_init_keyed(&h, names_key);
        blake3_hasher_update(&h, plain_rel, start); /* the folder above, with its slash */
        blake3_hasher_update(&h, "", 1);
        blake3_hasher_update(&h, plain_rel + start, end - start);
        blake3_hasher_finalize(&h, mac, sizeof(mac));
        if (o) out[o++] = '/';
        base32(mac, sizeof(mac), out + o);
        o += 24;
        start = end + 1;
    }
    if (sealed) memcpy(out + o, BD_SEALED_SUFFIX, slen), o += slen;
    out[o] = '\0';
    return out;
}
