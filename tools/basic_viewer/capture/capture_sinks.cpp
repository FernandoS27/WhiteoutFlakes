#include "capture/capture_sinks.h"

#include "whiteout/flakes/util/path_utf8.h"

#include <whiteout/textures/gif/writer.h>
#include <whiteout/textures/png/writer.h>
#include <whiteout/utils/simple_thread_pool.h>

#if defined(WDX_HAVE_WEBP)
#include <webp/encode.h>
#include <webp/mux.h>
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#define WDX_POPEN _popen
#define WDX_PCLOSE _pclose
#else
#define WDX_POPEN popen
#define WDX_PCLOSE pclose
#endif

namespace whiteout::flakes {
namespace {

namespace fs = std::filesystem;
using whiteout::textures::PixelFormat;
using whiteout::textures::Texture;

/// RGBA8's bytes per pixel.
constexpr usize kRgbaBytes = 4;
/// A frame delay when the rate is not one (0 fps): 30 fps.
constexpr i32 kDefaultFrameDelayMs = 33;
constexpr f64 kMillisecondsPerSecond = 1000.0;
constexpr f64 kCentisecondsPerSecond = 100.0;
/// GIF, APNG and WebP store a frame's delay in 16 bits.
constexpr i32 kMaxFrameDelay = 65535;
/// libwebp's effort for lossless (not a lossy quality: `lossless` is set).
constexpr f32 kWebpEffort = 90.0f;
/// ffmpeg's constant-rate factors: visually lossless H.264, and VP9's
/// equivalent.
constexpr i32 kMp4Crf = 18;
constexpr i32 kVp9Crf = 24;

constexpr const char* kNoFrames = "no frames captured";

/// A delay for @p fps frames a second in @p unitsPerSecond, clamped to what the
/// containers store.
i32 FrameDelay(i32 fps, f64 unitsPerSecond) {
    return std::clamp<i32>(static_cast<i32>(std::llround(unitsPerSecond / fps)), 1, kMaxFrameDelay);
}

void Log(const ExportSinkSetup& s, const std::string& msg) {
    if (s.log)
        s.log(msg);
}

Texture EnsureRgba8(Texture&& t) {
    if (t.format() == PixelFormat::RGBA8)
        return std::move(t);
    return t.copyAsFormat(PixelFormat::RGBA8);
}

ExportSinkResult Failed(std::string error) {
    ExportSinkResult result;
    result.error = std::move(error);
    return result;
}

// ---------------------------------------------------------------------------
// PNG frames — streams
// ---------------------------------------------------------------------------

class PngFrameSink final : public IExportSink {
public:
    bool Begin(const ExportSinkSetup& setup) override {
        setup_ = setup;
        return true;
    }
    void Accept(const ExportStep& step, Texture&& frame) override {
        std::string stem = setup_.frameName ? setup_.frameName(step) : std::string("frame");
        const fs::path file = setup_.folder / io::FsPathFromUtf8(stem + ".png");
        writer_.write(io::PathToUtf8(file), frame);
        // The writer clears its issue list at the top of every write, so this
        // is the state of *this* frame, not an accumulation.
        if (writer_.hasIssues()) {
            if (result_.error.empty())
                result_.error = writer_.getIssues().front();
            return;
        }
        ++result_.framesWritten;
        // One entry per frame would be a thousand-element vector no caller
        // reads; the report only needs the folder and the count.
        if (result_.files.empty())
            result_.files.push_back(file);
    }
    ExportSinkResult Finish() override {
        return result_;
    }

private:
    ExportSinkSetup setup_;
    whiteout::textures::png::Writer writer_;
    ExportSinkResult result_;
};

// ---------------------------------------------------------------------------
// GIF — buffered (the writer takes the whole vector)
// ---------------------------------------------------------------------------

class GifSink final : public IExportSink {
public:
    bool Begin(const ExportSinkSetup& setup) override {
        setup_ = setup;
        if (setup.expectedFrames > 0)
            frames_.reserve(static_cast<usize>(setup.expectedFrames));
        return true;
    }
    void Accept(const ExportStep&, Texture&& frame) override {
        frames_.push_back(std::move(frame));
    }
    ExportSinkResult Finish() override {
        if (frames_.empty())
            return Failed(kNoFrames);
        const fs::path file = setup_.folder / io::FsPathFromUtf8(setup_.baseName + ".gif");
        Log(setup_, "encoding " + std::to_string(frames_.size()) + "-frame GIF (palette quantise)");

        const unsigned hw = std::thread::hardware_concurrency();
        whiteout::utils::SimpleThreadPool pool(hw > 1 ? hw : 2);
        whiteout::textures::gif::Writer writer(&pool);
        whiteout::textures::gif::SaveOptions opts;
        opts.delayCs = static_cast<u16>(FrameDelay(setup_.fps, kCentisecondsPerSecond));
        opts.loopCount = 0;
        opts.transparent = setup_.transparent;
        writer.write(io::PathToUtf8(file), frames_, opts);
        if (writer.hasIssues())
            return Failed(writer.getIssues().front());
        ExportSinkResult out;
        out.framesWritten = static_cast<i32>(frames_.size());
        out.files.push_back(file);
        return out;
    }

private:
    ExportSinkSetup setup_;
    std::vector<Texture> frames_;
};

// ---------------------------------------------------------------------------
// APNG — buffered, but only once
// ---------------------------------------------------------------------------

class ApngSink final : public IExportSink {
public:
    bool Begin(const ExportSinkSetup& setup) override {
        setup_ = setup;
        delayMs_ = static_cast<u32>(ExportFrameDelayMs(setup.fps));
        if (setup.expectedFrames > 0)
            frames_.reserve(static_cast<usize>(setup.expectedFrames));
        return true;
    }
    void Accept(const ExportStep&, Texture&& frame) override {
        // Built directly as ApngFrames, moving each texture in. The old path
        // collected a vector<Texture> and then copied it into a
        // vector<ApngFrame> — and ApngFrame holds its Texture by value, whose
        // copy constructor is a deep copy, so an APNG export peaked at twice
        // the pixels it needed.
        frames_.push_back({std::move(frame), delayMs_});
    }
    ExportSinkResult Finish() override {
        if (frames_.empty())
            return Failed(kNoFrames);
        const fs::path file = setup_.folder / io::FsPathFromUtf8(setup_.baseName + ".apng");
        Log(setup_, "encoding " + std::to_string(frames_.size()) + "-frame APNG");
        whiteout::textures::png::Writer writer;
        whiteout::textures::png::ApngSaveOptions opts;
        opts.loopCount = 0;
        writer.writeAnimated(io::PathToUtf8(file), frames_, opts);
        if (writer.hasIssues())
            return Failed(writer.getIssues().front());
        ExportSinkResult out;
        out.framesWritten = static_cast<i32>(frames_.size());
        out.files.push_back(file);
        return out;
    }

private:
    ExportSinkSetup setup_;
    u32 delayMs_ = kDefaultFrameDelayMs;
    std::vector<whiteout::textures::png::ApngFrame> frames_;
};

#if defined(WDX_HAVE_WEBP)
// ---------------------------------------------------------------------------
// WebP — streams
// ---------------------------------------------------------------------------

class WebpSink final : public IExportSink {
public:
    ~WebpSink() override {
        if (enc_)
            WebPAnimEncoderDelete(enc_);
    }

