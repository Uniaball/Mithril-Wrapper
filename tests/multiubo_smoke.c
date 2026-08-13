/* M8 multi-draw UBO smoke test: three draws with distinct uniforms in ONE
 * frame, verified with a single readback.
 *
 * Regression for the M8 black-screen hunt: on iOS-class devices
 * (minUniformBufferOffsetAlignment = 256) the dynamic-UBO offset math must
 * honour the device alignment, otherwise every draw past the first in a
 * frame reads its uniform block from the wrong offset and renders black.
 * CI cannot reproduce the iOS limit -- lavapipe and macOS Metal both report
 * 16 -- so this smoke pins the *invariant* that several uniform-carrying
 * draws can share one frame and each keeps its own tint value.
 *
 * Build (from project root):
 *   gcc -o tests/multiubo_smoke tests/multiubo_smoke.c -ldl
 *   LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu ./tests/multiubo_smoke
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* GL 3.3 core constants (values from glcorearb.h) */
#define GL_VERTEX_SHADER    0x8B31
#define GL_FRAGMENT_SHADER  0x8B30
#define GL_TRIANGLES        0x0004
#define GL_ARRAY_BUFFER     0x8892
#define GL_FLOAT            0x1406
#define GL_FALSE            0
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_RGBA             0x1908
#define GL_UNSIGNED_BYTE    0x1401

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
typedef void (*fn_glUniform4f)(GLint, float, float, float, float);
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

#define W 512
#define H 512

static int failures = 0;

