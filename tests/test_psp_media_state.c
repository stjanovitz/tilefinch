#include "tilefinch/psp_media_state.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "PSP MEDIA STATE CHECK failed at %s:%d: %s\n",     \
                __FILE__, __LINE__, #condition);                             \
        return false;                                                        \
    }                                                                        \
} while (0)

static bool apply(PspMediaMachine *machine, PspMediaEvent event)
{
    PspMediaDecision decision =
        psp_media_machine_transition(machine, &event);
    CHECK(decision.handled);
    CHECK(psp_media_machine_violations(&decision.next) == 0);
    *machine = decision.next;
    return true;
}

static bool open_to_priming(
    PspMediaMachine *machine, bool audio, bool autoplay)
{
    CHECK(apply(machine, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_OPEN,
        .has_separate_audio = audio,
        .autoplay = autoplay
    }));
    CHECK(machine->state == PSP_MEDIA_SESSION_OPENING);
    unsigned completions = audio ? 7u : 5u;
    for (unsigned at = 0; at < completions; at++) {
        CHECK(apply(machine, (PspMediaEvent) {
            .type = PSP_MEDIA_EVENT_OPEN_PHASE_COMPLETE,
            .has_separate_audio = audio
        }));
    }
    CHECK(machine->state == PSP_MEDIA_SESSION_PRIMING);
    CHECK(machine->pipeline == PSP_MEDIA_PIPELINE_FULL);
    return true;
}

static bool quiesce_successfully(PspMediaMachine *machine)
{
    static const PspMediaEventType events[] = {
        PSP_MEDIA_EVENT_ADMISSION_STOPPED,
        PSP_MEDIA_EVENT_TRANSPORT_CANCELLED,
        PSP_MEDIA_EVENT_BACKEND_QUIESCED
    };
    for (size_t at = 0; at < sizeof(events) / sizeof(events[0]); at++)
        CHECK(apply(machine, (PspMediaEvent) {.type = events[at]}));
    return true;
}

static bool test_open_priming_and_play(void)
{
    PspMediaMachine machine = psp_media_machine_initial();
    CHECK(psp_media_machine_violations(&machine) == 0);
    CHECK(open_to_priming(&machine, true, true));
    CHECK(machine.resume_target == PSP_MEDIA_RESUME_PLAYING);
    CHECK(apply(&machine, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_PRIME_READY
    }));
    CHECK(machine.state == PSP_MEDIA_SESSION_PLAYING);
    PspMediaUiProjection ui = psp_media_machine_project_ui(&machine);
    CHECK(ui.mode == PSP_MEDIA_UI_PLAYING);
    CHECK(ui.visible && ui.controls_enabled && ui.playing);
    CHECK(ui.play_pause_enabled && ui.seek_enabled);
    return true;
}

static bool test_one_shot_presentation_boundaries(void)
{
    PspMediaMachine machine = psp_media_machine_initial();
    CHECK(open_to_priming(&machine, true, true));
    CHECK(apply(&machine, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_PAUSE_AFTER_FRAME
    }));
    CHECK(machine.pause_after_frame);
    CHECK(apply(&machine, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_FRAME_DISPLAYED
    }));
    CHECK(machine.state == PSP_MEDIA_SESSION_PAUSED);
    CHECK(!machine.pause_after_frame);

    CHECK(apply(&machine, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_SEEK
    }));
    CHECK(apply(&machine, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_PREVIEW_STARTED
    }));
    PspMediaUiProjection preview =
        psp_media_machine_project_ui(&machine);
    CHECK(preview.preview_active && !preview.playing);
    CHECK(apply(&machine, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_PREVIEW_ENDED
    }));
    CHECK(!psp_media_machine_project_ui(&machine).preview_active);
    CHECK(apply(&machine, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_SEEK_COMPLETE
    }));
    CHECK(apply(&machine, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_PRIME_READY
    }));
    CHECK(apply(&machine, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_PLAYBACK_ENDED
    }));
    PspMediaUiProjection ui = psp_media_machine_project_ui(&machine);
    CHECK(machine.state == PSP_MEDIA_SESSION_PAUSED);
    CHECK(ui.ended && !ui.playing);

    /* The open-time still-frame boundary is superseded when the user asks to
       play before that first frame arrives. This is the real-device startup
       ordering: Opening commits Priming, arms the boundary, then validation
       (or X) supplies PLAY. */
    machine = psp_media_machine_initial();
    CHECK(open_to_priming(&machine, true, false));
    CHECK(apply(&machine, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_PAUSE_AFTER_FRAME
    }));
    CHECK(machine.pause_after_frame);
    CHECK(apply(&machine, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_PLAY
    }));
    CHECK(!machine.pause_after_frame);
    CHECK(machine.resume_target == PSP_MEDIA_RESUME_PLAYING);
    CHECK(apply(&machine, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_PRIME_READY
    }));
    CHECK(machine.state == PSP_MEDIA_SESSION_PLAYING);
    return true;
}

