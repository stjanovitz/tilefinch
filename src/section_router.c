#include "tilefinch/section_router.h"

#include <ctype.h>
#include <stdint.h>
#include <string.h>

#include "tilefinch/platform.h"

#define ROUTE_MIN_BYTES (512u * 1024u)
#define ROUTE_MAX_BYTES (1536u * 1024u)

static bool route_name_is(const char *name, size_t length,
                          const char *wanted)
{
    size_t wanted_length = strlen(wanted);
    return length == wanted_length
        && memcmp(name, wanted, wanted_length) == 0;
}

static bool route_name_byte(unsigned char byte)
{
    return isalnum(byte) || byte == '-' || byte == ':' || byte == '_';
}

static bool route_void_element(const char *name, size_t length)
{
    static const char *const names[] = {
        "area", "base", "br", "col", "embed", "hr", "img", "input",
        "link", "meta", "param", "source", "track", "wbr"
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (route_name_is(name, length, names[i])) return true;
    }
    return false;
}

static bool route_raw_text_element(const char *name, size_t length)
{
    return route_name_is(name, length, "script")
        || route_name_is(name, length, "style")
        || route_name_is(name, length, "xmp")
        || route_name_is(name, length, "iframe")
        || route_name_is(name, length, "noembed")
        || route_name_is(name, length, "noframes")
        || route_name_is(name, length, "textarea")
        || route_name_is(name, length, "title")
        || route_name_is(name, length, "plaintext");
}

static void route_begin_tag(SectionRouteStream *router)
{
    router->in_tag = true;
    router->tag_first_pending = true;
    router->tag_name_complete = false;
    router->closing_tag = false;
    router->declaration_tag = false;
    router->quote = '\0';
    router->tag_name_length = 0;
    router->tag_prefix_length = 0;
    router->tag_name_overflow = false;
}

static bool route_stack_push(SectionRouteStream *router)
{
    if (router->tag_name_overflow
        || router->depth >= sizeof(router->open_tag_lengths)) {
        router->safety_disabled = true;
    } else {
        size_t index = router->depth;
        memcpy(router->open_tags[index], router->tag_name,
               router->tag_name_length);
        router->open_tag_lengths[index] = router->tag_name_length;
    }
    if (router->depth == SIZE_MAX) {
        router->safety_disabled = true;
        return false;
    }
    router->depth++;
    if (router->depth > router->maximum_depth) {
        router->maximum_depth = router->depth;
    }
    return true;
}

static bool route_stack_pop_matching(SectionRouteStream *router)
{
    if (router->depth == 0 || router->tag_name_overflow) return false;
    if (router->depth > sizeof(router->open_tag_lengths)) {
        /* The exact lexical stack is no longer representable. Safety was
           already disabled when it overflowed; keep only approximate depth
           metrics until EOF. */
        router->depth--;
        return false;
    }
    size_t index = router->depth - 1;
    if (router->open_tag_lengths[index] != router->tag_name_length
        || memcmp(router->open_tags[index], router->tag_name,
                  router->tag_name_length) != 0) return false;
    router->open_tag_lengths[index] = 0;
    router->depth--;
    return true;
}

size_t section_route_stream_threshold(const Budget *budget)
{
    size_t threshold = budget == NULL ? ROUTE_MIN_BYTES : budget->limit / 20u;
    if (threshold < ROUTE_MIN_BYTES) threshold = ROUTE_MIN_BYTES;
    if (threshold > ROUTE_MAX_BYTES) threshold = ROUTE_MAX_BYTES;
    return threshold;
}

