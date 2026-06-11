#pragma once

#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/interface/diagnostics/issue.hpp>
#include <cornflakes/events/payload_cache_store.hpp>
#include <cornflakes/events/payload_key.hpp>
#include <cornflakes/scheduler/timeline_semaphore.hpp>
#include <cornflakes/scheduler/worker_pool.hpp>
#include <cornflakes/interface/schema/handles.hpp>

#include <functional>
#include <span>

namespace whiteout::cornflakes {

struct KickEventTask {
    PayloadKey source;
    LayerId target;

    std::function<void(const PayloadKey&, LayerId, PayloadCacheStore&, IssueBag&)> onRun;
};

class KickProcessor {
public:
    KickProcessor() = default;

    bool submit(IWorkerPool& pool, ITimelineSemaphore& syncJob, ITimelineSemaphore::Value syncValue,
                KickEventTask task, PayloadCacheStore& store, IssueBag& issues) const;
};

}
