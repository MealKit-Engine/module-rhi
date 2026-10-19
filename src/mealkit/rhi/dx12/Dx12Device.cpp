#include "mealkit/rhi/dx12/Dx12.h"

#include "mealkit/core/Log.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace mealkit::rhi {
namespace {

ID3D12Device* g_errorDevice = nullptr;

// DXGI 이름(UTF-16) → UTF-8 (BMP 밖 글자는 서로게이트 쌍)
std::string narrow(const wchar_t* w) {
    std::string out;
    for (; *w; ++w) {
        u32 c = static_cast<u16>(*w);
        if (c >= 0xD800 && c < 0xDC00 && w[1] >= 0xDC00 && w[1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (static_cast<u16>(w[1]) - 0xDC00);
            ++w;
        }
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

// 사용자 모드 드라이버 버전. 장치 관리자·제조사 사이트가 보여 주는 네 자리 번호와 같다.
std::string driverVersion(IDXGIAdapter1* adapter) {
    LARGE_INTEGER v{};
    if (FAILED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &v))) return {};
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", HIWORD(v.HighPart), LOWORD(v.HighPart), HIWORD(v.LowPart),
                  LOWORD(v.LowPart));
    return buf;
}

// 고성능 우선 순서로 D3D12 장치를 만들 수 있는 첫 하드웨어 어댑터.
ComPtr<IDXGIAdapter1> findAdapter(IDXGIFactory6* factory) {
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> candidate;
        if (factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&candidate)) ==
            DXGI_ERROR_NOT_FOUND)
            break;
        DXGI_ADAPTER_DESC1 desc;
        candidate->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (SUCCEEDED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr)))
            return candidate;
    }
    return nullptr;
}

void describeGpu(IDXGIAdapter1* adapter, ID3D12Device* device, GpuInfo& info) {
    DXGI_ADAPTER_DESC1 desc;
    adapter->GetDesc1(&desc);
    info.api = "Direct3D 12";
    info.adapter = narrow(desc.Description);
    info.driverVersion = driverVersion(adapter);
    info.vendorId = desc.VendorId;
    info.deviceId = desc.DeviceId;
    info.dedicatedVideoMemory = desc.DedicatedVideoMemory;
    info.sharedSystemMemory = desc.SharedSystemMemory;

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
                                        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D12_FEATURE_DATA_FEATURE_LEVELS fl{_countof(levels), levels, D3D_FEATURE_LEVEL_11_0};
    device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &fl, sizeof(fl));
    info.featureLevel = static_cast<u32>(fl.MaxSupportedFeatureLevel);

    // 런타임이 모르는 셰이더 모델을 물으면 실패하므로 높은 것부터 내려간다 (SDK 열거값이 없는 것은 숫자로).
    for (u32 model = 0x69; model >= 0x60; --model) {
        D3D12_FEATURE_DATA_SHADER_MODEL sm{static_cast<D3D_SHADER_MODEL>(model)};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm)))) {
            info.shaderModel = static_cast<u32>(sm.HighestShaderModel);
            break;
        }
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))))
        info.resourceBindingTier = static_cast<u32>(options.ResourceBindingTier);
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 options1{};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options1, sizeof(options1)))) {
        info.waveLaneCountMin = options1.WaveLaneCountMin;
        info.waveLaneCountMax = options1.WaveLaneCountMax;
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7{};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7, sizeof(options7))))
        info.meshShaderTier = static_cast<u32>(options7.MeshShaderTier);

    D3D12_FEATURE_DATA_ARCHITECTURE arch{};
    device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &arch, sizeof(arch));
    info.uma = arch.UMA != FALSE;
}

D3D12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES p{};
    p.Type = type;
    p.CreationNodeMask = 1;
    p.VisibleNodeMask = 1;
    return p;
}

D3D12_RESOURCE_DESC bufferDesc(u64 size) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = size;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_UNKNOWN;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return d;
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
}

