#pragma once

#include "../gfx/gfx.h"
#include "animation/animation_driver.h"
#include "effects/event_emitter_pool.h"
#include "model/model_template.h"
#include "core/surface_vocabulary.h"
#include "model/render_model.h"
#include "render_target.h" // RenderMode
#include "whiteout/flakes/model_source.h"
#include "whiteout/flakes/types.h"
#include "whiteout/flakes/util/coordinate_system.h"

#include <memory>
#include <vector>

namespace whiteout::flakes::renderer::animation {
struct ActorEvalContext;
}

namespace whiteout::flakes::renderer::model {

// Why an actor is in the scene. Determines which simulation path drives its
// transform / lifetime / sequence selection. Units are spawned by the app
// and represent top-level renderable entities; External units are also
// app-spawned but their evaluation is hand-driven by the host (Max plugin
// scrubs Max's timeline). Children are spawned by their parent's effects
// and live in `parent->children`.
// Caps on child-model ("particles that ARE models") spawning. Actor-tree
// policy, enforced where actors are created rather than in the particle sim:
// each instance is a full Actor with GPU resources. Raise with care.
constexpr i32 kMaxChildModelDepth = 3;
constexpr i32 kMaxChildModelInstances = 256;

enum class ActorRole : u8 {
    Unit,       // top-level, app-spawned, normal scene-clock evaluation
    External,   // top-level, host evaluates manually (Max plugin)
    Attachment, // parent owns slot config; transform pushed from parent FrameState
    PE1,        // parent's PE1 sim drives transform + lifetime
    SPN,        // event-spawned, frozen transform, sequence-duration lifetime
};

struct Actor {
    u32 handle = 0;

    Matrix44f worldTransform = Matrix44f::identity();

    // Game units → renderer units, from the profile that owns this actor's
    // product. Stamped once at StageActor, because it is a property of the
    // format the template came from and never changes afterwards.
    //
    // Separate from `worldTransform` because that one belongs to the *host*:
    // the viewer and the Max plugin write it, in game units, and would
    // overwrite a scale folded into it. So it is applied where the renderer
    // consumes the transform, not where the host writes it — see
    // ScaledWorldTransform.
    //
    // 1.0 for Warcraft III, which authors in renderer units already. A World
    // of Warcraft creature is 2–5 yards against camera constants sized in the
    // hundreds, which is the whole reason this exists.
    f32 worldScale = 1.0f;

    // The axis convention the actor's geometry is authored in, from the same
    // profile as `worldScale` and stamped at the same moment.
    //
    // Applied on the transform rather than baked into the vertices, and that
    // is now forced rather than merely tidy: a MeshBuffer is uploaded to the
    // GPU byte for byte, so there is no CPU-side vertex array left to rotate.
    // Bone matrices and particle samples ride the same transform, so one
    // basis change covers all three.
    //
    // Blizzard (== renderer-native) for Warcraft III and World of Warcraft;
    // StarCraft II and Heroes author +Y forward instead of +X.
    CoordSpace sourceSpace = kDefaultCoordSpace;

    // The actor's model-space bounding box, in the same units `worldScale`
    // converts from. Stamped at spawn by both routes, because it is the only
    // thing an actor can be framed by and `sourceTemplate` is null for every
    // actor built from a live IModelSource — which is every `.m2` and `.m3`,
    // and the Max plugin's live scene. Framing through the template alone left
    // those at a hardcoded fallback distance.
    //
    // Invalid when the source reported no usable box; callers fall back rather
    // than trusting a degenerate one.
    ModelBounds bounds;

    // Which shading model draws this actor's surfaces, when it is not the one
    // the active profile selects. `None` means "ask the profile", which is
    // every Warcraft III actor.
    //
    // Per-actor rather than per-scene because a scene can legitimately hold
    // both: the model explorer renders thumbnails of whatever it is pointed
    // at, and P10 adds a third format. An M2 sets this to Unlit because it has
    // no materials at all — handing it to a WC3 model would mean asking for a
    // surface table that was never built.
    core::ShadingModelId shadingModel = core::ShadingModelId::None;

