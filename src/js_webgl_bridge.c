#include "js_runtime_internal.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#if defined(__PSP__)
#include "tilefinch/psp_display.h"
#include "tilefinch/psp_media_present.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include <pspge.h>
#include <pspgu.h>
#include <pspgum.h>
#include <pspkernel.h>
#pragma GCC diagnostic pop
#endif

/* This is intentionally a command translator, not a software implementation
   of GLSL. webgl.js admits a small fixed-function-compatible shader shape and
   serializes bounded draws here. Untrusted values are revalidated at this
   boundary before either the host rasterizer or PSP GE sees them. */
#define WEBGL_COMMAND_WORDS 64u
#define WEBGL_TEXTURE_WORDS 9u
#define WEBGL_WIRE_HEADER_WORDS 4u
#define WEBGL_COMMAND_WIRE_MAGIC UINT32_C(0x54465743)
#define WEBGL_TEXTURE_WIRE_MAGIC UINT32_C(0x54465754)
#define WEBGL_WIRE_VERSION 1u
#define WEBGL_COMMAND_LIMIT 64u
#define WEBGL_SOURCE_LIMIT 32u
#define WEBGL_TEXTURE_LIMIT 8u
#define WEBGL_VERTEX_LIMIT 4096u
#define WEBGL_PIXEL_LIMIT 131072u
#define WEBGL_RASTER_WORK_LIMIT (4u * 1024u * 1024u)

_Static_assert(WEBGL_SOURCE_LIMIT == 24u + WEBGL_TEXTURE_LIMIT,
               "24 buffer payloads plus eight texture payloads fill sources");
_Static_assert(sizeof(float) == sizeof(uint32_t),
               "the WebGL wire requires 32-bit native float words");

/* A process-wide incarnation is security authority, not page state.  Keep it
   as two 32-bit words protected by a tiny creation-time lock: Allegrex has no
   atomic 64-bit load/store, while realm creation is far outside frame paths. */
static atomic_flag webgl_realm_epoch_lock = ATOMIC_FLAG_INIT;
static uint32_t webgl_realm_epoch_next_high;
static uint32_t webgl_realm_epoch_next_low;

bool js_webgl_realm_epoch_advance(DomBridge *bridge)
{
    if (bridge == NULL) return false;
    bool locked = false;
    for (size_t attempt = 0; attempt < 1024u; attempt++) {
        if (!atomic_flag_test_and_set_explicit(
                &webgl_realm_epoch_lock, memory_order_acquire)) {
            locked = true;
            break;
        }
    }
    if (!locked) return false;
    if (webgl_realm_epoch_next_high == UINT32_MAX
        && webgl_realm_epoch_next_low == UINT32_MAX) {
        atomic_flag_clear_explicit(
            &webgl_realm_epoch_lock, memory_order_release);
        return false;
    }
    webgl_realm_epoch_next_low++;
    if (webgl_realm_epoch_next_low == 0u) webgl_realm_epoch_next_high++;
    bridge->webgl_realm_epoch_high = webgl_realm_epoch_next_high;
    bridge->webgl_realm_epoch_low = webgl_realm_epoch_next_low;
    atomic_flag_clear_explicit(
        &webgl_realm_epoch_lock, memory_order_release);
    return bridge->webgl_realm_epoch_high != 0u
        || bridge->webgl_realm_epoch_low != 0u;
}

bool script_runtime_webgl_cache_admit(
    ScriptWebglCacheAdmission *state,
    uint32_t realm_epoch_high,
    uint32_t realm_epoch_low,
    int64_t canvas_handle,
    bool *reset)
{
    if (state == NULL || reset == NULL
        || (realm_epoch_high == 0u && realm_epoch_low == 0u)) return false;
    *reset = !state->valid
        || state->realm_epoch_high != realm_epoch_high
        || state->realm_epoch_low != realm_epoch_low
        || state->canvas_handle != canvas_handle;
    if (*reset) {
        state->realm_epoch_high = realm_epoch_high;
        state->realm_epoch_low = realm_epoch_low;
        state->canvas_handle = canvas_handle;
        state->cached_entries = 0;
        state->cached_bytes = 0;
        state->valid = true;
    }
    return true;
}

enum {
    WEBGL_POINTS = 0,
    WEBGL_LINES = 1,
    WEBGL_LINE_LOOP = 2,
    WEBGL_LINE_STRIP = 3,
    WEBGL_TRIANGLES = 4,
    WEBGL_TRIANGLE_STRIP = 5,
    WEBGL_TRIANGLE_FAN = 6,
    WEBGL_BYTE = 0x1400,
    WEBGL_UNSIGNED_BYTE = 0x1401,
    WEBGL_SHORT = 0x1402,
    WEBGL_UNSIGNED_SHORT = 0x1403,
    WEBGL_FLOAT = 0x1406,
    WEBGL_DEPTH_BUFFER_BIT = 0x0100,
    WEBGL_COLOR_BUFFER_BIT = 0x4000,
    WEBGL_SRC_ALPHA = 0x0302,
    WEBGL_ONE_MINUS_SRC_ALPHA = 0x0303,
    WEBGL_ONE = 1,
    WEBGL_ZERO = 0
};

typedef struct {
    const uint8_t *bytes;
    size_t length;
    JSValue buffer;
} WebglSource;

typedef struct {
    uint32_t identifier;
    uint32_t generation;
    int width;
    int height;
    int source;
    int minimum_filter;
    int magnification_filter;
    bool repeat_s;
    bool repeat_t;
} WebglTexture;

typedef struct {
    float x;
    float y;
    float z;
    float inverse_w;
    float object_x;
    float object_y;
    float object_z;
    float u;
    float v;
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    uint8_t alpha;
    bool geometry_valid;
} WebglVertex;

typedef struct {
    const WebglSource *source;
    size_t stride;
    size_t offset;
    int size;
    int type;
    bool normalized;
} WebglAttributeView;

typedef struct {
    const WebglSource *index_source;
    WebglAttributeView position;
    WebglAttributeView color;
    WebglAttributeView texcoord;
    float matrix[16];
    float uniform[4];
    uint32_t first;
    size_t index_offset;
    int index_type;
    int viewport_x;
    int viewport_y;
    int viewport_width;
    int viewport_height;
    bool indexed;
} WebglDecodedDraw;

typedef enum {
    WEBGL_JS_READ_INVALID = 0,
    WEBGL_JS_READ_OK,
    WEBGL_JS_READ_EXCEPTION
} WebglJsReadResult;

static uint32_t webgl_wire_u32(const uint8_t *bytes, size_t index)
{
    uint32_t value = 0;
    memcpy(&value, bytes + index * sizeof(value), sizeof(value));
    return value;
}

static int32_t webgl_wire_i32(const uint8_t *bytes, size_t index)
{
    int32_t value = 0;
    memcpy(&value, bytes + index * sizeof(value), sizeof(value));
    return value;
}

static float webgl_wire_float(const uint8_t *bytes, size_t index)
{
    float value = 0.0f;
    memcpy(&value, bytes + index * sizeof(value), sizeof(value));
    return value;
}

static bool webgl_integer_in_range(int32_t value, int minimum, int maximum,
                                   int *output)
{
    if (output == NULL || value < minimum || value > maximum) return false;
    *output = (int) value;
    return true;
}

static bool webgl_wire_header(const uint8_t *bytes, size_t word_count,
                              uint32_t magic, size_t record_words,
                              size_t record_limit, size_t *record_count)
{
    if (bytes == NULL || record_count == NULL
        || word_count < WEBGL_WIRE_HEADER_WORDS
        || webgl_wire_u32(bytes, 0u) != magic
        || webgl_wire_u32(bytes, 1u) != WEBGL_WIRE_VERSION
        || webgl_wire_u32(bytes, 2u) != record_words) return false;
    uint32_t count = webgl_wire_u32(bytes, 3u);
    if (count > record_limit
        || count > (word_count - WEBGL_WIRE_HEADER_WORDS) / record_words)
        return false;
    *record_count = count;
    return true;
}

static WebglJsReadResult webgl_typed_values(
    JSContext *context, JSValueConst value, size_t element_size,
    const uint8_t **bytes, size_t *count, JSValue *buffer)
{
    size_t offset = 0, length = 0, bytes_per_element = 0;
    *buffer = JS_GetTypedArrayBuffer(
        context, value, &offset, &length, &bytes_per_element);
    if (JS_IsException(*buffer)) return WEBGL_JS_READ_EXCEPTION;
    size_t buffer_length = 0;
    uint8_t *storage = JS_GetArrayBuffer(context, &buffer_length, *buffer);
    if (storage == NULL || bytes_per_element != element_size
        || offset > buffer_length || length > buffer_length - offset
        || length % element_size != 0u) {
        JS_FreeValue(context, *buffer);
        *buffer = JS_UNDEFINED;
        return WEBGL_JS_READ_INVALID;
    }
    *bytes = storage + offset;
    *count = length / element_size;
    return WEBGL_JS_READ_OK;
}

static void webgl_sources_release(JSContext *context,
                                  WebglSource sources[WEBGL_SOURCE_LIMIT],
                                  size_t count)
{
    for (size_t i = 0; i < count; i++) JS_FreeValue(context, sources[i].buffer);
}

static WebglJsReadResult webgl_sources_read(
    JSContext *context, JSValueConst value,
    WebglSource sources[WEBGL_SOURCE_LIMIT], size_t *count)
{
    *count = 0;
    JSValue length_value = JS_GetPropertyStr(context, value, "length");
    uint32_t length = 0;
    if (JS_IsException(length_value)) return WEBGL_JS_READ_EXCEPTION;
    if (JS_ToUint32(context, &length, length_value) < 0) {
        JS_FreeValue(context, length_value);
        return WEBGL_JS_READ_EXCEPTION;
    }
    JS_FreeValue(context, length_value);
    if (length > WEBGL_SOURCE_LIMIT) return WEBGL_JS_READ_INVALID;
    for (uint32_t i = 0; i < length; i++) {
        JSValue entry = JS_GetPropertyUint32(context, value, i);
        if (JS_IsException(entry)) {
            webgl_sources_release(context, sources, *count);
            *count = 0;
            return WEBGL_JS_READ_EXCEPTION;
        }
        size_t offset = 0, byte_length = 0, bytes_per_element = 0;
        JSValue buffer = JS_GetTypedArrayBuffer(
            context, entry, &offset, &byte_length, &bytes_per_element);
        JS_FreeValue(context, entry);
        if (JS_IsException(buffer)) {
            webgl_sources_release(context, sources, *count);
            *count = 0;
            return WEBGL_JS_READ_EXCEPTION;
        }
        size_t storage_length = 0;
        uint8_t *storage = JS_GetArrayBuffer(context, &storage_length, buffer);
        if (storage == NULL || bytes_per_element == 0u
            || offset > storage_length
            || byte_length > storage_length - offset) {
            JS_FreeValue(context, buffer);
            webgl_sources_release(context, sources, *count);
            *count = 0;
            return WEBGL_JS_READ_INVALID;
        }
        sources[*count] = (WebglSource) {
            .bytes = storage + offset,
            .length = byte_length,
            .buffer = buffer
        };
        (*count)++;
    }
    return WEBGL_JS_READ_OK;
}

