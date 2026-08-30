#pragma once

// ============================================================================
// The animation-export recipe and the schedule built from it.
//
// A recipe is what the user configured: a clip queue, a timing policy, a
// camera motion, an output spec. A schedule is the recipe resolved against a
// model's sequence table into a pure function of the frame index — `At(i)`
// answers everything about frame `i` and advances nothing.
//
// Four consumers share the schedule: the capture loop, the live preview, the
// dialog's timeline strip and the tests. It holds no cursor precisely so those
// cannot drift apart.
//
// Deliberately device-free: nothing here includes a renderer header, so this
// TU compiles into a G0 test the same way settings_io.cpp does.
// ============================================================================

#include "whiteout/flakes/display.h" // SequenceInfo
#include "whiteout/flakes/types.h"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace whiteout::flakes {

// ---------------------------------------------------------------------------
// Output format
// ---------------------------------------------------------------------------

// Output format for an animation export. The enum order is the canonical
// order used by the UI format dropdown and by GetExportFormatInfo(). The
// first four values and their meaning are unchanged from the original
// exporter — an existing ini or CLI invocation keeps its format.
enum class ExportFormat {
    PngFrames, ///< One <model>_<anim>_<id>.png per frame.
    Gif,       ///< A single animated <model>_<anim>.gif.
    Apng,      ///< A single animated <model>_<anim>.apng (APNG).
    Webp,      ///< A single animated <model>_<anim>.webp.
    PngSheet,  ///< A single <model>_<anim>.png sprite sheet.
    Mp4,       ///< H.264 / yuv420p via ffmpeg.
    WebmVp9,   ///< VP9 via ffmpeg; keeps alpha.
};
constexpr i32 kExportFormatCount = 7;

// How the frames of a recording become files. Derived from the format rather
// than configured beside it: the two axes have no independent combinations,
// and a separate enum only creates states the UI has to forbid.
enum class ExportLayout {
    Frames,   ///< One file per frame.
    Animated, ///< One animated container.
    Sheet,    ///< One image, frames tiled into a grid.
    Video,    ///< One video file, written by an external encoder.
};

// Static description of an ExportFormat — its human label, its output file
// extension and its layout. PngFrames has an empty extension since it writes a
// numbered PNG per frame instead.
struct ExportFormatInfo {
    const char* label;     ///< e.g. "Animated WebP".
    const char* extension; ///< Single-file extension incl. dot; "" = per-frame PNGs.
    ExportLayout layout;
    const char* iniName; ///< Stable token for the ini / CLI ("webp", "mp4", …).
};

// Look up the description of a format. Indexed by ExportFormat.
const ExportFormatInfo& GetExportFormatInfo(ExportFormat format);

// True for the animated single-file formats (GIF/APNG/WebP); false for
// PngFrames. Kept with its original meaning — sheets and videos are single
// files too, but they are not *animated containers*, which is what every
// existing caller of this was actually asking.
inline bool IsSingleFileFormat(ExportFormat format) {
    return GetExportFormatInfo(format).layout == ExportLayout::Animated;
}

inline ExportLayout LayoutOf(ExportFormat format) {
    return GetExportFormatInfo(format).layout;
}

// Parse a format from its ini token. Returns false (and leaves `out` alone)
// for anything unrecognised, so a newer ini degrades to the default rather
// than to format 0.
bool ParseExportFormat(std::string_view token, ExportFormat& out);

// ---------------------------------------------------------------------------
// The recipe
// ---------------------------------------------------------------------------

/// @brief One entry in the clip queue.
struct ExportClip {
    i32 sequence = 0;    ///< index into the actor's sequence table; <0 = unresolved
    i32 repeats = 1;     ///< whole plays; each repeat restarts the clip
    f32 speed = 1.0f;    ///< animation-clock rate (§4: scales the actor clock, not the world)
    i32 holdMs = 0;      ///< freeze the final pose this long after the clip
    i32 blendMs = 0;     ///< cross-fade in from the previous clip; 0 = hard cut
    i32 trimStartMs = 0; ///< sequence-local trim; 0 = from the start
    i32 trimEndMs = 0;   ///< 0 = to the sequence end

    /// @brief The `Name#occurrence` key this clip was restored from.
    ///
    /// Not in the original design, and needed by two things that are: a queue
    /// restored from the ini against a model that does not have the sequence
    /// has to stay in the list as an unresolved row with a reason (§7), and
    /// that row has to say *which* animation it wanted. Empty for a clip the
    /// user picked live.
    std::string savedName;

    bool Resolved() const {
        return sequence >= 0;
    }
};

enum class ExportDuration { Clips, Fixed };
enum class ExportFill { LoopLast, LoopQueue, HoldLast };

struct ExportTiming {
    i32 fps = 30;
    ExportDuration duration = ExportDuration::Clips;
    i32 durationMs = 5000; ///< Fixed only
    ExportFill fill = ExportFill::LoopLast;
    i32 preRollMs = 0; ///< simulate before frame 0, capturing nothing
    i32 frameStep = 1; ///< capture every Nth frame
};