static bool test_video_only_skips_audio_open_phases(void)
{
    PspMediaMachine machine = psp_media_machine_initial();
    CHECK(apply(&machine, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_OPEN,
        .autoplay = false,
        .has_separate_audio = false
    }));
    CHECK(machine.opening_phase == PSP_MEDIA_OPEN_RESOLVING);
    CHECK(apply(&machine, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_OPEN_PHASE_COMPLETE
    }));
    CHECK(machine.opening_phase == PSP_MEDIA_OPEN_DECODER_PREPARE);
    CHECK(apply(&machine, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_OPEN_PHASE_COMPLETE
    }));
    CHECK(machine.opening_phase == PSP_MEDIA_OPEN_VIDEO_RANGE);
    CHECK(apply(&machine, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_OPEN_PHASE_COMPLETE
    }));
    CHECK(machine.opening_phase == PSP_MEDIA_OPEN_VIDEO_DEMUX);
    CHECK(apply(&machine, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_OPEN_PHASE_COMPLETE
    }));
    CHECK(machine.opening_phase == PSP_MEDIA_OPEN_PLAYBACK_CREATE);
    CHECK(apply(&machine, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_OPEN_PHASE_COMPLETE
    }));
    CHECK(machine.state == PSP_MEDIA_SESSION_PRIMING);
    CHECK(machine.resume_target == PSP_MEDIA_RESUME_PAUSED);
    PspMediaUiProjection ui = psp_media_machine_project_ui(&machine);
    CHECK(ui.play_pause_enabled && !ui.seek_enabled && !ui.playing);
    return true;
}

static bool test_buffering_readiness_dispatch(void)
{
    PspMediaMachine machine = psp_media_machine_initial();
    CHECK(open_to_priming(&machine, true, true));
    CHECK(apply(&machine, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_PRIME_READY
    }));
    CHECK(apply(&machine, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_SOURCE_STARVED
    }));
    CHECK(machine.state == PSP_MEDIA_SESSION_BUFFERING);
    CHECK(apply(&machine, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_BUFFER_STABLE,
        .readiness = PSP_MEDIA_PRESENTATION_READY
    }));
    CHECK(machine.state == PSP_MEDIA_SESSION_PLAYING);

    CHECK(apply(&machine, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_SOURCE_STARVED
    }));
    CHECK(apply(&machine, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_BUFFER_STABLE,
        .readiness = PSP_MEDIA_PRESENTATION_NEEDS_PRIME
    }));
    CHECK(machine.state == PSP_MEDIA_SESSION_PRIMING);
    CHECK(machine.priming_phase == PSP_MEDIA_PRIME_FEEDING);
    return true;
}

static bool test_prime_ready_while_source_starved_enters_buffering(void)
{
    PspMediaMachine machine = psp_media_machine_initial();
    CHECK(open_to_priming(&machine, true, true));
    CHECK(apply(&machine, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_SOURCE_STARVED
    }));
    CHECK(machine.state == PSP_MEDIA_SESSION_PRIMING);
    CHECK(machine.priming_phase == PSP_MEDIA_PRIME_WAITING_FOR_SOURCE);
    CHECK(apply(&machine, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_PRIME_READY
    }));
    CHECK(machine.state == PSP_MEDIA_SESSION_BUFFERING);
    CHECK(machine.readiness == PSP_MEDIA_PRESENTATION_NEEDS_SOURCE);
    CHECK(apply(&machine, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_BUFFER_STABLE,
        .readiness = PSP_MEDIA_PRESENTATION_READY
    }));
    CHECK(machine.state == PSP_MEDIA_SESSION_PLAYING);
    return true;
}

