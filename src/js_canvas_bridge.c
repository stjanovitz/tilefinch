#include "js_runtime_internal.h"
#include "tilefinch/image_retarget.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define CANVAS_BRIDGE_PIXEL_BYTE_LIMIT (512u * 1024u)
#define CANVAS_RASTER_WORK_LIMIT (4u * 1024u * 1024u)
/* canvas.js keeps every surface at or below 512 KiB of RGBA pixels and
   flushes rectangle batches of at most 32 commands. The facade retains a
   batch whose native call reports status 2 (clipped), so a batch of 32
   full-surface rectangles must always fit one call's allowance; otherwise a
   retained batch could replay rectangles that were already blended. */
#define CANVAS_SURFACE_PIXEL_LIMIT ((512u * 1024u) / 4u)
#define CANVAS_RECT_BATCH_LIMIT 32u
_Static_assert(CANVAS_RECT_BATCH_LIMIT * CANVAS_SURFACE_PIXEL_LIMIT
                   <= CANVAS_RASTER_WORK_LIMIT,
               "canvas rectangle batch must fit the raster work allowance");

static bool canvas_work_take(size_t *remaining, size_t cost)
{
    if (remaining == NULL || cost > *remaining) return false;
    *remaining -= cost;
    return true;
}

static uint8_t canvas_byte(double value)
{
    if (value <= 0.0) return 0;
    if (value >= 255.0) return 255;
    return (uint8_t) (value + 0.5);
}

static void canvas_blend_pixel(uint8_t *pixels, size_t at,
                               int red, int green, int blue, int alpha,
                               double global_alpha, int operation,
                               double coverage)
{
    /* Opaque source-over and copy produce exactly the source color at full
       alpha; skip the (soft-float on the PSP) blend. */
    if ((operation == 1 || operation == 2) && alpha == 255
        && global_alpha == 1.0 && coverage == 1.0) {
        pixels[at] = (uint8_t) red;
        pixels[at + 1u] = (uint8_t) green;
        pixels[at + 2u] = (uint8_t) blue;
        pixels[at + 3u] = 255u;
        return;
    }
    double source_alpha = ((double) alpha / 255.0) * global_alpha * coverage;
    if (source_alpha < 0.0) source_alpha = 0.0;
    if (source_alpha > 1.0) source_alpha = 1.0;
    double destination_alpha = (double) pixels[at + 3u] / 255.0;
    double source_factor = 1.0;
    double destination_factor = 1.0 - source_alpha;
    switch (operation) {
        case 2: destination_factor = 0.0; break; /* copy */
        case 3: /* destination-over */
            source_factor = 1.0 - destination_alpha;
            destination_factor = 1.0;
            break;
        case 4: source_factor = destination_alpha; destination_factor = 0.0; break;
        case 5: source_factor = 1.0 - destination_alpha; destination_factor = 0.0; break;
        case 6: source_factor = destination_alpha; destination_factor = 1.0 - source_alpha; break;
        case 7: source_factor = 0.0; destination_factor = source_alpha; break;
        case 8: source_factor = 0.0; destination_factor = 1.0 - source_alpha; break;
        case 9: source_factor = 1.0 - destination_alpha; destination_factor = source_alpha; break;
        case 10:
            source_factor = 1.0 - destination_alpha;
            destination_factor = 1.0 - source_alpha;
            break;
        case 11: destination_factor = 1.0; break;
        default: break;
    }
    double output_alpha = source_alpha * source_factor
        + destination_alpha * destination_factor;
    if (output_alpha > 1.0) output_alpha = 1.0;
    if (output_alpha <= 0.0) {
        memset(pixels + at, 0, 4u);
        return;
    }
    pixels[at] = canvas_byte(
        ((double) red * source_alpha * source_factor
         + (double) pixels[at] * destination_alpha * destination_factor)
        / output_alpha);
    pixels[at + 1u] = canvas_byte(
        ((double) green * source_alpha * source_factor
         + (double) pixels[at + 1u] * destination_alpha * destination_factor)
        / output_alpha);
    pixels[at + 2u] = canvas_byte(
        ((double) blue * source_alpha * source_factor
         + (double) pixels[at + 2u] * destination_alpha * destination_factor)
        / output_alpha);
    pixels[at + 3u] = canvas_byte(output_alpha * 255.0);
}

static bool canvas_typed_bytes(JSContext *context, JSValueConst value,
                               size_t element_size, const uint8_t **bytes,
                               size_t *count, JSValue *buffer)
{
    size_t offset = 0, length = 0, bytes_per_element = 0;
    *buffer = JS_GetTypedArrayBuffer(
        context, value, &offset, &length, &bytes_per_element);
    if (JS_IsException(*buffer)) return false;
    size_t buffer_length = 0;
    uint8_t *storage = JS_GetArrayBuffer(context, &buffer_length, *buffer);
    if (bytes_per_element != element_size || storage == NULL
        || offset > buffer_length || length > buffer_length - offset) {
        JS_FreeValue(context, *buffer);
        *buffer = JS_UNDEFINED;
        return false;
    }
    *bytes = storage + offset;
    *count = length / element_size;
    return true;
}

static double canvas_double_at(const uint8_t *bytes, size_t index)
{
    double value = 0.0;
    memcpy(&value, bytes + index * sizeof(value), sizeof(value));
    return value;
}

typedef enum {
    CANVAS_INTEGER_TRUNCATE = 0,
    CANVAS_INTEGER_FLOOR,
    CANVAS_INTEGER_CEIL,
    CANVAS_INTEGER_NEAREST
} CanvasIntegerRounding;

/* A float-to-int conversion outside the representable range is undefined C
   behaviour.  Packed Canvas commands are authored by JavaScript and must be
   treated as hostile even when the bootstrap normally emits small values. */
static bool canvas_integer(double value, CanvasIntegerRounding rounding,
                           int *output)
{
    if (output == NULL || !isfinite(value)) return false;
    double rounded = value;
    if (rounding == CANVAS_INTEGER_FLOOR) rounded = floor(value);
    else if (rounding == CANVAS_INTEGER_CEIL) rounded = ceil(value);
    else if (rounding == CANVAS_INTEGER_NEAREST) rounded = floor(value + 0.5);
    if (rounded < (double) INT_MIN || rounded > (double) INT_MAX) return false;
    *output = (int) rounded;
    return true;
}

static bool canvas_dimensions(int32_t width, int32_t height, size_t *required)
{
    if (width <= 0 || height <= 0
        || (size_t) width > SIZE_MAX / (size_t) height
        || (size_t) width * (size_t) height > SIZE_MAX / 4u) return false;
    *required = (size_t) width * (size_t) height * 4u;
    return *required <= CANVAS_BRIDGE_PIXEL_BYTE_LIMIT;
}

static const FontFace *canvas_font_face(const DomBridge *bridge,
                                        int family, bool italic, bool bold)
{
    if (bridge == NULL || bridge->fonts == NULL) return NULL;
    FontFamily selected = FONT_SANS;
    if (family == 1) selected = FONT_SERIF;
    else if (family == 2) selected = FONT_MONOSPACE;
    return font_set_face_variant(bridge->fonts, selected, italic, bold);
}

static bool canvas_path_inside(const uint8_t *points, size_t point_count,
                               double x, double y, bool even_odd);

/* Clip stacks are serialized as [count, evenOdd, coordinateCount,
   x0, y0, ...]. Each path keeps NaN pairs between subpaths. The format stays
   local to the bootstrap/native seam and is bounded to four clip operations. */
static bool canvas_clip_stack_contains(const uint8_t *clips,
                                       size_t clip_value_count,
                                       double x, double y)
{
    if (clips == NULL || clip_value_count < 1u) return true;
    double raw_count = canvas_double_at(clips, 0);
    if (!isfinite(raw_count) || raw_count < 0.0 || raw_count > 4.0
        || floor(raw_count) != raw_count) return false;
    size_t clip_count = (size_t) raw_count;
    size_t at = 1u;
    for (size_t clip = 0; clip < clip_count; clip++) {
        if (at > clip_value_count || clip_value_count - at < 2u) return false;
        double raw_even_odd = canvas_double_at(clips, at++);
        double raw_coordinates = canvas_double_at(clips, at++);
        if (!isfinite(raw_even_odd) || !isfinite(raw_coordinates)
            || raw_coordinates < 4.0 || raw_coordinates > 2048.0
            || floor(raw_coordinates) != raw_coordinates) return false;
        size_t coordinate_count = (size_t) raw_coordinates;
        if ((coordinate_count & 1u) != 0u
            || coordinate_count > clip_value_count - at) return false;
        if (!canvas_path_inside(
                clips + at * sizeof(double), coordinate_count / 2u,
                x, y, raw_even_odd != 0.0)) return false;
        at += coordinate_count;
    }
    return at == clip_value_count;
}

/* Native pixel stores (DomBridge.canvas_taints). */
static JSRuntime *canvas_store_runtime(const DomBridge *bridge)
{
    return bridge != NULL && bridge->host != NULL
        ? bridge->host->runtime : NULL;
}

static DomBridgeCanvasStore *canvas_store_for_node(
    const DomBridge *bridge, const lxb_dom_node_t *node)
{
    if (bridge == NULL || node == NULL) return NULL;
    for (size_t i = 0; i < bridge->canvas_taint_count; i++)
        if (bridge->canvas_taints[i].node == node)
            return &bridge->canvas_taints[i];
    return NULL;
}

bool js_rt_bridge_canvas_tainted(const DomBridge *bridge,
                                 const lxb_dom_node_t *node)
{
    return canvas_store_for_node(bridge, node) != NULL;
}

static size_t canvas_store_bitmap_count(const DomBridge *bridge)
{
    size_t count = 0;
    for (size_t i = 0; i < bridge->canvas_taint_count; i++)
        if (bridge->canvas_taints[i].node == NULL) count++;
    return count;
}

static void canvas_store_drop_pixels(const DomBridge *bridge,
                                     DomBridgeCanvasStore *store)
{
    JSRuntime *runtime = canvas_store_runtime(bridge);
    if (store->pixels != NULL && runtime != NULL)
        js_free_rt(runtime, store->pixels);
    store->pixels = NULL;
    store->width = 0;
    store->height = 0;
}

/* A zeroed (or copied) store of width x height, charged to the realm's
   JavaScript heap limit exactly as the Uint8ClampedArray it replaces. */
static uint8_t *canvas_store_allocate(const DomBridge *bridge,
                                      int32_t width, int32_t height,
                                      const uint8_t *copy)
{
    size_t required = 0;
    JSRuntime *runtime = canvas_store_runtime(bridge);
    if (runtime == NULL || !canvas_dimensions(width, height, &required))
        return NULL;
    uint8_t *pixels = js_malloc_rt(runtime, required);
    if (pixels == NULL) return NULL;
    if (copy != NULL) memcpy(pixels, copy, required);
    else memset(pixels, 0, required);
    return pixels;
}

/* A new zeroed entry, or NULL when the table is full or cannot grow. */
static DomBridgeCanvasStore *canvas_store_append(DomBridge *bridge)
{
    const size_t limit = DOM_BRIDGE_CANVAS_TAINT_LIMIT
        + DOM_BRIDGE_CANVAS_BITMAP_LIMIT;
    if (bridge->budget == NULL || bridge->canvas_taint_count >= limit)
        return NULL;
    if (bridge->canvas_taint_count == bridge->canvas_taint_capacity) {
        size_t capacity = bridge->canvas_taint_capacity == 0u ? 4u
            : bridge->canvas_taint_capacity * 2u;
        if (capacity > limit) capacity = limit;
        DomBridgeCanvasStore *grown = budget_realloc(
            bridge->budget, bridge->canvas_taints,
            capacity * sizeof(*grown));
        if (grown == NULL) return NULL;
        bridge->canvas_taints = grown;
        bridge->canvas_taint_capacity = capacity;
    }
    DomBridgeCanvasStore *entry =
        &bridge->canvas_taints[bridge->canvas_taint_count++];
    memset(entry, 0, sizeof(*entry));
    return entry;
}

static void canvas_store_remove(DomBridge *bridge,
                                DomBridgeCanvasStore *store)
{
    canvas_store_drop_pixels(bridge, store);
    size_t at = (size_t) (store - bridge->canvas_taints);
    bridge->canvas_taints[at] =
        bridge->canvas_taints[--bridge->canvas_taint_count];
}

void js_rt_bridge_canvas_taints_retire_subtree(DomBridge *bridge,
                                               const lxb_dom_node_t *root)
{
    if (bridge == NULL || root == NULL) return;
    size_t kept = 0;
    for (size_t i = 0; i < bridge->canvas_taint_count; i++) {
        DomBridgeCanvasStore *entry = &bridge->canvas_taints[i];
        const lxb_dom_node_t *at = entry->node;
        size_t depth = 0;
        /* As images_retire_subtree: a walk cut short by its bound retires
           the entry rather than keep a possibly dangling pointer. The
           element is being destroyed with its pixels, so nothing tainted
           outlives the entry. Bitmap stores have no node and stay. */
        while (at != NULL && at != root && ++depth < 4096u) at = at->parent;
        if (at == NULL) bridge->canvas_taints[kept++] = *entry;
        else canvas_store_drop_pixels(bridge, entry);
    }
    bridge->canvas_taint_count = kept;
}

void js_rt_bridge_canvas_taints_free(DomBridge *bridge)
{
    if (bridge == NULL) return;
    for (size_t i = 0; i < bridge->canvas_taint_count; i++)
        canvas_store_drop_pixels(bridge, &bridge->canvas_taints[i]);
    budget_free(bridge->budget, bridge->canvas_taints);
    bridge->canvas_taints = NULL;
    bridge->canvas_taint_count = 0;
    bridge->canvas_taint_capacity = 0;
}

static lxb_dom_node_t *canvas_handle_node(JSContext *context,
                                          DomBridge *bridge,
                                          JSValueConst value)
{
    lxb_dom_node_t *node = js_rt_bridge_node_arg(context, bridge, value);
    return node != NULL && node->type == LXB_DOM_NODE_TYPE_ELEMENT
        && node->local_name == LXB_TAG_CANVAS ? node : NULL;
}

/* The store a number names: a negative ImageBitmap store id, or a canvas
   element handle (NULL unless that canvas is tainted). Reading a number
   runs no script. */
static DomBridgeCanvasStore *canvas_store_for_value(DomBridge *bridge,
                                                    JSContext *context,
                                                    JSValueConst value,
                                                    lxb_dom_node_t **node)
{
    int64_t key = 0;
    if (node != NULL) *node = NULL;
    if (bridge == NULL || !JS_IsNumber(value)
        || JS_ToInt64(context, &key, value) < 0 || key == 0) return NULL;
    if (key < 0) {
        for (size_t i = 0; i < bridge->canvas_taint_count; i++) {
            DomBridgeCanvasStore *store = &bridge->canvas_taints[i];
            if (store->node == NULL && store->bitmap_id > 0
                && -(int64_t) store->bitmap_id == key) return store;
        }
        return NULL;
    }
    size_t slot = 0;
    lxb_dom_node_t *found = js_rt_bridge_node_slot_for_handle(
            bridge, key, &slot) ? bridge->nodes[slot] : NULL;
    if (node != NULL) *node = found;
    if (found == NULL || found->type != LXB_DOM_NODE_TYPE_ELEMENT
        || found->local_name != LXB_TAG_CANVAS) return NULL;
    return canvas_store_for_node(bridge, found);
}

