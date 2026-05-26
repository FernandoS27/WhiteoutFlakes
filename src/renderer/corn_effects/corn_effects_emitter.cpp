#include "renderer/corn_effects/corn_effects_emitter.h"

#include "renderer/assets/asset_manager.h"

#include <cornflakes/interface/asset/effect_asset_model.hpp>
#include <cornflakes/interface/binding/effect_execution_plan.hpp>
#include <cornflakes/interface/binding/external_binding.hpp>
#include <cornflakes/interface/binding/layer_program.hpp>
#include <cornflakes/interface/core/arena.hpp>
#include <cornflakes/interface/diagnostics/issue.hpp>
#include <cornflakes/interface/render/render_packet.hpp>
#include <cornflakes/interface/sim/effect_runtime.hpp>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace whiteout::flakes::renderer::corn_effects {

namespace {

std::atomic<u64> g_nextEffectId{1};

::whiteout::cornflakes::EffectId NextEffectId() {
    return ::whiteout::cornflakes::EffectId{g_nextEffectId.fetch_add(1, std::memory_order_relaxed)};
}

bool CaseInsensitiveContains(const std::string& haystack, const std::string& needle) {
    if (needle.empty())
        return true;
    if (haystack.size() < needle.size())
        return false;
    for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < needle.size(); ++j) {
            const char a =
                static_cast<char>(std::tolower(static_cast<unsigned char>(haystack[i + j])));
            const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(needle[j])));
            if (a != b) {
                match = false;
                break;
            }
        }
        if (match)
            return true;
    }
    return false;
}

bool CaseInsensitiveEqual(const std::string& a, const std::string& b) {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const char x = static_cast<char>(std::tolower(static_cast<unsigned char>(a[i])));
        const char y = static_cast<char>(std::tolower(static_cast<unsigned char>(b[i])));
        if (x != y)
            return false;
    }
    return true;
}

std::string_view FindExternalBySuffix(const ::whiteout::cornflakes::LayerProgram& lp,
                                      std::string_view suffix) {
    auto scan =
        [&](std::span<const ::whiteout::cornflakes::ExternalBinding> bs) -> std::string_view {
        for (const auto& ext : bs) {
            if (ext.name.size() >= suffix.size() &&
                ext.name.substr(ext.name.size() - suffix.size()) == suffix) {
                return ext.name;
            }
        }
        return {};
    };
    if (auto n = scan(lp.initProgram.externals); !n.empty())
        return n;
    if (auto n = scan(lp.physicsProgram.externals); !n.empty())
        return n;
    if (auto n = scan(lp.timeFixedProgram.externals); !n.empty())
        return n;
    if (auto n = scan(lp.timeVaryingProgram.externals); !n.empty())
        return n;
    return {};
}

std::string_view FindExternalByPrefix(const ::whiteout::cornflakes::LayerProgram& lp,
                                      std::string_view prefix) {
    auto scan =
        [&](std::span<const ::whiteout::cornflakes::ExternalBinding> bs) -> std::string_view {
        for (const auto& ext : bs) {
            if (ext.name.size() >= prefix.size() && ext.name.substr(0, prefix.size()) == prefix) {
                return ext.name;
            }
        }
        return {};
    };
    if (auto n = scan(lp.initProgram.externals); !n.empty())
        return n;
    if (auto n = scan(lp.physicsProgram.externals); !n.empty())
        return n;
    if (auto n = scan(lp.timeFixedProgram.externals); !n.empty())
        return n;
    if (auto n = scan(lp.timeVaryingProgram.externals); !n.empty())
        return n;
    return {};
}

std::string_view FindRenderInput(const ::whiteout::cornflakes::LayerProgram& lp,
                                 std::string_view suffix, std::string_view prefix) {
    if (auto n = FindExternalBySuffix(lp, suffix); !n.empty())
        return n;
    return FindExternalByPrefix(lp, prefix);
}

::whiteout::cornflakes::LayerRenderInputMap InferLayerRenderInputMap(
    const ::whiteout::cornflakes::LayerProgram& lp) {
    using ::whiteout::cornflakes::RenderSlot;
    ::whiteout::cornflakes::LayerRenderInputMap m;
    m.names[static_cast<size_t>(RenderSlot::Position)] =
        FindRenderInput(lp, "__Position", "Position_");
    m.names[static_cast<size_t>(RenderSlot::Size)] = FindRenderInput(lp, "__Size", "Size_");
    m.names[static_cast<size_t>(RenderSlot::Enabled)] =
        FindRenderInput(lp, "__Enabled", "Enabled_");
    m.names[static_cast<size_t>(RenderSlot::Orientation)] =
        FindRenderInput(lp, "__Orientation", "Orientation_");
    m.names[static_cast<size_t>(RenderSlot::Axis0)] = FindRenderInput(lp, "__Axis", "Axis_");
    m.names[static_cast<size_t>(RenderSlot::Axis1)] =
        FindRenderInput(lp, "__NormalAxis", "NormalAxis_");
    m.names[static_cast<size_t>(RenderSlot::Rotation)] =
        FindRenderInput(lp, "__Rotation", "Rotation_");
    m.names[static_cast<size_t>(RenderSlot::Color)] = FindRenderInput(lp, "__Color", "Color_");
    m.names[static_cast<size_t>(RenderSlot::TextureID)] =
        FindRenderInput(lp, "__TextureID", "TextureID_");
    return m;
}

} // namespace

