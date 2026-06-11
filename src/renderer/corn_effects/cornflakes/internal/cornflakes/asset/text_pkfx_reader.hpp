#pragma once

#include <cornflakes/interface/asset/asset_reader.hpp>

namespace whiteout::cornflakes {

class TextPkfxReader final : public IAssetReader {
public:
    TextPkfxReader() = default;

    i32 priority() const noexcept override {
        return kPriorityTextPkfx;
    }

    bool canHandle(const BakedSource& src) const noexcept override;
    std::optional<EffectAssetModel> read(const BakedSource& src, IArena& arena,
                                         IssueBag& issues) override;
};

}
