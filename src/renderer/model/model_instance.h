#pragma once

#include "../gfx/gfx.h"
#include "common_types.h"
#include "animation/animation_driver.h"
#include "effects/event_emitter_pool.h"
#include "model/model_source.h"
#include "model/model_template.h"
#include "model/render_model.h"
#include "render_target.h"  // RenderMode

#include <memory>
#include <vector>

namespace whiteout::flakes::renderer::animation { struct ActorEvalContext; }

namespace whiteout::flakes::renderer::model {

struct Actor {
    u32       handle  = 0;
    bool      isFocus = false;

    Matrix44f worldTransform = Matrix44f::identity();

    animation::AnimationDriver animation;

    i32 sequenceStartTimeMs = 0;
    i32 prevActiveSequence  = -1;

    bool externallyDriven = false;

    bool ignoreNonLooping = false;

    struct AttachmentSlot {
        AttachmentConfig config;
        u32 childModelHandle = 0;
        bool loaded = false;
        bool wasVisible = false;
    };
    std::vector<AttachmentSlot> attachmentSlots;

    f32 parentVisibility = 1.0f;

    u32 parent     = 0;

    i32  pe1Depth   = 0;
    bool isPE1Child = false;

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
