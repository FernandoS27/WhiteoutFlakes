// ============================================================================
// FileContentProvider — the async surface hosts read content through.
//
// What lives here: the request queue and its worker pool, the loose-file
// search path, the configuration a host can change, and the rule that a
// configuration change is paid for by the next read rather than at the moment
// it is made.
//
// What does not: anything that differs between games. Which storages a
// product opens, how it spells a path, whether it has archives or file ids at
// all — all of that is `storage/game_rules.cpp` and the sources under it. This
// class asks for a GameStorage and reads through it.
// ============================================================================

#include "file_resolver.h"
#include "io/file_content_provider.h"
#include "io/storage/game_rules.h"
#include "io/storage/install_locator.h"
#include "io/storage/storage_paths.h"
#include "whiteout/flakes/types.h"
#include "whiteout/flakes/util/path_utf8.h"

#if WHITEOUT_HAS_CASC
#include <whiteout/utils/simple_thread_pool.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <climits>
#include <unistd.h>
#elif defined(__APPLE__)
#include <climits>
#include <mach-o/dyld.h>
#endif

namespace whiteout::flakes::io {

namespace fs = std::filesystem;

// Returns the directory containing the running executable, or {} on failure.
// Used as a fallback search root for engine-shipped assets (shaders, etc.)
// that ship next to the binary rather than alongside the loaded model.
static fs::path DiscoverExecutableDirectory() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH * 4] = {};
    DWORD len = ::GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    if (len == 0 || len >= std::size(buf))
        return {};
    return fs::path(std::wstring(buf, buf + len)).parent_path();
#elif defined(__linux__)
    char buf[PATH_MAX] = {};
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0)
        return {};
    return fs::path(std::string(buf, static_cast<usize>(n))).parent_path();
#elif defined(__APPLE__)
    // _NSGetExecutablePath writes the path used to launch the process;
    // canonicalise via std::filesystem to resolve symlinks. When the
    // executable lives inside a .app bundle (`.../X.app/Contents/MacOS/X`)
    // the asset search root is Contents/Resources/ — that's where macOS
    // wants read-only ship-with-the-binary data (and where codesign won't
    // choke on our non-Mach-O `.bls` files). Detect that case by checking
    // for the `Contents/MacOS` suffix on the exe's parent.
    char buf[PATH_MAX] = {};
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0)
        return {};
    std::error_code ec;
    fs::path resolved = fs::canonical(fs::path(buf), ec);
    if (ec)
        resolved = fs::path(buf);
    fs::path dir = resolved.parent_path();
    if (dir.filename() == "MacOS" && dir.parent_path().filename() == "Contents")
        return dir.parent_path() / "Resources";
    return dir;
#else
    return {};
#endif
}

static bool ReadDiskFile(const fs::path& resolved, std::vector<u8>& outBytes) {
    if (resolved.empty())
        return false;
    std::ifstream file(resolved, std::ios::binary | std::ios::ate);
    if (!file)
        return false;
    auto size = file.tellg();
    if (size <= 0)
        return false;
    outBytes.resize(static_cast<usize>(size));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(outBytes.data()), size);
    return true;
}

// ---- Async machinery --------------------------------------------------------

struct PendingRequest {
    RequestId id = kInvalidRequestId;
    ContentRef ref;
    CompletionCallback cb;
};

struct CompletedRequest {
    RequestId id = kInvalidRequestId;
    CompletionCallback cb;
    RequestResult result;
};

struct FileContentProvider::Impl {
    // ---- Storage state (guarded by storageMu) ----
    // Worker threads hold a *shared* lock while reading — the underlying CASC
    // and MPQ readers are themselves thread-safe, so N workers can decode in
    // parallel. Reconfiguration takes the *exclusive* lock so a storage is
    // never swapped out from under an in-flight read. The mutex is mutable
    // because HasCasc() / HasMpq() are logically-const observers that still
    // need to synchronise with worker reads.
    mutable std::shared_mutex storageMu;

    InstallLocator installs;

