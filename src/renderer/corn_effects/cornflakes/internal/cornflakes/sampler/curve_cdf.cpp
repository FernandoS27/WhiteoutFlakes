#include <cornflakes/sampler/curve_cdf.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace whiteout::cornflakes {

namespace {

constexpr i32 kMinSegmentPoints = 8;
constexpr i32 kMaxSegmentPoints = 256;

constexpr std::size_t kEstimatedMaxPoints = 384;

constexpr f32 kDecimateThreshold = 0.0024999999F;

f32 linearSpanIntegral(f32 p0, f32 p1, f32 x0, f32 x1) noexcept {
    return ((x1 - x0) * p0) + ((0.5F * (x1 - x0)) * (x1 + x0) * (p1 - p0));
}

f32 hermiteSpanIntegral(f32 p0, f32 p1, f32 m0, f32 m1, f32 x0, f32 x1) noexcept {
    const f32 kA = 0.5F;
    const f32 kB = 0.33333334F;
    const f32 t2 = x0 * x0;
    const f32 u2 = x1 * x1;
    const f32 t3 = (x0 * x0) * x0;
    const f32 u3 = (x1 * x1) * x1;
    const f32 t4 = (x0 * x0) * (x0 * x0);
    const f32 u4 = (x1 * x1) * (x1 * x1);
    const f32 s = x1 - x0;
    const f32 q2 = u2 - t2;
    const f32 q3 = u3 - t3;
    const f32 q4 = u4 - t4;

    const f32 a = p0 - p1;
    const f32 b = m0 + m1;

    const f32 head = (s * p0) + ((kA * q2) * m0);
    const f32 cubic = q3 * ((kB * (b + m0)) + a);
    const f32 quartic = (kA * q4) * ((kA * b) + a);
    return (head - cubic) + quartic;
}

f32 integrateCurveAt(std::span<const f32> times, std::span<const f32> values,
                     std::span<const f32> tangents, bool hermite, f32 x0Raw, f32 x1Raw) noexcept {
    const f32 minT = times.front();
    const f32 maxT = times.back();
    const f32 x0 = std::min(x0Raw, x1Raw);
    const f32 x1 = std::max(x0Raw, x1Raw);

    f32 acc = 0.0F;
    if (x0 < minT) {
        if (x1 < minT) {
            return values.front() * (x1Raw - x0Raw);
        }
        acc = values.front() * (minT - x0);
    }
    for (std::size_t j = 1; j < times.size(); ++j) {
        if (x0 > times[j]) {
            continue;
        }
        const f32 c0 = times[j - 1];
        const f32 c1 = times[j];
        const f32 d = c1 - c0;
        if (d != 0.0F) {
            const f32 rd = 1.0F / d;
            const f32 f0 = (std::max(x0, c0) - c0) * rd;
            const f32 f1 = (std::min(x1, c1) - c0) * rd;
            const f32 span =
                hermite ? hermiteSpanIntegral(values[j - 1], values[j], tangents[(2U * j) - 1U],
                                              tangents[2U * j], f0, f1)
                        : linearSpanIntegral(values[j - 1], values[j], f0, f1);
            acc += span * d;
        }
        if (x1 <= times[j]) {
            break;
        }
    }
    if (x1 > maxT) {
        acc += values.back() * (x1 - maxT);
    }
    return (x0Raw <= x1Raw) ? acc : -acc;
}

void decimateRecursive(std::span<const f32> times, std::span<const f32> values, f32 threshold,
                       std::size_t indexStart, std::size_t indexStop, std::vector<u32>& toRemove) {
    if (indexStart + 2U >= indexStop) {
        return;
    }
    const f32 deltaT = times[indexStop - 1U] - times[indexStart];
    if (std::fabs(deltaT) < 0.0000001F) {
        for (std::size_t i = indexStart + 1U; i < indexStop - 1U; ++i) {
            toRemove.push_back(static_cast<u32>(i));
        }
        return;
    }
    const f32 invDeltaT = 1.0F / deltaT;
    const f32 slope = (values[indexStop - 1U] - values[indexStart]) * invDeltaT;

    std::size_t indexMax = indexStart;
    f32 distanceMax = 0.0F;
    for (std::size_t pos = indexStart + 1U; pos < indexStop - 1U; ++pos) {
        const f32 yvalue = values[indexStart] + (slope * (times[pos] - times[indexStart]));
        const f32 curDistance = std::fabs(values[pos] - yvalue);
        if (curDistance > distanceMax) {
            indexMax = pos;
            distanceMax = curDistance;
        }
    }
    if (distanceMax <= threshold) {
        for (std::size_t i = indexStart + 1U; i < indexStop - 1U; ++i) {
            toRemove.push_back(static_cast<u32>(i));
        }
        return;
    }
    decimateRecursive(times, values, threshold, indexStart, indexMax, toRemove);
    decimateRecursive(times, values, threshold, indexMax, indexStop, toRemove);
}

std::size_t decimateCdf(f32* times, f32* values, std::size_t count, f32 threshold) {
    if (count < 2U) {
        return count;
    }
    f32 lo = values[0];
    f32 hi = values[0];
    for (std::size_t i = 1; i < count; ++i) {
        lo = std::min(lo, values[i]);
        hi = std::max(hi, values[i]);
    }
    const f32 range = hi - lo;
    const f32 curveScale = (range <= 0.000099999997F) ? 10000.0F : (1.0F / range);

    std::vector<u32> toRemove;
    toRemove.reserve(count);
    decimateRecursive(std::span<const f32>{times, count}, std::span<const f32>{values, count},
                      curveScale * threshold, 0U, count, toRemove);
    if (toRemove.empty()) {
        return count;
    }

    std::size_t iRemove = 0;
    std::size_t iPoint = 0;
    for (std::size_t pos = 0; pos < count; ++pos) {
        if (iRemove < toRemove.size() && toRemove[iRemove] == pos) {
            ++iRemove;
            continue;
        }
        times[iPoint] = times[pos];
        values[iPoint] = values[pos];
        ++iPoint;
    }
    return iPoint;
}

}

