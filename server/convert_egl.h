#ifndef KMSTREAM_CONVERT_EGL_H
#define KMSTREAM_CONVERT_EGL_H

#include <stdint.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <va/va.h>
#include <va/va_drmcommon.h>

#include "capture_kms.h"

struct egl_convert_ctx {
    int                 render_fd;
    struct gbm_device  *gbm;
    EGLDisplay          egl_dpy;
    EGLContext          egl_ctx;

    PFNEGLCREATEIMAGEKHRPROC            eglCreateImageKHR;
    PFNEGLDESTROYIMAGEKHRPROC           eglDestroyImageKHR;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;
    PFNEGLCREATESYNCKHRPROC             eglCreateSyncKHR;
    PFNEGLDESTROYSYNCKHRPROC            eglDestroySyncKHR;
    PFNEGLCLIENTWAITSYNCKHRPROC         eglClientWaitSyncKHR;

    int         enc_w;
    int         enc_h;
    int         prime_bound;
    GLuint      prog_y;
    GLuint      prog_uv;
    GLint       loc_cur_y;
    GLint       loc_cur_uv;
    GLint       loc_half_texel_uv;
    GLuint      vbo;
    GLuint      tex_y;
    GLuint      tex_uv;
    GLuint      fbo_y;
    GLuint      fbo_uv;
    GLuint      tex_kms;
    GLuint      tex_cur;
    EGLImageKHR img_y;
    EGLImageKHR img_uv;
};

void egl_convert_init_device(struct egl_convert_ctx *ctx);
void egl_convert_bind_prime(struct egl_convert_ctx *ctx, int enc_w, int enc_h,
                            const VADRMPRIMESurfaceDescriptor *prime);
void egl_convert_unbind_prime(struct egl_convert_ctx *ctx);
int  egl_convert_render(struct egl_convert_ctx *ctx,
                        const struct kms_captured_plane *prim,
                        const struct kms_captured_plane *cur);
void egl_convert_cleanup(struct egl_convert_ctx *ctx);

#endif /* KMSTREAM_CONVERT_EGL_H */
