#ifndef TILEFINCH_MEDIA_H264_PSP_COMPAT_H
#define TILEFINCH_MEDIA_H264_PSP_COMPAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Recovery-to-IDR rewriting changes only slice headers. This is deliberately
   larger than the largest admitted growth of one access unit, so callers can
   reserve once without sizing from untrusted syntax. */
#define MEDIA_H264_PSP_COMPAT_EXTRA_BYTES 16u

typedef enum {
    MEDIA_H264_PSP_COMPAT_PASSTHROUGH = 0,
    MEDIA_H264_PSP_COMPAT_REWRITTEN,
    MEDIA_H264_PSP_COMPAT_ERROR
} MediaH264PspCompatResult;

typedef struct {
    bool enabled;
    bool active_gop;
    bool first_reference_pending;
    uint8_t nal_length_size;
    uint8_t log2_max_frame_num;
    uint8_t log2_max_pic_order_cnt_lsb;
    uint8_t chroma_format_idc;
    uint8_t weighted_bipred_idc;
    uint8_t num_ref_idx_l0_default;
    uint8_t num_ref_idx_l1_default;
    uint32_t pps_id;
    uint32_t sps_id;
    uint32_t frame_num_base;
    bool entropy_coding_mode;
    bool bottom_field_pic_order_present;
    bool weighted_pred;
    bool redundant_pic_count_present;
    bool deblocking_filter_control_present;
    uint32_t recovery_points_rewritten;
    uint32_t reference_markings_removed;
    uint32_t frame_numbers_rebased;
} MediaH264PspCompat;

/* Pure policy helpers kept here so malformed-stream boundaries can be pinned
   without exposing the bitstream parser as a test API. */
static inline bool media_h264_psp_compat_ebsp_edit_safe(
    const unsigned char *nal, size_t length,
    size_t first_bit, unsigned bit_count)
{
    if (nal == NULL || bit_count == 0u || length > SIZE_MAX / 8u)
        return false;
    size_t total_bits = length * 8u;
    if (first_bit >= total_bits || bit_count > total_bits - first_bit)
        return false;
    size_t first_byte = first_bit / 8u;
    size_t last_byte = (first_bit + bit_count - 1u) / 8u;
    size_t at = first_byte > 2u ? first_byte - 2u : 0u;
    /* Only triplets which include an edited byte can have been introduced by
       the rewrite.  00 00 00..03 is forbidden in EBSP unless an existing
       emulation-prevention byte already separated the payload. */
    for (; at <= last_byte && at + 2u < length; at++) {
        if (nal[at] == 0u && nal[at + 1u] == 0u && nal[at + 2u] <= 3u)
            return false;
    }
    return true;
}

static inline bool media_h264_psp_compat_mmco_removable(uint32_t operation)
{
    return operation == 1u || operation == 2u;
}

/* Returns true for a supported, conservatively bounded AVC configuration.
   Unsupported streams remain valid browser media; they simply bypass this
   PSP firmware compatibility transform. */
bool media_h264_psp_compat_init(
    MediaH264PspCompat *compat,
    const unsigned char *avcc, size_t avcc_length);

void media_h264_psp_compat_reset(MediaH264PspCompat *compat);

/* Rewrites one complete AVCC access unit in place. The transform is activated
   only by a closed recovery point (recovery_frame_cnt=0, exact_match=1,
   broken_link=0) followed by an I slice. */
MediaH264PspCompatResult media_h264_psp_compat_transform(
    MediaH264PspCompat *compat,
    unsigned char *sample, size_t *sample_length, size_t capacity);

#endif
