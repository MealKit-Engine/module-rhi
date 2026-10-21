#pragma once

// Vulkan 백엔드 내부 (rhi/vulkan/ 파일만 include한다). Vulkan 1.3 + descriptor indexing + dynamic rendering + synchronization2.
// SDK 없이 빌드한다: 헤더는 third_party/vulkan-headers, 로더(vulkan-1.dll)는 volk가 실행 중에 불러온다, 메모리는 VMA.
#include "mealkit/rhi/Rhi.h"

#include <volk.h>
#include <vk_mem_alloc.h>

namespace mealkit::rhi {

constexpr u32 kMaxBuffers = 16;    // g_buffers[] (바인딩 0)
constexpr u32 kMaxTextures = 4096; // g_textures[] (바인딩 1)
constexpr u64 kUploadBytesPerFrame = 8ull << 20;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
constexpr u32 kQueriesPerFrame = 2 + 2 * kMaxGpuTimers; // 프레임 처음·끝 + 구간마다 처음·끝

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

    // 스왑체인: 이미지 하나에 뷰 두 개 (sRGB = 장면, UNORM = 오버레이, VK_KHR_swapchain_mutable_format)
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapchainFormat = VK_FORMAT_UNDEFINED; // UNORM
    VkFormat sceneFormat = VK_FORMAT_UNDEFINED;     // 같은 이미지의 SRGB
    bool swapchainBgra = false;
    struct SwapImage {
        VkImage image = VK_NULL_HANDLE;
        VkImageView sceneView = VK_NULL_HANDLE;
        VkImageView overlayView = VK_NULL_HANDLE;
        VkSemaphore renderFinished = VK_NULL_HANDLE; // 프레젠트가 기다린다 (이미지마다)
    };
    std::vector<SwapImage> swapImages;
    VkImage depthImage = VK_NULL_HANDLE;
    VmaAllocation depthAllocation = VK_NULL_HANDLE;
    VkImageView depthView = VK_NULL_HANDLE;
    bool swapchainVsync = true;
    bool swapchainDirty = false;
    bool immediateSupported = false;
    bool mailboxSupported = false;

    // bindless: 세트 하나 (버퍼 배열, 텍스처 배열, 고정 샘플러), 모든 파이프라인이 같은 레이아웃
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkSampler linearClamp = VK_NULL_HANDLE;
    std::vector<u32> freeTextureSlots;

    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore imageAcquired = VK_NULL_HANDLE;
        bool timed = false; // 타임스탬프를 쓰고 제출했다 (fence 뒤에 읽는다)
        u32 timerMask = 0;  // 이 프레임에 쓴 구간
    } frames[Device::kFramesInFlight];
    CommandList cmd;
    VkCommandPool immediatePool = VK_NULL_HANDLE; // 불러올 때 복사 (GPU를 기다린다)

    VkBuffer uploadBuffer = VK_NULL_HANDLE; // 프레임마다 한 구간, 계속 매핑
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
    std::vector<VkPipeline> pipelines;

    // GPU 시간: 프레임마다 kQueriesPerFrame칸
    VkQueryPool queryPool = VK_NULL_HANDLE;
    f64 timestampMsPerTick = 0.0;
    u64 timestampMask = ~0ull; // timestampValidBits
    GpuTimes gpuTimes;
    bool memoryBudget = false;
    bool fillModeNonSolid = false; // 와이어프레임 파이프라인 (없으면 면으로) // VK_EXT_memory_budget (없으면 VMA가 자기 할당으로 어림한다)

    u32 width = 0, height = 0;
    u32 frameIndex = 0;
    u32 imageIndex = 0;
    bool inFrame = false;
    enum class Bound : u8 { None, Scene, Overlay } bound = Bound::None;
    bool imageIsTarget = false; // 이번 프레임에 백버퍼를 그리기 대상 배치로 바꿨다
    GpuInfo gpu;
    std::string initError;

    VkCommandBuffer current() const { return frames[frameIndex].cmd; }
    u32 queryBase() const { return frameIndex * kQueriesPerFrame; }
    void readGpuTimes(const Frame& frame, u32 base);
    bool createSwapchain();
    void destroySwapchain();
    void endRendering();
    void beginRendering(Bound target, const f32* clearColor, f32 clearDepth);
    VkCommandBuffer beginImmediate();
    void endImmediate(VkCommandBuffer cmd);
    u32 allocTextureSlot();
};

} // namespace mealkit::rhi
