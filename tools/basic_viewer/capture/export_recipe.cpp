#include "capture/export_recipe.h"

#include "string_util.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace whiteout::flakes {
namespace {

i32 RoundFrames(f64 ms, i32 fps) {
    return static_cast<i32>(std::llround(ms * fps / 1000.0));
}

using tools::ToLowerAscii;
using tools::TrimWhitespace;

} // namespace

// ---------------------------------------------------------------------------
// Formats
// ---------------------------------------------------------------------------

const ExportFormatInfo& GetExportFormatInfo(ExportFormat format) {
    static_assert(kExportFormats.size() == static_cast<usize>(ExportFormat::WebmVp9) + 1,
                  "kExportFormats has one row per ExportFormat");
    const i32 i = static_cast<i32>(format);
    return kExportFormats[static_cast<usize>((i >= 0 && i < kExportFormatCount) ? i : 0)];
}

bool ParseExportFormat(std::string_view token, ExportFormat& out) {
    const std::string want = ToLowerAscii(TrimWhitespace(token));
    for (i32 i = 0; i < kExportFormatCount; ++i) {
        if (want == GetExportFormatInfo(static_cast<ExportFormat>(i)).iniName) {
            out = static_cast<ExportFormat>(i);
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Schedule
// ---------------------------------------------------------------------------

i32 ExportSchedule::ClockAtFrame(i32 frameIndex, i32 fps) {
    if (fps <= 0)
        return 0;
    // The whole point of the schedule owning the clock. Actor::Advance
    // truncates `dtSec * 1000 + 0.5` to integer milliseconds, so a constant
    // dt = 1/fps accumulates integer error — nine frames short over thirty
    // seconds at 30 fps, thirty-six frames long at 60. Deriving each frame's
    // target clock from its index and emitting the *difference* makes the
    // accumulated clock exactly this value on every frame, at every rate.
    return static_cast<i32>((static_cast<i64>(frameIndex) * 1000 + fps / 2) / fps);
}

i32 ExportSchedule::DurationMs() const {
    return ClockAtFrame(frameCount_, fps_);
}

i32 ExportSchedule::OutputFps() const {
    return std::max(1, static_cast<i32>(std::llround(static_cast<f64>(fps_) / frameStep_)));
}

i32 ExportSchedule::CapturedFrameCount() const {
    if (frameCount_ <= 0)
        return 0;
    const i32 perPass = (frameCount_ + frameStep_ - 1) / frameStep_;
    return perPass * angleCount_;
}

std::vector<i32> ExportSchedule::SegmentClips() const {
    std::vector<i32> out;
    out.reserve(segments_.size());
    for (const Segment& s : segments_)
        out.push_back(s.clipIndex);
    return out;
}

ExportSchedule ExportSchedule::Build(const ExportRecipe& recipe, std::span<const SequenceInfo> seqs) {
    ExportSchedule sch;
    sch.fps_ = std::clamp(recipe.timing.fps, 1, 240);
    sch.frameStep_ = std::clamp(recipe.timing.frameStep, 1, 64);
    const i32 fps = sch.fps_;

    const bool orbit = recipe.camera.mode == ExportCameraMode::Orbit;
    sch.angleCount_ = orbit ? std::clamp(recipe.camera.angleCount, 1, 64) : 1;
    if (!orbit && recipe.camera.angleCount > 1)
        sch.warnings_.emplace_back("Multi-angle passes need the turntable camera; using one pass.");

    if (seqs.empty()) {
        sch.warnings_.emplace_back("The model has no animation sequences.");
        return sch;
    }

    // ---- one pass over the queue, expanded into segments ----
    const i32 n = static_cast<i32>(seqs.size());
    std::vector<Segment> base;
    // Where each clip's own segments begin in `base`, so LoopLast can repeat
    // the last clip rather than the last segment.
    std::vector<usize> clipSegStart;

    for (usize c = 0; c < recipe.clips.size(); ++c) {
        const ExportClip& clip = recipe.clips[c];
        clipSegStart.push_back(base.size());
        if (!clip.Resolved()) {
            sch.warnings_.emplace_back("Animation '" +
                                       (clip.savedName.empty() ? std::string("(unknown)")
                                                               : clip.savedName) +
                                       "' is not in this model; it contributes no frames.");
            continue;
        }
        const SequenceInfo& seq = seqs[((clip.sequence % n) + n) % n];
        const i32 seqLen = std::max(0, seq.endMs - seq.startMs);
        const i32 trimStart = std::clamp(clip.trimStartMs, 0, seqLen);
        const i32 trimEnd = (clip.trimEndMs > 0) ? std::clamp(clip.trimEndMs, trimStart, seqLen)
                                                 : seqLen;
        const i32 startAbs = seq.startMs + trimStart;
        const i32 endAbs = seq.startMs + trimEnd;
        const i32 lenMs = endAbs - startAbs;
        const f32 speed = (clip.speed > 0.001f) ? clip.speed : 1.0f;

        // A zero-length sequence is a static pose: exactly one frame, matching
        // the original exporter's `durationMs <= 0` fallback.
        const i32 playFrames =
            (lenMs <= 0) ? 1 : std::max(1, RoundFrames(static_cast<f64>(lenMs) / speed, fps));

        Segment play;
        play.clipIndex = static_cast<i32>(c);
        play.sequence = clip.sequence;
        play.frames = playFrames;
        play.speed = speed;
        play.blendMs = std::max(0, clip.blendMs);
        play.seekFrameMs = startAbs;

        // Each repeat is its own segment. That is not just simpler than a
        // repeat count: it is the only thing that makes a *non-looping*
        // sequence repeat (the natural wrap needs ignoreNonLooping), and it
        // lets a repeat cross-fade into itself.
        const i32 repeats = std::clamp(clip.repeats, 1, 999);
        for (i32 r = 0; r < repeats; ++r)
            base.push_back(play);

        if (clip.holdMs > 0) {
            Segment hold;
            hold.clipIndex = static_cast<i32>(c);
            hold.sequence = clip.sequence;
            hold.frames = std::max(1, RoundFrames(clip.holdMs, fps));
            hold.hold = true;
            // One millisecond short of the end, NOT the end itself. The
            // playlist windows a scrub through the same wrap as playback, so
            // pinning a looping sequence at exactly `endMs` lands back on its
            // FIRST frame — a hold on the end of a walk cycle would silently
            // freeze the wrong pose. Clamped for a zero-length static pose.
            hold.holdFrameMs = std::max(startAbs, endAbs - 1);
            base.push_back(hold);
        }
    }

    i32 baseFrames = 0;
    for (const Segment& s : base)
        baseFrames += s.frames;

    if (base.empty() || baseFrames <= 0) {
        if (sch.warnings_.empty())
            sch.warnings_.emplace_back("The queue is empty.");
        return sch;
    }

    // ---- duration policy ----
    i32 target = baseFrames;
    if (recipe.timing.duration == ExportDuration::Fixed)
        target = std::max(1, RoundFrames(std::max(0, recipe.timing.durationMs), fps));

    // Emit the queue, truncating at `target`.
    i32 emitted = 0;
    for (usize i = 0; i < base.size() && emitted < target; ++i) {
        Segment s = base[i];
        s.frames = std::min(s.frames, target - emitted);
        emitted += s.frames;
        sch.segments_.push_back(s);
    }
    if (emitted >= target && sch.segments_.size() < base.size()) {
        // Name the first clip a Fixed length never reaches, so the dialog can
        // say so rather than leaving the user to notice the missing animation.
        for (usize i = sch.segments_.size(); i < base.size(); ++i) {
            const i32 c = base[i].clipIndex;
            bool reached = false;
            for (const Segment& done : sch.segments_)
                reached = reached || done.clipIndex == c;
            if (!reached) {
                sch.truncatedFromClip_ = c;
                break;
            }
        }
    }

    sch.fillStartFrame_ = emitted;

    // ---- fill ----
    if (emitted < target) {
        const Segment& last = sch.segments_.back();
        switch (recipe.timing.fill) {
        case ExportFill::HoldLast: {
            // The final pose freezes. Pin the end of the last *play*; if the
            // queue ended on a hold already, extend that one.
            Segment hold;
            hold.clipIndex = last.clipIndex;
            hold.sequence = last.sequence;
            hold.hold = true;
            hold.holdFrameMs = last.hold ? last.holdFrameMs : 0;
            if (!last.hold) {
                // Recover the play's end pose from the clip it came from —
                // one millisecond short of the end, for the wrap above.
                const ExportClip& clip = recipe.clips[static_cast<usize>(last.clipIndex)];
                const SequenceInfo& seq = seqs[((clip.sequence % n) + n) % n];
                const i32 seqLen = std::max(0, seq.endMs - seq.startMs);
                const i32 endAbs =
                    seq.startMs + ((clip.trimEndMs > 0) ? std::min(clip.trimEndMs, seqLen) : seqLen);
                hold.holdFrameMs = std::max(seq.startMs, endAbs - 1);
            }
            hold.frames = target - emitted;
            emitted = target;
            sch.segments_.push_back(hold);
            break;
        }
        case ExportFill::LoopLast: {
            // Repeat the last clip's own segments — its plays and its hold —
            // rather than the last segment, so a clip with a hold keeps it.
            const i32 lastClip = last.clipIndex;
            std::vector<Segment> unit;
            for (const Segment& s : base)
                if (s.clipIndex == lastClip)
                    unit.push_back(s);
            if (unit.empty())
                unit.push_back(last);
            while (emitted < target) {
                for (const Segment& s : unit) {
                    if (emitted >= target)
                        break;
                    Segment copy = s;
                    copy.frames = std::min(copy.frames, target - emitted);
                    emitted += copy.frames;
                    sch.segments_.push_back(copy);
                }
            }
            break;
        }
        case ExportFill::LoopQueue: {
            while (emitted < target) {
                for (const Segment& s : base) {
                    if (emitted >= target)
                        break;
                    Segment copy = s;
                    copy.frames = std::min(copy.frames, target - emitted);
                    emitted += copy.frames;
                    sch.segments_.push_back(copy);
                }
            }
            break;
        }
        }
    }

    // ---- prefix sums ----
    sch.starts_.reserve(sch.segments_.size());
    i32 acc = 0;
    for (const Segment& s : sch.segments_) {
        sch.starts_.push_back(acc);
        acc += s.frames;
    }
    sch.frameCount_ = acc;

    // ---- orbit ----
    sch.orbit_ = orbit;
    sch.anglePitchDeg_ = (sch.angleCount_ > 1) ? 360.0f / static_cast<f32>(sch.angleCount_) : 0.0f;
    if (orbit) {
        sch.orbitStartYawDeg_ = recipe.camera.startYawDeg;
        if (recipe.camera.timing == OrbitTiming::Revolutions) {
            // Derived from the recording length, which is the only way a
            // turntable's last frame lands one step short of its first.
            const f64 totalSec = static_cast<f64>(sch.frameCount_) / fps;
            sch.orbitDegPerSec_ =
                (totalSec > 0.0)
                    ? static_cast<f32>(360.0 * recipe.camera.revolutions / totalSec)
                    : 0.0f;
        } else {
            sch.orbitDegPerSec_ = recipe.camera.degPerSec;
        }
    }

    return sch;
}

ExportStep ExportSchedule::At(i32 frameIndex) const {
    ExportStep step;
    if (frameCount_ <= 0)
        return step;

    const i32 total = TotalFrameCount();
    const i32 idx = std::clamp(frameIndex, 0, total - 1);
    step.frameIndex = idx;
    step.angle = idx / frameCount_;
    const i32 local = idx % frameCount_;

    // Each angle pass is its own recording: the clock rewinds and the queue
    // starts over, so the lighting is identical across directions.
    step.passStart = (local == 0);
    step.dtMs = step.passStart ? 0 : (ClockAtFrame(local, fps_) - ClockAtFrame(local - 1, fps_));
    step.capture = (local % frameStep_) == 0;

    // Binary search the prefix sums: the last segment whose start is <= local.
    const auto it = std::upper_bound(starts_.begin(), starts_.end(), local);
    const usize s = static_cast<usize>(it - starts_.begin() - 1);
    const Segment& seg = segments_[s];
    const i32 inSeg = local - starts_[s];

    step.clipIndex = seg.clipIndex;
    step.sequence = seg.sequence;
    step.clipFrameIndex = inSeg;
    step.holding = seg.hold;
    step.holdFrameMs = seg.holdFrameMs;
    // The actor clock keeps running through a hold — that is what separates a
    // hold from a pause. `playbackSpeed = 0` would stop the children and the
    // emitters with their ancestor; the pose is pinned by a per-frame scrub
    // instead, which the runner does off `holding`.
    step.speed = seg.hold ? 1.0f : seg.speed;
    if (!seg.hold && inSeg == 0) {
        step.clipStart = true;
        step.blendMs = seg.blendMs;
        step.seekFrameMs = seg.seekFrameMs;
    }

    if (orbit_) {
        step.yawDeg = orbitStartYawDeg_ +
                      orbitDegPerSec_ * (static_cast<f32>(local) / static_cast<f32>(fps_)) +
                      anglePitchDeg_ * static_cast<f32>(step.angle);
    }
    return step;
}

// ---------------------------------------------------------------------------
// Naming
// ---------------------------------------------------------------------------

std::string SanitizeExportName(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    bool lastUnderscore = false;
    for (unsigned char c : name) {
        if (std::isalnum(c)) {
            out.push_back(tools::ToLowerAscii(static_cast<char>(c)));
            lastUnderscore = false;
        } else if (!lastUnderscore) {
            out.push_back('_');
            lastUnderscore = true;
        }
    }
    while (!out.empty() && out.back() == '_')
        out.pop_back();
    usize start = 0;
    while (start < out.size() && out[start] == '_')
        ++start;
    out = out.substr(start);
    return out.empty() ? std::string("unnamed") : out;
}

const char* DefaultNameTemplate(ExportFormat format) {
    // The defaults reproduce the original exporter's filenames exactly, so an
    // existing workflow — and the `--export-anim` invocations recorded in the
    // parity documents — keep writing the same names.
    switch (LayoutOf(format)) {
    case ExportLayout::Frames:
        return "{model}_{anim}_{index:04}";
    case ExportLayout::Sheet:
    case ExportLayout::Animated:
    case ExportLayout::Video:
        break;
    }
    return "{model}_{anim}";
}

bool NameTemplateCollides(std::string_view tmpl, ExportFormat format) {
    if (LayoutOf(format) != ExportLayout::Frames)
        return false;
    return ToLowerAscii(tmpl).find("{index") == std::string::npos;
}

std::string ExpandNameTemplate(std::string_view tmpl, const NameTokens& tokens,
                               std::vector<std::string>* unknown) {
    const auto pad = [](i64 value, i32 width) {
        std::string digits = std::to_string(value);
        const bool neg = !digits.empty() && digits[0] == '-';
        std::string body = neg ? digits.substr(1) : digits;
        while (static_cast<i32>(body.size()) < width)
            body.insert(body.begin(), '0');
        return neg ? "-" + body : body;
    };

    std::string out;
    out.reserve(tmpl.size() + 16);
    for (usize i = 0; i < tmpl.size();) {
        if (tmpl[i] != '{') {
            out.push_back(tmpl[i++]);
            continue;
        }
        const usize close = tmpl.find('}', i + 1);
        if (close == std::string_view::npos) {
            out.append(tmpl.substr(i));
            break;
        }
        const std::string_view body = tmpl.substr(i + 1, close - i - 1);
        const usize colon = body.find(':');
        const std::string name = ToLowerAscii(body.substr(0, colon));
        i32 width = 0;
        if (colon != std::string_view::npos) {
            for (char c : body.substr(colon + 1))
                if (c >= '0' && c <= '9')
                    width = width * 10 + (c - '0');
        }

        bool known = true;
        if (name == "model")
            out += tokens.model;
        else if (name == "anim")
            out += tokens.anim;
        else if (name == "clip")
            out += tokens.clip;
        else if (name == "index")
            out += pad(tokens.index, width);
        else if (name == "clipframe")
            out += pad(tokens.clipFrame, width);
        else if (name == "angle")
            out += pad(tokens.angle, width);
        else if (name == "fps")
            out += pad(tokens.fps, width);
        else if (name == "w")
            out += pad(tokens.width, width);
        else if (name == "h")
            out += pad(tokens.height, width);
        else if (name == "date")
            out += tokens.date;
        else if (name == "time")
            out += tokens.time;
        else
            known = false;

        if (!known) {
            // Left literal rather than dropped: a filename with a visible
            // `{frameno}` in it is a bug the user can see and fix, and an
            // empty one is a collision they cannot.
            out.append(tmpl.substr(i, close - i + 1));
            if (unknown) {
                const std::string token(body);
                if (std::find(unknown->begin(), unknown->end(), token) == unknown->end())
                    unknown->push_back(token);
            }
        }
        i = close + 1;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Sequence keys
// ---------------------------------------------------------------------------

std::string SequenceKey(std::span<const std::string> names, i32 index) {
    if (index < 0 || index >= static_cast<i32>(names.size()))
        return {};
    const std::string& want = names[static_cast<usize>(index)];
    i32 occurrence = 0;
    for (i32 i = 0; i < index; ++i)
        if (names[static_cast<usize>(i)] == want)
            ++occurrence;
    bool duplicated = occurrence > 0;
    for (i32 i = index + 1; !duplicated && i < static_cast<i32>(names.size()); ++i)
        duplicated = names[static_cast<usize>(i)] == want;
    if (!duplicated)
        return want;
    return want + "#" + std::to_string(occurrence);
}

i32 ResolveSequenceKey(std::span<const std::string> names, std::string_view key) {
    if (names.empty() || key.empty())
        return -1;

    std::string_view name = key;
    i32 occurrence = 0;
    const usize hash = key.rfind('#');
    if (hash != std::string_view::npos && hash + 1 < key.size()) {
        bool digits = true;
        i32 parsed = 0;
        for (char c : key.substr(hash + 1)) {
            if (c < '0' || c > '9') {
                digits = false;
                break;
            }
            parsed = parsed * 10 + (c - '0');
        }
        if (digits) {
            name = key.substr(0, hash);
            occurrence = parsed;
        }
    }

    // 1. exact name at the requested occurrence.
    i32 seen = 0;
    for (i32 i = 0; i < static_cast<i32>(names.size()); ++i) {
        if (names[static_cast<usize>(i)] == name) {
            if (seen == occurrence)
                return i;
            ++seen;
        }
    }
    // 2. the name's first occurrence — the model has it, just fewer copies.
    for (i32 i = 0; i < static_cast<i32>(names.size()); ++i)
        if (names[static_cast<usize>(i)] == name)
            return i;
    // 3. whitespace-trimmed, case-insensitive. Model sequence names do carry
    //    trailing spaces, and the ini reader trims them on the way back in.
    const std::string want = ToLowerAscii(TrimWhitespace(name));
    for (i32 i = 0; i < static_cast<i32>(names.size()); ++i)
        if (ToLowerAscii(TrimWhitespace(names[static_cast<usize>(i)])) == want)
            return i;
    return -1;
}

} // namespace whiteout::flakes
