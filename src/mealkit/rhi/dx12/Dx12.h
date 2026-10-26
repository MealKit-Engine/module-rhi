#pragma once

// DX12 백엔드 내부 (rhi/dx12/ 파일만 include한다)
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
constexpr u64 kUploadBytesPerFrame = 8ull << 20; // 인스턴스 약 17만 개분
constexpr u32 kMaxBuffers = 16;     // g_buffers[] 칸 (서술자 힙 앞쪽)
constexpr u32 kMaxTextures = 4096;  // g_textures[] 칸 (그 뒤, 디버그 UI 텍스처도 여기서)
constexpr DXGI_FORMAT kSwapchainFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT kSceneFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; // 셰이더는 선형으로 쓰고 하드웨어가 감마 인코딩
constexpr DXGI_FORMAT kOverlayFormat = DXGI_FORMAT_R8G8B8A8_UNORM;    // UI는 이미 sRGB 색이므로 변환하지 않는다
constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;
constexpr u32 kQueriesPerFrame = 2 + 2 * kMaxGpuTimers; // 프레임 처음·끝 + 구간마다 처음·끝

void checkHr(HRESULT hr, const char* expr, const char* file, int line);
#define MEALKIT_HR(expr) ::mealkit::rhi::checkHr((expr), #expr, __FILE__, __LINE__)

struct Device::Impl {
    HWND hwnd = nullptr;
    ComPtr<IDXGIFactory6> factory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIAdapter3> adapter3; // QueryVideoMemoryInfo (없으면 GPU 메모리 0)
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;

    ComPtr<IDXGISwapChain3> swapchain;
    HANDLE frameLatencyWaitable = nullptr;
    UINT swapchainFlags = 0;
    bool tearingSupported = false;

    ComPtr<ID3D12DescriptorHeap> rtvHeap; // 백버퍼마다 sRGB 뷰 + 오버레이 뷰
    ComPtr<ID3D12DescriptorHeap> dsvHeap;
    UINT rtvStride = 0;
    ComPtr<ID3D12Resource> backBuffers[kBackBufferCount];
    ComPtr<ID3D12Resource> depthBuffer;

    // bindless: 셰이더가 보는 서술자 힙 하나 (버퍼 kMaxBuffers칸 + 텍스처 kMaxTextures칸)
    ComPtr<ID3D12DescriptorHeap> srvHeap;
    UINT srvStride = 0;
    std::vector<u32> freeTextureSlots;

    struct Frame {
        ComPtr<ID3D12CommandAllocator> allocator;
        u64 fenceValue = 0;
        bool timed = false; // 타임스탬프를 쓰고 제출했다 (fence 뒤에 읽는다)
        u32 timerMask = 0;  // 이 프레임에 쓴 구간
    } frames[Device::kFramesInFlight];
    ComPtr<ID3D12GraphicsCommandList> list;
    CommandList cmd;

    ComPtr<ID3D12Fence> fence;
    HANDLE fenceEvent = nullptr;
    u64 fenceCounter = 0;

    ComPtr<ID3D12Resource> uploadBuffer; // 프레임마다 한 구간, 계속 매핑 (UMA에서는 복사 없음)
    u8* uploadCpu = nullptr;
    u64 uploadOffset = 0;
    u64 uploadLimit = 0;

    ComPtr<ID3D12RootSignature> rootSignature; // 모든 파이프라인이 같이 쓴다
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
    // 오프스크린 대상: 색은 TYPELESS 자원 하나에 sRGB RTV + UNORM SRV(textures의 한 칸), 깊이는 대상마다 따로
    struct TargetEntry {
        u32 texture = 0; // textures 번호 (Texture::id)
        ComPtr<ID3D12Resource> depth;
        ComPtr<ID3D12DescriptorHeap> rtvHeap, dsvHeap;
        bool depthOnly = false; // createDepthTarget: depth가 곧 textures[texture - 1] (R32 SRV)
    };
    std::vector<TargetEntry> targets;
    std::vector<ComPtr<ID3D12PipelineState>> pipelines;

    // GPU 시간: 프레임마다 kQueriesPerFrame칸, 결과는 계속 매핑한 readback 버퍼로 모은다
    ComPtr<ID3D12QueryHeap> queryHeap;
    ComPtr<ID3D12Resource> queryReadback;
    const u64* queryCpu = nullptr;
    f64 timestampMsPerTick = 0.0;
    GpuTimes gpuTimes;

    u32 width = 0, height = 0;
    u32 frameIndex = 0;
    UINT backBufferIndex = 0;
    bool inFrame = false;
    bool backBufferIsTarget = false; // 이번 프레임에 PRESENT → RENDER_TARGET으로 바꿨다
    GpuInfo gpu;
    std::string initError;
#if defined(TRACY_ENABLE)
    tracy::D3D12QueueCtx* tracyGpu = nullptr; // GPU 시간 측정 (프로파일 빌드)
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
    u32 allocTextureSlot(); // 힙 안 번호 (kMaxBuffers 이상). 다 쓰면 치명적 오류
    void freeTextureSlot(u32 heapIndex) { freeTextureSlots.push_back(heapIndex); }
};

} // namespace mealkit::rhi
