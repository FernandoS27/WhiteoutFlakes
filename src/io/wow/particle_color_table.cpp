#include "io/progress.h"
#include "io/wow/particle_color_table.h"

#include "whiteout/flakes/content_provider.h"

#include <whiteout/database/parser.h>
#include <whiteout/database/table.h>

#include <cstdio>
#include <optional>
#include <utility>

namespace whiteout::flakes::io::wow {

namespace db = ::whiteout::database;

namespace {

// Both spellings, for the same reason CreatureSkinTable carries both: the path
// is what an extracted dump answers to, the fileDataID is what a CASC root is
// keyed by with no listfile in sight. 1284820 is also the id the retail client
// carries in its own DB2 meta block, so this is the client's number, not a
// listfile's guess.
constexpr const char* kParticleColorPath = "dbfilesclient/particlecolor.db2";
constexpr u32 kParticleColorFileId = 1284820;

// ---- Column positions -------------------------------------------------------
//
// Three array fields of three, which is exactly how the client's own export SQL
// spells the table:
//
//   SELECT "ID", "START0","START1","START2", "MID0","MID1","MID2",
//          "END0","END1","END2" FROM "PARTICLECOLOR"
//
// So the array runs over the SLOT and the field over the KEY — start, mid, end
// — and not the other way round. Getting that backwards produces colours that
// are individually plausible and wrong on every model, which is why the test
// pins a shipped row rather than the field count alone.
constexpr u32 kStartField = 0;
constexpr u32 kMidField = 1;
constexpr u32 kEndField = 2;
constexpr u32 kFields = 3;

/// The client's marker for "your ParticleColorID names no row": opaque green,
/// written into all three keys of all three slots (`ReplaceParticleColor`
/// @0x100345e20 and its 11.x counterpart both do this).
constexpr Vector3f kMissingRowColor{0.0f, 1.0f, 0.0f};

/// One packed `0xAARRGGBB` column, in the space the colour track is decoded
/// into. Alpha is dropped on purpose — the client reads bytes 0/1/2 only, so a
/// skin never touches a particle's alpha curve.
Vector3f UnpackArgb(u64 v) {
    return Vector3f{static_cast<f32>((v >> 16) & 0xFF) / 255.0f,
                    static_cast<f32>((v >> 8) & 0xFF) / 255.0f,
                    static_cast<f32>(v & 0xFF) / 255.0f};
}

} // namespace

void ParticleColorTable::Clear() {
    loaded_ = false;
    rows_.clear();
}

bool ParticleColorTable::Load(IContentProvider& provider, ProgressMonitor* progress) {
    if (loaded_)
        return true;
    // `progress` is accepted and unused: this is one 80 KB read, which is over
    // before a bar drawn for it could be seen.
    (void)progress;

    auto bytes = provider.ReadFile(kParticleColorPath);
    if (!bytes || bytes->empty())
        bytes = provider.ReadFile(ContentRef::FromFileId(kParticleColorFileId));
    if (!bytes || bytes->empty()) {
        std::fprintf(stderr,
                     "[wow] particle colours: '%s' unreadable by path or by id %u — skins recolour "
                     "nothing\n",
                     kParticleColorPath, kParticleColorFileId);
        return false;
    }

    db::Parser parser;
    std::optional<db::Table> table = parser.parse(std::move(*bytes));
    if (!table) {
        std::fprintf(stderr, "[wow] particle colours: '%s' failed to parse\n", kParticleColorPath);
        for (const auto& issue : parser.getIssues())
            std::fprintf(stderr, "[wow]   %s\n", issue.c_str());
        return false;
    }

    // Checked against a shape only this layout has: three fields, each an array
    // as wide as there are slots. A build that added a column would land here
    // rather than silently reading a neighbour as a colour.
    const auto& fields = table->fields();
    if (fields.size() != kFields) {
        std::fprintf(stderr,
                     "[wow] ParticleColor has an unexpected layout (%zu fields) — skins recolour "
                     "nothing\n",
                     fields.size());
        return false;
    }
    for (u32 f = 0; f < kFields; ++f) {
        if (fields[f].arrayCount != renderer::M2ParticleColorOverride::kSlots) {
            std::fprintf(stderr,
                         "[wow] ParticleColor field %u is %u wide, expected %u — skins recolour "
                         "nothing\n",
                         f, fields[f].arrayCount, renderer::M2ParticleColorOverride::kSlots);
            return false;
        }
    }

    rows_.clear();
    rows_.reserve(table->rowCount());
    for (usize i = 0; i < table->rowCount(); ++i) {
        const db::Row row = table->row(i);
        if (row.isEncrypted())
            continue;
        renderer::M2ParticleColorOverride entry;
        for (u32 slot = 0; slot < renderer::M2ParticleColorOverride::kSlots; ++slot) {
            entry.key[slot][0] = UnpackArgb(row.getUInt(kStartField, slot));
            entry.key[slot][1] = UnpackArgb(row.getUInt(kMidField, slot));
            entry.key[slot][2] = UnpackArgb(row.getUInt(kEndField, slot));
        }
        rows_.emplace(row.id(), entry);
    }

    loaded_ = true;
    std::printf("[wow] particle colours: %zu rows\n", rows_.size());
    return true;
}

bool ParticleColorTable::Resolve(u32 id, renderer::M2ParticleColorOverride& out) const {
    if (id == 0)
        return false;
    if (const auto it = rows_.find(id); it != rows_.end()) {
        out = it->second;
        return true;
    }
    // Not "nothing to do": the client marks this case, and so does this.
    for (auto& slot : out.key)
        for (auto& key : slot)
            key = kMissingRowColor;
    return true;
}

} // namespace whiteout::flakes::io::wow
