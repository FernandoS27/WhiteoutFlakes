// Texture-usage policy: which textures are colour (sRGB) and which are data
// tables (linear). A wrong answer here desaturates a model or flattens its
// normals, and it's driven purely by path naming — cheap to pin down.

#include <catch2/catch_test_macros.hpp>

#include "whiteout/flakes/util/texture_image_usage.h"

using whiteout::flakes::gfx::Format;
using whiteout::flakes::io::ApplySrgbPolicy;
using whiteout::flakes::io::ApplyTextureSrgbPolicy;
using whiteout::flakes::io::DetermineImageUsage;
using whiteout::flakes::io::ImageUsage;
using whiteout::flakes::io::IsLinearImageUsage;

TEST_CASE("Usage is inferred from the texture's name suffix") {
    REQUIRE(DetermineImageUsage("Textures/rock_diffuse.dds") == ImageUsage::Default);
    REQUIRE(DetermineImageUsage("Textures/rock_normal.dds") == ImageUsage::NormalMap);
    REQUIRE(DetermineImageUsage("Textures/rock_orm.dds") == ImageUsage::ORM);
    REQUIRE(DetermineImageUsage("Textures/rock_emissive.dds") == ImageUsage::Emissive);
    REQUIRE(DetermineImageUsage("Textures/sky_ibl.dds") == ImageUsage::IBL);
}

TEST_CASE("Suffix matching ignores case and separator style") {
    REQUIRE(DetermineImageUsage("Textures\\Rock_NORMAL.DDS") == ImageUsage::NormalMap);
    REQUIRE(DetermineImageUsage("TEXTURES\\ROCK_ORM.BLP") == ImageUsage::ORM);
}

TEST_CASE("A directory hint classifies textures with no suffix") {
    REQUIRE(DetermineImageUsage("Textures\\Normal\\rock.dds") == ImageUsage::NormalMap);
    REQUIRE(DetermineImageUsage("Textures\\ORM\\rock.dds") == ImageUsage::ORM);
}

TEST_CASE("Anything unrecognised is colour data") {
    REQUIRE(DetermineImageUsage("Textures/rock.blp") == ImageUsage::Default);
    REQUIRE(DetermineImageUsage("rock") == ImageUsage::Default);
    REQUIRE(DetermineImageUsage("") == ImageUsage::Default);
    // A suffix has to end the stem — "_normalish" is not a normal map.
    REQUIRE(DetermineImageUsage("Textures/rock_normalish.dds") == ImageUsage::Default);
}

TEST_CASE("Only normal and ORM maps demand linear sampling") {
    REQUIRE(IsLinearImageUsage(ImageUsage::NormalMap));
    REQUIRE(IsLinearImageUsage(ImageUsage::ORM));
    REQUIRE_FALSE(IsLinearImageUsage(ImageUsage::Default));
    REQUIRE_FALSE(IsLinearImageUsage(ImageUsage::Emissive));
    REQUIRE_FALSE(IsLinearImageUsage(ImageUsage::IBL));
}

TEST_CASE("Colour textures are promoted to their sRGB variant") {
    REQUIRE(ApplySrgbPolicy(Format::R8G8B8A8_UNORM, ImageUsage::Default) ==
            Format::R8G8B8A8_UNORM_SRGB);
    REQUIRE(ApplySrgbPolicy(Format::BC1_UNORM, ImageUsage::Default) == Format::BC1_UNORM_SRGB);
    REQUIRE(ApplySrgbPolicy(Format::BC3_UNORM, ImageUsage::Emissive) == Format::BC3_UNORM_SRGB);
    REQUIRE(ApplySrgbPolicy(Format::BC7_UNORM, ImageUsage::IBL) == Format::BC7_UNORM_SRGB);
}

TEST_CASE("Data textures are stripped back to linear") {
    REQUIRE(ApplySrgbPolicy(Format::R8G8B8A8_UNORM_SRGB, ImageUsage::NormalMap) ==
            Format::R8G8B8A8_UNORM);
    REQUIRE(ApplySrgbPolicy(Format::BC7_UNORM_SRGB, ImageUsage::ORM) == Format::BC7_UNORM);
    REQUIRE(ApplySrgbPolicy(Format::BC5_UNORM, ImageUsage::NormalMap) == Format::BC5_UNORM);
}

TEST_CASE("The SD gamma pipeline keeps colour textures raw") {
    // SD is gamma-space end to end; promoting to _SRGB would linearise on
    // sample and wash the whole model out.
    REQUIRE(ApplySrgbPolicy(Format::R8G8B8A8_UNORM, ImageUsage::Default,
                            /*gammaColorPipeline=*/true) == Format::R8G8B8A8_UNORM);
    REQUIRE(ApplySrgbPolicy(Format::BC1_UNORM_SRGB, ImageUsage::Default,
                            /*gammaColorPipeline=*/true) == Format::BC1_UNORM);
}

TEST_CASE("The policy is idempotent") {
    const Format once = ApplySrgbPolicy(Format::BC3_UNORM, ImageUsage::Default);
    REQUIRE(ApplySrgbPolicy(once, ImageUsage::Default) == once);

    const Format linear = ApplySrgbPolicy(Format::BC3_UNORM_SRGB, ImageUsage::NormalMap);
    REQUIRE(ApplySrgbPolicy(linear, ImageUsage::NormalMap) == linear);
}

TEST_CASE("Formats with no sRGB twin pass through untouched") {
    REQUIRE(ApplySrgbPolicy(Format::R16G16B16A16_FLOAT, ImageUsage::Default) ==
            Format::R16G16B16A16_FLOAT);
    REQUIRE(ApplySrgbPolicy(Format::R16G16B16A16_FLOAT, ImageUsage::NormalMap) ==
            Format::R16G16B16A16_FLOAT);
}

TEST_CASE("The path convenience overload matches the two-step call") {
    REQUIRE(ApplyTextureSrgbPolicy(Format::BC7_UNORM, "Textures/rock_normal.dds") ==
            ApplySrgbPolicy(Format::BC7_UNORM, ImageUsage::NormalMap));
    REQUIRE(ApplyTextureSrgbPolicy(Format::BC7_UNORM, "Textures/rock.blp") ==
            ApplySrgbPolicy(Format::BC7_UNORM, ImageUsage::Default));
}

TEST_CASE("The name heuristic misses StarCraft II's shipped normal-map spellings") {
    // Not a wish list — these are the four commonest misses over the 51469-model
    // SC2 + Heroes corpus, where 22.7% of normal-map references are named in a
    // way this function reads as colour. They are pinned as FAILURES on purpose:
    // the fix is not more suffixes (the next model would spell it differently
    // again), it is that a source which knows the material slot declares the
    // colour space itself and the filename is only the fallback. See
    // `M3TextureRef::linear` and AssetManager's `kTextureLinearSubKind`.
    REQUIRE(DetermineImageUsage("Assets/Textures/Marine_Normal_Blood.dds") == ImageUsage::Default);
    REQUIRE(DetermineImageUsage("Assets/Textures/Ultralisk_Normals.dds") == ImageUsage::Default);
    REQUIRE(DetermineImageUsage("Assets/Textures/Tank_Treads_Norms.dds") == ImageUsage::Default);
    REQUIRE(DetermineImageUsage("Assets/Textures/Gen_Splat4_Normal2.dds") == ImageUsage::Default);
}
