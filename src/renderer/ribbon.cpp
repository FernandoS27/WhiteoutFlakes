#include "renderer/ribbon.h"
#include "sim_util.h"

namespace WhiteoutDex {

void RibbonSystem::Clear() { emitters_.clear(); }

void RibbonSystem::AddEmitter(i32 id, const RibbonEmitterConfig& cfg) {
    auto& em = emitters_[id];
    em.config = cfg;
    em.segments.clear();
    em.accumEmission = 0;
    em.startTime = 0;
    em.posSet = false;
}

void RibbonSystem::UpdateEmitterState(i32 id, const RibbonEmitterState& st) {
    auto it = emitters_.find(id);
    if (it == emitters_.end()) return;
    auto& em = it->second;

    em.state = st;

    Vector3f newPos  = whiteout::transform_point(Vector3f{0,0,0}, st.transform);
    Vector3f newDir   = whiteout::transform_normal(Vector3f{0,0,1}, st.transform).normalized();
    Vector3f newVert  = whiteout::transform_normal(Vector3f{0,1,0}, st.transform).normalized();

    if (em.posSet) {
        em.prevPos      = em.currPos;
        em.prevDir      = em.currDir;
        em.prevVertical = em.currVertical;
    } else {
        em.prevPos      = newPos;
        em.prevDir      = newDir;
        em.prevVertical = newVert;
        em.startTime    = 0;
        em.posSet       = true;
    }
    em.currPos      = newPos;
    em.currDir      = newDir;
    em.currVertical = newVert;
}

bool RibbonSystem::HasEmitters() const { return !emitters_.empty(); }

void RibbonSystem::Simulate(f32 dt) {
    dt = ClampDeltaTime(dt);

    for (auto& [id, em] : emitters_) {
        f32 lifeSpan = em.config.life;
        if (lifeSpan < kRibbonMinLifespan) lifeSpan = kRibbonMinLifespan;

        if (dt >= lifeSpan) {
            em.segments.clear();
            em.prevPos      = em.currPos;
            em.prevDir      = em.currDir;
            em.prevVertical = em.currVertical;
            em.startTime    = 0;
            dt = 0;
        }

        em.segments.erase(em.segments.begin(),
            std::find_if(em.segments.begin(), em.segments.end(),
                [lifeSpan, dt](const RibbonSegment& s) {
                    return s.age < (lifeSpan - dt);
                }));

        bool emittedHead = false;
        if (dt > 0 && IsEmitterVisible(em.state.visibility) &&
            em.config.emission > 0 && em.posSet)
        {
            f32 edgesPerSec = em.config.emission;
            f32 endTime     = em.startTime + dt * edgesPerSec;
            f32 newEdgeTime = 1.0f;

            if (endTime >= 1.0f) {
                i32   numNew  = (i32)floorf(endTime - newEdgeTime) + 1;
                f32 ooDenom = (endTime - em.startTime > kVectorEpsilon)
                                ? 1.0f / (endTime - em.startTime) : 1.0f;

                f32 dx = em.currPos.x - em.prevPos.x;
                f32 dy = em.currPos.y - em.prevPos.y;
                f32 dz = em.currPos.z - em.prevPos.z;
                f32 dist = sqrtf(dx*dx + dy*dy + dz*dz);

                Vector3f prevDirS = {em.prevDir.x*dist, em.prevDir.y*dist, em.prevDir.z*dist};
                Vector3f currDirS = {em.currDir.x*dist, em.currDir.y*dist, em.currDir.z*dist};

                Vector3f above0 = {em.prevPos.x + em.prevVertical.x * em.state.above,
                                   em.prevPos.y + em.prevVertical.y * em.state.above,
                                   em.prevPos.z + em.prevVertical.z * em.state.above};
                Vector3f above1 = {em.currPos.x + em.currVertical.x * em.state.above,
                                   em.currPos.y + em.currVertical.y * em.state.above,
                                   em.currPos.z + em.currVertical.z * em.state.above};
                Vector3f below0 = {em.prevPos.x - em.prevVertical.x * em.state.below,
                                   em.prevPos.y - em.prevVertical.y * em.state.below,
                                   em.prevPos.z - em.prevVertical.z * em.state.below};
                Vector3f below1 = {em.currPos.x - em.currVertical.x * em.state.below,
                                   em.currPos.y - em.currVertical.y * em.state.below,
                                   em.currPos.z - em.currVertical.z * em.state.below};

                for (i32 i = 0; i < numNew; ++i) {
                    f32 t = (newEdgeTime - em.startTime) * ooDenom;
                    if (t < 0) t = 0;
                    if (t > 1) t = 1;
                    f32 omt = 1.0f - t;

                    RibbonSegment seg;

                    seg.bot = {
                        (below0.x + prevDirS.x*t)*omt + (below1.x - currDirS.x*omt)*t,
                        (below0.y + prevDirS.y*t)*omt + (below1.y - currDirS.y*omt)*t,
                        (below0.z + prevDirS.z*t)*omt + (below1.z - currDirS.z*omt)*t
                    };
                    seg.top = {
                        (above0.x + prevDirS.x*t)*omt + (above1.x - currDirS.x*omt)*t,
                        (above0.y + prevDirS.y*t)*omt + (above1.y - currDirS.y*omt)*t,
                        (above0.z + prevDirS.z*t)*omt + (above1.z - currDirS.z*omt)*t
                    };

                    seg.age = -dt * t;
                    em.segments.push_back(seg);
                    newEdgeTime += 1.0f;
                }
            }

            em.startTime = endTime - floorf(endTime);

            RibbonSegment head;
            head.top = {em.currPos.x + em.currVertical.x * em.state.above,
                        em.currPos.y + em.currVertical.y * em.state.above,
                        em.currPos.z + em.currVertical.z * em.state.above};
            head.bot = {em.currPos.x - em.currVertical.x * em.state.below,
                        em.currPos.y - em.currVertical.y * em.state.below,
                        em.currPos.z - em.currVertical.z * em.state.below};
            head.age = 0;
            em.segments.push_back(head);
            emittedHead = true;
        }

        usize updateEnd = emittedHead ? em.segments.size() - 1
                                       : em.segments.size();
        for (usize i = 0; i < updateEnd; ++i) {
            auto& seg = em.segments[i];

            f32 dz = em.config.gravity * dt * dt
                     + 2.0f * em.config.gravity * seg.age * dt;
            seg.top.z -= dz;
            seg.bot.z -= dz;
            seg.age   += dt;
        }
    }
}

RibbonSystem::StripResult RibbonSystem::BuildStrips() const
{
    StripResult result;

    for (auto& [id, em] : emitters_) {
        if (em.state.visibility <= 0.0f) continue;
        if (em.segments.size() < 2)      continue;

        i32  startIdx = (i32)result.vertices.size();
        auto& segs    = em.segments;
        i32   numSegs = (i32)segs.size();

        f32 cellW = (em.config.cols > 0) ? 1.0f / em.config.cols : 1.0f;
        f32 cellH = (em.config.rows > 0) ? 1.0f / em.config.rows : 1.0f;
        i32   slotRow = (em.config.cols > 0) ? em.state.slot / em.config.cols : 0;
        i32   slotCol = (em.config.cols > 0) ? em.state.slot % em.config.cols : 0;
        f32 texL = cellW * slotCol;
        f32 texT = cellH * slotRow;
        f32 texR = texL + cellW;
        f32 texB = texT + cellH;
        f32 texDU = texR - texL;

        f32 lifeSpan = em.config.life;
        if (lifeSpan < 0.25f) lifeSpan = 0.25f;
        f32 ooLife = 1.0f / lifeSpan;

        Vector4f vertColor = {em.state.color.x, em.state.color.y, em.state.color.z,
                              em.state.alpha};

        Vector3f normal = {1, 0, 0};

        for (i32 i = 0; i < numSegs - 1; i++) {
            const auto& s0 = segs[i];
            const auto& s1 = segs[i + 1];

            f32 u0 = texDU * s0.age * ooLife + texL;
            f32 u1 = texDU * s1.age * ooLife + texL;

            result.vertices.push_back({s0.top, normal, vertColor, {u0, texT}});
            result.vertices.push_back({s0.bot, normal, vertColor, {u0, texB}});
            result.vertices.push_back({s1.top, normal, vertColor, {u1, texT}});

            result.vertices.push_back({s0.bot, normal, vertColor, {u0, texB}});
            result.vertices.push_back({s1.bot, normal, vertColor, {u1, texB}});
            result.vertices.push_back({s1.top, normal, vertColor, {u1, texT}});
        }

        if ((i32)result.vertices.size() > startIdx)
            result.emitterIds.push_back(id);
    }

    return result;
}

const RibbonEmitterConfig* RibbonSystem::GetConfig(i32 id) const {
    auto it = emitters_.find(id);
    return (it != emitters_.end()) ? &it->second.config : nullptr;
}

i32 RibbonSystem::GetTotalSegmentCount() const {
    i32 total = 0;
    for (auto& [id, em] : emitters_) total += (i32)em.segments.size();
    return total;
}

i32 RibbonSystem::GetEmitterVertCount(i32 emitterId) const {
    auto it = emitters_.find(emitterId);
    if (it == emitters_.end()) return 0;
    i32 segs = (i32)it->second.segments.size();
    return (segs > 1) ? (segs - 1) * 6 : 0;
}

}
