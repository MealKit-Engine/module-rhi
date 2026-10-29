// Debug UI (Dear ImGui) Vulkan drawing backend. Draws into the overlay view (no gamma conversion) with dynamic rendering.
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
    info.DescriptorPoolSize = 64; // fonts + user textures
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
    device.impl()->imguiTextures.clear(); // descriptor sets go away with the ImGui pool
    ImGui_ImplVulkan_Shutdown(); // also frees GPU resources of user textures
}

void imguiNewFrame() { ImGui_ImplVulkan_NewFrame(); }

void imguiRender(Device& device) { ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), device.impl()->current()); }

u64 imguiTextureId(Device& device, Texture texture) {
    if (!texture) return 0;
    Device::Impl* m = device.impl();
    for (const auto& [id, set] : m->imguiTextures)
        if (id == texture.id) return set;
    // A single-texture set from the ImGui backend's pool (returns the same value after that)
    const VkDescriptorSet set = ImGui_ImplVulkan_AddTexture(m->linearClamp, m->textures[texture.id - 1].view,
                                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    const u64 value = reinterpret_cast<u64>(set);
    m->imguiTextures.emplace_back(texture.id, value);
    return value;
}

} // namespace mealkit::rhi
