/* Optional lossless storage for reconstructed sfnt outlines. Small faces keep
   the ordinary memory stream. All offsets are internal, validated sfnt offsets;
   no cache file or author-controlled compressed block format is introduced. */
#define FONT_PACKED_BLOCK 8192u
#define FONT_PACKED_BLOCKS 128u
#define FONT_PACKED_HOT_BYTES 4096u
#define FONT_PACKED_HOT_ENTRIES 16u
typedef struct {
    FT_Memory memory;
    FT_StreamRec stream;
    unsigned char *prefix, *blocks, *scratch, *hot;
    size_t prefix_length, length, stored, block_count, cached;
    struct {
        uint32_t offset;
        uint16_t length;
    } index[FONT_PACKED_BLOCKS];
    struct {
        uint32_t offset;
        uint16_t length, at;
    } hot_entries[FONT_PACKED_HOT_ENTRIES];
    unsigned hot_count, hot_used;
    z_stream inflater;
    bool inflate_ready;
} FontPackedStream;

static unsigned long font_packed_read(FT_Stream stream, unsigned long offset,
                                      unsigned char *output, unsigned long count);

static bool font_stream_metadata(FT_Stream stream, const unsigned char **bytes,
                                 size_t *length)
{
    if (stream == NULL)
        return false;
    if (stream->read == font_packed_read) {
        const FontPackedStream *packed = stream->descriptor.pointer;
        if (packed == NULL)
            return false;
        *bytes = packed->prefix;
        *length = packed->prefix_length;
        return true;
    }
    if (stream->read != NULL || stream->base == NULL)
        return false;
    *bytes = stream->base;
    *length = stream->size;
    return true;
}

static voidpf font_packed_zalloc(voidpf opaque, uInt count, uInt size)
{
    if (size != 0 && count > (size_t)LONG_MAX / size)
        return NULL;
    return font_ft_allocate(opaque, (size_t)count * size);
}

static void font_packed_destroy(FontPackedStream *packed)
{
    if (packed == NULL)
        return;
    FT_Memory memory = packed->memory;
    if (packed->inflate_ready)
        inflateEnd(&packed->inflater);
    font_ft_release(memory, packed->prefix);
    font_ft_release(memory, packed->blocks);
    font_ft_release(memory, packed->scratch);
    font_ft_release(memory, packed->hot);
    memory->free(memory, packed);
}

static unsigned long font_packed_read(FT_Stream stream, unsigned long offset,
                                      unsigned char *output, unsigned long count)
{
    FontPackedStream *packed = stream->descriptor.pointer;
    if (packed == NULL || offset > packed->length || count > packed->length - offset)
        return count != 0 ? 0 : 1;
    if (count == 0)
        return 0;
    if (output == NULL)
        return 0;
    for (unsigned i = 0; offset >= packed->prefix_length && i < packed->hot_count;
         i++) {
        uint32_t start = packed->hot_entries[i].offset;
        size_t size = packed->hot_entries[i].length;
        if (offset >= start && offset - start <= size &&
            count <= size - (offset - start)) {
            memcpy(output, packed->hot + packed->hot_entries[i].at + offset - start,
                   count);
            return count;
        }
    }
    unsigned char *begin = output;
    size_t at = offset, left = count;
    if (at < packed->prefix_length) {
        size_t take = packed->prefix_length - at;
        if (take > left)
            take = left;
        memcpy(output, packed->prefix + at, take);
        at += take;
        output += take;
        left -= take;
    }
    while (left != 0) {
        size_t local = at - packed->prefix_length;
        size_t block = local / FONT_PACKED_BLOCK;
        size_t in_block = local % FONT_PACKED_BLOCK;
        if (block >= packed->block_count)
            return 0;
        size_t raw = packed->length - packed->prefix_length - block * FONT_PACKED_BLOCK;
        if (raw > FONT_PACKED_BLOCK)
            raw = FONT_PACKED_BLOCK;
        if (packed->cached != block) {
            if (inflateReset(&packed->inflater) != Z_OK)
                return 0;
            packed->inflater.next_in = packed->blocks + packed->index[block].offset;
            packed->inflater.avail_in = packed->index[block].length;
            packed->inflater.next_out = packed->scratch;
            packed->inflater.avail_out = (uInt)raw;
            if (inflate(&packed->inflater, Z_FINISH) != Z_STREAM_END ||
                packed->inflater.avail_in != 0 || packed->inflater.avail_out != 0)
                return 0;
            packed->cached = block;
        }
        size_t take = raw - in_block;
        if (take > left)
            take = left;
        memcpy(output, packed->scratch + in_block, take);
        at += take;
        output += take;
        left -= take;
    }
    /* Cache complete small glyph reads, not every ten-byte header probe.
       Subsequent metrics/raster reads can borrow any contained subrange. */
    if (offset >= packed->prefix_length && count >= 32u && count <= 1024u) {
        if (packed->hot_count == FONT_PACKED_HOT_ENTRIES ||
            count > FONT_PACKED_HOT_BYTES - packed->hot_used) {
            packed->hot_count = packed->hot_used = 0;
        }
        unsigned slot = packed->hot_count++;
        packed->hot_entries[slot].offset = (uint32_t)offset;
        packed->hot_entries[slot].length = (uint16_t)count;
        packed->hot_entries[slot].at = (uint16_t)packed->hot_used;
        memcpy(packed->hot + packed->hot_used, begin, count);
        packed->hot_used += (unsigned)count;
    }
    return count;
}


