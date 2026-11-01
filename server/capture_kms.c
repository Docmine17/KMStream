#include "capture_kms.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#define DIE_IF(cond, fmt, ...) do { \
    if (cond) { \
        fprintf(stderr, "[kmstreamd] FATAL (%s:%d): " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
        exit(1); \
    } \
} while (0)

static void get_plane_atomic_info(int drm_fd, uint32_t plane_id, uint64_t *type,
                                  uint32_t *fb_id, uint32_t *crtc_id, int32_t *crtc_x, int32_t *crtc_y) {
    *type = 0; *fb_id = 0; *crtc_id = 0; *crtc_x = 0; *crtc_y = 0;
    drmModeObjectProperties *props = drmModeObjectGetProperties(drm_fd, plane_id, DRM_MODE_OBJECT_PLANE);
    if (!props) return;
    for (uint32_t i = 0; i < props->count_props; i++) {
        drmModePropertyRes *prop = drmModeGetProperty(drm_fd, props->props[i]);
        if (!prop) continue;
        uint64_t val = props->prop_values[i];
        if (strcmp(prop->name, "type") == 0) *type = val;
        else if (strcmp(prop->name, "FB_ID") == 0) *fb_id = (uint32_t)val;
        else if (strcmp(prop->name, "CRTC_ID") == 0) *crtc_id = (uint32_t)val;
        else if (strcmp(prop->name, "CRTC_X") == 0) *crtc_x = (int32_t)val;
        else if (strcmp(prop->name, "CRTC_Y") == 0) *crtc_y = (int32_t)val;
        drmModeFreeProperty(prop);
    }
    drmModeFreeObjectProperties(props);
}

void kms_capture_init(struct kms_capture_ctx *ctx, const char *card_path) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->active_crtc_idx = -1;

    ctx->drm_fd = open(card_path, O_RDWR | O_CLOEXEC);
    DIE_IF(ctx->drm_fd < 0, "open(%s): %s (run as root or 'make caps')", card_path, strerror(errno));
    DIE_IF(drmSetClientCap(ctx->drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) < 0,
           "DRM_CLIENT_CAP_UNIVERSAL_PLANES failed");
    DIE_IF(drmSetClientCap(ctx->drm_fd, DRM_CLIENT_CAP_ATOMIC, 1) < 0,
           "DRM_CLIENT_CAP_ATOMIC failed");

    drmModeRes *res = drmModeGetResources(ctx->drm_fd);
    DIE_IF(!res, "drmModeGetResources failed");
    drmModePlaneRes *plane_res = drmModeGetPlaneResources(ctx->drm_fd);
    DIE_IF(!plane_res, "drmModeGetPlaneResources failed");

    uint32_t fb_id = 0;
    for (uint32_t i = 0; i < plane_res->count_planes; i++) {
        uint64_t type; uint32_t f, c; int32_t cx, cy;
        get_plane_atomic_info(ctx->drm_fd, plane_res->planes[i], &type, &f, &c, &cx, &cy);
        if (type == DRM_PLANE_TYPE_PRIMARY && f != 0 && c != 0 && !ctx->primary_plane_id) {
            ctx->primary_plane_id = plane_res->planes[i];
            ctx->active_crtc_id   = c;
            fb_id                 = f;
        }
    }
    DIE_IF(!ctx->primary_plane_id || !fb_id, "no active primary DRM plane found on %s", card_path);

    for (int i = 0; i < res->count_crtcs; i++) {
        if (res->crtcs[i] == ctx->active_crtc_id) {
            ctx->active_crtc_idx = i;
            break;
        }
    }
    drmModeFreeResources(res);

    for (uint32_t i = 0; i < plane_res->count_planes; i++) {
        uint64_t type; uint32_t f, c; int32_t cx, cy;
        get_plane_atomic_info(ctx->drm_fd, plane_res->planes[i], &type, &f, &c, &cx, &cy);
        if (type == DRM_PLANE_TYPE_CURSOR) {
            drmModePlane *p = drmModeGetPlane(ctx->drm_fd, plane_res->planes[i]);
            if (p && ctx->active_crtc_idx >= 0 && (p->possible_crtcs & (1 << ctx->active_crtc_idx))) {
                ctx->cursor_plane_id = plane_res->planes[i];
                drmModeFreePlane(p);
                break;
            }
            if (p) drmModeFreePlane(p);
        }
    }
    drmModeFreePlaneResources(plane_res);

    drmModeFB2 *fb0 = drmModeGetFB2(ctx->drm_fd, fb_id);
    DIE_IF(!fb0, "drmModeGetFB2 failed (need CAP_SYS_ADMIN)");
    ctx->src_w = (int)fb0->width;
    ctx->src_h = (int)fb0->height;
    drmModeFreeFB2(fb0);
}

