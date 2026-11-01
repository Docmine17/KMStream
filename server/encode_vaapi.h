#ifndef KMSTREAM_ENCODE_VAAPI_H
#define KMSTREAM_ENCODE_VAAPI_H

#include <stdint.h>
#include <va/va.h>
#include <va/va_drmcommon.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>

struct vaapi_enc_ctx {
    AVBufferRef    *hw_device_ctx;
    AVBufferRef    *hw_frames_ref;
    AVFrame        *va_frame;
    AVCodecContext *enc;
    AVPacket       *pkt;
    uint8_t        *patched_buf;
    int             sps_logged;
};

void vaapi_enc_init(struct vaapi_enc_ctx *ctx, int enc_w, int enc_h,
                    int target_fps, int bitrate_mbps,
                    VADRMPRIMESurfaceDescriptor *prime_out);
void vaapi_enc_close_prime(VADRMPRIMESurfaceDescriptor *prime);
int  vaapi_enc_submit(struct vaapi_enc_ctx *ctx, uint32_t frame_id, int force_idr);
int  vaapi_enc_receive(struct vaapi_enc_ctx *ctx, const uint8_t **data_out,
                       int *size_out, uint8_t *flags_out);
void vaapi_enc_unref_pkt(struct vaapi_enc_ctx *ctx);

#endif /* KMSTREAM_ENCODE_VAAPI_H */
