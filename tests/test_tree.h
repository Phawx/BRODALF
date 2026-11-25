/* For the tests: remove a folder and everything in it, so a test leaves
 * nothing behind in the temp folder. Include after brodalf.h and
 * internal.h. */
#ifndef BRODALF_TEST_TREE_H
#define BRODALF_TEST_TREE_H

#include <stdlib.h>
#ifndef _WIN32
#include <sys/stat.h>
#endif

/* The walk lists parents before what is inside them, so going through the
 * list backwards empties each folder before it is removed. */
typedef struct { char **paths; int *dirs; size_t n, cap; const char *root; } test_tree_list;

static int test_tree_note(void *ctx, const char *rel, const bd_stat_t *st)
{
    test_tree_list *t = ctx;
    if (t->n == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 64;
        t->paths = realloc(t->paths, t->cap * sizeof(*t->paths));
        t->dirs = realloc(t->dirs, t->cap * sizeof(*t->dirs));
        if (!t->paths || !t->dirs) exit(1);
    }
    t->paths[t->n] = bd_sprintf("%s/%s", t->root, rel);
    t->dirs[t->n] = st->is_dir && !st->is_link;
    t->n++;
    return 0;
}

static void remove_tree(const char *root)
{
#ifndef _WIN32
    /* A test may have left a folder unreadable. */
    chmod(root, 0755);
#endif
    test_tree_list t = {0};
    t.root = root;
    bd_walk(root, test_tree_note, NULL, &t);
    for (size_t i = t.n; i-- > 0;) {
        if (t.dirs[i]) bd_rmdir_empty(t.paths[i]);
        else bd_remove(t.paths[i]);
        free(t.paths[i]);
    }
    free(t.paths);
    free(t.dirs);
    bd_rmdir_empty(root);
}

#endif
