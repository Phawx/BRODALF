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
    CHECK(bd_option_get(cat, "auto_backup") == 1 && bd_option_get(cat, "check_days") == 180);
    CHECK(bd_option_get(cat, "no_such_option") == -1);
    REQUIRE_OK(bd_option_set(cat, "auto_backup", 0), cat);
    REQUIRE_OK(bd_option_set(cat, "check_days", 30), cat);
    CHECK(bd_option_get(cat, "auto_backup") == 0 && bd_option_get(cat, "check_days") == 30);
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
    REQUIRE_OK(bd_media_connect(cat, at("find/A"), &again, &cs, quiet, NULL), cat);
    CHECK(due_of(cat, a).due == 1); /* a quick check is not a full one */
    REQUIRE_OK(bd_media_check(cat, a, 1, &cs, quiet, NULL), cat);
    CHECK(due_of(cat, a).due == 0);

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

    write_file(at("span/src/keep.txt"), "keep");
    write_file(at("span/src/big.bin"), "0123456789012345678901234567890123456789");
    write_file(at("span/src/scratch.tmp"), "tmp");
    write_file(at("span/src/node_modules/pkg/index.js"), "js");
    write_file(at("span/src/v.txt"), "1");
    bd_mkdirs(at("span/D"));
    bd_mkdirs(at("span/E"));
    bd_mkdirs(at("span/out"));

    bd_catalog *cat;
    REQUIRE_OK(bd_catalog_create(at("span/span.brodalf"), &cat), NULL);
    int64_t src, d, e;
    REQUIRE_OK(bd_source_add(cat, at("span/src"), &src), cat);
    bd_scan_stats ss;
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.skipped == 2 && ss.files_seen == 3);
    CHECK(count_hits(cat, "index") == 0 && count_hits(cat, "scratch") == 0 && count_hits(cat, "keep") == 1);

    /* An empty list skips nothing; a new pattern drops what it now matches. */
    REQUIRE_OK(bd_skip_list_set(cat, ""), cat);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.skipped == 0 && count_hits(cat, "index.js") == 1);
    REQUIRE_OK(bd_skip_list_set(cat, "*.js\nnode_modules/\n*.tmp\n"), cat);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(count_hits(cat, "index") == 0 && count_hits(cat, "node_modules") == 0 && count_hits(cat, "scratch") == 0);
    CHECK(ss.files_deleted == 0);
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

    /* Carry on onto another drive with only what did not fit. */
    REQUIRE_OK(bd_media_init(cat, at("span/E"), "Drive E", 0, &e), cat);
    REQUIRE_OK(bd_media_set_location(cat, e, "Box 7"), cat);
    bd_backup_opts opts = {{d}};
    REQUIRE_OK(bd_backup_ex(cat, e, 0, &opts, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 1 && bs.bytes_copied == 40 && bs.files_no_room == 0);

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
    bd_check_stats cs;
    REQUIRE_OK(bd_media_connect(cat, at("span/E"), &again, &cs, quiet, NULL), cat);
    memset(&p, 0, sizeof(p));
    REQUIRE_OK(bd_restore_plan(cat, src, "", at("span/out"), collect_plan, &p, &total, &none), cat);
    CHECK(p.n == 2 && strcmp(p.labels[0], "Drive E") == 0 && p.connected[0] && !p.connected[1]);
    bd_restore_stats rs;
    REQUIRE_OK(bd_restore(cat, src, NULL, at("span/out"), &rs, quiet, NULL), cat);
    CHECK(rs.files_restored == 1 && rs.files_offline == 2);

    /* Then D arrives: the rest comes back, E's file is left alone. */
    REQUIRE_OK(bd_media_connect(cat, at("span/D"), &again, &cs, quiet, NULL), cat);
    memset(&p, 0, sizeof(p));
    REQUIRE_OK(bd_restore_plan(cat, src, "", at("span/out"), collect_plan, &p, &total, &none), cat);
    CHECK(total == 2 && p.n == 1 && strcmp(p.labels[0], "Drive D") == 0);
    REQUIRE_OK(bd_restore(cat, src, NULL, at("span/out"), &rs, quiet, NULL), cat);
    CHECK(rs.files_restored == 2 && rs.files_already_there == 1 && rs.files_offline == 0);
    CHECK(file_equals(at("span/out/src/big.bin"), "0123456789012345678901234567890123456789"));
    REQUIRE_OK(bd_restore_plan(cat, src, "", at("span/out"), collect_plan, &p, &total, &none), cat);
    CHECK(total == 0);

    /* Old versions: the default rule (5 versions or a year) keeps them all. */
    const char *contents[] = {"22", "333", "4444"};
    for (int i = 0; i < 3; i++) {
        write_file(at("span/src/v.txt"), contents[i]);
        REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
        REQUIRE_OK(bd_backup(cat, d, 0, &bs, quiet, NULL), cat);
        CHECK(bs.versions_pruned == 0);
    }
    CHECK(exists(at("span/D/BRODALF")));
    char vdir[1400];
    snprintf(vdir, sizeof(vdir), "span/D/BRODALF/%s/.versions/src", bd_catalog_uuid(cat));
    char v1[1500], v3[1500];
    snprintf(v1, sizeof(v1), "%s/v.v1.txt", vdir);
    snprintf(v3, sizeof(v3), "%s/v.v3.txt", vdir);
    CHECK(exists(at(v1)) && exists(at(v3)));

    /* Keep 2 versions, no matter how recent: v1 and v2 go, v3 stays. */
    REQUIRE_OK(bd_option_set(cat, "keep_versions", 2), cat);
    REQUIRE_OK(bd_option_set(cat, "keep_days", 0), cat);
    bd_prune_stats ps;
    REQUIRE_OK(bd_prune_versions(cat, d, &ps, quiet, NULL), cat);
    CHECK(ps.copies_removed == 2 && ps.bytes_freed == 3 && ps.failed == 0);
    CHECK(!exists(at(v1)) && exists(at(v3)));
    REQUIRE_OK(bd_prune_versions(cat, d, &ps, quiet, NULL), cat);
    CHECK(ps.copies_removed == 0);
    /* 0 keeps everything. */
    REQUIRE_OK(bd_option_set(cat, "keep_versions", 0), cat);
    write_file(at("span/src/v.txt"), "55555");
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_backup(cat, d, 0, &bs, quiet, NULL), cat);
    CHECK(bs.versions_pruned == 0 && bs.files_copied == 1);

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
    int reports, ends;
    bd_progress last;
    char phase[32];
} progress_log;