void section_route_stream_scan(SectionRouteStream *router,
                               const unsigned char *data, size_t length)
{
    uint64_t started = tilefinch_platform_monotonic_time_us();
    for (size_t i = 0; i < length; i++) {
        unsigned char byte = data[i];
        size_t absolute_end = router->scanned_bytes + i + 1;
        if (router->in_comment) {
            if (byte == '-') {
                if (router->comment_dashes < 2) router->comment_dashes++;
            } else if (byte == '>' && router->comment_dashes >= 2) {
                router->in_comment = false;
                router->comment_dashes = 0;
            } else {
                router->comment_dashes = 0;
            }
            continue;
        }
        if (!router->in_tag) {
            if (byte != '<') continue;
            route_begin_tag(router);
            continue;
        }

        if (router->tag_prefix_length < sizeof(router->tag_prefix)) {
            router->tag_prefix[router->tag_prefix_length++] = (char) byte;
            if (router->raw_name_length == 0
                && router->tag_prefix_length == sizeof(router->tag_prefix)
                && memcmp(router->tag_prefix, "!--", 3) == 0) {
                router->in_tag = false;
                router->in_comment = true;
                router->comment_dashes = 0;
                continue;
            }
        }

        /* In an HTML raw-text/RCDATA element, only a closing-tag opener can
           leave raw mode. A less-than operator or markup-looking JS/CSS text
           must not contribute fake elements, attributes, or nesting. */
        if (router->raw_name_length != 0
            && router->tag_first_pending && !router->closing_tag) {
            if (byte != '/') {
                router->in_tag = false;
                if (byte == '<') route_begin_tag(router);
                continue;
            }
            if (route_name_is(router->raw_name,
                              router->raw_name_length, "plaintext")) {
                router->in_tag = false;
                continue;
            }
            router->closing_tag = true;
            continue;
        }

        if (router->tag_first_pending) {
            if (isspace(byte)) continue;
            if (byte == '/' && !router->closing_tag) {
                router->closing_tag = true;
                continue;
            }
            router->tag_first_pending = false;
            if (byte == '!' || byte == '?') {
                router->declaration_tag = true;
                router->tag_name_complete = true;
            } else if (route_name_byte(byte)) {
                router->tag_name[router->tag_name_length++] =
                    (char) tolower(byte);
            } else {
                router->tag_name_complete = true;
            }
        } else if (!router->tag_name_complete) {
            if (route_name_byte(byte)) {
                if (router->tag_name_length < sizeof(router->tag_name)) {
                    router->tag_name[router->tag_name_length++] =
                        (char) tolower(byte);
                } else {
                    router->tag_name_overflow = true;
                }
            } else {
                router->tag_name_complete = true;
            }
        }
        if (router->quote != '\0') {
            if (byte == (unsigned char) router->quote) router->quote = '\0';
            continue;
        }
        if (byte == '\'' || byte == '"') {
            router->quote = (char) byte;
        } else if (byte == '=' && router->raw_name_length == 0
                   && !router->declaration_tag) {
            router->attribute_count++;
        } else if (byte == '>') {
            bool raw_candidate = router->raw_name_length != 0;
            if (raw_candidate) {
                if (router->closing_tag
                    && route_name_is(
                           router->tag_name, router->tag_name_length,
                           router->raw_name)) {
                    size_t depth_before = router->depth;
                    bool matched = route_stack_pop_matching(router);
                    if (matched && !router->safety_disabled
                        && router->body_seen
                        && depth_before == router->body_depth + 1
                        && router->depth == router->body_depth) {
                        router->safe_boundary = absolute_end;
                    }
                    if (matched) {
                        router->raw_name_length = 0;
                        router->raw_name[0] = '\0';
                    }
                }
            } else if (!router->declaration_tag
                       && router->tag_name_length != 0) {
                if (router->closing_tag) {
                    size_t depth_before = router->depth;
                    bool matched = route_stack_pop_matching(router);
                    if (matched && !router->safety_disabled
                        && router->body_seen
                        && !route_name_is(
                            router->tag_name, router->tag_name_length,
                            "body")
                        && depth_before == router->body_depth + 1
                        && router->depth == router->body_depth) {
                        router->safe_boundary = absolute_end;
                    }
                } else {
                    size_t depth_before = router->depth;
                    bool is_void = route_void_element(
                        router->tag_name, router->tag_name_length);
                    router->element_count++;
                    /* In HTML syntax a slash on a non-void start tag does not
                       close it. Treat only actual void elements as complete;
                       foreign-content self-closing tags are conservatively
                       left unavailable until EOF. */
                    if (!is_void && !route_stack_push(router)) {
                        router->in_tag = false;
                        router->scanned_bytes += length;
                        return;
                    }
                    if (route_name_is(
                            router->tag_name, router->tag_name_length,
                            "body") && !is_void) {
                        router->body_seen = true;
                        router->body_depth = router->depth;
                        router->prefix_length = absolute_end;
                        router->sealed_boundary = absolute_end;
                    } else if (!router->safety_disabled
                               && router->body_seen
                               && depth_before == router->body_depth
                               && is_void) {
                        router->safe_boundary = absolute_end;
                    }
                    if (!is_void
                        && route_raw_text_element(
                               router->tag_name,
                               router->tag_name_length)) {
                        memcpy(router->raw_name, router->tag_name,
                               router->tag_name_length);
                        router->raw_name[router->tag_name_length] = '\0';
                        router->raw_name_length = router->tag_name_length;
                    }
                }
            }
            router->in_tag = false;
        }
    }
    router->scanned_bytes += length;
    router->scan_us += tilefinch_platform_monotonic_time_us() - started;
}

bool section_route_stream_prefers_sections(
    const SectionRouteStream *router, size_t prospective_length)
{
    if (prospective_length > router->byte_threshold) return true;
    size_t estimate = prospective_length;
    if (router->element_count > (SIZE_MAX - estimate) / 160u) return true;
    estimate += router->element_count * 160u;
    if (router->attribute_count > (SIZE_MAX - estimate) / 64u) return true;
    estimate += router->attribute_count * 64u;
    if (router->maximum_depth > (SIZE_MAX - estimate) / 1024u) return true;
    estimate += router->maximum_depth * 1024u;
    return router->budget != NULL && estimate > router->budget->limit / 4u;
}

