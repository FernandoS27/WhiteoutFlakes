#include "binding_internal.hpp"

#include <cornflakes/interface/binding/effect_binder.hpp>
#include <cornflakes/core/determinism.hpp>
#include <cornflakes/vm/bytecode_decoder.hpp>

#include <cstring>

namespace whiteout::cornflakes {

namespace {

// Builds a fully-resolved VMProgramDescriptor from one CCompilerBlobCache:
// arena-copies the bytecode, runs the decoder for instruction-level use,
// arena-copies the constants pool, and resolves Externals + ExternalCalls
// links into typed binding spans.
VMProgramDescriptor descriptorFromBlob(const EffectAssetModel& model, const AssetObject& blob,
                                       IArena& arena) {
    VMProgramDescriptor d;

    const auto parsed = parseBlob(blob);
    if (!parsed || parsed->bytecode.empty()) {
        return d;
    }
    const auto bcCopy = arenaArray<u8>(arena, parsed->bytecode.size());
    std::memcpy(bcCopy.data(), parsed->bytecode.data(), parsed->bytecode.size());
    d.cbemBytecode = std::span<const u8>{bcCopy.data(), bcCopy.size()};

    {
        IssueBag decodeIssues;
        const auto decoded = decodeBytecodeStream(d.cbemBytecode, arena, decodeIssues);
        if (!decodeIssues.hasFatal()) {
            d.decodedInstructions = decoded.instructions;
        }
    }
    if (!parsed->constants.empty()) {
        const auto constCopy = arenaArray<std::byte>(arena, parsed->constants.size());
        std::memcpy(constCopy.data(), parsed->constants.data(), parsed->constants.size());
        d.constantsPool = std::span<const std::byte>{constCopy.data(), constCopy.size()};
    }
    d.registerCounts = parsed->registerCounts;

    if (const auto extLinks = fieldLinks(blob, "Externals"); !extLinks.empty()) {
        const auto bindingArr = arenaArray<ExternalBinding>(arena, extLinks.size());
        std::size_t written = 0;
        for (std::size_t i = 0; i < extLinks.size(); ++i) {
            const auto view = readExternalLink(model, blob, static_cast<u32>(i));
            if (!view) {
                continue;
            }
            ExternalBinding& b = bindingArr[written++];
            b.slot = static_cast<u16>(i);
            b.canonicalSlot = static_cast<u16>(i);
            b.name = stableCopy(view->name, arena);
            b.typeName = stableCopy(view->typeName, arena);
            b.nativeType = view->nativeType;
            b.storageSize = view->storageSize;
            b.accessMask = view->accessMask;
        }
        if (written > 0) {
            d.externals = std::span<const ExternalBinding>{bindingArr.data(), written};
        }
    }

    if (const auto callLinks = fieldLinks(blob, "ExternalCalls"); !callLinks.empty()) {
        const auto fnArr = arenaArray<FunctionBinding>(arena, callLinks.size());
        std::size_t written = 0;
        for (std::size_t i = 0; i < callLinks.size(); ++i) {
            const auto view = readFunctionCallLink(model, blob, static_cast<u32>(i));
            if (!view) {
                continue;
            }
            FunctionBinding& f = fnArr[written++];
            f.slot = static_cast<u16>(i);
            f.symbolName = stableCopy(view->symbolName, arena);
            f.symbolSlot = view->symbolSlot;
            f.traits = view->traits;
        }
        if (written > 0) {
            d.functions = std::span<const FunctionBinding>{fnArr.data(), written};
        }
    }
    return d;
}

} // namespace

void loadScopePrograms(const EffectAssetModel& model, const AssetObject& layerCache,
                       LayerProgram& lp, IArena& arena) {
    // BlobCache_IR_TimeFixed contains init / physics / timeFixed all tagged by `Identifier`.
    for (const u32 blobUid : fieldLinks(layerCache, "BlobCache_IR_TimeFixed")) {
        const AssetObject* blob = findObjectByUid(model, blobUid);
        if (blob == nullptr || blob->type != "CCompilerBlobCache") {
            continue;
        }
        const u32 ident = fieldUint(*blob, "Identifier").value_or(0U);
        VMProgramDescriptor d = descriptorFromBlob(model, *blob, arena);
        switch (static_cast<BlobScope>(ident)) {
        case BlobScope::Init:
            if (lp.initProgram.cbemBytecode.empty()) {
                lp.initProgram = d;
            }
            break;
        case BlobScope::Physics:
            if (lp.physicsProgram.cbemBytecode.empty()) {
                lp.physicsProgram = d;
            }
            break;
        case BlobScope::TimeFixed:
            if (lp.timeFixedProgram.cbemBytecode.empty()) {
                lp.timeFixedProgram = d;
            }
            break;
        default:
            break;
        }
    }

    // BlobCache_IR_TimeVarying lives on its own field.
    if (const auto tvUid = fieldLink(layerCache, "BlobCache_IR_TimeVarying")) {
        if (const AssetObject* blob = findObjectByUid(model, *tvUid);
            blob != nullptr && blob->type == "CCompilerBlobCache") {
            lp.timeVaryingProgram = descriptorFromBlob(model, *blob, arena);
        }
    }

    lp.program = lp.timeFixedProgram;
}

} // namespace whiteout::cornflakes
