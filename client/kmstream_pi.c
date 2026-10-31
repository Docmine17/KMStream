/* kmstream_pi.c - KMStream Raspberry Pi 3B Client (UDP -> V4L2 M2M H.264 -> KMS/DRM) */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <poll.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/opt.h>

#include "protocol.h"
#include "stats.h"

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

static inline uint16_t clamp_u16(uint32_t v) {
    return (v > 65535U) ? 65535U : (uint16_t)v;
}

struct frame_meta_slot {
    uint32_t frame_id;
    uint64_t capture_ts_us;
    uint64_t t_rx_first_us;
    uint16_t t_convert_us;
    uint16_t t_encode_us;
};

struct clock_sync {
    int64_t  rtt_ring[16];
    uint32_t ring_count;
    uint32_t ring_idx;
    int64_t  offset_us;      /* t_pc ≈ t_pi + offset_us */
    int64_t  min_rtt_us;
    int      synced;
    uint32_t seq;
    uint64_t last_ping_us;
};

static void clock_sync_on_pong(struct clock_sync *cs, const struct kms_ctrl_pong *pong, uint64_t t3_us) {
    int64_t total_us = (int64_t)(t3_us - pong->t0_us);
    int64_t srv_us   = (int64_t)(pong->t2_us - pong->t1_us);
    int64_t rtt_us   = total_us - srv_us;
    if (rtt_us <= 0 || rtt_us > 50000) return;

    int64_t sample_offset = ((int64_t)pong->t1_us - (int64_t)pong->t0_us +
                             (int64_t)pong->t2_us - (int64_t)t3_us) / 2;

    cs->rtt_ring[cs->ring_idx] = rtt_us;
    cs->ring_idx = (cs->ring_idx + 1) & 15;
    if (cs->ring_count < 16) cs->ring_count++;

    int64_t min_rtt = cs->rtt_ring[0];
    for (uint32_t i = 1; i < cs->ring_count; i++) {
        if (cs->rtt_ring[i] < min_rtt) min_rtt = cs->rtt_ring[i];
    }
    cs->min_rtt_us = min_rtt;

    if (!cs->synced) {
        cs->offset_us = sample_offset;
        cs->synced = 1;
    } else if (rtt_us <= min_rtt + 150) {
        /* Update offset via EWMA using only low-jitter samples near minimum RTT */
        cs->offset_us = (cs->offset_us * 7 + sample_offset) / 8;
    }
}

static void drain_ctrl_pongs(int ctrl_sock, struct clock_sync *cs) {
    uint8_t buf[128];
    while (1) {
        ssize_t n = recv(ctrl_sock, buf, sizeof(buf), MSG_DONTWAIT);
        if (n < (ssize_t)sizeof(struct kms_ctrl_hdr)) break;
        uint64_t t3 = now_us();
        const struct kms_ctrl_hdr *h = (const struct kms_ctrl_hdr *)buf;
        if (h->version == KMS_PROTO_VERSION && h->type == KMS_CTRL_PONG &&
            n >= (ssize_t)sizeof(struct kms_ctrl_pong)) {
            clock_sync_on_pong(cs, (const struct kms_ctrl_pong *)buf, t3);
        }
    }
}

