#include "tilefinch/psp_media_state.h"

#include <string.h>

uint64_t psp_media_service_token_begin(
    PspMediaServiceToken *token, PspMediaCommand command,
    uint64_t started_us, uint64_t deadline_us)
{
    if (token == NULL || command <= PSP_MEDIA_COMMAND_NONE
        || command >= PSP_MEDIA_COMMAND_COUNT)
        return 0;
    uint64_t epoch = token->epoch + 1u;
    if (epoch == 0) epoch = 1u;
    token->command = command;
    token->epoch = epoch;
    token->started_us = started_us;
    token->deadline_us = deadline_us;
    return epoch;
}

bool psp_media_service_token_matches(
    const PspMediaServiceToken *token, const PspMediaEvent *event)
{
    return token != NULL && event != NULL && event->service_epoch != 0
        && token->command == event->service_command
        && token->epoch == event->service_epoch;
}

void psp_media_service_token_clear(PspMediaServiceToken *token)
{
    if (token == NULL) return;
    /* Preserve the epoch so it stays monotonic across pipeline lifetimes. */
    token->command = PSP_MEDIA_COMMAND_NONE;
    token->started_us = 0;
    token->deadline_us = 0;
}

static bool psp_media_state_is_active(PspMediaSessionState state)
{
    return state >= PSP_MEDIA_SESSION_PRIMING
        && state <= PSP_MEDIA_SESSION_RECOVERING;
}

static void psp_media_clear_child_states(PspMediaMachine *machine)
{
    machine->opening_phase = PSP_MEDIA_OPEN_NONE;
    machine->priming_phase = PSP_MEDIA_PRIME_NONE;
    machine->seeking_phase = PSP_MEDIA_SEEK_NONE;
    machine->quiesce_phase = PSP_MEDIA_QUIESCE_NONE;
    machine->quiesce_target = PSP_MEDIA_QUIESCE_TARGET_NONE;
}

static void psp_media_enter_priming(PspMediaDecision *decision)
{
    psp_media_clear_child_states(&decision->next);
    decision->next.state = PSP_MEDIA_SESSION_PRIMING;
    decision->next.priming_phase = PSP_MEDIA_PRIME_FEEDING;
    decision->next.readiness = PSP_MEDIA_PRESENTATION_NEEDS_PRIME;
    decision->command = PSP_MEDIA_COMMAND_START_PRIMING;
}

static void psp_media_enter_seeking(PspMediaDecision *decision)
{
    psp_media_clear_child_states(&decision->next);
    decision->next.state = PSP_MEDIA_SESSION_SEEKING;
    decision->next.seeking_phase = PSP_MEDIA_SEEK_PREPARING;
    decision->next.pending_seek = false;
    decision->next.ended = false;
    decision->next.seek_generation++;
    decision->command = PSP_MEDIA_COMMAND_START_SEEK;
}

static void psp_media_enter_recovering(PspMediaDecision *decision)
{
    psp_media_clear_child_states(&decision->next);
    decision->next.state = PSP_MEDIA_SESSION_RECOVERING;
    decision->command = PSP_MEDIA_COMMAND_START_RECOVERY;
}

static void psp_media_enter_quiescing(
    PspMediaDecision *decision, PspMediaQuiesceTarget target,
    PspMediaFailureKind failure)
{
    psp_media_clear_child_states(&decision->next);
    decision->next.state = PSP_MEDIA_SESSION_QUIESCING;
    decision->next.quiesce_phase = PSP_MEDIA_QUIESCE_STOP_ADMISSION;
    decision->next.quiesce_target = target;
    decision->next.pause_after_frame = false;
    decision->next.preview_active = false;
    if (target == PSP_MEDIA_QUIESCE_TARGET_SUSPENDED)
        decision->next.suspended_resume_state =
            PSP_MEDIA_SESSION_OPENING;
    if (failure != PSP_MEDIA_FAILURE_NONE)
        decision->next.failure = failure;
    decision->command = PSP_MEDIA_COMMAND_STOP_ADMISSION;
}

static void psp_media_enter_opening(PspMediaDecision *decision)
{
    psp_media_clear_child_states(&decision->next);
    decision->next.state = PSP_MEDIA_SESSION_OPENING;
    decision->next.opening_phase = PSP_MEDIA_OPEN_RESOLVING;
    decision->next.pipeline = PSP_MEDIA_PIPELINE_NONE;
    decision->next.failure = PSP_MEDIA_FAILURE_NONE;
    decision->next.pending_seek = false;
    decision->next.pause_after_frame = false;
    decision->next.preview_active = false;
    decision->next.ended = false;
    decision->command = PSP_MEDIA_COMMAND_START_OPEN_PHASE;
}

PspMediaMachine psp_media_machine_initial(void)
{
    PspMediaMachine machine;
    memset(&machine, 0, sizeof(machine));
    machine.state = PSP_MEDIA_SESSION_IDLE;
    machine.backend_health = PSP_MEDIA_BACKEND_HEALTHY;
    machine.resume_target = PSP_MEDIA_RESUME_PAUSED;
    machine.readiness = PSP_MEDIA_PRESENTATION_NEEDS_SOURCE;
    machine.suspended_resume_state = PSP_MEDIA_SESSION_IDLE;
    return machine;
}

