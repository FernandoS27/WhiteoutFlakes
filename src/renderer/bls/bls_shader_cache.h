#pragma once

#include "common_types.h"
#include "bls_container.h"
#include "content_provider.h"
#include "gfx/gfx.h"

#include <array>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace WhiteoutDex::bls {

struct BlsShader {
    std::string                    name;
    gfx::ShaderStage               stage;
    BlsContainer                   container;
    std::vector<gfx::ShaderHandle> permuteHandles;
    std::vector<PermuteHeader>     permuteHeaders;
    u32                            refs = 0;

    usize PermuteCount() const { return permuteHandles.size(); }
};

class BlsShaderCache {
public:
    BlsShaderCache(gfx::IGFXDevice* device, IContentProvider* contentProvider);
    ~BlsShaderCache();

    BlsShader* Acquire(gfx::ShaderStage stage, const std::string& name);
    void       Release(BlsShader* shader);
    void       ReleaseAll();

private:
    static constexpr usize kStageCount = 3;

    gfx::IGFXDevice*  device_          = nullptr;
    IContentProvider* contentProvider_ = nullptr;

    std::array<std::unordered_map<std::string, std::unique_ptr<BlsShader>>, kStageCount> byStage_;

    static const char* StagePrefix(gfx::ShaderStage stage);
    static std::string Lowercase(const std::string& in);
};

}
