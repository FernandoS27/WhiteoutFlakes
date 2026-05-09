#include <cornflakes/asset/object_accessors.hpp>
#include <cornflakes/core/determinism.hpp>

#include <cstring>

namespace whiteout::cornflakes {

namespace {

constexpr u32 kStringIndexInvalid = 0xFFFFFFFFU;

template <typename T>
std::optional<T> readScalar(std::span<const std::byte> bytes) noexcept {
    if (bytes.size() < sizeof(T)) {
        return std::nullopt;
    }
    T out{};
    std::memcpy(&out, bytes.data(), sizeof(T));
    return out;
}

} // namespace

const FieldRaw* findField(const AssetObject& obj, std::string_view name) noexcept {
    for (const auto& f : obj.fields) {
        if (f.name == name) {
            return &f;
        }
    }
    return nullptr;
}

std::optional<bool> fieldBool(const AssetObject& obj, std::string_view name) noexcept {
    const auto* f = findField(obj, name);
    if (f == nullptr || f->type != "bool" || f->bytes.empty()) {
        return std::nullopt;
    }
    return static_cast<u8>(f->bytes[0]) != 0U;
}

std::optional<i32> fieldInt(const AssetObject& obj, std::string_view name) noexcept {
    const auto* f = findField(obj, name);
    if (f == nullptr || f->type != "int") {
        return std::nullopt;
    }
    return readScalar<i32>(f->bytes);
}

std::optional<u32> fieldUint(const AssetObject& obj, std::string_view name) noexcept {
    const auto* f = findField(obj, name);
    if (f == nullptr || f->type != "uint") {
        return std::nullopt;
    }
    return readScalar<u32>(f->bytes);
}

std::optional<f32> fieldFloat(const AssetObject& obj, std::string_view name) noexcept {
    const auto* f = findField(obj, name);
    if (f == nullptr || f->type != "float") {
        return std::nullopt;
    }
    return readScalar<f32>(f->bytes);
}

std::optional<u32> fieldLink(const AssetObject& obj, std::string_view name) noexcept {
    const auto* f = findField(obj, name);
    if (f == nullptr || f->type != "link") {
        return std::nullopt;
    }
    const auto raw = readScalar<u32>(f->bytes);
    if (!raw || *raw == kStringIndexInvalid || *raw == 0U) {
        return std::nullopt;
    }
    return raw;
}

std::span<const u32> fieldLinks(const AssetObject& obj, std::string_view name) noexcept {
    const auto* f = findField(obj, name);
    if (f == nullptr || f->type != "link[]" || f->bytes.size() < 4) {
        return {};
    }

    u32 count = 0;
    std::memcpy(&count, f->bytes.data(), sizeof(count));
    if (count == 0) {
        return {};
    }
    const std::size_t bodyBytes = static_cast<std::size_t>(count) * sizeof(u32);
    if (4U + bodyBytes > f->bytes.size()) {
        return {};
    }
    const auto* base = reinterpret_cast<const u32*>(f->bytes.data() + 4);
    return std::span<const u32>{base, count};
}

std::span<const f32> fieldFloatArray(const AssetObject& obj, std::string_view name) noexcept {
    const auto* f = findField(obj, name);
    if (f == nullptr || f->type != "float[]" || f->bytes.size() < 4) {
        return {};
    }
    u32 count = 0;
    std::memcpy(&count, f->bytes.data(), sizeof(count));
    if (count == 0) {
        return {};
    }
    const std::size_t bodyBytes = static_cast<std::size_t>(count) * sizeof(f32);
    if (4U + bodyBytes > f->bytes.size()) {
        return {};
    }
    const auto* base = reinterpret_cast<const f32*>(f->bytes.data() + 4);
    return std::span<const f32>{base, count};
}

std::span<const u32> fieldUintArray(const AssetObject& obj, std::string_view name) noexcept {
    const auto* f = findField(obj, name);

    if (f == nullptr || f->bytes.size() < 4) {
        return {};
    }
    if (f->type != "uint[]" && f->type != "int[]" && f->type != "u32[]" && f->type != "link[]") {
        return {};
    }
    u32 count = 0;
    std::memcpy(&count, f->bytes.data(), sizeof(count));
    if (count == 0) {
        return {};
    }
    const std::size_t bodyBytes = static_cast<std::size_t>(count) * sizeof(u32);
    if (4U + bodyBytes > f->bytes.size()) {
        return {};
    }
    const auto* base = reinterpret_cast<const u32*>(f->bytes.data() + 4);
    return std::span<const u32>{base, count};
}

std::string_view fieldString(const AssetObject& obj, std::string_view name) noexcept {
    const auto* f = findField(obj, name);
    if (f == nullptr) {
        return {};
    }
    if (f->type != "string" && f->type != "string_unicode") {
        return {};
    }
    return f->stringValue;
}

std::span<const std::byte> fieldBytes(const AssetObject& obj, std::string_view name) noexcept {
    const auto* f = findField(obj, name);
    if (f == nullptr) {
        return {};
    }
    return f->bytes;
}

const AssetObject* findObjectByUid(const EffectAssetModel& model, u32 uid) noexcept {

    if (uid == 0U || uid == kStringIndexInvalid) {
        return nullptr;
    }
    const std::size_t idx = static_cast<std::size_t>(uid) - 1U;
    if (idx >= model.objects.size()) {
        return nullptr;
    }
    return &model.objects[idx];
}

std::optional<BlobView> parseBlob(const AssetObject& blob) noexcept {

    const auto body = blobBytecode(blob);
    if (body.size() < 36U) {
        return std::nullopt;
    }
    BlobView v;
    std::memcpy(&v.reserved0, body.data() + 0, sizeof(u32));
    std::memcpy(&v.reserved1, body.data() + 4, sizeof(u32));
    std::memcpy(&v.constStorageBytes, body.data() + 8, sizeof(u32));
    std::memcpy(&v.bytecodeBytes, body.data() + 12, sizeof(u32));
    for (std::size_t i = 0; i < 5; ++i) {
        std::memcpy(&v.registerCounts[i], body.data() + 16 + (i * sizeof(u32)), sizeof(u32));
    }
    const std::size_t constStart = 36U;
    const std::size_t bcStart = constStart + v.constStorageBytes;
    const std::size_t bcEnd = bcStart + v.bytecodeBytes;
    if (bcEnd > body.size()) {
        return std::nullopt;
    }
    v.constants = body.subspan(constStart, v.constStorageBytes);
    const auto bcSpan = body.subspan(bcStart, v.bytecodeBytes);
    v.bytecode = std::span<const u8>{reinterpret_cast<const u8*>(bcSpan.data()), bcSpan.size()};
    return v;
}

std::optional<FunctionCallLinkView> readFunctionCallLink(const EffectAssetModel& model,
                                                         const AssetObject& blob,
                                                         u32 slot) noexcept {
    const auto links = fieldLinks(blob, "ExternalCalls");
    if (slot >= links.size()) {
        return std::nullopt;
    }
    const AssetObject* fn = findObjectByUid(model, links[slot]);
    if (fn == nullptr || fn->type != "CCompilerBlobCacheFunctionDef") {
        return std::nullopt;
    }
    FunctionCallLinkView v;
    v.symbolName = fieldString(*fn, "SymbolName");
    v.symbolSlot = fieldUint(*fn, "SymbolSlot").value_or(0U);
    v.traits = fieldUint(*fn, "FunctionTraits").value_or(0U);
    return v;
}

std::optional<ExternalLinkView> readExternalLink(const EffectAssetModel& model,
                                                 const AssetObject& blob, u32 slot) noexcept {
    const auto extLinks = fieldLinks(blob, "Externals");
    if (slot >= extLinks.size()) {
        return std::nullopt;
    }
    const AssetObject* ext = findObjectByUid(model, extLinks[slot]);
    if (ext == nullptr || ext->type != "CCompilerBlobCacheExternal") {
        return std::nullopt;
    }
    ExternalLinkView v;
    v.name = fieldString(*ext, "NameGUID");
    v.typeName = fieldString(*ext, "TypeName");
    v.nativeType = fieldUint(*ext, "NativeType").value_or(0U);
    v.storageSize = fieldUint(*ext, "StorageSize").value_or(0U);
    v.metaType = fieldUint(*ext, "MetaType").value_or(0U);
    v.attributes = fieldUint(*ext, "Attributes").value_or(0U);
    v.accessMask = fieldUint(*ext, "AccessMask").value_or(0U);
    return v;
}

std::span<const std::byte> blobBytecode(const AssetObject& blob) noexcept {

    const auto raw = fieldBytes(blob, "Blob");
    if (raw.size() < 4) {
        return {};
    }
    u32 count = 0;
    std::memcpy(&count, raw.data(), sizeof(count));
    const std::size_t bodyBytes = static_cast<std::size_t>(count) * 4U;
    if (4U + bodyBytes > raw.size()) {
        return {};
    }
    return raw.subspan(4U, bodyBytes);
}

} // namespace whiteout::cornflakes