D3D12_RESOURCE_STATES toD3d(TextureState s) {
    return s == TextureState::CopyDst ? D3D12_RESOURCE_STATE_COPY_DEST
                                      : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
}

DXGI_FORMAT toDxgi(Format f) { return f == Format::R8Unorm ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM; }

void __stdcall onDebugMessage(D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID,
                              LPCSTR description, void*) {
    if (severity <= D3D12_MESSAGE_SEVERITY_ERROR) MEALKIT_ERROR("[D3D12] %s", description);
    else if (severity == D3D12_MESSAGE_SEVERITY_WARNING) MEALKIT_WARN("[D3D12] %s", description);
}

} // namespace

void checkHr(HRESULT hr, const char* expr, const char* file, int line) {
    if (SUCCEEDED(hr)) return;
    HRESULT reason = S_OK;
    if (g_errorDevice && (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET))
        reason = g_errorDevice->GetDeviceRemovedReason();
    MEALKIT_FATAL("D3D12 call failed (hr=0x%08X, device removed reason=0x%08X)\n%s\n%s:%d", static_cast<unsigned>(hr),
                  static_cast<unsigned>(reason), expr, file, line);
}
#define HR MEALKIT_HR

// ---------------------------------------------------------------- Impl

u64 Device::Impl::signal() {
    ++fenceCounter;
    HR(queue->Signal(fence.Get(), fenceCounter));
    return fenceCounter;
}

void Device::Impl::waitForFence(u64 value) {
    if (fence->GetCompletedValue() < value) {
        HR(fence->SetEventOnCompletion(value, fenceEvent));
        WaitForSingleObject(fenceEvent, INFINITE);
    }
}

u32 Device::Impl::allocTextureSlot() {
    if (freeTextureSlots.empty()) MEALKIT_FATAL("Bindless texture slots exhausted (%u)", kMaxTextures);
    const u32 slot = freeTextureSlots.back();
    freeTextureSlots.pop_back();
    return slot;
}

void Device::Impl::createSwapchainResources() {
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (u32 i = 0; i < kBackBufferCount; ++i) {
        HR(swapchain->GetBuffer(i, IID_PPV_ARGS(&backBuffers[i])));
        D3D12_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = kSceneFormat;
        rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device->CreateRenderTargetView(backBuffers[i].Get(), &rd, {rtv.ptr + i * rtvStride});
        rd.Format = kOverlayFormat;
        device->CreateRenderTargetView(backBuffers[i].Get(), &rd, {rtv.ptr + (kBackBufferCount + i) * rtvStride});
    }

    D3D12_RESOURCE_DESC dd{};
    dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    dd.Width = width;
    dd.Height = height;
    dd.DepthOrArraySize = 1;
    dd.MipLevels = 1;
    dd.Format = kDepthFormat;
    dd.SampleDesc.Count = 1;
    dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = kDepthFormat;
    clear.DepthStencil.Depth = 0.0f;
    const D3D12_HEAP_PROPERTIES defaultHeap = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
    HR(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &dd, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                       &clear, IID_PPV_ARGS(&depthBuffer)));
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = kDepthFormat;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device->CreateDepthStencilView(depthBuffer.Get(), &dsv, dsvHeap->GetCPUDescriptorHandleForHeapStart());
}

void Device::Impl::releaseSwapchainResources() {
    for (auto& bb : backBuffers) bb.Reset();
    depthBuffer.Reset();
}

