/* Read-only queries behind the ghost tree. "Connected" means the drive was
 * passed to bd_media_connect in this session (temp.connected). */
#include "internal.h"

#include <stdlib.h>
#include <string.h>

/* Per-file availability, shared by children listing and folder totals. */
#define CUR_AVAILABLE \
    "EXISTS(SELECT 1 FROM copies c JOIN temp.connected k ON k.media_id=c.media_id" \
    " WHERE c.version_id=n.current_version_id AND c.state='ok')"
#define ANY_AVAILABLE \
    "EXISTS(SELECT 1 FROM copies c JOIN versions v ON v.id=c.version_id JOIN temp.connected k ON k.media_id=c.media_id" \
    " WHERE v.node_id=n.id AND c.state='ok')"
#define BAD_CONNECTED \
    "EXISTS(SELECT 1 FROM copies c JOIN versions v ON v.id=c.version_id JOIN temp.connected k ON k.media_id=c.media_id" \
    " WHERE v.node_id=n.id AND c.state<>'ok')"
#define ANY_COPY \
    "EXISTS(SELECT 1 FROM copies c JOIN versions v ON v.id=c.version_id WHERE v.node_id=n.id AND c.state='ok')"

static bd_node_state file_state(int deleted, int cur_avail, int any_avail, int bad_connected, int any_copy)
{
    if (deleted) return BD_STATE_DELETED;
    if (cur_avail) return BD_STATE_AVAILABLE;
    if (any_avail) return BD_STATE_AVAILABLE_OLDER;
    if (bad_connected) return BD_STATE_BAD;
    if (any_copy) return BD_STATE_OFFLINE;
    return BD_STATE_NO_COPY;
}

static bd_node_state folder_state(int64_t total, int64_t available, int64_t with_copy)
{
    if (available == total) return BD_STATE_AVAILABLE;
    if (available > 0) return BD_STATE_PARTIAL;
    if (with_copy > 0) return BD_STATE_OFFLINE;
    return BD_STATE_NO_COPY;
}

bd_status bd_list_sources(bd_catalog *cat, bd_source_fn fn, void *ctx)
{
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT s.id, s.name, s.path,"
                           " (SELECT COUNT(*) FROM nodes n WHERE n.source_id=s.id AND n.is_dir=0 AND n.deleted=0),"
                           " (SELECT COUNT(*) FROM nodes n WHERE n.source_id=s.id AND n.is_dir=0 AND n.deleted=0 AND " CUR_AVAILABLE "),"
                           " (SELECT COUNT(*) FROM nodes n WHERE n.source_id=s.id AND n.is_dir=0 AND n.deleted=0 AND " ANY_COPY "),"
                           " (SELECT m.label FROM nodes n JOIN copies c ON c.version_id=n.current_version_id"
                           "   JOIN media m ON m.id=c.media_id WHERE n.source_id=s.id AND n.is_dir=0 AND n.deleted=0"
                           "   AND c.state='ok' LIMIT 1),"
                           " (SELECT COALESCE(SUM(n.size),0) FROM nodes n WHERE n.source_id=s.id AND n.is_dir=0 AND n.deleted=0),"
                           " s.active, EXISTS(SELECT 1 FROM temp.missing_sources ms WHERE ms.source_id=s.id),"
                           " COALESCE(s.last_scan_ms,0)"
                           " FROM sources s ORDER BY s.name COLLATE NOCASE",
                           -1, &q, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "list sources");
    while (sqlite3_step(q) == SQLITE_ROW) {
        bd_source_info info;
        memset(&info, 0, sizeof(info));
        info.source_id = sqlite3_column_int64(q, 0);
        info.name = (const char *)sqlite3_column_text(q, 1);
        info.path = (const char *)sqlite3_column_text(q, 2);
        info.files_total = sqlite3_column_int64(q, 3);
        info.files_available = sqlite3_column_int64(q, 4);
        info.state = folder_state(info.files_total, info.files_available, sqlite3_column_int64(q, 5));
        if (info.state == BD_STATE_OFFLINE) info.offline_media_label = (const char *)sqlite3_column_text(q, 6);
        info.bytes_total = sqlite3_column_int64(q, 7);
        info.active = sqlite3_column_int(q, 8);
        info.missing = sqlite3_column_int(q, 9);
        info.last_scan_ms = sqlite3_column_int64(q, 10);
        if (fn(ctx, &info) != 0) break;
    }
    sqlite3_finalize(q);
    return BD_OK;
}