/* The pixels a raster native draws into: a script-owned typed array of at
   least width * height * 4 bytes, or a number naming a native store of
   exactly width x height (*native is then set). */
static bool canvas_mutable_pixels(JSContext *context, JSValueConst value,
                                  int32_t width, int32_t height,
                                  size_t required, uint8_t **pixels,
                                  JSValue *buffer, bool *native)
{
    *native = false;
    *buffer = JS_UNDEFINED;
    if (JS_IsNumber(value)) {
        DomBridgeCanvasStore *store = canvas_store_for_value(
            JS_GetContextOpaque(context), context, value, NULL);
        if (store == NULL || store->pixels == NULL
            || store->width != width || store->height != height)
            return false;
        *pixels = store->pixels;
        *native = true;
        return true;
    }
    size_t offset = 0, length = 0, bytes_per_element = 0;
    *buffer = JS_GetTypedArrayBuffer(
        context, value, &offset, &length, &bytes_per_element);
    if (JS_IsException(*buffer)) return false;
    size_t buffer_length = 0;
    uint8_t *bytes = JS_GetArrayBuffer(context, &buffer_length, *buffer);
    if (bytes_per_element != 1u || bytes == NULL
        || offset > buffer_length || length > buffer_length - offset
        || length < required) {
        JS_FreeValue(context, *buffer);
        *buffer = JS_UNDEFINED;
        return false;
    }
    *pixels = bytes + offset;
    return true;
}

/* The decoded pixels script may draw from an element (its image, its
   committed canvas surface, or its preload), pinned at full decoded size;
   NULL when none is available or it exceeds a canvas surface. */
static const ImageResource *canvas_image_resolve(DomBridge *bridge,
                                                 lxb_dom_node_t *node)
{
    if (bridge == NULL || node == NULL) return NULL;
    const ImageResource *image = images_find_node(bridge->images, node);
    ImageResources *owner = bridge->images;
    bool preload = false;
    if (!image_resource_available(image)) {
        owner = js_rt_image_preload_resources(bridge);
        image = js_rt_image_preload_find(bridge, node);
        preload = true;
    }
    if (!image_resource_available(image)) return NULL;
    if (preload)
        js_rt_image_preload_prepare_pin(
            bridge, (size_t) (image - owner->items));
    /* The pixels are drawn: undo any display retargeting and keep the
       surface at its full decoded size from now on. */
    if (image_resource_available(image)) {
        (void) images_retarget_pin_full(
            owner, (size_t) (image - owner->items),
            bridge->session);
    }
    size_t required = 0;
    if (!image_resource_available(image) || image->pixels == NULL
        || !canvas_dimensions(image->width, image->height, &required))
        return NULL;
    return image;
}

typedef struct {
    const uint8_t *pixels;
    int width;
    int height;
    /* Named by a number: a native store or an element's decoded image.
       Such pixels may be drawn into a native store only. */
    bool native;
    JSValue buffer;
} CanvasSource;

/* A raster native's image source: a script-owned typed array of at least
   width * height * 4 bytes, or a number naming a native store or an
   element whose decoded image (of any origin) is read here. A clean canvas
   is always passed as its script buffer, so a number naming one resolves
   to nothing. */
static bool canvas_source_pixels(JSContext *context, JSValueConst value,
                                 int width, int height, CanvasSource *source)
{
    memset(source, 0, sizeof(*source));
    source->buffer = JS_UNDEFINED;
    if (JS_IsNumber(value)) {
        DomBridge *bridge = JS_GetContextOpaque(context);
        lxb_dom_node_t *node = NULL;
        DomBridgeCanvasStore *store = canvas_store_for_value(
            bridge, context, value, &node);
        source->native = true;
        if (store != NULL) {
            source->pixels = store->pixels;
            source->width = store->width;
            source->height = store->height;
            return store->pixels != NULL;
        }
        if (node == NULL || (node->type == LXB_DOM_NODE_TYPE_ELEMENT
                             && node->local_name == LXB_TAG_CANVAS))
            return false;
        const ImageResource *image = canvas_image_resolve(bridge, node);
        if (image == NULL) return false;
        source->pixels = image->pixels;
        source->width = image->width;
        source->height = image->height;
        return true;
    }
    size_t required = 0, count = 0;
    if (!canvas_dimensions(width, height, &required)) return false;
    if (!canvas_typed_bytes(context, value, 1u, &source->pixels, &count,
                            &source->buffer)) return false;
    if (count < required) {
        JS_FreeValue(context, source->buffer);
        source->buffer = JS_UNDEFINED;
        return false;
    }
    source->width = width;
    source->height = height;
    return true;
}

/* A source that is also the target is read from a copy, as canvas.js does
   for a script buffer. False when the copy cannot be made. */
static bool canvas_source_unalias(JSContext *context, CanvasSource *source,
                                  const uint8_t *target, uint8_t **copy)
{
    *copy = NULL;
    if (source->pixels == NULL || source->pixels != target) return true;
    size_t length = (size_t) source->width * (size_t) source->height * 4u;
    *copy = js_malloc(context, length);
    if (*copy == NULL) return false;
    memcpy(*copy, source->pixels, length);
    source->pixels = *copy;
    return true;
}

JSValue js_canvas_commit_surface(JSContext *context,
                                 JSValueConst this_value,
                                 int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || bridge->images == NULL || bridge->budget == NULL
        || argc < 8) return JS_FALSE;
    int64_t handle = 0;
    int32_t width = 0, height = 0;
    int32_t left = 0, top = 0, right = 0, bottom = 0;
    if (JS_ToInt64(context, &handle, argv[0]) < 0
        || JS_ToInt32(context, &width, argv[1]) < 0
        || JS_ToInt32(context, &height, argv[2]) < 0
        || JS_ToInt32(context, &left, argv[4]) < 0
        || JS_ToInt32(context, &top, argv[5]) < 0
        || JS_ToInt32(context, &right, argv[6]) < 0
        || JS_ToInt32(context, &bottom, argv[7]) < 0) {
        return JS_EXCEPTION;
    }
    if (width <= 0 || height <= 0
        || (size_t) width > SIZE_MAX / (size_t) height
        || (size_t) width * (size_t) height > SIZE_MAX / 4u) {
        return JS_FALSE;
    }
    size_t required = (size_t) width * (size_t) height * 4u;
    /* A tainted canvas commits its native store and passes null here. */
    JSValue buffer = JS_UNDEFINED;
    const uint8_t *bytes = NULL;
    size_t length = 0;
    bool valid = false;
    if (!JS_IsNull(argv[3]) && !JS_IsUndefined(argv[3])) {
        size_t offset = 0, bytes_per_element = 0;
        buffer = JS_GetTypedArrayBuffer(
            context, argv[3], &offset, &length, &bytes_per_element);
        if (JS_IsException(buffer)) return buffer;
        size_t buffer_length = 0;
        bytes = JS_GetArrayBuffer(context, &buffer_length, buffer);
        valid = bytes_per_element == 1u
            && (bytes != NULL || required == 0u)
            && offset <= buffer_length
            && length <= buffer_length - offset
            && length >= required;
        if (valid) bytes += offset;
    }
    /* Every operation above may invoke JavaScript (including valueOf).
       Resolve the generation-bearing handle only after those coercions,
       immediately before the native commit. */
    size_t slot = 0;
    lxb_dom_node_t *commit_node = NULL;
    if (js_rt_bridge_node_slot_for_handle(bridge, handle, &slot))
        commit_node = bridge->nodes[slot];
    if (commit_node == NULL || !bridge_node_is_connected(commit_node)) {
        JS_FreeValue(context, buffer);
        return JS_FALSE;
    }
    /* Decided here, not by the caller: a tainted canvas shows its store. */
    const DomBridgeCanvasStore *store =
        canvas_store_for_node(bridge, commit_node);
    if (store != NULL) {
        valid = store->pixels != NULL && store->width == width
            && store->height == height;
        bytes = store->pixels;
        length = required;
    }
    ImageCanvasCommitResult committed = IMAGE_CANVAS_COMMIT_REFUSED;
    if (valid) {
        committed = images_commit_canvas_surface(
            bridge->images, bridge->budget, commit_node,
            bytes, length, width, height, left, top, right, bottom);
    }
    JS_FreeValue(context, buffer);
    if (committed == IMAGE_CANVAS_COMMIT_REFUSED) return JS_FALSE;
    js_rt_bridge_note_canvas_mutation(
        bridge, commit_node, committed == IMAGE_CANVAS_COMMIT_UPDATED);
    return JS_TRUE;
}

/* Bootstrap-only: (handle, width, height, pixels) taints a canvas and gives
   it a native store of width x height: the canvas's script pixels (a typed
   array of that size, copied once) when it has none yet, else zeroes. A
   tainted canvas keeps a store of that size as it is, and width or height 0
   frees its store (the bitmap was reset). False, with nothing changed, when
   the registry is full or the store cannot be allocated; a canvas that was
   not tainted then stays clean, and the caller must not draw unclean pixels
   into it. */
JSValue js_canvas_native_surface(JSContext *context, JSValueConst this_value,
                                 int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    int32_t width = 0, height = 0;
    if (bridge == NULL || argc < 3) return JS_FALSE;
    if (!JS_IsNumber(argv[0])
        || JS_ToInt32(context, &width, argv[1]) < 0
        || JS_ToInt32(context, &height, argv[2]) < 0) return JS_FALSE;
    bool sized = width > 0 && height > 0;
    size_t required = 0;
    if (sized && !canvas_dimensions(width, height, &required))
        return JS_FALSE;
    const uint8_t *copy = NULL;
    size_t count = 0;
    JSValue buffer = JS_UNDEFINED;
    if (sized && argc >= 4 && JS_IsObject(argv[3])
        && (!canvas_typed_bytes(context, argv[3], 1u, &copy, &count,
                                &buffer) || count < required)) {
        JS_FreeValue(context, buffer);
        return JS_FALSE;
    }
    lxb_dom_node_t *node = canvas_handle_node(context, bridge, argv[0]);
    DomBridgeCanvasStore *store = canvas_store_for_node(bridge, node);
    bool ok = node != NULL;
    if (ok && store == NULL) {
        size_t canvases = bridge->canvas_taint_count
            - canvas_store_bitmap_count(bridge);
        uint8_t *pixels = sized
            ? canvas_store_allocate(bridge, width, height, copy) : NULL;
        ok = canvases < DOM_BRIDGE_CANVAS_TAINT_LIMIT
            && (!sized || pixels != NULL)
            && (store = canvas_store_append(bridge)) != NULL;
        if (ok) {
            store->node = node;
            store->pixels = pixels;
            store->width = sized ? width : 0;
            store->height = sized ? height : 0;
        } else if (pixels != NULL) {
            js_free_rt(canvas_store_runtime(bridge), pixels);
        }
    } else if (ok && !sized) {
        canvas_store_drop_pixels(bridge, store);
    } else if (ok && (store->pixels == NULL || store->width != width
                      || store->height != height)) {
        canvas_store_drop_pixels(bridge, store);
        store->pixels = canvas_store_allocate(bridge, width, height, copy);
        ok = store->pixels != NULL;
        if (ok) {
            store->width = width;
            store->height = height;
        }
    }
    JS_FreeValue(context, buffer);
    return JS_NewBool(context, ok);
}

/* Bootstrap-only: (width, height) creates a zeroed ImageBitmap store and
   answers its id (negative, as raster natives take it) or 0; (id) releases
   it. */
JSValue js_canvas_bitmap_surface(JSContext *context, JSValueConst this_value,
                                 int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || argc < 1) return JS_NewInt32(context, 0);
    if (argc < 2 || JS_IsUndefined(argv[1])) {
        DomBridgeCanvasStore *store = JS_IsNumber(argv[0])
            ? canvas_store_for_value(bridge, context, argv[0], NULL) : NULL;
        if (store != NULL && store->node == NULL)
            canvas_store_remove(bridge, store);
        return JS_NewInt32(context, store != NULL);
    }
    int32_t width = 0, height = 0;
    if (JS_ToInt32(context, &width, argv[0]) < 0
        || JS_ToInt32(context, &height, argv[1]) < 0
        || canvas_store_bitmap_count(bridge) >= DOM_BRIDGE_CANVAS_BITMAP_LIMIT)
        return JS_NewInt32(context, 0);
    uint8_t *pixels = canvas_store_allocate(bridge, width, height, NULL);
    DomBridgeCanvasStore *store = pixels == NULL ? NULL
        : canvas_store_append(bridge);
    if (store == NULL) {
        if (pixels != NULL) js_free_rt(canvas_store_runtime(bridge), pixels);
        return JS_NewInt32(context, 0);
    }
    /* Ids are never reused while their store lives: the next id past any
       live one (at most DOM_BRIDGE_CANVAS_BITMAP_LIMIT steps). */
    int32_t id = bridge->canvas_bitmap_next_id;
    for (size_t attempt = 0; attempt <= DOM_BRIDGE_CANVAS_BITMAP_LIMIT;
         attempt++) {
        id = id >= INT32_MAX || id < 1 ? 1 : id + 1;
        bool used = false;
        for (size_t i = 0; i < bridge->canvas_taint_count; i++)
            used = used || bridge->canvas_taints[i].bitmap_id == id;
        if (!used) break;
    }
    bridge->canvas_bitmap_next_id = id;
    store->bitmap_id = id;
    store->pixels = pixels;
    store->width = width;
    store->height = height;
    return JS_NewInt32(context, -id);
}

/* Bootstrap-only: false once the canvas drew pixels that are not
   origin-clean. */
JSValue js_canvas_origin_clean(JSContext *context, JSValueConst this_value,
                               int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || argc < 1) return JS_TRUE;
    return JS_NewBool(context, !js_rt_bridge_canvas_tainted(
        bridge, canvas_handle_node(context, bridge, argv[0])));
}

/* Bootstrap-only: zlib's Adler-32 (first argument true) or CRC-32 of a
   byte array, continued from the second argument. canvas.js encodes PNG
   with these (the chunk CRCs, the zlib stream's checksum) through the
   tables already linked for ZIP installs and CompressionStream; the
   interpreted bit-at-a-time CRC cost 8 steps per byte. */
