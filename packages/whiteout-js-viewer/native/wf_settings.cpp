// Render-mode / lighting / grid / background / shadow / time-of-day toggles.

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

// HD debug visualisation mode. 0=Off, 1=Albedo, 2=WorldNormal,
// 3=LodHeatmap, 4=LightCount, 5=ShadingWhite, 6=ShadingGrey,
// 7=SpecularOnly, 8=NoOrm. Matches basic_viewer's kDebugVisLabels.
void wf_set_hd_debug_mode(WfRenderer* h, int mode) {
    if (!h) return;
    h->renderer.Settings().SetHdDebugMode(mode);
}

// Day-night-cycle time of day, in hours [0..24). DncService wraps out-of-range
// values. The web UI drives the 60-second animated cycle by pushing a fresh
// value each frame (see wf-viewer.js); the slider pushes a manual one.
void wf_set_time_of_day(WfRenderer* h, float hours) {
    if (!h) return;
    h->renderer.Dnc().SetTimeOfDay(hours);
}

// IBL probe set. 0=Portrait, 1=DayNight, 2=Dungeon, 3=Sunset (enums.h::IblMode).
// The web UI flips Portrait↔DayNight with the day/night toggle so HD env
// lighting/reflections cycle with TOD; the pipeline reloads the probes lazily
// on the next frame (ConsumeIblModeDirty), reading the prefetched DDS bytes.
void wf_set_ibl_mode(WfRenderer* h, int mode) {
    if (!h) return;
    using whiteout::flakes::IblMode;
    const IblMode m = (mode == 1)   ? IblMode::DayNight
                      : (mode == 2) ? IblMode::Dungeon
                      : (mode == 3) ? IblMode::Sunset
                                    : IblMode::Portrait;
    h->renderer.Settings().SetIblMode(m);
}

} // extern "C"
