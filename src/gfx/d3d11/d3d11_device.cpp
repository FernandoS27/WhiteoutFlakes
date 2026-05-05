#include "d3d11_device.h"
#include "d3d11_command_list.h"
#include <algorithm>
#include <cstring>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace WhiteoutDex::gfx::d3d11 {

D3D11Device::D3D11Device() = default;

D3D11Device::~D3D11Device() {
    immediateCtx_.reset();

    swapChains_.ForEach([](SwapChainEntry& e) { e.Release(); });
    swapChains_.Clear();
    samplers_.ForEach([](SamplerEntry& e) { e.Release(); });
    samplers_.Clear();
    pipelines_.ForEach([](PipelineEntry& e) { e.Release(); });
    pipelines_.Clear();
    shaders_.ForEach([](ShaderEntry& e) { e.Release(); });
    shaders_.Clear();
    textures_.ForEach([](TextureEntry& e) { e.Release(); });
    textures_.Clear();
    buffers_.ForEach([](BufferEntry& e) { e.Release(); });
    buffers_.Clear();

    SafeRelease(context_);
    SafeRelease(factory_);
    SafeRelease(device_);
}

bool D3D11Device::Init() {

    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                    reinterpret_cast<void**>(&factory_));
    if (FAILED(hr)) return false;

    IDXGIAdapter1* bestAdapter = nullptr;
    SIZE_T bestVRAM = 0;
    {
        IDXGIAdapter1* adapter = nullptr;
        for (UINT i = 0; factory_->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 desc{};
            adapter->GetDesc1(&desc);
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) { adapter->Release(); continue; }
            if (desc.DedicatedVideoMemory > bestVRAM) {
                if (bestAdapter) bestAdapter->Release();
                bestAdapter = adapter;
                bestVRAM = desc.DedicatedVideoMemory;
            } else {
                adapter->Release();
            }
        }
    }

    UINT flags = 0;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
    hr = D3D11CreateDevice(
        bestAdapter,
        bestAdapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
        nullptr, flags,
        &featureLevel, 1,
        D3D11_SDK_VERSION,
        &device_, nullptr, &context_);

    if (bestAdapter) {
        DXGI_ADAPTER_DESC1 desc{};
        bestAdapter->GetDesc1(&desc);
        char name[256]{};
        usize converted = 0;
        wcstombs_s(&converted, name, sizeof(name), desc.Description, _TRUNCATE);
        deviceName_ = name;
        bestAdapter->Release();
    }

    if (FAILED(hr)) return false;

    immediateCtx_ = std::make_unique<D3D11CommandList>(*this);
    return true;
}

BufferHandle D3D11Device::CreateBuffer(const BufferDesc& desc, const void* initial) {
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = static_cast<UINT>(desc.size);

    if (hasFlag(desc.usage, BufferUsage::Vertex))
        bd.BindFlags |= D3D11_BIND_VERTEX_BUFFER;
    if (hasFlag(desc.usage, BufferUsage::Index))
        bd.BindFlags |= D3D11_BIND_INDEX_BUFFER;
    if (hasFlag(desc.usage, BufferUsage::Constant))
        bd.BindFlags |= D3D11_BIND_CONSTANT_BUFFER;
    if (hasFlag(desc.usage, BufferUsage::ShaderResource))
        bd.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
    if (hasFlag(desc.usage, BufferUsage::UnorderedAccess))
        bd.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;

    if (hasFlag(desc.usage, BufferUsage::CpuWritable)) {
        bd.Usage          = D3D11_USAGE_DYNAMIC;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    } else if (hasFlag(desc.usage, BufferUsage::GpuWritable)) {
        bd.Usage = D3D11_USAGE_DEFAULT;
    } else {
        bd.Usage = initial ? D3D11_USAGE_IMMUTABLE : D3D11_USAGE_DEFAULT;
    }

    if (desc.elementStride > 0) {
        bd.MiscFlags        = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = desc.elementStride;
    }

    D3D11_SUBRESOURCE_DATA srd{};
    srd.pSysMem = initial;

    BufferEntry entry{};
    entry.desc = desc;
    HRESULT hr = device_->CreateBuffer(&bd, initial ? &srd : nullptr, &entry.buffer);
    if (FAILED(hr)) return BufferHandle::Invalid;

    if (hasFlag(desc.usage, BufferUsage::ShaderResource) && desc.elementStride > 0) {
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format              = DXGI_FORMAT_UNKNOWN;
        srvDesc.ViewDimension       = D3D11_SRV_DIMENSION_BUFFER;
        srvDesc.Buffer.NumElements  = static_cast<UINT>(desc.size / desc.elementStride);
        device_->CreateShaderResourceView(entry.buffer, &srvDesc, &entry.srv);
    }

    if (hasFlag(desc.usage, BufferUsage::UnorderedAccess) && desc.elementStride > 0) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format             = DXGI_FORMAT_UNKNOWN;
        uavDesc.ViewDimension      = D3D11_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.NumElements = static_cast<UINT>(desc.size / desc.elementStride);
        device_->CreateUnorderedAccessView(entry.buffer, &uavDesc, &entry.uav);
    }

    return static_cast<BufferHandle>(buffers_.Insert(std::move(entry)));
}

