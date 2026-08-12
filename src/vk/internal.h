// Mithril-Wrapper Vulkan backend internal shared state (split across
// dispatch.cpp / target.cpp / pipeline.cpp / draw.cpp and the
// thin public engine.cpp). Holds the runtime dispatch table, the engine
// globals, and the cross-TU helper declarations.
//
// The loader is discovered at runtime through dlopen + vkGetInstanceProcAddr /
// vkGetDeviceProcAddr, so libmithril never links the loader and never leaks
// vk* symbols into the export table (macOS -exported_symbols_list keeps the
// GL/EGL surface clean; on iOS the same seam loads MoltenVK).

#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <util/log.h>

#include "engine.h"

namespace mithril::vk {

// ---------------------------------------------------------------------------
// Runtime dispatch table (never link the loader; never export vk*).
// ---------------------------------------------------------------------------

#define ML_FN(name) PFN_vk##name name = nullptr

struct FnTable {
    // instance-level
    ML_FN(CreateInstance);
    ML_FN(DestroyInstance);
    ML_FN(EnumeratePhysicalDevices);
    ML_FN(GetPhysicalDeviceProperties);
    ML_FN(GetPhysicalDeviceFeatures);
    ML_FN(GetPhysicalDeviceMemoryProperties);
    ML_FN(GetPhysicalDeviceQueueFamilyProperties);
    ML_FN(CreateDevice);
    ML_FN(DestroyDevice);
    ML_FN(EnumerateDeviceExtensionProperties);
    ML_FN(GetDeviceProcAddr);
    ML_FN(DestroySurfaceKHR);
    ML_FN(GetPhysicalDeviceSurfaceCapabilitiesKHR);
    ML_FN(GetPhysicalDeviceSurfaceFormatsKHR);
    ML_FN(GetPhysicalDeviceSurfacePresentModesKHR);
    ML_FN(GetPhysicalDeviceSurfaceSupportKHR);
#ifdef VK_USE_PLATFORM_METAL_EXT
    ML_FN(CreateMetalSurfaceEXT);
#endif
    // device-level
    ML_FN(GetDeviceQueue);
    ML_FN(DeviceWaitIdle);
    ML_FN(CreateSemaphore);
    ML_FN(DestroySemaphore);
    ML_FN(CreateCommandPool);
    ML_FN(DestroyCommandPool);
    ML_FN(AllocateCommandBuffers);
    ML_FN(FreeCommandBuffers);
    ML_FN(BeginCommandBuffer);
    ML_FN(EndCommandBuffer);
    ML_FN(ResetCommandBuffer);
    ML_FN(QueueSubmit);
    ML_FN(QueueWaitIdle);
    ML_FN(CreateFence);
    ML_FN(DestroyFence);
    ML_FN(WaitForFences);
    ML_FN(ResetFences);
    ML_FN(GetFenceStatus);
    ML_FN(CreateRenderPass);
    ML_FN(DestroyRenderPass);
    ML_FN(CreateImageView);
    ML_FN(DestroyImageView);
    ML_FN(CreateImage);
    ML_FN(DestroyImage);
    ML_FN(AllocateMemory);
    ML_FN(FreeMemory);
    ML_FN(BindImageMemory);
    ML_FN(BindBufferMemory);
    ML_FN(CreateBuffer);
    ML_FN(DestroyBuffer);
    ML_FN(GetBufferMemoryRequirements);
    ML_FN(GetImageMemoryRequirements);
    ML_FN(MapMemory);
    ML_FN(UnmapMemory);
    ML_FN(CreateFramebuffer);
    ML_FN(DestroyFramebuffer);
    ML_FN(CreateShaderModule);
    ML_FN(DestroyShaderModule);
    ML_FN(CreateDescriptorSetLayout);
    ML_FN(DestroyDescriptorSetLayout);
    ML_FN(CreateDescriptorPool);
    ML_FN(DestroyDescriptorPool);
    ML_FN(AllocateDescriptorSets);
    ML_FN(ResetDescriptorPool);
    ML_FN(UpdateDescriptorSets);
    ML_FN(CreatePipelineLayout);
    ML_FN(DestroyPipelineLayout);
    ML_FN(CreateGraphicsPipelines);
    ML_FN(DestroyPipeline);
    ML_FN(CreateSampler);
    ML_FN(DestroySampler);
    ML_FN(CreateSwapchainKHR);
    ML_FN(DestroySwapchainKHR);
    ML_FN(GetSwapchainImagesKHR);
    ML_FN(AcquireNextImageKHR);
    ML_FN(QueuePresentKHR);
    ML_FN(CmdCopyBufferToImage);
    ML_FN(CmdBindPipeline);
    ML_FN(CmdBindVertexBuffers);
    ML_FN(CmdBindIndexBuffer);
    ML_FN(CmdBindDescriptorSets);
    ML_FN(CmdDraw);
    ML_FN(CmdDrawIndexed);
    ML_FN(CmdBeginRenderPass);
    ML_FN(CmdEndRenderPass);
    ML_FN(CmdClearColorImage);
    ML_FN(CmdClearDepthStencilImage);
    ML_FN(CmdPipelineBarrier);
    ML_FN(CmdCopyImageToBuffer);
    ML_FN(CmdCopyImage);
    ML_FN(CmdBlitImage);
    ML_FN(CmdResolveImage);
    ML_FN(CmdSetViewport);
    ML_FN(CmdSetScissor);
    // M6 stage D: query objects (occlusion + timestamps).
    ML_FN(CreateQueryPool);
    ML_FN(DestroyQueryPool);
    ML_FN(CmdBeginQuery);
    ML_FN(CmdEndQuery);
    ML_FN(CmdWriteTimestamp);
    ML_FN(CmdResetQueryPool);
    ML_FN(GetQueryPoolResults);
};

#undef ML_FN

constexpr VkDeviceSize kUboPoolSize = 1u << 20;   // 1 MiB dynamic UBO pool

// Per-frame-slot descriptor budget: the engine allocates one descriptor set
// per draw, so the pool must cover the largest frame MC records between two
// flushes. 256 was exhausted by real 1.21.1 world rendering (routinely
// hundreds of draws/frame on the Render thread), after which every draw of
// the frame was dropped. 4096 keeps headroom for large render distances and
// shader packs while staying a plain memory pool on MoltenVK.
constexpr uint32_t kDescriptorPoolSets = 4096;

// One reflected mithril_GlobalBlock member (std140 offsets from SPIR-V).
struct UboMember {
    std::string name;
    VkDeviceSize offset = 0;
    VkDeviceSize size = 0;
};

struct Program {
    VkShaderModule vs_mod = VK_NULL_HANDLE;
    VkShaderModule fs_mod = VK_NULL_HANDLE;
    std::vector<UboMember> members;
    VkDeviceSize ubo_size = 0;
    bool has_ubo = false;
    // Sampler uniforms (descriptor binding mirrors the GLSL layout() we
    // inject in assign_sampler_bindings: binding = program listing).
    struct SamplerBind {
        std::string name;
        uint32_t binding = 0;   // descriptor binding (index into set 0)
    };
    std::vector<SamplerBind> samplers;
};

// Resident GPU texture (uploaded via UploadTexture).
struct TexObj {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    uint32_t levels = 1;
};

// S5: renderbuffer object (glRenderbufferStorage). Backed by a device-local
// image that the FBO attachment views reference.
struct RbObj {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t samples = 1;
};

// S5: one live render attachment for an FBO. Either a texture (tex_id, the
// resident TexObj image is referenced) or a renderbuffer (rbo_id into
// g.renderbuffers). `level`/`layer` select the mip/array slice for textures.
struct FboSlot {
    bool is_texture = false;
    uint64_t tex_id = 0;
    uint64_t rbo_id = 0;
    uint32_t level = 0;
    uint32_t layer = 0;

