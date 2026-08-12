// Mithril-Wrapper Vulkan backend (milestone M2-VK, M3-VK).
//
// Owns the Vulkan device (loaded through dlopen so the dylib never exports
// vk* symbols), an offscreen render target, and the pipeline/descriptor
// machinery the GL layer feeds through src/gl/. The offscreen target is the
// seam where a swapchain (CAMetalLayer on iOS) lands later.
//
// The GL layer resolves VAO/VBO/vertex-attrib state into interleaved CPU
// payloads, so the engine stays free of GL object state:
//  - one per-vertex stream (binding 0),
//  - one optional per-instance stream (binding 1),
//  - one optional UINT32 index stream (everything else is expanded on CPU,
//    including UNSIGNED_BYTE/SHORT indices and baseVertex/baseInstance).

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <GL/glcorearb.h>

namespace mithril::vk {

// GL 3.3 core guarantees 16 fragment texture units; the engine mirrors that.
constexpr uint32_t kMaxUnits = 16;

// GL primitive modes we translate to Vulkan topology (GL_TRIANGLES/STRIP/FAN).
enum class Topology {
    Triangles = 0,
    TriangleStrip = 1,
    TriangleFan = 2,
};

// One enabled vertex location inside a stream.
struct VertexAttr {
    uint32_t location = 0;    // GL attribute location
    uint32_t components = 0;  // 1..4 (staged as 4-byte units)
    uint32_t offset = 0;      // byte offset within one interleaved record
    // Shader input kind for this location: 0 = float, 1 = int, 2 = uint
    // (mirrors sh::Program::attrib_kinds). The pipeline's vertex-input format
    // must match it -- MoltenVK refuses to compile a pipeline whose
    // MTLAttributeFormat is FloatN while the MSL vertex input is intN/uintN.
    uint8_t kind = 0;
};

// Interleaved 4-byte-unit payload; every component is already converted on the
// CPU side (normalization, half/double->float, or kept as an int32/uint32 bit
// pattern when the shader input is integral), so the engine only sees
// R32G32B32A32-style formats whose numeric kind matches `kind`.
struct VertexStream {
    std::vector<float> data;       // empty => no stream
    uint32_t stride = 0;           // bytes per record
    std::vector<VertexAttr> attrs; // sorted by offset, unique locations
};

// Per-draw pipeline-affecting state derived from the GL context (M5).
// The GL layer maps its enums straight through; the backend encodes them
// into the pipeline cache key and the Vk*StateCreateInfo structs. Values
// are GL-sourced so the GL side can forward without a second mapping table.
struct PipelineState {
    // scissor test (dynamic rect; enablement is static)
    bool scissor_test = false;
    // depth test / depth buffer writes
    bool depth_test = false;
    GLenum depth_func = GL_LESS;
    GLboolean depth_write = GL_TRUE;
    // stencil test
    bool stencil_test = false;
    GLenum stencil_front_func = GL_ALWAYS;
    GLenum stencil_back_func = GL_ALWAYS;
    GLint stencil_front_ref = 0;
    GLint stencil_back_ref = 0;
    GLuint stencil_front_read_mask = 0xFFFFFFFFu;
    GLuint stencil_back_read_mask = 0xFFFFFFFFu;
    GLuint stencil_front_write_mask = 0xFFFFFFFFu;
    GLuint stencil_back_write_mask = 0xFFFFFFFFu;
    GLenum stencil_front_op_fail = GL_KEEP;
    GLenum stencil_front_op_zfail = GL_KEEP;
    GLenum stencil_front_op_zpass = GL_KEEP;
    GLenum stencil_back_op_fail = GL_KEEP;
    GLenum stencil_back_op_zfail = GL_KEEP;
    GLenum stencil_back_op_zpass = GL_KEEP;
    // blend
    bool blend_enable = false;
    GLenum blend_src_rgb = GL_ONE, blend_dst_rgb = GL_ZERO;
    GLenum blend_src_alpha = GL_ONE, blend_dst_alpha = GL_ZERO;
    GLenum blend_eq_rgb = GL_FUNC_ADD, blend_eq_alpha = GL_FUNC_ADD;
    float blend_color[4] = {0, 0, 0, 0};
    // cull / polygon mode
    bool cull_test = false;
    GLenum cull_face = GL_BACK;
    GLenum front_face = GL_CCW;
    GLenum polygon_mode = GL_FILL;
    float poly_offset_factor = 0.f, poly_offset_units = 0.f;
    // per-channel color write mask
    GLboolean color_wmask_r = GL_TRUE, color_wmask_g = GL_TRUE;
    GLboolean color_wmask_b = GL_TRUE, color_wmask_a = GL_TRUE;
    // primitive restart (GL_PRIMITIVE_RESTART cap). The GL layer rewrites the
    // restart index to the fixed UINT32 restart value 0xFFFFFFFF, which the
    // pipeline's input-assembly restart uses.
    bool primitive_restart = false;
    // provoking vertex convention (glProvokingVertex); maps to the
    // VK_EXT_provoking_vertex mode when the extension is live.
    GLenum provoking_vertex = GL_LAST_VERTEX_CONVENTION;
};

// One sampler-uniform assignment for a draw: Vulkan descriptor binding,
// the GL sampler object bound at that unit (0 = none, fall back to the
// texture's own baked VkSampler), and the GL texture bound at that unit.
struct SamplerBind {
    uint32_t binding = 0;
    uint64_t sampler_id = 0;   // GL sampler object id (0 = use texture's sampler)
    uint64_t tex_id = 0;       // GL texture id (0 = unbound -> 1x1 white dummy)
};

// Everything one GL draw call needs. `uniforms` maps mithril_GlobalBlock
// member name -> flat float values. `sampler_binds` holds the active
// sampler assignments for this program: Vulkan descriptor binding ->
// (sampler object id, texture id). sampler_id == 0 falls back to the
// texture's own baked VkSampler; tex_id == 0 resolves to the 1x1 white
// dummy.
struct DrawParams {
    uint64_t program = 0;
    VertexStream vertex_stream;                    // binding 0 (per-vertex)
    VertexStream instance_stream;                  // binding 1 (per-instance)
    std::vector<uint32_t> indices;                 // empty => non-indexed
    uint32_t instance_count = 1;
    Topology topology = Topology::Triangles;
    std::unordered_map<std::string, std::vector<float>> uniforms;
    std::vector<SamplerBind> sampler_binds;
    PipelineState pipeline;                        // M5 depth/blend/cull/... state
};

// GL texture mip chain ready for upload (M4). Each level is R8G8B8A8
// row-major, bottom-up (GL convention); mip[0] is the base level.
// Layered textures (3D / 2D array / cubemap) concatenate every "slice" of
// the level into one buffer: 3D stores z in order, arrays store the layers
// in order, cubemaps store the six faces in VkCubeMapFace order
// (POSITIVE_X, NEGATIVE_X, POSITIVE_Y, NEGATIVE_Y, POSITIVE_Z, NEGATIVE_Z).
// Each slice is `width*height*4` bytes; slice 0 starts at buffer offset 0.
struct TexUpload {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t depth = 1;    // 3D thickness / array layer count (1 for 2D/cube)
    bool is_3d = false;    // image type 3D (slices are the z axis)
    bool is_cube = false;  // six face slices, cube-compatible 2D array
    std::vector<std::vector<uint8_t>> mip;   // mip[level] pixels
};

// Upload (or replace) the image of texture `gl_id` from CPU pixels. The
// engine keeps the image resident until the next upload or teardown; sampler
// state (wrap/filter/mip mode) comes with it.
enum class TexFilter { Nearest = 0, Linear = 1 };
struct TexSamplerInfo {
    TexFilter mag = TexFilter::Linear;
    TexFilter min = TexFilter::Linear;
    bool mip = false;              // use mipmapped min filter
    GLenum wrap_s = GL_REPEAT, wrap_t = GL_REPEAT, wrap_r = GL_REPEAT;
};
void UploadTexture(uint64_t gl_id, const TexUpload& img,
                   const TexSamplerInfo& sampler);

// Drop the resident GPU image of `gl_id` (glDeleteTextures path). The next
// UploadTexture rebuilds it from scratch.
void DestroyResidentTexture(uint64_t gl_id);

// M6 stage E: GL sampler objects (glGenSamplers / glBindSampler). A sampler
// object owns its own VkSampler, decoupled from any texture; the draw path
// pairs it with the bound texture's image view. UpdateSampler (re)creates the
// resident VkSampler from GL sampler state; DestroyResidentSampler frees it.
void UpdateSampler(uint64_t gl_id, const TexSamplerInfo& sampler);
void DestroyResidentSampler(uint64_t gl_id);

// Lazily create loader + instance + device + offscreen target. Idempotent.
bool EnsureInit();
bool IsInitialized();

// Recreate the offscreen color target (usually once at startup).
bool SetTargetSize(uint32_t w, uint32_t h);
uint32_t TargetWidth();
uint32_t TargetHeight();

// Colour used by the next render pass clear (glClearColor/glClear).
void SetClearColor(float r, float g, float b, float a);
// GLbitfield from glClear: decide which buffer(s) get cleared next flush.
void SetClearMask(GLbitfield mask);
// glClearBuffer*: restrict the pending clear to one color attachment
// (-1 = all enabled draw buffers).
void SetClearAttachment(int index);
// Clear values for GL_DEPTH_BUFFER_BIT / GL_STENCIL_BUFFER_BIT.
void SetClearDepth(double depth);
void SetClearStencil(GLint value);
// Viewport in target coordinates (glViewport), clamped to the target.
void SetViewport(float x, float y, float w, float h);
// Scissor rect for the next frame (GL_SCISSOR_TEST).
void SetScissor(float x, float y, float w, float h);

// Feed SPIR-V for a linked program; returns a stable handle.
uint64_t CreateProgram(const std::vector<uint32_t>& vs,
                       const std::vector<uint32_t>& fs);
void DestroyProgram(uint64_t program);

// Record a draw into the pending frame (submitted by SubmitFlush).
void Draw(const DrawParams& params);

// Execute the pending frame (clear + draws). With `wait` (glFinish) the host
// blocks until the GPU finishes and the readback is ready; without it
// (glFlush) the frame is kicked and the engine advances to the other frame
// slot so recording can continue while the GPU is still busy.
void SubmitFlush(bool wait);

// Block until every frame slot the GPU may still be working on has finished,
// recycling their descriptors/UBO/staging. Called before resource mutation
// (texture uploads, FBO changes) so in-flight frames never reference freed
// memory, and before readback so results reflect the latest submission.
void RetireAllInflight();

// GL sync object (glFenceSync family, S6). A GLsync wraps one dedicated
// VkFence that fires after every command recorded before the sync is created.
// Pending (unsubmitted) frames are flushed first so the fence reflects them.
// Returns 0 when the backend is unavailable => GL layer degrades to
// always-signaled (MobileGL mode).
uint64_t CreateGLSync();
// True once the fence the sync owns has signaled (no flush, pure status read).
bool CheckGLSync(uint64_t sync);
// Block up to `timeout_ns` for the sync (UINT64_MAX waits forever).
// Returns true when signaled in time.
bool WaitGLSync(uint64_t sync, uint64_t timeout_ns);
// Wait for completion, then release the fence (never free a GPU-in-use fence).
void DestroyGLSync(uint64_t sync);

// Copy a finished frame region (RGBA8, GL-style bottom-up origin) into `out`.
void ReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, void* out);

