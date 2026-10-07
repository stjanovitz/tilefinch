#ifndef TILEFINCH_SCRIPT_TEST_SUPPORT_H
#define TILEFINCH_SCRIPT_TEST_SUPPORT_H

/* Shared scaffolding for the compiled-script cache and offline app test
   binaries (test_module_bytecode.c, test_classic_bytecode.c,
   test_lazy_bundle_records.c, test_script_disk_cache.c,
   test_offline_app_limits.c, test_offline_app_launch.c): a one-script page
   fixture and the scratch directories the persistent tiers write. */

#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <lexbor/dom/interfaces/node.h>

#include "tilefinch/budget.h"
#include "tilefinch/document.h"
#include "tilefinch/js_runtime.h"
#include "tilefinch/session.h"
#include "tilefinch/viewport.h"

/* ---- a page with one script element ---- */

typedef struct {
    Budget budget;
    PocDocument document;
    ScriptRuntimeOptions options;
    lxb_dom_node_t *script;
    /* Set up by script_page_fixture_open when it is given a response
       cache size, and then wired into `options`. */
    BrowserSession session;
    bool has_session;
} ScriptPageFixture;

/* The first <script> element at or after `node`, depth first. */
static inline lxb_dom_node_t *script_test_find_script(lxb_dom_node_t *node)
{
    for (; node != NULL; node = node->next) {
        size_t length = 0;
        const char *name = document_element_name(node, &length);
        if (name != NULL && length == 6 && memcmp(name, "script", 6) == 0)
            return node;
        lxb_dom_node_t *nested = script_test_find_script(node->first_child);
        if (nested != NULL) return nested;
    }
    return NULL;
}

/* Parse a 480x272 lab page whose body is `script_tag` (one script element)
   under a fresh 48 MiB Budget with the lexbor allocator installed, and
   prepare deferred top-level runtime options for it. A nonzero
   `session_cache_bytes` also opens a BrowserSession with that response
   cache and sets it as `options.session`; with zero the caller attaches
   its own session, if any. */
static inline bool script_page_fixture_open(ScriptPageFixture *fixture,
                                            const char *script_tag,
                                            size_t session_cache_bytes)
{
    char html[256];
    int length = snprintf(html, sizeof(html),
                          "<!doctype html><html><body>%s</body></html>",
                          script_tag);
    memset(fixture, 0, sizeof(*fixture));
    if (length <= 0 || (size_t) length >= sizeof(html)) return false;
    budget_init(&fixture->budget, 48u * 1024u * 1024u);
    if (!budget_install_lexbor(&fixture->budget)
        || !document_parse(&fixture->document, &fixture->budget, html,
                           (size_t) length, (size_t) length + 1u))
        return false;
    ViewportContext viewport;
    ScriptExecutionPolicy policy;
    if (!viewport_context_init(&viewport, 480, 272, 480, 272)
        || !script_execution_policy_for_profile(
               SCRIPT_EXECUTION_PROFILE_LAB, &policy)) return false;
    if (session_cache_bytes > 0) {
        if (!browser_session_init(&fixture->session, &fixture->budget,
                                  session_cache_bytes)) return false;
        fixture->has_session = true;
    }
    fixture->options = (ScriptRuntimeOptions) {
        .viewport = viewport,
        .execution_policy = policy,
        .defer_document_scripts = true,
        .document_scope = SCRIPT_DOCUMENT_SCOPE_TOP_LEVEL,
        .session = fixture->has_session ? &fixture->session : NULL
    };
    fixture->script = script_test_find_script(
        lxb_dom_interface_node(fixture->document.html));
    return fixture->script != NULL;
}

/* Tear the page (and the fixture's own session) down; true when every
   byte the Budget lent was returned. */
static inline bool script_page_fixture_close(ScriptPageFixture *fixture)
{
    if (fixture->has_session) {
        browser_session_destroy(&fixture->session);
        fixture->has_session = false;
    }
    document_destroy(&fixture->document);
    return fixture->budget.current == 0
        && budget_uninstall_lexbor(&fixture->budget);
}

/* ---- scratch directories ---- */

/* A fresh `<base>/<prefix>-XXXXXX` directory, where base is
   TILEFINCH_TEST_SCRATCH_DIR (CTest sets it inside the build tree) or
   ./test-scratch. Refuses a device path such as ms0:/: a test's scratch
   tree is always a host directory. */
static inline bool script_test_make_scratch_root(char *out, size_t capacity,
                                                 const char *prefix)
{
    const char *base = getenv("TILEFINCH_TEST_SCRATCH_DIR");
    if (base == NULL || base[0] == '\0') base = "test-scratch";
    (void) mkdir(base, 0777);
    int length = snprintf(out, capacity, "%s/%s-XXXXXX", base, prefix);
    if (length <= 0 || (size_t) length >= capacity) return false;
    if (mkdtemp(out) == NULL) return false;
    return strchr(out, ':') == NULL;
}

/* Remove `directory` and everything under it, subdirectories included.
   Best effort: a test's next run starts from a fresh directory anyway. */
static inline void script_test_remove_tree(const char *directory)
{
    DIR *dir = opendir(directory);
    if (dir != NULL) {
        struct dirent *entry;
        char path[1024];
        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0
                || strcmp(entry->d_name, "..") == 0) continue;
            snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
            struct stat info;
            if (lstat(path, &info) == 0 && S_ISDIR(info.st_mode))
                script_test_remove_tree(path);
            else
                (void) remove(path);
        }
        closedir(dir);
    }
    (void) rmdir(directory);
}

#endif
