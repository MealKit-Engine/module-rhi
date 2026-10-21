#pragma once

#include <string>

// GPU 캡처 도구 연결 (그래픽 프로그래머용, docs/ROADMAP.md 2단계 "GPU 캡처 버튼과 에디터 보기 모드").
// 디버그 UI가 있는 빌드(에디터·개발 exe)에서 --gpu-capture로 켜면, 장치를 만들기 전에 설치된 RenderDoc(DX12·Vulkan) 또는
// PIX(DX12, WinPixGpuCapturer)를 불러오고, 버튼·단축키가 다음 프레임을 캡처한다. 도구가 없으면 아무것도 하지 않고 버튼을 숨긴다.
// 출시 빌드(build.bat Export)에는 코드가 없다. 도구 DLL은 저장소에 없다 (RenderDoc 헤더만 third_party/renderdoc).
namespace mealkit::rhi {

// Device::init이 장치를 만들기 전에 부른다 (DeviceDesc::gpuCapture). dx12: PIX도 찾는다
void loadGpuCaptureTool(bool dx12);
// 불러온 도구 이름 ("RenderDoc", "PIX"). 없으면 nullptr (버튼을 숨긴다)
const char* gpuCaptureTool();
// 다음 프레임을 캡처한다. openViewer: RenderDoc 창을 연다 (자동 시험은 false). 도구가 없으면 false
bool captureNextGpuFrame(bool openViewer = true);
// 캡처 파일이 가는 곳 (상태 표시줄·로그)
std::string gpuCaptureFolder();

} // namespace mealkit::rhi
