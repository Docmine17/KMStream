/* kmstream_pi.c - KMStream Raspberry Pi 3B Client (UDP -> V4L2 M2M H.264 -> KMS/DRM) */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/opt.h>

#include "protocol.h"

#define DIE_IF(cond, fmt, ...) do { \
    if (cond) { \
        fprintf(stderr, "[kmstream-pi] FATAL (%s:%d): " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
        exit(1); \
    } \
} while (0)

static inline uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static enum AVPixelFormat get_drm_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts) {
    (void)ctx;
    for (const enum AVPixelFormat *p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == AV_PIX_FMT_DRM_PRIME) return *p;
    }
    return pix_fmts[0];
}

static int plane_supports_format(drmModePlane *p, uint32_t fmt) {
    for (uint32_t i = 0; i < p->count_formats; i++) {
        if (p->formats[i] == fmt) return 1;
    }
    return 0;
}

int main(void) {
    int drm_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    DIE_IF(drm_fd < 0, "open(/dev/dri/card0): %s", strerror(errno));
    DIE_IF(drmSetClientCap(drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) < 0,
           "DRM_CLIENT_CAP_UNIVERSAL_PLANES failed");

    drmModeRes *res = drmModeGetResources(drm_fd);
    DIE_IF(!res, "drmModeGetResources failed");
    uint32_t crtc_id = 0;
    int crtc_idx = -1, screen_w = 1920, screen_h = 1080;

    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *conn = drmModeGetConnector(drm_fd, res->connectors[i]);
        if (conn && conn->connection == DRM_MODE_CONNECTED && conn->encoder_id) {
            drmModeEncoder *enc = drmModeGetEncoder(drm_fd, conn->encoder_id);
            if (enc && enc->crtc_id) {
                crtc_id = enc->crtc_id;
                drmModeCrtc *crtc = drmModeGetCrtc(drm_fd, crtc_id);
                if (crtc && crtc->mode_valid) {
                    screen_w = crtc->mode.hdisplay;
                    screen_h = crtc->mode.vdisplay;
                }
                if (crtc) drmModeFreeCrtc(crtc);
            }
            if (enc) drmModeFreeEncoder(enc);
            drmModeFreeConnector(conn);
            if (crtc_id) break;
        }
        if (conn) drmModeFreeConnector(conn);
    }
    for (int i = 0; i < res->count_crtcs; i++) {
        if (res->crtcs[i] == crtc_id) { crtc_idx = i; break; }
    }
    drmModeFreeResources(res);
    DIE_IF(!crtc_id || crtc_idx < 0, "no active HDMI/DRM connector found on /dev/dri/card0");

    const AVCodec *codec = avcodec_find_decoder_by_name("h264_v4l2m2m");
    DIE_IF(!codec, "h264_v4l2m2m decoder not found");
    AVCodecContext *dec = avcodec_alloc_context3(codec);
    DIE_IF(!dec, "avcodec_alloc_context3 failed");
    dec->get_format = get_drm_format;
    dec->flags |= AV_CODEC_FLAG_LOW_DELAY;
    dec->flags2 |= AV_CODEC_FLAG2_FAST;
    dec->extra_hw_frames = 1;
    av_opt_set_int(dec->priv_data, "num_output_buffers", 2, 0);
    av_opt_set_int(dec->priv_data, "num_capture_buffers", 3, 0);
    DIE_IF(avcodec_open2(dec, codec, NULL) < 0, "avcodec_open2(h264_v4l2m2m) failed");

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    DIE_IF(sock < 0, "socket: %s", strerror(errno));
    int rcvbuf = 256 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    struct sockaddr_in addr = {
        .sin_family = AF_INET, .sin_port = htons(KMS_VIDEO_PORT), .sin_addr.s_addr = INADDR_ANY
    };
    DIE_IF(bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0, "bind(port %d): %s",
           KMS_VIDEO_PORT, strerror(errno));

    printf("=== KMStream Pi 3B Client (proto v%d, Zero-Hold + Drain-to-Latest) | Display: %dx%d ===\n",
           KMS_PROTO_VERSION, screen_w, screen_h);

    uint8_t *frame_buf = malloc(KMS_MAX_FRAME);
    uint8_t pkt_buf[sizeof(struct kms_video_hdr) + KMS_CHUNK_SIZE + 64];
    uint32_t cur_frame_id = 0, chunks_recv = 0, expected_chunks = 0, frame_len = 0, last_complete_len = 0;

    AVPacket *avpkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    DIE_IF(!frame_buf || !avpkt || !frame, "allocation failed");
    uint32_t video_plane_id = 0;

    uint32_t fb_ring[3] = {0, 0, 0};
    uint32_t gem_ring[3] = {0, 0, 0};
    int ring_idx = 0;

    uint64_t fps_timer = now_us();
    uint64_t sum_dec_us = 0;
    int frames_shown = 0, skipped_stale = 0;
    int64_t last_v4l2_hold = 0;
    int first_frame_done = 0;

    while (1) {
        ssize_t n = recv(sock, pkt_buf, sizeof(pkt_buf), 0);
        if (n <= (ssize_t)sizeof(struct kms_video_hdr)) continue;

        /*
         * Process the received packet and drain any packets already queued in the
         * socket buffer. If a newer frame is already arriving, feed the previous
         * complete frame to the decoder to keep P-frame references valid without
         * accumulating display latency.
         */
        int ready_to_decode = 0;
        while (1) {
            struct kms_video_hdr *hdr = (struct kms_video_hdr *)pkt_buf;
            if (hdr->version != KMS_PROTO_VERSION) {
                n = recv(sock, pkt_buf, sizeof(pkt_buf), MSG_DONTWAIT);
                if (n <= (ssize_t)sizeof(struct kms_video_hdr)) break;
                continue;
            }
            uint8_t *payload = pkt_buf + sizeof(struct kms_video_hdr);

            if (hdr->frame_id < cur_frame_id && (cur_frame_id - hdr->frame_id) < 1000) {
                /* Stale packet from an older frame, ignore */
            } else {
                if (hdr->frame_id > cur_frame_id) {
                    if (ready_to_decode) {
                        avpkt->data = frame_buf;
                        avpkt->size = frame_len;
                        avpkt->pts  = cur_frame_id;
                        avcodec_send_packet(dec, avpkt);
                        ready_to_decode = 0;
                        skipped_stale++;
                    }
                    cur_frame_id = hdr->frame_id;
                    chunks_recv = 0;
                    frame_len = 0;
                    expected_chunks = hdr->total_chunks;
                }

                if (hdr->chunk_idx < KMS_MAX_CHUNKS && hdr->payload_len <= KMS_CHUNK_SIZE) {
                    memcpy(frame_buf + ((size_t)hdr->chunk_idx * KMS_CHUNK_SIZE), payload, hdr->payload_len);
                    if (hdr->chunk_idx == hdr->total_chunks - 1) {
                        frame_len = (hdr->chunk_idx * KMS_CHUNK_SIZE) + hdr->payload_len;
                    }
                    chunks_recv++;
                }

                if (chunks_recv == expected_chunks && expected_chunks > 0 && frame_len > 0) {
                    ready_to_decode = 1;
                }
            }

            n = recv(sock, pkt_buf, sizeof(pkt_buf), MSG_DONTWAIT);
            if (n <= (ssize_t)sizeof(struct kms_video_hdr)) break;
        }

        if (ready_to_decode) {
            uint64_t t_dec0 = now_us();
            last_complete_len = frame_len;
            avpkt->data = frame_buf;
            avpkt->size = frame_len;
            avpkt->pts  = cur_frame_id;
            avcodec_send_packet(dec, avpkt);
            chunks_recv = 0;

            /*
             * With the patched SPS (max_dec_frame_buffering=1), bcm2835-codec
             * releases cur_frame_id in ~1.5 ms (720p). Poll at 100 us intervals.
             */
            AVFrame *latest_frame = NULL;
            uint64_t wait_deadline = t_dec0 + (first_frame_done ? 4500 : 50000);

            while (now_us() < wait_deadline) {
                while (avcodec_receive_frame(dec, frame) == 0) {
                    if (frame->format == AV_PIX_FMT_DRM_PRIME) {
                        if (latest_frame) av_frame_unref(latest_frame);
                        else latest_frame = av_frame_alloc();
                        av_frame_move_ref(latest_frame, frame);
                    } else {
                        av_frame_unref(frame);
                    }
                }
                if (latest_frame && latest_frame->pts == (int64_t)cur_frame_id) break;
                usleep(100);
            }

            uint64_t dec_elapsed = now_us() - t_dec0;
            sum_dec_us += dec_elapsed;

            if (latest_frame) {
                first_frame_done = 1;
                if (latest_frame->pts > 0) {
                    last_v4l2_hold = (int64_t)cur_frame_id - latest_frame->pts;
                }

                AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor *)latest_frame->data[0];
                uint32_t drm_fmt = desc->layers[0].format;

                if (!video_plane_id) {
                    drmModePlaneRes *pres = drmModeGetPlaneResources(drm_fd);
                    for (uint32_t i = 0; i < pres->count_planes; i++) {
                        drmModePlane *p = drmModeGetPlane(drm_fd, pres->planes[i]);
                        if (p && (p->possible_crtcs & (1 << crtc_idx)) && plane_supports_format(p, drm_fmt)) {
                            video_plane_id = p->plane_id;
                            if (p->fb_id == 0) { drmModeFreePlane(p); break; }
                        }
                        if (p) drmModeFreePlane(p);
                    }
                    drmModeFreePlaneResources(pres);
                    printf("-> Stream %dx%d connected! Plane %u (%.4s)\n",
                           latest_frame->width, latest_frame->height, video_plane_id, (char *)&drm_fmt);
                }

                uint32_t gem_handle = 0;
                drmPrimeFDToHandle(drm_fd, desc->objects[0].fd, &gem_handle);

                uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};
                for (int p = 0; p < desc->layers[0].nb_planes && p < 4; p++) {
                    handles[p] = gem_handle;
                    pitches[p] = desc->layers[0].planes[p].pitch;
                    offsets[p] = desc->layers[0].planes[p].offset;
                }

                uint32_t fb_id = 0;
                if (drmModeAddFB2(drm_fd, latest_frame->width, latest_frame->height, drm_fmt,
                                  handles, pitches, offsets, &fb_id, 0) == 0) {
                    drmModeSetPlane(drm_fd, video_plane_id, crtc_id, fb_id, 0,
                                    0, 0, screen_w, screen_h,
                                    0, 0, latest_frame->width << 16, latest_frame->height << 16);

                    int old_slot = (ring_idx + 1) % 3;
                    if (fb_ring[old_slot]) {
                        drmModeRmFB(drm_fd, fb_ring[old_slot]);
                        fb_ring[old_slot] = 0;
                    }
                    if (gem_ring[old_slot] && gem_ring[old_slot] != gem_handle) {
                        struct drm_gem_close cl = { .handle = gem_ring[old_slot] };
                        drmIoctl(drm_fd, DRM_IOCTL_GEM_CLOSE, &cl);
                        gem_ring[old_slot] = 0;
                    }
                    fb_ring[ring_idx] = fb_id;
                    gem_ring[ring_idx] = gem_handle;
                    ring_idx = (ring_idx + 1) % 3;
                }

                frames_shown++;
                uint64_t now = now_us();
                if (now - fps_timer >= 2000000ULL) {
                    printf("[Pi] %4.1f FPS | Size: %5u B | Decode Wait: %.2f ms | V4L2 Hold: %ld frames | Skipped Queue: %d\n",
                           frames_shown * 1000000.0 / (now - fps_timer),
                           last_complete_len,
                           (frames_shown ? (sum_dec_us / (double)frames_shown) / 1000.0 : 0.0),
                           (long)last_v4l2_hold,
                           skipped_stale);
                    frames_shown = 0;
                    sum_dec_us = 0;
                    fps_timer = now;
                }
                av_frame_free(&latest_frame);
            }
        }
    }
    return 0;
}
