#pragma once

// ============================================================================
// Where the games are installed.
//
// Discovery runs once and never changes: a scan of the Blizzard product
// registry, mapped onto the ProductIds the renderer uses. Kept apart from the
// provider because "where is StarCraft II" is a question worth answering
// without opening anything.
// ============================================================================

#include "whiteout/flakes/enums.h" // ProductId

#include <string>

namespace whiteout::flakes::io {

class InstallLocator {
public:
    // Runs the scan. Prints what it found, once, as the provider always has.
    InstallLocator();

    // Root for @p game, or empty when it was not found. Heroes of the Storm is
    // reached through @ref Hots — it shares ProductId::Sc2 with StarCraft II
    // and both can be installed at once.
    const std::string& PathFor(ProductId game) const;
    const std::string& Wc3() const {
        return wc3_;
    }
    const std::string& Wow() const {
        return wow_;
    }
    const std::string& Sc2() const {
        return sc2_;
    }
    const std::string& Hots() const {
        return hots_;
    }

private:
    std::string wc3_;
    std::string wow_;
    std::string sc2_;
    std::string hots_;
};

} // namespace whiteout::flakes::io
