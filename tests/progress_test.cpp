// ============================================================================
// ProgressMonitor / ProgressState / LoadTaskRunner.
//
// The property worth testing here is the one an integration test cannot see: a
// child monitor destroyed on an early return still closes its budget. The load
// paths this exists for (fourteen client databases, two CASC roots, a manifest
// walk) return early on missing files, shape mismatches and cancellation, and
// every one of those paths would otherwise leave the bar parked until the whole
// operation ended — while looking exactly like the freeze it replaced.
// ============================================================================

#include "io/file_content_provider.h"
#include "io/load_task.h"
#include "io/progress.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using namespace whiteout::flakes;
using Catch::Matchers::WithinAbs;

namespace {

// Push through the throttle. Begin/Note/Split/destruction are unthrottled, but
// Worked and SetProgress are not, so a test that only calls those would read
// whatever the last unthrottled write left behind.
void SettleThrottle() {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
}

} // namespace

TEST_CASE("An inert monitor reports nowhere and is never cancelled", "[progress]") {
    io::ProgressMonitor m;
    REQUIRE(m.Inert());
    REQUIRE_FALSE(m.Cancelled());
    // The uninstrumented path: every call is a branch and a return.
    m.Begin("stage", 10);
    m.Worked(5);
    m.Note("thing");
    io::ProgressMonitor child = m.Split(1);
    REQUIRE(child.Inert());
    REQUIRE_FALSE(child.Cancelled());
}

TEST_CASE("A child closes its budget when destroyed on an early return", "[progress]") {
    io::ProgressState state;
    state.Begin("test", /*cancellable=*/true);
    io::ProgressMonitor root(&state);
    root.Begin("four steps", 4);

    {
        io::ProgressMonitor step = root.Split(1);
        step.Begin("inner", 100);
        step.Worked(10);
        SettleThrottle();
        step.Worked(0); // force the throttled sample out
        // A tenth of the first quarter.
        REQUIRE_THAT(state.Poll().fraction, WithinAbs(0.025f, 0.001f));
        // Scope ends here the way an early `return false` would, with the
        // inner step 90% unfinished.
    }
    REQUIRE_THAT(state.Poll().fraction, WithinAbs(0.25f, 0.001f));

    // And the next sibling starts exactly where that one closed, rather than
    // overlapping it.
    {
        io::ProgressMonitor step = root.Split(2);
        step.Begin("inner", 2);
        step.Worked(1);
        SettleThrottle();
        step.Worked(0);
        REQUIRE_THAT(state.Poll().fraction, WithinAbs(0.5f, 0.001f));
    }
    REQUIRE_THAT(state.Poll().fraction, WithinAbs(0.75f, 0.001f));
}

TEST_CASE("Nesting three deep stays monotonic and reaches exactly 1.0", "[progress]") {
    io::ProgressState state;
    state.Begin("test", false);
    float last = 0.0f;
    {
        io::ProgressMonitor root(&state);
        root.Begin("outer", 2);
        for (int i = 0; i < 2; ++i) {
            io::ProgressMonitor mid = root.Split(1);
            mid.Begin("mid", 3);
            for (int j = 0; j < 3; ++j) {
                io::ProgressMonitor leaf = mid.Split(1);
                leaf.Begin("leaf", 5);
                for (int k = 0; k < 5; ++k) {
                    leaf.Worked();
                    SettleThrottle();
                    leaf.Worked(0);
                    const float f = state.Poll().fraction;
                    REQUIRE(f >= last);
                    last = f;
                }
            }
        }
    }
    // The root's own destructor is what closes the last unit.
    REQUIRE_THAT(state.Poll().fraction, WithinAbs(1.0f, 0.0001f));
}

TEST_CASE("A step with no total draws as indeterminate, not as zero", "[progress]") {
    io::ProgressState state;
    state.Begin("test", false);
    io::ProgressMonitor root(&state);
    root.Begin("unknown"); // total 0
    root.Note("something");
    const io::ProgressSnapshot s = state.Poll();
    REQUIRE(s.indeterminate);
    REQUIRE(s.total == 0);
    REQUIRE(s.stage == "unknown");
    REQUIRE(s.object == "something");
}

TEST_CASE("Cancellation is visible without reporting anything", "[progress]") {
    io::ProgressState state;
    state.Begin("test", true);
    io::ProgressMonitor root(&state);
    root.Begin("work", 1000);
    REQUIRE_FALSE(root.Cancelled());

    state.RequestCancel();
    REQUIRE(root.Cancelled());
    // A child inherits it — the deep loops are the ones that need to ask.
    io::ProgressMonitor child = root.Split(1);
    REQUIRE(child.Cancelled());
    REQUIRE(state.Poll().cancelRequested);
}

TEST_CASE("A finished operation ignores late worker reports", "[progress]") {
    io::ProgressState state;
    state.Begin("test", false);
    {
        io::ProgressMonitor root(&state);
        root.Begin("work", 10);
        root.Worked(5);
    }
    state.Finish(/*ok=*/true);
    REQUIRE(state.Poll().done);
    REQUIRE_THAT(state.Poll().fraction, WithinAbs(1.0f, 0.0001f));

    // A monitor that outlives Finish (a worker draining after cancellation)
    // must not reopen it or walk the bar back.
    io::ProgressMonitor late(&state);
    late.Begin("late", 10);
    late.Worked(1);
    SettleThrottle();
    late.Worked(0);
    const io::ProgressSnapshot s = state.Poll();
    REQUIRE(s.done);
    REQUIRE_THAT(s.fraction, WithinAbs(1.0f, 0.0001f));
}