static bool webgl_textures_read(const uint8_t *values, size_t value_count,
                                const WebglSource *sources,
                                size_t source_count,
                                WebglTexture textures[WEBGL_TEXTURE_LIMIT],
                                size_t *texture_count)
{
    if (value_count % WEBGL_TEXTURE_WORDS != 0u
        || value_count / WEBGL_TEXTURE_WORDS > WEBGL_TEXTURE_LIMIT) {
        return false;
    }
    *texture_count = value_count / WEBGL_TEXTURE_WORDS;
    size_t retained = 0;
    for (size_t i = 0; i < *texture_count; i++) {
        size_t at = i * WEBGL_TEXTURE_WORDS;
        int identifier = 0, generation = 0, width = 0, height = 0;
        int source = 0, minimum = 0, magnification = 0;
        int repeat_s = 0, repeat_t = 0;
        if (!webgl_integer_in_range(webgl_wire_i32(values, at), 1, INT_MAX,
                                  &identifier)
            || !webgl_integer_in_range(webgl_wire_i32(values, at + 1u),
                                     1, INT_MAX, &generation)
            || !webgl_integer_in_range(webgl_wire_i32(values, at + 2u),
                                     1, 512, &width)
            || !webgl_integer_in_range(webgl_wire_i32(values, at + 3u),
                                     1, 512, &height)
            || !webgl_integer_in_range(webgl_wire_i32(values, at + 4u),
                                     0, (int) source_count - 1, &source)
            || !webgl_integer_in_range(webgl_wire_i32(values, at + 5u),
                                     0, INT_MAX, &minimum)
            || !webgl_integer_in_range(webgl_wire_i32(values, at + 6u),
                                     0, INT_MAX, &magnification)
            || !webgl_integer_in_range(webgl_wire_i32(values, at + 7u),
                                     0, 1, &repeat_s)
            || !webgl_integer_in_range(webgl_wire_i32(values, at + 8u),
                                     0, 1, &repeat_t)) return false;
        size_t pixels = (size_t) width * (size_t) height;
        if (pixels > SIZE_MAX / 4u || pixels * 4u > sources[source].length
            || retained > 416u * 1024u
            || pixels * 4u > 416u * 1024u - retained) return false;
        retained += pixels * 4u;
        textures[i] = (WebglTexture) {
            .identifier = (uint32_t) identifier,
            .generation = (uint32_t) generation,
            .width = width,
            .height = height,
            .source = source,
            .minimum_filter = minimum,
            .magnification_filter = magnification,
            .repeat_s = repeat_s != 0,
            .repeat_t = repeat_t != 0
        };
    }
    return true;
}

static bool webgl_source_scalar(const WebglSource *source, int type,
                                size_t offset, bool normalized, float *value)
{
    if (source == NULL || value == NULL) return false;
    if (type == WEBGL_FLOAT) {
        if (offset > source->length || sizeof(float) > source->length - offset)
            return false;
        memcpy(value, source->bytes + offset, sizeof(*value));
        /* NaN/Inf vertex data is valid buffer content. The primitive decoder
           discards affected geometry without treating the command contract
           as corrupt. */
        return true;
    }
    if (type == WEBGL_BYTE || type == WEBGL_UNSIGNED_BYTE) {
        if (offset >= source->length) return false;
        if (type == WEBGL_BYTE) {
            int8_t signed_value = 0;
            memcpy(&signed_value, source->bytes + offset, sizeof(signed_value));
            *value = normalized
                ? fmaxf(-1.0f, (float) signed_value / 127.0f)
                : (float) signed_value;
        } else {
            *value = normalized ? (float) source->bytes[offset] / 255.0f
                                : (float) source->bytes[offset];
        }
        return true;
    }
    if (type == WEBGL_SHORT || type == WEBGL_UNSIGNED_SHORT) {
        if (offset > source->length || 2u > source->length - offset)
            return false;
        uint16_t raw = 0;
        memcpy(&raw, source->bytes + offset, sizeof(raw));
        if (type == WEBGL_SHORT) {
            int16_t signed_raw = 0;
            memcpy(&signed_raw, &raw, sizeof(signed_raw));
            *value = normalized ? fmaxf(-1.0f, (float) signed_raw / 32767.0f)
                                : (float) signed_raw;
        } else {
            *value = normalized ? (float) raw / 65535.0f : (float) raw;
        }
        return true;
    }
    return false;
}

static size_t webgl_type_bytes(int type)
{
    if (type == WEBGL_BYTE || type == WEBGL_UNSIGNED_BYTE) return 1u;
    if (type == WEBGL_SHORT || type == WEBGL_UNSIGNED_SHORT) return 2u;
    if (type == WEBGL_FLOAT) return 4u;
    return 0u;
}

static uint8_t webgl_channel_byte(float value)
{
    if (!isfinite(value) || value <= 0.0f) return 0;
    if (value >= 255.0f) return 255;
    return (uint8_t) value;
}

static uint8_t webgl_color_byte(float value)
{
    return webgl_channel_byte(value * 255.0f + 0.5f);
}

static bool webgl_attribute_view(
    const uint8_t *command, size_t slot,
    const WebglSource *sources, size_t source_count,
    WebglAttributeView *view)
{
    int source = 0, size = 0, type = 0, normalized = 0;
    int stride = 0, offset = 0;
    if (view == NULL
        || !webgl_integer_in_range(webgl_wire_i32(command, slot), -1,
                              (int) source_count - 1, &source)
        || !webgl_integer_in_range(webgl_wire_i32(command, slot + 1u),
                                 0, 4, &size)
        || !webgl_integer_in_range(webgl_wire_i32(command, slot + 2u),
                                 0, INT_MAX, &type)
        || !webgl_integer_in_range(webgl_wire_i32(command, slot + 3u),
                                 0, 1, &normalized)
        || !webgl_integer_in_range(webgl_wire_i32(command, slot + 4u),
                                 0, 255, &stride)
        || !webgl_integer_in_range(webgl_wire_i32(command, slot + 5u),
                                 0, INT_MAX, &offset)) return false;
    *view = (WebglAttributeView) {
        .source = source < 0 ? NULL : &sources[source],
        .offset = (size_t) offset,
        .size = size,
        .type = type,
        .normalized = normalized != 0
    };
    if (source < 0 || size == 0) return true;
    size_t scalar_bytes = webgl_type_bytes(type);
    if (scalar_bytes == 0u) return false;
    view->stride = stride != 0 ? (size_t) stride
                               : scalar_bytes * (size_t) size;
    return true;
}

static bool webgl_attribute_from_view(
    const WebglAttributeView *view, uint32_t vertex,
    float output[4], const float fallback[4])
{
    if (view == NULL || output == NULL || fallback == NULL) return false;
    memcpy(output, fallback, sizeof(float) * 4u);
    if (view->source == NULL || view->size == 0) return true;
    size_t scalar_bytes = webgl_type_bytes(view->type);
    if (scalar_bytes == 0u || view->stride == 0u
        || (size_t) vertex > (SIZE_MAX - view->offset) / view->stride)
        return false;
    size_t base = view->offset + (size_t) vertex * view->stride;
    for (int component = 0; component < view->size; component++) {
        size_t component_offset = base + (size_t) component * scalar_bytes;
        if (component_offset < base
            || !webgl_source_scalar(view->source, view->type,
                                    component_offset, view->normalized,
                                    &output[component])) return false;
    }
    return true;
}

static bool webgl_decode_draw(const uint8_t *command,
                              const WebglSource *sources,
                              size_t source_count,
                              int surface_width, int surface_height,
                              WebglDecodedDraw *draw)
{
    int indexed = 0, first = 0, index_source = 0, index_type = 0;
    int index_offset = 0;
    if (draw == NULL
        || !webgl_integer_in_range(webgl_wire_i32(command, 2u), 0,
                              INT_MAX, &first)
        || !webgl_integer_in_range(webgl_wire_i32(command, 4u), 0, 1,
                                 &indexed)) return false;
    *draw = (WebglDecodedDraw) {
        .first = (uint32_t) first,
        .indexed = indexed != 0
    };
    if (indexed) {
        if (!webgl_integer_in_range(webgl_wire_i32(command, 5u), 0,
                                  (int) source_count - 1, &index_source)
            || !webgl_integer_in_range(webgl_wire_i32(command, 6u), 0,
                                     INT_MAX, &index_type)
            || !webgl_integer_in_range(webgl_wire_i32(command, 7u), 0,
                                     INT_MAX, &index_offset)
            || (index_type != WEBGL_UNSIGNED_BYTE
                && index_type != WEBGL_UNSIGNED_SHORT)) return false;
        draw->index_source = &sources[index_source];
        draw->index_type = index_type;
        draw->index_offset = (size_t) index_offset;
    }
    if (!webgl_attribute_view(
            command, 8u, sources, source_count, &draw->position)
        || !webgl_attribute_view(
            command, 14u, sources, source_count, &draw->color)
        || !webgl_attribute_view(
            command, 20u, sources, source_count, &draw->texcoord)
        || !webgl_integer_in_range(webgl_wire_i32(command, 35u), INT_MIN,
                                 INT_MAX, &draw->viewport_x)
        || !webgl_integer_in_range(webgl_wire_i32(command, 36u), INT_MIN,
                                 INT_MAX, &draw->viewport_y)
        || !webgl_integer_in_range(webgl_wire_i32(command, 37u), 0,
                                 surface_width, &draw->viewport_width)
        || !webgl_integer_in_range(webgl_wire_i32(command, 38u), 0,
                                 surface_height, &draw->viewport_height)) {
        return false;
    }
    for (size_t at = 0; at < 16u; at++) {
        float raw = webgl_wire_float(command, 44u + at);
        if (!isfinite(raw)) return false;
        draw->matrix[at] = raw;
    }
    for (size_t at = 0; at < 4u; at++) {
        float raw = webgl_wire_float(command, 60u + at);
        if (!isfinite(raw)) return false;
        draw->uniform[at] = raw;
    }
    return true;
}

static bool webgl_draw_vertex_index(const WebglDecodedDraw *draw,
                                    uint32_t sequence, uint32_t *vertex)
{
    if (draw == NULL || vertex == NULL) return false;
    if (!draw->indexed) {
        if (draw->first > UINT32_MAX - sequence) return false;
        *vertex = draw->first + sequence;
        return true;
    }
    size_t element = draw->index_type == WEBGL_UNSIGNED_BYTE ? 1u : 2u;
    if (sequence > (SIZE_MAX - draw->index_offset) / element) return false;
    size_t at = draw->index_offset + (size_t) sequence * element;
    if (at > draw->index_source->length
        || element > draw->index_source->length - at) return false;
    if (element == 1u) *vertex = draw->index_source->bytes[at];
    else {
        uint16_t value = 0;
        memcpy(&value, draw->index_source->bytes + at, sizeof(value));
        *vertex = value;
    }
    return true;
}

