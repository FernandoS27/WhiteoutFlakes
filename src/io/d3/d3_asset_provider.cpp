#include "io/d3/d3_asset_provider.h"

namespace whiteout::flakes::io {

std::shared_ptr<const std::vector<::whiteout::u8>> D3AssetProvider::Read(::whiteout::i32 snoId) {
    // -1 is D3's "no reference" and reaches here constantly (an actor with no
    // physics, a sub-object with no cloth). Not an error and not worth a memo
    // entry of its own.
    if (snoId <= 0 || !provider_)
        return nullptr;

    if (auto it = memo_.find(snoId); it != memo_.end()) {
        ++memoHits_;
        return it->second; // may be null: a remembered miss
    }
    ++reads_;
    auto bytes = provider_->ReadFile(ContentRef::FromFileId(static_cast<u32>(snoId)));
    std::shared_ptr<const std::vector<::whiteout::u8>> held;
    if (bytes && !bytes->empty())
        held = std::make_shared<const std::vector<::whiteout::u8>>(std::move(*bytes));
    memo_.emplace(snoId, held);
    return held;
}

std::vector<::whiteout::u8> D3AssetProvider::load(d3n::Group, ::whiteout::i32 snoId) {
    auto held = Read(snoId);
    return held ? *held : std::vector<::whiteout::u8>{};
}

} // namespace whiteout::flakes::io