// 모든 파이프라인이 같이 쓰는 루트 시그니처: 루트 상수(b0) + 버퍼 표(t0, space1) + 텍스처 표(t0, space2) + 샘플러(s0)
void Device::Impl::createRootSignature() {
    D3D12_DESCRIPTOR_RANGE1 ranges[2] = {};
    for (u32 i = 0; i < 2; ++i) {
        ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[i].NumDescriptors = UINT_MAX; // 무한 배열 (바인딩 티어 3)
        ranges[i].BaseShaderRegister = 0;
        ranges[i].RegisterSpace = 1 + i;
        ranges[i].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE; // 쓰지 않는 칸은 비어 있어도 된다
    }
    D3D12_ROOT_PARAMETER1 params[3] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = Device::kPushConstantBytes / 4;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    for (u32 i = 0; i < 2; ++i) {
        params[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1 + i].DescriptorTable.NumDescriptorRanges = 1;
        params[1 + i].DescriptorTable.pDescriptorRanges = &ranges[i];
        params[1 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rs{};
    rs.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    rs.Desc_1_1.NumParameters = 3;
    rs.Desc_1_1.pParameters = params;
    rs.Desc_1_1.NumStaticSamplers = 1;
    rs.Desc_1_1.pStaticSamplers = &sampler;
    rs.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> blob, error;
    if (FAILED(D3D12SerializeVersionedRootSignature(&rs, &blob, &error)))
        MEALKIT_FATAL("Root signature: %s", error ? static_cast<const char*>(error->GetBufferPointer()) : "unknown error");
    HR(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rootSignature)));
}

// ---------------------------------------------------------------- Device

Device::~Device() { shutdown(); }

bool Device::init(const DeviceDesc& desc) {
    m = new Impl();
    m->hwnd = static_cast<HWND>(desc.window);
    m->width = desc.width;
    m->height = desc.height;
    m->cmd.m_device = this;

    UINT factoryFlags = 0;
    if (desc.debug) {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
            factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
            MEALKIT_INFO("D3D12 debug layer enabled");
        } else {
            MEALKIT_WARN("D3D12 debug layer unavailable (install the 'Graphics Tools' optional feature)");
        }
    }

    if (FAILED(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&m->factory)))) {
        m->initError = "DXGI 1.6 is required (Windows 10 1803 or later)";
        MEALKIT_ERROR("%s", m->initError.c_str());
        return false;
    }
    m->adapter = findAdapter(m->factory.Get());
    if (!m->adapter) {
        m->initError = "No Direct3D 12 capable GPU found";
        MEALKIT_ERROR("%s", m->initError.c_str());
        return false;
    }
    HR(D3D12CreateDevice(m->adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m->device)));
    g_errorDevice = m->device.Get();

    describeGpu(m->adapter.Get(), m->device.Get(), m->gpu);
    const GpuInfo& gpu = m->gpu;
    MEALKIT_INFO("GPU: %s | driver %s | feature level %u_%u | SM %u.%u | binding tier %u | %s memory",
                 gpu.adapter.c_str(), gpu.driverVersion.empty() ? "?" : gpu.driverVersion.c_str(),
                 gpu.featureLevel >> 12, (gpu.featureLevel >> 8) & 0xF, gpu.shaderModel >> 4, gpu.shaderModel & 0xF,
                 gpu.resourceBindingTier, gpu.uma ? "unified (UMA)" : "dedicated");
    m->initError = gpuMinimumError(gpu);
    if (!m->initError.empty()) {
        MEALKIT_ERROR("%s", m->initError.c_str());
        return false;
    }

    if (desc.debug) {
        ComPtr<ID3D12InfoQueue1> infoQueue;
        if (SUCCEEDED(m->device.As(&infoQueue))) {
            DWORD cookie = 0;
            infoQueue->RegisterMessageCallback(onDebugMessage, D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &cookie);
        }
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    HR(m->device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m->queue)));

    BOOL allowTearing = FALSE;
    if (SUCCEEDED(
            m->factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing))))
        m->tearingSupported = allowTearing != FALSE;

    m->swapchainFlags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
                        (m->tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u);
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = m->width;
    sd.Height = m->height;
    sd.Format = kSwapchainFormat;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = kBackBufferCount;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    sd.Flags = m->swapchainFlags;
    ComPtr<IDXGISwapChain1> swapchain1;
    HR(m->factory->CreateSwapChainForHwnd(m->queue.Get(), m->hwnd, &sd, nullptr, nullptr, &swapchain1));
    HR(m->factory->MakeWindowAssociation(m->hwnd, DXGI_MWA_NO_ALT_ENTER));
    HR(swapchain1.As(&m->swapchain));
    HR(m->swapchain->SetMaximumFrameLatency(kFramesInFlight));
    m->frameLatencyWaitable = m->swapchain->GetFrameLatencyWaitableObject();

    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.NumDescriptors = kBackBufferCount * 2; // sRGB 뷰 + 오버레이 뷰
    HR(m->device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&m->rtvHeap)));
    D3D12_DESCRIPTOR_HEAP_DESC dsvDesc{};
    dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvDesc.NumDescriptors = 1;
    HR(m->device->CreateDescriptorHeap(&dsvDesc, IID_PPV_ARGS(&m->dsvHeap)));
    m->rtvStride = m->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_DESCRIPTOR_HEAP_DESC srvDesc{};
    srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvDesc.NumDescriptors = kMaxBuffers + kMaxTextures;
    srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    HR(m->device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&m->srvHeap)));
    m->srvStride = m->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (u32 i = kMaxBuffers + kMaxTextures; i > kMaxBuffers; --i) m->freeTextureSlots.push_back(i - 1);

    for (auto& frame : m->frames)
        HR(m->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame.allocator)));
    HR(m->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m->frames[0].allocator.Get(), nullptr,
                                    IID_PPV_ARGS(&m->list)));
    HR(m->list->Close());

    HR(m->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m->fence)));
    m->fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // 업로드 링: 프레임마다 kUploadBytesPerFrame. 셰이더는 g_buffers[0](원시 버퍼 뷰)으로 읽는다
    const u64 ringBytes = kUploadBytesPerFrame * kFramesInFlight;
    const D3D12_HEAP_PROPERTIES uploadHeap = heapProperties(D3D12_HEAP_TYPE_UPLOAD);
    const D3D12_RESOURCE_DESC uploadDesc = bufferDesc(ringBytes);
    HR(m->device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
                                          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m->uploadBuffer)));
    const D3D12_RANGE noRead{0, 0};
    HR(m->uploadBuffer->Map(0, &noRead, reinterpret_cast<void**>(&m->uploadCpu)));
    D3D12_SHADER_RESOURCE_VIEW_DESC rv{};
    rv.Format = DXGI_FORMAT_R32_TYPELESS;
    rv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    rv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    rv.Buffer.NumElements = static_cast<UINT>(ringBytes / 4);
    rv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    m->device->CreateShaderResourceView(m->uploadBuffer.Get(), &rv, m->srvCpu(0));

    m->createRootSignature();
    m->createSwapchainResources();
