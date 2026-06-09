#pragma once

// Shared camera-framing helpers used by the model viewer and the model
// explorer: position an orbital camera to fit a freshly-loaded model or a
// standalone PopcornFX effect. Factored here so both tools frame identically.

#include "renderer/types.h" // u32

#include <string>

namespace whiteout::flakes::renderer {
class Camera;
class RenderService;
namespace model {
struct Actor;
}
} // namespace whiteout::flakes::renderer

namespace whiteout::flakes::tools {

// Index of the actor's "stand" sequence (first whose name contains "stand",
// case-insensitive). Falls back to 0 (the first sequence) when there is no
// stand, or -1 when the actor has no sequences at all.
int PickStandSequenceIndex(renderer::model::Actor* hero);

// True when the model has any Reforged HD material layer (a non-SD shader) —
// such models must render in RenderMode::HD; classic SD geometry mangles in the
// HD pipeline and vice-versa, so each thumbnail picks its mode from this.
bool IsHdModel(renderer::model::Actor* hero);

// Applies the model's first embedded camera preset (its portrait / cinematic
// camera) to `cam` as a fixed pose. Returns false when the model has no camera
// presets, so the caller can fall back to auto-framing.
bool ApplyModelCamera(renderer::Camera& cam, renderer::model::Actor* hero);

// Frames `cam` (set to orbital) on the model's animation-AABB: the union of its
// per-sequence extents (death/dissipate/birth excluded), falling back to geoset
// bind-pose / model extents, then a fixed distance. Three-quarter view.
void FrameCameraToModel(renderer::Camera& cam, renderer::model::Actor* hero);

// Frames `cam` on a SINGLE named sequence's extent (e.g. the stand pose) rather
// than the union — used so each explorer thumbnail is framed to the animation
// it actually plays. Falls back to geoset bind-pose / model extents when that
// sequence's extent is degenerate or the name isn't found.
void FrameCameraToModelSequence(renderer::Camera& cam, renderer::model::Actor* hero,
                                const std::string& sequenceName);

// Frames `cam` on a standalone effect's LIVE particle cloud, queried from the
// renderer. Returns false until the effect has spawned particles (caller
// retries next frame). The bounds query reads the renderer's ACTIVE scene, so
// the caller must make the effect's scene active before calling.
bool FrameCameraToEffect(renderer::RenderService& svc, renderer::Camera& cam, u32 actorId);

} // namespace whiteout::flakes::tools
