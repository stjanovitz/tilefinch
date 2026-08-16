#include "media_h264_psp_compat.h"

#include <limits.h>
#include <string.h>

#define H264_COMPAT_PARAMETER_BYTES 512u
#define H264_COMPAT_HEADER_BYTES 128u
/* A normal target access unit has one or two NALs, but AVC permits
   multi-slice pictures. Keep the walk bounded without turning an otherwise
   legal, moderately sliced picture into a device-only playback failure. */
#define H264_COMPAT_MAX_NALS 64u

typedef struct {
    const unsigned char *data;
    size_t bytes;
    size_t bit;
} H264Bits;

typedef struct {
    unsigned char data[H264_COMPAT_HEADER_BYTES];
    size_t bit;
} H264Writer;

static bool h264_bits(H264Bits *bits, unsigned count, uint32_t *value)
{
    if (bits == NULL || value == NULL || count > 32u
        || bits->bit > bits->bytes * 8u
        || count > bits->bytes * 8u - bits->bit) return false;
    uint32_t result = 0;
    for (unsigned i = 0; i < count; i++) {
        size_t at = bits->bit++;
        result = (result << 1u)
            | ((bits->data[at >> 3u] >> (7u - (at & 7u))) & 1u);
    }
    *value = result;
    return true;
}

static bool h264_bit(H264Bits *bits, uint32_t *value)
{
    return h264_bits(bits, 1u, value);
}

static bool h264_ue(H264Bits *bits, uint32_t *value)
{
    unsigned zeros = 0;
    uint32_t bit = 0, suffix = 0;
    while (zeros < 31u) {
        if (!h264_bit(bits, &bit)) return false;
        if (bit != 0) break;
        zeros++;
    }
    if (bit == 0 || !h264_bits(bits, zeros, &suffix)) return false;
    *value = ((UINT32_C(1) << zeros) - 1u) + suffix;
    return true;
}

static bool h264_se(H264Bits *bits, int32_t *value)
{
    uint32_t code = 0;
    if (!h264_ue(bits, &code) || code == UINT32_MAX) return false;
    *value = (code & 1u)
        ? (int32_t) ((code + 1u) / 2u)
        : -(int32_t) (code / 2u);
    return true;
}

static bool h264_writer_bit(H264Writer *writer, uint32_t value)
{
    if (writer == NULL || writer->bit >= sizeof(writer->data) * 8u)
        return false;
    if ((value & 1u) != 0)
        writer->data[writer->bit >> 3u] |=
            (unsigned char) (1u << (7u - (writer->bit & 7u)));
    writer->bit++;
    return true;
}

static bool h264_writer_bits(
    H264Writer *writer, uint32_t value, unsigned count)
{
    if (count > 32u) return false;
    for (unsigned left = count; left != 0; left--) {
        if (!h264_writer_bit(writer, value >> (left - 1u))) return false;
    }
    return true;
}

static bool h264_writer_copy(
    H264Writer *writer, const unsigned char *source,
    size_t first, size_t last)
{
    if (source == NULL || first > last) return false;
    for (size_t at = first; at < last; at++) {
        if (!h264_writer_bit(
                writer, source[at >> 3u] >> (7u - (at & 7u)))) return false;
    }
    return true;
}

static bool h264_writer_ue(H264Writer *writer, uint32_t value)
{
    if (value == UINT32_MAX) return false;
    uint32_t code = value + 1u;
    unsigned width = 0;
    for (uint32_t scan = code; scan != 0; scan >>= 1u) width++;
    for (unsigned i = 1; i < width; i++) {
        if (!h264_writer_bit(writer, 0)) return false;
    }
    return h264_writer_bits(writer, code, width);
}

static bool h264_writer_align_ones(H264Writer *writer)
{
    while ((writer->bit & 7u) != 0) {
        if (!h264_writer_bit(writer, 1u)) return false;
    }
    return true;
}

