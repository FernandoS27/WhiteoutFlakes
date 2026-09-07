#include "renderer/profiles/sc2_heroes/sc2_model_catalog.h"

#include "io/file_content_provider.h"
#include "io/m3/m3_model_adapter.h"
#include "io/progress.h"
#include "whiteout/flakes/content_provider.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace whiteout::flakes::renderer::profiles::sc2_heroes {

namespace {

// ---------------------------------------------------------------------------
// A tag scanner, not an XML parser
// ---------------------------------------------------------------------------
//
// The catalog is machine-generated and rigidly regular: four element names, all
// attributes double-quoted, no entities in the values that matter (they are file
// paths), no CDATA. Pulling in an XML library to read `<Model value="x"/>` would
// be a dependency for a `find`, so this reads it the way the rest of src/io
// reads `.slk` and `CoreTOC` — narrowly, and only what it needs.

bool IsNameEnd(char c) {
    return c == '>' || c == '/' || std::isspace(static_cast<unsigned char>(c)) != 0;
}

/// Offset of the `>` closing the tag that starts at @p at, skipping any inside
/// a quoted attribute value. No shipped path carries one, but a `.stormmod` is
/// user content, and a tag that ends in the wrong place silently swallows the
/// next entry rather than failing.
usize FindTagEnd(std::string_view s, usize at) {
    bool quoted = false;
    for (usize i = at; i < s.size(); ++i) {
        if (s[i] == '"')
            quoted = !quoted;
        else if (s[i] == '>' && !quoted)
            return i;
    }
    return std::string_view::npos;
}

/// Offset of the next `<`+@p name element start at or after @p from, requiring
/// the name to end on a delimiter so `<Model` does not match `<ModelFoo` (and
/// so the leading `<` keeps `<LowQualityModel` from matching `Model`).
usize FindElement(std::string_view s, std::string_view name, usize from) {
    while (from < s.size()) {
        const usize at = s.find(name, from);
        if (at == std::string_view::npos || at == 0)
            return std::string_view::npos;
        const usize end = at + name.size();
        if (s[at - 1] == '<' && end < s.size() && IsNameEnd(s[end]))
            return at - 1;
        from = at + 1;
    }
    return std::string_view::npos;
}

/// The value of attribute @p name inside the single tag @p tag, or nullopt.
/// Case-insensitive on the name, because the shipped catalog spells the same
/// idea `value` on RequiredAnims and `FilePath` on RequiredAnimsEx and there is
/// no promise the casing is stable.
std::optional<std::string_view> Attr(std::string_view tag, std::string_view name) {
    auto equalsFold = [](std::string_view a, std::string_view b) {
        return a.size() == b.size() &&
               std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                   return std::tolower(static_cast<unsigned char>(x)) ==
                          std::tolower(static_cast<unsigned char>(y));
               });
    };
    for (usize i = 1; i + name.size() < tag.size(); ++i) {
        // Attribute names start on a delimiter, so `index` never matches the
        // tail of another attribute's name.
        if (std::isspace(static_cast<unsigned char>(tag[i - 1])) == 0)
            continue;
        if (!equalsFold(tag.substr(i, name.size()), name))
            continue;
        usize j = i + name.size();
        while (j < tag.size() && std::isspace(static_cast<unsigned char>(tag[j])) != 0)
            ++j;
        if (j >= tag.size() || tag[j] != '=')
            continue;
        ++j;
        while (j < tag.size() && std::isspace(static_cast<unsigned char>(tag[j])) != 0)
            ++j;
        if (j >= tag.size() || tag[j] != '"')
            continue;
        const usize close = tag.find('"', j + 1);
        if (close == std::string_view::npos)
            return std::nullopt;
        return tag.substr(j + 1, close - j - 1);
    }
    return std::nullopt;
}

/// Lowercased, `/`-separated, leading separators trimmed. Both halves of the
/// join arrive spelled differently — the catalog writes
/// `Assets\Units\Heroes\X.m3`, the storage lists
/// `mods/heroes.stormmod/base.stormassets/assets/units/heroes/x.m3` — and this
/// is the form they have in common.
std::string Normalize(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in)
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(c == '\\' ? '/' : c)));
    usize start = 0;
    while (start < out.size() && out[start] == '/')
        ++start;
    out.erase(0, start);
    return out;
}

