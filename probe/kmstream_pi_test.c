// kmstream_pi_test.c - Decodificador Hardware V4L2 M2M (DRM_PRIME) -> Scanout KMS Direto no Pi 3B
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/pixdesc.h>

static inline uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

// Forca o decoder h264_v4l2m2m a entregar buffers DRM_PRIME (Zero-Copy DMA-BUF)
static enum AVPixelFormat get_drm_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts) {
    for (const enum AVPixelFormat *p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == AV_PIX_FMT_DRM_PRIME) return *p;
    }
    fprintf(stderr, "Aviso: DRM_PRIME nao disponivel, usando fallback: %s\n", av_get_pix_fmt_name(pix_fmts[0]));
    return pix_fmts[0];
}

// Verifica se um Plane do KMS suporta um formato especifico (ex: YU12 ou NV12)
static int plane_supports_format(drmModePlane *p, uint32_t fmt) {
    for (uint32_t i = 0; i < p->count_formats; i++) {
        if (p->formats[i] == fmt) return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *filename = (argc > 1) ? argv[1] : "teste_kmstream.h264";
    int drm_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) { perror("open /dev/dri/card0"); return 1; }

    drmSetClientCap(drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);

    // 1. Acha o CRTC ativo do conector HDMI-A-1
    drmModeRes *res = drmModeGetResources(drm_fd);
    uint32_t crtc_id = 0;
    int crtc_idx = -1, screen_w = 1920, screen_h = 1080;

    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *conn = drmModeGetConnector(drm_fd, res->connectors[i]);
        if (conn && conn->connection == DRM_MODE_CONNECTED && conn->encoder_id) {
            drmModeEncoder *enc = drmModeGetEncoder(drm_fd, conn->encoder_id);
            if (enc && enc->crtc_id) {
                crtc_id = enc->crtc_id;
                drmModeCrtc *crtc = drmModeGetCrtc(drm_fd, crtc_id);
                if (crtc && crtc->mode_valid) {
                    screen_w = crtc->mode.hdisplay;
                    screen_h = crtc->mode.vdisplay;
                }
                if (crtc) drmModeFreeCrtc(crtc);
            }
            if (enc) drmModeFreeEncoder(enc);
            drmModeFreeConnector(conn);
            if (crtc_id) break;
        }
        if (conn) drmModeFreeConnector(conn);
    }
    for (int i = 0; i < res->count_crtcs; i++) {
        if (res->crtcs[i] == crtc_id) { crtc_idx = i; break; }
    }
    drmModeFreeResources(res);

    printf("=== KMStream Pi 3B Benchmark ===\n");
    printf("HDMI CRTC Ativo: %u (Indice %d) | Resolucao: %dx%d\n", crtc_id, crtc_idx, screen_w, screen_h);

    // 2. Abre o arquivo H.264 e inicializa o decodificador de hardware h264_v4l2m2m
    AVFormatContext *fmt_ctx = NULL;
    if (avformat_open_input(&fmt_ctx, filename, NULL, NULL) < 0) {
        fprintf(stderr, "Erro ao abrir %s\n", filename);
        return 1;
    }
    avformat_find_stream_info(fmt_ctx, NULL);

    const AVCodec *codec = avcodec_find_decoder_by_name("h264_v4l2m2m");
    if (!codec) { fprintf(stderr, "h264_v4l2m2m nao encontrado!\n"); return 1; }

    AVCodecContext *dec = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(dec, fmt_ctx->streams[0]->codecpar);
    dec->get_format = get_drm_format;
    // Importante no Pi: evita retencao excessiva de buffers no V4L2
    dec->extra_hw_frames = 2;

    if (avcodec_open2(dec, codec, NULL) < 0) {
        fprintf(stderr, "Falha ao abrir h264_v4l2m2m\n");
        return 1;
    }

    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    uint32_t video_plane_id = 0;
    uint32_t prev_fb_id = 0;

    int total_frames = 0;
    uint64_t t_start_all = now_us();
    uint64_t sum_dec_us = 0, sum_kms_us = 0;

    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        uint64_t t0 = now_us();
        avcodec_send_packet(dec, pkt);
        av_packet_unref(pkt);

        while (avcodec_receive_frame(dec, frame) == 0) {
            uint64_t t1 = now_us();
            sum_dec_us += (t1 - t0);

            if (frame->format != AV_PIX_FMT_DRM_PRIME) {
                printf("Frame nao eh DRM_PRIME (fmt=%d)!\n", frame->format);
                break;
            }

            AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor *)frame->data[0];
            uint32_t drm_fmt = desc->layers[0].format;

            // No 1o frame decodificado, escolhe o melhor Plane do vc4 que aceite esse formato YUV!
            if (!video_plane_id) {
                printf("\n[Primeiro Frame Decodificado] Formato DRM_PRIME: %.4s (0x%08x) | Objects: %d | Layers: %d | Planes na Layer0: %d\n",
                       (char *)&drm_fmt, drm_fmt, desc->nb_objects, desc->nb_layers, desc->layers[0].nb_planes);

                drmModePlaneRes *pres = drmModeGetPlaneResources(drm_fd);
                for (uint32_t i = 0; i < pres->count_planes; i++) {
                    drmModePlane *p = drmModeGetPlane(drm_fd, pres->planes[i]);
                    if (!p) continue;
                    if ((p->possible_crtcs & (1 << crtc_idx)) && plane_supports_format(p, drm_fmt)) {
                        // Prefere um Overlay Plane livre (fb_id == 0) para jogar por cima do TTY sem erro 22!
                        video_plane_id = p->plane_id;
                        if (p->fb_id == 0) { drmModeFreePlane(p); break; }
                    }
                    drmModeFreePlane(p);
                }
                drmModeFreePlaneResources(pres);
                printf("Plane KMS escolhido no Pi 3B para Scanout YUV direto: Plane %u\n\n", video_plane_id);
            }

            // Importa o DMA-BUF do V4L2 para um Handle GEM do DRM do Pi 3B
            uint32_t gem_handles[4] = {0};
            for (int i = 0; i < desc->nb_objects; i++) {
                drmPrimeFDToHandle(drm_fd, desc->objects[i].fd, &gem_handles[i]);
            }

            uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};
            int plane_idx = 0;
            for (int l = 0; l < desc->nb_layers; l++) {
                for (int p = 0; p < desc->layers[l].nb_planes && plane_idx < 4; p++) {
                    handles[plane_idx] = gem_handles[desc->layers[l].planes[p].object_index];
                    pitches[plane_idx] = desc->layers[l].planes[p].pitch;
                    offsets[plane_idx] = desc->layers[l].planes[p].offset;
                    plane_idx++;
                }
            }

            uint32_t fb_id = 0;
            if (drmModeAddFB2(drm_fd, frame->width, frame->height, drm_fmt,
                handles, pitches, offsets, &fb_id, 0) == 0) {
                // Joga o buffer YUV direto no cabo HDMI via Hardware Video Scaler (HVS)!
                int ret = drmModeSetPlane(drm_fd, video_plane_id, crtc_id, fb_id, 0,
                                          0, 0, screen_w, screen_h,
                                          0, 0, frame->width << 16, frame->height << 16);
                if (ret < 0 && total_frames == 0) {
                    printf("Erro em drmModeSetPlane: %s (errno=%d)\n", strerror(errno), errno);
                }
                if (prev_fb_id) drmModeRmFB(drm_fd, prev_fb_id);
                prev_fb_id = fb_id;
                } else if (total_frames == 0) {
                    printf("Erro em drmModeAddFB2: %s (errno=%d)\n", strerror(errno), errno);
                }

                uint64_t t2 = now_us();
                sum_kms_us += (t2 - t1);
                total_frames++;

                if (total_frames % 60 == 0) {
                    printf("Frames: %3d/300 | V4L2 Decode: %4lu us (%.2f ms) | KMS Scanout: %4lu us (%.2f ms)\n",
                           total_frames, sum_dec_us / 60, (sum_dec_us / 60) / 1000.0,
                           sum_kms_us / 60, (sum_kms_us / 60) / 1000.0);
                    sum_dec_us = sum_kms_us = 0;
                }
                av_frame_unref(frame);
                t0 = now_us();
        }
    }

    uint64_t total_time_ms = (now_us() - t_start_all) / 1000;
    printf("\n=== Resultado Final no Pi 3B ===\n");
    printf("Total de Frames Exibidos: %d em %lu ms (Velocidade Real: %.1f FPS!)\n",
           total_frames, (unsigned long)total_time_ms,
           total_frames * 1000.0 / (total_time_ms ? total_time_ms : 1));

    if (prev_fb_id) drmModeRmFB(drm_fd, prev_fb_id);
    close(drm_fd);
    return 0;
}
