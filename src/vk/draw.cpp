// Mithril-Wrapper Vulkan backend -- frame submission path.
// Stages the GL payloads into vertex/instance/index buffers, records
// the frame (clear + ordered draws) in one VkCommandBuffer, and reads
// the finished frame back for glReadPixels.

#include "internal.h"

#include <algorithm>
#include <cstring>

namespace mithril::vk {

// Draw path
// ---------------------------------------------------------------------------

namespace {

// MRT: whether colour attachment `i` of `f` is among the active draw buffers.
static bool OpDrawBufEnabled(const FboObj& f, size_t i) {
    for (auto b : f.draw_bufs)
        if (b == (GLenum)(GL_COLOR_ATTACHMENT0 + i)) return true;
    return false;
}

// Create a host-visible staging buffer of `size` bytes and copy `data` in.
bool StageBytes(const void* data, VkDeviceSize size, VkBufferUsageFlags usage,
                VkBuffer* buf, VkDeviceMemory* mem) {
    if (CreateHostBuffer(size, usage, buf, mem) != VK_SUCCESS) {
        ML_LOG_ERROR("vk: draw staging allocation failed");
        return false;
    }
    void* map = nullptr;
    if (g.fn.MapMemory(g.device, *mem, 0, VK_WHOLE_SIZE, 0, &map) ==
        VK_SUCCESS) {
        std::memcpy(map, data, (size_t)size);
        g.fn.UnmapMemory(g.device, *mem);
    }
    return true;
}

// Stage a float32 stream into buf/mem (no-op for an empty stream).
bool StageStream(const VertexStream& stream, VkBuffer* buf,
                 VkDeviceMemory* mem) {
    if (stream.data.empty() || stream.stride == 0) return true;
    return StageBytes(stream.data.data(), stream.data.size() * sizeof(float),
                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, buf, mem);
}

} // namespace

void Draw(const DrawParams& params) {
    // Frame slots are shared with RetireAllInflight() (worker-thread texture
    // uploads); serialise so frame_draws/UBO/descriptors never race.
    std::lock_guard<std::recursive_mutex> frame_lock(g_frame_mutex);
    if (!g.initialized) return;
    auto prog_it = g_programs.find(params.program);
    if (prog_it == g_programs.end()) return;
    const Program& prog = prog_it->second;
    if (params.vertex_stream.data.empty()) {
        ++g.stats_draws_skipped;
        return;
    }
    // Ensure the bound draw framebuffer's device resources exist (rebuilds
    // them lazily); the pass identity flows into the pipeline cache key.
    FboObj fbo;
    bool rp_ok = ResolveDrawFbo(&fbo);
    if (!rp_ok) return;
    (void)fbo;

    DrawOp op;
    op.program = params.program;
    op.topology = (uint32_t)params.topology;
    op.v_stride = params.vertex_stream.stride;
    op.v_attrs = params.vertex_stream.attrs;
    op.i_stride = params.instance_stream.stride;
    op.i_attrs = params.instance_stream.attrs;
    op.instance_count = std::max<uint32_t>(params.instance_count, 1);
    op.vertex_count =
        (uint32_t)(params.vertex_stream.data.size() * sizeof(float) /
                   op.v_stride);
    op.index_count = (uint32_t)params.indices.size();
    op.pipe = params.pipeline;
    if (g.bound_draw_fbo) {
        // resolve the FBO material in case Draw() ran before flush
        ResolveDrawFbo(&fbo);
        op.has_render_pass = true;
        op.render_pass = fbo.pass;
        op.rp_sig = fbo.sig;
        op.color_count = (uint32_t)fbo.colors.size();
        op.samples = fbo.samples;
        op.draw_mask = 0;
        for (size_t i = 0; i < fbo.draw_bufs.size() && i < 32; ++i)
            if (fbo.draw_bufs[i] != GL_NONE)
                op.draw_mask |= 1u << (fbo.draw_bufs[i] - GL_COLOR_ATTACHMENT0);
    }

    std::string base_key =
        BuildPipelineKey(params.program, op.topology, op.v_attrs, op.v_stride,
                         op.i_attrs, op.i_stride) +
        StateSignature(params.pipeline);
    op.pipeline_key = base_key + "|RP" + (op.rp_sig.empty() ? "default" : op.rp_sig);

if (!StageStream(params.vertex_stream, &op.vertex_buffer,
                     &op.vertex_mem))
        return;
    if (!op.i_attrs.empty() &&
        !StageStream(params.instance_stream, &op.instance_buffer,
                     &op.instance_mem)) {
        g.fn.DestroyBuffer(g.device, op.vertex_buffer, nullptr);
        g.fn.FreeMemory(g.device, op.vertex_mem, nullptr);
        return;
    }
    if (op.index_count &&
        !StageBytes(params.indices.data(), op.index_count * sizeof(uint32_t),
                    VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &op.index_buffer,
                    &op.index_mem)) {
        g.fn.DestroyBuffer(g.device, op.vertex_buffer, nullptr);
        g.fn.FreeMemory(g.device, op.vertex_mem, nullptr);
        if (op.instance_buffer) {
            g.fn.DestroyBuffer(g.device, op.instance_buffer, nullptr);
            g.fn.FreeMemory(g.device, op.instance_mem, nullptr);
        }
        return;
    }

    // Compose the UBO from the reflected members + current uniform values.
    VkDeviceSize range = prog.has_ubo ? prog.ubo_size : 16;
    if (g.frames[g.frame_index].ubo_next + range > kUboPoolSize) {
        ML_LOG_WARN("vk: dynamic UBO exhausted; flushing and resetting");
        ++g.stats_ubo_wrap;
        SubmitFlush(false);
    }
    FrameSlot& frame = g.frames[g.frame_index];
    // Dynamic UBO offsets must be multiples of the device's
    // minUniformBufferOffsetAlignment (g.ubo_align, 256 on MoltenVK/iOS).
    // Hard-coding 16 here passes lavapipe (16) and macOS Metal (16) but lands
    // mid-way on A11-class iOS devices, where Metal's constant-buffer offset
    // alignment is 256 -- every draw past the first in a frame then reads its
    // uniform block from the wrong offset (zeroed/garbage MVP matrices ->
    // black screen with no error logged).
    op.ubo_offset = AlignUp(frame.ubo_next, g.ubo_align);
    op.ubo_range = range;
    frame.ubo_next = op.ubo_offset + range;
    if (prog.has_ubo) {
        std::vector<uint8_t> bytes((size_t)prog.ubo_size, 0);
        for (const auto& m : prog.members) {
            auto it = params.uniforms.find(m.name);
            if (it == params.uniforms.end()) continue;
            size_t n = std::min<size_t>(m.size, it->second.size() * sizeof(float));
            std::memcpy(bytes.data() + m.offset, it->second.data(), n);
        }
        std::memcpy(frame.ubo_map + op.ubo_offset, bytes.data(), bytes.size());
    } else {
        std::memset(frame.ubo_map + op.ubo_offset, 0, 16);
    }

    // Descriptor for this draw.
    VkDescriptorSetAllocateInfo dsa{};
    dsa.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsa.descriptorPool = frame.desc_pool;
    dsa.descriptorSetCount = 1;
    dsa.pSetLayouts = &g.set_layout;
    if (g.fn.AllocateDescriptorSets(g.device, &dsa, &op.desc_set) !=
        VK_SUCCESS) {
        ML_LOG_ERROR("vk: AllocateDescriptorSets failed");
        g.fn.DestroyBuffer(g.device, op.vertex_buffer, nullptr);
        g.fn.FreeMemory(g.device, op.vertex_mem, nullptr);
        if (op.instance_buffer) {
            g.fn.DestroyBuffer(g.device, op.instance_buffer, nullptr);
            g.fn.FreeMemory(g.device, op.instance_mem, nullptr);
        }
        if (op.index_buffer) {
            g.fn.DestroyBuffer(g.device, op.index_buffer, nullptr);
            g.fn.FreeMemory(g.device, op.index_mem, nullptr);
        }
        return;
    }
    VkDescriptorBufferInfo dbi{};
    dbi.buffer = frame.ubo;
    dbi.range = op.ubo_range;
    // Reserve up front so the push_backs below never reallocate: the writes
    // carry pointers into dbi/tis (pBufferInfo/pImageInfo), and a realloc of
    // tis between iterations would leave every previously recorded
    // pImageInfo dangling. MoltenVK then reads that stale memory while
    // writing the descriptor set and crashes with a null MVKBuffer deref
    // (writeDescriptorSetCPUBufferDispatch, si_addr=0x52) on the first draw
    // that binds more than one sampler.
    std::vector<VkWriteDescriptorSet> writes;
    std::vector<VkDescriptorImageInfo> tis;
    writes.reserve(1 + params.sampler_binds.size());
    tis.reserve(params.sampler_binds.size());
    VkWriteDescriptorSet w{};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = op.desc_set;
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    w.pBufferInfo = &dbi;
    writes.push_back(w);

    // Combined image samplers: one per (binding, sampler_id, tex_id) handed
    // over by the GL layer. A bound sampler object (sampler_id != 0 with a
    // resident VkSampler) overrides the texture's own baked sampler; absent
    // one the draw falls back to tex->sampler (stage D behaviour). Unbound
    // units resolve to the 1x1 white dummy. Bindings without a resident view
    // are skipped rather than handed to MoltenVK as a null imageView.
    for (const auto& sb : params.sampler_binds) {
        TexObj* tex = GetTexObj(sb.tex_id);
        if (!tex || !tex->view) continue;
        VkSampler smp = sb.sampler_id ? GetResidentSampler(sb.sampler_id)
                                      : VK_NULL_HANDLE;
        if (smp == VK_NULL_HANDLE) smp = tex->sampler;   // fall back to texture's own
        VkDescriptorImageInfo di{};
        di.sampler = smp;
        di.imageView = tex->view;
        di.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        tis.push_back(di);
        op.tex_binds.push_back({sb.binding, di});
        VkWriteDescriptorSet ws{};
        ws.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        ws.dstSet = op.desc_set;
        ws.dstBinding = sb.binding;
        ws.descriptorCount = 1;
        ws.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        ws.pImageInfo = &tis.back();
        writes.push_back(ws);
    }
    g.fn.UpdateDescriptorSets(g.device, (uint32_t)writes.size(), writes.data(),
                              0, nullptr);

    // M6 stage D: if an occlusion capture is active, bracket this draw into
    // one occlusion slot of the current frame's pool (registers the slot
    // under the active query handle for draining at retire time).
    if (frame.occ_pool && AllocDrawOccSlot(&op.occ_slot)) op.has_occ_slot = true;

    frame.frame_draws.push_back(std::move(op));
    ++g.stats_draws_vk;
    g.frame_dirty = true;
}

// Free every per-draw staging buffer an op recorded. The buffers were only
// alive for that op's draw; after the frame's fence signals they can go.
static void DestroyOpBuffers(DrawOp& op) {
    g.fn.DestroyBuffer(g.device, op.vertex_buffer, nullptr);
    g.fn.FreeMemory(g.device, op.vertex_mem, nullptr);
    if (op.instance_buffer) {
        g.fn.DestroyBuffer(g.device, op.instance_buffer, nullptr);
        g.fn.FreeMemory(g.device, op.instance_mem, nullptr);
    }
    if (op.index_buffer) {
        g.fn.DestroyBuffer(g.device, op.index_buffer, nullptr);
        g.fn.FreeMemory(g.device, op.index_mem, nullptr);
    }
    op.vertex_buffer = VK_NULL_HANDLE;
    op.instance_buffer = VK_NULL_HANDLE;
    op.index_buffer = VK_NULL_HANDLE;
    op.vertex_mem = VK_NULL_HANDLE;
    op.instance_mem = VK_NULL_HANDLE;
    op.index_mem = VK_NULL_HANDLE;
}

// Recycle one frame slot after the GPU finished with it: wait its fence,
// free the recorded staging buffers, reset the descriptor pool + UBO cursor.
static void RetireFrame(uint32_t idx) {
    FrameSlot& fr = g.frames[idx];
    if (!fr.in_flight) return;
    g.fn.WaitForFences(g.device, 1, &fr.fence, VK_TRUE, UINT64_MAX);
    g.fn.ResetFences(g.device, 1, &fr.fence);
    // M6 stage D: drain this frame's occlusion/timestamp slot values into the
    // owning QueryObjs (safe: the fence above guarantees GPU completion).
    RetireFrameQueries(idx);
    for (auto& op : fr.frame_draws) DestroyOpBuffers(op);
    fr.frame_draws.clear();
    g.fn.ResetDescriptorPool(g.device, fr.desc_pool, 0);
    fr.ubo_next = 0;
    fr.in_flight = false;
}

// Block until every submitted-but-not-yet-recycled frame slot is done. Used
// before resource mutation (texture uploads / FBO changes) and before
// readback so the GPU can never reference memory the host is about to free.
void RetireAllInflight() {
    // Frame slots are shared with SubmitFlush/Draw on the render thread;
    // worker-thread texture uploads call this to drain frames.
    std::lock_guard<std::recursive_mutex> frame_lock(g_frame_mutex);
    if (!g.initialized) return;
    for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) RetireFrame(i);
}

