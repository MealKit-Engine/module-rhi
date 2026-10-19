// 디버그 UI(Dear ImGui) Vulkan 그리기 백엔드. 오버레이 뷰(감마 변환 없음)에 dynamic rendering으로 그린다.
#include "mealkit/rhi/vulkan/Vk.h"

#include <imgui.h>
#include <imgui_impl_vulkan.h>

namespace mealkit::rhi {

bool imguiInit(Device& device) {
    Device::Impl* m = device.impl();
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = m->apiVersion;
    info.Instance = m->instance;
    info.PhysicalDevice = m->physical;
    info.Device = m->device;
    info.QueueFamily = m->queueFamily;
    info.Queue = m->queue;
    info.DescriptorPoolSize = 64; // 글꼴 + 사용자 텍스처
    info.MinImageCount = 2;
    info.ImageCount = static_cast<u32>(m->swapImages.size());
    info.UseDynamicRendering = true;
    VkPipelineRenderingCreateInfoKHR& rendering = info.PipelineInfoMain.PipelineRenderingCreateInfo;
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &m->swapchainFormat;
    return ImGui_ImplVulkan_Init(&info);
}

void imguiShutdown(Device& device) {
    device.waitIdle();
    ImGui_ImplVulkan_Shutdown(); // 사용자 텍스처의 GPU 자원도 지운다
}

void imguiNewFrame() { ImGui_ImplVulkan_NewFrame(); }

void imguiRender(Device& device) { ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), device.impl()->current()); }

} // namespace mealkit::rhi
