#include "js_runtime_internal.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static lxb_dom_node_t *webgl_resolve_canvas(
    DomBridge *bridge, int64_t handle);

bool script_runtime_webgl_depth_readback_required(
    bool preserve_drawing_buffer, unsigned initial_clear_mask)
{
    return preserve_drawing_buffer
        || (initial_clear_mask & UINT32_C(0x00000100)) == 0u;
}

bool script_runtime_webgl_antialias_edges_admit(
    size_t admitted_indices, size_t requested_indices)
{
    const size_t limit = 512u;
    return admitted_indices <= limit
        && requested_indices <= limit - admitted_indices;
}

bool script_runtime_webgl_antialias_radius_admit(
    float projected_radius_squared)
{
    return isfinite(projected_radius_squared)
        && projected_radius_squared >= 9.0f;
}

bool script_runtime_webgl_temporal_matrix_admit(
    const float previous[16], const float current[16], bool *moved)
{
    if (moved != NULL) *moved = false;
    if (previous == NULL || current == NULL || moved == NULL) return false;
    float maximum_delta = 0.0f;
    for (size_t at = 0; at < 16u; at++) {
        if (!isfinite(previous[at]) || !isfinite(current[at])) return false;
        float delta = fabsf(previous[at] - current[at]);
        if (delta > maximum_delta) maximum_delta = delta;
    }
    /* A large camera cut gets no history. Tiny float noise gets no redundant
       draw. The admitted interval covers the bounded shake used by modest
       games while keeping a prior edge close enough to read as coverage. */
    if (maximum_delta > 0.20f) return false;
    *moved = maximum_delta >= 0.0005f;
    return true;
}

bool script_runtime_webgl_temporal_geometry_admit(
    bool has_instance_matrix, bool has_instance_transform)
{
    /* The bounded temporal pass retains the preceding camera matrix, not a
       second copy of every instance transform. Reprojecting current tank or
       sprite transforms through the old camera produces a false history edge
       at a position that never existed. Static/non-instanced geometry is the
       only shape for which the retained matrix is sufficient evidence. */
    return !has_instance_matrix && !has_instance_transform;
}

#if defined(__PSP__)
static bool webgl_temporal_matrix_same(const float left[16],
                                       const float right[16])
{
    if (left == NULL || right == NULL) return false;
    for (size_t at = 0; at < 16u; at++) {
        if (!isfinite(left[at]) || !isfinite(right[at])
            || fabsf(left[at] - right[at]) > 0.00001f) return false;
    }
    return true;
}
#endif

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
#define WEBGL_COMMAND_WORDS 96u
#define WEBGL_TEXTURE_WORDS 9u
#define WEBGL_WIRE_HEADER_WORDS 4u
#define WEBGL_COMMAND_WIRE_MAGIC UINT32_C(0x54465743)
#define WEBGL_TEXTURE_WIRE_MAGIC UINT32_C(0x54465754)
#define WEBGL_WIRE_VERSION 5u
#define WEBGL_COMMAND_LIMIT 64u
#define WEBGL_SOURCE_LIMIT 32u
#define WEBGL_TEXTURE_LIMIT 8u
#define WEBGL_VERTEX_LIMIT 4096u
#define WEBGL_INSTANCE_LIMIT 64u
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

bool script_runtime_webgl_geometry_cache_admit(
    ScriptWebglGeometryCacheState *state,
    uint32_t realm_epoch_high,
    uint32_t realm_epoch_low,
    int64_t canvas_handle,
    const ScriptWebglGeometryCacheSignature *signature,
    size_t bytes,
    size_t *slot,
    bool *hit,
    bool *reset)
{
    if (state == NULL || signature == NULL || slot == NULL || hit == NULL
        || reset == NULL || bytes == 0u
        || bytes > SCRIPT_WEBGL_GEOMETRY_CACHE_BYTE_LIMIT
        || (realm_epoch_high == 0u && realm_epoch_low == 0u)) return false;
    *reset = !state->valid
        || state->realm_epoch_high != realm_epoch_high
        || state->realm_epoch_low != realm_epoch_low
        || state->canvas_handle != canvas_handle;
    if (*reset) {
        memset(state, 0, sizeof(*state));
        state->realm_epoch_high = realm_epoch_high;
        state->realm_epoch_low = realm_epoch_low;
        state->canvas_handle = canvas_handle;
        state->valid = true;
    }
    state->clock++;
    if (state->clock == 0u) {
        uint32_t old_ages[SCRIPT_WEBGL_GEOMETRY_CACHE_ENTRY_LIMIT];
        size_t valid_count = 0u;
        for (size_t i = 0; i < SCRIPT_WEBGL_GEOMETRY_CACHE_ENTRY_LIMIT; i++) {
            old_ages[i] = state->records[i].age;
            if (state->records[i].valid) valid_count++;
        }
        for (size_t i = 0; i < SCRIPT_WEBGL_GEOMETRY_CACHE_ENTRY_LIMIT; i++) {
            if (!state->records[i].valid) continue;
            uint32_t rank = 1u;
            for (size_t other = 0;
                 other < SCRIPT_WEBGL_GEOMETRY_CACHE_ENTRY_LIMIT; other++) {
                if (!state->records[other].valid || other == i) continue;
                if (old_ages[other] < old_ages[i]
                    || (old_ages[other] == old_ages[i] && other < i)) rank++;
            }
            state->records[i].age = rank;
        }
        state->clock = (uint32_t) valid_count + 1u;
    }
    for (size_t i = 0; i < SCRIPT_WEBGL_GEOMETRY_CACHE_ENTRY_LIMIT; i++) {
        ScriptWebglGeometryCacheRecord *record = &state->records[i];
        if (record->valid && record->bytes == bytes
            && memcmp(&record->signature, signature,
                      sizeof(*signature)) == 0) {
            record->age = state->clock;
            *slot = i;
            *hit = true;
            return true;
        }
    }
    size_t selected = SCRIPT_WEBGL_GEOMETRY_CACHE_ENTRY_LIMIT;
    for (size_t i = 0; i < SCRIPT_WEBGL_GEOMETRY_CACHE_ENTRY_LIMIT; i++) {
        if (!state->records[i].valid) { selected = i; break; }
    }
    if (selected == SCRIPT_WEBGL_GEOMETRY_CACHE_ENTRY_LIMIT) {
        selected = 0u;
        for (size_t i = 1; i < SCRIPT_WEBGL_GEOMETRY_CACHE_ENTRY_LIMIT; i++) {
            if (state->records[i].age < state->records[selected].age)
                selected = i;
        }
    }
    ScriptWebglGeometryCacheRecord *record = &state->records[selected];
    if (record->valid) state->retained_bytes -= record->bytes;
    record->valid = false;
    while (bytes > SCRIPT_WEBGL_GEOMETRY_CACHE_BYTE_LIMIT
                       - state->retained_bytes) {
        size_t victim = SCRIPT_WEBGL_GEOMETRY_CACHE_ENTRY_LIMIT;
        for (size_t i = 0; i < SCRIPT_WEBGL_GEOMETRY_CACHE_ENTRY_LIMIT; i++) {
            if (i == selected || !state->records[i].valid) continue;
            if (victim == SCRIPT_WEBGL_GEOMETRY_CACHE_ENTRY_LIMIT
                || state->records[i].age < state->records[victim].age) {
                victim = i;
            }
        }
        if (victim == SCRIPT_WEBGL_GEOMETRY_CACHE_ENTRY_LIMIT) return false;
        state->retained_bytes -= state->records[victim].bytes;
        memset(&state->records[victim], 0, sizeof(state->records[victim]));
    }
    *record = (ScriptWebglGeometryCacheRecord) {
        .signature = *signature,
        .bytes = bytes,
        .age = state->clock,
        .valid = true
    };
    state->retained_bytes += bytes;
    *slot = selected;
    *hit = false;
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
    WebglAttributeView instance_matrix;
    WebglAttributeView instance_color;
    WebglAttributeView instance_transform;
    float matrix[16];
    float uniform[4];
    uint32_t first;
    size_t index_offset;
    int index_type;
    int viewport_x;
    int viewport_y;
    int viewport_width;
    int viewport_height;
    uint32_t instance_count;
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
    if (count > record_limit) return false;
    size_t used_words = WEBGL_WIRE_HEADER_WORDS
        + (size_t) count * record_words;
    size_t capacity_words = WEBGL_WIRE_HEADER_WORDS
        + record_limit * record_words;
    /* The browser uses one reusable maximum-capacity view to avoid a typed
       array allocation per flush. Native callers may instead pass the exact
       used prefix. Reject every other trailing/truncated shape so stale or
       hostile words cannot become an accidental extension of the wire ABI. */
    if (word_count != used_words && word_count != capacity_words)
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

#if defined(__PSP__)
static uint32_t webgl_color_modulate(uint32_t color, const float tint[4])
{
    return (uint32_t) webgl_channel_byte(
               (float) (color & 0xffu) * tint[0] + 0.5f)
        | (uint32_t) webgl_channel_byte(
              (float) ((color >> 8) & 0xffu) * tint[1] + 0.5f) << 8
        | (uint32_t) webgl_channel_byte(
              (float) ((color >> 16) & 0xffu) * tint[2] + 0.5f) << 16
        | (uint32_t) webgl_channel_byte(
              (float) (color >> 24) * tint[3] + 0.5f) << 24;
}
#endif

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
    int indexed = 0, first = 0, count = 0;
    int index_source = 0, index_type = 0;
    int index_offset = 0, instance_count = 0;
    if (draw == NULL
        || !webgl_integer_in_range(webgl_wire_i32(command, 2u), 0,
                              INT_MAX, &first)
        || !webgl_integer_in_range(webgl_wire_i32(command, 4u), 0, 1,
                                 &indexed)
        || !webgl_integer_in_range(webgl_wire_i32(command, 3u), 0,
                                 WEBGL_VERTEX_LIMIT, &count)) return false;
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
        || !webgl_attribute_view(
            command, 77u, sources, source_count, &draw->instance_matrix)
        || !webgl_attribute_view(
            command, 83u, sources, source_count, &draw->instance_color)
        || !webgl_attribute_view(
            command, 89u, sources, source_count, &draw->instance_transform)
        || !webgl_integer_in_range(webgl_wire_i32(command, 35u), INT_MIN,
                                 INT_MAX, &draw->viewport_x)
        || !webgl_integer_in_range(webgl_wire_i32(command, 36u), INT_MIN,
                                 INT_MAX, &draw->viewport_y)
        || !webgl_integer_in_range(webgl_wire_i32(command, 37u), 0,
                                 surface_width, &draw->viewport_width)
        || !webgl_integer_in_range(webgl_wire_i32(command, 38u), 0,
                                 surface_height, &draw->viewport_height)
        || !webgl_integer_in_range(webgl_wire_i32(command, 76u), 1,
                                 WEBGL_INSTANCE_LIMIT, &instance_count)) {
        return false;
    }
    draw->instance_count = (uint32_t) instance_count;
    /* JavaScript enforces the same expanded-work ceiling, but the native
       boundary remains authoritative for hostile direct bridge calls. */
    if ((uint32_t) count > WEBGL_VERTEX_LIMIT / draw->instance_count)
        return false;
    if (draw->instance_matrix.source != NULL
        && (draw->instance_matrix.size != 4
            || draw->instance_matrix.type != WEBGL_FLOAT
            || draw->instance_matrix.normalized
            || draw->instance_matrix.stride < sizeof(float) * 16u)) {
        return false;
    }
    if (draw->instance_color.source != NULL
        && draw->instance_color.size != 4) return false;
    if (draw->instance_transform.source != NULL
        && (draw->instance_transform.size != 4
            || draw->instance_transform.type != WEBGL_FLOAT
            || draw->instance_transform.normalized
            || draw->instance_matrix.source != NULL)) return false;
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

