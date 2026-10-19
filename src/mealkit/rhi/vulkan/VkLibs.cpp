// 외부 라이브러리 구현 (경고 끔, engine/CMakeLists.txt): volk(로더), VMA(메모리 할당)
#include "mealkit/rhi/vulkan/Vk.h"

#include "volk.c"

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>
