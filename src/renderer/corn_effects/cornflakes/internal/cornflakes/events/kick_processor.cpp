#include <cornflakes/core/determinism.hpp>
#include <cornflakes/events/kick_processor.hpp>

#include <utility>

namespace whiteout::cornflakes {

bool KickProcessor::submit(IWorkerPool& pool, ITimelineSemaphore& syncJob,
                           ITimelineSemaphore::Value syncValue, KickEventTask task,
                           PayloadCacheStore& store, IssueBag& issues) const {

    const bool fatalBefore = issues.hasFatal();

    auto taskMoved = std::move(task);

    WorkerTask worker;
    worker.fn = [moved = std::move(taskMoved), &store, &issues]() mutable {
        if (moved.onRun) {
            moved.onRun(moved.source, moved.target, store, issues);
        }
    };
    worker.waitSemaphore = &syncJob;
    worker.waitValue = syncValue;

    pool.submit(worker);

    return fatalBefore == issues.hasFatal();
}

} // namespace whiteout::cornflakes