static bool webgl_instance_matrix(const WebglDecodedDraw *base,
                                  uint32_t instance, float matrix[16])
{
    if (base == NULL || matrix == NULL || instance >= base->instance_count)
        return false;
    const WebglAttributeView *view = &base->instance_matrix;
    if (view->source == NULL) {
        memset(matrix, 0, sizeof(float) * 16u);
        matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;
        if (base->instance_transform.source != NULL) {
            float transform[4];
            static const float identity_transform[4] = {0, 0, 0, 1};
            if (!webgl_attribute_from_view(
                    &base->instance_transform, instance, transform,
                    identity_transform)) return false;
            for (size_t component = 0; component < 4u; component++)
                if (!isfinite(transform[component])) return false;
            matrix[0] = matrix[5] = matrix[10] = transform[3];
            matrix[12] = transform[0];
            matrix[13] = transform[1];
            matrix[14] = transform[2];
        }
        return true;
    }
    if ((size_t) instance > (SIZE_MAX - view->offset) / view->stride)
        return false;
    size_t offset = view->offset + (size_t) instance * view->stride;
    if (offset > view->source->length
        || sizeof(float) * 16u > view->source->length - offset) return false;
    memcpy(matrix, view->source->bytes + offset, sizeof(float) * 16u);
    for (size_t at = 0; at < 16u; at++)
        if (!isfinite(matrix[at])) return false;
    return true;
}

static bool webgl_instance_color(const WebglDecodedDraw *base,
                                 uint32_t instance, float color[4])
{
    static const float white[4] = {1, 1, 1, 1};
    if (base == NULL || color == NULL || instance >= base->instance_count)
        return false;
    const WebglAttributeView *view = &base->instance_color;
    /* Instanced game geometry overwhelmingly uses a tightly-packed FLOAT
       vec4 tint. Read that already-validated shape as one bounded copy
       rather than routing its four values through the generic scalar
       decoder. The generic path remains authoritative for every other
       admitted WebGL attribute representation. */
    if (view->source != NULL && view->size == 4
        && view->type == WEBGL_FLOAT && !view->normalized
        && view->stride == sizeof(float) * 4u) {
        if ((size_t) instance > (SIZE_MAX - view->offset) / view->stride)
            return false;
        size_t offset = view->offset + (size_t) instance * view->stride;
        if (offset > view->source->length
            || sizeof(float) * 4u > view->source->length - offset)
            return false;
        memcpy(color, view->source->bytes + offset, sizeof(float) * 4u);
    } else if (!webgl_attribute_from_view(view, instance, color, white)) {
        return false;
    }
    for (size_t component = 0; component < 4u; component++)
        if (!isfinite(color[component])) return false;
    return true;
}

static bool webgl_instance_draw(const WebglDecodedDraw *base,
                                uint32_t instance,
                                WebglDecodedDraw *output)
{
    if (base == NULL || output == NULL) return false;
    float matrix[16];
    if (!webgl_instance_matrix(base, instance, matrix)) return false;
    *output = *base;
    for (size_t column = 0; column < 4u; column++) {
        for (size_t row = 0; row < 4u; row++) {
            float value = 0.0f;
            for (size_t inner = 0; inner < 4u; inner++) {
                value += base->matrix[inner * 4u + row]
                    * matrix[column * 4u + inner];
            }
            if (!isfinite(value)) return false;
            output->matrix[column * 4u + row] = value;
        }
    }
    if (base->instance_color.source != NULL) {
        float color[4];
        if (!webgl_instance_color(base, instance, color)) return false;
        for (size_t component = 0; component < 4u; component++) {
            output->uniform[component] *= color[component];
            if (!isfinite(output->uniform[component])) return false;
        }
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

/* Resolve a partially covered edge sample without allocating a multisample
   framebuffer.  Coverage is distinct from authored alpha: when blending is
   disabled it still blends the covered fraction over the retained pixel,
   which is the resolve step a multisample buffer would have performed. */
static void webgl_pixel_coverage(
    uint8_t *surface, int width, int height, int x, int y,
    float red, float green, float blue, float alpha,
    const WebglRasterState *state, float coverage)
{
    if (coverage >= 0.999f) {
        webgl_pixel(surface, width, height, x, y,
                    red, green, blue, alpha, state->blend,
                    state->source_factor, state->destination_factor);
        return;
    }
    if (coverage <= 0.0f) return;
    if (!state->blend) {
        webgl_pixel(surface, width, height, x, y,
                    red, green, blue, coverage * 255.0f, true,
                    WEBGL_SRC_ALPHA, WEBGL_ONE_MINUS_SRC_ALPHA);
        return;
    }
    if (state->source_factor == WEBGL_ONE) {
        red *= coverage;
        green *= coverage;
        blue *= coverage;
    }
    webgl_pixel(surface, width, height, x, y,
                red, green, blue, alpha * coverage, true,
                state->source_factor, state->destination_factor);
}

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

static bool webgl_depth_test_write(uint16_t *depth, int width, int height,
                                   int x, int y, float value,
                                   const WebglRasterState *state);

static bool webgl_raster_triangle(uint8_t *surface, int width, int height,
                                  uint16_t *depth,
                                  const WebglVertex *a,
                                  const WebglVertex *b,
                                  const WebglVertex *c,
                                  const WebglRasterState *state,
                                  const WebglTexture *texture,
                                  const WebglSource *sources,
                                  bool antialias, size_t *work)
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
    size_t work_per_pixel = antialias ? 4u : 1u;
    if (pixels > (WEBGL_RASTER_WORK_LIMIT - *work) / work_per_pixel)
        return true;
    *work += pixels * work_per_pixel;
    for (int y = top; y < bottom; y++) {
        for (int x = left; x < right; x++) {
            if (!webgl_scissor_contains(&state->scissor, x, y)) continue;
            float px = (float) x + 0.5f, py = (float) y + 0.5f;
            float wa = ((b->x - px) * (c->y - py)
                        - (b->y - py) * (c->x - px)) / area;
            float wb = ((c->x - px) * (a->y - py)
                        - (c->y - py) * (a->x - px)) / area;
            float wc = 1.0f - wa - wb;
            float coverage = 1.0f;
            if (antialias) {
                static const float offsets[4][2] = {
                    {0.25f, 0.25f}, {0.75f, 0.25f},
                    {0.25f, 0.75f}, {0.75f, 0.75f}
                };
                float sum_a = 0.0f, sum_b = 0.0f, sum_c = 0.0f;
                unsigned covered = 0;
                for (size_t sample = 0; sample < 4u; sample++) {
                    float sx = (float) x + offsets[sample][0];
                    float sy = (float) y + offsets[sample][1];
                    float sa = ((b->x - sx) * (c->y - sy)
                                - (b->y - sy) * (c->x - sx)) / area;
                    float sb = ((c->x - sx) * (a->y - sy)
                                - (c->y - sy) * (a->x - sx)) / area;
                    float sc = 1.0f - sa - sb;
                    if (sa < -0.0001f || sb < -0.0001f || sc < -0.0001f)
                        continue;
                    sum_a += sa; sum_b += sb; sum_c += sc; covered++;
                }
                if (covered == 0u) continue;
                float inverse = 1.0f / (float) covered;
                wa = sum_a * inverse;
                wb = sum_b * inverse;
                wc = sum_c * inverse;
                coverage = (float) covered * 0.25f;
            } else if (wa < -0.0001f || wb < -0.0001f || wc < -0.0001f) {
                continue;
            }
            float perspective_sum = wa * a->inverse_w
                + wb * b->inverse_w + wc * c->inverse_w;
            if (!isfinite(perspective_sum)
                || fabsf(perspective_sum) < 0.000001f) continue;
            float pa = wa * a->inverse_w / perspective_sum;
            float pb = wb * b->inverse_w / perspective_sum;
            float pc = wc * c->inverse_w / perspective_sum;
            if (!webgl_depth_test_write(
                    depth, width, height, x, y,
                    wa * a->z + wb * b->z + wc * c->z, state)) continue;
            uint8_t sample[4];
            webgl_sample_texture(texture, sources,
                pa * a->u + pb * b->u + pc * c->u,
                pa * a->v + pb * b->v + pc * c->v, sample);
            webgl_pixel_coverage(surface, width, height, x, y,
                (pa * a->red + pb * b->red + pc * c->red) * sample[0] / 255.0f,
                (pa * a->green + pb * b->green + pc * c->green) * sample[1] / 255.0f,
                (pa * a->blue + pb * b->blue + pc * c->blue) * sample[2] / 255.0f,
                (pa * a->alpha + pb * b->alpha + pc * c->alpha) * sample[3] / 255.0f,
                state, coverage);
        }
    }
    return true;
}

static bool webgl_depth_test_write(uint16_t *depth, int width, int height,
                                   int x, int y, float value,
                                   const WebglRasterState *state)
{
    if (state == NULL || !state->depth) return true;
    if (depth == NULL || !isfinite(value)
        || x < 0 || y < 0 || x >= width || y >= height) return false;
    uint16_t incoming = value <= 0.0f ? UINT16_MAX
        : value >= 1.0f ? 0u
        : (uint16_t) ((1.0f - value) * 65535.0f + 0.5f);
    size_t at = (size_t) y * (size_t) width + (size_t) x;
    uint16_t retained = depth[at];
    int function = state->depth_function;
    bool passes = function == 0x0207
        || (function == 0x0201 && incoming > retained)
        || (function == 0x0202 && incoming == retained)
        || (function == 0x0203 && incoming >= retained)
        || (function == 0x0204 && incoming < retained)
        || (function == 0x0205 && incoming != retained)
        || (function == 0x0206 && incoming <= retained);
    if (passes) depth[at] = incoming;
    return passes;
}

static bool webgl_raster_line(uint8_t *surface, uint16_t *depth,
                              int width, int height,
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
        if (!webgl_depth_test_write(
                depth, width, height, x, y,
                (1.0f - ratio) * a->z + ratio * b->z, state)) continue;
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
                                  size_t texture_count, bool antialias)
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
        WebglDecodedDraw base_draw;
        WebglRasterState raster_state;
        if (!webgl_decode_draw(command, sources, source_count,
                               width, height, &base_draw)
            || !webgl_raster_state_decode(command, height, &raster_state)) {
            rendered = false;
            break;
        }
        const WebglTexture *texture = texture_index >= 0
            ? &textures[texture_index] : NULL;
        for (uint32_t instance = 0; instance < base_draw.instance_count;
             instance++) {
            WebglDecodedDraw draw;
            if (!webgl_instance_draw(&base_draw, instance, &draw)) {
                rendered = false; break;
            }
            for (int i = 0; i < count; i++) {
                if (!webgl_decode_vertex(&draw, (uint32_t) i, height,
                                         &decoded[i])) {
                    rendered = false; break;
                }
            }
            if (!rendered) break;
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
                if (!webgl_depth_test_write(
                        depth, width, height, pixel_x, pixel_y,
                        decoded[i].z, &raster_state)) continue;
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
                if (!webgl_raster_line(surface, depth, width, height,
                                       &decoded[left], &decoded[left + 1],
                                       &raster_state, &work)) {
                    rendered = false; break;
                }
            }
            if (!rendered) break;
            if (mode == WEBGL_LINE_LOOP && count > 2
                && !webgl_raster_line(surface, depth, width, height,
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
                        &raster_state, texture, sources, antialias, &work)) {
                    rendered = false; break;
                }
            }
            if (!rendered) break;
            }
        }
        if (!rendered) break;
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
/* Edge coverage is cosmetic and must not scale with a full 4,096-vertex
   primary mesh. Large filled meshes retain their ordinary draw; 512 indices
   leave room for compact moving geometry without a command-time spike. */
