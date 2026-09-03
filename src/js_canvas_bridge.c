#include "js_runtime_internal.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define CANVAS_BRIDGE_PIXEL_BYTE_LIMIT (512u * 1024u)
#define CANVAS_RASTER_WORK_LIMIT (4u * 1024u * 1024u)

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

static bool canvas_mutable_pixels(JSContext *context, JSValueConst value,
                                  size_t required, uint8_t **pixels,
                                  JSValue *buffer)
{
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

    size_t offset = 0, length = 0, bytes_per_element = 0;
    JSValue buffer = JS_GetTypedArrayBuffer(
        context, argv[3], &offset, &length, &bytes_per_element);
    if (JS_IsException(buffer)) return buffer;
    size_t buffer_length = 0;
    const uint8_t *bytes = JS_GetArrayBuffer(
        context, &buffer_length, buffer);
    size_t required = (size_t) width * (size_t) height * 4u;
    bool valid = bytes_per_element == 1u
        && (bytes != NULL || required == 0u)
        && offset <= buffer_length
        && length <= buffer_length - offset
        && length >= required;
    ImageCanvasCommitResult committed = IMAGE_CANVAS_COMMIT_REFUSED;
    lxb_dom_node_t *commit_node = NULL;
    if (valid) {
        /* Every operation above may invoke JavaScript (including valueOf).
           Resolve the generation-bearing handle only after those coercions,
           immediately before the native commit. */
        size_t slot = 0;
        if (js_rt_bridge_node_slot_for_handle(bridge, handle, &slot))
            commit_node = bridge->nodes[slot];
        if (commit_node == NULL || !bridge_node_is_connected(commit_node)) {
            JS_FreeValue(context, buffer);
            return JS_FALSE;
        }
        committed = images_commit_canvas_surface(
            bridge->images, bridge->budget, commit_node,
            bytes + offset, length,
            width, height, left, top, right, bottom);
    }
    JS_FreeValue(context, buffer);
    if (committed == IMAGE_CANVAS_COMMIT_REFUSED) return JS_FALSE;
    js_rt_bridge_note_canvas_mutation(
        bridge, commit_node, committed == IMAGE_CANVAS_COMMIT_UPDATED);
    return JS_TRUE;
}