void SubmitFlush(bool wait);   // defined below; CreateGLSync needs it

// M6 stage C: GL sync objects. A GLsync wraps a dedicated VkFence submitted
// with a real (empty) command buffer, which the queue orders after every
// previously submitted command batch - so the fence signals exactly when all
// GL work recorded before glFenceSync has completed. A plain empty
// VkSubmitInfo (0 command buffers) is avoided: lavapipe signals it instantly,
// but MoltenVK only advances fences when a submitted MTLCommandBuffer
// completes, so an empty-batch fence would never fire on Metal.
uint64_t CreateGLSync() {
    if (!EnsureInit()) return 0;
    // The sync must cover commands the app already recorded but has not yet
    // flushed (glFenceSync does not flush in GL, but our deferred recording
    // only reaches the GPU through SubmitFlush, so kick the pending frame).
    if (g.frame_dirty) SubmitFlush(false);

    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = g.pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (g.fn.AllocateCommandBuffers(g.device, &cbai, &cmd) != VK_SUCCESS) {
        ML_LOG_ERROR("vk: glFenceSync command buffer alloc failed");
        return 0;
    }
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (g.fn.BeginCommandBuffer(cmd, &bi) != VK_SUCCESS ||
        g.fn.EndCommandBuffer(cmd) != VK_SUCCESS) {
        g.fn.FreeCommandBuffers(g.device, g.pool, 1, &cmd);
        return 0;
    }

    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (g.fn.CreateFence(g.device, &fci, nullptr, &fence) != VK_SUCCESS) {
        g.fn.FreeCommandBuffers(g.device, g.pool, 1, &cmd);
        ML_LOG_ERROR("vk: glFenceSync CreateFence failed");
        return 0;
    }
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if (g.fn.QueueSubmit(g.queue, 1, &si, fence) != VK_SUCCESS) {
        g.fn.DestroyFence(g.device, fence, nullptr);
        g.fn.FreeCommandBuffers(g.device, g.pool, 1, &cmd);
        ML_LOG_ERROR("vk: glFenceSync QueueSubmit failed");
        return 0;
    }
    uint64_t handle = g.glsync_next++;
    g.glsyncs[handle] = {fence, cmd};
    return handle;
}

