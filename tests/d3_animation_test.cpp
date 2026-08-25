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

#include "io/d3/d3_model_adapter.h"
#include "io/d3/d3_sno_cache.h"
#include "io/file_content_provider.h"
#include "io/storage_browser.h"
#include "whiteout/flakes/content_ref.h"

#include <whiteout/sno/d3/native/d3_native.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
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

// ---------------------------------------------------------------------------
// Clip DURATIONS reaching the host, which is a separate question from whether
// the keys decode. `AnimationDriver::Advance` hands `GetSequences()` to the
// playlist and the playlist wraps the playback cursor at `endMs`, so a wrong
// duration is a wrong LOOP POINT no matter how correct the sampling is.
//
// D3 is the one format here whose durations are not free. `.m2` states every
// sequence's duration in the main file, so its lazy `.anim` path costs nothing;
// `AnimSetTagMapEntry` is 12 bytes — value type, tag id, Anim SNO — and states
// no frame count, so a lazily-bound D3 clip can only advertise a nominal one.
// Measured on `43_AD_graveDigger_A`: all 34 clips have a true duration other
// than that nominal 1 s, spanning 33 ms to 7000 ms. Windowed at 1 s a 7 s clip
// plays its first seventh and cuts back; only an exactly-1 s clip loops clean.
//
// Needs the install: the AnimSet names its clips by SNO id, and resolving an id
// is what a CASC storage does. Skipped is not passed.
// ---------------------------------------------------------------------------
TEST_CASE("D3 clip durations reach the host, not a nominal placeholder",
          "[d3][animation][install]") {
    using ::whiteout::flakes::ContentRef;
    using ::whiteout::flakes::ProductId;

    flakes::io::FileContentProvider provider;
    if (const char* root = std::getenv("WDX_TEST_D3_INSTALL"); root && *root)
        provider.SetInstallPath(root);
    provider.SetGame(ProductId::D3);
    if (provider.GamePath(ProductId::D3).empty()) {
        WARN("No Diablo III install (set WDX_TEST_D3_INSTALL). SKIPPED, not passed.");
        return;
    }

    flakes::io::StorageBrowser browser;
    std::string err;
    if (!browser.Open(provider.GamePath(ProductId::D3), flakes::io::StorageKind::Casc, &err)) {
        WARN("Could not browse the D3 install: " << err << ". SKIPPED, not passed.");
        return;
    }

    // Walk for actors with a decent clip count — a one-clip prop proves nothing
    // about a loop point.
    std::vector<std::string> actors;
    std::vector<std::string> stack{""};
    while (!stack.empty() && actors.size() < 12) {
        const std::string dir = stack.back();
        stack.pop_back();
        browser.NavigateTo(dir);
        for (const auto& f : browser.Current().folders)
            stack.push_back(dir.empty() ? f : dir + "\\" + f);
        for (const auto& f : browser.Current().modelFiles) {
            if (f.size() > 4 && f.compare(f.size() - 4, 4, ".acr") == 0) {
                actors.push_back(browser.ChildPath(f));
                if (actors.size() >= 12)
                    break;
            }
        }
    }
    if (actors.empty()) {
        WARN("No `.acr` in the install listing. SKIPPED, not passed.");
        return;
    }

    std::size_t examined = 0, offNominal = 0, total = 0;
    i32 shortest = 1 << 30, longest = 0;

    for (const std::string& ref : actors) {
        auto bytes = provider.ReadFile(ref);
        if (!bytes)
            continue;
        flakes::io::D3SnoCache cache(&provider);
        // The shipping default. Passing it explicitly rather than reading
        // RenderSettings keeps this a statement about the adapter.
        auto a = flakes::io::D3ModelAdapter::LoadActor(ContentRef::FromPath(ref), *bytes, cache,
                                                       /*lazyClips=*/false);
        if (!a)
            continue;
        const auto seqs = a->GetSequences();
        if (seqs.size() < 4)
            continue; // props and one-clip spawners say nothing about looping
        ++examined;
        for (const auto& s : seqs) {
            ++total;
            // 1000 ms is the placeholder an unresolved clip advertises. A real
            // clip landing on exactly 1000 ms is possible but vanishingly rare;
            // what would be damning is EVERY clip landing there.
            if (s.endMs != 1000)
                ++offNominal;
            REQUIRE(s.endMs > 0);
            shortest = (std::min)(shortest, s.endMs);
            longest = (std::max)(longest, s.endMs);
        }
        if (examined >= 4)
            break;
    }

    if (examined == 0) {
        WARN("No multi-clip actor in the sample. SKIPPED, not passed.");
        return;
    }
    std::printf("[d3-anim] %zu actor(s), %zu clip(s): %zu off the 1000 ms nominal, "
                "range %d..%d ms\n",
                examined, total, offNominal, shortest, longest);

    // The discriminator. Bind lazily and every one of these collapses onto the
    // placeholder, which is what silently cut every loop short.
    CHECK(offNominal * 4 > total * 3); // >75% must carry a real duration
    CHECK(longest > 1000);             // and something must be longer than the placeholder
}