static void psp_media_open_phase_complete(
    PspMediaDecision *decision, const PspMediaEvent *event)
{
    switch (decision->next.opening_phase) {
    case PSP_MEDIA_OPEN_RESOLVING:
        decision->next.has_separate_audio = event->has_separate_audio;
        decision->next.audio_only = event->audio_only;
        /* Modules are prepared before range and demux allocations so their
           contiguous-memory admission is independent of page allocation
           order on a real PSP. */
        decision->next.opening_phase = PSP_MEDIA_OPEN_DECODER_PREPARE;
        break;
    case PSP_MEDIA_OPEN_DECODER_PREPARE:
        decision->next.opening_phase = decision->next.audio_only
            ? PSP_MEDIA_OPEN_AUDIO_RANGE : PSP_MEDIA_OPEN_VIDEO_RANGE;
        break;
    case PSP_MEDIA_OPEN_VIDEO_RANGE:
        decision->next.opening_phase = PSP_MEDIA_OPEN_VIDEO_DEMUX;
        decision->next.pipeline = PSP_MEDIA_PIPELINE_PARTIAL;
        break;
    case PSP_MEDIA_OPEN_VIDEO_DEMUX:
        decision->next.opening_phase = PSP_MEDIA_OPEN_VIDEO_PRIME;
        break;
    case PSP_MEDIA_OPEN_VIDEO_PRIME:
        decision->next.opening_phase = decision->next.has_separate_audio
            ? PSP_MEDIA_OPEN_AUDIO_RANGE
            : PSP_MEDIA_OPEN_PLAYBACK_CREATE;
        break;
    case PSP_MEDIA_OPEN_AUDIO_RANGE:
        decision->next.opening_phase = PSP_MEDIA_OPEN_AUDIO_DEMUX;
        decision->next.pipeline = PSP_MEDIA_PIPELINE_PARTIAL;
        break;
    case PSP_MEDIA_OPEN_AUDIO_DEMUX:
        decision->next.opening_phase = PSP_MEDIA_OPEN_PLAYBACK_CREATE;
        break;
    case PSP_MEDIA_OPEN_PLAYBACK_CREATE:
        decision->next.pipeline = PSP_MEDIA_PIPELINE_FULL;
        psp_media_enter_priming(decision);
        return;
    case PSP_MEDIA_OPEN_NONE:
    default:
        decision->handled = false;
        return;
    }
    decision->command = PSP_MEDIA_COMMAND_START_OPEN_PHASE;
}

static void psp_media_transition_idle(
    PspMediaDecision *decision, const PspMediaEvent *event)
{
    switch (event->type) {
    case PSP_MEDIA_EVENT_OPEN:
        decision->next.has_plan = true;
        decision->next.has_separate_audio = event->has_separate_audio;
        decision->next.audio_only = event->audio_only;
        decision->next.resume_target = event->autoplay
            ? PSP_MEDIA_RESUME_PLAYING : PSP_MEDIA_RESUME_PAUSED;
        if (decision->next.backend_health
            == PSP_MEDIA_BACKEND_QUARANTINED) {
            decision->next.state = PSP_MEDIA_SESSION_FAILED;
            decision->next.failure = PSP_MEDIA_FAILURE_CODEC_TIMEOUT;
        } else {
            psp_media_enter_opening(decision);
        }
        break;
    case PSP_MEDIA_EVENT_SUSPEND:
        decision->next.state = PSP_MEDIA_SESSION_SUSPENDED;
        decision->next.suspended_resume_state = PSP_MEDIA_SESSION_IDLE;
        break;
    case PSP_MEDIA_EVENT_PLAYBACK_FAILED:
        decision->next.has_plan = true;
        psp_media_enter_quiescing(
            decision, PSP_MEDIA_QUIESCE_TARGET_FAILED,
            PSP_MEDIA_FAILURE_PLAYBACK);
        break;
    case PSP_MEDIA_EVENT_CLOSE:
        decision->deliberate_noop = true;
        break;
    default:
        decision->handled = false;
        break;
    }
}

static void psp_media_transition_opening(
    PspMediaDecision *decision, const PspMediaEvent *event)
{
    switch (event->type) {
    case PSP_MEDIA_EVENT_OPEN:
        decision->next.has_plan = true;
        decision->next.has_separate_audio = event->has_separate_audio;
        decision->next.audio_only = event->audio_only;
        decision->next.resume_target = event->autoplay
            ? PSP_MEDIA_RESUME_PLAYING : PSP_MEDIA_RESUME_PAUSED;
        psp_media_enter_opening(decision);
        break;
    case PSP_MEDIA_EVENT_OPEN_PHASE_COMPLETE:
        psp_media_open_phase_complete(decision, event);
        break;
    case PSP_MEDIA_EVENT_OPEN_FAILED:
        psp_media_enter_quiescing(
            decision, PSP_MEDIA_QUIESCE_TARGET_FAILED,
            PSP_MEDIA_FAILURE_OPEN);
        break;
    case PSP_MEDIA_EVENT_PLAYBACK_FAILED:
        psp_media_enter_quiescing(
            decision, PSP_MEDIA_QUIESCE_TARGET_FAILED,
            PSP_MEDIA_FAILURE_PLAYBACK);
        break;
    case PSP_MEDIA_EVENT_PLAY:
        decision->next.resume_target = PSP_MEDIA_RESUME_PLAYING;
        break;
    case PSP_MEDIA_EVENT_PAUSE:
        decision->next.resume_target = PSP_MEDIA_RESUME_PAUSED;
        break;
    case PSP_MEDIA_EVENT_CLOSE:
        psp_media_enter_quiescing(
            decision, PSP_MEDIA_QUIESCE_TARGET_IDLE,
            PSP_MEDIA_FAILURE_NONE);
        break;
    case PSP_MEDIA_EVENT_SUSPEND:
        psp_media_enter_quiescing(
            decision, PSP_MEDIA_QUIESCE_TARGET_SUSPENDED,
            PSP_MEDIA_FAILURE_NONE);
        break;
    default:
        decision->handled = false;
        break;
    }
}

