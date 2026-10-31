#ifndef KMSTREAM_SPS_PATCH_H
#define KMSTREAM_SPS_PATCH_H

#include <stdint.h>

/*
 * Scans an H.264 Annex-B packet for SPS NAL units (type 7) and rewrites
 * their VUI with bitstream_restriction_flag=1, max_num_reorder_frames=0,
 * and max_dec_frame_buffering=1 so the Raspberry Pi VideoCore IV V4L2 M2M
 * decoder (/dev/video10) outputs each frame immediately without holding
 * 1-3 frames in the reorder queue.
 *
 * Returns the total size written to out_buf, and sets *patched_flag = 1
 * if an SPS was found and rewritten.
 */
int sps_patch_packet(const uint8_t *in_buf, int in_size,
                     uint8_t *out_buf, int max_out, int *patched_flag);

#endif /* KMSTREAM_SPS_PATCH_H */
