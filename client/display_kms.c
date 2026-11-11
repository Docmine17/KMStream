#include "display_kms.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <time.h>
#include <sys/ioctl.h>
#include <drm_fourcc.h>

#define DIE(fmt, ...) do { fprintf(stderr, "[kmstream-pi] FATAL: " fmt "\n", ##__VA_ARGS__); exit(1); } while (0)
#define DIE_IF(cond, fmt, ...) do { if (cond) DIE(fmt, ##__VA_ARGS__); } while (0)

static inline uint64_t kms_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static const char *connector_type_str(uint32_t type) {
    switch (type) {
    case DRM_MODE_CONNECTOR_HDMIA:       return "HDMI-A";
    case DRM_MODE_CONNECTOR_HDMIB:       return "HDMI-B";
    case DRM_MODE_CONNECTOR_DisplayPort: return "DP";
    case DRM_MODE_CONNECTOR_eDP:         return "eDP";
    case DRM_MODE_CONNECTOR_DVID:        return "DVI-D";
    case DRM_MODE_CONNECTOR_DVII:        return "DVI-I";
    case DRM_MODE_CONNECTOR_VGA:         return "VGA";
    case DRM_MODE_CONNECTOR_DSI:         return "DSI";
    default:                             return "CONN";
    }
}

static int get_property_info(int fd, uint32_t obj_id, uint32_t obj_type,
                             const char *name, uint32_t *prop_id,
                             uint64_t *cur_val, drmModePropertyPtr *out_prop) {
    drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(fd, obj_id, obj_type);
    if (!props) return -1;

    for (uint32_t i = 0; i < props->count_props; i++) {
        drmModePropertyPtr prop = drmModeGetProperty(fd, props->props[i]);
        if (!prop) continue;

        if (strcmp(prop->name, name) == 0) {
            if (prop_id)  *prop_id  = prop->prop_id;
            if (cur_val)  *cur_val  = props->prop_values[i];
            if (out_prop) *out_prop = prop;
            else          drmModeFreeProperty(prop);
            drmModeFreeObjectProperties(props);
            return 0;
        }
        drmModeFreeProperty(prop);
    }

    drmModeFreeObjectProperties(props);
    return -1;
}

static int get_property_id(int fd, uint32_t obj_id, uint32_t obj_type,
                           const char *name, uint32_t *prop_id) {
    return get_property_info(fd, obj_id, obj_type, name, prop_id, NULL, NULL);
}