bd_status bd_list_children(bd_catalog *cat, int64_t source_id, int64_t parent_node_id, bd_node_fn fn, void *ctx)
{
    sqlite3_stmt *q = NULL, *agg = NULL;
    bd_status s = BD_OK;
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT n.id, n.is_dir, n.name, n.rel_path, n.size, n.deleted,"
                           " COALESCE((SELECT version_no FROM versions WHERE id=n.current_version_id),0),"
                           " (SELECT COUNT(*) FROM versions WHERE node_id=n.id),"
                           " " CUR_AVAILABLE ", " ANY_AVAILABLE ", " BAD_CONNECTED ", " ANY_COPY ","
                           " COALESCE((SELECT m.label FROM copies c JOIN media m ON m.id=c.media_id"
                           "   WHERE c.version_id=n.current_version_id AND c.state='ok' LIMIT 1),"
                           "  (SELECT m.label FROM copies c JOIN versions v ON v.id=c.version_id JOIN media m ON m.id=c.media_id"
                           "   WHERE v.node_id=n.id AND c.state='ok' ORDER BY v.version_no DESC LIMIT 1))"
                           " FROM nodes n WHERE n.source_id=? AND n.parent_id IS ?"
                           " ORDER BY n.is_dir DESC, n.name COLLATE NOCASE",
                           -1, &q, NULL) != SQLITE_OK) {
        s = bd_fail_db(cat, "list folder");
        goto done;
    }
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT COUNT(*), COALESCE(SUM(" CUR_AVAILABLE "),0), COALESCE(SUM(" ANY_COPY "),0),"
                           " (SELECT m.label FROM nodes n2 JOIN copies c ON c.version_id=n2.current_version_id"
                           "   JOIN media m ON m.id=c.media_id WHERE n2.source_id=?1 AND n2.is_dir=0 AND n2.deleted=0"
                           "   AND c.state='ok' AND substr(n2.rel_path,1,length(?2)+1)=?2||'/' LIMIT 1)"
                           " FROM nodes n WHERE n.source_id=?1 AND n.is_dir=0 AND n.deleted=0"
                           " AND substr(n.rel_path,1,length(?2)+1)=?2||'/'",
                           -1, &agg, NULL) != SQLITE_OK) {
        s = bd_fail_db(cat, "count folder");
        goto done;
    }
    sqlite3_bind_int64(q, 1, source_id);
    if (parent_node_id) sqlite3_bind_int64(q, 2, parent_node_id);
    else sqlite3_bind_null(q, 2);

    while (sqlite3_step(q) == SQLITE_ROW) {
        bd_node_info info;
        memset(&info, 0, sizeof(info));
        info.node_id = sqlite3_column_int64(q, 0);
        info.source_id = source_id;
        info.is_dir = sqlite3_column_int(q, 1);
        info.name = (const char *)sqlite3_column_text(q, 2);
        info.rel_path = (const char *)sqlite3_column_text(q, 3);
        info.size = sqlite3_column_int64(q, 4);
        int deleted = sqlite3_column_int(q, 5);
        info.version_no = sqlite3_column_int(q, 6);
        info.version_count = sqlite3_column_int(q, 7);
        info.offline_media_label = (const char *)sqlite3_column_text(q, 12);

        char *label_copy = NULL;
        if (!info.is_dir) {
            info.state = file_state(deleted, sqlite3_column_int(q, 8), sqlite3_column_int(q, 9),
                                    sqlite3_column_int(q, 10), sqlite3_column_int(q, 11));
            if (info.state != BD_STATE_OFFLINE) info.offline_media_label = NULL;
        } else {
            sqlite3_reset(agg);
            sqlite3_bind_int64(agg, 1, source_id);
            sqlite3_bind_text(agg, 2, info.rel_path, -1, SQLITE_TRANSIENT);
            int64_t with_copy = 0;
            info.offline_media_label = NULL;
            if (sqlite3_step(agg) == SQLITE_ROW) {
                info.files_total = sqlite3_column_int64(agg, 0);
                info.files_available = sqlite3_column_int64(agg, 1);
                with_copy = sqlite3_column_int64(agg, 2);
                label_copy = bd_strdup((const char *)sqlite3_column_text(agg, 3));
            }
            info.state = deleted ? BD_STATE_DELETED : folder_state(info.files_total, info.files_available, with_copy);
            if (info.state == BD_STATE_OFFLINE) info.offline_media_label = label_copy;
        }
        int stop = fn(ctx, &info);
        free(label_copy);
        if (stop) break;
    }