int kms_capture_wait_vblank(struct kms_capture_ctx *ctx) {
    uint64_t seq = 0;
    if (drmCrtcQueueSequence(ctx->drm_fd, ctx->active_crtc_id,
                             DRM_CRTC_SEQUENCE_RELATIVE, 1, &seq, 0) == 0) {
        drmEventContext ev = { .version = DRM_EVENT_CONTEXT_VERSION };
        drmHandleEvent(ctx->drm_fd, &ev);
        return 0;
    }
    return -1;
}

int kms_capture_acquire(struct kms_capture_ctx *ctx,
                        struct kms_captured_plane *prim,
                        struct kms_captured_plane *cur) {
    memset(prim, 0, sizeof(*prim));
    memset(cur,  0, sizeof(*cur));
    prim->dmabuf_fd = -1;
    cur->dmabuf_fd  = -1;

    drmModePlane *p = drmModeGetPlane(ctx->drm_fd, ctx->primary_plane_id);
    if (!p || p->fb_id == 0) {
        if (p) drmModeFreePlane(p);
        return -1;
    }
    uint32_t fb_id = p->fb_id;
    drmModeFreePlane(p);

    drmModeFB2 *fb = drmModeGetFB2(ctx->drm_fd, fb_id);
    if (!fb) return -1;
    DIE_IF(fb->handles[0] == 0 && ++ctx->handle_fail_cnt >= 3,
           "drmModeGetFB2 returned handle=0 (CAP_SYS_ADMIN required: run with sudo or 'make caps')");
    if (fb->handles[0] == 0) {
        drmModeFreeFB2(fb);
        return -1;
    }

    int fd = -1;
    if (drmPrimeHandleToFD(ctx->drm_fd, fb->handles[0], DRM_CLOEXEC | DRM_RDWR, &fd) < 0) {
        drmModeFreeFB2(fb);
        return -1;
    }
    prim->fb        = fb;
    prim->dmabuf_fd = fd;

    if (ctx->cursor_plane_id) {
        uint64_t ctype; uint32_t cfb_id, ccrtc_id; int32_t cx, cy;
        get_plane_atomic_info(ctx->drm_fd, ctx->cursor_plane_id, &ctype, &cfb_id, &ccrtc_id, &cx, &cy);
        if (cfb_id != 0) {
            drmModeFB2 *cfb = drmModeGetFB2(ctx->drm_fd, cfb_id);
            int cfd = -1;
            if (cfb && cfb->handles[0] != 0 &&
                drmPrimeHandleToFD(ctx->drm_fd, cfb->handles[0], DRM_CLOEXEC | DRM_RDWR, &cfd) == 0) {
                cur->fb        = cfb;
                cur->dmabuf_fd = cfd;
                cur->crtc_x    = cx;
                cur->crtc_y    = cy;
            } else if (cfb) {
                drmModeFreeFB2(cfb);
            }
        }
    }
    return 0;
}

void kms_capture_release_plane(struct kms_captured_plane *pl) {
    if (pl->dmabuf_fd >= 0) {
        close(pl->dmabuf_fd);
        pl->dmabuf_fd = -1;
    }
    if (pl->fb) {
        drmModeFreeFB2(pl->fb);
        pl->fb = NULL;
    }
}
