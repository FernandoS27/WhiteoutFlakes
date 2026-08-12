#include "renderer/debug/draw_trace.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace whiteout::flakes::renderer::debug {

namespace detail {
bool g_drawTraceOn = false;
}

namespace {

// v1 is the first schema. The reader below already tolerates a short line, so
// a field appended at the end does not force a re-baseline; bump the magic
// only when an existing column changes meaning.
constexpr const char* kMagic = "wdt1";

// %.9g round-trips a float exactly, which is what makes a recorded baseline an
// equality test rather than an approximate one.
std::string F(f32 v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.9g", v);
    return buf;
}

// Zero tolerance means bit-identical. Otherwise the tolerance is relative to
// the magnitude involved — sqDist runs into the millions, where an absolute
// epsilon would be meaningless.
bool Near(f32 a, f32 b, f32 eps) {
    if (eps <= 0.0f)
        return a == b;
    const f32 scale = (std::max)(1.0f, (std::max)(std::fabs(a), std::fabs(b)));
    return std::fabs(a - b) <= eps * scale;
}

} // namespace

u64 TraceHashBytes(const void* data, usize size, u64 seed) {
    const auto* p = static_cast<const u8*>(data);
    u64 h = seed;
    for (usize i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 0x100000001B3ull;
    }
    return h;
}

u32 TracePsoKey(const TracePsoInputs& in) {
    const u32 packed[] = {
        in.vsPermute,
        in.psPermute,
        in.matAlpha,
        in.disables,
        in.vertexLayout,
        in.extraRtvCount,
        static_cast<u32>(in.extraColorWrite) | (static_cast<u32>(in.wireframe) << 1) |
            (static_cast<u32>(in.lhClipSpace) << 2),
    };
    u32 h = 0x811C9DC5u;
    const auto* p = reinterpret_cast<const u8*>(packed);
    for (usize i = 0; i < sizeof(packed); ++i) {
        h ^= p[i];
        h *= 0x01000193u;
    }
    return h;
}

// ---------------------------------------------------------------------------
// Recorder
// ---------------------------------------------------------------------------

DrawTraceRecorder& DrawTraceRecorder::Instance() {
    static DrawTraceRecorder inst;
    return inst;
}

void DrawTraceRecorder::Begin() {
    detail::g_drawTraceOn = true;
}

void DrawTraceRecorder::End() {
    detail::g_drawTraceOn = false;
}

void DrawTraceRecorder::Clear() {
    trace_.frames.clear();
    ctx_ = {};
}

void DrawTraceRecorder::BeginFrame(i32 frame) {
    TraceFrame tf;
    tf.frame = frame;
    trace_.frames.push_back(std::move(tf));
    ctx_ = {};
}

void DrawTraceRecorder::Record(const TraceDraw& d) {
    if (trace_.frames.empty())
        BeginFrame(0);
    trace_.frames.back().draws.push_back(d);
}

// ---------------------------------------------------------------------------
// Serialisation
// ---------------------------------------------------------------------------

bool WriteTrace(const DrawTrace& t, const std::string& path, std::string& err) {
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        err = "cannot open for write: " + path;
        return false;
    }
    f << kMagic << "\n";
    for (const auto& fr : t.frames) {
        f << "f " << fr.frame << " " << fr.draws.size() << "\n";
        for (const auto& d : fr.draws) {
            f << "d " << (i32)d.passSlot << " " << (i32)d.producer << " " << (i32)d.shadingModel
              << " " << (i32)d.blendClass << " " << (i32)d.depthFill << " " << d.actor.rootActor
              << " " << (i32)d.actor.role << " " << (i32)d.actor.treeDepth << " "
              << d.actor.emitterId << " " << d.actor.slotIndex << " " << d.submesh << " "
              << d.surface << " " << d.layer << " " << d.lod << " " << d.priorityPlane << " "
              << d.sortOrder << " " << F(d.sqDist) << " " << (i32)d.underWater << " "
              << d.filterMode << " " << d.matFlags;
            for (i32 s = 0; s < kTraceTexSlots; ++s)
                f << " " << d.texIds[s];
            f << " " << d.texAnimId << " " << d.indexCount << " " << d.vertexCount << " "
              << (i32)d.streamMask << " " << (i32)d.palettePath << " " << d.paletteSlots << " "
              << (i32)d.lightCount << " " << d.lightPaletteHash << " " << d.psoKey << " "
              << d.cbHash << " " << F(d.combinedAlpha) << " " << d.texMtxHash << "\n";
        }
    }
    return true;
}

