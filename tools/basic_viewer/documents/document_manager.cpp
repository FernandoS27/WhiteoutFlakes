#include "documents/document_manager.h"

#include "renderer/render_service.h"
#include "renderer/scene_manager.h"

#include <algorithm>
#include <utility>

namespace whiteout::flakes {

DocumentManager::DocumentManager(renderer::RenderService& service) : service_(service) {}

Document* DocumentManager::Active() {
    return (active_ >= 0 && active_ < Count()) ? &documents_[static_cast<usize>(active_)] : nullptr;
}

const Document* DocumentManager::Active() const {
    return (active_ >= 0 && active_ < Count()) ? &documents_[static_cast<usize>(active_)] : nullptr;
}

DocumentState& DocumentManager::ActiveState() {
    if (Document* doc = Active())
        return doc->state;
    return empty_;
}

const DocumentState& DocumentManager::ActiveState() const {
    if (const Document* doc = Active())
        return doc->state;
    return empty_;
}

renderer::SceneId DocumentManager::ActiveScene() const {
    if (const Document* doc = Active())
        return doc->scene;
    return service_.DefaultSceneId();
}

const std::string& DocumentManager::Title(i32 index) const {
    static const std::string kEmpty;
    if (index < 0 || index >= Count())
        return kEmpty;
    return documents_[static_cast<usize>(index)].title;
}

bool DocumentManager::Open(std::shared_ptr<io::IContentProvider> provider, std::string title,
                           const std::function<bool(Document&)>& load) {
    const i32 previous = active_;

    // Every document owns its own scene, bound to the caller's provider.
    const renderer::SceneId sid = service_.CreateScene();
    service_.SceneAt(sid).SetContentProvider(std::move(provider));
    service_.SetActiveScene(sid);

    Document& doc = documents_.emplace_back();
    doc.scene = sid;
    doc.title = std::move(title);
    active_ = Count() - 1;
    if (!load(doc)) {
        // The candidate never becomes a tab: its scene goes and the previous
        // document is active again.
        service_.DestroyScene(sid);
        documents_.pop_back();
        active_ = previous;
        if (active_ < 0)
            empty_ = DocumentState{};
        service_.SetActiveScene(ActiveScene());
        return false;
    }
    pendingTabSelect_ = active_;
    return true;
}

bool DocumentManager::Activate(i32 index) {
    if (index < 0 || index >= Count() || index == active_)
        return false;
    active_ = index;
    service_.SetActiveScene(documents_[static_cast<usize>(index)].scene);
    return true;
}

bool DocumentManager::Close(i32 index) {
    if (index < 0 || index >= Count())
        return false;
    const bool closingActive = index == active_;
    // DestroyScene falls back to the default scene when this was the active one.
    service_.DestroyScene(documents_[static_cast<usize>(index)].scene);
    documents_.erase(documents_.begin() + index);

    if (documents_.empty()) {
        active_ = -1;
        empty_ = DocumentState{};
        service_.SetActiveScene(service_.DefaultSceneId());
        return false;
    }
    if (closingActive) {
        active_ = (std::min)(index, Count() - 1);
        service_.SetActiveScene(documents_[static_cast<usize>(active_)].scene);
        pendingTabSelect_ = active_;
        return true;
    }
    if (index < active_)
        --active_; // our slot shifted left
    return false;
}

std::optional<i32> DocumentManager::ConsumePendingTabSelect() {
    return std::exchange(pendingTabSelect_, std::nullopt);
}

} // namespace whiteout::flakes
