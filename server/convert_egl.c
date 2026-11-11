#include "convert_egl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <drm_fourcc.h>

#define DIE_IF(cond, fmt, ...) do { \
    if (cond) { \
        fprintf(stderr, "[kmstreamd] FATAL (%s:%d): " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
        exit(1); \
    } \
} while (0)

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

/* 2-tap horizontal chroma filter matching Sunshine ConvertUV.frag (BT.709 Limited Range) */
static const char *fs_uv_src =
    "precision mediump float;\n"
    "varying vec2 uv;\n"
    "uniform sampler2D tex;\n"
    "uniform int is_cursor;\n"
    "uniform float u_half_texel_w;\n"
    "void main() {\n"
    "    vec4 c;\n"
    "    if (is_cursor == 1) {\n"
    "        c = texture2D(tex, uv);\n"
    "        if (c.a < 0.05) discard;\n"
    "    } else {\n"
    "        vec3 c0 = texture2D(tex, vec2(uv.x - u_half_texel_w, uv.y)).rgb;\n"
    "        vec3 c1 = texture2D(tex, vec2(uv.x + u_half_texel_w, uv.y)).rgb;\n"
    "        c = vec4((c0 + c1) * 0.5, 1.0);\n"
    "    }\n"
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

    glDeleteShader(v);
    glDeleteShader(f);
    return p;
}

