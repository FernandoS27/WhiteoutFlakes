#include "renderer/assets/asset_manager.h"

#include "renderer/assets/texture_asset_manager.h"
#include "renderer/model/model_source_utils.h"
#include "renderer/model/model_template.h"
#include "whiteout/flakes/util/texture_image_usage.h"

#include <cornflakes/interface/asset/effect_asset_model.hpp>
#include <cornflakes/interface/asset/pkb_reader.hpp>
#include <cornflakes/interface/diagnostics/issue.hpp>
#include <cornflakes/interface/service/service_types.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>

namespace whiteout::flakes::renderer::assets {

AssetManager::AssetManager(TextureAssetManager& textures) : textures_(textures) {
    // PkbReader handles both .pkb and .pkfx — cornflakes' dispatcher
    // probes registered readers in priority order, first match wins.
    particleDispatch_.addReader(
        std::make_unique<::whiteout::cornflakes::PkbReader>());
}
AssetManager::~AssetManager() = default;

void AssetManager::SetGfxDevice(gfx::IGFXDevice* gfx) {
    std::lock_guard<std::mutex> lk(mu_);
    gfx_ = gfx;
}

void AssetManager::SetChildModelBuilder(ChildModelBuilder builder) {
    std::lock_guard<std::mutex> lk(mu_);
    childModelBuilder_ = std::move(builder);
}

void AssetManager::SetOnApplied(OnAppliedFn cb) {
    std::lock_guard<std::mutex> lk(mu_);
    onApplied_ = std::move(cb);
}

void AssetManager::AddDependency(SlotId parent, SlotId child) {
    if (parent == kInvalidSlot || child == kInvalidSlot) return;
    std::lock_guard<std::mutex> lk(mu_);
    auto it = slots_.find(parent);
    if (it == slots_.end()) return;
    it->second.dependencies.push_back(child);
}

std::string AssetManager::Normalize(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        if (c == '\\') c = '/';
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

AssetManager::SlotId AssetManager::AllocSlotId() noexcept {
    SlotId id = nextSlot_++;
    if (id == kInvalidSlot) id = nextSlot_++; // skip 0 on wrap
    return id;
}

AssetManager::SlotId AssetManager::Acquire(AssetKind kind, std::string_view path) {
    if (path.empty()) return kInvalidSlot;
    const std::string norm = Normalize(path);

    std::lock_guard<std::mutex> lk(mu_);
    ++statAcquires_;
    if (auto it = pathToSlot_.find(norm); it != pathToSlot_.end()) {
        slots_[it->second].refCount++;
        return it->second;
    }

    const SlotId id = AllocSlotId();
    Slot s;
    s.kind     = kind;
    s.path     = norm;
    s.refCount = 1;
    // Texture: bind the manager's shared "white" default until real
    // bytes arrive. Particle/ChildModel: leave payload null — consumers
    // null-check the typed accessors.
    if (kind == AssetKind::Texture)
        s.texHandle = textures_.GetDefaults().White;
    slots_.emplace(id, std::move(s));
    pathToSlot_.emplace(norm, id);
    needs_.emplace_back(kind, norm);
    return id;
}

void AssetManager::Release(SlotId slot) {
    if (slot == kInvalidSlot) return;
    gfx::TextureHandle toDestroy = gfx::TextureHandle::Invalid;
    std::vector<SlotId> dependencies;
    {
        std::lock_guard<std::mutex> lk(mu_);
        ++statReleases_;
        auto it = slots_.find(slot);
        if (it == slots_.end()) return;
        if (it->second.refCount > 0)
            it->second.refCount--;
        if (it->second.refCount != 0) return;

        // Only destroy if it's a real, non-placeholder texture. The
        // shared placeholder lives on TextureAssetManager and outlives
        // every slot.
        if (it->second.kind == AssetKind::Texture &&
            it->second.loaded &&
            it->second.texHandle != gfx::TextureHandle::Invalid &&
            it->second.texHandle != textures_.GetDefaults().White) {
            toDestroy = it->second.texHandle;
        }

        dependencies = std::move(it->second.dependencies);
        pathToSlot_.erase(it->second.path);
        slots_.erase(it);
    }
    if (toDestroy != gfx::TextureHandle::Invalid && gfx_)
        gfx_->Destroy(toDestroy);
    // Outside the lock — dependent releases recurse into Release(), which
    // takes mu_ itself.
    for (SlotId d : dependencies)
        Release(d);
}

bool AssetManager::Loaded(SlotId slot) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = slots_.find(slot);
    return it != slots_.end() && it->second.loaded;
}

u32 AssetManager::GenerationOf(SlotId slot) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = slots_.find(slot);
    return (it != slots_.end()) ? it->second.generation : 0u;
}