    // One per product, and each one keeps whatever it opened. Switching games
    // — from the settings panel, or because a loaded model turned out to be
    // another game's — only moves `active`; the storage the previous game was
    // reading through stays open behind it, so coming back costs nothing.
    // Opening a CASC install parses its indices and encoding tables, and doing
    // that again for a game already visited is the expensive thing this
    // arrangement exists to stop.
    struct GameSlot {
        // What the host has asked for; `storage` is what that resolved to.
        StorageConfig config;
        std::unique_ptr<GameStorage> storage;
        // Config filled in from the install scan. Deferred per slot because
        // ScanArchives walks a World of Warcraft install's Data/ directory,
        // which is work for a product the host may never select.
        bool configured = false;
        // Config changed since `storage` was built, or it never was. The open
        // itself is deferred further still — to the first read that cannot be
        // answered without one.
        bool dirty = true;
    };
    ProductId active = ProductId::Wc3;

    std::string hotsInstallPath; // active Heroes root, for the Sc2 product

    // HD mod-overlay precedence flag, read by the Warcraft III CASC source on
    // every path it builds. Atomic so a render-thread setter doesn't race the
    // storage workers, and a member so the source can hold a pointer to it.
    std::atomic<bool> hdMode{false};

    FileResolver resolver;

#if WHITEOUT_HAS_CASC
    // Handed to every CASC source. CASC parallelises the slow part of opening
    // a Reforged install (index + encoding-table parsing) and also fans out
    // BLTE block decompression across it. Sources keep a *non-owning* pointer,
    // so the pool must outlive them — hence declared before `games`, which
    // owns them (members destruct in reverse declaration order).
    std::unique_ptr<whiteout::utils::SimpleThreadPool> cascPool;
#endif

    // Declared after the pool for the reason above.
    std::array<GameSlot, 4> games; // indexed by ProductId

    // ---- Request queue (guarded by reqMu) ----
    // `pending` is the worker's input. `alive` tracks every id that has been
    // submitted but neither delivered (via Pump) nor cancelled — Wait spins
    // on its absence. `cancelled` records ids the worker should skip.
    std::mutex reqMu;
    std::condition_variable reqCv;
    std::condition_variable doneCv; // signalled when an id leaves `alive`
    std::deque<std::shared_ptr<PendingRequest>> pending;
    std::unordered_set<RequestId> alive;
    std::unordered_set<RequestId> cancelled;
    std::atomic<RequestId> nextId{1};
    std::atomic<bool> stopping{false};

    // The first thread to call Pump() (or Wait()) is treated as the host's
    // Pump thread — callbacks fire here, and a Wait() on this thread runs
    // Pump in a loop so a single-threaded host doesn't deadlock during init
    // (sync ReadFile before the per-frame Pump loop has started). Other
    // threads that call Wait block on doneCv instead, letting the Pump
    // thread deliver callbacks for them.
    std::atomic<std::thread::id> pumpThread{};

    // ---- Completion queue (guarded by compMu) ----
    // Worker pushes; Pump (on the host thread) drains.
    std::mutex compMu;
    std::deque<CompletedRequest> completed;

    std::vector<std::thread> workers;

    // ----------------------------------------------------------------

    Impl() {
        hotsInstallPath = installs.Hots();
        Configure(ProductId::Wc3);
        // Nothing is opened here: every slot starts dirty, so the first read
        // that needs one (or an observer that has to know) opens it. A scene
        // that is constructed and never read from costs nothing.
    }

    GameSlot& Slot() {
        return games[static_cast<usize>(active)];
    }
    const GameSlot& Slot() const {
        return games[static_cast<usize>(active)];
    }

    // Fill in a slot's configuration from what is installed. Caller holds the
    // exclusive lock (or is the constructor, where nothing else can see this).
    void Configure(ProductId game) {
        GameSlot& s = games[static_cast<usize>(game)];
        if (s.configured)
            return;
        s.config.game = game;
        s.config.installPath = installs.PathFor(game);
        // The archive list belongs to the product: scanned for World of
        // Warcraft, whose names move with the expansion; fixed for Warcraft
        // III; empty for StarCraft II and Heroes, which never shipped one.
        s.config.archives = ScanArchives(game, s.config.installPath);
        s.configured = true;
    }

