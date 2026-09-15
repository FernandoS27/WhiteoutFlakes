#include "renderer/particle/d3/d3_path.h"

#include <algorithm>
#include <cmath>

namespace whiteout::flakes::renderer::particle::d3 {

namespace {

f32 Clamp01Engine(f32 v) {
    // The engine's order: min against 1, then `< 0` substitutes zero. Not
    // `std::clamp`, which differs on a NaN that would reach the node lerp.
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
        // Every other mode reads gameplay state a viewer lacks; behaving as
        // mode 0 is the honest answer (6,209 of 798,941 paths, §12.4, §30.6).
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
    // The minimum of the START lane and the maximum of the END lane, each over
    // its own lane only — `InterpolationPath_GetScalarRange` @0x7100376030 never
    // compares a start against an end. A path whose end lane dips below its
    // start lane therefore reports a range that contains neither extreme, and
    // that is the engine's answer.
    lo = nodes[0].start.x;
    hi = nodes[0].end.x;
    for (usize k = 1; k < nodes.size(); ++k) {
        lo = std::fmin(lo, nodes[k].start.x);
        hi = std::fmax(hi, nodes[k].end.x);
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


namespace {

// The normalised sample time, `InterpolationPath_Sample`'s three time modes.
// Both thresholds are the constant pool's: `period == 1/60` (0.016666675, not the
// printed 0.016667, which never compares equal) falls through to t = 0, and the
// full-range test is `loopEnd > 0.999999f` @0x7100E3BFF8, not `> 1.0f` (§20.1).
f32 NormalisedTime(const Path& p, const EvalCtx& ctx) {
    const f32 period = ctx.period;
    if (period == 0.0f || period == kFrameSeconds)
        return 0.0f;
    if (ctx.timeMode == TimeMode::Raw)
        return ctx.time / period; // no wrap at all
    const f32 lo = p.loopStart;
    const f32 hi = p.loopEnd;
    if (lo < kEpsilon && hi > kNearlyOne)
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
    if (ctx.timeMode == TimeMode::LoopedBlend && ctx.blend > kEpsilon) {
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

void Path::ScalarEndpoints(const EvalCtx& ctx, f32& lo, f32& hi) const {
    if (nodes.empty()) {
        lo = hi = 0.0f;
        return;
    }
    // The same early-out Sample has, and for the same reason: one node and no
    // driver needs none of the machinery.
    if (nodes.size() == 1 && driver.mode == 0) {
        lo = nodes[0].start.x;
        hi = nodes[0].end.x;
        return;
    }

    auto lanesAt = [this](f32 t, f32& l, f32& h) {
        const usize n = nodes.size();
        usize k = 0;
        while (k < n && nodes[k].time <= t)
            ++k;
        if (k >= n || k == 0) {
            const PathNode& node = nodes[(k >= n) ? (n - 1) : 0];
            l = node.start.x;
            h = node.end.x;
            return;
        }
        const PathNode& a = nodes[k - 1];
        const PathNode& b = nodes[k];
        const f32 dt = b.time - a.time;
        const f32 u = (dt != 0.0f) ? ((t - a.time) / dt) : 0.0f;
        l = a.start.x + u * (b.start.x - a.start.x);
        h = a.end.x + u * (b.end.x - a.end.x);
    };

    lanesAt(NormalisedTime(*this, ctx), lo, hi);
    if (ctx.timeMode == TimeMode::LoopedBlend && ctx.blend > kEpsilon) {
        f32 lo2 = 0.0f, hi2 = 0.0f;
        lanesAt(loopEnd + ctx.blendT * (1.0f - loopEnd), lo2, hi2);
        lo = lo + ctx.blend * (lo2 - lo);
        hi = hi + (hi2 - hi) * ctx.blend;
    }

    // Applied to BOTH outputs, through two separate calls on the same scalar.
    f32 mul = 1.0f;
    if (EvalDriver(driver, ctx.driver, mul)) {
        lo *= mul;
        hi *= mul;
    }
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

// ---------------------------------------------------------------------------
// The colour channel. `InterpolationPath_EvalColor` @0x71003779A0 and its
// sampler @0x7100377550 / @0x71003777E0 never leave the byte domain: the packed
// dword's four bytes are lerped in 8.8 fixed point, and the SAME single random
// drives all four. See the header for why that is not the generic path.
// ---------------------------------------------------------------------------

namespace {

/// The exact inverse of the adapter's `UnpackColor`, which is `byte / 255`.
u8 ByteOf(f32 v) {
    return static_cast<u8>(std::lround(v * 255.0f));
}

/// `vcvts_n_s32_f32(v, 8)` — a saturating fixed-point convert, truncating
/// toward zero. Every t in this file is already in [0,1], so the saturation
/// arms are unreachable in practice and present for fidelity.
i32 Fx8(f32 v) {
    const f32 s = v * 256.0f;
    if (!(s > -2147483648.0f))
        return -2147483647 - 1;
    if (s >= 2147483648.0f)
        return 2147483647;
    return static_cast<i32>(s);
}

/// `a + ((unsigned __int16)((b - a) * q) >> 8)`, truncated to a byte. The u16
/// cast sits BEFORE the shift, so a negative delta wraps and un-wraps through
/// the final byte truncation — which is why this is written out rather than
/// expressed as a lerp.
u8 LerpByte(u8 a, u8 b, i32 q) {
    const i32 d = static_cast<i32>(b) - static_cast<i32>(a);
    const u16 prod = static_cast<u16>(static_cast<u32>(d * q) & 0xFFFFu);
    return static_cast<u8>(a + (prod >> 8));
}

struct Rgba8 {
    u8 c[4];
};

Rgba8 NodeColor(const PathNode& n, i32 q) {
    Rgba8 out{};
    for (i32 i = 0; i < 4; ++i)
        out.c[i] = LerpByte(ByteOf(n.start.data[i]), ByteOf(n.end.data[i]), q);
    return out;
}

Rgba8 BlendColor(const Rgba8& a, const Rgba8& b, i32 q) {
    Rgba8 out{};
    for (i32 i = 0; i < 4; ++i)
        out.c[i] = LerpByte(a.c[i], b.c[i], q);
    return out;
}

/// `sub_71003777E0` — the node scan, with the same k == 0 guard SampleNodes
/// uses and for the same reason.
Rgba8 SampleColorNodes(const Path& p, f32 t, i32 q) {
    const usize n = p.nodes.size();
    if (n == 0)
        return {};
    usize k = 0;
    while (k < n && p.nodes[k].time <= t)
        ++k;
    if (k >= n || k == 0)
        return NodeColor(p.nodes[(k >= n) ? (n - 1) : 0], q);
    const PathNode& a = p.nodes[k - 1];
    const PathNode& b = p.nodes[k];
    const f32 dt = b.time - a.time;
    const f32 u = (dt != 0.0f) ? ((t - a.time) / dt) : 0.0f;
    return BlendColor(NodeColor(a, q), NodeColor(b, q), Fx8(u));
}

/// @brief The engine's `x + 2^23` magic-number round: half-to-EVEN, not
///        half-away-from-zero (a decompile prints the constant as `8388600.0`).
///        Its second arm above 2^23 rounds away from zero and is unreachable
///        here. See §20.1, §30.6.
i32 RoundHalfEven(f32 v) {
    return static_cast<i32>(std::nearbyint(v));
}

/// `sub_7100378530` — the driver multiply, rounded into a byte.
u8 ScaleByte(u8 v, f32 mul) {
    return static_cast<u8>(RoundHalfEven(static_cast<f32>(v) * mul));
}

/// One int node's own start/end lerp: `start + round((end - start) * r)`, with
/// the delta taken in INTEGERS and only the product in floats.
i32 IntNodeValue(const PathNode& n, f32 r) {
    const i32 a = static_cast<i32>(n.start.x);
    const i32 b = static_cast<i32>(n.end.x);
    const i32 d = b - a;
    return d != 0 ? a + RoundHalfEven(static_cast<f32>(d) * r) : a;
}

/// `sub_7100376F70` — the int node scan. Every stage rounds back to an integer,
/// so a count curve steps rather than ramps.
i32 SampleIntNodes(const Path& p, f32 t, f32 r) {
    const usize n = p.nodes.size();
    if (n == 0)
        return 0;
    usize k = 0;
    while (k < n && p.nodes[k].time <= t)
        ++k;
    if (k >= n || k == 0)
        return IntNodeValue(p.nodes[(k >= n) ? (n - 1) : 0], r);
    const PathNode& a = p.nodes[k - 1];
    const PathNode& b = p.nodes[k];
    const i32 va = IntNodeValue(a, r);
    const i32 vb = IntNodeValue(b, r);
    const f32 dt = b.time - a.time;
    const f32 u = (dt != 0.0f) ? ((t - a.time) / dt) : 0.0f;
    return va + RoundHalfEven(u * static_cast<f32>(vb - va));
}

} // namespace

Vector4f Path::EvalColor(u32 particleSeed, i32 channelId, const EvalCtx& ctx) const {
    if (nodes.empty())
        return {0, 0, 0, 0};

    // ONE draw, not one per component. The engine's constant test is the packed
    // start dword against the packed end dword of the single node.
    f32 r = 0.0f;
    if (!IsConstant()) {
        MwcRng rng = MwcRng::ForChannel(channelId, particleSeed);
        r = ApplyDistribution(distribution, rng.NextUnit());
    }
    const i32 q = Fx8(r);

    Rgba8 c{};
    if (nodes.size() == 1 && driver.mode == 0) {
        c = NodeColor(nodes[0], q);
    } else {
        const f32 t = NormalisedTime(*this, ctx);
        c = SampleColorNodes(*this, t, q);
        if (ctx.timeMode == TimeMode::LoopedBlend && ctx.blend > kEpsilon) {
            const f32 t2 = loopEnd + ctx.blendT * (1.0f - loopEnd);
            c = BlendColor(c, SampleColorNodes(*this, t2, q), Fx8(ctx.blend));
        }
        f32 mul = 1.0f;
        if (EvalDriver(driver, ctx.driver, mul)) {
            for (i32 i = 0; i < 4; ++i)
                c.c[i] = ScaleByte(c.c[i], mul);
        }
    }
    return {c.c[0] / 255.0f, c.c[1] / 255.0f, c.c[2] / 255.0f, c.c[3] / 255.0f};
}

i32 Path::EvalInt(u32 particleSeed, i32 channelId, const EvalCtx& ctx) const {
    if (nodes.empty())
        return 0;

    f32 r = 0.0f;
    if (!IsConstant()) {
        MwcRng rng = MwcRng::ForChannel(channelId, particleSeed);
        r = ApplyDistribution(distribution, rng.NextUnit());
    }

    if (nodes.size() == 1 && driver.mode == 0)
        return IntNodeValue(nodes[0], r);

    const f32 t = NormalisedTime(*this, ctx);
    i32 v = SampleIntNodes(*this, t, r);
    if (ctx.timeMode == TimeMode::LoopedBlend && ctx.blend > kEpsilon) {
        const f32 t2 = loopEnd + ctx.blendT * (1.0f - loopEnd);
        const i32 v2 = SampleIntNodes(*this, t2, r);
        v += RoundHalfEven(ctx.blend * static_cast<f32>(v2 - v));
    }
    f32 mul = 1.0f;
    if (EvalDriver(driver, ctx.driver, mul))
        v = RoundHalfEven(mul * static_cast<f32>(v));
    return v;
}

} // namespace whiteout::flakes::renderer::particle::d3
