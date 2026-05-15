#pragma once

/// @file sound_emitter.h
/// @brief Pluggable sound-emitter interface driven by MDX SND events.

#include "event_data.h"
#include "types.h"

#include <memory>

namespace whiteout::flakes::renderer {

/// @brief Sink for MDX SND event playback.
///
/// Hosts implement this to integrate with their preferred audio backend
/// (the project ships a cubeb-backed spatial emitter; the default
/// installed by `RenderService` is `NullSoundEmitter`, which is silent).
/// Replace via `RenderService::SwapSoundEmitter()`.
class ISoundEmitter {
public:
    virtual ~ISoundEmitter() = default;

    /// @brief Play (or queue) a one-shot sound at @p worldPos.
    /// @param entry      Sound definition resolved by the engine
    ///                   (contains the asset path, attenuation, etc.).
    /// @param worldPos   World-space position of the SND event in
    ///                   renderer-native coordinates.
    virtual void Play(const io::SndEntry& entry, const Vector3f& worldPos) = 0;

    /// @brief Set master volume in 0..1.
    virtual void SetVolume(f32) {}
    /// @brief Read master volume in 0..1.
    virtual f32 GetVolume() const {
        return 1.0f;
    }

    /// @brief Update the listener pose used for 3D spatialisation.
    ///
    /// Hosts that want positional audio call this once per frame from
    /// their camera before the per-frame tick fires SND events.
    /// @p forward and @p up need not be normalised. Default is a no-op —
    /// only spatialising emitters (e.g. the cubeb backend) override it;
    /// `NullSoundEmitter` and 2D backends ignore it.
    virtual void SetListener(const Vector3f& /*pos*/, const Vector3f& /*forward*/,
                             const Vector3f& /*up*/) {}
};

/// @brief Silent sink installed by default.
///
/// Stores the volume so `RenderService::SwapSoundEmitter()` can carry
/// the persisted user setting across to a real backend; without
/// storing it the seeded default would be lost by the inherited no-op
/// `SetVolume`.
class NullSoundEmitter final : public ISoundEmitter {
public:
    void Play(const io::SndEntry&, const Vector3f&) override {}

    void SetVolume(f32 v) override {
        volume_ = v;
    }
    f32 GetVolume() const override {
        return volume_;
    }

private:
    f32 volume_ = 1.0f;
};

/// @brief Construct a fresh `NullSoundEmitter` (owning pointer).
inline std::unique_ptr<ISoundEmitter> MakeNullSoundEmitter() {
    return std::make_unique<NullSoundEmitter>();
}

} // namespace whiteout::flakes::renderer

namespace whiteout::flakes {
using ::whiteout::flakes::renderer::ISoundEmitter;
using ::whiteout::flakes::renderer::MakeNullSoundEmitter;
using ::whiteout::flakes::renderer::NullSoundEmitter;
} // namespace whiteout::flakes
