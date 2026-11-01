#ifndef KMSTREAM_NET_RX_H
#define KMSTREAM_NET_RX_H

#include <stdint.h>
#include <pthread.h>
#include <netinet/in.h>
#include "protocol.h"

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
    int                actual_rcvbuf;
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

struct rx_shared *net_rx_init_and_start(void);
void              net_rx_wait_for_ready(struct rx_shared *sh);

#endif /* KMSTREAM_NET_RX_H */
