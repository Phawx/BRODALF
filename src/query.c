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

/* The good copies of the current version of a node n, on drives that count. */
#define CUR_COPY \
    "EXISTS(SELECT 1 FROM copies c WHERE c.version_id=n.current_version_id AND c.state='ok' AND " BD_SQL_USABLE("c") ")"

/* SQL: drive m is one to name where copies are: it is not marked lost or
 * broken, or it is plugged in now (a restore reads from it then). */
#define NAMED_DRIVE \
    "(m.retired_ms IS NULL OR EXISTS(SELECT 1 FROM temp.connected rk WHERE rk.media_id=m.id))"

/* Drives and extra places holding a good copy of the current version of a
 * file under a folder (?2 its path, "" for the whole source ?1), counting
 * the deleted files when ?3 is not 0: name, plugged in now. */
#define DRIVES_UNDER \
    "SELECT m.label, k.media_id IS NOT NULL FROM media m LEFT JOIN temp.connected k ON k.media_id=m.id" \
    " WHERE " NAMED_DRIVE " AND m.id IN (SELECT c.media_id FROM nodes n JOIN copies c ON c.version_id=n.current_version_id" \
    "   WHERE n.source_id=?1 AND n.is_dir=0 AND (n.deleted=0 OR ?3) AND c.state='ok'" \
    "   AND (?2='' OR substr(n.rel_path,1,length(?2)+1)=?2||'/'))" \
    " ORDER BY m.label COLLATE NOCASE"

/* Names, ", " between them, built up one at a time. */
typedef struct { char *p; size_t len, cap; } name_list;

static void names_add(name_list *l, const char *name)
{
    if (!name) return;
    size_t add = strlen(name) + 2;
    if (l->len + add + 1 > l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 64;
        while (cap < l->len + add + 1) cap *= 2;
        char *p = realloc(l->p, cap);
        if (!p) return;
        l->p = p;
        l->cap = cap;
    }
    if (l->len) {
        memcpy(l->p + l->len, ", ", 2);
        l->len += 2;
    }
    memcpy(l->p + l->len, name, strlen(name));
    l->len += strlen(name);
    l->p[l->len] = 0;
}

/* Run a DRIVES_UNDER statement: every drive, and those plugged in. The
 * strings are "" when there is none; free both. */
static void drives_under(sqlite3_stmt *q, int64_t source_id, const char *rel, int with_deleted, char **all, char **plugged)
{
    name_list a = {NULL, 0, 0}, p = {NULL, 0, 0};
    if (q) {
        sqlite3_reset(q);
        sqlite3_bind_int64(q, 1, source_id);
        sqlite3_bind_text(q, 2, rel ? rel : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(q, 3, with_deleted);
        while (sqlite3_step(q) == SQLITE_ROW) {
            const char *label = (const char *)sqlite3_column_text(q, 0);
            names_add(&a, label);
            if (sqlite3_column_int(q, 1)) names_add(&p, label);
        }
    }
    *all = a.p ? a.p : bd_strdup("");
    *plugged = p.p ? p.p : bd_strdup("");
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
                           " COALESCE(s.last_scan_ms,0),"
                           " (SELECT COUNT(*) FROM nodes n WHERE n.source_id=s.id AND n.is_dir=0 AND n.deleted=0 AND " CUR_COPY "),"
                           " COALESCE((SELECT ms.files_gone FROM temp.missing_sources ms WHERE ms.source_id=s.id),0),"
                           " EXISTS(SELECT 1 FROM settings st WHERE st.key='wait_disk.'||s.id)"
                           " FROM sources s ORDER BY s.name COLLATE NOCASE",
                           -1, &q, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "list sources");
    sqlite3_stmt *dq = NULL;
    if (sqlite3_prepare_v2(cat->db, DRIVES_UNDER, -1, &dq, NULL) != SQLITE_OK) dq = NULL;
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
        info.files_backed_up = sqlite3_column_int64(q, 11);
        info.files_gone = sqlite3_column_int64(q, 12);
        info.wait_for_disk = sqlite3_column_int(q, 13);
        char *all = NULL, *plugged = NULL;
        drives_under(dq, info.source_id, "", 0, &all, &plugged);
        info.copies_on = all ? all : "";
        info.copies_on_plugged = plugged ? plugged : "";
        int stop = fn(ctx, &info);
        free(all);
        free(plugged);
        if (stop) break;
    }
    sqlite3_finalize(dq);
    sqlite3_finalize(q);
    return BD_OK;
}

