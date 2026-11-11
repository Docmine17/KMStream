#ifndef KMSTREAM_DECODE_V4L2_H
#define KMSTREAM_DECODE_V4L2_H

#include <stdint.h>
#include <stddef.h>
#include <signal.h>
#include <pthread.h>
#include <linux/videodev2.h>

#define V4L2_NUM_OUTPUT_BUFS   4
#define V4L2_MAX_CAPTURE_BUFS  20
#define V4L2_MIN_CAPTURE_BUFS  6
#define V4L2_CAPTURE_CUSHION   4
#define V4L2_OUTPUT_BUF_SIZE   (2 * 1024 * 1024)

struct display_kms_ctx;

struct v4l2_output_buf {
    void   *start;
    size_t  length;
    int     queued;
};

struct v4l2_capture_buf {
    int      export_fd;
    uint32_t gem_handle;
    uint32_t fb_id;
    uint32_t frame_id;
    uint64_t t_dqbuf_us;
    int      queued;
};

struct decode_v4l2_ctx {
    int      v4l2_fd;
    int      output_streaming;
    int      capture_streaming;
    int      num_output_bufs;
    int      num_capture_bufs;

    uint32_t coded_width;
    uint32_t coded_height;
    uint32_t visible_width;
    uint32_t visible_height;
    uint32_t stride;

    struct v4l2_output_buf  output_bufs[V4L2_NUM_OUTPUT_BUFS];
    struct v4l2_capture_buf capture_bufs[V4L2_MAX_CAPTURE_BUFS];

    pthread_mutex_t v4l2_lock;
    pthread_cond_t  drc_cond;
    int             drc_in_progress;
};

void decode_v4l2_init(struct decode_v4l2_ctx *ctx);
int  decode_v4l2_submit(struct decode_v4l2_ctx *ctx, const uint8_t *data,
                        uint32_t size, uint32_t frame_id,
                        const volatile sig_atomic_t *stop_flag);
int  decode_v4l2_handle_drc(struct decode_v4l2_ctx *ctx, struct display_kms_ctx *kms);
void decode_v4l2_requeue_capture(struct decode_v4l2_ctx *ctx, int index);
void decode_v4l2_cleanup(struct decode_v4l2_ctx *ctx);

#endif /* KMSTREAM_DECODE_V4L2_H */
