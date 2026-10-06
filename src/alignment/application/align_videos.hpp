// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "alignment/application/alignment_report.hpp"
#include "alignment/domain/alignment_settings.hpp"
#include "alignment/domain/signal_alignment.hpp"
#include "alignment/domain/signal_matcher.hpp"
#include "alignment/domain/visual_verification.hpp"
#include "media/application/frame_sampler.hpp"
#include "media/application/media_probe.hpp"
#include "media/application/media_reader.hpp"
#include "shared/application/progress_reporter.hpp"

#include <filesystem>

namespace ttrally::alignment {

struct AlignVideosRequest {
    std::filesystem::path original; ///< Untrimmed original video
    std::filesystem::path cut;      ///< Video cut from the original (e.g. by Liimba)
    AlignmentSettings settings;
    bool visual_check = true;
    VisualCheckPolicy visual_policy;
    media::DecodeBackend decode_backend = media::DecodeBackend::Auto;
};

/// Use case behind `ttrally align`: locates every segment of a cut video in its original via
/// the audio track and expresses it as times and frame indices of the original.
///
/// Throws AlignmentNotPossible if a file lacks the required streams or the audio does not match,
/// media::MediaError if a file cannot be read.
class AlignVideos {
  public:
    AlignVideos(media::MediaProbe& probe, media::MediaReader& reader,
                media::FrameSamplerFactory& frame_samplers, const SignalMatcher& matcher,
                ProgressReporter& progress);

    [[nodiscard]] AlignmentReport execute(const AlignVideosRequest& request);

  private:
    struct Inputs;

    [[nodiscard]] Inputs load_inputs(const AlignVideosRequest& request);
    [[nodiscard]] SignalAlignment align_audio(const Inputs& inputs,
                                              const AlignmentSettings& settings);
    void verify_visually(AlignmentReport& report, const AlignVideosRequest& request);

    media::MediaProbe& probe_;
    media::MediaReader& reader_;
    media::FrameSamplerFactory& frame_samplers_;
    const SignalMatcher& matcher_;
    ProgressReporter& progress_;
};

} // namespace ttrally::alignment
