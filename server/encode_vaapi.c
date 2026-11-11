#include "encode_vaapi.h"
#include "protocol.h"
#include "sps_patch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>

#define DIE_IF(cond, fmt, ...) do { \
    if (cond) { \
        fprintf(stderr, "[kmstreamd] FATAL (%s:%d): " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
        exit(1); \
    } \
} while (0)

void vaapi_enc_init(struct vaapi_enc_ctx *ctx, int enc_w, int enc_h,
                    int target_fps, int bitrate_mbps,
                    uint8_t rc_mode, uint8_t strict_rc, uint8_t cqp_val,
                    VADRMPRIMESurfaceDescriptor *prime_out) {
    vaapi_enc_cleanup(ctx);
    memset(ctx, 0, sizeof(*ctx));
    memset(prime_out, 0, sizeof(*prime_out));

    DIE_IF(av_hwdevice_ctx_create(&ctx->hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI,
                                  "/dev/dri/renderD128", NULL, 0) < 0,
           "av_hwdevice_ctx_create(VAAPI) failed");
    VADisplay va_dpy = ((AVVAAPIDeviceContext *)((AVHWDeviceContext *)ctx->hw_device_ctx->data)->hwctx)->display;
    const char *va_vendor = vaQueryVendorString(va_dpy);

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
    ctx->va_frame->color_range     = AVCOL_RANGE_MPEG;
    ctx->va_frame->colorspace      = AVCOL_SPC_BT709;
    ctx->va_frame->color_primaries = AVCOL_PRI_BT709;
    ctx->va_frame->color_trc       = AVCOL_TRC_BT709;
    VASurfaceID nv12_surf = (VASurfaceID)(uintptr_t)ctx->va_frame->data[3];

    VAStatus vast = vaExportSurfaceHandle(va_dpy, nv12_surf, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                          VA_EXPORT_SURFACE_WRITE_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS,
                                          prime_out);
    DIE_IF(vast != VA_STATUS_SUCCESS, "vaExportSurfaceHandle failed: %d", vast);

    const AVCodec *codec = avcodec_find_encoder_by_name("h264_vaapi");
    DIE_IF(!codec, "h264_vaapi encoder not found");
    ctx->enc = avcodec_alloc_context3(codec);
    DIE_IF(!ctx->enc, "avcodec_alloc_context3 failed");
    ctx->enc->width             = enc_w;
    ctx->enc->height            = enc_h;
    ctx->enc->time_base         = (AVRational){1, target_fps};
    ctx->enc->framerate         = (AVRational){target_fps, 1};
    ctx->enc->pix_fmt           = AV_PIX_FMT_VAAPI;
    ctx->enc->max_b_frames      = 0;
    ctx->enc->gop_size          = 32767; /* Sunshine LIMITED_GOP_SIZE: avoids Mesa VCE 3.4 GOP budget overflow */
    ctx->enc->keyint_min        = 32767;
    ctx->enc->refs              = 1;
    ctx->enc->slices            = 1;
    ctx->enc->compression_level = 32; /* RX 580 speed preset (matching Sunshine vaapi_quality=speed) */
    ctx->enc->color_range       = AVCOL_RANGE_MPEG;
    ctx->enc->colorspace        = AVCOL_SPC_BT709;
    ctx->enc->color_primaries   = AVCOL_PRI_BT709;
    ctx->enc->color_trc         = AVCOL_TRC_BT709;
    ctx->enc->flags            |= AV_CODEC_FLAG_CLOSED_GOP | AV_CODEC_FLAG_LOW_DELAY;
    ctx->enc->flags2           |= AV_CODEC_FLAG2_FAST;
    ctx->enc->hw_frames_ctx     = av_buffer_ref(ctx->hw_frames_ref);

    if (bitrate_mbps <= 0) {
        rc_mode = KMS_RC_CQP;
    }
    if (cqp_val == 0) {
        cqp_val = (enc_h <= 720) ? 20 : 22;
    }

    const char *rc_name = "CBR";
    char vbv_desc[64]   = "Standard Driver VBV (Sunshine default)";

    if (rc_mode == KMS_RC_CQP) {
        rc_name = "CQP";
        snprintf(vbv_desc, sizeof(vbv_desc), "QP=%u", cqp_val);
        ctx->enc->global_quality = cqp_val;
        av_opt_set(ctx->enc->priv_data, "rc_mode", "CQP", 0);
        av_opt_set_int(ctx->enc->priv_data, "qp", cqp_val, 0);
    } else if (rc_mode == KMS_RC_VBR) {
        rc_name = "VBR";
        ctx->enc->bit_rate    = (int64_t)bitrate_mbps * 1000000LL;
        ctx->enc->rc_max_rate = (ctx->enc->bit_rate * 3) / 2;
        if (strict_rc) {
            ctx->enc->rc_buffer_size = (int)(ctx->enc->bit_rate / target_fps);
            snprintf(vbv_desc, sizeof(vbv_desc), "Strict 1-Frame VBV (%d kbit)",
                     ctx->enc->rc_buffer_size / 1000);
        }
        av_opt_set(ctx->enc->priv_data, "rc_mode", "VBR", 0);
    } else {
        rc_name = "CBR";
        ctx->enc->bit_rate    = (int64_t)bitrate_mbps * 1000000LL;
        ctx->enc->rc_max_rate = ctx->enc->bit_rate;
        if (strict_rc) {
            ctx->enc->rc_buffer_size = (int)(ctx->enc->bit_rate / target_fps);
            snprintf(vbv_desc, sizeof(vbv_desc), "Strict 1-Frame VBV (%d kbit)",
                     ctx->enc->rc_buffer_size / 1000);
        }
        av_opt_set(ctx->enc->priv_data, "rc_mode", "CBR", 0);
    }

    /* H.264 High Profile (CABAC + 8x8 transform) + Sunshine low-latency VAAPI options */
    av_opt_set(ctx->enc->priv_data, "profile", "high", 0);
    av_opt_set_int(ctx->enc->priv_data, "quality", 32, 0);
    av_opt_set_int(ctx->enc->priv_data, "async_depth", 1, 0);
    av_opt_set_int(ctx->enc->priv_data, "sei", 0, 0);
    av_opt_set_int(ctx->enc->priv_data, "idr_interval", 1, 0);
    av_opt_set_int(ctx->enc->priv_data, "aud", 1, 0);

    DIE_IF(avcodec_open2(ctx->enc, codec, NULL) < 0, "avcodec_open2(h264_vaapi) failed");

    printf("[VA-API Enc] Vendor: %s\n", va_vendor ? va_vendor : "Unknown");
    printf("[VA-API Enc] Config: H.264 High Profile (CABAC + 8x8) | %dx%d @ %d FPS | RC=%s",
           enc_w, enc_h, target_fps, rc_name);
    if (rc_mode != KMS_RC_CQP) {
        printf(" (%d Mbps, %s)", bitrate_mbps, vbv_desc);
    } else {
        printf(" (%s)", vbv_desc);
    }
    printf(" | quality=32(speed) slices=1 async_depth=1 BT.709-Limited\n");
    fflush(stdout);

    ctx->pkt         = av_packet_alloc();
    ctx->patched_buf = malloc(KMS_MAX_FRAME);
    DIE_IF(!ctx->pkt || !ctx->patched_buf, "allocation failed");
    ctx->initialized = 1;
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

/*
 * Strip trailing H.264 Filler Data NAL unit (NAL type 12 = 0x0C) emitted by
 * Mesa/VCE in CBR mode so 0xFF padding bytes are not sent over the network or
 * fed into the Raspberry Pi 3B hardware decoder.
 */
static int strip_h264_filler_nal(const uint8_t *buf, int size) {
    if (size < 6) return size;
    int i = size - 1;
    while (i >= 4 && (buf[i] == 0xFF || buf[i] == 0x80 || buf[i] == 0x00)) {
        if (buf[i] == 0x00 && i >= 3 &&
            buf[i - 1] == 0x0C && buf[i - 2] == 0x01 && buf[i - 3] == 0x00 &&
            (i - 4 < 0 || buf[i - 4] == 0x00)) {
            int sc = (i >= 4 && buf[i - 4] == 0x00) ? (i - 4) : (i - 3);
            return (sc > 0) ? sc : size;
        }
        i--;
    }
    if (i >= 3 && (buf[i] & 0x1F) == 12 &&
        buf[i - 1] == 0x01 && buf[i - 2] == 0x00 && buf[i - 3] == 0x00) {
        int sc = (i >= 4 && buf[i - 4] == 0x00) ? (i - 4) : (i - 3);
        if (sc > 0) return sc;
    }
    return size;
}

int vaapi_enc_receive(struct vaapi_enc_ctx *ctx, const uint8_t **data_out,
                      int *size_out, uint8_t *flags_out) {
    int ret = avcodec_receive_packet(ctx->enc, ctx->pkt);
    if (ret != 0) return ret;

    const uint8_t *src_ptr = ctx->pkt->data;
    int src_size = strip_h264_filler_nal(ctx->pkt->data, ctx->pkt->size);
    uint8_t flags = 0;

    if (ctx->pkt->flags & AV_PKT_FLAG_KEY) {
        flags |= KMS_FLAG_IDR;
        int patched = 0;
        int new_sz = sps_patch_packet(src_ptr, src_size,
                                      ctx->patched_buf, KMS_MAX_FRAME - 16, &patched);
        if (patched) {
            src_ptr  = ctx->patched_buf;
            src_size = new_sz;
            flags   |= KMS_FLAG_SPS_PATCHED;
            if (!ctx->sps_logged) {
                printf("-> [SPS Patch] High Profile VUI rewritten (BT.709, num_reorder_frames=0, max_dec_frame_buffering=1)\n");
                fflush(stdout);
                ctx->sps_logged = 1;
            }
        }
    }

    *data_out  = src_ptr;
    *size_out  = src_size;
    *flags_out = flags;
    return 0;
}

void vaapi_enc_unref_pkt(struct vaapi_enc_ctx *ctx) {
    av_packet_unref(ctx->pkt);
}

void vaapi_enc_cleanup(struct vaapi_enc_ctx *ctx) {
    if (!ctx->initialized) return;
    if (ctx->pkt)           av_packet_free(&ctx->pkt);
    if (ctx->enc)           avcodec_free_context(&ctx->enc);
    if (ctx->va_frame)      av_frame_free(&ctx->va_frame);
    if (ctx->hw_frames_ref) av_buffer_unref(&ctx->hw_frames_ref);
    if (ctx->hw_device_ctx) av_buffer_unref(&ctx->hw_device_ctx);
    free(ctx->patched_buf);
    ctx->patched_buf = NULL;
    ctx->initialized = 0;
}
