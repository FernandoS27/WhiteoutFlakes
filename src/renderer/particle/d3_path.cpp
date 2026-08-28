#include "renderer/particle/d3_path.h"

#include <algorithm>
#include <cmath>

namespace whiteout::flakes::renderer::particle::d3 {

namespace {

f32 Clamp01Engine(f32 v) {
    // The engine's own order: min against 1 first, then a separate `< 0` test
    // that substitutes zero. Written out because `std::clamp` would give the
    // same answer for every finite input but not for a NaN, and a NaN here
    // would otherwise reach the node lerp.
    const f32 hi = std::fmin(v, 1.0f);
    return (v < 0.0f) ? 0.0f : hi;
}

f32 SafeSqrt(f32 v) {
    return (v > 0.0f) ? std::sqrt(v) : 0.0f;
}

} // namespace

f32 ApplyDistribution(i32 distribution, f32 u) {
    switch (distribution) {
    case 0:
        return u;
    case 1:
        return std::fmin(u * u, 1.0f);
    case 2: {
        const f32 c = u - 0.5f;
        const f32 s = 2.0f * c * c;
        return Clamp01Engine(0.5f + ((c < 0.0f) ? -s : s));
    }
    case 3:
        return Clamp01Engine(1.0f - u * u);
    case 4: {
        const f32 c = u - 0.5f;
        const f32 s = 2.0f * c * c;
        return Clamp01Engine((c < 0.0f) ? s : (1.0f - s));
    }
    case 5: {
        const f32 v = 1.0f - SafeSqrt(u);
        return (v < 0.0f) ? 0.0f : std::fmin(v, 1.0f);
    }
    case 6: {
        const f32 c = u - 0.5f;
        const f32 h = 0.5f * SafeSqrt(2.0f * std::fabs(c));
        return Clamp01Engine((c < 0.0f) ? h : (1.0f - h));
    }
    case 7: {
        const f32 v = SafeSqrt(u);
        return (v < 0.0f) ? 0.0f : std::fmin(v, 1.0f);
    }
    case 8: {
        const f32 c = u - 0.5f;
        const f32 h = 0.5f * SafeSqrt(2.0f * std::fabs(c));
        return Clamp01Engine(0.5f + ((c < 0.0f) ? -h : h));
    }
    default:
        // The switch's own default, reached for anything outside 0..8. The
        // corpus holds nothing outside it over 798,941 paths, but a corrupt
        // file must not index off the end of a table.
        return 0.0f;
    }
}

bool EvalDriver(const Driver& d, const DriverInputs& in, f32& outMultiplier) {
    outMultiplier = 1.0f;
    f32 t = 0.0f;
    switch (d.mode) {
    case 0:
        // Disabled. The engine returns "not applied" and the caller leaves the
        // sample untouched, which is not the same as multiplying by lo..hi.
        return false;
    case 3:
        t = in.distNorm;
        break;
    case 6:
        t = in.heightNorm;
        break;
    default:
        // Every other mode reads gameplay state a viewer does not have: an
        // actor's health fraction, an attribute, a bone-table float. Behaving
        // as mode 0 leaves the channel unmodulated, which is the honest
        // answer; 6,209 of 798,941 authored paths land here.
        return false;
    }
    t = std::clamp(t, 0.0f, 1.0f);
    outMultiplier = d.lo + t * (d.hi - d.lo);
    return true;
}

bool Path::IsConstant() const {
    if (nodes.size() != 1)
        return false;
    const PathNode& n = nodes[0];
    const i32 c = components;
    if (n.start.x != n.end.x)
        return false;
    if (c >= 2 && n.start.y != n.end.y)
        return false;
    if (c >= 3 && n.start.z != n.end.z)
        return false;
    if (c >= 4 && n.start.w != n.end.w)
        return false;
    return true;
}

void Path::ScalarRange(f32& lo, f32& hi) const {
    if (nodes.empty()) {
        lo = hi = 0.0f;
        return;
    }
    lo = hi = nodes[0].start.x;
    for (const PathNode& n : nodes) {
        lo = std::fmin(lo, std::fmin(n.start.x, n.end.x));
        hi = std::fmax(hi, std::fmax(n.start.x, n.end.x));
    }
}

bool Path::IsInert(f32 value) const {
    if (nodes.empty())
        return true;
    for (const PathNode& n : nodes) {
        for (i32 c = 0; c < components; ++c) {
            if (n.start.data[c] != value || n.end.data[c] != value)
                return false;
        }
    }
    return true;
}

void Path::ScalarEndpoints(f32& lo, f32& span) const {
    f32 a = 0.0f, b = 0.0f;
    ScalarRange(a, b);
    lo = a;
    span = b - a;
}

namespace {

// The normalised sample time, reproducing `InterpolationPath_Sample`'s three
// time modes exactly — including the `period == 1/60` special case, which
// falls through to t = 0 rather than dividing.
f32 NormalisedTime(const Path& p, const EvalCtx& ctx) {
    const f32 period = ctx.period;
    if (period == 0.0f || period == 0.016667f)
        return 0.0f;
    if (ctx.timeMode == 0)
        return ctx.time / period; // no wrap at all
    const f32 lo = p.loopStart;
    const f32 hi = p.loopEnd;
    if (lo < 0.000001f && hi > 1.0f)
        return ctx.time / period - std::trunc(ctx.time / period);
    const f32 hiT = period * hi;
    f32 time = ctx.time;
    if (time > hiT) {
        const f32 loT = period * lo;
        const f32 spanT = hiT - loT;
        if (spanT != 0.0f) {
            const f32 rel = time - loT;
            return (loT + std::fmax(rel - spanT * std::trunc(rel / spanT), 0.0f)) / period;
        }
        time = loT;
    }
    return time / period;
}

// One linear sample of the node array at normalised time @p t with the
// per-component random @p r already drawn.
//
// The engine's node scan reads `nodes[k-1]` after finding the first `k` whose
// time exceeds `t`, and relies on `nodes[0].time == 0` so that `k` is never
// zero. Shipped data satisfies that; a hand-built or corrupt path need not, so
// k == 0 holds `nodes[0]` here instead of reading behind the array.
Vector4f SampleNodes(const Path& p, f32 t, const Vector4f& r) {
    const usize n = p.nodes.size();
    if (n == 0)
        return {0, 0, 0, 0};

    auto lerpNode = [&](const PathNode& node) {
        Vector4f v;
        for (i32 c = 0; c < 4; ++c)
            v.data[c] = node.start.data[c] + (node.end.data[c] - node.start.data[c]) * r.data[c];
        return v;
    };

    usize k = 0;
    while (k < n && p.nodes[k].time <= t)
        ++k;

    if (k >= n || k == 0) {
        // Past the last node — or before the first, which shipped data never
        // is. The result is the HELD RANDOM value, not a held interpolated
        // one: `lerp(start, end, r)` with no time term at all.
        return lerpNode(p.nodes[(k >= n) ? (n - 1) : 0]);
    }

    const PathNode& a = p.nodes[k - 1];
    const PathNode& b = p.nodes[k];
    const Vector4f va = lerpNode(a);
    const Vector4f vb = lerpNode(b);
    const f32 dt = b.time - a.time;
    const f32 u = (dt != 0.0f) ? ((t - a.time) / dt) : 0.0f;

    Vector4f out;
    for (i32 c = 0; c < 4; ++c)
        out.data[c] = va.data[c] + u * (vb.data[c] - va.data[c]);
    return out;
}

} // namespace

Vector4f SampleAt(const Path& p, const Vector4f& r, const EvalCtx& ctx) {
    if (p.nodes.empty())
        return {0, 0, 0, 0};

    // The engine's early-out: one node and no driver skips the whole time
    // machinery, so a constant channel costs a lerp.
    if (p.nodes.size() == 1 && p.driver.mode == 0) {
        const PathNode& n = p.nodes[0];
        Vector4f v;
        for (i32 c = 0; c < 4; ++c)
            v.data[c] = n.start.data[c] + (n.end.data[c] - n.start.data[c]) * r.data[c];
        return v;
    }

    const f32 t = NormalisedTime(p, ctx);
    Vector4f v = SampleNodes(p, t, r);

    // Time mode 2 pulls every channel toward its end-of-curve value as the
    // emitter runs down its own period. The second sample is taken at
    // `loopEnd + blendT*(1 - loopEnd)` and weighted by `blend`.
    if (ctx.timeMode == 2 && ctx.blend > 0.000001f) {
        const f32 t2 = p.loopEnd + ctx.blendT * (1.0f - p.loopEnd);
        const Vector4f v2 = SampleNodes(p, t2, r);
        for (i32 c = 0; c < 4; ++c)
            v.data[c] += ctx.blend * (v2.data[c] - v.data[c]);
    }

    f32 mul = 1.0f;
    if (EvalDriver(p.driver, ctx.driver, mul)) {
        for (i32 c = 0; c < 4; ++c)
            v.data[c] *= mul;
    }
    return v;
}

Vector4f Path::Eval(u32 particleSeed, i32 channelId, const EvalCtx& ctx) const {
    if (nodes.empty())
        return {0, 0, 0, 0};

    Vector4f r{0, 0, 0, 0};
    if (!IsConstant()) {
        MwcRng rng = MwcRng::ForChannel(channelId, particleSeed);
        if (components >= 3) {
            // A vector channel draws one random PER COMPONENT, in x, y, z
            // order. Drawing one and reusing it would make every vector
            // channel diagonal.
            r.x = ApplyDistribution(distribution, rng.NextUnit());
            r.y = ApplyDistribution(distribution, rng.NextUnit());
            r.z = ApplyDistribution(distribution, rng.NextUnit());
            r.w = r.x;
        } else {
            const f32 u = ApplyDistribution(distribution, rng.NextUnit());
            r = {u, u, u, u};
        }
    }
    return SampleAt(*this, r, ctx);
}

} // namespace whiteout::flakes::renderer::particle::d3
