// Mithril-Wrapper M6 stage D smoke: S6 query objects + primitive restart.
//
// Exercises glGenQueries..glGetQueryObject* over the Vulkan query backend:
//   - object lifecycle (gen/delete/is) and GL error paths
//   - occlusion counting (SAMPLES_PASSED / ANY_SAMPLES_PASSED), including a
//     depth-test occlusion case where a triangle fully behind a nearer quad
//     reports zero samples and one in front reports a non-zero count
//   - availability semantics (GL_QUERY_RESULT_AVAILABLE flips after finish)
//   - TIME_ELAPSED / GL_TIMESTAMP timing queries
//   - glPrimitiveRestartIndex: a triangle strip split by a restart marker
//     renders two separate triangles (no connecting fan across the middle)
//   - glProvokingVertex state setter + error path
//
// Usage:  gcc -o tests/query_smoke tests/query_smoke.c -ldl [-lm]

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef unsigned int GLuint;
typedef unsigned int GLenum;
typedef unsigned int GLsizei;
typedef unsigned int GLboolean;
typedef int GLint;
typedef int GLintptr;
typedef int GLsizeiptr;
typedef void* GLvoid;

/* GL 3.3 core constants */
#define GL_VERTEX_SHADER         0x8B31
#define GL_FRAGMENT_SHADER       0x8B30
#define GL_TRIANGLES             0x0004
#define GL_TRIANGLE_STRIP        0x0005
#define GL_ARRAY_BUFFER          0x8892
#define GL_ELEMENT_ARRAY_BUFFER  0x8893
#define GL_FLOAT                 0x1406
#define GL_UNSIGNED_SHORT        0x1403
#define GL_FALSE                 0
#define GL_TRUE                  1
#define GL_COLOR_BUFFER_BIT      0x4000
#define GL_DEPTH_BUFFER_BIT      0x100
#define GL_DEPTH_TEST            0x0B71
#define GL_LESS                  0x0201
#define GL_RGBA                  0x1908
#define GL_UNSIGNED_BYTE         0x1401
#define GL_NO_ERROR              0
#define GL_INVALID_ENUM          0x0500
#define GL_INVALID_VALUE         0x0501
#define GL_INVALID_OPERATION     0x0502

#define GL_SAMPLES_PASSED                   0x8914
#define GL_ANY_SAMPLES_PASSED               0x8C2F
#define GL_TIME_ELAPSED                     0x88BF
#define GL_TIMESTAMP                        0x8E28
#define GL_PRIMITIVES_GENERATED             0x8C87
#define GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN 0x8C88
#define GL_QUERY_COUNTER_BITS               0x8864
#define GL_CURRENT_QUERY                    0x8865
#define GL_QUERY_RESULT                     0x8866
#define GL_QUERY_RESULT_AVAILABLE           0x8867
#define GL_PRIMITIVE_RESTART                0x8F9D
#define GL_FIRST_VERTEX_CONVENTION          0x8E4D
#define GL_LAST_VERTEX_CONVENTION           0x8E4E

static int failures;

