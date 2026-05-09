#include "common_types.h"
#include "renderer/model/model_loader.h"
#include "renderer/render_service.h"
#include "renderer/scene_manager.h"
#include "renderer/camera.h"
#include "renderer/model/model_instance.h"
#include "renderer/model/model_template.h"
#include "windows_sound_emitter.h"
#include "render_window.h"
#include "settings_ini.h"
#include "gfx/gfx_types.h"
#include "io/path_utf8.h"

#include <cctype>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>

using whiteout::flakes::i32;
using whiteout::flakes::f32;
using whiteout::flakes::usize;

static std::filesystem::path OpenFileDialog() {
    wchar_t filename[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize  = sizeof(ofn);
    ofn.lpstrFilter  = L"MDX Files (*.mdx)\0*.mdx\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile    = filename;
    ofn.nMaxFile     = MAX_PATH;
    ofn.lpstrTitle   = L"Open MDX Model";
    ofn.Flags        = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn))
        return std::filesystem::path(filename);
    return {};
}

int wmain(int argc, wchar_t* argv[]) {

    whiteout::flakes::gfx::GfxApi backend = whiteout::flakes::gfx::GfxApi::D3D12;
    std::filesystem::path mdxPath;

    for (i32 i = 1; i < argc; ++i) {
        const wchar_t* a = argv[i];
        if ((std::wcscmp(a, L"--backend") == 0 || std::wcscmp(a, L"-b") == 0) && i + 1 < argc) {
            const wchar_t* v = argv[++i];
            if      (_wcsicmp(v, L"d3d11") == 0 || _wcsicmp(v, L"dx11") == 0)
                backend = whiteout::flakes::gfx::GfxApi::D3D11;
            else if (_wcsicmp(v, L"d3d12") == 0 || _wcsicmp(v, L"dx12") == 0)
                backend = whiteout::flakes::gfx::GfxApi::D3D12;
            else {
                std::wcerr << L"Unknown backend: " << v << L" (valid: d3d11, d3d12)\n";
                return 1;
            }
        } else if (std::wcscmp(a, L"--help") == 0 || std::wcscmp(a, L"-h") == 0) {
            std::cout << "Usage: WhiteoutFlakes.exe [--backend d3d11|d3d12] [<mdx-path>]\n";
            return 0;
        } else if (mdxPath.empty()) {
            mdxPath = a;
        }
    }

    if (mdxPath.empty()) {
        mdxPath = OpenFileDialog();
        if (mdxPath.empty()) {
            std::cerr << "No file selected.\n";
            return 1;
        }
    }
    if (!std::filesystem::exists(mdxPath)) {

        std::cerr << "File not found: " << whiteout::flakes::io::PathToUtf8(mdxPath) << "\n";
        return 1;
    }

    std::cout << "Backend: "
              << (backend == whiteout::flakes::gfx::GfxApi::D3D11 ? "D3D11" : "D3D12") << "\n";

    whiteout::flakes::renderer::SceneManager  scene;
    whiteout::flakes::renderer::RenderService renderer(scene);

    whiteout::flakes::RenderWindow  renderWindow(renderer);
    if (!renderWindow.Open(1024, 768, backend)) {
        std::cerr << "Failed to open renderer window\n";
        return 1;
    }

    bool loopPolicy = renderWindow.LoopNonLoopingPolicy();
    whiteout::flakes::LoadSettingsIni(renderer, loopPolicy);
    renderWindow.SetLoopNonLoopingPolicy(loopPolicy);

    renderWindow.SyncViewMenuFromService();

    scene.SetPE1BasePath(mdxPath.parent_path());

    renderer.SwapSoundEmitter(std::make_unique<whiteout::flakes::WindowsSoundEmitter>(
        scene.ActiveContentProvider()));

    std::cout << "Loading " << whiteout::flakes::io::PathToUtf8(mdxPath.filename()) << "...\n";
    renderer.Loader().RequestClearAll();
    whiteout::flakes::renderer::model::Actor* hero =
        renderer.Loader().SpawnUnit(whiteout::flakes::io::PathToUtf8(mdxPath));
    if (!hero) {
        std::cerr << "Failed to load MDX.\n";
        return 1;
    }
    renderWindow.SetFocusActor(hero->handle);
    hero->ignoreNonLooping = renderWindow.LoopNonLoopingPolicy();
    renderer.Settings().SetRenderMode(hero->PreferredRenderMode());

    auto sequences = hero->animation.Sequences();
    std::cout << "Loaded: " << hero->render.gpuMaterials.size() << " materials, "
              << hero->animation.Source()->GetSequences().size() << " sequences"
              << ", template @ " << hero->sourceTemplate.get() << "\n";

    if (!sequences.empty()) {
        std::vector<std::string> names;
        names.reserve(sequences.size());
        for (auto& s : sequences) names.push_back(s.name);
        renderWindow.SetSequences(std::move(names), sequences);
        std::cout << "Playing: " << sequences[0].name
                  << " [" << sequences[0].startMs << "-" << sequences[0].endMs << "ms]\n";
    }

    scene.Camera().SetPitch(30.0f);
    scene.Camera().SetYaw(45.0f);
    scene.Camera().SetDistance(300.0f);
    scene.Camera().SetTarget(0, 0, 50.0f);

    if (hero->sourceTemplate && !hero->sourceTemplate->cameraPresets.empty())
        renderWindow.SetCameraPresets(hero->sourceTemplate->cameraPresets);

    struct WalkDrift {
        i32   prevSeqIdx  = -1;
        f32 accumulated = 0.0f;
    };
    WalkDrift drift;
    constexpr f32 kDefaultWalkSpeed = 100.0f;
    auto containsWalk = [](const std::string& s) {
        for (usize i = 0; i + 4 <= s.size(); ++i) {
            const auto lc = [](char c) {
                return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            };
            if (lc(s[i]) == 'w' && lc(s[i+1]) == 'a' && lc(s[i+2]) == 'l' && lc(s[i+3]) == 'k')
                return true;
        }
        return false;
    };
    auto effectiveMoveSpeed = [&](const whiteout::flakes::renderer::model::SequenceInfo& s) {
        if (!containsWalk(s.name)) return 0.0f;
        return s.moveSpeed != 0.0f ? s.moveSpeed : kDefaultWalkSpeed;
    };
    auto applyWalkDrift = [&](f32 dt) {
        if (!hero) return;
        if (scene.Camera().GetMode() != whiteout::flakes::renderer::Camera::Mode::Orbital) return;
        const i32   idx  = hero->animation.ActiveSequenceIndex();
        const auto& seqs = renderWindow.SequenceRanges();
        f32 delta = 0.0f;
        if (idx != drift.prevSeqIdx) {

            delta = -drift.accumulated;
            drift.prevSeqIdx = idx;
        } else if (idx >= 0 && idx < (i32)seqs.size()) {
            const f32 ms = effectiveMoveSpeed(seqs[idx]);
            if (ms != 0.0f) delta = ms * dt;
        }
        if (delta == 0.0f) return;
        drift.accumulated += delta;
        hero->worldTransform.data[3][0] += delta;
        const auto t = scene.Camera().GetTarget();
        scene.Camera().SetTarget(t.x + delta, t.y, t.z);
    };

    auto last = std::chrono::steady_clock::now();
    std::cout << "Renderer open. Close the window to exit.\n";

    while (renderWindow.IsOpen()) {
        auto  now = std::chrono::steady_clock::now();
        f32 dt  = std::chrono::duration<f32>(now - last).count();
        last = now;
        scene.Update(dt);
        applyWalkDrift(dt);

        if (auto* dnc = renderer.GetDncService()) dnc->Advance(dt);
        Sleep(16);
    }

    renderWindow.Close();
    std::cout << "Done.\n";
    return 0;
}
