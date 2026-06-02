#include "bls_pso_trace.h"

#include "bls_program.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <system_error>

namespace whiteout::flakes::renderer::bls {

namespace {

constexpr u32 kMagic = 0x544F5350u; // 'PSOT' LE
constexpr u32 kVersion = 2u;
constexpr usize kV1EntrySize = 16; // pre-MRT layout

u64 EntryKey(const PsoTraceEntry& e) {
    // Hash to dedupe — fold all 20 bytes into a u64 (three 8-byte
    // chunks; last chunk reads past the entry but we keep entries
    // padded so this is safe — see Load/MakeEntry zero-init).
    u64 lo = 0, mid = 0, hi = 0;
    std::memcpy(&lo, reinterpret_cast<const u8*>(&e), 8);
    std::memcpy(&mid, reinterpret_cast<const u8*>(&e) + 8, 8);
    std::memcpy(&hi, reinterpret_cast<const u8*>(&e) + 16, 4);
    return lo ^ (mid * 0x9E3779B185EBCA87ull) ^ (hi * 0xC2B2AE3D27D4EB4Full);
}

PsoTraceEntry MakeEntry(const PsoRequest& r) {
    PsoTraceEntry e{};
    e.programId = r.program ? static_cast<u8>(r.program->id) : 0;
    e.alpha = static_cast<u8>(r.material.alpha);
    e.layout = static_cast<u8>(r.layout);
    e.topology = static_cast<u8>(r.topology);
    e.rtvFormat = static_cast<u8>(static_cast<u32>(r.rtvFormat));
    e.dsvFormat = static_cast<u8>(static_cast<u32>(r.dsvFormat));
    e.flags = (r.wireframe ? 0x1u : 0u) | (r.lhClipSpace ? 0x2u : 0u);
    e.vsIndex = static_cast<u16>(r.vsIndex);
    e.psIndex = static_cast<u16>(r.psIndex);
    e.disables = r.material.disables;
    e.extraRtvCount = static_cast<u8>(r.extraRtvCount);
    for (u32 i = 0; i < r.extraRtvCount &&
                    i < gfx::GraphicsPipelineDesc::kMaxExtraColorAttachments &&
                    i < std::size(e.extraRtvFormats);
         ++i) {
        e.extraRtvFormats[i] = static_cast<u8>(static_cast<u32>(r.extraRtvFormats[i]));
    }
    return e;
}

} // namespace

BlsPsoTrace::BlsPsoTrace(std::filesystem::path path) : path_(std::move(path)) {
    Load();
}

BlsPsoTrace::~BlsPsoTrace() {
    if (dirty_)
        Save();
}

void BlsPsoTrace::Load() {
    if (path_.empty())
        return;
    std::error_code ec;
    if (!std::filesystem::exists(path_, ec) || ec)
        return;

    std::ifstream f(path_, std::ios::binary);
    if (!f)
        return;

    u32 magic = 0, version = 0, count = 0;
    f.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    f.read(reinterpret_cast<char*>(&version), sizeof(version));
    f.read(reinterpret_cast<char*>(&count), sizeof(count));
    if (!f || magic != kMagic)
        return;
    if (version != 1u && version != kVersion)
        return;

    // Sanity cap so a corrupt header doesn't drive a multi-GB allocation.
    if (count > 1'000'000u)
        return;

    entries_.resize(count);
    if (version == kVersion) {
        f.read(reinterpret_cast<char*>(entries_.data()),
               static_cast<std::streamsize>(count) * sizeof(PsoTraceEntry));
    } else {
        // v1 upgrade: read the first 16 bytes of each row and zero the
        // four trailing MRT bytes (PsoTraceEntry's zero-init via resize
        // already handles that, so we only need to read the legacy
        // prefix into the same slot).
        for (u32 i = 0; i < count; ++i) {
            f.read(reinterpret_cast<char*>(&entries_[i]), kV1EntrySize);
            if (!f)
                break;
        }
        // Mark dirty so the next Save() rewrites in v2 layout.
        dirty_ = true;
    }
    if (!f) {
        entries_.clear();
        return;
    }
    keys_.reserve(count);
    for (const auto& e : entries_)
        keys_.insert(EntryKey(e));
}

void BlsPsoTrace::Save() {
    if (path_.empty())
        return;

    std::error_code ec;
    if (path_.has_parent_path())
        std::filesystem::create_directories(path_.parent_path(), ec);

    std::ofstream f(path_, std::ios::binary | std::ios::trunc);
    if (!f)
        return;

    u32 count = static_cast<u32>(entries_.size());
    f.write(reinterpret_cast<const char*>(&kMagic), sizeof(kMagic));
    f.write(reinterpret_cast<const char*>(&kVersion), sizeof(kVersion));
    f.write(reinterpret_cast<const char*>(&count), sizeof(count));
    if (count) {
        f.write(reinterpret_cast<const char*>(entries_.data()),
                static_cast<std::streamsize>(count) * sizeof(PsoTraceEntry));
    }
    dirty_ = false;
}

void BlsPsoTrace::Record(const PsoRequest& request) {
    if (!request.program)
        return;
    PsoTraceEntry e = MakeEntry(request);
    const u64 key = EntryKey(e);
    if (!keys_.insert(key).second)
        return;
    entries_.push_back(e);
    dirty_ = true;
}

void BlsPsoTrace::Replay(BlsPsoBuilder& builder, const BlsProgramCatalog& catalog) const {
    for (const auto& e : entries_) {
        const BlsProgram* prog = catalog.Get(static_cast<GxShaderID>(e.programId));
        if (!prog || !prog->IsValid())
            continue;

        PsoRequest req{};
        req.program = prog;
        req.vsIndex = e.vsIndex;
        req.psIndex = e.psIndex;
        req.material.alpha = static_cast<GxMatAlpha>(e.alpha);
        req.material.disables = e.disables;
        req.layout = static_cast<VertexLayoutKind>(e.layout);
        req.topology = static_cast<gfx::PrimitiveTopology>(e.topology);
        req.rtvFormat = static_cast<gfx::Format>(e.rtvFormat);
        req.extraRtvCount = e.extraRtvCount;
        for (u32 i = 0; i < e.extraRtvCount &&
                        i < gfx::GraphicsPipelineDesc::kMaxExtraColorAttachments &&
                        i < std::size(e.extraRtvFormats);
             ++i) {
            req.extraRtvFormats[i] = static_cast<gfx::Format>(e.extraRtvFormats[i]);
        }
        req.dsvFormat = static_cast<gfx::Format>(e.dsvFormat);
        req.wireframe = (e.flags & 0x1u) != 0;
        req.lhClipSpace = (e.flags & 0x2u) != 0;

        builder.GetOrBuild(req); // builds + caches, or no-ops if invalid indices
    }
}

} // namespace whiteout::flakes::renderer::bls
