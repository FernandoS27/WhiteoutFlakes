#pragma once

#include "bls_mat_params.h"
#include "bls_permuter.h"
#include "bls_program.h"
#include "gfx/gfx.h"
#include "whiteout/flakes/types.h"

#include <unordered_map>
#include <vector>

namespace whiteout::flakes::renderer::bls {

enum class VertexLayoutKind : u8 {
    MeshSD = 0,
    MeshSDTc2 = 1,
    MeshSDSkinned = 2,
    ParticleSD = 3,

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
    const BlsProgram* program = nullptr;
    u32 vsIndex = 0;
    u32 psIndex = 0;
    MatParams material;
    VertexLayoutKind layout = VertexLayoutKind::MeshSD;
    gfx::PrimitiveTopology topology = gfx::PrimitiveTopology::TriangleList;

    gfx::Format rtvFormat = gfx::Format::R11G11B10_FLOAT;
    gfx::Format dsvFormat = gfx::Format::D24_UNORM_S8_UINT;
    bool wireframe = false;

    bool lhClipSpace = false;
};

class BlsPsoTrace;

// Cumulative counters per session. Useful for confirming the trace
// replay actually pre-warmed every PSO the runtime needs:
//   replayCacheBuilds : PSOs built during BlsPsoTrace::Replay (startup)
//   runtimeCacheBuilds: PSOs built during a normal GetOrBuild call
//                       (i.e. cache misses outside the replay path —
//                       every one of these is a potential mid-frame
//                       stutter from a synchronous driver pipeline
//                       compile).
//   cacheHits         : GetOrBuild calls that hit the in-memory cache.
struct BlsPsoBuilderStats {
    u64 replayCacheBuilds = 0;
    u64 runtimeCacheBuilds = 0;
    u64 cacheHits = 0;
};

class BlsPsoBuilder {
public:
    explicit BlsPsoBuilder(gfx::IGFXDevice* device);
    ~BlsPsoBuilder();

    gfx::PipelineHandle GetOrBuild(const PsoRequest& request);

    void Clear();

    // Attach a trace recorder. Every cache miss in GetOrBuild forwards
    // its PsoRequest to the trace so the on-disk file can replay the
    // same keys on the next run's pre-warm. Pass nullptr to detach.
    void SetTrace(BlsPsoTrace* trace) {
        trace_ = trace;
    }

    // The trace's Replay() phase calls GetOrBuild from a "warmup"
    // context. Toggle these around it so the counters attribute builds
    // correctly. When `inReplay_` is true, cache misses are recorded
    // under replayCacheBuilds; otherwise under runtimeCacheBuilds.
    void SetReplayMode(bool on) {
        inReplay_ = on;
    }

    const BlsPsoBuilderStats& Stats() const {
        return stats_;
    }

private:
    gfx::IGFXDevice* device_ = nullptr;
    std::unordered_map<u64, gfx::PipelineHandle> cache_;
    BlsPsoTrace* trace_ = nullptr;
    BlsPsoBuilderStats stats_{};
    bool inReplay_ = false;
};

} // namespace whiteout::flakes::renderer::bls