    // What a settings change costs: the storage that configuration opened is
    // closed now rather than left to be replaced on the next read. Nothing can
    // read through it again — the config it answered for is gone — so holding
    // an open CASC install for it would only cost memory. Caller holds the
    // exclusive lock.
    void Invalidate(ProductId game) {
        GameSlot& s = games[static_cast<usize>(game)];
        s.dirty = true;
        s.storage.reset();
    }

    // Realise the active slot's deferred build. Call WITHOUT storageMu held —
    // it takes the lock itself, shared for the common "already built" check
    // and exclusive only for the one call that does the work.
    void EnsureStorage() {
        {
            std::shared_lock sg(storageMu);
            if (!Slot().dirty)
                return;
        }
        std::unique_lock sg(storageMu);
        GameSlot& s = Slot();
        if (!s.dirty) // another thread got here first
            return;
        s.dirty = false;
#if WHITEOUT_HAS_CASC
        if (!cascPool) {
            // 2–4 threads: enough to overlap index parsing and large-file
            // BLTE decompression without oversubscribing the request pool.
            const unsigned hw = std::thread::hardware_concurrency();
            cascPool = std::make_unique<whiteout::utils::SimpleThreadPool>(
                std::clamp<unsigned>(hw ? hw : 4u, 2u, 4u));
        }
        auto* pool = cascPool.get();
#else
        whiteout::utils::SimpleThreadPool* pool = nullptr;
#endif
        StorageConfig cfg = s.config;
        cfg.secondaryPath = (cfg.game == ProductId::Sc2) ? hotsInstallPath : std::string{};
        // Replaced wholesale rather than mutated: a storage set is only ever
        // consistent as a whole, and the old one is dropped only once the new
        // one exists.
        s.storage = BuildGameStorage(cfg, pool, &hdMode);
    }

    void WorkerLoop() {
        while (true) {
            std::shared_ptr<PendingRequest> req;
            {
                std::unique_lock lk(reqMu);
                reqCv.wait(lk, [&] { return stopping.load() || !pending.empty(); });
                if (stopping.load() && pending.empty())
                    return;
                req = std::move(pending.front());
                pending.pop_front();
                // Skip if cancelled before we got to it.
                if (cancelled.erase(req->id)) {
                    alive.erase(req->id);
                    doneCv.notify_all(); // unblock any Wait() pinned on this id
                    continue;
                }
            }

            // A read that cannot be answered any other way is what "actually
            // needed" means: this is where a deferred build is paid for, on a
            // worker thread rather than on whichever thread last touched a
            // setting. The shared lock lets any number of workers read
            // concurrently; only reconfiguration takes the exclusive one.
            RequestResult result;
            if (req->ref.IsFileId()) {
                // A fileDataID names a file in a CASC root manifest and
                // nowhere else, so there is nothing to try first.
                EnsureStorage();
                std::shared_lock sg(storageMu);
                ReadFileId(req->ref.fileId, result);
            } else {
                {
                    std::shared_lock sg(storageMu);
                    ReadDisk(req->ref.path, result);
                }
                // Only a disk miss is worth opening an install for. Every read
                // this process makes before content is loaded is an
                // engine-shipped asset sitting beside the executable — the BLS
                // shader pack, the PSO trace — and they all land above.
                if (!result.ok) {
                    EnsureStorage();
                    std::shared_lock sg(storageMu);
                    ReadStorage(req->ref.path, result);
                }
            }

            // Re-check cancellation between IO and delivery so a Cancel that
            // races the read still suppresses the callback.
            {
                std::lock_guard lk(reqMu);
                if (cancelled.erase(req->id)) {
                    alive.erase(req->id);
                    doneCv.notify_all();
                    continue;
                }
            }

            {
                std::lock_guard lk(compMu);
                CompletedRequest c;
                c.id = req->id;
                c.cb = std::move(req->cb);
                c.result = std::move(result);
                completed.push_back(std::move(c));
            }
            // Wake any Wait() that's blocking — Pump on the host thread will
            // erase from alive_ and re-notify, but the wake here also lets a
            // Wait that's actively pumping notice the new completion sooner.
            doneCv.notify_all();
        }
    }

