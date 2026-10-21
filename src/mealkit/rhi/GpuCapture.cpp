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
// PIX: WinPixGpuCapturer.dll의 PIXGpuCaptureNextFrames(파일, 프레임 수) (pix3.h와 같은 이름, WinPixEventRuntime 없이)
using PixCaptureNextFrames = long(__stdcall*)(const wchar_t* fileName, unsigned int numFrames);
PixCaptureNextFrames g_pixCapture = nullptr;
std::string g_folder;
unsigned g_pixCount = 0;

// 설치 폴더의 DLL을 불러온다 (이미 들어와 있으면 같은 모듈)
void* loadFrom(const std::string& dir, const char* dll) {
    std::string error;
    void* lib = loadLibrary(dll, error); // 도구가 이 프로세스를 띄웠으면 이미 있다
    if (!lib && !dir.empty()) lib = loadLibrary(dir + "\\" + dll, error);
    return lib;
}

bool loadRenderDoc() {
    void* lib = loadFrom(environmentVariable("ProgramFiles") + "\\RenderDoc", "renderdoc.dll");
    if (!lib) return false;
    const auto getApi = reinterpret_cast<pRENDERDOC_GetAPI>(findSymbol(lib, "RENDERDOC_GetAPI"));
    if (!getApi || !getApi(eRENDERDOC_API_Version_1_6_0, reinterpret_cast<void**>(&g_renderdoc)) || !g_renderdoc) return false;
    g_renderdoc->SetCaptureFilePathTemplate((g_folder + "/capture").c_str());
    g_renderdoc->MaskOverlayBits(eRENDERDOC_Overlay_None, eRENDERDOC_Overlay_None); // 게임 화면에 글자를 덧그리지 않는다
    return true;
}

bool loadPix() {
    // %ProgramFiles%\Microsoft PIX\<버전>\WinPixGpuCapturer.dll (가장 새 버전 폴더)
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
        // RenderDoc 창이 연결해 캡처를 받는다 (이미 연결돼 있으면 그 창이 받는다)
        if (openViewer && !g_renderdoc->IsTargetControlConnected()) g_renderdoc->LaunchReplayUI(1, nullptr);
        MEALKIT_INFO("GPU capture: next frame (RenderDoc)");
        return true;
    }
    if (g_pixCapture) {
        const std::string file = g_folder + "/capture" + std::to_string(++g_pixCount) + ".wpix";
        std::wstring wide(file.begin(), file.end()); // 사용자 폴더 경로가 ASCII가 아니면 PIX가 거절할 수 있다
        const bool ok = g_pixCapture(wide.c_str(), 1) >= 0;
        MEALKIT_INFO("GPU capture: next frame (PIX) -> %s%s", file.c_str(), ok ? "" : " (failed)");
        return ok;
    }
    return false;
}

std::string gpuCaptureFolder() { return g_folder; }

#else // 출시 빌드: 캡처 도구를 불러오지 않는다

void loadGpuCaptureTool(bool) {}
const char* gpuCaptureTool() { return nullptr; }
bool captureNextGpuFrame(bool) { return false; }
std::string gpuCaptureFolder() { return {}; }

#endif

} // namespace mealkit::rhi
