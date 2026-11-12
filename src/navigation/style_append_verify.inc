/* Host-only validation, compiled out of the PSP and trace-free builds
   (included by relayout.inc under !TILEFINCH_NO_TRACE && !__PSP__).

   TILEFINCH_TRACE_STYLE_APPEND_VERIFY=1: after each relayout that changed
   the sheet in place (navigation_try_append_style_sources: inserted,
   retired or reordered sources), force a whole-sheet rebuild of the same
   DOM and compare. Prints one line per check:

     style-append-verify ok=1 full=1 styles=same(N/N) layout=same ...

   `styles` folds every element's computed style resolved straight from
   the sheet (no retained matches), with interned one-based IDs replaced by
   what they name; `layout` folds the draw commands, which also covers the
   retained matched-rule lists the in-place path kept. A layout difference
   with equal styles is image-load timing: the rebuild decodes images
   synchronously that the incremental path still has pending. Differences
   print the first elements (`style-append-verify-diff`, with the differing
   ComputedStyle byte offsets) and commands (`style-append-verify-cmd`).
   `dom=changed` says the forced rebuild itself changed the document (it
   dispatched stylesheet load/error events whose handlers ran, or content
   moved): the two sides then describe different DOMs and the check proves
   nothing either way. tools/style_append_verify.py reads these lines for
   the ctest gate and `census.py replay --verify-style-appends`.
   The check changes the run it observes (each one costs a rebuild), so it
   is for proving equality, never for timing. */
#include "style_values_internal.h"

static uint64_t navigation_verify_fold(uint64_t hash, const void *data,
                                       size_t length)
{
    const unsigned char *bytes = data;
    for (size_t i = 0; i < length; i++)
        hash = (hash ^ bytes[i]) * UINT64_C(1099511628211);
    return hash;
}

static uint64_t navigation_verify_string(uint64_t hash, const char *text,
                                         size_t length)
{
    if (text == NULL) return navigation_verify_fold(hash, "\1", 1);
    return navigation_verify_fold(hash, text, length);
}

static uint64_t navigation_verify_layout_digest(const LayoutDocument *layout)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; layout != NULL && i < layout->count; i++) {
        const DrawCommand *c = &layout->commands[i];
        int64_t fields[] = {
            c->type, c->x, c->y, c->width, c->height, (int64_t) c->color,
            c->radius, c->z_index, c->scale, c->font_size, c->opacity_scale,
            c->font_family, c->font_weight, c->font_italic,
            c->letter_spacing, c->image_fit,
            c->type == DRAW_TEXT ? (int64_t) c->text_length : 0
        };
        hash = navigation_verify_fold(hash, fields, sizeof(fields));
    }
    return hash;
}

/* Folds a math-program length (STYLE_LENGTH_PROGRAM_TAG + index into the
   sheet's interned programs) by its instructions, and clears it in `bare`. */
static uint64_t navigation_verify_math_lengths(uint64_t hash,
                                               const Stylesheet *sheet,
                                               ComputedStyle *bare)
{
    unsigned char *bytes = (unsigned char *) bare;
    for (size_t at = 0; at + sizeof(int32_t) <= sizeof(*bare);
         at += sizeof(int32_t)) {
        int32_t value;
        memcpy(&value, bytes + at, sizeof(value));
        if (value < STYLE_LENGTH_PROGRAM_TAG
            || value > STYLE_LENGTH_PROGRAM_END) continue;
        size_t index = (size_t) (value - STYLE_LENGTH_PROGRAM_TAG);
        if (index >= sheet->math_program_count) continue;
        const StyleMathProgram *program = &sheet->math_programs[index];
        hash = navigation_verify_fold(hash, &at, sizeof(at));
        hash = navigation_verify_fold(
            hash, sheet->math_instructions + program->first,
            program->count * sizeof(*sheet->math_instructions));
        value = 0;
        memcpy(bytes + at, &value, sizeof(value));
    }
    return hash;
}

