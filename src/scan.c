#include "internal.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static char *sanitize_name(const char *folder)
{
    size_t len = strlen(folder);
    while (len > 0 && (folder[len - 1] == '/' || folder[len - 1] == '\\')) len--;
    size_t start = len;
    while (start > 0 && folder[start - 1] != '/' && folder[start - 1] != '\\') start--;
    char *name = malloc(len - start + 16);
    if (!name) return NULL;
    size_t j = 0;
    for (size_t i = start; i < len; i++) {
        char c = folder[i];
        name[j++] = (c == ':' || c == '<' || c == '>' || c == '"' || c == '|' || c == '?' || c == '*') ? '_' : c;
    }
    name[j] = '\0';
    /* "C:\" becomes "C_"; make drive roots read better. */
    if (j == 2 && name[1] == '_') { name[1] = '\0'; strcat(name, "-drive"); }
    if (j == 0 || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) strcpy(name, "source");
    return name;
}

/* catalog-r<digits>.brodalf: the copies of the catalog on a drive. */
static int catalog_copy_name(const char *name)
{
    static const char pre[] = "catalog-r", post[] = ".brodalf";
    size_t n = strlen(name), a = sizeof(pre) - 1, b = sizeof(post) - 1, k;
    if (n <= a + b) return 0;
    for (k = 0; k < a; k++) if ((name[k] | 0x20) != pre[k]) return 0;
    for (k = 0; k < b; k++) if ((name[n - b + k] | 0x20) != post[k]) return 0;
    for (k = a; k < n - b; k++) if (name[k] < '0' || name[k] > '9') return 0;
    return 1;
}

/* Names next to the backup roots on a drive that are BRODALF's own. */
static int name_reserved(const char *name)
{
    if (catalog_copy_name(name) || bd_newest_list_name(name)) return 1;
    static const char *const own[] = {BD_VERSIONS_DIR, BD_MEDIA_FILE, "catalog-backup.brodalf"};
    size_t tl = strlen(BD_TMP_MARKER);
    for (size_t i = 0; i < sizeof(own) / sizeof(own[0]); i++) {
        size_t n = strlen(own[i]), k = 0;
        while (k < n && name[k] && (name[k] | 0x20) == (own[i][k] | 0x20)) k++;
        if (k == n && (name[n] == '\0' || (strncmp(name + n, BD_TMP_MARKER, tl) == 0 && name[n + tl] == '\0'))) return 1;
    }
    return 0;
}

/* "_versions" for ".versions", "_BRODALF.media" for "BRODALF.media". */
static char *escape_reserved(const char *name)
{
    return name[0] == '.' ? bd_sprintf("_%s", name + 1) : bd_sprintf("_%s", name);
}

static int name_taken(bd_catalog *cat, const char *name)
{
    if (name_reserved(name)) return 1;
    sqlite3_stmt *st;
    int taken = 1;
    if (sqlite3_prepare_v2(cat->db, "SELECT 1 FROM sources WHERE name=? COLLATE NOCASE", -1, &st, NULL) != SQLITE_OK) return 1;
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    taken = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    return taken;
}

/* A backup root's path as the file system compares paths: '/' between
 * names, no empty, "." or ".." names (".." taken off with the name before
 * it), no '/' at the end, and on Windows letter case folded as Windows
 * folds it (bd_fold_case; in tests too with bd_test_fold_case). "\\?\"
 * and "\\?\UNC\" prefixes are taken off; a UNC path keeps its leading
 * "//". NULL when out of memory. */
static char *root_key(const char *path)
{
    size_t len = strlen(path);
    char *s = malloc(len + 3);
    if (!s) return NULL;
    for (size_t i = 0; i <= len; i++) s[i] = path[i] == '\\' ? '/' : path[i];
    const char *p = s;
    char prefix[4] = "";
    if (strncmp(p, "//?/UNC/", 8) == 0) { p += 8; strcpy(prefix, "//"); }
    else if (strncmp(p, "//?/", 4) == 0) p += 4;
    if (!prefix[0]) {
        if (p[0] == '/' && p[1] == '/') { strcpy(prefix, "//"); p += 2; }
        else if (p[0] && p[1] == ':' && (p[2] == '/' || p[2] == '\0')) {
            prefix[0] = p[0];
            prefix[1] = ':';
            prefix[2] = '/';
            p += 2;
        } else if (p[0] == '/') { strcpy(prefix, "/"); }
    }
    char *out = malloc(len + 4);
    size_t *starts = malloc(sizeof(size_t) * (len / 2 + 2)); /* where each name kept begins */
    if (!out || !starts) { free(s); free(out); free(starts); return NULL; }
    size_t plen = strlen(prefix), n = plen, depth = 0, up = 0; /* up: ".." kept at the front (relative path) */
    memcpy(out, prefix, plen);
    while (*p) {
        while (*p == '/') p++;
        const char *e = p;
        while (*e && *e != '/') e++;
        size_t seg = (size_t)(e - p);
        if (seg == 0 || (seg == 1 && p[0] == '.')) {
            /* nothing */
        } else if (seg == 2 && p[0] == '.' && p[1] == '.' && depth > up) {
            n = starts[--depth];
            if (n > plen) n--; /* the '/' before it */
        } else if (seg == 2 && p[0] == '.' && p[1] == '.' && plen > 0) {
            /* above the top of an absolute path: stays at the top */
        } else {
            if (n > plen) out[n++] = '/';
            starts[depth++] = n;
            if (seg == 2 && p[0] == '.' && p[1] == '.') up++;
            memcpy(out + n, p, seg);
            n += seg;
        }
        p = e;
    }
    free(starts);
    out[n] = '\0';
    free(s);
#ifdef _WIN32
    int fold = 1;
#else
    int fold = bd_test_fold_case;
#endif
    if (!fold) return out;
    char *folded = bd_fold_case(out);
    free(out);
    return folded;
}

/* 1 if the folder whose key is inner is inside the one whose key is outer. */
static int key_inside(const char *inner, const char *outer)
{
    size_t lo = strlen(outer);
    if (strncmp(inner, outer, lo) != 0 || inner[lo] == '\0') return 0;
    return inner[lo] == '/' || (lo > 0 && outer[lo - 1] == '/'); /* outer is "/" or "c:/" */
}

/* A folder's key as it is written, and as the folder it names: links on
 * the way followed, a mapped network drive as its network path
 * (bd_real_path). One folder reached two ways has two written keys and one
 * real one. When the real folder cannot be told, both are the written
 * key. */
typedef struct {
    char *key, *real;
} root_keys;

static int keys_of(const char *path, root_keys *k)
{
    k->key = root_key(path);
    char *r = k->key ? bd_real_path(path) : NULL;
    k->real = root_key(r ? r : path);
    free(r);
    if (k->key && k->real) return 1;
    free(k->key);
    free(k->real);
    k->key = k->real = NULL;
    return 0;
}

static void keys_free(root_keys *k)
{
    free(k->key);
    free(k->real);
}

static int keys_same(const root_keys *a, const root_keys *b) { return !strcmp(a->key, b->key) || !strcmp(a->real, b->real); }
static int keys_inside(const root_keys *in, const root_keys *out)
{
    return key_inside(in->key, out->key) || key_inside(in->real, out->real);
}

/* Two backup roots never overlap: a folder inside a root, or holding one,
 * would have its files scanned and backed up twice, under two names. That
 * holds however the folder is reached: by another spelling, through a
 * link, or by a mapped drive letter and its network path. Roots that are
 * stopped (active 0) are not scanned and do not count, and with nesting 0
 * (for a stopped root) only another root that is this folder does. *same
 * gets the id of a root that is this very folder (any root, stopped or
 * not), or 0. BD_OK, or BD_ERR_EXISTS with a message naming the root in
 * the way. */
bd_status bd_root_check(bd_catalog *cat, const char *path, int64_t except_id, int nesting, int64_t *same)
{
    *same = 0;
    root_keys k;
    if (!keys_of(path, &k)) return BD_ERR_NOMEM;
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(cat->db, "SELECT id, path, active FROM sources WHERE id<>? ORDER BY id", -1, &q, NULL) != SQLITE_OK) {
        keys_free(&k);
        return bd_fail_db(cat, "source lookup");
    }
    sqlite3_bind_int64(q, 1, except_id);
    bd_status s = BD_OK;
    while (s == BD_OK && sqlite3_step(q) == SQLITE_ROW) {
        const char *other = (const char *)sqlite3_column_text(q, 1);
        root_keys o;
        if (!other || !keys_of(other, &o)) { s = BD_ERR_NOMEM; break; }
        int written = !strcmp(k.key, o.key) || key_inside(k.key, o.key) || key_inside(o.key, k.key);
        const char *how = written ? "" : ", though by another path (a link or a mapped drive)";
        if (keys_same(&k, &o)) {
            if (!*same) *same = sqlite3_column_int64(q, 0);
        } else if (nesting && sqlite3_column_int(q, 2) && keys_inside(&k, &o)) {
            s = bd_fail(cat, BD_ERR_EXISTS, "%s is inside %s, which is already a backup root%s: its files are backed up with it",
                        path, other, how);
        } else if (nesting && sqlite3_column_int(q, 2) && keys_inside(&o, &k)) {
            s = bd_fail(cat, BD_ERR_EXISTS,
                        "%s holds %s, which is already a backup root%s: a folder can be in only one backup root."
                        " Stop backing up %s first, or choose another folder",
                        path, other, how, other);
        }
        keys_free(&o);
    }
    sqlite3_finalize(q);
    keys_free(&k);
    return s;
}

/* The same test without a message: 1 if path is another backup root than
 * except_id (stopped or not), is inside an active one or holds one. */
int bd_root_overlaps(bd_catalog *cat, const char *path, int64_t except_id)
{
    root_keys k;
    if (!keys_of(path, &k)) return 1;
    sqlite3_stmt *q;
    int in = 0;
    if (sqlite3_prepare_v2(cat->db, "SELECT path, active FROM sources WHERE id<>?", -1, &q, NULL) != SQLITE_OK) {
        keys_free(&k);
        return 1;
    }
    sqlite3_bind_int64(q, 1, except_id);
    while (!in && sqlite3_step(q) == SQLITE_ROW) {
        const char *other = (const char *)sqlite3_column_text(q, 0);
        root_keys o;
        if (!other || !keys_of(other, &o)) { in = 1; break; }
        in = keys_same(&k, &o) || (sqlite3_column_int(q, 1) && (keys_inside(&k, &o) || keys_inside(&o, &k)));
        keys_free(&o);
    }
    sqlite3_finalize(q);
    keys_free(&k);
    return in;
}

bd_status bd_source_add(bd_catalog *cat, const char *folder, int64_t *out_id)
{
    return bd_source_add_ex(cat, folder, out_id, NULL);
}

bd_status bd_source_add_ex(bd_catalog *cat, const char *folder, int64_t *out_id, int *is_new)
{
    if (is_new) *is_new = 0;
    bd_stat_t st;
    if (bd_stat(folder, &st) == 0 && st.is_link) {
        char *target = bd_link_target(folder, NULL);
        bd_status s = target ? bd_fail(cat, BD_ERR_INVALID, "%s is a link to %s. BRODALF does not follow links: add %s instead",
                                       folder, target, target)
                             : bd_fail(cat, BD_ERR_NOT_FOUND, "%s is a link that leads nowhere", folder);
        free(target);
        return s;
    }
    if (bd_stat(folder, &st) != 0 || !st.is_dir) return bd_fail(cat, BD_ERR_NOT_FOUND, "%s is not a folder", folder);

    /* Kept as a full path, so a folder typed from the current folder is
     * found again from any other. */
    char *path = bd_full_path(folder);
    if (!path) return BD_ERR_NOMEM;
    size_t len = strlen(path);
    while (len > 1 && (path[len - 1] == '/' || path[len - 1] == '\\') && !(len == 3 && path[1] == ':')) path[--len] = '\0';

    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(cat->db, "SELECT id FROM sources WHERE path=?", -1, &q, NULL) != SQLITE_OK) { free(path); return bd_fail_db(cat, "source lookup"); }
    sqlite3_bind_text(q, 1, path, -1, SQLITE_STATIC);
    if (sqlite3_step(q) == SQLITE_ROW) {
        if (out_id) *out_id = sqlite3_column_int64(q, 0);
        sqlite3_finalize(q);
        free(path);
        return BD_OK;
    }
    sqlite3_finalize(q);

    int64_t same = 0;
    bd_status overlap = bd_root_check(cat, path, 0, 1, &same);
    if (overlap == BD_OK && same) {
        if (out_id) *out_id = same;
        free(path);
        return BD_OK;
    }
    if (overlap != BD_OK) { free(path); return overlap; }

    char *base = sanitize_name(path);
    if (base && name_reserved(base)) {
        char *esc = escape_reserved(base);
        free(base);
        base = esc;
    }
    char *name = base ? bd_strdup(base) : NULL;
    for (int n = 2; name && name_taken(cat, name) && n < 10000; n++) {
        free(name);
        name = bd_sprintf("%s-%d", base, n);
    }
    free(base);
    if (!name) { free(path); return BD_ERR_NOMEM; }

    sqlite3_stmt *ins;
    bd_status s = BD_OK;
    if (sqlite3_prepare_v2(cat->db, "INSERT INTO sources(path,name,added_ms) VALUES(?,?,?)", -1, &ins, NULL) != SQLITE_OK) {
        s = bd_fail_db(cat, "add source");
    } else {
        sqlite3_bind_text(ins, 1, path, -1, SQLITE_STATIC);
        sqlite3_bind_text(ins, 2, name, -1, SQLITE_STATIC);
        sqlite3_bind_int64(ins, 3, bd_now_ms());
        if (sqlite3_step(ins) != SQLITE_DONE) {
            s = bd_fail_db(cat, "add source");
        } else {
            if (out_id) *out_id = sqlite3_last_insert_rowid(cat->db);
            if (is_new) *is_new = 1;
        }
        sqlite3_finalize(ins);
    }
    free(name);
    free(path);
    return s;
}

/* ---- Making backup roots part of a new one -------------------------------- */

typedef struct {
    int64_t id;
    char *path;
    char *rel; /* its place in the new root: '/' between names, as its path writes them */
} inner_root;

static void inner_free(inner_root *r, int n)
{
    for (int i = 0; i < n; i++) {
        free(r[i].path);
        free(r[i].rel);
    }
    free(r);
}

/* folder as bd_source_add keeps it: a full path, no '/' at the end. */
static char *root_path(const char *folder)
{
    char *path = bd_full_path(folder);
    if (!path) return NULL;
    size_t len = strlen(path);
    while (len > 1 && (path[len - 1] == '/' || path[len - 1] == '\\') && !(len == 3 && path[1] == ':')) path[--len] = '\0';
    return path;
}

/* The last n names of path, '/' between them; NULL if it has fewer, or a
 * "." or ".." among them. */
static char *last_names(const char *path, int n)
{
    size_t len = strlen(path);
    char *out = malloc(len + 1);
    if (!out) return NULL;
    size_t end = len, used = 0;
    char *r = out + len;
    *r = '\0';
    for (int k = 0; k < n; k++) {
        while (end > 0 && (path[end - 1] == '/' || path[end - 1] == '\\')) end--;
        size_t start = end;
        while (start > 0 && path[start - 1] != '/' && path[start - 1] != '\\') start--;
        size_t seg = end - start;
        if (seg == 0 || (seg == 1 && path[start] == '.') || (seg == 2 && path[start] == '.' && path[start + 1] == '.')) {
            free(out);
            return NULL;
        }
        if (used) { *--r = '/'; used++; }
        r -= seg;
        memcpy(r, path + start, seg);
        used += seg;
        end = start;
    }
    memmove(out, r, used + 1);
    return out;
}

/* The backup roots that are backed up (active) inside path, a full path
 * as root_path gives it, with each one's place in it. *by_link (may be
 * NULL) gets 1 when one is inside it only another way (a link or a mapped
 * drive): its place cannot be told. */