    bool Begin(const ExportSinkSetup& setup) override {
        setup_ = setup;
        delayMs_ = ExportFrameDelayMs(setup.fps);
        return true;
    }

    void Accept(const ExportStep&, Texture&& frame) override {
        if (!error_.empty())
            return;
        const Texture rgba = EnsureRgba8(std::move(frame));
        if (!enc_) {
            // The canvas is frame 0's size; the encoder cannot be created
            // before one has arrived.
            width_ = static_cast<i32>(rgba.width());
            height_ = static_cast<i32>(rgba.height());
            WebPAnimEncoderOptions encOpts;
            WebPAnimEncoderOptionsInit(&encOpts);
            encOpts.anim_params.loop_count = 0;
            encOpts.anim_params.bgcolor = 0; // transparent ARGB background
            enc_ = WebPAnimEncoderNew(width_, height_, &encOpts);
            if (!enc_) {
                error_ = "WebP encoder creation failed";
                return;
            }
            WebPConfigInit(&config_);
            config_.lossless = 1; // bit-exact pixels + full alpha
            config_.quality = kWebpEffort;
            WebPValidateConfig(&config_);
        }
        if (static_cast<i32>(rgba.width()) != width_ || static_cast<i32>(rgba.height()) != height_)
            return; // a resize mid-recording is not something WebP can carry

        WebPPicture pic;
        WebPPictureInit(&pic);
        pic.use_argb = 1;
        pic.width = width_;
        pic.height = height_;
        if (!WebPPictureImportRGBA(&pic, rgba.dataPtr(), width_ * static_cast<int>(kRgbaBytes)) ||
            !WebPAnimEncoderAdd(enc_, &pic, timestampMs_, &config_)) {
            error_ = WebPAnimEncoderGetError(enc_);
            WebPPictureFree(&pic);
            return;
        }
        WebPPictureFree(&pic);
        timestampMs_ += delayMs_;
        ++written_;
    }