gfx::TextureHandle AssetManager::TextureOf(SlotId slot) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = slots_.find(slot);
    if (it == slots_.end() || it->second.kind != AssetKind::Texture)
        return textures_.GetDefaults().White;
    return it->second.texHandle;
}

const cornflakes::EffectAssetModel* AssetManager::ParticleAssetOf(SlotId slot) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = slots_.find(slot);
    if (it == slots_.end() || it->second.kind != AssetKind::Particle)
        return nullptr;
    return it->second.particleAsset.get();
}

std::shared_ptr<model::ModelTemplate> AssetManager::ChildModelOf(SlotId slot) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = slots_.find(slot);
    if (it == slots_.end() || it->second.kind != AssetKind::ChildModel)
        return nullptr;
    return it->second.childTemplate;
}

void AssetManager::DrainNeeds(const NeededFn& cb) {
    std::deque<std::pair<AssetKind, std::string>> batch;
    {
        std::lock_guard<std::mutex> lk(mu_);
        batch.swap(needs_);
    }
    if (!cb) return;
    for (auto& [kind, path] : batch)
        cb(kind, path);
}

std::size_t AssetManager::PendingNeedsCount() const {
    std::lock_guard<std::mutex> lk(mu_);
    return needs_.size();
}

namespace {
// Decode a texture file into the format the GPU will sample. Keeps
// the source's native PixelFormat (BLP1 / BC1 / BC3 / etc.) when the
// gfx backend speaks it — that preserves compression AND lets us
// upload the full mip chain straight from the file with no recompress
// step. Only falls through to RGBA8 when the source format isn't a
// supported gfx::Format. The output `outBytes` holds every mip
// concatenated tightly: mip0, mip1, mip2, … `outMipLevels` is the
// count; the gfx layer slices the buffer using rowPitch/levelSize
// math derived from format + (w, h).
bool DecodeTexture(std::span<const u8> bytes, const std::string& ext,
                   const std::string& pathForSrgb,
                   std::vector<u8>& outBytes, i32& outW, i32& outH,
                   i32& outMipLevels, gfx::Format& outFormat) {
    auto result = model::DispatchTextureParser(
        ext, [&](auto& parser) { return parser.parse(bytes); });
    if (!result)
        return false;

    // Pick the most faithful gfx::Format. If WhiteoutLib's PixelFormat
    // maps to something the backend supports, use it directly (keeps
    // BC compression and the mip chain intact). Otherwise re-encode
    // the decoded data to RGBA8 — drops compression but salvages
    // the texture.
    gfx::Format fmt = model::WhiteoutFormatToGfx(result->format(), result->isSrgb());
    if (fmt == gfx::Format::Unknown) {
        result->format(whiteout::textures::PixelFormat::RGBA8);
        fmt = result->isSrgb() ? gfx::Format::R8G8B8A8_UNORM_SRGB
                                : gfx::Format::R8G8B8A8_UNORM;
    }
    outFormat = ::whiteout::flakes::ApplyTextureSrgbPolicy(fmt, pathForSrgb);
    outW = static_cast<i32>(result->width());
    outH = static_cast<i32>(result->height());
    outMipLevels = static_cast<i32>(result->mipCount());
    if (outMipLevels < 1) outMipLevels = 1;

    // Concatenate every mip into a single buffer so the upload path
    // sees the chain inline. The gfx backend walks the levels based
    // on TextureDesc.mipLevels + format-derived per-level sizes.
    std::size_t total = 0;
    for (u32 m = 0; m < result->mipCount(); ++m)
        total += result->mipData(m).size();
    outBytes.resize(total);
    u8* cursor = outBytes.data();
    for (u32 m = 0; m < result->mipCount(); ++m) {
        auto src = result->mipData(m);
        std::memcpy(cursor, src.data(), src.size());
        cursor += src.size();
    }
    return outW > 0 && outH > 0 && !outBytes.empty();
}
} // namespace

