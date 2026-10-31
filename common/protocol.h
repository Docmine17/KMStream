#ifndef KMSTREAM_PROTOCOL_H
#define KMSTREAM_PROTOCOL_H

#include <stdint.h>

_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "little-endian required");

#define KMS_PROTO_VERSION 1
#define KMS_VIDEO_PORT    5000   /* PC -> Pi */
#define KMS_CTRL_PORT     5001   /* Pi -> PC (back-channel) */
#define KMS_CHUNK_SIZE    1400   /* header (32) + payload (1400) < 1472 (MTU 1500) */
#define KMS_MAX_FRAME     (2 * 1024 * 1024)
#define KMS_MAX_CHUNKS    ((KMS_MAX_FRAME + KMS_CHUNK_SIZE - 1) / KMS_CHUNK_SIZE)

enum {
    KMS_FLAG_IDR         = 1 << 0,
    KMS_FLAG_SPS_PATCHED = 1 << 1,
};

struct __attribute__((packed)) kms_video_hdr {
    uint8_t  version;
    uint8_t  flags;
    uint16_t chunk_idx;
    uint16_t total_chunks;
    uint16_t payload_len;
    uint32_t frame_id;
    uint32_t frame_size;         /* total frame size in bytes (detects loss of last chunk) */
    uint64_t capture_ts_us;      /* PC CLOCK_MONOTONIC_RAW timestamp (filled in Phase 1) */
    uint16_t t_convert_us;       /* GLES RGB->NV12 conversion time (Phase 1) */
    uint16_t t_encode_us;        /* VAAPI encode time (Phase 1) */
    uint8_t  slice_idx;          /* multi-slice index (Phase 2/3) */
    uint8_t  total_slices;       /* total slices in frame (Phase 2/3) */
    uint16_t reserved;
};
_Static_assert(sizeof(struct kms_video_hdr) == 32, "kms_video_hdr must be 32 bytes");

enum kms_ctrl_type {
    KMS_CTRL_IDR_REQUEST = 1,
    KMS_CTRL_PING        = 2,
    KMS_CTRL_PONG        = 3,
    KMS_CTRL_STATS       = 4,
    KMS_CTRL_NACK        = 5,
};

struct __attribute__((packed)) kms_ctrl_hdr {
    uint8_t  version;
    uint8_t  type;
    uint16_t reserved;
    uint32_t seq;
};
_Static_assert(sizeof(struct kms_ctrl_hdr) == 8, "kms_ctrl_hdr must be 8 bytes");

struct __attribute__((packed)) kms_ctrl_idr_request {
    struct kms_ctrl_hdr h;
    uint32_t last_good_frame;
};
_Static_assert(sizeof(struct kms_ctrl_idr_request) == 12, "kms_ctrl_idr_request size mismatch");

struct __attribute__((packed)) kms_ctrl_nack {
    struct kms_ctrl_hdr h;
    uint32_t frame_id;
    uint16_t missing_chunk_idx;
    uint16_t reserved;
};
_Static_assert(sizeof(struct kms_ctrl_nack) == 16, "kms_ctrl_nack size mismatch");

struct __attribute__((packed)) kms_ctrl_ping {
    struct kms_ctrl_hdr h;
    uint64_t t0_us;
};
_Static_assert(sizeof(struct kms_ctrl_ping) == 16, "kms_ctrl_ping size mismatch");

struct __attribute__((packed)) kms_ctrl_pong {
    struct kms_ctrl_hdr h;
    uint64_t t0_us;
    uint64_t t1_us;
    uint64_t t2_us;
};
_Static_assert(sizeof(struct kms_ctrl_pong) == 32, "kms_ctrl_pong size mismatch");

struct __attribute__((packed)) kms_ctrl_stats {
    struct kms_ctrl_hdr h;
    uint32_t frames_ok;
    uint32_t frames_lost;
    uint32_t rx_drops;
    uint16_t decode_us_avg;
    uint16_t decode_us_p99;
    uint16_t display_us_avg;
    uint16_t display_us_p99;
};
_Static_assert(sizeof(struct kms_ctrl_stats) == 28, "kms_ctrl_stats size mismatch");

#endif /* KMSTREAM_PROTOCOL_H */