static void psp_media_transition_paused(
    PspMediaDecision *decision, const PspMediaEvent *event)
{
    switch (event->type) {
    case PSP_MEDIA_EVENT_PLAY:
        decision->next.resume_target = PSP_MEDIA_RESUME_PLAYING;
        decision->next.readiness = event->readiness;
        if (event->readiness == PSP_MEDIA_PRESENTATION_READY) {
            decision->next.state = PSP_MEDIA_SESSION_PLAYING;
        } else if (event->readiness
                   == PSP_MEDIA_PRESENTATION_NEEDS_SOURCE) {
            decision->next.state = PSP_MEDIA_SESSION_BUFFERING;
        } else {
            psp_media_enter_priming(decision);
        }
        break;
    case PSP_MEDIA_EVENT_PAUSE:
        decision->deliberate_noop = true;
        break;
    case PSP_MEDIA_EVENT_SOURCE_STARVED:
        decision->next.readiness = PSP_MEDIA_PRESENTATION_NEEDS_SOURCE;
        break;
    case PSP_MEDIA_EVENT_BUFFER_STABLE:
        decision->next.readiness = event->readiness;
        break;
    default:
        decision->handled = false;
        break;
    }
}

static void psp_media_transition_priming(
    PspMediaDecision *decision, const PspMediaEvent *event)
{
    switch (event->type) {
    case PSP_MEDIA_EVENT_PLAY:
        decision->next.resume_target = PSP_MEDIA_RESUME_PLAYING;
        /* A passive OPEN asks for one decoded frame while paused so the
           player never exposes the page behind it. An explicit Play
           supersedes that one-shot boundary. */
        decision->next.pause_after_frame = false;
        break;
    case PSP_MEDIA_EVENT_PAUSE:
        decision->next.resume_target = PSP_MEDIA_RESUME_PAUSED;
        break;
    case PSP_MEDIA_EVENT_SOURCE_STARVED:
        decision->next.priming_phase =
            PSP_MEDIA_PRIME_WAITING_FOR_SOURCE;
        decision->next.readiness = PSP_MEDIA_PRESENTATION_NEEDS_SOURCE;
        break;
    case PSP_MEDIA_EVENT_SOURCE_AVAILABLE:
        decision->next.priming_phase = PSP_MEDIA_PRIME_FEEDING;
        decision->next.readiness = PSP_MEDIA_PRESENTATION_NEEDS_PRIME;
        break;
    case PSP_MEDIA_EVENT_BUFFER_STABLE:
        /* The source can settle before the first presentable frame. That
           clears only the nested source wait; Priming remains responsible
           for the first-frame/audio gate. */
        decision->next.priming_phase = PSP_MEDIA_PRIME_FEEDING;
        decision->next.readiness = PSP_MEDIA_PRESENTATION_NEEDS_PRIME;
        break;
    case PSP_MEDIA_EVENT_PRIME_READY:
        if (decision->next.priming_phase
                == PSP_MEDIA_PRIME_WAITING_FOR_SOURCE
            || decision->next.readiness
                == PSP_MEDIA_PRESENTATION_NEEDS_SOURCE) {
            psp_media_clear_child_states(&decision->next);
            decision->next.state = PSP_MEDIA_SESSION_BUFFERING;
            decision->next.readiness =
                PSP_MEDIA_PRESENTATION_NEEDS_SOURCE;
            break;
        }
        psp_media_clear_child_states(&decision->next);
        decision->next.readiness = PSP_MEDIA_PRESENTATION_READY;
        decision->next.state = decision->next.resume_target
                == PSP_MEDIA_RESUME_PLAYING
            ? PSP_MEDIA_SESSION_PLAYING : PSP_MEDIA_SESSION_PAUSED;
        break;
    default:
        decision->handled = false;
        break;
    }
}

static void psp_media_transition_playing(
    PspMediaDecision *decision, const PspMediaEvent *event)
{
    switch (event->type) {
    case PSP_MEDIA_EVENT_PLAY:
        decision->deliberate_noop = true;
        break;
    case PSP_MEDIA_EVENT_PAUSE:
        decision->next.state = PSP_MEDIA_SESSION_PAUSED;
        decision->next.resume_target = PSP_MEDIA_RESUME_PAUSED;
        break;
    case PSP_MEDIA_EVENT_SOURCE_STARVED:
        decision->next.state = PSP_MEDIA_SESSION_BUFFERING;
        decision->next.readiness = PSP_MEDIA_PRESENTATION_NEEDS_SOURCE;
        break;
    default:
        decision->handled = false;
        break;
    }
}

static void psp_media_transition_buffering(
    PspMediaDecision *decision, const PspMediaEvent *event)
{
    switch (event->type) {
    case PSP_MEDIA_EVENT_PLAY:
        decision->next.resume_target = PSP_MEDIA_RESUME_PLAYING;
        break;
    case PSP_MEDIA_EVENT_PAUSE:
        decision->next.state = PSP_MEDIA_SESSION_PAUSED;
        decision->next.resume_target = PSP_MEDIA_RESUME_PAUSED;
        break;
    case PSP_MEDIA_EVENT_SOURCE_STARVED:
        decision->deliberate_noop = true;
        break;
    case PSP_MEDIA_EVENT_SOURCE_AVAILABLE:
        /* Stability is a timed fact; receiving bytes alone does not leave. */
        decision->deliberate_noop = true;
        break;
    case PSP_MEDIA_EVENT_BUFFER_STABLE:
        decision->next.readiness = event->readiness;
        if (event->readiness == PSP_MEDIA_PRESENTATION_READY) {
            decision->next.state = decision->next.resume_target
                    == PSP_MEDIA_RESUME_PLAYING
                ? PSP_MEDIA_SESSION_PLAYING : PSP_MEDIA_SESSION_PAUSED;
        } else if (event->readiness
                   == PSP_MEDIA_PRESENTATION_NEEDS_PRIME) {
            psp_media_enter_priming(decision);
        } else {
            decision->deliberate_noop = true;
        }
        break;
    default:
        decision->handled = false;
        break;
    }
}

