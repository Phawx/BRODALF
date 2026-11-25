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
    if (catalog_copy_name(name)) return 1;
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

/* Two backup roots never overlap: a folder inside a root, or holding one,
 * would have its files scanned and backed up twice, under two names. Roots
 * that are stopped (active 0) are not scanned and do not count. *same gets
 * the id of a root that is this very folder, spelled another way (any
 * root, stopped or not), or 0. BD_OK, or BD_ERR_EXISTS with a message
 * naming the root in the way. */
static bd_status check_overlap(bd_catalog *cat, const char *path, int64_t except_id, int64_t *same)
{
    *same = 0;
    char *key = root_key(path);
    if (!key) return BD_ERR_NOMEM;
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(cat->db, "SELECT id, path, active FROM sources WHERE id<>? ORDER BY id", -1, &q, NULL) != SQLITE_OK) {
        free(key);
        return bd_fail_db(cat, "source lookup");
    }
    sqlite3_bind_int64(q, 1, except_id);
    bd_status s = BD_OK;
    while (s == BD_OK && sqlite3_step(q) == SQLITE_ROW) {
        const char *other = (const char *)sqlite3_column_text(q, 1);
        char *okey = other ? root_key(other) : NULL;
        if (!okey) { s = BD_ERR_NOMEM; break; }
        if (strcmp(key, okey) == 0) {
            if (!*same) *same = sqlite3_column_int64(q, 0);
        } else if (sqlite3_column_int(q, 2) && key_inside(key, okey)) {
            s = bd_fail(cat, BD_ERR_EXISTS, "%s is inside %s, which is already a backup root: its files are backed up with it",
                        path, other);
        } else if (sqlite3_column_int(q, 2) && key_inside(okey, key)) {
            s = bd_fail(cat, BD_ERR_EXISTS,
                        "%s holds %s, which is already a backup root: a folder can be in only one backup root."
                        " Stop backing up %s first, or choose another folder",
                        path, other, other);
        }
        free(okey);
    }
    sqlite3_finalize(q);
    free(key);
    return s;
}

bd_status bd_source_add(bd_catalog *cat, const char *folder, int64_t *out_id)
{
    bd_stat_t st;
    if (bd_stat(folder, &st) != 0 || !st.is_dir) return bd_fail(cat, BD_ERR_NOT_FOUND, "%s is not a folder", folder);

    char *path = bd_strdup(folder);
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
    bd_status overlap = check_overlap(cat, path, 0, &same);
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
        if (sqlite3_step(ins) != SQLITE_DONE) s = bd_fail_db(cat, "add source");
        else if (out_id) *out_id = sqlite3_last_insert_rowid(cat->db);
        sqlite3_finalize(ins);
    }
    free(name);
    free(path);
    return s;
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
    ".gradle/\n";

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
            bd_status s = check_overlap(cat, path, source_id, &same);
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
        size_t n = (size_t)(b - a);
        r.pat = malloc(n + 1);
        if (!r.pat) continue;
        memcpy(r.pat, a, n);
        r.pat[n] = '\0';
        for (char *c = r.pat; *c; c++) if (*c == '\\') *c = '/';
        if (n > 1 && r.pat[n - 1] == '/') { r.dir_only = 1; r.pat[--n] = '\0'; }
        while (r.pat[0] == '/') memmove(r.pat, r.pat + 1, strlen(r.pat));
        r.whole_path = strchr(r.pat, '/') != NULL;
        if (!r.pat[0]) { free(r.pat); continue; }
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

/* * and ? wildcards, ignoring ASCII case. */
static int glob_match(const char *p, const char *s)
{
    const char *star = NULL, *mark = NULL;
    while (*s) {
        if (*p == '*') { star = p++; mark = s; continue; }
        if (*p && (*p == '?' || lower_c((unsigned char)*p) == lower_c((unsigned char)*s))) { p++; s++; continue; }
        if (star) { p = star + 1; s = ++mark; continue; }
        return 0;
    }
    while (*p == '*') p++;
    return *p == '\0';
}

static int skip_one(const skip_list *l, const char *rel, int is_dir)
{
    const char *name = strrchr(rel, '/');
    name = name ? name + 1 : rel;
    for (int i = 0; i < l->n; i++) {
        const skip_rule *r = &l->rules[i];
        if (r->dir_only && !is_dir) continue;
        if (glob_match(r->pat, r->whole_path ? rel : name)) return 1;
    }
    return 0;
}