JSValue js_canvas_png_checksum(JSContext *context, JSValueConst this_value,
                               int argc, JSValueConst *argv)
{
    (void) this_value;
    uint32_t seed = 0;
    if (argc < 3 || JS_ToUint32(context, &seed, argv[1]) < 0)
        return JS_EXCEPTION;
    bool adler = JS_ToBool(context, argv[0]) > 0;
    const uint8_t *bytes = NULL;
    size_t count = 0;
    JSValue buffer = JS_UNDEFINED;
    if (!canvas_typed_bytes(context, argv[2], 1, &bytes, &count, &buffer)) {
        if (JS_HasException(context)) return JS_EXCEPTION;
        return JS_ThrowTypeError(context, "checksum input must be bytes");
    }
    uLong value = seed;
    /* zlib takes unsigned int lengths: feed it bounded parts. */
    while (count > 0) {
        uInt part = (uInt) (count < ((size_t) 1 << 30)
                                ? count : ((size_t) 1 << 30));
        value = adler ? adler32(value, bytes, part)
                      : crc32(value, bytes, part);
        bytes += part;
        count -= part;
    }
    JS_FreeValue(context, buffer);
    return JS_NewUint32(context, (uint32_t) value);
}

/* {width, height, sameOrigin, pixels} for a decoded image or a committed
   canvas surface. Pixels that are not origin-clean (a no-CORS cross-origin
   image, or a tainted canvas) never reach script: for those the result has
   no pixels, and canvas.js draws them by handle into a native store. */
