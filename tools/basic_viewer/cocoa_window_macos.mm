// macOS window chrome — Cocoa equivalent of the Windows DwmSetWindow-
// Attribute path in viewer_app.cpp, kept native to macOS conventions:
//
//   • NSAppearanceNameDarkAqua forces dark traffic-light pucks and dark
//     control rendering (mirrors DWMWA_USE_IMMERSIVE_DARK_MODE on Win11).
//   • The title bar stays in its own region above the contentView, so the
//     red/yellow/green buttons never land on top of the ImGui menu strip
//     — the menu sits naturally below the title bar.
//   • The contentView background is painted with the ImGui MenuBarBg
//     colour so any gap between the menu and the title bar (e.g. during
//     resize live-redraw) blends instead of flashing the default window
//     fill.
//
// The (r, g, b) argument is plumbed through from the same RGB triple the
// Windows DWM call uses (see tools/common/imgui_theme.cpp for the source).

#import <Cocoa/Cocoa.h>

namespace whiteout::flakes {

void SetCocoaWindowChrome(void* nsWindow, float r, float g, float b) {
    if (!nsWindow)
        return;
    NSWindow* window = static_cast<NSWindow*>(nsWindow);

    // Dark traffic-light pucks + dark default control rendering. Equivalent
    // to DWMWA_USE_IMMERSIVE_DARK_MODE on Windows 10/11.
    window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];

    // Leave the title bar as a separate region above the contentView —
    // i.e. clear NSWindowStyleMaskFullSizeContentView in case some caller
    // (or a future GLFW version) flipped it on. AppKit then renders the
    // standard dark-mode title bar with the traffic lights, and the ImGui
    // menu bar starts at y=0 of the contentView, immediately below it.
    window.styleMask &= ~NSWindowStyleMaskFullSizeContentView;
    window.titlebarAppearsTransparent = NO;

    window.backgroundColor = [NSColor colorWithSRGBRed:r green:g blue:b alpha:1.0];
}

} // namespace whiteout::flakes
