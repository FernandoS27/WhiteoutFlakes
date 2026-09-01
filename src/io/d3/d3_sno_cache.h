#pragma once

// ============================================================================
// D3SnoCache — parsed SNO assets, read once and parsed once.
//
// D3 is the first product here where one model is a *graph of shared assets*.
// 19,154 actors reference 8,550 distinct appearances (mean 2.24 actors per
// `.app`, worst case 594 naming one), so "read it when you need it" parses the
// same 4.5 MB file 594 times. This is the cache that stops that.
//
// Five decisions, each of which is a way to get it wrong:
//
// **Keyed on the id alone.** D3 SNO ids are globally unique across groups —
// CoreTOC is one flat `snoId -> entry` map and the CASC root's own
// findByFileDataId takes an id with no group. Measured: 4,051 files across 11
// groups, 4,051 distinct header snoIds, zero collisions, and 600 id round-trips
// returning bytes identical to the path read. The group is still stored with
// the value and asserted on hit — not to disambiguate, but because a hit that
// returns the wrong type is a silent reinterpret into unrelated memory, exactly
// what SurfaceTableCast's product assert exists to prevent one layer up.
//
// **One read, one parse, one funnel.** Every accessor goes through `Load`,
// which reads the bytes, settles the group from them, parses into that group's
// type and stores the result. There is deliberately no "sniff the group, then
// fetch it typed" pair of calls: that shape reads the file twice for a caller
// that does the obvious thing.
//
// **Negative caching is not optional.** References are -1 constantly, and a
// live reference can still miss (2 of Barbarian_Male's 259 clip references do
// not resolve in the shipped install). A remembered miss costs one hash lookup;
// a forgotten one costs a CASC probe per actor, forever. Misses are counted
// separately from `misses` so "my model is missing things" and "my cache is
// thrashing" stay distinguishable.
//
// **Budgeted LRU over refcounted entries.** With a p95 of 8.3 MB and a max of
// 37.1 MB per `.app`, an unbounded map browsing a few hundred models is a
// gigabyte. Entries are handed out as `shared_ptr` and eviction only ever drops
// **the cache's own reference**, so a live model can never have its data pulled
// out from under it.
//
// **Resident bytes are estimated, not sizeof.** A parsed Appearances is a tree
// of vectors and `sizeof` is a constant. Each entry weighs its *source file's*
// byte count — off by the parse expansion factor, but monotonic in the right
// variable and free. A real EstimateBytes walking the vectors is the upgrade if
// the proxy misleads.
// ============================================================================

#include "whiteout/flakes/content_provider.h"
#include "whiteout/flakes/types.h"

#include <whiteout/sno/d3/native/d3_native.h>

#include <list>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace whiteout::flakes::io {

namespace d3n = ::whiteout::sno::d3::native;

/// @brief Which group @p bytes belong to, from the file alone.
///
/// The SNO preamble is `{magic, version, 0, 0}` and carries no group, so this
/// reads the *version* word against a measured per-group table. It is a
/// heuristic and the last resort in the cascade — `AssetRef` states the group
/// for every reference inside a parsed asset, and a browsed path states it by
/// extension; this only has to answer for a bare id a host typed.
///
/// Measured across the corpus: Actor 282, Appearance 260, Anim 118, Material
/// 25, ShaderMap 26, AnimTree 30, Particle 180, EffectGroup 47, Cloth 51,
/// Physics 37 — and **AnimSet and PhysMesh both 24**, which is why that pair
/// resolves to Unknown rather than to a coin flip.
d3n::Group D3GroupOfBytes(std::span<const u8> bytes);

/// @brief Which group a file extension names, or Unknown.
d3n::Group D3GroupOfExtension(std::string_view ext);

/// @brief The full cascade: the ref's extension first, then the version word.
d3n::Group D3GroupFor(const ContentRef& ref, std::span<const u8> bytes);

/// @brief The first four bytes are the SNO magic.
bool LooksLikeD3(std::span<const u8> bytes);

/// @brief The asset's own SNO id, or -1.
///
/// The 16-byte preamble carries no id; `dwSnoId` is the first field of the
/// struct image that follows it, which every group this reads has. Texture does
/// not, and is not a group anything asks this about — its bytes reach
/// AssetManager, never the SNO cache.
i32 D3SnoIdOfBytes(std::span<const u8> bytes);

