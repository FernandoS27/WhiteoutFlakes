#include "d3d12_device.h"
#include "d3d12_command_list.h"

#include <cstring>
#include <algorithm>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

namespace whiteout::flakes::gfx::d3d12 {

D3D12Device::D3D12Device() = default;

D3D12Device::~D3D12Device() {

    if (queue_ && fence_ && fenceEvent_) FlushGpu();

    immediateCtx_.reset();

    swapChains_.ForEach([](SwapChainEntry& e) { e.Release(); });
    swapChains_.Clear();
    samplers_.ForEach  ([](SamplerEntry& e)  { e.Release(); });
    samplers_.Clear();
    pipelines_.ForEach ([](PipelineEntry& e) { e.Release(); });
    pipelines_.Clear();
    shaders_.ForEach   ([](ShaderEntry& e)   { e.Release(); });
    shaders_.Clear();
    textures_.ForEach  ([](TextureEntry& e)  { e.Release(); });
    textures_.Clear();
    buffers_.ForEach   ([](BufferEntry& e)   { e.Release(); });
    buffers_.Clear();

    uploadRing_.Release();
    cbvSrvUavRing_.Release();
    samplerRing_.Release();
    cbvSrvUavPool_.Release();
    samplerPool_.Release();
    rtvPool_.Release();
    dsvPool_.Release();

    SafeRelease(graphicsRS_);
    SafeRelease(computeRS_);

    SafeRelease(cmdList_);
    for (auto& a : allocators_) SafeRelease(a);

    if (fenceEvent_) { CloseHandle(fenceEvent_); fenceEvent_ = nullptr; }
    SafeRelease(fence_);
    SafeRelease(queue_);
    SafeRelease(device_);
    SafeRelease(factory_);
}

bool D3D12Device::Init(bool enableValidation) {
    enableValidation_ = enableValidation;
    if (!CreateDeviceAndQueue())   return false;
    if (!CreateCommandInfra())     return false;
    if (!CreateDescriptorPools())  return false;
    if (!CreateRootSignatures())   return false;
    if (!CreateNullDescriptors())  return false;
    if (!OpenCommandList())        return false;

    immediateCtx_ = std::make_unique<D3D12CommandList>(*this);
    return true;
}

bool D3D12Device::CreateDeviceAndQueue() {
    UINT factoryFlags = 0;

    if (enableValidation_) {
        ID3D12Debug* debug = nullptr;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
            debug->Release();
            factoryFlags = DXGI_CREATE_FACTORY_DEBUG;
        }
    }

    HRESULT hr = CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&factory_));
    if (FAILED(hr)) return false;

    IDXGIAdapter1* bestAdapter = nullptr;
    SIZE_T bestVRAM = 0;
    {
        IDXGIAdapter1* adapter = nullptr;
        for (UINT i = 0; factory_->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 desc{};
            adapter->GetDesc1(&desc);
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) { adapter->Release(); continue; }

            if (SUCCEEDED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0,
                                            __uuidof(ID3D12Device), nullptr))) {
                if (desc.DedicatedVideoMemory > bestVRAM) {
                    if (bestAdapter) bestAdapter->Release();
                    bestAdapter = adapter;
                    bestVRAM    = desc.DedicatedVideoMemory;
                    continue;
                }
            }
            adapter->Release();
        }
    }

    hr = D3D12CreateDevice(bestAdapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_));
    if (FAILED(hr)) { if (bestAdapter) bestAdapter->Release(); return false; }

    if (bestAdapter) {
        DXGI_ADAPTER_DESC1 desc{};
        bestAdapter->GetDesc1(&desc);
        char name[256]{};
        usize converted = 0;
        wcstombs_s(&converted, name, sizeof(name), desc.Description, _TRUNCATE);
        deviceName_ = name;
        bestAdapter->Release();
    }

    if (enableValidation_) {
        ID3D12InfoQueue* iq = nullptr;
        if (SUCCEEDED(device_->QueryInterface(IID_PPV_ARGS(&iq)))) {
            iq->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
            iq->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR,      TRUE);
            iq->Release();
        }
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type     = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    qd.Flags    = D3D12_COMMAND_QUEUE_FLAG_NONE;
    hr = device_->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_));
    return SUCCEEDED(hr);
}

bool D3D12Device::CreateCommandInfra() {
    for (u32 i = 0; i < kFramesInFlight; ++i) {
        HRESULT hr = device_->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators_[i]));
        if (FAILED(hr)) return false;
    }

    HRESULT hr = device_->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators_[0], nullptr,
        IID_PPV_ARGS(&cmdList_));
    if (FAILED(hr)) return false;
    cmdListOpen_ = true;

    hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
    if (FAILED(hr)) return false;

    fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return fenceEvent_ != nullptr;
}

bool D3D12Device::CreateDescriptorPools() {
    if (!rtvPool_.Init      (device_, D3D12_DESCRIPTOR_HEAP_TYPE_RTV,          1024)) return false;
    if (!dsvPool_.Init      (device_, D3D12_DESCRIPTOR_HEAP_TYPE_DSV,          256))  return false;
    if (!cbvSrvUavPool_.Init(device_, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,  16384)) return false;
    if (!samplerPool_.Init  (device_, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,      2048)) return false;

    if (!cbvSrvUavRing_.Init(device_, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 65536)) return false;
    if (!samplerRing_.Init  (device_, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,     2048))  return false;

    if (!uploadRing_.Init(device_, 256 * 1024 * 1024)) return false;
    return true;
}