    ExportSinkResult Finish() override {
        if (!error_.empty() || !enc_)
            return Failed(error_.empty() ? kNoFrames : error_);
        // WebPAnimEncoderAdd takes each frame's *start* timestamp; the
        // duration is the gap to the next, so a trailing NULL frame gives the
        // last one its.
        WebPAnimEncoderAdd(enc_, nullptr, timestampMs_, nullptr);
        WebPData data;
        WebPDataInit(&data);
        if (!WebPAnimEncoderAssemble(enc_, &data)) {
            WebPDataClear(&data);
            return Failed(WebPAnimEncoderGetError(enc_));
        }
        const fs::path file = setup_.folder / io::FsPathFromUtf8(setup_.baseName + ".webp");
        std::ofstream f(file, std::ios::binary);
        f.write(reinterpret_cast<const char*>(data.bytes), static_cast<std::streamsize>(data.size));
        const bool ok = static_cast<bool>(f);
        WebPDataClear(&data);
        if (!ok)
            return Failed("WebP write failed for " + io::PathToUtf8(file));
        ExportSinkResult out;
        out.framesWritten = written_;
        out.files.push_back(file);
        return out;
    }

private:
    ExportSinkSetup setup_;
    std::string error_;
    i32 written_ = 0;
    WebPAnimEncoder* enc_ = nullptr;
    WebPConfig config_{};
    i32 width_ = 0, height_ = 0;
    int timestampMs_ = 0;
    int delayMs_ = kDefaultFrameDelayMs;
};
#else
constexpr const char* kWebpMissing =
    "WebP support was not built (reconfigure with -DWDX_ENABLE_WEBP=ON)";

/// A format this build cannot write: refuses to start, and says why.
class UnavailableSink final : public IExportSink {
public:
    explicit UnavailableSink(const char* why) : why_(why) {}
    bool Begin(const ExportSinkSetup&) override {
        return false;
    }
    void Accept(const ExportStep&, Texture&&) override {}
    ExportSinkResult Finish() override {
        return Failed(why_);
    }

private:
    const char* why_;
};
#endif

// ---------------------------------------------------------------------------
// Sprite sheet — streams into one image
// ---------------------------------------------------------------------------

class SheetSink final : public IExportSink {
public:
    bool Begin(const ExportSinkSetup& setup) override {
        setup_ = setup;
        return setup.expectedFrames > 0;
    }