// ---------------------------------------------------------------------------
// M6 stage B: swapchain / present (CAMetalLayer, Apple builds).
// ---------------------------------------------------------------------------

// Hand the native window (a CAMetalLayer on Apple) supplied through
// eglCreateWindowSurface to the engine. Non-layers and non-Apple builds stay
// offscreen. Lazy: the swapchain is created on the first Present().
void SetNativeLayer(void* layer);

// vsync on/off (eglSwapInterval >0/==0) for the next swapchain / present mode.
void SetVsync(bool enable);

// Present the last offscreen frame: acquire a swapchain image, blit the
// finished frame into it, queue the present. False when no swapchain (Linux).
bool Present();

// Bring up the backend + swapchain eagerly so surface queries (EGL_WIDTH /
// EGL_HEIGHT) return the real presentation size instead of the 512x512
// default target. Minecraft queries the window size at boot and lays out its
// viewport/framebuffers from it; without this the first-present swapchain
// (e.g. 1827x844 on an iPhone X) appears only after the game sized itself
// 512x512, leaving everything in the top-left corner.
bool EnsurePresentReady();

// Presented surface size (offscreen target size when no swapchain).
uint32_t PresentWidth();
uint32_t PresentHeight();

// True once a live swapchain exists (Metal builds after the first Present()).
// Lets callers/stubs distinguish real presentation from offscreen fallback.
bool HasSwapchain();

