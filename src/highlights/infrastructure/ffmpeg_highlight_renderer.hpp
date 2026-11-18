// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "highlights/application/ports.hpp"

namespace ttrally::highlights {

/// Renders highlight videos with the FFmpeg libraries into an MP4 file (H.265 or H.264 video
/// with the encoders `cut` uses, AAC stereo audio at 48 kHz).
///
/// Each clip is read from the keyframe before its start and decoded in software; the output's
/// frames are taken from the clip on the output's frame grid (size and frame rate of the first
/// clip's video), so clips of videos with another frame rate or size fit in. Consecutive clips
/// overlap by the cross-fade: picture and sound of the one fade out while the next fades in.
/// Titles are drawn with stb_truetype in white with a black outline.
class FfmpegHighlightRenderer final : public HighlightRenderer {
  public:
    [[nodiscard]] cutting::CutResult render(std::span<const Clip> clips,
                                            const std::filesystem::path& output,
                                            const RenderOptions& options,
                                            ProgressReporter& progress) override;
};

} // namespace ttrally::highlights
