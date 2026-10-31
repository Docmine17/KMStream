// kmstream_probe_v2.c - Teste de Importacao RGB64 + Benchmark Real do Encoder H.264 da RX 580
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
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <va/va_enc_h264.h>

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
        drmModePlane *plane = drmModeGetPlane(drm_fd, plane_res->planes[i]);
        if (plane && plane->fb_id != 0 && plane->crtc_id != 0) {
            active_fb_id = plane->fb_id;
            drmModeFreePlane(plane);
            break;
        }
        if (plane) drmModeFreePlane(plane);
    }
    drmModeFreePlaneResources(plane_res);

    drmModeFB2 *fb2 = drmModeGetFB2(drm_fd, active_fb_id);
    int dmabuf_fd = -1;
    drmPrimeHandleToFD(drm_fd, fb2->handles[0], DRM_CLOEXEC | DRM_RDWR, &dmabuf_fd);

    printf("=== KMStream Probe v2 ===\n");
    printf("Formato atual do KWin: %.4s (%ux%u, pitch=%u)\n",
           (char *)&fb2->pixel_format, fb2->width, fb2->height, fb2->pitches[0]);

    int render_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    VADisplay va_dpy = vaGetDisplayDRM(render_fd >= 0 ? render_fd : drm_fd);
    int major, minor;
    vaInitialize(va_dpy, &major, &minor);

    // 1. Testa se o Mesa radeonsi aceita AB48 usando VA_RT_FORMAT_RGB64 (ou RGB32 para XR24)
    uint32_t rt_fmt = (fb2->pixel_format == DRM_FORMAT_ABGR16161616F ||
                       fb2->pixel_format == DRM_FORMAT_XBGR16161616F)
                      ? 0x00020000 /* VA_RT_FORMAT_RGB16 / 64-bit */ : VA_RT_FORMAT_RGB32;

    VADRMPRIMESurfaceDescriptor prime_desc = {0};
    prime_desc.fourcc = fb2->pixel_format;
    prime_desc.width  = fb2->width;
    prime_desc.height = fb2->height;
    prime_desc.num_objects = 1;
    prime_desc.objects[0].fd = dmabuf_fd;
    prime_desc.objects[0].size = fb2->height * fb2->pitches[0];
    prime_desc.objects[0].drm_format_modifier = fb2->modifier;
    prime_desc.num_layers = 1;
    prime_desc.layers[0].drm_format = fb2->pixel_format;
    prime_desc.layers[0].num_planes = 1;
    prime_desc.layers[0].pitch[0]   = fb2->pitches[0];

    VASurfaceAttrib attribs[2] = {0};
    attribs[0].type = VASurfaceAttribMemoryType;
    attribs[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attribs[0].value.type = VAGenericValueTypeInteger;
    attribs[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
    attribs[1].type = VASurfaceAttribExternalBufferDescriptor;
    attribs[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attribs[1].value.type = VAGenericValueTypePointer;
    attribs[1].value.value.p = &prime_desc;

    VASurfaceID import_surf;
    VAStatus st = vaCreateSurfaces(va_dpy, rt_fmt, fb2->width, fb2->height, &import_surf, 1, attribs, 2);
    printf("[1] Importacao VA-API com RT_FORMAT ajustado: %s\n", vaErrorStr(st));
    if (st == VA_STATUS_SUCCESS) vaDestroySurfaces(va_dpy, &import_surf, 1);

    // 2. Testa a criacao do Encoder H.264 Constrained Baseline na RX 580 e mede o tempo de alocacao NV12
    VAConfigAttrib enc_attr = { .type = VAConfigAttribRateControl, .value = VA_RC_CQP };
    VAConfigID config_id;
    st = vaCreateConfig(va_dpy, VAProfileH264ConstrainedBaseline, VAEntrypointEncSlice, &enc_attr, 1, &config_id);
    if (st == VA_STATUS_SUCCESS) {
        VASurfaceID nv12_surf;
        uint64_t t0 = now_us();
        vaCreateSurfaces(va_dpy, VA_RT_FORMAT_YUV420, fb2->width, fb2->height, &nv12_surf, 1, NULL, 0);
        VAContextID ctx_id;
        vaCreateContext(va_dpy, config_id, fb2->width, fb2->height, VA_PROGRESSIVE, &nv12_surf, 1, &ctx_id);
        uint64_t t1 = now_us();
        printf("[2] Hardware Encoder H.264 (VCE 4.0 RX 580) pronto em %lu us (%.2f ms)!\n",
               (unsigned long)(t1 - t0), (t1 - t0) / 1000.0);
        vaDestroyContext(va_dpy, ctx_id);
        vaDestroySurfaces(va_dpy, &nv12_surf, 1);
        vaDestroyConfig(va_dpy, config_id);
    } else {
        printf("[2] Erro ao abrir Encoder H.264: %s\n", vaErrorStr(st));
    }

    vaTerminate(va_dpy);
    if (render_fd >= 0) close(render_fd);
    close(dmabuf_fd);
    drmModeFreeFB2(fb2);
    close(drm_fd);
    return 0;
}