bool CheckGLSync(uint64_t sync) {
    auto it = g.glsyncs.find(sync);
    if (it == g.glsyncs.end()) return true;   // degraded => already done
    return g.fn.GetFenceStatus(g.device, it->second.fence) == VK_SUCCESS;
}

bool WaitGLSync(uint64_t sync, uint64_t timeout_ns) {
    auto it = g.glsyncs.find(sync);
    if (it == g.glsyncs.end()) return true;   // degraded => already done
    VkFence fence = it->second.fence;
    VkResult r = g.fn.WaitForFences(g.device, 1, &fence, VK_TRUE, timeout_ns);
    return r == VK_SUCCESS;
}

void DestroyGLSync(uint64_t sync) {
    auto it = g.glsyncs.find(sync);
    if (it == g.glsyncs.end()) return;
    VkFence fence = it->second.fence;
    g.fn.WaitForFences(g.device, 1, &fence, VK_TRUE, UINT64_MAX);
    g.fn.DestroyFence(g.device, fence, nullptr);
    g.fn.FreeCommandBuffers(g.device, g.pool, 1, &it->second.cmd);
    g.glsyncs.erase(it);
}

void SubmitFlush(bool wait) {
    std::lock_guard<std::recursive_mutex> frame_lock(g_frame_mutex);
    if (!g.initialized) return;

    // Nothing to submit this call: if the caller wants completion semantics
    // (glFinish) make sure any in-flight frame from a previous flush is done.
    if (!g.frame_dirty) {
        if (wait) RetireAllInflight();
        return;
    }

    FrameSlot& frame = g.frames[g.frame_index];
    uint32_t fidx = g.frame_index;
    // The slot we're about to reuse was submitted two frames ago; the GPU
    // must be fully done with it before we recycle its UBO/descriptor pool.
    RetireFrame(fidx);

    // Target resolution: default framebuffer vs bound FBO.
    uint32_t pw = g.width, ph = g.height;
    VkRenderPass rp = g.renderpass;
    VkFramebuffer fb_handle = g.target_fb;
    VkImage color_img = g.target_image;
    VkImageLayout* color_layout = &g.target_layout;
    VkImage depth_img = g.depth_image;
    VkImageLayout* depth_layout = &g.depth_layout;
    bool has_depth = true;
    FboObj* fbo = nullptr;

    if (g.bound_draw_fbo) {
        FboObj resolved;
        if (!ResolveDrawFbo(&resolved)) return;
        auto it = g.framebuffers.find(g.bound_draw_fbo);
        if (it == g.framebuffers.end()) return;
        fbo = &it->second;
        rp = fbo->pass;
        fb_handle = fbo->fb;
        pw = fbo->width;
        ph = fbo->height;
        color_img =
            fbo->color_msaa.empty() ? VK_NULL_HANDLE
                                    : FboColorImage(*fbo, 0);
        color_layout = &fbo->color_layout;
        if (fbo->has_depth) {
            depth_img = FboDepthImage(*fbo);
            depth_layout = &fbo->depth_layout;
        } else {
            depth_img = VK_NULL_HANDLE;
            has_depth = false;
        }
    }

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    g.fn.ResetCommandBuffer(frame.cmd, 0);
    g.fn.BeginCommandBuffer(frame.cmd, &bi);

    // M6 stage D: reset this frame's query pools at the start of recording.
    // A slot is only re-recorded after retire, so this reset is always ordered
    // after the last submission that used the pool (fresh pools get their
    // first reset here too).
    if (frame.occ_pool != VK_NULL_HANDLE)
        g.fn.CmdResetQueryPool(frame.cmd, frame.occ_pool, 0,
                               kOcclusionSlotsPerFrame);
    if (frame.ts_pool != VK_NULL_HANDLE)
        g.fn.CmdResetQueryPool(frame.cmd, frame.ts_pool, 0,
                               kTimestampSlotsPerFrame);

    // Optional explicit clear of the current target. MRT: one clear per
    // colour attachment (only those selected by the draw buffers).
    VkImageSubresourceRange color_range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (g.pending_clear) {
        if (g.clear_mask & GL_COLOR_BUFFER_BIT) {
            VkClearColorValue c{};
            c.float32[0] = g.clear_r;
            c.float32[1] = g.clear_g;
            c.float32[2] = g.clear_b;
            c.float32[3] = g.clear_a;
            // Record the new layout as TRANSFER_DST_OPTIMAL so the restore
            // transition below (back to COLOR_ATTACHMENT_OPTIMAL) actually
            // fires -- the depth clear path does the same bookkeeping.
            auto clear_img = [&](VkImage img, VkImageLayout* lay) {
                if (img == VK_NULL_HANDLE) return;
                TransitionLayout(frame.cmd, img, *lay,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                g.fn.CmdClearColorImage(frame.cmd, img,
                                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                        &c, 1, &color_range);
                *lay = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            };
            if (!fbo) {
                if (g.clear_attachment <= 0)
                    clear_img(color_img, color_layout);
            } else {
                for (size_t i = 0; i < fbo->colors.size(); ++i) {
                    if (!(OpDrawBufEnabled(*fbo, i))) continue;
                    // glClearBufferfv(GL_COLOR, i, ..) targets one attachment.
                    if (g.clear_attachment >= 0 &&
                        (int)i != g.clear_attachment)
                        continue;
                    clear_img(FboColorImage(*fbo, (int)i), &fbo->color_layout);
                    if (fbo->color_msaa[i]) {
                        VkImageLayout tmp =
                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                        clear_img(FboResolveImage(*fbo, (int)i), &tmp);
                    }
                }
            }
        }
    }
    if (g.pending_clear && has_depth &&
        (g.clear_mask & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT))) {
        VkImageAspectFlags aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
        if (g.clear_mask & GL_STENCIL_BUFFER_BIT)
            aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
        if (*depth_layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
            TransitionLayoutAspect(frame.cmd, depth_img, {aspect, 0, 1, 0, 1},
                                   *depth_layout,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            *depth_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        }
        VkClearDepthStencilValue c{};
        c.depth = (float)g.clear_depth;
        c.stencil = (uint32_t)g.clear_stencil;
        VkImageSubresourceRange depth_range{aspect, 0, 1, 0, 1};
        g.fn.CmdClearDepthStencilImage(frame.cmd, depth_img,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                       &c, 1, &depth_range);
    }
    if (*color_layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
        TransitionLayout(frame.cmd, color_img, *color_layout,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        *color_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    // Bring the FBO colour resolve targets back to color-attachment optimal
    // (the resolve blit in the render pass expects that layout).
    for (size_t i = 0; i < (fbo ? fbo->resolve_view.size() : 0); ++i)
        if (fbo->color_msaa[i] &&
            fbo->resolve_view[i] &&
            FboResolveImage(*fbo, (int)i) != VK_NULL_HANDLE) {
            auto rim = FboResolveImage(*fbo, (int)i);
            TransitionLayout(frame.cmd, rim, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        }
    if (has_depth &&
        *depth_layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
        TransitionLayoutAspect(frame.cmd, depth_img,
                               {VK_IMAGE_ASPECT_DEPTH_BIT |
                                    VK_IMAGE_ASPECT_STENCIL_BIT,
                                0, 1, 0, 1},
                               *depth_layout,
                               VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        *depth_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }

    if (!frame.frame_draws.empty()) {
        VkRenderPassBeginInfo rbi{};
        rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rbi.renderPass = rp;
        rbi.framebuffer = fb_handle;
        rbi.renderArea = {{0, 0}, {pw, ph}};
        g.fn.CmdBeginRenderPass(frame.cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);

        // GL viewport origin is bottom-left; Vulkan's is top-left, and GL NDC
        // has +Y up while Vulkan's is +Y down. A negative viewport height
        // (core Vulkan 1.1, VK_KHR_maintenance1) flips the rasterisation Y so
        // content renders upright (gui y=0 at the top) -- the rect still
        // covers the GL viewport's Vulkan rows [ph-(gl_y+gl_h), ph-gl_y].
        // (The scissor path below does the same ph - y - h flip.)
        // Clamp the GL rect into the target first: viewports that extend past
        // the framebuffer (e.g. the untouched default 512x512 viewport against
        // a smaller FBO) would otherwise produce a negative Vulkan Y and push
        // the whole draw off-screen.
        VkViewport vp{};
        const float gl_w = std::min<float>(g.vp_w, pw);
        const float gl_h = std::min<float>(g.vp_h, ph);
        const float gl_x = std::max<float>(g.vp_x, 0.f);
        const float gl_y = std::max<float>(g.vp_y, 0.f);
        vp.x = gl_x;
        vp.y = ph - gl_y;
        vp.width = gl_w;
        vp.height = -gl_h;
        vp.minDepth = 0.f;
        vp.maxDepth = 1.f;
        g.fn.CmdSetViewport(frame.cmd, 0, 1, &vp);

        // Per-draw scissor: GL_SCISSOR_TEST gates a per-draw rectangle
        // (dynamic state), otherwise the full target.
        // M6 stage D: timestamp GL calls fired between draw records interleave
        // at their draw-slot position, and each draw inside an occlusion
        // capture is bracketed by CmdBeginQuery/CmdEndQuery (one slot).
        size_t tw = 0;
        auto emit_ts = [&](const FrameSlot::TsWrite& t) {
            if (frame.ts_pool && t.slot < kTimestampSlotsPerFrame)
                g.fn.CmdWriteTimestamp(frame.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                       frame.ts_pool, t.slot);
        };
        for (size_t di = 0; di < frame.frame_draws.size(); ++di) {
            const DrawOp& op = frame.frame_draws[di];
            while (tw < frame.ts_writes.size() &&
                   frame.ts_writes[tw].pos <= di) {
                emit_ts(frame.ts_writes[tw]);
                ++tw;
            }
            VkPipeline pipe =
                GetOrCreatePipeline(g_programs.at(op.program), op);
            if (pipe == VK_NULL_HANDLE) continue;
            g.fn.CmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);

            // Dynamic scissor for this draw (GL_SCISSOR_TEST).
            VkRect2D sc;
            if (op.pipe.scissor_test && g.sc_w > 0 && g.sc_h > 0) {
                // GL scissor has a bottom-left origin; Vulkan is top-left, so
                // flip Y and clamp the rectangle to the target.
                int32_t sx = std::clamp<int32_t>((int32_t)g.sc_x, 0,
                                                 (int32_t)pw);
                int32_t sy = std::clamp<int32_t>((int32_t)ph - ((int32_t)g.sc_y + (int32_t)g.sc_h), 0,
                                                 (int32_t)ph);
                uint32_t sw = std::min<uint32_t>((uint32_t)g.sc_w,
                                                 pw - (uint32_t)sx);
                uint32_t sh = std::min<uint32_t>((uint32_t)g.sc_h,
                                                 ph - (uint32_t)sy);
                sc.offset = {sx, sy};
                sc.extent = {sw, sh};
            } else {
                sc.offset = {0, 0};
                sc.extent = {pw, ph};
            }
            g.fn.CmdSetScissor(frame.cmd, 0, 1, &sc);

            const VkBuffer binds[2] = {op.vertex_buffer, op.instance_buffer};
            const VkDeviceSize zeros[2] = {0, 0};
            uint32_t nb = op.instance_buffer ? 2 : 1;
            g.fn.CmdBindVertexBuffers(frame.cmd, 0, nb, binds, zeros);

            if (op.index_count) {
                g.fn.CmdBindIndexBuffer(frame.cmd, op.index_buffer, 0,
                                        VK_INDEX_TYPE_UINT32);
            }
            uint32_t dyn = (uint32_t)op.ubo_offset;
            g.fn.CmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                       g.pipeline_layout, 0, 1, &op.desc_set,
                                       1, &dyn);
            if (op.has_occ_slot && frame.occ_pool)
                g.fn.CmdBeginQuery(frame.cmd, frame.occ_pool, op.occ_slot, 0);
            if (op.index_count) {
                g.fn.CmdDrawIndexed(frame.cmd, op.index_count, op.instance_count,
                                    0, 0, 0);
            } else {
                g.fn.CmdDraw(frame.cmd, op.vertex_count, op.instance_count, 0, 0);
            }
            if (op.has_occ_slot && frame.occ_pool)
                g.fn.CmdEndQuery(frame.cmd, frame.occ_pool, op.occ_slot);
        }
        // Timestamp writes recorded after the last draw fire at the tail of
        // the pass.
        while (tw < frame.ts_writes.size()) {
            emit_ts(frame.ts_writes[tw]);
            ++tw;
        }
        g.fn.CmdEndRenderPass(frame.cmd);
    }

    // Copy the finished read-buffer colour image to the readback buffer.
    VkImage read_img = g.target_image;
    VkImageLayout read_layout = g.target_layout;
    uint32_t rw = g.width, rh = g.height;
    FboObj* read_fbo = nullptr;
    bool have_read = false;
    int ridx = 0;
    if (g.bound_read_fbo) {
        auto rit = g.framebuffers.find(g.bound_read_fbo);
        if (rit != g.framebuffers.end()) {
            read_fbo = &rit->second;
            ridx = (int)(read_fbo->read_buf - GL_COLOR_ATTACHMENT0);
            if (ridx < 0 || ridx >= (int)read_fbo->colors.size()) ridx = 0;
            rw = rit->second.width;
            rh = rit->second.height;
            have_read = true;
        }
    } else if (fbo) {
        // No separate read binding: read back the draw target.
        read_fbo = fbo;
        ridx = (int)(read_fbo->read_buf - GL_COLOR_ATTACHMENT0);
        if (ridx < 0 || ridx >= (int)read_fbo->colors.size()) ridx = 0;
        rw = fbo->width;
        rh = fbo->height;
        have_read = true;
    }
    if (have_read) {
        if (read_fbo->color_msaa[ridx]) {
            read_img = FboResolveImage(*read_fbo, ridx);
            read_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        } else {
            read_img = FboColorImage(*read_fbo, ridx);
            read_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
    }

    if (read_img != VK_NULL_HANDLE &&
        read_layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        TransitionLayout(frame.cmd, read_img, read_layout,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        read_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    }

    // (Re)size the readback buffer to the read target.
    if (g.read_w != rw || g.read_h != rh) {
        // The previous readback may still be the target of an in-flight
        // CmdCopyImageToBuffer (async-flushed frames). Destroying it now would
        // hand the GPU a freed buffer -> MoltenVK Invalid Resource -> device
        // lost. This is exactly the retarget case: the first frame renders
        // 512x512, the swapchain then retargets to 1827x844, and the next
        // SubmitFlush hits a size change while frame 1 is still on the queue.
        RetireAllInflight();
        if (g.readback) {
            g.fn.UnmapMemory(g.device, g.readback_mem);
            g.fn.DestroyBuffer(g.device, g.readback, nullptr);
            g.fn.FreeMemory(g.device, g.readback_mem, nullptr);
            g.readback = VK_NULL_HANDLE;
            g.readback_mem = VK_NULL_HANDLE;
            g.readback_map = nullptr;
        }
        if (CreateHostBuffer((VkDeviceSize)rw * rh * 4,
                             VK_BUFFER_USAGE_TRANSFER_DST_BIT, &g.readback,
                             &g.readback_mem) == VK_SUCCESS &&
            g.fn.MapMemory(g.device, g.readback_mem, 0, VK_WHOLE_SIZE, 0,
                           reinterpret_cast<void**>(&g.readback_map)) == VK_SUCCESS) {
            g.read_w = rw;
            g.read_h = rh;
        }
    }

    if (read_img != VK_NULL_HANDLE && g.readback_map && rw && rh) {
        VkBufferImageCopy bic{};
        bic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bic.imageExtent = {rw, rh, 1};
        g.fn.CmdCopyImageToBuffer(frame.cmd, read_img,
                                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.readback,
                                  1, &bic);
    }

    // Restore the read-image layout. MSAA resolve images are only ever
    // transfer-src (read back), so nothing to do; FBO textures may also be
    // sampled next, so leave them shader-read-optimal.
    if (read_img != VK_NULL_HANDLE) {
        if (read_fbo && !read_fbo->color_msaa[ridx]) {
            TransitionLayout(frame.cmd, read_img, read_layout,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            read_fbo->color_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        } else if (!read_fbo && read_layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
            TransitionLayout(frame.cmd, read_img, read_layout,
                             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            g.target_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
    }

    g.fn.EndCommandBuffer(frame.cmd);

    // Capture a one-shot introspection of this batch for Present()'s diag.
    {
        auto& d = g.last_diag;
        d.frame_ops = (uint32_t)frame.frame_draws.size();
        d.pipe_miss_delta = g.stats_pipe_miss - g.last_pipe_miss;
        g.last_pipe_miss = g.stats_pipe_miss;
        d.clear_applied = g.pending_clear;
        d.clear[0] = g.clear_r;
        d.clear[1] = g.clear_g;
        d.clear[2] = g.clear_b;
        d.clear[3] = g.clear_a;
        d.clear_mask = (uint32_t)g.clear_mask;
        size_t n = frame.frame_draws.size() < 4 ? frame.frame_draws.size() : 4;
        d.nops = (uint32_t)n;
        for (size_t i = 0; i < n; ++i) {
            const DrawOp& op = frame.frame_draws[i];
            Engine::DiagOp& o = d.ops[i];
            o.program = op.program;
            o.ubo_offset = (uint32_t)op.ubo_offset;
            o.ubo_range = (uint32_t)op.ubo_range;
            const uint8_t* p = frame.ubo_map + op.ubo_offset;
            uint64_t v = 0;
            for (int b = 0; b < 8; ++b) v |= (uint64_t)p[b] << (8 * b);
            o.ubo8 = v;
            o.vertex_count = op.vertex_count;
            o.index_count = op.index_count;
            o.instance_count = op.instance_count;
            o.topology = op.topology;
            o.tex_count = (uint32_t)op.tex_binds.size();
            o.tex0_has_view = !op.tex_binds.empty() &&
                              op.tex_binds[0].second.imageView != VK_NULL_HANDLE
                                  ? 1
                                  : 0;
            o.vp[0] = g.vp_x;
            o.vp[1] = g.vp_y;
            o.vp[2] = g.vp_w;
            o.vp[3] = g.vp_h;
            o.sc[0] = g.sc_x;
            o.sc[1] = g.sc_y;
            o.sc[2] = g.sc_w;
            o.sc[3] = g.sc_h;
        }
    }

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &frame.cmd;
    g.last_frame_ops = (uint32_t)frame.frame_draws.size();
    if (g.fn.QueueSubmit(g.queue, 1, &si, frame.fence) != VK_SUCCESS) {
        ML_LOG_ERROR("vk: QueueSubmit failed");
        return;
    }

    // Advance to the other slot so the next frame can be recorded while this
    // one executes. The slot we leave is retired on its next reuse.
    g.frame_index = (g.frame_index + 1) % kMaxFramesInFlight;
    g.pending_clear = false;
    g.frame_dirty = false;
    frame.in_flight = true;

    // glFinish semantics: block until this frame (and any previous one)
    // completed and the readback is populated.
    if (wait) RetireAllInflight();
    ML_LOG_DEBUG("vk: frame submitted to %s target (%ux%u)%s",
                 fbo ? "FBO" : "default", pw, ph,
                 wait ? " (sync)" : " (async)");
}

void RefreshReadback() {
    // GL_READ_BUFFER / the read target changed after a rendered frame:
    // arrange for the next SubmitFlush to re-record the readback copy.
    if (!g.initialized) return;
    g.frame_dirty = true;
}

void ReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, void* out) {
    std::lock_guard<std::recursive_mutex> frame_lock(g_frame_mutex);
    if (!g.initialized || !g.readback_map) return;
    // The readback must reflect the latest submitted frame. Force the pending
    // frame through synchronously so the buffer below is guaranteed fresh.
    if (g.frame_dirty) SubmitFlush(true);
    RetireAllInflight();
    x = std::max<GLint>(0, x);
    y = std::max<GLint>(0, y);
    width = std::min<GLsizei>(width, (GLsizei)g.read_w - x);
    height = std::min<GLsizei>(height, (GLsizei)g.read_h - y);
    if (width <= 0 || height <= 0) return;
    // Rows are copied upside-down to match GL's bottom-left framebuffer
    // origin (the Vulkan framebuffer has +Y down).
    // When the default target was retargeted to a BGRA format (swapchain
    // unification on MoltenVK surfaces), the readback bytes are B,G,R,A:
    // swap R/B so glReadPixels keeps GL_RGBA semantics.
    const bool bgra = g.format == VK_FORMAT_B8G8R8A8_UNORM ||
                      g.format == VK_FORMAT_B8G8R8A8_SRGB;
    for (GLsizei row = 0; row < height; ++row) {
        uint8_t* dst = reinterpret_cast<uint8_t*>(out) + (size_t)row * width * 4;
        const uint8_t* src =
            g.readback_map +
            ((size_t)(g.read_h - 1 - (y + row)) * g.read_w + x) * 4;
        if (bgra) {
            for (GLsizei i = 0; i < width; ++i) {
                dst[i * 4 + 0] = src[i * 4 + 2];  // R
                dst[i * 4 + 1] = src[i * 4 + 1];  // G
                dst[i * 4 + 2] = src[i * 4 + 0];  // B
                dst[i * 4 + 3] = src[i * 4 + 3];  // A
            }
        } else {
            std::memcpy(dst, src, (size_t)width * 4);
        }
    }
}
} // namespace mithril::vk
