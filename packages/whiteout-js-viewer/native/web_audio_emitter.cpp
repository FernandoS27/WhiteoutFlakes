// See web_audio_emitter.h. EM_JS bridges call user-installed
// `wfWebAudio*JS` globals bound by WebAudioBridge on startup.

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
// Native stubs — lets the file compile cleanly outside Emscripten.
static void wfWebAudioPlay(const char*, float, float, float, float, float, float, float) {}
static void wfWebAudioSetListener(float, float, float, float, float, float, float, float, float) {}
static void wfWebAudioSetVolume(float) {}
#endif

namespace whiteout::flakes::web {

void WebAudioSoundEmitter::Play(const io::SndEntry& entry, const Vector3f& worldPos) {
    if (entry.filePaths.empty()) return;
    // Random variant pick (WC3's What1/What2/What3 convention).
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
