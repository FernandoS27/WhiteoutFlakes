#pragma once

#include <cornflakes/interface/asset/effect_asset_model.hpp>
#include <cornflakes/interface/core/arena.hpp>
#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/interface/diagnostics/issue.hpp>
#include <cornflakes/interface/service/service_types.hpp>

#include <memory>
#include <optional>
#include <vector>

namespace whiteout::cornflakes {

class IAssetReader {
public:
    static constexpr i32 kPriorityPkb = 1000;
    static constexpr i32 kPriorityTextPkfx = 10;

    IAssetReader() = default;
    virtual ~IAssetReader() = default;

    IAssetReader(const IAssetReader&) = delete;
    IAssetReader& operator=(const IAssetReader&) = delete;
    IAssetReader(IAssetReader&&) = delete;
    IAssetReader& operator=(IAssetReader&&) = delete;

    virtual i32 priority() const noexcept = 0;
    virtual bool canHandle(const BakedSource& src) const noexcept = 0;
    virtual std::optional<EffectAssetModel> read(const BakedSource& src, IArena& arena,
                                                 IssueBag& issues) = 0;
};

class SerializerPriorityDispatcher {
public:
    SerializerPriorityDispatcher();

    void addReader(std::unique_ptr<IAssetReader> reader);

    std::optional<EffectAssetModel> read(const BakedSource& src, IArena& arena, IssueBag& issues);

private:
    std::vector<std::unique_ptr<IAssetReader>> m_readers;
};

}
