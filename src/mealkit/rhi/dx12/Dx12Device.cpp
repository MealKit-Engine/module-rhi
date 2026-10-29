#include "mealkit/rhi/dx12/Dx12.h"

#include "mealkit/core/Log.h"
#include "mealkit/rhi/GpuCapture.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace mealkit::rhi {
namespace {

ID3D12Device* g_errorDevice = nullptr;

// DXGI name (UTF-16) → UTF-8 (characters outside the BMP are surrogate pairs)
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

// User-mode driver version. Same as the four-part number shown by Device Manager and vendor sites.
std::string driverVersion(IDXGIAdapter1* adapter) {
    LARGE_INTEGER v{};
    if (FAILED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &v))) return {};
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", HIWORD(v.HighPart), LOWORD(v.HighPart), HIWORD(v.LowPart),
                  LOWORD(v.LowPart));
    return buf;
}

// The first hardware adapter that can create a D3D12 device, in high-performance-first order.
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

    // Asking for a shader model the runtime doesn't know fails, so go down from the highest (ones without SDK enum values as numbers).
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

DXGI_FORMAT toDxgi(Format f) {
    switch (f) {
    case Format::R8Unorm: return DXGI_FORMAT_R8_UNORM;
    case Format::Rgba8Srgb: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case Format::Bc7Unorm: return DXGI_FORMAT_BC7_UNORM;
    case Format::Bc7Srgb: return DXGI_FORMAT_BC7_UNORM_SRGB;
    case Format::Bc5Unorm: return DXGI_FORMAT_BC5_UNORM;
    case Format::Rgb9e5: return DXGI_FORMAT_R9G9B9E5_SHAREDEXP;
    default: return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

void __stdcall onDebugMessage(D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id,
                              LPCSTR description, void*) {
    // Offscreen targets are cleared with different colors each time (lookdev lighting presets), so no optimized clear value is given: it's only a performance hint
    if (id == D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE) return;
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

void Device::Impl::readGpuTimes(const Frame& frame, u32 base) {
    const u64* q = queryCpu + base;
    const auto ms = [&](u32 begin) { return q[begin + 1] > q[begin] ? static_cast<f32>((q[begin + 1] - q[begin]) * timestampMsPerTick) : 0.0f; };
    GpuTimes t;
    t.valid = true;
    t.frameMs = ms(0);
    for (u32 i = 0; i < kMaxGpuTimers; ++i)
        if (frame.timerMask & (1u << i)) t.timerMs[i] = ms(2 + 2 * i);
    gpuTimes = t;
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
        rd.Format = kOverlayFormat;
        rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device->CreateRenderTargetView(backBuffers[i].Get(), &rd, {rtv.ptr + i * rtvStride});
    }
}

void Device::Impl::releaseSwapchainResources() {
    for (auto& bb : backBuffers) bb.Reset();
}

// Root signature shared by all pipelines: root constants (b0) + buffer table (t0, space1) + texture table (t0, space2) + samplers (s0)
void Device::Impl::createRootSignature() {
    // Ranges: buffers (space1), 2D textures (space2), cubes (space3). Textures and cubes look at the same heap slots (each slot has only one kind of view)
    D3D12_DESCRIPTOR_RANGE1 ranges[3] = {};
    for (u32 i = 0; i < 3; ++i) {
        ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[i].NumDescriptors = UINT_MAX; // unbounded array (binding tier 3)
        ranges[i].BaseShaderRegister = 0;
        ranges[i].RegisterSpace = 1 + i;
        ranges[i].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE; // unused slots may be empty
    }
    D3D12_ROOT_PARAMETER1 params[4] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = Device::kPushConstantBytes / 4;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    for (u32 i = 0; i < 3; ++i) {
        params[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1 + i].DescriptorTable.NumDescriptorRanges = 1;
        params[1 + i].DescriptorTable.pDescriptorRanges = &ranges[i];
        params[1 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
    D3D12_STATIC_SAMPLER_DESC& sampler = samplers[0]; // s0: UI atlases
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_STATIC_SAMPLER_DESC& wrap = samplers[1]; // s1: model textures
    wrap = sampler;
    wrap.Filter = D3D12_FILTER_ANISOTROPIC;
    wrap.MaxAnisotropy = 8;
    wrap.AddressU = wrap.AddressV = wrap.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    wrap.ShaderRegister = 1;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rs{};
    rs.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    rs.Desc_1_1.NumParameters = 4;
    rs.Desc_1_1.pParameters = params;
    rs.Desc_1_1.NumStaticSamplers = 2;
    rs.Desc_1_1.pStaticSamplers = samplers;
    rs.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> blob, error;
    if (FAILED(D3D12SerializeVersionedRootSignature(&rs, &blob, &error)))
        MEALKIT_FATAL("Root signature: %s", error ? static_cast<const char*>(error->GetBufferPointer()) : "unknown error");
    HR(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rootSignature)));
}

// ---------------------------------------------------------------- Device

Device::~Device() { shutdown(); }

bool Device::init(const DeviceDesc& desc) {
    if (desc.gpuCapture) loadGpuCaptureTool(true); // capture tools must be loaded before the device to intercept the API
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
    rtvDesc.NumDescriptors = kBackBufferCount; // overlay views
    HR(m->device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&m->rtvHeap)));
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

    // Upload ring: kUploadBytesPerFrame per frame. Shaders read it via g_buffers[0] (raw buffer view)
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

    // GPU time: timestamp query heap + readback buffer (one section per frame)
    UINT64 frequency = 0;
    if (SUCCEEDED(m->queue->GetTimestampFrequency(&frequency)) && frequency > 0) {
        D3D12_QUERY_HEAP_DESC timestampDesc{};
        timestampDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        timestampDesc.Count = kQueriesPerFrame * kFramesInFlight;
        const D3D12_HEAP_PROPERTIES readbackHeap = heapProperties(D3D12_HEAP_TYPE_READBACK);
        const D3D12_RESOURCE_DESC rbDesc = bufferDesc(timestampDesc.Count * 8ull);
        if (SUCCEEDED(m->device->CreateQueryHeap(&timestampDesc, IID_PPV_ARGS(&m->queryHeap))) &&
            SUCCEEDED(m->device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &rbDesc,
                                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                         IID_PPV_ARGS(&m->queryReadback))) &&
            SUCCEEDED(m->queryReadback->Map(0, nullptr, reinterpret_cast<void**>(const_cast<u64**>(&m->queryCpu))))) {
            m->timestampMsPerTick = 1000.0 / static_cast<f64>(frequency);
        } else {
            m->queryHeap.Reset();
            m->queryReadback.Reset();
            m->queryCpu = nullptr;
        }
    }
    m->adapter.As(&m->adapter3);

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
    // Buffers are implicitly promoted/decayed from COMMON to COPY_DEST and back to a read state after execution.
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