/// @brief One sprite sheet's flip-book table, as the particle simulation needs
///        it.
///
/// A Diablo III `.tex` carries its own sub-rect table — `Textures+528` is the
/// frame count and `+544` the array — and that, not any `.an2`, is where a
/// particle atlas comes from. WhiteoutLib's TEX parser already reads it into
/// `TexInfo::frames`; this is the shape the emitter wants, with the pixel size
/// kept because the engine derives the quad's ASPECT from it.
struct D3TextureAtlas {
    /// (u0, v0, u1, v1) per frame, in the file's own order.
    std::vector<Vector4f> frames;
    u32 width = 0;  ///< Pixels, for the quad aspect.
    u32 height = 0;
    /// @brief How many leading records the reader skipped as junk.
    ///
    /// Diagnostics only, and kept because the skip is a heuristic: if it were
    /// really a fixed leading slot this would be 1 on every sheet, and if it
    /// were really junk-detection it would vary. Counting it is what separates
    /// the two, and the answer decides whether a sheet can silently lose its
    /// last tile. See D3SnoCache::TextureAtlas.
    u32 leadSkip = 0;

    /// @brief The tile SIZE, which the engine takes from frame 0 alone and
    ///        applies to every frame — only the origin varies per frame.
    Vector2f TileSize() const {
        if (frames.empty())
            return {1.0f, 1.0f};
        return {frames[0].z - frames[0].x, frames[0].w - frames[0].y};
    }
};

class D3SnoCache {
public:
    struct Stats {
        usize hits = 0;           ///< A typed accessor answered from memory.
        usize misses = 0;         ///< Read + parsed for the first time.
        usize negativeHits = 0;   ///< A remembered miss answered from memory.
        usize negativeMisses = 0; ///< Newly discovered miss (read or parse failed).
        usize bytesResident = 0;  ///< Sum of the source-file weights held.
        usize evictions = 0;
        usize reads = 0; ///< CASC reads issued through this cache.
        usize parses = 0;
    };

    explicit D3SnoCache(IContentProvider* provider) : provider_(provider) {}

    /// @brief Re-point at a different provider. Implies @ref Clear — a sno id
    ///        means something different in another install.
    void SetContentProvider(IContentProvider* provider);

    // One accessor per group we parse. The group is in the accessor's NAME,
    // which is what makes a typed hit checkable at all: a hit whose stored
    // group disagrees returns null rather than reinterpreting the value.
    std::shared_ptr<const d3n::Actor> Actor(i32 sno);
    std::shared_ptr<const d3n::Appearances> Appearance(i32 sno);
    std::shared_ptr<const d3n::Anim> Anim(i32 sno);
    std::shared_ptr<const d3n::AnimSet> AnimSet(i32 sno);
    std::shared_ptr<const d3n::Material> Material(i32 sno);
    std::shared_ptr<const d3n::Physics> Physics(i32 sno);
    /// The `.prt` particle system. Its channels are the whole animated
    /// content of the effect; see D3_PARTICLE_DESIGN.md.
    std::shared_ptr<const d3n::Particle> Particle(i32 sno);
    /// The `.efg` effect group — the indirection that carries the bulk of the
    /// shipped particles. 11,849 of the 21,593 `.prt` are named by an effect
    /// group and by nothing else. See `D3EffectResolver`.
    std::shared_ptr<const d3n::EffectGroup> EffectGroup(i32 sno);
    /// The `.clt` cloth *tuning*. The cloth GEOMETRY is baked into the
    /// Appearance's SubObjects, so this is the only asset a cloth needs beyond
    /// the model it hangs off.
    std::shared_ptr<const d3n::Cloth> Cloth(i32 sno);
    /// The `.shm` tag map and the `.shd` it resolves to. Together they carry
    /// the render state a material does not: blend, cull, depth and the
    /// alpha-test reference all live on the Shaders asset's RenderPass, never
    /// on the UberMaterial. See `D3PassStateFor`.
    std::shared_ptr<const d3n::ShaderMap> ShaderMap(i32 sno);
    std::shared_ptr<const d3n::Shaders> Shaders(i32 sno);

    /// @brief Parse @p bytes the caller already has, and cache the result
    ///        under @p sno.
    ///
    /// The entry point uses this: `TrySpawnForeign` has already read the file
    /// to sniff its magic, and re-reading it here would be the first duplicate
    /// read on the very first load.
    std::shared_ptr<const d3n::Actor> AdoptActor(i32 sno, std::span<const u8> bytes);
    std::shared_ptr<const d3n::Appearances> AdoptAppearance(i32 sno, std::span<const u8> bytes);

