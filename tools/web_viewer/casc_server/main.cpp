// ============================================================================
// wf_casc_server — Crow-based dev server that imitates Hiveworkshop's
// CASC-backed loose-file delivery for the WhiteoutFlakes web viewer.
//
// Opens a local Warcraft III install via WhiteoutLib's casc::Storage and
// serves bytes by path. The same server also fronts the static web/
// directory so the viewer page, wf-core.{js,wasm}, prebuilt .bls shaders,
// and CASC assets all come from one origin (no CORS preflight, no second
// http.server).
//
// Routes:
//   GET /casc/<path>   CASC readFile bytes (404 on miss)
//   GET /              web-root/index.html
//   GET /<file...>     web-root/<file...> (any depth)
//
// CLI:
//   --wc3-dir <path>   required; WC3 install dir (containing .build.info)
//   --port <n>         default 8080
//   --web-root <dir>   default ./web (relative to cwd)
//   --simulate-delays  throttle CASC responses to ~1s per MiB (latency sim)
// ============================================================================

#include <crow.h>

#include <whiteout/storages/casc/storage.h>
#include <whiteout/utils/blizzard_game_finder.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using whiteout::storages::casc::Storage;

namespace {

bool IEndsWith(std::string_view s, std::string_view suffix) {
    if (s.size() < suffix.size()) return false;
    return std::equal(suffix.rbegin(), suffix.rend(), s.rbegin(),
        [](char a, char b) { return std::tolower(static_cast<unsigned char>(a))
                                 == std::tolower(static_cast<unsigned char>(b)); });
}

const char* MimeFor(std::string_view path) {
    if (IEndsWith(path, ".html") || IEndsWith(path, ".htm")) return "text/html; charset=utf-8";
    if (IEndsWith(path, ".js") || IEndsWith(path, ".mjs"))   return "application/javascript; charset=utf-8";
    if (IEndsWith(path, ".wasm"))                            return "application/wasm";
    if (IEndsWith(path, ".css"))                             return "text/css; charset=utf-8";
    if (IEndsWith(path, ".json"))                            return "application/json; charset=utf-8";
    if (IEndsWith(path, ".png"))                             return "image/png";
    if (IEndsWith(path, ".jpg") || IEndsWith(path, ".jpeg")) return "image/jpeg";
    if (IEndsWith(path, ".mp3"))                             return "audio/mpeg";
    if (IEndsWith(path, ".wav"))                             return "audio/wav";
    if (IEndsWith(path, ".ogg"))                             return "audio/ogg";
    // MDX, BLP, DDS, TGA, TIF, BLS, MDL, SLK, TXT-ish — viewer treats them
    // as ArrayBuffer regardless of Content-Type.
    return "application/octet-stream";
}

std::optional<std::vector<char>> ReadDiskFile(const fs::path& p) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) return std::nullopt;
    std::ifstream f(p, std::ios::binary);
    if (!f) return std::nullopt;
    return std::vector<char>(std::istreambuf_iterator<char>(f),
                             std::istreambuf_iterator<char>());
}

// WC3 Reforged uses TVFS root with three logical mods stacked. The same
// table the desktop FileContentProvider uses (src/io/file_content_provider.cpp).
// Order matters: _hd.w3mod overrides war3.w3mod for HD assets.
constexpr const char* kCascPrefixes[] = {
    "war3.w3mod:",
    "war3.w3mod:_hd.w3mod:",
    "war3.w3mod:_deprecated.w3mod:",
};

// Mod-name prefixes some renderer adapters (notably corn_fx) bake into
// their relative paths, e.g. `_HD.w3mod/Textures/FX/...`. In TVFS chain
// syntax the segment that names the mod is separated by `:`, not `\`, so
// we need to flip it before joining with the base prefix.
constexpr const char* kEmbeddedModPrefixes[] = {
    "_hd.w3mod\\",
    "_deprecated.w3mod\\",
};

// Mirror file_content_provider.cpp's NormalizeCascPath: forward → back-
// slashes, lowercase, strip leading separators. Required because WC3's
// CASC root manifest is keyed by Blizzard's native (backslash, lowercase)
// shape. Additionally, if the path opens with a mod-name segment (corn_fx
// produces these), convert the trailing `\` to `:` so it slots into the
// TVFS chain instead of being read as a single leaf-path component.
std::string NormalizeCascPath(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        if (c == '/') c = '\\';
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    std::size_t start = 0;
    while (start < out.size() && (out[start] == '\\' || out[start] == '/')) ++start;
    if (start > 0) out.erase(0, start);
    for (const char* p : kEmbeddedModPrefixes) {
        const std::size_t plen = std::strlen(p);
        if (out.size() > plen && out.compare(0, plen, p) == 0) {
            out[plen - 1] = ':'; // flip trailing `\` of the mod-name to `:`
            break;
        }
    }
    return out;
}