static bool h264_rbsp(
    const unsigned char *nal, size_t length,
    unsigned char *output, size_t capacity, size_t *output_length)
{
    if (output_length != NULL) *output_length = 0;
    if (nal == NULL || output == NULL || output_length == NULL
        || length < 2u || capacity == 0) return false;
    size_t written = 0;
    unsigned zeros = 0;
    for (size_t at = 1; at < length; at++) {
        if (zeros >= 2u && nal[at] == 3u
            && at + 1u < length && nal[at + 1u] <= 3u) {
            zeros = 0;
            continue;
        }
        if (written >= capacity) return false;
        output[written++] = nal[at];
        zeros = nal[at] == 0 ? zeros + 1u : 0u;
    }
    *output_length = written;
    return true;
}

static bool h264_skip_scaling(H264Bits *bits, unsigned count)
{
    int last = 8, next = 8;
    for (unsigned i = 0; i < count; i++) {
        if (next != 0) {
            int32_t delta = 0;
            if (!h264_se(bits, &delta)) return false;
            next = (last + delta + 256) & 255;
        }
        if (next != 0) last = next;
    }
    return true;
}

static bool h264_parse_sps(
    MediaH264PspCompat *compat,
    const unsigned char *nal, size_t length)
{
    unsigned char rbsp[H264_COMPAT_PARAMETER_BYTES];
    size_t rbsp_length = 0;
    if (nal == NULL || length < 4u || length > sizeof(rbsp)
        || (nal[0] & 0x1fu) != 7u
        || !h264_rbsp(nal, length, rbsp, sizeof(rbsp), &rbsp_length))
        return false;
    H264Bits bits = {rbsp, rbsp_length, 0};
    uint32_t profile = 0, ignored = 0, chroma = 1, sps_id = 0;
    if (!h264_bits(&bits, 8u, &profile)
        || !h264_bits(&bits, 8u, &ignored)
        || !h264_bits(&bits, 8u, &ignored)
        || !h264_ue(&bits, &sps_id)) return false;
    bool separate_colour_plane = false;
    if (profile == 100u || profile == 110u || profile == 122u
        || profile == 244u || profile == 44u || profile == 83u
        || profile == 86u || profile == 118u || profile == 128u
        || profile == 138u || profile == 139u || profile == 134u
        || profile == 135u) {
        if (!h264_ue(&bits, &chroma) || chroma > 3u) return false;
        if (chroma == 3u) {
            if (!h264_bit(&bits, &ignored)) return false;
            separate_colour_plane = ignored != 0;
        }
        if (!h264_ue(&bits, &ignored) || !h264_ue(&bits, &ignored)
            || !h264_bit(&bits, &ignored) || !h264_bit(&bits, &ignored))
            return false;
        if (ignored != 0) {
            unsigned lists = chroma == 3u ? 12u : 8u;
            for (unsigned i = 0; i < lists; i++) {
                uint32_t present = 0;
                if (!h264_bit(&bits, &present)
                    || (present && !h264_skip_scaling(
                        &bits, i < 6u ? 16u : 64u))) return false;
            }
        }
    }
    uint32_t frame_minus4 = 0, order_type = 0, poc_minus4 = 0;
    if (!h264_ue(&bits, &frame_minus4) || frame_minus4 > 12u
        || !h264_ue(&bits, &order_type) || order_type != 0u
        || !h264_ue(&bits, &poc_minus4) || poc_minus4 > 12u
        || !h264_ue(&bits, &ignored) || !h264_bit(&bits, &ignored))
        return false;
    uint32_t width = 0, height = 0, frame_only = 0;
    if (!h264_ue(&bits, &width) || !h264_ue(&bits, &height)
        || !h264_bit(&bits, &frame_only) || frame_only == 0u)
        return false;
    compat->sps_id = sps_id;
    compat->log2_max_frame_num = (uint8_t) (frame_minus4 + 4u);
    compat->log2_max_pic_order_cnt_lsb = (uint8_t) (poc_minus4 + 4u);
    compat->chroma_format_idc = separate_colour_plane ? 0u : (uint8_t) chroma;
    return compat->chroma_format_idc == 1u;
}

