#pragma once

// ============================================================================
// StarCraft II external animation files (`.m3a`) on the focus actor.
//
// A `.m3` names none of these — its chunk table has no path of any kind. The
// game reads them off the model's catalog entry and merges each into one global
// sequence space, binding tracks to the model by animId. Here the user does the
// naming; M3ModelAdapter does the merge.
//
// Abstract so a build without `.m3` support has no implementation to link:
// MakeSc2AnimationFiles returns null there, and callers ask for the module
// rather than for a stub that answers "no".
// ============================================================================

#include "whiteout/flakes/types.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace whiteout::flakes::renderer {
class RenderService;
}

namespace whiteout::flakes {

class DocumentManager;
class PlaybackController;

class Sc2AnimationFiles {
public:
    virtual ~Sc2AnimationFiles() = default;

    struct Attached {
        std::string label;
        usize sequenceCount = 0;
        usize firstSequence = 0;
    };
    /// One row per sub-track container of a sequence, in the order
    /// AnimTrackInfo::subtrack indexes them.
    struct Subtrack {
        std::string name;
        u16 priority = 0;
        bool concurrent = false;
        usize trackCount = 0;
    };

    /// True only when the focus actor is an `.m3` — nothing else takes one.
    virtual bool CanAttach() const = 0;
    virtual std::vector<Attached> AttachedFiles() const = 0;
    /// Reads @p path (a loose file, else through the scene's provider), merges
    /// it, and refreshes the sequence list. False on a parse failure, a file
    /// with no sequences, or one already attached.
    virtual bool Attach(const std::filesystem::path& path) = 0;
    virtual bool Detach(usize index) = 0;
    /// Empty for anything but an `.m3`.
    virtual std::vector<Subtrack> SubtracksOf(i32 sequence) const = 0;
};

/// Null without `.m3` support.
std::unique_ptr<Sc2AnimationFiles> MakeSc2AnimationFiles(renderer::RenderService& service,
                                                         DocumentManager& documents,
                                                         PlaybackController& playback);

} // namespace whiteout::flakes
