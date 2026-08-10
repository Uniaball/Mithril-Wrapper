// Mithril-Wrapper M6 stage A smoke: double-buffered frame submission.
//
// Renders 64 frames of alternating colours with glFlush (async kick, no
// wait), letting the engine rotate between its two frame slots while the
// GPU rides ahead. Every frame is then read back and verified against the
// colour that frame was cleared to — proving the ring never cross-contaminates
// staging/descriptors/UBO and that a later readback sees the latest frame.
//
// Also exercises a burst of interleaved async+sync: half the frames use
// glFinish (blocking) to prove both paths coexist on the same slot ring.
//
// Usage:  gcc -o tests/multiframe_smoke tests/multiframe_smoke.c -ldl

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

static void ReadPixel(int x, int y, unsigned char out[4]) {
    glReadPixels(x, y, 1, 1, 0x1908 /*GL_RGBA*/, 0x1401 /*GL_UNSIGNED_BYTE*/,
                 out);
}

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

    glViewport(0, 0, 64, 64);

    // 64 frames, alternating red/blue/green clears. Async flush the ring;
    // every 7th frame use a blocking glFinish to interleave both paths.
    const unsigned char palette[3][3] = {{255, 0, 0}, {0, 0, 255}, {0, 255, 0}};
    for (int f = 0; f < 64; ++f) {
        const unsigned char* col = palette[f % 3];
        glClearColor(col[0] / 255.f, col[1] / 255.f, col[2] / 255.f, 1.f);
        glClear(0x4000 /*GL_COLOR_BUFFER_BIT*/);
        if (f % 7 == 6)
            glFinish();      // blocking path
        else
            glFlush();       // async kick into the ring

        unsigned char px[4];
        ReadPixel(16, 16, px);
        CHECK(px[0] == col[0] && px[1] == col[1] && px[2] == col[2],
              "frame %d readback colour", f);
    }

    // After the ring has torn down every slot, one final glFinish + readback
    // must still return the last cleared colour (63 % 3 == 0 => red).
    glFinish();
    unsigned char px[4];
    ReadPixel(16, 16, px);
    CHECK(px[0] == 255 && px[1] == 0 && px[2] == 0, "post-ring readback");

    dlclose(g_gl);
    if (failures == 0) { printf("\nMULTIFRAME SMOKE ALL PASSED\n"); return 0; }
    printf("\nMULTIFRAME SMOKE FAILED: %d failure(s)\n", failures);
    return 1;
}