#ifndef KMSTREAM_CAPTURE_KMS_H
#define KMSTREAM_CAPTURE_KMS_H

#include <stdint.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

struct kms_capture_ctx {
    int      drm_fd;
    uint32_t primary_plane_id;
    uint32_t cursor_plane_id;
    uint32_t active_crtc_id;
    int      active_crtc_idx;
    int      src_w;
    int      src_h;
    double   src_hz;
    uint32_t src_vrefresh;
    char     conn_name[32];
    char     driver_name[64];
    uint32_t handle_fail_cnt;
};

struct kms_captured_plane {
    drmModeFB2 *fb;
    int         dmabuf_fd;
    int32_t     crtc_x;
    int32_t     crtc_y;
};

void kms_capture_init(struct kms_capture_ctx *ctx, const char *card_path);
int  kms_capture_acquire(struct kms_capture_ctx *ctx,
                         struct kms_captured_plane *prim,
                         struct kms_captured_plane *cur);
void kms_capture_release_plane(struct kms_captured_plane *pl);
void kms_capture_cleanup(struct kms_capture_ctx *ctx);

#endif /* KMSTREAM_CAPTURE_KMS_H */
