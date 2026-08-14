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
    // Genuinely an MDX question — it reads Reforged layer shader types, which
    // no other format has. The downcast says so instead of the type system
    // pretending every template is MDX. A non-MDX model is not HD.
    const auto* mdxAdapter =
        dynamic_cast<const io::MdxModelAdapter*>(hero->sourceTemplate->adapter.get());
    if (!mdxAdapter)
        return false;
    const whiteout::mdx::Model& m = mdxAdapter->SourceModel();
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
    // Per-*sequence* extents, which only MDX exposes: SequenceInfo carries no
    // bounds, and inventing them for every format to serve a thumbnail helper
    // would be a public-API change for one caller. A non-MDX model falls
    // through to ApplyFraming's no-extent path, and FrameCameraToModel below
    // is the format-neutral one.
    const io::MdxModelAdapter* mdxAdapter =
        (hero && hero->sourceTemplate)
            ? dynamic_cast<const io::MdxModelAdapter*>(hero->sourceTemplate->adapter.get())
            : nullptr;
    if (mdxAdapter) {
        const whiteout::mdx::Model& m = mdxAdapter->SourceModel();
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

    // The actor's own bounds. Format-neutral on purpose: this used to reach
    // through the template into whiteout::mdx::Model, which meant a non-MDX
    // model got no extents at all and fell back to a distance of 260 against
    // camera constants sized in the hundreds — a World of Warcraft creature is
    // 2–5 yards, so it rendered as a sub-pixel dot that a golden image cannot
    // distinguish from a load failure.
    //
    // Read off the *actor*, not the template: an actor spawned from a live
    // IModelSource has no template at all — every `.m2` and `.m3`, and the Max
    // plugin's live scene — so a template-only read put exactly those back on
    // the fallback distance.
    //
    // The value is unchanged for MDX: StageActor copies the template's bounds
    // onto the actor verbatim, and MdxModelAdapter::GetBounds applies exactly
    // the rule that used to live here (union of non-death / non-dissipate /
    // non-birth sequence extents, falling back to geoset extents then the
    // model extent), so no WC3 camera moves.
    Vector3f lo{0, 0, 0}, hi{0, 0, 0};
    bool have = false;
    if (hero && hero->bounds.valid) {
        lo = hero->bounds.min;
        hi = hero->bounds.max;
        have = true;
    } else if (hero && hero->sourceTemplate && hero->sourceTemplate->bounds.valid) {
        // Belt and braces for the template path: StageActor copies these onto
        // the actor, so reaching here means an actor was built some other way.
        // Falling back keeps a WC3 camera where it was rather than dropping to
        // the 260 fallback, which no gate would catch — the goldens use a fixed
        // camera and never exercise framing at all.
        lo = hero->sourceTemplate->bounds.min;
        hi = hero->sourceTemplate->bounds.max;
        have = true;
    }

    Vector3f center{0.0f, 0.0f, 50.0f};
    f32 maxAxis = 260.0f; // fallback when the model carries no usable extents
    if (have) {
        // Bounds are model-space; the renderer draws through
        // ScaledWorldTransform, so the camera has to frame the scaled extent
        // or a WoW creature sits 100× closer than the box it is framed against.
        // Exactly 1.0 for Warcraft III, so no WC3 camera moves.
        const f32 s = (hero->worldScale > 0.0f) ? hero->worldScale : 1.0f;
        // …and through the same basis change, for a product that does not
        // author in renderer axes. Both corners are converted and re-min/maxed
        // rather than the centre alone: the conversion is a signed axis
        // permutation, so it can swap which corner is the minimum. The longest
        // axis survives it either way, but the centre does not — an SC2 model
        // framed on an unconverted centre sits off to one side.
        const CoordSpace src = hero->sourceSpace;
        if (src != kDefaultCoordSpace) {
            const Vector3f a = CoordinateSystem::ToDefault(src, lo);
            const Vector3f b = CoordinateSystem::ToDefault(src, hi);
            lo = {(std::min)(a.x, b.x), (std::min)(a.y, b.y), (std::min)(a.z, b.z)};
            hi = {(std::max)(a.x, b.x), (std::max)(a.y, b.y), (std::max)(a.z, b.z)};
        }
        center = {(lo.x + hi.x) * 0.5f * s, (lo.y + hi.y) * 0.5f * s, (lo.z + hi.z) * 0.5f * s};
        maxAxis = (std::max)({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z}) * s;
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
