#include "renderer/particle/d3_emitter.h"

#include "io/d3/d3_sno_cache.h"
#include "renderer/particle/d3_orientation.h"
#include "renderer/particle/particle_draw.h"
#include "renderer/particle/particle_geometry.h"

#include <algorithm>
#include <cmath>

namespace whiteout::flakes::renderer::particle::d3 {

namespace {

/// @brief `ch5 * ch35` as the byte the draw is gated on.
///
/// `Particle_PrepareDrawFrame` @0x71000BCC48 loads `particle+0xD0` and turns it
/// into an integer: below 1e-6 (@0x7100E3BEA0) the result is zero and
/// `TST W27,#0xFF / B.EQ` @0x71000BCD54 skips `Particle_WriteQuadVertices`
/// outright; at or above 0.999999 (@0x7100E3BFF8) it is opaque; in between it is
/// `FCVTPS(s * 255)` @0x71000BCC80, i.e. a ceil.
///
/// This is why neither channel belongs in the quad's extent. `arScalePath`'s
/// shipped shape is a 0 -> 1 -> 0 ramp over the particle's life, which is a fade,
/// and `arEffectScalePath`'s range never exceeds 1.0 on any of the 21,593 files,
/// which is an attenuation.
u8 D3OpacityByte(f32 s) {
    if (!(s >= kEpsilon))
        return 0;
    if (s >= kNearlyOne)
        return 255;
    return static_cast<u8>(std::ceil(s * 255.0f));
}

/// The four POSITIONAL uv sets, and the base RECTANGLE each one's coordinates
/// are built over. `Particle_WriteQuadVertices` takes that rectangle off
/// **stage 0's** sheet and no other's, and off the SHEET rather than off the
/// flip-book: it reads the frame table before it has looked at the entry's uv
/// mode at all, so a type-1 layer whose texture carries frames shapes the
/// quad even when its entry is mode 0 or 2, while a flip-book on any later
/// stage gets the unit square (and has to carry its own scale — see
/// D3UvXform::atlasScale). A set with no layer bakes the identity rectangle.
struct UvSet {
    const MaterialLayer* layer = nullptr;
    f32 extU = 1.0f;
    f32 extV = 1.0f;
};

/// What the material fixes for every quad of the emitter.
struct QuadSheet {
    UvSet sets[MaterialDesc::kMaxLayers];
    /// A non-square tile makes a non-square quad: the engine divides the frame's
    /// V extent in PIXELS by its U extent in pixels and scales the quad's
    /// vertical half-extent by it. Same sheet as the base rectangle — stage 0's.
    /// Square tiles, which is most of the corpus, leave this at 1.
    f32 atlasAspect = 1.0f;
};

QuadSheet SheetFor(const MaterialDesc& m) {
    QuadSheet q;
    UvSet* const sets = q.sets;
    for (u32 s = 0; s < MaterialDesc::kMaxLayers; ++s) {
        if (m.setLayer[s] < 0)
            continue;
        sets[s].layer = &m.layers[static_cast<usize>(m.setLayer[s])];
        if (s == 0 && sets[s].layer->atlas) {
            const Vector2f tile = sets[s].layer->atlas->TileSize();
            sets[s].extU = tile.x;
            sets[s].extV = tile.y;
        }
    }
    if (sets[0].layer && sets[0].layer->atlas && sets[0].layer->atlas->width > 0 &&
        sets[0].layer->atlas->height > 0) {
        const f32 wpx = sets[0].extU * static_cast<f32>(sets[0].layer->atlas->width);
        const f32 hpx = sets[0].extV * static_cast<f32>(sets[0].layer->atlas->height);
        if (wpx > kEpsilon && hpx > kEpsilon)
            q.atlasAspect = hpx / wpx;
    }
    return q;
}

/// The quad's right and up for one particle.
///
/// `Particle_BuildOrientationBasis`, read out as a right/up pair — see
/// `d3_orientation.h`. When a mode writes nothing (0 ungated, 1 and 8 — 60.7% of
/// the corpus) or the frame comes out degenerate, the engine leaves the
/// emitter's frozen quaternion standing and the GPU builds the quad from THAT,
/// not from the camera: `Particle_WriteQuadVertices` carries a packed quaternion
/// and no view (G-D3P-11/12), and the modes that do want the camera fold
/// `camForward` in themselves (6, 13, 0-gated). So the fallback is the emitter
/// frame's own right/up — a ground effect like player_fogRipple (mode 1) then
/// lies flat under an upright emitter instead of standing up model-tall to face
/// the viewer.
void QuadAxes(const EmitterDesc& d, const ParticleState& st, const Vector3f& fromSystem,
              const Vector3f& camForward, Vector3f& right, Vector3f& up) {
    right = st.birthEmitterQuat.rotate_vector(Vector3f{1.0f, 0.0f, 0.0f});
    up = st.birthEmitterQuat.rotate_vector(Vector3f{0.0f, 1.0f, 0.0f});
    if (QuadFrame frame; BuildQuadFrame(d.renderMode,
                                       {camForward, st.axis, st.axisUnit, fromSystem,
                                        st.groundNormal, st.birthEmitterQuat},
                                       frame)) {
        right = frame.right;
        up = frame.up;
    }

    // Roll spins the quad in its own plane; the spin quaternion rotates the
    // plane itself.
    if (st.rollAngle != 0.0f) {
        const f32 c = std::cos(st.rollAngle), s = std::sin(st.rollAngle);
        const Vector3f r2{right.x * c + up.x * s, right.y * c + up.y * s, right.z * c + up.z * s};
        const Vector3f u2{up.x * c - right.x * s, up.y * c - right.y * s, up.z * c - right.z * s};
        right = r2;
        up = u2;
    }
    if (d.Cap(kCapSpin)) {
        right = st.orientation.rotate_vector(right);
        up = st.orientation.rotate_vector(up);
    }
}

/// This particle's four texcoord transforms, one per set. Everything that
/// varies is read out of the particle's own UV state, which is the whole reason
/// these are baked per vertex rather than uploaded per draw: the scroll phase
/// and its rate were drawn at emit, the rotation has been accumulating since,
/// and each set walks its own flip-book.
void LayerAffines(const QuadSheet& sheet, const ParticleState& st,
                  f32 aff[MaterialDesc::kMaxLayers][6]) {
    for (u32 s = 0; s < MaterialDesc::kMaxLayers; ++s) {
        f32(&a)[6] = aff[s];
        a[0] = 1.0f; a[1] = 0.0f; a[2] = 0.0f;
        a[3] = 0.0f; a[4] = 1.0f; a[5] = 0.0f;
        const MaterialLayer* L = sheet.sets[s].layer;
        if (!L)
            continue;
        if (L->uv.mode == ::whiteout::flakes::io::D3UvMode::Anim2D) {
            // `MatTex_BuildUvAffine2x3` case 3: a translation to frame k's
            // origin, plus frame ZERO's far corner as a scale when the entry
            // asks. Never frame k's corner — only the origin varies.
            if (L->atlas && !L->atlas->frames.empty()) {
                const auto& frames = L->atlas->frames;
                const i32 k = std::clamp(static_cast<i32>(st.uv[s].cursor), 0,
                                         static_cast<i32>(frames.size()) - 1);
                a[2] = frames[static_cast<usize>(k)].x;
                a[5] = frames[static_cast<usize>(k)].y;
                if (L->uv.atlasScale) {
                    a[0] = frames[0].z;
                    a[4] = frames[0].w;
                }
            }
        } else {
            ::whiteout::flakes::io::D3UvAffineAt(L->uv, st.uv[s].u, st.uv[s].v, st.uv[s].rot, a);
        }
        // Fold the base rectangle in, so the quad's own coordinates stay the
        // unit square and the corner below is one multiply-add.
        a[0] *= sheet.sets[s].extU;
        a[3] *= sheet.sets[s].extU;
        a[1] *= sheet.sets[s].extV;
        a[4] *= sheet.sets[s].extV;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

i32 Emitter::BuildGeometry(const BuildGeometryInput& in, std::vector<Vertex>& out) const {
    if (!in.worldToView || Pool().AliveCount() == 0)
        return 0;

    const Matrix44f& view = *in.worldToView;
    // What `Particle_PrepareDrawFrame` hands the frame builder as its last
    // argument: `view+0x28C`, the same vector the particle draw list sorts on.
    const Vector3f camForward{-view.data[0][2], -view.data[1][2], -view.data[2][2]};
    const Vector3f sysPos = WorldPosition();

    const EmitterDesc& d = *d3desc_;
    const i32 before = static_cast<i32>(out.size());
    // The flip-book, if this material has one. The tile SIZE is constant — the
    // engine reads it off frame 0 and applies it to every frame — and rides the
    // layer's UV transform in the constant buffer; only the tile ORIGIN varies
    // per particle, and that goes in the vertex, which is what `normal` is for
    // here. Nothing in the D3 particle program reads a normal (the family is
    // unlit; `Particle_DrawBatch` uploads no light constants for it), so the
    // three floats were already dead weight in this stream.
    Vector3f normal{0.0f, 0.0f, 1.0f};
    const QuadSheet sheet = SheetFor(d.d3mat);
    // Renderer units per `.prt` unit. Positions are already in renderer units;
    // the size channels and the wind spring's offset are not.
    const f32 u = UnitScale();

    // The quad's corner signs and its UVs, matching the WC3/WoW builder so the
    // two streams stay interchangeable at the dispatcher.
    static constexpr f32 kCorner[4][2] = {{-1, 1}, {-1, -1}, {1, 1}, {1, -1}};
    static constexpr f32 kUV[4][2] = {{0, 0}, {0, 1}, {1, 0}, {1, 1}};

    for (usize i = 0; i < Pool().AliveCount(); ++i) {
        const u32 idx = Pool().AliveAt(i);
        const Particle2& p = Pool()[idx];
        const ParticleState& st = states_[idx];

        const Vector3f pos = p.position;
        // The sway offset BENDS the quad: `Particle_WriteQuadVertices` adds it to
        // the two +v corners and leaves the two -v corners on the particle's own
        // position (G-D3P-12), so foliage leans from a base that stays put.
        // Translating the whole particle by it slides the plant across the ground.
        Vector3f sway{0, 0, 0};
        if (UsesWindSpring(d.systemType))
            sway = {st.swayOffset.x * u, st.swayOffset.y * u, 0.0f};

        Vector3f right, up;
        QuadAxes(d, st, {pos.x - sysPos.x, pos.y - sysPos.y, pos.z - sysPos.z}, camForward,
                 right, up);

        // Width is `particle+0xD4` ALONE — `Particle_PrepareDrawFrame` stores it
        // to drawDesc+4 @0x71000BCD60 and `Particle_WriteQuadVertices` halves it
        // @0x71000BC4C4. The opacity term never reaches either extent. The
        // height takes the same half-width through the sheet aspect and then
        // ch2: `v55 = (v11 * v23) * drawDesc[2]` @0x71000BC4E4.
        const f32 half = st.size * 0.5f * u;
        if (!(half > 0.0f))
            continue;
        const u8 op = D3OpacityByte(st.opacity);
        if (op == 0)
            continue;
        const f32 halfV = half * sheet.atlasAspect * st.heightRatio;

        f32 aff[MaterialDesc::kMaxLayers][6];
        LayerAffines(sheet, st, aff);

        // Straight, not premultiplied: the blend factors come from the `.prt`'s
        // own RenderPass and 169 of the corpus's 223 particle passes already
        // source SrcAlpha, so folding alpha into the colour here would apply it
        // twice. Same convention the WC3/WoW builder uses.
        //
        // The alpha is the opacity byte ALONE. Whatever `arColorPath` authored
        // in its own alpha lane is overwritten @0x71000BCCAC, and ch6 rides
        // COLOR1 instead.
        Vector4f vcol = st.color;
        vcol.w = static_cast<f32>(op) * (1.0f / 255.0f);

        Vertex v[4];
        Vector2f tc[4][MaterialDesc::kMaxLayers];
        for (i32 c = 0; c < 4; ++c) {
            const f32 sx = kCorner[c][0] * half;
            const f32 sy = kCorner[c][1] * halfV;
            const Vector3f base = kCorner[c][1] > 0.0f ? Vector3f{pos.x + sway.x, pos.y + sway.y,
                                                                 pos.z + sway.z}
                                                       : pos;
            v[c].position = {base.x + right.x * sx + up.x * sy, base.y + right.y * sx + up.y * sy,
                             base.z + right.z * sx + up.z * sy};
            v[c].normal = normal;
            v[c].color = vcol;
            v[c].uv = {kUV[c][0], kUV[c][1]};
            for (u32 s = 0; s < MaterialDesc::kMaxLayers; ++s) {
                const f32* a = aff[s];
                tc[c][s] = {a[0] * kUV[c][0] + a[1] * kUV[c][1] + a[2],
                            a[3] * kUV[c][0] + a[4] * kUV[c][1] + a[5]};
            }
        }
        static constexpr i32 kWind[6] = {0, 1, 2, 3, 2, 1};
        for (const i32 c : kWind)
            out.push_back(v[c]);
        if (in.d3) {
            for (const i32 c : kWind) {
                in.d3->uv01.push_back({tc[c][0].x, tc[c][0].y, tc[c][1].x, tc[c][1].y});
                in.d3->uv23.push_back({tc[c][2].x, tc[c][2].y, tc[c][3].x, tc[c][3].y});
            }
            for (i32 c = 0; c < 6; ++c)
                in.d3->color1.push_back(st.dissolve);
        }
    }

    return static_cast<i32>(out.size()) - before;
}

} // namespace whiteout::flakes::renderer::particle::d3