CornEffectsEmitter::CornEffectsEmitter(assets::AssetManager& assets, std::string pkbPath,
                                       std::string animVisibilityGuide, i32 replaceableId,
                                       bool cornEffectsScaling)
    : assets_(assets), pkbPath_(std::move(pkbPath)),
      animVisibilityGuide_(std::move(animVisibilityGuide)), cornEffectsScaling_(cornEffectsScaling),
      replaceableId_(replaceableId) {
    ParseAnimVisibilityGuide();
    if (!defaultAnimEnabled_) {
        simFlags_ |= SimFlag_SystemDead;
    }
    // Acquire a Particle slot for this .pkb path. The slot starts
    // empty; the host's pump fetches + parses the bytes and the
    // model pointer swaps in via CommitPrepared. TrySpawn polls
    // ParticleAssetOf each tick until it's non-null.
    if (!pkbPath_.empty())
        assetSlot_ = assets_.Acquire(assets::AssetKind::Particle, pkbPath_);
}

CornEffectsEmitter::~CornEffectsEmitter() {
    if (assetSlot_ != 0)
        assets_.Release(assetSlot_);
}

void CornEffectsEmitter::SetWeatherParams(const Vector3f& position, const Vector2f& size,
                                          f32 emissionRate) {
    weatherPosition_ = position;
    weatherSize_ = size;
    weatherEmissionRate_ = emissionRate;
}

void CornEffectsEmitter::SetOwningAgentVisibility(bool visible) {
    const bool wasVisible = (simFlags_ & SimFlag_OwningAgentIsVisible) != 0;
    if (visible && !wasVisible) {
        simFlags_ |= SimFlag_OwningAgentBecameVisible;
    }
    if (visible)
        simFlags_ |= SimFlag_OwningAgentIsVisible;
    else
        simFlags_ &= ~SimFlag_OwningAgentIsVisible;
}

void CornEffectsEmitter::ParseAnimVisibilityGuide() {
    defaultAnimEnabled_ = true;
    enabledAnimNames_.clear();
    disabledAnimNames_.clear();

    std::stringstream ss(animVisibilityGuide_);
    std::string token;
    bool sawAlways = false;
    bool sawEnable = false;
    while (std::getline(ss, token, ',')) {
        size_t lo = 0;
        while (lo < token.size() && std::isspace(static_cast<unsigned char>(token[lo])))
            ++lo;
        token.erase(0, lo);
        while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back())))
            token.pop_back();
        if (token.empty())
            continue;

        bool enabled = true;
        const auto eq = token.find('=');
        if (eq != std::string::npos) {
            std::string val = token.substr(eq + 1);
            token = token.substr(0, eq);
            while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back())))
                token.pop_back();
            size_t vlo = 0;
            while (vlo < val.size() && std::isspace(static_cast<unsigned char>(val[vlo])))
                ++vlo;
            val.erase(0, vlo);
            while (!val.empty() && std::isspace(static_cast<unsigned char>(val.back())))
                val.pop_back();
            enabled = CaseInsensitiveEqual(val, "on");
        }
        if (CaseInsensitiveEqual(token, "always")) {
            defaultAnimEnabled_ = enabled;
            sawAlways = true;
        } else if (enabled) {
            enabledAnimNames_.push_back(token);
            sawEnable = true;
        } else {
            disabledAnimNames_.push_back(token);
        }
    }

    // When the guide lists names without an explicit `always` token, the
    // engine convention is that bare/`=on` tokens enable visibility against
    // an implicit default-OFF (i.e. listed anims are the only ones visible).
    // A guide containing only `=off` tokens keeps the default ON.
    if (!sawAlways && sawEnable) {
        defaultAnimEnabled_ = false;
    }
}

void CornEffectsEmitter::SetCurrentAnimationName(const char* name) {
    const std::string newName = name ? std::string(name) : std::string();
    if (newName == currentAnimName_)
        return;
    currentAnimName_ = newName;

    bool enabled = defaultAnimEnabled_;
    const auto& searchList = enabled ? disabledAnimNames_ : enabledAnimNames_;
    for (const auto& s : searchList) {
        if (CaseInsensitiveContains(currentAnimName_, s)) {
            enabled = !enabled;
            break;
        }
    }
    if (enabled) {
        simFlags_ &= ~SimFlag_SystemDead;
    } else {
        simFlags_ =
            (simFlags_ & ~(SimFlag_SystemDead | SimFlag_KilledByEffect)) | SimFlag_SystemDead;
    }
}

