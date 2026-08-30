#pragma once

// ============================================================================
// Where captured frames go.
//
// One interface, one implementation per output layout. The point of the split
// is memory: the original exporter collected every frame into a
// `std::vector<Texture>` before encoding anything, which is 8.3 MB a frame at
// 1080p — a thirty-second recording at 30 fps is 7.5 GB. Fixed-duration
// recordings make that routine, so the sinks that *can* stream do.
//
//   PNG frames   1 frame held   (writes each frame as it arrives)
//   WebP         1 frame held   (WebPAnimEncoderAdd is incremental)
//   Video        1 frame held   (raw RGBA down an ffmpeg pipe)
//   Sheet        1 frame held   (blitted into the sheet on arrival)
//   GIF          all            (gif::Writer::write takes the whole vector)
//   APNG         all            (writeAnimated takes the whole vector — but
//                                the frames are *moved* into it, not copied,
//                                which is the 2x the old path paid)
// ============================================================================

#include "export_recipe.h"

#include <whiteout/textures/texture.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace whiteout::flakes {

/// @brief What a sink produced.
struct ExportSinkResult {
    i32 framesWritten = 0;
    std::vector<std::filesystem::path> files;
    std::string error; ///< empty on success
    // Sheet layout only.
    i32 sheetColumns = 0, sheetRows = 0;
    i32 cellWidth = 0, cellHeight = 0;
    // Auto-crop only; zero width/height means the frames were not cropped.
    i32 cropX = 0, cropY = 0, cropWidth = 0, cropHeight = 0;
};

struct ExportSinkSetup {
    ExportFormat format = ExportFormat::PngFrames;
    std::filesystem::path folder;
    /// @brief Expanded name (no extension) for a single-file output.
    std::string baseName;
    /// @brief Expanded name (no extension) for one frame of a Frames layout.
    std::function<std::string(const ExportStep&)> frameName;
    /// @brief Playback rate of the written file — `fps / frameStep`.
    i32 fps = 30;
    bool transparent = false;
    i32 sheetColumns = 0; ///< 0 = auto
    i32 expectedFrames = 0;
    /// @brief Frames in one angle pass. With more than one pass, the sheet
    ///        auto-layout uses this as its column count so each ROW is one
    ///        direction — which is the whole point of a directional rip.
    i32 framesPerPass = 0;
    /// @brief Progress / diagnostics, e.g. "encoding 900-frame GIF…".
    std::function<void(const std::string&)> log;
};

class IExportSink {
public:
    virtual ~IExportSink() = default;
    virtual bool Begin(const ExportSinkSetup& setup) = 0;
    virtual void Accept(const ExportStep& step, whiteout::textures::Texture&& frame) = 0;
    virtual bool Finish(ExportSinkResult& out) = 0;
    /// @brief Frames currently held in RAM, for the runner's progress line.
    virtual usize BufferedFrames() const {
        return 0;
    }
};

/// @brief The sink for `format`, wrapped in the auto-crop decorator when the
///        recipe asks for one.
///
/// Auto-crop cannot stream: the rect is the union over *every* frame (a
/// per-frame crop makes the subject jitter), so the decorator buffers. That is
/// a deliberate, opt-in cost, and the dialog's memory estimate accounts for it.
std::unique_ptr<IExportSink> MakeExportSink(const ExportOutput& output);

/// @brief Bytes the sink for `output` will hold at its peak.
///
/// What the dialog's memory callout is computed from. RGBA8, so 4 bytes a
/// pixel; the buffered formats hold every frame, the streaming ones hold one.
u64 EstimateSinkMemory(const ExportOutput& output, i32 frames, i32 width, i32 height);

/// @brief True when an external `ffmpeg` is on PATH. Probed once and cached —
///        the video formats grey out without it, the way WebP already does
///        when it was not compiled in.
bool FfmpegAvailable();

/// @brief False when the format needs something this build or machine has
///        not got. `reason` is a short user-facing sentence when so.
bool ExportFormatAvailable(ExportFormat format, std::string* reason = nullptr);

/// @brief Per-frame display duration in milliseconds, clamped to the 16-bit
///        range the container formats store it in.
i32 ExportFrameDelayMs(i32 fps);

/// @brief The rate the container can actually represent.
///
/// GIF stores centiseconds and APNG/WebP whole milliseconds, so the rate the
/// user typed is not always the rate they get: 30 fps is 3 cs = 33.3 fps in a
/// GIF, 60 fps is 2 cs = 50 fps. The dialog says so next to the format.
f32 EffectiveFrameRate(ExportFormat format, i32 fps);

} // namespace whiteout::flakes
