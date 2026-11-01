#include "display_kms.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#define DIE(fmt, ...) do { fprintf(stderr, "FATAL: " fmt "\n", ##__VA_ARGS__); exit(1); } while(0)
#define DIE_IF(cond, fmt, ...) do { if (cond) DIE(fmt, ##__VA_ARGS__); } while(0)

static int plane_supports_format(drmModePlane *p, uint32_t fmt) {
    for (uint32_t i = 0; i < p->count_formats; i++) {
        if (p->formats[i] == fmt) return 1;
    }
    return 0;
}

static void resolve_plane_props(struct display_kms_ctx *ctx) {
    drmModeObjectProperties *props = drmModeObjectGetProperties(
        ctx->drm_fd, ctx->plane_id, DRM_MODE_OBJECT_PLANE);
    if (!props) return;

    for (uint32_t i = 0; i < props->count_props; i++) {
        drmModePropertyRes *p = drmModeGetProperty(ctx->drm_fd, props->props[i]);
        if (!p) continue;
        if      (strcmp(p->name, "FB_ID")   == 0) ctx->prop_fb_id   = p->prop_id;
        else if (strcmp(p->name, "CRTC_ID") == 0) ctx->prop_crtc_id = p->prop_id;
        else if (strcmp(p->name, "CRTC_X")  == 0) ctx->prop_crtc_x  = p->prop_id;
        else if (strcmp(p->name, "CRTC_Y")  == 0) ctx->prop_crtc_y  = p->prop_id;
        else if (strcmp(p->name, "CRTC_W")  == 0) ctx->prop_crtc_w  = p->prop_id;
        else if (strcmp(p->name, "CRTC_H")  == 0) ctx->prop_crtc_h  = p->prop_id;
        else if (strcmp(p->name, "SRC_X")   == 0) ctx->prop_src_x   = p->prop_id;
        else if (strcmp(p->name, "SRC_Y")   == 0) ctx->prop_src_y   = p->prop_id;
        else if (strcmp(p->name, "SRC_W")   == 0) ctx->prop_src_w   = p->prop_id;
        else if (strcmp(p->name, "SRC_H")   == 0) ctx->prop_src_h   = p->prop_id;
        drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(props);

    if (ctx->prop_fb_id && ctx->prop_crtc_id &&
        ctx->prop_crtc_w && ctx->prop_crtc_h &&
        ctx->prop_src_w && ctx->prop_src_h) {
        ctx->atomic_ready = 1;
    }
}

static void fb_cache_invalidate_all(struct display_kms_ctx *ctx) {
    for (int i = 0; i < KMS_FB_CACHE_SIZE; i++) {
        if (ctx->fb_cache[i].valid) {
            if (ctx->fb_cache[i].fb_id) {
                drmModeRmFB(ctx->drm_fd, ctx->fb_cache[i].fb_id);
            }
            if (ctx->fb_cache[i].gem_handle) {
                struct drm_gem_close cl = { .handle = ctx->fb_cache[i].gem_handle };
                drmIoctl(ctx->drm_fd, DRM_IOCTL_GEM_CLOSE, &cl);
            }
            memset(&ctx->fb_cache[i], 0, sizeof(ctx->fb_cache[i]));
        }
    }
}

static uint32_t fb_cache_get_or_create(struct display_kms_ctx *ctx,
                                       const AVDRMFrameDescriptor *desc,
                                       int width, int height) {
    int dmabuf_fd = desc->objects[0].fd;
    uint32_t drm_format = desc->layers[0].format;

    /* 1. Linear lookup by dmabuf_fd */
    for (int i = 0; i < KMS_FB_CACHE_SIZE; i++) {
        struct kms_fb_cache_entry *e = &ctx->fb_cache[i];
        if (e->valid && e->dmabuf_fd == dmabuf_fd) {
            if (e->width == width && e->height == height && e->drm_format == drm_format) {
                ctx->cache_hits++;
                return e->fb_id;
            }
            /* Resolution or format changed on this fd -> invalidate entire cache */
            fb_cache_invalidate_all(ctx);
            break;
        }
    }

    /* 2. Cache miss: import DMA-BUF and create persistent DRM FB */
    uint32_t gem_handle = 0;
    if (drmPrimeFDToHandle(ctx->drm_fd, dmabuf_fd, &gem_handle) < 0) {
        return 0;
    }

    uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};
    int nplanes = desc->layers[0].nb_planes;
    for (int i = 0; i < nplanes && i < 4; i++) {
        handles[i] = gem_handle;
        pitches[i] = desc->layers[0].planes[i].pitch;
        offsets[i] = desc->layers[0].planes[i].offset;
    }

    uint32_t fb_id = 0;
    if (drmModeAddFB2(ctx->drm_fd, width, height, drm_format,
                      handles, pitches, offsets, &fb_id, 0) < 0) {
        struct drm_gem_close cl = { .handle = gem_handle };
        drmIoctl(ctx->drm_fd, DRM_IOCTL_GEM_CLOSE, &cl);
        return 0;
    }

    ctx->cache_misses++;

    for (int i = 0; i < KMS_FB_CACHE_SIZE; i++) {
        if (!ctx->fb_cache[i].valid) {
            ctx->fb_cache[i].dmabuf_fd  = dmabuf_fd;
            ctx->fb_cache[i].gem_handle = gem_handle;
            ctx->fb_cache[i].fb_id      = fb_id;
            ctx->fb_cache[i].width      = width;
            ctx->fb_cache[i].height     = height;
            ctx->fb_cache[i].drm_format = drm_format;
            ctx->fb_cache[i].valid      = 1;
            return fb_id;
        }
    }

    fb_cache_invalidate_all(ctx);
    return fb_cache_get_or_create(ctx, desc, width, height);
}

