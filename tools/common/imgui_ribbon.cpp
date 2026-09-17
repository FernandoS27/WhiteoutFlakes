#include "imgui_ribbon.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace whiteout::flakes::ui {

namespace {

constexpr f32 kPi = 3.14159265358979323846f;

// One ribbon per frame, so the geometry the groups share lives here rather than
// being threaded through every call. BeginRibbon resets it.
struct RibbonState {
    RibbonLayout layout{};
    /// Screen position of the ribbon body's content area: where a group starts.
    ImVec2 bodyOrigin{};
    /// Screen y the group captions are drawn at, under the controls.
    f32 captionY = 0.0f;
    /// Half the gap a group's trailing rule sits in.
    f32 groupGap = 0.0f;
    /// Where RibbonRow puts the control after its caption.
    f32 rowLabelW = 0.0f;
    /// The x a group must end before, or it collapses to a drop-down.
    f32 bodyRight = 0.0f;
    /// Set by the first group that did not fit: everything after it collapses
    /// too, so the row cannot re-flow into a different order.
    bool collapsing = false;
};
RibbonState s_ribbon;

/// What each group measured last frame, by id. A group is only as wide as what
/// it draws, and immediate mode cannot know that before drawing it — so the
/// decision to collapse is made on the previous frame's width, which is wrong
/// for one frame after the group's contents change and right after that.
struct GroupWidth {
    ImGuiID id;
    f32 width;
};
ImVector<GroupWidth> s_groupWidths;

f32 MeasuredWidth(ImGuiID id) {
    for (const GroupWidth& g : s_groupWidths)
        if (g.id == id)
            return g.width;
    return 0.0f;
}

void RecordWidth(ImGuiID id, f32 width) {
    for (GroupWidth& g : s_groupWidths) {
        if (g.id == id) {
            g.width = width;
            return;
        }
    }
    s_groupWidths.push_back(GroupWidth{id, width});
}

/// Where the next rail cell goes. The cells stack flush, which zero item
/// spacing would also give — but the host's popups are built inside the rail's
/// window, and a pushed spacing (or padding) would land in those too.
struct RailState {
    f32 x = 0.0f;
    f32 y = 0.0f;
};
RailState s_rail;

// ---- The glyphs ----
// Each draws inside a box of side `s` centred on `c`, in the caller's colour, so
// one shape serves the small rail tab and the larger application tile alike.
//
// They share one design grid, which is what makes a row of them read as a set
// rather than as a pile of separate drawings: the ink runs to kUnit of the
// centre along the glyph's long axis and no further than kWide across it, it is
// centred on `c` — the INK, not the construction that produced it — and every
// outline carries the same weight, so none of them looks bolder than the ones
// beside it.
constexpr f32 kUnit = 0.30f;   ///< Half the ink's extent along the long axis.
constexpr f32 kWide = 0.32f;   ///< Half the widest a glyph may run.
constexpr f32 kStroke = 0.07f; ///< Outline weight, as a fraction of the box.

f32 Stroke(f32 s) {
    return std::max(1.2f, s * kStroke);
}

void DrawPage(ImDrawList* dl, ImVec2 c, f32 s, ImU32 col) {
    const f32 w = s * 0.23f;
    const f32 h = s * kUnit;
    const f32 fold = s * 0.12f;
    const f32 th = Stroke(s);
    const ImVec2 p0(c.x - w, c.y - h);
    const ImVec2 p1(c.x + w, c.y + h);
    dl->PathLineTo(p0);
    dl->PathLineTo(ImVec2(p1.x - fold, p0.y));
    dl->PathLineTo(ImVec2(p1.x, p0.y + fold));
    dl->PathLineTo(p1);
    dl->PathLineTo(ImVec2(p0.x, p1.y));
    dl->PathStroke(col, ImDrawFlags_Closed, th);
    // The fold itself, and two rules standing in for the page's text.
    dl->AddLine(ImVec2(p1.x - fold, p0.y), ImVec2(p1.x, p0.y + fold), col, th);
    for (i32 i = 0; i < 2; ++i) {
        const f32 y = c.y + s * (0.02f + 0.11f * static_cast<f32>(i));
        dl->AddLine(ImVec2(p0.x + s * 0.07f, y), ImVec2(p1.x - s * 0.07f, y), col, th * 0.8f);
    }
}

/// The application's own mark: the snowflake off the window icon. Drawn rather
/// than sampled from resources/WhiteoutFlakes.png so it takes the theme's
/// colour, stays crisp at every DPI, and costs a host no texture.
void DrawSnowflake(ImDrawList* dl, ImVec2 c, f32 s, ImU32 col) {
    const f32 r = s * kUnit;
    const f32 th = Stroke(s) * 0.7f;
    // Where the branches fork along an arm, and how long they are: six bare
    // spokes read as an asterisk, and it is the forks that make it ice.
    constexpr f32 kAt[2] = {0.46f, 0.78f};
    constexpr f32 kLen[2] = {0.36f, 0.22f};
    constexpr f32 kDiag = 0.70710678f; // 45 degrees off the arm, the way a dendrite grows
    for (i32 i = 0; i < 6; ++i) {
        const f32 a = static_cast<f32>(i) * (kPi / 3.0f) + kPi * 0.5f;
        const ImVec2 d(std::cos(a), std::sin(a));
        const ImVec2 n(-d.y, d.x);
        dl->AddLine(c, ImVec2(c.x + d.x * r, c.y + d.y * r), col, th);
        for (i32 j = 0; j < 2; ++j) {
            const ImVec2 root(c.x + d.x * r * kAt[j], c.y + d.y * r * kAt[j]);
            const f32 len = r * kLen[j];
            const ImVec2 out(d.x * kDiag * len, d.y * kDiag * len);
            const ImVec2 side(n.x * kDiag * len, n.y * kDiag * len);
            dl->AddLine(root, ImVec2(root.x + out.x + side.x, root.y + out.y + side.y), col, th);
            dl->AddLine(root, ImVec2(root.x + out.x - side.x, root.y + out.y - side.y), col, th);
        }
    }
}

void DrawFolder(ImDrawList* dl, ImVec2 c, f32 s, ImU32 col) {
    // Outlined rather than filled: solid, at this size, it carried half again
    // the weight of the strokes either side of it, which is most of what made
    // the row look uneven.
    const f32 w = s * kWide;
    const f32 th = Stroke(s);
    const f32 top = c.y - s * 0.25f; // the raised tab
    const f32 lip = c.y - s * 0.12f; // where the body's top edge runs
    const f32 bot = c.y + s * 0.25f;
    dl->PathLineTo(ImVec2(c.x - w, bot));
    dl->PathLineTo(ImVec2(c.x - w, top));
    dl->PathLineTo(ImVec2(c.x - w * 0.34f, top));
    dl->PathLineTo(ImVec2(c.x - w * 0.08f, lip));
    dl->PathLineTo(ImVec2(c.x + w, lip));
    dl->PathLineTo(ImVec2(c.x + w, bot));
    dl->PathStroke(col, ImDrawFlags_Closed, th);
}

void DrawFloppy(ImDrawList* dl, ImVec2 c, f32 s, ImU32 col) {
    const f32 r = s * 0.27f;
    const f32 th = Stroke(s);
    dl->AddRect(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), col, s * 0.05f, 0, th);
    // The shutter above, the label below: filled, so the disk reads on any
    // background without needing to know it.
    dl->AddRectFilled(ImVec2(c.x - r * 0.42f, c.y - r * 0.74f), ImVec2(c.x + r * 0.42f, c.y - r * 0.16f), col);
    dl->AddRectFilled(ImVec2(c.x - r * 0.58f, c.y + r * 0.12f), ImVec2(c.x + r * 0.58f, c.y + r * 0.72f), col);
}