void D3D11Device::Destroy(BufferHandle h) {
    if (h == BufferHandle::Invalid) return;
    auto* e = buffers_.Get(static_cast<u64>(h));
    if (e) e->Release();
    buffers_.Remove(static_cast<u64>(h));
}

void D3D11Device::UpdateBuffer(BufferHandle h, const void* data, usize size) {
    auto* e = buffers_.Get(static_cast<u64>(h));
    if (!e || !e->buffer) return;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = context_->Map(e->buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        std::memcpy(mapped.pData, data, size);
        context_->Unmap(e->buffer, 0);
    }
}

void* D3D11Device::MapBuffer(BufferHandle h) {
    auto* e = buffers_.Get(static_cast<u64>(h));
    if (!e || !e->buffer) return nullptr;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = context_->Map(e->buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) return nullptr;
    return mapped.pData;
}

void D3D11Device::UnmapBuffer(BufferHandle h) {
    auto* e = buffers_.Get(static_cast<u64>(h));
    if (!e || !e->buffer) return;
    context_->Unmap(e->buffer, 0);
}

TextureHandle D3D11Device::CreateTexture(const TextureDesc& desc, const void* initialPixels) {
    const UINT arraySize = desc.isCube
        ? static_cast<UINT>(desc.arraySize > 0 ? desc.arraySize : 6)
        : static_cast<UINT>(desc.arraySize > 0 ? desc.arraySize : 1);
    const UINT mipCount = desc.mipLevels == 0
        ? 0u
        : static_cast<UINT>(desc.mipLevels);

    const bool isDepth = hasFlag(desc.usage, TextureUsage::DepthStencil);
    const bool isSrv   = hasFlag(desc.usage, TextureUsage::ShaderResource);

    D3D11_TEXTURE2D_DESC td{};
    td.Width     = static_cast<UINT>(desc.width);
    td.Height    = static_cast<UINT>(desc.height);
    td.MipLevels = mipCount;
    td.ArraySize = arraySize;

    td.Format    = (isDepth && isSrv)
                       ? (desc.format == Format::D24_UNORM_S8_UINT
                              ? DXGI_FORMAT_R24G8_TYPELESS
                              : DXGI_FORMAT_R32_TYPELESS)
                       : ToDXGI(desc.format);
    td.SampleDesc.Count = 1;
    if (desc.isCube) td.MiscFlags |= D3D11_RESOURCE_MISC_TEXTURECUBE;

    if (isSrv)
        td.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
    if (hasFlag(desc.usage, TextureUsage::RenderTarget))
        td.BindFlags |= D3D11_BIND_RENDER_TARGET;
    if (isDepth)
        td.BindFlags |= D3D11_BIND_DEPTH_STENCIL;

    td.Usage = initialPixels ? D3D11_USAGE_IMMUTABLE : D3D11_USAGE_DEFAULT;
    if (hasFlag(desc.usage, TextureUsage::RenderTarget) ||
        hasFlag(desc.usage, TextureUsage::DepthStencil))
        td.Usage = D3D11_USAGE_DEFAULT;

    const UINT fillMips = (mipCount == 0) ? 1u : mipCount;
    std::vector<D3D11_SUBRESOURCE_DATA> initSubres;
    if (initialPixels) {

        const bool  isBcn        = IsBlockCompressed(desc.format);
        const UINT  blockSize    = FormatBytesPerBlock(desc.format);
        const UINT  blockEdge    = isBcn ? 4u : 1u;
        initSubres.resize(static_cast<usize>(arraySize) * fillMips);
        const u8* cursor = static_cast<const u8*>(initialPixels);
        for (UINT slice = 0; slice < arraySize; ++slice) {
            for (UINT mip = 0; mip < fillMips; ++mip) {
                UINT mipW = std::max<UINT>(1, static_cast<UINT>(desc.width)  >> mip);
                UINT mipH = std::max<UINT>(1, static_cast<UINT>(desc.height) >> mip);
                UINT blocksW = (mipW + blockEdge - 1) / blockEdge;
                UINT blocksH = (mipH + blockEdge - 1) / blockEdge;
                D3D11_SUBRESOURCE_DATA& s = initSubres[slice * fillMips + mip];
                s.pSysMem          = cursor;
                s.SysMemPitch      = blocksW * blockSize;
                s.SysMemSlicePitch = 0;
                cursor += static_cast<usize>(s.SysMemPitch) * blocksH;
            }
        }
    }

    TextureEntry entry{};
    entry.desc = desc;
    HRESULT hr = device_->CreateTexture2D(
        &td, initialPixels ? initSubres.data() : nullptr, &entry.tex);
    if (FAILED(hr)) return TextureHandle::Invalid;

    if (hasFlag(desc.usage, TextureUsage::ShaderResource)) {
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = td.Format;

        if (desc.format == Format::D24_UNORM_S8_UINT)
            srvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        else if (desc.format == Format::D32_FLOAT)
            srvDesc.Format = DXGI_FORMAT_R32_FLOAT;

        const UINT srvMips = (mipCount == 0) ? static_cast<UINT>(-1) : mipCount;
        if (desc.isCube) {

            srvDesc.ViewDimension                    = D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
            srvDesc.TextureCubeArray.MipLevels       = srvMips;
            srvDesc.TextureCubeArray.NumCubes        = std::max<UINT>(1, arraySize / 6);
        } else if (arraySize > 1) {
            srvDesc.ViewDimension                    = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            srvDesc.Texture2DArray.MipLevels         = srvMips;
            srvDesc.Texture2DArray.ArraySize         = arraySize;
        } else {
            srvDesc.ViewDimension         = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels   = srvMips;
        }
        device_->CreateShaderResourceView(entry.tex, &srvDesc, &entry.srv);
    }

    if (hasFlag(desc.usage, TextureUsage::RenderTarget)) {
        device_->CreateRenderTargetView(entry.tex, nullptr, &entry.rtv);
    }

    if (hasFlag(desc.usage, TextureUsage::DepthStencil)) {
        D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format        = ToDXGI(desc.format);
        dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        device_->CreateDepthStencilView(entry.tex, &dsvDesc, &entry.dsv);
    }

    return static_cast<TextureHandle>(textures_.Insert(std::move(entry)));
}