static bool h264_parse_pps(
    MediaH264PspCompat *compat,
    const unsigned char *nal, size_t length)
{
    unsigned char rbsp[H264_COMPAT_PARAMETER_BYTES];
    size_t rbsp_length = 0;
    if (nal == NULL || length < 2u || length > sizeof(rbsp)
        || (nal[0] & 0x1fu) != 8u
        || !h264_rbsp(nal, length, rbsp, sizeof(rbsp), &rbsp_length))
        return false;
    H264Bits bits = {rbsp, rbsp_length, 0};
    uint32_t pps_id = 0, sps_id = 0, value = 0;
    if (!h264_ue(&bits, &pps_id) || !h264_ue(&bits, &sps_id)
        || sps_id != compat->sps_id
        || !h264_bit(&bits, &value)) return false;
    compat->entropy_coding_mode = value != 0;
    if (!h264_bit(&bits, &value)) return false;
    compat->bottom_field_pic_order_present = value != 0;
    if (!h264_ue(&bits, &value) || value != 0u) return false;
    if (!h264_ue(&bits, &value) || value >= UINT8_MAX) return false;
    compat->num_ref_idx_l0_default = (uint8_t) (value + 1u);
    if (!h264_ue(&bits, &value) || value >= UINT8_MAX) return false;
    compat->num_ref_idx_l1_default = (uint8_t) (value + 1u);
    if (!h264_bit(&bits, &value)) return false;
    compat->weighted_pred = value != 0;
    if (!h264_bits(&bits, 2u, &value)) return false;
    compat->weighted_bipred_idc = (uint8_t) value;
    int32_t signed_value = 0;
    if (!h264_se(&bits, &signed_value) || !h264_se(&bits, &signed_value)
        || !h264_se(&bits, &signed_value) || !h264_bit(&bits, &value))
        return false;
    compat->deblocking_filter_control_present = value != 0;
    if (!h264_bit(&bits, &value) || !h264_bit(&bits, &value)) return false;
    compat->redundant_pic_count_present = value != 0;
    compat->pps_id = pps_id;
    return compat->entropy_coding_mode;
}

static bool h264_avcc_sets(
    const unsigned char *avcc, size_t length,
    const unsigned char **sps, size_t *sps_length,
    const unsigned char **pps, size_t *pps_length, uint8_t *prefix)
{
    if (avcc == NULL || sps == NULL || sps_length == NULL
        || pps == NULL || pps_length == NULL || prefix == NULL
        || length < 8u || avcc[0] != 1u) return false;
    *prefix = (uint8_t) ((avcc[4] & 3u) + 1u);
    unsigned sps_count = avcc[5] & 31u;
    if (*prefix != 4u || sps_count != 1u) return false;
    size_t at = 6u;
    if (length - at < 2u) return false;
    *sps_length = ((size_t) avcc[at] << 8u) | avcc[at + 1u];
    at += 2u;
    if (*sps_length == 0 || *sps_length > length - at) return false;
    *sps = avcc + at;
    at += *sps_length;
    if (at >= length || avcc[at++] != 1u || length - at < 2u)
        return false;
    *pps_length = ((size_t) avcc[at] << 8u) | avcc[at + 1u];
    at += 2u;
    if (*pps_length == 0 || *pps_length > length - at) return false;
    *pps = avcc + at;
    return true;
}

bool media_h264_psp_compat_init(
    MediaH264PspCompat *compat,
    const unsigned char *avcc, size_t avcc_length)
{
    if (compat == NULL) return false;
    memset(compat, 0, sizeof(*compat));
    const unsigned char *sps = NULL, *pps = NULL;
    size_t sps_length = 0, pps_length = 0;
    if (!h264_avcc_sets(
            avcc, avcc_length, &sps, &sps_length,
            &pps, &pps_length, &compat->nal_length_size)
        || !h264_parse_sps(compat, sps, sps_length)
        || !h264_parse_pps(compat, pps, pps_length)) return false;
    compat->enabled = true;
    return true;
}

void media_h264_psp_compat_reset(MediaH264PspCompat *compat)
{
    if (compat == NULL) return;
    compat->active_gop = false;
    compat->first_reference_pending = false;
    compat->frame_num_base = 0;
}

