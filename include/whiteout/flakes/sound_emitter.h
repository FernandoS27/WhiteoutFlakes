#pragma once

// ============================================================================
// WhiteoutFlakes — pluggable sound emitter.
//
// Hosts implement ISoundEmitter to drive spatial audio playback for MDX SND
// events. The default backend is NullSoundEmitter (silent); applications
// install their own via Renderer::SwapSoundEmitter().
// ============================================================================

#include "types.h"
#include "event_data.h"

#include <memory>

namespace whiteout::flakes::renderer {

class ISoundEmitter {
public:
    virtual ~ISoundEmitter() = default;

    virtual void Play(const io::SndEntry& entry, const Vector3f& worldPos) = 0;

    virtual void SetVolume(f32) {}
    virtual f32  GetVolume() const { return 1.0f; }
};

class NullSoundEmitter final : public ISoundEmitter {
public:
    void Play(const io::SndEntry&, const Vector3f&) override {}
};

inline std::unique_ptr<ISoundEmitter> MakeNullSoundEmitter() {
    return std::make_unique<NullSoundEmitter>();
}

}  // namespace whiteout::flakes::renderer

namespace whiteout::flakes {
using ::whiteout::flakes::renderer::ISoundEmitter;
using ::whiteout::flakes::renderer::NullSoundEmitter;
using ::whiteout::flakes::renderer::MakeNullSoundEmitter;
}
