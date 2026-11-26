/* End-to-end test of the core: scan, back up, go offline, change a file,
 * keep the old version, detect a missing copy, restore. */
#include "brodalf.h"
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#endif

static int failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                        \
        }                                                                      \
    } while (0)

#define REQUIRE_OK(expr, cat)                                                  \
    do {                                                                       \
        bd_status _s = (expr);                                                 \
        if (_s != BD_OK) {                                                     \
            fprintf(stderr, "%s:%d: %s -> %s: %s\n", __FILE__, __LINE__, #expr, \
                    bd_status_name(_s), (cat) ? bd_catalog_error(cat) : bd_open_error()); \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

static char base[1024];

static char *at(const char *rel)
{
    static char buf[8][2048];
    static int i = 0;
    i = (i + 1) % 8;
    snprintf(buf[i], sizeof(buf[i]), "%s/%s", base, rel);
    return buf[i];
}

static void write_file(const char *path, const char *text)
{
    char *dir = bd_strdup(path);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = '\0'; bd_mkdirs(dir); }
    free(dir);
    FILE *f = bd_fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); exit(1); }
    fputs(text, f);
    fclose(f);
}

static int file_equals(const char *path, const char *text)
{
    FILE *f = bd_fopen(path, "rb");
    if (!f) return 0;
    char buf[256] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    return n == strlen(text) && memcmp(buf, text, n) == 0;
}

/* The whole file, however big, is exactly text. */
static int file_equals_long(const char *path, const char *text)
{
    FILE *f = bd_fopen(path, "rb");
    if (!f) return 0;
    size_t want = strlen(text), got = 0;
    char buf[8192];
    int same = 1;
    for (size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0; got += n)
        if (got + n > want || memcmp(buf, text + got, n) != 0) { same = 0; break; }
    fclose(f);
    return same && got == want;
}

static int exists(const char *path)
{
    bd_stat_t st;
    return bd_stat(path, &st) == 0;
}

typedef struct {
    const char *name;
    bd_node_state state;
    int version_no;
    char label[128];
    int64_t node_id;
    int found;
} find_ctx;

static int find_child(void *ctx, const bd_node_info *n)
{
    find_ctx *f = ctx;
    if (strcmp(n->name, f->name) != 0) return 0;
    f->state = n->state;
    f->version_no = n->version_no;
    f->node_id = n->node_id;
    snprintf(f->label, sizeof(f->label), "%s", n->offline_media_label ? n->offline_media_label : "");
    f->found = 1;
    return 1;
}

static find_ctx lookup(bd_catalog *cat, int64_t source, int64_t parent, const char *name)
{
    find_ctx f;
    memset(&f, 0, sizeof(f));
    f.name = name;
    bd_list_children(cat, source, parent, find_child, &f);
    return f;
}

static int count_copies(void *ctx, const bd_copy_info *c)
{
    if (c->media_id && strcmp(c->copy_state, "ok") == 0) (*(int *)ctx)++;
    return 0;
}

typedef struct { bd_node_state state; int64_t total, available; char label[128]; } source_totals;

static int get_totals(void *ctx, const bd_source_info *info)
{
    source_totals *t = ctx;
    t->state = info->state;
    t->total = info->files_total;
    t->available = info->files_available;
    snprintf(t->label, sizeof(t->label), "%s", info->offline_media_label ? info->offline_media_label : "");
    return 1;
}

static source_totals totals(bd_catalog *cat)
{
    source_totals t;
    memset(&t, 0, sizeof(t));
    bd_list_sources(cat, get_totals, &t);
    return t;
}

static void quiet(void *ctx, const char *msg)
{
    (void)ctx;
    (void)msg;
}

typedef struct {
    char location[128];
    int has_hw;
    bd_drive_hw hw;
    int64_t want;
} media_snapshot_t;

static int snap_media(void *ctx, const bd_media_info *m)
{
    media_snapshot_t *s = ctx;
    if (m->media_id != s->want) return 0;
    snprintf(s->location, sizeof(s->location), "%s", m->location ? m->location : "(null)");
    s->has_hw = m->hw != NULL;
    if (m->hw) s->hw = *m->hw;
    return 1;
}

static media_snapshot_t media_of(bd_catalog *cat, int64_t id)
{
    media_snapshot_t s;
    memset(&s, 0, sizeof(s));
    s.want = id;
    bd_list_media(cat, snap_media, &s);
    return s;
}

static void count_log(void *ctx, const char *msg)
{
    if (strstr(msg, "different disk")) (*(int *)ctx)++;
}

typedef struct { int n; int64_t copies_sum; int older; int unknown; } risk_count;

static int count_risk(void *ctx, const bd_risk_info *r)
{
    risk_count *c = ctx;
    c->n++;
    c->copies_sum += r->copies;
    c->older += r->older_copies;
    c->unknown += r->unknown_place;
    return 0;
}

typedef struct { char labels[8][64]; int64_t files[8]; int n; } help_list;

static int collect_help(void *ctx, const bd_risk_help *h)
{
    help_list *l = ctx;
    if (l->n < 8) {
        snprintf(l->labels[l->n], 64, "%s", h->label);
        l->files[l->n++] = h->files;
    }
    return 0;
}

static bd_risk_stats risk_of(bd_catalog *cat, risk_count *c)
{
    bd_risk_stats st;
    if (c) memset(c, 0, sizeof(*c));
    CHECK(bd_list_at_risk(cat, 0, c ? count_risk : NULL, c, &st) == BD_OK);
    return st;
}

static help_list help_of(bd_catalog *cat)
{
    help_list l;
    memset(&l, 0, sizeof(l));
    CHECK(bd_list_risk_help(cat, collect_help, &l) == BD_OK);
    return l;
}

/* Copies, places and which drive to plug in next. */
static void test_risk(void)
{
    write_file(at("risk/src/one.txt"), "1");
    write_file(at("risk/src/two.txt"), "22");
    write_file(at("risk/src/sub/three.txt"), "333");
    write_file(at("risk/other/four.txt"), "4444");
    bd_mkdirs(at("risk/A"));
    bd_mkdirs(at("risk/B"));
    bd_mkdirs(at("risk/C"));

    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("risk/risk.brodalf"), &cat), NULL);
    int64_t src, other;
    REQUIRE_OK(bd_source_add(cat, at("risk/src"), &src), cat);
    REQUIRE_OK(bd_source_add(cat, at("risk/other"), &other), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);

    bd_target t;
    REQUIRE_OK(bd_target_get(cat, &t), cat);
    CHECK(t.copies == 2 && t.places == 2);

    risk_count rc;
    bd_risk_stats st = risk_of(cat, &rc);
    CHECK(st.files_total == 4 && st.files_at_risk == 4 && st.files_no_copy == 4 && st.bytes_at_risk == 10);
    CHECK(rc.n == 4 && rc.copies_sum == 0);
    CHECK(help_of(cat).n == 0);

    /* One drive, no place set: one copy each. */
    int64_t a, b, c;
    bd_backup_stats bs;
    REQUIRE_OK(bd_media_init(cat, at("risk/A"), "Drive A", 0, &a), cat);
    REQUIRE_OK(bd_backup(cat, a, 0, &bs, quiet, NULL), cat);
    st = risk_of(cat, &rc);
    CHECK(st.files_at_risk == 4 && st.files_no_copy == 0 && rc.copies_sum == 4 && rc.unknown == 4);

    /* A second drive with no place set helps with copies... */
    REQUIRE_OK(bd_media_init(cat, at("risk/B"), "Drive B", 0, &b), cat);
    help_list h = help_of(cat);
    CHECK(h.n == 1 && strcmp(h.labels[0], "Drive B") == 0 && h.files[0] == 4);
    REQUIRE_OK(bd_backup(cat, b, 0, &bs, quiet, NULL), cat);
    /* ...but both are in the same unknown place. */
    st = risk_of(cat, &rc);
    CHECK(st.files_at_risk == 4 && rc.copies_sum == 8);
    CHECK(help_of(cat).n == 0);

    /* Saying where B is kept makes two places. */
    REQUIRE_OK(bd_media_set_location(cat, b, "Box B"), cat);
    st = risk_of(cat, NULL);
    CHECK(st.files_total == 4 && st.files_at_risk == 0);

    /* Raise the target; a third drive in B's place (different case and
     * spacing) helps with copies. */
    t.copies = 3;
    t.places = 2;
    REQUIRE_OK(bd_target_set(cat, &t), cat);
    CHECK(risk_of(cat, NULL).files_at_risk == 4);
    REQUIRE_OK(bd_media_init(cat, at("risk/C"), "Drive C", 0, &c), cat);
    REQUIRE_OK(bd_media_set_location(cat, c, "  box b "), cat);
    h = help_of(cat);
    CHECK(h.n == 1 && strcmp(h.labels[0], "Drive C") == 0 && h.files[0] == 4);

    /* Only places short: C (same place as B) no longer helps; A-place does. */
    t.copies = 3;
    t.places = 3;
    REQUIRE_OK(bd_target_set(cat, &t), cat);
    REQUIRE_OK(bd_backup(cat, c, 0, &bs, quiet, NULL), cat);
    st = risk_of(cat, NULL);
    CHECK(st.files_at_risk == 4);
    CHECK(help_of(cat).n == 0);

    t.copies = 2;
    t.places = 3;
    CHECK(bd_target_set(cat, &t) == BD_ERR_INVALID);
    t.copies = 0;
    CHECK(bd_target_set(cat, &t) == BD_ERR_INVALID);
    t.copies = 2;
    t.places = 2;
    REQUIRE_OK(bd_target_set(cat, &t), cat);
    CHECK(risk_of(cat, NULL).files_at_risk == 0);

    /* A changed file has copies of its old version only. */
    write_file(at("risk/src/two.txt"), "changed");
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    st = risk_of(cat, &rc);
    CHECK(st.files_at_risk == 1 && st.files_no_copy == 1 && rc.older == 1 && st.bytes_at_risk == 7);
    bd_risk_stats one;
    REQUIRE_OK(bd_list_at_risk(cat, other, NULL, NULL, &one), cat);
    CHECK(one.files_total == 1 && one.files_at_risk == 0);
    h = help_of(cat);
    CHECK(h.n == 3 && h.files[0] == 1);

    /* Once the new version is backed up anywhere, the file may still be
     * short of the target, but it is no longer "changed since its last
     * backup", however many copies of older versions are around. */
    REQUIRE_OK(bd_backup(cat, a, 0, &bs, quiet, NULL), cat);
    st = risk_of(cat, &rc);
    CHECK(st.files_at_risk == 1 && st.files_no_copy == 0 && rc.n == 1 && rc.copies_sum == 1 && rc.older == 0);

    bd_catalog_close(cat);
}

typedef struct { int n; char first[256]; char first_where[256]; int first_dir; bd_node_state first_state; } search_result;

static int collect_search(void *ctx, const bd_search_info *r)
{
    search_result *sr = ctx;
    if (sr->n++ == 0) {
        snprintf(sr->first, sizeof(sr->first), "%s", r->rel_path);
        snprintf(sr->first_where, sizeof(sr->first_where), "%s", r->where);
        sr->first_dir = r->is_dir;
        sr->first_state = r->state;
    }
    return 0;
}

static search_result search(bd_catalog *cat, const char *text)
{
    search_result sr;
    memset(&sr, 0, sizeof(sr));
    CHECK(bd_search(cat, text, 0, collect_search, &sr) == BD_OK);
    return sr;
}

typedef struct { int64_t id; int due; int64_t oldest; } due_snap;

static int snap_due(void *ctx, const bd_media_info *m)
{
    due_snap *d = ctx;
    if (m->media_id == d->id) { d->due = m->check_due; d->oldest = m->oldest_check_ms; }
    return 0;
}

static due_snap due_of(bd_catalog *cat, int64_t id)
{
    due_snap d = {id, -1, -1};
    bd_list_media(cat, snap_due, &d);
    return d;
}

/* Search by name, options, and check reminders. */
static void test_search(void)
{
    write_file(at("find/src/Photos/2024/party.jpg"), "party");
    write_file(at("find/src/Photos/beach.jpg"), "beach");
    write_file(at("find/src/Docs/tax_2024.pdf"), "tax");
    write_file(at("find/src/Docs/100%.txt"), "full");
    bd_mkdirs(at("find/A"));

    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("find/find.brodalf"), &cat), NULL);
    int64_t src, a;
    REQUIRE_OK(bd_source_add(cat, at("find/src"), &src), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);

    search_result r = search(cat, "party");
    CHECK(r.n == 1 && strcmp(r.first, "Photos/2024/party.jpg") == 0 && r.first_where[0] == '\0' && r.first_state == BD_STATE_NO_COPY);

    REQUIRE_OK(bd_media_init(cat, at("find/A"), "Drive A", 0, &a), cat);
    REQUIRE_OK(bd_media_set_location(cat, a, "Box A"), cat);
    bd_backup_stats bs;
    REQUIRE_OK(bd_backup(cat, a, 0, &bs, quiet, NULL), cat);
    bd_media_disconnect(cat, a);

    r = search(cat, "PARTY");
    CHECK(r.n == 1 && strcmp(r.first_where, "Drive A (Box A)") == 0 && r.first_state == BD_STATE_OFFLINE);
    r = search(cat, "2024");
    CHECK(r.n == 3 && r.first_dir && strcmp(r.first, "Photos/2024") == 0 && strcmp(r.first_where, "Drive A (Box A)") == 0);
    CHECK(r.first_state == BD_STATE_OFFLINE);
    CHECK(search(cat, "2024 jpg").n == 1);
    r = search(cat, "100%");
    CHECK(r.n == 1 && strcmp(r.first, "Docs/100%.txt") == 0);
    r = search(cat, "_");
    CHECK(r.n == 1 && strcmp(r.first, "Docs/tax_2024.pdf") == 0);
    CHECK(search(cat, "photos\\2024").n == 2);
    CHECK(search(cat, "   ").n == 0);
    CHECK(search(cat, "nothing-like-this").n == 0);

    /* Options. */
    CHECK(bd_option_get(cat, "check_days") == 180 && bd_option_get(cat, "verify_days") == 30);
    CHECK(bd_option_get(cat, "no_such_option") == -1);
    /* The options that started things by themselves are gone. */
    CHECK(bd_option_get(cat, "auto_backup") == -1);
    CHECK(bd_option_get(cat, "guard_percent") == -1);
    CHECK(bd_option_set(cat, "auto_backup", 1) == BD_ERR_INVALID);
    REQUIRE_OK(bd_option_set(cat, "check_days", 30), cat);
    CHECK(bd_option_get(cat, "check_days") == 30);
    CHECK(bd_option_set(cat, "check_days", -1) == BD_ERR_INVALID);
    CHECK(bd_option_set(cat, "nope", 1) == BD_ERR_INVALID);

    /* Fresh copies are not due; 40-day-old unchecked ones are; a full
     * check makes them fresh again. */
    due_snap d = due_of(cat, a);
    CHECK(d.due == 0 && d.oldest > 0);
    int64_t old = bd_now_ms() - (int64_t)40 * 24 * 3600 * 1000;
    char sql[128];
    snprintf(sql, sizeof(sql), "UPDATE copies SET written_ms=%lld", (long long)old);
    CHECK(sqlite3_exec(cat->db, sql, NULL, NULL, NULL) == SQLITE_OK);
    d = due_of(cat, a);
    CHECK(d.due == 1 && d.oldest == old);
    REQUIRE_OK(bd_option_set(cat, "check_days", 0), cat);
    CHECK(due_of(cat, a).due == 0);
    REQUIRE_OK(bd_option_set(cat, "check_days", 30), cat);
    int64_t again;
    bd_check_stats cs;
    REQUIRE_OK(bd_media_connect(cat, at("find/A"), &again, quiet, NULL), cat);
    REQUIRE_OK(bd_media_check(cat, a, &cs, quiet, NULL), cat);
    CHECK(due_of(cat, a).due == 0); /* every check reads every copy back */

    bd_catalog_close(cat);
}

static int count_hits(bd_catalog *cat, const char *text)
{
    return search(cat, text).n;
}

static int source_bytes(void *ctx, const bd_source_info *info)
{
    *(int64_t *)ctx += info->bytes_total;
    return 0;
}

typedef struct { char labels[8][64]; int64_t files[8]; int connected[8]; int n; } plan_list;

static int collect_plan(void *ctx, const bd_restore_step *st)
{
    plan_list *p = ctx;
    if (p->n < 8) {
        snprintf(p->labels[p->n], 64, "%s", st->label);
        p->files[p->n] = st->files;
        p->connected[p->n] = st->connected;
        p->n++;
    }
    return 0;
}

/* Skip lists, full drives, the keep rule for old versions, restore plans. */
static void test_drives_and_versions(void)
{
    const char *def = bd_skip_list_default();
    CHECK(bd_skip_match(def, "node_modules/pkg/index.js", 0));
    CHECK(bd_skip_match(def, "code/app/node_modules", 1));
    CHECK(!bd_skip_match(def, "code/node_modules", 0)); /* a file with that name */
    CHECK(bd_skip_match(def, "Report.TMP", 0) && bd_skip_match(def, "Docs/~$letter.docx", 0));
    CHECK(!bd_skip_match(def, "Docs/letter.docx", 0) && !bd_skip_match(def, "notes.txt", 0));
    CHECK(bd_skip_match("# mine\n  Photos/Exports/  \r\n*.bak\n", "Photos/Exports/a.jpg", 0));
    CHECK(!bd_skip_match("Photos/Exports/\n", "Photos/a.jpg", 0));
    CHECK(bd_skip_match("*.bak\n", "x/y/file.BAK", 0) && !bd_skip_match("", "file.bak", 0));

    /* big.bin is seen first, so backups come to it first. */
    write_file(at("span/src/big.bin"), "0123456789012345678901234567890123456789");
    bd_mkdirs(at("span/D"));
    bd_mkdirs(at("span/E"));
    bd_mkdirs(at("span/out"));
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("span/span.brodalf"), &cat), NULL);
    int64_t src, d, e;
    REQUIRE_OK(bd_source_add(cat, at("span/src"), &src), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    write_file(at("span/src/keep.txt"), "keep");
    write_file(at("span/src/scratch.tmp"), "tmp");
    write_file(at("span/src/node_modules/pkg/index.js"), "js");
    write_file(at("span/src/v.txt"), "1");
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.skipped == 2 && ss.files_seen == 3);
    CHECK(count_hits(cat, "index") == 0 && count_hits(cat, "scratch") == 0 && count_hits(cat, "keep") == 1);

    /* An empty list skips nothing; a new pattern drops what it now matches. */
    REQUIRE_OK(bd_skip_list_set(cat, ""), cat);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.skipped == 0 && count_hits(cat, "index.js") == 1);
    REQUIRE_OK(bd_skip_list_set(cat, "*.js\nnode_modules/\n*.tmp\n"), cat);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    /* Newly left out: marked deleted, for the user to acknowledge or forget. */
    CHECK(ss.files_deleted == 2);
    {
        int64_t n = 0;
        REQUIRE_OK(bd_list_deleted(cat, 0, 1, NULL, NULL, &n), cat);
        CHECK(n == 2);
        REQUIRE_OK(bd_deleted_forget(cat, 0, NULL), cat);
    }
    CHECK(count_hits(cat, "index") == 0 && count_hits(cat, "scratch") == 0);
    char *list = bd_skip_list_get(cat);
    CHECK(list && strcmp(list, "*.js\nnode_modules/\n*.tmp\n") == 0);
    free(list);
    REQUIRE_OK(bd_skip_list_set(cat, NULL), cat);
    list = bd_skip_list_get(cat);
    CHECK(list && strcmp(list, def) == 0);
    free(list);

    /* What the folder needs on a drive: keep (4) + big (40) + v (1). */
    int64_t need = 0;
    CHECK(bd_list_sources(cat, source_bytes, &need) == BD_OK && need == 45);

    /* A nearly full drive takes what fits and leaves the rest. */
    REQUIRE_OK(bd_media_init(cat, at("span/D"), "Drive D", 0, &d), cat);
    bd_backup_stats bs;
    bd_test_free_bytes = 20;
    REQUIRE_OK(bd_backup(cat, d, 0, &bs, quiet, NULL), cat);
    bd_test_free_bytes = -1;
    CHECK(bs.files_copied == 2 && bs.files_no_room == 1 && bs.bytes_no_room == 40);

    /* Another drive takes what it has room for. */
    REQUIRE_OK(bd_media_init(cat, at("span/E"), "Drive E", 0, &e), cat);
    REQUIRE_OK(bd_media_set_location(cat, e, "Box 7"), cat);
    bd_test_free_bytes = 40;
    REQUIRE_OK(bd_backup(cat, e, 0, &bs, quiet, NULL), cat);
    bd_test_free_bytes = -1;
    CHECK(bs.files_copied == 1 && bs.bytes_copied == 40 && bs.files_no_room == 2);
    {
        bd_set_progress sp[4];
        int sets = 0;
        REQUIRE_OK(bd_backup_sets(cat, sp, 4, &sets), cat);
        CHECK(sets == 2);
        CHECK(sp[0].files_total == 3 && sp[0].files_done == 3 && sp[0].bytes_done == 45);
        CHECK(sp[1].files_done == 0 && sp[1].bytes_total == 45);
    }

    /* Restore plan: both unplugged, D holds more, so D first. */
    bd_media_disconnect(cat, d);
    bd_media_disconnect(cat, e);
    plan_list p;
    memset(&p, 0, sizeof(p));
    int64_t total = 0, none = 0;
    REQUIRE_OK(bd_restore_plan(cat, src, "", at("span/out"), collect_plan, &p, &total, &none), cat);
    CHECK(total == 3 && none == 0 && p.n == 2);
    CHECK(strcmp(p.labels[0], "Drive D") == 0 && p.files[0] == 2 && strcmp(p.labels[1], "Drive E") == 0 && p.files[1] == 1);

    /* E plugged in: it comes first. Restore its part now. */
    int64_t again;
    REQUIRE_OK(bd_media_connect(cat, at("span/E"), &again, quiet, NULL), cat);
    memset(&p, 0, sizeof(p));
    REQUIRE_OK(bd_restore_plan(cat, src, "", at("span/out"), collect_plan, &p, &total, &none), cat);
    CHECK(p.n == 2 && strcmp(p.labels[0], "Drive E") == 0 && p.connected[0] && !p.connected[1]);
    bd_restore_stats rs;
    REQUIRE_OK(bd_restore(cat, src, NULL, at("span/out"), &rs, quiet, NULL), cat);
    CHECK(rs.files_restored == 1 && rs.files_offline == 2);

    /* Then D arrives: the rest comes back, E's file is left alone. */
    REQUIRE_OK(bd_media_connect(cat, at("span/D"), &again, quiet, NULL), cat);
    memset(&p, 0, sizeof(p));
    REQUIRE_OK(bd_restore_plan(cat, src, "", at("span/out"), collect_plan, &p, &total, &none), cat);
    CHECK(total == 2 && p.n == 1 && strcmp(p.labels[0], "Drive D") == 0);
    REQUIRE_OK(bd_restore(cat, src, NULL, at("span/out"), &rs, quiet, NULL), cat);
    CHECK(rs.files_restored == 2 && rs.files_already_there == 1 && rs.files_offline == 0);
    CHECK(file_equals(at("span/out/big.bin"), "0123456789012345678901234567890123456789"));
    REQUIRE_OK(bd_restore_plan(cat, src, "", at("span/out"), collect_plan, &p, &total, &none), cat);
    CHECK(total == 0);

    /* D gets big.bin now that it has room. */
    REQUIRE_OK(bd_backup(cat, d, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 1 && bs.copies_alongside == 0);

    /* Old versions are never removed: each new one goes next to them. */
    const char *contents[] = {"22", "333", "4444"};
    for (int i = 0; i < 3; i++) {
        write_file(at("span/src/v.txt"), contents[i]);
        REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
        REQUIRE_OK(bd_backup(cat, d, 0, &bs, quiet, NULL), cat);
        CHECK(bs.files_copied == 1 && bs.copies_alongside == 1);
    }
    CHECK(exists(at("span/D/BRODALF")));
    char vdir[1400];
    snprintf(vdir, sizeof(vdir), "span/D/BRODALF/%s", bd_catalog_uuid(cat));
    char v1[1500], v2[1500], v4[1500];
    snprintf(v1, sizeof(v1), "%s/src/v.txt", vdir);
    snprintf(v2, sizeof(v2), "%s/.versions/src/v.v2.txt", vdir);
    snprintf(v4, sizeof(v4), "%s/.versions/src/v.v4.txt", vdir);
    CHECK(file_equals(at(v1), "1") && file_equals(at(v2), "22") && file_equals(at(v4), "4444"));

    /* A file put on the drive by someone else at the place BRODALF would
     * use is left alone; the copy goes next to it. */
    write_file(at("span/src/new.txt"), "brodalf's");
    char intruder[1500];
    snprintf(intruder, sizeof(intruder), "%s/src/new.txt", vdir);
    write_file(at(intruder), "not brodalf's");
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    bd_test_free_bytes = 1000;
    REQUIRE_OK(bd_backup(cat, d, 0, &bs, quiet, NULL), cat);
    bd_test_free_bytes = -1;
    CHECK(bs.files_copied == 1 && bs.copies_alongside == 1);
    CHECK(file_equals(at(intruder), "not brodalf's"));

        bd_catalog_close(cat);
}