    // A fileDataID names a file in a CASC root manifest and nowhere else —
    // there is no disk fallback, no archive fallback, and no extension to
    // probe. A miss is a miss.
    void ReadFileId(u32 fileId, RequestResult& out) const {
        const auto& storage = Slot().storage;
        if (!storage)
            return;
        SourceRead hit;
        if (!storage->ReadById(fileId, hit))
            return;
        out.data = std::move(hit.data);
        out.actualExt = std::move(hit.actualExt);
        out.ok = true;
    }

    // The loose-file half of a path read, and the half that needs no storage
    // open. Disk shadows the archives so a host can drop a modified asset next
    // to the model and have it win, which is the whole point of the base path.
    void ReadDisk(const std::string& path, RequestResult& out) const {
        const std::string norm = FileResolver::NormalizeSeparators(path);
        const std::string ext = GetLowerExtension(norm);
        fs::path resolved;
        switch (ClassifyByExtension(ext)) {
        case AssetKind::Texture:
            resolved = resolver.ResolveTexture(norm);
            break;
        case AssetKind::Model:
            resolved = resolver.ResolveModel(norm);
            break;
        case AssetKind::Other:
            resolved = resolver.Resolve(norm, {});
            break;
        }

        std::vector<u8> bytes;
        if (ReadDiskFile(resolved, bytes)) {
            std::string e = resolved.extension().string();
            for (auto& c : e)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            out.actualExt = std::move(e);
            out.data = std::move(bytes);
            out.ok = true;
        }
    }

    // The archive half, run only when ReadDisk missed.
    void ReadStorage(const std::string& path, RequestResult& out) const {
        const auto& storage = Slot().storage;
        if (!storage)
            return;
        SourceRead hit;
        if (!storage->Read(path, hit))
            return;
        out.data = std::move(hit.data);
        out.actualExt = std::move(hit.actualExt);
        out.ok = true;
    }
};

FileContentProvider::FileContentProvider() : impl_(std::make_unique<Impl>()) {
    const fs::path exeDir = DiscoverExecutableDirectory();
    if (!exeDir.empty()) {
        impl_->resolver.SetSystemBasePath(exeDir);
        std::printf("[FileContentProvider] Executable dir: %s\n", PathToUtf8(exeDir).c_str());
    }

#if !defined(__EMSCRIPTEN__)
    // IO worker pool. Reads mix disk latency with CASC block decompression
    // (CPU-bound), so scaling with cores genuinely helps bulk texture loads;
    // capped at 8 to avoid pointless oversubscription on big machines.
    // Skipped on the web — the SceneManager-owned FileContentProvider
    // member is never used (FetchContentProvider takes its place), so
    // spawning threads under single-threaded WASM would trap for nothing.
    unsigned hw = std::thread::hardware_concurrency();
    const unsigned workerCount = std::clamp<unsigned>(hw ? hw : 4u, 2u, 8u);
    impl_->workers.reserve(workerCount);
    for (unsigned i = 0; i < workerCount; ++i)
        impl_->workers.emplace_back([this] { impl_->WorkerLoop(); });
#endif
}

FileContentProvider::~FileContentProvider() {
    if (!impl_)
        return;
    {
        std::lock_guard lk(impl_->reqMu);
        impl_->stopping.store(true);
    }
    impl_->reqCv.notify_all();
    for (auto& w : impl_->workers) {
        if (w.joinable())
            w.join();
    }
}

FileContentProvider::FileContentProvider(FileContentProvider&&) noexcept = default;
FileContentProvider& FileContentProvider::operator=(FileContentProvider&&) noexcept = default;

void FileContentProvider::SetBasePath(const std::filesystem::path& basePath) {
    std::unique_lock sg(impl_->storageMu);
    impl_->resolver.SetBasePath(basePath);
}

void FileContentProvider::SetSystemBasePath(const std::filesystem::path& root) {
    std::unique_lock sg(impl_->storageMu);
    impl_->resolver.SetSystemBasePath(root);
}

// ---- Async surface ----------------------------------------------------------