static EGLImageKHR import_fb_dmabuf(struct egl_convert_ctx *ctx, const drmModeFB2 *fb, int dmabuf_fd) {
    EGLint attr[24];
    int k = 0;
    attr[k++] = EGL_WIDTH;                  attr[k++] = (EGLint)fb->width;
    attr[k++] = EGL_HEIGHT;                 attr[k++] = (EGLint)fb->height;
    attr[k++] = EGL_LINUX_DRM_FOURCC_EXT;   attr[k++] = (EGLint)fb->pixel_format;
    attr[k++] = EGL_DMA_BUF_PLANE0_FD_EXT;  attr[k++] = dmabuf_fd;
    attr[k++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT; attr[k++] = (EGLint)fb->offsets[0];
    attr[k++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;  attr[k++] = (EGLint)fb->pitches[0];

#ifdef EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT
    if ((fb->flags & DRM_MODE_FB_MODIFIERS) && fb->modifier != DRM_FORMAT_MOD_INVALID) {
        int k_mod = k;
        attr[k++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
        attr[k++] = (EGLint)(fb->modifier & 0xFFFFFFFFULL);
        attr[k++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
        attr[k++] = (EGLint)(fb->modifier >> 32);
        attr[k]   = EGL_NONE;
        EGLImageKHR img = ctx->eglCreateImageKHR(ctx->egl_dpy, EGL_NO_CONTEXT,
                                                 EGL_LINUX_DMA_BUF_EXT, NULL, attr);
        if (img != EGL_NO_IMAGE_KHR) return img;
        k = k_mod; /* Fallback without modifier if multi-plane DCC needs auxiliary plane */
    }
#endif
    attr[k] = EGL_NONE;
    return ctx->eglCreateImageKHR(ctx->egl_dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attr);
}

void egl_convert_init_device(struct egl_convert_ctx *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->render_fd = -1;
    ctx->img_y     = EGL_NO_IMAGE_KHR;
    ctx->img_uv    = EGL_NO_IMAGE_KHR;

    ctx->render_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    DIE_IF(ctx->render_fd < 0, "open(/dev/dri/renderD128): %s", strerror(errno));
    ctx->gbm = gbm_create_device(ctx->render_fd);
    DIE_IF(!ctx->gbm, "gbm_create_device failed");
    ctx->egl_dpy = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, ctx->gbm, NULL);
    DIE_IF(ctx->egl_dpy == EGL_NO_DISPLAY, "eglGetPlatformDisplay failed");
    DIE_IF(!eglInitialize(ctx->egl_dpy, NULL, NULL), "eglInitialize failed");
    DIE_IF(!eglBindAPI(EGL_OPENGL_ES_API), "eglBindAPI failed");

    EGLConfig cfg; EGLint num_cfg = 0;
    const EGLint cfg_attr[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
    DIE_IF(!eglChooseConfig(ctx->egl_dpy, cfg_attr, &cfg, 1, &num_cfg) || num_cfg < 1, "eglChooseConfig failed");
    const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    ctx->egl_ctx = eglCreateContext(ctx->egl_dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    DIE_IF(ctx->egl_ctx == EGL_NO_CONTEXT, "eglCreateContext failed");
    DIE_IF(!eglMakeCurrent(ctx->egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx->egl_ctx), "eglMakeCurrent failed");

    ctx->eglCreateImageKHR            = (void *)eglGetProcAddress("eglCreateImageKHR");
    ctx->eglDestroyImageKHR           = (void *)eglGetProcAddress("eglDestroyImageKHR");
    ctx->glEGLImageTargetTexture2DOES = (void *)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    ctx->eglCreateSyncKHR             = (void *)eglGetProcAddress("eglCreateSyncKHR");
    ctx->eglDestroySyncKHR            = (void *)eglGetProcAddress("eglDestroySyncKHR");
    ctx->eglClientWaitSyncKHR         = (void *)eglGetProcAddress("eglClientWaitSyncKHR");
    DIE_IF(!ctx->eglCreateImageKHR || !ctx->eglDestroyImageKHR || !ctx->glEGLImageTargetTexture2DOES,
           "missing EGL image extensions");

    ctx->prog_y            = compile_prog(vs_src, fs_y_src);
    ctx->prog_uv           = compile_prog(vs_src, fs_uv_src);
    ctx->loc_cur_y         = glGetUniformLocation(ctx->prog_y, "is_cursor");
    ctx->loc_cur_uv        = glGetUniformLocation(ctx->prog_uv, "is_cursor");
    ctx->loc_half_texel_uv = glGetUniformLocation(ctx->prog_uv, "u_half_texel_w");

    glGenBuffers(1, &ctx->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, ctx->vbo);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)(2 * sizeof(float)));

    glGenTextures(1, &ctx->tex_kms);
    glGenTextures(1, &ctx->tex_cur);

    const char *gl_renderer = (const char *)glGetString(GL_RENDERER);
    const char *gl_version  = (const char *)glGetString(GL_VERSION);
    const char *amd_dbg     = getenv("AMD_DEBUG");
    printf("[EGL Convert] Device: /dev/dri/renderD128 | GPU: %s (%s) | AMD_DEBUG=%s | Chroma: 2-tap BT.709\n",
           gl_renderer ? gl_renderer : "Unknown",
           gl_version ? gl_version : "GLES2",
           amd_dbg ? amd_dbg : "unset");
    fflush(stdout);
}

void egl_convert_bind_prime(struct egl_convert_ctx *ctx, int enc_w, int enc_h,
                            const VADRMPRIMESurfaceDescriptor *prime) {
    egl_convert_unbind_prime(ctx);

    ctx->enc_w = enc_w;
    ctx->enc_h = enc_h;

    EGLint y_attr[] = {
        EGL_WIDTH, enc_w, EGL_HEIGHT, enc_h, EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_R8,
        EGL_DMA_BUF_PLANE0_FD_EXT, prime->objects[prime->layers[0].object_index[0]].fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)prime->layers[0].offset[0],
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)prime->layers[0].pitch[0], EGL_NONE
    };
    EGLint uv_attr[] = {
        EGL_WIDTH, enc_w / 2, EGL_HEIGHT, enc_h / 2, EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_GR88,
        EGL_DMA_BUF_PLANE0_FD_EXT, prime->objects[prime->layers[1].object_index[0]].fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)prime->layers[1].offset[0],
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)prime->layers[1].pitch[0], EGL_NONE
    };
    ctx->img_y  = ctx->eglCreateImageKHR(ctx->egl_dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, y_attr);
    ctx->img_uv = ctx->eglCreateImageKHR(ctx->egl_dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, uv_attr);
    DIE_IF(ctx->img_y == EGL_NO_IMAGE_KHR || ctx->img_uv == EGL_NO_IMAGE_KHR,
           "eglCreateImageKHR for NV12 surface failed");

    glGenTextures(1, &ctx->tex_y);
    glBindTexture(GL_TEXTURE_2D, ctx->tex_y);
    ctx->glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, ctx->img_y);
    glGenFramebuffers(1, &ctx->fbo_y);
    glBindFramebuffer(GL_FRAMEBUFFER, ctx->fbo_y);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ctx->tex_y, 0);
    DIE_IF(glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE, "fbo_y incomplete");

    glGenTextures(1, &ctx->tex_uv);
    glBindTexture(GL_TEXTURE_2D, ctx->tex_uv);
    ctx->glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, ctx->img_uv);
    glGenFramebuffers(1, &ctx->fbo_uv);
    glBindFramebuffer(GL_FRAMEBUFFER, ctx->fbo_uv);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ctx->tex_uv, 0);
    DIE_IF(glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE, "fbo_uv incomplete");

    ctx->prime_bound = 1;
}

