#include <cornflakes/asset/text_pkfx_reader.hpp>
#include <cornflakes/core/determinism.hpp>
#include <cornflakes/diagnostics/issue_codes.hpp>
#include <cornflakes/interface/asset/asset_reader.hpp>
#include <cornflakes/interface/asset/pkb_reader.hpp>

#include <algorithm>
#include <memory>
#include <string_view>

namespace whiteout::cornflakes {

namespace {

Issue assetFatal(u32 code, std::string_view message) noexcept {
    Issue issue;
    issue.severity = Severity::Fatal;
    issue.category = Category::Asset;
    issue.code = code;
    issue.message = message;
    return issue;
}

} // namespace

SerializerPriorityDispatcher::SerializerPriorityDispatcher() {
    addReader(std::make_unique<PkbReader>());
    addReader(std::make_unique<TextPkfxReader>());
}

void SerializerPriorityDispatcher::addReader(std::unique_ptr<IAssetReader> reader) {

    auto it = std::upper_bound(
        m_readers.begin(), m_readers.end(), reader,
        [](const std::unique_ptr<IAssetReader>& a, const std::unique_ptr<IAssetReader>& b) {
            return a->priority() > b->priority();
        });
    m_readers.insert(it, std::move(reader));
}

std::optional<EffectAssetModel> SerializerPriorityDispatcher::read(const BakedSource& src,
                                                                   IArena& arena,
                                                                   IssueBag& issues) {
    for (const auto& reader : m_readers) {
        if (reader->canHandle(src)) {
            return reader->read(src, arena, issues);
        }
    }
    issues.push(assetFatal(issues::asset::kNoReaderMatched,
                           "no registered asset reader accepted the source"));
    return std::nullopt;
}

} // namespace whiteout::cornflakes