bool D3D12Device::CreateRootSignatures() {

    {
        D3D12_DESCRIPTOR_RANGE srvRangeVs{};
        srvRangeVs.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRangeVs.NumDescriptors     = kSrvsPerStage;
        srvRangeVs.BaseShaderRegister = 0;
        srvRangeVs.RegisterSpace      = 0;
        srvRangeVs.OffsetInDescriptorsFromTableStart = 0;

        D3D12_DESCRIPTOR_RANGE srvRangePs = srvRangeVs;
        D3D12_DESCRIPTOR_RANGE samplerRangePs{};
        samplerRangePs.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        samplerRangePs.NumDescriptors     = kSamplersPerStage;
        samplerRangePs.BaseShaderRegister = 0;
        samplerRangePs.RegisterSpace      = 0;
        samplerRangePs.OffsetInDescriptorsFromTableStart = 0;

        D3D12_ROOT_PARAMETER params[static_cast<u32>(GraphicsRP::Count)] = {};

        for (u32 i = 0; i < kRootCbvsPerStage; ++i) {
            auto& p = params[static_cast<u32>(GraphicsRP::CBV_VS_0) + i];
            p.ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
            p.Descriptor.ShaderRegister = i;
            p.Descriptor.RegisterSpace  = 0;
            p.ShaderVisibility          = D3D12_SHADER_VISIBILITY_VERTEX;
        }

        for (u32 i = 0; i < kRootCbvsPerStage; ++i) {
            auto& p = params[static_cast<u32>(GraphicsRP::CBV_PS_0) + i];
            p.ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
            p.Descriptor.ShaderRegister = i;
            p.Descriptor.RegisterSpace  = 0;
            p.ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;
        }

        {
            auto& p = params[static_cast<u32>(GraphicsRP::SRV_TABLE_VS)];
            p.ParameterType                      = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            p.DescriptorTable.NumDescriptorRanges = 1;
            p.DescriptorTable.pDescriptorRanges   = &srvRangeVs;
            p.ShaderVisibility                    = D3D12_SHADER_VISIBILITY_VERTEX;
        }

        {
            auto& p = params[static_cast<u32>(GraphicsRP::SRV_TABLE_PS)];
            p.ParameterType                      = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            p.DescriptorTable.NumDescriptorRanges = 1;
            p.DescriptorTable.pDescriptorRanges   = &srvRangePs;
            p.ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
        }

        {
            auto& p = params[static_cast<u32>(GraphicsRP::SAMPLER_TABLE_PS)];
            p.ParameterType                      = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            p.DescriptorTable.NumDescriptorRanges = 1;
            p.DescriptorTable.pDescriptorRanges   = &samplerRangePs;
            p.ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
        }

        D3D12_STATIC_SAMPLER_DESC staticSamplers[7] = {};
        auto MakeSampler = [](UINT shaderRegister, D3D12_TEXTURE_ADDRESS_MODE addressMode) {
            D3D12_STATIC_SAMPLER_DESC s{};
            s.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            s.AddressU         = addressMode;
            s.AddressV         = addressMode;
            s.AddressW         = addressMode;
            s.MipLODBias       = 0.0f;
            s.MaxAnisotropy    = 0;
            s.ComparisonFunc   = D3D12_COMPARISON_FUNC_NEVER;
            s.BorderColor      = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
            s.MinLOD           = 0.0f;
            s.MaxLOD           = D3D12_FLOAT32_MAX;
            s.ShaderRegister   = shaderRegister;
            s.RegisterSpace    = 0;
            s.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            return s;
        };
        auto MakeShadowSampler = [](UINT shaderRegister) {
            D3D12_STATIC_SAMPLER_DESC s{};
            s.Filter           = D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
            s.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
            s.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
            s.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
            s.MipLODBias       = 0.0f;
            s.MaxAnisotropy    = 0;
            s.ComparisonFunc   = D3D12_COMPARISON_FUNC_LESS_EQUAL;
            s.BorderColor      = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
            s.MinLOD           = 0.0f;
            s.MaxLOD           = D3D12_FLOAT32_MAX;
            s.ShaderRegister   = shaderRegister;
            s.RegisterSpace    = 0;
            s.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            return s;
        };
        staticSamplers[0] = MakeSampler(4,  D3D12_TEXTURE_ADDRESS_MODE_WRAP);
        staticSamplers[1] = MakeShadowSampler(10);
        staticSamplers[2] = MakeShadowSampler(11);
        staticSamplers[3] = MakeShadowSampler(12);
        staticSamplers[4] = MakeSampler(13, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
        staticSamplers[5] = MakeSampler(14, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
        staticSamplers[6] = MakeSampler(15, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters     = static_cast<UINT>(GraphicsRP::Count);
        rsd.pParameters       = params;
        rsd.NumStaticSamplers = 7;
        rsd.pStaticSamplers   = staticSamplers;
        rsd.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ID3DBlob* blob = nullptr;
        ID3DBlob* err  = nullptr;
        HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
        if (FAILED(hr)) { SafeRelease(err); return false; }
        hr = device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                          IID_PPV_ARGS(&graphicsRS_));
        SafeRelease(blob);
        SafeRelease(err);
        if (FAILED(hr)) return false;
    }

    {
        D3D12_DESCRIPTOR_RANGE srvRange{};
        srvRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors     = kSrvsPerStage;
        srvRange.BaseShaderRegister = 0;
        srvRange.RegisterSpace      = 0;
        srvRange.OffsetInDescriptorsFromTableStart = 0;

        D3D12_DESCRIPTOR_RANGE uavRange{};
        uavRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavRange.NumDescriptors     = kUavsForCompute;
        uavRange.BaseShaderRegister = 0;
        uavRange.RegisterSpace      = 0;
        uavRange.OffsetInDescriptorsFromTableStart = 0;

        D3D12_DESCRIPTOR_RANGE samplerRange{};
        samplerRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        samplerRange.NumDescriptors     = kSamplersPerStage;
        samplerRange.BaseShaderRegister = 0;
        samplerRange.RegisterSpace      = 0;
        samplerRange.OffsetInDescriptorsFromTableStart = 0;

        D3D12_ROOT_PARAMETER params[static_cast<u32>(ComputeRP::Count)] = {};

        for (u32 i = 0; i < kRootCbvsPerStage; ++i) {
            auto& p = params[static_cast<u32>(ComputeRP::CBV_0) + i];
            p.ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
            p.Descriptor.ShaderRegister = i;
            p.Descriptor.RegisterSpace  = 0;
            p.ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
        }
        {
            auto& p = params[static_cast<u32>(ComputeRP::SRV_TABLE)];
            p.ParameterType                      = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            p.DescriptorTable.NumDescriptorRanges = 1;
            p.DescriptorTable.pDescriptorRanges   = &srvRange;
            p.ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;
        }
        {
            auto& p = params[static_cast<u32>(ComputeRP::UAV_TABLE)];
            p.ParameterType                      = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            p.DescriptorTable.NumDescriptorRanges = 1;
            p.DescriptorTable.pDescriptorRanges   = &uavRange;
            p.ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;
        }
        {
            auto& p = params[static_cast<u32>(ComputeRP::SAMPLER_TABLE)];
            p.ParameterType                      = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            p.DescriptorTable.NumDescriptorRanges = 1;
            p.DescriptorTable.pDescriptorRanges   = &samplerRange;
            p.ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;
        }

        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = static_cast<UINT>(ComputeRP::Count);
        rsd.pParameters   = params;
        rsd.Flags         = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        ID3DBlob* blob = nullptr;
        ID3DBlob* err  = nullptr;
        HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
        if (FAILED(hr)) { SafeRelease(err); return false; }
        hr = device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                          IID_PPV_ARGS(&computeRS_));
        SafeRelease(blob);
        SafeRelease(err);
        if (FAILED(hr)) return false;
    }

    return true;
}

bool D3D12Device::CreateNullDescriptors() {
    nullSrv_ = cbvSrvUavPool_.Allocate();
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels     = 1;
    device_->CreateShaderResourceView(nullptr, &sd, nullSrv_);

    nullUav_ = cbvSrvUavPool_.Allocate();
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format                  = DXGI_FORMAT_R32_UINT;
    ud.ViewDimension           = D3D12_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements      = 1;
    device_->CreateUnorderedAccessView(nullptr, nullptr, &ud, nullUav_);

    nullSampler_ = samplerPool_.Allocate();
    D3D12_SAMPLER_DESC sampd{};
    sampd.Filter         = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampd.AddressU       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampd.AddressV       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampd.AddressW       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampd.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampd.MaxLOD         = D3D12_FLOAT32_MAX;
    device_->CreateSampler(&sampd, nullSampler_);
    return true;
}

bool D3D12Device::OpenCommandList() {

    frameFenceValues_[0] = 0;
    frameFenceValues_[1] = 0;
    frameIndex_          = 0;
    fenceValue_          = 0;
    return true;
}

void D3D12Device::WaitForFence(u64 value) {
    if (fence_->GetCompletedValue() < value) {
        fence_->SetEventOnCompletion(value, fenceEvent_);
        DWORD result = WaitForSingleObject(fenceEvent_, 5000);
        if (result == WAIT_TIMEOUT) {
            OutputDebugStringA("[D3D12] WaitForFence timeout — possible GPU hang\n");
        }
    }
}

void D3D12Device::DeferredRelease(IUnknown* obj) {
    if (!obj) return;

    pendingDeletes_.push_back({obj, fenceValue_ + 1});
}

void D3D12Device::FlushPendingDeletes(u64 completedFenceValue) {
    auto it = pendingDeletes_.begin();
    while (it != pendingDeletes_.end()) {
        if (it->fenceValue <= completedFenceValue) {
            if (it->obj) it->obj->Release();
            it = pendingDeletes_.erase(it);
        } else {
            ++it;
        }
    }
}

void D3D12Device::FlushGpu() {

    if (cmdListOpen_) {
        cmdList_->Close();
        ID3D12CommandList* lists[] = { cmdList_ };
        queue_->ExecuteCommandLists(1, lists);
        cmdListOpen_ = false;
    }
    ++fenceValue_;
    queue_->Signal(fence_, fenceValue_);
    WaitForFence(fenceValue_);

    u64 completed = fence_->GetCompletedValue();
    uploadRing_.Retire(completed);
    cbvSrvUavRing_.Retire(completed);
    samplerRing_.Retire(completed);
    FlushPendingDeletes(completed);
}

BufferHandle D3D12Device::CreateBuffer(const BufferDesc& desc, const void* initial) {
    BufferEntry entry{};
    entry.desc = desc;

    const bool cpuWritable = hasFlag(desc.usage, BufferUsage::CpuWritable);

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = cpuWritable ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width            = std::max<UINT64>(desc.size, 256);
    rd.Height           = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.Format           = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags            = D3D12_RESOURCE_FLAG_NONE;

    if (hasFlag(desc.usage, BufferUsage::UnorderedAccess))
        rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_RESOURCE_STATES initialState =
        cpuWritable ? D3D12_RESOURCE_STATE_GENERIC_READ
                    : D3D12_RESOURCE_STATE_COMMON;

    HRESULT hr = device_->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &rd, initialState, nullptr,
        IID_PPV_ARGS(&entry.resource));
    if (FAILED(hr)) return BufferHandle::Invalid;
    entry.currentState = initialState;

    if (initial && !cpuWritable && desc.size > 0) {

        auto alloc = uploadRing_.Allocate(desc.size);
        std::memcpy(alloc.cpu, initial, desc.size);

        D3D12_RESOURCE_BARRIER b{};
        b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = entry.resource;
        b.Transition.StateBefore = entry.currentState;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList_->ResourceBarrier(1, &b);

        cmdList_->CopyBufferRegion(entry.resource, 0,
                                   alloc.resource, alloc.offset, desc.size);

        D3D12_RESOURCE_STATES postState = D3D12_RESOURCE_STATE_COMMON;
        if (hasFlag(desc.usage, BufferUsage::Vertex) ||
            hasFlag(desc.usage, BufferUsage::Index) ||
            hasFlag(desc.usage, BufferUsage::Constant))
            postState = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
        if (hasFlag(desc.usage, BufferUsage::ShaderResource))
            postState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter  = postState;
        cmdList_->ResourceBarrier(1, &b);
        entry.currentState = postState;
    } else if (initial && cpuWritable && desc.size > 0) {

        D3D12_RANGE nr{0, 0};
        void* p = nullptr;
        if (SUCCEEDED(entry.resource->Map(0, &nr, &p))) {
            std::memcpy(p, initial, desc.size);
            entry.resource->Unmap(0, nullptr);
            entry.cpuWritableVA = entry.resource->GetGPUVirtualAddress();
        }
    }

    if (hasFlag(desc.usage, BufferUsage::ShaderResource) && desc.elementStride > 0) {
        entry.srvCpu = cbvSrvUavPool_.Allocate();
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format                     = DXGI_FORMAT_UNKNOWN;
        sd.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
        sd.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Buffer.FirstElement        = 0;
        sd.Buffer.NumElements         = static_cast<UINT>(desc.size / desc.elementStride);
        sd.Buffer.StructureByteStride = desc.elementStride;
        device_->CreateShaderResourceView(entry.resource, &sd, entry.srvCpu);
        entry.hasSrv = true;
    }

    if (hasFlag(desc.usage, BufferUsage::UnorderedAccess) && desc.elementStride > 0) {
        entry.uavCpu = cbvSrvUavPool_.Allocate();
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format                     = DXGI_FORMAT_UNKNOWN;
        ud.ViewDimension              = D3D12_UAV_DIMENSION_BUFFER;
        ud.Buffer.FirstElement        = 0;
        ud.Buffer.NumElements         = static_cast<UINT>(desc.size / desc.elementStride);
        ud.Buffer.StructureByteStride = desc.elementStride;
        device_->CreateUnorderedAccessView(entry.resource, nullptr, &ud, entry.uavCpu);
        entry.hasUav = true;
    }

    return static_cast<BufferHandle>(buffers_.Insert(std::move(entry)));
}