enum class ExportCameraMode { Viewport, Preset, Orbit };
enum class OrbitTiming { Velocity, Revolutions };
enum class OrbitSubject { Camera, Model };

struct ExportCameraMotion {
    ExportCameraMode mode = ExportCameraMode::Viewport;
    i32 preset = -1; ///< Preset mode: index into CameraPresets()

    // ---- Orbit ----
    OrbitSubject subject = OrbitSubject::Camera;
    OrbitTiming timing = OrbitTiming::Velocity;
    f32 degPerSec = 30.0f;   ///< Velocity; signed, negative = clockwise
    f32 revolutions = 1.0f;  ///< Revolutions; degPerSec derived from the total
    f32 startYawDeg = 0.0f;  ///< offset from the viewport yaw when `yawRelative`
    bool yawRelative = true; ///< start yaw is an offset from the viewport camera's
    bool overridePitch = false;
    f32 pitchDeg = 20.0f;
    bool overrideDistance = false;
    f32 distance = 0.0f;
    bool fitToBounds = false;
    f32 fitMargin = 1.15f;

    i32 angleCount = 1; ///< >1 = multi-angle sprite pass
};

struct ExportOutput {
    ExportFormat format = ExportFormat::PngFrames;
    bool transparent = false;
    bool captureUi = false;
    bool hideOverlays = true; ///< grid / colliders / lights / events off for the capture
    bool overrideBackground = false;
    u8 backgroundR = 0, backgroundG = 0, backgroundB = 0;
    i32 width = 0, height = 0; ///< 0x0 = current view
    bool autoCrop = false;     ///< union-of-frames crop
    i32 cropPadding = 2;
    i32 sheetColumns = 0;     ///< Sheet: 0 = auto (near-square)
    bool writeSidecar = true; ///< <name>.json describing the recording
    std::string nameTemplate; ///< "" = the per-layout default
    std::filesystem::path folder;
};

struct ExportRecipe {
    std::vector<ExportClip> clips;
    ExportTiming timing;
    ExportCameraMotion camera;
    ExportOutput output;
};

// ---------------------------------------------------------------------------
// The schedule
// ---------------------------------------------------------------------------

/// @brief Everything the runner needs to know about one frame.
struct ExportStep {
    i32 frameIndex = 0;
    i32 clipIndex = -1;      ///< index into recipe.clips; -1 for a pure hold segment
    i32 sequence = 0;        ///< raw sequence index to request
    bool passStart = false;  ///< first frame of a recording pass (frame 0, or a new angle)
    bool clipStart = false;  ///< this frame begins a play (a new clip, or a repeat)
    i32 blendMs = 0;         ///< cross-fade for this start; 0 = hard cut
    i32 dtMs = 0;            ///< EXACT clock advance for this frame; 0 on a pass start
    /// @brief On a clip start, the play's first frame (absolute, sequence-local);
    ///        -1 on every other frame.
    ///
    /// Always set, not only for a trimmed clip: the runner re-bases to it on
    /// every clip start, which is a no-op for a fresh switch, the trim for a
    /// trimmed one, and the whole restart for a repeat of the sequence already
    /// playing — where there is no index change for the playlist to notice.
    i32 seekFrameMs = -1;
    bool holding = false;    ///< pin the pose, keep the world running
    i32 holdFrameMs = 0;     ///< the (absolute, sequence-local) pose to pin while holding
    f32 speed = 1.0f;        ///< actor playbackSpeed for this frame
    bool capture = true;     ///< false on a frame `frameStep` skips: simulate, do not record
    /// @brief Yaw for this frame in *recipe* space — the start yaw, the spin
    ///        so far and the multi-angle offset.
    ///
    /// Not the absolute camera yaw: with @ref ExportCameraMotion::yawRelative
    /// the recording begins on the framing the user is looking at, and the
    /// schedule has never seen a camera. The runner adds the base yaw it
    /// snapshotted at Build time.
    f32 yawDeg = 0.0f;
    i32 angle = 0;          ///< multi-angle pass index
    i32 clipFrameIndex = 0; ///< frame index within the current clip play
};

/// @brief A recipe resolved against one model's sequence table.
///
/// `Build` does all the arithmetic once; `At` is a binary search over a
/// prefix-sum table and allocates nothing.
class ExportSchedule {
public:
    static ExportSchedule Build(const ExportRecipe& recipe, std::span<const SequenceInfo> seqs);

