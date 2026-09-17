#pragma once

#include "whiteout/flakes/enums.h" // ProductId

#include <filesystem>
#include <string>
#include <vector>

namespace whiteout::flakes::renderer {
class RenderService;
}

namespace whiteout::flakes::io {
class FileContentProvider;
}

namespace whiteout::flakes {

// Where the settings ini lives: beside the executable on Windows, in the
// per-user config directory on Linux / macOS.
std::filesystem::path SettingsIniPath();

// Redirect that file. An empty path restores the per-OS default. Exists so a
// test can exercise the round-trip without touching the developer's real
// settings, which on Linux and macOS are in a shared user config directory.
void SetSettingsIniPathOverride(const std::filesystem::path& file);

// Root for bundled read-only assets (`lang/`, `fonts/`): the executable's
// directory, or Contents/Resources/ inside a macOS .app bundle (where codesign
// accepts non-code data), mirroring the engine's asset lookup.
std::filesystem::path AssetDir();

// `loopNonLoopingPolicy` is a tool-side default: when true, freshly-loaded
// actors should have actor->ignoreNonLooping = true (callers apply this on
// load). `forceHd` is the "Reforged Graphics" tool toggle (force HD for every
// model). `languageCode` is the persisted UI-language code ("en", "es", …).
// These are viewer-side prefs the caller applies after load; the rest of the
// settings live on RenderService. LoadSettingsIni populates them from disk;
// SaveSettingsIni reads them.
void LoadSettingsIni(renderer::RenderService& service, bool& loopNonLoopingPolicy, bool& forceHd,
                     std::string& languageCode);

void SaveSettingsIni(const renderer::RenderService& service, bool loopNonLoopingPolicy, bool forceHd,
                     const std::string& languageCode);

// Reads only the keys that must land on RenderSettings *before* the
// render thread spins up — currently `GraphicsDebug` (the validation
// layer is wired in at gfx::CreateDevice time) and `DefaultBackend`
// (main.cpp consults it when --backend is omitted). Default values
// stay in place when the keys are missing.
void LoadStartupSettingsFromIni(renderer::RenderService& service);

// User overrides for the FileContentProvider, driven by the Settings > IO
// tab. installPath empty means "use that game's auto-detected path";
// mpqListSet=false means "use the provider's default list for the game" (so
// "never opened the IO tab" silently falls back to defaults instead of
// loading zero MPQs).
//
// Stored per game, because the three products have genuinely different
// storage setups and a user configures each once. Warcraft III keeps the
// original `[IO]` section so an existing ini carries over untouched; the
// others get `[IO.Wow]` / `[IO.Sc2]`.
struct IoPathOverrides {
    std::string installPath;
    bool ignoreCasc = false;
    bool ignoreMpq = false;
    bool mpqListSet = false;
    std::vector<std::string> mpqList;
    // World of Warcraft only: community `id;path` CSV that turns its id-keyed
    // CASC root into something browsable.
    std::string listfilePath;
    // World of Warcraft only: community `keyName keyHex` list, without which
    // any file holding a TACT-encrypted frame reads back as missing.
    std::string tactKeyPath;
    // StarCraft II only: the Heroes of the Storm root, the second CASC that
    // product opens.
    std::string hotsInstallPath;
};

// The no-argument forms mean Warcraft III, which is what every existing
// caller meant before the settings grew a per-game profile.
IoPathOverrides LoadIoPathOverrides();
IoPathOverrides LoadIoPathOverrides(ProductId game);
void SaveIoPathOverrides(const IoPathOverrides& overrides);
void SaveIoPathOverrides(ProductId game, const IoPathOverrides& overrides);

// Which game the Settings profile panel was left on, so the viewer comes back
// reading the same storage it was closed on. Warcraft III when unset.
ProductId LoadIoProduct();
void SaveIoProduct(ProductId game);

// Point @p provider at @p game and apply that game's saved overrides. Startup
// and the Settings profile panel both go through here so a provider is
// configured identically whether the user picked the game now or last week.
void ApplyIoPathOverrides(io::FileContentProvider& provider, ProductId game);

} // namespace whiteout::flakes