static bool test_source_stabilizes_before_prime_ready(void)
{
    PspMediaMachine machine = psp_media_machine_initial();
    CHECK(open_to_priming(&machine, true, true));
    CHECK(apply(&machine, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_SOURCE_STARVED
    }));
    CHECK(machine.priming_phase == PSP_MEDIA_PRIME_WAITING_FOR_SOURCE);
    CHECK(apply(&machine, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_BUFFER_STABLE,
        .readiness = PSP_MEDIA_PRESENTATION_NEEDS_PRIME
    }));
    CHECK(machine.state == PSP_MEDIA_SESSION_PRIMING);
    CHECK(machine.priming_phase == PSP_MEDIA_PRIME_FEEDING);
    CHECK(machine.readiness == PSP_MEDIA_PRESENTATION_NEEDS_PRIME);
    CHECK(apply(&machine, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_PRIME_READY
    }));
    CHECK(machine.state == PSP_MEDIA_SESSION_PLAYING);
    return true;
}

static bool test_seek_supersede_and_recovery(void)
{
    PspMediaMachine machine = psp_media_machine_initial();
    CHECK(open_to_priming(&machine, true, true));
    CHECK(apply(&machine, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_PRIME_READY
    }));
    CHECK(apply(&machine, (PspMediaEvent) {.type = PSP_MEDIA_EVENT_SEEK}));
    uint64_t first_generation = machine.seek_generation;
    CHECK(machine.state == PSP_MEDIA_SESSION_SEEKING);
    CHECK(apply(&machine, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_SOURCE_STARVED
    }));
    CHECK(machine.seeking_phase == PSP_MEDIA_SEEK_WAITING_FOR_SOURCE);
    CHECK(apply(&machine, (PspMediaEvent) {.type = PSP_MEDIA_EVENT_SEEK}));
    CHECK(machine.state == PSP_MEDIA_SESSION_SEEKING);
    CHECK(machine.seeking_phase == PSP_MEDIA_SEEK_PREPARING);
    CHECK(machine.seek_generation == first_generation + 1u);
    CHECK(apply(&machine, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_DECODER_REFUSED
    }));
    CHECK(machine.state == PSP_MEDIA_SESSION_RECOVERING);
    CHECK(apply(&machine, (PspMediaEvent) {.type = PSP_MEDIA_EVENT_SEEK}));
    CHECK(machine.state == PSP_MEDIA_SESSION_RECOVERING);
    CHECK(machine.pending_seek);
    CHECK(apply(&machine, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_RECOVERY_COMPLETE
    }));
    CHECK(machine.state == PSP_MEDIA_SESSION_SEEKING);
    CHECK(!machine.pending_seek);
    CHECK(machine.seek_generation == first_generation + 2u);
    return true;
}

static bool test_failure_always_quiesces(void)
{
    PspMediaMachine machine = psp_media_machine_initial();
    CHECK(apply(&machine, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_OPEN,
        .autoplay = true,
        .has_separate_audio = true
    }));
    CHECK(apply(&machine, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_OPEN_PHASE_COMPLETE
    }));
    CHECK(apply(&machine, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_OPEN_FAILED
    }));
    CHECK(machine.state == PSP_MEDIA_SESSION_QUIESCING);
    CHECK(machine.quiesce_target == PSP_MEDIA_QUIESCE_TARGET_FAILED);
    CHECK(quiesce_successfully(&machine));
    CHECK(machine.state == PSP_MEDIA_SESSION_FAILED);
    CHECK(machine.pipeline == PSP_MEDIA_PIPELINE_NONE);
    CHECK(machine.failure == PSP_MEDIA_FAILURE_OPEN);
    CHECK(apply(&machine, (PspMediaEvent) {.type = PSP_MEDIA_EVENT_RETRY}));
    CHECK(machine.state == PSP_MEDIA_SESSION_OPENING);
    return true;
}

