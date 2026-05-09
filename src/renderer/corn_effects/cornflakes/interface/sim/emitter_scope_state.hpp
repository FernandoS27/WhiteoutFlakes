#pragma once

/// @file
/// @brief Owned per-scope register storage shared across an emitter's particles for spawn-time evaluation.

#include <cornflakes/interface/vm/bytecode_exec_context.hpp>
#include <cornflakes/interface/vm/register_value.hpp>

#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace whiteout::cornflakes {

/// @brief Owned per-scope register vectors + externals, with span-views for the VM context.
struct EmitterScopeState {

    std::array<std::vector<RegisterValue>, kScopeRegisterBuckets> registers;

    std::vector<RegisterValue> externals;

    std::array<std::span<RegisterValue>, kScopeRegisterBuckets> scopeSpans() noexcept {
        std::array<std::span<RegisterValue>, kScopeRegisterBuckets> out{};
        for (std::size_t i = 0; i < kScopeRegisterBuckets; ++i) {
            out[i] = std::span<RegisterValue>{registers[i].data(), registers[i].size()};
        }
        return out;
    }
};

} // namespace whiteout::cornflakes
