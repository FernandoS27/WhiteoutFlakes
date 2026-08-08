#pragma once

#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/interface/service/service_types.hpp>

#include <span>
#include <string_view>

namespace whiteout::cornflakes {

enum class BakerGenerator : u8 {
    Editor = 0,
    Baker = 1,
};

struct AssetVersion {
    u16 major = 0;
    u16 minor = 0;
    u16 patch = 0;
    u32 revisionId = 0;
};

struct FieldRaw {
    std::string_view name;
    std::string_view type;
    std::span<const std::byte> bytes;
    std::string_view stringValue;
    std::span<const std::string_view> stringValues;
};

struct AssetObject {
    std::string_view type;
    std::string_view uid;
    std::string_view customName;
    std::span<const FieldRaw> fields;
};

struct EffectAssetModel {
    AssetFormat format = AssetFormat::Pkb;
    AssetVersion version;
    BakerGenerator generator = BakerGenerator::Editor;

    std::string_view rootEffectUid;

    std::string_view rootLayerUid;

    std::span<const AssetObject> objects;
};

}