bd_status bd_list_children(bd_catalog *cat, int64_t source_id, int64_t parent_node_id, bd_node_fn fn, void *ctx)
{
    sqlite3_stmt *q = NULL, *agg = NULL, *dq = NULL;
    bd_status s = BD_OK;
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT n.id, n.is_dir, n.name, n.rel_path, n.size, n.deleted,"
                           " COALESCE((SELECT version_no FROM versions WHERE id=n.current_version_id),0),"
                           " (SELECT COUNT(*) FROM versions WHERE node_id=n.id),"
                           " " CUR_AVAILABLE ", " ANY_AVAILABLE ", " BAD_CONNECTED ", " ANY_COPY ","
                           " COALESCE((SELECT m.label FROM copies c JOIN media m ON m.id=c.media_id"
                           "   WHERE c.version_id=n.current_version_id AND c.state='ok' LIMIT 1),"
                           "  (SELECT m.label FROM copies c JOIN versions v ON v.id=c.version_id JOIN media m ON m.id=c.media_id"
                           "   WHERE v.node_id=n.id AND c.state='ok' ORDER BY v.version_no DESC LIMIT 1)),"
                           " n.mtime_ns, COALESCE((SELECT mtime_ns FROM versions WHERE id=n.current_version_id),0),"
                           " (SELECT group_concat(label, ', ') FROM (SELECT DISTINCT m.label AS label FROM copies c"
                           "   JOIN media m ON m.id=c.media_id WHERE c.version_id=n.current_version_id AND c.state='ok'"
                           "   AND " NAMED_DRIVE " ORDER BY m.label COLLATE NOCASE)),"
                           " (SELECT group_concat(label, ', ') FROM (SELECT DISTINCT m.label AS label FROM copies c"
                           "   JOIN media m ON m.id=c.media_id JOIN temp.connected k ON k.media_id=m.id"
                           "   WHERE c.version_id=n.current_version_id AND c.state='ok'"
                           "   ORDER BY m.label COLLATE NOCASE)),"
                           " (SELECT group_concat(label, ', ') FROM (SELECT DISTINCT m.label AS label FROM copies c"
                           "   JOIN versions v ON v.id=c.version_id JOIN media m ON m.id=c.media_id"
                           "   WHERE v.node_id=n.id AND v.id<>COALESCE(n.current_version_id,0) AND c.state='ok'"
                           "   AND " NAMED_DRIVE " ORDER BY m.label COLLATE NOCASE)),"
                           " CASE WHEN n.is_dir THEN (SELECT COUNT(*) FROM nodes d WHERE d.source_id=n.source_id AND d.parent_id=n.id"
                           "   AND d.is_dir=1) ELSE 0 END,"
                           " COALESCE((SELECT o.changed+1 FROM temp.online_only o WHERE o.node_id=n.id),0)"
                           " FROM nodes n WHERE n.source_id=? AND n.parent_id IS ?"
                           " ORDER BY n.is_dir DESC, n.name COLLATE NOCASE",
                           -1, &q, NULL) != SQLITE_OK) {
        s = bd_fail_db(cat, "list folder");
        goto done;
    }
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT COUNT(*), COALESCE(SUM(" CUR_AVAILABLE "),0), COALESCE(SUM(" ANY_COPY "),0),"
                           " COALESCE(SUM(" CUR_COPY "),0),"
                           " (SELECT m.label FROM nodes n2 JOIN copies c ON c.version_id=n2.current_version_id"
                           "   JOIN media m ON m.id=c.media_id WHERE n2.source_id=?1 AND n2.is_dir=0 AND n2.deleted=0"
                           "   AND c.state='ok' AND substr(n2.rel_path,1,length(?2)+1)=?2||'/' LIMIT 1)"
                           " FROM nodes n WHERE n.source_id=?1 AND n.is_dir=0 AND n.deleted=0"
                           " AND substr(n.rel_path,1,length(?2)+1)=?2||'/'",
                           -1, &agg, NULL) != SQLITE_OK) {
        s = bd_fail_db(cat, "count folder");
        goto done;
    }
    if (sqlite3_prepare_v2(cat->db, DRIVES_UNDER, -1, &dq, NULL) != SQLITE_OK) {
        s = bd_fail_db(cat, "list the drives of a folder");
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

        info.subfolders = sqlite3_column_int64(q, 18);
        info.online_only = sqlite3_column_int(q, 19);
        info.mtime_ns = sqlite3_column_int64(q, info.is_dir ? 13 : 14);
        if (!info.mtime_ns) info.mtime_ns = sqlite3_column_int64(q, 13);
        char *label_copy = NULL, *all = NULL, *plugged = NULL;
        if (!info.is_dir) {
            info.state = file_state(deleted, sqlite3_column_int(q, 8), sqlite3_column_int(q, 9),
                                    sqlite3_column_int(q, 10), sqlite3_column_int(q, 11));
            if (info.state != BD_STATE_OFFLINE) info.offline_media_label = NULL;
            info.copies_on = sqlite3_column_type(q, 15) == SQLITE_NULL ? "" : (const char *)sqlite3_column_text(q, 15);
            info.copies_on_plugged = sqlite3_column_type(q, 16) == SQLITE_NULL ? "" : (const char *)sqlite3_column_text(q, 16);
            info.older_on = "";
            if (!info.copies_on[0] && sqlite3_column_type(q, 17) != SQLITE_NULL) info.older_on = (const char *)sqlite3_column_text(q, 17);
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
                info.files_backed_up = sqlite3_column_int64(agg, 3);
                label_copy = bd_strdup((const char *)sqlite3_column_text(agg, 4));
            }
            info.state = deleted ? BD_STATE_DELETED : folder_state(info.files_total, info.files_available, with_copy);
            if (info.state == BD_STATE_OFFLINE) info.offline_media_label = label_copy;
            drives_under(dq, source_id, info.rel_path, deleted, &all, &plugged);
            info.copies_on = all ? all : "";
            info.copies_on_plugged = plugged ? plugged : "";
            info.older_on = "";
        }
        int stop = fn(ctx, &info);
        free(label_copy);
        free(all);
        free(plugged);
        if (stop) break;
    }