bool section_route_stream_begin(SectionRouteStream *router,
                                SectionStoreStreamBuilder *builder,
                                CompressedSectionStore *store,
                                Budget *budget, size_t block_bytes,
                                size_t maximum_section_bytes,
                                bool force_sections)
{
    if (router == NULL || builder == NULL || store == NULL || budget == NULL) {
        return false;
    }
    memset(router, 0, sizeof(*router));
    router->builder = builder;
    router->store = store;
    router->budget = budget;
    router->byte_threshold = section_route_stream_threshold(budget);
    router->block_bytes = block_bytes;
    router->maximum_section_bytes = maximum_section_bytes;
    router->active = true;
    if (!force_sections) return true;
    router->sectioned = section_store_stream_begin(
        builder, store, budget, block_bytes, maximum_section_bytes);
    return router->sectioned;
}

static bool section_route_stream_switch(SectionRouteStream *router)
{
    if (router->sectioned) return true;
    uint64_t started = tilefinch_platform_monotonic_time_us();
    if (!section_store_stream_begin(
            router->builder, router->store, router->budget,
            router->block_bytes, router->maximum_section_bytes)) {
        return false;
    }
    section_store_stream_set_progress(
        router->builder, router->on_progress, router->progress_opaque);
    if (!section_store_stream_append(
               router->builder, router->buffer, router->length)
    ) {
        return false;
    }
    budget_free(router->budget, router->buffer);
    router->buffer = NULL;
    router->capacity = 0;
    router->sectioned = true;
    router->replay_us += tilefinch_platform_monotonic_time_us() - started;
    return true;
}

static bool section_route_stream_seal_safe(SectionRouteStream *router)
{
    if (!router->sectioned || !router->body_seen
        || router->safety_disabled
        || router->prefix_length == 0
        || router->safe_boundary <= router->sealed_boundary) return true;
    size_t length = router->safe_boundary - router->sealed_boundary;
    size_t minimum = router->block_bytes / 4u;
    if (minimum < 1024) minimum = 1024;
    if (length < minimum) return true;
    /* A direct child larger than the configured section ceiling has no safe
       bounded pre-EOF split. Leave it unadvertised; the complete structural
       index may later split it with reconstructed context. */
    if (length > router->maximum_section_bytes) return true;
    if (!section_store_stream_seal_section(
            router->builder, router->prefix_length,
            router->sealed_boundary, router->safe_boundary)) return false;
    router->sealed_boundary = router->safe_boundary;
    return true;
}

bool section_route_stream_body(void *opaque, const unsigned char *data,
                               size_t length)
{
    SectionRouteStream *router = opaque;
    if (router == NULL || !router->active
        || (data == NULL && length != 0)) return false;
    if (router->sectioned) {
        while (length != 0) {
            size_t step = length < router->block_bytes
                ? length : router->block_bytes;
            section_route_stream_scan(router, data, step);
            if (!section_store_stream_append(router->builder, data, step)
                || !section_route_stream_seal_safe(router)) return false;
            data += step;
            length -= step;
        }
        return true;
    }
    section_route_stream_scan(router, data, length);
    if (length > SIZE_MAX - router->length) return false;
    size_t needed = router->length + length;
    if (section_route_stream_prefers_sections(router, needed)) {
        return section_route_stream_switch(router)
            && section_store_stream_append(router->builder, data, length)
            && section_route_stream_seal_safe(router);
    }
    if (needed + 1 > router->capacity) {
        size_t capacity = router->capacity == 0 ? 16384 : router->capacity;
        while (capacity < needed + 1) {
            if (capacity > SIZE_MAX / 2u) { capacity = needed + 1; break; }
            capacity *= 2u;
        }
        unsigned char *resized = budget_realloc_category(
            router->budget, BUDGET_CATEGORY_RESOURCE,
            router->buffer, capacity);
        if (resized == NULL) return false;
        router->buffer = resized;
        router->capacity = capacity;
    }
    memcpy(router->buffer + router->length, data, length);
    router->length = needed;
    router->buffer[router->length] = '\0';
    return true;
}

void section_route_stream_set_progress(
    SectionRouteStream *router, SectionStoreProgressCallback callback,
    void *opaque)
{
    if (router == NULL || !router->active) return;
    router->on_progress = callback;
    router->progress_opaque = opaque;
    if (router->builder != NULL && router->builder->active) {
        section_store_stream_set_progress(router->builder, callback, opaque);
    }
}

bool section_route_stream_finish(SectionRouteStream *router)
{
    if (router == NULL || !router->active
        || (router->length == 0 && !router->sectioned)) return false;
    bool success = !router->sectioned
        || section_store_stream_finish(router->builder);
    router->active = false;
    return success;
}

void section_route_stream_abort(SectionRouteStream *router)
{
    if (router == NULL) return;
    if (router->builder != NULL && router->builder->active) {
        section_store_stream_abort(router->builder);
    }
    budget_free(router->budget, router->buffer);
    memset(router, 0, sizeof(*router));
}