bool ReadTrace(DrawTrace& t, const std::string& path, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        err = "cannot open for read: " + path;
        return false;
    }
    std::string magic;
    std::getline(f, magic);
    // Accept any wdt* revision: a baseline recorded before a column existed is
    // exactly what the phase that introduced it needs to be checked against.
    // A short line leaves the trailing fields at their struct defaults.
    if (magic.rfind("wdt", 0) != 0) {
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
            fr.draws.reserve(n);
            t.frames.push_back(std::move(fr));
        } else if (tag == 'd') {
            if (t.frames.empty()) {
                err = "draw record before any frame";
                return false;
            }
            TraceDraw d;
            i32 passSlot = 0, producer = 0, shading = 0, blend = 0, depthFill = 0;
            i32 role = 0, depth = 0, underWater = 0, streamMask = 0, palettePath = 0,
                lightCount = 0;
            is >> passSlot >> producer >> shading >> blend >> depthFill >> d.actor.rootActor >>
                role >> depth >> d.actor.emitterId >> d.actor.slotIndex >> d.submesh >> d.surface >>
                d.layer >> d.lod >> d.priorityPlane >> d.sortOrder >> d.sqDist >> underWater >>
                d.filterMode >> d.matFlags;
            for (i32 s = 0; s < kTraceTexSlots; ++s)
                is >> d.texIds[s];
            is >> d.texAnimId >> d.indexCount >> d.vertexCount >> streamMask >> palettePath >>
                d.paletteSlots >> lightCount >> d.lightPaletteHash >> d.psoKey >> d.cbHash >>
                d.combinedAlpha >> d.texMtxHash;
            d.passSlot = (u8)passSlot;
            d.producer = (u8)producer;
            d.shadingModel = (u8)shading;
            d.blendClass = (u8)blend;
            d.depthFill = (u8)depthFill;
            d.actor.role = (u8)role;
            d.actor.treeDepth = (u8)depth;
            d.underWater = (u8)underWater;
            d.streamMask = (u8)streamMask;
            d.palettePath = (u8)palettePath;
            d.lightCount = (u8)lightCount;
            t.frames.back().draws.push_back(d);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Comparison
// ---------------------------------------------------------------------------

const char* FirstDrawDiff(const TraceDraw& a, const TraceDraw& b, const CompareTolerance& tol) {
    // Ordered coarse-to-fine so the reported field is the most explanatory one:
    // a reclassified geoset should say "passSlot", not "psoKey".
    if (a.passSlot != b.passSlot)
        return "passSlot";
    if (a.producer != b.producer)
        return "producer";
    if (a.shadingModel != b.shadingModel)
        return "shadingModel";
    if (a.blendClass != b.blendClass)
        return "blendClass";
    if (a.depthFill != b.depthFill)
        return "depthFill";

    if (a.actor.rootActor != b.actor.rootActor)
        return "actor.rootActor";
    if (a.actor.role != b.actor.role)
        return "actor.role";
    if (a.actor.treeDepth != b.actor.treeDepth)
        return "actor.treeDepth";
    if (a.actor.emitterId != b.actor.emitterId)
        return "actor.emitterId";
    if (a.actor.slotIndex != b.actor.slotIndex)
        return "actor.slotIndex";

    if (a.submesh != b.submesh)
        return "submesh";
    if (a.surface != b.surface)
        return "surface";
    if (a.layer != b.layer)
        return "layer";
    if (a.lod != b.lod)
        return "lod";

    if (a.priorityPlane != b.priorityPlane)
        return "priorityPlane";
    if (a.sortOrder != b.sortOrder)
        return "sortOrder";
    if (!Near(a.sqDist, b.sqDist, tol.distance))
        return "sqDist";
    if (a.underWater != b.underWater)
        return "underWater";

    if (a.filterMode != b.filterMode)
        return "filterMode";
    if (a.matFlags != b.matFlags)
        return "matFlags";
    for (i32 s = 0; s < kTraceTexSlots; ++s) {
        if (a.texIds[s] != b.texIds[s])
            return "texIds";
    }
    if (a.texAnimId != b.texAnimId)
        return "texAnimId";

    if (a.indexCount != b.indexCount)
        return "indexCount";
    if (a.vertexCount != b.vertexCount)
        return "vertexCount";
    if (a.streamMask != b.streamMask)
        return "streamMask";
    if (a.palettePath != b.palettePath)
        return "palettePath";
    if (a.paletteSlots != b.paletteSlots)
        return "paletteSlots";

    if (a.lightCount != b.lightCount)
        return "lightCount";
    if (a.lightPaletteHash != b.lightPaletteHash)
        return "lightPaletteHash";

    if (a.psoKey != b.psoKey)
        return "psoKey";
    if (tol.requireCbHash && a.cbHash != b.cbHash)
        return "cbHash";
    if (!Near(a.combinedAlpha, b.combinedAlpha, tol.alpha))
        return "combinedAlpha";
    if (tol.requireCbHash && a.texMtxHash != b.texMtxHash)
        return "texMtxHash";

    return nullptr;
}

namespace {

// Enough of a draw's identity to make a divergence report actionable without
// dumping all thirty-odd columns.
std::string Describe(const TraceDraw& d) {
    std::ostringstream os;
    os << "pass=" << (i32)d.passSlot << " producer=" << (i32)d.producer
       << " model=" << (i32)d.shadingModel << " root=" << d.actor.rootActor
       << " role=" << (i32)d.actor.role << " depth=" << (i32)d.actor.treeDepth
       << " emitter=" << d.actor.emitterId << " slot=" << d.actor.slotIndex
       << " submesh=" << d.submesh << " surface=" << d.surface << " layer=" << d.layer;
    return os.str();
}

} // namespace

bool CompareTraces(const DrawTrace& baseline, const DrawTrace& actual, const CompareTolerance& tol,
                   std::string& report) {
    // A tolerance and an exact hash are contradictory demands: any perturbation
    // large enough to need the tolerance also changes the constant-buffer bytes.
    // Mirrors CompareTolerance::requireVertexHash in particle_trace.
    if (tol.requireCbHash && (tol.distance > 0.0f || tol.alpha > 0.0f)) {
        report = "requireCbHash cannot be combined with a nonzero float tolerance";
        return false;
    }

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
        if (b.frame != a.frame) {
            os << "frame index differs at position " << fi << ": baseline " << b.frame
               << " vs actual " << a.frame;
            report = os.str();
            return false;
        }
        if (b.draws.size() != a.draws.size()) {
            os << "frame " << b.frame << ": draw count differs, baseline " << b.draws.size()
               << " vs actual " << a.draws.size();
            report = os.str();
            return false;
        }
        for (usize di = 0; di < b.draws.size(); ++di) {
            const char* field = FirstDrawDiff(b.draws[di], a.draws[di], tol);
            if (field) {
                os << "frame " << b.frame << ", draw " << di << ": " << field << " differs\n"
                   << "  baseline " << Describe(b.draws[di]) << "\n"
                   << "  actual   " << Describe(a.draws[di]);
                report = os.str();
                return false;
            }
        }
    }

    report = "identical";
    return true;
}

} // namespace whiteout::flakes::renderer::debug