Texture Device::createTexture(const TextureInit& init) {
    Impl::TextureEntry entry;
    entry.format = toDxgi(init.format);
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = init.width;
    td.Height = init.height;
    const u32 layers = init.cube ? 6u : 1u;
    td.DepthOrArraySize = static_cast<UINT16>(layers);
    td.MipLevels = static_cast<UINT16>(init.mipLevels);
    td.Format = entry.format;
    td.SampleDesc.Count = 1;
    const D3D12_HEAP_PROPERTIES defaultHeap = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
    HR(m->device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                          IID_PPV_ARGS(&entry.resource)));

    // Each subresource (per face from mip 0) is moved into the upload buffer at the GPU row pitch (256-byte aligned) and copied in one go
    const u32 subresources = init.mipLevels * layers;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(subresources);
    std::vector<UINT> rows(subresources);
    std::vector<UINT64> rowBytes(subresources);
    UINT64 total = 0;
    m->device->GetCopyableFootprints(&td, 0, subresources, 0, footprints.data(), rows.data(), rowBytes.data(), &total);
    const D3D12_HEAP_PROPERTIES uploadHeap = heapProperties(D3D12_HEAP_TYPE_UPLOAD);
    const D3D12_RESOURCE_DESC stagingDesc = bufferDesc(total);
    ComPtr<ID3D12Resource> staging;
    HR(m->device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &stagingDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                          nullptr, IID_PPV_ARGS(&staging)));
    u8* mapped = nullptr;
    const D3D12_RANGE noRead{0, 0};
    HR(staging->Map(0, &noRead, reinterpret_cast<void**>(&mapped)));
    const u8* src = static_cast<const u8*>(init.data);
    for (u32 sub = 0; sub < subresources; ++sub) {
        const u32 mip = sub % init.mipLevels;
        const u32 w = std::max(1u, init.width >> mip), h = std::max(1u, init.height >> mip);
        const u64 tight = textureMipBytes(init.format, w, h) / rows[sub]; // one row (block row for BC)
        for (UINT row = 0; row < rows[sub]; ++row)
            std::memcpy(mapped + footprints[sub].Offset + static_cast<u64>(row) * footprints[sub].Footprint.RowPitch,
                        src + row * tight, tight);
        src += textureMipBytes(init.format, w, h);
    }
    staging->Unmap(0, nullptr);

    ComPtr<ID3D12CommandAllocator> allocator;
    HR(m->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList> list;
    HR(m->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
    for (u32 sub = 0; sub < subresources; ++sub) {
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = entry.resource.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = sub; // face × mip count + mip
        D3D12_TEXTURE_COPY_LOCATION from{};
        from.pResource = staging.Get();
        from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        from.PlacedFootprint = footprints[sub];
        list->CopyTextureRegion(&dst, 0, 0, 0, &from, nullptr);
    }
    const D3D12_RESOURCE_BARRIER toRead =
        transition(entry.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, toD3d(TextureState::ShaderRead));
    list->ResourceBarrier(1, &toRead);
    HR(list->Close());
    ID3D12CommandList* lists[] = {list.Get()};
    m->queue->ExecuteCommandLists(1, lists);
    m->waitIdle();

    entry.slot = m->allocTextureSlot();
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = entry.format;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (init.cube) {
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        sd.TextureCube.MipLevels = init.mipLevels;
    } else {
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = init.mipLevels;
    }
    m->device->CreateShaderResourceView(entry.resource.Get(), &sd, m->srvCpu(entry.slot));
    m->textures.push_back(std::move(entry));
    return {static_cast<u32>(m->textures.size())};
}

u32 Device::textureIndex(Texture texture) const { return m->textures[texture.id - 1].slot - kMaxBuffers; }

RenderTarget Device::createRenderTarget(u32 width, u32 height, TargetKind kind) {
    Impl::TextureEntry color;
    color.format = kind == TargetKind::Ldr ? kOverlayFormat : kHdrFormat;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = width;
    td.Height = height;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = color.format;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    // The clear color differs per user (lighting preset backgrounds), so no optimized clear value is given
    const D3D12_HEAP_PROPERTIES defaultHeap = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
    HR(m->device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &td, toD3d(TextureState::ShaderRead), nullptr,
                                          IID_PPV_ARGS(&color.resource)));
    color.slot = m->allocTextureSlot();
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = color.format;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    m->device->CreateShaderResourceView(color.resource.Get(), &sd, m->srvCpu(color.slot));

    Impl::TargetEntry target;
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.NumDescriptors = 1;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    HR(m->device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&target.rtvHeap)));
    D3D12_RENDER_TARGET_VIEW_DESC rd{};
    rd.Format = color.format;
    rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    m->device->CreateRenderTargetView(color.resource.Get(), &rd, target.rtvHeap->GetCPUDescriptorHandleForHeapStart());

    if (kind == TargetKind::Scene) {
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        HR(m->device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&target.dsvHeap)));
        D3D12_RESOURCE_DESC dd = td;
        dd.Format = kDepthFormat;
        dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
        D3D12_CLEAR_VALUE depthClear{};
        depthClear.Format = kDepthFormat;
        HR(m->device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &dd, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                              &depthClear, IID_PPV_ARGS(&target.depth)));
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
        dsv.Format = kDepthFormat;
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        m->device->CreateDepthStencilView(target.depth.Get(), &dsv, target.dsvHeap->GetCPUDescriptorHandleForHeapStart());
    }

    m->textures.push_back(std::move(color));
    target.texture = static_cast<u32>(m->textures.size());
    m->targets.push_back(std::move(target));
    return {static_cast<u32>(m->targets.size())};
}