    void Accept(const ExportStep&, Texture&& frame) override {
        const Texture rgba = EnsureRgba8(std::move(frame));
        if (sheet_.width() == 0) {
            cellW_ = static_cast<i32>(rgba.width());
            cellH_ = static_cast<i32>(rgba.height());
            const i32 n = std::max(1, setup_.expectedFrames);
            if (setup_.sheetColumns > 0) {
                cols_ = setup_.sheetColumns;
            } else if (setup_.framesPerPass > 0 && setup_.framesPerPass < n) {
                // Multi-angle: one row per direction, which is what makes a
                // directional rip usable. Overrides the near-square rule,
                // because a sheet whose rows do not line up with the passes is
                // no better than a strip.
                cols_ = setup_.framesPerPass;
            } else {
                // Auto: near-square in *pixels*, not in cells — a 1280x256
                // frame tiled square-in-cells is a 10k-wide image nothing will
                // open.
                cols_ = std::max(1, static_cast<i32>(std::lround(
                                        std::sqrt(static_cast<f64>(n) * cellH_ / cellW_))));
            }
            cols_ = std::clamp(cols_, 1, n);
            rows_ = (n + cols_ - 1) / cols_;
            sheet_ = Texture::create2D(PixelFormat::RGBA8, static_cast<u32>(cols_ * cellW_),
                                       static_cast<u32>(rows_ * cellH_), 1);
            auto dst = sheet_.mipData(0);
            std::memset(dst.data(), 0, dst.size());
        }
        if (static_cast<i32>(rgba.width()) != cellW_ || static_cast<i32>(rgba.height()) != cellH_)
            return;
        const i32 cell = written_++;
        if (cell >= cols_ * rows_)
            return;
        const i32 cx = (cell % cols_) * cellW_;
        const i32 cy = (cell / cols_) * cellH_;
        auto dst = sheet_.mipData(0);
        auto src = rgba.mipData(0);
        const i32 sheetW = cols_ * cellW_;
        for (i32 y = 0; y < cellH_; ++y) {
            std::memcpy(dst.data() + (static_cast<usize>(cy + y) * sheetW + cx) * kRgbaBytes,
                        src.data() + static_cast<usize>(y) * cellW_ * kRgbaBytes,
                        static_cast<usize>(cellW_) * kRgbaBytes);
        }
    }

    ExportSinkResult Finish() override {
        if (sheet_.width() == 0)
            return Failed(kNoFrames);
        const fs::path file = setup_.folder / io::FsPathFromUtf8(setup_.baseName + ".png");
        whiteout::textures::png::Writer writer;
        writer.write(io::PathToUtf8(file), sheet_);
        if (writer.hasIssues())
            return Failed(writer.getIssues().front());
        ExportSinkResult out;
        out.framesWritten = written_;
        out.files.push_back(file);
        out.sheetColumns = cols_;
        out.sheetRows = rows_;
        out.cellWidth = cellW_;
        out.cellHeight = cellH_;
        return out;
    }

private:
    ExportSinkSetup setup_;
    Texture sheet_;
    i32 cols_ = 0, rows_ = 0, cellW_ = 0, cellH_ = 0, written_ = 0;
};

// ---------------------------------------------------------------------------
// Video — raw RGBA down an ffmpeg pipe
// ---------------------------------------------------------------------------

class VideoSink final : public IExportSink {
public:
    ~VideoSink() override {
        if (pipe_)
            WDX_PCLOSE(pipe_);
    }

    bool Begin(const ExportSinkSetup& setup) override {
        setup_ = setup;
        if (!FfmpegAvailable()) {
            error_ = "ffmpeg was not found on PATH";
            return false;
        }
        return true;
    }

