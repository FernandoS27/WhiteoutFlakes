#include "io/event_data.h"
#include "common_types.h"
#include "io/content_provider.h"
#include "slk.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>

namespace whiteout::flakes::io {

namespace {

void LogHeaders(const char*  , const SlkTable&  ) {

}

i32 FindAny(const SlkTable& t, std::initializer_list<const char*> names) {
    for (auto* n : names) { i32 c = t.FindColumn(n); if (c >= 0) return c; }
    return -1;
}

std::string ToLower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = (char)std::tolower((unsigned char)c);
    return out;
}

f32 ParseFloat(std::string_view sv) {
    if (sv.empty()) return 0.0f;
    return std::strtof(std::string(sv).c_str(), nullptr);
}

i32 ParseInt(std::string_view sv) {
    if (sv.empty()) return 0;
    return (i32)std::strtol(std::string(sv).c_str(), nullptr, 10);
}

std::vector<std::string> SplitComma(std::string_view sv) {
    std::vector<std::string> out;
    usize start = 0;
    for (usize i = 0; i <= sv.size(); ++i) {
        if (i == sv.size() || sv[i] == ',') {
            if (i > start) {
                std::string_view part = sv.substr(start, i - start);
                while (!part.empty() && (part.front() == ' ' || part.front() == '\t')) part.remove_prefix(1);
                while (!part.empty() && (part.back()  == ' ' || part.back()  == '\t')) part.remove_suffix(1);
                if (!part.empty()) out.emplace_back(part);
            }
            start = i + 1;
        }
    }
    return out;
}

struct DataCache {
    std::mutex mu;
    bool       loaded = false;
    std::unordered_map<std::string, SpnEntry> spn;
    std::unordered_map<std::string, SplEntry> spl;
    std::unordered_map<std::string, UbrEntry> ubr;
    std::unordered_map<std::string, SndEntry> snd;

