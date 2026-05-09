#pragma once

#include "whiteout/flakes/types.h"
#include "whiteout/flakes/model_types.h"
#include "types.h"

#include <vector>

namespace whiteout::flakes::renderer { class ISoundEmitter; }
namespace whiteout::flakes::renderer::particle { class SplatService; }
namespace whiteout::flakes::renderer::model { struct Actor; }

namespace whiteout::flakes::renderer::effects {

class SpnSpawner;


class EventEmitterPool {
public:

    void Reset(std::vector<model::EventObjectConfig> configs,
               std::vector<u32>                      globalSequences);

    void Tick(const model::Actor&                    actor,
              const std::vector<Matrix44f>&          boneWorldMatrices,
              i32                                    activeSeqIdx,
              i32                                    localTimeMs,
              i32                                    globalTimeMs,
              i32                                    seqStartMs,
              i32                                    seqEndMs,
              particle::SplatService*                splats,
              SpnSpawner*                            spn,
              ISoundEmitter*                         sounds);

    bool Empty() const { return entries_.empty(); }

private:

    struct Entry {
        model::EventObjectConfig cfg;
        i32  lastFrame        = -1;
        bool resolutionFailed = false;
    };
    std::vector<Entry>    entries_;
    std::vector<u32>      globalSequences_;
    i32                   prevSeqIdx_ = -1;
};

}