/// An arrow leaving an open tray.
void DrawTray(ImDrawList* dl, ImVec2 c, f32 s, ImU32 col) {
    const f32 w = s * 0.28f;
    const f32 th = Stroke(s);
    const f32 lip = c.y + s * 0.04f;
    const f32 base = c.y + s * 0.28f;
    dl->PathLineTo(ImVec2(c.x - w, lip));
    dl->PathLineTo(ImVec2(c.x - w, base));
    dl->PathLineTo(ImVec2(c.x + w, base));
    dl->PathLineTo(ImVec2(c.x + w, lip));
    dl->PathStroke(col, ImDrawFlags_None, th);

    const f32 tip = c.y - s * 0.28f;
    dl->AddLine(ImVec2(c.x, lip), ImVec2(c.x, tip), col, th);
    const f32 head = s * 0.11f;
    dl->AddTriangleFilled(ImVec2(c.x, tip), ImVec2(c.x - head, tip + head), ImVec2(c.x + head, tip + head), col);
}

void DrawFrames(ImDrawList* dl, ImVec2 c, f32 s, ImU32 col) {
    // Two offset cards: a sequence of images, which is what an animation export
    // writes. It was a sprocketed film strip, and at a ribbon button's size the
    // sprockets closed up into a solid band — a glyph has to hold at the size it
    // is actually drawn, not at the size it is designed at.
    const f32 w = s * 0.23f;
    const f32 h = s * 0.20f;
    const f32 off = s * 0.09f;
    const f32 th = Stroke(s);
    // The card behind, as the two edges of it the front card leaves showing:
    // stroked shapes cannot occlude one another without knowing the background.
    dl->PathLineTo(ImVec2(c.x - w + off * 0.6f, c.y - h - off));
    dl->PathLineTo(ImVec2(c.x + w + off, c.y - h - off));
    dl->PathLineTo(ImVec2(c.x + w + off, c.y + h - off * 0.6f));
    dl->PathStroke(col, ImDrawFlags_None, th);
    dl->AddRect(ImVec2(c.x - w - off, c.y - h + off), ImVec2(c.x + w - off, c.y + h + off), col, s * 0.04f, 0, th);
}