done:
    sqlite3_finalize(q);
    sqlite3_finalize(agg);
    return s;
}

bd_status bd_find_node(bd_catalog *cat, int64_t source_id, const char *rel_path, int64_t *out_node_id)
{
    char *rel = bd_strdup(rel_path);
    if (!rel) return BD_ERR_NOMEM;
    for (char *p = rel; *p; p++) if (*p == '\\') *p = '/';
    char *start = rel;
    while (*start == '/') start++;
    size_t len = strlen(start);
    while (len > 0 && start[len - 1] == '/') start[--len] = '\0';

    sqlite3_stmt *q;
    bd_status s = BD_OK;
    if (sqlite3_prepare_v2(cat->db, "SELECT id FROM nodes WHERE source_id=? AND rel_path=?", -1, &q, NULL) != SQLITE_OK) {
        free(rel);
        return bd_fail_db(cat, "find file");
    }
    sqlite3_bind_int64(q, 1, source_id);
    sqlite3_bind_text(q, 2, start, -1, SQLITE_STATIC);
    if (sqlite3_step(q) == SQLITE_ROW) *out_node_id = sqlite3_column_int64(q, 0);
    else s = bd_fail(cat, BD_ERR_NOT_FOUND, "%s is not in the catalog", rel_path);
    sqlite3_finalize(q);
    free(rel);
    return s;
}

bd_status bd_list_copies(bd_catalog *cat, int64_t node_id, bd_copy_fn fn, void *ctx)
{
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT v.version_no, v.id=n.current_version_id, v.hash, v.size, v.mtime_ns, v.first_seen_ms,"
                           " COALESCE(c.media_id,0), m.label, c.path_on_media, c.state, k.media_id IS NOT NULL,"
                           " MAX(COALESCE(c.last_full_check_ms,0), COALESCE(c.last_quick_check_ms,0)), COALESCE(c.encrypted<>0 OR m.encrypted<>0,0),"
                           " COALESCE(m.location,''), v.id, m.retired_ms IS NOT NULL"
                           " FROM versions v JOIN nodes n ON n.id=v.node_id"
                           " LEFT JOIN copies c ON c.version_id=v.id LEFT JOIN media m ON m.id=c.media_id"
                           " LEFT JOIN temp.connected k ON k.media_id=c.media_id"
                           " WHERE v.node_id=? ORDER BY v.version_no DESC, m.label COLLATE NOCASE",
                           -1, &q, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "list copies");
    sqlite3_bind_int64(q, 1, node_id);
    while (sqlite3_step(q) == SQLITE_ROW) {
        bd_copy_info info;
        info.version_no = sqlite3_column_int(q, 0);
        info.is_current = sqlite3_column_int(q, 1);
        info.hash = (const char *)sqlite3_column_text(q, 2);
        info.size = sqlite3_column_int64(q, 3);
        info.mtime_ns = sqlite3_column_int64(q, 4);
        info.first_seen_ms = sqlite3_column_int64(q, 5);
        info.media_id = sqlite3_column_int64(q, 6);
        info.media_label = (const char *)sqlite3_column_text(q, 7);
        info.path_on_media = (const char *)sqlite3_column_text(q, 8);
        info.copy_state = (const char *)sqlite3_column_text(q, 9);
        info.connected = sqlite3_column_int(q, 10);
        info.last_check_ms = sqlite3_column_int64(q, 11);
        info.encrypted = sqlite3_column_int(q, 12);
        info.media_location = info.media_id ? (const char *)sqlite3_column_text(q, 13) : "";
        info.version_id = sqlite3_column_int64(q, 14);
        info.media_lost = info.media_id ? sqlite3_column_int(q, 15) : 0;
        if (fn(ctx, &info) != 0) break;
    }
    sqlite3_finalize(q);
    return BD_OK;
}

