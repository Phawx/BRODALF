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
    bd_restore_opts ro = {before_attack, 0};
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
    if (st->is_file) t->files++;
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
            REQUIRE_OK(bd_media_check(cat, drive, 1, &cs, quiet, NULL), cat);
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
    REQUIRE_OK(bd_media_check(cat, drive, 1, &cs, quiet, NULL), cat);
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
    CHECK(count_rows(cat, "SELECT COUNT(*) FROM jobs WHERE status='stopped'") == 1);
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
    ca.phase = "verify";
    ca.after_files = 3;
    bd_catalog_set_progress(cat, cancel_progress, &ca);
    bd_check_stats cs;
    CHECK(bd_media_check(cat, drive, 1, &cs, quiet, NULL) == BD_ERR_CANCELLED && ca.fired);
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
    CHECK(!exists(at("cx/out/Src/big.bin")) && !exists(at("cx/out/Src/big.bin" BD_TMP_MARKER)));
    bd_catalog_set_progress(cat, NULL, NULL);
    REQUIRE_OK(bd_restore(cat, src, "", at("cx/out"), &rs, quiet, NULL), cat);
    CHECK(rs.files_failed == 0 && rs.files_restored + rs.files_already_there == 41);
    char back[BD_HASH_HEX_LEN + 1], want[BD_HASH_HEX_LEN + 1];
    CHECK(bd_hash_file(at("cx/out/Src/big.bin"), NULL, back, NULL) == 0 &&
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

    /* The quick check (size and time) finds every copy as recorded. */
    bd_check_stats cs;
    REQUIRE_OK(bd_media_check(cat, drive, 0, &cs, quiet, NULL), cat);
    CHECK(cs.copies == 2 && cs.ok == 2 && cs.rehashed == 0);

    /* A new version: the old copy moves into .versions with its time. */
    bd_sleep_ms(5);
    int64_t between = bd_now_ms();
    bd_sleep_ms(5);
    write_file(at("mt/Src/a.txt"), "second a, longer");
    set_time(at("mt/Src/a.txt"), t3);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_backup(cat, drive, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 1 && bs.versions_moved == 1);
    CHECK(time_of(copy_a) == t3);
    char *old_a = bd_sprintf("mt/drive/BRODALF/%s/.versions/Src/a.v1.txt", bd_catalog_uuid(cat));
    CHECK(time_of(at(old_a)) == t1);
    REQUIRE_OK(bd_media_check(cat, drive, 0, &cs, quiet, NULL), cat);
    CHECK(cs.copies == 3 && cs.ok == 3 && cs.rehashed == 0);

    /* Restored files get the time the catalog has for that version. */
    bd_restore_stats rs;
    REQUIRE_OK(bd_restore(cat, src, "", at("mt/now"), &rs, quiet, NULL), cat);
    CHECK(rs.files_restored == 2);
    CHECK(time_of(at("mt/now/Src/a.txt")) == t3);
    CHECK(time_of(at("mt/now/Src/big.bin")) == t2);
    bd_restore_opts ro = {between};
    REQUIRE_OK(bd_restore_ex(cat, src, "", at("mt/then"), &ro, &rs, quiet, NULL), cat);
    CHECK(file_equals(at("mt/then/Src/a.txt"), "first a"));
    CHECK(time_of(at("mt/then/Src/a.txt")) == t1);

    /* Restored in place: the next scan sees nothing changed. */
    bd_remove(at("mt/Src/big.bin"));
    REQUIRE_OK(bd_restore(cat, src, "big.bin", at("mt"), &rs, quiet, NULL), cat);
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
    CHECK(bs.files_copied == 0 && bs.files_moved == 0 && bs.files_already_there == 2);
    bd_restore_stats rs;
    REQUIRE_OK(bd_restore(cat, src, "", at("cr/out"), &rs, quiet, NULL), cat);
    CHECK(rs.files_restored == 2);
    CHECK(file_equals(at("cr/out/Src/photo.jpg"), "picture") && file_equals(at("cr/out/Src/album/One.txt"), "one"));

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
    CHECK(ss.files_new == 1 && ss.files_deleted == 1);
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
    CHECK(count_rows(cat, "SELECT value FROM meta WHERE key='schema_version'") == 6);
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
    CHECK(bs.files_copied == 1 && bs.versions_moved == 1);
    char *root = bd_sprintf("rv/drive/BRODALF/%s", bd_catalog_uuid(cat));
    char *p1 = bd_sprintf("%s/_versions/a.txt", root), *p2 = bd_sprintf("%s/.versions/Docs/a.v1.txt", root);
    char *p3 = bd_sprintf("%s/_brodalf.MEDIA/m.txt", root);
    CHECK(file_equals(at(p1), "in a folder called .versions"));
    CHECK(file_equals(at(p2), "doc v1"));
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
    REQUIRE_OK(bd_media_check(cat, drive, 1, &cs, quiet, NULL), cat);
    CHECK(cs.copies == 5 && cs.ok == 5);
    free(root);
    free(p1);
    free(p2);
    free(p3);
    free(p4);
    bd_catalog_close(cat);
}

