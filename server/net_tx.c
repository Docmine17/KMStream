#include "net_tx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <time.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/ip.h>

#define DIE_IF(cond, fmt, ...) do { \
    if (cond) { \
        fprintf(stderr, "[kmstreamd] FATAL (%s:%d): " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
        exit(1); \
    } \
} while (0)

static inline uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

#if KMSTREAM_STATS
static inline uint16_t clamp_u16(uint64_t v) {
    return (v > 65535ULL) ? 65535U : (uint16_t)v;
}
#endif

static inline void ts_add_ns(struct timespec *ts, long ns) {
    ts->tv_nsec += ns;
    while (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

void net_tx_init(struct net_tx_ctx *ctx) {
    memset(ctx, 0, sizeof(*ctx));

    ctx->video_sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    DIE_IF(ctx->video_sock < 0, "socket: %s", strerror(errno));
    int sndbuf   = 2 * 1024 * 1024;
    int tos      = IPTOS_LOWDELAY;
    int prio_vid = 5;
    int prio_ctl = 6;
    setsockopt(ctx->video_sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    setsockopt(ctx->video_sock, SOL_SOCKET, SO_PRIORITY, &prio_vid, sizeof(prio_vid));
    setsockopt(ctx->video_sock, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));

    ctx->ctrl_sock = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    DIE_IF(ctx->ctrl_sock < 0, "ctrl socket: %s", strerror(errno));
    setsockopt(ctx->ctrl_sock, SOL_SOCKET, SO_PRIORITY, &prio_ctl, sizeof(prio_ctl));
    setsockopt(ctx->ctrl_sock, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
    struct sockaddr_in ctrl_addr = {
        .sin_family = AF_INET, .sin_port = htons(KMS_CTRL_PORT), .sin_addr.s_addr = INADDR_ANY
    };
    DIE_IF(bind(ctx->ctrl_sock, (struct sockaddr *)&ctrl_addr, sizeof(ctrl_addr)) < 0,
           "bind ctrl port %d: %s", KMS_CTRL_PORT, strerror(errno));
}

void net_tx_drain_ctrl(struct net_tx_ctx *ctx) {
    uint8_t buf[128];
    struct sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);
    static int version_mismatch_logged = 0;

    while (1) {
        peer_len = sizeof(peer);
        ssize_t n = recvfrom(ctx->ctrl_sock, buf, sizeof(buf), MSG_DONTWAIT,
                             (struct sockaddr *)&peer, &peer_len);
        if (n < (ssize_t)sizeof(struct kms_ctrl_hdr)) break;

        uint64_t t1 = now_us();
        const struct kms_ctrl_hdr *h = (const struct kms_ctrl_hdr *)buf;
        if (h->version != KMS_PROTO_VERSION) {
            if (!version_mismatch_logged) {
                fprintf(stderr,
                        "[kmstreamd] Protocol version mismatch from client: got v%u, expected v%u (ensure both PC and Pi use same stats=0/1 build)\n",
                        h->version, KMS_PROTO_VERSION);
                version_mismatch_logged = 1;
            }
            continue;
        }

        if (h->type == KMS_CTRL_START && n >= (ssize_t)sizeof(struct kms_ctrl_start)) {
            const struct kms_ctrl_start *st = (const struct kms_ctrl_start *)buf;
            uint16_t w      = st->width ? (st->width & ~1U) : 1280;
            uint16_t ht     = st->height ? (st->height & ~1U) : 720;
            uint16_t fps    = st->fps ? st->fps : 60;
            uint16_t br     = st->bitrate_mbps;
            uint8_t  rc     = (st->rc_mode <= KMS_RC_CQP) ? st->rc_mode : KMS_RC_CBR;
            uint8_t  strict = st->strict_rc ? 1 : 0;
            uint8_t  cqp    = st->cqp ? st->cqp : 22;

            if (w < 320) w = 320;
            if (w > 1920) w = 1920;
            if (ht < 240) ht = 240;
            if (ht > 1080) ht = 1080;
            if (fps < 10) fps = 10;
            if (fps > 120) fps = 120;
            if (br > 50) br = 50;
            if (cqp < 1) cqp = 1;
            if (cqp > 51) cqp = 51;

            ctx->last_client_rx_us = t1;
            ctx->pi_ctrl_addr      = peer;

            if (!ctx->client_connected ||
                ctx->pi_addr.sin_addr.s_addr != peer.sin_addr.s_addr ||
                ctx->req_width != w || ctx->req_height != ht ||
                ctx->req_fps != fps || ctx->req_bitrate_mbps != br ||
                ctx->req_rc_mode != rc || ctx->req_strict_rc != strict ||
                ctx->req_cqp != cqp) {
                ctx->pi_addr.sin_family = AF_INET;
                ctx->pi_addr.sin_addr   = peer.sin_addr;
                ctx->pi_addr.sin_port   = htons(KMS_VIDEO_PORT);
                ctx->req_width          = w;
                ctx->req_height         = ht;
                ctx->req_fps            = fps;
                ctx->req_bitrate_mbps   = br;
                ctx->req_rc_mode        = rc;
                ctx->req_strict_rc      = strict;
                ctx->req_cqp            = cqp;
                ctx->start_requested    = 1;
                ctx->stop_requested     = 0;
            }
            ctx->force_idr = 1;
            continue;
        }

        if (!ctx->client_connected || peer.sin_addr.s_addr != ctx->pi_addr.sin_addr.s_addr) {
            continue;
        }

        ctx->last_client_rx_us = t1;
        ctx->pi_ctrl_addr      = peer;

        if (h->type == KMS_CTRL_STOP) {
            ctx->stop_requested = 1;
        } else if (h->type == KMS_CTRL_IDR_REQUEST && n >= (ssize_t)sizeof(struct kms_ctrl_idr_request)) {
            ctx->force_idr = 1;
            ctx->idr_reqs_total++;
        }
#if KMSTREAM_STATS
        else if (h->type == KMS_CTRL_PING && n >= (ssize_t)sizeof(struct kms_ctrl_ping)) {
            const struct kms_ctrl_ping *ping = (const struct kms_ctrl_ping *)buf;
            struct kms_ctrl_pong pong = {
                .h = {
                    .version  = KMS_PROTO_VERSION,
                    .type     = KMS_CTRL_PONG,
                    .reserved = 0,
                    .seq      = ping->h.seq,
                },
                .t0_us = ping->t0_us,
                .t1_us = t1,
                .t2_us = now_us(),
            };
            sendto(ctx->ctrl_sock, &pong, sizeof(pong), MSG_DONTWAIT,
                   (const struct sockaddr *)&peer, peer_len);
        } else if (h->type == KMS_CTRL_STATS && n >= (ssize_t)sizeof(struct kms_ctrl_stats)) {
            memcpy(&ctx->last_pi_stats, buf, sizeof(ctx->last_pi_stats));
            ctx->has_pi_stats = 1;
        }
#endif
    }
}

void net_tx_wait_idle(struct net_tx_ctx *ctx, const volatile sig_atomic_t *stop_flag) {
    struct pollfd pfd = { .fd = ctx->ctrl_sock, .events = POLLIN };
    while (!*stop_flag && !ctx->start_requested) {
        net_tx_drain_ctrl(ctx);
        if (ctx->start_requested || *stop_flag) break;
        poll(&pfd, 1, 250);
    }
}

void net_tx_pace_frame(struct net_tx_ctx *ctx, uint64_t deadline_us,
                       const volatile sig_atomic_t *stop_flag) {
    while (!*stop_flag && !ctx->stop_requested && !ctx->start_requested) {
        net_tx_drain_ctrl(ctx);
        if (ctx->stop_requested || ctx->start_requested || *stop_flag) break;

        uint64_t now = now_us();
        if (now >= deadline_us) break;

        uint64_t rem_us = deadline_us - now;
        struct timespec ts = {
            .tv_sec  = (time_t)(rem_us / 1000000ULL),
            .tv_nsec = (long)((rem_us % 1000000ULL) * 1000ULL),
        };
        struct pollfd pfd = { .fd = ctx->ctrl_sock, .events = POLLIN };
        int ret = ppoll(&pfd, 1, &ts, NULL);
        if (ret < 0 && errno == EINTR) break;
    }
}

void net_tx_send_frame(struct net_tx_ctx *ctx, uint32_t frame_id,
                       const uint8_t *data, int size, uint8_t flags,
                       uint64_t capture_ts_us, uint64_t conv_us, uint64_t enc_us) {
#if !KMSTREAM_STATS
    (void)capture_ts_us;
    (void)conv_us;
    (void)enc_us;
#endif
    uint16_t total_chunks = (uint16_t)((size + KMS_CHUNK_SIZE - 1) / KMS_CHUNK_SIZE);
    struct timespec pace_ts;
    if (total_chunks > 12) {
        clock_gettime(CLOCK_MONOTONIC, &pace_ts);
    }

    for (uint16_t c = 0; c < total_chunks; c++) {
        int offset = c * KMS_CHUNK_SIZE;
        uint16_t len = (size - offset > KMS_CHUNK_SIZE) ? KMS_CHUNK_SIZE : (uint16_t)(size - offset);
        struct kms_video_hdr *hdr = (struct kms_video_hdr *)ctx->udp_pkt;
        memset(hdr, 0, sizeof(*hdr));
        hdr->version       = KMS_PROTO_VERSION;
        hdr->flags         = flags;
        hdr->chunk_idx     = c;
        hdr->total_chunks  = total_chunks;
        hdr->payload_len   = len;
        hdr->frame_id      = frame_id;
        hdr->frame_size    = (uint32_t)size;
#if KMSTREAM_STATS
        hdr->capture_ts_us = capture_ts_us;
        hdr->t_convert_us  = clamp_u16(conv_us);
        hdr->t_encode_us   = clamp_u16(enc_us);
#endif
        memcpy(ctx->udp_pkt + sizeof(struct kms_video_hdr), data + offset, len);

        ssize_t sret = sendto(ctx->video_sock, ctx->udp_pkt, sizeof(struct kms_video_hdr) + len, 0,
                              (const struct sockaddr *)&ctx->pi_addr, sizeof(ctx->pi_addr));
        if (sret < 0 && (errno == ENOBUFS || errno == EAGAIN)) {
            usleep(100);
            sret = sendto(ctx->video_sock, ctx->udp_pkt, sizeof(struct kms_video_hdr) + len, 0,
                          (const struct sockaddr *)&ctx->pi_addr, sizeof(ctx->pi_addr));
        }
        if (sret < 0) ctx->tx_errors++;

        /* Only pace large IDR bursts (> 12 chunks = > 16.8 KB) every 8 chunks (400 us) */
        if (total_chunks > 12 && ((c + 1) & 7) == 0 && (c + 1) < total_chunks) {
            ts_add_ns(&pace_ts, 400000L);
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &pace_ts, NULL);
        }
    }
}

void net_tx_send_stop(struct net_tx_ctx *ctx) {
    if (!ctx->client_connected || ctx->ctrl_sock < 0) return;
    struct kms_ctrl_hdr stop_pkt = {
        .version  = KMS_PROTO_VERSION,
        .type     = KMS_CTRL_STOP,
        .reserved = 0,
        .seq      = 0,
    };
    for (int i = 0; i < 3; i++) {
        sendto(ctx->ctrl_sock, &stop_pkt, sizeof(stop_pkt), MSG_DONTWAIT,
               (const struct sockaddr *)&ctx->pi_ctrl_addr, sizeof(ctx->pi_ctrl_addr));
    }
}

void net_tx_cleanup(struct net_tx_ctx *ctx) {
    if (ctx->video_sock >= 0) {
        close(ctx->video_sock);
        ctx->video_sock = -1;
    }
    if (ctx->ctrl_sock >= 0) {
        close(ctx->ctrl_sock);
        ctx->ctrl_sock = -1;
    }
}
