#include "encode_vaapi.h"
#include "protocol.h"
#include "sps_patch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libavutil/opt.h>

#define DIE_IF(cond, fmt, ...) do { \
    if (cond) { \
        fprintf(stderr, "[kmstreamd] FATAL (%s:%d): " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
        exit(1); \
    } \
} while (0)

void vaapi_enc_init(struct vaapi_enc_ctx *ctx, int enc_w, int enc_h,
                    int target_fps, int bitrate_mbps,
                    VADRMPRIMESurfaceDescriptor *prime_out) {
    memset(ctx, 0, sizeof(*ctx));
    memset(prime_out, 0, sizeof(*prime_out));

    DIE_IF(av_hwdevice_ctx_create(&ctx->hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI,
                                  "/dev/dri/renderD128", NULL, 0) < 0,
           "av_hwdevice_ctx_create(VAAPI) failed");
    VADisplay va_dpy = ((AVVAAPIDeviceContext *)((AVHWDeviceContext *)ctx->hw_device_ctx->data)->hwctx)->display;

    ctx->hw_frames_ref = av_hwframe_ctx_alloc(ctx->hw_device_ctx);
    DIE_IF(!ctx->hw_frames_ref, "av_hwframe_ctx_alloc failed");
    AVHWFramesContext *frames_ctx = (AVHWFramesContext *)ctx->hw_frames_ref->data;
    frames_ctx->format            = AV_PIX_FMT_VAAPI;
    frames_ctx->sw_format         = AV_PIX_FMT_NV12;
    frames_ctx->width             = enc_w;
    frames_ctx->height            = enc_h;
    frames_ctx->initial_pool_size = 4;
    DIE_IF(av_hwframe_ctx_init(ctx->hw_frames_ref) < 0, "av_hwframe_ctx_init failed");

    ctx->va_frame = av_frame_alloc();
    DIE_IF(!ctx->va_frame, "av_frame_alloc failed");
    DIE_IF(av_hwframe_get_buffer(ctx->hw_frames_ref, ctx->va_frame, 0) < 0,
           "av_hwframe_get_buffer failed");
    VASurfaceID nv12_surf = (VASurfaceID)(uintptr_t)ctx->va_frame->data[3];

    VAStatus vast = vaExportSurfaceHandle(va_dpy, nv12_surf, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                          VA_EXPORT_SURFACE_WRITE_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS,
                                          prime_out);
    DIE_IF(vast != VA_STATUS_SUCCESS, "vaExportSurfaceHandle failed: %d", vast);

    const AVCodec *codec = avcodec_find_encoder_by_name("h264_vaapi");
    DIE_IF(!codec, "h264_vaapi encoder not found");
    ctx->enc = avcodec_alloc_context3(codec);
    DIE_IF(!ctx->enc, "avcodec_alloc_context3 failed");
    ctx->enc->width        = enc_w;
    ctx->enc->height       = enc_h;
    ctx->enc->time_base    = (AVRational){1, target_fps};
    ctx->enc->framerate    = (AVRational){target_fps, 1};
    ctx->enc->pix_fmt      = AV_PIX_FMT_VAAPI;
    ctx->enc->max_b_frames = 0;
    ctx->enc->gop_size     = 600;
    ctx->enc->refs         = 1;
    ctx->enc->flags       |= AV_CODEC_FLAG_LOW_DELAY;
    ctx->enc->hw_frames_ctx = av_buffer_ref(ctx->hw_frames_ref);

    if (bitrate_mbps > 0) {
        ctx->enc->bit_rate       = (int64_t)bitrate_mbps * 1000000LL;
        ctx->enc->rc_max_rate    = (int64_t)(bitrate_mbps + 4) * 1000000LL;
        ctx->enc->rc_buffer_size = (int)(ctx->enc->bit_rate / 45);
        av_opt_set(ctx->enc->priv_data, "rc_mode", "VBR", 0);
    } else {
        ctx->enc->global_quality = (enc_h <= 720) ? 24 : 26;
        av_opt_set(ctx->enc->priv_data, "rc_mode", "CQP", 0);
    }
    av_opt_set(ctx->enc->priv_data, "profile", "constrained_baseline", 0);
    av_opt_set_int(ctx->enc->priv_data, "idr_interval", 1, 0);
    av_opt_set_int(ctx->enc->priv_data, "aud", 1, 0);
    DIE_IF(avcodec_open2(ctx->enc, codec, NULL) < 0, "avcodec_open2(h264_vaapi) failed");

    ctx->pkt         = av_packet_alloc();
    ctx->patched_buf = malloc(KMS_MAX_FRAME);
    DIE_IF(!ctx->pkt || !ctx->patched_buf, "allocation failed");
}

