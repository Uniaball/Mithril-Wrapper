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

#include <chrono>

// M8 diagnostics: GL-layer draw counters (defined in gl/draw.cpp). Kept out
// of the public header graph -- the diag only needs the two totals.
uint64_t GetGlDrawCalls();
uint64_t GetGlFetchFail();
// M8 device-black-screen probe: renders one fullscreen red quad through the
// real GL draw path (see gl/draw.cpp) on the very first Present() call.
void RunGLSelfTestOnce();

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
    for (uint32_t i = 0; i < nfmt; ++i)
        ML_LOG_INFO("vk: surface format[%u] = %u (colorspace %u)",
                    i, (unsigned)fmts[i].format, (unsigned)fmts[i].colorSpace);
    g.fn.GetPhysicalDeviceSurfacePresentModesKHR(g.physical, surface, &npm, nullptr);
    std::vector<VkPresentModeKHR> modes(npm);
    if (npm) g.fn.GetPhysicalDeviceSurfacePresentModesKHR(g.physical, surface, &npm,
                                                          modes.data());

    // Pick a surface format. The offscreen target, MC's textures and FBO
    // attachments are all R8G8B8A8, and vkCmdBlitImage with FILTER_LINEAR
    // requires src/dst to share the same format (VUID-vkCmdBlitImage-srcImage-
    // 00229), so an R8G8B8A8 swapchain keeps the whole pipeline single-format.
    // Prefer R8G8B8A8_UNORM with any colorspace; only when the surface has no
    // such format at all (MoltenVK CAMetalLayer surfaces sometimes advertise
    // only B8G8R8A8) fall back to the first format and retarget the offscreen
    // target to match.
    VkSurfaceFormatKHR fmt{};
    if (nfmt == 0) return false;
    fmt = fmts[0];
    for (auto& f : fmts) {
        if (f.format == VK_FORMAT_R8G8B8A8_UNORM) {
            fmt = f;
            break;
        }
    }
    if (fmt.format != g.format) {
        ML_LOG_WARN("vk: swapchain format %u != target format %u; retargeting offscreen target",
                    (unsigned)fmt.format, (unsigned)g.format);
        if (!RecreateTargetForFormat(fmt.format)) {
            ML_LOG_ERROR("vk: failed to retarget offscreen target to swapchain format");
            return false;
        }
        ML_LOG_INFO("vk: offscreen target now format %u (%ux%u)",
                    (unsigned)g.format, g.width, g.height);
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
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (g.fn.CreateFence(g.device, &fci, nullptr, &g.swap.acquire_fence) !=
        VK_SUCCESS) {
        ML_LOG_ERROR("vk: swapchain acquire fence creation failed");
        return false;
    }
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
    if (g.swap.acquire_fence) {
        g.fn.DestroyFence(g.device, g.swap.acquire_fence, nullptr);
        g.swap.acquire_fence = VK_NULL_HANDLE;
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
    } else if (layer) {
        // CAMetalLayer's default pixel format is BGRA8Unorm, so the surface
        // exposes only B8G8R8A8 swapchain formats while the offscreen target
        // is fixed at R8G8B8A8_UNORM. vkCmdBlitImage with FILTER_LINEAR
        // requires identical src/dst formats (VUID-vkCmdBlitImage-srcImage-
        // 00229), so a BGRA swapchain either violates the spec or (via NEAREST
        // on the same format class) swaps R/B on screen. Pin the layer to
        // MTLPixelFormatRGBA8Unorm (== 70) so the surface advertises the
        // R8G8B8A8_UNORM family and both blit endpoints match. Sent through
        // the objc runtime to avoid a compile-time Metal framework dependency.
        typedef void (*SetPixelFormatFn)(id, SEL, uint64_t);
        ((SetPixelFormatFn)objc_msgSend)((id)layer,
                                         sel_registerName("setPixelFormat:"),
                                         (uint64_t)70 /* MTLPixelFormatRGBA8Unorm */);
    }
#endif
}

void SetVsync(bool enable) {
    g.vsync = enable;
}

bool EnsurePresentReady() {
    if (!EnsureInit()) return false;
#ifdef VK_USE_PLATFORM_METAL_EXT
    return g.native_layer ? EnsureSwapchain() : true;
#else
    return true;
#endif
}