void DrawGear(ImDrawList* dl, ImVec2 c, f32 s, ImU32 col) {
    const f32 r = s * 0.20f;
    const f32 tooth = s * 0.10f;
    const f32 th = Stroke(s) * 1.15f;
    dl->AddCircle(c, r, col, 0, th);
    for (i32 i = 0; i < 8; ++i) {
        const f32 a = static_cast<f32>(i) * (kPi * 0.25f);
        const ImVec2 d(std::cos(a), std::sin(a));
        dl->AddLine(ImVec2(c.x + d.x * r, c.y + d.y * r), ImVec2(c.x + d.x * (r + tooth), c.y + d.y * (r + tooth)),
                    col, th);
    }
}

void DrawStack(ImDrawList* dl, ImVec2 c, f32 s, ImU32 col) {
    // The cylinder every archive is drawn as. It was three platters seen
    // edge-on, which struck the same problem the film strip did: an outline a
    // third as thick as the platter it outlines fills the platter in.
    const f32 w = s * 0.24f;
    const f32 h = s * kUnit;
    const f32 lid = s * 0.09f; // half the height the end caps are seen at
    const f32 th = Stroke(s);
    const ImVec2 cap(w, lid);
    dl->AddEllipse(ImVec2(c.x, c.y - h + lid), cap, col, 0.0f, 0, th);
    dl->AddLine(ImVec2(c.x - w, c.y - h + lid), ImVec2(c.x - w, c.y + h - lid), col, th);
    dl->AddLine(ImVec2(c.x + w, c.y - h + lid), ImVec2(c.x + w, c.y + h - lid), col, th);
    // Only the near half of the lower cap is on a solid, and one band across the
    // middle is what makes it a stack of volumes rather than a tin.
    dl->PathEllipticalArcTo(ImVec2(c.x, c.y + h - lid), cap, 0.0f, 0.0f, kPi);
    dl->PathStroke(col, ImDrawFlags_None, th);
    dl->PathEllipticalArcTo(ImVec2(c.x, c.y), cap, 0.0f, 0.0f, kPi);
    dl->PathStroke(col, ImDrawFlags_None, th * 0.85f);
}

void DrawPerson(ImDrawList* dl, ImVec2 c, f32 s, ImU32 col) {
    // Head and shoulders: the wardrobe controls act on who is wearing it, not
    // on a garment, and a bust is the one silhouette that reads at this size.
    dl->AddCircleFilled(ImVec2(c.x, c.y - s * 0.17f), s * 0.12f, col, 0);
    dl->PathArcTo(ImVec2(c.x, c.y + s * 0.29f), s * 0.28f, kPi, kPi * 2.0f);
    dl->PathFillConvex(col);
}

void DrawCube(ImDrawList* dl, ImVec2 c, f32 s, ImU32 col) {
    // An isometric box: the hexagon silhouette plus the three edges meeting at
    // the near corner, which is what makes it read as a solid rather than a tile.
    const f32 r = s * kUnit;
    const f32 th = Stroke(s);
    ImVec2 p[6];
    for (i32 i = 0; i < 6; ++i) {
        const f32 a = static_cast<f32>(i) * (kPi / 3.0f) - kPi / 6.0f;
        p[i] = ImVec2(c.x + r * std::cos(a), c.y + r * std::sin(a));
    }
    for (i32 i = 0; i < 6; ++i)
        dl->PathLineTo(p[i]);
    dl->PathStroke(col, ImDrawFlags_Closed, th);
    for (i32 i = 0; i < 3; ++i)
        dl->AddLine(c, p[i * 2 + 1], col, th);
}

