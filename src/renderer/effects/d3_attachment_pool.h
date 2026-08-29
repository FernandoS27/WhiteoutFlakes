#pragma once

// ============================================================================
// D3AttachmentPool — the third place a Diablo III TriggerEvent is authored.
//
// The first two are load-time: an `.acr`'s message-1000 events and the
// `BoneStructure::snoParticle` on the appearance, both resolved once by
// ModelLoader. This is the third, and the only one keyed on a *frame*: an
// `AnimPermutation` carries `KeyframedAttachment{flFrame, TriggerEvent}`, and
// the engine plays each through the same `TriggerEvent_Execute` when playback
// crosses its frame. 52,138 of them over 10,484 of the 15,258 shipped anims —
// 4,243 distinct `.prt` and 549 distinct `.acr`, most of which nothing else
// reaches.
//
// Two things come out of it and they are not alike:
//
//   * **A particle.** One `d3::Emitter` per fired attachment, created on the
//     first fire and `Restart()`ed on every one after. Created lazily and not
//     at bind: an emitter that exists is an emitter that emits, and the
//     attachments of a sequence nobody plays should cost nothing. Creation
//     happens inside the evaluation pass and `ApplyD3ParticleFrames` runs
//     immediately after it in the same `ApplyFrameState`, so a new emitter is
//     placed on the frame it appears rather than spending one at the origin.
//
//   * **A whole other model.** A group 1 payload is an `.acr` —
//     `Actor_SpawnFromSno` builds a full ACD with its own Appearance and
//     AnimSet. The pool cannot spawn one itself: that mutates the scene's actor
//     map, which the evaluation walk it ticks inside is iterating. So it
//     reports the request and `FrameTicker::DriveD3Attachments` drains it a
//     pass later, exactly as `SpnSpawner` does.
//
// A re-fired child is re-birthed rather than duplicated. The engine really does
// spawn a second ACD, but a looping viewer would then grow one model per lap;
// this is `ApplyAttachmentStates`' rule for an MDX attachment coming back into
// view, applied for the same reason.
// ============================================================================

#include "whiteout/flakes/types.h"

#include <memory>
#include <unordered_map>
#include <vector>

namespace whiteout::flakes::io {
class D3ModelAdapter;
class D3SnoCache;
} // namespace whiteout::flakes::io
namespace whiteout::flakes::renderer::model {
struct Actor;
}
namespace whiteout::flakes::renderer::particle {
class ParticleService;
}

namespace whiteout::flakes::renderer::effects {

class D3AttachmentPool {
public:
    /// @brief One child model an attachment asked for, for the FrameTicker.
    struct PendingChild {
        i32 snoActor = -1;
        i32 bone = -1; ///< Of the PARENT's skeleton; -1 is its origin.
        Matrix44f offset = Matrix44f::identity();
        /// Non-zero when this attachment has already spawned its child once:
        /// the request is then a re-birth, not a second model.
        u32 existing = 0;
        /// Where the answer goes back — see @ref NoteChildSpawned.
        i32 sequence = -1;
        i32 entry = -1;
    };

    /// @brief Adopt the actor's D3 half.
    ///
    /// @p firstEmitterId is one past the last id the load-time route used, so
    /// the two never collide in the particle service's id space.
    void Bind(std::shared_ptr<io::D3ModelAdapter> adapter, io::D3SnoCache* cache,
              i32 firstEmitterId);

    bool Empty() const {
        return adapter_ == nullptr;
    }

    /// @brief Fire everything sequence @p activeSeq crossed since the last tick.
    ///
    /// @p seqStartMs / @p seqEndMs are the sequence's window, which for a D3
    /// clip always starts at zero; they are taken rather than assumed so the
    /// crossing arithmetic is the one every other format uses.
    void Tick(const model::Actor& actor, i32 activeSeq, i32 localTimeMs, i32 seqStartMs,
              i32 seqEndMs, particle::ParticleService* particles);

    /// @brief Child spawns requested since the last drain.
    std::vector<PendingChild> TakePending();

    /// @brief Record what a drained request produced. A zero handle marks the
    ///        entry dead, so an `.acr` that does not resolve is not retried on
    ///        every lap of the animation.
    void NoteChildSpawned(i32 sequence, i32 entry, u32 handle);

    /// @brief The child of (@p sequence, @p entry), or 0.
    u32 ChildHandle(i32 sequence, i32 entry) const;

private:
    struct Entry {
        /// One element. Kept as a vector so the crossing arithmetic in
        /// `event_crossing.h` — the same one MDX, `.m2` and `.m3` use — takes
        /// it unchanged.
        std::vector<u32> times;
        i32 snoActor = -1;    ///< Group 1 payload, or -1.
        i32 snoParticle = -1; ///< Group 27 payload, or -1.
        i32 bone = -1;
        Matrix44f offset = Matrix44f::identity();
        i32 emitterId = -1;  ///< Assigned on the first fire.
        u32 childHandle = 0; ///< Actor payloads; 0 until the spawn lands.
        bool dead = false;   ///< Asked for and refused. Stop asking.
    };

    /// @brief Sequence @p seq's entries, resolving the clip on first use.
    std::vector<Entry>* Resolve(i32 seq);

    std::shared_ptr<io::D3ModelAdapter> adapter_;
    io::D3SnoCache* cache_ = nullptr;
    /// Per sequence, because a clip is resolved (and possibly fetched) the
    /// first time it plays and never again.
    std::unordered_map<i32, std::vector<Entry>> bySequence_;
    std::vector<PendingChild> pending_;
    i32 nextEmitterId_ = 0;
    i32 prevSeq_ = -1;
    i32 prevTimeMs_ = 0;
};

} // namespace whiteout::flakes::renderer::effects
