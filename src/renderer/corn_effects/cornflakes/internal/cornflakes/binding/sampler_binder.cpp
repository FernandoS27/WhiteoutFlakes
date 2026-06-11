#include "binding_internal.hpp"

#include <cornflakes/sampler/turbulence.hpp>
#include <cornflakes/interface/binding/effect_binder.hpp>
#include <cornflakes/interface/binding/sampler_resource.hpp>

#include <algorithm>
#include <cstring>

namespace whiteout::cornflakes {

namespace {

void readField3F(const AssetObject& obj, const char* name, std::array<f32, 3>& out) noexcept {
    const auto* f = findField(obj, name);
    if (f != nullptr && f->bytes.size() >= sizeof(f32) * 3U) {
        std::memcpy(out.data(), f->bytes.data(), sizeof(f32) * 3U);
    }
}

void buildCurveSampler(SamplerResource& res, const AssetObject& data) {
    res.kind = SamplerKind::Curve;
    res.curve.times = fieldFloatArray(data, "Times");
    res.curve.values = fieldFloatArray(data, "FloatValues");
    res.curve.tangents = fieldFloatArray(data, "FloatTangents");

    const u32 valueType = fieldUint(data, "ValueType").value_or(0U);
    u8 components = 0;
    if (valueType >= 1U && valueType <= 4U) {
        components = static_cast<u8>(valueType);
    } else if (!res.curve.times.empty()) {
        const std::size_t derived = res.curve.values.size() / res.curve.times.size();
        if (derived >= 1U && derived <= 4U) {
            components = static_cast<u8>(derived);
        }
    }
    res.curve.components = components > 0 ? components : 1U;
    res.curve.interpolator = fieldUint(data, "Interpolator").value_or(0U);
    res.curve.looped = fieldBool(data, "IsLoopedCurve").value_or(false);
}

void buildShapeSampler(SamplerResource& res, const AssetObject& data) {
    res.kind = SamplerKind::Shape;
    res.shape.type = static_cast<ShapeType>(fieldInt(data, "ShapeType").value_or(0));

    res.shape.radius = fieldFloat(data, "Radius").value_or(1.0F);
    res.shape.innerRadius = fieldFloat(data, "InnerRadius").value_or(0.0F);
    res.shape.height = fieldFloat(data, "Height").value_or(0.0F);
    res.shape.hemisphere = fieldBool(data, "Hemisphere").value_or(false);
    res.shape.transformTranslate = fieldBool(data, "TransformTranslate").value_or(true);
    res.shape.transformRotate = fieldBool(data, "TransformRotate").value_or(true);

    readField3F(data, "BoxDimensions", res.shape.boxDimensions);
    readField3F(data, "Position", res.shape.position);
    readField3F(data, "EulerOrientation", res.shape.eulerOrientation);
    readField3F(data, "NonUniformScale", res.shape.nonUniformScale);

    constexpr f32 kDegToRad = 0.01745329252F;
    res.shape.eulerOrientation[0] *= kDegToRad;
    res.shape.eulerOrientation[1] *= kDegToRad;
    res.shape.eulerOrientation[2] *= kDegToRad;

    const f32 posY = res.shape.position[1];
    res.shape.position[1] = -res.shape.position[2];
    res.shape.position[2] = posY;

    std::swap(res.shape.nonUniformScale[1], res.shape.nonUniformScale[2]);
}

void buildEventStreamSampler(SamplerResource& res, const AssetObject& data) {
    res.kind = SamplerKind::EventStream;
    res.eventStream.times = fieldFloatArray(data, "Times");
}

void buildTurbulenceSampler(SamplerResource& res, const AssetObject& data, IArena& arena) {
    res.kind = SamplerKind::Turbulence;
    SamplerTurbulence& tb = res.turbulence;
    tb.strength = fieldFloat(data, "Strength").value_or(1.0F);
    tb.wavelength = fieldFloat(data, "Wavelength").value_or(1.0F);
    tb.globalScale = fieldFloat(data, "GlobalScale").value_or(1.0F);
    tb.lacunarity = fieldFloat(data, "Lacunarity").value_or(2.0F);
    tb.gain = fieldFloat(data, "Gain").value_or(0.5F);
    tb.gainMultiplier = fieldFloat(data, "GainMultiplier").value_or(1.0F);
    tb.octaves = fieldUint(data, "Octaves").value_or(3U);
    tb.interpolator = static_cast<u32>(fieldInt(data, "Interpolator").value_or(1));
    tb.seed = fieldUint(data, "InitialSeed").value_or(0U);
    tb.timeScale = fieldFloat(data, "TimeScale").value_or(0.0F);
    tb.timeBase = fieldFloat(data, "TimeBase").value_or(0.0F);

    const auto grads = arenaArray<f32>(arena, kTurbulenceGradientCount);
    generateTurbulenceGradients(tb.seed, std::span<f32>{grads.data(), grads.size()});
    tb.gradients = std::span<const f32>{grads.data(), grads.size()};
}

}

void loadSamplers(const EffectAssetModel& model, const AssetObject& layerCache, LayerProgram& lp,
                  IArena& arena) {
    const auto samplerUids = fieldLinks(layerCache, "Samplers");
    if (samplerUids.empty()) {
        return;
    }
    const auto samplerArr = arenaArray<SamplerResource>(arena, samplerUids.size());
    std::size_t written = 0;

    for (const u32 sUid : samplerUids) {
        const AssetObject* sObj = findObjectByUid(model, sUid);
        if (sObj == nullptr || sObj->type != "CLayerCompileCacheSampler") {
            continue;
        }
        SamplerResource& res = samplerArr[written++];
        res.name = stableCopy(fieldString(*sObj, "SamplerName"), arena);

        const auto dataUid = fieldLink(*sObj, "Sampler");
        if (!dataUid) {
            continue;
        }
        const AssetObject* data = findObjectByUid(model, *dataUid);
        if (data == nullptr) {
            continue;
        }
        if (data->type == "CParticleNodeSamplerData_Curve" || data->type == "CSamplerCurve") {
            buildCurveSampler(res, *data);
        } else if (data->type == "CParticleNodeSamplerData_Shape") {
            buildShapeSampler(res, *data);
        } else if (data->type == "CParticleNodeSamplerData_EventStream") {
            buildEventStreamSampler(res, *data);
        } else if (data->type == "CParticleNodeSamplerData_Turbulence") {
            buildTurbulenceSampler(res, *data, arena);
        }
    }
    if (written > 0) {
        lp.samplers = std::span<const SamplerResource>{samplerArr.data(), written};
    }
}

}
