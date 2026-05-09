#include <cornflakes/core/determinism.hpp>
#include <cornflakes/diagnostics/issue_codes.hpp>
#include <cornflakes/events/payload_builder.hpp>
#include <cornflakes/events/payload_layout.hpp>

#include <cstring>

namespace whiteout::cornflakes {

namespace {

Issue eventsFatal(u32 code, std::string_view message) noexcept {
    Issue issue;
    issue.severity = Severity::Fatal;
    issue.category = Category::Events;
    issue.code = code;
    issue.message = message;
    return issue;
}

bool boundsOk(std::size_t blobSize, u32 offset, std::size_t bytes, IssueBag& issues) noexcept {
    if (static_cast<std::size_t>(offset) + bytes > blobSize) {
        issues.push(eventsFatal(issues::events::kPayloadOob,
                                "Payload read/write: offset + size exceeds blob bounds"));
        return false;
    }
    return true;
}

} // namespace

std::span<std::byte> PayloadInitializer::initialize(IArena& arena, u32 elementCount) const {
    const std::size_t bytes = computePayloadCacheByteCount(elementCount);
    const auto blob = arenaArray<std::byte>(arena, bytes);

    return blob;
}

bool PayloadElementBuilder::writeU32(std::span<std::byte> blob, u32 offset, u32 value,
                                     IssueBag& issues) const {
    if (!boundsOk(blob.size(), offset, sizeof(value), issues)) {
        return false;
    }
    std::memcpy(blob.data() + offset, &value, sizeof(value));
    return true;
}

bool PayloadElementBuilder::writeF32(std::span<std::byte> blob, u32 offset, f32 value,
                                     IssueBag& issues) const {
    if (!boundsOk(blob.size(), offset, sizeof(value), issues)) {
        return false;
    }
    std::memcpy(blob.data() + offset, &value, sizeof(value));
    return true;
}

bool PayloadElementBuilder::writeFloat3(std::span<std::byte> blob, u32 offset, Float3 value,
                                        IssueBag& issues) const {
    if (!boundsOk(blob.size(), offset, sizeof(value), issues)) {
        return false;
    }
    std::memcpy(blob.data() + offset, &value, sizeof(value));
    return true;
}

std::optional<u32> PayloadElementExtractor::readU32(std::span<const std::byte> blob, u32 offset,
                                                    IssueBag& issues) const {
    if (!boundsOk(blob.size(), offset, sizeof(u32), issues)) {
        return std::nullopt;
    }
    u32 value = 0;
    std::memcpy(&value, blob.data() + offset, sizeof(value));
    return value;
}

std::optional<f32> PayloadElementExtractor::readF32(std::span<const std::byte> blob, u32 offset,
                                                    IssueBag& issues) const {
    if (!boundsOk(blob.size(), offset, sizeof(f32), issues)) {
        return std::nullopt;
    }
    f32 value = 0.0F;
    std::memcpy(&value, blob.data() + offset, sizeof(value));
    return value;
}

std::optional<Float3> PayloadElementExtractor::readFloat3(std::span<const std::byte> blob,
                                                          u32 offset, IssueBag& issues) const {
    if (!boundsOk(blob.size(), offset, sizeof(Float3), issues)) {
        return std::nullopt;
    }
    Float3 value{};
    std::memcpy(&value, blob.data() + offset, sizeof(value));
    return value;
}

} // namespace whiteout::cornflakes
