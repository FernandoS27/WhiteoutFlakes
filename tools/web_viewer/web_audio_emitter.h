#pragma once

// ISoundEmitter that marshals SND events to web_audio.js via EM_JS.
// JS side owns the AudioContext, fetch/decode, and gesture gating.

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