/* What the elevated shadow-copy helper accepts: only a list whose hash
 * matches, and only files inside the protected folders. */
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
    snprintf(backup, sizeof(backup), "r/d1/BRODALF/%s/catalog-backup.brodalf", uuid);
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
    REQUIRE_OK(bd_media_connect(cat, at("r/d1"), &found, &cs, quiet, NULL), cat);
    CHECK(found == d1 && cs.ok == 3 && cs.missing == 0);

    /* The user folder has another name here. Until it is pointed there a
     * scan skips it rather than calling its files deleted. */
    CHECK(bd_rename_noreplace(at("r/Users/old"), at("r/Users/new")) == 0);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_deleted == 0 && ss.errors == 1);
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
    CHECK(rs.files_restored == 2 && file_equals(at("r/out/Docs/sub/b.txt"), "beta"));
    REQUIRE_OK(bd_catalog_save(cat), cat);
    bd_catalog_close(cat);
}

typedef struct { int64_t want, retired_ms, replaced_by; } retired_snap;

static int snap_retired(void *ctx, const bd_media_info *m)
{
    retired_snap *r = ctx;
    if (m->media_id != r->want) return 0;
    r->retired_ms = m->retired_ms;
    r->replaced_by = m->replaced_by;
    return 1;
}

static retired_snap retired_of(bd_catalog *cat, int64_t id)
{
    retired_snap r = {id, -1, -1};
    bd_list_media(cat, snap_retired, &r);
    return r;
}

/* Replacing a drive: what needs it goes to the new one, nothing new goes to
 * it, and its copies stop counting once the new drive holds them. */