#if defined(TRACY_ENABLE)
    m->tracyGpu = TracyD3D12Context(m->device.Get(), m->queue.Get());
#endif
    return true;
}

const std::string& Device::initError() const {
    static const std::string none;
    return m ? m->initError : none;
}

void Device::shutdown() {
    if (!m) return;
    if (m->queue && m->fence) m->waitIdle();
#if defined(TRACY_ENABLE)
    if (m->tracyGpu) TracyD3D12Destroy(m->tracyGpu);
#endif
    if (m->fenceEvent) CloseHandle(m->fenceEvent);
    if (m->frameLatencyWaitable) CloseHandle(m->frameLatencyWaitable);
    delete m;
    m = nullptr;
    g_errorDevice = nullptr;
}

void Device::resize(u32 width, u32 height) {
    if (!m || width == 0 || height == 0 || (width == m->width && height == m->height)) return;
    m->waitIdle();
    m->releaseSwapchainResources();
    HR(m->swapchain->ResizeBuffers(kBackBufferCount, width, height, DXGI_FORMAT_UNKNOWN, m->swapchainFlags));
    m->width = width;
    m->height = height;
    m->createSwapchainResources();
}

u32 Device::width() const { return m ? m->width : 0; }
u32 Device::height() const { return m ? m->height : 0; }

const GpuInfo& Device::gpu() const {
    static const GpuInfo empty{};
    return m ? m->gpu : empty;
}