::whiteout::cornflakes::Mat4x3 CornEffectsEmitter::ToCornflakesL2W(const Matrix44f& m) {
    ::whiteout::cornflakes::Mat4x3 out{};
    for (i32 i = 0; i < 3; ++i) {
        for (i32 j = 0; j < 3; ++j) {
            out.m[i][j] = m.data[j][i];
        }
        out.m[i][3] = m.data[3][i];
    }
    return out;
}

bool CornEffectsEmitter::TrySpawn() {
    // Pull the latest parsed model from the slot. Returns null until
    // the host's pump fetches + parses + commits the .pkb bytes, at
    // which point the slot's generation bumps and we see the model.
    // No retry storm — Acquire was a one-time op in the constructor;
    // here we just observe the slot.
    if (assetSlot_ != 0) {
        const u32 g = assets_.GenerationOf(assetSlot_);
        if (g != lastAssetGen_) {
            assetModel_ = assets_.ParticleAssetOf(assetSlot_);
            lastAssetGen_ = g;
        }
    }
    if (!assetModel_)
        return false;
    if (live_.runtime)
        return true;
    if (!frameArena_)
        return false;

    auto bindArena =
        std::make_unique<::whiteout::cornflakes::ExpandingArena>(std::size_t{1U} << 16);

    ::whiteout::cornflakes::IssueBag issues;
    auto rt = std::make_unique<::whiteout::cornflakes::EffectRuntime>(
        *assetModel_, NextEffectId(), *bindArena, *frameArena_, issues);
    if (!rt->isValid()) {
        std::fprintf(stderr, "[corn_fx] ERR: TrySpawn '%s' produced invalid runtime:\n",
                     pkbPath_.c_str());
        for (const auto& iss : issues.view()) {
            std::fprintf(stderr, "[corn_fx]   bind issue cat=%u code=%u sev=%u: %.*s\n",
                         (unsigned)iss.category, iss.code, (unsigned)iss.severity,
                         (int)iss.message.size(), iss.message.data());
        }
        return false;
    }

    if (const auto* plan = rt->plan()) {
        for (size_t i = 0; i < plan->layers.size(); ++i) {
            const auto& lp = plan->layers[i];
            if (lp.renderers.empty())
                continue;
            rt->setPoolSize(i, kDefaultRenderPoolSize);
            rt->setRenderInputMap(i, InferLayerRenderInputMap(lp));
        }
    }

    std::unique_ptr<CornEffectsGfxBackend> backend;
    if (backendInit_) {
        backend = std::make_unique<CornEffectsGfxBackend>(*backendInit_);
        const bool prep = rt->setBackend(backend.get(), issues);
        if (!prep) {
            std::fprintf(stderr, "[corn_fx] WARN: TrySpawn '%s' backend prepare failed:\n",
                         pkbPath_.c_str());
            for (const auto& iss : issues.view()) {
                std::fprintf(stderr, "[corn_fx]   prepare issue cat=%u code=%u sev=%u: %.*s\n",
                             (unsigned)iss.category, iss.code, (unsigned)iss.severity,
                             (int)iss.message.size(), iss.message.data());
            }
        }
    }

    live_.bindArena = std::move(bindArena);
    live_.runtime = std::move(rt);
    live_.backend = std::move(backend);
    return true;
}

void CornEffectsEmitter::PushAttributes(::whiteout::cornflakes::EffectRuntime& rt) {
    using ArrF4 = std::array<f32, 4>;

    rt.setAttribute("__a_Game.LifespanMultiplier", ArrF4{lifeSpanMultiplier_, 0, 0, 0});
    rt.setAttribute("__a_Game.EmissionRateMultiplier", ArrF4{emissionRateMultiplier_, 0, 0, 0});
    rt.setAttribute("__a_Game.SpeedMultiplier", ArrF4{speedMultiplier_, 0, 0, 0});
    rt.setAttribute("__a_Game.ColorMultiplier", ArrF4{color_.x, color_.y, color_.z, color_.w});

    rt.setAttribute("__a_Game.TeamColor", ArrF4{replaceableColor_.x, replaceableColor_.y,
                                                replaceableColor_.z, replaceableColor_.w});

    const f32 s = gameToCornEffectsScale_;
    rt.setAttribute("__a_Game.TargetPosition",
                    ArrF4{s * position_.x, s * position_.y, s * position_.z, 0});
    rt.setAttribute("__a_Game.Scale", ArrF4{GetCornFxScale(), 0, 0, 0});
    rt.setAttribute("__a_Weather.TileCenter", ArrF4{s * weatherPosition_.x, s * weatherPosition_.y,
                                                    s * weatherPosition_.z, 0});
    rt.setAttribute("__a_Weather.Size", ArrF4{s * weatherSize_.x, s * weatherSize_.y, 0, 0});
    rt.setAttribute("__a_Weather.EmissionRate", ArrF4{weatherEmissionRate_, 0, 0, 0});
}

