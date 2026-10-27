#pragma once

#include "mealkit/core/Types.h"

#include <string>
#include <vector>

// 엔진이 만드는 얇은 그래픽 API 층 (docs/CODING_MODEL.md "그래픽 API와 OS 기능", ROADMAP 2단계 "RHI 층").
// 렌더러가 쓰는 만큼만: 장치·스왑체인, 프레임 명령 목록, 정적 버퍼, 텍스처, 파이프라인, 프레임 업로드 링, 캡처.
// 모양은 DX12를 따른다(명령 목록 기록, 명시적 배리어, bindless 번호). 백엔드는 빌드 때 하나다(MEALKIT_RHI: rhi/dx12/, rhi/vulkan/),
// 가상 함수 없이 백엔드별 .cpp가 같은 클래스를 구현한다. 그래픽 API 헤더는 백엔드 폴더 밖에서 include하지 않는다.
//
// 바인딩 (docs/DECISIONS.md 2026-10-19 "RHI 바인딩 모양"): 모든 파이프라인이 같은 모양을 쓴다.
//   푸시 상수 64바이트 (HLSL `[[vk::push_constant]] ConstantBuffer<T> : register(b0)`)
//   버퍼 배열 `ByteAddressBuffer g_buffers[] : register(t0, space1)` ([[vk::binding(0, 0)]]) — bufferIndex()
//   텍스처 배열 `Texture2D g_textures[] : register(t0, space2)` ([[vk::binding(1, 0)]]) — textureIndex()
//   큐브 배열 `TextureCube g_cubes[] : register(t0, space3)` ([[vk::binding(4, 0)]]) — 큐브 텍스처의 textureIndex() (같은 번호 공간)
//   고정 샘플러 `SamplerState g_linearClamp : register(s0)` ([[vk::binding(2, 0)]]),
//              `SamplerState g_linearWrap : register(s1)` ([[vk::binding(3, 0)]], 이방성 8, 모델 텍스처)
namespace mealkit::rhi {

// GPU 기능 조사 결과. 시작 로그와 `--system-info`(최소 사양 실기 측정 기록)에 쓴다.
struct GpuInfo {
    std::string api;           // "Direct3D 12", "Vulkan 1.3.280"
    std::string adapter;       // 어댑터 이름 (UTF-8)
    std::string driverVersion; // 드라이버 버전 "31.0.101.5186" (모르면 빈 문자열)
    u32 vendorId = 0;          // 0x8086 Intel, 0x1002 AMD, 0x10DE NVIDIA
    u32 deviceId = 0;
    u64 dedicatedVideoMemory = 0;
    u64 sharedSystemMemory = 0;
    u32 featureLevel = 0;        // D3D_FEATURE_LEVEL (0xC100 = 12_1). Vulkan은 0
    u32 shaderModel = 0;         // D3D_SHADER_MODEL (0x66 = 6.6). Vulkan은 0
    u32 resourceBindingTier = 0; // 1~3. Vulkan은 0
    u32 meshShaderTier = 0;      // 0 = 없음 (참고용, 엔진은 쓰지 않는다)
    u32 waveLaneCountMin = 0;
    u32 waveLaneCountMax = 0;
    bool uma = false; // 내장 그래픽처럼 CPU와 메모리를 같이 쓴다
    std::string unsupported; // 백엔드가 확인한 최소 사양 부족 (Vulkan 버전·기능). 비면 없음
};

// 최소 사양에 못 미치는 이유. 맞으면 빈 문자열.
std::string gpuMinimumError(const GpuInfo& info);
// 창 없이 장치가 고를 GPU를 조사한다. GPU가 없으면 false와 error.
bool queryGpu(GpuInfo& out, std::string* error = nullptr);

// 핸들: 0 = 없음
struct Buffer {
    u32 id = 0;
    explicit operator bool() const { return id != 0; }
};
struct Texture {
    u32 id = 0;
    explicit operator bool() const { return id != 0; }
};
struct Pipeline {
    u32 id = 0;
    explicit operator bool() const { return id != 0; }
};
// 오프스크린 그리기 대상: 색 + 깊이. 형식이 장면(Target::Scene)과 같아 장면 파이프라인을 그대로 쓴다.
// 색은 sRGB로 쓰이고(셰이더는 선형) Texture로는 감마 변환 없이 읽힌다(UNORM 뷰): 디버그 UI·오버레이에 그대로 띄운다
struct RenderTarget {
    u32 id = 0;
    explicit operator bool() const { return id != 0; }
};

// 텍스처 형식. Srgb는 읽을 때 선형으로 바꾼다 (색 텍스처). BC는 4×4 블록 압축 (BC7 = 색 16바이트, BC5 = 두 채널 16바이트)
// Rgb9e5: HDR (공유 지수, 4바이트. 하늘·반사 큐브맵)
enum class Format : u8 { R8Unorm, Rgba8Unorm, Rgba8Srgb, Bc7Unorm, Bc7Srgb, Bc5Unorm, Rgb9e5 };

inline bool isBlockCompressed(Format f) { return f == Format::Bc7Unorm || f == Format::Bc7Srgb || f == Format::Bc5Unorm; }
// 밉 하나의 바이트 (줄 사이 빈틈 없음, BC는 블록 줄)
inline u64 textureMipBytes(Format f, u32 width, u32 height) {
    if (isBlockCompressed(f)) return static_cast<u64>((width + 3) / 4) * ((height + 3) / 4) * 16;
    return static_cast<u64>(width) * height * (f == Format::R8Unorm ? 1u : 4u);
}

// 내용이 있는 텍스처 (불러올 때 한 번 올리고 GPU를 기다린다). data는 밉 0부터 차례로 textureMipBytes씩.
// cube면 면 여섯(+X, -X, +Y, -Y, +Z, -Z)이 차례로, 면마다 밉 0부터 (셰이더는 g_cubes[textureIndex()]로 읽는다)
struct TextureInit {
    u32 width = 0, height = 0;
    u32 mipLevels = 1;
    Format format = Format::Rgba8Unorm;
    const void* data = nullptr;
    bool cube = false;
};
enum class BufferUsage : u8 { Vertex, Index16, Index32 };
enum class TextureState : u8 { ShaderRead, CopyDst };

// 정점 속성 (HLSL 입력 순서대로. DX12 시맨틱 이름, Vulkan location = 순서)
enum class VertexFormat : u8 { Float3, Unorm8x4, Half2, Uint16x2, Uint8x4 };
struct VertexAttribute {
    const char* semantic = "";
    VertexFormat format = VertexFormat::Float3;
    u32 offset = 0;
};

struct ShaderCode {
    const void* data = nullptr;
    size_t size = 0;
};

enum class Target : u8 {
    Scene,   // 백버퍼 sRGB 뷰(셰이더는 선형으로 쓴다) + 깊이 D32 (reversed-Z: GREATER)
    Overlay, // 백버퍼 감마 변환 없는 뷰, 깊이 없음 (UI는 이미 sRGB 색)
    Shadow,  // 깊이만 대상(createDepthTarget, 그림자 맵): 색 없음, 깊이 D32 (reversed-Z: GREATER). 픽셀 셰이더는 버리기(알파 마스크)만
};

struct PipelineDesc {
    ShaderCode vs, ps;
    std::vector<VertexAttribute> vertexAttributes; // 비면 정점 버퍼 없음 (SV_VertexID)
    u32 vertexStride = 0;
    Target target = Target::Scene;
    bool cullBack = true;       // 시계 방향이 앞면 (DX12 기본)
    bool cullFront = false;     // 앞면을 버린다 (외곽선의 뒤집은 껍질). cullBack보다 먼저
    bool premultipliedBlend = false;
    // 에디터 보기 모드용 (render::DebugView): 선으로 그리기, 더하기 섞기(겹친 수 = 밝기), 깊이 시험·쓰기 끄기
    bool wireframe = false; // Vulkan은 fillModeNonSolid가 없으면 면으로 그린다
    bool additiveBlend = false;
    bool depthTest = true; // Scene 대상만
    bool depthWrite = true; // 깊이 시험을 해도 쓰지 않는다 (반투명 파티클)
};

struct DeviceDesc {
    void* window = nullptr; // Window::nativeHandle()
    u32 width = 0;
    u32 height = 0;
    bool debug = false; // 검증 레이어 (DX12 debug layer, Vulkan validation)
    bool gpuCapture = false; // 장치를 만들기 전에 RenderDoc·PIX를 불러온다 (rhi/GpuCapture.h, 개발 빌드)
};

// 캡처한 백버퍼 (RGBA8, 줄 순서대로). source가 있으면 백버퍼 대신 그 오프스크린 대상을 읽는다 (반사 프로브 굽기,
// 값은 sRGB로 쓰인 그대로). 대상은 이번 프레임에 endTarget까지 끝나 있어야 한다
struct Capture {
    RenderTarget source;
    u32 width = 0, height = 0;
    std::vector<u8> rgba;
};

// GPU 시간 (타임스탬프 쿼리). 구간 번호(0..kMaxGpuTimers-1)의 뜻은 쓰는 쪽(렌더러의 패스)이 정한다.
// 결과는 GPU가 끝낸 가장 최근 프레임 값이다 (kFramesInFlight 프레임 늦다). 이번 프레임에 쓰지 않은 구간은 0.
inline constexpr u32 kMaxGpuTimers = 8;
struct GpuTimes {
    bool valid = false; // 타임스탬프를 지원하고 한 프레임 이상 끝났다
    f32 frameMs = 0.0f; // 명령 목록 처음부터 끝까지
    f32 timerMs[kMaxGpuTimers] = {};
};

// GPU 메모리: 드라이버가 이 프로세스에 주는 예산과 지금 쓰는 양 (DX12 QueryVideoMemoryInfo, Vulkan VK_EXT_memory_budget).
// 내장 그래픽(UMA)은 시스템 메모리를 같이 쓰므로 예산이 공유 메모리 기준이다. 모르면 0
struct GpuMemory {
    u64 used = 0;
    u64 budget = 0;
};

class Device;

// 한 프레임의 명령 목록 (Device::beginFrame ~ endFrame 사이).
// 그리기 대상(Scene/Overlay)은 begin*으로 묶고, 복사(copyToTexture)는 묶인 대상을 푼다: 복사 뒤에는 다시 begin*을 부른다.
class CommandList {
public:
    // 백버퍼를 sRGB 뷰 + 깊이로 묶고 지운다 (프레임마다 처음 한 번)
    void beginScene(const f32 clearColor[4], f32 clearDepth);
    // 백버퍼를 감마 변환 없는 뷰로 다시 묶는다 (지우지 않는다)
    void beginOverlay();
    // 오프스크린 대상을 묶고 지운다 (장면 파이프라인으로 그린다). 다 그리면 endTarget으로 셰이더가 읽는 상태로 돌린다.
    // 그 뒤 백버퍼에 그리려면 beginScene/beginOverlay를 다시 부른다. 깊이만 대상은 clearColor를 쓰지 않는다
    void beginTarget(RenderTarget target, const f32 clearColor[4], f32 clearDepth);
    void endTarget(RenderTarget target);
    void setViewport(f32 x, f32 y, f32 width, f32 height); // 백버퍼 픽셀, 왼쪽 위 원점 (가위 영역도 같이)
    void setPipeline(Pipeline pipeline);
    void pushConstants(const void* data, u32 bytes); // 64바이트까지, 다음 draw에 쓰인다
    void setVertexBuffer(Buffer buffer, u32 stride);
    void setIndexBuffer(Buffer buffer);
    void drawIndexed(u32 indexCount, u32 instanceCount);
    void draw(u32 vertexCount, u32 instanceCount);
    void textureBarrier(Texture texture, TextureState before, TextureState after);
    // 업로드 링(Device::allocUpload)의 offset에서 텍스처 영역으로 복사한다. rowPitch = Device::copyPitch
    void copyToTexture(Texture texture, u32 x, u32 y, u32 width, u32 height, u64 uploadOffset, u32 rowPitch);
    // GPU 시간 구간 (Device::gpuTimes). 한 프레임에 구간마다 한 번씩, begin 다음 end
    void beginTimer(u32 timer);
    void endTimer(u32 timer);

private:
    friend class Device;
    Device* m_device = nullptr;
};

class Device {
public:
    static constexpr u32 kFramesInFlight = 2;
    static constexpr u32 kPushConstantBytes = 64;