/* Every element's computed style in DOM order, resolved straight from the
   sheet (no retained matches), with each borrowed string folded by value. */
enum { NAVIGATION_VERIFY_CAPTURE = 8192 };
static uint64_t navigation_verify_style_digest(NavigationSession *session,
                                               size_t *elements,
                                               ComputedStyle *capture,
                                               lxb_dom_node_t **capture_nodes)
{
    enum { DEPTH = 1024 };
    uint64_t hash = UINT64_C(1469598103934665603);
    *elements = 0;
    lxb_dom_node_t *root = lxb_dom_interface_node(session->page.document.html);
    ComputedStyle *stack = calloc(DEPTH, sizeof(*stack));
    lxb_dom_node_t **nodes = calloc(DEPTH, sizeof(*nodes));
    if (stack == NULL || nodes == NULL || root == NULL) {
        free(stack);
        free(nodes);
        return 0;
    }
    size_t depth = 0;
    lxb_dom_node_t *at = root->first_child;
    const Stylesheet *sheet = &session->page.stylesheet;
    while (at != NULL) {
        bool descend = at->type != LXB_DOM_NODE_TYPE_ELEMENT;
        if (at->type == LXB_DOM_NODE_TYPE_ELEMENT && depth < DEPTH) {
            ComputedStyle style = style_for_node(
                sheet, at, depth == 0 ? NULL : &stack[depth - 1]);
            ComputedStyle bare;
            memset(&bare, 0, sizeof(bare));
            bare = style;
            const char *background = bare.background_image;
            const char *mask = bare.mask_image;
            const char *generated = bare.generated_text;
            bare.background_image = NULL;
            bare.mask_image = NULL;
            bare.generated_text = NULL;
            if (bare.generated_attr_length != UINT8_MAX /* scalar sentinel */) {
                const char *attr = bare.generated_attr;
                bare.generated_attr = NULL;
                hash = navigation_verify_string(
                    hash, attr, attr == NULL ? 0 : bare.generated_attr_length);
            }
            /* Interned one-based IDs number in parse or first-resolution
               order, which an in-place insertion legitimately changes:
               fold what they name instead. */
            uint8_t colors = bare.border_color_set;
            bare.border_color_set = 0;
            if (colors != 0 && colors <= sheet->border_color_set_count)
                hash = navigation_verify_fold(
                    hash, &sheet->border_color_sets[colors - 1],
                    sizeof(sheet->border_color_sets[0]));
            if (computed_style_paint_stack_id(&style) != 0) {
                const StyleGradient *gradient =
                    stylesheet_background_gradient(sheet, &style);
                if (gradient != NULL)
                    hash = navigation_verify_fold(hash, gradient,
                                                  sizeof(*gradient));
                size_t shadows = stylesheet_box_shadow_count(sheet, &style);
                hash = navigation_verify_fold(hash, &shadows,
                                              sizeof(shadows));
                for (size_t k = 0; k < shadows; k++) {
                    const StyleBoxShadow *shadow =
                        stylesheet_box_shadow(sheet, &style, k);
                    if (shadow != NULL)
                        hash = navigation_verify_fold(hash, shadow,
                                                      sizeof(*shadow));
                }
                int radius = stylesheet_border_radius_code(sheet, &style);
                hash = navigation_verify_fold(hash, &radius, sizeof(radius));
                computed_style_set_paint_stack_id(&bare, 0);
            }
            if (computed_style_grid_column_template_id(&style) != 0
                || computed_style_grid_row_template_id(&style) != 0
                || computed_style_grid_template_area_id(&style) != 0) {
                char text[512];
                for (int rows = 0; rows < 2; rows++) {
                    text[0] = 0;
                    stylesheet_serialize_grid_template_tracks(
                        sheet, &style, rows != 0, text, sizeof(text));
                    hash = navigation_verify_string(
                        hash, text, strnlen(text, sizeof(text)));
                }
                text[0] = 0;
                stylesheet_serialize_grid_template_areas(sheet, &style, text,
                                                         sizeof(text));
                hash = navigation_verify_string(
                    hash, text, strnlen(text, sizeof(text)));
                computed_style_set_grid_column_template_id(&bare, 0);
                computed_style_set_grid_row_template_id(&bare, 0);
                computed_style_set_grid_template_area_id(&bare, 0);
            }
            hash = navigation_verify_math_lengths(hash, sheet, &bare);
            hash = navigation_verify_fold(hash, &bare, sizeof(bare));
            if (capture != NULL && *elements < NAVIGATION_VERIFY_CAPTURE) {
                capture[*elements] = bare;
                capture_nodes[*elements] = at;
            }
            hash = navigation_verify_string(
                hash, background, background == NULL ? 0
                                  : strnlen(background, 4096));
            hash = navigation_verify_string(
                hash, mask, mask == NULL ? 0 : strnlen(mask, 4096));
            hash = navigation_verify_string(
                hash, generated, generated == NULL ? 0
                                 : bare.generated_text_length);
            ++*elements;
            stack[depth] = style;
            nodes[depth] = at;
            depth++;
            descend = true;
        }
        if (descend && at->first_child != NULL) {
            at = at->first_child;
            continue;
        }
        for (;;) {
            if (depth > 0 && nodes[depth - 1] == at) depth--;
            if (at->next != NULL) {
                at = at->next;
                break;
            }
            at = at->parent;
            if (at == root || at == NULL) {
                at = NULL;
                break;
            }
        }
    }
    free(stack);
    free(nodes);
    return hash;
}