static void set_env(const char *name, const char *value)
{
#ifdef _WIN32
    _putenv_s(name, value);
    wchar_t wn[64], wv[256];
    MultiByteToWideChar(CP_UTF8, 0, name, -1, wn, 64);
    MultiByteToWideChar(CP_UTF8, 0, value, -1, wv, 256);
    SetEnvironmentVariableW(wn, wv);
#else
    setenv(name, value, 1);
#endif
}

/* App log, redaction and the prefilled issue link. */
static void test_report(void)
{
    /* The last database: nothing until one is set, then that path,
     * replaced by the next. */
    set_env("BRODALF_STATE", at("state"));
    CHECK(bd_last_database() == NULL);
    REQUIRE_OK(bd_last_database_set("C:\\Users\\me\\My backups.brodalf"), NULL);
    char *last = bd_last_database();
    CHECK(last && strcmp(last, "C:\\Users\\me\\My backups.brodalf") == 0);
    free(last);
    REQUIRE_OK(bd_last_database_set("/home/me/Ünïcode backups.brodalf"), NULL);
    last = bd_last_database();
    CHECK(last && strcmp(last, "/home/me/Ünïcode backups.brodalf") == 0);
    free(last);
    CHECK(!exists(at("state/last-database.txt.tmp")));

#ifdef _WIN32
    set_env("USERNAME", "Zqxwolf");
#else
    set_env("USER", "Zqxwolf");
#endif
    char *r = bd_redact("C:\\Users\\zqxwolf\\Photos: signed in as jane.doe@example.com, Authorization: Bearer abc.DEF-123 ok");
    CHECK(r && strstr(r, "C:\\Users\\<user>\\Photos") && strstr(r, "signed in as <email>,") && strstr(r, "Bearer <hidden> ok"));
    CHECK(r && !strstr(r, "zqxwolf") && !strstr(r, "jane") && !strstr(r, "abc.DEF"));
    free(r);

    char log[2048];
    snprintf(log, sizeof(log), "%s", at("logs/brodalf.log"));
    REQUIRE_OK(bd_applog_open(log), NULL);
    CHECK(exists(log));
    bd_applog("first line from /home/Zqxwolf/x");
    for (int i = 0; i < 5000; i++) bd_applog("line %d", i);
    char *body = bd_report_body("Backup failed: disk full");
    CHECK(body && strstr(body, "Backup failed: disk full") && strstr(body, "Recent log") && strstr(body, "line 4999\n"));
    CHECK(body && strstr(body, bd_version()) && !strstr(body, "Zqxwolf"));
    free(body);

    char *saved = bd_report_save("Backup failed: disk full");
    CHECK(saved != NULL);
    if (saved) {
        FILE *f = fopen(saved, "rb");
        char text[4096] = "";
        if (f) { text[fread(text, 1, sizeof(text) - 1, f)] = '\0'; fclose(f); }
        CHECK(strstr(text, "https://github.com/Phawx/BRODALF/issues") && strstr(text, "Backup failed: disk full"));
        CHECK(strstr(saved, "reports") && strstr(saved, "brodalf-error-"));
        free(saved);
    }

    /* A long log moves to .old once it passes 1 MB. */
    char big[1001];
    memset(big, 'x', 1000);
    big[1000] = '\0';
    for (int i = 0; i < 1100; i++) bd_applog("%s", big);
    char old[2100];
    snprintf(old, sizeof(old), "%s.old", log);
    CHECK(exists(old));
    bd_applog_open(NULL);
    CHECK(bd_applog_path() == NULL);
}

typedef struct {
    int reports, ends, recorded;
    bd_progress last;
    char phase[32];
    int64_t max_file_total;
} progress_log;

static void on_progress(void *ctx, const bd_progress *p)
{
    progress_log *l = ctx;
    l->reports++;
    if (p->done) l->ends++;
    if (p->recorded) l->recorded++;
    l->last = *p;
    snprintf(l->phase, sizeof(l->phase), "%s", p->phase);
    l->last.phase = l->phase;
    l->last.current = "";
    if (p->file_bytes_total > l->max_file_total) l->max_file_total = p->file_bytes_total;
}

typedef struct { int n; char path[1024]; } in_use_list;

static int collect_in_use(void *ctx, const char *path, int64_t size)
{
    in_use_list *l = ctx;
    if (l->n++ == 0) snprintf(l->path, sizeof(l->path), "%s", path);
    (void)size;
    return 0;
}

static int count_changed(void *ctx, const bd_changed_info *c)
{
    (void)c;
    (*(int *)ctx)++;
    return 0;
}

static int first_source_path(void *ctx, const bd_source_info *info)
{
    snprintf((char *)ctx, 1024, "%s", info->path);
    return 1;
}

/* Moves, the verify pass, progress, the backup preview and its warning of
 * mass changes, drive suggestions, online-only files and files in use. */
static void test_moves_guard_and_progress(void)
{
    char name[64], text[64];
    for (int i = 0; i < 60; i++) {
        snprintf(name, sizeof(name), "g/Work/%s/f%02d.txt", i < 20 ? "old" : "keep", i);
        snprintf(text, sizeof(text), "file %d, first version", i);
        write_file(at(name), text);
    }
    bd_mkdirs(at("g/d1"));
    bd_mkdirs(at("g/d2"));
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("g/g.brodalf"), &cat), NULL);
    int64_t src, d1, d2;
    REQUIRE_OK(bd_source_add(cat, at("g/Work"), &src), cat);
    progress_log pl;
    memset(&pl, 0, sizeof(pl));
    bd_catalog_set_progress(cat, on_progress, &pl);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_new == 60);
    CHECK(pl.ends == 1 && strcmp(pl.last.phase, "scan") == 0 && pl.last.files_done == 60 && pl.last.bytes_done > 0);

    REQUIRE_OK(bd_media_init(cat, at("g/d1"), "One", 0, &d1), cat);
    memset(&pl, 0, sizeof(pl));
    bd_backup_stats bs;
    REQUIRE_OK(bd_backup(cat, d1, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 60);
    CHECK(pl.ends == 1 && strcmp(pl.last.phase, "backup") == 0);
    CHECK(pl.recorded > 0); /* the window shows the full copies growing from these */
    CHECK(pl.last.files_total == 60 && pl.last.files_done == 60 && pl.last.bytes_done == pl.last.bytes_total);

    /* Rename a folder holding a third of the files: the scan sees the same
     * files under a new path, so they keep their versions and copies. The
     * copies stay where they are on the drive; only the catalog changes. */
    CHECK(bd_rename_noreplace(at("g/Work/old"), at("g/Work/renamed")) == 0);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_moved == 20 && ss.files_new == 0 && ss.files_deleted == 0);
    bd_backup_preview_info pv;
    REQUIRE_OK(bd_backup_preview(cat, d1, 0, &pv), cat);
    CHECK(pv.files_to_copy == 0 && pv.files_new == 0 && pv.files_changed == 0);
    CHECK(pv.files_held == 60 && !pv.mass_change);
    CHECK(pv.last_scan_ms > 0 && pv.free_is_current && pv.bytes_short == 0);
    REQUIRE_OK(bd_backup(cat, d1, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 0 && bs.files_failed == 0);
    char p[1200];
    snprintf(p, sizeof(p), "g/d1/BRODALF/%s/Work/old/f03.txt", bd_catalog_uuid(cat));
    CHECK(file_equals(at(p), "file 3, first version"));
    find_ctx ren = lookup(cat, src, 0, "renamed");
    CHECK(lookup(cat, src, ren.node_id, "f03.txt").state == BD_STATE_AVAILABLE);
    CHECK(lookup(cat, src, 0, "old").node_id == 0); /* the emptied folder is gone from the catalog */
    /* Renamed back: the same again. */
    CHECK(bd_rename_noreplace(at("g/Work/renamed"), at("g/Work/old")) == 0);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_moved == 20 && ss.files_new == 0 && ss.files_deleted == 0);
    CHECK(bd_rename_noreplace(at("g/Work/old"), at("g/Work/renamed")) == 0);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_backup(cat, d1, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 0);
    /* Restoring puts the file where it is now. */
    {
        bd_restore_stats rs0;
        REQUIRE_OK(bd_restore(cat, src, "renamed/f03.txt", at("g/one"), &rs0, quiet, NULL), cat);
        CHECK(rs0.files_restored == 1 && file_equals(at("g/one/renamed/f03.txt"), "file 3, first version"));
    }

    /* The verify pass reads back copies never read back, then skips them. */
    bd_check_stats cs;
    memset(&pl, 0, sizeof(pl));
    REQUIRE_OK(bd_media_verify(cat, d1, 30, &cs, quiet, NULL), cat);
    CHECK(cs.rehashed == 60 && cs.ok == 60);
    CHECK(strcmp(pl.last.phase, "verify") == 0 && pl.last.files_total == 60 && pl.last.bytes_done == pl.last.bytes_total);
    REQUIRE_OK(bd_media_verify(cat, d1, 30, &cs, quiet, NULL), cat);
    CHECK(cs.copies == 0 && cs.rehashed == 0);
    snprintf(p, sizeof(p), "g/d1/BRODALF/%s/Work/keep/f20.txt", bd_catalog_uuid(cat));
    write_file(at(p), "file 20, first versioX");
    REQUIRE_OK(bd_media_verify(cat, d1, 0, &cs, quiet, NULL), cat);
    CHECK(cs.rehashed == 60 && cs.bad == 1);
    REQUIRE_OK(bd_backup(cat, d1, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 1);
    bd_catalog_set_progress(cat, NULL, NULL);

    /* A second drive: it would take everything, and it has room. */
    REQUIRE_OK(bd_media_init(cat, at("g/d2"), "Two", 0, &d2), cat);
    bd_media_disconnect(cat, d2);
    bd_risk_help sug;
    REQUIRE_OK(bd_suggest_drive(cat, &sug), cat);
    CHECK(sug.media_id == d2 && sug.fits && sug.files == 60 && sug.free_bytes > 0 && strcmp(sug.label, "Two") == 0);

    /* Half the files change at once. Nothing pauses by itself, but the
     * preview warns before the user starts a backup. */
    for (int i = 20; i < 55; i++) {
        snprintf(name, sizeof(name), "g/Work/keep/f%02d.txt", i);
        write_file(at(name), "ENCRYPTED GARBAGE");
    }
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_changed == 35);
    REQUIRE_OK(bd_backup_preview(cat, d1, 0, &pv), cat);
    CHECK(pv.files_to_copy == 35 && pv.files_changed == 35 && pv.files_new == 0);
    CHECK(pv.files_held == 60 && pv.held_changed == 35 && pv.mass_change && pv.held_deleted == 0);
    {
        int changed = 0;
        REQUIRE_OK(bd_list_changed_on_media(cat, d1, count_changed, &changed), cat);
        CHECK(changed == 35);
    }
    REQUIRE_OK(bd_backup_preview(cat, d2, 0, &pv), cat);
    CHECK(pv.files_to_copy == 60 && pv.files_held == 0 && !pv.mass_change && !pv.free_is_current);

    /* The latest versions were never backed up: a restore brings back the
     * versions that were. */
    bd_restore_stats rs;
    REQUIRE_OK(bd_restore(cat, src, NULL, at("g/latest"), &rs, quiet, NULL), cat);
    CHECK(rs.files_restored == 60 && rs.files_older_version == 35 && rs.files_no_copy == 0);
    CHECK(file_equals(at("g/latest/keep/f20.txt"), "file 20, first version"));

    /* The changes were the user's: back up. Every old version stays. */
    REQUIRE_OK(bd_backup(cat, d1, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 35 && bs.copies_alongside == 35);
    REQUIRE_OK(bd_backup_preview(cat, d1, 0, &pv), cat);
    CHECK(pv.files_to_copy == 0 && !pv.mass_change);
    {
        /* The one full copy there is, is on the first drive. */
        bd_set_drive sd[8];
        int nsd = 0;
        REQUIRE_OK(bd_backup_set_drives(cat, sd, 8, &nsd), cat);
        CHECK(nsd == 1 && sd[0].set == 1 && sd[0].media_id == d1 && sd[0].files == 60);
    }

    /* Where the drive stands. */
    bd_media_status mst;
    REQUIRE_OK(bd_media_status_get(cat, d1, &mst), cat);
    CHECK(mst.files_current == 60 && mst.files_behind == 0 && mst.files_not_here == 0);
    CHECK(mst.old_versions == 35 && mst.copies_bad == 0);
    CHECK(mst.last_backup_ms > 0 && mst.last_backup_end_ms >= mst.last_backup_ms);
    CHECK(strcmp(mst.last_backup_status, "done") == 0 && mst.last_backup_files == 35);
    CHECK(strcmp(mst.last_check_kind, "read back") == 0 && mst.copies_never_read == 36); /* 35 now, and f20 written again */
    REQUIRE_OK(bd_media_status_get(cat, d2, &mst), cat);
    CHECK(mst.files_current == 0 && mst.files_not_here == 60 && mst.last_backup_ms == 0 && mst.last_check_ms == 0);
    CHECK(mst.last_backup_status[0] == 0);
    write_file(at("g/Work/keep/f56.txt"), "file 56, second version");
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_media_status_get(cat, d1, &mst), cat);
    CHECK(mst.files_current == 59 && mst.files_behind == 1 && mst.files_not_here == 0);
    REQUIRE_OK(bd_backup(cat, d1, 0, &bs, quiet, NULL), cat);

    /* At risk also counts deleted files and older versions. */
    bd_risk_stats rk;
    REQUIRE_OK(bd_list_at_risk(cat, 0, NULL, NULL, &rk), cat);
    CHECK(rk.old_versions_kept == 36 && rk.old_versions_short == 36); /* one drive, target 2 */
    CHECK(rk.deleted_kept == 0);

    /* An online-only cloud file is left alone, not read (which would
     * download it), and comes in once it is on the PC. */
    char root[1024] = "";
    bd_list_sources(cat, first_source_path, root);
    char *mail = bd_path_join(root, "mail.pst");
    write_file(at("g/Work/mail.pst"), "live mailbox");
    bd_test_online_only = "mail.pst";
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_online_only == 1 && ss.files_new == 0 && ss.files_deleted == 0 && ss.errors == 0);
    REQUIRE_OK(bd_backup(cat, d1, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 0 && bs.files_failed == 0);
    bd_test_online_only = NULL;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_online_only == 0 && ss.files_new == 1);
    REQUIRE_OK(bd_backup(cat, d1, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 1);
    /* Once known, it stays known while online-only: not marked deleted. */
    bd_test_online_only = "mail.pst";
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_deleted == 0);
    bd_test_online_only = NULL;

    /* The whole list as CSV. */
    int64_t rows = 0;
    REQUIRE_OK(bd_export_csv(cat, at("g/files.csv"), &rows), cat);
    CHECK(rows == 61); /* 60 live, mail.pst */
    FILE *csv = fopen(at("g/files.csv"), "rb");
    char line[512] = "";
    CHECK(csv && fgets(line, sizeof(line), csv));
    CHECK(memcmp(line, "\xEF\xBB\xBF\"Folder\",\"File\"", 17) == 0);
    int found_mail = 0;
    while (csv && fgets(line, sizeof(line), csv))
        if (strstr(line, "mail.pst\",12,") && strstr(line, "\"no\",1,\"One\",1")) found_mail = 1;
    CHECK(found_mail);
    if (csv) fclose(csv);

    /* A file in use is read from the copy the user had taken out of a
     * shadow copy, in scans and backups, until those copies are cleared. */
    write_file(at("g/staged-mail.bin"), "mailbox as the snapshot saw it");
    write_file(at("g/Work/mail.pst"), "live mailbox!");
    REQUIRE_OK(bd_substitute_add(cat, mail, at("g/staged-mail.bin")), cat);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_changed == 1 && ss.errors == 0);
    REQUIRE_OK(bd_backup(cat, d1, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 1 && bs.files_failed == 0);
    bd_restore_stats shr;
    REQUIRE_OK(bd_restore(cat, src, "mail.pst", at("g/shadow-out"), &shr, quiet, NULL), cat);
    CHECK(shr.files_restored == 1 && file_equals(at("g/shadow-out/mail.pst"), "mailbox as the snapshot saw it"));
    bd_substitutes_clear(cat);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_changed == 1); /* the file itself again */
#ifdef _WIN32
    /* Locked against reading by another program: listed as in use. */
    wchar_t wmail[1024];
    MultiByteToWideChar(CP_UTF8, 0, mail, -1, wmail, 1024);
    for (wchar_t *c = wmail; *c; c++) if (*c == L'/') *c = L'\\';
    write_file(at("g/Work/mail.pst"), "live mailbox, changed");
    HANDLE h = CreateFileW(wmail, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(h != INVALID_HANDLE_VALUE);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_in_use == 1 && ss.errors == 0);
    in_use_list iu;
    memset(&iu, 0, sizeof(iu));
    bd_list_in_use(cat, collect_in_use, &iu);
    CHECK(iu.n == 1 && strcmp(iu.path, mail) == 0);
    /* With a copy from a shadow copy it is read while still locked. */
    write_file(at("g/staged-mail2.bin"), "live mailbox, changed");
    REQUIRE_OK(bd_substitute_add(cat, mail, at("g/staged-mail2.bin")), cat);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_in_use == 0 && ss.files_changed == 1 && ss.errors == 0);
    REQUIRE_OK(bd_backup(cat, d1, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 1 && bs.files_in_use == 0);
    bd_substitutes_clear(cat);
    CloseHandle(h);
#endif
    free(mail);
    bd_catalog_close(cat);
}


/* Big copies go through the writer thread: the copy and the hash must be
 * exactly those of the file, plain or sealed, and a write that fails after
 * the thread started must fail the copy as before. */