    Device() = default;
    ~Device();
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    bool init(const DeviceDesc& desc);
    const std::string& initError() const;
    void shutdown();
    void resize(u32 width, u32 height);
    u32 width() const;
    u32 height() const;
    const GpuInfo& gpu() const;

    // 정적 버퍼: GPU 메모리에 만들고 data를 복사한다 (로딩 때, GPU를 기다린다)
    Buffer createBuffer(BufferUsage usage, const void* data, u64 bytes);
    // 셰이더가 읽는 텍스처. 처음 상태는 ShaderRead (내용은 0)
    Texture createTexture(u32 width, u32 height, Format format);
    // 밉이 있는 정적 텍스처 (모델 텍스처 캐시). 상태는 ShaderRead
    Texture createTexture(const TextureInit& init);
    u32 textureIndex(Texture texture) const; // g_textures[] 번호
    // 오프스크린 대상 (만들 때 한 번, GPU를 기다린다. 지우지 않으므로 크기를 정해 하나 만들어 계속 쓴다)
    RenderTarget createRenderTarget(u32 width, u32 height);
    // 깊이만 있는 오프스크린 대상 (그림자 맵, Target::Shadow 파이프라인). renderTargetTexture는 그 깊이를 R32 float로 읽는다
    RenderTarget createDepthTarget(u32 width, u32 height);
    Texture renderTargetTexture(RenderTarget target) const; // 색 (깊이만 대상은 깊이, 처음 상태 ShaderRead)
    Pipeline createPipeline(const PipelineDesc& desc);
    // 같은 핸들의 파이프라인을 새 desc로 바꾼다 (셰이더 핫 리로드). GPU를 기다리므로 프레임 밖에서만 부른다
    bool recreatePipeline(Pipeline pipeline, const PipelineDesc& desc);

