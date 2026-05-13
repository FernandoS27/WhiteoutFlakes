#pragma once

// ============================================================================
// WhiteoutFlakes — public Renderer facade.
//
// The single owning entry point to WhiteoutFlakesLib. Constructs and holds
// the scene, pipeline, ticker, loader, settings, and optional services
// (DNC, shadow, etc.) internally. All consumer-facing operations go through
// the sub-service views returned from this class or per-actor mutations
// through ActorView.
//
// Thread-safety: all accessors return views that share the same Impl
// pointer. The internal renderer was not engineered for concurrent
// mutation, so callers should serialize access on a single thread.
// ============================================================================

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

class Renderer {
public:
    Renderer();
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Sub-service handles. Each call creates a small value-typed view; the
    // underlying impl is shared.
    PipelineView Pipeline();
    SceneView Scene();
    CameraView Camera();
    SettingsView Settings();
    LoaderView Loader();
    DebugView Debug();
    DncView Dnc();
    ShadowView Shadow();
    SplatView Splats();
    ReplaceablesView Replaceables();

    // Per-actor view. Returns an invalid view (IsValid()==false) for unknown
    // handles.
    ActorView Actor(ActorHandle h);

    // Sound is consumer-pluggable: pass in any ISoundEmitter implementation
    // (NullSoundEmitter is the default).
    void SwapSoundEmitter(std::unique_ptr<ISoundEmitter>);

    // Per-frame entry point. Drives animation + attachment + particle / PE1
    // / ribbon simulation. Hosts that drive their own actor evaluation
    // (Max plugin) can skip Tick and call ActorView::EvaluateAndApply
    // manually.
    void Tick(f32 dt);

    // Texture cache probe (used by adapters that want to dedupe shared
    // textures by their cache key).
    bool IsTextureCached(std::string_view sharedKey) const;

private:
    std::unique_ptr<detail::RendererImpl> impl_;
};

} // namespace whiteout::flakes