static void test_big_copy(void)
{
    size_t len = (size_t)9 * 1024 * 1024 + 12345;
    unsigned char *data = malloc(len);
    CHECK(data != NULL);
    if (!data) return;
    for (size_t i = 0; i < len; i++) data[i] = (unsigned char)((i * 2654435761u) >> 13);
    bd_mkdirs(at("big"));
    FILE *f = bd_fopen(at("big/src.bin"), "wb");
    CHECK(f && fwrite(data, 1, len, f) == len);
    if (f) fclose(f);

    char want[BD_HASH_HEX_LEN + 1], got[BD_HASH_HEX_LEN + 1], back[BD_HASH_HEX_LEN + 1];
    int64_t size = 0;
    CHECK(bd_hash_file(at("big/src.bin"), NULL, want, &size) == 0 && size == (int64_t)len);

    /* Plain. */
    FILE *out = bd_fopen(at("big/copy.bin"), "wb");
    CHECK(out && bd_hash_copy(at("big/src.bin"), NULL, out, NULL, got, &size) == 0);
    if (out) fclose(out);
    CHECK(strcmp(got, want) == 0 && size == (int64_t)len);
    CHECK(bd_hash_file(at("big/copy.bin"), NULL, back, NULL) == 0 && strcmp(back, want) == 0);
    bd_stat_t st;
    CHECK(bd_stat(at("big/copy.bin"), &st) == 0 && st.size == (int64_t)len);

    /* Sealed, then read back through the key. */
    uint8_t key[32];
    CHECK(bd_random_bytes(key, sizeof(key)) == 0);
    out = bd_fopen(at("big/copy.bdenc"), "wb");
    CHECK(out && bd_hash_copy(at("big/src.bin"), NULL, out, key, got, NULL) == 0);
    if (out) fclose(out);
    CHECK(strcmp(got, want) == 0);
    CHECK(bd_hash_copy(at("big/copy.bdenc"), key, NULL, NULL, back, &size) == 0 && strcmp(back, want) == 0 &&
          size == (int64_t)len);
    /* And re-sealed while copying, as a drive-to-drive copy would be. */
    out = bd_fopen(at("big/copy2.bdenc"), "wb");
    CHECK(out && bd_hash_copy(at("big/copy.bdenc"), key, out, key, got, NULL) == 0);
    if (out) fclose(out);
    CHECK(strcmp(got, want) == 0);
    CHECK(bd_hash_copy(at("big/copy2.bdenc"), key, NULL, NULL, back, NULL) == 0 && strcmp(back, want) == 0);

#ifndef _WIN32
    /* A write that fails at 6 MiB, after the writer thread took over. */
    struct rlimit old, lim;
    if (getrlimit(RLIMIT_FSIZE, &old) == 0) {
        void (*prev)(int) = signal(SIGXFSZ, SIG_IGN);
        lim = old;
        lim.rlim_cur = 6 * 1024 * 1024;
        if (setrlimit(RLIMIT_FSIZE, &lim) == 0) {
            out = bd_fopen(at("big/short.bin"), "wb");
            int rc = out ? bd_hash_copy(at("big/src.bin"), NULL, out, NULL, got, NULL) : 0;
            if (out) fclose(out);
            setrlimit(RLIMIT_FSIZE, &old);
            CHECK(rc == -2);
        }
        signal(SIGXFSZ, prev);
    }
#endif
    free(data);
}

typedef struct { int files, tmp; } tmp_count;

static int count_tmp(void *ctx, const char *rel, const bd_stat_t *st)
{
    tmp_count *t = ctx;
    if (st->is_file && strncmp(rel, "catalog-r", 9) != 0) t->files++; /* the catalog's own copies aside */
    if (strstr(rel, BD_TMP_MARKER)) t->tmp++;
    return 0;
}

/* Everything a scan records, without the times that differ between runs. */
static char *catalog_dump(bd_catalog *cat)
{
    sqlite3_stmt *q;
    char *out = NULL;
    if (sqlite3_prepare_v2(cat->db,
                           "SELECT group_concat(line, char(10)) FROM ("
                           " SELECT 'n|'||id||'|'||rel_path||'|'||is_dir||'|'||size||'|'||mtime_ns||'|'||"
                           "  COALESCE(current_version_id,0)||'|'||deleted||'|'||COALESCE(parent_id,0) AS line FROM nodes"
                           " UNION ALL SELECT 'v|'||id||'|'||node_id||'|'||version_no||'|'||hash||'|'||size||'|'||mtime_ns"
                           "  FROM versions ORDER BY 1)",
                           -1, &q, NULL) == SQLITE_OK) {
        if (sqlite3_step(q) == SQLITE_ROW && sqlite3_column_text(q, 0))
            out = bd_strdup((const char *)sqlite3_column_text(q, 0));
        sqlite3_finalize(q);
    }
    return out ? out : bd_strdup("");
}

static int same_stats(const bd_scan_stats *a, const bd_scan_stats *b)
{
    return a->files_seen == b->files_seen && a->dirs_seen == b->dirs_seen && a->files_new == b->files_new &&
           a->files_changed == b->files_changed && a->files_deleted == b->files_deleted &&
           a->bytes_hashed == b->bytes_hashed && a->errors == b->errors && a->skipped == b->skipped;
}

/* A scan that hashes on several threads must record exactly what hashing
 * one file at a time does. */
static void test_parallel_scan(void)
{
    char name[128];
    static char text[300000];
    for (int i = 0; i < 300; i++) {
        snprintf(name, sizeof(name), "ps/Tree/d%d/e%d/f%03d.bin", i % 7, i % 3, i);
        /* Mostly small files, a few big ones that take a while. */
        size_t n = i % 50 == 0 ? sizeof(text) - 1 : (size_t)(i * 37 % 2000);
        for (size_t k = 0; k < n; k++) text[k] = (char)('a' + (k * 7 + (size_t)i) % 26);
        text[n] = '\0';
        write_file(at(name), text);
    }
    int modes[] = {-1, 1, 4};
    char *dumps[3][2];
    bd_scan_stats stats[3][2];
    progress_log pl[3];
    for (int m = 0; m < 3; m++) {
        bd_test_scan_workers = modes[m];
        snprintf(name, sizeof(name), "ps/c%d.brodalf", m);
        bd_catalog *cat;
        REQUIRE_OK(bd_catalog_create(at(name), &cat), NULL);
        REQUIRE_OK(bd_source_add(cat, at("ps/Tree"), NULL), cat);
        memset(&pl[m], 0, sizeof(pl[m]));
        bd_catalog_set_progress(cat, on_progress, &pl[m]);
        REQUIRE_OK(bd_scan(cat, &stats[m][0], quiet, NULL), cat);
        dumps[m][0] = catalog_dump(cat);
        bd_catalog_save(cat);
        bd_catalog_close(cat);
    }
    /* Change some files; every catalog rescans the changed tree. */
    for (int i = 0; i < 300; i += 11) {
        snprintf(name, sizeof(name), "ps/Tree/d%d/e%d/f%03d.bin", i % 7, i % 3, i);
        snprintf(text, sizeof(text), "changed %d", i);
        write_file(at(name), text);
    }
    for (int m = 0; m < 3; m++) {
        bd_test_scan_workers = modes[m];
        snprintf(name, sizeof(name), "ps/c%d.brodalf", m);
        bd_catalog *cat;
        REQUIRE_OK(bd_catalog_open(at(name), &cat), NULL);
        REQUIRE_OK(bd_scan(cat, &stats[m][1], quiet, NULL), cat);
        dumps[m][1] = catalog_dump(cat);
        if (m == 0) {
            /* Many small files: synced in groups, then renamed and recorded. */
            bd_mkdirs(at("ps/drive"));
            int64_t drive;
            REQUIRE_OK(bd_media_init(cat, at("ps/drive"), "Many", 0, &drive), cat);
            bd_backup_stats bs;
            REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
            CHECK(bs.files_copied == 300 && bs.files_failed == 0);
            bd_check_stats cs;
            REQUIRE_OK(bd_media_check(cat, drive, &cs, quiet, NULL), cat);
            CHECK(cs.copies == 300 && cs.ok == 300);
            char dir[1200];
            snprintf(dir, sizeof(dir), "ps/drive/BRODALF/%s", bd_catalog_uuid(cat));
            tmp_count tc = {0, 0};
            bd_walk(at(dir), count_tmp, NULL, &tc);
            CHECK(tc.tmp == 0 && tc.files == 301); /* the copies and BRODALF.media */
        }
        bd_catalog_close(cat);
    }
    bd_test_scan_workers = 0;
    CHECK(stats[0][0].files_new == 300 && stats[0][1].files_changed == 28);
    for (int m = 1; m < 3; m++)
        for (int k = 0; k < 2; k++) CHECK(strcmp(dumps[0][k], dumps[m][k]) == 0 && same_stats(&stats[0][k], &stats[m][k]));
    CHECK(strlen(dumps[0][1]) > 1000);
    /* Progress counted every byte, from the scanning thread. */
    CHECK(pl[2].ends == 1 && pl[2].last.bytes_done == stats[2][0].bytes_hashed);
    CHECK(pl[1].last.bytes_done == stats[1][0].bytes_hashed);
    for (int m = 0; m < 3; m++) { free(dumps[m][0]); free(dumps[m][1]); }
}

/* Cancelling from the progress callback. */
typedef struct {
    bd_catalog *cat;
    const char *phase;   /* cancel in this phase */
    int64_t after_files; /* once this many files are done */
    const char *in_file; /* or this many reports into this file */
    int reports_in_file, after_reports;
    int fired;
} cancel_at;

static void cancel_progress(void *ctx, const bd_progress *p)
{
    cancel_at *c = ctx;
    c->cat->progress_last_ms = 0; /* report every file and block, not ten times a second */
    if (c->fired || p->done || strcmp(p->phase, c->phase) != 0) return;
    if (c->in_file) {
        if (strcmp(p->current, c->in_file) == 0 && ++c->reports_in_file >= c->after_reports) c->fired = 1;
    } else if (p->files_done >= c->after_files) {
        c->fired = 1;
    }
    if (c->fired) bd_catalog_cancel(c->cat);
}

static int64_t count_rows(bd_catalog *cat, const char *sql)
{
    sqlite3_stmt *q;
    int64_t n = -1;
    if (sqlite3_prepare_v2(cat->db, sql, -1, &q, NULL) == SQLITE_OK) {
        if (sqlite3_step(q) == SQLITE_ROW) n = sqlite3_column_int64(q, 0);
        sqlite3_finalize(q);
    }
    return n;
}

/* Every recorded copy is on the drive with its recorded size, and nothing
 * half-written is left. */
static void check_drive_consistent(bd_catalog *cat, int64_t drive, const char *drive_rel)
{
    char dir[1200];
    snprintf(dir, sizeof(dir), "%s/BRODALF/%s", drive_rel, bd_catalog_uuid(cat));
    tmp_count tc = {0, 0};
    bd_walk(at(dir), count_tmp, NULL, &tc);
    CHECK(tc.tmp == 0);
    CHECK(tc.files == 1 + count_rows(cat, "SELECT COUNT(*) FROM copies")); /* plus BRODALF.media */
    bd_check_stats cs;
    REQUIRE_OK(bd_media_check(cat, drive, &cs, quiet, NULL), cat);
    CHECK(cs.copies == cs.ok && cs.bad == 0 && cs.missing == 0);
}

static void test_cancel(void)
{
    char name[64];
    static char text[4096];
    for (int i = 0; i < 40; i++) {
        snprintf(name, sizeof(name), "cx/Src/f%02d.txt", i);
        snprintf(text, sizeof(text), "small file %d", i);
        write_file(at(name), text);
    }
    /* A big file, copied in blocks through the writer thread. */
    size_t big = (size_t)7 * 1024 * 1024 + 99;
    char *data = malloc(big);
    if (!data) return;
    for (size_t i = 0; i < big; i++) data[i] = (char)(i * 31 + (i >> 11));
    /* It is written only after the between-files stop below, so the files
     * copied before that stop never include it, whatever order the system
     * lists the folder in (alphabetical on NTFS). */
    bd_mkdirs(at("cx/drive"));

    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("cx/c.brodalf"), &cat), NULL);
    int64_t src, drive;
    REQUIRE_OK(bd_source_add(cat, at("cx/Src"), &src), cat);
    cancel_at ca;

    /* A scan stopped halfway leaves the folder as the catalog knew it. */
    memset(&ca, 0, sizeof(ca));
    ca.cat = cat;
    ca.phase = "scan";
    ca.after_files = 5;
    bd_catalog_set_progress(cat, cancel_progress, &ca);
    bd_scan_stats ss;
    CHECK(bd_scan(cat, &ss, quiet, NULL) == BD_ERR_CANCELLED && ca.fired);
    CHECK(count_rows(cat, "SELECT COUNT(*) FROM nodes") == 0 && ss.files_new == 0);
    /* The next scan clears the request and runs to the end. */
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_new == 40);

    REQUIRE_OK(bd_media_init(cat, at("cx/drive"), "Stop", 0, &drive), cat);

    /* Stopped between files: what was copied stays recorded. */
    memset(&ca, 0, sizeof(ca));
    ca.cat = cat;
    ca.phase = "backup";
    ca.after_files = 10;
    bd_backup_stats bs;
    CHECK(bd_backup(cat, drive, 0, &bs, quiet, NULL) == BD_ERR_CANCELLED && ca.fired);
    CHECK(bs.files_copied >= 10 && bs.files_copied < 40 && bs.files_failed == 0);
    CHECK(count_rows(cat, "SELECT COUNT(*) FROM copies") == bs.files_copied);
    CHECK(count_rows(cat, "SELECT COUNT(*) FROM jobs WHERE kind='backup' AND status='stopped'") == 1);
    check_drive_consistent(cat, drive, "cx/drive");
    int64_t first = bs.files_copied;

    FILE *f = bd_fopen(at("cx/Src/big.bin"), "wb");
    CHECK(f && fwrite(data, 1, big, f) == big);
    if (f) fclose(f);
    bd_catalog_set_progress(cat, NULL, NULL);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_new == 1);

    /* Stopped in the middle of the big file: no half copy on the drive. */
    memset(&ca, 0, sizeof(ca));
    ca.cat = cat;
    ca.phase = "backup";
    ca.in_file = "big.bin";
    ca.after_reports = 4;
    bd_catalog_set_progress(cat, cancel_progress, &ca);
    bd_status st = bd_backup(cat, drive, 0, &bs, quiet, NULL);
    CHECK(st == BD_ERR_CANCELLED && ca.fired);
    CHECK(count_rows(cat, "SELECT COUNT(*) FROM copies c JOIN versions v ON v.id=c.version_id JOIN nodes n ON n.id=v.node_id"
                          " WHERE n.name='big.bin'") == 0);
    check_drive_consistent(cat, drive, "cx/drive");
    int64_t second = bs.files_copied;

    /* The next backup finishes the job. */
    bd_catalog_set_progress(cat, NULL, NULL);
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_failed == 0 && first + second + bs.files_copied == 41);
    CHECK(count_rows(cat, "SELECT COUNT(*) FROM copies") == 41);
    check_drive_consistent(cat, drive, "cx/drive");

    /* A full check stopped halfway keeps what it found and touches nothing else. */
    memset(&ca, 0, sizeof(ca));
    ca.cat = cat;
    ca.phase = "check";
    ca.after_files = 3;
    bd_catalog_set_progress(cat, cancel_progress, &ca);
    bd_check_stats cs;
    CHECK(bd_media_check(cat, drive, &cs, quiet, NULL) == BD_ERR_CANCELLED && ca.fired);
    /* The copy being read when the request came is left as it was. */
    CHECK(cs.copies >= 2 && cs.copies < 41 && cs.bad == 0 && cs.missing == 0 && cs.ok == cs.copies);
    CHECK(count_rows(cat, "SELECT COUNT(*) FROM copies WHERE state<>'ok'") == 0);

    /* A restore stopped inside the big file leaves no half-written file. */
    memset(&ca, 0, sizeof(ca));
    ca.cat = cat;
    ca.phase = "restore";
    ca.in_file = "big.bin";
    ca.after_reports = 3;
    bd_restore_stats rs;
    CHECK(bd_restore(cat, src, "", at("cx/out"), &rs, quiet, NULL) == BD_ERR_CANCELLED && ca.fired);
    CHECK(!exists(at("cx/out/big.bin")) && !exists(at("cx/out/big.bin" BD_TMP_MARKER)));
    bd_catalog_set_progress(cat, NULL, NULL);
    REQUIRE_OK(bd_restore(cat, src, "", at("cx/out"), &rs, quiet, NULL), cat);
    CHECK(rs.files_failed == 0 && rs.files_restored + rs.files_already_there == 41);
    char back[BD_HASH_HEX_LEN + 1], want[BD_HASH_HEX_LEN + 1];
    CHECK(bd_hash_file(at("cx/out/big.bin"), NULL, back, NULL) == 0 &&
          bd_hash_file(at("cx/Src/big.bin"), NULL, want, NULL) == 0 && strcmp(back, want) == 0);
    bd_catalog_close(cat);
    free(data);
}

#ifndef _WIN32
static void set_time(const char *path, int64_t ns)
{
    struct timespec ts[2];
    ts[0].tv_sec = ts[1].tv_sec = (time_t)(ns / 1000000000LL);
    ts[0].tv_nsec = ts[1].tv_nsec = (long)(ns % 1000000000LL);
    CHECK(utimensat(AT_FDCWD, path, ts, 0) == 0);
}

static int64_t time_of(const char *path)
{
    bd_stat_t st;
    return bd_stat(path, &st) == 0 ? st.mtime_ns : -1;
}

/* Copies on the drive and restored files keep the original modified time. */
static void test_mtimes(void)
{
    const int64_t t1 = 1000000000123456789LL, t2 = 1200000000987654321LL, t3 = 1300000000000000500LL;
    write_file(at("mt/Src/a.txt"), "first a");
    set_time(at("mt/Src/a.txt"), t1);
    /* Big enough for the copy's writer thread. */
    size_t big = (size_t)5 * 1024 * 1024 + 7;
    char *data = malloc(big);
    if (!data) return;
    for (size_t i = 0; i < big; i++) data[i] = (char)(i * 13 + (i >> 10));
    FILE *f = bd_fopen(at("mt/Src/big.bin"), "wb");
    CHECK(f && fwrite(data, 1, big, f) == big);
    if (f) fclose(f);
    free(data);
    set_time(at("mt/Src/big.bin"), t2);
    bd_mkdirs(at("mt/drive"));

    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("mt/c.brodalf"), &cat), NULL);
    int64_t src, drive;
    REQUIRE_OK(bd_source_add(cat, at("mt/Src"), &src), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_media_init(cat, at("mt/drive"), "Times", 0, &drive), cat);
    bd_backup_stats bs;
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 2);
    char dir[1200];
    snprintf(dir, sizeof(dir), "mt/drive/BRODALF/%s/Src", bd_catalog_uuid(cat));
    char *copy_a = bd_sprintf("%s/a.txt", at(dir)), *copy_big = bd_sprintf("%s/big.bin", at(dir));
    CHECK(time_of(copy_a) == t1);
    CHECK(time_of(copy_big) == t2);

    /* The check finds every copy as recorded, reading each one back. */
    bd_check_stats cs;
    REQUIRE_OK(bd_media_check(cat, drive, &cs, quiet, NULL), cat);
    CHECK(cs.copies == 2 && cs.ok == 2 && cs.rehashed == 2);

    /* A new version goes next to the old copy, which keeps its time. */
    write_file(at("mt/Src/a.txt"), "second a, longer");
    set_time(at("mt/Src/a.txt"), t3);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 1 && bs.copies_alongside == 1);
    CHECK(time_of(copy_a) == t1);
    char *old_a = bd_sprintf("mt/drive/BRODALF/%s/.versions/Src/a.v2.txt", bd_catalog_uuid(cat));
    CHECK(time_of(at(old_a)) == t3);
    REQUIRE_OK(bd_media_check(cat, drive, &cs, quiet, NULL), cat);
    CHECK(cs.copies == 3 && cs.ok == 3);

    /* Restored files get the time the catalog has for that version. */
    bd_restore_stats rs;
    REQUIRE_OK(bd_restore(cat, src, "", at("mt/now"), &rs, quiet, NULL), cat);
    CHECK(rs.files_restored == 2);
    CHECK(time_of(at("mt/now/a.txt")) == t3);
    CHECK(time_of(at("mt/now/big.bin")) == t2);
    int64_t v1 = count_rows(cat, "SELECT v.id FROM versions v JOIN nodes n ON n.id=v.node_id WHERE n.name='a.txt' AND v.version_no=1");
    REQUIRE_OK(bd_restore_version(cat, v1, at("mt/then"), &rs, quiet, NULL), cat);
    CHECK(file_equals(at("mt/then/a.txt"), "first a"));
    CHECK(time_of(at("mt/then/a.txt")) == t1);

    /* Restored in place: the next scan sees nothing changed. */
    bd_remove(at("mt/Src/big.bin"));
    REQUIRE_OK(bd_restore(cat, src, "big.bin", at("mt/Src"), &rs, quiet, NULL), cat);
    CHECK(rs.files_restored == 1 && time_of(at("mt/Src/big.bin")) == t2);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_changed == 0 && ss.files_new == 0 && ss.bytes_hashed == 0);

    free(copy_a);
    free(copy_big);
    free(old_a);
    bd_catalog_close(cat);
}
#endif

static void rename_log(void *ctx, const char *msg)
{
    if (strstr(msg, "its new copies go to")) (*(int *)ctx)++;
}

/* Where names ignore case (Windows; here bd_test_fold_case stands in),
 * a change of letter case only is a rename, not a delete and a new file. */
/* Compressing a backup drive: only when asked, only where Windows can,
 * and the copies read back the same. */
