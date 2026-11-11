#include "decode_v4l2.h"
#include "display_kms.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#define DIE(fmt, ...) do { fprintf(stderr, "[kmstream-pi] FATAL: " fmt "\n", ##__VA_ARGS__); exit(1); } while (0)
#define DIE_IF(cond, fmt, ...) do { if (cond) DIE(fmt, ##__VA_ARGS__); } while (0)

static int open_bcm2835_decoder_device(char *path_out, size_t path_len,
                                       char *driver_out, size_t driver_len,
                                       char *card_out, size_t card_len) {
    for (int i = 10; i <= 24; i++) {
        char dev_path[32];
        snprintf(dev_path, sizeof(dev_path), "/dev/video%d", i);

        int fd = open(dev_path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;

        struct v4l2_capability cap;
        memset(&cap, 0, sizeof(cap));
        if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
            uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
            uint32_t req = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING;
            if ((caps & req) == req && strcmp((char *)cap.card, "bcm2835-codec-decode") == 0) {
                snprintf(path_out, path_len, "%s", dev_path);
                snprintf(driver_out, driver_len, "%s", (const char *)cap.driver);
                snprintf(card_out, card_len, "%s", (const char *)cap.card);
                return fd;
            }
        }
        close(fd);
    }
    snprintf(path_out, path_len, "/dev/video10");
    snprintf(driver_out, driver_len, "bcm2835-codec");
    snprintf(card_out, card_len, "bcm2835-codec-decode");
    return open("/dev/video10", O_RDWR | O_NONBLOCK | O_CLOEXEC);
}

void decode_v4l2_init(struct decode_v4l2_ctx *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->v4l2_fd = -1;
    for (int i = 0; i < V4L2_MAX_CAPTURE_BUFS; i++) {
        ctx->capture_bufs[i].export_fd = -1;
    }

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT);
    pthread_mutex_init(&ctx->v4l2_lock, &attr);
    pthread_mutexattr_destroy(&attr);
    pthread_cond_init(&ctx->drc_cond, NULL);

    char dev_path[32] = {0}, drv_name[32] = {0}, card_name[64] = {0};
    ctx->v4l2_fd = open_bcm2835_decoder_device(dev_path, sizeof(dev_path),
                                               drv_name, sizeof(drv_name),
                                               card_name, sizeof(card_name));
    DIE_IF(ctx->v4l2_fd < 0, "Cannot open bcm2835-codec-decode (/dev/video10): %s", strerror(errno));

    /* 1. Subscribe to Dynamic Resolution Change (SOURCE_CHANGE) events */
    struct v4l2_event_subscription sub;
    memset(&sub, 0, sizeof(sub));
    sub.type = V4L2_EVENT_SOURCE_CHANGE;
    DIE_IF(ioctl(ctx->v4l2_fd, VIDIOC_SUBSCRIBE_EVENT, &sub) != 0,
           "VIDIOC_SUBSCRIBE_EVENT(SOURCE_CHANGE) failed: %s", strerror(errno));

    /* 2. Configure OUTPUT_MPLANE with 32x32 placeholder geometry so first SPS fires SOURCE_CHANGE */
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    fmt.fmt.pix_mp.width = 32;
    fmt.fmt.pix_mp.height = 32;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264;
    fmt.fmt.pix_mp.num_planes = 1;
    fmt.fmt.pix_mp.plane_fmt[0].sizeimage = V4L2_OUTPUT_BUF_SIZE;
    DIE_IF(ioctl(ctx->v4l2_fd, VIDIOC_S_FMT, &fmt) != 0,
           "VIDIOC_S_FMT(OUTPUT_MPLANE) failed: %s", strerror(errno));

    /* 3. Allocate 4 OUTPUT_MPLANE buffers (matching moonlight-embedded-rpi3-v4l2) */
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count  = V4L2_NUM_OUTPUT_BUFS;
    req.type   = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;
    DIE_IF(ioctl(ctx->v4l2_fd, VIDIOC_REQBUFS, &req) != 0 || req.count == 0,
           "VIDIOC_REQBUFS(OUTPUT_MPLANE) failed: %s", strerror(errno));
    ctx->num_output_bufs = (req.count > V4L2_NUM_OUTPUT_BUFS) ? V4L2_NUM_OUTPUT_BUFS : (int)req.count;

    for (int i = 0; i < ctx->num_output_bufs; i++) {
        struct v4l2_plane planes[1];
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));
        buf.type     = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        buf.memory   = V4L2_MEMORY_MMAP;
        buf.index    = i;
        buf.length   = 1;
        buf.m.planes = planes;

        DIE_IF(ioctl(ctx->v4l2_fd, VIDIOC_QUERYBUF, &buf) != 0,
               "VIDIOC_QUERYBUF(OUTPUT_MPLANE %d) failed: %s", i, strerror(errno));

        ctx->output_bufs[i].length = buf.m.planes[0].length;
        ctx->output_bufs[i].start  = mmap(NULL, buf.m.planes[0].length,
                                          PROT_READ | PROT_WRITE, MAP_SHARED,
                                          ctx->v4l2_fd, buf.m.planes[0].m.mem_offset);
        DIE_IF(ctx->output_bufs[i].start == MAP_FAILED,
               "mmap(OUTPUT_MPLANE %d) failed: %s", i, strerror(errno));
        ctx->output_bufs[i].queued = 0;
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    DIE_IF(ioctl(ctx->v4l2_fd, VIDIOC_STREAMON, &type) != 0,
           "VIDIOC_STREAMON(OUTPUT_MPLANE) failed: %s", strerror(errno));
    ctx->output_streaming = 1;

    printf("[V4L2 Init] Device: %s (%s / %s) | OUTPUT_MPLANE: %d x %zu KB (H264 + zero-copy trailing AUD)\n",
           dev_path, drv_name, card_name, ctx->num_output_bufs,
           ctx->output_bufs[0].length / 1024);
    fflush(stdout);
}