    FboSlot() = default;
    FboSlot(const FboAttach& a)
        : is_texture(a.is_texture), tex_id(a.tex_id), rbo_id(a.rbo_id),
          level(a.level), layer(a.layer) {}
};

// S5: a configured (GL) framebuffer object, keyed by gl id. The Vk
// framebuffer + render pass are rebuilt lazily before each draw flush so a
// texture re-upload pops a fresh image view automatically. MRT supports
// GL_COLOR_ATTACHMENT0..kMaxColorAtt (the render pass + framebuffer expose one
// colour attachment per attached colour image). MSAA: a multisampled color
// attachment gets a single-sample resolve image (colour attachment + resolve)
// so readback/blit see the resolved result.
constexpr uint32_t kMaxColorAtt = 8;
struct FboObj {
    std::vector<FboSlot> colors;         // one per color attachment slot
    std::vector<VkImageView> color_view; // views for the framebuffer
    std::vector<VkImageView> resolve_view;// single-sample resolve views (MSAA)
    std::vector<bool> color_msaa;        // per-slot multisampled flag
    bool has_depth = false;
    FboSlot depth;                 // depth/stencil attachment
    VkFormat color_fmt = VK_FORMAT_R8G8B8A8_UNORM;
    VkFormat depth_fmt = VK_FORMAT_D32_SFLOAT_S8_UINT;  // matches default depth_format
    uint32_t samples = 1;
    uint32_t width = 0, height = 0;
    VkImageView depth_view = VK_NULL_HANDLE;
    VkRenderPass pass = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE;
    std::vector<GLenum> draw_bufs;       // current draw buffer list (MRT)
    GLenum read_buf = GL_COLOR_ATTACHMENT0;
    std::string sig;               // render-pass signature (pipeline keys)
    uint64_t last_tex_gen = 0;      // cache stamp of the colour textures
    bool dirty = true;              // rebuild pass+framebuffer next use
    VkImageLayout color_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkImageLayout depth_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
};

struct DrawOp {
    uint64_t program = 0;
    VkBuffer vertex_buffer = VK_NULL_HANDLE;
    VkDeviceMemory vertex_mem = VK_NULL_HANDLE;
    VkBuffer instance_buffer = VK_NULL_HANDLE;
    VkDeviceMemory instance_mem = VK_NULL_HANDLE;
    VkBuffer index_buffer = VK_NULL_HANDLE;
    VkDeviceMemory index_mem = VK_NULL_HANDLE;
    uint32_t vertex_count = 0;
    uint32_t index_count = 0;
    uint32_t instance_count = 1;
    uint32_t topology = 0;         // Topology index
    uint32_t v_stride = 0;         // per-vertex record bytes
    uint32_t i_stride = 0;         // per-instance record bytes
    std::vector<VertexAttr> v_attrs;
    std::vector<VertexAttr> i_attrs;
    std::string pipeline_key;
    VkDescriptorSet desc_set = VK_NULL_HANDLE;
    VkDeviceSize ubo_offset = 0;
    VkDeviceSize ubo_range = 0;
    // Sampler descriptor images for this draw (one per samper bound).
    std::vector<std::pair<uint32_t, VkDescriptorImageInfo>> tex_binds;
    // M5: pipeline-affecting state captured at draw-record time.
    PipelineState pipe;
    // M6 stage D: occlusion query slot bracketing this draw
    // (VK_QUERY_TYPE_OCCLUSION in the current frame's pool). Query slots
    // behave like the pool they live in: a slot's result is valid from its
    // draw until the owning frame slot is retired, so results land in the
    // query object at retire time.
    bool has_occ_slot = false;
    uint32_t occ_slot = 0;
    // S5: render pass signature for the target this draw records into
    // (empty => default framebuffer). Included in the pipeline cache key and
    // the VkGraphicsPipelineCreateInfo.renderPass. `color_count`/`samples`
    // describe that render pass (multi-colour MRT + MSAA).
    bool has_render_pass = false;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    std::string rp_sig;
    uint32_t color_count = 1;
    uint32_t samples = 1;
    uint32_t draw_mask = 1;    // bit i set => colour attachment i receives the draw
};

// M6 stage A: double-buffered frame submission (two slots in flight while
// the host keeps recording into the next). Each slot owns everything the GPU
// may still be consuming after an async flush: the command buffer, its
// descriptor pool, its dynamic UBO region and the per-draw staging buffers.
// A slot is retired (fence wait + descriptor/UBO recycle + staging free) when
// it is reused two submissions later or when a synchronous read needs the
// result. `g.cmd`/`g.fence` remain a separate auxiliary one-shot path for
// texture uploads / blits, which are always synchronous.
// M6 stage C: GL sync objects. Each live GLsync owns one dedicated
// command buffer + fence; the empty batch is ordered after every
// previously submitted command batch so the fence signals exactly when
// all GL work recorded before glFenceSync completed.
struct GLSyncObj {
    VkFence fence = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
};

constexpr uint32_t kMaxFramesInFlight = 2;

// M6 stage D: query objects (occlusion + timestamps).
//
// Occlusion (GL_SAMPLES_PASSED / GL_ANY_SAMPLES_PASSED): each draw records
// one vkCmdBeginQuery/vkCmdEndQuery slot in the current frame's occlusion
// pool; a query wraps many slots across frames (the draw sequence between
// BeginQuery and EndQuery). The result is the sum of the slot samples, so we
// drain slot values into `acc` at retire time, once the frame fence signals.
//
// Timestamps (GL_TIME_ELAPSED via BeginQuery, GL_TIMESTAMP via QueryCounter):
// a timestamp slot in the frame's timestamp pool, written around the draws
// with vkCmdWriteTimestamp; TIME_ELAPSED is end - begin ticks scaled by the
// device's timestampPeriod. Degraded (no backend / feature) => handle 0, the
// GL layer reads a zero immediately-available result without caching.
struct QueryObj {
    uint32_t target = 0;       // GL target (for GL_QUERY_TARGET echo)
    // Occlusion slots: every (frame idx, pool slot) this query bracketed.
    std::vector<std::pair<uint16_t, uint32_t>> occ_slots;
    uint64_t acc = 0;          // summed slot samples drained at retire
    uint32_t occ_expected = 0; // slots registered
    uint32_t occ_drained = 0;  // slots drained into `acc`
    // Timestamp slots (frame idx + pool slot, -1 = none).
    int ts_begin_frame = -1, ts_begin_slot = -1;
    int ts_end_frame = -1, ts_end_slot = -1;
    int ts_single_frame = -1, ts_single_slot = -1;
    uint64_t ts_begin_ticks = 0, ts_end_ticks = 0, ts_single_ticks = 0;
    bool ts_begin_ready = false, ts_end_ready = false, ts_single_ready = false;
    bool active = false;       // BeginQuery seen, EndQuery not yet
    bool ended = false;        // EndQuery seen (occlusion) / QueryCounter done
    uint64_t result = 0;       // lazy cached final result
    bool result_cached = false;
};

constexpr uint32_t kOcclusionSlotsPerFrame = 4096;
constexpr uint32_t kTimestampSlotsPerFrame = 4096;

struct FrameSlot {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkDescriptorPool desc_pool = VK_NULL_HANDLE;
    VkBuffer ubo = VK_NULL_HANDLE;
    VkDeviceMemory ubo_mem = VK_NULL_HANDLE;
    uint8_t* ubo_map = nullptr;
    VkDeviceSize ubo_next = 0;
    bool in_flight = false;          // submitted async and not yet retired
    std::vector<DrawOp> frame_draws; // recorded draws; staging freed on retire
    // M6 stage D: per-frame query pools. Slots are reset together after the
    // frame fence signals (results drained into the owning QueryObj first).
    VkQueryPool occ_pool = VK_NULL_HANDLE;
    VkQueryPool ts_pool = VK_NULL_HANDLE;
    uint32_t occ_cursor = 0;
    uint32_t ts_cursor = 0;
    // Timestamp writes requested mid-frame (GL call happened between draws).
    // Recorded into the command buffer at their draw-slot position: each op
    // names the QueryObj handle + whether it is the TIME_ELAPSED begin write,
    // and `pos` = number of draws recorded when the GL call fired (SubmitFlush
    // interleaves the write into the draw loop at that position).
    struct TsWrite {
        uint64_t query = 0;    // QueryObj handle
        bool is_begin = false; // TIME_ELAPSED begin vs end
        uint32_t slot = 0;     // ts pool slot
        uint32_t pos = 0;      // frame_draws.size() when the write fired
    };
    std::vector<TsWrite> ts_writes;
    // Occlusion slots taken this frame: (QueryObj handle).
    std::vector<uint64_t> occ_queries;   // queries with slots in this frame
};

struct Swapchain {
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR handle = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D extent{};
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    VkSemaphore acquire_sem = VK_NULL_HANDLE;   // unused (fence path); kept for ABI
    VkFence acquire_fence = VK_NULL_HANDLE;      // acquire sync (replaces semaphore)
    std::vector<VkSemaphore> render_finished;
    uint32_t acquire_index = 0;
    bool acquire_valid = false;
};

struct Engine {
    void* loader = nullptr;
    FnTable fn{};
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queue_family = 0;

    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    uint32_t width = 512, height = 512;
    VkImage target_image = VK_NULL_HANDLE;
    VkDeviceMemory target_mem = VK_NULL_HANDLE;
    VkImageView target_view = VK_NULL_HANDLE;
    VkFramebuffer target_fb = VK_NULL_HANDLE;
    VkImageLayout target_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    // M5: depth attachment for the default framebuffer. D32_SFLOAT_S8_UINT is
    // universally supported on Apple silicon / A-series GPUs, while
    // D24_UNORM_S8_UINT is not (MoltenVK logged FORMAT_NOT_SUPPORTED on the
    // iPhone X A11 and silently substituted D32S8, leaving image/view/render
    // pass formats inconsistent).
    VkFormat depth_format = VK_FORMAT_D32_SFLOAT_S8_UINT;
    VkImage depth_image = VK_NULL_HANDLE;
    VkDeviceMemory depth_mem = VK_NULL_HANDLE;
    VkImageView depth_view = VK_NULL_HANDLE;
    VkImageLayout depth_layout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkRenderPass renderpass = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    void* native_layer = nullptr;
    Swapchain swap;
    bool vsync = true;
    // Auxiliary one-shot command buffer + fence (texture uploads, blits,
    // initial layout transitions). Always submitted and waited synchronously.
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    // M6 stage A: double-buffered frame slots (draw path).
    FrameSlot frames[kMaxFramesInFlight];
    uint32_t frame_index = 0;   // slot the next SubmitFlush records/submits

