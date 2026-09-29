#ifndef BD_INTERNAL_H
#define BD_INTERNAL_H

#include "brodalf.h"
#include "platform.h"
#include "sqlite3.h"

#include <stdarg.h>
#include <stdio.h>

#define BD_HASH_HEX_LEN 64
#define BD_MEDIA_DIR "BRODALF"
#define BD_MEDIA_FILE "BRODALF.media"
#define BD_VERSIONS_DIR ".versions"
#define BD_TMP_MARKER ".brodalf-tmp"

struct bd_catalog {
    sqlite3 *db;
    char *path;       /* the .brodalf file */
    char *lock_path;
    char *work_path;  /* unpacked SQLite working copy */
    char uuid[37];
    char err[1024];
    bd_progress_fn progress;
    void *progress_ctx;
    int64_t progress_last_ms;
    uint8_t key[32];  /* master key while unlocked */
    int have_key;
    void *clouds;     /* signed-in cloud sessions (cloud.c) */
};

/* Record an error message on the catalog and return status. */
bd_status bd_fail(bd_catalog *cat, bd_status status, const char *fmt, ...);
bd_status bd_fail_db(bd_catalog *cat, const char *what);
void bd_logf(bd_log_fn log, void *ctx, const char *fmt, ...);

char *bd_strdup(const char *s);
char *bd_sprintf(const char *fmt, ...);
/* Join with '/' (platform functions accept either separator). */
char *bd_path_join(const char *a, const char *b);
/* Directory part of a '/'-joined relative path ("" if none). */
char *bd_rel_dirname(const char *rel);
const char *bd_rel_basename(const char *rel);

/* Hash a file with BLAKE3. If copy_to is non-NULL the bytes are also written
 * there. hex_out receives 64 hex chars plus NUL. 0 on success; on failure
 * -1 for read errors, -2 for write errors. */
int bd_hash_file(const char *path, FILE *copy_to, char hex_out[BD_HASH_HEX_LEN + 1], int64_t *size_out);

/* Like bd_hash_file, but the source can be a sealed (encrypted) file when
 * src_key is set, and the copy is sealed when dst_key is set. The hash is
 * always of the plaintext. -3 means the sealed source is damaged or was
 * sealed with another key. */
int bd_hash_copy(const char *path, const uint8_t *src_key, FILE *copy_to, const uint8_t *dst_key,
                 char hex_out[BD_HASH_HEX_LEN + 1], int64_t *size_out);

/* Streaming encryption (see crypto.c for the format). */
typedef struct bd_sealer bd_sealer;
typedef struct bd_opener bd_opener;
bd_sealer *bd_seal_begin(FILE *out, const uint8_t key[32]);
int bd_seal_write(bd_sealer *s, const void *data, size_t n); /* 0 or -2 */
int bd_seal_end(bd_sealer *s);                                /* writes the last chunk, frees */
bd_opener *bd_open_begin(FILE *in, const uint8_t key[32]);   /* NULL: not a sealed stream */
long bd_open_read(bd_opener *o, void *buf, size_t cap);       /* bytes, 0 at end, -1 I/O, -3 damaged */
void bd_open_end(bd_opener *o);

/* Write to a file, sealed or not; read likewise. */
typedef struct { FILE *file; bd_sealer *sealer; } bd_sink;
typedef struct { FILE *file; bd_opener *opener; } bd_source;
int bd_sink_write(bd_sink *k, const void *data, size_t n);
long bd_source_read(bd_source *k, void *buf, size_t cap);

/* Catalog settings (the settings table). get: 1 if found. set: 0 on success. */
int bd_setting_get(bd_catalog *cat, const char *key, char *out, size_t cap);
int bd_setting_set(bd_catalog *cat, const char *key, const char *value);

/* The passphrase-wrapped master key, as stored in settings and in the
 * header of an encrypted .brodalf file. */
#define BD_KEY_BLOCK 96
int bd_catalog_key_block(bd_catalog *cat, uint8_t block[BD_KEY_BLOCK]);
int bd_unwrap_key_block(const char *pass, const uint8_t block[BD_KEY_BLOCK], uint8_t key[32]); /* -1 nomem, -3 wrong */
void bd_catalog_set_key(bd_catalog *cat, const uint8_t key[32]);
void bd_wipe(void *p, size_t n);

int bd_media_encrypted(bd_catalog *cat, int64_t media_id);

/* Save a hardware reading for a drive. Logs when the drive ID turns up on a
 * different disk than last time (a copied or moved BRODALF folder). */
/* SMART parsing, exposed for tests (src/drive_hw.c). */
int bd_drive_hw_parse_ata(bd_drive_hw *hw, const unsigned char data[512], int predicted_failure);
int bd_drive_hw_parse_nvme(bd_drive_hw *hw, const unsigned char log[512]);
void bd_media_store_hw(bd_catalog *cat, int64_t media_id, const bd_drive_hw *hw, bd_log_fn log, void *log_ctx);

/* The BRODALF.media file that identifies a drive or cloud folder. */
typedef struct {
    char media_uuid[37];
    char catalog_uuid[37];
    char label[256];
    int encrypted;
} bd_media_file;
int bd_media_file_read(const char *path, bd_media_file *mf);
int bd_media_file_write(const char *path, const bd_media_file *mf);
bd_status bd_media_record_connected(bd_catalog *cat, int64_t media_id, const char *root);
int64_t bd_media_id_for_uuid(bd_catalog *cat, const char *uuid);
int64_t bd_media_insert(bd_catalog *cat, const char *uuid, const char *kind, const char *label, int encrypted);

/* Cloud sessions live on the catalog; forget them on close. */
void bd_cloud_forget_all(bd_catalog *cat);

/* Saved secrets (Credential Manager on Windows). */
int bd_secret_set(const char *name, const char *value);
char *bd_secret_get(const char *name);
int bd_secret_delete(const char *name);
int bd_net_init(void);

/* Save a copy of the catalog to dest, encrypted or not. */
bd_status bd_catalog_save_to(bd_catalog *cat, const char *dest, int encrypt);

/* Encrypted copies on a drive carry this suffix. */
#define BD_SEALED_SUFFIX ".bdenc"

void bd_uuid_v4(char out[37]);

/* Media root directory for this catalog: <root>/BRODALF/<uuid>. */
char *bd_media_catalog_dir(const bd_catalog *cat, const char *root);
/* Root of a connected media, or NULL (caller frees). */
char *bd_connected_root(bd_catalog *cat, int64_t media_id);

int bd_exec(bd_catalog *cat, const char *sql);

/* Report progress, throttled unless force is set. */
void bd_report(bd_catalog *cat, const char *phase, int64_t files, int64_t bytes, const char *current, int force);

#endif
