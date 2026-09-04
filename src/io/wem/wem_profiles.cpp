#include "io/wem/wem_profiles.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace whiteout::flakes::io {

namespace {

// The build flags decide which native models this binary can construct at all.
// Warcraft III is unconditional — the MDX path is the renderer's own — so there
// is no WDX_ENABLE_MDX to consult.
#if WDX_ENABLE_M2
constexpr bool kM2Openable = true;
#else
constexpr bool kM2Openable = false;
#endif
#if WDX_ENABLE_M3
constexpr bool kM3Openable = true;
#else
constexpr bool kM3Openable = false;
#endif

#if WDX_ENABLE_D3
constexpr bool kD3Openable = true;
#else
constexpr bool kD3Openable = false;
#endif

} // namespace

ProductId ProductForWemProfile(wem::ProfileId profile) {
    switch (profile) {
    case wem::ProfileId::Wow:
        return ProductId::Wow;
    case wem::ProfileId::Sc2:
    case wem::ProfileId::Heroes:
        return ProductId::Sc2;
    case wem::ProfileId::Diablo3:
        return ProductId::D3;
    case wem::ProfileId::Generic:
    case wem::ProfileId::Wc3Classic:
    case wem::ProfileId::Wc3Reforged:
    default:
        return ProductId::Wc3;
    }
}

bool WemProfileOpenable(wem::ProfileId profile) {
    switch (profile) {
    case wem::ProfileId::Wc3Classic:
    case wem::ProfileId::Wc3Reforged:
        return true;
    case wem::ProfileId::Generic:
        // Never, and not because of a build flag: `Generic` names no game, so
        // it has no `formatId` and no converter serves it — `MdxConverter`
        // refuses it by name. It is a *source* profile, which is exactly what
        // WemDeriveSource uses it as.
        return false;
    case wem::ProfileId::Wow:
        return kM2Openable;
    case wem::ProfileId::Sc2:
    case wem::ProfileId::Heroes:
        return kM3Openable;
    case wem::ProfileId::Diablo3:
        return kD3Openable;
    default:
        return false;
    }
}

const char* WemProfileUnsupportedReason(wem::ProfileId profile) {
    switch (profile) {
    case wem::ProfileId::Wow:
        return kM2Openable ? "" : "this build has .m2 support compiled out (WDX_ENABLE_M2)";
    case wem::ProfileId::Sc2:
    case wem::ProfileId::Heroes:
        return kM3Openable ? "" : "this build has .m3 support compiled out (WDX_ENABLE_M3)";
    case wem::ProfileId::Diablo3:
        // A build flag now, and only that. Writing SNO *files* is still out of
        // scope (WEM design §18), but a viewer never needed one: `toAppearance`
        // builds the native `Appearances` in memory, which is exactly what the
        // other three profiles do with their format's struct.
        return kD3Openable ? "" : "this build has Diablo III support compiled out (WDX_ENABLE_D3)";
    case wem::ProfileId::Generic:
        return "a generic set names no game; pick the one to open it as";
    default:
        return "";
    }
}

wem::ProfileId WemProfileFromName(const std::string& name) {
    return wem::ProfileFromName(name);
}

bool WemProfileIsHd(wem::ProfileId profile) {
    return profile == wem::ProfileId::Wc3Reforged;
}

u32 MdxVersionForWemProfile(wem::ProfileId profile) {
    if (profile == wem::ProfileId::Wc3Reforged)
        return 1000u;
    if (profile == wem::ProfileId::Wc3Classic)
        return 800u;
    return 0u;
}

bool LooksLikeWemPath(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    for (char& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".wem";
}

wem::ProfileId WemDeriveSource(const wem::Document& document, wem::ProfileId target) {
    if (document.carries(target))
        return target;
    if (document.carries(document.defaultProfile))
        return document.defaultProfile;
    for (const wem::ProfileId declared : document.profiles) {
        if (declared != target)
            return declared;
    }
    return wem::ProfileId::Count;
}

std::vector<WemProfileOption> WemProfileOptions(const wem::Document& document) {
    // The union of what every model draws. A document with no models — legal,
    // and what an empty write produces — leaves this zero and every row a
    // derive, which is the honest answer for a file with nothing in it.
    wem::ProfileMask drawn = wem::kNoProfiles;
    for (const wem::Model& model : document.models)
        drawn |= model.drawnProfiles();

    std::vector<WemProfileOption> out;
    out.reserve(static_cast<usize>(wem::ProfileId::Count));
    for (u32 i = 0; i < static_cast<u32>(wem::ProfileId::Count); ++i) {
        const auto profile = static_cast<wem::ProfileId>(i);
        const wem::ProfileDesc& desc = wem::Profile(profile);

        WemProfileOption option;
        option.profile = profile;
        option.name = desc.name;
        option.displayName = desc.displayName;
        option.carried = document.carries(profile);
        option.drawn = wem::HasProfile(drawn, profile);
        option.supported = WemProfileOpenable(profile);
        option.derived = !option.carried;
        option.deriveFrom = option.derived ? WemDeriveSource(document, profile) : profile;
        out.push_back(option);
    }

    // Stable, so two profiles that tie keep registry order — which is the order
    // ProfileId is declared in, and the one a reader of profile.h expects.
    std::stable_sort(out.begin(), out.end(),
                     [](const WemProfileOption& a, const WemProfileOption& b) {
                         const int ra = a.supported ? (a.carried ? 0 : 1) : 2;
                         const int rb = b.supported ? (b.carried ? 0 : 1) : 2;
                         return ra < rb;
                     });
    return out;
}

wem::ProfileId DefaultWemProfile(const wem::Document& document) {
    if (document.carries(document.defaultProfile) && WemProfileOpenable(document.defaultProfile))
        return document.defaultProfile;
    for (const wem::ProfileId declared : document.profiles) {
        if (WemProfileOpenable(declared))
            return declared;
    }
    // Nothing carried is openable. A derive still can be — a build with a
    // format compiled out opening someone else's document is exactly that — so
    // fall through to the option list's own preference rather than refusing.
    for (const WemProfileOption& option : WemProfileOptions(document)) {
        if (option.supported && option.deriveFrom != wem::ProfileId::Count)
            return option.profile;
    }
    return wem::ProfileId::Count;
}

} // namespace whiteout::flakes::io