#define WEBGL_GE_AA_INDEX_LIMIT 256u
/* The PSP has no multisample target, so triangle smoothing is a second GE
   submission of the visible edges. Keep that cosmetic pass bounded per frame:
   primary geometry always renders, and ordinary scenes remain below the cap,
   while particle storms cannot turn dozens of tiny fringes into a frame-time
   spike. Instance order is retained, so authored foreground geometry wins. */
#define WEBGL_GE_AA_DRAW_LIMIT 4u
/* Temporal stability is an edge-history problem, not permission for another
   framebuffer. Reproject at most six prior fringes and 384 endpoints; the
   filled scene is always current and a large camera cut rejects history. */
#define WEBGL_GE_TAA_DRAW_LIMIT 6u
#define WEBGL_GE_TAA_VERTEX_LIMIT 384u

typedef struct {
    float u, v;
    uint32_t color;
    float x, y, z;
} WebglGeVertex;

static bool webgl_ge_stable_signature(
    const uint8_t *command, ScriptWebglGeometryCacheSignature *signature)
{
    if (command == NULL || signature == NULL) return false;
    memset(signature, 0, sizeof(*signature));
    const size_t identity_slots[4] = {64u, 67u, 70u, 73u};
    const size_t source_slots[4] = {5u, 8u, 14u, 20u};
    bool indexed = webgl_wire_i32(command, 4u) != 0;
    for (size_t i = 0; i < 4u; i++) {
        bool present = i == 0u ? indexed
            : webgl_wire_i32(command, source_slots[i]) >= 0;
        if (!present) continue;
        if (webgl_wire_i32(command, identity_slots[i]) <= 0
            || webgl_wire_i32(command, identity_slots[i] + 1u) <= 0)
            return false;
    }
    size_t out = 0;
    const size_t fixed_slots[] = {1u, 2u, 3u, 4u, 6u, 7u};
    for (size_t i = 0; i < sizeof(fixed_slots) / sizeof(fixed_slots[0]); i++)
        signature->words[out++] = webgl_wire_u32(command, fixed_slots[i]);
    const size_t attribute_slots[] = {8u, 14u, 20u};
    for (size_t view = 0; view < 3u; view++) {
        size_t slot = attribute_slots[view];
        for (size_t field = 1u; field < 6u; field++)
            signature->words[out++] = webgl_wire_u32(command, slot + field);
    }
    for (size_t slot = 64u; slot < 76u; slot++)
        signature->words[out++] = webgl_wire_u32(command, slot);
    for (size_t slot = 60u; slot < 64u; slot++)
        signature->words[out++] = webgl_wire_u32(command, slot);
    return out <= SCRIPT_WEBGL_GEOMETRY_CACHE_SIGNATURE_WORDS;
}

static bool webgl_ge_fast_layout(const WebglDecodedDraw *draw)
{
    if (draw == NULL || draw->position.source == NULL
        || draw->position.type != WEBGL_FLOAT
        || (draw->position.size != 2 && draw->position.size != 3)
        || draw->position.normalized
        || draw->position.stride
            < (size_t) draw->position.size * sizeof(float)
        || draw->position.stride % sizeof(float) != 0u)
        return false;
    if (draw->indexed && draw->index_type != WEBGL_UNSIGNED_SHORT)
        return false;
    if (draw->color.source != NULL
        && (draw->color.type != WEBGL_FLOAT || draw->color.size != 4
            || draw->color.normalized || draw->color.stride < 16u
            || draw->color.stride % sizeof(float) != 0u))
        return false;
    if (draw->texcoord.source != NULL
        && (draw->texcoord.type != WEBGL_FLOAT || draw->texcoord.size != 2
            || draw->texcoord.normalized || draw->texcoord.stride < 8u
            || draw->texcoord.stride % sizeof(float) != 0u))
        return false;
    return true;
}

static bool webgl_ge_vertex(const WebglDecodedDraw *draw, uint32_t sequence,
                            bool fast, WebglGeVertex *output, bool *valid)
{
    if (draw == NULL || output == NULL || valid == NULL) return false;
    uint32_t index = 0;
    if (!webgl_draw_vertex_index(draw, sequence, &index)) return false;
    float position[4] = {0, 0, 0, 1};
    float color[4] = {1, 1, 1, 1};
    float texcoord[4] = {0, 0, 0, 1};
    if (fast) {
        const WebglAttributeView *views[3] = {
            &draw->position, &draw->color, &draw->texcoord
        };
        float *outputs[3] = {position, color, texcoord};
        const size_t components[3] = {
            (size_t) draw->position.size, 4u, 2u
        };
        for (size_t view = 0; view < 3u; view++) {
            if (views[view]->source == NULL) continue;
            if ((size_t) index > (SIZE_MAX - views[view]->offset)
                                      / views[view]->stride) return false;
            size_t at = views[view]->offset
                + (size_t) index * views[view]->stride;
            size_t bytes = components[view] * sizeof(float);
            if (at > views[view]->source->length
                || bytes > views[view]->source->length - at) return false;
            memcpy(outputs[view], views[view]->source->bytes + at, bytes);
        }
    } else {
        static const float position_fallback[4] = {0, 0, 0, 1};
        static const float color_fallback[4] = {1, 1, 1, 1};
        static const float texcoord_fallback[4] = {0, 0, 0, 1};
        if (!webgl_attribute_from_view(
                &draw->position, index, position, position_fallback)
            || !webgl_attribute_from_view(
                &draw->color, index, color, color_fallback)
            || !webgl_attribute_from_view(
                &draw->texcoord, index, texcoord, texcoord_fallback))
            return false;
    }
    *valid = true;
    for (size_t component = 0; component < 4u; component++) {
        if (!isfinite(position[component]) || !isfinite(color[component])
            || !isfinite(texcoord[component])) *valid = false;
    }
    if (fabsf(position[3] - 1.0f) > 0.000001f
        || fabsf(position[0]) > 1048576.0f
        || fabsf(position[1]) > 1048576.0f
        || fabsf(position[2]) > 1048576.0f) *valid = false;
    *output = (WebglGeVertex) {
        .u = texcoord[0], .v = texcoord[1],
        .color = (uint32_t) webgl_color_byte(color[0] * draw->uniform[0])
            | (uint32_t) webgl_color_byte(color[1] * draw->uniform[1]) << 8
            | (uint32_t) webgl_color_byte(color[2] * draw->uniform[2]) << 16
            | (uint32_t) webgl_color_byte(color[3] * draw->uniform[3]) << 24,
        .x = position[0], .y = position[1], .z = position[2]
    };
    return true;
}

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
typedef struct {
    uint32_t realm_epoch_high;
    uint32_t realm_epoch_low;
    int64_t canvas_handle;
    int width;
    int height;
    uint32_t display_epoch;
    float temporal_matrix[16];
    bool temporal_matrix_valid;
    bool valid;
} WebglGeSurfaceOwner;
static WebglGeSurfaceOwner webgl_ge_surface_owner;
static WebglGeTextureCacheEntry
    webgl_ge_texture_cache[WEBGL_TEXTURE_LIMIT];
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
static ScriptWebglNativeMetrics webgl_native_metrics;
#define WEBGL_NATIVE_TIMING_SAMPLE_LIMIT 1024u
static uint32_t webgl_native_total_samples[WEBGL_NATIVE_TIMING_SAMPLE_LIMIT];
static uint32_t webgl_native_sorted_samples[WEBGL_NATIVE_TIMING_SAMPLE_LIMIT];
static size_t webgl_native_total_sample_count;
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
    const uint8_t *commands, size_t command_count, bool antialias,
    size_t *vertex_capacity, size_t *validity_capacity,
    size_t *edge_index_capacity, size_t *instance_capacity)
{
    if (commands == NULL || vertex_capacity == NULL
        || validity_capacity == NULL || edge_index_capacity == NULL
        || instance_capacity == NULL)
        return false;
    size_t vertices = 0, maximum_draw = 0, edge_indices = 0;
    size_t maximum_instances = 0;
    for (size_t at = 0; at < command_count; at++) {
        const uint8_t *command = commands
            + at * WEBGL_COMMAND_WORDS * sizeof(uint32_t);
        int kind = 0;
        if (!webgl_integer_in_range(webgl_wire_i32(command, 0u), 0, 1,
                                    &kind)) return false;
        if (kind == 0) continue;
        int mode = 0, count = 0, instances = 0;
        if (!webgl_integer_in_range(webgl_wire_i32(command, 1u),
                                    WEBGL_POINTS, WEBGL_TRIANGLE_FAN, &mode)
            || !webgl_integer_in_range(webgl_wire_i32(command, 3u), 0,
                                       WEBGL_VERTEX_LIMIT, &count)
            || !webgl_integer_in_range(webgl_wire_i32(command, 76u), 1,
                                       WEBGL_INSTANCE_LIMIT, &instances)
            || (count != 0 && (size_t) instances
                   > WEBGL_VERTEX_LIMIT / (size_t) count)) return false;
        size_t required = (size_t) count
            + (mode == WEBGL_LINE_LOOP && count > 1 ? 1u : 0u);
        int instance_color_source = 0;
        if (!webgl_integer_in_range(webgl_wire_i32(command, 83u), -1,
                                    WEBGL_SOURCE_LIMIT - 1,
                                    &instance_color_source)) return false;
        if (instance_color_source >= 0) {
            if (required != 0u
                && (size_t) instances > SIZE_MAX / required) return false;
            required *= (size_t) instances;
        }
        if (required > WEBGL_VERTEX_LIMIT + 1u
            || vertices > WEBGL_VERTEX_LIMIT + WEBGL_COMMAND_LIMIT
            || required > WEBGL_VERTEX_LIMIT + WEBGL_COMMAND_LIMIT - vertices)
            return false;
        vertices += required;
        if ((size_t) count > maximum_draw) maximum_draw = (size_t) count;
        if ((size_t) instances > maximum_instances)
            maximum_instances = (size_t) instances;
        if (antialias && mode >= WEBGL_TRIANGLES) {
            size_t triangles = mode == WEBGL_TRIANGLES
                ? (size_t) count / 3u
                : count > 2 ? (size_t) count - 2u : 0u;
            size_t wanted = triangles * 6u;
            if (script_runtime_webgl_antialias_edges_admit(
                    edge_indices, wanted))
                edge_indices += wanted;
        }
    }
    *vertex_capacity = vertices;
    *validity_capacity = maximum_draw;
    *edge_index_capacity = edge_indices;
    *instance_capacity = maximum_instances;
    return true;
}

