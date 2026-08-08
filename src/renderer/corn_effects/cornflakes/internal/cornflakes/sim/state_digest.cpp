#include <cornflakes/interface/sim/effect_runtime.hpp>
#include <cornflakes/interface/sim/state_digest.hpp>

#include <cstring>

namespace whiteout::cornflakes {

void DigestHasher::mixSpawnEvent(const SpawnEvent& ev) noexcept {
    DigestHasher& h = *this;
    h.mixU32(ev.eventId);
    for (const i32 p : ev.payload) {
        h.mixU32(static_cast<u32>(p));
    }
    h.mixU32(ev.sequenceIndex);
    h.mixU64(ev.parentSelfId);
    h.mixU32(ev.parentRngState);

    h.mixBool(ev.hasSpawnPosition);
    for (const f32 v : ev.spawnPosition) {
        h.mixF32(v);
    }
    h.mixU32(ev.spawnPositionPayloadId);
    h.mixBool(ev.hasSpawnOrientation);
    for (const f32 v : ev.spawnOrientation) {
        h.mixF32(v);
    }
    h.mixU32(ev.spawnOrientationPayloadId);

    h.mixBool(ev.hasIntPayload);
    h.mixU8(ev.intPayloadWidth);
    for (const i32 v : ev.intPayload) {
        h.mixU32(static_cast<u32>(v));
    }
    h.mixU32(ev.intPayloadId);
    h.mixBool(ev.hasBoolPayload);
    h.mixU8(ev.boolPayloadWidth);
    for (const i32 v : ev.boolPayload) {
        h.mixU32(static_cast<u32>(v));
    }
    h.mixU32(ev.boolPayloadId);

    for (const auto& slot : ev.floatSlots) {
        h.mixU32(slot.nameId);
        h.mixBool(slot.valid);
        h.mixU8(slot.width);
        for (const f32 v : slot.value) {
            h.mixF32(v);
        }
    }

    h.mixF32(ev.subFrameFraction);
    h.mixF32(ev.lerpedTime);
}

void DigestHasher::mixProximityEntry(const ProximityEntry& e) noexcept {
    mixU32(e.insertSeq);
    for (const f32 v : e.position) {
        mixF32(v);
    }
    for (const f32 v : e.payload) {
        mixF32(v);
    }
    mixU64(e.sourceSelfId);
    mixU8(e.payloadCount);
    for (u8 p = 0; p < e.payloadCount && p < kMaxProximityPayloads; ++p) {
        mixU32(e.payloads[p].nameHash);
        mixU8(e.payloads[p].components);
        for (const f32 v : e.payloads[p].value) {
            mixF32(v);
        }
    }
}

bool DigestHasher::isDefaultRegister(const RegisterValue& rv) noexcept {
    for (const f32 lane : rv.lanes) {
        u32 bits = 0U;
        std::memcpy(&bits, &lane, sizeof(bits));
        if (bits != 0U) {
            return false;
        }
    }
    return rv.componentCount == 0U && rv.typeBank == 0U;
}

void DigestHasher::mixF32(f32 v) noexcept {
    u32 bits = 0U;
    std::memcpy(&bits, &v, sizeof(bits));
    mixU32(bits);
}

std::string EffectStateDigest::toHex() const {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out(16, '0');
    u64 v = combined;
    for (int i = 15; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = kHex[v & 0xFULL];
        v >>= 4;
    }
    return out;
}

EffectStateDigest digestEffect(const EffectRuntime& rt) {
    EffectStateDigest out;
    const std::size_t layerCount = rt.layerCount();
    out.layers.resize(layerCount);

    for (std::size_t layerIdx = 0; layerIdx < layerCount; ++layerIdx) {
        auto& ld = out.layers[layerIdx];

        DigestHasher ph;
        const std::size_t slots = rt.poolSize(layerIdx);
        ld.slotCount = slots;
        ph.mixSize(slots);

        for (std::size_t slot = 0; slot < slots; ++slot) {
            const auto p = rt.inspectParticle(layerIdx, slot);
            if (!p.valid) {
                ph.mixU8(0xFFU);
                continue;
            }
            if (!p.dead) {
                ++ld.aliveCount;
            }
            ph.mixSize(p.slotIndex);
            ph.mixU32(p.rngState);
            ph.mixF32(p.lifeRatio);
            ph.mixU64(p.selfId);
            ph.mixU64(p.parentSelfId);
            ph.mixBool(p.dead);
            ph.mixBool(p.wasDeadAtFrameStart);
            ph.mixRegisterSpan(p.externals);
            ph.mixRegisterSpan(p.streamRegisters);
            ph.mixRegisterSpan(p.localRegisters);
        }
        ld.particles = ph.value();

        DigestHasher sh;
        const auto events = rt.spawnQueueEvents(layerIdx);
        ld.spawnQueueLength = events.size();
        sh.mixSize(events.size());
        for (const auto& ev : events) {
            sh.mixSpawnEvent(ev);
        }
        ld.spawnQueue = sh.value();
    }

    DigestHasher xh;
    const std::size_t hashCount = rt.spatialHashCount();
    xh.mixSize(hashCount);
    for (std::size_t i = 0; i < hashCount; ++i) {
        const auto name = rt.spatialHashName(i);
        xh.mixSize(name.size());
        for (const char c : name) {
            xh.mixU8(static_cast<u8>(c));
        }
        const auto entries = rt.spatialHashEntries(i);
        xh.mixSize(entries.size());
        for (const auto& e : entries) {
            xh.mixProximityEntry(e);
        }
    }
    out.spatial = xh.value();

    DigestHasher ch;
    ch.mixSize(layerCount);
    for (const auto& ld : out.layers) {
        ch.mixU64(ld.particles);
        ch.mixU64(ld.spawnQueue);
        ch.mixSize(ld.slotCount);
        ch.mixSize(ld.aliveCount);
        ch.mixSize(ld.spawnQueueLength);
    }
    ch.mixU64(out.spatial);
    out.combined = ch.value();
    return out;
}

}
