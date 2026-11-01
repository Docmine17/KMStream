/* kmstream_pi.c - KMStream Raspberry Pi 3B Client (Dedicated RX Thread + V4L2 M2M + KMS) */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
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

#define RX_RING_SLOTS 8
#define BMAP_WORDS    ((KMS_MAX_CHUNKS + 63) / 64)

enum slot_state {
    SLOT_FREE       = 0,
    SLOT_ASSEMBLING = 1,
    SLOT_READY      = 2,
    SLOT_SENDING    = 3,
};

struct rx_slot {
    uint8_t  data[KMS_MAX_FRAME];
    uint64_t chunk_bmap[BMAP_WORDS];
    uint32_t frame_id;
    uint32_t frame_size;
    uint16_t total_chunks;
    uint16_t chunks_unique;
    uint8_t  flags;
    uint8_t  state;
    uint64_t capture_ts_us;
    uint64_t t_rx_first_us;
    uint64_t t_rx_last_us;
    uint16_t t_convert_us;
    uint16_t t_encode_us;
};

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

struct rx_shared {
    pthread_mutex_t mtx;
    pthread_cond_t  cond;
    struct rx_slot  slots[RX_RING_SLOTS];

    int                video_sock;
    int                ctrl_sock;
    struct sockaddr_in srv_ctrl_addr;
    int                has_srv_addr;

    struct clock_sync  cs;

    uint32_t frames_incomplete;
    uint32_t idr_requests_sent;
    uint32_t last_completed_id;
    int      waiting_for_idr;
    uint64_t last_idr_req_us;
    uint32_t idr_req_seq;
};

static inline uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static inline uint16_t clamp_u16(uint32_t v) {
    return (v > 65535U) ? 65535U : (uint16_t)v;
}

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
        cs->offset_us = (cs->offset_us * 7 + sample_offset) / 8;
    }
}

static void drain_ctrl_pongs_locked(struct rx_shared *sh) {
    uint8_t buf[128];
    while (1) {
        ssize_t n = recv(sh->ctrl_sock, buf, sizeof(buf), MSG_DONTWAIT);
        if (n < (ssize_t)sizeof(struct kms_ctrl_hdr)) break;
        uint64_t t3 = now_us();
        const struct kms_ctrl_hdr *h = (const struct kms_ctrl_hdr *)buf;
        if (h->version == KMS_PROTO_VERSION && h->type == KMS_CTRL_PONG &&
            n >= (ssize_t)sizeof(struct kms_ctrl_pong)) {
            clock_sync_on_pong(&sh->cs, (const struct kms_ctrl_pong *)buf, t3);
        }
    }
}

static void maybe_send_ping_locked(struct rx_shared *sh) {
    if (!sh->has_srv_addr) return;
    uint64_t now = now_us();
    uint64_t interval = (sh->cs.ring_count < 8) ? 50000ULL : 250000ULL;
    if (now - sh->cs.last_ping_us < interval) return;
    sh->cs.last_ping_us = now;

    struct kms_ctrl_ping ping = {
        .h = {
            .version  = KMS_PROTO_VERSION,
            .type     = KMS_CTRL_PING,
            .reserved = 0,
            .seq      = ++sh->cs.seq,
        },
        .t0_us = now_us(),
    };
    sendto(sh->ctrl_sock, &ping, sizeof(ping), MSG_DONTWAIT,
           (const struct sockaddr *)&sh->srv_ctrl_addr, sizeof(sh->srv_ctrl_addr));
}

static void send_idr_request_locked(struct rx_shared *sh, uint64_t now) {
    sh->waiting_for_idr = 1;
    if (!sh->has_srv_addr) return;
    if (now - sh->last_idr_req_us < 30000ULL) return; /* 30 ms cooldown */
    sh->last_idr_req_us = now;
    sh->idr_requests_sent++;

    struct kms_ctrl_idr_request req = {
        .h = {
            .version  = KMS_PROTO_VERSION,
            .type     = KMS_CTRL_IDR_REQUEST,
            .reserved = 0,
            .seq      = ++sh->idr_req_seq,
        },
        .last_good_frame = sh->last_completed_id,
    };
    sendto(sh->ctrl_sock, &req, sizeof(req), MSG_DONTWAIT,
           (const struct sockaddr *)&sh->srv_ctrl_addr, sizeof(sh->srv_ctrl_addr));
}