static bool h264_no_escape_before(
    const unsigned char *nal, size_t length, size_t byte_limit)
{
    if (nal == NULL || byte_limit > length) return false;
    for (size_t at = 1u; at + 2u < byte_limit; at++) {
        if (nal[at] == 0u && nal[at + 1u] == 0u && nal[at + 2u] == 3u)
            return false;
    }
    return true;
}

typedef struct {
    uint32_t nal_header;
    uint32_t first_mb;
    uint32_t slice_type;
    uint32_t pps_id;
    uint32_t frame_num;
    size_t frame_num_at;
} H264SlicePrefix;

static bool h264_slice_prefix(
    const MediaH264PspCompat *compat,
    const unsigned char *nal, size_t length,
    H264Bits *bits, H264SlicePrefix *prefix)
{
    if (compat == NULL || nal == NULL || bits == NULL || prefix == NULL
        || length < 3u || (nal[0] & 0x80u) != 0) return false;
    *bits = (H264Bits) {nal, length, 0};
    if (!h264_bits(bits, 8u, &prefix->nal_header)
        || !h264_ue(bits, &prefix->first_mb)
        || !h264_ue(bits, &prefix->slice_type)
        || !h264_ue(bits, &prefix->pps_id)
        || prefix->pps_id != compat->pps_id) return false;
    prefix->frame_num_at = bits->bit;
    return h264_bits(
        bits, compat->log2_max_frame_num, &prefix->frame_num);
}

static bool h264_slice_poc(
    const MediaH264PspCompat *compat, H264Bits *bits)
{
    uint32_t ignored = 0;
    int32_t signed_ignored = 0;
    if (!h264_bits(bits, compat->log2_max_pic_order_cnt_lsb, &ignored))
        return false;
    if (compat->bottom_field_pic_order_present
        && !h264_se(bits, &signed_ignored)) return false;
    if (compat->redundant_pic_count_present
        && !h264_ue(bits, &ignored)) return false;
    return true;
}

static bool h264_slice_tail(
    const MediaH264PspCompat *compat, H264Bits *bits,
    unsigned slice_class, size_t *semantic_end, size_t *payload_byte)
{
    uint32_t value = 0;
    int32_t signed_value = 0;
    if (compat->entropy_coding_mode && slice_class != 2u
        && slice_class != 4u && !h264_ue(bits, &value)) return false;
    if (!h264_se(bits, &signed_value)) return false;
    if (slice_class == 3u) {
        if (!h264_bit(bits, &value) || !h264_se(bits, &signed_value))
            return false;
    } else if (slice_class == 4u
               && !h264_se(bits, &signed_value)) return false;
    if (compat->deblocking_filter_control_present) {
        if (!h264_ue(bits, &value)) return false;
        if (value != 1u
            && (!h264_se(bits, &signed_value)
                || !h264_se(bits, &signed_value))) return false;
    }
    *semantic_end = bits->bit;
    if (!compat->entropy_coding_mode) return false;
    while ((bits->bit & 7u) != 0) {
        if (!h264_bit(bits, &value) || value != 1u) return false;
    }
    *payload_byte = bits->bit / 8u;
    return h264_no_escape_before(bits->data, bits->bytes, *payload_byte);
}

static bool h264_replace_header(
    unsigned char *nal, size_t *length, size_t capacity,
    const H264Writer *header, size_t old_payload_byte)
{
    if (nal == NULL || length == NULL || header == NULL
        || (header->bit & 7u) != 0 || old_payload_byte > *length)
        return false;
    size_t header_bytes = header->bit / 8u;
    size_t payload = *length - old_payload_byte;
    if (header_bytes > capacity || payload > capacity - header_bytes)
        return false;
    memmove(nal + header_bytes, nal + old_payload_byte, payload);
    memcpy(nal, header->data, header_bytes);
    *length = header_bytes + payload;
    return true;
}

