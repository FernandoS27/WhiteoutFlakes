#pragma once
#include "whiteout/flakes/types.h"
#include <vector>

namespace whiteout::flakes::renderer::assets {

std::vector<u8> GenerateViewCubeAtlas(i32& outW, i32& outH);

}
