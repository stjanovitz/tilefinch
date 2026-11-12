/* Display retargeting of decoded rasters; see tilefinch/image_retarget.h. */
#include "tilefinch/image_retarget.h"

#include <stdint.h>
#include <string.h>

#include "tilefinch/budget.h"
#include "tilefinch/platform.h"
#include "image_decode_internal.h"
#include "image_svg_decode_internal.h"

typedef struct {
    int width;
    int height;
    bool painted;
    bool fixed;
    /* A paint lies within the reader's near (rasterize) or far (keep)
       distance; both true without a reader. */
    bool near;
    bool kept;
} RetargetNeed;

struct ImageRetargetPlan {
    const DrawCommand *commands;
    const ImageResource *items;
    size_t command_count, image_count, generation, cursor;
    /* The reader window the needs were judged against. With a reader, a
       paint inside a scroll container is judged where the container shows
       it: when one was (scrolled), the plan holds only while the layout's
       scroll_generation is what it was then. */
    bool has_reader;
    bool scrolled;
    ImageRetargetReader reader;
    uint32_t scroll_generation;
    RetargetNeed needs[];
};

void images_retarget_plan_discard(ImageResources *images)
{
    budget_free(images->budget, images->retarget_plan);
    images->retarget_plan = NULL;
}

void images_retarget_plan_rebase(ImageResources *images, size_t generation)
{
    if (images->retarget_plan != NULL)
        images->retarget_plan->generation = generation;
}

static int retarget_ceil_div(int64_t value, int64_t divisor)
{
    if (divisor <= 0) return 0;
    int64_t result = (value + divisor - 1) / divisor;
    return result > INT32_MAX ? INT32_MAX : (int) result;
}

static int retarget_scaled(int value, int numerator, int denominator)
{
    return retarget_ceil_div((int64_t) value * numerator, denominator);
}

/* The aspect-preserving size of a surface of pw x ph that resolves a box of
   box_w x box_h: the larger of the two axis ratios (cover, stretch, sprite)
   or the smaller (contain). */
static void retarget_fit(int pw, int ph, int box_w, int box_h,
                         bool larger_ratio, int *width, int *height)
{
    bool width_rules = (int64_t) box_w * ph >= (int64_t) box_h * pw;
    if (!larger_ratio) width_rules = !width_rules;
    if (width_rules) {
        *width = box_w;
        *height = retarget_ceil_div((int64_t) box_w * ph, pw);
    } else {
        *height = box_h;
        *width = retarget_ceil_div((int64_t) box_h * pw, ph);
    }
}

static const void *retarget_identity(const ImageResource *item)
{
    if (item->pixels != NULL) return item->pixels;
    if (item->encoded_body != NULL) return item->encoded_body;
    return item->encoded;
}

/* Whether paint `index` of `layout` belongs to a viewport-fixed or sticky
   layer, whose document position says nothing of where it is shown. */
static bool retarget_command_floats(const LayoutDocument *layout,
                                    size_t index)
{
    for (size_t r = 0; r < layout->fixed_count; r++) {
        if (index >= layout->fixed_ranges[r].command_start
            && index < layout->fixed_ranges[r].command_end) return true;
    }
    for (size_t r = 0; r < layout->sticky_count; r++) {
        if (index >= layout->sticky_ranges[r].command_start
            && index < layout->sticky_ranges[r].command_end) return true;
    }
    return false;
}

/* Whether [top, top + height) comes within `distance` of the reader's
   viewport. 64-bit, so no coordinate or distance can wrap. */
static bool retarget_within(const ImageRetargetReader *reader, int top,
                            int height, int distance)
{
    int64_t first = (int64_t) reader->top - distance;
    int64_t last = (int64_t) reader->bottom + distance;
    int64_t bottom = (int64_t) top + (height > 0 ? height : 0);
    return bottom > first && (int64_t) top < last;
}

static bool retarget_distant_eligible(const ImageResource *owner);

/* Whether `command` comes within `distance` of the reader's viewport where
   it is shown: moved by the scroll containers around it and limited to
   what their clips (widened by `distance`) can show, when `placement` (of
   its element) is not NULL. */
static bool retarget_shown_within(const ImageRetargetReader *reader,
                                  const DrawCommand *command,
                                  const LayoutScrollPlacement *placement,
                                  int distance)
{
    if (placement == NULL)
        return retarget_within(reader, command->y, command->height,
                               distance);
    int64_t top = (int64_t) command->y + placement->dy;
    int64_t bottom = top + (command->height > 0 ? command->height : 0);
    if (placement->clipped_y) {
        if (top < (int64_t) placement->clip_top - distance)
            top = (int64_t) placement->clip_top - distance;
        if (bottom > (int64_t) placement->clip_bottom + distance)
            bottom = (int64_t) placement->clip_bottom + distance;
        if (top >= bottom) return false;
    }
    if (placement->clipped_x) {
        int64_t left = (int64_t) command->x + placement->dx;
        int64_t right = left + (command->width > 0 ? command->width : 0);
        if (right <= (int64_t) placement->clip_left - distance
            || left >= (int64_t) placement->clip_right + distance)
            return false;
    }
    return bottom > (int64_t) reader->top - distance
        && top < (int64_t) reader->bottom + distance;
}

/* Fills needs[] from every paint of the table's surfaces. *scrolled (when
   not NULL) tells whether a paint was judged through scroll containers. */
