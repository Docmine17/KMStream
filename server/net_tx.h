#ifndef KMSTREAM_NET_TX_H
#define KMSTREAM_NET_TX_H

#include <stdint.h>
#include <netinet/in.h>
#include "protocol.h"

struct net_tx_ctx {
    int                    video_sock;
    int                    ctrl_sock;
    struct sockaddr_in     pi_addr;
    struct kms_ctrl_stats  last_pi_stats;
    int                    has_pi_stats;
    int                    force_idr;
    uint32_t               idr_reqs_total;
    uint32_t               tx_errors;
    uint8_t                udp_pkt[sizeof(struct kms_video_hdr) + KMS_CHUNK_SIZE];
};

void net_tx_init(struct net_tx_ctx *ctx, const char *pi_ip);
void net_tx_drain_ctrl(struct net_tx_ctx *ctx);
void net_tx_wait_until(struct net_tx_ctx *ctx, uint64_t deadline_us);
void net_tx_send_frame(struct net_tx_ctx *ctx, uint32_t frame_id,
                       const uint8_t *data, int size, uint8_t flags,
                       uint64_t capture_ts_us, uint64_t conv_us, uint64_t enc_us);

#endif /* KMSTREAM_NET_TX_H */
