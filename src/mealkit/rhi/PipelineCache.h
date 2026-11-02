#pragma once

#include "mealkit/core/Hash.h"
#include "mealkit/rhi/Rhi.h"

#include <cstring>

// Pipeline cache on disk (docs/ROADMAP.md optimization 4): the backends keep compiled pipelines in DeviceDesc::pipelineCache
// (D3D12 ID3D12PipelineLibrary, Vulkan VkPipelineCache) and write it on shutdown. A cache from another driver/GPU is ignored
// (D3D12 rejects it on creation, Vulkan is checked by the header). Pipelines rebuilt by shader hot reload bypass it.
namespace mealkit::rhi {

// Everything that makes a pipeline different: the bytecode and every desc field (names in the D3D12 library)
inline u64 pipelineKey(const PipelineDesc& desc) {
    Hasher h;
    h.bytes(desc.vs.data, desc.vs.size);
    h.add(desc.vs.size);
    h.bytes(desc.ps.data, desc.ps.size);
    h.add(desc.ps.size);
    for (const VertexAttribute& a : desc.vertexAttributes) {
        h.bytes(a.semantic, std::strlen(a.semantic) + 1);
        h.add(a.format);
        h.add(a.offset);
    }
    h.add(desc.vertexStride);
    h.add(desc.target);
    const bool flags[] = {desc.cullBack, desc.cullFront, desc.premultipliedBlend, desc.wireframe, desc.additiveBlend, desc.depthTest, desc.depthWrite};
    h.bytes(flags, sizeof(flags));
    return h.value;
}

} // namespace mealkit::rhi
