// Mithril-Wrapper Vulkan backend -- swapchain + present (Stage B).
// Wraps a CAMetalLayer (handed in through EGL eglCreateWindowSurface) in a
// VK_EXT_metal_surface + VK_KHR_swapchain. The GL render continues into the
// offscreen target exactly as before; Present() copies that finished image
// into the acquired swapchain image and queues the present, synchronising
// with an image-available semaphore on acquire and a per-image render-finished
// semaphore before present.
//
// On non-Apple builds (or without a native layer) every function degrades to
// a harmless no-op/false so the Linux lavapipe offscreen path is unchanged.

#include "internal.h"

#ifdef VK_USE_PLATFORM_METAL_EXT
#include <objc/message.h>
#include <objc/runtime.h>
#endif

namespace mithril::vk {

#ifdef VK_USE_PLATFORM_METAL_EXT

// objc runtime: confirm the opaque pointer really is a CAMetalLayer before
// handing it to VkMetalSurfaceCreateInfoEXT (eglCreateWindowSurface may be
// called with arbitrary/placeholder pointers, e.g. the contract smoke).
static bool IsCametalLayer(void* layer) {
    if (!layer) return false;
    void* cls = objc_getClass("CAMetalLayer");
    if (!cls) return false;
    typedef bool (*IsKindFn)(void*, void*, void*);
    IsKindFn iskind =
        reinterpret_cast<IsKindFn>(objc_msgSend);
    return iskind(layer, sel_registerName("isKindOfClass:"), cls);
}

static VkSurfaceKHR CreateMetalSurface(void* layer) {
    if (!g.fn.CreateMetalSurfaceEXT) return VK_NULL_HANDLE;
    VkMetalSurfaceCreateInfoEXT sci{};
    sci.sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT;
    sci.pLayer = layer;
    VkSurfaceKHR surf = VK_NULL_HANDLE;
    if (g.fn.CreateMetalSurfaceEXT(g.instance, &sci, nullptr, &surf) !=
        VK_SUCCESS)
        return VK_NULL_HANDLE;
    return surf;
}

static bool EnsureSwapchain() {
    if (g.swap.handle) return true;
    if (!g.native_layer || !g.initialized) return false;

    VkSurfaceKHR surface = CreateMetalSurface(g.native_layer);
    if (surface == VK_NULL_HANDLE) return false;
    g.swap.surface = surface;

    VkSurfaceCapabilitiesKHR caps{};
    if (g.fn.GetPhysicalDeviceSurfaceCapabilitiesKHR(g.physical, surface,
                                                     &caps) != VK_SUCCESS)
        return false;

    VkBool32 present_supported = VK_FALSE;
    if (g.fn.GetPhysicalDeviceSurfaceSupportKHR(g.physical, g.queue_family,
                                                surface, &present_supported) !=
            VK_SUCCESS ||
        !present_supported) {
        ML_LOG_ERROR("vk: queue family %u cannot present to the surface",
                     g.queue_family);
        return false;
    }

    uint32_t nfmt = 0, npm = 0;
    g.fn.GetPhysicalDeviceSurfaceFormatsKHR(g.physical, surface, &nfmt, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(nfmt);
    if (nfmt) g.fn.GetPhysicalDeviceSurfaceFormatsKHR(g.physical, surface, &nfmt,
                                                     fmts.data());
    g.fn.GetPhysicalDeviceSurfacePresentModesKHR(g.physical, surface, &npm, nullptr);
    std::vector<VkPresentModeKHR> modes(npm);
    if (npm) g.fn.GetPhysicalDeviceSurfacePresentModesKHR(g.physical, surface, &npm,
                                                          modes.data());

    VkSurfaceFormatKHR fmt{};
    if (nfmt == 0) return false;
    fmt = fmts[0];
    for (auto& f : fmts) {
        if ((f.format == VK_FORMAT_R8G8B8A8_UNORM ||
             f.format == VK_FORMAT_B8G8R8A8_UNORM) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            fmt = f;
            break;
        }
    }

    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!g.vsync) {
        for (auto m : modes)
            if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) { mode = m; break; }
    }

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0 || extent.width == 0xFFFFFFFFu) {
        extent.width = g.width;
        extent.height = g.height;
    }

    if (extent.width != g.width || extent.height != g.height)
        SetTargetSize(extent.width, extent.height);

    uint32_t count = caps.minImageCount > 2 ? caps.minImageCount : 2;
    if (caps.maxImageCount && count > caps.maxImageCount)
        count = caps.maxImageCount;

    VkSwapchainCreateInfoKHR sci{};
    sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    sci.surface = surface;
    sci.minImageCount = count;
    sci.imageFormat = fmt.format;
    sci.imageColorSpace = fmt.colorSpace;
    sci.imageExtent = extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = mode;
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = VK_NULL_HANDLE;
    if (g.fn.CreateSwapchainKHR(g.device, &sci, nullptr, &g.swap.handle) !=
        VK_SUCCESS)
        return false;
    std::vector<VkImage> images(count);
    if (g.fn.GetSwapchainImagesKHR(g.device, g.swap.handle, &count,
                                   images.data()) != VK_SUCCESS)
        return false;
    g.swap.images = std::move(images);
    g.swap.format = fmt.format;
    g.swap.color_space = fmt.colorSpace;
    g.swap.extent = extent;

    for (auto img : g.swap.images) {
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = fmt.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view = VK_NULL_HANDLE;
        if (g.fn.CreateImageView(g.device, &vi, nullptr, &view) != VK_SUCCESS)
            return false;
        g.swap.views.push_back(view);
    }

    VkSemaphoreCreateInfo semi{};
    semi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    g.fn.CreateSemaphore(g.device, &semi, nullptr, &g.swap.acquire_sem);
    g.swap.render_finished.resize(g.swap.images.size());
    for (auto& s : g.swap.render_finished)
        g.fn.CreateSemaphore(g.device, &semi, nullptr, &s);

    ML_LOG_INFO("vk: swapchain ready (%ux%u, %u images)", extent.width,
                extent.height, count);
    return true;
}

