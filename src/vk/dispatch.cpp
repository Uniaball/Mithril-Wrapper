// Mithril-Wrapper Vulkan backend -- loader/instance dispatch (M2-VK).
// dlopens the Vulkan loader, resolves the instance/device function
// tables against the live handles, and owns EnsureInit: the one-time
// pipeline setup composed from the target.cpp helpers.

#include "internal.h"

#include <dlfcn.h>

#include <cstring>
#include <vector>

namespace mithril::vk {

void LoadDeviceFunctions() {
#define LOAD_DEV(NAME)                                                        \
    g.fn.NAME = reinterpret_cast<PFN_vk##NAME>(                              \
        g.fn.GetDeviceProcAddr(g.device, "vk" #NAME))
    LOAD_DEV(GetDeviceQueue);
    LOAD_DEV(DeviceWaitIdle);
    LOAD_DEV(CreateSemaphore);
    LOAD_DEV(DestroySemaphore);
    LOAD_DEV(CreateCommandPool);
    LOAD_DEV(DestroyCommandPool);
    LOAD_DEV(AllocateCommandBuffers);
    LOAD_DEV(FreeCommandBuffers);
    LOAD_DEV(BeginCommandBuffer);
    LOAD_DEV(EndCommandBuffer);
    LOAD_DEV(ResetCommandBuffer);
    LOAD_DEV(QueueSubmit);
    LOAD_DEV(QueueWaitIdle);
    LOAD_DEV(CreateFence);
    LOAD_DEV(DestroyFence);
    LOAD_DEV(WaitForFences);
    LOAD_DEV(ResetFences);
    LOAD_DEV(GetFenceStatus);
    LOAD_DEV(CreateRenderPass);
    LOAD_DEV(DestroyRenderPass);
    LOAD_DEV(CreateImageView);
    LOAD_DEV(DestroyImageView);
    LOAD_DEV(CreateImage);
    LOAD_DEV(DestroyImage);
    LOAD_DEV(AllocateMemory);
    LOAD_DEV(FreeMemory);
    LOAD_DEV(BindImageMemory);
    LOAD_DEV(BindBufferMemory);
    LOAD_DEV(CreateBuffer);
    LOAD_DEV(DestroyBuffer);
    LOAD_DEV(GetBufferMemoryRequirements);
    LOAD_DEV(GetImageMemoryRequirements);
    LOAD_DEV(MapMemory);
    LOAD_DEV(UnmapMemory);
    LOAD_DEV(CreateFramebuffer);
    LOAD_DEV(DestroyFramebuffer);
    LOAD_DEV(CreateShaderModule);
    LOAD_DEV(DestroyShaderModule);
    LOAD_DEV(CreateDescriptorSetLayout);
    LOAD_DEV(DestroyDescriptorSetLayout);
    LOAD_DEV(CreateDescriptorPool);
    LOAD_DEV(DestroyDescriptorPool);
    LOAD_DEV(AllocateDescriptorSets);
    LOAD_DEV(ResetDescriptorPool);
    LOAD_DEV(UpdateDescriptorSets);
    LOAD_DEV(CreatePipelineLayout);
    LOAD_DEV(DestroyPipelineLayout);
    LOAD_DEV(CreateGraphicsPipelines);
    LOAD_DEV(DestroyPipeline);
    LOAD_DEV(CmdBindPipeline);
    LOAD_DEV(CmdBindVertexBuffers);
    LOAD_DEV(CmdBindIndexBuffer);
    LOAD_DEV(CmdBindDescriptorSets);
    LOAD_DEV(CmdDraw);
    LOAD_DEV(CmdDrawIndexed);
    LOAD_DEV(CmdBeginRenderPass);
    LOAD_DEV(CmdEndRenderPass);
    LOAD_DEV(CmdClearColorImage);
    LOAD_DEV(CmdClearDepthStencilImage);
    LOAD_DEV(CmdPipelineBarrier);
    LOAD_DEV(CmdCopyImageToBuffer);
    LOAD_DEV(CmdCopyBufferToImage);
    LOAD_DEV(CmdCopyImage);
    LOAD_DEV(CmdBlitImage);
    LOAD_DEV(CmdResolveImage);
    LOAD_DEV(CreateSampler);
    LOAD_DEV(DestroySampler);
    LOAD_DEV(CreateSwapchainKHR);
    LOAD_DEV(DestroySwapchainKHR);
    LOAD_DEV(GetSwapchainImagesKHR);
    LOAD_DEV(AcquireNextImageKHR);
    LOAD_DEV(QueuePresentKHR);
    LOAD_DEV(CmdSetViewport);
    LOAD_DEV(CmdSetScissor);
    // M6 stage D: query objects (occlusion + timestamps).
    LOAD_DEV(CreateQueryPool);
    LOAD_DEV(DestroyQueryPool);
    LOAD_DEV(CmdBeginQuery);
    LOAD_DEV(CmdEndQuery);
    LOAD_DEV(CmdWriteTimestamp);
    LOAD_DEV(CmdResetQueryPool);
    LOAD_DEV(GetQueryPoolResults);
#undef LOAD_DEV
}

bool EnsureInit() {
    if (g.initialized) return true;

    // iOS has no Vulkan loader: MoltenVK is dlopen'd directly from the app
    // bundle. iOS dyld cannot resolve a bare "libMoltenVK.dylib" leaf name to
    // an embedded dylib, so anchor the lookup to this dylib's own directory --
    // libmithril.dylib and libMoltenVK.dylib ship side by side under the
    // launcher's Frameworks/. macOS keeps the loader-first order (Homebrew
    // vulkan-loader) and Linux the libvulkan*.so family.
    static const char* kLoaders[] = {
#ifdef MITHRIL_IOS
        "@loader_path/libMoltenVK.dylib",
#endif
        "libvulkan.so.1", "libvulkan.so", "libvulkan.dylib", "libvulkan.1.dylib",
        "libMoltenVK.dylib",
    };
    for (const char* name : kLoaders) {
        g.loader = dlopen(name, RTLD_NOW | RTLD_LOCAL);
        if (g.loader) break;
    }
    if (!g.loader) {
        ML_LOG_WARN("vk: no Vulkan loader found -- GL stays validation-only");
        return false;
    }

    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        dlsym(g.loader, "vkGetInstanceProcAddr"));
    auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        dlsym(g.loader, "vkGetDeviceProcAddr"));
    auto create_inst = reinterpret_cast<PFN_vkCreateInstance>(
        dlsym(g.loader, "vkCreateInstance"));
    if (!gipa || !gdpa || !create_inst) {
        ML_LOG_ERROR("vk: loader missing core entry points");
        dlclose(g.loader);
        g.loader = nullptr;
        return false;
    }
    g.fn.CreateInstance = create_inst;
    g.fn.GetDeviceProcAddr = gdpa;

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "Mithril-Wrapper";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
#ifdef VK_USE_PLATFORM_METAL_EXT
    // Loader-based builds (macOS Homebrew vulkan-loader, Linux) request
    // VK_KHR_portability_enumeration so the loader surfaces MoltenVK as a
    // portability ICD. When MoltenVK is dlopen'd directly (MITHRIL_IOS) there
    // is no loader in between, and MoltenVK itself does not implement this
    // extension -- requesting it fails vkCreateInstance at boot with
    // VK_ERROR_EXTENSION_NOT_PRESENT.
    const char* kInstExts[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_EXT_METAL_SURFACE_EXTENSION_NAME,
#ifndef MITHRIL_IOS
        VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME,
#endif
    };
    ici.enabledExtensionCount =
        (uint32_t)(sizeof(kInstExts) / sizeof(kInstExts[0]));
    ici.ppEnabledExtensionNames = kInstExts;
#ifndef MITHRIL_IOS
    // The loader only enumerates MoltenVK (a portability ICD) physical devices
    // when this flag is set; VK_KHR_portability_enumeration pairs with it.
    ici.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif
#endif
    if (g.fn.CreateInstance(&ici, nullptr, &g.instance) != VK_SUCCESS) {
        ML_LOG_ERROR("vk: vkCreateInstance failed");
        dlclose(g.loader);
        g.loader = nullptr;
        return false;
    }

