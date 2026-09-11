#pragma once

// ============================================================================
// Sc2Compose — the joins between the measured kernels.
//
// Every kernel in `particle_stages_sc2.h` is gated against a golden one at a
// time. Nothing gates the wiring between them, and this phase's own lesson is
// that the seam between two measured things is the part nobody measures — so
// the wiring lives here, in a unit that takes plain structs and returns plain
// structs, rather than inside `Emitter2::InternalUpdate` where the only way to
// test it would be to stand up a whole emitter.
//
// Three joins, in the order a frame runs them:
//
//   * @ref Sc2ParticleStore — the element pool. The kernels take
//     `std::span<Sc2SpawnedElement>` and an `Sc2ElementList` over indices into
//     it; something has to own both and hand out slots.
//   * @ref Sc2BatchDescFrom — the load-time desc to the batch row OP15
//     measured. Pure, and the one place `Sc2EmitterDesc::Look` is read for the
//     shader's constants.
//   * @ref Sc2BuildQuads — the live list to triangles. Walks the list the way
//     retail's draw does, expands each particle through `Sc2ExpandQuad`, and
//     writes the shared 48-byte `Vertex`.
// ============================================================================

#include "particle_stages_sc2.h"
#include "sc2_emitter_desc.h"
#include "renderer/types.h"
#include "types.h"
#include "whiteout/flakes/types.h"

#include <span>
#include <vector>

