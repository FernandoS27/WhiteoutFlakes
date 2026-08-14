#pragma once

// ============================================================================
// draw_trace — deterministic CPU-side record of every decision the draw path
// makes, diffed against a recorded baseline. The gate that protects the
// multi-format refactor (REFACTOR_PLAN.md §2, gate G1).
//
// Modelled on particle_trace.h: same two-level structure (frame → draws), same
// WriteTrace / ReadTrace / CompareTraces shape, same versioned magic with a
// forward-compatible reader. Do not invent a second pattern.
//
// Three rules the schema exists to enforce:
//   * Never record a raw gfx handle. Handles are allocation-order dependent
//     and differ run-to-run for reasons that are not regressions. Record
//     stable identities (structural actor ref, geoset index, texture id) and
//     a streamMask.
//   * Record submit order, not a sorted set. Order *is* the thing under test.
//   * Carry the fields the refactor will introduce (passSlot, shadingModel)
//     from the first commit, emitted as constants pre-refactor, so a phase
//     that starts populating one is not also a re-baseline.
//
// This header is device-free and depends on nothing above `types.h`, so the
// comparator is unit-testable (tests/draw_trace_test.cpp) without a GPU.
// ============================================================================

#include "whiteout/flakes/types.h"

#include <string>
#include <vector>

namespace whiteout::flakes::renderer::debug {

// Which pass the draw was submitted into. Only the first two are produced
// today; the rest are declared so P5's real pass list does not re-baseline.
enum class TracePassSlot : u8 {
    OpaqueColor = 0,
    TransparentScene = 1,
    ShadowMap = 2,
    DepthPrepass = 3,
    GBuffer = 4,
};

// Which producer issued the draw. `SurfacePass` (P2) only ever sees Geoset;
// the other three keep their own hooks for the life of the plan, and P2's gate
// is that this histogram is unchanged.
enum class TraceProducer : u8 { Geoset = 0, Particle = 1, Ribbon = 2, Corn = 3 };

// Pre-refactor these are the four BLS programs a geoset draw can resolve to,
// which is what `ShadingModelId` will name from P2 on.
enum class TraceShadingModel : u8 {
    Wc3Sd = 0,
    Wc3Hd = 1,
    Wc3SdOnHd = 2,
    Wc3Crystal = 3,
    Unlit = 4,
    M2Combiners = 5,
    None = 255,
};

inline constexpr i32 kTraceTexSlots = 8;

// Structural identity of the drawing actor. Deliberately *not* the actor
// handle: PE1 and attachment children take theirs from AllocActorId() while
// iterating unordered maps, so which child gets handle 42 is hash-order
// dependent — and those are exactly the corpus entries the trace must pin.
struct TraceActorRef {
    // Rank of the top-level ancestor among the scene's top-level actors, in
    // handle order — not its handle. A handle is a position in the
    // AllocActorId sequence, so it moves whenever anything upstream of it
    // spawns, and the gate's perturbation arm shifts the whole sequence on
    // purpose. A rank derived from a sort survives both.
    u32 rootActor = 0;
    u8 role = 0;        // model::ActorRole
    u8 treeDepth = 0;
    i32 emitterId = -1; // PE1 emitter that spawned it, else -1
    i32 slotIndex = -1; // attachment slot that spawned it, else -1
};

struct TraceDraw {
    // ---- where in the frame ----
    u8 passSlot = 0;     // TracePassSlot
    u8 producer = 0;     // TraceProducer
    u8 shadingModel = 0; // TraceShadingModel
    u8 blendClass = 0;   // bls::GxMatAlpha
    u8 depthFill = 0;    // bls::DepthFill

    // ---- what is being drawn ----
    TraceActorRef actor;
    i32 submesh = -1; // geoset index
    i32 surface = -1; // material index
    i32 layer = -1;   // layer within the surface (-1 = whole-geoset draw)
    u32 lod = 0;

    // ---- ordering ----
    i32 priorityPlane = 0;
    i32 sortOrder = -1; // position in the sorted list this draw came out of
    f32 sqDist = 0.0f;  // bit-exact; see CompareTolerance::distance
    u8 underWater = 0;

    // ---- material resolve ----
    i32 filterMode = 0;
    i32 matFlags = 0;
    i32 texIds[kTraceTexSlots] = {-1, -1, -1, -1, -1, -1, -1, -1};
    i32 texAnimId = -1;

    // ---- geometry ----
    i32 indexCount = 0;
    i32 vertexCount = 0;
    u8 streamMask = 0;  // TraceStream bits
    u8 palettePath = 0; // 0 none, 1 per-actor (A), 2 per-geoset (B)
    i32 paletteSlots = 0;

    // ---- lighting ----
    u8 lightCount = 0;        // live permutation dimension in SD
    u64 lightPaletteHash = 0; // over frame.lights[0..n) + lightAmbientColors