static bool h264_rewrite_recovery(
    MediaH264PspCompat *compat,
    unsigned char *nal, size_t *length, size_t capacity)
{
    H264Bits bits;
    H264SlicePrefix prefix;
    if (!h264_slice_prefix(compat, nal, *length, &bits, &prefix)
        || (prefix.nal_header & 0x1fu) != 1u
        || (prefix.nal_header >> 5u) == 0u
        || prefix.first_mb != 0u || prefix.slice_type % 5u != 2u)
        return false;
    size_t after_frame_num = bits.bit;
    if (!h264_slice_poc(compat, &bits)) return false;
    size_t marking_at = bits.bit;
    uint32_t adaptive = 0;
    if (!h264_bit(&bits, &adaptive) || adaptive != 0u) return false;
    size_t after_marking = bits.bit;
    size_t semantic_end = 0, payload_byte = 0;
    if (!h264_slice_tail(
            compat, &bits, prefix.slice_type % 5u,
            &semantic_end, &payload_byte)) return false;
    H264Writer writer = {{0}, 0};
    if (!h264_writer_copy(
            &writer, nal, 0, prefix.frame_num_at)
        || !h264_writer_bits(&writer, 0, compat->log2_max_frame_num)
        || !h264_writer_ue(&writer, 0)
        || !h264_writer_copy(
            &writer, nal, after_frame_num, marking_at)
        || !h264_writer_bits(&writer, 0, 2u)
        || !h264_writer_copy(
            &writer, nal, after_marking, semantic_end)
        || !h264_writer_align_ones(&writer)) return false;
    writer.data[0] = (unsigned char) ((writer.data[0] & 0xe0u) | 5u);
    compat->frame_num_base = prefix.frame_num;
    return h264_replace_header(
        nal, length, capacity, &writer, payload_byte);
}

static bool h264_rebase_frame_num(
    MediaH264PspCompat *compat, unsigned char *nal, size_t length,
    H264SlicePrefix *prefix, H264Bits *bits)
{
    if (!h264_slice_prefix(compat, nal, length, bits, prefix)
        || !h264_no_escape_before(
            nal, length, (bits->bit + 7u) / 8u)) return false;
    uint32_t mask = compat->log2_max_frame_num == 32u
        ? UINT32_MAX
        : (UINT32_C(1) << compat->log2_max_frame_num) - 1u;
    uint32_t original = prefix->frame_num;
    uint32_t rebased = (original - compat->frame_num_base) & mask;
    for (unsigned at = 0; at < compat->log2_max_frame_num; at++) {
        size_t bit = prefix->frame_num_at + at;
        unsigned shift = compat->log2_max_frame_num - at - 1u;
        unsigned char bit_mask = (unsigned char) (1u << (7u - (bit & 7u)));
        if (((rebased >> shift) & 1u) != 0)
            nal[bit >> 3u] |= bit_mask;
        else
            nal[bit >> 3u] &= (unsigned char) ~bit_mask;
    }
    if (rebased != original
        && !media_h264_psp_compat_ebsp_edit_safe(
            nal, length, prefix->frame_num_at,
            compat->log2_max_frame_num)) {
        /* The caller fails this access unit closed. Restore its bytes too, so
           a diagnostic or fallback cannot observe a partially edited NAL. */
        for (unsigned at = 0; at < compat->log2_max_frame_num; at++) {
            size_t bit = prefix->frame_num_at + at;
            unsigned shift = compat->log2_max_frame_num - at - 1u;
            unsigned char bit_mask =
                (unsigned char) (1u << (7u - (bit & 7u)));
            if (((original >> shift) & 1u) != 0)
                nal[bit >> 3u] |= bit_mask;
            else
                nal[bit >> 3u] &= (unsigned char) ~bit_mask;
        }
        return false;
    }
    if (rebased != original) compat->frame_numbers_rebased++;
    prefix->frame_num = rebased;
    return true;
}

static bool h264_skip_ref_list_modification(
    H264Bits *bits, unsigned lists)
{
    uint32_t flag = 0, operation = 0, ignored = 0;
    for (unsigned list = 0; list < lists; list++) {
        if (!h264_bit(bits, &flag)) return false;
        if (!flag) continue;
        unsigned operations = 0;
        do {
            if (++operations > 32u || !h264_ue(bits, &operation))
                return false;
            if ((operation == 0u || operation == 1u || operation == 2u)
                && !h264_ue(bits, &ignored)) return false;
            if (operation > 3u) return false;
        } while (operation != 3u);
    }
    return true;
}