static bool webgl_decode_vertex(const WebglDecodedDraw *draw,
                                uint32_t sequence,
                                int surface_height,
                                WebglVertex *output)
{
    uint32_t index = 0;
    float position[4], color[4], texcoord[4];
    static const float position_fallback[4] = {0, 0, 0, 1};
    static const float color_fallback[4] = {1, 1, 1, 1};
    static const float texcoord_fallback[4] = {0, 0, 0, 1};
    if (output == NULL) return false;
    *output = (WebglVertex) {0};
    if (!webgl_draw_vertex_index(draw, sequence, &index)
        || !webgl_attribute_from_view(
            &draw->position, index, position, position_fallback)
        || !webgl_attribute_from_view(
            &draw->color, index, color, color_fallback)
        || !webgl_attribute_from_view(
            &draw->texcoord, index, texcoord, texcoord_fallback)) return false;
    for (int component = 0; component < 4; component++) {
        if (!isfinite(position[component]) || !isfinite(color[component])
            || !isfinite(texcoord[component])) return true;
    }
    /* The GE vertex format carries xyz and an implicit object-space w of 1.
       Keep host and PSP behavior identical by refusing the uncommon authored
       non-unit position w; perspective generated by matrices remains fully
       supported. */
    if (fabsf(position[3] - 1.0f) > 0.000001f) return true;
    float transformed[4] = {0, 0, 0, 0};
    for (int row = 0; row < 4; row++) {
        for (int column = 0; column < 4; column++) {
            transformed[row] += draw->matrix[(size_t) column * 4u
                                              + (size_t) row]
                                * position[column];
        }
    }
    for (int component = 0; component < 4; component++)
        if (!isfinite(transformed[component])) return true;
    if (transformed[3] < 0.000001f
        || transformed[2] < -transformed[3]
        || transformed[2] > transformed[3]) return true;
    float ndc_x = transformed[0] / transformed[3];
    float ndc_y = transformed[1] / transformed[3];
    float ndc_z = transformed[2] / transformed[3];
    output->x = (float) draw->viewport_x
        + (ndc_x + 1.0f) * 0.5f * (float) draw->viewport_width;
    output->y = (float) surface_height
        - ((float) draw->viewport_y
           + (ndc_y + 1.0f) * 0.5f * (float) draw->viewport_height);
    output->z = (ndc_z + 1.0f) * 0.5f;
    output->inverse_w = 1.0f / transformed[3];
    output->object_x = position[0] / position[3];
    output->object_y = position[1] / position[3];
    output->object_z = position[2] / position[3];
    output->u = texcoord[0]; output->v = texcoord[1];
    output->red = webgl_color_byte(color[0] * draw->uniform[0]);
    output->green = webgl_color_byte(color[1] * draw->uniform[1]);
    output->blue = webgl_color_byte(color[2] * draw->uniform[2]);
    output->alpha = webgl_color_byte(color[3] * draw->uniform[3]);
    output->geometry_valid = isfinite(output->x) && isfinite(output->y)
        && isfinite(output->z) && isfinite(output->inverse_w)
        && isfinite(output->object_x) && isfinite(output->object_y)
        && isfinite(output->object_z)
        && output->x >= -1048576.0f && output->x <= 1048576.0f
        && output->y >= -1048576.0f && output->y <= 1048576.0f
        && output->z >= -1048576.0f && output->z <= 1048576.0f;
    return true;
}

static bool webgl_blend_factors(const uint8_t *command, bool blend,
                                int *source, int *destination)
{
    if (source == NULL || destination == NULL) return false;
    *source = WEBGL_ONE;
    *destination = WEBGL_ZERO;
    if (!blend) return true;
    if (!webgl_integer_in_range(webgl_wire_i32(command, 31u), 0, INT_MAX,
                              source)
        || !webgl_integer_in_range(webgl_wire_i32(command, 32u), 0, INT_MAX,
                                 destination)) return false;
    bool source_valid = *source == WEBGL_ZERO || *source == WEBGL_ONE
        || *source == WEBGL_SRC_ALPHA
        || *source == WEBGL_ONE_MINUS_SRC_ALPHA;
    bool destination_valid = *destination == WEBGL_ZERO
        || *destination == WEBGL_ONE || *destination == WEBGL_SRC_ALPHA
        || *destination == WEBGL_ONE_MINUS_SRC_ALPHA;
    return source_valid && destination_valid;
}

static float webgl_blend_weight(int factor, float source_alpha)
{
    if (factor == WEBGL_ZERO) return 0.0f;
    if (factor == WEBGL_ONE) return 1.0f;
    if (factor == WEBGL_SRC_ALPHA) return source_alpha;
    return 1.0f - source_alpha;
}

static void webgl_pixel(uint8_t *surface, int width, int height, int x, int y,
                        float red, float green, float blue, float alpha,
                        bool blend, int source_factor,
                        int destination_factor)
{
    if (x < 0 || y < 0 || x >= width || y >= height) return;
    size_t at = ((size_t) y * (size_t) width + (size_t) x) * 4u;
    float source_alpha = fmaxf(0.0f, fminf(1.0f, alpha / 255.0f));
    if (blend) {
        float source_weight = webgl_blend_weight(
            source_factor, source_alpha);
        float destination_weight = webgl_blend_weight(
            destination_factor, source_alpha);
        surface[at] = webgl_channel_byte(
            red * source_weight
                + (float) surface[at] * destination_weight + 0.5f);
        surface[at + 1u] = webgl_channel_byte(
            green * source_weight
                + (float) surface[at + 1u] * destination_weight + 0.5f);
        surface[at + 2u] = webgl_channel_byte(
            blue * source_weight
                + (float) surface[at + 2u] * destination_weight + 0.5f);
        surface[at + 3u] = 255u;
    } else {
        surface[at] = webgl_channel_byte(red);
        surface[at + 1u] = webgl_channel_byte(green);
        surface[at + 2u] = webgl_channel_byte(blue);
        surface[at + 3u] = 255u;
    }
}

typedef struct {
    bool enabled;
    int64_t left;
    int64_t top;
    int64_t right;
    int64_t bottom;
} WebglScissorBounds;

typedef struct {
    WebglScissorBounds scissor;
    int source_factor;
    int destination_factor;
    int depth_function;
    int cull_face;
    int front_face;
    bool blend;
    bool depth;
    bool cull;
} WebglRasterState;

static bool webgl_scissor_decode(const uint8_t *command, int surface_height,
                                 WebglScissorBounds *bounds)
{
    int enabled = 0;
    if (bounds == NULL
        || !webgl_integer_in_range(
            webgl_wire_i32(command, 39u), 0, 1, &enabled))
        return false;
    *bounds = (WebglScissorBounds) {
        .enabled = enabled != 0,
        .left = INT64_MIN,
        .top = INT64_MIN,
        .right = INT64_MAX,
        .bottom = INT64_MAX
    };
    if (!enabled) return true;
    int left = 0, bottom = 0, width = 0, height = 0;
    if (!webgl_integer_in_range(webgl_wire_i32(command, 40u), INT_MIN,
                              INT_MAX, &left)
        || !webgl_integer_in_range(webgl_wire_i32(command, 41u), INT_MIN,
                                 INT_MAX, &bottom)
        || !webgl_integer_in_range(webgl_wire_i32(command, 42u), 0,
                                 INT_MAX, &width)
        || !webgl_integer_in_range(webgl_wire_i32(command, 43u), 0,
                                 INT_MAX, &height)) return false;
    bounds->left = left;
    bounds->top = (int64_t) surface_height - bottom - height;
    bounds->right = (int64_t) left + width;
    bounds->bottom = bounds->top + height;
    return true;
}

static bool webgl_scissor_contains(const WebglScissorBounds *bounds,
                                   int x, int y)
{
    return bounds != NULL
        && (!bounds->enabled
            || ((int64_t) x >= bounds->left && (int64_t) x < bounds->right
                && (int64_t) y >= bounds->top
                && (int64_t) y < bounds->bottom));
}

static bool webgl_raster_state_decode(const uint8_t *command,
                                      int surface_height,
                                      WebglRasterState *state)
{
    if (state == NULL) return false;
    int blend = 0, depth = 0, cull = 0;
    *state = (WebglRasterState) {0};
    if (!webgl_scissor_decode(command, surface_height, &state->scissor)
        || !webgl_integer_in_range(webgl_wire_i32(command, 27u), 0, 1,
                                   &blend)
        || !webgl_integer_in_range(webgl_wire_i32(command, 28u), 0, 1,
                                   &depth)
        || !webgl_integer_in_range(webgl_wire_i32(command, 29u), 0, 1,
                                   &cull)
        || !webgl_blend_factors(command, blend != 0,
                                &state->source_factor,
                                &state->destination_factor)) return false;
    state->blend = blend != 0;
    state->depth = depth != 0;
    state->cull = cull != 0;
    if (state->depth
        && !webgl_integer_in_range(webgl_wire_i32(command, 30u),
                                   0x0200, 0x0207,
                                   &state->depth_function)) return false;
    if (state->cull
        && (!webgl_integer_in_range(webgl_wire_i32(command, 33u),
                                    0, INT_MAX, &state->cull_face)
            || !webgl_integer_in_range(webgl_wire_i32(command, 34u),
                                       0, INT_MAX,
                                       &state->front_face))) return false;
    return true;
}

static void webgl_sample_texture(const WebglTexture *texture,
                                 const WebglSource *sources,
                                 float u, float v, uint8_t output[4])
{
    if (texture == NULL) {
        memset(output, 255, 4u);
        return;
    }
    if (texture->repeat_s) u -= floorf(u);
    else u = fmaxf(0.0f, fminf(1.0f, u));
    if (texture->repeat_t) v -= floorf(v);
    else v = fmaxf(0.0f, fminf(1.0f, v));
    int x = (int) floorf(u * (float) texture->width);
    int y = (int) floorf((1.0f - v) * (float) texture->height);
    if (x >= texture->width) x = texture->width - 1;
    if (y >= texture->height) y = texture->height - 1;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    size_t at = ((size_t) y * (size_t) texture->width + (size_t) x) * 4u;
    memcpy(output, sources[texture->source].bytes + at, 4u);
}