    // Resolve instance-level functions against the live instance (global
    // GIPA only guarantees global commands).
#define LOAD_INST(NAME)                                                       \
    g.fn.NAME = reinterpret_cast<PFN_vk##NAME>(gipa(g.instance, "vk" #NAME))
    LOAD_INST(DestroyInstance);
    LOAD_INST(EnumeratePhysicalDevices);
    LOAD_INST(GetPhysicalDeviceProperties);
    LOAD_INST(GetPhysicalDeviceFeatures);
    LOAD_INST(GetPhysicalDeviceMemoryProperties);
    LOAD_INST(GetPhysicalDeviceQueueFamilyProperties);
    LOAD_INST(CreateDevice);
    LOAD_INST(EnumerateDeviceExtensionProperties);
    LOAD_INST(DestroySurfaceKHR);
    LOAD_INST(GetPhysicalDeviceSurfaceCapabilitiesKHR);
    LOAD_INST(GetPhysicalDeviceSurfaceFormatsKHR);
    LOAD_INST(GetPhysicalDeviceSurfacePresentModesKHR);
    LOAD_INST(GetPhysicalDeviceSurfaceSupportKHR);
#ifdef VK_USE_PLATFORM_METAL_EXT
    LOAD_INST(CreateMetalSurfaceEXT);
#endif
#undef LOAD_INST

