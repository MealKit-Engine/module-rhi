#include "mealkit/rhi/GpuCapture.h"

#if MEALKIT_DEBUG_UI
#include "mealkit/core/Log.h"
#include "mealkit/platform/DynamicLibrary.h"
#include "mealkit/platform/Paths.h"
#include "mealkit/platform/System.h"

#include <renderdoc_app.h>

#include <string>
#endif

namespace mealkit::rhi {

#if MEALKIT_DEBUG_UI

namespace {

RENDERDOC_API_1_6_0* g_renderdoc = nullptr;
// PIX: PIXGpuCaptureNextFrames(file, frame count) in WinPixGpuCapturer.dll (same name as pix3.h, without WinPixEventRuntime)
using PixCaptureNextFrames = long(__stdcall*)(const wchar_t* fileName, unsigned int numFrames);
PixCaptureNextFrames g_pixCapture = nullptr;
std::string g_folder;
unsigned g_pixCount = 0;

// Loads the DLL from the install folder (the same module if already loaded)
void* loadFrom(const std::string& dir, const char* dll) {
    std::string error;
    void* lib = loadLibrary(dll, error); // already present if the tool launched this process
    if (!lib && !dir.empty()) lib = loadLibrary(dir + "\\" + dll, error);
    return lib;
}

bool loadRenderDoc() {
    void* lib = loadFrom(environmentVariable("ProgramFiles") + "\\RenderDoc", "renderdoc.dll");
    if (!lib) return false;
    const auto getApi = reinterpret_cast<pRENDERDOC_GetAPI>(findSymbol(lib, "RENDERDOC_GetAPI"));
    if (!getApi || !getApi(eRENDERDOC_API_Version_1_6_0, reinterpret_cast<void**>(&g_renderdoc)) || !g_renderdoc) return false;
    g_renderdoc->SetCaptureFilePathTemplate((g_folder + "/capture").c_str());
    g_renderdoc->MaskOverlayBits(eRENDERDOC_Overlay_None, eRENDERDOC_Overlay_None); // don't draw text over the game screen
    return true;
}

bool loadPix() {
    // %ProgramFiles%\Microsoft PIX\<version>\WinPixGpuCapturer.dll (newest version folder)
    const std::string root = environmentVariable("ProgramFiles") + "\\Microsoft PIX";
    std::string dir;
    for (const std::string& version : listDirectories(root)) dir = root + "\\" + version;
    void* lib = loadFrom(dir, "WinPixGpuCapturer.dll");
    if (!lib) return false;
    g_pixCapture = reinterpret_cast<PixCaptureNextFrames>(findSymbol(lib, "PIXGpuCaptureNextFrames"));
    return g_pixCapture != nullptr;
}

} // namespace

void loadGpuCaptureTool(bool dx12) {
    if (g_renderdoc || g_pixCapture) return;
    g_folder = userDataDirectory() + "/MealKit/gpu-captures";
    createDirectories(g_folder);
    if (loadRenderDoc()) MEALKIT_INFO("GPU capture: RenderDoc loaded (F11 or the editor's View menu), captures in %s", g_folder.c_str());
    else if (dx12 && loadPix()) MEALKIT_INFO("GPU capture: PIX loaded (F11 or the editor's View menu), captures in %s", g_folder.c_str());
    else MEALKIT_WARN("GPU capture: neither RenderDoc nor PIX is installed (--gpu-capture ignored)");
}

const char* gpuCaptureTool() {
    if (g_renderdoc) return "RenderDoc";
    if (g_pixCapture) return "PIX";
    return nullptr;
}

bool captureNextGpuFrame(bool openViewer) {
    if (g_renderdoc) {
        g_renderdoc->TriggerCapture();
        // The RenderDoc window connects and receives the capture (if already connected, that window receives it)
        if (openViewer && !g_renderdoc->IsTargetControlConnected()) g_renderdoc->LaunchReplayUI(1, nullptr);
        MEALKIT_INFO("GPU capture: next frame (RenderDoc)");
        return true;
    }
    if (g_pixCapture) {
        const std::string file = g_folder + "/capture" + std::to_string(++g_pixCount) + ".wpix";
        std::wstring wide(file.begin(), file.end()); // PIX may reject user folder paths that aren't ASCII
        const bool ok = g_pixCapture(wide.c_str(), 1) >= 0;
        MEALKIT_INFO("GPU capture: next frame (PIX) -> %s%s", file.c_str(), ok ? "" : " (failed)");
        return ok;
    }
    return false;
}

std::string gpuCaptureFolder() { return g_folder; }

#else // shipping builds: capture tools aren't loaded

void loadGpuCaptureTool(bool) {}
const char* gpuCaptureTool() { return nullptr; }
bool captureNextGpuFrame(bool) { return false; }
std::string gpuCaptureFolder() { return {}; }

#endif

} // namespace mealkit::rhi