bool AssetManager::ApplyPrepared(AssetKind kind, std::string_view path,
                                 std::span<const u8> bytes, std::string_view foundExt) {
    if (path.empty() || bytes.empty()) {
        std::lock_guard<std::mutex> lk(mu_);
        ++statApplyMisses_;
        return false;
    }
    const std::string norm = Normalize(path);

    // Locate the target slot up front. If nothing is waiting on this
    // path, drop the bytes — happens when consumers Release before the
    // host's fetch resolved. Stats count it as a miss.
    SlotId target = kInvalidSlot;
    AssetKind expectedKind = kind;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = pathToSlot_.find(norm);
        if (it == pathToSlot_.end()) {
            ++statApplyMisses_;
            return false;
        }
        target = it->second;
        expectedKind = slots_[target].kind;
    }
    if (expectedKind != kind) {
        std::lock_guard<std::mutex> lk(mu_);
        ++statApplyMisses_;
        return false;
    }

    Prepared prep;
    prep.slot = target;
    prep.kind = kind;

    if (kind == AssetKind::Texture) {
        std::string ext(foundExt);
        if (ext.empty()) {
            // Fall back to the requested-path extension. Keeps Apply
            // usable without the host having to thread foundExt through.
            ext = model::ExtensionLower(std::filesystem::path(norm));
        }
        if (!DecodeTexture(bytes, ext, norm, prep.pixels, prep.width, prep.height,
                           prep.mipLevels, prep.format)) {
            std::lock_guard<std::mutex> lk(mu_);
            ++statApplyMisses_;
            return false;
        }
    } else if (kind == AssetKind::Particle) {
        // .pkb / .pkfx — parse into a FRESH per-slot arena. PkbReader
        // keeps spans into both the source buffer and the arena, so
        // both have to outlive the slot — and FREE when the slot dies
        // so memory reclaims on model unload. Pinning the bytes +
        // owning the arena per-slot gives us that automatically (the
        // unique_ptr + shared_ptr destructors release the chunks).
        auto pinned = std::make_shared<std::vector<std::byte>>();
        pinned->resize(bytes.size());
        std::memcpy(pinned->data(), bytes.data(), bytes.size());
        auto arena = std::make_unique<::whiteout::cornflakes::ExpandingArena>(64 * 1024);
        ::whiteout::cornflakes::BakedSource src;
        src.path  = norm;
        src.bytes = std::span<const std::byte>(pinned->data(), pinned->size());
        ::whiteout::cornflakes::IssueBag issues;
        std::optional<::whiteout::cornflakes::EffectAssetModel> parsed;
        {
            // SerializerPriorityDispatcher::read isn't documented as
            // thread-safe even when each call gets its own arena;
            // serialise to be safe (Particle applies are infrequent).
            std::lock_guard<std::mutex> lk(mu_);
            parsed = particleDispatch_.read(src, *arena, issues);
        }
        if (!parsed.has_value() || issues.hasFatal()) {
            std::lock_guard<std::mutex> lk(mu_);
            ++statApplyMisses_;
            return false;
        }
        prep.particleAsset =
            std::make_shared<::whiteout::cornflakes::EffectAssetModel>(*parsed);
        prep.particleBytes = std::move(pinned);
        prep.particleArena = std::move(arena);
    } else if (kind == AssetKind::ChildModel) {
        // MDX child template — defer to the host-provided builder. The
        // builder wraps ModelTemplateManager's parse path so AssetManager
        // doesn't need to drag MDX-parser headers into its translation
        // unit. Builder runs without the manager mutex held (parses can
        // be expensive — texture-cache lookups, adapter construction,
        // etc.) so other Acquires/Releases can proceed in parallel.
        ChildModelBuilder builder;
        {
            std::lock_guard<std::mutex> lk(mu_);
            builder = childModelBuilder_;
        }
        if (!builder) {
            std::lock_guard<std::mutex> lk(mu_);
            ++statApplyMisses_;
            return false;
        }
        auto tmpl = builder(norm, bytes, foundExt);
        if (!tmpl) {
            std::lock_guard<std::mutex> lk(mu_);
            ++statApplyMisses_;
            return false;
        }
        prep.childTemplate = std::move(tmpl);
    } else {
        std::lock_guard<std::mutex> lk(mu_);
        ++statApplyMisses_;
        return false;
    }

    std::lock_guard<std::mutex> lk(mu_);
    ++statApplies_;
    prepared_.emplace_back(std::move(prep));
    return true;
}

