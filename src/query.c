/* Read-only queries behind the ghost tree. "Connected" means the drive was
 * passed to bd_media_connect in this session (temp.connected). */
#include "internal.h"

#include <stdlib.h>
#include <string.h>

bd_status bd_list_sources(bd_catalog *cat, bd_source_fn fn, void *ctx)
{
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(cat->db, "SELECT id, name, path FROM sources ORDER BY name COLLATE NOCASE", -1, &q, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "list sources");
    while (sqlite3_step(q) == SQLITE_ROW) {
        bd_source_info info = {sqlite3_column_int64(q, 0), (const char *)sqlite3_column_text(q, 1),
                               (const char *)sqlite3_column_text(q, 2)};
        if (fn(ctx, &info) != 0) break;
    }
    sqlite3_finalize(q);
    return BD_OK;
}

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
            if (deleted) info.state = BD_STATE_DELETED;
            else if (info.files_available == info.files_total) info.state = BD_STATE_AVAILABLE;
            else if (info.files_available > 0) info.state = BD_STATE_PARTIAL;
            else if (with_copy > 0) { info.state = BD_STATE_OFFLINE; info.offline_media_label = label_copy; }
            else info.state = BD_STATE_NO_COPY;
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
                           " MAX(COALESCE(c.last_full_check_ms,0), COALESCE(c.last_quick_check_ms,0))"
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
        if (fn(ctx, &info) != 0) break;
    }
    sqlite3_finalize(q);
    return BD_OK;
}