void vaapi_enc_close_prime(VADRMPRIMESurfaceDescriptor *prime) {
    for (uint32_t i = 0; i < prime->num_objects; i++) {
        if (prime->objects[i].fd >= 0) {
            close(prime->objects[i].fd);
            prime->objects[i].fd = -1;
        }
    }
}

int vaapi_enc_submit(struct vaapi_enc_ctx *ctx, uint32_t frame_id, int force_idr) {
    ctx->va_frame->pts = frame_id;
    if (force_idr) {
        ctx->va_frame->pict_type = AV_PICTURE_TYPE_I;
        ctx->va_frame->flags    |= AV_FRAME_FLAG_KEY;
    }
    int ret = avcodec_send_frame(ctx->enc, ctx->va_frame);
    if (force_idr) {
        ctx->va_frame->pict_type = AV_PICTURE_TYPE_NONE;
        ctx->va_frame->flags    &= ~AV_FRAME_FLAG_KEY;
    }
    return ret;
}

int vaapi_enc_receive(struct vaapi_enc_ctx *ctx, const uint8_t **data_out,
                      int *size_out, uint8_t *flags_out) {
    int ret = avcodec_receive_packet(ctx->enc, ctx->pkt);
    if (ret != 0) return ret;

    static const uint8_t trailing_aud[6] = { 0x00, 0x00, 0x00, 0x01, 0x09, 0xF0 };
    const uint8_t *src_ptr = ctx->pkt->data;
    int src_size = ctx->pkt->size;
    uint8_t flags = 0;

    if (ctx->pkt->flags & AV_PKT_FLAG_KEY) {
        flags |= KMS_FLAG_IDR;
        int patched = 0;
        int new_sz = sps_patch_packet(ctx->pkt->data, ctx->pkt->size,
                                      ctx->patched_buf, KMS_MAX_FRAME - 16, &patched);
        if (patched) {
            src_ptr  = ctx->patched_buf;
            src_size = new_sz;
            flags   |= KMS_FLAG_SPS_PATCHED;
            if (!ctx->sps_logged) {
                printf("-> [SPS Patch] VUI rewritten (BT.709, num_reorder_frames=0, max_dec_frame_buffering=1)\n");
                fflush(stdout);
                ctx->sps_logged = 1;
            }
        }
    }

    /*
     * Append a trailing Access Unit Delimiter (00 00 00 01 09 F0) at the end of
     * every encoded frame so the VideoCore IV bitstream parser on the Pi sees the
     * boundary of the current access unit immediately without waiting for frame N+1.
     */
    if (src_ptr != ctx->patched_buf && src_size + (int)sizeof(trailing_aud) <= KMS_MAX_FRAME) {
        memcpy(ctx->patched_buf, src_ptr, (size_t)src_size);
        src_ptr = ctx->patched_buf;
    }
    if (src_ptr == ctx->patched_buf && src_size + (int)sizeof(trailing_aud) <= KMS_MAX_FRAME) {
        memcpy(ctx->patched_buf + src_size, trailing_aud, sizeof(trailing_aud));
        src_size += (int)sizeof(trailing_aud);
    }

    *data_out  = src_ptr;
    *size_out  = src_size;
    *flags_out = flags;
    return 0;
}

void vaapi_enc_unref_pkt(struct vaapi_enc_ctx *ctx) {
    av_packet_unref(ctx->pkt);
}