static bool retarget_scan(const ImageResources *images,
                          const LayoutDocument *layout, RetargetNeed *needs,
                          const ImageRetargetReader *reader, bool *scrolled)
{
    if (scrolled != NULL) *scrolled = false;
    /* The element last placed through its scroll containers (most of an
       element's paints are consecutive). */
    const ImageResource *placed_item = NULL;
    LayoutScrollPlacement placement;
    bool placed = false;
    int numerator = layout->viewport.scale_numerator > 0
        ? layout->viewport.scale_numerator : 1;
    int denominator = layout->viewport.scale_denominator > 0
        ? layout->viewport.scale_denominator : 1;
    const ImageResource *first = images->items;
    const ImageResource *end = images->items + images->count;
    for (size_t c = 0; c < layout->count; c++) {
        const DrawCommand *command = &layout->commands[c];
        if (command->type != DRAW_IMAGE || command->image == NULL) continue;
        if (command->image < first || command->image >= end) return false;
        size_t index = (size_t) (command->image - first);
        const ImageResource *item = &images->items[index];
        RetargetNeed *need = &needs[index];
        if (command->width <= 0 || command->height <= 0) continue;
        need->painted = true;
        if (reader == NULL || retarget_command_floats(layout, c)) {
            need->near = need->kept = true;
        } else {
            /* Only a paint inside an overflow clip (flagged by layout) of a
               surface the distance rule applies to is placed: the walk up
               its element's ancestors is skipped for every other paint. */
            if (layout->command_flags != NULL
                && (layout->command_flags[c] & LAYOUT_COMMAND_OVERFLOW) != 0
                && retarget_distant_eligible(item)) {
                if (item != placed_item) {
                    placed_item = item;
                    placed = layout_node_overflow_placement(
                        layout, item->node, &placement);
                }
                if (placed && scrolled != NULL) *scrolled = true;
            } else {
                placed_item = NULL;
                placed = false;
            }
            const LayoutScrollPlacement *shown = placed ? &placement : NULL;
            need->near |= retarget_shown_within(reader, command, shown,
                                                reader->near);
            need->kept |= retarget_shown_within(reader, command, shown,
                                                reader->far);
        }
        /* Judge against the surface as first decoded, so repeated
           reductions do not compound rounding of its aspect. */
        int pw = item->full_width > 0 ? item->full_width : item->width;
        int ph = item->full_height > 0 ? item->full_height : item->height;
        if (pw <= 0 || ph <= 0 || draw_command_rotation_quadrants(command)) {
            need->fixed = true;
            continue;
        }
        int box_w = retarget_scaled(command->width, numerator, denominator);
        int box_h = retarget_scaled(command->height, numerator, denominator);
        int width = 0, height = 0;
        switch (command->image_fit) {
        case LAYOUT_IMAGE_FIT_STRETCH:
        case LAYOUT_IMAGE_FIT_COVER:
            retarget_fit(pw, ph, box_w, box_h, true, &width, &height);
            break;
        case LAYOUT_IMAGE_FIT_CONTAIN:
            retarget_fit(pw, ph, box_w, box_h, false, &width, &height);
            break;
        case LAYOUT_IMAGE_FIT_SPRITE:
        case LAYOUT_IMAGE_FIT_SPRITE_TILE_X:
        case LAYOUT_IMAGE_FIT_SPRITE_TILE_Y:
        case LAYOUT_IMAGE_FIT_SPRITE_TILE_XY: {
            /* One tile is painted at the authored background-size (natural
               size when omitted) in CSS pixels. */
            int tile_w = draw_command_image_sprite_width(command);
            int tile_h = draw_command_image_sprite_height(command);
            if (tile_w <= 0) tile_w = item->source_width > 0
                ? item->source_width : pw;
            if (tile_h <= 0) tile_h = item->source_height > 0
                ? item->source_height : ph;
            retarget_fit(pw, ph,
                         retarget_scaled(tile_w, numerator, denominator),
                         retarget_scaled(tile_h, numerator, denominator),
                         true, &width, &height);
            break;
        }
        default:
            /* object-fit none/scale-down paint the decoded size itself. */
            need->fixed = true;
            continue;
        }
        if (width > need->width) need->width = width;
        if (height > need->height) need->height = height;
    }
    return true;
}

static void retarget_forget_session(BrowserSession *session,
                                    const ImageResource *owner)
{
    if (session != NULL && owner->pixel_body != NULL) {
        browser_session_decoded_image_forget(session, owner->pixel_body);
    }
}

