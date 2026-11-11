#ifndef KMSTREAM_DISPLAY_KMS_H
#define KMSTREAM_DISPLAY_KMS_H

#include <stdint.h>
#include <pthread.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include "decode_v4l2.h"

struct display_kms_ctx {
    int                   drm_fd;
    uint32_t              conn_id;
    uint32_t              crtc_id;
    uint32_t              plane_id;
    uint32_t              mode_blob_id;
    uint32_t              orig_crtc_fb_id;
    int                   needs_modeset;

    int                   crtc_width;
    int                   crtc_height;
    double                crtc_hz;
    uint32_t              crtc_vrefresh;
    char                  conn_name[32];
    char                  driver_name[64];
    int                   dst_x;
    int                   dst_y;
    int                   dst_w;
    int                   dst_h;
    uint32_t              visible_width;
    uint32_t              visible_height;

    drmModeAtomicReqPtr   atomic_req;

    /* Connector & CRTC atomic property IDs */
    uint32_t              prop_conn_crtc_id;
    uint32_t              prop_crtc_mode_id;
    uint32_t              prop_crtc_active;

    /* Plane atomic property IDs */
    uint32_t              prop_fb_id;
    uint32_t              prop_crtc_id;
    uint32_t              prop_crtc_x;
    uint32_t              prop_crtc_y;
    uint32_t              prop_crtc_w;
    uint32_t              prop_crtc_h;
    uint32_t              prop_src_x;
    uint32_t              prop_src_y;
    uint32_t              prop_src_w;
    uint32_t              prop_src_h;
    uint32_t              prop_color_encoding;
    uint32_t              prop_color_range;
    uint64_t              color_encoding_bt709;
    uint64_t              color_range_limited;

    /* VBlank-synchronized presentation mailbox */
    pthread_mutex_t       drm_lock;
    int                   flip_pending;
    int                   current_displayed_idx;
    int                   pending_flip_idx;
    int                   staged_frame_idx;
    uint32_t              flips_completed;
    uint32_t              staged_replacements;
    uint64_t              last_flip_wait_us;
    uint64_t              last_flip_ts_us;
    uint32_t              last_flipped_frame_id;

    struct decode_v4l2_ctx *dec;
};

void display_kms_init(struct display_kms_ctx *ctx, struct decode_v4l2_ctx *dec);
void display_kms_update_geometry(struct display_kms_ctx *ctx, uint32_t vis_w, uint32_t vis_h);
int  display_kms_import_capture_buf(struct display_kms_ctx *ctx, struct decode_v4l2_ctx *dec, int index);
void display_kms_release_capture_bufs(struct display_kms_ctx *ctx, struct decode_v4l2_ctx *dec);
void display_kms_present_idx(struct display_kms_ctx *ctx, int new_buf_idx);
void display_kms_page_flip_handler(int fd, unsigned int seq, unsigned int tv_sec,
                                   unsigned int tv_usec, void *user_data);
void display_kms_drain_pending_flip(struct display_kms_ctx *ctx);
void display_kms_disable_plane_sync(struct display_kms_ctx *ctx, int restore_fbcon);
void display_kms_cleanup(struct display_kms_ctx *ctx, struct decode_v4l2_ctx *dec);

#endif /* KMSTREAM_DISPLAY_KMS_H */
