#pragma once

// ============================================================================
// A ribbon for Dear ImGui: the Office-style top strip — a slim menu row, a body
// of captioned groups of large icon buttons, and a rail of mode tabs down the
// left edge with the application button at its head.
//
// ImGui ships no such widget and no third-party one is maintained (the wiki's
// extension list has toolbars, not ribbons), so this is the extension: plain
// ImGui calls plus a draw list, no new render state, nothing the hosts do not
// already link.
//
// Every size derives from ImGui::GetFrameHeight(), so the ribbon follows the
// host's DPI scale the way the rest of the UI does. The icons are drawn rather
// than typed for the same reason WhiteoutFlakes' old toolbar drew its transport
// glyphs: the hosts bake ONE static font atlas, so an icon font would be a
// second TTF merged into it and still missing on the Roboto fallback.
//
// One ribbon per frame: the group/caption geometry lives in file-static state
// that BeginRibbon resets.
//
// Shape of a frame:
//
//     if (ui::BeginRibbonRail()) {                  // the left column
//         ui::RailTile("##file", Icon::Logo, "File");   // or its own icon
//         ui::RailTab("##preview", Icon::Cube, "Preview", true);
//     }
//     ui::EndRibbonRail();
//
//     if (ui::BeginRibbon()) {                      // the strip to its right
//         if (ui::BeginRibbonMenus()) { ...menus...; ui::EndRibbonMenus(); }
//         ui::BeginRibbonGroup("Document");
//         ui::RibbonButton("##open", Icon::Open, "Open");
//         ui::EndRibbonGroup();
//     }
//     ui::EndRibbon();
// ============================================================================

#include "whiteout/flakes/types.h"

#include <imgui.h>

#include <functional>

namespace whiteout::flakes::ui {

// ---- Icons ----

/// The ribbon's glyph set. Vector shapes, scaled to the button asking for them
/// and drawn in the theme's text colour unless a caller overrides it.
enum class Icon {
    None,
    Logo,   ///< The host's own mark, for the application tile: a snowflake.
    File,   ///< A page with a folded corner.
    Open,   ///< A folder.
    Save,   ///< A floppy disk, still what "save" looks like.
    Export, ///< An arrow leaving a tray.
    Frames, ///< Two offset cards: a sequence of images.
    Play,
    Pause,
    Restart,
    Tracks,   ///< Stacked bars: a timeline's track list.
    Settings, ///< A gear.
    Storage,  ///< A cylinder: an archive to browse.
    Person,   ///< A head and shoulders: what a character is wearing.
    Cube,     ///< A wireframe box: the 3D view.
};

/// @p size is the box the glyph is drawn to fit, centred on @p centre.
void DrawIcon(ImDrawList* dl, Icon icon, ImVec2 centre, f32 size, ImU32 col);

/// A real image to draw where a glyph would go — a host's own icon, which no
/// drawn approximation of it beats. Left invalid, the glyph is drawn instead.
/// Anything ImGui can sample will do; the cheapest is a custom rectangle in the
/// font atlas (imgui_app_icon.h), whose UVs move when the atlas is rebuilt, so
/// fill this in per frame rather than holding on to it.
struct IconImage {
    ImTextureRef tex{};
    ImVec2 uv0{};
    ImVec2 uv1{};
    bool valid = false;
};

// ---- Popups hung off a ribbon item ----

/// Where a popup opened from a ribbon button goes. The default — at the pointer
/// — puts it nowhere near the button that owns it, and a headless UI capture has
/// no pointer at all.
///
/// Read with ItemPopupAnchor right after the button, applied with
/// SetNextPopupUnder right before BeginPopup, and never in between: a tooltip
/// submitted between the two would both overwrite the last item and eat the
/// pending window position.
struct PopupAnchor {
    ImVec2 min;
    ImVec2 max;
};
PopupAnchor ItemPopupAnchor();
/// Under the anchor, or hung from its right edge when there is no room to its
/// right — SetNextWindowPos opts out of ImGui's own clamping, so the flip is
/// this function's job.
void SetNextPopupUnder(const PopupAnchor& anchor);

// ---- Layout ----

/// Where the ribbon puts things, in pixels at the current frame height. A host
/// needs these to place whatever it draws under or beside the ribbon.
struct RibbonLayout {
    f32 menuH;    ///< The slim menu row at the top of the ribbon.
    f32 contentH; ///< A group's controls: three small rows, or one large button.
    f32 captionH; ///< The group caption under them.
    f32 bodyH;    ///< The ribbon body: contentH + captionH + padding.
    f32 topH;     ///< menuH + bodyH — where the content area starts.
    f32 railW;    ///< The mode rail down the left edge.
    f32 tileH;    ///< The application tile at the head of that rail.
    f32 tabH;     ///< One mode tab on that rail.
};
RibbonLayout RibbonMetrics();

// ---- The rail: the application button, then the mode tabs ----

/// The left column, from the top of the viewport to its bottom. False when the
/// window is clipped; call EndRibbonRail either way.
bool BeginRibbonRail();
void EndRibbonRail();

/// The application button at the head of the rail, railW by tileH. It wears the
/// accent colour — the one solid block in the chrome — so what goes on it is the
/// host's own mark: @p image when the host has its icon to hand, and @p icon
/// drawn when it does not. Clicked opens the host's file menu.
bool RailTile(const char* id, Icon icon, const char* label, const IconImage& image = IconImage{});

/// One mode tab. @p selected wears the accent bar and the lit background.
bool RailTab(const char* id, Icon icon, const char* label, bool selected);

// ---- The ribbon: the menu row, then the groups ----

/// The strip right of the rail. False when clipped; call EndRibbon either way.
bool BeginRibbon();
void EndRibbon();

/// The slim menu row along the top of the ribbon. ImGui::BeginMenu inside it
/// lays out horizontally, exactly as in a main menu bar.
bool BeginRibbonMenus();
void EndRibbonMenus();
/// Right-aligned in the menu row, in the disabled colour: what the window is
/// showing. Call last, inside the menu row.
void RibbonMenuStatus(const char* text);

/// A captioned run of controls. The caption is centred under whatever @p body
/// drew and a rule follows it, so a group is as wide as its contents.
///
/// @p body is a callback rather than a Begin/End pair because the ribbon may
/// draw it somewhere else: when the row runs out of width the group collapses
/// to a drop-down wearing @p icon, and the body is called inside that instead.
/// Everything from the first group that does not fit collapses, so the row
/// never re-flows into a different order.
///
/// Width is measured as the group draws, so a group the ribbon has not seen
/// before is laid out inline and can overflow for exactly one frame.
void RibbonGroup(const char* caption, Icon icon, const std::function<void()>& body);

/// A large button: the icon over the label, the group's full height. @p active
/// holds it in its pressed colour (a button that toggles a window).
bool RibbonButton(const char* id, Icon icon, const char* label, bool enabled = true, bool active = false);

/// A stack of @p rows small controls inside a group, centred against the large
/// buttons beside them. @p labelWidth is the column the controls start at, so
/// they line up under each other. Three rows is what the body is sized for; a
/// fourth would run into the captions.
void BeginRibbonRows(i32 rows, f32 labelWidth);
/// Opens one such row: the caption, then the caller's widget on the same line.
void RibbonRow(const char* label);
void EndRibbonRows();

} // namespace whiteout::flakes::ui
