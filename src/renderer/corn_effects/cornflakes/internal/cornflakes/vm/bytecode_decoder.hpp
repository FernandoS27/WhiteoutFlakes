#pragma once

#include <cornflakes/interface/binding/ir_to_cbem_lowerer.hpp>
#include <cornflakes/interface/core/arena.hpp>
#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/interface/diagnostics/issue.hpp>

#include <cstddef>
#include <span>

namespace whiteout::cornflakes {

struct DecodedProgram {
    std::span<const CBEMInstruction> instructions;
};

DecodedProgram decodeBytecodeStream(std::span<const u8> bytes, IArena& arena, IssueBag& issues);

}
