#include "renderer/particle/particle_trace.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace whiteout::flakes::renderer::particle {

namespace {

constexpr const char* kMagic = "wpt2";
/// The format before the output column; still read.
constexpr const char* kMagicV1 = "wpt1";

inline u64 EmitterIndexKey(ModelId model, u8 output, i32 id) {
    return (static_cast<u64>(model) << 40) | (static_cast<u64>(output) << 32) |
           static_cast<u32>(id);
}

// FNV-1a over the exact bit patterns, so the hash is an equality test rather
// than a similarity test. Tolerance comparisons switch it off instead of
// loosening it.
inline void HashF32(u64& h, f32 v) {
    u32 bits;
    std::memcpy(&bits, &v, sizeof(bits));
    for (i32 b = 0; b < 4; ++b) {
        h ^= static_cast<u8>((bits >> (b * 8)) & 0xFF);
        h *= 0x100000001B3ull;
    }
}

// %.9g round-trips a float exactly, which is what makes a recorded baseline a
// bit-identical check rather than an approximate one.
std::string F(f32 v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.9g", v);
    return buf;
}

} // namespace

void CaptureFrame(const ParticleService& svc, const Matrix44f& worldToView, i32 frame, Trace& out) {
    TraceFrame tf;
    tf.frame = frame;

    // L1 — pool state, straight off each emitter.
    std::unordered_map<u64, usize> index;
    svc.ForEachEmitter([&](const EmitterKey& k, const ParticleEmitter& e) {
        TraceEmitter te;
        te.output = static_cast<u8>(k.output);
        te.model = k.model;
        te.emitterId = k.id;
        te.priorityPlane = e.DrawHeader().priorityPlane;

        const ParticlePool& pool = e.Pool();
        te.particles.reserve(pool.AliveCount());
        for (usize i = 0; i < pool.AliveCount(); ++i) {
            const Particle2& p = pool[pool.AliveAt(i)];
            te.particles.push_back({p.position, p.velocity, p.age, p.aux});
        }

        index[EmitterIndexKey(k.model, static_cast<u8>(k.output), k.id)] = tf.emitters.size();
        tf.emitters.push_back(std::move(te));
    });

    // L2 — the emitter's slice of the frame's vertex stream. Goes through the
    // service's own BuildGeometry so the trace sees the real fog configuration
    // and draw-list splitting rather than a reconstruction of them.
    std::vector<Vertex> verts;
    std::vector<EmitterDrawList> draws;
    svc.BuildGeometry(worldToView, verts, draws);

    for (const auto& dl : draws) {
        // Draw lists only ever come from billboard emitters.
        auto it = index.find(EmitterIndexKey(
            dl.model, static_cast<u8>(ParticleOutput::Billboard), dl.emitterId));
        if (it == index.end())
            continue;
        TraceEmitter& te = tf.emitters[it->second];
        te.vertexCount = dl.vertexCount;
        te.priorityPlane = dl.priorityPlane;

        u64 h = 0xCBF29CE484222325ull;
        Vector3f lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
        f64 sum[4] = {0, 0, 0, 0};
        const i32 begin = dl.vertexOffset;
        const i32 end = dl.vertexOffset + dl.vertexCount;
        for (i32 v = begin; v < end && v < (i32)verts.size(); ++v) {
            const Vertex& vt = verts[v];
            for (i32 c = 0; c < 3; ++c) {
                HashF32(h, vt.position.data[c]);
                lo.data[c] = (std::min)(lo.data[c], vt.position.data[c]);
                hi.data[c] = (std::max)(hi.data[c], vt.position.data[c]);
            }
            for (i32 c = 0; c < 4; ++c) {
                HashF32(h, vt.color.data[c]);
                sum[c] += vt.color.data[c];
            }
            HashF32(h, vt.uv.x);
            HashF32(h, vt.uv.y);
        }
        te.vertexHash = h;
        if (dl.vertexCount > 0) {
            te.boundsMin = lo;
            te.boundsMax = hi;
            for (i32 c = 0; c < 4; ++c)
                te.meanColor.data[c] = static_cast<f32>(sum[c] / dl.vertexCount);
        }
    }

    // Stable order so the file is diffable and comparison is positional.
    std::sort(tf.emitters.begin(), tf.emitters.end(), [](const TraceEmitter& a, const TraceEmitter& b) {
        if (a.model != b.model)
            return a.model < b.model;
        if (a.output != b.output)
            return a.output < b.output;
        return a.emitterId < b.emitterId;
    });

    out.frames.push_back(std::move(tf));
}

