/* Moving to a new PC: the catalog is copied from a drive with
 * bd_catalog_recover (catalog.c), then protected folders whose paths do
 * not exist here (another user name, another drive letter) are pointed at
 * where they are now. Only the path changes: the folder keeps its name on
 * the drives, its files keep their versions and copies, and the next scan
 * matches files by their path inside the folder as usual. */
#include "internal.h"

#include <stdlib.h>
#include <string.h>

bd_status bd_source_relocate(bd_catalog *cat, int64_t source_id, const char *new_path)
{
    bd_stat_t st;
    if (!new_path || bd_stat(new_path, &st) != 0 || !st.is_dir)
        return bd_fail(cat, BD_ERR_NOT_FOUND, "%s is not a folder", new_path ? new_path : "");

    char *path = bd_strdup(new_path);
    if (!path) return BD_ERR_NOMEM;
    size_t len = strlen(path);
    while (len > 1 && (path[len - 1] == '/' || path[len - 1] == '\\') && !(len == 3 && path[1] == ':')) path[--len] = '\0';

    bd_status s = BD_OK;
    sqlite3_stmt *q;
    int found = 0;
    if (sqlite3_prepare_v2(cat->db, "SELECT id FROM sources WHERE id=? OR path=?", -1, &q, NULL) != SQLITE_OK) {
        free(path);
        return bd_fail_db(cat, "source lookup");
    }
    sqlite3_bind_int64(q, 1, source_id);
    sqlite3_bind_text(q, 2, path, -1, SQLITE_STATIC);
    while (s == BD_OK && sqlite3_step(q) == SQLITE_ROW) {
        if (sqlite3_column_int64(q, 0) == source_id) found = 1;
        else s = bd_fail(cat, BD_ERR_EXISTS, "%s is already protected", path);
    }
    sqlite3_finalize(q);
    if (s == BD_OK && !found) s = bd_fail(cat, BD_ERR_NOT_FOUND, "no such protected folder");

    if (s == BD_OK) {
        sqlite3_stmt *u;
        if (sqlite3_prepare_v2(cat->db, "UPDATE sources SET path=? WHERE id=?", -1, &u, NULL) != SQLITE_OK) {
            s = bd_fail_db(cat, "change the folder");
        } else {
            sqlite3_bind_text(u, 1, path, -1, SQLITE_STATIC);
            sqlite3_bind_int64(u, 2, source_id);
            if (sqlite3_step(u) != SQLITE_DONE) s = bd_fail_db(cat, "change the folder");
            sqlite3_finalize(u);
        }
    }
    free(path);
    return s;
}
