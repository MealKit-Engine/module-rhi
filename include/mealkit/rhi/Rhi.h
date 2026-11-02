#pragma once

#include "mealkit/core/Types.h"

#include <string>
#include <vector>

// Thin graphics API layer made by the engine (docs/CODING_MODEL.md "Graphics APIs and OS features", ROADMAP stage 2 "RHI layer").
// Only as much as the renderer uses: device/swap chain, frame command lists, static buffers, textures, pipelines, frame upload ring, capture.
// Its shape follows DX12 (command list recording, explicit barriers, bindless indices). One backend per build (MEALKIT_RHI: rhi/dx12/, rhi/vulkan/),
// with per-backend .cpp files implementing the same classes without virtual functions. Graphics API headers aren't included outside the backend folders.
//
// Binding (docs/DECISIONS.md 2026-10-19 "RHI binding layout"): every pipeline uses the same layout.
//   push constants 64 bytes (HLSL `[[vk::push_constant]] ConstantBuffer<T> : register(b0)`)
//   buffer array `ByteAddressBuffer g_buffers[] : register(t0, space1)` ([[vk::binding(0, 0)]]) — bufferIndex()
//   texture array `Texture2D g_textures[] : register(t0, space2)` ([[vk::binding(1, 0)]]) — textureIndex()
//   cube array `TextureCube g_cubes[] : register(t0, space3)` ([[vk::binding(4, 0)]]) — textureIndex() of cube textures (same index space)
//   static samplers `SamplerState g_linearClamp : register(s0)` ([[vk::binding(2, 0)]]),
//              `SamplerState g_linearWrap : register(s1)` ([[vk::binding(3, 0)]], anisotropy 8, model textures)
namespace mealkit::rhi {

// GPU feature inspection results. Written to the startup log and `--system-info` (min-spec hardware measurement records).
struct GpuInfo {
    std::string api;           // "Direct3D 12", "Vulkan 1.3.280"
    std::string adapter;       // adapter name (UTF-8)
    std::string driverVersion; // driver version "31.0.101.5186" (empty string if unknown)
    u32 vendorId = 0;          // 0x8086 Intel, 0x1002 AMD, 0x10DE NVIDIA
    u32 deviceId = 0;
    u64 dedicatedVideoMemory = 0;
    u64 sharedSystemMemory = 0;
    u32 featureLevel = 0;        // D3D_FEATURE_LEVEL (0xC100 = 12_1). 0 for Vulkan
    u32 shaderModel = 0;         // D3D_SHADER_MODEL (0x66 = 6.6). 0 for Vulkan
    u32 resourceBindingTier = 0; // 1~3. 0 for Vulkan
    u32 meshShaderTier = 0;      // 0 = none (for reference; the engine doesn't use it)
    u32 waveLaneCountMin = 0;
    u32 waveLaneCountMax = 0;
    bool uma = false; // shares memory with the CPU like integrated graphics
    std::string unsupported; // min-spec shortfalls found by the backend (Vulkan version/features). Empty if none
};

// Why it falls short of the min spec. Empty string if it meets it.
std::string gpuMinimumError(const GpuInfo& info);
// Inspects the GPU the device would pick, without a window. false and error if there's no GPU.
bool queryGpu(GpuInfo& out, std::string* error = nullptr);

// Handles: 0 = none
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
// Offscreen render target (TargetKind decides the format; draw with pipelines whose Target matches). Depth-only targets via createDepthTarget
struct RenderTarget {
    u32 id = 0;
    explicit operator bool() const { return id != 0; }
};

// Texture formats. Srgb converts to linear on read (color textures). BC is 4×4 block compression (BC7 = color 16 bytes, BC5 = two channels 16 bytes)
// Rgb9e5: HDR (shared exponent, 4 bytes. Sky/reflection cubemaps)
enum class Format : u8 { R8Unorm, Rgba8Unorm, Rgba8Srgb, Bc7Unorm, Bc7Srgb, Bc5Unorm, Rgb9e5 };

inline bool isBlockCompressed(Format f) { return f == Format::Bc7Unorm || f == Format::Bc7Srgb || f == Format::Bc5Unorm; }
// Bytes of one mip (no gaps between rows, block rows for BC)
inline u64 textureMipBytes(Format f, u32 width, u32 height) {
    if (isBlockCompressed(f)) return static_cast<u64>((width + 3) / 4) * ((height + 3) / 4) * 16;
    return static_cast<u64>(width) * height * (f == Format::R8Unorm ? 1u : 4u);
}

// Texture with contents (uploaded once on load, waits for the GPU). data is textureMipBytes each, in order from mip 0.
// For cubes, six faces (+X, -X, +Y, -Y, +Z, -Z) in order, each from mip 0 (shaders read via g_cubes[textureIndex()])
struct TextureInit {
    u32 width = 0, height = 0;
    u32 mipLevels = 1;
    Format format = Format::Rgba8Unorm;
    const void* data = nullptr;
    bool cube = false;
};
enum class BufferUsage : u8 { Vertex, Index16, Index32 };
enum class TextureState : u8 { ShaderRead, CopyDst };

// Vertex attributes (in HLSL input order. DX12 semantic names, Vulkan location = order)
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

// Format of the target a pipeline draws to (docs/DECISIONS.md 2026-10-27 "Post-processing")
enum class Target : u8 {
    Scene,   // HDR scene target (TargetKind::Scene): color R11G11B10F (linear) + depth D32 (reversed-Z: GREATER)
    Hdr,     // HDR color only (TargetKind::Hdr, bloom chain): R11G11B10F, no depth
    Overlay, // 8-bit color, no depth: the back buffer and TargetKind::Ldr. No gamma conversion, so shaders write sRGB values (post/UI)
    Shadow,  // depth-only target (createDepthTarget, shadow maps): no color, depth D32 (reversed-Z: GREATER). Pixel shaders only discard (alpha mask)
};

// Kinds of offscreen targets. The color is also a Texture shaders read (renderTargetTexture)
enum class TargetKind : u8 {
    Scene, // scene (Target::Scene): HDR color + depth
    Hdr,   // HDR color only (Target::Hdr)
    Ldr,   // 8-bit color only (Target::Overlay, same format as the back buffer): sRGB values read as is (debug UI/capture)
};

struct PipelineDesc {
    ShaderCode vs, ps;
    std::vector<VertexAttribute> vertexAttributes; // empty = no vertex buffer (SV_VertexID)
    u32 vertexStride = 0;
    Target target = Target::Scene;
    bool cullBack = true;       // clockwise is the front face (DX12 default)
    bool cullFront = false;     // discards front faces (inverted hull of outlines). Takes precedence over cullBack
    bool premultipliedBlend = false;
    // For editor view modes (render::DebugView): draw as lines, additive blend (overlap count = brightness), depth test/write off
    bool wireframe = false; // Vulkan draws filled faces if fillModeNonSolid is unavailable
    bool additiveBlend = false;
    bool depthTest = true; // Scene targets only
    bool depthWrite = true; // doesn't write even when depth testing (translucent particles)
};

struct DeviceDesc {
    void* window = nullptr; // Window::nativeHandle()
    u32 width = 0;
    u32 height = 0;
    bool debug = false; // validation layer (DX12 debug layer, Vulkan validation)
    bool gpuCapture = false; // loads RenderDoc/PIX before creating the device (rhi/GpuCapture.h, dev builds)
    std::string pipelineCache; // folder of the compiled pipelines file (rhi/PipelineCache.h, one per backend), read on init and written on shutdown. Empty = off
};

// Captured back buffer (RGBA8, in row order). If source is given, reads that offscreen target (TargetKind::Ldr) instead of the back buffer
// (reflection probe baking, values are the written sRGB as is). The target must have finished through endTarget this frame
struct Capture {
    RenderTarget source;
    u32 width = 0, height = 0;
    std::vector<u8> rgba;
};

// GPU time (timestamp queries). The meaning of section numbers (0..kMaxGpuTimers-1) is decided by the user (renderer passes).
// Results are from the most recent frame the GPU finished (kFramesInFlight frames late). Sections not used this frame are 0.
inline constexpr u32 kMaxGpuTimers = 8;
struct GpuTimes {
    bool valid = false; // timestamps are supported and at least one frame has finished
    f32 frameMs = 0.0f; // from the start to the end of the command list
    f32 timerMs[kMaxGpuTimers] = {};
};

// GPU memory: the budget the driver gives this process and current usage (DX12 QueryVideoMemoryInfo, Vulkan VK_EXT_memory_budget).
// Integrated graphics (UMA) share system memory, so the budget is based on shared memory. 0 if unknown
struct GpuMemory {
    u64 used = 0;
    u64 budget = 0;
};

class Device;

// One frame's command list (between Device::beginFrame and endFrame).
// Draw targets (Scene/Overlay) are bound with begin*, and copies (copyToTexture) unbind the bound target: call begin* again after copying.
class CommandList {
public:
    // Binds the back buffer (Target::Overlay, no gamma conversion, not cleared. If first in the frame the contents are undefined: post covers them)
    void beginOverlay();
    // Binds an offscreen target. If clearColor is nullptr, color isn't cleared and contents are kept (post passes, bloom additive blend).
    // Depth is always cleared to clearDepth. When done, endTarget returns it to the shader-readable state.
    // To draw to the back buffer after that, call beginOverlay again. Depth-only targets don't use clearColor
    void beginTarget(RenderTarget target, const f32 clearColor[4], f32 clearDepth);
    void endTarget(RenderTarget target);
    void setViewport(f32 x, f32 y, f32 width, f32 height); // back buffer pixels, top-left origin (scissor too)
    void setPipeline(Pipeline pipeline);
    void pushConstants(const void* data, u32 bytes); // up to 64 bytes, used by the next draw
    void setVertexBuffer(Buffer buffer, u32 stride);
    void setIndexBuffer(Buffer buffer);
    void drawIndexed(u32 indexCount, u32 instanceCount);
    void draw(u32 vertexCount, u32 instanceCount);
    void textureBarrier(Texture texture, TextureState before, TextureState after);
    // Copies from offset in the upload ring (Device::allocUpload) to a texture region. rowPitch = Device::copyPitch
    void copyToTexture(Texture texture, u32 x, u32 y, u32 width, u32 height, u64 uploadOffset, u32 rowPitch);
    // GPU time sections (Device::gpuTimes). Once per section per frame, begin then end
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

