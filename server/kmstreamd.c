/* kmstreamd.c - KMStream Server (KMS/DRM -> EGL/GLES -> VAAPI H.264 -> UDP) */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>
#include <poll.h>
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
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/opt.h>

#include "protocol.h"
#include "stats.h"
#include "sps_patch.h"

#define DIE_IF(cond, fmt, ...) do { \
    if (cond) { \
        fprintf(stderr, "[kmstreamd] FATAL (%s:%d): " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
        exit(1); \
    } \
} while (0)

static inline uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static inline uint16_t clamp_u16(uint64_t v) {
    return (v > 65535ULL) ? 65535U : (uint16_t)v;
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
    GLint ok = 0;
    GLuint v = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(v, 1, &vs, NULL);
    glCompileShader(v);
    glGetShaderiv(v, GL_COMPILE_STATUS, &ok);
    DIE_IF(!ok, "vertex shader compilation failed");

    GLuint f = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(f, 1, &fs, NULL);
    glCompileShader(f);
    glGetShaderiv(f, GL_COMPILE_STATUS, &ok);
    DIE_IF(!ok, "fragment shader compilation failed");

    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glBindAttribLocation(p, 0, "pos");
    glBindAttribLocation(p, 1, "in_uv");
    glLinkProgram(p);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    DIE_IF(!ok, "shader program link failed");
    return p;
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

/* Drains incoming back-channel messages (PING -> PONG immediately, STATS -> cached). */
static void drain_ctrl_sock(int ctrl_sock, struct kms_ctrl_stats *last_pi_stats, int *has_pi_stats) {
    uint8_t buf[128];
    struct sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);

    while (1) {
        peer_len = sizeof(peer);
        ssize_t n = recvfrom(ctrl_sock, buf, sizeof(buf), MSG_DONTWAIT,
                             (struct sockaddr *)&peer, &peer_len);
        if (n < (ssize_t)sizeof(struct kms_ctrl_hdr)) break;

        uint64_t t1 = now_us();
        const struct kms_ctrl_hdr *h = (const struct kms_ctrl_hdr *)buf;
        if (h->version != KMS_PROTO_VERSION) continue;

        if (h->type == KMS_CTRL_PING && n >= (ssize_t)sizeof(struct kms_ctrl_ping)) {
            const struct kms_ctrl_ping *ping = (const struct kms_ctrl_ping *)buf;
            struct kms_ctrl_pong pong = {
                .h = {
                    .version  = KMS_PROTO_VERSION,
                    .type     = KMS_CTRL_PONG,
                    .reserved = 0,
                    .seq      = ping->h.seq,
                },
                .t0_us = ping->t0_us,
                .t1_us = t1,
                .t2_us = now_us(),
            };
            sendto(ctrl_sock, &pong, sizeof(pong), MSG_DONTWAIT,
                   (const struct sockaddr *)&peer, peer_len);
        } else if (h->type == KMS_CTRL_STATS && n >= (ssize_t)sizeof(struct kms_ctrl_stats)) {
            memcpy(last_pi_stats, buf, sizeof(*last_pi_stats));
            *has_pi_stats = 1;
        }
    }
}

/* Sleeps until deadline_us while waking immediately on any incoming control packet (PING). */
static void wait_until_or_ctrl(int ctrl_sock, uint64_t deadline_us,
                               struct kms_ctrl_stats *last_pi_stats, int *has_pi_stats) {
    while (1) {
        drain_ctrl_sock(ctrl_sock, last_pi_stats, has_pi_stats);
        uint64_t now = now_us();
        if (now >= deadline_us) break;
        uint64_t rem_us = deadline_us - now;
        struct timespec ts = {
            .tv_sec  = (time_t)(rem_us / 1000000ULL),
            .tv_nsec = (long)((rem_us % 1000000ULL) * 1000ULL),
        };
        struct pollfd pfd = { .fd = ctrl_sock, .events = POLLIN };
        ppoll(&pfd, 1, &ts, NULL);
    }
}

