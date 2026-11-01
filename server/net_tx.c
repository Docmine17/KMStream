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

void net_tx_init(struct net_tx_ctx *ctx, const char *pi_ip) {
    memset(ctx, 0, sizeof(*ctx));

    ctx->video_sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    DIE_IF(ctx->video_sock < 0, "socket: %s", strerror(errno));
    int sndbuf = 2 * 1024 * 1024;
    int tos = IPTOS_LOWDELAY;
    setsockopt(ctx->video_sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    setsockopt(ctx->video_sock, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));

    ctx->pi_addr.sin_family = AF_INET;
    ctx->pi_addr.sin_port   = htons(KMS_VIDEO_PORT);
    DIE_IF(inet_pton(AF_INET, pi_ip, &ctx->pi_addr.sin_addr) != 1, "invalid target IP '%s'", pi_ip);

    ctx->ctrl_sock = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    DIE_IF(ctx->ctrl_sock < 0, "ctrl socket: %s", strerror(errno));
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

    while (1) {
        peer_len = sizeof(peer);
        ssize_t n = recvfrom(ctx->ctrl_sock, buf, sizeof(buf), MSG_DONTWAIT,
                             (struct sockaddr *)&peer, &peer_len);
        if (n < (ssize_t)sizeof(struct kms_ctrl_hdr)) break;

#if KMSTREAM_STATS
        uint64_t t1 = now_us();
#endif
        const struct kms_ctrl_hdr *h = (const struct kms_ctrl_hdr *)buf;
        if (h->version != KMS_PROTO_VERSION) continue;

        if (h->type == KMS_CTRL_IDR_REQUEST && n >= (ssize_t)sizeof(struct kms_ctrl_idr_request)) {
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

void net_tx_wait_until(struct net_tx_ctx *ctx, uint64_t deadline_us,
                       const volatile sig_atomic_t *stop_flag) {
    while (!*stop_flag) {
        net_tx_drain_ctrl(ctx);
        uint64_t now = now_us();
        if (now >= deadline_us) break;
        uint64_t rem_us = deadline_us - now;
        struct timespec ts = {
            .tv_sec  = (time_t)(rem_us / 1000000ULL),
            .tv_nsec = (long)((rem_us % 1000000ULL) * 1000ULL),
        };
        struct pollfd pfd = { .fd = ctx->ctrl_sock, .events = POLLIN };
        if (ppoll(&pfd, 1, &ts, NULL) < 0 && errno == EINTR) {
            break;
        }
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
    if (frame_id <= 5) {
        flags |= KMS_FLAG_STREAM_RESET;
    }

    uint16_t total_chunks = (uint16_t)((size + KMS_CHUNK_SIZE - 1) / KMS_CHUNK_SIZE);
    struct timespec pace_ts;
    if (total_chunks > 6) {
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

        /* Pace bursts > 6 chunks at ~80 Mbps (4 chunks = ~5.7 KB every 560 us) */
        if (total_chunks > 6 && ((c + 1) & 3) == 0 && (c + 1) < total_chunks) {
            ts_add_ns(&pace_ts, 560000L);
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &pace_ts, NULL);
        }
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
