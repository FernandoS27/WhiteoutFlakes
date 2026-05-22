#pragma once

/// @file renderer.h
/// @brief Public façade for `WhiteoutFlakesLib`.
///
/// `Renderer` is the single owning entry point: it constructs and holds
/// the scene, pipeline, ticker, loader, settings, and optional services
/// (DNC, shadow, …) internally. Consumer-facing operations go through
/// the sub-service views returned from this class, and per-actor
/// mutations through @ref ActorView.
///
/// @par Thread-safety
/// All accessors return small value-typed views that share the same
/// internal `Impl` pointer. The internal renderer is not engineered for
/// concurrent mutation; serialise calls on a single thread.

#include "actor_view.h"
#include "content_provider.h"
#include "display.h"
#include "enums.h"
#include "frame_state.h"
#include "model_data.h"
#include "model_source.h"
#include "sound_emitter.h"
#include "types.h"
#include "views.h"

#include <memory>
#include <string_view>

namespace whiteout::flakes {

/// @brief Top-level WhiteoutFlakes renderer.
///
/// Construct one per process / per host. Holds every internal subsystem
/// behind a stable PIMPL pointer; copies are disallowed.
class Renderer {
public:
    Renderer();
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    /// @name Sub-service handles
    /// Each call returns a small value-typed view; the underlying impl
    /// is shared with the owning `Renderer`. Views are cheap to copy.
    /// @{
    PipelineView Pipeline();
    SceneView Scene();
    CameraView Camera();
    SettingsView Settings();
    LoaderView Loader();
    DncView Dnc();
    ShadowView Shadow();
    SplatView Splats();
    ReplaceablesView Replaceables();
    /// @}

    /// @brief Per-actor view. Returns an invalid view
    ///        (`IsValid() == false`) for unknown handles.
    ActorView Actor(ActorHandle h);

    /// @brief Replace the sound emitter.
    ///
    /// The default installed at construction is `NullSoundEmitter`.
    /// Hosts that want audio install a cubeb / Win32 / CoreAudio
    /// implementation here. The previous emitter's volume is carried
    /// over to the new one.
    void SwapSoundEmitter(std::unique_ptr<ISoundEmitter>);

    /// @brief Per-frame entry point.
    ///
    /// Drives animation, attachment-graph propagation, particle / PE1 /
    /// ribbon / corn-effects simulation, splat decay, DNC advancement.
    /// Hosts that drive their own actor evaluation (e.g. the Max plugin,
    /// which receives time scrubs from the host timeline) can skip
    /// `Tick` and call @ref ActorView::EvaluateAndApply manually.
    /// @param dt Elapsed time since the last tick, in seconds.
    void Tick(f32 dt);

    /// @brief Texture-cache probe.
    ///
    /// Used by adapters that want to dedupe shared textures by their
    /// content-addressed cache key (model imports often reference the
    /// same BLP multiple times).
    bool IsTextureCached(std::string_view sharedKey) const;

private:
    std::unique_ptr<detail::RendererImpl> impl_;
};

} // namespace whiteout::flakes
