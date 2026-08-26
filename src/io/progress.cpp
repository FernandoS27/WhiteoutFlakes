#include "io/progress.h"

#include <algorithm>
#include <utility>

namespace whiteout::flakes::io {

namespace {
// How often a running step is allowed to touch the cell. Anything faster is
// invisible at 60 Hz and only costs the worker a lock. Begin, Note, Split and
// destruction bypass it — those are transitions, and a dropped one leaves the
// wrong label on screen rather than a slightly stale number.
constexpr i64 kMinWriteIntervalUs = 16000;

i64 NowUs(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::microseconds>(t.time_since_epoch()).count();
}
} // namespace

// ---- ProgressState ---------------------------------------------------------

void ProgressState::Begin(std::string title, bool cancellable, bool modal) {
    std::lock_guard lk(mu_);
    snap_ = ProgressSnapshot{};
    snap_.title = std::move(title);
    snap_.cancellable = cancellable;
    snap_.modal = modal;
    snap_.active = true;
    snap_.indeterminate = true;
    start_ = Clock::now();
    cancel_.store(false, std::memory_order_release);
    lastWriteUs_.store(0, std::memory_order_relaxed);
}

void ProgressState::Finish(bool ok, std::string error) {
    std::lock_guard lk(mu_);
    if (snap_.done)
        return;
    snap_.active = false;
    snap_.done = true;
    snap_.failed = !ok;
    snap_.error = std::move(error);
    if (ok) {
        snap_.fraction = 1.0f;
        snap_.indeterminate = false;
    }
    snap_.elapsedMs = std::chrono::duration<f64, std::milli>(Clock::now() - start_).count();
}

ProgressSnapshot ProgressState::Poll() const {
    std::lock_guard lk(mu_);
    ProgressSnapshot out = snap_;
    out.cancelRequested = cancel_.load(std::memory_order_acquire);
    if (out.active)
        out.elapsedMs = std::chrono::duration<f64, std::milli>(Clock::now() - start_).count();
    return out;
}

void ProgressState::Write(f32 fraction, bool indeterminate, std::string_view stage,
                          std::string_view object, u64 current, u64 total, u64 bytesDone,
                          u64 bytesTotal) {
    std::lock_guard lk(mu_);
    if (snap_.done)
        return; // a late worker report must not reopen a finished operation
    // Monotonic. A sibling on another thread, or a child closing its budget
    // after a later one opened, can otherwise walk the bar backwards.
    snap_.fraction = (std::max)(snap_.fraction, fraction);
    snap_.indeterminate = indeterminate;
    snap_.stage.assign(stage);
    snap_.object.assign(object);
    snap_.current = current;
    snap_.total = total;
    snap_.bytesDone = bytesDone;
    snap_.bytesTotal = bytesTotal;
}

// ---- ProgressMonitor -------------------------------------------------------

ProgressMonitor::ProgressMonitor(ProgressMonitor&& o) noexcept
    : state_(o.state_), lo_(o.lo_), hi_(o.hi_), done_(o.done_), total_(o.total_),
      bytesDone_(o.bytesDone_), bytesTotal_(o.bytesTotal_), stage_(std::move(o.stage_)),
      object_(std::move(o.object_)), restoreStage_(std::move(o.restoreStage_)),
      hasRestore_(o.hasRestore_) {
    // The moved-from must not close a budget the moved-to now owns.
    o.state_ = nullptr;
    o.hasRestore_ = false;
}

ProgressMonitor& ProgressMonitor::operator=(ProgressMonitor&& o) noexcept {
    if (this == &o)
        return *this;
    state_ = o.state_;
    lo_ = o.lo_;
    hi_ = o.hi_;
    done_ = o.done_;
    total_ = o.total_;
    bytesDone_ = o.bytesDone_;
    bytesTotal_ = o.bytesTotal_;
    stage_ = std::move(o.stage_);
    object_ = std::move(o.object_);
    restoreStage_ = std::move(o.restoreStage_);
    hasRestore_ = o.hasRestore_;
    o.state_ = nullptr;
    o.hasRestore_ = false;
    return *this;
}

ProgressMonitor::~ProgressMonitor() {
    if (!state_)
        return;
    // Close the budget. This is the reason the type exists: the loads it
    // instruments return early on missing files, shape mismatches and
    // cancellation, and none of those paths should leave the bar parked.
    done_ = total_;
    object_.clear();
    if (hasRestore_)
        stage_ = restoreStage_;
    Push(/*force=*/true);
}

void ProgressMonitor::Begin(std::string_view stage, u64 total) {
    if (!state_)
        return;
    stage_.assign(stage);
    total_ = static_cast<f64>(total);
    done_ = 0.0;
    bytesDone_ = bytesTotal_ = 0;
    object_.clear();
    Push(/*force=*/true);
}

void ProgressMonitor::Worked(u64 items) {
    if (!state_)
        return;
    done_ += static_cast<f64>(items);
    if (total_ > 0.0)
        done_ = (std::min)(done_, total_);
    Push(/*force=*/false);
}

void ProgressMonitor::SetProgress(f64 done, f64 total) {
    if (!state_)
        return;
    total_ = total > 0.0 ? total : 0.0;
    done_ = total_ > 0.0 ? std::clamp(done, 0.0, total_) : 0.0;
    Push(/*force=*/false);
}

void ProgressMonitor::Note(std::string_view object) {
    if (!state_)
        return;
    if (object_ == object)
        return; // same thing still being worked on; the counters say the rest
    object_.assign(object);
    Push(/*force=*/true);
}

void ProgressMonitor::Report(std::string_view stage, std::string_view object, f64 done, f64 total) {
    if (!state_)
        return;
    const bool changed = stage_ != stage || object_ != object;
    if (changed) {
        stage_.assign(stage);
        object_.assign(object);
    }
    total_ = total > 0.0 ? total : 0.0;
    done_ = total_ > 0.0 ? std::clamp(done, 0.0, total_) : 0.0;
    Push(changed);
}

void ProgressMonitor::Bytes(u64 done, u64 total) {
    if (!state_)
        return;
    bytesDone_ = done;
    bytesTotal_ = total;
    Push(/*force=*/false);
}

ProgressMonitor ProgressMonitor::Split(u64 budget) {
    ProgressMonitor child;
    if (!state_)
        return child;

    const f64 span = hi_ - lo_;
    // An unknown total cannot be sliced. Give the child the whole remaining
    // window: it reports honestly inside it, and the parent's own position is
    // what stays indeterminate.
    const f64 startF = total_ > 0.0 ? done_ / total_ : 0.0;
    const f64 endF =
        total_ > 0.0 ? (std::min)((done_ + static_cast<f64>(budget)) / total_, 1.0) : 1.0;

    child.state_ = state_;
    child.lo_ = lo_ + startF * span;
    child.hi_ = lo_ + endF * span;
    child.stage_ = stage_; // until the child names its own
    child.restoreStage_ = stage_;
    child.hasRestore_ = true;

    // Reserved now, not when the child finishes. The child's window covers
    // exactly the reserved slice, so the bar advances through it rather than
    // jumping — and my own position is already correct for the NEXT Split.
    done_ += static_cast<f64>(budget);
    if (total_ > 0.0)
        done_ = (std::min)(done_, total_);
    return child;
}

void ProgressMonitor::Push(bool force) {
    if (!state_)
        return;
    const auto now = std::chrono::steady_clock::now();
    const i64 nowUs = NowUs(now);
    if (!force) {
        const i64 last = state_->lastWriteUs_.load(std::memory_order_relaxed);
        if (last != 0 && nowUs - last < kMinWriteIntervalUs)
            return;
    }
    state_->lastWriteUs_.store(nowUs, std::memory_order_relaxed);

    const bool indeterminate = total_ <= 0.0 && bytesTotal_ == 0;
    f64 f = lo_;
    if (total_ > 0.0)
        f = lo_ + (done_ / total_) * (hi_ - lo_);
    else if (bytesTotal_ != 0)
        f = lo_ + (static_cast<f64>(bytesDone_) / static_cast<f64>(bytesTotal_)) * (hi_ - lo_);

    state_->Write(static_cast<f32>(std::clamp(f, 0.0, 1.0)), indeterminate, stage_, object_,
                  static_cast<u64>(done_), static_cast<u64>(total_), bytesDone_, bytesTotal_);
}

} // namespace whiteout::flakes::io