void AssetManager::CommitPrepared() {
    std::deque<Prepared> batch;
    {
        std::lock_guard<std::mutex> lk(mu_);
        batch.swap(prepared_);
    }
    if (batch.empty() || !gfx_)
        return;

    const gfx::TextureHandle placeholder = textures_.GetDefaults().White;

    // Slots that completed this Commit. Their OnApplied callback fires
    // OUTSIDE the manager mutex below — the callback typically Acquires
    // dependent slots which would otherwise deadlock against mu_.
    std::vector<std::pair<SlotId, AssetKind>> applied;
    applied.reserve(batch.size());

    for (auto& p : batch) {
        if (p.kind == AssetKind::Texture) {
            // Build the GPU texture outside the lock — CreateTexture
            // can be expensive and we don't want to block
            // Acquire/Release callers. `p.pixels` holds every mip
            // level concatenated; the backend slices it using
            // (format, w, h, mipLevels).
            const gfx::TextureDesc desc{
                .width     = p.width,
                .height    = p.height,
                .mipLevels = (std::max)(1, p.mipLevels),
                .format    = p.format,
                .usage     = gfx::TextureUsage::ShaderResource,
            };
            const gfx::TextureHandle freshHandle = gfx_->CreateTexture(desc, p.pixels.data());

            gfx::TextureHandle toDestroy = gfx::TextureHandle::Invalid;
            bool slotApplied = false;
            {
                std::lock_guard<std::mutex> lk(mu_);
                auto it = slots_.find(p.slot);
                if (it == slots_.end()) {
                    toDestroy = freshHandle;
                } else {
                    Slot& s = it->second;
                    // Don't destroy the shared placeholder — every other
                    // unloaded slot still holds it.
                    if (s.loaded && s.texHandle != gfx::TextureHandle::Invalid &&
                        s.texHandle != placeholder) {
                        toDestroy = s.texHandle;
                    }
                    s.texHandle = freshHandle;
                    s.loaded    = true;
                    s.generation++;
                    slotApplied = true;
                }
            }
            if (toDestroy != gfx::TextureHandle::Invalid)
                gfx_->Destroy(toDestroy);
            if (slotApplied) applied.emplace_back(p.slot, p.kind);
        } else if (p.kind == AssetKind::Particle) {
            // Particle slot — swap in the parsed model and adopt the
            // per-slot arena + source bytes that back its spans. The
            // arena's unique_ptr lives on the slot, so when the slot
            // is Released-to-zero the arena destructs and reclaims
            // every chunk it allocated. No GPU work; consumers
            // (CornEffectsEmitter::TrySpawn) notice via generation
            // bump on their next tick.
            bool slotApplied = false;
            {
                std::lock_guard<std::mutex> lk(mu_);
                auto it = slots_.find(p.slot);
                if (it != slots_.end()) {
                    Slot& s = it->second;
                    s.particleAsset = std::move(p.particleAsset);
                    s.particleBytes = std::move(p.particleBytes);
                    s.particleArena = std::move(p.particleArena);
                    s.loaded        = true;
                    s.generation++;
                    slotApplied = true;
                }
            }
            if (slotApplied) applied.emplace_back(p.slot, p.kind);
        } else if (p.kind == AssetKind::ChildModel) {
            // ChildModel slot — assign the parsed template. The
            // template's GPU geosets get uploaded lazily by
            // ModelLoader::uploadTemplateGpu when the first actor
            // referencing it hits its UploadStagedGeosets pass.
            bool slotApplied = false;
            {
                std::lock_guard<std::mutex> lk(mu_);
                auto it = slots_.find(p.slot);
                if (it != slots_.end()) {
                    Slot& s = it->second;
                    s.childTemplate = std::move(p.childTemplate);
                    s.loaded        = true;
                    s.generation++;
                    slotApplied = true;
                }
            }
            if (slotApplied) applied.emplace_back(p.slot, p.kind);
        }
    }

    // Fire the OnApplied hook outside the manager mutex so callbacks can
    // Acquire dependent slots (e.g. corn-fx textures referenced by a
    // freshly-parsed .pkb) without deadlocking.
    OnAppliedFn cb;
    {
        std::lock_guard<std::mutex> lk(mu_);
        cb = onApplied_;
    }
    if (cb) {
        for (const auto& [id, kind] : applied)
            cb(id, kind);
    }
}

AssetManager::Stats AssetManager::GetStats() const {
    std::lock_guard<std::mutex> lk(mu_);
    Stats s;
    s.liveSlots        = slots_.size();
    s.totalAcquires    = statAcquires_;
    s.totalReleases    = statReleases_;
    s.totalApplies     = statApplies_;
    s.totalApplyMisses = statApplyMisses_;
    for (const auto& [_, slot] : slots_) {
        if (slot.loaded) ++s.loadedSlots;
    }
    return s;
}

} // namespace whiteout::flakes::renderer::assets
