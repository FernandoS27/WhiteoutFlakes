#pragma once

#include "../gfx/gfx.h"
#include "common_types.h"
#include "animation_driver.h"
#include "event_emitter_pool.h"
#include "model_source.h"
#include "render_model.h"

#include <memory>
#include <vector>

namespace WhiteoutDex {

struct ModelTemplate;

struct Actor {
    u32       handle  = 0;
    bool      isFocus = false;

    Matrix44f worldTransform = Matrix44f::identity();

    AnimationDriver animation;

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

    EventEmitterPool events;

    RenderModel&       Render()       { return render; }
    const RenderModel& Render() const { return render; }

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
