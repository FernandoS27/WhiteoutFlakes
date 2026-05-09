#include "windows_sound_emitter.h"

#include "whiteout/flakes/content_provider.h"
#include "whiteout/flakes/event_data.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>

#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_OGG
#include "dr_flac.h"

#pragma comment(lib, "winmm.lib")

namespace whiteout::flakes {

using namespace whiteout::flakes::io;
using namespace whiteout::flakes::renderer;

namespace {

std::vector<u8> WrapPcmS16AsWav(const drflac_int16* samples,
                                      drflac_uint64       totalFrames,
                                      u32        channels,
                                      u32        sampleRate) {
    const u32 payloadBytes =
        static_cast<u32>(totalFrames) *
        static_cast<u32>(channels)    * sizeof(i16);
    const u32 totalBytes = 44u + payloadBytes;

    std::vector<u8> wav;
    wav.resize(totalBytes);
    auto put32 = [&](usize off, u32 v) {
        wav[off]     = static_cast<u8>(v        & 0xFF);
        wav[off + 1] = static_cast<u8>((v >> 8) & 0xFF);
        wav[off + 2] = static_cast<u8>((v >> 16) & 0xFF);
        wav[off + 3] = static_cast<u8>((v >> 24) & 0xFF);
    };
    auto put16 = [&](usize off, u16 v) {
        wav[off]     = static_cast<u8>(v        & 0xFF);
        wav[off + 1] = static_cast<u8>((v >> 8) & 0xFF);
    };
    std::memcpy(wav.data() + 0, "RIFF", 4);
    put32(4, totalBytes - 8);
    std::memcpy(wav.data() + 8,  "WAVE", 4);
    std::memcpy(wav.data() + 12, "fmt ", 4);
    put32(16, 16);
    put16(20, 1);
    put16(22, static_cast<u16>(channels));
    put32(24, sampleRate);
    put32(28, sampleRate * channels * 2);
    put16(32, static_cast<u16>(channels * 2));
    put16(34, 16);
    std::memcpy(wav.data() + 36, "data", 4);
    put32(40, payloadBytes);
    if (payloadBytes > 0) {
        std::memcpy(wav.data() + 44, samples, payloadBytes);
    }
    return wav;
}

std::optional<std::vector<u8>> DecodeFlacToWav(const std::vector<u8>& flac) {
    u32    channels    = 0;
    u32    sampleRate  = 0;
    drflac_uint64   totalFrames = 0;
    drflac_int16*   samples     = drflac_open_memory_and_read_pcm_frames_s16(
        flac.data(), flac.size(), &channels, &sampleRate, &totalFrames, nullptr);
    if (!samples || channels == 0 || sampleRate == 0 || totalFrames == 0) {
        if (samples) drflac_free(samples, nullptr);
        return std::nullopt;
    }
    auto wav = WrapPcmS16AsWav(samples, totalFrames, channels, sampleRate);
    drflac_free(samples, nullptr);
    return wav;
}

std::optional<std::vector<u8>> ResolveSoundBytes(
    const IContentProvider& cp,
    const io::SndEntry&     entry,
    std::string*            attemptedOut) {

    if (entry.filePaths.empty()) return std::nullopt;

    struct PathParts {
        std::string dir;
        std::string stem;
        std::string extLow;
        bool        hasExt;
    };
    auto split = [](const std::string& path) -> PathParts {
        const auto sepPos = path.find_last_of("/\\");
        const auto dotPos = path.rfind('.');
        const bool hasExt =
            dotPos != std::string::npos &&
            (sepPos == std::string::npos || dotPos > sepPos);
        const usize baseStart = (sepPos == std::string::npos) ? 0 : sepPos + 1;
        PathParts p;
        p.dir    = path.substr(0, baseStart);
        p.stem   = hasExt ? path.substr(baseStart, dotPos - baseStart)
                          : path.substr(baseStart);
        p.extLow = hasExt ? path.substr(dotPos) : std::string{};
        for (auto& c : p.extLow) c = (char)std::tolower((unsigned char)c);
        p.hasExt = hasExt;
        return p;
    };

    auto looks_like_flac = [](const std::vector<u8>& b) {
        return b.size() >= 4 && b[0] == 'f' && b[1] == 'L' &&
                                b[2] == 'a' && b[3] == 'C';
    };

    auto fetch = [&](const std::string& p, bool decodeFlac)
        -> std::optional<std::vector<u8>> {
        if (p.empty()) return std::nullopt;
        auto bytes = cp.ReadFile(p, nullptr);
        if (!bytes) return std::nullopt;
        if (decodeFlac && looks_like_flac(*bytes)) {
            auto wav = DecodeFlacToWav(*bytes);
            if (!wav) return std::nullopt;
            return wav;
        }
        return bytes;
    };

    auto candidates = [&](const std::string& path)
        -> std::vector<std::pair<std::string, bool>> {
        std::vector<std::pair<std::string, bool>> out;
        const PathParts p = split(path);

        std::vector<std::string> stems{ p.stem };
        usize end = p.stem.size();
        while (end > 0 && p.stem[end - 1] >= '0' && p.stem[end - 1] <= '9') --end;
        if (end > 0 && end < p.stem.size()) {
            stems.push_back(p.stem.substr(0, end));
        }

        auto push = [&](std::string path, bool flac) {
            for (const auto& [existing, _] : out) {
                if (existing == path) return;
            }
            out.emplace_back(std::move(path), flac);
        };

        if (p.hasExt) push(path, p.extLow == ".flac");
        else          push(path, true);

        for (const auto& s : stems) {
            push(p.dir + s + ".wav",  false);
            push(p.dir + s + ".flac", true);
        }
        return out;
    };

    static thread_local std::mt19937 rng{std::random_device{}()};
    const usize n     = entry.filePaths.size();
    const usize start = (n == 1) ? 0 : (rng() % n);
    for (usize i = 0; i < n; ++i) {
        for (const auto& [path, decodeFlac] : candidates(entry.filePaths[(start + i) % n])) {
            if (auto bytes = fetch(path, decodeFlac)) return bytes;
            if (attemptedOut) {
                if (!attemptedOut->empty()) attemptedOut->append(", ");
                attemptedOut->append(path);
            }
        }
    }
    return std::nullopt;
}

void ScalePcmGain(u8* buf, usize size, f32 gain) {
    if (gain == 1.0f || size < 44) return;
    if (std::memcmp(buf, "RIFF", 4) != 0) return;
    if (std::memcmp(buf + 8, "WAVE", 4) != 0) return;

    u16 formatTag    = 0;
    u16 bitsPerSample = 0;

    auto rd_u32 = [](const u8* p) {
        return static_cast<u32>(p[0])
             | (static_cast<u32>(p[1]) << 8)
             | (static_cast<u32>(p[2]) << 16)
             | (static_cast<u32>(p[3]) << 24);
    };
    auto rd_u16 = [](const u8* p) {
        return static_cast<u16>(
            static_cast<u16>(p[0]) |
            (static_cast<u16>(p[1]) << 8));
    };

    usize pos = 12;
    while (pos + 8 <= size) {
        const u8* hdr  = buf + pos;
        const u32 csz  = rd_u32(hdr + 4);
        const usize   next = pos + 8 + ((csz + 1u) & ~1u);

        if (std::memcmp(hdr, "fmt ", 4) == 0 && csz >= 16 && pos + 8 + 16 <= size) {
            formatTag     = rd_u16(hdr + 8);
            bitsPerSample = rd_u16(hdr + 8 + 14);
        } else if (std::memcmp(hdr, "data", 4) == 0) {

            if (formatTag != 1) return;
            const usize dataOff = pos + 8;
            if (dataOff > size) return;
            const usize dataSz = std::min(static_cast<usize>(csz), size - dataOff);

            if (bitsPerSample == 16) {
                i16* samples = reinterpret_cast<i16*>(buf + dataOff);
                const usize n   = dataSz / sizeof(i16);
                for (usize i = 0; i < n; ++i) {
                    i32 s = static_cast<i32>(static_cast<f32>(samples[i]) * gain);
                    if (s >  32767) s =  32767;
                    if (s < -32768) s = -32768;
                    samples[i] = static_cast<i16>(s);
                }
            } else if (bitsPerSample == 8) {

                u8* samples = buf + dataOff;
                for (usize i = 0; i < dataSz; ++i) {
                    i32 centred = static_cast<i32>(samples[i]) - 128;
                    i32 scaled  = static_cast<i32>(static_cast<f32>(centred) * gain);
                    i32 out     = scaled + 128;
                    if (out > 255) out = 255;
                    if (out <   0) out =   0;
                    samples[i] = static_cast<u8>(out);
                }
            }
            return;
        }

        if (next <= pos) return;
        pos = next;
    }
}

}

WindowsSoundEmitter::WindowsSoundEmitter(const IContentProvider* content)
    : content_(content) {}

void WindowsSoundEmitter::SetVolume(f32 v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    volume_.store(v, std::memory_order_relaxed);
}

f32 WindowsSoundEmitter::GetVolume() const {
    return volume_.load(std::memory_order_relaxed);
}

WindowsSoundEmitter::~WindowsSoundEmitter() {

    PlaySoundW(nullptr, nullptr, SND_PURGE);
}

void WindowsSoundEmitter::Play(const io::SndEntry& entry,
                               const Vector3f&      ) {
    if (!content_) return;

    std::string attempted;
    auto bytes = ResolveSoundBytes(*content_, entry, &attempted);
    if (!bytes) {

        std::fprintf(stderr,
                     "[WDEX sound] missing -- %zu source path(s); probed: %s\n",
                     entry.filePaths.size(),
                     attempted.empty() ? "<none>" : attempted.c_str());
        return;
    }

    std::lock_guard<std::mutex> lk(mu_);
    PlaySoundW(nullptr, nullptr, SND_PURGE);
    currentBuffer_ = std::move(*bytes);

    ScalePcmGain(currentBuffer_.data(), currentBuffer_.size(),
                 volume_.load(std::memory_order_relaxed));

    PlaySoundW(reinterpret_cast<LPCWSTR>(currentBuffer_.data()),
               nullptr,
               SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

}
