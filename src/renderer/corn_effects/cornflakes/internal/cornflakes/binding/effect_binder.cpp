#include "binding_internal.hpp"

#include <cornflakes/interface/binding/effect_binder.hpp>
#include <cornflakes/core/determinism.hpp>

#include <cstring>
#include <optional>
#include <vector>

namespace whiteout::cornflakes {

std::string_view stableCopy(std::string_view src, IArena& arena) {
    if (src.empty()) {
        return {};
    }
    auto* p = static_cast<char*>(arena.allocate(src.size(), 1));
    std::memcpy(p, src.data(), src.size());
    return {p, src.size()};
}

namespace {

// ---- Asset utility helpers -----------------------------------------------

const AssetObject* findRootEffect(const EffectAssetModel& model) noexcept {
    for (const auto& obj : model.objects) {
        if (obj.type == "CParticleEffect") {
            return &obj;
        }
    }
    return nullptr;
}

std::size_t countNodeGraphs(const EffectAssetModel& model) noexcept {
    std::size_t n = 0;
    for (const auto& obj : model.objects) {
        if (obj.type == "CParticleNodeGraph") {
            ++n;
        }
    }
    return n;
}

// ---- Small per-section loaders (large ones live in *_binder.cpp) -------

void loadSpatialLayers(const EffectAssetModel& model, const AssetObject& layerCache,
                       LayerProgram& lp, IArena& arena) {
    const auto spatialUids = fieldLinks(layerCache, "SpatialLayers");
    if (spatialUids.empty()) {
        return;
    }
    const auto layerArr = arenaArray<SpatialLayerResource>(arena, spatialUids.size());
    std::size_t written = 0;

    for (const u32 slUid : spatialUids) {
        const AssetObject* slObj = findObjectByUid(model, slUid);
        if (slObj == nullptr || slObj->type != "CLayerCompileCacheSpatialLayer") {
            continue;
        }
        SpatialLayerResource& res = layerArr[written++];
        res.name = stableCopy(fieldString(*slObj, "SpatialLayerLocalName"), arena);
        res.fullName = stableCopy(fieldString(*slObj, "SpatialLayerName"), arena);
        res.cellSize = fieldFloat(*slObj, "SpatialLayerCellSize").value_or(0.75F);
        if (res.cellSize <= 0.0F) {
            res.cellSize = 0.75F;
        }
        res.flags = fieldUint(*slObj, "SpatialLayerFlags").value_or(1U);

        const auto payloadUids = fieldLinks(*slObj, "SpatialLayerPayload");
        if (payloadUids.empty()) {
            continue;
        }
        const auto plArr = arenaArray<SpatialLayerPayload>(arena, payloadUids.size());
        std::size_t plWritten = 0;
        for (const u32 pUid : payloadUids) {
            const AssetObject* pObj = findObjectByUid(model, pUid);
            if (pObj == nullptr || pObj->type != "CLayerCompileCacheSpatialLayerPayload") {
                continue;
            }
            SpatialLayerPayload& pl = plArr[plWritten++];
            pl.name = stableCopy(fieldString(*pObj, "PayloadName"), arena);
            pl.payloadType = fieldUint(*pObj, "PayloadType").value_or(0U);
            pl.payloadFlags = fieldUint(*pObj, "PayloadFlags").value_or(0U);
        }
        if (plWritten > 0) {
            res.payloads = std::span<const SpatialLayerPayload>{plArr.data(), plWritten};
        }
    }
    if (written > 0) {
        lp.spatialLayers = std::span<const SpatialLayerResource>{layerArr.data(), written};
    }
}

void loadAttributeDefaults(const EffectAssetModel& model, const AssetObject& layerCache,
                           LayerProgram& lp, IArena& arena) {
    const auto attribUids = fieldLinks(layerCache, "Attribs");
    if (attribUids.empty()) {
        return;
    }
    const auto attrArr = arenaArray<AttributeDefault>(arena, attribUids.size());
    std::size_t written = 0;
    for (const u32 aUid : attribUids) {
        const AssetObject* aObj = findObjectByUid(model, aUid);
        if (aObj == nullptr || aObj->type != "CLayerCompileCacheAttrib") {
            continue;
        }
        AttributeDefault& out = attrArr[written++];
        out.name = stableCopy(fieldString(*aObj, "AttrName"), arena);
        const auto bytes = fieldBytes(*aObj, "AttrDefaultValueF4");
        if (bytes.size() >= 4U * sizeof(f32)) {
            std::memcpy(out.defaultValue.data(), bytes.data(), 4U * sizeof(f32));
        }
    }
    if (written > 0) {
        lp.attributeDefaults = std::span<const AttributeDefault>{attrArr.data(), written};
    }
}

// Top-level driver — calls each `load*` step that the file split exposes.
void populateLayerPrograms(const EffectAssetModel& model, const AssetObject& layerCache,
                           LayerProgram& lp, IArena& arena) {
    loadScopePrograms(model, layerCache, lp, arena);
    loadRenderers(model, layerCache, lp, arena);
    loadSamplers(model, layerCache, lp, arena);
    loadSpatialLayers(model, layerCache, lp, arena);
    loadAttributeDefaults(model, layerCache, lp, arena);
}

// ---- External canonicalisation -----------------------------------------

// Walks every scope's externals and assigns each binding a canonical slot
// index that's stable across scopes (same `name` → same canonical slot). This
// is what lets the runtime store one external slot per effect-wide name even
// though different scopes have different per-scope slot layouts.
void canonicaliseLayerExternals(LayerProgram& lp) {
    std::vector<std::pair<std::string_view, u16>> nameToCanonical;
    auto canonicalIdxFor = [&](std::string_view name) -> u16 {
        for (const auto& [n, idx] : nameToCanonical) {
            if (n == name) {
                return idx;
            }
        }
        const auto next = static_cast<u16>(nameToCanonical.size());
        nameToCanonical.emplace_back(name, next);
        return next;
    };
    auto canonicalisePass = [&](std::span<const ExternalBinding> exts) {
        for (auto& b : exts) {
            // canonicalSlot is the only field this pass writes; the cast
            // confines mutation to that slot. The arena owns the storage so
            // the lifetime is fine.
            const_cast<ExternalBinding&>(b).canonicalSlot = canonicalIdxFor(b.name);
        }
    };
    canonicalisePass(lp.initProgram.externals);
    canonicalisePass(lp.physicsProgram.externals);
    canonicalisePass(lp.timeFixedProgram.externals);
    canonicalisePass(lp.timeVaryingProgram.externals);
    canonicalisePass(lp.program.externals);
}

// ---- Baked layer graph binding -----------------------------------------

struct EventSlotMetadata {
    std::string_view name;
    i32 parentLayerSlot = -1;
    std::span<const u32> layerTargets;
};

// Reads the EventSlots table from the LayerGraphCompileCache. Returns the
// per-slot metadata plus the running total of (slot, target) route rows.
struct EventSlotTable {
    std::vector<EventSlotMetadata> slots;
    std::size_t totalRouteRows = 0;
};

EventSlotTable loadEventSlots(const EffectAssetModel& model, std::span<const u32> eventSlotUids,
                              IArena& arena) {
    EventSlotTable t;
    t.slots.reserve(eventSlotUids.size());
    for (const u32 evtUid : eventSlotUids) {
        EventSlotMetadata meta;
        if (const AssetObject* evt = findObjectByUid(model, evtUid);
            evt != nullptr && evt->type == "CLayerGraphCompileCache_EventSlot") {
            meta.name = stableCopy(fieldString(*evt, "EventName"), arena);
            meta.parentLayerSlot = fieldInt(*evt, "ParentLayerSlot").value_or(-1);
            meta.layerTargets = fieldUintArray(*evt, "LayerTargets");
        }
        t.totalRouteRows += meta.layerTargets.size();
        t.slots.push_back(meta);
    }
    return t;
}

// Maps each layer slot index to the global event slot ids it owns.
std::vector<std::span<const u32>> loadLayerOwnedEventSlots(const EffectAssetModel& model,
                                                            std::span<const u32> layerSlotUids) {
    std::vector<std::span<const u32>> out;
    out.reserve(layerSlotUids.size());
    for (const u32 slotUid : layerSlotUids) {
        std::span<const u32> owned;
        if (const AssetObject* slot = findObjectByUid(model, slotUid)) {
            owned = fieldUintArray(*slot, "EventSlots");
        }
        out.push_back(owned);
    }
    return out;
}

// Resolves a single LayerSlot UID into a fully-bound LayerProgram. Returns
// nullopt when the slot or its LayerCache reference is malformed.
std::optional<LayerProgram> buildLayerFromSlot(const EffectAssetModel& model, u32 slotUid,
                                                u32 layerId, IArena& arena) {
    const AssetObject* slot = findObjectByUid(model, slotUid);
    if (slot == nullptr) {
        return std::nullopt;
    }
    const auto cacheUid = fieldLink(*slot, "LayerCache");
    if (!cacheUid) {
        return std::nullopt;
    }
    const AssetObject* cache = findObjectByUid(model, *cacheUid);
    if (cache == nullptr || cache->type != "CLayerCompileCache") {
        return std::nullopt;
    }
    LayerProgram lp;
    lp.id = LayerId{layerId};
    lp.sourceUid = stableCopy(cache->uid, arena);
    populateLayerPrograms(model, *cache, lp, arena);
    canonicaliseLayerExternals(lp);
    return lp;
}

// Attaches the event-external bindings owned by `layerSlotIdx` to `lp`.
void attachLayerOwnedEvents(LayerProgram& lp, std::span<const u32> ownedIds,
                            std::span<const EventSlotMetadata> eventSlots, IArena& arena) {
    if (ownedIds.empty()) {
        return;
    }
    const auto bindings = arenaArray<EventExternalBinding>(arena, ownedIds.size());
    std::size_t written = 0;
    for (const u32 globalSlotId : ownedIds) {
        if (globalSlotId >= eventSlots.size()) {
            continue;
        }
        EventExternalBinding& b = bindings[written++];
        b.externalName = eventSlots[globalSlotId].name;
        b.globalEventSlotId = globalSlotId;
    }
    if (written > 0) {
        lp.eventExternals = std::span<const EventExternalBinding>{bindings.data(), written};
    }
}

// Builds the EventRoutingTable from the per-slot LayerTargets fan-out.
EventRoutingTable buildEventRoutes(std::span<const EventSlotMetadata> eventSlots,
                                   std::span<const LayerProgram> layers, std::size_t totalRouteRows,
                                   IArena& arena) {
    EventRoutingTable out;
    if (totalRouteRows == 0) {
        return out;
    }
    const auto routeArr = arenaArray<EventRoute>(arena, totalRouteRows);
    std::size_t written = 0;
    for (std::size_t globalSlotId = 0; globalSlotId < eventSlots.size(); ++globalSlotId) {
        const auto& meta = eventSlots[globalSlotId];
        for (const u32 targetSlotIdx : meta.layerTargets) {
            if (targetSlotIdx >= layers.size()) {
                continue;
            }
            EventRoute& r = routeArr[written++];
            r.channel = meta.name;
            r.target = layers[targetSlotIdx].id;
            r.globalEventSlotId = static_cast<u32>(globalSlotId);
            r.parentLayerSlot = meta.parentLayerSlot;
        }
    }
    if (written > 0) {
        out.routes = std::span<const EventRoute>{routeArr.data(), written};
    }
    return out;
}

std::span<LayerProgram> bindBakedLayers(const EffectAssetModel& model, const AssetObject& effect,
                                        IArena& arena, EventRoutingTable& outRouting) {
    outRouting = {};

    const auto graphUid = fieldLink(effect, "LayerGraphCompileCache");
    if (!graphUid) {
        return {};
    }
    const AssetObject* graph = findObjectByUid(model, *graphUid);
    if (graph == nullptr || graph->type != "CLayerGraphCompileCache") {
        return {};
    }
    const auto layerSlotUids = fieldLinks(*graph, "LayerSlots");
    if (layerSlotUids.empty()) {
        return {};
    }

    const auto eventSlotTable = loadEventSlots(model, fieldLinks(*graph, "EventSlots"), arena);
    const auto layerOwnedEventSlots = loadLayerOwnedEventSlots(model, layerSlotUids);

    std::vector<LayerProgram> built;
    built.reserve(layerSlotUids.size());

    u32 nextLayerId = 0;
    for (std::size_t i = 0; i < layerSlotUids.size(); ++i) {
        auto lp = buildLayerFromSlot(model, layerSlotUids[i], nextLayerId, arena);
        if (!lp) {
            continue;
        }
        ++nextLayerId;
        attachLayerOwnedEvents(*lp, layerOwnedEventSlots[i], eventSlotTable.slots, arena);
        built.push_back(*lp);
    }

    if (built.empty()) {
        return {};
    }
    auto out = arenaArray<LayerProgram>(arena, built.size());
    for (std::size_t i = 0; i < built.size(); ++i) {
        out[i] = built[i];
    }

    outRouting = buildEventRoutes(eventSlotTable.slots, std::span<const LayerProgram>{out},
                                  eventSlotTable.totalRouteRows, arena);
    return out;
}

// ---- Fallback: bare CParticleNodeGraph layout (no LayerGraphCompileCache) ---

std::span<LayerProgram> bindNodeGraphLayers(const EffectAssetModel& model, IArena& arena) {
    const std::size_t layerCount = countNodeGraphs(model);
    if (layerCount == 0) {
        return {};
    }
    auto layerSpan = arenaArray<LayerProgram>(arena, layerCount);
    std::size_t rootOut = 0;
    std::size_t nonRootOut = 1;

    u32 nextLayerId = 0;
    for (const auto& obj : model.objects) {
        if (obj.type != "CParticleNodeGraph") {
            continue;
        }
        LayerProgram lp;
        lp.id = LayerId{nextLayerId++};
        lp.name = stableCopy(obj.customName, arena);
        lp.sourceUid = stableCopy(obj.uid, arena);

        const bool isRoot = obj.customName == "Root";
        if (isRoot) {
            layerSpan[rootOut] = lp;
        } else {
            const std::size_t dst = nonRootOut < layerCount ? nonRootOut : layerCount - 1;
            layerSpan[dst] = lp;
            ++nonRootOut;
        }
    }
    return layerSpan;
}

} // namespace

std::optional<EffectExecutionPlan> EffectBinder::bind(const EffectAssetModel& model, EffectId id,
                                                      IArena& planArena, IssueBag&) const {
    EffectExecutionPlan plan;
    plan.id = id;
    plan.version = model.version;
    plan.generator = model.generator;

    if (model.objects.empty()) {
        return plan;
    }

    if (const AssetObject* effect = findRootEffect(model)) {
        EventRoutingTable routing;
        if (auto baked = bindBakedLayers(model, *effect, planArena, routing); !baked.empty()) {
            plan.layers = baked;
            plan.eventRouting = routing;
            return plan;
        }
    }

    // Fallback path for assets that haven't been through the compile-cache
    // pipeline — synthesise a minimal layer list from the raw NodeGraphs.
    if (auto fallback = bindNodeGraphLayers(model, planArena); !fallback.empty()) {
        plan.layers = fallback;
    }
    return plan;
}

} // namespace whiteout::cornflakes