void buildSamplerCurveCdf(SamplerCurve& curve, IArena& arena) noexcept {
    curve.cdfTimes = {};
    curve.cdfValues = {};

    if (!curve.isProbabilityCurve || curve.components != 1U) {
        return;
    }
    const std::span<const f32> srcTimes = curve.times;
    const std::span<const f32> srcValues = curve.values;
    if (srcTimes.size() < 2U || srcValues.size() != srcTimes.size()) {
        return;
    }
    const std::span<const f32> srcTangents = curve.tangents;
    if (!srcTangents.empty() && srcTangents.size() != 2U * srcTimes.size()) {
        return;
    }
    const bool hermite = !srcTangents.empty();

    const f32 timeStart = srcTimes.front();
    const f32 timeEnd = srcTimes.back();
    const f32 timeRange = timeEnd - timeStart;

    std::vector<u32> segmentPoints;
    segmentPoints.reserve(srcTimes.size());
    std::size_t total = 0;
    for (std::size_t i = 1; i < srcTimes.size(); ++i) {
        const bool isLastKey = (i + 1U) == srcTimes.size();
        const f32 timeDiff = srcTimes[i] - srcTimes[i - 1U];
        const f32 segmentPointCount = ((256.0F * timeDiff) - timeStart) / timeRange;
        const i32 truncated = static_cast<i32>(segmentPointCount);
        const i32 nbPoints = std::clamp(static_cast<i32>(isLastKey ? 1 : 0) + truncated,
                                        kMinSegmentPoints, kMaxSegmentPoints);
        segmentPoints.push_back(static_cast<u32>(nbPoints));
        total += static_cast<std::size_t>(nbPoints);
    }
    if (total < 2U) {
        return;
    }

    const std::size_t capacity = std::max(total, kEstimatedMaxPoints);
    const std::span<f32> cdfTimes = arenaArray<f32>(arena, capacity);
    const std::span<f32> cdfValues = arenaArray<f32>(arena, capacity);
    if (cdfTimes.empty() || cdfValues.empty()) {
        return;
    }

    std::size_t previousPointsSum = 0;
    for (std::size_t i = 1; i < srcTimes.size(); ++i) {
        const bool isLastKey = (i + 1U) == srcTimes.size();
        const f32 timeDiff = srcTimes[i] - srcTimes[i - 1U];
        const auto nbPoints = static_cast<std::size_t>(segmentPoints[i - 1U]);
        const auto divisor = static_cast<i32>(nbPoints) - (isLastKey ? 1 : 0);
        const f32 invPointCount = timeDiff / static_cast<f32>(divisor);
        for (std::size_t j = 0; j < nbPoints; ++j) {
            cdfValues[previousPointsSum + j] =
                (static_cast<f32>(static_cast<i32>(j)) * invPointCount) + srcTimes[i - 1U];
        }
        for (std::size_t j = 0; j < nbPoints; ++j) {
            cdfTimes[previousPointsSum + j] = integrateCurveAt(
                srcTimes, srcValues, srcTangents, hermite, 0.0F, cdfValues[previousPointsSum + j]);
        }
        previousPointsSum += nbPoints;
    }

    f32 realT0 = cdfTimes[0];
    for (std::size_t pos = 1; pos < total; ++pos) {
        const f32 t0 = cdfTimes[pos - 1U];
        const f32 t1 = cdfTimes[pos];
        const f32 newT = t0 + std::max(0.0F, t1 - realT0);
        realT0 = t1;
        cdfTimes[pos] = newT;
    }

    std::size_t count = total;
    if (cdfTimes[total - 1U] <= 0.0F) {
        cdfTimes[0] = 0.0F;
        cdfTimes[1] = 1.0F;
        cdfValues[0] = 0.0F;
        cdfValues[1] = 1.0F;
        count = 2U;
    } else {
        const f32 rcpMaxTime = 1.0F / cdfTimes[total - 1U];
        for (std::size_t i = 0; i < total; ++i) {
            cdfTimes[i] = cdfTimes[i] * rcpMaxTime;
        }
        cdfTimes[total - 1U] = 1.0F;
        count = decimateCdf(cdfTimes.data(), cdfValues.data(), total, kDecimateThreshold);
    }

    curve.cdfTimes = std::span<const f32>{cdfTimes.data(), count};
    curve.cdfValues = std::span<const f32>{cdfValues.data(), count};
}

}