static void test_compression(void)
{
    /* Big enough for NTFS to save room: it compresses 64 KB at a time. */
    size_t n = 256 * 1024;
    char *big = malloc(n + 1);
    if (!big) return;
    for (size_t i = 0; i < n; i++) big[i] = "compress me "[i % 12];
    big[n] = 0;
    write_file(at("cz/Src/text.txt"), big);
    write_file(at("cz/Src/sub/more.txt"), big);
    bd_mkdirs(at("cz/drive"));
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("cz/c.brodalf"), &cat), NULL);
    int64_t src, drive;
    REQUIRE_OK(bd_source_add(cat, at("cz/Src"), &src), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_media_init(cat, at("cz/drive"), "Squeeze", 0, &drive), cat);
    bd_backup_stats bs;
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    char dir[1024], copy[1200];
    snprintf(dir, sizeof(dir), "cz/drive/BRODALF/%s", bd_catalog_uuid(cat));
    snprintf(copy, sizeof(copy), "%s/Src/text.txt", dir);

    /* Nothing is compressed until asked. */
    int can = -1, on = -1;
    const char *why = NULL;
    REQUIRE_OK(bd_media_compression(cat, drive, &can, &on, &why), cat);
    CHECK(on == 0);
    CHECK(can ? why == NULL : why != NULL);
    bd_compress_stats cs;
    if (!can) {
        /* Outside Windows, or a drive that cannot: nothing changes. */
        CHECK(bd_media_set_compression(cat, drive, 1, &cs, quiet, NULL) == BD_ERR_INVALID);
        CHECK(bd_fs_compressed(at(copy)) == 0);
    } else {
        REQUIRE_OK(bd_media_set_compression(cat, drive, 1, &cs, quiet, NULL), cat);
        CHECK(cs.files >= 3 && cs.failed == 0 && cs.changed == cs.files); /* two copies and the catalog's */
        CHECK(cs.bytes_on_disk < cs.bytes);
        CHECK(bd_fs_compressed(at(dir)) == 1 && bd_fs_compressed(at(copy)) == 1);
        REQUIRE_OK(bd_media_compression(cat, drive, &can, &on, &why), cat);
        CHECK(on == 1);
        /* New copies land compressed, in old folders and new ones. */
        write_file(at("cz/Src/sub/new.txt"), big);
        write_file(at("cz/Src/other/new.txt"), big);
        REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
        REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
        CHECK(bs.files_copied == 2);
        snprintf(copy, sizeof(copy), "%s/Src/sub/new.txt", dir);
        CHECK(bd_fs_compressed(at(copy)) == 1);
        snprintf(copy, sizeof(copy), "%s/Src/other/new.txt", dir);
        CHECK(bd_fs_compressed(at(copy)) == 1);
        CHECK(file_equals_long(at(copy), big));
        bd_check_stats ck;
        REQUIRE_OK(bd_media_check(cat, drive, &ck, quiet, NULL), cat);
        CHECK(ck.copies == 4 && ck.ok == 4);
        /* And off again. */
        REQUIRE_OK(bd_media_set_compression(cat, drive, 0, &cs, quiet, NULL), cat);
        CHECK(cs.failed == 0 && bd_fs_compressed(at(copy)) == 0 && bd_fs_compressed(at(dir)) == 0);
        CHECK(file_equals_long(at(copy), big));
    }
    /* An encrypted drive never can: its copies are random data. */
    REQUIRE_OK(bd_catalog_set_passphrase(cat, "squeeze it please"), cat);
    int64_t vault;
    bd_mkdirs(at("cz/vault"));
    REQUIRE_OK(bd_media_init(cat, at("cz/vault"), "Vault", BD_MEDIA_ENCRYPTED, &vault), cat);
    REQUIRE_OK(bd_media_compression(cat, vault, &can, &on, &why), cat);
    CHECK(!can && why != NULL && on == 0);
    CHECK(bd_media_set_compression(cat, vault, 1, &cs, quiet, NULL) == BD_ERR_INVALID);
    /* A drive that is not plugged in. */
    bd_media_disconnect(cat, drive);
    CHECK(bd_media_compression(cat, drive, &can, &on, &why) == BD_ERR_NOT_FOUND);
    bd_catalog_close(cat);
    free(big);
}

static void test_case_rename(void)
{
    write_file(at("cr/Src/Photo.JPG"), "picture");
    write_file(at("cr/Src/Album/One.txt"), "one");
    bd_mkdirs(at("cr/drive"));
    bd_test_fold_case = 1;
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("cr/c.brodalf"), &cat), NULL);
    int64_t src, drive;
    REQUIRE_OK(bd_source_add(cat, at("cr/Src"), &src), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_media_init(cat, at("cr/drive"), "Case", 0, &drive), cat);
    bd_backup_stats bs;
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 2);
    int64_t photo = count_rows(cat, "SELECT id FROM nodes WHERE rel_path='Photo.JPG'");
    int64_t one = count_rows(cat, "SELECT id FROM nodes WHERE rel_path='Album/One.txt'");

    CHECK(rename(at("cr/Src/Photo.JPG"), at("cr/Src/photo.jpg")) == 0);
    CHECK(rename(at("cr/Src/Album"), at("cr/Src/album")) == 0);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_new == 0 && ss.files_deleted == 0 && ss.files_changed == 0 && ss.bytes_hashed == 0);
    CHECK(count_rows(cat, "SELECT id FROM nodes WHERE rel_path='photo.jpg' AND name='photo.jpg' AND deleted=0") == photo);
    CHECK(count_rows(cat, "SELECT id FROM nodes WHERE rel_path='album/One.txt' AND deleted=0") == one);
    CHECK(count_rows(cat, "SELECT COUNT(*) FROM nodes WHERE rel_path IN ('Photo.JPG','Album','Album/One.txt')") == 0);
    CHECK(count_rows(cat, "SELECT COUNT(*) FROM versions") == 2);
    /* Nothing to copy again; a restore uses the new names. */
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 0 && bs.files_considered == 0);
    bd_restore_stats rs;
    REQUIRE_OK(bd_restore(cat, src, "", at("cr/out"), &rs, quiet, NULL), cat);
    CHECK(rs.files_restored == 2);
    CHECK(file_equals(at("cr/out/photo.jpg"), "picture") && file_equals(at("cr/out/album/One.txt"), "one"));

    /* The lookup uses its index. */
    sqlite3_stmt *q;
    int indexed = 0;
    if (sqlite3_prepare_v2(cat->db, "EXPLAIN QUERY PLAN SELECT id FROM nodes WHERE source_id=1 AND rel_fold=? AND last_seen_ms<?",
                           -1, &q, NULL) == SQLITE_OK) {
        while (sqlite3_step(q) == SQLITE_ROW)
            if (strstr((const char *)sqlite3_column_text(q, 3), "nodes_fold")) indexed = 1;
        sqlite3_finalize(q);
    }
    CHECK(indexed);

#ifndef _WIN32
    /* Where names are case sensitive nothing changes: a delete and a new file. */
    bd_test_fold_case = 0;
    CHECK(rename(at("cr/Src/photo.jpg"), at("cr/Src/PHOTO.jpg")) == 0);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_moved == 1 && ss.files_new == 0 && ss.files_deleted == 0); /* the same file under a new name */
#endif
    bd_catalog_close(cat);

    /* A schema 5 catalog gets the keys when it is opened. */
    write_file(at("mg/Src/Notes.TXT"), "notes");
    REQUIRE_OK(bd_catalog_create(at("mg/c.brodalf"), &cat), NULL);
    REQUIRE_OK(bd_source_add(cat, at("mg/Src"), &src), cat);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(bd_exec(cat, "DROP INDEX nodes_fold; ALTER TABLE nodes DROP COLUMN rel_fold;"
                       " UPDATE meta SET value='5' WHERE key='schema_version'") == 0);
    REQUIRE_OK(bd_catalog_save(cat), cat);
    bd_catalog_close(cat);
    bd_test_fold_case = 1;
    REQUIRE_OK(bd_catalog_open(at("mg/c.brodalf"), &cat), NULL);
    CHECK(count_rows(cat, "SELECT COUNT(*) FROM nodes WHERE rel_fold IS NULL") == 0);
    CHECK(count_rows(cat, "SELECT value FROM meta WHERE key='schema_version'") == 8);
    CHECK(rename(at("mg/Src/Notes.TXT"), at("mg/Src/notes.txt")) == 0);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_new == 0 && ss.files_deleted == 0);
    bd_catalog_close(cat);
    bd_test_fold_case = 0;
}

/* A source folder called .versions (or BRODALF.media) must not land in
 * BRODALF's own entries on the drive. */
static void test_reserved_source_names(void)
{
    write_file(at("rv/.versions/a.txt"), "in a folder called .versions");
    write_file(at("rv/Docs/a.txt"), "doc v1");
    write_file(at("rv/brodalf.MEDIA/m.txt"), "media");
    bd_mkdirs(at("rv/drive"));
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("rv/c.brodalf"), &cat), NULL);
    int64_t vs, docs, drive;
    REQUIRE_OK(bd_source_add(cat, at("rv/.versions"), &vs), cat);
    REQUIRE_OK(bd_source_add(cat, at("rv/Docs"), &docs), cat);
    REQUIRE_OK(bd_source_add(cat, at("rv/brodalf.MEDIA"), NULL), cat);
    CHECK(count_rows(cat, "SELECT COUNT(*) FROM sources WHERE name IN ('_versions','_brodalf.MEDIA')") == 2);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_media_init(cat, at("rv/drive"), "Names", 0, &drive), cat);
    bd_backup_stats bs;
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 3);
    write_file(at("rv/Docs/a.txt"), "doc v2, longer");
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 1 && bs.copies_alongside == 1);
    char *root = bd_sprintf("rv/drive/BRODALF/%s", bd_catalog_uuid(cat));
    char *p1 = bd_sprintf("%s/_versions/a.txt", root), *p2 = bd_sprintf("%s/.versions/Docs/a.v2.txt", root);
    char *p3 = bd_sprintf("%s/_brodalf.MEDIA/m.txt", root);
    CHECK(file_equals(at(p1), "in a folder called .versions"));
    CHECK(file_equals(at(p2), "doc v2, longer"));
    CHECK(file_equals(at(p3), "media"));

    /* A catalog that already has such a name: the next backup gives the
     * source a name of its own and says so; its copies so far stay good. */
    CHECK(bd_exec(cat, "UPDATE sources SET name='.versions' WHERE name='_versions'") == 0);
    write_file(at("rv/.versions/b.txt"), "new file");
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    int said = 0;
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, rename_log, &said), cat);
    CHECK(said == 1 && bs.files_copied == 1);
    CHECK(count_rows(cat, "SELECT COUNT(*) FROM sources WHERE name='_versions'") == 1);
    char *p4 = bd_sprintf("%s/_versions/b.txt", root);
    CHECK(file_equals(at(p4), "new file"));
    bd_check_stats cs;
    REQUIRE_OK(bd_media_check(cat, drive, &cs, quiet, NULL), cat);
    CHECK(cs.copies == 5 && cs.ok == 5);
    free(root);
    free(p1);
    free(p2);
    free(p3);
    free(p4);
    bd_catalog_close(cat);
}

typedef struct { char path[8][1024]; int64_t id[8]; int n; } source_paths;

static int collect_sources(void *ctx, const bd_source_info *info)
{
    source_paths *sp = ctx;
    if (sp->n < 8) {
        snprintf(sp->path[sp->n], sizeof(sp->path[0]), "%s", info->path);
        sp->id[sp->n++] = info->source_id;
    }
    return 0;
}

/* A new PC: the catalog comes back from the copy on a drive, and a
 * protected folder that now lives elsewhere is pointed at its new place
 * without anything looking deleted or being copied again. */
static void test_recover_and_relocate(void)
{
    write_file(at("r/Users/old/Docs/a.txt"), "alpha");
    write_file(at("r/Users/old/Docs/sub/b.txt"), "beta");
    write_file(at("r/Other/c.txt"), "gamma");
    bd_mkdirs(at("r/d1"));
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("r/old.brodalf"), &cat), NULL);
    int64_t docs, other, d1;
    REQUIRE_OK(bd_source_add(cat, at("r/Users/old/Docs"), &docs), cat);
    REQUIRE_OK(bd_source_add(cat, at("r/Other"), &other), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_media_init(cat, at("r/d1"), "One", 0, &d1), cat);
    bd_backup_stats bs;
    REQUIRE_OK(bd_backup(cat, d1, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 3);
    REQUIRE_OK(bd_catalog_copy_to_media(cat, d1), cat);
    char uuid[64], backup[1200];
    snprintf(uuid, sizeof(uuid), "%s", bd_catalog_uuid(cat));
    snprintf(backup, sizeof(backup), "r/d1/BRODALF/%s/catalog-r%08lld.brodalf", uuid, (long long)bd_catalog_revision(cat));
    bd_catalog_close(cat);

    /* Copy it back as a new catalog; never over an existing file, never from something else. */
    CHECK(bd_catalog_recover(at(backup), at("r/new.brodalf")) == BD_OK);
    CHECK(bd_catalog_recover(at(backup), at("r/new.brodalf")) == BD_ERR_EXISTS);
    CHECK(bd_catalog_recover(at("r/Other/c.txt"), at("r/bad.brodalf")) == BD_ERR_FORMAT);
    CHECK(!exists(at("r/bad.brodalf")) && !exists(at("r/bad.brodalf.recovering")));
    CHECK(bd_catalog_recover(at("r/missing.brodalf"), at("r/bad.brodalf")) == BD_ERR_NOT_FOUND);
    CHECK(!exists(at("r/new.brodalf.recovering")));
    REQUIRE_OK(bd_catalog_open(at("r/new.brodalf"), &cat), NULL);
    CHECK(strcmp(bd_catalog_uuid(cat), uuid) == 0);
    int64_t found = 0;
    bd_check_stats cs;
    REQUIRE_OK(bd_media_connect(cat, at("r/d1"), &found, quiet, NULL), cat);
    REQUIRE_OK(bd_media_check(cat, found, &cs, quiet, NULL), cat);
    CHECK(found == d1 && cs.ok == 3 && cs.missing == 0);

    /* The user folder has another name here. Until it is pointed there a
     * scan skips it rather than calling its files deleted. */
    CHECK(bd_rename_noreplace(at("r/Users/old"), at("r/Users/new")) == 0);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_deleted == 0 && ss.roots_missing == 1);
    CHECK(bd_source_relocate(cat, docs, at("r/Users/nowhere")) == BD_ERR_NOT_FOUND);
    CHECK(bd_source_relocate(cat, docs, at("r/Other")) == BD_ERR_EXISTS);
    CHECK(bd_source_relocate(cat, 9999, at("r/Users/new/Docs")) == BD_ERR_NOT_FOUND);
    REQUIRE_OK(bd_source_relocate(cat, docs, at("r/Users/new/Docs/")), cat);
    REQUIRE_OK(bd_source_relocate(cat, docs, at("r/Users/new/Docs")), cat); /* same place again is fine */
    source_paths sp;
    memset(&sp, 0, sizeof(sp));
    bd_list_sources(cat, collect_sources, &sp);
    CHECK(sp.n == 2 && sp.id[0] == docs && strcmp(sp.path[0], at("r/Users/new/Docs")) == 0);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_new == 0 && ss.files_changed == 0 && ss.files_deleted == 0 && ss.errors == 0);
    find_ctx sub = lookup(cat, docs, 0, "sub");
    CHECK(lookup(cat, docs, sub.node_id, "b.txt").state == BD_STATE_AVAILABLE);
    REQUIRE_OK(bd_backup(cat, d1, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 0);
    bd_restore_stats rs;
    REQUIRE_OK(bd_restore(cat, docs, NULL, at("r/out"), &rs, quiet, NULL), cat);
    CHECK(rs.files_restored == 2 && file_equals(at("r/out/sub/b.txt"), "beta"));
    /* Both roots: each goes into a folder of its own name. */
    REQUIRE_OK(bd_restore(cat, 0, NULL, at("r/all"), &rs, quiet, NULL), cat);
    CHECK(rs.files_restored == 3 && file_equals(at("r/all/Docs/sub/b.txt"), "beta") && file_equals(at("r/all/Other/c.txt"), "gamma"));
    /* A root that is no longer backed up keeps its copies but is left out. */
    REQUIRE_OK(bd_source_set_active(cat, other, 0), cat);
    write_file(at("r/Other/d.txt"), "delta");
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.roots_stopped == 1 && ss.files_new == 0);
    REQUIRE_OK(bd_backup(cat, d1, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 0);
    REQUIRE_OK(bd_source_set_active(cat, other, 1), cat);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_new == 1);
    /* A skip list of the root's own. */
    REQUIRE_OK(bd_source_skip_set(cat, other, "d.txt\n"), cat);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.skipped == 1);
    {
        char *own = bd_source_skip_get(cat, other);
        CHECK(own && strcmp(own, "d.txt\n") == 0);
        free(own);
    }
    REQUIRE_OK(bd_catalog_save(cat), cat);
    bd_catalog_close(cat);
}

typedef struct { int64_t want, lost_ms; char why[16]; int64_t bytes_held; } lost_snap;

static int snap_lost(void *ctx, const bd_media_info *m)
{
    lost_snap *r = ctx;
    if (m->media_id != r->want) return 0;
    r->lost_ms = m->lost_ms;
    snprintf(r->why, sizeof(r->why), "%s", m->lost_why);
    r->bytes_held = m->bytes_held;
    return 1;
}

static lost_snap lost_of(bd_catalog *cat, int64_t id)
{
    lost_snap r = {id, -1, "", -1};
    bd_list_media(cat, snap_lost, &r);
    return r;
}

static int count_lost_copies(void *ctx, const bd_copy_info *c)
{
    if (c->media_lost) (*(int *)ctx)++;
    return 0;
}

static void sets_of(bd_catalog *cat, int64_t done[2])
{
    bd_set_progress sp[2];
    int n = 0;
    CHECK(bd_backup_sets(cat, sp, 2, &n) == BD_OK && n == 2);
    done[0] = sp[0].files_done;
    done[1] = sp[1].files_done;
}

/* A drive marked lost or broken: its copies stop counting, so the sets
 * drop, nothing more goes to it, and other drives take its place. */