bool WriteTrace(const Trace& t, const std::string& path, std::string& err) {
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        err = "cannot open for write: " + path;
        return false;
    }
    f << kMagic << "\n";
    for (const auto& fr : t.frames) {
        f << "f " << fr.frame << " " << fr.emitters.size() << "\n";
        for (const auto& e : fr.emitters) {
            f << "e " << e.model << " " << (i32)e.output << " " << e.emitterId << " " << e.particles.size() << " "
              << e.vertexCount << " " << e.vertexHash << " " << e.priorityPlane << " "
              << F(e.boundsMin.x) << " " << F(e.boundsMin.y) << " " << F(e.boundsMin.z) << " "
              << F(e.boundsMax.x) << " " << F(e.boundsMax.y) << " " << F(e.boundsMax.z) << " "
              << F(e.meanColor.x) << " " << F(e.meanColor.y) << " " << F(e.meanColor.z) << " "
              << F(e.meanColor.w) << "\n";
            for (const auto& p : e.particles) {
                f << "p " << F(p.position.x) << " " << F(p.position.y) << " " << F(p.position.z)
                  << " " << F(p.velocity.x) << " " << F(p.velocity.y) << " " << F(p.velocity.z)
                  << " " << F(p.age) << " " << p.aux << "\n";
            }
        }
    }
    return true;
}

bool ReadTrace(Trace& t, const std::string& path, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        err = "cannot open for read: " + path;
        return false;
    }
    // v1 had no output-kind column. Reading it is still useful: a baseline
    // recorded before the output axis existed is exactly what the step that
    // introduced it needs to be checked against.
    std::string magic;
    std::getline(f, magic);
    const bool hasOutputColumn = (magic.rfind(kMagic, 0) == 0);
    if (!hasOutputColumn && magic.rfind(kMagicV1, 0) != 0) {
        err = "bad magic in " + path;
        return false;
    }
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty())
            continue;
        std::istringstream is(line);
        char tag = 0;
        is >> tag;
        if (tag == 'f') {
            TraceFrame fr;
            usize n = 0;
            is >> fr.frame >> n;
            t.frames.push_back(std::move(fr));
        } else if (tag == 'e') {
            if (t.frames.empty()) {
                err = "emitter record before any frame";
                return false;
            }
            TraceEmitter e;
            usize np = 0;
            i32 outKind = 0;
            is >> e.model;
            if (hasOutputColumn)
                is >> outKind;
            is >> e.emitterId >> np >> e.vertexCount >> e.vertexHash >>
                e.priorityPlane >> e.boundsMin.x >> e.boundsMin.y >> e.boundsMin.z >>
                e.boundsMax.x >> e.boundsMax.y >> e.boundsMax.z >> e.meanColor.x >>
                e.meanColor.y >> e.meanColor.z >> e.meanColor.w;
            e.output = static_cast<u8>(outKind);
            e.particles.reserve(np);
            t.frames.back().emitters.push_back(std::move(e));
        } else if (tag == 'p') {
            if (t.frames.empty() || t.frames.back().emitters.empty()) {
                err = "particle record before any emitter";
                return false;
            }
            TraceParticle p;
            is >> p.position.x >> p.position.y >> p.position.z >> p.velocity.x >> p.velocity.y >>
                p.velocity.z >> p.age >> p.aux;
            t.frames.back().emitters.back().particles.push_back(p);
        }
    }
    return true;
}

namespace {

// Zero tolerance means bit-identical. Otherwise the tolerance is relative to
// the magnitude involved: particle coordinates run into the thousands, where an
// absolute epsilon would reject differences that are just float rounding, while
// still being far too loose near the origin.
bool Near(f32 a, f32 b, f32 eps) {
    if (eps <= 0.0f)
        return a == b;
    const f32 scale = (std::max)(1.0f, (std::max)(std::fabs(a), std::fabs(b)));
    return std::fabs(a - b) <= eps * scale;
}

std::string Where(i32 frame, const TraceEmitter& e) {
    std::ostringstream os;
    os << "frame " << frame << ", emitter (" << e.model << "," << e.emitterId << ")";
    return os.str();
}

} // namespace