void DrawTransport(ImDrawList* dl, Icon icon, ImVec2 c, f32 s, ImU32 col) {
    switch (icon) {
    case Icon::Play: {
        // The box sits a shade right of centre: a triangle centred on its
        // bounding box reads as leaning left, because its mass is in the flat
        // edge rather than at the point.
        const f32 h = s * 0.28f;
        const f32 back = c.x - s * 0.20f;
        const f32 tip = c.x + s * 0.24f;
        dl->AddTriangleFilled(ImVec2(back, c.y - h), ImVec2(tip, c.y), ImVec2(back, c.y + h), col);
        break;
    }
    case Icon::Pause: {
        const f32 bar = s * 0.12f;
        const f32 gap = s * 0.08f; // half the space between the two bars
        const f32 h = s * 0.28f;
        dl->AddRectFilled(ImVec2(c.x - gap - bar, c.y - h), ImVec2(c.x - gap, c.y + h), col, bar * 0.3f);
        dl->AddRectFilled(ImVec2(c.x + gap, c.y - h), ImVec2(c.x + gap + bar, c.y + h), col, bar * 0.3f);
        break;
    }
    case Icon::Restart: {
        // The circle-arrow everything else uses for "play it again": most of a
        // circle with the gap across the top, and the head at the END of travel,
        // so the arrow points the way the stroke was going.
        const f32 r = s * 0.26f;
        const f32 th = Stroke(s) * 1.2f;
        constexpr f32 kFrom = -0.28f * kPi;
        constexpr f32 kTo = 1.28f * kPi;
        dl->PathArcTo(c, r, kFrom, kTo);
        dl->PathStroke(col, ImDrawFlags_None, th);

        // Tangent at kTo, in the direction the arc was drawn, and its normal.
        // The head's base sits ON the arc's last point so the two read as one
        // stroke; longer than it is wide, or it looks like a blob.
        const ImVec2 end(c.x + r * std::cos(kTo), c.y + r * std::sin(kTo));
        const ImVec2 dir(-std::sin(kTo), std::cos(kTo));
        const ImVec2 nrm(-dir.y, dir.x);
        const f32 len = th * 2.3f;
        const f32 wide = th * 1.15f;
        dl->AddTriangleFilled(ImVec2(end.x + dir.x * len, end.y + dir.y * len),
                              ImVec2(end.x + nrm.x * wide, end.y + nrm.y * wide),
                              ImVec2(end.x - nrm.x * wide, end.y - nrm.y * wide), col);
        break;
    }
    case Icon::Tracks: {
        // Three stacked bars of unequal length — the track list a timeline puts
        // beside its transport, which is what these open.
        const f32 th = Stroke(s) * 1.35f;
        const f32 x0 = c.x - s * kWide;
        const f32 w[3] = {s * kWide * 2.0f, s * kWide * 1.15f, s * kWide * 1.6f};
        for (i32 i = 0; i < 3; ++i) {
            const f32 y = c.y + (static_cast<f32>(i) - 1.0f) * s * 0.21f;
            dl->AddRectFilled(ImVec2(x0, y - th * 0.5f), ImVec2(x0 + w[i], y + th * 0.5f), col, th * 0.5f);
        }
        break;
    }
    default:
        break;
    }
}

