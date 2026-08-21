#include "tilefinch/resources.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "data_url.h"
#include "tilefinch/content_blocker.h"
#include "tilefinch/fetch.h"
#include "tilefinch/integer_math.h"
#include "tilefinch/layout.h"
#include "tilefinch/platform.h"
#include "tilefinch/url.h"
#include "style_internal.h"

#include <lexbor/html/serialize.h>
#include <webp/decode.h>

#undef budget_malloc
#undef budget_calloc
#undef budget_realloc
#define budget_malloc(b, s) budget_malloc_category((b), BUDGET_CATEGORY_RESOURCE, (s))
#define budget_calloc(b, n, s) budget_calloc_category((b), BUDGET_CATEGORY_RESOURCE, (n), (s))
#define budget_realloc(b, p, s) budget_realloc_category((b), BUDGET_CATEGORY_RESOURCE, (p), (s))

static Budget *decode_budget;

static bool image_is_webp(const unsigned char *encoded, size_t length)
{
    return encoded != NULL && length >= 12
        && memcmp(encoded, "RIFF", 4) == 0
        && memcmp(encoded + 8, "WEBP", 4) == 0;
}

static unsigned char *image_decode_webp_scaled(
    const unsigned char *encoded, size_t encoded_length,
    int target_width, int target_height, int *source_width,
    int *source_height, int *components, bool *interrupted)
{
    if (interrupted != NULL) *interrupted = false;
    if (decode_budget == NULL || !image_is_webp(encoded, encoded_length)
        || target_width <= 0 || target_height <= 0
        || encoded_length > INT32_MAX) return NULL;
    WebPDecoderConfig config;
    if (!WebPInitDecoderConfig(&config)
        || WebPGetFeatures(encoded, encoded_length, &config.input)
               != VP8_STATUS_OK
        || config.input.width <= 0 || config.input.height <= 0
        || config.input.has_animation
        || target_width > config.input.width
        || target_height > config.input.height) return NULL;
    const int decoded_source_width = config.input.width;
    const int decoded_source_height = config.input.height;
    if ((size_t) target_width > SIZE_MAX / (size_t) target_height
        || (size_t) target_width * (size_t) target_height
               > SIZE_MAX / 4u) return NULL;
    size_t target_pixels = (size_t) target_width * (size_t) target_height;

    /* libwebp owns transient coefficient/YUV scratch even when its RGBA
       destination is caller-owned. Lossless streams alone retain a four-byte
       source-sized transform plane plus row caches and Huffman metadata, so
       pre-admit a conservative eight source bytes per pixel plus the encoded
       stream before entering the upstream allocator. The ordinary shared
       budget can refuse a very large image without exposing raw allocations
       outside Tilefinch's memory discipline. */
    if ((size_t) decoded_source_width
            > SIZE_MAX / (size_t) decoded_source_height) return NULL;
    size_t source_pixels = (size_t) decoded_source_width
                           * (size_t) decoded_source_height;
    if (source_pixels > (SIZE_MAX - encoded_length) / 8u) return NULL;
    size_t scratch_bytes = source_pixels * 8u + encoded_length;
    BudgetReservation scratch = {0};
    if (!budget_reservation_acquire(
            &scratch, decode_budget, BUDGET_CATEGORY_RESOURCE,
            scratch_bytes)) return NULL;
    size_t target_bytes = target_pixels * 4u;
    unsigned char *output = budget_malloc(decode_budget, target_bytes);
    if (output == NULL) {
        budget_reservation_release(&scratch);
        return NULL;
    }
    config.options.use_scaling = target_width != config.input.width
                                 || target_height != config.input.height;
    config.options.scaled_width = target_width;
    config.options.scaled_height = target_height;
    config.options.use_threads = 0;
    config.output.colorspace = MODE_RGBA;
    config.output.is_external_memory = 1;
    config.output.u.RGBA.rgba = output;
    config.output.u.RGBA.stride = target_width * 4;
    config.output.u.RGBA.size = target_bytes;
    /* The one-shot decoder can spend hundreds of milliseconds inside one
       upstream call on Allegrex, including when invoked by a raster cache
       miss. Feed the same decoder incrementally so input/cancel/watchdog
       service gets a checkpoint between small compressed-data windows. The
       external destination and source-pixel reservation keep ownership and
       memory accounting identical to the one-shot path. */
    WebPIDecoder *decoder = WebPIDecode(NULL, 0, &config);
    VP8StatusCode status = VP8_STATUS_SUSPENDED;
    size_t offset = 0;
    size_t work_units = 0;
    while (decoder != NULL && offset < encoded_length
           && status == VP8_STATUS_SUSPENDED) {
        size_t chunk = encoded_length - offset;
        if (chunk > 2048u) chunk = 2048u;
        status = WebPIAppend(decoder, encoded + offset, chunk);
        offset += chunk;
        work_units++;
        if (status == VP8_STATUS_SUSPENDED && offset < encoded_length
            && !tilefinch_platform_cooperate(
                   "image-webp-decode", work_units)) {
            if (interrupted != NULL) *interrupted = true;
            break;
        }
    }
    if (decoder != NULL) WebPIDelete(decoder);
    WebPFreeDecBuffer(&config.output);
    budget_reservation_release(&scratch);
    if (status != VP8_STATUS_OK) {
        budget_free(decode_budget, output);
        return NULL;
    }
    if (source_width != NULL) *source_width = decoded_source_width;
    if (source_height != NULL) *source_height = decoded_source_height;
    if (components != NULL) *components = config.input.has_alpha ? 4 : 3;
    return output;
}

static void *image_malloc(size_t size)
{
    return budget_malloc(decode_budget, size);
}

static void *image_realloc(void *pointer, size_t size)
{
    return budget_realloc(decode_budget, pointer, size);
}

static void image_free(void *pointer)
{
    budget_free(decode_budget, pointer);
}

#define STBI_MALLOC(size) image_malloc(size)
#define STBI_REALLOC(pointer, size) image_realloc((pointer), (size))
#define STBI_FREE(pointer) image_free(pointer)
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#if defined(TILEFINCH_DISABLE_GIF)
#define STBI_NO_GIF
#endif
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

/* stb_image's ordinary JPEG path materializes a full RGBA source after its
   component planes.  That transient peak is unnecessary when the retained
   resource is already bounded to the physical viewport.  Reuse stb's JPEG
   decoder and resamplers, but emit only the requested rows and columns into
   the final target.  The component decoder remains upstream and unchanged;
   this function only replaces load_jpeg_image's full-size color-conversion
   surface. */
static unsigned char *image_decode_jpeg_scaled(
    const unsigned char *encoded, int encoded_length,
    int target_width, int target_height, int *source_width,
    int *source_height, int *components)
{
    if (encoded == NULL || encoded_length <= 0
        || target_width <= 0 || target_height <= 0) return NULL;
    stbi__context stream;
    stbi__start_mem(&stream, encoded, encoded_length);
    if (!stbi__jpeg_test(&stream)) return NULL;
    stbi__rewind(&stream);
    stbi__jpeg *jpeg = (stbi__jpeg *) stbi__malloc(sizeof(*jpeg));
    if (jpeg == NULL) return NULL;
    memset(jpeg, 0, sizeof(*jpeg));
    jpeg->s = &stream;
    stbi__setup_jpeg(jpeg);
    if (!stbi__decode_jpeg_image(jpeg)) {
        stbi__cleanup_jpeg(jpeg);
        STBI_FREE(jpeg);
        return NULL;
    }
    int width = stream.img_x;
    int height = stream.img_y;
    int decode_components = stream.img_n;
    if (width <= 0 || height <= 0 || decode_components <= 0
        || target_width > width || target_height > height
        || (size_t) target_width > SIZE_MAX / (size_t) target_height
        || (size_t) target_width * (size_t) target_height > SIZE_MAX / 4u) {
        stbi__cleanup_jpeg(jpeg);
        STBI_FREE(jpeg);
        return NULL;
    }

    stbi__resample resample[4];
    stbi_uc *component_rows[4] = {NULL, NULL, NULL, NULL};
    bool setup_ok = decode_components <= 4;
    for (int component = 0; setup_ok && component < decode_components;
         component++) {
        stbi__resample *row = &resample[component];
        jpeg->img_comp[component].linebuf =
            (stbi_uc *) stbi__malloc((size_t) width + 3u);
        if (jpeg->img_comp[component].linebuf == NULL) {
            setup_ok = false;
            break;
        }
        row->hs = jpeg->img_h_max / jpeg->img_comp[component].h;
        row->vs = jpeg->img_v_max / jpeg->img_comp[component].v;
        row->ystep = row->vs >> 1;
        row->w_lores = (width + row->hs - 1) / row->hs;
        row->ypos = 0;
        row->line0 = row->line1 = jpeg->img_comp[component].data;
        if (row->hs == 1 && row->vs == 1) {
            row->resample = resample_row_1;
        } else if (row->hs == 1 && row->vs == 2) {
            row->resample = stbi__resample_row_v_2;
        } else if (row->hs == 2 && row->vs == 1) {
            row->resample = stbi__resample_row_h_2;
        } else if (row->hs == 2 && row->vs == 2) {
            row->resample = jpeg->resample_row_hv_2_kernel;
        } else {
            row->resample = stbi__resample_row_generic;
        }
    }
    size_t target_bytes = (size_t) target_width * (size_t) target_height * 4u;
    stbi_uc *output = setup_ok ? (stbi_uc *) stbi__malloc(target_bytes) : NULL;
    stbi_uc *rgba_row = output == NULL
        ? NULL : (stbi_uc *) stbi__malloc((size_t) width * 4u);
    if (output == NULL || rgba_row == NULL) setup_ok = false;

    int next_target_y = 0;
    bool source_is_rgb = stream.img_n == 3
        && (jpeg->rgb == 3
            || (jpeg->app14_color_transform == 0 && !jpeg->jfif));
    for (int source_y = 0; setup_ok && source_y < height; source_y++) {
        for (int component = 0; component < decode_components; component++) {
            stbi__resample *row = &resample[component];
            int lower = row->ystep >= (row->vs >> 1);
            component_rows[component] = row->resample(
                jpeg->img_comp[component].linebuf,
                lower ? row->line1 : row->line0,
                lower ? row->line0 : row->line1,
                row->w_lores, row->hs);
            if (++row->ystep >= row->vs) {
                row->ystep = 0;
                row->line0 = row->line1;
                if (++row->ypos < jpeg->img_comp[component].y) {
                    row->line1 += jpeg->img_comp[component].w2;
                }
            }
        }
        int wanted_source_y = next_target_y < target_height
            ? tilefinch_mul_div_int(next_target_y, height, target_height)
            : height;
        if (wanted_source_y != source_y) continue;

        if (stream.img_n == 3 && source_is_rgb) {
            for (int x = 0; x < width; x++) {
                rgba_row[(size_t) x * 4u] = component_rows[0][x];
                rgba_row[(size_t) x * 4u + 1u] = component_rows[1][x];
                rgba_row[(size_t) x * 4u + 2u] = component_rows[2][x];
                rgba_row[(size_t) x * 4u + 3u] = 255;
            }
        } else if (stream.img_n == 3) {
            jpeg->YCbCr_to_RGB_kernel(
                rgba_row, component_rows[0], component_rows[1],
                component_rows[2], width, 4);
        } else if (stream.img_n == 4
                   && jpeg->app14_color_transform == 0) {
            for (int x = 0; x < width; x++) {
                stbi_uc multiplier = component_rows[3][x];
                rgba_row[(size_t) x * 4u] = stbi__blinn_8x8(
                    component_rows[0][x], multiplier);
                rgba_row[(size_t) x * 4u + 1u] = stbi__blinn_8x8(
                    component_rows[1][x], multiplier);
                rgba_row[(size_t) x * 4u + 2u] = stbi__blinn_8x8(
                    component_rows[2][x], multiplier);
                rgba_row[(size_t) x * 4u + 3u] = 255;
            }
        } else if (stream.img_n == 4
                   && jpeg->app14_color_transform == 2) {
            jpeg->YCbCr_to_RGB_kernel(
                rgba_row, component_rows[0], component_rows[1],
                component_rows[2], width, 4);
            for (int x = 0; x < width; x++) {
                stbi_uc multiplier = component_rows[3][x];
                rgba_row[(size_t) x * 4u] = stbi__blinn_8x8(
                    255 - rgba_row[(size_t) x * 4u], multiplier);
                rgba_row[(size_t) x * 4u + 1u] = stbi__blinn_8x8(
                    255 - rgba_row[(size_t) x * 4u + 1u], multiplier);
                rgba_row[(size_t) x * 4u + 2u] = stbi__blinn_8x8(
                    255 - rgba_row[(size_t) x * 4u + 2u], multiplier);
            }
        } else if (stream.img_n >= 3) {
            jpeg->YCbCr_to_RGB_kernel(
                rgba_row, component_rows[0], component_rows[1],
                component_rows[2], width, 4);
        } else {
            for (int x = 0; x < width; x++) {
                stbi_uc value = component_rows[0][x];
                rgba_row[(size_t) x * 4u] = value;
                rgba_row[(size_t) x * 4u + 1u] = value;
                rgba_row[(size_t) x * 4u + 2u] = value;
                rgba_row[(size_t) x * 4u + 3u] = 255;
            }
        }
        while (next_target_y < target_height
               && tilefinch_mul_div_int(
                      next_target_y, height, target_height) == source_y) {
            stbi_uc *target_row = output
                + (size_t) next_target_y * (size_t) target_width * 4u;
            int source_x = 0;
            int remainder = 0;
            for (int target_x = 0; target_x < target_width; target_x++) {
                memcpy(target_row + (size_t) target_x * 4u,
                       rgba_row + (size_t) source_x * 4u, 4u);
                remainder += width;
                while (remainder >= target_width) {
                    source_x++;
                    remainder -= target_width;
                }
                if (source_x >= width) source_x = width - 1;
            }
            next_target_y++;
        }
    }
    STBI_FREE(rgba_row);
    stbi__cleanup_jpeg(jpeg);
    STBI_FREE(jpeg);
    if (!setup_ok || next_target_y != target_height) {
        STBI_FREE(output);
        return NULL;
    }
    if (source_width != NULL) *source_width = width;
    if (source_height != NULL) *source_height = height;
    if (components != NULL) *components = stream.img_n >= 3 ? 3 : 1;
    return output;
}

static Budget *svg_budget;

static void *svg_malloc(size_t size)
{
    return budget_malloc(svg_budget, size);
}

static void *svg_realloc(void *pointer, size_t size)
{
    return budget_realloc(svg_budget, pointer, size);
}

static void svg_free(void *pointer)
{
    budget_free(svg_budget, pointer);
}

#define malloc(size) svg_malloc(size)
#define realloc(pointer, size) svg_realloc((pointer), (size))
#define free(pointer) svg_free(pointer)
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-compare"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#define NANOSVG_IMPLEMENTATION
#include <nanosvg.h>
#define NANOSVGRAST_IMPLEMENTATION
#include <nanosvgrast.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#undef malloc
#undef realloc
#undef free

#define MAX_TRACKED_IMAGE_NODES 128
#define MAX_RASTER_DECODE_WORKING_BYTES (8u * 1024u * 1024u)
#define MAX_PRIORITY_RASTER_DECODE_WORKING_BYTES (10u * 1024u * 1024u)
#define IMAGE_FETCH_CONCURRENCY 4
#define IMAGE_FETCH_PUMP_COMPLETIONS 1u
#define IMAGE_FETCH_POLL_WAIT_MS 1u
#define IMAGE_FETCH_PUMP_CALLBACKS 1u
#define IMAGE_FETCH_PUMP_BYTES (16u * 1024u)
#define IMAGE_FETCH_PUMP_TIME_US UINT64_C(2000)
#define IMAGE_FETCH_MAX_REPLAY_IDLE_POLLS 256u
#define IMAGE_FETCH_NO_PROGRESS_FLOOR_MS 1000.0
#define IMAGE_FETCH_NO_PROGRESS_CEILING_MS 3000.0
#define IMAGE_FETCH_UNHEALTHY_ORIGIN_LIMIT 4u
#define IMAGE_FETCH_ORIGIN_COOLDOWN_STRIKES 2u
static void image_trace(const char *reason, const char *source,
                        size_t length)
{
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("TILEFINCH_TRACE_IMAGES") != NULL;
    if (!enabled) return;
    if (source == NULL) source = "";
    if (length > 200) length = 200;
    fprintf(stderr, "tilefinch: image %s %.*s\n", reason, (int) length, source);
}

#define INLINE_SVG_SOURCE_LIMIT (64u * 1024u)
#define INLINE_SVG_USE_LIMIT 32u
#define INLINE_SVG_USE_DEPTH_LIMIT 8u
#define INLINE_SVG_EXPANSION_NODE_LIMIT 512u
#define INLINE_SVG_STYLE_DEPTH_LIMIT 48u
#define INLINE_SVG_STYLE_NODE_LIMIT 2048u
#define EXTERNAL_SVG_SYMBOL_LIMIT (64u * 1024u)
#define EXTERNAL_SVG_SYMBOL_ID_LIMIT 96u
#define EXTERNAL_SVG_SYMBOL_TAG_LIMIT 4096u
#define EXTERNAL_SVG_SPRITE_FETCH_LIMIT (768u * 1024u)
#define IMAGE_DATA_URL_SOURCE_LIMIT (256u * 1024u)
#define IMAGE_TRAVERSAL_PENDING_LIMIT 2048u
#define IMAGE_TRAVERSAL_NODE_LIMIT 65536u

typedef struct {
    lxb_dom_node_t *node;
    uint64_t source_hash;
    uint16_t display_width;
    uint16_t display_height;
    bool is_mask;
    bool is_background;
    PseudoElement pseudo;
} PendingImageTarget;

typedef struct {
    uint64_t request_id;
    uint64_t url_hash;
    char *url;
    size_t reserved_bytes;
    size_t last_received_bytes;
    long last_status_code;
    double started_ms;
    double last_progress_ms;
    bool no_progress_cancelled;
    bool from_resource_cache;
    bool resource_grant_valid;
    TilefinchResourceGrant resource_grant;
    PendingImageTarget *targets;
    size_t target_count;
    size_t target_capacity;
} PendingImageFetch;

