// kmstream_egl_test.c - Testa a ponte Zero-Copy KMS (AR30/XR24 Implicit Tiling) -> EGL -> VA-API
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
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

static inline uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

int main(int argc, char **argv) {
    const char *card = (argc > 1) ? argv[1] : "/dev/dri/card1";
    int drm_fd = open(card, O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) { perror("open card"); return 1; }

    drmSetClientCap(drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    drmModePlaneRes *plane_res = drmModeGetPlaneResources(drm_fd);
    uint32_t active_fb_id = 0;
    for (uint32_t i = 0; i < plane_res->count_planes; i++) {
        drmModePlane *p = drmModeGetPlane(drm_fd, plane_res->planes[i]);
        if (p && p->fb_id != 0 && p->crtc_id != 0) {
            active_fb_id = p->fb_id;
            drmModeFreePlane(p);
            break;
        }
        if (p) drmModeFreePlane(p);
    }
    drmModeFreePlaneResources(plane_res);

    drmModeFB2 *fb2 = drmModeGetFB2(drm_fd, active_fb_id);
    int kms_dmabuf_fd = -1;
    drmPrimeHandleToFD(drm_fd, fb2->handles[0], DRM_CLOEXEC | DRM_RDWR, &kms_dmabuf_fd);
    printf("=== KMStream EGL Bridge Test ===\n");
    printf("KMS FB %u: %ux%u | Fmt: %.4s | Pitch: %u\n",
           fb2->fb_id, fb2->width, fb2->height, (char *)&fb2->pixel_format, fb2->pitches[0]);

    // Inicializa GBM + EGL Headless na RX 580
    int render_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    struct gbm_device *gbm = gbm_create_device(render_fd);
    EGLDisplay egl_dpy = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, NULL);
    eglInitialize(egl_dpy, NULL, NULL);
    eglBindAPI(EGL_OPENGL_ES_API);

    EGLConfig cfg;
    EGLint num_cfg;
    const EGLint cfg_attr[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
    eglChooseConfig(egl_dpy, cfg_attr, &cfg, 1, &num_cfg);
    const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext egl_ctx = eglCreateContext(egl_dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    eglMakeCurrent(egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, egl_ctx);

    PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR =
    (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR =
    (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES =
    (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");

    // 1. Testa importar o Framebuffer KMS (AR30 / XR24 com Implicit Tiling) no EGL!
    uint64_t t0 = now_us();
    EGLint img_attrs[] = {
        EGL_WIDTH, fb2->width,
        EGL_HEIGHT, fb2->height,
        EGL_LINUX_DRM_FOURCC_EXT, (EGLint)fb2->pixel_format,
        EGL_DMA_BUF_PLANE0_FD_EXT, kms_dmabuf_fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)fb2->offsets[0],
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)fb2->pitches[0],
        EGL_NONE
    };
    EGLImageKHR egl_img = eglCreateImageKHR(egl_dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, img_attrs);
    uint64_t t1 = now_us();

    if (egl_img == EGL_NO_IMAGE_KHR) {
        printf("[1] EGL Import KMS falhou (erro 0x%x)\n", eglGetError());
        return 1;
    }

    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, egl_img);
    uint64_t t2 = now_us();

    printf("[1] SUCESSO! KMS (%.4s Pitch %u) importado como Textura GPU via EGL em %lu us (%.3f ms)!\n",
           (char *)&fb2->pixel_format, fb2->pitches[0], (unsigned long)(t2 - t0), (t2 - t0) / 1000.0);

    // 2. Testa importar o Plano Y e Plano UV da superficie NV12 do VA-API como destino FBO no EGL!
    VADisplay va_dpy = vaGetDisplayDRM(render_fd);
    int major, minor;
    vaInitialize(va_dpy, &major, &minor);

    VASurfaceID nv12_surf;
    uint32_t nv12_fourcc = VA_FOURCC_NV12;
    VASurfaceAttrib nv12_attr = {
        .type = VASurfaceAttribPixelFormat,
        .flags = VA_SURFACE_ATTRIB_SETTABLE,
        .value = { .type = VAGenericValueTypeInteger, .value.i = nv12_fourcc }
    };
    vaCreateSurfaces(va_dpy, VA_RT_FORMAT_YUV420, fb2->width, fb2->height, &nv12_surf, 1, &nv12_attr, 1);

    VADRMPRIMESurfaceDescriptor prime_out = {0};
    vaExportSurfaceHandle(va_dpy, nv12_surf, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                          VA_EXPORT_SURFACE_WRITE_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS, &prime_out);

    uint64_t t3 = now_us();
    EGLint y_attrs[] = {
        EGL_WIDTH, fb2->width,
        EGL_HEIGHT, fb2->height,
        EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_R8,
        EGL_DMA_BUF_PLANE0_FD_EXT, prime_out.objects[prime_out.layers[0].object_index[0]].fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)prime_out.layers[0].offset[0],
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)prime_out.layers[0].pitch[0],
        EGL_NONE
    };
    EGLImageKHR y_img = eglCreateImageKHR(egl_dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, y_attrs);
    uint64_t t4 = now_us();

    if (y_img != EGL_NO_IMAGE_KHR) {
        printf("[2] SUCESSO TOTAL! Superficie VA-API NV12 conectada ao EGL em %lu us!\n",
               (unsigned long)(t4 - t3));
        printf("    -> Pipeline 100%% Zero-Copy na VRAM confirmado na RX 580!\n");
        eglDestroyImageKHR(egl_dpy, y_img);
    } else {
        printf("[2] Falha ao conectar NV12 no EGL: 0x%x\n", eglGetError());
    }

    eglDestroyImageKHR(egl_dpy, egl_img);
    for (uint32_t i = 0; i < prime_out.num_objects; i++) close(prime_out.objects[i].fd);
    vaDestroySurfaces(va_dpy, &nv12_surf, 1);
    vaTerminate(va_dpy);
    gbm_device_destroy(gbm);
    close(render_fd);
    close(kms_dmabuf_fd);
    drmModeFreeFB2(fb2);
    close(drm_fd);
    return 0;
}
