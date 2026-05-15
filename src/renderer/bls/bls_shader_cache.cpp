#include "bls_shader_cache.h"

#include <cctype>
#include <cstdio>
#include <optional>

namespace whiteout::flakes::renderer::bls {

using namespace ::whiteout::flakes::io;

BlsShaderCache::BlsShaderCache(gfx::IGFXDevice* device, IContentProvider* contentProvider,
                               gfx::GfxApi api, bool useDebugShaders)
    : device_(device), contentProvider_(contentProvider), api_(api) {
    // Debug builds prefer the -g2 -O0 bundles but fall back to the optimised
    // tree so a missing debug_shaders/ (WDX_BUILD_WC3_DEBUG_SHADERS=OFF)
    // doesn't break the run.
    if (useDebugShaders)
        roots_ = {"debug_shaders", "shaders"};
    else
        roots_ = {"shaders"};
}

BlsShaderCache::~BlsShaderCache() {
    ReleaseAll();
}

const char* BlsShaderCache::StagePrefix(gfx::ShaderStage stage) {
    switch (stage) {
    case gfx::ShaderStage::Vertex:
        return "vs";
    case gfx::ShaderStage::Pixel:
        return "ps";
    case gfx::ShaderStage::Compute:
        return "cs";
    }
    return "vs";
}

std::string BlsShaderCache::Lowercase(const std::string& in) {
    std::string out = in;
    for (char& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

BlsShader* BlsShaderCache::Acquire(gfx::ShaderStage stage, const std::string& name) {
    if (!device_ || !contentProvider_)
        return nullptr;

    auto& bucket = byStage_[static_cast<usize>(stage)];
    const std::string key = Lowercase(name);

    if (auto it = bucket.find(key); it != bucket.end()) {
        it->second->refs += 1;
        return it->second.get();
    }

    // Per-API lookup, with a d3d11-shaders fallback for d3d12. The
    // D3D12 runtime accepts SM5 DXBC bytecode just fine, so when a
    // d3d12-specific (DXIL SM6) bundle isn't packed we transparently
    // pick up the D3D11 (or game-shipped) DXBC bundle at the unprefixed
    // path. Vulkan can't fall back: SPIR-V and DXBC aren't
    // interchangeable.
    //   D3D11 → DXBC under <root>/<stage>/
    //   D3D12 → DXIL under <root>/d3d12/<stage>/ → fallback <root>/<stage>/
    //   Vulkan → SPIR-V under <root>/vulkan/<stage>/
    // <root> is each entry of roots_ in order: debug_shaders/ before
    // shaders/ in graphics-debug builds, shaders/ alone otherwise.
    std::string path;
    std::optional<std::vector<u8>> bytes;
    auto tryRead = [&](const std::string& root, const char* apiSubdir) {
        path = root + "/" + apiSubdir + StagePrefix(stage) + "/" + key + ".bls";
        bytes = contentProvider_->ReadFile(path);
        return bytes && !bytes->empty();
    };
    auto tryRoot = [&](const std::string& root) {
        if (api_ == gfx::GfxApi::Vulkan)
            return tryRead(root, "vulkan/");
        if (api_ == gfx::GfxApi::D3D12)
            return tryRead(root, "d3d12/") || tryRead(root, "");
        return tryRead(root, ""); // D3D11
    };

    bool found = false;
    for (const std::string& root : roots_) {
        if (tryRoot(root)) {
            found = true;
            break;
        }
    }
    if (!found) {
        std::fprintf(stderr, "[bls] ERR: shader read FAIL '%s' (stage %s, last tried '%s')\n",
                     key.c_str(), StagePrefix(stage), path.c_str());
        return nullptr;
    }

    auto entry = std::make_unique<BlsShader>();
    entry->name = key;
    entry->stage = stage;

    std::string err;
    if (!entry->container.Load(*bytes, &err)) {
        std::fprintf(stderr, "[bls] ERR: shader parse FAIL '%s': %s\n", path.c_str(), err.c_str());
        return nullptr;
    }

    const usize count = entry->container.PermuteCount();
    entry->permuteHandles.reserve(count);
    entry->permuteHeaders.reserve(count);

    for (usize i = 0; i < count; ++i) {
        PermuteView view = entry->container.Permute(i);
        gfx::ShaderHandle h = device_->CreateShader(stage, view.dxbc.data(), view.dxbc.size());
        entry->permuteHandles.push_back(h);
        entry->permuteHeaders.push_back(view.header);
    }

    entry->refs = 1;
    BlsShader* raw = entry.get();
    bucket.emplace(key, std::move(entry));
    return raw;
}

void BlsShaderCache::Release(BlsShader* shader) {
    if (!shader)
        return;
    if (shader->refs == 0)
        return;
    shader->refs -= 1;
    if (shader->refs > 0)
        return;

    auto& bucket = byStage_[static_cast<usize>(shader->stage)];
    auto it = bucket.find(shader->name);
    if (it == bucket.end())
        return;

    for (gfx::ShaderHandle h : it->second->permuteHandles) {
        device_->Destroy(h);
    }
    bucket.erase(it);
}

void BlsShaderCache::ReleaseAll() {
    if (!device_) {
        for (auto& b : byStage_)
            b.clear();
        return;
    }
    for (auto& bucket : byStage_) {
        for (auto& [name, entry] : bucket) {
            for (gfx::ShaderHandle h : entry->permuteHandles) {
                device_->Destroy(h);
            }
        }
        bucket.clear();
    }
}

} // namespace whiteout::flakes::renderer::bls
