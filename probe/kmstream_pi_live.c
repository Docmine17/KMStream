// kmstream_pi_live.c (v2) - Zero Queue Delay + KMS Triple Ring Buffer (Sem bloqueio de VSync)
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext_drm.h>

#define UDP_PORT 5000
#define CHUNK_SIZE 1400
#define MAX_CHUNKS 384
#define MAX_FRAME_SIZE (MAX_CHUNKS * CHUNK_SIZE)

struct __attribute__((packed)) UdpHeader {
    uint32_t frame_id;
    uint16_t chunk_idx;
    uint16_t total_chunks;
    uint16_t payload_len;
    uint8_t  is_idr;
    uint8_t  reserved;
};

static inline uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static enum AVPixelFormat get_drm_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts) {
    for (const enum AVPixelFormat *p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == AV_PIX_FMT_DRM_PRIME) return *p;
    }
    return pix_fmts[0];
}

static int plane_supports_format(drmModePlane *p, uint32_t fmt) {
    for (uint32_t i = 0; i < p->count_formats; i++) {
        if (p->formats[i] == fmt) return 1;
    }
    return 0;
}

int main(void) {
    int drm_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) { perror("open /dev/dri/card0"); return 1; }
    drmSetClientCap(drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);

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

    const AVCodec *codec = avcodec_find_decoder_by_name("h264_v4l2m2m");
    AVCodecContext *dec = avcodec_alloc_context3(codec);
    dec->width = 1920; dec->height = 1080;
    dec->get_format = get_drm_format;
    dec->flags |= AV_CODEC_FLAG_LOW_DELAY;
    dec->extra_hw_frames = 3;
    avcodec_open2(dec, codec, NULL);

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    // Buffer contido (256 KB) para impedir que o kernel acumule segundos de atraso!
    int rcvbuf = 256 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    struct sockaddr_in addr = {
        .sin_family = AF_INET, .sin_port = htons(UDP_PORT), .sin_addr.s_addr = INADDR_ANY
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }

    printf("=== KMStream Pi 3B LIVE Client v2 (Zero-Lag) ===\n");
    printf("Aguardando stream UDP na porta %d (%dx%d)...\n", UDP_PORT, screen_w, screen_h);

    uint8_t *frame_buf = malloc(MAX_FRAME_SIZE);
    uint8_t pkt_buf[1500];
    uint32_t cur_frame_id = 0, chunks_recv = 0, expected_chunks = 0, frame_len = 0;

    AVPacket *avpkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    uint32_t video_plane_id = 0;

    // Ring buffer de 3 slots para liberar o FB antigo SEM bloquear no VSync!
    uint32_t fb_ring[3] = {0, 0, 0};
    uint32_t gem_ring[3] = {0, 0, 0};
    int ring_idx = 0;

    uint64_t fps_timer = now_us();
    int frames_shown = 0, dropped_frames = 0;

    while (1) {
        ssize_t n = recv(sock, pkt_buf, sizeof(pkt_buf), 0);
        if (n <= (ssize_t)sizeof(struct UdpHeader)) continue;

        struct UdpHeader *hdr = (struct UdpHeader *)pkt_buf;
        uint8_t *payload = pkt_buf + sizeof(struct UdpHeader);

        // Ignora pacotes atrasados de frames antigos
        if (hdr->frame_id < cur_frame_id && (cur_frame_id - hdr->frame_id) < 1000) continue;

        if (hdr->frame_id > cur_frame_id) {
            if (chunks_recv > 0 && chunks_recv != expected_chunks) dropped_frames++;
            cur_frame_id = hdr->frame_id;
            chunks_recv = 0;
            frame_len = 0;
            expected_chunks = hdr->total_chunks;
        }

        if (hdr->chunk_idx < MAX_CHUNKS && hdr->payload_len <= CHUNK_SIZE) {
            memcpy(frame_buf + (hdr->chunk_idx * CHUNK_SIZE), payload, hdr->payload_len);
            if (hdr->chunk_idx == hdr->total_chunks - 1) {
                frame_len = (hdr->chunk_idx * CHUNK_SIZE) + hdr->payload_len;
            }
            chunks_recv++;
        }

        // SO decodifica se 100% dos chunks daquele frame chegaram intactos (zero artefatos!)
        if (chunks_recv == expected_chunks && expected_chunks > 0 && frame_len > 0) {
            avpkt->data = frame_buf;
            avpkt->size = frame_len;
            avcodec_send_packet(dec, avpkt);

            // Pega todos os frames decodificados, mas se houver mais de 1 pronto, exibe apenas o mais recente!
            AVFrame *latest_frame = NULL;
            while (avcodec_receive_frame(dec, frame) == 0) {
                if (frame->format == AV_PIX_FMT_DRM_PRIME) {
                    if (latest_frame) av_frame_unref(latest_frame);
                    else latest_frame = av_frame_alloc();
                    av_frame_move_ref(latest_frame, frame);
                } else {
                    av_frame_unref(frame);
                }
            }

            if (latest_frame) {
                AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor *)latest_frame->data[0];
                uint32_t drm_fmt = desc->layers[0].format;

                if (!video_plane_id) {
                    drmModePlaneRes *pres = drmModeGetPlaneResources(drm_fd);
                    for (uint32_t i = 0; i < pres->count_planes; i++) {
                        drmModePlane *p = drmModeGetPlane(drm_fd, pres->planes[i]);
                        if (p && (p->possible_crtcs & (1 << crtc_idx)) && plane_supports_format(p, drm_fmt)) {
                            video_plane_id = p->plane_id;
                            if (p->fb_id == 0) { drmModeFreePlane(p); break; }
                        }
                        if (p) drmModeFreePlane(p);
                    }
                    drmModeFreePlaneResources(pres);
                    printf("-> Conectado! KMS Plane %u (%.4s)\n", video_plane_id, (char *)&drm_fmt);
                }

                uint32_t gem_handle = 0;
                drmPrimeFDToHandle(drm_fd, desc->objects[0].fd, &gem_handle);

                uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};
                for (int p = 0; p < desc->layers[0].nb_planes && p < 4; p++) {
                    handles[p] = gem_handle;
                    pitches[p] = desc->layers[0].planes[p].pitch;
                    offsets[p] = desc->layers[0].planes[p].offset;
                }

                uint32_t fb_id = 0;
                if (drmModeAddFB2(drm_fd, latest_frame->width, latest_frame->height, drm_fmt,
                    handles, pitches, offsets, &fb_id, 0) == 0) {
                    drmModeSetPlane(drm_fd, video_plane_id, crtc_id, fb_id, 0,
                                    0, 0, screen_w, screen_h,
                                    0, 0, latest_frame->width << 16, latest_frame->height << 16);

                    // Limpa o slot de 2 frames atras (nao bloqueia no VSync atual!)
                    int old_slot = (ring_idx + 1) % 3;
                if (fb_ring[old_slot]) {
                    drmModeRmFB(drm_fd, fb_ring[old_slot]);
                    fb_ring[old_slot] = 0;
                }
                if (gem_ring[old_slot] && gem_ring[old_slot] != gem_handle) {
                    struct drm_gem_close cl = { .handle = gem_ring[old_slot] };
                    drmIoctl(drm_fd, DRM_IOCTL_GEM_CLOSE, &cl);
                    gem_ring[old_slot] = 0;
                }
                fb_ring[ring_idx] = fb_id;
                gem_ring[ring_idx] = gem_handle;
                ring_idx = (ring_idx + 1) % 3;
                    }

                    frames_shown++;
                    uint64_t now = now_us();
                    if (now - fps_timer >= 2000000ULL) {
                        printf("[Pi v2] %.1f FPS | Frame: %u (%5u B) | Drops Rede: %d\n",
                               frames_shown * 1000000.0 / (now - fps_timer), cur_frame_id, frame_len, dropped_frames);
                        frames_shown = 0;
                        fps_timer = now;
                    }
                    av_frame_free(&latest_frame);
            }
        }
    }
    return 0;
}