static void col_text(sqlite3_stmt *q, int col, char *out, size_t cap)
{
    const unsigned char *t = sqlite3_column_text(q, col);
    snprintf(out, cap, "%s", t ? (const char *)t : "");
}

static int64_t col_num(sqlite3_stmt *q, int col)
{
    return sqlite3_column_type(q, col) == SQLITE_NULL ? -1 : sqlite3_column_int64(q, col);
}

bd_status bd_list_media(bd_catalog *cat, bd_media_fn fn, void *ctx)
{
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT m.id, m.label, m.kind, COALESCE(k.root, m.last_root), k.media_id IS NOT NULL,"
                           " COALESCE(m.total_bytes,0), COALESCE(m.free_bytes,0), COALESCE(m.last_seen_ms,0),"
                           " (SELECT COUNT(*) FROM copies c WHERE c.media_id=m.id), m.encrypted, COALESCE(m.location,''), m.added_ms,"
                           " h.read_ms, h.vendor, h.model, h.serial, h.firmware, h.bus, h.disk_bytes, h.volume_name, h.volume_serial,"
                           " h.filesystem, h.smart, h.health, h.temperature_c, h.power_on_hours, h.power_cycles, h.reallocated,"
                           " h.pending, h.uncorrectable, h.percent_used, h.note,"
                           " (SELECT MIN(COALESCE(c.last_full_check_ms, c.written_ms)) FROM copies c"
                           "   WHERE c.media_id=m.id AND c.state='ok'),"
                           " (SELECT MAX(l.at_ms) FROM space_log l WHERE l.media_id=m.id),"
                           " m.retired_ms, COALESCE(m.lost_why,''),"
                           " (SELECT COALESCE(SUM(v.size),0) FROM copies c JOIN versions v ON v.id=c.version_id"
                           "   WHERE c.media_id=m.id AND c.state='ok'),"
                           " m.secondary, COALESCE(m.limit_bytes,0), m.hide_names"
                           " FROM media m LEFT JOIN temp.connected k ON k.media_id=m.id LEFT JOIN media_hardware h ON h.media_id=m.id"
                           " ORDER BY m.label COLLATE NOCASE",
                           -1, &q, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "list drives");
    int check_days = bd_option_get(cat, "check_days");
    int64_t reserve = bd_reserve_bytes(cat);
    int64_t now = bd_now_ms();
    while (sqlite3_step(q) == SQLITE_ROW) {
        bd_media_info info;
        info.media_id = sqlite3_column_int64(q, 0);
        info.label = (const char *)sqlite3_column_text(q, 1);
        info.kind = (const char *)sqlite3_column_text(q, 2);
        info.last_root = (const char *)sqlite3_column_text(q, 3);
        info.connected = sqlite3_column_int(q, 4);
        info.total_bytes = sqlite3_column_int64(q, 5);
        info.free_bytes = sqlite3_column_int64(q, 6);
        info.last_seen_ms = sqlite3_column_int64(q, 7);
        info.copies = sqlite3_column_int64(q, 8);
        info.encrypted = sqlite3_column_int(q, 9);
        info.location = (const char *)sqlite3_column_text(q, 10);
        info.added_ms = sqlite3_column_int64(q, 11);
        info.oldest_check_ms = col_num(q, 32);
        info.space_ms = col_num(q, 33);
        if (info.space_ms < 0) info.space_ms = 0;
        if (info.oldest_check_ms < 0) info.oldest_check_ms = 0;
        info.lost_ms = sqlite3_column_type(q, 34) == SQLITE_NULL ? 0 : sqlite3_column_int64(q, 34);
        info.lost_why = info.lost_ms ? (const char *)sqlite3_column_text(q, 35) : "";
        if (info.lost_ms && !info.lost_why[0]) info.lost_why = "lost";
        info.bytes_held = sqlite3_column_int64(q, 36);
        info.reserve_bytes = reserve;
        info.secondary = sqlite3_column_int(q, 37);
        info.limit_bytes = sqlite3_column_int64(q, 38);
        info.hide_names = sqlite3_column_int(q, 39);
        info.check_due = check_days > 0 && info.oldest_check_ms > 0 &&
                         now - info.oldest_check_ms >= (int64_t)check_days * 24 * 3600 * 1000;
        bd_drive_hw hw;
        info.hw = NULL;
        info.hw_read_ms = 0;
        if (sqlite3_column_type(q, 12) != SQLITE_NULL) {
            memset(&hw, 0, sizeof(hw));
            info.hw_read_ms = sqlite3_column_int64(q, 12);
            col_text(q, 13, hw.vendor, sizeof(hw.vendor));
            col_text(q, 14, hw.model, sizeof(hw.model));
            col_text(q, 15, hw.serial, sizeof(hw.serial));
            col_text(q, 16, hw.firmware, sizeof(hw.firmware));
            col_text(q, 17, hw.bus, sizeof(hw.bus));
            hw.disk_bytes = col_num(q, 18);
            col_text(q, 19, hw.volume_name, sizeof(hw.volume_name));
            col_text(q, 20, hw.volume_serial, sizeof(hw.volume_serial));
            col_text(q, 21, hw.filesystem, sizeof(hw.filesystem));
            hw.smart = sqlite3_column_int(q, 22);
            col_text(q, 23, hw.health, sizeof(hw.health));
            hw.temperature_c = (int)col_num(q, 24);
            hw.power_on_hours = col_num(q, 25);
            hw.power_cycles = col_num(q, 26);
            hw.reallocated_sectors = col_num(q, 27);
            hw.pending_sectors = col_num(q, 28);
            hw.uncorrectable_sectors = col_num(q, 29);
            hw.percent_used = (int)col_num(q, 30);
            col_text(q, 31, hw.note, sizeof(hw.note));
            info.hw = &hw;
        }
        if (fn(ctx, &info) != 0) break;
    }
    sqlite3_finalize(q);
    return BD_OK;
}

