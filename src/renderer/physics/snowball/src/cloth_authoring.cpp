//===----------------------------------------------------------------------===//
// snowball/cloth_authoring.cpp -- the cloth build pipeline, stage for stage.
//
// The stages run in a fixed order (S0..S18 below) and the order is normative: the record
// order it produces is what the runtime's Gauss-Seidel sweeps observe. Two things here look
// wrong and are engine contract, kept on purpose:
//
//  * The degenerate-triangle WINDOW DEFECT: a degenerate source triangle writes its record
//    slot and advances it, but not the live count -- every later pass iterates the first
//    liveTriCount slots, so one stale slot stays in the window and the last live triangle
//    falls out. Well-formed content never hits it (a degenerate is an authoring error), and
//    "fixing" it re-windows every record a malformed mesh produces.
//
//  * The SORTS are libc++'s std::sort of pre-LLVM-14 vintage, carried whole below. All four
//    comparators are single-key, so almost everything is a tie, and the record order that
//    falls out -- which the Gauss-Seidel sweeps then observe -- is a property of this exact
//    algorithm (median picks, the equal-pivot partition path, the 8-move insertion
//    bailout), not of the keys. Swapping in std::sort silently reorders the records.
//
// Division policy: plain lengths are exact sqrt; the inverse mass and the three atan
// quotients are TRUE divisions; the wing-height reciprocal is a form-C rcp estimate;
// normalisations are rsqrt+NR in math.h's association.
//===----------------------------------------------------------------------===//
#include "snowball/cloth_authoring.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <deque>
#include <limits>
#include <utility>

#include "snowball/math.h"