// ---------------------------------------------------------------------------
// S5: FBO / renderbuffer support.
// ---------------------------------------------------------------------------

// One attachment reference for a GL framebuffer: a texture image (tex_id +
// mip level / array layer) or a renderbuffer (rbo_id).
struct FboAttach {
    bool is_texture = false;
    uint64_t tex_id = 0;
    uint32_t level = 0;
    uint32_t layer = 0;
    uint64_t rbo_id = 0;
};

// Complete attachment set for a GL framebuffer object. `width`/`height` are
// the resolved render-target size (from the attached textures/renderbuffers).
struct FboSpec {
    std::vector<FboAttach> color;      // one per GL_COLOR_ATTACHMENTi (MRT)
    std::vector<GLenum> draw_bufs;     // current draw buffers (GL_COLOR_ATTACHMENTn)
    GLenum read_buf = GL_COLOR_ATTACHMENT0;   // current read buffer
    bool has_depth = false;
    FboAttach depth;             // optional depth/stencil attachment
    uint32_t width = 0, height = 0;
};

// Create/replace a resident renderbuffer image (glRenderbufferStorage).
// `internalformat` is a GL internal format (rgba8, depth24_stencil8, ...).
void CreateRenderbuffer(uint64_t rbo_id, GLenum internalformat,
                        uint32_t width, uint32_t height, uint32_t samples);