void D3D11Device::Destroy(TextureHandle h) {
    if (h == TextureHandle::Invalid) return;
    auto* e = textures_.Get(static_cast<u64>(h));
    if (e) e->Release();
    textures_.Remove(static_cast<u64>(h));
}

TextureHandle D3D11Device::CreateColorTarget(i32 w, i32 h, Format f) {
    TextureDesc desc{};
    desc.width     = w;
    desc.height    = h;
    desc.mipLevels = 1;
    desc.format    = f;
    desc.usage     = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
    return CreateTexture(desc, nullptr);
}

TextureHandle D3D11Device::CreateDepthTarget(i32 w, i32 h, Format f) {
    TextureDesc desc{};
    desc.width     = w;
    desc.height    = h;
    desc.mipLevels = 1;
    desc.format    = f;
    desc.usage     = TextureUsage::DepthStencil;

    D3D11_TEXTURE2D_DESC td{};
    td.Width     = static_cast<UINT>(w);
    td.Height    = static_cast<UINT>(h);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    td.Usage     = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;

    if (f == Format::D24_UNORM_S8_UINT)
        td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    else if (f == Format::D32_FLOAT)
        td.Format = DXGI_FORMAT_D32_FLOAT;
    else
        td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;

    TextureEntry entry{};
    entry.desc = desc;
    HRESULT hr = device_->CreateTexture2D(&td, nullptr, &entry.tex);
    if (FAILED(hr)) return TextureHandle::Invalid;

    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
    dsvDesc.Format        = td.Format;
    dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    device_->CreateDepthStencilView(entry.tex, &dsvDesc, &entry.dsv);

    return static_cast<TextureHandle>(textures_.Insert(std::move(entry)));
}