static void test_lost_drive(void)
{
    write_file(at("x/S1/f1.txt"), "one");
    write_file(at("x/S1/f2.txt"), "two");
    write_file(at("x/S2/f3.txt"), "three");
    write_file(at("x/S2/f4.txt"), "four");
    bd_mkdirs(at("x/a"));
    bd_mkdirs(at("x/b"));
    bd_mkdirs(at("x/c"));
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("x/x.brodalf"), &cat), NULL);
    bd_target t = {2, 1};
    REQUIRE_OK(bd_target_set(cat, &t), cat);
    int64_t s1, s2, a, b, c;
    REQUIRE_OK(bd_source_add(cat, at("x/S1"), &s1), cat);
    REQUIRE_OK(bd_source_add(cat, at("x/S2"), &s2), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_media_init(cat, at("x/a"), "Old", 0, &a), cat);
    REQUIRE_OK(bd_media_init(cat, at("x/b"), "Other", 0, &b), cat);
    int64_t done[2];
    sets_of(cat, done);
    CHECK(done[0] == 0 && done[1] == 0);
    bd_backup_stats bs;
    REQUIRE_OK(bd_backup(cat, a, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 4);
    sets_of(cat, done);
    CHECK(done[0] == 4 && done[1] == 0);
    REQUIRE_OK(bd_backup(cat, b, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 4);
    sets_of(cat, done);
    CHECK(done[0] == 4 && done[1] == 4);
    risk_count rc;
    CHECK(risk_of(cat, &rc).files_at_risk == 0);
    CHECK(lost_of(cat, a).lost_ms == 0 && lost_of(cat, a).bytes_held == 3 + 3 + 5 + 4);

    /* Marked lost: the second set drops to nothing. */
    CHECK(bd_media_mark_lost(cat, a, "stolen") == BD_ERR_INVALID);
    REQUIRE_OK(bd_media_mark_lost(cat, a, "lost"), cat);
    lost_snap ls = lost_of(cat, a);
    CHECK(ls.lost_ms > 0 && strcmp(ls.why, "lost") == 0);
    sets_of(cat, done);
    CHECK(done[0] == 4 && done[1] == 0);
    CHECK(risk_of(cat, &rc).files_at_risk == 4);
    CHECK(bd_backup(cat, a, 0, &bs, quiet, NULL) == BD_ERR_INVALID);
    help_list h = help_of(cat);
    for (int i = 0; i < h.n; i++) CHECK(strcmp(h.labels[i], "Old") != 0);
    find_ctx f1 = lookup(cat, s1, 0, "f1.txt");
    int lost_copies = 0;
    bd_list_copies(cat, f1.node_id, count_lost_copies, &lost_copies);
    CHECK(lost_copies == 1);
    /* Any drive with room makes it whole again; no drive "replaces" another. */
    REQUIRE_OK(bd_media_init(cat, at("x/c"), "New", 0, &c), cat);
    REQUIRE_OK(bd_backup(cat, c, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 4);
    sets_of(cat, done);
    CHECK(done[0] == 4 && done[1] == 4);

    /* Found again: it counts again. Broken is recorded as such. */
    REQUIRE_OK(bd_media_found(cat, a), cat);
    CHECK(lost_of(cat, a).lost_ms == 0);
    REQUIRE_OK(bd_media_mark_lost(cat, b, "broken"), cat);
    CHECK(strcmp(lost_of(cat, b).why, "broken") == 0);
    sets_of(cat, done);
    CHECK(done[0] == 4 && done[1] == 4); /* Old and New */
    bd_catalog_close(cat);
}

typedef struct { const char *name; char on[256], plugged[256], older[256]; int64_t backed_up, mtime, subs; int found; } where_ctx;

static int find_where(void *ctx, const bd_node_info *n)
{
    where_ctx *w = ctx;
    if (strcmp(n->name, w->name) != 0) return 0;
    snprintf(w->on, sizeof(w->on), "%s", n->copies_on ? n->copies_on : "(null)");
    snprintf(w->plugged, sizeof(w->plugged), "%s", n->copies_on_plugged ? n->copies_on_plugged : "(null)");
    snprintf(w->older, sizeof(w->older), "%s", n->older_on ? n->older_on : "(null)");
    w->backed_up = n->files_backed_up;
    w->mtime = n->mtime_ns;
    w->subs = n->subfolders;
    w->found = 1;
    return 1;
}

static where_ctx where_of(bd_catalog *cat, int64_t source, int64_t parent, const char *name)
{
    where_ctx w;
    memset(&w, 0, sizeof(w));
    w.name = name;
    bd_list_children(cat, source, parent, find_where, &w);
    return w;
}

static int source_where(void *ctx, const bd_source_info *s)
{
    where_ctx *w = ctx;
    snprintf(w->on, sizeof(w->on), "%s", s->copies_on ? s->copies_on : "(null)");
    snprintf(w->plugged, sizeof(w->plugged), "%s", s->copies_on_plugged ? s->copies_on_plugged : "(null)");
    w->backed_up = s->files_backed_up;
    w->found = 1;
    return 1;
}

/* Where each file's copies are: the drives named for a file, a folder and
 * a whole root, which of them are plugged in, and where an older version
 * is when the newest has no copy. */
static void test_copies_on(void)
{
    write_file(at("co/Src/one.txt"), "one");
    write_file(at("co/Src/sub/two.txt"), "two");
    write_file(at("co/Src/sub/three.txt"), "three");
    write_file(at("co/Src/sub/inner/four.txt"), "four");
    bd_mkdirs(at("co/blue"));
    bd_mkdirs(at("co/red"));
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("co/c.brodalf"), &cat), NULL);
    int64_t src, blue, red;
    REQUIRE_OK(bd_source_add(cat, at("co/Src"), &src), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    where_ctx w = where_of(cat, src, 0, "one.txt");
    CHECK(w.found && strcmp(w.on, "") == 0 && strcmp(w.plugged, "") == 0 && strcmp(w.older, "") == 0 && w.mtime > 0 && w.subs == 0);
    w = where_of(cat, src, 0, "sub");
    CHECK(w.found && strcmp(w.on, "") == 0 && w.backed_up == 0 && w.subs == 1);

    REQUIRE_OK(bd_media_init(cat, at("co/red"), "Red", 0, &red), cat);
    REQUIRE_OK(bd_media_init(cat, at("co/blue"), "Blue", 0, &blue), cat);
    bd_backup_stats bs;
    REQUIRE_OK(bd_backup(cat, red, 0, &bs, quiet, NULL), cat);
    REQUIRE_OK(bd_backup(cat, blue, 0, &bs, quiet, NULL), cat);
    w = where_of(cat, src, 0, "one.txt");
    CHECK(strcmp(w.on, "Blue, Red") == 0 && strcmp(w.plugged, "Blue, Red") == 0 && strcmp(w.older, "") == 0);
    w = where_of(cat, src, 0, "sub");
    CHECK(strcmp(w.on, "Blue, Red") == 0 && w.backed_up == 3);
    where_ctx sw;
    memset(&sw, 0, sizeof(sw));
    bd_list_sources(cat, source_where, &sw);
    CHECK(sw.found && strcmp(sw.on, "Blue, Red") == 0 && sw.backed_up == 4);

    /* Red unplugged: still named, no longer plugged in. */
    bd_media_disconnect(cat, red);
    w = where_of(cat, src, 0, "one.txt");
    CHECK(strcmp(w.on, "Blue, Red") == 0 && strcmp(w.plugged, "Blue") == 0);
    memset(&sw, 0, sizeof(sw));
    bd_list_sources(cat, source_where, &sw);
    CHECK(strcmp(sw.plugged, "Blue") == 0);

    /* Changed since: no copy of the new version, an older one on both. */
    write_file(at("co/Src/one.txt"), "one, changed and longer");
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    w = where_of(cat, src, 0, "one.txt");
    CHECK(strcmp(w.on, "") == 0 && strcmp(w.older, "Blue, Red") == 0);
    memset(&sw, 0, sizeof(sw));
    bd_list_sources(cat, source_where, &sw);
    CHECK(sw.backed_up == 3);

    /* A drive marked lost no longer counts. */
    REQUIRE_OK(bd_media_mark_lost(cat, blue, "lost"), cat);
    w = where_of(cat, src, 0, "sub");
    CHECK(strcmp(w.on, "Red") == 0 && strcmp(w.plugged, "") == 0 && w.backed_up == 3);
    w = where_of(cat, src, 0, "one.txt");
    CHECK(strcmp(w.older, "Red") == 0);
    bd_catalog_close(cat);
}

typedef struct { int n; char first[256]; char drives[256]; int connected; int64_t last_seen; int backed_up; int64_t never_id; } deleted_list;

static int collect_deleted(void *ctx, const bd_deleted_info *d)
{
    deleted_list *l = ctx;
    if (!l->n++) {
        snprintf(l->first, sizeof(l->first), "%s/%s", d->source_name, d->rel_path);
        snprintf(l->drives, sizeof(l->drives), "%s", d->drives);
        l->connected = d->connected;
        l->last_seen = d->last_seen_ms;
    }
    l->backed_up += d->backed_up;
    if (strstr(d->rel_path, "never")) l->never_id = d->node_id;
    return 0;
}

/* Files deleted from their folder that a drive still holds, and getting
 * them back. */
static void test_deleted_files(void)
{
    write_file(at("del/D/a.txt"), "a");
    write_file(at("del/D/keep.txt"), "keep");
    write_file(at("del/D/sub/b.txt"), "b");
    write_file(at("del/D/sub/c.txt"), "c1");
    bd_mkdirs(at("del/one"));
    bd_mkdirs(at("del/two"));
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("del/d.brodalf"), &cat), NULL);
    int64_t src, one, two;
    REQUIRE_OK(bd_source_add(cat, at("del/D"), &src), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_media_init(cat, at("del/one"), "One", 0, &one), cat);
    REQUIRE_OK(bd_media_init(cat, at("del/two"), "Two", 0, &two), cat);
    bd_backup_stats bs;
    REQUIRE_OK(bd_backup(cat, one, 0, &bs, quiet, NULL), cat);
    write_file(at("del/D/sub/c.txt"), "c2, changed");
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_backup(cat, one, 0, &bs, quiet, NULL), cat);
    REQUIRE_OK(bd_backup(cat, two, 0, &bs, quiet, NULL), cat);
    bd_media_disconnect(cat, two);
    /* A file that never had a copy is listed too when it goes. */
    write_file(at("del/D/never.txt"), "never backed up");
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);

    int64_t count = -1;
    REQUIRE_OK(bd_list_deleted(cat, 0, 0, NULL, NULL, &count), cat);
    CHECK(count == 0);
    CHECK(bd_remove(at("del/D/a.txt")) == 0);
    CHECK(bd_remove(at("del/D/sub/b.txt")) == 0);
    CHECK(bd_remove(at("del/D/sub/c.txt")) == 0);
    CHECK(bd_rmdir_empty(at("del/D/sub")) == 0);
    CHECK(bd_remove(at("del/D/never.txt")) == 0);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_deleted == 4 && ss.deleted_no_copy == 1);
    deleted_list dl;
    memset(&dl, 0, sizeof(dl));
    REQUIRE_OK(bd_list_deleted(cat, src, 1, collect_deleted, &dl, &count), cat);
    CHECK(count == 4 && dl.n == 4 && dl.last_seen > 0 && dl.backed_up == 3);
    CHECK(strcmp(dl.first, "D/a.txt") == 0 && strcmp(dl.drives, "One, Two") == 0 && dl.connected);
    REQUIRE_OK(bd_list_deleted(cat, 9999, 0, NULL, NULL, &count), cat);
    CHECK(count == 0);

    /* The plan and the restore of a folder take only what was deleted from it. */
    bd_restore_opts ro;
    memset(&ro, 0, sizeof(ro));
    ro.deleted_only = 1;
    int64_t total = 0, none = 0;
    REQUIRE_OK(bd_restore_plan_ex(cat, src, "", NULL, &ro, NULL, NULL, &total, &none), cat);
    CHECK(total == 4 && none == 1); /* never.txt has no copy */
    bd_restore_stats rs;
    REQUIRE_OK(bd_restore_ex(cat, src, "sub", at("del/out"), &ro, &rs, quiet, NULL), cat);
    CHECK(rs.files_restored == 2 && rs.files_failed == 0);
    CHECK(file_equals(at("del/out/sub/b.txt"), "b") && file_equals(at("del/out/sub/c.txt"), "c2, changed"));
    CHECK(!exists(at("del/out/a.txt")) && !exists(at("del/out/keep.txt")));
    REQUIRE_OK(bd_restore_ex(cat, src, "a.txt", at("del/out"), &ro, &rs, quiet, NULL), cat);
    CHECK(rs.files_restored == 1 && file_equals(at("del/out/a.txt"), "a"));
    REQUIRE_OK(bd_restore_ex(cat, src, "", at("del/out"), &ro, &rs, quiet, NULL), cat);
    CHECK(rs.files_restored == 0 && rs.files_already_there == 3 && rs.files_no_copy == 1 && !exists(at("del/out/keep.txt")));

    /* Back in its folder, it is no longer deleted. */
    write_file(at("del/D/a.txt"), "a");
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_list_deleted(cat, 0, 0, NULL, NULL, &count), cat);
    CHECK(count == 3);
    bd_media_disconnect(cat, one);
    memset(&dl, 0, sizeof(dl));
    REQUIRE_OK(bd_list_deleted(cat, 0, 0, collect_deleted, &dl, NULL), cat);
    CHECK(dl.n == 3 && !dl.connected && dl.never_id > 0);

    /* Forgetting: only when asked, and only deleted files. The copies stay
     * on the drives. */
    int64_t forgotten = -1;
    CHECK(bd_deleted_forget(cat, lookup(cat, src, 0, "keep.txt").node_id, &forgotten) == BD_ERR_INVALID);
    REQUIRE_OK(bd_deleted_forget(cat, dl.never_id, &forgotten), cat);
    CHECK(forgotten == 1);
    REQUIRE_OK(bd_list_deleted(cat, 0, 0, NULL, NULL, &count), cat);
    CHECK(count == 2);
    REQUIRE_OK(bd_deleted_forget(cat, 0, &forgotten), cat);
    CHECK(forgotten == 2);
    REQUIRE_OK(bd_list_deleted(cat, 0, 0, NULL, NULL, &count), cat);
    CHECK(count == 0);
    char kept[1200];
    snprintf(kept, sizeof(kept), "del/two/BRODALF/%s/D/sub/b.txt", bd_catalog_uuid(cat));
    CHECK(file_equals(at(kept), "b"));
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_deleted == 0 && ss.files_new == 0);
    bd_catalog_close(cat);
}

typedef struct { int n; int64_t node_id, version_id; char rel[256]; int differ; } damaged_list;

static int collect_damaged(void *ctx, const bd_damaged_info *d)
{
    damaged_list *l = ctx;
    if (!l->n++) {
        l->node_id = d->node_id;
        snprintf(l->rel, sizeof(l->rel), "%s", d->rel_path);
        l->differ = strcmp(d->hash_on_record, d->hash_found) != 0;
        l->version_id = d->version_id;
    }
    return 0;
}

/* Change a byte of a file but keep its size and modified time, as bit rot
 * would. */
static void rot_byte(const char *path)
{
    bd_stat_t st;
    CHECK(bd_stat(path, &st) == 0);
    FILE *f = bd_fopen(path, "r+b");
    CHECK(f != NULL);
    if (!f) return;
    int c = fgetc(f);
    fseek(f, 0, SEEK_SET);
    fputc(c ^ 0x20, f);
    fflush(f);
    CHECK(bd_set_mtime(f, st.mtime_ns) == 0);
    fclose(f);
    bd_stat_t after;
    CHECK(bd_stat(path, &after) == 0 && after.size == st.size && after.mtime_ns == st.mtime_ns);
}

typedef struct { int n; int64_t first_size; } largest_list;

static int collect_largest(void *ctx, const bd_risk_info *r)
{
    largest_list *l = ctx;
    if (!l->n++) l->first_size = r->size;
    return 0;
}

/* The bit-rot scan, the largest files, the reserve and progress inside a
 * file. */
static void test_bitrot_and_sizes(void)
{
    write_file(at("br/S/a.txt"), "alpha alpha alpha");
    write_file(at("br/S/b.txt"), "beta");
    write_file(at("br/S/big.txt"), "a longer file than the others, by some way");
    bd_mkdirs(at("br/drive"));
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("br/c.brodalf"), &cat), NULL);
    int64_t src, drive;
    REQUIRE_OK(bd_source_add(cat, at("br/S"), &src), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_media_init(cat, at("br/drive"), "Rot", 0, &drive), cat);
    bd_backup_stats bs;
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 3);

    /* A plain scan trusts size and time; the bit-rot scan reads everything. */
    rot_byte(at("br/S/a.txt"));
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_changed == 0 && ss.files_damaged == 0);
    bd_scan_opts so = {1};
    progress_log pl;
    memset(&pl, 0, sizeof(pl));
    bd_catalog_set_progress(cat, on_progress, &pl);
    REQUIRE_OK(bd_scan_ex(cat, &so, &ss, quiet, NULL), cat);
    bd_catalog_set_progress(cat, NULL, NULL);
    CHECK(strcmp(pl.phase, "bit-rot scan") == 0);
    CHECK(ss.files_damaged == 1 && ss.files_changed == 0 && ss.bytes_hashed > 0);
    damaged_list dl;
    memset(&dl, 0, sizeof(dl));
    REQUIRE_OK(bd_list_damaged(cat, collect_damaged, &dl), cat);
    CHECK(dl.n == 1 && strcmp(dl.rel, "a.txt") == 0 && dl.differ);
    /* The damaged file is not taken as a new version: the backup holds the
     * good one, and restoring it brings the file back. */
    CHECK(count_rows(cat, "SELECT COUNT(*) FROM versions") == 3);
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 0);
    bd_restore_stats rs;
    REQUIRE_OK(bd_restore(cat, src, "a.txt", at("br/fixed"), &rs, quiet, NULL), cat);
    CHECK(rs.files_restored == 1 && file_equals(at("br/fixed/a.txt"), "alpha alpha alpha"));
    /* The list names the good version, which the window restores. */
    CHECK(dl.version_id > 0);
    REQUIRE_OK(bd_restore_version(cat, dl.version_id, at("br/good"), &rs, quiet, NULL), cat);
    CHECK(rs.files_restored == 1 && file_equals(at("br/good/a.txt"), "alpha alpha alpha"));
    /* Or the user says the change was theirs: it becomes a new version. */
    REQUIRE_OK(bd_damaged_accept(cat, dl.node_id), cat);
    memset(&dl, 0, sizeof(dl));
    REQUIRE_OK(bd_list_damaged(cat, collect_damaged, &dl), cat);
    CHECK(dl.n == 0);
    CHECK(count_rows(cat, "SELECT COUNT(*) FROM versions") == 4);
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 1 && bs.copies_alongside == 1);
    REQUIRE_OK(bd_scan_ex(cat, &so, &ss, quiet, NULL), cat);
    CHECK(ss.files_damaged == 0);

    /* Largest first. */
    largest_list ll = {0, 0};
    REQUIRE_OK(bd_list_largest(cat, 2, collect_largest, &ll), cat);
    CHECK(ll.n == 2 && ll.first_size == 42);

    /* The reserve and low priority are settings of the catalog. */
    CHECK(bd_option_get(cat, "reserve_mb") == 512 && bd_option_get(cat, "low_priority") == 1);
    REQUIRE_OK(bd_option_set(cat, "reserve_mb", 256), cat);
    REQUIRE_OK(bd_option_set(cat, "low_priority", 0), cat);
    CHECK(bd_option_get(cat, "reserve_mb") == 256 && bd_option_get(cat, "low_priority") == 0);
    CHECK(bd_option_set(cat, "low_priority", 2) == BD_ERR_INVALID);
    bd_backup_preview_info pv;
    REQUIRE_OK(bd_backup_preview(cat, drive, 0, &pv), cat);
    CHECK(pv.reserve_bytes == (int64_t)256 * 1024 * 1024);

    /* A changed file shows how far into it the copy is. */
    size_t len = (size_t)3 * 1024 * 1024 + 5;
    char *data = malloc(len);
    CHECK(data != NULL);
    if (data) {
        memset(data, 'x', len);
        FILE *bf = bd_fopen(at("br/S/huge.bin"), "wb");
        CHECK(bf && fwrite(data, 1, len, bf) == len);
        if (bf) fclose(bf);
        free(data);
    }
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    memset(&pl, 0, sizeof(pl));
    bd_catalog_set_progress(cat, on_progress, &pl);
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    bd_catalog_set_progress(cat, NULL, NULL);
    CHECK(bs.files_copied == 1);
    CHECK(pl.max_file_total == (int64_t)len);
    bd_catalog_close(cat);
}

static int count_conflict(void *ctx, const bd_conflict_info *c)
{
    /* Each says which version it was going to put there, so the window
     * can restore it somewhere else. */
    if (c->version_id > 0) (*(int *)ctx)++;
    return 0;
}

typedef struct {
    int64_t v1;
} version_pick;

static int pick_version(void *ctx, const bd_copy_info *c)
{
    version_pick *vp = ctx;
    if (c->version_no == 1) vp->v1 = c->version_id;
    return 0;
}

typedef struct {
    int active, missing;
} source_flags;

static int get_flags(void *ctx, const bd_source_info *info)
{
    source_flags *sf = ctx;
    sf->active = info->active;
    sf->missing = info->missing;
    return 1;
}

static int count_catalog_cb(void *ctx, const char *rel, const bd_stat_t *st)
{
    (void)st;
    if (strncmp(rel, "catalog-r", 9) == 0 && !strstr(rel, "tmp")) (*(int *)ctx)++;
    return strchr(rel, '/') ? 0 : (st->is_dir ? BD_WALK_SKIP : 0);
}

typedef struct { int n; int64_t revision; char id[64]; char path[1024]; } found_copy;

static int found_copy_cb(void *ctx, const bd_catalog_copy_info *info)
{
    found_copy *f = ctx;
    f->n++;
    f->revision = info->revision;
    snprintf(f->id, sizeof(f->id), "%s", info->catalog_id);
    snprintf(f->path, sizeof(f->path), "%s", info->path);
    return 0;
}

static int count_catalog_copies(const char *dir)
{
    int n = 0;
    bd_walk(dir, count_catalog_cb, NULL, &n);
    return n;
}

/* ---- 0.8.0: several drives at once, starred files, encrypted folders ---- */

typedef struct { int n; int64_t files; char rel[256]; } mark_list_t;

static int collect_mark(void *ctx, const bd_mark_info *m)
{
    mark_list_t *l = ctx;
    l->n++;
    l->files += m->files;
    snprintf(l->rel, sizeof(l->rel), "%s", m->rel_path);
    return 0;
}

typedef struct { int n; int starred, encrypt, backed_up, max_copies; } pending_list_t;

static int collect_pending(void *ctx, const bd_pending_info *p)
{
    pending_list_t *l = ctx;
    l->n++;
    l->starred += p->starred;
    l->encrypt += p->encrypt;
    l->backed_up += p->backed_up;
    if (p->copies > l->max_copies) l->max_copies = p->copies;
    return 0;
}

static int64_t pending_total(bd_catalog *cat, int64_t media_id, pending_list_t *l)
{
    int64_t total = -1;
    pending_list_t local;
    if (!l) l = &local;
    memset(l, 0, sizeof(*l));
    CHECK(bd_list_pending(cat, media_id, 0, collect_pending, l, &total) == BD_OK);
    CHECK(total == l->n);
    return total;
}

typedef struct { int64_t id; int secondary; int64_t limit; } place_snap;

static int snap_place(void *ctx, const bd_media_info *m)
{
    place_snap *p = ctx;
    if (m->media_id != p->id) return 0;
    p->secondary = m->secondary;
    p->limit = m->limit_bytes;
    return 1;
}

typedef struct { int sealed, plain; } sealed_count;

static int count_sealed(void *ctx, const bd_copy_info *c)
{
    sealed_count *s = ctx;
    if (!c->media_id || strcmp(c->copy_state, "ok") != 0) return 0;
    if (c->encrypted) s->sealed++;
    else s->plain++;
    return 0;
}

static sealed_count sealed_of(bd_catalog *cat, int64_t src, const char *rel)
{
    sealed_count s = {0, 0};
    int64_t node = 0;
    CHECK(bd_find_node(cat, src, rel, &node) == BD_OK);
    if (node) CHECK(bd_list_copies(cat, node, count_sealed, &s) == BD_OK);
    return s;
}

static void test_multi_copy(void)
{
    size_t len = (size_t)6 * 1024 * 1024 + 777;
    unsigned char *data = malloc(len);
    CHECK(data != NULL);
    if (!data) return;
    for (size_t i = 0; i < len; i++) data[i] = (unsigned char)((i * 2246822519u) >> 11);
    bd_mkdirs(at("multi"));
    FILE *f = bd_fopen(at("multi/src.bin"), "wb");
    CHECK(f && fwrite(data, 1, len, f) == len);
    if (f) fclose(f);
    free(data);
    char want[BD_HASH_HEX_LEN + 1], got[BD_HASH_HEX_LEN + 1], back[BD_HASH_HEX_LEN + 1];
    CHECK(bd_hash_file(at("multi/src.bin"), NULL, want, NULL) == 0);
    uint8_t key[32];
    CHECK(bd_random_bytes(key, sizeof(key)) == 0);

    /* Big enough for the writer threads: three copies, the middle one sealed. */
    write_file(at("multi/ro.bin"), "");
    FILE *outs[3] = {bd_fopen(at("multi/a.bin"), "wb"), bd_fopen(at("multi/b.bdenc"), "wb"), bd_fopen(at("multi/c.bin"), "wb")};
    const uint8_t *keys[3] = {NULL, key, NULL};
    int64_t sizes[3];
    int failed[3];
    CHECK(outs[0] && outs[1] && outs[2]);
    CHECK(bd_hash_copy_multi(at("multi/src.bin"), outs, keys, 3, got, sizes, failed) == 0);
    for (int i = 0; i < 3; i++) if (outs[i]) fclose(outs[i]);
    CHECK(strcmp(got, want) == 0);
    CHECK(!failed[0] && !failed[1] && !failed[2] && sizes[0] == (int64_t)len && sizes[2] == (int64_t)len);
    CHECK(bd_hash_file(at("multi/a.bin"), NULL, back, NULL) == 0 && strcmp(back, want) == 0);
    CHECK(bd_hash_file(at("multi/c.bin"), NULL, back, NULL) == 0 && strcmp(back, want) == 0);
    CHECK(bd_hash_copy(at("multi/b.bdenc"), key, NULL, NULL, back, NULL) == 0 && strcmp(back, want) == 0);

    /* A copy that cannot be written (opened for reading only) fails alone. */
    FILE *bad[2] = {bd_fopen(at("multi/d.bin"), "wb"), bd_fopen(at("multi/ro.bin"), "rb")};
    const uint8_t *nokeys[2] = {NULL, NULL};
    CHECK(bad[0] && bad[1]);
    CHECK(bd_hash_copy_multi(at("multi/src.bin"), bad, nokeys, 2, got, sizes, failed) == 0);
    for (int i = 0; i < 2; i++) if (bad[i]) fclose(bad[i]);
    CHECK(!failed[0] && failed[1]);
    CHECK(bd_hash_file(at("multi/d.bin"), NULL, back, NULL) == 0 && strcmp(back, want) == 0);
    /* All fail: -2. A missing file: -1. */
    FILE *both[2] = {bd_fopen(at("multi/ro.bin"), "rb"), bd_fopen(at("multi/ro.bin"), "rb")};
    CHECK(bd_hash_copy_multi(at("multi/src.bin"), both, nokeys, 2, got, sizes, failed) == -2);
    for (int i = 0; i < 2; i++) if (both[i]) fclose(both[i]);
    FILE *more[2] = {bd_fopen(at("multi/e.bin"), "wb"), bd_fopen(at("multi/f.bin"), "wb")};
    CHECK(bd_hash_copy_multi(at("multi/none.bin"), more, nokeys, 2, got, sizes, failed) == -1);
    for (int i = 0; i < 2; i++) if (more[i]) fclose(more[i]);
    CHECK(bd_crypto_speed() > 0);
}

