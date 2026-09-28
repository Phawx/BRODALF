#include "internal.h"

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

static int name_taken(bd_catalog *cat, const char *name)
{
    sqlite3_stmt *st;
    int taken = 1;
    if (sqlite3_prepare_v2(cat->db, "SELECT 1 FROM sources WHERE name=? COLLATE NOCASE", -1, &st, NULL) != SQLITE_OK) return 1;
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    taken = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    return taken;
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

    char *base = sanitize_name(path);
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

typedef struct {
    bd_catalog *cat;
    int64_t source_id;
    const char *root;
    int64_t scan_ms;
    bd_scan_stats *stats;
    bd_log_fn log;
    void *log_ctx;
    sqlite3_stmt *find_node, *find_parent, *insert_node, *touch_node, *update_file;
    sqlite3_stmt *find_version, *next_version_no, *insert_version;
    char **failed_dirs; /* relative folders that could not be read */
    size_t failed_len, failed_cap;
    int db_error;
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

/* Returns the version id for this hash, creating the next version if new. */
static int64_t ensure_version(scan_ctx *c, int64_t node_id, const char *hash, int64_t size, int64_t mtime_ns, int *created)
{
    *created = 0;
    sqlite3_reset(c->find_version);
    sqlite3_bind_int64(c->find_version, 1, node_id);
    sqlite3_bind_text(c->find_version, 2, hash, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(c->find_version) == SQLITE_ROW) return sqlite3_column_int64(c->find_version, 0);

    sqlite3_reset(c->next_version_no);
    sqlite3_bind_int64(c->next_version_no, 1, node_id);
    int64_t no = 1;
    if (sqlite3_step(c->next_version_no) == SQLITE_ROW) no = sqlite3_column_int64(c->next_version_no, 0);

    sqlite3_reset(c->insert_version);
    sqlite3_bind_int64(c->insert_version, 1, node_id);
    sqlite3_bind_int64(c->insert_version, 2, no);
    sqlite3_bind_text(c->insert_version, 3, hash, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(c->insert_version, 4, size);
    sqlite3_bind_int64(c->insert_version, 5, mtime_ns);
    sqlite3_bind_int64(c->insert_version, 6, c->scan_ms);
    if (sqlite3_step(c->insert_version) != SQLITE_DONE) return 0;
    *created = 1;
    return sqlite3_last_insert_rowid(c->cat->db);
}

static int scan_entry(void *ctx, const char *rel, const bd_stat_t *st)
{
    scan_ctx *c = ctx;
    if (st->is_link) {
        c->stats->skipped_links++;
        bd_logf(c->log, c->log_ctx, "skipped link: %s", rel);
        return 0;
    }
    if (!st->is_dir && !st->is_file) return 0;

    sqlite3_reset(c->find_node);
    sqlite3_bind_int64(c->find_node, 1, c->source_id);
    sqlite3_bind_text(c->find_node, 2, rel, -1, SQLITE_TRANSIENT);
    int64_t node_id = 0, old_size = -1, old_mtime = -1, old_version = 0;
    if (sqlite3_step(c->find_node) == SQLITE_ROW) {
        node_id = sqlite3_column_int64(c->find_node, 0);
        old_size = sqlite3_column_int64(c->find_node, 1);
        old_mtime = sqlite3_column_int64(c->find_node, 2);
        old_version = sqlite3_column_int64(c->find_node, 3);
    }
    int64_t parent = lookup_parent(c, rel);

    if (st->is_dir) c->stats->dirs_seen++;
    else c->stats->files_seen++;
    bd_report(c->cat, "scan", c->stats->files_seen, c->stats->bytes_hashed, rel, 0);

    if (!node_id) {
        sqlite3_reset(c->insert_node);
        sqlite3_bind_int64(c->insert_node, 1, c->source_id);
        bind_parent(c->insert_node, 2, parent);
        sqlite3_bind_text(c->insert_node, 3, rel, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(c->insert_node, 4, bd_rel_basename(rel), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(c->insert_node, 5, st->is_dir);
        sqlite3_bind_int64(c->insert_node, 6, c->scan_ms);
        if (sqlite3_step(c->insert_node) != SQLITE_DONE) { c->db_error = 1; return 1; }
        node_id = sqlite3_last_insert_rowid(c->cat->db);
    } else {
        sqlite3_reset(c->touch_node);
        bind_parent(c->touch_node, 1, parent);
        sqlite3_bind_int(c->touch_node, 2, st->is_dir);
        sqlite3_bind_int64(c->touch_node, 3, c->scan_ms);
        sqlite3_bind_int64(c->touch_node, 4, node_id);
        if (sqlite3_step(c->touch_node) != SQLITE_DONE) { c->db_error = 1; return 1; }
    }
    if (st->is_dir) return 0;

    int is_new = old_size < 0;
    if (!is_new && old_version && old_size == st->size && old_mtime == st->mtime_ns) return 0;

    char *full = bd_path_join(c->root, rel);
    char hash[BD_HASH_HEX_LEN + 1];
    int64_t hashed = 0;
    int rc = full ? bd_hash_file(full, NULL, hash, &hashed) : -1;
    free(full);
    if (rc != 0) {
        c->stats->errors++;
        bd_logf(c->log, c->log_ctx, "cannot read %s; keeping what the catalog already knows", rel);
        return 0;
    }
    c->stats->bytes_hashed += hashed;

    int created = 0;
    int64_t version_id = ensure_version(c, node_id, hash, hashed, st->mtime_ns, &created);
    if (!version_id) { c->db_error = 1; return 1; }

    sqlite3_reset(c->update_file);
    sqlite3_bind_int64(c->update_file, 1, hashed);
    sqlite3_bind_int64(c->update_file, 2, st->mtime_ns);
    sqlite3_bind_int64(c->update_file, 3, version_id);
    sqlite3_bind_int64(c->update_file, 4, node_id);
    if (sqlite3_step(c->update_file) != SQLITE_DONE) { c->db_error = 1; return 1; }

    if (is_new) c->stats->files_new++;
    else if (version_id != old_version) c->stats->files_changed++;
    return 0;
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

static bd_status mark_deleted(scan_ctx *c)
{
    sqlite3_stmt *q, *u;
    if (sqlite3_prepare_v2(c->cat->db, "SELECT id, rel_path, is_dir FROM nodes WHERE source_id=? AND deleted=0 AND last_seen_ms<?", -1, &q, NULL) != SQLITE_OK)
        return bd_fail_db(c->cat, "find deleted files");
    if (sqlite3_prepare_v2(c->cat->db, "UPDATE nodes SET deleted=1 WHERE id=?", -1, &u, NULL) != SQLITE_OK) {
        sqlite3_finalize(q);
        return bd_fail_db(c->cat, "mark deleted files");
    }
    sqlite3_bind_int64(q, 1, c->source_id);
    sqlite3_bind_int64(q, 2, c->scan_ms);
    while (sqlite3_step(q) == SQLITE_ROW) {
        const char *rel = (const char *)sqlite3_column_text(q, 1);
        if (under_failed_dir(c, rel)) continue;
        sqlite3_reset(u);
        sqlite3_bind_int64(u, 1, sqlite3_column_int64(q, 0));
        sqlite3_step(u);
        if (!sqlite3_column_int(q, 2)) c->stats->files_deleted++;
    }
    sqlite3_finalize(q);
    sqlite3_finalize(u);
    return BD_OK;
}

static bd_status scan_source(bd_catalog *cat, int64_t source_id, const char *root, int64_t scan_ms,
                             bd_scan_stats *stats, bd_log_fn log, void *log_ctx)
{
    bd_stat_t rst;
    if (bd_stat(root, &rst) != 0 || !rst.is_dir) {
        stats->errors++;
        bd_logf(log, log_ctx, "source folder %s is not reachable; skipped (nothing marked deleted)", root);
        return BD_OK;
    }
    scan_ctx c = {0};
    c.cat = cat;
    c.source_id = source_id;
    c.root = root;
    c.scan_ms = scan_ms;
    c.stats = stats;
    c.log = log;
    c.log_ctx = log_ctx;

    struct { sqlite3_stmt **st; const char *sql; } stmts[] = {
        {&c.find_node, "SELECT id, size, mtime_ns, current_version_id FROM nodes WHERE source_id=? AND rel_path=?"},
        {&c.find_parent, "SELECT id FROM nodes WHERE source_id=? AND rel_path=?"},
        {&c.insert_node, "INSERT INTO nodes(source_id,parent_id,rel_path,name,is_dir,last_seen_ms) VALUES(?,?,?,?,?,?)"},
        {&c.touch_node, "UPDATE nodes SET parent_id=?, is_dir=?, last_seen_ms=?, deleted=0 WHERE id=?"},
        {&c.update_file, "UPDATE nodes SET size=?, mtime_ns=?, current_version_id=? WHERE id=?"},
        {&c.find_version, "SELECT id FROM versions WHERE node_id=? AND hash=?"},
        {&c.next_version_no, "SELECT COALESCE(MAX(version_no),0)+1 FROM versions WHERE node_id=?"},
        {&c.insert_version, "INSERT INTO versions(node_id,version_no,hash,size,mtime_ns,first_seen_ms) VALUES(?,?,?,?,?,?)"},
    };
    bd_status s = BD_OK;
    size_t n = sizeof(stmts) / sizeof(stmts[0]);
    for (size_t i = 0; i < n; i++)
        if (sqlite3_prepare_v2(cat->db, stmts[i].sql, -1, stmts[i].st, NULL) != SQLITE_OK) { s = bd_fail_db(cat, "prepare scan"); goto done; }

    if (bd_exec(cat, "BEGIN") != 0) { s = bd_fail_db(cat, "begin scan"); goto done; }
    if (bd_walk(root, scan_entry, scan_error, &c) != 0 && !c.db_error) {
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
    return s;
}

bd_status bd_scan(bd_catalog *cat, bd_scan_stats *stats, bd_log_fn log, void *log_ctx)
{
    bd_scan_stats local;
    if (!stats) stats = &local;
    memset(stats, 0, sizeof(*stats));

    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(cat->db, "SELECT id, path, COALESCE(last_scan_ms,0) FROM sources ORDER BY id", -1, &q, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "list sources");
    typedef struct { int64_t id; char *path; } src;
    src *list = NULL;
    size_t len = 0;
    int64_t scan_ms = bd_now_ms();
    while (sqlite3_step(q) == SQLITE_ROW) {
        src *nl = realloc(list, (len + 1) * sizeof(src));
        if (!nl) break;
        list = nl;
        list[len].id = sqlite3_column_int64(q, 0);
        list[len].path = bd_strdup((const char *)sqlite3_column_text(q, 1));
        /* last_seen_ms marks what this scan saw, so it must move forward. */
        int64_t prev = sqlite3_column_int64(q, 2);
        if (scan_ms <= prev) scan_ms = prev + 1;
        len++;
    }
    sqlite3_finalize(q);

    bd_status s = BD_OK;
    for (size_t i = 0; i < len && s == BD_OK; i++) {
        bd_logf(log, log_ctx, "scanning %s", list[i].path);
        s = scan_source(cat, list[i].id, list[i].path, scan_ms, stats, log, log_ctx);
    }
    for (size_t i = 0; i < len; i++) free(list[i].path);
    free(list);
    bd_report(cat, "scan", stats->files_seen, stats->bytes_hashed, NULL, 1);
    return s;
}
