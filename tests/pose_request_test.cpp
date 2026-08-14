// ============================================================================
// PoseRequest (REFACTOR_PLAN.md P7) — shape only, so there is little behaviour
// to test. What there is: the two contracts every adapter relies on.
//
// Everything else in PoseRequest is inert for Warcraft III and has no
// implementation to check yet; asserting on defaults that nothing reads would
// pin the header against itself. P9/P10 grow this file when the fields go live.
// ============================================================================

#include <catch2/catch_test_macros.hpp>

#include "whiteout/flakes/pose_request.h"

#include <utility>

using whiteout::flakes::ClipMask;
using whiteout::flakes::ClipRef;
using whiteout::flakes::PoseRequest;

// The rvalue overload of OneClip is deleted, so `OneClip(ClipRef{...})` is a
// compile error rather than a span over a destroyed temporary. Asserted at
// compile time because that is the only place it is observable — a
// well-meaning "add the convenience overload back" reintroduces a dangling
// read that no runtime test can catch.
template <class Arg>
concept OneClipCallable = requires(Arg a) { PoseRequest::OneClip(std::forward<Arg>(a)); };
static_assert(OneClipCallable<const ClipRef&>, "a named clip must be accepted");
static_assert(!OneClipCallable<ClipRef&&>, "a temporary clip must not compile");

TEST_CASE("an empty clip list evaluates as bind pose") {
    // "No clips" and "sequence -1" are deliberately the same request, so a
    // single-clip adapter can read PrimaryClip() unconditionally instead of
    // handling an empty span itself. Every current adapter depends on this.
    const PoseRequest req;
    CHECK(req.clips.empty());
    CHECK(req.PrimaryClip().sequence == -1);
}

TEST_CASE("PrimaryClip is the first clip") {
    const ClipRef clips[] = {
        ClipRef{.sequence = 3, .timeMs = 120},
        ClipRef{.sequence = 7, .timeMs = 900},
    };
    PoseRequest req;
    req.clips = clips;

    // A multi-clip request is legal to *build* — WoW and SC2 will — and a
    // single-clip adapter takes the first rather than refusing. Blending is
    // the adapter's business; the core never sees a partial pose.
    CHECK(req.PrimaryClip().sequence == 3);
    CHECK(req.PrimaryClip().timeMs == 120);
}

TEST_CASE("OneClip views the caller's clip") {
    const ClipRef clip{.sequence = 4, .timeMs = 250};
    const PoseRequest req = PoseRequest::OneClip(clip);

    REQUIRE(req.clips.size() == 1);
    CHECK(req.clips.data() == &clip); // a view, not a copy
    CHECK(req.PrimaryClip().sequence == 4);
}

TEST_CASE("clip defaults are the inert ones") {
    // These are the values every Warcraft III call leaves alone. A change here
    // would silently alter what a future blending adapter does with a
    // single-clip request built by existing code.
    const ClipRef c;
    CHECK(c.weight == 1.0f);
    CHECK(c.speed == 1.0f);
    CHECK(c.loop);
    CHECK(c.mask == ClipMask::FillDefault);
    CHECK(c.rootNode == -1); // whole skeleton
}