done:
    sqlite3_finalize(q);
    sqlite3_finalize(agg);
    sqlite3_finalize(dq);
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
                           " COALESCE(m.location,''), v.id, m.retired_ms IS NOT NULL, c.last_full_check_ms IS NOT NULL"
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
        info.read_back = info.media_id ? sqlite3_column_int(q, 16) : 0;
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
                           " m.secondary, COALESCE(m.limit_bytes,0), m.hide_names,"
                           " (SELECT MIN(c.written_ms) FROM copies c WHERE c.media_id=m.id AND c.state='ok'"
                           "   AND c.last_full_check_ms IS NULL),"
                           " COALESCE((SELECT u.why FROM temp.uncounted u WHERE u.media_id=m.id),0),"
                           " (SELECT o.label FROM media o WHERE o.id<>m.id AND o.kind='drive' AND o.retired_ms IS NULL"
                           "   AND m.kind='drive' AND " BD_SQL_DISK("o.id") "=" BD_SQL_DISK("m.id") " ORDER BY o.label LIMIT 1)"
                           " FROM media m LEFT JOIN temp.connected k ON k.media_id=m.id LEFT JOIN media_hardware h ON h.media_id=m.id"
                           " ORDER BY m.label COLLATE NOCASE",
                           -1, &q, NULL) != SQLITE_OK)
        return bd_fail_db(cat, "list drives");
    int check_days = bd_option_get(cat, "check_days");
    /* A copy never read back since it was written is due sooner: whatever
     * went wrong in the writing shows up while the file is still on the PC. */
    int unread_days = check_days < BD_UNREAD_CHECK_DAYS ? check_days : BD_UNREAD_CHECK_DAYS;
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
        info.uncounted = sqlite3_column_int(q, 41);
        info.same_disk_as = sqlite3_column_type(q, 42) == SQLITE_NULL ? "" : (const char *)sqlite3_column_text(q, 42);
        int64_t oldest_unread = col_num(q, 40);
        info.oldest_unread_ms = oldest_unread > 0 ? oldest_unread : 0;
        info.check_due = check_days > 0 &&
                         ((info.oldest_check_ms > 0 && now - info.oldest_check_ms >= (int64_t)check_days * 24 * 3600 * 1000) ||
                          (oldest_unread > 0 && now - oldest_unread >= (int64_t)unread_days * 24 * 3600 * 1000));
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

