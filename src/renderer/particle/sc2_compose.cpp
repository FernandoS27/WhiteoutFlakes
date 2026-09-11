#include "sc2_compose.h"

#include "renderer/sc2/sc2_element.h"
#include "renderer/sc2/sc2_element_math.h"

#include <algorithm>
#include <array>
#include <bit>
#include <utility>

namespace whiteout::flakes::renderer::particle {

namespace {

/// The colour nodes are packed with alpha in the HIGH byte, then r, g, b —
/// the same packing `Sc2SampleColor` produces and OP6 pinned. The vertex
/// declaration normalises the four bytes; that normalise is not measured by
/// any gate, so it is spelled out here rather than folded into the expander.
std::array<f32, 4> UnpackColor(u32 v) {
    constexpr f32 k = 1.0f / 255.0f;
    return {static_cast<f32>((v >> 16) & 0xFFu) * k,
            static_cast<f32>((v >> 8) & 0xFFu) * k,
            static_cast<f32>(v & 0xFFu) * k,
            static_cast<f32>((v >> 24) & 0xFFu) * k};
}

} // namespace

void Sc2ParticleStore::Init(u32 maxParticles) {
    const usize n = maxParticles;
    elements.assign(n, Sc2SpawnedElement{});
    vertices.assign(n, Sc2GpuVertex{});
    list.next.assign(n, Sc2ElementList::kNull);
    list.prev.assign(n, Sc2ElementList::kNull);
    // Everything free, in index order. `Sc2ElementList::Reset` does the
    // opposite — it links a pool that is entirely LIVE, which is the fixture
    // shape the retirement gate needs and not the shape a running emitter
    // starts in.
    for (usize i = 0; i < n; ++i)
        list.next[i] = (i + 1 < n) ? static_cast<i32>(i + 1) : Sc2ElementList::kNull;
    list.head = Sc2ElementList::kSentinel;
    list.tail = Sc2ElementList::kNull;
    list.freeHead = n != 0 ? 0 : Sc2ElementList::kNull;
    list.freeTail = n != 0 ? static_cast<i32>(n - 1) : Sc2ElementList::kNull;
    list.poolCount = 0;
    recycle = Sc2RecycleArray{};
}

i32 Sc2ParticleStore::Acquire() {
    const i32 node = list.freeHead;
    if (node < 0)
        return -1;
    const usize n = static_cast<usize>(node);
    list.freeHead = list.next[n];
    if (list.freeHead < 0) {
        list.freeHead = Sc2ElementList::kNull;
        list.freeTail = Sc2ElementList::kNull;
    }

    // Appended, not prepended: the head stays the OLDEST particle, which is
    // what makes `Sc2RetireExpired`'s forward walk stop early and what makes
    // the draw order oldest-first (OP8b).
    list.next[n] = Sc2ElementList::kSentinel;
    list.prev[n] = list.tail;
    if (list.tail >= 0)
        list.next[static_cast<usize>(list.tail)] = node;
    else
        list.head = node;
    list.tail = node;
    ++list.poolCount;
    return node;
}

std::array<f32, 16> Sc2Mat16(const Matrix44f& m) {
    std::array<f32, 16> out{};
    for (usize r = 0; r < 4; ++r)
        for (usize col = 0; col < 4; ++col)
            out[r * 4 + col] = m.data[r][col];
    return out;
}

Sc2QuadCamera Sc2CameraFromView(const Matrix44f& v) {
    Sc2QuadCamera cam;
    cam.billboardRight = {v.data[0][0], v.data[1][0], v.data[2][0]};
    cam.billboardUp = {v.data[0][1], v.data[1][1], v.data[2][1]};
    // `−(right × up)`, which for an orthonormal view basis is the negated
    // third column — the same forward the WC3 builder already takes out of
    // this matrix, so the two dialects face the same way.
    cam.direction = {-v.data[0][2], -v.data[1][2], -v.data[2][2]};
    // The world position whose view-space image is the origin: `eye = −t·Rᵀ`.
    const f32 tx = v.data[3][0];
    const f32 ty = v.data[3][1];
    const f32 tz = v.data[3][2];
    cam.eye = {-(tx * v.data[0][0] + ty * v.data[0][1] + tz * v.data[0][2]),
               -(tx * v.data[1][0] + ty * v.data[1][1] + tz * v.data[1][2]),
               -(tx * v.data[2][0] + ty * v.data[2][1] + tz * v.data[2][2])};
    return cam;
}

Sc2BatchDesc Sc2BatchDescFrom(const Sc2EmitterDesc& d) {
    Sc2BatchDesc b;
    b.sizeMidTime = d.look.midTime[0];
    b.colorMidTime = d.look.midTime[1];
    b.alphaMidTime = d.look.midTime[2];
    b.rotationMidTime = d.look.midTime[3];
    b.sizeMidHoldTime = d.look.midHold[0];
    b.colorMidHoldTime = d.look.midHold[1];
    b.alphaMidHoldTime = d.look.midHold[2];
    b.rotationMidHoldTime = d.look.midHold[3];
    b.flipbookStartInitIndex = d.look.flipbookStartInit;
    b.flipbookStartStopIndex = d.look.flipbookStartStop;
    b.flipbookEndInitIndex = d.look.flipbookEndInit;
    b.flipbookMidTime = d.look.flipbookMidTime;
    b.flipbookColumns = d.look.flipbookColumns;
    b.flipbookRows = d.look.flipbookRows;
    b.flipbookColumnFraction = d.look.flipbookColumnFraction;
    b.flipbookRowFraction = d.look.flipbookRowFraction;
    b.worldSpace = d.emit.worldSpace;
    return b;
}

Sc2InitWords Sc2InitRuntimeWords(const Sc2EmitterDesc& d) {
    namespace bits = whiteout::flakes::renderer::sc2;
    Sc2InitWords w;
    const u32 flags = static_cast<u32>(d.flags);
    if ((flags & static_cast<u32>(ParticleFlag::ScaleTimeByParent)) != 0)
        w.stateFlags |= bits::kStateScaleTimeByParent;
    if ((flags & static_cast<u32>(ParticleFlag::UseLocalTime)) != 0)
        w.stateFlags |= bits::kStateUseLocalTime;
    // An ASSIGNMENT, not an or: an emitter with a fallback force pair loses the
    // two time bits it was just given. Transcribed, not tidied.
    if (d.motion.forcesFallback != 0)
        w.stateFlags = bits::kStateForces;
    if ((static_cast<u32>(d.additionalFlags) & 8u) != 0)
        w.stateFlags |= bits::kStateWorldSpace;
    if (d.motion.forces >= 0x10000u)
        w.stateFlags |= bits::kStateWorldForces;
    // Strictly above the threshold (`0x103BC8C10`, 0.001f): an amplitude of
    // exactly 0.001 demotes the emitter to Euler — `CanUseGpuMotion` tests
    // `< 0.001` — and still leaves its noise off.
    if (d.motion.noiseAmplitude > 0.001f)
        w.emitFlags |= bits::kEmitNoise;
    if ((flags & static_cast<u32>(ParticleFlag::InheritParentVelocity)) != 0)
        w.stateFlags |= bits::kStateInheritVelocity;
    // The load already asked `Sc2CanUseGpuMotion`; asking again here would be
    // a second spelling of the one selector.
    if (d.motion.analytic)
        w.stateFlags |= bits::kStateGpuMotion;
    return w;
}

Sc2QuadFlags Sc2QuadFlagsFrom(const Sc2EmitterDesc& d, bool flipbookUv,
                              bool uvRandomOffset) {
    Sc2QuadFlags f;
    f.instanceType = d.look.instanceType;
    f.sizeInterp = static_cast<i32>(d.look.sizeSmoothing);
    f.colorInterp = static_cast<i32>(d.look.colorSmoothing);
    f.rotationInterp = static_cast<i32>(d.look.rotationSmoothing);
    f.localSpace = !d.emit.worldSpace;
    // `b_useModelInstancing` is a RENDER-CONTEXT bit, not a `PAR_` one, so it
    // is never set from the desc — a caller with hardware model instancing on
    // sets it after this returns.
    f.modelInstancing = false;
    f.proceduralPosition = d.motion.analytic;
    f.fixedTailLength = d.Has(ParticleFlag::FixTailLengthOnCreation);
    f.clampedTailLength = d.Has(ParticleFlag::ClampTailLength);
    f.randomFlipbookStart = d.Has(ParticleFlag::RandomFlipbookStart);
    f.flipbookUv = flipbookUv;
    f.uvRandomOffset = uvRandomOffset;
    return f;
}

Sc2QuadInput Sc2QuadInputFrom(const Sc2GpuVertex& v) {
    Sc2QuadInput q;
    q.position = {v.position[0], v.position[1], v.position[2]};
    for (usize i = 0; i < 4; ++i)
        q.size[i] = static_cast<f32>(v.size[i]);
    for (usize i = 0; i < 3; ++i)
        q.color[i] = UnpackColor(v.color[i]);
    q.rotation = {static_cast<f32>(v.rotation[0]), static_cast<f32>(v.rotation[1]),
                  static_cast<f32>(v.rotation[2]),
                  static_cast<f32>(v.flipbookRand)};
    q.birthTime = v.birthTime;
    q.deathTime = v.deathTime;
    q.drag = v.drag;
    q.invDrag = v.invDrag;
    q.velocity = {v.velocity[0], v.velocity[1], v.velocity[2]};
    q.invMass = v.invMass;
    q.instanceVec = {v.instanceVec[0], v.instanceVec[1], v.instanceVec[2]};
    q.gravityZ = v.gravityZ;
    q.noise = {v.noise[0], v.noise[1], v.noise[2]};
    q.flipbookRandStart = v.flipbookRandStart;
    return q;
}

usize Sc2BuildQuads(const Sc2ParticleStore& store, const Sc2QuadBatch& batch,
                    const Sc2QuadCamera& camera, const Sc2QuadFlags& flags,
                    bool sortReverse, std::vector<Vertex>& out, Sc2SortKey sort,
                    f32 hostScale) {
    namespace vs = whiteout::flakes::renderer::sc2::vs;
    std::vector<i32> order;
    if (sortReverse)
        store.list.WalkBackward(order);
    else
        store.list.Walk(order);

    if (sort != Sc2SortKey::None) {
        // `BuildRenderBatch_CPU`'s keys, which are INTS: SortHeight takes the
        // element's `[+0x4C]` — the bits of its position z — and Sort the BIT
        // PATTERN of its depth along the camera, so the order is a depth order
        // only in front of the eye and folds behind it. Both read the element's
        // own position, never taken into world space, a local emitter's too.
        std::vector<std::pair<i32, i32>> keyed;
        keyed.reserve(order.size());
        for (const i32 node : order) {
            const Sc2GpuVertex& v = store.vertices[static_cast<usize>(node)];
            i32 key = 0;
            if (sort == Sc2SortKey::Height) {
                key = std::bit_cast<i32>(v.position[2]);
            } else {
                // y and z summed first, then x — the decompile's grouping.
                const f32 depth = ((v.position[1] - camera.eye.y) * camera.direction.y +
                                   (v.position[2] - camera.eye.z) * camera.direction.z) +
                                  (v.position[0] - camera.eye.x) * camera.direction.x;
                key = std::bit_cast<i32>(depth);
            }
            keyed.emplace_back(key, node);
        }
        // Back to front, the farthest first; SortReverse draws front to back.
        std::stable_sort(keyed.begin(), keyed.end(),
                         [sortReverse](const auto& a, const auto& b) {
                             return sortReverse ? a.first < b.first : a.first > b.first;
                         });
        for (usize i = 0; i < keyed.size(); ++i)
            order[i] = keyed[i].second;
    }

    usize drawn = 0;
    for (const i32 node : order) {
        const usize n = static_cast<usize>(node);
        if (n >= store.vertices.size())
            break;
        const Sc2QuadResult q =
            Sc2ExpandQuad(Sc2QuadInputFrom(store.vertices[n]), batch, camera, flags);
        if (!q.supported)
            continue;
        const Vector4f color{q.color[0], q.color[1], q.color[2], q.color[3]};
        // c0 c1 c2 / c3 c2 c1 over `kSc2Corners` — the same winding every other
        // dialect's quad takes, so one pipeline state draws all of them.
        for (const usize k : {usize{0}, usize{1}, usize{2}, usize{3}, usize{2}, usize{1}})
            out.push_back({vs::Scale(q.corner[k].position, hostScale), q.corner[k].normal,
                           color, q.corner[k].uv});
        ++drawn;
    }
    return drawn;
}

Sc2Crossing Sc2CrossSquirtKeys(const Sc2EmitterDesc& d, std::span<const Sc2ClockSample> players,
                               Sc2SquirtMemory& memory, i32 frameDtMs) {
    Sc2Crossing out;
    if (players.empty())
        return out;
    // A sequence change: the first sample, a different player list, or a clip
    // that does not loop stepping backwards. A looping player stepping back is
    // its wrap, which the reader handles itself.
    bool changed = !memory.valid || memory.stcs.size() != players.size();
    for (usize p = 0; p < players.size() && !changed; ++p) {
        changed = memory.stcs[p] != players[p].stc ||
                  (!players[p].loop && players[p].timeMs < memory.timeMs[p]);
    }
    const usize slots = d.emit.squirt.size();
    out.bursts.assign(slots, 0u);
    if (memory.sinks.size() != slots)
        memory.sinks.assign(slots, Sc2KeySink{});

    // One contiguous run of keys per player, spanned only once the vectors
    // have stopped growing.
    struct Run {
        usize from = 0, count = 0;
        i32 frame = 0;
    };
    std::vector<i32> times;
    std::vector<u16> values;
    std::vector<Run> runs;
    std::vector<Sc2KeyPlayer> live;
    for (usize s = 0; s < slots; ++s) {
        times.clear();
        values.clear();
        runs.clear();
        for (const Sc2ClockSample& p : players) {
            const usize from = times.size();
            bool resolved = false;
            i32 trackEnd = 0;
            for (const auto& key : d.emit.squirt[s]) {
                if (key.stc != p.stc)
                    continue;
                resolved = true;
                trackEnd = key.trackEnd;
                // A looping player's frame never passes its track's end, so a
                // key authored past it is one the player can never cross. The
                // wrapped arm reports the tail from the cursor, and would count
                // it on every wrap; retail's keeps only a key AT the cursor.
                if (p.loop && key.trackEnd > 0 && static_cast<i32>(key.timeMs) > key.trackEnd)
                    continue;
                times.push_back(static_cast<i32>(key.timeMs));
                values.push_back(static_cast<u16>(static_cast<i32>(key.amount)));
            }
            // No track in this player's container, or one that ends at 0:
            // retail skips the player without consuming a cursor slot. A track
            // whose every key was clipped above still takes its slot.
            if (!resolved || trackEnd == 0) {
                times.resize(from);
                values.resize(from);
                continue;
            }
            // The whole frame modulo the track's end, truncating as `idiv`
            // does — and only for a looping player.
            const i32 frame = (p.loop && trackEnd > 0) ? p.timeMs % trackEnd : p.timeMs;
            runs.push_back({from, times.size() - from, frame});
        }
        if (runs.empty())
            continue;
        live.clear();
        for (const Run& r : runs) {
            live.push_back({{std::span<const i32>(times).subspan(r.from, r.count),
                             std::span<const u16>(values).subspan(r.from, r.count)},
                            r.frame});
        }
        Sc2KeySink& sink = memory.sinks[s];
        if (changed) {
            sink.prime = true;
            Sc2CollectCrossedKeys(live, -frameDtMs, sink);
        }
        if (Sc2CollectCrossedKeys(live, 0, sink))
            out.bursts[s] = static_cast<u32>(Sc2SquirtBurst(sink));
    }
    memory.stcs.resize(players.size());
    memory.timeMs.resize(players.size());
    for (usize p = 0; p < players.size(); ++p) {
        memory.stcs[p] = players[p].stc;
        memory.timeMs[p] = players[p].timeMs;
    }
    memory.valid = true;
    return out;
}

i32 Sc2ActiveSequence(std::span<const Sc2ClockSample> players) {
    // The players arrive in priority order, and a dead one is never among
    // them. The playlist holds a concurrent global on top for the blend
    // budget; retail inserted it before any host play, so a host play of the
    // same priority is ahead of it there. Among globals, the list's own order.
    const Sc2ClockSample* best = nullptr;
    for (const Sc2ClockSample& p : players) {
        if (p.blendingOut)
            continue;
        if (best != nullptr && (p.priority < best->priority || !best->global))
            break;
        if (best == nullptr || !p.global)
            best = &p;
    }
    return best != nullptr ? static_cast<i32>(best->sequence) : -1;
}

} // namespace whiteout::flakes::renderer::particle