static bd_status roots_inside(bd_catalog *cat, const char *path, inner_root **out, int *count, int *by_link)
{
    *out = NULL;
    *count = 0;
    if (by_link) *by_link = 0;
    root_keys k;
    if (!keys_of(path, &k)) return BD_ERR_NOMEM;
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(cat->db, "SELECT id, path FROM sources WHERE active<>0 ORDER BY id", -1, &q, NULL) != SQLITE_OK) {
        keys_free(&k);
        return bd_fail_db(cat, "source lookup");
    }
    bd_status s = BD_OK;
    inner_root *list = NULL;
    int n = 0;
    while (s == BD_OK && sqlite3_step(q) == SQLITE_ROW) {
        const char *other = (const char *)sqlite3_column_text(q, 1);
        root_keys o;
        if (!other || !keys_of(other, &o)) { s = BD_ERR_NOMEM; break; }
        if (key_inside(o.key, k.key)) {
            size_t lo = strlen(k.key);
            const char *tail = o.key + lo + (o.key[lo] == '/');
            int names = 1;
            for (const char *c = tail; *c; c++) names += *c == '/';
            inner_root *more = realloc(list, sizeof(inner_root) * (size_t)(n + 1));
            if (!more) s = BD_ERR_NOMEM;
            else {
                list = more;
                list[n].id = sqlite3_column_int64(q, 0);
                list[n].path = bd_strdup(other);
                list[n].rel = last_names(other, names);
                if (!list[n].path || !list[n].rel) {
                    s = list[n].path && !list[n].rel ? bd_fail(cat, BD_ERR_INVALID, "the path of the backup root %s cannot be read", other)
                                                     : BD_ERR_NOMEM;
                    free(list[n].path);
                    free(list[n].rel);
                } else {
                    n++;
                }
            }
        } else if (by_link && keys_inside(&o, &k)) {
            *by_link = 1;
        }
        keys_free(&o);
    }
    sqlite3_finalize(q);
    keys_free(&k);
    if (s != BD_OK) { inner_free(list, n); return s; }
    *out = list;
    *count = n;
    return BD_OK;
}

int bd_roots_inside(bd_catalog *cat, const char *folder, bd_root_fn fn, void *ctx)
{
    char *path = root_path(folder);
    if (!path) return -1;
    inner_root *in;
    int n;
    bd_status s = roots_inside(cat, path, &in, &n, NULL);
    free(path);
    if (s != BD_OK) return -1;
    for (int i = 0; i < n; i++)
        if (fn && fn(ctx, in[i].id, in[i].path)) break;
    inner_free(in, n);
    return n;
}

/* A root's own skip list for the root it is now part of, at rel in it: a
 * path from the root's top gets rel in front, and a name anywhere in it
 * becomes that name anywhere under rel. (A * in such a name may then also
 * stand for a folder or more under rel, as * does in a path.) Comments
 * stay. */
static char *skip_moved(const char *list, const char *rel)
{
    size_t cap = strlen(list) * 2 + 64, used = 0;
    char *out = malloc(cap);
    if (!out) return NULL;
    out[0] = '\0';
    for (const char *p = list; *p;) {
        const char *end = p;
        while (*end && *end != '\n') end++;
        const char *a = p, *b = end;
        while (a < b && (*a == ' ' || *a == '\t')) a++;
        while (b > a && (b[-1] == ' ' || b[-1] == '\t' || b[-1] == '\r')) b--;
        p = *end ? end + 1 : end;
        char *line;
        if (a == b || *a == '#') {
            line = bd_sprintf("%.*s\n", (int)(b - a), a);
        } else {
            int anchored = *a == '/' || *a == '\\';
            while (a < b && (*a == '/' || *a == '\\')) a++;
            const char *body_end = b;
            if (body_end - a > 1 && (body_end[-1] == '/' || body_end[-1] == '\\')) body_end--; /* a folder only */
            int has_slash = 0;
            for (const char *c = a; c < body_end; c++) has_slash |= *c == '/' || *c == '\\';
            if (a == b) line = bd_strdup("");
            else if (anchored || has_slash) line = bd_sprintf("/%s/%.*s\n", rel, (int)(b - a), a);
            else line = bd_sprintf("/%s/%.*s\n/%s/*/%.*s\n", rel, (int)(b - a), a, rel, (int)(b - a), a);
        }
        if (!line) { free(out); return NULL; }
        size_t ln = strlen(line);
        if (used + ln + 1 > cap) {
            cap = (used + ln + 1) * 2;
            char *more = realloc(out, cap);
            if (!more) { free(line); free(out); return NULL; }
            out = more;
        }
        memcpy(out + used, line, ln + 1);
        used += ln;
        free(line);
    }
    return out;
}