static bool test_playback_failure_quiesces_from_active_or_empty(void)
{
    PspMediaMachine machine = psp_media_machine_initial();
    CHECK(open_to_priming(&machine, true, true));
    CHECK(apply(&machine, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_PRIME_READY
    }));
    CHECK(machine.state == PSP_MEDIA_SESSION_PLAYING);
    CHECK(apply(&machine, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_PLAYBACK_FAILED
    }));
    CHECK(machine.state == PSP_MEDIA_SESSION_QUIESCING);
    CHECK(machine.quiesce_target == PSP_MEDIA_QUIESCE_TARGET_FAILED);
    CHECK(machine.failure == PSP_MEDIA_FAILURE_PLAYBACK);
    CHECK(quiesce_successfully(&machine));
    CHECK(machine.state == PSP_MEDIA_SESSION_FAILED);
    CHECK(machine.pipeline == PSP_MEDIA_PIPELINE_NONE);

    /* A backend can fail after its pipeline has already been torn down. The
       failure still travels through the same explicit quiesce boundary; its
       service phases complete immediately. */
    machine = psp_media_machine_initial();
    CHECK(apply(&machine, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_PLAYBACK_FAILED
    }));
    CHECK(machine.state == PSP_MEDIA_SESSION_QUIESCING);
    CHECK(machine.pipeline == PSP_MEDIA_PIPELINE_NONE);
    CHECK(quiesce_successfully(&machine));
    CHECK(machine.state == PSP_MEDIA_SESSION_FAILED);
    return true;
}

static bool test_backend_quarantine_finishes_failure(void)
{
    PspMediaMachine machine = psp_media_machine_initial();
    CHECK(open_to_priming(&machine, true, true));
    CHECK(apply(&machine, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_DECODER_REFUSED
    }));
    CHECK(apply(&machine, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_RECOVERY_FAILED
    }));
    CHECK(apply(&machine, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_ADMISSION_STOPPED
    }));
    CHECK(apply(&machine, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_TRANSPORT_CANCELLED
    }));
    CHECK(apply(&machine, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_BACKEND_QUARANTINED
    }));
    CHECK(machine.backend_health == PSP_MEDIA_BACKEND_QUARANTINED);
    CHECK(machine.state == PSP_MEDIA_SESSION_FAILED);
    PspMediaDecision retry = psp_media_machine_transition(
        &machine, &(PspMediaEvent) {.type = PSP_MEDIA_EVENT_RETRY});
    CHECK(retry.handled && retry.deliberate_noop);
    CHECK(retry.next.state == PSP_MEDIA_SESSION_FAILED);
    CHECK(!psp_media_machine_project_ui(&machine).retry_available);
    return true;
}

static bool test_open_escape_and_suspend_close(void)
{
    PspMediaMachine machine = psp_media_machine_initial();
    CHECK(apply(&machine, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_OPEN,
        .autoplay = true,
        .has_separate_audio = true
    }));
    CHECK(apply(&machine, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_SUSPEND
    }));
    CHECK(machine.state == PSP_MEDIA_SESSION_QUIESCING);
    CHECK(machine.quiesce_target == PSP_MEDIA_QUIESCE_TARGET_SUSPENDED);
    CHECK(quiesce_successfully(&machine));
    CHECK(machine.state == PSP_MEDIA_SESSION_SUSPENDED);
    CHECK(machine.pipeline == PSP_MEDIA_PIPELINE_NONE);
    CHECK(apply(&machine, (PspMediaEvent) {.type = PSP_MEDIA_EVENT_CLOSE}));
    CHECK(machine.state == PSP_MEDIA_SESSION_IDLE);
    CHECK(!machine.has_plan);
    return true;
}

static bool test_retained_pipeline_is_dormant_not_idle(void)
{
    PspMediaMachine machine = psp_media_machine_initial();
    CHECK(open_to_priming(&machine, true, true));
    CHECK(apply(&machine, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_PRIME_READY
    }));
    CHECK(apply(&machine, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_CLOSE,
        .retain_pipeline = true
    }));
    CHECK(machine.state == PSP_MEDIA_SESSION_DORMANT);
    CHECK(machine.pipeline == PSP_MEDIA_PIPELINE_FULL);
    CHECK(psp_media_machine_project_ui(&machine).mode
          == PSP_MEDIA_UI_HIDDEN);
    CHECK(apply(&machine, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_OPEN,
        .reuse_pipeline = true,
        .has_separate_audio = true
    }));
    CHECK(machine.state == PSP_MEDIA_SESSION_PAUSED);

    CHECK(apply(&machine, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_CLOSE,
        .retain_pipeline = true
    }));
    CHECK(apply(&machine, (PspMediaEvent) {
        .type = PSP_MEDIA_EVENT_RECLAIM
    }));
    CHECK(machine.state == PSP_MEDIA_SESSION_QUIESCING);
    CHECK(machine.quiesce_target == PSP_MEDIA_QUIESCE_TARGET_IDLE);
    CHECK(quiesce_successfully(&machine));
    CHECK(machine.state == PSP_MEDIA_SESSION_IDLE);
    CHECK(machine.pipeline == PSP_MEDIA_PIPELINE_NONE);
    return true;
}

