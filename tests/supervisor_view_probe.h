/* A stand-in for the PSP loading supervisor's UI copy (AU-05 follow-up).
 * A scope copies the UI state by value when it begins, the native-view
 * pointer included, and the callback thread draws that copy until the scope
 * ends. When the navigation pump begins a scope again while a preview sheet
 * is up (an app preview, a file offer, a ZIP install), the copy shows the
 * sheet; answering the sheet frees the preview. The probe keeps such a copy,
 * installs itself as the store's release_view hook as the PSP browser does
 * (psp_work_cooperate_release_view, minus the fence), and presents the copy
 * as the supervisor would. Test-only.
 */
#ifndef TILEFINCH_TEST_SUPERVISOR_VIEW_PROBE_H
#define TILEFINCH_TEST_SUPERVISOR_VIEW_PROBE_H

#include "tilefinch/psp_offline_store.h"
#include "tilefinch/psp_ui.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define SUPERVISOR_VIEW_PROBE_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) && !defined(SUPERVISOR_VIEW_PROBE_ASAN)
#define SUPERVISOR_VIEW_PROBE_ASAN 1
#endif

static struct {
    PspUiState copy;
    unsigned releases;
    unsigned dropped;
    uint16_t pixels[480u * 272u];
} view_probe;

static void view_probe_release(const void *view)
{
    view_probe.releases++;
    if (psp_ui_release_view(&view_probe.copy, view)) view_probe.dropped++;
}

/* The scope begins over `live`, which shows a sheet. */
static void view_probe_begin(PspOfflineStore *store, const PspUiState *live)
{
    memset(&view_probe, 0, sizeof(view_probe));
    view_probe.copy = *live;
    store->release_view = view_probe_release;
}

static void view_probe_end(PspOfflineStore *store)
{
    store->release_view = NULL;
}

/* One supervisor present after `freed` went away: true when the copy no
   longer points at it. Under AddressSanitizer the copy is drawn regardless,
   so a dangling sheet is reported as the heap-use-after-free it is;
   elsewhere it is drawn only when it is safe to. */
static bool view_probe_present(const void *freed)
{
    const PspUiState *copy = &view_probe.copy;
    bool dangling = (const void *) copy->zip_install_preview == freed
        && (copy->screen == PSP_UI_SCREEN_ZIP_INSTALL
            || copy->screen == PSP_UI_SCREEN_FILE_DOWNLOAD
            || copy->screen == PSP_UI_SCREEN_OFFLINE_APP_PREVIEW);
#ifndef SUPERVISOR_VIEW_PROBE_ASAN
    if (dangling) return false;
#endif
    psp_ui_composite(copy, view_probe.pixels, 480, 272, 480);
    return !dangling;
}

#endif