int decode_v4l2_submit(struct decode_v4l2_ctx *ctx, const uint8_t *data,
                       uint32_t size, uint32_t frame_id,
                       const volatile sig_atomic_t *stop_flag) {
    static const uint8_t trailing_aud[6] = { 0x00, 0x00, 0x00, 0x01, 0x09, 0xF0 };

    pthread_mutex_lock(&ctx->v4l2_lock);
    while (ctx->drc_in_progress && (!stop_flag || !*stop_flag)) {
        pthread_cond_wait(&ctx->drc_cond, &ctx->v4l2_lock);
    }
    if ((stop_flag && *stop_flag) || ctx->v4l2_fd < 0) {
        pthread_mutex_unlock(&ctx->v4l2_lock);
        return -ECANCELED;
    }

    int free_idx = -1;
    struct v4l2_plane planes[1];
    struct v4l2_buffer buf;

    for (int attempt = 0; attempt < 4; attempt++) {
        while (1) {
            memset(&buf, 0, sizeof(buf));
            memset(planes, 0, sizeof(planes));
            buf.type     = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
            buf.memory   = V4L2_MEMORY_MMAP;
            buf.length   = 1;
            buf.m.planes = planes;

            if (ioctl(ctx->v4l2_fd, VIDIOC_DQBUF, &buf) != 0) break;
            if ((int)buf.index < ctx->num_output_bufs) {
                ctx->output_bufs[buf.index].queued = 0;
            }
        }

        for (int i = 0; i < ctx->num_output_bufs; i++) {
            if (!ctx->output_bufs[i].queued) {
                free_idx = i;
                break;
            }
        }

        if (free_idx >= 0 || attempt == 3) break;

        int v4l2_fd = ctx->v4l2_fd;
        pthread_mutex_unlock(&ctx->v4l2_lock);

        struct pollfd pfd = { .fd = v4l2_fd, .events = POLLOUT, .revents = 0 };
        poll(&pfd, 1, 3);

        pthread_mutex_lock(&ctx->v4l2_lock);
        while (ctx->drc_in_progress && (!stop_flag || !*stop_flag)) {
            pthread_cond_wait(&ctx->drc_cond, &ctx->v4l2_lock);
        }
        if ((stop_flag && *stop_flag) || ctx->v4l2_fd < 0) {
            pthread_mutex_unlock(&ctx->v4l2_lock);
            return -ECANCELED;
        }
    }

    if (free_idx < 0) {
        pthread_mutex_unlock(&ctx->v4l2_lock);
        return -EBUSY;
    }

    size_t cap = ctx->output_bufs[free_idx].length;
    if ((size_t)size + sizeof(trailing_aud) > cap) {
        pthread_mutex_unlock(&ctx->v4l2_lock);
        return -EINVAL;
    }

    uint8_t *dst = (uint8_t *)ctx->output_bufs[free_idx].start;
    memcpy(dst, data, size);
    /* Append trailing AUD directly into the V4L2 OUTPUT mmap buffer so VideoCore IV
     * flushes the current frame immediately without waiting for frame N+1. */
    memcpy(dst + size, trailing_aud, sizeof(trailing_aud));
    uint32_t total_bytes = size + (uint32_t)sizeof(trailing_aud);

    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));
    buf.type                  = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    buf.memory                = V4L2_MEMORY_MMAP;
    buf.index                 = free_idx;
    buf.length                = 1;
    buf.m.planes              = planes;
    buf.m.planes[0].bytesused = total_bytes;
    buf.flags                 = V4L2_BUF_FLAG_TIMESTAMP_COPY;
    buf.timestamp.tv_sec      = (time_t)frame_id;
    buf.timestamp.tv_usec     = 0;

    if (ioctl(ctx->v4l2_fd, VIDIOC_QBUF, &buf) != 0) {
        int err = -errno;
        pthread_mutex_unlock(&ctx->v4l2_lock);
        return err;
    }

    ctx->output_bufs[free_idx].queued = 1;
    pthread_mutex_unlock(&ctx->v4l2_lock);
    return 0;
}