static void psp_media_transition_seeking(
    PspMediaDecision *decision, const PspMediaEvent *event)
{
    switch (event->type) {
    case PSP_MEDIA_EVENT_PLAY:
        decision->next.resume_target = PSP_MEDIA_RESUME_PLAYING;
        break;
    case PSP_MEDIA_EVENT_PAUSE:
        decision->next.resume_target = PSP_MEDIA_RESUME_PAUSED;
        break;
    case PSP_MEDIA_EVENT_SEEK:
        psp_media_enter_seeking(decision);
        break;
    case PSP_MEDIA_EVENT_SOURCE_STARVED:
        decision->next.seeking_phase =
            PSP_MEDIA_SEEK_WAITING_FOR_SOURCE;
        break;
    case PSP_MEDIA_EVENT_SOURCE_AVAILABLE:
        decision->next.seeking_phase = PSP_MEDIA_SEEK_PREPARING;
        break;
    case PSP_MEDIA_EVENT_SEEK_COMPLETE:
        psp_media_enter_priming(decision);
        break;
    case PSP_MEDIA_EVENT_SEEK_FAILED:
        psp_media_enter_quiescing(
            decision, PSP_MEDIA_QUIESCE_TARGET_FAILED,
            PSP_MEDIA_FAILURE_SEEK);
        break;
    default:
        decision->handled = false;
        break;
    }
}

static void psp_media_transition_recovering(
    PspMediaDecision *decision, const PspMediaEvent *event)
{
    switch (event->type) {
    case PSP_MEDIA_EVENT_PLAY:
        decision->next.resume_target = PSP_MEDIA_RESUME_PLAYING;
        break;
    case PSP_MEDIA_EVENT_PAUSE:
        decision->next.resume_target = PSP_MEDIA_RESUME_PAUSED;
        break;
    case PSP_MEDIA_EVENT_SEEK:
        decision->next.pending_seek = true;
        break;
    case PSP_MEDIA_EVENT_DECODER_REFUSED:
        decision->deliberate_noop = true;
        break;
    case PSP_MEDIA_EVENT_RECOVERY_COMPLETE:
        if (decision->next.pending_seek)
            psp_media_enter_seeking(decision);
        else
            psp_media_enter_priming(decision);
        break;
    case PSP_MEDIA_EVENT_RECOVERY_FAILED:
        psp_media_enter_quiescing(
            decision, PSP_MEDIA_QUIESCE_TARGET_FAILED,
            PSP_MEDIA_FAILURE_RECOVERY);
        break;
    default:
        decision->handled = false;
        break;
    }
}

static void psp_media_transition_active(
    PspMediaDecision *decision, const PspMediaEvent *event)
{
    /* Common control events are exhaustive for every active child. */
    if (event->type == PSP_MEDIA_EVENT_PAUSE_AFTER_FRAME) {
        decision->next.pause_after_frame = true;
        return;
    }
    if (event->type == PSP_MEDIA_EVENT_FRAME_DISPLAYED) {
        if (!decision->next.pause_after_frame) {
            decision->deliberate_noop = true;
            return;
        }
        decision->next.pause_after_frame = false;
        decision->next.resume_target = PSP_MEDIA_RESUME_PAUSED;
        psp_media_clear_child_states(&decision->next);
        decision->next.state = PSP_MEDIA_SESSION_PAUSED;
        decision->next.readiness = PSP_MEDIA_PRESENTATION_READY;
        return;
    }
    if (event->type == PSP_MEDIA_EVENT_PREVIEW_STARTED) {
        decision->next.preview_active = true;
        return;
    }
    if (event->type == PSP_MEDIA_EVENT_PREVIEW_ENDED) {
        decision->next.preview_active = false;
        return;
    }
    if (event->type == PSP_MEDIA_EVENT_PLAYBACK_ENDED) {
        decision->next.ended = true;
        decision->next.pause_after_frame = false;
        decision->next.preview_active = false;
        decision->next.resume_target = PSP_MEDIA_RESUME_PAUSED;
        psp_media_clear_child_states(&decision->next);
        decision->next.state = PSP_MEDIA_SESSION_PAUSED;
        decision->next.readiness = PSP_MEDIA_PRESENTATION_READY;
        return;
    }
    if (event->type == PSP_MEDIA_EVENT_CLOSE
        || event->type == PSP_MEDIA_EVENT_SUSPEND) {
        if (event->type == PSP_MEDIA_EVENT_CLOSE
            && event->retain_pipeline) {
            psp_media_clear_child_states(&decision->next);
            decision->next.state = PSP_MEDIA_SESSION_DORMANT;
            decision->next.resume_target = PSP_MEDIA_RESUME_PAUSED;
            return;
        }
        psp_media_enter_quiescing(
            decision,
            event->type == PSP_MEDIA_EVENT_CLOSE
                ? PSP_MEDIA_QUIESCE_TARGET_IDLE
                : PSP_MEDIA_QUIESCE_TARGET_SUSPENDED,
            PSP_MEDIA_FAILURE_NONE);
        return;
    }
    if (event->type == PSP_MEDIA_EVENT_PLAYBACK_FAILED) {
        psp_media_enter_quiescing(
            decision, PSP_MEDIA_QUIESCE_TARGET_FAILED,
            PSP_MEDIA_FAILURE_PLAYBACK);
        return;
    }
    if (event->type == PSP_MEDIA_EVENT_SEEK
        && decision->next.state != PSP_MEDIA_SESSION_SEEKING
        && decision->next.state != PSP_MEDIA_SESSION_RECOVERING) {
        psp_media_enter_seeking(decision);
        return;
    }
    if (event->type == PSP_MEDIA_EVENT_DECODER_REFUSED
        && decision->next.state != PSP_MEDIA_SESSION_RECOVERING) {
        psp_media_enter_recovering(decision);
        return;
    }
    switch (decision->next.state) {
    case PSP_MEDIA_SESSION_PRIMING:
        psp_media_transition_priming(decision, event);
        break;
    case PSP_MEDIA_SESSION_PLAYING:
        psp_media_transition_playing(decision, event);
        break;
    case PSP_MEDIA_SESSION_PAUSED:
        psp_media_transition_paused(decision, event);
        break;
    case PSP_MEDIA_SESSION_BUFFERING:
        psp_media_transition_buffering(decision, event);
        break;
    case PSP_MEDIA_SESSION_SEEKING:
        psp_media_transition_seeking(decision, event);
        break;
    case PSP_MEDIA_SESSION_RECOVERING:
        psp_media_transition_recovering(decision, event);
        break;
    default:
        decision->handled = false;
        break;
    }
}

