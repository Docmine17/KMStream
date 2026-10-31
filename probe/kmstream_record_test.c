// kmstream_record_test.c - Captura 300 frames (KMS -> EGL NV12 -> VA-API H.264) e grava em disco
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
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

static inline uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static const char *vs_src =
"attribute vec2 pos;\n"
"varying vec2 uv;\n"
"void main() {\n"
"    uv = pos * 0.5 + 0.5;\n"
"    gl_Position = vec4(pos, 0.0, 1.0);\n"
"}\n";

// Shader BT.709 Limited Range para converter RGB -> Y (Plano 0)
static const char *fs_y_src =
"precision mediump float;\n"
"varying vec2 uv;\n"
"uniform sampler2D tex;\n"
"void main() {\n"
"    vec3 rgb = texture2D(tex, uv).rgb;\n"
"    float y = 0.1826 * rgb.r + 0.6142 * rgb.g + 0.0620 * rgb.b + 0.0627;\n"
"    gl_FragColor = vec4(y, 0.0, 0.0, 1.0);\n"
"}\n";

// Shader BT.709 Limited Range para converter RGB -> UV (Plano 1 em R8G8)
static const char *fs_uv_src =
"precision mediump float;\n"
"varying vec2 uv;\n"
"uniform sampler2D tex;\n"
"void main() {\n"
"    vec3 rgb = texture2D(tex, uv).rgb;\n"
"    float u = -0.1006 * rgb.r - 0.3386 * rgb.g + 0.4392 * rgb.b + 0.5020;\n"
"    float v =  0.4392 * rgb.r - 0.3989 * rgb.g - 0.0403 * rgb.b + 0.5020;\n"
"    gl_FragColor = vec4(u, v, 0.0, 1.0);\n"
"}\n";

static GLuint compile_prog(const char *vs, const char *fs) {
    GLuint v = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(v, 1, &vs, NULL);
    glCompileShader(v);
    GLuint f = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(f, 1, &fs, NULL);
    glCompileShader(f);
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glBindAttribLocation(p, 0, "pos");
    glLinkProgram(p);
    return p;
}