static void on_progress(void *ctx, const bd_progress *p)
{
    progress_log *l = ctx;
    l->reports++;
    if (p->done) l->ends++;
    l->last = *p;
    snprintf(l->phase, sizeof(l->phase), "%s", p->phase);
    l->last.phase = l->phase;
    l->last.current = "";
}

typedef struct { int n; char path[1024]; } in_use_list;

static int collect_in_use(void *ctx, const char *path, int64_t size)
{
    in_use_list *l = ctx;
    if (l->n++ == 0) snprintf(l->path, sizeof(l->path), "%s", path);
    (void)size;
    return 0;
}

static int first_source_path(void *ctx, const bd_source_info *info)
{
    snprintf((char *)ctx, 1024, "%s", info->path);
    return 1;
}

/* Moves, the verify pass, progress, the ransomware guard, restoring as of
 * a date, drive suggestions and files in use. */
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
    CHECK(pl.last.files_total == 60 && pl.last.files_done == 60 && pl.last.bytes_done == pl.last.bytes_total);

    /* Rename a folder holding a third of the files: its copies move on the
     * drive instead of being copied again, and the guard knows a move from damage. */
    CHECK(bd_rename_noreplace(at("g/Work/old"), at("g/Work/renamed")) == 0);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_new == 20 && ss.files_deleted == 20 && !ss.guard_tripped);
    REQUIRE_OK(bd_backup(cat, d1, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_moved == 20 && bs.files_copied == 0 && bs.files_failed == 0);
    char p[1200];
    snprintf(p, sizeof(p), "g/d1/BRODALF/%s/Work/renamed/f03.txt", bd_catalog_uuid(cat));
    CHECK(file_equals(at(p), "file 3, first version"));
    snprintf(p, sizeof(p), "g/d1/BRODALF/%s/Work/old/f03.txt", bd_catalog_uuid(cat));
    CHECK(!exists(at(p)));
    snprintf(p, sizeof(p), "g/d1/BRODALF/%s/Work/old", bd_catalog_uuid(cat));
    CHECK(!exists(at(p))); /* the emptied folder went with its files */
    find_ctx ren = lookup(cat, src, 0, "renamed");
    CHECK(lookup(cat, src, ren.node_id, "f03.txt").state == BD_STATE_AVAILABLE);
    /* Renamed back: the old entries return, the copies move back, no alarm. */
    CHECK(bd_rename_noreplace(at("g/Work/renamed"), at("g/Work/old")) == 0);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_new == 20 && ss.files_deleted == 20 && !ss.guard_tripped);
    REQUIRE_OK(bd_backup(cat, d1, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_moved == 20 && bs.files_copied == 0);
    CHECK(bd_rename_noreplace(at("g/Work/old"), at("g/Work/renamed")) == 0);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(!ss.guard_tripped);
    REQUIRE_OK(bd_backup(cat, d1, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_moved == 20 && bs.files_copied == 0);
    snprintf(p, sizeof(p), "g/d1/BRODALF/%s/Work/renamed/f03.txt", bd_catalog_uuid(cat));
    CHECK(file_equals(at(p), "file 3, first version"));

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

    /* Half the files change at once: backups pause. */
    int64_t before_attack = 0;
    for (int i = 20; i < 55; i++) {
        snprintf(name, sizeof(name), "g/Work/keep/f%02d.txt", i);
        write_file(at(name), "ENCRYPTED GARBAGE");
    }
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_changed == 35 && ss.guard_tripped);
    bd_guard_info g;
    REQUIRE_OK(bd_guard_get(cat, &g), cat);
    CHECK(g.tripped && g.files_changed == 35 && g.files_total == 60 && g.before_ms > 0);
    before_attack = g.before_ms;
    CHECK(bd_backup(cat, d1, 0, &bs, quiet, NULL) == BD_ERR_GUARD);

    /* Restore as the files were before. */
    bd_restore_opts ro = {before_attack};
    int64_t total = 0, none = 0;
    REQUIRE_OK(bd_restore_plan_ex(cat, src, NULL, NULL, &ro, NULL, NULL, &total, &none), cat);
    CHECK(total == 60 && none == 0);
    bd_restore_stats rs;
    REQUIRE_OK(bd_restore_ex(cat, src, NULL, at("g/back"), &ro, &rs, quiet, NULL), cat);
    CHECK(rs.files_restored == 60 && rs.files_no_copy == 0);
    CHECK(file_equals(at("g/back/Work/keep/f21.txt"), "file 21, first version"));
    CHECK(file_equals(at("g/back/Work/renamed/f03.txt"), "file 3, first version"));
    /* The latest versions were never backed up. */
    REQUIRE_OK(bd_restore(cat, src, NULL, at("g/latest"), &rs, quiet, NULL), cat);
    CHECK(rs.files_restored == 25 && rs.files_no_copy == 35);

    /* The changes were the user's: back up again. */
    bd_backup_opts bo;
    memset(&bo, 0, sizeof(bo));
    bo.ignore_guard = 1;
    REQUIRE_OK(bd_backup_ex(cat, d1, 0, &bo, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 35);
    REQUIRE_OK(bd_guard_clear(cat), cat);
    REQUIRE_OK(bd_guard_get(cat, &g), cat);
    CHECK(!g.tripped);
    REQUIRE_OK(bd_backup(cat, d1, 0, &bs, quiet, NULL), cat);

    /* A file in use is read from its staged (shadow) copy. */
    char root[1024] = "";
    bd_list_sources(cat, first_source_path, root);
    write_file(at("g/Work/mail.pst"), "live mailbox");
    write_file(at("g/staged-mail.bin"), "live mailbox");
    char *mail = bd_path_join(root, "mail.pst");
    REQUIRE_OK(bd_substitute_add(cat, mail, at("g/staged-mail.bin")), cat);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_new == 1);
    REQUIRE_OK(bd_backup(cat, d1, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 1);
    bd_substitutes_clear(cat);
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
    CloseHandle(h);
#endif
    free(mail);
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
    REQUIRE_OK(bd_media_connect(cat, at("drive"), &drive, &cs, quiet, NULL), cat);
    CHECK(cs.copies == 2 && cs.ok == 2);
    CHECK(lookup(cat, src, 0, "a.txt").state == BD_STATE_AVAILABLE);

    /* Change a file: only the old version is reachable until the next backup. */
    write_file(at("src/Photos/a.txt"), "hello, version two");
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_changed == 1);
    a = lookup(cat, src, 0, "a.txt");
    CHECK(a.state == BD_STATE_AVAILABLE_OLDER);
    CHECK(a.version_no == 2);
    CHECK(totals(cat).state == BD_STATE_PARTIAL && totals(cat).available == 1);
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 1);
    CHECK(bs.versions_moved == 1);
    CHECK(file_equals(at(p1), "hello, version two"));
    char pv[1200];
    snprintf(pv, sizeof(pv), "%s/.versions/Photos/a.v1.txt", drive_dir);
    CHECK(file_equals(at(pv), "hello"));
    int copies = 0;
    bd_list_copies(cat, a.node_id, count_copies, &copies);
    CHECK(copies == 2);

    /* Lose a copy on the drive: the quick check notices it. */
    char pb[1200];
    snprintf(pb, sizeof(pb), "%s/Photos/sub/b.txt", drive_dir);
    bd_remove(at(pb));
    REQUIRE_OK(bd_media_check(cat, drive, 0, &cs, quiet, NULL), cat);
    CHECK(cs.missing == 1);
    find_ctx sub = lookup(cat, src, 0, "sub");
    CHECK(sub.state == BD_STATE_NO_COPY || sub.state == BD_STATE_PARTIAL || sub.state == BD_STATE_OFFLINE);
    CHECK(lookup(cat, src, sub.node_id, "b.txt").state == BD_STATE_BAD);

    /* Damage a copy: the full check notices it. */
    write_file(at(pv), "HELLO");
    REQUIRE_OK(bd_media_check(cat, drive, 1, &cs, quiet, NULL), cat);
    CHECK(cs.bad == 1);

    /* The next backup rewrites the missing copy. */
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 1);
    CHECK(lookup(cat, src, sub.node_id, "b.txt").state == BD_STATE_AVAILABLE);

    /* The old copy of b vanishes, then b changes: the new copy still records. */
    bd_remove(at(pb));
    write_file(at("src/Photos/sub/b.txt"), "world, version two");
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 1);
    CHECK(bs.files_failed == 0);
    CHECK(lookup(cat, src, sub.node_id, "b.txt").state == BD_STATE_AVAILABLE);

    /* Restore everything and compare. */
    bd_restore_stats rs;
    REQUIRE_OK(bd_restore(cat, 0, NULL, at("restore"), &rs, quiet, NULL), cat);
    CHECK(rs.files_restored == 2);
    CHECK(file_equals(at("restore/Photos/a.txt"), "hello, version two"));
    CHECK(file_equals(at("restore/Photos/sub/b.txt"), "world, version two"));

    /* A backup whose catalog was never saved: the next run adopts the copy. */
    write_file(at("src/Photos/c.txt"), "unsaved");
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_catalog_save(cat), cat);
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 1);
    bd_catalog_close(cat); /* closed without saving */
    REQUIRE_OK(bd_catalog_open(at("test.brodalf"), &cat), NULL);
    REQUIRE_OK(bd_media_connect(cat, at("drive"), &drive, &cs, quiet, NULL), cat);
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 0);
    CHECK(bs.files_already_there == 3);
    CHECK(lookup(cat, src, 0, "c.txt").state == BD_STATE_AVAILABLE);

    /* A missing source folder must not mark its files deleted. */
    bd_rename_noreplace(at("src/Photos"), at("src/Photos-away"));
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_deleted == 0);
    bd_rename_noreplace(at("src/Photos-away"), at("src/Photos"));

    /* A real deletion is recorded but the copies stay tracked. */
    bd_remove(at("src/Photos/c.txt"));
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_deleted == 1);
    CHECK(lookup(cat, src, 0, "c.txt").state == BD_STATE_DELETED);

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
    REQUIRE_OK(bd_media_connect(cat, at("drive"), &drive, &cs, quiet, NULL), cat);
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

    REQUIRE_OK(bd_catalog_copy_to_media(cat, drive), cat);
    char pc[1200];
    snprintf(pc, sizeof(pc), "%s/catalog-backup.brodalf", drive_dir);
    CHECK(exists(at(pc)));
    bd_catalog_close(cat);
    CHECK(!exists(at("test.brodalf.lock")));

    test_risk();
    test_search();
    test_drives_and_versions();
    test_report();
    test_moves_guard_and_progress();

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("all core checks passed");
    return 0;
}
