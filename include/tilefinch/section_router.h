#ifndef TILEFINCH_SECTION_ROUTER_H
#define TILEFINCH_SECTION_ROUTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/section_store.h"

typedef struct {
    SectionStoreStreamBuilder *builder;
    CompressedSectionStore *store;
    Budget *budget;
    unsigned char *buffer;
    size_t length;
    size_t capacity;
    size_t byte_threshold;
    size_t block_bytes;
    size_t maximum_section_bytes;
    size_t element_count;
    size_t attribute_count;
    size_t maximum_depth;
    size_t depth;
    size_t scanned_bytes;
    size_t body_depth;
    size_t prefix_length;
    size_t safe_boundary;
    size_t sealed_boundary;
    uint64_t scan_us;
    uint64_t replay_us;
    SectionStoreProgressCallback on_progress;
    void *progress_opaque;
    char tag_name[16];
    char raw_name[10];
    char tag_prefix[3];
    char open_tags[64][16];
    uint8_t open_tag_lengths[64];
    char quote;
    uint8_t tag_name_length;
    uint8_t raw_name_length;
    uint8_t tag_prefix_length;
    uint8_t comment_dashes;
    bool in_tag;
    bool tag_first_pending;
    bool tag_name_complete;
    bool tag_name_overflow;
    bool closing_tag;
    bool declaration_tag;
    bool in_comment;
    bool body_seen;
    bool safety_disabled;
    bool sectioned;
    bool active;
} SectionRouteStream;

size_t section_route_stream_threshold(const Budget *budget);
void section_route_stream_scan(SectionRouteStream *router,
                               const unsigned char *data, size_t length);
bool section_route_stream_prefers_sections(
    const SectionRouteStream *router, size_t prospective_length);
bool section_route_stream_begin(SectionRouteStream *router,
                                SectionStoreStreamBuilder *builder,
                                CompressedSectionStore *store,
                                Budget *budget, size_t block_bytes,
                                size_t maximum_section_bytes,
                                bool force_sections);
bool section_route_stream_body(void *opaque, const unsigned char *data,
                               size_t length);
void section_route_stream_set_progress(
    SectionRouteStream *router, SectionStoreProgressCallback callback,
    void *opaque);
bool section_route_stream_finish(SectionRouteStream *router);
void section_route_stream_abort(SectionRouteStream *router);

#endif
