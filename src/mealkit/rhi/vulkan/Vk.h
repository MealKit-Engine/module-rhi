#pragma once

// Vulkan backend internals (only rhi/vulkan/ files include this). Vulkan 1.3 + descriptor indexing + dynamic rendering + synchronization2.
// Built without the SDK: headers in third_party/vulkan-headers, the loader (vulkan-1.dll) loaded at runtime by volk, memory via VMA.
#include "mealkit/rhi/Rhi.h"

#include <volk.h>
#include <vk_mem_alloc.h>

namespace mealkit::rhi {

constexpr u32 kMaxBuffers = 16;    // g_buffers[] (binding 0)
constexpr u32 kMaxTextures = 4096; // g_textures[] (binding 1)
constexpr u64 kUploadBytesPerFrame = 8ull << 20;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
constexpr VkFormat kHdrFormat = VK_FORMAT_B10G11R11_UFLOAT_PACK32; // scene/bloom targets (Target::Scene/Hdr, linear)
constexpr u32 kQueriesPerFrame = 2 + 2 * kMaxGpuTimers; // frame start/end + start/end per section

void checkVk(VkResult result, const char* expr, const char* file, int line);
#define MEALKIT_VK(expr) ::mealkit::rhi::checkVk((expr), #expr, __FILE__, __LINE__)

struct Device::Impl {
    void* hwnd = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties{};
    VkDevice device = VK_NULL_HANDLE;
    u32 queueFamily = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VmaAllocator allocator = VK_NULL_HANDLE;
    u32 apiVersion = 0;

    // Swap chain: one UNORM view (Target::Overlay, shaders write sRGB values). Depth is in the scene target
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapchainFormat = VK_FORMAT_UNDEFINED; // UNORM (Ldr targets use this format too)
    bool swapchainBgra = false;
    struct SwapImage {
        VkImage image = VK_NULL_HANDLE;
        VkImageView overlayView = VK_NULL_HANDLE;
        VkSemaphore renderFinished = VK_NULL_HANDLE; // present waits on it (per image)
    };
    std::vector<SwapImage> swapImages;
    bool swapchainVsync = true;
    bool swapchainDirty = false;
    bool immediateSupported = false;
    bool mailboxSupported = false;

    // bindless: one set (buffer array, texture array, static samplers), the same layout for all pipelines
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkSampler linearClamp = VK_NULL_HANDLE;
    VkSampler linearWrap = VK_NULL_HANDLE; // model textures (repeat, anisotropy)
    std::vector<u32> freeTextureSlots;

    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore imageAcquired = VK_NULL_HANDLE;
        bool timed = false; // timestamps were written and submitted (read after the fence)
        u32 timerMask = 0;  // sections used this frame
    } frames[Device::kFramesInFlight];
    CommandList cmd;
    VkCommandPool immediatePool = VK_NULL_HANDLE; // copies at load time (waits for the GPU)

    VkBuffer uploadBuffer = VK_NULL_HANDLE; // one section per frame, persistently mapped
    VmaAllocation uploadAllocation = VK_NULL_HANDLE;
    u8* uploadCpu = nullptr;
    u64 uploadOffset = 0;
    u64 uploadLimit = 0;

    struct BufferEntry {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        BufferUsage usage = BufferUsage::Vertex;
    };
    std::vector<BufferEntry> buffers;
    struct TextureEntry {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        u32 slot = 0;
    };
    std::vector<TextureEntry> textures;
    // Offscreen targets: color image (a slot in textures, same view for read and draw), depth only for TargetKind::Scene (per target)
    struct TargetEntry {
        u32 texture = 0; // textures index (Texture::id)
        VkImage depthImage = VK_NULL_HANDLE;
        VmaAllocation depthAllocation = VK_NULL_HANDLE;
        VkImageView depthView = VK_NULL_HANDLE;
        u32 width = 0, height = 0;
        bool depthOnly = false; // createDepthTarget: the depth image is textures[texture - 1] itself
    };
    std::vector<TargetEntry> targets;
    std::vector<std::pair<u32, u64>> imguiTextures; // Texture::id → debug UI descriptor set (imguiTextureId)
    std::vector<VkPipeline> pipelines;

    // GPU time: kQueriesPerFrame slots per frame
    VkQueryPool queryPool = VK_NULL_HANDLE;
    f64 timestampMsPerTick = 0.0;
    u64 timestampMask = ~0ull; // timestampValidBits
    GpuTimes gpuTimes;
    bool memoryBudget = false;
    bool fillModeNonSolid = false;
    bool samplerAnisotropy = false; // wireframe pipelines (filled faces if absent) // VK_EXT_memory_budget (if absent, VMA estimates from its own allocations)

    u32 width = 0, height = 0;
    u32 frameIndex = 0;
    u32 imageIndex = 0;
    bool inFrame = false;
    enum class Bound : u8 { None, Overlay, Target } bound = Bound::None;
    bool imageIsTarget = false; // the back buffer was transitioned to the render target layout this frame
    GpuInfo gpu;
    std::string initError;

    VkCommandBuffer current() const { return frames[frameIndex].cmd; }
    u32 queryBase() const { return frameIndex * kQueriesPerFrame; }
    void readGpuTimes(const Frame& frame, u32 base);
    bool createSwapchain();
    void destroySwapchain();
    void endRendering();
    void beginOverlay();
    VkCommandBuffer beginImmediate();
    void endImmediate(VkCommandBuffer cmd);
    u32 allocTextureSlot();
};

} // namespace mealkit::rhi
