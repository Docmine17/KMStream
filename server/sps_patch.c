#include "sps_patch.h"
#include <string.h>

/*
 * H.264 SPS header rewriter (MattKC / Vanilla Wii U zero-latency fix).
 * Ensures every SPS NAL unit (Type 7) sent to the Raspberry Pi 3B sets:
 *   vui_parameters_present_flag = 1
 *   bitstream_restriction_flag  = 1
 *   max_num_reorder_frames      = 0
 *   max_dec_frame_buffering     = 1
 * This prevents the VideoCore IV firmware (/dev/video10) from holding 1-3 frames.
 */

typedef struct {
    const uint8_t *buf;
    int size_bits;
    int bit_pos;
} BitReader;

typedef struct {
    uint8_t data[256];
    int bit_pos;
} BitWriter;

static inline uint32_t br_get_bit(BitReader *r) {
    if (r->bit_pos >= r->size_bits) return 0;
    uint32_t b = (r->buf[r->bit_pos >> 3] >> (7 - (r->bit_pos & 7))) & 1;
    r->bit_pos++;
    return b;
}

static inline void bw_put_bit(BitWriter *w, uint32_t b) {
    if ((w->bit_pos >> 3) >= (int)sizeof(w->data)) return;
    if (b) w->data[w->bit_pos >> 3] |= (1 << (7 - (w->bit_pos & 7)));
    else   w->data[w->bit_pos >> 3] &= ~(1 << (7 - (w->bit_pos & 7)));
    w->bit_pos++;
}

static inline uint32_t br_copy_bits(BitReader *r, BitWriter *w, int n) {
    uint32_t val = 0;
    for (int i = 0; i < n; i++) {
        uint32_t b = br_get_bit(r);
        bw_put_bit(w, b);
        val = (val << 1) | b;
    }
    return val;
}

static inline uint32_t br_copy_ue(BitReader *r, BitWriter *w) {
    int zeros = 0;
    while (r->bit_pos < r->size_bits && br_get_bit(r) == 0) {
        bw_put_bit(w, 0);
        zeros++;
        if (zeros > 30) break;
    }
    bw_put_bit(w, 1);
    uint32_t info = (zeros > 0) ? br_copy_bits(r, w, zeros) : 0;
    return ((1U << zeros) - 1U) + info;
}

static inline void bw_write_ue(BitWriter *w, uint32_t val) {
    uint32_t code = val + 1;
    int len = 0;
    for (uint32_t tmp = code; tmp > 0; tmp >>= 1) len++;
    for (int i = 0; i < len - 1; i++) bw_put_bit(w, 0);
    for (int i = len - 1; i >= 0; i--) bw_put_bit(w, (code >> i) & 1);
}

static inline void bw_put_bits(BitWriter *w, uint32_t val, int n) {
    for (int i = n - 1; i >= 0; i--) {
        bw_put_bit(w, (val >> i) & 1);
    }
}