/// The `assets/...` tail of a normalised path, which is what the catalog names
/// and what every mod root ends with.
///
/// Searched as `/assets/` rather than `assets/`: a StarCraft II mod directory is
/// called `base.sc2assets` and a Heroes one `base.stormassets`, both of which
/// contain the bare substring.
std::string_view AssetTail(std::string_view norm) {
    const usize at = norm.find("/assets/");
    if (at != std::string_view::npos)
        return norm.substr(at + 1);
    if (norm.rfind("assets/", 0) == 0)
        return norm;
    return {};
}

std::string_view BaseName(std::string_view norm) {
    const usize slash = norm.rfind('/');
    return slash == std::string_view::npos ? norm : norm.substr(slash + 1);
}

// One catalog entry as it was written, before inheritance is resolved.
struct RawEntry {
    std::string parent;
    std::string model;
    // A RequiredAnims tag. `index < 0` is the plain appending form; a
    // non-negative index SETS that slot, which is how a skin overrides one of
    // its parent's files and keeps the rest (see the header).
    struct Op {
        int index = -1;
        bool removed = false;
        std::string value;
    };
    std::vector<Op> ops;
};

void ParseCatalog(std::string_view xml, std::unordered_map<std::string, RawEntry>& out) {
    usize at = 0;
    while ((at = FindElement(xml, "CModel", at)) != std::string_view::npos) {
        const usize tagEnd = FindTagEnd(xml, at);
        if (tagEnd == std::string_view::npos)
            return;
        const std::string_view head = xml.substr(at, tagEnd - at + 1);
        const bool selfClosing = tagEnd > 0 && xml[tagEnd - 1] == '/';

        std::string_view body;
        usize next = tagEnd + 1;
        if (!selfClosing) {
            // CModel entries never nest, so the first close is this one's.
            const usize close = xml.find("</CModel", tagEnd);
            if (close == std::string_view::npos)
                return;
            body = xml.substr(tagEnd + 1, close - tagEnd - 1);
            next = close + 1;
        }
        at = next;

        const auto id = Attr(head, "id");
        if (!id || id->empty())
            continue;

        // Later files override earlier ones for the same id, and a mod that
        // only adjusts an entry writes just the fields it changes — so this
        // accumulates rather than replaces.
        RawEntry& rec = out[std::string(*id)];
        if (const auto parent = Attr(head, "parent"); parent && !parent->empty())
            rec.parent.assign(*parent);
        if (body.empty())
            continue;

        if (const usize m = FindElement(body, "Model", 0); m != std::string_view::npos) {
            const usize e = FindTagEnd(body, m);
            if (e != std::string_view::npos) {
                if (const auto v = Attr(body.substr(m, e - m + 1), "value"); v && !v->empty())
                    rec.model.assign(*v);
            }
        }

        // `RequiredAnims` and `RequiredAnimsEx` are the same field in two
        // spellings; FindElement's delimiter rule would reject the second under
        // the first's name, so both are walked.
        for (std::string_view name : {std::string_view("RequiredAnims"),
                                      std::string_view("RequiredAnimsEx")}) {
            usize t = 0;
            while ((t = FindElement(body, name, t)) != std::string_view::npos) {
                const usize e = FindTagEnd(body, t);
                if (e == std::string_view::npos)
                    break;
                const std::string_view tag = body.substr(t, e - t + 1);
                t = e + 1;

                RawEntry::Op op;
                if (const auto idx = Attr(tag, "index"); idx && !idx->empty()) {
                    int slot = -1;
                    const auto [ptr, ec] =
                        std::from_chars(idx->data(), idx->data() + idx->size(), slot);
                    (void)ptr;
                    if (ec != std::errc{} || slot < 0)
                        continue;
                    op.index = slot;
                }
                if (const auto rm = Attr(tag, "removed"); rm && *rm == "1")
                    op.removed = true;
                const auto v = Attr(tag, "value");
                const auto f = Attr(tag, "FilePath");
                if (v && !v->empty())
                    op.value.assign(*v);
                else if (f && !f->empty())
                    op.value.assign(*f);
                if (op.value.empty() && !op.removed)
                    continue;
                rec.ops.push_back(std::move(op));
            }
        }
    }
}

