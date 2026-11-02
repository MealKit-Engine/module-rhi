#pragma once

// DX12 backend internals (only rhi/dx12/ files include this)
#include "mealkit/rhi/Rhi.h"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#if defined(TRACY_ENABLE)
#include <optional>
#include <tracy/TracyD3D12.hpp>
#endif

namespace mealkit::rhi {

using Microsoft::WRL::ComPtr;

constexpr u32 kBackBufferCount = 3;
constexpr u64 kUploadBytesPerFrame = 8ull << 20; // about 170k instances
constexpr u32 kMaxBuffers = 16;     // g_buffers[] slots (front of the descriptor heap)
constexpr u32 kMaxTextures = 4096;  // g_textures[] slots (after that; debug UI textures also come from here)
constexpr DXGI_FORMAT kSwapchainFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT kHdrFormat = DXGI_FORMAT_R11G11B10_FLOAT;   // scene/bloom targets (Target::Scene/Hdr, linear)
constexpr DXGI_FORMAT kOverlayFormat = DXGI_FORMAT_R8G8B8A8_UNORM; // back buffer/Ldr targets: shaders write sRGB values (post/UI)
constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;
constexpr u32 kQueriesPerFrame = 2 + 2 * kMaxGpuTimers; // frame start/end + start/end per section

void checkHr(HRESULT hr, const char* expr, const char* file, int line);
#define MEALKIT_HR(expr) ::mealkit::rhi::checkHr((expr), #expr, __FILE__, __LINE__)

struct Device::Impl {
    HWND hwnd = nullptr;
    ComPtr<IDXGIFactory6> factory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIAdapter3> adapter3; // QueryVideoMemoryInfo (GPU memory 0 if absent)
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    // Pipeline cache (rhi/PipelineCache.h): libraryData must outlive the library (it reads from that memory).
    // File = u32 count + the stored keys + the serialized library; only stored keys are loaded (a miss is a debug layer warning)
    ComPtr<ID3D12PipelineLibrary1> library;
    std::string libraryData, libraryPath;
    std::vector<u64> libraryKeys;
    bool libraryDirty = false;

    ComPtr<IDXGISwapChain3> swapchain;
    HANDLE frameLatencyWaitable = nullptr;
    UINT swapchainFlags = 0;
    bool tearingSupported = false;

    ComPtr<ID3D12DescriptorHeap> rtvHeap; // one overlay view per back buffer (depth is in the scene target)
    UINT rtvStride = 0;
    ComPtr<ID3D12Resource> backBuffers[kBackBufferCount];

    // bindless: one descriptor heap visible to shaders (kMaxBuffers buffer slots + kMaxTextures texture slots)
    ComPtr<ID3D12DescriptorHeap> srvHeap;
    UINT srvStride = 0;
    std::vector<u32> freeTextureSlots;

    struct Frame {
        ComPtr<ID3D12CommandAllocator> allocator;
        u64 fenceValue = 0;
        bool timed = false; // timestamps were written and submitted (read after the fence)
        u32 timerMask = 0;  // sections used this frame
    } frames[Device::kFramesInFlight];
    ComPtr<ID3D12GraphicsCommandList> list;
    CommandList cmd;

    ComPtr<ID3D12Fence> fence;
    HANDLE fenceEvent = nullptr;
    u64 fenceCounter = 0;

    ComPtr<ID3D12Resource> uploadBuffer; // one section per frame, persistently mapped (no copy on UMA)
    u8* uploadCpu = nullptr;
    u64 uploadOffset = 0;
    u64 uploadLimit = 0;

    ComPtr<ID3D12RootSignature> rootSignature; // shared by all pipelines
    struct BufferEntry {
        ComPtr<ID3D12Resource> resource;
        u64 bytes = 0;
        BufferUsage usage = BufferUsage::Vertex;
    };
    std::vector<BufferEntry> buffers;
    struct TextureEntry {
        ComPtr<ID3D12Resource> resource;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        u32 slot = 0;
    };
    std::vector<TextureEntry> textures;
    // Offscreen targets: RTV + SRV on the color (a slot in textures), depth only for TargetKind::Scene (per target)
    struct TargetEntry {
        u32 texture = 0; // textures index (Texture::id)
        ComPtr<ID3D12Resource> depth;
        ComPtr<ID3D12DescriptorHeap> rtvHeap, dsvHeap;
        bool depthOnly = false; // createDepthTarget: depth is textures[texture - 1] itself (R32 SRV)
    };
    std::vector<TargetEntry> targets;
    std::vector<ComPtr<ID3D12PipelineState>> pipelines;

    // GPU time: kQueriesPerFrame slots per frame, results collected into a persistently mapped readback buffer
    ComPtr<ID3D12QueryHeap> queryHeap;
    ComPtr<ID3D12Resource> queryReadback;
    const u64* queryCpu = nullptr;
    f64 timestampMsPerTick = 0.0;
    GpuTimes gpuTimes;

    u32 width = 0, height = 0;
    u32 frameIndex = 0;
    UINT backBufferIndex = 0;
    bool inFrame = false;
    bool backBufferIsTarget = false; // changed PRESENT → RENDER_TARGET this frame
    GpuInfo gpu;
    std::string initError;
#if defined(TRACY_ENABLE)
    tracy::D3D12QueueCtx* tracyGpu = nullptr; // GPU time measurement (profile builds)
    std::optional<tracy::D3D12ZoneScope> tracyFrameZone;
#endif

    u64 signal();
    void waitForFence(u64 value);
    void waitIdle() { waitForFence(signal()); }
    void createSwapchainResources();
    void releaseSwapchainResources();
    void createRootSignature();
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu(u32 index) const {
        return {srvHeap->GetCPUDescriptorHandleForHeapStart().ptr + static_cast<SIZE_T>(index) * srvStride};
    }
    D3D12_GPU_DESCRIPTOR_HANDLE srvGpu(u32 index) const {
        return {srvHeap->GetGPUDescriptorHandleForHeapStart().ptr + static_cast<u64>(index) * srvStride};
    }
    u32 queryBase() const { return frameIndex * kQueriesPerFrame; }
    void resolveQueries(u32 first, u32 count) {
        list->ResolveQueryData(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first, count, queryReadback.Get(), first * 8ull);
    }
    void readGpuTimes(const Frame& frame, u32 base);
    u32 allocTextureSlot(); // index within the heap (kMaxBuffers or more). Fatal error when exhausted
    void freeTextureSlot(u32 heapIndex) { freeTextureSlots.push_back(heapIndex); }
};

} // namespace mealkit::rhi
