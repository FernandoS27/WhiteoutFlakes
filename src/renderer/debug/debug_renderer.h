#pragma once

#include "whiteout/flakes/types.h"
#include "gfx/gfx.h"
#include "render_target.h"
#include "types.h"

#include <cstring>
#include <vector>

namespace whiteout::flakes::renderer { class RenderService; }

namespace whiteout::flakes::renderer::debug {

template <class LV>
void DrawWireLines(gfx::IGFXDevice*      gfx,
                   gfx::IGFXCommandList* cmd,
                   gfx::BufferHandle     cbPerFrame,
                   const std::vector<LV>& lines) {
    if (lines.empty() || !gfx) return;
    gfx::BufferDesc bd;
    bd.size          = static_cast<u32>(sizeof(LV) * lines.size());
    bd.usage         = gfx::BufferUsage::Vertex | gfx::BufferUsage::CpuWritable;
    bd.ringSlotsHint = 4;  // one-shot per debug draw
    gfx::BufferHandle tempVB = gfx->CreateBuffer(bd);
    if (tempVB == gfx::BufferHandle::Invalid) return;

    if (void* mapped = gfx->MapBuffer(tempVB)) {
        std::memcpy(mapped, lines.data(), sizeof(LV) * lines.size());
        gfx->UnmapBuffer(tempVB);

        cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 0, cbPerFrame);
        cmd->BindVertexBuffer(0, tempVB, sizeof(LV));
        cmd->Draw(static_cast<u32>(lines.size()), 0);
    }
    gfx->Destroy(tempVB);
}

class DebugRenderer {
public:
    explicit DebugRenderer(RenderService& rs) noexcept : rs_(rs) {}

    bool CreateResources();
    void DestroyResources();

    void RenderGrid();
    void RenderCollisions();
    void RenderLightMarkers();
    void RenderViewCube();

    Rect GetViewCubeRect() const;
    i32  HitTestViewCube(i32 mx, i32 my) const;
    void SetViewCubeHovered(bool h) noexcept { vcHovered_ = h; }
    bool IsViewCubeHovered()       const noexcept { return vcHovered_; }

    static constexpr i32 kViewCubeSize = 120;

private:

    bool CreateGridResources();

    bool CreateViewCubeResources();

    RenderService& rs_;

    gfx::BufferHandle gridVB_        = gfx::BufferHandle::Invalid;
    i32               gridVertCount_ = 0;

    static constexpr const char* kViewCubeFaceTexName = "debug.viewCubeFace";

    gfx::BufferHandle   vcCubeVB_    = gfx::BufferHandle::Invalid;
    gfx::BufferHandle   vcCubeIB_    = gfx::BufferHandle::Invalid;
    gfx::BufferHandle   vcOutlineVB_ = gfx::BufferHandle::Invalid;
    gfx::BufferHandle   vcHomeVB_    = gfx::BufferHandle::Invalid;
    gfx::TextureHandle  vcFaceTex_   = gfx::TextureHandle::Invalid;

    gfx::ShaderHandle   viewCubeVS_  = gfx::ShaderHandle::Invalid;
    gfx::ShaderHandle   viewCubePS_  = gfx::ShaderHandle::Invalid;

    gfx::PipelineHandle viewCubePSOHdr_ = gfx::PipelineHandle::Invalid;
    gfx::PipelineHandle viewCubePSOSd_  = gfx::PipelineHandle::Invalid;
    bool                vcHovered_   = false;
};

}