// Pick the prefix set to try. If the normalized path already carries a
// `:` (we just inserted one in NormalizeCascPath, or the caller passed
// one explicitly), there's already a mod-name baked in — prepend only
// `war3.w3mod:`. Otherwise the caller didn't pin a mod, so we walk all
// three stack levels in priority order.
struct PrefixSpan { const char* const* data; std::size_t size; };
PrefixSpan ChoosePrefixes(std::string_view normPath) {
    static constexpr const char* kBase[] = { "war3.w3mod:" };
    if (normPath.find(':') != std::string_view::npos)
        return { kBase, 1 };
    return { kCascPrefixes, std::size(kCascPrefixes) };
}

void AddCommonHeaders(crow::response& res, std::string_view contentType) {
    // Open CORS for cross-origin embeds (the combined server's own origin
    // wouldn't need it, but a host page that fetches casc/* from elsewhere
    // would). no-store keeps dev iterations honest — we tear into wf-viewer.js
    // a lot.
    res.add_header("Access-Control-Allow-Origin", "*");
    res.add_header("Cache-Control", "no-store");
    res.add_header("Content-Type", std::string(contentType));
}

struct Args {
    std::string wc3Dir;
    std::string webRoot        = "web";
    int         port           = 8080;
    bool        simulateDelays = false;
};

[[noreturn]] void Usage(const char* prog) {
    std::fprintf(stderr,
        "usage: %s [--wc3-dir <wc3-install>] [--port N] [--web-root DIR]\n"
        "          [--simulate-delays]\n"
        "  --wc3-dir         WC3 install path (auto-discovered via the Blizzard\n"
        "                    game finder when omitted: registry / Battle.net /\n"
        "                    Steam on Win, /Applications on macOS, Wine + Proton\n"
        "                    compatdata on Linux)\n"
        "  --port            HTTP port (default 8080)\n"
        "  --web-root        Static file root (default ./web)\n"
        "  --simulate-delays Delay each CASC response proportional to its size\n"
        "                    (~1s per MiB) to mimic real-world network latency\n",
        prog);
    std::exit(2);
}

Args ParseArgs(int argc, char** argv) {
    Args a;
    auto next = [&](int& i) -> std::string {
        if (i + 1 >= argc) {
            std::fprintf(stderr, "missing value for %s\n", argv[i]);
            Usage(argv[0]);
        }
        return std::string(argv[++i]);
    };
    for (int i = 1; i < argc; ++i) {
        std::string_view k = argv[i];
        if      (k == "--wc3-dir")         a.wc3Dir         = next(i);
        else if (k == "--port")            a.port           = std::atoi(next(i).c_str());
        else if (k == "--web-root")        a.webRoot        = next(i);
        else if (k == "--simulate-delays") a.simulateDelays = true;
        else if (k == "-h" || k == "--help") Usage(argv[0]);
        else {
            std::fprintf(stderr, "unknown flag: %s\n", argv[i]);
            Usage(argv[0]);
        }
    }
    return a;
}

// Locate a Warcraft III install via WhiteoutLib's cross-platform discovery
// (Windows registry + Battle.net DB + Steam libraries, macOS /Applications,
// Linux Wine prefixes + Proton compatdata). Prefers an install whose
// `Data/` subdir exists — that's the one with a usable CASC archive —
// falling back to any matching entry so the user gets a clearer Storage
// error than "(empty path)". Mirrors the desktop FileContentProvider's
// pick in src/io/file_content_provider.cpp.
std::string AutoDiscoverWc3() {
    using whiteout::utils::BlizzardGame;
    auto games = whiteout::utils::findBlizzardGames();
    std::string fallback;
    for (const auto& info : games) {
        if (info.game != BlizzardGame::WarcraftIII &&
            info.game != BlizzardGame::WarcraftIIIReforged)
            continue;
        if (std::filesystem::exists(std::filesystem::path(info.path) / "Data"))
            return info.path;
        if (fallback.empty()) fallback = info.path;
    }
    return fallback;
}

} // namespace

