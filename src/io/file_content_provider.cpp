#include "file_resolver.h"
#include "io/file_content_provider.h"
#include "whiteout/flakes/types.h"
#include "whiteout/flakes/util/path_utf8.h"

#include <whiteout/utils/blizzard_game_finder.h>

#if WHITEOUT_HAS_CASC
#include <whiteout/storages/casc/storage.h>
#endif

#if WHITEOUT_HAS_MPQ
#include <whiteout/storages/mpq/storage.h>
#endif

#include <cstdio>
#include <filesystem>
#include <fstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#include <climits>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <climits>
#endif

namespace whiteout::flakes::io {

namespace fs = std::filesystem;

// Stock load order for Warcraft III's MPQs — patch first so its overrides
// win, then the expansion, then the base game. Surfaced via DefaultMpqList()
// so the UI can offer "reset" without re-deriving the list itself.
static const char* const kDefaultMpqNames[] = {
    "War3Patch.mpq",
    "War3x.mpq",
    "war3.mpq",
};

// Returns the directory containing the running executable, or {} on failure.
// Used as a fallback search root for engine-shipped assets (shaders, etc.)
// that ship next to the binary rather than alongside the loaded model.
static fs::path DiscoverExecutableDirectory() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH * 4] = {};
    DWORD len = ::GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    if (len == 0 || len >= std::size(buf))
        return {};
    return fs::path(std::wstring(buf, buf + len)).parent_path();
#elif defined(__linux__)
    char buf[PATH_MAX] = {};
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0)
        return {};
    return fs::path(std::string(buf, static_cast<usize>(n))).parent_path();
#elif defined(__APPLE__)
    // _NSGetExecutablePath writes the path used to launch the process;
    // canonicalise via std::filesystem to resolve symlinks. When the
    // executable lives inside a .app bundle (`.../X.app/Contents/MacOS/X`)
    // the asset search root is Contents/Resources/ — that's where macOS
    // wants read-only ship-with-the-binary data (and where codesign won't
    // choke on our non-Mach-O `.bls` files). Detect that case by checking
    // for the `Contents/MacOS` suffix on the exe's parent.
    char buf[PATH_MAX] = {};
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0)
        return {};
    std::error_code ec;
    fs::path resolved = fs::canonical(fs::path(buf), ec);
    if (ec)
        resolved = fs::path(buf);
    fs::path dir = resolved.parent_path();
    if (dir.filename() == "MacOS" && dir.parent_path().filename() == "Contents") {
        return dir.parent_path() / "Resources";
    }
    return dir;
#else
    return {};
#endif
}

struct FileContentProvider::Impl {
    std::string wc3Path;      // auto-detected install root from Discover().
    std::string installPath;  // currently-active install root (defaults to wc3Path).
    bool ignoreCasc = false;
    bool ignoreMpq = false;
    std::vector<std::string> mpqList = FileContentProvider::DefaultMpqList();
    FileResolver resolver;

#if WHITEOUT_HAS_CASC
    std::optional<whiteout::storages::casc::Storage> cascStorage;
#endif

#if WHITEOUT_HAS_MPQ
    std::vector<whiteout::storages::mpq::Storage> mpqStorages;
#endif

    void Discover() {
        auto games = whiteout::utils::findBlizzardGames();

        std::string fallbackPath;
        for (auto& info : games) {
            if (info.game != whiteout::utils::BlizzardGame::WarcraftIII &&
                info.game != whiteout::utils::BlizzardGame::WarcraftIIIReforged)
                continue;

            if (fs::exists(FsPathFromUtf8(info.path) / "Data")) {
                wc3Path = info.path;
                break;
            }
            if (fallbackPath.empty())
                fallbackPath = info.path;
        }
        if (wc3Path.empty())
            wc3Path = std::move(fallbackPath);

        installPath = wc3Path;

        if (wc3Path.empty()) {
            std::printf("[FileContentProvider] Warcraft III installation not found.\n");
            return;
        }

        std::printf("[FileContentProvider] Found Warcraft III at: %s\n", wc3Path.c_str());

        TryOpenCasc();
        TryOpenMpq();
    }

    void TryOpenCasc() {
#if WHITEOUT_HAS_CASC
        cascStorage.reset();
        if (ignoreCasc || installPath.empty())
            return;
        std::string error;
        cascStorage = whiteout::storages::casc::Storage::open(installPath, &error);
        if (cascStorage) {
            std::printf("[FileContentProvider] CASC storage opened: %s\n", installPath.c_str());
        } else {
            std::printf("[FileContentProvider] CASC not available at '%s': %s\n",
                        installPath.c_str(), error.c_str());
            cascStorage.reset();
        }
#endif
    }