/* One code point's upper case, for the letters of the European scripts
 * (Latin, Greek, Cyrillic, Armenian); others are left as they are. The
 * same mapping as Windows' own for these, which bd_path_key uses. */
static uint32_t upper_cp(uint32_t c)
{
    if (c < 0x80) return c >= 'a' && c <= 'z' ? c - 32 : c;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 32;
    if (c == 0xFF) return 0x178;
    if (c == 0x131) return 'I';
    if ((c >= 0x100 && c <= 0x12F) || (c >= 0x132 && c <= 0x137) || (c >= 0x14A && c <= 0x177) ||
        (c >= 0x1DE && c <= 0x1EF) || (c >= 0x1F8 && c <= 0x21F) || (c >= 0x222 && c <= 0x233) ||
        (c >= 0x460 && c <= 0x481) || (c >= 0x48A && c <= 0x4BF) || (c >= 0x4D0 && c <= 0x52F) ||
        (c >= 0x1E00 && c <= 0x1E95) || (c >= 0x1EA0 && c <= 0x1EFF))
        return c & ~1u;
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E) || (c >= 0x1CD && c <= 0x1DC) ||
        (c >= 0x4C1 && c <= 0x4CE))
        return c & 1u ? c : c - 1;
    if (c >= 0x3B1 && c <= 0x3CB && c != 0x3C2) return c - 32;
    if (c == 0x3C2) return 0x3A3;
    if (c == 0x3AC) return 0x386;
    if (c >= 0x3AD && c <= 0x3AF) return c - 37;
    if (c == 0x3CC) return 0x38C;
    if (c == 0x3CD || c == 0x3CE) return c - 63;
    if (c >= 0x430 && c <= 0x44F) return c - 32;
    if (c >= 0x450 && c <= 0x45F) return c - 80;
    if (c == 0x4CF) return 0x4C0;
    if (c >= 0x561 && c <= 0x586) return c - 48;
    if (c >= 0xFF41 && c <= 0xFF5A) return c - 32;
    return c;
}

/* The code point at *p (UTF-8), moving *p past it; a byte that does not
 * start one is taken as itself. */
static uint32_t next_cp(const unsigned char **p)
{
    const unsigned char *s = *p;
    uint32_t c = s[0];
    int n = c >= 0xF0 && c < 0xF8 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (c >= 0xF8) n = 0;
    for (int i = 1; i <= n; i++)
        if ((s[i] & 0xC0) != 0x80) n = 0;
    if (n == 0) {
        *p = s + 1;
        return c;
    }
    c &= 0x3F >> n;
    for (int i = 1; i <= n; i++) c = (c << 6) | (s[i] & 0x3F);
    *p = s + n + 1;
    return c;
}

