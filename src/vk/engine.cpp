// Mithril-Wrapper Vulkan backend -- public API shell.
// Owns the engine globals (Engine g / g_programs / g_pipelines) and the
// small state accessors; the heavy lifting lives in dispatch.cpp /
// target.cpp / pipeline.cpp / draw.cpp.

#include "internal.h"

namespace mithril::vk {

// Engine globals (declared extern in internal.h).
Engine g;
std::unordered_map<uint64_t, Program> g_programs;
std::unordered_map<std::string, VkPipeline> g_pipelines;
std::mutex g_aux_mutex;
std::recursive_mutex g_frame_mutex;

bool IsInitialized() { return g.initialized; }

bool SetTargetSize(uint32_t w, uint32_t h) {
    if (!g.initialized) return false;
    if (w == g.width && h == g.height) return true;
    // The old target may still be referenced by an in-flight frame (async
    // flush); destroying it now hands the GPU freed images -> MoltenVK
    // Invalid Resource -> device lost.
    RetireAllInflight();
    g.fn.DestroyFramebuffer(g.device, g.target_fb, nullptr);
    g.fn.DestroyImageView(g.device, g.target_view, nullptr);
    g.fn.DestroyImage(g.device, g.target_image, nullptr);
    g.fn.FreeMemory(g.device, g.target_mem, nullptr);
    // Depth must be rebuilt at the new size too: CreateDepthTarget early-outs
    // when a handle already exists, and a stale 512x512 depth backing a larger
    // framebuffer is a validation violation (attachment < framebuffer extent).
    g.fn.DestroyImageView(g.device, g.depth_view, nullptr);
    g.fn.DestroyImage(g.device, g.depth_image, nullptr);
    g.fn.FreeMemory(g.device, g.depth_mem, nullptr);
    g.depth_view = VK_NULL_HANDLE;
    g.depth_image = VK_NULL_HANDLE;
    g.depth_mem = VK_NULL_HANDLE;
    g.width = w;
    g.height = h;
    // GL default viewport/scissor track the window size; they are only
    // overwritten by explicit glViewport/glScissor calls. The defaults were
    // 512x512 (the initial offscreen size), so after the first real-surface
    // resize every scissored draw (MC uses the default box without calling
    // glScissor) got clipped to a 512x512 corner and the screen stayed black.
    g.vp_x = 0;
    g.vp_y = 0;
    g.vp_w = (float)w;
    g.vp_h = (float)h;
    g.sc_x = 0;
    g.sc_y = 0;
    g.sc_w = (float)w;
    g.sc_h = (float)h;
    if (!CreateTarget()) return false;

    // The fresh images were created in UNDEFINED and never transitioned. The
    // render pass declares COLOR_ATTACHMENT_OPTIMAL / DEPTH_STENCIL_ATTACHMENT_
    // OPTIMAL as its initial layouts, so they must actually BE in those layouts
    // on the GPU before any render pass runs -- otherwise the layout mismatch
    // makes the first frame's contents undefined (observed as a solid-colour
    // screen on device). Same one-shot transition as EnsureInit /
    // RecreateTargetForFormat.
    {
        std::lock_guard<std::mutex> aux_lock(g_aux_mutex);
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (g.fn.BeginCommandBuffer(g.cmd, &bi) == VK_SUCCESS) {
            TransitionLayout(g.cmd, g.target_image, VK_IMAGE_LAYOUT_UNDEFINED,
                             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            TransitionLayoutAspect(g.cmd, g.depth_image,
                                   {VK_IMAGE_ASPECT_DEPTH_BIT |
                                        VK_IMAGE_ASPECT_STENCIL_BIT,
                                    0, 1, 0, 1},
                                   VK_IMAGE_LAYOUT_UNDEFINED,
                                   VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
            g.fn.EndCommandBuffer(g.cmd);
            VkSubmitInfo si{};
            si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            si.commandBufferCount = 1;
            si.pCommandBuffers = &g.cmd;
            g.fn.QueueSubmit(g.queue, 1, &si, g.fence);
            g.fn.WaitForFences(g.device, 1, &g.fence, VK_TRUE, UINT64_MAX);
            g.fn.ResetFences(g.device, 1, &g.fence);
        }
    }
    g.target_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    g.depth_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    // Pipelines cached against the default render pass survive a resize (the
    // attachment format is unchanged); the viewport/scissor adapt per draw.
    return true;
}

uint32_t TargetWidth() { return g.width; }
uint32_t TargetHeight() { return g.height; }

void SetClearColor(float r, float g2, float b, float a2) {
    g.clear_r = r;
    g.clear_g = g2;
    g.clear_b = b;
    g.clear_a = a2;
}

void SetClearMask(GLbitfield mask) {
    g.pending_clear = true;
    g.frame_dirty = true;
    g.clear_mask = mask;
}

void SetClearAttachment(int index) {
    g.pending_clear = true;
    g.frame_dirty = true;
    g.clear_attachment = index;
}

void SetClearDepth(double depth) { g.clear_depth = depth; }
void SetClearStencil(GLint value) { g.clear_stencil = value; }

void SetViewport(float x, float y, float w, float h) {
    g.vp_x = x;
    g.vp_y = y;
    g.vp_w = w;
    g.vp_h = h;
}

void SetScissor(float x, float y, float w, float h) {
    g.sc_x = x;
    g.sc_y = y;
    g.sc_w = w;
    g.sc_h = h;
}

void MarkSelfTestDone() {
    g.selftest_done = true;
}
} // namespace mithril::vk