typedef struct {
    char origin[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
    size_t strikes;
    bool cooled;
} ImageOriginHealth;

typedef struct {
    char reference[2048];
    char resolved[4096];
    FetchPreparedPageRequest prepared;
} ImageRequestScratch;

typedef struct {
    const PocDocument *document;
    ImageResources *images;
    Stylesheet *stylesheet;
    Budget *budget;
    const char *base_url;
    const char *document_url;
    const char *referrer_policy;
    size_t maximum_count;
    size_t maximum_total_encoded_bytes;
    size_t maximum_single_encoded_bytes;
    size_t maximum_decoded_bytes;
    long timeout_ms;
    FetchScheduler *scheduler;
    BrowserSession *session;
    LayoutReuseCache *style_cache;
    const FontSet *fonts;
    int viewport_width;
    const ImagePriorityTarget *priority_targets;
    size_t priority_target_count;
    uint16_t current_display_width;
    uint16_t current_display_height;
    bool preserve_staged_document_images;
    PendingImageFetch pending[IMAGE_FETCH_CONCURRENCY];
    ImageOriginHealth unhealthy_origins[
        IMAGE_FETCH_UNHEALTHY_ORIGIN_LIMIT];
    size_t unhealthy_origin_count;
    size_t pending_count;
    size_t pending_reserved_bytes;
    size_t external_svg_sprite_attempts;
    double deadline_ms;
    ImageRequestScratch *request_scratch;
    uint64_t slice_started_us;
    size_t slice_work_units;
    bool eager_decode_rasters;
    bool deadline_cancelled;
    /* The owner supplies the scheduling boundary. A decoded response is one
       complete idle unit, so do not call the global cooperation hook from
       inside that unit and accidentally turn benign input into image failure. */
    bool externally_pumped;
} ImageLoadContext;

struct ImagePriorityLoadJob {
    ImageLoadContext context;
    ImagePriorityTarget target;
    ExternalImageStats retained_stats;
    size_t retained_count;
    double started_ms;
    bool retained_priority_staged;
    bool admitted;
    bool terminal;
    bool failed;
    bool rolled_back;
};

static unsigned char *decode_svg(const void *data, size_t length,
                                 Budget *budget, size_t maximum_decoded_bytes,
                                 int *width, int *height);

static TilefinchRequestContext image_request_context(
    const ImageLoadContext *context, const char *url)
{
    return (TilefinchRequestContext) {
        .target_url = url,
        .initiator_url = context == NULL ? NULL : context->document_url,
        .top_level_url = context == NULL ? NULL : context->document_url,
        .method = "GET",
        .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_IMAGE
    };
}

static void image_accept_response_cookies(
    ImageLoadContext *context, const char *fallback_url,
    const FetchResult *fetch)
{
    if (context == NULL || context->session == NULL || fetch == NULL) return;
    TilefinchRequestContext response = image_request_context(
        context, fallback_url);
    for (size_t i = 0; i < fetch->set_cookie_count; i++) {
        response.target_url = fetch_set_cookie_url(fetch, i, fallback_url);
        (void) browser_session_cookie_set_http_context(
            context->session, &response, fetch->set_cookies[i]);
    }
}

static double image_now_ms(void)
{
    return (double) tilefinch_platform_monotonic_time_us() * 0.001;
}

static double image_no_progress_limit_ms(const ImageLoadContext *context)
{
    double timeout = context == NULL ? 0.0 : (double) context->timeout_ms;
    if (timeout <= 0.0) return 0.0;
    double limit = timeout / 2.0;
    if (limit < IMAGE_FETCH_NO_PROGRESS_FLOOR_MS) {
        limit = IMAGE_FETCH_NO_PROGRESS_FLOOR_MS;
    }
    if (limit > IMAGE_FETCH_NO_PROGRESS_CEILING_MS) {
        limit = IMAGE_FETCH_NO_PROGRESS_CEILING_MS;
    }
    return limit < timeout ? limit : timeout;
}

static ImageOriginHealth *image_origin_health(ImageLoadContext *context,
                                              const char *url,
                                              bool create)
{
    if (context == NULL || url == NULL) return NULL;
    char origin[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
    if (!tilefinch_url_origin(url, origin, sizeof(origin))) return NULL;
    for (size_t i = 0; i < context->unhealthy_origin_count; i++) {
        if (strcmp(context->unhealthy_origins[i].origin, origin) == 0) {
            return &context->unhealthy_origins[i];
        }
    }
    if (!create
        || context->unhealthy_origin_count
             == IMAGE_FETCH_UNHEALTHY_ORIGIN_LIMIT) return NULL;
    ImageOriginHealth *health =
        &context->unhealthy_origins[context->unhealthy_origin_count++];
    snprintf(health->origin, sizeof(health->origin), "%s", origin);
    return health;
}

static bool image_origin_cooled(ImageLoadContext *context, const char *url)
{
    ImageOriginHealth *health = image_origin_health(context, url, false);
    return health != NULL && health->cooled;
}

static void image_note_origin_no_progress(ImageLoadContext *context,
                                          const char *url)
{
    ImageOriginHealth *health = image_origin_health(context, url, true);
    if (health == NULL || health->cooled) return;
    if (health->strikes != SIZE_MAX) health->strikes++;
    if (health->strikes < IMAGE_FETCH_ORIGIN_COOLDOWN_STRIKES) return;
    health->cooled = true;
    context->images->stats.no_progress_origin_cooldowns++;
    image_trace("origin-cooldown", health->origin, strlen(health->origin));
}

static void image_note_pending_progress(ImageLoadContext *context)
{
    double now = image_now_ms();
    for (size_t i = 0; i < IMAGE_FETCH_CONCURRENCY; i++) {
        PendingImageFetch *pending = &context->pending[i];
        if (pending->request_id == 0) continue;
        FetchRequestProgress progress;
        if (!fetch_scheduler_request_progress(
                context->scheduler, pending->request_id, &progress)) {
            continue;
        }
        ExternalImageStats *stats = &context->images->stats;
        stats->progress_samples++;
        bool headers_progressed = progress.status_code != 0
            && progress.status_code != pending->last_status_code;
        if (headers_progressed) {
            pending->last_status_code = progress.status_code;
            pending->last_progress_ms = now;
            stats->progress_events++;
        }
        if (progress.received_body_bytes > pending->last_received_bytes) {
            size_t added =
                progress.received_body_bytes - pending->last_received_bytes;
            if (added <= SIZE_MAX - stats->progress_bytes) {
                stats->progress_bytes += added;
            } else {
                stats->progress_bytes = SIZE_MAX;
            }
            stats->progress_events++;
            pending->last_received_bytes = progress.received_body_bytes;
            pending->last_progress_ms = now;
        } else if (!headers_progressed && !progress.complete) {
            stats->stalled_polls++;
            double stalled = now - pending->last_progress_ms;
            if (stalled > 0.0
                && (uint64_t) stalled > stats->maximum_no_progress_ms) {
                stats->maximum_no_progress_ms = (uint64_t) stalled;
            }
        }
    }
}

static void image_cancel_no_progress_pending(ImageLoadContext *context)
{
    double limit = image_no_progress_limit_ms(context);
    if (limit <= 0.0) return;
    double now = image_now_ms();
    for (size_t i = 0; i < IMAGE_FETCH_CONCURRENCY; i++) {
        PendingImageFetch *pending = &context->pending[i];
        if (pending->request_id == 0 || pending->no_progress_cancelled
            || now - pending->last_progress_ms < limit) continue;
        FetchRequestProgress progress;
        if (!fetch_scheduler_request_progress(
                context->scheduler, pending->request_id, &progress)
            || progress.complete) continue;
        pending->no_progress_cancelled = true;
        if (!fetch_scheduler_cancel(
                context->scheduler, pending->request_id,
                "image request made no progress")) continue;
        context->images->stats.no_progress_cancelled++;
        image_note_origin_no_progress(context, pending->url);
        double stalled = now - pending->last_progress_ms;
        if (stalled > 0.0
            && (uint64_t) stalled
                 > context->images->stats.maximum_no_progress_ms) {
            context->images->stats.maximum_no_progress_ms =
                (uint64_t) stalled;
        }
        image_trace("no-progress-cancel", pending->url,
                    pending->url == NULL ? 0 : strlen(pending->url));
    }
}

static void image_note_request_finished(ImageLoadContext *context,
                                        PendingImageFetch *pending,
                                        const FetchResult *fetched,
                                        bool success)
{
    ExternalImageStats *stats = &context->images->stats;
    size_t received = fetched->received_body_bytes;
    if (received < fetched->length) received = fetched->length;
    if (received > pending->last_received_bytes) {
        size_t added = received - pending->last_received_bytes;
        stats->progress_bytes = added <= SIZE_MAX - stats->progress_bytes
            ? stats->progress_bytes + added : SIZE_MAX;
        stats->progress_events++;
        pending->last_received_bytes = received;
    }
    double elapsed = image_now_ms() - pending->started_ms;
    if (elapsed > 0.0 && (uint64_t) elapsed > stats->maximum_request_ms) {
        stats->maximum_request_ms = (uint64_t) elapsed;
    }
    if (success) return;
    if (fetched->timed_out) {
        stats->fetch_failures_timeout++;
    } else if (fetched->status_code >= 400
               && fetched->status_code < 500) {
        stats->fetch_failures_http_4xx++;
    } else if (fetched->status_code >= 500
               && fetched->status_code < 600) {
        stats->fetch_failures_http_5xx++;
    } else if (fetched->trace_external_cancel
               || strstr(fetched->error, "cancel") != NULL
               || strstr(fetched->error, "Cancel") != NULL
               || strstr(fetched->error, "deadline") != NULL) {
        stats->fetch_failures_cancelled++;
    } else if (strstr(fetched->error, "quota exceeded") != NULL) {
        stats->fetch_failures_quota++;
    } else {
        stats->fetch_failures_transport++;
    }
    if (getenv("TILEFINCH_TRACE_IMAGES") != NULL) {
        fprintf(stderr,
                "tilefinch: image fetch-result status=%ld timed-out=%s "
                "bytes=%zu elapsed-ms=%llu no-progress-ms=%llu "
                "error=\"%.120s\" url=%.200s\n",
                fetched->status_code, fetched->timed_out ? "yes" : "no",
                received, (unsigned long long) (elapsed > 0.0
                    ? (uint64_t) elapsed : 0),
                (unsigned long long)
                    (image_now_ms() > pending->last_progress_ms
                       ? (uint64_t)
                           (image_now_ms() - pending->last_progress_ms) : 0),
                fetched->error,
                pending->url == NULL ? "" : pending->url);
    }
}

static bool image_profile_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        enabled = getenv("TILEFINCH_TRACE_IMAGE_PROFILE") != NULL;
    }
    return enabled != 0;
}

static uint64_t image_profile_now_us(void)
{
    return tilefinch_platform_monotonic_time_us();
}

static void image_finish_slice(ImageLoadContext *context)
{
    uint64_t finished = tilefinch_platform_monotonic_time_us();
    uint64_t elapsed_us = finished >= context->slice_started_us
        ? finished - context->slice_started_us : 0;
    if (elapsed_us > context->images->stats.max_slice_us) {
        context->images->stats.max_slice_us = elapsed_us;
        context->images->stats.max_slice_work_units =
            context->slice_work_units;
    }
    context->slice_started_us = finished;
    context->slice_work_units = 0;
}

/*
 * The three things this stage does between checkpoints have very different
 * costs, and the supervisor's max-gap line names the phase that ended the
 * widest one. Reporting all three as "resource" meant a wide gap said only
 * that it happened somewhere in image loading; these names say whether it
 * was a transport pump, one response's decode, or the style walk.
 */
static bool image_cooperate(ImageLoadContext *context, const char *phase)
{
    image_finish_slice(context);
    context->images->stats.cooperative_yields++;
    if (context->externally_pumped) return true;
    return tilefinch_platform_cooperate(
        phase, context->images->stats.work_units);
}

static bool image_work(ImageLoadContext *context, size_t units,
                       bool force_yield, const char *phase)
{
    ExternalImageStats *stats = &context->images->stats;
    if (units > SIZE_MAX - stats->work_units) stats->work_units = SIZE_MAX;
    else stats->work_units += units;
    if (units > SIZE_MAX - context->slice_work_units) {
        context->slice_work_units = SIZE_MAX;
    } else {
        context->slice_work_units += units;
    }
    return (!force_yield && context->slice_work_units < 32)
           || image_cooperate(context, phase);
}

static bool image_stage_expired(const ImageLoadContext *context)
{
    return image_now_ms() >= context->deadline_ms;
}

static void cancel_expired_pending(ImageLoadContext *context)
{
    if (context->deadline_cancelled || !image_stage_expired(context)) return;
    context->deadline_cancelled = true;
    for (size_t i = 0; i < IMAGE_FETCH_CONCURRENCY; i++) {
        if (context->pending[i].request_id != 0) {
            (void) fetch_scheduler_cancel(context->scheduler,
                                          context->pending[i].request_id,
                                          "image stage deadline exceeded");
        }
    }
}

static bool image_name_is(lxb_dom_node_t *node, const char *wanted)
{
    size_t length = 0;
    const char *name = document_element_name(node, &length);
    return name != NULL && strlen(wanted) == length
           && memcmp(name, wanted, length) == 0;
}

static uint64_t image_hash_url(const char *url)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; url[i] != '\0'; i++) {
        hash = (hash ^ (unsigned char) url[i]) * UINT64_C(1099511628211);
    }
    return hash;
}

static uint64_t image_hash_request(const char *url,
                                   const char *referrer_source,
                                   const char *referrer_policy)
{
    uint64_t hash = image_hash_url(url);
    const char *parts[2] = {
        referrer_source == NULL ? "" : referrer_source,
        referrer_policy == NULL ? "" : referrer_policy
    };
    for (size_t part = 0; part < 2; part++) {
        hash = (hash ^ UINT8_C(0xff)) * UINT64_C(1099511628211);
        for (size_t i = 0; parts[part][i] != '\0'; i++) {
            hash = (hash ^ (unsigned char) parts[part][i])
                   * UINT64_C(1099511628211);
        }
    }
    return hash;
}

static uint64_t image_hash_bytes(const char *data, size_t length)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < length; i++) {
        hash = (hash ^ (unsigned char) data[i]) * UINT64_C(1099511628211);
    }
    return hash;
}

static bool image_add(ImageResources *images, ImageResource resource)
{
    if (images->count >= MAX_TRACKED_IMAGE_NODES) {
        images->stats.skipped_limit++;
        return true;
    }
    if (images->count == images->capacity) {
        size_t capacity = images->capacity == 0 ? 16 : images->capacity * 2;
        if (capacity > MAX_TRACKED_IMAGE_NODES) capacity = MAX_TRACKED_IMAGE_NODES;
        ImageResource *items = budget_realloc(images->budget, images->items,
                                              capacity * sizeof(*items));
        if (items == NULL) return false;
        images->items = items;
        images->capacity = capacity;
    }
    images->items[images->count++] = resource;
    return true;
}

static void image_resource_release_owned_pixels(
    Budget *budget, ImageResource *resource)
{
    if (budget == NULL || resource == NULL || !resource->owns_pixels) return;
    if (resource->pixel_body != NULL) {
        browser_shared_body_release(resource->pixel_body);
    } else {
        budget_free(budget, resource->pixels);
    }
    resource->pixels = NULL;
    resource->pixel_body = NULL;
    resource->owns_pixels = false;
}

static bool image_node_already_tracked(const ImageResources *images,
                                       const lxb_dom_node_t *node,
                                       uint64_t source_hash,
                                       bool is_mask, bool is_background,
                                       PseudoElement pseudo);

bool images_adopt_decoded_surface(ImageResources *images, Budget *budget,
                                  lxb_dom_node_t *node,
                                  unsigned char *rgba_pixels,
                                  int width, int height)
{
    if (images == NULL || budget == NULL || node == NULL
        || rgba_pixels == NULL || width <= 0 || height <= 0
        || (size_t) width > SIZE_MAX / (size_t) height
        || (size_t) width * (size_t) height > SIZE_MAX / 4u) {
        return false;
    }
    size_t surface_bytes = (size_t) width * (size_t) height * 4u;
    if (budget_usable_size(rgba_pixels) < surface_bytes
        || images->stats.decoded_bytes > SIZE_MAX - surface_bytes) {
        return false;
    }
    if (images->budget != NULL && images->budget != budget) return false;
    if (image_node_already_tracked(
            images, node, 0, false, false, PSEUDO_NONE)) {
        return false;
    }
    images->budget = budget;
    size_t before = images->count;
    bool added = image_add(images, (ImageResource) {
        .node = node,
        .pixels = rgba_pixels,
        .source_width = width,
        .source_height = height,
        .width = width,
        .height = height,
        .owns_pixels = true
    });
    if (!added || images->count == before) return false;
    images->stats.discovered++;
    images->stats.attempted++;
    images->stats.loaded++;
    images->stats.decoded_bytes += surface_bytes;
    return true;
}

bool images_replace_with_decoded_surface(ImageResources *images,
                                         Budget *budget,
                                         lxb_dom_node_t *node,
                                         unsigned char *rgba_pixels,
                                         int width, int height)
{
    if (images == NULL || budget == NULL || node == NULL
        || rgba_pixels == NULL || width <= 0 || height <= 0
        || (size_t) width > SIZE_MAX / (size_t) height
        || (size_t) width * (size_t) height > SIZE_MAX / 4u) {
        return false;
    }
    size_t surface_bytes = (size_t) width * (size_t) height * 4u;
    if (budget_usable_size(rgba_pixels) < surface_bytes
        || images->stats.decoded_bytes > SIZE_MAX - surface_bytes
        || (images->budget != NULL && images->budget != budget)) {
        return false;
    }
    size_t index = images->count;
    for (size_t i = 0; i < images->count; i++) {
        ImageResource *item = &images->items[i];
        if (item->node == node && !item->is_mask
            && !item->is_background && item->pseudo == PSEUDO_NONE) {
            index = i;
            break;
        }
    }
    if (index == images->count) {
        return images_adopt_decoded_surface(
            images, budget, node, rgba_pixels, width, height);
    }

    ImageResource *retired = &images->items[index];
    bool pixels_retained = false;
    bool encoded_retained = false;
    for (size_t i = 0; i < images->count; i++) {
        ImageResource *alias = &images->items[i];
        if (i == index) continue;
        if (retired->owns_pixels && alias->pixels == retired->pixels) {
            alias->pixel_body = retired->pixel_body;
            alias->owns_pixels = true;
            pixels_retained = true;
        }
        if (retired->owns_encoded
            && alias->encoded == retired->encoded
            && alias->encoded_body == retired->encoded_body) {
            alias->owns_encoded = true;
            encoded_retained = true;
        }
    }
    if (retired->owns_pixels && !pixels_retained) {
        size_t retired_bytes = 0;
        if (retired->width > 0 && retired->height > 0
            && (size_t) retired->width <= SIZE_MAX / (size_t) retired->height
            && (size_t) retired->width * (size_t) retired->height
                   <= SIZE_MAX / 4u) {
            retired_bytes =
                (size_t) retired->width * (size_t) retired->height * 4u;
        }
        image_resource_release_owned_pixels(budget, retired);
        images->stats.decoded_bytes =
            retired_bytes <= images->stats.decoded_bytes
            ? images->stats.decoded_bytes - retired_bytes : 0;
    }
    if (retired->owns_encoded && !encoded_retained) {
        if (retired->encoded_body != NULL) {
            browser_shared_body_release(retired->encoded_body);
        } else {
            budget_free(budget, retired->encoded);
        }
        images->stats.encoded_bytes =
            retired->encoded_length <= images->stats.encoded_bytes
            ? images->stats.encoded_bytes - retired->encoded_length : 0;
    }
    *retired = (ImageResource) {
        .node = node,
        .pixels = rgba_pixels,
        .source_width = width,
        .source_height = height,
        .width = width,
        .height = height,
        .owns_pixels = true
    };
    images->budget = budget;
    images->stats.decoded_bytes += surface_bytes;
    return true;
}

static const ImageResource *image_find_hash(const ImageResources *images,
                                            uint64_t hash)
{
    for (size_t i = 0; i < images->count; i++) {
        if (images->items[i].url_hash == hash
            && image_resource_available(&images->items[i])) {
            return &images->items[i];
        }
    }
    return NULL;
}

static bool image_node_already_tracked(const ImageResources *images,
                                       const lxb_dom_node_t *node,
                                       uint64_t source_hash,
                                       bool is_mask, bool is_background,
                                       PseudoElement pseudo)
{
    for (size_t i = 0; i < images->count; i++) {
        const ImageResource *item = &images->items[i];
        if (item->node == node && item->source_hash == source_hash
            && item->is_mask == is_mask
            && item->is_background == is_background
            && item->pseudo == pseudo) return true;
    }
    return false;
}

typedef struct {
    Budget *budget;
    char *data;
    size_t length;
    size_t capacity;
} InlineSvgBuffer;

typedef struct {
    const PocDocument *document;
    lxb_dom_node_t *lookup_root;
    lxb_dom_node_t *symbols[INLINE_SVG_USE_DEPTH_LIMIT];
    size_t use_count;
    size_t expanded_nodes;
} InlineSvgUseValidation;

static lxb_status_t inline_svg_receive(const lxb_char_t *data, size_t length,
                                       void *opaque)
{
    InlineSvgBuffer *buffer = opaque;
    if (buffer == NULL || data == NULL
        || length > INLINE_SVG_SOURCE_LIMIT - buffer->length) {
        return LXB_STATUS_ERROR;
    }
    size_t wanted = buffer->length + length + 1;
    if (wanted > buffer->capacity) {
        size_t capacity = buffer->capacity == 0 ? 512 : buffer->capacity;
        while (capacity < wanted && capacity < INLINE_SVG_SOURCE_LIMIT + 1u) {
            size_t grown = capacity <= (INLINE_SVG_SOURCE_LIMIT + 1u) / 2u
                           ? capacity * 2u : INLINE_SVG_SOURCE_LIMIT + 1u;
            if (grown <= capacity) break;
            capacity = grown;
        }
        if (capacity < wanted) return LXB_STATUS_ERROR;
        char *expanded = budget_realloc(buffer->budget, buffer->data,
                                        capacity);
        if (expanded == NULL) return LXB_STATUS_ERROR;
        buffer->data = expanded;
        buffer->capacity = capacity;
    }
    memcpy(buffer->data + buffer->length, data, length);
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
    return LXB_STATUS_OK;
}

static lxb_dom_node_t *inline_svg_next_node(lxb_dom_node_t *root,
                                            lxb_dom_node_t *node,
                                            bool skip_children)
{
    if (!skip_children && node->first_child != NULL) {
        return node->first_child;
    }
    while (node != root && node->next == NULL) node = node->parent;
    return node == root ? NULL : node->next;
}

static bool inline_svg_definition_container(lxb_dom_node_t *node)
{
    return image_name_is(node, "symbol") || image_name_is(node, "defs");
}

static const char *inline_svg_use_href(lxb_dom_node_t *node, size_t *length)
{
    const char *href = document_attribute(node, "href", length);
    if (href == NULL) {
        href = document_attribute(node, "xlink:href", length);
    }
    return href;
}

/* External SVG sprites are a common way for responsive navigation controls
   to share a few tiny icons. Treat only the deterministic shape as an image
   resource: one rendered <use> child with a path plus bounded fragment. */