RequestId FileContentProvider::Request(const ContentRef& ref, CompletionCallback cb) {
    if (ref.Empty() || !cb)
        return kInvalidRequestId;
#if defined(__EMSCRIPTEN__)
    // Web build: no worker pool exists (see ctor). Pushing onto `pending`
    // would queue a request no one consumes, and a subsequent Wait()
    // would deadlock on doneCv. Synthesise an immediate "not found"
    // completion so Wait/Pump retire the request cleanly. The web host
    // installs a FetchContentProvider via SceneView::SetContentProvider
    // for real data; this stub only fires for code paths reached before
    // that swap-in (e.g. RenderPipeline::InitBlsShaders during startup).
    const RequestId id = impl_->nextId.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard lk(impl_->reqMu);
        impl_->alive.insert(id);
    }
    {
        std::lock_guard lk(impl_->compMu);
        impl_->completed.push_back(CompletedRequest{id, std::move(cb), RequestResult{}});
    }
    impl_->doneCv.notify_all();
    return id;
#else
    auto req = std::make_shared<PendingRequest>();
    req->id = impl_->nextId.fetch_add(1, std::memory_order_relaxed);
    req->ref = ref;
    req->cb = std::move(cb);
    {
        std::lock_guard lk(impl_->reqMu);
        impl_->pending.push_back(req);
        impl_->alive.insert(req->id);
    }
    impl_->reqCv.notify_one();
    return req->id;
#endif
}

void FileContentProvider::Cancel(RequestId id) {
    if (id == kInvalidRequestId)
        return;
    {
        std::lock_guard lk(impl_->reqMu);
        if (impl_->alive.erase(id))
            impl_->cancelled.insert(id);
    }
    impl_->doneCv.notify_all();
}

void FileContentProvider::Pump() {
    // First Pump() caller becomes the registered Pump thread — see Wait()
    // for why this matters. compare_exchange leaves a previously-set value
    // alone.
    std::thread::id expected{};
    impl_->pumpThread.compare_exchange_strong(expected, std::this_thread::get_id());

    std::deque<CompletedRequest> batch;
    {
        std::lock_guard lk(impl_->compMu);
        batch.swap(impl_->completed);
    }
    for (auto& c : batch) {
        // Late-cancel race: if Cancel was called between the worker queueing
        // this completion and us draining the batch, suppress the callback.
        // cancelled.erase() also tidies the set so it doesn't leak entries.
        bool fire = true;
        {
            std::lock_guard lk(impl_->reqMu);
            if (impl_->cancelled.erase(c.id))
                fire = false;
            impl_->alive.erase(c.id);
        }
        if (fire && c.cb)
            c.cb(std::move(c.result));
    }
    impl_->doneCv.notify_all();
}

void FileContentProvider::Wait(RequestId id) {
    if (id == kInvalidRequestId)
        return;

    // Treat the calling thread as the Pump thread when none has registered
    // yet — covers init-time sync ReadFile() before the host's per-frame
    // Pump loop has started.
    std::thread::id expected{};
    impl_->pumpThread.compare_exchange_strong(expected, std::this_thread::get_id());

    const auto myThread = std::this_thread::get_id();
    if (impl_->pumpThread.load() == myThread) {
        // We're the Pump thread — drain completions ourselves while the
        // request is in flight. Callbacks fire on this thread (which is the
        // host's main thread by construction).
        while (true) {
            Pump();
            std::unique_lock lk(impl_->reqMu);
            if (impl_->alive.count(id) == 0)
                return;
            // 10ms cap is a defensive backstop in case a notify is lost —
            // under normal operation the worker wakes us exactly on
            // completion.
            impl_->doneCv.wait_for(lk, std::chrono::milliseconds(10),
                                   [&] { return impl_->alive.count(id) == 0; });
            if (impl_->alive.count(id) == 0)
                return;
        }
    }

    // Some other thread (background loader, etc.) is the Pump thread. Just
    // block on doneCv — Pump on that thread will erase from `alive` and
    // signal. This is the path that lets ModelTemplateManager::LoaderFunc
    // sync-read off the worker without firing main-thread callbacks
    // (texture stubs, etc.) on the wrong thread.
    std::unique_lock lk(impl_->reqMu);
    impl_->doneCv.wait(lk, [&] { return impl_->alive.count(id) == 0; });
}

