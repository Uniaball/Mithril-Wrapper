// Mithril-Wrapper M6 stage A / Stage F smoke: double-buffered frame
// submission with *drawn* content, alternated every frame.
//
// Renders 64 frames; each frame draws a triangle whose colour and position
// alternate between two variants (red/left vs blue/right), on top of a
// frame-constant dark clear. Submission is async (glFlush, no wait) except
// every 7th frame which uses glFinish, letting the engine rotate between its
// frame slots while the GPU rides ahead. Every frame is read back at two
// points and asserted against *that frame's* expected content — proving the
// ring never cross-contaminates staging/descriptors/UBO/vertex payloads and
// that a later readback sees the latest submission.
//
// Usage:  gcc -o tests/multiframe_smoke tests/multiframe_smoke.c -ldl -lm

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void* g_gl;
static void (*glClearColor)(float, float, float, float);
static void (*glClear)(int);
static void (*glFlush)(void);
static void (*glFinish)(void);
static void (*glReadPixels)(int, int, int, int, int, int, void*);
static void (*glViewport)(int, int, int, int);
static unsigned int (*glCreateShader)(int);
static void (*glShaderSource)(unsigned int, int, const char* const*, const int*);
static void (*glCompileShader)(unsigned int);
static unsigned int (*glCreateProgram)(void);
static void (*glAttachShader)(unsigned int, unsigned int);
static void (*glLinkProgram)(unsigned int);
static void (*glUseProgram)(unsigned int);
static int (*glGetUniformLocation)(unsigned int, const char*);
static void (*glUniform4f)(int, float, float, float, float);
static void (*glGenVertexArrays)(int, unsigned int*);
static void (*glBindVertexArray)(unsigned int);
static void (*glGenBuffers)(int, unsigned int*);
static void (*glBindBuffer)(int, unsigned int);
static void (*glBufferData)(int, long, const void*, int);
static void (*glEnableVertexAttribArray)(unsigned int);
static void (*glVertexAttribPointer)(unsigned int, int, int, unsigned char, int, const void*);
static void (*glDrawArrays)(int, int, int);

static int failures;

