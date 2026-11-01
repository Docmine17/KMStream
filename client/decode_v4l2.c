#include "decode_v4l2.h"
#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <libavutil/opt.h>

#define DIE(fmt, ...) do { fprintf(stderr, "FATAL: " fmt "\n", ##__VA_ARGS__); exit(1); } while(0)
#define DIE_IF(cond, fmt, ...) do { if (cond) DIE(fmt, ##__VA_ARGS__); } while(0)

static inline uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static enum AVPixelFormat get_drm_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts) {
    (void)ctx;
    for (const enum AVPixelFormat *p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == AV_PIX_FMT_DRM_PRIME) return *p;
    }
    fprintf(stderr, "[V4L2] DRM_PRIME not offered, falling back to %d\n", pix_fmts[0]);
    return pix_fmts[0];
}

void decode_v4l2_init(struct decode_v4l2_ctx *ctx) {
    memset(ctx, 0, sizeof(*ctx));

    const AVCodec *codec = avcodec_find_decoder_by_name("h264_v4l2m2m");
    DIE_IF(!codec, "Decoder h264_v4l2m2m not found");

    ctx->dec_ctx = avcodec_alloc_context3(codec);
    DIE_IF(!ctx->dec_ctx, "avcodec_alloc_context3 failed");

    ctx->dec_ctx->get_format = get_drm_format;
    ctx->dec_ctx->flags  |= AV_CODEC_FLAG_LOW_DELAY;
    ctx->dec_ctx->flags2 |= AV_CODEC_FLAG2_FAST;
    ctx->dec_ctx->thread_count = 1;
    ctx->dec_ctx->extra_hw_frames = 2;

    AVDictionary *opts = NULL;
    av_dict_set(&opts, "num_output_buffers",  "4", 0);
    av_dict_set(&opts, "num_capture_buffers", "6", 0);

    int ret = avcodec_open2(ctx->dec_ctx, codec, &opts);
    av_dict_free(&opts);
    DIE_IF(ret < 0, "avcodec_open2(h264_v4l2m2m) failed: %d", ret);

    ctx->pkt = av_packet_alloc();
    DIE_IF(!ctx->pkt, "av_packet_alloc failed");
}

AVFrame *decode_v4l2_step(struct decode_v4l2_ctx *ctx, const uint8_t *data,
                          uint32_t size, uint32_t cur_frame_id,
                          uint64_t *t_send_done_out) {
    ctx->pkt->data = (uint8_t *)data;
    ctx->pkt->size = (int)size;
    ctx->pkt->pts  = (int64_t)cur_frame_id;

    int ret = avcodec_send_packet(ctx->dec_ctx, ctx->pkt);
    *t_send_done_out = now_us();
    if (ret < 0 && ret != AVERROR(EAGAIN)) {
        return NULL;
    }

    AVFrame *latest_frame = NULL;
    AVFrame *temp_frame   = av_frame_alloc();

    /*
     * In 720p (<= 720p), VideoCore IV finishes decoding `cur_frame_id` in ~11 ms
     * when flushed by our trailing AUD NAL, so waiting for `pts == cur_frame_id`
     * achieves Hold: 0f (~14.5 ms E2E) at 60 FPS.
     * In 1080p (> 720p, 2.26x macroblocks), VPU decode overlaps with the next frame
     * interval; returning as soon as a decoded frame is ready (0.02 ms) keeps VPU
     * and non-blocking atomic KMS pipelined at full 50-55 FPS with ~20 ms E2E.
     */
    for (int poll_iter = 0; poll_iter < 42; poll_iter++) {
        while (1) {
            ret = avcodec_receive_frame(ctx->dec_ctx, temp_frame);
            if (ret == 0) {
                if (latest_frame) av_frame_free(&latest_frame);
                latest_frame = temp_frame;
                temp_frame   = av_frame_alloc();
            } else {
                break;
            }
        }
        if (latest_frame &&
            (latest_frame->height > 720 || latest_frame->pts == (int64_t)cur_frame_id)) {
            break;
        }
        struct timespec ts_sleep = { .tv_sec = 0, .tv_nsec = 250000 }; /* 0.25 ms */
        nanosleep(&ts_sleep, NULL);
    }
    av_frame_free(&temp_frame);
    return latest_frame;
}

void decode_v4l2_cleanup(struct decode_v4l2_ctx *ctx) {
    if (ctx->pkt) av_packet_free(&ctx->pkt);
    if (ctx->dec_ctx) avcodec_free_context(&ctx->dec_ctx);
}
