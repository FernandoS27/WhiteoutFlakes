#pragma once

// ============================================================================
// One `.prt` channel read at one site.
//
// Every channel sample in the emitter names its id three times —
// `d.Has(id) ? d.Channel(id).EvalScalar(seed, id, ctx) : absent` — and a site
// that mistypes one of the three samples a different channel's random stream
// without failing anywhere. The sampler binds the desc, the seed and the
// context once, so the id is written once per read.
//
// Evaluation is stateless: a channel's value is a function of the seed, the id
// and the context, with no stream behind it. So reading through the sampler is
// the same number as reading the path directly, whatever the order.
// ============================================================================

#include "d3_channels.h"
#include "d3_emitter_desc.h"
#include "d3_path.h"
#include "types.h"

namespace whiteout::flakes::renderer::particle::d3 {

struct ChannelSampler {
    const EmitterDesc& d;
    u32 seed;
    const EvalCtx& ctx;

    bool Has(i32 id) const {
        return d.Has(id);
    }

    f32 EvalScalar(i32 id) const {
        return d.Channel(id).EvalScalar(seed, id, ctx);
    }
    Vector3f EvalVector(i32 id) const {
        return d.Channel(id).EvalVector(seed, id, ctx);
    }
    Vector4f EvalColor(i32 id) const {
        return d.Channel(id).EvalColor(seed, id, ctx);
    }
    i32 EvalInt(i32 id) const {
        return d.Channel(id).EvalInt(seed, id, ctx);
    }

    /// The channel, or @p absent when the asset has no path for it.
    f32 Scalar(i32 id, f32 absent) const {
        return Has(id) ? EvalScalar(id) : absent;
    }

    /// A per-frame rate channel in per-second units, or 0 when absent.
    f32 PerSecond(i32 id) const {
        return Has(id) ? EvalScalar(id) * kFramesPerSecond : 0.0f;
    }

    /// `rate·60 + (now − prev)·invDt`: a rate channel plus an offset channel
    /// differentiated against its value last step, @p prev advanced to `now`.
    /// Every motion model with a rate-and-offset pair drives itself this way.
    ///
    /// @p reversed takes `prev − now` instead. Channel 16's pair is the one
    /// that does, re-checked at instruction level — reproduce it, do not repair
    /// it.
    f32 Differentiated(i32 rate, i32 offset, f32& prev, f32 invDt, bool reversed = false) const {
        f32 w = PerSecond(rate);
        if (Has(offset)) {
            const f32 now = EvalScalar(offset);
            w += (reversed ? prev - now : now - prev) * invDt;
            prev = now;
        }
        return w;
    }
};

} // namespace whiteout::flakes::renderer::particle::d3