static void navigation_verify_style_append(NavigationSession *session)
{
    static bool verifying = false;
    if (verifying || getenv("TILEFINCH_TRACE_STYLE_APPEND_VERIFY") == NULL)
        return;
    verifying = true;
    size_t elements = 0, rebuilt_elements = 0;
    ComputedStyle *first = calloc(NAVIGATION_VERIFY_CAPTURE, sizeof(*first));
    ComputedStyle *second = calloc(NAVIGATION_VERIFY_CAPTURE, sizeof(*second));
    lxb_dom_node_t **first_nodes =
        calloc(NAVIGATION_VERIFY_CAPTURE, sizeof(*first_nodes));
    lxb_dom_node_t **second_nodes =
        calloc(NAVIGATION_VERIFY_CAPTURE, sizeof(*second_nodes));
    uint64_t styles = navigation_verify_style_digest(session, &elements,
                                                     first, first_nodes);
    uint64_t layout = navigation_verify_layout_digest(&session->page.layout);
    size_t rules = session->page.stylesheet.count;
    bool keyframes = session->page.stylesheet.has_motion_keyframes;
    size_t full = session->performance.full_relayouts;
    size_t command_count = session->page.layout.count;
    DrawCommand *commands = calloc(command_count + 1u, sizeof(*commands));
    if (commands != NULL && command_count != 0)
        memcpy(commands, session->page.layout.commands,
               command_count * sizeof(*commands));
    uint64_t content_generation = session->page.document.content_generation;
    size_t event_dispatches = session->performance.stylesheet_event_dispatches;
    session->style_rebuild_required = true;
    bool ok = navigation_relayout(session);
    bool dom_changed =
        session->page.document.content_generation != content_generation
        || session->performance.stylesheet_event_dispatches
               != event_dispatches;
    for (size_t i = 0, shown = 0; commands != NULL && shown < 4
         && (i < command_count || i < session->page.layout.count); i++) {
        const DrawCommand *a = i < command_count ? &commands[i] : NULL;
        const DrawCommand *b = i < session->page.layout.count
            ? &session->page.layout.commands[i] : NULL;
        if (a != NULL && b != NULL && a->type == b->type && a->x == b->x
            && a->y == b->y && a->width == b->width && a->height == b->height
            && a->color == b->color) continue;
        fprintf(stderr, "style-append-verify-cmd #%zu/%zu/%zu a=%d %d,%d %dx%d %08x %.*s | b=%d %d,%d %dx%d %08x %.*s\n",
                i, command_count, session->page.layout.count,
                a ? a->type : -1, a ? a->x : 0, a ? a->y : 0, a ? a->width : 0, a ? a->height : 0, a ? a->color : 0,
                a && a->type == DRAW_TEXT && a->image == NULL ? (int) (a->text_length > 30 ? 30 : a->text_length) : 0, a && a->text ? a->text : "",
                b ? b->type : -1, b ? b->x : 0, b ? b->y : 0, b ? b->width : 0, b ? b->height : 0, b ? b->color : 0,
                b && b->type == DRAW_TEXT && b->image == NULL ? (int) (b->text_length > 30 ? 30 : b->text_length) : 0, b && b->text ? b->text : "");
        shown++;
    }
    free(commands);
    uint64_t rebuilt_styles =
        navigation_verify_style_digest(session, &rebuilt_elements, second,
                                       second_nodes);
    size_t reported = 0;
    for (size_t i = 0; first != NULL && second != NULL && reported < 6
         && i < elements && i < rebuilt_elements
         && i < NAVIGATION_VERIFY_CAPTURE; i++) {
        if (memcmp(&first[i], &second[i], sizeof(first[i])) == 0) continue;
        const unsigned char *a = (const unsigned char *) &first[i];
        const unsigned char *b = (const unsigned char *) &second[i];
        char offsets[160];
        size_t used = 0;
        offsets[0] = 0;
        for (size_t o = 0; o < sizeof(first[i]) && used < 140; o++) {
            if (a[o] != b[o])
                used += (size_t) snprintf(offsets + used,
                                          sizeof(offsets) - used,
                                          "%zu:%02x>%02x ", o, a[o], b[o]);
        }
        size_t name_length = 0;
        const lxb_char_t *name = lxb_dom_element_local_name(
            lxb_dom_interface_element(first_nodes[i]), &name_length);
        size_t id_length = 0, class_length = 0;
        const lxb_char_t *id = lxb_dom_element_get_attribute(
            lxb_dom_interface_element(first_nodes[i]),
            (const lxb_char_t *) "id", 2, &id_length);
        const lxb_char_t *klass = lxb_dom_element_get_attribute(
            lxb_dom_interface_element(first_nodes[i]),
            (const lxb_char_t *) "class", 5, &class_length);
        fprintf(stderr,
                "style-append-verify-diff #%zu %.*s id=%.*s class=%.*s "
                "same-node=%d bytes %s\n", i, (int) name_length,
                (const char *) name, (int) (id ? id_length : 0),
                id ? (const char *) id : "",
                (int) (klass ? (class_length > 40 ? 40 : class_length) : 0),
                klass ? (const char *) klass : "",
                first_nodes[i] == second_nodes[i], offsets);
        reported++;
    }
    free(first);
    free(second);
    free(first_nodes);
    free(second_nodes);
    uint64_t rebuilt_layout =
        navigation_verify_layout_digest(&session->page.layout);
    fprintf(stderr,
            "style-append-verify ok=%d full=%d styles=%s(%zu/%zu) "
            "layout=%s rules=%zu/%zu keyframes=%d/%d dom=%s\n",
            ok ? 1 : 0, session->performance.full_relayouts == full + 1,
            styles == rebuilt_styles && elements == rebuilt_elements
                ? "same" : "DIFF",
            elements, rebuilt_elements,
            layout == rebuilt_layout ? "same" : "DIFF", rules,
            session->page.stylesheet.count, keyframes ? 1 : 0,
            session->page.stylesheet.has_motion_keyframes ? 1 : 0,
            dom_changed ? "changed" : "same");
    verifying = false;
}
