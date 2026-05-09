#pragma once

#include "common_types.h"

#include <vector>

namespace whiteout::flakes::io {

std::vector<u8> DecodeTeamGlow(u8 tcR, u8 tcG, u8 tcB,
                               i32& outW, i32& outH);

}