static void DestroySwapchain() {
    if (g.swap.handle) {
        g.fn.DeviceWaitIdle(g.device);
        g.fn.DestroySwapchainKHR(g.device, g.swap.handle, nullptr);
        g.swap.handle = VK_NULL_HANDLE;
    }
    for (auto v : g.swap.views) g.fn.DestroyImageView(g.device, v, nullptr);
    g.swap.views.clear();
    g.swap.images.clear();
    if (g.swap.acquire_sem) {
        g.fn.DestroySemaphore(g.device, g.swap.acquire_sem, nullptr);
        g.swap.acquire_sem = VK_NULL_HANDLE;
    }
    for (auto s : g.swap.render_finished)
        g.fn.DestroySemaphore(g.device, s, nullptr);
    g.swap.render_finished.clear();
    if (g.swap.surface) {
        g.fn.DestroySurfaceKHR(g.instance, g.swap.surface, nullptr);
        g.swap.surface = VK_NULL_HANDLE;
    }
    g.swap.acquire_valid = false;
}

#endif  // VK_USE_PLATFORM_METAL_EXT

void SetNativeLayer(void* layer) {
    g.native_layer = layer;
#ifdef VK_USE_PLATFORM_METAL_EXT
    if (layer && !IsCametalLayer(layer)) {
        g.native_layer = nullptr;
        ML_LOG_WARN("vk: native window is not a CAMetalLayer; staying offscreen");
    }
#endif
}

void SetVsync(bool enable) {
    g.vsync = enable;
}