    // ---- resolved state ----
    // Decision inputs only — never HashRequest(), which seeds with the program
    // pointer (moves with ASLR) and folds in rtv/dsv formats (differ by vendor
    // and backend). See TracePsoInputs.
    u32 psoKey = 0;
    u64 cbHash = 0;          // over the constant-buffer *values*, not the shape
    f32 combinedAlpha = 1.0f;
    u64 texMtxHash = 0;
};

// Which vertex streams the draw bound. Bit positions are core::StreamId's
// values — the same four buffers, named once. Kept as plain constants here so
// draw_trace stays dependency-free and the comparator is unit-testable without
// pulling in the renderer.
enum TraceStream : u8 {
    kStreamBase = 1 << 0,    // slot 0, the interleaved Vertex
    kStreamBaseUv1 = 1 << 1, // slot 0 came from the second interleaved copy
    kStreamTangent = 1 << 2,
    kStreamBone = 1 << 3,
};

struct TraceFrame {
    i32 frame = 0;
    std::vector<TraceDraw> draws; // in submit order
};

struct DrawTrace {
    std::vector<TraceFrame> frames;
};

// ---------------------------------------------------------------------------
// PSO key
// ---------------------------------------------------------------------------

// The subset of PsoRequest that is a *decision*. `program`, `rtvFormat` and
// `dsvFormat` are excluded by construction: they are environment, not choice.
struct TracePsoInputs {
    u32 vsPermute = 0;
    u32 psPermute = 0;
    u32 matAlpha = 0;
    u32 disables = 0;
    u32 vertexLayout = 0;
    u32 extraRtvCount = 0;
    // Not in the design's list, but a genuine decision input rather than a
    // format: the HD fading-opaque prepass twin and its colour draw differ on
    // this alone, and without it the two collide onto one key.
    bool extraColorWrite = false;
    bool wireframe = false;
    bool lhClipSpace = false;
};

u32 TracePsoKey(const TracePsoInputs& in);

// FNV-1a over raw bytes. Exposed so the submission hooks hash constant-buffer
// values with the same function the comparator's tests exercise.
u64 TraceHashBytes(const void* data, usize size, u64 seed = 0xCBF29CE484222325ull);

// ---------------------------------------------------------------------------
// Recording
// ---------------------------------------------------------------------------

namespace detail {
extern bool g_drawTraceOn;
}

// Hot-path predicate: every submission hook is guarded by this, so a normal
// frame pays one predictable branch.
inline bool DrawTraceEnabled() {
    return detail::g_drawTraceOn;
}

// What the submission loop knows and the innermost draw hook does not: which
// pass and queue position this draw came out of, and the sort keys that put it
// there. Published by the opaque loop and by the interleaved transparent
// dispatcher; read by whichever producer hook fires next.
struct TraceSubmitContext {
    TracePassSlot pass = TracePassSlot::OpaqueColor;
    TraceProducer producer = TraceProducer::Geoset;
    i32 sortOrder = -1;
    f32 sqDist = 0.0f;
    i32 priorityPlane = 0;
    u8 underWater = 0;
    u8 depthFill = 0;
};

// Process-wide recorder. A singleton because the hooks sit several call levels
// down inside two class templates and threading a recorder reference through
// them would be a real interface change in service of a debug feature.
class DrawTraceRecorder {
public:
    static DrawTraceRecorder& Instance();

    void Begin();
    void End();

    void BeginFrame(i32 frame);
    void Record(const TraceDraw& d);

    const DrawTrace& Trace() const {
        return trace_;
    }
    void Clear();

    TraceSubmitContext& Context() {
        return ctx_;
    }
    const TraceSubmitContext& Context() const {
        return ctx_;
    }

private:
    DrawTrace trace_;
    TraceSubmitContext ctx_;
};

// ---------------------------------------------------------------------------
// Serialisation + comparison
// ---------------------------------------------------------------------------

bool WriteTrace(const DrawTrace& t, const std::string& path, std::string& err);
bool ReadTrace(DrawTrace& t, const std::string& path, std::string& err);

// All-zero means bit-identical, which is the expectation for every phase of
// the refactor. `requireCbHash` and a nonzero float tolerance contradict each
// other — an exact hash cannot survive a perturbed value — so CompareTraces
// refuses the combination rather than silently preferring one.
struct CompareTolerance {
    f32 distance = 0.0f; // sqDist, relative
    f32 alpha = 0.0f;    // combinedAlpha, relative
    bool requireCbHash = true;
};

// Returns the name of the first field on which `a` and `b` differ, or nullptr
// when they match within tolerance. Split out from CompareTraces so the
// per-field divergence test can drive it directly: a comparator that stopped
// looking at a field would report MATCH while every regression in that field
// walked through.
const char* FirstDrawDiff(const TraceDraw& a, const TraceDraw& b, const CompareTolerance& tol);

// True when `actual` matches `baseline`. On failure `report` names the frame,
// the draw index, the field, and both values.
bool CompareTraces(const DrawTrace& baseline, const DrawTrace& actual, const CompareTolerance& tol,
                   std::string& report);

} // namespace whiteout::flakes::renderer::debug