    void TryOpenMpq() {
#if WHITEOUT_HAS_MPQ
        mpqStorages.clear();
        if (ignoreMpq || installPath.empty())
            return;
        for (const std::string& name : mpqList) {
            if (name.empty())
                continue;

            fs::path mpqFsPath = FsPathFromUtf8(installPath) / name;
            if (!fs::exists(mpqFsPath)) {
                std::printf("[FileContentProvider] MPQ not found, skipping: %s\n",
                            PathToUtf8(mpqFsPath).c_str());
                continue;
            }

            std::string error;
            auto storage = whiteout::storages::mpq::Storage::open(PathToUtf8(mpqFsPath), &error);
            if (storage) {
                std::printf("[FileContentProvider] Opened MPQ: %s\n", PathToUtf8(mpqFsPath).c_str());
                mpqStorages.push_back(std::move(*storage));
            } else {
                std::printf("[FileContentProvider] Failed to open %s: %s\n",
                            PathToUtf8(mpqFsPath).c_str(), error.c_str());
            }
        }
#endif
    }
};

FileContentProvider::FileContentProvider() : impl_(std::make_unique<Impl>()) {
    impl_->Discover();

    // Surface the directory containing the host executable as a secondary
    // lookup root. The model-specific basePath is set later via
    // SetBasePath(); engine-shipped assets like the v1.8 / v1.14 BLS
    // bundles staged next to the exe by the build are reached through
    // this fallback.
    const fs::path exeDir = DiscoverExecutableDirectory();
    if (!exeDir.empty()) {
        impl_->resolver.SetSystemBasePath(exeDir);
        std::printf("[FileContentProvider] Executable dir: %s\n", PathToUtf8(exeDir).c_str());
    }
}

FileContentProvider::~FileContentProvider() = default;

FileContentProvider::FileContentProvider(FileContentProvider&&) noexcept = default;
FileContentProvider& FileContentProvider::operator=(FileContentProvider&&) noexcept = default;

void FileContentProvider::SetBasePath(const std::filesystem::path& basePath) {
    impl_->resolver.SetBasePath(basePath);
}

static constexpr const char* kTextureExts[] = {
    ".blp", ".dds", ".tga", ".png", ".tif",
};

static constexpr const char* kModelExts[] = {
    ".mdx",
    ".mdl",
};

static bool HasExtension(const std::string& ext, const char* const* list, usize count) {
    for (usize i = 0; i < count; ++i)
        if (ext == list[i])
            return true;
    return false;
}

static std::optional<std::vector<u8>> ReadDiskFile(const fs::path& resolved) {
    if (resolved.empty())
        return std::nullopt;
    std::ifstream file(resolved, std::ios::binary | std::ios::ate);
    if (!file)
        return std::nullopt;
    auto size = file.tellg();
    if (size <= 0)
        return std::nullopt;
    std::vector<u8> buf(static_cast<usize>(size));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(buf.data()), size);
    return buf;
}

std::optional<std::vector<u8>> FileContentProvider::ReadFile(const std::string& path,
                                                             std::string* actualExt) const {

    {
        std::string norm = FileResolver::NormalizeSeparators(path);
        std::string ext = fs::path(norm).extension().string();

        for (auto& c : ext)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        fs::path resolved;
        if (HasExtension(ext, kTextureExts, std::size(kTextureExts)))
            resolved = impl_->resolver.ResolveTexture(norm);
        else if (HasExtension(ext, kModelExts, std::size(kModelExts)))
            resolved = impl_->resolver.ResolveModel(norm);
        else
            resolved = impl_->resolver.Resolve(norm, {});

        auto data = ReadDiskFile(resolved);
        if (data) {
            if (actualExt) {
                std::string e = resolved.extension().string();
                for (auto& c : e)
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                *actualExt = e;
            }
            return data;
        }
    }

#if WHITEOUT_HAS_CASC
    if (impl_->cascStorage) {
        auto data = ReadFromCasc(path, actualExt);
        if (data)
            return data;
    }
#endif

#if WHITEOUT_HAS_MPQ
    if (!impl_->mpqStorages.empty()) {
        auto data = ReadFromMpq(path, actualExt);
        if (data)
            return data;
    }
#endif

    return std::nullopt;
}

static constexpr const char* kCascPrefixes[] = {
    "war3.w3mod:",
    "war3.w3mod:_hd.w3mod:",
    "war3.w3mod:_deprecated.w3mod:",
};

static constexpr const char* kArchiveTextureExts[] = {".blp", ".dds", ".tga", ".png"};
static constexpr const char* kArchiveModelExts[] = {".mdx", ".mdl"};

