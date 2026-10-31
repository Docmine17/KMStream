// kmstreamd_live.c (v4) - Fix Cursor Plane + Low-Latency Pacing + Zero-Reorder VUI
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/opt.h>

#define UDP_PORT 5000
#define CHUNK_SIZE 1400

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

static const char *vs_src =
    "attribute vec2 pos;\n"
    "attribute vec2 in_uv;\n"
    "varying vec2 uv;\n"
    "void main() { uv = in_uv; gl_Position = vec4(pos, 0.0, 1.0); }\n";

static const char *fs_y_src =
    "precision mediump float;\n"
    "varying vec2 uv;\n"
    "uniform sampler2D tex;\n"
    "uniform int is_cursor;\n"
    "void main() {\n"
    "    vec4 c = texture2D(tex, uv);\n"
    "    if (is_cursor == 1 && c.a < 0.05) discard;\n"
    "    float y = 0.1826 * c.r + 0.6142 * c.g + 0.0620 * c.b + 0.0627;\n"
    "    gl_FragColor = vec4(y, 0.0, 0.0, 1.0);\n"
    "}\n";

static const char *fs_uv_src =
    "precision mediump float;\n"
    "varying vec2 uv;\n"
    "uniform sampler2D tex;\n"
    "uniform int is_cursor;\n"
    "void main() {\n"
    "    vec4 c = texture2D(tex, uv);\n"
    "    if (is_cursor == 1 && c.a < 0.05) discard;\n"
    "    float u = -0.1006 * c.r - 0.3386 * c.g + 0.4392 * c.b + 0.5020;\n"
    "    float v =  0.4392 * c.r - 0.3989 * c.g - 0.0403 * c.b + 0.5020;\n"
    "    gl_FragColor = vec4(u, v, 0.0, 1.0);\n"
    "}\n";

static GLuint compile_prog(const char *vs, const char *fs) {
    GLuint v = glCreateShader(GL_VERTEX_SHADER); glShaderSource(v, 1, &vs, NULL); glCompileShader(v);
    GLuint f = glCreateShader(GL_FRAGMENT_SHADER); glShaderSource(f, 1, &fs, NULL); glCompileShader(f);
    GLuint p = glCreateProgram(); glAttachShader(p, v); glAttachShader(p, f);
    glBindAttribLocation(p, 0, "pos"); glBindAttribLocation(p, 1, "in_uv");
    glLinkProgram(p); return p;
}

