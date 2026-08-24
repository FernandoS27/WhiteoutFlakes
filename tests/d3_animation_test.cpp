// ============================================================================
// D3 animation — gate D3-G4, offline.
//
// The trap this gate exists for: `native::Quaternion16` declares
// `u16 nX, nY, nZ, nW`, and the encoding is **signed** `i16 / 32767`. Read as
// unsigned, every rotation lands in the wrong hemisphere and the model folds.
//
// So the gate asserts `|sum(q^2) - 1| < 0.01` over every rotation key under the
// SIGNED decode, and — this is the half that makes it a gate rather than a
// formality — checks that the UNSIGNED decode *fails* the same test on
// essentially every key. A discriminator that both readings pass would prove
// nothing.
//
// The second thing measured here is a claim about the on-disk shape: the
// guide's `base + (u16/32767 - 1)` translation decode and its `u16/32767` scale
// decode describe the **runtime's** compressed clip codec, not the v260 file
// format. `AnimPermutation` ships plainly-keyed curves, and applying the
// runtime decode to them produces animation that is uniformly, subtly wrong.
// The evidence is that translation keys are plain `Vector3f` in world-sized
// units and scale keys are plain floats near 1 — both printed and asserted.
//
// Corpus root: WDX_TEST_D3_CORPUS, default C:/Projects/WhiteoutLib/Corpus/D3.
// Skipped is not passed.
// ============================================================================

#include <catch2/catch_test_macros.hpp>

#include <whiteout/sno/d3/native/d3_native.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace d3n = ::whiteout::sno::d3::native;
using namespace ::whiteout;

namespace {

fs::path CorpusRoot() {
    if (const char* v = std::getenv("WDX_TEST_D3_CORPUS"); v && *v)
        return fs::path(v);
    return fs::path("C:/Projects/WhiteoutLib/Corpus/D3");
}

std::size_t SweepLimit() {
    if (const char* v = std::getenv("WDX_TEST_D3_LIMIT"); v && *v)
        return static_cast<std::size_t>(std::strtoul(v, nullptr, 10));
    return 300;
}

std::vector<fs::path> FindFiles(const fs::path& dir, const char* ext) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec))
        return out;
    for (fs::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (ec)
            break;
        if (it->is_regular_file(ec) && it->path().extension() == ext)
            out.push_back(it->path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<u8> ReadAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f)
        return {};
    f.seekg(0, std::ios::end);
    const auto n = static_cast<std::size_t>(f.tellg());
    f.seekg(0);
    std::vector<u8> b(n);
    f.read(reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(n));
    return b;
}

f32 SignedNorm(const d3n::Quaternion16& q) {
    auto s = [](u16 v) { return static_cast<f32>(static_cast<i16>(v)) * (1.0f / 32767.0f); };
    const f32 x = s(q.nX), y = s(q.nY), z = s(q.nZ), w = s(q.nW);
    return x * x + y * y + z * z + w * w;
}

f32 UnsignedNorm(const d3n::Quaternion16& q) {
    auto s = [](u16 v) { return static_cast<f32>(v) * (1.0f / 32767.0f); };
    const f32 x = s(q.nX), y = s(q.nY), z = s(q.nZ), w = s(q.nW);
    return x * x + y * y + z * z + w * w;
}

} // namespace

TEST_CASE("D3 corpus: rotation keys are signed i16/32767", "[d3][corpus]") {
    const fs::path root = CorpusRoot() / "Anim";
    const auto files = FindFiles(root, ".ani");
    if (files.empty()) {
        WARN("No D3 corpus at " << root.string()
                                << " (set WDX_TEST_D3_CORPUS). SKIPPED, not passed.");
        return;
    }
    const std::size_t limit = SweepLimit();
    const std::size_t take = (limit == 0) ? files.size() : (std::min)(limit, files.size());

    std::size_t keys = 0, signedFail = 0, unsignedFail = 0;
    f32 worstSigned = 0.0f;
    std::size_t parsed = 0, parseFailed = 0;

    for (std::size_t i = 0; i < take; ++i) {
        const auto bytes = ReadAll(files[i]);
        auto anim = d3n::parseAnim(bytes);
        if (!anim) {
            ++parseFailed;
            continue;
        }
        ++parsed;
        for (const auto& perm : anim->arPermutations) {
            for (const auto& curve : perm.arRotationCurves) {
                for (const auto& k : curve.arKeys) {
                    ++keys;
                    const f32 sn = SignedNorm(k.tRotation);
                    worstSigned = (std::max)(worstSigned, std::fabs(sn - 1.0f));
                    if (std::fabs(sn - 1.0f) >= 0.01f)
                        ++signedFail;
                    if (std::fabs(UnsignedNorm(k.tRotation) - 1.0f) >= 0.01f)
                        ++unsignedFail;
                }
            }
        }
    }

    std::printf("[d3-g4] %zu/%zu files: %zu parsed, %zu failed | %zu rotation keys | "
                "signed decode off-unity: %zu (worst %.6f) | unsigned decode off-unity: %zu\n",
                take, files.size(), parsed, parseFailed, keys, signedFail,
                static_cast<double>(worstSigned), unsignedFail);
    if (take < files.size())
        std::printf("[d3-g4] %zu files NOT swept (WDX_TEST_D3_LIMIT)\n", files.size() - take);

    REQUIRE(keys > 0);
    CHECK(signedFail == 0);
    // The discriminator half. If the unsigned reading also passed, this gate
    // would be a formality rather than evidence — and the one-line mistake it
    // exists to catch (reading nX..nW as the u16 they are declared as) would
    // sail through.
    CHECK(unsignedFail > keys / 2);
}