    void Accept(const ExportStep&, Texture&& frame) override {
        if (!error_.empty())
            return;
        const Texture rgba = EnsureRgba8(std::move(frame));
        if (!pipe_) {
            width_ = static_cast<i32>(rgba.width());
            height_ = static_cast<i32>(rgba.height());
            // H.264 needs even dimensions; refuse rather than let ffmpeg emit
            // a green half-row.
            if (setup_.format == ExportFormat::Mp4 && ((width_ | height_) & 1)) {
                error_ = "MP4 needs even width and height (got " + std::to_string(width_) + "x" +
                         std::to_string(height_) + ")";
                return;
            }
            file_ = setup_.folder /
                    io::FsPathFromUtf8(setup_.baseName +
                                       GetExportFormatInfo(setup_.format).extension);
            const std::string cmd = BuildCommand();
            Log(setup_, "piping frames to: " + cmd);
            pipe_ = WDX_POPEN(cmd.c_str(), "wb");
            if (!pipe_) {
                error_ = "could not start ffmpeg";
                return;
            }
        }
        if (static_cast<i32>(rgba.width()) != width_ || static_cast<i32>(rgba.height()) != height_)
            return;
        auto src = rgba.mipData(0);
        if (std::fwrite(src.data(), 1, src.size(), pipe_) != src.size()) {
            error_ = "ffmpeg closed the pipe early";
            return;
        }
        ++written_;
    }

    ExportSinkResult Finish() override {
        if (pipe_) {
            const int rc = WDX_PCLOSE(pipe_);
            pipe_ = nullptr;
            if (rc != 0 && error_.empty())
                error_ = "ffmpeg exited with code " + std::to_string(rc);
        }
        if (!error_.empty())
            return Failed(error_);
        if (written_ == 0)
            return Failed(kNoFrames);
        ExportSinkResult out;
        out.framesWritten = written_;
        out.files.push_back(file_);
        return out;
    }

private:
    std::string BuildCommand() const {
        // Quoted so a path with spaces survives the shell popen runs it under.
        std::string cmd = "ffmpeg -hide_banner -loglevel error -y -f rawvideo -pixel_format rgba";
        cmd += " -video_size " + std::to_string(width_) + "x" + std::to_string(height_);
        cmd += " -framerate " + std::to_string(setup_.fps) + " -i -";
        if (setup_.format == ExportFormat::Mp4) {
            // yuv420p and faststart: what every player and every browser will
            // actually open. Alpha is dropped — that is what MP4 is.
            cmd += " -c:v libx264 -pix_fmt yuv420p -crf " + std::to_string(kMp4Crf) +
                   " -preset medium -movflags +faststart";
        } else {
            // VP9 keeps the alpha channel, which is the reason to pick WebM
            // over MP4 for a transparent recording.
            cmd += " -c:v libvpx-vp9 -pix_fmt yuva420p -crf " + std::to_string(kVp9Crf) + " -b:v 0";
        }
        cmd += " \"" + io::PathToUtf8(file_) + "\"";
#if defined(_WIN32)
        // cmd.exe strips the outer pair of quotes from a command that both
        // starts and ends with one; wrapping the whole thing keeps the path
        // quotes intact.
        cmd = "\"" + cmd + "\"";
#endif
        return cmd;
    }

    ExportSinkSetup setup_;
    std::FILE* pipe_ = nullptr;
    fs::path file_;
    std::string error_;
    i32 width_ = 0, height_ = 0, written_ = 0;
};

// ---------------------------------------------------------------------------
// Auto-crop decorator
// ---------------------------------------------------------------------------

class CropSink final : public IExportSink {
public:
    CropSink(std::unique_ptr<IExportSink> inner, i32 padding)
        : inner_(std::move(inner)), padding_(std::max(0, padding)) {}

    bool Begin(const ExportSinkSetup& setup) override {
        setup_ = setup;
        return inner_->Begin(setup);
    }

    void Accept(const ExportStep& step, Texture&& frame) override {
        // The crop rect is the union over every frame — a per-frame crop makes
        // the subject jitter — so there is nothing to forward until the last
        // frame has been seen.
        Texture rgba = EnsureRgba8(std::move(frame));
        AccumulateBounds(rgba);
        steps_.push_back(step);
        frames_.push_back(std::move(rgba));
    }

