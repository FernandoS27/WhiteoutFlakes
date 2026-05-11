#pragma once

#include "whiteout/flakes/types.h"
#include "bls_mat_params.h"
#include "bls_permuter.h"
#include "bls_program.h"
#include "gfx/gfx.h"

#include <unordered_map>
#include <vector>

namespace whiteout::flakes::renderer::bls {

enum class VertexLayoutKind : u8 {
    MeshSD        = 0,
    MeshSDTc2     = 1,
    MeshSDSkinned = 2,
    ParticleSD    = 3,

    ParticleSDSkinned = 7,

    MeshHDTangent = 4,

    MeshHDSkinned = 5,

    MeshHDSkinnedNoTangent = 6,

    // CornFx (BasicUV path, slot 0 only). Matches wc3_shaders/types/
    // vs_io.slang::CornFxVSInput's HAS_VC=1 / HAS_NT=0 / HAS_RANDOM=0
    // permute. CornEffectsVertex (corn_effects_vertex.h) is the host-side struct
    // with the same field offsets so VB writes are trivially memcpy-able.
    CornFx = 8,
};

std::span<const gfx::InputElement> LayoutFor(VertexLayoutKind k);

struct PsoRequest {
    const BlsProgram*      program    = nullptr;
    u32                    vsIndex    = 0;
    u32                    psIndex    = 0;
    MatParams              material;
    VertexLayoutKind       layout     = VertexLayoutKind::MeshSD;
    gfx::PrimitiveTopology topology   = gfx::PrimitiveTopology::TriangleList;

    gfx::Format            rtvFormat  = gfx::Format::R11G11B10_FLOAT;
    gfx::Format            dsvFormat  = gfx::Format::D24_UNORM_S8_UINT;
    bool                   wireframe  = false;

    bool                   lhClipSpace = false;
};

class BlsPsoBuilder {
public:
    explicit BlsPsoBuilder(gfx::IGFXDevice* device);
    ~BlsPsoBuilder();

    gfx::PipelineHandle GetOrBuild(const PsoRequest& request);

    void Clear();

private:
    gfx::IGFXDevice*                              device_ = nullptr;
    std::unordered_map<u64, gfx::PipelineHandle>  cache_;
};

}