/* ---- Search ------------------------------------------------------------- */

/* "Label (kept in)" for a medium m. */
#define WHERE_TEXT "m.label || CASE WHEN trim(COALESCE(m.location,''))<>'' THEN ' ('||trim(m.location)||')' ELSE '' END"

#define MAX_WORDS 8

/* %word% with LIKE's wildcards escaped by '\'. */
static char *like_pattern(const char *word, size_t n)
{
    char *p = malloc(n * 2 + 3);
    if (!p) return NULL;
    size_t j = 0;
    p[j++] = '%';
    for (size_t i = 0; i < n; i++) {
        if (word[i] == '%' || word[i] == '_' || word[i] == '\\') p[j++] = '\\';
        p[j++] = word[i];
    }
    p[j++] = '%';
    p[j] = '\0';
    return p;
}

bd_status bd_search(bd_catalog *cat, const char *text, int limit, bd_search_fn fn, void *ctx)
{
    char *pat[MAX_WORDS];
    int nw = 0;
    for (const char *p = text ? text : ""; *p && nw < MAX_WORDS;) {
        while (*p == ' ' || *p == '\t') p++;
        const char *start = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (p > start) {
            /* Paths are stored with '/'; let people type either. */
            char word[256];
            size_t n = (size_t)(p - start) < sizeof(word) - 1 ? (size_t)(p - start) : sizeof(word) - 1;
            memcpy(word, start, n);
            word[n] = '\0';
            for (char *c = word; *c; c++) if (*c == '\\') *c = '/';
            char *w = like_pattern(word, n);
            if (!w) break;
            pat[nw++] = w;
        }
    }
    if (nw == 0) return BD_OK;
    if (limit <= 0) limit = 500;

    char path_where[MAX_WORDS * 48], name_match[MAX_WORDS * 48];
    path_where[0] = name_match[0] = '\0';
    for (int i = 0; i < nw; i++) {
        size_t a = strlen(path_where), b = strlen(name_match);
        snprintf(path_where + a, sizeof(path_where) - a, "%sn.rel_path LIKE ?%d ESCAPE '\\'", i ? " AND " : "", i + 1);
        snprintf(name_match + b, sizeof(name_match) - b, "%sn.name LIKE ?%d ESCAPE '\\'", i ? " AND " : "", i + 1);
    }
    char *sql = bd_sprintf(
        "SELECT n.id, n.source_id, s.name, n.rel_path, n.name, n.is_dir, n.size, n.deleted,"
        " " CUR_AVAILABLE ", " ANY_AVAILABLE ", " BAD_CONNECTED ", " ANY_COPY ","
        " (SELECT group_concat(w, '; ') FROM (SELECT DISTINCT " WHERE_TEXT " AS w FROM copies c JOIN media m ON m.id=c.media_id"
        "   WHERE c.version_id=n.current_version_id AND c.state='ok' ORDER BY 1)),"
        " (%s) AS nm"
        " FROM nodes n JOIN sources s ON s.id=n.source_id WHERE %s"
        " ORDER BY nm DESC, n.deleted, n.is_dir DESC, s.name COLLATE NOCASE, n.rel_path COLLATE NOCASE LIMIT %d",
        name_match, path_where, limit);
    sqlite3_stmt *q = NULL, *dir = NULL;
    bd_status st = BD_OK;
    if (!sql || sqlite3_prepare_v2(cat->db, sql, -1, &q, NULL) != SQLITE_OK) {
        st = sql ? bd_fail_db(cat, "search") : BD_ERR_NOMEM;
        goto done;
    }
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT COUNT(*), COALESCE(SUM(" CUR_AVAILABLE "),0), COALESCE(SUM(" ANY_COPY "),0),"
                           " (SELECT group_concat(w, '; ') FROM (SELECT DISTINCT " WHERE_TEXT " AS w"
                           "   FROM nodes n2 JOIN copies c ON c.version_id=n2.current_version_id JOIN media m ON m.id=c.media_id"
                           "   WHERE n2.source_id=?1 AND n2.is_dir=0 AND n2.deleted=0 AND c.state='ok'"
                           "   AND substr(n2.rel_path,1,length(?2)+1)=?2||'/' ORDER BY 1))"
                           " FROM nodes n WHERE n.source_id=?1 AND n.is_dir=0 AND n.deleted=0"
                           " AND substr(n.rel_path,1,length(?2)+1)=?2||'/'",
                           -1, &dir, NULL) != SQLITE_OK) {
        st = bd_fail_db(cat, "search");
        goto done;
    }
    for (int i = 0; i < nw; i++) sqlite3_bind_text(q, i + 1, pat[i], -1, SQLITE_STATIC);
    while (sqlite3_step(q) == SQLITE_ROW) {
        bd_search_info info;
        memset(&info, 0, sizeof(info));
        info.node_id = sqlite3_column_int64(q, 0);
        info.source_id = sqlite3_column_int64(q, 1);
        info.source_name = (const char *)sqlite3_column_text(q, 2);
        info.rel_path = (const char *)sqlite3_column_text(q, 3);
        info.name = (const char *)sqlite3_column_text(q, 4);
        info.is_dir = sqlite3_column_int(q, 5);
        int deleted = sqlite3_column_int(q, 7);
        char *where = NULL;
        if (!info.is_dir) {
            info.size = sqlite3_column_int64(q, 6);
            info.state = file_state(deleted, sqlite3_column_int(q, 8), sqlite3_column_int(q, 9),
                                    sqlite3_column_int(q, 10), sqlite3_column_int(q, 11));
            where = bd_strdup(sqlite3_column_text(q, 12) ? (const char *)sqlite3_column_text(q, 12) : "");
        } else {
            sqlite3_reset(dir);
            sqlite3_bind_int64(dir, 1, info.source_id);
            sqlite3_bind_text(dir, 2, info.rel_path, -1, SQLITE_TRANSIENT);
            info.state = BD_STATE_NO_COPY;
            if (sqlite3_step(dir) == SQLITE_ROW) {
                info.state = deleted ? BD_STATE_DELETED
                                     : folder_state(sqlite3_column_int64(dir, 0), sqlite3_column_int64(dir, 1),
                                                    sqlite3_column_int64(dir, 2));
                where = bd_strdup(sqlite3_column_text(dir, 3) ? (const char *)sqlite3_column_text(dir, 3) : "");
            }
        }
        info.where = where ? where : "";
        int stop = fn(ctx, &info);
        free(where);
        if (stop) break;
    }
