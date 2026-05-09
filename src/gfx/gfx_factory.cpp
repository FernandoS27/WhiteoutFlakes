#include "gfx/gfx.h"
#include "gfx/d3d11/d3d11_device.h"
#include "gfx/d3d12/d3d12_device.h"

#include <stdexcept>

namespace whiteout::flakes::gfx {

std::unique_ptr<IGFXDevice> CreateDevice(GfxApi api) {
    switch (api) {
        case GfxApi::D3D11: {
            auto device = std::make_unique<d3d11::D3D11Device>();
            if (!device->Init())
                return nullptr;
            return device;
        }
        case GfxApi::D3D12: {
            auto device = std::make_unique<d3d12::D3D12Device>();
            if (!device->Init())
                return nullptr;
            return device;
        }
        default:
            return nullptr;
    }
}

}