static void get_plane_atomic_info(int drm_fd, uint32_t plane_id, uint64_t *type,
                                  uint32_t *fb_id, uint32_t *crtc_id, int32_t *crtc_x, int32_t *crtc_y) {
    *type = 0; *fb_id = 0; *crtc_id = 0; *crtc_x = 0; *crtc_y = 0;
    drmModeObjectProperties *props = drmModeObjectGetProperties(drm_fd, plane_id, DRM_MODE_OBJECT_PLANE);
    if (!props) return;
    for (uint32_t i = 0; i < props->count_props; i++) {
        drmModePropertyRes *prop = drmModeGetProperty(drm_fd, props->props[i]);
        if (!prop) continue;
        uint64_t val = props->prop_values[i];
        if (strcmp(prop->name, "type") == 0) *type = val;
        else if (strcmp(prop->name, "FB_ID") == 0) *fb_id = (uint32_t)val;
        else if (strcmp(prop->name, "CRTC_ID") == 0) *crtc_id = (uint32_t)val;
        else if (strcmp(prop->name, "CRTC_X") == 0) *crtc_x = (int32_t)val;
        else if (strcmp(prop->name, "CRTC_Y") == 0) *crtc_y = (int32_t)val;
        drmModeFreeProperty(prop);
    }
    drmModeFreeObjectProperties(props);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Uso: sudo %s <IP_DO_PI> [/dev/dri/card1] [1080|720]\n", argv[0]);
        return 1;
    }
    const char *pi_ip = argv[1];
    const char *card = (argc > 2) ? argv[2] : "/dev/dri/card1";
    int use_720p = (argc > 3 && atoi(argv[3]) == 720) ? 1 : 0;

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    int sndbuf = 512 * 1024;
    int tos = IPTOS_LOWDELAY;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    setsockopt(sock, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
    struct sockaddr_in pi_addr = { .sin_family = AF_INET, .sin_port = htons(UDP_PORT) };
    inet_pton(AF_INET, pi_ip, &pi_addr.sin_addr);

    int drm_fd = open(card, O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) { perror("open card"); return 1; }
    drmSetClientCap(drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    drmSetClientCap(drm_fd, DRM_CLIENT_CAP_ATOMIC, 1);

    drmModeRes *res = drmModeGetResources(drm_fd);
    drmModePlaneRes *plane_res = drmModeGetPlaneResources(drm_fd);
    uint32_t primary_plane_id = 0, cursor_plane_id = 0, active_crtc_id = 0, fb_id = 0;
    int active_crtc_idx = -1;

    // 1. Acha o Primary Plane ativo e descobre o indice do CRTC (monitor ativo)
    for (uint32_t i = 0; i < plane_res->count_planes; i++) {
        uint64_t type; uint32_t f, c; int32_t cx, cy;
        get_plane_atomic_info(drm_fd, plane_res->planes[i], &type, &f, &c, &cx, &cy);
        if (type == DRM_PLANE_TYPE_PRIMARY && f != 0 && c != 0 && !primary_plane_id) {
            primary_plane_id = plane_res->planes[i];
            active_crtc_id = c;
            fb_id = f;
        }
    }
    for (int i = 0; i < res->count_crtcs; i++) {
        if (res->crtcs[i] == active_crtc_id) { active_crtc_idx = i; break; }
    }
    drmModeFreeResources(res);

    // 2. Acha o Cursor Plane compativel com este CRTC (mesmo que o cursor esteja oculto no momento!)
    for (uint32_t i = 0; i < plane_res->count_planes; i++) {
        uint64_t type; uint32_t f, c; int32_t cx, cy;
        get_plane_atomic_info(drm_fd, plane_res->planes[i], &type, &f, &c, &cx, &cy);
        if (type == DRM_PLANE_TYPE_CURSOR) {
            drmModePlane *p = drmModeGetPlane(drm_fd, plane_res->planes[i]);
            if (p && active_crtc_idx >= 0 && (p->possible_crtcs & (1 << active_crtc_idx))) {
                cursor_plane_id = plane_res->planes[i];
                drmModeFreePlane(p);
                break;
            }
            if (p) drmModeFreePlane(p);
        }
    }

    drmModeFB2 *fb0 = drmModeGetFB2(drm_fd, fb_id);
    int src_w = fb0->width, src_h = fb0->height;
    drmModeFreeFB2(fb0);

    int enc_w = use_720p ? 1280 : src_w;
    int enc_h = use_720p ? 720  : src_h;

    int render_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    struct gbm_device *gbm = gbm_create_device(render_fd);
    EGLDisplay egl_dpy = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, NULL);
    eglInitialize(egl_dpy, NULL, NULL); eglBindAPI(EGL_OPENGL_ES_API);
    EGLConfig cfg; EGLint num_cfg;
    const EGLint cfg_attr[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
    eglChooseConfig(egl_dpy, cfg_attr, &cfg, 1, &num_cfg);
    const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext egl_ctx = eglCreateContext(egl_dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    eglMakeCurrent(egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, egl_ctx);

    PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR = (void *)eglGetProcAddress("eglCreateImageKHR");
    PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR = (void *)eglGetProcAddress("eglDestroyImageKHR");
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES = (void *)eglGetProcAddress("glEGLImageTargetTexture2DOES");

    GLuint prog_y = compile_prog(vs_src, fs_y_src);
    GLuint prog_uv = compile_prog(vs_src, fs_uv_src);
    GLint loc_cur_y = glGetUniformLocation(prog_y, "is_cursor");
    GLint loc_cur_uv = glGetUniformLocation(prog_uv, "is_cursor");

    GLuint vbo; glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glEnableVertexAttribArray(0); glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)(2 * sizeof(float)));

    AVBufferRef *hw_device_ctx = NULL;
    av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI, "/dev/dri/renderD128", NULL, 0);
    VADisplay va_dpy = ((AVVAAPIDeviceContext *)((AVHWDeviceContext *)hw_device_ctx->data)->hwctx)->display;

    AVBufferRef *hw_frames_ref = av_hwframe_ctx_alloc(hw_device_ctx);
    AVHWFramesContext *frames_ctx = (AVHWFramesContext *)hw_frames_ref->data;
    frames_ctx->format = AV_PIX_FMT_VAAPI; frames_ctx->sw_format = AV_PIX_FMT_NV12;
    frames_ctx->width = enc_w; frames_ctx->height = enc_h; frames_ctx->initial_pool_size = 4;
    av_hwframe_ctx_init(hw_frames_ref);

    AVFrame *va_frame = av_frame_alloc();
    av_hwframe_get_buffer(hw_frames_ref, va_frame, 0);
    VASurfaceID nv12_surf = (VASurfaceID)(uintptr_t)va_frame->data[3];

    VADRMPRIMESurfaceDescriptor prime = {0};
    vaExportSurfaceHandle(va_dpy, nv12_surf, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                          VA_EXPORT_SURFACE_WRITE_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS, &prime);

    EGLint y_attr[] = {
        EGL_WIDTH, enc_w, EGL_HEIGHT, enc_h, EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_R8,
        EGL_DMA_BUF_PLANE0_FD_EXT, prime.objects[prime.layers[0].object_index[0]].fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)prime.layers[0].offset[0],
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)prime.layers[0].pitch[0], EGL_NONE
    };
    EGLint uv_attr[] = {
        EGL_WIDTH, enc_w / 2, EGL_HEIGHT, enc_h / 2, EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_GR88,
        EGL_DMA_BUF_PLANE0_FD_EXT, prime.objects[prime.layers[1].object_index[0]].fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)prime.layers[1].offset[0],
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)prime.layers[1].pitch[0], EGL_NONE
    };
    EGLImageKHR img_y = eglCreateImageKHR(egl_dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, y_attr);
    EGLImageKHR img_uv = eglCreateImageKHR(egl_dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, uv_attr);

    GLuint tex_y, tex_uv, fbo_y, fbo_uv, tex_kms, tex_cur;
    glGenTextures(1, &tex_y); glBindTexture(GL_TEXTURE_2D, tex_y); glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, img_y);
    glGenFramebuffers(1, &fbo_y); glBindFramebuffer(GL_FRAMEBUFFER, fbo_y);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex_y, 0);

    glGenTextures(1, &tex_uv); glBindTexture(GL_TEXTURE_2D, tex_uv); glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, img_uv);
    glGenFramebuffers(1, &fbo_uv); glBindFramebuffer(GL_FRAMEBUFFER, fbo_uv);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex_uv, 0);

    glGenTextures(1, &tex_kms); glGenTextures(1, &tex_cur);

    const AVCodec *codec = avcodec_find_encoder_by_name("h264_vaapi");
    AVCodecContext *enc = avcodec_alloc_context3(codec);
    enc->width = enc_w; enc->height = enc_h;
    enc->time_base = (AVRational){1, 60};
    enc->framerate = (AVRational){60, 1};
    enc->pix_fmt = AV_PIX_FMT_VAAPI;
    enc->max_b_frames = 0;
    enc->gop_size = 120;
    enc->global_quality = 24;
    enc->refs = 1; // Apenas 1 frame de referencia (minimiza DPB no decodificador do Pi 3B!)
    enc->flags |= AV_CODEC_FLAG_LOW_DELAY;
    enc->hw_frames_ctx = av_buffer_ref(hw_frames_ref);
    av_opt_set(enc->priv_data, "rc_mode", "CQP", 0);
    av_opt_set(enc->priv_data, "profile", "constrained_baseline", 0);
    av_opt_set_int(enc->priv_data, "idr_interval", 1, 0);
    avcodec_open2(enc, codec, NULL);

    AVPacket *pkt = av_packet_alloc();
    float fullscreen_quad[] = { -1,-1,0,0,  1,-1,1,0,  -1,1,0,1,  1,1,1,1 };

    printf("=== KMStream Server v4 (%dx%d -> %dx%d) | Primary: %u | Cursor Plane: %u ===\n",
           src_w, src_h, enc_w, enc_h, primary_plane_id, cursor_plane_id);

    uint32_t frame_id = 1;
    uint8_t udp_pkt[sizeof(struct UdpHeader) + CHUNK_SIZE];
    uint64_t next_frame_tick = now_us();

    while (1) {
        uint64_t now = now_us();
        if (now < next_frame_tick) usleep(next_frame_tick - now);
        next_frame_tick += 16666;
        if (now > next_frame_tick + 30000) next_frame_tick = now;

        drmModePlane *p = drmModeGetPlane(drm_fd, primary_plane_id);
        if (!p || p->fb_id == 0) {
            if (p) drmModeFreePlane(p);
            usleep(10000); continue;
        }
        drmModeFB2 *fb = drmModeGetFB2(drm_fd, p->fb_id);
        if (!fb) { drmModeFreePlane(p); continue; }

        int dmabuf_fd = -1;
        if (drmPrimeHandleToFD(drm_fd, fb->handles[0], DRM_CLOEXEC | DRM_RDWR, &dmabuf_fd) < 0) {
            drmModeFreeFB2(fb); drmModeFreePlane(p); continue;
        }

        EGLint kms_attr[] = {
            EGL_WIDTH, fb->width, EGL_HEIGHT, fb->height,
            EGL_LINUX_DRM_FOURCC_EXT, (EGLint)fb->pixel_format,
            EGL_DMA_BUF_PLANE0_FD_EXT, dmabuf_fd,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)fb->offsets[0],
            EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)fb->pitches[0], EGL_NONE
        };
        EGLImageKHR img_kms = eglCreateImageKHR(egl_dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, kms_attr);

        // Leitura Atomica do Cursor Plane (agora sempre encontrado via possible_crtcs!)
        EGLImageKHR img_cur = EGL_NO_IMAGE_KHR;
        int cur_fd = -1;
        drmModeFB2 *cfb = NULL;
        float cursor_quad[16];

        if (cursor_plane_id) {
            uint64_t ctype; uint32_t cfb_id, ccrtc_id; int32_t cx, cy;
            get_plane_atomic_info(drm_fd, cursor_plane_id, &ctype, &cfb_id, &ccrtc_id, &cx, &cy);
            if (cfb_id != 0) {
                cfb = drmModeGetFB2(drm_fd, cfb_id);
                if (cfb && drmPrimeHandleToFD(drm_fd, cfb->handles[0], DRM_CLOEXEC | DRM_RDWR, &cur_fd) == 0) {
                    EGLint cur_attr[] = {
                        EGL_WIDTH, cfb->width, EGL_HEIGHT, cfb->height,
                        EGL_LINUX_DRM_FOURCC_EXT, (EGLint)cfb->pixel_format,
                        EGL_DMA_BUF_PLANE0_FD_EXT, cur_fd,
                        EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)cfb->offsets[0],
                        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)cfb->pitches[0], EGL_NONE
                    };
                    img_cur = eglCreateImageKHR(egl_dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, cur_attr);
                    float x0 = (2.0f * cx) / fb->width - 1.0f;
                    float y0 = (2.0f * cy) / fb->height - 1.0f;
                    float x1 = (2.0f * (cx + (int)cfb->width)) / fb->width - 1.0f;
                    float y1 = (2.0f * (cy + (int)cfb->height)) / fb->height - 1.0f;
                    float tmp[16] = { x0, y0, 0, 0,  x1, y0, 1, 0,  x0, y1, 0, 1,  x1, y1, 1, 1 };
                    memcpy(cursor_quad, tmp, sizeof(tmp));
                }
            }
        }

        // Renderiza Plano Y
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_y);
        glViewport(0, 0, enc_w, enc_h);
        glUseProgram(prog_y);
        glUniform1i(loc_cur_y, 0);
        glBindTexture(GL_TEXTURE_2D, tex_kms);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, img_kms);
        glBufferData(GL_ARRAY_BUFFER, sizeof(fullscreen_quad), fullscreen_quad, GL_STREAM_DRAW);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        if (img_cur != EGL_NO_IMAGE_KHR) {
            glUniform1i(loc_cur_y, 1);
            glBindTexture(GL_TEXTURE_2D, tex_cur);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, img_cur);
            glBufferData(GL_ARRAY_BUFFER, sizeof(cursor_quad), cursor_quad, GL_STREAM_DRAW);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }

        // Renderiza Plano UV
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_uv);
        glViewport(0, 0, enc_w / 2, enc_h / 2);
        glUseProgram(prog_uv);
        glUniform1i(loc_cur_uv, 0);
        glBindTexture(GL_TEXTURE_2D, tex_kms);
        glBufferData(GL_ARRAY_BUFFER, sizeof(fullscreen_quad), fullscreen_quad, GL_STREAM_DRAW);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        if (img_cur != EGL_NO_IMAGE_KHR) {
            glUniform1i(loc_cur_uv, 1);
            glBindTexture(GL_TEXTURE_2D, tex_cur);
            glBufferData(GL_ARRAY_BUFFER, sizeof(cursor_quad), cursor_quad, GL_STREAM_DRAW);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
        glFinish();

        va_frame->pts = frame_id;
        avcodec_send_frame(enc, va_frame);
        while (avcodec_receive_packet(enc, pkt) == 0) {
            uint16_t total_chunks = (pkt->size + CHUNK_SIZE - 1) / CHUNK_SIZE;
            for (uint16_t c = 0; c < total_chunks; c++) {
                int offset = c * CHUNK_SIZE;
                uint16_t len = (pkt->size - offset > CHUNK_SIZE) ? CHUNK_SIZE : (pkt->size - offset);
                struct UdpHeader *hdr = (struct UdpHeader *)udp_pkt;
                hdr->frame_id = frame_id;
                hdr->chunk_idx = c;
                hdr->total_chunks = total_chunks;
                hdr->payload_len = len;
                hdr->is_idr = (pkt->flags & AV_PKT_FLAG_KEY) ? 1 : 0;
                memcpy(udp_pkt + sizeof(struct UdpHeader), pkt->data + offset, len);
                sendto(sock, udp_pkt, sizeof(struct UdpHeader) + len, 0,
                       (struct sockaddr *)&pi_addr, sizeof(pi_addr));
            }
            frame_id++;
            av_packet_unref(pkt);
        }

        if (img_cur != EGL_NO_IMAGE_KHR) eglDestroyImageKHR(egl_dpy, img_cur);
        if (cur_fd >= 0) close(cur_fd);
        if (cfb) drmModeFreeFB2(cfb);

        eglDestroyImageKHR(egl_dpy, img_kms);
        close(dmabuf_fd);
        drmModeFreeFB2(fb);
        drmModeFreePlane(p);
    }
    return 0;
}