void egl_convert_unbind_prime(struct egl_convert_ctx *ctx) {
    if (!ctx->prime_bound) return;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);

    if (ctx->fbo_y)  { glDeleteFramebuffers(1, &ctx->fbo_y);  ctx->fbo_y = 0; }
    if (ctx->fbo_uv) { glDeleteFramebuffers(1, &ctx->fbo_uv); ctx->fbo_uv = 0; }
    if (ctx->tex_y)  { glDeleteTextures(1, &ctx->tex_y);      ctx->tex_y = 0; }
    if (ctx->tex_uv) { glDeleteTextures(1, &ctx->tex_uv);     ctx->tex_uv = 0; }

    if (ctx->img_y != EGL_NO_IMAGE_KHR) {
        ctx->eglDestroyImageKHR(ctx->egl_dpy, ctx->img_y);
        ctx->img_y = EGL_NO_IMAGE_KHR;
    }
    if (ctx->img_uv != EGL_NO_IMAGE_KHR) {
        ctx->eglDestroyImageKHR(ctx->egl_dpy, ctx->img_uv);
        ctx->img_uv = EGL_NO_IMAGE_KHR;
    }
    ctx->prime_bound = 0;
}

int egl_convert_render(struct egl_convert_ctx *ctx,
                       const struct kms_captured_plane *prim,
                       const struct kms_captured_plane *cur) {
    static const float fullscreen_quad[] = { -1,-1,0,0,  1,-1,1,0,  -1,1,0,1,  1,1,1,1 };

    EGLImageKHR img_kms = import_fb_dmabuf(ctx, prim->fb, prim->dmabuf_fd);
    if (img_kms == EGL_NO_IMAGE_KHR) return -1;

    EGLImageKHR img_cur = EGL_NO_IMAGE_KHR;
    float cursor_quad[16];
    if (cur->fb && cur->dmabuf_fd >= 0) {
        img_cur = import_fb_dmabuf(ctx, cur->fb, cur->dmabuf_fd);
        if (img_cur != EGL_NO_IMAGE_KHR) {
            float x0 = (2.0f * cur->crtc_x) / prim->fb->width - 1.0f;
            float y0 = (2.0f * cur->crtc_y) / prim->fb->height - 1.0f;
            float x1 = (2.0f * (cur->crtc_x + (int)cur->fb->width)) / prim->fb->width - 1.0f;
            float y1 = (2.0f * (cur->crtc_y + (int)cur->fb->height)) / prim->fb->height - 1.0f;
            float tmp[16] = { x0, y0, 0, 0,  x1, y0, 1, 0,  x0, y1, 0, 1,  x1, y1, 1, 1 };
            memcpy(cursor_quad, tmp, sizeof(tmp));
        }
    }

    /* Render Y plane */
    glBindFramebuffer(GL_FRAMEBUFFER, ctx->fbo_y);
    glViewport(0, 0, ctx->enc_w, ctx->enc_h);
    glUseProgram(ctx->prog_y);
    glUniform1i(ctx->loc_cur_y, 0);
    glBindTexture(GL_TEXTURE_2D, ctx->tex_kms);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    ctx->glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, img_kms);
    glBufferData(GL_ARRAY_BUFFER, sizeof(fullscreen_quad), fullscreen_quad, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    if (img_cur != EGL_NO_IMAGE_KHR) {
        glUniform1i(ctx->loc_cur_y, 1);
        glBindTexture(GL_TEXTURE_2D, ctx->tex_cur);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        ctx->glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, img_cur);
        glBufferData(GL_ARRAY_BUFFER, sizeof(cursor_quad), cursor_quad, GL_STREAM_DRAW);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    /* Render UV plane (with 2-tap horizontal chroma filter on desktop pass) */
    glBindFramebuffer(GL_FRAMEBUFFER, ctx->fbo_uv);
    glViewport(0, 0, ctx->enc_w / 2, ctx->enc_h / 2);
    glUseProgram(ctx->prog_uv);
    glUniform1i(ctx->loc_cur_uv, 0);
    glUniform1f(ctx->loc_half_texel_uv, 0.5f / (float)ctx->enc_w);
    glBindTexture(GL_TEXTURE_2D, ctx->tex_kms);
    glBufferData(GL_ARRAY_BUFFER, sizeof(fullscreen_quad), fullscreen_quad, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    if (img_cur != EGL_NO_IMAGE_KHR) {
        glUniform1i(ctx->loc_cur_uv, 1);
        glUniform1f(ctx->loc_half_texel_uv, 0.0f);
        glBindTexture(GL_TEXTURE_2D, ctx->tex_cur);
        glBufferData(GL_ARRAY_BUFFER, sizeof(cursor_quad), cursor_quad, GL_STREAM_DRAW);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    /* Synchronize via EGLSyncKHR fence (fall back to glFinish if unavailable) */
    if (ctx->eglCreateSyncKHR && ctx->eglClientWaitSyncKHR && ctx->eglDestroySyncKHR) {
        EGLSyncKHR sync = ctx->eglCreateSyncKHR(ctx->egl_dpy, EGL_SYNC_FENCE_KHR, NULL);
        if (sync != EGL_NO_SYNC_KHR) {
            glFlush();
            ctx->eglClientWaitSyncKHR(ctx->egl_dpy, sync, EGL_SYNC_FLUSH_COMMANDS_BIT_KHR, 5000000ULL);
            ctx->eglDestroySyncKHR(ctx->egl_dpy, sync);
        } else {
            glFinish();
        }
    } else {
        glFinish();
    }

    if (img_cur != EGL_NO_IMAGE_KHR) ctx->eglDestroyImageKHR(ctx->egl_dpy, img_cur);
    ctx->eglDestroyImageKHR(ctx->egl_dpy, img_kms);
    return 0;
}

void egl_convert_cleanup(struct egl_convert_ctx *ctx) {
    if (ctx->egl_dpy != EGL_NO_DISPLAY && ctx->egl_ctx != EGL_NO_CONTEXT) {
        egl_convert_unbind_prime(ctx);
        if (ctx->tex_kms) { glDeleteTextures(1, &ctx->tex_kms); ctx->tex_kms = 0; }
        if (ctx->tex_cur) { glDeleteTextures(1, &ctx->tex_cur); ctx->tex_cur = 0; }
        if (ctx->vbo)     { glDeleteBuffers(1, &ctx->vbo);      ctx->vbo = 0; }
        if (ctx->prog_y)  { glDeleteProgram(ctx->prog_y);       ctx->prog_y = 0; }
        if (ctx->prog_uv) { glDeleteProgram(ctx->prog_uv);      ctx->prog_uv = 0; }
        eglMakeCurrent(ctx->egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(ctx->egl_dpy, ctx->egl_ctx);
        ctx->egl_ctx = EGL_NO_CONTEXT;
    }
    if (ctx->egl_dpy != EGL_NO_DISPLAY) {
        eglTerminate(ctx->egl_dpy);
        ctx->egl_dpy = EGL_NO_DISPLAY;
    }
    if (ctx->gbm) {
        gbm_device_destroy(ctx->gbm);
        ctx->gbm = NULL;
    }
    if (ctx->render_fd >= 0) {
        close(ctx->render_fd);
        ctx->render_fd = -1;
    }
}
