#pragma once

// ============================================================================
// What every panel of the viewer's UI is handed: the app whose subsystems it
// drives, and the frame's settings-changed flag.
//
// A widget that changes a persisted setting marks the flag; ViewerUI writes the
// settings file once at the end of the frame when it is set, rather than every
// widget saving on its own.
// ============================================================================

namespace whiteout::flakes {

class ViewerApp;

struct UiContext {
    ViewerApp& app;
    bool settingsDirty = false;

    void MarkSettingsDirty() {
        settingsDirty = true;
    }
};

} // namespace whiteout::flakes
