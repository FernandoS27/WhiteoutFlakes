#pragma once

// ============================================================================
// WhiteoutFlakes — pluggable sound emitter.
//
// Hosts implement ISoundEmitter to drive spatial audio playback for MDX SND
// events. The default backend is NullSoundEmitter (silent); applications
// install their own via Renderer::SwapSoundEmitter().
// ============================================================================

#include "event_data.h"
#include "types.h"

#include <memory>

namespace whiteout::flakes::renderer {

class ISoundEmitter {
public:
    virtual ~ISoundEmitter() = default;

    virtual void Play(const io::SndEntry& entry, const Vector3f& worldPos) = 0;

    virtual void SetVolume(f32) {}
    virtual f32 GetVolume() const {
        return 1.0f;
    }

    // Update the listener pose used for 3D spatialisation. Hosts that want
    // positional audio call this once per frame from their camera before
    // the per-frame tick fires SND events. `forward` / `up` need not be
    // normalised. Default no-op — only spatialising emitters (e.g.
    // CubebSoundEmitter) override it; NullSoundEmitter and 2D backends
    // ignore it.
    virtual void SetListener(const Vector3f& /*pos*/, const Vector3f& /*forward*/,
                             const Vector3f& /*up*/) {}
};

class NullSoundEmitter final : public ISoundEmitter {
public:
    void Play(const io::SndEntry&, const Vector3f&) override {}
};

inline std::unique_ptr<ISoundEmitter> MakeNullSoundEmitter() {
    return std::make_unique<NullSoundEmitter>();
}

} // namespace whiteout::flakes::renderer

namespace whiteout::flakes {
using ::whiteout::flakes::renderer::ISoundEmitter;
using ::whiteout::flakes::renderer::MakeNullSoundEmitter;
using ::whiteout::flakes::renderer::NullSoundEmitter;
} // namespace whiteout::flakes