/// The effective (model, animations) of @p id with `parent=` resolved.
///
/// @p depth guards a catalog that names itself, directly or in a ring — which
/// no shipped one does, but a mod is user content.
void Resolve(const std::unordered_map<std::string, RawEntry>& raw, const std::string& id,
             std::string& model, std::vector<std::string>& anims, int depth = 0) {
    const auto it = raw.find(id);
    if (it == raw.end() || depth > 32)
        return;
    const RawEntry& rec = it->second;
    if (!rec.parent.empty())
        Resolve(raw, rec.parent, model, anims, depth + 1);
    if (!rec.model.empty())
        model = rec.model;
    for (const auto& op : rec.ops) {
        if (op.index >= 0) {
            const auto slot = static_cast<usize>(op.index);
            if (slot >= anims.size())
                anims.resize(slot + 1);
            anims[slot] = op.removed ? std::string{} : op.value;
        } else if (!op.removed) {
            anims.push_back(op.value);
        }
    }
}

} // namespace

namespace {

/// What decides a provider's answer: which install it reads and as which
/// product. Two providers that agree here have the same catalog, whichever
/// objects they are. Empty for a provider that cannot say, which then caches
/// nothing rather than sharing an index it cannot vouch for.
std::string InstallKeyOf(io::IContentProvider* provider) {
    const auto* file = dynamic_cast<const io::FileContentProvider*>(provider);
    if (!file)
        return {};
    // Both roots: ProductId::Sc2 covers two installs and a provider may carry
    // either or both, and an index built over one is not the other's.
    return Normalize(file->InstallPath()) + "|" + Normalize(file->HotsInstallPath()) + "|" +
           std::to_string(static_cast<int>(file->Game()));
}

// Enough for the two a session reaches, and a bound rather than a budget.
constexpr usize kMaxCachedIndexes = 4;

} // namespace

void Sc2ModelCatalog::SetContentProvider(io::IContentProvider* provider) {
    std::string key = InstallKeyOf(provider);
    // The KEY, not just the pointer. A host keeps one provider and repoints it
    // — the viewer's settings page switches its product, and its install path
    // with it — so an unchanged pointer is no promise the install is the same
    // one. Comparing pointers alone left the previous game's catalog standing.
    if (provider_ == provider && key == installKey_)
        return;
    provider_ = provider;
    installKey_ = std::move(key);
    byModel_.clear();
    loaded_ = false;
    if (installKey_.empty())
        return;
    for (const auto& [key, index] : cache_) {
        if (key != installKey_)
            continue;
        // Already read for this install — by the other provider, or by this one
        // before the host pointed it elsewhere and back.
        byModel_ = *index;
        loaded_ = true;
        return;
    }
}

void Sc2ModelCatalog::Clear() {
    byModel_.clear();
    loaded_ = false;
    cache_.clear();
}

bool Sc2ModelCatalog::Prewarm(io::ProgressMonitor* progress) {
    return Build(progress);
}

