#pragma once

#include "cbem_internal.hpp"

#include <cornflakes/interface/binding/ir_to_cbem_lowerer.hpp>
#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/interface/diagnostics/issue.hpp>
#include <cornflakes/interface/simt/simt_packet.hpp>
#include <cornflakes/interface/vm/bytecode_exec_context.hpp>
#include <cornflakes/interface/vm/register_value.hpp>

#include <array>
#include <concepts>
#include <cstddef>

namespace whiteout::cornflakes::cbem {

template <std::size_t N>
struct RegPack {
    std::array<std::array<u32, N>, 4> c{};
};

struct AlwaysOn {
    static constexpr bool live(std::size_t) noexcept {
        return true;
    }
};

struct PacketMask {
    simt::LaneMask bits = simt::kAllLanesLive;

    constexpr bool live(std::size_t lane) const noexcept {
        return ((bits >> lane) & 1U) != 0U;
    }
};

template <class B>
concept CbemBackend = requires(typename B::State& s, typename B::Mask m, std::size_t lane,
                               u32 regId, RegisterValue& out, const RegisterValue& in,
                               IssueBag& issues, const CBEMInstruction& ins) {
    { B::kLanes } -> std::convertible_to<std::size_t>;
    typename B::Value;
    typename B::Mask;
    typename B::State;
    { B::lane(s, lane) } -> std::same_as<BytecodeExecContext&>;
    { B::live(m, lane) } -> std::convertible_to<bool>;

    { B::readSrc(s, lane, regId, out, issues) } -> std::convertible_to<bool>;
    { B::writeDst(s, lane, regId, in, issues) } -> std::convertible_to<bool>;

    { B::beforeCall(s, lane, ins) } -> std::same_as<void>;
    { B::afterCall(s, lane, ins) } -> std::same_as<void>;
};

struct ScalarBackend {
    static constexpr std::size_t kLanes = 1U;

    using Value = RegPack<1>;
    using Mask = AlwaysOn;
    using State = BytecodeExecContext;

    static constexpr BytecodeExecContext& lane(State& s, std::size_t) noexcept {
        return s;
    }
    static constexpr bool live(Mask, std::size_t) noexcept {
        return true;
    }

    static bool readSrc(State& s, std::size_t, u32 regId, RegisterValue& out,
                        IssueBag& issues) noexcept {
        return whiteout::cornflakes::readSrc(s, regId, out, issues);
    }
    static bool writeDst(State& s, std::size_t, u32 regId, const RegisterValue& v,
                         IssueBag& issues) noexcept {
        return whiteout::cornflakes::writeDst(s, regId, v, issues);
    }

    static void beforeCall(State&, std::size_t, const CBEMInstruction&) noexcept {}
    static void afterCall(State&, std::size_t, const CBEMInstruction&) noexcept {}
};

static_assert(CbemBackend<ScalarBackend>);
static_assert(sizeof(AlwaysOn) <= 1U, "AlwaysOn must be empty; a mask word is not zero-cost");

}