static int open_vc4_drm_device(char *dev_out, size_t dev_len, char *drv_out, size_t drv_len) {
    for (int i = 0; i < 4; i++) {
        char dev_path[32];
        snprintf(dev_path, sizeof(dev_path), "/dev/dri/card%d", i);

        int fd = open(dev_path, O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;

        drmVersionPtr ver = drmGetVersion(fd);
        if (ver) {
            int is_vc4 = (strcmp(ver->name, "vc4") == 0);
            if (is_vc4) {
                snprintf(dev_out, dev_len, "%s", dev_path);
                snprintf(drv_out, drv_len, "%s %d.%d.%d",
                         ver->name, ver->version_major, ver->version_minor, ver->version_patchlevel);
            }
            drmFreeVersion(ver);
            if (is_vc4) return fd;
        }
        close(fd);
    }
    snprintf(dev_out, dev_len, "/dev/dri/card0");
    snprintf(drv_out, drv_len, "drm");
    return open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
}

void display_kms_update_geometry(struct display_kms_ctx *ctx, uint32_t vis_w, uint32_t vis_h) {
    ctx->visible_width  = vis_w;
    ctx->visible_height = vis_h;

    if (ctx->crtc_width <= 0 || ctx->crtc_height <= 0 || vis_w == 0 || vis_h == 0) {
        ctx->dst_x = 0;
        ctx->dst_y = 0;
        ctx->dst_w = ctx->crtc_width;
        ctx->dst_h = ctx->crtc_height;
        return;
    }

    float crtc_ratio  = (float)ctx->crtc_width / (float)ctx->crtc_height;
    float frame_ratio = (float)vis_w / (float)vis_h;

    if (crtc_ratio > frame_ratio) {
        ctx->dst_w = (int)(frame_ratio / crtc_ratio * ctx->crtc_width);
        ctx->dst_h = ctx->crtc_height;
        ctx->dst_x = (ctx->crtc_width - ctx->dst_w) / 2;
        ctx->dst_y = 0;
    } else {
        ctx->dst_w = ctx->crtc_width;
        ctx->dst_h = (int)(crtc_ratio / frame_ratio * ctx->crtc_height);
        ctx->dst_x = 0;
        ctx->dst_y = (ctx->crtc_height - ctx->dst_h) / 2;
    }
}

void display_kms_init(struct display_kms_ctx *ctx, struct decode_v4l2_ctx *dec) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->drm_fd = -1;
    ctx->dec    = dec;
    ctx->current_displayed_idx = -1;
    ctx->pending_flip_idx      = -1;
    ctx->staged_frame_idx      = -1;

    char drm_path[32] = {0};
    ctx->drm_fd = open_vc4_drm_device(drm_path, sizeof(drm_path),
                                      ctx->driver_name, sizeof(ctx->driver_name));
    DIE_IF(ctx->drm_fd < 0, "Cannot open DRM device (/dev/dri/card0): %s", strerror(errno));

    drmSetClientCap(ctx->drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    DIE_IF(drmSetClientCap(ctx->drm_fd, DRM_CLIENT_CAP_ATOMIC, 1) != 0,
           "Atomic KMS not supported on DRM device");

    drmModeResPtr res = drmModeGetResources(ctx->drm_fd);
    DIE_IF(!res, "drmModeGetResources failed");

    drmModeConnectorPtr conn = NULL;
    for (int i = 0; i < res->count_connectors; i++) {
        conn = drmModeGetConnector(ctx->drm_fd, res->connectors[i]);
        if (conn && conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) break;
        if (conn) {
            drmModeFreeConnector(conn);
            conn = NULL;
        }
    }
    DIE_IF(!conn, "No connected HDMI/DSI connector found");

    ctx->conn_id = conn->connector_id;
    snprintf(ctx->conn_name, sizeof(ctx->conn_name), "%s-%u",
             connector_type_str(conn->connector_type), conn->connector_type_id);

    uint32_t crtc_id = 0;
    if (conn->encoder_id != 0) {
        drmModeEncoderPtr enc = drmModeGetEncoder(ctx->drm_fd, conn->encoder_id);
        if (enc) {
            crtc_id = enc->crtc_id;
            drmModeFreeEncoder(enc);
        }
    }
    if (crtc_id == 0) {
        for (int i = 0; i < conn->count_encoders && crtc_id == 0; i++) {
            drmModeEncoderPtr enc = drmModeGetEncoder(ctx->drm_fd, conn->encoders[i]);
            if (!enc) continue;
            for (int j = 0; j < res->count_crtcs; j++) {
                if (enc->possible_crtcs & (1 << j)) {
                    crtc_id = res->crtcs[j];
                    break;
                }
            }
            drmModeFreeEncoder(enc);
        }
    }
    if (crtc_id == 0 && res->count_crtcs > 0) {
        crtc_id = res->crtcs[0];
    }
    ctx->crtc_id = crtc_id;

    drmModeCrtcPtr crtc = drmModeGetCrtc(ctx->drm_fd, ctx->crtc_id);
    ctx->orig_crtc_fb_id = (crtc && crtc->mode_valid) ? crtc->buffer_id : 0;

    uint64_t crtc_active_val = 1;
    get_property_info(ctx->drm_fd, ctx->crtc_id, DRM_MODE_OBJECT_CRTC,
                      "ACTIVE", &ctx->prop_crtc_active, &crtc_active_val, NULL);

    drmModeModeInfo active_mode = {0};
    int have_mode = 0;

    if (crtc && crtc->mode_valid && crtc->width > 0 && crtc->height > 0 && crtc_active_val != 0) {
        ctx->crtc_width  = crtc->width;
        ctx->crtc_height = crtc->height;
        active_mode      = crtc->mode;
        have_mode        = 1;
    } else if (conn->count_modes > 0) {
        drmModeModeInfo *chosen_mode = &conn->modes[0];
        if (crtc && crtc->mode_valid && crtc->width > 0 && crtc->height > 0) {
            chosen_mode = &crtc->mode;
        } else {
            for (int i = 0; i < conn->count_modes; i++) {
                if (conn->modes[i].type & DRM_MODE_TYPE_PREFERRED) {
                    chosen_mode = &conn->modes[i];
                    break;
                }
            }
        }
        ctx->crtc_width  = chosen_mode->hdisplay;
        ctx->crtc_height = chosen_mode->vdisplay;
        active_mode      = *chosen_mode;
        have_mode        = 1;

        if (drmModeCreatePropertyBlob(ctx->drm_fd, chosen_mode, sizeof(*chosen_mode),
                                      &ctx->mode_blob_id) == 0 &&
            get_property_id(ctx->drm_fd, ctx->conn_id, DRM_MODE_OBJECT_CONNECTOR,
                            "CRTC_ID", &ctx->prop_conn_crtc_id) == 0 &&
            get_property_id(ctx->drm_fd, ctx->crtc_id, DRM_MODE_OBJECT_CRTC,
                            "MODE_ID", &ctx->prop_crtc_mode_id) == 0 &&
            get_property_id(ctx->drm_fd, ctx->crtc_id, DRM_MODE_OBJECT_CRTC,
                            "ACTIVE", &ctx->prop_crtc_active) == 0) {
            ctx->needs_modeset = 1;
        } else if (ctx->mode_blob_id != 0) {
            drmModeDestroyPropertyBlob(ctx->drm_fd, ctx->mode_blob_id);
            ctx->mode_blob_id = 0;
        }
    } else {
        ctx->crtc_width  = 1920;
        ctx->crtc_height = 1080;
    }

    if (have_mode) {
        ctx->crtc_vrefresh = active_mode.vrefresh;
        if (active_mode.htotal > 0 && active_mode.vtotal > 0 && active_mode.clock > 0) {
            ctx->crtc_hz = ((double)active_mode.clock * 1000.0) /
                           ((double)active_mode.htotal * (double)active_mode.vtotal);
        } else {
            ctx->crtc_hz = (double)active_mode.vrefresh;
        }
    } else {
        ctx->crtc_vrefresh = 60;
        ctx->crtc_hz       = 60.0;
    }

    if (crtc) drmModeFreeCrtc(crtc);
    drmModeFreeConnector(conn);

    int crtc_index = -1;
    for (int i = 0; i < res->count_crtcs; i++) {
        if (res->crtcs[i] == ctx->crtc_id) {
            crtc_index = i;
            break;
        }
    }
    DIE_IF(crtc_index < 0, "Failed to find CRTC index for ID %u", ctx->crtc_id);

    /* Select the first plane (Primary Plane 86 on vc4) supporting DRM_FORMAT_NV12 */
    drmModePlaneResPtr plane_res = drmModeGetPlaneResources(ctx->drm_fd);
    DIE_IF(!plane_res, "drmModeGetPlaneResources failed");

    uint32_t selected_plane = 0;
    for (uint32_t i = 0; i < plane_res->count_planes; i++) {
        drmModePlanePtr plane = drmModeGetPlane(ctx->drm_fd, plane_res->planes[i]);
        if (!plane) continue;

        if (plane->possible_crtcs & (1 << crtc_index)) {
            for (uint32_t j = 0; j < plane->count_formats; j++) {
                if (plane->formats[j] == DRM_FORMAT_NV12) {
                    selected_plane = plane->plane_id;
                    break;
                }
            }
        }
        drmModeFreePlane(plane);
        if (selected_plane != 0) break;
    }
    drmModeFreePlaneResources(plane_res);
    drmModeFreeResources(res);
    DIE_IF(selected_plane == 0, "No DRM plane supporting DRM_FORMAT_NV12 found on CRTC %u", ctx->crtc_id);
    ctx->plane_id = selected_plane;

    DIE_IF(get_property_id(ctx->drm_fd, ctx->plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID",   &ctx->prop_fb_id) != 0 ||
           get_property_id(ctx->drm_fd, ctx->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID", &ctx->prop_crtc_id) != 0 ||
           get_property_id(ctx->drm_fd, ctx->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X",  &ctx->prop_crtc_x) != 0 ||
           get_property_id(ctx->drm_fd, ctx->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y",  &ctx->prop_crtc_y) != 0 ||
           get_property_id(ctx->drm_fd, ctx->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W",  &ctx->prop_crtc_w) != 0 ||
           get_property_id(ctx->drm_fd, ctx->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H",  &ctx->prop_crtc_h) != 0 ||
           get_property_id(ctx->drm_fd, ctx->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_X",   &ctx->prop_src_x) != 0 ||
           get_property_id(ctx->drm_fd, ctx->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_Y",   &ctx->prop_src_y) != 0 ||
           get_property_id(ctx->drm_fd, ctx->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W",   &ctx->prop_src_w) != 0 ||
           get_property_id(ctx->drm_fd, ctx->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H",   &ctx->prop_src_h) != 0,
           "Required atomic properties missing on plane %u", ctx->plane_id);

    ctx->color_encoding_bt709 = 1;
    ctx->color_range_limited  = 0;

    drmModePropertyPtr prop = NULL;
    if (get_property_info(ctx->drm_fd, ctx->plane_id, DRM_MODE_OBJECT_PLANE,
                          "COLOR_ENCODING", &ctx->prop_color_encoding, NULL, &prop) == 0 && prop) {
        for (int i = 0; i < prop->count_enums; i++) {
            if (strcmp(prop->enums[i].name, "ITU-R BT.709 YCbCr") == 0) {
                ctx->color_encoding_bt709 = prop->enums[i].value;
            }
        }
        drmModeFreeProperty(prop);
        prop = NULL;
    }

    if (get_property_info(ctx->drm_fd, ctx->plane_id, DRM_MODE_OBJECT_PLANE,
                          "COLOR_RANGE", &ctx->prop_color_range, NULL, &prop) == 0 && prop) {
        for (int i = 0; i < prop->count_enums; i++) {
            if (strcmp(prop->enums[i].name, "YCbCr limited range") == 0) {
                ctx->color_range_limited = prop->enums[i].value;
            }
        }
        drmModeFreeProperty(prop);
        prop = NULL;
    }

    ctx->atomic_req = drmModeAtomicAlloc();
    DIE_IF(!ctx->atomic_req, "Failed to allocate persistent drmModeAtomicReq");

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT);
    pthread_mutex_init(&ctx->drm_lock, &attr);
    pthread_mutexattr_destroy(&attr);

    printf("[KMS Display] Device: %s (%s) | Connector %u (%s) -> CRTC %u: %dx%d @ %.2f Hz (vrefresh=%u)\n",
           drm_path, ctx->driver_name, ctx->conn_id, ctx->conn_name,
           ctx->crtc_id, ctx->crtc_width, ctx->crtc_height, ctx->crtc_hz, ctx->crtc_vrefresh);
    printf("[KMS Display] Selected Plane %u (Primary NV12, BT.709 Limited) | Saved fbcon FB=%u\n",
           ctx->plane_id, ctx->orig_crtc_fb_id);
    fflush(stdout);
}

int display_kms_import_capture_buf(struct display_kms_ctx *ctx, struct decode_v4l2_ctx *dec, int index) {
    if (dec->capture_bufs[index].export_fd < 0) return -1;

    dec->capture_bufs[index].gem_handle = 0;
    dec->capture_bufs[index].fb_id      = 0;

    if (drmPrimeFDToHandle(ctx->drm_fd, dec->capture_bufs[index].export_fd,
                           &dec->capture_bufs[index].gem_handle) != 0) {
        perror("drmPrimeFDToHandle failed");
        return -1;
    }

    uint32_t handles[4] = {
        dec->capture_bufs[index].gem_handle,
        dec->capture_bufs[index].gem_handle,
        0, 0
    };
    uint32_t pitches[4] = { dec->stride, dec->stride, 0, 0 };
    uint32_t offsets[4] = { 0, dec->stride * dec->coded_height, 0, 0 };

    if (drmModeAddFB2(ctx->drm_fd, dec->coded_width, dec->coded_height,
                      DRM_FORMAT_NV12, handles, pitches, offsets,
                      &dec->capture_bufs[index].fb_id, 0) != 0) {
        perror("drmModeAddFB2(NV12) failed");
        struct drm_gem_close close_req = { .handle = dec->capture_bufs[index].gem_handle };
        ioctl(ctx->drm_fd, DRM_IOCTL_GEM_CLOSE, &close_req);
        dec->capture_bufs[index].gem_handle = 0;
        return -1;
    }

    return 0;
}

void display_kms_release_capture_bufs(struct display_kms_ctx *ctx, struct decode_v4l2_ctx *dec) {
    for (int i = 0; i < dec->num_capture_bufs; i++) {
        if (dec->capture_bufs[i].fb_id != 0) {
            drmModeRmFB(ctx->drm_fd, dec->capture_bufs[i].fb_id);
            dec->capture_bufs[i].fb_id = 0;
        }
        if (dec->capture_bufs[i].gem_handle != 0) {
            struct drm_gem_close close_req = { .handle = dec->capture_bufs[i].gem_handle };
            ioctl(ctx->drm_fd, DRM_IOCTL_GEM_CLOSE, &close_req);
            dec->capture_bufs[i].gem_handle = 0;
        }
        if (dec->capture_bufs[i].export_fd >= 0) {
            close(dec->capture_bufs[i].export_fd);
            dec->capture_bufs[i].export_fd = -1;
        }
    }
}

static int commit_atomic_flip(struct display_kms_ctx *ctx, int buf_idx) {
    drmModeAtomicReqPtr req = ctx->atomic_req;
    drmModeAtomicSetCursor(req, 0);

    uint32_t fb_id = ctx->dec->capture_bufs[buf_idx].fb_id;
    uint32_t flags = DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT;

    if (ctx->needs_modeset && ctx->mode_blob_id != 0) {
        drmModeAtomicAddProperty(req, ctx->conn_id, ctx->prop_conn_crtc_id, ctx->crtc_id);
        drmModeAtomicAddProperty(req, ctx->crtc_id, ctx->prop_crtc_mode_id, ctx->mode_blob_id);
        drmModeAtomicAddProperty(req, ctx->crtc_id, ctx->prop_crtc_active, 1);
        flags = DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_PAGE_FLIP_EVENT;
    }

    drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_fb_id,   fb_id);
    drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_crtc_id, ctx->crtc_id);
    drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_crtc_x,  ctx->dst_x);
    drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_crtc_y,  ctx->dst_y);
    drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_crtc_w,  ctx->dst_w);
    drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_crtc_h,  ctx->dst_h);
    drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_src_x,   0);
    drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_src_y,   0);
    drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_src_w,   (uint64_t)ctx->visible_width  << 16);
    drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_src_h,   (uint64_t)ctx->visible_height << 16);

    if (ctx->prop_color_encoding != 0) {
        drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_color_encoding, ctx->color_encoding_bt709);
    }
    if (ctx->prop_color_range != 0) {
        drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_color_range, ctx->color_range_limited);
    }

    int ret = drmModeAtomicCommit(ctx->drm_fd, req, flags, ctx);
    if (ret == 0) {
        ctx->needs_modeset = 0;
    }
    return ret;
}

