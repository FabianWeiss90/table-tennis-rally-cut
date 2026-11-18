// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

// Finding bit rates at which a hardware encoder works. h264_vaapi on Mesa (RX 7800 XT, Mesa
// 26.2) misreads the stream headers FFmpeg passes it at some bit rates: it then hangs (e.g. 2, 3,
// 4 and 6 Mbit/s for Full HD at 59.94 fps) or writes broken pictures (7, 7.5, 7.95 Mbit/s), with
// no error reported. Which bit rates fail cannot be predicted, only tried.

#pragma once

#include "cutting/infrastructure/ffmpeg_encoding.hpp"
#include "shared/application/progress_reporter.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ttrally::cutting {

enum class ProbeOutcome {
    Works,
    Broken,     ///< the decoded test pictures differ from what was encoded
    Hangs,      ///< no result in time
    Unavailable ///< the encoder cannot be opened
};

/// Encodes a short moving test pattern in `format` with the encoder at the bit rate, decodes it
/// with FFmpeg's decoder and compares the pictures. A hanging driver cannot be interrupted: its
/// thread is left behind (it keeps one CPU core busy until the program ends).
[[nodiscard]] ProbeOutcome probe_encoder(const std::string& encoder, const VideoFormat& format,
                                         std::int64_t bitrate);

/// Bit rates to try for a wanted one: itself, then its neighbours in 0.5 Mbit/s steps, the
/// higher one first (at least 1 Mbit/s, at most `max`).
[[nodiscard]] std::vector<std::int64_t> bitrate_candidates(std::int64_t wanted, std::int64_t max);

/// Bit rates found not to work, per encoder and video format, kept in a file between runs so
/// that a hanging one is never tried again.
class EncoderFailures {
  public:
    explicit EncoderFailures(std::filesystem::path file) : file_(std::move(file)) {}

    [[nodiscard]] std::optional<ProbeOutcome> known(const std::string& key) const;
    void add(const std::string& key, ProbeOutcome outcome);

    /// The key of an encoder, format and bit rate.
    [[nodiscard]] static std::string key(const std::string& encoder, const VideoFormat& format,
                                         std::int64_t bitrate);

  private:
    std::filesystem::path file_;
};

/// $XDG_CACHE_HOME/ttrally (else ~/.cache/ttrally) on Linux, %LOCALAPPDATA%\ttrally on Windows.
[[nodiscard]] std::filesystem::path default_encoder_failures_file();

/// The bit rate nearest `wanted` (see bitrate_candidates) at which the encoder works, skipping
/// those known to fail and recording new failures; each skipped one is reported. nullopt if
/// none of them works.
[[nodiscard]] std::optional<std::int64_t> working_bitrate(const std::string& encoder,
                                                          const VideoFormat& format,
                                                          std::int64_t wanted,
                                                          std::int64_t max,
                                                          EncoderFailures& failures,
                                                          ProgressReporter& progress);

} // namespace ttrally::cutting
