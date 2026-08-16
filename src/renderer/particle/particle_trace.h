#pragma once

// ============================================================================
// particle_trace — deterministic CPU-side capture of the particle sim and its
// geometry output, for validating a refactor against a recorded baseline.
//
// Two levels, per PARTICLE_TYPES_DESIGN.md:
//   L1  per-particle pool state (position / velocity / age / key cursor). This
//       is what bisects a failure to an exact frame and particle.
//   L2  a summary of each emitter's contribution to the vertex stream (count,
//       checksum, bounds, mean colour). Catches UV cell selection, corner math,
//       tail construction, sort order and fog combination — everything L1
//       cannot see — without needing a GPU.
//
// Neither level touches the device: BuildGeometry is a pure function of sim
// state plus a view matrix, so a trace run needs no rendering.
// ============================================================================

#include "particle_service.h"
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
};

struct TraceFrame {
    i32 frame = 0;
    std::vector<TraceEmitter> emitters; // sorted by (model, emitterId)
};

struct Trace {
    std::vector<TraceFrame> frames;
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