// ============================================================================
// Clip names — install-only, because the names are not in the model files.
//
// Nothing inside a D3 asset names anything it references. An AnimSet maps an
// animation tag id to an Anim SNO and stops there, and the tag id has no name
// either: 2.6.2's own tag-to-text call (AnimTagName, 0x71006A09A0) walks a
// 453-entry table whose every name pointer aims at the same empty string, so
// the shipped client cannot spell a tag out loud any more than we can. The
// power-tag table sitting immediately after it in the same array kept all
// 1,258 of its names, which is what makes the blanks a decision rather than a
// misread record layout.
//
// What does name things is CoreTOC, which the CASC root has already parsed to
// spell an id as `Base\Anim\<name>.ani`. So this asserts the two ends of that:
// the id -> path direction exists at all, and clips come out of the adapter
// carrying it. The discriminator is `Tag_%05X`, the fallback the adapter uses
// when no name is in reach — a run where the plumbing is missing does not fail
// to produce names, it produces those, which reads as working.
// ============================================================================
TEST_CASE("D3 clips are named after the `.ani` CoreTOC names, not their tag id",
          "[d3][animation][install]") {
    using ::whiteout::flakes::ContentRef;
    using ::whiteout::flakes::ProductId;

    flakes::io::FileContentProvider provider;
    if (const char* root = std::getenv("WDX_TEST_D3_INSTALL"); root && *root)
        provider.SetInstallPath(root);
    provider.SetGame(ProductId::D3);
    if (provider.GamePath(ProductId::D3).empty()) {
        WARN("No Diablo III install (set WDX_TEST_D3_INSTALL). SKIPPED, not passed.");
        return;
    }

    flakes::io::StorageBrowser browser;
    std::string err;
    if (!browser.Open(provider.GamePath(ProductId::D3), flakes::io::StorageKind::Casc, &err)) {
        WARN("Could not browse the D3 install: " << err << ". SKIPPED, not passed.");
        return;
    }

    std::vector<std::string> actors;
    std::vector<std::string> stack{""};
    while (!stack.empty() && actors.size() < 12) {
        const std::string dir = stack.back();
        stack.pop_back();
        browser.NavigateTo(dir);
        for (const auto& f : browser.Current().folders)
            stack.push_back(dir.empty() ? f : dir + "\\" + f);
        for (const auto& f : browser.Current().modelFiles) {
            if (f.size() > 4 && f.compare(f.size() - 4, 4, ".acr") == 0) {
                actors.push_back(browser.ChildPath(f));
                if (actors.size() >= 12)
                    break;
            }
        }
    }
    if (actors.empty()) {
        WARN("No `.acr` in the install listing. SKIPPED, not passed.");
        return;
    }

    // Half of the claim, on its own: the manifest answers in both directions
    // for the actor we are about to load through it.
    const u32 actorId = provider.FileIdForPath(actors.front());
    REQUIRE(actorId != 0);
    const std::string back = provider.PathForFileId(actorId);
    CHECK_FALSE(back.empty());
    CHECK(back.find(".acr") != std::string::npos);

    std::size_t examined = 0, total = 0, placeholder = 0, duplicate = 0;
    std::vector<std::string> sample;

    for (const std::string& ref : actors) {
        auto bytes = provider.ReadFile(ref);
        if (!bytes)
            continue;
        flakes::io::D3SnoCache cache(&provider);
        auto a = flakes::io::D3ModelAdapter::LoadActor(ContentRef::FromPath(ref), *bytes, cache,
                                                       /*lazyClips=*/true);
        if (!a)
            continue;
        const auto seqs = a->GetSequences();
        if (seqs.size() < 4)
            continue;
        ++examined;
        std::set<std::string> seen;
        for (const auto& s : seqs) {
            ++total;
            REQUIRE_FALSE(s.name.empty());
            if (s.name.compare(0, 4, "Tag_") == 0)
                ++placeholder;
            if (!seen.insert(s.name).second)
                ++duplicate;
            if (sample.size() < 6)
                sample.push_back(s.name);
        }
        if (examined >= 4)
            break;
    }

    if (examined == 0) {
        WARN("No multi-clip actor in the sample. SKIPPED, not passed.");
        return;
    }
    std::printf("[d3-names] %zu actor(s), %zu clip(s): %zu unnamed, %zu duplicate\n", examined,
                total, placeholder, duplicate);
    for (const std::string& n : sample)
        std::printf("[d3-names]   %s\n", n.c_str());

    // A handful of tags can genuinely point at an Anim the install does not
    // hold (2 of Barbarian_Male's 259 do), so this is a supermajority and not
    // an absolute.
    CHECK(placeholder * 20 < total);
    // Names go into a host's sequence list, where two identical rows are a
    // bug report. The adapter disambiguates with the tag id; nothing should
    // reach the host still colliding.
    CHECK(duplicate == 0);
}
