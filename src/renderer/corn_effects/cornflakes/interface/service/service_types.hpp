#pragma once

/// @file
/// @brief Plain value types exchanged across the service boundary.

#include <cornflakes/interface/core/types.hpp>

#include <cstddef>
#include <span>
#include <string_view>

namespace whiteout::cornflakes {

struct Float2 {
    f32 x = 0.0F;
    f32 y = 0.0F;
};

struct Float3 {
    f32 x = 0.0F;
    f32 y = 0.0F;
    f32 z = 0.0F;
};

struct Float4 {
    f32 x = 0.0F;
    f32 y = 0.0F;
    f32 z = 0.0F;
    f32 w = 0.0F;
};

/// @brief XYZW unit quaternion; identity is `(0, 0, 0, 1)`.
struct Quat {
    f32 x = 0.0F;
    f32 y = 0.0F;
    f32 z = 0.0F;
    f32 w = 1.0F;
};

/// @brief TRS triple used for emitter placement.
struct Transform {
    Float3 position;
    Quat orientation;
    Float3 scale{1.0F, 1.0F, 1.0F};
};

/// @brief A baked asset blob plus the path it came from (for issue reporting).
struct BakedSource {
    std::string_view path;
    std::span<const std::byte> bytes;
};

/// @brief Initial state passed to `createEmitter`.
struct EmitterInitState {
    Transform transform;
    bool enabled = true;
};

/// @brief Tagged-union attribute value pushed into bound externals via the service API.
struct AttributeValue {
    enum class Kind : u8 { Float, Float3, Bool, I32, U32 };
    Kind kind = Kind::Float;
    f32 asFloat = 0.0F;
    Float3 asFloat3;
    bool asBool = false;
    i32 asI32 = 0;
    u32 asU32 = 0;
};

/// @brief Opaque host-provided payload bytes for `pushEmitterEvent`.
struct EventPayload {
    std::span<const std::byte> bytes;
};

/// @brief How the service seeds RNG state for new effects/emitters.
enum class RandomSeedMode : u8 {
    FixedSeed,    ///< Deterministic — derived from effect/emitter ids only.
    HostProvided, ///< Host supplies a seed via `EffectFrameInputs::baseRngSeed`.
};

/// @brief Trace verbosity selector for diagnostics.
enum class TraceLevel : u8 {
    Off,
    Errors,
    Summary,
    Verbose,
};

/// @brief Asset format bits for `AssetFormats`.
enum class AssetFormat : u8 {
    Pkb = 0x1,
    TextPkfx = 0x2,
};

/// @brief Bitset of `AssetFormat` values used to gate which readers the service may use.
struct AssetFormats {
    u8 bits = 0;

    constexpr bool has(AssetFormat f) const noexcept {
        return (bits & static_cast<u8>(f)) != 0U;
    }

    static constexpr AssetFormats all() noexcept {
        return AssetFormats{static_cast<u8>(static_cast<u8>(AssetFormat::Pkb) |
                                            static_cast<u8>(AssetFormat::TextPkfx))};
    }
};

} // namespace whiteout::cornflakes
