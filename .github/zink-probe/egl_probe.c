// Minimal surfaceless EGL + desktop-GL probe.
//
// Creates an OpenGL context through Mesa's EGL surfaceless platform and
// prints the GL vendor/renderer/version strings. The whole point is to
// learn which Gallium driver Mesa selects in a given environment: with
// GALLIUM_DRIVER unset it reports "llvmpipe" (software); with
// GALLIUM_DRIVER=zink and a working Vulkan ICD it reports the Vulkan
// device behind Zink (on Apple Silicon, the Metal GPU via KosmicKrisp).
//
// No window server or display is required, so it runs in headless CI.

#include <stdio.h>
#include <EGL/egl.h>
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>

int main(void) {
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY) {
        printf("FAIL: eglGetDisplay returned EGL_NO_DISPLAY\n");
        return 2;
    }

    EGLint major = 0, minor = 0;
    if (!eglInitialize(dpy, &major, &minor)) {
        printf("FAIL: eglInitialize error 0x%x\n", eglGetError());
        return 3;
    }
    printf("EGL %d.%d vendor=%s\n", major, minor, eglQueryString(dpy, EGL_VENDOR));

    if (!eglBindAPI(EGL_OPENGL_API)) {
        printf("FAIL: eglBindAPI(EGL_OPENGL_API) error 0x%x\n", eglGetError());
        return 4;
    }

    const EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_NONE
    };
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || n < 1) {
        printf("FAIL: eglChooseConfig found no config (error 0x%x)\n", eglGetError());
        return 5;
    }

    // Request a 4.x compatibility context, matching what the engine asks for.
    const EGLint ctx_attr[] = {
        EGL_CONTEXT_MAJOR_VERSION, 4,
        EGL_CONTEXT_MINOR_VERSION, 1,
        EGL_NONE
    };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (ctx == EGL_NO_CONTEXT) {
        printf("FAIL: eglCreateContext error 0x%x\n", eglGetError());
        return 6;
    }

    if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        printf("FAIL: eglMakeCurrent error 0x%x\n", eglGetError());
        return 7;
    }

    const GLubyte *vendor   = glGetString(GL_VENDOR);
    const GLubyte *renderer = glGetString(GL_RENDERER);
    const GLubyte *version  = glGetString(GL_VERSION);
    printf("GL_VENDOR=%s\n",   vendor   ? (const char *)vendor   : "(null)");
    printf("GL_RENDERER=%s\n", renderer ? (const char *)renderer : "(null)");
    printf("GL_VERSION=%s\n",  version  ? (const char *)version  : "(null)");

    // A software fallback reports llvmpipe; a hardware path names the GPU.
    if (renderer && (const char *)renderer) {
        const char *r = (const char *)renderer;
        // crude substring check without <string.h> dependency noise
        int is_llvmpipe = 0;
        for (const char *p = r; *p; ++p) {
            if (p[0]=='l'&&p[1]=='l'&&p[2]=='v'&&p[3]=='m'&&p[4]=='p'&&p[5]=='i'&&p[6]=='p'&&p[7]=='e') { is_llvmpipe = 1; break; }
        }
        printf("RESULT=%s\n", is_llvmpipe ? "SOFTWARE_LLVMPIPE" : "HARDWARE_OR_OTHER");
    }
    return 0;
}