static void test_replace_drive(void)
{
    write_file(at("x/S1/f1.txt"), "one");
    write_file(at("x/S1/f2.txt"), "two");
    write_file(at("x/S2/f3.txt"), "three");
    write_file(at("x/S2/f4.txt"), "four");
    write_file(at("x/S2/gone.txt"), "gone");
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
    bd_backup_stats bs;
    REQUIRE_OK(bd_backup(cat, a, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 5);
    REQUIRE_OK(bd_backup(cat, b, s1, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 2);
    /* An older version and a deleted file that only the old drive holds. */
    write_file(at("x/S2/f3.txt"), "three, changed");
    CHECK(bd_remove(at("x/S2/gone.txt")) == 0);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    risk_count rc;
    CHECK(risk_of(cat, &rc).files_at_risk == 2); /* f3 (no copy of its new version) and f4 (one copy) */

    /* The new drive has to be plugged in; nothing changes until it is. */
    CHECK(bd_media_replace(cat, a, 999, NULL, quiet, NULL) == BD_ERR_NOT_FOUND);
    CHECK(retired_of(cat, a).retired_ms == 0);
    CHECK(bd_media_retire(cat, a, a) == BD_ERR_INVALID);
    REQUIRE_OK(bd_media_init(cat, at("x/c"), "New", 0, &c), cat);
    bd_replace_stats rs;
    REQUIRE_OK(bd_media_replace(cat, a, c, &rs, quiet, NULL), cat);
    /* f1 and f2 would have one copy without the old drive, f4 none. */
    CHECK(rs.files_needed == 3 && rs.backup.files_copied == 3 && rs.backup.files_failed == 0);
    CHECK(rs.files_only_old == 0 && rs.deleted_only_old == 1 && rs.versions_only_old == 1);
    retired_snap r = retired_of(cat, a);
    CHECK(r.retired_ms > 0 && r.replaced_by == c);
    CHECK(retired_of(cat, c).retired_ms == 0);
    char p[1200];
    snprintf(p, sizeof(p), "x/c/BRODALF/%s/S2/f4.txt", bd_catalog_uuid(cat));
    CHECK(file_equals(at(p), "four"));
    snprintf(p, sizeof(p), "x/c/BRODALF/%s/S2/f3.txt", bd_catalog_uuid(cat));
    CHECK(!exists(at(p))); /* not needed for the old drive: a normal backup copies it */

    /* f1 and f2 are on Other and New now (Old no longer counts); f4 has one
     * copy that counts, on New. */
    CHECK(risk_of(cat, &rc).files_at_risk == 2 && rc.copies_sum == 1);
    CHECK(bd_backup(cat, a, 0, &bs, quiet, NULL) == BD_ERR_INVALID);
    help_list h = help_of(cat);
    for (int i = 0; i < h.n; i++) CHECK(strcmp(h.labels[i], "Old") != 0);
    /* Its copies still restore. */
    find_ctx f = lookup(cat, s2, 0, "gone.txt");
    CHECK(f.found && f.state == BD_STATE_DELETED);

    /* Changed my mind: it counts and takes backups again. */
    REQUIRE_OK(bd_media_unretire(cat, a), cat);
    CHECK(retired_of(cat, a).retired_ms == 0 && retired_of(cat, a).replaced_by == 0);
    CHECK(risk_of(cat, &rc).files_at_risk == 1); /* only f3 */
    REQUIRE_OK(bd_backup(cat, a, 0, &bs, quiet, NULL), cat);
    CHECK(bs.files_copied == 1);
    /* Retiring with no replacement chosen keeps every copy counting. */
    REQUIRE_OK(bd_media_retire(cat, a, 0), cat);
    CHECK(retired_of(cat, a).retired_ms > 0 && retired_of(cat, a).replaced_by == 0);
    CHECK(risk_of(cat, &rc).files_at_risk == 1);
    bd_catalog_close(cat);
}

typedef struct { int n; char first[256]; char drives[256]; int connected; int64_t last_seen; } deleted_list;

static int collect_deleted(void *ctx, const bd_deleted_info *d)
{
    deleted_list *l = ctx;
    if (!l->n++) {
        snprintf(l->first, sizeof(l->first), "%s/%s", d->source_name, d->rel_path);
        snprintf(l->drives, sizeof(l->drives), "%s", d->drives);
        l->connected = d->connected;
        l->last_seen = d->last_seen_ms;
    }
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
    /* A file that never had a copy is not listed when it goes. */
    write_file(at("del/D/never.txt"), "never backed up");
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);

    int64_t count = -1;
    REQUIRE_OK(bd_list_deleted(cat, 0, NULL, NULL, &count), cat);
    CHECK(count == 0);
    CHECK(bd_remove(at("del/D/a.txt")) == 0);
    CHECK(bd_remove(at("del/D/sub/b.txt")) == 0);
    CHECK(bd_remove(at("del/D/sub/c.txt")) == 0);
    CHECK(bd_rmdir_empty(at("del/D/sub")) == 0);
    CHECK(bd_remove(at("del/D/never.txt")) == 0);
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    CHECK(ss.files_deleted == 4);
    deleted_list dl;
    memset(&dl, 0, sizeof(dl));
    REQUIRE_OK(bd_list_deleted(cat, src, collect_deleted, &dl, &count), cat);
    CHECK(count == 3 && dl.n == 3 && dl.last_seen > 0);
    CHECK(strcmp(dl.first, "D/a.txt") == 0 && strcmp(dl.drives, "One, Two") == 0 && dl.connected);
    REQUIRE_OK(bd_list_deleted(cat, 9999, NULL, NULL, &count), cat);
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
    CHECK(file_equals(at("del/out/D/sub/b.txt"), "b") && file_equals(at("del/out/D/sub/c.txt"), "c2, changed"));
    CHECK(!exists(at("del/out/D/a.txt")) && !exists(at("del/out/D/keep.txt")));
    REQUIRE_OK(bd_restore_ex(cat, src, "a.txt", at("del/out"), &ro, &rs, quiet, NULL), cat);
    CHECK(rs.files_restored == 1 && file_equals(at("del/out/D/a.txt"), "a"));
    REQUIRE_OK(bd_restore_ex(cat, src, "", at("del/out"), &ro, &rs, quiet, NULL), cat);
    CHECK(rs.files_restored == 0 && rs.files_already_there == 3 && rs.files_no_copy == 1 && !exists(at("del/out/D/keep.txt")));

    /* Back in its folder, it is no longer deleted. */
    write_file(at("del/D/a.txt"), "a");
    REQUIRE_OK(bd_scan(cat, &ss, quiet, NULL), cat);
    REQUIRE_OK(bd_list_deleted(cat, 0, NULL, NULL, &count), cat);
    CHECK(count == 2);
    bd_media_disconnect(cat, one);
    memset(&dl, 0, sizeof(dl));
    REQUIRE_OK(bd_list_deleted(cat, 0, collect_deleted, &dl, NULL), cat);
    CHECK(dl.n == 2 && !dl.connected);
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

    /* Bit rot looks like this: the bytes are wrong but the size and the
     * modified time are just what the catalog recorded. A quick check must
     * go on calling the copy damaged until a re-read proves it good, not
     * trust the unchanged time. */
    {
        bd_stat_t rot;
        CHECK(bd_stat(at(pv), &rot) == 0);
        sqlite3_stmt *fix;
        CHECK(sqlite3_prepare_v2(cat->db, "UPDATE copies SET stored_mtime_ns=? WHERE state='bad'", -1, &fix, NULL) == SQLITE_OK);
        sqlite3_bind_int64(fix, 1, rot.mtime_ns);
        CHECK(sqlite3_step(fix) == SQLITE_DONE);
        sqlite3_finalize(fix);
        REQUIRE_OK(bd_media_check(cat, drive, 0, &cs, quiet, NULL), cat);
        CHECK(cs.bad == 1 && cs.rehashed == 1);
        /* The right bytes come back (as if from another drive), with the
         * recorded time: the next quick check re-reads it and clears the mark. */
        write_file(at(pv), "hello");
        CHECK(bd_stat(at(pv), &rot) == 0);
        CHECK(sqlite3_prepare_v2(cat->db, "UPDATE copies SET stored_mtime_ns=? WHERE state='bad'", -1, &fix, NULL) == SQLITE_OK);
        sqlite3_bind_int64(fix, 1, rot.mtime_ns);
        CHECK(sqlite3_step(fix) == SQLITE_DONE);
        sqlite3_finalize(fix);
        REQUIRE_OK(bd_media_check(cat, drive, 0, &cs, quiet, NULL), cat);
        CHECK(cs.bad == 0 && cs.rehashed == 1);
    }

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
    /* The ATA table is only read from ATA and SATA answers, never NVMe,
     * SCSI, SAS, USB or RAID ones. */
    CHECK(bd_drive_hw_bus_is_ata(3) && bd_drive_hw_bus_is_ata(11));
    CHECK(!bd_drive_hw_bus_is_ata(17) && !bd_drive_hw_bus_is_ata(1) && !bd_drive_hw_bus_is_ata(10));
    CHECK(!bd_drive_hw_bus_is_ata(7) && !bd_drive_hw_bus_is_ata(8) && !bd_drive_hw_bus_is_ata(0));

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
    test_big_copy();
    test_parallel_scan();
    test_cancel();
    test_case_rename();
    test_reserved_source_names();
#ifndef _WIN32
    test_mtimes();
#endif
    test_shadow_list();
    test_recover_and_relocate();
    test_replace_drive();
    test_deleted_files();

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("all core checks passed");
    return 0;
}