    std::unordered_map<std::string, std::string> assetMap;
};
DataCache& Cache() { static DataCache c; return c; }

void RewriteMdlToMdx(std::string& path) {
    if (path.size() >= 4) {
        auto tail = path.substr(path.size() - 4);
        for (auto& c : tail) c = (char)std::tolower((unsigned char)c);
        if (tail == ".mdl") {
            path[path.size() - 1] = 'x';
        }
    }
}

void LoadSpawnData(IContentProvider& cp, DataCache& cache) {
    auto data = cp.ReadFile("Splats\\SpawnData.slk", nullptr);
    if (!data) { std::fprintf(stderr, "[events] ERR: SpawnData.slk: not found\n"); return; }
    SlkTable t = ParseSlk(*data);
    LogHeaders("SpawnData.slk", t);

    const i32 colId    = 0;
    const i32 colModel = FindAny(t, {"Model", "model"});
    if (colModel < 0) {
        std::fprintf(stderr, "[events] ERR: SpawnData.slk missing Model col\n");
        return;
    }
    for (usize r = 1; r < t.RowCount(); ++r) {
        std::string_view id    = t.Cell(r, colId);
        std::string_view model = t.Cell(r, colModel);
        if (id.empty() || model.empty()) continue;
        SpnEntry e;
        e.modelPath.assign(model);
        RewriteMdlToMdx(e.modelPath);
        cache.spn.emplace(ToLower(id), std::move(e));
    }
}

void LoadSplatData(IContentProvider& cp, DataCache& cache) {
    auto data = cp.ReadFile("Splats\\SplatData.slk", nullptr);
    if (!data) { std::fprintf(stderr, "[events] ERR: SplatData.slk: not found\n"); return; }
    SlkTable t = ParseSlk(*data);
    LogHeaders("SplatData.slk", t);

    const i32 cId   = 0;
    const i32 cDir  = FindAny(t, {"Dir", "Texture"});
    const i32 cFile = FindAny(t, {"file", "File", "Tex"});
    const i32 cScale = FindAny(t, {"Scale"});
    const i32 cSR = FindAny(t, {"StartR"}), cSG = FindAny(t, {"StartG"}),
              cSB = FindAny(t, {"StartB"}), cSA = FindAny(t, {"StartA"});
    const i32 cMR = FindAny(t, {"MiddleR"}), cMG = FindAny(t, {"MiddleG"}),
              cMB = FindAny(t, {"MiddleB"}), cMA = FindAny(t, {"MiddleA"});
    const i32 cER = FindAny(t, {"EndR"}), cEG = FindAny(t, {"EndG"}),
              cEB = FindAny(t, {"EndB"}), cEA = FindAny(t, {"EndA"});
    const i32 cCols = FindAny(t, {"Columns"}), cRows = FindAny(t, {"Rows"});
    const i32 cLife = FindAny(t, {"Lifespan"}), cDecay = FindAny(t, {"Decay"});
    const i32 cUvLs = FindAny(t, {"UVLifespanStart"}), cUvLe = FindAny(t, {"UVLifespanEnd"});
    const i32 cLR   = FindAny(t, {"LifespanRepeat"});
    const i32 cUvDs = FindAny(t, {"UVDecayStart"}),    cUvDe = FindAny(t, {"UVDecayEnd"});
    const i32 cDR   = FindAny(t, {"DecayRepeat"});
    const i32 cBl   = FindAny(t, {"BlendMode"});

    for (usize r = 1; r < t.RowCount(); ++r) {
        std::string_view id = t.Cell(r, cId);
        if (id.empty()) continue;
        SplEntry e;

        if (cFile >= 0) {
            std::string fn(t.Cell(r, cFile));
            if (cDir >= 0 && !t.Cell(r, cDir).empty()) {
                std::string dir(t.Cell(r, cDir));
                if (dir.back() != '\\' && dir.back() != '/') dir += '\\';
                e.file = dir + fn + ".blp";
            } else {
                e.file = "ReplaceableTextures\\Splats\\" + fn + ".blp";
            }
        }
        e.scale          = (cScale >= 0) ? ParseFloat(t.Cell(r, cScale)) : 1.0f;
        if (cSR >= 0) { e.startC[0]=ParseFloat(t.Cell(r,cSR))/255.f; e.startC[1]=ParseFloat(t.Cell(r,cSG))/255.f; e.startC[2]=ParseFloat(t.Cell(r,cSB))/255.f; e.startC[3]=ParseFloat(t.Cell(r,cSA))/255.f; }
        if (cMR >= 0) { e.midC[0]  =ParseFloat(t.Cell(r,cMR))/255.f; e.midC[1]  =ParseFloat(t.Cell(r,cMG))/255.f; e.midC[2]  =ParseFloat(t.Cell(r,cMB))/255.f; e.midC[3]  =ParseFloat(t.Cell(r,cMA))/255.f; }
        if (cER >= 0) { e.endC[0]  =ParseFloat(t.Cell(r,cER))/255.f; e.endC[1]  =ParseFloat(t.Cell(r,cEG))/255.f; e.endC[2]  =ParseFloat(t.Cell(r,cEB))/255.f; e.endC[3]  =ParseFloat(t.Cell(r,cEA))/255.f; }
        e.columns        = (cCols >= 0) ? std::max(1, ParseInt(t.Cell(r, cCols))) : 1;
        e.rows           = (cRows >= 0) ? std::max(1, ParseInt(t.Cell(r, cRows))) : 1;
        e.lifespan       = (cLife >= 0) ? ParseFloat(t.Cell(r, cLife)) : 0.0f;
        e.decay          = (cDecay >= 0) ? ParseFloat(t.Cell(r, cDecay)) : 0.0f;
        e.uvLifeStart    = (cUvLs >= 0) ? ParseInt(t.Cell(r, cUvLs)) : 0;
        e.uvLifeEnd      = (cUvLe >= 0) ? ParseInt(t.Cell(r, cUvLe)) : 0;
        e.lifespanRepeat = (cLR  >= 0) ? std::max(1, ParseInt(t.Cell(r, cLR))) : 1;
        e.uvDecayStart   = (cUvDs >= 0) ? ParseInt(t.Cell(r, cUvDs)) : 0;
        e.uvDecayEnd     = (cUvDe >= 0) ? ParseInt(t.Cell(r, cUvDe)) : 0;
        e.decayRepeat    = (cDR  >= 0) ? std::max(1, ParseInt(t.Cell(r, cDR))) : 1;
        e.blendMode      = (cBl  >= 0) ? ParseInt(t.Cell(r, cBl)) : 0;
        cache.spl.emplace(ToLower(id), std::move(e));
    }
}

void LoadUberSplatData(IContentProvider& cp, DataCache& cache) {
    auto data = cp.ReadFile("Splats\\UberSplatData.slk", nullptr);
    if (!data) { std::fprintf(stderr, "[events] ERR: UberSplatData.slk: not found\n"); return; }
    SlkTable t = ParseSlk(*data);
    LogHeaders("UberSplatData.slk", t);

    const i32 cId    = 0;
    const i32 cDir   = FindAny(t, {"Dir"});
    const i32 cFile  = FindAny(t, {"file", "File"});
    const i32 cScale = FindAny(t, {"Scale"});
    const i32 cSR = FindAny(t, {"StartR"}), cSG = FindAny(t, {"StartG"}),
              cSB = FindAny(t, {"StartB"}), cSA = FindAny(t, {"StartA"});
    const i32 cMR = FindAny(t, {"MiddleR"}), cMG = FindAny(t, {"MiddleG"}),
              cMB = FindAny(t, {"MiddleB"}), cMA = FindAny(t, {"MiddleA"});
    const i32 cER = FindAny(t, {"EndR"}), cEG = FindAny(t, {"EndG"}),
              cEB = FindAny(t, {"EndB"}), cEA = FindAny(t, {"EndA"});
    const i32 cBirth = FindAny(t, {"BirthTime"});
    const i32 cPause = FindAny(t, {"PauseTime"});
    const i32 cDecay = FindAny(t, {"Decay"});
    const i32 cBl    = FindAny(t, {"BlendMode"});
    for (usize r = 1; r < t.RowCount(); ++r) {
        std::string_view id = t.Cell(r, cId);
        if (id.empty()) continue;
        UbrEntry e;
        if (cFile >= 0) {
            std::string fn(t.Cell(r, cFile));
            if (cDir >= 0 && !t.Cell(r, cDir).empty()) {
                std::string dir(t.Cell(r, cDir));
                if (dir.back() != '\\' && dir.back() != '/') dir += '\\';
                e.file = dir + fn + ".blp";
            } else {
                e.file = "ReplaceableTextures\\Splats\\" + fn + ".blp";
            }
        }
        e.scale = (cScale >= 0) ? ParseFloat(t.Cell(r, cScale)) : 1.0f;
        if (cSR >= 0) { e.c[0][0]=ParseFloat(t.Cell(r,cSR))/255.f; e.c[0][1]=ParseFloat(t.Cell(r,cSG))/255.f; e.c[0][2]=ParseFloat(t.Cell(r,cSB))/255.f; e.c[0][3]=ParseFloat(t.Cell(r,cSA))/255.f; }
        if (cMR >= 0) { e.c[1][0]=ParseFloat(t.Cell(r,cMR))/255.f; e.c[1][1]=ParseFloat(t.Cell(r,cMG))/255.f; e.c[1][2]=ParseFloat(t.Cell(r,cMB))/255.f; e.c[1][3]=ParseFloat(t.Cell(r,cMA))/255.f; }
        if (cER >= 0) { e.c[2][0]=ParseFloat(t.Cell(r,cER))/255.f; e.c[2][1]=ParseFloat(t.Cell(r,cEG))/255.f; e.c[2][2]=ParseFloat(t.Cell(r,cEB))/255.f; e.c[2][3]=ParseFloat(t.Cell(r,cEA))/255.f; }
        e.birthTime = (cBirth >= 0) ? ParseFloat(t.Cell(r, cBirth)) : 0.0f;
        e.pauseTime = (cPause >= 0) ? ParseFloat(t.Cell(r, cPause)) : 0.0f;
        e.decay     = (cDecay >= 0) ? ParseFloat(t.Cell(r, cDecay)) : 0.0f;
        e.blendMode = (cBl    >= 0) ? ParseInt(t.Cell(r, cBl)) : 0;
        cache.ubr.emplace(ToLower(id), std::move(e));
    }
}

void LoadAssetSlk(IContentProvider& cp, DataCache& cache, const char* path) {
    auto bytes = cp.ReadFile(path, nullptr);
    if (!bytes) {
        std::fprintf(stderr, "[events] WARN: %s: not found\n", path);
        return;
    }
    SlkTable t = ParseSlk(*bytes);
    LogHeaders(path, t);

    const i32 cLabel = 0;
    const i32 cPath  = 1;
    for (usize r = 1; r < t.RowCount(); ++r) {
        std::string_view label = t.Cell(r, cLabel);
        std::string_view fp    = t.Cell(r, cPath);
        if (label.empty() || fp.empty() || fp == "_") continue;

        cache.assetMap.emplace(ToLower(label), std::string(fp));
    }
}

void LoadSoundSlk(IContentProvider& cp, DataCache& cache, const char* path) {
    auto sounds = cp.ReadFile(path, nullptr);
    if (!sounds) {
        std::fprintf(stderr, "[events] WARN: %s: not found\n", path);
        return;
    }
    SlkTable st = ParseSlk(*sounds);
    LogHeaders(path, st);

    const i32 sCode = FindAny(st, {"AnimationEventCode", "EventCode", "Code"});
    if (sCode < 0) return;
    const i32 sFiles = FindAny(st, {"FileNames", "Files", "filenames"});
    const i32 sVol   = FindAny(st, {"Volume"});
    const i32 sMin   = FindAny(st, {"MinDistance"});
    const i32 sMax   = FindAny(st, {"MaxDistance"});
    const i32 sCut   = FindAny(st, {"DistanceCutoff"});

    for (usize r = 1; r < st.RowCount(); ++r) {
        std::string_view code = st.Cell(r, sCode);
        if (code.empty() || code == "_") continue;
        SndEntry e;
        if (sFiles >= 0) {
            for (auto& tok : SplitComma(st.Cell(r, sFiles))) {
                auto it = cache.assetMap.find(ToLower(tok));
                e.filePaths.push_back(it != cache.assetMap.end() ? it->second
                                                                 : std::move(tok));
            }
        }
        e.volume         = (sVol >= 0) ? ParseFloat(st.Cell(r, sVol)) : 1.0f;
        e.minDistance    = (sMin >= 0) ? ParseFloat(st.Cell(r, sMin)) : 0.0f;
        e.maxDistance    = (sMax >= 0) ? ParseFloat(st.Cell(r, sMax)) : 0.0f;
        e.distanceCutoff = (sCut >= 0) ? ParseFloat(st.Cell(r, sCut)) : 0.0f;

        cache.snd.emplace(ToLower(code), std::move(e));
    }
}

void LoadAllSoundSlks(IContentProvider& cp, DataCache& cache) {
    static constexpr const char* kAssetSlks[] = {
        "UI\\SoundInfo\\DialogueCreepsBase.slk",
        "UI\\SoundInfo\\DialogueDemonBase.slk",
        "UI\\SoundInfo\\DialogueHumanBase.slk",
        "UI\\SoundInfo\\DialogueNagaBase.slk",
        "UI\\SoundInfo\\DialogueNightElfBase.slk",
        "UI\\SoundInfo\\DialogueOrcBase.slk",
        "UI\\SoundInfo\\DialogueUndeadBase.slk",
        "UI\\SoundInfo\\SoundAssetCombat.slk",
    };
    for (const char* p : kAssetSlks) LoadAssetSlk(cp, cache, p);

    static constexpr const char* kSoundSlks[] = {
        "UI\\SoundInfo\\UnitAckSounds.slk",
        "UI\\SoundInfo\\UnitCombatSounds.slk",
        "UI\\SoundInfo\\UISounds.slk",
        "UI\\SoundInfo\\AmbienceSounds.slk",
        "UI\\SoundInfo\\AnimSounds.slk",
        "UI\\SoundInfo\\AbilitySounds.slk",
        "UI\\SoundInfo\\DialogSounds.slk",
        "UI\\SoundInfo\\AmbientMusic.slk",
        "UI\\SoundInfo\\Music.slk",
    };
    for (const char* p : kSoundSlks) LoadSoundSlk(cp, cache, p);
}

template <class Map>
const typename Map::mapped_type* FindIn(const Map& m, std::string_view id) {
    if (id.empty()) return nullptr;
    auto key = ToLower(id);
    auto it = m.find(key);
    return (it == m.end()) ? nullptr : &it->second;
}

}

