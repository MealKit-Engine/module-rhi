#pragma once

#include <string>

// GPU capture tool hookup (for graphics programmers, docs/ROADMAP.md stage 2 "GPU capture button and editor view modes").
// In builds with debug UI (editor/dev exe), when enabled with --gpu-capture, loads the installed RenderDoc (DX12/Vulkan) or
// PIX (DX12, WinPixGpuCapturer) before creating the device, and buttons/shortcuts capture the next frame. Without the tool it does nothing and hides the button.
// Shipping builds (build.bat Export) contain no code for this. Tool DLLs aren't in the repo (only the RenderDoc header in third_party/renderdoc).
namespace mealkit::rhi {

// Called by Device::init before creating the device (DeviceDesc::gpuCapture). dx12: also looks for PIX
void loadGpuCaptureTool(bool dx12);
// Name of the loaded tool ("RenderDoc", "PIX"). nullptr if none (hides the button)
const char* gpuCaptureTool();
// Captures the next frame. openViewer: opens the RenderDoc window (false for automated tests). false without the tool
bool captureNextGpuFrame(bool openViewer = true);
// Where capture files go (status bar/log)
std::string gpuCaptureFolder();

} // namespace mealkit::rhi
