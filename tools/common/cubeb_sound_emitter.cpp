#include "cubeb_sound_emitter.h"

#include "whiteout/flakes/content_provider.h"
#include "whiteout/flakes/event_data.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <numbers>
#include <optional>
#include <random>
#include <string>

#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_OGG
#include "dr_flac.h"
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"
#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"

#include <cubeb/cubeb.h>

namespace whiteout::flakes {

using namespace whiteout::flakes::io;
using namespace whiteout::flakes::renderer;

namespace {

// ---- Sound file resolution -------------------------------------------------
// Probes the SndEntry's source path(s) against the content provider, trying
// extension swaps (.wav / .flac) and trailing-digit stem trims — the same
// candidate scheme the old Win32 backend used. Returns the raw file bytes of
// the first hit; decoding happens separately so every format goes through one
// path.
std::optional<std::vector<u8>> ResolveSoundBytes(IContentProvider& cp,
                                                 const io::SndEntry& entry,
                                                 std::string* attemptedOut) {
    if (entry.filePaths.empty())
        return std::nullopt;

    struct PathParts {
        std::string dir;
        std::string stem;
        std::string extLow;
        bool hasExt;
    };
    auto split = [](const std::string& path) -> PathParts {
        const auto sepPos = path.find_last_of("/\\");
        const auto dotPos = path.rfind('.');
        const bool hasExt =
            dotPos != std::string::npos && (sepPos == std::string::npos || dotPos > sepPos);
        const usize baseStart = (sepPos == std::string::npos) ? 0 : sepPos + 1;
        PathParts p;
        p.dir = path.substr(0, baseStart);
        p.stem = hasExt ? path.substr(baseStart, dotPos - baseStart) : path.substr(baseStart);
        p.extLow = hasExt ? path.substr(dotPos) : std::string{};
        for (auto& c : p.extLow)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        p.hasExt = hasExt;
        return p;
    };

    // WC3 unit-sound SLKs list paths relative to the `Sound\` directory
    // (e.g. "Units\Human\Gyrocopter\GyrocopterDeath1"), so the file actually
    // lives at "Sound\Units\...". The content provider's CASC/MPQ lookup
    // doesn't add the prefix — its alt-extension logic only covers textures
    // and models — so we probe each form both as-is and under `Sound/`.
    auto hasSoundPrefix = [](const std::string& s) {
        if (s.size() < 7 || (s[5] != '/' && s[5] != '\\'))
            return false;
        const char* k = "sound";
        for (i32 i = 0; i < 5; ++i)
            if (std::tolower(static_cast<unsigned char>(s[i])) != k[i])
                return false;
        return true;
    };

    auto candidates = [&](const std::string& path) -> std::vector<std::string> {
        const PathParts p = split(path);

        std::vector<std::string> stems{p.stem};
        usize end = p.stem.size();
        while (end > 0 && p.stem[end - 1] >= '0' && p.stem[end - 1] <= '9')
            --end;
        if (end > 0 && end < p.stem.size())
            stems.push_back(p.stem.substr(0, end));

        // Base path forms: the original (when it carried an extension) plus
        // every stem x {.wav,.flac,.mp3}.
        std::vector<std::string> base;
        auto pushBase = [&](std::string cand) {
            if (std::find(base.begin(), base.end(), cand) == base.end())
                base.push_back(std::move(cand));
        };
        if (p.hasExt)
            pushBase(path);
        for (const auto& s : stems) {
            pushBase(p.dir + s + ".wav");
            pushBase(p.dir + s + ".flac");
            pushBase(p.dir + s + ".mp3");
        }

        // Each base form is tried as-is, then under a `Sound/` prefix unless
        // it already carries one (asset-map hits sometimes do).
        std::vector<std::string> out;
        auto push = [&](std::string cand) {
            if (std::find(out.begin(), out.end(), cand) == out.end())
                out.push_back(std::move(cand));
        };
        for (const auto& b : base) {
            push(b);
            if (!hasSoundPrefix(b))
                push("Sound/" + b);
        }
        return out;
    };

    static thread_local std::mt19937 rng{std::random_device{}()};
    const usize n = entry.filePaths.size();
    const usize start = (n == 1) ? 0 : (rng() % n);
    for (usize i = 0; i < n; ++i) {
        for (const auto& path : candidates(entry.filePaths[(start + i) % n])) {
            if (auto bytes = cp.ReadFile(path, nullptr))
                return bytes;
            if (attemptedOut) {
                if (!attemptedOut->empty())
                    attemptedOut->append(", ");
                attemptedOut->append(path);
            }
        }
    }
    return std::nullopt;
}

// ---- Decode ----------------------------------------------------------------
struct DecodedPcm {
    std::vector<f32> interleaved; // src channel count, interleaved
    u32 channels = 0;
    u32 sampleRate = 0;
};

bool LooksLikeFlac(const std::vector<u8>& b) {
    return b.size() >= 4 && b[0] == 'f' && b[1] == 'L' && b[2] == 'a' && b[3] == 'C';
}
bool LooksLikeWav(const std::vector<u8>& b) {
    return b.size() >= 12 && std::memcmp(b.data(), "RIFF", 4) == 0 &&
           std::memcmp(b.data() + 8, "WAVE", 4) == 0;
}
bool LooksLikeMp3(const std::vector<u8>& b) {
    // ID3 tag or an MPEG-audio frame sync.
    if (b.size() >= 3 && b[0] == 'I' && b[1] == 'D' && b[2] == '3')
        return true;
    return b.size() >= 2 && b[0] == 0xFF && (b[1] & 0xE0) == 0xE0;
}

std::optional<DecodedPcm> DecodeFlac(const std::vector<u8>& bytes) {
    u32 channels = 0, sampleRate = 0;
    drflac_uint64 frames = 0;
    f32* s = drflac_open_memory_and_read_pcm_frames_f32(bytes.data(), bytes.size(), &channels,
                                                        &sampleRate, &frames, nullptr);
    if (!s)
        return std::nullopt;
    DecodedPcm out;
    out.channels = channels;
    out.sampleRate = sampleRate;
    out.interleaved.assign(s, s + frames * channels);
    drflac_free(s, nullptr);
    if (out.channels == 0 || out.sampleRate == 0 || out.interleaved.empty())
        return std::nullopt;
    return out;
}

std::optional<DecodedPcm> DecodeWav(const std::vector<u8>& bytes) {
    u32 channels = 0, sampleRate = 0;
    drwav_uint64 frames = 0;
    f32* s = drwav_open_memory_and_read_pcm_frames_f32(bytes.data(), bytes.size(), &channels,
                                                       &sampleRate, &frames, nullptr);
    if (!s)
        return std::nullopt;
    DecodedPcm out;
    out.channels = channels;
    out.sampleRate = sampleRate;
    out.interleaved.assign(s, s + frames * channels);
    drwav_free(s, nullptr);
    if (out.channels == 0 || out.sampleRate == 0 || out.interleaved.empty())
        return std::nullopt;
    return out;
}

std::optional<DecodedPcm> DecodeMp3(const std::vector<u8>& bytes) {
    drmp3_config cfg{};
    drmp3_uint64 frames = 0;
    f32* s = drmp3_open_memory_and_read_pcm_frames_f32(bytes.data(), bytes.size(), &cfg, &frames,
                                                       nullptr);
    if (!s)
        return std::nullopt;
    DecodedPcm out;
    out.channels = cfg.channels;
    out.sampleRate = cfg.sampleRate;
    out.interleaved.assign(s, s + frames * cfg.channels);
    drmp3_free(s, nullptr);
    if (out.channels == 0 || out.sampleRate == 0 || out.interleaved.empty())
        return std::nullopt;
    return out;
}

std::optional<DecodedPcm> DecodePcm(const std::vector<u8>& bytes) {
    if (bytes.empty())
        return std::nullopt;
    // Prefer the magic-number match; fall back to trying each decoder so a
    // mislabelled file still plays.
    if (LooksLikeFlac(bytes)) {
        if (auto d = DecodeFlac(bytes))
            return d;
    } else if (LooksLikeWav(bytes)) {
        if (auto d = DecodeWav(bytes))
            return d;
    } else if (LooksLikeMp3(bytes)) {
        if (auto d = DecodeMp3(bytes))
            return d;
    }
    if (auto d = DecodeWav(bytes))
        return d;
    if (auto d = DecodeFlac(bytes))
        return d;
    if (auto d = DecodeMp3(bytes))
        return d;
    return std::nullopt;
}

// ---- Channel fold + resample ----------------------------------------------
// Folds an N-channel interleaved buffer down to interleaved stereo: mono is
// duplicated to both sides, stereo passes through, >2 channels keep the first
// two (front L/R) — good enough for WC3 SFX preview.
std::vector<f32> FoldToStereo(const DecodedPcm& src) {
    const usize frames = src.interleaved.size() / src.channels;
    std::vector<f32> out(frames * 2);
    if (src.channels == 1) {
        for (usize i = 0; i < frames; ++i) {
            const f32 m = src.interleaved[i];
            out[i * 2 + 0] = m;
            out[i * 2 + 1] = m;
        }
    } else {
        for (usize i = 0; i < frames; ++i) {
            out[i * 2 + 0] = src.interleaved[i * src.channels + 0];
            out[i * 2 + 1] = src.interleaved[i * src.channels + 1];
        }
    }
    return out;
}

// Linear-interpolation resampler, interleaved stereo in → interleaved stereo
// out. Linear is plenty for preview-quality SFX and keeps the emitter free of
// any resampler dependency.
std::vector<f32> ResampleStereo(const std::vector<f32>& src, u32 srcRate, u32 dstRate) {
    if (srcRate == dstRate || src.size() < 4)
        return src;
    const usize srcFrames = src.size() / 2;
    const f64 step = static_cast<f64>(srcRate) / static_cast<f64>(dstRate);
    const usize dstFrames = static_cast<usize>(static_cast<f64>(srcFrames) / step);
    std::vector<f32> dst(dstFrames * 2);
    for (usize i = 0; i < dstFrames; ++i) {
        const f64 pos = static_cast<f64>(i) * step;
        const usize i0 = static_cast<usize>(pos);
        const usize i1 = std::min(i0 + 1, srcFrames - 1);
        const f32 frac = static_cast<f32>(pos - static_cast<f64>(i0));
        for (i32 c = 0; c < 2; ++c) {
            const f32 a = src[i0 * 2 + c];
            const f32 b = src[i1 * 2 + c];
            dst[i * 2 + c] = a + (b - a) * frac;
        }
    }
    return dst;
}

// ---- 3D spatialisation -----------------------------------------------------
// Computes a voice's target per-channel gains from the live listener pose:
// clamped inverse-distance attenuation, then a constant-power stereo pan from
// the emitter's projection onto the listener's right axis.
//
// kRefDist is the full-volume radius in *previewer* units (MDX model space).
// The SndEntry's SLK MinDistance / MaxDistance live in WC3 game-map units —
// a completely different scale — so feeding them into the curve produced a
// "full, then abruptly silent" band. A fixed reference plus a pure 1/r tail
// (no hard far-cutoff) gives a falloff that actually reads as fading: the
// sound just keeps getting quieter the further the camera pulls away.
constexpr f32 kRefDist = 300.0f;

// Per-voice base-level headroom. SLK Volume values cluster near 1.0, and at
// close range (within kRefDist) attenuation is already 1.0 — so without this
// a nearby unit's SFX hit the mix at near-unity and felt overbearing once 3D
// panning was added. ~-9 dB pulls every voice down to a comfortable preview
// level; the Settings sound-volume slider still scales on top.
constexpr f32 kBaseGain = 0.35f;

// The Settings sound-volume slider stores a raw 0..1 value (persisted to the
// INI, shown on the slider), but the master gain applied in the mix is
// slider^kVolumeCurveExp. The exponent is tuned so the comfortable level
// sits at mid-travel: solving 0.5^k = 0.04 (the effective gain that felt
// right when the slider sat near 0.25 under the previous, gentler curve)
// gives k = ln(0.04)/ln(0.5) ≈ 4.6438. 1.0 still maps to full volume.
constexpr f32 kVolumeCurveExp = 4.6438f;

void ComputeVoiceGains(const CubebSoundEmitter::Voice& v, const Vector3f& lpos,
                       const Vector3f& lright, bool haveListener, f32& outL, f32& outR) {
    if (!haveListener) {
        outL = v.entryVolume * kBaseGain;
        outR = v.entryVolume * kBaseGain;
        return;
    }

    const f32 dx = v.worldPos.x - lpos.x;
    const f32 dy = v.worldPos.y - lpos.y;
    const f32 dz = v.worldPos.z - lpos.z;
    const f32 dist = std::sqrt(dx * dx + dy * dy + dz * dz);

    // Clamped inverse-distance: 1.0 within kRefDist, ~1/r beyond, asymptotic
    // to zero but never a hard cliff.
    const f32 atten = kRefDist / std::max(dist, kRefDist);

    // Constant-power pan: -1 (full left) .. +1 (full right) mapped onto a
    // [0, pi/2] quarter-circle so L^2 + R^2 stays constant across the pan.
    f32 panNorm = 0.0f;
    if (dist > 1e-4f) {
        const f32 inv = 1.0f / dist;
        panNorm = std::clamp((dx * lright.x + dy * lright.y + dz * lright.z) * inv, -1.0f, 1.0f);
    }
    const f32 angle = (panNorm * 0.5f + 0.5f) * (std::numbers::pi_v<f32> * 0.5f);
    const f32 g = atten * v.entryVolume * kBaseGain;
    outL = g * std::cos(angle);
    outR = g * std::sin(angle);
}

// ---- cubeb callbacks -------------------------------------------------------
long DataCb(cubeb_stream* /*stream*/, void* user, const void* /*input*/, void* output,
            long nframes) {
    auto* self = static_cast<CubebSoundEmitter*>(user);
    self->MixVoices(static_cast<f32*>(output), nframes);
    return nframes; // always-on stream — never drains
}

void StateCb(cubeb_stream* /*stream*/, void* /*user*/, cubeb_state /*state*/) {
    // Persistent stream: nothing to do on started/stopped/drained/error.
    // An error here just means the device went away; voices keep queueing
    // harmlessly and the next stream re-init (none today) would recover.
}

} // namespace

CubebSoundEmitter::CubebSoundEmitter(IContentProvider* content) : content_(content) {
    if (cubeb_init(&ctx_, "WhiteoutFlakes", nullptr) != CUBEB_OK || !ctx_) {
        std::fprintf(stderr, "[cubeb] cubeb_init failed — audio disabled\n");
        ctx_ = nullptr;
        return;
    }

    u32 preferred = 0;
    if (cubeb_get_preferred_sample_rate(ctx_, &preferred) == CUBEB_OK && preferred != 0)
        streamRate_ = preferred;

    cubeb_stream_params params{};
    params.format = CUBEB_SAMPLE_FLOAT32NE;
    params.rate = streamRate_;
    params.channels = 2;
    params.layout = CUBEB_LAYOUT_STEREO;
    params.prefs = CUBEB_STREAM_PREF_NONE;

    u32 latencyFrames = 0;
    if (cubeb_get_min_latency(ctx_, &params, &latencyFrames) != CUBEB_OK || latencyFrames == 0)
        latencyFrames = streamRate_ / 50; // ~20 ms fallback

    if (cubeb_stream_init(ctx_, &stream_, "WhiteoutFlakes SFX", nullptr, nullptr, nullptr, &params,
                          latencyFrames, &DataCb, &StateCb, this) != CUBEB_OK ||
        !stream_) {
        std::fprintf(stderr, "[cubeb] cubeb_stream_init failed — audio disabled\n");
        stream_ = nullptr;
        cubeb_destroy(ctx_);
        ctx_ = nullptr;
        return;
    }

    if (cubeb_stream_start(stream_) != CUBEB_OK)
        std::fprintf(stderr, "[cubeb] cubeb_stream_start failed\n");
}

CubebSoundEmitter::~CubebSoundEmitter() {
    if (stream_) {
        cubeb_stream_stop(stream_);
        cubeb_stream_destroy(stream_);
        stream_ = nullptr;
    }
    if (ctx_) {
        cubeb_destroy(ctx_);
        ctx_ = nullptr;
    }
}

void CubebSoundEmitter::SetVolume(f32 v) {
    volume_.store(std::clamp(v, 0.0f, 1.0f), std::memory_order_relaxed);
}

f32 CubebSoundEmitter::GetVolume() const {
    return volume_.load(std::memory_order_relaxed);
}

void CubebSoundEmitter::SetListener(const Vector3f& pos, const Vector3f& forward,
                                    const Vector3f& up) {
    // right = forward x up, normalised. A degenerate basis (forward ∥ up, or
    // either zero) just leaves the previous right axis in place.
    const f32 rx = forward.y * up.z - forward.z * up.y;
    const f32 ry = forward.z * up.x - forward.x * up.z;
    const f32 rz = forward.x * up.y - forward.y * up.x;
    const f32 len = std::sqrt(rx * rx + ry * ry + rz * rz);

    std::lock_guard<std::mutex> lk(mu_);
    listenerPos_ = pos;
    if (len > 1e-6f)
        listenerRight_ = {rx / len, ry / len, rz / len};
    haveListener_ = true;
}

void CubebSoundEmitter::MixVoices(f32* out, i64 frames) {
    std::memset(out, 0, static_cast<usize>(frames) * 2 * sizeof(f32));

    {
        std::lock_guard<std::mutex> lk(mu_);
        const Vector3f lpos = listenerPos_;
        const Vector3f lright = listenerRight_;
        const bool haveListener = haveListener_;

        for (auto& v : voices_) {
            const usize total = v.samples.size() / 2;
            const usize avail = total - v.cursor;
            const usize n = std::min<usize>(avail, static_cast<usize>(frames));
            if (n == 0)
                continue;

            f32 tgtL = 0.0f, tgtR = 0.0f;
            ComputeVoiceGains(v, lpos, lright, haveListener, tgtL, tgtR);
            if (!v.gainPrimed) {
                v.curGainL = tgtL;
                v.curGainR = tgtR;
                v.gainPrimed = true;
            }
            // Ramp the per-channel gain across the buffer so a moving camera
            // pans the voice smoothly instead of stepping once per buffer.
            const f32 startL = v.curGainL;
            const f32 startR = v.curGainR;
            const f32 stepL = (tgtL - startL) / static_cast<f32>(n);
            const f32 stepR = (tgtR - startR) / static_cast<f32>(n);
            const f32* src = v.samples.data() + v.cursor * 2;
            for (usize i = 0; i < n; ++i) {
                const f32 gl = startL + stepL * static_cast<f32>(i);
                const f32 gr = startR + stepR * static_cast<f32>(i);
                out[i * 2 + 0] += src[i * 2 + 0] * gl;
                out[i * 2 + 1] += src[i * 2 + 1] * gr;
            }
            v.curGainL = tgtL;
            v.curGainR = tgtR;
            v.cursor += n;
        }
        voices_.erase(std::remove_if(voices_.begin(), voices_.end(),
                                     [](const Voice& v) {
                                         return v.cursor * 2 >= v.samples.size();
                                     }),
                      voices_.end());
    }

    // Master gain: the stored slider value run through the perceptual curve
    // (see kVolumeCurveExp) so the slider's sweet spot sits mid-travel.
    const f32 sliderVol = volume_.load(std::memory_order_relaxed);
    const f32 gain = std::pow(sliderVol, kVolumeCurveExp);
    if (gain != 1.0f) {
        for (i64 i = 0; i < frames * 2; ++i)
            out[i] *= gain;
    }
    // Hard-clip — overlapping voices can sum past [-1, 1].
    for (i64 i = 0; i < frames * 2; ++i)
        out[i] = std::clamp(out[i], -1.0f, 1.0f);
}

void CubebSoundEmitter::Play(const io::SndEntry& entry, const Vector3f& worldPos) {
    if (!content_ || !stream_)
        return;

    std::string attempted;
    auto bytes = ResolveSoundBytes(*content_, entry, &attempted);
    if (!bytes) {
        std::fprintf(stderr, "[cubeb] missing -- %zu source path(s); probed: %s\n",
                     entry.filePaths.size(), attempted.empty() ? "<none>" : attempted.c_str());
        return;
    }

    auto decoded = DecodePcm(*bytes);
    if (!decoded) {
        std::fprintf(stderr, "[cubeb] decode failed for SND entry\n");
        return;
    }

    // Decode + fold + resample on the caller's thread so the audio callback
    // only ever does a gain-ramped mix.
    std::vector<f32> stereo = FoldToStereo(*decoded);
    stereo = ResampleStereo(stereo, decoded->sampleRate, streamRate_);
    if (stereo.empty())
        return;

    Voice voice;
    voice.samples = std::move(stereo);
    voice.cursor = 0;
    voice.worldPos = worldPos;
    voice.entryVolume = entry.volume;

    std::lock_guard<std::mutex> lk(mu_);
    if (voices_.size() >= kMaxVoices)
        voices_.erase(voices_.begin()); // drop the oldest
    voices_.push_back(std::move(voice));
}

} // namespace whiteout::flakes