    // The transform the renderer draws and poses with.
    //
    // The identity case returns the host's matrix untouched rather than
    // multiplying by a unit scale. Not an optimisation: `Scale(1) * M` is
    // `M[i][j] + 0 + 0 + 0`, which flips a genuine `-0.0` to `+0.0`, and a
    // rotation matrix has plenty of those. Skipping the multiply is what makes
    // "identity for WC3" a fact about bits rather than an argument about
    // floating point.
    const Matrix44f& ScaledWorldTransform() const {
        const bool rebase = sourceSpace != kDefaultCoordSpace;
        if (worldScale == 1.0f && !rebase)
            return worldTransform;
        // Basis change first, then scale, then the host's matrix: the first
        // two act on model-space geometry, and the host writes the last one in
        // world space. The scale is uniform so it commutes with the rotation;
        // the order is written the way it reads rather than to matter.
        Matrix44f m = Matrix44f::scaling({worldScale, worldScale, worldScale});
        if (rebase)
            m = CoordinateSystem::BasisChange(sourceSpace, kDefaultCoordSpace) * m;
        scaledWorld_ = m * worldTransform;
        return scaledWorld_;
    }

    animation::AnimationDriver animation;

    // Cache for ScaledWorldTransform; never read unless worldScale != 1.
    mutable Matrix44f scaledWorld_ = Matrix44f::identity();

    // Per-actor playback clock. The host calls Advance(dt) every frame; the
    // method scales dt by playbackSpeed and feeds the actor's animation
    // cursor. Two actors of the same MDX can run at totally different rates
    // (e.g., one paused at speed=0, one at 2x), or have their cursors set
    // explicitly via animation.SetTimeMs (Max plugin scrubs the timeline).
    //
    // `cursor` holds Advance()'s scratch state — actor-local clock plus the
    // bookkeeping it needs to detect sequence transitions. Hosts shouldn't
    // touch these fields; AncestorActorTimeMs() is the read API.
    f32 playbackSpeed = 1.0f;
    struct Cursor {
        i32 actorTimeMs = 0;
        i32 sequenceStartTimeMs = 0;
        i32 prevActiveSequence = -1;
        // Increments each time the active sequence wraps (looped or
        // ignoreNonLooping-forced-loop). Consumers (corn-fx emitters
        // flagged IsNonLoopingEffect) read this to re-fire per loop.
        i32 sequenceCycle = 0;
    };
    Cursor cursor;

    // Advance this actor's playback clock by dt seconds and update its
    // animation cursor (sequence wrapping / non-looping clamp). Only meaningful
    // for Unit actors driven by the renderer's clock; External actors set the
    // cursor directly, and PE1/SPN children derive it from wall clock - birth.
    void Advance(f32 dtSec);

    bool ignoreNonLooping = false;

    // Team color, packed 0x00BBGGRR (so byte 0 = red, byte 1 = green,
    // byte 2 = blue, alpha implicit 0xFF). Default is red (team 1).
    // Children inherit the parent's color at spawn time. Use SetTeamColor
    // to mutate at runtime — it sets teamColorDirty so the
    // ReplaceableTextureManager re-bakes this actor's slots on the next
    // frame. Direct writes to the field are allowed but bypass the rebake.
    u32 teamColor = 0x000000FFu;
    bool teamColorDirty = false;

    void SetTeamColor(u8 r, u8 g, u8 b) {
        teamColor = (u32)r | ((u32)g << 8) | ((u32)b << 16);
        teamColorDirty = true;
    }

    // ---- Tree position ----
    ActorRole role = ActorRole::Unit;
    u32 parent = 0;            // 0 for top-level (Unit/External)
    i32 treeDepth = 0;         // top-level = 0; children = parent.treeDepth + 1
    std::vector<u32> children; // canonical owner list of child handles