static size_t webgl_ge_triangle_edge_indices(
    int mode, int count, const WebglGeVertex *vertices,
    uint16_t *indices, size_t capacity)
{
    if (indices == NULL || vertices == NULL || count < 3
        || mode < WEBGL_TRIANGLES
        || mode > WEBGL_TRIANGLE_FAN) return 0;
    size_t triangles = mode == WEBGL_TRIANGLES
        ? (size_t) count / 3u : (size_t) count - 2u;
    if (triangles > capacity / 6u) return 0;
    size_t out = 0;
    /* Exported box meshes commonly place the two triangles of one face next
       to each other.  Cancel their one byte-identical shared edge locally;
       this removes the invisible diagonal with constant work and no mesh-
       sized hash table.  Other topology keeps the conservative full fringe. */
    if (mode == WEBGL_TRIANGLES) {
        size_t triangle = 0;
        for (; triangle + 1u < triangles; triangle += 2u) {
            unsigned edge_vertices[6][2] = {
                {(unsigned) triangle * 3u,
                 (unsigned) triangle * 3u + 1u},
                {(unsigned) triangle * 3u + 1u,
                 (unsigned) triangle * 3u + 2u},
                {(unsigned) triangle * 3u + 2u,
                 (unsigned) triangle * 3u},
                {(unsigned) triangle * 3u + 3u,
                 (unsigned) triangle * 3u + 4u},
                {(unsigned) triangle * 3u + 4u,
                 (unsigned) triangle * 3u + 5u},
                {(unsigned) triangle * 3u + 5u,
                 (unsigned) triangle * 3u + 3u}
            };
            int shared_left = -1, shared_right = -1;
            for (int left = 0; left < 3 && shared_left < 0; left++) {
                for (int right = 3; right < 6; right++) {
                    const WebglGeVertex *la =
                        &vertices[edge_vertices[left][0]];
                    const WebglGeVertex *lb =
                        &vertices[edge_vertices[left][1]];
                    const WebglGeVertex *ra =
                        &vertices[edge_vertices[right][0]];
                    const WebglGeVertex *rb =
                        &vertices[edge_vertices[right][1]];
                    if ((memcmp(la, ra, sizeof(*la)) == 0
                         && memcmp(lb, rb, sizeof(*lb)) == 0)
                        || (memcmp(la, rb, sizeof(*la)) == 0
                            && memcmp(lb, ra, sizeof(*lb)) == 0)) {
                        shared_left = left;
                        shared_right = right;
                        break;
                    }
                }
            }
            for (int edge = 0; edge < 6; edge++) {
                if (edge == shared_left || edge == shared_right) continue;
                indices[out++] = (uint16_t) edge_vertices[edge][0];
                indices[out++] = (uint16_t) edge_vertices[edge][1];
            }
        }
        if (triangle < triangles) {
            unsigned a = (unsigned) triangle * 3u;
            indices[out++] = (uint16_t) a;
            indices[out++] = (uint16_t) (a + 1u);
            indices[out++] = (uint16_t) (a + 1u);
            indices[out++] = (uint16_t) (a + 2u);
            indices[out++] = (uint16_t) (a + 2u);
            indices[out++] = (uint16_t) a;
        }
        return out;
    }
    for (size_t triangle = 0; triangle < triangles; triangle++) {
        unsigned a = 0, b = 0, c = 0;
        if (mode == WEBGL_TRIANGLE_STRIP) {
            a = (unsigned) triangle;
            b = a + 1u;
            c = a + 2u;
            if ((triangle & 1u) != 0u) {
                unsigned swap = a; a = b; b = swap;
            }
        } else {
            a = 0u;
            b = (unsigned) triangle + 1u;
            c = (unsigned) triangle + 2u;
        }
        indices[out++] = (uint16_t) a;
        indices[out++] = (uint16_t) b;
        indices[out++] = (uint16_t) b;
        indices[out++] = (uint16_t) c;
        indices[out++] = (uint16_t) c;
        indices[out++] = (uint16_t) a;
    }
    return out;
}

/* A smoothed fringe cannot improve a compact instance whose complete
   projected radius is below three output pixels. At one or two pixels the
   PSP's line-coverage result changes discontinuously as moving geometry
   crosses a pixel center; retaining only the filled primitive is steadier.
   Estimate a conservative
   squared screen radius from the three transformed object axes. Comparing
   squared values keeps the per-instance decision out of PSP soft-float sqrt;
   only this explicit compact transform form takes the shortcut. Full matrices
   retain the exact AA path because they may contain perspective or shear. */
static bool webgl_ge_compact_mesh_extent(
    const WebglGeVertex *vertices, size_t count, float extent[3])
{
    if (vertices == NULL || count == 0u || extent == NULL) return false;
    extent[0] = extent[1] = extent[2] = 0.0f;
    for (size_t vertex = 0; vertex < count; vertex++) {
        float values[3] = {
            fabsf(vertices[vertex].x), fabsf(vertices[vertex].y),
            fabsf(vertices[vertex].z)
        };
        for (size_t axis = 0; axis < 3u; axis++) {
            if (!isfinite(values[axis])) return false;
            if (values[axis] > extent[axis]) extent[axis] = values[axis];
        }
    }
    return true;
}

static bool webgl_ge_compact_instance_needs_antialias(
    const WebglDecodedDraw *draw, const float instance_matrix[16],
    const float extent[3], float *projected_radius_squared)
{
    if (projected_radius_squared != NULL)
        *projected_radius_squared = -1.0f;
    if (draw == NULL || extent == NULL
        || instance_matrix == NULL
        || draw->instance_transform.source == NULL
        || draw->instance_matrix.source != NULL) return true;
    float transform[4] = {
        instance_matrix[12], instance_matrix[13], instance_matrix[14],
        instance_matrix[0]
    };
    float center[4];
    for (size_t row = 0; row < 4u; row++) {
        center[row] = draw->matrix[12u + row]
            + transform[0] * draw->matrix[row]
            + transform[1] * draw->matrix[4u + row]
            + transform[2] * draw->matrix[8u + row];
        if (!isfinite(center[row])) return true;
    }
    if (fabsf(center[3]) < 0.0001f) return true;
    float center_x = center[0] / center[3];
    float center_y = center[1] / center[3];
    float radius_pixels_squared = 0.0f;
    for (size_t axis = 0; axis < 3u; axis++) {
        float scale = transform[3] * extent[axis];
        float sample_w = center[3] + draw->matrix[axis * 4u + 3u] * scale;
        if (!isfinite(scale) || fabsf(sample_w) < 0.0001f) return true;
        float sample_x = (center[0] + draw->matrix[axis * 4u] * scale)
            / sample_w;
        float sample_y = (center[1] + draw->matrix[axis * 4u + 1u] * scale)
            / sample_w;
        float dx = (sample_x - center_x) * draw->viewport_width * 0.5f;
        float dy = (sample_y - center_y) * draw->viewport_height * 0.5f;
        float contribution_squared = dx * dx + dy * dy;
        if (!isfinite(contribution_squared)) return true;
        radius_pixels_squared += contribution_squared;
    }
    if (projected_radius_squared != NULL)
        *projected_radius_squared = radius_pixels_squared;
    return script_runtime_webgl_antialias_radius_admit(
        radius_pixels_squared);
}

static void webgl_ge_geometry_cache_clear(DomBridge *bridge)
{
    if (bridge == NULL) return;
    for (size_t i = 0; i < SCRIPT_WEBGL_GEOMETRY_CACHE_ENTRY_LIMIT; i++) {
        budget_free(bridge->budget, bridge->webgl_geometry_vertices[i]);
        bridge->webgl_geometry_vertices[i] = NULL;
    }
    memset(&bridge->webgl_geometry_cache, 0,
           sizeof(bridge->webgl_geometry_cache));
}

static void webgl_ge_geometry_cache_abandon(DomBridge *bridge, size_t slot)
{
    if (bridge == NULL || slot >= SCRIPT_WEBGL_GEOMETRY_CACHE_ENTRY_LIMIT)
        return;
    ScriptWebglGeometryCacheRecord *record =
        &bridge->webgl_geometry_cache.records[slot];
    if (record->valid) {
        bridge->webgl_geometry_cache.retained_bytes -= record->bytes;
        memset(record, 0, sizeof(*record));
    }
    budget_free(bridge->budget, bridge->webgl_geometry_vertices[slot]);
    bridge->webgl_geometry_vertices[slot] = NULL;
}

