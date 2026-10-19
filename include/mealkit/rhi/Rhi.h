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
//   고정 샘플러 `SamplerState g_linearClamp : register(s0)` ([[vk::binding(2, 0)]])
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

enum class Format : u8 { R8Unorm, Rgba8Unorm };
enum class BufferUsage : u8 { Vertex, Index16, Index32 };
enum class TextureState : u8 { ShaderRead, CopyDst };

// 정점 속성 (HLSL 입력 순서대로. DX12 시맨틱 이름, Vulkan location = 순서)
enum class VertexFormat : u8 { Float3, Unorm8x4 };
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
};

struct PipelineDesc {
    ShaderCode vs, ps;
    std::vector<VertexAttribute> vertexAttributes; // 비면 정점 버퍼 없음 (SV_VertexID)
    u32 vertexStride = 0;
    Target target = Target::Scene;
    bool cullBack = true;       // 시계 방향이 앞면 (DX12 기본)
    bool premultipliedBlend = false;
};

struct DeviceDesc {
    void* window = nullptr; // Window::nativeHandle()
    u32 width = 0;
    u32 height = 0;
    bool debug = false; // 검증 레이어 (DX12 debug layer, Vulkan validation)
};

// 캡처한 백버퍼 (RGBA8, 줄 순서대로)
struct Capture {
    u32 width = 0, height = 0;
    std::vector<u8> rgba;
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
    u32 textureIndex(Texture texture) const; // g_textures[] 번호
    Pipeline createPipeline(const PipelineDesc& desc);

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

} // namespace mealkit::rhi