static void psp_media_finish_quiescing(PspMediaDecision *decision)
{
    PspMediaQuiesceTarget target = decision->next.quiesce_target;
    psp_media_clear_child_states(&decision->next);
    decision->next.pipeline = PSP_MEDIA_PIPELINE_NONE;
    decision->next.pending_seek = false;
    switch (target) {
    case PSP_MEDIA_QUIESCE_TARGET_IDLE:
        decision->next.state = PSP_MEDIA_SESSION_IDLE;
        decision->next.has_plan = false;
        decision->next.has_separate_audio = false;
        decision->next.audio_only = false;
        decision->next.failure = PSP_MEDIA_FAILURE_NONE;
        decision->next.pause_after_frame = false;
        decision->next.preview_active = false;
        decision->next.ended = false;
        break;
    case PSP_MEDIA_QUIESCE_TARGET_OPENING:
        psp_media_enter_opening(decision);
        break;
    case PSP_MEDIA_QUIESCE_TARGET_SUSPENDED:
        decision->next.state = PSP_MEDIA_SESSION_SUSPENDED;
        if (decision->next.backend_health
            == PSP_MEDIA_BACKEND_QUARANTINED)
            decision->next.suspended_resume_state =
                PSP_MEDIA_SESSION_FAILED;
        break;
    case PSP_MEDIA_QUIESCE_TARGET_FAILED:
        decision->next.state = PSP_MEDIA_SESSION_FAILED;
        break;
    case PSP_MEDIA_QUIESCE_TARGET_NONE:
    default:
        decision->handled = false;
        break;
    }
}

static void psp_media_transition_dormant(
    PspMediaDecision *decision, const PspMediaEvent *event)
{
    switch (event->type) {
    case PSP_MEDIA_EVENT_OPEN:
        decision->next.has_separate_audio = event->has_separate_audio;
        decision->next.audio_only = event->audio_only;
        decision->next.resume_target = event->autoplay
            ? PSP_MEDIA_RESUME_PLAYING : PSP_MEDIA_RESUME_PAUSED;
        if (event->reuse_pipeline) {
            decision->next.state = PSP_MEDIA_SESSION_PAUSED;
            decision->next.readiness = PSP_MEDIA_PRESENTATION_READY;
        } else {
            psp_media_enter_quiescing(
                decision, PSP_MEDIA_QUIESCE_TARGET_OPENING,
                PSP_MEDIA_FAILURE_NONE);
        }
        break;
    case PSP_MEDIA_EVENT_RECLAIM:
        psp_media_enter_quiescing(
            decision, PSP_MEDIA_QUIESCE_TARGET_IDLE,
            PSP_MEDIA_FAILURE_NONE);
        break;
    case PSP_MEDIA_EVENT_CLOSE:
        decision->deliberate_noop = true;
        break;
    case PSP_MEDIA_EVENT_SUSPEND:
        psp_media_enter_quiescing(
            decision, PSP_MEDIA_QUIESCE_TARGET_SUSPENDED,
            PSP_MEDIA_FAILURE_NONE);
        break;
    case PSP_MEDIA_EVENT_PLAYBACK_FAILED:
        psp_media_enter_quiescing(
            decision, PSP_MEDIA_QUIESCE_TARGET_FAILED,
            PSP_MEDIA_FAILURE_PLAYBACK);
        break;
    default:
        decision->handled = false;
        break;
    }
}

