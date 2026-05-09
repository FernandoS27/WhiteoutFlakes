#pragma once

// ============================================================================
// WhiteoutFlakes — event-data POD types + lookup.
//
// SpnEntry / SplEntry / UbrEntry / SndEntry are read from .slk files and
// indexed by event id. Renderer subsystems and host code (sound emitter)
// look them up via the FindXxx helpers. Loading is one-shot via
// LoadEventDataFiles, called by the host once a content provider is
// available.
// ============================================================================

#include "types.h"

#include <string>
#include <string_view>
#include <vector>

namespace whiteout::flakes::io {
class IContentProvider;
}

namespace whiteout::flakes::io {

struct SpnEntry {
    std::string modelPath;
};

struct SplEntry {
    std::string file;
    f32         scale          = 1.0f;
    f32         startC[4]      = {1, 1, 1, 1};
    f32         midC[4]        = {1, 1, 1, 1};
    f32         endC[4]        = {1, 1, 1, 1};
    i32         columns        = 1;
    i32         rows           = 1;
    f32         lifespan       = 0.0f;
    f32         decay          = 0.0f;
    i32         uvLifeStart    = 0;
    i32         uvLifeEnd      = 0;
    i32         lifespanRepeat = 1;
    i32         uvDecayStart   = 0;
    i32         uvDecayEnd     = 0;
    i32         decayRepeat    = 1;
    i32         blendMode      = 0;
};

struct UbrEntry {
    std::string file;
    f32         scale     = 1.0f;
    f32         c[3][4]   = { {1,1,1,1}, {1,1,1,1}, {1,1,1,1} };
    f32         birthTime = 0.0f;
    f32         pauseTime = 0.0f;
    f32         decay     = 0.0f;
    i32         blendMode = 0;
};

struct SndEntry {
    std::vector<std::string> filePaths;
    f32                      volume         = 1.0f;
    f32                      minDistance    = 0.0f;
    f32                      maxDistance    = 0.0f;
    f32                      distanceCutoff = 0.0f;
};

void LoadEventDataFiles(IContentProvider* cp, bool force = false);

const SpnEntry* FindSpn(std::string_view id);
const SplEntry* FindSpl(std::string_view id);
const UbrEntry* FindUbr(std::string_view id);
const SndEntry* FindSnd(std::string_view id);

}  // namespace whiteout::flakes::io

namespace whiteout::flakes {
using ::whiteout::flakes::io::SndEntry;
using ::whiteout::flakes::io::SpnEntry;
using ::whiteout::flakes::io::SplEntry;
using ::whiteout::flakes::io::UbrEntry;
}