    // Static buffer: created in GPU memory with data copied in (at load time, waits for the GPU)
    Buffer createBuffer(BufferUsage usage, const void* data, u64 bytes);
    // Waits for the GPU and deletes (outside a frame: terrain chunks rebuilt by editor brushes). Handle numbers aren't reused
    void destroyBuffer(Buffer buffer);
    // Texture read by shaders. Initial state ShaderRead (contents 0)
    Texture createTexture(u32 width, u32 height, Format format);
    // Static texture with mips (model texture cache). State ShaderRead
    Texture createTexture(const TextureInit& init);
    u32 textureIndex(Texture texture) const; // g_textures[] index
    // Offscreen target (once at creation, waits for the GPU). Targets that follow the window size are recreated after destroyRenderTarget
    RenderTarget createRenderTarget(u32 width, u32 height, TargetKind kind);
    // Waits for the GPU and deletes (outside a frame. The renderer's window-size targets). Handle numbers aren't reused
    void destroyRenderTarget(RenderTarget target);
    // Depth-only offscreen target (shadow maps, Target::Shadow pipelines). renderTargetTexture reads that depth as R32 float
    RenderTarget createDepthTarget(u32 width, u32 height);
    Texture renderTargetTexture(RenderTarget target) const; // color (depth for depth-only targets, initial state ShaderRead)
    Pipeline createPipeline(const PipelineDesc& desc);
    // Replaces the pipeline of the same handle with a new desc (shader hot reload). Waits for the GPU, so call only outside a frame
    bool recreatePipeline(Pipeline pipeline, const PipelineDesc& desc);