/* Replaces the surface shared by every item with identity old_identity. */
static void retarget_replace(ImageResources *images, const void *old_identity,
                             ImageResource *owner, unsigned char *pixels,
                             int width, int height, uint8_t set_flags,
                             BrowserSession *session)
{
    size_t old_bytes = owner->pixels == NULL ? 0
        : (size_t) owner->width * (size_t) owner->height * 4u;
    int full_width = owner->full_width > 0 ? owner->full_width : owner->width;
    int full_height = owner->full_height > 0
        ? owner->full_height : owner->height;
    retarget_forget_session(session, owner);
    BrowserSharedBody *body = NULL;
    if (pixels != NULL) {
        body = browser_shared_body_take(
            images->budget, pixels, (size_t) width * (size_t) height * 4u);
    }
    for (size_t i = 0; i < images->count; i++) {
        ImageResource *item = &images->items[i];
        if (retarget_identity(item) != old_identity) continue;
        BrowserSharedBody *old_body = item->pixel_body;
        unsigned char *old_pixels = item->pixels;
        bool old_owned = item->owns_pixels;
        item->pixels = pixels;
        item->pixel_body = body;
        item->width = width;
        item->height = height;
        item->full_width = full_width;
        item->full_height = full_height;
        item->retarget_flags |= set_flags;
        item->owns_pixels = item == owner && pixels != NULL;
        /* A cache acquisition owns its own body lease. Multiple owning
           entries may therefore share one pixel identity; plain aliases
           own none. Release every former lease before collapsing the new
           publication to one owner. */
        if (old_owned) {
            if (old_body != NULL) browser_shared_body_release(old_body);
            else budget_free(images->budget, old_pixels);
        }
    }
    size_t new_bytes = pixels == NULL ? 0
        : (size_t) width * (size_t) height * 4u;
    if (old_bytes > new_bytes) {
        images->retarget.released_bytes += old_bytes - new_bytes;
    } else {
        images->retarget.restored_bytes += new_bytes - old_bytes;
    }
    if (images->stats.decoded_bytes >= old_bytes) {
        images->stats.decoded_bytes -= old_bytes;
    }
    images->stats.decoded_bytes += new_bytes;
}

/* Point-samples the current surface down to width x height. */
static unsigned char *retarget_sample(const ImageResources *images,
                                      const ImageResource *owner,
                                      int width, int height)
{
    unsigned char *target = budget_malloc_category(
        images->budget, BUDGET_CATEGORY_RESOURCE,
        (size_t) width * (size_t) height * 4u);
    if (target == NULL) return NULL;
    image_point_sample_rgba(owner->pixels, owner->width, owner->height,
                            target, width, height);
    return target;
}

/* Decodes the owner's encoded bytes at width x height. */
static ImageDecodeStatus retarget_decode(const ImageResources *images,
                                         const ImageResource *owner,
                                         int width, int height,
                                         unsigned char **pixels)
{
    ImageResource decode = *owner;
    decode.pixels = NULL;
    decode.pixel_body = NULL;
    decode.width = width;
    decode.height = height;
    return image_resource_decode_checked(&decode, images->budget, pixels);
}

/* Rasterizes a kept parse of the owner's markup at width x height,
   reporting as image_resource_decode_checked does. */
static ImageDecodeStatus retarget_rasterize_parse(const ImageResources *images,
                                                  ImageSvgParse *parse,
                                                  int width, int height,
                                                  unsigned char **pixels)
{
    if (width <= 0 || height <= 0
        || (size_t) width > SIZE_MAX / 4u / (size_t) height) {
        image_svg_parse_release(parse);
        return IMAGE_DECODE_DETERMINISTIC_FAILURE;
    }
    size_t failures_before = images->budget->failure_count;
    int raster_width = width, raster_height = height;
    *pixels = image_svg_parse_rasterize(
        parse, (size_t) width * (size_t) height * 4u, &raster_width,
        &raster_height);
    if (*pixels != NULL) return IMAGE_DECODE_SUCCEEDED;
    return images->budget->failure_count != failures_before
        ? IMAGE_DECODE_TRANSIENT_FAILURE
        : IMAGE_DECODE_DETERMINISTIC_FAILURE;
}

/* Decodes (or, for SVG markup, rasterizes) the surface again at
   width x height and counts the change in *counter. A kept parse of its
   markup (taken from the table, or NULL) is rasterized instead of parsing
   the markup again, and is released whatever the outcome. */
static ImageRetargetOutcome retarget_restore_parsed(
    ImageResources *images, ImageResource *owner, const void *identity,
    int width, int height, uint8_t set_flags, BrowserSession *session,
    size_t *counter, ImageSvgParse *parse)
{
    unsigned char *pixels = NULL;
    if (parse != NULL && image_svg_decode_busy()) {
        /* Another raster is in progress: retry from the markup later. */
        image_svg_parse_release(parse);
        parse = NULL;
    }
    ImageDecodeStatus status = parse != NULL
        ? retarget_rasterize_parse(images, parse, width, height, &pixels)
        : retarget_decode(images, owner, width, height, &pixels);
    if (status == IMAGE_DECODE_TRANSIENT_FAILURE) {
        images->retarget.deferred++;
        return IMAGE_RETARGET_RETRY;
    }
    if (status != IMAGE_DECODE_SUCCEEDED) {
        for (size_t i = 0; i < images->count; i++) {
            if (retarget_identity(&images->items[i]) == identity) {
                images->items[i].retarget_flags |= IMAGE_RETARGET_REFUSED;
            }
        }
        return IMAGE_RETARGET_CHANGED;
    }
    if ((owner->retarget_flags & IMAGE_RETARGET_VECTOR) != 0)
        images->retarget.rasters++;
    retarget_replace(images, identity, owner, pixels, width, height,
                     set_flags, session);
    (*counter)++;
    return IMAGE_RETARGET_CHANGED;
}

static ImageRetargetOutcome retarget_restore(
    ImageResources *images, ImageResource *owner, const void *identity,
    int width, int height, uint8_t set_flags, BrowserSession *session,
    size_t *counter)
{
    return retarget_restore_parsed(images, owner, identity, width, height,
                                   set_flags, session, counter, NULL);
}