void display_kms_init(struct display_kms_ctx *ctx) {
    memset(ctx, 0, sizeof(*ctx));

    ctx->drm_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (ctx->drm_fd < 0) ctx->drm_fd = open("/dev/dri/card1", O_RDWR | O_CLOEXEC);
    DIE_IF(ctx->drm_fd < 0, "Cannot open /dev/dri/card0 or card1: %s", strerror(errno));

    drmSetClientCap(ctx->drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    int atomic_cap = (drmSetClientCap(ctx->drm_fd, DRM_CLIENT_CAP_ATOMIC, 1) == 0);

    drmModeRes *res = drmModeGetResources(ctx->drm_fd);
    DIE_IF(!res, "drmModeGetResources failed");

    drmModeModeInfo mode = {0};
    int crtc_idx = -1;

    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *c = drmModeGetConnector(ctx->drm_fd, res->connectors[i]);
        if (!c) continue;
        if (c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) {
            ctx->conn_id = c->connector_id;
            mode = c->modes[0];
            if (c->encoder_id) {
                drmModeEncoder *enc = drmModeGetEncoder(ctx->drm_fd, c->encoder_id);
                if (enc) {
                    ctx->crtc_id = enc->crtc_id;
                    drmModeFreeEncoder(enc);
                }
            }
            if (!ctx->crtc_id && res->count_crtcs > 0) {
                ctx->crtc_id = res->crtcs[0];
            }
            drmModeFreeConnector(c);
            break;
        }
        drmModeFreeConnector(c);
    }
    DIE_IF(!ctx->conn_id || !ctx->crtc_id, "No active HDMI/DSI connector + CRTC found");

    for (int i = 0; i < res->count_crtcs; i++) {
        if (res->crtcs[i] == ctx->crtc_id) { crtc_idx = i; break; }
    }
    drmModeFreeResources(res);

    ctx->disp_w = mode.hdisplay;
    ctx->disp_h = mode.vdisplay;

    drmModePlaneRes *pres = drmModeGetPlaneResources(ctx->drm_fd);
    DIE_IF(!pres, "drmModeGetPlaneResources failed");

    for (uint32_t i = 0; i < pres->count_planes; i++) {
        drmModePlane *p = drmModeGetPlane(ctx->drm_fd, pres->planes[i]);
        if (!p) continue;
        if ((p->possible_crtcs & (1u << crtc_idx)) &&
            (plane_supports_format(p, DRM_FORMAT_YUV420) ||
             plane_supports_format(p, DRM_FORMAT_NV12))) {
            ctx->plane_id = p->plane_id;
            if (p->crtc_id == 0) {
                drmModeFreePlane(p);
                break;
            }
        }
        drmModeFreePlane(p);
    }
    drmModeFreePlaneResources(pres);
    DIE_IF(!ctx->plane_id, "No KMS plane supporting YUV420/NV12 found on CRTC %u", ctx->crtc_id);

    if (atomic_cap) {
        resolve_plane_props(ctx);
    }

    printf("[KMS] Connected %ux%u@%uHz (Conn=%u, CRTC=%u, Plane=%u, Atomic=%s)\n",
           ctx->disp_w, ctx->disp_h, mode.vrefresh,
           ctx->conn_id, ctx->crtc_id, ctx->plane_id,
           ctx->atomic_ready ? "yes" : "legacy");
}