int decode_v4l2_handle_drc(struct decode_v4l2_ctx *ctx, struct display_kms_ctx *kms) {
    pthread_mutex_lock(&ctx->v4l2_lock);
    ctx->drc_in_progress = 1;
    pthread_mutex_unlock(&ctx->v4l2_lock);

    /* 1. Drain pending page flips and detach DRM plane before reallocating capture buffers */
    display_kms_drain_pending_flip(kms);

    pthread_mutex_lock(&kms->drm_lock);
    if (ctx->capture_streaming) {
        display_kms_disable_plane_sync(kms, 0);
    }
    kms->flip_pending          = 0;
    kms->current_displayed_idx = -1;
    kms->pending_flip_idx      = -1;
    kms->staged_frame_idx      = -1;
    pthread_mutex_unlock(&kms->drm_lock);

    pthread_mutex_lock(&ctx->v4l2_lock);

    /* 2. If capture queue was active, drain it and stop */
    if (ctx->capture_streaming) {
        struct v4l2_plane planes[1];
        struct v4l2_buffer b;
        memset(&b, 0, sizeof(b));
        memset(planes, 0, sizeof(planes));
        b.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        b.memory   = V4L2_MEMORY_MMAP;
        b.length   = 1;
        b.m.planes = planes;

        while (ioctl(ctx->v4l2_fd, VIDIOC_DQBUF, &b) == 0) {
            if (b.flags & V4L2_BUF_FLAG_LAST) break;
        }

        enum v4l2_buf_type cap_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        ioctl(ctx->v4l2_fd, VIDIOC_STREAMOFF, &cap_type);

        display_kms_release_capture_bufs(kms, ctx);

        struct v4l2_requestbuffers req0;
        memset(&req0, 0, sizeof(req0));
        req0.count  = 0;
        req0.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        req0.memory = V4L2_MEMORY_MMAP;
        ioctl(ctx->v4l2_fd, VIDIOC_REQBUFS, &req0);

        ctx->capture_streaming = 0;
    }

    /* 3. Query negotiated format and enforce NV12 (2 planes) */
    struct v4l2_format gfmt;
    memset(&gfmt, 0, sizeof(gfmt));
    gfmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (ioctl(ctx->v4l2_fd, VIDIOC_G_FMT, &gfmt) != 0) {
        perror("VIDIOC_G_FMT(CAPTURE_MPLANE) failed");
        goto out_unlock;
    }

    gfmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    if (ioctl(ctx->v4l2_fd, VIDIOC_S_FMT, &gfmt) != 0) {
        perror("VIDIOC_S_FMT(NV12) failed");
        goto out_unlock;
    }

    ctx->coded_width  = gfmt.fmt.pix_mp.width;
    ctx->coded_height = gfmt.fmt.pix_mp.height;
    ctx->stride       = gfmt.fmt.pix_mp.plane_fmt[0].bytesperline;

    /* 4. Query visible crop selection (e.g. 1920x1080 inside 1920x1088) */
    struct v4l2_selection sel;
    memset(&sel, 0, sizeof(sel));
    sel.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    sel.target = V4L2_SEL_TGT_COMPOSE;

    if (ioctl(ctx->v4l2_fd, VIDIOC_G_SELECTION, &sel) == 0 &&
        sel.r.width > 0 && sel.r.height > 0) {
        ctx->visible_width  = sel.r.width;
        ctx->visible_height = sel.r.height;
    } else {
        ctx->visible_width  = ctx->coded_width;
        ctx->visible_height = (ctx->coded_height == 1088) ? 1080 : ctx->coded_height;
    }

    display_kms_update_geometry(kms, ctx->visible_width, ctx->visible_height);

    /* 5. Query min capture buffers and request allocation */
    struct v4l2_control ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.id = V4L2_CID_MIN_BUFFERS_FOR_CAPTURE;
    int min_bufs = 2;
    if (ioctl(ctx->v4l2_fd, VIDIOC_G_CTRL, &ctrl) == 0 && ctrl.value > 0) {
        min_bufs = ctrl.value;
    }

    int req_count = min_bufs + V4L2_CAPTURE_CUSHION;
    if (req_count < V4L2_MIN_CAPTURE_BUFS) req_count = V4L2_MIN_CAPTURE_BUFS;
    if (req_count > V4L2_MAX_CAPTURE_BUFS) req_count = V4L2_MAX_CAPTURE_BUFS;

    for (int i = 0; i < V4L2_MAX_CAPTURE_BUFS; i++) {
        ctx->capture_bufs[i].export_fd  = -1;
        ctx->capture_bufs[i].gem_handle = 0;
        ctx->capture_bufs[i].fb_id      = 0;
        ctx->capture_bufs[i].frame_id   = 0;
        ctx->capture_bufs[i].t_dqbuf_us = 0;
        ctx->capture_bufs[i].queued     = 0;
    }

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count  = req_count;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(ctx->v4l2_fd, VIDIOC_REQBUFS, &req) != 0 || req.count == 0) {
        perror("VIDIOC_REQBUFS(CAPTURE_MPLANE) failed");
        goto out_unlock;
    }
    ctx->num_capture_bufs = (int)req.count;

    /* 6. Export DMABUF, import to DRM as NV12 FB, and queue each capture buffer */
    for (int i = 0; i < ctx->num_capture_bufs; i++) {
        struct v4l2_exportbuffer exp;
        memset(&exp, 0, sizeof(exp));
        exp.type  = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        exp.index = i;
        exp.flags = O_CLOEXEC | O_RDWR;

        if (ioctl(ctx->v4l2_fd, VIDIOC_EXPBUF, &exp) != 0) {
            perror("VIDIOC_EXPBUF failed");
            goto err_cleanup_capture;
        }
        ctx->capture_bufs[i].export_fd = exp.fd;

        if (display_kms_import_capture_buf(kms, ctx, i) != 0) {
            fprintf(stderr, "[V4L2] Failed to import capture buffer %d to DRM\n", i);
            goto err_cleanup_capture;
        }

        struct v4l2_plane p[1];
        struct v4l2_buffer qb;
        memset(&qb, 0, sizeof(qb));
        memset(p, 0, sizeof(p));
        qb.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        qb.memory   = V4L2_MEMORY_MMAP;
        qb.index    = i;
        qb.length   = 1;
        qb.m.planes = p;

        if (ioctl(ctx->v4l2_fd, VIDIOC_QBUF, &qb) != 0) {
            perror("VIDIOC_QBUF(CAPTURE_MPLANE) failed");
            goto err_cleanup_capture;
        }
        ctx->capture_bufs[i].queued = 1;
    }

    enum v4l2_buf_type cap_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (ioctl(ctx->v4l2_fd, VIDIOC_STREAMON, &cap_type) != 0) {
        perror("VIDIOC_STREAMON(CAPTURE_MPLANE) failed");
        goto err_cleanup_capture;
    }
    ctx->capture_streaming = 1;

    printf("[V4L2 DRC] Negotiated %ux%u (coded %ux%u, stride %u) NV12 | %d capture bufs (min=%d) -> KMS Plane %u dst=%d,%d %dx%d\n",
           ctx->visible_width, ctx->visible_height,
           ctx->coded_width, ctx->coded_height, ctx->stride,
           ctx->num_capture_bufs, min_bufs, kms->plane_id,
           kms->dst_x, kms->dst_y, kms->dst_w, kms->dst_h);
    fflush(stdout);

    ctx->drc_in_progress = 0;
    pthread_cond_broadcast(&ctx->drc_cond);
    pthread_mutex_unlock(&ctx->v4l2_lock);
    return 0;