size_t images_retarget_svg_parse_allowance(const ImageResources *images)
{
    if (images == NULL
        || images->svg_parse_bytes >= IMAGE_SVG_KEPT_PARSE_BYTES) return 0;
    for (size_t i = 0; i < IMAGE_SVG_KEPT_PARSE_LIMIT; i++) {
        if (images->svg_parses[i].parse == NULL)
            return IMAGE_SVG_KEPT_PARSE_BYTES - images->svg_parse_bytes;
    }
    return 0;
}

void images_retarget_hold_svg_parse(ImageResources *images,
                                    const void *identity,
                                    struct ImageSvgParse *parse)
{
    if (parse == NULL) return;
    size_t bytes = image_svg_parse_bytes(parse);
    for (size_t i = 0; images != NULL && identity != NULL
             && i < IMAGE_SVG_KEPT_PARSE_LIMIT; i++) {
        if (images->svg_parses[i].parse != NULL) continue;
        if (bytes > images_retarget_svg_parse_allowance(images)) break;
        images->svg_parses[i].identity = identity;
        images->svg_parses[i].parse = parse;
        images->svg_parse_bytes += bytes;
        return;
    }
    image_svg_parse_release(parse);
}

static void retarget_drop_svg_parse(ImageResources *images, size_t slot)
{
    ImageSvgParse *parse = images->svg_parses[slot].parse;
    if (parse == NULL) return;
    size_t bytes = image_svg_parse_bytes(parse);
    images->svg_parse_bytes = images->svg_parse_bytes >= bytes
        ? images->svg_parse_bytes - bytes : 0;
    images->svg_parses[slot].parse = NULL;
    images->svg_parses[slot].identity = NULL;
    image_svg_parse_release(parse);
}

void images_retarget_release_svg_parses(ImageResources *images)
{
    if (images == NULL) return;
    for (size_t i = 0; i < IMAGE_SVG_KEPT_PARSE_LIMIT; i++)
        retarget_drop_svg_parse(images, i);
    images->svg_parse_bytes = 0;
}

/* The kept parse of exactly the owner's markup, taken out of the table, or
   NULL. A parse held under the same bytes' address but made from other
   markup (the bytes were released and the address reused) is dropped. */
static ImageSvgParse *retarget_take_svg_parse(ImageResources *images,
                                              const ImageResource *owner)
{
    for (size_t i = 0; i < IMAGE_SVG_KEPT_PARSE_LIMIT; i++) {
        ImageSvgParse *parse = images->svg_parses[i].parse;
        if (parse == NULL || images->svg_parses[i].identity != owner->encoded)
            continue;
        if (!image_svg_parse_matches(parse, owner->encoded,
                                     owner->encoded_length)) {
            retarget_drop_svg_parse(images, i);
            continue;
        }
        size_t bytes = image_svg_parse_bytes(parse);
        images->svg_parse_bytes = images->svg_parse_bytes >= bytes
            ? images->svg_parse_bytes - bytes : 0;
        images->svg_parses[i].parse = NULL;
        images->svg_parses[i].identity = NULL;
        return parse;
    }
    return NULL;
}

/* Keeps only the parses an item still waiting for a layout can use: one
   judged far, or no longer in the table, holds nothing. */
static void retarget_sweep_svg_parses(ImageResources *images)
{
    for (size_t slot = 0; slot < IMAGE_SVG_KEPT_PARSE_LIMIT; slot++) {
        const void *identity = images->svg_parses[slot].identity;
        if (images->svg_parses[slot].parse == NULL) continue;
        bool waiting = false;
        for (size_t i = 0; i < images->count && !waiting; i++) {
            const ImageResource *item = &images->items[i];
            waiting = item->encoded == identity && item->owns_encoded
                && (item->retarget_flags & IMAGE_RETARGET_AWAITING_LAYOUT)
                       != 0;
        }
        if (!waiting) retarget_drop_svg_parse(images, slot);
    }
}

/* The item that owns the surface's encoded bytes and, when present, its
   pixels; NULL when the surface does not take part. */
static ImageResource *retarget_owner(ImageResources *images, size_t first,
                                     const void *identity, bool *eligible)
{
    ImageResource *owner = NULL;
    *eligible = true;
    for (size_t i = first; i < images->count; i++) {
        ImageResource *item = &images->items[i];
        if (retarget_identity(item) != identity) continue;
        if (item->is_canvas || item->canvas_native_surface != NULL
            || item->borrows_previous
            || (item->retarget_flags
                & (IMAGE_RETARGET_PINNED | IMAGE_RETARGET_REFUSED)) != 0) {
            *eligible = false;
        }
        if (item->owns_encoded && item->encoded != NULL
            && item->encoded_length != 0
            && (item->pixels == NULL || item->owns_pixels)) {
            owner = item;
        }
    }
    if (owner == NULL) *eligible = false;
    return owner;
}

/* Whether the distance rule applies to the surface `owner` holds: SVG
   markup whose first raster is IMAGE_RETARGET_DISTANT_MINIMUM_BYTES or
   more. */