int display_kms_present(struct display_kms_ctx *ctx, AVFrame *frame) {
    if (!frame || frame->format != AV_PIX_FMT_DRM_PRIME) {
        if (frame) av_frame_free(&frame);
        return -1;
    }

    AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor *)frame->data[0];
    if (!desc || desc->nb_objects < 1) {
        av_frame_free(&frame);
        return -1;
    }

    uint32_t fb_id = fb_cache_get_or_create(ctx, desc, frame->width, frame->height);
    if (! fb_id) {
        av_frame_free(&frame);
        return -1;
    }

    if (ctx->cache_misses == 1 && ctx->cache_hits == 0) {
        printf("[KMS] Presenting %dx%d stream on Plane %u (FB=%u)\n",
               frame->width, frame->height, ctx->plane_id, fb_id);
        fflush(stdout);
    }

    int committed = 0;

    if (ctx->atomic_ready) {
        drmModeAtomicReq *req = drmModeAtomicAlloc();
        if (req) {
            drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_fb_id,   fb_id);
            drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_crtc_id, ctx->crtc_id);
            drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_crtc_x,  0);
            drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_crtc_y,  0);
            drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_crtc_w,  ctx->disp_w);
            drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_crtc_h,  ctx->disp_h);
            drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_src_x,   0);
            drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_src_y,   0);
            drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_src_w,   (uint64_t)frame->width  << 16);
            drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_src_h,   (uint64_t)frame->height << 16);

            uint32_t flags = ctx->plane_activated ? DRM_MODE_ATOMIC_NONBLOCK : 0;
            int ret = drmModeAtomicCommit(ctx->drm_fd, req, flags, NULL);
            if (ret == 0) {
                ctx->plane_activated = 1;
                if (flags & DRM_MODE_ATOMIC_NONBLOCK) ctx->nonblock_commits++;
                committed = 1;
            } else if (ret == -EBUSY) {
                ctx->ebusy_count++;
                ret = drmModeAtomicCommit(ctx->drm_fd, req, 0, NULL);
                if (ret == 0) committed = 1;
            } else if (!ctx->plane_activated) {
                ret = drmModeAtomicCommit(ctx->drm_fd, req, 0, NULL);
                if (ret == 0) {
                    ctx->plane_activated = 1;
                    committed = 1;
                }
            }
            drmModeAtomicFree(req);
        }
    }

    if (!committed) {
        drmModeSetPlane(ctx->drm_fd, ctx->plane_id, ctx->crtc_id, fb_id, 0,
                        0, 0, ctx->disp_w, ctx->disp_h,
                        0, 0, (uint32_t)frame->width << 16, (uint32_t)frame->height << 16);
        ctx->plane_activated = 1;
    }

    if (ctx->frame_ring[ctx->ring_idx]) {
        av_frame_free(&ctx->frame_ring[ctx->ring_idx]);
    }
    ctx->frame_ring[ctx->ring_idx] = frame;
    ctx->ring_idx = (ctx->ring_idx + 1) % KMS_FRAME_RING;

    return 0;
}

void display_kms_disable_plane(struct display_kms_ctx *ctx) {
    if (ctx->drm_fd < 0) return;

    int disabled = 0;
    if (ctx->atomic_ready && ctx->plane_activated) {
        drmModeAtomicReq *req = drmModeAtomicAlloc();
        if (req) {
            drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_fb_id,   0);
            drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_crtc_id, 0);
            if (drmModeAtomicCommit(ctx->drm_fd, req, 0, NULL) == 0) {
                disabled = 1;
            }
            drmModeAtomicFree(req);
        }
    }
    if (!disabled && ctx->plane_activated) {
        drmModeSetPlane(ctx->drm_fd, ctx->plane_id, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    }
    ctx->plane_activated = 0;

    /*
     * Always invalidate fb_cache and release frame_ring BEFORE V4L2 buffers
     * are closed/reallocated so the kernel does not reuse the same dmabuf_fd
     * integer for a new buffer while fb_cache still maps it to an old fb_id.
     */
    fb_cache_invalidate_all(ctx);
    ctx->cache_hits   = 0;
    ctx->cache_misses = 0;
    for (int i = 0; i < KMS_FRAME_RING; i++) {
        if (ctx->frame_ring[i]) av_frame_free(&ctx->frame_ring[i]);
    }
    ctx->ring_idx = 0;
}

void display_kms_cleanup(struct display_kms_ctx *ctx) {
    display_kms_disable_plane(ctx);
    if (ctx->drm_fd >= 0) {
        close(ctx->drm_fd);
        ctx->drm_fd = -1;
    }
}
