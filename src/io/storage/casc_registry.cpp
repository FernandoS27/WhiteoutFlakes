#include "casc_registry.h"

#if WHITEOUT_HAS_CASC

#include "io/progress.h"
#include "whiteout/flakes/util/path_utf8.h"

#include <whiteout/utils/simple_thread_pool.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <thread>

namespace whiteout::flakes::io {

namespace fs = std::filesystem;
namespace casc = whiteout::storages::casc;

namespace {

// Two spellings of one install must not open it twice, and on Windows they
// routinely differ: a discovered root, a path typed into Settings and one
// dropped on the window can disagree on case, separators and `.` segments.
// Resolved against the filesystem rather than lexically so a symlinked or
// relative root normalises too.
std::string NormalizeRoot(const std::string& root) {
    std::error_code ec;
    fs::path p = fs::weakly_canonical(FsPathFromUtf8(root), ec);
    if (ec)
        p = FsPathFromUtf8(root);
    std::string out = PathToUtf8(p);
    std::replace(out.begin(), out.end(), '\\', '/');
    while (!out.empty() && out.back() == '/')
        out.pop_back();
#ifdef _WIN32
    // Case-insensitive filesystem, so case cannot be part of the identity.
    for (char& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
#endif
    return out;
}

// Read in chunks rather than with an istreambuf_iterator so it can report and
// be cancelled. A community listfile is ~90 MB, which is a visible pause of its
// own before the open the user is waiting for has even started — and the
// library cannot see this step, because the registry does it, not casc::Storage.
std::vector<u8> LoadListfile(const std::string& path, ProgressMonitor& mon) {
    if (path.empty())
        return {};
    std::ifstream f(FsPathFromUtf8(path), std::ios::binary | std::ios::ate);
    if (!f) {
        std::printf("[casc] listfile not readable: %s\n", path.c_str());
        return {};
    }
    const std::streamoff size = f.tellg();
    f.seekg(0);
    if (size <= 0)
        return {};

    mon.Begin("Reading listfile");
    mon.Note(path);

    std::vector<u8> bytes(static_cast<usize>(size));
    constexpr std::streamsize kChunk = 4 << 20;
    std::streamoff done = 0;
    while (done < size) {
        const std::streamsize n = (std::min)(kChunk, static_cast<std::streamsize>(size - done));
        if (!f.read(reinterpret_cast<char*>(bytes.data() + done), n))
            break;
        done += n;
        mon.Bytes(static_cast<u64>(done), static_cast<u64>(size));
        if (mon.Cancelled())
            return {};
    }

    std::printf("[casc] listfile loaded: %s (%zu bytes)\n", path.c_str(), bytes.size());
    return bytes;
}

/// Shared by every open. CASC fans index parsing and BLTE decompression across
/// it, and holds a non-owning pointer — so it is owned by the entries rather
/// than by the registry, and outlives the last of them by construction. One
/// pool for the process rather than one per reader is the same argument as the
/// storage itself: the work is the install's, not the reader's.
std::shared_ptr<whiteout::utils::SimpleThreadPool> Pool() {
    // Its own lock, not the registry's: two *different* installs opening at
    // once hold different slot mutexes and would otherwise race here.
    static std::mutex mu;
    static std::weak_ptr<whiteout::utils::SimpleThreadPool> weak;
    std::lock_guard lk(mu);
    if (auto pool = weak.lock())
        return pool;
    // 2–4 threads: enough to overlap index parsing and large-file BLTE
    // decompression without oversubscribing the callers' own request pools.
    const unsigned hw = std::thread::hardware_concurrency();
    auto pool = std::make_shared<whiteout::utils::SimpleThreadPool>(
        std::clamp<unsigned>(hw ? hw : 4u, 2u, 4u));
    weak = pool;
    return pool;
}

/// The registry slot for one key. Present in the map from the moment a key is
/// claimed, so a second caller finds it and waits on `mu` instead of opening
/// the same install again — while a caller with a *different* key never
/// touches this mutex at all.
struct Slot {
    std::mutex mu;
    std::weak_ptr<const SharedCasc> storage;
};

std::mutex& RegistryMutex() {
    static std::mutex mu;
    return mu;
}

std::map<std::tuple<std::string, std::string, std::string, bool>, std::shared_ptr<Slot>>&
Registry() {
    static std::map<std::tuple<std::string, std::string, std::string, bool>, std::shared_ptr<Slot>>
        map;
    return map;
}

} // namespace

SharedCasc::SharedCasc(std::string root, std::shared_ptr<whiteout::utils::SimpleThreadPool> pool,
                       std::vector<u8> listfile, casc::Storage storage)
    : root_(std::move(root)), pool_(std::move(pool)), listfile_(std::move(listfile)),
      storage_(std::move(storage)) {}

SharedCasc::~SharedCasc() = default;

std::shared_ptr<const SharedCasc> AcquireSharedCasc(const CascOpenKey& key, std::string& error,
                                                    ProgressMonitor* progress) {
    if (key.root.empty()) {
        error = "no install path";
        return nullptr;
    }
    const std::string root = NormalizeRoot(key.root);

    std::shared_ptr<Slot> slot;
    {
        std::lock_guard lk(RegistryMutex());
        auto& entry = Registry()[std::make_tuple(root, key.listfilePath, key.tactKeyFile,
                                                 key.zeroFillEncrypted)];
        if (!entry)
            entry = std::make_shared<Slot>();
        slot = entry;
    }

    // The registry lock is not held across the open, which is seconds of index
    // and manifest parsing: one install opening must not stall a read of
    // another. Two callers of the *same* install do serialise here, which is
    // the point — the second gets the first's result.
    std::lock_guard lk(slot->mu);
    // Nothing was opened, so nothing is reported. The monitor's window closes
    // itself when the caller's scope ends, which is what keeps a cache hit from
    // leaving the bar parked partway through a step that never ran.
    if (auto existing = slot->storage.lock())
        return existing;

    ProgressMonitor inert;
    ProgressMonitor& m = progress ? *progress : inert;

    // Weighted, and only for the steps this open will actually run. Skipping a
    // step by not planning it is better than dropping one later: the bar never
    // reserves room for work that was never going to happen. The open itself
    // dominates by an order of magnitude, hence 90 of 100.
    const bool wantsListfile = !key.listfilePath.empty();
    const bool wantsKeys = !key.tactKeyFile.empty();
    m.Begin("Opening storage", 90 + (wantsListfile ? 8u : 0u) + (wantsKeys ? 2u : 0u));

    std::vector<u8> listfile;
    if (wantsListfile) {
        ProgressMonitor step = m.Split(8);
        listfile = LoadListfile(key.listfilePath, step);
    }
    if (m.Cancelled()) {
        error = "Open cancelled";
        return nullptr;
    }

    auto pool = Pool();

    casc::OpenOptions co;
    // The caller's spelling, not the normalised key: the key exists to decide
    // identity, and a status display wants the path a human typed.
    co.path = key.root;
    co.pool = pool.get();
    co.errorOut = &error;
    if (!listfile.empty())
        co.listfile = std::span<const u8>(listfile);

    ProgressMonitor openStep = m.Split(90);
    // The library has reported all of this since it was written; nothing has
    // ever asked for it. Its events are absolute (step index plus a fraction
    // within the step), which is exactly what Report takes. Returning false is
    // how a cancel reaches it — the open then fails with CascError::Cancelled.
    casc::ProgressCallback cb = [&openStep](const casc::ProgressInfo& info) {
        openStep.Report(casc::progressStepName(info.step), info.object,
                        static_cast<f64>(info.stepIndex) + info.stepFraction(),
                        static_cast<f64>(info.stepCount));
        return !openStep.Cancelled();
    };
    co.progressCallback = cb;

    auto storage = casc::Storage::open(co);
    if (!storage)
        return nullptr;

    // Before the first read, and not conditional on the file being there: a
    // storage with no keys still reads everything unencrypted, which is most
    // of an install.
    bool keys = false;
    if (wantsKeys) {
        ProgressMonitor step = m.Split(2);
        step.Begin("Importing TACT keys");
        step.Note(key.tactKeyFile);
        keys = storage->importKeysFromFile(key.tactKeyFile);
        if (!keys)
            std::printf("[casc] TACT keys not readable: %s\n", key.tactKeyFile.c_str());
    }
    storage->setZeroFillEncrypted(key.zeroFillEncrypted);

    std::shared_ptr<const SharedCasc> shared(
        new SharedCasc(key.root, std::move(pool), std::move(listfile), std::move(*storage)));
    slot->storage = shared;
    std::printf("[casc] storage opened: %s%s%s\n", key.root.c_str(),
                shared->HasListfile() ? " (with listfile)" : "", keys ? " (with TACT keys)" : "");
    return shared;
}

usize OpenCascCount() {
    std::lock_guard lk(RegistryMutex());
    usize n = 0;
    for (const auto& [key, slot] : Registry()) {
        (void)key;
        // Not under the slot's own lock: this counts what is open *now*, and a
        // slot mid-open is by definition not open yet.
        if (!slot->storage.expired())
            ++n;
    }
    return n;
}

} // namespace whiteout::flakes::io

#endif // WHITEOUT_HAS_CASC
