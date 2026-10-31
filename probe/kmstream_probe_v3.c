// kmstream_probe_v3.c - Testa AR30 (10-bit), XR24 (8-bit) e Ponte Reversa VA-API
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

    printf("=== KMStream Probe v3 ===\n");
    for (uint32_t i = 0; i < plane_res->count_planes; i++) {
        drmModePlane *p = drmModeGetPlane(drm_fd, plane_res->planes[i]);
        if (p && p->fb_id != 0 && p->crtc_id != 0) {
            if (!active_fb_id) active_fb_id = p->fb_id;
            drmModeFB2 *fb = drmModeGetFB2(drm_fd, p->fb_id);
            if (fb) {
                printf(" -> Plane %2u | FB %3u | %ux%u | Fmt: %.4s | Pitch: %u | Flags: 0x%x | Mod: 0x%lx\n",
                       p->plane_id, fb->fb_id, fb->width, fb->height,
                       (char *)&fb->pixel_format, fb->pitches[0], fb->flags, (unsigned long)fb->modifier);
                drmModeFreeFB2(fb);
            }
        }
        if (p) drmModeFreePlane(p);
    }
    drmModeFreePlaneResources(plane_res);

    drmModeFB2 *fb2 = drmModeGetFB2(drm_fd, active_fb_id);
    int dmabuf_fd = -1;
    drmPrimeHandleToFD(drm_fd, fb2->handles[0], DRM_CLOEXEC | DRM_RDWR, &dmabuf_fd);

    int render_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    VADisplay va_dpy = vaGetDisplayDRM(render_fd >= 0 ? render_fd : drm_fd);
    int major, minor;
    vaInitialize(va_dpy, &major, &minor);

    // Seleciona o RT_FORMAT correto para AR30/XR30 (10-bit) vs XR24/AR24 (8-bit)
    uint32_t rt_fmt = VA_RT_FORMAT_RGB32;
    if (fb2->pixel_format == DRM_FORMAT_ARGB2101010 || fb2->pixel_format == DRM_FORMAT_XRGB2101010 ||
        fb2->pixel_format == DRM_FORMAT_ABGR2101010 || fb2->pixel_format == DRM_FORMAT_XBGR2101010) {
        rt_fmt = VA_RT_FORMAT_RGB32_10;
        }

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
    printf("\n[1] Importacao Direta KMS -> VA-API (RT_FMT=0x%x): %s\n", rt_fmt, vaErrorStr(st));
    if (st == VA_STATUS_SUCCESS) vaDestroySurfaces(va_dpy, &import_surf, 1);

    // Teste 2: Ponte Reversa (VA-API cria NV12 e exporta DMA-BUF para o EGL escrever nele)
    VASurfaceID nv12_surf;
    uint32_t nv12_fourcc = VA_FOURCC_NV12;
    VASurfaceAttrib nv12_attr = {
        .type = VASurfaceAttribPixelFormat,
        .flags = VA_SURFACE_ATTRIB_SETTABLE,
        .value = { .type = VAGenericValueTypeInteger, .value.i = nv12_fourcc }
    };
    uint64_t t0 = now_us();
    st = vaCreateSurfaces(va_dpy, VA_RT_FORMAT_YUV420, fb2->width, fb2->height, &nv12_surf, 1, &nv12_attr, 1);
    VADRMPRIMESurfaceDescriptor prime_out = {0};
    if (st == VA_STATUS_SUCCESS) {
        st = vaExportSurfaceHandle(va_dpy, nv12_surf,
                                   VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                   VA_EXPORT_SURFACE_WRITE_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS,
                                   &prime_out);
    }
    uint64_t t1 = now_us();
    if (st == VA_STATUS_SUCCESS) {
        printf("[2] Ponte Reversa VA-API (NV12 Export): SUCESSO em %lu us! (Planes: %u, FD0: %d, FD1: %d)\n",
               (unsigned long)(t1 - t0), prime_out.num_layers,
               prime_out.objects[0].fd, prime_out.num_objects > 1 ? prime_out.objects[1].fd : prime_out.objects[0].fd);
        for (uint32_t i = 0; i < prime_out.num_objects; i++) close(prime_out.objects[i].fd);
        vaDestroySurfaces(va_dpy, &nv12_surf, 1);
    } else {
        printf("[2] Ponte Reversa VA-API falhou: %s\n", vaErrorStr(st));
    }

    vaTerminate(va_dpy);
    if (render_fd >= 0) close(render_fd);
    close(dmabuf_fd);
    drmModeFreeFB2(fb2);
    close(drm_fd);
    return 0;
}
