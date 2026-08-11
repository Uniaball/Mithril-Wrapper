// Mithril-Wrapper M6 stage C smoke: GL sync objects (S6).
//
// Exercises the glFenceSync family against the Vulkan backend: create a
// fence after drawing, verify it signals once the previously flushed frame
// completes, query its GL_SYNC ivars, degrade to always-signaled when the
// engine is not up, and check the GL error paths (bad condition / deleted
// sync).
//
// Usage:  gcc -o tests/sync_smoke tests/sync_smoke.c -ldl [-lm]

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct __GLsync* GLsync;

static void* g_gl;
static void (*glClearColor)(float, float, float, float);
static void (*glClear)(int);
static void (*glFlush)(void);
static void (*glFinish)(void);
static void (*glReadPixels)(int, int, int, int, int, int, void*);
static void (*glViewport)(int, int, int, int);
static unsigned int (*glGetError)(void);
static GLsync (*glFenceSync)(int, int);
static void (*glDeleteSync)(GLsync);
static int (*glIsSync)(GLsync);
static int (*glClientWaitSync)(GLsync, int, uint64_t);
static void (*glWaitSync)(GLsync, int, uint64_t);
static void (*glGetSynciv)(GLsync, int, int, int*, int*);

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

// GL constants not pulled from a GL header (the smoke only links libdl).
#define GL_COLOR_BUFFER_BIT      0x4000
#define GL_RGBA                  0x1908
#define GL_UNSIGNED_BYTE         0x1401
#define GL_SYNC_GPU_COMMANDS_COMPLETE 0x9117
#define GL_OBJECT_TYPE           0x9112
#define GL_SYNC_CONDITION        0x9113
#define GL_SYNC_STATUS           0x9114
#define GL_SYNC_FLAGS            0x9115
#define GL_SYNC_FENCE            0x9116
#define GL_SIGNALED              0x9119
#define GL_UNSIGNALED            0x9118
#define GL_ALREADY_SIGNALED      0x911A
#define GL_TIMEOUT_EXPIRED       0x911B
#define GL_CONDITION_SATISFIED   0x911C
#define GL_WAIT_FAILED           0x911D
#define GL_SYNC_FLUSH_COMMANDS_BIT 0x00000001
#define GL_TIMEOUT_IGNORED       0xFFFFFFFFFFFFFFFFull
#define GL_INVALID_ENUM          0x0500
#define GL_INVALID_VALUE         0x0501
#define GL_NO_ERROR              0