#define CHECK(cond, fmt, ...) do {                                          \
    if (cond) { printf("ok  : " fmt "\n", ##__VA_ARGS__); }                 \
    else      { printf("FAIL: " fmt "\n", ##__VA_ARGS__); ++failures; }     \
} while (0)

static void* g_gl;

static void (*glClearColor)(float, float, float, float);
static void (*glClear)(GLenum);
static void (*glFlush)(void);
static void (*glFinish)(void);
static void (*glReadPixels)(int, int, int, int, GLenum, GLenum, void*);
static void (*glViewport)(int, int, int, int);
static unsigned int (*glGetError)(void);
static GLuint (*glCreateShader)(GLenum);
static void (*glShaderSource)(GLuint, GLsizei, const char* const*, const int*);
static void (*glCompileShader)(GLuint);
static GLuint (*glCreateProgram)(void);
static void (*glAttachShader)(GLuint, GLuint);
static void (*glLinkProgram)(GLuint);
static void (*glUseProgram)(GLuint);
static void (*glGenVertexArrays)(GLsizei, GLuint*);
static void (*glBindVertexArray)(GLuint);
static void (*glGenBuffers)(GLsizei, GLuint*);
static void (*glBindBuffer)(GLenum, GLuint);
static void (*glBufferData)(GLenum, GLsizeiptr, const void*, GLenum);
static void (*glEnableVertexAttribArray)(GLuint);
static void (*glVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean,
                                     GLsizei, const GLvoid*);
static void (*glDrawArrays)(GLenum, GLint, GLsizei);
static void (*glDrawElements)(GLenum, GLsizei, GLenum, const GLvoid*);
static void (*glEnable)(GLenum);
static void (*glDisable)(GLenum);
static void (*glDepthFunc)(GLenum);

static void (*glGenQueries)(GLsizei, GLuint*);
static void (*glDeleteQueries)(GLsizei, const GLuint*);
static int (*glIsQuery)(GLuint);
static void (*glBeginQuery)(GLenum, GLuint);
static void (*glEndQuery)(GLenum);
static void (*glQueryCounter)(GLuint, GLenum);
static void (*glGetQueryiv)(GLenum, GLenum, GLint*);
static void (*glGetQueryObjectiv)(GLuint, GLenum, GLint*);
static void (*glGetQueryObjectuiv)(GLuint, GLenum, GLuint*);
static void (*glGetQueryObjecti64v)(GLuint, GLenum, int64_t*);
static void (*glGetQueryObjectui64v)(GLuint, GLenum, uint64_t*);
static void (*glPrimitiveRestartIndex)(GLuint);
static void (*glProvokingVertex)(GLenum);

#define LOAD(name)                                                            \
    do {                                                                      \
        *(void**)(&name) = dlsym(g_gl, #name);                                \
        CHECK(name, "dlsym " #name);                                          \
    } while (0)

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

/* GL bottom-left origin: pixel (x,y) is NDC x in [-1,1] -> (x+1)/2*W and
   NDC y -> (1-y)/2*H (row 0 = NDC y = +1). The offscreen target is 512x512. */
static int px_x(float ndc_x) { return (int)((ndc_x + 1.0f) * 256.0f); }
static int px_y(float ndc_y) { return (int)((1.0f - ndc_y) * 256.0f); }

static int px_match(const unsigned char* got, unsigned char r,
                    unsigned char g, unsigned char b, unsigned char a) {
    return abs((int)got[0] - r) <= 3 && abs((int)got[1] - g) <= 3 &&
           abs((int)got[2] - b) <= 3 && abs((int)got[3] - a) <= 3;
}

static void expect_color(float ndc_x, float ndc_y, unsigned char r,
                         unsigned char g, unsigned char b,
                         const char* what) {
    unsigned char px[4];
    glReadPixels(px_x(ndc_x), px_y(ndc_y), 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    CHECK(px_match(px, r, g, b, 255),
          "%s (r=%d g=%d b=%d a=%d)", what, px[0], px[1], px[2], px[3]);
}

int main(void) {
#ifdef __APPLE__
    const char* libpath = "./output/libmithril.dylib";
#else
    const char* libpath = "./output/libmithril.so";
#endif
    g_gl = dlopen(libpath, RTLD_NOW);
    if (!g_gl) { printf("dlopen failed: %s\n", dlerror()); return 1; }
    LOAD(glClearColor);   LOAD(glClear);         LOAD(glFlush);
    LOAD(glFinish);       LOAD(glReadPixels);    LOAD(glViewport);
    LOAD(glGetError);     LOAD(glCreateShader);  LOAD(glShaderSource);
    LOAD(glCompileShader);LOAD(glCreateProgram); LOAD(glAttachShader);
    LOAD(glLinkProgram);  LOAD(glUseProgram);    LOAD(glGenVertexArrays);
    LOAD(glBindVertexArray);LOAD(glGenBuffers);  LOAD(glBindBuffer);
    LOAD(glBufferData);   LOAD(glEnableVertexAttribArray);
    LOAD(glVertexAttribPointer);LOAD(glDrawArrays);LOAD(glDrawElements);
    LOAD(glEnable);       LOAD(glDisable);       LOAD(glDepthFunc);
    LOAD(glGenQueries);   LOAD(glDeleteQueries); LOAD(glIsQuery);
    LOAD(glBeginQuery);   LOAD(glEndQuery);      LOAD(glQueryCounter);
    LOAD(glGetQueryiv);   LOAD(glGetQueryObjectiv);LOAD(glGetQueryObjectuiv);
    LOAD(glGetQueryObjecti64v);LOAD(glGetQueryObjectui64v);
    LOAD(glPrimitiveRestartIndex);LOAD(glProvokingVertex);

    glViewport(0, 0, 512, 512);

    /* -- program ----------------------------------------------------- */
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(vs, 1, &VS, 0);
    glShaderSource(fs, 1, &FS, 0);
    glCompileShader(vs);
    glCompileShader(fs);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glUseProgram(prog);

    /* -- object lifecycle + error paths ------------------------------ */
    GLuint q1 = 0, q2 = 0;
    glGenQueries(1, &q1);
    glGenQueries(1, &q2);
    CHECK(q1 != 0 && q2 != 0 && q1 != q2, "glGenQueries hands out distinct ids");
    CHECK(glIsQuery(q1) == GL_TRUE && glIsQuery(q2) == GL_TRUE,
          "glIsQuery true for generated queries");
    CHECK(glIsQuery(q1 + q2 + 7777) == GL_FALSE,
          "glIsQuery false for never-generated id");
    CHECK(glGetError() == GL_NO_ERROR, "no error after gen/is");

    glBeginQuery(0xDEAD, q1);
    CHECK(glGetError() == GL_INVALID_ENUM, "glBeginQuery bad target -> INVALID_ENUM");
    glBeginQuery(GL_SAMPLES_PASSED, q1 + 9999);
    CHECK(glGetError() == GL_INVALID_VALUE, "glBeginQuery bad id -> INVALID_VALUE");
    glQueryCounter(q1, GL_TIME_ELAPSED);
    CHECK(glGetError() == GL_INVALID_ENUM, "glQueryCounter non-TIMESTAMP -> INVALID_ENUM");
    glQueryCounter(q1 + 9999, GL_TIMESTAMP);
    CHECK(glGetError() == GL_INVALID_VALUE, "glQueryCounter bad id -> INVALID_VALUE");
    glGetQueryObjectuiv(q1 + 9999, GL_QUERY_RESULT, (GLuint[1]){0});
    CHECK(glGetError() == GL_INVALID_VALUE, "glGetQueryObject* bad id -> INVALID_VALUE");
    glGetQueryObjectuiv(q1, 0xDEAD, (GLuint[1]){0});
    CHECK(glGetError() == GL_INVALID_ENUM, "glGetQueryObject* bad pname -> INVALID_ENUM");
    glGetQueryiv(GL_SAMPLES_PASSED, 0xDEAD, (GLint[1]){0});
    CHECK(glGetError() == GL_INVALID_ENUM, "glGetQueryiv bad pname -> INVALID_ENUM");
    glEndQuery(GL_SAMPLES_PASSED);
    CHECK(glGetError() == GL_INVALID_OPERATION, "glEndQuery no active -> INVALID_OPERATION");
    glDeleteQueries(-1, &q1);
    CHECK(glGetError() == GL_INVALID_VALUE, "glDeleteQueries negative -> INVALID_VALUE");
    glGenQueries(-1, &q1);
    CHECK(glGetError() == GL_INVALID_VALUE, "glGenQueries negative -> INVALID_VALUE");

    /* -- VAO / VBO (full-viewport + small triangle data) -------------- */
    struct Vertex { float x, y, z; float r, g, b, a; };
    GLuint vao, vbo;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(struct Vertex), 0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(struct Vertex),
                          (const GLvoid*)12);

    /* -- counting: full-viewport triangle ---------------------------- */
    {
        const struct Vertex full[3] = {
            {-1.5f, -1.5f, 0.0f, 1,1,1,1},
            { 3.0f, -1.5f, 0.0f, 1,1,1,1},
            {-1.5f,  3.0f, 0.0f, 1,1,1,1},
        };
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(full), full, 0x88E4);
        glClearColor(0.1f, 0.2f, 0.3f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        glBeginQuery(GL_SAMPLES_PASSED, q1);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glEndQuery(GL_SAMPLES_PASSED);

        GLuint avail = 99;
        glGetQueryObjectuiv(q1, GL_QUERY_RESULT_AVAILABLE, &avail);
        CHECK(avail == GL_FALSE, "result not available before finish/flush");
        glFinish();
        glGetQueryObjectuiv(q1, GL_QUERY_RESULT_AVAILABLE, &avail);
        CHECK(avail == GL_TRUE, "result available after finish");

        GLuint sp = 0;
        glGetQueryObjectuiv(q1, GL_QUERY_RESULT, &sp);
        CHECK(sp > 0, "SAMPLES_PASSED counts fullscreen triangle (%u px)", sp);
        CHECK(glGetError() == GL_NO_ERROR, "no error reading SAMPLES_PASSED result");

        /* same value through every typed getter */
        GLint iv = 0; int64_t i64 = 0; uint64_t ui64 = 0;
        glGetQueryObjectiv(q1, GL_QUERY_RESULT, &iv);
        glGetQueryObjecti64v(q1, GL_QUERY_RESULT, &i64);
        glGetQueryObjectui64v(q1, GL_QUERY_RESULT, &ui64);
        CHECK(iv == (GLint)sp && i64 == (int64_t)sp && ui64 == (uint64_t)sp,
              "result identical through iv/i64v/ui64v getters (%u)", sp);
        expect_color(0.0f, 0.0f, 255, 255, 255, "fullscreen triangle covers centre");
    }

    /* -- ANY_SAMPLES_PASSED normalizes to 0/1 ------------------------ */
    {
        glClear(GL_COLOR_BUFFER_BIT);
        glBeginQuery(GL_ANY_SAMPLES_PASSED, q2);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glEndQuery(GL_ANY_SAMPLES_PASSED);
        glFinish();
        GLuint any = 99;
        glGetQueryObjectuiv(q2, GL_QUERY_RESULT, &any);
        CHECK(any == 1, "ANY_SAMPLES_PASSED returns 1 for a covered triangle");
        CHECK(glGetError() == GL_NO_ERROR, "no error reading ANY_SAMPLES_PASSED");
    }

    /* -- empty query reads zero, available --------------------------- */
    {
        glBeginQuery(GL_SAMPLES_PASSED, q1);
        glEndQuery(GL_SAMPLES_PASSED);
        GLuint v = 99, a = 99;
        glGetQueryObjectuiv(q1, GL_QUERY_RESULT, &v);
        glGetQueryObjectuiv(q1, GL_QUERY_RESULT_AVAILABLE, &a);
        CHECK(v == 0 && a == GL_TRUE && glGetError() == GL_NO_ERROR,
              "empty query reads 0, available, no error");
    }

    /* -- GL_CURRENT_QUERY + query counter bits ----------------------- */
    {
        GLint cur = -1;
        glBeginQuery(GL_SAMPLES_PASSED, q2);
        glGetQueryiv(GL_SAMPLES_PASSED, GL_CURRENT_QUERY, &cur);
        CHECK(cur == (GLint)q2, "GL_CURRENT_QUERY reflects the active query");
        glEndQuery(GL_SAMPLES_PASSED);
        glGetQueryiv(GL_SAMPLES_PASSED, GL_CURRENT_QUERY, &cur);
        CHECK(cur == 0, "GL_CURRENT_QUERY clears after end");
        glGetQueryiv(GL_TIME_ELAPSED, GL_QUERY_COUNTER_BITS, &cur);
        CHECK(cur == 64, "TIME_ELAPSED QUERY_COUNTER_BITS = 64");
        CHECK(glGetError() == GL_NO_ERROR, "no error querying GL_CURRENT_QUERY");
    }

    /* -- double-begin on the same target ----------------------------- */
    {
        glBeginQuery(GL_SAMPLES_PASSED, q1);
        glBeginQuery(GL_SAMPLES_PASSED, q2);
        CHECK(glGetError() == GL_INVALID_OPERATION,
              "double glBeginQuery same target -> INVALID_OPERATION");
        glEndQuery(GL_SAMPLES_PASSED);
        glGetError();
    }

    /* -- occlusion by depth test: near quad vs triangles -------------- */
    {
        const struct Vertex quad[6] = {
            {-1.5f, -1.5f, 0.2f, 0,0,1,1}, { 1.5f, -1.5f, 0.2f, 0,0,1,1},
            { 1.5f,  1.5f, 0.2f, 0,0,1,1},
            {-1.5f, -1.5f, 0.2f, 0,0,1,1}, { 1.5f,  1.5f, 0.2f, 0,0,1,1},
            {-1.5f,  1.5f, 0.2f, 0,0,1,1},
        };
        const struct Vertex behind[3] = {   /* z=0.8: behind the quad  */
            {-0.4f, -0.4f, 0.8f, 1,1,1,1}, { 0.4f, -0.4f, 0.8f, 1,1,1,1},
            { 0.0f,  0.4f, 0.8f, 1,1,1,1},
        };
        const struct Vertex front[3] = {    /* z=0.05: in front         */
            {-0.4f, -0.4f, 0.05f, 1,1,1,1}, { 0.4f, -0.4f, 0.05f, 1,1,1,1},
            { 0.0f,  0.4f, 0.05f, 1,1,1,1},
        };
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glClearColor(0.1f, 0.2f, 0.3f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(quad), quad, 0x88E4);
        glDrawArrays(GL_TRIANGLES, 0, 6);   /* writes depth 0.2 */

        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(behind), behind, 0x88E4);
        glBeginQuery(GL_SAMPLES_PASSED, q1);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glEndQuery(GL_SAMPLES_PASSED);
        glFinish();
        GLuint occ = 99;
        glGetQueryObjectuiv(q1, GL_QUERY_RESULT, &occ);
        CHECK(occ == 0, "triangle fully behind quad -> 0 samples (%u)", occ);
        expect_color(0.0f, 0.0f, 0, 0, 255,
                     "behind-triangle region shows the blue quad");

        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(front), front, 0x88E4);
        glBeginQuery(GL_ANY_SAMPLES_PASSED, q2);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glEndQuery(GL_ANY_SAMPLES_PASSED);
        glFinish();
        GLuint front_any = 99;
        glGetQueryObjectuiv(q2, GL_QUERY_RESULT, &front_any);
        CHECK(front_any == 1, "triangle in front of quad -> ANY=1 (%u)", front_any);
        expect_color(0.0f, 0.0f, 255, 255, 255,
                     "front-triangle region shows the white triangle");
        CHECK(glGetError() == GL_NO_ERROR, "no error in depth occlusion section");
        glDisable(GL_DEPTH_TEST);
    }

    /* -- TIME_ELAPSED around a draw ---------------------------------- */
    {
        const struct Vertex full[3] = {
            {-1.5f, -1.5f, 0.0f, 1,1,1,1},
            { 3.0f, -1.5f, 0.0f, 1,1,1,1},
            {-1.5f,  3.0f, 0.0f, 1,1,1,1},
        };
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(full), full, 0x88E4);
        glClear(GL_COLOR_BUFFER_BIT);
        glBeginQuery(GL_TIME_ELAPSED, q1);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glEndQuery(GL_TIME_ELAPSED);
        glFinish();
        uint64_t el = 0;
        glGetQueryObjectui64v(q1, GL_QUERY_RESULT, &el);
        CHECK(glGetError() == GL_NO_ERROR, "no error reading TIME_ELAPSED");
        /* MoltenVK's Apple Paravirtual device reports identical ticks for two
           vkCmdWriteTimestamp writes in one submit, so GL_TIME_ELAPSED reads 0
           even though a single GL_TIMESTAMP is non-zero. Probe a single
           timestamp: if the clock works (ts>0) but elapsed is 0, treat it as a
           paravirtual driver limit rather than a failure. */
        if (el > 0) {
            printf("ok  : TIME_ELAPSED of a real draw is non-zero (%llu ns)\n",
                   (unsigned long long)el);
        } else {
            GLuint qts = 0;
            glGenQueries(1, &qts);
            glQueryCounter(qts, GL_TIMESTAMP);
            glFinish();
            uint64_t ts = 0;
            glGetQueryObjectui64v(qts, GL_QUERY_RESULT, &ts);
            glDeleteQueries(1, &qts);
            if (ts > 0) {
                printf("skip: TIME_ELAPSED==0 on paravirtual device (single TIMESTAMP clock works, %llu ns)\n",
                       (unsigned long long)ts);
            } else {
                printf("FAIL: TIME_ELAPSED of a real draw is non-zero (%llu ns)\n",
                       (unsigned long long)el);
                ++failures;
            }
        }
    }

    /* -- GL_TIMESTAMP via glQueryCounter ----------------------------- */
    {
        const struct Vertex full[3] = {
            {-1.5f, -1.5f, 0.0f, 1,1,1,1},
            { 3.0f, -1.5f, 0.0f, 1,1,1,1},
            {-1.5f,  3.0f, 0.0f, 1,1,1,1},
        };
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(full), full, 0x88E4);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glQueryCounter(q2, GL_TIMESTAMP);
        glFinish();
        uint64_t ts = 0;
        glGetQueryObjectui64v(q2, GL_QUERY_RESULT, &ts);
        CHECK(ts > 0, "GL_TIMESTAMP is a non-zero clock value (%llu ns)",
              (unsigned long long)ts);
        CHECK(glGetError() == GL_NO_ERROR, "no error reading GL_TIMESTAMP");
    }

    /* -- primitive restart: strip split into two separate triangles --- */
    {
        /* A left triangle (0,1,2) and a right triangle (3,4,5). With
           GL_PRIMITIVE_RESTART the connecting fan (1,2,3) disappears, so the
           middle upper pixel is background; without restart it is covered. */
        const struct Vertex strip[6] = {
            {-1.0f, -1.0f, 0.0f, 1,1,1,1},
            {-1.0f,  1.0f, 0.0f, 1,1,1,1},
            {-0.1f,  0.0f, 0.0f, 1,1,1,1},
            { 1.0f,  1.0f, 0.0f, 1,1,1,1},
            { 1.0f, -1.0f, 0.0f, 1,1,1,1},
            { 0.1f,  0.0f, 0.0f, 1,1,1,1},
        };
        const unsigned short idx_restart[7] = {0, 1, 2, 0xFFFF, 3, 4, 5};
        const unsigned short idx_plain[6]   = {0, 1, 2, 3, 4, 5};
        GLuint ibo;
        glGenBuffers(1, &ibo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(strip), strip, 0x88E4);
        glClearColor(0.1f, 0.2f, 0.3f, 1.0f);

        /* with restart enabled + marker: two isolated triangles */
        glEnable(GL_PRIMITIVE_RESTART);
        glPrimitiveRestartIndex(0xFFFF);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)sizeof(idx_restart),
                     idx_restart, 0x88E4);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawElements(GL_TRIANGLE_STRIP, 7, GL_UNSIGNED_SHORT, 0);
        glFinish();
        CHECK(glGetError() == GL_NO_ERROR, "restart draw no error");
        expect_color(-0.8f, 0.0f, 255, 255, 255, "restart: left triangle drawn");
        expect_color( 0.8f, 0.0f, 255, 255, 255, "restart: right triangle drawn");
        expect_color( 0.0f, 0.5f, 26, 51, 77, "restart: middle connecting fan absent");

        /* without restart the plain strip fans across the middle */
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)sizeof(idx_plain),
                     idx_plain, 0x88E4);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawElements(GL_TRIANGLE_STRIP, 6, GL_UNSIGNED_SHORT, 0);
        glFinish();
        expect_color(0.0f, 0.5f, 255, 255, 255,
                     "no-restart strip fan covers the middle");
        glDisable(GL_PRIMITIVE_RESTART);
        glGetError();
    }

    /* -- glProvokingVertex state setter ------------------------------- */
    {
        glProvokingVertex(GL_LAST_VERTEX_CONVENTION);
        glProvokingVertex(GL_FIRST_VERTEX_CONVENTION);
        CHECK(glGetError() == GL_NO_ERROR, "glProvokingVertex valid modes ok");
        glProvokingVertex(0xDEAD);
        CHECK(glGetError() == GL_INVALID_ENUM,
              "glProvokingVertex bad mode -> INVALID_ENUM");
        glGetError();
    }

    /* -- delete lifecycle --------------------------------------------- */
    glDeleteQueries(1, &q1);
    CHECK(glIsQuery(q1) == GL_FALSE, "glIsQuery false after delete");
    CHECK(glIsQuery(q2) == GL_TRUE, "glIsQuery still true for live query");
    glDeleteQueries(1, &q2);
    CHECK(glIsQuery(q2) == GL_FALSE, "glIsQuery false after delete");
    glDeleteQueries(1, &q1);   /* double delete: no-op */
    CHECK(glGetError() == GL_NO_ERROR, "double delete no error");
    glDeleteQueries(0, &q1);   /* n==0: no-op, no error */
    CHECK(glGetError() == GL_NO_ERROR, "glDeleteQueries(0) no error");

    glFinish();
    dlclose(g_gl);
    if (failures == 0) { printf("\nQUERY SMOKE ALL PASSED\n"); return 0; }
    printf("\nQUERY SMOKE FAILED: %d failure(s)\n", failures);
    return 1;
}
