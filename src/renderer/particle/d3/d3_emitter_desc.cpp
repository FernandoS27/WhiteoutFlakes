#include "renderer/particle/d3/d3_emitter_desc.h"

namespace whiteout::flakes::renderer::particle::d3 {

// ---------------------------------------------------------------------------
// The capability mask, derived as ParticleSystem_Spawn derives it (§9.1): the
// per-frame step branches on it. `IsInert` applies the engine's constancy test
// only to the channels that GATE a model. See §30.7.
// ---------------------------------------------------------------------------

void EmitterDesc::DeriveCapabilities() {
    caps = 0;
    // Every gating channel's identity is zero.
    auto live = [&](i32 id) {
        const Path& p = Channel(id);
        return !p.nodes.empty() && !p.IsInert(0.0f);
    };

    // All THREE orbit channels feed the bit, not just the radial pair: Spawn
    // ORs the ranges of ch 7, ch 8 and ch 9 together, so an asset that only
    // spins (angular speed, no radial motion) still enables the model.
    if (live(kChOrbitRadSpeed) || live(kChOrbitRadius) || live(kChOrbitAngSpeed))
        caps |= kCapOrbit;
    if (live(kChRadialSpeed) || live(kChRadialOffset))
        caps |= kCapRadial;
    if (live(kChOffsetA) || live(kChVelocityA) || live(kChAccelA))
        caps |= kCapTripleA;
    if (live(kChOffsetB) || live(kChVelocityB) || live(kChAccelB))
        caps |= kCapTripleB;
    if (live(kChSpinRate) || live(kChSpinAngle))
        caps |= kCapSpin;
    if (live(kChSeekSpeed) || live(kChSeekOffset))
        caps |= kCapSeek;
    // Rotation is FORCED OFF for the two foliage types, whatever the channels
    // say — the engine skips the whole block for them.
    if (!UsesWindSpring(systemType) && (live(kChRollRate) || live(kChRollAngle)))
        caps |= kCapRoll;

    // Channel 23 is tested apart from the spin bit, on its start lane's minimum
    // (`InterpolationPath_GetVectorRange` @0x7100375C00, then three `!= 0`), so
    // a NaN lane counts as authored.
    const Path& axis = Channel(kChSpinAxis);
    if (!axis.nodes.empty()) {
        Vector3f lo{axis.nodes[0].start.x, axis.nodes[0].start.y, axis.nodes[0].start.z};
        for (usize k = 1; k < axis.nodes.size(); ++k) {
            const Vector4f& s = axis.nodes[k].start;
            lo = {lo.x < s.x ? lo.x : s.x, lo.y < s.y ? lo.y : s.y, lo.z < s.z ? lo.z : s.z};
        }
        if (lo.x != 0.0f || lo.y != 0.0f || lo.z != 0.0f)
            caps |= kCapSpinAxis;
    }
    spinAxisConstant = axis.nodes.empty() ||
                       (axis.nodes.size() == 1 && axis.driver.mode == 0 &&
                        axis.nodes[0].start.x == axis.nodes[0].end.x &&
                        axis.nodes[0].start.y == axis.nodes[0].end.y &&
                        axis.nodes[0].start.z == axis.nodes[0].end.z);
}

} // namespace whiteout::flakes::renderer::particle::d3