bool Present() {
    if (!g.initialized) return false;
#ifdef VK_USE_PLATFORM_METAL_EXT
    if (!EnsureSwapchain()) return false;

    RetireAllInflight();

    // Acquire the next image. In-flight image reuse is guarded by the
    // per-image render-finished semaphore being waited at present.
    VkResult ar = g.fn.AcquireNextImageKHR(
        g.device, g.swap.handle, UINT64_MAX, g.swap.acquire_sem,
        VK_NULL_HANDLE, &g.swap.acquire_index);
    if (ar == VK_ERROR_OUT_OF_DATE_KHR || ar == VK_SUBOPTIMAL_KHR) {
        DestroySwapchain();
        if (!EnsureSwapchain()) return false;
        ar = g.fn.AcquireNextImageKHR(g.device, g.swap.handle, UINT64_MAX,
                                      g.swap.acquire_sem, VK_NULL_HANDLE,
                                      &g.swap.acquire_index);
    }
    if (ar != VK_SUCCESS) return false;
    g.swap.acquire_valid = true;
    const uint32_t idx = g.swap.acquire_index;

    // Swapchain image to a copyable layout, then blit the finished offscreen
    // target into it; hand it to the WSI in PRESENT_SRC.
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    g.fn.ResetCommandBuffer(g.cmd, 0);
    g.fn.BeginCommandBuffer(g.cmd, &bi);

    // offscreen target: COLOR_ATTACHMENT_OPTIMAL -> TRANSFER_SRC_OPTIMAL
    VkImageMemoryBarrier src_bar{};
    src_bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    src_bar.oldLayout = g.target_layout;
    src_bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    src_bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    src_bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    src_bar.image = g.target_image;
    src_bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    src_bar.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    src_bar.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    g.fn.CmdPipelineBarrier(g.cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                            nullptr, 1, &src_bar);

    // swapchain image: UNDEFINED -> TRANSFER_DST_OPTIMAL
    VkImageMemoryBarrier dst_bar{};
    dst_bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    dst_bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    dst_bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    dst_bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    dst_bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    dst_bar.image = g.swap.images[idx];
    dst_bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    dst_bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    g.fn.CmdPipelineBarrier(g.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                            nullptr, 1, &dst_bar);

    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[0] = {0, 0, 0};
    blit.srcOffsets[1] = {(int32_t)g.width, (int32_t)g.height, 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[0] = {0, 0, 0};
    blit.dstOffsets[1] = {(int32_t)g.swap.extent.width,
                          (int32_t)g.swap.extent.height, 1};
    g.fn.CmdBlitImage(g.cmd, g.target_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      g.swap.images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                      &blit, VK_FILTER_LINEAR);

    // swapchain: TRANSFER_DST -> PRESENT_SRC
    VkImageMemoryBarrier ps_bar{dst_bar};
    ps_bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    ps_bar.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    ps_bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    ps_bar.dstAccessMask = 0;
    g.fn.CmdPipelineBarrier(g.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr,
                            0, nullptr, 1, &ps_bar);

    // offscreen back to color-attachment for the next frame's render pass.
    VkImageMemoryBarrier src_back{src_bar};
    src_back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    src_back.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    src_back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    src_back.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    g.fn.CmdPipelineBarrier(g.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
                            nullptr, 0, nullptr, 1, &src_back);
    g.target_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    g.fn.EndCommandBuffer(g.cmd);

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &g.swap.acquire_sem;
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g.cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &g.swap.render_finished[idx];
    g.fn.QueueSubmit(g.queue, 1, &si, g.fence);

    VkPresentInfoKHR pi{};
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &g.swap.render_finished[idx];
    pi.swapchainCount = 1;
    pi.pSwapchains = &g.swap.handle;
    pi.pImageIndices = &idx;
    if (g.fn.QueuePresentKHR(g.queue, &pi) != VK_SUCCESS) {
        ML_LOG_ERROR("vk: QueuePresentKHR failed");
        return false;
    }
    // Present is synchronous for milestone safety: wait so the blit buffer's
    // images/semaphores are never reused while the GPU still holds them.
    g.fn.WaitForFences(g.device, 1, &g.fence, VK_TRUE, UINT64_MAX);
    g.fn.ResetFences(g.device, 1, &g.fence);
    g.swap.acquire_valid = false;
    return true;
#else
    (void)0;
    return false;
#endif
}

uint32_t PresentWidth() {
#ifdef VK_USE_PLATFORM_METAL_EXT
    if (g.swap.handle) return g.swap.extent.width;
#endif
    return TargetWidth();
}

uint32_t PresentHeight() {
#ifdef VK_USE_PLATFORM_METAL_EXT
    if (g.swap.handle) return g.swap.extent.height;
#endif
    return TargetHeight();
}

} // namespace mithril::vk