    /// @brief The raw bytes of @p sno, read fresh and **not** cached.
    ///
    /// The one thing the parsed tree cannot answer: a `CollisionShape`'s cooked
    /// polytope is a header holding four more payload references, and resolving
    /// them needs the file the offsets are relative to. The cache stores parsed
    /// values, not bytes, and holding a second copy of a 37 MB `.app` to serve
    /// one caller would undo the budget the LRU exists to keep.
    ///
    /// So this deliberately re-reads. It is called once per model load, by the
    /// collision builder; anything calling it per frame is using it wrong.
    std::vector<u8> ReadBytes(i32 sno);

    /// @brief The flip-book frame table of the `.tex` @p sno, or null.
    ///
    /// Null both when the texture carries no table (the overwhelming majority)
    /// and when it cannot be read, which are the same answer to the caller: a
    /// layer with no atlas samples the whole sheet.
    ///
    /// Memoised in its own map rather than through the LRU: the value is a few
    /// dozen floats and re-reading a 1 MB `.tex` to recover it would be the
    /// expensive half. It does cost one full texture decode on the first ask,
    /// because the TEX parser has no metadata-only entry point — which is why
    /// this is a load-time call and nothing asks it per frame.
    std::shared_ptr<const D3TextureAtlas> TextureAtlas(i32 sno);

    /// @brief Which group @p sno is, reading it through the cache if needed.
    d3n::Group GroupOf(i32 sno);

    /// @brief The author's name for @p sno — the SNO's file name with no
    ///        directory and no extension — or empty.
    ///
    /// Nothing inside a D3 asset names anything it references: an AnimSet maps
    /// a tag id to an Anim SNO and stops. The name lives in CoreTOC, which the
    /// storage root has already read to build `Base\Anim\<name>.ani`, so this
    /// is a manifest lookup and not a file read — the file is never opened.
    ///
    /// Memoised for the same reason the parses are: a character AnimSet asks
    /// this 259 times on load and the answers never change while a storage is
    /// open. The map is separate from @ref entries_ so a name survives the LRU
    /// evicting the asset — it weighs a few dozen bytes and re-deriving it
    /// after eviction would put a manifest lookup on the *replay* path.
    const std::string& NameOf(i32 sno);

    Stats GetStats() const {
        return stats_;
    }
    void ResetStats() {
        stats_ = {};
    }
    usize BudgetBytes() const {
        return budget_;
    }
    usize EntryCount() const {
        return entries_.size();
    }
    void SetBudgetBytes(usize bytes);

    /// @brief Forget everything. Every handed-out `shared_ptr` stays valid.
    void Clear();

private:
    struct Entry {
        d3n::Group group = d3n::Group::Unknown;
        std::shared_ptr<const void> value; ///< Null for a remembered miss.
        usize weight = 0;
        std::list<i32>::iterator lru;
    };

    /// @brief What a load produced. Returned by value rather than as a
    ///        pointer into the map: a file bigger than the whole budget is
    ///        evicted by the very insert that added it, and the caller must
    ///        still get what it asked for.
    struct Loaded {
        d3n::Group group = d3n::Group::Unknown;
        std::shared_ptr<const void> value;
    };

    /// @brief Find-or-(read, parse, insert). Parses at most once per id.
    ///        @p bytes, when non-empty, is used instead of reading.
    Loaded Load(i32 sno, std::span<const u8> bytes);

    /// @brief Typed view of @ref Load: null on a miss *or* a group mismatch.
    template <typename T>
    std::shared_ptr<const T> Typed(i32 sno, d3n::Group want, std::span<const u8> bytes = {});

    void Touch(Entry& e);
    void EvictToBudget();

    IContentProvider* provider_ = nullptr;
    std::unordered_map<i32, Entry> entries_;
    std::unordered_map<i32, std::string> names_;
    /// Null values are remembered misses — see @ref TextureAtlas.
    std::unordered_map<i32, std::shared_ptr<const D3TextureAtlas>> atlases_;
    std::list<i32> lru_; ///< Front = most recently used.
    // 512 MB is a starting number to revise against Stats::bytesResident in the
    // model browser, not a measurement.
    usize budget_ = 512u * 1024u * 1024u;
    Stats stats_;
};

} // namespace whiteout::flakes::io