Buffer Device::createBuffer(BufferUsage usage, const void* data, u64 bytes) {
    const D3D12_HEAP_PROPERTIES defaultHeap = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
    const D3D12_HEAP_PROPERTIES uploadHeap = heapProperties(D3D12_HEAP_TYPE_UPLOAD);
    const D3D12_RESOURCE_DESC desc = bufferDesc(bytes);

    Impl::BufferEntry entry;
    entry.bytes = bytes;
    entry.usage = usage;
    HR(m->device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
                                          nullptr, IID_PPV_ARGS(&entry.resource)));
    ComPtr<ID3D12Resource> staging;
    HR(m->device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                          nullptr, IID_PPV_ARGS(&staging)));
    void* mapped = nullptr;
    const D3D12_RANGE noRead{0, 0};
    HR(staging->Map(0, &noRead, &mapped));
    std::memcpy(mapped, data, bytes);
    staging->Unmap(0, nullptr);

    ComPtr<ID3D12CommandAllocator> allocator;
    HR(m->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList> list;
    HR(m->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
    // 버퍼는 COMMON에서 COPY_DEST로, 실행 후 다시 읽기 상태로 암묵적으로 승격/감쇠된다.
    list->CopyBufferRegion(entry.resource.Get(), 0, staging.Get(), 0, bytes);
    HR(list->Close());
    ID3D12CommandList* lists[] = {list.Get()};
    m->queue->ExecuteCommandLists(1, lists);
    m->waitIdle();
    m->buffers.push_back(std::move(entry));
    return {static_cast<u32>(m->buffers.size())};
}

Texture Device::createTexture(u32 width, u32 height, Format format) {
    Impl::TextureEntry entry;
    entry.format = toDxgi(format);
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = width;
    td.Height = height;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = entry.format;
    td.SampleDesc.Count = 1;
    const D3D12_HEAP_PROPERTIES defaultHeap = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
    HR(m->device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &td, toD3d(TextureState::ShaderRead),
                                          nullptr, IID_PPV_ARGS(&entry.resource)));
    entry.slot = m->allocTextureSlot();
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = entry.format;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    m->device->CreateShaderResourceView(entry.resource.Get(), &sd, m->srvCpu(entry.slot));
    m->textures.push_back(std::move(entry));
    return {static_cast<u32>(m->textures.size())};
}

u32 Device::textureIndex(Texture texture) const { return m->textures[texture.id - 1].slot - kMaxBuffers; }

Pipeline Device::createPipeline(const PipelineDesc& desc) {
    std::vector<D3D12_INPUT_ELEMENT_DESC> layout;
    for (const VertexAttribute& a : desc.vertexAttributes)
        layout.push_back({a.semantic, 0,
                          a.format == VertexFormat::Float3 ? DXGI_FORMAT_R32G32B32_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM, 0,
                          a.offset, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0});

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = m->rootSignature.Get();
    pd.VS = {desc.vs.data, desc.vs.size};
    pd.PS = {desc.ps.data, desc.ps.size};
    D3D12_RENDER_TARGET_BLEND_DESC& b = pd.BlendState.RenderTarget[0];
    if (desc.premultipliedBlend) {
        b.BlendEnable = TRUE;
        b.SrcBlend = D3D12_BLEND_ONE;
        b.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        b.BlendOp = D3D12_BLEND_OP_ADD;
        b.SrcBlendAlpha = D3D12_BLEND_ONE;
        b.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }
    b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = desc.cullBack ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    const bool scene = desc.target == Target::Scene;
    if (scene) {
        pd.DepthStencilState.DepthEnable = TRUE;
        pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER; // reversed-Z
        pd.DSVFormat = kDepthFormat;
    }
    pd.InputLayout = {layout.data(), static_cast<UINT>(layout.size())};
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = scene ? kSceneFormat : kOverlayFormat;
    pd.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pso;
    HR(m->device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)));
    m->pipelines.push_back(std::move(pso));
    return {static_cast<u32>(m->pipelines.size())};
}

