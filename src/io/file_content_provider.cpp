#include "file_resolver.h"
#include "io/file_content_provider.h"
#include "whiteout/flakes/types.h"
#include "whiteout/flakes/util/path_utf8.h"

#include <whiteout/utils/blizzard_game_finder.h>

#if WHITEOUT_HAS_CASC
#include <whiteout/storages/casc/storage.h>
#include <whiteout/utils/simple_thread_pool.h>
#endif

#if WHITEOUT_HAS_MPQ
#include <whiteout/storages/mpq/storage.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
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

// Stock load order for Warcraft III's MPQs — patch first so its overrides
// win, then the expansion, then the base game. Surfaced via DefaultMpqList()
// so the UI can offer "reset" without re-deriving the list itself.
static const char* const kDefaultMpqNames[] = {
    "War3Patch.mpq",
    "War3x.mpq",
    "war3.mpq",
};

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
    if (dir.filename() == "MacOS" && dir.parent_path().filename() == "Contents") {
        return dir.parent_path() / "Resources";
    }
    return dir;
#else
    return {};
#endif
}

// ---- File-extension classification helpers ----------------------------------

static constexpr const char* kTextureExts[] = {".blp", ".dds", ".tga", ".png", ".tif"};
static constexpr const char* kModelExts[] = {".mdx", ".mdl"};
static constexpr const char* kArchiveTextureExts[] = {".blp", ".dds", ".tga", ".png"};
static constexpr const char* kArchiveModelExts[] = {".mdx", ".mdl"};
static constexpr const char* kCascPrefixes[] = {
    "war3.w3mod:",
    "war3.w3mod:_hd.w3mod:",
    "war3.w3mod:_deprecated.w3mod:",
};

static bool HasExtension(const std::string& ext, const char* const* list, usize count) {
    for (usize i = 0; i < count; ++i)
        if (ext == list[i])
            return true;
    return false;
}

