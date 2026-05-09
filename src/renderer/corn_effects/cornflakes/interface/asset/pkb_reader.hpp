#pragma once

/// @file
/// @brief Asset reader for the binary `.pkb` format produced by the CornFx baker.

#include <cornflakes/interface/asset/asset_reader.hpp>
#include <cornflakes/interface/core/types.hpp>

#include <array>
#include <cstddef>

namespace whiteout::cornflakes {

/// @brief Concrete `IAssetReader` for `.pkb` (binary) assets.
class PkbReader final : public IAssetReader {
public:
    static constexpr std::array<u8, 3> kMagicPrefix{0x11U, 0x0BU, 0x00U}; ///< First 3 magic bytes.
    static constexpr u8 kMagicCurrentVersion = 0xCAU;                    ///< Accepted version byte.
    static constexpr std::array<u8, 5> kMagicRejectedVersions{0xC9U, 0xC8U, 0xA9U, 0x69U, 0x45U};

    static constexpr std::size_t kGeneratorByteOffset = 7U;
    static constexpr std::size_t kHeaderBytes = 28U;

    PkbReader() = default;

    i32 priority() const noexcept override {
        return kPriorityPkb;
    }

    bool canHandle(const BakedSource& src) const noexcept override;
    std::optional<EffectAssetModel> read(const BakedSource& src, IArena& arena,
                                         IssueBag& issues) override;
};

} // namespace whiteout::cornflakes
