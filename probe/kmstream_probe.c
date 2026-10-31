// kmstream_probe.c - Teste de Bancada 1 do KMStream (KMS -> DMA-BUF -> VA-API)
#define _POSIX_C_SOURCE 199309L
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

static void print_fourcc(uint32_t fmt) {
    printf("%c%c%c%c (0x%08x)",
           fmt & 0xff, (fmt >> 8) & 0xff, (fmt >> 16) & 0xff, (fmt >> 24) & 0xff, fmt);
}

int main(int argc, char **argv) {
    const char *card = (argc > 1) ? argv[1] : "/dev/dri/card0";
    printf("=== KMStream Probe: Inspecionando %s ===\n", card);

    int drm_fd = open(card, O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) {
        perror("Erro ao abrir dispositivo DRM (tente com sudo ou cap_sys_admin)");
        return 1;
    }

    // Habilita suporte a planos universais (Primary, Cursor, Overlay)
    drmSetClientCap(drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);

    uint64_t t0 = now_us();
    drmModePlaneRes *plane_res = drmModeGetPlaneResources(drm_fd);
    if (!plane_res) {
        perror("drmModeGetPlaneResources falhou (verifique se eh o card correto ou se falta sudo)");
        close(drm_fd);
        return 1;
    }

    uint32_t active_fb_id = 0;
    uint32_t active_plane_id = 0;

    // Procura o primeiro plano ativo que esteja exibindo um Framebuffer na tela
    for (uint32_t i = 0; i < plane_res->count_planes; i++) {
        drmModePlane *plane = drmModeGetPlane(drm_fd, plane_res->planes[i]);
        if (!plane) continue;

        if (plane->fb_id != 0 && plane->crtc_id != 0) {
            active_fb_id = plane->fb_id;
            active_plane_id = plane->plane_id;
            drmModeFreePlane(plane);
            break; // Achou o Primary Plane ativo do monitor principal
        }
        drmModeFreePlane(plane);
    }
    drmModeFreePlaneResources(plane_res);

    if (!active_fb_id) {
        printf("Nenhum Framebuffer ativo encontrado em %s. Tente /dev/dri/card1.\n", card);
        close(drm_fd);
        return 1;
    }

    // Pega os metadados modernos do Framebuffer (incluindo Modifiers AMD/DCC)
    drmModeFB2 *fb2 = drmModeGetFB2(drm_fd, active_fb_id);
    uint64_t t1 = now_us();

    if (!fb2) {
        perror("drmModeGetFB2 falhou (requer CAP_SYS_ADMIN / root)");
        close(drm_fd);
        return 1;
    }

    printf("\n[1] Captura KMS Bem-Sucedida em %lu microssegundos (%.3f ms)!\n",
           (unsigned long)(t1 - t0), (t1 - t0) / 1000.0);
    printf("    Plane ID:   %u | FB ID: %u\n", active_plane_id, fb2->fb_id);
    printf("    Resolucao:  %ux%u\n", fb2->width, fb2->height);
    printf("    Pixel Fmt:  "); print_fourcc(fb2->pixel_format); printf("\n");
    printf("    Modifier:   0x%016lx %s\n", (unsigned long)fb2->modifier,
           (fb2->flags & DRM_MODE_FB_MODIFIERS) ? "(Explicito)" : "(Linear/Implicito)");

    // Verifica se o modifier tem DCC (Delta Color Compression) da AMD ativo
    if (IS_AMD_FMT_MOD(fb2->modifier)) {
        uint64_t dcc = AMD_FMT_MOD_GET(DCC, fb2->modifier);
        printf("    AMD Tiling: Detectado! | Compressao DCC: %s\n",
               dcc ? "ATIVA (Cuidado no VA-API!)" : "DESATIVADA (Perfeito para Zero-Copy!)");
    }

    // Exporta o Handle da GPU para um File Descriptor DMA-BUF
    int dmabuf_fd = -1;
    uint64_t t2 = now_us();
    if (drmPrimeHandleToFD(drm_fd, fb2->handles[0], DRM_CLOEXEC | DRM_RDWR, &dmabuf_fd) < 0) {
        perror("drmPrimeHandleToFD falhou");
        drmModeFreeFB2(fb2);
        close(drm_fd);
        return 1;
    }
    uint64_t t3 = now_us();
    printf("\n[2] DMA-BUF FD (%d) exportado em %lu microssegundos!\n",
           dmabuf_fd, (unsigned long)(t3 - t2));

    // Testa importacao Zero-Copy direto no VA-API (Mesa radeonsi)
    int render_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    VADisplay va_dpy = vaGetDisplayDRM(render_fd >= 0 ? render_fd : drm_fd);
    int major, minor;
    if (vaInitialize(va_dpy, &major, &minor) == VA_STATUS_SUCCESS) {
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
        prime_desc.layers[0].object_index[0] = 0;
        prime_desc.layers[0].offset[0] = fb2->offsets[0];
        prime_desc.layers[0].pitch[0]  = fb2->pitches[0];

        VASurfaceAttrib attribs[2] = {0};
        attribs[0].type = VASurfaceAttribMemoryType;
        attribs[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
        attribs[0].value.type = VAGenericValueTypeInteger;
        attribs[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;

        attribs[1].type = VASurfaceAttribExternalBufferDescriptor;
        attribs[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
        attribs[1].value.type = VAGenericValueTypePointer;
        attribs[1].value.value.p = &prime_desc;

        VASurfaceID surface_id;
        uint64_t t4 = now_us();
        VAStatus st = vaCreateSurfaces(va_dpy, VA_RT_FORMAT_RGB32, fb2->width, fb2->height,
                                       &surface_id, 1, attribs, 2);
        uint64_t t5 = now_us();

        if (st == VA_STATUS_SUCCESS) {
            printf("[3] VA-API Zero-Copy: SUCESSO! VASurface criada em %lu us (%.3f ms)!\n",
                   (unsigned long)(t5 - t4), (t5 - t4) / 1000.0);
            vaDestroySurfaces(va_dpy, &surface_id, 1);
        } else {
            printf("[3] VA-API Zero-Copy: Recusou importacao direta (%s).\n", vaErrorStr(st));
            printf("    -> Motivo provavel: Compressao DCC ativa ou formato 10-bit.\n");
        }
        vaTerminate(va_dpy);
    }
    if (render_fd >= 0) close(render_fd);

    close(dmabuf_fd);
    drmModeFreeFB2(fb2);
    close(drm_fd);
    return 0;
}
