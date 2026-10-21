#include "mealkit/rhi/vulkan/Vk.h"

#include "mealkit/core/Log.h"
#include "mealkit/rhi/GpuCapture.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace mealkit::rhi {
namespace {

const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

const char* resultName(VkResult r) {
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    default: return "VkResult";
    }
}

VKAPI_ATTR VkBool32 VKAPI_CALL onValidation(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
                                            const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) MEALKIT_ERROR("[Vulkan] %s", data->pMessage);
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) MEALKIT_WARN("[Vulkan] %s", data->pMessage);
    return VK_FALSE;
}

std::string versionText(u32 v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u", VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v));
    return buf;
}

bool hasExtension(VkPhysicalDevice physical, const char* name) {
    u32 count = 0;
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> exts(count);
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, exts.data());
    for (const VkExtensionProperties& e : exts)
        if (std::strcmp(e.extensionName, name) == 0) return true;
    return false;
}

// 그래픽 큐 (surface가 있으면 프레젠트도 되는 것). 없으면 UINT32_MAX
u32 findQueueFamily(VkPhysicalDevice physical, VkSurfaceKHR surface) {
    u32 count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
    for (u32 i = 0; i < count; ++i) {
        if (!(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
        VkBool32 present = VK_TRUE;
        if (surface) vkGetPhysicalDeviceSurfaceSupportKHR(physical, i, surface, &present);
        if (present) return i;
    }
    return UINT32_MAX;
}

// 최소 사양 (Vulkan 1.3 + 엔진이 쓰는 기능). 모자란 것을 문장으로, 맞으면 빈 문자열
std::string missingFeatures(VkPhysicalDevice physical, VkSurfaceKHR surface) {
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physical, &props);
    std::string missing;
    auto need = [&](bool ok, const char* what) {
        if (ok) return;
        if (!missing.empty()) missing += ", ";
        missing += what;
    };
    need(props.apiVersion >= VK_API_VERSION_1_3, "Vulkan 1.3");
    if (props.apiVersion < VK_API_VERSION_1_3) return missing;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12};
    vkGetPhysicalDeviceFeatures2(physical, &features);
    need(f12.descriptorIndexing && f12.runtimeDescriptorArray && f12.descriptorBindingPartiallyBound &&
             f12.descriptorBindingSampledImageUpdateAfterBind && f12.descriptorBindingStorageBufferUpdateAfterBind &&
             f12.shaderSampledImageArrayNonUniformIndexing,
         "descriptor indexing");
    need(f13.dynamicRendering, "dynamic rendering");
    need(f13.synchronization2, "synchronization2");
    need(f13.shaderDemoteToHelperInvocation, "demote to helper invocation");
    need(features.features.textureCompressionBC, "BC texture compression");
    need(hasExtension(physical, VK_KHR_SWAPCHAIN_EXTENSION_NAME), VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    need(hasExtension(physical, VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME), VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME);
    need(findQueueFamily(physical, surface) != UINT32_MAX, surface ? "graphics + present queue" : "graphics queue");
    return missing;
}

void describeGpu(VkPhysicalDevice physical, GpuInfo& info) {
    VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &driver};
    vkGetPhysicalDeviceProperties2(physical, &props);
    const VkPhysicalDeviceProperties& p = props.properties;
    info.api = "Vulkan " + versionText(p.apiVersion);
    info.adapter = p.deviceName;
    info.driverVersion = driver.driverInfo[0] ? driver.driverInfo : driver.driverName;
    info.vendorId = p.vendorID;
    info.deviceId = p.deviceID;
    info.uma = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    for (u32 i = 0; i < memory.memoryHeapCount; ++i) {
        const bool local = memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
        if (local && !info.uma) info.dedicatedVideoMemory += memory.memoryHeaps[i].size;
        else info.sharedSystemMemory += memory.memoryHeaps[i].size;
    }
    VkPhysicalDeviceSubgroupProperties subgroup{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &subgroup};
    vkGetPhysicalDeviceProperties2(physical, &props2);
    info.waveLaneCountMin = info.waveLaneCountMax = subgroup.subgroupSize;
}

// 최소 사양을 넘는 장치 중 외장 > 내장 > 그 밖 순서. 맞는 것이 없으면 첫 장치와 모자란 것
VkPhysicalDevice pickDevice(VkInstance instance, VkSurfaceKHR surface, std::string* missing) {
    u32 count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());
    VkPhysicalDevice best = VK_NULL_HANDLE;
    int bestRank = -1;
    for (VkPhysicalDevice d : devices) {
        if (!missingFeatures(d, surface).empty()) continue;
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(d, &p);
        const int rank = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 3
                         : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2
                         : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU            ? 0
                                                                                  : 1;
        if (rank > bestRank) {
            best = d;
            bestRank = rank;
        }
    }
    if (best) {
        missing->clear();
        return best;
    }
    if (devices.empty()) {
        *missing = "no Vulkan device";
        return VK_NULL_HANDLE;
    }
    *missing = missingFeatures(devices[0], surface);
    return devices[0];
}

bool hasInstanceLayer(const char* name) {
    u32 count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    for (const VkLayerProperties& l : layers)
        if (std::strcmp(l.layerName, name) == 0) return true;
    return false;
}

VkInstance createInstance(bool withSurface, bool validation, bool* debugUtils) {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "MealKit";
    app.pEngineName = "MealKit";
    app.apiVersion = VK_API_VERSION_1_3;
    std::vector<const char*> extensions;
    if (withSurface) {
        extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
        extensions.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
    }
    std::vector<const char*> layers;
    *debugUtils = false;
    if (validation) {
        if (hasInstanceLayer(kValidationLayer)) {
            layers.push_back(kValidationLayer);
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            *debugUtils = true;
            MEALKIT_INFO("Vulkan validation layer enabled");
        } else {
            MEALKIT_WARN("Vulkan validation layer unavailable (install the Vulkan SDK)");
        }
    }
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = static_cast<u32>(extensions.size());
    ci.ppEnabledExtensionNames = extensions.data();
    ci.enabledLayerCount = static_cast<u32>(layers.size());
    ci.ppEnabledLayerNames = layers.data();
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&ci, nullptr, &instance) != VK_SUCCESS) return VK_NULL_HANDLE;
    volkLoadInstanceOnly(instance);
    return instance;
}