static bool h264_skip_pred_weight_table(
    const MediaH264PspCompat *compat, H264Bits *bits,
    unsigned l0, unsigned l1)
{
    uint32_t value = 0;
    int32_t signed_value = 0;
    if (!h264_ue(bits, &value) || !h264_ue(bits, &value)) return false;
    unsigned lists[2] = {l0, l1};
    for (unsigned list = 0; list < 2u; list++) {
        for (unsigned ref = 0; ref < lists[list]; ref++) {
            if (!h264_bit(bits, &value)) return false;
            if (value && (!h264_se(bits, &signed_value)
                          || !h264_se(bits, &signed_value))) return false;
            if (compat->chroma_format_idc != 0u) {
                if (!h264_bit(bits, &value)) return false;
                if (value) {
                    for (unsigned component = 0; component < 2u; component++) {
                        if (!h264_se(bits, &signed_value)
                            || !h264_se(bits, &signed_value)) return false;
                    }
                }
            }
        }
    }
    return true;
}

static bool h264_remove_first_reference_marking(
    MediaH264PspCompat *compat,
    unsigned char *nal, size_t *length, size_t capacity)
{
    H264Bits bits;
    H264SlicePrefix prefix;
    if (!h264_slice_prefix(compat, nal, *length, &bits, &prefix)
        || prefix.slice_type % 5u != 0u
        || !h264_slice_poc(compat, &bits)) return false;
    uint32_t value = 0;
    unsigned l0 = compat->num_ref_idx_l0_default;
    if (!h264_bit(&bits, &value)) return false;
    if (value) {
        if (!h264_ue(&bits, &value) || value >= UINT8_MAX) return false;
        l0 = value + 1u;
    }
    if (!h264_skip_ref_list_modification(&bits, 1u)) return false;
    if (compat->weighted_pred
        && !h264_skip_pred_weight_table(compat, &bits, l0, 0u))
        return false;
    size_t marking_at = bits.bit;
    if (!h264_bit(&bits, &value)) return false;
    if (value == 0u) return true;
    unsigned operations = 0;
    uint32_t operation = 0;
    do {
        if (++operations > 32u || !h264_ue(&bits, &operation)) return false;
        if (operation == 0u) break;
        /* The synthetic IDR makes unmarking a pre-recovery short- or
           long-term picture obsolete. Assigning/resetting long-term state
           (MMCO 3..6) affects future prediction and cannot be erased while
           preserving unfamiliar encoder semantics. */
        if (!media_h264_psp_compat_mmco_removable(operation)
            || !h264_ue(&bits, &value)) return false;
    } while (operation != 0u);
    size_t after_marking = bits.bit;
    size_t semantic_end = 0, payload_byte = 0;
    if (!h264_slice_tail(
            compat, &bits, prefix.slice_type % 5u,
            &semantic_end, &payload_byte)) return false;
    H264Writer writer = {{0}, 0};
    if (!h264_writer_copy(&writer, nal, 0, marking_at)
        || !h264_writer_bit(&writer, 0)
        || !h264_writer_copy(
            &writer, nal, after_marking, semantic_end)
        || !h264_writer_align_ones(&writer)
        || !h264_replace_header(
            nal, length, capacity, &writer, payload_byte)) return false;
    compat->reference_markings_removed++;
    return true;
}