static void maybe_send_ping(int ctrl_sock, const struct sockaddr_in *srv_ctrl_addr,
                            int has_srv_addr, struct clock_sync *cs) {
    if (!has_srv_addr) return;
    uint64_t now = now_us();
    uint64_t interval = (cs->ring_count < 8) ? 50000ULL : 250000ULL;
    if (now - cs->last_ping_us < interval) return;
    cs->last_ping_us = now;

    struct kms_ctrl_ping ping = {
        .h = {
            .version  = KMS_PROTO_VERSION,
            .type     = KMS_CTRL_PING,
            .reserved = 0,
            .seq      = ++cs->seq,
        },
        .t0_us = now_us(),
    };
    sendto(ctrl_sock, &ping, sizeof(ping), MSG_DONTWAIT,
           (const struct sockaddr *)srv_ctrl_addr, sizeof(*srv_ctrl_addr));
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

    int ctrl_sock = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    DIE_IF(ctrl_sock < 0, "ctrl socket: %s", strerror(errno));
    int tos = IPTOS_LOWDELAY;
    setsockopt(ctrl_sock, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));

    printf("=== KMStream Pi 3B Client (proto v%d, Zero-Hold + Telemetry) | Display: %dx%d ===\n",
           KMS_PROTO_VERSION, screen_w, screen_h);
    fflush(stdout);

    uint8_t *frame_buf = malloc(KMS_MAX_FRAME);
    uint8_t pkt_buf[sizeof(struct kms_video_hdr) + KMS_CHUNK_SIZE + 64];
    uint32_t cur_frame_id = 0, chunks_recv = 0, expected_chunks = 0, frame_len = 0;
    uint64_t cur_rx_first_us = 0, cur_rx_last_us = 0;

    AVPacket *avpkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    DIE_IF(!frame_buf || !avpkt || !frame, "allocation failed");
    uint32_t video_plane_id = 0;

    uint32_t fb_ring[3] = {0, 0, 0};
    uint32_t gem_ring[3] = {0, 0, 0};
    int ring_idx = 0;

    struct frame_meta_slot frame_meta[64];
    memset(frame_meta, 0, sizeof(frame_meta));

    struct clock_sync cs = {0};
    struct sockaddr_in srv_ctrl_addr = {0};
    int has_srv_addr = 0;

    struct kms_stat_win st_e2e = {0}, st_net = {0}, st_reass = {0}, st_dec = {0}, st_kms = {0}, st_srv = {0};
    uint64_t fps_timer = now_us();
    uint32_t frames_shown_total = 0, frames_incomplete = 0, skipped_stale = 0;
    int64_t last_v4l2_hold = 0;
    int first_frame_done = 0;

    struct pollfd pfds[2] = {
        { .fd = sock,      .events = POLLIN },
        { .fd = ctrl_sock, .events = POLLIN },
    };

    while (1) {
        poll(pfds, 2, 50);
        drain_ctrl_pongs(ctrl_sock, &cs);

        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        ssize_t n = recvfrom(sock, pkt_buf, sizeof(pkt_buf), MSG_DONTWAIT,
                             (struct sockaddr *)&peer, &peer_len);
        if (n <= (ssize_t)sizeof(struct kms_video_hdr)) {
            maybe_send_ping(ctrl_sock, &srv_ctrl_addr, has_srv_addr, &cs);
            continue;
        }

        if (!has_srv_addr) {
            srv_ctrl_addr = peer;
            srv_ctrl_addr.sin_port = htons(KMS_CTRL_PORT);
            has_srv_addr = 1;
            maybe_send_ping(ctrl_sock, &srv_ctrl_addr, has_srv_addr, &cs);
        }

        int ready_to_decode = 0;
        while (1) {
            uint64_t t_pkt = now_us();
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
                    } else if (cur_frame_id > 0 && chunks_recv > 0) {
                        frames_incomplete++;
                    }
                    cur_frame_id = hdr->frame_id;
                    chunks_recv = 0;
                    frame_len = 0;
                    expected_chunks = hdr->total_chunks;
                    cur_rx_first_us = t_pkt;

                    struct frame_meta_slot *slot = &frame_meta[cur_frame_id & 63];
                    slot->frame_id      = cur_frame_id;
                    slot->capture_ts_us = hdr->capture_ts_us;
                    slot->t_rx_first_us = t_pkt;
                    slot->t_convert_us  = hdr->t_convert_us;
                    slot->t_encode_us   = hdr->t_encode_us;
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
                    cur_rx_last_us = t_pkt;
                }
            }

            n = recv(sock, pkt_buf, sizeof(pkt_buf), MSG_DONTWAIT);
            if (n <= (ssize_t)sizeof(struct kms_video_hdr)) break;
        }

        drain_ctrl_pongs(ctrl_sock, &cs);

        if (ready_to_decode) {
            uint64_t t_dec0 = now_us();
            avpkt->data = frame_buf;
            avpkt->size = frame_len;
            avpkt->pts  = cur_frame_id;
            avcodec_send_packet(dec, avpkt);
            chunks_recv = 0;
            kms_stat_add(&st_reass, (uint32_t)(cur_rx_last_us - cur_rx_first_us));

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

            uint64_t t_dec_end = now_us();
            uint32_t dec_elapsed = (uint32_t)(t_dec_end - t_dec0);

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
                    fflush(stdout);
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

                uint64_t t_drm_end = now_us();
                uint32_t kms_elapsed = (uint32_t)(t_drm_end - t_dec_end);

                kms_stat_add(&st_dec, dec_elapsed);
                kms_stat_add(&st_kms, kms_elapsed);

                int64_t shown_pts = latest_frame->pts;
                if (shown_pts > 0) {
                    const struct frame_meta_slot *m = &frame_meta[(uint32_t)shown_pts & 63];
                    if (m->frame_id == (uint32_t)shown_pts && m->capture_ts_us > 0) {
                        uint32_t srv_us = (uint32_t)m->t_convert_us + (uint32_t)m->t_encode_us;
                        kms_stat_add(&st_srv, srv_us);
                        if (cs.synced) {
                            int64_t e2e = (int64_t)t_drm_end + cs.offset_us - (int64_t)m->capture_ts_us;
                            int64_t net = (int64_t)m->t_rx_first_us + cs.offset_us -
                                          (int64_t)m->capture_ts_us - (int64_t)srv_us;
                            if (e2e < 0) e2e = 0;
                            if (net < 0) net = 0;
                            kms_stat_add(&st_e2e, (uint32_t)e2e);
                            kms_stat_add(&st_net, (uint32_t)net);
                        }
                    }
                }

                frames_shown_total++;
                av_frame_free(&latest_frame);

                /* Send PING right after scanout when the video pipeline is idle until next frame */
                maybe_send_ping(ctrl_sock, &srv_ctrl_addr, has_srv_addr, &cs);
                drain_ctrl_pongs(ctrl_sock, &cs);

                uint64_t now = now_us();
                if (now - fps_timer >= 2000000ULL && st_dec.count > 0) {
                    uint32_t e2e_avg, e2e_p99, net_avg, net_p99, reass_avg, reass_p99;
                    uint32_t dec_avg, dec_p99, kms_avg, kms_p99, srv_avg, srv_p99;
                    double fps = st_dec.count * 1000000.0 / (double)(now - fps_timer);

                    kms_stat_compute(&st_e2e,   &e2e_avg,   &e2e_p99);
                    kms_stat_compute(&st_net,   &net_avg,   &net_p99);
                    kms_stat_compute(&st_reass, &reass_avg, &reass_p99);
                    kms_stat_compute(&st_dec,   &dec_avg,   &dec_p99);
                    kms_stat_compute(&st_kms,   &kms_avg,   &kms_p99);
                    kms_stat_compute(&st_srv,   &srv_avg,   &srv_p99);

                    printf("[Pi] %4.1f FPS | E2E: %.2f/%.2f ms | Srv: %.2f ms | Net+Rx: %.2f+%.2f ms | Dec: %.2f/%.2f ms | KMS: %.2f/%.2f ms | Hold:%ldf | Inc:%u Stale:%u | RTT: %.2f ms\n",
                           fps,
                           e2e_avg / 1000.0, e2e_p99 / 1000.0,
                           srv_avg / 1000.0,
                           net_avg / 1000.0, reass_avg / 1000.0,
                           dec_avg / 1000.0, dec_p99 / 1000.0,
                           kms_avg / 1000.0, kms_p99 / 1000.0,
                           (long)last_v4l2_hold,
                           frames_incomplete, skipped_stale,
                           cs.min_rtt_us / 1000.0);
                    fflush(stdout);

                    if (has_srv_addr) {
                        struct kms_ctrl_stats st_pkt = {
                            .h = {
                                .version  = KMS_PROTO_VERSION,
                                .type     = KMS_CTRL_STATS,
                                .reserved = 0,
                                .seq      = frames_shown_total,
                            },
                            .frames_ok      = frames_shown_total,
                            .frames_lost    = frames_incomplete,
                            .rx_drops       = skipped_stale,
                            .e2e_us_avg     = e2e_avg,
                            .e2e_us_p99     = e2e_p99,
                            .decode_us_avg  = clamp_u16(dec_avg),
                            .decode_us_p99  = clamp_u16(dec_p99),
                            .display_us_avg = clamp_u16(kms_avg),
                            .display_us_p99 = clamp_u16(kms_p99),
                        };
                        sendto(ctrl_sock, &st_pkt, sizeof(st_pkt), MSG_DONTWAIT,
                               (const struct sockaddr *)&srv_ctrl_addr, sizeof(srv_ctrl_addr));
                    }

                    kms_stat_reset(&st_e2e);
                    kms_stat_reset(&st_net);
                    kms_stat_reset(&st_reass);
                    kms_stat_reset(&st_dec);
                    kms_stat_reset(&st_kms);
                    kms_stat_reset(&st_srv);
                    fps_timer = now;
                }
            }
        }
    }
    return 0;
}