static void test_backup_many(void)
{
    bd_mkdirs(at("bm/d1"));
    bd_mkdirs(at("bm/d2"));
    write_file(at("bm/src/a.txt"), "alpha");
    write_file(at("bm/src/sub/b.txt"), "bravo");
    /* One big file, so the copy runs on writer threads. */
    size_t len = (size_t)5 * 1024 * 1024 + 99;
    char *big = malloc(len + 1);
    CHECK(big != NULL);
    if (!big) return;
    for (size_t i = 0; i < len; i++) big[i] = (char)('a' + (i * 7919) % 26);
    big[len] = '\0';
    write_file(at("bm/src/big.dat"), big);
    free(big);

    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("bm/c.brodalf"), &cat), NULL);
    int64_t src, d1, d2;
    REQUIRE_OK(bd_source_add(cat, at("bm/src"), &src), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_media_init(cat, at("bm/d1"), "Drive one", 0, &d1), cat);
    REQUIRE_OK(bd_media_init(cat, at("bm/d2"), "Drive two", 0, &d2), cat);
    CHECK(bd_option_get(cat, "parallel_copies") == 0);
    REQUIRE_OK(bd_option_set(cat, "parallel_copies", 1), cat);
    CHECK(bd_option_get(cat, "parallel_copies") == 1);

    int64_t both[2] = {d1, d2}, twice[2] = {d1, d1};
    bd_backup_stats bs[2];
    CHECK(bd_backup_many(cat, twice, 2, 0, bs, quiet, NULL) == BD_ERR_INVALID);
    REQUIRE_OK(bd_backup_many(cat, both, 2, 0, bs, quiet, NULL), cat);
    CHECK(bs[0].files_copied == 3 && bs[1].files_copied == 3);
    CHECK(bs[0].files_failed == 0 && bs[1].files_failed == 0);
    CHECK(bs[0].bytes_copied == bs[1].bytes_copied && bs[0].bytes_copied == (int64_t)len + 10);
    bd_check_stats cs;
    REQUIRE_OK(bd_media_check(cat, d1, &cs, quiet, NULL), cat);
    CHECK(cs.copies == 3 && cs.ok == 3);
    REQUIRE_OK(bd_media_check(cat, d2, &cs, quiet, NULL), cat);
    CHECK(cs.copies == 3 && cs.ok == 3);
    /* Each drive got the catalog too. */
    for (int k = 0; k < 2; k++) {
        found_copy fc;
        memset(&fc, 0, sizeof(fc));
        CHECK(bd_find_catalog_copies(at(k ? "bm/d2" : "bm/d1"), found_copy_cb, &fc) == BD_OK && fc.n >= 1);
    }

    /* One new file; a drive that has everything else only gets it. */
    write_file(at("bm/src/c.txt"), "charlie");
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    bd_mkdirs(at("bm/d3"));
    int64_t d3;
    REQUIRE_OK(bd_media_init(cat, at("bm/d3"), "Drive three", 0, &d3), cat);
    int64_t three[3] = {d1, d2, d3};
    bd_backup_stats b3[3];
    REQUIRE_OK(bd_backup_many(cat, three, 3, 0, b3, quiet, NULL), cat);
    CHECK(b3[0].files_copied == 1 && b3[1].files_copied == 1 && b3[2].files_copied == 4);
    CHECK(b3[0].files_considered == 1 && b3[2].files_considered == 4);

    /* Every file now has three copies: the target is met. */
    bd_target t = {3, 1};
    REQUIRE_OK(bd_target_set(cat, &t), cat);
    CHECK(pending_total(cat, 0, NULL) == 0);
    t.copies = 4;
    REQUIRE_OK(bd_target_set(cat, &t), cat);
    pending_list_t pl;
    CHECK(pending_total(cat, 0, &pl) == 4 && pl.max_copies == 3 && pl.backed_up == 0);
    CHECK(pending_total(cat, d1, NULL) == 0);

    /* A drive that is not plugged in stops the whole job before it starts. */
    bd_media_disconnect(cat, d2);
    write_file(at("bm/src/d.txt"), "delta");
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(bd_backup_many(cat, both, 2, 0, bs, quiet, NULL) != BD_OK);
    REQUIRE_OK(bd_media_check(cat, d1, &cs, quiet, NULL), cat);
    CHECK(cs.copies == 4);
    bd_catalog_close(cat);
}

static void test_stars_and_places(void)
{
    write_file(at("st/src/docs/a.txt"), "twenty bytes of text");
    write_file(at("st/src/docs/sub/b.txt"), "twenty more bytes ok");
    write_file(at("st/src/photos/p.jpg"), "a photo that is not starred");
    write_file(at("st/src/notes.txt"), "notes");
    bd_mkdirs(at("st/drive"));
    bd_mkdirs(at("st/Dropbox"));

    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("st/c.brodalf"), &cat), NULL);
    int64_t src, drive, place;
    REQUIRE_OK(bd_source_add(cat, at("st/src"), &src), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_media_init(cat, at("st/drive"), "Main drive", 0, &drive), cat);

    /* A place needs a real description. */
    CHECK(bd_place_add(cat, at("st/Dropbox"), "Dropbox", 0, 0, NULL) == BD_ERR_INVALID);
    CHECK(bd_place_add(cat, at("st/Dropbox"), "my files", 0, 0, NULL) == BD_ERR_INVALID);
    CHECK(bd_place_add(cat, at("st/Dropbox"), "Dropbox folder on the laptop", -1, 0, NULL) == BD_ERR_INVALID);
    REQUIRE_OK(bd_place_add(cat, at("st/Dropbox"), "Dropbox folder on the laptop", 30, 0, &place), cat);
    place_snap ps = {place, 0, 0};
    bd_list_media(cat, snap_place, &ps);
    CHECK(ps.secondary == 1 && ps.limit == 30);
    CHECK(bd_place_set_limit(cat, drive, 10) == BD_ERR_INVALID); /* a drive is not a place */
    char mf_path[1200];
    snprintf(mf_path, sizeof(mf_path), "st/Dropbox/BRODALF/%s/BRODALF.media", bd_catalog_uuid(cat));
    bd_media_file mf;
    CHECK(bd_media_file_read(at(mf_path), &mf) == 0 && mf.secondary == 1);

    /* Nothing starred: nothing goes there. */
    bd_backup_stats bs;
    REQUIRE_OK(bd_backup(cat, place, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 0 && bs.files_considered == 0);

    CHECK(bd_star_set(cat, src, "nope", 1) == BD_ERR_NOT_FOUND);
    REQUIRE_OK(bd_star_set(cat, src, "docs/", 1), cat);
    REQUIRE_OK(bd_star_set(cat, src, "docs", 1), cat); /* twice is fine */
    CHECK(bd_starred(cat, src, "docs") == 1);
    CHECK(bd_starred(cat, src, "docs/sub/b.txt") == 2);
    CHECK(bd_starred(cat, src, "docsx/a.txt") == 0);
    CHECK(bd_starred(cat, src, "notes.txt") == 0);
    mark_list_t ml;
    memset(&ml, 0, sizeof(ml));
    REQUIRE_OK(bd_list_stars(cat, collect_mark, &ml), cat);
    CHECK(ml.n == 1 && ml.files == 2 && strcmp(ml.rel, "docs") == 0);
    int64_t sf = 0, sb = 0;
    REQUIRE_OK(bd_starred_totals(cat, &sf, &sb), cat);
    CHECK(sf == 2 && sb == 40);

    /* The limit lets one of the two starred files in. */
    bd_place_status pst;
    REQUIRE_OK(bd_place_status_get(cat, place, &pst), cat);
    CHECK(pst.starred_files == 2 && pst.waiting_files == 1 && pst.over_limit_files == 1 && pst.have_files == 0);
    pending_list_t pl;
    CHECK(pending_total(cat, place, &pl) == 2 && pl.starred == 2);
    bd_backup_preview_info pv;
    REQUIRE_OK(bd_backup_preview(cat, place, 0, &pv), cat);
    CHECK(pv.files_to_copy == 2);
    REQUIRE_OK(bd_backup(cat, place, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 1 && bs.files_no_room == 1 && bs.files_considered == 2);
    char p1[1200];
    snprintf(p1, sizeof(p1), "st/Dropbox/BRODALF/%s/src/docs/a.txt", bd_catalog_uuid(cat));
    CHECK(file_equals(at(p1), "twenty bytes of text"));
    REQUIRE_OK(bd_place_status_get(cat, place, &pst), cat);
    CHECK(pst.have_files == 1 && pst.used_bytes == 20 && pst.over_limit_files == 1 && pst.waiting_files == 0);

    /* Copies in a place never count toward the copies you want. */
    bd_target t = {1, 1};
    REQUIRE_OK(bd_target_set(cat, &t), cat);
    CHECK(pending_total(cat, 0, NULL) == 4);
    bd_set_progress sp[2];
    int sets = 0;
    REQUIRE_OK(bd_backup_sets(cat, sp, 2, &sets), cat);
    CHECK(sets == 1 && sp[0].files_done == 0);
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 4);
    CHECK(pending_total(cat, 0, NULL) == 0);
    REQUIRE_OK(bd_backup_sets(cat, sp, 2, &sets), cat);
    CHECK(sp[0].files_done == 4);
    /* And no drive suggestion points at the place. */
    help_list hl = help_of(cat);
    for (int i = 0; i < hl.n; i++) CHECK(strcmp(hl.labels[i], "Dropbox folder on the laptop") != 0);

    /* With the drive away, the starred file comes back from the place. */
    bd_media_disconnect(cat, drive);
    bd_restore_stats rs;
    REQUIRE_OK(bd_restore(cat, src, "docs", at("st/out"), &rs, quiet, NULL), cat);
    CHECK(rs.files_restored == 1 && rs.files_offline == 1);
    CHECK(file_equals(at("st/out/docs/a.txt"), "twenty bytes of text"));
    REQUIRE_OK(bd_media_connect(cat, at("st/drive"), &drive, quiet, NULL), cat);

    /* No limit: the other one goes too. */
    REQUIRE_OK(bd_place_set_limit(cat, place, 0), cat);
    REQUIRE_OK(bd_backup(cat, place, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 1 && bs.files_no_room == 0);

    /* A star on a file moves with it when it is renamed. */
    REQUIRE_OK(bd_star_set(cat, src, "notes.txt", 1), cat);
    CHECK(rename(at("st/src/notes.txt"), at("st/src/notes-renamed.txt")) == 0);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_moved == 1);
    CHECK(bd_starred(cat, src, "notes-renamed.txt") == 1 && bd_starred(cat, src, "notes.txt") == 0);
    REQUIRE_OK(bd_backup(cat, place, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 1);
    /* Unstarred files stay where they are, and nothing new goes there. */
    REQUIRE_OK(bd_star_set(cat, src, "docs", 0), cat);
    write_file(at("st/src/docs/new.txt"), "new");
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_backup(cat, place, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 0);
    CHECK(file_equals(at(p1), "twenty bytes of text"));

    /* The place survives a save and reopen as a place. */
    REQUIRE_OK(bd_catalog_save(cat), cat);
    bd_catalog_close(cat);
    REQUIRE_OK(bd_catalog_open(at("st/c.brodalf"), &cat), NULL);
    ps.secondary = 0;
    bd_list_media(cat, snap_place, &ps);
    CHECK(ps.secondary == 1 && ps.limit == 0);
    /* A catalog that lost the place (an older copy of it) finds it again
     * as a place. */
    CHECK(bd_exec(cat, "DELETE FROM copies WHERE media_id IN (SELECT id FROM media WHERE secondary<>0);"
                       " DELETE FROM media WHERE secondary<>0") == 0);
    int64_t again = 0;
    REQUIRE_OK(bd_media_connect(cat, at("st/Dropbox"), &again, quiet, NULL), cat);
    ps.id = again;
    ps.secondary = 0;
    bd_list_media(cat, snap_place, &ps);
    CHECK(ps.secondary == 1);
    bd_catalog_close(cat);

    /* An extra place inside a backup root: its copies are not scanned as
     * new files of that root. */
    bd_mkdirs(at("st2/home/Dropbox"));
    write_file(at("st2/home/mine.txt"), "my own file");
    REQUIRE_OK(bd_catalog_create(at("st2/c.brodalf"), &cat), NULL);
    REQUIRE_OK(bd_source_add(cat, at("st2/home"), &src), cat);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_place_add(cat, at("st2/home/Dropbox"), "Dropbox folder in my home folder", 0, 0, &place), cat);
    REQUIRE_OK(bd_star_set(cat, src, "mine.txt", 1), cat);
    REQUIRE_OK(bd_backup(cat, place, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 1);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_new == 0 && ss.files_seen == 1);
    /* Another folder named BRODALF is the user's own and is scanned. */
    write_file(at("st2/home/BRODALF/readme.txt"), "not ours");
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_new == 1);
    bd_catalog_close(cat);
}

static void test_encrypt_folders(void)
{
    write_file(at("en/src/secret/a.txt"), "the secret text");
    write_file(at("en/src/open/b.txt"), "open text");
    bd_mkdirs(at("en/drive"));

    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("en/c.brodalf"), &cat), NULL);
    int64_t src, drive;
    REQUIRE_OK(bd_source_add(cat, at("en/src"), &src), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_media_init(cat, at("en/drive"), "Plain drive", 0, &drive), cat);

    CHECK(bd_encrypt_path_set(cat, src, "secret", 1) == BD_ERR_PASSPHRASE);
    REQUIRE_OK(bd_catalog_set_passphrase(cat, "a passphrase for the test"), cat);
    REQUIRE_OK(bd_encrypt_path_set(cat, src, "secret", 1), cat);
    CHECK(bd_encrypt_marked(cat, src, "secret/a.txt") == 2 && bd_encrypt_marked(cat, src, "open/b.txt") == 0);
    mark_list_t ml;
    memset(&ml, 0, sizeof(ml));
    REQUIRE_OK(bd_list_encrypt_paths(cat, collect_mark, &ml), cat);
    CHECK(ml.n == 1 && ml.files == 1);
    bd_encrypt_stats es;
    REQUIRE_OK(bd_encrypt_stats_get(cat, &es), cat);
    CHECK(es.files == 1 && es.files_waiting == 1 && es.sealed_copies == 0 && es.bytes_per_second > 0);
    pending_list_t pl;
    CHECK(pending_total(cat, drive, &pl) == 2 && pl.encrypt == 1);

    bd_backup_stats bs;
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 2 && bs.files_failed == 0);
    char sealed_path[1200], plain_path[1200];
    snprintf(sealed_path, sizeof(sealed_path), "en/drive/BRODALF/%s/src/secret/a.txt" BD_SEALED_SUFFIX, bd_catalog_uuid(cat));
    snprintf(plain_path, sizeof(plain_path), "en/drive/BRODALF/%s/src/open/b.txt", bd_catalog_uuid(cat));
    CHECK(exists(at(sealed_path)) && !file_equals(at(sealed_path), "the secret text"));
    CHECK(file_equals(at(plain_path), "open text"));
    sealed_count sc = sealed_of(cat, src, "secret/a.txt");
    CHECK(sc.sealed == 1 && sc.plain == 0);
    sc = sealed_of(cat, src, "open/b.txt");
    CHECK(sc.sealed == 0 && sc.plain == 1);
    REQUIRE_OK(bd_encrypt_stats_get(cat, &es), cat);
    CHECK(es.sealed_copies == 1 && es.files_waiting == 0 && es.plain_copies == 0);

    /* Locked: the plain copy is checked, the sealed one skipped. */
    bd_catalog_lock_key(cat);
    bd_check_stats cs;
    REQUIRE_OK(bd_media_check(cat, drive, &cs, quiet, NULL), cat);
    CHECK(cs.copies == 2 && cs.ok == 1 && cs.skipped == 1);
    bd_restore_stats rs;
    REQUIRE_OK(bd_restore(cat, src, "", at("en/out1"), &rs, quiet, NULL), cat);
    CHECK(rs.files_restored == 1 && rs.files_need_passphrase == 1);
    /* A new file outside the encrypted folder backs up while locked; one
     * inside needs the passphrase. */
    write_file(at("en/src/open/c.txt"), "more open text");
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 1);
    write_file(at("en/src/secret/d.txt"), "another secret");
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(bd_backup(cat, drive, 0, &bs, quiet, NULL) == BD_ERR_PASSPHRASE);

    REQUIRE_OK(bd_catalog_unlock(cat, "a passphrase for the test"), cat);
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 1);
    REQUIRE_OK(bd_media_check(cat, drive, &cs, quiet, NULL), cat);
    CHECK(cs.copies == 4 && cs.ok == 4 && cs.skipped == 0);
    REQUIRE_OK(bd_restore(cat, src, "secret", at("en/out2"), &rs, quiet, NULL), cat);
    CHECK(rs.files_restored == 2);
    CHECK(file_equals(at("en/out2/secret/a.txt"), "the secret text"));

    /* A folder set later keeps its plain copies; its next version is sealed. */
    REQUIRE_OK(bd_encrypt_path_set(cat, src, "open", 1), cat);
    REQUIRE_OK(bd_encrypt_stats_get(cat, &es), cat);
    CHECK(es.files == 4 && es.plain_copies == 2 && es.files_waiting == 2);
    write_file(at("en/src/open/b.txt"), "open text, changed");
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 1);
    CHECK(file_equals(at(plain_path), "open text"));
    sc = sealed_of(cat, src, "open/b.txt");
    CHECK(sc.sealed == 1);
    REQUIRE_OK(bd_encrypt_path_set(cat, src, "open", 0), cat);
    CHECK(bd_encrypt_marked(cat, src, "open/b.txt") == 0);

    /* An older catalog's copies on an encrypted drive are marked sealed on opening. */
    CHECK(bd_exec(cat, "UPDATE media SET encrypted=1; UPDATE copies SET encrypted=0") == 0);
    REQUIRE_OK(bd_catalog_save(cat), cat);
    bd_catalog_close(cat);
    REQUIRE_OK(bd_catalog_open_with(at("en/c.brodalf"), "a passphrase for the test", &cat), NULL);
    CHECK(count_rows(cat, "SELECT COUNT(*) FROM copies WHERE encrypted=0") == 0);
    bd_catalog_close(cat);
}

/* What the elevated shadow-copy helper accepts: only a list whose hash
 * matches, and only files inside the backup roots. */
static void test_shadow_list(void)
{
    const char *folders[] = {"C:\\Users\\me\\Documents", "D:/Mail/", "E:\\"};
    CHECK(bd_shadow_path_allowed("C:\\Users\\me\\Documents\\Outlook.pst", folders, 3));
    CHECK(bd_shadow_path_allowed("c:/users/ME/documents/sub/a.db", folders, 3));
    CHECK(bd_shadow_path_allowed("D:\\Mail\\x.pst", folders, 3));
    CHECK(bd_shadow_path_allowed("E:\\anything.txt", folders, 3));
    CHECK(!bd_shadow_path_allowed("C:\\Users\\me\\Documents", folders, 3));          /* the folder itself */
    CHECK(!bd_shadow_path_allowed("C:\\Users\\me\\Documents2\\a.pst", folders, 3));  /* only a prefix */
    CHECK(!bd_shadow_path_allowed("C:\\Users\\me\\Documents\\..\\..\\..\\Windows\\System32\\config\\SAM", folders, 3));
    CHECK(!bd_shadow_path_allowed("C:\\Users\\me\\Documents\\.\\a.pst", folders, 3));
    CHECK(!bd_shadow_path_allowed("C:\\Users\\me\\Documents\\. .\\a.pst", folders, 3));
    CHECK(!bd_shadow_path_allowed("C:\\Users\\me\\Documents\\a.pst:secret", folders, 3));
    CHECK(!bd_shadow_path_allowed("\\\\?\\C:\\Users\\me\\Documents\\a.pst", folders, 3));
    CHECK(!bd_shadow_path_allowed("\\\\server\\share\\a.pst", folders, 3));
    CHECK(!bd_shadow_path_allowed("Documents\\a.pst", folders, 3));
    CHECK(!bd_shadow_path_allowed("C:\\Windows\\System32\\config\\SAM", folders, 3));
    CHECK(!bd_shadow_path_allowed("C:\\Users\\me\\Documents\\a.pst", folders, 0));

    const char *files[] = {"C:\\Users\\me\\Documents\\Outlook.pst", "C:\\Windows\\System32\\config\\SAM"};
    char hash[65];
    char *text = bd_shadow_list_format("S-1-5-21-1-2-3-1001", folders, 3, files, 2, hash);
    CHECK(text != NULL && strlen(hash) == 64);
    if (!text) return;
    bd_shadow_list l;
    CHECK(bd_shadow_list_parse(text, strlen(text), hash, &l) == 0);
    CHECK(l.user_sid && strcmp(l.user_sid, "S-1-5-21-1-2-3-1001") == 0);
    CHECK(l.n_folders == 3 && strcmp(l.folders[1], "D:/Mail/") == 0);
    CHECK(l.n_files == 2 && strcmp(l.files[1], "C:\\Windows\\System32\\config\\SAM") == 0);
    CHECK(bd_shadow_path_allowed(l.files[0], (const char *const *)l.folders, l.n_folders));
    CHECK(!bd_shadow_path_allowed(l.files[1], (const char *const *)l.folders, l.n_folders));
    bd_shadow_list_free(&l);
    /* Changed after the hash was taken (as on the helper's command line). */
    char *tampered = bd_strdup(text);
    tampered[strlen(tampered) - 2] = 'X';
    CHECK(bd_shadow_list_parse(tampered, strlen(tampered), hash, &l) == -2);
    CHECK(bd_shadow_list_parse(text, strlen(text), "00", &l) == -2);
    CHECK(bd_shadow_list_parse(text, strlen(text), NULL, &l) == -2);
    free(tampered);
    const char *bad[] = {"C:\\a\nfile\tC:\\Windows\\win.ini"};
    CHECK(bd_shadow_list_format("S-1-5-21-1", folders, 3, bad, 1, hash) == NULL);
    free(text);

    /* The helper refuses a list whose hash does not match, before any copy. */
    write_file(at("shadow/list.txt"), "BRODALF shadow-copy list 1\nuser\tS-1-5-21-1\nfile\tC:\\x\n");
    char err[256];
    CHECK(bd_shadow_helper(at("shadow/out"), at("shadow/list.txt"), hash, err, sizeof(err)) == -1);
    CHECK(strstr(err, "changed") != NULL);
    CHECK(!exists(at("shadow/out")));
}

