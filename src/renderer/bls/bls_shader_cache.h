#pragma once

#include "bls_container.h"
#include "gfx/gfx.h"
#include "whiteout/flakes/content_provider.h"
#include "whiteout/flakes/types.h"

#include <array>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace whiteout::flakes::renderer::bls {

struct BlsShader {
    std::string name;
    gfx::ShaderStage stage;
    BlsContainer container;
    std::vector<gfx::ShaderHandle> permuteHandles;
    std::vector<PermuteHeader> permuteHeaders;
    u32 refs = 0;

    usize PermuteCount() const {
        return permuteHandles.size();
    }
};

class BlsShaderCache {
public:
    // `useDebugShaders` is wired from RenderSettings::GraphicsDebug(): when
    // set, Acquire looks under debug_shaders/ first (the -g2 -O0 BLS bundles
    // staged by the WDX_BUILD_WC3_DEBUG_SHADERS pipeline) and falls back to
    // the optimised shaders/ tree if a debug bundle is missing — so a build
    // configured with WDX_BUILD_WC3_DEBUG_SHADERS=OFF still works.
    BlsShaderCache(gfx::IGFXDevice* device, io::IContentProvider* contentProvider, gfx::GfxApi api,
                   bool useDebugShaders = false);
    ~BlsShaderCache();

    BlsShader* Acquire(gfx::ShaderStage stage, const std::string& name);
    void Release(BlsShader* shader);
    void ReleaseAll();

private:
    static constexpr usize kStageCount = 3;

    gfx::IGFXDevice* device_ = nullptr;
    io::IContentProvider* contentProvider_ = nullptr;
    gfx::GfxApi api_ = gfx::GfxApi::D3D11;

    // Content-root folders to search, in order. {"shaders"} normally;
    // {"debug_shaders", "shaders"} when useDebugShaders is set.
    std::vector<std::string> roots_;

    std::array<std::unordered_map<std::string, std::unique_ptr<BlsShader>>, kStageCount> byStage_;

    static const char* StagePrefix(gfx::ShaderStage stage);
    static std::string Lowercase(const std::string& in);
};

} // namespace whiteout::flakes::renderer::bls
