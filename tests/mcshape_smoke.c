/* M8 MC-shaped GUI smoke: reproduces the Minecraft menu-renderer pipeline
 * (GL 3.3 "position_tex_color" core shaders) with the parts that make a
 * black-screen device hard to debug, all in one frame:
 *
 *   - UBO composition from a 144-byte block (mat4 ModelViewMat + mat4 ProjMat
 *     + vec4 ColorModulator) driving a pixel-space ortho transform
 *   - a POSITION_TEX_COLOR vertex format (vec3 + vec2 + 4 normalized ubytes,
 *     stride 24) exactly like MC's GUI VertexFormat
 *   - a 2x2 RGBA texture with GL_LINEAR/CLAMP_TO_EDGE, bound via
 *     glUniform1i(Sampler0, 0) + glActiveTexture, sampled in the FS
 *   - the MC menu shader's alpha kill: `if (color.a < 0.1) discard;`
 *   - blending (SRC_ALPHA / ONE_MINUS_SRC_ALPHA), depth disabled
 *
 * If any of those degrades (wrong UBO offset -> zero matrices -> degenerate
 * clip coords; broken sampler binding -> zero-alpha texels -> every fragment
 * discarded) the samples below go black -- the same signature the device
 * shows for the whole MC menu. lavapipe and macOS Metal both pass this;
 * it exists so the iOS simulator CI job can run the same shaders before the
 * user's device does.
 *
 * Expected pixels (bilerp of the 2x2 checker at the given u/v; texel
 * centres sit at (0.25,0.25),(0.75,0.25),(0.25,0.75),(0.75,0.75) in uv):
 *   draw1 fullscreen gradient: (0.25, 0.5)  -> (128,  0, 128)  (u on texel
 *     col-0 centre, v mid-way between rows: 0.5*red + 0.5*blue)
 *   draw2 opaque "button" quad: center       -> (128, 128, 128) (all four
 *     texels averaged)
 *   draw3 alpha-0 quad (blend off): center   -> gradient underneath (128,128,251)
 *     (v=0.742 => row-1 weight (0.742-0.25)/0.5 = 0.984, both col-1 texels
 *     white/blue have B=255)
 *
 * Build (from project root):
 *   gcc -o tests/mcshape_smoke tests/mcshape_smoke.c -ldl
 *   LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu ./tests/mcshape_smoke
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __APPLE__
#include <objc/message.h>
#include <objc/runtime.h>
#endif

#define GL_VERTEX_SHADER     0x8B31
#define GL_FRAGMENT_SHADER   0x8B30
#define GL_TRIANGLES         0x0004
#define GL_ARRAY_BUFFER      0x8892
#define GL_FLOAT             0x1406
#define GL_FALSE             0
#define GL_UNSIGNED_BYTE     0x1401
#define GL_RGBA              0x1908
#define EGL_DEFAULT_DISPLAY 0
#define EGL_RED_SIZE 0x3024
#define EGL_GREEN_SIZE 0x3023
#define EGL_BLUE_SIZE 0x3022
#define EGL_DEPTH_SIZE 0x3025
#define EGL_SURFACE_TYPE 0x3033
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_WINDOW_BIT 0x0004
#define EGL_OPENGL_BIT 0x0008
#define EGL_OPENGL_API 0x30A2
#define EGL_NONE 0x3038
#define GL_COLOR_BUFFER_BIT  0x00004000
#define GL_BLEND             0x0BE2
#define GL_DEPTH_TEST        0x0B71
#define GL_SRC_ALPHA         0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_TEXTURE_2D        0x0DE1
#define GL_TEXTURE0          0x84C0
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S    0x2802
#define GL_TEXTURE_WRAP_T    0x2803
#define GL_CLAMP_TO_EDGE     0x812F
#define GL_LINEAR            0x2601

typedef unsigned int GLuint;
typedef unsigned int GLenum;
typedef unsigned int GLsizei;
typedef unsigned char GLboolean;
typedef int GLint;
typedef int GLsizeiptr;
typedef void* GLvoid;

typedef void (*fn_glClearColor)(float, float, float, float);
typedef void (*fn_glClear)(GLenum);
typedef GLuint (*fn_glCreateShader)(GLenum);
typedef void (*fn_glShaderSource)(GLuint, GLsizei, const char* const*, const GLint*);
typedef void (*fn_glCompileShader)(GLuint);
typedef GLuint (*fn_glCreateProgram)(void);
typedef void (*fn_glAttachShader)(GLuint, GLuint);
typedef void (*fn_glLinkProgram)(GLuint);
typedef void (*fn_glUseProgram)(GLuint);
typedef GLint (*fn_glGetUniformLocation)(GLuint, const char*);
typedef void (*fn_glUniformMatrix4fv)(GLint, GLsizei, GLboolean, const float*);
typedef void (*fn_glUniform4f)(GLint, float, float, float, float);
typedef void (*fn_glUniform1i)(GLint, GLint);
typedef void (*fn_glGenVertexArrays)(GLsizei, GLuint*);
typedef void (*fn_glBindVertexArray)(GLuint);
typedef void (*fn_glGenBuffers)(GLsizei, GLuint*);
typedef void (*fn_glBindBuffer)(GLenum, GLuint);
typedef void (*fn_glBufferData)(GLenum, GLsizeiptr, const void*, GLenum);
typedef void (*fn_glEnableVertexAttribArray)(GLuint);
typedef void (*fn_glVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const GLvoid*);
typedef void (*fn_glDrawArrays)(GLenum, GLint, GLsizei);
typedef void (*fn_glFinish)(void);
typedef void (*fn_glReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);
typedef void (*fn_glEnable)(GLenum);
typedef void (*fn_glDisable)(GLenum);
typedef void (*fn_glBlendFunc)(GLenum, GLenum);
typedef void (*fn_glViewport)(GLint, GLint, GLsizei, GLsizei);
typedef void (*fn_glGenTextures)(GLsizei, GLuint*);
typedef void (*fn_glBindTexture)(GLenum, GLuint);
typedef void (*fn_glTexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
typedef void (*fn_glTexParameteri)(GLenum, GLenum, GLint);
typedef void (*fn_glActiveTexture)(GLenum);
typedef void* (*fn_eglGetDisplay)(int);
typedef int (*fn_eglInitialize)(void*, int*, int*);
typedef int (*fn_eglBindAPI)(int);
typedef int (*fn_eglChooseConfig)(void*, const int*, void**, int, int*);
typedef void* (*fn_eglCreateWindowSurface)(void*, void*, void*, const int*);
typedef void* (*fn_eglCreateContext)(void*, void*, void*, const int*);
typedef int (*fn_eglMakeCurrent)(void*, void*, void*, void*);
typedef int (*fn_eglSwapBuffers)(void*, void*);

#define W 512
#define H 512

static int failures = 0;

#define CHECK(cond, fmt, ...) do {                                          \
    if (cond) { printf("ok  : " fmt "\n", ##__VA_ARGS__); }                 \
    else      { printf("FAIL: " fmt "\n", ##__VA_ARGS__); ++failures; }     \
} while (0)

static int px_match(const unsigned char* got, int r, int g, int b, int a) {
    return abs((int)got[0] - r) <= 4 && abs((int)got[1] - g) <= 4 &&
           abs((int)got[2] - b) <= 4 && abs((int)got[3] - a) <= 4;
}

/* MC 1.21 core "position_tex_color" (GUI) shaders, verbatim. */
static const char* VS =
    "#version 150\n"
    "in vec3 Position;\n"
    "in vec2 UV0;\n"
    "in vec4 Color;\n"
    "uniform mat4 ModelViewMat;\n"
    "uniform mat4 ProjMat;\n"
    "out vec2 texCoord0;\n"
    "out vec4 vertexColor;\n"
    "void main() {\n"
    "    gl_Position = ProjMat * ModelViewMat * vec4(Position, 1.0);\n"
    "    texCoord0 = UV0;\n"
    "    vertexColor = Color;\n"
    "}\n";

