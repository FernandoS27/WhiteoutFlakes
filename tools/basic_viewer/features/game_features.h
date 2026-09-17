#pragma once

// ============================================================================
// The per-game features the viewer offers on top of a loaded model. Each is
// null when its format is compiled out, so a caller asks for the module —
// `if (auto* d3 = features.D3())` — instead of calling a stub that answers
// "no". game_features.cpp is the only C++ file in app/, documents/, features/
// and ui/ that tests a WDX_ENABLE_* flag.
// ============================================================================

#include "features/d3_wardrobe.h"
#include "features/sc2_animation_files.h"
#include "features/wow_appearance.h"

#include <memory>

namespace whiteout::flakes::io {
class LoadTaskRunner;
}
namespace whiteout::flakes::renderer {
class RenderService;
}

namespace whiteout::flakes {

class DocumentLoader;
class DocumentManager;
class PlaybackController;

class GameFeatures {
public:
    GameFeatures(renderer::RenderService& service, io::LoadTaskRunner& tasks, DocumentManager& documents,
                 PlaybackController& playback, DocumentLoader& loader);

    WowAppearance* Wow() const {
        return wow_.get();
    }
    D3Wardrobe* D3() const {
        return d3_.get();
    }
    Sc2AnimationFiles* Sc2() const {
        return sc2_.get();
    }

private:
    std::unique_ptr<WowAppearance> wow_;
    std::unique_ptr<D3Wardrobe> d3_;
    std::unique_ptr<Sc2AnimationFiles> sc2_;
};

} // namespace whiteout::flakes