static bool webgl_raster_triangle(uint8_t *surface, int width, int height,
                                  uint16_t *depth,
                                  const WebglVertex *a,
                                  const WebglVertex *b,
                                  const WebglVertex *c,
                                  const WebglRasterState *state,
                                  const WebglTexture *texture,
                                  const WebglSource *sources,
                                  size_t *work)
{
    if (!a->geometry_valid || !b->geometry_valid || !c->geometry_valid)
        return true;
    float area = (b->x - a->x) * (c->y - a->y)
               - (b->y - a->y) * (c->x - a->x);
    if (!isfinite(area) || fabsf(area) < 0.00001f) return true;
    bool front = state->front_face == 0x0901 ? area < 0.0f : area > 0.0f;
    if (state->cull && (state->cull_face == 0x0408
        || (state->cull_face == 0x0405 && !front)
        || (state->cull_face == 0x0404 && front))) return true;
    if (fmaxf(a->x, fmaxf(b->x, c->x)) <= 0.0f
        || fminf(a->x, fminf(b->x, c->x)) >= (float) width
        || fmaxf(a->y, fmaxf(b->y, c->y)) <= 0.0f
        || fminf(a->y, fminf(b->y, c->y)) >= (float) height) return true;
    int left = (int) floorf(fminf(a->x, fminf(b->x, c->x)));
    int right = (int) ceilf(fmaxf(a->x, fmaxf(b->x, c->x)));
    int top = (int) floorf(fminf(a->y, fminf(b->y, c->y)));
    int bottom = (int) ceilf(fmaxf(a->y, fmaxf(b->y, c->y)));
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > width) right = width;
    if (bottom > height) bottom = height;
    if (right <= left || bottom <= top) return true;
    size_t pixels = (size_t) (right - left) * (size_t) (bottom - top);
    /* Exhausting a page-controlled raster budget drops this primitive. It is
       bounded degradation, not corruption of the JS/native command stream. */
    if (pixels > WEBGL_RASTER_WORK_LIMIT - *work) return true;
    *work += pixels;
    for (int y = top; y < bottom; y++) {
        for (int x = left; x < right; x++) {
            if (!webgl_scissor_contains(&state->scissor, x, y)) continue;
            float px = (float) x + 0.5f, py = (float) y + 0.5f;
            float wa = ((b->x - px) * (c->y - py)
                        - (b->y - py) * (c->x - px)) / area;
            float wb = ((c->x - px) * (a->y - py)
                        - (c->y - py) * (a->x - px)) / area;
            float wc = 1.0f - wa - wb;
            if (wa < -0.0001f || wb < -0.0001f || wc < -0.0001f) continue;
            float perspective_sum = wa * a->inverse_w
                + wb * b->inverse_w + wc * c->inverse_w;
            if (!isfinite(perspective_sum)
                || fabsf(perspective_sum) < 0.000001f) continue;
            float pa = wa * a->inverse_w / perspective_sum;
            float pb = wb * b->inverse_w / perspective_sum;
            float pc = wc * c->inverse_w / perspective_sum;
            if (depth != NULL && state->depth) {
                float interpolated = wa * a->z + wb * b->z + wc * c->z;
                if (!isfinite(interpolated)) return false;
                uint16_t incoming = interpolated <= 0.0f ? UINT16_MAX
                    : interpolated >= 1.0f ? 0u
                    : (uint16_t) ((1.0f - interpolated) * 65535.0f + 0.5f);
                size_t depth_at = (size_t) y * (size_t) width + (size_t) x;
                int function = state->depth_function;
                uint16_t retained = depth[depth_at];
                bool passes = function == 0x0207
                    || (function == 0x0201 && incoming > retained)
                    || (function == 0x0202 && incoming == retained)
                    || (function == 0x0203 && incoming >= retained)
                    || (function == 0x0204 && incoming < retained)
                    || (function == 0x0205 && incoming != retained)
                    || (function == 0x0206 && incoming <= retained);
                if (!passes) continue;
                depth[depth_at] = incoming;
            }
            uint8_t sample[4];
            webgl_sample_texture(texture, sources,
                pa * a->u + pb * b->u + pc * c->u,
                pa * a->v + pb * b->v + pc * c->v, sample);
            webgl_pixel(surface, width, height, x, y,
                (pa * a->red + pb * b->red + pc * c->red) * sample[0] / 255.0f,
                (pa * a->green + pb * b->green + pc * c->green) * sample[1] / 255.0f,
                (pa * a->blue + pb * b->blue + pc * c->blue) * sample[2] / 255.0f,
                (pa * a->alpha + pb * b->alpha + pc * c->alpha) * sample[3] / 255.0f,
                state->blend, state->source_factor,
                state->destination_factor);
        }
    }
    return true;
}

static bool webgl_raster_line(uint8_t *surface, int width, int height,
                              const WebglVertex *a, const WebglVertex *b,
                              const WebglRasterState *state, size_t *work)
{
    if (!a->geometry_valid || !b->geometry_valid) return true;
    float dx = b->x - a->x, dy = b->y - a->y;
    int steps = (int) ceilf(fmaxf(fabsf(dx), fabsf(dy)));
    if (steps < 1) steps = 1;
    if ((size_t) steps > WEBGL_RASTER_WORK_LIMIT - *work) return true;
    *work += (size_t) steps;
    for (int i = 0; i <= steps; i++) {
        float ratio = (float) i / (float) steps;
        int x = (int) floorf(a->x + dx * ratio);
        int y = (int) floorf(a->y + dy * ratio);
        if (!webgl_scissor_contains(&state->scissor, x, y)) continue;
        float perspective_sum = (1.0f - ratio) * a->inverse_w
            + ratio * b->inverse_w;
        if (!isfinite(perspective_sum)
            || fabsf(perspective_sum) < 0.000001f) continue;
        float right = ratio * b->inverse_w / perspective_sum;
        float left = 1.0f - right;
        webgl_pixel(surface, width, height, x, y,
                    left * a->red + right * b->red,
                    left * a->green + right * b->green,
                    left * a->blue + right * b->blue,
                    left * a->alpha + right * b->alpha, state->blend,
                    state->source_factor, state->destination_factor);
    }
    return true;
}

#if defined(__PSP__)
__attribute__((unused))
#endif
static bool webgl_render_software(Budget *budget, uint8_t *surface,
                                  uint16_t *depth, int width, int height,
                                  const uint8_t *commands,
                                  size_t command_count,
                                  const WebglSource *sources,
                                  size_t source_count,
                                  const WebglTexture *textures,
                                  size_t texture_count)
{
    size_t work = 0;
    WebglVertex *decoded = budget_malloc_category(
        budget, BUDGET_CATEGORY_RENDER,
        WEBGL_VERTEX_LIMIT * sizeof(*decoded));
    if (decoded == NULL) return false;
    bool rendered = true;
    for (size_t command_index = 0; command_index < command_count;
         command_index++) {
        const uint8_t *command = commands
            + command_index * WEBGL_COMMAND_WORDS * sizeof(uint32_t);
        int kind = 0;
        if (!webgl_integer_in_range(webgl_wire_i32(command, 0u), 0, 1, &kind))
            { rendered = false; break; }
        if (kind == 0) {
            int mask = 0;
            WebglScissorBounds scissor;
            if (!webgl_integer_in_range(webgl_wire_i32(command, 1u), 0,
                                      INT_MAX, &mask)
                || !webgl_scissor_decode(command, height, &scissor)) {
                rendered = false; break;
            }
            if ((mask & WEBGL_COLOR_BUFFER_BIT) != 0) {
                uint8_t color[4];
                for (int component = 0; component < 4; component++) {
                    float raw = webgl_wire_float(
                        command, 2u + (size_t) component);
                    if (!isfinite(raw)) { rendered = false; break; }
                    color[component] = webgl_color_byte((float) raw);
                }
                color[3] = 255u;
                if (!rendered) break;
                if ((size_t) width * (size_t) height
                    > WEBGL_RASTER_WORK_LIMIT - work) {
                    budget_free(budget, decoded); return true;
                }
                work += (size_t) width * (size_t) height;
                for (int y = 0; y < height; y++) {
                    for (int x = 0; x < width; x++) {
                        if (!webgl_scissor_contains(&scissor, x, y))
                            continue;
                        memcpy(surface + ((size_t) y * (size_t) width
                                          + (size_t) x) * 4u,
                               color, sizeof(color));
                    }
                }
            }
            if ((mask & WEBGL_DEPTH_BUFFER_BIT) != 0 && depth != NULL) {
                float raw = webgl_wire_float(command, 6u);
                if (!isfinite(raw)) { rendered = false; break; }
                uint16_t value = raw <= 0.0f ? UINT16_MAX : raw >= 1.0f ? 0u
                    : (uint16_t) ((1.0f - raw) * 65535.0f + 0.5f);
                if ((size_t) width * (size_t) height
                    > WEBGL_RASTER_WORK_LIMIT - work) {
                    budget_free(budget, decoded); return true;
                }
                work += (size_t) width * (size_t) height;
                for (int y = 0; y < height; y++) {
                    for (int x = 0; x < width; x++) {
                        if (webgl_scissor_contains(&scissor, x, y))
                            depth[(size_t) y * (size_t) width + (size_t) x] = value;
                    }
                }
            }
            continue;
        }
        int mode = 0, count = 0, texture_index = 0;
        if (!webgl_integer_in_range(webgl_wire_i32(command, 1u), WEBGL_POINTS,
                                  WEBGL_TRIANGLE_FAN, &mode)
            || !webgl_integer_in_range(webgl_wire_i32(command, 3u), 0,
                                     WEBGL_VERTEX_LIMIT, &count)
            || !webgl_integer_in_range(webgl_wire_i32(command, 26u), -1,
                                     (int) texture_count - 1,
                                     &texture_index)) { rendered = false; break; }
        WebglDecodedDraw draw;
        WebglRasterState raster_state;
        if (!webgl_decode_draw(command, sources, source_count,
                               width, height, &draw)
            || !webgl_raster_state_decode(command, height, &raster_state)) {
            rendered = false;
            break;
        }
        for (int i = 0; i < count; i++) {
            if (!webgl_decode_vertex(&draw, (uint32_t) i, height,
                                     &decoded[i])) { rendered = false; break; }
        }
        if (!rendered) break;
        const WebglTexture *texture = texture_index >= 0
            ? &textures[texture_index] : NULL;
        if (mode == WEBGL_POINTS) {
            if ((size_t) count > WEBGL_RASTER_WORK_LIMIT - work) {
                budget_free(budget, decoded); return true;
            }
            work += (size_t) count;
            for (int i = 0; i < count; i++) {
                if (!decoded[i].geometry_valid) continue;
                int pixel_x = (int) decoded[i].x;
                int pixel_y = (int) decoded[i].y;
                if (!webgl_scissor_contains(
                        &raster_state.scissor, pixel_x, pixel_y)) continue;
                webgl_pixel(
                    surface, width, height, pixel_x, pixel_y,
                    decoded[i].red, decoded[i].green,
                    decoded[i].blue, decoded[i].alpha, raster_state.blend,
                    raster_state.source_factor,
                    raster_state.destination_factor);
            }
        } else if (mode == WEBGL_LINES || mode == WEBGL_LINE_STRIP
                   || mode == WEBGL_LINE_LOOP) {
            int segments = mode == WEBGL_LINES ? count / 2 : count - 1;
            for (int i = 0; i < segments; i++) {
                int left = mode == WEBGL_LINES ? i * 2 : i;
                if (!webgl_raster_line(surface, width, height,
                                       &decoded[left], &decoded[left + 1],
                                       &raster_state, &work)) {
                    rendered = false; break;
                }
            }
            if (!rendered) break;
            if (mode == WEBGL_LINE_LOOP && count > 2
                && !webgl_raster_line(surface, width, height,
                                      &decoded[count - 1], &decoded[0],
                                      &raster_state, &work)) {
                rendered = false; break;
            }
        } else {
            int triangles = mode == WEBGL_TRIANGLES ? count / 3 : count - 2;
            for (int i = 0; i < triangles; i++) {
                int ia = mode == WEBGL_TRIANGLES ? i * 3 : 0;
                int ib = mode == WEBGL_TRIANGLES ? ia + 1
                       : mode == WEBGL_TRIANGLE_STRIP ? i : i + 1;
                int ic = mode == WEBGL_TRIANGLES ? ia + 2
                       : mode == WEBGL_TRIANGLE_STRIP ? i + 1 : i + 2;
                if (mode == WEBGL_TRIANGLE_STRIP) {
                    ia = i; ib = i + 1; ic = i + 2;
                    if ((i & 1) != 0) { int swap = ia; ia = ib; ib = swap; }
                }
                if (!webgl_raster_triangle(surface, width, height, depth,
                        &decoded[ia], &decoded[ib], &decoded[ic],
                        &raster_state, texture, sources, &work)) {
                    rendered = false; break;
                }
            }
            if (!rendered) break;
        }
    }
    budget_free(budget, decoded);
    return rendered;
}