static std::string NormalizeCascPath(const std::string& relPath) {
    std::string out;
    out.reserve(relPath.size());
    for (char c : relPath) {
        if (c == '/')
            c = '\\';
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    usize start = 0;
    while (start < out.size() && (out[start] == '\\' || out[start] == '/'))
        ++start;
    if (start > 0)
        out.erase(0, start);
    return out;
}

static std::string StripExtension(const std::string& path) {
    auto dot = path.rfind('.');
    return (dot != std::string::npos) ? path.substr(0, dot) : path;
}

static std::string GetLowerExtension(const std::string& relPath) {
    std::string ext = fs::path(relPath).extension().string();
    for (auto& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

static std::pair<const char* const*, usize> AltExtensionsFor(const std::string& ext) {
    if (HasExtension(ext, kTextureExts, std::size(kTextureExts)))
        return {kArchiveTextureExts, std::size(kArchiveTextureExts)};
    if (HasExtension(ext, kModelExts, std::size(kModelExts)))
        return {kArchiveModelExts, std::size(kArchiveModelExts)};
    return {nullptr, 0};
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
    std::string path;
    CompletionCallback cb;
};

struct CompletedRequest {
    RequestId id = kInvalidRequestId;
    CompletionCallback cb;
    RequestResult result;
};

struct FileContentProvider::Impl {
    // ---- Storage state (guarded by storageMu) ----
    // Worker threads hold a *shared* lock while reading — CASC and MPQ
    // readFile() are themselves thread-safe (shared-lock internally), so N
    // workers can decode in parallel. Reconfiguration paths (SetInstallPath /
    // SetIgnoreCasc / SetMpqList) take the *exclusive* lock so a storage
    // handle is never swapped out from under an in-flight read. The mutex is
    // mutable because HasCasc() / HasMpq() are logically-const observers that
    // still need to synchronise with worker reads.
    mutable std::shared_mutex storageMu;
    std::string wc3Path;     // auto-detected; immutable after Discover()
    std::string installPath; // currently-active install root
    bool ignoreCasc = false;
    bool ignoreMpq = false;
    std::vector<std::string> mpqList = FileContentProvider::DefaultMpqList();
    FileResolver resolver;

#if WHITEOUT_HAS_CASC
    // Worker pool handed to casc::Storage::open(). CASC parallelises the slow
    // part of opening a Reforged install (index + encoding-table parsing) and
    // also fans out BLTE block decompression of large files across it. The
    // storage keeps a *non-owning* pointer, so the pool must outlive it —
    // hence declared before cascStorage (members destruct in reverse order).
    // Created lazily on first CASC open so MPQ-only installs spawn no idle
    // threads.
    std::unique_ptr<whiteout::utils::SimpleThreadPool> cascPool;
    std::optional<whiteout::storages::casc::Storage> cascStorage;
#endif

#if WHITEOUT_HAS_MPQ
    std::vector<whiteout::storages::mpq::Storage> mpqStorages;
#endif

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

    void Discover() {
#if defined(__EMSCRIPTEN__)
        // Web build: no native install discovery — the host swaps in a
        // FetchContentProvider before any request runs. This member exists
        // only so SceneManager's `FileContentProvider contentProvider_`
        // member constructs; its Request() path is never reached.
        wc3Path.clear();
        installPath.clear();
#else
        auto games = whiteout::utils::findBlizzardGames();

        std::string fallbackPath;
        for (auto& info : games) {
            if (info.game != whiteout::utils::BlizzardGame::WarcraftIII &&
                info.game != whiteout::utils::BlizzardGame::WarcraftIIIReforged)
                continue;
            if (fs::exists(FsPathFromUtf8(info.path) / "Data")) {
                wc3Path = info.path;
                break;
            }
            if (fallbackPath.empty())
                fallbackPath = info.path;
        }
        if (wc3Path.empty())
            wc3Path = std::move(fallbackPath);

        installPath = wc3Path;

        if (wc3Path.empty()) {
            std::printf("[FileContentProvider] Warcraft III installation not found.\n");
            return;
        }

        std::printf("[FileContentProvider] Found Warcraft III at: %s\n", wc3Path.c_str());

        TryOpenCascLocked();
        TryOpenMpqLocked();
#endif
    }

    void TryOpenCascLocked() {
#if WHITEOUT_HAS_CASC
        cascStorage.reset();
        if (ignoreCasc || installPath.empty())
            return;
        if (!cascPool) {
            // 2–4 threads: enough to overlap index parsing and large-file
            // BLTE decompression without oversubscribing the request pool.
            const unsigned hw = std::thread::hardware_concurrency();
            cascPool = std::make_unique<whiteout::utils::SimpleThreadPool>(
                std::clamp<unsigned>(hw ? hw : 4u, 2u, 4u));
        }
        std::string error;
        cascStorage = whiteout::storages::casc::Storage::open(installPath, &error, cascPool.get());
        if (cascStorage)
            std::printf("[FileContentProvider] CASC storage opened: %s\n", installPath.c_str());
        else {
            std::printf("[FileContentProvider] CASC not available at '%s': %s\n",
                        installPath.c_str(), error.c_str());
            cascStorage.reset();
        }
#endif
    }

    void TryOpenMpqLocked() {
#if WHITEOUT_HAS_MPQ
        mpqStorages.clear();
        if (ignoreMpq || installPath.empty())
            return;
        for (const std::string& name : mpqList) {
            if (name.empty())
                continue;
            fs::path mpqFsPath = FsPathFromUtf8(installPath) / name;
            if (!fs::exists(mpqFsPath)) {
                std::printf("[FileContentProvider] MPQ not found, skipping: %s\n",
                            PathToUtf8(mpqFsPath).c_str());
                continue;
            }
            std::string error;
            auto storage = whiteout::storages::mpq::Storage::open(PathToUtf8(mpqFsPath), &error);
            if (storage) {
                std::printf("[FileContentProvider] Opened MPQ: %s\n",
                            PathToUtf8(mpqFsPath).c_str());
                mpqStorages.push_back(std::move(*storage));
            } else {
                std::printf("[FileContentProvider] Failed to open %s: %s\n",
                            PathToUtf8(mpqFsPath).c_str(), error.c_str());
            }
        }
#endif
    }

    // Disk-then-CASC-then-MPQ read, run on the worker thread with storageMu
    // already held. Mirrors the legacy synchronous ReadFile fallback chain.
    void DoRead(const std::string& path, RequestResult& out) {
        // ---- Disk ----
        std::string norm = FileResolver::NormalizeSeparators(path);
        std::string ext = GetLowerExtension(norm);
        fs::path resolved;
        if (HasExtension(ext, kTextureExts, std::size(kTextureExts)))
            resolved = resolver.ResolveTexture(norm);
        else if (HasExtension(ext, kModelExts, std::size(kModelExts)))
            resolved = resolver.ResolveModel(norm);
        else
            resolved = resolver.Resolve(norm, {});

        std::vector<u8> bytes;
        if (ReadDiskFile(resolved, bytes)) {
            std::string e = resolved.extension().string();
            for (auto& c : e)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            out.actualExt = std::move(e);
            out.data = std::move(bytes);
            out.ok = true;
            return;
        }

        // ---- CASC ----
#if WHITEOUT_HAS_CASC
        if (cascStorage) {
            std::string normCasc = NormalizeCascPath(path);
            std::string stem = StripExtension(normCasc);
            std::string cascExt = GetLowerExtension(normCasc);
            auto [altExts, altCount] = AltExtensionsFor(cascExt);
            for (const char* prefix : kCascPrefixes) {
                if (!cascExt.empty()) {
                    std::string cascPath = std::string(prefix) + stem + cascExt;
                    auto data = cascStorage->readFile(cascPath);
                    if (data && !data->empty()) {
                        out.actualExt = cascExt;
                        out.data = std::move(*data);
                        out.ok = true;
                        return;
                    }
                }
                for (usize i = 0; i < altCount; ++i) {
                    if (altExts[i] == cascExt)
                        continue;
                    std::string cascPath = std::string(prefix) + stem + altExts[i];
                    auto data = cascStorage->readFile(cascPath);
                    if (data && !data->empty()) {
                        out.actualExt = altExts[i];
                        out.data = std::move(*data);
                        out.ok = true;
                        return;
                    }
                }
            }
        }
#endif

        // ---- MPQ ----
#if WHITEOUT_HAS_MPQ
        if (!mpqStorages.empty()) {
            std::string mpqExt = GetLowerExtension(path);
            std::string stem = StripExtension(path);
            auto [altExts, altCount] = AltExtensionsFor(mpqExt);
            for (const auto& mpq : mpqStorages) {
                if (!mpqExt.empty()) {
                    auto data = mpq.readFile(path);
                    if (data && !data->empty()) {
                        out.actualExt = mpqExt;
                        out.data = std::move(*data);
                        out.ok = true;
                        return;
                    }
                }
                for (usize i = 0; i < altCount; ++i) {
                    if (altExts[i] == mpqExt)
                        continue;
                    auto data = mpq.readFile(stem + altExts[i]);
                    if (data && !data->empty()) {
                        out.actualExt = altExts[i];
                        out.data = std::move(*data);
                        out.ok = true;
                        return;
                    }
                }
            }
        }
#endif
        // Miss — ok stays false; data + actualExt stay empty.
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

            RequestResult result;
            {
                // Shared lock: any number of workers may read concurrently;
                // only reconfiguration takes the exclusive lock.
                std::shared_lock sg(storageMu);
                DoRead(req->path, result);
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
};

FileContentProvider::FileContentProvider() : impl_(std::make_unique<Impl>()) {
    // Discover runs storage IO directly; safe to call before the workers
    // exist because no requests can be in flight yet.
    impl_->Discover();

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

// ---- Async surface ----------------------------------------------------------

RequestId FileContentProvider::Request(const std::string& path, CompletionCallback cb) {
    if (path.empty() || !cb)
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
    req->path = path;
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
            // completion (it signals doneCv after pushing).
            impl_->doneCv.wait_for(lk, std::chrono::milliseconds(10));
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

// ---- Storage observers / configuration --------------------------------------

bool FileContentProvider::HasCasc() const {
#if WHITEOUT_HAS_CASC
    std::shared_lock sg(impl_->storageMu);
    return impl_->cascStorage.has_value();
#else
    return false;
#endif
}

bool FileContentProvider::HasMpq() const {
#if WHITEOUT_HAS_MPQ
    std::shared_lock sg(impl_->storageMu);
    return !impl_->mpqStorages.empty();
#else
    return false;
#endif
}

const std::string& FileContentProvider::Wc3Path() const {
    // Set once in Discover() and never written again; safe to return by ref
    // without taking storageMu.
    return impl_->wc3Path;
}

std::string FileContentProvider::InstallPath() const {
    std::shared_lock sg(impl_->storageMu);
    return impl_->installPath;
}

void FileContentProvider::SetInstallPath(const std::string& path) {
    std::unique_lock sg(impl_->storageMu);
    impl_->installPath = path.empty() ? impl_->wc3Path : path;
    impl_->TryOpenCascLocked();
    impl_->TryOpenMpqLocked();
}

bool FileContentProvider::IgnoreCasc() const {
    std::shared_lock sg(impl_->storageMu);
    return impl_->ignoreCasc;
}

bool FileContentProvider::IgnoreMpq() const {
    std::shared_lock sg(impl_->storageMu);
    return impl_->ignoreMpq;
}

void FileContentProvider::SetIgnoreCasc(bool ignore) {
    std::unique_lock sg(impl_->storageMu);
    if (impl_->ignoreCasc == ignore)
        return;
    impl_->ignoreCasc = ignore;
    impl_->TryOpenCascLocked();
}

void FileContentProvider::SetIgnoreMpq(bool ignore) {
    std::unique_lock sg(impl_->storageMu);
    if (impl_->ignoreMpq == ignore)
        return;
    impl_->ignoreMpq = ignore;
    impl_->TryOpenMpqLocked();
}

std::vector<std::string> FileContentProvider::MpqList() const {
    std::shared_lock sg(impl_->storageMu);
    return impl_->mpqList;
}

void FileContentProvider::SetMpqList(std::vector<std::string> list) {
    std::unique_lock sg(impl_->storageMu);
    impl_->mpqList = std::move(list);
    impl_->TryOpenMpqLocked();
}

std::vector<std::string> FileContentProvider::DefaultMpqList() {
    std::vector<std::string> out;
    out.reserve(std::size(kDefaultMpqNames));
    for (const char* n : kDefaultMpqNames)
        out.emplace_back(n);
    return out;
}

} // namespace whiteout::flakes::io