bool Present() {
    // Lazy engine boot: eglSwapBuffers is the first device-touching call when
    // an app swaps without drawing (cleanup frames), so spin the engine up here.
    // If the backend cannot come up (no Vulkan loader, non-Apple build without
    // a swapchain path) treat the swap as a successful offscreen no-op so EGL
    // callers and the Linux lavapipe test contract see EGL_TRUE; only a real
    // swapchain failure (acquire/present error on Apple) is reported as failure.
    if (!EnsureInit()) return true;
    // M8 device black-screen probe: on the very first swap, render one
    // fullscreen red quad through the real GL->vk draw path (via the GL
    // layer, like any app draw) and leave it on the target. Present() below
    // then blits it out, so the screen shows red for the first frame if the
    // engine's render/present chain works at all -- the app's own (possibly
    // black) content overwrites it afterwards. Env-gated: MITHRIL_SELFTEST_FRAME=1.
    RunGLSelfTestOnce();
    // eglSwapBuffers has implicit flush semantics (EGL 1.5 §3.9.4): any GL
    // commands recorded for the default framebuffer must reach the GPU before
    // the present. Minecraft's render loop relies on this -- it does not call
    // glFlush/glFinish at frame end, only eglSwapBuffers -- so without kicking
    // the pending frame through SubmitFlush the offscreen target is blitted
    // while still in its initial undefined contents and the screen stays black.
    if (g.frame_dirty) SubmitFlush(false);
    RetireAllInflight();

    // Periodic diagnostic: report what the offscreen target actually contains
    // every few seconds. M8: five sample points (centre + four corners inset
    // 1/8) instead of the centre alone -- the Minecraft loading screen is
    // black at the centre but the title-screen panorama / in-game HUD fill
    // the corners, so a corner sample distinguishes "render bug" from "black
    // centre is normal" in one shot. Also dump the draw-path counters so a
    // dark target bisects immediately: growing draws/gl_draws with a dark
    // target points at the render side (pipeline/UBO), a flat gl_draws at the
    // GL fetch layer. Values are raw target bytes in target format order
    // (RGBA if format==37, BGRA if format==44; black is identical either way).
    {
        static auto s_diag_last = std::chrono::steady_clock::now() -
                                  std::chrono::seconds(10);
        auto now = std::chrono::steady_clock::now();
        if (now - s_diag_last >= std::chrono::seconds(5)) {
            s_diag_last = now;
            SubmitFlush(true);  // guarantee the readback reflects the latest frame
            RetireAllInflight();
            if (g.readback_map && g.read_w && g.read_h) {
                // Readback rows are top-down (row 0 = screen top).
                auto px = [&](uint32_t x, uint32_t y) {
                    const uint8_t* p =
                        g.readback_map + ((size_t)y * g.read_w + x) * 4;
                    return p;
                };
                const uint32_t w = g.read_w, h = g.read_h;
                const uint8_t* c = px(w / 2, h / 2);
                const uint8_t* tl = px(w / 8, h / 8);
                const uint8_t* tr = px(w - w / 8 - 1, h / 8);
                const uint8_t* bl = px(w / 8, h - h / 8 - 1);
                const uint8_t* br = px(w - w / 8 - 1, h - h / 8 - 1);
                ML_LOG_INFO(
                    "vk: diag target %ux%u format=%u align=%llu frame_ops=%u "
                    "draws_vk=%llu skip=%llu pipe_fail=%llu miss=%llu "
                    "ubo_wrap=%llu gl_draws=%llu gl_fetch_fail=%llu "
                    "selftest=%d clear=%d(%.2f,%.2f,%.2f,%.2f,m=%u) | "
                    "center=%u,%u,%u,%u tl=%u,%u,%u,%u tr=%u,%u,%u,%u "
                    "bl=%u,%u,%u,%u br=%u,%u,%u,%u",
                    w, h, (unsigned)g.format, (unsigned long long)g.ubo_align,
                    g.last_frame_ops, (unsigned long long)g.stats_draws_vk,
                    (unsigned long long)g.stats_draws_skipped,
                    (unsigned long long)g.stats_pipe_fail,
                    (unsigned long long)g.stats_pipe_miss,
                    (unsigned long long)g.stats_ubo_wrap,
                    (unsigned long long)GetGlDrawCalls(),
                    (unsigned long long)GetGlFetchFail(),
                    g.selftest_done ? 1 : 0,
                    g.last_diag.clear_applied ? 1 : 0, g.last_diag.clear[0],
                    g.last_diag.clear[1], g.last_diag.clear[2],
                    g.last_diag.clear[3], g.last_diag.clear_mask,
                    c[0], c[1], c[2], c[3], tl[0], tl[1], tl[2], tl[3],
                    tr[0], tr[1], tr[2], tr[3], bl[0], bl[1], bl[2], bl[3],
                    br[0], br[1], br[2], br[3]);
                for (uint32_t i = 0; i < g.last_diag.nops; ++i) {
                    const auto& o = g.last_diag.ops[i];
                    ML_LOG_INFO(
                        "vk: diag op%u prog=%llu off=%u rng=%u ubo8=%llx "
                        "v=%u idx=%u inst=%u m=%u tx=%u/%u vp=%d,%d,%d,%d "
                        "sc=%d,%d,%d,%d",
                        i, (unsigned long long)o.program, o.ubo_offset,
                        o.ubo_range, (unsigned long long)o.ubo8, o.vertex_count,
                        o.index_count, o.instance_count, o.topology, o.tex_count,
                        o.tex0_has_view, o.vp[0], o.vp[1], o.vp[2], o.vp[3],
                        o.sc[0], o.sc[1], o.sc[2], o.sc[3]);
                }
            } else {
                ML_LOG_INFO("vk: diag target center px unavailable (no readback)");
            }
        }
    }

#ifdef VK_USE_PLATFORM_METAL_EXT
    // No native window (offscreen fallback: the contract smoke passes a fake
    // non-CAMetalLayer pointer, which SetNativeLayer rejected) means there is
    // nothing to present to -- this swap is a successful no-op, matching the
    // offscreen lavapipe contract. Only a live layer that fails to build a
    // swapchain is a genuine present failure.
    if (!g.native_layer) return true;
    if (!EnsureSwapchain()) return false;

    RetireAllInflight();

    // Acquire the next image. The acquire is signalled via a fence rather
    // than a semaphore so the same sync object can be safely waited + reset
    // between frames; reusing a semaphore that MoltenVK has not yet observed
    // as signalled (or that the previous frame's QueueSubmit is still waiting
    // on) provokes VK_ERROR_OUT_OF_DATE_KHR / VK_ERROR_SURFACE_LOST_KHR,
    // which Present() read as a broken swapchain and rebuilt on every frame
    // without ever presenting.
    if (g.swap.acquire_valid) {
        g.fn.WaitForFences(g.device, 1, &g.swap.acquire_fence, VK_TRUE,
                           UINT64_MAX);
        g.fn.ResetFences(g.device, 1, &g.swap.acquire_fence);
    }
    VkResult ar = g.fn.AcquireNextImageKHR(
        g.device, g.swap.handle, UINT64_MAX, VK_NULL_HANDLE,
        g.swap.acquire_fence, &g.swap.acquire_index);
    // Only a hard OUT_OF_DATE warrants rebuilding. SUBOPTIMAL is not a
    // failure: the image is valid and presentable, just not a perfect surface
    // match (e.g. transform) -- rebuilding on every frame for it would spin.
    if (ar == VK_ERROR_OUT_OF_DATE_KHR) {
        DestroySwapchain();
        if (!EnsureSwapchain()) return false;
        ar = g.fn.AcquireNextImageKHR(g.device, g.swap.handle, UINT64_MAX,
                                      VK_NULL_HANDLE, g.swap.acquire_fence,
                                      &g.swap.acquire_index);
    }
    if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) return false;
    g.fn.WaitForFences(g.device, 1, &g.swap.acquire_fence, VK_TRUE,
                       UINT64_MAX);
    g.fn.ResetFences(g.device, 1, &g.swap.acquire_fence);
    g.swap.acquire_valid = true;
    const uint32_t idx = g.swap.acquire_index;

    // Swapchain image to a copyable layout, then blit the finished offscreen
    // target into it; hand it to the WSI in PRESENT_SRC.
    // g.cmd/g.fence are shared with texture uploads (worker threads) and
    // one-shot transitions -- serialise record+submit+wait like UploadImageData.
    std::lock_guard<std::mutex> aux_lock(g_aux_mutex);

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
    // Same-format blit (the target was retargeted to the swapchain format
    // above when needed), so FILTER_LINEAR is spec-legal and the scaler keeps
    // full quality on size mismatch.
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

    // No acquire semaphore to wait on -- acquire was fenced above and the
    // image is already available. Signal render_finished[idx] so the
    // present waits on the blit completion.
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g.cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &g.swap.render_finished[idx];
    if (g.fn.QueueSubmit(g.queue, 1, &si, g.fence) != VK_SUCCESS) {
        ML_LOG_ERROR("vk: QueueSubmit (blit) failed; present will fail too");
        return false;
    }

    VkPresentInfoKHR pi{};
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &g.swap.render_finished[idx];
    pi.swapchainCount = 1;
    pi.pSwapchains = &g.swap.handle;
    pi.pImageIndices = &idx;
    VkResult pr = g.fn.QueuePresentKHR(g.queue, &pi);
    // SUBOPTIMAL is NOT a present failure: the frame was still queued for
    // display (the swapchain just no longer perfectly matches the surface,
    // e.g. a transform or a slightly stale extent). Report success so the
    // app's swap completes; a full rebuild can follow on a later frame.
    if (pr != VK_SUCCESS && pr != VK_SUBOPTIMAL_KHR) {
        ML_LOG_ERROR("vk: QueuePresentKHR failed (0x%x)", (unsigned)pr);
        return false;
    }
    // Present is synchronous for milestone safety: wait so the blit buffer's
    // images/semaphores are never reused while the GPU still holds them.
    g.fn.WaitForFences(g.device, 1, &g.fence, VK_TRUE, UINT64_MAX);
    g.fn.ResetFences(g.device, 1, &g.fence);
    g.swap.acquire_valid = false;
    return true;
#else
    // Non-Apple build: no Metal surface / swapchain. The offscreen target is
    // the only sink, so the implicit flush above already did all the work;
    // report success so EGL callers (and the lavapipe test contract) see
    // EGL_TRUE rather than a spurious present failure.
    return true;
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

bool HasSwapchain() {
#ifdef VK_USE_PLATFORM_METAL_EXT
    return g.swap.handle != VK_NULL_HANDLE;
#else
    return false;
#endif
}

// Public ABI probe (matches scripts/exported_symbols.txt): tells the app/EGL
// layer whether a real swapchain is live rather than offscreen fallback.
extern "C" int mithril_has_swapchain(void) {
    return HasSwapchain() ? 1 : 0;
}

} // namespace mithril::vk