static const char* FS =
    "#version 150\n"
    "uniform sampler2D Sampler0;\n"
    "uniform vec4 ColorModulator;\n"
    "in vec2 texCoord0;\n"
    "in vec4 vertexColor;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "    vec4 color = texture(Sampler0, texCoord0) * vertexColor * ColorModulator;\n"
    "    if (color.a < 0.1) discard;\n"
    "    fragColor = color;\n"
    "}\n";

/* MC GUI vertex format: POSITION(3f) + UV0(2f) + COLOR(4 normalized ubytes). */
typedef struct { float x, y, z, u, v; unsigned char r, g, b, a; } Vtx;

static void Quad(Vtx* q, float x0, float y0, float x1, float y1,
                 unsigned char r, unsigned char g, unsigned char b, unsigned char a) {
    q[0] = (Vtx){x0, y0, 0, 0, 0, r, g, b, a};
    q[1] = (Vtx){x1, y0, 0, 1, 0, r, g, b, a};
    q[2] = (Vtx){x1, y1, 0, 1, 1, r, g, b, a};
    q[3] = (Vtx){x0, y0, 0, 0, 0, r, g, b, a};
    q[4] = (Vtx){x1, y1, 0, 1, 1, r, g, b, a};
    q[5] = (Vtx){x0, y1, 0, 0, 1, r, g, b, a};
}