/* Keeps every log line, to look for one. */
typedef struct {
    char text[16384];
    size_t len;
} log_keep;

static void keep_log(void *ctx, const char *msg)
{
    log_keep *k = ctx;
    int n = snprintf(k->text + k->len, sizeof(k->text) - k->len, "%s\n", msg);
    if (n > 0) k->len += (size_t)n < sizeof(k->text) - k->len ? (size_t)n : sizeof(k->text) - k->len - 1;
}

/* Takes the drive away (renames its folder) from the progress callback,
 * as if it were unplugged halfway through a job. */
typedef struct {
    bd_catalog *cat;
    const char *phase, *from, *to;
    int64_t after_files;
    int fired, moved;
} vanish_at;

static void vanish_progress(void *ctx, const bd_progress *p)
{
    vanish_at *v = ctx;
    v->cat->progress_last_ms = 0; /* report every file */
    if (v->fired || p->done || strcmp(p->phase, v->phase) != 0 || p->files_done < v->after_files) return;
    v->fired = 1;
    v->moved = bd_rename_noreplace(v->from, v->to) == 0;
}

/* A drive that drops out is not a drive full of missing copies, and a
 * copy marked missing that is there again counts again. */
static void test_drive_gone(void)
{
    char name[64], text[64];
    for (int i = 0; i < 12; i++) {
        snprintf(name, sizeof(name), "dg/Src/f%02d.txt", i);
        snprintf(text, sizeof(text), "file %d", i);
        write_file(at(name), text);
    }
    bd_mkdirs(at("dg/drive"));
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("dg/c.brodalf"), &cat), NULL);
    int64_t src, drive;
    REQUIRE_OK(bd_source_add(cat, at("dg/Src"), &src), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_media_init(cat, at("dg/drive"), "Goes away", 0, &drive), cat);
    bd_backup_stats bs;
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 12);
    char dir[1200], path[1300];
    snprintf(dir, sizeof(dir), "dg/drive/BRODALF/%s", bd_catalog_uuid(cat));

    /* Unplugged halfway through a check, then a read-back: the check
     * stops, keeps what it found good, and marks nothing missing. */
    vanish_at va;
    bd_check_stats cs;
    for (int pass = 0; pass < 2; pass++) {
        memset(&va, 0, sizeof(va));
        va.cat = cat;
        va.phase = pass ? "verify" : "check";
        va.from = at("dg/drive");
        va.to = at("dg/drive-away");
        va.after_files = 5;
        bd_catalog_set_progress(cat, vanish_progress, &va);
        bd_status st = pass ? bd_media_verify(cat, drive, 0, &cs, quiet, NULL) : bd_media_check(cat, drive, &cs, quiet, NULL);
        bd_catalog_set_progress(cat, NULL, NULL);
        CHECK(va.fired && va.moved);
        CHECK(st == BD_ERR_DRIVE_GONE);
        CHECK(cs.missing == 0 && cs.bad == 0 && cs.ok > 0 && cs.ok < 12);
        CHECK(count_rows(cat, "SELECT COUNT(*) FROM copies WHERE state<>'ok'") == 0);
        CHECK(count_rows(cat, pass ? "SELECT COUNT(*) FROM jobs WHERE kind='read back' AND status='failed'"
                                   : "SELECT COUNT(*) FROM jobs WHERE kind='check' AND status='failed'") == 1);
        CHECK(bd_rename_noreplace(at("dg/drive-away"), at("dg/drive")) == 0);
    }
    REQUIRE_OK(bd_media_check(cat, drive, &cs, quiet, NULL), cat);
    CHECK(cs.copies == 12 && cs.ok == 12);

    /* Gone before a backup starts: it does not start, and nothing is
     * written where the drive was. */
    CHECK(bd_rename_noreplace(at("dg/drive"), at("dg/drive-away")) == 0);
    CHECK(bd_backup(cat, drive, 0, &bs, quiet, NULL) == BD_ERR_DRIVE_GONE);
    CHECK(!exists(at("dg/drive")));
    CHECK(bd_rename_noreplace(at("dg/drive-away"), at("dg/drive")) == 0);

    /* Every file changes, and the drive goes halfway through the backup:
     * the older copies it can no longer see are not marked missing, and
     * no folder is made where the drive was. */
    for (int i = 0; i < 12; i++) {
        snprintf(name, sizeof(name), "dg/Src/f%02d.txt", i);
        snprintf(text, sizeof(text), "file %d, version two", i);
        write_file(at(name), text);
    }
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_changed == 12);
    memset(&va, 0, sizeof(va));
    va.cat = cat;
    va.phase = "backup";
    va.from = at("dg/drive");
    va.to = at("dg/drive-away");
    va.after_files = 5;
    bd_catalog_set_progress(cat, vanish_progress, &va);
    bd_status st = bd_backup(cat, drive, 0, &bs, quiet, NULL);
    bd_catalog_set_progress(cat, NULL, NULL);
    CHECK(va.fired && va.moved);
    CHECK(st == BD_ERR_DRIVE_GONE);
    CHECK(bs.files_copied < 12);
    CHECK(count_rows(cat, "SELECT COUNT(*) FROM copies WHERE state<>'ok'") == 0);
    CHECK(count_rows(cat, "SELECT COUNT(*) FROM copies") == 12 + bs.files_copied);
    CHECK(count_rows(cat, "SELECT COUNT(*) FROM jobs WHERE kind='backup' AND status='failed'") == 1);
    CHECK(!exists(at("dg/drive")));
    CHECK(bd_rename_noreplace(at("dg/drive-away"), at("dg/drive")) == 0);
    int64_t first = bs.files_copied;
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(first + bs.files_copied == 12 && bs.files_failed == 0);
    check_drive_consistent(cat, drive, "dg/drive");

    /* A copy goes missing and comes back: a read-back of what is due
     * looks at it again, finds it as it was, and it counts again. */
    snprintf(path, sizeof(path), "%s/Src/f00.txt", dir);
    CHECK(bd_rename_noreplace(at(path), at("dg/f00.keep")) == 0);
    REQUIRE_OK(bd_media_check(cat, drive, &cs, quiet, NULL), cat);
    CHECK(cs.missing == 1);
    CHECK(bd_rename_noreplace(at("dg/f00.keep"), at(path)) == 0);
    REQUIRE_OK(bd_media_verify(cat, drive, 30, &cs, quiet, NULL), cat);
    CHECK(cs.copies == 1 && cs.ok == 1);
    CHECK(count_rows(cat, "SELECT COUNT(*) FROM copies WHERE state<>'ok'") == 0);

    /* The same for the newest version's copy, found by the next backup:
     * it counts again and no second copy is written next to it. */
    snprintf(path, sizeof(path), "%s/.versions/Src/f01.v2.txt", dir);
    CHECK(file_equals(at(path), "file 1, version two"));
    CHECK(bd_rename_noreplace(at(path), at("dg/f01.keep")) == 0);
    REQUIRE_OK(bd_media_check(cat, drive, &cs, quiet, NULL), cat);
    CHECK(cs.missing == 1);
    CHECK(bd_rename_noreplace(at("dg/f01.keep"), at(path)) == 0);
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 0 && bs.files_already_there == 1 && bs.files_failed == 0);
    snprintf(path, sizeof(path), "%s/.versions/Src/f01.v2-2.txt", dir);
    CHECK(!exists(at(path)));
    CHECK(count_rows(cat, "SELECT COUNT(*) FROM copies WHERE state<>'ok'") == 0);
    CHECK(count_rows(cat, "SELECT COUNT(*) FROM copies") == 24);
    check_drive_consistent(cat, drive, "dg/drive");

    /* An older version's copy comes back while the file has changed: the
     * new version goes alongside and the old copy counts again. */
    write_file(at("dg/Src/g.txt"), "g, version one");
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 1);
    snprintf(path, sizeof(path), "%s/Src/g.txt", dir);
    CHECK(bd_rename_noreplace(at(path), at("dg/g.keep")) == 0);
    REQUIRE_OK(bd_media_check(cat, drive, &cs, quiet, NULL), cat);
    CHECK(cs.missing == 1);
    CHECK(bd_rename_noreplace(at("dg/g.keep"), at(path)) == 0);
    write_file(at("dg/Src/g.txt"), "g, version two");
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    log_keep lk;
    memset(&lk, 0, sizeof(lk));
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, keep_log, &lk), cat);
    CHECK(bs.files_copied == 1 && bs.copies_alongside == 1);
    CHECK(strstr(lk.text, "counts again: Src/g.txt") != NULL);
    CHECK(count_rows(cat, "SELECT COUNT(*) FROM copies WHERE state<>'ok'") == 0);
    check_drive_consistent(cat, drive, "dg/drive");
    bd_catalog_close(cat);
}

static int64_t copy_state_count(bd_catalog *cat, int64_t media_id, const char *name, const char *state)
{
    char sql[512];
    snprintf(sql, sizeof(sql),
             "SELECT COUNT(*) FROM copies c JOIN versions v ON v.id=c.version_id JOIN nodes n ON n.id=v.node_id"
             " WHERE c.media_id=%lld AND n.name='%s' AND c.state='%s'",
             (long long)media_id, name, state);
    return count_rows(cat, sql);
}

/* A restore reads the next good copy when the first one it tries is
 * damaged or missing, and marks that one. */
static void test_restore_fallback(void)
{
    write_file(at("rf/Src/a.txt"), "the first file");
    write_file(at("rf/Src/b.txt"), "the second file");
    write_file(at("rf/Src/c.txt"), "the third file");
    write_file(at("rf/Src/d.txt"), "the fourth file");
    bd_mkdirs(at("rf/d1"));
    bd_mkdirs(at("rf/d2"));
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("rf/c.brodalf"), &cat), NULL);
    int64_t src, d1, d2;
    REQUIRE_OK(bd_source_add(cat, at("rf/Src"), &src), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_media_init(cat, at("rf/d1"), "Drive One", 0, &d1), cat);
    REQUIRE_OK(bd_media_init(cat, at("rf/d2"), "Drive Two", 0, &d2), cat);
    bd_backup_stats bs;
    REQUIRE_OK(bd_backup(cat, d1, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 4);
    REQUIRE_OK(bd_backup(cat, d2, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 4);
    char dir1[1200], path[1300];
    snprintf(dir1, sizeof(dir1), "rf/d1/BRODALF/%s/Src", bd_catalog_uuid(cat));

    /* The drive read from goes halfway through: the restore stops, keeps
     * what it restored, and marks nothing missing. */
    vanish_at va;
    memset(&va, 0, sizeof(va));
    va.cat = cat;
    va.phase = "restore";
    va.from = at("rf/d1");
    va.to = at("rf/d1-away");
    va.after_files = 2;
    bd_catalog_set_progress(cat, vanish_progress, &va);
    bd_restore_stats rs;
    bd_status st = bd_restore(cat, 0, NULL, at("rf/out1"), &rs, quiet, NULL);
    bd_catalog_set_progress(cat, NULL, NULL);
    CHECK(va.fired && va.moved);
    CHECK(st == BD_ERR_DRIVE_GONE && strstr(bd_catalog_error(cat), "Drive One") != NULL);
    CHECK(rs.files_restored == 2 && rs.files_failed == 0);
    CHECK(count_rows(cat, "SELECT COUNT(*) FROM copies WHERE state<>'ok'") == 0);
    CHECK(bd_rename_noreplace(at("rf/d1-away"), at("rf/d1")) == 0);

    /* On the drive tried first, a's copy rots (same size, other bytes) and
     * b's is lost. The restore reads them from the second drive. */
    snprintf(path, sizeof(path), "%s/a.txt", dir1);
    write_file(at(path), "THE first file");
    snprintf(path, sizeof(path), "%s/b.txt", dir1);
    CHECK(bd_remove(at(path)) == 0);
    log_keep lk;
    memset(&lk, 0, sizeof(lk));
    REQUIRE_OK(bd_restore(cat, 0, NULL, at("rf/out2"), &rs, keep_log, &lk), cat);
    CHECK(rs.files_restored == 4 && rs.files_failed == 0);
    CHECK(file_equals(at("rf/out2/a.txt"), "the first file"));
    CHECK(file_equals(at("rf/out2/b.txt"), "the second file"));
    CHECK(file_equals(at("rf/out2/c.txt"), "the third file"));
    CHECK(strstr(lk.text, "the copy on \"Drive One\" is damaged") != NULL);
    CHECK(strstr(lk.text, "the copy on \"Drive One\" is missing") != NULL);
    CHECK(strstr(lk.text, "from the copy on \"Drive Two\"") != NULL);
    CHECK(copy_state_count(cat, d1, "a.txt", "bad") == 1);
    CHECK(copy_state_count(cat, d1, "b.txt", "missing") == 1);
    CHECK(copy_state_count(cat, d2, "a.txt", "ok") == 1 && copy_state_count(cat, d2, "b.txt", "ok") == 1);
    CHECK(count_rows(cat, "SELECT COUNT(*) FROM copies WHERE state<>'ok'") == 2);

    /* One version, the same way: c's copy on the first drive rots too. */
    snprintf(path, sizeof(path), "%s/c.txt", dir1);
    write_file(at(path), "THE third file");
    int64_t c_version = count_rows(cat, "SELECT current_version_id FROM nodes WHERE name='c.txt'");
    REQUIRE_OK(bd_restore_version(cat, c_version, at("rf/out3"), &rs, quiet, NULL), cat);
    CHECK(rs.files_restored == 1 && rs.files_failed == 0);
    CHECK(file_equals(at("rf/out3/c.txt"), "the third file"));
    CHECK(copy_state_count(cat, d1, "c.txt", "bad") == 1);

    /* No good copy anywhere: not restored, nothing written. */
    snprintf(path, sizeof(path), "rf/d2/BRODALF/%s/Src/a.txt", bd_catalog_uuid(cat));
    write_file(at(path), "THE first file");
    REQUIRE_OK(bd_restore(cat, 0, "a.txt", at("rf/out4"), &rs, quiet, NULL), cat);
    CHECK(rs.files_restored == 0 && rs.files_failed == 1);
    CHECK(!exists(at("rf/out4/a.txt")));
    CHECK(copy_state_count(cat, d2, "a.txt", "bad") == 1);
    bd_catalog_close(cat);
}

/* Ransomware: every file is overwritten after the backup and the scan
 * records the new contents. The restore brings back what was backed up. */
static void test_restore_older_version(void)
{
    char name[64], text[64];
    for (int i = 0; i < 5; i++) {
        snprintf(name, sizeof(name), "ro/Src/f%d.txt", i);
        snprintf(text, sizeof(text), "my file %d", i);
        write_file(at(name), text);
    }
    bd_mkdirs(at("ro/drive"));
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("ro/c.brodalf"), &cat), NULL);
    int64_t src, drive;
    REQUIRE_OK(bd_source_add(cat, at("ro/Src"), &src), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_media_init(cat, at("ro/drive"), "Safe", 0, &drive), cat);
    bd_backup_stats bs;
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 5);

    for (int i = 0; i < 5; i++) {
        snprintf(name, sizeof(name), "ro/Src/f%d.txt", i);
        snprintf(text, sizeof(text), "ENCRYPTED GARBAGE %d", i);
        write_file(at(name), text);
    }
    write_file(at("ro/Src/new.txt"), "made after the backup");
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_changed == 5 && ss.files_new == 1);

    /* The plan asks for the drive with the old versions. */
    int64_t plan_total = 0, plan_no_copy = 0;
    REQUIRE_OK(bd_restore_plan(cat, 0, NULL, NULL, NULL, NULL, &plan_total, &plan_no_copy), cat);
    CHECK(plan_total == 6 && plan_no_copy == 1);

    log_keep lk;
    memset(&lk, 0, sizeof(lk));
    bd_restore_stats rs;
    REQUIRE_OK(bd_restore(cat, 0, NULL, at("ro/out"), &rs, keep_log, &lk), cat);
    CHECK(rs.files_restored == 5 && rs.files_older_version == 5);
    CHECK(rs.files_no_copy == 1 && rs.files_failed == 0 && rs.files_offline == 0);
    for (int i = 0; i < 5; i++) {
        snprintf(name, sizeof(name), "ro/out/f%d.txt", i);
        snprintf(text, sizeof(text), "my file %d", i);
        CHECK(file_equals(at(name), text));
    }
    CHECK(!exists(at("ro/out/new.txt")));
    CHECK(strstr(lk.text, "f3.txt: restored the version from ") != NULL);
    CHECK(strstr(lk.text, "the newer one was never backed up") != NULL);
    /* Again: already there, not counted again. */
    REQUIRE_OK(bd_restore(cat, 0, NULL, at("ro/out"), &rs, quiet, NULL), cat);
    CHECK(rs.files_restored == 0 && rs.files_already_there == 5 && rs.files_older_version == 0);

    /* Unplugged, the old versions are on a drive that is not connected,
     * not never backed up. */
    bd_media_disconnect(cat, drive);
    REQUIRE_OK(bd_restore(cat, 0, NULL, at("ro/out-offline"), &rs, quiet, NULL), cat);
    CHECK(rs.files_offline == 5 && rs.files_no_copy == 1 && rs.files_restored == 0);
    bd_catalog_close(cat);
}

/* A catalog recovered from a drive may hold any path: a restore never
 * writes outside the folder chosen, nor reads outside a drive's folder. */
