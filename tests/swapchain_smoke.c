/* Swapchain smoke: EGL window-surface init + eglSwapBuffers + surface query.
 *
 * Portable: on Linux the fake native window fails the CAMetalLayer check and
 * the engine stays offscreen, so Present() degrades to a no-op and the surface
 * dims fall back to the offscreen target. On Apple builds with a real Metal
 * layer the path runs the full acquire->blit->present cycle. Either way the
 * EGL contract (create/swap/query) must hold and WIDTH/HEIGHT must be sane.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <assert.h>

#define EGL_DEFAULT_DISPLAY 0
#define EGL_RED_SIZE 0x3024
#define EGL_GREEN_SIZE 0x3023
#define EGL_BLUE_SIZE 0x3022
#define EGL_ALPHA_SIZE 0x3021
#define EGL_DEPTH_SIZE 0x3025
#define EGL_SURFACE_TYPE 0x3033
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_WINDOW_BIT 0x0004
#define EGL_PBUFFER_BIT 0x0001
#define EGL_OPENGL_BIT 0x0008
#define EGL_OPENGL_API 0x30A2
#define EGL_NATIVE_VISUAL_ID 0x302E
#define EGL_NONE 0x3038
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056
#define EGL_SUCCESS 0x3000

typedef void* (*eglGetDisplay_fn)(int);
typedef int   (*eglInitialize_fn)(void*, int*, int*);
typedef int   (*eglChooseConfig_fn)(void*, const int*, void**, int, int*);
typedef void* (*eglCreateWindowSurface_fn)(void*, void*, void*, const int*);
typedef int   (*eglMakeCurrent_fn)(void*, void*, void*, void*);
typedef int   (*eglSwapBuffers_fn)(void*, void*);
typedef int   (*eglSwapInterval_fn)(void*, int);
typedef int   (*eglQuerySurface_fn)(void*, void*, int, int*);
typedef void* (*eglCreateContext_fn)(void*, void*, void*, const int*);
typedef int   (*eglBindAPI_fn)(int);

#define CHECK(cond, fmt, ...)                                                       \
    do {                                                                            \
        if (!(cond)) {                                                              \
            printf("FAIL: " fmt "\n", ##__VA_ARGS__);                              \
            assert(cond);                                                           \
        }                                                                           \
        printf("ok  : " fmt "\n", ##__VA_ARGS__);                                   \
    } while (0)

#ifdef __APPLE__
#include <objc/message.h>
#include <objc/runtime.h>

// setDrawableSize: takes a CGSize, which is {double,double}. Declared with a
// distinct name so the test compiles without pulling in CoreGraphics headers
// (objc_msgSend only cares about the ABI layout, which matches CGSize).
struct DrawableSize { double width, height; };

// Create a real CAMetalLayer via the objc runtime so the Apple build drives
// the full VK_EXT_metal_surface + swapchain path under the MoltenVK ICD.
static void* MakeMetalLayer(void) {
    Class layerClass = objc_getClass("CAMetalLayer");
    if (!layerClass) return (void*)0x1;
    typedef id (*NewFn)(id, SEL);
    NewFn alloc = (NewFn)&objc_msgSend;
    id layer = alloc((id)layerClass, sel_registerName("new"));
    // A layer with no drawableSize has no drawables: MoltenVK's
    // AcquireNextImageKHR then returns VK_ERROR_OUT_OF_DATE_KHR forever and
    // Present() rebuilds the swapchain on every frame while never presenting.
    // Give it a real size so acquire->blit->present actually completes.
    typedef void (*SetDrawableFn)(id, SEL, struct DrawableSize);
    ((SetDrawableFn)&objc_msgSend)(layer, sel_registerName("setDrawableSize:"),
                                   (struct DrawableSize){512, 512});
    return layer;
}
#endif

int main(void) {
#if defined(__APPLE__)
    const char* libpath = "./output/libmithril.dylib";
#else
    const char* libpath = "./output/libmithril.so";
#endif
    void* h = dlopen(libpath, RTLD_NOW | RTLD_GLOBAL);
    CHECK(h, "dlopen %s: %s", libpath, dlerror());

    eglGetDisplay_fn            eglGetDisplay            = dlsym(h, "eglGetDisplay");
    eglInitialize_fn            eglInitialize            = dlsym(h, "eglInitialize");
    eglChooseConfig_fn          eglChooseConfig          = dlsym(h, "eglChooseConfig");
    eglCreateWindowSurface_fn   eglCreateWindowSurface   = dlsym(h, "eglCreateWindowSurface");
    eglCreateContext_fn         eglCreateContext         = dlsym(h, "eglCreateContext");
    eglMakeCurrent_fn           eglMakeCurrent           = dlsym(h, "eglMakeCurrent");
    eglSwapBuffers_fn           eglSwapBuffers           = dlsym(h, "eglSwapBuffers");
    eglSwapInterval_fn          eglSwapInterval          = dlsym(h, "eglSwapInterval");
    eglQuerySurface_fn          eglQuerySurface          = dlsym(h, "eglQuerySurface");
    eglBindAPI_fn               eglBindAPI               = dlsym(h, "eglBindAPI");

    CHECK(eglGetDisplay && eglInitialize && eglChooseConfig && eglCreateWindowSurface &&
          eglCreateContext && eglMakeCurrent && eglSwapBuffers && eglSwapInterval &&
          eglQuerySurface && eglBindAPI, "all EGL symbols resolved");

    void* dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    CHECK(dpy, "eglGetDisplay");
    int maj = 0, min = 0;
    CHECK(eglInitialize(dpy, &maj, &min), "eglInitialize (%d.%d)", maj, min);
    CHECK(eglBindAPI(EGL_OPENGL_API) == 1, "eglBindAPI(OPENGL_API)");

    const int attribs[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                           EGL_DEPTH_SIZE, 24, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                           EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE};
    void* configs[1];
    int ncfg = 0;
    CHECK(eglChooseConfig(dpy, attribs, configs, 1, &ncfg) == 1 && ncfg == 1,
          "eglChooseConfig -> %d config", ncfg);

// CAMetalLayer needs the objc runtime; elsewhere a fake pointer exercises the
// EGL contract path and confirms the non-layer input degrades safely to offscreen.
#ifdef __APPLE__
    void* win = MakeMetalLayer();
    CHECK(win, "MakeMetalLayer");
#else
    void* win = (void*)0x1;
#endif
    void* surf = eglCreateWindowSurface(dpy, configs[0], win, 0);
    CHECK(surf, "eglCreateWindowSurface");

    const int ctx_attrs[] = {0x3098, 2, EGL_NONE};
    void* ctx = eglCreateContext(dpy, configs[0], 0, ctx_attrs);
    CHECK(ctx, "eglCreateContext");
    CHECK(eglMakeCurrent(dpy, surf, surf, ctx) == 1, "eglMakeCurrent");

    // Swap/interval must be accepted even with no live swapchain (offscreen).
    CHECK(eglSwapInterval(dpy, 0) == 1, "eglSwapInterval(0)");
    CHECK(eglSwapBuffers(dpy, surf) == 1, "eglSwapBuffers");

#ifdef __APPLE__
    // With a CAMetalLayer the swap must have driven a real swapchain; on
    // offscreen/non-Apple the (valid) fallback keeps mithril_has_swapchain 0.
    typedef int (*has_swapchain_fn)(void);
    has_swapchain_fn vkHasSwapchain = (has_swapchain_fn)dlsym(h, "mithril_has_swapchain");
    CHECK(vkHasSwapchain && vkHasSwapchain() == 1,
          "mithril_has_swapchain after swap");
#endif

    int ws = 0, hs = 0;
    CHECK(eglQuerySurface(dpy, surf, EGL_WIDTH, &ws) == 1, "eglQuerySurface(WIDTH)");
    CHECK(eglQuerySurface(dpy, surf, EGL_HEIGHT, &hs) == 1, "eglQuerySurface(HEIGHT)");
    CHECK(ws > 0 && hs > 0, "surface dims %dx%d", ws, hs);

    printf("SWAPCHAIN SMOKE ALL PASSED (%dx%d, EGL %d.%d)\n", ws, hs, maj, min);
    (void)EGL_SUCCESS;
    return 0;
}