void D3D12Device::Destroy(BufferHandle h) {
    if (h == BufferHandle::Invalid) return;
    auto* e = buffers_.Get(static_cast<u64>(h));
    if (e) {
        if (e->hasSrv) cbvSrvUavPool_.Free(e->srvCpu);
        if (e->hasUav) cbvSrvUavPool_.Free(e->uavCpu);

        DeferredRelease(e->resource);
        e->resource       = nullptr;
        e->cpuWritableVA  = 0;
        e->cpuWritablePtr = nullptr;
    }
    buffers_.Remove(static_cast<u64>(h));
}

void D3D12Device::UpdateBuffer(BufferHandle h, const void* data, usize size) {
    auto* e = buffers_.Get(static_cast<u64>(h));
    if (!e || size == 0) return;

    if (hasFlag(e->desc.usage, BufferUsage::CpuWritable)) {

        u64 align = hasFlag(e->desc.usage, BufferUsage::Constant) ? 256 : 16;
        if (e->desc.elementStride > 0)
            align = std::max<u64>(align, e->desc.elementStride);
        auto alloc = uploadRing_.Allocate(size, align);
        std::memcpy(alloc.cpu, data, size);
        e->cpuWritableVA  = alloc.gpu;
        e->cpuWritablePtr = alloc.cpu;

        if (e->hasSrv && e->desc.elementStride > 0) {
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format                     = DXGI_FORMAT_UNKNOWN;
            sd.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
            sd.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Buffer.FirstElement        = alloc.offset / e->desc.elementStride;
            sd.Buffer.NumElements         = static_cast<UINT>(size / e->desc.elementStride);
            sd.Buffer.StructureByteStride = e->desc.elementStride;
            device_->CreateShaderResourceView(alloc.resource, &sd, e->srvCpu);
        }
    } else {

        auto alloc = uploadRing_.Allocate(size);
        std::memcpy(alloc.cpu, data, size);

        D3D12_RESOURCE_BARRIER b{};
        b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = e->resource;
        b.Transition.StateBefore = e->currentState;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        if (e->currentState != D3D12_RESOURCE_STATE_COPY_DEST)
            cmdList_->ResourceBarrier(1, &b);
        cmdList_->CopyBufferRegion(e->resource, 0, alloc.resource, alloc.offset, size);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter  = e->currentState;
        if (e->currentState != D3D12_RESOURCE_STATE_COPY_DEST)
            cmdList_->ResourceBarrier(1, &b);
    }
}

