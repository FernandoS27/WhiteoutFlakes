// `--particle-diff`: the L1/L2 particle trace harness (PARTICLE_TYPES_DESIGN.md).
// Spawns the model, ticks a fixed number of frames at a fixed step, and captures
// the particle pool state plus each emitter's slice of the vertex stream. With
// --trace-record it writes a baseline; with --trace-check it compares against
// one and reports the first divergence (frame, emitter, particle, field).
//
// Both trace levels are pure CPU — BuildGeometry is a function of sim state and
// a view matrix — and emitter registration is synchronous. The per-frame tick
// is not device-free yet (FrameTicker::Tick faults without a live device), so
// the harness brings the backend up; --trace-no-device exists to retest that.

#include "harness/harness.h"
#include "harness/harness_common.h"

#include "renderer/frame_ticker.h"
#include "renderer/model/model_instance.h"
#include "renderer/model/model_loader.h"
#include "renderer/particle/output/particle_trace.h"
#include "renderer/particle/particle_service.h"
#include "renderer/render_pipeline.h"
#include "renderer/render_service.h"
#include "renderer/scene_manager.h"
#include "whiteout/flakes/util/path_utf8.h"
#if WDX_ENABLE_D3
#include "renderer/particle/d3/d3_emitter.h"
#endif

#include <cstdio>
#include <iostream>