int main(int argc, char **argv) {
    const char *card = (argc > 1) ? argv[1] : "/dev/dri/card1";
    int drm_fd = open(card, O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) { perror("open card"); return 1; }
    drmSetClientCap(drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);

    // Descobre o Plane principal ativo
    drmModePlaneRes *plane_res = drmModeGetPlaneResources(drm_fd);
    uint32_t plane_id = 0, fb_id = 0;
    for (uint32_t i = 0; i < plane_res->count_planes; i++) {
        drmModePlane *p = drmModeGetPlane(drm_fd, plane_res->planes[i]);
        if (p && p->fb_id != 0 && p->crtc_id != 0) {
            plane_id = p->plane_id;
            fb_id = p->fb_id;
            drmModeFreePlane(p);
            break;
        }
        if (p) drmModeFreePlane(p);
    }
    drmModeFreePlaneResources(plane_res);

    drmModeFB2 *fb0 = drmModeGetFB2(drm_fd, fb_id);
    int width = fb0->width, height = fb0->height;
    drmModeFreeFB2(fb0);

    // 1. Inicializa GBM + EGL no /dev/dri/renderD128
    int render_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    struct gbm_device *gbm = gbm_create_device(render_fd);
    EGLDisplay egl_dpy = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, NULL);
    eglInitialize(egl_dpy, NULL, NULL);
    eglBindAPI(EGL_OPENGL_ES_API);
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
    float quad[] = { -1,-1,  1,-1,  -1,1,  1,1 };
    GLuint vbo; glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);

    // 2. Inicializa VA-API via FFmpeg hwcontext para ter o Encoder H.264 Low-Latency pronto
    AVBufferRef *hw_device_ctx = NULL;
    av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI, "/dev/dri/renderD128", NULL, 0);
    AVHWDeviceContext *hw_dev = (AVHWDeviceContext *)hw_device_ctx->data;
    AVVAAPIDeviceContext *va_ctx = (AVVAAPIDeviceContext *)hw_dev->hwctx;
    VADisplay va_dpy = va_ctx->display;

    AVBufferRef *hw_frames_ref = av_hwframe_ctx_alloc(hw_device_ctx);
    AVHWFramesContext *frames_ctx = (AVHWFramesContext *)hw_frames_ref->data;
    frames_ctx->format = AV_PIX_FMT_VAAPI;
    frames_ctx->sw_format = AV_PIX_FMT_NV12;
    frames_ctx->width = width;
    frames_ctx->height = height;
    frames_ctx->initial_pool_size = 4;
    av_hwframe_ctx_init(hw_frames_ref);

    // Aloca 1 Frame VA-API NV12 fixo e conecta os planos Y e UV a dois FBOs no EGL!
    AVFrame *va_frame = av_frame_alloc();
    av_hwframe_get_buffer(hw_frames_ref, va_frame, 0);
    VASurfaceID nv12_surf = (VASurfaceID)(uintptr_t)va_frame->data[3];

    VADRMPRIMESurfaceDescriptor prime = {0};
    vaExportSurfaceHandle(va_dpy, nv12_surf, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                          VA_EXPORT_SURFACE_WRITE_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS, &prime);

    EGLint y_attr[] = {
        EGL_WIDTH, width, EGL_HEIGHT, height,
        EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_R8,
        EGL_DMA_BUF_PLANE0_FD_EXT, prime.objects[prime.layers[0].object_index[0]].fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)prime.layers[0].offset[0],
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)prime.layers[0].pitch[0], EGL_NONE
    };
    EGLint uv_attr[] = {
        EGL_WIDTH, width / 2, EGL_HEIGHT, height / 2,
        EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_GR88,
        EGL_DMA_BUF_PLANE0_FD_EXT, prime.objects[prime.layers[1].object_index[0]].fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)prime.layers[1].offset[0],
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)prime.layers[1].pitch[0], EGL_NONE
    };
    EGLImageKHR img_y = eglCreateImageKHR(egl_dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, y_attr);
    EGLImageKHR img_uv = eglCreateImageKHR(egl_dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, uv_attr);

    GLuint tex_y, tex_uv, fbo_y, fbo_uv, tex_kms;
    glGenTextures(1, &tex_y); glBindTexture(GL_TEXTURE_2D, tex_y);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, img_y);
    glGenFramebuffers(1, &fbo_y); glBindFramebuffer(GL_FRAMEBUFFER, fbo_y);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex_y, 0);

    glGenTextures(1, &tex_uv); glBindTexture(GL_TEXTURE_2D, tex_uv);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, img_uv);
    glGenFramebuffers(1, &fbo_uv); glBindFramebuffer(GL_FRAMEBUFFER, fbo_uv);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex_uv, 0);

    glGenTextures(1, &tex_kms);

    // 3. Configura o Encoder h264_vaapi para Ultra-Low Latency (Zero B-Frames)
    const AVCodec *codec = avcodec_find_encoder_by_name("h264_vaapi");
    AVCodecContext *enc = avcodec_alloc_context3(codec);
    enc->width = width; enc->height = height;
    enc->time_base = (AVRational){1, 60};
    enc->framerate = (AVRational){60, 1};
    enc->pix_fmt = AV_PIX_FMT_VAAPI;
    enc->max_b_frames = 0; // ZERO B-FRAMES!
    enc->gop_size = 60;
    enc->bit_rate = 15000000; // 15 Mbps
    enc->hw_frames_ctx = av_buffer_ref(hw_frames_ref);
    av_opt_set(enc->priv_data, "profile", "constrained_baseline", 0);
    av_opt_set_int(enc->priv_data, "idr_interval", 1, 0);
    avcodec_open2(enc, codec, NULL);

    FILE *out = fopen("teste_kmstream.h264", "wb");
    AVPacket *pkt = av_packet_alloc();

    printf("=== Gravando 300 frames (5s) em teste_kmstream.h264 ===\n");
    uint64_t sum_kms = 0, sum_egl = 0, sum_enc = 0;

    for (int f = 0; f < 300; f++) {
        uint64_t frame_start = now_us();

        // A. Captura o FB atual do Plane
        drmModePlane *p = drmModeGetPlane(drm_fd, plane_id);
        drmModeFB2 *fb = drmModeGetFB2(drm_fd, p->fb_id);
        int dmabuf_fd = -1;
        drmPrimeHandleToFD(drm_fd, fb->handles[0], DRM_CLOEXEC | DRM_RDWR, &dmabuf_fd);
        uint64_t t_kms = now_us();

        // B. Converte RGB (AR30/XR24) -> NV12 (Planos Y e UV) na GPU via EGL
        EGLint kms_attr[] = {
            EGL_WIDTH, width, EGL_HEIGHT, height,
            EGL_LINUX_DRM_FOURCC_EXT, (EGLint)fb->pixel_format,
            EGL_DMA_BUF_PLANE0_FD_EXT, dmabuf_fd,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)fb->offsets[0],
            EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)fb->pitches[0], EGL_NONE
        };
        EGLImageKHR img_kms = eglCreateImageKHR(egl_dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, kms_attr);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex_kms);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, img_kms);

        // Renderiza Plano Y (1920x1080)
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_y);
        glViewport(0, 0, width, height);
        glUseProgram(prog_y);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        // Renderiza Plano UV (960x540)
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_uv);
        glViewport(0, 0, width / 2, height / 2);
        glUseProgram(prog_uv);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glFinish(); // Garante que o shader terminou antes do VCE 4.0 ler a superficie NV12
        uint64_t t_egl = now_us();

        // C. Codifica em H.264 no hardware da RX 580
        va_frame->pts = f;
        avcodec_send_frame(enc, va_frame);
        while (avcodec_receive_packet(enc, pkt) == 0) {
            fwrite(pkt->data, 1, pkt->size, out);
            av_packet_unref(pkt);
        }
        uint64_t t_enc = now_us();

        sum_kms += (t_kms - frame_start);
        sum_egl += (t_egl - t_kms);
        sum_enc += (t_enc - t_egl);

        eglDestroyImageKHR(egl_dpy, img_kms);
        close(dmabuf_fd);
        drmModeFreeFB2(fb);
        drmModeFreePlane(p);

        if ((f + 1) % 60 == 0) {
            printf("Segundo %d/5 | Media -> KMS: %lu us | EGL RGB->NV12: %lu us | RX580 H.264: %lu us (%.2f ms)\n",
                   (f + 1) / 60, sum_kms / 60, sum_egl / 60, sum_enc / 60, (sum_enc / 60) / 1000.0);
            sum_kms = sum_egl = sum_enc = 0;
        }

        // Mantem ritmo aproximado de 60fps para o teste de 5 segundos
        uint64_t elapsed = now_us() - frame_start;
        if (elapsed < 16666) usleep(16666 - elapsed);
    }

    fclose(out);
    printf("\nConcluido! Abra o arquivo 'teste_kmstream.h264' com: mpv teste_kmstream.h264\n");
    return 0;
}