err_cleanup_capture:
    {
        enum v4l2_buf_type off_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        ioctl(ctx->v4l2_fd, VIDIOC_STREAMOFF, &off_type);
    }
    display_kms_release_capture_bufs(kms, ctx);
    {
        struct v4l2_requestbuffers req0;
        memset(&req0, 0, sizeof(req0));
        req0.count  = 0;
        req0.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        req0.memory = V4L2_MEMORY_MMAP;
        ioctl(ctx->v4l2_fd, VIDIOC_REQBUFS, &req0);
    }
    ctx->num_capture_bufs  = 0;
    ctx->capture_streaming = 0;

out_unlock:
    ctx->drc_in_progress = 0;
    pthread_cond_broadcast(&ctx->drc_cond);
    pthread_mutex_unlock(&ctx->v4l2_lock);
    return -1;
}

void decode_v4l2_requeue_capture(struct decode_v4l2_ctx *ctx, int index) {
    if (index < 0 || index >= ctx->num_capture_bufs || !ctx->capture_streaming) return;

    struct v4l2_plane planes[1];
    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));
    buf.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory   = V4L2_MEMORY_MMAP;
    buf.index    = index;
    buf.length   = 1;
    buf.m.planes = planes;

    ioctl(ctx->v4l2_fd, VIDIOC_QBUF, &buf);
}