/// The shared body of RailTile and RailTab: an invisible button at the caller's
/// size, painted by hand, with the icon over the label.
bool RailCell(const char* id, Icon icon, const char* label, ImVec2 size, bool selected, bool tile,
              const IconImage& image) {
    const ImVec2 p(s_rail.x, s_rail.y);
    s_rail.y += size.y;
    ImGui::SetCursorScreenPos(p);
    const bool clicked = ImGui::InvisibleButton(id, size);
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 hi(p.x + size.x, p.y + size.y);
    const f32 inset = std::floor(size.x * 0.08f);
    ImU32 fg = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    ImU32 iconCol = fg;

    if (tile) {
        // The application tile is the accent colour taken well down towards
        // black, shaded over its height, under a bar of the accent at full
        // strength: a deep panel rather than a bright slab, because what sits on
        // it is the host's own icon and those are drawn for a dark ground.
        const ImVec4 accent = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
        const f32 lift = held ? 0.22f : (hovered ? 0.46f : 0.32f);
        const auto shade = [&](f32 k) {
            return ImGui::ColorConvertFloat4ToU32(ImVec4(accent.x * k, accent.y * k, accent.z * k, accent.w));
        };
        const ImU32 top = shade(lift);
        const ImU32 bottom = shade(lift * 0.5f);
        dl->AddRectFilledMultiColor(p, hi, top, top, bottom, bottom);
        const f32 bar = std::max(2.0f, std::floor(size.x * 0.03f));
        dl->AddRectFilled(ImVec2(p.x, hi.y - bar), hi, ImGui::GetColorU32(ImGuiCol_ButtonActive));
        fg = IM_COL32_WHITE;
        iconCol = fg;
    } else if (selected) {
        // Flush to the content area rather than inset: a mode tab is the mouth of
        // the panel it opens, not a button that happens to be lit.
        dl->AddRectFilled(p, hi, ImGui::GetColorU32(ImGuiCol_WindowBg));
        // The accent, held off the cell's own edges and rounded at both ends — run
        // the full height it reads as a border between two panes instead.
        const f32 bar = std::max(2.0f, std::floor(size.x * 0.04f));
        dl->AddRectFilled(ImVec2(p.x, p.y + inset), ImVec2(p.x + bar, hi.y - inset),
                          ImGui::GetColorU32(ImGuiCol_ButtonActive), bar * 0.5f);
        fg = ImGui::GetColorU32(ImGuiCol_Text);
        // Icon in the accent, label in plain text: two levels, so the tab still
        // says which mode it is with the rail glanced at rather than read.
        iconCol = ImGui::GetColorU32(ImGuiCol_ButtonActive);
    } else if (hovered || held) {
        dl->AddRectFilled(
            ImVec2(p.x + inset, p.y + inset), ImVec2(hi.x - inset, hi.y - inset),
            ImGui::GetColorU32(held ? ImGuiCol_ButtonActive : ImGuiCol_ButtonHovered, held ? 0.55f : 0.28f),
            std::floor(size.x * 0.10f));
        fg = ImGui::GetColorU32(ImGuiCol_Text);
        iconCol = fg;
    }

    // Icon and label are one block, centred in the cell. Hung off the bottom edge
    // instead, the glyph drifts with whatever height the cell happens to have —
    // and the tile is twice a tab's.
    const f32 textH = ImGui::GetTextLineHeight();
    const f32 iconS = std::floor(std::min(size.x, size.y - textH) * (tile ? 0.62f : 0.60f));
    const f32 gap = std::floor(textH * 0.35f);
    const f32 top = p.y + std::floor((size.y - iconS - gap - textH) * 0.5f);
    const f32 cx = std::floor((p.x + hi.x - iconS) * 0.5f);
    if (image.valid)
        dl->AddImage(image.tex, ImVec2(cx, top), ImVec2(cx + iconS, top + iconS), image.uv0, image.uv1);
    else
        DrawIcon(dl, icon, ImVec2(cx + iconS * 0.5f, top + iconS * 0.5f), iconS, iconCol);
    const f32 textW = ImGui::CalcTextSize(label).x;
    dl->AddText(ImVec2(std::floor((p.x + hi.x - textW) * 0.5f), top + iconS + gap), fg, label);
    return clicked;
}

} // namespace

void DrawIcon(ImDrawList* dl, Icon icon, ImVec2 centre, f32 size, ImU32 col) {
    switch (icon) {
    case Icon::None:
        break;
    case Icon::Logo:
        DrawSnowflake(dl, centre, size, col);
        break;
    case Icon::File:
        DrawPage(dl, centre, size, col);
        break;
    case Icon::Open:
        DrawFolder(dl, centre, size, col);
        break;
    case Icon::Save:
        DrawFloppy(dl, centre, size, col);
        break;
    case Icon::Export:
        DrawTray(dl, centre, size, col);
        break;
    case Icon::Frames:
        DrawFrames(dl, centre, size, col);
        break;
    case Icon::Settings:
        DrawGear(dl, centre, size, col);
        break;
    case Icon::Storage:
        DrawStack(dl, centre, size, col);
        break;
    case Icon::Person:
        DrawPerson(dl, centre, size, col);
        break;
    case Icon::Cube:
        DrawCube(dl, centre, size, col);
        break;
    default:
        DrawTransport(dl, icon, centre, size, col);
        break;
    }
}

