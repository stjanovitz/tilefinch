#ifndef TILEFINCH_PSP_MEDIA_STATE_H
#define TILEFINCH_PSP_MEDIA_STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Pure control model for the PSP media session.
 *
 * This module owns decisions, not resources.  The PSP frontend supplies one
 * sampled event, applies the returned non-blocking commands, and feeds service
 * completions back as later events.  Decoder, DMA, transport and teardown work
 * must never run inside psp_media_machine_transition().
 */

typedef enum {
    PSP_MEDIA_SESSION_IDLE = 0,
    PSP_MEDIA_SESSION_OPENING,
    PSP_MEDIA_SESSION_PRIMING,
    PSP_MEDIA_SESSION_PLAYING,
    PSP_MEDIA_SESSION_PAUSED,
    PSP_MEDIA_SESSION_BUFFERING,
    PSP_MEDIA_SESSION_SEEKING,
    PSP_MEDIA_SESSION_RECOVERING,
    PSP_MEDIA_SESSION_DORMANT,
    PSP_MEDIA_SESSION_QUIESCING,
    PSP_MEDIA_SESSION_SUSPENDED,
    PSP_MEDIA_SESSION_FAILED,
    PSP_MEDIA_SESSION_STATE_COUNT
} PspMediaSessionState;

typedef enum {
    PSP_MEDIA_OPEN_NONE = 0,
    PSP_MEDIA_OPEN_RESOLVING,
    PSP_MEDIA_OPEN_VIDEO_RANGE,
    PSP_MEDIA_OPEN_VIDEO_DEMUX,
    PSP_MEDIA_OPEN_VIDEO_PRIME,
    PSP_MEDIA_OPEN_AUDIO_RANGE,
    PSP_MEDIA_OPEN_AUDIO_DEMUX,
    PSP_MEDIA_OPEN_DECODER_PREPARE,
    PSP_MEDIA_OPEN_PLAYBACK_CREATE
} PspMediaOpeningPhase;

typedef enum {
    PSP_MEDIA_PRIME_NONE = 0,
    PSP_MEDIA_PRIME_FEEDING,
    PSP_MEDIA_PRIME_WAITING_FOR_SOURCE
} PspMediaPrimingPhase;

typedef enum {
    PSP_MEDIA_SEEK_NONE = 0,
    PSP_MEDIA_SEEK_PREPARING,
    PSP_MEDIA_SEEK_WAITING_FOR_SOURCE
} PspMediaSeekingPhase;

typedef enum {
    PSP_MEDIA_QUIESCE_NONE = 0,
    PSP_MEDIA_QUIESCE_STOP_ADMISSION,
    PSP_MEDIA_QUIESCE_CANCEL_TRANSPORT,
    PSP_MEDIA_QUIESCE_BACKEND
} PspMediaQuiescePhase;

typedef enum {
    PSP_MEDIA_QUIESCE_TARGET_NONE = 0,
    PSP_MEDIA_QUIESCE_TARGET_IDLE,
    PSP_MEDIA_QUIESCE_TARGET_OPENING,
    PSP_MEDIA_QUIESCE_TARGET_SUSPENDED,
    PSP_MEDIA_QUIESCE_TARGET_FAILED
} PspMediaQuiesceTarget;

typedef enum {
    PSP_MEDIA_PIPELINE_NONE = 0,
    PSP_MEDIA_PIPELINE_PARTIAL,
    PSP_MEDIA_PIPELINE_FULL
} PspMediaPipelineState;

typedef enum {
    PSP_MEDIA_BACKEND_HEALTHY = 0,
    PSP_MEDIA_BACKEND_QUARANTINED
} PspMediaBackendHealth;

typedef enum {
    PSP_MEDIA_RESUME_PAUSED = 0,
    PSP_MEDIA_RESUME_PLAYING
} PspMediaResumeTarget;

typedef enum {
    PSP_MEDIA_PRESENTATION_READY = 0,
    PSP_MEDIA_PRESENTATION_NEEDS_PRIME,
    PSP_MEDIA_PRESENTATION_NEEDS_SOURCE
} PspMediaPresentationReadiness;

typedef enum {
    PSP_MEDIA_FAILURE_NONE = 0,
    PSP_MEDIA_FAILURE_OPEN,
    PSP_MEDIA_FAILURE_PLAYBACK,
    PSP_MEDIA_FAILURE_SEEK,
    PSP_MEDIA_FAILURE_RECOVERY,
    PSP_MEDIA_FAILURE_CODEC_TIMEOUT,
    PSP_MEDIA_FAILURE_DMA_TIMEOUT
} PspMediaFailureKind;