void CornEffectsEmitter::ResetRuntime() {
    if (!live_.runtime)
        return;
    live_.runtime->reset();
    effectAge_ = 0.0f;
}

void CornEffectsEmitter::Update(f32 dt, bool paused) {
    const bool hadPrevPos = (simFlags_ & SimFlag_UpdatedByAnim) != 0;

    if (paused)
        simFlags_ |= SimFlag_Paused;
    else
        simFlags_ &= ~SimFlag_Paused;

    const Vector3f newPos = {
        modelToWorld_.data[3][0],
        modelToWorld_.data[3][1],
        modelToWorld_.data[3][2],
    };
    const Vector3f oldPos = hadPrevPos ? position_ : newPos;
    position_ = newPos;

    if ((simFlags_ & SimFlag_OwningAgentBecameVisible) != 0) {
        simFlags_ &= ~SimFlag_OwningAgentBecameVisible;
        const f32 dx = oldPos.x - newPos.x;
        const f32 dy = oldPos.y - newPos.y;
        const f32 dz = oldPos.z - newPos.z;
        const f32 dist2 = dx * dx + dy * dy + dz * dz;
        if (dist2 > 200.0f * 200.0f) {
            ResetRuntime();
        }
    }

    simFlags_ |= SimFlag_UpdatedByAnim;

    const bool agentVis = (simFlags_ & SimFlag_OwningAgentIsVisible) != 0;
    const bool notKilled = (simFlags_ & SimFlag_KilledByEffect) == 0;
    const bool active = agentVis && ShouldBeSpawning() && notKilled;

    if (wasActive_ && !active) {
        ResetRuntime();
    }
    wasActive_ = active;

    // Drop the `assetModel_ &&` gate so the web build's lazy retry
    // inside TrySpawn can run when the .pkb arrives after the emitter
    // was constructed. TrySpawn returns false cleanly when assetModel_
    // is still null, so this is safe on desktop too.
    if (!live_.runtime && active) {
        TrySpawn();
    }
    if (!live_.runtime) {
        simFlags_ &= ~SimFlag_RenderAttempted;
        return;
    }

    ::whiteout::cornflakes::EffectFrameInputs inputs;
    inputs.dt = dt;
    inputs.effectAge = effectAge_;
    inputs.emitterL2W = ToCornflakesL2W(modelToWorld_);
    inputs.emitterL2W.m[0][3] = modelToWorld_.data[3][0] * gameToCornEffectsScale_;
    inputs.emitterL2W.m[1][3] = modelToWorld_.data[3][1] * gameToCornEffectsScale_;
    inputs.emitterL2W.m[2][3] = modelToWorld_.data[3][2] * gameToCornEffectsScale_;
    inputs.baseRngSeed = static_cast<u32>(reinterpret_cast<uintptr_t>(this) >> 4) ^ 0xC0FFEE00u;
    inputs.effectIsRunning = active;
    if (active) {
        effectAge_ += dt;
    }

    if (live_.backend) {
        CornEffectsFrameInputs fi = frameInputs_;
        fi.world = modelToWorld_;
        live_.backend->SetFrameInputs(fi);
    }

    inputs.view.viewport = {frameInputs_.viewportRect.x, frameInputs_.viewportRect.y,
                            frameInputs_.viewportRect.z, frameInputs_.viewportRect.w};
    inputs.view.effectTime = frameInputs_.effectTime;

    ::whiteout::cornflakes::IssueBag issues;
    live_.runtime->setSpawnerEnabled(active);
    PushAttributes(*live_.runtime);
    live_.runtime->tick(inputs, issues);

    simFlags_ &= ~SimFlag_RenderAttempted;
}

bool CornEffectsEmitter::Alive() const {
    if (!live_.runtime)
        return false;
    for (const auto& pkt : live_.runtime->lastPackets()) {
        if (pkt.particleCount > 0)
            return true;
    }
    return false;
}

i32 CornEffectsEmitter::TotalAlive() const {
    if (!live_.runtime)
        return 0;
    i32 n = 0;
    for (const auto& pkt : live_.runtime->lastPackets()) {
        n += static_cast<i32>(pkt.particleCount);
    }
    return n;
}

} // namespace whiteout::flakes::renderer::corn_effects
