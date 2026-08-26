#include "game_storage.h"

#include "io/progress.h"

#include "casc_source.h"
#include "mpq_source.h"
#include "whiteout/flakes/util/path_utf8.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace whiteout::flakes::io {

namespace fs = std::filesystem;

GameStorage::GameStorage(ProductId game) : game_(game) {}

GameStorage::~GameStorage() = default;

bool GameStorage::Read(const std::string& path, SourceRead& out) const {
    for (const auto& s : sources_) {
        if (s->Read(path, out))
            return true;
    }
    return false;
}

bool GameStorage::ReadById(u32 fileId, SourceRead& out) const {
    for (const auto& s : sources_) {
        if (s->ReadById(fileId, out))
            return true;
    }
    return false;
}

u32 GameStorage::FileIdForPath(const std::string& path) const {
    for (const auto& s : sources_) {
        if (const u32 id = s->FileIdForPath(path))
            return id;
    }
    return 0;
}

std::string GameStorage::PathForFileId(u32 fileId) const {
    for (const auto& s : sources_) {
        if (std::string p = s->PathForFileId(fileId); !p.empty())
            return p;
    }
    return {};
}

void GameStorage::List(const std::function<void(std::string)>& emit) const {
    for (const auto& s : sources_)
        s->List(emit);
}

bool GameStorage::HasListfile() const {
#if WHITEOUT_HAS_CASC
    for (usize i = 0; i < cascCount_; ++i)
        if (static_cast<const CascSource*>(sources_[i].get())->HasListfile())
            return true;
#endif
    return false;
}

std::vector<std::string> GameStorage::CascRoots() const {
    std::vector<std::string> out;
    out.reserve(cascCount_);
    for (usize i = 0; i < cascCount_; ++i)
        out.push_back(sources_[i]->Root());
    return out;
}

// ---- Builder ---------------------------------------------------------------

StorageBuilder& StorageBuilder::ModChain(const std::atomic<bool>* hdMode) {
    hdMode_ = hdMode;
    return *this;
}

StorageBuilder& StorageBuilder::FileIds() {
    fileIds_ = true;
    return *this;
}

StorageBuilder& StorageBuilder::FrameSuffixFallback() {
    frameSuffixFallback_ = true;
    return *this;
}

StorageBuilder& StorageBuilder::AssetPrefixes() {
    assetPrefixes_ = true;
    return *this;
}

StorageBuilder& StorageBuilder::Listfile(std::string csvPath) {
    listfilePath_ = std::move(csvPath);
    return *this;
}

StorageBuilder& StorageBuilder::TactKeys(std::string keyPath) {
    tactKeyPath_ = std::move(keyPath);
    return *this;
}

StorageBuilder& StorageBuilder::Casc(std::string root) {
    if (root.empty())
        return *this;
    // Deduplicated: a Heroes-only install names the same directory twice.
    for (const auto& r : cascRoots_)
        if (r == root)
            return *this;
    cascRoots_.push_back(std::move(root));
    return *this;
}

StorageBuilder& StorageBuilder::Archives(const std::string& installRoot,
                                         const std::vector<std::string>& names) {
    if (installRoot.empty())
        return *this;
    for (const std::string& name : names) {
        if (name.empty())
            continue;
        const fs::path file = FsPathFromUtf8(installRoot) / name;
        if (!fs::exists(file)) {
            std::printf("[FileContentProvider] MPQ not found, skipping: %s\n",
                        PathToUtf8(file).c_str());
            continue;
        }
        archiveFiles_.push_back(PathToUtf8(file));
    }
    return *this;
}

std::unique_ptr<GameStorage> StorageBuilder::Build(ProgressMonitor* progress) {
    std::unique_ptr<GameStorage> storage(new GameStorage(game_));

    ProgressMonitor inert;
    ProgressMonitor& m = progress ? *progress : inert;
    // Archives are memory-mapped and cost effectively nothing next to a CASC
    // open, so they share one unit between them rather than one each.
    m.Begin("Opening storages", cascRoots_.size() + (archiveFiles_.empty() ? 0u : 1u));

#if WHITEOUT_HAS_CASC
    for (std::string& root : cascRoots_) {
        // One unit per install. StarCraft II and Heroes are two separate
        // roots either of which may be absent, and without the split the
        // second one restarts the bar at zero and reads as a hang.
        ProgressMonitor step = m.Split(1);
        if (m.Cancelled())
            break;
        CascSourceOptions opts;
        opts.hdMode = hdMode_;
        opts.fileIds = fileIds_;
        opts.frameSuffixFallback = frameSuffixFallback_;
        opts.assetPrefixFallback = assetPrefixes_;
        opts.listfilePath = listfilePath_;
        opts.tactKeyFile = tactKeyPath_;
        // Unconditional, and deliberately NOT paired with the key list. It was
        // paired, and that made a session with no keys the one that got the
        // least: `creaturedisplayinfo.db2` carries a TACT-locked frame on a
        // stock 11.x install, so the whole table read back as *missing* and
        // every creature wearing a CreatureDisplayInfo skin bound white.
        // Measured 2026-08-25 — with zero-fill alone (no keys imported at all)
        // the same file reads 2439358 bytes, and centaur2_male / aetherwyrm
        // resolve their type-11/12 slots. Keys are what decrypts a frame;
        // zero-fill is what stops one frame from costing the file, and wanting
        // the second has never implied having the first.
        opts.zeroFillEncrypted = true;
        std::string error;
        if (auto src = CascSource::Open(root, opts, error, &step)) {
            storage->sources_.push_back(std::move(src));
        } else {
            // Not necessarily a failure: StarCraft II and Heroes are offered
            // both roots and only one may be installed.
            std::printf("[casc] not available at '%s': %s\n", root.c_str(), error.c_str());
        }
    }
#endif
    storage->cascCount_ = storage->sources_.size();

#if WHITEOUT_HAS_MPQ
    ProgressMonitor archives = archiveFiles_.empty() ? ProgressMonitor{} : m.Split(1);
    archives.Begin("Opening archives", archiveFiles_.size());
    for (std::string& file : archiveFiles_) {
        archives.Note(file);
        std::string error;
        if (auto src = MpqSource::Open(file, error))
            storage->sources_.push_back(std::move(src));
        else
            std::printf("[FileContentProvider] Failed to open %s: %s\n", file.c_str(),
                        error.c_str());
        archives.Worked();
    }
#endif
    return storage;
}

} // namespace whiteout::flakes::io