PopupAnchor ItemPopupAnchor() {
    return PopupAnchor{ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
}

void SetNextPopupUnder(const PopupAnchor& anchor) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const bool flip = anchor.min.x > vp->WorkPos.x + vp->WorkSize.x * 0.5f;
    ImGui::SetNextWindowPos(ImVec2(flip ? anchor.max.x : anchor.min.x, anchor.max.y), ImGuiCond_Always,
                            ImVec2(flip ? 1.0f : 0.0f, 0.0f));
}

RibbonLayout RibbonMetrics() {
    const f32 fh = ImGui::GetFrameHeight();
    RibbonLayout l{};
    l.menuH = fh;
    // Three small rows, which is what a ribbon group is: the tallest stack
    // BeginRibbonRows has to fit, and the height every large button takes.
    l.contentH = std::floor(3.0f * fh + 2.0f * ImGui::GetStyle().ItemSpacing.y);
    l.captionH = std::floor(ImGui::GetTextLineHeight() + fh * 0.2f);
    l.bodyH = l.contentH + l.captionH + std::floor(fh * 0.5f);
    l.topH = l.menuH + l.bodyH;
    l.railW = std::floor(fh * 4.4f);
    // Taller than a mode tab, because it is the application button and not one
    // more mode — but well short of topH, which squared the corner off at the
    // price of a block of solid colour the size of the ribbon beside it.
    l.tileH = std::floor(fh * 3.4f);
    l.tabH = std::floor(fh * 2.9f);
    return l;
}

// ---- The rail ----

bool BeginRibbonRail() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const RibbonLayout l = RibbonMetrics();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(ImVec2(l.railW, vp->WorkSize.y));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetColorU32(ImGuiCol_MenuBarBg));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
    const bool open = ImGui::Begin("##ribbonrail", nullptr, kFlags);
    // Begin reads the padding and paints the background, so the style goes back
    // immediately: a host popup opened from a rail cell is built inside this
    // window, and would otherwise inherit a chrome meant for the rail.
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);

    s_rail.x = vp->WorkPos.x;
    s_rail.y = vp->WorkPos.y;
    return open;
}

void EndRibbonRail() {
    // The edge the content area butts against, drawn last so no cell covers it.
    const ImVec2 p = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x + size.x - 1.0f, p.y),
                                        ImVec2(p.x + size.x - 1.0f, p.y + size.y),
                                        ImGui::GetColorU32(ImGuiCol_Border));
    ImGui::End();
}

bool RailTile(const char* id, Icon icon, const char* label, const IconImage& image) {
    const RibbonLayout l = RibbonMetrics();
    return RailCell(id, icon, label, ImVec2(l.railW, l.tileH), false, true, image);
}

bool RailTab(const char* id, Icon icon, const char* label, bool selected) {
    const RibbonLayout l = RibbonMetrics();
    return RailCell(id, icon, label, ImVec2(l.railW, l.tabH), selected, false, IconImage{});
}

// ---- The ribbon ----

bool BeginRibbon() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const RibbonLayout l = RibbonMetrics();
    const f32 pad = std::floor(ImGui::GetFrameHeight() * 0.25f);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + l.railW, vp->WorkPos.y));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x - l.railW, l.topH));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetColorU32(ImGuiCol_MenuBarBg));
    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_MenuBar;
    const bool open = ImGui::Begin("##ribbon", nullptr, kFlags);
    // As in BeginRibbonRail: the ribbon's own chrome must not reach the popups a
    // group's button opens.
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);

    s_ribbon = RibbonState{};
    s_ribbon.layout = l;
    s_ribbon.groupGap = std::floor(ImGui::GetFrameHeight() * 0.42f);
    if (open) {
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        s_ribbon.bodyOrigin = origin;
        s_ribbon.captionY = origin.y + l.contentH;
        s_ribbon.bodyRight = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - pad;
    }
    return open;
}