    uint32_t n = 0;
    if (g.fn.EnumeratePhysicalDevices(g.instance, &n, nullptr) != VK_SUCCESS ||
        n == 0) {
        ML_LOG_ERROR("vk: no physical device");
        return false;
    }
    std::vector<VkPhysicalDevice> devs(n);
    g.fn.EnumeratePhysicalDevices(g.instance, &n, devs.data());
    g.physical = devs[0];

    VkPhysicalDeviceProperties props;
    g.fn.GetPhysicalDeviceProperties(g.physical, &props);
    ML_LOG_INFO("vk: physical device: %s", props.deviceName);
    if (props.limits.minUniformBufferOffsetAlignment > 16)
        g.ubo_align = props.limits.minUniformBufferOffsetAlignment;
    else
        g.ubo_align = 16;

    // M6 stage D: timestamp + occlusion query capabilities. Timestamp support
    // needs the graphics queue's timestamp precision too (the device limits
    // only carry the flag + period; per-queue bits live in the queue-family
    // properties), so the full check happens after queue selection below.
    g.ts_period_ns = (double)props.limits.timestampPeriod;
    VkPhysicalDeviceFeatures dev_feats;
    g.fn.GetPhysicalDeviceFeatures(g.physical, &dev_feats);
    g.occlusion_precise = (dev_feats.occlusionQueryPrecise == VK_TRUE);