namespace whiteout::flakes::renderer::particle {

/// The element pool: retail's `pElementBuffer` and the array it links over.
///
/// Retail carves 192-byte nodes out of 1104-byte blocks from a global arena
/// (OP8b) and threads the live list through them. The block arena is an
/// allocator detail — what is observable is the ORDER, and OP8b settles it:
/// elements come back in carve order and go into the live list in **append**
/// order, so the head is the oldest particle and nothing sorts or recycles.
/// A flat array with a free list reproduces that exactly and costs one
/// allocation.
struct Sc2ParticleStore {
    std::vector<Sc2SpawnedElement> elements;
    /// The analytic path's vertex, written ONCE when the slot is acquired and
    /// never touched again — which is the whole reason `Sc2VertexBody` has no
    /// second chance to correct anything. Parallel to @ref elements.
    ///
    /// One vertex per particle rather than four: the four differ only in the
    /// corner pair, and the corner is `kSc2Corners[k]` for every particle
    /// alive. That is what retail's hardware-instanced path stores too.
    std::vector<Sc2GpuVertex> vertices;
    Sc2ElementList list;
    /// Retail's freed-`ParticleVB`-slot list. It stays EMPTY here, by
    /// construction rather than by omission: `Sc2SpawnedElement::vbSlot`
    /// indexes a shared arena every emitter draws out of, and @ref vertices
    /// keeps each particle's vertex at the element's own index instead — so
    /// there is no second index space to hand back, and `Sc2RetireExpired`
    /// guards on `vbSlot != -1`. Kept as a field because the arena is what a
    /// batched draw would need if one is ever built.
    Sc2RecycleArray recycle;

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

/// A `Matrix44f` as the sixteen floats the SC2 kernels take.
///
/// A straight row-major copy, because both sides use the same convention:
/// `Matrix44f` puts the translation in `data[3][0..2]` and `vs::MulPointMat4`
/// reads it at `m[12..14]`. Pinned by a test rather than asserted here — it is
/// exactly the kind of agreement that is true until someone changes one side.
std::array<f32, 16> Sc2Mat16(const Matrix44f& m);

/// The four camera uniforms — `p_vBillboardRight/Up`, `p_vCameraDirection` and
/// `p_vEyePos` — out of a world-to-view matrix.
///
/// Retail hands the shader these four from its render context and OP12 takes
/// them as independent inputs, so no gate measures how they are DERIVED; this
/// is our derivation and its test is the round trip. The one thing the goldens
/// do fix is the handedness: their basis is `(+X, +Z, +Y)` and `+X x +Z` is
/// `-Y`, so `direction` is `-(right x up)`.
Sc2QuadCamera Sc2CameraFromView(const Matrix44f& worldToView);

/// The load-time half of the batch row (OP15) out of the converted `PAR_`.
Sc2BatchDesc Sc2BatchDescFrom(const Sc2EmitterDesc& d);

/// The runtime words `CParticleSystem::Init` derives from the record.
struct Sc2InitWords {
    u32 stateFlags = 0; ///< `+0x120`.
    u32 emitFlags = 0;  ///< `+0x34C`, the u24 word.
};

/// `CParticleSystem::Init`'s flag setup (4.8 `0x102923140`).
///
/// The order is the contract: the fallback force pair ASSIGNS `stateFlags`, so
/// the two time-scale bits written before it are lost on those emitters and
/// everything written after it survives.
Sc2InitWords Sc2InitRuntimeWords(const Sc2EmitterDesc& d);

/// One animation player the actor layer samples a `PAR_`'s squirt keys
/// against: a container, its unwrapped time, and whether it loops.
using Sc2ClockSample = renderer::model::FrameState::Sc2AnimPlayer;

/// What one frame's squirt crossing asks of the emitter.
struct Sc2Crossing {
    /// Per emission slot, the burst the crossed keys owe; 0 for none.
    std::vector<u32> bursts;
};

/// The actor layer's half of the squirt keys (design §7, R5): the crossing
/// over every slot's table, against the memory the actor keeps between frames.
///
/// Walks @p players the way `M3Anim_CollectCrossedKeys` walks the model's live
/// players (RE §16.7): each resolves the slot's track through its OWN
/// container, a looping player's frame wraps on that track's end, and a player
/// with no track — or a track that ends at 0 — is skipped without taking a
/// cursor. So a global loop playing above a sequence hides none of its keys.
///
/// A sequence change — a different player list, the first sample, or a clip
/// that does not loop stepping backwards — does what `UpdateEmitterState`
/// does: primes each cursor one frame BEHIND its playhead, so the first window
/// is `[now − dt, now]` and a key on the sequence's first frame fires. A
/// playhead that has not moved owes nothing, although retail would re-read the
/// keys it crossed last (design §8).
Sc2Crossing Sc2CrossSquirtKeys(const Sc2EmitterDesc& d, std::span<const Sc2ClockSample> players,
                               Sc2SquirtMemory& memory, i32 frameDtMs);

/// `M3Anim_GetActiveSequenceIndex(state, 1)`, the index `UpdateEmitterState`
/// watches for the pre-roll: the sequence of the first player in RETAIL's list
/// that is not fading out, or −1 when every one is or nothing plays (RE
/// §16.33). Retail's list breaks a priority tie newest first, and a global
/// loop is the oldest player there is, so within the leading priority a host
/// play wins however the sampler ordered them. Not the player list itself — a
/// global loop starting, or another container of the same sequence, moves
/// nothing.
i32 Sc2ActiveSequence(std::span<const Sc2ClockSample> players);

/// The shader's static branch set for this emitter.
///
/// `flipbookUv` and `uvRandomOffset` are per texture SLOT in the shader and
/// come from the material, not from `PAR_`; slot 0 is what a single-layer
/// particle samples, so that is what this fills. A multi-layer material has to
/// build one of these per slot.
Sc2QuadFlags Sc2QuadFlagsFrom(const Sc2EmitterDesc& d, bool flipbookUv,
                              bool uvRandomOffset);

/// One stored vertex as the vertex DECLARATION hands it to the shader.
///
/// The hardware widens the u16 pairs and normalises the UBYTE4 colours; that
/// decode is the declaration's and no gate measures it, so it is written out
/// here rather than hidden. The packing is the one OP6 pinned for the colour
/// nodes: alpha in the high byte, then r, g, b.
Sc2QuadInput Sc2QuadInputFrom(const Sc2GpuVertex& v);

/// How @ref Sc2BuildQuads orders an emitter: `PAR_.flags` Sort (0x1) by view
/// depth, or SortHeight (0x80) by the element's height — both as the ints
/// `BuildRenderBatch_CPU` keys them on.
enum class Sc2SortKey : u8 { None, Depth, Height };

/// Walk the live list and write two triangles per particle.
///
/// The walk is FORWARD from the head, which is retail's order for every
/// emitter that does not set `Sort` — `UploadGpuParticles` only walks backward
/// from the tail under `PAR_.flags & 0x100`, and the draw dispatcher picks
/// head or tail by that same bit. @p sortReverse is that bit.
///
/// Returns how many particles were written. A particle whose instance type is
/// past the eleven the shader has is SKIPPED rather than drawn as a
/// billboard; no shipped record carries one.
///
/// With @p sort the walk is then ordered as `BuildRenderBatch_CPU` orders it
/// (RE 8.4, `PAR_.flags` Sort 0x1), on INT keys read off the element's own
/// position — never taken into world space, a local emitter's included:
/// Sort's key is the bit pattern of the depth along the camera, which orders
/// like depth only in front of the eye, and SortHeight's (0x80) is the bits of
/// position z. Emitted back to front unless @p sortReverse flips it. Ties keep
/// the walk order, where retail's introsort promises none (design §8).
///
/// Everything above runs in the runtime's SC2 units, the camera's eye
/// included; @p hostScale takes only the written corner positions into the
/// host's units.
usize Sc2BuildQuads(const Sc2ParticleStore& store, const Sc2QuadBatch& batch,
                    const Sc2QuadCamera& camera, const Sc2QuadFlags& flags,
                    bool sortReverse, std::vector<Vertex>& out,
                    Sc2SortKey sort = Sc2SortKey::None, f32 hostScale = 1.0f);

} // namespace whiteout::flakes::renderer::particle
