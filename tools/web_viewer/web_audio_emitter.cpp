// WebAudioSoundEmitter — see web_audio_emitter.h.
//
// The EM_JS bridges are C-linkage globals; they emit a snippet of JS that
// dispatches to a user-installed `wfWebAudio*JS` global. JS-side, the
// HiveApp's WebAudioBridge binds those globals at startup so the
// dispatch lands on its AudioContext / PannerNode graph.

#include "web_audio_emitter.h"

#include <cstdlib>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

EM_JS(void, wfWebAudioPlay,
      (const char* path, float volume, float x, float y, float z,
       float minDist, float maxDist, float cutoff),
      {
          if (typeof globalThis.wfWebAudioPlayJS === 'function') {
              globalThis.wfWebAudioPlayJS(UTF8ToString(path), volume, x, y, z,
                                          minDist, maxDist, cutoff);
          }
      });

EM_JS(void, wfWebAudioSetListener,
      (float px, float py, float pz, float fx, float fy, float fz,
       float ux, float uy, float uz),
      {
          if (typeof globalThis.wfWebAudioSetListenerJS === 'function') {
              globalThis.wfWebAudioSetListenerJS(px, py, pz, fx, fy, fz, ux, uy, uz);
          }
      });

EM_JS(void, wfWebAudioSetVolume, (float v), {
    if (typeof globalThis.wfWebAudioSetVolumeJS === 'function') {
        globalThis.wfWebAudioSetVolumeJS(v);
    }
});
#else
// Native stubs — this emitter is wired in only under the Emscripten
// build, but keeping no-op symbols here lets the file compile cleanly
// for any host that wants to link it for tests / dry runs.
static void wfWebAudioPlay(const char*, float, float, float, float, float, float, float) {}
static void wfWebAudioSetListener(float, float, float, float, float, float, float, float, float) {}
static void wfWebAudioSetVolume(float) {}
#endif

namespace whiteout::flakes::web {

void WebAudioSoundEmitter::Play(const io::SndEntry& entry, const Vector3f& worldPos) {
    if (entry.filePaths.empty()) return;
    // Same per-fire random pick the desktop CubebSoundEmitter does;
    // matches WC3's "What1/What2/What3" voice variant convention.
    const auto& path = entry.filePaths[std::rand() % entry.filePaths.size()];
    wfWebAudioPlay(path.c_str(), entry.volume * volume_, worldPos.x, worldPos.y, worldPos.z,
                   entry.minDistance, entry.maxDistance, entry.distanceCutoff);
}

void WebAudioSoundEmitter::SetVolume(f32 v) {
    volume_ = v;
    wfWebAudioSetVolume(v);
}

f32 WebAudioSoundEmitter::GetVolume() const {
    return volume_;
}

void WebAudioSoundEmitter::SetListener(const Vector3f& pos, const Vector3f& fwd,
                                       const Vector3f& up) {
    wfWebAudioSetListener(pos.x, pos.y, pos.z, fwd.x, fwd.y, fwd.z, up.x, up.y, up.z);
}

} // namespace whiteout::flakes::web