/* Rewrites SPS RBSP, stripping existing VUI and injecting a minimal zero-latency + BT.709 VUI. */
static int rewrite_sps_zero_latency(const uint8_t *nal_in, int nal_len, uint8_t *nal_out) {
    uint8_t rbsp[256];
    int rbsp_len = 0;
    for (int i = 0; i < nal_len && rbsp_len < (int)sizeof(rbsp); i++) {
        if (i >= 2 && nal_in[i] == 0x03 && nal_in[i - 1] == 0x00 && nal_in[i - 2] == 0x00) continue;
        rbsp[rbsp_len++] = nal_in[i];
    }

    BitReader r = { .buf = rbsp, .size_bits = rbsp_len * 8, .bit_pos = 0 };
    BitWriter w;
    memset(&w, 0, sizeof(w));

    uint32_t nal_hdr = br_copy_bits(&r, &w, 8);
    if ((nal_hdr & 0x1F) != 7) return 0;

    uint32_t profile_idc = br_copy_bits(&r, &w, 8);
    br_copy_bits(&r, &w, 8); /* constraint_set flags */
    br_copy_bits(&r, &w, 8); /* level_idc */
    br_copy_ue(&r, &w);      /* seq_parameter_set_id */

    if (profile_idc == 100 || profile_idc == 110 || profile_idc == 122 || profile_idc == 244 ||
        profile_idc == 44  || profile_idc == 83  || profile_idc == 86  || profile_idc == 118 ||
        profile_idc == 128 || profile_idc == 138 || profile_idc == 139 || profile_idc == 134) {
        uint32_t chroma = br_copy_ue(&r, &w);
        if (chroma == 3) br_copy_bits(&r, &w, 1);
        br_copy_ue(&r, &w);
        br_copy_ue(&r, &w);
        br_copy_bits(&r, &w, 1);
        if (br_copy_bits(&r, &w, 1)) {
            int cnt = (chroma != 3) ? 8 : 12;
            for (int i = 0; i < cnt; i++) {
                if (br_copy_bits(&r, &w, 1)) {
                    int sz = (i < 6) ? 16 : 64;
                    int last = 8, next = 8;
                    for (int j = 0; j < sz; j++) {
                        if (next != 0) {
                            uint32_t delta = br_copy_ue(&r, &w);
                            int d = (delta & 1) ? (int)((delta + 1) >> 1) : -(int)(delta >> 1);
                            next = (last + d + 256) % 256;
                        }
                        last = (next == 0) ? last : next;
                    }
                }
            }
        }
    }

    br_copy_ue(&r, &w); /* log2_max_frame_num_minus4 */
    uint32_t poc_type = br_copy_ue(&r, &w);
    if (poc_type == 0) {
        br_copy_ue(&r, &w);
    } else if (poc_type == 1) {
        br_copy_bits(&r, &w, 1);
        br_copy_ue(&r, &w);
        br_copy_ue(&r, &w);
        uint32_t cycles = br_copy_ue(&r, &w);
        for (uint32_t i = 0; i < cycles; i++) br_copy_ue(&r, &w);
    }

    /* Force max_num_ref_frames = 1 */
    uint32_t num_ref = br_get_bit(&r);
    (void)num_ref;
    r.bit_pos--;
    int z = 0;
    while (r.bit_pos < r.size_bits && br_get_bit(&r) == 0) z++;
    for (int i = 0; i < z; i++) br_get_bit(&r);
    bw_write_ue(&w, 1); /* max_num_ref_frames = 1 */

    br_copy_bits(&r, &w, 1); /* gaps_in_frame_num_value_allowed_flag */
    br_copy_ue(&r, &w);      /* pic_width_in_mbs_minus1 */
    br_copy_ue(&r, &w);      /* pic_height_in_map_units_minus1 */
    uint32_t frame_mbs_only = br_copy_bits(&r, &w, 1);
    if (!frame_mbs_only) br_copy_bits(&r, &w, 1);
    br_copy_bits(&r, &w, 1); /* direct_8x8_inference_flag */
    if (br_copy_bits(&r, &w, 1)) { /* frame_cropping_flag */
        br_copy_ue(&r, &w);
        br_copy_ue(&r, &w);
        br_copy_ue(&r, &w);
        br_copy_ue(&r, &w);
    }

    /* Discard original VUI and write clean VUI with BT.709 + bitstream_restriction_flag = 1 */
    bw_put_bit(&w, 1);     /* vui_parameters_present_flag = 1 */
    bw_put_bit(&w, 0);     /* aspect_ratio_info_present_flag = 0 */
    bw_put_bit(&w, 0);     /* overscan_info_present_flag = 0 */
    bw_put_bit(&w, 1);     /* video_signal_type_present_flag = 1 */
    bw_put_bits(&w, 5, 3); /* video_format = 5 (unspecified) */
    bw_put_bit(&w, 0);     /* video_full_range_flag = 0 (limited range 16..235) */
    bw_put_bit(&w, 1);     /* colour_description_present_flag = 1 */
    bw_put_bits(&w, 1, 8); /* colour_primaries = 1 (BT.709) */
    bw_put_bits(&w, 1, 8); /* transfer_characteristics = 1 (BT.709) */
    bw_put_bits(&w, 1, 8); /* matrix_coefficients = 1 (BT.709) */
    bw_put_bit(&w, 0);     /* chroma_loc_info_present_flag = 0 */
    bw_put_bit(&w, 0);     /* timing_info_present_flag = 0 */
    bw_put_bit(&w, 0);     /* nal_hrd_parameters_present_flag = 0 */
    bw_put_bit(&w, 0);     /* vcl_hrd_parameters_present_flag = 0 */
    bw_put_bit(&w, 0);     /* pic_struct_present_flag = 0 */
    bw_put_bit(&w, 1);     /* bitstream_restriction_flag = 1 */
    bw_put_bit(&w, 1);     /* motion_vectors_over_pic_boundaries_flag = 1 */
    bw_write_ue(&w, 0);    /* max_bytes_per_pic_denom = 0 */
    bw_write_ue(&w, 0);    /* max_bits_per_mb_denom = 0 */
    bw_write_ue(&w, 10);   /* log2_max_mv_length_horizontal = 10 */
    bw_write_ue(&w, 10);   /* log2_max_mv_length_vertical = 10 */
    bw_write_ue(&w, 0);    /* max_num_reorder_frames = 0 (ZERO REORDER) */
    bw_write_ue(&w, 1);    /* max_dec_frame_buffering = 1 (IMMEDIATE V4L2 RELEASE) */

    /* RBSP trailing bits (stop bit 1 + zero padding to byte boundary) */
    bw_put_bit(&w, 1);
    while ((w.bit_pos & 7) != 0) bw_put_bit(&w, 0);

    int raw_bytes = w.bit_pos >> 3;
    int out_len = 0;
    int zero_cnt = 0;
    for (int i = 0; i < raw_bytes; i++) {
        if (zero_cnt == 2 && w.data[i] <= 0x03) {
            nal_out[out_len++] = 0x03; /* Emulation prevention byte */
            zero_cnt = 0;
        }
        nal_out[out_len++] = w.data[i];
        if (w.data[i] == 0x00) zero_cnt++;
        else zero_cnt = 0;
    }
    return out_len;
}