done:
    sqlite3_finalize(q);
    sqlite3_finalize(dir);
    free(sql);
    for (int i = 0; i < nw; i++) free(pat[i]);
    return st;
}

/* ---- Deleted files ----------------------------------------------------------- */

#define DELETED_WHERE                                                                                        \
    " WHERE n.deleted<>0 AND n.is_dir=0 AND (?1=0 OR n.source_id=?1) AND (?2=0 OR n.deleted_ack=0)"

bd_status bd_list_deleted(bd_catalog *cat, int64_t source_id, int new_only, bd_deleted_fn fn, void *ctx, int64_t *count)
{
    sqlite3_stmt *q;
    if (count) {
        *count = 0;
        if (sqlite3_prepare_v2(cat->db, "SELECT COUNT(*) FROM nodes n" DELETED_WHERE, -1, &q, NULL) != SQLITE_OK)
            return bd_fail_db(cat, "count deleted files");
        sqlite3_bind_int64(q, 1, source_id);
        sqlite3_bind_int(q, 2, new_only);
        if (sqlite3_step(q) == SQLITE_ROW) *count = sqlite3_column_int64(q, 0);
        sqlite3_finalize(q);
    }
    if (!fn) return BD_OK;
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT n.id, n.source_id, s.name, n.rel_path, COALESCE(v.size, n.size), n.last_seen_ms,"
                           " (SELECT group_concat(label, ', ') FROM (SELECT DISTINCT m.label FROM copies c JOIN media m ON m.id=c.media_id"
                           "   WHERE c.version_id=v.id AND c.state='ok' ORDER BY m.label)),"
                           " EXISTS(SELECT 1 FROM copies c JOIN temp.connected k ON k.media_id=c.media_id"
                           "   WHERE c.version_id=v.id AND c.state='ok'),"
                           " EXISTS(SELECT 1 FROM copies c WHERE c.version_id=v.id AND c.state='ok' AND " BD_SQL_USABLE("c") "),"
                           " n.deleted_ack"
                           " FROM nodes n JOIN sources s ON s.id=n.source_id LEFT JOIN versions v ON v.id=n.current_version_id"
                           DELETED_WHERE
                           " ORDER BY n.last_seen_ms DESC, s.name COLLATE NOCASE, n.rel_path COLLATE NOCASE",
                           -1, &q, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "list deleted files");
    sqlite3_bind_int64(q, 1, source_id);
    sqlite3_bind_int(q, 2, new_only);
    while (sqlite3_step(q) == SQLITE_ROW) {
        bd_deleted_info info;
        info.node_id = sqlite3_column_int64(q, 0);
        info.source_id = sqlite3_column_int64(q, 1);
        info.source_name = (const char *)sqlite3_column_text(q, 2);
        info.rel_path = (const char *)sqlite3_column_text(q, 3);
        info.size = sqlite3_column_int64(q, 4);
        info.last_seen_ms = sqlite3_column_int64(q, 5);
        info.drives = sqlite3_column_type(q, 6) == SQLITE_NULL ? "" : (const char *)sqlite3_column_text(q, 6);
        info.connected = sqlite3_column_int(q, 7);
        info.backed_up = sqlite3_column_int(q, 8);
        info.acknowledged = sqlite3_column_int(q, 9);
        if (fn(ctx, &info) != 0) break;
    }
    sqlite3_finalize(q);
    return BD_OK;
}