    ExportSinkResult Finish() override {
        if (frames_.empty())
            return Failed(kNoFrames);
        const i32 w = static_cast<i32>(frames_[0].width());
        const i32 h = static_cast<i32>(frames_[0].height());
        i32 x0 = minX_, y0 = minY_, x1 = maxX_, y1 = maxY_;
        if (x1 < x0 || y1 < y0) {
            x0 = 0;
            y0 = 0;
            x1 = w - 1;
            y1 = h - 1; // every frame was empty; crop nothing
        }
        x0 = std::max(0, x0 - padding_);
        y0 = std::max(0, y0 - padding_);
        x1 = std::min(w - 1, x1 + padding_);
        y1 = std::min(h - 1, y1 + padding_);
        const i32 cw = x1 - x0 + 1, ch = y1 - y0 + 1;

        for (usize i = 0; i < frames_.size(); ++i) {
            Texture cropped = Texture::create2D(PixelFormat::RGBA8, static_cast<u32>(cw),
                                                static_cast<u32>(ch), 1);
            auto dst = cropped.mipData(0);
            auto src = frames_[i].mipData(0);
            for (i32 y = 0; y < ch; ++y)
                std::memcpy(dst.data() + static_cast<usize>(y) * cw * kRgbaBytes,
                            src.data() + (static_cast<usize>(y0 + y) * w + x0) * kRgbaBytes,
                            static_cast<usize>(cw) * kRgbaBytes);
            frames_[i] = Texture(); // release as we go: the peak is 1x, not 2x
            inner_->Accept(steps_[i], std::move(cropped));
        }
        frames_.clear();

        ExportSinkResult out = inner_->Finish();
        out.cropX = x0;
        out.cropY = y0;
        out.cropWidth = cw;
        out.cropHeight = ch;
        return out;
    }

private:
    void AccumulateBounds(const Texture& t) {
        const i32 w = static_cast<i32>(t.width());
        const i32 h = static_cast<i32>(t.height());
        auto px = t.mipData(0);
        if (px.size() < static_cast<usize>(w) * h * kRgbaBytes)
            return;
        for (i32 y = 0; y < h; ++y) {
            const u8* row = px.data() + static_cast<usize>(y) * w * kRgbaBytes;
            for (i32 x = 0; x < w; ++x) {
                if (row[x * kRgbaBytes + 3] == 0)
                    continue;
                minX_ = std::min(minX_, x);
                maxX_ = std::max(maxX_, x);
                minY_ = std::min(minY_, y);
                maxY_ = std::max(maxY_, y);
            }
        }
    }

