// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "m3_save.h"

#include "io/m3/m3_model_adapter.h"
#include "whiteout/flakes/util/path_utf8.h"

#include <whiteout/models/m3/engine_compat.h>
#include <whiteout/models/m3/writer.h>

#include <exception>
#include <system_error>

namespace whiteout::flakes {

namespace fs = std::filesystem;
namespace m3 = ::whiteout::m3;

namespace {

// The version the writer will stamp a chunk with: it reads it off the first
// element and writes every element at that one, so an appended record carrying
// a different version would be serialised at a layout it was never parsed at.
template <typename T>
i32 ChunkVersion(const std::vector<T>& chunk, i32 fallback) {
    return chunk.empty() ? fallback : chunk.front().getVersion();
}

// Append `src`'s animation chunks to `dst`, rebased into one index space.
//
// The layout is `M3AnimTables::Build`'s, made structural: sequences and
// containers append end to end, and a file's STG_ groups name its OWN
// containers, so the indices inside them shift by the base. Bones are pointedly
// not merged — an `.m3a` binds to the model through `animId` and never through
// bone index, and its bone list is a differently-ordered subset that can name
// bones the model does not have.
std::size_t MergeAnimationChunks(m3::Model& dst, const m3::Model& src,
                                 std::vector<std::string>& lossy, const std::string& label) {
    const std::size_t seqBase = dst.sequences.size();
    const auto stcBase = static_cast<u32>(dst.subTrackCollections.size());
    const auto stsBase = static_cast<u16>(dst.animationStates.size());

    const i32 seqVersion = ChunkVersion(dst.sequences, ChunkVersion(src.sequences, 0));
    const i32 stcVersion =
        ChunkVersion(dst.subTrackCollections, ChunkVersion(src.subTrackCollections, 0));
    const i32 stgVersion = ChunkVersion(dst.animationGroups, ChunkVersion(src.animationGroups, 0));
    const i32 stsVersion = ChunkVersion(dst.animationStates, ChunkVersion(src.animationStates, 0));

    // SEQS is the one chunk here whose layout moves with the version: v1 and
    // older carry a deprecated word v2 dropped, and the writer stamps the whole
    // chunk with the version of its first record. Everything else the merge
    // touches -- STC_, STG_, STS_ -- parses and writes the same at every
    // version, so a rewrite there carries all of it and is not worth a line.
    const i32 srcSeqVersion = ChunkVersion(src.sequences, seqVersion);
    if ((srcSeqVersion <= 1) != (seqVersion <= 1)) {
        lossy.push_back(label + ": SEQS v" + std::to_string(srcSeqVersion) +
                        " written at the model's v" + std::to_string(seqVersion) +
                        "; the deprecated per-sequence word is not carried");
    }

    // STG_ is parallel to SEQS — group i names the containers sequence i plays
    // — so a short group array has to be padded before anything appends after
    // it, or the merged groups land on the wrong sequences.
    if (dst.animationGroups.size() < seqBase)
        dst.animationGroups.resize(seqBase);

    for (const m3::AnimationState& state : src.animationStates) {
        m3::AnimationState copy = state;
        copy.forceVersion(stsVersion);
        dst.animationStates.push_back(std::move(copy));
    }

    for (const m3::SubTrackContainer& stc : src.subTrackCollections) {
        m3::SubTrackContainer copy = stc;
        copy.animationStateIndex = static_cast<u16>(stsBase + copy.animationStateIndex);
        copy.forceVersion(stcVersion);
        dst.subTrackCollections.push_back(std::move(copy));
    }

    for (std::size_t i = 0; i < src.sequences.size(); ++i) {
        m3::Sequence copy = src.sequences[i];
        copy.index = static_cast<i32>(dst.sequences.size());
        copy.forceVersion(seqVersion);
        dst.sequences.push_back(std::move(copy));

        m3::AnimationGroup group;
        if (i < src.animationGroups.size()) {
            group = src.animationGroups[i];
            for (u32& index : group.subtrackIndices)
                index += stcBase;
        }
        group.forceVersion(stgVersion);
        dst.animationGroups.push_back(std::move(group));
    }

    return src.sequences.size();
}

// Drop the MADD records nothing names any more.
//
// M3ModelAdapter's constructor reverses every data-driven material it can into
// a StandardMaterial and repoints the MATM entries at it, so by the time a save
// sees the model most of MADD is dead weight — but `toStarCraft2` converts the
// array, not the references, and would both duplicate every record it already
// restored and refuse the whole model over one it could not. Pruning first
// makes the retarget see the model the renderer sees.
void PruneUnreferencedDataDriven(m3::Model& model) {
    if (model.dataDrivenMaterials.empty())
        return;

    std::vector<u32> remap(model.dataDrivenMaterials.size(), 0xFFFFFFFFu);
    std::vector<m3::DataDrivenMaterial> kept;
    for (m3::MaterialMap& map : model.materialMaps) {
        if (map.materialType != m3::MaterialType::DataDriven || map.materialIndex >= remap.size())
            continue;
        if (remap[map.materialIndex] == 0xFFFFFFFFu) {
            remap[map.materialIndex] = static_cast<u32>(kept.size());
            kept.push_back(model.dataDrivenMaterials[map.materialIndex]);
        }
        map.materialIndex = remap[map.materialIndex];
    }
    model.dataDrivenMaterials = std::move(kept);
}

} // namespace

M3SaveReport SaveModelAsM3(const M3SaveRequest& request) {
    M3SaveReport report;
    if (!request.source) {
        report.error = "no StarCraft II model on screen";
        return report;
    }

    // A copy, because both options rewrite it and the adapter behind the
    // pointer is still driving the actor on screen.
    m3::Model model = request.source->SourceModel();

    if (request.mergeAnimations) {
        const auto attached = request.source->AttachedAnimations();
        for (std::size_t i = 0; i < attached.size(); ++i) {
            const m3::Model* anim = request.source->AttachedAnimationModel(i);
            if (!anim)
                continue;
            report.mergedSequences +=
                MergeAnimationChunks(model, *anim, report.lossy, attached[i].label);
            ++report.mergedFiles;
        }
    }

    if (request.convertToSc2) {
        PruneUnreferencedDataDriven(model);
        // Asked before the conversion, which returns a model that by definition
        // is not Heroes-only any more.
        report.retargeted = m3::isHeroesOnly(model);
        m3::EngineConversion conversion = m3::toStarCraft2(model);
        if (!conversion.converted) {
            report.error = conversion.blocker;
            return report;
        }
        for (std::string& reason : conversion.lossy)
            report.lossy.push_back(std::move(reason));
        model = std::move(conversion.model);
    }

    report.version = model.getVersion();

    std::error_code dirError;
    fs::create_directories(request.outPath.parent_path(), dirError);
    try {
        m3::Writer writer;
        writer.write(io::PathToUtf8(request.outPath), model);
    } catch (const std::exception& e) {
        report.error = std::string("could not write the model: ") + e.what();
        return report;
    }
    if (!fs::exists(request.outPath)) {
        report.error = "could not write the model to " + io::PathToUtf8(request.outPath);
        return report;
    }

    report.ok = true;
    return report;
}

} // namespace whiteout::flakes
