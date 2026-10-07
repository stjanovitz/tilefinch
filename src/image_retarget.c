/* Display retargeting of decoded rasters; see tilefinch/image_retarget.h. */
#include "tilefinch/image_retarget.h"

#include <stdint.h>
#include <string.h>

#include "tilefinch/budget.h"
#include "tilefinch/platform.h"
#include "image_decode_internal.h"

typedef struct {
    int width;
    int height;
    bool painted;
    bool fixed;
} RetargetNeed;

struct ImageRetargetPlan {
    const DrawCommand *commands;
    const ImageResource *items;
    size_t command_count, image_count, generation, cursor;
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

static bool retarget_scan(const ImageResources *images,
                          const LayoutDocument *layout, RetargetNeed *needs)
{
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
    BrowserSharedBody *old_body = owner->pixel_body;
    unsigned char *old_pixels = owner->pixels;
    bool old_owned = owner->owns_pixels;
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
        item->pixels = pixels;
        item->pixel_body = body;
        item->width = width;
        item->height = height;
        item->full_width = full_width;
        item->full_height = full_height;
        item->retarget_flags |= set_flags;
        item->owns_pixels = item == owner && pixels != NULL;
    }
    if (old_owned) {
        if (old_body != NULL) browser_shared_body_release(old_body);
        else budget_free(images->budget, old_pixels);
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

/* Decodes (or, for SVG markup, rasterizes) the surface again at
   width x height and counts the change in *counter. */
static ImageRetargetOutcome retarget_restore(
    ImageResources *images, ImageResource *owner, const void *identity,
    int width, int height, uint8_t set_flags, BrowserSession *session,
    size_t *counter)
{
    unsigned char *pixels = NULL;
    ImageDecodeStatus status = retarget_decode(
        images, owner, width, height, &pixels);
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

ImageRetargetOutcome images_retarget_display_step(
    ImageResources *images, const LayoutDocument *layout,
    BrowserSession *session, size_t generation)
{
    if (images == NULL || layout == NULL || images->count == 0
        || images->budget == NULL) return IMAGE_RETARGET_IDLE;
    uint64_t started = tilefinch_platform_monotonic_time_us();
    struct ImageRetargetPlan *plan = images->retarget_plan;
    if (plan != NULL && (plan->generation != generation
        || plan->commands != layout->commands || plan->items != images->items
        || plan->command_count != layout->count
        || plan->image_count != images->count)) {
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
        images->retarget.scans++;
        if (!retarget_scan(images, layout, plan->needs)) {
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
                a->painted |= b->painted;
                a->fixed |= b->fixed;
                if (b->width > a->width) a->width = b->width;
                if (b->height > a->height) a->height = b->height;
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
        if (owner->pixels == NULL) {
            /* Released while unpainted: decode once it is painted again, at
               the painted size (never above the original). */
            if ((flags & IMAGE_RETARGET_DROPPED) == 0 || !need.painted)
                continue;
            int width = full_width, height = full_height;
            if (!need.fixed && need.width > 0 && need.height > 0
                && need.width < full_width && need.height < full_height) {
                width = need.width;
                height = need.height;
            }
            outcome = retarget_restore(
                images, owner, identity, width, height,
                width < full_width ? IMAGE_RETARGET_SHRUNK
                                   : IMAGE_RETARGET_GROWN, session,
                &images->retarget.grows);
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

bool images_retarget_pin_full(ImageResources *images, size_t index,
                              BrowserSession *session)
{
    if (images == NULL || index >= images->count) return false;
    ImageResource *item = &images->items[index];
    const void *identity = retarget_identity(item);
    if (identity == NULL) return false;
    bool eligible = false;
    ImageResource *owner = retarget_owner(images, 0, identity, &eligible);
    bool reduced = owner != NULL
        && (owner->pixels == NULL
            || (owner->full_width > 0
                && (owner->width < owner->full_width
                    || owner->height < owner->full_height)));
    if (reduced && eligible) {
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