static bool font_cmap4_glyph(const unsigned char *data, size_t length, unsigned code,
                             unsigned *glyph)
{
    if (length < 16u)
        return false;
    unsigned count = read_be16(data + 6) / 2u;
    if (count == 0 || count > (length - 16u) / 8u)
        return false;
    unsigned lo = 0, hi = count;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2u;
        if (code > read_be16(data + 14u + 2u * mid))
            lo = mid + 1u;
        else
            hi = mid;
    }
    *glyph = 0;
    if (lo == count || code < read_be16(data + 16u + 2u * count + 2u * lo))
        return true;
    unsigned delta = read_be16(data + 16u + 4u * count + 2u * lo);
    size_t range_at = 16u + 6u * count + 2u * lo;
    unsigned range = read_be16(data + range_at);
    if (range == 0) {
        *glyph = (code + delta) & 65535u;
        return true;
    }
    size_t at = range_at + range +
                2u * (code - read_be16(data + 16u + 2u * count + 2u * lo));
    if (at > length || length - at < 2u)
        return false;
    unsigned value = read_be16(data + at);
    *glyph = value == 0 ? 0 : (value + delta) & 65535u;
    return true;
}

/* A full-Unicode map sometimes repeats precisely the BMP map in a much
   larger format. Remove it only after comparing the entire mapping, including
   unmapped characters. Non-BMP/variation/different mappings remain intact. */
static unsigned char *font_packed_cmap(FT_Memory memory, const unsigned char *data,
                                       size_t length, size_t *result_length)
{
    if (length < 4u)
        return NULL;
    unsigned count = read_be16(data + 2);
    if (count == 0 || count > 32u || count > (length - 4u) / 8u)
        return NULL;
    struct {
        unsigned platform, encoding, format;
        size_t offset, length;
        bool drop;
    } records[32];
    const unsigned char *bmp = NULL;
    size_t bmp_length = 0;
    bool changed = false;
    for (unsigned i = 0; i < count; i++) {
        const unsigned char *r = data + 4u + 8u * i;
        size_t at = read_be32(r + 4);
        if (at < 4u + 8u * count || at > length || length - at < 4u)
            return NULL;
        unsigned format = read_be16(data + at);
        size_t size;
        if (format == 14u) {
            if (length - at < 6u)
                return NULL;
            size = read_be32(data + at + 2);
        } else if (format == 8u || format == 10u || format == 12u || format == 13u) {
            if (length - at < 8u)
                return NULL;
            size = read_be32(data + at + 4);
        } else
            size = read_be16(data + at + 2);
        if (size > length - at || size < 4u)
            return NULL;
        records[i].platform = read_be16(r);
        records[i].encoding = read_be16(r + 2);
        records[i].format = format;
        records[i].offset = at;
        records[i].length = size;
        records[i].drop = false;
        if (format == 4u &&
            (records[i].platform == 0u ||
             (records[i].platform == 3u && records[i].encoding == 1u))) {
            bmp = data + at;
            bmp_length = size;
        }
    }
    if (bmp == NULL)
        return NULL;
    for (unsigned i = 0; i < count; i++) {
        if (records[i].format != 12u ||
            !(records[i].platform == 0u ||
              (records[i].platform == 3u && records[i].encoding == 10u)))
            continue;
        const unsigned char *full = data + records[i].offset;
        size_t size = records[i].length;
        if (size < 16u)
            continue;
        uint32_t groups = read_be32(full + 12);
        if (groups > 65536u || groups > (size - 16u) / 12u)
            continue;
        bool equal = true;
        unsigned group = 0;
        for (unsigned code = 0; code < 65536u && equal; code++) {
            while (group < groups && code > read_be32(full + 16u + 12u * group + 4u))
                group++;
            unsigned mapped = 0, glyph = 0;
            if (group < groups && code >= read_be32(full + 16u + 12u * group)) {
                uint32_t first = read_be32(full + 16u + 12u * group);
                uint32_t base = read_be32(full + 16u + 12u * group + 8u);
                if (base > 65535u || code - first > 65535u - base) {
                    equal = false;
                    break;
                }
                mapped = base + code - first;
            }
            equal = font_cmap4_glyph(bmp, bmp_length, code, &glyph) && glyph == mapped;
            if ((code & 255u) == 255u &&
                !tilefinch_platform_cooperate("font-cmap", code))
                return NULL;
        }
        for (unsigned g = 0; g < groups && equal; g++) {
            uint32_t first = read_be32(full + 16u + 12u * g),
                     last = read_be32(full + 20u + 12u * g);
            if (first > last || last > 65535u ||
                (g != 0 && first <= read_be32(full + 8u + 12u * g)))
                equal = false;
        }
        records[i].drop = equal;
        changed |= equal;
    }
    if (!changed)
        return NULL;
    unsigned kept = 0;
    for (unsigned i = 0; i < count; i++)
        kept += !records[i].drop;
    unsigned char *output = memory->alloc(memory, (long)length);
    if (output == NULL)
        return NULL;
    memset(output, 0, 4u + 8u * kept);
    output[2] = (unsigned char)(kept >> 8);
    output[3] = (unsigned char)kept;
    size_t used = 4u + 8u * kept;
    unsigned out = 0;
    uint32_t positions[32] = {0};
    for (unsigned i = 0; i < count; i++) {
        if (records[i].drop)
            continue;
        size_t at = 0;
        for (unsigned j = 0; j < i; j++)
            if (!records[j].drop && records[j].offset == records[i].offset) {
                at = positions[j];
                break;
            }
        if (at == 0) {
            if (used > length || records[i].length > length - used) {
                memory->free(memory, output);
                return NULL;
            }
            at = used;
            memcpy(output + used, data + records[i].offset, records[i].length);
            used += records[i].length;
        }
        positions[i] = (uint32_t)at;
        memcpy(output + 4u + 8u * out, data + 4u + 8u * i, 4);
        write_be32(output + 8u + 8u * out++, (uint32_t)at);
    }
    *result_length = used;
    return output;
}