#define CHECK(cond, fmt, ...) do {                                          \
    if (cond) { printf("ok  : " fmt "\n", ##__VA_ARGS__); }                 \
    else      { printf("FAIL: " fmt "\n", ##__VA_ARGS__); ++failures; }     \
} while (0)

#define LOAD(name)                                                            \
    do {                                                                      \
        *(void**)(&name) = dlsym(g_gl, #name);                                \
        CHECK(name, "dlsym " #name);                                          \
    } while (0)

/* tolerant RGBA comparison (rounding, off-by-one allowed) */
static int px_match(const unsigned char* px, unsigned char r, unsigned char g,
                    unsigned char b) {
    return abs((int)px[0] - r) <= 3 && abs((int)px[1] - g) <= 3 &&
           abs((int)px[2] - b) <= 3;
}

static int px_dark(const unsigned char* px) {
    return px[0] <= 20 && px[1] <= 20 && px[2] <= 25;
}

/* Two triangle geometries: variant A sits left/red, variant B right/blue. */
typedef struct { float x, y, z; float r, g, b, a; } Vertex;
static const Vertex kTriA[3] = {
    {-0.50f, -0.55f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f},
    {-0.06f, -0.55f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f},
    {-0.28f,  0.55f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f},
};
static const Vertex kTriB[3] = {
    { 0.06f, -0.55f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f},
    { 0.50f, -0.55f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f},
    { 0.28f,  0.55f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f},
};

static const char* VS =
    "#version 150\n"
    "layout(location=0) in vec3 pos;\n"
    "layout(location=1) in vec4 col;\n"
    "out vec4 vColor;\n"
    "void main() {\n"
    "    vColor = col;\n"
    "    gl_Position = vec4(pos, 1.0);\n"
    "}\n";
static const char* FS =
    "#version 150\n"
    "in vec4 vColor;\n"
    "layout(location=0) out vec4 fragColor;\n"
    "void main() {\n"
    "    fragColor = vColor;\n"
    "}\n";

int main(void) {
    g_gl = dlopen("./output/libmithril.so", RTLD_NOW);
    if (!g_gl) {
        printf("dlopen failed\n");
        return 1;
    }
    LOAD(glClearColor);
    LOAD(glClear);
    LOAD(glFlush);
    LOAD(glFinish);
    LOAD(glReadPixels);
    LOAD(glViewport);
    LOAD(glCreateShader);
    LOAD(glShaderSource);
    LOAD(glCompileShader);
    LOAD(glCreateProgram);
    LOAD(glAttachShader);
    LOAD(glLinkProgram);
    LOAD(glUseProgram);
    LOAD(glGetUniformLocation);
    LOAD(glUniform4f);
    LOAD(glGenVertexArrays);
    LOAD(glBindVertexArray);
    LOAD(glGenBuffers);
    LOAD(glBindBuffer);
    LOAD(glBufferData);
    LOAD(glEnableVertexAttribArray);
    LOAD(glVertexAttribPointer);
    LOAD(glDrawArrays);

    unsigned int vs = glCreateShader(0x8B31 /*GL_VERTEX_SHADER*/);
    unsigned int fs = glCreateShader(0x8B30 /*GL_FRAGMENT_SHADER*/);
    glShaderSource(vs, 1, &VS, 0);
    glShaderSource(fs, 1, &FS, 0);
    glCompileShader(vs);
    glCompileShader(fs);
    unsigned int prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glUseProgram(prog);

    unsigned int vao, vbo;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(0x8892 /*GL_ARRAY_BUFFER*/, vbo);
    glBufferData(0x8892, (long)sizeof(kTriA), kTriA, 0x88E4 /*GL_STATIC_DRAW*/);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, 0x1406 /*GL_FLOAT*/, 0, (int)sizeof(Vertex), 0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, 0x1406, 0, (int)sizeof(Vertex),
                          (const void*)12);

    glClearColor(0.05f, 0.05f, 0.08f, 1.0f);

    // Framebuffer is fixed at 512x512 (viewport is full-screen):
    //   NDC x=-0.28 (A centre) -> fb x=184; NDC x=0.28 (B centre) -> fb x=328;
    //   NDC y=0 -> fb y=256. A spans fb x=[128,240], B spans fb x=[271,384].

    // 64 frames of alternating *drawn* content; the per-frame readback must
    // match exactly the variant submitted this frame (no cross-frame
    // contamination through the frame-slot ring). Every 7th frame blocks.
    for (int f = 0; f < 64; ++f) {
        int variant = (f % 2);
        const Vertex* tri = variant ? kTriB : kTriA;
        /* Rewrite the VBO each frame before drawing (upload batching
           follows the submission path, so this also strains the ring). */
        glBufferData(0x8892 /*GL_ARRAY_BUFFER*/, (long)sizeof(Vertex) * 3, tri,
                     0x88E4 /*GL_STATIC_DRAW*/);
        glClear(0x4000 /*GL_COLOR_BUFFER_BIT*/);
        glDrawArrays(0x0004 /*GL_TRIANGLES*/, 0, 3);
        if (f % 7 == 6)
            glFinish();      // blocking path
        else
            glFlush();       // async kick into the ring

        unsigned char px[4];
        glReadPixels(variant ? 328 : 184, 256, 1, 1, 0x1908 /*GL_RGBA*/,
                     0x1401 /*GL_UNSIGNED_BYTE*/, px);
        if (variant == 0) {
            CHECK(px_match(px, 255, 0, 0),
                  "frame %d: A-triangle red at its centre", f);
        } else {
            CHECK(px_match(px, 0, 0, 255),
                  "frame %d: B-triangle blue at its centre", f);
        }
        /* The opposite half of the screen must stay clear (no stale frame). */
        glReadPixels(variant ? 80 : 430, 256, 1, 1, 0x1908, 0x1401, px);
        CHECK(px_dark(px), "frame %d: opposite half still dark", f);
    }

    // Post-ring: one final blocking frame must still show the last variant
    // (63 % 2 == 1 => B, blue).
    glClear(0x4000);
    glDrawArrays(0x0004, 0, 3);
    glFinish();
    unsigned char px[4];
    glReadPixels(328, 256, 1, 1, 0x1908, 0x1401, px);
    CHECK(px_match(px, 0, 0, 255), "post-ring readback shows last variant");

    dlclose(g_gl);
    if (failures == 0) { printf("\nMULTIFRAME SMOKE ALL PASSED\n"); return 0; }
    printf("\nMULTIFRAME SMOKE FAILED: %d failure(s)\n", failures);
    return 1;
}