TEST_CASE("D3 corpus: translation and scale keys are plain, not the runtime codec",
          "[d3][corpus]") {
    const fs::path root = CorpusRoot() / "Anim";
    const auto files = FindFiles(root, ".ani");
    if (files.empty()) {
        WARN("No D3 corpus at " << root.string() << ". SKIPPED, not passed.");
        return;
    }
    const std::size_t limit = SweepLimit();
    const std::size_t take = (limit == 0) ? files.size() : (std::min)(limit, files.size());

    std::size_t tKeys = 0, sKeys = 0;
    std::size_t scaleNearOne = 0, scaleNonFinite = 0, transNonFinite = 0;
    f32 worstAbsTranslation = 0.0f;
    std::size_t framesUnsorted = 0;

    for (std::size_t i = 0; i < take; ++i) {
        auto anim = d3n::parseAnim(ReadAll(files[i]));
        if (!anim)
            continue;
        for (const auto& perm : anim->arPermutations) {
            for (const auto& c : perm.arTranslationCurves) {
                i32 last = -1;
                for (const auto& k : c.arKeys) {
                    ++tKeys;
                    if (k.nFrame < last)
                        ++framesUnsorted;
                    last = k.nFrame;
                    const f32 m = (std::max)({std::fabs(k.vPosition.x), std::fabs(k.vPosition.y),
                                              std::fabs(k.vPosition.z)});
                    if (!std::isfinite(m))
                        ++transNonFinite;
                    else
                        worstAbsTranslation = (std::max)(worstAbsTranslation, m);
                }
            }
            for (const auto& c : perm.arScaleCurves) {
                for (const auto& k : c.arKeys) {
                    ++sKeys;
                    if (!std::isfinite(k.flScale))
                        ++scaleNonFinite;
                    else if (k.flScale > 0.5f && k.flScale < 2.0f)
                        ++scaleNearOne;
                }
            }
        }
    }

    const double scalePct =
        sKeys ? (100.0 * static_cast<double>(scaleNearOne) / static_cast<double>(sKeys)) : 0.0;
    std::printf("[d3-anim] %zu translation keys (worst |component| %.3f, %zu non-finite) | "
                "%zu scale keys (%.1f%% in [0.5, 2.0], %zu non-finite) | %zu unsorted frame runs\n",
                tKeys, static_cast<double>(worstAbsTranslation), transNonFinite, sKeys, scalePct,
                scaleNonFinite, framesUnsorted);

    REQUIRE(tKeys > 0);
    CHECK(transNonFinite == 0);
    CHECK(scaleNonFinite == 0);
    // The bracketing sampler does an upper_bound over frame numbers, which
    // needs them sorted. Shipped curves are; a run that reports otherwise would
    // mean the sampler is interpolating between the wrong pair.
    CHECK(framesUnsorted == 0);
    // Scales cluster at 1. Under the runtime's `u16/32767` decode a float near
    // 1 would have to have been stored as ~32767, and these are plain floats —
    // which is the shape claim, measured rather than argued.
    if (sKeys > 0)
        CHECK(scalePct > 50.0);
}

TEST_CASE("D3 corpus: clip timing comes out of the permutation", "[d3][corpus]") {
    const fs::path root = CorpusRoot() / "Anim";
    const auto files = FindFiles(root, ".ani");
    if (files.empty()) {
        WARN("No D3 corpus at " << root.string() << ". SKIPPED, not passed.");
        return;
    }
    const std::size_t take = (std::min)(SweepLimit() ? SweepLimit() : files.size(), files.size());

    std::size_t perms = 0, sane = 0, zeroFps = 0, singleFrame = 0;
    f32 worstDuration = 0.0f;
    for (std::size_t i = 0; i < take; ++i) {
        auto anim = d3n::parseAnim(ReadAll(files[i]));
        if (!anim)
            continue;
        for (const auto& p : anim->arPermutations) {
            ++perms;
            // fps = flFramesPerTick * 60; duration = (frames - 1) / fps.
            const f32 fps = p.flFramesPerTick * 60.0f;
            if (fps <= 0.0f) {
                ++zeroFps;
                continue;
            }
            if (p.dwFrameCount <= 1) {
                ++singleFrame;
                continue;
            }
            const f32 dur = static_cast<f32>(p.dwFrameCount - 1) / fps;
            worstDuration = (std::max)(worstDuration, dur);
            // A clip under an hour, which every animation in a game is.
            if (dur > 0.0f && dur < 3600.0f)
                ++sane;
        }
    }
    std::printf("[d3-anim] %zu permutations: %zu with a sane duration, %zu zero-fps, "
                "%zu single-frame | longest %.2f s\n",
                perms, sane, zeroFps, singleFrame, static_cast<double>(worstDuration));
    REQUIRE(perms > 0);
    CHECK(sane > 0);
}