void Device::destroyRenderTarget(RenderTarget target) {
    if (!m || !target || target.id > m->targets.size() || m->inFrame) return;
    Impl::TargetEntry& t = m->targets[target.id - 1];
    if (!t.texture) return;
    m->waitIdle();
    Impl::TextureEntry& color = m->textures[t.texture - 1];
    m->freeTextureSlot(color.slot);
    color = {};
    t = {};
}

RenderTarget Device::createDepthTarget(u32 width, u32 height) {
    Impl::TextureEntry depth;
    depth.format = DXGI_FORMAT_R32_FLOAT; // read view
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = width;
    td.Height = height;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R32_TYPELESS; // writes use a D32 view, reads an R32 view
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    const D3D12_HEAP_PROPERTIES defaultHeap = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_CLEAR_VALUE clear{};
    clear.Format = kDepthFormat;
    HR(m->device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &td, toD3d(TextureState::ShaderRead), &clear,
                                          IID_PPV_ARGS(&depth.resource)));
    depth.slot = m->allocTextureSlot();
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = depth.format;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    m->device->CreateShaderResourceView(depth.resource.Get(), &sd, m->srvCpu(depth.slot));

    Impl::TargetEntry target;
    target.depthOnly = true;
    target.depth = depth.resource;
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.NumDescriptors = 1;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    HR(m->device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&target.dsvHeap)));
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = kDepthFormat;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    m->device->CreateDepthStencilView(depth.resource.Get(), &dsv, target.dsvHeap->GetCPUDescriptorHandleForHeapStart());
    m->textures.push_back(std::move(depth));
    target.texture = static_cast<u32>(m->textures.size());
    m->targets.push_back(std::move(target));
    return {static_cast<u32>(m->targets.size())};
}

