#include <cornflakes/interface/binding/sampler_resource.hpp>

#include <algorithm>

namespace whiteout::cornflakes {

namespace {

struct HermiteWeights {
    f32 h00;
    f32 h10;
    f32 h01;
    f32 h11;
};

constexpr HermiteWeights hermiteWeights(f32 u) noexcept {
    const f32 u2 = u * u;
    const f32 u3 = u2 * u;
    return {
        2.0F * u3 - 3.0F * u2 + 1.0F,
        u3 - 2.0F * u2 + u,
        -2.0F * u3 + 3.0F * u2,
        u3 - u2,
    };
}

bool curveHasHermiteTangents(const SamplerCurve& curve) noexcept {
    return !curve.tangents.empty() && curve.tangents.size() >= 2U * curve.values.size();
}

// Result of locating `t` within a curve's key array. `kind` selects which of
// the three regions `t` fell into; `upperKey` / `u` are only meaningful for
// `Inside`.
enum class KeyRegion : u8 { BeforeFirst, AfterLast, Inside };
struct KeyHit {
    KeyRegion kind;
    std::size_t upperKey; ///< Index of the right key (i in `[i-1, i]`); for AfterLast == back.
    f32 u;                ///< Local parameter in [0, 1]; degenerate spans set this to 0.
};

KeyHit locateKey(std::span<const f32> times, f32 t) noexcept {
    if (times.empty()) {
        return {KeyRegion::BeforeFirst, 0, 0.0F};
    }
    if (t <= times.front()) {
        return {KeyRegion::BeforeFirst, 0, 0.0F};
    }
    if (t >= times.back()) {
        return {KeyRegion::AfterLast, times.size() - 1U, 0.0F};
    }
    for (std::size_t i = 1; i < times.size(); ++i) {
        const f32 t1 = times[i];
        if (t < t1) {
            const f32 t0 = times[i - 1];
            const f32 span = t1 - t0;
            const f32 u = span > 0.0F ? (t - t0) / span : 0.0F;
            return {KeyRegion::Inside, i, u};
        }
    }
    // Unreachable given the AfterLast guard above, but keep the fallback.
    return {KeyRegion::AfterLast, times.size() - 1U, 0.0F};
}

} // namespace

f32 evalSamplerCurveScalar(const SamplerCurve& curve, f32 t, f32 defaultValue) noexcept {
    if (curve.components != 1U || curve.times.empty() || curve.values.size() < curve.times.size()) {
        return defaultValue;
    }

    const KeyHit hit = locateKey(curve.times, t);
    switch (hit.kind) {
    case KeyRegion::BeforeFirst:
        return curve.values.front();
    case KeyRegion::AfterLast:
        return curve.values.back();
    case KeyRegion::Inside: {
        const std::size_t i = hit.upperKey;
        const f32 v0 = curve.values[i - 1];
        const f32 v1 = curve.values[i];
        if (curveHasHermiteTangents(curve)) {
            const f32 m0Out = curve.tangents[2U * (i - 1U) + 1U];
            const f32 m1In = curve.tangents[2U * i];
            const auto w = hermiteWeights(hit.u);
            return w.h00 * v0 + w.h10 * m0Out + w.h01 * v1 + w.h11 * m1In;
        }
        return v0 + (v1 - v0) * hit.u;
    }
    }
    return defaultValue;
}

u8 evalSamplerCurveVec(const SamplerCurve& curve, f32 t, f32* out, u8 outLen) noexcept {
    if (out == nullptr || outLen == 0U) {
        return 0;
    }
    const u8 comps = curve.components;
    if (comps == 0U || comps > 4U || curve.times.empty()) {
        return 0;
    }
    const std::size_t need = curve.times.size() * static_cast<std::size_t>(comps);
    if (curve.values.size() < need) {
        return 0;
    }

    auto writeKey = [&](std::size_t key) {
        const std::size_t base = key * static_cast<std::size_t>(comps);
        for (u8 lane = 0; lane < outLen; ++lane) {
            out[lane] = (lane < comps) ? curve.values[base + lane] : 0.0F;
        }
    };

    const KeyHit hit = locateKey(curve.times, t);
    switch (hit.kind) {
    case KeyRegion::BeforeFirst:
        writeKey(0);
        return std::min(outLen, comps);
    case KeyRegion::AfterLast:
        writeKey(curve.times.size() - 1U);
        return std::min(outLen, comps);
    case KeyRegion::Inside: {
        const std::size_t i = hit.upperKey;
        const std::size_t base0 = (i - 1U) * static_cast<std::size_t>(comps);
        const std::size_t base1 = i * static_cast<std::size_t>(comps);
        const bool hermite = curveHasHermiteTangents(curve);
        const std::size_t mOutBase = (2U * (i - 1U) + 1U) * static_cast<std::size_t>(comps);
        const std::size_t mInBase = (2U * i) * static_cast<std::size_t>(comps);
        const HermiteWeights w = hermiteWeights(hit.u);

        for (u8 lane = 0; lane < outLen; ++lane) {
            if (lane >= comps) {
                out[lane] = 0.0F;
                continue;
            }
            const f32 v0 = curve.values[base0 + lane];
            const f32 v1 = curve.values[base1 + lane];
            if (hermite) {
                const f32 m0Out = curve.tangents[mOutBase + lane];
                const f32 m1In = curve.tangents[mInBase + lane];
                out[lane] = w.h00 * v0 + w.h10 * m0Out + w.h01 * v1 + w.h11 * m1In;
            } else {
                out[lane] = v0 + (v1 - v0) * hit.u;
            }
        }
        return std::min(outLen, comps);
    }
    }
    return 0;
}

const SamplerResource* findSamplerByName(std::span<const SamplerResource> samplers,
                                         std::string_view name) noexcept {
    for (const auto& s : samplers) {
        if (s.name == name) {
            return &s;
        }
    }
    return nullptr;
}

} // namespace whiteout::cornflakes
