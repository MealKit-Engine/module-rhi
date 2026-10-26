// 디버그 UI(Dear ImGui) DX12 그리기 백엔드. 텍스처 서술자는 장치의 bindless 힙 텍스처 칸에서 빌린다.
#include "mealkit/rhi/dx12/Dx12.h"

#include <imgui.h>
#include <imgui_impl_dx12.h>

namespace mealkit::rhi {
namespace {

void allocSrv(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu) {
    auto* m = static_cast<Device::Impl*>(info->UserData);
    const u32 slot = m->allocTextureSlot();
    *cpu = m->srvCpu(slot);
    *gpu = m->srvGpu(slot);
}

void freeSrv(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE) {
    auto* m = static_cast<Device::Impl*>(info->UserData);
    m->freeTextureSlot(static_cast<u32>((cpu.ptr - m->srvHeap->GetCPUDescriptorHandleForHeapStart().ptr) / m->srvStride));
}

} // namespace

bool imguiInit(Device& device) {
    Device::Impl* m = device.impl();
    ImGui_ImplDX12_InitInfo info;
    info.Device = m->device.Get();
    info.CommandQueue = m->queue.Get();
    info.NumFramesInFlight = static_cast<int>(Device::kFramesInFlight);
    info.RTVFormat = kOverlayFormat;
    info.DSVFormat = DXGI_FORMAT_UNKNOWN;
    info.UserData = m;
    info.SrvDescriptorHeap = m->srvHeap.Get();
    info.SrvDescriptorAllocFn = allocSrv;
    info.SrvDescriptorFreeFn = freeSrv;
    return ImGui_ImplDX12_Init(&info);
}

void imguiShutdown(Device& device) {
    device.waitIdle();
    ImGui_ImplDX12_Shutdown(); // 사용자 텍스처의 GPU 자원도 지운다
}

void imguiNewFrame() { ImGui_ImplDX12_NewFrame(); }

void imguiRender(Device& device) { ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), device.impl()->list.Get()); }

u64 imguiTextureId(Device& device, Texture texture) {
    Device::Impl* m = device.impl();
    return texture ? m->srvGpu(m->textures[texture.id - 1].slot).ptr : 0; // 같은 bindless 힙 (ImGui 백엔드가 이 힙을 묶는다)
}

} // namespace mealkit::rhi