Texture Device::renderTargetTexture(RenderTarget target) const { return {target ? m->targets[target.id - 1].texture : 0}; }

Pipeline Device::createPipeline(const PipelineDesc& desc) {
    std::vector<D3D12_INPUT_ELEMENT_DESC> layout;
    for (const VertexAttribute& a : desc.vertexAttributes)
        layout.push_back({a.semantic, 0,
                          a.format == VertexFormat::Float3   ? DXGI_FORMAT_R32G32B32_FLOAT
                          : a.format == VertexFormat::Half2  ? DXGI_FORMAT_R16G16_FLOAT
                          : a.format == VertexFormat::Uint16x2 ? DXGI_FORMAT_R16G16_UINT
                          : a.format == VertexFormat::Uint8x4 ? DXGI_FORMAT_R8G8B8A8_UINT
                                                             : DXGI_FORMAT_R8G8B8A8_UNORM, 0,
                          a.offset, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0});

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = m->rootSignature.Get();
    pd.VS = {desc.vs.data, desc.vs.size};
    pd.PS = {desc.ps.data, desc.ps.size};
    D3D12_RENDER_TARGET_BLEND_DESC& b = pd.BlendState.RenderTarget[0];
    if (desc.additiveBlend) {
        b.BlendEnable = TRUE;
        b.SrcBlend = b.DestBlend = b.SrcBlendAlpha = b.DestBlendAlpha = D3D12_BLEND_ONE;
        b.BlendOp = b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    } else if (desc.premultipliedBlend) {
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
    pd.RasterizerState.FillMode = desc.wireframe ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = desc.cullFront ? D3D12_CULL_MODE_FRONT : desc.cullBack ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    const bool scene = desc.target == Target::Scene, shadow = desc.target == Target::Shadow;
    if (scene || shadow) {
        pd.DepthStencilState.DepthEnable = desc.depthTest || shadow ? TRUE : FALSE;
        pd.DepthStencilState.DepthWriteMask = desc.depthWrite || shadow ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER; // reversed-Z
        pd.DSVFormat = kDepthFormat;
    }
    pd.InputLayout = {layout.data(), static_cast<UINT>(layout.size())};
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = shadow ? 0 : 1;
    pd.RTVFormats[0] = shadow ? DXGI_FORMAT_UNKNOWN : desc.target == Target::Overlay ? kOverlayFormat : kHdrFormat;
    pd.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pso;
    HR(m->device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)));
    m->pipelines.push_back(std::move(pso));
    return {static_cast<u32>(m->pipelines.size())};
}