    // M6 stage C: live GLsync objects (handle -> object).
    std::unordered_map<uint64_t, GLSyncObj> glsyncs;
    uint64_t glsync_next = 1;   // handles start at 1 (0 = degraded)

    // M6 stage D: live query objects (handle -> object).
    std::unordered_map<uint64_t, QueryObj> queries;
    uint64_t query_next = 1;    // handles start at 1 (0 = degraded)

    // Timestamp support facts captured at init.
    bool have_timestamps = false;          // timestampComputeAndGraphics + valid bits
    double ts_period_ns = 1.0;             // timestampPeriod as ns per tick
    uint64_t ts_valid_mask = ~0ull;        // timestampValidBits => mask
    bool occlusion_precise = false;        // occlusionQueryPrecise feature
    bool have_occlusion = false;           // occlusion query pool created OK
    bool have_provoking_vertex = false;    // VK_EXT_provoking_vertex device ext
    // M6 stage D: currently active occlusion query handle (0 = none). Every
    // draw recorded while this is non-zero brackets one occlusion slot in the
    // current frame's pool; Draw() registers the slot under this handle.
    uint64_t active_occ = 0;

    // S5: FBO/renderbuffer tables + current draw/read framebuffer bindings.
    std::unordered_map<uint64_t, RbObj> renderbuffers;
    std::unordered_map<uint64_t, FboObj> framebuffers;
    // MSAA resolve images for multisampled colour FBO attachments; keyed by
    // the FboObj pointer (owns the single-sample resolve RbObj storage).
    std::unordered_map<const FboObj*, std::vector<RbObj>> fbo_msaa;
    uint64_t bound_draw_fbo = 0;   // 0 => default framebuffer
    uint64_t bound_read_fbo = 0;   // 0 => default framebuffer
    // Extra FBO render passes (framebuffers revive their own when formats
    // differ from the default color+depth); keyed by their signature.
    std::unordered_map<std::string, VkRenderPass> fbo_passes;

    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;