TEST_CASE("Task outcomes are delivered once, on the pumping thread", "[progress][task]") {
    io::LoadTaskRunner runner;

    const std::thread::id pumpThread = std::this_thread::get_id();
    std::atomic<int> calls{0};
    std::atomic<bool> ranOnPumpThread{false};
    std::atomic<bool> bodyRanElsewhere{false};

    runner.Run(
        "unit",
        [&](io::ProgressMonitor& m) {
            bodyRanElsewhere = std::this_thread::get_id() != pumpThread;
            m.Begin("stage", 2);
            m.Worked(2);
            return io::TaskResult::Ok();
        },
        [&](const io::TaskOutcome& out) {
            ++calls;
            ranOnPumpThread = std::this_thread::get_id() == pumpThread;
            REQUIRE(out.ok);
            REQUIRE_FALSE(out.cancelled);
        });

    // Nothing may fire before the host pumps.
    REQUIRE(calls.load() == 0);
    for (int i = 0; i < 500 && calls.load() == 0; ++i) {
        runner.Pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    runner.Pump(); // a second pump must not re-deliver

    REQUIRE(calls.load() == 1);
    REQUIRE(ranOnPumpThread.load());
    REQUIRE(bodyRanElsewhere.load());
    REQUIRE_FALSE(runner.Busy());
}

TEST_CASE("A failing body reports its error, and cancellation outranks it", "[progress][task]") {
    SECTION("failure") {
        io::LoadTaskRunner runner;
        std::string got;
        bool fired = false;
        runner.Run(
            "unit", [](io::ProgressMonitor&) { return io::TaskResult::Fail("no storage"); },
            [&](const io::TaskOutcome& out) {
                fired = true;
                got = out.error;
                REQUIRE_FALSE(out.ok);
                REQUIRE_FALSE(out.cancelled);
            });
        for (int i = 0; i < 500 && !fired; ++i) {
            runner.Pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        REQUIRE(fired);
        REQUIRE(got == "no storage");
    }

    SECTION("cancel wins over the body's own failure") {
        io::LoadTaskRunner runner;
        std::atomic<bool> entered{false};
        bool fired = false;
        runner.Run(
            "unit",
            [&](io::ProgressMonitor& m) {
                entered = true;
                // Spin until the test cancels, the way a real loop's
                // per-item check does.
                for (int i = 0; i < 5000 && !m.Cancelled(); ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                return io::TaskResult::Fail("gave up");
            },
            [&](const io::TaskOutcome& out) {
                fired = true;
                REQUIRE(out.cancelled);
                REQUIRE_FALSE(out.ok);
                REQUIRE(out.error == "Cancelled");
            });
        while (!entered.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        runner.RequestCancel();
        for (int i = 0; i < 2000 && !fired; ++i) {
            runner.Pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        REQUIRE(fired);
    }
}

// ============================================================================
// Opt-in: the real thing, against a real install.
//
// Hidden behind a `[.]` tag and an environment variable, because the corpus box
// has no game installs. Run it with, e.g.:
//
//   WDX_TEST_CASC_ROOT="D:/Programs/Warcraft III" progress_test.exe "[casc]"
//
// This is the test that would have caught the feature being plumbed but never
// firing: everything above passes with a monitor nothing ever calls.
// ============================================================================
TEST_CASE("A real CASC open reports through the monitor", "[.][casc]") {
    const char* root = std::getenv("WDX_TEST_CASC_ROOT");
    if (!root || !*root) {
        WARN("WDX_TEST_CASC_ROOT unset - skipping");
        return;
    }

    io::FileContentProvider provider;
    provider.SetInstallPath(root);
    REQUIRE(provider.StoragesState() == io::StorageState::Dirty);

    io::ProgressState state;
    state.Begin("open", /*cancellable=*/true);

    // Sampled from this thread while the open runs on another, which is the
    // arrangement the whole design exists to make possible.
    std::atomic<bool> done{false};
    std::vector<float> fractions;
    std::vector<std::string> stages;
    std::thread sampler([&] {
        while (!done.load()) {
            const io::ProgressSnapshot s = state.Poll();
            if (fractions.empty() || s.fraction != fractions.back())
                fractions.push_back(s.fraction);
            if (!s.stage.empty() && (stages.empty() || stages.back() != s.stage))
                stages.push_back(s.stage);
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
    });

    bool opened = false;
    {
        io::ProgressMonitor m(&state);
        opened = provider.OpenStorages(&m);
    }
    done = true;
    sampler.join();

    REQUIRE(opened);
    REQUIRE(provider.StoragesState() == io::StorageState::Open);

    // More than one distinct sample: a single value would mean the callback
    // fired once (or never) and the bar would have sat still for the whole open.
    INFO("distinct fractions: " << fractions.size() << ", stages: " << stages.size());
    REQUIRE(fractions.size() > 1);
    REQUIRE_FALSE(stages.empty());
    // Monotonic, and finished.
    for (std::size_t i = 1; i < fractions.size(); ++i)
        REQUIRE(fractions[i] >= fractions[i - 1]);
    REQUIRE_THAT(state.Poll().fraction, WithinAbs(1.0f, 0.01f));

    for (const std::string& st : stages)
        WARN("stage: " << st);
}