static bool h264_closed_recovery_payload(
    const unsigned char *payload, size_t length)
{
    H264Bits bits = {payload, length, 0};
    uint32_t recovery_frame_count = 0;
    uint32_t exact_match = 0;
    uint32_t broken_link = 0;
    uint32_t changing_slice_group = 0;
    if (payload == NULL || length == 0u
        || !h264_ue(&bits, &recovery_frame_count)
        || !h264_bit(&bits, &exact_match)
        || !h264_bit(&bits, &broken_link)
        || !h264_bits(&bits, 2u, &changing_slice_group)
        || recovery_frame_count != 0u || exact_match == 0u
        || broken_link != 0u || changing_slice_group != 0u)
        return false;

    /* recovery_point() payloads are byte-aligned with one stop bit followed
       by zero padding. Requiring that canonical tail keeps this adapter from
       treating an arbitrary type-6 message prefix as a closed recovery. */
    uint32_t bit = 0;
    if (!h264_bit(&bits, &bit) || bit != 1u) return false;
    while (bits.bit < bits.bytes * 8u) {
        if (!h264_bit(&bits, &bit) || bit != 0u) return false;
    }
    return true;
}

static bool h264_closed_recovery_sei(
    const unsigned char *nal, size_t length)
{
    unsigned char rbsp[H264_COMPAT_PARAMETER_BYTES];
    size_t rbsp_length = 0;
    if (nal == NULL || length < 2u || (nal[0] & 0x1fu) != 6u
        || !h264_rbsp(nal, length, rbsp, sizeof(rbsp), &rbsp_length))
        return false;

    /* A single SEI NAL may carry several messages. Some observed renditions
       place recovery_point before user_data_unregistered in the same NAL, so
       AVC renditions, so an exact five-byte NAL comparison misses otherwise
       valid closed recovery groups. Parse the bounded message envelope while
       leaving every message byte unchanged. */
    size_t at = 0;
    bool closed_recovery = false;
    unsigned messages = 0;
    while (at < rbsp_length) {
        if (rbsp[at] == 0x80u && at + 1u == rbsp_length)
            return closed_recovery;
        if (++messages > 16u) return false;

        size_t payload_type = 0;
        while (at < rbsp_length && rbsp[at] == 0xffu) {
            if (payload_type > SIZE_MAX - 255u) return false;
            payload_type += 255u;
            at++;
        }
        if (at >= rbsp_length || payload_type > SIZE_MAX - rbsp[at])
            return false;
        payload_type += rbsp[at++];

        size_t payload_size = 0;
        while (at < rbsp_length && rbsp[at] == 0xffu) {
            if (payload_size > SIZE_MAX - 255u) return false;
            payload_size += 255u;
            at++;
        }
        if (at >= rbsp_length || payload_size > SIZE_MAX - rbsp[at])
            return false;
        payload_size += rbsp[at++];
        if (payload_size > rbsp_length - at) return false;
        if (payload_type == 6u
            && h264_closed_recovery_payload(rbsp + at, payload_size))
            closed_recovery = true;
        at += payload_size;
    }
    return false;
}

static bool h264_prefix_read(
    const unsigned char *data, unsigned width, size_t *length)
{
    if (data == NULL || length == NULL || width == 0u || width > 4u)
        return false;
    size_t value = 0;
    for (unsigned i = 0; i < width; i++) value = (value << 8u) | data[i];
    *length = value;
    return value != 0;
}

static bool h264_prefix_write(
    unsigned char *data, unsigned width, size_t length)
{
    if (data == NULL || width == 0u || width > 4u
        || (width < sizeof(size_t)
            && length >= ((size_t) 1u << (width * CHAR_BIT)))) return false;
    for (unsigned i = 0; i < width; i++) {
        unsigned shift = (width - i - 1u) * CHAR_BIT;
        data[i] = (unsigned char) (length >> shift);
    }
    return true;
}

