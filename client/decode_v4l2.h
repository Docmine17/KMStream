#ifndef KMSTREAM_DECODE_V4L2_H
#define KMSTREAM_DECODE_V4L2_H

#include <stdint.h>
#include <libavcodec/avcodec.h>

struct decode_v4l2_ctx {
    AVCodecContext *dec_ctx;
    AVPacket       *pkt;
};

void     decode_v4l2_init(struct decode_v4l2_ctx *ctx);
AVFrame *decode_v4l2_step(struct decode_v4l2_ctx *ctx, const uint8_t *data,
                          uint32_t size, uint32_t cur_frame_id,
                          uint64_t *t_send_done_out);
void     decode_v4l2_cleanup(struct decode_v4l2_ctx *ctx);

#endif /* KMSTREAM_DECODE_V4L2_H */
