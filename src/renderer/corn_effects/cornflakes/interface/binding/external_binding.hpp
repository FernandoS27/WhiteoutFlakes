#pragma once

#include <cornflakes/interface/core/types.hpp>

#include <span>
#include <string_view>

namespace whiteout::cornflakes {

namespace external_access {
inline constexpr u32 kRead = 0x01U;
inline constexpr u32 kWrite = 0x02U;
inline constexpr u32 kReadWrite = 0x03U;
inline constexpr u32 kLifecycle = 0x04U;
}

struct ExternalBinding {
    u16 slot = 0;
    std::string_view name;
    std::string_view typeName;
    u32 nativeType = 0;
    u32 storageSize = 0;
    u32 accessMask = 0;

    u16 canonicalSlot = 0;
};

inline constexpr u32 kSymbolSlotUnbound = 0xFFFFFFFFU;

struct FunctionBinding {
    u16 slot = 0;
    std::string_view symbolName;
    u32 symbolSlot = 0;
    u32 traits = 0;

    std::string_view canonicalName{};
};

const ExternalBinding* findBindingByName(std::span<const ExternalBinding> bindings,
                                         std::string_view name) noexcept;

const FunctionBinding* findFunctionByName(std::span<const FunctionBinding> bindings,
                                          std::string_view symbolName) noexcept;

constexpr u16 resolveExternalSlot(const ExternalBinding& b) noexcept {
    return (b.canonicalSlot == 0U && b.slot != 0U) ? b.slot : b.canonicalSlot;
}

}