// 이미지 배치 바꾸기 (synchronization2). 프레임에 몇 번뿐이라 단계는 넓게 잡는다
void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                  u32 mipLevels = 1) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, 0, mipLevels, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

VkImageView createView(VkDevice device, VkImage image, VkFormat format, VkImageAspectFlags aspect, u32 mipLevels = 1) {
    VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    ci.image = image;
    ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ci.format = format;
    ci.subresourceRange = {aspect, 0, mipLevels, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    MEALKIT_VK(vkCreateImageView(device, &ci, nullptr, &view));
    return view;
}

VkFormat toVk(Format f) {
    switch (f) {
    case Format::R8Unorm: return VK_FORMAT_R8_UNORM;
    case Format::Rgba8Srgb: return VK_FORMAT_R8G8B8A8_SRGB;
    case Format::Bc7Unorm: return VK_FORMAT_BC7_UNORM_BLOCK;
    case Format::Bc7Srgb: return VK_FORMAT_BC7_SRGB_BLOCK;
    case Format::Bc5Unorm: return VK_FORMAT_BC5_UNORM_BLOCK;
    default: return VK_FORMAT_R8G8B8A8_UNORM;
    }
}

} // namespace

void checkVk(VkResult result, const char* expr, const char* file, int line) {
    if (result == VK_SUCCESS) return;
    MEALKIT_FATAL("Vulkan call failed (%s, %d)\n%s\n%s:%d", resultName(result), static_cast<int>(result), expr, file, line);
}
#define VK MEALKIT_VK

// ---------------------------------------------------------------- Impl

