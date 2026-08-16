#include "io/wow/character_appearance.h"

#include <algorithm>
#include <cstring>

namespace whiteout::flakes::io::wow {

namespace {

// Nearest-neighbour resample of @p src into the @p dst rect, blended.
//
// Nearest rather than bilinear on purpose: every real paste is either 1:1 or a
// power-of-two reduction, which is what the client's mip walk produces, and a
// filtered downscale would blur a seam the layout is built to hide.
void Blit(std::vector<u8>& dst, u32 dstW, u32 dstH, u32 x0, u32 y0, u32 w, u32 h,
          const std::vector<u8>& src, u32 srcW, u32 srcH, bool overwrite) {
    if (w == 0 || h == 0 || srcW == 0 || srcH == 0)
        return;
    for (u32 y = 0; y < h; ++y) {
        const u32 dy = y0 + y;
        if (dy >= dstH)
            break;
        const u32 sy = static_cast<u32>(static_cast<u64>(y) * srcH / h);
        for (u32 x = 0; x < w; ++x) {
            const u32 dx = x0 + x;
            if (dx >= dstW)
                break;
            const u32 sx = static_cast<u32>(static_cast<u64>(x) * srcW / w);
            const u8* s = &src[(static_cast<usize>(sy) * srcW + sx) * 4];
            u8* d = &dst[(static_cast<usize>(dy) * dstW + dx) * 4];
            if (overwrite) {
                d[0] = s[0];
                d[1] = s[1];
                d[2] = s[2];
                d[3] = 255; // the base layer is the sheet; nothing shows through it
                continue;
            }
            // Source-over, the only blend a character composite needs: a face
            // or an underwear sheet is an RGBA overlay on the skin beneath it.
            const u32 a = s[3];
            if (a == 0)
                continue;
            const u32 ia = 255 - a;
            d[0] = static_cast<u8>((s[0] * a + d[0] * ia) / 255);
            d[1] = static_cast<u8>((s[1] * a + d[1] * ia) / 255);
            d[2] = static_cast<u8>((s[2] * a + d[2] * ia) / 255);
            d[3] = static_cast<u8>(std::max<u32>(d[3], a));
        }
    }
}

// The single section a mask names, or null when it names none or several.
//
// All-ones is the common "several": the base skin arrives as one sheet the size
// of the whole composite, not as eight pieces, so it is pasted at the origin
// and scaled to the sheet. A mask with exactly one bit set is a piece.
const TextureSection* SoleSection(const ChrModelInfo& model, i64 mask) {
    if (mask < 0)
        return nullptr;
    const u64 bits = static_cast<u64>(mask);
    if (bits == 0 || (bits & (bits - 1)) != 0)
        return nullptr;
    u32 section = 0;
    for (u64 b = bits; b > 1; b >>= 1)
        ++section;
    return model.Section(section);
}

} // namespace

std::vector<u32> DefaultChoices(const ChrModelInfo& model) {
    std::vector<u32> out;
    out.reserve(model.options.size());
    for (const auto& opt : model.options)
        if (!opt.choices.empty())
            out.push_back(opt.choices.front().id);
    return out;
}

ResolvedAppearance ResolveAppearance(const ChrCustomizationTable& tables, const ChrModelInfo& model,
                                     std::span<const u32> choiceIds) {
    ResolvedAppearance out;
    out.geosets = DefaultSelection();

    // Every geoset this model's options *could* name, active or not. An option
    // owns those ids and nothing else: the ones its chosen variant asks for
    // draw, its siblings do not, and a geoset in the same group that no choice
    // ever mentions is none of its business. Narrowing the whole group instead
    // is what took the 3201 mouth piece off every race with a Face Shape
    // option, and left the races without one showing 3201 alone — a blank plate
    // where `bloodelfmale_hd` should have had a face.
    for (const CustomizationOption& opt : model.options)
        for (const CustomizationChoice& choice : opt.choices)
            for (const ChoiceElement& e : tables.Elements(choice.id))
                if (e.geoset >= 0)
                    out.geosets.Control(static_cast<u16>(e.geoset));

    // Which layer each texture target feeds. Built from the model's own layer
    // list so a material for a target this layout does not composite — an item
    // texture, a target another race uses — is dropped rather than pasted
    // somewhere arbitrary.
    for (const u32 choice : choiceIds) {
        for (const ChoiceElement& e : tables.Elements(choice)) {
            if (e.relatedChoiceId != 0 &&
                std::find(choiceIds.begin(), choiceIds.end(), e.relatedChoiceId) == choiceIds.end())
                continue;
            if (e.geoset >= 0)
                out.geosets.Show(static_cast<u16>(e.geoset));
            if (e.skinnedModelFileId != 0 && e.skinnedGeoset >= 0)
                out.skinnedModels.push_back({e.skinnedModelFileId, e.skinnedGeoset});
            if (e.materialResourcesId == 0)
                continue;
            const u32 file = tables.TextureFileFor(e.materialResourcesId);
            if (file == 0)
                continue;
            for (const CompositeLayer& layer : model.layers) {
                if (layer.target != e.materialTarget)
                    continue;
                CompositePaste p;
                p.textureType = layer.textureType;
                p.layer = layer.layer;
                p.blendMode = layer.blendMode;
                p.sectionMask = layer.sectionMask;
                p.textureFileId = file;
                out.pastes.push_back(p);
            }
        }
    }

    std::stable_sort(out.pastes.begin(), out.pastes.end(),
                     [](const CompositePaste& a, const CompositePaste& b) {
                         if (a.textureType != b.textureType)
                             return a.textureType < b.textureType;
                         return a.layer < b.layer;
                     });
    return out;
}

std::vector<ComposedTexture> ComposeCharacter(const ChrModelInfo& model,
                                              const ResolvedAppearance& appearance,
                                              const ImageFetch& fetch) {
    std::vector<ComposedTexture> out;
    if (!fetch)
        return out;

    for (const CompositeTarget& target : model.composites) {
        ComposedTexture sheet;
        sheet.textureType = target.textureType;
        sheet.width = target.width;
        sheet.height = target.height;
        bool any = false;

        for (const CompositePaste& paste : appearance.pastes) {
            if (paste.textureType != target.textureType)
                continue;
            std::vector<u8> src;
            u32 sw = 0, sh = 0;
            if (!fetch(paste.textureFileId, src, sw, sh) || sw == 0 || sh == 0 ||
                src.size() < static_cast<usize>(sw) * sh * 4)
                continue;
            const bool first = !any;
            if (first) {
                // Allocated on the first layer that actually arrived, so a
                // composite whose every source is missing produces nothing at
                // all rather than a transparent sheet that would blank the
                // model. Zero-filled: the sections no layer covers stay
                // transparent, which is what the client's own scratch buffer
                // holds before the first paste.
                sheet.rgba.assign(static_cast<usize>(sheet.width) * sheet.height * 4, 0);
                any = true;
            }
            const TextureSection* section = SoleSection(model, paste.sectionMask);
            const u32 x = section ? section->x : 0;
            const u32 y = section ? section->y : 0;
            const u32 w = section ? section->w : sheet.width;
            const u32 h = section ? section->h : sheet.height;
            // The body sheet, and only the body sheet, is forced opaque by its
            // first full-sheet layer: it is the skin, nothing shows through a
            // character's chest, and a `.blp` carrying a stray alpha channel
            // would otherwise leave holes in it.
            //
            // Every other composite is the opposite case — its alpha *is* the
            // shape. The hair sheet's alpha cuts the strands out of their
            // quads and the eye sheet's cuts the eyeball out of its padding;
            // overwriting either with 255 turns the geometry into the solid
            // slab it was drawn on. That is what put a brown mask over
            // `draeneimale_hd`'s face and glowing tiles over its eyes.
            const bool opaqueBase =
                first && section == nullptr &&
                target.textureType == static_cast<u32>(M2TextureType::Skin);
            Blit(sheet.rgba, sheet.width, sheet.height, x, y, w, h, src, sw, sh, opaqueBase);
        }

        if (any)
            out.push_back(std::move(sheet));
    }
    return out;
}

} // namespace whiteout::flakes::io::wow
