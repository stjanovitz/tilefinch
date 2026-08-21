#ifndef TILEFINCH_CONTROLLER_H
#define TILEFINCH_CONTROLLER_H

#include <stdbool.h>
#include <stddef.h>

#include "tilefinch/navigation.h"
#include "tilefinch/media_discovery.h"
#include "tilefinch/request_context.h"

typedef enum {
    CONTROLLER_FOCUS_NONE,
    CONTROLLER_FOCUS_LINK,
    CONTROLLER_FOCUS_CONTROL,
    /* A pointer/touch hit on an ordinary rendered element. It is not added
       to sequential keyboard focus, but script-delegated click handlers must
       still be reachable on coarse-pointer devices. */
    CONTROLLER_FOCUS_POINTER
} ControllerFocusKind;

typedef enum {
    CONTROLLER_FOCUS_UP,
    CONTROLLER_FOCUS_DOWN,
    CONTROLLER_FOCUS_LEFT,
    CONTROLLER_FOCUS_RIGHT
} ControllerFocusDirection;

typedef enum {
    CONTROLLER_POINTER_MOVE = 1,
    CONTROLLER_POINTER_DOWN,
    CONTROLLER_POINTER_UP,
    CONTROLLER_POINTER_CANCEL
} ControllerPointerPhase;

typedef enum {
    CONTROLLER_ACTION_NONE,
    CONTROLLER_ACTION_NAVIGATE,
    CONTROLLER_ACTION_CONTROL,
    CONTROLLER_ACTION_FORM_SUBMIT,
    CONTROLLER_ACTION_MEDIA
} ControllerActionType;

typedef struct {
    ControllerActionType type;
    /* An engine-authored adapter may mark a normal navigation link as a
       request for the platform's native provider player. The URL remains the
       canonical web URL; frontends which do not implement the native route
       can execute the action as an ordinary navigation. */
    bool prefer_native_media;
    /* True for authored page audio or a high-confidence structured audio
       preview. The platform route uses the same player lifecycle, controls,
       clock, and seek machinery as video, but opens only the AAC/MP4 track. */
    bool media_audio_only;
    char url[NAVIGATION_URL_LIMIT];
    char method[8];
    char content_type[64];
    char body[4096];
    size_t body_length;
    int64_t media_node_handle;
    TilefinchRequestMode media_mode;
    TilefinchCredentialsMode media_credentials;
    MediaDiscoveryKind media_kind;
} ControllerAction;

typedef struct {
    uint32_t color;
    int offset;
    uint8_t alpha;
    uint8_t width;
    uint8_t style;
} ControllerFocusOutline;

typedef struct {
    NavigationSession *navigation;
    ControllerFocusKind focus_kind;
    size_t focus_index;
    size_t focus_moves;
    size_t activations;
    size_t text_edits;
    /* Indices are layout-generation local. Retain the DOM identity so a
       relayout between focus and activation cannot silently retarget input. */
    lxb_dom_node_t *focus_node;
    long focus_handle;
    char focus_link_url[NAVIGATION_URL_LIMIT];
    size_t focus_link_url_length;
    lxb_dom_node_t *pointer_node;
    lxb_dom_node_t *pointer_hover_node;
    lxb_dom_node_t *pointer_down_node;
    long pointer_hover_handle;
    long pointer_down_handle;
    int pointer_x;
    int pointer_y;
    int pointer_width;
    int pointer_height;
    int pointer_click_x;
    int pointer_click_y;
    int pointer_click_offset_x;
    int pointer_click_offset_y;
    bool pointer_down_active;
    bool pointer_click_pending;
    bool pointer_resize_active;
    uint8_t pointer_resize_mode;
    int pointer_resize_start_x;
    int pointer_resize_start_y;
    int pointer_resize_start_width;
    int pointer_resize_start_height;
    bool has_authored_focus_outline;
    ControllerFocusOutline authored_focus_outline;
    uint64_t authored_focus_stylesheet_generation;
    size_t authored_focus_relayout_generation;
    int viewport_height;
    int focus_margin;
    /* Activation scratch, rebuilt immediately before every structured-media
       fallback. Candidate spans point into live DOM text and are never reused
       across activations because author script may destroy that storage. */
    MediaStructuredAudioIndex structured_audio;
    size_t structured_audio_matched_activations;
    size_t structured_audio_ambiguous_rejections;
} BrowserController;

typedef struct {
    bool editable;
    bool voice_allowed;
    bool keyboard_url_mode;
    bool multiline;
} ControllerTextInputInfo;

bool controller_init(BrowserController *controller,
                     NavigationSession *navigation);
bool controller_configure_viewport(BrowserController *controller,
                                   int viewport_height, int focus_margin);
bool controller_focus_next(BrowserController *controller);
bool controller_focus_previous(BrowserController *controller);
/* Moves through the retained, rendered focus map. Directional geometry is
   preferred; sequential focus order is the deterministic edge fallback. */
bool controller_focus_direction(BrowserController *controller,
                                ControllerFocusDirection direction);
bool controller_focus_at(BrowserController *controller, int x, int y);
bool controller_pointer_event(BrowserController *controller,
                              ControllerPointerPhase phase,
                              int x, int y, bool *activate);
/* Dispatch the click synthesized by a completed pointer down/up pair without
   applying keyboard/controller default actions such as form submission. */
bool controller_commit_pointer_click(BrowserController *controller);
void controller_pointer_discard_click(BrowserController *controller);
bool controller_focus_node(BrowserController *controller,
                           lxb_dom_node_t *node);
bool controller_focused_rect(const BrowserController *controller,
                             int *x, int *y, int *width, int *height);
/*
 * Returns the live authored focus outline only when focus can be represented
 * as a compositor overlay without changing any retained layout/paint field.
 */
bool controller_focused_outline_style(
    BrowserController *controller, ControllerFocusOutline *outline);
bool controller_activate(BrowserController *controller,
                         ControllerAction *action);
/* Build the HTML form default action. Set dispatch_submit_event only for a
   user activation; script requestSubmit() has already dispatched it. */
bool controller_build_form_action(
    BrowserController *controller, lxb_dom_node_t *form,
    lxb_dom_node_t *submitter, bool dispatch_submit_event,
    ControllerAction *action);
bool controller_insert_text(BrowserController *controller,
                            const char *utf8, size_t length);
bool controller_replace_text(BrowserController *controller,
                             const char *utf8, size_t length);
bool controller_text_value(const BrowserController *controller,
                           char *output, size_t capacity, size_t *length);
bool controller_text_input_info(
    const BrowserController *controller, ControllerTextInputInfo *info);
bool controller_backspace(BrowserController *controller);
bool controller_reveal_focus(BrowserController *controller);
bool controller_scroll_by(BrowserController *controller, int delta_y,
                          int viewport_height);
bool controller_scroll_settle(BrowserController *controller);
LayoutCursor controller_pointer_cursor(const BrowserController *controller);
LayoutScrollbarWidth controller_root_scrollbar_width(
    const BrowserController *controller);
bool controller_scroll_step(BrowserController *controller, int direction,
                            unsigned held_frames, int viewport_height);
bool controller_scroll_page(BrowserController *controller, int direction,
                            int viewport_height);
bool controller_scroll_to_top(BrowserController *controller,
                              int viewport_height);
bool controller_scroll_to_bottom(BrowserController *controller,
                                 int viewport_height);
bool controller_execute_action(BrowserController *controller,
                               const ControllerAction *action,
                               size_t maximum_bytes, long timeout_ms);

#endif