#if defined(__PSP__)
#define WEBGL_GE_LIST_BYTES (64u * 1024u)
#define WEBGL_GE_COLOR_OFFSET ((size_t) 0x0cc000)
#define WEBGL_GE_DEPTH_OFFSET ((size_t) 0x154000)
#define WEBGL_GE_TEXTURE_OFFSET ((size_t) 0x198000)
#define WEBGL_GE_UNCACHED UINT32_C(0x40000000)

typedef struct {
    float u, v;
    uint32_t color;
    float x, y, z;
} WebglGeVertex;

static const ScePspFMatrix4 webgl_ge_identity = {
    .x = {1.0f, 0.0f, 0.0f, 0.0f},
    .y = {0.0f, 1.0f, 0.0f, 0.0f},
    .z = {0.0f, 0.0f, 1.0f, 0.0f},
    .w = {0.0f, 0.0f, 0.0f, 1.0f}
};

_Static_assert(sizeof(ScePspFMatrix4) == sizeof(float) * 16u,
               "PSP GE matrix must retain 16 WebGL column-major floats");

static unsigned int __attribute__((aligned(64)))
    webgl_ge_list[WEBGL_GE_LIST_BYTES / sizeof(unsigned int)];

typedef struct {
    uint32_t identifier;
    uint32_t generation;
    int width;
    int height;
    unsigned stride;
    unsigned padded_height;
    size_t offset;
} WebglGeTextureCacheEntry;

static ScriptWebglCacheAdmission webgl_ge_texture_owner;
static uint32_t webgl_ge_display_content_epoch;
static WebglGeTextureCacheEntry
    webgl_ge_texture_cache[WEBGL_TEXTURE_LIMIT];
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
static ScriptWebglNativeMetrics webgl_native_metrics;
#endif

static unsigned webgl_power_of_two(unsigned value)
{
    unsigned result = 1;
    while (result < value && result < 512u) result <<= 1;
    return result;
}

static int webgl_ge_primitive(int mode)
{
    static const int primitives[] = {
        GU_POINTS, GU_LINES, GU_LINE_STRIP, GU_LINE_STRIP,
        GU_TRIANGLES, GU_TRIANGLE_STRIP, GU_TRIANGLE_FAN
    };
    return mode >= WEBGL_POINTS && mode <= WEBGL_TRIANGLE_FAN
        ? primitives[mode] : -1;
}

static unsigned webgl_ge_initial_full_clear_mask(
    const uint8_t *commands, size_t command_count)
{
    if (commands == NULL || command_count == 0) return 0;
    int kind = 0, mask = 0, scissor_enabled = 0;
    if (!webgl_integer_in_range(webgl_wire_i32(commands, 0u), 0, 1, &kind)
        || kind != 0
        || !webgl_integer_in_range(webgl_wire_i32(commands, 1u), 0,
                                 INT_MAX, &mask)
        || !webgl_integer_in_range(webgl_wire_i32(commands, 39u), 0, 1,
                                 &scissor_enabled)
        || scissor_enabled != 0) return 0;
    return (unsigned) mask;
}

static bool webgl_ge_vertex_storage_required(
    const uint8_t *commands, size_t command_count,
    size_t *vertex_capacity, size_t *validity_capacity)
{
    if (commands == NULL || vertex_capacity == NULL
        || validity_capacity == NULL) return false;
    size_t vertices = 0, maximum_draw = 0;
    for (size_t at = 0; at < command_count; at++) {
        const uint8_t *command = commands
            + at * WEBGL_COMMAND_WORDS * sizeof(uint32_t);
        int kind = 0;
        if (!webgl_integer_in_range(webgl_wire_i32(command, 0u), 0, 1,
                                    &kind)) return false;
        if (kind == 0) continue;
        int mode = 0, count = 0;
        if (!webgl_integer_in_range(webgl_wire_i32(command, 1u),
                                    WEBGL_POINTS, WEBGL_TRIANGLE_FAN, &mode)
            || !webgl_integer_in_range(webgl_wire_i32(command, 3u), 0,
                                       WEBGL_VERTEX_LIMIT, &count)) return false;
        size_t required = (size_t) count
            + (mode == WEBGL_LINE_LOOP && count > 1 ? 1u : 0u);
        if (required > WEBGL_VERTEX_LIMIT + 1u
            || vertices > WEBGL_VERTEX_LIMIT + WEBGL_COMMAND_LIMIT
            || required > WEBGL_VERTEX_LIMIT + WEBGL_COMMAND_LIMIT - vertices)
            return false;
        vertices += required;
        if ((size_t) count > maximum_draw) maximum_draw = (size_t) count;
    }
    *vertex_capacity = vertices;
    *validity_capacity = maximum_draw;
    return true;
}