int sps_patch_packet(const uint8_t *in_buf, int in_size,
                     uint8_t *out_buf, int max_out, int *patched_flag) {
    *patched_flag = 0;
    int i = 0, out_pos = 0;
    while (i < in_size - 4) {
        int sc_len = 0;
        if (in_buf[i] == 0 && in_buf[i + 1] == 0 && in_buf[i + 2] == 1) sc_len = 3;
        else if (in_buf[i] == 0 && in_buf[i + 1] == 0 && in_buf[i + 2] == 0 && in_buf[i + 3] == 1) sc_len = 4;

        if (sc_len > 0) {
            uint8_t nal_type = in_buf[i + sc_len] & 0x1F;
            if (nal_type == 7) {
                int nal_start = i + sc_len;
                int nal_end = in_size;
                for (int j = nal_start; j < in_size - 3; j++) {
                    if (in_buf[j] == 0 && in_buf[j + 1] == 0 &&
                        (in_buf[j + 2] == 1 || (in_buf[j + 2] == 0 && in_buf[j + 3] == 1))) {
                        nal_end = j;
                        break;
                    }
                }
                uint8_t new_sps[300];
                int new_sps_len = rewrite_sps_zero_latency(in_buf + nal_start, nal_end - nal_start, new_sps);
                if (new_sps_len > 0 && out_pos + sc_len + new_sps_len + (in_size - nal_end) < max_out) {
                    memcpy(out_buf + out_pos, in_buf + i, sc_len);
                    out_pos += sc_len;
                    memcpy(out_buf + out_pos, new_sps, new_sps_len);
                    out_pos += new_sps_len;
                    memcpy(out_buf + out_pos, in_buf + nal_end, in_size - nal_end);
                    out_pos += (in_size - nal_end);
                    *patched_flag = 1;
                    return out_pos;
                }
            }
        }
        if (out_pos >= max_out) break;
        out_buf[out_pos++] = in_buf[i++];
    }
    while (i < in_size && out_pos < max_out) out_buf[out_pos++] = in_buf[i++];
    return out_pos;
}