void display_kms_present_idx(struct display_kms_ctx *ctx, int new_buf_idx) {
    pthread_mutex_lock(&ctx->drm_lock);

    if (ctx->flip_pending) {
        if (ctx->staged_frame_idx != -1) {
            /* Drop older staged frame to maintain lowest latency */
            decode_v4l2_requeue_capture(ctx->dec, ctx->staged_frame_idx);
            ctx->staged_replacements++;
        }
        ctx->staged_frame_idx = new_buf_idx;
        pthread_mutex_unlock(&ctx->drm_lock);
        return;
    }

    ctx->flip_pending     = 1;
    ctx->pending_flip_idx = new_buf_idx;

    if (commit_atomic_flip(ctx, new_buf_idx) != 0) {
        ctx->flip_pending     = 0;
        ctx->pending_flip_idx = -1;
        decode_v4l2_requeue_capture(ctx->dec, new_buf_idx);
    }

    pthread_mutex_unlock(&ctx->drm_lock);
}

void display_kms_page_flip_handler(int fd, unsigned int seq, unsigned int tv_sec,
                                   unsigned int tv_usec, void *user_data) {
    (void)fd;
    (void)seq;
    (void)tv_sec;
    (void)tv_usec;

    struct display_kms_ctx *ctx = (struct display_kms_ctx *)user_data;
    if (!ctx) return;

    uint64_t t_flip = kms_now_us();

    pthread_mutex_lock(&ctx->drm_lock);

    /* 1. Requeue previously displayed buffer back to V4L2 decoder */
    if (ctx->current_displayed_idx != -1) {
        decode_v4l2_requeue_capture(ctx->dec, ctx->current_displayed_idx);
    }

    int flipped_idx = ctx->pending_flip_idx;
    if (flipped_idx >= 0 && flipped_idx < ctx->dec->num_capture_bufs) {
        uint64_t t_dq = ctx->dec->capture_bufs[flipped_idx].t_dqbuf_us;
        ctx->last_flip_wait_us     = (t_dq > 0 && t_flip > t_dq) ? (t_flip - t_dq) : 0;
        ctx->last_flip_ts_us       = t_flip;
        ctx->last_flipped_frame_id = ctx->dec->capture_bufs[flipped_idx].frame_id;
    }

    ctx->current_displayed_idx = flipped_idx;
    ctx->flip_pending          = 0;
    ctx->flips_completed++;

    /* 2. If a newer frame arrived during VBlank wait, commit it immediately */
    if (ctx->staged_frame_idx != -1) {
        int next_idx          = ctx->staged_frame_idx;
        ctx->staged_frame_idx = -1;
        ctx->flip_pending     = 1;
        ctx->pending_flip_idx = next_idx;

        if (commit_atomic_flip(ctx, next_idx) != 0) {
            ctx->flip_pending     = 0;
            ctx->pending_flip_idx = -1;
            decode_v4l2_requeue_capture(ctx->dec, next_idx);
        }
    }

    pthread_mutex_unlock(&ctx->drm_lock);
}

