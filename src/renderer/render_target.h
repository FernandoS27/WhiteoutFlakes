#pragma once

#include "common_types.h"
#include "gfx/gfx.h"

namespace WhiteoutDex {

struct Rect {
    i32 left, top, right, bottom;
};

using RenderTargetId = u32;

enum class RenderMode : u8 {
    SD = 0,
    HD = 1,
};

enum class LightingMode : u8 {
    InGame  = 0,
    Glue    = 1,
    Dynamic = 2,
};

enum class IblMode : u8 {
    Portrait = 0,
    DayNight = 1,
    Dungeon  = 2,
    Sunset   = 3,
};

struct DisplayFlags {
    bool showGrid       = true;
    bool showParticles  = true;
    bool showRibbons    = true;
    bool showCollisions = false;
    bool showLights     = false;
    bool showEvents     = true;
    RenderMode renderMode = RenderMode::SD;
};

struct RenderTarget {
    RenderTargetId        id     = 0;
    gfx::SwapChainHandle  swap   = gfx::SwapChainHandle::Invalid;

    gfx::TextureHandle    color  = gfx::TextureHandle::Invalid;

    gfx::TextureHandle    colorLinear = gfx::TextureHandle::Invalid;

    gfx::TextureHandle    hdrColor = gfx::TextureHandle::Invalid;
    gfx::TextureHandle    depth  = gfx::TextureHandle::Invalid;
    i32                   width  = 0;
    i32                   height = 0;
};

}
