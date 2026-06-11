#pragma once

#include <cornflakes/interface/core/types.hpp>

namespace whiteout::cornflakes {

struct PayloadKey {
    u32 generatorKey = 0;
    u32 payloadKey = 0;
    u32 elementKey = 0;

    constexpr bool operator==(const PayloadKey&) const = default;
};

}