/* The item itself, or any folder it is in. */
static int skip_any(const skip_list *l, const char *rel, int is_dir)
{
    if (!l->n) return 0;
    if (skip_one(l, rel, is_dir)) return 1;
    char *dir = bd_strdup(rel);
    int hit = 0;
    for (char *slash = dir ? strrchr(dir, '/') : NULL; slash && !hit; slash = strrchr(dir, '/')) {
        *slash = '\0';
        hit = skip_one(l, dir, 1);
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
    int have_damaged; /* damaged_files has rows: a file read again may clear one */
    char **failed_dirs; /* relative folders that could not be read */
    size_t failed_len, failed_cap;
    int db_error;
    int cancelled;
    hash_pool *pool;
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

static int scan_entry(void *ctx, const char *rel, const bd_stat_t *st)
{
    scan_ctx *c = ctx;
    if (bd_cancelled(c->cat)) { c->cancelled = 1; return 1; }
    if (st->is_link) {
        c->stats->skipped_links++;
        bd_logf(c->log, c->log_ctx, "skipped link: %s", rel);
        return 0;
    }
    if (!st->is_dir && !st->is_file) return 0;
    /* Parent folders on the skip lists were never entered. */
    if (skip_one(c->skip, rel, st->is_dir) || skip_one(c->own_skip, rel, st->is_dir)) {
        c->stats->skipped++;
        return st->is_dir ? BD_WALK_SKIP : 0;
    }
    if (st->is_dir && is_own_store(c, rel)) return BD_WALK_SKIP;

    sqlite3_reset(c->find_node);
    sqlite3_bind_int64(c->find_node, 1, c->source_id);
    sqlite3_bind_text(c->find_node, 2, rel, -1, SQLITE_TRANSIENT);
    int64_t node_id = 0, old_size = -1, old_mtime = -1, old_version = 0;
    int was_deleted = 0;
    char old_hash[BD_HASH_HEX_LEN + 1] = "";
    if (sqlite3_step(c->find_node) == SQLITE_ROW) {
        node_id = sqlite3_column_int64(c->find_node, 0);
        old_size = sqlite3_column_int64(c->find_node, 1);
        old_mtime = sqlite3_column_int64(c->find_node, 2);
        old_version = sqlite3_column_int64(c->find_node, 3);
        was_deleted = sqlite3_column_int(c->find_node, 4);
        const char *h = (const char *)sqlite3_column_text(c->find_node, 5);
        if (h) snprintf(old_hash, sizeof(old_hash), "%s", h);
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
    if (!is_new && old_version && old_size == st->size && old_mtime == st->mtime_ns) {
        if (!c->bitrot || !old_hash[0] || st->online_only) {
            if (was_deleted) c->stats->files_new++; /* back where it once was */
            return 0;
        }
        bitrot = 1;
    }
    /* A cloud file that is not on this PC would be downloaded by reading
     * it. Leave it alone: the catalog keeps what it knew, and nothing is
     * marked deleted. */
    if (st->online_only || (bd_test_online_only && bd_rel_ends_with(rel, bd_test_online_only))) {
        if (!c->stats->files_online_only++)
            bd_logf(c->log, c->log_ctx, "files that are online-only (in the cloud, not on this PC) are skipped, not downloaded");
        bd_logf(c->log, c->log_ctx, "online-only, skipped: %s", rel);
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
 * the user forgets them (bd_deleted_forget). */
static bd_status mark_deleted(scan_ctx *c)
{
    sqlite3_stmt *q, *u, *gone;
    if (sqlite3_prepare_v2(c->cat->db, "SELECT id, rel_path, is_dir FROM nodes WHERE source_id=? AND deleted=0 AND last_seen_ms<?", -1, &q, NULL) != SQLITE_OK)
        return bd_fail_db(c->cat, "find deleted files");
    if (sqlite3_prepare_v2(c->cat->db, "UPDATE nodes SET deleted=1, deleted_ack=0 WHERE id=?", -1, &u, NULL) != SQLITE_OK) {
        sqlite3_finalize(q);
        return bd_fail_db(c->cat, "mark deleted files");
    }
    if (sqlite3_prepare_v2(c->cat->db, "INSERT OR IGNORE INTO temp.scan_gone(node_id) VALUES(?)", -1, &gone, NULL) != SQLITE_OK) {
        sqlite3_finalize(q);
        sqlite3_finalize(u);
        return bd_fail_db(c->cat, "mark deleted files");
    }
    sqlite3_bind_int64(q, 1, c->source_id);
    sqlite3_bind_int64(q, 2, c->scan_ms);
    while (sqlite3_step(q) == SQLITE_ROW) {
        const char *rel = (const char *)sqlite3_column_text(q, 1);
        if (under_failed_dir(c, rel)) continue;
        int64_t id = sqlite3_column_int64(q, 0);
        sqlite3_reset(u);
        sqlite3_bind_int64(u, 1, id);
        sqlite3_step(u);
        sqlite3_reset(gone);
        sqlite3_bind_int64(gone, 1, id);
        sqlite3_step(gone);
        if (!sqlite3_column_int(q, 2)) c->stats->files_deleted++;
    }
    sqlite3_finalize(q);
    sqlite3_finalize(u);
    sqlite3_finalize(gone);
    return BD_OK;
}

static void note_missing(bd_catalog *cat, int64_t source_id, int missing)
{
    sqlite3_stmt *u;
    const char *sql = missing ? "INSERT OR IGNORE INTO temp.missing_sources(source_id) VALUES(?)"
                              : "DELETE FROM temp.missing_sources WHERE source_id=?";
    if (sqlite3_prepare_v2(cat->db, sql, -1, &u, NULL) != SQLITE_OK) return;
    sqlite3_bind_int64(u, 1, source_id);
    sqlite3_step(u);
    sqlite3_finalize(u);
}

static bd_status scan_source(bd_catalog *cat, const skip_list *skip, int bitrot, hash_pool *pool, int64_t source_id,
                             const char *root, int64_t scan_ms, bd_scan_stats *stats, bd_log_fn log, void *log_ctx)
{
    bd_stat_t rst;
    if (bd_stat(root, &rst) != 0 || !rst.is_dir) {
        stats->roots_missing++;
        note_missing(cat, source_id, 1);
        bd_logf(log, log_ctx, "backup root %s is not reachable; skipped (nothing marked deleted)", root);
        return BD_OK;
    }
    note_missing(cat, source_id, 0);
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
        {&c.find_node, "SELECT n.id, n.size, n.mtime_ns, n.current_version_id, n.deleted, v.hash FROM nodes n"
                       " LEFT JOIN versions v ON v.id=n.current_version_id WHERE n.source_id=? AND n.rel_path=?"},
        {&c.find_parent, "SELECT id FROM nodes WHERE source_id=? AND rel_path=?"},
        {&c.insert_node, "INSERT INTO nodes(source_id,parent_id,rel_path,name,is_dir,last_seen_ms,rel_fold,btime_ns)"
                         " VALUES(?,?,?,?,?,?,?,?)"},
        /* The same name in other letter case, not seen yet in this scan. */
        {&c.find_case, "SELECT n.id, n.size, n.mtime_ns, n.current_version_id, n.deleted, v.hash FROM nodes n"
                       " LEFT JOIN versions v ON v.id=n.current_version_id"
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
    int walk_rc = bd_walk(root, scan_entry, scan_error, &c);
    /* The files still with the workers. */
    pool_collect(&c, pool->submitted, walk_rc == 0 && !c.db_error && !c.cancelled);
    if (c.cancelled) {
        /* A half-scanned folder would look like files deleted: leave this
         * one as the last full scan saw it. */
        bd_exec(cat, "ROLLBACK");
        s = bd_fail_cancelled(cat);
        goto done;
    }
    if (walk_rc != 0 && !c.db_error) {
        bd_exec(cat, "ROLLBACK");
        s = bd_fail(cat, BD_ERR_NOMEM, "out of memory while scanning %s", root);
        goto done;
    }
    if (c.db_error) { s = bd_fail_db(cat, "record scan results"); bd_exec(cat, "ROLLBACK"); goto done; }
    s = mark_deleted(&c);
    if (s == BD_OK) {
        sqlite3_stmt *u;
        if (sqlite3_prepare_v2(cat->db, "UPDATE sources SET last_scan_ms=? WHERE id=?", -1, &u, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(u, 1, scan_ms);
            sqlite3_bind_int64(u, 2, source_id);
            sqlite3_step(u);
            sqlite3_finalize(u);
        }
    }
    if (s == BD_OK && bd_exec(cat, "COMMIT") != 0) s = bd_fail_db(cat, "commit scan");
    if (s != BD_OK) bd_exec(cat, "ROLLBACK");

done:
    for (size_t i = 0; i < n; i++) sqlite3_finalize(*stmts[i].st);
    for (size_t i = 0; i < c.failed_len; i++) free(c.failed_dirs[i]);
    free(c.failed_dirs);
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
        if (s == BD_ERR_CANCELLED) *stats = before; /* that folder was rolled back */
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
                           "SELECT n.id, n.source_id, s.name, n.rel_path, n.size, v.hash, d.found_hash, d.found_ms"
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