static bool retarget_distant_eligible(const ImageResource *owner)
{
    if (owner == NULL
        || (owner->retarget_flags & IMAGE_RETARGET_VECTOR) == 0) return false;
    int width = owner->full_width > 0 ? owner->full_width : owner->width;
    int height = owner->full_height > 0 ? owner->full_height : owner->height;
    return width > 0 && height > 0
        && (uint64_t) width * (uint64_t) height * 4u
               >= IMAGE_RETARGET_DISTANT_MINIMUM_BYTES;
}

static void retarget_fold_need(RetargetNeed *into, const RetargetNeed *other)
{
    into->painted |= other->painted;
    into->fixed |= other->fixed;
    into->near |= other->near;
    into->kept |= other->kept;
    if (other->width > into->width) into->width = other->width;
    if (other->height > into->height) into->height = other->height;
}

static void retarget_mark_group(ImageResources *images, const void *identity,
                                uint8_t set, uint8_t clear)
{
    for (size_t i = 0; i < images->count; i++) {
        ImageResource *item = &images->items[i];
        if (retarget_identity(item) != identity) continue;
        item->retarget_flags = (uint8_t) ((item->retarget_flags & ~clear)
                                          | set);
    }
}

/* The size a released surface is decoded at once painted again, and the
   flag that records it. A raster that had grown keeps its last size. A
   large SVG raster released for distance takes what display retargeting
   would leave it at had it never been released: the painted size when
   that is half its first area or less, otherwise its first size. Any
   other released surface takes its painted size when that is smaller. */
static void retarget_restore_size(const ImageResource *owner,
                                  const RetargetNeed *need, uint8_t flags,
                                  int full_width, int full_height,
                                  int *width, int *height, uint8_t *set)
{
    *width = full_width;
    *height = full_height;
    bool smaller = !need->fixed && need->width > 0 && need->height > 0
        && need->width < full_width && need->height < full_height;
    if ((flags & IMAGE_RETARGET_GROWN) != 0) {
        /* Pressure release is not permission to reduce a surface
           which already grew. Restore its last published size. */
        *width = owner->width;
        *height = owner->height;
    } else if ((flags & IMAGE_RETARGET_DISTANT) != 0) {
        if (smaller && (uint64_t) need->width * (uint64_t) need->height * 2u
                <= (uint64_t) full_width * (uint64_t) full_height) {
            *width = need->width;
            *height = need->height;
        }
        *set = *width < full_width ? IMAGE_RETARGET_SHRUNK : 0;
        return;
    } else if (smaller) {
        *width = need->width;
        *height = need->height;
    }
    *set = *width < full_width ? IMAGE_RETARGET_SHRUNK : IMAGE_RETARGET_GROWN;
}

/* Rasterizes a released large SVG raster painted near the reader. */
static ImageRetargetOutcome retarget_restore_distant(
    ImageResources *images, ImageResource *owner, const void *identity,
    const RetargetNeed *need, uint8_t flags, BrowserSession *session,
    size_t *counter)
{
    int full_width = owner->full_width > 0 ? owner->full_width : owner->width;
    int full_height = owner->full_height > 0
        ? owner->full_height : owner->height;
    int width = 0, height = 0;
    uint8_t set = 0;
    retarget_restore_size(owner, need, flags, full_width, full_height,
                          &width, &height, &set);
    ImageRetargetOutcome outcome = retarget_restore(
        images, owner, identity, width, height, set, session,
        &images->retarget.grows);
    if (owner->pixels != NULL) {
        /* The group is now known by its new pixels. */
        retarget_mark_group(images, retarget_identity(owner), 0,
                            IMAGE_RETARGET_DISTANT);
        (*counter)++;
    }
    return outcome;
}

bool images_retarget_distance_candidates(const ImageResources *images)
{
    if (images == NULL) return false;
    for (size_t i = 0; i < images->count; i++) {
        const ImageResource *item = &images->items[i];
        if (item->owns_encoded && item->encoded != NULL
            && (item->pixels == NULL || item->owns_pixels)
            && retarget_distant_eligible(item)) return true;
    }
    return false;
}

size_t images_retarget_restore_near(ImageResources *images,
                                    const LayoutDocument *layout,
                                    BrowserSession *session,
                                    const ImageRetargetReader *reader,
                                    size_t limit)
{
    if (images == NULL || layout == NULL || reader == NULL || limit == 0
        || images->count == 0 || images->budget == NULL) return 0;
    /* Nothing released: no scan, no allocation (the common case). */
    bool released = false;
    for (size_t i = 0; i < images->count && !released; i++) {
        const ImageResource *item = &images->items[i];
        released = item->pixels == NULL && item->owns_encoded
            && (item->retarget_flags & IMAGE_RETARGET_DROPPED) != 0
            && retarget_distant_eligible(item);
    }
    if (!released) return 0;
    uint64_t started = tilefinch_platform_monotonic_time_us();
    RetargetNeed *needs = budget_calloc_category(
        images->budget, BUDGET_CATEGORY_RESOURCE, images->count,
        sizeof(*needs));
    if (needs == NULL) return 0;
    size_t restored = 0;
    bool scanned = retarget_scan(images, layout, needs, reader, NULL);
    for (size_t i = 0; scanned && i < images->count && restored < limit;
         i++) {
        ImageResource *item = &images->items[i];
        if (item->pixels != NULL || !item->owns_encoded
            || (item->retarget_flags & IMAGE_RETARGET_DROPPED) == 0
            || !retarget_distant_eligible(item)) continue;
        const void *identity = retarget_identity(item);
        if (identity == NULL) continue;
        RetargetNeed need = {0};
        uint8_t flags = 0;
        for (size_t j = 0; j < images->count; j++) {
            if (retarget_identity(&images->items[j]) != identity) continue;
            retarget_fold_need(&need, &needs[j]);
            flags |= images->items[j].retarget_flags;
        }
        if (!need.painted || !need.near) continue;
        bool eligible = false;
        ImageResource *owner = retarget_owner(images, 0, identity, &eligible);
        if (!eligible || owner != item) continue;
        (void) retarget_restore_distant(
            images, owner, identity, &need, flags, session,
            &images->retarget.distant_input_restores);
        if (owner->pixels != NULL) restored++;
    }
    budget_free(images->budget, needs);
    if (restored != 0) images_retarget_plan_discard(images);
    images->retarget.us += tilefinch_platform_monotonic_time_us() - started;
    return restored;
}

