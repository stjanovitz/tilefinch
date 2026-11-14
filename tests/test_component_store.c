#include "tilefinch/component_store.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "tilefinch/install_paths.h"

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static bool path_in(const char *root, const char *relative,
                    char output[TILEFINCH_INSTALL_PATH_LIMIT])
{
    return tilefinch_component_store_join(
        root, relative, output, TILEFINCH_INSTALL_PATH_LIMIT);
}

static bool make_file(const char *root, const char *relative)
{
    char path[TILEFINCH_INSTALL_PATH_LIMIT];
    static const char byte = 'x';
    return path_in(root, relative, path)
        && tilefinch_component_store_write_file(path, &byte, 1);
}

static bool exists_in(const char *root, const char *relative)
{
    char path[TILEFINCH_INSTALL_PATH_LIMIT];
    return path_in(root, relative, path)
        && tilefinch_component_store_exists(path);
}

int main(void)
{
    char root[] = "/tmp/tilefinch-component-store.XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    char path[TILEFINCH_INSTALL_PATH_LIMIT];
    char other[TILEFINCH_INSTALL_PATH_LIMIT];

    puts("test: join refuses missing arguments and truncation");
    char small[8];
    CHECK(tilefinch_component_store_join("ab", "cd", small, sizeof(small))
          && strcmp(small, "ab/cd") == 0);
    CHECK(!tilefinch_component_store_join("abc", "defg", small, sizeof(small)));
    CHECK(!tilefinch_component_store_join(NULL, "a", small, sizeof(small)));
    CHECK(!tilefinch_component_store_join("a", "b", small, 0));

    puts("test: make_parents creates ancestors only and restores the path");
    CHECK(path_in(root, "a/b/c/leaf", path));
    char before[TILEFINCH_INSTALL_PATH_LIMIT];
    snprintf(before, sizeof(before), "%s", path);
    CHECK(tilefinch_component_store_make_parents(path));
    CHECK(strcmp(path, before) == 0);
    CHECK(exists_in(root, "a/b/c") && !exists_in(root, "a/b/c/leaf"));
    CHECK(tilefinch_component_store_make_parents(path));
    CHECK(path_in(root, "a/b/d", path)
          && tilefinch_component_store_make_directory(path)
          && tilefinch_component_store_make_directory(path)
          && exists_in(root, "a/b/d"));

    puts("test: write_file and file_matches agree on exact contents");
    static const uint8_t digest[32] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    CHECK(path_in(root, "a/READY", path)
          && tilefinch_component_store_write_file(path, digest, 32));
    CHECK(tilefinch_component_store_file_matches(path, digest, 32));
    CHECK(!tilefinch_component_store_file_matches(path, digest, 31));
    uint8_t changed[32];
    memcpy(changed, digest, sizeof(changed));
    changed[31] ^= 1u;
    CHECK(!tilefinch_component_store_file_matches(path, changed, 32));
    CHECK(path_in(root, "a/missing", other)
          && !tilefinch_component_store_file_matches(other, digest, 32));

    puts("test: read_file returns the whole file within its limit only");
    Budget budget;
    budget_init(&budget, 1024u);
    size_t length = 0;
    uint8_t *bytes = tilefinch_component_store_read_file(
        &budget, path, 32, &length);
    CHECK(bytes != NULL && length == 32
          && memcmp(bytes, digest, 32) == 0 && budget.current != 0);
    budget_free(&budget, bytes);
    CHECK(budget.current == 0);
    length = 7;
    CHECK(tilefinch_component_store_read_file(&budget, path, 31, &length)
              == NULL
          && length == 7 && budget.current == 0);
    CHECK(tilefinch_component_store_read_file(&budget, other, 64, &length)
          == NULL);
    CHECK(path_in(root, "a/empty", other)
          && tilefinch_component_store_write_file(other, "", 0)
          && tilefinch_component_store_read_file(&budget, other, 64, &length)
                 == NULL);
    Budget tiny;
    budget_init(&tiny, 16u);
    CHECK(tilefinch_component_store_read_file(&tiny, path, 64, &length)
              == NULL
          && tiny.current == 0);

    puts("test: rename stays within one directory");
    CHECK(path_in(root, "a/b/part", path) && make_file(root, "a/b/part"));
    CHECK(path_in(root, "a/b/pack", other)
          && tilefinch_component_store_rename(path, other)
          && !exists_in(root, "a/b/part") && exists_in(root, "a/b/pack"));
    /* The firmware would report success here and leave the file behind
       under the target's name; the guard refuses before calling rename. */
    CHECK(path_in(root, "a/b/pack", path) && path_in(root, "a/pack", other));
    errno = 0;
    CHECK(!tilefinch_component_store_rename(path, other) && errno == EXDEV);
    CHECK(exists_in(root, "a/b/pack") && !exists_in(root, "a/pack"));
    CHECK(path_in(root, "a/b/c/pack", other));
    errno = 0;
    CHECK(!tilefinch_component_store_rename(path, other) && errno == EXDEV);
    /* Same-length parents that differ are still different directories. */
    CHECK(path_in(root, "a/b/d/pack", path) && make_file(root, "a/b/d/pack")
          && path_in(root, "a/b/c/pack", other));
    errno = 0;
    CHECK(!tilefinch_component_store_rename(path, other) && errno == EXDEV
          && exists_in(root, "a/b/d/pack"));
    /* Directory promotion: a sibling directory rename is allowed, and a
       missing source reports rename's own ENOENT. */
    CHECK(path_in(root, "a/b/d", path) && path_in(root, "a/b/active", other)
          && tilefinch_component_store_rename(path, other)
          && exists_in(root, "a/b/active/pack"));
    errno = 0;
    CHECK(!tilefinch_component_store_rename(path, other) && errno == ENOENT);
    errno = 0;
    CHECK(!tilefinch_component_store_rename(NULL, other) && errno == EINVAL);

    puts("test: remove_tree is bounded by depth and entry count");
    CHECK(path_in(root, "missing", path)
          && tilefinch_component_store_remove_tree(path, 4u, 24u));
    CHECK(path_in(root, "wide", path)
          && tilefinch_component_store_make_directory(path)
          && make_file(root, "wide/1") && make_file(root, "wide/2")
          && make_file(root, "wide/3"));
    CHECK(path_in(root, "wide", path)
          && !tilefinch_component_store_remove_tree(path, 4u, 2u)
          && exists_in(root, "wide"));
    CHECK(tilefinch_component_store_remove_tree(path, 4u, 3u)
          && !exists_in(root, "wide"));
    CHECK(path_in(root, "deep/1/2/leaf", path)
          && tilefinch_component_store_make_parents(path)
          && make_file(root, "deep/1/2/leaf"));
    CHECK(path_in(root, "deep", path)
          && !tilefinch_component_store_remove_tree(path, 1u, 24u)
          && exists_in(root, "deep/1/2/leaf"));
    CHECK(tilefinch_component_store_remove_tree(path, 2u, 24u)
          && !exists_in(root, "deep"));
    CHECK(!tilefinch_component_store_remove_tree(NULL, 4u, 24u));

    CHECK(tilefinch_component_store_remove_tree(root, 8u, 64u));
    CHECK(!tilefinch_component_store_exists(root));
    puts("component store tests passed");
    return 0;
}
