#pragma once

// ============================================================================
// The parts of `--draw-trace` that set a scenario up or print about it: the
// Diablo III outfit, `.m3a` attaches, sequence resolution, the solver and
// ragdoll arms, the selection overlay and the listings/probes. Split from
// draw_trace.cpp, which keeps the capture itself.
// ============================================================================

#include "cli/cli_options.h"
#include "whiteout/flakes/types.h"

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace whiteout::flakes {
struct SequenceInfo;
}
namespace whiteout::flakes::renderer {
class RenderService;
class SceneManager;
namespace model {
struct Actor;
}
} // namespace whiteout::flakes::renderer

namespace whiteout::flakes::harness::draw_trace {

using Actors = std::span<renderer::model::Actor* const>;

/// `spec` is an index if it parses as one, else a case-insensitive substring of
/// a sequence name, `+` standing in for a space. -1 when nothing matches, which
/// callers treat as "leave it alone": a corpus curated against one build should
/// not hard-fail on a model whose export lacks the sequence.
i32 ResolveSequenceSpec(const std::vector<SequenceInfo>& seqs, const std::string& spec);

/// Dress a Diablo III character by item name, before the settle so the capture
/// never sees it undressed. A no-op without `--d3-equip` or D3 compiled out.
void ApplyD3Outfit(renderer::RenderService& renderer, renderer::SceneManager& scene, Actors spawned,
                   const cli::AnimScenario& anim, const std::filesystem::path& model,
                   const std::string& contentRoot);

/// Merge each `.m3a` into every spawned `.m3`, before the scenario resolves a
/// sequence name. Nullopt on success, else the exit code.
std::optional<i32> AttachAnimations(Actors spawned, std::span<const std::filesystem::path> files);

/// `--draw-trace-anim-list`, `-particle-list`, `-ribbon-list`: print and report
/// true, after which the run ends. False when none was asked for.
bool PrintListing(renderer::RenderService& renderer, renderer::model::Actor& hero,
                  const std::vector<SequenceInfo>& seqs, const cli::AnimScenario& anim);

/// Start sequence, global loops, pose solvers and the ragdoll: everything a
/// scenario sets before frame 0.
void ApplyScenario(renderer::RenderService& renderer, Actors spawned,
                   const std::vector<SequenceInfo>& seqs, const cli::AnimScenario& anim);

/// Select every `stride`th vertex and face of every geoset, and hover the next.
void SelectMeshElements(renderer::RenderService& renderer, Actors spawned, i32 stride);

/// `--draw-trace-anim-probe` before the capture: the skinning plumbing.
void PrintProbeSetup(renderer::model::Actor& hero);
/// ... and during it: a pose hash, the clips and the model's lights.
void PrintProbeFrame(renderer::model::Actor& hero, i32 frame);
/// `--draw-trace-attach-list` after frame 0.
void PrintAttachments(renderer::model::Actor& hero);

} // namespace whiteout::flakes::harness::draw_trace
