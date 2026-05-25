#pragma once

// WebAudioSoundEmitter — ISoundEmitter that marshals every SND event to a
// JS layer via EM_JS bridges. The JS layer (`web_audio.js`) owns the
// AudioContext, PannerNode graph, asset fetch/decode, and autoplay-
// gesture gating. The C++ side stays trivial: pick the random file from
// the SndEntry, hand it off through `wfWebAudioPlay`, return.

#include "whiteout/flakes/sound_emitter.h"
#include "whiteout/flakes/types.h"

namespace whiteout::flakes::web {

class WebAudioSoundEmitter final : public ISoundEmitter {
public:
    void Play(const io::SndEntry& entry, const Vector3f& worldPos) override;
    void SetVolume(f32 v) override;
    f32 GetVolume() const override;
    void SetListener(const Vector3f& pos, const Vector3f& forward,
                     const Vector3f& up) override;

private:
    f32 volume_ = 1.0f;
};

} // namespace whiteout::flakes::web