static int QuerySynciv(GLsync sync, int pname) {
    int v = -1, len = -1;
    glGetSynciv(sync, pname, 1, &len, &v);
    return v;
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
    LOAD(glGetError);
    LOAD(glFenceSync);
    LOAD(glDeleteSync);
    LOAD(glIsSync);
    LOAD(glClientWaitSync);
    LOAD(glWaitSync);
    LOAD(glGetSynciv);

    glViewport(0, 0, 64, 64);

    // A sync created before any engine-init call still yields a real object:
    // CreateGLSync lazily boots the backend, so a blocking wait must complete
    // (no commands were recorded, but the fence fires with its empty batch).
    GLsync early = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    CHECK(early != 0, "fence before engine init returns non-null");
    CHECK(glIsSync(early), "glIsSync(early)");
    CHECK(QuerySynciv(early, GL_OBJECT_TYPE) == GL_SYNC_FENCE, "early OBJECT_TYPE");
    int ew = glClientWaitSync(early, 0, 2000000000ull);
    CHECK(ew == GL_ALREADY_SIGNALED || ew == GL_CONDITION_SATISFIED,
          "early ClientWaitSync satisfied");
    CHECK(QuerySynciv(early, GL_SYNC_STATUS) == GL_SIGNALED,
          "early STATUS signaled after wait");
    CHECK(glGetError() == GL_NO_ERROR, "no error from early fence path");
    glDeleteSync(early);
    CHECK(!glIsSync(early), "glIsSync(early) false after delete");

    // Live path: clear (spins the backend up), draw lots of GPU work, snap a
    // fence, flush async, then wait for it and verify ordered completion.
    glClearColor(0.2f, 0.3f, 0.9f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    GLsync main = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    CHECK(main != 0, "glFenceSync after clear returns non-null");
    CHECK(glIsSync(main), "glIsSync(main)");
    CHECK(QuerySynciv(main, GL_OBJECT_TYPE) == GL_SYNC_FENCE, "main OBJECT_TYPE");
    CHECK(QuerySynciv(main, GL_SYNC_CONDITION) == GL_SYNC_GPU_COMMANDS_COMPLETE,
          "main SYNC_CONDITION");
    CHECK(QuerySynciv(main, GL_SYNC_FLAGS) == 0, "main SYNC_FLAGS");

    // glGetSynciv must not flush: the pending clear frame is still recorded
    // and the fence (flushed at creation) may or may not be done yet, but a
    // pure status read is legal at any value.
    int st = QuerySynciv(main, GL_SYNC_STATUS);
    CHECK(st == GL_SIGNALED || st == GL_UNSIGNALED, "main STATUS legal");

    // Blocking wait completes and the clear is ordered before the fence.
    int w = glClientWaitSync(main, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
    CHECK(w == GL_ALREADY_SIGNALED || w == GL_CONDITION_SATISFIED,
          "main ClientWaitSync satisfied");
    CHECK(glGetError() == GL_NO_ERROR, "no error after live wait");
    CHECK(QuerySynciv(main, GL_SYNC_STATUS) == GL_SIGNALED,
          "main STATUS signaled after wait");

    // The wait drained the pending frame; the readback reflects it.
    unsigned char px[4];
    glReadPixels(16, 16, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    CHECK(px[0] == 51 && px[1] == 76 && px[2] == 230,
          "readback matches frame covered by fence (%d,%d,%d)",
          px[0], px[1], px[2]);

    // Zero-timeout poll on an unsignaled live fence: draw, fence, no flush.
    glClearColor(0, 1, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    GLsync late = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    int poll = glClientWaitSync(late, 0, 0);
    CHECK(poll == GL_ALREADY_SIGNALED || poll == GL_TIMEOUT_EXPIRED,
          "zero-timeout poll returns signaled-or-expired");
    poll = glClientWaitSync(late, GL_SYNC_FLUSH_COMMANDS_BIT, 0);
    CHECK(poll == GL_ALREADY_SIGNALED || poll == GL_TIMEOUT_EXPIRED,
          "poll with flush bit returns signaled-or-expired");
    int pl = glClientWaitSync(late, 0, 2000000000ull);
    CHECK(pl == GL_ALREADY_SIGNALED || pl == GL_CONDITION_SATISFIED,
          "blocking wait after poll satisfied");
    CHECK(QuerySynciv(late, GL_SYNC_STATUS) == GL_SIGNALED,
          "late STATUS signaled after poll+flush");
    glDeleteSync(late);

    // glWaitSync no-op sanity (flags/timeout validated).
    glWaitSync(main, 0, GL_TIMEOUT_IGNORED);
    CHECK(glGetError() == GL_NO_ERROR, "glWaitSync ok");
    glWaitSync(main, 1, GL_TIMEOUT_IGNORED);
    CHECK(glGetError() == GL_INVALID_VALUE, "glWaitSync bad flags -> INVALID_VALUE");
    glGetError();

    // Error paths.
    glFenceSync(0xDEAD, 0);
    CHECK(glGetError() == GL_INVALID_ENUM, "glFenceSync bad condition");
    CHECK(glIsSync((GLsync)0x1) == 0, "glIsSync bogus handle false");
    int r = glClientWaitSync((GLsync)0x1, 0, 0);
    CHECK(r == GL_WAIT_FAILED && glGetError() == GL_INVALID_VALUE,
          "glClientWaitSync deleted/invalid -> WAIT_FAILED");
    glDeleteSync(main);
    CHECK(!glIsSync(main), "glIsSync(main) false after delete");
    glFinish();

    dlclose(g_gl);
    if (failures == 0) { printf("\nSYNC SMOKE ALL PASSED\n"); return 0; }
    printf("\nSYNC SMOKE FAILED: %d failure(s)\n", failures);
    return 1;
}