namespace whiteout::flakes::harness {

namespace {

namespace part = renderer::particle;

#if WDX_ENABLE_D3
// A Diablo III emitter's placement and material, which the world position and
// the trace schema cannot say.
void PrintD3Emitter(const part::d3::Emitter& d3) {
    const auto& m = d3.D3Desc().d3mat;
    // Where the emitter was PUT: a hardpoint that resolved to the wrong bone and
    // one whose frame is wrong read the same from outside.
    const Matrix44f& hp = d3.AttachOffset();
    const Matrix44f& m2w = d3.ModelToWorld();
    std::printf("       place: bone=%d hp=(%.2f %.2f %.2f) m2w=(%.2f %.2f %.2f) unit=%.4f\n",
                d3.AttachBone(), hp.data[3][0], hp.data[3][1], hp.data[3][2], m2w.data[3][0],
                m2w.data[3][1], m2w.data[3][2], d3.UnitScale());
    // The erosion tail's exponent is `10 * ch6` and ch6 is per particle, so both
    // halves belong in the same line.
    f32 a6lo = 1.0f, a6hi = 1.0f;
    d3.D3Desc().Channel(part::d3::kChAlpha).ScalarRange(a6lo, a6hi);
    std::printf("       sno=%d caps=0x%04X type=%d shape=%d mat: pass=%d %s blend=(%u,%u) "
                "vcol=(%d%d %d%d) aTest=%.3f erosion=%d ch6=%.2f..%.2f layers=%u\n",
                d3.D3Desc().snoId, d3.D3Desc().caps, static_cast<int>(d3.D3Desc().systemType),
                int(d3.D3Desc().shape), m.passResolved ? 1 : 0, m.effectFile.c_str(), m.blendSrc,
                m.blendDst, m.colorVcolFirst ? 1 : 0, m.colorVcolLast ? 1 : 0,
                m.alphaVcolFirst ? 1 : 0, m.alphaVcolLast ? 1 : 0, m.alphaTest, m.erosion ? 1 : 0,
                a6lo, a6hi, m.layerCount);
    for (unsigned L = 0; L < m.layerCount; ++L) {
        const auto& lay = m.layers[L];
        std::printf("         L%u type=%2d sno=%d texId=%d wrap=%u op=%d/%d gain=%.0f/%.0f clamp=%d%d uv=%d",
                    L, lay.rawType, lay.textureSno, lay.textureId, lay.wrapFlags, int(lay.colorOp),
                    int(lay.alphaOp), lay.colorGain, lay.alphaGain, lay.colorClamp ? 1 : 0,
                    lay.alphaClamp ? 1 : 0, int(lay.uv.mode));
        // The sheet for EVERY layer, not only the mode-3 ones: the quad's base
        // rectangle and aspect come off stage 0's frame table whatever uv mode
        // stage 0 carries.
        if (lay.atlas) {
            const unsigned n = unsigned(lay.atlas->frames.size());
            std::printf(" atlas: frames=%u tile=%.4f,%.4f px=%ux%u rate=%.1f(+%.1f) start=%d..%d %s", n,
                        n ? lay.atlas->TileSize().x : 0.0f, n ? lay.atlas->TileSize().y : 0.0f,
                        n ? lay.atlas->width : 0u, n ? lay.atlas->height : 0u, lay.atlasRate,
                        lay.atlasRateJitter, lay.atlasFrameBase,
                        lay.atlasFrameBase + lay.atlasFrameRange,
                        lay.uv.mode == io::D3UvMode::Anim2D ? "flip"
                                                            : (lay.rawType == 1 ? "baseRect" : "-"));
            for (unsigned k = 0; k < n && k < 3; ++k)
                std::printf(" [%.3f,%.3f..%.3f,%.3f]", lay.atlas->frames[k].x, lay.atlas->frames[k].y,
                            lay.atlas->frames[k].z, lay.atlas->frames[k].w);
        }
        std::printf("\n");
    }
}
#endif

// What each emitter put in the vertex stream on the last frame. Enough to tell
// "nothing emitted" from "emitted at the origin" from "the right shape at the
// wrong scale" without a GPU or a baseline.
void PrintLastFrame(renderer::RenderService& renderer, const part::Trace& trace) {
    // Where the host put each emitter, straight off the live service — the one
    // number that separates "the sim is wrong" from "the emitter is in the wrong
    // place", and the trace schema carries no equivalent.
    renderer.Particles().ForEachEmitter([](const part::EmitterKey& k, const part::ParticleEmitter& e) {
        const Vector3f& w = e.WorldPosition();
        std::printf("  em %2d out=%u at=(%8.2f %8.2f %8.2f) visible=%d\n", k.id, unsigned(k.output),
                    w.x, w.y, w.z, e.Visible() ? 1 : 0);
#if WDX_ENABLE_D3
        if (const auto* d3 = e.AsD3())
            PrintD3Emitter(*d3);
#endif
    });
    const auto& f = trace.frames.back();
    std::printf("[ptrace] frame %d: %zu emitter(s)\n", f.frame, f.emitters.size());
    for (const auto& e : f.emitters) {
        const Vector3f c{(e.boundsMin.x + e.boundsMax.x) * 0.5f, (e.boundsMin.y + e.boundsMax.y) * 0.5f,
                         (e.boundsMin.z + e.boundsMax.z) * 0.5f};
        std::printf("  em %2d out=%u alive=%3zu verts=%5d centre=(%8.2f %8.2f %8.2f) "
                    "extent=(%7.2f %7.2f %7.2f) rgba=(%.3f %.3f %.3f %.3f)\n",
                    e.emitterId, unsigned(e.output), e.particles.size(), e.vertexCount, c.x, c.y, c.z,
                    e.boundsMax.x - e.boundsMin.x, e.boundsMax.y - e.boundsMin.y,
                    e.boundsMax.z - e.boundsMin.z, e.meanColor.x, e.meanColor.y, e.meanColor.z,
                    e.meanColor.w);
        for (std::size_t k = 0; k < e.particles.size() && k < 4; ++k) {
            const auto& pt = e.particles[k];
            std::printf("         p%zu pos=(%8.2f %8.2f %8.2f) vel=(%7.2f %7.2f %7.2f) age=%.3f\n", k,
                        pt.position.x, pt.position.y, pt.position.z, pt.velocity.x, pt.velocity.y,
                        pt.velocity.z, pt.age);
        }
    }
}

} // namespace

i32 RunParticleDiff(const GateContext& ctx, const cli::ParticleDiffOptions& options) {
    auto& renderer = ctx.renderer;
    auto& scene = ctx.scene;
    if (ctx.model.empty()) {
        std::cerr << "[ptrace] --particle-diff needs a model path" << std::endl;
        return exit_code::kUsage;
    }
    if (options.recordPath.empty() && options.checkPath.empty() && !options.dump) {
        std::cerr << "[ptrace] pass --trace-record <file>, --trace-check <file> or --trace-dump"
                  << std::endl;
        return exit_code::kUsage;
    }
    if (options.useDevice && !InitDevice(renderer.Pipeline(), ctx.backend, "ptrace"))
        return exit_code::kDevice;

    // Same reason --draw-trace takes one: a Diablo III `.acr` names its content
    // by SNO, and the root that resolves those is above the file's own folder.
    scene.SetPE1BasePath(ctx.contentRoot.empty() ? ctx.model.parent_path()
                                                 : io::FsPathFromUtf8(ctx.contentRoot));
    auto* hero = renderer.Loader().SpawnUnit(io::PathToUtf8(ctx.model));
    if (!hero) {
        std::cerr << "[ptrace] SpawnUnit failed: " << io::PathToUtf8(ctx.model) << std::endl;
        return exit_code::kSpawn;
    }

    if (options.dump) {
        const auto& b = hero->bounds;
        std::printf("[ptrace] model bounds valid=%d min=(%.2f %.2f %.2f) max=(%.2f %.2f %.2f)"
                    " scale=%.4f\n",
                    b.valid ? 1 : 0, b.min.x, b.min.y, b.min.z, b.max.x, b.max.y, b.max.z,
                    hero->worldScale);
    }
    const i32 emitters = renderer.Particles().EmitterCount();
    std::cout << "[ptrace] " << io::PathToUtf8(ctx.model.filename()) << ": " << emitters
              << " emitter(s), " << ctx.frames << " frames" << std::endl;
    if (emitters == 0)
        std::cout << "[ptrace] note: model has no PE2 emitters — trace covers PE1/none only"
                  << std::endl;

    // A fixed, non-axis-aligned view, so the billboard basis, tail perpendicular
    // and sort key are all exercised and the trace does not depend on wherever
    // the scene camera sits.
    const Matrix44f kTraceView = Matrix44f::rotation_x(0.4f) * Matrix44f::rotation_y(0.7f);

    part::Trace trace;
    for (i32 i = 0; i < ctx.frames; ++i) {
        scene.Update(kTraceStep);
        renderer.Ticker().Tick(kTraceStep);
        part::CaptureFrame(renderer.Particles(), kTraceView, i, trace);
    }
    if (options.dump && !trace.frames.empty())
        PrintLastFrame(renderer, trace);

    std::string err;
    if (!options.recordPath.empty()) {
        if (!part::WriteTrace(trace, options.recordPath, err)) {
            std::cerr << "[ptrace] " << err << std::endl;
            return exit_code::kTraceIo;
        }
        std::cout << "[ptrace] recorded " << trace.frames.size() << " frames -> "
                  << options.recordPath << std::endl;
    }

    bool pass = true;
    if (!options.checkPath.empty()) {
        part::Trace baseline;
        if (!part::ReadTrace(baseline, options.checkPath, err)) {
            std::cerr << "[ptrace] " << err << std::endl;
            return exit_code::kTraceIo;
        }
        part::CompareTolerance tol;
        if (options.curveTolerance) {
            // Colour within 1/255, geometry within 1e-5 relative: re-normalising
            // particle age into [0,1] and back costs a few float ULPs, which
            // shows up in the vertex bounds.
            tol.color = 1.0f / 255.0f;
            tol.position = 1e-5f;
            tol.bounds = 1e-5f;
            tol.requireVertexHash = false;
        }
        std::string report;
        pass = part::CompareTraces(baseline, trace, tol, report);
        std::cout << "[ptrace] " << (pass ? "MATCH" : "DIFF") << ": " << report << std::endl;
    }

    std::cout << "[ptrace] " << (pass ? "PASS" : "FAIL") << std::endl;
    if (options.useDevice)
        renderer.Pipeline().Shutdown();
    Exit(pass ? exit_code::kPass : exit_code::kParticleDiffFail);
}

} // namespace whiteout::flakes::harness