    // Frame: if false, this frame isn't drawn (minimized, swap chain recreated, etc.)
    bool beginFrame();
    CommandList& cmd();
    // Carves from this frame's upload ring (CPU write, GPU read). nullptr if out of space. offset is the byte position in the ring buffer
    u8* allocUpload(u64 bytes, u32 alignment, u64* offset);
    u32 uploadBufferIndex() const; // g_buffers[] index of the upload ring
    u32 copyPitch(u32 width, u32 bytesPerPixel) const; // copyToTexture row pitch
    u32 copyAlignment() const;                         // copyToTexture start offset alignment
    // Submits and presents. If capture is given, waits for the GPU and reads the back buffer
    void endFrame(bool vsync, Capture* capture = nullptr);
    void waitIdle();

    const GpuTimes& gpuTimes() const;
    GpuMemory gpuMemory() const;

    struct Impl;
    Impl* impl() const { return m; } // backend files (rhi/<api>/) only

private:
    Impl* m = nullptr;
};

// Debug UI (Dear ImGui) drawing backend. Only in MEALKIT_DEBUG_UI builds (rhi/<api>/ImGui*.cpp).
bool imguiInit(Device& device);
void imguiShutdown(Device& device);
void imguiNewFrame();
void imguiRender(Device& device); // draws ImGui::GetDrawData() into the current command list (after beginOverlay)
// ImTextureID value for showing a device texture with ImGui::Image (DX12: GPU descriptor, Vulkan: debug UI descriptor set, created on first call)
u64 imguiTextureId(Device& device, Texture texture);

} // namespace mealkit::rhi