bool Device::Impl::createSwapchain() {
    VkSurfaceCapabilitiesKHR caps{};
    MEALKIT_VK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps));
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) extent = {width, height};
    extent.width = std::clamp(extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
    extent.height = std::clamp(extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    if (extent.width == 0 || extent.height == 0) return false; // 최소화

    u32 imageCount = std::max(caps.minImageCount, 3u);
    if (caps.maxImageCount) imageCount = std::min(imageCount, caps.maxImageCount);

    const VkFormat formats[2] = {swapchainFormat, sceneFormat};
    VkImageFormatListCreateInfo formatList{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
    formatList.viewFormatCount = 2;
    formatList.pViewFormats = formats;

    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, &formatList};
    ci.flags = VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR;
    ci.surface = surface;
    ci.minImageCount = imageCount;
    ci.imageFormat = swapchainFormat;
    ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    // 수직동기 끔: 티어링 허용(IMMEDIATE), 없으면 MAILBOX. DX12의 ALLOW_TEARING과 같다
    ci.presentMode = swapchainVsync         ? VK_PRESENT_MODE_FIFO_KHR
                     : immediateSupported   ? VK_PRESENT_MODE_IMMEDIATE_KHR
                     : mailboxSupported     ? VK_PRESENT_MODE_MAILBOX_KHR
                                            : VK_PRESENT_MODE_FIFO_KHR;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = swapchain;
    VkSwapchainKHR created = VK_NULL_HANDLE;
    VK(vkCreateSwapchainKHR(device, &ci, nullptr, &created));
    destroySwapchain(); // 이전 것 (GPU는 이미 기다렸다)
    swapchain = created;
    width = extent.width;
    height = extent.height;

    u32 count = 0;
    vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr);
    std::vector<VkImage> images(count);
    vkGetSwapchainImagesKHR(device, swapchain, &count, images.data());
    swapImages.resize(count);
    for (u32 i = 0; i < count; ++i) {
        SwapImage& s = swapImages[i];
        s.image = images[i];
        s.sceneView = createView(device, s.image, sceneFormat, VK_IMAGE_ASPECT_COLOR_BIT);
        s.overlayView = createView(device, s.image, swapchainFormat, VK_IMAGE_ASPECT_COLOR_BIT);
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK(vkCreateSemaphore(device, &si, nullptr, &s.renderFinished));
    }

    VkImageCreateInfo di{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    di.imageType = VK_IMAGE_TYPE_2D;
    di.format = kDepthFormat;
    di.extent = {width, height, 1};
    di.mipLevels = 1;
    di.arrayLayers = 1;
    di.samples = VK_SAMPLE_COUNT_1_BIT;
    di.tiling = VK_IMAGE_TILING_OPTIMAL;
    di.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VK(vmaCreateImage(allocator, &di, &ai, &depthImage, &depthAllocation, nullptr));
    depthView = createView(device, depthImage, kDepthFormat, VK_IMAGE_ASPECT_DEPTH_BIT);
    swapchainDirty = false;
    return true;
}

void Device::Impl::destroySwapchain() {
    for (SwapImage& s : swapImages) {
        vkDestroyImageView(device, s.sceneView, nullptr);
        vkDestroyImageView(device, s.overlayView, nullptr);
        vkDestroySemaphore(device, s.renderFinished, nullptr);
    }
    swapImages.clear();
    if (depthView) vkDestroyImageView(device, depthView, nullptr);
    if (depthImage) vmaDestroyImage(allocator, depthImage, depthAllocation);
    depthView = VK_NULL_HANDLE;
    depthImage = VK_NULL_HANDLE;
    if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
    swapchain = VK_NULL_HANDLE;
}

void Device::Impl::endRendering() {
    if (bound == Bound::None) return;
    vkCmdEndRendering(current());
    bound = Bound::None;
}

void Device::Impl::beginRendering(Bound target, const f32* clearColor, f32 clearDepth) {
    endRendering();
    VkCommandBuffer list = current();
    const SwapImage& s = swapImages[imageIndex];
    if (!imageIsTarget) {
        imageBarrier(list, s.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        imageIsTarget = true;
    }
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = target == Bound::Scene ? s.sceneView : s.overlayView;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = clearColor ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    if (clearColor) std::memcpy(color.clearValue.color.float32, clearColor, sizeof(f32) * 4);
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {width, height}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    if (target == Bound::Scene) {
        imageBarrier(list, depthImage, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
        depth.imageView = depthView;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.clearValue.depthStencil.depth = clearDepth;
        ri.pDepthAttachment = &depth;
    }
    vkCmdBeginRendering(list, &ri);
    bound = target;
}

VkCommandBuffer Device::Impl::beginImmediate() {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = immediatePool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer list = VK_NULL_HANDLE;
    VK(vkAllocateCommandBuffers(device, &ai, &list));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK(vkBeginCommandBuffer(list, &bi));
    return list;
}

void Device::Impl::endImmediate(VkCommandBuffer list) {
    VK(vkEndCommandBuffer(list));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &list;
    VK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
    VK(vkQueueWaitIdle(queue));
    vkFreeCommandBuffers(device, immediatePool, 1, &list);
}

u32 Device::Impl::allocTextureSlot() {
    if (freeTextureSlots.empty()) MEALKIT_FATAL("Bindless texture slots exhausted (%u)", kMaxTextures);
    const u32 slot = freeTextureSlots.back();
    freeTextureSlots.pop_back();
    return slot;
}

// ---------------------------------------------------------------- Device

Device::~Device() { shutdown(); }

bool Device::init(const DeviceDesc& desc) {
    if (desc.gpuCapture) loadGpuCaptureTool(false); // 캡처 도구는 장치보다 먼저 들어와야 API를 가로챈다
    m = new Impl();
    m->hwnd = desc.window;
    m->width = desc.width;
    m->height = desc.height;
    m->cmd.m_device = this;
    auto fail = [&](const std::string& why) {
        m->initError = why;
        MEALKIT_ERROR("%s", why.c_str());
        return false;
    };

    if (volkInitialize() != VK_SUCCESS) return fail("Vulkan is not available (vulkan-1.dll not found). Update the GPU driver.");
    if (volkGetInstanceVersion() < VK_API_VERSION_1_3)
        return fail("Vulkan 1.3 is required, but the Vulkan loader is " + versionText(volkGetInstanceVersion()) +
                    ". Update the GPU driver.");
    bool debugUtils = false;
    m->instance = createInstance(true, desc.debug, &debugUtils);
    if (!m->instance) return fail("Vulkan instance creation failed");
    if (debugUtils) {
        VkDebugUtilsMessengerCreateInfoEXT mi{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        mi.pfnUserCallback = onValidation;
        vkCreateDebugUtilsMessengerEXT(m->instance, &mi, nullptr, &m->messenger);
    }

    VkWin32SurfaceCreateInfoKHR si{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    si.hwnd = static_cast<HWND>(desc.window);
    si.hinstance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(si.hwnd, GWLP_HINSTANCE));
    VK(vkCreateWin32SurfaceKHR(m->instance, &si, nullptr, &m->surface));

    std::string missing;
    m->physical = pickDevice(m->instance, m->surface, &missing);
    if (!m->physical) return fail("No Vulkan capable GPU found");
    describeGpu(m->physical, m->gpu);
    m->gpu.unsupported = missing.empty() ? std::string() : m->gpu.adapter + " lacks " + missing + ". Update the GPU driver.";
    const GpuInfo& gpu = m->gpu;
    MEALKIT_INFO("GPU: %s | driver %s | %s | %s memory", gpu.adapter.c_str(),
                 gpu.driverVersion.empty() ? "?" : gpu.driverVersion.c_str(), gpu.api.c_str(),
                 gpu.uma ? "unified (UMA)" : "dedicated");
    if (!m->gpu.unsupported.empty()) return fail(m->gpu.unsupported);
    vkGetPhysicalDeviceProperties(m->physical, &m->properties);
    m->apiVersion = VK_API_VERSION_1_3;
    m->queueFamily = findQueueFamily(m->physical, m->surface);

    // 장치: 큐 하나 + 엔진이 쓰는 기능만 켠다
    const f32 priority = 1.0f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = m->queueFamily;
    qi.queueCount = 1;
    qi.pQueuePriorities = &priority;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    f13.shaderDemoteToHelperInvocation = VK_TRUE;
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
    f12.descriptorIndexing = VK_TRUE;
    f12.runtimeDescriptorArray = VK_TRUE;
    f12.descriptorBindingPartiallyBound = VK_TRUE;
    f12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    f12.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
    f12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE; // 머티리얼 텍스처 (NonUniformResourceIndex)
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12};
    VkPhysicalDeviceFeatures supported{};
    vkGetPhysicalDeviceFeatures(m->physical, &supported);
    m->fillModeNonSolid = supported.fillModeNonSolid == VK_TRUE; // 선택 (에디터 와이어프레임 보기)
    features.features.fillModeNonSolid = supported.fillModeNonSolid;
    m->samplerAnisotropy = supported.samplerAnisotropy == VK_TRUE; // 모델 텍스처 샘플러 (없으면 이방성 없이)
    features.features.samplerAnisotropy = supported.samplerAnisotropy;
    features.features.textureCompressionBC = supported.textureCompressionBC; // BC7·BC5 (데스크톱 GPU는 모두 있다)
    std::vector<const char*> extensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME};
    m->memoryBudget = hasExtension(m->physical, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME); // 선택 (GPU 메모리 숫자)
    if (m->memoryBudget) extensions.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &features};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qi;
    dci.enabledExtensionCount = static_cast<u32>(extensions.size());
    dci.ppEnabledExtensionNames = extensions.data();
    VK(vkCreateDevice(m->physical, &dci, nullptr, &m->device));
    volkLoadDevice(m->device);
    vkGetDeviceQueue(m->device, m->queueFamily, 0, &m->queue);

    VmaVulkanFunctions functions{};
    functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo vi{};
    vi.vulkanApiVersion = m->apiVersion;
    vi.physicalDevice = m->physical;
    vi.device = m->device;
    vi.instance = m->instance;
    vi.pVulkanFunctions = &functions;
    if (m->memoryBudget) vi.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
    VK(vmaCreateAllocator(&vi, &m->allocator));

    // 스왑체인 형식: UNORM(오버레이) + 같은 이미지의 SRGB 뷰(장면). Windows 드라이버는 BGRA가 흔하다
    u32 formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(m->physical, m->surface, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> surfaceFormats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(m->physical, m->surface, &formatCount, surfaceFormats.data());
    for (VkFormat want : {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM}) {
        for (const VkSurfaceFormatKHR& f : surfaceFormats) {
            if (f.format == want && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR && !m->swapchainFormat) {
                m->swapchainFormat = want;
                m->sceneFormat = want == VK_FORMAT_R8G8B8A8_UNORM ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_B8G8R8A8_SRGB;
                m->swapchainBgra = want == VK_FORMAT_B8G8R8A8_UNORM;
            }
        }
    }
    if (!m->swapchainFormat) return fail("No 8-bit RGBA swapchain format on this GPU");
    u32 modeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(m->physical, m->surface, &modeCount, nullptr);
    std::vector<VkPresentModeKHR> modes(modeCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(m->physical, m->surface, &modeCount, modes.data());
    for (VkPresentModeKHR mode : modes) {
        m->immediateSupported |= mode == VK_PRESENT_MODE_IMMEDIATE_KHR;
        m->mailboxSupported |= mode == VK_PRESENT_MODE_MAILBOX_KHR;
    }
    if (!m->createSwapchain()) return fail("Swapchain creation failed (window has no size)");

    // bindless 세트: 0 = 버퍼 배열, 1 = 텍스처 배열, 2 = 고정 샘플러 (shaders/Bindless.hlsli)
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = VK_LOD_CLAMP_NONE;
    VK(vkCreateSampler(m->device, &sci, nullptr, &m->linearClamp));
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT; // 3: 모델 텍스처
    sci.anisotropyEnable = m->samplerAnisotropy ? VK_TRUE : VK_FALSE;
    sci.maxAnisotropy = 8.0f;
    VK(vkCreateSampler(m->device, &sci, nullptr, &m->linearWrap));
    VkDescriptorSetLayoutBinding bindings[4] = {};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kMaxBuffers, VK_SHADER_STAGE_ALL_GRAPHICS, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kMaxTextures, VK_SHADER_STAGE_ALL_GRAPHICS, nullptr};
    bindings[2] = {2, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_ALL_GRAPHICS, &m->linearClamp};
    bindings[3] = {3, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_ALL_GRAPHICS, &m->linearWrap};
    const VkDescriptorBindingFlags bindless =
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
    const VkDescriptorBindingFlags bindingFlags[4] = {bindless, bindless, 0, 0};
    VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    flagsInfo.bindingCount = 4;
    flagsInfo.pBindingFlags = bindingFlags;
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &flagsInfo};
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    li.bindingCount = 4;
    li.pBindings = bindings;
    VK(vkCreateDescriptorSetLayout(m->device, &li, nullptr, &m->setLayout));
    const VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kMaxBuffers},
                                           {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kMaxTextures},
                                           {VK_DESCRIPTOR_TYPE_SAMPLER, 2}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pi.maxSets = 1;
    pi.poolSizeCount = 3;
    pi.pPoolSizes = sizes;
    VK(vkCreateDescriptorPool(m->device, &pi, nullptr, &m->descriptorPool));
    VkDescriptorSetAllocateInfo sai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    sai.descriptorPool = m->descriptorPool;
    sai.descriptorSetCount = 1;
    sai.pSetLayouts = &m->setLayout;
    VK(vkAllocateDescriptorSets(m->device, &sai, &m->descriptorSet));
    for (u32 i = kMaxTextures; i > 0; --i) m->freeTextureSlots.push_back(i - 1);
    const VkPushConstantRange push{VK_SHADER_STAGE_ALL_GRAPHICS, 0, kPushConstantBytes};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &m->setLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &push;
    VK(vkCreatePipelineLayout(m->device, &pli, nullptr, &m->pipelineLayout));

    // 업로드 링: CPU가 쓰고 GPU가 읽는 메모리 하나 (UMA에서는 GPU 메모리 그대로), g_buffers[0]
    const u64 ringBytes = kUploadBytesPerFrame * kFramesInFlight;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = ringBytes;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};
    VK(vmaCreateBuffer(m->allocator, &bci, &aci, &m->uploadBuffer, &m->uploadAllocation, &info));
    m->uploadCpu = static_cast<u8*>(info.pMappedData);
    const VkDescriptorBufferInfo ringInfo{m->uploadBuffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = m->descriptorSet;
    write.dstBinding = 0;
    write.dstArrayElement = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &ringInfo;
    vkUpdateDescriptorSets(m->device, 1, &write, 0, nullptr);

    for (Impl::Frame& f : m->frames) {
        VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        ci.queueFamilyIndex = m->queueFamily;
        VK(vkCreateCommandPool(m->device, &ci, nullptr, &f.pool));
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = f.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VK(vkAllocateCommandBuffers(m->device, &ai, &f.cmd));
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK(vkCreateFence(m->device, &fi, nullptr, &f.fence));
        VkSemaphoreCreateInfo si2{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK(vkCreateSemaphore(m->device, &si2, nullptr, &f.imageAcquired));
    }
    VkCommandPoolCreateInfo ipi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    ipi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    ipi.queueFamilyIndex = m->queueFamily;
    VK(vkCreateCommandPool(m->device, &ipi, nullptr, &m->immediatePool));

    // GPU 시간: 그래픽 큐가 타임스탬프를 지원하면 쿼리 풀 하나 (프레임마다 한 구간)
    u32 familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(m->physical, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(m->physical, &familyCount, families.data());
    const u32 validBits = m->queueFamily < familyCount ? families[m->queueFamily].timestampValidBits : 0;
    if (validBits > 0 && m->properties.limits.timestampPeriod > 0.0f) {
        VkQueryPoolCreateInfo qpi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qpi.queryCount = kQueriesPerFrame * kFramesInFlight;
        if (vkCreateQueryPool(m->device, &qpi, nullptr, &m->queryPool) == VK_SUCCESS) {
            m->timestampMsPerTick = static_cast<f64>(m->properties.limits.timestampPeriod) / 1e6;
            m->timestampMask = validBits >= 64 ? ~0ull : (1ull << validBits) - 1;
        }
    }
    return true;
}

void Device::Impl::readGpuTimes(const Frame& frame, u32 base) {
    const auto ms = [&](u32 begin) {
        u64 q[2] = {};
        if (vkGetQueryPoolResults(device, queryPool, base + begin, 2, sizeof(q), q, sizeof(u64), VK_QUERY_RESULT_64_BIT) !=
            VK_SUCCESS)
            return 0.0f;
        const u64 ticks = (q[1] - q[0]) & timestampMask;
        return static_cast<f32>(static_cast<f64>(ticks) * timestampMsPerTick);
    };
    GpuTimes t;
    t.valid = true;
    t.frameMs = ms(0);
    for (u32 i = 0; i < kMaxGpuTimers; ++i)
        if (frame.timerMask & (1u << i)) t.timerMs[i] = ms(2 + 2 * i);
    gpuTimes = t;
}

const std::string& Device::initError() const {
    static const std::string none;
    return m ? m->initError : none;
}

void Device::shutdown() {
    if (!m) return;
    if (m->device) {
        vkDeviceWaitIdle(m->device);
        for (VkPipeline p : m->pipelines) vkDestroyPipeline(m->device, p, nullptr);
        for (Impl::TextureEntry& t : m->textures) {
            vkDestroyImageView(m->device, t.view, nullptr);
            vmaDestroyImage(m->allocator, t.image, t.allocation);
        }
        for (Impl::BufferEntry& b : m->buffers) vmaDestroyBuffer(m->allocator, b.buffer, b.allocation);
        if (m->uploadBuffer) vmaDestroyBuffer(m->allocator, m->uploadBuffer, m->uploadAllocation);
        for (Impl::Frame& f : m->frames) {
            vkDestroyCommandPool(m->device, f.pool, nullptr);
            vkDestroyFence(m->device, f.fence, nullptr);
            vkDestroySemaphore(m->device, f.imageAcquired, nullptr);
        }
        vkDestroyCommandPool(m->device, m->immediatePool, nullptr);
        if (m->queryPool) vkDestroyQueryPool(m->device, m->queryPool, nullptr);
        vkDestroyPipelineLayout(m->device, m->pipelineLayout, nullptr);
        vkDestroyDescriptorPool(m->device, m->descriptorPool, nullptr);
        vkDestroyDescriptorSetLayout(m->device, m->setLayout, nullptr);
        vkDestroySampler(m->device, m->linearClamp, nullptr);
        vkDestroySampler(m->device, m->linearWrap, nullptr);
        m->destroySwapchain();
        if (m->allocator) vmaDestroyAllocator(m->allocator);
        vkDestroyDevice(m->device, nullptr);
    }
    if (m->surface) vkDestroySurfaceKHR(m->instance, m->surface, nullptr);
    if (m->messenger) vkDestroyDebugUtilsMessengerEXT(m->instance, m->messenger, nullptr);
    if (m->instance) vkDestroyInstance(m->instance, nullptr);
    delete m;
    m = nullptr;
}

void Device::resize(u32 width, u32 height) {
    if (!m || width == 0 || height == 0 || (width == m->width && height == m->height)) return;
    vkDeviceWaitIdle(m->device);
    m->width = width;
    m->height = height;
    m->createSwapchain();
}

u32 Device::width() const { return m ? m->width : 0; }
u32 Device::height() const { return m ? m->height : 0; }

const GpuInfo& Device::gpu() const {
    static const GpuInfo empty{};
    return m ? m->gpu : empty;
}

Buffer Device::createBuffer(BufferUsage usage, const void* data, u64 bytes) {
    Impl::BufferEntry entry;
    entry.usage = usage;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = bytes;
    bci.usage = (usage == BufferUsage::Vertex ? VK_BUFFER_USAGE_VERTEX_BUFFER_BIT : VK_BUFFER_USAGE_INDEX_BUFFER_BIT) |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VK(vmaCreateBuffer(m->allocator, &bci, &aci, &entry.buffer, &entry.allocation, nullptr));

    VkBuffer staging = VK_NULL_HANDLE;
    VmaAllocation stagingAllocation = VK_NULL_HANDLE;
    VmaAllocationInfo info{};
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VK(vmaCreateBuffer(m->allocator, &bci, &aci, &staging, &stagingAllocation, &info));
    std::memcpy(info.pMappedData, data, bytes);
    VK(vmaFlushAllocation(m->allocator, stagingAllocation, 0, VK_WHOLE_SIZE));
    VkCommandBuffer cmd = m->beginImmediate();
    const VkBufferCopy region{0, 0, bytes};
    vkCmdCopyBuffer(cmd, staging, entry.buffer, 1, &region);
    m->endImmediate(cmd);
    vmaDestroyBuffer(m->allocator, staging, stagingAllocation);
    m->buffers.push_back(entry);
    return {static_cast<u32>(m->buffers.size())};
}

Texture Device::createTexture(u32 width, u32 height, Format format) {
    Impl::TextureEntry entry;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = toVk(format);
    ici.extent = {width, height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VK(vmaCreateImage(m->allocator, &ici, &aci, &entry.image, &entry.allocation, nullptr));
    entry.view = createView(m->device, entry.image, ici.format, VK_IMAGE_ASPECT_COLOR_BIT);

    // DX12처럼 내용 0, 셰이더가 읽는 배치로 시작한다
    VkCommandBuffer cmd = m->beginImmediate();
    imageBarrier(cmd, entry.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkClearColorValue zero{};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, entry.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
    imageBarrier(cmd, entry.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    m->endImmediate(cmd);

    entry.slot = m->allocTextureSlot();
    const VkDescriptorImageInfo imageInfo{VK_NULL_HANDLE, entry.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = m->descriptorSet;
    write.dstBinding = 1;
    write.dstArrayElement = entry.slot;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(m->device, 1, &write, 0, nullptr);
    m->textures.push_back(entry);
    return {static_cast<u32>(m->textures.size())};
}

Texture Device::createTexture(const TextureInit& init) {
    Impl::TextureEntry entry;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = toVk(init.format);
    ici.extent = {init.width, init.height, 1};
    ici.mipLevels = init.mipLevels;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VK(vmaCreateImage(m->allocator, &ici, &aci, &entry.image, &entry.allocation, nullptr));
    entry.view = createView(m->device, entry.image, ici.format, VK_IMAGE_ASPECT_COLOR_BIT, init.mipLevels);

    // 밉을 빈틈 없이 차례로 담은 스테이징 버퍼 하나 → 밉마다 복사 (오프셋은 블록·텍셀 크기의 배수)
    u64 total = 0;
    std::vector<VkBufferImageCopy> regions(init.mipLevels);
    for (u32 mip = 0; mip < init.mipLevels; ++mip) {
        const u32 w = std::max(1u, init.width >> mip), h = std::max(1u, init.height >> mip);
        VkBufferImageCopy& r = regions[mip];
        r.bufferOffset = total;
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1};
        r.imageExtent = {w, h, 1};
        total += textureMipBytes(init.format, w, h);
    }
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = total;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo sai{};
    sai.usage = VMA_MEMORY_USAGE_AUTO;
    sai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VkBuffer staging = VK_NULL_HANDLE;
    VmaAllocation stagingAllocation = VK_NULL_HANDLE;
    VmaAllocationInfo stagingInfo{};
    VK(vmaCreateBuffer(m->allocator, &bci, &sai, &staging, &stagingAllocation, &stagingInfo));
    std::memcpy(stagingInfo.pMappedData, init.data, total);
    VK(vmaFlushAllocation(m->allocator, stagingAllocation, 0, VK_WHOLE_SIZE));

    VkCommandBuffer cmd = m->beginImmediate();
    imageBarrier(cmd, entry.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 init.mipLevels);
    vkCmdCopyBufferToImage(cmd, staging, entry.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, init.mipLevels, regions.data());
    imageBarrier(cmd, entry.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, init.mipLevels);
    m->endImmediate(cmd);
    vmaDestroyBuffer(m->allocator, staging, stagingAllocation);

    entry.slot = m->allocTextureSlot();
    const VkDescriptorImageInfo imageInfo{VK_NULL_HANDLE, entry.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = m->descriptorSet;
    write.dstBinding = 1;
    write.dstArrayElement = entry.slot;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(m->device, 1, &write, 0, nullptr);
    m->textures.push_back(entry);
    return {static_cast<u32>(m->textures.size())};
}

u32 Device::textureIndex(Texture texture) const { return m->textures[texture.id - 1].slot; }

Pipeline Device::createPipeline(const PipelineDesc& desc) {
    auto module = [&](const ShaderCode& code) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = code.size;
        ci.pCode = static_cast<const u32*>(code.data);
        VkShaderModule mod = VK_NULL_HANDLE;
        VK(vkCreateShaderModule(m->device, &ci, nullptr, &mod));
        return mod;
    };
    const VkShaderModule vs = module(desc.vs), ps = module(desc.ps);
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "VSMain"};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, ps, "PSMain"};

    std::vector<VkVertexInputAttributeDescription> attributes;
    for (size_t i = 0; i < desc.vertexAttributes.size(); ++i) {
        const VertexAttribute& a = desc.vertexAttributes[i];
        attributes.push_back({static_cast<u32>(i), 0,
                              a.format == VertexFormat::Float3     ? VK_FORMAT_R32G32B32_SFLOAT
                              : a.format == VertexFormat::Half2    ? VK_FORMAT_R16G16_SFLOAT
                              : a.format == VertexFormat::Uint16x2 ? VK_FORMAT_R16G16_UINT
                                                                   : VK_FORMAT_R8G8B8A8_UNORM,
                              a.offset});
    }
    const VkVertexInputBindingDescription binding{0, desc.vertexStride, VK_VERTEX_INPUT_RATE_VERTEX};
    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    if (!attributes.empty()) {
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount = static_cast<u32>(attributes.size());
        vertexInput.pVertexAttributeDescriptions = attributes.data();
    }
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = desc.wireframe && m->fillModeNonSolid ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    raster.cullMode = desc.cullBack ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_CLOCKWISE; // 뷰포트를 뒤집어(setViewport) 화면 좌표가 DX12와 같다
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    const bool scene = desc.target == Target::Scene;
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    if (scene && desc.depthTest) {
        depth.depthTestEnable = VK_TRUE;
        depth.depthWriteEnable = VK_TRUE;
        depth.depthCompareOp = VK_COMPARE_OP_GREATER; // reversed-Z
    }
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (desc.additiveBlend) {
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstColorBlendFactor = blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.colorBlendOp = blend.alphaBlendOp = VK_BLEND_OP_ADD;
    } else if (desc.premultipliedBlend) {
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstColorBlendFactor = blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.colorBlendOp = blend.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo colorBlend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blend;
    const VkDynamicState dynamics[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamics;
    const VkFormat colorFormat = scene ? m->sceneFormat : m->swapchainFormat;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &colorFormat;
    rendering.depthAttachmentFormat = scene ? kDepthFormat : VK_FORMAT_UNDEFINED;

    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering};
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = &vertexInput;
    ci.pInputAssemblyState = &assembly;
    ci.pViewportState = &viewport;
    ci.pRasterizationState = &raster;
    ci.pMultisampleState = &multisample;
    ci.pDepthStencilState = &depth;
    ci.pColorBlendState = &colorBlend;
    ci.pDynamicState = &dynamic;
    ci.layout = m->pipelineLayout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VK(vkCreateGraphicsPipelines(m->device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline));
    vkDestroyShaderModule(m->device, vs, nullptr);
    vkDestroyShaderModule(m->device, ps, nullptr);
    m->pipelines.push_back(pipeline);
    return {static_cast<u32>(m->pipelines.size())};
}

bool Device::recreatePipeline(Pipeline pipeline, const PipelineDesc& desc) {
    if (!m || m->inFrame || !pipeline || pipeline.id > m->pipelines.size()) return false;
    vkDeviceWaitIdle(m->device);
    createPipeline(desc); // 끝에 붙은 새 것을 원래 자리로
    vkDestroyPipeline(m->device, m->pipelines[pipeline.id - 1], nullptr);
    m->pipelines[pipeline.id - 1] = m->pipelines.back();
    m->pipelines.pop_back();
    return true;
}

bool Device::beginFrame() {
    if (!m || m->width == 0 || m->height == 0) return false;
    if (m->swapchainDirty || !m->swapchain) {
        vkDeviceWaitIdle(m->device);
        if (!m->createSwapchain()) return false;
    }
    Impl::Frame& frame = m->frames[m->frameIndex];
    VK(vkWaitForFences(m->device, 1, &frame.fence, VK_TRUE, UINT64_MAX));
    const VkResult acquired =
        vkAcquireNextImageKHR(m->device, m->swapchain, UINT64_MAX, frame.imageAcquired, VK_NULL_HANDLE, &m->imageIndex);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        m->swapchainDirty = true;
        return false;
    }
    if (acquired != VK_SUBOPTIMAL_KHR) VK(acquired);
    if (frame.timed) m->readGpuTimes(frame, m->queryBase());
    frame.timed = false;
    frame.timerMask = 0;
    VK(vkResetFences(m->device, 1, &frame.fence));
    VK(vkResetCommandPool(m->device, frame.pool, 0));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK(vkBeginCommandBuffer(frame.cmd, &bi));
    if (m->queryPool) {
        vkCmdResetQueryPool(frame.cmd, m->queryPool, m->queryBase(), kQueriesPerFrame);
        vkCmdWriteTimestamp2(frame.cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, m->queryPool, m->queryBase());
    }
    m->uploadOffset = m->frameIndex * kUploadBytesPerFrame;
    m->uploadLimit = m->uploadOffset + kUploadBytesPerFrame;
    m->imageIsTarget = false;
    m->bound = Impl::Bound::None;
    m->inFrame = true;
    return true;
}

CommandList& Device::cmd() { return m->cmd; }

u8* Device::allocUpload(u64 bytes, u32 alignment, u64* offset) {
    const u64 aligned = (m->uploadOffset + alignment - 1) & ~static_cast<u64>(alignment - 1);
    if (aligned + bytes > m->uploadLimit) return nullptr;
    m->uploadOffset = aligned + bytes;
    *offset = aligned;
    return m->uploadCpu + aligned;
}

u32 Device::uploadBufferIndex() const { return 0; }

u32 Device::copyPitch(u32 width, u32 bytesPerPixel) const { return width * bytesPerPixel; }

u32 Device::copyAlignment() const {
    return std::max<u32>(16, static_cast<u32>(m->properties.limits.optimalBufferCopyOffsetAlignment));
}

void Device::endFrame(bool vsync, Capture* capture) {
    if (!m || !m->inFrame) return;
    m->endRendering();
    VkCommandBuffer cmd = m->current();
    const Impl::SwapImage& s = m->swapImages[m->imageIndex];
    const VkImageLayout current = m->imageIsTarget ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;

    VkBuffer readback = VK_NULL_HANDLE;
    VmaAllocation readbackAllocation = VK_NULL_HANDLE;
    VmaAllocationInfo readbackInfo{};
    if (capture) {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = static_cast<u64>(m->width) * m->height * 4;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VK(vmaCreateBuffer(m->allocator, &bci, &aci, &readback, &readbackAllocation, &readbackInfo));
        imageBarrier(cmd, s.image, VK_IMAGE_ASPECT_COLOR_BIT, current, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {m->width, m->height, 1};
        vkCmdCopyImageToBuffer(cmd, s.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &region);
        imageBarrier(cmd, s.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    } else {
        imageBarrier(cmd, s.image, VK_IMAGE_ASPECT_COLOR_BIT, current, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    }
    Impl::Frame& frame = m->frames[m->frameIndex];
    if (m->queryPool) {
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, m->queryPool, m->queryBase() + 1);
        frame.timed = true;
    }
    VK(vkEndCommandBuffer(cmd));

    VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait.semaphore = frame.imageAcquired;
    wait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.semaphore = s.renderFinished;
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = cmd;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.waitSemaphoreInfoCount = 1;
    submit.pWaitSemaphoreInfos = &wait;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cbi;
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos = &signal;
    VK(vkQueueSubmit2(m->queue, 1, &submit, frame.fence));

    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &s.renderFinished;
    present.swapchainCount = 1;
    present.pSwapchains = &m->swapchain;
    present.pImageIndices = &m->imageIndex;
    const VkResult presented = vkQueuePresentKHR(m->queue, &present);
    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) m->swapchainDirty = true;
    else VK(presented);
    if (vsync != m->swapchainVsync) { // 프레젠트 방식은 스왑체인을 다시 만들어야 바뀐다
        m->swapchainVsync = vsync;
        m->swapchainDirty = true;
    }

    if (readback) {
        VK(vkWaitForFences(m->device, 1, &frame.fence, VK_TRUE, UINT64_MAX));
        VK(vmaInvalidateAllocation(m->allocator, readbackAllocation, 0, VK_WHOLE_SIZE));
        capture->width = m->width;
        capture->height = m->height;
        capture->rgba.resize(static_cast<size_t>(m->width) * m->height * 4);
        const u8* src = static_cast<const u8*>(readbackInfo.pMappedData);
        for (size_t i = 0; i < capture->rgba.size(); i += 4) {
            capture->rgba[i + 0] = src[i + (m->swapchainBgra ? 2 : 0)];
            capture->rgba[i + 1] = src[i + 1];
            capture->rgba[i + 2] = src[i + (m->swapchainBgra ? 0 : 2)];
            capture->rgba[i + 3] = 255;
        }
        vmaDestroyBuffer(m->allocator, readback, readbackAllocation);
    }
    m->frameIndex = (m->frameIndex + 1) % kFramesInFlight;
    m->inFrame = false;
}

void Device::waitIdle() {
    if (m && m->device) vkDeviceWaitIdle(m->device);
}

const GpuTimes& Device::gpuTimes() const {
    static const GpuTimes none;
    return m ? m->gpuTimes : none;
}

GpuMemory Device::gpuMemory() const {
    if (!m || !m->allocator) return {};
    VmaBudget budgets[VK_MAX_MEMORY_HEAPS] = {};
    vmaGetHeapBudgets(m->allocator, budgets);
    const VkPhysicalDeviceMemoryProperties* props = nullptr;
    vmaGetMemoryProperties(m->allocator, &props);
    GpuMemory out; // DX12의 LOCAL 세그먼트처럼 장치 로컬 힙만 (UMA는 시스템 메모리 힙이 장치 로컬)
    for (u32 i = 0; i < props->memoryHeapCount; ++i) {
        if (!(props->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)) continue;
        out.used += budgets[i].usage;
        out.budget += budgets[i].budget;
    }
    return out;
}

// ---------------------------------------------------------------- CommandList

void CommandList::beginScene(const f32 clearColor[4], f32 clearDepth) {
    m_device->impl()->beginRendering(Device::Impl::Bound::Scene, clearColor, clearDepth);
}

void CommandList::beginOverlay() { m_device->impl()->beginRendering(Device::Impl::Bound::Overlay, nullptr, 0.0f); }

void CommandList::beginTimer(u32 timer) {
    Device::Impl& d = *m_device->impl();
    if (!d.queryPool || timer >= kMaxGpuTimers) return;
    d.frames[d.frameIndex].timerMask |= 1u << timer;
    vkCmdWriteTimestamp2(d.current(), VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, d.queryPool, d.queryBase() + 2 + 2 * timer);
}

void CommandList::endTimer(u32 timer) {
    Device::Impl& d = *m_device->impl();
    if (!d.queryPool || timer >= kMaxGpuTimers) return;
    vkCmdWriteTimestamp2(d.current(), VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, d.queryPool, d.queryBase() + 3 + 2 * timer);
}

void CommandList::setViewport(f32 x, f32 y, f32 width, f32 height) {
    VkCommandBuffer cmd = m_device->impl()->current();
    // 높이를 음수로 뒤집어 클립 공간 y가 DX12처럼 위를 향하게 한다 (Vulkan 1.1 기본)
    const VkViewport viewport{x, y + height, width, -height, 0.0f, 1.0f};
    const VkRect2D scissor{{static_cast<i32>(x), static_cast<i32>(y)}, {static_cast<u32>(width), static_cast<u32>(height)}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}

void CommandList::setPipeline(Pipeline pipeline) {
    Device::Impl* m = m_device->impl();
    VkCommandBuffer cmd = m->current();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m->pipelines[pipeline.id - 1]);
    // 디버그 UI가 자기 세트를 묶었을 수 있으므로 매번 다시 묶는다
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m->pipelineLayout, 0, 1, &m->descriptorSet, 0, nullptr);
}

void CommandList::pushConstants(const void* data, u32 bytes) {
    Device::Impl* m = m_device->impl();
    vkCmdPushConstants(m->current(), m->pipelineLayout, VK_SHADER_STAGE_ALL_GRAPHICS, 0, bytes, data);
}

void CommandList::setVertexBuffer(Buffer buffer, u32) {
    Device::Impl* m = m_device->impl();
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(m->current(), 0, 1, &m->buffers[buffer.id - 1].buffer, &offset);
}

void CommandList::setIndexBuffer(Buffer buffer) {
    Device::Impl* m = m_device->impl();
    const Device::Impl::BufferEntry& b = m->buffers[buffer.id - 1];
    vkCmdBindIndexBuffer(m->current(), b.buffer, 0,
                         b.usage == BufferUsage::Index16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
}

void CommandList::drawIndexed(u32 indexCount, u32 instanceCount) {
    vkCmdDrawIndexed(m_device->impl()->current(), indexCount, instanceCount, 0, 0, 0);
}

void CommandList::draw(u32 vertexCount, u32 instanceCount) {
    vkCmdDraw(m_device->impl()->current(), vertexCount, instanceCount, 0, 0);
}

void CommandList::textureBarrier(Texture texture, TextureState before, TextureState after) {
    Device::Impl* m = m_device->impl();
    m->endRendering(); // 배리어·복사는 그리기 대상 밖에서
    auto layout = [](TextureState s) {
        return s == TextureState::CopyDst ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    };
    imageBarrier(m->current(), m->textures[texture.id - 1].image, VK_IMAGE_ASPECT_COLOR_BIT, layout(before), layout(after));
}

void CommandList::copyToTexture(Texture texture, u32 x, u32 y, u32 width, u32 height, u64 uploadOffset, u32 rowPitch) {
    Device::Impl* m = m_device->impl();
    m->endRendering();
    const u32 bpp = rowPitch / width;
    VkBufferImageCopy region{};
    region.bufferOffset = uploadOffset;
    region.bufferRowLength = rowPitch / bpp;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageOffset = {static_cast<i32>(x), static_cast<i32>(y), 0};
    region.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(m->current(), m->uploadBuffer, m->textures[texture.id - 1].image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

// ---------------------------------------------------------------- GPU 조사

std::string gpuMinimumError(const GpuInfo& info) { return info.unsupported; }

bool queryGpu(GpuInfo& out, std::string* error) {
    if (volkInitialize() != VK_SUCCESS) {
        if (error) *error = "Vulkan is not available (vulkan-1.dll not found)";
        return false;
    }
    bool debugUtils = false;
    VkInstance instance = volkGetInstanceVersion() >= VK_API_VERSION_1_3 ? createInstance(false, false, &debugUtils)
                                                                          : VK_NULL_HANDLE;
    if (!instance) {
        if (error) *error = "Vulkan 1.3 instance could not be created";
        return false;
    }
    std::string missing;
    VkPhysicalDevice physical = pickDevice(instance, VK_NULL_HANDLE, &missing);
    if (physical) {
        describeGpu(physical, out);
        out.unsupported = missing.empty() ? std::string() : out.adapter + " lacks " + missing + ". Update the GPU driver.";
    }
    vkDestroyInstance(instance, nullptr);
    if (!physical && error) *error = "No Vulkan capable GPU found";
    return physical != VK_NULL_HANDLE;
}

} // namespace mealkit::rhi
