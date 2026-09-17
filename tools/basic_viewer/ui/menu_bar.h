#pragma once

// ============================================================================
// The main menu bar: File, View, Debug, Tools, Language and Settings.
// ============================================================================

namespace whiteout::flakes {

class ExportDialogs;
class ExportWindow;
class OpenDialog;
class SettingsWindow;
struct UiContext;

class MenuBar {
public:
    MenuBar(UiContext& ctx, OpenDialog& openDialog, ExportDialogs& exportDialogs, ExportWindow& exportWindow,
            SettingsWindow& settings);

    void Build();

    /// View ▸ View Cube.
    bool ShowViewCube() const {
        return showViewCube_;
    }
    /// Debug ▸ Log Console; the console's own close button writes it back.
    bool& ShowLogConsole() {
        return showLogConsole_;
    }

    /// The `--ui-shot` harness: hold the menu labelled by @p key open.
    void HoldMenuOpen(const char* key) {
        shotMenuKey_ = key;
    }

private:
    void BuildFileMenu();
    void BuildViewMenu();
    void BuildDebugMenu();
    void BuildPhysicsMenu();

    UiContext& ctx_;
    OpenDialog& openDialog_;
    ExportDialogs& exportDialogs_;
    ExportWindow& exportWindow_;
    SettingsWindow& settings_;
    bool showViewCube_ = true;
    bool showLogConsole_ = false;
    const char* shotMenuKey_ = nullptr;
};

} // namespace whiteout::flakes