JSValue js_canvas_image_source(JSContext *context,
                               JSValueConst this_value,
                               int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || argc < 1) return JS_NULL;
    lxb_dom_node_t *node = js_rt_bridge_node_arg(
        context, bridge, argv[0]);
    const ImageResource *image = canvas_image_resolve(bridge, node);
    if (image == NULL) return JS_NULL;
    bool same_origin = !image->cross_origin
        && !js_rt_bridge_canvas_tainted(bridge, node);
    /* Copy the pixels and read every field before any JavaScript-visible
       object exists: `image` points into a table page code can grow, and
       the surface it names can be resized. The fields are defined, not set,
       so no Object.prototype accessor runs (or can intercept a field)
       while the result is built. */
    int width = image->width, height = image->height;
    JSValue pixels = same_origin
        ? JS_NewArrayBufferCopy(context, image->pixels,
                                (size_t) width * (size_t) height * 4u)
        : JS_UNDEFINED;
    if (JS_IsException(pixels)) return pixels;
    JSValue result = JS_NewObject(context);
    if (JS_IsException(result)) {
        JS_FreeValue(context, pixels);
        return result;
    }
    /* JS_DefinePropertyValueStr consumes the value on success and failure. */
    if (JS_DefinePropertyValueStr(context, result, "width",
                                  JS_NewInt32(context, width),
                                  JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(context, result, "height",
                                     JS_NewInt32(context, height),
                                     JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(context, result, "sameOrigin",
                                     JS_NewBool(context, same_origin),
                                     JS_PROP_C_W_E) < 0) {
        JS_FreeValue(context, pixels);
        JS_FreeValue(context, result);
        return JS_EXCEPTION;
    }
    if (same_origin
        && JS_DefinePropertyValueStr(context, result, "pixels", pixels,
                                     JS_PROP_C_W_E) < 0) {
        JS_FreeValue(context, result);
        return JS_EXCEPTION;
    }
    return result;
}

JSValue js_canvas_raster_rect(JSContext *context,
                              JSValueConst this_value,
                              int argc, JSValueConst *argv)
{
    (void) this_value;
    int32_t width = 0, height = 0;
    int32_t left = 0, top = 0, right = 0, bottom = 0;
    int32_t red = 0, green = 0, blue = 0, alpha = 0, operation = 0;
    double global_alpha = 0.0;
    if (argc < 13
        || JS_ToInt32(context, &width, argv[1]) < 0
        || JS_ToInt32(context, &height, argv[2]) < 0
        || JS_ToInt32(context, &left, argv[3]) < 0
        || JS_ToInt32(context, &top, argv[4]) < 0
        || JS_ToInt32(context, &right, argv[5]) < 0
        || JS_ToInt32(context, &bottom, argv[6]) < 0
        || JS_ToInt32(context, &red, argv[7]) < 0
        || JS_ToInt32(context, &green, argv[8]) < 0
        || JS_ToInt32(context, &blue, argv[9]) < 0
        || JS_ToInt32(context, &alpha, argv[10]) < 0
        || JS_ToFloat64(context, &global_alpha, argv[11]) < 0
        || JS_ToInt32(context, &operation, argv[12]) < 0) {
        return JS_EXCEPTION;
    }
    if (width <= 0 || height <= 0 || left < 0 || top < 0
        || right < left || bottom < top || right > width || bottom > height
        || red < 0 || red > 255 || green < 0 || green > 255
        || blue < 0 || blue > 255 || alpha < 0 || alpha > 255
        || global_alpha < 0.0 || global_alpha > 1.0
        || operation < 0 || operation > 11
        || (size_t) width > SIZE_MAX / (size_t) height
        || (size_t) width * (size_t) height > SIZE_MAX / 4u) {
        return JS_FALSE;
    }
    size_t required = (size_t) width * (size_t) height * 4u;
    if (required > CANVAS_BRIDGE_PIXEL_BYTE_LIMIT) return JS_FALSE;
    /* Optional clip paths (serializedClips): a tainted canvas's clipped
       fillRect, which a clean canvas blends in script. */
    const uint8_t *clip_paths = NULL;
    size_t clip_path_count = 0;
    JSValue clip_path_buffer = JS_UNDEFINED;
    if (argc >= 14 && !JS_IsUndefined(argv[13])
        && (!canvas_typed_bytes(context, argv[13], sizeof(double),
                                &clip_paths, &clip_path_count,
                                &clip_path_buffer)
            || clip_path_count > 8201u || operation == 0)) {
        JS_FreeValue(context, clip_path_buffer);
        return JS_FALSE;
    }
    uint8_t *pixels = NULL;
    JSValue buffer = JS_UNDEFINED;
    bool native = false;
    if (!canvas_mutable_pixels(context, argv[0], width, height, required,
                               &pixels, &buffer, &native)) {
        JS_FreeValue(context, clip_path_buffer);
        if (JS_IsException(buffer)) return buffer;
        return JS_FALSE;
    }

    /* Operation 0 is the private clear command. Public Porter-Duff operation
       codes begin at 1 and are kept compact by the JavaScript facade. */
    if (operation == 0) {
        size_t row_bytes = (size_t) (right - left) * 4u;
        for (int32_t y = top; y < bottom; y++) {
            memset(pixels + ((size_t) y * (size_t) width
                             + (size_t) left) * 4u,
                   0, row_bytes);
        }
        JS_FreeValue(context, buffer);
        return JS_TRUE;
    }

    double source_alpha = ((double) alpha / 255.0) * global_alpha;
    if (operation == 1 && source_alpha <= 0.0) {
        JS_FreeValue(context, clip_path_buffer);
        JS_FreeValue(context, buffer);
        return JS_TRUE;
    }
    if (clip_paths != NULL) {
        size_t work_remaining = CANVAS_RASTER_WORK_LIMIT;
        size_t sample_cost = clip_path_count / 2u + 1u;
        bool work_exhausted = false;
        for (int32_t y = top; y < bottom && !work_exhausted; y++) {
            for (int32_t x = left; x < right; x++) {
                if (!canvas_work_take(&work_remaining, sample_cost)) {
                    work_exhausted = true;
                    break;
                }
                if (!canvas_clip_stack_contains(
                        clip_paths, clip_path_count, x + 0.5, y + 0.5))
                    continue;
                canvas_blend_pixel(
                    pixels, ((size_t) y * (size_t) width + (size_t) x) * 4u,
                    red, green, blue, alpha, global_alpha, operation, 1.0);
            }
        }
        JS_FreeValue(context, clip_path_buffer);
        JS_FreeValue(context, buffer);
        return JS_NewInt32(context, work_exhausted ? 2 : 1);
    }
    if (operation == 1 && source_alpha >= 1.0) {
        for (int32_t y = top; y < bottom; y++) {
            uint8_t *row = pixels + ((size_t) y * (size_t) width
                                     + (size_t) left) * 4u;
            for (int32_t x = left; x < right; x++, row += 4u) {
                row[0] = (uint8_t) red;
                row[1] = (uint8_t) green;
                row[2] = (uint8_t) blue;
                row[3] = 255u;
            }
        }
        JS_FreeValue(context, buffer);
        return JS_TRUE;
    }
    for (int32_t y = top; y < bottom; y++) {
        for (int32_t x = left; x < right; x++) {
            size_t at = ((size_t) y * (size_t) width + (size_t) x) * 4u;
            double destination_alpha = (double) pixels[at + 3u] / 255.0;
            double source_factor = 1.0;
            double destination_factor = 1.0 - source_alpha;
            switch (operation) {
                case 2: /* copy */
                    destination_factor = 0.0;
                    break;
                case 3: /* destination-over */
                    source_factor = 1.0 - destination_alpha;
                    destination_factor = 1.0;
                    break;
                case 4: /* source-in */
                    source_factor = destination_alpha;
                    destination_factor = 0.0;
                    break;
                case 5: /* source-out */
                    source_factor = 1.0 - destination_alpha;
                    destination_factor = 0.0;
                    break;
                case 6: /* source-atop */
                    source_factor = destination_alpha;
                    destination_factor = 1.0 - source_alpha;
                    break;
                case 7: /* destination-in */
                    source_factor = 0.0;
                    destination_factor = source_alpha;
                    break;
                case 8: /* destination-out */
                    source_factor = 0.0;
                    destination_factor = 1.0 - source_alpha;
                    break;
                case 9: /* destination-atop */
                    source_factor = 1.0 - destination_alpha;
                    destination_factor = source_alpha;
                    break;
                case 10: /* xor */
                    source_factor = 1.0 - destination_alpha;
                    destination_factor = 1.0 - source_alpha;
                    break;
                case 11: /* lighter */
                    destination_factor = 1.0;
                    break;
                default: /* source-over */
                    break;
            }
            double output_alpha = source_alpha * source_factor
                + destination_alpha * destination_factor;
            if (output_alpha > 1.0) output_alpha = 1.0;
            if (output_alpha <= 0.0) {
                memset(pixels + at, 0, 4u);
                continue;
            }
            pixels[at] = canvas_byte(
                ((double) red * source_alpha * source_factor
                 + (double) pixels[at] * destination_alpha
                       * destination_factor) / output_alpha);
            pixels[at + 1u] = canvas_byte(
                ((double) green * source_alpha * source_factor
                 + (double) pixels[at + 1u] * destination_alpha
                       * destination_factor) / output_alpha);
            pixels[at + 2u] = canvas_byte(
                ((double) blue * source_alpha * source_factor
                 + (double) pixels[at + 2u] * destination_alpha
                       * destination_factor) / output_alpha);
            pixels[at + 3u] = canvas_byte(output_alpha * 255.0);
        }
    }
    JS_FreeValue(context, buffer);
    return JS_TRUE;
}

JSValue js_canvas_raster_rect_batch(JSContext *context,
                                    JSValueConst this_value,
                                    int argc, JSValueConst *argv)
{
    (void) this_value;
    int32_t width = 0, height = 0;
    size_t required = 0;
    if (argc < 4 || JS_ToInt32(context, &width, argv[1]) < 0
        || JS_ToInt32(context, &height, argv[2]) < 0) return JS_EXCEPTION;
    if (!canvas_dimensions(width, height, &required)) return JS_FALSE;
    uint8_t *pixels = NULL;
    JSValue pixel_buffer = JS_UNDEFINED;
    bool native = false;
    if (!canvas_mutable_pixels(context, argv[0], width, height, required,
                               &pixels, &pixel_buffer, &native)) {
        if (JS_IsException(pixel_buffer)) return pixel_buffer;
        return JS_FALSE;
    }
    const uint8_t *commands = NULL;
    size_t count = 0;
    JSValue command_buffer = JS_UNDEFINED;
    if (!canvas_typed_bytes(context, argv[3], sizeof(double),
                            &commands, &count, &command_buffer)
        || count % 10u != 0u || count > 640u) {
        JS_FreeValue(context, command_buffer);
        JS_FreeValue(context, pixel_buffer);
        return JS_FALSE;
    }
    bool rendered = true;
    /* One JavaScript-to-native batch is one browser-thread call.  Share the
       allowance across every command and degrade the unpainted tail instead
       of multiplying the watchdog bound by the batch length. */
    bool work_exhausted = false;
    size_t work_remaining = CANVAS_RASTER_WORK_LIMIT;
    for (size_t command = 0; command < count / 10u; command++) {
        size_t base = command * 10u;
        int left = 0, top = 0, right = 0, bottom = 0;
        int red = 0, green = 0, blue = 0, alpha = 0, operation = 0;
        double global_alpha = canvas_double_at(commands, base + 8u);
        bool integers_ok = canvas_integer(
                canvas_double_at(commands, base), CANVAS_INTEGER_FLOOR, &left)
            && canvas_integer(canvas_double_at(commands, base + 1u),
                              CANVAS_INTEGER_FLOOR, &top)
            && canvas_integer(canvas_double_at(commands, base + 2u),
                              CANVAS_INTEGER_CEIL, &right)
            && canvas_integer(canvas_double_at(commands, base + 3u),
                              CANVAS_INTEGER_CEIL, &bottom)
            && canvas_integer(canvas_double_at(commands, base + 4u),
                              CANVAS_INTEGER_TRUNCATE, &red)
            && canvas_integer(canvas_double_at(commands, base + 5u),
                              CANVAS_INTEGER_TRUNCATE, &green)
            && canvas_integer(canvas_double_at(commands, base + 6u),
                              CANVAS_INTEGER_TRUNCATE, &blue)
            && canvas_integer(canvas_double_at(commands, base + 7u),
                              CANVAS_INTEGER_TRUNCATE, &alpha)
            && canvas_integer(canvas_double_at(commands, base + 9u),
                              CANVAS_INTEGER_TRUNCATE, &operation);
        if (!integers_ok) {
            rendered = false;
            break;
        }
        if (left < 0) left = 0;
        if (top < 0) top = 0;
        if (right > width) right = width;
        if (bottom > height) bottom = height;
        if (right <= left || bottom <= top || red < 0 || red > 255
            || green < 0 || green > 255 || blue < 0 || blue > 255
            || alpha < 0 || alpha > 255 || global_alpha < 0.0
            || global_alpha > 1.0 || operation < 1 || operation > 11) {
            continue;
        }
        size_t rect_width = (size_t) (right - left);
        size_t rect_height = (size_t) (bottom - top);
        if (rect_width > SIZE_MAX / rect_height
            || !canvas_work_take(
                &work_remaining, rect_width * rect_height)) {
            /* Never leave a valid rectangle half-painted. Status 2 tells the
               facade that the untouched tail was clipped by the call bound. */
            work_exhausted = true;
            break;
        }
        bool opaque = (operation == 1 || operation == 2) && alpha == 255
            && global_alpha == 1.0;
        for (int y = top; y < bottom; y++) {
            size_t row = ((size_t) y * (size_t) width + (size_t) left) * 4u;
            if (opaque) {
                uint8_t *at = pixels + row;
                for (int x = left; x < right; x++, at += 4) {
                    at[0] = (uint8_t) red;
                    at[1] = (uint8_t) green;
                    at[2] = (uint8_t) blue;
                    at[3] = 255u;
                }
                continue;
            }
            for (int x = left; x < right; x++, row += 4u)
                canvas_blend_pixel(pixels, row, red, green, blue, alpha,
                                   global_alpha, operation, 1.0);
        }
    }
    JS_FreeValue(context, command_buffer);
    JS_FreeValue(context, pixel_buffer);
    if (!rendered) return JS_FALSE;
    return JS_NewInt32(context, work_exhausted ? 2 : 1);
}

JSValue js_canvas_measure_text(JSContext *context,
                               JSValueConst this_value,
                               int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    int32_t pixel_height = 0, family = 0;
    if (bridge == NULL || argc < 5
        || JS_ToInt32(context, &pixel_height, argv[1]) < 0
        || JS_ToInt32(context, &family, argv[2]) < 0) return JS_NULL;
    bool bold = JS_ToBool(context, argv[3]) > 0;
    bool italic = JS_ToBool(context, argv[4]) > 0;
    if (pixel_height < 1 || pixel_height > TILEFINCH_FONT_RASTER_PIXEL_LIMIT
        || family < 0 || family > 2) return JS_NULL;
    size_t length = 0;
    const char *text = JS_ToCStringLen(context, &length, argv[0]);
    if (text == NULL) return JS_EXCEPTION;
    if (length > 1024u) length = 1024u;
    const FontFace *face = canvas_font_face(
        bridge, family, italic, bold);
    if (face == NULL) {
        JS_FreeCString(context, text);
        return JS_NULL;
    }
    int width_fixed = font_text_width_fixed(
        face, text, length, pixel_height, bold);
    int ascent = font_ascent(face, pixel_height);
    int metric_height = font_metric_height(face, pixel_height);
    int descent = metric_height > ascent ? metric_height - ascent : 0;
    JS_FreeCString(context, text);
    JSValue result = JS_NewObject(context);
    if (JS_IsException(result)) return result;
    if (JS_DefinePropertyValueStr(context, result, "width",
                                  JS_NewFloat64(context,
                                               (double) width_fixed / 64.0),
                                  JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(context, result, "ascent",
                                     JS_NewFloat64(context, (double) ascent),
                                     JS_PROP_C_W_E) < 0
        || JS_DefinePropertyValueStr(context, result, "descent",
                                     JS_NewFloat64(context,
                                                   (double) descent),
                                     JS_PROP_C_W_E) < 0) {
        JS_FreeValue(context, result);
        return JS_EXCEPTION;
    }
    return result;
}

static bool canvas_read_transform_clip(JSContext *context,
                                       JSValueConst transform_value,
                                       JSValueConst clip_value,
                                       double transform[6],
                                       int clip[4], int width, int height,
                                       JSValue *transform_buffer,
                                       JSValue *clip_buffer)
{
    const uint8_t *bytes = NULL;
    size_t count = 0;
    if (!canvas_typed_bytes(context, transform_value, sizeof(double),
                            &bytes, &count, transform_buffer)
        || count < 6u) return false;
    for (size_t i = 0; i < 6u; i++) {
        transform[i] = canvas_double_at(bytes, i);
        if (!isfinite(transform[i])) {
            JS_FreeValue(context, *transform_buffer);
            *transform_buffer = JS_UNDEFINED;
            return false;
        }
    }
    bytes = NULL;
    count = 0;
    if (!canvas_typed_bytes(context, clip_value, sizeof(double),
                            &bytes, &count, clip_buffer)
        || count < 4u) {
        JS_FreeValue(context, *transform_buffer);
        *transform_buffer = JS_UNDEFINED;
        return false;
    }
    for (size_t i = 0; i < 4u; i++) {
        double value = canvas_double_at(bytes, i);
        if (!canvas_integer(value, i < 2u ? CANVAS_INTEGER_FLOOR
                                          : CANVAS_INTEGER_CEIL,
                            &clip[i])) {
            JS_FreeValue(context, *clip_buffer);
            *clip_buffer = JS_UNDEFINED;
            JS_FreeValue(context, *transform_buffer);
            *transform_buffer = JS_UNDEFINED;
            return false;
        }
    }
    if (clip[0] < 0) clip[0] = 0;
    if (clip[1] < 0) clip[1] = 0;
    if (clip[2] > width) clip[2] = width;
    if (clip[3] > height) clip[3] = height;
    return clip[2] > clip[0] && clip[3] > clip[1];
}

/* A CanvasPattern's pixels as a raster native reads them. canvas.js samples
   an origin-clean pattern itself; one that is not origin-clean (or any
   pattern painted into a tainted canvas) is sampled here. */
typedef struct {
    CanvasSource source;
    uint8_t *copy;
    double e, f;
    bool repeat_x, repeat_y;
} CanvasPatternSource;

/* `params` is a Float64Array [e, f, repeatX, repeatY, sourceWidth,
   sourceHeight, ...]; values past those six go to `extra`. A null source is
   a pattern without pixels (transparent). A source named by a number may
   fill a native target only. Runs no script. */
static bool canvas_pattern_open(JSContext *context, JSValueConst source_value,
                                JSValueConst params_value,
                                const uint8_t *target, bool target_native,
                                CanvasPatternSource *pattern,
                                double *extra, size_t extra_count)
{
    memset(pattern, 0, sizeof(*pattern));
    pattern->source.buffer = JS_UNDEFINED;
    const uint8_t *bytes = NULL;
    size_t count = 0;
    JSValue params_buffer = JS_UNDEFINED;
    if (!canvas_typed_bytes(context, params_value, sizeof(double), &bytes,
                            &count, &params_buffer)
        || count < 6u + extra_count) {
        JS_FreeValue(context, params_buffer);
        return false;
    }
    double values[6];
    for (size_t i = 0; i < 6u; i++) values[i] = canvas_double_at(bytes, i);
    for (size_t i = 0; i < extra_count; i++)
        extra[i] = canvas_double_at(bytes, 6u + i);
    JS_FreeValue(context, params_buffer);
    int source_width = 0, source_height = 0;
    if (!isfinite(values[0]) || !isfinite(values[1])
        || !canvas_integer(values[4], CANVAS_INTEGER_TRUNCATE, &source_width)
        || !canvas_integer(values[5], CANVAS_INTEGER_TRUNCATE,
                           &source_height)) return false;
    pattern->e = values[0];
    pattern->f = values[1];
    pattern->repeat_x = values[2] != 0.0;
    pattern->repeat_y = values[3] != 0.0;
    if (JS_IsNull(source_value) || JS_IsUndefined(source_value)) return true;
    if (!canvas_source_pixels(context, source_value, source_width,
                              source_height, &pattern->source)
        || (pattern->source.native && !target_native)
        || !canvas_source_unalias(context, &pattern->source, target,
                                  &pattern->copy)) {
        JS_FreeValue(context, pattern->source.buffer);
        pattern->source.buffer = JS_UNDEFINED;
        js_free(context, pattern->copy);
        pattern->copy = NULL;
        return false;
    }
    return true;
}

static void canvas_pattern_close(JSContext *context,
                                 CanvasPatternSource *pattern)
{
    JS_FreeValue(context, pattern->source.buffer);
    pattern->source.buffer = JS_UNDEFINED;
    js_free(context, pattern->copy);
    pattern->copy = NULL;
}

/* canvas.js's paintColorAt: the source pixel under (x, y) translated by the
   pattern transform, repeated as asked, else transparent. */
static void canvas_pattern_color(const CanvasPatternSource *pattern,
                                 double x, double y, int output[4])
{
    memset(output, 0, 4u * sizeof(*output));
    const CanvasSource *source = &pattern->source;
    if (source->pixels == NULL || source->width <= 0 || source->height <= 0)
        return;
    double width = source->width, height = source->height;
    double px = floor(x - pattern->e), py = floor(y - pattern->f);
    if (!isfinite(px) || !isfinite(py)) return;
    if (!pattern->repeat_x && (px < 0.0 || px >= width)) return;
    if (!pattern->repeat_y && (py < 0.0 || py >= height)) return;
    px = fmod(fmod(px, width) + width, width);
    py = fmod(fmod(py, height) + height, height);
    if (!(px >= 0.0 && px < width && py >= 0.0 && py < height)) return;
    size_t at = ((size_t) py * (size_t) source->width + (size_t) px) * 4u;
    for (size_t c = 0; c < 4u; c++) output[c] = source->pixels[at + c];
}

static void canvas_transform_point(const double transform[6],
                                   double x, double y,
                                   double *output_x, double *output_y)
{
    *output_x = transform[0] * x + transform[2] * y + transform[4];
    *output_y = transform[1] * x + transform[3] * y + transform[5];
}

static JSValue canvas_raster_text_with_work(
    JSContext *context, JSValueConst this_value,
    int argc, JSValueConst *argv, size_t *work_remaining,
    bool *clipped_out)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    int32_t width = 0, height = 0, pixel_height = 0, family = 0;
    int32_t red = 0, green = 0, blue = 0, alpha = 0, operation = 0;
    double x = 0.0, baseline = 0.0, global_alpha = 0.0, line_width = 0.0;
    double horizontal_scale = 1.0;
    if (bridge == NULL || argc < 22
        || JS_ToInt32(context, &width, argv[1]) < 0
        || JS_ToInt32(context, &height, argv[2]) < 0
        || JS_ToFloat64(context, &x, argv[4]) < 0
        || JS_ToFloat64(context, &baseline, argv[5]) < 0
        || JS_ToInt32(context, &pixel_height, argv[6]) < 0
        || JS_ToInt32(context, &family, argv[7]) < 0
        || JS_ToInt32(context, &red, argv[10]) < 0
        || JS_ToInt32(context, &green, argv[11]) < 0
        || JS_ToInt32(context, &blue, argv[12]) < 0
        || JS_ToInt32(context, &alpha, argv[13]) < 0
        || JS_ToFloat64(context, &global_alpha, argv[14]) < 0
        || JS_ToInt32(context, &operation, argv[15]) < 0
        || JS_ToFloat64(context, &line_width, argv[17]) < 0
        || JS_ToFloat64(context, &horizontal_scale, argv[20]) < 0) {
        return JS_EXCEPTION;
    }
    size_t required = 0;
    if (!canvas_dimensions(width, height, &required)
        || pixel_height < 1 || pixel_height > TILEFINCH_FONT_RASTER_PIXEL_LIMIT
        || family < 0 || family > 2 || !isfinite(x) || !isfinite(baseline)
        || !isfinite(horizontal_scale) || horizontal_scale <= 0.0
        || horizontal_scale > 1.0
        || red < 0 || red > 255 || green < 0 || green > 255
        || blue < 0 || blue > 255 || alpha < 0 || alpha > 255
        || global_alpha < 0.0 || global_alpha > 1.0
        || operation < 1 || operation > 11) return JS_FALSE;
    bool bold = JS_ToBool(context, argv[8]) > 0;
    bool italic = JS_ToBool(context, argv[9]) > 0;
    bool stroke = JS_ToBool(context, argv[16]) > 0;
    /* A string coercion can run author toString(), and that code can detach
       or resize every buffer read below. Finish it before resolving any raw
       pointer; nothing after this point calls back into JavaScript. */
    size_t text_length = 0;
    const char *text = JS_ToCStringLen(context, &text_length, argv[3]);
    if (text == NULL) return JS_EXCEPTION;
    if (text_length > 1024u) text_length = 1024u;
    const FontFace *face = canvas_font_face(bridge, family, italic, bold);
    uint8_t *pixels = NULL;
    JSValue pixel_buffer = JS_UNDEFINED;
    bool native = false;
    if (face == NULL || !canvas_mutable_pixels(
            context, argv[0], width, height, required, &pixels,
            &pixel_buffer, &native)) {
        JS_FreeCString(context, text);
        if (JS_IsException(pixel_buffer)) return pixel_buffer;
        return JS_FALSE;
    }
    if (argc >= 24) {
        /* Text filled with a pattern canvas.js may not sample: the colour is
           the pattern's pixel at the given point, as canvas.js samples a
           clean one, read here so it never reaches script. */
        CanvasPatternSource pattern;
        double at[2] = {0.0, 0.0};
        if (!canvas_pattern_open(context, argv[22], argv[23], pixels, native,
                                 &pattern, at, 2u)) {
            JS_FreeCString(context, text);
            JS_FreeValue(context, pixel_buffer);
            return JS_FALSE;
        }
        int color[4];
        canvas_pattern_color(&pattern, at[0], at[1], color);
        canvas_pattern_close(context, &pattern);
        red = color[0];
        green = color[1];
        blue = color[2];
        alpha = color[3];
    }
    double transform[6];
    int clip[4] = {0};
    JSValue transform_buffer = JS_UNDEFINED, clip_buffer = JS_UNDEFINED;
    if (!canvas_read_transform_clip(
            context, argv[18], argv[19], transform, clip, width, height,
            &transform_buffer, &clip_buffer)) {
        JS_FreeCString(context, text);
        JS_FreeValue(context, pixel_buffer);
        if (JS_IsException(transform_buffer)) return transform_buffer;
        if (JS_IsException(clip_buffer)) return clip_buffer;
        return JS_FALSE;
    }
    const uint8_t *clip_paths = NULL;
    size_t clip_path_count = 0;
    JSValue clip_path_buffer = JS_UNDEFINED;
    if (!canvas_typed_bytes(
            context, argv[21], sizeof(double), &clip_paths,
            &clip_path_count, &clip_path_buffer)
        || clip_path_count > 8201u) {
        JS_FreeCString(context, text);
        JS_FreeValue(context, clip_path_buffer);
        JS_FreeValue(context, clip_buffer);
        JS_FreeValue(context, transform_buffer);
        JS_FreeValue(context, pixel_buffer);
        return JS_FALSE;
    }
    double pen = x;
    unsigned previous = 0;
    size_t offset = 0, glyph_count = 0;
    int stroke_radius = 0;
    if (stroke && !canvas_integer(
            fmax(1.0, fmin(16.0, line_width)) / 2.0,
            CANVAS_INTEGER_CEIL, &stroke_radius)) {
        JS_FreeCString(context, text);
        JS_FreeValue(context, clip_path_buffer);
        JS_FreeValue(context, clip_buffer);
        JS_FreeValue(context, transform_buffer);
        JS_FreeValue(context, pixel_buffer);
        return JS_FALSE;
    }
    bool rendered = true;
    bool work_exhausted = false;
    size_t sample_cost = clip_path_count / 2u + 1u;
    if (stroke) {
        size_t diameter = (size_t) (2 * stroke_radius + 1);
        size_t neighborhood = diameter * diameter;
        sample_cost = neighborhood > SIZE_MAX - sample_cost
            ? SIZE_MAX : sample_cost + neighborhood;
    }
    while (offset < text_length && glyph_count++ < 256u) {
        unsigned codepoint = 0;
        size_t used = font_utf8_next(text + offset, text_length - offset,
                                     &codepoint);
        if (used == 0u) break;
        offset += used;
        if (previous != 0u)
            pen += font_kerning(face, previous, codepoint, pixel_height);
        FontGlyph glyph;
        if (!font_glyph_load(face, codepoint, pixel_height, bold, &glyph)) {
            previous = codepoint;
            continue;
        }
        int pen_integer = 0, baseline_integer = 0;
        if (!canvas_integer(pen, CANVAS_INTEGER_FLOOR, &pen_integer)
            || !canvas_integer(
                   baseline, CANVAS_INTEGER_FLOOR, &baseline_integer)
            || (int64_t) pen_integer + glyph.x_offset > INT_MAX
            || (int64_t) pen_integer + glyph.x_offset < INT_MIN
            || (int64_t) baseline_integer + glyph.y_offset > INT_MAX
            || (int64_t) baseline_integer + glyph.y_offset < INT_MIN) {
            font_glyph_destroy(face, &glyph);
            rendered = false;
            break;
        }
        int source_left = pen_integer + glyph.x_offset;
        int source_top = baseline_integer + glyph.y_offset;
        for (int gy = -stroke_radius;
             gy < glyph.height + stroke_radius && !work_exhausted; gy++) {
            for (int gx = -stroke_radius; gx < glyph.width + stroke_radius;
                 gx++) {
                if (!canvas_work_take(work_remaining, sample_cost)) {
                    work_exhausted = true;
                    break;
                }
                unsigned coverage = 0;
                if (!stroke) {
                    if (gx >= 0 && gy >= 0 && gx < glyph.width
                        && gy < glyph.height && glyph.pixels != NULL) {
                        coverage = glyph.pixels[(size_t) gy
                                                * (size_t) glyph.width
                                                + (size_t) gx];
                    }
                } else if (glyph.pixels != NULL) {
                    for (int oy = -stroke_radius; oy <= stroke_radius; oy++) {
                        int sy = gy + oy;
                        if (sy < 0 || sy >= glyph.height) continue;
                        for (int ox = -stroke_radius; ox <= stroke_radius; ox++) {
                            if (ox * ox + oy * oy
                                    > stroke_radius * stroke_radius) continue;
                            int sx = gx + ox;
                            if (sx < 0 || sx >= glyph.width) continue;
                            unsigned sample = glyph.pixels[
                                (size_t) sy * (size_t) glyph.width
                                + (size_t) sx];
                            if (sample > coverage) coverage = sample;
                        }
                    }
                    if (gx >= 0 && gy >= 0 && gx < glyph.width
                        && gy < glyph.height) {
                        unsigned interior = glyph.pixels[
                            (size_t) gy * (size_t) glyph.width + (size_t) gx];
                        coverage = coverage > interior
                            ? coverage - interior : 0u;
                    }
                }
                if (coverage == 0u) continue;
                double tx = 0.0, ty = 0.0;
                canvas_transform_point(
                    transform,
                    x + (source_left + gx + 0.5 - x) * horizontal_scale,
                    source_top + gy + 0.5, &tx, &ty);
                int px = 0, py = 0;
                if (!canvas_integer(tx, CANVAS_INTEGER_FLOOR, &px)
                    || !canvas_integer(ty, CANVAS_INTEGER_FLOOR, &py)) {
                    font_glyph_destroy(face, &glyph);
                    rendered = false;
                    goto canvas_text_finished;
                }
                if (px < clip[0] || py < clip[1]
                    || px >= clip[2] || py >= clip[3]) continue;
                if (!canvas_clip_stack_contains(
                        clip_paths, clip_path_count, tx, ty)) continue;
                size_t at = ((size_t) py * (size_t) width
                             + (size_t) px) * 4u;
                canvas_blend_pixel(pixels, at, red, green, blue, alpha,
                                   global_alpha, operation,
                                   (double) coverage / 255.0);
            }
        }
        pen += glyph.advance;
        previous = codepoint;
        font_glyph_destroy(face, &glyph);
    }
canvas_text_finished:
    JS_FreeCString(context, text);
    JS_FreeValue(context, clip_path_buffer);
    JS_FreeValue(context, clip_buffer);
    JS_FreeValue(context, transform_buffer);
    JS_FreeValue(context, pixel_buffer);
    /* Work exhaustion is a bounded compatibility degradation, not a signal
       to replay the same expensive command through the JavaScript fallback. */
    if (clipped_out != NULL) *clipped_out = work_exhausted;
    if (!rendered) return JS_FALSE;
    return JS_NewInt32(context, work_exhausted ? 2 : 1);
}

static bool canvas_path_inside(const uint8_t *points, size_t point_count,
                               double x, double y, bool even_odd)
{
    int winding = 0;
    bool parity = false;
    size_t start = 0;
    while (start < point_count) {
        while (start < point_count
               && (!isfinite(canvas_double_at(points, start * 2u))
                   || !isfinite(canvas_double_at(points, start * 2u + 1u)))) {
            start++;
        }
        size_t end = start;
        while (end < point_count
               && isfinite(canvas_double_at(points, end * 2u))
               && isfinite(canvas_double_at(points, end * 2u + 1u))) end++;
        if (end - start >= 2u) {
            for (size_t i = start; i < end; i++) {
                size_t j = i + 1u < end ? i + 1u : start;
                double x0 = canvas_double_at(points, i * 2u);
                double y0 = canvas_double_at(points, i * 2u + 1u);
                double x1 = canvas_double_at(points, j * 2u);
                double y1 = canvas_double_at(points, j * 2u + 1u);
                bool crosses = (y0 <= y && y1 > y) || (y1 <= y && y0 > y);
                if (!crosses) continue;
                double crossing = x0 + (y - y0) * (x1 - x0) / (y1 - y0);
                if (crossing <= x) continue;
                parity = !parity;
                if (y1 > y0) winding++; else winding--;
            }
        }
        start = end + 1u;
    }
    return even_odd ? parity : winding != 0;
}

static double canvas_segment_distance_squared(double x, double y,
                                              double x0, double y0,
                                              double x1, double y1,
                                              double *position)
{
    double dx = x1 - x0, dy = y1 - y0;
    double length_squared = dx * dx + dy * dy;
    double raw_t = length_squared <= 1e-12 ? 0.0
        : ((x - x0) * dx + (y - y0) * dy) / length_squared;
    double t = raw_t;
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    if (position != NULL) *position = raw_t;
    double nearest_x = x0 + t * dx, nearest_y = y0 + t * dy;
    double nearest_dx = x - nearest_x;
    double nearest_dy = y - nearest_y;
    return nearest_dx * nearest_dx + nearest_dy * nearest_dy;
}

static bool canvas_dash_on(const uint8_t *dash, size_t count,
                           double offset, double distance)
{
    if (count == 0u) return true;
    double total = 0.0;
    for (size_t i = 0; i < count; i++) total += canvas_double_at(dash, i);
    if (total <= 1e-9) return true;
    double at = fmod(distance + offset, total);
    if (at < 0.0) at += total;
    for (size_t i = 0; i < count; i++) {
        double span = canvas_double_at(dash, i);
        if (at <= span) return (i & 1u) == 0u;
        at -= span;
    }
    return true;
}

static bool canvas_point_in_triangle(double x, double y,
                                     double ax, double ay,
                                     double bx, double by,
                                     double cx, double cy)
{
    double ab = (bx - ax) * (y - ay) - (by - ay) * (x - ax);
    double bc = (cx - bx) * (y - by) - (cy - by) * (x - bx);
    double ca = (ax - cx) * (y - cy) - (ay - cy) * (x - cx);
    return (ab >= 0.0 && bc >= 0.0 && ca >= 0.0)
        || (ab <= 0.0 && bc <= 0.0 && ca <= 0.0);
}

static bool canvas_gradient_color(const uint8_t *gradient,
                                  size_t gradient_count,
                                  double x, double y,
                                  const int fallback[4], int output[4])
{
    memcpy(output, fallback, 4u * sizeof(*output));
    if (gradient == NULL || gradient_count < 8u) return true;
    int kind = 0, stop_count = 0;
    if (!canvas_integer(canvas_double_at(gradient, 0),
                        CANVAS_INTEGER_TRUNCATE, &kind)
        || !canvas_integer(canvas_double_at(gradient, 7),
                           CANVAS_INTEGER_TRUNCATE, &stop_count)) return false;
    if (kind < 1 || kind > 3 || stop_count < 1
        || stop_count > 16
        || 8u + (size_t) stop_count * 5u > gradient_count) return false;
    double offset = 0.0;
    if (kind == 1) {
        double x0 = canvas_double_at(gradient, 1);
        double y0 = canvas_double_at(gradient, 2);
        double dx = canvas_double_at(gradient, 3) - x0;
        double dy = canvas_double_at(gradient, 4) - y0;
        double denominator = dx * dx + dy * dy;
        offset = denominator <= 1e-12 ? 0.0
            : ((x - x0) * dx + (y - y0) * dy) / denominator;
    } else if (kind == 2) {
        double r0 = canvas_double_at(gradient, 3);
        double x1 = canvas_double_at(gradient, 4);
        double y1 = canvas_double_at(gradient, 5);
        double r1 = canvas_double_at(gradient, 6);
        offset = fabs(r1 - r0) <= 1e-12 ? 0.0
            : (hypot(x - x1, y - y1) - r0) / (r1 - r0);
    } else {
        double start = canvas_double_at(gradient, 1);
        double x0 = canvas_double_at(gradient, 2);
        double y0 = canvas_double_at(gradient, 3);
        offset = fmod((atan2(y - y0, x - x0) - start)
                      / 6.28318530717958647692, 1.0);
        if (offset < 0.0) offset += 1.0;
    }
    if (offset < 0.0) offset = 0.0;
    if (offset > 1.0) offset = 1.0;
    size_t first = 8u;
    size_t left = first, right = first;
    for (int i = 0; i < stop_count; i++) {
        size_t at = first + (size_t) i * 5u;
        if (canvas_double_at(gradient, at) <= offset) left = at;
        if (canvas_double_at(gradient, at) >= offset) {
            right = at;
            break;
        }
        right = at;
    }
    double left_offset = canvas_double_at(gradient, left);
    double right_offset = canvas_double_at(gradient, right);
    double ratio = right_offset > left_offset
        ? (offset - left_offset) / (right_offset - left_offset) : 0.0;
    for (size_t channel = 0; channel < 4u; channel++) {
        double a = canvas_double_at(gradient, left + 1u + channel);
        double b = canvas_double_at(gradient, right + 1u + channel);
        if (!canvas_integer(a + (b - a) * ratio,
                            CANVAS_INTEGER_NEAREST, &output[channel])) {
            return false;
        }
    }
    return true;
}

typedef struct {
    double x;
    int winding;
} CanvasPathCrossing;

static int canvas_compare_crossings(const void *left, const void *right)
{
    const CanvasPathCrossing *a = left;
    const CanvasPathCrossing *b = right;
    if (a->x < b->x) return -1;
    if (a->x > b->x) return 1;
    return a->winding - b->winding;
}

static size_t canvas_path_crossings(const uint8_t *points,
                                    size_t point_count, double y,
                                    CanvasPathCrossing *output,
                                    size_t capacity)
{
    size_t count = 0, start = 0;
    while (start < point_count) {
        while (start < point_count
               && (!isfinite(canvas_double_at(points, start * 2u))
                   || !isfinite(canvas_double_at(
                           points, start * 2u + 1u)))) start++;
        size_t end = start;
        while (end < point_count
               && isfinite(canvas_double_at(points, end * 2u))
               && isfinite(canvas_double_at(points, end * 2u + 1u))) end++;
        if (end - start >= 2u) {
            for (size_t i = start; i < end; i++) {
                size_t j = i + 1u < end ? i + 1u : start;
                double x0 = canvas_double_at(points, i * 2u);
                double y0 = canvas_double_at(points, i * 2u + 1u);
                double x1 = canvas_double_at(points, j * 2u);
                double y1 = canvas_double_at(points, j * 2u + 1u);
                if (!((y0 <= y && y1 > y) || (y1 <= y && y0 > y))) continue;
                if (count >= capacity) return capacity + 1u;
                output[count].x = x0 + (y - y0) * (x1 - x0) / (y1 - y0);
                output[count].winding = y1 > y0 ? 1 : -1;
                count++;
            }
        }
        start = end + 1u;
    }
    qsort(output, count, sizeof(*output), canvas_compare_crossings);
    return count;
}

static bool canvas_raster_path_fill(
    JSContext *context, uint8_t *pixels, int width,
    int left, int top, int right, int bottom,
    const uint8_t *points, size_t point_count, bool even_odd,
    const uint8_t *clip_paths, size_t clip_path_count,
    const uint8_t *gradient, size_t gradient_count,
    const int fallback[4], double global_alpha, int operation,
    size_t *work_remaining, bool *clipped_out)
{
    size_t span = (size_t) (right - left);
    if (span == 0u || point_count == 0u
        || point_count > SIZE_MAX / sizeof(CanvasPathCrossing)) return false;
    uint8_t *coverage = js_malloc(context, span);
    CanvasPathCrossing *crossings = js_malloc(
        context, point_count * sizeof(*crossings));
    if (coverage == NULL || crossings == NULL) {
        js_free(context, crossings);
        js_free(context, coverage);
        return false;
    }
    static const double sample[2] = {0.25, 0.75};
    bool valid = true, work_exhausted = false;
    size_t sample_cost = point_count + clip_path_count / 2u + 1u;
    for (int y = top; y < bottom && valid && !work_exhausted; y++) {
        memset(coverage, 0, span);
        for (size_t sy = 0; sy < 2u && valid; sy++) {
            double py = y + sample[sy];
            size_t count = canvas_path_crossings(
                points, point_count, py, crossings, point_count);
            if (count > point_count) {
                valid = false;
                break;
            }
            size_t crossing = 0;
            int winding = 0;
            bool parity = false;
            for (int x = left; x < right && !work_exhausted; x++) {
                for (size_t sx = 0; sx < 2u; sx++) {
                    if (!canvas_work_take(work_remaining, sample_cost)) {
                        work_exhausted = true;
                        break;
                    }
                    double px = x + sample[sx];
                    while (crossing < count && crossings[crossing].x <= px) {
                        parity = !parity;
                        winding += crossings[crossing].winding;
                        crossing++;
                    }
                    bool inside = even_odd ? parity : winding != 0;
                    if (inside && canvas_clip_stack_contains(
                            clip_paths, clip_path_count, px, py)) {
                        coverage[(size_t) (x - left)]++;
                    }
                }
            }
        }
        for (int x = left; x < right && valid && !work_exhausted; x++) {
            unsigned covered = coverage[(size_t) (x - left)];
            if (covered == 0u) continue;
            int color[4];
            if (!canvas_gradient_color(
                    gradient, gradient_count, x + 0.5, y + 0.5,
                    fallback, color)) {
                valid = false;
                break;
            }
            size_t at = ((size_t) y * (size_t) width + (size_t) x) * 4u;
            canvas_blend_pixel(pixels, at, color[0], color[1], color[2],
                               color[3], global_alpha, operation,
                               (double) covered / 4.0);
        }
    }
    js_free(context, crossings);
    js_free(context, coverage);
    if (clipped_out != NULL) *clipped_out = work_exhausted;
    return valid;
}

static bool canvas_stroke_join_contains(
    double x, double y, double x0, double y0,
    double px, double py, double x2, double y2,
    double radius, int line_join, double miter_limit)
{
    double d0x = px - x0, d0y = py - y0;
    double d1x = x2 - px, d1y = y2 - py;
    double length0 = hypot(d0x, d0y), length1 = hypot(d1x, d1y);
    if (length0 <= 1e-9 || length1 <= 1e-9) return false;
    d0x /= length0; d0y /= length0;
    d1x /= length1; d1y /= length1;
    double cross = d0x * d1y - d0y * d1x;
    if (fabs(cross) <= 1e-9) return false;
    if (line_join == 1) return hypot(x - px, y - py) <= radius;
    double side = cross > 0.0 ? -1.0 : 1.0;
    double n0x = -d0y * side, n0y = d0x * side;
    double n1x = -d1y * side, n1y = d1x * side;
    double q0x = px + n0x * radius, q0y = py + n0y * radius;
    double q1x = px + n1x * radius, q1y = py + n1y * radius;
    if (canvas_point_in_triangle(x, y, px, py, q0x, q0y, q1x, q1y))
        return true;
    if (line_join != 0) return false;
    double denominator = d0x * d1y - d0y * d1x;
    double rx = q1x - q0x, ry = q1y - q0y;
    double t = (rx * d1y - ry * d1x) / denominator;
    double mx = q0x + d0x * t, my = q0y + d0y * t;
    return hypot(mx - px, my - py) <= miter_limit * radius
        && canvas_point_in_triangle(x, y, q0x, q0y, mx, my, q1x, q1y);
}

static bool canvas_raster_path_stroke(
    JSContext *context, uint8_t *pixels, int width,
    int left, int top, int right, int bottom,
    const uint8_t *points, size_t point_count, double radius,
    const uint8_t *dash, size_t dash_count, double dash_offset,
    int line_cap, int line_join, double miter_limit,
    const uint8_t *clip_paths, size_t clip_path_count,
    const int color[4], double global_alpha, int operation,
    size_t *work_remaining, bool *clipped_out)
{
    size_t span = (size_t) (right - left);
    size_t rows = (size_t) (bottom - top);
    if (span == 0u || rows == 0u || span > SIZE_MAX / rows) return false;
    size_t area = span * rows;
    uint8_t *coverage = js_mallocz(context, area);
    if (coverage == NULL) return false;
    static const double sample[2] = {0.25, 0.75};
    size_t start = 0;
    bool work_exhausted = false;
    size_t sample_cost = clip_path_count / 2u + dash_count + 1u;
    double radius_squared = radius * radius;
    while (start < point_count && !work_exhausted) {
        while (start < point_count
               && !isfinite(canvas_double_at(points, start * 2u))) start++;
        size_t end = start;
        while (end < point_count
               && isfinite(canvas_double_at(points, end * 2u))) end++;
        bool closed = end - start >= 3u
            && canvas_double_at(points, start * 2u)
                == canvas_double_at(points, (end - 1u) * 2u)
            && canvas_double_at(points, start * 2u + 1u)
                == canvas_double_at(points, (end - 1u) * 2u + 1u);
        double distance_along = 0.0;
        for (size_t i = start + 1u; i < end; i++) {
            double x0 = canvas_double_at(points, (i - 1u) * 2u);
            double y0 = canvas_double_at(points, (i - 1u) * 2u + 1u);
            double x1 = canvas_double_at(points, i * 2u);
            double y1 = canvas_double_at(points, i * 2u + 1u);
            double length = hypot(x1 - x0, y1 - y0);
            if (length <= 1e-9) continue;
            bool start_cap = !closed && i == start + 1u;
            bool end_cap = !closed && i + 1u == end;
            double extension = radius;
            int segment_left = 0, segment_top = 0;
            int segment_right = 0, segment_bottom = 0;
            if (!canvas_integer(fmin(x0, x1) - extension - 1.0,
                                CANVAS_INTEGER_FLOOR, &segment_left)
                || !canvas_integer(fmin(y0, y1) - extension - 1.0,
                                   CANVAS_INTEGER_FLOOR, &segment_top)
                || !canvas_integer(fmax(x0, x1) + extension + 1.0,
                                   CANVAS_INTEGER_CEIL, &segment_right)
                || !canvas_integer(fmax(y0, y1) + extension + 1.0,
                                   CANVAS_INTEGER_CEIL, &segment_bottom)) {
                js_free(context, coverage);
                return false;
            }
            if (segment_left < left) segment_left = left;
            if (segment_top < top) segment_top = top;
            if (segment_right > right) segment_right = right;
            if (segment_bottom > bottom) segment_bottom = bottom;
            for (int y = segment_top;
                 y < segment_bottom && !work_exhausted; y++) {
                for (int x = segment_left;
                     x < segment_right && !work_exhausted; x++) {
                    size_t mask_at = (size_t) (y - top) * span
                        + (size_t) (x - left);
                    for (size_t sy = 0; sy < 2u; sy++) {
                        for (size_t sx = 0; sx < 2u; sx++) {
                            if (!canvas_work_take(
                                    work_remaining, sample_cost)) {
                                work_exhausted = true;
                                break;
                            }
                            unsigned bit = 1u << (sy * 2u + sx);
                            if ((coverage[mask_at] & bit) != 0u) continue;
                            double px = x + sample[sx], py = y + sample[sy];
                            double position = 0.0;
                            double distance_squared =
                                canvas_segment_distance_squared(
                                px, py, x0, y0, x1, y1, &position);
                            bool inside = position >= 0.0 && position <= 1.0
                                && distance_squared <= radius_squared;
                            if (!inside && line_cap == 1
                                && ((position < 0.0 && start_cap)
                                    || (position > 1.0 && end_cap))) {
                                inside = distance_squared <= radius_squared;
                            } else if (!inside && line_cap == 2
                                       && ((position < 0.0 && start_cap)
                                           || (position > 1.0 && end_cap))) {
                                double perpendicular = fabs(
                                    (x1 - x0) * (y0 - py)
                                    - (x0 - px) * (y1 - y0)) / length;
                                inside = perpendicular <= radius
                                    && position >= -radius / length
                                    && position <= 1.0 + radius / length;
                            }
                            double dash_position = fmax(0.0, fmin(1.0, position));
                            if (inside && canvas_dash_on(
                                    dash, dash_count, dash_offset,
                                    distance_along + dash_position * length)
                                && canvas_clip_stack_contains(
                                    clip_paths, clip_path_count, px, py)) {
                                coverage[mask_at] |= (uint8_t) bit;
                            }
                        }
                    }
                }
            }
            distance_along += length;
        }
        for (size_t i = start + 1u; i + 1u < end; i++) {
            double x0 = canvas_double_at(points, (i - 1u) * 2u);
            double y0 = canvas_double_at(points, (i - 1u) * 2u + 1u);
            double px = canvas_double_at(points, i * 2u);
            double py = canvas_double_at(points, i * 2u + 1u);
            double x2 = canvas_double_at(points, (i + 1u) * 2u);
            double y2 = canvas_double_at(points, (i + 1u) * 2u + 1u);
            double extent = radius * (line_join == 0
                ? miter_limit : 1.0) + 1.0;
            int join_left = 0, join_top = 0, join_right = 0, join_bottom = 0;
            if (!canvas_integer(px - extent, CANVAS_INTEGER_FLOOR,
                                &join_left)
                || !canvas_integer(py - extent, CANVAS_INTEGER_FLOOR,
                                   &join_top)
                || !canvas_integer(px + extent, CANVAS_INTEGER_CEIL,
                                   &join_right)
                || !canvas_integer(py + extent, CANVAS_INTEGER_CEIL,
                                   &join_bottom)) {
                js_free(context, coverage);
                return false;
            }
            if (join_left < left) join_left = left;
            if (join_top < top) join_top = top;
            if (join_right > right) join_right = right;
            if (join_bottom > bottom) join_bottom = bottom;
            for (int y = join_top;
                 y < join_bottom && !work_exhausted; y++) {
                for (int x = join_left;
                     x < join_right && !work_exhausted; x++) {
                    size_t mask_at = (size_t) (y - top) * span
                        + (size_t) (x - left);
                    for (size_t sy = 0; sy < 2u; sy++) {
                        for (size_t sx = 0; sx < 2u; sx++) {
                            if (!canvas_work_take(
                                    work_remaining, sample_cost)) {
                                work_exhausted = true;
                                break;
                            }
                            unsigned bit = 1u << (sy * 2u + sx);
                            if ((coverage[mask_at] & bit) != 0u) continue;
                            double sample_x = x + sample[sx];
                            double sample_y = y + sample[sy];
                            if (canvas_stroke_join_contains(
                                    sample_x, sample_y, x0, y0, px, py,
                                    x2, y2, radius, line_join, miter_limit)
                                && canvas_clip_stack_contains(
                                    clip_paths, clip_path_count,
                                    sample_x, sample_y)) {
                                coverage[mask_at] |= (uint8_t) bit;
                            }
                        }
                    }
                }
            }
        }
        start = end + 1u;
    }
    static const uint8_t bit_count[16] = {
        0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4,
    };
    for (int y = top; y < bottom; y++) {
        for (int x = left; x < right; x++) {
            unsigned covered = bit_count[coverage[
                (size_t) (y - top) * span + (size_t) (x - left)] & 15u];
            if (covered == 0u) continue;
            size_t at = ((size_t) y * (size_t) width + (size_t) x) * 4u;
            canvas_blend_pixel(pixels, at, color[0], color[1], color[2],
                               color[3], global_alpha, operation,
                               (double) covered / 4.0);
        }
    }
    js_free(context, coverage);
    if (clipped_out != NULL) *clipped_out = work_exhausted;
    return true;
}

static JSValue canvas_raster_path_with_work(
    JSContext *context, JSValueConst this_value,
    int argc, JSValueConst *argv, size_t *work_remaining,
    bool *clipped_out)
{
    (void) this_value;
    int32_t width = 0, height = 0, red = 0, green = 0, blue = 0, alpha = 0;
    int32_t operation = 0;
    int32_t line_cap = 0, line_join = 0;
    double miter_limit = 10.0;
    double global_alpha = 0.0, line_width = 0.0, dash_offset = 0.0;
    if (argc < 21 || JS_ToInt32(context, &width, argv[1]) < 0
        || JS_ToInt32(context, &height, argv[2]) < 0
        || JS_ToInt32(context, &red, argv[6]) < 0
        || JS_ToInt32(context, &green, argv[7]) < 0
        || JS_ToInt32(context, &blue, argv[8]) < 0
        || JS_ToInt32(context, &alpha, argv[9]) < 0
        || JS_ToFloat64(context, &global_alpha, argv[10]) < 0
        || JS_ToInt32(context, &operation, argv[11]) < 0
        || JS_ToFloat64(context, &line_width, argv[12]) < 0
        || JS_ToInt32(context, &line_cap, argv[13]) < 0
        || JS_ToInt32(context, &line_join, argv[14]) < 0
        || JS_ToFloat64(context, &miter_limit, argv[15]) < 0
        || JS_ToFloat64(context, &dash_offset, argv[17]) < 0) {
        return JS_EXCEPTION;
    }
    size_t required = 0;
    if (!canvas_dimensions(width, height, &required)
        || operation < 1 || operation > 11
        || line_cap < 0 || line_cap > 2
        || line_join < 0 || line_join > 2
        || miter_limit <= 0.0 || miter_limit > 64.0
        || global_alpha < 0.0 || global_alpha > 1.0) return JS_FALSE;
    bool fill = JS_ToBool(context, argv[4]) > 0;
    bool even_odd = JS_ToBool(context, argv[5]) > 0;
    uint8_t *pixels = NULL;
    JSValue pixel_buffer = JS_UNDEFINED;
    bool native = false;
    if (!canvas_mutable_pixels(context, argv[0], width, height, required,
                               &pixels, &pixel_buffer, &native)) {
        if (JS_IsException(pixel_buffer)) return pixel_buffer;
        return JS_FALSE;
    }
    const uint8_t *points = NULL, *dash = NULL, *clip_bytes = NULL;
    const uint8_t *gradient = NULL, *clip_paths = NULL;
    size_t coordinate_count = 0, dash_count = 0, clip_count = 0;
    size_t gradient_count = 0, clip_path_count = 0;
    JSValue point_buffer = JS_UNDEFINED, dash_buffer = JS_UNDEFINED;
    JSValue clip_buffer = JS_UNDEFINED, gradient_buffer = JS_UNDEFINED;
    JSValue clip_path_buffer = JS_UNDEFINED;
    bool arrays_ok = canvas_typed_bytes(
            context, argv[3], sizeof(double), &points, &coordinate_count,
            &point_buffer)
        && canvas_typed_bytes(
            context, argv[16], sizeof(double), &dash, &dash_count,
            &dash_buffer)
        && canvas_typed_bytes(
            context, argv[18], sizeof(double), &clip_bytes, &clip_count,
            &clip_buffer)
        && canvas_typed_bytes(
            context, argv[19], sizeof(double), &gradient, &gradient_count,
            &gradient_buffer)
        && canvas_typed_bytes(
            context, argv[20], sizeof(double), &clip_paths,
            &clip_path_count, &clip_path_buffer);
    if (!arrays_ok || coordinate_count < 4u || (coordinate_count & 1u)
        || coordinate_count > 2048u || dash_count > 32u || clip_count < 4u
        || clip_path_count > 8201u) {
        JS_FreeValue(context, clip_path_buffer);
        JS_FreeValue(context, gradient_buffer);
        JS_FreeValue(context, clip_buffer);
        JS_FreeValue(context, dash_buffer);
        JS_FreeValue(context, point_buffer);
        JS_FreeValue(context, pixel_buffer);
        return JS_FALSE;
    }
    size_t point_count = coordinate_count / 2u;
    double minimum_x = width, minimum_y = height, maximum_x = 0, maximum_y = 0;
    size_t finite_points = 0;
    for (size_t i = 0; i < point_count; i++) {
        double x = canvas_double_at(points, i * 2u);
        double y = canvas_double_at(points, i * 2u + 1u);
        if (!isfinite(x) || !isfinite(y)) continue;
        if (x < minimum_x) minimum_x = x;
        if (x > maximum_x) maximum_x = x;
        if (y < minimum_y) minimum_y = y;
        if (y > maximum_y) maximum_y = y;
        finite_points++;
    }
    int clip[4] = {0};
    bool bounds_ok = true;
    for (size_t i = 0; i < 4u; i++) {
        double value = canvas_double_at(clip_bytes, i);
        bounds_ok = bounds_ok && canvas_integer(
            value, i < 2u ? CANVAS_INTEGER_FLOOR : CANVAS_INTEGER_CEIL,
            &clip[i]);
    }
    if (clip[0] < 0) clip[0] = 0;
    if (clip[1] < 0) clip[1] = 0;
    if (clip[2] > width) clip[2] = width;
    if (clip[3] > height) clip[3] = height;
    double radius = fill ? 1.0 : fmax(0.5, fmin(16.0, line_width) / 2.0);
    double extent = !fill && line_join == 0
        ? radius * miter_limit : radius;
    int left = 0, top = 0, right = 0, bottom = 0;
    bounds_ok = bounds_ok
        && canvas_integer(minimum_x - extent - 1.0,
                          CANVAS_INTEGER_FLOOR, &left)
        && canvas_integer(minimum_y - extent - 1.0,
                          CANVAS_INTEGER_FLOOR, &top)
        && canvas_integer(maximum_x + extent + 1.0,
                          CANVAS_INTEGER_CEIL, &right)
        && canvas_integer(maximum_y + extent + 1.0,
                          CANVAS_INTEGER_CEIL, &bottom);
    if (left < clip[0]) left = clip[0];
    if (top < clip[1]) top = clip[1];
    if (right > clip[2]) right = clip[2];
    if (bottom > clip[3]) bottom = clip[3];
    size_t area = right > left && bottom > top
        ? (size_t) (right - left) * (size_t) (bottom - top) : 0u;
    if (!bounds_ok || finite_points < 2u || area == 0u
        || (finite_points > 0u
            && area > (8u * 1024u * 1024u) / finite_points)) {
        JS_FreeValue(context, clip_path_buffer);
        JS_FreeValue(context, gradient_buffer);
        JS_FreeValue(context, clip_buffer);
        JS_FreeValue(context, dash_buffer);
        JS_FreeValue(context, point_buffer);
        JS_FreeValue(context, pixel_buffer);
        return JS_FALSE;
    }
    int fallback[4] = {red, green, blue, alpha};
    if (fill) {
        bool clipped = false;
        bool rendered = canvas_raster_path_fill(
            context, pixels, width, left, top, right, bottom,
            points, point_count, even_odd, clip_paths, clip_path_count,
            gradient, gradient_count, fallback, global_alpha, operation,
            work_remaining, &clipped);
        JS_FreeValue(context, clip_path_buffer);
        JS_FreeValue(context, gradient_buffer);
        JS_FreeValue(context, clip_buffer);
        JS_FreeValue(context, dash_buffer);
        JS_FreeValue(context, point_buffer);
        JS_FreeValue(context, pixel_buffer);
        if (clipped_out != NULL) *clipped_out = clipped;
        if (!rendered) return JS_FALSE;
        return JS_NewInt32(context, clipped ? 2 : 1);
    }
    bool clipped = false;
    bool rendered = canvas_raster_path_stroke(
        context, pixels, width, left, top, right, bottom,
        points, point_count, radius, dash, dash_count, dash_offset,
        line_cap, line_join, miter_limit, clip_paths, clip_path_count,
        fallback, global_alpha, operation, work_remaining, &clipped);
    JS_FreeValue(context, clip_path_buffer);
    JS_FreeValue(context, gradient_buffer);
    JS_FreeValue(context, clip_buffer);
    JS_FreeValue(context, dash_buffer);
    JS_FreeValue(context, point_buffer);
    JS_FreeValue(context, pixel_buffer);
    if (clipped_out != NULL) *clipped_out = clipped;
    if (!rendered) return JS_FALSE;
    return JS_NewInt32(context, clipped ? 2 : 1);
}

JSValue js_canvas_raster_path(JSContext *context,
                              JSValueConst this_value,
                              int argc, JSValueConst *argv)
{
    size_t work_remaining = CANVAS_RASTER_WORK_LIMIT;
    return canvas_raster_path_with_work(
        context, this_value, argc, argv, &work_remaining, NULL);
}

/* Path and text operations retain variable-sized geometry, clip and string
   payloads, so flattening them into one giant numeric record would duplicate
   the largest per-canvas buffers in QuickJS. Keep those already-bounded typed
   arrays in a small command list and cross into C once per animation turn.
   A failure bit lets the facade apply its existing path fallback without
   replaying successful translucent commands. */
JSValue js_canvas_raster_paint_batch(JSContext *context,
                                     JSValueConst this_value,
                                     int argc, JSValueConst *argv)
{
    (void) this_value;
    if (argc < 4) return JS_NewUint32(context, UINT32_MAX);
    JSValue length_value = JS_GetPropertyStr(context, argv[3], "length");
    if (JS_IsException(length_value)) return length_value;
    uint32_t command_count = 0;
    int converted = JS_ToUint32(context, &command_count, length_value);
    JS_FreeValue(context, length_value);
    if (converted < 0) return JS_EXCEPTION;
    if (command_count == 0u || command_count > 16u)
        return JS_NewUint32(context, UINT32_MAX);

    uint32_t failed = 0u, clipped = 0u;
    size_t work_remaining = CANVAS_RASTER_WORK_LIMIT;
    for (uint32_t command_index = 0; command_index < command_count;
         command_index++) {
        JSValue command = JS_GetPropertyUint32(
            context, argv[3], command_index);
        if (JS_IsException(command)) return command;
        JSValue kind_value = JS_GetPropertyUint32(context, command, 0u);
        int32_t kind = -1;
        if (JS_IsException(kind_value)
            || JS_ToInt32(context, &kind, kind_value) < 0) {
            JS_FreeValue(context, kind_value);
            JS_FreeValue(context, command);
            return JS_EXCEPTION;
        }
        JS_FreeValue(context, kind_value);
        /* Kind 2 is text whose colour comes from a pattern (two more
           payload values: the source and its parameters). */
        int payload_count = kind == 0 ? 18 : kind == 1 ? 19
            : kind == 2 ? 21 : 0;
        JSValue call_args[24];
        call_args[0] = argv[0];
        call_args[1] = argv[1];
        call_args[2] = argv[2];
        bool payload_ok = payload_count != 0;
        int acquired = 0;
        for (; payload_ok && acquired < payload_count; acquired++) {
            call_args[acquired + 3] = JS_GetPropertyUint32(
                context, command, (uint32_t) acquired + 1u);
            if (JS_IsException(call_args[acquired + 3])) payload_ok = false;
        }
        JSValue result = JS_FALSE;
        bool command_clipped = false;
        if (payload_ok) {
            result = kind == 0
                ? canvas_raster_path_with_work(
                      context, JS_UNDEFINED, 21, call_args, &work_remaining,
                      &command_clipped)
                : canvas_raster_text_with_work(
                      context, JS_UNDEFINED, payload_count + 3, call_args,
                      &work_remaining, &command_clipped);
        }
        for (int at = 0; at < acquired; at++)
            JS_FreeValue(context, call_args[at + 3]);
        JS_FreeValue(context, command);
        if (!payload_ok) {
            JS_FreeValue(context, result);
            return JS_EXCEPTION;
        }
        if (JS_IsException(result)) return result;
        if (JS_ToBool(context, result) <= 0)
            failed |= UINT32_C(1) << command_index;
        else if (command_clipped)
            clipped |= UINT32_C(1) << command_index;
        JS_FreeValue(context, result);
    }
    return JS_NewUint32(context, failed | (clipped << 16u));
}

static bool canvas_image_sample(const uint8_t *pixels, int width, int height,
                                double x, double y, bool smooth,
                                int output[4])
{
    if (!smooth) {
        int ix = 0, iy = 0;
        if (!canvas_integer(x, CANVAS_INTEGER_FLOOR, &ix)
            || !canvas_integer(y, CANVAS_INTEGER_FLOOR, &iy)) return false;
        if (ix < 0 || iy < 0 || ix >= width || iy >= height) {
            memset(output, 0, 4u * sizeof(*output));
            return true;
        }
        size_t at = ((size_t) iy * (size_t) width + (size_t) ix) * 4u;
        for (size_t c = 0; c < 4u; c++) output[c] = pixels[at + c];
        return true;
    }
    double sample_x = x - 0.5, sample_y = y - 0.5;
    int x0 = 0, y0 = 0;
    if (!canvas_integer(sample_x, CANVAS_INTEGER_FLOOR, &x0)
        || !canvas_integer(sample_y, CANVAS_INTEGER_FLOOR, &y0)) return false;
    double fx = sample_x - x0, fy = sample_y - y0;
    for (size_t c = 0; c < 4u; c++) {
        double value = 0.0;
        for (int oy = 0; oy < 2; oy++) {
            int py = y0 + oy;
            if (py < 0) py = 0;
            if (py >= height) py = height - 1;
            for (int ox = 0; ox < 2; ox++) {
                int px = x0 + ox;
                if (px < 0) px = 0;
                if (px >= width) px = width - 1;
                double weight = (ox ? fx : 1.0 - fx)
                    * (oy ? fy : 1.0 - fy);
                value += pixels[((size_t) py * (size_t) width + (size_t) px)
                                * 4u + c] * weight;
            }
        }
        if (!canvas_integer(value, CANVAS_INTEGER_NEAREST, &output[c]))
            return false;
    }
    return true;
}

static bool canvas_raster_image_pixels(
    uint8_t *target, int width,
    const uint8_t *source, int source_width, int source_height,
    double sx, double sy, double sw, double sh,
    double dx, double dy, double dw, double dh, bool smooth,
    double global_alpha, int operation, const double transform[6],
    const int clip[4], const uint8_t *clip_paths, size_t clip_path_count,
    size_t *work_remaining, bool *clipped_out)
{
    double determinant = transform[0] * transform[3]
        - transform[1] * transform[2];
    if (fabs(determinant) < 1e-12) return true;
    double corners[8];
    canvas_transform_point(transform, dx, dy, &corners[0], &corners[1]);
    canvas_transform_point(transform, dx + dw, dy, &corners[2], &corners[3]);
    canvas_transform_point(
        transform, dx + dw, dy + dh, &corners[4], &corners[5]);
    canvas_transform_point(transform, dx, dy + dh, &corners[6], &corners[7]);
    double min_x = corners[0], max_x = corners[0];
    double min_y = corners[1], max_y = corners[1];
    for (size_t i = 1; i < 4u; i++) {
        if (corners[i * 2u] < min_x) min_x = corners[i * 2u];
        if (corners[i * 2u] > max_x) max_x = corners[i * 2u];
        if (corners[i * 2u + 1u] < min_y) min_y = corners[i * 2u + 1u];
        if (corners[i * 2u + 1u] > max_y) max_y = corners[i * 2u + 1u];
    }
    int left = 0, top = 0, right = 0, bottom = 0;
    if (!canvas_integer(min_x, CANVAS_INTEGER_FLOOR, &left)
        || !canvas_integer(min_y, CANVAS_INTEGER_FLOOR, &top)
        || !canvas_integer(max_x, CANVAS_INTEGER_CEIL, &right)
        || !canvas_integer(max_y, CANVAS_INTEGER_CEIL, &bottom)) return false;
    if (left < clip[0]) left = clip[0];
    if (top < clip[1]) top = clip[1];
    if (right > clip[2]) right = clip[2];
    if (bottom > clip[3]) bottom = clip[3];
    size_t sample_cost = clip_path_count / 2u + (smooth ? 4u : 1u);
    bool work_exhausted = false;
    for (int y = top; y < bottom && !work_exhausted; y++) {
        for (int x = left; x < right; x++) {
            if (!canvas_work_take(work_remaining, sample_cost)) {
                work_exhausted = true;
                break;
            }
            if (!canvas_clip_stack_contains(
                    clip_paths, clip_path_count, x + 0.5, y + 0.5)) continue;
            double tx = x + 0.5 - transform[4];
            double ty = y + 0.5 - transform[5];
            double local_x = (transform[3] * tx - transform[2] * ty)
                / determinant;
            double local_y = (-transform[1] * tx + transform[0] * ty)
                / determinant;
            if (local_x < dx || local_y < dy
                || local_x >= dx + dw || local_y >= dy + dh) continue;
            double source_x = sx + (local_x - dx) * sw / dw;
            double source_y = sy + (local_y - dy) * sh / dh;
            if (source_x < 0.0 || source_y < 0.0
                || source_x >= source_width || source_y >= source_height) {
                continue;
            }
            int color[4];
            if (!canvas_image_sample(source, source_width, source_height,
                                     source_x, source_y, smooth, color)) {
                return false;
            }
            size_t at = ((size_t) y * (size_t) width + (size_t) x) * 4u;
            if (operation == 0) {
                /* putImageData into a native store: the bytes as given. */
                for (size_t c = 0; c < 4u; c++)
                    target[at + c] = (uint8_t) color[c];
                continue;
            }
            canvas_blend_pixel(target, at, color[0], color[1], color[2],
                               color[3], global_alpha, operation, 1.0);
        }
    }
    if (clipped_out != NULL) *clipped_out = work_exhausted;
    return true;
}

JSValue js_canvas_raster_image(JSContext *context,
                               JSValueConst this_value,
                               int argc, JSValueConst *argv)
{
    (void) this_value;
    int32_t width = 0, height = 0, source_width = 0, source_height = 0;
    int32_t operation = 0;
    double sx = 0, sy = 0, sw = 0, sh = 0, dx = 0, dy = 0, dw = 0, dh = 0;
    double global_alpha = 0.0;
    if (argc < 20 || JS_ToInt32(context, &width, argv[1]) < 0
        || JS_ToInt32(context, &height, argv[2]) < 0
        || JS_ToInt32(context, &source_width, argv[4]) < 0
        || JS_ToInt32(context, &source_height, argv[5]) < 0
        || JS_ToFloat64(context, &sx, argv[6]) < 0
        || JS_ToFloat64(context, &sy, argv[7]) < 0
        || JS_ToFloat64(context, &sw, argv[8]) < 0
        || JS_ToFloat64(context, &sh, argv[9]) < 0
        || JS_ToFloat64(context, &dx, argv[10]) < 0
        || JS_ToFloat64(context, &dy, argv[11]) < 0
        || JS_ToFloat64(context, &dw, argv[12]) < 0
        || JS_ToFloat64(context, &dh, argv[13]) < 0
        || JS_ToFloat64(context, &global_alpha, argv[15]) < 0
        || JS_ToInt32(context, &operation, argv[16]) < 0) {
        return JS_EXCEPTION;
    }
    size_t required = 0, source_required = 0;
    if (!canvas_dimensions(width, height, &required)
        || !canvas_dimensions(source_width, source_height, &source_required)
        || !isfinite(sx) || !isfinite(sy) || !isfinite(sw) || !isfinite(sh)
        || !isfinite(dx) || !isfinite(dy) || !isfinite(dw) || !isfinite(dh)
        || sw == 0.0 || sh == 0.0 || dw == 0.0 || dh == 0.0
        || global_alpha < 0.0 || global_alpha > 1.0
        || operation < 0 || operation > 11) return JS_FALSE;
    if (sw < 0.0) { sx += sw; sw = -sw; }
    if (sh < 0.0) { sy += sh; sh = -sh; }
    if (dw < 0.0) { dx += dw; dw = -dw; }
    if (dh < 0.0) { dy += dh; dh = -dh; }
    uint8_t *target = NULL;
    JSValue target_buffer = JS_UNDEFINED;
    bool native = false;
    if (!canvas_mutable_pixels(context, argv[0], width, height, required,
                               &target, &target_buffer, &native)) {
        if (JS_IsException(target_buffer)) return target_buffer;
        return JS_FALSE;
    }
    /* A source named by a number (not origin-clean, or a native store) is
       drawn into a native store only: never into a script buffer. */
    CanvasSource source;
    uint8_t *source_copy = NULL;
    if (!canvas_source_pixels(context, argv[3], source_width, source_height,
                              &source)
        || (source.native && !native)
        || !canvas_source_unalias(context, &source, target, &source_copy)) {
        JS_FreeValue(context, target_buffer);
        if (JS_IsException(source.buffer)) return source.buffer;
        JS_FreeValue(context, source.buffer);
        return JS_FALSE;
    }
    double transform[6];
    int clip[4];
    JSValue transform_buffer = JS_UNDEFINED, clip_buffer = JS_UNDEFINED;
    const uint8_t *clip_paths = NULL;
    size_t clip_path_count = 0;
    JSValue clip_path_buffer = JS_UNDEFINED;
    bool rendered = false, clipped = false;
    if (canvas_read_transform_clip(
            context, argv[17], argv[18], transform, clip, width, height,
            &transform_buffer, &clip_buffer)
        && canvas_typed_bytes(
            context, argv[19], sizeof(double), &clip_paths,
            &clip_path_count, &clip_path_buffer)
        && clip_path_count <= 8201u) {
        bool smooth = JS_ToBool(context, argv[14]) > 0;
        size_t work_remaining = CANVAS_RASTER_WORK_LIMIT;
        rendered = canvas_raster_image_pixels(
            target, width, source.pixels, source.width, source.height,
            sx, sy, sw, sh, dx, dy, dw, dh, smooth, global_alpha, operation,
            transform, clip, clip_paths, clip_path_count, &work_remaining,
            &clipped);
    }
    js_free(context, source_copy);
    JS_FreeValue(context, clip_path_buffer);
    JS_FreeValue(context, clip_buffer);
    JS_FreeValue(context, transform_buffer);
    JS_FreeValue(context, source.buffer);
    JS_FreeValue(context, target_buffer);
    if (!rendered) return JS_FALSE;
    return JS_NewInt32(context, clipped ? 2 : 1);
}

JSValue js_canvas_raster_image_batch(JSContext *context,
                                     JSValueConst this_value,
                                     int argc, JSValueConst *argv)
{
    (void) this_value;
    int32_t width = 0, height = 0;
    size_t required = 0;
    if (argc < 5 || JS_ToInt32(context, &width, argv[1]) < 0
        || JS_ToInt32(context, &height, argv[2]) < 0) return JS_EXCEPTION;
    if (!canvas_dimensions(width, height, &required)) return JS_FALSE;
    /* Source lookups are property reads on an author object, so a getter or
       Proxy can detach the target or the command buffer. Read the source
       indices, fetch every source value, and only then resolve the raw
       pointers that the blit loop uses; no JavaScript runs after that. */
    const uint8_t *commands = NULL;
    size_t command_values = 0;
    JSValue command_buffer = JS_UNDEFINED;
    if (!canvas_typed_bytes(
            context, argv[4], sizeof(double), &commands,
            &command_values, &command_buffer)
        || command_values == 0u || command_values % 24u != 0u
        || command_values > 16u * 24u) {
        JS_FreeValue(context, command_buffer);
        return JS_FALSE;
    }
    size_t command_count = command_values / 24u;
    uint8_t source_indices[16];
    for (size_t command = 0; command < command_count; command++) {
        double raw_source_index = canvas_double_at(commands, command * 24u);
        if (!isfinite(raw_source_index) || raw_source_index < 0.0
            || raw_source_index > 15.0
            || floor(raw_source_index) != raw_source_index) {
            JS_FreeValue(context, command_buffer);
            return JS_FALSE;
        }
        source_indices[command] = (uint8_t) raw_source_index;
    }
    JS_FreeValue(context, command_buffer);
    command_buffer = JS_UNDEFINED;
    JSValue source_values[16];
    size_t fetched = 0;
    for (; fetched < command_count; fetched++) {
        source_values[fetched] = JS_GetPropertyUint32(
            context, argv[3], source_indices[fetched]);
        if (JS_IsException(source_values[fetched])) break;
    }
    uint8_t *target = NULL;
    JSValue target_buffer = JS_UNDEFINED;
    bool native = false;
    size_t current_values = 0;
    bool resolved = fetched == command_count
        && canvas_typed_bytes(
            context, argv[4], sizeof(double), &commands,
            &current_values, &command_buffer)
        && current_values == command_values
        && canvas_mutable_pixels(context, argv[0], width, height, required,
                                 &target, &target_buffer, &native);
    if (!resolved) {
        bool threw = fetched != command_count
            || JS_IsException(command_buffer)
            || JS_IsException(target_buffer);
        JS_FreeValue(context, target_buffer);
        JS_FreeValue(context, command_buffer);
        for (size_t at = 0; at < fetched; at++)
            JS_FreeValue(context, source_values[at]);
        return threw ? JS_EXCEPTION : JS_FALSE;
    }
    bool rendered = true, clipped = false;
    size_t work_remaining = CANVAS_RASTER_WORK_LIMIT;
    for (size_t command = 0; command < command_count; command++) {
        size_t base = command * 24u;
        JSValueConst source_value = source_values[command];
        double raw_source_width = canvas_double_at(commands, base + 1u);
        double raw_source_height = canvas_double_at(commands, base + 2u);
        int source_width = 0, source_height = 0;
        bool source_dimensions_ok = raw_source_width >= 1.0
            && raw_source_height >= 1.0
            && canvas_integer(raw_source_width, CANVAS_INTEGER_TRUNCATE,
                              &source_width)
            && canvas_integer(raw_source_height, CANVAS_INTEGER_TRUNCATE,
                              &source_height);
        CanvasSource resolved_source;
        uint8_t *source_copy = NULL;
        bool source_ok = source_dimensions_ok && canvas_source_pixels(
                context, source_value, source_width, source_height,
                &resolved_source);
        if (JS_IsNumber(source_value)
            && (!source_ok || !native
                || !canvas_source_unalias(context, &resolved_source, target,
                                          &source_copy))) {
            /* A source named by a number that is gone, or that may not be
               drawn into this target, draws nothing (as a broken image);
               it never holds up the rest of the batch. */
            continue;
        }
        if (!source_ok) {
            JS_FreeValue(context, resolved_source.buffer);
            rendered = false;
            break;
        }
        const uint8_t *source = resolved_source.pixels;
        source_width = resolved_source.width;
        source_height = resolved_source.height;
        JSValue source_buffer = resolved_source.buffer;
        double sx = canvas_double_at(commands, base + 3u);
        double sy = canvas_double_at(commands, base + 4u);
        double sw = canvas_double_at(commands, base + 5u);
        double sh = canvas_double_at(commands, base + 6u);
        double dx = canvas_double_at(commands, base + 7u);
        double dy = canvas_double_at(commands, base + 8u);
        double dw = canvas_double_at(commands, base + 9u);
        double dh = canvas_double_at(commands, base + 10u);
        bool smooth = canvas_double_at(commands, base + 11u) != 0.0;
        double global_alpha = canvas_double_at(commands, base + 12u);
        int operation = 0;
        bool operation_ok = canvas_integer(
            canvas_double_at(commands, base + 13u),
            CANVAS_INTEGER_TRUNCATE, &operation);
        double transform[6];
        int clip[4];
        for (size_t i = 0; i < 6u; i++)
            transform[i] = canvas_double_at(commands, base + 14u + i);
        bool clip_ok = true;
        for (size_t i = 0; i < 4u; i++) {
            double value = canvas_double_at(commands, base + 20u + i);
            if (!canvas_integer(value, CANVAS_INTEGER_TRUNCATE, &clip[i])) {
                clip_ok = false;
                clip[i] = 0;
            }
        }
        bool values_ok = isfinite(sx) && isfinite(sy)
            && isfinite(sw) && isfinite(sh) && isfinite(dx) && isfinite(dy)
            && isfinite(dw) && isfinite(dh) && sw != 0.0 && sh != 0.0
            && dw != 0.0 && dh != 0.0 && global_alpha >= 0.0
            && global_alpha <= 1.0 && operation >= 1 && operation <= 11;
        for (size_t i = 0; i < 6u; i++)
            values_ok = values_ok && isfinite(transform[i]);
        if (sw < 0.0) { sx += sw; sw = -sw; }
        if (sh < 0.0) { sy += sh; sh = -sh; }
        if (dw < 0.0) { dx += dw; dw = -dw; }
        if (dh < 0.0) { dy += dh; dh = -dh; }
        if (clip[0] < 0) clip[0] = 0;
        if (clip[1] < 0) clip[1] = 0;
        if (clip[2] > width) clip[2] = width;
        if (clip[3] > height) clip[3] = height;
        if (!values_ok || !operation_ok || !clip_ok
            || clip[2] <= clip[0] || clip[3] <= clip[1]
            || !canvas_raster_image_pixels(
                target, width, source, source_width, source_height,
                sx, sy, sw, sh, dx, dy, dw, dh, smooth, global_alpha,
                operation, transform, clip, NULL, 0u,
                &work_remaining, &clipped)) rendered = false;
        js_free(context, source_copy);
        JS_FreeValue(context, source_buffer);
        if (!rendered) break;
    }
    for (size_t at = 0; at < command_count; at++)
        JS_FreeValue(context, source_values[at]);
    JS_FreeValue(context, command_buffer);
    JS_FreeValue(context, target_buffer);
    if (!rendered) return JS_FALSE;
    return JS_NewInt32(context, clipped ? 2 : 1);
}

/* Bootstrap-only: fill a path with a CanvasPattern into a tainted canvas's
   native store, pixel for pixel as canvas.js fills a clean canvas (a pixel
   is painted when its centre is inside the path and the clip, with the
   pattern pixel under that centre). Arguments: target, width, height,
   points (Float64Array, NaN between subpaths), evenOdd, globalAlpha,
   operation, clipRect (Float64Array [left, top, right, bottom]), clip paths
   (serializedClips), pattern source (script buffer, number, or null) and
   pattern parameters (see canvas_pattern_open). */
JSValue js_canvas_raster_pattern(JSContext *context, JSValueConst this_value,
                                 int argc, JSValueConst *argv)
{
    (void) this_value;
    int32_t width = 0, height = 0, operation = 0;
    double global_alpha = 0.0;
    if (argc < 11 || JS_ToInt32(context, &width, argv[1]) < 0
        || JS_ToInt32(context, &height, argv[2]) < 0
        || JS_ToFloat64(context, &global_alpha, argv[5]) < 0
        || JS_ToInt32(context, &operation, argv[6]) < 0) return JS_EXCEPTION;
    size_t required = 0;
    if (!canvas_dimensions(width, height, &required)
        || global_alpha < 0.0 || global_alpha > 1.0
        || operation < 1 || operation > 11) return JS_FALSE;
    bool even_odd = JS_ToBool(context, argv[4]) > 0;
    /* Nothing below runs script. */
    const uint8_t *points = NULL, *clip_bytes = NULL, *clip_paths = NULL;
    size_t coordinate_count = 0, clip_count = 0, clip_path_count = 0;
    JSValue point_buffer = JS_UNDEFINED, clip_buffer = JS_UNDEFINED;
    JSValue clip_path_buffer = JS_UNDEFINED, pixel_buffer = JS_UNDEFINED;
    uint8_t *pixels = NULL;
    bool native = false, opened = false, rendered = false;
    bool work_exhausted = false;
    CanvasPatternSource pattern;
    if (canvas_typed_bytes(context, argv[3], sizeof(double), &points,
                           &coordinate_count, &point_buffer)
        && canvas_typed_bytes(context, argv[7], sizeof(double), &clip_bytes,
                              &clip_count, &clip_buffer)
        && canvas_typed_bytes(context, argv[8], sizeof(double), &clip_paths,
                              &clip_path_count, &clip_path_buffer)
        && coordinate_count >= 4u && (coordinate_count & 1u) == 0u
        && coordinate_count <= 2048u && clip_count >= 4u
        && clip_path_count <= 8201u
        && canvas_mutable_pixels(context, argv[0], width, height, required,
                                 &pixels, &pixel_buffer, &native)) {
        opened = canvas_pattern_open(context, argv[9], argv[10], pixels,
                                     native, &pattern, NULL, 0u);
    }
    if (opened) {
        size_t point_count = coordinate_count / 2u;
        /* canvas.js's bounds: the floor/ceil extent of the points, within
           the clip rectangle (and here also the surface). */
        double left = width, top = height, right = 0.0, bottom = 0.0;
        for (size_t i = 0; i < point_count; i++) {
            double x = canvas_double_at(points, i * 2u);
            double y = canvas_double_at(points, i * 2u + 1u);
            if (!isfinite(x) || !isfinite(y)) continue;
            left = fmin(left, floor(x));
            top = fmin(top, floor(y));
            right = fmax(right, ceil(x));
            bottom = fmax(bottom, ceil(y));
        }
        double clip_rect[4];
        for (size_t i = 0; i < 4u; i++)
            clip_rect[i] = canvas_double_at(clip_bytes, i);
        left = fmax(fmax(left, clip_rect[0]), 0.0);
        top = fmax(fmax(top, clip_rect[1]), 0.0);
        right = fmin(fmin(right, clip_rect[2]), (double) width);
        bottom = fmin(fmin(bottom, clip_rect[3]), (double) height);
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        rendered = !(left < right && top < bottom)
            || (canvas_integer(left, CANVAS_INTEGER_CEIL, &x0)
                && canvas_integer(top, CANVAS_INTEGER_CEIL, &y0)
                && canvas_integer(right, CANVAS_INTEGER_CEIL, &x1)
                && canvas_integer(bottom, CANVAS_INTEGER_CEIL, &y1));
        size_t work_remaining = CANVAS_RASTER_WORK_LIMIT;
        size_t sample_cost = point_count + clip_path_count / 2u + 1u;
        for (int y = y0; rendered && y < y1 && !work_exhausted; y++) {
            for (int x = x0; x < x1; x++) {
                if (!canvas_work_take(&work_remaining, sample_cost)) {
                    work_exhausted = true;
                    break;
                }
                if (!canvas_path_inside(points, point_count, x + 0.5,
                                        y + 0.5, even_odd)
                    || !canvas_clip_stack_contains(
                        clip_paths, clip_path_count, x + 0.5, y + 0.5))
                    continue;
                int color[4];
                canvas_pattern_color(&pattern, x + 0.5, y + 0.5, color);
                /* canvas.js leaves the pixel alone for an invisible
                   source-over colour. */
                if (operation == 1
                    && ((double) color[3] / 255.0) * global_alpha <= 0.0)
                    continue;
                canvas_blend_pixel(
                    pixels, ((size_t) y * (size_t) width + (size_t) x) * 4u,
                    color[0], color[1], color[2], color[3], global_alpha,
                    operation, 1.0);
            }
        }
        canvas_pattern_close(context, &pattern);
    }
    JS_FreeValue(context, pixel_buffer);
    JS_FreeValue(context, clip_path_buffer);
    JS_FreeValue(context, clip_buffer);
    JS_FreeValue(context, point_buffer);
    if (!rendered) return JS_FALSE;
    return JS_NewInt32(context, work_exhausted ? 2 : 1);
}