typedef enum {
    PSP_MEDIA_EVENT_NONE = 0,
    PSP_MEDIA_EVENT_OPEN,
    PSP_MEDIA_EVENT_OPEN_PHASE_COMPLETE,
    PSP_MEDIA_EVENT_OPEN_FAILED,
    PSP_MEDIA_EVENT_PLAYBACK_FAILED,
    PSP_MEDIA_EVENT_PLAY,
    PSP_MEDIA_EVENT_PAUSE,
    PSP_MEDIA_EVENT_SEEK,
    PSP_MEDIA_EVENT_SOURCE_STARVED,
    PSP_MEDIA_EVENT_SOURCE_AVAILABLE,
    PSP_MEDIA_EVENT_BUFFER_STABLE,
    PSP_MEDIA_EVENT_PRIME_READY,
    PSP_MEDIA_EVENT_DECODER_REFUSED,
    PSP_MEDIA_EVENT_SEEK_COMPLETE,
    PSP_MEDIA_EVENT_SEEK_FAILED,
    PSP_MEDIA_EVENT_RECOVERY_COMPLETE,
    PSP_MEDIA_EVENT_RECOVERY_FAILED,
    PSP_MEDIA_EVENT_CLOSE,
    PSP_MEDIA_EVENT_RECLAIM,
    PSP_MEDIA_EVENT_SUSPEND,
    PSP_MEDIA_EVENT_RESUME,
    PSP_MEDIA_EVENT_RETRY,
    PSP_MEDIA_EVENT_ADMISSION_STOPPED,
    PSP_MEDIA_EVENT_TRANSPORT_CANCELLED,
    PSP_MEDIA_EVENT_BACKEND_QUIESCED,
    PSP_MEDIA_EVENT_BACKEND_QUARANTINED,
    PSP_MEDIA_EVENT_PAUSE_AFTER_FRAME,
    PSP_MEDIA_EVENT_FRAME_DISPLAYED,
    PSP_MEDIA_EVENT_PREVIEW_STARTED,
    PSP_MEDIA_EVENT_PREVIEW_ENDED,
    PSP_MEDIA_EVENT_PLAYBACK_ENDED,
    PSP_MEDIA_EVENT_COUNT
} PspMediaEventType;

typedef enum {
    PSP_MEDIA_COMMAND_NONE = 0,
    PSP_MEDIA_COMMAND_START_OPEN_PHASE,
    PSP_MEDIA_COMMAND_START_PRIMING,
    PSP_MEDIA_COMMAND_START_SEEK,
    PSP_MEDIA_COMMAND_START_RECOVERY,
    PSP_MEDIA_COMMAND_STOP_ADMISSION,
    PSP_MEDIA_COMMAND_CANCEL_TRANSPORT,
    PSP_MEDIA_COMMAND_QUIESCE_BACKEND,
    PSP_MEDIA_COMMAND_COUNT
} PspMediaCommand;

typedef struct {
    PspMediaEventType type;
    bool autoplay;
    bool has_separate_audio;
    bool retain_pipeline;
    bool reuse_pipeline;
    PspMediaPresentationReadiness readiness;
    PspMediaCommand service_command;
    uint64_t service_epoch;
} PspMediaEvent;

typedef struct {
    PspMediaSessionState state;
    PspMediaOpeningPhase opening_phase;
    PspMediaPrimingPhase priming_phase;
    PspMediaSeekingPhase seeking_phase;
    PspMediaQuiescePhase quiesce_phase;
    PspMediaQuiesceTarget quiesce_target;
    PspMediaPipelineState pipeline;
    PspMediaBackendHealth backend_health;
    PspMediaResumeTarget resume_target;
    PspMediaPresentationReadiness readiness;
    PspMediaFailureKind failure;
    PspMediaSessionState suspended_resume_state;
    uint64_t seek_generation;
    bool has_plan;
    bool has_separate_audio;
    bool pending_seek;
    bool pause_after_frame;
    bool preview_active;
    bool ended;
} PspMediaMachine;

typedef struct {
    PspMediaCommand command;
    uint64_t epoch;
    uint64_t started_us;
    uint64_t deadline_us;
} PspMediaServiceToken;

typedef struct {
    PspMediaMachine next;
    PspMediaCommand command;
    bool handled;
    bool deliberate_noop;
} PspMediaDecision;

typedef enum {
    PSP_MEDIA_UI_HIDDEN = 0,
    PSP_MEDIA_UI_OPENING,
    PSP_MEDIA_UI_PRIMING,
    PSP_MEDIA_UI_PLAYING,
    PSP_MEDIA_UI_PAUSED,
    PSP_MEDIA_UI_BUFFERING,
    PSP_MEDIA_UI_SEEKING,
    PSP_MEDIA_UI_RECOVERING,
    PSP_MEDIA_UI_STOPPING,
    PSP_MEDIA_UI_SUSPENDED,
    PSP_MEDIA_UI_FAILED
} PspMediaUiMode;

typedef struct {
    PspMediaUiMode mode;
    bool visible;
    bool controls_enabled;
    bool play_pause_enabled;
    bool seek_enabled;
    bool show_progress;
    bool playing;
    bool retry_available;
    bool preview_active;
    bool ended;
} PspMediaUiProjection;

enum {
    PSP_MEDIA_MACHINE_VIOLATION_NONE = 0,
    PSP_MEDIA_MACHINE_VIOLATION_STATE = 1u << 0,
    PSP_MEDIA_MACHINE_VIOLATION_PIPELINE = 1u << 1,
    PSP_MEDIA_MACHINE_VIOLATION_CHILD_STATE = 1u << 2,
    PSP_MEDIA_MACHINE_VIOLATION_TARGET = 1u << 3,
    PSP_MEDIA_MACHINE_VIOLATION_PLAN = 1u << 4,
    PSP_MEDIA_MACHINE_VIOLATION_BACKEND = 1u << 5
};

PspMediaMachine psp_media_machine_initial(void);
PspMediaDecision psp_media_machine_transition(
    const PspMediaMachine *machine, const PspMediaEvent *event);
uint32_t psp_media_machine_violations(const PspMediaMachine *machine);
PspMediaUiProjection psp_media_machine_project_ui(
    const PspMediaMachine *machine);
uint64_t psp_media_service_token_begin(
    PspMediaServiceToken *token, PspMediaCommand command,
    uint64_t started_us, uint64_t deadline_us);
bool psp_media_service_token_matches(
    const PspMediaServiceToken *token, const PspMediaEvent *event);
void psp_media_service_token_clear(PspMediaServiceToken *token);
const char *psp_media_session_state_name(PspMediaSessionState state);
const char *psp_media_event_name(PspMediaEventType event);

#endif
