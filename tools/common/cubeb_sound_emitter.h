#pragma once

// ============================================================================
// CubebSoundEmitter — cross-platform ISoundEmitter built on Mozilla cubeb.
//
// Replaces the old Win32 PlaySoundW backend. One persistent output stream
// (stereo float32 at the device's preferred rate); every MDX SND event is
// decoded (FLAC / WAV / MP3 via dr_libs), downmixed to stereo, resampled to
// the stream rate, and handed to the audio callback as a Voice. The callback
// mixes all active voices — overlapping SND events layer instead of cutting
// each other off.
//
// Shared by the standalone viewer and the 3ds Max plugin.
// ============================================================================

#include "whiteout/flakes/sound_emitter.h"
#include "whiteout/flakes/types.h"

#include <atomic>
#include <mutex>
#include <vector>

// cubeb's context / stream handles are opaque — forward-declare so this
// header stays free of <cubeb/cubeb.h>.
struct cubeb;
struct cubeb_stream;

namespace whiteout::flakes::io {
class IContentProvider;
}

namespace whiteout::flakes {

using namespace whiteout::flakes::io;
using namespace whiteout::flakes::renderer;

class CubebSoundEmitter : public ISoundEmitter {
public:
    explicit CubebSoundEmitter(IContentProvider* content);
    ~CubebSoundEmitter() override;

    CubebSoundEmitter(const CubebSoundEmitter&) = delete;
    CubebSoundEmitter& operator=(const CubebSoundEmitter&) = delete;

    void Play(const io::SndEntry& entry, const Vector3f& worldPos) override;

    void SetVolume(f32 v) override;
    f32 GetVolume() const override;

    void SetListener(const Vector3f& pos, const Vector3f& forward, const Vector3f& up) override;

    // Mixes active voices into `out` (interleaved stereo f32, `frames`
    // frames). Public only so the file-local cubeb data callback can reach
    // it; not part of ISoundEmitter.
    void MixVoices(f32* out, i64 frames);

    // One decoded + resampled sound, mid-playback. `samples` is interleaved
    // stereo at streamRate_; `cursor` is a frame index. The spatial fields
    // are captured at Play() time; the per-channel gains they drive are
    // recomputed against the live listener pose every mix buffer. Public so
    // the file-local ComputeVoiceGains helper can take one — impl-detail in
    // practice (the type isn't exposed by any public method signature).
    struct Voice {
        std::vector<f32> samples;
        usize cursor = 0;

        // Spatialisation source params captured at Play(). The SndEntry's
        // SLK MinDistance / MaxDistance / DistanceCutoff are deliberately
        // *not* kept — they're in WC3 game-map units, unrelated to the MDX
        // model units the previewer renders in, so the mixer uses a fixed
        // preview-space reference distance instead.
        Vector3f worldPos{};
        f32 entryVolume = 1.0f;

        // Smoothed per-channel gains, ramped across each buffer so camera
        // movement pans the voice without zipper noise. `gainPrimed` snaps
        // the first buffer to the target instead of ramping up from zero.
        f32 curGainL = 0.0f;
        f32 curGainR = 0.0f;
        bool gainPrimed = false;
    };

private:
    // Cap on simultaneously-mixing voices. A debug preview never needs many;
    // the cap keeps the mix loop bounded if SND events fire in a tight burst.
    static constexpr usize kMaxVoices = 24;

    IContentProvider* content_ = nullptr;

    cubeb* ctx_ = nullptr;
    cubeb_stream* stream_ = nullptr;
    u32 streamRate_ = 48000;

    std::mutex mu_; // guards voices_ + the listener pose below
    std::vector<Voice> voices_;

    // Listener pose for 3D panning. listenerRight_ is the normalised right
    // axis (forward × up); a voice's worldPos projected onto it gives the
    // L/R pan. haveListener_ stays false until the host calls SetListener,
    // in which case every voice plays as flat 2D.
    Vector3f listenerPos_{};
    Vector3f listenerRight_{1.0f, 0.0f, 0.0f};
    bool haveListener_ = false;

    std::atomic<f32> volume_{1.0f};
};

} // namespace whiteout::flakes
