// Sampler create / destroy. Shader create + graphics / compute pipeline
// build land in Phase D — left stubbed here so the metal_device.mm
// overrides stay consistent. metal_command_list.mm's FlushBindings
// (also Phase D) will consume the SamplerEntry slot via
// [encoder setVertexSamplerState:atIndex:] / setFragmentSamplerState.

#include "metal_device.h"
#include "metal_device_state.h"
#include "metal_handles.h"
#include "metal_translate.h"

#import <Metal/Metal.h>

namespace whiteout::flakes::gfx::metal {

namespace {

MTLSamplerMinMagFilter ToMtlMinMag(Filter f) {
    return f == Filter::Point ? MTLSamplerMinMagFilterNearest
                              : MTLSamplerMinMagFilterLinear;
}

MTLSamplerMipFilter ToMtlMip(Filter f) {
    return f == Filter::Point ? MTLSamplerMipFilterNearest
                              : MTLSamplerMipFilterLinear;
}

MTLSamplerAddressMode ToMtlAddress(AddressMode a) {
    switch (a) {
    case AddressMode::Wrap:
        return MTLSamplerAddressModeRepeat;
    case AddressMode::Clamp:
        return MTLSamplerAddressModeClampToEdge;
    case AddressMode::Mirror:
        return MTLSamplerAddressModeMirrorRepeat;
    }
    return MTLSamplerAddressModeClampToEdge;
}

MTLCompareFunction ToMtlCompare(CompareOp op) {
    switch (op) {
    case CompareOp::Never:
        return MTLCompareFunctionNever;
    case CompareOp::Less:
        return MTLCompareFunctionLess;
    case CompareOp::LessEqual:
        return MTLCompareFunctionLessEqual;
    case CompareOp::Equal:
        return MTLCompareFunctionEqual;
    case CompareOp::Greater:
        return MTLCompareFunctionGreater;
    case CompareOp::GreaterEqual:
        return MTLCompareFunctionGreaterEqual;
    case CompareOp::Always:
        return MTLCompareFunctionAlways;
    }
    return MTLCompareFunctionAlways;
}

} // namespace

SamplerHandle MetalDevice::CreateSampler(const SamplerDesc& desc) {
    @autoreleasepool {
        auto& state = *state_;

        MTLSamplerDescriptor* sd = [[MTLSamplerDescriptor alloc] init];
        sd.minFilter = ToMtlMinMag(desc.minFilter);
        sd.magFilter = ToMtlMinMag(desc.magFilter);
        sd.mipFilter = ToMtlMip(desc.minFilter);
        sd.sAddressMode = ToMtlAddress(desc.addressU);
        sd.tAddressMode = ToMtlAddress(desc.addressV);
        sd.rAddressMode = ToMtlAddress(desc.addressW);
        sd.maxAnisotropy = 1;
        sd.lodMinClamp = 0.0f;
        sd.lodMaxClamp = FLT_MAX;
        // Comparison samplers (shadow maps) bake the compare function
        // into the sampler so the shader's textureSampleCompareLevel
        // path resolves correctly.
        if (desc.comparison)
            sd.compareFunction = ToMtlCompare(desc.comparisonFunc);

        id<MTLSamplerState> sampler = [state.device newSamplerStateWithDescriptor:sd];
        if (!sampler)
            return SamplerHandle::Invalid;
        if (state.validationRequested)
            sd.label = @"wf.sampler";

        SamplerEntry entry;
        entry.sampler = sampler;
        return static_cast<SamplerHandle>(state.samplers.Insert(std::move(entry)));
    }
}

void MetalDevice::Destroy(SamplerHandle h) {
    auto& state = *state_;
    auto* s = state.samplers.Get(static_cast<u64>(h));
    if (!s)
        return;
    // Samplers are device-state — Metal retains them only for the
    // lifetime of any encoder still using them. The completion handler
    // for the in-flight frame holds the encoder's retain set; we defer
    // the ARC drop to keep that invariant true.
    SamplerEntry moved = std::move(*s);
    state.samplers.Remove(static_cast<u64>(h));

    const u64 retireAfter = state.pendingEpoch + 1;
    id<MTLSamplerState> ms = moved.sampler;
    {
        std::lock_guard<std::mutex> lock(state.pendingDeletesMutex);
        state.pendingDeletes.push_back(PendingDelete{
            retireAfter,
            [ms]() { (void)ms; },
        });
    }
}

} // namespace whiteout::flakes::gfx::metal
