// Mithril-Wrapper GL layer internal shared state (split across the
// per-domain translation units: state.cpp / shader.cpp / vertex.cpp /
// draw.cpp). Each TU owns one domain and exposes its entry points through
// its own `extern "C"` block, so the MGL_IMPL list in gen_gl_stubs.py keeps
// excluding exactly the same symbols.
//
// Only the definitions actually shared between two or more TUs live here:
//  - object tables for VAO/VBO state (vertex + draw path),
//  - the lazy program->Vulkan-program map (shader + draw path),
//  - attribute fetch helpers (draw path, called through the header below).

#pragma once

// glcorearb.h hides the APIENTRY prototypes behind GL_GLEXT_PROTOTYPES;
// every GL-layer TU defines and calls gl* entry points, so expose them.
#define GL_GLEXT_PROTOTYPES 1

#include <GL/glcorearb.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <shader/shader.h>
#include <state/state.h>
#include <vk/engine.h>

namespace s = mithril::state;
namespace v = mithril::vk;

#define PUSH_ERROR(e) s::GetState().errors.Push((e))

// ---- shared vertex attribute state (vertex.cpp owns the storage) --------

constexpr GLuint kMaxAttribs = 16;

// One enabled/constant vertex attribute slot.
struct AttribData {
    bool enabled = false;
    GLint size = 0;            // 1..4 components
    GLenum type = GL_FLOAT;
    GLboolean normalized = GL_FALSE;
    GLsizei stride = 0;
    GLsizeiptr offset = 0;
    GLuint buffer = 0;         // GL_ARRAY_BUFFER bound at glVertexAttribPointer
    GLuint divisor = 0;        // glVertexAttribDivisor
    bool is_pointer = false;   // array (pointer) vs generic constant value
    std::array<GLfloat, 4> constant{0.0f, 0.0f, 0.0f, 1.0f};
    // Stage F (S3): glVertexAttribI* keeps the exact integer constants so
    // glGetVertexAttribIiv/Iuiv answer precisely (the float mirror above is
    // what the draw path consumes).
    std::array<GLint, 4> constant_i{0, 0, 0, 1};
};

struct VAOData {
    std::array<AttribData, kMaxAttribs> attribs{};
};

struct BufferData {
    std::vector<uint8_t> data;
};

// Storage lives in vertex.cpp; the draw path reads these through the header.
extern std::unordered_map<GLuint, VAOData> g_vaos;
extern std::unordered_map<GLuint, BufferData> g_buffers;
extern GLuint g_bound_vao;           // default VAO is 0
extern GLuint g_bound_array_buffer;
extern GLuint g_bound_element_buffer;

// GL program id -> Vulkan program handle (lazily created at first draw).
extern std::unordered_map<GLuint, uint64_t> g_vk_programs;

// ---- Stage F (S3): transform feedback CPU counting -------------------------
// glBegin/EndTransformFeedback bracket a session; DrawCommon accumulates the
// primitive count while active, and glEndTransformFeedback writes it into
// the bound GL_TRANSFORM_FEEDBACK_BUFFER mirrors (CPU-side count, per the
// degraded MobileGL-style contract). Storage lives in vertex.cpp.
extern bool g_tfb_active;          // session open (Begin seen, End not yet)
extern uint64_t g_tfb_primitives;  // primitives generated since Begin

// glBindBufferBase/Range: target -> index -> bound buffer + range.
// GL_TRANSFORM_FEEDBACK_BUFFER and GL_UNIFORM_BUFFER both land here.
constexpr GLuint kMaxIndexedBindings = 64;
struct IndexedBinding {
    GLuint buffer = 0;
    GLintptr offset = 0;
    GLsizeiptr size = 0;   // -1 = whole buffer (BindBufferBase)
};
extern std::unordered_map<GLenum, std::array<IndexedBinding, kMaxIndexedBindings>>
    g_indexed_bindings;

// ---- shared texture state (texture.cpp owns the storage) ------------------

// Mirrors the engine's texture-unit count (v::kMaxUnits); GL uses units
// beyond this only as a no-op.
constexpr GLuint kMaxTexUnits = 16;

