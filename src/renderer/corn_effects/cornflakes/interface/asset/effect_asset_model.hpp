#pragma once

/// @file
/// @brief Reader-agnostic asset model: typed objects + raw fields produced by every IAssetReader.

#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/interface/service/service_types.hpp>

#include <span>
#include <string_view>

namespace whiteout::cornflakes {

/// @brief Which tool produced the asset; baker output and editor output differ in some quirks.
enum class BakerGenerator : u8 {
    Editor = 0,
    Baker = 1,
};

/// @brief Asset format version; `revisionId` is opaque and used only for issue context.
struct AssetVersion {
    u16 major = 0;
    u16 minor = 0;
    u16 patch = 0;
    u32 revisionId = 0;
};

/// @brief One field of an `AssetObject`, decoded just enough to dispatch by type.
struct FieldRaw {
    std::string_view name;
    std::string_view type;
    std::span<const std::byte> bytes;
    std::string_view stringValue;
};

/// @brief One handler instance from the asset (effect, layer, blob, sampler, ...).
struct AssetObject {
    std::string_view type;
    std::string_view uid;
    std::string_view customName;
    std::span<const FieldRaw> fields;
};

/// @brief Top-level deserialised asset; spans alias into the arena passed to the reader.
struct EffectAssetModel {
    AssetFormat format = AssetFormat::Pkb;
    AssetVersion version;
    BakerGenerator generator = BakerGenerator::Editor;

    std::string_view rootEffectUid;

    std::string_view rootLayerUid;

    std::span<const AssetObject> objects;
};

} // namespace whiteout::cornflakes