static size_t put_cp(char *out, uint32_t c)
{
    if (c < 0x80) { out[0] = (char)c; return 1; }
    if (c < 0x800) { out[0] = (char)(0xC0 | (c >> 6)); out[1] = (char)(0x80 | (c & 0x3F)); return 2; }
    if (c < 0x10000) {
        out[0] = (char)(0xE0 | (c >> 12));
        out[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        out[2] = (char)(0x80 | (c & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (c >> 18));
    out[1] = (char)(0x80 | ((c >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((c >> 6) & 0x3F));
    out[3] = (char)(0x80 | (c & 0x3F));
    return 4;
}

/* s with letter case folded (to upper case) and '\' as '/', into buf when
 * it fits (else malloc'd: *heap set). On Windows a name with letters past
 * ASCII is folded by Windows itself, as paths are (bd_path_key). */
static char *fold_text(const char *s, char *buf, size_t cap, int *heap)
{
    *heap = 0;
    size_t len = strlen(s);
    int ascii = 1;
    for (size_t i = 0; i < len && ascii; i++) ascii = (unsigned char)s[i] < 0x80;
    char *out = NULL;
#ifdef _WIN32
    if (!ascii) {
        out = bd_fold_case(s);
        if (out) {
            *heap = 1;
            for (char *p = out; *p; p++) if (*p == '\\') *p = '/';
            return out;
        }
    }
#endif
    /* Upper case is at most one byte longer per code point here (U+0131
     * to 'I' is shorter, U+00FF to U+0178 the same). */
    size_t need = len * 2 + 1;
    out = need <= cap ? buf : malloc(need);
    if (!out) return NULL;
    if (out != buf) *heap = 1;
    size_t j = 0;
    if (ascii) {
        for (size_t i = 0; i < len; i++) {
            char c = s[i];
            out[j++] = c == '\\' ? '/' : c >= 'a' && c <= 'z' ? (char)(c - 32) : c;
        }
    } else {
        const unsigned char *p = (const unsigned char *)s;
        while (*p) {
            uint32_t c = next_cp(&p);
            j += put_cp(out + j, c == '\\' ? '/' : upper_cp(c));
        }
    }
    out[j] = '\0';
    return out;
}

/* Does the folded text t match the folded pattern g, where '*' is any
 * run of characters and '?' one character (not one byte)? */
static int glob_match(const char *g, const char *t)
{
    const char *star = NULL, *resume = NULL;
    while (*t) {
        if (*g == '*') {
            star = ++g;
            resume = t;
        } else if (*g == '?' || (*g && *g == *t)) {
            if (*g == '?') {
                const unsigned char *u = (const unsigned char *)t;
                next_cp(&u);
                t = (const char *)u;
            } else {
                t++;
            }
            g++;
        } else if (star) {
            g = star;
            const unsigned char *u = (const unsigned char *)resume;
            next_cp(&u);
            resume = (const char *)u;
            t = resume;
        } else {
            return 0;
        }
    }
    while (*g == '*') g++;
    return *g == '\0';
}

/* The words searched for, folded; parsed once per search (auxdata). */
typedef struct {
    int n;
    char *word[MAX_WORDS];
    int wild[MAX_WORDS]; /* has '*' or '?' */
    int path[MAX_WORDS]; /* a wildcard word with '/': matched against the end of the path */
} search_words;

static void words_free(void *p)
{
    search_words *w = p;
    if (!w) return;
    for (int i = 0; i < w->n; i++) free(w->word[i]);
    free(w);
}

static search_words *words_parse(const char *text)
{
    search_words *w = calloc(1, sizeof(*w));
    if (!w) return NULL;
    for (const char *p = text ? text : ""; *p && w->n < MAX_WORDS;) {
        while (*p == ' ' || *p == '\t') p++;
        const char *start = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (p == start) continue;
        char word[256];
        size_t n = (size_t)(p - start) < sizeof(word) - 1 ? (size_t)(p - start) : sizeof(word) - 1;
        memcpy(word, start, n);
        word[n] = '\0';
        int heap = 0;
        char fb[512];
        char *f = fold_text(word, fb, sizeof(fb), &heap);
        char *keep = f ? bd_strdup(f) : NULL;
        if (heap) free(f);
        if (!keep) { words_free(w); return NULL; }
        int i = w->n++;
        w->word[i] = keep;
        w->wild[i] = strchr(keep, '*') || strchr(keep, '?');
        w->path[i] = w->wild[i] && strchr(keep, '/');
        /* "Taxes/report*.pdf" matches the end of a path, after a '/'. */
        if (w->path[i] && keep[0] == '/') memmove(keep, keep + 1, strlen(keep));
    }
    return w;
}

/* bd_match(rel_path, name, text): 0 when a word of text is not found, 1
 * when every word is in the path, 2 when every word is in the name. A word
 * with '*' or '?' matches the whole name (or, with a '/', the end of the
 * path); any other word is found anywhere in it. Letter case is folded as
 * fold_text does. */
static void match_fn(sqlite3_context *ctx, int argc, sqlite3_value **argv)
{
    (void)argc;
    search_words *w = sqlite3_get_auxdata(ctx, 2);
    if (!w) {
        w = words_parse((const char *)sqlite3_value_text(argv[2]));
        if (!w) { sqlite3_result_error_nomem(ctx); return; }
        sqlite3_set_auxdata(ctx, 2, w, words_free);
        w = sqlite3_get_auxdata(ctx, 2);
        if (!w) { sqlite3_result_int(ctx, 0); return; } /* SQLite let go of it: no words */
    }
    const char *rel = (const char *)sqlite3_value_text(argv[0]);
    const char *name = (const char *)sqlite3_value_text(argv[1]);
    if (!rel || !name || w->n == 0) { sqlite3_result_int(ctx, 0); return; }
    char pb[1024], nb[512];
    int ph = 0, nh = 0;
    char *fp = fold_text(rel, pb, sizeof(pb), &ph);
    char *fname = fold_text(name, nb, sizeof(nb), &nh);
    int in_path = fp && fname, in_name = in_path;
    for (int i = 0; in_path && i < w->n; i++) {
        const char *g = w->word[i];
        int p, n;
        if (!w->wild[i]) {
            p = strstr(fp, g) != NULL;
            n = p && strstr(fname, g) != NULL;
        } else if (!w->path[i]) {
            n = p = glob_match(g, fname);
        } else {
            n = 0;
            p = glob_match(g, fp);
            for (const char *s = fp; !p && (s = strchr(s, '/')) != NULL;) p = glob_match(g, ++s);
        }
        in_path = p;
        in_name = in_name && n;
    }
    if (ph) free(fp);
    if (nh) free(fname);
    sqlite3_result_int(ctx, !in_path ? 0 : in_name ? 2 : 1);
}

/* Made once on each database connection; SQLite keeps the mark. */
static int match_ready(bd_catalog *cat)
{
    static const char mark[] = "bd_match";
    if (sqlite3_get_clientdata(cat->db, mark)) return 1;
    if (sqlite3_create_function_v2(cat->db, "bd_match", 3, SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL, match_fn, NULL, NULL,
                                   NULL) != SQLITE_OK)
        return 0;
    sqlite3_set_clientdata(cat->db, mark, (void *)mark, NULL);
    return 1;
}

bd_status bd_search(bd_catalog *cat, const char *text, int limit, bd_search_fn fn, void *ctx)
{
    int any = 0;
    for (const char *p = text ? text : ""; *p && !any; p++) any = *p != ' ' && *p != '\t';
    if (!any) return BD_OK;
    if (limit <= 0) limit = 500;

    /* The whole search runs as one step of the database: a job on another
     * thread (a scan) waits for it between its own steps, so it sees the
     * database as it stands and a save cannot start halfway. */
    sqlite3_mutex *mx = sqlite3_db_mutex(cat->db);
    if (mx) sqlite3_mutex_enter(mx);
    sqlite3_stmt *q = NULL, *dir = NULL;
    bd_status st = BD_OK;
    if (!match_ready(cat)) {
        st = bd_fail_db(cat, "search");
        goto done;
    }
    /* The drives named are those of the version a restore brings back. */
    if (sqlite3_prepare_v2(
            cat->db,
            "SELECT n.id, n.source_id, s.name, n.rel_path, n.name, n.is_dir, n.size, n.deleted,"
            " " CUR_AVAILABLE ", " ANY_AVAILABLE ", " BAD_CONNECTED ", " ANY_COPY ","
            " (SELECT group_concat(w, '; ') FROM (SELECT DISTINCT " WHERE_TEXT " AS w FROM copies c JOIN media m ON m.id=c.media_id"
            "   WHERE c.version_id=" BD_SQL_RESTORE_VERSION("n") " AND c.state='ok' AND " NAMED_DRIVE " ORDER BY 1)),"
            " bd_match(n.rel_path, n.name, ?1) AS mt, n.mtime_ns"
            " FROM nodes n JOIN sources s ON s.id=n.source_id WHERE mt>0"
            " ORDER BY mt DESC, n.deleted, n.is_dir DESC, s.name COLLATE NOCASE, n.rel_path COLLATE NOCASE LIMIT ?2",
            -1, &q, NULL) != SQLITE_OK) {
        st = bd_fail_db(cat, "search");
        goto done;
    }
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT COUNT(*), COALESCE(SUM(" CUR_AVAILABLE "),0), COALESCE(SUM(" ANY_COPY "),0),"
                           " (SELECT group_concat(w, '; ') FROM (SELECT DISTINCT " WHERE_TEXT " AS w"
                           "   FROM nodes n JOIN copies c ON c.version_id=" BD_SQL_RESTORE_VERSION("n")
                           "   JOIN media m ON m.id=c.media_id"
                           "   WHERE n.source_id=?1 AND n.is_dir=0 AND n.deleted=0 AND c.state='ok' AND " NAMED_DRIVE
                           "   AND substr(n.rel_path,1,length(?2)+1)=?2||'/' ORDER BY 1))"
                           " FROM nodes n WHERE n.source_id=?1 AND n.is_dir=0 AND n.deleted=0"
                           " AND substr(n.rel_path,1,length(?2)+1)=?2||'/'",
                           -1, &dir, NULL) != SQLITE_OK) {
        st = bd_fail_db(cat, "search");
        goto done;
    }
    sqlite3_bind_text(q, 1, text, -1, SQLITE_STATIC);
    sqlite3_bind_int(q, 2, limit);
    int rc;
    while ((rc = sqlite3_step(q)) == SQLITE_ROW) {
        bd_search_info info;
        memset(&info, 0, sizeof(info));
        info.node_id = sqlite3_column_int64(q, 0);
        info.source_id = sqlite3_column_int64(q, 1);
        info.source_name = (const char *)sqlite3_column_text(q, 2);
        info.rel_path = (const char *)sqlite3_column_text(q, 3);
        info.name = (const char *)sqlite3_column_text(q, 4);
        info.is_dir = sqlite3_column_int(q, 5);
        info.mtime_ns = sqlite3_column_int64(q, 14);
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
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) st = bd_fail_db(cat, "search");
done:
    sqlite3_finalize(q);
    sqlite3_finalize(dir);
    if (mx) sqlite3_mutex_leave(mx);
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
                           "   WHERE c.version_id=v.id AND c.state='ok' AND m.retired_ms IS NULL ORDER BY m.label)),"
                           " EXISTS(SELECT 1 FROM copies c JOIN temp.connected k ON k.media_id=c.media_id"
                           "   WHERE c.version_id=v.id AND c.state='ok' AND " BD_SQL_USABLE("c") "),"
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