static void reset_slot(struct rx_slot *s, const struct kms_video_hdr *hdr, uint64_t t_pkt) {
    uint16_t words = (uint16_t)((hdr->total_chunks + 63) / 64);
    if (words > BMAP_WORDS) words = BMAP_WORDS;
    memset(s->chunk_bmap, 0, (size_t)words * sizeof(uint64_t));
    s->frame_id      = hdr->frame_id;
    s->frame_size    = hdr->frame_size;
    s->total_chunks  = hdr->total_chunks;
    s->chunks_unique = 0;
    s->flags         = hdr->flags;
    s->state         = SLOT_ASSEMBLING;
    s->capture_ts_us = hdr->capture_ts_us;
    s->t_rx_first_us = t_pkt;
    s->t_rx_last_us  = t_pkt;
    s->t_convert_us  = hdr->t_convert_us;
    s->t_encode_us   = hdr->t_encode_us;
}

static void *rx_thread_fn(void *arg) {
    struct rx_shared *sh = (struct rx_shared *)arg;
    uint8_t pkt_buf[sizeof(struct kms_video_hdr) + KMS_CHUNK_SIZE + 64];
    int cur_slot_idx = -1;
    uint32_t cur_frame_id = 0;

    struct pollfd pfds[2] = {
        { .fd = sh->video_sock, .events = POLLIN },
        { .fd = sh->ctrl_sock,  .events = POLLIN },
    };

    while (1) {
        poll(pfds, 2, 20);

        pthread_mutex_lock(&sh->mtx);
        drain_ctrl_pongs_locked(sh);
        maybe_send_ping_locked(sh);
        if (sh->waiting_for_idr) {
            send_idr_request_locked(sh, now_us());
        }
        pthread_mutex_unlock(&sh->mtx);

        while (1) {
            struct sockaddr_in peer;
            socklen_t peer_len = sizeof(peer);
            ssize_t n = recvfrom(sh->video_sock, pkt_buf, sizeof(pkt_buf), MSG_DONTWAIT,
                                 (struct sockaddr *)&peer, &peer_len);
            if (n <= (ssize_t)sizeof(struct kms_video_hdr)) break;

            uint64_t t_pkt = now_us();
            const struct kms_video_hdr *hdr = (const struct kms_video_hdr *)pkt_buf;
            if (hdr->version != KMS_PROTO_VERSION) continue;
            if (hdr->total_chunks == 0 || hdr->total_chunks > KMS_MAX_CHUNKS) continue;
            if (hdr->chunk_idx >= hdr->total_chunks || hdr->payload_len > KMS_CHUNK_SIZE) continue;
            if ((size_t)n < sizeof(struct kms_video_hdr) + hdr->payload_len) continue;

            const uint8_t *payload = pkt_buf + sizeof(struct kms_video_hdr);

            pthread_mutex_lock(&sh->mtx);
            if (!sh->has_srv_addr) {
                sh->srv_ctrl_addr = peer;
                sh->srv_ctrl_addr.sin_port = htons(KMS_CTRL_PORT);
                sh->has_srv_addr = 1;
                maybe_send_ping_locked(sh);
            }

            if (hdr->frame_id < cur_frame_id && (hdr->flags & KMS_FLAG_IDR) && hdr->frame_id <= 10) {
                cur_frame_id = 0;
                sh->last_completed_id = 0;
                sh->waiting_for_idr = 0;
                if (cur_slot_idx >= 0 && sh->slots[cur_slot_idx].state == SLOT_ASSEMBLING) {
                    sh->slots[cur_slot_idx].state = SLOT_FREE;
                }
                cur_slot_idx = -1;
            }

            if (hdr->frame_id < cur_frame_id && (cur_frame_id - hdr->frame_id) < 10000) {
                pthread_mutex_unlock(&sh->mtx);
                continue;
            }

            if (hdr->frame_id > cur_frame_id) {
                if (cur_slot_idx >= 0 && sh->slots[cur_slot_idx].state == SLOT_ASSEMBLING) {
                    sh->slots[cur_slot_idx].state = SLOT_FREE;
                    sh->frames_incomplete++;
                    send_idr_request_locked(sh, t_pkt);
                } else if (sh->last_completed_id > 0 && hdr->frame_id > sh->last_completed_id + 1 &&
                           !(hdr->flags & KMS_FLAG_IDR)) {
                    sh->frames_incomplete += (hdr->frame_id - sh->last_completed_id - 1);
                    send_idr_request_locked(sh, t_pkt);
                }

                cur_frame_id = hdr->frame_id;
                cur_slot_idx = -1;

                if (sh->waiting_for_idr && !(hdr->flags & KMS_FLAG_IDR)) {
                    pthread_mutex_unlock(&sh->mtx);
                    continue;
                }

                for (int i = 0; i < RX_RING_SLOTS; i++) {
                    if (sh->slots[i].state == SLOT_FREE) {
                        cur_slot_idx = i;
                        break;
                    }
                }
                if (cur_slot_idx < 0) {
                    int oldest_idx = -1;
                    uint32_t oldest_id = UINT32_MAX;
                    for (int i = 0; i < RX_RING_SLOTS; i++) {
                        if (sh->slots[i].state == SLOT_READY && sh->slots[i].frame_id < oldest_id) {
                            oldest_id = sh->slots[i].frame_id;
                            oldest_idx = i;
                        }
                    }
                    cur_slot_idx = (oldest_idx >= 0) ? oldest_idx : 0;
                }

                reset_slot(&sh->slots[cur_slot_idx], hdr, t_pkt);
            }

            if (cur_slot_idx >= 0 && sh->slots[cur_slot_idx].state == SLOT_ASSEMBLING &&
                sh->slots[cur_slot_idx].frame_id == hdr->frame_id) {
                struct rx_slot *s = &sh->slots[cur_slot_idx];
                uint16_t c = hdr->chunk_idx;
                uint64_t mask = 1ULL << (c & 63);
                if (!(s->chunk_bmap[c >> 6] & mask)) {
                    s->chunk_bmap[c >> 6] |= mask;
                    memcpy(s->data + ((size_t)c * KMS_CHUNK_SIZE), payload, hdr->payload_len);
                    s->chunks_unique++;
                    if (hdr->frame_size > 0) {
                        s->frame_size = hdr->frame_size;
                    } else if (c == hdr->total_chunks - 1) {
                        s->frame_size = (uint32_t)c * KMS_CHUNK_SIZE + hdr->payload_len;
                    }

                    if (s->chunks_unique == s->total_chunks && s->frame_size > 0) {
                        s->t_rx_last_us = t_pkt;
                        s->state = SLOT_READY;
                        sh->last_completed_id = s->frame_id;
                        if (s->flags & KMS_FLAG_IDR) {
                            sh->waiting_for_idr = 0;
                        }
                        cur_slot_idx = -1;
                        pthread_cond_signal(&sh->cond);
                    }
                }
            }
            pthread_mutex_unlock(&sh->mtx);
        }
    }
    return NULL;
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
    dec->extra_hw_frames = 2;
    av_opt_set_int(dec->priv_data, "num_output_buffers", 4, 0);
    av_opt_set_int(dec->priv_data, "num_capture_buffers", 6, 0);
    DIE_IF(avcodec_open2(dec, codec, NULL) < 0, "avcodec_open2(h264_v4l2m2m) failed");

    int sock = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    DIE_IF(sock < 0, "socket: %s", strerror(errno));
    int req_rcvbuf = 4 * 1024 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &req_rcvbuf, sizeof(req_rcvbuf));
    int actual_rcvbuf = 0;
    socklen_t optlen = sizeof(actual_rcvbuf);
    getsockopt(sock, SOL_SOCKET, SO_RCVBUF, &actual_rcvbuf, &optlen);

    struct sockaddr_in addr = {
        .sin_family = AF_INET, .sin_port = htons(KMS_VIDEO_PORT), .sin_addr.s_addr = INADDR_ANY
    };
    DIE_IF(bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0, "bind(port %d): %s",
           KMS_VIDEO_PORT, strerror(errno));

    int ctrl_sock = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    DIE_IF(ctrl_sock < 0, "ctrl socket: %s", strerror(errno));
    int tos = IPTOS_LOWDELAY;
    setsockopt(ctrl_sock, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));

    printf("=== KMStream Pi 3B Client (proto v%d, Dedicated RX Thread, SO_RCVBUF=%d KB) | Display: %dx%d ===\n",
           KMS_PROTO_VERSION, actual_rcvbuf / 1024, screen_w, screen_h);
    fflush(stdout);

    struct rx_shared *sh = calloc(1, sizeof(*sh));
    DIE_IF(!sh, "calloc rx_shared failed");
    pthread_mutex_init(&sh->mtx, NULL);
    pthread_cond_init(&sh->cond, NULL);
    sh->video_sock = sock;
    sh->ctrl_sock  = ctrl_sock;

    pthread_t rx_tid;
    DIE_IF(pthread_create(&rx_tid, NULL, rx_thread_fn, sh) != 0, "pthread_create failed");

    AVPacket *avpkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    DIE_IF(!avpkt || !frame, "AVPacket/AVFrame alloc failed");
    uint32_t video_plane_id = 0;

    uint32_t fb_ring[3] = {0, 0, 0};
    uint32_t gem_ring[3] = {0, 0, 0};
    AVFrame *frame_ring[3] = {NULL, NULL, NULL};
    int ring_idx = 0;

    struct frame_meta_slot frame_meta[64];
    memset(frame_meta, 0, sizeof(frame_meta));

    struct kms_stat_win st_e2e = {0}, st_net = {0}, st_reass = {0}, st_dec = {0}, st_kms = {0}, st_srv = {0};
    uint64_t fps_timer = now_us();
    uint32_t frames_shown_total = 0, skipped_stale = 0;
    int64_t last_v4l2_hold = 0;
    int first_frame_done = 0;
    int stream_h = 720;

    while (1) {
        pthread_mutex_lock(&sh->mtx);
        while (1) {
            int has_ready = 0;
            for (int i = 0; i < RX_RING_SLOTS; i++) {
                if (sh->slots[i].state == SLOT_READY) { has_ready = 1; break; }
            }
            if (has_ready) break;
            struct timespec wait_ts;
            clock_gettime(CLOCK_REALTIME, &wait_ts);
            wait_ts.tv_nsec += 50000000L;
            if (wait_ts.tv_nsec >= 1000000000L) {
                wait_ts.tv_sec++;
                wait_ts.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&sh->cond, &sh->mtx, &wait_ts);
        }

        uint32_t newest_idr_id = 0;
        for (int i = 0; i < RX_RING_SLOTS; i++) {
            if (sh->slots[i].state == SLOT_READY && (sh->slots[i].flags & KMS_FLAG_IDR)) {
                if (sh->slots[i].frame_id > newest_idr_id) newest_idr_id = sh->slots[i].frame_id;
            }
        }
        if (newest_idr_id > 0) {
            for (int i = 0; i < RX_RING_SLOTS; i++) {
                if (sh->slots[i].state == SLOT_READY && sh->slots[i].frame_id < newest_idr_id) {
                    sh->slots[i].state = SLOT_FREE;
                    skipped_stale++;
                }
            }
        }

        uint32_t cur_frame_id = 0;
        uint32_t reass_us = 0;

        while (1) {
            int oldest_idx = -1;
            int ready_cnt = 0;
            uint32_t oldest_id = UINT32_MAX;
            for (int i = 0; i < RX_RING_SLOTS; i++) {
                if (sh->slots[i].state == SLOT_READY) {
                    ready_cnt++;
                    if (sh->slots[i].frame_id < oldest_id) {
                        oldest_id = sh->slots[i].frame_id;
                        oldest_idx = i;
                    }
                }
            }
            if (oldest_idx < 0) break;

            struct rx_slot *s = &sh->slots[oldest_idx];
            uint32_t fid = s->frame_id;
            struct frame_meta_slot *mslot = &frame_meta[fid & 63];
            mslot->frame_id      = fid;
            mslot->capture_ts_us = s->capture_ts_us;
            mslot->t_rx_first_us = s->t_rx_first_us;
            mslot->t_convert_us  = s->t_convert_us;
            mslot->t_encode_us   = s->t_encode_us;

            reass_us = (uint32_t)(s->t_rx_last_us - s->t_rx_first_us);
            cur_frame_id = fid;

            avpkt->data = s->data;
            avpkt->size = (int)s->frame_size;
            avpkt->pts  = fid;
            s->state    = SLOT_SENDING;

            pthread_mutex_unlock(&sh->mtx);
            avcodec_send_packet(dec, avpkt);
            if (ready_cnt > 1) {
                while (avcodec_receive_frame(dec, frame) == 0) {
                    av_frame_unref(frame);
                }
            }
            pthread_mutex_lock(&sh->mtx);
            s->state = SLOT_FREE;

            if (ready_cnt > 1) {
                skipped_stale++;
            } else {
                break;
            }
        }
        pthread_mutex_unlock(&sh->mtx);

        if (cur_frame_id == 0) continue;

        uint64_t t_dec0 = now_us();
        kms_stat_add(&st_reass, reass_us);

        AVFrame *latest_frame = NULL;
        uint64_t wait_budget_us = !first_frame_done ? 50000ULL : ((stream_h >= 1080) ? 14000ULL : 6500ULL);
        uint64_t wait_deadline = t_dec0 + wait_budget_us;

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
            if (latest_frame) break;
            usleep(100);
        }

        uint64_t t_dec_end = now_us();
        uint32_t dec_elapsed = (uint32_t)(t_dec_end - t_dec0);

        if (latest_frame) {
            first_frame_done = 1;
            stream_h = latest_frame->height;
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
                printf("-> Stream %dx%d connected! Plane %u (%.4s) modifier=0x%llx\n",
                       latest_frame->width, latest_frame->height, video_plane_id,
                       (char *)&drm_fmt, (unsigned long long)desc->objects[0].format_modifier);
                fflush(stdout);
            }

            uint32_t gem_handle = 0;
            drmPrimeFDToHandle(drm_fd, desc->objects[0].fd, &gem_handle);

            uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};
            uint64_t modifiers[4] = {0};
            uint64_t mod = desc->objects[0].format_modifier;
            for (int p = 0; p < desc->layers[0].nb_planes && p < 4; p++) {
                handles[p]   = gem_handle;
                pitches[p]   = desc->layers[0].planes[p].pitch;
                offsets[p]   = desc->layers[0].planes[p].offset;
                modifiers[p] = mod;
            }

            uint32_t fb_id = 0;
            int fb_ok = -1;
            if (mod != DRM_FORMAT_MOD_INVALID && mod != DRM_FORMAT_MOD_LINEAR) {
                fb_ok = drmModeAddFB2WithModifiers(drm_fd, latest_frame->width, latest_frame->height,
                                                   drm_fmt, handles, pitches, offsets, modifiers,
                                                   &fb_id, DRM_MODE_FB_MODIFIERS);
            }
            if (fb_ok != 0) {
                fb_ok = drmModeAddFB2(drm_fd, latest_frame->width, latest_frame->height,
                                      drm_fmt, handles, pitches, offsets, &fb_id, 0);
            }

            int64_t shown_pts = latest_frame->pts;

            if (fb_ok == 0) {
                drmModeSetPlane(drm_fd, video_plane_id, crtc_id, fb_id, 0,
                                0, 0, screen_w, screen_h,
                                0, 0, latest_frame->width << 16, latest_frame->height << 16);

                int old_slot = (ring_idx + 1) % 3;
                if (fb_ring[old_slot]) {
                    drmModeRmFB(drm_fd, fb_ring[old_slot]);
                    fb_ring[old_slot] = 0;
                }
                if (gem_ring[old_slot]) {
                    uint32_t h_old = gem_ring[old_slot];
                    int other_a = ring_idx;
                    int other_b = (ring_idx + 2) % 3;
                    if (h_old != gem_handle && h_old != gem_ring[other_a] && h_old != gem_ring[other_b]) {
                        struct drm_gem_close cl = { .handle = h_old };
                        drmIoctl(drm_fd, DRM_IOCTL_GEM_CLOSE, &cl);
                    }
                    gem_ring[old_slot] = 0;
                }
                if (frame_ring[old_slot]) {
                    av_frame_free(&frame_ring[old_slot]);
                }

                fb_ring[ring_idx]    = fb_id;
                gem_ring[ring_idx]   = gem_handle;
                frame_ring[ring_idx] = latest_frame;
                latest_frame         = NULL;
                ring_idx             = (ring_idx + 1) % 3;
            }

            uint64_t t_drm_end = now_us();
            uint32_t kms_elapsed = (uint32_t)(t_drm_end - t_dec_end);

            kms_stat_add(&st_dec, dec_elapsed);
            kms_stat_add(&st_kms, kms_elapsed);

            pthread_mutex_lock(&sh->mtx);
            int synced = sh->cs.synced;
            int64_t offset_us = sh->cs.offset_us;
            int64_t min_rtt_us = sh->cs.min_rtt_us;
            uint32_t inc_cnt = sh->frames_incomplete;
            uint32_t idr_cnt = sh->idr_requests_sent;
            int has_srv = sh->has_srv_addr;
            struct sockaddr_in srv_addr = sh->srv_ctrl_addr;
            pthread_mutex_unlock(&sh->mtx);

            if (shown_pts > 0) {
                const struct frame_meta_slot *m = &frame_meta[(uint32_t)shown_pts & 63];
                if (m->frame_id == (uint32_t)shown_pts && m->capture_ts_us > 0) {
                    uint32_t srv_us = (uint32_t)m->t_convert_us + (uint32_t)m->t_encode_us;
                    kms_stat_add(&st_srv, srv_us);
                    if (synced) {
                        int64_t e2e = (int64_t)t_drm_end + offset_us - (int64_t)m->capture_ts_us;
                        int64_t net = (int64_t)m->t_rx_first_us + offset_us -
                                      (int64_t)m->capture_ts_us - (int64_t)srv_us;
                        if (e2e < 0) e2e = 0;
                        if (net < 0) net = 0;
                        kms_stat_add(&st_e2e, (uint32_t)e2e);
                        kms_stat_add(&st_net, (uint32_t)net);
                    }
                }
            }

            frames_shown_total++;
            if (latest_frame) av_frame_free(&latest_frame);

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

                printf("[Pi] %4.1f FPS | E2E: %.2f/%.2f ms | Srv: %.2f ms | Net+Rx: %.2f+%.2f ms | Dec: %.2f/%.2f ms | KMS: %.2f/%.2f ms | Hold:%ldf | Inc:%u IDR:%u Stale:%u | RTT: %.2f ms\n",
                       fps,
                       e2e_avg / 1000.0, e2e_p99 / 1000.0,
                       srv_avg / 1000.0,
                       net_avg / 1000.0, reass_avg / 1000.0,
                       dec_avg / 1000.0, dec_p99 / 1000.0,
                       kms_avg / 1000.0, kms_p99 / 1000.0,
                       (long)last_v4l2_hold,
                       inc_cnt, idr_cnt, skipped_stale,
                       min_rtt_us / 1000.0);
                fflush(stdout);

                if (has_srv) {
                    struct kms_ctrl_stats st_pkt = {
                        .h = {
                            .version  = KMS_PROTO_VERSION,
                            .type     = KMS_CTRL_STATS,
                            .reserved = 0,
                            .seq      = frames_shown_total,
                        },
                        .frames_ok      = frames_shown_total,
                        .frames_lost    = inc_cnt,
                        .rx_drops       = skipped_stale,
                        .e2e_us_avg     = e2e_avg,
                        .e2e_us_p99     = e2e_p99,
                        .decode_us_avg  = clamp_u16(dec_avg),
                        .decode_us_p99  = clamp_u16(dec_p99),
                        .display_us_avg = clamp_u16(kms_avg),
                        .display_us_p99 = clamp_u16(kms_p99),
                    };
                    sendto(ctrl_sock, &st_pkt, sizeof(st_pkt), MSG_DONTWAIT,
                           (const struct sockaddr *)&srv_addr, sizeof(srv_addr));
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
    return 0;
}
