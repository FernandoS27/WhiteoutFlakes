#pragma once

// ============================================================================
// ImGui view-cube widget — an orientation gizmo drawn entirely with ImDrawList.
//
// Host code (basic_viewer, max_plugin) calls DrawViewCube() once per frame
// between ImGui::NewFrame() and ImGui::Render(). It reflects the camera's
// yaw/pitch, snaps the camera to an axis view when a face is clicked, and
// resets it via the Home button. The renderer knows nothing about it.
// ============================================================================

#include "renderer/camera.h"
#include "whiteout/flakes/util/coordinate_system.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace whiteout::flakes::tools {

using whiteout::Vector3f;

namespace detail {

inline f32 Dot(const Vector3f& a, const Vector3f& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

// Lerp a packed colour towards white by `t` (alpha kept). Channel-order
// agnostic — uses ImGui's IM_COL32 shift constants.
inline ImU32 Brighten(ImU32 c, f32 t) {
    auto ch = [&](i32 shift) -> ImU32 {
        const i32 v = static_cast<i32>((c >> shift) & 0xFF);
        return static_cast<ImU32>(v + static_cast<i32>((255 - v) * t));
    };
    return (ch(IM_COL32_R_SHIFT) << IM_COL32_R_SHIFT) |
           (ch(IM_COL32_G_SHIFT) << IM_COL32_G_SHIFT) |
           (ch(IM_COL32_B_SHIFT) << IM_COL32_B_SHIFT) |
           (((c >> IM_COL32_A_SHIFT) & 0xFF) << IM_COL32_A_SHIFT);
}

// True if `p` lies inside the convex quad `q[0..3]` (consistent edge winding).
inline bool PointInQuad(const ImVec2& p, const ImVec2 q[4]) {
    bool pos = false, neg = false;
    for (i32 i = 0; i < 4; ++i) {
        const ImVec2& a = q[i];
        const ImVec2& b = q[(i + 1) & 3];
        const f32 s = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
        pos |= (s > 0.0f);
        neg |= (s < 0.0f);
    }
    return !(pos && neg);
}

} // namespace detail

// Draws the view-cube as a small borderless overlay near the top-right of the
// main viewport.
inline void DrawViewCube(renderer::Camera& camera) {
    using renderer::CoordinateSystem;
    using renderer::CoordSpace;

    // The six faces, in the historical view-cube order (Front, Back, Left,
    // Right, Top, Bottom): Max-space normals converted once to the renderer's
    // native space, plus a distinct per-face colour so orientation reads at a
    // glance without text labels.
    static constexpr Vector3f kNormalsMax[6] = {
        {0, 1, 0}, {0, -1, 0}, {-1, 0, 0}, {1, 0, 0}, {0, 0, 1}, {0, 0, -1},
    };
    static constexpr ImU32 kFaceColor[6] = {
        IM_COL32(94, 138, 196, 240),  // Front  — blue
        IM_COL32(196, 110, 92, 240),  // Back   — red
        IM_COL32(104, 178, 116, 240), // Left   — green
        IM_COL32(204, 176, 96, 240),  // Right  — amber
        IM_COL32(150, 156, 174, 240), // Top    — light grey
        IM_COL32(120, 112, 104, 240), // Bottom — dark taupe
    };
    static const std::array<Vector3f, 6> kFaceN = [] {
        std::array<Vector3f, 6> a{};
        for (i32 i = 0; i < 6; ++i)
            a[i] = CoordinateSystem::ConvertDirection(CoordSpace::Max,
                                                     CoordinateSystem::Default(), kNormalsMax[i]);
        return a;
    }();

    // ---- Layout ----
    constexpr f32 kCube = 86.0f; // cube draw area (square)
    constexpr f32 kHome = 22.0f; // home-button strip height
    constexpr f32 kPad = 10.0f;
    const f32 boxW = kCube + 2.0f * kPad;
    const f32 boxH = kCube + kHome + 2.0f * kPad;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 boxPos(vp->WorkPos.x + vp->WorkSize.x - boxW - 8.0f, vp->WorkPos.y + 8.0f);

    ImGui::SetNextWindowPos(boxPos);
    ImGui::SetNextWindowSize(ImVec2(boxW, boxH));
    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavInputs;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    if (!ImGui::Begin("##viewcube", nullptr, kFlags)) {
        ImGui::End();
        ImGui::PopStyleVar(2);
        return;
    }

    // One invisible button makes the whole box an interactive item — that
    // gates camera input (WantCaptureMouse) and gives a clean click signal.
    ImGui::InvisibleButton("##viewcube_hit", ImVec2(boxW, boxH));
    const bool active = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // ---- Home button ----
    const ImVec2 homeMin(boxPos.x + kPad, boxPos.y + kPad);
    const ImVec2 homeMax(homeMin.x + kCube, homeMin.y + kHome);
    const bool homeHover = active && mouse.x >= homeMin.x && mouse.x <= homeMax.x &&
                           mouse.y >= homeMin.y && mouse.y <= homeMax.y;
    dl->AddRectFilled(homeMin, homeMax,
                      homeHover ? IM_COL32(96, 132, 188, 235) : IM_COL32(48, 56, 68, 190), 4.0f);
    {
        const char* txt = "Home";
        const ImVec2 ts = ImGui::CalcTextSize(txt);
        dl->AddText(ImVec2((homeMin.x + homeMax.x - ts.x) * 0.5f,
                           (homeMin.y + homeMax.y - ts.y) * 0.5f),
                    IM_COL32(235, 235, 235, 255), txt);
    }
    if (homeHover && clicked)
        camera.Reset();

    // ---- Cube ----
    const ImVec2 center(boxPos.x + kPad + kCube * 0.5f,
                        boxPos.y + kPad + kHome + kCube * 0.5f);
    const f32 scale = kCube * 0.5f / 0.92f; // fit the projected unit cube

    // Eye direction in renderer-native space — same yaw/pitch basis the scene
    // camera uses (see Camera / the old RenderViewCube).
    const f32 cp = std::cos(camera.GetPitch()), sp = std::sin(camera.GetPitch());
    const f32 cy = std::cos(camera.GetYaw()), sy = std::sin(camera.GetYaw());
    const Vector3f eye{cp * cy, cp * sy, sp};
    const Vector3f fwd{-eye.x, -eye.y, -eye.z}; // into the screen

    Vector3f right = cross(Vector3f{0, 0, 1}, fwd);
    if (right.length_squared() < 1e-5f)
        right = Vector3f{1, 0, 0};
    right = right.normalized();
    const Vector3f vUp = cross(fwd, right).normalized();

    auto project = [&](const Vector3f& p) -> ImVec2 {
        return ImVec2(center.x + detail::Dot(p, right) * scale,
                      center.y - detail::Dot(p, vUp) * scale);
    };

    // Front-facing faces only, painter-sorted (nearest the eye drawn last).
    struct Vis {
        i32 face;
        f32 depth;
    };
    std::array<Vis, 6> vis{};
    i32 nVis = 0;
    for (i32 i = 0; i < 6; ++i) {
        const f32 d = detail::Dot(kFaceN[i], eye);
        if (d > 0.0f)
            vis[nVis++] = {i, d};
    }
    std::sort(vis.begin(), vis.begin() + nVis,
              [](const Vis& a, const Vis& b) { return a.depth < b.depth; });

    i32 hoverFace = -1;
    for (i32 v = 0; v < nVis; ++v) {
        const i32 i = vis[v].face;
        const Vector3f n = kFaceN[i];
        // Two in-plane axes for an axis-aligned normal.
        Vector3f a{n.y, n.z, n.x};
        a = (a - n * detail::Dot(a, n)).normalized();
        const Vector3f b = cross(n, a);
        const Vector3f c = n * 0.5f;
        const ImVec2 quad[4] = {
            project(c + a * 0.5f + b * 0.5f),
            project(c - a * 0.5f + b * 0.5f),
            project(c - a * 0.5f - b * 0.5f),
            project(c + a * 0.5f - b * 0.5f),
        };

        const bool hover = active && detail::PointInQuad(mouse, quad);
        if (hover)
            hoverFace = i;

        const ImU32 fill = hover ? detail::Brighten(kFaceColor[i], 0.38f) : kFaceColor[i];
        dl->AddConvexPolyFilled(quad, 4, fill);
        dl->AddPolyline(quad, 4, IM_COL32(28, 32, 40, 255), ImDrawFlags_Closed, 1.5f);
    }

    if (hoverFace >= 0 && clicked)
        camera.SnapToAxisView(kFaceN[hoverFace]);

    ImGui::End();
    ImGui::PopStyleVar(2);
}

} // namespace whiteout::flakes::tools