// CPU-side image of one GL texture object (M4). Pixels are kept as an RGBA8
// mip chain so TexSubImage/GenerateMipmap can rebuild the upload cheaply.
// Layered targets concatenate their slices inside each level buffer in the
// same order vk::TexUpload expects (3D: z; array: layers; cube: 6 faces).
struct TexState {
    GLenum target = GL_TEXTURE_2D;       // target bound at creation
    GLenum min_filter = GL_LINEAR;       // sampler state (GL enums)
    GLenum mag_filter = GL_LINEAR;
    GLenum wrap_s = GL_REPEAT, wrap_t = GL_REPEAT, wrap_r = GL_REPEAT;
    uint32_t width = 0, height = 0, depth = 1;  // depth: 3D z / array layers
    // Stage F (S4): multisample textures are recorded (dims + sample count)
    // but degrade to a single-sample CPU mirror at upload/FBO time.
    GLint samples = 0;
    GLboolean fixed_sample_locations = GL_FALSE;
    std::vector<std::vector<uint8_t>> mip;      // [level] slices concatenated
    bool has_image = false;                    // level 0 present
    // Partial re-upload tracking: per-level dirty rectangles accumulated since
    // the last flush (union bounding box per level, so a frame of scattered
    // glTexSubImage2D calls coalesce into one region copy). `dirty_full`
    // forces a full rebuild of the whole texture (image redefinition, sampler
    // change, mipmap regen) -- those paths rebuild the resident image/sampler.
    struct DirtyRect {
        uint32_t x = 0, y = 0, w = 0, h = 0;
        bool valid = false;
    };
    std::vector<DirtyRect> dirty;
    bool dirty_full = false;
    void ClearDirty() {
        dirty.clear();
        dirty_full = false;
    }
    // Compressed mirror: raw S3TC bytes per level (kept for GetCompressedTexImage).
    std::vector<std::vector<uint8_t>> comp;    // comp[level], empty = none
    bool has_comp = false;
    GLint comp_format = 0;
    // glTexBuffer attachment (buffer texture; mirror holds a 1xN 2D image).
    bool has_tex_buffer = false;
    GLuint tex_buffer = 0;
    GLenum tex_buffer_format = 0;

    bool IsCube() const { return target == GL_TEXTURE_CUBE_MAP; }
    bool Is3D() const { return target == GL_TEXTURE_3D; }
    size_t SliceCount() const {
        if (IsCube()) return 6;
        if (Is3D() || target == GL_TEXTURE_2D_ARRAY ||
            target == GL_TEXTURE_1D_ARRAY ||
            target == GL_TEXTURE_2D_MULTISAMPLE_ARRAY)
            return depth;
        return 1;
    }
    size_t SliceBytes(uint32_t level) const {
        uint32_t w = std::max<uint32_t>(1, width >> level);
        uint32_t h = std::max<uint32_t>(1, height >> level);
        return (size_t)w * h * 4;
    }
};

extern std::unordered_map<GLuint, TexState> g_textures;
extern std::array<GLuint, kMaxTexUnits> g_texture_units;  // unit -> texture id
extern GLuint g_next_texture;

// Texture ids whose CPU mirror changed while the Vulkan backend was not yet
// initialized; flushed to the engine at the next draw (see DrawCommon).
extern std::unordered_set<GLuint> g_dirty_textures;
void FlushDirtyTextureUploads();   // defined in texture.cpp

// ---- shared sampler state (sampler.cpp owns the storage) ------------------

// CPU-side image of one GL sampler object (M6 stage E). Fields mirror the
// sampler subset of TexState so the same TexSamplerInfo projection works.
struct SamplerState {
    GLenum min_filter = GL_LINEAR;
    GLenum mag_filter = GL_LINEAR;
    GLenum wrap_s = GL_REPEAT, wrap_t = GL_REPEAT, wrap_r = GL_REPEAT;
};

extern std::unordered_map<GLuint, SamplerState> g_samplers;
extern std::array<GLuint, kMaxTexUnits> g_sampler_units;  // unit -> sampler id (0 = none)
extern GLuint g_next_sampler;
extern std::unordered_set<GLuint> g_dirty_samplers;
void FlushDirtySamplerUploads();   // defined in sampler.cpp

// ---- draw-time attribute fetch helpers (defined in draw.cpp) ------------

// M8 diagnostics: GL-layer draw counters, read by the vk backend's periodic
// diag (swapchain.cpp) to bisect a dark screen -- a flat g_gl_draw_calls with
// frames still submitting points at the GL fetch layer dropping every draw.
extern uint64_t g_gl_draw_calls;   // DrawCommon accepted a draw
extern uint64_t g_gl_fetch_fail;   // FetchAttribRowBits failed -> draw dropped
uint64_t GetGlDrawCalls();         // extern "C" mithril_gl_draw_calls()
uint64_t GetGlFetchFail();         // extern "C" mithril_gl_fetch_fail()

// IEEE 754 half (binary16) -> float.
float HalfToFloat(uint16_t h);

// Byte size of one component of `type` (0 for unknown types).
uint32_t AttribTypeSize(GLenum type);

// Read `count` components of `type` at `p` into float, honouring the
// normalized flag. Integer types read the raw integer value as float32
// (kept exact for |v| < 2^24, which covers MC's attribute usage).
void FetchComponents(const uint8_t* p, GLenum type, GLboolean normalized,
                     float* out, GLuint count);

// Snapshot the current GL context into the backend pipeline state (M5).
v::PipelineState BuildPipelineState();