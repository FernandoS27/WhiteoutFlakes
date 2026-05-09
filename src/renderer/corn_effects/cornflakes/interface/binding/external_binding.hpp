#pragma once

/// @file
/// @brief Declarations for VM externals (input/output ports) and function-call symbols.

#include <cornflakes/interface/core/types.hpp>

#include <span>
#include <string_view>

namespace whiteout::cornflakes {

/// @brief Bit flags for `ExternalBinding::accessMask`.
namespace external_access {
inline constexpr u32 kRead = 0x01U;
inline constexpr u32 kWrite = 0x02U;
inline constexpr u32 kReadWrite = 0x03U;
inline constexpr u32 kLifecycle = 0x04U; ///< Engine-managed (lifeRatio, isAlive, etc).
} // namespace external_access

/// @brief One declared external port of a VM scope.
struct ExternalBinding {
    u16 slot = 0;
    std::string_view name;
    std::string_view typeName;
    u32 nativeType = 0;
    u32 storageSize = 0;
    u32 accessMask = 0;

    u16 canonicalSlot = 0; ///< Slot in the effect-wide ExternalBindingTable; 0 if unmapped.
};

/// @brief Sentinel for `FunctionBinding::symbolSlot` when no host symbol resolved.
inline constexpr u32 kSymbolSlotUnbound = 0xFFFFFFFFU;

/// @brief One declared FunctionCall site (native dispatcher resolved by name).
struct FunctionBinding {
    u16 slot = 0;
    std::string_view symbolName;
    u32 symbolSlot = 0;
    u32 traits = 0;
};

const ExternalBinding* findBindingByName(std::span<const ExternalBinding> bindings,
                                         std::string_view name) noexcept;

const FunctionBinding* findFunctionByName(std::span<const FunctionBinding> bindings,
                                          std::string_view symbolName) noexcept;

/// @brief Pick the canonical slot, falling back to the per-scope slot when unmapped.
constexpr u16 resolveExternalSlot(const ExternalBinding& b) noexcept {
    return (b.canonicalSlot == 0U && b.slot != 0U) ? b.slot : b.canonicalSlot;
}

} // namespace whiteout::cornflakes
