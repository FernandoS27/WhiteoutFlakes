#include "mpq_source.h"

#if WHITEOUT_HAS_MPQ

#include "storage_paths.h"

#include <cstdio>

namespace whiteout::flakes::io {

namespace mpq = whiteout::storages::mpq;

MpqSource::MpqSource(std::string file, mpq::Storage storage)
    : file_(std::move(file)), storage_(std::move(storage)) {}

std::unique_ptr<MpqSource> MpqSource::Open(std::string file, std::string& error) {
    auto storage = mpq::Storage::open(file, &error);
    if (!storage)
        return nullptr;
    std::printf("[FileContentProvider] Opened MPQ: %s\n", file.c_str());
    return std::unique_ptr<MpqSource>(new MpqSource(std::move(file), std::move(*storage)));
}

bool MpqSource::Read(const std::string& path, SourceRead& out) const {
    const std::string ext = GetLowerExtension(path);
    auto [altExts, altCount] = AltExtensionsFor(ext);

    // The as-asked path goes in verbatim: an archive stores whatever spelling
    // its author used, and the reader matches case-insensitively itself.
    if (!ext.empty()) {
        auto data = storage_.readFile(path);
        if (data && !data->empty()) {
            out.actualExt = ext;
            out.data = std::move(*data);
            return true;
        }
    }
    const std::string stem = StripExtension(path);
    for (usize i = 0; i < altCount; ++i) {
        if (altExts[i] == ext)
            continue;
        auto data = storage_.readFile(stem + altExts[i]);
        if (data && !data->empty()) {
            out.actualExt = altExts[i];
            out.data = std::move(*data);
            return true;
        }
    }
    return false;
}

void MpqSource::List(const std::function<void(std::string)>& emit) const {
    for (auto& name : storage_.listFiles())
        emit(ToListingPath(name));
}

} // namespace whiteout::flakes::io

#endif // WHITEOUT_HAS_MPQ
