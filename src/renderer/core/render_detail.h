#pragma once

#include "core/draw_list.h"
#include "core/surface_table.h"
#include "gfx/gfx.h"
#include "renderer/model/model_instance.h"
#include "renderer/types.h"
#include "whiteout/flakes/types.h"

#include <memory>
#include <unordered_map>
#include <vector>

namespace whiteout::flakes::renderer::bls {
struct FrameInputs;
}

namespace whiteout::flakes::renderer::assets {
class SamplerAssetManager;
}

namespace whiteout::flakes::renderer::shading {
class IShadingModel;
class ShadingRegistry;
}

namespace whiteout::flakes::renderer::render_detail {

struct RenderableView {
    const std::vector<model::GPUGeoset>* geosets = nullptr;
    const core::ISurfaceTable* surfaceTable = nullptr;
    assets::TextureAssetManager::ModelScope* textures = nullptr;
    const animation::SkinningSystem* skinning = nullptr;
    const std::vector<model::RenderModel::TexAnimPaletteEntry>* texAnimPalette = nullptr;
    const std::vector<model::RenderModel::SurfaceAnim>* surfaceAnim = nullptr;
    Matrix44f worldTransform = Matrix44f::identity();
    // Game units per renderer unit, from Actor::worldScale — 1 for Warcraft III
    // and 100 for World of Warcraft. `worldTransform` already folds it in; this
    // is here for the shading models that need the factor itself, which so far
    // is anything reading a falloff radius authored in game units.
    f32 worldScale = 1.0f;
    f32 parentVisibility = 1.0f;
    bool mirrored = false; // reversed winding — see Actor::mirrored
    bool hasLods = false;
    u32 teamColor = 0x000000FFu;

    // Structural actor identity for the draw trace (see debug/draw_trace.h).
    // `Actor::handle` is not usable here: child handles come from
    // AllocActorId() called while iterating unordered maps, so which child
    // gets which number is hash-order dependent. The top-level ancestor's
    // handle plus (role, depth, emitter/slot) is a function of the scene.
    u32 rootActor = 0;
    u8 actorRole = 0;
    u8 actorDepth = 0;
    i32 spawnEmitterId = -1;
    i32 spawnSlotIndex = -1;
};

// The point WC3's RenderGeosetPrep feeds to GxuLightSelect: the geoset's local
// centroid pushed through the actor's world transform. Also what the
// transparent pass sorts by, so the two agree by construction.
inline Vector3f GeosetCentroidWS(const RenderableView& view, const model::GPUGeoset& geo) {
    return whiteout::transform_point(geo.localCentroid, view.worldTransform);
}

// Owns the per-actor views the draw items point into (kept stable for the
// frame), plus the classified + sorted opaque/transparent lists. Replaces the
// per-geoset bucket model with WC3's per-layer opaque / whole-geoset transparent
// split (see ClassifyGeoset / draw_list.h).
struct CollectedDrawLists {
    std::vector<RenderableView> views;
    DrawLists lists;
    // Every visible actor's lights, already in world space. WC3 keeps one
    // global registry any geoset can draw from; collecting per scene here is
    // the same thing scoped to what BuildDrawLists was handed.
    std::vector<model::FrameState::LightState> sceneLights;
};

// `shadingModel` both stamps every item's SurfaceKey and answers what each
// geoset currently classifies as. Classification is asked every frame and is
// never cached: WC3's rule is genuinely animated — it reports "some layer is
// *currently* visible" and "this HD opaque layer has *faded* below full" — so
// SurfaceKey::blend is a load-time bucket-reservation hint and nothing here
// reads it.
//
// One WC3 model is live per frame — RenderMode picks which — so it also
// decides every WC3 surface's SurfaceKey::model. Actors carrying their own
// `shadingModel` (M2/M3, which name UnlitShading) override that per actor,
// which is what lets a foreign model draw beside a WC3 one. Deriving it from
// layer.shaderId instead would route a shaderId-1 surface in SD mode to the HD
// model — a behaviour change disguised as a refactor, since the classification
// rule is render-mode independent and applies the HD fading test regardless.
// `unlitOddGeosets` is RenderSettings::DebugUnlitOddGeosets — see there for
// why the multi-model toggle has to be per-geoset. It changes only which model
// each item *names*; classification and collection stay the WC3 model's, so
// with the flag false every key is what it was.
// `m2DistanceSortGeometry` is RenderSettings::M2DistanceSortGeometry. Off, a
// transparent `.m2` geo item gets sort distance 0 — what the client passes —
// so distance ties across all geometry and TransparentOrder falls through to
// priorityPlane and materialLayer. WC3 items always carry their real distance.
CollectedDrawLists BuildDrawLists(
    const std::unordered_map<u32, std::unique_ptr<model::Actor>>& models, i32 selectedLod,
    const Vector3f& cameraPos, const shading::IShadingModel& shadingModel,
    bool unlitOddGeosets = false, const shading::ShadingRegistry* registry = nullptr,
    bool m2DistanceSortGeometry = false);

// Bind a RenderableView to an actor's render state. Shared by BuildDrawLists
// and by the shadow pass, which classifies casters with the same rule the
// scene passes use but runs before the draw lists are collected.
void FillRenderableView(RenderableView& view, model::Actor& mi,
                        const std::unordered_map<u32, std::unique_ptr<model::Actor>>& actors);

// `paletteCb` is the bone-palette CB to bind when this geoset has
// skinning data. Pass `geo.bonePaletteCb` directly when the actor is
// on Path B (per-geoset palette); pass `actor.skinning.ActorPaletteCb()`
// when the actor is on Path A. The caller picks because BindSdMeshGeometry
// itself has no view of the actor. Skinning is considered active iff
// both `geo.boneVb` and `paletteCb` are valid.
bool BindSdMeshGeometry(gfx::IGFXCommandList* cmd, const model::GPUGeoset& geo,
                        gfx::BufferHandle paletteCb, i32 coordId = 0);

gfx::BufferHandle PickSlot0Vb(const model::GPUGeoset& geo, i32 coordId);

void BindLayerAlbedo(gfx::IGFXCommandList* cmd, assets::TextureAssetManager::ModelScope* scope,
                     i32 textureId, gfx::TextureHandle defaultTex,
                     assets::SamplerAssetManager& samplers, u32 slot = 0);

struct CbPerFrameDesc {
    Matrix44f world = Matrix44f::identity();
    Matrix44f view = Matrix44f::identity();
    Matrix44f projection = Matrix44f::identity();
    Vector4f lightDir = {0.0f, 0.0f, 0.0f, 0.0f};
    Vector4f lightColor = {1.0f, 1.0f, 1.0f, 1.0f};
    Vector4f ambientColor = {1.0f, 1.0f, 1.0f, 0.0f};
    Vector4f extraParams = {1.0f, 1.0f, 1.0f, 1.0f};
    Vector4f texAnimParams = {0.0f, 0.0f, 1.0f, 1.0f};
    Vector4f materialFlags = {0.0f, 0.0f, 0.0f, 0.0f};
};

void WriteCbPerFrame(gfx::IGFXDevice* gfx, gfx::BufferHandle cb, const CbPerFrameDesc& d);

Vector4f NormalizedLightDir4(const Vector4f& dir);

void ApplyTexAnimPaletteToFrame(bls::FrameInputs& frame,
                                const std::vector<model::RenderModel::TexAnimPaletteEntry>* palette,
                                i32 textureAnimationId);

} // namespace whiteout::flakes::renderer::render_detail