#ifdef __APPLE__
// Create a real CAMetalLayer via the objc runtime so the Apple build drives
// the full VK_EXT_metal_surface + swapchain path under the MoltenVK ICD.
// Without it, eglCreateWindowSurface(0x1) dereferences a bogus layer pointer
// in SetNativeLayer()/IsCametalLayer() and the test segfaults on Metal
// (lavapipe has no Metal branch, so a fake 0x1 window is safe there).
struct DrawableSize { double width, height; };
static void* MakeMetalLayer(void) {
    Class layerClass = objc_getClass("CAMetalLayer");
    if (!layerClass) return (void*)0x1;
    typedef id (*NewFn)(id, SEL);
    NewFn alloc = (NewFn)&objc_msgSend;
    id layer = alloc((id)layerClass, sel_registerName("new"));
    typedef void (*SetDrawableFn)(id, SEL, struct DrawableSize);
    ((SetDrawableFn)&objc_msgSend)(layer, sel_registerName("setDrawableSize:"),
                                   (struct DrawableSize){512, 512});
    return layer;
}
#endif

int main(void) {
    // MITHRIL_LIB_PATH lets CI run the binary from an arbitrary CWD (e.g. a
    // booted iOS Simulator where ./output is not the working directory).
    const char* libpath = getenv("MITHRIL_LIB_PATH");
    if (!libpath) {
#if defined(__APPLE__)
        libpath = "./output/libmithril.dylib";
#else
        libpath = "./output/libmithril.so";
#endif
    }
    void* h = dlopen(libpath, RTLD_NOW | RTLD_GLOBAL);
    if (!h) { printf("dlopen: %s\n", dlerror()); return 2; }

    fn_glClearColor       clearColor      = (fn_glClearColor)dlsym(h, "glClearColor");
    fn_glClear            clear           = (fn_glClear)dlsym(h, "glClear");
    fn_glCreateShader     createShader    = (fn_glCreateShader)dlsym(h, "glCreateShader");
    fn_glShaderSource     shaderSource    = (fn_glShaderSource)dlsym(h, "glShaderSource");
    fn_glCompileShader    compileShader   = (fn_glCompileShader)dlsym(h, "glCompileShader");
    fn_glCreateProgram    createProgram   = (fn_glCreateProgram)dlsym(h, "glCreateProgram");
    fn_glAttachShader     attachShader    = (fn_glAttachShader)dlsym(h, "glAttachShader");
    fn_glLinkProgram      linkProgram     = (fn_glLinkProgram)dlsym(h, "glLinkProgram");
    fn_glUseProgram       useProgram      = (fn_glUseProgram)dlsym(h, "glUseProgram");
    fn_glGetUniformLocation getUniformLoc = (fn_glGetUniformLocation)dlsym(h, "glGetUniformLocation");
    fn_glUniformMatrix4fv uniformMatrix4fv = (fn_glUniformMatrix4fv)dlsym(h, "glUniformMatrix4fv");
    fn_glUniform4f        uniform4f       = (fn_glUniform4f)dlsym(h, "glUniform4f");
    fn_glUniform1i        uniform1i       = (fn_glUniform1i)dlsym(h, "glUniform1i");
    fn_glGenVertexArrays  genVertexArrays = (fn_glGenVertexArrays)dlsym(h, "glGenVertexArrays");
    fn_glBindVertexArray  bindVertexArray = (fn_glBindVertexArray)dlsym(h, "glBindVertexArray");
    fn_glGenBuffers       genBuffers      = (fn_glGenBuffers)dlsym(h, "glGenBuffers");
    fn_glBindBuffer       bindBuffer      = (fn_glBindBuffer)dlsym(h, "glBindBuffer");
    fn_glBufferData       bufferData      = (fn_glBufferData)dlsym(h, "glBufferData");
    fn_glEnableVertexAttribArray enableAttrib = (fn_glEnableVertexAttribArray)dlsym(h, "glEnableVertexAttribArray");
    fn_glVertexAttribPointer vertexAttribPtr = (fn_glVertexAttribPointer)dlsym(h, "glVertexAttribPointer");
    fn_glDrawArrays       drawArrays      = (fn_glDrawArrays)dlsym(h, "glDrawArrays");
    fn_glFinish           finish          = (fn_glFinish)dlsym(h, "glFinish");
    fn_glReadPixels       readPixels      = (fn_glReadPixels)dlsym(h, "glReadPixels");
    fn_glEnable           enable          = (fn_glEnable)dlsym(h, "glEnable");
    fn_glDisable          disable         = (fn_glDisable)dlsym(h, "glDisable");
    fn_glBlendFunc        blendFunc       = (fn_glBlendFunc)dlsym(h, "glBlendFunc");
    fn_glViewport         viewport        = (fn_glViewport)dlsym(h, "glViewport");
    fn_glGenTextures      genTextures     = (fn_glGenTextures)dlsym(h, "glGenTextures");
    fn_glBindTexture      bindTexture     = (fn_glBindTexture)dlsym(h, "glBindTexture");
    fn_glTexImage2D       texImage2D      = (fn_glTexImage2D)dlsym(h, "glTexImage2D");
    fn_glTexParameteri    texParameteri   = (fn_glTexParameteri)dlsym(h, "glTexParameteri");
    fn_glActiveTexture    activeTexture   = (fn_glActiveTexture)dlsym(h, "glActiveTexture");
    fn_eglGetDisplay      eglGetDisplay   = (fn_eglGetDisplay)dlsym(h, "eglGetDisplay");
    fn_eglInitialize      eglInitialize   = (fn_eglInitialize)dlsym(h, "eglInitialize");
    fn_eglBindAPI         eglBindAPI      = (fn_eglBindAPI)dlsym(h, "eglBindAPI");
    fn_eglChooseConfig    eglChooseConfig = (fn_eglChooseConfig)dlsym(h, "eglChooseConfig");
    fn_eglCreateWindowSurface eglCreateWindowSurface = (fn_eglCreateWindowSurface)dlsym(h, "eglCreateWindowSurface");
    fn_eglCreateContext   eglCreateContext = (fn_eglCreateContext)dlsym(h, "eglCreateContext");
    fn_eglMakeCurrent     eglMakeCurrent  = (fn_eglMakeCurrent)dlsym(h, "eglMakeCurrent");
    fn_eglSwapBuffers     eglSwapBuffers  = (fn_eglSwapBuffers)dlsym(h, "eglSwapBuffers");

    CHECK(clearColor && clear && createShader && shaderSource && compileShader &&
          createProgram && attachShader && linkProgram && useProgram &&
          getUniformLoc && uniformMatrix4fv && uniform4f && uniform1i &&
          genVertexArrays && bindVertexArray && genBuffers && bindBuffer &&
          bufferData && enableAttrib && vertexAttribPtr && drawArrays &&
          finish && readPixels && enable && disable && blendFunc && viewport &&
          genTextures && bindTexture && texImage2D && texParameteri &&
          activeTexture,
          "all required GL symbols resolved");

    GLuint vs = createShader(GL_VERTEX_SHADER);
    GLuint fs = createShader(GL_FRAGMENT_SHADER);
    shaderSource(vs, 1, &VS, 0);
    shaderSource(fs, 1, &FS, 0);
    compileShader(vs);
    compileShader(fs);
    GLuint prog = createProgram();
    attachShader(prog, vs);
    attachShader(prog, fs);
    linkProgram(prog);
    useProgram(prog);

    GLint mv = getUniformLoc(prog, "ModelViewMat");
    GLint pm = getUniformLoc(prog, "ProjMat");
    GLint cm = getUniformLoc(prog, "ColorModulator");
    GLint s0 = getUniformLoc(prog, "Sampler0");
    CHECK(mv >= 0 && pm >= 0 && cm >= 0 && s0 >= 0,
          "all four MC uniforms resolve (mv=%d pm=%d cm=%d s0=%d)",
          mv, pm, cm, s0);

    /* MC GUI: orthographic(0, W, H, 0, -1000, 1000), column-major (pixel space,
     * GUI y grows downward). */
    float ortho[16] = {
        2.0f / W, 0, 0, 0,
        0, -2.0f / H, 0, 0,
        0, 0, -1.0f / 1000.0f, 0,
        -1.0f, 1.0f, 0, 1.0f};
    float ident[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    uniformMatrix4fv(mv, 1, 0, ident);
    uniformMatrix4fv(pm, 1, 0, ortho);
    uniform4f(cm, 1.0f, 1.0f, 1.0f, 1.0f);
    uniform1i(s0, 0);

    /* 2x2 checker: t00=red, t01=green, t10=blue, t11=white. */
    static const unsigned char tex4[16] = {
        255, 0, 0, 255,      0, 255, 0, 255,
          0, 0, 255, 255,  255, 255, 255, 255};
    GLuint tex;
    genTextures(1, &tex);
    activeTexture(GL_TEXTURE0);
    bindTexture(GL_TEXTURE_2D, tex);
    texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    texParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    texParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    texImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, tex4);

    viewport(0, 0, W, H);
    enable(GL_BLEND);
    blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    disable(GL_DEPTH_TEST);

    GLuint vao;
    genVertexArrays(1, &vao);
    bindVertexArray(vao);
    GLuint vbo;
    genBuffers(1, &vbo);
    bindBuffer(GL_ARRAY_BUFFER, vbo);
    enableAttrib(0);
    vertexAttribPtr(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void*)0);
    enableAttrib(1);
    vertexAttribPtr(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void*)12);
    enableAttrib(2);
    vertexAttribPtr(2, 4, GL_UNSIGNED_BYTE, 1, sizeof(Vtx), (void*)20);

    Vtx v[18];
    Quad(v,      0,   0, (float)W, (float)H, 255, 255, 255, 255);
    Quad(v + 6, 100,  80,       412,       200, 255, 255, 255, 255);
    Quad(v + 12, 200, 300,      312,       460,   0,   0,   0,   0);
    bufferData(GL_ARRAY_BUFFER, sizeof v, v, 0x88E4);

    clearColor(0.02f, 0.02f, 0.02f, 1.0f);
    clear(GL_COLOR_BUFFER_BIT);

    /* draw1: fullscreen panorama-style gradient quad */
    drawArrays(GL_TRIANGLES, 0, 6);
    /* draw2: opaque menu button on top */
    drawArrays(GL_TRIANGLES, 6, 6);
    /* draw3: alpha-0 quad (blend off so only discard can hide it) */
    disable(GL_BLEND);
    drawArrays(GL_TRIANGLES, 12, 6);

    /* Mirror MC's swap cadence: eglSwapBuffers pushes the frame through
     * Present() (needed for the engine's Linux diag to observe it too). */
    {
        void* dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        int maj = 0, min = 0;
        eglInitialize(dpy, &maj, &min);
        eglBindAPI(EGL_OPENGL_API);
        const int attribs[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                               EGL_DEPTH_SIZE, 24, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                               EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE};
        void* configs[1]; int ncfg = 0;
        eglChooseConfig(dpy, attribs, configs, 1, &ncfg);
#ifdef __APPLE__
        void* win = MakeMetalLayer();
#else
        void* win = (void*)0x1;
#endif
        void* surf = eglCreateWindowSurface(dpy, configs[0], win, 0);
        const int ctx_attrs[] = {0x3098, 2, EGL_NONE};
        void* ctx = eglCreateContext(dpy, configs[0], 0, ctx_attrs);
        eglMakeCurrent(dpy, surf, surf, ctx);
        eglSwapBuffers(dpy, surf);
    }
    finish();

    unsigned char px[4];

    /* gui (128, 256) -> uv (0.25, 0.5) */
    readPixels(128, H - 1 - 256, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    CHECK(px_match(px, 128, 0, 128, 255),
          "gradient bilerp at (0.25,0.5) = (128,0,128), got (%d,%d,%d,%d)",
          px[0], px[1], px[2], px[3]);

    /* button centre: gui (256,140) -> uv (0.5,0.5) */
    readPixels(256, H - 1 - 140, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    CHECK(px_match(px, 128, 128, 128, 255),
          "opaque button centre = (128,128,128), got (%d,%d,%d,%d)",
          px[0], px[1], px[2], px[3]);

    /* discard guard: gui (256,380) -> gradient underneath (128,128,251) */
    readPixels(256, H - 1 - 380, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    CHECK(px_match(px, 128, 128, 251, 255),
          "alpha-0 quad discarded, gradient underneath = (128,128,251), got (%d,%d,%d,%d)",
          px[0], px[1], px[2], px[3]);

    /* corner still shows clear colour (gradient covers the frame, but the
     * very last pixel row/column is a known edge; just prove it is not black) */
    readPixels(1, H - 1 - 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    CHECK(px[0] + px[1] + px[2] > 100,
          "corner is not black (sum=%d)", px[0] + px[1] + px[2]);

    dlclose(h);
    if (failures == 0) { printf("\nMCSHAPE SMOKE ALL PASSED\n"); return 0; }
    printf("\nMCSHAPE SMOKE FAILED: %d failure(s)\n", failures);
    return 1;
}