std::vector<std::string> FileContentProvider::ListFiles(const std::string& directory,
                                                        bool recursive) {
    impl_->EnsureStorage();
    const std::string dir = NormalizeListingDir(directory);

    // Ordered + deduped: the same logical file usually exists under several
    // CASC mod prefixes, and disk copies shadow archive ones.
    std::set<std::string> out;
    auto consider = [&](std::string relPath) {
        if (MatchesListingDir(relPath, dir, recursive))
            out.insert(std::move(relPath));
    };

    std::shared_lock sg(impl_->storageMu);

    // ---- Disk (the base path a host points at a loose asset tree) ----
    const fs::path base = impl_->resolver.BasePath();
    if (!base.empty()) {
        const fs::path root = dir.empty() ? base : base / FsPathFromUtf8(dir);
        std::error_code ec;
        if (fs::is_directory(root, ec)) {
            auto add = [&](const fs::path& p) {
                std::error_code re;
                const std::string rel = PathToUtf8(fs::relative(p, base, re));
                if (!re && !rel.empty())
                    consider(ToListingPath(rel));
            };
            // Skip-on-error so one unreadable subdirectory doesn't abort the
            // walk, as StorageBrowser::OpenFolder does.
            if (recursive) {
                fs::recursive_directory_iterator it(
                    root, fs::directory_options::skip_permission_denied, ec);
                if (!ec) {
                    for (const auto& entry : it) {
                        std::error_code fe;
                        if (entry.is_regular_file(fe) && !fe)
                            add(entry.path());
                    }
                }
            } else {
                fs::directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
                if (!ec) {
                    for (const auto& entry : it) {
                        std::error_code fe;
                        if (entry.is_regular_file(fe) && !fe)
                            add(entry.path());
                    }
                }
            }
        }
    }

    // ---- Archives ----
    if (const auto& storage = impl_->Slot().storage)
        storage->List(consider);

    return {out.begin(), out.end()};
}

// ---- Storage observers / configuration --------------------------------------

bool FileContentProvider::HasCasc() const {
    // "Is a storage open" cannot be answered without opening it. Callers that
    // only want to know whether one is *pending* ask StoragesPending().
    impl_->EnsureStorage();
    std::shared_lock sg(impl_->storageMu);
    const auto& storage = impl_->Slot().storage;
    return storage && storage->HasCasc();
}

bool FileContentProvider::HasMpq() const {
    impl_->EnsureStorage();
    std::shared_lock sg(impl_->storageMu);
    const auto& storage = impl_->Slot().storage;
    return storage && storage->HasArchives();
}

bool FileContentProvider::StoragesPending() const {
    std::shared_lock sg(impl_->storageMu);
    return impl_->Slot().dirty;
}

const std::string& FileContentProvider::Wc3Path() const {
    // Discovered once at construction and never written again; safe to return
    // by reference without taking storageMu.
    return impl_->installs.Wc3();
}

const std::string& FileContentProvider::HotsPath() const {
    return impl_->installs.Hots();
}

std::string FileContentProvider::HotsInstallPath() const {
    std::shared_lock sg(impl_->storageMu);
    return impl_->hotsInstallPath;
}

void FileContentProvider::SetHotsInstallPath(const std::string& path) {
    std::unique_lock sg(impl_->storageMu);
    const std::string next = path.empty() ? impl_->installs.Hots() : path;
    if (impl_->hotsInstallPath == next)
        return;
    impl_->hotsInstallPath = next;
    // Only the Sc2 product reads this root, so for anything else the change
    // cannot invalidate what is open.
    impl_->Invalidate(ProductId::Sc2);
}

std::vector<std::string> FileContentProvider::OpenCascRoots() const {
    impl_->EnsureStorage();
    std::shared_lock sg(impl_->storageMu);
    const auto& storage = impl_->Slot().storage;
    return storage ? storage->CascRoots() : std::vector<std::string>{};
}

std::string FileContentProvider::GamePath(ProductId game) const {
    return impl_->installs.PathFor(game);
}

