#ifndef KMSTREAM_DISPLAY_KMS_H
#define KMSTREAM_DISPLAY_KMS_H

#include <stdint.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext_drm.h>

#define KMS_FB_CACHE_SIZE 16
#define KMS_FRAME_RING    3

struct kms_fb_cache_entry {
    int      dmabuf_fd;
    uint32_t gem_handle;
    uint32_t fb_id;
    int      width;
    int      height;
    uint32_t drm_format;
    int      valid;
};

struct display_kms_ctx {
    int      drm_fd;
    uint32_t conn_id;
    uint32_t crtc_id;
    uint32_t plane_id;
    uint32_t disp_w;
    uint32_t disp_h;

    /* Cached atomic property IDs for plane_id */
    uint32_t prop_fb_id;
    uint32_t prop_crtc_id;
    uint32_t prop_crtc_x;
    uint32_t prop_crtc_y;
    uint32_t prop_crtc_w;
    uint32_t prop_crtc_h;
    uint32_t prop_src_x;
    uint32_t prop_src_y;
    uint32_t prop_src_w;
    uint32_t prop_src_h;
    int      atomic_ready;
    int      plane_activated;

    /* Persistent FB cache indexed linearly by dmabuf_fd */
    struct kms_fb_cache_entry fb_cache[KMS_FB_CACHE_SIZE];
    uint32_t cache_hits;
    uint32_t cache_misses;
    uint32_t nonblock_commits;
    uint32_t ebusy_count;

    /* Ring of referenced AVFrames to prevent V4L2 buffer reuse during scanout */
    AVFrame *frame_ring[KMS_FRAME_RING];
    int      ring_idx;
};

void display_kms_init(struct display_kms_ctx *ctx);
int  display_kms_present(struct display_kms_ctx *ctx, AVFrame *frame);
void display_kms_disable_plane(struct display_kms_ctx *ctx);
void display_kms_cleanup(struct display_kms_ctx *ctx);

#endif /* KMSTREAM_DISPLAY_KMS_H */