void* D3D12Device::MapBuffer(BufferHandle h) {
    auto* e = buffers_.Get(static_cast<u64>(h));
    if (!e) return nullptr;
    if (!hasFlag(e->desc.usage, BufferUsage::CpuWritable)) return nullptr;

    u64 align = hasFlag(e->desc.usage, BufferUsage::Constant) ? 256 : 16;
    if (e->desc.elementStride > 0)
        align = std::max<u64>(align, e->desc.elementStride);
    auto alloc = uploadRing_.Allocate(e->desc.size, align);
    e->cpuWritableVA  = alloc.gpu;
    e->cpuWritablePtr = alloc.cpu;

    if (e->hasSrv && e->desc.elementStride > 0) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format                     = DXGI_FORMAT_UNKNOWN;
        sd.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
        sd.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Buffer.FirstElement        = alloc.offset / e->desc.elementStride;
        sd.Buffer.NumElements         = static_cast<UINT>(e->desc.size / e->desc.elementStride);
        sd.Buffer.StructureByteStride = e->desc.elementStride;
        device_->CreateShaderResourceView(alloc.resource, &sd, e->srvCpu);
    }
    return alloc.cpu;
}

void D3D12Device::UnmapBuffer(BufferHandle) {

}

TextureHandle D3D12Device::CreateTexture(const TextureDesc& desc, const void* initialPixels) {
    TextureEntry entry{};
    entry.desc = desc;

    const bool isDepth = hasFlag(desc.usage, TextureUsage::DepthStencil);
    const bool isRt    = hasFlag(desc.usage, TextureUsage::RenderTarget);
    const bool isSrv   = hasFlag(desc.usage, TextureUsage::ShaderResource);

    const UINT16 arraySlices = desc.isCube
        ? static_cast<UINT16>(desc.arraySize > 0 ? desc.arraySize : 6)
        : static_cast<UINT16>(desc.arraySize > 0 ? desc.arraySize : 1);
    const UINT16 mipCount = static_cast<UINT16>(desc.mipLevels == 0 ? 1 : desc.mipLevels);

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width              = static_cast<UINT64>(desc.width);
    rd.Height             = static_cast<UINT>(desc.height);
    rd.DepthOrArraySize   = arraySlices;
    rd.MipLevels          = mipCount;
    rd.Format             = isDepth ? DepthFormatToTypeless(desc.format) : ToDXGI(desc.format);
    rd.SampleDesc.Count   = 1;
    rd.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags              = D3D12_RESOURCE_FLAG_NONE;

    if (isRt)    rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (isDepth) rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    if (isDepth && !isSrv)
        rd.Flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

    D3D12_CLEAR_VALUE clear{};
    clear.Format = ToDXGI(desc.format);
    D3D12_CLEAR_VALUE* pClear = nullptr;
    if (isRt) {
        clear.Color[0] = clear.Color[1] = clear.Color[2] = clear.Color[3] = 0.0f;
        pClear = &clear;
    } else if (isDepth) {
        clear.DepthStencil.Depth   = 1.0f;
        clear.DepthStencil.Stencil = 0;
        pClear = &clear;
    }

    D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON;
    if (initialPixels) initialState = D3D12_RESOURCE_STATE_COPY_DEST;
    else if (isRt)     initialState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    else if (isDepth)  initialState = D3D12_RESOURCE_STATE_DEPTH_WRITE;

    HRESULT hr = device_->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &rd, initialState, pClear,
        IID_PPV_ARGS(&entry.resource));
    if (FAILED(hr)) return TextureHandle::Invalid;
    entry.currentState = initialState;

    if (initialPixels) {
        const UINT subresCount = static_cast<UINT>(arraySlices) * mipCount;
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(subresCount);
        std::vector<UINT>   numRows(subresCount);
        std::vector<UINT64> rowSizeBytes(subresCount);
        UINT64 totalBytes = 0;
        device_->GetCopyableFootprints(
            &rd, 0, subresCount, 0,
            layouts.data(), numRows.data(), rowSizeBytes.data(), &totalBytes);

        auto alloc = uploadRing_.Allocate(totalBytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);

        const u8* src = static_cast<const u8*>(initialPixels);
        u8*       base = static_cast<u8*>(alloc.cpu);
        for (UINT s = 0; s < subresCount; ++s) {
            const auto& fp       = layouts[s];
            const UINT  rows     = numRows[s];
            const UINT64 rowSize = rowSizeBytes[s];
            u8* dst = base + fp.Offset;
            for (UINT y = 0; y < rows; ++y) {
                std::memcpy(dst + static_cast<usize>(y) * fp.Footprint.RowPitch,
                            src + static_cast<usize>(y) * rowSize,
                            static_cast<usize>(rowSize));
            }
            src += static_cast<usize>(rowSize) * rows;
        }

        for (UINT s = 0; s < subresCount; ++s) {
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout = layouts[s];
            layout.Offset += alloc.offset;

            D3D12_TEXTURE_COPY_LOCATION dstLoc{};
            dstLoc.pResource        = entry.resource;
            dstLoc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dstLoc.SubresourceIndex = s;

            D3D12_TEXTURE_COPY_LOCATION srcLoc{};
            srcLoc.pResource       = alloc.resource;
            srcLoc.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            srcLoc.PlacedFootprint = layout;

            cmdList_->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
        }

        D3D12_RESOURCE_BARRIER b{};
        b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = entry.resource;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList_->ResourceBarrier(1, &b);
        entry.currentState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }

    if (isSrv) {
        entry.srvCpu = cbvSrvUavPool_.Allocate();
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format                    = isDepth ? DepthFormatToSrvFormat(desc.format)
                                               : ToDXGI(desc.format);
        sd.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        const UINT srvMips = desc.mipLevels == 0 ? static_cast<UINT>(-1)
                                                 : static_cast<UINT>(desc.mipLevels);
        if (desc.isCube) {

            sd.ViewDimension                    = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
            sd.TextureCubeArray.MipLevels       = srvMips;
            sd.TextureCubeArray.NumCubes        = std::max<UINT>(1, arraySlices / 6);
        } else if (arraySlices > 1) {
            sd.ViewDimension                 = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            sd.Texture2DArray.MipLevels      = srvMips;
            sd.Texture2DArray.ArraySize      = arraySlices;
        } else {
            sd.ViewDimension         = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MipLevels   = srvMips;
        }
        device_->CreateShaderResourceView(entry.resource, &sd, entry.srvCpu);
        entry.hasSrv = true;
    }

    if (isRt) {
        entry.rtvCpu = rtvPool_.Allocate();
        device_->CreateRenderTargetView(entry.resource, nullptr, entry.rtvCpu);
        entry.hasRtv = true;
    }

    if (isDepth) {
        entry.dsvCpu = dsvPool_.Allocate();
        D3D12_DEPTH_STENCIL_VIEW_DESC dd{};
        dd.Format        = ToDXGI(desc.format);
        dd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        device_->CreateDepthStencilView(entry.resource, &dd, entry.dsvCpu);
        entry.hasDsv = true;
    }

    return static_cast<TextureHandle>(textures_.Insert(std::move(entry)));
}