static std::string NormalizeCascPath(const std::string& relPath) {
    std::string out;
    out.reserve(relPath.size());
    for (char c : relPath) {
        if (c == '/')
            c = '\\';
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    usize start = 0;
    while (start < out.size() && (out[start] == '\\' || out[start] == '/'))
        ++start;
    if (start > 0)
        out.erase(0, start);
    return out;
}

static std::string StripExtension(const std::string& path) {
    auto dot = path.rfind('.');
    return (dot != std::string::npos) ? path.substr(0, dot) : path;
}

static std::string GetLowerExtension(const std::string& relPath) {
    std::string ext = fs::path(relPath).extension().string();
    for (auto& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

static std::pair<const char* const*, usize> AltExtensionsFor(const std::string& ext) {
    if (HasExtension(ext, kTextureExts, std::size(kTextureExts)))
        return {kArchiveTextureExts, std::size(kArchiveTextureExts)};
    if (HasExtension(ext, kModelExts, std::size(kModelExts)))
        return {kArchiveModelExts, std::size(kArchiveModelExts)};
    return {nullptr, 0};
}

std::optional<std::vector<u8>> FileContentProvider::ReadFromCasc(const std::string& path,
                                                                 std::string* actualExt) const {
#if WHITEOUT_HAS_CASC
    if (!impl_->cascStorage)
        return std::nullopt;

    std::string norm = NormalizeCascPath(path);
    std::string stem = StripExtension(norm);
    std::string ext = GetLowerExtension(norm);

    auto [altExts, altCount] = AltExtensionsFor(ext);

    for (const char* prefix : kCascPrefixes) {

        if (!ext.empty()) {
            std::string cascPath = std::string(prefix) + stem + ext;
            auto data = impl_->cascStorage->readFile(cascPath);
            if (data && !data->empty()) {
                if (actualExt)
                    *actualExt = ext;
                return data;
            }
        }

        for (usize i = 0; i < altCount; ++i) {
            if (altExts[i] == ext)
                continue;
            std::string cascPath = std::string(prefix) + stem + altExts[i];
            auto data = impl_->cascStorage->readFile(cascPath);
            if (data && !data->empty()) {
                if (actualExt)
                    *actualExt = altExts[i];
                return data;
            }
        }
    }
#endif
    return std::nullopt;
}

std::optional<std::vector<u8>> FileContentProvider::ReadFromMpq(const std::string& path,
                                                                std::string* actualExt) const {
#if WHITEOUT_HAS_MPQ
    if (impl_->mpqStorages.empty())
        return std::nullopt;

    const std::string& raw = path;
    std::string ext = GetLowerExtension(raw);
    std::string stem = StripExtension(raw);

    auto [altExts, altCount] = AltExtensionsFor(ext);

    for (const auto& mpq : impl_->mpqStorages) {

        if (!ext.empty()) {
            auto data = mpq.readFile(raw);
            if (data && !data->empty()) {
                if (actualExt)
                    *actualExt = ext;
                return data;
            }
        }

        for (usize i = 0; i < altCount; ++i) {
            if (altExts[i] == ext)
                continue;
            auto data = mpq.readFile(stem + altExts[i]);
            if (data && !data->empty()) {
                if (actualExt)
                    *actualExt = altExts[i];
                return data;
            }
        }
    }
#endif
    return std::nullopt;
}

bool FileContentProvider::HasCasc() const {
#if WHITEOUT_HAS_CASC
    return impl_->cascStorage.has_value();
#else
    return false;
#endif
}

bool FileContentProvider::HasMpq() const {
#if WHITEOUT_HAS_MPQ
    return !impl_->mpqStorages.empty();
#else
    return false;
#endif
}

const std::string& FileContentProvider::Wc3Path() const {
    return impl_->wc3Path;
}

const std::string& FileContentProvider::InstallPath() const {
    return impl_->installPath;
}

void FileContentProvider::SetInstallPath(const std::string& path) {
    impl_->installPath = path.empty() ? impl_->wc3Path : path;
    impl_->TryOpenCasc();
    impl_->TryOpenMpq();
}

bool FileContentProvider::IgnoreCasc() const {
    return impl_->ignoreCasc;
}

bool FileContentProvider::IgnoreMpq() const {
    return impl_->ignoreMpq;
}

void FileContentProvider::SetIgnoreCasc(bool ignore) {
    if (impl_->ignoreCasc == ignore)
        return;
    impl_->ignoreCasc = ignore;
    impl_->TryOpenCasc();
}

void FileContentProvider::SetIgnoreMpq(bool ignore) {
    if (impl_->ignoreMpq == ignore)
        return;
    impl_->ignoreMpq = ignore;
    impl_->TryOpenMpq();
}

const std::vector<std::string>& FileContentProvider::MpqList() const {
    return impl_->mpqList;
}

void FileContentProvider::SetMpqList(std::vector<std::string> list) {
    impl_->mpqList = std::move(list);
    impl_->TryOpenMpq();
}

std::vector<std::string> FileContentProvider::DefaultMpqList() {
    std::vector<std::string> out;
    out.reserve(std::size(kDefaultMpqNames));
    for (const char* n : kDefaultMpqNames)
        out.emplace_back(n);
    return out;
}

} // namespace whiteout::flakes::io