size_t images_retarget_decode_awaiting(ImageResources *images,
                                       const LayoutDocument *layout,
                                       BrowserSession *session,
                                       const ImageRetargetReader *reader)
{
    if (images == NULL || layout == NULL || images->count == 0
        || images->budget == NULL) return 0;
    bool awaiting = false;
    for (size_t i = 0; i < images->count && !awaiting; i++)
        awaiting = (images->items[i].retarget_flags
                    & IMAGE_RETARGET_AWAITING_LAYOUT) != 0;
    if (!awaiting) {
        retarget_sweep_svg_parses(images);
        return 0;
    }
    RetargetNeed *needs = budget_calloc_category(
        images->budget, BUDGET_CATEGORY_RESOURCE, images->count,
        sizeof(*needs));
    if (needs == NULL) return 0;
    size_t decoded = 0;
    bool scanned = retarget_scan(images, layout, needs, reader, NULL);
    for (size_t i = 0; scanned && i < images->count; i++) {
        ImageResource *item = &images->items[i];
        if ((item->retarget_flags & IMAGE_RETARGET_AWAITING_LAYOUT) == 0)
            continue;
        const void *identity = retarget_identity(item);
        RetargetNeed need = needs[i];
        for (size_t j = i + 1; j < images->count; j++) {
            if (retarget_identity(&images->items[j]) != identity) continue;
            retarget_fold_need(&need, &needs[j]);
        }
        /* Unpainted (a preview's prefix, a provisional commit), it stays
           released for a later layout or display retarget step. */
        if (identity == NULL || !need.painted) continue;
        for (size_t j = i; j < images->count; j++) {
            if (retarget_identity(&images->items[j]) == identity)
                images->items[j].retarget_flags &=
                    (uint8_t) ~IMAGE_RETARGET_AWAITING_LAYOUT;
        }
        bool eligible = false;
        ImageResource *owner = retarget_owner(images, 0, identity, &eligible);
        if (!eligible || owner->pixels != NULL) continue;
        int full_width = owner->full_width > 0
            ? owner->full_width : owner->width;
        int full_height = owner->full_height > 0
            ? owner->full_height : owner->height;
        int width = full_width, height = full_height;
        bool vector = (owner->retarget_flags & IMAGE_RETARGET_VECTOR) != 0;
        if (retarget_distant_eligible(owner) && !need.near) {
            /* Painted only far from the reader: the display step
               rasterizes it when the reader comes near. */
            retarget_mark_group(images, identity, IMAGE_RETARGET_DISTANT, 0);
            continue;
        }
        /* SVG markup is rasterized at the size the load would have given
           it, so what is first painted is what an eager load paints. */
        if (!vector && !need.fixed && need.width > 0 && need.height > 0
            && need.width < full_width && need.height < full_height) {
            width = need.width;
            height = need.height;
        }
        /* At its full size it is as an eager decode would have left it. An
           SVG raster here is the load's own, made later: it counts as
           neither a restore nor a retarget rasterization. */
        size_t rasters = images->retarget.rasters, load_rasters = 0;
        (void) retarget_restore_parsed(
            images, owner, identity, width, height,
            width < full_width ? IMAGE_RETARGET_SHRUNK : 0, session,
            vector ? &load_rasters : &images->retarget.grows,
            vector ? retarget_take_svg_parse(images, owner) : NULL);
        if (vector) images->retarget.rasters = rasters;
        if (owner->pixels != NULL) decoded++;
    }
    budget_free(images->budget, needs);
    retarget_sweep_svg_parses(images);
    if (decoded != 0) images_retarget_plan_discard(images);
    return decoded;
}

size_t images_reclaim_rebuildable_rasters(ImageResources *images,
                                         BrowserSession *session,
                                         size_t needed_bytes)
{
    if (images == NULL || images->budget == NULL || needed_bytes == 0)
        return 0;
    size_t before = budget_remaining(images->budget);
    /* Parses kept for the next layout are the cheapest to give back. */
    images_retarget_release_svg_parses(images);
    bool changed = false;
    for (size_t i = 0; i < images->count; i++) {
        if (images->items[i].pixels == NULL) continue;
        const void *identity = retarget_identity(&images->items[i]);
        bool eligible;
        ImageResource *owner = retarget_owner(images, 0, identity, &eligible);
        if (!eligible) continue;
        retarget_replace(images, identity, owner, NULL,
            owner->width, owner->height, IMAGE_RETARGET_DROPPED, session);
        changed = true;
        images->retarget.drops++;
        size_t now = budget_remaining(images->budget);
        if (now > before && now - before >= needed_bytes) break;
    }
    /* A pending plan may have passed a newly dropped surface already. */
    if (changed) images_retarget_plan_discard(images);
    size_t after = budget_remaining(images->budget);
    return after > before ? after - before : 0;
}

