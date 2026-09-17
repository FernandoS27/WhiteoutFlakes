#include "features/game_features.h"

namespace whiteout::flakes {

// A compiled-out feature has no implementation file in the build (CMake lists
// each under its flag), so its factory is defined here instead and makes nothing.
#if !WDX_ENABLE_M2
std::unique_ptr<WowAppearance> MakeWowAppearance(renderer::RenderService&, DocumentManager&, DocumentLoader&) {
    return nullptr;
}
#endif
#if !WDX_ENABLE_D3
std::unique_ptr<D3Wardrobe> MakeD3Wardrobe(renderer::RenderService&, io::LoadTaskRunner&, DocumentManager&,
                                           DocumentLoader&) {
    return nullptr;
}
#endif
#if !WDX_ENABLE_M3
std::unique_ptr<Sc2AnimationFiles> MakeSc2AnimationFiles(renderer::RenderService&, DocumentManager&,
                                                         PlaybackController&) {
    return nullptr;
}
#endif

GameFeatures::GameFeatures(renderer::RenderService& service, io::LoadTaskRunner& tasks, DocumentManager& documents,
                           PlaybackController& playback, DocumentLoader& loader)
    : wow_(MakeWowAppearance(service, documents, loader)),
      d3_(MakeD3Wardrobe(service, tasks, documents, loader)),
      sc2_(MakeSc2AnimationFiles(service, documents, playback)) {}

} // namespace whiteout::flakes