void EndRibbon() {
    // Every group ends by placing the cursor where the next one starts, so the
    // last one leaves a SetCursorScreenPos with no item after it — which ImGui
    // reports as an attempt to grow the window by moving the cursor. A zero-size
    // item is the acknowledgement it asks for.
    ImGui::Dummy(ImVec2(0.0f, 0.0f));

    const ImVec2 p = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + size.y - 1.0f),
                                        ImVec2(p.x + size.x, p.y + size.y - 1.0f),
                                        ImGui::GetColorU32(ImGuiCol_Border));
    ImGui::End();
}

bool BeginRibbonMenus() {
    return ImGui::BeginMenuBar();
}

void EndRibbonMenus() {
    ImGui::EndMenuBar();
}

void RibbonMenuStatus(const char* text) {
    const f32 avail = ImGui::GetWindowWidth() - ImGui::GetCursorPosX() - ImGui::GetStyle().FramePadding.x * 2.0f;
    if (avail <= 0.0f)
        return; // the menus already fill the row

    // Trimmed from the FRONT: this is a path, and its tail — the file — is the
    // part worth reading. ASCII dots rather than an ellipsis, which the
    // Latin-only fallback atlas draws as a missing-glyph box. The scan steps
    // over UTF-8 continuation bytes so a trim cannot split a character.
    const char* shown = text;
    std::string elided;
    if (ImGui::CalcTextSize(shown).x > avail) {
        const char* end = text + std::strlen(text);
        const char* cut = text;
        while (cut < end) {
            do {
                ++cut;
            } while (cut < end && (static_cast<u8>(*cut) & 0xC0u) == 0x80u);
            elided = std::string("...") + cut;
            if (ImGui::CalcTextSize(elided.c_str()).x <= avail)
                break;
        }
        if (cut >= end)
            return; // not even the dots fit
        shown = elided.c_str();
    }
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - ImGui::CalcTextSize(shown).x -
                         ImGui::GetStyle().FramePadding.x * 2.0f);
    ImGui::TextDisabled("%s", shown);
}

namespace {

/// The caption under a group and the rule that closes it. Both go on the draw
/// list: neither may move the layout cursor, which has to land at the next
/// group's origin, which is what the last line does.
void GroupTail(const char* caption, f32 left, f32 right) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (caption) {
        const f32 captionW = ImGui::CalcTextSize(caption).x;
        dl->AddText(ImVec2((left + right - captionW) * 0.5f, s_ribbon.captionY),
                    ImGui::GetColorU32(ImGuiCol_TextDisabled), caption);
    }

    const f32 sepX = std::floor(right + s_ribbon.groupGap) + 0.5f;
    const f32 top = s_ribbon.bodyOrigin.y;
    const f32 bottom = s_ribbon.captionY + s_ribbon.layout.captionH;
    dl->AddLine(ImVec2(sepX, top + s_ribbon.layout.contentH * 0.1f), ImVec2(sepX, bottom),
                ImGui::GetColorU32(ImGuiCol_Separator));
    ImGui::SetCursorScreenPos(ImVec2(sepX + s_ribbon.groupGap, top));
}

/// The group as a drop-down: one large button wearing the group's icon and
/// caption, and the body inside the popup it opens. The caption band under it
/// carries a chevron instead of the caption, which the button already says.
void CollapsedGroup(const char* caption, Icon icon, const std::function<void()>& body) {
    const f32 left = ImGui::GetCursorScreenPos().x;
    const bool clicked = RibbonButton("##drop", icon, caption);
    const PopupAnchor anchor = ItemPopupAnchor();
    const f32 right = anchor.max.x;
    if (clicked)
        ImGui::OpenPopup("##groupdrop");

    SetNextPopupUnder(anchor);
    if (ImGui::BeginPopup("##groupdrop")) {
        // The body lays itself out against the body origin — the rows centre on
        // it, the large buttons hang from it — so for the length of the popup
        // that origin is the popup.
        const ImVec2 saved = s_ribbon.bodyOrigin;
        s_ribbon.bodyOrigin = ImGui::GetCursorScreenPos();
        body();
        s_ribbon.bodyOrigin = saved;
        ImGui::EndPopup();
    }

    const f32 cx = (left + right) * 0.5f;
    const f32 cy = s_ribbon.captionY + s_ribbon.layout.captionH * 0.35f;
    const f32 w = std::max(3.0f, s_ribbon.layout.captionH * 0.22f);
    ImGui::GetWindowDrawList()->AddTriangleFilled(ImVec2(cx - w, cy - w * 0.6f), ImVec2(cx + w, cy - w * 0.6f),
                                                  ImVec2(cx, cy + w * 0.7f),
                                                  ImGui::GetColorU32(ImGuiCol_TextDisabled));
    GroupTail(nullptr, left, right);
}

} // namespace