bool Device::recreatePipeline(Pipeline pipeline, const PipelineDesc& desc) {
    if (!m || m->inFrame || !pipeline || pipeline.id > m->pipelines.size()) return false;
    m->waitIdle();
    createPipeline(desc); // the new one appended at the end goes into the original slot
    m->pipelines[pipeline.id - 1] = std::move(m->pipelines.back());
    m->pipelines.pop_back();
    return true;
}

bool Device::beginFrame() {
    if (!m || m->width == 0 || m->height == 0) return false;

    // Waits for the present queue to drain: keeps the CPU from running too far ahead of the GPU and piling up input latency.
    WaitForSingleObjectEx(m->frameLatencyWaitable, 1000, TRUE);

    Impl::Frame& frame = m->frames[m->frameIndex];
    m->waitForFence(frame.fenceValue);
    if (frame.timed) m->readGpuTimes(frame, m->queryBase());
    frame.timed = false;
    frame.timerMask = 0;
    HR(frame.allocator->Reset());
    HR(m->list->Reset(frame.allocator.Get(), nullptr));
    if (m->queryHeap) m->list->EndQuery(m->queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, m->queryBase());
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
    ID3D12Resource* captured = capture && capture->source ? m->textures[m->targets[capture->source.id - 1].texture - 1].resource.Get()
                                                          : backBuffer;
    const D3D12_RESOURCE_STATES capturedState = captured == backBuffer ? current : toD3d(TextureState::ShaderRead);
    if (capture) {
        const D3D12_RESOURCE_DESC texDesc = captured->GetDesc();
        m->device->GetCopyableFootprints(&texDesc, 0, 1, 0, &footprint, nullptr, nullptr, &readbackBytes);
        const D3D12_HEAP_PROPERTIES readbackHeap = heapProperties(D3D12_HEAP_TYPE_READBACK);
        const D3D12_RESOURCE_DESC rbDesc = bufferDesc(readbackBytes);
        HR(m->device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &rbDesc,
                                              D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));
        const D3D12_RESOURCE_BARRIER toCopy = transition(captured, capturedState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &toCopy);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = readback.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = captured;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        const D3D12_RESOURCE_BARRIER back = transition(captured, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                       captured == backBuffer ? D3D12_RESOURCE_STATE_PRESENT : capturedState);
        list->ResourceBarrier(1, &back);
    }
    if ((!capture || captured != backBuffer) && m->backBufferIsTarget) {
        const D3D12_RESOURCE_BARRIER toPresent =
            transition(backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        list->ResourceBarrier(1, &toPresent);
    }

#if defined(TRACY_ENABLE)
    m->tracyFrameZone.reset(); // end of the GPU zone (before closing the command list)
#endif
    Impl::Frame& frame = m->frames[m->frameIndex];
    if (m->queryHeap) { // only written queries go to readback (reading unwritten slots is a validation layer error)
        const u32 base = m->queryBase();
        list->EndQuery(m->queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, base + 1);
        m->resolveQueries(base, 2);
        for (u32 i = 0; i < kMaxGpuTimers; ++i)
            if (frame.timerMask & (1u << i)) m->resolveQueries(base + 2 + 2 * i, 2);
        frame.timed = true;
    }
    HR(list->Close());
    ID3D12CommandList* lists[] = {list};
    m->queue->ExecuteCommandLists(1, lists);

    const UINT syncInterval = vsync ? 1 : 0;
    const UINT presentFlags = (!vsync && m->tearingSupported) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    HR(m->swapchain->Present(syncInterval, presentFlags));

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

const GpuTimes& Device::gpuTimes() const {
    static const GpuTimes none;
    return m ? m->gpuTimes : none;
}

GpuMemory Device::gpuMemory() const {
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    if (!m || !m->adapter3 || FAILED(m->adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) return {};
    return {info.CurrentUsage, info.Budget};
}

// ---------------------------------------------------------------- CommandList

void CommandList::beginTimer(u32 timer) {
    Device::Impl& d = *m_device->impl();
    if (!d.queryHeap || timer >= kMaxGpuTimers) return;
    d.frames[d.frameIndex].timerMask |= 1u << timer;
    d.list->EndQuery(d.queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, d.queryBase() + 2 + 2 * timer);
}

void CommandList::endTimer(u32 timer) {
    Device::Impl& d = *m_device->impl();
    if (!d.queryHeap || timer >= kMaxGpuTimers) return;
    d.list->EndQuery(d.queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, d.queryBase() + 3 + 2 * timer);
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
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv{m->rtvHeap->GetCPUDescriptorHandleForHeapStart().ptr + m->backBufferIndex * m->rtvStride};
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
}

void CommandList::beginTarget(RenderTarget target, const f32 clearColor[4], f32 clearDepth) {
    Device::Impl* m = m_device->impl();
    ID3D12GraphicsCommandList* list = m->list.Get();
    const Device::Impl::TargetEntry& t = m->targets[target.id - 1];
    if (t.depthOnly) { // shadow maps: bind and clear depth only
        const D3D12_RESOURCE_BARRIER toDepth =
            transition(t.depth.Get(), toD3d(TextureState::ShaderRead), D3D12_RESOURCE_STATE_DEPTH_WRITE);
        list->ResourceBarrier(1, &toDepth);
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv = t.dsvHeap->GetCPUDescriptorHandleForHeapStart();
        list->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
        list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, clearDepth, 0, 0, nullptr);
        return;
    }
    const D3D12_RESOURCE_BARRIER toRender = transition(m->textures[t.texture - 1].resource.Get(), toD3d(TextureState::ShaderRead),
                                                       D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->ResourceBarrier(1, &toRender);
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = t.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    if (t.dsvHeap) {
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv = t.dsvHeap->GetCPUDescriptorHandleForHeapStart();
        list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, clearDepth, 0, 0, nullptr);
    } else {
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    }
    if (clearColor) list->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
}

void CommandList::endTarget(RenderTarget target) {
    Device::Impl* m = m_device->impl();
    const Device::Impl::TargetEntry& t = m->targets[target.id - 1];
    const D3D12_RESOURCE_BARRIER toRead =
        transition(m->textures[t.texture - 1].resource.Get(),
                   t.depthOnly ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET, toD3d(TextureState::ShaderRead));
    m->list->ResourceBarrier(1, &toRead);
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
    // The debug UI may have bound its own root signature/heap, so rebind every time (root constants after this)
    ID3D12DescriptorHeap* heaps[] = {m->srvHeap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootSignature(m->rootSignature.Get());
    list->SetGraphicsRootDescriptorTable(1, m->srvGpu(0));
    list->SetGraphicsRootDescriptorTable(2, m->srvGpu(kMaxBuffers));
    list->SetGraphicsRootDescriptorTable(3, m->srvGpu(kMaxBuffers)); // cubes (same slots)
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

// ---------------------------------------------------------------- GPU inspection

std::string gpuMinimumError(const GpuInfo& info) {
    // Min-spec Intel 11th gen (Xe-LP): uses SM 6.6 and binding tier 3 (bindless unbounded arrays).
    // Even the same GPU won't report 6.6 with an old vendor-installed driver, so the driver version is reported too.
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