ProductId FileContentProvider::Game() const {
    std::shared_lock sg(impl_->storageMu);
    return impl_->active;
}

void FileContentProvider::SetGame(ProductId game) {
    std::unique_lock sg(impl_->storageMu);
    if (impl_->active == game)
        return;
    // Nothing is invalidated and nothing is opened. Each product's install
    // root, archive list and open storage live in its own slot, so switching
    // is a pointer move: the game being left keeps what it had open for when
    // the host comes back, and the one being entered opens on its first read.
    impl_->Configure(game);
    impl_->active = game;
}

void FileContentProvider::SetListfilePath(const std::filesystem::path& csv) {
    std::unique_lock sg(impl_->storageMu);
    auto& s = impl_->Slot();
    std::string next = PathToUtf8(csv);
    if (s.config.listfilePath == next)
        return;
    s.config.listfilePath = std::move(next);
    impl_->Invalidate(s.config.game);
}

std::string FileContentProvider::ListfilePath() const {
    std::shared_lock sg(impl_->storageMu);
    return impl_->Slot().config.listfilePath;
}

bool FileContentProvider::HasListfile() const {
    std::shared_lock sg(impl_->storageMu);
    const auto& storage = impl_->Slot().storage;
    return storage && storage->HasListfile();
}

std::vector<std::string> FileContentProvider::ScanMpqList() const {
    std::shared_lock sg(impl_->storageMu);
    const auto& cfg = impl_->Slot().config;
    return ScanArchives(cfg.game, cfg.installPath);
}

std::string FileContentProvider::InstallPath() const {
    std::shared_lock sg(impl_->storageMu);
    return impl_->Slot().config.installPath;
}

void FileContentProvider::SetInstallPath(const std::string& path) {
    std::unique_lock sg(impl_->storageMu);
    auto& s = impl_->Slot();
    // Empty reverts to the active product's discovered root, not always
    // Warcraft III's — otherwise clearing the override on a WoW scene would
    // silently point it at a WC3 install.
    const std::string next = path.empty() ? impl_->installs.PathFor(s.config.game) : path;
    if (s.config.installPath == next)
        return;
    s.config.installPath = next;
    impl_->Invalidate(s.config.game);
}

bool FileContentProvider::IgnoreCasc() const {
    std::shared_lock sg(impl_->storageMu);
    return impl_->Slot().config.ignoreCasc;
}

bool FileContentProvider::IgnoreMpq() const {
    std::shared_lock sg(impl_->storageMu);
    return impl_->Slot().config.ignoreArchives;
}

void FileContentProvider::SetHdMode(bool enabled) {
    impl_->hdMode.store(enabled, std::memory_order_relaxed);
}

bool FileContentProvider::HdMode() const {
    return impl_->hdMode.load(std::memory_order_relaxed);
}

void FileContentProvider::SetIgnoreCasc(bool ignore) {
    std::unique_lock sg(impl_->storageMu);
    auto& s = impl_->Slot();
    if (s.config.ignoreCasc == ignore)
        return;
    s.config.ignoreCasc = ignore;
    impl_->Invalidate(s.config.game);
}

void FileContentProvider::SetIgnoreMpq(bool ignore) {
    std::unique_lock sg(impl_->storageMu);
    auto& s = impl_->Slot();
    if (s.config.ignoreArchives == ignore)
        return;
    s.config.ignoreArchives = ignore;
    impl_->Invalidate(s.config.game);
}

std::vector<std::string> FileContentProvider::MpqList() const {
    std::shared_lock sg(impl_->storageMu);
    return impl_->Slot().config.archives;
}

void FileContentProvider::SetMpqList(std::vector<std::string> list) {
    std::unique_lock sg(impl_->storageMu);
    auto& s = impl_->Slot();
    if (s.config.archives == list)
        return;
    s.config.archives = std::move(list);
    impl_->Invalidate(s.config.game);
}

std::vector<std::string> FileContentProvider::DefaultMpqList() {
    return DefaultArchives(ProductId::Wc3);
}

std::vector<std::string> FileContentProvider::DefaultMpqList(ProductId game) {
    return DefaultArchives(game);
}

} // namespace whiteout::flakes::io
