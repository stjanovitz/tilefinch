#undef NDEBUG
#include "tilefinch/psp_voice_component_session.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Replace only the already-downloaded transport boundary. The production
   session, installer allocation/open/cancel and UI publication stay real. */
static unsigned client_token, create_calls;
static TilefinchUpdateClientSnapshot downloaded;
static bool client_snapshot(const TilefinchUpdateClient *client,
                            TilefinchUpdateClientSnapshot *snapshot)
{
    assert(client == (const TilefinchUpdateClient *) &client_token);
    *snapshot = downloaded;
    return true;
}
static const uint8_t *client_envelope(const TilefinchUpdateClient *client,
                                     size_t *length)
{
    assert(client == (const TilefinchUpdateClient *) &client_token);
    static const uint8_t envelope[] = {'m'};
    *length = sizeof(envelope);
    return envelope;
}
static bool client_pump(TilefinchUpdateClient *client, unsigned maximum_us)
{
    (void) maximum_us;
    assert(client == (TilefinchUpdateClient *) &client_token);
    return true;
}
static void client_destroy(TilefinchUpdateClient *client)
{
    assert(client == NULL || client == (TilefinchUpdateClient *) &client_token);
}
static TilefinchVoiceComponentInstall *create_install(
    Budget *budget, const TilefinchVoiceComponentInstallOptions *options)
{
    create_calls++;
    return tilefinch_voice_component_install_create(budget, options);
}
#define tilefinch_update_client_snapshot client_snapshot
#define tilefinch_update_client_envelope client_envelope
#define tilefinch_update_client_pump client_pump
#define tilefinch_update_client_destroy client_destroy
#define tilefinch_voice_component_install_create create_install
#include "../src/psp_voice_component_session.c"

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "component session failure at %d: %s\n", __LINE__, \
            #condition); return false; } } while (0)

static void session_init(PspVoiceComponentSession *session, Budget *budget,
                         const char *root)
{
    budget_init(budget, 1024u * 1024u);
    *session = (PspVoiceComponentSession) {
        .budget = budget, .available = true, .initialized = true,
        .auto_install = true,
        .client = (TilefinchUpdateClient *) &client_token
    };
    snprintf(session->package_path, sizeof(session->package_path),
             "%s/model.part", root);
    downloaded = (TilefinchUpdateClientSnapshot) {
        .phase = TILEFINCH_UPDATE_CLIENT_DOWNLOADED,
        .manifest = {.package_format = TILEFINCH_UPDATE_PACKAGE_VOICE}
    };
    create_calls = 0;
}

static bool rejected_install(const TilefinchInstallPaths *paths, int failure)
{
    Budget budget;
    PspVoiceComponentSession session;
    session_init(&session, &budget, paths->install_root);
    if (failure >= 0) budget_inject_failure_after(&budget, (size_t) failure);
    CHECK(psp_voice_component_session_pump(&session, paths));
    budget_clear_failure_injection(&budget);
    for (unsigned turn = 0; turn < 8; turn++)
        CHECK(psp_voice_component_session_pump(&session, paths));
    PspUiState ui = {0};
    psp_voice_component_session_refresh_ui(&session, &ui);
    bool stopped = create_calls == 1u && !session.auto_install
        && session.installer == NULL
        && ui.voice_component_phase == PSP_UI_VOICE_COMPONENT_ERROR;
    bool clean = budget.current == 0 && budget_active_allocations(&budget, NULL) == 0;
    psp_voice_component_session_destroy(&session);
    if (!stopped || !clean)
        fprintf(stderr, "install refusal %d: attempts=%u stopped=%d clean=%d\n",
                failure, create_calls, stopped, clean);
    return stopped && clean && budget.current == 0;
}

static bool retry_cancel(const TilefinchInstallPaths *paths)
{
    Budget budget;
    PspVoiceComponentSession session;
    session_init(&session, &budget, paths->install_root);
    FILE *file = fopen(session.package_path, "wb");
    CHECK(file != NULL && fputs("synthetic component", file) >= 0);
    CHECK(fclose(file) == 0);
    session.install_snapshot.phase = TILEFINCH_UPDATE_INSTALL_CANCELLED;
    size_t owned = 0;
    for (unsigned attempt = 0; attempt < 8; attempt++) {
        CHECK(psp_voice_component_session_primary(&session, paths)
              == PSP_VOICE_COMPONENT_PRIMARY_NONE);
        CHECK(session.installer != NULL);
        PspUiState ui = {0};
        psp_voice_component_session_refresh_ui(&session, &ui);
        CHECK(psp_voice_component_session_active(&session)
              && ui.voice_component_phase == PSP_UI_VOICE_COMPONENT_INSTALLING);
        if (attempt == 0) owned = budget.current;
        CHECK(owned != 0 && budget.current == owned);
        if ((attempt & 1u) == 0) {
            CHECK(psp_voice_component_session_cancel(&session));
            CHECK(psp_voice_component_session_pump(&session, paths));
            CHECK(session.install_snapshot.phase == TILEFINCH_UPDATE_INSTALL_CANCELLED);
        } else {
            /* The synthetic file is intentionally not a valid package. */
            for (unsigned turn = 0; turn < 8
                 && session.install_snapshot.phase < TILEFINCH_UPDATE_INSTALL_COMPLETE;
                 turn++)
                CHECK(psp_voice_component_session_pump(&session, paths));
            CHECK(session.install_snapshot.phase == TILEFINCH_UPDATE_INSTALL_ERROR);
        }
        if (budget.current != 0)
            fprintf(stderr, "terminal installer retains %zu bytes\n", budget.current);
        CHECK(session.installer == NULL && budget.current == 0
              && budget_active_allocations(&budget, NULL) == 0);
        psp_voice_component_session_refresh_ui(&session, &ui);
        CHECK(ui.voice_component_phase == PSP_UI_VOICE_COMPONENT_ERROR);
        unsigned attempts = create_calls;
        CHECK(psp_voice_component_session_pump(&session, paths));
        CHECK(create_calls == attempts && budget.current == 0);
    }
    psp_voice_component_session_destroy(&session);
    CHECK(budget.current == 0 && budget_active_allocations(&budget, NULL) == 0
          && budget_categories_reconcile(&budget));
    char path[TILEFINCH_INSTALL_PATH_LIMIT];
    snprintf(path, sizeof(path), "%s/model.part", paths->install_root);
    CHECK(remove(path) == 0);
    return true;
}

int main(void)
{
    char root[] = "/tmp/tilefinch-component-session.XXXXXX";
    assert(mkdtemp(root) != NULL);
    TilefinchInstallPaths paths = {.slotted = true};
    snprintf(paths.install_root, sizeof(paths.install_root), "%s", root);
    bool ok = true;
    for (int failure = -1; failure < 3; failure++)
        ok = rejected_install(&paths, failure) && ok;
    ok = retry_cancel(&paths) && ok;
    /* Even a failing-before retry test leaves only its synthetic file. */
    char path[TILEFINCH_INSTALL_PATH_LIMIT];
    snprintf(path, sizeof(path), "%s/model.part", root);
    (void) remove(path);
    (void) rmdir(root);
    return ok ? 0 : 1;
}