void display_kms_drain_pending_flip(struct display_kms_ctx *ctx) {
    if (ctx->drm_fd < 0) return;

    drmEventContext evctx;
    memset(&evctx, 0, sizeof(evctx));
    evctx.version           = DRM_EVENT_CONTEXT_VERSION;
    evctx.page_flip_handler = display_kms_page_flip_handler;

    pthread_mutex_lock(&ctx->drm_lock);
    while (ctx->flip_pending) {
        pthread_mutex_unlock(&ctx->drm_lock);

        struct pollfd pfd = { .fd = ctx->drm_fd, .events = POLLIN, .revents = 0 };
        int ret = poll(&pfd, 1, 100);
        if (ret > 0 && (pfd.revents & POLLIN)) {
            drmHandleEvent(ctx->drm_fd, &evctx);
            pthread_mutex_lock(&ctx->drm_lock);
        } else {
            pthread_mutex_lock(&ctx->drm_lock);
            ctx->flip_pending = 0;
            break;
        }
    }
    pthread_mutex_unlock(&ctx->drm_lock);
}

static int restore_crtc_primary_fb(struct display_kms_ctx *ctx) {
    if (ctx->drm_fd < 0 || ctx->plane_id == 0 || ctx->orig_crtc_fb_id == 0 ||
        ctx->crtc_width <= 0 || ctx->crtc_height <= 0) {
        return -1;
    }

    drmModeAtomicReqPtr req = drmModeAtomicAlloc();
    if (!req) return -1;

    drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_fb_id,   ctx->orig_crtc_fb_id);
    drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_crtc_id, ctx->crtc_id);
    drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_crtc_x,  0);
    drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_crtc_y,  0);
    drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_crtc_w,  ctx->crtc_width);
    drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_crtc_h,  ctx->crtc_height);
    drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_src_x,   0);
    drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_src_y,   0);
    drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_src_w,   (uint32_t)ctx->crtc_width  << 16);
    drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_src_h,   (uint32_t)ctx->crtc_height << 16);

    int ret = drmModeAtomicCommit(ctx->drm_fd, req, 0, NULL);
    drmModeAtomicFree(req);
    return ret;
}