bool Sc2ModelCatalog::Build(io::ProgressMonitor* progress) {
    if (loaded_)
        return !byModel_.empty();
    if (!provider_)
        return false;
    // Set before the work, not after: a storage with no catalog in it must not
    // re-walk its whole listing on every model that misses.
    loaded_ = true;

    // Indeterminate until the listing says how many files there are — the walk
    // itself is the slow half on a cold storage.
    if (progress)
        progress->Begin("Reading the model catalog");

    // One listing, filtered down. The provider cannot enumerate by pattern, so
    // this pays for the whole file list once — 316k entries on Heroes — to keep
    // ~5,700 of them. That is why Prewarm exists and why it runs on a task.
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::string> catalogs;
    for (std::string& p : provider_->ListFiles({}, /*recursive*/ true)) {
        // ListFiles answers lowercased with `/` separators, so both tests are
        // plain. `.sc2data` and `.stormdata` differ; `/gamedata/` does not.
        if (p.size() > 4 && p.compare(p.size() - 4, 4, ".xml") == 0 &&
            p.find("/gamedata/") != std::string::npos)
            catalogs.push_back(std::move(p));
    }
    if (catalogs.empty())
        return false; // no GameData here — a Warcraft III root, or a loose folder
    // Read order is the override order for an id declared twice, so it has to be
    // stable across runs — ListFiles answers from a std::set, but the filter
    // above does not promise to keep that.
    std::sort(catalogs.begin(), catalogs.end());

    if (progress)
        progress->Begin("Reading the model catalog", catalogs.size());

    const auto t1 = std::chrono::steady_clock::now();
    std::unordered_map<std::string, RawEntry> raw;
    usize read = 0;

    // Submitted in windows rather than read one at a time. `ReadFile` is
    // Request+Wait, and Wait's `doneCv` has a 10 ms backstop it reaches on
    // every one of these — 5,477 files came to 85 seconds that way, which is
    // not a wait anyone would sit through. Submitting a window first gives the
    // provider's worker pool something to run ahead on, and the retire order is
    // still `catalogs` order, which is what makes an id declared twice resolve
    // the same way every run.
    //
    // The window bounds what is in memory at once: the whole Heroes catalog is
    // ~90 MB of XML, and none of it needs to be resident after it is parsed.
    constexpr usize kWindow = 256;
    struct Slot {
        io::RequestId id = io::kInvalidRequestId;
        // Shared, not a reference into `window`: Pump erases a request from
        // `alive` just BEFORE running its callback, so a Wait on a non-pump
        // thread can return while the callback is still to come. A late one
        // then writes somewhere, and it must not be a vector this loop has
        // moved on from.
        std::shared_ptr<std::vector<u8>> bytes;
    };
    std::vector<Slot> window;
    window.reserve(kWindow);

    for (usize base = 0; base < catalogs.size(); base += kWindow) {
        const usize end = std::min(base + kWindow, catalogs.size());
        window.clear();
        for (usize i = base; i < end; ++i) {
            Slot slot;
            slot.bytes = std::make_shared<std::vector<u8>>();
            auto sink = slot.bytes;
            slot.id = provider_->Request(ContentRef::FromPath(catalogs[i]),
                                         [sink = std::move(sink)](io::RequestResult&& r) {
                                             if (r.ok)
                                                 *sink = std::move(r.data);
                                         });
            window.push_back(std::move(slot));
        }
        for (Slot& slot : window) {
            if (slot.id != io::kInvalidRequestId)
                provider_->Wait(slot.id);
            if (progress)
                progress->Worked(1);
            if (slot.bytes->empty())
                continue;
            ++read;
            ParseCatalog(std::string_view(reinterpret_cast<const char*>(slot.bytes->data()),
                                          slot.bytes->size()),
                         raw);
            slot.bytes.reset();
        }
        // Checked between windows, not between files: a cancel has to retire
        // the requests already in flight or their callbacks outlive `window`.
        if (progress && progress->Cancelled())
            break;
    }

    // Basenames that two different models share are dropped rather than
    // guessed at. None do in either shipped catalog, which is what makes the
    // fallback in LookupKey's second chance safe; the guard is so a future
    // catalog cannot make it quietly wrong.
    std::unordered_map<std::string, const std::vector<std::string>*> byName;
    std::unordered_set<std::string> ambiguous;

    for (const auto& [id, rec] : raw) {
        if (rec.ops.empty() && rec.parent.empty())
            continue; // nothing of its own and nothing to inherit
        std::string model;
        std::vector<std::string> anims;
        Resolve(raw, id, model, anims);
        if (model.empty() || anims.empty())
            continue;
        // `##id##` / `##race##` templates are abstract entries whose children
        // expand them. Left in, they collect every skin's animations onto one
        // unreachable key — 110 files on one of the three shipped ones.
        if (model.find("##") != std::string::npos)
            continue;

        std::vector<std::string> files;
        for (const std::string& a : anims) {
            if (a.empty())
                continue;
            const std::string an = Normalize(a);
            const std::string_view atail = AssetTail(an);
            files.emplace_back(atail.empty() ? std::string_view(an) : atail);
        }
        if (files.empty())
            continue;

        const std::string norm = Normalize(model);
        const std::string_view tail = AssetTail(norm);
        // Several catalog entries name one model — a skin and its portrait
        // addition, say — so this unions, and a file already listed is not
        // listed twice.
        auto& slot = byModel_[std::string(tail.empty() ? std::string_view(norm) : tail)];
        for (std::string& f : files) {
            if (std::find(slot.begin(), slot.end(), f) == slot.end())
                slot.push_back(std::move(f));
        }
    }

    for (const auto& [key, anims] : byModel_) {
        const std::string name(BaseName(key));
        const auto it = byName.find(name);
        if (it == byName.end())
            byName.emplace(name, &anims);
        else if (*it->second != anims)
            ambiguous.insert(name);
    }
    // Fold the unambiguous basenames in as their own keys, so a model opened
    // out of a flat extraction — where there is no `assets/` prefix to key on —
    // still finds its animations.
    for (const auto& [name, anims] : byName) {
        if (ambiguous.count(name) != 0 || byModel_.count(name) != 0)
            continue;
        byModel_.emplace(name, *anims);
    }

    // Remembered under the install, so the other provider over the same one
    // adopts it instead of reading 5,477 files again.
    if (!installKey_.empty() && !byModel_.empty()) {
        cache_.insert(cache_.begin(),
                      {installKey_, std::make_shared<const Index>(byModel_)});
        if (cache_.size() > kMaxCachedIndexes)
            cache_.resize(kMaxCachedIndexes);
    }

    const auto t2 = std::chrono::steady_clock::now();
    using Ms = std::chrono::milliseconds;
    std::printf("[sc2] model catalog: %zu of %zu GameData files read, %zu models with "
                "external animations (list %lldms, read+parse %lldms)\n",
                read, catalogs.size(), byModel_.size(),
                static_cast<long long>(std::chrono::duration_cast<Ms>(t1 - t0).count()),
                static_cast<long long>(std::chrono::duration_cast<Ms>(t2 - t1).count()));
    return !byModel_.empty();
}