static bool webgl_render_ge(Budget *budget,
                            uint32_t realm_epoch_high,
                            uint32_t realm_epoch_low,
                            int64_t canvas_handle,
                            uint8_t *surface,
                            uint16_t *depth,
                            int width, int height,
                            const uint8_t *commands, size_t command_count,
                            const WebglSource *sources, size_t source_count,
                            const WebglTexture *textures, size_t texture_count)
{
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
    uint64_t total_started = (uint64_t) sceKernelGetSystemTimeWide();
#endif
    if (!psp_media_present_ge_context_acquire()
        || !psp_media_present_ge_context_idle()) return false;
    size_t ge_vertex_capacity = 0, validity_capacity = 0;
    if (!webgl_ge_vertex_storage_required(
            commands, command_count, &ge_vertex_capacity,
            &validity_capacity)) return false;
    size_t scratch_bytes = ge_vertex_capacity * sizeof(WebglGeVertex)
        + validity_capacity;
    if (scratch_bytes == 0u) scratch_bytes = 1u;
    WebglGeVertex *scratch = budget_malloc_category(
        budget, BUDGET_CATEGORY_RENDER, scratch_bytes);
    if (scratch == NULL) return false;
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
    webgl_native_metrics.scratch_bytes += scratch_bytes;
    if (scratch_bytes > webgl_native_metrics.maximum_scratch_bytes)
        webgl_native_metrics.maximum_scratch_bytes = scratch_bytes;
#endif
    uint8_t *vertex_validity = (uint8_t *) (scratch + ge_vertex_capacity);
    void *edram = sceGeEdramGetAddr();
    if (edram == NULL) { budget_free(budget, scratch); return false; }
    uint8_t *color_target = (uint8_t *) edram + WEBGL_GE_COLOR_OFFSET;
    size_t color_cache_bytes = ((size_t) (height - 1)
        * PSP_DISPLAY_STRIDE + (size_t) width) * sizeof(uint32_t);
    size_t depth_cache_bytes = ((size_t) (height - 1)
        * PSP_DISPLAY_STRIDE + (size_t) width) * sizeof(uint16_t);
    unsigned initial_clear = webgl_ge_initial_full_clear_mask(
        commands, command_count);
    if ((initial_clear & WEBGL_COLOR_BUFFER_BIT) == 0u) {
        for (int y = 0; y < height; y++) memcpy(
            color_target + (size_t) y * PSP_DISPLAY_STRIDE * 4u,
            surface + (size_t) y * (size_t) width * 4u,
            (size_t) width * 4u);
        sceKernelDcacheWritebackRange(
            color_target, color_cache_bytes);
    }
    if (depth != NULL
        && (initial_clear & WEBGL_DEPTH_BUFFER_BIT) == 0u) {
        uint8_t *depth_target = (uint8_t *) edram + WEBGL_GE_DEPTH_OFFSET;
        for (int y = 0; y < height; y++) memcpy(
            depth_target + (size_t) y * PSP_DISPLAY_STRIDE * sizeof(uint16_t),
            depth + (size_t) y * (size_t) width,
            (size_t) width * sizeof(uint16_t));
        sceKernelDcacheWritebackRange(
            depth_target, depth_cache_bytes);
    }
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
    uint64_t seeded = (uint64_t) sceKernelGetSystemTimeWide();
#endif
    bool cache_owner_reset = false;
    uint32_t display_content_epoch = psp_display_edram_content_epoch();
    if (webgl_ge_display_content_epoch != display_content_epoch) {
        webgl_ge_display_content_epoch = display_content_epoch;
        webgl_ge_texture_owner.valid = false;
    }
    if (!script_runtime_webgl_cache_admit(
            &webgl_ge_texture_owner,
            realm_epoch_high, realm_epoch_low,
            canvas_handle, &cache_owner_reset)) {
        budget_free(budget, scratch); return false;
    }
    if (cache_owner_reset) {
        memset(webgl_ge_texture_cache, 0,
               sizeof(webgl_ge_texture_cache));
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
        webgl_native_metrics.texture_cache_owner_resets++;
#endif
    }
    void *texture_addresses[WEBGL_TEXTURE_LIMIT] = {0};
    unsigned texture_strides[WEBGL_TEXTURE_LIMIT] = {0};
    unsigned texture_heights[WEBGL_TEXTURE_LIMIT] = {0};
    size_t texture_capacity =
        PSP_DISPLAY_EDRAM_BYTES - WEBGL_GE_TEXTURE_OFFSET;
    size_t additional_bytes = 0;
    for (size_t i = 0; i < texture_count; i++) {
        bool retained = false;
        for (size_t entry = 0;
             entry < webgl_ge_texture_owner.cached_entries; entry++) {
            const WebglGeTextureCacheEntry *candidate =
                &webgl_ge_texture_cache[entry];
            if (candidate->identifier == textures[i].identifier
                && candidate->generation == textures[i].generation
                && candidate->width == textures[i].width
                && candidate->height == textures[i].height) {
                retained = true;
                break;
            }
        }
        if (retained) continue;
        size_t bytes = (size_t) webgl_power_of_two((unsigned) textures[i].width)
            * webgl_power_of_two((unsigned) textures[i].height) * 4u;
        if (bytes > texture_capacity - additional_bytes) {
            budget_free(budget, scratch); return false;
        }
        additional_bytes += bytes;
    }
    if (additional_bytes > texture_capacity
                               - webgl_ge_texture_owner.cached_bytes
        || webgl_ge_texture_owner.cached_entries + texture_count
               > WEBGL_TEXTURE_LIMIT) {
        webgl_ge_texture_owner.cached_entries = 0;
        webgl_ge_texture_owner.cached_bytes = 0;
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
        webgl_native_metrics.texture_cache_capacity_resets++;
#endif
    }
    for (size_t i = 0; i < texture_count; i++) {
        unsigned stride = webgl_power_of_two((unsigned) textures[i].width);
        unsigned padded_height = webgl_power_of_two((unsigned) textures[i].height);
        size_t bytes = (size_t) stride * padded_height * 4u;
        size_t cached = webgl_ge_texture_owner.cached_entries;
        for (size_t entry = 0;
             entry < webgl_ge_texture_owner.cached_entries; entry++) {
            const WebglGeTextureCacheEntry *candidate =
                &webgl_ge_texture_cache[entry];
            if (candidate->identifier == textures[i].identifier
                && candidate->generation == textures[i].generation
                && candidate->width == textures[i].width
                && candidate->height == textures[i].height) {
                cached = entry;
                break;
            }
        }
        if (cached == webgl_ge_texture_owner.cached_entries) {
            if (webgl_ge_texture_owner.cached_entries >= WEBGL_TEXTURE_LIMIT
                || bytes > texture_capacity
                               - webgl_ge_texture_owner.cached_bytes) {
                budget_free(budget, scratch); return false;
            }
            cached = webgl_ge_texture_owner.cached_entries++;
            WebglGeTextureCacheEntry *entry =
                &webgl_ge_texture_cache[cached];
            *entry = (WebglGeTextureCacheEntry) {
                .identifier = textures[i].identifier,
                .generation = textures[i].generation,
                .width = textures[i].width,
                .height = textures[i].height,
                .stride = stride,
                .padded_height = padded_height,
                .offset = webgl_ge_texture_owner.cached_bytes
            };
            uint8_t *destination = (uint8_t *) edram
                + WEBGL_GE_TEXTURE_OFFSET + entry->offset;
            memset(destination, 0, bytes);
            for (int y = 0; y < textures[i].height; y++) memcpy(
                destination + (size_t) y * stride * 4u,
                sources[textures[i].source].bytes
                    + (size_t) y * (size_t) textures[i].width * 4u,
                (size_t) textures[i].width * 4u);
            sceKernelDcacheWritebackRange(destination, bytes);
            webgl_ge_texture_owner.cached_bytes += bytes;
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
            webgl_native_metrics.texture_upload_bytes += bytes;
#endif
        }
        const WebglGeTextureCacheEntry *entry =
            &webgl_ge_texture_cache[cached];
        texture_addresses[i] = (uint8_t *) edram
            + WEBGL_GE_TEXTURE_OFFSET + entry->offset;
        texture_strides[i] = entry->stride;
        texture_heights[i] = entry->padded_height;
    }
    sceGuStart(GU_DIRECT, (void *) ((uintptr_t) webgl_ge_list
                                    | WEBGL_GE_UNCACHED));
    sceGuDrawBufferList(GU_PSM_8888,
                        (void *) (uintptr_t) WEBGL_GE_COLOR_OFFSET,
                        PSP_DISPLAY_STRIDE);
    sceGuDepthBuffer((void *) (uintptr_t) WEBGL_GE_DEPTH_OFFSET,
                     PSP_DISPLAY_STRIDE);
    sceGuOffset(2048 - width / 2, 2048 - height / 2);
    sceGuViewport(2048, 2048, width, height);
    sceGuScissor(0, 0, width, height); sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_LIGHTING); sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_CULL_FACE); sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_BLEND); sceGuDisable(GU_TEXTURE_2D);
    sceGuShadeModel(GU_SMOOTH);
    size_t vertex_cursor = 0;
    for (size_t ci = 0; ci < command_count; ci++) {
        const uint8_t *command = commands
            + ci * WEBGL_COMMAND_WORDS * sizeof(uint32_t);
        int kind = 0;
        if (!webgl_integer_in_range(webgl_wire_i32(command, 0u), 0, 1, &kind)) {
            sceGuFinish(); sceGuSync(0, 0); budget_free(budget, scratch);
            return false;
        }
        int scissor_enabled = 0;
        if (!webgl_integer_in_range(webgl_wire_i32(command, 39u), 0, 1,
                                  &scissor_enabled)) {
            sceGuFinish(); sceGuSync(0, 0); budget_free(budget, scratch);
            return false;
        }
        if (scissor_enabled) {
            int left = 0, bottom = 0, scissor_width = 0, scissor_height = 0;
            if (!webgl_integer_in_range(webgl_wire_i32(command, 40u), INT_MIN,
                                      INT_MAX, &left)
                || !webgl_integer_in_range(webgl_wire_i32(command, 41u), INT_MIN,
                                         INT_MAX, &bottom)
                || !webgl_integer_in_range(webgl_wire_i32(command, 42u), 0,
                                         INT_MAX, &scissor_width)
                || !webgl_integer_in_range(webgl_wire_i32(command, 43u), 0,
                                         INT_MAX, &scissor_height)) {
                sceGuFinish(); sceGuSync(0, 0); budget_free(budget, scratch);
                return false;
            }
            int64_t right = (int64_t) left + scissor_width;
            int64_t top = (int64_t) height - bottom - scissor_height;
            int64_t lower = (int64_t) height - bottom;
            if (left < 0) left = 0;
            if (left > width) left = width;
            if (right < 0) right = 0;
            if (right > width) right = width;
            if (top < 0) top = 0;
            if (top > height) top = height;
            if (lower < 0) lower = 0;
            if (lower > height) lower = height;
            int64_t clipped_width = right - left;
            int64_t clipped_height = lower - top;
            if (clipped_width <= 0 || clipped_height <= 0) continue;
            sceGuScissor(left, (int) top, (int) clipped_width,
                         (int) clipped_height);
        } else sceGuScissor(0, 0, width, height);
        if (kind == 0) {
            int mask = 0;
            if (!webgl_integer_in_range(webgl_wire_i32(command, 1u), 0,
                                      INT_MAX, &mask)) continue;
            uint32_t color = 0;
            for (unsigned component = 0; component < 4; component++) {
                float value = webgl_wire_float(command, 2u + component);
                if (!isfinite(value)) value = 0;
                color |= (uint32_t) webgl_color_byte(value)
                      << (component * 8u);
            }
            float raw_depth = webgl_wire_float(command, 6u);
            if (!isfinite(raw_depth)) raw_depth = 1.0;
            if (raw_depth < 0.0) raw_depth = 0.0;
            if (raw_depth > 1.0) raw_depth = 1.0;
            sceGuClearColor(color);
            sceGuClearDepth((unsigned) ((1.0f - raw_depth) * 65535.0f + 0.5f));
            unsigned bits = 0;
            if (mask & WEBGL_COLOR_BUFFER_BIT)
                bits |= GU_COLOR_BUFFER_BIT;
            if (mask & WEBGL_DEPTH_BUFFER_BIT) bits |= GU_DEPTH_BUFFER_BIT;
            if (bits != 0) sceGuClear(bits);
            continue;
        }
        int mode = 0, count = 0, texture_index = 0;
        if (!webgl_integer_in_range(webgl_wire_i32(command, 1u), 0, 6, &mode)
            || !webgl_integer_in_range(webgl_wire_i32(command, 3u), 0,
                                     WEBGL_VERTEX_LIMIT, &count)
            || !webgl_integer_in_range(webgl_wire_i32(command, 26u), -1,
                                     (int) texture_count - 1,
                                     &texture_index)) continue;
        size_t command_vertices = (size_t) count
            + (mode == WEBGL_LINE_LOOP && count > 1 ? 1u : 0u);
        if (vertex_cursor > ge_vertex_capacity
            || command_vertices > ge_vertex_capacity - vertex_cursor) {
            sceGuFinish(); sceGuSync(0, 0); budget_free(budget, scratch);
            return false;
        }
        WebglGeVertex *draw_vertices = scratch + vertex_cursor;
        WebglDecodedDraw draw;
        if (!webgl_decode_draw(command, sources, source_count,
                               width, height, &draw)) {
            sceGuFinish(); sceGuSync(0, 0); budget_free(budget, scratch);
            return false;
        }
        bool geometry_valid = true;
        for (int i = 0; i < count; i++) {
            WebglVertex decoded;
            if (!webgl_decode_vertex(&draw, (uint32_t) i, height, &decoded)) {
                sceGuFinish(); sceGuSync(0, 0); budget_free(budget, scratch);
                return false;
            }
            vertex_validity[i] = decoded.geometry_valid ? 1u : 0u;
            if (!decoded.geometry_valid) geometry_valid = false;
            draw_vertices[i] = (WebglGeVertex) {
                .u = decoded.u, .v = decoded.v,
                .color = (uint32_t) decoded.red
                    | (uint32_t) decoded.green << 8
                    | (uint32_t) decoded.blue << 16
                    | (uint32_t) decoded.alpha << 24,
                .x = decoded.object_x, .y = decoded.object_y,
                .z = decoded.object_z
            };
        }
        /* The GE clips homogeneous 3D primitives. Geometry Tilefinch refuses
           (non-finite input, non-unit authored w, or a near/far-plane
           crossing outside this bounded profile) is page geometry, never a
           lost-context condition. Preserve unaffected independent primitives
           without growing the bounded vertex buffer. */
        if (!geometry_valid) {
            int primitive_width = mode == WEBGL_POINTS ? 1
                : mode == WEBGL_LINES ? 2
                : mode == WEBGL_TRIANGLES ? 3 : 0;
            if (primitive_width == 0) continue;
            int retained = 0;
            for (int first = 0; first + primitive_width <= count;
                 first += primitive_width) {
                bool valid = true;
                for (int at = 0; at < primitive_width; at++)
                    valid = valid && vertex_validity[first + at] != 0u;
                if (!valid) continue;
                for (int at = 0; at < primitive_width; at++)
                    draw_vertices[retained++] = draw_vertices[first + at];
            }
            count = retained;
            if (count == 0) continue;
        }
        int64_t viewport_center_x = (int64_t) (2048 - width / 2)
            + draw.viewport_x + draw.viewport_width / 2;
        int64_t viewport_center_y = (int64_t) (2048 - height / 2)
            + height - draw.viewport_y - draw.viewport_height / 2;
        if (draw.viewport_width <= 0 || draw.viewport_height <= 0
            || viewport_center_x < 0 || viewport_center_x > 4095
            || viewport_center_y < 0 || viewport_center_y > 4095) {
            vertex_cursor += (size_t) count;
            continue;
        }
        ScePspFMatrix4 projection;
        memcpy(&projection, draw.matrix, sizeof(projection));
        sceGumMatrixMode(GU_PROJECTION);
        sceGumLoadMatrix(&projection);
        sceGumMatrixMode(GU_VIEW);
        sceGumLoadMatrix(&webgl_ge_identity);
        sceGumMatrixMode(GU_MODEL);
        sceGumLoadMatrix(&webgl_ge_identity);
        sceGuViewport((int) viewport_center_x, (int) viewport_center_y,
                      draw.viewport_width, draw.viewport_height);
        if (texture_index >= 0) {
            const WebglTexture *texture = &textures[texture_index];
            for (int i = 0; i < count; i++) {
                draw_vertices[i].u *= (float) texture->width;
                draw_vertices[i].v *= (float) texture->height;
            }
            sceGuEnable(GU_TEXTURE_2D);
            sceGuTexMode(GU_PSM_8888, 0, 0, GU_FALSE);
            sceGuTexImage(0, texture_strides[texture_index],
                          texture_heights[texture_index],
                          texture_strides[texture_index],
                          texture_addresses[texture_index]);
            sceGuTexScale(1.0f, 1.0f);
            sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
            sceGuTexFilter(texture->minimum_filter == 0x2600
                               ? GU_NEAREST : GU_LINEAR,
                           texture->magnification_filter == 0x2600
                               ? GU_NEAREST : GU_LINEAR);
            sceGuTexWrap(texture->repeat_s ? GU_REPEAT : GU_CLAMP,
                         texture->repeat_t ? GU_REPEAT : GU_CLAMP);
        } else sceGuDisable(GU_TEXTURE_2D);
        if (webgl_wire_i32(command, 27u) != 0) {
            int source_factor = 0, destination_factor = 0;
            if (!webgl_integer_in_range(webgl_wire_i32(command, 31u), 0,
                                      INT_MAX, &source_factor)
                || !webgl_integer_in_range(webgl_wire_i32(command, 32u), 0,
                                         INT_MAX, &destination_factor)) {
                sceGuFinish(); sceGuSync(0, 0); budget_free(budget, scratch);
                return false;
            }
            int source_ge = source_factor == WEBGL_ZERO ? GU_FIX
                : source_factor == WEBGL_ONE ? GU_FIX
                : source_factor == WEBGL_SRC_ALPHA ? GU_SRC_ALPHA
                : source_factor == WEBGL_ONE_MINUS_SRC_ALPHA
                    ? GU_ONE_MINUS_SRC_ALPHA : -1;
            int destination_ge = destination_factor == WEBGL_ZERO ? GU_FIX
                : destination_factor == WEBGL_ONE ? GU_FIX
                : destination_factor == WEBGL_SRC_ALPHA ? GU_SRC_ALPHA
                : destination_factor == WEBGL_ONE_MINUS_SRC_ALPHA
                    ? GU_ONE_MINUS_SRC_ALPHA : -1;
            if (source_ge < 0 || destination_ge < 0) {
                sceGuFinish(); sceGuSync(0, 0); budget_free(budget, scratch);
                return false;
            }
            sceGuEnable(GU_BLEND);
            sceGuBlendFunc(
                GU_ADD, source_ge, destination_ge,
                source_factor == WEBGL_ONE ? UINT32_C(0x00ffffff) : 0,
                destination_factor == WEBGL_ONE
                    ? UINT32_C(0x00ffffff) : 0);
        } else sceGuDisable(GU_BLEND);
        if (webgl_wire_i32(command, 28u) != 0) {
            int depth_function = 0;
            if (!webgl_integer_in_range(webgl_wire_i32(command, 30u),
                                      0x0200, 0x0207, &depth_function)) {
                sceGuFinish(); sceGuSync(0, 0); budget_free(budget, scratch);
                return false;
            }
            static const int ge_depth_functions[8] = {
                GU_NEVER, GU_GREATER, GU_EQUAL, GU_GEQUAL,
                GU_LESS, GU_NOTEQUAL, GU_LEQUAL, GU_ALWAYS
            };
            sceGuEnable(GU_DEPTH_TEST);
            sceGuDepthFunc(ge_depth_functions[depth_function - 0x0200]);
            sceGuDepthRange(65535, 0); sceGuDepthMask(GU_FALSE);
        } else { sceGuDisable(GU_DEPTH_TEST); sceGuDepthMask(GU_TRUE); }
        int cull = 0, cull_face = 0, front_face = 0;
        if (!webgl_integer_in_range(webgl_wire_i32(command, 29u), 0, 1, &cull)
            || !webgl_integer_in_range(webgl_wire_i32(command, 33u), 0,
                                     INT_MAX, &cull_face)
            || !webgl_integer_in_range(webgl_wire_i32(command, 34u), 0,
                                     INT_MAX, &front_face)) {
            sceGuFinish(); sceGuSync(0, 0); budget_free(budget, scratch);
            return false;
        }
        if (cull && cull_face == 0x0408) {
            vertex_cursor += (size_t) count;
            continue;
        }
        if (cull) {
            bool keep_clockwise = front_face == 0x0901;
            if (cull_face == 0x0404) keep_clockwise = !keep_clockwise;
            sceGuFrontFace(keep_clockwise ? GU_CW : GU_CCW);
            sceGuEnable(GU_CULL_FACE);
        } else sceGuDisable(GU_CULL_FACE);
        size_t ge_count = (size_t) count;
        if (mode == WEBGL_LINE_LOOP && count > 1) {
            draw_vertices[count] = draw_vertices[0];
            ge_count++;
        }
        sceGumDrawArray(webgl_ge_primitive(mode),
            GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF
                | GU_TRANSFORM_3D,
            (int) ge_count, NULL, draw_vertices);
        vertex_cursor += ge_count;
    }
    if (vertex_cursor != 0u)
        sceKernelDcacheWritebackRange(
            scratch, vertex_cursor * sizeof(*scratch));
    sceGuFinish();
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
    uint64_t emitted = (uint64_t) sceKernelGetSystemTimeWide();
