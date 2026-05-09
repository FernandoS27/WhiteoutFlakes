#pragma once

/// @file
/// @brief Typed accessors over `AssetObject::fields` plus blob-format / link-table parsers.

#include <cornflakes/interface/asset/effect_asset_model.hpp>
#include <cornflakes/interface/core/types.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

namespace whiteout::cornflakes {

const FieldRaw* findField(const AssetObject& obj, std::string_view name) noexcept;

std::optional<bool> fieldBool(const AssetObject& obj, std::string_view name) noexcept;
std::optional<i32> fieldInt(const AssetObject& obj, std::string_view name) noexcept;
std::optional<u32> fieldUint(const AssetObject& obj, std::string_view name) noexcept;
std::optional<f32> fieldFloat(const AssetObject& obj, std::string_view name) noexcept;

std::optional<u32> fieldLink(const AssetObject& obj, std::string_view name) noexcept;

std::span<const u32> fieldLinks(const AssetObject& obj, std::string_view name) noexcept;

std::span<const f32> fieldFloatArray(const AssetObject& obj, std::string_view name) noexcept;

std::span<const u32> fieldUintArray(const AssetObject& obj, std::string_view name) noexcept;

std::string_view fieldString(const AssetObject& obj, std::string_view name) noexcept;

std::span<const std::byte> fieldBytes(const AssetObject& obj, std::string_view name) noexcept;

const AssetObject* findObjectByUid(const EffectAssetModel& model, u32 uid) noexcept;

std::span<const std::byte> blobBytecode(const AssetObject& blob) noexcept;

/// @brief Parsed view of a Blob handler — header + constants + bytecode.
struct BlobView {
    u32 reserved0 = 0;
    u32 reserved1 = 0;
    u32 constStorageBytes = 0;
    u32 bytecodeBytes = 0;
    std::array<u32, 5> registerCounts{};
    std::span<const std::byte> constants;
    std::span<const u8> bytecode;
};

std::optional<BlobView> parseBlob(const AssetObject& blob) noexcept;

/// @brief Decoded view of one external-link entry referenced from a Blob.
struct ExternalLinkView {
    std::string_view name;
    std::string_view typeName;
    u32 nativeType = 0;
    u32 storageSize = 0;
    u32 metaType = 0;
    u32 attributes = 0;
    u32 accessMask = 0;
};

std::optional<ExternalLinkView> readExternalLink(const EffectAssetModel& model,
                                                 const AssetObject& blob, u32 slot) noexcept;

/// @brief Decoded view of one function-call link entry referenced from a Blob.
struct FunctionCallLinkView {
    std::string_view symbolName;
    u32 symbolSlot = 0;
    u32 traits = 0;
};

std::optional<FunctionCallLinkView> readFunctionCallLink(const EffectAssetModel& model,
                                                         const AssetObject& blob,
                                                         u32 slot) noexcept;

} // namespace whiteout::cornflakes
