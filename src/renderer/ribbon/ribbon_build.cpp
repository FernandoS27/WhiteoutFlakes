// BUILD — every path that turns elements into triangles.
//
// Three centrelines reach the same cross-section expander: the WC3 baked
// top/bot pair (which bypasses it entirely, being its own two-corner strip),
// the SC2 trail, and the SC2 spline. The section math (Ribbon.fx §4.1/§4.7)
// is written once and the two SC2 paths differ only in how the centreline was
// produced.

#include "renderer/ribbon/ribbon_emitter.h"

#include "constants.h"
#include "renderer/sc2/sc2_element.h"
#include "renderer/sc2/sc2_element_math.h"

#include <algorithm>
#include <cmath>

namespace whiteout::flakes::renderer::ribbon {

namespace {

namespace vs = whiteout::flakes::renderer::sc2::vs;

/// One centreline sample ready to expand: world position, the frame inputs, and
/// the interpolated per-vertex values. Both the time/length and spline BUILD
/// paths fill a list of these and hand it to EmitSc2Strip, so the cross-section
/// math (Ribbon.fx §4.1/§4.7) lives once regardless of how the centreline was
/// produced.
struct StripNode {
    Vector3f pos, tangent, up;
    f32 size, twist, v;
    Vector4f color;
};

/// The three-stop keys one node interpolates. A trail samples them off its
/// element; a spline has no elements and samples the emitter's own, with the
/// size wave already folded in -- which is why this is a parameter rather than
/// something the sampler digs out for itself.
struct Sc2Keys {
    Vector3f size3{0, 0, 0};
    Vector4f color3[ColorStop::kCount] = {{1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 1}};
    Vector3f rotation3{0, 0, 0};
};

struct Sc2Attributes {
    f32 twist = 0;
    f32 size = 0;
    Vector4f color{1, 1, 1, 1};
};

/// Twist, size and colour at @p fAge -- the four `Ribbon.fx` interpolator calls
/// both BUILD paths make, with identical argument shapes. @p sizeScale is the
/// halving the path owes: a trail was halved once by the head writer and halves
/// again here, a spline has no head writer and halves twice (RE 4, A7).
Sc2Attributes Sc2SampleAttributes(f32 fAge, const Sc2Keys& k, const RibbonDesc::Sc2& s,
                                  f32 sizeScale) {
    Sc2Attributes a;
    a.twist = vs::TwistAngle(fAge, k.rotation3.x, k.rotation3.y, k.rotation3.z,
                             s.RotationMidTime());
    const f32 fSize = vs::InterpolateValue(fAge, k.size3.x, k.size3.y, k.size3.z,
                                           s.midTime[MidChannel::Size],
                                           InvMidTime(s.midTime[MidChannel::Size]),
                                           s.midHold[MidChannel::Size], s.sizeSmoothing);
    a.size = fSize * sizeScale;
    const Vector3f rgb = vs::InterpolateValue3(
        fAge, {k.color3[0].x, k.color3[0].y, k.color3[0].z},
        {k.color3[1].x, k.color3[1].y, k.color3[1].z},
        {k.color3[2].x, k.color3[2].y, k.color3[2].z},
        s.midTime[MidChannel::Color], InvMidTime(s.midTime[MidChannel::Color]),
        s.midHold[MidChannel::Color], s.colorSmoothing);
    const f32 alpha = vs::InterpolateValue(fAge, k.color3[0].w, k.color3[1].w,
                                           k.color3[2].w, s.midTime[MidChannel::Alpha],
                                           InvMidTime(s.midTime[MidChannel::Alpha]),
                                           s.midHold[MidChannel::Alpha], s.colorSmoothing);
    a.color = {rgb.x, rgb.y, rgb.z, alpha};
    return a;
}

/// The averaged direction of the two adjacent samples (Ribbon.fx:446),
/// one-sided at the ends. Both centrelines read their frame off the geometry
/// this way -- the spline always, a trail on the accurate/legacy techniques.
template <class PosAt>
Vector3f Sc2GeometryTangent(usize i, usize count, PosAt&& posAt) {
    if (i == 0)
        return vs::Sub(posAt(1), posAt(0));
    if (i + 1 == count)
        return vs::Sub(posAt(i), posAt(i - 1));
    return vs::Sub(posAt(i + 1), posAt(i - 1));
}

/// Billboard / planar: two edge vertices per node, bridged into quads. U runs
/// across the width (0/1); V is the node's V along the length.
i32 EmitFlatStrip(const std::vector<StripNode>& nodes, const Sc2Section& sec,
                  const Vector3f& camDir, std::vector<Vertex>& out) {
    const i32 before = (i32)out.size();
    struct Edge {
        Vector3f top, bot, normal;
        Vector4f color;
        f32 v;
    };
    std::vector<Edge> ev;
    ev.reserve(nodes.size());
    for (const StripNode& n : nodes) {
        const vs::Frame f = vs::BuildFrame(static_cast<int>(sec.xsec), n.tangent, n.up,
                                           camDir, 1.0f, 1.0f, n.twist, sec.smoothPath);
        const Vector3f half = vs::Scale(f.offset, n.size);
        ev.push_back({vs::Add(n.pos, half), vs::Sub(n.pos, half), f.normal, n.color, n.v});
    }
    for (usize i = 0; i + 1 < ev.size(); ++i) {
        const Edge& a = ev[i];
        const Edge& b = ev[i + 1];
        out.push_back({a.top, a.normal, a.color, {0.0f, a.v}});
        out.push_back({a.bot, a.normal, a.color, {1.0f, a.v}});
        out.push_back({b.top, b.normal, b.color, {0.0f, b.v}});
        out.push_back({a.bot, a.normal, a.color, {1.0f, a.v}});
        out.push_back({b.bot, b.normal, b.color, {1.0f, b.v}});
        out.push_back({b.top, b.normal, b.color, {0.0f, b.v}});
    }
    return (i32)out.size() - before;
}

/// Cylinder / star: a ring of `sec.ringEdges` vertices per node - the section
/// perpendicular to the trail - with consecutive rings bridged into a closed
/// tube (Ribbon.fx:521). The ring plane is BuildFrame's post-twist
/// normal/binormal; the outward offset is the vertex normal, so the tube shades
/// as a tube instead of a flat strip that vanishes edge-on.
i32 EmitTubeStrip(const std::vector<StripNode>& nodes, const Sc2Section& sec,
                  f32 innerRadius, std::vector<Vertex>& out) {
    const i32 before = (i32)out.size();
    const i32 ringEdges = sec.ringEdges;
    // Cylinder / star: a ring of `ringEdges` vertices per node — the section
    // perpendicular to the trail — with consecutive rings bridged into a closed
    // tube (Ribbon.fx:521). The ring plane is BuildFrame's post-twist
    // normal/binormal; the outward offset is the vertex normal, so the tube
    // shades as a tube instead of a flat strip that vanishes edge-on.
    auto ringAxes = [&](const StripNode& n, Vector3f& N, Vector3f& B) {
        const Vector3f t = n.tangent;
        Vector3f binormal, normal;
        if (sec.smoothPath) {
            binormal = n.up;
            normal = vs::SafeNormalize(vs::Cross3(t, binormal), Vector3f{0, 0, 1});
        } else {
            binormal = vs::SafeNormalize(vs::Cross3(t, n.up), Vector3f{0, 1, 0});
            normal = vs::SafeNormalize(vs::Cross3(t, binormal), Vector3f{0, 0, 1});
        }
        const vs::Mat3 rot = vs::MakeRotation(n.twist, t);
        B = vs::MulVecMat3(binormal, rot);
        N = vs::MulVecMat3(normal, rot);
    };
    // One ring vertex: outward direction (normalized for a cylinder; the star
    // pinches odd spokes to innerRadius, Ribbon.fx:527) and its world position.
    auto ringVert = [&](const StripNode& n, const Vector3f& N, const Vector3f& B,
                        i32 k, Vector3f& pos, Vector3f& nrm) {
        const f32 th = kTwoPi * (f32)k / (f32)ringEdges;
        const f32 cs = std::cos(th), sn = std::sin(th);
        Vector3f o;
        if (sec.xsec == m3::RibbonType::Cylinder)
            o = vs::SafeNormalize(vs::Add(vs::Scale(N, cs), vs::Scale(B, sn)),
                                  Vector3f{0, 0, 1});
        else {
            // Star (BuildCrossSection type 3): EVEN spokes pinch to innerRadius,
            // odd spokes stay at the outer radius — index 0 is inner. The ring
            // holds 2·edges points (set at the call site), one inner + one outer
            // per authored edge.
            const f32 mag = ((k & 1) == 0) ? innerRadius : 1.0f;
            o = vs::Add(vs::Scale(N, cs * mag), vs::Scale(B, sn * mag));
        }
        nrm = vs::SafeNormalize(o, Vector3f{0, 0, 1});
        pos = vs::Add(n.pos, vs::Scale(o, n.size));
    };
    for (usize i = 0; i + 1 < nodes.size(); ++i) {
        Vector3f Na, Ba, Nb, Bb;
        ringAxes(nodes[i], Na, Ba);
        ringAxes(nodes[i + 1], Nb, Bb);
        const Vector4f ca = nodes[i].color, cb = nodes[i + 1].color;
        const f32 va = nodes[i].v, vb = nodes[i + 1].v;
        for (i32 k = 0; k < ringEdges; ++k) {
            const i32 k1 = (k + 1) % ringEdges;
            const f32 ua = (f32)k / (f32)ringEdges;
            const f32 ub = (f32)(k + 1) / (f32)ringEdges;
            Vector3f a0, a1, b0, b1, na0, na1, nb0, nb1;
            ringVert(nodes[i], Na, Ba, k, a0, na0);
            ringVert(nodes[i], Na, Ba, k1, a1, na1);
            ringVert(nodes[i + 1], Nb, Bb, k, b0, nb0);
            ringVert(nodes[i + 1], Nb, Bb, k1, b1, nb1);
            out.push_back({a0, na0, ca, {ua, va}});
            out.push_back({a1, na1, ca, {ub, va}});
            out.push_back({b0, nb0, cb, {ua, vb}});
            out.push_back({a1, na1, ca, {ub, va}});
            out.push_back({b1, nb1, cb, {ub, vb}});
            out.push_back({b0, nb0, cb, {ua, vb}});
        }
    }
    return (i32)out.size() - before;
}

/// Expand a node centreline into the ribbon's cross-section. Both BUILD paths
/// fill a list of StripNode and hand it here, so the section math
/// (Ribbon.fx 4.1/4.7) lives once regardless of how the centreline was made.
i32 EmitSc2Strip(const std::vector<StripNode>& nodes, const Sc2Section& sec,
                 const Vector3f& camDir, f32 innerRadius, std::vector<Vertex>& out) {
    return sec.tube ? EmitTubeStrip(nodes, sec, innerRadius, out)
                    : EmitFlatStrip(nodes, sec, camDir, out);
}

} // namespace

namespace sc2 {

Vector3f NoiseDisplacement(f32 t, f32 headU, f32 amplitude, f32 frequency,
                           f32 coherence, f32 edge, bool spline) {
    // `FillVertices_Animated`'s order: the mute first — the near edge, or else
    // the far one on a spline — then the two coordinates, then three samples
    // each times the muted amplitude.
    f32 amp = amplitude;
    if (t < edge)
        amp = amp * (t / edge);
    else if (edge != 0.0f && t > (1.0f - edge) && spline)
        amp = amp * ((1.0f - t) / edge);
    const f32 u = t * frequency;
    const f32 w = coherence * headU;
    const auto& table = ::whiteout::flakes::renderer::sc2::GlobalNoiseTable();
    return {table.Sample3D(u, w, 0.0f) * amp, table.Sample3D(u, w, kNoiseZ1) * amp,
            table.Sample3D(u, w, kNoiseZ2) * amp};
}

} // namespace sc2

// ---------------------------------------------------------------------------
// Draw records: one strip, one record per pass.
// ---------------------------------------------------------------------------

i32 RibbonEmitter::BuildStage(const RibbonBuildContext& ctx, std::vector<Vertex>& out,
                              std::vector<RibbonDrawList>& outDraws) const {
    // The one place BUILD reads the family. Each arm returns; nothing falls
    // through into a body written below the switch.
    switch (desc_.family) {
    case RibbonDesc::Family::Wc3:
        return BuildStageWc3(out, outDraws);
    case RibbonDesc::Family::Sc2:
        return BuildStageSc2(ctx, out, outDraws);
    }
    return 0;
}

i32 RibbonEmitter::BuildStageSc2(const RibbonBuildContext& ctx, std::vector<Vertex>& out,
                                 std::vector<RibbonDrawList>& outDraws) const {
    const i32 offset = (i32)out.size();
    const i32 added =
        desc_.sc2.hasSpline ? BuildStripSplineSc2(ctx, out) : BuildStripSc2(ctx, out);
    if (added <= 0)
        return 0;

    // One draw record, routed through the resolved M3 surface (blend +
    // lighting). m3Surface == -1 falls back to the BLS path.
    RibbonDrawList dl;
    dl.vertexOffset = offset;
    dl.vertexCount = added;
    dl.priorityPlane = desc_.priorityPlane;
    dl.m3Surface = desc_.sc2.m3Surface;
    dl.twoSided = true;
    dl.worldOrigin = out[(usize)offset].position;
    outDraws.push_back(dl);
    return added;
}

i32 RibbonEmitter::BuildStageWc3(std::vector<Vertex>& out,
                                 std::vector<RibbonDrawList>& outDraws) const {
    const i32 offset = (i32)out.size();
    const i32 added = BuildStrip(out);
    if (added <= 0)
        return 0;

    // One strip, drawn once per layer. `CRibbonEmitter::Render` @0x100e7e1d0
    // loops the emitter's CRibbonMat array rebinding texture and blend state
    // each pass over the SAME vertex/index buffer, so the passes share a
    // vertex range and differ only in material. Order is the array's, which
    // the transparent queue preserves via the unit index.
    const Vector3f origin = out[(usize)offset].position;
    for (const RibbonLayer& layer : desc_.layers) {
        RibbonDrawList dl;
        dl.vertexOffset = offset;
        dl.vertexCount = added;
        dl.priorityPlane = desc_.priorityPlane;
        dl.textureId = layer.textureId;
        dl.filterMode = layer.filterMode;
        dl.unshaded = layer.unshaded;
        dl.unfogged = layer.unfogged;
        dl.twoSided = layer.twoSided;
        dl.worldOrigin = origin;
        outDraws.push_back(dl);
    }
    return added;
}

// ---------------------------------------------------------------------------
// WC3 family: the baked top/bot pair is already its own two-corner strip, so
// it bypasses the section expander entirely.
// ---------------------------------------------------------------------------

i32 RibbonEmitter::BuildStrip(std::vector<Vertex>& out) const {
    if (state_.visibility <= 0.0f || edges_.size() < 2)
        return 0;

    // Sprite-sheet cell, in the client's own axis assignment: Initialize sets
    // tmpDU = texBox.width / ROWS and tmpDV = texBox.height / COLS (texBox is
    // {0,0,1,1} for every `.m2`), and SetTexSlot @0x100e7d460 then indexes U by
    // slot/cols and V by slot%cols. That is the transpose of the obvious
    // reading, and it is unobservable in shipped data — every one of the
    // corpus's 5293 ribbons is 1x1 — so it is transcribed, not inferred.
    // SetTexSlot asserts `slot < m_rows * m_cols`; the slot arrives from an
    // animation track, so clamp instead of trusting it into the divide.
    const i32 rows = (std::max)(desc_.rows, 1);
    const i32 cols = (std::max)(desc_.cols, 1);
    const i32 slot = std::clamp(state_.slot, 0, rows * cols - 1);

    const f32 cellU = 1.0f / static_cast<f32>(rows);
    const f32 cellV = 1.0f / static_cast<f32>(cols);
    const i32 slotRow = slot / cols;
    const i32 slotCol = slot % cols;
    const f32 texL = cellU * slotRow;
    const f32 texT = cellV * slotCol;
    const f32 texB = texT + cellV;
    const f32 texDU = cellU;

    const f32 ooLife = 1.0f / SimLifespan();

    const Vector4f vertColor = {state_.color.x, state_.color.y, state_.color.z, state_.alpha};
    const Vector3f normal = {1, 0, 0};

    // The emitter's texture transform, applied last and per vertex. The client
    // hands it to the stage as a matrix and transforms per fragment, but it is
    // affine in uv, so transforming the corners and interpolating is the same
    // result — and it keeps the ribbon draw a plain textured strip.
    const f32* const r0 = state_.texAnimRow0;
    const f32* const r1 = state_.texAnimRow1;
    const auto uv = [r0, r1](f32 u, f32 v) -> Vector2f {
        return {r0[0] * u + r0[1] * v + r0[3], r1[0] * u + r1[1] * v + r1[3]};
    };

    const i32 before = (i32)out.size();
    const i32 numEdges = (i32)edges_.size();
    for (i32 i = 0; i < numEdges - 1; i++) {
        const auto& e0 = edges_[i];
        const auto& e1 = edges_[i + 1];

        const f32 u0 = texDU * e0.age * ooLife + texL;
        const f32 u1 = texDU * e1.age * ooLife + texL;

        out.push_back({e0.top, normal, vertColor, uv(u0, texT)});
        out.push_back({e0.bot, normal, vertColor, uv(u0, texB)});
        out.push_back({e1.top, normal, vertColor, uv(u1, texT)});

        out.push_back({e0.bot, normal, vertColor, uv(u0, texB)});
        out.push_back({e1.bot, normal, vertColor, uv(u1, texB)});
        out.push_back({e1.top, normal, vertColor, uv(u1, texT)});
    }
    return (i32)out.size() - before;
}

// ---------------------------------------------------------------------------
// SC2 trail: four steps, in the order the data settles.
// ---------------------------------------------------------------------------

/// Step 1: every live element's displaced world position and frame, plus the
/// synthesised live head. @p elements gains that head, so a node's `src`
/// indexes one list and the head needs no special case downstream.
void RibbonEmitter::CollectNodesSc2(std::vector<RibbonElement>& elements,
                                    std::vector<Sc2Node>& nodes) const {
    const auto& s = desc_.sc2;
    const SimTechnique tech = s.simTechnique;
    const Matrix44f& xform = state_.transform;
    const f32 mass = (s.mass > 0.0f) ? s.mass : 1.0f;
    const f32 drag = (s.drag > 0.0f) ? s.drag : kDragFloor;
    const f32 gravityZ = s.gravity3.z;
    const bool worldSpace = s.IsWorldSpace();

    nodes.reserve(elements.size() + 1);


    const bool legacy = IntegratesOnCpu(tech);
    // Legacy: position/velocity were integrated per-tick and stored on the
    // element (Simulate_Type4), so BUILD reads them directly — the VS sets
    // b_proceduralPosition = false there. The analytic techniques
    // reconstruct pos = birthPos + closed-form drag/gravity displacement(age)
    // here (the VS's b_proceduralPosition path). Age is the RAW seconds
    // (ribbon_vs.slang:402); gravity takes the renderer-unit scale in world space
    // (the velocity was already scaled into world). `e` must outlive `nodes`.
    auto appendNode = [&](usize index) {
        const RibbonElement& e = elements[index];
        if (e.deathU - e.birthU <= 0.0f)
            return;
        Vector3f localPos, localVel;
        if (legacy) {
            localPos = e.pos;
            localVel = e.velocity;
        } else {
            const f32 age = sc2_->headU - e.birthU;
            const f32 g = worldSpace ? (-gravityZ * state_.unitScale) : -gravityZ;
            const vs::DragResult d = vs::CalculateDisplacementAndVelocity(
                age, e.velocity, mass, 1.0f / mass, drag, 1.0f / drag, g);
            localPos = vs::Add(e.birthPos, d.displacement);
            localVel = d.velocity;
        }
        Sc2Node n;
        if (worldSpace) {
            n.pos = localPos;
            n.tangent = vs::Scale(localVel, -1.0f); // provisional (velTangent)
            n.up = e.up;
        } else {
            n.pos = whiteout::transform_point(localPos, xform);
            n.tangent = whiteout::transform_normal(vs::Scale(localVel, -1.0f), xform);
            n.up = whiteout::transform_normal(e.up, xform);
        }
        n.up = vs::SafeNormalize(n.up, Vector3f{0, 0, 1});
        n.src = index;
        nodes.push_back(n);
    };
    for (usize i = 0; i < edges_.size(); ++i)
        appendNode(i);
    // Live head. The binary keeps a persistent head element (pBuffer[3]) that
    // CRibbon_UpdateHeadSegment re-stamps EVERY frame at the emitter world pos
    // with birthU == headU, and Simulate skips integrating it. Our edges_ hold
    // only the frozen history, appended at the sub-frame emission cadence
    // (divisions/maxLength, ~one segment per 8 frames for Tyrael's wings), so
    // without the head the newest strip vertex is a discrete spawn: arcFront —
    // hence rpVScale, hence every node's V→size/twist — jumps on each spawn and
    // retire, the Tyrael-wing / Zealot-hair twinkle at rest. Synthesising the
    // head here anchors the leading edge to the emitter (birthU == headU keeps
    // the newest age at 0), so the arc grows continuously between spawns.
    elements.push_back(MakeSegmentSc2(sc2_->headU, 1.0f));
    appendNode(elements.size() - 1);

}

/// Step 2: the frame's tangent. The procedural techniques keep the analytic
/// velocity step 1 wrote; the accurate-tangent and legacy ones read the trail
/// geometry instead, which retail precomputes on the CPU.
void RibbonEmitter::ResolveTangentsSc2(std::vector<Sc2Node>& nodes,
                                       bool velTangent) const {
    // Tangent source (Ribbon.fx:409/446): -instVel degenerates to {1,0,0} for a
    // slow world trail, so a tube built from it collapses to a line - the
    // geometry tangent is what makes a world-space ribbon read as a spreading
    // trail.
    // Geometry tangents for the accurate/legacy techniques: the averaged
    // direction of the two adjacent segments (Ribbon.fx:446), one-sided at the
    // ends. The procedural techniques keep the analytic velocity above.
    if (!velTangent) {
        const auto posAt = [&](usize i) { return nodes[i].pos; };
        std::vector<Vector3f> tan(nodes.size());
        for (usize i = 0; i < nodes.size(); ++i)
            tan[i] = Sc2GeometryTangent(i, nodes.size(), posAt);
        for (usize i = 0; i < nodes.size(); ++i)
            nodes[i].tangent = tan[i];
    }
    for (Sc2Node& n : nodes)
        n.tangent = vs::SafeNormalize(n.tangent, Vector3f{1, 0, 0});

}

/// Step 3: fAge per node, and in length mode the arc walk and tail cut that
/// define it. The only step the cull method reaches.
void RibbonEmitter::ResolveAgeSc2(const std::vector<RibbonElement>& elements,
                                  std::vector<Sc2Node>& nodes) const {
    const auto& s = desc_.sc2;
    const bool legacy = IntegratesOnCpu(s.simTechnique);
    // fAge per node. Time mode (cullMethod 0): the age fraction over the
    // segment's own lifespan (Ribbon.fx:387). Length mode (cullMethod 1): the
    // strip is culled to `maxLength` of centreline for GEOMETRY, and V is
    // ARC-based, not an age proxy (RIBBON_REVIEW_FINDINGS A4/A9/DRIFT-3, from
    // Simulate_Type4 0x1029604A0 phase 2 and Simulate_Type2 0x10295F520):
    //   tech 4 (legacy):    V = arcFromHead / (unitScale·maxLength), per segment;
    //   tech 2/3 (analytic): V = age · rpVScale, rpVScale = 1/(headU − cutU),
    //     cut = the birthU interpolated at the arc = maxLength point.
    // The arc is measured in render space, so unitScale cancels against maxLen —
    // the denominator is the AUTHORED length (stable), so V does not pulse with
    // the flapping geometry (the earlier speed/maxLength age-proxy dropped the
    // worldScale factor and mis-stretched under variable emitter speed).
    // flags & 0x1000 (UseLengthAndTime) maxes V with the time fraction. Nodes run
    // oldest (front) → head (back).
    const bool lengthMode = (s.cullMethod == CullMethod::Length);
    const f32 maxLen = state_.sc2.maxLength * state_.unitScale;
    if (lengthMode && maxLen > 0.0f) {
        const usize head = nodes.size() - 1;
        std::vector<f32> arcFromHead(nodes.size(), 0.0f); // render-space arc head→node
        f32 acc = 0.0f;
        usize keepFrom = 0;
        bool cutFound = false;
        f32 cutU = elements[nodes.front().src].birthU; // oldest kept birthU (no-cut default)
        for (usize i = head; i > 0; --i) {
            const f32 seg = vs::Length3(vs::Sub(nodes[i - 1].pos, nodes[i].pos));
            if (acc + seg >= maxLen && seg > kArcEpsilon) {
                const f32 t = (maxLen - acc) / seg; // land the tail on maxLength
                nodes[i - 1].pos = vs::Lerp(nodes[i].pos, nodes[i - 1].pos, t);
                arcFromHead[i - 1] = maxLen;
                cutU = elements[nodes[i].src].birthU +
                       (elements[nodes[i - 1].src].birthU - elements[nodes[i].src].birthU) * t;
                keepFrom = i - 1;
                cutFound = true;
                break;
            }
            acc += seg;
            arcFromHead[i - 1] = acc;
        }
        if (keepFrom > 0) {
            const auto cut = static_cast<std::ptrdiff_t>(keepFrom);
            nodes.erase(nodes.begin(), nodes.begin() + cut);
            arcFromHead.erase(arcFromHead.begin(), arcFromHead.begin() + cut);
        }
        // rpVScale (Simulate_Type2/3 0x10295F520/0x10295FBE0 phase-2 arc walk):
        // V spreads the arc fraction over the age span from the head. A cut
        // anchors it at headU→cutU (V reaches 1 at the cut); an UNCUT trail
        // (arc < maxLength, incl. at rest) reaches only arc/maxLength at the
        // oldest. The binary writes uncut = v45/(v72−v41) with v72 = the newest
        // element's birthU and v41 = the oldest's. The newest element is the
        // persistent head (pBuffer[3]) that UpdateHeadSegment re-stamps every
        // frame at birthU==headU, so v72==headU; the synthesised live head above
        // is that element, so headU−cutU (cutU = the oldest birthU here) is
        // exactly v72−v41. cutFound mirrors the binary's cut flag — any element
        // whose cumulative arc reaches maxLength, including one landing on the
        // oldest node (keepFrom 0 but cutFound).
        const f32 headU = sc2_->headU;
        const f32 span = headU - cutU;
        const f32 rpVScale =
            (span <= kArcEpsilon) ? 0.0f
            : cutFound      ? 1.0f / span
                            : (arcFromHead.front() / maxLen) / span;
        const bool useLengthAndTime = s.UsesLengthAndTime();
        for (usize i = 0; i < nodes.size(); ++i) {
            f32 v = legacy ? (arcFromHead[i] / maxLen)                    // tech 4
                           : (headU - elements[nodes[i].src].birthU) * rpVScale;   // tech 2/3
            if (useLengthAndTime)
                v = (std::max)(v, vs::FAge(headU, elements[nodes[i].src].birthU,
                                           elements[nodes[i].src].deathU, 1.0f));
            nodes[i].fAge = (std::min)(v, 1.0f);
        }
    } else {
        for (Sc2Node& n : nodes)
            n.fAge = vs::FAge(sc2_->headU, elements[n.src].birthU, elements[n.src].deathU, 1.0f);
    }

}

/// Step 4: twist, size, colour and the noise wobble, now that fAge is settled.
void RibbonEmitter::SampleAttributesSc2(const std::vector<RibbonElement>& elements,
                                        std::vector<Sc2Node>& nodes) const {
    const auto& s = desc_.sc2;
    const bool legacy = IntegratesOnCpu(s.simTechnique);
    // Interpolated scalars, now that fAge is settled. The sampled size is model
    // units; the half-width is added in WORLD space, so it takes the
    // model->renderer factor by hand (as EmitWc3 rescales above/below).
    for (Sc2Node& n : nodes) {
        const RibbonElement& e = elements[n.src];
        Sc2Keys keys;
        keys.size3 = e.size3;
        keys.rotation3 = e.rotation3;
        for (i32 c = 0; c < ColorStop::kCount; ++c)
            keys.color3[c] = e.color3[c];
        const Sc2Attributes attr =
            Sc2SampleAttributes(n.fAge, keys, s, kSizeHalfScale * state_.unitScale);
        n.twist = attr.twist;
        n.size = attr.size;
        n.color = attr.color;
        n.v = n.fAge; // V = fAge (RE 4.6)

        // Noise displacement (RE 4.3): a per-node positional wobble, present
        // only for legacy ribbons that authored noise (which is what demoted
        // them to legacy). n.pos is in render space, so the amplitude takes the
        // model->renderer factor; muted toward the head only.
        if (legacy && s.HasNoise()) {
            n.pos = vs::Add(n.pos, sc2::NoiseDisplacement(
                                       n.fAge, sc2_->headU,
                                       s.noiseAmplitude * state_.unitScale,
                                       s.noiseFrequency, s.noiseCoherence,
                                       s.noiseEdge, /*spline=*/false));
        }
    }

}

i32 RibbonEmitter::BuildStripSc2(const RibbonBuildContext& ctx,
                                 std::vector<Vertex>& out) const {
    if (state_.visibility <= 0.0f || edges_.size() < 2)
        return 0;

    const auto& s = desc_.sc2;
    const SimTechnique tech = s.simTechnique;

    // Four steps, in the order the data settles: where the nodes ARE, which way
    // they point, how far along the trail each one is, and what colour and size
    // that makes it. Only the third reaches the cull method and only the fourth
    // the material, so each one reads on its own.
    std::vector<RibbonElement> elements(edges_.begin(), edges_.end());
    std::vector<Sc2Node> nodes;
    CollectNodesSc2(elements, nodes);
    if (nodes.size() < 2)
        return 0;
    ResolveTangentsSc2(nodes, TangentFromVelocity(tech));
    ResolveAgeSc2(elements, nodes);
    SampleAttributesSc2(elements, nodes);

    // U across the width (0/1), V = fAge along the length (the SC2 clock puts
    // age on V, RE 4.6); the material's own tiling is separate. The section
    // expansion is shared with the spline path.
    std::vector<StripNode> strip;
    strip.reserve(nodes.size());
    for (const Sc2Node& n : nodes)
        strip.push_back({n.pos, n.tangent, n.up, n.size, n.twist, n.v, n.color});
    return EmitSc2Strip(strip, Sc2SectionFor(s, UsesSmoothFrame(tech)),
                        ctx.cameraDir, s.innerRadius, out);
}

// ---------------------------------------------------------------------------
// SC2 spline (technique 1). One cubic Bezier from the single SRIB record - a
// whole-ribbon shape rebuilt each frame, not a trail of aged segments.
// Verified against CRibbon_Simulate_Spline (4.8 0x10295E170): control-point
// construction, persistent age-squared sag, 32 samples at t = i/31, V = 1 - t.
// ---------------------------------------------------------------------------

/// The Bezier control points, in WORLD space, with this frame's overlay waves
/// applied. The binary builds them emitter-local and multiplies by the emitter
/// world at draw; folding inv(emitterWorld)*emitterWorld away lets C0/C1 come
/// straight off the emitter frame and C3/C2 off the SRIB node frame, so no
/// matrix inverse is needed.
///
/// `sizeWave` and `alphaWave` come back because they are the two channels the
/// SAMPLING loop applies rather than the control points.
RibbonEmitter::Sc2SplineFrame RibbonEmitter::BuildSplineFrameSc2() const {
    Sc2SplineFrame f;
    const auto& s = desc_.sc2;
    const auto& sp = s.spline;
    const bool swap = s.SwapsYawPitch();

    const Matrix44f& ew = state_.transform;
    const Matrix44f& nw = state_.sc2.splineNodeTransform;

    // Overlay waves (W6). Simulate_Spline runs the wave clock on the whole-ribbon
    // age; a channel with a wave type REPLACES the base yaw/pitch (its else
    // branch reads the static value only when the type is 0), while the
    // speed/velocity waves ADD to the tangent-length factors (scaled by the
    // precomputed norm factors). All inert when the types are 0, so a spline
    // with no wave channels is byte-identical to the W5 base.
    const f32 ot = sc2_->splineAge;
    const f32 phase = state_.sc2.overlayPhase;
    const u32* wt = desc_.sc2.waveTypes;      // main: yaw/pitch/speed/size/alpha
    const u32* swt = sp.waveTypes;            // spline: yaw/pitch/velocity
    const auto mainWave = [&](i32 i) -> f32 {
        return wt[i] ? sc2::SampleWave(wt[i], state_.sc2.waveFreq[i] * ot + phase,
                                          state_.sc2.waveAmp[i])
                     : 0.0f;
    };
    const auto splWave = [&](i32 i) -> f32 {
        return swt[i] ? sc2::SampleWave(swt[i], state_.sc2.splineWaveFreq[i] * ot + phase,
                                           state_.sc2.splineWaveAmp[i])
                      : 0.0f;
    };
    const f32 ribYaw = wt[WaveChannel::Yaw] ? mainWave(WaveChannel::Yaw)
                                            : state_.sc2.yawDeg;
    const f32 ribPitch = wt[WaveChannel::Pitch] ? mainWave(WaveChannel::Pitch)
                                                : state_.sc2.pitchDeg;
    const f32 sribYaw = swt[SplineWaveChannel::Yaw] ? splWave(SplineWaveChannel::Yaw)
                                                    : state_.sc2.splineYawDeg;
    const f32 sribPitch = swt[SplineWaveChannel::Pitch]
                              ? splWave(SplineWaveChannel::Pitch)
                              : state_.sc2.splinePitchDeg;
    const vs::Mat3 rRib = sc2::YawPitchBasis(ribYaw, ribPitch, swap);
    const vs::Mat3 rSrib = sc2::YawPitchBasis(sribYaw, sribPitch, swap);
    const f32 baseFactor =
        state_.sc2.velocityBaseFactor +
        mainWave(WaveChannel::Speed) * sp.emissionVectorNormFactor;
    const f32 endFactor =
        state_.sc2.velocityEndFactor +
        splWave(SplineWaveChannel::Velocity) * sp.velocityNormFactor;

    // The four SRIB vec3s are DIRECT Bezier control points, each a POINT in its
    // own frame — Simulate_Spline (4.8 0x10295E170, pinned by O9) does NOT add
    // the endpoints to the tangents. C0/C1 are emissionOffset and the
    // RIB-rotated emissionVector·baseFactor, both through the emitter frame; C3/
    // C2 are endOffset and the SRIB-rotated endTangent·endFactor, both through
    // the SRIB node frame. C1 and C2 are transform_POINT (they take the frame's
    // translation too — O9's node-translate vector records C2 = nodeTranslate +
    // R_srib·endTangent, matching C3's translation with no cross term). Adding
    // the endpoints to the tangents was a documented-but-wrong RE guess (§3.4).
    const Vector3f c0 = whiteout::transform_point(sp.emissionOffset, ew);
    const Vector3f c1 = whiteout::transform_point(
        vs::Scale(vs::MulVecMat3(sp.emissionVector, rRib), baseFactor), ew);
    const Vector3f c3 = whiteout::transform_point(
        vs::MulVecMat3(sp.endOffset, rSrib), nw);
    const Vector3f c2 = whiteout::transform_point(
        vs::Scale(vs::MulVecMat3(sp.endTangent, rSrib), endFactor), nw);


    f.p[0] = vs::Add(c0, sc2_->sag[0]);
    const f32 sizeWave = mainWave(WaveChannel::Size);
    const f32 alphaWave = mainWave(WaveChannel::Alpha);
    f.p[1] = vs::Add(c1, sc2_->sag[1]);
    f.p[2] = vs::Add(c2, sc2_->sag[2]);
    f.p[3] = vs::Add(c3, sc2_->sag[3]);
    f.sizeWave = sizeWave;
    f.alphaWave = alphaWave;
    return f;
}

/// The cross-section up. A GPU spline (no noise) computes a STABILIZED up in
/// the VS from the control points - the mean plane normal, x-biased so
/// near-colinear points still resolve (Ribbon.fx:361, pinned by O12's
/// spline_up). A fixed +Z twists a tube whenever the curve runs parallel to it
/// (the vertical Spine Crawler stalk). A noisy CPU spline instead writes
/// emitter +Z per element (RE 3.4), so that is kept for the noise path.
Vector3f RibbonEmitter::SplineUpSc2(const Sc2SplineFrame& f) const {
    const auto& s = desc_.sc2;
    const Matrix44f& ew = state_.transform;
    if (s.HasNoise()) {
        return vs::SafeNormalize(whiteout::transform_normal(Vector3f{0, 0, 1}, ew),
                                     Vector3f{0, 0, 1});
    } else {
        Vector3f n0 = vs::Cross3(vs::Sub(f.p[1], f.p[0]), vs::Sub(f.p[3], f.p[0]));
        const Vector3f n1 = vs::Cross3(vs::Sub(f.p[2], f.p[3]), vs::Sub(f.p[0], f.p[3]));
        if (vs::Dot3(n0, n1) < 0.0f)
            n0 = vs::Scale(n0, -1.0f);
        n0 = vs::Add(n0, n1);
        n0.x += 1.0f;
        return vs::SafeNormalize(n0, Vector3f{0, 0, 1});
    }
}

i32 RibbonEmitter::BuildStripSplineSc2(const RibbonBuildContext& ctx,
                                       std::vector<Vertex>& out) const {
    if (state_.visibility <= 0.0f)
        return 0;
    const auto& s = desc_.sc2;

    // Three steps: where the curve IS, which way its section faces, and what
    // each sample along it looks like.
    const Sc2SplineFrame f = BuildSplineFrameSc2();
    const Vector3f splineUp = SplineUpSc2(f);
    const f32 sizeWave = f.sizeWave, alphaWave = f.alphaWave;

    // B(t) = C0(1−t)³ + 3C1·t(1−t)² + 3C2·t²(1−t) + C3·t³ (weights in the
    // binary's order).
    auto cubic = [&](f32 t) -> Vector3f {
        const f32 u = 1.0f - t;
        const f32 w0 = u * u * u, w1 = 3.0f * t * u * u, w2 = 3.0f * t * t * u,
                  w3 = t * t * t;
        return {f.p[0].x * w0 + f.p[1].x * w1 + f.p[2].x * w2 + f.p[3].x * w3,
                f.p[0].y * w0 + f.p[1].y * w1 + f.p[2].y * w2 + f.p[3].y * w3,
                f.p[0].z * w0 + f.p[1].z * w1 + f.p[2].z * w2 + f.p[3].z * w3};
    };

    // Every spline takes Ribbon.fx's smooth (non-flattened) frame branch.
    const Sc2Section section = Sc2SectionFor(s, /*smoothPath=*/true);

    std::vector<Vector3f> pos(kSplineSamples);
    for (i32 i = 0; i < kSplineSamples; ++i)
        pos[static_cast<usize>(i)] = cubic(static_cast<f32>(i) / (kSplineSamples - 1.0f));

    // fAge = t drives the interpolators (start value at C0/emitter, end value at
    // C3/tip); V = 1 − t (RE §4.6). up = world +Z; tangent = the finite-diff
    // curve direction (spline elements carry no velocity, so the frame reads the
    // geometry). Every spline takes Ribbon.fx's smooth (non-flattened) branch.
    std::vector<StripNode> strip(kSplineSamples);
    for (i32 i = 0; i < kSplineSamples; ++i) {
        const usize u = static_cast<usize>(i);
        const f32 t = static_cast<f32>(i) / (kSplineSamples - 1.0f);
        const auto posAt = [&](usize k) { return pos[k]; };
        const Vector3f tan = Sc2GeometryTangent(u, pos.size(), posAt);

        StripNode n;
        n.pos = pos[u];
        // Noise displacement (RE 4.3): a spline mutes near the head by t/edge
        // or, past 1 - edge, near the tip by (1-t)/edge. Present only when noise
        // was authored (which demotes the spline to Legacy but keeps the spline
        // geometry).
        if (s.HasNoise()) {
            n.pos = vs::Add(n.pos, sc2::NoiseDisplacement(
                                       t, sc2_->splineAge, s.noiseAmplitude * state_.unitScale,
                                       s.noiseFrequency, s.noiseCoherence, s.noiseEdge,
                                       /*spline=*/true));
        }
        n.up = splineUp;
        n.tangent = vs::SafeNormalize(tan, Vector3f{1, 0, 0});

        // The same interpolators the trail path runs, on the emitter's own keys
        // rather than an element's: a spline has no elements. The size wave is
        // folded into the keys, and the half-extent is size*0.25 (RE 4, A7) --
        // the binary halves TWICE and the spline has no head writer to have done
        // the first, so both halvings land here or the tube renders twice as
        // thick.
        Sc2Keys keys;
        keys.size3 = {state_.sc2.size3.x + sizeWave, state_.sc2.size3.y + sizeWave,
                      state_.sc2.size3.z + sizeWave};
        keys.rotation3 = state_.sc2.rotation3;
        for (i32 c = 0; c < ColorStop::kCount; ++c)
            keys.color3[c] = state_.sc2.color3[c];
        const Sc2Attributes attr =
            Sc2SampleAttributes(t, keys, s, kSplineSizeScale * state_.unitScale);
        n.twist = attr.twist;
        n.size = attr.size;
        // The alpha overlay wave rides on top of the interpolated alpha, as
        // Simulate_Spline applies it.
        n.color = {attr.color.x, attr.color.y, attr.color.z,
                   std::clamp(attr.color.w + alphaWave, 0.0f, 1.0f)};
        n.v = 1.0f - t;
        strip[u] = n;
    }
    return EmitSc2Strip(strip, section, ctx.cameraDir, s.innerRadius, out);
}

} // namespace whiteout::flakes::renderer::ribbon