void decode_v4l2_cleanup(struct decode_v4l2_ctx *ctx) {
    if (ctx->v4l2_fd < 0) return;

    struct v4l2_decoder_cmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.cmd = V4L2_DEC_CMD_STOP;
    ioctl(ctx->v4l2_fd, VIDIOC_DECODER_CMD, &cmd);

    /* Strict order: OUTPUT first, then CAPTURE */
    if (ctx->output_streaming) {
        enum v4l2_buf_type out_type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        ioctl(ctx->v4l2_fd, VIDIOC_STREAMOFF, &out_type);
        ctx->output_streaming = 0;
    }
    if (ctx->capture_streaming) {
        enum v4l2_buf_type cap_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        ioctl(ctx->v4l2_fd, VIDIOC_STREAMOFF, &cap_type);
        ctx->capture_streaming = 0;
    }

    for (int i = 0; i < ctx->num_output_bufs; i++) {
        if (ctx->output_bufs[i].start && ctx->output_bufs[i].start != MAP_FAILED) {
            munmap(ctx->output_bufs[i].start, ctx->output_bufs[i].length);
            ctx->output_bufs[i].start = NULL;
        }
    }

    struct v4l2_requestbuffers req0;
    memset(&req0, 0, sizeof(req0));
    req0.count  = 0;
    req0.memory = V4L2_MEMORY_MMAP;

    req0.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    ioctl(ctx->v4l2_fd, VIDIOC_REQBUFS, &req0);

    req0.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    ioctl(ctx->v4l2_fd, VIDIOC_REQBUFS, &req0);

    close(ctx->v4l2_fd);
    ctx->v4l2_fd = -1;

    pthread_cond_destroy(&ctx->drc_cond);
    pthread_mutex_destroy(&ctx->v4l2_lock);
}