bool Device::beginFrame() {
    if (!m || m->width == 0 || m->height == 0) return false;

    // 프레젠트 큐가 비기를 기다린다: CPU가 GPU보다 너무 앞서가 입력 지연이 쌓이는 것을 막는다.
    WaitForSingleObjectEx(m->frameLatencyWaitable, 1000, TRUE);

    Impl::Frame& frame = m->frames[m->frameIndex];
    m->waitForFence(frame.fenceValue);
    HR(frame.allocator->Reset());
    HR(m->list->Reset(frame.allocator.Get(), nullptr));
#if defined(TRACY_ENABLE)
    static const tracy::SourceLocationData gpuFrameLocation{"GPU frame", __FUNCTION__, __FILE__, __LINE__, 0};
    if (m->tracyGpu) m->tracyFrameZone.emplace(m->tracyGpu, m->list.Get(), &gpuFrameLocation, true);
#endif
    m->uploadOffset = m->frameIndex * kUploadBytesPerFrame;
    m->uploadLimit = m->uploadOffset + kUploadBytesPerFrame;
    m->backBufferIndex = m->swapchain->GetCurrentBackBufferIndex();
    m->backBufferIsTarget = false;

    ID3D12DescriptorHeap* heaps[] = {m->srvHeap.Get()};
    m->list->SetDescriptorHeaps(1, heaps);
    m->list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
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

u32 Device::copyPitch(u32 width, u32 bytesPerPixel) const {
    return (width * bytesPerPixel + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
}

u32 Device::copyAlignment() const { return D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT; }

void Device::endFrame(bool vsync, Capture* capture) {
    if (!m || !m->inFrame) return;
    ID3D12GraphicsCommandList* list = m->list.Get();
    ID3D12Resource* backBuffer = m->backBuffers[m->backBufferIndex].Get();
    const D3D12_RESOURCE_STATES current =
        m->backBufferIsTarget ? D3D12_RESOURCE_STATE_RENDER_TARGET : D3D12_RESOURCE_STATE_PRESENT;

    ComPtr<ID3D12Resource> readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    u64 readbackBytes = 0;
    if (capture) {
        const D3D12_RESOURCE_DESC texDesc = backBuffer->GetDesc();
        m->device->GetCopyableFootprints(&texDesc, 0, 1, 0, &footprint, nullptr, nullptr, &readbackBytes);
        const D3D12_HEAP_PROPERTIES readbackHeap = heapProperties(D3D12_HEAP_TYPE_READBACK);
        const D3D12_RESOURCE_DESC rbDesc = bufferDesc(readbackBytes);
        HR(m->device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &rbDesc,
                                              D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));
        const D3D12_RESOURCE_BARRIER toCopy = transition(backBuffer, current, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &toCopy);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = readback.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = backBuffer;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        const D3D12_RESOURCE_BARRIER toPresent =
            transition(backBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
        list->ResourceBarrier(1, &toPresent);
    } else if (m->backBufferIsTarget) {
        const D3D12_RESOURCE_BARRIER toPresent =
            transition(backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        list->ResourceBarrier(1, &toPresent);
    }

#if defined(TRACY_ENABLE)
    m->tracyFrameZone.reset(); // GPU 구간 끝 (커맨드 리스트를 닫기 전에)
#endif
    HR(list->Close());
    ID3D12CommandList* lists[] = {list};
    m->queue->ExecuteCommandLists(1, lists);

    const UINT syncInterval = vsync ? 1 : 0;
    const UINT presentFlags = (!vsync && m->tearingSupported) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    HR(m->swapchain->Present(syncInterval, presentFlags));

    Impl::Frame& frame = m->frames[m->frameIndex];
    frame.fenceValue = m->signal();
#if defined(TRACY_ENABLE)
    if (m->tracyGpu) {
        TracyD3D12NewFrame(m->tracyGpu);
        TracyD3D12Collect(m->tracyGpu);
    }
#endif

    if (readback) {
        m->waitForFence(frame.fenceValue);
        const u32 w = footprint.Footprint.Width, h = footprint.Footprint.Height;
        capture->width = w;
        capture->height = h;
        capture->rgba.resize(static_cast<size_t>(w) * h * 4);
        u8* data = nullptr;
        const D3D12_RANGE readRange{0, static_cast<SIZE_T>(readbackBytes)};
        if (SUCCEEDED(readback->Map(0, &readRange, reinterpret_cast<void**>(&data)))) {
            for (u32 y = 0; y < h; ++y) {
                const u8* src = data + footprint.Offset + static_cast<u64>(y) * footprint.Footprint.RowPitch;
                u8* dst = capture->rgba.data() + static_cast<size_t>(y) * w * 4;
                std::memcpy(dst, src, static_cast<size_t>(w) * 4);
                for (u32 x = 0; x < w; ++x) dst[x * 4 + 3] = 255;
            }
            const D3D12_RANGE noWrite{0, 0};
            readback->Unmap(0, &noWrite);
        } else {
            capture->width = capture->height = 0;
            capture->rgba.clear();
        }
    }

    m->frameIndex = (m->frameIndex + 1) % kFramesInFlight;
    m->inFrame = false;
}

void Device::waitIdle() {
    if (m && m->queue && m->fence) m->waitIdle();
}

// ---------------------------------------------------------------- CommandList

void CommandList::beginScene(const f32 clearColor[4], f32 clearDepth) {
    Device::Impl* m = m_device->impl();
    ID3D12GraphicsCommandList* list = m->list.Get();
    if (!m->backBufferIsTarget) {
        const D3D12_RESOURCE_BARRIER toRender = transition(m->backBuffers[m->backBufferIndex].Get(),
                                                           D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->ResourceBarrier(1, &toRender);
        m->backBufferIsTarget = true;
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv{m->rtvHeap->GetCPUDescriptorHandleForHeapStart().ptr +
                                          m->backBufferIndex * m->rtvStride};
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = m->dsvHeap->GetCPUDescriptorHandleForHeapStart();
    list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    list->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
    list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, clearDepth, 0, 0, nullptr);
}

void CommandList::beginOverlay() {
    Device::Impl* m = m_device->impl();
    ID3D12GraphicsCommandList* list = m->list.Get();
    if (!m->backBufferIsTarget) {
        const D3D12_RESOURCE_BARRIER toRender = transition(m->backBuffers[m->backBufferIndex].Get(),
                                                           D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->ResourceBarrier(1, &toRender);
        m->backBufferIsTarget = true;
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv{m->rtvHeap->GetCPUDescriptorHandleForHeapStart().ptr +
                                          (kBackBufferCount + m->backBufferIndex) * m->rtvStride};
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
}

void CommandList::setViewport(f32 x, f32 y, f32 width, f32 height) {
    ID3D12GraphicsCommandList* list = m_device->impl()->list.Get();
    const D3D12_VIEWPORT viewport{x, y, width, height, 0.0f, 1.0f};
    const D3D12_RECT scissor{static_cast<LONG>(x), static_cast<LONG>(y), static_cast<LONG>(x + width),
                             static_cast<LONG>(y + height)};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
}

void CommandList::setPipeline(Pipeline pipeline) {
    Device::Impl* m = m_device->impl();
    ID3D12GraphicsCommandList* list = m->list.Get();
    list->SetPipelineState(m->pipelines[pipeline.id - 1].Get());
    // 디버그 UI가 자기 루트 시그니처·힙을 묶었을 수 있으므로 매번 다시 묶는다 (루트 상수는 이 뒤에)
    ID3D12DescriptorHeap* heaps[] = {m->srvHeap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootSignature(m->rootSignature.Get());
    list->SetGraphicsRootDescriptorTable(1, m->srvGpu(0));
    list->SetGraphicsRootDescriptorTable(2, m->srvGpu(kMaxBuffers));
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void CommandList::pushConstants(const void* data, u32 bytes) {
    m_device->impl()->list->SetGraphicsRoot32BitConstants(0, bytes / 4, data, 0);
}

void CommandList::setVertexBuffer(Buffer buffer, u32 stride) {
    const Device::Impl::BufferEntry& b = m_device->impl()->buffers[buffer.id - 1];
    const D3D12_VERTEX_BUFFER_VIEW vbv{b.resource->GetGPUVirtualAddress(), static_cast<UINT>(b.bytes), stride};
    m_device->impl()->list->IASetVertexBuffers(0, 1, &vbv);
}

void CommandList::setIndexBuffer(Buffer buffer) {
    const Device::Impl::BufferEntry& b = m_device->impl()->buffers[buffer.id - 1];
    const D3D12_INDEX_BUFFER_VIEW ibv{b.resource->GetGPUVirtualAddress(), static_cast<UINT>(b.bytes),
                                      b.usage == BufferUsage::Index16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT};
    m_device->impl()->list->IASetIndexBuffer(&ibv);
}

void CommandList::drawIndexed(u32 indexCount, u32 instanceCount) {
    m_device->impl()->list->DrawIndexedInstanced(indexCount, instanceCount, 0, 0, 0);
}

void CommandList::draw(u32 vertexCount, u32 instanceCount) {
    m_device->impl()->list->DrawInstanced(vertexCount, instanceCount, 0, 0);
}

void CommandList::textureBarrier(Texture texture, TextureState before, TextureState after) {
    Device::Impl* m = m_device->impl();
    const D3D12_RESOURCE_BARRIER b = transition(m->textures[texture.id - 1].resource.Get(), toD3d(before), toD3d(after));
    m->list->ResourceBarrier(1, &b);
}

void CommandList::copyToTexture(Texture texture, u32 x, u32 y, u32 width, u32 height, u64 uploadOffset, u32 rowPitch) {
    Device::Impl* m = m_device->impl();
    const Device::Impl::TextureEntry& t = m->textures[texture.id - 1];
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = m->uploadBuffer.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Offset = uploadOffset;
    src.PlacedFootprint.Footprint = {t.format, width, height, 1, rowPitch};
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = t.resource.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    m->list->CopyTextureRegion(&dst, x, y, 0, &src, nullptr);
}

// ---------------------------------------------------------------- GPU 조사

std::string gpuMinimumError(const GpuInfo& info) {
    // 최소 사양 Intel 11세대(Xe-LP): SM 6.6과 바인딩 티어 3(bindless 무한 배열)을 쓴다.
    // 같은 GPU라도 제조사가 깐 오래된 드라이버면 6.6이 안 나오므로 드라이버 버전을 같이 알려 준다.
    const char* driver = info.driverVersion.empty() ? "unknown" : info.driverVersion.c_str();
    char buf[512];
    if (info.shaderModel < 0x66) {
        std::snprintf(buf, sizeof(buf),
                      "Shader Model 6.6 is required, but %s (driver %s) reports %u.%u. Update the GPU driver. "
                      "Minimum GPU: Intel 11th gen (Iris Xe / UHD Xe) integrated graphics or newer.",
                      info.adapter.c_str(), driver, info.shaderModel >> 4, info.shaderModel & 0xF);
        return buf;
    }
    if (info.resourceBindingTier < 3) {
        std::snprintf(buf, sizeof(buf),
                      "Resource binding tier 3 is required, but %s (driver %s) reports tier %u. Update the GPU driver.",
                      info.adapter.c_str(), driver, info.resourceBindingTier);
        return buf;
    }
    return {};
}

bool queryGpu(GpuInfo& out, std::string* error) {
    ComPtr<IDXGIFactory6> factory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
        if (error) *error = "DXGI 1.6 is required (Windows 10 1803 or later)";
        return false;
    }
    adapter = findAdapter(factory.Get());
    if (!adapter || FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
        if (error) *error = "No Direct3D 12 capable GPU found";
        return false;
    }
    describeGpu(adapter.Get(), device.Get(), out);
    return true;
}

} // namespace mealkit::rhi