void display_kms_disable_plane_sync(struct display_kms_ctx *ctx, int restore_fbcon) {
    if (ctx->drm_fd < 0 || ctx->plane_id == 0) return;

    if (restore_fbcon && restore_crtc_primary_fb(ctx) == 0) {
        return;
    }

    drmModeAtomicReqPtr req = drmModeAtomicAlloc();
    if (!req) return;

    drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_fb_id,   0);
    drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_crtc_id, 0);

    drmModeAtomicCommit(ctx->drm_fd, req, 0, NULL);
    drmModeAtomicFree(req);
}

void display_kms_cleanup(struct display_kms_ctx *ctx, struct decode_v4l2_ctx *dec) {
    if (ctx->drm_fd >= 0) {
        display_kms_drain_pending_flip(ctx);
        display_kms_disable_plane_sync(ctx, 1);
        display_kms_release_capture_bufs(ctx, dec);
        if (ctx->mode_blob_id != 0) {
            drmModeDestroyPropertyBlob(ctx->drm_fd, ctx->mode_blob_id);
            ctx->mode_blob_id = 0;
        }
        close(ctx->drm_fd);
        ctx->drm_fd = -1;
    }
    if (ctx->atomic_req) {
        drmModeAtomicFree(ctx->atomic_req);
        ctx->atomic_req = NULL;
    }
    pthread_mutex_destroy(&ctx->drm_lock);
}