static void test_unsafe_paths(void)
{
    const char *both_ok[] = {"a.txt", "sub/b.txt", ".versions/Src/a.v2.txt", "a b/c d", "x..y", ".hidden"};
    const char *both_bad[] = {"", "/etc/passwd", "../x", "a/../b", "a/./b", "./a", "a//b", "a/", "..", ".", "a/.."};
    const char *win_bad[] = {"a:b", "C:/x", "a\\b", "..\\x", "sub/.. /x", "...", "a/ . /b"};
    for (size_t i = 0; i < sizeof(both_ok) / sizeof(both_ok[0]); i++)
        CHECK(bd_stored_path_safe_as(both_ok[i], 0) && bd_stored_path_safe_as(both_ok[i], 1));
    for (size_t i = 0; i < sizeof(both_bad) / sizeof(both_bad[0]); i++)
        CHECK(!bd_stored_path_safe_as(both_bad[i], 0) && !bd_stored_path_safe_as(both_bad[i], 1));
    for (size_t i = 0; i < sizeof(win_bad) / sizeof(win_bad[0]); i++)
        CHECK(bd_stored_path_safe_as(win_bad[i], 0) && !bd_stored_path_safe_as(win_bad[i], 1));
    CHECK(!bd_stored_path_safe_as(NULL, 0));
    CHECK(bd_stored_name_safe("Docs") && !bd_stored_name_safe("a/b") && !bd_stored_name_safe(".."));

    write_file(at("up/Src/a.txt"), "alpha");
    write_file(at("up/Src/b.txt"), "beta");
    write_file(at("up/Src/c.txt"), "gamma");
    bd_mkdirs(at("up/d1"));
    bd_mkdirs(at("up/d2"));
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("up/c.brodalf"), &cat), NULL);
    int64_t src, d1, d2;
    REQUIRE_OK(bd_source_add(cat, at("up/Src"), &src), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_media_init(cat, at("up/d1"), "One", 0, &d1), cat);
    bd_backup_stats bs;
    REQUIRE_OK(bd_backup(cat, d1, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 3);
    int64_t unsafe = -1;
    REQUIRE_OK(bd_check_stored_paths(cat, quiet, NULL, &unsafe), cat);
    CHECK(unsafe == 0);

    /* What a doctored catalog could say: a file whose path climbs out of
     * the restore folder, and a copy recorded outside its drive folder. */
    CHECK(bd_exec(cat, "UPDATE nodes SET rel_path='../escape.txt' WHERE name='a.txt'") == 0);
    CHECK(bd_exec(cat, "UPDATE copies SET path_on_media='../../../c.brodalf' WHERE path_on_media='Src/b.txt'") == 0);
    log_keep lk;
    memset(&lk, 0, sizeof(lk));
    REQUIRE_OK(bd_check_stored_paths(cat, keep_log, &lk, &unsafe), cat);
    CHECK(unsafe == 2);
    CHECK(strstr(lk.text, "Src/../escape.txt") != NULL && strstr(lk.text, "../../../c.brodalf") != NULL);

    bd_restore_stats rs;
    memset(&lk, 0, sizeof(lk));
    REQUIRE_OK(bd_restore(cat, 0, NULL, at("up/out/inner"), &rs, keep_log, &lk), cat);
    CHECK(rs.files_restored == 1 && rs.files_failed == 2);
    CHECK(file_equals(at("up/out/inner/c.txt"), "gamma"));
    CHECK(!exists(at("up/out/escape.txt")) && !exists(at("up/out/inner/b.txt")));
    CHECK(strstr(lk.text, "not restored: the path stored for \"Src/../escape.txt\"") != NULL);
    /* The copy was not read, so it is not marked damaged either. */
    CHECK(count_rows(cat, "SELECT COUNT(*) FROM copies WHERE state<>'ok'") == 0);
    int64_t a_version = count_rows(cat, "SELECT current_version_id FROM nodes WHERE name='a.txt'");
    REQUIRE_OK(bd_restore_version(cat, a_version, at("up/out/one"), &rs, quiet, NULL), cat);
    CHECK(rs.files_restored == 0 && rs.files_failed == 1);

    /* Nor is such a file backed up to a new drive. */
    REQUIRE_OK(bd_media_init(cat, at("up/d2"), "Two", 0, &d2), cat);
    REQUIRE_OK(bd_backup(cat, d2, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 2 && bs.files_failed == 1);
    char p[1200];
    snprintf(p, sizeof(p), "up/d2/BRODALF/%s/escape.txt", bd_catalog_uuid(cat));
    CHECK(!exists(at(p)));
    bd_catalog_close(cat);
}

typedef struct { int64_t want; int check_due, found; } due_ctx;

static int get_due(void *ctx, const bd_media_info *m)
{
    due_ctx *d = ctx;
    if (m->media_id != d->want) return 0;
    d->check_due = m->check_due;
    d->found = 1;
    return 1;
}

static int drive_due(bd_catalog *cat, int64_t media_id)
{
    due_ctx d = {media_id, 0, 0};
    bd_list_media(cat, get_due, &d);
    CHECK(d.found);
    return d.check_due;
}

typedef struct { int copies, read_back; } read_back_count;

static int count_read_back(void *ctx, const bd_copy_info *c)
{
    read_back_count *r = ctx;
    if (!c->media_id) return 0;
    r->copies++;
    r->read_back += c->read_back;
    return 0;
}

/* Copies never read back since they were written: listed as such, read
 * back on their own on request, and due for a check after a week. */
static void test_unread_copies(void)
{
    write_file(at("ur/Src/a.txt"), "alpha");
    write_file(at("ur/Src/b.txt"), "beta");
    bd_mkdirs(at("ur/drive"));
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("ur/c.brodalf"), &cat), NULL);
    int64_t src, drive;
    REQUIRE_OK(bd_source_add(cat, at("ur/Src"), &src), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_media_init(cat, at("ur/drive"), "Fresh", 0, &drive), cat);
    bd_backup_stats bs;
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 2);
    find_ctx a = lookup(cat, src, 0, "a.txt");
    read_back_count rb = {0, 0};
    REQUIRE_OK(bd_list_copies(cat, a.node_id, count_read_back, &rb), cat);
    CHECK(rb.copies == 1 && rb.read_back == 0);
    CHECK(!drive_due(cat, drive));

    /* Written eight days ago and never read back: due, though the drive's
     * usual interval (check_days, 180) is far off. */
    CHECK(bd_exec(cat, "UPDATE copies SET written_ms=written_ms-8*24*3600*1000") == 0);
    CHECK(drive_due(cat, drive));

    /* Reading back only those copies. */
    bd_check_stats cs;
    REQUIRE_OK(bd_media_verify_unread(cat, drive, &cs, quiet, NULL), cat);
    CHECK(cs.copies == 2 && cs.rehashed == 2 && cs.ok == 2);
    rb.copies = rb.read_back = 0;
    REQUIRE_OK(bd_list_copies(cat, a.node_id, count_read_back, &rb), cat);
    CHECK(rb.copies == 1 && rb.read_back == 1);
    CHECK(!drive_due(cat, drive));
    REQUIRE_OK(bd_media_verify_unread(cat, drive, &cs, quiet, NULL), cat);
    CHECK(cs.copies == 0);

    /* A new copy is the only one read back next time. */
    write_file(at("ur/Src/c.txt"), "gamma");
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 1);
    REQUIRE_OK(bd_media_verify_unread(cat, drive, &cs, quiet, NULL), cat);
    CHECK(cs.copies == 1 && cs.ok == 1);

    /* check_days 0 means never due, unread copies or not. */
    CHECK(bd_exec(cat, "UPDATE copies SET last_full_check_ms=NULL, written_ms=written_ms-30*24*3600*1000") == 0);
    CHECK(drive_due(cat, drive));
    REQUIRE_OK(bd_option_set(cat, "check_days", 0), cat);
    CHECK(!drive_due(cat, drive));
    /* A shorter interval than a week wins. */
    REQUIRE_OK(bd_option_set(cat, "check_days", 3), cat);
    CHECK(bd_exec(cat, "UPDATE copies SET written_ms=strftime('%s','now')*1000-4*24*3600*1000") == 0);
    CHECK(drive_due(cat, drive));
    bd_catalog_close(cat);
}

int main(void)
{
    char tmp[512], id[37];
    bd_temp_dir(tmp, sizeof(tmp));
    bd_uuid_v4(id);
    snprintf(base, sizeof(base), "%s/brodalf-test-%.8s", tmp, id);
    for (char *p = base; *p; p++) if (*p == '\\') *p = '/';
    bd_mkdirs(base);
    printf("working in %s\n", base);

    write_file(at("src/Photos/a.txt"), "hello");
    write_file(at("src/Photos/sub/b.txt"), "world");
    bd_mkdirs(at("drive"));

    /* New catalog, one source, first scan. */
    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("test.brodalf"), &cat), NULL);
    int64_t src;
    REQUIRE_OK(bd_source_add(cat, at("src/Photos"), &src), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_new == 2);
    CHECK(ss.dirs_seen == 1);

    /* Everything starts greyed out. */
    CHECK(lookup(cat, src, 0, "a.txt").state == BD_STATE_NO_COPY);
    CHECK(lookup(cat, src, 0, "sub").state == BD_STATE_NO_COPY);
    CHECK(totals(cat).state == BD_STATE_NO_COPY && totals(cat).total == 2);

    /* A second window cannot open the same catalog. */
    bd_catalog *other = NULL;
    CHECK(bd_catalog_open(at("test.brodalf"), &other) == BD_ERR_LOCKED);

    /* Set up a drive and back up. */
    int64_t drive;
    REQUIRE_OK(bd_media_init(cat, at("drive"), "Test Drive", 0, &drive), cat);
    CHECK(bd_media_init(cat, at("drive"), "Again", 0, NULL) == BD_ERR_EXISTS);
    bd_backup_stats bs;
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 2);
    CHECK(bs.files_failed == 0);
    char drive_dir[1024];
    snprintf(drive_dir, sizeof(drive_dir), "drive/BRODALF/%s", bd_catalog_uuid(cat));
    char p1[1200];
    snprintf(p1, sizeof(p1), "%s/Photos/a.txt", drive_dir);
    CHECK(file_equals(at(p1), "hello"));
    CHECK(lookup(cat, src, 0, "a.txt").state == BD_STATE_AVAILABLE);
    CHECK(lookup(cat, src, 0, "sub").state == BD_STATE_AVAILABLE);

    /* Unplug: files grey out and name the drive to plug in. */
    bd_media_disconnect(cat, drive);
    find_ctx a = lookup(cat, src, 0, "a.txt");
    CHECK(a.state == BD_STATE_OFFLINE);
    CHECK(strcmp(a.label, "Test Drive") == 0);
    CHECK(lookup(cat, src, 0, "sub").state == BD_STATE_OFFLINE);
    CHECK(totals(cat).state == BD_STATE_OFFLINE && strcmp(totals(cat).label, "Test Drive") == 0);

    /* The catalog survives a save and reopen. */
    REQUIRE_OK(bd_catalog_save(cat), cat);
    bd_catalog_close(cat);
    REQUIRE_OK(bd_catalog_open(at("test.brodalf"), &cat), NULL);
    bd_check_stats cs;
    REQUIRE_OK(bd_media_connect(cat, at("drive"), &drive, quiet, NULL), cat);
    REQUIRE_OK(bd_media_check(cat, drive, &cs, quiet, NULL), cat);
    CHECK(cs.copies == 2 && cs.ok == 2 && cs.rehashed == 2);
    CHECK(lookup(cat, src, 0, "a.txt").state == BD_STATE_AVAILABLE);

    /* Change a file: only the old version is reachable until the next backup. */
    write_file(at("src/Photos/a.txt"), "hello, version two");
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_changed == 1);
    a = lookup(cat, src, 0, "a.txt");
    CHECK(a.state == BD_STATE_AVAILABLE_OLDER);
    CHECK(a.version_no == 2);
    CHECK(totals(cat).state == BD_STATE_PARTIAL && totals(cat).available == 1);
    /* The new version is written next to the old one, which is never
     * touched: BRODALF only adds to a drive. */
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 1);
    CHECK(bs.copies_alongside == 1);
    CHECK(file_equals(at(p1), "hello"));
    char pv[1200];
    snprintf(pv, sizeof(pv), "%s/.versions/Photos/a.v2.txt", drive_dir);
    CHECK(file_equals(at(pv), "hello, version two"));
    int copies = 0;
    bd_list_copies(cat, a.node_id, count_copies, &copies);
    CHECK(copies == 2);
    CHECK(lookup(cat, src, 0, "a.txt").state == BD_STATE_AVAILABLE);

    /* Lose a copy on the drive: the check notices it. */
    char pb[1200];
    snprintf(pb, sizeof(pb), "%s/Photos/sub/b.txt", drive_dir);
    bd_remove(at(pb));
    REQUIRE_OK(bd_media_check(cat, drive, &cs, quiet, NULL), cat);
    CHECK(cs.missing == 1);
    find_ctx sub = lookup(cat, src, 0, "sub");
    CHECK(sub.state == BD_STATE_NO_COPY || sub.state == BD_STATE_PARTIAL || sub.state == BD_STATE_OFFLINE);
    CHECK(lookup(cat, src, sub.node_id, "b.txt").state == BD_STATE_BAD);

    /* Bit rot: the bytes are wrong but the size and modified time are just
     * what the catalog recorded. Every check reads the copy back, so it is
     * caught. */
    {
        bd_stat_t before, after;
        CHECK(bd_stat(at(pv), &before) == 0);
        FILE *rot = bd_fopen(at(pv), "r+b");
        CHECK(rot != NULL);
        if (rot) { fputc('H', rot); fclose(rot); }
        CHECK(bd_stat(at(pv), &after) == 0);
        sqlite3_stmt *fix;
        CHECK(sqlite3_prepare_v2(cat->db, "UPDATE copies SET stored_mtime_ns=? WHERE path_on_media LIKE '.versions/%'", -1, &fix, NULL) == SQLITE_OK);
        sqlite3_bind_int64(fix, 1, after.mtime_ns);
        CHECK(sqlite3_step(fix) == SQLITE_DONE);
        sqlite3_finalize(fix);
        REQUIRE_OK(bd_media_check(cat, drive, &cs, quiet, NULL), cat);
        CHECK(cs.bad == 1);
        CHECK(lookup(cat, src, 0, "a.txt").state == BD_STATE_AVAILABLE_OLDER);
        /* The right bytes come back: the next check clears the mark. */
        write_file(at(pv), "hello, version two");
        REQUIRE_OK(bd_media_check(cat, drive, &cs, quiet, NULL), cat);
        CHECK(cs.bad == 0);
        CHECK(lookup(cat, src, 0, "a.txt").state == BD_STATE_AVAILABLE);
    }

    /* The next backup writes the missing copy again. */
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 1);
    CHECK(file_equals(at(pb), "world"));
    CHECK(lookup(cat, src, sub.node_id, "b.txt").state == BD_STATE_AVAILABLE);

    /* The copy of b vanishes, then b changes: the new version goes next to
     * where the old one was, and the old one stays recorded as missing. */
    bd_remove(at(pb));
    write_file(at("src/Photos/sub/b.txt"), "world, version two");
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 1);
    CHECK(bs.files_failed == 0);
    CHECK(!exists(at(pb)));
    CHECK(lookup(cat, src, sub.node_id, "b.txt").state == BD_STATE_AVAILABLE);

    /* Restore everything: with one backup root its files go straight into
     * the chosen folder. */
    bd_restore_stats rs;
    REQUIRE_OK(bd_restore(cat, 0, NULL, at("restore"), &rs, quiet, NULL), cat);
    CHECK(rs.files_restored == 2);
    CHECK(file_equals(at("restore/a.txt"), "hello, version two"));
    CHECK(file_equals(at("restore/sub/b.txt"), "world, version two"));
    /* Again: the files are already there. */
    REQUIRE_OK(bd_restore(cat, 0, NULL, at("restore"), &rs, quiet, NULL), cat);
    CHECK(rs.files_restored == 0 && rs.files_already_there == 2 && rs.files_conflict == 0);
    /* A different file at the destination is never overwritten: it is
     * skipped and listed. */
    write_file(at("restore/a.txt"), "someone else's a");
    REQUIRE_OK(bd_restore(cat, 0, NULL, at("restore"), &rs, quiet, NULL), cat);
    CHECK(rs.files_conflict == 1 && rs.files_already_there == 1);
    CHECK(file_equals(at("restore/a.txt"), "someone else's a"));
    {
        int conflicts = 0;
        REQUIRE_OK(bd_list_restore_conflicts(cat, count_conflict, &conflicts), cat);
        CHECK(conflicts == 1);
    }
    /* One chosen version of a file, into a folder. */
    {
        version_pick vp = {0};
        bd_list_copies(cat, a.node_id, pick_version, &vp);
        CHECK(vp.v1 > 0);
        REQUIRE_OK(bd_restore_version(cat, vp.v1, at("restore-v1"), &rs, quiet, NULL), cat);
        CHECK(rs.files_restored == 1);
        CHECK(file_equals(at("restore-v1/a.txt"), "hello"));
    }

    /* A copy the catalog lost track of (BRODALF closed before it saved):
     * the next run finds the same file there and adopts it. */
    write_file(at("src/Photos/c.txt"), "unsaved");
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 1);
    CHECK(bd_exec(cat, "DELETE FROM copies WHERE path_on_media LIKE '%/c.txt'") == 0);
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 0);
    CHECK(bs.files_already_there == 1);
    CHECK(lookup(cat, src, 0, "c.txt").state == BD_STATE_AVAILABLE);

    /* A missing backup root must not mark its files deleted; the app asks
     * where it went. */
    bd_rename_noreplace(at("src/Photos"), at("src/Photos-away"));
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_deleted == 0 && ss.roots_missing == 1);
    {
        source_flags sf = {0};
        bd_list_sources(cat, get_flags, &sf);
        CHECK(sf.missing == 1 && sf.active == 1);
    }
    bd_rename_noreplace(at("src/Photos-away"), at("src/Photos"));
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.roots_missing == 0);

    /* A real deletion is recorded and the copies stay tracked; the user
     * acknowledges it or forgets it. */
    bd_remove(at("src/Photos/c.txt"));
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_deleted == 1 && ss.deleted_no_copy == 0);
    CHECK(lookup(cat, src, 0, "c.txt").state == BD_STATE_DELETED);
    {
        int64_t n = -1;
        REQUIRE_OK(bd_list_deleted(cat, 0, 1, NULL, NULL, &n), cat);
        CHECK(n == 1);
        REQUIRE_OK(bd_deleted_ack(cat, 0), cat);
        REQUIRE_OK(bd_list_deleted(cat, 0, 1, NULL, NULL, &n), cat);
        CHECK(n == 0);
        REQUIRE_OK(bd_list_deleted(cat, 0, 0, NULL, NULL, &n), cat);
        CHECK(n == 1);
        REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
        REQUIRE_OK(bd_list_deleted(cat, 0, 1, NULL, NULL, &n), cat);
        CHECK(n == 0); /* seen stays seen */
    }

    /* Where a drive is kept, renaming, and what the disk said about itself. */
    REQUIRE_OK(bd_media_set_location(cat, drive, "Box A, top shelf"), cat);
    CHECK(strcmp(media_of(cat, drive).location, "Box A, top shelf") == 0);
    bd_drive_hw hw;
    memset(&hw, 0, sizeof(hw));
    snprintf(hw.vendor, sizeof(hw.vendor), "WD");
    snprintf(hw.model, sizeof(hw.model), "Elements 25A3");
    snprintf(hw.serial, sizeof(hw.serial), "WX11A1234567");
    snprintf(hw.bus, sizeof(hw.bus), "USB");
    hw.disk_bytes = 4000787030016LL;
    hw.smart = 1;
    snprintf(hw.health, sizeof(hw.health), "good");
    hw.temperature_c = 31;
    hw.power_on_hours = 2345;
    hw.power_cycles = 120;
    hw.reallocated_sectors = 0;
    hw.pending_sectors = -1;
    hw.uncorrectable_sectors = -1;
    hw.percent_used = -1;
    bd_media_store_hw(cat, drive, &hw, quiet, NULL);
    media_snapshot_t m = media_of(cat, drive);
    CHECK(m.has_hw && strcmp(m.hw.model, "Elements 25A3") == 0 && strcmp(m.hw.serial, "WX11A1234567") == 0);
    CHECK(m.hw.power_on_hours == 2345 && m.hw.temperature_c == 31 && m.hw.pending_sectors == -1);
    CHECK(strcmp(m.hw.health, "good") == 0 && m.hw.disk_bytes == 4000787030016LL);
    /* Same drive ID on another disk: logged. */
    snprintf(hw.serial, sizeof(hw.serial), "OTHER999");
    int logged = 0;
    bd_media_store_hw(cat, drive, &hw, count_log, &logged);
    CHECK(logged == 1);
    REQUIRE_OK(bd_media_set_location(cat, drive, ""), cat);
    CHECK(strcmp(media_of(cat, drive).location, "") == 0);
    REQUIRE_OK(bd_media_rename(cat, drive, "Blue WD 4TB"), cat);
    bd_media_disconnect(cat, drive);
    REQUIRE_OK(bd_media_connect(cat, at("drive"), &drive, quiet, NULL), cat);
    char pm[1200];
    snprintf(pm, sizeof(pm), "%s/BRODALF.media", drive_dir);
    bd_media_file mfile;
    CHECK(bd_media_file_read(at(pm), &mfile) == 0 && strcmp(mfile.label, "Blue WD 4TB") == 0);
    CHECK(bd_media_rename(cat, 9999, "x") == BD_ERR_NOT_FOUND);

    /* SMART parsing: an ATA attribute table and an NVMe health log. */
    unsigned char ata[512] = {0};
    const unsigned char attrs[][12] = {
        {5, 0x33, 0, 100, 100, 3, 0, 0, 0, 0, 0, 0},       /* 3 reallocated */
        {9, 0x32, 0, 95, 95, 0x39, 0x30, 0, 0, 0, 0, 0},   /* 12345 hours */
        {194, 0x22, 0, 110, 100, 36, 0, 0, 0, 0, 0, 0},    /* 36 C */
    };
    for (int i = 0; i < 3; i++) memcpy(ata + 2 + i * 12, attrs[i], 12);
    CHECK(bd_drive_hw_parse_ata(&hw, ata, 0) == 0);
    CHECK(hw.reallocated_sectors == 3 && hw.power_on_hours == 12345 && hw.temperature_c == 36);
    CHECK(strcmp(hw.health, "warning") == 0);
    CHECK(bd_drive_hw_parse_ata(&hw, ata, 1) == 0 && strcmp(hw.health, "failing") == 0);
    unsigned char nvme[512] = {0};
    nvme[1] = (unsigned char)(310 & 0xFF);
    nvme[2] = (unsigned char)(310 >> 8); /* 310 K = 37 C */
    nvme[5] = 4;                          /* 4% used */
    nvme[112] = 50;                       /* power cycles */
    nvme[128] = 0xE8;
    nvme[129] = 0x03;                     /* 1000 hours */
    bd_drive_hw_parse_nvme(&hw, nvme);
    CHECK(hw.temperature_c == 37 && hw.percent_used == 4 && hw.power_cycles == 50 && hw.power_on_hours == 1000);
    CHECK(strcmp(hw.health, "good") == 0);
    /* The ATA table is only read from ATA and SATA answers, never NVMe,
     * SCSI, SAS, USB or RAID ones. */
    CHECK(bd_drive_hw_bus_is_ata(3) && bd_drive_hw_bus_is_ata(11));
    CHECK(!bd_drive_hw_bus_is_ata(17) && !bd_drive_hw_bus_is_ata(1) && !bd_drive_hw_bus_is_ata(10));
    CHECK(!bd_drive_hw_bus_is_ata(7) && !bd_drive_hw_bus_is_ata(8) && !bd_drive_hw_bus_is_ata(0));

    /* Each save is a new revision: its copy is added to the drive, and an
     * older one is never overwritten. */
    REQUIRE_OK(bd_catalog_save(cat), cat);
    REQUIRE_OK(bd_catalog_copy_to_media(cat, drive), cat);
    char pc[1200];
    snprintf(pc, sizeof(pc), "%s/catalog-r%08lld.brodalf", drive_dir, (long long)bd_catalog_revision(cat));
    CHECK(exists(at(pc)));
    {
        char dd[1200];
        snprintf(dd, sizeof(dd), "%s", at(drive_dir));
        CHECK(count_catalog_copies(dd) >= 2);
        /* Recovering finds the newest copy whether given the drive, its
         * BRODALF folder or the catalog's own folder. */
        const char *where[3] = {"drive", "drive/BRODALF", drive_dir};
        for (int w = 0; w < 3; w++) {
            found_copy fc;
            memset(&fc, 0, sizeof(fc));
            REQUIRE_OK(bd_find_catalog_copies(at(where[w]), found_copy_cb, &fc), cat);
            CHECK(fc.n == 1 && fc.revision == bd_catalog_revision(cat));
            CHECK(strcmp(fc.id, bd_catalog_uuid(cat)) == 0 && strstr(fc.path, "catalog-r") != NULL);
        }
        CHECK(bd_find_catalog_copies(at("src"), NULL, NULL) == BD_ERR_NOT_FOUND);
    }
    bd_catalog_close(cat);
    CHECK(!exists(at("test.brodalf.lock")));

    test_risk();
    test_search();
    test_drives_and_versions();
    test_report();
    test_moves_guard_and_progress();
    test_big_copy();
    test_parallel_scan();
    test_cancel();
    test_case_rename();
    test_compression();
    test_reserved_source_names();
#ifndef _WIN32
    test_mtimes();
#endif
    test_recover_and_relocate();
    test_lost_drive();
    test_copies_on();
    test_deleted_files();
    test_bitrot_and_sizes();
    test_multi_copy();
    test_backup_many();
    test_stars_and_places();
    test_encrypt_folders();
    test_shadow_list();
    test_drive_gone();
    test_restore_fallback();
    test_restore_older_version();
    test_unsafe_paths();
    test_unread_copies();

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("all core checks passed");
    return 0;
}
