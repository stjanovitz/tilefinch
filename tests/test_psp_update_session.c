#include "tilefinch/psp_update_session.h"
#include "tilefinch/update_history.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

int main(void)
{
    TilefinchInstallPaths paths = {0};
    snprintf(paths.slot_name, sizeof(paths.slot_name), "%s", "slot-a");
    CHECK(psp_update_session_current_slot(&paths)
          == TILEFINCH_UPDATE_SLOT_A);
    snprintf(paths.slot_name, sizeof(paths.slot_name), "%s", "slot-b");
    CHECK(psp_update_session_current_slot(&paths)
          == TILEFINCH_UPDATE_SLOT_B);
    snprintf(paths.slot_name, sizeof(paths.slot_name), "%s", "standalone");
    CHECK(psp_update_session_current_slot(&paths)
          == TILEFINCH_UPDATE_SLOT_NONE);
    char metadata[256];
    CHECK(psp_update_session_metadata_url(NULL, metadata, sizeof(metadata))
          && strstr(metadata, "/releases/latest/download/") != NULL);
    char history_url[256];
    CHECK(tilefinch_update_history_url(
              "stjanovitz", "tilefinch", history_url,
              sizeof(history_url))
          && strstr(history_url, "api.github.com/repos/") != NULL
          && strstr(history_url, "/releases?per_page=9") != NULL);
    char previous_tag[16];
    snprintf(previous_tag, sizeof(previous_tag), "%s", "v0.1.4");
    PspUpdateSessionOptions previous = {
        .channel = BROWSER_UPDATE_CHANNEL_STABLE,
        .release_tag = previous_tag,
        .allow_downgrade = true
    };
    CHECK(psp_update_session_metadata_url(
              &previous, metadata, sizeof(metadata))
          && strstr(metadata, "/releases/download/v") != NULL
          && strstr(metadata, "/tilefinch-update-v1.tfum") != NULL);
    previous.release_tag = "../bad";
    CHECK(!psp_update_session_metadata_url(
        &previous, metadata, sizeof(metadata)));
    PspUpdateSessionOptions signed_test = {
        .channel = BROWSER_UPDATE_CHANNEL_STABLE,
        .signed_metadata_url_override =
            "https://127.0.0.1:8443/tilefinch-update-v1.tfum"
    };
    CHECK(psp_update_session_metadata_url(
              &signed_test, metadata, sizeof(metadata))
          && strcmp(metadata, signed_test.signed_metadata_url_override) == 0);
    signed_test.signed_metadata_url_override =
        "http://127.0.0.1:8443/tilefinch-update-v1.tfum";
    CHECK(!psp_update_session_metadata_url(
        &signed_test, metadata, sizeof(metadata)));
    PspUpdateSessionOptions beta = {
        .channel = BROWSER_UPDATE_CHANNEL_BETA
    };
    CHECK(psp_update_session_metadata_url(
              &beta, metadata, sizeof(metadata))
          && strstr(metadata, "/releases/download/beta/") != NULL);
    PspUpdateSessionOptions developer = {
        .channel = BROWSER_UPDATE_CHANNEL_DEVELOPER,
        .developer_metadata_url =
            "https://192.0.2.1/tilefinch-dev/update.tfum",
        .developer_package_url =
            "https://1drv.ms/u/s!package-token"
    };
    CHECK(psp_update_session_metadata_url(
              &developer, metadata, sizeof(metadata))
          && strcmp(metadata, developer.developer_metadata_url) == 0);
    /* Developer ships an unsigned package, so a cleartext endpoint is
       refused outright rather than merely warned about. */
    developer.developer_metadata_url =
        "http://192.0.2.1/tilefinch-dev/update.tfum";
    CHECK(!psp_update_session_metadata_url(
        &developer, metadata, sizeof(metadata)));
    developer.developer_metadata_url =
        "https://192.0.2.1/tilefinch-dev/update.tfum";
    developer.developer_metadata_url =
        "https://1drv.ms/u/s!metadata-token";
    CHECK(psp_update_session_metadata_url(
              &developer, metadata, sizeof(metadata))
          && strcmp(
                 metadata,
                 "https://1drv.ms/u/s!metadata-token?download=1") == 0);
    developer.developer_metadata_url =
        "https://user@example.test/update.tfum";
    CHECK(!psp_update_session_metadata_url(
        &developer, metadata, sizeof(metadata)));

    Budget budget;
    budget_init(&budget, 2u * 1024u * 1024u);
    PspUiState ui;
    psp_ui_init(&ui);
    /* Update text shares storage with other screens' text, so it is only
       published while the Update screen is showing. */
    PspUpdateSession session = {0};
    {
        PspUiState options;
        psp_ui_init(&options);
        options.screen = PSP_UI_SCREEN_OPTIONS;
        snprintf(options.network_profile_label,
                 sizeof(options.network_profile_label), "%s", "Home Wi-Fi");
        options.network_profile_label_valid = true;
        paths.slotted = false;
        (void) psp_update_session_initialize(
            &session, &budget, &paths, NULL, &options);
        CHECK(options.network_profile_label_valid
              && strcmp(options.network_profile_label, "Home Wi-Fi") == 0);
        psp_update_session_refresh_ui(&session, &options);
        CHECK(strcmp(options.network_profile_label, "Home Wi-Fi") == 0);
        psp_update_session_destroy(&session);
        memset(&session, 0, sizeof(session));
    }
    ui.screen = PSP_UI_SCREEN_UPDATE;
    CHECK(!psp_update_session_initialized(&session));
    CHECK(!psp_update_session_available(&session));
    CHECK(!psp_update_session_active(&session));
    CHECK(!psp_update_session_cancel(&session));

    paths.slotted = false;
    CHECK(!psp_update_session_initialize(
        &session, &budget, &paths, NULL, &ui));
    CHECK(psp_update_session_initialized(&session));
    CHECK(!psp_update_session_available(&session));
    CHECK(strcmp(
        ui.update_status, "SIGNED UPDATES ARE NOT CONFIGURED") == 0);
    CHECK(psp_update_session_primary(&session, &paths)
          == PSP_UPDATE_PRIMARY_NONE);

    psp_update_session_destroy(&session);
    CHECK(!psp_update_session_initialized(&session));
    CHECK(budget.current == 0);

    /*
     * A slotted session reclaims a stranded update/package.part at startup
     * whenever the journal shows no trial in flight. That covers a download
     * abandoned by a crash and an install that completed but was cut before
     * it removed its own part file.
     */
    char root[] = "/tmp/tilefinch-update-session.XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    char data[512], part[640];
    snprintf(data, sizeof(data), "%s/data", root);
    CHECK(mkdir(data, 0700) == 0);
    TilefinchInstallPaths slotted = {0};
    slotted.slotted = true;
    snprintf(slotted.slot_name, sizeof(slotted.slot_name), "%s", "slot-a");
    snprintf(slotted.program_dir, sizeof(slotted.program_dir),
             "%s/slot-a", root);
    snprintf(slotted.install_root, sizeof(slotted.install_root), "%s", root);
    snprintf(slotted.data_dir, sizeof(slotted.data_dir), "%s", data);
    CHECK(tilefinch_install_data_path(
        &slotted, "update/package.part", part, sizeof(part)));

    /* No journal at all: the state defaults to no trial, so the part goes. */
    PspUpdateSession reclaim = {0};
    (void) psp_update_session_initialize(
        &reclaim, &budget, &slotted, NULL, &ui);
    FILE *stranded = fopen(part, "wb");
    CHECK(stranded != NULL && fputs("stranded download", stranded) >= 0
          && fclose(stranded) == 0);
    psp_update_session_destroy(&reclaim);
    memset(&reclaim, 0, sizeof(reclaim));
    (void) psp_update_session_initialize(
        &reclaim, &budget, &slotted, NULL, &ui);
    CHECK(fopen(part, "rb") == NULL);
    psp_update_session_destroy(&reclaim);

    /* A pending trial is left alone: that session has not settled yet. */
    TilefinchUpdateState pending = {
        .generation = 3,
        .active_slot = TILEFINCH_UPDATE_SLOT_A,
        .pending_slot = TILEFINCH_UPDATE_SLOT_B,
        .trial = TILEFINCH_UPDATE_TRIAL_PENDING,
        .installed_sequence = 42
    };
    CHECK(tilefinch_update_journal_store(data, &pending, NULL, NULL));
    stranded = fopen(part, "wb");
    CHECK(stranded != NULL && fputs("mid-trial", stranded) >= 0
          && fclose(stranded) == 0);
    memset(&reclaim, 0, sizeof(reclaim));
    (void) psp_update_session_initialize(
        &reclaim, &budget, &slotted, NULL, &ui);
    FILE *kept = fopen(part, "rb");
    CHECK(kept != NULL && fclose(kept) == 0);
    psp_update_session_destroy(&reclaim);
    CHECK(budget.current == 0);

    /* Developer is a deliberate local trust mode and therefore remains
       usable in a contributor build with no embedded release root. */
    PspUpdateSession developer_session = {0};
    developer.developer_metadata_url =
        "https://192.0.2.1/tilefinch-dev/update.tfum";
    developer.developer_package_url =
        "https://1drv.ms/u/s!package-token";
    CHECK(psp_update_session_initialize(
              &developer_session, &budget, &slotted, &developer, &ui)
          && psp_update_session_available(&developer_session)
          && strcmp(
                 ui.update_status,
                 "DEVELOPER URL - UNSIGNED TRIAL") == 0);
    psp_update_session_destroy(&developer_session);
    CHECK(budget.current == 0);

    /* IS-01: the install target comes from the journal on the card at the
       moment of the install, never from the copy read at initialization,
       and is never the slot this process runs from. */
    TilefinchInstallPaths running_b = slotted;
    snprintf(running_b.slot_name, sizeof(running_b.slot_name), "%s", "slot-b");
    snprintf(running_b.program_dir, sizeof(running_b.program_dir),
             "%s/slot-b", root);
    TilefinchUpdateState started = {
        .generation = 4,
        .active_slot = TILEFINCH_UPDATE_SLOT_A,
        .pending_slot = TILEFINCH_UPDATE_SLOT_B,
        .trial = TILEFINCH_UPDATE_TRIAL_STARTED,
        .installed_sequence = 42,
        .candidate_sequence = 43
    };
    CHECK(tilefinch_update_journal_store(data, &started, NULL, NULL));
    PspUpdateSession trial_session = {0};
    CHECK(psp_update_session_initialize(
              &trial_session, &budget, &running_b, &developer, &ui));
    /* Slot B is on its unconfirmed first start: nothing is replaceable. */
    CHECK(psp_update_session_install_target(&trial_session)
          == TILEFINCH_UPDATE_SLOT_NONE);
    /* The browser's health confirmation then commits from its own copy. */
    TilefinchUpdateState healthy = started;
    CHECK(tilefinch_update_state_confirm_healthy(&healthy)
          && tilefinch_update_journal_store(data, &healthy, NULL, NULL));
    CHECK(psp_update_session_install_target(&trial_session)
              == TILEFINCH_UPDATE_SLOT_A
          && trial_session.state.generation == healthy.generation
          && trial_session.state.active_slot == TILEFINCH_UPDATE_SLOT_B
          && trial_session.state.installed_sequence == 43);
    psp_update_session_destroy(&trial_session);
    /* Safe start runs the previous slot A while the journal's active slot
       is B: A is the only other slot, and it is the running one. */
    PspUpdateSession safe_session = {0};
    CHECK(psp_update_session_initialize(
              &safe_session, &budget, &slotted, &developer, &ui));
    CHECK(psp_update_session_install_target(&safe_session)
          == TILEFINCH_UPDATE_SLOT_NONE);
    psp_update_session_destroy(&safe_session);
    CHECK(budget.current == 0);

    /* IS-10: the recovery screen was answered with O or timed out, so the
       launcher started slot A and recorded the decline. The Update page
       offers the screen's X (TRY AGAIN) and Square (DISCARD). */
    TilefinchUpdateState declined = {
        .generation = 10,
        .active_slot = TILEFINCH_UPDATE_SLOT_A,
        .pending_slot = TILEFINCH_UPDATE_SLOT_B,
        .trial = TILEFINCH_UPDATE_TRIAL_STARTED,
        .installed_sequence = 42,
        .candidate_sequence = 43
    };
    char marker[640];
    snprintf(marker, sizeof(marker), "%s/update-declined", data);
    CHECK(tilefinch_update_journal_store(data, &declined, NULL, NULL)
          && tilefinch_update_decline_store(data, &declined, NULL, NULL));
    PspUpdateSession choice = {0};
    CHECK(psp_update_session_initialize(
              &choice, &budget, &slotted, &developer, &ui)
          && psp_update_session_trial_declined(&choice));
    psp_update_session_refresh_ui(&choice, &ui);
    CHECK(strcmp(ui.update_status, "UPDATE DID NOT FINISH STARTING") == 0
          && strcmp(ui.update_primary_label, "TRY AGAIN") == 0
          && ui.update_primary_enabled && ui.update_discard_enabled
          && !ui.update_cancel_enabled);
    /* The journal is re-read: a write it cannot make leaves the choice. */
    CHECK(chmod(data, 0500) == 0);
    bool refused = !psp_update_session_resolve_trial(&choice, true);
    CHECK(chmod(data, 0700) == 0);
    CHECK(refused && psp_update_session_trial_declined(&choice));
    CHECK(psp_update_session_resolve_trial(&choice, true));
    TilefinchUpdateState on_card;
    struct stat marker_info;
    CHECK(tilefinch_update_journal_load(data, &on_card, NULL)
          && on_card.generation == 11
          && on_card.trial == TILEFINCH_UPDATE_TRIAL_PENDING
          && on_card.pending_slot == TILEFINCH_UPDATE_SLOT_B
          && stat(marker, &marker_info) != 0
          && !psp_update_session_trial_declined(&choice));
    psp_update_session_refresh_ui(&choice, &ui);
    CHECK(strcmp(ui.update_status, "READY TO RESTART") == 0
          && strcmp(ui.update_primary_label, "RESTART TO UPDATE") == 0
          && ui.update_primary_enabled && !ui.update_discard_enabled
          && psp_update_session_primary(&choice, &slotted)
                 == PSP_UPDATE_PRIMARY_RESTART_REQUIRED);
    psp_update_session_destroy(&choice);

    /* The retried trial failed again and was declined again: DISCARD. */
    CHECK(tilefinch_update_state_start_trial(&on_card)
          && tilefinch_update_journal_store(data, &on_card, NULL, NULL));
    memset(&choice, 0, sizeof(choice));
    CHECK(psp_update_session_initialize(
              &choice, &budget, &slotted, &developer, &ui)
          && psp_update_session_resolve_trial(&choice, false)
          && tilefinch_update_journal_load(data, &on_card, NULL)
          && on_card.trial == TILEFINCH_UPDATE_TRIAL_NONE
          && on_card.pending_slot == TILEFINCH_UPDATE_SLOT_NONE
          && on_card.active_slot == TILEFINCH_UPDATE_SLOT_A
          && !psp_update_session_trial_declined(&choice)
          && !choice.trial_retry_requested);
    psp_update_session_refresh_ui(&choice, &ui);
    CHECK(strcmp(ui.update_primary_label, "CHECK NOW") == 0
          && !ui.update_discard_enabled);
    psp_update_session_destroy(&choice);

    /* A journal that moved on since the page showed the choice wins. */
    declined.generation = on_card.generation + 1u;
    CHECK(tilefinch_update_journal_store(data, &declined, NULL, NULL));
    memset(&choice, 0, sizeof(choice));
    CHECK(psp_update_session_initialize(
              &choice, &budget, &slotted, &developer, &ui)
          && psp_update_session_trial_declined(&choice));
    TilefinchUpdateState moved = declined;
    CHECK(tilefinch_update_state_discard_trial(&moved)
          && tilefinch_update_journal_store(data, &moved, NULL, NULL));
    CHECK(!psp_update_session_resolve_trial(&choice, true)
          && tilefinch_update_journal_load(data, &on_card, NULL)
          && on_card.generation == moved.generation
          && on_card.trial == TILEFINCH_UPDATE_TRIAL_NONE);
    psp_update_session_destroy(&choice);

    /* The trial boot itself (running the pending slot) offers no choice. */
    declined.generation = moved.generation + 1u;
    CHECK(tilefinch_update_journal_store(data, &declined, NULL, NULL));
    memset(&choice, 0, sizeof(choice));
    CHECK(psp_update_session_initialize(
              &choice, &budget, &running_b, &developer, &ui)
          && !psp_update_session_trial_declined(&choice)
          && !psp_update_session_resolve_trial(&choice, false));
    psp_update_session_refresh_ui(&choice, &ui);
    CHECK(!ui.update_discard_enabled);
    psp_update_session_destroy(&choice);
    CHECK(budget.current == 0);

    remove(part);
    char scratch[640];
    snprintf(scratch, sizeof(scratch), "%s/update", data);
    CHECK(rmdir(scratch) == 0);
    for (unsigned copy = 0; copy < 2; copy++) {
        snprintf(scratch, sizeof(scratch), "%s/update-state.%u", data, copy);
        remove(scratch);
        snprintf(scratch, sizeof(scratch),
                 "%s/update-state.%u.tmp", data, copy);
        remove(scratch);
    }
    remove(marker);
    snprintf(scratch, sizeof(scratch), "%s/update-declined.tmp", data);
    remove(scratch);
    CHECK(rmdir(data) == 0);
    CHECK(rmdir(root) == 0);

    puts("psp-update-session-tests: all checks passed");
    return 0;
}