static PspMediaMachine valid_machine_for_state(PspMediaSessionState state)
{
    PspMediaMachine machine = psp_media_machine_initial();
    machine.state = state;
    switch (state) {
    case PSP_MEDIA_SESSION_OPENING:
        machine.has_plan = true;
        machine.opening_phase = PSP_MEDIA_OPEN_RESOLVING;
        break;
    case PSP_MEDIA_SESSION_PRIMING:
        machine.has_plan = true;
        machine.pipeline = PSP_MEDIA_PIPELINE_FULL;
        machine.priming_phase = PSP_MEDIA_PRIME_FEEDING;
        break;
    case PSP_MEDIA_SESSION_PLAYING:
    case PSP_MEDIA_SESSION_PAUSED:
    case PSP_MEDIA_SESSION_BUFFERING:
    case PSP_MEDIA_SESSION_RECOVERING:
    case PSP_MEDIA_SESSION_DORMANT:
        machine.has_plan = true;
        machine.pipeline = PSP_MEDIA_PIPELINE_FULL;
        break;
    case PSP_MEDIA_SESSION_SEEKING:
        machine.has_plan = true;
        machine.pipeline = PSP_MEDIA_PIPELINE_FULL;
        machine.seeking_phase = PSP_MEDIA_SEEK_PREPARING;
        break;
    case PSP_MEDIA_SESSION_QUIESCING:
        machine.has_plan = true;
        machine.pipeline = PSP_MEDIA_PIPELINE_FULL;
        machine.quiesce_phase = PSP_MEDIA_QUIESCE_STOP_ADMISSION;
        machine.quiesce_target = PSP_MEDIA_QUIESCE_TARGET_IDLE;
        break;
    case PSP_MEDIA_SESSION_SUSPENDED:
        machine.suspended_resume_state = PSP_MEDIA_SESSION_IDLE;
        break;
    case PSP_MEDIA_SESSION_FAILED:
        machine.has_plan = true;
        machine.failure = PSP_MEDIA_FAILURE_OPEN;
        break;
    case PSP_MEDIA_SESSION_IDLE:
    default:
        break;
    }
    return machine;
}

static bool test_event_state_grid_is_total(void)
{
    for (int state = PSP_MEDIA_SESSION_IDLE;
         state < PSP_MEDIA_SESSION_STATE_COUNT; state++) {
        PspMediaMachine machine = valid_machine_for_state(
            (PspMediaSessionState) state);
        CHECK(psp_media_machine_violations(&machine) == 0);
        for (int event = PSP_MEDIA_EVENT_OPEN;
             event < PSP_MEDIA_EVENT_COUNT; event++) {
            PspMediaEvent input = {
                .type = (PspMediaEventType) event,
                .readiness = PSP_MEDIA_PRESENTATION_READY
            };
            PspMediaDecision decision =
                psp_media_machine_transition(&machine, &input);
            CHECK(decision.handled);
            CHECK(decision.deliberate_noop
                  || memcmp(&decision.next, &machine,
                            sizeof(machine)) != 0
                  || decision.command != PSP_MEDIA_COMMAND_NONE);
            CHECK(psp_media_machine_violations(&decision.next) == 0);
        }
    }
    return true;
}

