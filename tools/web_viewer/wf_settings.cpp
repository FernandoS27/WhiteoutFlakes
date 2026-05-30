// Render-mode / lighting / grid / background / shadow toggles.

#include "wf_web_internal.h"

#include "whiteout/flakes/enums.h"

#include <cstdint>

using wf_web::WfRenderer;

namespace {
inline uint8_t clamp8(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return static_cast<uint8_t>(v);
}
} // namespace

extern "C" {

// 0=SD, 1=HD. Bad values clamp to SD.
void wf_set_render_mode(WfRenderer* h, int mode) {
    if (!h) return;
    h->renderer.Settings().SetRenderMode(
        mode == 1 ? whiteout::flakes::RenderMode::HD
                  : whiteout::flakes::RenderMode::SD);
}

void wf_set_background(WfRenderer* h, int r, int g, int b) {
    if (!h) return;
    h->renderer.Settings().SetBackgroundColor(clamp8(r), clamp8(g), clamp8(b));
}

// 0=InGame, 1=Glue, 2=Dynamic.
void wf_set_lighting_mode(WfRenderer* h, int mode) {
    if (!h) return;
    using LM = whiteout::flakes::LightingMode;
    LM lm = (mode == 1) ? LM::Glue : (mode == 2) ? LM::Dynamic : LM::InGame;
    h->renderer.Settings().SetLightingMode(lm);
}

void wf_set_show_grid(WfRenderer* h, int on) {
    if (!h) return;
    auto df = h->renderer.Settings().GetDisplayFlags();
    df.showGrid = (on != 0);
    h->renderer.Settings().SetDisplayFlags(df);
}

// Off on Firefox by default — wgpu/naga's HD shadow sample is expensive.
void wf_set_shadows_enabled(WfRenderer* h, int on) {
    if (!h) return;
    h->renderer.Shadow().SetEnabled(on != 0);
}

} // extern "C"
