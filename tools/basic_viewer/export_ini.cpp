#include "export_ini.h"

#include "ini_file.h"
#include "settings_ini.h" // SettingsIniPath
#include "whiteout/flakes/util/path_utf8.h"

#include <algorithm>

namespace whiteout::flakes {
namespace {

using ini::IniMap;
namespace fs = std::filesystem;

constexpr const char* kClipPrefix = "Export.Clip";

// ---- small typed accessors --------------------------------------------------

void GetI32(const IniMap& m, const char* key, i32& out) {
    if (const std::string* v = m.Get(key))
        ini::ParseInt(*v, out);
}
void GetF32(const IniMap& m, const char* key, f32& out) {
    if (const std::string* v = m.Get(key))
        ini::ParseFloat(*v, out);
}
void GetBool(const IniMap& m, const char* key, bool& out) {
    if (const std::string* v = m.Get(key))
        ini::ParseBool(*v, out);
}
void GetStr(const IniMap& m, const char* key, std::string& out) {
    if (const std::string* v = m.Get(key))
        out = *v;
}
void SetBool(IniMap& m, const char* key, bool v) {
    m.Set(key, v ? "1" : "0");
}

// ---- enum tokens ------------------------------------------------------------
//
// Spelled out rather than stored as integers: an ini a user can read and
// hand-edit is half the point of persisting the recipe at all, and a
// reordered enum must not silently repoint an existing file.

struct Token {
    const char* name;
    i32 value;
};

i32 ParseToken(const std::string* v, std::span<const Token> table, i32 fallback) {
    if (!v)
        return fallback;
    const std::string want = IniMap::Trim(*v);
    for (const Token& t : table)
        if (want == t.name)
            return t.value;
    return fallback;
}

const char* TokenName(std::span<const Token> table, i32 value, const char* fallback) {
    for (const Token& t : table)
        if (t.value == value)
            return t.name;
    return fallback;
}

constexpr Token kDurationTokens[] = {{"clips", (i32)ExportDuration::Clips},
                                     {"fixed", (i32)ExportDuration::Fixed}};
constexpr Token kFillTokens[] = {{"loop_last", (i32)ExportFill::LoopLast},
                                 {"loop_queue", (i32)ExportFill::LoopQueue},
                                 {"hold_last", (i32)ExportFill::HoldLast}};
constexpr Token kCameraTokens[] = {{"viewport", (i32)ExportCameraMode::Viewport},
                                   {"preset", (i32)ExportCameraMode::Preset},
                                   {"orbit", (i32)ExportCameraMode::Orbit}};
constexpr Token kOrbitTimingTokens[] = {{"velocity", (i32)OrbitTiming::Velocity},
                                        {"revolutions", (i32)OrbitTiming::Revolutions}};
constexpr Token kSubjectTokens[] = {{"camera", (i32)OrbitSubject::Camera},
                                    {"model", (i32)OrbitSubject::Model}};

// ---- the map <-> recipe conversion -----------------------------------------

void RecipeFromIni(const IniMap& m, ExportRecipe& r, std::span<const std::string> seqNames,
                   ExportRecipeLoadReport* report) {
    if (report)
        *report = {};

    // "Did the file carry an [Export] section?" — the caller uses this to
    // decide whether to seed a first-run default instead of the struct's.
    const bool present = m.Get("Export.Fps") != nullptr || m.Get("Export.ClipCount") != nullptr;
    if (report)
        report->hadSection = present;

    ExportTiming& t = r.timing;
    GetI32(m, "Export.Fps", t.fps);
    t.duration = static_cast<ExportDuration>(
        ParseToken(m.Get("Export.DurationMode"), kDurationTokens, (i32)t.duration));
    GetI32(m, "Export.DurationMs", t.durationMs);
    t.fill = static_cast<ExportFill>(ParseToken(m.Get("Export.Fill"), kFillTokens, (i32)t.fill));
    GetI32(m, "Export.PreRollMs", t.preRollMs);
    GetI32(m, "Export.FrameStep", t.frameStep);

    ExportCameraMotion& c = r.camera;
    c.mode = static_cast<ExportCameraMode>(
        ParseToken(m.Get("Export.CameraMode"), kCameraTokens, (i32)c.mode));
    GetI32(m, "Export.CameraPreset", c.preset);
    c.subject =
        static_cast<OrbitSubject>(ParseToken(m.Get("Export.OrbitSubject"), kSubjectTokens, (i32)c.subject));
    c.timing = static_cast<OrbitTiming>(
        ParseToken(m.Get("Export.OrbitTiming"), kOrbitTimingTokens, (i32)c.timing));
    GetF32(m, "Export.OrbitDegPerSec", c.degPerSec);
    GetF32(m, "Export.OrbitRevolutions", c.revolutions);
    GetF32(m, "Export.OrbitStartYaw", c.startYawDeg);
    GetBool(m, "Export.OrbitYawRelative", c.yawRelative);
    GetBool(m, "Export.OrbitOverridePitch", c.overridePitch);
    GetF32(m, "Export.OrbitPitch", c.pitchDeg);
    GetBool(m, "Export.OrbitOverrideDistance", c.overrideDistance);
    GetF32(m, "Export.OrbitDistance", c.distance);
    GetBool(m, "Export.OrbitFit", c.fitToBounds);
    GetF32(m, "Export.OrbitFitMargin", c.fitMargin);
    GetI32(m, "Export.Angles", c.angleCount);

    ExportOutput& o = r.output;
    if (const std::string* v = m.Get("Export.Format"))
        ParseExportFormat(*v, o.format);
    GetBool(m, "Export.Transparent", o.transparent);
    GetBool(m, "Export.CaptureUi", o.captureUi);
    GetBool(m, "Export.HideOverlays", o.hideOverlays);
    GetBool(m, "Export.OverrideBackground", o.overrideBackground);
    {
        i32 rgb = (o.backgroundR << 16) | (o.backgroundG << 8) | o.backgroundB;
        GetI32(m, "Export.Background", rgb);
        o.backgroundR = static_cast<u8>((rgb >> 16) & 0xFF);
        o.backgroundG = static_cast<u8>((rgb >> 8) & 0xFF);
        o.backgroundB = static_cast<u8>(rgb & 0xFF);
    }
    GetI32(m, "Export.Width", o.width);
    GetI32(m, "Export.Height", o.height);
    GetBool(m, "Export.AutoCrop", o.autoCrop);
    GetI32(m, "Export.CropPadding", o.cropPadding);
    GetI32(m, "Export.SheetColumns", o.sheetColumns);
    GetBool(m, "Export.Sidecar", o.writeSidecar);
    GetStr(m, "Export.NameTemplate", o.nameTemplate);
    {
        std::string folder;
        GetStr(m, "Export.Folder", folder);
        if (!folder.empty())
            o.folder = io::FsPathFromUtf8(folder);
    }
    if (report)
        GetStr(m, "Export.ClipModel", report->savedModel);

    // ---- the queue ----
    i32 clipCount = -1;
    GetI32(m, "Export.ClipCount", clipCount);
    if (clipCount < 0)
        return; // no queue stored; leave whatever the caller had

    r.clips.clear();
    r.clips.reserve(static_cast<usize>(std::clamp(clipCount, 0, 4096)));
    for (i32 i = 0; i < std::clamp(clipCount, 0, 4096); ++i) {
        const std::string sec = std::string(kClipPrefix) + std::to_string(i) + ".";
        ExportClip clip;
        GetStr(m, (sec + "Sequence").c_str(), clip.savedName);
        GetI32(m, (sec + "Repeats").c_str(), clip.repeats);
        GetF32(m, (sec + "Speed").c_str(), clip.speed);
        GetI32(m, (sec + "HoldMs").c_str(), clip.holdMs);
        GetI32(m, (sec + "BlendMs").c_str(), clip.blendMs);
        GetI32(m, (sec + "TrimStartMs").c_str(), clip.trimStartMs);
        GetI32(m, (sec + "TrimEndMs").c_str(), clip.trimEndMs);
        clip.sequence = ResolveSequenceKey(seqNames, clip.savedName);
        if (clip.sequence < 0 && report)
            report->unresolvedClips.push_back(clip.savedName);
        r.clips.push_back(std::move(clip));
    }
}

void RecipeToIni(IniMap& m, const ExportRecipe& r, std::span<const std::string> seqNames,
                 const fs::path& modelPath) {
    const ExportTiming& t = r.timing;
    m.Set("Export.Fps", ini::ToString(t.fps));
    m.Set("Export.DurationMode", TokenName(kDurationTokens, (i32)t.duration, "clips"));
    m.Set("Export.DurationMs", ini::ToString(t.durationMs));
    m.Set("Export.Fill", TokenName(kFillTokens, (i32)t.fill, "loop_last"));
    m.Set("Export.PreRollMs", ini::ToString(t.preRollMs));
    m.Set("Export.FrameStep", ini::ToString(t.frameStep));

    const ExportCameraMotion& c = r.camera;
    m.Set("Export.CameraMode", TokenName(kCameraTokens, (i32)c.mode, "viewport"));
    m.Set("Export.CameraPreset", ini::ToString(c.preset));
    m.Set("Export.OrbitSubject", TokenName(kSubjectTokens, (i32)c.subject, "camera"));
    m.Set("Export.OrbitTiming", TokenName(kOrbitTimingTokens, (i32)c.timing, "velocity"));
    m.Set("Export.OrbitDegPerSec", ini::FloatToString(c.degPerSec));
    m.Set("Export.OrbitRevolutions", ini::FloatToString(c.revolutions));
    m.Set("Export.OrbitStartYaw", ini::FloatToString(c.startYawDeg));
    SetBool(m, "Export.OrbitYawRelative", c.yawRelative);
    SetBool(m, "Export.OrbitOverridePitch", c.overridePitch);
    m.Set("Export.OrbitPitch", ini::FloatToString(c.pitchDeg));
    SetBool(m, "Export.OrbitOverrideDistance", c.overrideDistance);
    m.Set("Export.OrbitDistance", ini::FloatToString(c.distance));
    SetBool(m, "Export.OrbitFit", c.fitToBounds);
    m.Set("Export.OrbitFitMargin", ini::FloatToString(c.fitMargin));
    m.Set("Export.Angles", ini::ToString(c.angleCount));

    const ExportOutput& o = r.output;
    m.Set("Export.Format", GetExportFormatInfo(o.format).iniName);
    SetBool(m, "Export.Transparent", o.transparent);
    SetBool(m, "Export.CaptureUi", o.captureUi);
    SetBool(m, "Export.HideOverlays", o.hideOverlays);
    SetBool(m, "Export.OverrideBackground", o.overrideBackground);
    m.Set("Export.Background",
          ini::ToString((static_cast<i32>(o.backgroundR) << 16) |
                        (static_cast<i32>(o.backgroundG) << 8) | static_cast<i32>(o.backgroundB)));
    m.Set("Export.Width", ini::ToString(o.width));
    m.Set("Export.Height", ini::ToString(o.height));
    SetBool(m, "Export.AutoCrop", o.autoCrop);
    m.Set("Export.CropPadding", ini::ToString(o.cropPadding));
    m.Set("Export.SheetColumns", ini::ToString(o.sheetColumns));
    SetBool(m, "Export.Sidecar", o.writeSidecar);
    m.Set("Export.NameTemplate", o.nameTemplate);
    m.Set("Export.Folder", o.folder.empty() ? std::string() : io::PathToUtf8(o.folder));

    // A variable-length list, so the old rows go first — see
    // IniMap::RemovePrefix. Note `ClipCount` and `ClipModel` share this
    // prefix, so both are written *after* the sweep, not before it.
    m.RemovePrefix(kClipPrefix);
    if (!modelPath.empty())
        m.Set("Export.ClipModel", io::PathToUtf8(modelPath));
    m.Set("Export.ClipCount", ini::ToString(static_cast<i32>(r.clips.size())));
    for (usize i = 0; i < r.clips.size(); ++i) {
        const ExportClip& clip = r.clips[i];
        const std::string sec = std::string(kClipPrefix) + std::to_string(i) + ".";
        // Prefer the live index's key; fall back to whatever the clip was
        // restored under, so an unresolved row survives a save/load cycle
        // instead of being quietly dropped on the next launch.
        std::string key = SequenceKey(seqNames, clip.sequence);
        if (key.empty())
            key = clip.savedName;
        m.Set(sec + "Sequence", key);
        m.Set(sec + "Repeats", ini::ToString(clip.repeats));
        m.Set(sec + "Speed", ini::FloatToString(clip.speed));
        if (clip.holdMs != 0)
            m.Set(sec + "HoldMs", ini::ToString(clip.holdMs));
        if (clip.blendMs != 0)
            m.Set(sec + "BlendMs", ini::ToString(clip.blendMs));
        if (clip.trimStartMs != 0)
            m.Set(sec + "TrimStartMs", ini::ToString(clip.trimStartMs));
        if (clip.trimEndMs != 0)
            m.Set(sec + "TrimEndMs", ini::ToString(clip.trimEndMs));
    }
}

} // namespace

void LoadExportRecipe(ExportRecipe& recipe, std::span<const std::string> sequenceNames,
                      ExportRecipeLoadReport* report) {
    IniMap m;
    m.Load(SettingsIniPath());
    RecipeFromIni(m, recipe, sequenceNames, report);
}

void SaveExportRecipe(const ExportRecipe& recipe, std::span<const std::string> sequenceNames,
                      const fs::path& modelPath) {
    const fs::path path = SettingsIniPath();
    IniMap m;
    m.Load(path); // round-trip: every unrelated key survives
    RecipeToIni(m, recipe, sequenceNames, modelPath);
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    m.Save(path);
}

bool WriteExportRecipeFile(const fs::path& file, const ExportRecipe& recipe,
                           std::span<const std::string> sequenceNames, const fs::path& modelPath) {
    IniMap m;
    RecipeToIni(m, recipe, sequenceNames, modelPath);
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    m.Save(file);
    return fs::exists(file, ec);
}

bool ReadExportRecipeFile(const fs::path& file, ExportRecipe& recipe,
                          std::span<const std::string> sequenceNames,
                          ExportRecipeLoadReport* report) {
    std::error_code ec;
    if (!fs::exists(file, ec))
        return false;
    IniMap m;
    m.Load(file);
    RecipeFromIni(m, recipe, sequenceNames, report);
    return true;
}

} // namespace whiteout::flakes