#define CHECK(cond, fmt, ...) do {                                          \
    if (cond) { printf("ok  : " fmt "\n", ##__VA_ARGS__); }                 \
    else      { printf("FAIL: " fmt "\n", ##__VA_ARGS__); ++failures; }     \
} while (0)

/* tolerant RGBA comparison (R8 conversions round, off-by-one allowed) */
static int px_match(const unsigned char* got, unsigned char r, unsigned char g,
                    unsigned char b, unsigned char a) {
    return abs((int)got[0] - r) <= 3 && abs((int)got[1] - g) <= 3 &&
           abs((int)got[2] - b) <= 3 && abs((int)got[3] - a) <= 3;
}

static const char* VS =
    "#version 150\n"
    "layout(location=0) in vec3 pos;\n"
    "void main() {\n"
    "    gl_Position = vec4(pos, 1.0);\n"
    "}\n";

static const char* FS =
    "#version 150\n"
    "uniform vec4 tint;\n"
    "layout(location=0) out vec4 fragColor;\n"
    "void main() {\n"
    "    fragColor = tint;\n"
    "}\n";

/* one quad (two triangles), NDC strip [x0,x1] x [-0.9, 0.9] */
static void FillQuad(float x0, float x1, float* out) {
    float q[6][3] = {
        {x0, -0.9f, 0.0f}, {x1, -0.9f, 0.0f}, {x1, 0.9f, 0.0f},
        {x0, -0.9f, 0.0f}, {x1, 0.9f, 0.0f}, {x0, 0.9f, 0.0f}};
    memcpy(out, q, sizeof q);
}

int main(void) {
#if defined(__APPLE__)
    const char* libpath = "./output/libmithril.dylib";
#else
    const char* libpath = "./output/libmithril.so";
#endif
    void* h = dlopen(libpath, RTLD_NOW | RTLD_GLOBAL);
    if (!h) { printf("dlopen: %s\n", dlerror()); return 2; }

    fn_glClearColor        clearColor     = (fn_glClearColor)dlsym(h, "glClearColor");
    fn_glClear             clear          = (fn_glClear)dlsym(h, "glClear");
    fn_glCreateShader      createShader   = (fn_glCreateShader)dlsym(h, "glCreateShader");
    fn_glShaderSource      shaderSource   = (fn_glShaderSource)dlsym(h, "glShaderSource");
    fn_glCompileShader     compileShader  = (fn_glCompileShader)dlsym(h, "glCompileShader");
    fn_glCreateProgram     createProgram  = (fn_glCreateProgram)dlsym(h, "glCreateProgram");
    fn_glAttachShader      attachShader   = (fn_glAttachShader)dlsym(h, "glAttachShader");
    fn_glLinkProgram       linkProgram    = (fn_glLinkProgram)dlsym(h, "glLinkProgram");
    fn_glUseProgram        useProgram     = (fn_glUseProgram)dlsym(h, "glUseProgram");
    fn_glGetUniformLocation getUniformLoc = (fn_glGetUniformLocation)dlsym(h, "glGetUniformLocation");
    fn_glUniform4f         uniform4f      = (fn_glUniform4f)dlsym(h, "glUniform4f");
    fn_glGenVertexArrays   genVertexArrays = (fn_glGenVertexArrays)dlsym(h, "glGenVertexArrays");
    fn_glBindVertexArray   bindVertexArray = (fn_glBindVertexArray)dlsym(h, "glBindVertexArray");
    fn_glGenBuffers        genBuffers     = (fn_glGenBuffers)dlsym(h, "glGenBuffers");
    fn_glBindBuffer        bindBuffer     = (fn_glBindBuffer)dlsym(h, "glBindBuffer");
    fn_glBufferData        bufferData     = (fn_glBufferData)dlsym(h, "glBufferData");
    fn_glEnableVertexAttribArray enableAttrib = (fn_glEnableVertexAttribArray)dlsym(h, "glEnableVertexAttribArray");
    fn_glVertexAttribPointer vertexAttribPtr = (fn_glVertexAttribPointer)dlsym(h, "glVertexAttribPointer");
    fn_glDrawArrays        drawArrays     = (fn_glDrawArrays)dlsym(h, "glDrawArrays");
    fn_glFinish            finish         = (fn_glFinish)dlsym(h, "glFinish");
    fn_glReadPixels        readPixels     = (fn_glReadPixels)dlsym(h, "glReadPixels");

    CHECK(clearColor && clear && createShader && shaderSource && compileShader &&
          createProgram && attachShader && linkProgram && useProgram &&
          getUniformLoc && uniform4f && genVertexArrays && bindVertexArray &&
          genBuffers && bindBuffer && bufferData && enableAttrib &&
          vertexAttribPtr && drawArrays && finish && readPixels,
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
    GLint tint = getUniformLoc(prog, "tint");
    CHECK(tint >= 0, "glGetUniformLocation(tint) resolves");

    GLuint vao;
    genVertexArrays(1, &vao);
    bindVertexArray(vao);

    /* three quad strips: red / green / blue across the frame, one draw each */
    float quad[6][3];
    GLuint vbo;
    genBuffers(1, &vbo);
    bindBuffer(GL_ARRAY_BUFFER, vbo);
    enableAttrib(0);
    vertexAttribPtr(0, 3, GL_FLOAT, GL_FALSE, 12, 0);

    clearColor(0.02f, 0.02f, 0.02f, 1.0f);
    clear(GL_COLOR_BUFFER_BIT);

    FillQuad(-1.0f, -0.34f, &quad[0][0]);
    bufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof quad, quad, 0x88E4);
    uniform4f(tint, 1.0f, 0.0f, 0.0f, 1.0f);
    drawArrays(GL_TRIANGLES, 0, 6);

    FillQuad(-0.33f, 0.33f, &quad[0][0]);
    bufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof quad, quad, 0x88E4);
    uniform4f(tint, 0.0f, 1.0f, 0.0f, 1.0f);
    drawArrays(GL_TRIANGLES, 0, 6);

    FillQuad(0.34f, 1.0f, &quad[0][0]);
    bufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof quad, quad, 0x88E4);
    uniform4f(tint, 0.0f, 0.0f, 1.0f, 1.0f);
    drawArrays(GL_TRIANGLES, 0, 6);

    finish();

    unsigned char px[4];
    readPixels(85, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    CHECK(px_match(px, 255, 0, 0, 255),
          "first draw in frame keeps its red tint (r=%d g=%d b=%d)", px[0], px[1], px[2]);

    readPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    CHECK(px_match(px, 0, 255, 0, 255),
          "second draw in frame keeps its green tint (r=%d g=%d b=%d)", px[0], px[1], px[2]);

    readPixels(427, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    CHECK(px_match(px, 0, 0, 255, 255),
          "third draw in frame keeps its blue tint (r=%d g=%d b=%d)", px[0], px[1], px[2]);

    dlclose(h);
    if (failures == 0) { printf("\nMULTIUBO SMOKE ALL PASSED\n"); return 0; }
    printf("\nMULTIUBO SMOKE FAILED: %d failure(s)\n", failures);
    return 1;
}