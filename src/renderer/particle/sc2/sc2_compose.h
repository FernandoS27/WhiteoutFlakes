#pragma once

// ============================================================================
// sc2_compose — the joins between the measured kernels, as plain structs in
// and out: the element pool (@ref ParticleStore), the load-time batch row
// (@ref BatchDescFrom), the live list to triangles (@ref BuildQuads), and the
// adapters that feed them. See SC2_PARTICLE_DESIGN.md §16.1.
// ============================================================================

#include "renderer/particle/sc2/particle_stages_sc2.h"
#include "renderer/particle/sc2/sc2_emitter_desc.h"
#include "renderer/types.h"
#include "types.h"
#include "whiteout/flakes/types.h"

#include <array>
#include <span>
#include <vector>

namespace whiteout::flakes::renderer::particle::sc2 {

/// The element pool: retail's `pElementBuffer` and the array it links over, as
/// a flat array with a free list. Live-list order is append order, so the head
/// is the oldest particle (OP8b). See SC2_PARTICLE_RE.md §17.1.
struct ParticleStore {
    std::vector<SpawnedElement> elements;
    /// The analytic path's vertex, parallel to @ref elements: written ONCE
    /// when the slot is acquired, never touched again. One per particle, not
    /// four — the corner is `kCorners[k]`.
    std::vector<GpuVertex> vertices;
    ElementList list;
    /// Retail's freed-`ParticleVB`-slot list. EMPTY by construction: vertices
    /// sit at the element's own index, so `RetireExpired` guards on
    /// `vbSlot != -1`. See SC2_PARTICLE_RE.md §17.1.
    RecycleArray recycle;

    /// Sized to `min(authored, 0x200000 / 464)` by the caller — the cap is the
    /// desc's, because it is a load-time decision.
    void Init(u32 maxParticles);

    /// Take the next free slot and append it to the live list, or −1 when the
    /// pool is full. Retail tests the ceiling per ELEMENT and `break`s, so a
    /// batch that hits it half way creates exactly as many as fit.
    i32 Acquire();

    u32 AliveCount() const { return list.poolCount; }
    u32 Capacity() const { return static_cast<u32>(elements.size()); }
};

/// A `Matrix44f` as the sixteen floats the SC2 kernels take: a straight
/// row-major copy (translation `data[3][0..2]` is `m[12..14]`), pinned by a test.
std::array<f32, 16> Mat16(const Matrix44f& m);

/// The four camera uniforms — `p_vBillboardRight/Up`, `p_vCameraDirection` and
/// `p_vEyePos` — out of a world-to-view matrix. Our derivation, tested by the
/// round trip; `direction` is `-(right x up)`. See SC2_PARTICLE_RE.md §17.1.
QuadCamera CameraFromView(const Matrix44f& worldToView);

/// The load-time half of the batch row (OP15) out of the converted `PAR_`.
BatchDesc BatchDescFrom(const EmitterDesc& d);

/// The runtime words `CParticleSystem::Init` derives from the record.
struct InitWords {
    u32 stateFlags = 0; ///< `+0x120`.
    u32 emitFlags = 0;  ///< `+0x34C`, the u24 word.
};

/// `CParticleSystem::Init`'s flag setup (4.8 `0x102923140`).
///
/// The order is the contract: the fallback force pair ASSIGNS `stateFlags`, so
/// the two time-scale bits written before it are lost on those emitters and
/// everything written after it survives.
InitWords InitRuntimeWords(const EmitterDesc& d);

/// One animation player the actor layer samples a `PAR_`'s squirt keys
/// against: a container, its unwrapped time, and whether it loops.
using ClockSample = renderer::model::FrameState::Sc2AnimPlayer;

/// What an actor keeps between frames for one `PAR_`'s squirt keys: the
/// players it last walked — each one's container and where its playhead was —
/// and one sink per emission slot.
struct SquirtMemory {
    std::vector<u16> stcs;
    std::vector<i32> timeMs;
    bool valid = false;
    std::vector<KeySink> sinks;
};

/// What one frame's squirt crossing asks of the emitter.
struct Crossing {
    /// Per emission slot, the burst the crossed keys owe; 0 for none.
    std::vector<u32> bursts;
};

/// The actor layer's half of the squirt keys (design §7, R5): the crossing
/// over every slot's table, walking @p players as `M3Anim_CollectCrossedKeys`
/// does (RE §16.7). A sequence change primes each cursor one frame BEHIND its
/// playhead. See SC2_PARTICLE_RE.md §17.1.
Crossing CrossSquirtKeys(const EmitterDesc& d, std::span<const ClockSample> players,
                               SquirtMemory& memory, i32 frameDtMs);

/// `M3Anim_GetActiveSequenceIndex(state, 1)`, the index `UpdateEmitterState`
/// watches for the pre-roll: the first player not fading out, in RETAIL's list
/// order, or −1. See SC2_PARTICLE_RE.md §16.33 and §17.1.
i32 ActiveSequence(std::span<const ClockSample> players);

/// The shader's static branch set for this emitter. `flipbookUv` and
/// `uvRandomOffset` are per texture SLOT, from the material; this fills slot 0,
/// so a multi-layer material builds one per slot.
QuadFlags QuadFlagsFrom(const EmitterDesc& d, bool flipbookUv,
                              bool uvRandomOffset);

/// One stored vertex as the vertex DECLARATION hands it to the shader, the
/// unmeasured u16 widening and UBYTE4 normalise written out. Colour packing is
/// OP6's: alpha high, then r, g, b. See SC2_PARTICLE_RE.md §17.1.
QuadInput QuadInputFrom(const GpuVertex& v);

/// How @ref BuildQuads orders an emitter: `PAR_.flags` Sort (0x1) by view
/// depth, or SortHeight (0x80) by the element's height — both as the ints
/// `BuildRenderBatch_CPU` keys them on.
enum class SortKey : u8 { None, Depth, Height };

/// Walk the live list and write two triangles per particle; returns how many.
/// Forward from the head unless @p sortReverse (`PAR_.flags & 0x100`); with
/// @p sort, reordered on `BuildRenderBatch_CPU`'s INT keys, back to front.
/// Types past the eleven are SKIPPED. Runs in SC2 units; @p hostScale scales
/// only the written corners. See SC2_PARTICLE_RE.md §17.1.
usize BuildQuads(const ParticleStore& store, const QuadBatch& batch,
                    const QuadCamera& camera, const QuadFlags& flags,
                    bool sortReverse, std::vector<Vertex>& out,
                    SortKey sort = SortKey::None, f32 hostScale = 1.0f);

} // namespace whiteout::flakes::renderer::particle::sc2
