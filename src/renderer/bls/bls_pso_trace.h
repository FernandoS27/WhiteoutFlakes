#pragma once

// =============================================================================
// BLS PSO trace
//
// Records the PsoRequest keys that BlsPsoBuilder actually built during a run
// and persists them so the next run can pre-warm the same set on a worker
// thread before opening the renderer window. Paired with the per-device
// VkPipelineCache file (and the d3d12 driver's internal cache), this turns
// every PSO build that fired during one run into a cache hit on the next.
//
// The trace stores only the *state-relevant* PsoRequest fields. MatParams
// scalars (diffuseColor, exposure, etc.) flow through uniform buffers and
// don't change the pipeline state, so they're omitted.
//
// File layout (little-endian, no padding):
//   u32 magic   = 'PSOT' (0x54_4F_53_50 LE)
//   u32 version = 1
//   u32 count
//   count * PsoTraceEntry  (16 bytes each)
// =============================================================================

#include "bls_mat_params.h"  // GxMatAlpha
#include "bls_permuter.h"    // GxShaderID
#include "bls_pso_builder.h" // PsoRequest, VertexLayoutKind
#include "gfx/gfx.h"         // Format, PrimitiveTopology
#include "whiteout/flakes/types.h"

#include <filesystem>
#include <unordered_set>
#include <vector>

namespace whiteout::flakes::renderer::bls {

class BlsProgramCatalog;

#pragma pack(push, 1)
struct PsoTraceEntry {
    u8 programId; // GxShaderID
    u8 alpha;     // GxMatAlpha
    u8 layout;    // VertexLayoutKind
    u8 topology;  // gfx::PrimitiveTopology

    u8 rtvFormat; // gfx::Format (8-bit cast — only common scene formats fit)
    u8 dsvFormat;
    u8 flags; // bit0=wireframe, bit1=lhClipSpace
    u8 reserved;

    u16 vsIndex;
    u16 psIndex;
    u32 disables; // MatParams::disables — full bitfield, state-affecting bits only
};
#pragma pack(pop)
static_assert(sizeof(PsoTraceEntry) == 16, "PsoTraceEntry must be 16 bytes for on-disk format");

class BlsPsoTrace {
public:
    // path: where to load and persist the trace. Empty path = in-memory
    // only (no load, no save), useful for tests. The directory is
    // created on save if it doesn't exist.
    explicit BlsPsoTrace(std::filesystem::path path);
    ~BlsPsoTrace();

    BlsPsoTrace(const BlsPsoTrace&) = delete;
    BlsPsoTrace& operator=(const BlsPsoTrace&) = delete;

    // Called by BlsPsoBuilder::GetOrBuild on every cache miss. Dedupes
    // against entries loaded from disk + previously recorded in this
    // run, so each unique key is stored exactly once.
    void Record(const PsoRequest& request);

    // Walk every loaded entry, reconstruct a PsoRequest by resolving
    // the program-id back to a BlsProgram* from `catalog`, and call
    // builder.GetOrBuild on each. Replay-built PSOs go into the
    // builder's cache, so the runtime draw path becomes a cache hit
    // for every key we replayed.
    //
    // Entries whose program/layout/permute index is invalid for the
    // current shader set are silently skipped (shaders may have been
    // recompiled with a different permute count between runs).
    void Replay(BlsPsoBuilder& builder, const BlsProgramCatalog& catalog) const;

    // Write the in-memory entries back to disk at the constructor's
    // path. Called by the dtor; expose explicitly so the renderer can
    // flush on shutdown ahead of process exit, in case the dtor runs
    // too late.
    void Save();

    usize EntryCount() const {
        return entries_.size();
    }

private:
    void Load();

    std::filesystem::path path_;
    std::vector<PsoTraceEntry> entries_;
    std::unordered_set<u64> keys_; // hashes of entries already in entries_
    bool dirty_ = false;
};

} // namespace whiteout::flakes::renderer::bls