    uint32_t n_fam = 0;
    g.fn.GetPhysicalDeviceQueueFamilyProperties(g.physical, &n_fam, nullptr);
    std::vector<VkQueueFamilyProperties> fam(n_fam);
    g.fn.GetPhysicalDeviceQueueFamilyProperties(g.physical, &n_fam, fam.data());
    bool have_graphics = false;
    for (uint32_t i = 0; i < n_fam; ++i) {
        if (fam[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            g.queue_family = i;
            have_graphics = true;
            break;
        }
    }
    if (!have_graphics) {
        ML_LOG_ERROR("vk: no graphics queue family");
        return false;
    }
    // M6 stage D: complete the timestamp capability check (graphics queue's
    // timestamp precision) once the graphics queue family is known.
    if (props.limits.timestampComputeAndGraphics &&
        fam[g.queue_family].timestampValidBits > 0) {
        g.have_timestamps = true;
        g.ts_valid_mask = fam[g.queue_family].timestampValidBits >= 64
                              ? ~0ull
                              : ((1ull << fam[g.queue_family].timestampValidBits) - 1);
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo dqc{};
    dqc.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    dqc.queueFamilyIndex = g.queue_family;
    dqc.queueCount = 1;
    dqc.pQueuePriorities = &prio;

    // Device extensions. VK_EXT_provoking_vertex (glProvokingVertex, S6) is
    // added when the device advertises it; the LAST convention it needs to
    // express matches GL's default, so without it the pipeline simply keeps
    // the implicit behaviour.
    std::vector<const char*> dev_exts;
#ifdef VK_USE_PLATFORM_METAL_EXT
    dev_exts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
#endif
    uint32_t n_ext = 0;
    g.fn.EnumerateDeviceExtensionProperties(g.physical, nullptr, &n_ext, nullptr);
    std::vector<VkExtensionProperties> ext_props(n_ext);
    g.fn.EnumerateDeviceExtensionProperties(g.physical, nullptr, &n_ext,
                                            ext_props.data());
    for (const auto& e : ext_props) {
        if (!std::strcmp(e.extensionName, VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME))
            g.have_provoking_vertex = true;
    }
    if (g.have_provoking_vertex)
        dev_exts.push_back(VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME);

    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &dqc;
    dci.enabledExtensionCount = (uint32_t)dev_exts.size();
    dci.ppEnabledExtensionNames = dev_exts.data();
    // M6 stage D: request precise occlusion counting when the device has it
    // (SAMPLES_PASSED needs exact sample counts; ANY_SAMPLES_PASSED works
    // either way from the non-zero slot sums), and the provoking-vertex LAST
    // mode (GL default) when the extension is present.
    VkPhysicalDeviceFeatures2 feats2{};
    feats2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    feats2.features.occlusionQueryPrecise =
        g.occlusion_precise ? VK_TRUE : VK_FALSE;
    VkPhysicalDeviceProvokingVertexFeaturesEXT pvf{};
    if (g.have_provoking_vertex) {
        pvf.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT;
        pvf.provokingVertexLast = VK_TRUE;
        pvf.pNext = feats2.pNext;
        feats2.pNext = &pvf;
    }
    dci.pNext = &feats2;
    if (g.fn.CreateDevice(g.physical, &dci, nullptr, &g.device) != VK_SUCCESS) {
        ML_LOG_ERROR("vk: vkCreateDevice failed");
        return false;
    }
    LoadDeviceFunctions();
    g.fn.GetDeviceQueue(g.device, g.queue_family, 0, &g.queue);

    // M6 stage D: per-frame occlusion/timestamp query pools.
    CreateQueryPools();

    // Dynamic UBO pool (host visible) per frame slot: an in-flight frame may
    // still be reading its region while the next frame records into the other
    // slot, so they must not share backing memory.
    for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        FrameSlot& fr = g.frames[i];
        if (CreateHostBuffer(kUboPoolSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                             &fr.ubo, &fr.ubo_mem) != VK_SUCCESS ||
            g.fn.MapMemory(g.device, fr.ubo_mem, 0, VK_WHOLE_SIZE, 0,
                           reinterpret_cast<void**>(&fr.ubo_map)) != VK_SUCCESS) {
            ML_LOG_ERROR("vk: dynamic UBO pool allocation failed");
            return false;
        }
    }

    // One dynamic UBO binding (0) + one combined image sampler per GL unit
    // (bindings 1..kMaxUnits) shared by every pipeline.
    std::array<VkDescriptorSetLayoutBinding, 1 + kMaxUnits> dslb{};
    dslb[0].binding = 0;
    dslb[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    dslb[0].descriptorCount = 1;
    dslb[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    for (uint32_t i = 1; i <= kMaxUnits; ++i) {
        dslb[i].binding = i;
        dslb[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        dslb[i].descriptorCount = 1;
        dslb[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dsli{};
    dsli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dsli.bindingCount = dslb.size();
    dsli.pBindings = dslb.data();
    if (g.fn.CreateDescriptorSetLayout(g.device, &dsli, nullptr,
                                       &g.set_layout) != VK_SUCCESS) {
        ML_LOG_ERROR("vk: CreateDescriptorSetLayout failed");
        return false;
    }

    std::array<VkDescriptorPoolSize, 2> dps{};
    dps[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    dps[0].descriptorCount = kDescriptorPoolSets;
    dps[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    dps[1].descriptorCount = kDescriptorPoolSets * kMaxUnits;
    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = kDescriptorPoolSets;
    dpci.poolSizeCount = dps.size();
    dpci.pPoolSizes = dps.data();
    for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        if (g.fn.CreateDescriptorPool(g.device, &dpci, nullptr,
                                      &g.frames[i].desc_pool) != VK_SUCCESS) {
            ML_LOG_ERROR("vk: CreateDescriptorPool failed");
            return false;
        }
    }

    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &g.set_layout;
    if (g.fn.CreatePipelineLayout(g.device, &plci, nullptr,
                                  &g.pipeline_layout) != VK_SUCCESS) {
        ML_LOG_ERROR("vk: CreatePipelineLayout failed");
        return false;
    }

    VkCommandPoolCreateInfo cpci{};
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = g.queue_family;
    if (g.fn.CreateCommandPool(g.device, &cpci, nullptr, &g.pool) != VK_SUCCESS)
        return false;

    // Auxiliary one-shot command buffer (texture uploads / blits / init) and
    // one command buffer + fence per frame slot (the draw path).
    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = g.pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    cbai.commandBufferCount = 1;
    if (g.fn.AllocateCommandBuffers(g.device, &cbai, &g.cmd) != VK_SUCCESS)
        return false;
    if (g.fn.CreateFence(g.device, &fci, nullptr, &g.fence) != VK_SUCCESS)
        return false;
    for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        if (g.fn.AllocateCommandBuffers(g.device, &cbai,
                                        &g.frames[i].cmd) != VK_SUCCESS)
            return false;
        if (g.fn.CreateFence(g.device, &fci, nullptr,
                             &g.frames[i].fence) != VK_SUCCESS)
            return false;
    }

    if (!CreateRenderPass()) {
        ML_LOG_ERROR("vk: CreateRenderPass failed");
        return false;
    }
    if (!CreateTarget()) {
        ML_LOG_ERROR("vk: target creation failed");
        return false;
    }

    // Bring the fresh image to COLOR_ATTACHMENT_OPTIMAL.
    {
        std::lock_guard<std::mutex> aux_lock(g_aux_mutex);
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (g.fn.BeginCommandBuffer(g.cmd, &bi) == VK_SUCCESS) {
            TransitionLayout(g.cmd, g.target_image, VK_IMAGE_LAYOUT_UNDEFINED,
                             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            g.fn.EndCommandBuffer(g.cmd);
            VkSubmitInfo si{};
            si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            si.commandBufferCount = 1;
            si.pCommandBuffers = &g.cmd;
            g.fn.QueueSubmit(g.queue, 1, &si, g.fence);
            g.fn.WaitForFences(g.device, 1, &g.fence, VK_TRUE, UINT64_MAX);
            g.fn.ResetFences(g.device, 1, &g.fence);
            g.target_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
    }

    if (CreateHostBuffer(g.width * g.height * 4,
                         VK_BUFFER_USAGE_TRANSFER_DST_BIT, &g.readback,
                         &g.readback_mem) != VK_SUCCESS ||
        g.fn.MapMemory(g.device, g.readback_mem, 0, VK_WHOLE_SIZE, 0,
                       reinterpret_cast<void**>(&g.readback_map)) !=
            VK_SUCCESS) {
        ML_LOG_ERROR("vk: readback staging allocation failed");
        return false;
    }

    g.initialized = true;

    // The 1x1 white fallback image; now that initialization is complete the
    // texture path (which early-outs on !g.initialized) can upload it.
    CreateDummyTexture();

    ML_LOG_INFO("vk: engine ready (%ux%u offscreen)", g.width, g.height);
    return true;
}
} // namespace mithril::vk
