#include "texture_thumbnail_cache.h"

#include "renderer/model/model_source_utils.h"
#include "renderer/render_pipeline.h"

#include "whiteout/flakes/content_provider.h"

#include <algorithm>
#include <cstring>
#include <filesystem>

namespace whiteout::flakes::tools {

namespace {

// Name for the info line. The SOURCE format, read before the decode converts
// everything to RGBA8 — "BC3" is the useful thing to know about a `.dds`, and
// after the conversion every file would report RGBA8.
const char* FormatName(whiteout::textures::PixelFormat pf) {
    using PF = whiteout::textures::PixelFormat;
    switch (pf) {
    case PF::R8:      return "R8";
    case PF::R16:     return "R16";
    case PF::R32F:    return "R32F";
    case PF::RG8:     return "RG8";
    case PF::RG16:    return "RG16";
    case PF::RG32F:   return "RG32F";
    case PF::RGBA8:   return "RGBA8";
    case PF::RGBA16:  return "RGBA16";
    case PF::RGBA32F: return "RGBA32F";
    case PF::R16F:    return "R16F";
    case PF::RG16F:   return "RG16F";
    case PF::RGBA16F: return "RGBA16F";
    case PF::BC1:     return "BC1";
    case PF::BC2:     return "BC2";
    case PF::BC3:     return "BC3";
    case PF::BC4:     return "BC4";
    case PF::BC5:     return "BC5";
    case PF::BC6H:    return "BC6H";
    case PF::BC7:     return "BC7";
    }
    return "?";
}

// Box-downsample RGBA8 by an integer factor. Only ever runs on a texture whose
// file carried no mip small enough — a `.tga` or a single-level `.dds`. An
// integer box is enough: the result is a thumbnail, and a proper filter here
// would cost more than the decode it follows.
void BoxDownsample(std::vector<u8>& pixels, int& w, int& h, int factor) {
    if (factor <= 1)
        return;
    const int dw = std::max(1, w / factor);
    const int dh = std::max(1, h / factor);
    std::vector<u8> out(static_cast<std::size_t>(dw) * dh * 4);
    for (int y = 0; y < dh; ++y) {
        for (int x = 0; x < dw; ++x) {
            unsigned acc[4] = {0, 0, 0, 0};
            unsigned n = 0;
            for (int sy = y * factor; sy < std::min(h, (y + 1) * factor); ++sy) {
                for (int sx = x * factor; sx < std::min(w, (x + 1) * factor); ++sx) {
                    const u8* p = &pixels[(static_cast<std::size_t>(sy) * w + sx) * 4];
                    acc[0] += p[0];
                    acc[1] += p[1];
                    acc[2] += p[2];
                    acc[3] += p[3];
                    ++n;
                }
            }
            u8* d = &out[(static_cast<std::size_t>(y) * dw + x) * 4];
            for (int c = 0; c < 4; ++c)
                d[c] = static_cast<u8>(n ? acc[c] / n : 0);
        }
    }
    pixels.swap(out);
    w = dw;
    h = dh;
}

} // namespace

TextureThumbnailCache::TextureThumbnailCache(renderer::RenderService& svc,
                                             std::shared_ptr<io::IContentProvider> provider,
                                             int cap)
    : svc_(svc), provider_(std::move(provider)), cap_(std::max(1, cap)) {}

TextureThumbnailCache::~TextureThumbnailCache() {
    Clear();
}

void TextureThumbnailCache::BeginFrame(std::uint64_t) {
    // Requests that were queued but never decoded (the budget ran out and the
    // user scrolled past them) must not accumulate: the ones still on screen
    // are re-added by this frame's Acquire calls.
    pending_.clear();
}

void TextureThumbnailCache::SetCap(int cap) {
    cap_ = std::max(1, cap);
    Trim();
}

const TextureThumbnail& TextureThumbnailCache::Acquire(const std::string& archivePath,
                                                       std::uint64_t frameId) {
    Entry& e = entries_[archivePath];
    e.lastVisibleFrame = frameId;
    if (!e.queued)
        return e.info; // decoded, or decoded-and-failed; either way it is settled

    // Still waiting. Re-queue every frame it is asked for rather than only on
    // the first: BeginFrame drops the previous frame's queue, so a cell that
    // lost the budget race would otherwise sit on an empty entry forever.
    if (e.queuedFrame != frameId) {
        e.queuedFrame = frameId;
        pending_.push_back(archivePath);
    }
    return placeholder_;
}

void TextureThumbnailCache::EndFrame() {
    int budget = kDecodeBudget;
    for (const std::string& path : pending_) {
        if (budget <= 0)
            break;
        auto it = entries_.find(path);
        // Gone already — the entry was trimmed between the Acquire and here.
        if (it == entries_.end() || !it->second.queued)
            continue;
        Decode(path, it->second);
        it->second.queued = false;
        --budget;
    }
    pending_.clear();
    Trim();
}

void TextureThumbnailCache::Decode(const std::string& archivePath, Entry& entry) {
    TextureThumbnail& info = entry.info;
    info = TextureThumbnail{};

    if (!provider_) {
        info.error = "no content provider";
        return;
    }
    std::string actualExt;
    auto bytes = provider_->ReadFile(archivePath, &actualExt);
    if (!bytes || bytes->empty()) {
        info.error = "could not read the file";
        return;
    }

    // The name is usually enough, but a CASC read can resolve an entry whose
    // stored name has no extension, and the containers all lead with a magic.
    std::string ext = actualExt;
    if (ext.empty())
        ext = renderer::model::ExtensionLower(std::filesystem::path(archivePath));
    if (ext.empty())
        ext = renderer::model::SniffTextureExtension(*bytes);

    auto decoded = renderer::model::DispatchTextureParser(
        ext, [&](auto& parser) { return parser.parse(*bytes); });
    if (!decoded) {
        info.error = ext.empty() ? "unrecognised image format"
                                 : ("no parser for '" + ext + "' files");
        return;
    }

    info.format = FormatName(decoded->format());

    // Everything becomes RGBA8: a thumbnail is sampled once per frame at one
    // size, so keeping BC compression would save memory the cap already bounds
    // while forcing the pick-a-mip arithmetic below to respect block sizes.
    decoded->format(whiteout::textures::PixelFormat::RGBA8);

    const int fullW = static_cast<int>(decoded->width());
    const int fullH = static_cast<int>(decoded->height());
    if (fullW <= 0 || fullH <= 0) {
        info.error = "the image has no pixels";
        return;
    }
    info.width = fullW;
    info.height = fullH; // report the SOURCE size; the upload may be smaller

    // Prefer a mip the file already carries over downsampling ourselves.
    const int mips = static_cast<int>(std::max(1u, decoded->mipCount()));
    int level = 0;
    int w = fullW;
    int h = fullH;
    while (level + 1 < mips && std::max(w, h) > kMaxEdge) {
        ++level;
        w = std::max(1, fullW >> level);
        h = std::max(1, fullH >> level);
    }

    const auto mip = decoded->mipData(static_cast<u32>(level));
    const std::size_t want = static_cast<std::size_t>(w) * h * 4;
    if (mip.size() < want) {
        info.error = "truncated image data";
        return;
    }
    std::vector<u8> pixels(mip.begin(), mip.begin() + static_cast<std::ptrdiff_t>(want));

    // Only reached when the file had no small enough mip of its own.
    if (std::max(w, h) > kMaxEdge)
        BoxDownsample(pixels, w, h, (std::max(w, h) + kMaxEdge - 1) / kMaxEdge);

    for (std::size_t i = 3; i < pixels.size(); i += 4) {
        if (pixels[i] != 255) {
            info.hasAlpha = true;
            break;
        }
    }

    auto* gfx = svc_.Pipeline().Gfx();
    if (!gfx) {
        info.error = "no graphics device";
        return;
    }
    gfx::TextureDesc desc;
    desc.width = w;
    desc.height = h;
    desc.mipLevels = 1;
    desc.arraySize = 1;
    // sRGB, matching the ImGui font atlas: the UI pass is built with an sRGB
    // RTV and the decode permute, so a texture it samples must linearize on
    // read for the hardware to re-encode it on store. A plain UNORM here comes
    // out a gamma step too bright.
    desc.format = gfx::Format::R8G8B8A8_UNORM_SRGB;
    desc.usage = gfx::TextureUsage::ShaderResource;
    info.texture = gfx->CreateTexture(desc, pixels.data());
    if (info.texture == gfx::TextureHandle::Invalid) {
        info.error = "could not upload the image";
        return;
    }
    info.ok = true;
}

// Caller must have drained the GPU first — see the single WaitIdle in Trim and
// Clear. Doing it here instead would stall once per released entry.
void TextureThumbnailCache::Release(Entry& entry) {
    if (entry.info.texture == gfx::TextureHandle::Invalid)
        return;
    if (auto* gfx = svc_.Pipeline().Gfx())
        gfx->Destroy(entry.info.texture);
    entry.info.texture = gfx::TextureHandle::Invalid;
}

void TextureThumbnailCache::Trim() {
    if (static_cast<int>(entries_.size()) <= cap_)
        return;
    std::vector<std::pair<std::uint64_t, const std::string*>> byAge;
    byAge.reserve(entries_.size());
    for (const auto& [path, e] : entries_)
        byAge.emplace_back(e.lastVisibleFrame, &path);
    std::sort(byAge.begin(), byAge.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    const std::size_t drop = entries_.size() - static_cast<std::size_t>(cap_);
    std::vector<std::string> doomed;
    doomed.reserve(drop);
    for (std::size_t i = 0; i < drop; ++i)
        doomed.push_back(*byAge[i].second);

    // Once for the whole trim: every handle about to go may still be
    // referenced by last frame's draw list.
    if (auto* gfx = svc_.Pipeline().Gfx())
        gfx->WaitIdle();
    for (const auto& path : doomed) {
        auto it = entries_.find(path);
        if (it == entries_.end())
            continue;
        Release(it->second);
        entries_.erase(it);
    }
}

void TextureThumbnailCache::Clear() {
    if (!entries_.empty()) {
        if (auto* gfx = svc_.Pipeline().Gfx())
            gfx->WaitIdle();
        for (auto& [path, e] : entries_) {
            (void)path;
            Release(e);
        }
    }
    entries_.clear();
    pending_.clear();
}

} // namespace whiteout::flakes::tools