namespace snowball {
namespace {

const f32 kTiny = std::bit_cast<f32>(u32{0x057A0000});
const f32 kDegenerateArea = std::bit_cast<f32>(u32{0x37D1B717});   // 2.5e-5
const f32 kMassFactor = std::bit_cast<f32>(u32{0x3E2AA64C});       // 0.16665, NOT 1/6
const f32 kAtanPade = std::bit_cast<f32>(u32{0x3E8F5C29});         // 0.28
const f32 kPi = std::bit_cast<f32>(u32{0x40490FDB});
const f32 kHalfPi = std::bit_cast<f32>(u32{0x3FC90FDB});
const f32 kSixthPi = std::bit_cast<f32>(u32{0x3F060A92});          // rest-angle clamp
const f32 kMinRestLength = std::bit_cast<f32>(u32{0x3BA3D70A});    // 0.005

// Form-C reciprocal refinement (the wing-height site): r' = (1 - x*r)*r + r.
#if SNOWBALL_HAS_SSE_RECIPROCAL
f32 RcpC(f32 v) {
    const __m128 x = _mm_set1_ps(v);
    const __m128 y = _mm_rcp_ps(x);
    const __m128 t = _mm_sub_ps(_mm_set1_ps(1.0f), _mm_mul_ps(x, y));
    return _mm_cvtss_f32(_mm_add_ps(_mm_mul_ps(t, y), y));
}
#else
f32 RcpC(f32 v) { return 1.0f / v; }
#endif

//===--------------------------------------------------------------------------------------===//
// libc++ std::sort, pre-LLVM-14 vintage, carried whole because its tie order is part of the
// record contract -- thresholds, median picks and partition structure included.
//===--------------------------------------------------------------------------------------===//

template <class It, class Comp>
unsigned Sort3(It x, It y, It z, Comp c) {
    unsigned r = 0;
    if (!c(*y, *x)) {
        if (!c(*z, *y)) {
            return r;
        }
        std::swap(*y, *z);
        r = 1;
        if (c(*y, *x)) {
            std::swap(*x, *y);
            r = 2;
        }
        return r;
    }
    if (c(*z, *y)) {
        std::swap(*x, *z);
        return 1;
    }
    std::swap(*x, *y);
    r = 1;
    if (c(*z, *y)) {
        std::swap(*y, *z);
        r = 2;
    }
    return r;
}

template <class It, class Comp>
unsigned Sort4(It x1, It x2, It x3, It x4, Comp c) {
    unsigned r = Sort3(x1, x2, x3, c);
    if (c(*x4, *x3)) {
        std::swap(*x3, *x4);
        ++r;
        if (c(*x3, *x2)) {
            std::swap(*x2, *x3);
            ++r;
            if (c(*x2, *x1)) {
                std::swap(*x1, *x2);
                ++r;
            }
        }
    }
    return r;
}

template <class It, class Comp>
unsigned Sort5(It x1, It x2, It x3, It x4, It x5, Comp c) {
    unsigned r = Sort4(x1, x2, x3, x4, c);
    if (c(*x5, *x4)) {
        std::swap(*x4, *x5);
        ++r;
        if (c(*x4, *x3)) {
            std::swap(*x3, *x4);
            ++r;
            if (c(*x3, *x2)) {
                std::swap(*x2, *x3);
                ++r;
                if (c(*x2, *x1)) {
                    std::swap(*x1, *x2);
                    ++r;
                }
            }
        }
    }
    return r;
}

template <class It, class Comp>
void InsertionSort3(It first, It last, Comp c) {
    using T = typename std::iterator_traits<It>::value_type;
    It j = first + 2;
    Sort3(first, first + 1, j, c);
    for (It i = j + 1; i != last; ++i) {
        if (c(*i, *j)) {
            T t(std::move(*i));
            It k = j;
            j = i;
            do {
                *j = std::move(*k);
                j = k;
            } while (j != first && c(t, *--k));
            *j = std::move(t);
        }
        j = i;
    }
}

// The bounded insertion sort: bails out after 8 moved elements.
template <class It, class Comp>
bool InsertionSortIncomplete(It first, It last, Comp c) {
    using T = typename std::iterator_traits<It>::value_type;
    switch (last - first) {
        case 0:
        case 1:
            return true;
        case 2:
            if (c(*--last, *first)) {
                std::swap(*first, *last);
            }
            return true;
        case 3:
            Sort3(first, first + 1, --last, c);
            return true;
        case 4:
            Sort4(first, first + 1, first + 2, --last, c);
            return true;
        case 5:
            Sort5(first, first + 1, first + 2, first + 3, --last, c);
            return true;
    }
    It j = first + 2;
    Sort3(first, first + 1, j, c);
    const unsigned limit = 8;
    unsigned count = 0;
    for (It i = j + 1; i != last; ++i) {
        if (c(*i, *j)) {
            T t(std::move(*i));
            It k = j;
            j = i;
            do {
                *j = std::move(*k);
                j = k;
            } while (j != first && c(t, *--k));
            *j = std::move(t);
            if (++count == limit) {
                return ++i == last;
            }
        }
        j = i;
    }
    return true;
}

template <class It, class Comp>
void LibcxxSort(It first, It last, Comp c) {
    using Diff = typename std::iterator_traits<It>::difference_type;
    const Diff insertionLimit = 30;  // trivially copyable element types
    while (true) {
    restart:
        Diff len = last - first;
        switch (len) {
            case 0:
            case 1:
                return;
            case 2:
                if (c(*--last, *first)) {
                    std::swap(*first, *last);
                }
                return;
            case 3:
                Sort3(first, first + 1, --last, c);
                return;
            case 4:
                Sort4(first, first + 1, first + 2, --last, c);
                return;
            case 5:
                Sort5(first, first + 1, first + 2, first + 3, --last, c);
                return;
        }
        if (len <= insertionLimit) {
            InsertionSort3(first, last, c);
            return;
        }
        It m = first;
        It lm1 = last;
        --lm1;
        unsigned nSwaps;
        {
            Diff delta = len / 2;
            m += delta;
            if (len >= 1000) {
                delta /= 2;
                nSwaps = Sort5(first, first + delta, m, m + delta, lm1, c);
            } else {
                nSwaps = Sort3(first, m, lm1, c);
            }
        }
        It i = first;
        It j = lm1;
        if (!c(*i, *m)) {
            // *first == *m: hunt for a guard, possibly discovering an all-equivalent range.
            while (true) {
                if (i == --j) {
                    ++i;
                    j = last;
                    if (!c(*first, *--j)) {
                        while (true) {
                            if (i == j) {
                                return;  // all equivalent
                            }
                            if (c(*first, *i)) {
                                std::swap(*i, *j);
                                ++nSwaps;
                                ++i;
                                break;
                            }
                            ++i;
                        }
                    }
                    if (i == j) {
                        return;
                    }
                    while (true) {
                        while (!c(*first, *i)) {
                            ++i;
                        }
                        while (c(*first, *--j)) {
                        }
                        if (i >= j) {
                            break;
                        }
                        std::swap(*i, *j);
                        ++nSwaps;
                        ++i;
                    }
                    first = i;
                    goto restart;
                }
                if (c(*j, *m)) {
                    std::swap(*i, *j);
                    ++nSwaps;
                    break;
                }
            }
        }
        ++i;
        if (i < j) {
            while (true) {
                while (c(*i, *m)) {
                    ++i;
                }
                while (!c(*--j, *m)) {
                }
                if (i > j) {
                    break;
                }
                std::swap(*i, *j);
                ++nSwaps;
                if (m == i) {
                    m = j;
                }
                ++i;
            }
        }
        if (i != m && c(*m, *i)) {
            std::swap(*i, *m);
            ++nSwaps;
        }
        if (nSwaps == 0) {
            const bool fs = InsertionSortIncomplete(first, i, c);
            if (InsertionSortIncomplete(i + 1, last, c)) {
                if (fs) {
                    return;
                }
                last = i;
                continue;
            }
            if (fs) {
                first = ++i;
                continue;
            }
        }
        if (i - first < last - i) {
            LibcxxSort(first, i, c);
            first = ++i;
        } else {
            LibcxxSort(i + 1, last, c);
            last = i;
        }
    }
}

//===--------------------------------------------------------------------------------------===//

// The edge scratch record (S7).
struct EdgeRec {
    i16 e0{}, e1{};    // min/max endpoint, SIGNED compares in the sort predicate
    i32 opp{-1};       // this face's third vertex
    i32 opp2{-1};      // the twin face's third vertex, once found
    f32 restAngle{}, h0{}, h1{};
    u8 isDistance{}, isBend{};
};

// The triangle record slots (S4); slot index = SOURCE triangle index (the window defect).
struct TriRec {
    u16 v0{}, v1{}, v2{};
    f32 area{};
    u16 selfFlag{};
};

struct DistRec {
    u16 a{}, b{};
    f32 rest{}, lambda1{}, lambda2{}, mask{};
};

struct BendRec {
    u16 w0{}, w1{}, p0{}, p1{};  // wings then hinge, baked-record order
    f32 restAngle{}, selfMask{}, h0{}, h1{};
};

}  // namespace

bool BuildCloth(const ClothMeshSource& source, ClothBuildResult& result) {
    const i32 n = static_cast<i32>(source.vertices.size());
    const i32 srcTriCount = static_cast<i32>(source.triangles.size());
    if (srcTriCount <= 0 || n < 3) {
        return false;  // the S0 reject path
    }

    // Working per-particle state, old vertex order. `hop` doubles as the dynamic flag until
    // the S13 BFS overwrites it with real hop counts (0xFFFF dynamic, 0 pinned) -- one
    // field, two meanings, and the handoff point is S13.
    struct Work {
        f32 mass{};        // area sum, then inverse mass after S5
        f32 tetherDist{};
        i32 proxy{-1};
        i32 root{-1};
        i32 group{-1};
        i32 frameRef{-1};
        u32 hop{0xFFFF};
        u16 flags{};
        f32 bestEdge{};    // longest-incident-edge tracker for the frame tangent
    };
    std::vector<Work> work(n);
    std::vector<Vec4> pos(n);
    i32 pinnedCount = 0;

    // S1: anchor index scan (+1, signed max, init 0) and the slot LUT.
    i32 anchorIndexCount = 0;
    for (const ClothMeshVertex& v : source.vertices) {
        for (const i16 a : v.anchors) {
            anchorIndexCount = std::max(anchorIndexCount, static_cast<i32>(a) + 1);
        }
    }
    result.anchorSlotLut.assign(anchorIndexCount, 0);

    // S2: per-particle init. A negative anchor index marks slot 0 as referenced -- absent
    // lanes alias slot 0, whose weight-zero contribution vanishes in every skin.
    for (i32 i = 0; i < n; ++i) {
        const ClothMeshVertex& v = source.vertices[i];
        Work& w = work[i];
        pos[i] = v.restPosition;
        w.flags = static_cast<u16>((v.movable ? 1 : 0) | (v.selfCollision ? 2 : 0));
        if (v.movable) {
            w.hop = 0xFFFF;
            w.proxy = i;
            w.root = i;
        } else {
            ++pinnedCount;
            w.hop = 0;
            w.proxy = -1;
            w.root = -1;
            for (const i16 a : v.anchors) {
                const i32 slot = a < 0 ? 0 : a;
                if (slot < anchorIndexCount) {
                    result.anchorSlotLut[slot] = 1;
                }
            }
        }
    }

    // S3: anchor-state compaction -- sequential ids for pinned-referenced anchors only.
    i32 anchorStateCount = 0;
    for (i16& entry : result.anchorSlotLut) {
        entry = (entry == 1) ? static_cast<i16>(anchorStateCount++) : i16{-1};
    }
    result.anchorIndexCount = static_cast<u32>(anchorIndexCount);
    result.anchorStateCount = static_cast<u32>(anchorStateCount);

    // S4: triangle pass. Degenerate slots keep their vertices and area but advance neither
    // the live count nor any accumulator -- the WINDOW DEFECT the file comment names.
    // Two per-vertex normal accumulators run side by side, differing ONLY by a factor of
    // two (area * n-hat is identically n/2), so every normalised consumer sees them as one;
    // they part company on the degenerate paths. The raw sum is the fallback the emitted
    // skinned rest normal keeps, the area-WEIGHTED sum the fallback reference row -- and on
    // the success path S6's one normalise serves both.
    std::vector<TriRec> tris(srcTriCount);
    std::vector<Vec4> normalAccum(n, Vec4{});   // the raw sum of cross(e01, e02)
    std::vector<Vec4> areaAccum(n, Vec4{});     // sum of area * unit normal
    i32 live = 0;
    f32 totalArea = 0.0f;
    for (i32 t = 0; t < srcTriCount; ++t) {
        const u16 v0 = source.triangles[t][0];
        const u16 v1 = source.triangles[t][1];
        const u16 v2 = source.triangles[t][2];
        TriRec& rec = tris[t];
        rec.v0 = v0;
        rec.v1 = v1;
        rec.v2 = v2;
        const Vec4 e01 = pos[v1] - pos[v0];
        const Vec4 e02 = pos[v2] - pos[v0];
        // cross(e01, e02) -- the SAME operand order as the runtime normal sweep, so the
        // authored rest normals and the first Step's rebuilt normals agree in sign
        // rather than fighting across the first frame.
        const Vec4 nrm = Cross3(e01, e02);
        const f32 nSq = Dot3(nrm, nrm);
        const f32 len = std::sqrt(nSq);
        const f32 area = len * 0.5f;
        rec.area = area;
        if (area < kDegenerateArea || !(nSq > kTiny)) {
            continue;  // degenerate: an authoring error, but the slot is already consumed
        }
        rec.selfFlag =
            ((work[v0].flags | work[v1].flags | work[v2].flags) & 2) ? u16{1} : u16{0};
        normalAccum[v0] = normalAccum[v0] + nrm;
        normalAccum[v1] = normalAccum[v1] + nrm;
        normalAccum[v2] = normalAccum[v2] + nrm;
        const Vec4 weighted = (nrm * Rsqrt(nSq)) * area;
        areaAccum[v0] = areaAccum[v0] + weighted;
        areaAccum[v1] = areaAccum[v1] + weighted;
        areaAccum[v2] = areaAccum[v2] + weighted;
        const f32 l01 = std::sqrt(Dot3(e01, e01));
        const f32 l02 = std::sqrt(Dot3(e02, e02));
        const Vec4 e12 = pos[v2] - pos[v1];
        const f32 l12 = std::sqrt(Dot3(e12, e12));
        // Frame-tangent reference: each corner keeps the longest incident edge (strictly >),
        // second candidate tested after the first within one face.
        if (l01 > work[v0].bestEdge) {
            work[v0].bestEdge = l01;
            work[v0].frameRef = v1;
        }
        if (l02 > work[v0].bestEdge) {
            work[v0].bestEdge = l02;
            work[v0].frameRef = v2;
        }
        if (l01 > work[v1].bestEdge) {
            work[v1].bestEdge = l01;
            work[v1].frameRef = v0;
        }
        if (l12 > work[v1].bestEdge) {
            work[v1].bestEdge = l12;
            work[v1].frameRef = v2;
        }
        if (l02 > work[v2].bestEdge) {
            work[v2].bestEdge = l02;
            work[v2].frameRef = v0;
        }
        if (l12 > work[v2].bestEdge) {
            work[v2].bestEdge = l12;
            work[v2].frameRef = v1;
        }
        totalArea += area;
        ++live;
        const f32 massShare = len * kMassFactor;  // |n| * 0.16665, about a third of the area
        work[v0].mass += massShare;
        work[v1].mass += massShare;
        work[v2].mass += massShare;
    }

    // S5: inverse mass -- one of the four TRUE divisions. Pinned stays 0.
    for (i32 i = 0; i < n; ++i) {
        Work& w = work[i];
        w.mass = (w.hop != 0 && w.mass >= kTiny) ? 1.0f / w.mass : 0.0f;
    }

    // S6: the output-frame reference matrices -- the authored bind rows expressed in the
    // per-vertex geometric frame {tangent-to-ref, cross, area-weighted normal}, the origin
    // row as the frame-space rest offset (exactly zero here, since the source's fourth row
    // IS the rest position -- kept as the two mat-vec subtractions all the same). The one
    // accumulator normalise in the pass produces Z AND the emitted skinned rest normal, so
    // that normal is UNIT on the success path; the raw sum survives only where the frame
    // cannot be built, alongside zero rows with the weighted accumulator in the third slot.
    std::vector<std::array<Vec4, 4>> refRows(static_cast<usize>(n));
    std::vector<Vec4> restNormalOut(normalAccum);
    for (i32 i = 0; i < n; ++i) {
        std::array<Vec4, 4>& out = refRows[static_cast<usize>(i)];
        out = {Vec4{}, Vec4{}, areaAccum[i], Vec4{}};
        const f32 aSq = Dot3(areaAccum[i], areaAccum[i]);
        if (work[i].frameRef < 0 || !(aSq > kTiny)) {
            continue;
        }
        const Vec4 z = areaAccum[i] * Rsqrt(aSq);
        restNormalOut[static_cast<usize>(i)] = z;
        const Vec4 d = pos[work[i].frameRef] - pos[i];
        const Vec4 t = d - z * Dot3(z, d);
        const f32 tSq = Dot3(t, t);
        if (!(tSq > kTiny)) {
            continue;
        }
        const Vec4 x = t * Rsqrt(tSq);
        const Vec4 y = Cross3(z, x);
        const ClothMeshVertex& v = source.vertices[i];
        const Vec4 rows[3] = {v.bindX, v.bindY, v.bindZ};
        for (i32 r = 0; r < 3; ++r) {
            out[static_cast<usize>(r)] = {Dot3(rows[r], x), Dot3(rows[r], y),
                                          Dot3(rows[r], z), 0.0f};
        }
        const Vec4 restF{Dot3(v.restPosition, x), Dot3(v.restPosition, y),
                         Dot3(v.restPosition, z), 0.0f};
        const Vec4 posF{Dot3(pos[i], x), Dot3(pos[i], y), Dot3(pos[i], z), 0.0f};
        out[3] = restF - posF;
        const f32 r0Sq = Dot3(out[0], out[0]);
        if (r0Sq > kTiny) {
            out[0] = out[0] * Rsqrt(r0Sq);  // row 0 alone is renormalised, on purpose
        }
    }

    // S7: edge extraction over the first `live` slots.
    std::vector<EdgeRec> edges;
    edges.reserve(3 * static_cast<usize>(live));
    for (i32 t = 0; t < live; ++t) {
        const TriRec& rec = tris[t];
        const auto add = [&edges](u16 a, u16 b, u16 opp) {
            EdgeRec e;
            e.e0 = static_cast<i16>(std::min(a, b));
            e.e1 = static_cast<i16>(std::max(a, b));
            e.opp = opp;
            e.opp2 = -1;
            edges.push_back(e);
        };
        add(rec.v0, rec.v1, rec.v2);
        add(rec.v1, rec.v2, rec.v0);
        add(rec.v2, rec.v0, rec.v1);
    }

    // S8: the endpoint sort. The predicate's signed compares are part of the tie order.
    LibcxxSort(edges.begin(), edges.end(), [](const EdgeRec& a, const EdgeRec& b) {
        return a.e0 < b.e0 || (a.e1 < b.e1 && a.e0 <= b.e0);
    });

    // S9: dedup runs, flag records, and the bend rest data derived inline.
    i32 distCount = 0;
    i32 bendCount = 0;
    for (usize i = 0; i < edges.size();) {
        EdgeRec& e = edges[i];
        usize j = i + 1;
        while (j < edges.size() && edges[j].e0 == e.e0 && edges[j].e1 == e.e1) {
            if (edges[j].opp != e.opp && e.opp2 == -1) {
                e.opp2 = edges[j].opp;
            }
            ++j;
        }
        const bool end0Dynamic = work[e.e0].hop != 0;
        const bool end1Dynamic = work[e.e1].hop != 0;
        if (end0Dynamic || end1Dynamic) {
            e.isDistance = 1;
            ++distCount;
        }
        if (e.opp != -1 && e.opp2 != -1 && (work[e.opp].hop != 0 || work[e.opp2].hop != 0)) {
            e.isBend = 1;
            ++bendCount;
            const Vec4 edge = pos[e.e1] - pos[e.e0];
            const f32 eSq = Dot3(edge, edge);
            if (!(eSq > kTiny)) {
                e.restAngle = e.h0 = e.h1 = 0.0f;
            } else {
                const Vec4 u0 = pos[e.opp] - pos[e.e0];
                const Vec4 u1 = pos[e.opp2] - pos[e.e0];
                const Vec4 n0 = Cross3(edge, u0);
                const Vec4 n1 = Cross3(u1, edge);
                const f32 cosA = Dot3(n1, n0);
                const f32 sinA = Dot3(Cross3(n0, n1), edge * Rsqrt(eSq));
                f32 a;
                if (cosA == 0.0f) {
                    a = sinA > 0.0f ? kHalfPi : (sinA < 0.0f ? -kHalfPi : 0.0f);
                } else {
                    const f32 t = sinA / cosA;  // true divisions, all three atan quotients
                    if (std::abs(t) < 1.0f) {
                        a = t / (kAtanPade * t * t + 1.0f);
                        if (cosA < 0.0f) {
                            a += (sinA >= 0.0f ? kPi : -kPi);
                        }
                    } else {
                        a = (sinA >= 0.0f ? kHalfPi : -kHalfPi) - t / (t * t + kAtanPade);
                    }
                }
                e.restAngle = std::max(-kSixthPi, std::min(a, kSixthPi));
                const f32 inv = RcpC(std::sqrt(eSq));  // exact sqrt, estimate reciprocal
                e.h0 = std::sqrt(Dot3(n0, n0)) * inv;  // wing heights 2*area/|edge|
                e.h1 = std::sqrt(Dot3(n1, n1)) * inv;
            }
        }
        i = j;
    }

    // S10: compaction into the two record arrays, edge-sorted order.
    std::vector<DistRec> dist;
    dist.reserve(distCount);
    std::vector<BendRec> bends;
    bends.reserve(bendCount);
    for (const EdgeRec& e : edges) {
        if (e.isDistance) {
            DistRec d;
            d.a = static_cast<u16>(e.e0);
            d.b = static_cast<u16>(e.e1);
            dist.push_back(d);
        }
        if (e.isBend) {
            BendRec b;
            b.w0 = static_cast<u16>(e.opp);
            b.w1 = static_cast<u16>(e.opp2);
            b.p0 = static_cast<u16>(e.e0);
            b.p1 = static_cast<u16>(e.e1);
            b.restAngle = e.restAngle;
            b.selfMask = 0.0f;
            b.h0 = e.h0;
            b.h1 = e.h1;
            bends.push_back(b);
        }
    }

    // S11: constraint adjacency by PREPENDING two half-edges per distance record -- each
    // particle's neighbour list is therefore the REVERSE of record order, and that order is
    // observable through the BFS parent picks below.
    std::vector<i32> head(n, -1);
    std::vector<std::pair<i32, i32>> links;  // {next, neighbour}
    links.reserve(2 * dist.size());
    for (const DistRec& d : dist) {
        links.push_back({head[d.a], d.b});
        head[d.a] = static_cast<i32>(links.size()) - 1;
        links.push_back({head[d.b], d.a});
        head[d.b] = static_cast<i32>(links.size()) - 1;
    }
    const auto neighbours = [&](i32 p, auto&& fn) {
        for (i32 l = head[p]; l != -1; l = links[l].first) {
            fn(links[l].second);
        }
    };

    // S12: transform-group islands, DFS with an explicit LIFO stack, seeded in old order
    // from dynamic particles. Reachable pinned particles join the island; pure-pinned
    // components keep no group.
    i32 islandCount = 0;
    std::vector<i32> stack;
    for (i32 i = 0; i < n; ++i) {
        if (work[i].group != -1 || work[i].hop == 0) {
            continue;
        }
        stack.push_back(i);
        while (!stack.empty()) {
            const i32 p = stack.back();
            stack.pop_back();
            if (work[p].group != -1) {
                continue;
            }
            work[p].group = islandCount;
            neighbours(p, [&](i32 nb) {
                if (work[nb].group == -1) {
                    stack.push_back(nb);
                }
            });
        }
        ++islandCount;
    }

    // S13: pinned-first reorder + hop BFS. Phase 1 numbers the pinned particles in old
    // order; phase 2 grows rings outward, picking the nearest ring-h parent (squared
    // distance, strictly <) as the collision proxy and accumulating the geodesic tether
    // distance; phase 3 sweeps up dynamic islands with no pinned particle (self-proxy).
    result.oldToNew.assign(n, -1);
    std::vector<i32> newToOld;
    newToOld.reserve(n);
    std::deque<i32> fifo;
    for (i32 i = 0; i < n; ++i) {
        if (work[i].hop == 0) {
            fifo.push_back(i);
            result.oldToNew[i] = static_cast<i16>(newToOld.size());
            newToOld.push_back(i);
            work[i].proxy = -1;
        }
    }
    while (!fifo.empty()) {
        const i32 p = fifo.front();
        fifo.pop_front();
        const u32 h = work[p].hop;
        neighbours(p, [&](i32 nb) {
            if (work[nb].hop != 0xFFFF) {
                return;
            }
            work[nb].hop = h + 1;
            f32 best = std::numeric_limits<f32>::max();
            i32 parent = -1;
            neighbours(nb, [&](i32 m) {
                if (work[m].hop == h) {
                    const Vec4 d = pos[nb] - pos[m];
                    const f32 d2 = Dot3(d, d);
                    if (d2 < best) {
                        parent = m;
                    }
                    best = std::min(best, d2);
                }
            });
            work[nb].proxy = result.oldToNew[parent];
            const Vec4 d = pos[nb] - pos[parent];
            work[nb].tetherDist = std::sqrt(Dot3(d, d)) + work[parent].tetherDist;
            fifo.push_back(nb);
            result.oldToNew[nb] = static_cast<i16>(newToOld.size());
            newToOld.push_back(nb);
        });
    }
    u32 tetherLutCount = 0;
    for (i32 i = 0; i < n; ++i) {
        if (work[i].hop == 0xFFFF) {
            work[i].hop = 0;
            work[i].proxy = static_cast<i32>(newToOld.size());  // self, NEW index
            result.oldToNew[i] = static_cast<i16>(newToOld.size());
            newToOld.push_back(i);
        }
        tetherLutCount = std::max(tetherLutCount, work[i].hop);
    }

    // Reordered views for S14..S18.
    const auto oldOf = [&](i32 newIdx) { return newToOld[newIdx]; };
    const auto hopNew = [&](i32 newIdx) { return work[oldOf(newIdx)].hop; };
    const auto proxyNew = [&](i32 newIdx) { return work[oldOf(newIdx)].proxy; };
    const auto flagsNew = [&](i32 newIdx) { return work[oldOf(newIdx)].flags; };

    // S14: tether root -- follow the proxy chain while it decreases.
    for (i32 i = pinnedCount; i < n; ++i) {
        i32 j = i;
        while (true) {
            const i32 next = proxyNew(j);
            if (next == -1 || next >= j) {
                break;
            }
            j = next;
        }
        work[oldOf(i)].root = j;
    }

    // S15: remaps and orientation fixups.
    for (i32 i = 0; i < n; ++i) {
        if (work[i].frameRef != -1) {
            work[i].frameRef = result.oldToNew[work[i].frameRef];
        }
    }
    for (DistRec& d : dist) {
        d.a = static_cast<u16>(result.oldToNew[d.a]);
        d.b = static_cast<u16>(result.oldToNew[d.b]);
        if (hopNew(d.b) < hopNew(d.a)) {
            std::swap(d.a, d.b);  // endpoint 0 carries the smaller hop count
        }
    }
    for (BendRec& b : bends) {
        b.w0 = static_cast<u16>(result.oldToNew[b.w0]);
        b.w1 = static_cast<u16>(result.oldToNew[b.w1]);
        b.p0 = static_cast<u16>(result.oldToNew[b.p0]);
        b.p1 = static_cast<u16>(result.oldToNew[b.p1]);
        if (hopNew(b.w1) < hopNew(b.w0)) {
            std::swap(b.w0, b.w1);  // wing 0 carries the smaller hop count...
            std::swap(b.p0, b.p1);  // ...and the hinge pair swaps WITH it
        }
        if (((flagsNew(b.w0) | flagsNew(b.w1) | flagsNew(b.p0) | flagsNew(b.p1)) & 2) != 0) {
            b.selfMask = 1.0f;
        }
    }
    for (i32 t = 0; t < live; ++t) {
        tris[t].v0 = static_cast<u16>(result.oldToNew[tris[t].v0]);
        tris[t].v1 = static_cast<u16>(result.oldToNew[tris[t].v1]);
        tris[t].v2 = static_cast<u16>(result.oldToNew[tris[t].v2]);
    }

    // S16: the three final single-key sorts.
    LibcxxSort(dist.begin(), dist.end(),
               [](const DistRec& a, const DistRec& b) { return a.a < b.a; });
    LibcxxSort(bends.begin(), bends.end(),
               [](const BendRec& a, const BendRec& b) { return a.w0 < b.w0; });
    LibcxxSort(tris.begin(), tris.begin() + live, [](const TriRec& a, const TriRec& b) {
        return std::min(a.v0, std::min(a.v1, a.v2)) < std::min(b.v0, std::min(b.v1, b.v2));
    });

    // S17: distance-lane fill. Edges free of any proxy relation split by hop ring (same
    // ring -> lambda1, cross ring -> lambda2); the rest keep both lanes zero and land on
    // distanceStiffness[0]. Rest lengths clamp up to 0.005.
    for (DistRec& d : dist) {
        const i32 p0 = d.a;
        const i32 p1 = d.b;
        if (proxyNew(p0) != p1 && proxyNew(p1) != p1 && proxyNew(p0) != p0 &&
            proxyNew(p1) != p0) {
            if (hopNew(p0) == hopNew(p1)) {
                d.lambda1 = 1.0f;
            } else {
                d.lambda2 = 1.0f;
            }
        }
        if (((flagsNew(p0) | flagsNew(p1)) & 2) != 0) {
            d.mask = 1.0f;
        }
        const Vec4 dp = pos[oldOf(p1)] - pos[oldOf(p0)];
        d.rest = std::max(std::sqrt(Dot3(dp, dp)), kMinRestLength);
    }

    // S18: hand off as a ClothDef record set (world/scale/params are the caller's).
    ClothDef& def = result.def;
    def = {};
    def.pinnedCount = static_cast<u32>(pinnedCount);
    def.transformGroupCount = static_cast<u32>(islandCount);
    def.tetherLutCount = tetherLutCount;
    def.particles.resize(n);
    for (i32 newIdx = 0; newIdx < n; ++newIdx) {
        const i32 i = oldOf(newIdx);
        const ClothMeshVertex& v = source.vertices[i];
        ClothParticleDef& p = def.particles[newIdx];
        p.restPosition = v.restPosition;
        p.restNormal = v.bindZ;
        p.skinnedRestNormal = restNormalOut[static_cast<usize>(i)];
        p.refRow0 = refRows[static_cast<usize>(i)][0];
        p.refRow1 = refRows[static_cast<usize>(i)][1];
        p.refRow2 = refRows[static_cast<usize>(i)][2];
        p.refRow3 = refRows[static_cast<usize>(i)][3];
        for (i32 k = 0; k < 4; ++k) {
            // An absent anchor lane emits slot 0 (the zeroed template), never -1 -- its
            // weight is 0, so slot 0's contribution vanishes in every skin.
            const i16 a = v.anchors[static_cast<usize>(k)];
            p.anchorSlots[k] = static_cast<u16>(a < 0 ? 0 : a);
        }
        p.anchorWeights = {v.weights[0], v.weights[1], v.weights[2], v.weights[3]};
        p.inverseMass = work[i].mass;
        p.tetherRestLength = work[i].tetherDist;
        p.tetherRoot = static_cast<i16>(work[i].root);
        p.lutIndex = static_cast<i16>(work[i].hop);
        p.transformGroup = static_cast<i16>(work[i].group);
        p.frameReference = static_cast<i16>(work[i].frameRef);
        p.collisionProxy = work[i].proxy;
        p.selfCollision = (work[i].flags & 2) != 0;
    }
    def.anchorMap = result.anchorSlotLut;
    def.anchorStateCount = result.anchorStateCount;
    def.edges.reserve(dist.size());
    for (const DistRec& d : dist) {
        def.edges.push_back({d.a, d.b, d.rest, d.lambda1, d.lambda2, d.mask});
    }
    def.bends.reserve(bends.size());
    for (const BendRec& b : bends) {
        def.bends.push_back({b.w0, b.w1, b.p0, b.p1, b.restAngle, b.selfMask, b.h0, b.h1});
    }
    def.triangles.reserve(live);
    for (i32 t = 0; t < live; ++t) {
        def.triangles.push_back({tris[t].v0, tris[t].v1, tris[t].v2, tris[t].area,
                                 tris[t].selfFlag != 0});
    }
    for (const ClothCapsuleDef& c : source.capsules) {
        if (c.featureType != 0) {  // the zero-feature compaction
            def.capsules.push_back(c);
        }
    }
    def.planes = source.planes;
    result.totalArea = totalArea;
    result.liveTriangleCount = static_cast<u32>(live);
    return true;
}

}  // namespace snowball
