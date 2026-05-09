#pragma once

#include "../gfx/gfx.h"
#include "whiteout/flakes/types.h"
#include "animation/animation_driver.h"
#include "effects/event_emitter_pool.h"
#include "whiteout/flakes/model_source.h"
#include "model/model_template.h"
#include "model/render_model.h"
#include "render_target.h"  // RenderMode

#include <memory>
#include <vector>

namespace whiteout::flakes::renderer::animation { struct ActorEvalContext; }

namespace whiteout::flakes::renderer::model {

// Why an actor is in the scene. Determines which simulation path drives its
// transform / lifetime / sequence selection. Units are spawned by the app
// and represent top-level renderable entities; External units are also
// app-spawned but their evaluation is hand-driven by the host (Max plugin
// scrubs Max's timeline). Children are spawned by their parent's effects
// and live in `parent->children`.
enum class ActorRole : u8 {
    Unit,        // top-level, app-spawned, normal scene-clock evaluation
    External,    // top-level, host evaluates manually (Max plugin)
    Attachment,  // parent owns slot config; transform pushed from parent FrameState
    PE1,         // parent's PE1 sim drives transform + lifetime
    SPN,         // event-spawned, frozen transform, sequence-duration lifetime
};

struct Actor {
    u32       handle  = 0;

    Matrix44f worldTransform = Matrix44f::identity();

    animation::AnimationDriver animation;

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
        i32 actorTimeMs         = 0;
        i32 sequenceStartTimeMs = 0;
        i32 prevActiveSequence  = -1;
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
    u32  teamColor      = 0x000000FFu;
    bool teamColorDirty = false;

    void SetTeamColor(u8 r, u8 g, u8 b) {
        teamColor      = (u32)r | ((u32)g << 8) | ((u32)b << 16);
        teamColorDirty = true;
    }

    // ---- Tree position ----
    ActorRole role      = ActorRole::Unit;
    u32       parent    = 0;       // 0 for top-level (Unit/External)
    i32       treeDepth = 0;       // top-level = 0; children = parent.treeDepth + 1
    std::vector<u32> children;     // canonical owner list of child handles

    bool IsChild() const { return role != ActorRole::Unit && role != ActorRole::External; }

    struct AttachmentSlot {
        AttachmentConfig config;
        u32 childModelHandle = 0;   // also present in `children`; this is the slot's own ref
        bool loaded = false;
        bool wasVisible = false;
    };
    std::vector<AttachmentSlot> attachmentSlots;

    f32 parentVisibility = 1.0f;

    std::shared_ptr<ModelTemplate> sourceTemplate;

    RenderModel render;

    effects::EventEmitterPool events;

    RenderModel&       Render()       { return render; }
    const RenderModel& Render() const { return render; }

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
        for (auto& g : render.gpuGeosets) g.Release(gfx, freeShared);
        render.gpuGeosets.clear();
        if (render.textures) render.textures->Clear();
        render.gpuMaterials.clear();
        gfx.Destroy(render.ribbonVB);
        render.ribbonVB = gfx::BufferHandle::Invalid;
        render.ribbonVBSize = 0;

        sourceTemplate.reset();
    }
};

}
