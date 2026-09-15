#include "renderer/particle/d3_emitter.h"

#include "io/d3/d3_sno_cache.h"

#include <algorithm>
#include <cmath>

namespace whiteout::flakes::renderer::particle::d3 {

/// @brief Seed one particle's four UV animation states, `MatTex_InitUvState`
///        @0x71000F7C60 once per stage, as `ParticleSystem_EmitParticle` does.
///
/// Everything here belongs to the PARTICLE. The engine draws the initial phase,
/// the rate jitter and the flip-book's start frame "so instances of the same
/// effect are desynchronised" — and for a particle system the instances are the
/// particles, one 72-byte state each at `particle+16 + 72*stage`. Without the
/// draws every member of a puff sits on the same tile of the same sheet, which
/// is what 12,361 of the corpus's 13,897 mode-2 entries are authored against.
///
/// The draws come off the PARTICLE's own stream rather than the engine's global
/// one, which is a deliberate difference: the engine's is a process-wide counter
/// no trace could reproduce, and the particle's is already the seed every
/// channel of this particle uses. The ORDER is the engine's — stages ascending,
/// and within a mode-2 stage the U phase, the V phase, then the U, V and
/// rotation rate jitters — so a stage that takes no draw does not shift the
/// ones after it.
void Emitter::SeedUvStates(ParticleState& st) const {
    const MaterialDesc& m = d3desc_->d3mat;
    MwcRng rng = MwcRng::Seed(st.seed ^ kUvSeedSalt);
    for (u32 s = 0; s < MaterialDesc::kMaxLayers; ++s) {
        if (m.setLayer[s] < 0)
            continue;
        const MaterialLayer& L = m.layers[static_cast<usize>(m.setLayer[s])];
        ParticleState::UvState& u = st.uv[s];
        if (L.uv.mode == ::whiteout::flakes::io::D3UvMode::Anim2D) {
            if (!L.atlas || L.atlas->frames.empty())
                continue;
            const i32 count = static_cast<i32>(L.atlas->frames.size());
            const u32 span = static_cast<u32>(L.atlasFrameRange) + 1u;
            i32 frame = L.atlasFrameBase + static_cast<i32>(rng.Next() % span);
            // `>=`, not `>`: the engine clamps to count-1 with a `>=` test, so a
            // range that overruns the sheet lands on the last frame rather than
            // wrapping.
            if (frame >= count - 1)
                frame = count - 1;
            if (frame < 0)
                frame = 0;
            u.cursor = static_cast<f32>(frame);
            u.cursorRate = L.atlasRate;
            if (L.atlasRateJitter != 0.0f)
                u.cursorRate += L.atlasRateJitter * rng.NextUnit();
        } else if (L.uv.mode == ::whiteout::flakes::io::D3UvMode::ScaleRotateScroll) {
            u.u = L.uv.randomPhaseU ? rng.NextUnit() : L.uv.offset.x;
            u.v = L.uv.randomPhaseV ? rng.NextUnit() : L.uv.offset.y;
            u.uRate = L.uv.scrollPerSec.x;
            if (L.uv.scrollJitter.x != 0.0f)
                u.uRate += L.uv.scrollJitter.x * rng.NextUnit();
            u.vRate = L.uv.scrollPerSec.y;
            if (L.uv.scrollJitter.y != 0.0f)
                u.vRate += L.uv.scrollJitter.y * rng.NextUnit();
            u.rotRate = L.uv.rotatePerSec;
            if (L.uv.rotateJitter != 0.0f)
                u.rotRate += L.uv.rotateJitter * rng.NextUnit();
            // Already clamped to 8x2pi and folded into [0, 2pi] by D3ReadUvXform,
            // which is what the engine does before the angle reaches the state.
            u.rot = L.uv.rotate;
        }
    }
}

/// @brief Advance them, `MatTex_TickUvStateEntry` @0x71000F8310 and
///        `Anim2D_AdvanceCursor` @0x710033EE90.
///
/// Mode 2 integrates the scroll and either WRAPS it into [0,1] or, with
/// `tAnim3.flAmount` set, CLAMPS it there — which turns a loop into a one-way
/// reveal. The angle is clamped to 8x2pi and folded the same way. Mode 3 steps
/// its flip-book: the length is `count - 0.0001`, which is what lets the last
/// frame be reached while a loop still wraps before `count`; a once-shot clamps
/// and zeroes its own rate.
void Emitter::StepUvStates(ParticleState& st, f32 dt) const {
    const MaterialDesc& m = d3desc_->d3mat;
    for (u32 s = 0; s < MaterialDesc::kMaxLayers; ++s) {
        if (m.setLayer[s] < 0)
            continue;
        const MaterialLayer& L = m.layers[static_cast<usize>(m.setLayer[s])];
        ParticleState::UvState& u = st.uv[s];
        if (L.uv.mode == ::whiteout::flakes::io::D3UvMode::Anim2D) {
            // `fabsf(rate) < 1e-6`, not `!= 0`: the engine's early-out is on the
            // magnitude, so a NEGATIVE rate plays the sheet backwards. One
            // shipped entry authors -30 fps.
            if (!L.atlas || std::fabs(u.cursorRate) < kEpsilon)
                continue;
            const f32 length = static_cast<f32>(L.atlas->frames.size()) - 0.0001f;
            if (length <= 0.0f)
                continue;
            f32 c = u.cursor + u.cursorRate * dt;
            if (L.atlasLoops) {
                while (c > length)
                    c -= length;
            } else if (c > length) {
                c = length;
                u.cursorRate = 0.0f;
            }
            u.cursor = c;
        } else if (L.uv.mode == ::whiteout::flakes::io::D3UvMode::ScaleRotateScroll) {
            u.u += u.uRate * dt;
            u.v += u.vRate * dt;
            if (L.uv.clampUv) {
                u.u = std::clamp(u.u, 0.0f, 1.0f);
                u.v = std::clamp(u.v, 0.0f, 1.0f);
            } else {
                u.u = ::whiteout::flakes::io::D3FoldUv(u.u);
                u.v = ::whiteout::flakes::io::D3FoldUv(u.v);
            }
            u.rot = ::whiteout::flakes::io::D3WrapAngle(u.rot + u.rotRate * dt);
        }
    }
}

} // namespace whiteout::flakes::renderer::particle::d3