JSValue js_canvas_image_source(JSContext *context,
                               JSValueConst this_value,
                               int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || bridge->images == NULL || argc < 1) return JS_NULL;
    lxb_dom_node_t *node = js_rt_bridge_node_arg(
        context, bridge, argv[0]);
    const ImageResource *image = node == NULL ? NULL
        : images_find_node(bridge->images, node);
    if (!image_resource_available(image) || image->pixels == NULL
        || image->width <= 0 || image->height <= 0
        || (size_t) image->width > SIZE_MAX / (size_t) image->height
        || (size_t) image->width * (size_t) image->height > SIZE_MAX / 4u) {
        return JS_NULL;
    }
    size_t length = (size_t) image->width * (size_t) image->height * 4u;
    if (length > CANVAS_BRIDGE_PIXEL_BYTE_LIMIT) return JS_NULL;
    JSValue result = JS_NewObject(context);
    if (JS_IsException(result)) return result;
    if (JS_SetPropertyStr(context, result, "width",
                          JS_NewInt32(context, image->width)) < 0
        || JS_SetPropertyStr(context, result, "height",
                             JS_NewInt32(context, image->height)) < 0
        || JS_SetPropertyStr(context, result, "sameOrigin",
                             JS_NewBool(context,
                                        !image->cross_origin)) < 0) {
        JS_FreeValue(context, result);
        return JS_EXCEPTION;
    }
    JSValue pixels = JS_NewArrayBufferCopy(context, image->pixels, length);
    if (JS_IsException(pixels)) {
        JS_FreeValue(context, result);
        return pixels;
    }
    /* JS_SetPropertyStr consumes pixels on both success and failure. */
    if (JS_SetPropertyStr(context, result, "pixels", pixels) < 0) {
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
    uint8_t *pixels = NULL;
    JSValue buffer = JS_UNDEFINED;
    if (!canvas_mutable_pixels(context, argv[0], required, &pixels, &buffer)) {
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
        JS_FreeValue(context, buffer);
        return JS_TRUE;
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
    if (!canvas_mutable_pixels(
            context, argv[0], required, &pixels, &pixel_buffer)) {
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
        for (int y = top; y < bottom && !work_exhausted; y++) {
            for (int x = left; x < right; x++) {
                if (!canvas_work_take(&work_remaining, 1u)) {
                    work_exhausted = true;
                    break;
                }
                size_t at = ((size_t) y * (size_t) width + (size_t) x) * 4u;
                canvas_blend_pixel(pixels, at, red, green, blue, alpha,
                                   global_alpha, operation, 1.0);
            }
        }
        if (work_exhausted) break;
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
    if (JS_SetPropertyStr(context, result, "width",
                          JS_NewFloat64(context,
                                       (double) width_fixed / 64.0)) < 0
        || JS_SetPropertyStr(context, result, "ascent",
                             JS_NewFloat64(context, (double) ascent)) < 0
        || JS_SetPropertyStr(context, result, "descent",
                             JS_NewFloat64(context, (double) descent)) < 0) {
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
    const FontFace *face = canvas_font_face(bridge, family, italic, bold);
    if (face == NULL) return JS_FALSE;
    uint8_t *pixels = NULL;
    JSValue pixel_buffer = JS_UNDEFINED;
    if (!canvas_mutable_pixels(
            context, argv[0], required, &pixels, &pixel_buffer)) {
        if (JS_IsException(pixel_buffer)) return pixel_buffer;
        return JS_FALSE;
    }
    double transform[6];
    int clip[4] = {0};
    JSValue transform_buffer = JS_UNDEFINED, clip_buffer = JS_UNDEFINED;
    if (!canvas_read_transform_clip(
            context, argv[18], argv[19], transform, clip, width, height,
            &transform_buffer, &clip_buffer)) {
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
        JS_FreeValue(context, clip_path_buffer);
        JS_FreeValue(context, clip_buffer);
        JS_FreeValue(context, transform_buffer);
        JS_FreeValue(context, pixel_buffer);
        return JS_FALSE;
    }
    size_t text_length = 0;
    const char *text = JS_ToCStringLen(context, &text_length, argv[3]);
    if (text == NULL) {
        JS_FreeValue(context, clip_path_buffer);
        JS_FreeValue(context, clip_buffer);
        JS_FreeValue(context, transform_buffer);
        JS_FreeValue(context, pixel_buffer);
        return JS_EXCEPTION;
    }
    if (text_length > 1024u) text_length = 1024u;
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

JSValue js_canvas_raster_text(JSContext *context,
                              JSValueConst this_value,
                              int argc, JSValueConst *argv)
{
    size_t work_remaining = CANVAS_RASTER_WORK_LIMIT;
    return canvas_raster_text_with_work(
        context, this_value, argc, argv, &work_remaining, NULL);
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
    if (!canvas_mutable_pixels(
            context, argv[0], required, &pixels, &pixel_buffer)) {
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
        int payload_count = kind == 0 ? 18 : kind == 1 ? 19 : 0;
        JSValue call_args[22];
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
                      context, JS_UNDEFINED, 22, call_args, &work_remaining,
                      &command_clipped);
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
        || operation < 1 || operation > 11) return JS_FALSE;
    if (sw < 0.0) { sx += sw; sw = -sw; }
    if (sh < 0.0) { sy += sh; sh = -sh; }
    if (dw < 0.0) { dx += dw; dw = -dw; }
    if (dh < 0.0) { dy += dh; dh = -dh; }
    uint8_t *target = NULL;
    JSValue target_buffer = JS_UNDEFINED;
    if (!canvas_mutable_pixels(
            context, argv[0], required, &target, &target_buffer)) {
        if (JS_IsException(target_buffer)) return target_buffer;
        return JS_FALSE;
    }
    const uint8_t *source = NULL;
    size_t source_count = 0;
    JSValue source_buffer = JS_UNDEFINED;
    if (!canvas_typed_bytes(context, argv[3], 1u, &source, &source_count,
                            &source_buffer)
        || source_count < source_required) {
        JS_FreeValue(context, target_buffer);
        if (JS_IsException(source_buffer)) return source_buffer;
        return JS_FALSE;
    }
    double transform[6];
    int clip[4];
    JSValue transform_buffer = JS_UNDEFINED, clip_buffer = JS_UNDEFINED;
    if (!canvas_read_transform_clip(
            context, argv[17], argv[18], transform, clip, width, height,
            &transform_buffer, &clip_buffer)) {
        JS_FreeValue(context, source_buffer);
        JS_FreeValue(context, target_buffer);
        return JS_FALSE;
    }
    const uint8_t *clip_paths = NULL;
    size_t clip_path_count = 0;
    JSValue clip_path_buffer = JS_UNDEFINED;
    if (!canvas_typed_bytes(
            context, argv[19], sizeof(double), &clip_paths,
            &clip_path_count, &clip_path_buffer)
        || clip_path_count > 8201u) {
        JS_FreeValue(context, clip_path_buffer);
        JS_FreeValue(context, clip_buffer);
        JS_FreeValue(context, transform_buffer);
        JS_FreeValue(context, source_buffer);
        JS_FreeValue(context, target_buffer);
        return JS_FALSE;
    }
    bool smooth = JS_ToBool(context, argv[14]) > 0;
    size_t work_remaining = CANVAS_RASTER_WORK_LIMIT;
    bool clipped = false;
    bool rendered = canvas_raster_image_pixels(
        target, width, source, source_width, source_height,
        sx, sy, sw, sh, dx, dy, dw, dh, smooth, global_alpha, operation,
        transform, clip, clip_paths, clip_path_count, &work_remaining,
        &clipped);
    JS_FreeValue(context, clip_path_buffer);
    JS_FreeValue(context, clip_buffer);
    JS_FreeValue(context, transform_buffer);
    JS_FreeValue(context, source_buffer);
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
    uint8_t *target = NULL;
    JSValue target_buffer = JS_UNDEFINED;
    if (!canvas_mutable_pixels(
            context, argv[0], required, &target, &target_buffer)) {
        if (JS_IsException(target_buffer)) return target_buffer;
        return JS_FALSE;
    }
    const uint8_t *commands = NULL;
    size_t command_values = 0;
    JSValue command_buffer = JS_UNDEFINED;
    if (!canvas_typed_bytes(
            context, argv[4], sizeof(double), &commands,
            &command_values, &command_buffer)
        || command_values == 0u || command_values % 24u != 0u
        || command_values > 16u * 24u) {
        JS_FreeValue(context, command_buffer);
        JS_FreeValue(context, target_buffer);
        return JS_FALSE;
    }
    bool rendered = true, clipped = false;
    size_t work_remaining = CANVAS_RASTER_WORK_LIMIT;
    for (size_t command = 0; command < command_values / 24u; command++) {
        size_t base = command * 24u;
        double raw_source_index = canvas_double_at(commands, base);
        if (!isfinite(raw_source_index) || raw_source_index < 0.0
            || raw_source_index > 15.0
            || floor(raw_source_index) != raw_source_index) {
            rendered = false;
            break;
        }
        JSValue source_value = JS_GetPropertyUint32(
            context, argv[3], (uint32_t) raw_source_index);
        if (JS_IsException(source_value)) {
            rendered = false;
            break;
        }
        const uint8_t *source = NULL;
        size_t source_count = 0;
        JSValue source_buffer = JS_UNDEFINED;
        double raw_source_width = canvas_double_at(commands, base + 1u);
        double raw_source_height = canvas_double_at(commands, base + 2u);
        int source_width = 0, source_height = 0;
        bool source_dimensions_ok = raw_source_width >= 1.0
            && raw_source_height >= 1.0
            && canvas_integer(raw_source_width, CANVAS_INTEGER_TRUNCATE,
                              &source_width)
            && canvas_integer(raw_source_height, CANVAS_INTEGER_TRUNCATE,
                              &source_height);
        size_t source_required = 0;
        bool source_ok = source_dimensions_ok && canvas_dimensions(
                source_width, source_height, &source_required)
            && canvas_typed_bytes(
                context, source_value, 1u, &source, &source_count,
                &source_buffer)
            && source_count >= source_required;
        if (!source_ok) {
            JS_FreeValue(context, source_buffer);
            JS_FreeValue(context, source_value);
            rendered = false;
            break;
        }
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
        JS_FreeValue(context, source_buffer);
        JS_FreeValue(context, source_value);
        if (!rendered) break;
    }
    JS_FreeValue(context, command_buffer);
    JS_FreeValue(context, target_buffer);
    if (!rendered) return JS_FALSE;
    return JS_NewInt32(context, clipped ? 2 : 1);
}