    /// @brief Frames in one pass (one angle).
    i32 FrameCount() const {
        return frameCount_;
    }
    /// @brief Frames across every angle pass — what the runner loops over.
    i32 TotalFrameCount() const {
        return frameCount_ * angleCount_;
    }
    i32 AngleCount() const {
        return angleCount_;
    }
    /// @brief Simulation rate. The world is stepped at this rate whatever
    ///        @ref ExportTiming::frameStep is.
    i32 Fps() const {
        return fps_;
    }
    i32 FrameStep() const {
        return frameStep_;
    }
    /// @brief Playback rate of the written file — `fps / frameStep`.
    i32 OutputFps() const;
    /// @brief Frames actually written, across every angle pass.
    i32 CapturedFrameCount() const;
    /// @brief Recording length of one pass, in milliseconds.
    i32 DurationMs() const;

    ExportStep At(i32 frameIndex) const;

    /// @brief Frame index (within a pass) at which each queue segment starts,
    ///        for the timeline strip. Parallel to @ref SegmentClips.
    std::span<const i32> SegmentStarts() const {
        return starts_;
    }
    /// @brief The clip each segment plays, `-1` for a hold / fill segment.
    std::vector<i32> SegmentClips() const;
    /// @brief Frame index at which the fill region begins, or FrameCount()
    ///        when the queue was not filled.
    i32 FillStartFrame() const {
        return fillStartFrame_;
    }
    /// @brief The first queue clip that is never reached, or -1.
    i32 TruncatedFromClip() const {
        return truncatedFromClip_;
    }

    std::span<const std::string> Warnings() const {
        return warnings_;
    }

    /// @brief Exact accumulated actor-clock time at frame `i` of a pass.
    ///        `round(i * 1000 / fps)` — the value the per-frame `dtMs` sums to.
    static i32 ClockAtFrame(i32 frameIndex, i32 fps);

private:
    struct Segment {
        i32 clipIndex = -1;
        i32 sequence = 0;
        i32 frames = 0;
        bool hold = false;
        i32 holdFrameMs = 0;
        i32 seekFrameMs = -1;
        f32 speed = 1.0f;
        i32 blendMs = 0;
    };

    std::vector<Segment> segments_;
    std::vector<i32> starts_; ///< parallel to segments_; prefix sums of `frames`
    std::vector<std::string> warnings_;
    i32 frameCount_ = 0;
    i32 angleCount_ = 1;
    i32 fps_ = 30;
    i32 frameStep_ = 1;
    i32 fillStartFrame_ = 0;
    i32 truncatedFromClip_ = -1;
    // Orbit, pre-resolved so At() is arithmetic only.
    bool orbit_ = false;
    f32 orbitStartYawDeg_ = 0.0f;
    f32 orbitDegPerSec_ = 0.0f;
    f32 anglePitchDeg_ = 0.0f; ///< 360 / angleCount
};

// ---------------------------------------------------------------------------
// Naming
// ---------------------------------------------------------------------------

/// @brief The values `{token}` expands to for one output file.
struct NameTokens {
    std::string model;
    std::string anim; ///< first clip's name, or "queue" for a multi-clip queue
    std::string clip; ///< the clip playing at this frame
    i32 index = 0;
    i32 clipFrame = 0;
    i32 angle = 0;
    i32 fps = 30;
    i32 width = 0, height = 0;
    std::string date; ///< YYYYMMDD
    std::string time; ///< HHMMSS
};

/// @brief Expand `{token}` / `{token:04}` against `tokens`.
///
/// Unknown tokens are left literal and appended to `unknown` (once each) so
/// the caller can report them rather than silently producing a filename with
/// a brace in it.
std::string ExpandNameTemplate(std::string_view tmpl, const NameTokens& tokens,
                               std::vector<std::string>* unknown = nullptr);

/// @brief The default name template for a format's layout.
const char* DefaultNameTemplate(ExportFormat format);

/// @brief True when a per-frame layout would write several frames to the same
///        file.
///
/// `{index}` is the only token guaranteed to differ on every frame: `{clip}`
/// repeats within a clip, and `{clipframe}` repeats across clips AND across a
/// clip's own repeats. Without it the export silently produces fewer files
/// than frames, which reads as a capture failure rather than a naming one.
bool NameTemplateCollides(std::string_view tmpl, ExportFormat format);

/// @brief Strip characters no filesystem wants. Same rule the exporter has
///        always applied to model and animation names.
std::string SanitizeExportName(std::string_view name);

// ---------------------------------------------------------------------------
// Sequence naming
// ---------------------------------------------------------------------------

/// @brief The persistence key for sequence `index`: `Name`, or `Name#N` when
///        the model ships several sequences with that name.
///
/// Sequence *indices* are export order and mean nothing across models, so a
/// saved queue has to name what it wants. Names are not unique either — an
/// MDX routinely ships several `Stand` variants — hence the occurrence.
std::string SequenceKey(std::span<const std::string> names, i32 index);

/// @brief Resolve a `SequenceKey` back to an index, or -1.
///
/// Order: exact name + occurrence, then the name's first occurrence, then a
/// whitespace-trimmed match. Never guesses beyond that — an unresolved clip
/// is shown to the user, not silently repointed.
i32 ResolveSequenceKey(std::span<const std::string> names, std::string_view key);

} // namespace whiteout::flakes