static FontPackedStream *font_packed_create(FT_Memory memory, FT_Stream source)
{
    if (source == NULL || source->read != NULL || source->base == NULL ||
        source->size < 12u || source->size > 2u * 1024u * 1024u ||
        read_be32(source->base) != SFNT_TRUETYPE_SIGNATURE)
        return NULL;
    const unsigned char *raw = source->base;
    size_t length = source->size;
    unsigned count = read_be16(raw + 4);
    if (count == 0 || count > 256u || count > (length - 12u) / 16u)
        return NULL;
    size_t glyf_at = 0, glyf_length = 0, cmap_length = 0;
    unsigned char *cmap = NULL;
    size_t sizes[256], offsets[256];
    bool keep[256];
    unsigned kept = 0;
    size_t prefix = 12u;
    for (unsigned i = 0; i < count; i++) {
        const unsigned char *r = raw + 12u + 16u * i;
        uint32_t tag = read_be32(r);
        for (unsigned previous = 0; previous < i; previous++)
            if (read_be32(raw + 12u + 16u * previous) == tag)
                goto fail;
        offsets[i] = read_be32(r + 8);
        sizes[i] = read_be32(r + 12);
        if (offsets[i] > length || sizes[i] > length - offsets[i])
            goto fail;
        keep[i] = tag != UINT32_C(0x47535542) && tag != UINT32_C(0x6e616d65) &&
                  tag != UINT32_C(0x6670676d) && tag != UINT32_C(0x70726570) &&
                  tag != UINT32_C(0x63767420) && tag != UINT32_C(0x44534947);
        if (tag == UINT32_C(0x676c7966)) {
            glyf_at = offsets[i];
            glyf_length = sizes[i];
        }
        if (tag == UINT32_C(0x636d6170)) {
            cmap = font_packed_cmap(memory, raw + offsets[i], sizes[i], &cmap_length);
            if (cmap != NULL)
                sizes[i] = cmap_length;
        }
        if (tag == UINT32_C(0x706f7374) && sizes[i] >= 32u)
            sizes[i] = 32u;
        if (keep[i]) {
            size_t charge = 16u;
            if (tag != UINT32_C(0x676c7966))
                charge += (sizes[i] + 3u) & ~(size_t)3u;
            if (charge > 2u * 1024u * 1024u - prefix)
                goto fail;
            kept++;
            prefix += charge;
        }
    }
    if (glyf_length == 0 || glyf_length > FONT_PACKED_BLOCK * FONT_PACKED_BLOCKS)
        goto fail;
    FontPackedStream *packed = memory->alloc(memory, sizeof(*packed));
    if (packed == NULL)
        goto fail;
    memset(packed, 0, sizeof(*packed));
    packed->memory = memory;
    packed->cached = SIZE_MAX;
    packed->prefix_length = prefix;
    packed->length = prefix + glyf_length;
    packed->prefix = memory->alloc(memory, (long)prefix);
    packed->blocks =
        memory->alloc(memory, (long)(glyf_length + FONT_PACKED_BLOCKS * 64u));
    packed->scratch = memory->alloc(memory, FONT_PACKED_BLOCK);
    packed->hot = memory->alloc(memory, FONT_PACKED_HOT_BYTES);
    if (!packed->prefix || !packed->blocks || !packed->scratch || !packed->hot)
        goto packed_fail;
    memset(packed->prefix, 0, prefix);
    memcpy(packed->prefix, raw, 12);
    packed->prefix[4] = (unsigned char)(kept >> 8);
    packed->prefix[5] = (unsigned char)kept;
    unsigned power = 1, selector = 0;
    while (power <= kept / 2u) {
        power *= 2u;
        selector++;
    }
    unsigned head[] = {power * 16u, selector, kept * 16u - power * 16u};
    for (unsigned i = 0; i < 3; i++) {
        packed->prefix[6 + 2 * i] = (unsigned char)(head[i] >> 8);
        packed->prefix[7 + 2 * i] = (unsigned char)head[i];
    }
    size_t at = 12u + 16u * kept;
    unsigned out = 0;
    for (unsigned i = 0; i < count; i++) {
        if (!keep[i])
            continue;
        const unsigned char *r = raw + 12u + 16u * i;
        uint32_t tag = read_be32(r);
        unsigned char *destination = packed->prefix + 12u + 16u * out++;
        memcpy(destination, r, 16);
        write_be32(destination + 12, (uint32_t)sizes[i]);
        write_be32(destination + 8,
                          (uint32_t)(tag == UINT32_C(0x676c7966) ? prefix : at));
        if (tag == UINT32_C(0x676c7966))
            continue;
        memcpy(packed->prefix + at,
               tag == UINT32_C(0x636d6170) && cmap ? cmap : raw + offsets[i], sizes[i]);
        if (tag == UINT32_C(0x706f7374) && sizes[i] >= 32u)
            write_be32(packed->prefix + at, UINT32_C(0x00030000));
        at += (sizes[i] + 3u) & ~(size_t)3u;
    }
    font_ft_release(memory, cmap);
    cmap = NULL;
    unsigned char *temporary =
        memory->alloc(memory, (long)compressBound(FONT_PACKED_BLOCK));
    if (temporary == NULL)
        goto packed_fail;
    z_stream encoder = {
        .zalloc = font_packed_zalloc, .zfree = font_ft_release, .opaque = memory};
    if (deflateInit2(&encoder, 1, Z_DEFLATED, 13, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        font_ft_release(memory, temporary);
        goto packed_fail;
    }
    bool encoded = true;
    for (size_t from = 0; from < glyf_length; from += FONT_PACKED_BLOCK) {
        if (!tilefinch_platform_cooperate("font-pack", from)) {
            encoded = false;
            break;
        }
        size_t take = glyf_length - from;
        if (take > FONT_PACKED_BLOCK)
            take = FONT_PACKED_BLOCK;
        if (deflateReset(&encoder) != Z_OK) {
            encoded = false;
            break;
        }
        encoder.next_in = (unsigned char *)raw + glyf_at + from;
        encoder.avail_in = (uInt)take;
        encoder.next_out = temporary;
        encoder.avail_out = (uInt)compressBound(FONT_PACKED_BLOCK);
        int result = deflate(&encoder, Z_FINISH);
        size_t size = encoder.total_out;
        if (result != Z_STREAM_END || size > UINT16_MAX ||
            size > glyf_length + FONT_PACKED_BLOCKS * 64u - packed->stored) {
            encoded = false;
            break;
        }
        packed->index[packed->block_count].offset = (uint32_t)packed->stored;
        packed->index[packed->block_count++].length = (uint16_t)size;
        memcpy(packed->blocks + packed->stored, temporary, size);
        packed->stored += size;
    }
    deflateEnd(&encoder);
    font_ft_release(memory, temporary);
    if (!encoded)
        goto packed_fail;
    unsigned char *smaller =
        memory->realloc(memory, (long)(glyf_length + FONT_PACKED_BLOCKS * 64u),
                        (long)packed->stored, packed->blocks);
    if (smaller == NULL)
        goto packed_fail;
    packed->blocks = smaller;
    packed->inflater = (z_stream){
        .zalloc = font_packed_zalloc, .zfree = font_ft_release, .opaque = memory};
    if (inflateInit2(&packed->inflater, 13) != Z_OK)
        goto packed_fail;
    packed->inflate_ready = true;
    packed->stream.size = (unsigned long)packed->length;
    packed->stream.read = font_packed_read;
    packed->stream.memory = memory;
    packed->stream.descriptor.pointer = packed;
    return packed;
packed_fail:
    font_packed_destroy(packed);
fail:
    font_ft_release(memory, cmap);
    return NULL;
}
