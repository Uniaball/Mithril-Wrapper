/* Integer-attribute pipeline smoke test: an ivec2 vertex input fed through
 * glVertexAttribIPointer, mirroring Minecraft's core shaders (e.g. the
 * entity/UI pass declares `in ivec2 UV1; in ivec2 UV2;`).
 *
 * Regression for the MoltenVK pipeline compile failure
 *   Cannot convert attribute from MTLAttributeFormatFloat2 to int2 or uint2.
 * which dropped every draw of a program with integer attributes when the
 * backend staged the stream as float32 and baked an SFLOAT vertex format.
 * The backend must reflect the SPIR-V input kind and build the pipeline with
 * an SINT vertex-input format; lavapipe accepts both, so this test verifies
 * the SINT path renders the correct integer-derived colour end to end.
 *
 * Build (from project root):
 *   gcc -o tests/intattr_smoke tests/intattr_smoke.c -ldl
 *   LD_LIBRARY_PATH=$PWD/output ./tests/intattr_smoke
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* GL 3.3 core constants */
#define GL_VERTEX_SHADER      0x8B31
#define GL_FRAGMENT_SHADER    0x8B30
#define GL_TRIANGLES          0x0004
#define GL_ARRAY_BUFFER       0x8892
#define GL_FLOAT              0x1406
#define GL_SHORT              0x1402
#define GL_UNSIGNED_SHORT     0x1403
#define GL_FALSE              0
#define GL_TRUE               1
#define GL_COLOR_BUFFER_BIT   0x00004000
#define GL_RGBA               0x1908
#define GL_UNSIGNED_BYTE      0x1401
#define GL_STATIC_DRAW        0x88E4

typedef unsigned int GLuint;
typedef unsigned int GLenum;
typedef unsigned int GLsizei;
typedef unsigned char GLboolean;
typedef int GLint;
typedef int GLsizeiptr;
typedef int GLintptr;
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
typedef void (*fn_glGenVertexArrays)(GLsizei, GLuint*);
typedef void (*fn_glBindVertexArray)(GLuint);
typedef void (*fn_glGenBuffers)(GLsizei, GLuint*);
typedef void (*fn_glBindBuffer)(GLenum, GLuint);
typedef void (*fn_glBufferData)(GLenum, GLsizeiptr, const void*, GLenum);
typedef void (*fn_glEnableVertexAttribArray)(GLuint);
typedef void (*fn_glVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const GLvoid*);
typedef void (*fn_glVertexAttribIPointer)(GLuint, GLint, GLenum, GLsizei, const GLvoid*);
typedef void (*fn_glDrawArrays)(GLenum, GLint, GLsizei);
typedef void (*fn_glFinish)(void);
typedef void (*fn_glReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);
typedef void (*fn_glDeleteShader)(GLuint);
typedef void (*fn_glDeleteProgram)(GLuint);

