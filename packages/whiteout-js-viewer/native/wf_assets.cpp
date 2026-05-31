// AssetManager bridge. Kinds: 0=Texture, 1=Particle, 2=ChildModel.
// _needs_count snapshots, then _get/_apply walk the snapshot.

#include "wf_web_internal.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

using wf_web::WfRenderer;

extern "C" {

int wf_assets_needs_count(WfRenderer* h) {
    if (!h) return 0;
    h->lastNeeds.clear();
    h->renderer.Assets().DrainNeeds(
        [&](whiteout::flakes::AssetsView::Kind k, std::string_view path) {
            h->lastNeeds.push_back({static_cast<int>(k), std::string(path)});
        });
    return static_cast<int>(h->lastNeeds.size());
}

int wf_assets_needs_get_kind(WfRenderer* h, int index) {
    if (!h) return -1;
    if (index < 0 || index >= static_cast<int>(h->lastNeeds.size())) return -1;
    return h->lastNeeds[index].kind;
}

int wf_assets_needs_get_path(WfRenderer* h, int index, char* outBuf, int bufCap) {
    if (!h || !outBuf || bufCap <= 0) return 0;
    if (index < 0 || index >= static_cast<int>(h->lastNeeds.size())) return 0;
    const std::string& s = h->lastNeeds[index].path;
    const int n = static_cast<int>(std::min<std::size_t>(
        s.size(), static_cast<std::size_t>(bufCap - 1)));
    std::memcpy(outBuf, s.data(), n);
    outBuf[n] = '\0';
    return n;
}

int wf_assets_apply(WfRenderer* h, int kind, const char* path,
                    const void* bytes, int len, const char* foundExt) {
    if (!h || !path || !bytes || len <= 0) return 0;
    if (kind < 0 || kind > 2) return 0;
    const auto k = static_cast<whiteout::flakes::AssetsView::Kind>(kind);
    std::span<const std::uint8_t> span(
        static_cast<const std::uint8_t*>(bytes), static_cast<std::size_t>(len));
    return h->renderer.Assets().ApplyAsset(
        k, std::string_view(path), span,
        foundExt ? std::string_view(foundExt) : std::string_view{}) ? 1 : 0;
}

// Live WebGPU bytes (excludes deferred-delete). double avoids i64 marshaling.
double wf_gpu_bytes(WfRenderer* h) {
    if (!h) return 0.0;
    return static_cast<double>(h->renderer.Pipeline().LiveGpuBytes());
}

int wf_assets_stat(WfRenderer* h, int which) {
    if (!h) return 0;
    auto s = h->renderer.Assets().GetStats();
    switch (which) {
        case 0: return static_cast<int>(s.liveSlots);
        case 1: return static_cast<int>(s.loadedSlots);
        case 2: return static_cast<int>(s.pendingNeeds);
        case 3: return static_cast<int>(s.totalAcquires);
        case 4: return static_cast<int>(s.totalReleases);
        case 5: return static_cast<int>(s.totalApplies);
        case 6: return static_cast<int>(s.totalApplyMisses);
        default: return 0;
    }
}

} // extern "C"
