#pragma once

/// @file
/// @brief Asset reader for the text `.pkfx` format (editor / decompiled source).

#include <cornflakes/interface/asset/asset_reader.hpp>

namespace whiteout::cornflakes {

/// @brief Concrete `IAssetReader` for `.pkfx` (text) assets.
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

} // namespace whiteout::cornflakes