static int failures = 0;
#define CHECK(cond, fmt, ...) do {                                          \
    if (cond) { printf("ok  : " fmt "\n", ##__VA_ARGS__); }                 \
    else      { printf("FAIL: " fmt "\n", ##__VA_ARGS__); ++failures; }     \
} while (0)

static int px_match(const unsigned char* got, unsigned char r, unsigned char g,
                    unsigned char b, unsigned char a) {
    return abs((int)got[0] - r) <= 3 && abs((int)got[1] - g) <= 3 &&
           abs((int)got[2] - b) <= 3 && abs((int)got[3] - a) <= 3;
}

static const char* VS =
    "#version 150\n"
    "layout(location=0) in vec2 pos;\n"
    "layout(location=1) in ivec2 uv;\n"   /* integer attribute (MC UV1/UV2) */
    "out vec2 vUV;\n"
    "void main() {\n"
    "    vUV = vec2(uv);\n"
    "    gl_Position = vec4(pos, 0.0, 1.0);\n"
    "}\n";

static const char* FS =
    "#version 150\n"
    "in vec2 vUV;\n"
    "layout(location=0) out vec4 fragColor;\n"
    "void main() {\n"
    "    fragColor = vec4(vUV / vec2(255.0, 255.0), 0.0, 1.0);\n"
    "}\n";

int main(void) {
#if defined(__APPLE__)
    const char* libpath = "./output/libmithril.dylib";
#else
    const char* libpath = "./output/libmithril.so";
#endif
    void* h = dlopen(libpath, RTLD_NOW | RTLD_GLOBAL);
    if (!h) { printf("dlopen: %s\n", dlerror()); return 2; }

    fn_glClearColor        clearColor        = (fn_glClearColor)dlsym(h, "glClearColor");
    fn_glClear             clear             = (fn_glClear)dlsym(h, "glClear");
    fn_glCreateShader      createShader      = (fn_glCreateShader)dlsym(h, "glCreateShader");
    fn_glShaderSource      shaderSource      = (fn_glShaderSource)dlsym(h, "glShaderSource");
    fn_glCompileShader     compileShader     = (fn_glCompileShader)dlsym(h, "glCompileShader");
    fn_glCreateProgram     createProgram     = (fn_glCreateProgram)dlsym(h, "glCreateProgram");
    fn_glAttachShader      attachShader      = (fn_glAttachShader)dlsym(h, "glAttachShader");
    fn_glLinkProgram       linkProgram       = (fn_glLinkProgram)dlsym(h, "glLinkProgram");
    fn_glUseProgram        useProgram        = (fn_glUseProgram)dlsym(h, "glUseProgram");
    fn_glGenVertexArrays   genVertexArrays   = (fn_glGenVertexArrays)dlsym(h, "glGenVertexArrays");
    fn_glBindVertexArray   bindVertexArray   = (fn_glBindVertexArray)dlsym(h, "glBindVertexArray");
    fn_glGenBuffers        genBuffers        = (fn_glGenBuffers)dlsym(h, "glGenBuffers");
    fn_glBindBuffer        bindBuffer        = (fn_glBindBuffer)dlsym(h, "glBindBuffer");
    fn_glBufferData        bufferData        = (fn_glBufferData)dlsym(h, "glBufferData");
    fn_glEnableVertexAttribArray enableAttrib = (fn_glEnableVertexAttribArray)dlsym(h, "glEnableVertexAttribArray");
    fn_glVertexAttribPointer vertexAttribPtr = (fn_glVertexAttribPointer)dlsym(h, "glVertexAttribPointer");
    fn_glVertexAttribIPointer vertexAttribIPtr = (fn_glVertexAttribIPointer)dlsym(h, "glVertexAttribIPointer");
    fn_glDrawArrays        drawArrays        = (fn_glDrawArrays)dlsym(h, "glDrawArrays");
    fn_glFinish            finish            = (fn_glFinish)dlsym(h, "glFinish");
    fn_glReadPixels        readPixels        = (fn_glReadPixels)dlsym(h, "glReadPixels");
    fn_glDeleteShader      deleteShader      = (fn_glDeleteShader)dlsym(h, "glDeleteShader");
    fn_glDeleteProgram     deleteProgram     = (fn_glDeleteProgram)dlsym(h, "glDeleteProgram");

    CHECK(clearColor && clear && createShader && shaderSource && compileShader &&
          createProgram && attachShader && linkProgram && useProgram &&
          genVertexArrays && bindVertexArray && genBuffers && bindBuffer &&
          bufferData && enableAttrib && vertexAttribPtr && vertexAttribIPtr &&
          drawArrays && finish && readPixels && deleteShader && deleteProgram,
          "all required GL symbols resolved");

    /* -- background ------------------------------------------------ */
    clearColor(0.0f, 0.0f, 0.0f, 1.0f);
    clear(GL_COLOR_BUFFER_BIT);

    /* -- program with an integer vertex attribute ------------------ */
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

    /* -- interleaved float pos + GL_SHORT uv ----------------------- */
    struct Vertex { float x, y; short uv[2]; };
    const struct Vertex verts[3] = {
        {-0.5f, -0.5f, {255, 64}},
        { 0.5f, -0.5f, {255, 64}},
        { 0.0f,  0.6f, {255, 64}},
    };
    GLuint vao, vbo;
    genVertexArrays(1, &vao);
    bindVertexArray(vao);
    genBuffers(1, &vbo);
    bindBuffer(GL_ARRAY_BUFFER, vbo);
    bufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(verts), verts, GL_STATIC_DRAW);
    enableAttrib(0);
    vertexAttribPtr(0, 2, GL_FLOAT, GL_FALSE, sizeof(struct Vertex), 0);
    enableAttrib(1);
    vertexAttribIPtr(1, 2, GL_SHORT, sizeof(struct Vertex), (const GLvoid*)8);

    /* -- draw ------------------------------------------------------- */
    drawArrays(GL_TRIANGLES, 0, 3);
    finish();

    /* -- pixel assertions ------------------------------------------- */
    unsigned char px[4];
    readPixels(256, 300, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    CHECK(px_match(px, 255, 64, 0, 255),
          "integer attribute drives the pixel colour (r=%d g=%d b=%d a=%d)",
          px[0], px[1], px[2], px[3]);

    unsigned char corner[4];
    readPixels(10, 10, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, corner);
    CHECK(px_match(corner, 0, 0, 0, 255),
          "background corner stays the clear colour (r=%d g=%d b=%d)",
          corner[0], corner[1], corner[2]);

    printf("%s\n", failures ? "INTATTR SMOKE FAILED" : "INTATTR SMOKE ALL PASSED");
    deleteShader(vs);
    deleteShader(fs);
    deleteProgram(prog);
    return failures ? 1 : 0;
}