void D3D12Device::Destroy(TextureHandle h) {
    if (h == TextureHandle::Invalid) return;
    auto* e = textures_.Get(static_cast<u64>(h));
    if (e) {
        if (e->hasSrv) cbvSrvUavPool_.Free(e->srvCpu);
        if (e->hasRtv) rtvPool_.Free(e->rtvCpu);
        if (e->hasDsv) dsvPool_.Free(e->dsvCpu);

        if (e->ownsResource) {
            DeferredRelease(e->resource);
            e->resource = nullptr;
        } else {
            e->resource = nullptr;
        }
    }
    textures_.Remove(static_cast<u64>(h));
}

TextureHandle D3D12Device::CreateColorTarget(i32 w, i32 h, Format f) {
    TextureDesc desc{};
    desc.width     = w;
    desc.height    = h;
    desc.mipLevels = 1;
    desc.format    = f;
    desc.usage     = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
    return CreateTexture(desc, nullptr);
}

TextureHandle D3D12Device::CreateDepthTarget(i32 w, i32 h, Format f) {
    TextureDesc desc{};
    desc.width     = w;
    desc.height    = h;
    desc.mipLevels = 1;
    desc.format    = f;
    desc.usage     = TextureUsage::DepthStencil;
    return CreateTexture(desc, nullptr);
}

ShaderHandle D3D12Device::CreateShader(ShaderStage stage, const void* bytecode, usize size) {
    ShaderEntry entry{};
    entry.stage = stage;
    entry.bytecode.assign(static_cast<const u8*>(bytecode),
                          static_cast<const u8*>(bytecode) + size);
    return static_cast<ShaderHandle>(shaders_.Insert(std::move(entry)));
}

