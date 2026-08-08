#pragma once

#include <cornflakes/interface/asset/effect_asset_model.hpp>
#include <cornflakes/interface/binding/effect_execution_plan.hpp>
#include <cornflakes/interface/core/arena.hpp>
#include <cornflakes/interface/diagnostics/issue.hpp>
#include <cornflakes/interface/schema/handles.hpp>

#include <optional>

namespace whiteout::cornflakes {

class IMeshResourceProvider;
class ITextureResourceProvider;
class IVectorFieldProvider;

class EffectBinder {
public:
    EffectBinder() = default;

    void setMeshProvider(IMeshResourceProvider* provider) noexcept {
        m_meshProvider = provider;
    }

    void setTextureProvider(ITextureResourceProvider* provider) noexcept {
        m_textureProvider = provider;
    }

    void setVectorFieldProvider(IVectorFieldProvider* provider) noexcept {
        m_vectorFieldProvider = provider;
    }

    std::optional<EffectExecutionPlan> bind(const EffectAssetModel& model, EffectId id,
                                            IArena& planArena, IssueBag& issues) const;

private:
    IMeshResourceProvider* m_meshProvider = nullptr;
    ITextureResourceProvider* m_textureProvider = nullptr;
    IVectorFieldProvider* m_vectorFieldProvider = nullptr;
};

}