static bool test_resource_invariants_reject_impossible_states(void)
{
    PspMediaMachine machine = valid_machine_for_state(
        PSP_MEDIA_SESSION_FAILED);
    machine.pipeline = PSP_MEDIA_PIPELINE_FULL;
    CHECK((psp_media_machine_violations(&machine)
           & PSP_MEDIA_MACHINE_VIOLATION_PIPELINE) != 0);

    machine = valid_machine_for_state(PSP_MEDIA_SESSION_SUSPENDED);
    machine.suspended_resume_state = PSP_MEDIA_SESSION_OPENING;
    machine.has_plan = false;
    CHECK((psp_media_machine_violations(&machine)
           & PSP_MEDIA_MACHINE_VIOLATION_PLAN) != 0);

    machine = valid_machine_for_state(PSP_MEDIA_SESSION_QUIESCING);
    machine.quiesce_target = PSP_MEDIA_QUIESCE_TARGET_FAILED;
    machine.has_plan = false;
    CHECK((psp_media_machine_violations(&machine)
           & PSP_MEDIA_MACHINE_VIOLATION_PLAN) != 0);

    machine = valid_machine_for_state(PSP_MEDIA_SESSION_PLAYING);
    machine.backend_health = PSP_MEDIA_BACKEND_QUARANTINED;
    CHECK((psp_media_machine_violations(&machine)
           & PSP_MEDIA_MACHINE_VIOLATION_BACKEND) != 0);

    machine = valid_machine_for_state(PSP_MEDIA_SESSION_OPENING);
    machine.opening_phase = PSP_MEDIA_OPEN_NONE;
    CHECK((psp_media_machine_violations(&machine)
           & PSP_MEDIA_MACHINE_VIOLATION_CHILD_STATE) != 0);
    return true;
}

static uint64_t state_test_random(uint64_t *state)
{
    uint64_t value = *state;
    value ^= value << 13;
    value ^= value >> 7;
    value ^= value << 17;
    *state = value;
    return value;
}

static bool test_service_tokens_are_monotonic_and_reject_stale_results(void)
{
    PspMediaServiceToken token = {0};
    uint64_t first = psp_media_service_token_begin(
        &token, PSP_MEDIA_COMMAND_START_OPEN_PHASE, 10u, 20u);
    CHECK(first != 0);
    PspMediaEvent completion = {
        .type = PSP_MEDIA_EVENT_OPEN_PHASE_COMPLETE,
        .service_command = PSP_MEDIA_COMMAND_START_OPEN_PHASE,
        .service_epoch = first
    };
    CHECK(psp_media_service_token_matches(&token, &completion));

    uint64_t second = psp_media_service_token_begin(
        &token, PSP_MEDIA_COMMAND_START_SEEK, 30u, 40u);
    CHECK(second > first);
    CHECK(!psp_media_service_token_matches(&token, &completion));
    completion.service_command = PSP_MEDIA_COMMAND_START_SEEK;
    completion.service_epoch = second;
    CHECK(psp_media_service_token_matches(&token, &completion));

    psp_media_service_token_clear(&token);
    CHECK(token.command == PSP_MEDIA_COMMAND_NONE);
    CHECK(token.epoch == second);
    CHECK(!psp_media_service_token_matches(&token, &completion));

    token.epoch = UINT64_MAX;
    CHECK(psp_media_service_token_begin(
        &token, PSP_MEDIA_COMMAND_START_PRIMING, 50u, 60u) == 1u);
    return true;
}

/*
 * Finish a copied random session as the real service pump would.  `fault`
 * selects a healthy drain, a codec timeout, or a DMA timeout.  This proves
 * that cancellation is not only invariant-preserving at the instant it is
 * requested: every partially-open, active, recovering and already-quiescing
 * state reaches the resource-free Idle state in a fixed number of service
 * completions, including both quarantine paths.
 */