static const char *inline_svg_external_use_href(lxb_dom_node_t *root,
                                                size_t *length)
{
    if (length != NULL) *length = 0;
    if (root == NULL || !image_name_is(root, "svg")) return NULL;
    lxb_dom_node_t *use = NULL;
    for (lxb_dom_node_t *child = root->first_child; child != NULL;
         child = child->next) {
        if (child->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        if (use != NULL || !image_name_is(child, "use")) return NULL;
        use = child;
    }
    size_t href_length = 0;
    const char *href = use == NULL ? NULL
        : inline_svg_use_href(use, &href_length);
    if (href == NULL || href_length < 3 || href_length >= 2048) return NULL;
    size_t hash = href_length;
    while (hash != 0 && href[hash - 1] != '#') hash--;
    if (hash <= 1 || hash == href_length
        || href_length - hash > EXTERNAL_SVG_SYMBOL_ID_LIMIT) return NULL;
    for (size_t i = hash; i < href_length; i++) {
        if (href[i] == '#' || isspace((unsigned char) href[i])) return NULL;
    }
    if (length != NULL) *length = href_length;
    return href;
}

static bool external_svg_identifier_matches(const char *tag, size_t length,
                                             const char *wanted,
                                             size_t wanted_length)
{
    size_t at = 0;
    while (at < length) {
        while (at < length && isspace((unsigned char) tag[at])) at++;
        size_t name_start = at;
        while (at < length
               && (isalnum((unsigned char) tag[at]) || tag[at] == '-'
                   || tag[at] == '_' || tag[at] == ':')) at++;
        size_t name_length = at - name_start;
        while (at < length && isspace((unsigned char) tag[at])) at++;
        if (at >= length || tag[at] != '=') {
            while (at < length && !isspace((unsigned char) tag[at])) at++;
            continue;
        }
        at++;
        while (at < length && isspace((unsigned char) tag[at])) at++;
        if (at >= length || (tag[at] != '\'' && tag[at] != '"')) return false;
        char quote = tag[at++];
        size_t value_start = at;
        while (at < length && tag[at] != quote) at++;
        if (at == length) return false;
        size_t value_length = at - value_start;
        if (name_length == 2
            && strncasecmp(tag + name_start, "id", 2) == 0
            && value_length == wanted_length
            && memcmp(tag + value_start, wanted, wanted_length) == 0) {
            return true;
        }
        at++;
    }
    return false;
}

static bool external_svg_symbol_bounds(const char *source, size_t length,
                                       const char *identifier,
                                       size_t identifier_length,
                                       size_t *attributes_start,
                                       size_t *open_end, size_t *close_start)
{
    static const char open_name[] = "<symbol";
    static const char close_name[] = "</symbol";
    for (size_t at = 0; at + sizeof(open_name) - 1u < length; at++) {
        if (source[at] != '<'
            || strncasecmp(source + at, open_name,
                           sizeof(open_name) - 1u) != 0) continue;
        size_t after_name = at + sizeof(open_name) - 1u;
        if (after_name >= length
            || (!isspace((unsigned char) source[after_name])
                && source[after_name] != '>')) continue;
        size_t end = after_name;
        char quote = '\0';
        while (end < length && end - at <= EXTERNAL_SVG_SYMBOL_TAG_LIMIT) {
            char c = source[end];
            if (quote != '\0') {
                if (c == quote) quote = '\0';
            } else if (c == '\'' || c == '"') {
                quote = c;
            } else if (c == '>') {
                break;
            }
            end++;
        }
        if (end == length || source[end] != '>'
            || !external_svg_identifier_matches(
                   source + after_name, end - after_name,
                   identifier, identifier_length)) continue;
        size_t close = end + 1u;
        while (close + sizeof(close_name) - 1u < length) {
            if (source[close] == '<'
                && strncasecmp(source + close, close_name,
                               sizeof(close_name) - 1u) == 0) break;
            close++;
        }
        if (close + sizeof(close_name) - 1u >= length
            || close - (end + 1u) > EXTERNAL_SVG_SYMBOL_LIMIT) return false;
        *attributes_start = after_name;
        *open_end = end;
        *close_start = close;
        return true;
    }
    return false;
}

static bool image_style_chain_for_node(const Stylesheet *sheet,
                                       lxb_dom_node_t *node,
                                       ComputedStyle *result)
{
    enum { MAXIMUM_ANCESTORS = 64 };
    lxb_dom_node_t *ancestors[MAXIMUM_ANCESTORS];
    size_t count = 0;
    for (lxb_dom_node_t *at = node; at != NULL; at = at->parent) {
        if (at->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        if (count == MAXIMUM_ANCESTORS) return false;
        ancestors[count++] = at;
    }
    ComputedStyle computed = {0};
    bool has_parent = false;
    while (count != 0) {
        computed = style_for_node(sheet, ancestors[--count],
                                  has_parent ? &computed : NULL);
        has_parent = true;
    }
    if (!has_parent) return false;
    *result = computed;
    return true;
}

static lxb_dom_node_t *inline_svg_find_id(lxb_dom_node_t *root,
                                          const char *identifier,
                                          size_t identifier_length)
{
    if (root == NULL || identifier == NULL
        || identifier_length == 0) return NULL;
    for (lxb_dom_node_t *node = root; node != NULL;
         node = inline_svg_next_node(root, node, false)) {
        size_t id_length = 0;
        const char *id = document_attribute(node, "id", &id_length);
        if (id != NULL && id_length == identifier_length
            && memcmp(id, identifier, identifier_length) == 0) {
            return node;
        }
    }
    return NULL;
}

static bool inline_svg_reference_target(lxb_dom_node_t *target)
{
    if (target == NULL || target->type != LXB_DOM_NODE_TYPE_ELEMENT
        || image_name_is(target, "use") || image_name_is(target, "defs")) {
        return false;
    }
    for (lxb_dom_node_t *at = target; at != NULL; at = at->parent) {
        if (image_name_is(at, "svg")) return true;
    }
    return false;
}

static lxb_dom_node_t *inline_svg_resolve_target(
    const InlineSvgUseValidation *validation, lxb_dom_node_t *use)
{
    size_t href_length = 0;
    const char *href = inline_svg_use_href(use, &href_length);
    if (href == NULL || href_length < 2 || href_length > 129
        || href[0] != '#') return NULL;
    for (size_t i = 1; i < href_length; i++) {
        if (href[i] == '#' || isspace((unsigned char) href[i])) return NULL;
    }
    lxb_dom_node_t *target = inline_svg_find_id(
        validation->lookup_root, href + 1, href_length - 1);
    if (target == NULL && validation->document != NULL
        && validation->document->html != NULL) {
        target = inline_svg_find_id(
            lxb_dom_interface_node(validation->document->html),
            href + 1, href_length - 1);
    }
    return inline_svg_reference_target(target) ? target : NULL;
}

static size_t inline_svg_subtree_nodes(lxb_dom_node_t *root, size_t limit)
{
    size_t count = 0;
    for (lxb_dom_node_t *node = root->first_child; node != NULL;
         node = inline_svg_next_node(root, node, false)) {
        if (++count > limit) return count;
    }
    return count;
}

static bool inline_svg_validate_tree(InlineSvgUseValidation *validation,
                                     lxb_dom_node_t *container,
                                     size_t depth);

static bool inline_svg_validate_use(InlineSvgUseValidation *validation,
                                    lxb_dom_node_t *use, size_t depth)
{
    if (validation->use_count >= INLINE_SVG_USE_LIMIT
        || depth >= INLINE_SVG_USE_DEPTH_LIMIT) return false;
    lxb_dom_node_t *symbol = inline_svg_resolve_target(validation, use);
    if (symbol == NULL) return false;
    for (size_t i = 0; i < depth; i++) {
        if (validation->symbols[i] == symbol) return false;
    }
    size_t remaining = INLINE_SVG_EXPANSION_NODE_LIMIT
                       - validation->expanded_nodes;
    size_t nodes = inline_svg_subtree_nodes(symbol, remaining);
    if (nodes > remaining) return false;
    validation->expanded_nodes += nodes;
    validation->use_count++;
    validation->symbols[depth] = symbol;
    bool valid = inline_svg_validate_tree(validation, symbol, depth + 1);
    validation->symbols[depth] = NULL;
    return valid;
}

static bool inline_svg_validate_tree(InlineSvgUseValidation *validation,
                                     lxb_dom_node_t *container,
                                     size_t depth)
{
    for (lxb_dom_node_t *node = container->first_child; node != NULL;
         node = inline_svg_next_node(
             container, node, inline_svg_definition_container(node))) {
        if (image_name_is(node, "use")
            && !inline_svg_validate_use(validation, node, depth)) {
            return false;
        }
    }
    return true;
}

static bool inline_svg_skip_instance_attribute(const char *name,
                                               size_t length)
{
    static const char *const skipped[] = {
        "href", "xlink:href", "id", "x", "y", "width", "height",
        "viewBox", "preserveAspectRatio"
    };
    for (size_t i = 0; i < sizeof(skipped) / sizeof(skipped[0]); i++) {
        size_t wanted = strlen(skipped[i]);
        if (length == wanted && strncasecmp(name, skipped[i], wanted) == 0) {
            return true;
        }
    }
    return false;
}

static bool inline_svg_copy_instance_attributes(lxb_dom_element_t *target,
                                                lxb_dom_node_t *source)
{
    if (target == NULL || source == NULL
        || source->type != LXB_DOM_NODE_TYPE_ELEMENT) return false;
    lxb_dom_element_t *element = lxb_dom_interface_element(source);
    for (lxb_dom_attr_t *attribute = element->first_attr;
         attribute != NULL; attribute = attribute->next) {
        size_t name_length = 0, value_length = 0;
        const lxb_char_t *name = lxb_dom_attr_qualified_name(
            attribute, &name_length);
        const lxb_char_t *value = lxb_dom_attr_value(
            attribute, &value_length);
        if (name == NULL || inline_svg_skip_instance_attribute(
                (const char *) name, name_length)) continue;
        if (lxb_dom_element_set_attribute(target, name, name_length,
                                          value, value_length) == NULL) {
            return false;
        }
    }
    return true;
}

static bool inline_svg_apply_use_translation(lxb_dom_element_t *group,
                                             lxb_dom_node_t *symbol,
                                             lxb_dom_node_t *use)
{
    size_t x_length = 0, y_length = 0, transform_length = 0;
    const char *x = document_attribute(use, "x", &x_length);
    const char *y = document_attribute(use, "y", &y_length);
    const char *transform = document_attribute(use, "transform",
                                                &transform_length);
    if (transform == NULL) {
        transform = document_attribute(symbol, "transform",
                                       &transform_length);
    }
    if ((x == NULL || x_length == 0) && (y == NULL || y_length == 0)) {
        return true;
    }
    if (x_length > 96 || y_length > 96 || transform_length > 256) {
        return false;
    }
    if (x == NULL || x_length == 0) {
        x = "0";
        x_length = 1;
    }
    if (y == NULL || y_length == 0) {
        y = "0";
        y_length = 1;
    }
    char combined[512];
    int written = snprintf(
        combined, sizeof(combined), "translate(%.*s %.*s)%s%.*s",
        (int) x_length, x, (int) y_length, y,
        transform_length == 0 ? "" : " ", (int) transform_length,
        transform == NULL ? "" : transform);
    return written > 0 && (size_t) written < sizeof(combined)
        && lxb_dom_element_set_attribute(
               group, (const lxb_char_t *) "transform", 9,
               (const lxb_char_t *) combined, (size_t) written) != NULL;
}

static bool inline_svg_expand_one_use(InlineSvgUseValidation *validation,
                                      lxb_dom_node_t *use)
{
    lxb_dom_node_t *target = inline_svg_resolve_target(validation, use);
    if (target == NULL || use->parent == NULL) return false;
    static const lxb_char_t group_name[] = "g";
    lxb_dom_element_t *group = lxb_dom_document_create_element(
        use->owner_document, group_name, 1, NULL);
    if (group == NULL
        || !inline_svg_copy_instance_attributes(group, target)
        || !inline_svg_copy_instance_attributes(group, use)
        || !inline_svg_apply_use_translation(group, target, use)) {
        if (group != NULL) {
            lxb_dom_node_destroy_deep(lxb_dom_interface_node(group));
        }
        return false;
    }
    lxb_dom_node_t *group_node = lxb_dom_interface_node(group);
    bool container = image_name_is(target, "symbol")
        || image_name_is(target, "g") || image_name_is(target, "svg");
    lxb_dom_node_t *child = container ? target->first_child : target;
    while (child != NULL) {
        lxb_dom_node_t *next = container ? child->next : NULL;
        lxb_dom_node_t *copy = lxb_dom_node_clone(child, true);
        if (copy == NULL
            || lxb_dom_node_append_child(group_node, copy)
                   != LXB_DOM_EXCEPTION_OK) {
            if (copy != NULL && copy->parent == NULL) {
                lxb_dom_node_destroy_deep(copy);
            }
            lxb_dom_node_destroy_deep(group_node);
            return false;
        }
        child = next;
    }
    lxb_dom_node_t *parent = use->parent;
    if (lxb_dom_node_replace_child(parent, group_node, use)
            != LXB_DOM_EXCEPTION_OK) {
        lxb_dom_node_destroy_deep(group_node);
        return false;
    }
    lxb_dom_node_destroy_deep(use);
    return true;
}

static lxb_dom_node_t *inline_svg_first_rendered_use(lxb_dom_node_t *root)
{
    for (lxb_dom_node_t *node = root->first_child; node != NULL;
         node = inline_svg_next_node(
             root, node, inline_svg_definition_container(node))) {
        if (image_name_is(node, "use")) return node;
    }
    return NULL;
}

static void inline_svg_remove_symbols(lxb_dom_node_t *root)
{
    lxb_dom_node_t *node = root->first_child;
    while (node != NULL) {
        bool symbol = image_name_is(node, "symbol");
        lxb_dom_node_t *next = inline_svg_next_node(root, node, symbol);
        if (symbol) {
            lxb_dom_node_destroy_deep(node);
        }
        node = next;
    }
}

static bool inline_svg_apply_single_symbol_viewbox(
    InlineSvgUseValidation *validation, lxb_dom_node_t *root,
    lxb_dom_node_t *original_root)
{
    size_t existing_length = 0;
    if (document_attribute(original_root, "viewBox", &existing_length) != NULL
        || validation->use_count != 1) return true;
    lxb_dom_node_t *use = inline_svg_first_rendered_use(original_root);
    lxb_dom_node_t *symbol = use == NULL ? NULL
        : inline_svg_resolve_target(validation, use);
    size_t viewbox_length = 0;
    const char *viewbox = symbol == NULL ? NULL
        : document_attribute(symbol, "viewBox", &viewbox_length);
    if (viewbox == NULL || viewbox_length == 0) return true;
    return lxb_dom_element_set_attribute(
        lxb_dom_interface_element(root), (const lxb_char_t *) "viewBox", 7,
        (const lxb_char_t *) viewbox, viewbox_length) != NULL;
}

static bool inline_svg_apply_default_viewport(
    lxb_dom_node_t *root, const ComputedStyle *style)
{
    if (root == NULL || root->type != LXB_DOM_NODE_TYPE_ELEMENT) return false;
    lxb_dom_element_t *element = lxb_dom_interface_element(root);
    size_t length = 0;
    char width[16] = "300";
    char height[16] = "150";
    if (style != NULL && style->has_width && !style->width_percent
        && style->width_offset == 0 && style->width > 0
        && style->width <= 32767) {
        snprintf(width, sizeof(width), "%d", style->width);
    }
    if (style != NULL && style->has_height && !style->height_percent
        && style->height > 0 && style->height <= 32767) {
        snprintf(height, sizeof(height), "%d", style->height);
    }
    if (document_attribute(root, "width", &length) == NULL
        && lxb_dom_element_set_attribute(
               element, (const lxb_char_t *) "width", 5,
               (const lxb_char_t *) width, strlen(width)) == NULL) {
        return false;
    }
    if (document_attribute(root, "height", &length) == NULL
        && lxb_dom_element_set_attribute(
               element, (const lxb_char_t *) "height", 6,
               (const lxb_char_t *) height, strlen(height)) == NULL) {
        return false;
    }
    return true;
}

static bool inline_svg_value_is(const char *value, const char *wanted)
{
    return value != NULL && strcasecmp(value, wanted) == 0;
}

static const char *inline_svg_initial_presentation(const char *name)
{
    if (strcmp(name, "fill") == 0) return "#000000";
    if (strcmp(name, "stroke") == 0) return "none";
    if (strcmp(name, "fill-opacity") == 0
        || strcmp(name, "stroke-opacity") == 0
        || strcmp(name, "opacity") == 0
        || strcmp(name, "stroke-width") == 0) return "1";
    if (strcmp(name, "stroke-miterlimit") == 0) return "4";
    if (strcmp(name, "fill-rule") == 0) return "nonzero";
    if (strcmp(name, "stroke-linecap") == 0) return "butt";
    if (strcmp(name, "stroke-linejoin") == 0) return "miter";
    if (strcmp(name, "stroke-dasharray") == 0) return "none";
    if (strcmp(name, "stroke-dashoffset") == 0) return "0";
    return NULL;
}

static bool inline_svg_inherited_presentation(const char *name)
{
    return strcmp(name, "opacity") != 0;
}

static bool inline_svg_apply_presentation_value(
    lxb_dom_node_t *original, lxb_dom_node_t *clone,
    const ComputedStyle *style, const char *name, const char *css_value)
{
    char value[sizeof(((StyleCustomRule *) 0)->value)];
    size_t name_length = strlen(name);
    bool from_css = css_value != NULL;
    if (from_css) snprintf(value, sizeof(value), "%s", css_value);
    if (!from_css) {
        size_t attribute_length = 0;
        const char *attribute = document_attribute(
            original, name, &attribute_length);
        if (attribute != NULL && attribute_length < sizeof(value)) {
            memcpy(value, attribute, attribute_length);
            value[attribute_length] = '\0';
        } else {
            value[0] = '\0';
        }
    }
    bool inherited = inline_svg_inherited_presentation(name);
    if (inline_svg_value_is(value, "inherit")
        || (inherited && inline_svg_value_is(value, "unset"))) {
        value[0] = '\0';
    } else if (inline_svg_value_is(value, "initial")
               || (!inherited && inline_svg_value_is(value, "unset"))) {
        const char *initial = inline_svg_initial_presentation(name);
        if (initial == NULL) return true;
        snprintf(value, sizeof(value), "%s", initial);
    }
    if (value[0] == '\0' && inherited && clone->parent != NULL
        && clone->parent->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        size_t parent_length = 0;
        const char *parent = document_attribute(
            clone->parent, name, &parent_length);
        if (parent != NULL && parent_length < sizeof(value)) {
            memcpy(value, parent, parent_length);
            value[parent_length] = '\0';
        }
    }
    if (value[0] == '\0') return true;
    if (inline_svg_value_is(value, "currentcolor")) {
        snprintf(value, sizeof(value), "#%06x",
                 (unsigned) (style->color & UINT32_C(0xffffff)));
    }
    return lxb_dom_element_set_attribute(
        lxb_dom_interface_element(clone),
        (const lxb_char_t *) name, name_length,
        (const lxb_char_t *) value, strlen(value)) != NULL;
}

static bool inline_svg_apply_node_presentation(
    ImageLoadContext *context, lxb_dom_node_t *original,
    lxb_dom_node_t *clone, const ComputedStyle *style)
{
    StyleRetainedPresentationValues retained;
    if (!style_retained_presentation_values(
            context->stylesheet, original, &retained)) return false;
    for (size_t i = 0; i < STYLE_RETAINED_PRESENTATION_COUNT; i++) {
        const char *name = style_retained_presentation_name(i);
        uint16_t bit = (uint16_t) (UINT16_C(1) << i);
        const char *css_value = (retained.present_mask & bit) != 0
            ? retained.values[i] : NULL;
        if (name == NULL
            || !inline_svg_apply_presentation_value(
                original, clone, style, name, css_value)) return false;
    }
    return true;
}

static bool inline_svg_apply_presentation(
    ImageLoadContext *context, lxb_dom_node_t *original,
    lxb_dom_node_t *clone, const ComputedStyle *parent_style,
    size_t depth, size_t *node_count)
{
    if (original == NULL || clone == NULL || node_count == NULL
        || depth > INLINE_SVG_STYLE_DEPTH_LIMIT
        || ++*node_count > INLINE_SVG_STYLE_NODE_LIMIT) return false;
    ComputedStyle style = *parent_style;
    if (original->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        if (depth != 0) {
            style = style_for_node(
                context->stylesheet, original, parent_style);
        }
        if (!inline_svg_apply_node_presentation(
                context, original, clone, &style)) return false;
    }
    lxb_dom_node_t *original_child = original->first_child;
    lxb_dom_node_t *clone_child = clone->first_child;
    while (original_child != NULL && clone_child != NULL) {
        if (!inline_svg_apply_presentation(
                context, original_child, clone_child, &style,
                depth + 1, node_count)) return false;
        original_child = original_child->next;
        clone_child = clone_child->next;
    }
    return original_child == NULL && clone_child == NULL;
}

static bool inline_svg_serialize_expanded(ImageLoadContext *context,
                                          lxb_dom_node_t *node,
                                          const ComputedStyle *root_style,
                                          InlineSvgBuffer *source)
{
    InlineSvgUseValidation validation = {
        .document = context->document,
        .lookup_root = lxb_dom_interface_node(context->document->html)
    };
    if (!inline_svg_validate_tree(&validation, node, 0)) return false;
    lxb_dom_node_t *clone = lxb_dom_node_clone(node, true);
    size_t styled_nodes = 0;
    if (clone == NULL || root_style == NULL
        || !inline_svg_apply_presentation(
            context, node, clone, root_style, 0, &styled_nodes)) {
        if (clone != NULL) lxb_dom_node_destroy_deep(clone);
        return false;
    }
    validation.lookup_root = clone;
    if (!inline_svg_apply_default_viewport(clone, root_style)
        || !inline_svg_apply_single_symbol_viewbox(
               &validation, clone, node)) {
        lxb_dom_node_destroy_deep(clone);
        return false;
    }
    size_t expanded = 0;
    lxb_dom_node_t *use = NULL;
    while ((use = inline_svg_first_rendered_use(clone)) != NULL) {
        if (expanded++ >= validation.use_count
            || !inline_svg_expand_one_use(&validation, use)) {
            lxb_dom_node_destroy_deep(clone);
            return false;
        }
    }
    inline_svg_remove_symbols(clone);
    bool serialized = lxb_html_serialize_tree_cb(
        clone, inline_svg_receive, source) == LXB_STATUS_OK
        && source->data != NULL && source->length != 0;
    lxb_dom_node_destroy_deep(clone);
    return serialized;
}

/* NanoSVG deliberately implements a small SVG subset and does not resolve
   the CSS currentColor keyword.  Resolve it to the root SVG's computed color
   before rasterization; this preserves the common inherited icon case while
   keeping the retained raster independent of the stylesheet lifetime. */
static void inline_svg_resolve_current_color(InlineSvgBuffer *buffer,
                                             uint32_t color)
{
    if (buffer == NULL || buffer->data == NULL) return;
    char replacement[8];
    snprintf(replacement, sizeof(replacement), "#%06x",
             (unsigned) (color & UINT32_C(0xffffff)));
    static const char keyword[] = "currentColor";
    size_t read = 0, write = 0;
    while (read < buffer->length) {
        if (buffer->length - read >= sizeof(keyword) - 1u
            && strncasecmp(buffer->data + read, keyword,
                           sizeof(keyword) - 1u) == 0) {
            memcpy(buffer->data + write, replacement, 7);
            write += 7;
            read += sizeof(keyword) - 1u;
        } else {
            buffer->data[write++] = buffer->data[read++];
        }
    }
    buffer->length = write;
    buffer->data[write] = '\0';
}

static bool external_svg_materialize_symbol(
    ImageLoadContext *context, lxb_dom_node_t *node,
    const FetchResult *fetched, InlineSvgBuffer *output)
{
    size_t href_length = 0;
    const char *href = inline_svg_external_use_href(node, &href_length);
    if (href == NULL || fetched == NULL || fetched->data == NULL
        || output == NULL) return false;
    size_t fragment = href_length;
    while (fragment != 0 && href[fragment - 1] != '#') fragment--;
    if (fragment == 0 || fragment == href_length) return false;
    size_t attributes_start = 0, open_end = 0, close_start = 0;
    if (!external_svg_symbol_bounds(
            fetched->data, fetched->length, href + fragment,
            href_length - fragment, &attributes_start, &open_end,
            &close_start)) return false;

    ComputedStyle style = {0};
    if (!image_style_chain_for_node(context->stylesheet, node, &style)) {
        return false;
    }
    int width = style.has_width && !style.width_percent
                && style.width_offset == 0 && style.width > 0
                && style.width <= 1024 ? style.width : 16;
    int height = style.has_height && !style.height_percent
                 && style.height > 0 && style.height <= 1024
                 ? style.height : width;
    size_t attributes_length = open_end - attributes_start;
    size_t content_start = open_end + 1u;
    size_t content_length = close_start - content_start;
    if (attributes_length > EXTERNAL_SVG_SYMBOL_TAG_LIMIT
        || content_length > EXTERNAL_SVG_SYMBOL_LIMIT) return false;
    size_t capacity = attributes_length + content_length + 192u;
    output->budget = context->budget;
    output->data = budget_malloc(context->budget, capacity);
    if (output->data == NULL) return false;
    output->capacity = capacity;
    int prefix = snprintf(
        output->data, capacity,
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%d\" "
        "height=\"%d\"%.*s>",
        width, height, (int) attributes_length,
        fetched->data + attributes_start);
    if (prefix < 0 || (size_t) prefix >= capacity
        || content_length > capacity - (size_t) prefix - 7u) {
        budget_free(context->budget, output->data);
        memset(output, 0, sizeof(*output));
        return false;
    }
    memcpy(output->data + prefix, fetched->data + content_start,
           content_length);
    output->length = (size_t) prefix + content_length;
    memcpy(output->data + output->length, "</svg>", 7u);
    output->length += 6u;
    inline_svg_resolve_current_color(output, style.color);
    return true;
}

static bool load_inline_svg(ImageLoadContext *context, lxb_dom_node_t *node,
                            const ComputedStyle *style)
{
    ImageResources *images = context->images;
    if (image_node_already_tracked(
            images, node, 0, false, false, PSEUDO_NONE)) {
        return true;
    }
    images->stats.discovered++;
    if (images->count >= MAX_TRACKED_IMAGE_NODES
        || images->stats.attempted >= context->maximum_count
        || images->stats.decoded_bytes >= context->maximum_decoded_bytes) {
        images->stats.skipped_limit++;
        return true;
    }
    size_t failures_before = context->budget->failure_count;
    InlineSvgBuffer source = {.budget = context->budget};
    if (!inline_svg_serialize_expanded(context, node, style, &source)) {
        budget_free(context->budget, source.data);
        if (context->budget->failure_count != failures_before) return false;
        images->stats.unsupported++;
        image_trace("inline-svg-serialize-unsupported", "", 0);
        return true;
    }
    inline_svg_resolve_current_color(&source,
                                     style == NULL ? 0 : style->color);
    image_trace("inline-svg", source.data, source.length);
    uint64_t hash = image_hash_bytes(source.data, source.length);
    const ImageResource *duplicate = image_find_hash(images, hash);
    if (duplicate != NULL) {
        ImageResource alias = *duplicate;
        alias.node = node;
        alias.is_mask = false;
        alias.is_background = false;
        alias.pseudo = PSEUDO_NONE;
        alias.owns_pixels = false;
        alias.owns_encoded = false;
        images->stats.duplicate++;
        budget_free(context->budget, source.data);
        return image_add(images, alias);
    }
    size_t remaining = context->maximum_decoded_bytes
                       - images->stats.decoded_bytes;
    int width = 0, height = 0;
    images->stats.attempted++;
    unsigned char *pixels = decode_svg(
        source.data, source.length, context->budget, remaining,
        &width, &height);
    budget_free(context->budget, source.data);
    if (pixels == NULL || width <= 0 || height <= 0
        || (size_t) width > SIZE_MAX / (size_t) height
        || (size_t) width * (size_t) height > SIZE_MAX / 4u) {
        budget_free(context->budget, pixels);
        if (context->budget->failure_count != failures_before) return false;
        images->stats.unsupported++;
        image_trace("inline-svg-decode-unsupported", "", 0);
        return true;
    }
    size_t decoded = (size_t) width * (size_t) height * 4u;
    ImageResource resource = {
        .node = node, .url_hash = hash, .pixels = pixels,
        .source_width = width, .source_height = height,
        .width = width, .height = height, .owns_pixels = true
    };
    if (!image_add(images, resource)) {
        budget_free(context->budget, pixels);
        return false;
    }
    images->stats.loaded++;
    images->stats.decoded_bytes += decoded;
    if (decoded > images->stats.largest_source_decode_bytes) {
        images->stats.largest_source_decode_bytes = decoded;
    }
    if (decoded > images->stats.largest_target_decode_bytes) {
        images->stats.largest_target_decode_bytes = decoded;
    }
    return true;
}

static int parse_source_size_length(const Stylesheet *stylesheet,
                                    const char *text, size_t length)
{
    while (length != 0 && isspace((unsigned char) *text)) {
        text++;
        length--;
    }
    while (length != 0
           && isspace((unsigned char) text[length - 1])) length--;
    if (length == 0 || length > 128) return -1;
    bool percent = false;
    int pixels = style_parse_length(
        stylesheet, text, length, INT_MIN, &percent);
    /* A source size is a non-negative CSS length. Percentages are invalid,
       while calc()/min()/max()/clamp(), font-relative, absolute, and
       viewport units share the engine's ordinary bounded length parser. */
    return pixels == INT_MIN || percent || pixels < 0 ? -1 : pixels;
}

static int image_source_size(const Stylesheet *stylesheet,
                             lxb_dom_node_t *candidate,
                             lxb_dom_node_t *image,
                             int layout_width)
{
    size_t length = 0;
    const char *sizes = document_attribute(candidate, "sizes", &length);
    if ((sizes == NULL || length == 0) && candidate != image) {
        sizes = document_attribute(image, "sizes", &length);
    }
    int viewport_width = stylesheet == NULL ? 480
                                             : stylesheet->viewport_width;
    if (sizes == NULL || length == 0) {
        return layout_width > 0 ? layout_width : viewport_width;
    }
    size_t start = 0;
    unsigned parentheses = 0;
    for (size_t at = 0; at <= length; at++) {
        char character = at < length ? sizes[at] : ',';
        if (character == '(') parentheses++;
        else if (character == ')' && parentheses != 0) parentheses--;
        if (character != ',' || parentheses != 0) continue;
        const char *item = sizes + start;
        size_t item_length = at - start;
        while (item_length != 0
               && isspace((unsigned char) *item)) {
            item++;
            item_length--;
        }
        while (item_length != 0
               && isspace((unsigned char) item[item_length - 1])) {
            item_length--;
        }
        size_t length_start = item_length;
        while (length_start != 0
               && !isspace((unsigned char) item[length_start - 1])) {
            length_start--;
        }
        int pixels = parse_source_size_length(
            stylesheet, item + length_start,
            item_length - length_start);
        if (pixels >= 0) {
            size_t media_length = length_start;
            while (media_length != 0
                   && isspace((unsigned char) item[media_length - 1])) {
                media_length--;
            }
            if (media_length == 0
                || (stylesheet != NULL
                    && stylesheet_media_matches(
                        stylesheet, item, media_length))) {
                return pixels;
            }
        }
        start = at + 1;
    }
    return viewport_width;
}

static const char *select_srcset(const char *text, size_t length,
                                 int source_size, const char *fallback,
                                 size_t fallback_length,
                                 size_t *selected_length)
{
    enum { SRCSET_DESCRIPTOR_NONE, SRCSET_DESCRIPTOR_WIDTH,
           SRCSET_DESCRIPTOR_DENSITY };
    const char *above = NULL, *below = NULL;
    size_t above_length = 0, below_length = 0;
    double above_density = HUGE_VAL, below_density = -1.0;
    int descriptor_kind = SRCSET_DESCRIPTOR_NONE;
    for (size_t at = 0; at < length;) {
        while (at < length
               && (isspace((unsigned char) text[at]) || text[at] == ',')) at++;
        size_t url_start = at;
        /* URL tokens end at ASCII whitespace, not at an embedded comma:
           data: URLs in responsive-image sets routinely contain commas. A
           trailing comma is the candidate separator and is stripped below,
           matching HTML's srcset tokenizer. */
        while (at < length && !isspace((unsigned char) text[at])) at++;
        size_t url_end = at;
        bool trailing_comma = false;
        while (url_end > url_start && text[url_end - 1] == ',') {
            url_end--;
            trailing_comma = true;
        }
        while (at < length && isspace((unsigned char) text[at])) at++;
        size_t descriptor_start = at;
        if (!trailing_comma) {
            while (at < length && text[at] != ',') at++;
        }
        size_t descriptor_end = at;
        while (descriptor_end > descriptor_start
               && isspace((unsigned char) text[descriptor_end - 1])) {
            descriptor_end--;
        }
        if (url_end == url_start) continue;
        char descriptor[32] = {0};
        size_t descriptor_length = descriptor_end - descriptor_start;
        if (descriptor_length >= sizeof(descriptor)) {
            descriptor_length = sizeof(descriptor) - 1;
        }
        memcpy(descriptor, text + descriptor_start, descriptor_length);
        char *end = NULL;
        double number = strtod(descriptor, &end);
        double density = -1.0;
        int candidate_kind = SRCSET_DESCRIPTOR_NONE;
        if (end != descriptor && (*end == 'w' || *end == 'W')
            && number > 0.0 && source_size > 0) {
            char *tail = end + 1;
            while (isspace((unsigned char) *tail)) tail++;
            bool valid_height = *tail == '\0';
            if (!valid_height) {
                char *height_end = NULL;
                double height = strtod(tail, &height_end);
                valid_height = height_end != tail && height > 0.0
                    && (*height_end == 'h' || *height_end == 'H')
                    && height_end[1] == '\0';
            }
            if (valid_height) {
                density = number / source_size;
                candidate_kind = SRCSET_DESCRIPTOR_WIDTH;
            }
        } else if (end != descriptor && (*end == 'x' || *end == 'X')
                   && number > 0.0 && end[1] == '\0') {
            density = number;
            candidate_kind = SRCSET_DESCRIPTOR_DENSITY;
        } else if (descriptor_length == 0) {
            density = 1.0;
            candidate_kind = SRCSET_DESCRIPTOR_DENSITY;
        }
        if (density > 0.0) {
            /* A source set has one descriptor family.  Keeping the family
               established by its first valid candidate makes mixed w/x
               authoring deterministic and matches HTML's candidate-set
               processing rather than comparing unlike units. */
            if (descriptor_kind == SRCSET_DESCRIPTOR_NONE) {
                descriptor_kind = candidate_kind;
            }
            if (candidate_kind != descriptor_kind) continue;
            if (density >= 1.0 && density < above_density) {
                above = text + url_start;
                above_length = url_end - url_start;
                above_density = density;
            } else if (density < 1.0 && density > below_density) {
                below = text + url_start;
                below_length = url_end - url_start;
                below_density = density;
            }
        }
    }
    /* In a density-descriptor srcset, HTML treats src as an implicit 1x
       candidate. Omitting it made ordinary 250px/500px article images pick
       the 2x source at device scale 1 and waste decode budget. Width-
       descriptor lists deliberately ignore src. */
    if (descriptor_kind != SRCSET_DESCRIPTOR_WIDTH
        && fallback != NULL && fallback_length != 0
        && (above == NULL || above_density > 1.0)) {
        above = fallback;
        above_length = fallback_length;
        above_density = 1.0;
    }
    const char *selected = above != NULL ? above : below;
    *selected_length = above != NULL ? above_length
                       : (below != NULL ? below_length : 0);
    return selected;
}

static bool supported_picture_type(lxb_dom_node_t *node)
{
    size_t length = 0;
    const char *type = document_attribute(node, "type", &length);
    return type == NULL || length == 0
        || (length == 9 && memcmp(type, "image/png", 9) == 0)
        || (length == 10 && memcmp(type, "image/webp", 10) == 0)
        || (length == 10 && memcmp(type, "image/jpeg", 10) == 0)
        || (length == 13 && memcmp(type, "image/svg+xml", 13) == 0);
}

static bool image_source_is_placeholder(const char *source, size_t length)
{
    if (source == NULL || length == 0) return true;
    while (length != 0 && isspace((unsigned char) *source)) {
        source++;
        length--;
    }
    while (length != 0
           && isspace((unsigned char) source[length - 1u])) length--;
    if (length == 0) return true;
    if ((length == 11u && strncasecmp(source, "about:blank", 11u) == 0)
        || (length == 1u && source[0] == '#')) return true;
    /* Recognize the conventional one-pixel GIF/PNG sentinels by their encoded
       dimensions. A byte-size threshold also catches small authored icons,
       which must remain the image source even when data-* metadata is present. */
    static const char gif_1x1[] =
        "data:image/gif;base64,R0lGODlhAQABA";
    static const char png_1x1[] =
        "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAAB";
    return (length >= sizeof(gif_1x1) - 1u
            && strncasecmp(source, gif_1x1, 22u) == 0
            && memcmp(source + 22u, gif_1x1 + 22u,
                      sizeof(gif_1x1) - 1u - 22u) == 0)
        || (length >= sizeof(png_1x1) - 1u
            && strncasecmp(source, png_1x1, 22u) == 0
            && memcmp(source + 22u, png_1x1 + 22u,
                      sizeof(png_1x1) - 1u - 22u) == 0);
}

static bool image_lazy_source_is_url(const char *source, size_t length)
{
    if (source == NULL || length == 0) return false;
    while (length != 0 && isspace((unsigned char) *source)) {
        source++;
        length--;
    }
    while (length != 0
           && isspace((unsigned char) source[length - 1u])) length--;
    if (length == 0 || source[0] == '#') return false;
    static const char *const rejected[] = {
        "data:", "javascript:", "vbscript:", "about:"
    };
    for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
        size_t prefix = strlen(rejected[i]);
        if (length >= prefix
            && strncasecmp(source, rejected[i], prefix) == 0) return false;
    }
    for (size_t i = 0; i < length; i++) {
        unsigned char byte = (unsigned char) source[i];
        if (byte < 0x20u || byte == 0x7fu || byte == '<' || byte == '>')
            return false;
    }
    return true;
}

static const char *image_lazy_source(lxb_dom_node_t *node, size_t *length)
{
    static const char *const names[] = {
        "data-src", "data-original", "data-thumb", "data-lazy-src"
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        size_t candidate_length = 0;
        const char *candidate = document_attribute(
            node, names[i], &candidate_length);
        while (candidate_length != 0
               && isspace((unsigned char) *candidate)) {
            candidate++;
            candidate_length--;
        }
        while (candidate_length != 0
               && isspace((unsigned char) candidate[candidate_length - 1u]))
            candidate_length--;
        if (image_lazy_source_is_url(candidate, candidate_length)) {
            *length = candidate_length;
            return candidate;
        }
    }
    return NULL;
}

static const char *image_select_source_for_width(
    const Stylesheet *stylesheet, lxb_dom_node_t *node, int layout_width,
    size_t *length)
{
    if (length == NULL) return NULL;
    *length = 0;
    if (node == NULL) return NULL;
    if (node->parent != NULL && image_name_is(node->parent, "picture")) {
        for (lxb_dom_node_t *child = node->parent->first_child;
             child != NULL && child != node; child = child->next) {
            if (!image_name_is(child, "source")
                || !supported_picture_type(child)) continue;
            size_t media_length = 0;
            const char *media = document_attribute(child, "media",
                                                    &media_length);
            if (media != NULL && media_length != 0
                && (stylesheet == NULL
                    || !stylesheet_media_matches(stylesheet, media,
                                                  media_length))) continue;
            size_t srcset_length = 0;
            const char *srcset = document_attribute(child, "srcset",
                                                     &srcset_length);
            if (srcset == NULL || srcset_length == 0) {
                srcset = document_attribute(child, "data-srcset",
                                            &srcset_length);
            }
            const char *selected = select_srcset(
                srcset, srcset == NULL ? 0 : srcset_length,
                image_source_size(stylesheet, child, node, layout_width),
                NULL, 0, length);
            if (selected != NULL) return selected;
        }
    }
    size_t srcset_length = 0;
    const char *srcset = document_attribute(node, "srcset", &srcset_length);
    if (srcset == NULL || srcset_length == 0) {
        srcset = document_attribute(node, "data-srcset", &srcset_length);
    }
    size_t source_length = 0;
    const char *source = document_attribute(node, "src", &source_length);
    const char *selected = select_srcset(
        srcset, srcset == NULL ? 0 : srcset_length,
        image_source_size(stylesheet, node, node, layout_width),
        source, source_length, length);
    if (selected != NULL
        && !image_source_is_placeholder(selected, *length)) return selected;
    size_t lazy_length = 0;
    const char *lazy = image_lazy_source(node, &lazy_length);
    if (lazy != NULL) {
        *length = lazy_length;
        return lazy;
    }
    if (selected != NULL) return selected;
    *length = source_length;
    return source;
}

const char *image_select_source(const Stylesheet *stylesheet,
                                lxb_dom_node_t *node, size_t *length)
{
    return image_select_source_for_width(stylesheet, node, 0, length);
}

static bool svg_response(const FetchResult *fetched)
{
    if (strstr(fetched->content_type, "image/svg+xml") != NULL) return true;
    size_t maximum = fetched->length < 512 ? fetched->length : 512;
    for (size_t i = 0; i + 4 <= maximum; i++) {
        if (fetched->data[i] == '<'
            && tolower((unsigned char) fetched->data[i + 1]) == 's'
            && tolower((unsigned char) fetched->data[i + 2]) == 'v'
            && tolower((unsigned char) fetched->data[i + 3]) == 'g') return true;
    }
    return false;
}

static unsigned char *decode_svg(const void *data, size_t length,
                                 Budget *budget, size_t maximum_decoded_bytes,
                                 int *width, int *height)
{
    if (data == NULL || budget == NULL || width == NULL || height == NULL
        || maximum_decoded_bytes < 4 || svg_budget != NULL) return NULL;
    uint64_t metadata_checkpoint = budget_checkpoint(budget);
    svg_budget = budget;
    char *source = budget_malloc(budget, length + 1);
    if (source != NULL) {
        memcpy(source, data, length);
        source[length] = '\0';
    }
    NSVGimage *svg = source == NULL
        ? NULL : nsvgParse(source, "px", 96.0f);
    if (svg == NULL || svg->width <= 0.0f || svg->height <= 0.0f
        || svg->width > 32767.0f || svg->height > 32767.0f) {
        if (svg != NULL) nsvgDelete(svg);
        svg_budget = NULL;
        budget_rollback(budget, metadata_checkpoint);
        return NULL;
    }
    int output_width = (int) ceilf(svg->width);
    int output_height = (int) ceilf(svg->height);
    if (output_width <= 0 || output_height <= 0
        || (size_t) output_width > SIZE_MAX / (size_t) output_height
        || (size_t) output_width * (size_t) output_height > SIZE_MAX / 4u
        || (size_t) output_width * (size_t) output_height * 4u
           > maximum_decoded_bytes) {
        nsvgDelete(svg);
        svg_budget = NULL;
        budget_rollback(budget, metadata_checkpoint);
        return NULL;
    }
    nsvgDelete(svg);
    svg_budget = NULL;
    budget_rollback(budget, metadata_checkpoint);

    size_t bytes = (size_t) output_width * (size_t) output_height * 4u;
    unsigned char *pixels = budget_calloc(budget, 1, bytes);
    if (pixels == NULL) return NULL;
    uint64_t raster_checkpoint = budget_checkpoint(budget);
    svg_budget = budget;
    source = budget_malloc(budget, length + 1);
    if (source != NULL) {
        memcpy(source, data, length);
        source[length] = '\0';
    }
    svg = source == NULL ? NULL : nsvgParse(source, "px", 96.0f);
    NSVGrasterizer *rasterizer = svg != NULL
        ? nsvgCreateRasterizer() : NULL;
    if (svg == NULL || rasterizer == NULL) {
        if (rasterizer != NULL) nsvgDeleteRasterizer(rasterizer);
        if (svg != NULL) nsvgDelete(svg);
        svg_budget = NULL;
        budget_rollback(budget, raster_checkpoint);
        budget_free(budget, pixels);
        return NULL;
    }
    float scale_x = (float) output_width / svg->width;
    float scale_y = (float) output_height / svg->height;
    float scale = scale_x < scale_y ? scale_x : scale_y;
    nsvgRasterize(rasterizer, svg, 0.0f, 0.0f, scale, pixels,
                  output_width, output_height, output_width * 4);
    nsvgDeleteRasterizer(rasterizer);
    nsvgDelete(svg);
    svg_budget = NULL;
    /* Parser/rasterizer state is scratch-only. A generation rollback also
       contains malformed-input leaks in the third-party SVG parser while the
       pixel allocation, made before the checkpoint, remains owned. */
    budget_rollback(budget, raster_checkpoint);
    *width = output_width;
    *height = output_height;
    return pixels;
}

static bool pending_add_target(ImageLoadContext *context,
                               PendingImageFetch *pending,
                               lxb_dom_node_t *node, uint64_t source_hash,
                               bool is_mask,
                               bool is_background, PseudoElement pseudo)
{
    if (pending->target_count >= MAX_TRACKED_IMAGE_NODES) {
        context->images->stats.skipped_limit++;
        return true;
    }
    if (pending->target_count == pending->target_capacity) {
        size_t capacity = pending->target_capacity == 0
                          ? 4 : pending->target_capacity * 2;
        if (capacity > MAX_TRACKED_IMAGE_NODES) {
            capacity = MAX_TRACKED_IMAGE_NODES;
        }
        PendingImageTarget *targets = budget_realloc(
            context->budget, pending->targets, capacity * sizeof(*targets));
        if (targets == NULL) return false;
        pending->targets = targets;
        pending->target_capacity = capacity;
    }
    pending->targets[pending->target_count++] = (PendingImageTarget) {
        .node = node,
        .source_hash = source_hash,
        .display_width = context->current_display_width,
        .display_height = context->current_display_height,
        .is_mask = is_mask,
        .is_background = is_background,
        .pseudo = pseudo
    };
    return true;
}

static bool image_layout_decode_target(
    const PendingImageFetch *pending, int source_width, int source_height,
    int *target_width, int *target_height)
{
    if (pending == NULL || pending->target_count == 0
        || source_width <= 0 || source_height <= 0
        || target_width == NULL || target_height == NULL) return false;
    int width = 0, height = 0;
    for (size_t i = 0; i < pending->target_count; i++) {
        const PendingImageTarget *target = &pending->targets[i];
        if (target->is_mask || target->is_background
            || target->display_width == 0 || target->display_height == 0) {
            return false;
        }
        int requested_width = target->display_width;
        int requested_height = target->display_height;
        if (requested_width >= source_width
            || requested_height >= source_height) return false;
        int scaled_width = requested_width;
        int scaled_height = requested_height;
        /* Preserve the source aspect and cover the proven box. Products are
           bounded by the admitted source pixel count, so this stays on the
           PSP's native 32-bit integer path. */
        size_t width_scale = (size_t) requested_width
                             * (size_t) source_height;
        size_t height_scale = (size_t) requested_height
                              * (size_t) source_width;
        if (width_scale >= height_scale) {
            size_t product = (size_t) source_height
                             * (size_t) requested_width;
            scaled_height = (int) (product / (size_t) source_width
                + (product % (size_t) source_width != 0));
        } else {
            size_t product = (size_t) source_width
                             * (size_t) requested_height;
            scaled_width = (int) (product / (size_t) source_height
                + (product % (size_t) source_height != 0));
        }
        if (scaled_width > width) width = scaled_width;
        if (scaled_height > height) height = scaled_height;
    }
    if (width <= 0 || height <= 0
        || width >= source_width || height >= source_height) return false;
    *target_width = width;
    *target_height = height;
    return true;
}

typedef enum {
    IMAGE_DECODED_CACHE_MISS = 0,
    IMAGE_DECODED_CACHE_USED,
    IMAGE_DECODED_CACHE_FAILED
} ImageDecodedCacheResult;

static ImageDecodedCacheResult image_adopt_decoded_cache(
    ImageLoadContext *context, PendingImageFetch *pending,
    const BrowserCacheEntry *cached,
    const TilefinchRequestContext *request_context)
{
    if (context == NULL || pending == NULL || cached == NULL
        || request_context == NULL || context->session == NULL
        || pending->url == NULL || pending->target_count == 0
        || cached->decoded_image_source_width <= 0
        || cached->decoded_image_source_height <= 0) {
        return IMAGE_DECODED_CACHE_MISS;
    }
    int target_width = 0, target_height = 0;
    if (!image_layout_decode_target(
            pending, cached->decoded_image_source_width,
            cached->decoded_image_source_height,
            &target_width, &target_height)
        || target_width != cached->decoded_image_width
        || target_height != cached->decoded_image_height) {
        return IMAGE_DECODED_CACHE_MISS;
    }
    BrowserDecodedImage decoded = {0};
    if (!browser_session_decoded_image_acquire(
            context->session, pending->url, request_context,
            cached->data, cached->length, &decoded)) {
        return IMAGE_DECODED_CACHE_MISS;
    }
    if (decoded.width != target_width || decoded.height != target_height
        || decoded.source_width != cached->decoded_image_source_width
        || decoded.source_height != cached->decoded_image_source_height
        || decoded.pixels == NULL
        || context->images->stats.decoded_bytes
               > context->maximum_decoded_bytes
        || decoded.pixels->length
               > context->maximum_decoded_bytes
                    - context->images->stats.decoded_bytes) {
        browser_shared_body_release(decoded.pixels);
        return IMAGE_DECODED_CACHE_MISS;
    }
    PendingImageTarget primary = pending->targets[0];
    ImageResource resource = {
        .node = primary.node,
        .url_hash = pending->url_hash,
        .source_hash = primary.source_hash,
        .pixels = decoded.pixels->data,
        .pixel_body = decoded.pixels,
        .source_width = decoded.source_width,
        .source_height = decoded.source_height,
        .width = decoded.width,
        .height = decoded.height,
        .is_mask = primary.is_mask,
        .is_background = primary.is_background,
        .pseudo = primary.pseudo,
        .owns_pixels = true
    };
    size_t before = context->images->count;
    if (!image_add(context->images, resource)
        || context->images->count == before) {
        image_resource_release_owned_pixels(context->budget, &resource);
        return IMAGE_DECODED_CACHE_FAILED;
    }
    context->images->stats.loaded++;
    context->images->stats.cache_hits++;
    context->images->stats.decoded_cache_hits++;
    context->images->stats.encoded_bytes += cached->length;
    context->images->stats.decoded_bytes += decoded.pixels->length;
    if (primary.is_mask) context->images->stats.masks_loaded++;
    if (primary.is_background) context->images->stats.backgrounds_loaded++;
    if (decoded.width != decoded.source_width
        || decoded.height != decoded.source_height) {
        context->images->stats.downsampled++;
    }
    size_t source_bytes = 0;
    if ((size_t) decoded.source_width
            <= SIZE_MAX / (size_t) decoded.source_height
        && (size_t) decoded.source_width * (size_t) decoded.source_height
               <= SIZE_MAX / 4u) {
        source_bytes = (size_t) decoded.source_width
                       * (size_t) decoded.source_height * 4u;
    }
    if (source_bytes
            > context->images->stats.largest_source_decode_bytes) {
        context->images->stats.largest_source_decode_bytes = source_bytes;
    }
    if (decoded.pixels->length
            > context->images->stats.largest_target_decode_bytes) {
        context->images->stats.largest_target_decode_bytes =
            decoded.pixels->length;
    }
    for (size_t i = 1; i < pending->target_count; i++) {
        PendingImageTarget target = pending->targets[i];
        ImageResource alias = resource;
        alias.node = target.node;
        alias.source_hash = target.source_hash;
        alias.is_mask = target.is_mask;
        alias.is_background = target.is_background;
        alias.pseudo = target.pseudo;
        alias.owns_pixels = false;
        context->images->stats.duplicate++;
        if (!image_add(context->images, alias)) {
            return IMAGE_DECODED_CACHE_FAILED;
        }
    }
    return IMAGE_DECODED_CACHE_USED;
}

static PendingImageFetch *pending_find_hash(ImageLoadContext *context,
                                            uint64_t hash)
{
    for (size_t i = 0; i < IMAGE_FETCH_CONCURRENCY; i++) {
        if (context->pending[i].request_id != 0
            && context->pending[i].url_hash == hash) {
            return &context->pending[i];
        }
    }
    return NULL;
}

static PendingImageFetch *pending_find_free(ImageLoadContext *context)
{
    for (size_t i = 0; i < IMAGE_FETCH_CONCURRENCY; i++) {
        if (context->pending[i].request_id == 0) {
            return &context->pending[i];
        }
    }
    return NULL;
}

static bool finish_image_fetch(ImageLoadContext *context,
                               PendingImageFetch *pending,
                               FetchResult *fetched, bool success)
{
    ImageResources *images = context->images;
    if (!success) {
        images->stats.failed++;
        image_trace("fetch-failed", pending->url,
                    pending->url == NULL ? 0 : strlen(pending->url));
        fetch_result_destroy(fetched);
        return true;
    }
    image_accept_response_cookies(context, pending->url, fetched);
    TilefinchRequestContext request_context = image_request_context(
        context, pending->url);
    TilefinchResourceGrant resource_grant = pending->resource_grant;
    if (pending->url != NULL && !pending->resource_grant_valid
        && !fetch_resource_grant_create(
               fetched, &request_context, false, true, false,
               &resource_grant, NULL)) {
        images->stats.failed++;
        image_trace("resource-denied", pending->url, strlen(pending->url));
        fetch_result_destroy(fetched);
        return true;
    }
    (void) fetch_result_share_body(fetched);
    bool transient_large_sprite = pending->target_count != 0
        && fetched->length > context->maximum_single_encoded_bytes
        && inline_svg_external_use_href(
               pending->targets[0].node, NULL) != NULL;
    if (context->session != NULL && pending->url != NULL
        && !pending->from_resource_cache && !transient_large_sprite) {
        char cache_control[256] = {0};
        char vary[128] = {0};
        (void) fetch_response_header_value(
            fetched, "cache-control", cache_control, sizeof(cache_control));
        (void) fetch_response_header_value(fetched, "vary", vary,
                                           sizeof(vary));
        bool stored = false;
        if (fetched->shared_body != NULL) {
            stored = browser_session_cache_put_http_shared_resource(
                context->session, pending->url, fetched->shared_body,
                fetched->etag, fetched->last_modified, fetched->content_type,
                cache_control, vary, tilefinch_platform_monotonic_time_ns(),
                &request_context, &resource_grant);
            if (stored) {
                stored = browser_session_cache_set_resource_response_provenance(
                    context->session, pending->url, &request_context,
                    fetched->effective_url, "");
            }
        }
        image_trace(stored ? "cache-stored" : "cache-store-rejected",
                    pending->url, strlen(pending->url));
    } else if (transient_large_sprite) {
        image_trace("cache-bypass-large-sprite", pending->url,
                    pending->url == NULL ? 0 : strlen(pending->url));
    }
    if (pending->target_count == 0
        || fetched->length > context->maximum_total_encoded_bytes
                              - images->stats.encoded_bytes) {
        images->stats.skipped_limit++;
        fetch_result_destroy(fetched);
        return true;
    }
    /* A primary resource owns decoded pixels or the detached response body.
       Never perform that ownership handoff when the bounded node table has no
       slot. Alias additions may still be dropped nonfatally because aliases
       own neither allocation. */
    if (images->count >= MAX_TRACKED_IMAGE_NODES) {
        images->stats.skipped_limit++;
        fetch_result_destroy(fetched);
        return true;
    }
    PendingImageTarget primary = pending->targets[0];
    int width = 0, height = 0, components = 0;
    bool is_svg = svg_response(fetched);
    bool supported = is_svg;
    InlineSvgBuffer external_symbol = {0};
    const void *decode_data = fetched->data;
    size_t decode_length = fetched->length;
    bool external_use = is_svg
        && inline_svg_external_use_href(primary.node, NULL) != NULL;
    if (external_use) {
        if (external_svg_materialize_symbol(
                context, primary.node, fetched, &external_symbol)) {
            decode_data = external_symbol.data;
            decode_length = external_symbol.length;
            image_trace("external-svg-use", pending->url,
                        pending->url == NULL ? 0 : strlen(pending->url));
        } else {
            is_svg = false;
            supported = false;
        }
    }
    const unsigned char *encoded = (const unsigned char *) fetched->data;
    bool is_webp = image_is_webp(encoded, fetched->length);
    if (is_webp) {
        supported = WebPGetInfo(
            encoded, fetched->length, &width, &height) != 0;
        components = 4;
    } else if (!is_svg) {
        uint64_t probe_checkpoint = budget_checkpoint(context->budget);
        size_t probe_baseline = context->budget->current;
        decode_budget = context->budget;
        supported = fetched->length <= INT32_MAX
                    && stbi_info_from_memory((const stbi_uc *) fetched->data,
                                             (int) fetched->length, &width,
                                             &height, &components) != 0;
        decode_budget = NULL;
        /* stb_image's metadata API has no returned allocation ownership.
           Malformed inputs in some format probes can strand decoder scratch;
           reclaim only allocations made by this synchronous probe. */
        if (context->budget->current != probe_baseline) {
            budget_rollback(context->budget, probe_checkpoint);
        }
    }
    size_t decoded_remaining = context->maximum_decoded_bytes
                               - images->stats.decoded_bytes;
    unsigned char *pixels = NULL;
    if (is_svg && (!external_use || external_symbol.data != NULL)) {
        uint64_t svg_checkpoint = budget_checkpoint(context->budget);
        size_t svg_baseline = context->budget->current;
        pixels = decode_svg(decode_data, decode_length, context->budget,
                            decoded_remaining, &width, &height);
        supported = pixels != NULL;
        /* NanoSVG does not expose a partial parse object when parsing fails.
           Its scratch is transaction-local, so reclaim that generation when
           no pixel ownership was returned. */
        if (pixels == NULL && context->budget->current != svg_baseline) {
            budget_rollback(context->budget, svg_checkpoint);
        }
    }
    budget_free(context->budget, external_symbol.data);
    if (!supported || width <= 0 || height <= 0
        || (size_t) width > SIZE_MAX / (size_t) height
        || (size_t) width * (size_t) height > SIZE_MAX / 4u) {
        images->stats.unsupported++;
        image_trace("decode-unsupported", fetched->effective_url,
                    strlen(fetched->effective_url));
        budget_free(context->budget, pixels);
        fetch_result_destroy(fetched);
        return true;
    }
    int source_width = width, source_height = height;
    size_t source_decoded = (size_t) width * (size_t) height * 4u;
    size_t decoded = source_decoded;
    size_t decoded_limit = is_svg ? decoded_remaining
                                  : context->maximum_decoded_bytes;
    if (!is_svg) {
        size_t working_limit = context->maximum_decoded_bytes;
        if (working_limit <= SIZE_MAX / 4u) working_limit *= 4u;
        else working_limit = SIZE_MAX;
        const size_t working_cap = context->eager_decode_rasters
            ? MAX_PRIORITY_RASTER_DECODE_WORKING_BYTES
            : MAX_RASTER_DECODE_WORKING_BYTES;
        if (context->maximum_decoded_bytes < working_cap
            && working_limit > working_cap) {
            working_limit = working_cap;
        }
        if (source_decoded > working_limit) {
            images->stats.skipped_limit++;
            fetch_result_destroy(fetched);
            return true;
        }
        /* A raster wider than the physical viewport cannot contribute more
           source samples to one presented row.  Retain its intrinsic source
           dimensions for layout, but make the deferred decoded target match
           the actual display.  Besides avoiding invisible excess work, this
           lets large responsive hero JPEGs decode within the PSP budget: the
           full source exists only while producing a viewport-sized target. */
        int layout_width = 0, layout_height = 0;
        bool layout_sized = context->eager_decode_rasters
            && image_layout_decode_target(
                pending, source_width, source_height,
                &layout_width, &layout_height);
        if (layout_sized) {
            width = layout_width;
            height = layout_height;
            decoded = (size_t) width * (size_t) height * 4u;
        } else if (context->eager_decode_rasters
            && context->viewport_width > 0
            && width > context->viewport_width) {
            height = tilefinch_mul_div_int(
                height, context->viewport_width, width);
            if (height < 1) height = 1;
            width = context->viewport_width;
            decoded = (size_t) width * (size_t) height * 4u;
        }
        while (decoded > decoded_limit && (width > 1 || height > 1)) {
            width = width > 1 ? (width + 1) / 2 : 1;
            height = height > 1 ? (height + 1) / 2 : 1;
            decoded = (size_t) width * (size_t) height * 4u;
        }
    }
    if (decoded > decoded_limit) {
        images->stats.skipped_limit++;
        fetch_result_destroy(fetched);
        return true;
    }
    size_t encoded_length = fetched->length;
    ImageResource resource = {.node = primary.node,
                              .url_hash = pending->url_hash,
                              .source_hash = primary.source_hash,
                              .pixels = pixels,
                              .source_width = source_width,
                              .source_height = source_height,
                              .width = width,
                              .height = height,
                              .is_mask = primary.is_mask,
                              .is_background = primary.is_background,
                              .pseudo = primary.pseudo,
                              .owns_pixels = is_svg};
    if (!is_svg) {
        resource.encoded = (unsigned char *) fetched->data;
        resource.encoded_body = fetched->shared_body;
        resource.encoded_length = fetched->length;
        resource.owns_encoded = true;
        fetched->shared_body = NULL;
        fetched->data = NULL;
        fetched->length = 0;
        fetched->capacity = 0;
        /* Priority resources are discovered before page script and the full
           resource walk inflate the shared realm.  Decode their bounded,
           viewport-sized target now while that temporary working window is
           available.  Keeping only the pixels also prevents a visible hero
           from repeatedly attempting an impossible late decode after the
           rest of the page has consumed the budget. */
        if (context->eager_decode_rasters || is_webp) {
            unsigned char *decoded_pixels = NULL;
            ImageDecodeStatus decode_status = image_resource_decode_checked(
                &resource, context->budget, &decoded_pixels);
            if (decode_status == IMAGE_DECODE_SUCCEEDED) {
                resource.pixels = decoded_pixels;
                resource.owns_pixels = true;
                BrowserSharedBody *pixel_body = browser_shared_body_take(
                    context->budget, decoded_pixels, decoded);
                if (pixel_body != NULL) {
                    resource.pixel_body = pixel_body;
                    if (context->session != NULL && pending->url != NULL) {
                        (void) browser_session_decoded_image_put(
                            context->session, pending->url,
                            &request_context, resource.encoded,
                            resource.encoded_length, pixel_body,
                            source_width, source_height, width, height);
                    }
                }
                if (resource.encoded_body != NULL) {
                    browser_shared_body_release(resource.encoded_body);
                } else {
                    budget_free(context->budget, resource.encoded);
                }
                resource.encoded = NULL;
                resource.encoded_body = NULL;
                resource.encoded_length = 0;
                resource.owns_encoded = false;
            } else if (is_webp) {
                /* WebP is always attempted in the cooperative resource
                   continuation. If that bounded attempt cannot be admitted,
                   keep the resource identity but release the body instead of
                   retrying the same expensive decode from every paint-cache
                   miss. A cancelled continuation rolls this suffix back. */
                if (resource.encoded_body != NULL) {
                    browser_shared_body_release(resource.encoded_body);
                } else {
                    budget_free(context->budget, resource.encoded);
                }
                resource.encoded = NULL;
                resource.encoded_body = NULL;
                resource.encoded_length = 0;
                resource.owns_encoded = false;
                images->stats.skipped_limit++;
            }
        }
    }
    if (!image_add(images, resource)) {
        image_resource_release_owned_pixels(context->budget, &resource);
        if (resource.encoded_body != NULL) {
            browser_shared_body_release(resource.encoded_body);
        } else {
            budget_free(context->budget, resource.encoded);
        }
        fetch_result_destroy(fetched);
        return false;
    }
    images->stats.loaded++;
    if (primary.is_mask) images->stats.masks_loaded++;
    if (primary.is_background) images->stats.backgrounds_loaded++;
    images->stats.encoded_bytes += encoded_length;
    if (is_svg) images->stats.decoded_bytes += decoded;
    if (!is_svg && resource.pixels != NULL) {
        images->stats.decoded_bytes += decoded;
    }
    if (!is_svg && (width != source_width || height != source_height)) {
        images->stats.downsampled++;
    }
    if (source_decoded > images->stats.largest_source_decode_bytes) {
        images->stats.largest_source_decode_bytes = source_decoded;
    }
    if (decoded > images->stats.largest_target_decode_bytes) {
        images->stats.largest_target_decode_bytes = decoded;
    }
    for (size_t i = 1; i < pending->target_count; i++) {
        PendingImageTarget target = pending->targets[i];
        ImageResource alias = resource;
        alias.node = target.node;
        alias.source_hash = target.source_hash;
        alias.is_mask = target.is_mask;
        alias.is_background = target.is_background;
        alias.pseudo = target.pseudo;
        alias.owns_pixels = false;
        alias.owns_encoded = false;
        images->stats.duplicate++;
        if (!image_add(images, alias)) {
            fetch_result_destroy(fetched);
            return false;
        }
    }
    fetch_result_destroy(fetched);
    return true;
}

static bool finish_one_pending(ImageLoadContext *context, bool wait)
{
    uint64_t scheduler_started = image_profile_enabled()
        ? image_profile_now_us() : 0;
    FetchResult *fetched = fetch_result_create(context->budget);
    if (fetched == NULL) return false;
    size_t idle_polls = 0;
    for (;;) {
        for (size_t i = 0; i < IMAGE_FETCH_CONCURRENCY; i++) {
            PendingImageFetch *pending = &context->pending[i];
            if (pending->request_id == 0) continue;
            bool success = false;
            if (!fetch_scheduler_take(context->scheduler,
                                      pending->request_id,
                                      &success, fetched)) {
                continue;
            }
            context->pending_count--;
            context->pending_reserved_bytes -= pending->reserved_bytes;
            image_note_request_finished(
                context, pending, fetched, success);
            if (scheduler_started != 0) {
                context->images->stats.scheduler_us +=
                    image_profile_now_us() - scheduler_started;
            }
            uint64_t finish_started = image_profile_enabled()
                ? image_profile_now_us() : 0;
            bool finished = finish_image_fetch(context, pending, fetched,
                                               success);
            if (finish_started != 0) {
                context->images->stats.finish_us +=
                    image_profile_now_us() - finish_started;
            }
            budget_free(context->budget, pending->targets);
            budget_free(context->budget, pending->url);
            memset(pending, 0, sizeof(*pending));
            fetch_result_free(fetched);
            /*
             * Decoding one response is the largest single unit this stage
             * owns, and the completion path above reaches it without passing
             * the poll's checkpoint. The final drain loop calls straight back
             * in, so with four requests in flight four decodes could run
             * back to back with nothing between them: measured under PPSSPP
             * that was a 454 ms stretch in which cancel could not be sampled.
             * Take the checkpoint per decode instead of per burst.
             */
            return finished && image_work(
                context, 1, true, "image-decode");
        }
        if (!wait) {
            if (scheduler_started != 0) {
                context->images->stats.scheduler_us +=
                    image_profile_now_us() - scheduler_started;
            }
            fetch_result_free(fetched);
            return true;
        }
        cancel_expired_pending(context);
        /*
         * This function runs inside the UI-facing resource continuation.
         * curl's poll timeout is a hard lower bound on cancellation and
         * presentation latency when no socket is ready.  A ten-millisecond
         * wait made an otherwise empty image unit five times larger than the
         * PSP's two-millisecond advisory budget.  Wait at most one millisecond,
         * admit one bounded body callback, and publish at most one completion
         * per continuation; the scheduler retains all in-flight transfers
         * between pumps, so this does not restart network work.
         */
        const FetchPumpQuota quota = {
            .maximum_body_callbacks = IMAGE_FETCH_PUMP_CALLBACKS,
            .maximum_body_bytes = IMAGE_FETCH_PUMP_BYTES,
            .maximum_time_us = IMAGE_FETCH_PUMP_TIME_US
        };
        size_t completed = fetch_scheduler_pump_bounded(
            context->scheduler, IMAGE_FETCH_PUMP_COMPLETIONS,
            IMAGE_FETCH_POLL_WAIT_MS, &quota, NULL);
        image_note_pending_progress(context);
        image_cancel_no_progress_pending(context);
        if (completed != 0) {
            idle_polls = 0;
        } else if (fetch_scheduler_uses_virtual_replay(context->scheduler)
                   && ++idle_polls
                        >= IMAGE_FETCH_MAX_REPLAY_IDLE_POLLS) {
            /* A live pump may wait on platform work, while deterministic
               replay advances immediately. Bound the virtual case so a
               request that only an external actor could cancel cannot spin
               until a wall-clock deadline; live timeout behavior remains
               unchanged. Completed requests retain their normal result path
               on the next pass. */
            context->deadline_cancelled = true;
            for (size_t i = 0; i < IMAGE_FETCH_CONCURRENCY; i++) {
                PendingImageFetch *pending = &context->pending[i];
                if (pending->request_id != 0) {
                    (void) fetch_scheduler_cancel(
                        context->scheduler, pending->request_id,
                        "image pipeline made no progress");
                }
            }
            idle_polls = 0;
        }
        if (!image_work(context, 1, true, "image-transport")) {
            fetch_result_free(fetched);
            return false;
        }
    }
}

static void cancel_pending(ImageLoadContext *context)
{
    for (size_t i = 0; i < IMAGE_FETCH_CONCURRENCY; i++) {
        PendingImageFetch *pending = &context->pending[i];
        if (pending->request_id != 0) {
            (void) fetch_scheduler_cancel(context->scheduler,
                                          pending->request_id,
                                          "image pipeline aborted");
            FetchResult *fetched = fetch_result_create(context->budget);
            bool success = false;
            if (fetched != NULL) {
                (void) fetch_scheduler_take(context->scheduler,
                                            pending->request_id,
                                            &success, fetched);
            }
            fetch_result_free(fetched);
        }
        budget_free(context->budget, pending->targets);
        budget_free(context->budget, pending->url);
        memset(pending, 0, sizeof(*pending));
    }
    context->pending_count = 0;
    context->pending_reserved_bytes = 0;
}

/* Many image CDNs expose the same asset through an extension-selected
   endpoint. Prefer a same-origin JPEG sibling when the URL makes that mapping
   explicit: the JPEG path has a smaller PSP decoder footprint, while signed
   or content-negotiated WebP URLs remain supported by the bounded decoder.
   This is deliberately narrower than arbitrary content negotiation: the path
   or one complete query value must end in .webp, and the substitution can
   only shrink it. */
static bool image_rewrite_webp_sibling(char url[4096])
{
    if (url == NULL) return false;
    char *suffix = NULL;
    for (char *cursor = url; *cursor != '\0' && *cursor != '#'; cursor++) {
        if (*cursor == '.' && strncasecmp(cursor, ".webp", 5) == 0
            && (cursor[5] == '\0' || cursor[5] == '?'
                || cursor[5] == '#' || cursor[5] == '&')) {
            suffix = cursor;
        }
    }
    if (suffix == NULL) return false;
    memcpy(suffix, ".jpg", 4);
    memmove(suffix + 4, suffix + 5, strlen(suffix + 5) + 1);
    return true;
}

static bool load_image_node_with_provenance_impl(
    ImageLoadContext *context, lxb_dom_node_t *node,
    const char *source, size_t source_length,
    const char *source_base_url, const char *referrer_source,
    const char *referrer_policy, bool is_mask, bool is_background,
    PseudoElement pseudo)
{
    ImageResources *images = context->images;
    uint64_t source_hash = source == NULL ? 0
        : image_hash_bytes(source, source_length);
    if (image_node_already_tracked(images, node, source_hash,
                                   is_mask, is_background,
                                   pseudo)) return true;
    images->stats.discovered++;
    bool data_source = source != NULL && source_length > 5
        && strncasecmp(source, "data:", 5) == 0;
    /* Network references are copied into a fixed-size resolver buffer below.
       Data URLs are decoded directly from the DOM string and have their own
       encoded-byte admission gate.  Keep a separate source-text ceiling so
       ordinary percent-encoded SVG icons larger than 2 KiB do not get
       rejected while hostile attributes remain bounded. */
    if (source == NULL || source_length == 0
        || (!data_source && source_length >= 2048)
        || (data_source && source_length > IMAGE_DATA_URL_SOURCE_LIMIT)
        || source_base_url == NULL || referrer_source == NULL) {
        images->stats.failed++;
        image_trace("invalid-source", source, source_length);
        return true;
    }
    if (data_source) {
        if (!tilefinch_csp_allows_request(
                &context->document->content_security_policy,
                TILEFINCH_DESTINATION_IMAGE, "data:")) {
            images->stats.failed++;
            return true;
        }
        if (images->stats.attempted >= context->maximum_count
            || images->stats.encoded_bytes
                   >= context->maximum_total_encoded_bytes
            || images->stats.decoded_bytes
                   >= context->maximum_decoded_bytes) {
            images->stats.skipped_limit++;
            return true;
        }
        uint64_t hash = image_hash_bytes(source, source_length);
        const ImageResource *duplicate = image_find_hash(images, hash);
        if (duplicate != NULL) {
            ImageResource alias = {
                .node = node,
                .url_hash = hash,
                .source_hash = source_hash,
                .pixels = duplicate->pixels,
                .pixel_body = duplicate->pixel_body,
                .encoded = duplicate->encoded,
                .encoded_body = duplicate->encoded_body,
                .encoded_length = duplicate->encoded_length,
                .source_width = duplicate->source_width,
                .source_height = duplicate->source_height,
                .width = duplicate->width,
                .height = duplicate->height,
                .is_mask = is_mask,
                .is_background = is_background,
                .pseudo = pseudo,
                .owns_pixels = false,
                .owns_encoded = false
            };
            images->stats.duplicate++;
            return image_add(images, alias);
        }
        size_t remaining = context->maximum_total_encoded_bytes
                           - images->stats.encoded_bytes;
        size_t maximum = context->maximum_single_encoded_bytes < remaining
                         ? context->maximum_single_encoded_bytes : remaining;
        unsigned char *body = NULL;
        size_t body_length = 0;
        char media_type[128] = {0};
        DataUrlDecodeResult decoded = data_url_decode(
            context->budget, source, source_length, maximum, &body,
            &body_length, media_type, sizeof(media_type));
        images->stats.attempted++;
        if (decoded != DATA_URL_DECODED) {
            if (decoded == DATA_URL_TOO_LARGE) {
                images->stats.skipped_limit++;
            } else if (decoded == DATA_URL_NO_MEMORY) {
                return false;
            } else {
                images->stats.unsupported++;
            }
            budget_free(context->budget, body);
            image_trace("data-uri-invalid", source, 40);
            return true;
        }
        PendingImageTarget target = {
            .node = node,
            .source_hash = source_hash,
            .display_width = context->current_display_width,
            .display_height = context->current_display_height,
            .is_mask = is_mask,
            .is_background = is_background,
            .pseudo = pseudo
        };
        PendingImageFetch pending = {
            .url_hash = hash,
            .targets = &target,
            .target_count = 1,
            .target_capacity = 1
        };
        FetchResult *fetched = fetch_result_create(context->budget);
        if (fetched == NULL) {
            budget_free(context->budget, body);
            return false;
        }
        fetched->data = (char *) body;
        fetched->length = body_length;
        fetched->capacity = body_length + 1;
        fetched->status_code = 200;
        snprintf(fetched->effective_url, sizeof(fetched->effective_url),
                 "data:");
        snprintf(fetched->content_type, sizeof(fetched->content_type), "%s",
                 media_type);
        image_trace("data-uri", source, 40);
        bool finished = finish_image_fetch(context, &pending, fetched, true);
        fetch_result_free(fetched);
        return finished;
    }
    uint64_t resolve_started = image_profile_enabled()
        ? image_profile_now_us() : 0;
    if (context->request_scratch == NULL) {
        context->request_scratch = budget_malloc(
            context->budget, sizeof(*context->request_scratch));
        if (context->request_scratch == NULL) return false;
    }
    char *reference = context->request_scratch->reference;
    memcpy(reference, source, source_length);
    reference[source_length] = '\0';
    char *resolved = context->request_scratch->resolved;
    if (!fetch_resolve_url(source_base_url, reference, resolved, 4096u)) {
        images->stats.failed++;
        return true;
    }
    if (image_rewrite_webp_sibling(resolved)) {
        images->stats.compatible_format_rewrites++;
    }
    if (!tilefinch_csp_allows_request(
            &context->document->content_security_policy,
            TILEFINCH_DESTINATION_IMAGE, resolved)) {
        images->stats.failed++;
        return true;
    }
    uint64_t hash = image_hash_request(
        resolved, referrer_source, referrer_policy);
    if (resolve_started != 0) {
        images->stats.admission_resolve_us +=
            image_profile_now_us() - resolve_started;
    }
    const ImageResource *duplicate = image_find_hash(images, hash);
    if (duplicate != NULL) {
        ImageResource alias = {.node = node, .url_hash = hash,
                               .source_hash = source_hash,
                               .pixels = duplicate->pixels,
                               .pixel_body = duplicate->pixel_body,
                               .encoded = duplicate->encoded,
                               .encoded_body = duplicate->encoded_body,
                               .encoded_length = duplicate->encoded_length,
                               .source_width = duplicate->source_width,
                               .source_height = duplicate->source_height,
                               .width = duplicate->width,
                               .height = duplicate->height,
                               .is_mask = is_mask,
                               .is_background = is_background,
                               .pseudo = pseudo,
                               .owns_pixels = false,
                               .owns_encoded = false};
        images->stats.duplicate++;
        return image_add(images, alias);
    }
    PendingImageFetch *same = pending_find_hash(context, hash);
    if (same != NULL) {
        return pending_add_target(context, same, node, source_hash, is_mask,
                                  is_background, pseudo);
    }
    if (image_origin_cooled(context, resolved)) {
        images->stats.skipped_limit++;
        images->stats.no_progress_origin_skipped++;
        image_trace("origin-cooled", resolved, strlen(resolved));
        return true;
    }
    if (image_stage_expired(context)) {
        images->stats.skipped_limit++;
        image_trace("stage-expired", resolved, strlen(resolved));
        return true;
    }
    if (images->stats.attempted >= context->maximum_count
        || images->stats.encoded_bytes >= context->maximum_total_encoded_bytes
        || images->stats.decoded_bytes >= context->maximum_decoded_bytes) {
        images->stats.skipped_limit++;
        image_trace("pre-limit", resolved, strlen(resolved));
        return true;
    }
    while (context->pending_count == IMAGE_FETCH_CONCURRENCY) {
        if (!finish_one_pending(context, true)) return false;
    }
    if (image_stage_expired(context)
        || images->stats.encoded_bytes >= context->maximum_total_encoded_bytes
        || images->stats.decoded_bytes >= context->maximum_decoded_bytes) {
        images->stats.skipped_limit++;
        return true;
    }
    size_t remaining = context->maximum_total_encoded_bytes
                       - images->stats.encoded_bytes;
    while (context->pending_count != 0
           && context->pending_reserved_bytes >= remaining) {
        if (!finish_one_pending(context, true)) return false;
        if (image_stage_expired(context)
            || images->stats.encoded_bytes
               >= context->maximum_total_encoded_bytes) {
            images->stats.skipped_limit++;
            return true;
        }
        remaining = context->maximum_total_encoded_bytes
                    - images->stats.encoded_bytes;
    }
    if (context->pending_reserved_bytes > remaining) return false;
    remaining -= context->pending_reserved_bytes;
    size_t maximum = context->maximum_single_encoded_bytes < remaining
                     ? context->maximum_single_encoded_bytes : remaining;
    /* A single shared sprite commonly carries mobile navigation chrome and
       can be moderately larger than an ordinary raster. In the realistic
       PSP profile, admit one such transient body up to a fixed 768 KiB cap;
       the traversal separately prevents later symbols from repeating it.
       Strict/low-memory profiles retain the caller's ordinary image cap. */
    if (inline_svg_external_use_href(node, NULL) != NULL
        && context->budget->limit >= 24u * 1024u * 1024u
        && maximum < EXTERNAL_SVG_SPRITE_FETCH_LIMIT) {
        maximum = remaining < EXTERNAL_SVG_SPRITE_FETCH_LIMIT
                  ? remaining : EXTERNAL_SVG_SPRITE_FETCH_LIMIT;
    }
    if (maximum == 0) {
        images->stats.skipped_limit++;
        return true;
    }
    PendingImageFetch *pending = pending_find_free(context);
    if (pending == NULL) return false;
    if (!pending_add_target(context, pending, node, source_hash, is_mask,
                            is_background, pseudo)) return false;
    size_t resolved_length = strlen(resolved);
    pending->url = budget_malloc(context->budget, resolved_length + 1);
    if (pending->url == NULL) {
        budget_free(context->budget, pending->targets);
        memset(pending, 0, sizeof(*pending));
        return false;
    }
    memcpy(pending->url, resolved, resolved_length + 1);
    pending->url_hash = hash;
    pending->reserved_bytes = maximum;
    images->stats.attempted++;
    uint64_t cache_started = image_profile_enabled()
        ? image_profile_now_us() : 0;
    const BrowserCacheEntry *cached = NULL;
    TilefinchRequestContext request_context = image_request_context(
        context, resolved);
    BrowserCacheStatus cache_status = context->session == NULL
        || content_blocker_would_block(
               context->session->content_blocker, resolved,
               context->document_url, "image", "no-cors")
        ? BROWSER_CACHE_MISS : browser_session_cache_match_resource(
            context->session, resolved, &request_context,
            tilefinch_platform_monotonic_time_ns(), &cached);
    /* The image pipeline has no conditional-request machinery: a stale
       entry is refetched from scratch.  Under hermetic replay that
       refetch can only fail once the original record is claimed (a
       resource-driven style rebuild would silently lose the image), and
       replay time is logical anyway, so a stale body is authoritative
       there.  Live fetches keep ordinary freshness semantics. */
    bool cache_usable = cached != NULL
        && (cache_status == BROWSER_CACHE_FRESH
            || (cache_status == BROWSER_CACHE_STALE
                && fetch_trace_replay_active()));
    if (cache_started != 0) {
        images->stats.admission_cache_us +=
            image_profile_now_us() - cache_started;
    }
    if (cache_usable && cached->length <= maximum) {
        ImageDecodedCacheResult decoded_cache = image_adopt_decoded_cache(
            context, pending, cached, &request_context);
        if (decoded_cache != IMAGE_DECODED_CACHE_MISS) {
            budget_free(context->budget, pending->targets);
            budget_free(context->budget, pending->url);
            memset(pending, 0, sizeof(*pending));
            return decoded_cache == IMAGE_DECODED_CACHE_USED;
        }
        pending->from_resource_cache = true;
        pending->resource_grant_valid = cached->resource_grant_valid;
        pending->resource_grant = cached->resource_grant;
        FetchResult *cached_result = fetch_result_create(context->budget);
        if (cached_result == NULL) {
            budget_free(context->budget, pending->targets);
            budget_free(context->budget, pending->url);
            memset(pending, 0, sizeof(*pending));
            return false;
        }
        cached_result->status_code = 200;
        if (cached->body != NULL) {
            cached_result->shared_body = browser_shared_body_retain(
                cached->body);
            if (cached_result->shared_body == NULL) {
                fetch_result_free(cached_result);
                budget_free(context->budget, pending->targets);
                budget_free(context->budget, pending->url);
                memset(pending, 0, sizeof(*pending));
                return false;
            }
            cached_result->data = (char *) cached->body->data;
            cached_result->capacity = 0;
        } else {
            cached_result->data = budget_malloc(context->budget,
                                                cached->length + 1);
            if (cached_result->data == NULL) {
                fetch_result_free(cached_result);
                budget_free(context->budget, pending->targets);
                budget_free(context->budget, pending->url);
                memset(pending, 0, sizeof(*pending));
                return false;
            }
            memcpy(cached_result->data, cached->data, cached->length);
            cached_result->data[cached->length] = '\0';
            cached_result->capacity = cached->length + 1;
        }
        cached_result->length = cached->length;
        snprintf(cached_result->content_type,
                 sizeof(cached_result->content_type), "%s",
                 cached->content_type);
        images->stats.cache_hits++;
        char *cached_url = pending->url;
        pending->url = NULL;
        bool finished = finish_image_fetch(
            context, pending, cached_result, true);
        fetch_result_free(cached_result);
        pending->url = cached_url;
        budget_free(context->budget, pending->targets);
        budget_free(context->budget, pending->url);
        memset(pending, 0, sizeof(*pending));
        return finished;
    }
    uint64_t context_started = image_profile_enabled()
        ? image_profile_now_us() : 0;
    FetchRequest transport = {
        .send_low_client_hints = true,
        /* Advertise only formats the decoders actually handle. */
#if defined(TILEFINCH_DISABLE_GIF)
        .accept = "image/png,image/jpeg,image/webp,image/svg+xml,"
                  "image/*;q=0.8,"
                  "*/*;q=0.5",
#else
        .accept = "image/png,image/jpeg,image/gif,image/webp,image/svg+xml,"
                  "image/*;q=0.8,*/*;q=0.5",
#endif
    };
    if (!fetch_prepare_page_request_context(
            &request_context, referrer_source, referrer_policy,
            context->session, &context->document->content_security_policy,
            NULL, &transport, &context->request_scratch->prepared, NULL)) {
        images->stats.failed++;
        budget_free(context->budget, pending->targets);
        budget_free(context->budget, pending->url);
        memset(pending, 0, sizeof(*pending));
        return true;
    }
    const FetchRequest *request = fetch_prepared_page_request(
        &context->request_scratch->prepared);
    if (request == NULL) return true;
    if (context_started != 0) {
        images->stats.admission_context_us +=
            image_profile_now_us() - context_started;
    }
    image_trace("enqueue", resolved, strlen(resolved));
    uint64_t enqueue_started = image_profile_enabled()
        ? image_profile_now_us() : 0;
    pending->request_id = fetch_scheduler_enqueue(
        context->scheduler, resolved, request, maximum, context->timeout_ms);
    if (enqueue_started != 0) {
        images->stats.admission_enqueue_us +=
            image_profile_now_us() - enqueue_started;
    }
    if (pending->request_id == 0) {
        images->stats.failed++;
        image_trace("enqueue-failed", resolved, strlen(resolved));
        if (getenv("TILEFINCH_TRACE_IMAGES") != NULL) {
            fprintf(stderr, "tilefinch: image enqueue error=%s\n",
                    fetch_scheduler_last_error(context->scheduler));
        }
        budget_free(context->budget, pending->targets);
        budget_free(context->budget, pending->url);
        memset(pending, 0, sizeof(*pending));
        return true;
    }
    pending->started_ms = image_now_ms();
    pending->last_progress_ms = pending->started_ms;
    context->pending_count++;
    context->pending_reserved_bytes += maximum;
    return finish_one_pending(context, false);
}

static bool load_image_node_with_provenance(
    ImageLoadContext *context, lxb_dom_node_t *node,
    const char *source, size_t source_length,
    const char *source_base_url, const char *referrer_source,
    const char *referrer_policy, bool is_mask, bool is_background,
    PseudoElement pseudo)
{
    uint64_t started = image_profile_enabled()
        ? image_profile_now_us() : 0;
    bool loaded = load_image_node_with_provenance_impl(
        context, node, source, source_length, source_base_url,
        referrer_source, referrer_policy, is_mask, is_background, pseudo);
    if (started != 0) {
        context->images->stats.admission_us +=
            image_profile_now_us() - started;
    }
    return loaded;
}

static bool load_document_image_node(
    ImageLoadContext *context, lxb_dom_node_t *node,
    const char *source, size_t source_length, bool is_mask,
    bool is_background, PseudoElement pseudo)
{
    return load_image_node_with_provenance(
        context, node, source, source_length,
        context->base_url, context->document_url, context->referrer_policy,
        is_mask, is_background, pseudo);
}

static bool load_stylesheet_image_node(
    ImageLoadContext *context, lxb_dom_node_t *node,
    const char *source, size_t source_length, bool is_mask,
    bool is_background, PseudoElement pseudo)
{
    const char *sheet_base = NULL;
    const char *sheet_policy = NULL;
    if (!stylesheet_image_url_source(
            context->stylesheet, source, &sheet_base, &sheet_policy)) {
        /* A CSS pointer without retained provenance is an internal or quota
           failure. Drop it rather than silently reinterpret it relative to
           the document and potentially leak a Referer. */
        context->images->stats.discovered++;
        context->images->stats.failed++;
        return true;
    }
    const char *base = sheet_base == NULL ? context->base_url : sheet_base;
    const char *referrer_source = sheet_base == NULL
        ? context->document_url : sheet_base;
    const char *policy = sheet_base == NULL
        ? context->referrer_policy : sheet_policy;
    return load_image_node_with_provenance(
        context, node, source, source_length, base, referrer_source, policy,
        is_mask, is_background, pseudo);
}

typedef struct {
    lxb_dom_node_t *node;
    ComputedStyle parent;
    bool has_parent;
} ImageTraversalEntry;

static bool image_traversal_push(
    ImageLoadContext *context, ImageTraversalEntry **entries,
    size_t *count, size_t *capacity, lxb_dom_node_t *node,
    const ComputedStyle *parent)
{
    if (node == NULL) return true;
    if (*count == IMAGE_TRAVERSAL_PENDING_LIMIT) return false;
    if (*count == *capacity) {
        size_t next = *capacity == 0 ? 8u : *capacity * 2u;
        if (next > IMAGE_TRAVERSAL_PENDING_LIMIT) {
            next = IMAGE_TRAVERSAL_PENDING_LIMIT;
        }
        if (next <= *capacity
            || next > SIZE_MAX / sizeof(**entries)) return false;
        ImageTraversalEntry *grown = budget_realloc(
            context->budget, *entries, next * sizeof(**entries));
        if (grown == NULL) return false;
        *entries = grown;
        *capacity = next;
    }
    ImageTraversalEntry *entry = &(*entries)[(*count)++];
    entry->node = node;
    entry->has_parent = parent != NULL;
    if (parent != NULL) entry->parent = *parent;
    return true;
}

static bool image_process_node(
    ImageLoadContext *context, lxb_dom_node_t *node,
    const ComputedStyle *parent, ComputedStyle *style, bool *traverse)
{
    *traverse = true;
    if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) return true;
    uint64_t style_started = image_profile_enabled()
        ? image_profile_now_us() : 0;
    bool style_cache_hit = false;
    if (context->style_cache != NULL) {
        style_cache_hit = layout_reuse_cache_resolve_style(
            context->style_cache, context->stylesheet, context->fonts,
            node, parent, style);
        if (style_cache_hit) context->images->stats.node_style_cache_hits++;
        else context->images->stats.node_style_cache_misses++;
    } else {
        *style = style_for_node(context->stylesheet, node, parent);
    }
    if (style_started != 0) {
        uint64_t elapsed = image_profile_now_us() - style_started;
        context->images->stats.style_resolve_us += elapsed;
        context->images->stats.node_style_resolve_us += elapsed;
    }
    *traverse = style->display != DISPLAY_NONE && !style->hidden;
    bool atomic_inline_svg = *traverse && image_name_is(node, "svg");
    if (*traverse && image_name_is(node, "img")
        && (!context->preserve_staged_document_images
            || images_find_node(context->images, node) == NULL)) {
        size_t source_length = 0;
        const char *source = image_select_source(
            context->stylesheet, node, &source_length);
        if (!load_document_image_node(context, node, source, source_length,
                                      false, false, PSEUDO_NONE)) return false;
    }
    if (*traverse && image_name_is(node, "video")) {
        size_t poster_length = 0;
        const char *poster = document_attribute(
            node, "poster", &poster_length);
        if (poster != NULL && poster_length != 0
            && !load_document_image_node(
                context, node, poster, poster_length,
                false, false, PSEUDO_NONE)) return false;
    }
    if (atomic_inline_svg) {
        size_t external_length = 0;
        const char *external = inline_svg_external_use_href(
            node, &external_length);
        if (external != NULL) {
            if (context->external_svg_sprite_attempts != 0) {
                context->images->stats.skipped_limit++;
            } else {
                context->external_svg_sprite_attempts++;
                if (!load_document_image_node(
                        context, node, external, external_length,
                        false, false, PSEUDO_NONE)) return false;
            }
        } else if (!load_inline_svg(context, node, style)) {
            return false;
        }
    }
    /* The scalar fields retain the common one-layer fast path. Multi-layer
       values live in the bounded paint stack and must keep distinct resource
       identities even though they belong to the same DOM node. */
    const StylePaintStack *paint = stylesheet_paint_stack(
        context->stylesheet, computed_style_paint_stack_id(style));
    bool layered_background = paint != NULL
        && (paint->components & STYLE_PAINT_COMPONENT_BACKGROUND_IMAGE) != 0;
    bool layered_mask = paint != NULL
        && (paint->components & STYLE_PAINT_COMPONENT_MASK_IMAGE) != 0;
    if (*traverse && layered_background) {
        for (size_t i = 0; i < paint->background_count; i++) {
            const StylePaintLayer *layer = &paint->backgrounds[i];
            if (layer->kind == STYLE_PAINT_IMAGE_URL
                && layer->image != NULL && layer->image[0] != '\0'
                && !load_stylesheet_image_node(
                       context, node, layer->image, strlen(layer->image),
                       false, true, PSEUDO_NONE)) return false;
        }
    } else if (*traverse
               && style->background_image_kind
                  == STYLE_BACKGROUND_IMAGE_URL
               && style->background_image != NULL
               && style->background_image[0] != '\0'
               && !load_stylesheet_image_node(
                      context, node, style->background_image,
                      strlen(style->background_image), false, true,
                      PSEUDO_NONE)) return false;
    if (*traverse && layered_mask) {
        for (size_t i = 0; i < paint->mask_count; i++) {
            const StylePaintLayer *layer = &paint->masks[i];
            if (layer->kind == STYLE_PAINT_IMAGE_URL
                && layer->image != NULL && layer->image[0] != '\0'
                && !load_stylesheet_image_node(
                       context, node, layer->image, strlen(layer->image),
                       true, false, PSEUDO_NONE)) return false;
        }
    } else if (*traverse && style->mask_image != NULL
               && style->mask_image[0] != '\0'
               && !load_stylesheet_image_node(
                      context, node, style->mask_image,
                      strlen(style->mask_image), true, false,
                      PSEUDO_NONE)) return false;
    if (!*traverse) return true;
    for (PseudoElement pseudo = PSEUDO_BEFORE;
         pseudo <= PSEUDO_AFTER; pseudo++) {
        style_started = image_profile_enabled()
            ? image_profile_now_us() : 0;
        ComputedStyle generated = style_for_pseudo(
            context->stylesheet, node, pseudo, style);
        if (style_started != 0) {
            uint64_t elapsed = image_profile_now_us() - style_started;
            context->images->stats.style_resolve_us += elapsed;
            context->images->stats.pseudo_style_resolve_us += elapsed;
        }
        const StylePaintStack *generated_paint = stylesheet_paint_stack(
            context->stylesheet,
            computed_style_paint_stack_id(&generated));
        bool generated_layered_background = generated_paint != NULL
            && (generated_paint->components
                & STYLE_PAINT_COMPONENT_BACKGROUND_IMAGE) != 0;
        bool generated_layered_mask = generated_paint != NULL
            && (generated_paint->components
                & STYLE_PAINT_COMPONENT_MASK_IMAGE) != 0;
        if (generated.generated_content && generated_layered_background) {
            for (size_t i = 0; i < generated_paint->background_count; i++) {
                const StylePaintLayer *layer =
                    &generated_paint->backgrounds[i];
                if (layer->kind == STYLE_PAINT_IMAGE_URL
                    && layer->image != NULL && layer->image[0] != '\0'
                    && !load_stylesheet_image_node(
                           context, node, layer->image,
                           strlen(layer->image), false, true, pseudo)) {
                    return false;
                }
            }
        } else if (generated.generated_content
                   && generated.background_image_kind
                      == STYLE_BACKGROUND_IMAGE_URL
                   && generated.background_image != NULL
                   && generated.background_image[0] != '\0'
                   && !load_stylesheet_image_node(
                          context, node, generated.background_image,
                          strlen(generated.background_image), false,
                          true, pseudo)) return false;
        if (generated.generated_content && generated_layered_mask) {
            for (size_t i = 0; i < generated_paint->mask_count; i++) {
                const StylePaintLayer *layer = &generated_paint->masks[i];
                if (layer->kind == STYLE_PAINT_IMAGE_URL
                    && layer->image != NULL && layer->image[0] != '\0'
                    && !load_stylesheet_image_node(
                           context, node, layer->image,
                           strlen(layer->image), true, false, pseudo)) {
                    return false;
                }
            }
        } else if (generated.generated_content
                   && generated.mask_image != NULL
                   && generated.mask_image[0] != '\0'
                   && !load_stylesheet_image_node(
                          context, node, generated.mask_image,
                          strlen(generated.mask_image), true, false,
                          pseudo)) return false;
    }
    /* Inline SVG is decoded as one atomic replaced image. Its serializer has
       already resolved the retained presentation cascade for the complete
       subtree; walking those descendants again can discover no independently
       paintable HTML resource and would repeat every computed-style pass. */
    if (atomic_inline_svg) *traverse = false;
    return true;
}

