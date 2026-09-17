#pragma once

// ============================================================================
// The Animation window: the model's global loops, the plays layered under the
// sequence dropdown, and the attached `.m3a` files.
//
// StarCraft II does not play "a sequence". It plays a *bracket*, and one bracket
// expands into one player per sub-track container the sequence spans: the
// Marine's `Cover` is `Cover_full` plus `Cover_Shield`, and the shield shows for
// as long as something holds that second container down. Several brackets run
// at once and blend against a weight budget. The dropdown is one bracket; this
// window is the others, plus the ones the model starts by itself — so it exists
// only for a model that can take an animation file.
// ============================================================================

#include "whiteout/flakes/types.h"

#include <string>

namespace whiteout::flakes {

class Sc2AnimationFiles;
struct UiContext;

class AnimationWindow {
public:
    explicit AnimationWindow(UiContext& ctx);

    bool IsOpen() const {
        return open_;
    }
    void SetOpen(bool open) {
        open_ = open;
    }
    /// Draws nothing unless open and the focus actor takes animation files.
    void Build();

private:
    /// One row of the track table. False when the row removed itself, which
    /// the caller must honour before touching the track list again.
    bool BuildTrackRow(Sc2AnimationFiles& sc2, usize index);
    /// A multi-select `.m3a` picker: a model's animations are routinely split
    /// across several files (`*_RequiredAnims`, `*OptionalAnims`, ...).
    void AttachFiles(Sc2AnimationFiles& sc2);

    UiContext& ctx_;
    bool open_ = false;
    /// The last pick the attach refused, shown until the next attempt: dropping
    /// it on the floor leaves the user staring at a list their file is not in.
    std::string attachError_;
};

} // namespace whiteout::flakes