ImageRetargetOutcome images_retarget_display_step(
    ImageResources *images, const LayoutDocument *layout,
    BrowserSession *session, size_t generation,
    const ImageRetargetReader *reader)
{
    if (images == NULL || layout == NULL || images->count == 0
        || images->budget == NULL) return IMAGE_RETARGET_IDLE;
    uint64_t started = tilefinch_platform_monotonic_time_us();
    struct ImageRetargetPlan *plan = images->retarget_plan;
    if (plan != NULL && (plan->generation != generation
        || plan->commands != layout->commands || plan->items != images->items
        || plan->command_count != layout->count
        || plan->image_count != images->count
        || plan->has_reader != (reader != NULL)
        || (reader != NULL && (memcmp(&plan->reader, reader,
                                      sizeof(*reader)) != 0
                               || (plan->scrolled
                                   && plan->scroll_generation
                                          != layout->scroll_generation))))) {
        images_retarget_plan_discard(images);
        plan = NULL;
    }
    ImageRetargetOutcome outcome = IMAGE_RETARGET_IDLE;
    if (plan == NULL) {
        size_t slots = 1;
        if (images->count > SIZE_MAX / 2u || images->count >= UINT32_MAX)
            return IMAGE_RETARGET_RETRY;
        while (slots < images->count * 2u) {
            if (slots > SIZE_MAX / 2u) return IMAGE_RETARGET_RETRY;
            slots *= 2u;
        }
        if (slots > (SIZE_MAX - sizeof(*plan)) / sizeof(uint32_t)
            || images->count > (SIZE_MAX - sizeof(*plan)
                - slots * sizeof(uint32_t)) / sizeof(RetargetNeed))
            return IMAGE_RETARGET_RETRY;
        plan = budget_calloc_category(images->budget, BUDGET_CATEGORY_RESOURCE,
            1, sizeof(*plan) + images->count * sizeof(RetargetNeed)
                + slots * sizeof(uint32_t));
        if (plan == NULL) return IMAGE_RETARGET_RETRY;
        images->retarget_plan = plan;
        plan->commands = layout->commands;
        plan->items = images->items;
        plan->command_count = layout->count;
        plan->image_count = images->count;
        plan->generation = generation;
        plan->has_reader = reader != NULL;
        if (reader != NULL) plan->reader = *reader;
        plan->scroll_generation = layout->scroll_generation;
        images->retarget.scans++;
        if (!retarget_scan(images, layout, plan->needs, reader,
                           &plan->scrolled)) {
            images_retarget_plan_discard(images);
            outcome = IMAGE_RETARGET_STALE;
            goto done;
        }
        /* Fold aliases once through a bounded identity table. Mark later
           aliases with a negative width so the cursor skips their group. */
        /* 32-bit indices preserve alignment after an odd number of needs
           on both the 32-bit device and 64-bit hosts. */
        uint32_t *groups = (uint32_t *) (plan->needs + images->count);
        for (size_t i = 0; i < images->count; i++) {
            const void *identity = retarget_identity(&images->items[i]);
            if (identity == NULL) continue;
            size_t slot = ((uintptr_t) identity >> 4) & (slots - 1u);
            for (size_t probes = 0; probes < slots; probes++) {
                if (groups[slot] == 0) {
                    groups[slot] = (uint32_t) i + 1u;
                    break;
                }
                size_t first = groups[slot] - 1u;
                if (retarget_identity(&images->items[first]) != identity) {
                    slot = (slot + 1u) & (slots - 1u);
                    continue;
                }
                RetargetNeed *a = &plan->needs[first], *b = &plan->needs[i];
                retarget_fold_need(a, b);
                b->width = -1;
                break;
            }
        }
    }
    for (size_t i = plan->cursor; i < images->count; i++) {
        plan->cursor = i;
        if (plan->needs[i].width < 0) continue;
        ImageResource *item = &images->items[i];
        const void *identity = retarget_identity(item);
        if (identity == NULL) continue;
        RetargetNeed need = plan->needs[i];
        uint8_t flags = 0;
        for (size_t j = i; j < images->count; j++) {
            if (retarget_identity(&images->items[j]) != identity) continue;
            flags |= images->items[j].retarget_flags;
        }
        bool eligible = false;
        ImageResource *owner = retarget_owner(images, i, identity, &eligible);
        if (!eligible) continue;
        int full_width = owner->full_width > 0
            ? owner->full_width : owner->width;
        int full_height = owner->full_height > 0
            ? owner->full_height : owner->height;
        bool distant = reader != NULL && retarget_distant_eligible(owner);
        if (owner->pixels == NULL) {
            /* Released while unpainted: decode once it is painted again, at
               the painted size (never above the original). A large SVG
               raster waits until a paint of it is near the reader. */
            if ((flags & IMAGE_RETARGET_DROPPED) == 0 || !need.painted
                || (distant && !need.near))
                continue;
            if (distant) {
                outcome = retarget_restore_distant(
                    images, owner, identity, &need, flags, session,
                    &images->retarget.distant_restores);
                goto done;
            }
            int width = 0, height = 0;
            uint8_t set = 0;
            retarget_restore_size(owner, &need, flags, full_width,
                                  full_height, &width, &height, &set);
            outcome = retarget_restore(
                images, owner, identity, width, height, set, session,
                &images->retarget.grows);
            goto done;
        }
        if (distant && need.painted && !need.kept) {
            /* Every paint is far from the reader: keep only the markup. */
            retarget_replace(images, identity, owner, NULL, owner->width,
                             owner->height,
                             IMAGE_RETARGET_DROPPED | IMAGE_RETARGET_DISTANT,
                             session);
            images->retarget.distant_releases++;
            outcome = IMAGE_RETARGET_CHANGED;
            goto done;
        }
        if (!need.painted) {
            if ((flags & IMAGE_RETARGET_GROWN) != 0) continue;
            retarget_replace(images, identity, owner, NULL, owner->width,
                             owner->height, IMAGE_RETARGET_DROPPED, session);
            images->retarget.drops++;
            outcome = IMAGE_RETARGET_CHANGED;
            goto done;
        }
        bool vector = (owner->retarget_flags & IMAGE_RETARGET_VECTOR) != 0;
        bool larger = need.fixed || need.width > owner->width
            || need.height > owner->height;
        if (larger) {
            if (owner->width >= full_width && owner->height >= full_height)
                continue;
            /* A raster grows back to its first-decoded size. SVG markup is
               rasterized at the painted size instead, at least doubling, so
               a size that keeps growing (zoom steps, an animation) costs a
               few rasterizations at most; neither is reduced again. */
            int width = full_width, height = full_height;
            if (vector && !need.fixed) {
                width = need.width > 2 * owner->width
                    ? need.width : 2 * owner->width;
                height = need.height > 2 * owner->height
                    ? need.height : 2 * owner->height;
                if (width > full_width) width = full_width;
                if (height > full_height) height = full_height;
            }
            outcome = retarget_restore(
                images, owner, identity, width, height,
                IMAGE_RETARGET_GROWN, session, &images->retarget.grows);
            goto done;
        }
        if ((flags & IMAGE_RETARGET_GROWN) != 0
            || need.width <= 0 || need.height <= 0) continue;
        if ((uint64_t) need.width * (uint64_t) need.height * 2u
            > (uint64_t) owner->width * (uint64_t) owner->height) continue;
        if (vector) {
            /* Rasterized at the painted size rather than sampled from the
               larger raster, so thin strokes keep their coverage. */
            outcome = retarget_restore(
                images, owner, identity, need.width, need.height,
                IMAGE_RETARGET_SHRUNK, session, &images->retarget.shrinks);
            goto done;
        }
        unsigned char *pixels = retarget_sample(
            images, owner, need.width, need.height);
        if (pixels == NULL) {
            outcome = IMAGE_RETARGET_RETRY;
            goto done;
        }
        retarget_replace(images, identity, owner, pixels, need.width,
                         need.height, IMAGE_RETARGET_SHRUNK, session);
        images->retarget.shrinks++;
        outcome = IMAGE_RETARGET_CHANGED;
        goto done;
    }
done:
    if (images->retarget_plan != NULL && outcome != IMAGE_RETARGET_RETRY) {
        if (outcome == IMAGE_RETARGET_CHANGED)
            images->retarget_plan->cursor++;
        else images_retarget_plan_discard(images);
    }
    images->retarget.us += tilefinch_platform_monotonic_time_us() - started;
    return outcome;
}