ShaderHandle D3D11Device::CreateShader(ShaderStage stage, const void* bytecode, usize size) {
    ShaderEntry entry{};
    entry.stage = stage;
    entry.bytecode.assign(static_cast<const u8*>(bytecode),
                          static_cast<const u8*>(bytecode) + size);
    HRESULT hr = E_FAIL;
    switch (stage) {
        case ShaderStage::Vertex:
            hr = device_->CreateVertexShader(bytecode, size, nullptr, &entry.vs);
            break;
        case ShaderStage::Pixel:
            hr = device_->CreatePixelShader(bytecode, size, nullptr, &entry.ps);
            break;
        case ShaderStage::Compute:
            hr = device_->CreateComputeShader(bytecode, size, nullptr, &entry.cs);
            break;
    }
    if (FAILED(hr)) return ShaderHandle::Invalid;
    return static_cast<ShaderHandle>(shaders_.Insert(std::move(entry)));
}

void D3D11Device::Destroy(ShaderHandle h) {
    if (h == ShaderHandle::Invalid) return;
    auto* e = shaders_.Get(static_cast<u64>(h));
    if (e) e->Release();
    shaders_.Remove(static_cast<u64>(h));
}

PipelineHandle D3D11Device::CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) {
    PipelineEntry entry{};
    entry.isCompute = false;
    entry.topology  = ToD3D11(desc.topology);

    auto* vsEntry = shaders_.Get(static_cast<u64>(desc.vs));
    auto* psEntry = shaders_.Get(static_cast<u64>(desc.ps));
    if (!vsEntry || !vsEntry->vs) return PipelineHandle::Invalid;
    entry.vs = vsEntry->vs;
    entry.ps = psEntry ? psEntry->ps : nullptr;

    D3D11_BLEND_DESC bd{};
    bd.AlphaToCoverageEnable = desc.blend.alphaToCoverage ? TRUE : FALSE;
    entry.alphaToCoverage    = desc.blend.alphaToCoverage;
    bd.RenderTarget[0].BlendEnable           = desc.blend.enable ? TRUE : FALSE;
    bd.RenderTarget[0].SrcBlend              = ToD3D11(desc.blend.srcColor);
    bd.RenderTarget[0].DestBlend             = ToD3D11(desc.blend.dstColor);
    bd.RenderTarget[0].BlendOp               = ToD3D11(desc.blend.opColor);
    bd.RenderTarget[0].SrcBlendAlpha         = ToD3D11(desc.blend.srcAlpha);
    bd.RenderTarget[0].DestBlendAlpha        = ToD3D11(desc.blend.dstAlpha);
    bd.RenderTarget[0].BlendOpAlpha          = ToD3D11(desc.blend.opAlpha);
    bd.RenderTarget[0].RenderTargetWriteMask = desc.blend.colorWrite ? D3D11_COLOR_WRITE_ENABLE_ALL : 0;
    device_->CreateBlendState(&bd, &entry.blendState);

    D3D11_DEPTH_STENCIL_DESC dsd{};
    dsd.DepthEnable    = desc.depthStencil.depthTest ? TRUE : FALSE;
    dsd.DepthWriteMask = desc.depthStencil.depthWrite ? D3D11_DEPTH_WRITE_MASK_ALL
                                                      : D3D11_DEPTH_WRITE_MASK_ZERO;
    dsd.DepthFunc      = ToD3D11(desc.depthStencil.depthCompare);
    device_->CreateDepthStencilState(&dsd, &entry.depthState);

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode        = ToD3D11(desc.rasterizer.fill);
    rd.CullMode        = ToD3D11(desc.rasterizer.cull);
    rd.FrontCounterClockwise = desc.rasterizer.frontCCW ? TRUE : FALSE;
    rd.ScissorEnable          = desc.rasterizer.scissorEnable ? TRUE : FALSE;
    rd.DepthClipEnable        = TRUE;
    rd.AntialiasedLineEnable  = TRUE;
    rd.DepthBias              = desc.rasterizer.depthBias;
    rd.SlopeScaledDepthBias   = desc.rasterizer.slopeScaledDepthBias;
    rd.DepthBiasClamp         = desc.rasterizer.depthBiasClamp;
    device_->CreateRasterizerState(&rd, &entry.rasterState);

    if (!desc.inputLayout.empty() && !vsEntry->bytecode.empty()) {
        std::vector<D3D11_INPUT_ELEMENT_DESC> elems;
        elems.reserve(desc.inputLayout.size());
        for (auto& ie : desc.inputLayout) {
            D3D11_INPUT_ELEMENT_DESC d{};
            d.SemanticName         = ie.semantic;
            d.SemanticIndex        = ie.semanticIndex;
            d.Format               = ToDXGI(ie.format);
            d.InputSlot            = ie.inputSlot;
            d.AlignedByteOffset    = ie.offset;
            d.InputSlotClass       = D3D11_INPUT_PER_VERTEX_DATA;
            d.InstanceDataStepRate = 0;
            elems.push_back(d);
        }
        device_->CreateInputLayout(elems.data(), static_cast<UINT>(elems.size()),
                                   vsEntry->bytecode.data(), vsEntry->bytecode.size(),
                                   &entry.inputLayout);
    }

    return static_cast<PipelineHandle>(pipelines_.Insert(std::move(entry)));
}