    // What in the parent produced this child: the PE1 emitter id, or the
    // attachment slot index. `handle` alone is not an identity a recording can
    // be keyed on — AllocActorId() hands out numbers in spawn order, and the
    // spawn walks iterate hash maps — so the draw trace records these instead.
    i32 spawnEmitterId = -1;
    i32 spawnSlotIndex = -1;

    bool IsChild() const {
        return role != ActorRole::Unit && role != ActorRole::External;
    }

    struct AttachmentSlot {
        AttachmentConfig config;
        // AssetManager slot for the child MDX. Acquired on first poll
        // and held for the actor's lifetime; the slot's payload swaps
        // from null → parsed ModelTemplate when the host pump finishes
        // the fetch.
        std::uint32_t assetSlot = 0;
        u32 childModelHandle = 0; // also present in `children`; this is the slot's own ref
        bool loaded = false;
        bool wasVisible = false;
    };
    std::vector<AttachmentSlot> attachmentSlots;

    // AssetManager slots this actor holds for the duration of its
    // lifetime — typically the PE1 child-MDX paths from the template's
    // pe1Configs, prefetched once at StageActor time. DestroyActor
    // releases them; without this hold the slots' refcounts could
    // either churn (allocate-destroy on every prefetch attempt) or
    // accumulate unbounded if any code path Acquires without Releasing.
    std::vector<std::uint32_t> assetSlots;

    f32 parentVisibility = 1.0f;

    // Draw this actor with the winding reversed. `CM2Model::SetMirrored` is the
    // client's only caller of SetReverseCulling, and that walks the child list —
    // so a mirrored actor flips culling on everything attached to it too, which
    // is what the propagation in UpdateAttachmentChildren reproduces. Nothing
    // sets this yet; a negative-scale host would.
    bool mirrored = false;

    std::shared_ptr<ModelTemplate> sourceTemplate;

    RenderModel render;

    effects::EventEmitterPool events;

    RenderModel& Render() {
        return render;
    }
    const RenderModel& Render() const {
        return render;
    }

    // True if this actor's template prefers the HD pipeline. The application
    // is responsible for calling Settings().SetRenderMode() based on this —
    // the renderer no longer flips render mode automatically on load.
    RenderMode PreferredRenderMode() const {
        return sourceTemplate ? sourceTemplate->PreferredRenderMode() : RenderMode::SD;
    }

    // ---- Per-frame evaluation ----
    // Evaluates this actor's animation source and applies the result. Owns
    // bone matrix application, particle/ribbon/PE1 frame state forwarding,
    // attachment updates, and event firing. Caller (RenderService::Tick or
    // Max plugin) supplies a context built via RenderService::MakeActorEvalContext.
    void EvaluateAndApply(const animation::ActorEvalContext& ctx);

    // Apply a pre-computed FrameState to this actor. RenderService::Tick uses
    // this when it batches Evaluate() calls and applies them in a second pass.
    void ApplyFrameState(const FrameState& state, i32 localTimeMs,
                         const animation::ActorEvalContext& ctx);

    void ReleaseGPU(gfx::IGFXDevice& gfx) {
        const bool freeShared = !sourceTemplate;
        for (auto& g : render.gpuGeosets)
            g.Release(gfx, freeShared);
        render.gpuGeosets.clear();
        if (render.textures)
            render.textures->Clear();
        render.surfaceTable.reset();
        render.surfaces.clear();
        gfx.Destroy(render.ribbonVB);
        render.ribbonVB = gfx::BufferHandle::Invalid;
        render.ribbonVBSize = 0;

        // Per-actor bone palette CB (Path A). Invalid for Path B
        // actors so Destroy is a no-op there. Always owned by this
        // actor instance even when SkinningData is shared via a
        // template — the buffer is per-instance state.
        gfx.Destroy(render.skinning.ActorPaletteCb());
        render.skinning.SetActorPaletteCb(gfx::BufferHandle::Invalid);

        sourceTemplate.reset();
    }
};

} // namespace whiteout::flakes::renderer::model