// Tear down a renderbuffer image (glDeleteRenderbuffers).
void DestroyRenderbuffer(uint64_t rbo_id);

// (Re)configure a framebuffer object. Idempotent; rebuilds the Vk
// framebuffer + render pass lazily on the next use.
void SetFramebuffer(uint64_t fbo_id, const FboSpec& spec);

// Tear down a framebuffer's device resources (glDeleteFramebuffers).
void DestroyFramebuffer(uint64_t fbo_id);

// Select the draw/read framebuffer for the next frame (0 => default).
void BindDrawFramebuffer(uint64_t fbo_id);
void BindReadFramebuffer(uint64_t fbo_id);

// Size of the framebuffer bound for drawing (for viewport/scissor clamps).
uint32_t DrawTargetWidth();
uint32_t DrawTargetHeight();

// Ask the next SubmitFlush to re-record the readback (GL_READ_BUFFER /
// read-framebuffer changed without any new drawing).
void RefreshReadback();

// Copy (blit) a rect from the src draw/read framebuffer to the dst draw
// framebuffer. The GL layer passes both framebuffer ids (0 = default);
// `color` handles GL_COLOR_BUFFER_BIT, `depth_src/depth_dst` can be null for
// no-op. Called between GL flushes on its own command buffer.
void BlitFramebuffer(uint64_t src_fbo, uint64_t dst_fbo,
                     GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1,
                     GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1,
                     GLbitfield mask, GLenum filter);

// ---------------------------------------------------------------------------
// M6 stage D: query objects (glBeginQuery / glQueryCounter, S6).
// ---------------------------------------------------------------------------

// Begin an occlusion query capture (GL_SAMPLES_PASSED / ANY_SAMPLES_PASSED).
// Every draw issued until EndOcclusionQuery(sameHandle) brackets one occlu-
// sion slot; the query result is the summed sample count. Returns a handle the
// GL layer stores as its backend query; 0 degrades the query (result reads 0,
// immediately available, not cached). Sampled occlusion precision follows the
// occlusionQueryPrecise device feature.
uint64_t BeginOcclusionQuery(uint32_t target);
// Stop an occlusion capture; the query's slots stay live until the frames
// that recorded them retire (then results are drained into the query).
void EndOcclusionQuery(uint64_t handle);
// Begin/end a GL_TIME_ELAPSED interval: allocates the timestamp slots and
// records the vkCmdWriteTimestamp ops at the current draw-slot position.
// Two distinct pool slots bracket the draws in between.
uint64_t BeginTimeElapsedQuery();
void EndTimeElapsedQuery(uint64_t handle);
// One GL_TIMESTAMP sample (glQueryCounter): a single timestamp slot.
uint64_t QueryCounterTimestamp();
// True once the query's data has fully landed: every occlusion slot retired /
// timestamp written, or the query is degraded (unavailable).
bool IsQueryResultAvailable(uint64_t handle);
// Read the query result. `wait` blocks (submits + retires the owning frames)
// until available; without it a not-yet-ready query returns false and leaves
// *out untouched. Values are scaled to GL semantics: occlusion = sample count
// (non-zero normalised to 1 for ANY_SAMPLES_PASSED by the GL layer),
// TIME_ELAPSED/TIMESTAMP = nanoseconds.
bool GetQueryResult64(uint64_t handle, bool wait, uint64_t* out);
// Drop the query object and any slots the frames have not retired yet.
void DeleteBackendQuery(uint64_t handle);
// Timestamp-query capability (timestampComputeAndGraphics + valid bits).
bool IsTimerQuerySupported();
// Occlusion query capability (occlusion query pool creation succeeded).
bool IsOcclusionSupported();
// Precise occlusion counting (occlusionQueryPrecise device feature).
bool IsPreciseOcclusionSupported();
// Retire occlusion/timestamp results out of a given frame slot's pools.
void RetireFrameQueries(uint32_t idx);

} // namespace mithril::vk