static void usage(const char *prog) {
    fprintf(stderr, "Usage: sudo %s [--dump <file.h264>] [--frames <N>] <PI_IP> [/dev/dri/card1] [1080|720]\n", prog);
}

int main(int argc, char **argv) {
    const char *dump_path = NULL;
    uint32_t max_frames = 0;

    static const struct option long_opts[] = {
        { "dump",   required_argument, NULL, 'd' },
        { "frames", required_argument, NULL, 'n' },
        { "help",   no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "d:n:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'd': dump_path = optarg; break;
        case 'n': max_frames = (uint32_t)strtoul(optarg, NULL, 10); break;
        case 'h':
        default:
            usage(argv[0]);
            return (opt == 'h') ? 0 : 1;
        }
    }

    if (argc - optind < 1) {
        usage(argv[0]);
        return 1;
    }

    const char *pi_ip = argv[optind];
    const char *card = (argc - optind > 1) ? argv[optind + 1] : "/dev/dri/card1";
    int use_720p = (argc - optind > 2 && atoi(argv[optind + 2]) == 720) ? 1 : 0;

    FILE *dump_fp = NULL;
    if (dump_path) {
        dump_fp = fopen(dump_path, "wb");
        DIE_IF(!dump_fp, "failed to open dump file '%s': %s", dump_path, strerror(errno));
    }

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    DIE_IF(sock < 0, "socket: %s", strerror(errno));
    int sndbuf = 512 * 1024;
    int tos = IPTOS_LOWDELAY;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    setsockopt(sock, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
    struct sockaddr_in pi_addr = { .sin_family = AF_INET, .sin_port = htons(KMS_VIDEO_PORT) };
    DIE_IF(inet_pton(AF_INET, pi_ip, &pi_addr.sin_addr) != 1, "invalid target IP '%s'", pi_ip);

    int ctrl_sock = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    DIE_IF(ctrl_sock < 0, "ctrl socket: %s", strerror(errno));
    setsockopt(ctrl_sock, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
    struct sockaddr_in ctrl_addr = {
        .sin_family = AF_INET, .sin_port = htons(KMS_CTRL_PORT), .sin_addr.s_addr = INADDR_ANY
    };
    DIE_IF(bind(ctrl_sock, (struct sockaddr *)&ctrl_addr, sizeof(ctrl_addr)) < 0,
           "bind ctrl port %d: %s", KMS_CTRL_PORT, strerror(errno));

    int drm_fd = open(card, O_RDWR | O_CLOEXEC);
    DIE_IF(drm_fd < 0, "open(%s): %s (run as root or setcap cap_sys_admin+ep)", card, strerror(errno));
    DIE_IF(drmSetClientCap(drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) < 0, "DRM_CLIENT_CAP_UNIVERSAL_PLANES failed");
    DIE_IF(drmSetClientCap(drm_fd, DRM_CLIENT_CAP_ATOMIC, 1) < 0, "DRM_CLIENT_CAP_ATOMIC failed");

    drmModeRes *res = drmModeGetResources(drm_fd);
    DIE_IF(!res, "drmModeGetResources failed");
    drmModePlaneRes *plane_res = drmModeGetPlaneResources(drm_fd);
    DIE_IF(!plane_res, "drmModeGetPlaneResources failed");
    uint32_t primary_plane_id = 0, cursor_plane_id = 0, active_crtc_id = 0, fb_id = 0;
    int active_crtc_idx = -1;

    for (uint32_t i = 0; i < plane_res->count_planes; i++) {
        uint64_t type; uint32_t f, c; int32_t cx, cy;
        get_plane_atomic_info(drm_fd, plane_res->planes[i], &type, &f, &c, &cx, &cy);
        if (type == DRM_PLANE_TYPE_PRIMARY && f != 0 && c != 0 && !primary_plane_id) {
            primary_plane_id = plane_res->planes[i];
            active_crtc_id = c;
            fb_id = f;
        }
    }
    DIE_IF(!primary_plane_id || !fb_id, "no active primary DRM plane found on %s", card);

    for (int i = 0; i < res->count_crtcs; i++) {
        if (res->crtcs[i] == active_crtc_id) { active_crtc_idx = i; break; }
    }
    drmModeFreeResources(res);

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
    drmModeFreePlaneResources(plane_res);

    drmModeFB2 *fb0 = drmModeGetFB2(drm_fd, fb_id);
    DIE_IF(!fb0, "drmModeGetFB2 failed (need CAP_SYS_ADMIN)");
    int src_w = fb0->width, src_h = fb0->height;
    drmModeFreeFB2(fb0);

    int enc_w = use_720p ? 1280 : src_w;
    int enc_h = use_720p ? 720  : src_h;

    int render_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    DIE_IF(render_fd < 0, "open(/dev/dri/renderD128): %s", strerror(errno));
    struct gbm_device *gbm = gbm_create_device(render_fd);
    DIE_IF(!gbm, "gbm_create_device failed");
    EGLDisplay egl_dpy = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, NULL);
    DIE_IF(egl_dpy == EGL_NO_DISPLAY, "eglGetPlatformDisplay failed");
    DIE_IF(!eglInitialize(egl_dpy, NULL, NULL), "eglInitialize failed");
    DIE_IF(!eglBindAPI(EGL_OPENGL_ES_API), "eglBindAPI failed");
    EGLConfig cfg; EGLint num_cfg = 0;
    const EGLint cfg_attr[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
    DIE_IF(!eglChooseConfig(egl_dpy, cfg_attr, &cfg, 1, &num_cfg) || num_cfg < 1, "eglChooseConfig failed");
    const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext egl_ctx = eglCreateContext(egl_dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    DIE_IF(egl_ctx == EGL_NO_CONTEXT, "eglCreateContext failed");
    DIE_IF(!eglMakeCurrent(egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, egl_ctx), "eglMakeCurrent failed");

    PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR = (void *)eglGetProcAddress("eglCreateImageKHR");
    PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR = (void *)eglGetProcAddress("eglDestroyImageKHR");
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES = (void *)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    DIE_IF(!eglCreateImageKHR || !eglDestroyImageKHR || !glEGLImageTargetTexture2DOES, "missing EGL image extensions");

    GLuint prog_y = compile_prog(vs_src, fs_y_src);
    GLuint prog_uv = compile_prog(vs_src, fs_uv_src);
    GLint loc_cur_y = glGetUniformLocation(prog_y, "is_cursor");
    GLint loc_cur_uv = glGetUniformLocation(prog_uv, "is_cursor");

    GLuint vbo; glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glEnableVertexAttribArray(0); glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)(2 * sizeof(float)));

    AVBufferRef *hw_device_ctx = NULL;
    DIE_IF(av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI, "/dev/dri/renderD128", NULL, 0) < 0,
           "av_hwdevice_ctx_create(VAAPI) failed");
    VADisplay va_dpy = ((AVVAAPIDeviceContext *)((AVHWDeviceContext *)hw_device_ctx->data)->hwctx)->display;

    AVBufferRef *hw_frames_ref = av_hwframe_ctx_alloc(hw_device_ctx);
    DIE_IF(!hw_frames_ref, "av_hwframe_ctx_alloc failed");
    AVHWFramesContext *frames_ctx = (AVHWFramesContext *)hw_frames_ref->data;
    frames_ctx->format = AV_PIX_FMT_VAAPI; frames_ctx->sw_format = AV_PIX_FMT_NV12;
    frames_ctx->width = enc_w; frames_ctx->height = enc_h; frames_ctx->initial_pool_size = 4;
    DIE_IF(av_hwframe_ctx_init(hw_frames_ref) < 0, "av_hwframe_ctx_init failed");

    AVFrame *va_frame = av_frame_alloc();
    DIE_IF(!va_frame, "av_frame_alloc failed");
    DIE_IF(av_hwframe_get_buffer(hw_frames_ref, va_frame, 0) < 0, "av_hwframe_get_buffer failed");
    VASurfaceID nv12_surf = (VASurfaceID)(uintptr_t)va_frame->data[3];

    VADRMPRIMESurfaceDescriptor prime = {0};
    VAStatus vast = vaExportSurfaceHandle(va_dpy, nv12_surf, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                          VA_EXPORT_SURFACE_WRITE_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS, &prime);
    DIE_IF(vast != VA_STATUS_SUCCESS, "vaExportSurfaceHandle failed: %d", vast);

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
    DIE_IF(img_y == EGL_NO_IMAGE_KHR || img_uv == EGL_NO_IMAGE_KHR, "eglCreateImageKHR for NV12 surface failed");
    for (uint32_t i = 0; i < prime.num_objects; i++) {
        if (prime.objects[i].fd >= 0) close(prime.objects[i].fd);
    }

    GLuint tex_y, tex_uv, fbo_y, fbo_uv, tex_kms, tex_cur;
    glGenTextures(1, &tex_y); glBindTexture(GL_TEXTURE_2D, tex_y); glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, img_y);
    glGenFramebuffers(1, &fbo_y); glBindFramebuffer(GL_FRAMEBUFFER, fbo_y);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex_y, 0);
    DIE_IF(glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE, "fbo_y incomplete");

    glGenTextures(1, &tex_uv); glBindTexture(GL_TEXTURE_2D, tex_uv); glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, img_uv);
    glGenFramebuffers(1, &fbo_uv); glBindFramebuffer(GL_FRAMEBUFFER, fbo_uv);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex_uv, 0);
    DIE_IF(glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE, "fbo_uv incomplete");

    glGenTextures(1, &tex_kms); glGenTextures(1, &tex_cur);

    const AVCodec *codec = avcodec_find_encoder_by_name("h264_vaapi");
    DIE_IF(!codec, "h264_vaapi encoder not found");
    AVCodecContext *enc = avcodec_alloc_context3(codec);
    DIE_IF(!enc, "avcodec_alloc_context3 failed");
    enc->width = enc_w; enc->height = enc_h;
    enc->time_base = (AVRational){1, 60};
    enc->framerate = (AVRational){60, 1};
    enc->pix_fmt = AV_PIX_FMT_VAAPI;
    enc->max_b_frames = 0;
    enc->gop_size = 120;
    enc->global_quality = 24;
    enc->refs = 1;
    enc->flags |= AV_CODEC_FLAG_LOW_DELAY;
    enc->hw_frames_ctx = av_buffer_ref(hw_frames_ref);
    av_opt_set(enc->priv_data, "rc_mode", "CQP", 0);
    av_opt_set(enc->priv_data, "profile", "constrained_baseline", 0);
    av_opt_set_int(enc->priv_data, "idr_interval", 1, 0);
    DIE_IF(avcodec_open2(enc, codec, NULL) < 0, "avcodec_open2(h264_vaapi) failed");

    AVPacket *pkt = av_packet_alloc();
    uint8_t *patched_buf = malloc(KMS_MAX_FRAME);
    DIE_IF(!pkt || !patched_buf, "allocation failed");
    float fullscreen_quad[] = { -1,-1,0,0,  1,-1,1,0,  -1,1,0,1,  1,1,1,1 };

    printf("=== KMStream Server (proto v%d, SPS Zero-Latency Patch) (%dx%d -> %dx%d) | Primary: %u | Cursor: %u ===\n",
           KMS_PROTO_VERSION, src_w, src_h, enc_w, enc_h, primary_plane_id, cursor_plane_id);
    fflush(stdout);

    uint32_t frame_id = 1;
    uint32_t handle_fail_cnt = 0;
    uint8_t udp_pkt[sizeof(struct kms_video_hdr) + KMS_CHUNK_SIZE];
    uint64_t next_frame_tick = now_us();
    uint64_t stat_timer = next_frame_tick;
    int sps_logged = 0;

    struct kms_stat_win st_conv = {0}, st_enc = {0}, st_tx = {0}, st_sz = {0};
    uint32_t tx_errors = 0;
    struct kms_ctrl_stats last_pi_stats = {0};
    int has_pi_stats = 0;

    while (!max_frames || frame_id <= max_frames) {
        wait_until_or_ctrl(ctrl_sock, next_frame_tick, &last_pi_stats, &has_pi_stats);
        uint64_t now = now_us();
        next_frame_tick += 16666;
        if (now > next_frame_tick + 30000) next_frame_tick = now;

        uint64_t t_cap0 = now;

        drmModePlane *p = drmModeGetPlane(drm_fd, primary_plane_id);
        if (!p || p->fb_id == 0) {
            if (p) drmModeFreePlane(p);
            usleep(10000); continue;
        }
        drmModeFB2 *fb = drmModeGetFB2(drm_fd, p->fb_id);
        if (!fb) { drmModeFreePlane(p); continue; }
        DIE_IF(fb->handles[0] == 0 && ++handle_fail_cnt >= 3,
               "drmModeGetFB2 returned handle=0 (CAP_SYS_ADMIN required: run with sudo or 'make caps')");
        if (fb->handles[0] == 0) { drmModeFreeFB2(fb); drmModeFreePlane(p); continue; }

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
        if (img_kms == EGL_NO_IMAGE_KHR) {
            close(dmabuf_fd); drmModeFreeFB2(fb); drmModeFreePlane(p); continue;
        }

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

        /* Render Y plane */
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

        /* Render UV plane */
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
        uint64_t t_conv_end = now_us();
        uint64_t conv_us = t_conv_end - t_cap0;

        va_frame->pts = frame_id;
        avcodec_send_frame(enc, va_frame);
        while (avcodec_receive_packet(enc, pkt) == 0) {
            const uint8_t *send_ptr = pkt->data;
            int send_size = pkt->size;
            uint8_t flags = 0;

            if (pkt->flags & AV_PKT_FLAG_KEY) {
                flags |= KMS_FLAG_IDR;
                int patched = 0;
                int new_sz = sps_patch_packet(pkt->data, pkt->size, patched_buf, KMS_MAX_FRAME, &patched);
                if (patched) {
                    send_ptr = patched_buf;
                    send_size = new_sz;
                    flags |= KMS_FLAG_SPS_PATCHED;
                    if (!sps_logged) {
                        printf("-> [SPS Patch] VUI rewritten (num_reorder_frames=0, max_dec_frame_buffering=1)\n");
                        fflush(stdout);
                        sps_logged = 1;
                    }
                }
            }

            uint64_t t_enc_end = now_us();
            uint64_t enc_us = t_enc_end - t_conv_end;

            if (dump_fp) {
                fwrite(send_ptr, 1, (size_t)send_size, dump_fp);
                fflush(dump_fp);
            }

            uint16_t total_chunks = (uint16_t)((send_size + KMS_CHUNK_SIZE - 1) / KMS_CHUNK_SIZE);
            for (uint16_t c = 0; c < total_chunks; c++) {
                int offset = c * KMS_CHUNK_SIZE;
                uint16_t len = (send_size - offset > KMS_CHUNK_SIZE) ? KMS_CHUNK_SIZE : (uint16_t)(send_size - offset);
                struct kms_video_hdr *hdr = (struct kms_video_hdr *)udp_pkt;
                memset(hdr, 0, sizeof(*hdr));
                hdr->version       = KMS_PROTO_VERSION;
                hdr->flags         = flags;
                hdr->chunk_idx     = c;
                hdr->total_chunks  = total_chunks;
                hdr->payload_len   = len;
                hdr->frame_id      = frame_id;
                hdr->frame_size    = (uint32_t)send_size;
                hdr->capture_ts_us = t_cap0;
                hdr->t_convert_us  = clamp_u16(conv_us);
                hdr->t_encode_us   = clamp_u16(enc_us);
                memcpy(udp_pkt + sizeof(struct kms_video_hdr), send_ptr + offset, len);
                if (sendto(sock, udp_pkt, sizeof(struct kms_video_hdr) + len, 0,
                           (struct sockaddr *)&pi_addr, sizeof(pi_addr)) < 0) {
                    tx_errors++;
                }
            }
            uint64_t t_tx_end = now_us();
            uint64_t tx_us = t_tx_end - t_enc_end;

            kms_stat_add(&st_conv, (uint32_t)conv_us);
            kms_stat_add(&st_enc,  (uint32_t)enc_us);
            kms_stat_add(&st_tx,   (uint32_t)tx_us);
            kms_stat_add(&st_sz,   (uint32_t)send_size);

            frame_id++;
            av_packet_unref(pkt);
        }

        drain_ctrl_sock(ctrl_sock, &last_pi_stats, &has_pi_stats);

        uint64_t t_log = now_us();
        if (t_log - stat_timer >= 2000000ULL && st_conv.count > 0) {
            uint32_t conv_avg, conv_p99, enc_avg, enc_p99, tx_avg, tx_p99, sz_avg, sz_p99;
            double fps = st_conv.count * 1000000.0 / (double)(t_log - stat_timer);
            kms_stat_compute(&st_conv, &conv_avg, &conv_p99);
            kms_stat_compute(&st_enc,  &enc_avg,  &enc_p99);
            kms_stat_compute(&st_tx,   &tx_avg,   &tx_p99);
            kms_stat_compute(&st_sz,   &sz_avg,   &sz_p99);

            if (has_pi_stats) {
                printf("[PC] %4.1f FPS | Size: %4.1f/%4.1f KB | Conv: %.2f/%.2f ms | Enc: %.2f/%.2f ms | TX: %.2f/%.2f ms (err:%u) | Pi E2E: %.2f/%.2f ms (inc:%u)\n",
                       fps,
                       sz_avg / 1024.0, sz_p99 / 1024.0,
                       conv_avg / 1000.0, conv_p99 / 1000.0,
                       enc_avg / 1000.0, enc_p99 / 1000.0,
                       tx_avg / 1000.0, tx_p99 / 1000.0,
                       tx_errors,
                       last_pi_stats.e2e_us_avg / 1000.0, last_pi_stats.e2e_us_p99 / 1000.0,
                       last_pi_stats.frames_lost);
            } else {
                printf("[PC] %4.1f FPS | Size: %4.1f/%4.1f KB | Conv: %.2f/%.2f ms | Enc: %.2f/%.2f ms | TX: %.2f/%.2f ms (err:%u)\n",
                       fps,
                       sz_avg / 1024.0, sz_p99 / 1024.0,
                       conv_avg / 1000.0, conv_p99 / 1000.0,
                       enc_avg / 1000.0, enc_p99 / 1000.0,
                       tx_avg / 1000.0, tx_p99 / 1000.0,
                       tx_errors);
            }
            fflush(stdout);
            kms_stat_reset(&st_conv);
            kms_stat_reset(&st_enc);
            kms_stat_reset(&st_tx);
            kms_stat_reset(&st_sz);
            stat_timer = t_log;
        }

        if (img_cur != EGL_NO_IMAGE_KHR) eglDestroyImageKHR(egl_dpy, img_cur);
        if (cur_fd >= 0) close(cur_fd);
        if (cfb) drmModeFreeFB2(cfb);

        eglDestroyImageKHR(egl_dpy, img_kms);
        close(dmabuf_fd);
        drmModeFreeFB2(fb);
        drmModeFreePlane(p);
    }

    if (dump_fp) fclose(dump_fp);
    return 0;
}