static void psp_media_transition_quiescing(
    PspMediaDecision *decision, const PspMediaEvent *event)
{
    switch (event->type) {
    case PSP_MEDIA_EVENT_ADMISSION_STOPPED:
        if (decision->next.quiesce_phase
            != PSP_MEDIA_QUIESCE_STOP_ADMISSION) {
            decision->handled = false;
            break;
        }
        decision->next.quiesce_phase = PSP_MEDIA_QUIESCE_CANCEL_TRANSPORT;
        decision->command = PSP_MEDIA_COMMAND_CANCEL_TRANSPORT;
        break;
    case PSP_MEDIA_EVENT_TRANSPORT_CANCELLED:
        if (decision->next.quiesce_phase
            != PSP_MEDIA_QUIESCE_CANCEL_TRANSPORT) {
            decision->handled = false;
            break;
        }
        decision->next.quiesce_phase = PSP_MEDIA_QUIESCE_BACKEND;
        decision->command = PSP_MEDIA_COMMAND_QUIESCE_BACKEND;
        break;
    case PSP_MEDIA_EVENT_BACKEND_QUIESCED:
        if (decision->next.quiesce_phase != PSP_MEDIA_QUIESCE_BACKEND) {
            decision->handled = false;
            break;
        }
        psp_media_finish_quiescing(decision);
        break;
    case PSP_MEDIA_EVENT_BACKEND_QUARANTINED:
        if (decision->next.quiesce_phase != PSP_MEDIA_QUIESCE_BACKEND) {
            decision->handled = false;
            break;
        }
        decision->next.backend_health = PSP_MEDIA_BACKEND_QUARANTINED;
        if (decision->next.failure == PSP_MEDIA_FAILURE_NONE)
            decision->next.failure = PSP_MEDIA_FAILURE_CODEC_TIMEOUT;
        psp_media_finish_quiescing(decision);
        break;
    case PSP_MEDIA_EVENT_SUSPEND:
        decision->next.quiesce_target =
            PSP_MEDIA_QUIESCE_TARGET_SUSPENDED;
        break;
    case PSP_MEDIA_EVENT_PLAYBACK_FAILED:
        decision->next.quiesce_target = PSP_MEDIA_QUIESCE_TARGET_FAILED;
        decision->next.failure = PSP_MEDIA_FAILURE_PLAYBACK;
        break;
    case PSP_MEDIA_EVENT_CLOSE:
        if (decision->next.quiesce_target
            == PSP_MEDIA_QUIESCE_TARGET_SUSPENDED) {
            decision->deliberate_noop = true;
        } else {
            decision->next.quiesce_target = PSP_MEDIA_QUIESCE_TARGET_IDLE;
        }
        break;
    default:
        decision->handled = false;
        break;
    }
}

static void psp_media_transition_suspended(
    PspMediaDecision *decision, const PspMediaEvent *event)
{
    switch (event->type) {
    case PSP_MEDIA_EVENT_RESUME:
        if (decision->next.suspended_resume_state
            == PSP_MEDIA_SESSION_FAILED) {
            decision->next.state = PSP_MEDIA_SESSION_FAILED;
        } else if (decision->next.has_plan) {
            psp_media_enter_opening(decision);
        } else {
            decision->next.state = PSP_MEDIA_SESSION_IDLE;
        }
        break;
    case PSP_MEDIA_EVENT_CLOSE:
        decision->next.state = PSP_MEDIA_SESSION_IDLE;
        decision->next.has_plan = false;
        decision->next.has_separate_audio = false;
        decision->next.audio_only = false;
        break;
    case PSP_MEDIA_EVENT_SUSPEND:
        decision->deliberate_noop = true;
        break;
    default:
        decision->handled = false;
        break;
    }
}

static void psp_media_transition_failed(
    PspMediaDecision *decision, const PspMediaEvent *event)
{
    switch (event->type) {
    case PSP_MEDIA_EVENT_RETRY:
        if (decision->next.backend_health
            == PSP_MEDIA_BACKEND_HEALTHY) {
            psp_media_enter_opening(decision);
        } else {
            decision->deliberate_noop = true;
        }
        break;
    case PSP_MEDIA_EVENT_CLOSE:
        decision->next.state = PSP_MEDIA_SESSION_IDLE;
        decision->next.has_plan = false;
        decision->next.has_separate_audio = false;
        decision->next.audio_only = false;
        decision->next.failure = PSP_MEDIA_FAILURE_NONE;
        decision->next.pause_after_frame = false;
        decision->next.preview_active = false;
        decision->next.ended = false;
        break;
    case PSP_MEDIA_EVENT_SUSPEND:
        decision->next.state = PSP_MEDIA_SESSION_SUSPENDED;
        decision->next.suspended_resume_state = PSP_MEDIA_SESSION_FAILED;
        break;
    default:
        decision->handled = false;
        break;
    }
}

PspMediaDecision psp_media_machine_transition(
    const PspMediaMachine *machine, const PspMediaEvent *event)
{
    PspMediaDecision decision;
    memset(&decision, 0, sizeof(decision));
    if (machine == NULL || event == NULL
        || event->type <= PSP_MEDIA_EVENT_NONE
        || event->type >= PSP_MEDIA_EVENT_COUNT) return decision;
    decision.next = *machine;
    decision.handled = true;
    if (psp_media_state_is_active(machine->state)) {
        psp_media_transition_active(&decision, event);
    } else {
        switch (machine->state) {
        case PSP_MEDIA_SESSION_IDLE:
            psp_media_transition_idle(&decision, event);
            break;
        case PSP_MEDIA_SESSION_OPENING:
            psp_media_transition_opening(&decision, event);
            break;
        case PSP_MEDIA_SESSION_DORMANT:
            psp_media_transition_dormant(&decision, event);
            break;
        case PSP_MEDIA_SESSION_QUIESCING:
            psp_media_transition_quiescing(&decision, event);
            break;
        case PSP_MEDIA_SESSION_SUSPENDED:
            psp_media_transition_suspended(&decision, event);
            break;
        case PSP_MEDIA_SESSION_FAILED:
            psp_media_transition_failed(&decision, event);
            break;
        default:
            decision.handled = false;
            break;
        }
    }
    /* The event/state grid is total. Inapplicable events are explicit no-ops,
       not blank cells whose behavior a later caller has to guess. */
    if (!decision.handled) {
        decision.next = *machine;
        decision.handled = true;
        decision.deliberate_noop = true;
    }
    if (decision.command == PSP_MEDIA_COMMAND_NONE
        && memcmp(&decision.next, machine, sizeof(*machine)) == 0)
        decision.deliberate_noop = true;
    return decision;
}