void LoadEventDataFiles(IContentProvider* cp, bool force) {
    if (!cp) return;
    auto& c = Cache();
    std::lock_guard<std::mutex> lk(c.mu);
    if (c.loaded && !force) return;
    c.spn.clear(); c.spl.clear(); c.ubr.clear(); c.snd.clear();
    c.assetMap.clear();
    LoadSpawnData    (*cp, c);
    LoadSplatData    (*cp, c);
    LoadUberSplatData(*cp, c);
    LoadAllSoundSlks (*cp, c);
    c.loaded = true;
}

const SpnEntry* FindSpn(std::string_view id) {
    auto& c = Cache();
    std::lock_guard<std::mutex> lk(c.mu);
    return FindIn(c.spn, id);
}
const SplEntry* FindSpl(std::string_view id) {
    auto& c = Cache();
    std::lock_guard<std::mutex> lk(c.mu);
    return FindIn(c.spl, id);
}
const UbrEntry* FindUbr(std::string_view id) {
    auto& c = Cache();
    std::lock_guard<std::mutex> lk(c.mu);
    return FindIn(c.ubr, id);
}
const SndEntry* FindSnd(std::string_view id) {
    auto& c = Cache();
    std::lock_guard<std::mutex> lk(c.mu);
    return FindIn(c.snd, id);
}

}