void D3D12Device::Destroy(ShaderHandle h) {
    if (h == ShaderHandle::Invalid) return;
    auto* e = shaders_.Get(static_cast<u64>(h));
    if (e) e->Release();
    shaders_.Remove(static_cast<u64>(h));
}

PipelineHandle D3D12Device::CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) {
    auto* vs = shaders_.Get(static_cast<u64>(desc.vs));
    auto* ps = shaders_.Get(static_cast<u64>(desc.ps));
    if (!vs) return PipelineHandle::Invalid;

    std::vector<D3D12_INPUT_ELEMENT_DESC> elems;
    elems.reserve(desc.inputLayout.size());
    for (const auto& ie : desc.inputLayout) {
        D3D12_INPUT_ELEMENT_DESC d{};
        d.SemanticName         = ie.semantic;
        d.SemanticIndex        = ie.semanticIndex;
        d.Format               = ToDXGI(ie.format);
        d.InputSlot            = ie.inputSlot;
        d.AlignedByteOffset    = ie.offset;
        d.InputSlotClass       = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        d.InstanceDataStepRate = 0;
        elems.push_back(d);
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = graphicsRS_;

    pd.VS.pShaderBytecode = vs->bytecode.data();
    pd.VS.BytecodeLength  = vs->bytecode.size();
    if (ps) {
        pd.PS.pShaderBytecode = ps->bytecode.data();
        pd.PS.BytecodeLength  = ps->bytecode.size();
    }

    D3D12_BLEND_DESC bd{};
    bd.AlphaToCoverageEnable  = desc.blend.alphaToCoverage ? TRUE : FALSE;
    bd.IndependentBlendEnable = FALSE;
    auto& rt0 = bd.RenderTarget[0];
    rt0.BlendEnable           = desc.blend.enable ? TRUE : FALSE;
    rt0.LogicOpEnable         = FALSE;
    rt0.SrcBlend              = ToD3D12(desc.blend.srcColor);
    rt0.DestBlend             = ToD3D12(desc.blend.dstColor);
    rt0.BlendOp               = ToD3D12(desc.blend.opColor);
    rt0.SrcBlendAlpha         = ToD3D12(desc.blend.srcAlpha);
    rt0.DestBlendAlpha        = ToD3D12(desc.blend.dstAlpha);
    rt0.BlendOpAlpha          = ToD3D12(desc.blend.opAlpha);
    rt0.LogicOp               = D3D12_LOGIC_OP_NOOP;
    rt0.RenderTargetWriteMask = desc.blend.colorWrite ? D3D12_COLOR_WRITE_ENABLE_ALL : 0;
    pd.BlendState = bd;
    pd.SampleMask = UINT_MAX;

    D3D12_RASTERIZER_DESC rs{};
    rs.FillMode              = ToD3D12(desc.rasterizer.fill);
    rs.CullMode              = ToD3D12(desc.rasterizer.cull);
    rs.FrontCounterClockwise = desc.rasterizer.frontCCW ? TRUE : FALSE;
    rs.DepthClipEnable       = TRUE;
    rs.AntialiasedLineEnable = TRUE;
    rs.MultisampleEnable     = FALSE;
    rs.ConservativeRaster    = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    rs.DepthBias             = desc.rasterizer.depthBias;
    rs.SlopeScaledDepthBias  = desc.rasterizer.slopeScaledDepthBias;
    rs.DepthBiasClamp        = desc.rasterizer.depthBiasClamp;
    pd.RasterizerState = rs;

    D3D12_DEPTH_STENCIL_DESC dsd{};
    dsd.DepthEnable      = desc.depthStencil.depthTest ? TRUE : FALSE;
    dsd.DepthWriteMask   = desc.depthStencil.depthWrite ? D3D12_DEPTH_WRITE_MASK_ALL
                                                        : D3D12_DEPTH_WRITE_MASK_ZERO;
    dsd.DepthFunc        = ToD3D12(desc.depthStencil.depthCompare);
    dsd.StencilEnable    = FALSE;
    pd.DepthStencilState = dsd;

    pd.InputLayout.pInputElementDescs = elems.data();
    pd.InputLayout.NumElements        = static_cast<UINT>(elems.size());
    pd.PrimitiveTopologyType          = ToD3D12TopologyType(desc.topology);

    if (desc.rtvFormat == Format::Unknown) {
        pd.NumRenderTargets = 0;
    } else {
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0]    = ToDXGI(desc.rtvFormat);
    }
    pd.DSVFormat                      = ToDXGI(desc.dsvFormat);
    pd.SampleDesc.Count               = 1;

    PipelineEntry entry{};
    entry.isCompute = false;
    entry.topology  = ToD3D12Topology(desc.topology);
    HRESULT hr = device_->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&entry.pso));
    if (FAILED(hr)) {
        std::fprintf(stderr,
                     "[d3d12] CreateGraphicsPipelineState FAILED hr=0x%08x "
                     "vs_size=%zu ps_size=%zu\n",
                     (unsigned)hr, vs->bytecode.size(),
                     ps ? ps->bytecode.size() : 0u);
        ID3D12InfoQueue* iq = nullptr;
        if (SUCCEEDED(device_->QueryInterface(IID_PPV_ARGS(&iq))) && iq) {
            UINT64 n = iq->GetNumStoredMessages();
            for (UINT64 i = 0; i < n; ++i) {
                SIZE_T sz = 0;
                iq->GetMessage(i, nullptr, &sz);
                std::vector<u8> buf(sz);
                auto* msg = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
                if (SUCCEEDED(iq->GetMessage(i, msg, &sz))) {
                    std::fprintf(stderr, "[d3d12]   %.*s\n",
                                 (int)msg->DescriptionByteLength,
                                 msg->pDescription);
                }
            }
            iq->ClearStoredMessages();
            iq->Release();
        }
        return PipelineHandle::Invalid;
    }

    return static_cast<PipelineHandle>(pipelines_.Insert(std::move(entry)));
}

