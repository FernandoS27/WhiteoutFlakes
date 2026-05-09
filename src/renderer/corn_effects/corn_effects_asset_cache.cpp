#include "renderer/corn_effects/corn_effects_asset_cache.h"

#include "whiteout/flakes/content_provider.h"

#include <cornflakes/interface/asset/effect_asset_model.hpp>
#include <cornflakes/interface/asset/pkb_reader.hpp>
#include <cornflakes/interface/diagnostics/issue.hpp>
#include <cornflakes/interface/service/service_types.hpp>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace whiteout::flakes::renderer::corn_effects {

namespace {

bool ReadFileFromDisk(const std::string& path, std::vector<std::byte>& out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const auto size = f.tellg();
    if (size < 0) return false;
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    f.read(reinterpret_cast<char*>(out.data()), size);
    return f.good() || f.eof();
}

std::string SwapExtension(const std::string& path, const char* newExt) {
    const auto dot = path.find_last_of('.');
    const auto sep = path.find_last_of("/\\");
    if (dot == std::string::npos || (sep != std::string::npos && dot < sep)) {
        return path + newExt;
    }
    return path.substr(0, dot) + newExt;
}

bool ReadViaProvider(io::IContentProvider* cp, const std::string& path,
                      std::vector<std::byte>& out) {
    auto bytesU8 = cp->ReadFile(path);
    if (!bytesU8.has_value()) return false;
    out.resize(bytesU8->size());
    std::memcpy(out.data(), bytesU8->data(), bytesU8->size());
    return true;
}

}

CornEffectsAssetCache::CornEffectsAssetCache()
    : arena_{1U << 20}
{
    dispatcher_.addReader(std::make_unique<::whiteout::cornflakes::PkbReader>());
}

CornEffectsAssetCache::~CornEffectsAssetCache() = default;

void CornEffectsAssetCache::SetContentProvider(io::IContentProvider* provider) {
    std::lock_guard<std::mutex> lock(mutex_);
    contentProvider_ = provider;
}

const ::whiteout::cornflakes::EffectAssetModel*
CornEffectsAssetCache::Acquire(const std::string& pkbPath,
                            ::whiteout::cornflakes::IssueBag& issues) {
    if (pkbPath.empty()) return nullptr;

    std::lock_guard<std::mutex> lock(mutex_);

    if (auto it = cache_.find(pkbPath); it != cache_.end()) {
        return it->second->model.get();
    }

    auto entry = std::make_unique<Entry>();

    std::string resolvedPath = pkbPath;
    auto tryRead = [&](const std::string& p) -> bool {
        if (contentProvider_ && ReadViaProvider(contentProvider_, p, entry->bytes)) {
            resolvedPath = p;
            return true;
        }
        if (!contentProvider_ && ReadFileFromDisk(p, entry->bytes)) {
            resolvedPath = p;
            return true;
        }
        return false;
    };

    bool gotBytes = tryRead(pkbPath);
    if (!gotBytes) {
        const std::string asPkb = SwapExtension(pkbPath, ".pkb");
        if (asPkb != pkbPath && tryRead(asPkb)) {
            gotBytes = true;
        }
    }
    if (!gotBytes) {
        const std::string asPkfx = SwapExtension(pkbPath, ".pkfx");
        if (asPkfx != pkbPath && tryRead(asPkfx)) {
            gotBytes = true;
        }
    }
    if (!gotBytes) {
        std::fprintf(stderr,
                     "[corn_fx] WARN: AssetCache unable to resolve '%s' (.pkb / .pkfx)\n",
                     pkbPath.c_str());
        return nullptr;
    }

    ::whiteout::cornflakes::BakedSource src;
    src.path  = resolvedPath;
    src.bytes = std::span<const std::byte>{entry->bytes.data(), entry->bytes.size()};

    auto model = dispatcher_.read(src, arena_, issues);
    if (!model.has_value() || issues.hasFatal()) {
        return nullptr;
    }

    entry->model = std::make_unique<::whiteout::cornflakes::EffectAssetModel>(*model);
    auto* raw    = entry->model.get();
    cache_.emplace(pkbPath, std::move(entry));
    return raw;
}

void CornEffectsAssetCache::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.clear();
}

}
