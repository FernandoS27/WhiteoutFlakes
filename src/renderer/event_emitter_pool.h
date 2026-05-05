#pragma once

#include "common_types.h"
#include "model_types.h"
#include "types.h"

#include <vector>

namespace WhiteoutDex {

class ISoundEmitter;
class SpnSpawner;

namespace particle { class SplatService; }

struct Actor;

class EventEmitterPool {
public:

    void Reset(std::vector<EventObjectConfig> configs,
               std::vector<u32>               globalSequences);

    void Tick(const Actor&                    actor,
              const std::vector<Matrix44f>&   boneWorldMatrices,
              i32                             activeSeqIdx,
              i32                             localTimeMs,
              i32                             globalTimeMs,
              i32                             seqStartMs,
              i32                             seqEndMs,
              particle::SplatService*         splats,
              SpnSpawner*                     spn,
              ISoundEmitter*                  sounds);

    bool Empty() const { return entries_.empty(); }

private:

    struct Entry {
        EventObjectConfig cfg;
        i32  lastFrame        = -1;
        bool resolutionFailed = false;
    };
    std::vector<Entry>    entries_;
    std::vector<u32>      globalSequences_;
    i32                   prevSeqIdx_ = -1;
};

}
