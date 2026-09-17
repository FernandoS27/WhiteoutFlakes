#pragma once

// ============================================================================
// File ▸ Open, and the profile picker a `.wem` or glTF open goes through.
//
// A `.wem` does not open on the strength of its name: the profile it is opened
// AS decides the materials, the render path and the game whose storage its
// textures resolve against, and only the file can say what it offers. Not a
// question a default can answer — a document carrying two material sets over
// one geometry has two right answers, and the file does not say which the user
// meant.
// ============================================================================

#include "io/wem/wem_profiles.h"
#include "whiteout/flakes/types.h"

#include <filesystem>
#include <memory>
#include <vector>

namespace whiteout::flakes::io {
struct WemDocument;
}

namespace whiteout::flakes {

struct UiContext;

class OpenDialog {
public:
    explicit OpenDialog(UiContext& ctx);

    /// The native picker, then the open: through the profile picker for an
    /// interchange file, straight to the loader otherwise.
    void PickAndOpen();
    /// The profile picker modal; draws nothing while no document waits on it.
    void Build();

    /// The `--ui-shot` harness: the picker for the active document, which must
    /// be one. The first row selected, as the recording had it.
    bool OpenForShot();

private:
    void Ask(const std::filesystem::path& path, std::shared_ptr<io::WemDocument> document);
    void Clear();

    UiContext& ctx_;
    // The document waiting on a profile, where it came from, the rows it offers
    // and which one is selected. All cleared when the picker closes, either way.
    std::shared_ptr<io::WemDocument> document_;
    std::filesystem::path path_;
    std::vector<io::WemProfileOption> options_;
    i32 selection_ = 0;
    bool openRequested_ = false;
};

} // namespace whiteout::flakes