PipelineHandle D3D11Device::CreateComputePipeline(const ComputePipelineDesc& desc) {
    auto* csEntry = shaders_.Get(static_cast<u64>(desc.cs));
    if (!csEntry || !csEntry->cs) return PipelineHandle::Invalid;

    PipelineEntry entry{};
    entry.isCompute = true;
    entry.cs        = csEntry->cs;
    return static_cast<PipelineHandle>(pipelines_.Insert(std::move(entry)));
}

void D3D11Device::Destroy(PipelineHandle h) {
    if (h == PipelineHandle::Invalid) return;
    auto* e = pipelines_.Get(static_cast<u64>(h));
    if (e) e->Release();
    pipelines_.Remove(static_cast<u64>(h));
}

SamplerHandle D3D11Device::CreateSampler(const SamplerDesc& desc) {
    D3D11_SAMPLER_DESC sd{};
    sd.Filter   = desc.comparison
                      ? ToD3D11FilterComparison(desc.minFilter, desc.magFilter)
                      : ToD3D11Filter(desc.minFilter, desc.magFilter);
    sd.AddressU = ToD3D11(desc.addressU);
    sd.AddressV = ToD3D11(desc.addressV);
    sd.AddressW = ToD3D11(desc.addressW);
    sd.MaxAnisotropy = 1;
    sd.ComparisonFunc = desc.comparison
                            ? ToD3D11(desc.comparisonFunc)
                            : D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (desc.comparison) {
        sd.BorderColor[0] = 1.0f;
        sd.BorderColor[1] = 1.0f;
        sd.BorderColor[2] = 1.0f;
        sd.BorderColor[3] = 1.0f;
    }

    SamplerEntry entry{};
    HRESULT hr = device_->CreateSamplerState(&sd, &entry.sampler);
    if (FAILED(hr)) return SamplerHandle::Invalid;
    return static_cast<SamplerHandle>(samplers_.Insert(std::move(entry)));
}

void D3D11Device::Destroy(SamplerHandle h) {
    if (h == SamplerHandle::Invalid) return;
    auto* e = samplers_.Get(static_cast<u64>(h));
    if (e) e->Release();
    samplers_.Remove(static_cast<u64>(h));
}

SwapChainHandle D3D11Device::CreateSwapChain(void* nativeWindowHandle,
                                              i32 width, i32 height,
                                              Format colorFormat) {

    DXGI_FORMAT rtvDxgi = ToDXGI(colorFormat);

    DXGI_FORMAT rtvDxgiLinear = rtvDxgi;
    switch (rtvDxgi) {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: rtvDxgiLinear = DXGI_FORMAT_R8G8B8A8_UNORM; break;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: rtvDxgiLinear = DXGI_FORMAT_B8G8R8A8_UNORM; break;
        default: break;
    }

    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferCount       = 1;
    scd.BufferDesc.Width  = static_cast<UINT>(width);
    scd.BufferDesc.Height = static_cast<UINT>(height);
    scd.BufferDesc.Format = rtvDxgi;
    scd.BufferUsage       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow      = static_cast<HWND>(nativeWindowHandle);
    scd.SampleDesc.Count  = 1;
    scd.Windowed          = TRUE;

    SwapChainEntry entry{};
    entry.rtvDxgiFormat       = rtvDxgi;
    entry.rtvDxgiFormatLinear = rtvDxgiLinear;
    HRESULT hr = factory_->CreateSwapChain(device_, &scd, &entry.swapChain);
    if (FAILED(hr)) return SwapChainHandle::Invalid;

    factory_->MakeWindowAssociation(static_cast<HWND>(nativeWindowHandle),
                                    DXGI_MWA_NO_ALT_ENTER);

    CreateSwapChainViews(entry);
    return static_cast<SwapChainHandle>(swapChains_.Insert(std::move(entry)));
}