bool CompareTraces(const Trace& baseline, const Trace& actual, const CompareTolerance& tol,
                   std::string& report) {
    std::ostringstream os;

    if (baseline.frames.size() != actual.frames.size()) {
        os << "frame count differs: baseline " << baseline.frames.size() << " vs actual "
           << actual.frames.size();
        report = os.str();
        return false;
    }

    for (usize fi = 0; fi < baseline.frames.size(); ++fi) {
        const TraceFrame& b = baseline.frames[fi];
        const TraceFrame& a = actual.frames[fi];
        if (b.emitters.size() != a.emitters.size()) {
            os << "frame " << b.frame << ": emitter count differs, baseline " << b.emitters.size()
               << " vs actual " << a.emitters.size();
            report = os.str();
            return false;
        }
        for (usize ei = 0; ei < b.emitters.size(); ++ei) {
            const TraceEmitter& be = b.emitters[ei];
            const TraceEmitter& ae = a.emitters[ei];
            if (be.model != ae.model || be.output != ae.output || be.emitterId != ae.emitterId) {
                os << Where(b.frame, be) << ": identity differs, actual (" << ae.model << ","
                   << ae.emitterId << ")";
                report = os.str();
                return false;
            }
            if (be.particles.size() != ae.particles.size()) {
                os << Where(b.frame, be) << ": alive count differs, baseline "
                   << be.particles.size() << " vs actual " << ae.particles.size();
                report = os.str();
                return false;
            }
            for (usize pi = 0; pi < be.particles.size(); ++pi) {
                const TraceParticle& bp = be.particles[pi];
                const TraceParticle& ap = ae.particles[pi];
                const char* field = nullptr;
                if (!Near(bp.position.x, ap.position.x, tol.position) ||
                    !Near(bp.position.y, ap.position.y, tol.position) ||
                    !Near(bp.position.z, ap.position.z, tol.position))
                    field = "position";
                else if (!Near(bp.velocity.x, ap.velocity.x, tol.velocity) ||
                         !Near(bp.velocity.y, ap.velocity.y, tol.velocity) ||
                         !Near(bp.velocity.z, ap.velocity.z, tol.velocity))
                    field = "velocity";
                else if (!Near(bp.age, ap.age, tol.age))
                    field = "age";
                else if (bp.aux != ap.aux)
                    field = "aux";
                if (field) {
                    os << Where(b.frame, be) << ", particle " << pi << ": " << field
                       << " differs\n  baseline pos=(" << bp.position.x << "," << bp.position.y
                       << "," << bp.position.z << ") vel=(" << bp.velocity.x << ","
                       << bp.velocity.y << "," << bp.velocity.z << ") age=" << bp.age
                       << " aux=" << bp.aux << "\n  actual   pos=(" << ap.position.x << ","
                       << ap.position.y << "," << ap.position.z << ") vel=(" << ap.velocity.x
                       << "," << ap.velocity.y << "," << ap.velocity.z << ") age=" << ap.age
                       << " aux=" << ap.aux;
                    report = os.str();
                    return false;
                }
            }

            if (be.vertexCount != ae.vertexCount) {
                os << Where(b.frame, be) << ": vertex count differs, baseline " << be.vertexCount
                   << " vs actual " << ae.vertexCount;
                report = os.str();
                return false;
            }
            if (tol.requireVertexHash && be.vertexHash != ae.vertexHash) {
                os << Where(b.frame, be) << ": vertex stream differs (hash " << be.vertexHash
                   << " vs " << ae.vertexHash << ")";
                report = os.str();
                return false;
            }
            for (i32 c = 0; c < 4; ++c) {
                if (!Near(be.meanColor.data[c], ae.meanColor.data[c], tol.color)) {
                    os << Where(b.frame, be) << ": mean colour channel " << c << " differs, "
                       << be.meanColor.data[c] << " vs " << ae.meanColor.data[c];
                    report = os.str();
                    return false;
                }
            }
            for (i32 c = 0; c < 3; ++c) {
                if (!Near(be.boundsMin.data[c], ae.boundsMin.data[c], tol.bounds) ||
                    !Near(be.boundsMax.data[c], ae.boundsMax.data[c], tol.bounds)) {
                    os << Where(b.frame, be) << ": bounds axis " << c << " differs";
                    report = os.str();
                    return false;
                }
            }
        }
    }

    report = "identical";
    return true;
}

} // namespace whiteout::flakes::renderer::particle