static bool image_process_refresh_node(
    ImageLoadContext *context, lxb_dom_node_t *node);

static bool image_process_priority_node(
    ImageLoadContext *context, lxb_dom_node_t *node)
{
    /* A provisional layout proves the element is visible, but it does not
       retain the exact inherited ComputedStyle chain needed to resolve CSS
       backgrounds and generated content authoritatively. Prioritize only
       HTML image sources here; the complete traversal below retains normal
       document order and quota semantics for every style-derived resource. */
    if (node == NULL || node->type != LXB_DOM_NODE_TYPE_ELEMENT) return true;
    if (image_name_is(node, "svg")) {
        /* A provisional layout has already proved that this exact SVG is
           visible. Resolve its bounded ancestor chain so compact responsive
           icons are rasterized before large markup images consume the PSP's
           decoded-image budget. */
        return image_process_refresh_node(context, node);
    }
    if (!image_name_is(node, "img") && !image_name_is(node, "video")) {
        return true;
    }
    size_t source_length = 0;
    const char *source = image_name_is(node, "video")
        ? document_attribute(node, "poster", &source_length)
        : image_select_source(context->stylesheet, node, &source_length);
    return load_document_image_node(
        context, node, source, source_length, false, false, PSEUDO_NONE);
}

static bool image_process_refresh_node(
    ImageLoadContext *context, lxb_dom_node_t *node)
{
    enum { MAXIMUM_ANCESTORS = 64 };
    if (context == NULL || node == NULL) return false;
    lxb_dom_node_t *ancestors[MAXIMUM_ANCESTORS];
    size_t count = 0;
    for (lxb_dom_node_t *at = node->parent; at != NULL; at = at->parent) {
        if (at->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        if (count == MAXIMUM_ANCESTORS) return false;
        ancestors[count++] = at;
    }
    ComputedStyle parent = {0};
    bool has_parent = false;
    while (count != 0) {
        parent = style_for_node(
            context->stylesheet, ancestors[--count],
            has_parent ? &parent : NULL);
        has_parent = true;
        /* A complete traversal would never reach a descendant of a hidden
           element. Refreshing one exact resource must retain that rule. */
        if (parent.display == DISPLAY_NONE || parent.hidden) return true;
    }
    ComputedStyle style = {0};
    bool traverse = true;
    return image_process_node(
        context, node, has_parent ? &parent : NULL, &style, &traverse);
}

static bool image_process_priority_target(
    ImageLoadContext *context, const ImagePriorityTarget *target)
{
    if (context == NULL || target == NULL || target->node == NULL) {
        return false;
    }
    if (target->kind == IMAGE_PRIORITY_KIND_DOCUMENT) {
        uint16_t display_width = target->display_width;
        uint16_t display_height = target->display_height;
        size_t source_length = 0;
        const char *source = image_name_is(target->node, "img")
            ? image_select_source_for_width(
                context->stylesheet, target->node, display_width,
                &source_length)
            : NULL;
        /* A shared response must satisfy every alias. Resolve the complete
           bounded priority set before the first request so traversal order
           cannot leave a later, larger box backed by an undersized bitmap. */
        if (source != NULL && source_length != 0) {
            for (size_t i = 0; i < context->priority_target_count; i++) {
                const ImagePriorityTarget *other =
                    &context->priority_targets[i];
                if (other->kind != IMAGE_PRIORITY_KIND_DOCUMENT
                    || !image_name_is(other->node, "img")) continue;
                size_t other_length = 0;
                const char *other_source = image_select_source_for_width(
                    context->stylesheet, other->node, other->display_width,
                    &other_length);
                if (other_source == NULL || other_length != source_length
                    || memcmp(other_source, source, source_length) != 0) {
                    continue;
                }
                if (other->display_width == 0
                    || other->display_height == 0) {
                    display_width = 0;
                    display_height = 0;
                    break;
                }
                if (other->display_width > display_width) {
                    display_width = other->display_width;
                }
                if (other->display_height > display_height) {
                    display_height = other->display_height;
                }
            }
        }
        context->current_display_width = display_width;
        context->current_display_height = display_height;
        bool loaded;
        if (source != NULL) {
            loaded = load_document_image_node(
                context, target->node, source, source_length,
                false, false, PSEUDO_NONE);
        } else {
            loaded = image_process_priority_node(context, target->node);
        }
        context->current_display_width = 0;
        context->current_display_height = 0;
        return loaded;
    }
    bool is_mask = target->kind == IMAGE_PRIORITY_KIND_MASK;
    bool is_background =
        target->kind == IMAGE_PRIORITY_KIND_BACKGROUND;
    if ((!is_mask && !is_background) || target->source == NULL
        || target->source[0] == '\0'
        || target->pseudo > (uint8_t) PSEUDO_AFTER) return false;
    return load_stylesheet_image_node(
        context, target->node, target->source, strlen(target->source),
        is_mask, is_background, (PseudoElement) target->pseudo);
}

static bool walk_images(ImageLoadContext *context, lxb_dom_node_t *node,
                        const ComputedStyle *parent)
{
    ImageTraversalEntry *entries = NULL;
    size_t count = 0, capacity = 0, visited = 0;
    bool ok = image_traversal_push(
        context, &entries, &count, &capacity, node, parent);
    while (ok && count != 0) {
        ImageTraversalEntry entry = entries[--count];
        node = entry.node;
        if (++visited > IMAGE_TRAVERSAL_NODE_LIMIT) {
            ok = false;
            break;
        }
        const ComputedStyle *node_parent = entry.has_parent
            ? &entry.parent : NULL;
        ComputedStyle style;
        bool traverse = true;
        if (!image_work(context, 1, false, "resource")
            || !image_process_node(
                context, node, node_parent, &style, &traverse)) {
            ok = false;
            break;
        }
        const ComputedStyle *child_parent =
            node->type == LXB_DOM_NODE_TYPE_ELEMENT ? &style : node_parent;
        /* Push the sibling first so the child remains the next item in
           document order. Each pending branch owns the exact inherited style
           it needs, so no C recursion or fixed stack-sized style array is
           required. */
        ok = image_traversal_push(
            context, &entries, &count, &capacity, node->next, node_parent);
        if (ok && traverse) {
            ok = image_traversal_push(
                context, &entries, &count, &capacity, node->first_child,
                child_parent);
        }
    }
    budget_free(context->budget, entries);
    return ok;
}

static void images_rollback_optional_suffix(
    ImageResources *images, size_t retained_count,
    const ExternalImageStats *retained_stats, bool retained_priority_staged)
{
    if (images == NULL || images->budget == NULL || retained_stats == NULL
        || retained_count > images->count) return;
    for (size_t i = retained_count; i < images->count; i++) {
        ImageResource *item = &images->items[i];
        image_resource_release_owned_pixels(images->budget, item);
        if (item->owns_encoded) {
            if (item->encoded_body != NULL) {
                browser_shared_body_release(item->encoded_body);
            } else {
                budget_free(images->budget, item->encoded);
            }
        }
    }
    images->count = retained_count;
    images->stats = *retained_stats;
    images->stats.priority_retained_on_failure++;
    images->priority_staged = retained_priority_staged;
}

static bool images_load_external_impl(
    const PocDocument *document, Stylesheet *stylesheet,
    ImageResources *images, lxb_dom_node_t *const *priority_nodes,
    size_t priority_node_count, const ImagePriorityTarget *priority_targets,
    size_t priority_target_count, bool full_traversal,
    bool refresh_complete_nodes, Budget *budget,
    const char *base_url, const char *document_url,
    const char *referrer_policy, size_t maximum_count,
    size_t maximum_total_encoded_bytes,
    size_t maximum_single_encoded_bytes, size_t maximum_decoded_bytes,
    long timeout_ms, FetchScheduler *scheduler, BrowserSession *session,
    LayoutReuseCache *style_cache, const FontSet *fonts, int viewport_width)
{
    if (document == NULL || document->html == NULL || stylesheet == NULL
        || images == NULL
        || budget == NULL || base_url == NULL || document_url == NULL
        || maximum_count == 0
        || maximum_total_encoded_bytes == 0
        || maximum_single_encoded_bytes == 0 || maximum_decoded_bytes == 0
        || timeout_ms <= 0 || decode_budget != NULL) return false;
    /* A caller may request more images than the engine can track (a
       reference-scoring profile can raise the count to fit a busy 106-image
       page).  Exceeding a limit must soft-skip, never fail the
       navigation: the per-image budgets already skip past their ceilings, so
       clamp the request down to the bounded node table rather than returning
       false.  MAX_TRACKED_IMAGE_NODES stays the hard structural cap the PSP
       device profiles depend on. */
    if (maximum_count > MAX_TRACKED_IMAGE_NODES) {
        maximum_count = MAX_TRACKED_IMAGE_NODES;
    }
    layout_reuse_cache_prepare(
        style_cache, stylesheet, fonts, images, viewport_width);
    bool owns_scheduler = scheduler == NULL;
    if (owns_scheduler) {
        size_t reserved = maximum_single_encoded_bytes;
        if (reserved <= SIZE_MAX / IMAGE_FETCH_CONCURRENCY) {
            reserved *= IMAGE_FETCH_CONCURRENCY;
        } else {
            reserved = SIZE_MAX;
        }
        scheduler = fetch_scheduler_create(
            budget, IMAGE_FETCH_CONCURRENCY, reserved);
        if (scheduler == NULL) return false;
    }
    if (images->budget == NULL) {
        memset(images, 0, sizeof(*images));
        images->budget = budget;
    } else if (images->budget != budget) {
        if (owns_scheduler) fetch_scheduler_destroy(scheduler);
        return false;
    }
    size_t retained_count = images->count;
    ExternalImageStats retained_stats = images->stats;
    bool retained_priority_staged = images->priority_staged;
    if (!images->priority_staged) {
        images->stats.max_slice_us = 0;
        images->stats.work_units = 0;
        images->stats.max_slice_work_units = 0;
        images->stats.cooperative_yields = 0;
    }
    double started_ms = image_now_ms();
    ImageLoadContext context = {
        .document = document,
        .images = images,
        .stylesheet = stylesheet,
        .budget = budget,
        .base_url = base_url,
        .document_url = document_url,
        .referrer_policy = referrer_policy,
        .maximum_count = maximum_count,
        .maximum_total_encoded_bytes = maximum_total_encoded_bytes,
        .maximum_single_encoded_bytes = maximum_single_encoded_bytes,
        .maximum_decoded_bytes = maximum_decoded_bytes,
        .timeout_ms = timeout_ms,
        .scheduler = scheduler,
        .session = session,
        .style_cache = style_cache,
        .fonts = fonts,
        .viewport_width = viewport_width > 0
            ? viewport_width : stylesheet->viewport_width,
        .priority_targets = priority_targets,
        .priority_target_count = priority_target_count,
        .preserve_staged_document_images =
            full_traversal && images->priority_staged,
        .deadline_ms = image_now_ms() + (double) timeout_ms,
        .slice_started_us = tilefinch_platform_monotonic_time_us(),
        .eager_decode_rasters = !full_traversal
            && (priority_node_count != 0 || priority_target_count != 0)
    };
    uint64_t traversal_started = image_profile_enabled()
        ? image_profile_now_us() : 0;
    bool traversed = true;
    if (full_traversal) {
        /* Match layout's exact html/body inheritance roots. Starting at the
           document node gives every descendant a different parent hash and
           makes otherwise identical computed styles unsafe to hand off. */
        lxb_dom_node_t *body = document_body_node(document);
        lxb_dom_node_t *html = body == NULL ? NULL : body->parent;
        ComputedStyle root = layout_initial_root_style();
        ComputedStyle html_style = root;
        bool html_traverse = true;
        if (html != NULL && html->type == LXB_DOM_NODE_TYPE_ELEMENT
            && image_name_is(html, "html")) {
            traversed = image_process_node(
                &context, html, &root, &html_style, &html_traverse);
        }
        if (traversed && html_traverse) {
            traversed = walk_images(&context, body, &html_style);
        }
    } else {
        for (size_t i = 0; traversed && i < priority_node_count; i++) {
            lxb_dom_node_t *node = priority_nodes[i];
            if (node == NULL
                || !image_work(&context, 1, false, "resource")) {
                traversed = node == NULL;
                continue;
            }
            traversed = refresh_complete_nodes
                ? image_process_refresh_node(&context, node)
                : image_process_priority_node(&context, node);
        }
        for (size_t i = 0; traversed && i < priority_target_count; i++) {
            if (!image_work(&context, 1, false, "resource")) {
                traversed = false;
                break;
            }
            traversed = image_process_priority_target(
                &context, &priority_targets[i]);
        }
    }
    if (!traversed) {
        cancel_pending(&context);
        if (retained_count != 0) {
            images_rollback_optional_suffix(
                images, retained_count, &retained_stats,
                retained_priority_staged);
        } else {
            images_destroy(images);
        }
        budget_free(budget, context.request_scratch);
        if (owns_scheduler) fetch_scheduler_destroy(scheduler);
        return false;
    }
    if (traversal_started != 0) {
        images->stats.traversal_us +=
            image_profile_now_us() - traversal_started;
    }
    uint64_t drain_started = image_profile_enabled()
        ? image_profile_now_us() : 0;
    while (context.pending_count != 0) {
        if (!finish_one_pending(&context, true)) {
            cancel_pending(&context);
            if (retained_count != 0) {
                images_rollback_optional_suffix(
                    images, retained_count, &retained_stats,
                    retained_priority_staged);
            } else {
                images_destroy(images);
            }
            if (owns_scheduler) fetch_scheduler_destroy(scheduler);
            budget_free(budget, context.request_scratch);
            return false;
        }
    }
    if (drain_started != 0) {
        images->stats.drain_us += image_profile_now_us() - drain_started;
    }
    image_finish_slice(&context);
    images->stats.deadline_cancelled = context.deadline_cancelled ? 1u : 0u;
    images->stats.deadline_exceeded = image_now_ms() >= context.deadline_ms;
    double elapsed = image_now_ms() - started_ms;
    if (elapsed < 0.0) elapsed = 0.0;
    images->stats.elapsed_ms += (uint64_t) elapsed;
    budget_free(budget, context.request_scratch);
    if (owns_scheduler) fetch_scheduler_destroy(scheduler);
    return true;
}

bool images_load_external(const PocDocument *document, Stylesheet *stylesheet,
                          ImageResources *images,
                          Budget *budget, const char *base_url,
                          const char *document_url,
                          const char *referrer_policy,
                          size_t maximum_count,
                          size_t maximum_total_encoded_bytes,
                          size_t maximum_single_encoded_bytes,
                          size_t maximum_decoded_bytes,
                          long timeout_ms, FetchScheduler *scheduler,
                          BrowserSession *session)
{
    bool loaded = images_load_external_impl(
        document, stylesheet, images, NULL, 0, NULL, 0, true, false,
        budget, base_url, document_url, referrer_policy, maximum_count,
        maximum_total_encoded_bytes, maximum_single_encoded_bytes,
        maximum_decoded_bytes, timeout_ms, scheduler, session,
        NULL, NULL, 0);
    if (images != NULL) images->priority_staged = false;
    return loaded;
}

bool images_load_external_reusing_layout_styles(
    const PocDocument *document, Stylesheet *stylesheet,
    ImageResources *images, Budget *budget, const char *base_url,
    const char *document_url, const char *referrer_policy,
    size_t maximum_count, size_t maximum_total_encoded_bytes,
    size_t maximum_single_encoded_bytes, size_t maximum_decoded_bytes,
    long timeout_ms, FetchScheduler *scheduler, BrowserSession *session,
    LayoutReuseCache *style_cache, const FontSet *fonts, int viewport_width)
{
    bool loaded = images_load_external_impl(
        document, stylesheet, images, NULL, 0, NULL, 0, true, false,
        budget, base_url, document_url, referrer_policy, maximum_count,
        maximum_total_encoded_bytes, maximum_single_encoded_bytes,
        maximum_decoded_bytes, timeout_ms, scheduler, session,
        style_cache, fonts, viewport_width);
    if (images != NULL) images->priority_staged = false;
    return loaded;
}

bool images_load_external_priority_nodes(
    const PocDocument *document, Stylesheet *stylesheet,
    ImageResources *images, lxb_dom_node_t *const *nodes, size_t node_count,
    Budget *budget, const char *base_url, const char *document_url,
    const char *referrer_policy, size_t maximum_count,
    size_t maximum_total_encoded_bytes, size_t maximum_single_encoded_bytes,
    size_t maximum_decoded_bytes, long timeout_ms, FetchScheduler *scheduler,
    BrowserSession *session)
{
    if (node_count == 0) return true;
    if (nodes == NULL) return false;
    bool loaded = images_load_external_impl(
        document, stylesheet, images, nodes, node_count, NULL, 0,
        false, false, budget, base_url, document_url, referrer_policy,
        maximum_count,
        maximum_total_encoded_bytes, maximum_single_encoded_bytes,
        maximum_decoded_bytes, timeout_ms, scheduler, session,
        NULL, NULL, 0);
    if (loaded && images != NULL) images->priority_staged = true;
    return loaded;
}

static bool image_node_in_refresh_set(
    const lxb_dom_node_t *node, lxb_dom_node_t *const *nodes,
    size_t node_count)
{
    for (size_t i = 0; i < node_count; i++) {
        if (nodes[i] == node) return true;
    }
    return false;
}

static size_t image_refresh_owned_encoded_bytes(
    const ImageResources *images, lxb_dom_node_t *const *nodes,
    size_t node_count)
{
    size_t bytes = 0;
    for (size_t i = 0; i < images->count; i++) {
        const ImageResource *item = &images->items[i];
        if (!item->owns_encoded
            || !image_node_in_refresh_set(
                item->node, nodes, node_count)) continue;
        bool retained_alias = false;
        for (size_t alias = 0; alias < images->count; alias++) {
            const ImageResource *candidate = &images->items[alias];
            if (alias != i
                && !image_node_in_refresh_set(
                    candidate->node, nodes, node_count)
                && candidate->encoded == item->encoded
                && candidate->encoded_body == item->encoded_body) {
                retained_alias = true;
                break;
            }
        }
        if (!retained_alias && item->encoded_length <= SIZE_MAX - bytes) {
            bytes += item->encoded_length;
        }
    }
    return bytes;
}

static size_t image_refresh_owned_decoded_bytes(
    const ImageResources *images, lxb_dom_node_t *const *nodes,
    size_t node_count)
{
    size_t bytes = 0;
    for (size_t i = 0; i < images->count; i++) {
        const ImageResource *item = &images->items[i];
        if (!item->owns_pixels
            || !image_node_in_refresh_set(item->node, nodes, node_count)
            || item->width <= 0 || item->height <= 0
            || (size_t) item->width > SIZE_MAX / (size_t) item->height
            || (size_t) item->width * (size_t) item->height
                   > SIZE_MAX / 4u) continue;
        bool retained_alias = false;
        for (size_t alias = 0; alias < images->count; alias++) {
            const ImageResource *candidate = &images->items[alias];
            if (alias != i
                && !image_node_in_refresh_set(
                    candidate->node, nodes, node_count)
                && candidate->pixels == item->pixels) {
                retained_alias = true;
                break;
            }
        }
        if (retained_alias) continue;
        size_t item_bytes =
            (size_t) item->width * (size_t) item->height * 4u;
        if (item_bytes <= SIZE_MAX - bytes) bytes += item_bytes;
    }
    return bytes;
}

static void image_refresh_transfer_old_ownership(
    ImageResources *images, size_t owner_index,
    lxb_dom_node_t *const *nodes, size_t node_count)
{
    ImageResource *owner = &images->items[owner_index];
    for (size_t i = 0; i < images->count; i++) {
        ImageResource *candidate = &images->items[i];
        if (i == owner_index
            || image_node_in_refresh_set(
                   candidate->node, nodes, node_count)) continue;
        if (owner->owns_pixels && candidate->pixels == owner->pixels) {
            candidate->owns_pixels = true;
            owner->owns_pixels = false;
        }
        if (owner->owns_encoded
            && candidate->encoded == owner->encoded
            && candidate->encoded_body == owner->encoded_body) {
            candidate->owns_encoded = true;
            owner->owns_encoded = false;
        }
        if (!owner->owns_pixels && !owner->owns_encoded) break;
    }
}

bool images_refresh_external_nodes(
    const PocDocument *document, Stylesheet *stylesheet,
    ImageResources *images, lxb_dom_node_t *const *nodes, size_t node_count,
    Budget *budget, const char *base_url, const char *document_url,
    const char *referrer_policy, size_t maximum_count,
    size_t maximum_total_encoded_bytes,
    size_t maximum_single_encoded_bytes, size_t maximum_decoded_bytes,
    long timeout_ms, FetchScheduler *scheduler, BrowserSession *session)
{
    if (node_count == 0) return true;
    if (document == NULL || stylesheet == NULL || images == NULL
        || nodes == NULL || budget == NULL || images->budget != budget) {
        return false;
    }
    size_t retired_encoded = image_refresh_owned_encoded_bytes(
        images, nodes, node_count);
    size_t retired_decoded = image_refresh_owned_decoded_bytes(
        images, nodes, node_count);
    ImageResources replacement = {
        .budget = budget,
        .stats = images->stats,
        .priority_staged = true
    };
    replacement.stats.encoded_bytes =
        retired_encoded <= replacement.stats.encoded_bytes
        ? replacement.stats.encoded_bytes - retired_encoded : 0;
    replacement.stats.decoded_bytes =
        retired_decoded <= replacement.stats.decoded_bytes
        ? replacement.stats.decoded_bytes - retired_decoded : 0;
    if (!images_load_external_impl(
            document, stylesheet, &replacement, nodes, node_count, NULL, 0,
            false, true, budget, base_url, document_url, referrer_policy,
            maximum_count, maximum_total_encoded_bytes,
            maximum_single_encoded_bytes, maximum_decoded_bytes, timeout_ms,
            scheduler, session, NULL, NULL, 0)) {
        images_destroy(&replacement);
        return false;
    }

    size_t retained_count = 0;
    for (size_t i = 0; i < images->count; i++) {
        if (!image_node_in_refresh_set(
                images->items[i].node, nodes, node_count)) retained_count++;
    }
    if (replacement.count > MAX_TRACKED_IMAGE_NODES - retained_count) {
        images_destroy(&replacement);
        return false;
    }
    size_t wanted = retained_count + replacement.count;
    if (wanted > images->capacity) {
        size_t capacity = images->capacity == 0 ? 16 : images->capacity;
        while (capacity < wanted && capacity < MAX_TRACKED_IMAGE_NODES) {
            size_t grown = capacity <= MAX_TRACKED_IMAGE_NODES / 2u
                ? capacity * 2u : MAX_TRACKED_IMAGE_NODES;
            if (grown <= capacity) break;
            capacity = grown;
        }
        ImageResource *grown = budget_realloc(
            budget, images->items, capacity * sizeof(*grown));
        if (grown == NULL) {
            images_destroy(&replacement);
            return false;
        }
        images->items = grown;
        images->capacity = capacity;
    }

    for (size_t i = 0; i < images->count; i++) {
        if (image_node_in_refresh_set(
                images->items[i].node, nodes, node_count)) {
            image_refresh_transfer_old_ownership(
                images, i, nodes, node_count);
        }
    }
    size_t write = 0;
    for (size_t i = 0; i < images->count; i++) {
        ImageResource item = images->items[i];
        if (image_node_in_refresh_set(item.node, nodes, node_count)) {
            image_resource_release_owned_pixels(budget, &item);
            if (item.owns_encoded) {
                if (item.encoded_body != NULL) {
                    browser_shared_body_release(item.encoded_body);
                } else {
                    budget_free(budget, item.encoded);
                }
            }
            continue;
        }
        images->items[write++] = item;
    }
    if (replacement.count != 0) {
        memcpy(images->items + write, replacement.items,
               replacement.count * sizeof(*replacement.items));
        write += replacement.count;
    }
    images->count = write;
    images->stats = replacement.stats;
    images->priority_staged = false;
    budget_free(budget, replacement.items);
    replacement.items = NULL;
    replacement.count = replacement.capacity = 0;
    return true;
}

bool images_load_external_priority_targets(
    const PocDocument *document, Stylesheet *stylesheet,
    ImageResources *images, const ImagePriorityTarget *targets,
    size_t target_count, Budget *budget, const char *base_url,
    const char *document_url, const char *referrer_policy,
    size_t maximum_count, size_t maximum_total_encoded_bytes,
    size_t maximum_single_encoded_bytes, size_t maximum_decoded_bytes,
    long timeout_ms, FetchScheduler *scheduler, BrowserSession *session)
{
    if (target_count == 0) return true;
    if (targets == NULL) return false;
    bool loaded = images_load_external_impl(
        document, stylesheet, images, NULL, 0, targets, target_count,
        false, false, budget, base_url, document_url, referrer_policy,
        maximum_count,
        maximum_total_encoded_bytes, maximum_single_encoded_bytes,
        maximum_decoded_bytes, timeout_ms, scheduler, session,
        NULL, NULL, 0);
    if (loaded && images != NULL) images->priority_staged = true;
    return loaded;
}

static ImagePriorityLoadStatus image_priority_load_fail(
    ImagePriorityLoadJob *job)
{
    if (job == NULL) return IMAGE_PRIORITY_LOAD_FAILED;
    cancel_pending(&job->context);
    if (!job->rolled_back) {
        images_rollback_optional_suffix(
            job->context.images, job->retained_count,
            &job->retained_stats, job->retained_priority_staged);
        job->rolled_back = true;
    }
    job->terminal = true;
    job->failed = true;
    return IMAGE_PRIORITY_LOAD_FAILED;
}

ImagePriorityLoadJob *images_priority_load_begin(
    const PocDocument *document, Stylesheet *stylesheet,
    ImageResources *images, const ImagePriorityTarget *target,
    Budget *budget, const char *base_url, const char *document_url,
    const char *referrer_policy, size_t maximum_count,
    size_t maximum_total_encoded_bytes,
    size_t maximum_single_encoded_bytes, size_t maximum_decoded_bytes,
    long timeout_ms, FetchScheduler *scheduler, BrowserSession *session)
{
    if (document == NULL || document->html == NULL || stylesheet == NULL
        || images == NULL || target == NULL || target->node == NULL
        || target->kind != IMAGE_PRIORITY_KIND_DOCUMENT
        || budget == NULL || base_url == NULL || document_url == NULL
        || maximum_count == 0 || maximum_total_encoded_bytes == 0
        || maximum_single_encoded_bytes == 0 || maximum_decoded_bytes == 0
        || timeout_ms <= 0 || scheduler == NULL || decode_budget != NULL) {
        return NULL;
    }
    if (maximum_count > MAX_TRACKED_IMAGE_NODES) {
        maximum_count = MAX_TRACKED_IMAGE_NODES;
    }
    if (images->budget == NULL) {
        memset(images, 0, sizeof(*images));
        images->budget = budget;
    } else if (images->budget != budget) {
        return NULL;
    }
    ImagePriorityLoadJob *job = budget_calloc(budget, 1, sizeof(*job));
    if (job == NULL) return NULL;
    job->target = *target;
    job->retained_count = images->count;
    job->retained_stats = images->stats;
    job->retained_priority_staged = images->priority_staged;
    job->started_ms = image_now_ms();
    job->context = (ImageLoadContext) {
        .document = document,
        .images = images,
        .stylesheet = stylesheet,
        .budget = budget,
        .base_url = base_url,
        .document_url = document_url,
        .referrer_policy = referrer_policy,
        .maximum_count = maximum_count,
        .maximum_total_encoded_bytes = maximum_total_encoded_bytes,
        .maximum_single_encoded_bytes = maximum_single_encoded_bytes,
        .maximum_decoded_bytes = maximum_decoded_bytes,
        .timeout_ms = timeout_ms,
        .scheduler = scheduler,
        .session = session,
        .viewport_width = stylesheet->viewport_width,
        .priority_targets = &job->target,
        .priority_target_count = 1,
        .deadline_ms = 0.0,
        .slice_started_us = tilefinch_platform_monotonic_time_us(),
        .eager_decode_rasters = true,
        .externally_pumped = true
    };
    return job;
}

ImagePriorityLoadStatus images_priority_load_pump(
    ImagePriorityLoadJob *job)
{
    if (job == NULL || job->failed) return IMAGE_PRIORITY_LOAD_FAILED;
    if (job->terminal) return IMAGE_PRIORITY_LOAD_COMPLETE;
    ImageLoadContext *context = &job->context;
    if (!job->admitted) {
        if (fetch_scheduler_enqueue_would_block(
                context->scheduler,
                context->maximum_single_encoded_bytes)) {
            if (image_now_ms() - job->started_ms
                >= (double) context->timeout_ms) {
                return image_priority_load_fail(job);
            }
            image_finish_slice(context);
            return IMAGE_PRIORITY_LOAD_PENDING;
        }
        job->admitted = true;
        job->started_ms = image_now_ms();
        context->deadline_ms =
            job->started_ms + (double) context->timeout_ms;
        if (!image_work(context, 1, false, "deferred-image-admission")
            || !image_process_priority_target(context, &job->target)) {
            return image_priority_load_fail(job);
        }
        if (context->pending_count == 0) {
            image_finish_slice(context);
            context->images->priority_staged = true;
            job->terminal = true;
            return IMAGE_PRIORITY_LOAD_COMPLETE;
        }
        /* Admission is one idle unit. Even an immediately available replay
           or cache-backed transport result is consumed on the next call. */
        image_finish_slice(context);
        return IMAGE_PRIORITY_LOAD_PENDING;
    }

    cancel_expired_pending(context);
    const FetchPumpQuota quota = {
        .maximum_body_callbacks = IMAGE_FETCH_PUMP_CALLBACKS,
        .maximum_body_bytes = IMAGE_FETCH_PUMP_BYTES,
        .maximum_time_us = IMAGE_FETCH_PUMP_TIME_US
    };
    size_t completed = fetch_scheduler_pump_bounded(
        context->scheduler, IMAGE_FETCH_PUMP_COMPLETIONS, 0,
        &quota, NULL);
    image_note_pending_progress(context);
    image_cancel_no_progress_pending(context);
    if (completed == 0) {
        image_finish_slice(context);
        return IMAGE_PRIORITY_LOAD_PENDING;
    }
    if (!finish_one_pending(context, false)) {
        return image_priority_load_fail(job);
    }
    if (context->pending_count != 0) {
        image_finish_slice(context);
        return IMAGE_PRIORITY_LOAD_PENDING;
    }
    context->images->stats.deadline_cancelled =
        context->deadline_cancelled ? 1u : 0u;
    context->images->stats.deadline_exceeded =
        image_now_ms() >= context->deadline_ms;
    double elapsed = image_now_ms() - job->started_ms;
    if (elapsed > 0.0) {
        context->images->stats.elapsed_ms += (uint64_t) elapsed;
    }
    context->images->priority_staged = true;
    job->terminal = true;
    return IMAGE_PRIORITY_LOAD_COMPLETE;
}

void images_priority_load_destroy(ImagePriorityLoadJob *job)
{
    if (job == NULL) return;
    if (!job->terminal) (void) image_priority_load_fail(job);
    budget_free(job->context.budget, job->context.request_scratch);
    budget_free(job->context.budget, job);
}

bool image_resource_available(const ImageResource *image)
{
    return image != NULL && (image->pixels != NULL
                             || (image->encoded != NULL
                                 && image->encoded_length != 0));
}

const void *image_resource_backing_identity(const ImageResource *image)
{
    if (image == NULL) return NULL;
    if (image->pixels != NULL) return image->pixels;
    if (image->encoded_body != NULL) return image->encoded_body;
    return image->encoded;
}

ImageDecodeStatus image_resource_decode_checked(
    const ImageResource *image, Budget *budget, unsigned char **decoded)
{
    if (decoded == NULL) return IMAGE_DECODE_DETERMINISTIC_FAILURE;
    *decoded = NULL;
    if (image == NULL || budget == NULL || image->encoded == NULL
        || image->encoded_length == 0
        || image->encoded_length > INT32_MAX) {
        return IMAGE_DECODE_DETERMINISTIC_FAILURE;
    }
    if (decode_budget != NULL) return IMAGE_DECODE_TRANSIENT_FAILURE;
    int width = 0, height = 0, components = 0;
    size_t failures_before = budget->failure_count;
    decode_budget = budget;
    bool scaled_jpeg = image->encoded_length >= 2u
        && image->encoded[0] == 0xffu && image->encoded[1] == 0xd8u
        && image->source_width > image->width
        && image->source_height > image->height;
    bool webp = image_is_webp(image->encoded, image->encoded_length);
    bool webp_interrupted = false;
    unsigned char *pixels = webp
        ? image_decode_webp_scaled(
              image->encoded, image->encoded_length,
              image->width, image->height,
              &width, &height, &components, &webp_interrupted)
        : scaled_jpeg
        ? image_decode_jpeg_scaled(
              image->encoded, (int) image->encoded_length,
              image->width, image->height, &width, &height, &components)
        : stbi_load_from_memory(
              image->encoded, (int) image->encoded_length,
              &width, &height, &components, 4);
    decode_budget = NULL;
    int source_width = image->source_width > 0
                       ? image->source_width : image->width;
    int source_height = image->source_height > 0
                        ? image->source_height : image->height;
    if (pixels == NULL) {
        return webp_interrupted || budget->failure_count != failures_before
            ? IMAGE_DECODE_TRANSIENT_FAILURE
            : IMAGE_DECODE_DETERMINISTIC_FAILURE;
    }
    if (width != source_width || height != source_height) {
        image_resource_free_decoded(budget, pixels);
        return IMAGE_DECODE_DETERMINISTIC_FAILURE;
    }
    if (scaled_jpeg || webp) {
        *decoded = pixels;
        return IMAGE_DECODE_SUCCEEDED;
    }
    if (width == image->width && height == image->height) {
        *decoded = pixels;
        return IMAGE_DECODE_SUCCEEDED;
    }
    if (image->width <= 0 || image->height <= 0
        || (size_t) image->width > SIZE_MAX / (size_t) image->height
        || (size_t) image->width * (size_t) image->height > SIZE_MAX / 4u) {
        image_resource_free_decoded(budget, pixels);
        return IMAGE_DECODE_DETERMINISTIC_FAILURE;
    }
    size_t target_bytes = (size_t) image->width * (size_t) image->height * 4u;
    unsigned char *target = budget_malloc(budget, target_bytes);
    if (target == NULL) {
        image_resource_free_decoded(budget, pixels);
        return IMAGE_DECODE_TRANSIENT_FAILURE;
    }
    for (int y = 0; y < image->height; y++) {
        int source_y = (int) ((int64_t) y * height / image->height);
        for (int x = 0; x < image->width; x++) {
            int source_x = (int) ((int64_t) x * width / image->width);
            memcpy(target + ((size_t) y * image->width + x) * 4u,
                   pixels + ((size_t) source_y * width + source_x) * 4u,
                   4u);
        }
    }
    image_resource_free_decoded(budget, pixels);
    *decoded = target;
    return IMAGE_DECODE_SUCCEEDED;
}

unsigned char *image_resource_decode(const ImageResource *image,
                                     Budget *budget)
{
    unsigned char *pixels = NULL;
    return image_resource_decode_checked(image, budget, &pixels)
               == IMAGE_DECODE_SUCCEEDED
        ? pixels : NULL;
}

void image_resource_free_decoded(Budget *budget, unsigned char *pixels)
{
    if (budget == NULL || pixels == NULL) return;
    budget_free(budget, pixels);
}

const ImageResource *images_find_node(const ImageResources *images,
                                      const lxb_dom_node_t *node)
{
    if (images == NULL || node == NULL) return NULL;
    for (size_t i = 0; i < images->count; i++) {
        if (images->items[i].node == node && !images->items[i].is_mask
            && !images->items[i].is_background) {
            return &images->items[i];
        }
    }
    return NULL;
}

const ImageResource *images_find_background_node(const ImageResources *images,
                                                 const lxb_dom_node_t *node)
{
    if (images == NULL || node == NULL) return NULL;
    for (size_t i = 0; i < images->count; i++) {
        if (images->items[i].node == node
            && images->items[i].is_background
            && images->items[i].pseudo == PSEUDO_NONE) {
            return &images->items[i];
        }
    }
    return NULL;
}

static const ImageResource *images_find_paint_source(
    const ImageResources *images, const lxb_dom_node_t *node,
    const char *source, PseudoElement pseudo, bool mask)
{
    if (images == NULL || node == NULL || source == NULL
        || source[0] == '\0') return NULL;
    uint64_t source_hash = image_hash_bytes(source, strlen(source));
    const ImageResource *legacy = NULL;
    for (size_t i = 0; i < images->count; i++) {
        const ImageResource *item = &images->items[i];
        if (item->node != node || item->is_mask != mask
            || item->is_background != !mask
            || item->pseudo != pseudo) continue;
        if (item->source_hash == source_hash) return item;
        /* Adopted/test resources predate authored-source identity. Preserve
           their one-resource-per-role behavior without letting such an
           entry alias two real layered loads. */
        if (item->source_hash == 0 && legacy == NULL) legacy = item;
    }
    return legacy;
}

const ImageResource *images_find_background_source(
    const ImageResources *images, const lxb_dom_node_t *node,
    const char *source, PseudoElement pseudo)
{
    return images_find_paint_source(images, node, source, pseudo, false);
}

const ImageResource *images_find_mask_source(
    const ImageResources *images, const lxb_dom_node_t *node,
    const char *source, PseudoElement pseudo)
{
    return images_find_paint_source(images, node, source, pseudo, true);
}

const ImageResource *images_find_mask_node(const ImageResources *images,
                                           const lxb_dom_node_t *node)
{
    if (images == NULL || node == NULL) return NULL;
    for (size_t i = 0; i < images->count; i++) {
        if (images->items[i].node == node && images->items[i].is_mask
            && images->items[i].pseudo == PSEUDO_NONE) {
            return &images->items[i];
        }
    }
    return NULL;
}

const ImageResource *images_find_pseudo_mask(const ImageResources *images,
                                             const lxb_dom_node_t *node,
                                             PseudoElement pseudo)
{
    if (images == NULL || node == NULL || pseudo == PSEUDO_NONE) return NULL;
    for (size_t i = 0; i < images->count; i++) {
        if (images->items[i].node == node && images->items[i].is_mask
            && images->items[i].pseudo == pseudo) return &images->items[i];
    }
    return NULL;
}

const ImageResource *images_find_pseudo_background(
    const ImageResources *images, const lxb_dom_node_t *node,
    PseudoElement pseudo)
{
    if (images == NULL || node == NULL || pseudo == PSEUDO_NONE) return NULL;
    for (size_t i = 0; i < images->count; i++) {
        if (images->items[i].node == node
            && images->items[i].is_background
            && images->items[i].pseudo == pseudo) return &images->items[i];
    }
    return NULL;
}

void images_destroy(ImageResources *images)
{
    if (images == NULL) return;
    if (images->budget != NULL) {
        for (size_t i = 0; i < images->count; i++) {
            image_resource_release_owned_pixels(
                images->budget, &images->items[i]);
            if (images->items[i].owns_encoded) {
                if (images->items[i].encoded_body != NULL) {
                    browser_shared_body_release(
                        images->items[i].encoded_body);
                } else {
                    budget_free(images->budget, images->items[i].encoded);
                }
            }
        }
        budget_free(images->budget, images->items);
    }
    memset(images, 0, sizeof(*images));
}