PipelineHandle D3D12Device::CreateComputePipeline(const ComputePipelineDesc& desc) {
    auto* cs = shaders_.Get(static_cast<u64>(desc.cs));
    if (!cs) return PipelineHandle::Invalid;

    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature     = computeRS_;
    pd.CS.pShaderBytecode = cs->bytecode.data();
    pd.CS.BytecodeLength  = cs->bytecode.size();

    PipelineEntry entry{};
    entry.isCompute = true;
    HRESULT hr = device_->CreateComputePipelineState(&pd, IID_PPV_ARGS(&entry.pso));
    if (FAILED(hr)) return PipelineHandle::Invalid;

    return static_cast<PipelineHandle>(pipelines_.Insert(std::move(entry)));
}

void D3D12Device::Destroy(PipelineHandle h) {
    if (h == PipelineHandle::Invalid) return;
    auto* e = pipelines_.Get(static_cast<u64>(h));
    if (e) e->Release();
    pipelines_.Remove(static_cast<u64>(h));
}

SamplerHandle D3D12Device::CreateSampler(const SamplerDesc& desc) {
    D3D12_SAMPLER_DESC sd{};
    sd.Filter         = desc.comparison
                            ? ToD3D12FilterComparison(desc.minFilter, desc.magFilter)
                            : ToD3D12Filter(desc.minFilter, desc.magFilter);
    sd.AddressU       = ToD3D12(desc.addressU);
    sd.AddressV       = ToD3D12(desc.addressV);
    sd.AddressW       = ToD3D12(desc.addressW);
    sd.MaxAnisotropy  = 1;
    sd.ComparisonFunc = desc.comparison
                            ? ToD3D12(desc.comparisonFunc)
                            : D3D12_COMPARISON_FUNC_NEVER;
    sd.MaxLOD         = D3D12_FLOAT32_MAX;

    if (desc.comparison) {
        sd.BorderColor[0] = 1.0f;
        sd.BorderColor[1] = 1.0f;
        sd.BorderColor[2] = 1.0f;
        sd.BorderColor[3] = 1.0f;
    }

    SamplerEntry entry{};
    entry.samplerCpu = samplerPool_.Allocate();
    entry.valid      = true;
    device_->CreateSampler(&sd, entry.samplerCpu);

    return static_cast<SamplerHandle>(samplers_.Insert(std::move(entry)));
}

void D3D12Device::Destroy(SamplerHandle h) {
    if (h == SamplerHandle::Invalid) return;
    auto* e = samplers_.Get(static_cast<u64>(h));
    if (e) {
        if (e->valid) samplerPool_.Free(e->samplerCpu);
        e->Release();
    }
    samplers_.Remove(static_cast<u64>(h));
}

SwapChainHandle D3D12Device::CreateSwapChain(void* nativeWindowHandle,
                                              i32 width, i32 height,
                                              Format colorFormat) {
    SwapChainEntry entry{};
    entry.hwnd        = static_cast<HWND>(nativeWindowHandle);
    entry.colorFormat = colorFormat;

    DXGI_FORMAT rtvDxgi      = ToDXGI(colorFormat);
    DXGI_FORMAT resourceDxgi = rtvDxgi;
    switch (rtvDxgi) {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: resourceDxgi = DXGI_FORMAT_R8G8B8A8_UNORM; break;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: resourceDxgi = DXGI_FORMAT_B8G8R8A8_UNORM; break;
        default: break;
    }

    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width       = static_cast<UINT>(width);
    scd.Height      = static_cast<UINT>(height);
    scd.Format      = resourceDxgi;
    scd.Stereo      = FALSE;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = kFramesInFlight;
    scd.Scaling     = DXGI_SCALING_STRETCH;
    scd.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.AlphaMode   = DXGI_ALPHA_MODE_UNSPECIFIED;
    scd.Flags       = 0;

    IDXGISwapChain1* sc1 = nullptr;
    HRESULT hr = factory_->CreateSwapChainForHwnd(queue_, entry.hwnd, &scd,
                                                   nullptr, nullptr, &sc1);
    if (FAILED(hr)) return SwapChainHandle::Invalid;

    hr = sc1->QueryInterface(IID_PPV_ARGS(&entry.swapChain));
    sc1->Release();
    if (FAILED(hr)) return SwapChainHandle::Invalid;

    factory_->MakeWindowAssociation(entry.hwnd, DXGI_MWA_NO_ALT_ENTER);

    entry.rtvDxgiFormat       = rtvDxgi;
    entry.rtvDxgiFormatLinear = resourceDxgi;

    TextureEntry proxy{};
    proxy.ownsResource = false;
    proxy.desc.width  = width;
    proxy.desc.height = height;
    proxy.desc.format = colorFormat;
    proxy.desc.usage  = TextureUsage::RenderTarget;
    entry.proxyTexHandle = textures_.Insert(TextureEntry(proxy));

    TextureEntry proxyLinear = proxy;

    if (colorFormat == Format::R8G8B8A8_UNORM_SRGB)
        proxyLinear.desc.format = Format::R8G8B8A8_UNORM;

    entry.proxyTexHandleLinear = textures_.Insert(std::move(proxyLinear));

    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    for (UINT i = 0; i < kFramesInFlight; ++i) {
        entry.swapChain->GetBuffer(i, IID_PPV_ARGS(&entry.backBuffers[i]));

        rtvDesc.Format            = rtvDxgi;
        entry.backBufferRtvs[i]   = rtvPool_.Allocate();
        device_->CreateRenderTargetView(entry.backBuffers[i], &rtvDesc, entry.backBufferRtvs[i]);

        rtvDesc.Format                  = entry.rtvDxgiFormatLinear;
        entry.backBufferRtvsLinear[i]   = rtvPool_.Allocate();
        device_->CreateRenderTargetView(entry.backBuffers[i], &rtvDesc, entry.backBufferRtvsLinear[i]);
    }

    entry.currentBackBufferIndex = entry.swapChain->GetCurrentBackBufferIndex();

    u64 h = swapChains_.Insert(std::move(entry));
    RefreshProxyTexture(*swapChains_.Get(h));
    return static_cast<SwapChainHandle>(h);
}

