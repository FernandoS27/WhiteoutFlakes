#pragma once

// ============================================================================
// The strip of document tabs under the toolbar: one per open model or effect,
// each with a close button. Nothing when no document is open.
// ============================================================================

namespace whiteout::flakes {

class ViewerApp;

void BuildTabBar(ViewerApp& app);

} // namespace whiteout::flakes
