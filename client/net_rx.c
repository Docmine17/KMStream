#include "net_rx.h"
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
        fprintf(stderr, "[kmstream-pi] FATAL (%s:%d): " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
        exit(1); \
    } \
} while (0)

#define SERVER_CONNECT_TIMEOUT_US 5000000ULL /* 5.0s waiting for initial frame */
#define SERVER_STREAM_TIMEOUT_US  2500000ULL /* 2.5s without packets during stream */

static volatile sig_atomic_t *g_rx_stop_flag = NULL;

static inline uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static void send_start_packet_locked(struct rx_shared *sh, uint64_t now) {
    sh->last_keepalive_us = now;
    struct kms_ctrl_start pkt = {
        .h = {
            .version  = KMS_PROTO_VERSION,
            .type     = KMS_CTRL_START,
            .reserved = 0,
            .seq      = ++sh->ctrl_seq,
        },
        .width        = sh->req_width,
        .height       = sh->req_height,
        .fps          = sh->req_fps,
        .bitrate_mbps = sh->req_bitrate_mbps,
        .rc_mode      = sh->req_rc_mode,
        .strict_rc    = sh->req_strict_rc,
        .cqp          = sh->req_cqp,
        .reserved_pad = 0,
    };
    sendto(sh->ctrl_sock, &pkt, sizeof(pkt), MSG_DONTWAIT,
           (const struct sockaddr *)&sh->srv_ctrl_addr, sizeof(sh->srv_ctrl_addr));
}

static void send_keepalive_locked(struct rx_shared *sh, uint64_t now) {
    sh->last_keepalive_us = now;
    struct kms_ctrl_hdr pkt = {
        .version  = KMS_PROTO_VERSION,
        .type     = KMS_CTRL_KEEPALIVE,
        .reserved = 0,
        .seq      = ++sh->ctrl_seq,
    };
    sendto(sh->ctrl_sock, &pkt, sizeof(pkt), MSG_DONTWAIT,
           (const struct sockaddr *)&sh->srv_ctrl_addr, sizeof(sh->srv_ctrl_addr));
}

#if KMSTREAM_STATS
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

