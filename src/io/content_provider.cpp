#include "whiteout/flakes/content_provider.h"

#include <memory>
#include <utility>

namespace whiteout::flakes::io {

// Sync convenience over the async surface. The shared_ptr-captured result
// outlives this stack frame in case the worker thread is mid-write when the
// callback fires; Wait() guarantees the write has finished before we read
// from `*p`, so no further synchronisation is required.
std::optional<std::vector<u8>> IContentProvider::ReadFile(const std::string& path,
                                                          std::string* actualExt) {
    auto p = std::make_shared<RequestResult>();
    const RequestId id = Request(path, [p](RequestResult&& r) { *p = std::move(r); });
    if (id == kInvalidRequestId)
        return std::nullopt;
    Wait(id);
    if (actualExt)
        *actualExt = std::move(p->actualExt);
    if (!p->ok)
        return std::nullopt;
    return std::move(p->data);
}

} // namespace whiteout::flakes::io