    // 프레임: false면 이번 프레임은 그리지 않는다 (최소화, 스왑체인 다시 만듦 등)
    bool beginFrame();
    CommandList& cmd();
    // 이번 프레임 업로드 링에서 잘라 준다 (CPU 쓰기, GPU 읽기). 공간이 없으면 nullptr. offset은 링 버퍼 안 바이트 위치
    u8* allocUpload(u64 bytes, u32 alignment, u64* offset);
    u32 uploadBufferIndex() const; // 업로드 링의 g_buffers[] 번호
    u32 copyPitch(u32 width, u32 bytesPerPixel) const; // copyToTexture 줄 간격
    u32 copyAlignment() const;                         // copyToTexture 시작 위치 정렬
    // 제출하고 보여 준다. capture가 있으면 GPU를 기다려 백버퍼를 읽는다
    void endFrame(bool vsync, Capture* capture = nullptr);
    void waitIdle();

    const GpuTimes& gpuTimes() const;
    GpuMemory gpuMemory() const;

    struct Impl;
    Impl* impl() const { return m; } // 백엔드 파일(rhi/<api>/) 전용

private:
    Impl* m = nullptr;
};

// 디버그 UI(Dear ImGui) 그리기 백엔드. MEALKIT_DEBUG_UI 빌드에만 있다 (rhi/<api>/ImGui*.cpp).
bool imguiInit(Device& device);
void imguiShutdown(Device& device);
void imguiNewFrame();
void imguiRender(Device& device); // ImGui::GetDrawData()를 지금 명령 목록에 그린다 (beginOverlay 뒤)
// 장치 텍스처를 ImGui::Image로 띄우는 ImTextureID 값 (DX12: GPU 서술자, Vulkan: 디버그 UI 서술자 세트, 처음 부를 때 만든다)
u64 imguiTextureId(Device& device, Texture texture);

} // namespace mealkit::rhi
