#include "bls_shader_cache.h"

#include <cctype>
#include <cstdio>

namespace WhiteoutDex::bls {

BlsShaderCache::BlsShaderCache(gfx::IGFXDevice* device, IContentProvider* contentProvider)
    : device_(device), contentProvider_(contentProvider) {}

BlsShaderCache::~BlsShaderCache() {
    ReleaseAll();
}

const char* BlsShaderCache::StagePrefix(gfx::ShaderStage stage) {
    switch (stage) {
        case gfx::ShaderStage::Vertex:  return "vs";
        case gfx::ShaderStage::Pixel:   return "ps";
        case gfx::ShaderStage::Compute: return "cs";
    }
    return "vs";
}

std::string BlsShaderCache::Lowercase(const std::string& in) {
    std::string out = in;
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

BlsShader* BlsShaderCache::Acquire(gfx::ShaderStage stage, const std::string& name) {
    if (!device_ || !contentProvider_) return nullptr;

    auto& bucket = byStage_[static_cast<usize>(stage)];
    const std::string key = Lowercase(name);

    if (auto it = bucket.find(key); it != bucket.end()) {
        it->second->refs += 1;
        return it->second.get();
    }

    const std::string path = std::string("Shaders/") + StagePrefix(stage) + "/" + key + ".bls";
    auto bytes = contentProvider_->ReadFile(path);
    if (!bytes || bytes->empty()) {
        std::fprintf(stderr,
                     "[bls] ERR: shader read FAIL '%s'\n",
                     path.c_str());
        return nullptr;
    }

    auto entry = std::make_unique<BlsShader>();
    entry->name  = key;
    entry->stage = stage;

    std::string err;
    if (!entry->container.Load(*bytes, &err)) {
        std::fprintf(stderr,
                     "[bls] ERR: shader parse FAIL '%s': %s\n",
                     path.c_str(), err.c_str());
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
    if (!shader) return;
    if (shader->refs == 0) return;
    shader->refs -= 1;
    if (shader->refs > 0) return;

    auto& bucket = byStage_[static_cast<usize>(shader->stage)];
    auto it = bucket.find(shader->name);
    if (it == bucket.end()) return;

    for (gfx::ShaderHandle h : it->second->permuteHandles) {
        device_->Destroy(h);
    }
    bucket.erase(it);
}

void BlsShaderCache::ReleaseAll() {
    if (!device_) { for (auto& b : byStage_) b.clear(); return; }
    for (auto& bucket : byStage_) {
        for (auto& [name, entry] : bucket) {
            for (gfx::ShaderHandle h : entry->permuteHandles) {
                device_->Destroy(h);
            }
        }
        bucket.clear();
    }
}

}