void D3D11Device::CreateSwapChainViews(SwapChainEntry& sc) {
    sc.ReleaseBackBuffer();

    auto dropProxy = [&](u64& h) {
        if (h == 0) return;
        if (auto* te = textures_.Get(h)) {
            SafeRelease(te->rtv);
            SafeRelease(te->srv);
            te->tex = nullptr;
        }
        textures_.Remove(h);
        h = 0;
    };
    dropProxy(sc.backBufferTexHandle);
    dropProxy(sc.backBufferTexHandleLinear);

    sc.swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                            reinterpret_cast<void**>(&sc.backBuffer));
    if (sc.backBuffer) {

        sc.backBufferTexHandle       = static_cast<u64>(RegisterBackBuffer(sc.backBuffer, sc.rtvDxgiFormat));
        sc.backBufferTexHandleLinear = static_cast<u64>(RegisterBackBuffer(sc.backBuffer, sc.rtvDxgiFormatLinear));
    }
}

TextureHandle D3D11Device::RegisterBackBuffer(ID3D11Texture2D* bb, DXGI_FORMAT rtvFormat) {
    TextureEntry entry{};
    entry.tex = bb;
    bb->AddRef();

    D3D11_TEXTURE2D_DESC td{};
    bb->GetDesc(&td);
    entry.desc.width  = static_cast<i32>(td.Width);
    entry.desc.height = static_cast<i32>(td.Height);
    entry.desc.usage  = TextureUsage::RenderTarget;

    D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
    rtvDesc.Format        = rtvFormat;
    rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    device_->CreateRenderTargetView(bb, &rtvDesc, &entry.rtv);
    return static_cast<TextureHandle>(textures_.Insert(std::move(entry)));
}

void D3D11Device::ResizeSwapChain(SwapChainHandle h, i32 width, i32 height) {
    auto* sc = swapChains_.Get(static_cast<u64>(h));
    if (!sc || !sc->swapChain) return;

    context_->OMSetRenderTargets(0, nullptr, nullptr);

    auto dropProxy = [&](u64& h) {
        if (h == 0) return;
        if (auto* te = textures_.Get(h)) {
            SafeRelease(te->rtv);
            SafeRelease(te->srv);
            te->tex = nullptr;
        }
        textures_.Remove(h);
        h = 0;
    };
    dropProxy(sc->backBufferTexHandle);
    dropProxy(sc->backBufferTexHandleLinear);
    sc->ReleaseBackBuffer();

    sc->swapChain->ResizeBuffers(0, static_cast<UINT>(width), static_cast<UINT>(height),
                                 DXGI_FORMAT_UNKNOWN, 0);
    CreateSwapChainViews(*sc);
}

void D3D11Device::DestroySwapChain(SwapChainHandle h) {
    if (h == SwapChainHandle::Invalid) return;
    auto* sc = swapChains_.Get(static_cast<u64>(h));
    if (sc) {
        auto dropProxy = [&](u64 handle) {
            if (handle == 0) return;
            if (auto* te = textures_.Get(handle)) {
                SafeRelease(te->rtv);
                SafeRelease(te->srv);
                te->tex = nullptr;
            }
            textures_.Remove(handle);
        };
        dropProxy(sc->backBufferTexHandle);
        dropProxy(sc->backBufferTexHandleLinear);
        sc->Release();
    }
    swapChains_.Remove(static_cast<u64>(h));
}

void D3D11Device::Present(SwapChainHandle h) {
    auto* sc = swapChains_.Get(static_cast<u64>(h));
    if (sc && sc->swapChain)
        sc->swapChain->Present(1, 0);
}

TextureHandle D3D11Device::GetSwapChainBackBuffer(SwapChainHandle h) {
    auto* sc = swapChains_.Get(static_cast<u64>(h));
    if (!sc) return TextureHandle::Invalid;
    return static_cast<TextureHandle>(sc->backBufferTexHandle);
}

TextureHandle D3D11Device::GetSwapChainBackBufferLinear(SwapChainHandle h) {
    auto* sc = swapChains_.Get(static_cast<u64>(h));
    if (!sc) return TextureHandle::Invalid;
    return static_cast<TextureHandle>(sc->backBufferTexHandleLinear);
}

IGFXCommandList* D3D11Device::GetImmediateContext() {
    return immediateCtx_.get();
}

}