#endif
    sceGuSync(0, 0);
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
    uint64_t synchronized = (uint64_t) sceKernelGetSystemTimeWide();
#endif
    uint8_t *color = (uint8_t *) edram + WEBGL_GE_COLOR_OFFSET;
    sceKernelDcacheInvalidateRange(color, color_cache_bytes);
    /* The bounded context advertises alpha:false. PSP GE stores stencil in
       the framebuffer alpha channel, so an ordinary color clear cannot also
       preserve WebGL's requested alpha byte. Copy RGB from GE and make the
       resolved drawing buffer explicitly opaque in the same pass. */
    for (int y = 0; y < height; y++) {
        uint32_t *destination = (uint32_t *) surface
            + (size_t) y * (size_t) width;
        const uint32_t *source = (const uint32_t *) color
            + (size_t) y * PSP_DISPLAY_STRIDE;
        for (int x = 0; x < width; x++)
            destination[x] = source[x] | UINT32_C(0xff000000);
    }
    if (depth != NULL) {
        uint8_t *depth_target = (uint8_t *) edram + WEBGL_GE_DEPTH_OFFSET;
        sceKernelDcacheInvalidateRange(depth_target, depth_cache_bytes);
        for (int y = 0; y < height; y++) memcpy(
            depth + (size_t) y * (size_t) width,
            depth_target + (size_t) y * PSP_DISPLAY_STRIDE * sizeof(uint16_t),
            (size_t) width * sizeof(uint16_t));
    }
    budget_free(budget, scratch);
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
    uint64_t completed = (uint64_t) sceKernelGetSystemTimeWide();
    uint64_t total_us = completed - total_started;
    webgl_native_metrics.frames++;
    webgl_native_metrics.vertices += vertex_cursor;
    webgl_native_metrics.seed_us += seeded - total_started;
    webgl_native_metrics.command_us += emitted - seeded;
    webgl_native_metrics.sync_us += synchronized - emitted;
    webgl_native_metrics.readback_us += completed - synchronized;
    webgl_native_metrics.total_us += total_us;
    if (total_us > webgl_native_metrics.maximum_total_us)
        webgl_native_metrics.maximum_total_us = total_us;
#endif
    return true;
}
#endif

bool script_runtime_webgl_native_metrics(ScriptWebglNativeMetrics *metrics)
{
    if (metrics == NULL) return false;
#if defined(__PSP__) && defined(TILEFINCH_PSP_VALIDATION_LOG)
    *metrics = webgl_native_metrics;
    return metrics->frames != 0;
#else
    memset(metrics, 0, sizeof(*metrics));
    return false;
#endif
}

void js_webgl_realm_epoch_release(DomBridge *bridge)
{
    if (bridge == NULL) return;
#if defined(__PSP__)
    if (webgl_ge_texture_owner.valid
        && webgl_ge_texture_owner.realm_epoch_high
               == bridge->webgl_realm_epoch_high
        && webgl_ge_texture_owner.realm_epoch_low
               == bridge->webgl_realm_epoch_low) {
        memset(&webgl_ge_texture_owner, 0,
               sizeof(webgl_ge_texture_owner));
        memset(webgl_ge_texture_cache, 0,
               sizeof(webgl_ge_texture_cache));
    }
#endif
    bridge->webgl_realm_epoch_high = 0u;
    bridge->webgl_realm_epoch_low = 0u;
}

static lxb_dom_node_t *webgl_resolve_canvas(
    DomBridge *bridge, int64_t handle);

JSValue js_webgl_release_surface(JSContext *context,
                                 JSValueConst this_value,
                                 int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || bridge->images == NULL || argc < 1)
        return JS_NewBool(context, false);
    int64_t handle = 0;
    if (JS_ToInt64(context, &handle, argv[0]) < 0) return JS_EXCEPTION;
    lxb_dom_node_t *node = webgl_resolve_canvas(bridge, handle);
    if (node == NULL) return JS_NewBool(context, false);
    bool released = js_webgl_realm_epoch_advance(bridge)
        && images_release_canvas(bridge->images, bridge->budget, node);
    if (released && bridge_node_is_connected(node))
        js_rt_bridge_note_canvas_mutation(bridge, node, true);
    return JS_NewBool(context, released);
}

