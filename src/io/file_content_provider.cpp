#include "io/file_content_provider.h"
#include "common_types.h"
#include "file_resolver.h"
#include "io/path_utf8.h"

#include <whiteout/utils/blizzard_game_finder.h>

#if WHITEOUT_HAS_CASC
#include <whiteout/storages/casc/storage.h>
#endif

#if WHITEOUT_HAS_MPQ
#include <whiteout/storages/mpq/storage.h>
#endif

#include <filesystem>
#include <fstream>
#include <cstdio>

namespace WhiteoutDex {

namespace fs = std::filesystem;

static constexpr const char* kMpqNames[] = {
    "War3Patch.mpq",
    "War3x.mpq",
    "war3.mpq",
};

struct FileContentProvider::Impl {
    std::string wc3Path;
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
        std::string error;
        cascStorage = whiteout::storages::casc::Storage::open(wc3Path, &error);
        if (cascStorage) {
            std::printf("[FileContentProvider] CASC storage opened.\n");
        } else {
            std::printf("[FileContentProvider] CASC not available: %s\n", error.c_str());
            cascStorage.reset();
        }
#endif
    }

    void TryOpenMpq() {
#if WHITEOUT_HAS_MPQ
        for (const char* name : kMpqNames) {

            fs::path mpqPath = FsPathFromUtf8(wc3Path) / name;
            if (!fs::exists(mpqPath))
                continue;

            std::string error;
            auto storage = whiteout::storages::mpq::Storage::open(
                PathToUtf8(mpqPath), &error);
            if (storage) {
                std::printf("[FileContentProvider] Opened MPQ: %s\n", name);
                mpqStorages.push_back(std::move(*storage));
            } else {
                std::printf("[FileContentProvider] Failed to open %s: %s\n",
                            name, error.c_str());
            }
        }
#endif
    }
};

FileContentProvider::FileContentProvider()
    : impl_(std::make_unique<Impl>()) {
    impl_->Discover();
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
    ".mdx", ".mdl",
};

static bool HasExtension(const std::string& ext, const char* const* list, usize count) {
    for (usize i = 0; i < count; ++i)
        if (ext == list[i]) return true;
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

std::optional<std::vector<u8>> FileContentProvider::ReadFile(
    const std::string& path, std::string* actualExt) const {

    {
        std::string norm = FileResolver::NormalizeSeparators(path);
        std::string ext = fs::path(norm).extension().string();

        for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

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
                for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
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
static constexpr const char* kArchiveModelExts[]   = {".mdx", ".mdl"};

static std::string NormalizeCascPath(const std::string& relPath) {
    std::string out;
    out.reserve(relPath.size());
    for (char c : relPath) {
        if (c == '/') c = '\\';
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
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

static std::pair<const char* const*, usize> AltExtensionsFor(const std::string& ext) {
    if (HasExtension(ext, kTextureExts, std::size(kTextureExts)))
        return {kArchiveTextureExts, std::size(kArchiveTextureExts)};
    if (HasExtension(ext, kModelExts, std::size(kModelExts)))
        return {kArchiveModelExts, std::size(kArchiveModelExts)};
    return {nullptr, 0};
}

std::optional<std::vector<u8>> FileContentProvider::ReadFromCasc(
    const std::string& path, std::string* actualExt) const {
#if WHITEOUT_HAS_CASC
    if (!impl_->cascStorage)
        return std::nullopt;

    std::string norm = NormalizeCascPath(path);
    std::string stem = StripExtension(norm);
    std::string ext  = GetLowerExtension(norm);

    auto [altExts, altCount] = AltExtensionsFor(ext);

    for (const char* prefix : kCascPrefixes) {

        if (!ext.empty()) {
            std::string cascPath = std::string(prefix) + stem + ext;
            auto data = impl_->cascStorage->readFile(cascPath);
            if (data && !data->empty()) {
                if (actualExt) *actualExt = ext;
                return data;
            }
        }

        for (usize i = 0; i < altCount; ++i) {
            if (altExts[i] == ext) continue;
            std::string cascPath = std::string(prefix) + stem + altExts[i];
            auto data = impl_->cascStorage->readFile(cascPath);
            if (data && !data->empty()) {
                if (actualExt) *actualExt = altExts[i];
                return data;
            }
        }
    }
#endif
    return std::nullopt;
}

std::optional<std::vector<u8>> FileContentProvider::ReadFromMpq(
    const std::string& path, std::string* actualExt) const {
#if WHITEOUT_HAS_MPQ
    if (impl_->mpqStorages.empty())
        return std::nullopt;

    const std::string& raw = path;
    std::string ext  = GetLowerExtension(raw);
    std::string stem = StripExtension(raw);

    auto [altExts, altCount] = AltExtensionsFor(ext);

    for (const auto& mpq : impl_->mpqStorages) {

        if (!ext.empty()) {
            auto data = mpq.readFile(raw);
            if (data && !data->empty()) {
                if (actualExt) *actualExt = ext;
                return data;
            }
        }

        for (usize i = 0; i < altCount; ++i) {
            if (altExts[i] == ext) continue;
            auto data = mpq.readFile(stem + altExts[i]);
            if (data && !data->empty()) {
                if (actualExt) *actualExt = altExts[i];
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

}
