#pragma once

// ============================================================================
// The open documents (tabs) and which one is active. Each owns a RenderService
// scene; the active one is what the frame loop ticks and renders, the rest stay
// resident and frozen so switching back is instant.
// ============================================================================

#include "documents/document.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace whiteout::flakes::io {
class IContentProvider;
}
namespace whiteout::flakes::renderer {
class RenderService;
}

namespace whiteout::flakes {

class DocumentManager {
public:
    explicit DocumentManager(renderer::RenderService& service);

    i32 Count() const {
        return static_cast<i32>(documents_.size());
    }
    /// -1 when no tab is open.
    i32 ActiveIndex() const {
        return active_;
    }
    Document* Active();
    const Document* Active() const;
    /// The active document's state, or the no-document state when nothing is
    /// open (reset when the last tab closes).
    DocumentState& ActiveState();
    const DocumentState& ActiveState() const;
    /// The active document's scene, or the default scene when none is open.
    renderer::SceneId ActiveScene() const;
    /// Tab label for @p index; empty when out of range.
    const std::string& Title(i32 index) const;

    /// Create a scene bound to @p provider, register a document for it and make
    /// it active, then run @p load against it. On failure the scene is destroyed
    /// and the previous tab is active again — the only rollback path.
    bool Open(std::shared_ptr<io::IContentProvider> provider, std::string title,
              const std::function<bool(Document&)>& load);
    /// Make @p index active and publish its scene. False when it already was, or
    /// is out of range.
    bool Activate(i32 index);
    /// Destroy @p index's scene. When it was active a neighbour takes over, and
    /// true says so (its render mode wants re-applying).
    bool Close(i32 index);

    /// After an app-driven change of the active tab (an open, a close handing
    /// focus to a neighbour) the tab bar must select it, or it snaps back to its
    /// first tab. The index once, then nothing.
    std::optional<i32> ConsumePendingTabSelect();

private:
    renderer::RenderService& service_;
    std::vector<Document> documents_;
    i32 active_ = -1;
    std::optional<i32> pendingTabSelect_;
    DocumentState empty_;
};

} // namespace whiteout::flakes