static int exec_ids(bd_catalog *cat, const char *sql, int64_t a, int64_t b)
{
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(cat->db, sql, -1, &q, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(q, 1, a);
    if (sqlite3_bind_parameter_count(q) >= 2) sqlite3_bind_int64(q, 2, b);
    int rc = sqlite3_step(q);
    sqlite3_finalize(q);
    return rc == SQLITE_DONE ? 0 : -1;
}

/* The folder node at rel in the root (made, with the folders it is in, if
 * the root has none); 0 on a database error. */
static int64_t folder_node(bd_catalog *cat, int64_t source_id, const char *rel, int64_t seen_ms)
{
    int64_t parent = 0;
    for (const char *e = rel;; e++) {
        if (*e != '/' && *e != '\0') continue;
        char *part = bd_sprintf("%.*s", (int)(e - rel), rel);
        if (!part) return 0;
        sqlite3_stmt *q;
        int64_t id = 0;
        if (sqlite3_prepare_v2(cat->db, "SELECT id FROM nodes WHERE source_id=? AND rel_path=?", -1, &q, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(q, 1, source_id);
            sqlite3_bind_text(q, 2, part, -1, SQLITE_STATIC);
            if (sqlite3_step(q) == SQLITE_ROW) id = sqlite3_column_int64(q, 0);
            sqlite3_finalize(q);
        }
        if (!id && sqlite3_prepare_v2(cat->db, "INSERT INTO nodes(source_id,parent_id,rel_path,name,is_dir,last_seen_ms,rel_fold)"
                                               " VALUES(?,?,?,?,1,?,?)", -1, &q, NULL) == SQLITE_OK) {
            char *fold = bd_path_key(part);
            sqlite3_bind_int64(q, 1, source_id);
            if (parent) sqlite3_bind_int64(q, 2, parent);
            else sqlite3_bind_null(q, 2);
            sqlite3_bind_text(q, 3, part, -1, SQLITE_STATIC);
            sqlite3_bind_text(q, 4, bd_rel_basename(part), -1, SQLITE_STATIC);
            sqlite3_bind_int64(q, 5, seen_ms);
            if (fold) sqlite3_bind_text(q, 6, fold, -1, SQLITE_STATIC);
            else sqlite3_bind_null(q, 6);
            if (sqlite3_step(q) == SQLITE_DONE) id = sqlite3_last_insert_rowid(cat->db);
            sqlite3_finalize(q);
            free(fold);
        }
        free(part);
        if (!id) return 0;
        parent = id;
        if (*e == '\0') return id;
    }
}

/* Root `in` becomes part of root `into`, in the catalog only: its files
 * and folders (with their versions and copies, which keep their place on
 * the drives), stars, encrypted folders and own skip list go to `into`
 * at in->rel, and the root itself is gone. In the caller's transaction. */
static bd_status take_in(bd_catalog *cat, int64_t into, const inner_root *in)
{
    int64_t now = bd_now_ms();
    int64_t top = folder_node(cat, into, in->rel, now);
    if (!top) return bd_fail_db(cat, "make the folder part of the new backup root");
    char *fold = bd_path_key(in->rel);
    sqlite3_stmt *q;
    bd_status s = BD_OK;
    if (sqlite3_prepare_v2(cat->db, "UPDATE nodes SET source_id=?1, parent_id=COALESCE(parent_id,?2), rel_path=?3||'/'||rel_path,"
                                    " rel_fold=CASE WHEN rel_fold IS NULL THEN NULL ELSE ?4||'/'||rel_fold END WHERE source_id=?5",
                           -1, &q, NULL) != SQLITE_OK) {
        s = bd_fail_db(cat, "make the folder part of the new backup root");
    } else {
        sqlite3_bind_int64(q, 1, into);
        sqlite3_bind_int64(q, 2, top);
        sqlite3_bind_text(q, 3, in->rel, -1, SQLITE_STATIC);
        if (fold) sqlite3_bind_text(q, 4, fold, -1, SQLITE_STATIC);
        else sqlite3_bind_null(q, 4);
        sqlite3_bind_int64(q, 5, in->id);
        if (sqlite3_step(q) != SQLITE_DONE) s = bd_fail_db(cat, "make the folder part of the new backup root");
        sqlite3_finalize(q);
    }
    free(fold);
    static const char *const marks[] = {
        "UPDATE stars SET source_id=?1, rel_path=CASE WHEN rel_path='' THEN ?2 ELSE ?2||'/'||rel_path END WHERE source_id=?3",
        "UPDATE encrypt_paths SET source_id=?1, rel_path=CASE WHEN rel_path='' THEN ?2 ELSE ?2||'/'||rel_path END WHERE source_id=?3",
    };
    for (size_t i = 0; s == BD_OK && i < sizeof(marks) / sizeof(marks[0]); i++) {
        if (sqlite3_prepare_v2(cat->db, marks[i], -1, &q, NULL) != SQLITE_OK) { s = bd_fail_db(cat, "move the starred files"); break; }
        sqlite3_bind_int64(q, 1, into);
        sqlite3_bind_text(q, 2, in->rel, -1, SQLITE_STATIC);
        sqlite3_bind_int64(q, 3, in->id);
        if (sqlite3_step(q) != SQLITE_DONE) s = bd_fail_db(cat, "move the starred files");
        sqlite3_finalize(q);
    }
    if (s == BD_OK) {
        char *own = bd_source_skip_get(cat, in->id), *have = bd_source_skip_get(cat, into);
        char *moved = own && *own ? skip_moved(own, in->rel) : NULL;
        if (!own || !have || (*own && !moved)) {
            s = BD_ERR_NOMEM;
        } else if (moved) {
            size_t hl = strlen(have);
            char *all = bd_sprintf("%s%s%s", have, hl && have[hl - 1] != '\n' ? "\n" : "", moved);
            s = all ? bd_source_skip_set(cat, into, all) : BD_ERR_NOMEM;
            free(all);
        }
        free(own);
        free(have);
        free(moved);
    }
    static const char *const keys[] = {"skip_list", "wait_disk", "moved_from", "scan_stopped"};
    for (size_t i = 0; s == BD_OK && i < sizeof(keys) / sizeof(keys[0]); i++) {
        char key[48];
        snprintf(key, sizeof(key), "%s.%lld", keys[i], (long long)in->id);
        if (bd_setting_del(cat, key) != 0) s = bd_fail_db(cat, "forget the folder's settings");
    }
    static const char *const gone[] = {
        "DELETE FROM temp.missing_sources WHERE source_id=?", "DELETE FROM temp.accept_gone WHERE source_id=?",
        "DELETE FROM temp.root_disks WHERE source_id=?",      "DELETE FROM temp.links WHERE source_id=?",
        "DELETE FROM sources WHERE id=?",
    };
    for (size_t i = 0; s == BD_OK && i < sizeof(gone) / sizeof(gone[0]); i++)
        if (exec_ids(cat, gone[i], in->id, 0) != 0) s = bd_fail_db(cat, "make the folder part of the new backup root");
    return s;
}

bd_status bd_source_add_holding(bd_catalog *cat, const char *folder, int64_t *out_id)
{
    char *path = root_path(folder);
    if (!path) return BD_ERR_NOMEM;
    inner_root *in;
    int n, by_link;
    bd_status s = roots_inside(cat, path, &in, &n, &by_link);
    free(path);
    if (s != BD_OK) return s;
    if (n == 0 || by_link) { /* nothing to take in, or not all of it can be: as bd_source_add */
        inner_free(in, n);
        return bd_source_add(cat, folder, out_id);
    }
    if (bd_exec(cat, "BEGIN") != 0) { inner_free(in, n); return bd_fail_db(cat, "add source"); }
    for (int i = 0; s == BD_OK && i < n; i++)
        if (exec_ids(cat, "UPDATE sources SET active=0 WHERE id=?", in[i].id, 0) != 0) s = bd_fail_db(cat, "add source");
    int64_t id = 0;
    int is_new = 0;
    if (s == BD_OK) s = bd_source_add_ex(cat, folder, &id, &is_new);
    if (s == BD_OK && !is_new) s = bd_fail(cat, BD_ERR_EXISTS, "%s is already a backup root", folder);
    for (int i = 0; s == BD_OK && i < n; i++) s = take_in(cat, id, &in[i]);
    inner_free(in, n);
    if (s == BD_OK && bd_exec(cat, "COMMIT") != 0) s = bd_fail_db(cat, "add source");
    if (s != BD_OK) {
        bd_exec(cat, "ROLLBACK");
        return s;
    }
    if (out_id) *out_id = id;
    return BD_OK;
}

bd_status bd_sources_fix_names(bd_catalog *cat, bd_log_fn log, void *log_ctx)
{
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(cat->db, "SELECT id, name FROM sources ORDER BY id", -1, &q, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "list sources");
    typedef struct { int64_t id; char *name; } bad_name;
    bad_name bad[8];
    size_t n = 0;
    while (n < sizeof(bad) / sizeof(bad[0]) && sqlite3_step(q) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(q, 1);
        if (!name_reserved(name)) continue;
        bad[n].id = sqlite3_column_int64(q, 0);
        bad[n].name = bd_strdup(name);
        if (bad[n].name) n++;
    }
    sqlite3_finalize(q);
    bd_status s = BD_OK;
    for (size_t i = 0; i < n; i++) {
        char *base = escape_reserved(bad[i].name);
        char *name = base ? bd_strdup(base) : NULL;
        for (int k = 2; name && name_taken(cat, name) && k < 10000; k++) {
            free(name);
            name = bd_sprintf("%s-%d", base, k);
        }
        sqlite3_stmt *u;
        if (!name) s = BD_ERR_NOMEM;
        else if (sqlite3_prepare_v2(cat->db, "UPDATE sources SET name=? WHERE id=?", -1, &u, NULL) != SQLITE_OK) {
            s = bd_fail_db(cat, "rename source");
        } else {
            sqlite3_bind_text(u, 1, name, -1, SQLITE_STATIC);
            sqlite3_bind_int64(u, 2, bad[i].id);
            if (sqlite3_step(u) != SQLITE_DONE) s = bd_fail_db(cat, "rename source");
            else bd_logf(log, log_ctx, "the folder %s has the name of BRODALF's own folder on the drives; its new copies go to %s instead",
                         bad[i].name, name);
            sqlite3_finalize(u);
        }
        free(base);
        free(name);
        free(bad[i].name);
    }
    return s;
}

/* ---- Skip list ------------------------------------------------------------ */

static const char DEFAULT_SKIP[] =
    "# Temporary and partly downloaded files\n"
    "*.tmp\n"
    "*.temp\n"
    "*.swp\n"
    "~$*\n"
    ".~lock.*\n"
    "*.crdownload\n"
    "*.part\n"
    "*.partial\n"
    "# System files\n"
    "Thumbs.db\n"
    "desktop.ini\n"
    ".DS_Store\n"
    "$RECYCLE.BIN/\n"
    "System Volume Information/\n"
    ".Trash-*/\n"
    ".Trashes/\n"
    "# Caches and things a program can download or build again\n"
    "node_modules/\n"
    "__pycache__/\n"
    ".cache/\n"
    ".pytest_cache/\n"
    ".gradle/\n"
    "# Windows: a user's settings, always in use, and the caches and\n"
    "# temporary files in a user's AppData folder (a folder named Cache\n"
    "# elsewhere, like in Documents, is still backed up)\n"
    "NTUSER.DAT*\n"
    "UsrClass.dat*\n"
    "INetCache/\n"
    "AppData/Local/Temp/\n"
    "*/AppData/Local/Temp/\n"
    "AppData/Local/CrashDumps/\n"
    "*/AppData/Local/CrashDumps/\n"
    "AppData/*/Crash Dumps/\n"
    "*/AppData/*/Crash Dumps/\n"
    "AppData/*/Cache/\n"
    "*/AppData/*/Cache/\n"
    "AppData/*/Code Cache/\n"
    "*/AppData/*/Code Cache/\n"
    "AppData/*/GPUCache/\n"
    "*/AppData/*/GPUCache/\n"
    "AppData/*/cache2/\n"
    "*/AppData/*/cache2/\n";

const char *bd_skip_list_default(void)
{
    return DEFAULT_SKIP;
}

#define SKIP_MAX 16384

char *bd_skip_list_get(bd_catalog *cat)
{
    char *buf = malloc(SKIP_MAX);
    if (!buf) return NULL;
    if (!bd_setting_get(cat, "skip_list", buf, SKIP_MAX)) snprintf(buf, SKIP_MAX, "%s", DEFAULT_SKIP);
    return buf;
}

char *bd_source_skip_get(bd_catalog *cat, int64_t source_id)
{
    char key[48];
    snprintf(key, sizeof(key), "skip_list.%lld", (long long)source_id);
    char *buf = malloc(SKIP_MAX);
    if (!buf) return NULL;
    if (!bd_setting_get(cat, key, buf, SKIP_MAX)) buf[0] = '\0';
    return buf;
}

bd_status bd_source_skip_set(bd_catalog *cat, int64_t source_id, const char *patterns)
{
    char key[48];
    snprintf(key, sizeof(key), "skip_list.%lld", (long long)source_id);
    if (!patterns || !*patterns) {
        sqlite3_stmt *d;
        if (sqlite3_prepare_v2(cat->db, "DELETE FROM settings WHERE key=?", -1, &d, NULL) != SQLITE_OK)
            return bd_fail_db(cat, "clear the folder's skip list");
        sqlite3_bind_text(d, 1, key, -1, SQLITE_STATIC);
        sqlite3_step(d);
        sqlite3_finalize(d);
        return BD_OK;
    }
    if (strlen(patterns) >= SKIP_MAX) return bd_fail(cat, BD_ERR_INVALID, "the skip list is too long");
    return bd_setting_set(cat, key, patterns) == 0 ? BD_OK : bd_fail_db(cat, "save the folder's skip list");
}

bd_status bd_leave_out(bd_catalog *cat, int64_t node_id)
{
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(cat->db, "SELECT source_id, rel_path, is_dir FROM nodes WHERE id=?", -1, &q, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "find the file to leave out");
    sqlite3_bind_int64(q, 1, node_id);
    if (sqlite3_step(q) != SQLITE_ROW) {
        sqlite3_finalize(q);
        return bd_fail(cat, BD_ERR_NOT_FOUND, "no such file in the catalog");
    }
    int64_t source_id = sqlite3_column_int64(q, 0);
    char *line = bd_sprintf("/%s%s", (const char *)sqlite3_column_text(q, 1), sqlite3_column_int(q, 2) ? "/" : "");
    sqlite3_finalize(q);
    char *list = bd_source_skip_get(cat, source_id);
    bd_status s = BD_OK;
    if (!line || !list) s = bd_fail(cat, BD_ERR_NOMEM, "out of memory");
    else if (bd_skip_match(list, line + 1, line[strlen(line) - 1] == '/')) s = BD_OK; /* left out already */
    else {
        size_t len = strlen(list);
        char *more = bd_sprintf("%s%s%s\n", list, len && list[len - 1] != '\n' ? "\n" : "", line);
        s = more ? bd_source_skip_set(cat, source_id, more) : bd_fail(cat, BD_ERR_NOMEM, "out of memory");
        free(more);
    }
    free(line);
    free(list);
    return s;
}

bd_status bd_source_set_active(bd_catalog *cat, int64_t source_id, int active)
{
    sqlite3_stmt *u;
    if (active) {
        /* Started again, it must not overlap a root added while it was stopped. */
        char *path = NULL;
        if (sqlite3_prepare_v2(cat->db, "SELECT path FROM sources WHERE id=?", -1, &u, NULL) != SQLITE_OK)
            return bd_fail_db(cat, "change the backup root");
        sqlite3_bind_int64(u, 1, source_id);
        if (sqlite3_step(u) == SQLITE_ROW) path = bd_strdup((const char *)sqlite3_column_text(u, 0));
        sqlite3_finalize(u);
        if (path) {
            int64_t same = 0;
            bd_status s = bd_root_check(cat, path, source_id, 1, &same);
            free(path);
            if (s != BD_OK) return s;
        }
    }
    if (sqlite3_prepare_v2(cat->db, "UPDATE sources SET active=? WHERE id=?", -1, &u, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "change the backup root");
    sqlite3_bind_int(u, 1, active ? 1 : 0);
    sqlite3_bind_int64(u, 2, source_id);
    int rc = sqlite3_step(u);
    sqlite3_finalize(u);
    if (rc != SQLITE_DONE) return bd_fail_db(cat, "change the backup root");
    if (sqlite3_changes(cat->db) == 0) return bd_fail(cat, BD_ERR_NOT_FOUND, "no such backup root");
    return BD_OK;
}

bd_status bd_skip_list_set(bd_catalog *cat, const char *patterns)
{
    if (!patterns) {
        sqlite3_stmt *d;
        if (sqlite3_prepare_v2(cat->db, "DELETE FROM settings WHERE key='skip_list'", -1, &d, NULL) != SQLITE_OK)
            return bd_fail_db(cat, "reset the skip list");
        sqlite3_step(d);
        sqlite3_finalize(d);
        return BD_OK;
    }
    if (strlen(patterns) >= SKIP_MAX) return bd_fail(cat, BD_ERR_INVALID, "the skip list is too long");
    return bd_setting_set(cat, "skip_list", patterns) == 0 ? BD_OK : bd_fail_db(cat, "save the skip list");
}

typedef struct { char *pat; int dir_only, whole_path; } skip_rule;
typedef struct { skip_rule *rules; int n; } skip_list;

static void skip_free(skip_list *l)
{
    for (int i = 0; i < l->n; i++) free(l->rules[i].pat);
    free(l->rules);
    l->rules = NULL;
    l->n = 0;
}

/* 1 if s has a byte beyond ASCII. */
static int has_high(const char *s)
{
    for (; *s; s++)
        if ((unsigned char)*s >= 0x80) return 1;
    return 0;
}

static void skip_parse(const char *text, skip_list *l)
{
    l->rules = NULL;
    l->n = 0;
    for (const char *p = text ? text : ""; *p;) {
        const char *end = p;
        while (*end && *end != '\n') end++;
        const char *a = p, *b = end;
        while (a < b && (*a == ' ' || *a == '\t')) a++;
        while (b > a && (b[-1] == ' ' || b[-1] == '\t' || b[-1] == '\r')) b--;
        p = *end ? end + 1 : end;
        if (a == b || *a == '#') continue;
        skip_rule r = {0};
        int anchored = *a == '/' || *a == '\\'; /* a path from the top of the root */
        size_t n = (size_t)(b - a);
        r.pat = malloc(n + 1);
        if (!r.pat) continue;
        memcpy(r.pat, a, n);
        r.pat[n] = '\0';
        for (char *c = r.pat; *c; c++) if (*c == '\\') *c = '/';
        if (n > 1 && r.pat[n - 1] == '/') { r.dir_only = 1; r.pat[--n] = '\0'; }
        while (r.pat[0] == '/') memmove(r.pat, r.pat + 1, strlen(r.pat));
        r.whole_path = anchored || strchr(r.pat, '/') != NULL;
        if (!r.pat[0]) { free(r.pat); continue; }
        /* Letters beyond ASCII ignore case as the file system's names do
         * (bd_fold_case): the pattern is folded here, a name when matched. */
        if (has_high(r.pat)) {
            char *f = bd_fold_case(r.pat);
            if (f) { free(r.pat); r.pat = f; }
        }
        skip_rule *nr = realloc(l->rules, sizeof(skip_rule) * (size_t)(l->n + 1));
        if (!nr) { free(r.pat); continue; }
        l->rules = nr;
        l->rules[l->n++] = r;
    }
}

static int lower_c(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

/* The character after the one s starts (UTF-8). */
static const char *next_char(const char *s)
{
    s++;
    while ((*s & 0xC0) == 0x80) s++;
    return s;
}

/* * and ? wildcards, ignoring ASCII case (and other case where both were
 * folded); ? is one character, not one byte. */
static int glob_match(const char *p, const char *s)
{
    const char *star = NULL, *mark = NULL;
    while (*s) {
        if (*p == '*') { star = p++; mark = s; continue; }
        if (*p == '?') { p++; s = next_char(s); continue; }
        if (*p && lower_c((unsigned char)*p) == lower_c((unsigned char)*s)) { p++; s++; continue; }
        if (star) { p = star + 1; s = mark = next_char(mark); continue; }
        return 0;
    }
    while (*p == '*') p++;
    return *p == '\0';
}

/* rel as patterns are matched against it: folded when it has letters
 * beyond ASCII (*fold gets the copy to free), else rel itself. */
static const char *skip_key(const char *rel, char **fold)
{
    *fold = has_high(rel) ? bd_fold_case(rel) : NULL;
    return *fold ? *fold : rel;
}

/* key: from skip_key. */
static int skip_one_key(const skip_list *l, const char *key, int is_dir)
{
    const char *name = strrchr(key, '/');
    name = name ? name + 1 : key;
    for (int i = 0; i < l->n; i++) {
        const skip_rule *r = &l->rules[i];
        if (r->dir_only && !is_dir) continue;
        if (glob_match(r->pat, r->whole_path ? key : name)) return 1;
    }
    return 0;
}

/* The item itself, or any folder it is in. */
static int skip_any(const skip_list *l, const char *rel, int is_dir)
{
    if (!l->n) return 0;
    char *fold;
    const char *key = skip_key(rel, &fold);
    if (skip_one_key(l, key, is_dir)) { free(fold); return 1; }
    char *dir = bd_strdup(key);
    free(fold);
    int hit = 0;
    for (char *slash = dir ? strrchr(dir, '/') : NULL; slash && !hit; slash = strrchr(dir, '/')) {
        *slash = '\0';
        hit = skip_one_key(l, dir, 1);
    }
    free(dir);
    return hit;
}

int bd_skip_match(const char *patterns, const char *rel_path, int is_dir)
{
    skip_list l;
    skip_parse(patterns, &l);
    char *rel = bd_strdup(rel_path ? rel_path : "");
    if (rel) for (char *c = rel; *c; c++) if (*c == '\\') *c = '/';
    int hit = rel ? skip_any(&l, rel, is_dir) : 0;
    free(rel);
    skip_free(&l);
    return hit;
}

/* ---- Hashing on more than one core ---------------------------------------
 *
 * New and changed files are hashed by a few worker threads while the walk
 * goes on. Results come back in the order the walk found the files and are
 * written to the catalog by the scanning thread alone, so the catalog ends
 * up exactly as a one-file-at-a-time scan leaves it. The threads start with
 * the first file that needs hashing; a scan that finds nothing new starts
 * none. */
#define HASH_SLOTS 64
#define HASH_MAX_WORKERS 8

int bd_test_scan_workers = 0;
const char *bd_test_online_only = NULL;
int bd_test_no_change_time = 0;

static int bd_rel_ends_with(const char *rel, const char *tail)
{
    size_t a = strlen(rel), b = strlen(tail);
    return a >= b && strcmp(rel + a - b, tail) == 0;
}

typedef struct {
    /* From the walk. */
    char *path;     /* what to read (the file itself) */
    char *full;     /* the file itself */
    char *rel;
    int is_new, was_deleted;
    int bitrot;     /* unchanged by size and time: read again to find damage */
    int64_t node_id, old_version, size, mtime_ns, btime_ns;
    int64_t ctime_ns, old_ctime; /* its change time now (0: not known), and as kept (had_ctime) */
    int had_ctime;
    char expect[BD_HASH_HEX_LEN + 1]; /* with bitrot: the hash on record */
    /* From the worker. */
    int done, rc, in_use;
    int64_t hashed;
    char hash[BD_HASH_HEX_LEN + 1];
} hash_job;

typedef struct {
    int workers;    /* wanted; 0 hashes on the scanning thread */
    int started;
    bd_thread *threads[HASH_MAX_WORKERS];
    bd_mutex *m;
    bd_cond *work, *done;
    hash_job slot[HASH_SLOTS];
    int64_t submitted, claimed, applied;
    int64_t ticked; /* bytes the workers read, not yet reported */
    int stop;
    int low;        /* the low_priority option */
} hash_pool;

typedef struct {
    bd_catalog *cat;
    const skip_list *skip;      /* the catalog's list */
    const skip_list *own_skip;  /* this backup root's own list */
    int bitrot;
    int64_t source_id;
    const char *root;
    int64_t scan_ms;
    bd_scan_stats *stats;
    bd_log_fn log;
    void *log_ctx;
    sqlite3_stmt *find_node, *find_parent, *insert_node, *touch_node, *update_file;
    sqlite3_stmt *find_version, *next_version_no, *insert_version;
    sqlite3_stmt *find_case, *rename_node, *note_new, *note_damaged, *clear_damaged;
    sqlite3_stmt *note_online, *note_link, *set_ctime;
    int have_damaged; /* damaged_files has rows: a file read again may clear one */
    char **failed_dirs; /* relative folders that could not be read */
    size_t failed_len, failed_cap;
    int db_error;
    int cancelled;
    int64_t inserted_files; /* files new to the catalog in this root */
    hash_pool *pool;
    char *own_keys[12]; /* the open database and its side files, when in this root */
    int n_own_keys;
} scan_ctx;

static int64_t lookup_parent(scan_ctx *c, const char *rel)
{
    char *dir = bd_rel_dirname(rel);
    int64_t id = 0;
    if (dir && *dir) {
        sqlite3_reset(c->find_parent);
        sqlite3_bind_int64(c->find_parent, 1, c->source_id);
        sqlite3_bind_text(c->find_parent, 2, dir, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(c->find_parent) == SQLITE_ROW) id = sqlite3_column_int64(c->find_parent, 0);
    }
    free(dir);
    return id;
}

static void bind_parent(sqlite3_stmt *st, int idx, int64_t parent)
{
    if (parent) sqlite3_bind_int64(st, idx, parent);
    else sqlite3_bind_null(st, idx);
}

#define SQL_FIND_VERSION "SELECT id, version_no FROM versions WHERE node_id=? AND hash=?"
#define SQL_NEXT_VERSION_NO "SELECT COALESCE(MAX(version_no),0)+1 FROM versions WHERE node_id=?"
#define SQL_INSERT_VERSION "INSERT INTO versions(node_id,version_no,hash,size,mtime_ns,first_seen_ms) VALUES(?,?,?,?,?,?)"

/* The version id for this hash, creating the next version if new; 0 on a
 * database error. The same hash is the same version, whatever the size or
 * time say. */
static int64_t version_for(sqlite3 *db, sqlite3_stmt *find, sqlite3_stmt *next, sqlite3_stmt *ins, int64_t node_id,
                           const char *hash, int64_t size, int64_t mtime_ns, int64_t now_ms, int *created, int64_t *no_out)
{
    *created = 0;
    sqlite3_reset(find);
    sqlite3_bind_int64(find, 1, node_id);
    sqlite3_bind_text(find, 2, hash, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(find) == SQLITE_ROW) {
        if (no_out) *no_out = sqlite3_column_int64(find, 1);
        return sqlite3_column_int64(find, 0);
    }
    sqlite3_reset(next);
    sqlite3_bind_int64(next, 1, node_id);
    int64_t no = 1;
    if (sqlite3_step(next) == SQLITE_ROW) no = sqlite3_column_int64(next, 0);

    sqlite3_reset(ins);
    sqlite3_bind_int64(ins, 1, node_id);
    sqlite3_bind_int64(ins, 2, no);
    sqlite3_bind_text(ins, 3, hash, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(ins, 4, size);
    sqlite3_bind_int64(ins, 5, mtime_ns);
    sqlite3_bind_int64(ins, 6, now_ms);
    if (sqlite3_step(ins) != SQLITE_DONE) return 0;
    *created = 1;
    if (no_out) *no_out = no;
    return sqlite3_last_insert_rowid(db);
}

static int64_t ensure_version(scan_ctx *c, int64_t node_id, const char *hash, int64_t size, int64_t mtime_ns, int *created)
{
    return version_for(c->cat->db, c->find_version, c->next_version_no, c->insert_version, node_id, hash, size, mtime_ns,
                       c->scan_ms, created, NULL);
}

int bd_record_version(bd_catalog *cat, int64_t node_id, const char *hash, int64_t size, int64_t mtime_ns,
                      int64_t *version_id, int64_t *version_no)
{
    sqlite3_stmt *find = NULL, *next = NULL, *ins = NULL, *upd = NULL;
    int rc = -1;
    if (sqlite3_prepare_v2(cat->db, SQL_FIND_VERSION, -1, &find, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(cat->db, SQL_NEXT_VERSION_NO, -1, &next, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(cat->db, SQL_INSERT_VERSION, -1, &ins, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(cat->db, "UPDATE nodes SET size=?, mtime_ns=?, current_version_id=? WHERE id=?", -1, &upd, NULL) != SQLITE_OK)
        goto done;
    int created = 0;
    int64_t no = 0;
    int64_t id = version_for(cat->db, find, next, ins, node_id, hash, size, mtime_ns, bd_now_ms(), &created, &no);
    if (!id) goto done;
    sqlite3_bind_int64(upd, 1, size);
    sqlite3_bind_int64(upd, 2, mtime_ns);
    sqlite3_bind_int64(upd, 3, id);
    sqlite3_bind_int64(upd, 4, node_id);
    if (sqlite3_step(upd) != SQLITE_DONE) goto done;
    if (version_id) *version_id = id;
    if (version_no) *version_no = no;
    rc = 0;
done:
    sqlite3_finalize(find);
    sqlite3_finalize(next);
    sqlite3_finalize(ins);
    sqlite3_finalize(upd);
    return rc;
}

/* The file was read and found right, or recorded as it is now: it is no
 * longer listed as damaged. 0, or -1 (db_error set) on a database error. */
static int forget_damage(scan_ctx *c, int64_t node_id)
{
    if (!c->have_damaged) return 0;
    sqlite3_reset(c->clear_damaged);
    sqlite3_bind_int64(c->clear_damaged, 1, node_id);
    if (sqlite3_step(c->clear_damaged) != SQLITE_DONE) { c->db_error = 1; return -1; }
    return 0;
}

/* Keep the change time a file has now (file_change_times). 0, or -1
 * (db_error set) on a database error. */
static int note_ctime(scan_ctx *c, int64_t node_id, int64_t ctime_ns)
{
    sqlite3_reset(c->set_ctime);
    sqlite3_bind_int64(c->set_ctime, 1, node_id);
    sqlite3_bind_int64(c->set_ctime, 2, ctime_ns);
    if (sqlite3_step(c->set_ctime) != SQLITE_DONE) { c->db_error = 1; return -1; }
    return 0;
}

/* Record one hashed file, as the scan always did. Frees the job's strings. */
static void apply_hash(scan_ctx *c, hash_job *j)
{
    if (c->db_error) goto done;
    if (j->rc == -4) { c->cancelled = 1; goto done; } /* the scan is being stopped */
    if (j->rc != 0 && j->in_use) {
        bd_note_in_use(c->cat, j->full, j->size);
        c->stats->files_in_use++;
        bd_logf(c->log, c->log_ctx, "in use by another program, skipped: %s", j->rel);
        goto done;
    }
    if (j->rc != 0) {
        c->stats->errors++;
        bd_logf(c->log, c->log_ctx, "cannot read %s; keeping what the catalog already knows", j->rel);
        goto done;
    }
    c->stats->bytes_hashed += j->hashed;
    /* Read now: its change time is kept, so it is not read again until
     * that moves (or its size or modified time do). */
    if (j->ctime_ns && (!j->had_ctime || j->old_ctime != j->ctime_ns) && note_ctime(c, j->node_id, j->ctime_ns) != 0) goto done;

    if (j->bitrot) {
        /* Same size and time as before, but are the contents? A file that
         * changed without its time changing is damage (or a program that
         * resets times): note it, and keep the version on record, whose
         * copies on the drives are the good ones. */
        if (strcmp(j->hash, j->expect) != 0) {
            sqlite3_reset(c->note_damaged);
            sqlite3_bind_int64(c->note_damaged, 1, j->node_id);
            sqlite3_bind_int64(c->note_damaged, 2, j->old_version);
            sqlite3_bind_text(c->note_damaged, 3, j->hash, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(c->note_damaged, 4, c->scan_ms);
            if (sqlite3_step(c->note_damaged) != SQLITE_DONE) { c->db_error = 1; goto done; }
            c->have_damaged = 1;
            c->stats->files_damaged++;
            bd_logf(c->log, c->log_ctx, "contents changed but size and time did not (damage?): %s", j->rel);
        } else if (forget_damage(c, j->node_id) != 0) {
            goto done; /* right again (restored, say): no longer damaged */
        }
        if (j->was_deleted) c->stats->files_new++;
        goto done;
    }

    int created = 0;
    int64_t version_id = ensure_version(c, j->node_id, j->hash, j->hashed, j->mtime_ns, &created);
    if (!version_id) { c->db_error = 1; goto done; }

    sqlite3_reset(c->update_file);
    sqlite3_bind_int64(c->update_file, 1, j->hashed);
    sqlite3_bind_int64(c->update_file, 2, j->mtime_ns);
    sqlite3_bind_int64(c->update_file, 3, version_id);
    sqlite3_bind_int64(c->update_file, 4, j->node_id);
    if (sqlite3_step(c->update_file) != SQLITE_DONE) { c->db_error = 1; goto done; }
    /* Read and recorded as it is now: right again, or a new version, and
     * either way the damage once found no longer stands. */
    if (forget_damage(c, j->node_id) != 0) goto done;

    if (j->is_new || j->was_deleted) c->stats->files_new++;
    else if (version_id != j->old_version) c->stats->files_changed++;
done:
    if (j->path != j->full) free(j->path);
    free(j->full);
    free(j->rel);
    j->path = j->full = j->rel = NULL;
}

static void pool_tick(void *ctx, int64_t bytes)
{
    hash_pool *p = ctx;
    bd_mutex_lock(p->m);
    p->ticked += bytes;
    bd_cond_signal(p->done);
    bd_mutex_unlock(p->m);
}

static void pool_worker(void *arg)
{
    hash_pool *p = arg;
    if (p->low) bd_thread_low_priority();
    bd_io_redirect(pool_tick, p);
    bd_mutex_lock(p->m);
    for (;;) {
        while (p->claimed == p->submitted && !p->stop) bd_cond_wait(p->work, p->m);
        if (p->claimed == p->submitted) break;
        hash_job *j = &p->slot[p->claimed % HASH_SLOTS];
        p->claimed++;
        bd_mutex_unlock(p->m);
        j->rc = bd_hash_file(j->path, NULL, j->hash, &j->hashed);
        j->in_use = j->rc != 0 && bd_open_was_in_use();
        bd_mutex_lock(p->m);
        j->done = 1;
        bd_cond_signal(p->done);
    }
    bd_mutex_unlock(p->m);
    bd_io_redirect(NULL, NULL);
}

static void pool_stop(hash_pool *p)
{
    if (!p->started) return;
    bd_mutex_lock(p->m);
    p->stop = 1;
    bd_cond_broadcast(p->work);
    bd_mutex_unlock(p->m);
    for (int i = 0; i < p->workers; i++) bd_thread_join(p->threads[i]);
    bd_cond_free(p->work);
    bd_cond_free(p->done);
    bd_mutex_free(p->m);
    p->started = 0;
}

static int pool_start(hash_pool *p)
{
    p->m = bd_mutex_new();
    p->work = bd_cond_new();
    p->done = bd_cond_new();
    int n = 0;
    if (p->m && p->work && p->done)
        while (n < p->workers && (p->threads[n] = bd_thread_start(pool_worker, p)) != NULL) n++;
    if (n == 0) {
        bd_cond_free(p->work);
        bd_cond_free(p->done);
        bd_mutex_free(p->m);
        p->workers = 0; /* hash on the scanning thread from now on */
        return -1;
    }
    p->workers = n;
    p->started = 1;
    return 0;
}

/* Record finished results in order, waiting until `until` have been taken
 * (0: only what is ready). With apply 0 they are dropped (the scan is being
 * abandoned); the workers still finish what they started. */
static void pool_collect(scan_ctx *c, int64_t until, int apply)
{
    hash_pool *p = c->pool;
    if (!p || !p->started) return;
    bd_mutex_lock(p->m);
    for (;;) {
        int64_t ticked = p->ticked;
        p->ticked = 0;
        if (ticked) {
            bd_mutex_unlock(p->m);
            bd_io_tick(ticked);
            bd_mutex_lock(p->m);
        }
        if (p->applied == p->submitted) break;
        hash_job *j = &p->slot[p->applied % HASH_SLOTS];
        if (!j->done) {
            if (p->applied >= until) break;
            bd_cond_wait(p->done, p->m);
            continue;
        }
        bd_mutex_unlock(p->m);
        if (apply) apply_hash(c, j);
        else {
            if (j->path != j->full) free(j->path);
            free(j->full);
            free(j->rel);
        }
        bd_mutex_lock(p->m);
        p->applied++;
    }
    bd_mutex_unlock(p->m);
}

/* Hand a file to the workers. -1 when there are none: hash it here. */
static int pool_submit(scan_ctx *c, const hash_job *in)
{
    hash_pool *p = c->pool;
    if (!p || p->workers <= 0) return -1;
    if (!p->started && pool_start(p) != 0) return -1;
    if (p->submitted - p->applied == HASH_SLOTS) pool_collect(c, p->applied + 1, 1);
    bd_mutex_lock(p->m);
    hash_job *j = &p->slot[p->submitted % HASH_SLOTS];
    *j = *in;
    j->done = 0;
    p->submitted++;
    bd_cond_signal(p->work);
    bd_mutex_unlock(p->m);
    pool_collect(c, 0, 1);
    return 0;
}

/* BRODALF's own folder for this database inside a backup root: an extra
 * place (a Dropbox folder in the backed-up home folder, say) or a drive
 * folder there. Its copies are never scanned as files to back up again. */
static int is_own_store(const scan_ctx *c, const char *rel)
{
    const char *base = bd_rel_basename(rel);
    const char *want = "BRODALF";
    for (; *base && *want; base++, want++)
        if (tolower((unsigned char)*base) != tolower((unsigned char)*want)) return 0;
    if (*base || *want) return 0;
    char *in = bd_sprintf("%s/%s/BRODALF.media", rel, bd_catalog_uuid(c->cat));
    char *full = in ? bd_path_join(c->root, in) : NULL;
    bd_stat_t st;
    int own = full && bd_stat(full, &st) == 0 && st.is_file;
    free(in);
    free(full);
    return own;
}

/* The open database and the files kept beside it (its lock, a save or a
 * recovery under way, the working copy): a backup root that holds them
 * never backs them up, or every backup would add the whole database again.
 * That holds when the database was opened by another way to the same
 * folder (a mapped drive letter, a link): the key is then the one the file
 * has in the root as the scan writes it. */
static char *own_key(const char *rk, const root_keys *root, const char *file)
{
    char *k = root_key(file);
    if (k && key_inside(k, rk)) return k;
    free(k);
    root_keys f;
    if (!root->real || !keys_of(file, &f)) return NULL;
    char *out = NULL;
    if (key_inside(f.real, root->real)) {
        const char *rest = f.real + strlen(root->real);
        if (*rest == '/') rest++;
        out = bd_sprintf("%s%s%s", rk, rk[0] && rk[strlen(rk) - 1] == '/' ? "" : "/", rest);
    }
    keys_free(&f);
    return out;
}

static void find_own_files(scan_ctx *c)
{
    const bd_catalog *cat = c->cat;
    char *rk = root_key(c->root);
    if (!rk) return;
    static const char *const side[] = {"", ".lock", ".saving", ".recovering"};
    static const char *const work[] = {"", "-wal", "-shm", "-journal", ".snapshot"};
    char *cand[12];
    int n = 0;
    for (size_t i = 0; cat->path && i < sizeof(side) / sizeof(side[0]); i++) cand[n++] = bd_sprintf("%s%s", cat->path, side[i]);
    for (size_t i = 0; cat->work_path && i < sizeof(work) / sizeof(work[0]); i++) cand[n++] = bd_sprintf("%s%s", cat->work_path, work[i]);
    if (cat->owner_path) cand[n++] = bd_sprintf("%s", cat->owner_path);
    root_keys root = {NULL, NULL};
    if (!keys_of(c->root, &root)) root.key = root.real = NULL;
    for (int i = 0; i < n; i++) {
        char *k = cand[i] ? own_key(rk, &root, cand[i]) : NULL;
        if (k && c->n_own_keys < (int)(sizeof(c->own_keys) / sizeof(c->own_keys[0])))
            c->own_keys[c->n_own_keys++] = k;
        else
            free(k);
        free(cand[i]);
    }
    keys_free(&root);
    free(rk);
}

static int is_own_file(const scan_ctx *c, const char *rel)
{
    if (!c->n_own_keys) return 0;
    char *full = bd_path_join(c->root, rel);
    char *k = full ? root_key(full) : NULL;
    int own = 0;
    for (int i = 0; k && !own && i < c->n_own_keys; i++) own = strcmp(k, c->own_keys[i]) == 0;
    free(full);
    free(k);
    return own;
}

char *bd_root_holding(bd_catalog *cat, const char *path)
{
    char *key = cat && path ? root_key(path) : NULL;
    if (!key) return NULL;
    sqlite3_stmt *q;
    char *found = NULL;
    if (sqlite3_prepare_v2(cat->db, "SELECT path FROM sources WHERE active=1 ORDER BY id", -1, &q, NULL) == SQLITE_OK) {
        while (!found && sqlite3_step(q) == SQLITE_ROW) {
            const char *root = (const char *)sqlite3_column_text(q, 0);
            char *okey = root ? root_key(root) : NULL;
            if (okey && key_inside(key, okey)) found = bd_strdup(root);
            free(okey);
        }
        sqlite3_finalize(q);
    }
    free(key);
    return found;
}

int bd_folders_overlap(const char *a, const char *b)
{
    root_keys ka, kb;
    if (!a || !b || !keys_of(a, &ka)) return 0;
    if (!keys_of(b, &kb)) {
        keys_free(&ka);
        return 0;
    }
    int r = keys_same(&ka, &kb) ? 1 : keys_inside(&ka, &kb) ? 2 : keys_inside(&kb, &ka) ? 3 : 0;
    keys_free(&ka);
    keys_free(&kb);
    return r;
}

/* The folder p is in ("C:\x" for "C:\x\y", "C:\" for "C:\x", "/" for
 * "/x"), or NULL at the top (of a drive, or of a network share). */
static char *folder_above(const char *p)
{
    size_t n = strlen(p);
    while (n > 1 && (p[n - 1] == '/' || p[n - 1] == '\\')) n--;
    size_t i = n;
    while (i > 0 && p[i - 1] != '/' && p[i - 1] != '\\') i--;
    if (i == 0 || i == n) return NULL;
    size_t keep = i;
    if (keep > 1 && !(keep == 3 && p[1] == ':')) keep--; /* the '/' after it, but not a top folder's own */
    int unc = (p[0] == '/' || p[0] == '\\') && (p[1] == '/' || p[1] == '\\');
    if (unc) { /* not above \\server\share */
        int seps = 0;
        for (size_t k = 2; k < keep; k++) seps += p[k] == '/' || p[k] == '\\';
        if (seps < 1) return NULL;
    }
    return bd_sprintf("%.*s", (int)keep, p);
}

typedef struct { const char *dir; int found; } drive_look;

static int drive_look_cb(void *ctx, const char *rel, const bd_stat_t *st)
{
    drive_look *l = ctx;
    if (!st->is_dir) return 0;
    char *f = bd_sprintf("%s/%s/%s", l->dir, rel, BD_MEDIA_FILE);
    bd_media_file mf;
    if (f && bd_media_file_read(f, &mf) == 0) l->found = 1;
    free(f);
    return l->found ? 1 : BD_WALK_SKIP;
}

char *bd_backup_drive_holding(const char *path)
{
    char *d = path ? bd_full_path(path) : NULL;
    while (d) {
        char *m = bd_path_join(d, BD_MEDIA_DIR);
        bd_stat_t st;
        if (m && bd_stat(m, &st) == 0 && st.is_dir && !st.is_link) {
            drive_look l = {m, 0};
            bd_walk(m, drive_look_cb, NULL, &l);
            if (l.found) {
                free(m);
                return d;
            }
        }
        free(m);
        char *up = folder_above(d);
        free(d);
        d = up;
    }
    return NULL;
}

/* 1 if path is in an active backup root. */
static int in_a_root(bd_catalog *cat, const char *path)
{
    char *key = root_key(path);
    if (!key) return 0;
    sqlite3_stmt *q;
    int in = 0;
    if (sqlite3_prepare_v2(cat->db, "SELECT path FROM sources WHERE active=1", -1, &q, NULL) == SQLITE_OK) {
        while (!in && sqlite3_step(q) == SQLITE_ROW) {
            char *okey = root_key((const char *)sqlite3_column_text(q, 0));
            in = okey && (strcmp(key, okey) == 0 || key_inside(key, okey));
            free(okey);
        }
        sqlite3_finalize(q);
    }
    free(key);
    return in;
}

/* A link the scan does not follow: listed when what it leads to is in no
 * backup root, so nothing backs it up. */
static void note_link(scan_ctx *c, const char *rel)
{
    char *full = bd_path_join(c->root, rel);
    int hidden_system = 0;
    char *target = full ? bd_link_target(full, &hidden_system) : NULL;
    free(full);
    bd_stat_t t;
    if (!target || hidden_system || bd_stat(target, &t) != 0 || (!t.is_dir && !t.is_file) || in_a_root(c->cat, target)) {
        bd_logf(c->log, c->log_ctx, "skipped link: %s", rel);
        free(target);
        return;
    }
    c->stats->links_not_followed++;
    bd_logf(c->log, c->log_ctx, "link not followed, so %s is not backed up: %s", target, rel);
    sqlite3_reset(c->note_link);
    sqlite3_bind_int64(c->note_link, 1, c->source_id);
    sqlite3_bind_text(c->note_link, 2, rel, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(c->note_link, 3, target, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(c->note_link, 4, t.is_dir);
    sqlite3_step(c->note_link);
    free(target);
}

bd_status bd_list_links(bd_catalog *cat, bd_link_fn fn, void *ctx)
{
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT l.source_id, s.name, l.rel_path, l.target, l.is_dir FROM temp.links l"
                           " JOIN sources s ON s.id=l.source_id WHERE s.active=1"
                           " ORDER BY s.name COLLATE NOCASE, l.rel_path COLLATE NOCASE",
                           -1, &q, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "list links");
    while (sqlite3_step(q) == SQLITE_ROW) {
        bd_link_info info;
        info.source_id = sqlite3_column_int64(q, 0);
        info.source_name = (const char *)sqlite3_column_text(q, 1);
        info.rel_path = (const char *)sqlite3_column_text(q, 2);
        info.target = (const char *)sqlite3_column_text(q, 3);
        info.is_dir = sqlite3_column_int(q, 4);
        if (in_a_root(cat, info.target)) continue; /* added as a root since */
        if (fn(ctx, &info) != 0) break;
    }
    sqlite3_finalize(q);
    return BD_OK;
}

char *bd_path_link_target(const char *path)
{
    bd_stat_t st;
    if (bd_stat(path, &st) != 0 || !st.is_link) return NULL;
    return bd_link_target(path, NULL);
}

static int scan_entry(void *ctx, const char *rel, const bd_stat_t *st)
{
    scan_ctx *c = ctx;
    if (bd_cancelled(c->cat)) { c->cancelled = 1; return 1; }
    if (st->is_link) {
        c->stats->skipped_links++;
        note_link(c, rel);
        return 0;
    }
    if (!st->is_dir && !st->is_file) return 0;
    /* Parent folders on the skip lists were never entered. */
    char *fold = NULL;
    const char *skey = c->skip->n || c->own_skip->n ? skip_key(rel, &fold) : rel;
    int skipped = skip_one_key(c->skip, skey, st->is_dir) || skip_one_key(c->own_skip, skey, st->is_dir);
    free(fold);
    if (skipped) {
        c->stats->skipped++;
        return st->is_dir ? BD_WALK_SKIP : 0;
    }
    if (st->is_dir && is_own_store(c, rel)) return BD_WALK_SKIP;
    if (st->is_file && is_own_file(c, rel)) return 0;

    sqlite3_reset(c->find_node);
    sqlite3_bind_int64(c->find_node, 1, c->source_id);
    sqlite3_bind_text(c->find_node, 2, rel, -1, SQLITE_TRANSIENT);
    int64_t node_id = 0, old_size = -1, old_mtime = -1, old_version = 0, old_ctime = 0;
    int was_deleted = 0, had_ctime = 0;
    char old_hash[BD_HASH_HEX_LEN + 1] = "";
    if (sqlite3_step(c->find_node) == SQLITE_ROW) {
        node_id = sqlite3_column_int64(c->find_node, 0);
        old_size = sqlite3_column_int64(c->find_node, 1);
        old_mtime = sqlite3_column_int64(c->find_node, 2);
        old_version = sqlite3_column_int64(c->find_node, 3);
        was_deleted = sqlite3_column_int(c->find_node, 4);
        const char *h = (const char *)sqlite3_column_text(c->find_node, 5);
        if (h) snprintf(old_hash, sizeof(old_hash), "%s", h);
        had_ctime = sqlite3_column_type(c->find_node, 6) != SQLITE_NULL;
        old_ctime = sqlite3_column_int64(c->find_node, 6);
    }
    /* Where names ignore case (Windows), Photo.JPG that is now photo.jpg
     * was renamed: keep its node, versions and copies under the new name. */
    char *key = node_id ? NULL : bd_path_key(rel);
    if (key) {
        sqlite3_reset(c->find_case);
        sqlite3_bind_int64(c->find_case, 1, c->source_id);
        sqlite3_bind_text(c->find_case, 2, key, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(c->find_case, 3, c->scan_ms);
        if (sqlite3_step(c->find_case) == SQLITE_ROW) {
            node_id = sqlite3_column_int64(c->find_case, 0);
            old_size = sqlite3_column_int64(c->find_case, 1);
            old_mtime = sqlite3_column_int64(c->find_case, 2);
            old_version = sqlite3_column_int64(c->find_case, 3);
            was_deleted = sqlite3_column_int(c->find_case, 4);
            const char *h = (const char *)sqlite3_column_text(c->find_case, 5);
            if (h) snprintf(old_hash, sizeof(old_hash), "%s", h);
            had_ctime = sqlite3_column_type(c->find_case, 6) != SQLITE_NULL;
            old_ctime = sqlite3_column_int64(c->find_case, 6);
            sqlite3_reset(c->find_case);
            sqlite3_reset(c->rename_node);
            sqlite3_bind_text(c->rename_node, 1, rel, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(c->rename_node, 2, bd_rel_basename(rel), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(c->rename_node, 3, node_id);
            if (sqlite3_step(c->rename_node) != SQLITE_DONE) { free(key); c->db_error = 1; return 1; }
        }
        sqlite3_reset(c->find_case);
    }
    int64_t parent = lookup_parent(c, rel);

    if (st->is_dir) c->stats->dirs_seen++;
    else c->stats->files_seen++;
    bd_report(c->cat, c->stats->files_seen, rel, 0);

    if (!node_id) {
        sqlite3_reset(c->insert_node);
        sqlite3_bind_int64(c->insert_node, 1, c->source_id);
        bind_parent(c->insert_node, 2, parent);
        sqlite3_bind_text(c->insert_node, 3, rel, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(c->insert_node, 4, bd_rel_basename(rel), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(c->insert_node, 5, st->is_dir);
        sqlite3_bind_int64(c->insert_node, 6, c->scan_ms);
        if (key) sqlite3_bind_text(c->insert_node, 7, key, -1, SQLITE_TRANSIENT);
        else sqlite3_bind_null(c->insert_node, 7);
        sqlite3_bind_int64(c->insert_node, 8, st->btime_ns);
        if (sqlite3_step(c->insert_node) != SQLITE_DONE) { free(key); c->db_error = 1; return 1; }
        node_id = sqlite3_last_insert_rowid(c->cat->db);
        if (!st->is_dir) {
            c->inserted_files++;
            /* Perhaps a file moved here: matched after the scan. */
            sqlite3_reset(c->note_new);
            sqlite3_bind_int64(c->note_new, 1, node_id);
            sqlite3_step(c->note_new);
        }
    } else {
        sqlite3_reset(c->touch_node);
        bind_parent(c->touch_node, 1, parent);
        sqlite3_bind_int(c->touch_node, 2, st->is_dir);
        sqlite3_bind_int64(c->touch_node, 3, c->scan_ms);
        sqlite3_bind_int64(c->touch_node, 4, st->btime_ns);
        sqlite3_bind_int64(c->touch_node, 5, node_id);
        if (sqlite3_step(c->touch_node) != SQLITE_DONE) { free(key); c->db_error = 1; return 1; }
    }
    free(key);
    if (st->is_dir) return 0;

    int is_new = old_size < 0 || !old_version; /* no version yet: first read now */
    int bitrot = 0;
    int64_t ctime_ns = bd_test_no_change_time ? 0 : st->ctime_ns;
    if (!is_new && old_version && old_size == st->size && old_mtime == st->mtime_ns) {
        /* The same size and modified time. If its change time moved, the
         * contents may have changed with the modified time put back (some
         * programs do that, VeraCrypt containers by default): read it again
         * as a changed file. The same checksum is no new version. A file
         * whose change time was never kept (scanned by an older BRODALF) is
         * only recorded now; a cloud file is never read here. */
        int ctime_moved = ctime_ns && had_ctime && old_ctime != ctime_ns && !st->online_only;
        if (!ctime_moved) {
            if (ctime_ns && !had_ctime && !st->online_only) {
                if (note_ctime(c, node_id, ctime_ns) != 0) return 1;
                had_ctime = 1;
                old_ctime = ctime_ns;
            }
            if (!c->bitrot || !old_hash[0] || st->online_only) {
                if (was_deleted) c->stats->files_new++; /* back where it once was */
                return 0;
            }
            bitrot = 1;
        }
    }
    /* A cloud file that is not on this PC would be downloaded by reading
     * it. Leave it alone: the catalog keeps what it knew, and nothing is
     * marked deleted. */
    if (st->online_only || (bd_test_online_only && bd_rel_ends_with(rel, bd_test_online_only))) {
        if (!c->stats->files_online_only++)
            bd_logf(c->log, c->log_ctx, "files that are online-only (in the cloud, not on this PC) are skipped, not downloaded");
        /* Known, but not as the catalog has it: edited on another device. */
        int changed = !is_new;
        if (changed) {
            c->stats->files_cloud_changed++;
            bd_logf(c->log, c->log_ctx, "online-only, changed in the cloud and not backed up: %s", rel);
        } else {
            bd_logf(c->log, c->log_ctx, "online-only, skipped: %s", rel);
        }
        sqlite3_reset(c->note_online);
        sqlite3_bind_int64(c->note_online, 1, node_id);
        sqlite3_bind_int(c->note_online, 2, changed);
        sqlite3_step(c->note_online);
        return 0;
    }

    hash_job j;
    memset(&j, 0, sizeof(j));
    j.full = bd_path_join(c->root, rel);
    j.rel = bd_strdup(rel);
    if (!j.full || !j.rel) {
        free(j.full);
        free(j.rel);
        c->stats->errors++;
        bd_logf(c->log, c->log_ctx, "cannot read %s; keeping what the catalog already knows", rel);
        return 0;
    }
    /* A file in use is read from its shadow copy, when the user had one taken. */
    j.path = bd_substitute_for(c->cat, j.full);
    if (!j.path) j.path = j.full;
    j.is_new = is_new;
    j.was_deleted = was_deleted;
    j.node_id = node_id;
    j.old_version = old_version;
    j.size = st->size;
    j.mtime_ns = st->mtime_ns;
    j.btime_ns = st->btime_ns;
    j.ctime_ns = st->online_only ? 0 : ctime_ns;
    j.old_ctime = old_ctime;
    j.had_ctime = had_ctime;
    j.bitrot = bitrot;
    if (bitrot) memcpy(j.expect, old_hash, sizeof(j.expect));
    if (pool_submit(c, &j) != 0) {
        j.rc = bd_hash_file(j.path, NULL, j.hash, &j.hashed);
        j.in_use = j.rc != 0 && bd_open_was_in_use();
        apply_hash(c, &j);
    }
    return c->db_error || c->cancelled ? 1 : 0;
}

static void scan_error(void *ctx, const char *path, const char *message)
{
    scan_ctx *c = ctx;
    c->stats->errors++;
    bd_logf(c->log, c->log_ctx, "cannot read %s: %s", path, message);

    /* Remember the relative folder so nothing under it is marked deleted. */
    size_t rlen = strlen(c->root);
    while (rlen > 0 && (c->root[rlen - 1] == '/' || c->root[rlen - 1] == '\\')) rlen--;
    const char *rel = strlen(path) > rlen ? path + rlen + 1 : "";
    char *copy = bd_strdup(rel);
    if (!copy) return;
    for (char *p = copy; *p; p++) if (*p == '\\') *p = '/';
    size_t n = strlen(copy);
    if (n >= 2 && strcmp(copy + n - 2, "/*") == 0) copy[n - 2] = '\0'; /* Windows search pattern */
    else if (n == 1 && copy[0] == '*') copy[0] = '\0';
    if (c->failed_len == c->failed_cap) {
        size_t cap = c->failed_cap ? c->failed_cap * 2 : 8;
        char **nd = realloc(c->failed_dirs, cap * sizeof(char *));
        if (!nd) { free(copy); return; }
        c->failed_dirs = nd;
        c->failed_cap = cap;
    }
    c->failed_dirs[c->failed_len++] = copy;
}

static int under_failed_dir(scan_ctx *c, const char *rel)
{
    for (size_t i = 0; i < c->failed_len; i++) {
        size_t n = strlen(c->failed_dirs[i]);
        if (n == 0) return 1;
        if (strncmp(rel, c->failed_dirs[i], n) == 0 && (rel[n] == '/' || rel[n] == '\0')) return 1;
    }
    return 0;
}

/* What this scan did not see is marked deleted, files newly on a skip list
 * included: they stay in the catalog with their versions and copies until
 * the user forgets them (bd_deleted_forget). With count_only, nothing is
 * marked: the files that would be are counted in *files. */
/* count_only: files gets how many files would be marked deleted, and
 * moved how many of them have their contents (checksum and size) in a
 * file new in this root in this scan: moved or renamed inside it. */
static bd_status mark_deleted(scan_ctx *c, int count_only, int64_t *files, int64_t *moved)
{
    sqlite3_stmt *q, *u, *gone, *match = NULL;
    if (files) *files = 0;
    if (moved) *moved = 0;
    if (count_only && moved) {
        sqlite3_stmt *fill;
        if (bd_exec(c->cat, "DELETE FROM temp.scan_new_hash") != 0 ||
            sqlite3_prepare_v2(c->cat->db, "INSERT OR IGNORE INTO temp.scan_new_hash(hash, size) SELECT v.hash, v.size"
                               " FROM temp.scan_new w JOIN nodes n ON n.id=w.node_id JOIN versions v ON v.id=n.current_version_id"
                               " WHERE n.source_id=? AND n.is_dir=0 AND n.deleted=0 AND v.size>0", -1, &fill, NULL) != SQLITE_OK)
            return bd_fail_db(c->cat, "find moved files");
        sqlite3_bind_int64(fill, 1, c->source_id);
        int rc = sqlite3_step(fill);
        sqlite3_finalize(fill);
        if (rc != SQLITE_DONE ||
            sqlite3_prepare_v2(c->cat->db, "SELECT 1 FROM nodes n JOIN versions v ON v.id=n.current_version_id"
                               " JOIN temp.scan_new_hash h ON h.hash=v.hash AND h.size=v.size WHERE n.id=?", -1, &match, NULL) != SQLITE_OK)
            return bd_fail_db(c->cat, "find moved files");
    }
    if (sqlite3_prepare_v2(c->cat->db, "SELECT id, rel_path, is_dir FROM nodes WHERE source_id=? AND deleted=0 AND last_seen_ms<?", -1, &q, NULL) != SQLITE_OK) {
        sqlite3_finalize(match);
        return bd_fail_db(c->cat, "find deleted files");
    }
    if (sqlite3_prepare_v2(c->cat->db, "UPDATE nodes SET deleted=1, deleted_ack=0 WHERE id=?", -1, &u, NULL) != SQLITE_OK) {
        sqlite3_finalize(q);
        sqlite3_finalize(match);
        return bd_fail_db(c->cat, "mark deleted files");
    }
    if (sqlite3_prepare_v2(c->cat->db, "INSERT OR IGNORE INTO temp.scan_gone(node_id) VALUES(?)", -1, &gone, NULL) != SQLITE_OK) {
        sqlite3_finalize(q);
        sqlite3_finalize(u);
        sqlite3_finalize(match);
        return bd_fail_db(c->cat, "mark deleted files");
    }
    sqlite3_bind_int64(q, 1, c->source_id);
    sqlite3_bind_int64(q, 2, c->scan_ms);
    while (sqlite3_step(q) == SQLITE_ROW) {
        const char *rel = (const char *)sqlite3_column_text(q, 1);
        if (under_failed_dir(c, rel)) continue;
        int is_dir = sqlite3_column_int(q, 2);
        if (count_only) {
            if (!is_dir && files) (*files)++;
            if (!is_dir && match) {
                sqlite3_reset(match);
                sqlite3_bind_int64(match, 1, sqlite3_column_int64(q, 0));
                if (sqlite3_step(match) == SQLITE_ROW) (*moved)++;
            }
            continue;
        }
        int64_t id = sqlite3_column_int64(q, 0);
        sqlite3_reset(u);
        sqlite3_bind_int64(u, 1, id);
        sqlite3_step(u);
        sqlite3_reset(gone);
        sqlite3_bind_int64(gone, 1, id);
        sqlite3_step(gone);
        if (!is_dir) c->stats->files_deleted++;
    }
    sqlite3_finalize(q);
    sqlite3_finalize(u);
    sqlite3_finalize(gone);
    sqlite3_finalize(match);
    return BD_OK;
}

/* gone: the folder is there but so many of its files were not (an
 * emptied root); 0 when the folder itself could not be found. */
static void note_missing(bd_catalog *cat, int64_t source_id, int missing, int64_t gone)
{
    sqlite3_stmt *u;
    const char *sql = missing ? "INSERT OR REPLACE INTO temp.missing_sources(source_id, files_gone) VALUES(?,?)"
                              : "DELETE FROM temp.missing_sources WHERE source_id=?";
    if (sqlite3_prepare_v2(cat->db, sql, -1, &u, NULL) != SQLITE_OK) return;
    sqlite3_bind_int64(u, 1, source_id);
    if (missing) sqlite3_bind_int64(u, 2, gone);
    sqlite3_step(u);
    sqlite3_finalize(u);
    if (!missing || gone > 0) bd_root_held_note(cat, source_id, missing ? gone : 0);
}

/* The user said the files of this root really were deleted: 1, and the
 * word is used up, so the next emptied scan asks again. */
static int take_accept_gone(bd_catalog *cat, int64_t source_id)
{
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(cat->db, "DELETE FROM temp.accept_gone WHERE source_id=?", -1, &q, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int64(q, 1, source_id);
    sqlite3_step(q);
    sqlite3_finalize(q);
    return sqlite3_changes(cat->db) > 0;
}

bd_status bd_source_accept_gone(bd_catalog *cat, int64_t source_id)
{
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(cat->db, "INSERT OR IGNORE INTO temp.accept_gone(source_id) VALUES(?)", -1, &q, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "note the deleted files");
    sqlite3_bind_int64(q, 1, source_id);
    int rc = sqlite3_step(q);
    sqlite3_finalize(q);
    return rc == SQLITE_DONE ? BD_OK : bd_fail_db(cat, "note the deleted files");
}

bd_status bd_source_wait_for_disk(bd_catalog *cat, int64_t source_id, int on)
{
    char key[48];
    snprintf(key, sizeof(key), "wait_disk.%lld", (long long)source_id);
    if (on) return bd_setting_set(cat, key, "1") == 0 ? BD_OK : bd_fail_db(cat, "remember the backup root's disk");
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(cat->db, "DELETE FROM settings WHERE key=?", -1, &q, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "remember the backup root's disk");
    sqlite3_bind_text(q, 1, key, -1, SQLITE_STATIC);
    int rc = sqlite3_step(q);
    sqlite3_finalize(q);
    return rc == SQLITE_DONE ? BD_OK : bd_fail_db(cat, "remember the backup root's disk");
}

int bd_setting_del(bd_catalog *cat, const char *key)
{
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(cat->db, "DELETE FROM settings WHERE key=?", -1, &q, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(q, 1, key, -1, SQLITE_STATIC);
    int rc = sqlite3_step(q);
    sqlite3_finalize(q);
    return rc == SQLITE_DONE ? 0 : -1;
}

/* 1 if a backup root is on a network share: a \\server\share path, or a
 * drive letter that stands for one (bd_real_path). */
static int on_network(const char *root)
{
    char *real = bd_real_path(root);
    const char *p = real ? real : root;
    int unc = (p[0] == '\\' || p[0] == '/') && (p[1] == '\\' || p[1] == '/');
    int net = unc && p[2] && ((p[2] != '?' && p[2] != '.') || strncmp(p + 2, "?\\UNC\\", 5) == 0 || strncmp(p + 2, "?/UNC/", 5) == 0);
    free(real);
    return net;
}

static bd_status scan_source(bd_catalog *cat, const skip_list *skip, int bitrot, hash_pool *pool, int64_t source_id,
                             const char *root, int64_t scan_ms, bd_scan_stats *stats, bd_log_fn log, void *log_ctx)
{
    bd_stat_t rst;
    char wait_key[48], wait_val[8];
    snprintf(wait_key, sizeof(wait_key), "wait_disk.%lld", (long long)source_id);
    int waiting = bd_setting_get(cat, wait_key, wait_val, sizeof(wait_val));
    if (bd_stat(root, &rst) != 0 || !rst.is_dir) {
        stats->roots_missing++;
        note_missing(cat, source_id, 1, 0);
        if (waiting) {
            stats->roots_waiting++;
            if (on_network(root))
                bd_logf(log, log_ctx, "backup root %s waits until its network share can be reached; skipped (nothing marked deleted)", root);
            else
                bd_logf(log, log_ctx, "backup root %s waits for its disk to be plugged in; skipped (nothing marked deleted)", root);
        } else {
            bd_logf(log, log_ctx, "backup root %s is not reachable; skipped (nothing marked deleted)", root);
        }
        return BD_OK;
    }
    if (waiting) bd_source_wait_for_disk(cat, source_id, 0);
    note_missing(cat, source_id, 0, 0);
    bd_scan_stats before = *stats;
    /* The last scan of this root was stopped part way: what it found new
     * is new to this scan too (moved files are matched against it, and it
     * says nothing about whether the folder holds the root's files). */
    char stop_key[48], stop_val[8];
    snprintf(stop_key, sizeof(stop_key), "scan_stopped.%lld", (long long)source_id);
    int resumed = bd_setting_get(cat, stop_key, stop_val, sizeof(stop_val));
    char *own_text = bd_source_skip_get(cat, source_id);
    skip_list own;
    skip_parse(own_text, &own);
    free(own_text);
    scan_ctx c = {0};
    c.cat = cat;
    c.skip = skip;
    c.own_skip = &own;
    c.bitrot = bitrot;
    c.source_id = source_id;
    c.root = root;
    c.scan_ms = scan_ms;
    c.stats = stats;
    c.log = log;
    c.log_ctx = log_ctx;
    c.pool = pool;

    struct { sqlite3_stmt **st; const char *sql; } stmts[] = {
        {&c.find_node, "SELECT n.id, n.size, n.mtime_ns, n.current_version_id, n.deleted, v.hash, t.ctime_ns FROM nodes n"
                       " LEFT JOIN versions v ON v.id=n.current_version_id LEFT JOIN file_change_times t ON t.node_id=n.id"
                       " WHERE n.source_id=? AND n.rel_path=?"},
        {&c.find_parent, "SELECT id FROM nodes WHERE source_id=? AND rel_path=?"},
        {&c.insert_node, "INSERT INTO nodes(source_id,parent_id,rel_path,name,is_dir,last_seen_ms,rel_fold,btime_ns)"
                         " VALUES(?,?,?,?,?,?,?,?)"},
        /* The same name in other letter case, not seen yet in this scan. */
        {&c.find_case, "SELECT n.id, n.size, n.mtime_ns, n.current_version_id, n.deleted, v.hash, t.ctime_ns FROM nodes n"
                       " LEFT JOIN versions v ON v.id=n.current_version_id LEFT JOIN file_change_times t ON t.node_id=n.id"
                       " WHERE n.source_id=? AND n.rel_fold=? AND n.last_seen_ms<? ORDER BY n.deleted, n.id LIMIT 1"},
        {&c.rename_node, "UPDATE nodes SET rel_path=?, name=? WHERE id=?"},
        {&c.touch_node, "UPDATE nodes SET parent_id=?, is_dir=?, last_seen_ms=?, deleted=0, deleted_ack=0,"
                        " btime_ns=CASE WHEN ?4<>0 THEN ?4 ELSE btime_ns END WHERE id=?5"},
        {&c.update_file, "UPDATE nodes SET size=?, mtime_ns=?, current_version_id=? WHERE id=?"},
        {&c.find_version, SQL_FIND_VERSION},
        {&c.next_version_no, SQL_NEXT_VERSION_NO},
        {&c.insert_version, SQL_INSERT_VERSION},
        {&c.note_new, "INSERT OR IGNORE INTO temp.scan_new(node_id) VALUES(?)"},
        /* Found again as it was: keep when it was first found. */
        {&c.note_damaged, "INSERT INTO damaged_files(node_id, version_id, found_hash, found_ms) VALUES(?1,?2,?3,?4)"
                          " ON CONFLICT(node_id) DO UPDATE SET"
                          " found_ms=CASE WHEN found_hash=excluded.found_hash AND version_id=excluded.version_id"
                          " THEN found_ms ELSE excluded.found_ms END,"
                          " version_id=excluded.version_id, found_hash=excluded.found_hash"},
        {&c.clear_damaged, "DELETE FROM damaged_files WHERE node_id=?"},
        {&c.note_online, "INSERT OR REPLACE INTO temp.online_only(node_id, changed) VALUES(?,?)"},
        {&c.note_link, "INSERT OR REPLACE INTO temp.links(source_id, rel_path, target, is_dir) VALUES(?,?,?,?)"},
        {&c.set_ctime, "INSERT INTO file_change_times(node_id, ctime_ns) VALUES(?1,?2)"
                       " ON CONFLICT(node_id) DO UPDATE SET ctime_ns=excluded.ctime_ns"},
    };
    bd_status s = BD_OK;
    size_t n = sizeof(stmts) / sizeof(stmts[0]);
    for (size_t i = 0; i < n; i++)
        if (sqlite3_prepare_v2(cat->db, stmts[i].sql, -1, stmts[i].st, NULL) != SQLITE_OK) { s = bd_fail_db(cat, "prepare scan"); goto done; }

    if (bd_exec(cat, "BEGIN") != 0) { s = bd_fail_db(cat, "begin scan"); goto done; }
    sqlite3_stmt *any;
    if (sqlite3_prepare_v2(cat->db, "SELECT 1 FROM damaged_files LIMIT 1", -1, &any, NULL) != SQLITE_OK) {
        s = bd_fail_db(cat, "prepare scan");
        bd_exec(cat, "ROLLBACK");
        goto done;
    }
    c.have_damaged = sqlite3_step(any) == SQLITE_ROW;
    sqlite3_finalize(any);
    /* What the last scan of this root found is found again. */
    sqlite3_stmt *clear;
    if (sqlite3_prepare_v2(cat->db, "DELETE FROM temp.online_only WHERE node_id IN (SELECT id FROM nodes WHERE source_id=?1)",
                           -1, &clear, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(clear, 1, source_id);
        sqlite3_step(clear);
        sqlite3_finalize(clear);
    }
    if (sqlite3_prepare_v2(cat->db, "DELETE FROM temp.links WHERE source_id=?1", -1, &clear, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(clear, 1, source_id);
        sqlite3_step(clear);
        sqlite3_finalize(clear);
    }
    if (resumed) {
        sqlite3_stmt *r;
        if (sqlite3_prepare_v2(cat->db, "INSERT OR IGNORE INTO temp.scan_new(node_id) SELECT n.id FROM nodes n"
                                        " WHERE n.source_id=?1 AND n.is_dir=0 AND n.deleted=0 AND NOT EXISTS(SELECT 1 FROM versions v"
                                        " WHERE v.node_id=n.id AND v.first_seen_ms<=COALESCE((SELECT last_scan_ms FROM sources"
                                        " WHERE id=?1),0))",
                               -1, &r, NULL) != SQLITE_OK) {
            s = bd_fail_db(cat, "prepare scan");
            bd_exec(cat, "ROLLBACK");
            goto done;
        }
        sqlite3_bind_int64(r, 1, source_id);
        int rc = sqlite3_step(r);
        sqlite3_finalize(r);
        if (rc != SQLITE_DONE) {
            s = bd_fail_db(cat, "prepare scan");
            bd_exec(cat, "ROLLBACK");
            goto done;
        }
    }
    find_own_files(&c);
    int walk_rc = bd_walk(root, scan_entry, scan_error, &c);
    /* The files still with the workers: those read to the end are kept,
     * also when the scan is being stopped. */
    pool_collect(&c, pool->submitted, walk_rc == 0 && !c.db_error);
    if (c.cancelled && walk_rc == 0 && !c.db_error) {
        /* Stopped: keep the files read so far, but a half-scanned folder
         * would look like files deleted, so nothing is marked deleted and
         * the root is not counted as scanned. The next scan goes on from
         * there (its files are not read again). A folder in which none of
         * the files the catalog knows turned up may not be the root at
         * all: that one is left as the last full scan saw it. */
        int64_t seen = stats->files_seen - before.files_seen, known_seen = seen - c.inserted_files;
        sqlite3_stmt *lq;
        int64_t last_ms = 0, had_files = 0;
        if (sqlite3_prepare_v2(cat->db, "SELECT COALESCE(last_scan_ms,0), EXISTS(SELECT 1 FROM nodes n WHERE n.source_id=?1"
                                        " AND n.is_dir=0 AND n.last_seen_ms<?2) FROM sources WHERE id=?1",
                               -1, &lq, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(lq, 1, source_id);
            sqlite3_bind_int64(lq, 2, scan_ms);
            if (sqlite3_step(lq) == SQLITE_ROW) {
                last_ms = sqlite3_column_int64(lq, 0);
                had_files = sqlite3_column_int64(lq, 1);
            }
            sqlite3_finalize(lq);
        }
        if (last_ms > 0 && had_files && known_seen <= 0) {
            bd_exec(cat, "ROLLBACK");
            *stats = before;
        } else if (bd_setting_set(cat, stop_key, "1") != 0 || bd_exec(cat, "COMMIT") != 0) {
            bd_exec(cat, "ROLLBACK");
            *stats = before;
        } else {
            bd_logf(log, log_ctx, "scan of %s stopped: the %lld files it read are kept; nothing was marked deleted", root,
                    (long long)seen);
        }
        s = bd_fail_cancelled(cat);
        goto done;
    }
    if (c.cancelled) {
        bd_exec(cat, "ROLLBACK");
        *stats = before;
        s = bd_fail_cancelled(cat);
        goto done;
    }
    if (walk_rc != 0 && !c.db_error) {
        bd_exec(cat, "ROLLBACK");
        s = bd_fail(cat, BD_ERR_NOMEM, "out of memory while scanning %s", root);
        goto done;
    }
    if (c.db_error) { s = bd_fail_db(cat, "record scan results"); bd_exec(cat, "ROLLBACK"); goto done; }
    /* Is this folder the backup root, by its files? The root is what the
     * catalog records in it; the folder at its path may be another one: an
     * empty folder made under the same name (a fresh Windows makes empty
     * Documents, Pictures and Desktop folders), or other files that happen
     * to have the same path. The files the catalog knows here are kept
     * (where it has them and as it has them, or moved inside the root:
     * their contents are in a file new here), changed (at their path, with
     * other contents) or lost (not here at all). Files new here say nothing
     * either way. When a tenth or less are kept and more are lost than
     * changed, marking the lost ones deleted would leave a restore with
     * nothing to restore: the root is left as the last scan saw it and
     * listed like one that cannot be found, and the user says where it is,
     * restores it, or says the files really were deleted
     * (bd_source_accept_gone). A folder whose files were all edited is not
     * held: they are changed, not lost. */
    int64_t gone = 0, moved = 0;
    s = mark_deleted(&c, 1, &gone, &moved);
    int64_t seen = stats->files_seen - before.files_seen;
    int64_t changed = stats->files_changed - before.files_changed + stats->files_cloud_changed - before.files_cloud_changed;
    /* After a stopped scan, the files it found new count as new here too. */
    int64_t inserted = c.inserted_files;
    if (resumed && s == BD_OK) {
        sqlite3_stmt *iq;
        if (sqlite3_prepare_v2(cat->db, "SELECT COUNT(*) FROM temp.scan_new w JOIN nodes n ON n.id=w.node_id"
                                        " WHERE n.source_id=? AND n.is_dir=0 AND n.last_seen_ms=?",
                               -1, &iq, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(iq, 1, source_id);
            sqlite3_bind_int64(iq, 2, scan_ms);
            if (sqlite3_step(iq) == SQLITE_ROW) inserted = sqlite3_column_int64(iq, 0);
            sqlite3_finalize(iq);
        }
    }
    int64_t known = gone + seen - inserted;
    int64_t lost = gone - moved;
    int64_t kept = known - lost - changed;
    if (s == BD_OK && lost > 0 && kept * 10 <= known && lost > changed && !take_accept_gone(cat, source_id)) {
        bd_exec(cat, "ROLLBACK");
        *stats = before;
        stats->roots_missing++;
        stats->roots_emptied++;
        note_missing(cat, source_id, 1, lost);
        bd_logf(log, log_ctx, "backup root %s is there, but %lld of the %lld files the catalog knows in it are not: left as it was "
                              "(nothing marked deleted). Another folder with the same name?",
                root, (long long)lost, (long long)known);
        goto done;
    }
    if (s == BD_OK) s = mark_deleted(&c, 0, NULL, NULL);
    if (s == BD_OK) {
        sqlite3_stmt *u;
        if (sqlite3_prepare_v2(cat->db, "UPDATE sources SET last_scan_ms=? WHERE id=?", -1, &u, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(u, 1, scan_ms);
            sqlite3_bind_int64(u, 2, source_id);
            sqlite3_step(u);
            sqlite3_finalize(u);
        }
        if (resumed && bd_setting_del(cat, stop_key) != 0) s = bd_fail_db(cat, "record scan results");
    }
    if (s == BD_OK && bd_exec(cat, "COMMIT") != 0) s = bd_fail_db(cat, "commit scan");
    if (s != BD_OK) bd_exec(cat, "ROLLBACK");

done:
    for (size_t i = 0; i < n; i++) sqlite3_finalize(*stmts[i].st);
    for (size_t i = 0; i < c.failed_len; i++) free(c.failed_dirs[i]);
    free(c.failed_dirs);
    for (int i = 0; i < c.n_own_keys; i++) free(c.own_keys[i]);
    skip_free(&own);
    return s;
}

/* ---- Moved and renamed files ------------------------------------------------
 *
 * After every backup root is scanned: a file new to the catalog whose
 * contents (hash and size) match exactly one file that went missing, and no
 * other new file, is the same file moved or renamed. The new entry takes the
 * old one's versions and with them its copies, wherever they are on the
 * drives, so nothing is copied again and nothing on a drive is moved: the
 * catalog records where each copy is. */
static bd_status merge_moves(bd_catalog *cat, bd_scan_stats *stats, bd_log_fn log, void *log_ctx)
{
    if (bd_exec(cat, "DELETE FROM temp.mv_gone; DELETE FROM temp.mv_new; DELETE FROM temp.mv_dirs;"
                     "INSERT INTO temp.mv_gone(node_id,hash,size) SELECT n.id, v.hash, v.size FROM temp.scan_gone g"
                     " JOIN nodes n ON n.id=g.node_id JOIN versions v ON v.id=n.current_version_id"
                     " WHERE n.is_dir=0 AND n.deleted=1 AND v.size>0;"
                     "INSERT INTO temp.mv_new(node_id,hash,size) SELECT n.id, v.hash, v.size FROM temp.scan_new w"
                     " JOIN nodes n ON n.id=w.node_id JOIN versions v ON v.id=n.current_version_id"
                     " WHERE n.is_dir=0 AND n.deleted=0 AND v.size>0;") != 0)
        return bd_fail_db(cat, "find moved files");
    sqlite3_stmt *q = NULL, *get = NULL, *dv = NULL, *mv = NULL, *setb = NULL, *dirs = NULL, *drop = NULL;
    bd_status s = BD_OK;
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT w.node_id, g.node_id FROM temp.mv_new w JOIN temp.mv_gone g ON g.hash=w.hash AND g.size=w.size"
                           " WHERE (SELECT COUNT(*) FROM temp.mv_gone x WHERE x.hash=w.hash AND x.size=w.size)=1"
                           " AND (SELECT COUNT(*) FROM temp.mv_new y WHERE y.hash=w.hash AND y.size=w.size)=1",
                           -1, &q, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(cat->db,
                           "SELECT n.current_version_id, s.name||'/'||n.rel_path, n.parent_id, n.source_id, n.rel_path FROM nodes n"
                           " JOIN sources s ON s.id=n.source_id WHERE n.id=?", -1, &get, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(cat->db, "DELETE FROM versions WHERE node_id=?", -1, &dv, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(cat->db, "UPDATE versions SET node_id=?1 WHERE node_id=?2", -1, &mv, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(cat->db, "UPDATE nodes SET current_version_id=?, moved_from=? WHERE id=?", -1, &setb, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(cat->db, "INSERT OR IGNORE INTO temp.mv_dirs(node_id) VALUES(?)", -1, &dirs, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(cat->db, "DELETE FROM nodes WHERE id=?", -1, &drop, NULL) != SQLITE_OK) {
        s = bd_fail_db(cat, "find moved files");
        goto done;
    }
    typedef struct { int64_t to, from; } pair;
    pair *pairs = NULL;
    size_t np = 0, cap = 0;
    while (sqlite3_step(q) == SQLITE_ROW) {
        if (np == cap) {
            size_t nc = cap ? cap * 2 : 64;
            pair *pp = realloc(pairs, nc * sizeof(pair));
            if (!pp) break;
            pairs = pp;
            cap = nc;
        }
        pairs[np].to = sqlite3_column_int64(q, 0);
        pairs[np].from = sqlite3_column_int64(q, 1);
        np++;
    }
    sqlite3_reset(q);
    for (size_t i = 0; i < np && s == BD_OK; i++) {
        sqlite3_reset(get);
        sqlite3_bind_int64(get, 1, pairs[i].from);
        if (sqlite3_step(get) != SQLITE_ROW) continue;
        int64_t cur = sqlite3_column_int64(get, 0);
        char *from_path = bd_strdup((const char *)sqlite3_column_text(get, 1));
        int64_t parent = sqlite3_column_type(get, 2) == SQLITE_NULL ? 0 : sqlite3_column_int64(get, 2);
        int64_t from_source = sqlite3_column_int64(get, 3);
        char *from_rel = bd_strdup((const char *)sqlite3_column_text(get, 4));
        sqlite3_reset(get);
        /* A star or an encrypt mark on the file itself moves with it. */
        for (int t = 0; t < 2 && from_rel; t++) {
            sqlite3_stmt *mk;
            const char *sql = t == 0 ? "UPDATE OR IGNORE stars SET source_id=(SELECT source_id FROM nodes WHERE id=?1),"
                                       " rel_path=(SELECT rel_path FROM nodes WHERE id=?1) WHERE source_id=?2 AND rel_path=?3"
                                     : "UPDATE OR IGNORE encrypt_paths SET source_id=(SELECT source_id FROM nodes WHERE id=?1),"
                                       " rel_path=(SELECT rel_path FROM nodes WHERE id=?1) WHERE source_id=?2 AND rel_path=?3";
            if (sqlite3_prepare_v2(cat->db, sql, -1, &mk, NULL) != SQLITE_OK) continue;
            sqlite3_bind_int64(mk, 1, pairs[i].to);
            sqlite3_bind_int64(mk, 2, from_source);
            sqlite3_bind_text(mk, 3, from_rel, -1, SQLITE_STATIC);
            sqlite3_step(mk);
            sqlite3_finalize(mk);
        }
        free(from_rel);
        /* The new entry's one version (just read, never copied) gives way
         * to the old entry's history. */
        sqlite3_reset(dv);
        sqlite3_bind_int64(dv, 1, pairs[i].to);
        int ok = sqlite3_step(dv) == SQLITE_DONE;
        sqlite3_reset(mv);
        sqlite3_bind_int64(mv, 1, pairs[i].to);
        sqlite3_bind_int64(mv, 2, pairs[i].from);
        ok = ok && sqlite3_step(mv) == SQLITE_DONE;
        sqlite3_reset(setb);
        sqlite3_bind_int64(setb, 1, cur);
        if (from_path) sqlite3_bind_text(setb, 2, from_path, -1, SQLITE_TRANSIENT);
        else sqlite3_bind_null(setb, 2);
        sqlite3_bind_int64(setb, 3, pairs[i].to);
        ok = ok && sqlite3_step(setb) == SQLITE_DONE;
        sqlite3_reset(drop);
        sqlite3_bind_int64(drop, 1, pairs[i].from);
        ok = ok && sqlite3_step(drop) == SQLITE_DONE;
        if (ok && parent) {
            sqlite3_reset(dirs);
            sqlite3_bind_int64(dirs, 1, parent);
            sqlite3_step(dirs);
        }
        if (!ok) s = bd_fail_db(cat, "record a moved file");
        else {
            stats->files_moved++;
            stats->files_new--;
            stats->files_deleted--;
            bd_logf(log, log_ctx, "moved or renamed, copies kept: %s", from_path ? from_path : "?");
        }
        free(from_path);
    }
    free(pairs);
    /* Folders left empty by moves were moved too: drop them, and then any
     * folder above them left empty the same way. */
    for (int round = 0; s == BD_OK && round < 256; round++) {
        sqlite3_stmt *e;
        if (sqlite3_prepare_v2(cat->db,
                               "SELECT d.node_id, n.parent_id FROM temp.mv_dirs d JOIN nodes n ON n.id=d.node_id"
                               " WHERE n.is_dir=1 AND n.deleted=1"
                               " AND NOT EXISTS(SELECT 1 FROM nodes c WHERE c.source_id=n.source_id AND c.parent_id=n.id)",
                               -1, &e, NULL) != SQLITE_OK) { s = bd_fail_db(cat, "tidy moved folders"); break; }
        int64_t ids[256], parents[256];
        int k = 0;
        while (k < 256 && sqlite3_step(e) == SQLITE_ROW) {
            ids[k] = sqlite3_column_int64(e, 0);
            parents[k] = sqlite3_column_type(e, 1) == SQLITE_NULL ? 0 : sqlite3_column_int64(e, 1);
            k++;
        }
        sqlite3_finalize(e);
        if (!k) break;
        for (int i = 0; i < k; i++) {
            sqlite3_reset(drop);
            sqlite3_bind_int64(drop, 1, ids[i]);
            sqlite3_step(drop);
            if (parents[i]) {
                sqlite3_reset(dirs);
                sqlite3_bind_int64(dirs, 1, parents[i]);
                sqlite3_step(dirs);
            }
        }
        sqlite3_stmt *x;
        if (sqlite3_prepare_v2(cat->db, "DELETE FROM temp.mv_dirs WHERE node_id NOT IN (SELECT id FROM nodes)", -1, &x, NULL) == SQLITE_OK) {
            sqlite3_step(x);
            sqlite3_finalize(x);
        }
    }
done:
    sqlite3_finalize(q);
    sqlite3_finalize(get);
    sqlite3_finalize(dv);
    sqlite3_finalize(mv);
    sqlite3_finalize(setb);
    sqlite3_finalize(dirs);
    sqlite3_finalize(drop);
    return s;
}

/* ---- Files in use --------------------------------------------------------- */

void bd_note_in_use(bd_catalog *cat, const char *path, int64_t size)
{
    sqlite3_stmt *u;
    if (!path || sqlite3_prepare_v2(cat->db, "INSERT OR REPLACE INTO temp.in_use(path, size) VALUES(?,?)", -1, &u, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_text(u, 1, path, -1, SQLITE_STATIC);
    sqlite3_bind_int64(u, 2, size);
    sqlite3_step(u);
    sqlite3_finalize(u);
}

bd_status bd_list_in_use(bd_catalog *cat, bd_in_use_fn fn, void *ctx)
{
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(cat->db, "SELECT path, size FROM temp.in_use ORDER BY path", -1, &q, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "list files in use");
    while (sqlite3_step(q) == SQLITE_ROW)
        if (fn(ctx, (const char *)sqlite3_column_text(q, 0), sqlite3_column_int64(q, 1)) != 0) break;
    sqlite3_finalize(q);
    return BD_OK;
}

bd_status bd_substitute_add(bd_catalog *cat, const char *path, const char *staged)
{
    sqlite3_stmt *u;
    if (!path || !staged) return bd_fail(cat, BD_ERR_INVALID, "no file given");
    if (sqlite3_prepare_v2(cat->db, "INSERT OR REPLACE INTO temp.substitute(path, staged) VALUES(?,?)", -1, &u, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "use a shadow copy");
    sqlite3_bind_text(u, 1, path, -1, SQLITE_STATIC);
    sqlite3_bind_text(u, 2, staged, -1, SQLITE_STATIC);
    int rc = sqlite3_step(u);
    sqlite3_finalize(u);
    return rc == SQLITE_DONE ? BD_OK : bd_fail_db(cat, "use a shadow copy");
}

void bd_substitutes_clear(bd_catalog *cat)
{
    bd_exec(cat, "DELETE FROM temp.substitute");
}

char *bd_substitute_for(bd_catalog *cat, const char *path)
{
    sqlite3_stmt *q;
    char *r = NULL;
    if (!path || sqlite3_prepare_v2(cat->db, "SELECT staged FROM temp.substitute WHERE path=?", -1, &q, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_text(q, 1, path, -1, SQLITE_STATIC);
    if (sqlite3_step(q) == SQLITE_ROW) r = bd_strdup((const char *)sqlite3_column_text(q, 0));
    sqlite3_finalize(q);
    return r;
}

bd_status bd_scan(bd_catalog *cat, bd_scan_stats *stats, bd_log_fn log, void *log_ctx)
{
    return bd_scan_ex(cat, NULL, stats, log, log_ctx);
}

/* The disk each backup root is on, read once a session: a drive on the
 * same disk does not count as a copy (bd_refresh_uncounted). */
static void note_root_disks(bd_catalog *cat, const void *roots, size_t n)
{
    typedef struct { int64_t id; char *path; } src;
    const src *list = roots;
    sqlite3_stmt *have = NULL, *put = NULL;
    if (sqlite3_prepare_v2(cat->db, "SELECT 1 FROM temp.root_disks WHERE source_id=?", -1, &have, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(cat->db, "INSERT OR REPLACE INTO temp.root_disks(source_id, model, serial, disk_bytes) VALUES(?,?,?,?)",
                           -1, &put, NULL) != SQLITE_OK) {
        sqlite3_finalize(have);
        return;
    }
    for (size_t i = 0; i < n; i++) {
        if (!list[i].path) continue;
        sqlite3_reset(have);
        sqlite3_bind_int64(have, 1, list[i].id);
        if (sqlite3_step(have) == SQLITE_ROW) continue;
        bd_drive_hw hw;
        int rc = bd_test_hw_read ? bd_test_hw_read(list[i].path, &hw) : bd_drive_hw_read(list[i].path, &hw);
        sqlite3_reset(put);
        sqlite3_bind_int64(put, 1, list[i].id);
        if (rc == 0 && hw.serial[0]) {
            sqlite3_bind_text(put, 2, hw.model, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(put, 3, hw.serial, -1, SQLITE_TRANSIENT);
            if (hw.disk_bytes >= 0) sqlite3_bind_int64(put, 4, hw.disk_bytes);
            else sqlite3_bind_null(put, 4);
        } else {
            sqlite3_bind_null(put, 2);
            sqlite3_bind_null(put, 3);
            sqlite3_bind_null(put, 4);
        }
        sqlite3_step(put);
    }
    sqlite3_finalize(have);
    sqlite3_finalize(put);
    bd_refresh_uncounted(cat);
}

bd_status bd_scan_ex(bd_catalog *cat, const bd_scan_opts *opts, bd_scan_stats *stats, bd_log_fn log, void *log_ctx)
{
    bd_scan_stats local;
    if (!stats) stats = &local;
    memset(stats, 0, sizeof(*stats));
    bd_cancel_reset(cat);
    int bitrot = opts && opts->bitrot;

    sqlite3_stmt *q;
    /* Roots no longer backed up are left as they are: not scanned, so
     * nothing in them is marked deleted. */
    if (sqlite3_prepare_v2(cat->db, "SELECT id, path, COALESCE(last_scan_ms,0), active FROM sources ORDER BY id", -1, &q, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "list backup roots");
    typedef struct { int64_t id; char *path; } src;
    src *list = NULL;
    size_t len = 0;
    int64_t scan_ms = bd_now_ms();
    while (sqlite3_step(q) == SQLITE_ROW) {
        /* last_seen_ms marks what this scan saw, so it must move forward. */
        int64_t prev = sqlite3_column_int64(q, 2);
        if (scan_ms <= prev) scan_ms = prev + 1;
        if (!sqlite3_column_int(q, 3)) {
            stats->roots_stopped++;
            continue;
        }
        src *nl = realloc(list, (len + 1) * sizeof(src));
        if (!nl) break;
        list = nl;
        list[len].id = sqlite3_column_int64(q, 0);
        list[len].path = bd_strdup((const char *)sqlite3_column_text(q, 1));
        len++;
    }
    sqlite3_finalize(q);

    /* The last scan's count is the best guess of how many files there are. */
    int64_t expect = 0, expect_bytes = 0;
    if (sqlite3_prepare_v2(cat->db, "SELECT COUNT(*), COALESCE(SUM(size),0) FROM nodes n WHERE is_dir=0 AND deleted=0 AND " BD_SQL_ACTIVE("n"),
                           -1, &q, NULL) == SQLITE_OK) {
        if (sqlite3_step(q) == SQLITE_ROW) {
            expect = sqlite3_column_int64(q, 0);
            expect_bytes = sqlite3_column_int64(q, 1);
        }
        sqlite3_finalize(q);
    }
    bd_progress_begin(cat, bitrot ? "bit-rot scan" : "scan", expect, bitrot ? expect_bytes : 0);
    bd_exec(cat, "DELETE FROM temp.in_use; DELETE FROM temp.scan_new; DELETE FROM temp.scan_gone;");
    /* A root's folder is where it was last; a root that is not there is
     * looked for by its files, and pointed at the one place holding them.
     * So is a root kept as it was typed from the current folder (by 0.13.0
     * and older), which then gets its full path. */
    for (size_t i = 0; i < len; i++) {
        bd_stat_t rst;
        int full = list[i].path && bd_path_is_full(list[i].path);
        if (!list[i].path || (full && bd_stat(list[i].path, &rst) == 0 && rst.is_dir)) continue;
        /* A root that waits for its disk is on that disk: a copy of it
         * somewhere else is not it. */
        char wkey[48], wval[8];
        snprintf(wkey, sizeof(wkey), "wait_disk.%lld", (long long)list[i].id);
        if (full && bd_setting_get(cat, wkey, wval, sizeof(wval))) continue;
        int64_t found = 0, checked = 0, sure_found = 0, sure_checked = 0;
        int places = 0;
        char *p = bd_source_find(cat, list[i].id, &found, &checked, &places);
        /* Pointed there without asking only when nearly every file is
         * there and a few hold their contents; otherwise the user is asked
         * (the root stays missing, and the window offers the folders). */
        int sure = p && places == 1 && bd_source_place_sure(cat, list[i].id, p, &sure_found, &sure_checked);
        if (sure && bd_source_relocate(cat, list[i].id, p) == BD_OK) {
            bd_logf(log, log_ctx, "backup root %s is now at %s: found it there by its files (%lld of %lld checked, and the "
                                  "contents of the smallest ones)",
                    list[i].path, p, (long long)sure_found, (long long)sure_checked);
            stats->roots_found++;
            /* Where it was, to say so if that folder comes back. */
            if (full) {
                char key[48];
                snprintf(key, sizeof(key), "moved_from.%lld", (long long)list[i].id);
                bd_setting_set(cat, key, list[i].path);
            }
            free(list[i].path);
            list[i].path = p;
            continue;
        }
        if (p && places > 1)
            bd_logf(log, log_ctx, "backup root %s is not where it was, and %lld folders hold its files, so none was picked: "
                                  "say where it is now. One is %s (%lld of %lld checked)",
                    list[i].path, (long long)places, p, (long long)found, (long long)checked);
        else if (p)
            bd_logf(log, log_ctx, "backup root %s is not where it was. %s holds %lld of the %lld of its files checked, which is "
                                  "not enough to point the root there without asking: say where it is now",
                    list[i].path, p, (long long)found, (long long)checked);
        free(p);
    }
    /* A root pointed somewhere else by its files whose old folder is back,
     * holding them: say so, as only one of the two is backed up. */
    for (size_t i = 0; i < len; i++) {
        int back = 0;
        char *old = bd_source_old_place(cat, list[i].id, &back);
        if (old && back) {
            stats->roots_back++;
            bd_logf(log, log_ctx, "the folder backup root %s was in before, %s, is there again and holds its files: only %s "
                                  "is backed up. If %s is the real one, point the root back there",
                    list[i].path, old, list[i].path, old);
        }
        free(old);
    }
    note_root_disks(cat, list, len);
    int64_t job = bd_job_start(cat, bitrot ? "bit-rot scan" : "scan", 0);

    bd_status s = BD_OK;
    char *patterns = bd_skip_list_get(cat);
    skip_list skip;
    skip_parse(patterns, &skip);
    free(patterns);
    /* Hashing threads: one fewer than the processors, at most four, at
     * least one. They start only when a file needs hashing. */
    hash_pool pool;
    memset(&pool, 0, sizeof(pool));
    pool.workers = bd_cpu_count() - 1;
    if (pool.workers > 4) pool.workers = 4;
    if (pool.workers < 1) pool.workers = 1;
    if (bd_test_scan_workers) pool.workers = bd_test_scan_workers < 0 ? 0 : bd_test_scan_workers;
    if (pool.workers > HASH_MAX_WORKERS) pool.workers = HASH_MAX_WORKERS;
    pool.low = bd_option_get(cat, "low_priority");
    for (size_t i = 0; i < len && s == BD_OK; i++) {
        bd_logf(log, log_ctx, "scanning %s", list[i].path);
        bd_scan_stats before = *stats;
        s = scan_source(cat, &skip, bitrot, &pool, list[i].id, list[i].path, scan_ms, stats, log, log_ctx);
        if (s == BD_OK && stats->roots_emptied > before.roots_emptied) {
            /* The folder there holds other files: say where its own are,
             * when another folder holds them. The user decides. */
            int64_t found = 0, checked = 0;
            char *p = bd_source_find(cat, list[i].id, &found, &checked, NULL);
            if (p)
                bd_logf(log, log_ctx, "the files of backup root %s are in %s (%lld of %lld checked): if that is where it "
                                      "is now, point the root there",
                        list[i].path, p, (long long)found, (long long)checked);
            free(p);
        }
    }
    pool_stop(&pool);
    skip_free(&skip);
    for (size_t i = 0; i < len; i++) free(list[i].path);
    free(list);
    /* Moves are matched across every root this scan finished, cancelled or
     * not: what it finished is recorded either way. */
    if (s == BD_OK || s == BD_ERR_CANCELLED) {
        if (bd_exec(cat, "BEGIN") == 0) {
            bd_status m = merge_moves(cat, stats, log, log_ctx);
            if (m == BD_OK && bd_exec(cat, "COMMIT") == 0) {
                /* Files gone from their folder that no drive holds: the user
                 * should hear about these first. */
                if (sqlite3_prepare_v2(cat->db,
                                       "SELECT COUNT(*) FROM temp.scan_gone g JOIN nodes n ON n.id=g.node_id WHERE n.is_dir=0"
                                       " AND NOT EXISTS(SELECT 1 FROM versions v JOIN copies c ON c.version_id=v.id"
                                       " WHERE v.node_id=n.id AND c.state='ok')", -1, &q, NULL) == SQLITE_OK) {
                    if (sqlite3_step(q) == SQLITE_ROW) stats->deleted_no_copy = sqlite3_column_int64(q, 0);
                    sqlite3_finalize(q);
                }
            } else {
                bd_exec(cat, "ROLLBACK");
                if (s == BD_OK) s = m != BD_OK ? m : bd_fail_db(cat, "record moved files");
            }
        } else if (s == BD_OK) {
            s = bd_fail_db(cat, "record moved files");
        }
    }
    if (stats->roots_missing)
        bd_logf(log, log_ctx, "%lld backup root(s) could not be found; nothing in them was marked deleted",
                (long long)stats->roots_missing);
    bd_report(cat, stats->files_seen, NULL, 1);
    bd_progress_end(cat);
    bd_job_finish(cat, job, s == BD_ERR_CANCELLED ? "stopped" : s != BD_OK ? "failed" : stats->errors ? "partial" : "done",
                  stats->files_seen, stats->errors, stats->bytes_hashed);
    return s;
}

/* ---- What the last scan found ---------------------------------------------- */

bd_status bd_list_damaged(bd_catalog *cat, bd_damaged_fn fn, void *ctx)
{
    /* An entry stands while the version it was found against is still the
     * file's and the file is still in its folder (another program, or an
     * earlier BRODALF, may have recorded a new version since). */
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT n.id, n.source_id, s.name, n.rel_path, n.size, v.hash, d.found_hash, d.found_ms, n.current_version_id"
                           " FROM damaged_files d"
                           " JOIN nodes n ON n.id=d.node_id AND n.current_version_id=d.version_id AND n.deleted=0"
                           " JOIN sources s ON s.id=n.source_id"
                           " LEFT JOIN versions v ON v.id=n.current_version_id ORDER BY s.name, n.rel_path",
                           -1, &q, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "list damaged files");
    int rc;
    while ((rc = sqlite3_step(q)) == SQLITE_ROW) {
        bd_damaged_info d;
        memset(&d, 0, sizeof(d));
        d.node_id = sqlite3_column_int64(q, 0);
        d.source_id = sqlite3_column_int64(q, 1);
        d.source_name = (const char *)sqlite3_column_text(q, 2);
        d.rel_path = (const char *)sqlite3_column_text(q, 3);
        d.size = sqlite3_column_int64(q, 4);
        d.hash_on_record = sqlite3_column_text(q, 5) ? (const char *)sqlite3_column_text(q, 5) : "";
        d.hash_found = (const char *)sqlite3_column_text(q, 6);
        d.found_ms = sqlite3_column_int64(q, 7);
        d.version_id = sqlite3_column_int64(q, 8);
        if (fn(ctx, &d) != 0) { rc = SQLITE_DONE; break; }
    }
    sqlite3_finalize(q);
    if (rc != SQLITE_DONE) return bd_fail_db(cat, "list damaged files");
    return BD_OK;
}

bd_status bd_damaged_accept(bd_catalog *cat, int64_t node_id)
{
    /* The user says the file as it is now is right: record it as a new
     * version, to be backed up like any change. */
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(cat->db, "SELECT d.found_hash, n.size, n.mtime_ns FROM damaged_files d"
                                    " JOIN nodes n ON n.id=d.node_id AND n.current_version_id=d.version_id AND n.deleted=0"
                                    " WHERE d.node_id=?", -1, &q, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "accept a changed file");
    sqlite3_bind_int64(q, 1, node_id);
    if (sqlite3_step(q) != SQLITE_ROW) {
        sqlite3_finalize(q);
        return bd_fail(cat, BD_ERR_NOT_FOUND, "that file is not listed as damaged");
    }
    char hash[BD_HASH_HEX_LEN + 1];
    snprintf(hash, sizeof(hash), "%s", (const char *)sqlite3_column_text(q, 0));
    int64_t size = sqlite3_column_int64(q, 1), mtime = sqlite3_column_int64(q, 2);
    sqlite3_finalize(q);
    if (bd_exec(cat, "BEGIN") != 0) return bd_fail_db(cat, "accept a changed file");
    sqlite3_stmt *d = NULL;
    int ok = bd_record_version(cat, node_id, hash, size, mtime, NULL, NULL) == 0 &&
             sqlite3_prepare_v2(cat->db, "DELETE FROM damaged_files WHERE node_id=?", -1, &d, NULL) == SQLITE_OK &&
             sqlite3_bind_int64(d, 1, node_id) == SQLITE_OK && sqlite3_step(d) == SQLITE_DONE;
    sqlite3_finalize(d);
    if (!ok || bd_exec(cat, "COMMIT") != 0) {
        bd_status st = bd_fail_db(cat, "accept a changed file");
        bd_exec(cat, "ROLLBACK");
        return st;
    }
    return BD_OK;
}
