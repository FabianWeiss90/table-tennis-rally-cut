// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

// Writing MP4 files with the FFmpeg libraries: the output file, the video encoder (GPU first)
// and the AAC audio encoder. Internal to the infrastructure that writes videos (cut,
// highlights); includes the FFmpeg headers.

#pragma once

#include "cutting/application/video_cutter.hpp"
#include "media/infrastructure/ffmpeg_api.hpp"

extern "C" {
#include <libavutil/audio_fifo.h>
}

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace ttrally::cutting {

struct FifoFreer {
    void operator()(AVAudioFifo* fifo) const noexcept { av_audio_fifo_free(fifo); }
};
using FifoPtr = std::unique_ptr<AVAudioFifo, FifoFreer>;

struct OutputCloser {
    void operator()(AVFormatContext* context) const noexcept;
};
using OutputPtr = std::unique_ptr<AVFormatContext, OutputCloser>;

/// The MP4 file being written: created first, its header written once all streams exist.
class Output {
  public:
    explicit Output(const std::filesystem::path& path);

    [[nodiscard]] AVStream* add_stream();
    /// Creates the folder and writes the header (moov first: playable while downloading).
    void open();
    void write(AVPacket& packet);
    void finish();
    [[nodiscard]] AVFormatContext* context() const noexcept { return context_.get(); }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
    OutputPtr context_;
};

/// Size, frame rate and colours of the video to encode.
struct VideoFormat {
    int width = 0;
    int height = 0;
    AVRational sample_aspect_ratio{0, 1};
    AVRational frame_rate{0, 1};
    bool full_range = false;
    AVColorSpace colorspace = AVCOL_SPC_UNSPECIFIED;
    AVColorPrimaries color_primaries = AVCOL_PRI_UNSPECIFIED;
    AVColorTransferCharacteristic color_trc = AVCOL_TRC_UNSPECIFIED;
};

/// The format of a decoded stream (camera footage is often full range BT.709).
[[nodiscard]] VideoFormat video_format_of(const AVCodecContext& decoder, const AVStream& stream);

class EncoderWatchdog;

class VideoEncoder {
  public:
    /// The encoder, or nullopt if it is missing or cannot be opened on this machine.
    static std::optional<VideoEncoder> open(const std::string& name, const VideoFormat& format,
                                            std::int64_t bitrate);

    [[nodiscard]] AVCodecContext* context() const noexcept { return context_.get(); }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }

    /// Converts a frame (any software pixel format, the encoder's size) to what the encoder
    /// takes and sends it with the given pts (in the encoder's time base: 1 / frame rate).
    void send(const AVFrame& frame, std::int64_t pts);
    /// Writes the packets the encoder has ready; with flush, also the delayed ones.
    void drain(AVStream* stream, Output& output, bool flush);

    /// From now on a call into the encoder (send, drain) that does not return within
    /// kEncoderHangTimeout means the driver hangs (h264_vaapi does at some bit rates); the
    /// process then prints `advice` to stderr, removes the unfinished `output` and exits with
    /// kEncoderHangExitCode, since the hanging call can neither be cancelled nor recovered from.
    void exit_on_hang(std::string advice, std::filesystem::path output);

  private:
    bool create_hardware_frames(AVHWDeviceType type, AVPixelFormat hardware_format);
    media::ff::FramePtr convert(const AVFrame& decoded);

    std::string name_;
    media::ff::CodecPtr context_;
    media::ff::BufferPtr device_;
    media::ff::BufferPtr frames_; ///< GPU frame pool for hardware encoders that need uploads
    AVPixelFormat software_format_ = AV_PIX_FMT_YUV420P;
    media::ff::SwsPtr scaler_;
    std::shared_ptr<EncoderWatchdog> watchdog_; ///< shared: the encoder is movable
};

/// Exit code of a process whose video encoder hung (see VideoEncoder::exit_on_hang).
inline constexpr int kEncoderHangExitCode = 3;

/// Opens the requested encoder, or with "auto" the first of the codec that works (GPU first).
/// The bit rate is the requested one, else chosen by resolution and frame rate. If the encoder
/// hangs later, the process ends with advice for another bit rate and `output` is removed (see
/// VideoEncoder::exit_on_hang). Throws media::MediaError if none opens.
[[nodiscard]] VideoEncoder open_video_encoder(const CutOptions& options, const VideoFormat& format,
                                              const std::filesystem::path& output,
                                              ProgressReporter& progress);

/// AAC audio, fed through a FIFO because the encoder takes fixed-size frames.
class AudioEncoder {
  public:
    /// Encodes at the given rate and channel layout; frames passed to append() have the sample
    /// format `input_format` (converted if it is not planar float).
    AudioEncoder(int sample_rate, const AVChannelLayout& layout, AVSampleFormat input_format);

    [[nodiscard]] AVCodecContext* context() const noexcept { return context_.get(); }
    [[nodiscard]] int channels() const noexcept { return context_->ch_layout.nb_channels; }

    /// Appends samples [first, first + count) of a decoded frame.
    void append(const AVFrame& frame, int first, int count);
    /// Appends `count` samples of planar float audio, one pointer per channel.
    void append_planar(const float* const* planes, int count);
    /// Sends complete encoder frames from the FIFO (and the rest at the end).
    void send_ready(bool final);

  private:
    media::ff::CodecPtr context_;
    media::ff::SwrPtr resampler_;
    FifoPtr fifo_;
    std::int64_t samples_sent_ = 0;
};

/// Writes all packets an encoder has ready; with flush, also the delayed ones.
void drain(AVCodecContext* encoder, AVStream* stream, Output& output, bool flush);

/// Adds a stream for an encoder's packets to the output.
[[nodiscard]] AVStream* add_encoded_stream(Output& output, const AVCodecContext& encoder);

} // namespace ttrally::cutting
