#pragma once

/// @file
/// @brief One-shot decoder turning a raw bytecode byte stream into `CBEMInstruction`s.

#include <cornflakes/interface/binding/ir_to_cbem_lowerer.hpp>
#include <cornflakes/interface/core/arena.hpp>
#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/interface/diagnostics/issue.hpp>

#include <cstddef>
#include <span>

namespace whiteout::cornflakes {

/// @brief Result of decoding a raw bytecode stream — instructions live in the supplied arena.
struct DecodedProgram {
    std::span<const CBEMInstruction> instructions;
};

/// @brief Decode `bytes` into `CBEMInstruction`s allocated from `arena`.
DecodedProgram decodeBytecodeStream(std::span<const u8> bytes, IArena& arena, IssueBag& issues);

} // namespace whiteout::cornflakes