MediaH264PspCompatResult media_h264_psp_compat_transform(
    MediaH264PspCompat *compat,
    unsigned char *sample, size_t *sample_length, size_t capacity)
{
    if (compat == NULL || sample == NULL || sample_length == NULL
        || *sample_length == 0 || *sample_length > capacity)
        return MEDIA_H264_PSP_COMPAT_ERROR;
    if (!compat->enabled) return MEDIA_H264_PSP_COMPAT_PASSTHROUGH;
    MediaH264PspCompat next = *compat;
    bool recovery_next = false, rewritten = false;
    size_t cursor = 0;
    unsigned nals = 0;
    while (cursor < *sample_length) {
        if (++nals > H264_COMPAT_MAX_NALS
            || compat->nal_length_size > *sample_length - cursor)
            return MEDIA_H264_PSP_COMPAT_ERROR;
        size_t prefix_at = cursor, nal_length = 0;
        if (!h264_prefix_read(
                sample + cursor, compat->nal_length_size, &nal_length))
            return MEDIA_H264_PSP_COMPAT_ERROR;
        cursor += compat->nal_length_size;
        if (nal_length > *sample_length - cursor)
            return MEDIA_H264_PSP_COMPAT_ERROR;
        size_t nal_at = cursor;
        unsigned nal_type = sample[nal_at] & 0x1fu;
        if (nal_type == 6u
            && h264_closed_recovery_sei(sample + nal_at, nal_length)) {
            recovery_next = true;
        } else if (recovery_next) {
            if (nal_type != 1u) return MEDIA_H264_PSP_COMPAT_ERROR;
            size_t old_length = nal_length;
            /* The header can grow or shrink. Supporting a following NAL
               would require moving the complete untrusted tail and repairing
               this loop's cursor. Current admitted YouTube streams put their
               recovery slice last; reject other shapes instead of partially
               rewriting them. */
            if (nal_at + old_length != *sample_length)
                return MEDIA_H264_PSP_COMPAT_ERROR;
            if (!h264_rewrite_recovery(
                    &next, sample + nal_at, &nal_length,
                    capacity - nal_at)) return MEDIA_H264_PSP_COMPAT_ERROR;
            ptrdiff_t delta = (ptrdiff_t) nal_length - (ptrdiff_t) old_length;
            if (delta > 0
                && (size_t) delta > capacity - *sample_length)
                return MEDIA_H264_PSP_COMPAT_ERROR;
            if (!h264_prefix_write(
                    sample + prefix_at, compat->nal_length_size, nal_length))
                return MEDIA_H264_PSP_COMPAT_ERROR;
            *sample_length = (size_t) ((ptrdiff_t) *sample_length + delta);
            next.active_gop = true;
            next.first_reference_pending = true;
            next.recovery_points_rewritten++;
            recovery_next = false;
            rewritten = true;
        } else if (nal_type == 5u) {
            media_h264_psp_compat_reset(&next);
        } else if (nal_type == 1u && next.active_gop) {
            H264Bits bits;
            H264SlicePrefix prefix;
            if (!h264_rebase_frame_num(
                    &next, sample + nal_at, nal_length, &prefix, &bits))
                return MEDIA_H264_PSP_COMPAT_ERROR;
            if (next.first_reference_pending
                && (prefix.nal_header >> 5u) != 0u) {
                size_t old_length = nal_length;
                /* See the recovery-slice restriction above: this edit is
                   variable-length and is intentionally confined to the final
                   NAL of the access unit. */
                if (nal_at + old_length != *sample_length)
                    return MEDIA_H264_PSP_COMPAT_ERROR;
                if (!h264_remove_first_reference_marking(
                        &next, sample + nal_at, &nal_length,
                        capacity - nal_at))
                    return MEDIA_H264_PSP_COMPAT_ERROR;
                ptrdiff_t delta =
                    (ptrdiff_t) nal_length - (ptrdiff_t) old_length;
                if (delta > 0
                    && (size_t) delta > capacity - *sample_length)
                    return MEDIA_H264_PSP_COMPAT_ERROR;
                if (!h264_prefix_write(
                        sample + prefix_at, compat->nal_length_size,
                        nal_length)) return MEDIA_H264_PSP_COMPAT_ERROR;
                *sample_length =
                    (size_t) ((ptrdiff_t) *sample_length + delta);
                next.first_reference_pending = false;
            }
            rewritten = true;
        }
        cursor = nal_at + nal_length;
    }
    if (cursor != *sample_length || recovery_next)
        return MEDIA_H264_PSP_COMPAT_ERROR;
    *compat = next;
    return rewritten
        ? MEDIA_H264_PSP_COMPAT_REWRITTEN
        : MEDIA_H264_PSP_COMPAT_PASSTHROUGH;
}
