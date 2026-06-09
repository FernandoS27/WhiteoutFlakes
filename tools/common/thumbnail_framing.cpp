#include "thumbnail_framing.h"

#include "io/mdx_model_adapter.h"
#include "renderer/camera.h"
#include "renderer/model/model_instance.h"
#include "renderer/model/model_template.h"
#include "renderer/render_service.h"

#include <whiteout/models/mdx/mdx.h>

#include <algorithm>
#include <cctype>
#include <string>

namespace whiteout::flakes::tools {

using renderer::Camera;

namespace {
std::string Lower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Pose the camera given a finished extent accumulation (or its absence).
void ApplyFraming(Camera& cam, bool have, const Vector3f& lo, const Vector3f& hi) {
    Vector3f center{0.0f, 0.0f, 50.0f};
    f32 maxAxis = 260.0f;
    if (have) {
        center = {(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f};
        maxAxis = (std::max)({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z});
    }
    maxAxis = (std::max)(maxAxis, 1.0f);
    cam.SetTarget(center);
    cam.SetYaw(Camera::kDefaultYaw - 0.785398f);
    cam.SetPitch(0.6f);
    cam.SetDistance(maxAxis * 1.0f);
}
} // namespace

int PickStandSequenceIndex(renderer::model::Actor* hero) {
    if (!hero)
        return -1;
    const auto seqs = hero->animation.Sequences();
    if (seqs.empty())
        return -1;
    for (int i = 0; i < static_cast<int>(seqs.size()); ++i)
        if (Lower(seqs[i].name).find("stand") != std::string::npos)
            return i;
    return 0; // no stand → first animation
}

bool IsHdModel(renderer::model::Actor* hero) {
    if (!hero || !hero->sourceTemplate || !hero->sourceTemplate->adapter)
        return false;
    const whiteout::mdx::Model& m = hero->sourceTemplate->adapter->SourceModel();
    for (const auto& mat : m.materials)
        for (const auto& layer : mat.layers)
            if (layer.shader != whiteout::mdx::Layer::ShaderType::SD)
                return true; // any non-SD layer ⇒ Reforged HD model
    return false;
}

bool ApplyModelCamera(Camera& cam, renderer::model::Actor* hero) {
    if (!hero || !hero->sourceTemplate || hero->sourceTemplate->cameraPresets.empty())
        return false;
    const auto& preset = hero->sourceTemplate->cameraPresets.front();
    cam.SetDirectPose(preset.position, preset.target, preset.staticRoll);
    const f32 fov = (preset.fovDiagonal > 1e-3f) ? preset.fovDiagonal : Camera::kDefaultFovDiagonal;
    cam.SetFovDiagonal(fov);
    cam.SetClip(preset.zNear, preset.zFar);
    return true;
}

void FrameCameraToModelSequence(Camera& cam, renderer::model::Actor* hero,
                                const std::string& sequenceName) {
    cam.SetOrbitalMode();
    Vector3f lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    bool have = false;
    auto consume = [&](const whiteout::mdx::Extent& e) {
        if (e.maximum.x <= e.minimum.x && e.maximum.y <= e.minimum.y &&
            e.maximum.z <= e.minimum.z)
            return;
        lo.x = (std::min)(lo.x, e.minimum.x);
        lo.y = (std::min)(lo.y, e.minimum.y);
        lo.z = (std::min)(lo.z, e.minimum.z);
        hi.x = (std::max)(hi.x, e.maximum.x);
        hi.y = (std::max)(hi.y, e.maximum.y);
        hi.z = (std::max)(hi.z, e.maximum.z);
        have = true;
    };
    if (hero && hero->sourceTemplate && hero->sourceTemplate->adapter) {
        const whiteout::mdx::Model& m = hero->sourceTemplate->adapter->SourceModel();
        // The chosen sequence's own extent.
        const std::string want = Lower(sequenceName);
        for (const auto& s : m.sequences)
            if (Lower(s.name) == want)
                consume(s.extent);
        // Fall back to geoset bind-pose / model extents if that was degenerate.
        if (!have)
            for (const auto& gs : m.geosets)
                consume(gs.extent);
        if (!have)
            consume(m.modelExtent);
    }
    ApplyFraming(cam, have, lo, hi);
}

void FrameCameraToModel(Camera& cam, renderer::model::Actor* hero) {
    cam.SetOrbitalMode();

    // Model AABB from the union of the per-sequence extents — these bound the
    // model as it actually animates. Death / dissipate / birth sequences are
    // skipped: they fling, collapse or scale the model and would bloat the box.
    // Geoset bind-pose / modelExtent are fallbacks for degenerate extents.
    Vector3f lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    bool have = false;
    auto consume = [&](const whiteout::mdx::Extent& e) {
        if (e.maximum.x <= e.minimum.x && e.maximum.y <= e.minimum.y &&
            e.maximum.z <= e.minimum.z)
            return; // degenerate / unset
        lo.x = (std::min)(lo.x, e.minimum.x);
        lo.y = (std::min)(lo.y, e.minimum.y);
        lo.z = (std::min)(lo.z, e.minimum.z);
        hi.x = (std::max)(hi.x, e.maximum.x);
        hi.y = (std::max)(hi.y, e.maximum.y);
        hi.z = (std::max)(hi.z, e.maximum.z);
        have = true;
    };
    auto excluded = [](std::string name) {
        for (char& c : name)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return name.find("death") != std::string::npos ||
               name.find("dissipate") != std::string::npos ||
               name.find("birth") != std::string::npos;
    };
    if (hero && hero->sourceTemplate && hero->sourceTemplate->adapter) {
        const whiteout::mdx::Model& m = hero->sourceTemplate->adapter->SourceModel();
        for (const auto& s : m.sequences)
            if (!excluded(s.name))
                consume(s.extent);
        if (!have)
            for (const auto& gs : m.geosets)
                consume(gs.extent);
        if (!have)
            consume(m.modelExtent);
    }

    Vector3f center{0.0f, 0.0f, 50.0f};
    f32 maxAxis = 260.0f; // fallback when the model carries no usable extents
    if (have) {
        center = {(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f};
        maxAxis = (std::max)({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z});
    }
    maxAxis = (std::max)(maxAxis, 1.0f);

    cam.SetTarget(center);
    cam.SetYaw(Camera::kDefaultYaw - 0.785398f); // −45° toward the left
    cam.SetPitch(0.6f);                          // ~34° up
    cam.SetDistance(maxAxis * 1.0f);
}

bool FrameCameraToEffect(renderer::RenderService& svc, Camera& cam, u32 actorId) {
    // The renderer owns the particle data + corn↔game scale; it computes the
    // live world-space AABB (of the ACTIVE scene's corn service). Caller makes
    // the effect's scene active first.
    Vector3f lo{}, hi{};
    if (!svc.ComputeEffectWorldBounds(actorId, /*emitterId*/ 0, lo, hi))
        return false; // no live particles yet — retry next frame

    const Vector3f center{(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f};
    f32 maxAxis = (std::max)({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z});
    maxAxis = (std::max)(maxAxis, 30.0f); // floor for tiny / point effects

    cam.SetTarget(center);
    cam.SetDistance(maxAxis * 1.3f); // a little margin around the cloud
    return true;
}

} // namespace whiteout::flakes::tools