int main(int argc, char** argv) {
    Args args = ParseArgs(argc, argv);

    if (args.wc3Dir.empty()) {
        args.wc3Dir = AutoDiscoverWc3();
        if (args.wc3Dir.empty()) {
            std::fprintf(stderr,
                "[wf_casc_server] no Warcraft III install found via auto-"
                "discovery. Pass --wc3-dir <path> explicitly.\n");
            return 1;
        }
        std::printf("[wf_casc_server] auto-discovered WC3 at: %s\n",
                    args.wc3Dir.c_str());
    }

    // Open one Storage at startup; the docs guarantee readFile is thread-safe
    // (shared lock), so Crow's worker pool can hit it concurrently.
    std::string err;
    auto storageOpt = Storage::open(args.wc3Dir, &err);
    if (!storageOpt) {
        std::fprintf(stderr, "CASC open failed at '%s': %s\n",
                     args.wc3Dir.c_str(),
                     err.empty() ? "(no message)" : err.c_str());
        return 1;
    }
    Storage* storage = &*storageOpt;
    std::printf("[wf_casc_server] CASC storage opened: %s\n", args.wc3Dir.c_str());

    const fs::path webRoot = fs::absolute(args.webRoot);
    std::printf("[wf_casc_server] web-root:  %s\n", webRoot.string().c_str());

    crow::SimpleApp app;
    app.loglevel(crow::LogLevel::Warning);

    // CASC bytes. Crow's `<path>` matcher captures everything after the
    // prefix (including slashes), giving us the WC3-relative path verbatim.
    // We walk the kCascPrefixes table — same shape the desktop provider
    // uses — so an MDX reference like `units/human/footman/footman.mdx`
    // resolves through `war3.w3mod:` and an HD-only asset finds its bytes
    // under `war3.w3mod:_hd.w3mod:`.
    const bool simulateDelays = args.simulateDelays;
    if (simulateDelays) {
        std::printf("[wf_casc_server] --simulate-delays ON: CASC responses "
                    "throttled to ~1s per MiB\n");
    }
    CROW_ROUTE(app, "/casc/<path>")
    ([storage, simulateDelays](std::string cascPath) {
        const std::string norm = NormalizeCascPath(cascPath);
        const PrefixSpan prefixes = ChoosePrefixes(norm);
        std::optional<std::vector<uint8_t>> bytes;
        for (std::size_t i = 0; i < prefixes.size; ++i) {
            bytes = storage->readFile(std::string(prefixes.data[i]) + norm);
            if (bytes) break;
        }
        crow::response res;
        if (!bytes) {
            res.code = 404;
            AddCommonHeaders(res, "text/plain; charset=utf-8");
            res.body = "Not found in CASC: " + cascPath;
            return res;
        }
        if (simulateDelays) {
            // ~1s per MiB. Sleeps on the Crow worker thread; the pool is
            // multithreaded so other requests keep flowing in parallel.
            const auto delayMs = static_cast<std::int64_t>(
                (bytes->size() * 250ULL) / (1024ULL * 1024ULL));
            if (delayMs > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        }
        res.code = 200;
        AddCommonHeaders(res, MimeFor(cascPath));
        res.body.assign(reinterpret_cast<const char*>(bytes->data()), bytes->size());
        return res;
    });

    // Static page root — serves index.html.
    CROW_ROUTE(app, "/")
    ([webRoot]() {
        crow::response res;
        auto bytes = ReadDiskFile(webRoot / "index.html");
        if (!bytes) {
            res.code = 404;
            AddCommonHeaders(res, "text/plain; charset=utf-8");
            res.body = "no index.html under " + webRoot.string();
            return res;
        }
        res.code = 200;
        AddCommonHeaders(res, "text/html; charset=utf-8");
        res.body.assign(bytes->data(), bytes->size());
        return res;
    });

    // Static catch-all for everything else (wf-core.js/wasm, wf-viewer.js,
    // shaders/, models/, ...). Path-traversal-rejected.
    CROW_ROUTE(app, "/<path>")
    ([webRoot](std::string relPath) {
        crow::response res;
        if (relPath.find("..") != std::string::npos) {
            res.code = 403;
            AddCommonHeaders(res, "text/plain; charset=utf-8");
            res.body = "forbidden";
            return res;
        }
        auto bytes = ReadDiskFile(webRoot / relPath);
        if (!bytes) {
            res.code = 404;
            AddCommonHeaders(res, "text/plain; charset=utf-8");
            res.body = "not found: " + relPath;
            return res;
        }
        res.code = 200;
        AddCommonHeaders(res, MimeFor(relPath));
        res.body.assign(bytes->data(), bytes->size());
        return res;
    });

    std::printf("[wf_casc_server] listening on http://localhost:%d\n", args.port);
    std::printf("                 viewer:  http://localhost:%d/\n",   args.port);
    std::printf("                 casc:    http://localhost:%d/casc/<path>\n", args.port);
    app.port(static_cast<uint16_t>(args.port)).multithreaded().run();
    return 0;
}