bd_status bd_deleted_ack(bd_catalog *cat, int64_t node_id)
{
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(cat->db, "UPDATE nodes SET deleted_ack=1 WHERE deleted<>0 AND (?1=0 OR id=?1)", -1, &q, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "mark deleted files as seen");
    sqlite3_bind_int64(q, 1, node_id);
    int rc = sqlite3_step(q);
    sqlite3_finalize(q);
    if (rc != SQLITE_DONE) return bd_fail_db(cat, "mark deleted files as seen");
    return BD_OK;
}

bd_status bd_deleted_forget(bd_catalog *cat, int64_t node_id, int64_t *forgotten)
{
    if (forgotten) *forgotten = 0;
    sqlite3_stmt *q;
    if (node_id) {
        int deleted = -1;
        if (sqlite3_prepare_v2(cat->db, "SELECT deleted FROM nodes WHERE id=?", -1, &q, NULL) != SQLITE_OK)
            return bd_fail_db(cat, "forget a deleted file");
        sqlite3_bind_int64(q, 1, node_id);
        if (sqlite3_step(q) == SQLITE_ROW) deleted = sqlite3_column_int(q, 0);
        sqlite3_finalize(q);
        if (deleted < 0) return bd_fail(cat, BD_ERR_NOT_FOUND, "that file is not in the catalog");
        if (!deleted) return bd_fail(cat, BD_ERR_INVALID, "only a deleted file can be forgotten");
    }
    if (sqlite3_exec(cat->db, "BEGIN", NULL, NULL, NULL) != SQLITE_OK) return bd_fail_db(cat, "forget deleted files");
    /* Count the files first; the folders' children go with them (ON DELETE CASCADE). */
    int64_t n = 0;
    if (sqlite3_prepare_v2(cat->db,
                           "WITH RECURSIVE t(id) AS (SELECT id FROM nodes WHERE deleted<>0 AND (?1=0 OR id=?1)"
                           " UNION SELECT c.id FROM nodes c JOIN t ON c.parent_id=t.id)"
                           " SELECT COUNT(*) FROM nodes WHERE is_dir=0 AND id IN (SELECT id FROM t)",
                           -1, &q, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(q, 1, node_id);
        if (sqlite3_step(q) == SQLITE_ROW) n = sqlite3_column_int64(q, 0);
        sqlite3_finalize(q);
    }
    if (sqlite3_prepare_v2(cat->db, "DELETE FROM nodes WHERE deleted<>0 AND (?1=0 OR id=?1)", -1, &q, NULL) != SQLITE_OK) {
        sqlite3_exec(cat->db, "ROLLBACK", NULL, NULL, NULL);
        return bd_fail_db(cat, "forget deleted files");
    }
    sqlite3_bind_int64(q, 1, node_id);
    int rc = sqlite3_step(q);
    sqlite3_finalize(q);
    if (rc != SQLITE_DONE || sqlite3_exec(cat->db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_exec(cat->db, "ROLLBACK", NULL, NULL, NULL);
        return bd_fail_db(cat, "forget deleted files");
    }
    if (forgotten) *forgotten = n;
    return BD_OK;
}