uint32_t psp_media_machine_violations(const PspMediaMachine *machine)
{
    if (machine == NULL) return UINT32_MAX;
    uint32_t result = 0;
    if (machine->state < PSP_MEDIA_SESSION_IDLE
        || machine->state >= PSP_MEDIA_SESSION_STATE_COUNT)
        result |= PSP_MEDIA_MACHINE_VIOLATION_STATE;

    bool pipeline_ok = true;
    switch (machine->state) {
    case PSP_MEDIA_SESSION_IDLE:
    case PSP_MEDIA_SESSION_SUSPENDED:
    case PSP_MEDIA_SESSION_FAILED:
        pipeline_ok = machine->pipeline == PSP_MEDIA_PIPELINE_NONE;
        break;
    case PSP_MEDIA_SESSION_OPENING:
    case PSP_MEDIA_SESSION_QUIESCING:
        pipeline_ok = machine->pipeline == PSP_MEDIA_PIPELINE_NONE
            || machine->pipeline == PSP_MEDIA_PIPELINE_PARTIAL
            || machine->pipeline == PSP_MEDIA_PIPELINE_FULL;
        break;
    case PSP_MEDIA_SESSION_PRIMING:
    case PSP_MEDIA_SESSION_PLAYING:
    case PSP_MEDIA_SESSION_PAUSED:
    case PSP_MEDIA_SESSION_BUFFERING:
    case PSP_MEDIA_SESSION_SEEKING:
    case PSP_MEDIA_SESSION_RECOVERING:
    case PSP_MEDIA_SESSION_DORMANT:
        pipeline_ok = machine->pipeline == PSP_MEDIA_PIPELINE_FULL;
        break;
    default:
        pipeline_ok = false;
        break;
    }
    if (!pipeline_ok) result |= PSP_MEDIA_MACHINE_VIOLATION_PIPELINE;

    bool children_ok = true;
    if (machine->state == PSP_MEDIA_SESSION_OPENING)
        children_ok = machine->opening_phase != PSP_MEDIA_OPEN_NONE;
    else if (machine->opening_phase != PSP_MEDIA_OPEN_NONE)
        children_ok = false;
    if (machine->state == PSP_MEDIA_SESSION_PRIMING)
        children_ok = children_ok
            && machine->priming_phase != PSP_MEDIA_PRIME_NONE;
    else if (machine->priming_phase != PSP_MEDIA_PRIME_NONE)
        children_ok = false;
    if (machine->state == PSP_MEDIA_SESSION_SEEKING)
        children_ok = children_ok
            && machine->seeking_phase != PSP_MEDIA_SEEK_NONE;
    else if (machine->seeking_phase != PSP_MEDIA_SEEK_NONE)
        children_ok = false;
    if (machine->state == PSP_MEDIA_SESSION_QUIESCING)
        children_ok = children_ok
            && machine->quiesce_phase != PSP_MEDIA_QUIESCE_NONE;
    else if (machine->quiesce_phase != PSP_MEDIA_QUIESCE_NONE)
        children_ok = false;
    if (!children_ok) result |= PSP_MEDIA_MACHINE_VIOLATION_CHILD_STATE;

    if ((machine->state == PSP_MEDIA_SESSION_QUIESCING)
        != (machine->quiesce_target != PSP_MEDIA_QUIESCE_TARGET_NONE))
        result |= PSP_MEDIA_MACHINE_VIOLATION_TARGET;
    if ((machine->state == PSP_MEDIA_SESSION_OPENING
         || psp_media_state_is_active(machine->state)
         || machine->state == PSP_MEDIA_SESSION_DORMANT
         || machine->state == PSP_MEDIA_SESSION_FAILED)
        && !machine->has_plan)
        result |= PSP_MEDIA_MACHINE_VIOLATION_PLAN;
    if (machine->state == PSP_MEDIA_SESSION_QUIESCING
        && machine->quiesce_target != PSP_MEDIA_QUIESCE_TARGET_IDLE
        && !machine->has_plan)
        result |= PSP_MEDIA_MACHINE_VIOLATION_PLAN;
    if (machine->state == PSP_MEDIA_SESSION_SUSPENDED
        && machine->suspended_resume_state != PSP_MEDIA_SESSION_IDLE
        && machine->suspended_resume_state != PSP_MEDIA_SESSION_OPENING
        && machine->suspended_resume_state != PSP_MEDIA_SESSION_FAILED)
        result |= PSP_MEDIA_MACHINE_VIOLATION_CHILD_STATE;
    if (machine->state == PSP_MEDIA_SESSION_SUSPENDED
        && machine->suspended_resume_state != PSP_MEDIA_SESSION_IDLE
        && !machine->has_plan)
        result |= PSP_MEDIA_MACHINE_VIOLATION_PLAN;
    if (machine->backend_health == PSP_MEDIA_BACKEND_QUARANTINED
        && (psp_media_state_is_active(machine->state)
            || machine->state == PSP_MEDIA_SESSION_OPENING
            || machine->state == PSP_MEDIA_SESSION_DORMANT))
        result |= PSP_MEDIA_MACHINE_VIOLATION_BACKEND;
    return result;
}