const std::vector<std::string>& Sc2ModelCatalog::AnimationsFor(const ContentRef& modelRef) const {
    static const std::vector<std::string> kNone;
    if (!modelRef.IsPath() || modelRef.path.empty())
        return kNone;
    const std::string norm = Normalize(modelRef.path);
    const std::string_view tail = AssetTail(norm);
    if (!tail.empty()) {
        const auto it = byModel_.find(std::string(tail));
        if (it != byModel_.end())
            return it->second;
    }
    if (const auto it = byModel_.find(norm); it != byModel_.end())
        return it->second;
    // Last chance: the file's own name. A model opened from a flat extraction
    // has no mod path to key on, and no two catalog models share a basename.
    const auto it = byModel_.find(std::string(BaseName(norm)));
    return it != byModel_.end() ? it->second : kNone;
}

usize Sc2ModelCatalog::Apply(io::M3ModelAdapter& adapter, const ContentRef& modelRef) {
    if (!provider_ || !modelRef.IsPath() || modelRef.path.empty())
        return 0;
    // A host that never prewarmed still gets the right answer, just on this
    // thread. Cheap on the second model: Build sets loaded_ before it works.
    Build(nullptr);

    const std::vector<std::string>& wanted = AnimationsFor(modelRef);
    if (wanted.empty())
        return 0;

    usize attached = 0;
    for (const std::string& rel : wanted) {
        // The catalog path as it stands: CascSource retries a relative
        // `assets\...` read under every mod root it learned, which is the same
        // mechanism that resolves an `.m3`'s own texture names.
        auto bytes = provider_->ReadFile(ContentRef::FromPath(rel));
        if (!bytes || bytes->empty()) {
            // A flat extraction has the file but not the path. The catalog
            // named it exactly; only where to look for it is in question.
            const std::string_view name = BaseName(rel);
            const std::string norm = Normalize(modelRef.path);
            const usize slash = norm.rfind('/');
            if (slash != std::string::npos) {
                bytes = provider_->ReadFile(
                    ContentRef::FromPath(norm.substr(0, slash + 1) + std::string(name)));
            }
            if (!bytes || bytes->empty())
                bytes = provider_->ReadFile(ContentRef::FromPath(std::string(name)));
        }
        if (!bytes || bytes->empty())
            continue;

        // The label is what rejects a double attach, so it has to identify the
        // file rather than describe it — two heroes' `..._PortraitAnims.m3a`
        // are different files.
        std::string label(BaseName(rel));
        if (const usize dot = label.rfind('.'); dot != std::string::npos)
            label.erase(dot);
        if (adapter.AttachAnimationFile(std::move(label), *bytes))
            ++attached;
    }
    return attached;
}

} // namespace whiteout::flakes::renderer::profiles::sc2_heroes