static bool close_random_session(PspMediaMachine machine, unsigned fault)
{
    for (unsigned step = 0; step < 12u; step++) {
        CHECK(psp_media_machine_violations(&machine) == 0);
        if (machine.state == PSP_MEDIA_SESSION_IDLE) return true;
        if (machine.state == PSP_MEDIA_SESSION_SUSPENDED
            || machine.state == PSP_MEDIA_SESSION_FAILED) {
            CHECK(apply(&machine, (PspMediaEvent) {
                .type = PSP_MEDIA_EVENT_CLOSE
            }));
            continue;
        }
        if (machine.state == PSP_MEDIA_SESSION_DORMANT) {
            CHECK(apply(&machine, (PspMediaEvent) {
                .type = PSP_MEDIA_EVENT_RECLAIM
            }));
            continue;
        }
        if (machine.state != PSP_MEDIA_SESSION_QUIESCING) {
            CHECK(apply(&machine, (PspMediaEvent) {
                .type = PSP_MEDIA_EVENT_CLOSE,
                .retain_pipeline = false
            }));
            continue;
        }
        PspMediaEventType event = PSP_MEDIA_EVENT_NONE;
        switch (machine.quiesce_phase) {
        case PSP_MEDIA_QUIESCE_STOP_ADMISSION:
            event = PSP_MEDIA_EVENT_ADMISSION_STOPPED;
            break;
        case PSP_MEDIA_QUIESCE_CANCEL_TRANSPORT:
            event = PSP_MEDIA_EVENT_TRANSPORT_CANCELLED;
            break;
        case PSP_MEDIA_QUIESCE_BACKEND:
            event = fault != 0u
                ? PSP_MEDIA_EVENT_BACKEND_QUARANTINED
                : PSP_MEDIA_EVENT_BACKEND_QUIESCED;
            break;
        case PSP_MEDIA_QUIESCE_NONE:
        default:
            CHECK(false);
            return false;
        }
        CHECK(apply(&machine, (PspMediaEvent) {.type = event}));
    }
    CHECK(false);
    return false;
}

static bool test_randomized_lifecycle_sequences_preserve_invariants(void)
{
    for (uint64_t seed = 1; seed <= 64; seed++) {
        uint64_t random = seed * UINT64_C(0x9e3779b97f4a7c15);
        PspMediaMachine machine = psp_media_machine_initial();
        for (unsigned step = 0; step < 4096; step++) {
            uint64_t bits = state_test_random(&random);
            PspMediaEvent event = {
                .type = (PspMediaEventType) (
                    1u + bits % (PSP_MEDIA_EVENT_COUNT - 1u)),
                .autoplay = (bits & (UINT64_C(1) << 8)) != 0,
                .has_separate_audio = (bits & (UINT64_C(1) << 9)) != 0,
                .retain_pipeline = (bits & (UINT64_C(1) << 10)) != 0,
                .reuse_pipeline = (bits & (UINT64_C(1) << 11)) != 0,
                .readiness = (PspMediaPresentationReadiness) (
                    (bits >> 12) % 3u)
            };
            uint64_t generation_before = machine.seek_generation;
            PspMediaDecision decision =
                psp_media_machine_transition(&machine, &event);
            CHECK(decision.handled);
            CHECK(decision.command >= PSP_MEDIA_COMMAND_NONE);
            CHECK(decision.command < PSP_MEDIA_COMMAND_COUNT);
            CHECK(psp_media_machine_violations(&decision.next) == 0);
            CHECK(decision.next.seek_generation >= generation_before);
            CHECK(psp_media_session_state_name(decision.next.state) != NULL);
            CHECK(psp_media_event_name(event.type) != NULL);
            PspMediaUiProjection ui =
                psp_media_machine_project_ui(&decision.next);
            CHECK((unsigned) ui.mode <= PSP_MEDIA_UI_FAILED);
            if (!ui.visible) CHECK(!ui.controls_enabled && !ui.playing);
            machine = decision.next;
            if ((step & 127u) == 127u)
                CHECK(close_random_session(
                    machine, (unsigned) ((step >> 7) % 3u)));
        }
        CHECK(close_random_session(machine, (unsigned) (seed % 3u)));
    }
    return true;
}

int main(void)
{
    if (!test_open_priming_and_play()
        || !test_one_shot_presentation_boundaries()
        || !test_video_only_skips_audio_open_phases()
        || !test_buffering_readiness_dispatch()
        || !test_prime_ready_while_source_starved_enters_buffering()
        || !test_source_stabilizes_before_prime_ready()
        || !test_seek_supersede_and_recovery()
        || !test_failure_always_quiesces()
        || !test_playback_failure_quiesces_from_active_or_empty()
        || !test_backend_quarantine_finishes_failure()
        || !test_open_escape_and_suspend_close()
        || !test_retained_pipeline_is_dormant_not_idle()
        || !test_event_state_grid_is_total()
        || !test_resource_invariants_reject_impossible_states()
        || !test_service_tokens_are_monotonic_and_reject_stale_results()
        || !test_randomized_lifecycle_sequences_preserve_invariants())
        return 1;
    puts("tilefinch-psp-media-state-tests: all checks passed");
    return 0;
}