PspMediaUiProjection psp_media_machine_project_ui(
    const PspMediaMachine *machine)
{
    PspMediaUiProjection ui;
    memset(&ui, 0, sizeof(ui));
    if (machine == NULL) return ui;
    switch (machine->state) {
    case PSP_MEDIA_SESSION_IDLE:
        ui.mode = PSP_MEDIA_UI_HIDDEN;
        break;
    case PSP_MEDIA_SESSION_OPENING:
        ui.mode = PSP_MEDIA_UI_OPENING;
        ui.visible = true;
        ui.show_progress = true;
        break;
    case PSP_MEDIA_SESSION_PRIMING:
        ui.mode = PSP_MEDIA_UI_PRIMING;
        ui.visible = true;
        ui.controls_enabled = true;
        ui.play_pause_enabled = true;
        ui.show_progress = true;
        ui.playing = machine->resume_target == PSP_MEDIA_RESUME_PLAYING;
        break;
    case PSP_MEDIA_SESSION_PLAYING:
        ui.mode = PSP_MEDIA_UI_PLAYING;
        ui.visible = true;
        ui.controls_enabled = true;
        ui.play_pause_enabled = true;
        ui.seek_enabled = true;
        ui.playing = true;
        break;
    case PSP_MEDIA_SESSION_PAUSED:
        ui.mode = PSP_MEDIA_UI_PAUSED;
        ui.visible = true;
        ui.controls_enabled = true;
        ui.play_pause_enabled = true;
        ui.seek_enabled = true;
        break;
    case PSP_MEDIA_SESSION_BUFFERING:
        ui.mode = PSP_MEDIA_UI_BUFFERING;
        ui.visible = true;
        ui.controls_enabled = true;
        ui.play_pause_enabled = true;
        ui.seek_enabled = true;
        ui.show_progress = true;
        ui.playing = machine->resume_target == PSP_MEDIA_RESUME_PLAYING;
        break;
    case PSP_MEDIA_SESSION_SEEKING:
        ui.mode = PSP_MEDIA_UI_SEEKING;
        ui.visible = true;
        ui.controls_enabled = true;
        ui.play_pause_enabled = true;
        /* A later scrub supersedes the pending target. The physical session
           coalesces repeated requests so this never overlaps decoder work. */
        ui.seek_enabled = true;
        ui.show_progress = true;
        ui.playing = machine->resume_target == PSP_MEDIA_RESUME_PLAYING;
        break;
    case PSP_MEDIA_SESSION_RECOVERING:
        ui.mode = PSP_MEDIA_UI_RECOVERING;
        ui.visible = true;
        ui.show_progress = true;
        break;
    case PSP_MEDIA_SESSION_DORMANT:
        ui.mode = PSP_MEDIA_UI_HIDDEN;
        break;
    case PSP_MEDIA_SESSION_QUIESCING:
        ui.mode = PSP_MEDIA_UI_STOPPING;
        ui.visible = machine->quiesce_target
            != PSP_MEDIA_QUIESCE_TARGET_IDLE;
        ui.show_progress = true;
        break;
    case PSP_MEDIA_SESSION_SUSPENDED:
        ui.mode = PSP_MEDIA_UI_SUSPENDED;
        break;
    case PSP_MEDIA_SESSION_FAILED:
        ui.mode = PSP_MEDIA_UI_FAILED;
        ui.visible = true;
        ui.controls_enabled = true;
        ui.retry_available = machine->backend_health
            == PSP_MEDIA_BACKEND_HEALTHY;
        break;
    default:
        break;
    }
    ui.preview_active = machine->preview_active;
    /* Preview is tentative even when the lifecycle has returned to Playing:
       the saved resume target is playing, but the visible clock and DAC stay
       held until the user commits or cancels the highlighted position. */
    if (ui.preview_active) ui.playing = false;
    ui.ended = machine->ended;
    return ui;
}

const char *psp_media_session_state_name(PspMediaSessionState state)
{
    static const char *const names[] = {
        "idle", "opening", "priming", "playing", "paused",
        "buffering", "seeking", "recovering", "dormant", "quiescing",
        "suspended", "failed"
    };
    _Static_assert(sizeof(names) / sizeof(names[0])
                   == PSP_MEDIA_SESSION_STATE_COUNT,
                   "media state names must cover the enum");
    return state >= PSP_MEDIA_SESSION_IDLE
            && state < PSP_MEDIA_SESSION_STATE_COUNT
        ? names[state] : "invalid";
}

const char *psp_media_event_name(PspMediaEventType event)
{
    static const char *const names[] = {
        "none", "open", "open-phase-complete", "open-failed",
        "playback-failed", "play", "pause", "seek", "source-starved",
        "source-available",
        "buffer-stable", "prime-ready", "decoder-refused",
        "seek-complete", "seek-failed", "recovery-complete",
        "recovery-failed", "close", "reclaim", "suspend", "resume",
        "retry", "admission-stopped",
        "transport-cancelled", "backend-quiesced", "backend-quarantined",
        "pause-after-frame", "frame-displayed", "preview-started",
        "preview-ended", "playback-ended"
    };
    _Static_assert(sizeof(names) / sizeof(names[0])
                   == PSP_MEDIA_EVENT_COUNT,
                   "media event names must cover the enum");
    return event >= PSP_MEDIA_EVENT_NONE && event < PSP_MEDIA_EVENT_COUNT
        ? names[event] : "invalid";
}