void D3D12Device::RefreshProxyTexture(SwapChainEntry& sc) {
    UINT idx = sc.currentBackBufferIndex;
    if (auto* proxy = textures_.Get(sc.proxyTexHandle)) {
        proxy->resource     = sc.backBuffers[idx];
        proxy->rtvCpu       = sc.backBufferRtvs[idx];
        proxy->hasRtv       = true;

        proxy->currentState = D3D12_RESOURCE_STATE_PRESENT;
    }

    if (auto* proxy = textures_.Get(sc.proxyTexHandleLinear)) {
        proxy->resource     = sc.backBuffers[idx];
        proxy->rtvCpu       = sc.backBufferRtvsLinear[idx];
        proxy->hasRtv       = true;
        proxy->currentState = D3D12_RESOURCE_STATE_PRESENT;
    }
}

void D3D12Device::ResizeSwapChain(SwapChainHandle h, i32 width, i32 height) {
    auto* sc = swapChains_.Get(static_cast<u64>(h));
    if (!sc || !sc->swapChain) return;

    FlushGpu();

    auto* proxy = textures_.Get(sc->proxyTexHandle);
    if (proxy) {
        proxy->resource = nullptr;
        proxy->hasRtv   = false;
    }
    if (auto* lp = textures_.Get(sc->proxyTexHandleLinear)) {
        lp->resource = nullptr;
        lp->hasRtv   = false;
    }
    sc->ReleaseBackBuffers();

    HRESULT hr = sc->swapChain->ResizeBuffers(kFramesInFlight,
                                               static_cast<UINT>(width),
                                               static_cast<UINT>(height),
                                               DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) return;

    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    for (UINT i = 0; i < kFramesInFlight; ++i) {
        sc->swapChain->GetBuffer(i, IID_PPV_ARGS(&sc->backBuffers[i]));
        rtvDesc.Format = sc->rtvDxgiFormat;
        device_->CreateRenderTargetView(sc->backBuffers[i], &rtvDesc, sc->backBufferRtvs[i]);
        rtvDesc.Format = sc->rtvDxgiFormatLinear;
        device_->CreateRenderTargetView(sc->backBuffers[i], &rtvDesc, sc->backBufferRtvsLinear[i]);
    }
    sc->currentBackBufferIndex = sc->swapChain->GetCurrentBackBufferIndex();

    if (proxy) {
        proxy->desc.width  = width;
        proxy->desc.height = height;
    }
    RefreshProxyTexture(*sc);

    allocators_[frameIndex_]->Reset();
    cmdList_->Reset(allocators_[frameIndex_], nullptr);
    cmdListOpen_ = true;
    if (immediateCtx_) immediateCtx_->OnFrameBegin();
}

void D3D12Device::DestroySwapChain(SwapChainHandle h) {
    if (h == SwapChainHandle::Invalid) return;
    auto* sc = swapChains_.Get(static_cast<u64>(h));
    if (sc) {
        FlushGpu();
        if (auto* proxy = textures_.Get(sc->proxyTexHandle)) {
            proxy->resource = nullptr;
            proxy->hasRtv   = false;
        }
        if (auto* proxy = textures_.Get(sc->proxyTexHandleLinear)) {
            proxy->resource = nullptr;
            proxy->hasRtv   = false;
        }
        textures_.Remove(sc->proxyTexHandle);
        textures_.Remove(sc->proxyTexHandleLinear);
        sc->Release();
    }
    swapChains_.Remove(static_cast<u64>(h));
}

void D3D12Device::Present(SwapChainHandle h) {
    auto* sc = swapChains_.Get(static_cast<u64>(h));
    if (!sc || !sc->swapChain) return;

    auto* proxy = textures_.Get(sc->proxyTexHandle);
    if (proxy && proxy->resource && proxy->currentState != D3D12_RESOURCE_STATE_PRESENT) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = proxy->resource;
        b.Transition.StateBefore = proxy->currentState;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList_->ResourceBarrier(1, &b);
        proxy->currentState = D3D12_RESOURCE_STATE_PRESENT;
    }

    cmdList_->Close();
    cmdListOpen_ = false;
    ID3D12CommandList* lists[] = { cmdList_ };
    queue_->ExecuteCommandLists(1, lists);

    HRESULT hrPresent = sc->swapChain->Present(1, 0);
    if (FAILED(hrPresent)) {
        HRESULT removed = device_->GetDeviceRemovedReason();
        char msg[256]{};
        _snprintf_s(msg, sizeof(msg),
            "[D3D12] Present failed 0x%08X, deviceRemovedReason=0x%08X\n",
            hrPresent, removed);
        OutputDebugStringA(msg);
    }

    ++fenceValue_;
    queue_->Signal(fence_, fenceValue_);
    frameFenceValues_[frameIndex_] = fenceValue_;

    uploadRing_.EndFrame(fenceValue_);
    cbvSrvUavRing_.EndFrame(fenceValue_);
    samplerRing_.EndFrame(fenceValue_);

    frameIndex_ = (frameIndex_ + 1) % kFramesInFlight;
    WaitForFence(frameFenceValues_[frameIndex_]);

    u64 completed = fence_->GetCompletedValue();
    uploadRing_.Retire(completed);
    cbvSrvUavRing_.Retire(completed);
    samplerRing_.Retire(completed);
    FlushPendingDeletes(completed);

    uploadRing_.BeginFrame(fenceValue_ + 1);
    cbvSrvUavRing_.BeginFrame(fenceValue_ + 1);
    samplerRing_.BeginFrame(fenceValue_ + 1);

    allocators_[frameIndex_]->Reset();
    cmdList_->Reset(allocators_[frameIndex_], nullptr);
    cmdListOpen_ = true;

    sc->currentBackBufferIndex = sc->swapChain->GetCurrentBackBufferIndex();
    RefreshProxyTexture(*sc);

    if (immediateCtx_) immediateCtx_->OnFrameBegin();
}

TextureHandle D3D12Device::GetSwapChainBackBuffer(SwapChainHandle h) {
    auto* sc = swapChains_.Get(static_cast<u64>(h));
    if (!sc) return TextureHandle::Invalid;
    return static_cast<TextureHandle>(sc->proxyTexHandle);
}

TextureHandle D3D12Device::GetSwapChainBackBufferLinear(SwapChainHandle h) {
    auto* sc = swapChains_.Get(static_cast<u64>(h));
    if (!sc) return TextureHandle::Invalid;
    return static_cast<TextureHandle>(sc->proxyTexHandleLinear);
}

IGFXCommandList* D3D12Device::GetImmediateContext() {
    return immediateCtx_.get();
}

}