/* Whether the surface owned by `owner` is below its decoded size. */
static bool retarget_reduced(const ImageResource *owner)
{
    return owner != NULL
        && (owner->pixels == NULL
            || (owner->full_width > 0
                && (owner->width < owner->full_width
                    || owner->height < owner->full_height)));
}

bool images_retarget_pin_full_restores(ImageResources *images, size_t index)
{
    if (images == NULL || index >= images->count) return false;
    const void *identity = retarget_identity(&images->items[index]);
    if (identity == NULL) return false;
    bool eligible = false;
    ImageResource *owner = retarget_owner(images, 0, identity, &eligible);
    return eligible && retarget_reduced(owner);
}

bool images_retarget_pin_full(ImageResources *images, size_t index,
                              BrowserSession *session)
{
    if (images == NULL || index >= images->count) return false;
    ImageResource *item = &images->items[index];
    const void *identity = retarget_identity(item);
    if (identity == NULL) return false;
    bool eligible = false;
    ImageResource *owner = retarget_owner(images, 0, identity, &eligible);
    if (retarget_reduced(owner) && eligible) {
        ImageRetargetOutcome outcome = retarget_restore(
            images, owner, identity, owner->full_width, owner->full_height,
            IMAGE_RETARGET_GROWN, session, &images->retarget.grows);
        identity = retarget_identity(item);
        if (outcome == IMAGE_RETARGET_RETRY) return false;
    }
    for (size_t i = 0; i < images->count; i++) {
        if (retarget_identity(&images->items[i]) == identity) {
            images->items[i].retarget_flags |= IMAGE_RETARGET_PINNED;
        }
    }
    return item->pixels != NULL;
}