static void maybe_send_ping_locked(struct rx_shared *sh, uint64_t now) {
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

void net_rx_send_stats(struct rx_shared *sh, const struct kms_ctrl_stats *st) {
    pthread_mutex_lock(&sh->mtx);
    sendto(sh->ctrl_sock, st, sizeof(*st), MSG_DONTWAIT,
           (const struct sockaddr *)&sh->srv_ctrl_addr, sizeof(sh->srv_ctrl_addr));
    pthread_mutex_unlock(&sh->mtx);
}
#endif /* KMSTREAM_STATS */

static void drain_ctrl_rx_locked(struct rx_shared *sh) {
    uint8_t buf[128];
    while (1) {
        ssize_t n = recv(sh->ctrl_sock, buf, sizeof(buf), MSG_DONTWAIT);
        if (n < (ssize_t)sizeof(struct kms_ctrl_hdr)) break;
#if KMSTREAM_STATS
        uint64_t t3 = now_us();
#endif
        const struct kms_ctrl_hdr *h = (const struct kms_ctrl_hdr *)buf;
        if (h->version != KMS_PROTO_VERSION) continue;

        if (h->type == KMS_CTRL_STOP) {
            sh->server_stopped = 1;
            if (g_rx_stop_flag) *g_rx_stop_flag = 1;
            pthread_cond_broadcast(&sh->cond);
        }
#if KMSTREAM_STATS
        else if (h->type == KMS_CTRL_PONG && n >= (ssize_t)sizeof(struct kms_ctrl_pong)) {
            clock_sync_on_pong(&sh->cs, (const struct kms_ctrl_pong *)buf, t3);
        }
#endif
    }
}

static void send_idr_request_locked(struct rx_shared *sh, uint64_t now) {
    sh->waiting_for_idr = 1;
    uint64_t cooldown_us = sh->first_idr_received ? 200000ULL : 25000ULL;
    if (sh->last_idr_req_us > 0 && (now - sh->last_idr_req_us < cooldown_us)) return;
    sh->last_idr_req_us = now;
    sh->idr_requests_sent++;

    struct kms_ctrl_idr_request req = {
        .h = {
            .version  = KMS_PROTO_VERSION,
            .type     = KMS_CTRL_IDR_REQUEST,
            .reserved = 0,
            .seq      = ++sh->ctrl_seq,
        },
        .last_good_frame = sh->last_completed_id,
    };
    sendto(sh->ctrl_sock, &req, sizeof(req), MSG_DONTWAIT,
           (const struct sockaddr *)&sh->srv_ctrl_addr, sizeof(sh->srv_ctrl_addr));
}

void net_rx_request_idr_locked(struct rx_shared *sh) {
    /* Respect the 200 ms IDR cooldown so transient queue fullness in 1080p60 never triggers an IDR storm */
    send_idr_request_locked(sh, now_us());
}

static void reset_slot(struct rx_slot *s, const struct kms_video_hdr *hdr, uint64_t t_pkt) {
#if !KMSTREAM_STATS
    (void)t_pkt;
#endif
    uint16_t words = (uint16_t)((hdr->total_chunks + 63) / 64);
    if (words > BMAP_WORDS) words = BMAP_WORDS;
    memset(s->chunk_bmap, 0, (size_t)words * sizeof(uint64_t));
    s->frame_id      = hdr->frame_id;
    s->frame_size    = hdr->frame_size;
    s->total_chunks  = hdr->total_chunks;
    s->chunks_unique = 0;
    s->flags         = hdr->flags;
    s->state         = SLOT_ASSEMBLING;
#if KMSTREAM_STATS
    s->capture_ts_us = hdr->capture_ts_us;
    s->t_rx_first_us = t_pkt;
    s->t_rx_last_us  = t_pkt;
    s->t_convert_us  = hdr->t_convert_us;
    s->t_encode_us   = hdr->t_encode_us;
#endif
}

static void *rx_thread_fn(void *arg) {
    struct rx_shared *sh = (struct rx_shared *)arg;
    uint8_t pkt_buf[sizeof(struct kms_video_hdr) + KMS_CHUNK_SIZE + 64];
    int cur_slot_idx = -1;
    uint32_t cur_frame_id = 0;
    int version_mismatch_logged = 0;

    struct pollfd pfds[2] = {
        { .fd = sh->video_sock, .events = POLLIN },
        { .fd = sh->ctrl_sock,  .events = POLLIN },
    };

    while (!g_rx_stop_flag || !*g_rx_stop_flag) {
        poll(pfds, 2, 20);
        if (g_rx_stop_flag && *g_rx_stop_flag) break;

        uint64_t now = now_us();
        pthread_mutex_lock(&sh->mtx);
        drain_ctrl_rx_locked(sh);
        if (g_rx_stop_flag && *g_rx_stop_flag) {
            pthread_mutex_unlock(&sh->mtx);
            break;
        }

        if (sh->last_rx_us == 0) {
            /* Waiting for server to start streaming: retransmit KMS_CTRL_START every 200ms */
            if (now - sh->start_sent_us > SERVER_CONNECT_TIMEOUT_US) {
                fprintf(stderr, "[kmstream-pi] Connection timed out (no response from server).\n");
                sh->server_stopped = 1;
                if (g_rx_stop_flag) *g_rx_stop_flag = 1;
                pthread_cond_broadcast(&sh->cond);
                pthread_mutex_unlock(&sh->mtx);
                break;
            }
            if (now - sh->last_keepalive_us >= 200000ULL) {
                send_start_packet_locked(sh, now);
            }
        } else {
            /* Stream active: check for 2.5s server silence and send 500ms keepalives */
            if (now - sh->last_rx_us > SERVER_STREAM_TIMEOUT_US) {
                printf("[kmstream-pi] Server stream stopped.\n");
                fflush(stdout);
                sh->server_stopped = 1;
                if (g_rx_stop_flag) *g_rx_stop_flag = 1;
                pthread_cond_broadcast(&sh->cond);
                pthread_mutex_unlock(&sh->mtx);
                break;
            }
            if (now - sh->last_keepalive_us >= 500000ULL) {
                send_keepalive_locked(sh, now);
            }
            if (sh->waiting_for_idr) {
                send_idr_request_locked(sh, now);
            }
#if KMSTREAM_STATS
            maybe_send_ping_locked(sh, now);
#endif
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
            if (hdr->version != KMS_PROTO_VERSION) {
                if (!version_mismatch_logged) {
                    fprintf(stderr,
                            "[kmstream-pi] Protocol version mismatch: got v%u, expected v%u (ensure both PC and Pi use same stats=0/1 build)\n",
                            hdr->version, KMS_PROTO_VERSION);
                    version_mismatch_logged = 1;
                }
                continue;
            }
            if (hdr->total_chunks == 0 || hdr->total_chunks > KMS_MAX_CHUNKS) continue;
            if (hdr->chunk_idx >= hdr->total_chunks || hdr->payload_len > KMS_CHUNK_SIZE) continue;
            if ((size_t)n < sizeof(struct kms_video_hdr) + hdr->payload_len) continue;

            const uint8_t *payload = pkt_buf + sizeof(struct kms_video_hdr);

            pthread_mutex_lock(&sh->mtx);
            sh->last_rx_us = t_pkt;

            if (hdr->frame_id < cur_frame_id) {
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
                } else if (!sh->first_idr_received && !(hdr->flags & KMS_FLAG_IDR)) {
                    send_idr_request_locked(sh, t_pkt);
                }

                cur_frame_id = hdr->frame_id;
                cur_slot_idx = -1;

                /* Only gate P-frames before the very first IDR of the session has been received */
                if (!sh->first_idr_received && !(hdr->flags & KMS_FLAG_IDR)) {
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
                    if (oldest_idx >= 0) {
                        cur_slot_idx = oldest_idx;
                        sh->rx_drops++;
                        send_idr_request_locked(sh, t_pkt);
                    } else {
                        cur_slot_idx = 0;
                    }
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
#if KMSTREAM_STATS
                        s->t_rx_last_us = t_pkt;
#endif
                        s->state = SLOT_READY;
                        sh->last_completed_id = s->frame_id;
                        sh->frames_completed++;
                        if (s->flags & KMS_FLAG_IDR) {
                            sh->first_idr_received = 1;
                            sh->waiting_for_idr    = 0;
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

struct rx_shared *net_rx_init_and_start(const char *server_ip,
                                        uint16_t width, uint16_t height,
                                        uint16_t fps, uint16_t bitrate_mbps,
                                        uint8_t rc_mode, uint8_t strict_rc, uint8_t cqp,
                                        volatile sig_atomic_t *stop_flag) {
    g_rx_stop_flag = stop_flag;

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

    struct rx_shared *sh = calloc(1, sizeof(*sh));
    DIE_IF(!sh, "calloc rx_shared failed");
    pthread_mutex_init(&sh->mtx, NULL);
    pthread_cond_init(&sh->cond, NULL);
    sh->video_sock       = sock;
    sh->ctrl_sock        = ctrl_sock;
    sh->actual_rcvbuf    = actual_rcvbuf;
    sh->req_width        = width;
    sh->req_height       = height;
    sh->req_fps          = fps;
    sh->req_bitrate_mbps = bitrate_mbps;
    sh->req_rc_mode      = rc_mode;
    sh->req_strict_rc    = strict_rc;
    sh->req_cqp          = cqp;
    sh->waiting_for_idr  = 0;

    sh->srv_ctrl_addr.sin_family = AF_INET;
    sh->srv_ctrl_addr.sin_port   = htons(KMS_CTRL_PORT);
    DIE_IF(inet_pton(AF_INET, server_ip, &sh->srv_ctrl_addr.sin_addr) != 1,
           "invalid server IP '%s'", server_ip);

    uint64_t now = now_us();
    sh->start_sent_us   = now;
    sh->last_idr_req_us = now;
    send_start_packet_locked(sh, now);
#if KMSTREAM_STATS
    maybe_send_ping_locked(sh, now);
#endif

    DIE_IF(pthread_create(&sh->rx_tid, NULL, rx_thread_fn, sh) != 0, "pthread_create failed");
    return sh;
}

int net_rx_wait_for_ready(struct rx_shared *sh,
                          const volatile sig_atomic_t *stop_flag) {
    while (1) {
        if ((stop_flag && *stop_flag) || sh->server_stopped) {
            return -1;
        }
        for (int i = 0; i < RX_RING_SLOTS; i++) {
            if (sh->slots[i].state == SLOT_READY) {
                return 1;
            }
        }

        struct timespec wait_ts;
        clock_gettime(CLOCK_REALTIME, &wait_ts);
        wait_ts.tv_nsec += 50000000L; /* 50 ms */
        if (wait_ts.tv_nsec >= 1000000000L) {
            wait_ts.tv_sec++;
            wait_ts.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&sh->cond, &sh->mtx, &wait_ts);
    }
}

void net_rx_cleanup(struct rx_shared *sh) {
    if (!sh) return;
    if (g_rx_stop_flag) *g_rx_stop_flag = 1;
    pthread_cond_broadcast(&sh->cond);
    pthread_join(sh->rx_tid, NULL);

    if (!sh->server_stopped && sh->ctrl_sock >= 0) {
        struct kms_ctrl_hdr stop_pkt = {
            .version  = KMS_PROTO_VERSION,
            .type     = KMS_CTRL_STOP,
            .reserved = 0,
            .seq      = ++sh->ctrl_seq,
        };
        for (int i = 0; i < 3; i++) {
            sendto(sh->ctrl_sock, &stop_pkt, sizeof(stop_pkt), MSG_DONTWAIT,
                   (const struct sockaddr *)&sh->srv_ctrl_addr, sizeof(sh->srv_ctrl_addr));
        }
    }

    if (sh->video_sock >= 0) close(sh->video_sock);
    if (sh->ctrl_sock >= 0)  close(sh->ctrl_sock);
    pthread_cond_destroy(&sh->cond);
    pthread_mutex_destroy(&sh->mtx);
    free(sh);
}