static lxb_dom_node_t *webgl_resolve_canvas(DomBridge *bridge, int64_t handle)
{
    size_t slot = 0;
    if (bridge == NULL
        || !js_rt_bridge_node_slot_for_handle(bridge, handle, &slot)) return NULL;
    return bridge->nodes[slot];
}

JSValue js_webgl_render(JSContext *context, JSValueConst this_value,
                        int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || bridge->budget == NULL || argc < 6)
        return JS_NewInt32(context, 0);
    if (bridge->images == NULL) return JS_NewInt32(context, 2);
    int64_t handle = 0;
    int32_t width = 0, height = 0;
    if (JS_ToInt64(context, &handle, argv[0]) < 0
        || JS_ToInt32(context, &width, argv[1]) < 0
        || JS_ToInt32(context, &height, argv[2]) < 0) return JS_EXCEPTION;
    if (width <= 0 || height <= 0 || width > 480 || height > 272
        || (size_t) width * (size_t) height > WEBGL_PIXEL_LIMIT)
        return JS_NewInt32(context, 0);
    const uint8_t *command_values = NULL, *texture_values = NULL;
    size_t command_value_count = 0, texture_value_count = 0;
    JSValue command_buffer = JS_UNDEFINED, texture_buffer = JS_UNDEFINED;
    WebglJsReadResult command_read = webgl_typed_values(
        context, argv[3], sizeof(uint32_t), &command_values,
        &command_value_count, &command_buffer);
    if (command_read == WEBGL_JS_READ_EXCEPTION) return JS_EXCEPTION;
    if (command_read != WEBGL_JS_READ_OK) return JS_NewInt32(context, 0);
    size_t command_count = 0;
    if (!webgl_wire_header(
            command_values, command_value_count, WEBGL_COMMAND_WIRE_MAGIC,
            WEBGL_COMMAND_WORDS, WEBGL_COMMAND_LIMIT, &command_count)) {
        JS_FreeValue(context, command_buffer); return JS_NewInt32(context, 0);
    }
    command_values += WEBGL_WIRE_HEADER_WORDS * sizeof(uint32_t);
    WebglSource sources[WEBGL_SOURCE_LIMIT] = {{0}};
    size_t source_count = 0;
    WebglJsReadResult source_read = webgl_sources_read(
        context, argv[4], sources, &source_count);
    if (source_read != WEBGL_JS_READ_OK) {
        webgl_sources_release(context, sources, source_count);
        JS_FreeValue(context, command_buffer);
        return source_read == WEBGL_JS_READ_EXCEPTION
            ? JS_EXCEPTION : JS_NewInt32(context, 0);
    }
    WebglJsReadResult texture_read = webgl_typed_values(
        context, argv[5], sizeof(uint32_t), &texture_values,
        &texture_value_count, &texture_buffer);
    if (texture_read != WEBGL_JS_READ_OK) {
        webgl_sources_release(context, sources, source_count);
        JS_FreeValue(context, command_buffer);
        return texture_read == WEBGL_JS_READ_EXCEPTION
            ? JS_EXCEPTION : JS_NewInt32(context, 0);
    }
    size_t texture_count = 0;
    if (!webgl_wire_header(
            texture_values, texture_value_count, WEBGL_TEXTURE_WIRE_MAGIC,
            WEBGL_TEXTURE_WORDS, WEBGL_TEXTURE_LIMIT, &texture_count)) {
        JS_FreeValue(context, texture_buffer);
        webgl_sources_release(context, sources, source_count);
        JS_FreeValue(context, command_buffer);
        return JS_NewInt32(context, 0);
    }
    texture_values += WEBGL_WIRE_HEADER_WORDS * sizeof(uint32_t);
    WebglTexture textures[WEBGL_TEXTURE_LIMIT];
    bool valid = webgl_textures_read(
        texture_values, texture_count * WEBGL_TEXTURE_WORDS,
        sources, source_count,
        textures, &texture_count);
    /* Resolve only after every potentially coercive JavaScript operation. */
    lxb_dom_node_t *node = valid ? webgl_resolve_canvas(bridge, handle) : NULL;
    uint8_t *surface = NULL;
    ImageCanvasCommitResult prepared = node == NULL
        ? IMAGE_CANVAS_COMMIT_REFUSED
        : images_prepare_canvas_surface(bridge->images, bridge->budget, node,
                                        width, height, &surface);
    bool rendered = prepared != IMAGE_CANVAS_COMMIT_REFUSED && surface != NULL;
    bool needs_depth = false;
    for (size_t i = 0; rendered && i < command_count; i++) {
        const uint8_t *command = command_values
            + i * WEBGL_COMMAND_WORDS * sizeof(uint32_t);
        int kind = 0, mask = 0;
        bool draw = webgl_wire_i32(command, 28u) == 1;
        bool clear = webgl_integer_in_range(webgl_wire_i32(command, 0u),
                                          0, 1, &kind)
            && kind == 0
            && webgl_integer_in_range(webgl_wire_i32(command, 1u),
                                    0, INT_MAX, &mask)
            && (mask & WEBGL_DEPTH_BUFFER_BIT) != 0;
        if (draw || clear) needs_depth = true;
    }
    uint16_t *depth = NULL;
    if (rendered && needs_depth) rendered = images_prepare_canvas_depth(
        bridge->images, bridge->budget, node, width, height, &depth);
    if (rendered) {
#if defined(__PSP__)
        rendered = webgl_render_ge(
            bridge->budget,
            bridge->webgl_realm_epoch_high,
            bridge->webgl_realm_epoch_low,
            handle, surface, depth, width, height, command_values,
            command_count, sources, source_count,
            textures, texture_count);
#else
        rendered = webgl_render_software(
            bridge->budget, surface, depth, width, height, command_values,
            command_count, sources, source_count,
            textures, texture_count);
#endif
    }
    JS_FreeValue(context, texture_buffer);
    webgl_sources_release(context, sources, source_count);
    JS_FreeValue(context, command_buffer);
    if (!rendered) return JS_NewInt32(context, 0);
    if (bridge_node_is_connected(node)) js_rt_bridge_note_canvas_mutation(
        bridge, node, prepared == IMAGE_CANVAS_COMMIT_UPDATED);
    return JS_NewInt32(context, 1);
}

JSValue js_webgl_snapshot(JSContext *context, JSValueConst this_value,
                          int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || bridge->images == NULL || argc < 1) return JS_NULL;
    int64_t handle = 0;
    if (JS_ToInt64(context, &handle, argv[0]) < 0) return JS_EXCEPTION;
    lxb_dom_node_t *node = webgl_resolve_canvas(bridge, handle);
    const ImageResource *image = node == NULL ? NULL
        : images_find_node(bridge->images, node);
    if (!image_resource_available(image) || !image->is_canvas
        || image->pixels == NULL || image->width <= 0 || image->height <= 0)
        return JS_NULL;
    size_t length = (size_t) image->width * (size_t) image->height * 4u;
    JSValue result = JS_NewObject(context);
    JSValue pixels = JS_NewArrayBufferCopy(context, image->pixels, length);
    if (JS_IsException(result) || JS_IsException(pixels)) {
        JS_FreeValue(context, result); JS_FreeValue(context, pixels);
        return JS_EXCEPTION;
    }
    if (JS_SetPropertyStr(context, result, "width",
                          JS_NewInt32(context, image->width)) < 0
        || JS_SetPropertyStr(context, result, "height",
                             JS_NewInt32(context, image->height)) < 0
        || JS_SetPropertyStr(context, result, "pixels", pixels) < 0) {
        JS_FreeValue(context, result); return JS_EXCEPTION;
    }
    return result;
}

JSValue js_webgl_read_pixels(JSContext *context, JSValueConst this_value,
                             int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || bridge->images == NULL || argc < 6) return JS_FALSE;
    int64_t handle = 0;
    int32_t x = 0, y = 0, width = 0, height = 0, alignment = 4;
    if (JS_ToInt64(context, &handle, argv[0]) < 0
        || JS_ToInt32(context, &x, argv[1]) < 0
        || JS_ToInt32(context, &y, argv[2]) < 0
        || JS_ToInt32(context, &width, argv[3]) < 0
        || JS_ToInt32(context, &height, argv[4]) < 0
        || (argc >= 7 && JS_ToInt32(context, &alignment, argv[6]) < 0))
        return JS_EXCEPTION;
    if (width < 0 || height < 0
        || width > (int32_t) WEBGL_PIXEL_LIMIT
        || height > (int32_t) WEBGL_PIXEL_LIMIT
        || (alignment != 1 && alignment != 2
            && alignment != 4 && alignment != 8)
        || (height != 0
            && (size_t) width > SIZE_MAX / (size_t) height)
        || (size_t) width * (size_t) height > WEBGL_PIXEL_LIMIT)
        return JS_FALSE;
    size_t offset = 0, length = 0, element_size = 0;
    JSValue buffer = JS_GetTypedArrayBuffer(
        context, argv[5], &offset, &length, &element_size);
    if (JS_IsException(buffer)) return buffer;
    size_t storage_length = 0;
    uint8_t *storage = JS_GetArrayBuffer(context, &storage_length, buffer);
    size_t row_bytes = (size_t) width * 4u;
    size_t row_stride = row_bytes;
    size_t remainder = row_bytes % (size_t) alignment;
    if (remainder != 0u) row_stride += (size_t) alignment - remainder;
    size_t required = height == 0 ? 0u
        : (size_t) (height - 1) * row_stride + row_bytes;
    bool valid = storage != NULL && element_size == 1u
        && offset <= storage_length && length <= storage_length - offset
        && required <= length;
    lxb_dom_node_t *node = valid ? webgl_resolve_canvas(bridge, handle) : NULL;
    const ImageResource *image = node == NULL ? NULL
        : images_find_node(bridge->images, node);
    if (!image_resource_available(image) || !image->is_canvas
        || image->pixels == NULL || image->width <= 0 || image->height <= 0)
        valid = false;
    if (valid) {
        /* WebGL readPixels has a bottom-left origin. Pixels outside the
           framebuffer leave the corresponding destination bytes untouched;
           rows inside the intersection are deliberately reversed here. */
        for (int row = 0; row < height; row++) {
            int64_t framebuffer_y = (int64_t) y + row;
            if (framebuffer_y < 0 || framebuffer_y >= image->height) continue;
            int64_t left = x < 0 ? 0 : x;
            int64_t requested_right = (int64_t) x + width;
            int64_t right = requested_right > image->width
                ? image->width : requested_right;
            if (right <= left) continue;
            size_t destination_x = (size_t) (left - x);
            size_t source_y = (size_t) (image->height - 1 - framebuffer_y);
            size_t copy_pixels = (size_t) (right - left);
            memcpy(storage + offset + (size_t) row * row_stride
                       + destination_x * 4u,
                   image->pixels + (source_y * (size_t) image->width
                                     + (size_t) left) * 4u,
                   copy_pixels * 4u);
        }
    }
    JS_FreeValue(context, buffer);
    return JS_NewBool(context, valid);
}