static bool webgl_render_ge(DomBridge *bridge,
                            int64_t canvas_handle,
                            uint8_t *surface,
                            uint16_t *depth,
                            int width, int height,
                            const uint8_t *commands, size_t command_count,
                            const WebglSource *sources, size_t source_count,
                            const WebglTexture *textures, size_t texture_count,
                            bool antialias, bool preserve_drawing_buffer,
                            bool *native_authoritative)
{
    if (native_authoritative != NULL) *native_authoritative = false;
    if (bridge == NULL || bridge->budget == NULL) return false;
    Budget *budget = bridge->budget;
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
    uint64_t total_started = (uint64_t) sceKernelGetSystemTimeWide();
#endif
    if (!psp_media_present_ge_context_acquire()
        || !psp_media_present_ge_context_idle()) return false;
    size_t ge_vertex_capacity = 0, validity_capacity = 0;
    size_t edge_index_capacity = 0, instance_capacity = 0;
    if (!webgl_ge_vertex_storage_required(
            commands, command_count, antialias, &ge_vertex_capacity,
            &validity_capacity, &edge_index_capacity,
            &instance_capacity)) return false;
    size_t temporal_vertex_capacity = antialias
        ? (edge_index_capacity < WEBGL_GE_TAA_VERTEX_LIMIT
            ? edge_index_capacity : WEBGL_GE_TAA_VERTEX_LIMIT) : 0u;
    if (temporal_vertex_capacity > SIZE_MAX - ge_vertex_capacity)
        return false;
    size_t scratch_vertex_capacity = ge_vertex_capacity
        + temporal_vertex_capacity;
    size_t scratch_bytes = scratch_vertex_capacity * sizeof(WebglGeVertex)
        + instance_capacity * 16u * sizeof(float)
        + edge_index_capacity * sizeof(uint16_t) + validity_capacity;
    if (scratch_bytes == 0u) scratch_bytes = 1u;
    WebglGeVertex *scratch = budget_malloc_category(
        budget, BUDGET_CATEGORY_RENDER, scratch_bytes);
    if (scratch == NULL) return false;
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
    webgl_native_metrics.scratch_bytes += scratch_bytes;
    if (scratch_bytes > webgl_native_metrics.maximum_scratch_bytes)
        webgl_native_metrics.maximum_scratch_bytes = scratch_bytes;
#endif
    WebglGeVertex *temporal_vertices = scratch + ge_vertex_capacity;
    float *instance_matrices = (float *) (
        scratch + scratch_vertex_capacity);
    uint16_t *edge_indices = (uint16_t *) (
        instance_matrices + instance_capacity * 16u);
    uint8_t *vertex_validity = (uint8_t *) (
        edge_indices + edge_index_capacity);
    void *edram = sceGeEdramGetAddr();
    if (edram == NULL) { budget_free(budget, scratch); return false; }
    uint8_t *color_target = (uint8_t *) edram + WEBGL_GE_COLOR_OFFSET;
    size_t color_cache_bytes = ((size_t) (height - 1)
        * PSP_DISPLAY_STRIDE + (size_t) width) * sizeof(uint32_t);
    size_t depth_cache_bytes = ((size_t) (height - 1)
        * PSP_DISPLAY_STRIDE + (size_t) width) * sizeof(uint16_t);
    unsigned initial_clear = webgl_ge_initial_full_clear_mask(
        commands, command_count);
    uint32_t display_content_epoch = psp_display_edram_content_epoch();
    bool same_surface = webgl_ge_surface_owner.valid
        && webgl_ge_surface_owner.display_epoch == display_content_epoch
        && webgl_ge_surface_owner.realm_epoch_high
               == bridge->webgl_realm_epoch_high
        && webgl_ge_surface_owner.realm_epoch_low
               == bridge->webgl_realm_epoch_low
        && webgl_ge_surface_owner.canvas_handle == canvas_handle
        && webgl_ge_surface_owner.width == width
        && webgl_ge_surface_owner.height == height;
    if (webgl_ge_surface_owner.valid && !same_surface) {
        if (webgl_ge_surface_owner.display_epoch == display_content_epoch
            && webgl_ge_surface_owner.realm_epoch_high
                   == bridge->webgl_realm_epoch_high
            && webgl_ge_surface_owner.realm_epoch_low
                   == bridge->webgl_realm_epoch_low) {
            lxb_dom_node_t *prior = webgl_resolve_canvas(
                bridge, webgl_ge_surface_owner.canvas_handle);
            if (prior != NULL) (void) images_materialize_canvas_native_surface(
                bridge->images, prior);
        }
        memset(&webgl_ge_surface_owner, 0,
               sizeof(webgl_ge_surface_owner));
    }
    if ((initial_clear & WEBGL_COLOR_BUFFER_BIT) == 0u && !same_surface) {
        for (int y = 0; y < height; y++) memcpy(
            color_target + (size_t) y * PSP_DISPLAY_STRIDE * 4u,
            surface + (size_t) y * (size_t) width * 4u,
            (size_t) width * 4u);
        sceKernelDcacheWritebackRange(
            color_target, color_cache_bytes);
    }
    if (depth != NULL
        && (initial_clear & WEBGL_DEPTH_BUFFER_BIT) == 0u
        && !same_surface) {
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
    if (webgl_ge_display_content_epoch != display_content_epoch) {
        webgl_ge_display_content_epoch = display_content_epoch;
        webgl_ge_texture_owner.valid = false;
    }
    if (!script_runtime_webgl_cache_admit(
            &webgl_ge_texture_owner,
            bridge->webgl_realm_epoch_high,
            bridge->webgl_realm_epoch_low,
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
    /* The bounded context advertises alpha:false. On a 8888 GE target the
       alpha byte is also the stencil plane. Seed it to opaque on clears, then
       mask alpha writes from ordinary draws. This makes the resolved surface
       directly copyable while preserving its required 0xff alpha byte. */
    sceGuClearStencil(0xffu);
    sceGuPixelMask(UINT32_C(0xff000000));
    size_t vertex_cursor = 0, edge_index_cursor = 0;
    size_t antialias_draws_used = 0;
    size_t temporal_vertex_cursor = 0, temporal_draws_used = 0;
    float temporal_frame_matrix[16] = {0};
    bool temporal_frame_matrix_valid = false;
    bool temporal_history_available = antialias && same_surface
        && webgl_ge_surface_owner.temporal_matrix_valid
        && (initial_clear & WEBGL_COLOR_BUFFER_BIT) != 0u;
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
    size_t frame_antialias_draws = 0;
    size_t frame_antialias_edge_indices = 0;
    uint64_t frame_decode_us = 0, frame_cache_us = 0;
    uint64_t frame_vertex_us = 0, frame_state_us = 0;
    uint64_t frame_instance_matrix_us = 0, frame_instance_color_us = 0;
    uint64_t frame_emit_us = 0, frame_antialias_us = 0;
    uint64_t frame_writeback_us = 0, frame_finalize_us = 0;
    size_t frame_commands = 0, frame_draw_calls = 0;
    size_t frame_matrix_loads = 0, frame_matrix_loads_avoided = 0;
    size_t frame_state_changes = 0, frame_state_changes_avoided = 0;
#endif
    bool projection_valid = false, view_identity_loaded = false;
    bool viewport_valid = false;
    float loaded_projection[16] = {0};
    int loaded_viewport_x = 0, loaded_viewport_y = 0;
    int loaded_viewport_width = 0, loaded_viewport_height = 0;
    for (size_t ci = 0; ci < command_count; ci++) {
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
        uint64_t decode_started = (uint64_t) sceKernelGetSystemTimeWide();
        frame_commands++;
#endif
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
            if (mask & WEBGL_COLOR_BUFFER_BIT) {
                /* Temporarily admit alpha/stencil for this clear. Geometry
                   resumes with alpha masked immediately afterward. */
                sceGuPixelMask(0u);
                sceGuClearStencil(0xffu);
                bits |= GU_COLOR_BUFFER_BIT | GU_STENCIL_BUFFER_BIT;
            }
            if (mask & WEBGL_DEPTH_BUFFER_BIT) bits |= GU_DEPTH_BUFFER_BIT;
            if (bits != 0) sceGuClear(bits);
            if (mask & WEBGL_COLOR_BUFFER_BIT)
                sceGuPixelMask(UINT32_C(0xff000000));
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
            frame_decode_us += (uint64_t) sceKernelGetSystemTimeWide()
                - decode_started;
#endif
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
        WebglDecodedDraw draw;
        if (!webgl_decode_draw(command, sources, source_count,
                               width, height, &draw)) {
            sceGuFinish(); sceGuSync(0, 0); budget_free(budget, scratch);
            return false;
        }
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
        frame_decode_us += (uint64_t) sceKernelGetSystemTimeWide()
            - decode_started;
        uint64_t cache_started = (uint64_t) sceKernelGetSystemTimeWide();
#endif
        size_t vertex_copies = draw.instance_color.source != NULL
            ? draw.instance_count : 1u;
        if (command_vertices != 0u
            && vertex_copies > SIZE_MAX / command_vertices) {
            sceGuFinish(); sceGuSync(0, 0); budget_free(budget, scratch);
            return false;
        }
        size_t stored_vertices = command_vertices * vertex_copies;
        if (vertex_cursor > ge_vertex_capacity
            || stored_vertices > ge_vertex_capacity - vertex_cursor) {
            sceGuFinish(); sceGuSync(0, 0); budget_free(budget, scratch);
            return false;
        }
        WebglGeVertex *draw_vertices = scratch + vertex_cursor;
        bool geometry_valid = true;
        bool fast = webgl_ge_fast_layout(&draw);
        bool cacheable = false, cache_hit = false, cache_promote = false;
        bool cache_reset = false;
        size_t cache_slot = 0;
        size_t previous_cache_bytes[
            SCRIPT_WEBGL_GEOMETRY_CACHE_ENTRY_LIMIT] = {0};
        for (size_t slot = 0;
             slot < SCRIPT_WEBGL_GEOMETRY_CACHE_ENTRY_LIMIT; slot++) {
            if (bridge->webgl_geometry_vertices[slot] != NULL)
                previous_cache_bytes[slot] =
                    bridge->webgl_geometry_cache.records[slot].bytes;
        }
        ScriptWebglGeometryCacheSignature cache_signature;
        size_t cache_bytes = (size_t) count * sizeof(*draw_vertices);
        if (fast && count > 0
            && webgl_ge_stable_signature(command, &cache_signature)
            && script_runtime_webgl_geometry_cache_admit(
                &bridge->webgl_geometry_cache,
                bridge->webgl_realm_epoch_high,
                bridge->webgl_realm_epoch_low,
                canvas_handle, &cache_signature, cache_bytes,
                &cache_slot, &cache_hit, &cache_reset)) {
            cacheable = true;
            for (size_t slot = 0;
                 slot < SCRIPT_WEBGL_GEOMETRY_CACHE_ENTRY_LIMIT; slot++) {
                if ((cache_reset
                     || !bridge->webgl_geometry_cache.records[slot].valid)
                    && bridge->webgl_geometry_vertices[slot] != NULL) {
                    budget_free(budget, bridge->webgl_geometry_vertices[slot]);
                    bridge->webgl_geometry_vertices[slot] = NULL;
                }
            }
            if (cache_hit
                && bridge->webgl_geometry_vertices[cache_slot] != NULL) {
                memcpy(draw_vertices,
                       bridge->webgl_geometry_vertices[cache_slot],
                       cache_bytes);
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
                webgl_native_metrics.geometry_cache_hit_vertices
                    += (size_t) count;
#endif
            } else {
                /* Admission may replace a valid LRU record in-place. Reuse
                   its allocation only when the translated byte size is
                   identical. Dynamic HUD generations otherwise paid a free
                   and equal-sized allocation before every possible hit. */
                bool reuse_equal_block = !cache_hit
                    && bridge->webgl_geometry_vertices[cache_slot] != NULL
                    && previous_cache_bytes[cache_slot] == cache_bytes;
                cache_promote = cache_hit || reuse_equal_block;
                cache_hit = false;
                if (!reuse_equal_block) {
                    budget_free(
                        budget, bridge->webgl_geometry_vertices[cache_slot]);
                    bridge->webgl_geometry_vertices[cache_slot] = NULL;
                }
            }
        }
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
        frame_cache_us += (uint64_t) sceKernelGetSystemTimeWide()
            - cache_started;
        uint64_t base_vertex_started =
            (uint64_t) sceKernelGetSystemTimeWide();
#endif
        if (!cache_hit) {
            for (int i = 0; i < count; i++) {
                bool valid = false;
                if (!webgl_ge_vertex(
                        &draw, (uint32_t) i, fast,
                        &draw_vertices[i], &valid)) {
                    sceGuFinish(); sceGuSync(0, 0);
                    budget_free(budget, scratch);
                    return false;
                }
                vertex_validity[i] = valid ? 1u : 0u;
                if (!valid) geometry_valid = false;
            }
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
            if (fast) webgl_native_metrics.fast_vertices += (size_t) count;
            if (cacheable) webgl_native_metrics.geometry_cache_misses++;
#endif
            if (cacheable && cache_promote && geometry_valid) {
                void *retained =
                    bridge->webgl_geometry_vertices[cache_slot];
                if (retained == NULL) retained = budget_malloc_category(
                    budget, BUDGET_CATEGORY_RENDER, cache_bytes);
                if (retained != NULL) {
                    memcpy(retained, draw_vertices, cache_bytes);
                    bridge->webgl_geometry_vertices[cache_slot] = retained;
                } else webgl_ge_geometry_cache_abandon(bridge, cache_slot);
            } else if (cacheable && !geometry_valid) {
                webgl_ge_geometry_cache_abandon(bridge, cache_slot);
            }
        }
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
        frame_vertex_us += (uint64_t) sceKernelGetSystemTimeWide()
            - base_vertex_started;
        uint64_t state_started = (uint64_t) sceKernelGetSystemTimeWide();
#endif
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
        if (!projection_valid
            || memcmp(loaded_projection, draw.matrix,
                      sizeof(loaded_projection)) != 0) {
            sceGumMatrixMode(GU_PROJECTION);
            sceGumLoadMatrix(&projection);
            memcpy(loaded_projection, draw.matrix,
                   sizeof(loaded_projection));
            projection_valid = true;
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
            frame_matrix_loads++;
#endif
        } else {
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
            frame_matrix_loads_avoided++;
#endif
        }
        if (!view_identity_loaded) {
            sceGumMatrixMode(GU_VIEW);
            sceGumLoadMatrix(&webgl_ge_identity);
            view_identity_loaded = true;
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
            frame_matrix_loads++;
#endif
        } else {
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
            frame_matrix_loads_avoided++;
#endif
        }
        if (!viewport_valid
            || loaded_viewport_x != (int) viewport_center_x
            || loaded_viewport_y != (int) viewport_center_y
            || loaded_viewport_width != draw.viewport_width
            || loaded_viewport_height != draw.viewport_height) {
            sceGuViewport((int) viewport_center_x, (int) viewport_center_y,
                          draw.viewport_width, draw.viewport_height);
            loaded_viewport_x = (int) viewport_center_x;
            loaded_viewport_y = (int) viewport_center_y;
            loaded_viewport_width = draw.viewport_width;
            loaded_viewport_height = draw.viewport_height;
            viewport_valid = true;
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
            frame_state_changes++;
#endif
        } else {
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
            frame_state_changes_avoided++;
#endif
        }
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
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
        frame_state_us += (uint64_t) sceKernelGetSystemTimeWide()
            - state_started;
        uint64_t instance_vertex_started =
            (uint64_t) sceKernelGetSystemTimeWide();
#endif
        size_t ge_count = (size_t) count;
        if (mode == WEBGL_LINE_LOOP && count > 1) {
            draw_vertices[count] = draw_vertices[0];
            ge_count++;
        }
        /* Decode each immutable instance transform once. It is consumed by
           the primary draw and may be consumed again by the bounded AA pass;
           re-reading attributes and rebuilding 16 floats in both loops was
           the dominant command-emission spike in particle-heavy scenes. */
        if (draw.instance_count > instance_capacity) {
            sceGuFinish(); sceGuSync(0, 0); budget_free(budget, scratch);
            return false;
        }
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
        uint64_t instance_matrix_started =
            (uint64_t) sceKernelGetSystemTimeWide();
#endif
        for (uint32_t instance = 0; instance < draw.instance_count;
             instance++) {
            if (!webgl_instance_matrix(
                    &draw, instance,
                    instance_matrices + (size_t) instance * 16u)) {
                sceGuFinish(); sceGuSync(0, 0); budget_free(budget, scratch);
                return false;
            }
        }
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
        frame_instance_matrix_us +=
            (uint64_t) sceKernelGetSystemTimeWide() - instance_matrix_started;
        uint64_t instance_color_started =
            (uint64_t) sceKernelGetSystemTimeWide();
#endif
        /* The GE cannot bind a second per-instance color stream. Keep the
           retained base mesh, then make one tiny colored vertex slice per
           instance. The JS/native work ceiling bounds the total to 4096
           vertices and the copies are emitted from back to front so the
           first slice remains the source until every snapshot exists. */
        if (draw.instance_color.source != NULL) {
            for (uint32_t instance = draw.instance_count; instance-- > 0u;) {
                WebglGeVertex *instance_vertices =
                    draw_vertices + (size_t) instance * ge_count;
                if (instance != 0u) memcpy(
                    instance_vertices, draw_vertices,
                    ge_count * sizeof(*instance_vertices));
                if (draw.instance_color.source != NULL) {
                    float tint[4];
                    if (!webgl_instance_color(&draw, instance, tint)) {
                        sceGuFinish(); sceGuSync(0, 0);
                        budget_free(budget, scratch); return false;
                    }
                    /* Indexed meshes commonly repeat a small authored face
                       palette. Modulate each distinct packed color once per
                       instance instead of repeating four Allegrex soft-float
                       conversions for every expanded index. Eight entries
                       cover the measured box/game meshes; excess colors use
                       the exact former calculation without growing storage. */
                    uint32_t source_colors[8], tinted_colors[8];
                    size_t cached_colors = 0;
                    uint32_t last_source = 0, last_tinted = 0;
                    bool have_last = false;
                    for (size_t vertex = 0; vertex < ge_count; vertex++) {
                        uint32_t color = instance_vertices[vertex].color;
                        uint32_t tinted = last_tinted;
                        if (!have_last || color != last_source) {
                            size_t cached = 0;
                            while (cached < cached_colors
                                   && source_colors[cached] != color) cached++;
                            tinted = cached < cached_colors
                                ? tinted_colors[cached]
                                : webgl_color_modulate(color, tint);
                            if (cached == cached_colors
                                && cached_colors < 8u) {
                                source_colors[cached_colors] = color;
                                tinted_colors[cached_colors] = tinted;
                                cached_colors++;
                            }
                            last_source = color;
                            last_tinted = tinted;
                            have_last = true;
                        }
                        instance_vertices[vertex].color = tinted;
                    }
                }
            }
        }
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
        frame_instance_color_us +=
            (uint64_t) sceKernelGetSystemTimeWide() - instance_color_started;
#endif
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
        frame_vertex_us += (uint64_t) sceKernelGetSystemTimeWide()
            - instance_vertex_started;
#endif
        /* GU_DIRECT may let the GE consume a draw before sceGuFinish().
           Publish this command's complete, bounded vertex range before its
           first draw is emitted. Deferring one aggregate writeback until the
           end of list construction leaves a hardware race in which the GE
           can fetch stale scratch vertices and stretch a triangle across the
           framebuffer. One range covers every per-instance color copy. */
        size_t published_vertex_copies = draw.instance_color.source != NULL
            ? draw.instance_count : 1u;
        if (ge_count != 0u && published_vertex_copies != 0u) {
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
            uint64_t writeback_started =
                (uint64_t) sceKernelGetSystemTimeWide();
#endif
            sceKernelDcacheWritebackRange(
                draw_vertices,
                ge_count * published_vertex_copies * sizeof(*draw_vertices));
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
            frame_writeback_us += (uint64_t) sceKernelGetSystemTimeWide()
                - writeback_started;
#endif
        }
        bool smooth_authored_lines = antialias
            && mode >= WEBGL_LINES && mode <= WEBGL_LINE_STRIP;
        if (smooth_authored_lines) sceGuEnable(GU_LINE_SMOOTH);
        else sceGuDisable(GU_LINE_SMOOTH);
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
        uint64_t primary_emit_started =
            (uint64_t) sceKernelGetSystemTimeWide();
#endif
        for (uint32_t instance = 0; instance < draw.instance_count;
             instance++) {
            ScePspFMatrix4 instance_matrix;
            memcpy(&instance_matrix,
                   instance_matrices + (size_t) instance * 16u,
                   sizeof(instance_matrix));
            sceGumMatrixMode(GU_MODEL);
            sceGumLoadMatrix(&instance_matrix);
            WebglGeVertex *instance_vertices = draw_vertices
                + (draw.instance_color.source != NULL
                    ? (size_t) instance * ge_count : 0u);
            sceGumDrawArray(webgl_ge_primitive(mode),
                GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF
                    | GU_TRANSFORM_3D,
                (int) ge_count, NULL, instance_vertices);
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
            frame_matrix_loads++;
            frame_draw_calls++;
#endif
        }
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
        frame_emit_us += (uint64_t) sceKernelGetSystemTimeWide()
            - primary_emit_started;
#endif
        /* The PSP GE has no multisample framebuffer, but it can apply
           hardware coverage to line fragments.  Re-submit triangle edges as
           uint16 indices into the already transformed vertex array.  This
           keeps AA transient (two bytes per edge endpoint), and culling skips
           it because line primitives cannot reproduce a culled face test. */
        if (antialias && !cull && mode >= WEBGL_TRIANGLES
            && edge_index_cursor < edge_index_capacity) {
            float compact_extent[3];
            bool compact_extent_valid =
                draw.instance_transform.source != NULL
                && webgl_ge_compact_mesh_extent(
                    draw_vertices, ge_count, compact_extent);
            size_t edge_count = webgl_ge_triangle_edge_indices(
                mode, count, draw_vertices,
                edge_indices + edge_index_cursor,
                edge_index_capacity - edge_index_cursor);
            if (edge_count != 0u) {
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
                uint64_t aa_started =
                    (uint64_t) sceKernelGetSystemTimeWide();
#endif
                /* GU_DIRECT can begin consuming an emitted draw before list
                   construction finishes, so its index stream follows the
                   same publish-before-submit rule as vertex streams. */
                sceKernelDcacheWritebackRange(
                    edge_indices + edge_index_cursor,
                    edge_count * sizeof(*edge_indices));
                if (!temporal_frame_matrix_valid) {
                    memcpy(temporal_frame_matrix, draw.matrix,
                           sizeof(temporal_frame_matrix));
                    temporal_frame_matrix_valid = true;
                }
                bool temporal_moved = false;
                bool temporal_command = temporal_history_available
                    && script_runtime_webgl_temporal_geometry_admit(
                        draw.instance_matrix.source != NULL,
                        draw.instance_transform.source != NULL)
                    && webgl_temporal_matrix_same(
                        temporal_frame_matrix, draw.matrix)
                    && script_runtime_webgl_temporal_matrix_admit(
                        webgl_ge_surface_owner.temporal_matrix,
                        temporal_frame_matrix, &temporal_moved)
                    && temporal_moved;
                sceGuEnable(GU_LINE_SMOOTH);
                sceGuEnable(GU_BLEND);
                sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA,
                               GU_ONE_MINUS_SRC_ALPHA, 0, 0);
                if (webgl_wire_i32(command, 28u) != 0) {
                    sceGuEnable(GU_DEPTH_TEST);
                    sceGuDepthFunc(GU_LEQUAL);
                    sceGuDepthMask(GU_TRUE);
                }
                uint32_t antialias_instances = 0;
                uint32_t temporal_instances = 0;
                for (uint32_t instance = 0;
                     instance < draw.instance_count; instance++) {
                    if (antialias_draws_used >= WEBGL_GE_AA_DRAW_LIMIT)
                        break;
                    float projected_radius_squared = -1.0f;
                    bool needs_antialias =
                        webgl_ge_compact_instance_needs_antialias(
                            &draw,
                            instance_matrices + (size_t) instance * 16u,
                            compact_extent_valid ? compact_extent : NULL,
                            &projected_radius_squared);
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
                    if (projected_radius_squared >= 0.0f) {
                        webgl_native_metrics.antialias_compact_instances++;
                        if (projected_radius_squared < 4.0f)
                            webgl_native_metrics.antialias_radius_lt2++;
                        if (projected_radius_squared < 9.0f)
                            webgl_native_metrics.antialias_radius_lt3++;
                        if (projected_radius_squared < 16.0f)
                            webgl_native_metrics.antialias_radius_lt4++;
                    }
#endif
                    if (!needs_antialias) continue;
                    ScePspFMatrix4 instance_matrix;
                    memcpy(&instance_matrix,
                           instance_matrices + (size_t) instance * 16u,
                           sizeof(instance_matrix));
                    WebglGeVertex *instance_vertices = draw_vertices
                        + (draw.instance_color.source != NULL
                            ? (size_t) instance * ge_count : 0u);
                    const uint16_t *edges = edge_indices
                        + edge_index_cursor;
                    if (temporal_command
                        && temporal_draws_used < WEBGL_GE_TAA_DRAW_LIMIT
                        && temporal_vertex_cursor
                               <= temporal_vertex_capacity
                        && edge_count <= temporal_vertex_capacity
                               - temporal_vertex_cursor) {
                        WebglGeVertex *history = temporal_vertices
                            + temporal_vertex_cursor;
                        for (size_t edge = 0; edge < edge_count; edge++) {
                            history[edge] = instance_vertices[edges[edge]];
                            uint32_t color = history[edge].color;
                            uint32_t alpha = (color >> 24) & 0xffu;
                            alpha = (alpha + 3u) / 4u;
                            history[edge].color = (color & UINT32_C(0x00ffffff))
                                | (alpha << 24);
                        }
                        ScePspFMatrix4 prior_projection;
                        memcpy(&prior_projection,
                               webgl_ge_surface_owner.temporal_matrix,
                               sizeof(prior_projection));
                        sceGumMatrixMode(GU_PROJECTION);
                        sceGumLoadMatrix(&prior_projection);
                        sceGumMatrixMode(GU_MODEL);
                        sceGumLoadMatrix(&instance_matrix);
                        sceKernelDcacheWritebackRange(
                            history, edge_count * sizeof(*history));
                        sceGumDrawArray(
                            GU_LINES,
                            GU_TEXTURE_32BITF | GU_COLOR_8888
                                | GU_VERTEX_32BITF | GU_TRANSFORM_3D,
                            (int) edge_count, NULL, history);
                        sceGumMatrixMode(GU_PROJECTION);
                        sceGumLoadMatrix(&projection);
                        temporal_vertex_cursor += edge_count;
                        temporal_draws_used++;
                        temporal_instances++;
                    }
                    sceGumMatrixMode(GU_MODEL);
                    sceGumLoadMatrix(&instance_matrix);
                    sceGumDrawArray(
                        GU_LINES,
                        GU_INDEX_16BIT | GU_TEXTURE_32BITF
                            | GU_COLOR_8888 | GU_VERTEX_32BITF
                            | GU_TRANSFORM_3D,
                        (int) edge_count,
                        edge_indices + edge_index_cursor,
                        instance_vertices);
                    antialias_instances++;
                    antialias_draws_used++;
                }
                edge_index_cursor += edge_count;
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
                webgl_native_metrics.antialias_draws += antialias_instances;
                webgl_native_metrics.antialias_edge_indices +=
                    edge_count * antialias_instances;
                webgl_native_metrics.antialias_draws += temporal_instances;
                webgl_native_metrics.antialias_edge_indices +=
                    edge_count * temporal_instances;
                frame_antialias_draws +=
                    antialias_instances + temporal_instances;
                frame_antialias_edge_indices +=
                    edge_count * (antialias_instances + temporal_instances);
                frame_matrix_loads += antialias_instances
                    + temporal_instances;
                frame_draw_calls += antialias_instances
                    + temporal_instances;
                frame_antialias_us +=
                    (uint64_t) sceKernelGetSystemTimeWide() - aa_started;
#endif
                (void) antialias_instances;
                (void) temporal_instances;
            }
        }
        sceGuDisable(GU_LINE_SMOOTH);
        vertex_cursor += ge_count * vertex_copies;
    }
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
    uint64_t finalize_started =
        (uint64_t) sceKernelGetSystemTimeWide();
#endif
    int display_list_bytes = sceGuFinish();
#if !defined(TILEFINCH_PSP_VALIDATION_LOG)
    (void) display_list_bytes;
#endif
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
    uint64_t emitted = (uint64_t) sceKernelGetSystemTimeWide();
    frame_finalize_us += emitted - finalize_started;
#endif
    sceGuSync(0, 0);
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
    uint64_t synchronized = (uint64_t) sceKernelGetSystemTimeWide();
#endif
    uint8_t *color = (uint8_t *) edram + WEBGL_GE_COLOR_OFFSET;
    sceKernelDcacheInvalidateRange(color, color_cache_bytes);
    if (preserve_drawing_buffer) {
        /* Alpha/stencil was seeded to 0xff and protected from geometry
           writes. Preserve exact RGBA semantics only when the page requests
           them; the default drawing-only path is consumed directly from the
           authoritative EDRAM surface by the canvas frame renderer. */
        for (int y = 0; y < height; y++) memcpy(
            surface + (size_t) y * (size_t) width * 4u,
            color + (size_t) y * PSP_DISPLAY_STRIDE * 4u,
            (size_t) width * 4u);
    } else if (native_authoritative != NULL) {
        *native_authoritative = true;
    }
    /* A non-preserved default framebuffer whose command stream fully clears
       depth has no page-visible depth contents to retain. The GE surface is
       authoritative until this frame is composited, and the next frame will
       clear it again, so copying the complete 16-bit depth plane back to CPU
       memory only adds bandwidth to every animation frame. Preserve the copy
       for partial/no-clear command streams and preserveDrawingBuffer, where a
       later flush may legitimately depend on the retained values. */
    if (depth != NULL
        && script_runtime_webgl_depth_readback_required(
               preserve_drawing_buffer, initial_clear)) {
        uint8_t *depth_target = (uint8_t *) edram + WEBGL_GE_DEPTH_OFFSET;
        sceKernelDcacheInvalidateRange(depth_target, depth_cache_bytes);
        for (int y = 0; y < height; y++) memcpy(
            depth + (size_t) y * (size_t) width,
            depth_target + (size_t) y * PSP_DISPLAY_STRIDE * sizeof(uint16_t),
            (size_t) width * sizeof(uint16_t));
    }
    webgl_ge_surface_owner = (WebglGeSurfaceOwner) {
        .realm_epoch_high = bridge->webgl_realm_epoch_high,
        .realm_epoch_low = bridge->webgl_realm_epoch_low,
        .canvas_handle = canvas_handle,
        .width = width,
        .height = height,
        .display_epoch = display_content_epoch,
        .temporal_matrix_valid = antialias
            && temporal_frame_matrix_valid
            && (initial_clear & WEBGL_COLOR_BUFFER_BIT) != 0u,
        .valid = true
    };
    if (webgl_ge_surface_owner.temporal_matrix_valid) memcpy(
        webgl_ge_surface_owner.temporal_matrix,
        temporal_frame_matrix, sizeof(temporal_frame_matrix));
    budget_free(budget, scratch);
#if defined(TILEFINCH_PSP_VALIDATION_LOG)
    uint64_t completed = (uint64_t) sceKernelGetSystemTimeWide();
    uint64_t total_us = completed - total_started;
    uint64_t seed_us = seeded - total_started;
    uint64_t command_us = emitted - seeded;
    uint64_t sync_us = synchronized - emitted;
    uint64_t readback_us = completed - synchronized;
    webgl_native_metrics.frames++;
    webgl_native_metrics.vertices += vertex_cursor;
    webgl_native_metrics.seed_us += seed_us;
    webgl_native_metrics.command_us += command_us;
    webgl_native_metrics.command_decode_us += frame_decode_us;
    webgl_native_metrics.command_cache_us += frame_cache_us;
    webgl_native_metrics.command_vertex_us += frame_vertex_us;
    webgl_native_metrics.command_instance_matrix_us +=
        frame_instance_matrix_us;
    webgl_native_metrics.command_instance_color_us +=
        frame_instance_color_us;
    webgl_native_metrics.command_state_us += frame_state_us;
    webgl_native_metrics.command_emit_us += frame_emit_us;
    webgl_native_metrics.command_antialias_us += frame_antialias_us;
    webgl_native_metrics.command_writeback_us += frame_writeback_us;
    webgl_native_metrics.command_finalize_us += frame_finalize_us;
    webgl_native_metrics.sync_us += sync_us;
    webgl_native_metrics.readback_us += readback_us;
    webgl_native_metrics.total_us += total_us;
    bridge->webgl_native_total_us += total_us;
    bridge->webgl_native_frames++;
    webgl_native_metrics.commands += frame_commands;
    webgl_native_metrics.draw_calls += frame_draw_calls;
    webgl_native_metrics.matrix_loads += frame_matrix_loads;
    webgl_native_metrics.matrix_loads_avoided += frame_matrix_loads_avoided;
    webgl_native_metrics.state_changes += frame_state_changes;
    webgl_native_metrics.state_changes_avoided += frame_state_changes_avoided;
    if (display_list_bytes > 0) {
        webgl_native_metrics.display_list_bytes +=
            (size_t) display_list_bytes;
        if ((size_t) display_list_bytes
                > webgl_native_metrics.maximum_display_list_bytes)
            webgl_native_metrics.maximum_display_list_bytes =
                (size_t) display_list_bytes;
    }
    if (total_us > webgl_native_metrics.maximum_total_us) {
        webgl_native_metrics.maximum_total_us = total_us;
        webgl_native_metrics.maximum_total_seed_us = seed_us;
        webgl_native_metrics.maximum_total_command_us = command_us;
        webgl_native_metrics.maximum_total_sync_us = sync_us;
        webgl_native_metrics.maximum_total_readback_us = readback_us;
        webgl_native_metrics.maximum_total_vertices = vertex_cursor;
        webgl_native_metrics.maximum_total_antialias_draws =
            frame_antialias_draws;
        webgl_native_metrics.maximum_total_antialias_edge_indices =
            frame_antialias_edge_indices;
    }
    if (webgl_native_total_sample_count < WEBGL_NATIVE_TIMING_SAMPLE_LIMIT) {
        webgl_native_total_samples[webgl_native_total_sample_count++] =
            total_us > UINT32_MAX ? UINT32_MAX : (uint32_t) total_us;
    }
#endif
    return true;
}
#endif

bool script_runtime_webgl_native_metrics(ScriptWebglNativeMetrics *metrics)
{
    if (metrics == NULL) return false;
#if defined(__PSP__) && defined(TILEFINCH_PSP_VALIDATION_LOG)
    *metrics = webgl_native_metrics;
    metrics->sampled_frames = webgl_native_total_sample_count;
    if (webgl_native_total_sample_count != 0) {
        memcpy(webgl_native_sorted_samples, webgl_native_total_samples,
               webgl_native_total_sample_count
                   * sizeof(webgl_native_sorted_samples[0]));
        for (size_t at = 1; at < webgl_native_total_sample_count; at++) {
            uint32_t value = webgl_native_sorted_samples[at];
            size_t before = at;
            while (before != 0
                   && webgl_native_sorted_samples[before - 1u] > value) {
                webgl_native_sorted_samples[before] =
                    webgl_native_sorted_samples[before - 1u];
                before--;
            }
            webgl_native_sorted_samples[before] = value;
        }
        size_t median = (webgl_native_total_sample_count - 1u) / 2u;
        size_t p95 = (webgl_native_total_sample_count * 95u + 99u) / 100u;
        if (p95 != 0) p95--;
        metrics->median_total_us = webgl_native_sorted_samples[median];
        metrics->p95_total_us = webgl_native_sorted_samples[p95];
    }
    return metrics->frames != 0;
#else
    memset(metrics, 0, sizeof(*metrics));
    return false;
#endif
}

void script_runtime_webgl_native_metrics_reset(void)
{
#if defined(__PSP__) && defined(TILEFINCH_PSP_VALIDATION_LOG)
    memset(&webgl_native_metrics, 0, sizeof(webgl_native_metrics));
    memset(webgl_native_total_samples, 0,
           sizeof(webgl_native_total_samples));
    webgl_native_total_sample_count = 0;
#endif
}

void js_webgl_realm_epoch_release(DomBridge *bridge)
{
    if (bridge == NULL) return;
#if defined(__PSP__)
    webgl_ge_geometry_cache_clear(bridge);
    if (webgl_ge_surface_owner.valid
        && webgl_ge_surface_owner.realm_epoch_high
               == bridge->webgl_realm_epoch_high
        && webgl_ge_surface_owner.realm_epoch_low
               == bridge->webgl_realm_epoch_low) {
        memset(&webgl_ge_surface_owner, 0,
               sizeof(webgl_ge_surface_owner));
    }
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

JSValue js_webgl_index_maximum(JSContext *context, JSValueConst this_value,
                               int argc, JSValueConst *argv)
{
    (void) this_value;
    if (argc < 4) return JS_NewInt32(context, -1);
    int32_t type = 0, offset = 0, count = 0;
    if (JS_ToInt32(context, &type, argv[1]) < 0
        || JS_ToInt32(context, &offset, argv[2]) < 0
        || JS_ToInt32(context, &count, argv[3]) < 0) return JS_EXCEPTION;
    if (offset < 0 || count < 0 || count > (int32_t) WEBGL_VERTEX_LIMIT
        || (type != WEBGL_UNSIGNED_BYTE && type != WEBGL_UNSIGNED_SHORT))
        return JS_NewInt32(context, -1);
    const uint8_t *bytes = NULL;
    size_t byte_count = 0;
    JSValue buffer = JS_UNDEFINED;
    WebglJsReadResult read = webgl_typed_values(
        context, argv[0], 1u, &bytes, &byte_count, &buffer);
    if (read == WEBGL_JS_READ_EXCEPTION) return JS_EXCEPTION;
    if (read != WEBGL_JS_READ_OK) return JS_NewInt32(context, -1);
    size_t element_bytes = type == WEBGL_UNSIGNED_BYTE ? 1u : 2u;
    size_t start = (size_t) offset;
    size_t needed = (size_t) count * element_bytes;
    if (start > byte_count || needed > byte_count - start) {
        JS_FreeValue(context, buffer);
        return JS_NewInt32(context, -1);
    }
    uint32_t maximum = 0;
    const uint8_t *values = bytes + start;
    if (element_bytes == 1u) {
        for (int32_t at = 0; at < count; at++)
            if (values[at] > maximum) maximum = values[at];
    } else {
        for (int32_t at = 0; at < count; at++) {
            uint16_t value = 0;
            memcpy(&value, values + (size_t) at * 2u, sizeof(value));
            if (value > maximum) maximum = value;
        }
    }
    JS_FreeValue(context, buffer);
    return JS_NewInt32(context, (int32_t) maximum);
}

JSValue js_webgl_finite_float32(JSContext *context,
                                JSValueConst this_value,
                                int argc, JSValueConst *argv)
{
    (void) this_value;
    if (argc < 2) return JS_FALSE;
    int32_t count = 0;
    if (JS_ToInt32(context, &count, argv[1]) < 0) return JS_EXCEPTION;
    if (count < 1 || count > 16) return JS_FALSE;
    const uint8_t *bytes = NULL;
    size_t value_count = 0;
    JSValue buffer = JS_UNDEFINED;
    WebglJsReadResult read = webgl_typed_values(
        context, argv[0], sizeof(float), &bytes, &value_count, &buffer);
    if (read == WEBGL_JS_READ_EXCEPTION) return JS_EXCEPTION;
    if (read != WEBGL_JS_READ_OK) return JS_FALSE;
    if (value_count != (size_t) count) {
        JS_FreeValue(context, buffer);
        return JS_FALSE;
    }
    bool valid = true;
    for (int32_t at = 0; at < count; at++) {
        float value = 0.0f;
        memcpy(&value, bytes + (size_t) at * sizeof(value), sizeof(value));
        if (!isfinite(value)) { valid = false; break; }
    }
    JS_FreeValue(context, buffer);
    return JS_NewBool(context, valid);
}

static void webgl_matrix4_multiply(float output[16],
                                   const float left[16],
                                   const float right[16])
{
    for (size_t column = 0; column < 4u; column++) {
        size_t at = column * 4u;
        float r0 = right[at];
        float r1 = right[at + 1u];
        float r2 = right[at + 2u];
        float r3 = right[at + 3u];
        output[at] = left[0] * r0 + left[4] * r1
            + left[8] * r2 + left[12] * r3;
        output[at + 1u] = left[1] * r0 + left[5] * r1
            + left[9] * r2 + left[13] * r3;
        output[at + 2u] = left[2] * r0 + left[6] * r1
            + left[10] * r2 + left[14] * r3;
        output[at + 3u] = left[3] * r0 + left[7] * r1
            + left[11] * r2 + left[15] * r3;
    }
}

JSValue js_webgl_combine_matrix4(JSContext *context,
                                 JSValueConst this_value,
                                 int argc, JSValueConst *argv)
{
    (void) this_value;
    if (argc < 3) return JS_FALSE;
    int32_t count = 0;
    if (JS_ToInt32(context, &count, argv[2]) < 0) return JS_EXCEPTION;
    if (count < 0 || count > 4) return JS_FALSE;
    const uint8_t *output_bytes = NULL, *input_bytes = NULL;
    size_t output_count = 0, input_count = 0;
    JSValue output_buffer = JS_UNDEFINED, input_buffer = JS_UNDEFINED;
    WebglJsReadResult output_read = webgl_typed_values(
        context, argv[0], sizeof(float), &output_bytes, &output_count,
        &output_buffer);
    if (output_read == WEBGL_JS_READ_EXCEPTION) return JS_EXCEPTION;
    if (output_read != WEBGL_JS_READ_OK || output_count != 16u) {
        JS_FreeValue(context, output_buffer);
        return JS_FALSE;
    }
    WebglJsReadResult input_read = webgl_typed_values(
        context, argv[1], sizeof(float), &input_bytes, &input_count,
        &input_buffer);
    if (input_read == WEBGL_JS_READ_EXCEPTION) {
        JS_FreeValue(context, output_buffer);
        return JS_EXCEPTION;
    }
    if (input_read != WEBGL_JS_READ_OK
        || input_count < (size_t) count * 16u) {
        JS_FreeValue(context, input_buffer);
        JS_FreeValue(context, output_buffer);
        return JS_FALSE;
    }
    float current[16] = {0};
    float alternate[16];
    current[0] = current[5] = current[10] = current[15] = 1.0f;
    bool valid = true;
    for (int32_t matrix = 0; matrix < count && valid; matrix++) {
        float right[16];
        memcpy(right, input_bytes + (size_t) matrix * sizeof(right),
               sizeof(right));
        for (size_t component = 0; component < 16u; component++) {
            if (!isfinite(right[component])) { valid = false; break; }
        }
        if (!valid) break;
        webgl_matrix4_multiply(alternate, current, right);
        for (size_t component = 0; component < 16u; component++) {
            if (!isfinite(alternate[component])) { valid = false; break; }
        }
        if (valid) memcpy(current, alternate, sizeof(current));
    }
    if (valid) memcpy((uint8_t *) (uintptr_t) output_bytes,
                      current, sizeof(current));
    JS_FreeValue(context, input_buffer);
    JS_FreeValue(context, output_buffer);
    return JS_NewBool(context, valid);
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
    bool antialias = argc >= 7 && JS_ToBool(context, argv[6]) > 0;
    bool preserve_drawing_buffer = argc >= 8
        && JS_ToBool(context, argv[7]) > 0;
#if !defined(__PSP__)
    (void) preserve_drawing_buffer;
#endif
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
        bool native_authoritative = false;
        rendered = webgl_render_ge(
            bridge, handle, surface, depth, width, height, command_values,
            command_count, sources, source_count,
            textures, texture_count, antialias, preserve_drawing_buffer,
            &native_authoritative);
        if (rendered && native_authoritative) {
            void *edram = sceGeEdramGetAddr();
            rendered = edram != NULL && images_set_canvas_native_surface(
                bridge->images, node,
                (const uint8_t *) edram + WEBGL_GE_COLOR_OFFSET,
                PSP_DISPLAY_STRIDE * 4u,
                psp_display_edram_content_epoch());
        }
#else
        rendered = webgl_render_software(
            bridge->budget, surface, depth, width, height, command_values,
            command_count, sources, source_count,
            textures, texture_count, antialias);
#endif
    }
    JS_FreeValue(context, texture_buffer);
    webgl_sources_release(context, sources, source_count);
    JS_FreeValue(context, command_buffer);
    if (!rendered) return JS_NewInt32(context, 0);
    (void) images_set_canvas_opaque(bridge->images, node, true);
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
#if defined(__PSP__)
    if (node != NULL) (void) images_materialize_canvas_native_surface(
        bridge->images, node);
#endif
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
#if defined(__PSP__)
    if (node != NULL) (void) images_materialize_canvas_native_surface(
        bridge->images, node);
#endif
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
