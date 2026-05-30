// Model spawn/clear + ActorView wrappers powering the JS Instance class.

#include "wf_web_internal.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>

using wf_web::WfRenderer;

namespace {
inline uint8_t clamp8(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return static_cast<uint8_t>(v);
}

inline int CopyToOutBuf(const std::string& s, char* outBuf, int bufCap) {
    const int n = static_cast<int>(std::min<std::size_t>(
        s.size(), static_cast<std::size_t>(bufCap - 1)));
    std::memcpy(outBuf, s.data(), n);
    outBuf[n] = '\0';
    return n;
}
} // namespace

extern "C" {

void wf_set_pe1_base(WfRenderer* h, const char* basePath) {
    if (!h || !basePath) return;
    h->renderer.Scene().SetPE1BasePath(std::filesystem::path(basePath));
}

uint32_t wf_spawn_unit(WfRenderer* h, const char* mdxPath) {
    if (!h || !mdxPath) return 0;
    // Child-model + PE1 templates load via FrameTicker's per-frame
    // GetOrLoadAsync; no separate prefetch needed here.
    return h->renderer.Loader().SpawnUnit(std::string(mdxPath));
}

void wf_clear_all(WfRenderer* h) {
    if (!h) return;
    h->renderer.Loader().RequestClearAll();
}

// Host calls on sequence change to avoid splat carryover.
void wf_clear_splats(WfRenderer* h) {
    if (!h) return;
    h->renderer.Splats().Clear();
}

void wf_actor_destroy(WfRenderer* h, uint32_t actor) {
    if (!h) return;
    h->renderer.Loader().Destroy(actor);
}

// Matrix44f wire-compatible with the JS-composed 16 floats.
void wf_actor_set_transform(WfRenderer* h, uint32_t actor, const float* m) {
    if (!h || !m) return;
    auto av = h->renderer.Actor(actor);
    if (!av.IsValid()) return;
    whiteout::flakes::Matrix44f mat;
    std::memcpy(&mat, m, sizeof(float) * 16);
    av.SetTransform(mat);
}

void wf_actor_set_sequence(WfRenderer* h, uint32_t actor, int seqIdx) {
    if (!h) return;
    auto av = h->renderer.Actor(actor);
    if (av.IsValid()) av.SetActiveSequence(seqIdx);
}

// 0=SD, 1=HD. JS forwards into wf_set_render_mode after spawn.
int wf_actor_preferred_render_mode(WfRenderer* h, uint32_t actor) {
    if (!h) return 0;
    auto av = h->renderer.Actor(actor);
    if (!av.IsValid()) return 0;
    return av.PreferredRenderMode() == whiteout::flakes::RenderMode::HD ? 1 : 0;
}

// mode 0 → ignoreNonLooping=true (clamp at last frame); 1/2 both loop.
void wf_actor_set_loop_mode(WfRenderer* h, uint32_t actor, int mode) {
    if (!h) return;
    auto av = h->renderer.Actor(actor);
    if (av.IsValid()) av.SetIgnoreNonLooping(mode == 0);
}

void wf_actor_set_team_color(WfRenderer* h, uint32_t actor, int r, int g, int b) {
    if (!h) return;
    auto av = h->renderer.Actor(actor);
    if (!av.IsValid()) return;
    av.SetTeamColor(clamp8(r), clamp8(g), clamp8(b));
}

void wf_actor_set_playback_speed(WfRenderer* h, uint32_t actor, float speed) {
    if (!h) return;
    auto av = h->renderer.Actor(actor);
    if (av.IsValid()) av.SetPlaybackSpeed(speed);
}

void wf_actor_set_anim_time(WfRenderer* h, uint32_t actor, int ms) {
    if (!h) return;
    auto av = h->renderer.Actor(actor);
    if (av.IsValid()) av.SetAnimationTimeMs(ms);
}

int wf_actor_get_sequence_count(WfRenderer* h, uint32_t actor) {
    if (!h) return 0;
    auto av = h->renderer.Actor(actor);
    if (!av.IsValid()) return 0;
    return static_cast<int>(av.Sequences().size());
}

// Copies name into JS buffer; returns bytes written (excl. NUL).
int wf_actor_get_sequence_name(WfRenderer* h, uint32_t actor, int idx,
                               char* outBuf, int bufCap) {
    if (!h || !outBuf || bufCap <= 0) return 0;
    auto av = h->renderer.Actor(actor);
    if (!av.IsValid()) return 0;
    const auto seqs = av.Sequences();
    if (idx < 0 || idx >= static_cast<int>(seqs.size())) return 0;
    return CopyToOutBuf(seqs[idx].name, outBuf, bufCap);
}

// Per-actor camera presets (Portrait_Camera, Cinematic_Camera, …).
int wf_actor_camera_preset_count(WfRenderer* h, uint32_t actor) {
    if (!h) return 0;
    auto av = h->renderer.Actor(actor);
    if (!av.IsValid()) return 0;
    return static_cast<int>(av.CameraPresets().size());
}

int wf_actor_camera_preset_name(WfRenderer* h, uint32_t actor, int idx,
                                char* outBuf, int bufCap) {
    if (!h || !outBuf || bufCap <= 0) return 0;
    auto av = h->renderer.Actor(actor);
    if (!av.IsValid()) return 0;
    const auto presets = av.CameraPresets();
    if (idx < 0 || idx >= static_cast<int>(presets.size())) return 0;
    return CopyToOutBuf(presets[idx].name, outBuf, bufCap);
}

} // extern "C"