    std::unique_ptr<IExportSink> inner_;
    ExportSinkSetup setup_;
    std::vector<Texture> frames_;
    std::vector<ExportStep> steps_;
    i32 padding_ = 0;
    i32 minX_ = INT32_MAX, minY_ = INT32_MAX, maxX_ = -1, maxY_ = -1;
};

// ---------------------------------------------------------------------------
// The ffmpeg probe
// ---------------------------------------------------------------------------

enum class FfmpegState : int { Unknown, Missing, Present };

std::atomic<FfmpegState> g_ffmpeg{FfmpegState::Unknown};

bool ProbeFfmpeg() {
#if defined(_WIN32)
    std::FILE* p = WDX_POPEN("ffmpeg -hide_banner -version 2>NUL", "r");
#else
    std::FILE* p = WDX_POPEN("ffmpeg -hide_banner -version 2>/dev/null", "r");
#endif
    if (!p)
        return false;
    char buf[128] = {};
    const bool got = std::fgets(buf, sizeof(buf), p) != nullptr;
    const int rc = WDX_PCLOSE(p);
    return got && rc == 0;
}

FormatAvailability VideoAvailability(bool ffmpeg) {
    if (ffmpeg)
        return {};
    return {false, "Needs ffmpeg on PATH."};
}

} // namespace

// ---------------------------------------------------------------------------

i32 ExportFrameDelayMs(i32 fps) {
    if (fps <= 0)
        return kDefaultFrameDelayMs;
    return FrameDelay(fps, kMillisecondsPerSecond);
}

f32 EffectiveFrameRate(ExportFormat format, i32 fps) {
    if (fps <= 0)
        return 0.0f;
    switch (format) {
    case ExportFormat::Gif:
        return static_cast<f32>(kCentisecondsPerSecond) /
               static_cast<f32>(FrameDelay(fps, kCentisecondsPerSecond));
    case ExportFormat::Apng:
    case ExportFormat::Webp:
        return static_cast<f32>(kMillisecondsPerSecond) / static_cast<f32>(ExportFrameDelayMs(fps));
    default:
        return static_cast<f32>(fps);
    }
}

bool FfmpegAvailable() {
    static std::once_flag probed;
    std::call_once(probed, [] {
        g_ffmpeg.store(ProbeFfmpeg() ? FfmpegState::Present : FfmpegState::Missing);
    });
    return g_ffmpeg.load() == FfmpegState::Present;
}

std::optional<bool> FfmpegProbed() {
    const FfmpegState state = g_ffmpeg.load();
    if (state == FfmpegState::Unknown)
        return std::nullopt;
    return state == FfmpegState::Present;
}

FormatAvailability ExportFormatAvailable(ExportFormat format) {
    switch (format) {
    case ExportFormat::Webp:
#if defined(WDX_HAVE_WEBP)
        return {};
#else
        return {false, "WebP support was not built into this viewer."};
#endif
    case ExportFormat::Mp4:
    case ExportFormat::WebmVp9:
        return VideoAvailability(FfmpegAvailable());
    default:
        return {};
    }
}

FormatAvailability ExportFormatAvailableNow(ExportFormat format) {
    if (format != ExportFormat::Mp4 && format != ExportFormat::WebmVp9)
        return ExportFormatAvailable(format);
    const std::optional<bool> ffmpeg = FfmpegProbed();
    if (!ffmpeg)
        return {false, "Checking for ffmpeg..."};
    return VideoAvailability(*ffmpeg);
}

std::unique_ptr<IExportSink> MakeExportSink(const ExportOutput& output) {
    std::unique_ptr<IExportSink> sink;
    switch (LayoutOf(output.format)) {
    case ExportLayout::Frames:
        sink = std::make_unique<PngFrameSink>();
        break;
    case ExportLayout::Sheet:
        sink = std::make_unique<SheetSink>();
        break;
    case ExportLayout::Video:
        sink = std::make_unique<VideoSink>();
        break;
    case ExportLayout::Animated:
        switch (output.format) {
        case ExportFormat::Gif:
            sink = std::make_unique<GifSink>();
            break;
        case ExportFormat::Apng:
            sink = std::make_unique<ApngSink>();
            break;
        default:
#if defined(WDX_HAVE_WEBP)
            sink = std::make_unique<WebpSink>();
#else
            sink = std::make_unique<UnavailableSink>(kWebpMissing);
#endif
            break;
        }
        break;
    }
    if (output.autoCrop)
        sink = std::make_unique<CropSink>(std::move(sink), output.cropPadding);
    return sink;
}

u64 EstimateSinkMemory(const ExportOutput& output, i32 frames, i32 width, i32 height) {
    if (frames <= 0 || width <= 0 || height <= 0)
        return 0;
    const u64 perFrame = static_cast<u64>(width) * height * kRgbaBytes;
    const bool buffers = output.autoCrop || output.format == ExportFormat::Gif ||
                         output.format == ExportFormat::Apng;
    if (!buffers)
        return perFrame; // one frame in flight
    // Auto-crop over a buffered encoder is still one copy: the decorator
    // releases each source frame as it hands the cropped one on.
    return perFrame * static_cast<u64>(frames);
}

} // namespace whiteout::flakes