    // M4 textures: gl texture id -> resident GPU image.
    std::unordered_map<uint64_t, TexObj> textures;
    TexObj dummy_tex;             // 1x1 white fallback for unbound units

    // M6 stage E: GL sampler objects -> resident VkSampler. Sampler objects
    // own their own VkSampler decoupled from any texture; the draw path pairs
    // it with the bound texture's image view.
    std::unordered_map<uint64_t, VkSampler> samplers;

    // Dynamic UBO via vkCmdBindDescriptorSets dynamic offsets; the backing
    // buffer is per-frame-slot since an in-flight frame may still be reading
    // its region. `ubo_align` is the dynamic-binding alignment.
    VkDeviceSize ubo_align = 256;

    VkBuffer readback = VK_NULL_HANDLE;
    VkDeviceMemory readback_mem = VK_NULL_HANDLE;
    uint8_t* readback_map = nullptr;
    uint32_t read_w = 0, read_h = 0;   // dims the readback buffer holds

    bool initialized = false;
    bool frame_dirty = false;
    bool pending_clear = false;
    GLbitfield clear_mask = 0;
    float clear_r = 0, clear_g = 0, clear_b = 0, clear_a = 0;
    int clear_attachment = -1;   // glClearBuffer*: which color attachment (-1 = all)
    double clear_depth = 1.0;
    int clear_stencil = 0;
    float vp_x = 0, vp_y = 0, vp_w = 512, vp_h = 512;
    float sc_x = 0, sc_y = 0, sc_w = 512, sc_h = 512;
};

// Engine + program/pipeline caches (storage lives in engine.cpp).
extern Engine g;
extern std::unordered_map<uint64_t, Program> g_programs;
extern std::unordered_map<std::string, VkPipeline> g_pipelines;

// ---- shared helpers (defined in target.cpp) ------------------------------

VkDeviceSize AlignUp(VkDeviceSize v, VkDeviceSize a);

// M6 stage D: create the per-frame occlusion + timestamp query pools once the
// device is live (query.cpp). Uses the capability facts captured at init.
void CreateQueryPools();

// M6 stage D: allocate an occlusion slot for the draw being recorded right
// now, registering it under the active occlusion query (returns false when no
// capture is active or the frame's pool is exhausted). Query.cpp owns the
// slot accounting; draw.cpp calls it inside Draw() and stores the slot in the
// DrawOp so SubmitFlush can bracket the draw with CmdBeginQuery/CmdEndQuery.
bool AllocDrawOccSlot(uint32_t* out_slot);

VkResult FindMemoryType(uint32_t bits, VkMemoryPropertyFlags want,
                        uint32_t* out);

VkResult CreateHostBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                          VkBuffer* buf, VkDeviceMemory* mem);

