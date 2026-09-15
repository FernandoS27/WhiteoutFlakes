#pragma once

// ============================================================================
// particle_trace — deterministic CPU capture of the particle sim and geometry,
// checked against a recorded baseline (PARTICLE_TYPES_DESIGN.md). L1: per-
// particle pool state, to bisect a failure to a frame and particle. L2: each
// emitter's vertex- and side-stream summary. Needs no device.
// See M2_PARTICLE_DESIGN.md §11.12.
// ============================================================================

#include "renderer/particle/particle_service.h"
#include "types.h"
#include "whiteout/flakes/types.h"

#include <string>
#include <vector>

namespace whiteout::flakes::renderer::particle {

struct TraceParticle {
    Vector3f position{0, 0, 0};
    Vector3f velocity{0, 0, 0};
    f32 age = 0.0f;
    // Particle2's dialect-owned four bytes, recorded raw: a curve cursor under
    // WC3, a packed lifespan variance + render seed under WoW. Compared
    // verbatim either way, which is what a regression diff wants.
    u32 aux = 0;
};

struct TraceEmitter {
    ModelId model = 0;
    u8 output = 0; // ParticleOutput; part of the emitter's identity
    i32 emitterId = 0;

    // L1
    std::vector<TraceParticle> particles;

    // L2 — summary of this emitter's slice of the frame's vertex stream.
    i32 vertexCount = 0;
    u64 vertexHash = 0;
    i32 priorityPlane = 0;
    Vector3f boundsMin{0, 0, 0};
    Vector3f boundsMax{0, 0, 0};
    Vector4f meanColor{0, 0, 0, 0};

    // L2, the side streams: a refraction emitter's vertices (which never reach
    // the shared stream), the two extra UV sets of a refraction or
    // multi-texture emitter, and a Diablo III emitter's baked texcoords and
    // second colour. `sideCount` is the refraction slice's vertex count.
    i32 sideCount = 0;
    u64 sideHash = 0;
};

struct TraceFrame {
    i32 frame = 0;
    std::vector<TraceEmitter> emitters; // sorted by (model, emitterId)
};

struct Trace {
    std::vector<TraceFrame> frames;
    /// False for a baseline recorded before the side streams were captured
    /// (`wpt1`/`wpt2`); the side fields are compared only when both traces
    /// carry them, so an old baseline still checks everything it recorded.
    bool hasSideStreams = true;
};

// Capture one frame's L1 + L2 state. `worldToView` drives the geometry build;
// pass the same matrix every frame so the trace stays camera-independent.
void CaptureFrame(const ParticleService& svc, const Matrix44f& worldToView, i32 frame, Trace& out);

bool WriteTrace(const Trace& t, const std::string& path, std::string& err);
bool ReadTrace(Trace& t, const std::string& path, std::string& err);

// Per-field tolerances. All-zero means bit-identical, which is the expectation
// for every step of the refactor except the curve and PE1 steps.
struct CompareTolerance {
    f32 position = 0.0f;
    f32 velocity = 0.0f;
    f32 age = 0.0f;
    f32 color = 0.0f;   // per-channel, on the emitter mean
    f32 bounds = 0.0f;
    bool requireVertexHash = true; // off when a tolerance is in play
};

// True when `actual` matches `baseline` within tolerance. On failure `report`
// names the first divergence: frame, emitter, particle index, and field.
bool CompareTraces(const Trace& baseline, const Trace& actual, const CompareTolerance& tol,
                   std::string& report);

} // namespace whiteout::flakes::renderer::particle
