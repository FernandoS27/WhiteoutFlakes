// ClassifyGeoset — WC3's IsOpaque rule, which decides whether a geoset joins
// the opaque queue or the back-to-front transparent one. Getting it wrong is a
// whole-model sorting artefact, so the rule gets its own tests.

#include <catch2/catch_test_macros.hpp>

#include "core/geoset_classify.h"
#include "model/render_model.h"
#include "core/render_detail.h"

#include <vector>

using whiteout::flakes::f32;
using whiteout::flakes::i32;
using whiteout::flakes::renderer::model::GPUGeoset;
using whiteout::flakes::renderer::model::GPUMaterial;
using whiteout::flakes::renderer::model::MaterialLayerData;
using whiteout::flakes::renderer::render_detail::ClassifyGeoset;
using whiteout::flakes::renderer::render_detail::GeosetClass;
using whiteout::flakes::renderer::render_detail::RenderableView;

namespace whiteout_flakes_test {

// HD shader ids; 0 is SD-on-HD and keeps the SD (blend-mode-only) rule.
constexpr i32 kShaderHd = 1;
constexpr i32 kShaderCrystal = 24;

MaterialLayerData MakeLayer(i32 filterMode, f32 alpha, i32 shaderId = 0) {
    MaterialLayerData l{};
    l.filterMode = filterMode;
    l.textureId = 0;
    l.alpha = alpha;
    l.flags = 0;
    l.shaderId = shaderId;
    return l;
}

// One geoset, one material, the fields ClassifyGeoset actually reads. The
// vectors have to outlive the view, so the caller owns them.
struct Fixture {
    std::vector<GPUMaterial> materials{GPUMaterial{}};
    GPUGeoset geo;
    RenderableView view;

    Fixture() {
        geo.materialId = 0;
        geo.geosetAlpha = 1.0f;
        view.materials = &materials;
        view.parentVisibility = 1.0f;
    }

    void SetLayers(std::initializer_list<MaterialLayerData> layers) {
        materials[0].cpu.layers.assign(layers);
    }

    GeosetClass Classify() {
        return ClassifyGeoset(view, geo);
    }
};

} // namespace whiteout_flakes_test

using whiteout_flakes_test::Fixture;
using whiteout_flakes_test::kShaderCrystal;
using whiteout_flakes_test::kShaderHd;
using whiteout_flakes_test::MakeLayer;
using namespace whiteout::flakes; // FILTER_* enumerators

TEST_CASE("An opaque layer classifies as opaque") {
    Fixture f;
    f.SetLayers({MakeLayer(FILTER_NONE, 1.0f)});

    const auto c = f.Classify();
    REQUIRE(c.visible);
    REQUIRE(c.firstVisibleLayer == 0);
    REQUIRE(c.opaque);
    REQUIRE_FALSE(c.needsDepthFill);
}

TEST_CASE("Alpha-keyed layers stay in the opaque queue") {
    // FILTER_TRANSPARENT is a discard, not a blend — WC3 draws it opaque.
    Fixture f;
    f.SetLayers({MakeLayer(FILTER_TRANSPARENT, 1.0f)});
    REQUIRE(f.Classify().opaque);
}

TEST_CASE("Blended and additive layers classify as transparent") {
    Fixture f;

    SECTION("blend") {
        f.SetLayers({MakeLayer(FILTER_BLEND, 1.0f)});
        REQUIRE_FALSE(f.Classify().opaque);
    }
    SECTION("additive") {
        f.SetLayers({MakeLayer(FILTER_ADDITIVE, 1.0f)});
        REQUIRE_FALSE(f.Classify().opaque);
    }
    SECTION("modulate") {
        f.SetLayers({MakeLayer(FILTER_MODULATE, 1.0f)});
        REQUIRE_FALSE(f.Classify().opaque);
    }
}

TEST_CASE("A fully faded geoset is not visible at all") {
    Fixture f;
    f.SetLayers({MakeLayer(FILTER_NONE, 1.0f)});
    f.geo.geosetAlpha = 0.0f;

    const auto c = f.Classify();
    REQUIRE_FALSE(c.visible);
    REQUIRE(c.firstVisibleLayer == -1);
}

TEST_CASE("Parent visibility folds into the geoset alpha") {
    Fixture f;
    f.SetLayers({MakeLayer(FILTER_NONE, 1.0f)});
    f.geo.geosetAlpha = 0.5f;
    f.view.parentVisibility = 0.0f; // e.g. the owning actor faded out

    REQUIRE_FALSE(f.Classify().visible);
}

TEST_CASE("Classification picks the first *visible* layer") {
    // An invisible leading layer must not decide the geoset's queue.
    Fixture f;
    f.SetLayers({MakeLayer(FILTER_BLEND, 0.0f), MakeLayer(FILTER_NONE, 1.0f)});

    const auto c = f.Classify();
    REQUIRE(c.visible);
    REQUIRE(c.firstVisibleLayer == 1);
    REQUIRE(c.opaque);
}

TEST_CASE("A material-less geoset is one implicit opaque layer") {
    Fixture f;
    f.geo.materialId = -1;

    const auto c = f.Classify();
    REQUIRE(c.visible);
    REQUIRE(c.opaque);
}

TEST_CASE("An out-of-range material id does not read past the vector") {
    Fixture f;
    f.geo.materialId = 42; // materials has exactly one entry

    const auto c = f.Classify();
    REQUIRE(c.visible); // falls back to the implicit opaque layer
    REQUIRE(c.opaque);
}

TEST_CASE("A fading HD opaque layer is promoted to transparent with a depth fill") {
    Fixture f;
    f.SetLayers({MakeLayer(FILTER_NONE, 0.5f, kShaderHd)});

    const auto c = f.Classify();
    REQUIRE(c.visible);
    REQUIRE_FALSE(c.opaque);
    REQUIRE(c.needsDepthFill);
}

TEST_CASE("A fully opaque HD layer stays opaque") {
    Fixture f;
    f.SetLayers({MakeLayer(FILTER_NONE, 1.0f, kShaderHd)});

    const auto c = f.Classify();
    REQUIRE(c.opaque);
    REQUIRE_FALSE(c.needsDepthFill);
}

TEST_CASE("The HD fade test uses the combined geoset x layer alpha") {
    Fixture f;
    f.SetLayers({MakeLayer(FILTER_NONE, 1.0f, kShaderCrystal)});
    f.geo.geosetAlpha = 0.5f; // layer is full, the geoset is not

    const auto c = f.Classify();
    REQUIRE_FALSE(c.opaque);
    REQUIRE(c.needsDepthFill);
}

TEST_CASE("SD-on-HD layers keep the SD rule when fading") {
    // shaderId 0 is GxShaderID_SD_ON_HD: blend mode alone decides, so a
    // half-faded opaque layer stays in the opaque queue with no depth twin.
    Fixture f;
    f.SetLayers({MakeLayer(FILTER_NONE, 0.5f, 0)});

    const auto c = f.Classify();
    REQUIRE(c.opaque);
    REQUIRE_FALSE(c.needsDepthFill);
}

TEST_CASE("An already-transparent HD layer needs no depth fill") {
    Fixture f;
    f.SetLayers({MakeLayer(FILTER_BLEND, 0.5f, kShaderHd)});

    const auto c = f.Classify();
    REQUIRE_FALSE(c.opaque);
    REQUIRE_FALSE(c.needsDepthFill); // only *opaque* HD layers get the twin
}