VkResult CreateTargetImage(VkImage* img, VkDeviceMemory* mem);

void TransitionLayout(VkCommandBuffer cb, VkImage image,
                      VkImageLayout old_layout, VkImageLayout new_layout);

void TransitionLayoutAspect(VkCommandBuffer cb, VkImage image,
                            VkImageSubresourceRange range,
                            VkImageLayout old_layout,
                            VkImageLayout new_layout);

bool CreateRenderPass();

bool CreateTarget();

bool CreateDepthTarget();

// Rebuild the offscreen target + default render pass with a new colour format
// (swapchain format unification; see target.cpp).
bool RecreateTargetForFormat(VkFormat fmt);

void CreateDummyTexture();

// Resolve the currently bound draw framebuffer's Vk resources, rebuilding
// the framebuffer + render pass when dirty. Returns false if it can't render;
// when the default framebuffer is bound it returns true and leaves out=0.
bool ResolveDrawFbo(FboObj* out);

// Render pass for an FBO signature ("RGBA8:D24S8" -> the default pass).
VkRenderPass GetOrCreateFboPass(const std::string& sig, size_t n_color,
                                bool has_depth, uint32_t samples);

// Live image handle for an FBO colour/depth attachment (texture or
// renderbuffer resolution). Returns VK_NULL_HANDLE when unattached.
VkImage FboColorImage(const FboObj& f, int idx);
VkImage FboResolveImage(const FboObj& f, int idx);
VkImage FboDepthImage(const FboObj& f);

// ---- texture helpers (defined in texture.cpp) ----------------------------

TexObj* GetTexObj(uint64_t gl_id);

// M6 stage E: sampler object's resident VkSampler (VK_NULL_HANDLE if none).
VkSampler GetResidentSampler(uint64_t gl_id);

// ---- pipeline helpers (defined in pipeline.cpp) --------------------------

std::string BuildPipelineKey(uint64_t program, uint32_t topology,
                             const std::vector<VertexAttr>& v_attrs,
                             uint32_t v_stride,
                             const std::vector<VertexAttr>& i_attrs,
                             uint32_t i_stride);

// Textual signature of the pipeline-affecting GL state (depth/blend/cull/
// stencil/color-mask/scissor/mode). Appended to the geometry key.
std::string StateSignature(const PipelineState& ps);

VkFormat AttrFormat(uint32_t components);

VkPipeline GetOrCreatePipeline(const Program& prog, const DrawOp& op);

// Map a GL sample count (1/2/4/8/...) to the Vulkan sample-count bit.
VkSampleCountFlagBits ToVkSampleCount(uint32_t samples);

} // namespace mithril::vk