void RibbonGroup(const char* caption, Icon icon, const std::function<void()>& body) {
    ImGui::PushID(caption);
    const ImGuiID id = ImGui::GetID("##group");
    const f32 left = ImGui::GetCursorScreenPos().x;
    const f32 measured = MeasuredWidth(id);
    // An unmeasured group draws inline and is measured by doing so; a measured
    // one that would run past the edge, and everything after it, collapses.
    if (s_ribbon.collapsing || (measured > 0.0f && left + measured > s_ribbon.bodyRight)) {
        s_ribbon.collapsing = true;
        CollapsedGroup(caption, icon, body);
        ImGui::PopID();
        return;
    }

    ImGui::BeginGroup();
    body();
    ImGui::EndGroup();
    const f32 right = ImGui::GetItemRectMax().x;
    RecordWidth(id, right - left);
    GroupTail(caption, ImGui::GetItemRectMin().x, right);
    ImGui::PopID();
}

bool RibbonButton(const char* id, Icon icon, const char* label, bool enabled, bool active) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const f32 h = s_ribbon.layout.contentH;
    const f32 textW = ImGui::CalcTextSize(label).x;
    const f32 w = std::max(h, textW + style.FramePadding.x * 4.0f);
    // Hung from the top of the body whatever the item before it did to the
    // cursor, so a group can mix large buttons with a centred stack of rows.
    const ImVec2 p(ImGui::GetCursorScreenPos().x, s_ribbon.bodyOrigin.y);
    ImGui::SetCursorScreenPos(p);

    ImGui::BeginDisabled(!enabled);
    // The ribbon's own background when at rest, so a row of buttons reads as one
    // surface and only the one under the pointer lifts out of it.
    ImGui::PushStyleColor(ImGuiCol_Button, active ? ImGui::GetColorU32(ImGuiCol_ButtonActive)
                                                  : ImGui::GetColorU32(ImGuiCol_MenuBarBg));
    const bool clicked = ImGui::Button(id, ImVec2(w, h));
    ImGui::PopStyleColor();
    ImGui::EndDisabled();

    // BeginDisabled dims items through style.Alpha, which the draw list does not
    // honour, so the colour carries it instead.
    const ImU32 fg = ImGui::GetColorU32(enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const f32 textH = ImGui::GetTextLineHeight();
    // Capped rather than proportional: a group three rows tall would otherwise
    // give the glyph half the button, which reads as an illustration.
    const f32 iconS = std::floor(std::min((h - textH) * 0.70f, ImGui::GetFrameHeight() * 1.38f));
    // Icon and label are one block, centred in the button. Pinning the label to
    // the bottom edge leaves the glyph adrift in whatever space is left over, and
    // presses the label into the group caption underneath.
    const f32 gap = std::floor(style.ItemInnerSpacing.y * 0.5f);
    const f32 top = p.y + std::floor((h - iconS - gap - textH) * 0.5f);
    DrawIcon(dl, icon, ImVec2(std::floor(p.x + w * 0.5f), top + iconS * 0.5f), iconS, fg);
    dl->AddText(ImVec2(std::floor(p.x + (w - textW) * 0.5f), top + iconS + gap), fg, label);
    ImGui::SameLine();
    return clicked;
}

void BeginRibbonRows(i32 rows, f32 labelWidth) {
    const f32 fh = ImGui::GetFrameHeight();
    const f32 n = static_cast<f32>(std::max(1, rows));
    const f32 used = n * fh + (n - 1.0f) * ImGui::GetStyle().ItemSpacing.y;
    // Centred against the large buttons beside them rather than hung from the
    // top, which is what makes a mixed group read as one row of controls.
    ImGui::SetCursorScreenPos(
        ImVec2(ImGui::GetCursorScreenPos().x,
               s_ribbon.bodyOrigin.y + std::max(0.0f, std::floor((s_ribbon.layout.contentH - used) * 0.5f))));
    s_ribbon.rowLabelW = labelWidth;
    ImGui::BeginGroup();
}

void RibbonRow(const char* label) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(s_ribbon.rowLabelW);
}

void EndRibbonRows() {
    ImGui::EndGroup();
    ImGui::SameLine();
}

} // namespace whiteout::flakes::ui
