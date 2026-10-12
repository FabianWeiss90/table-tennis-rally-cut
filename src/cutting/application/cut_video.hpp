// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "annotation/application/ports.hpp"
#include "cutting/application/video_cutter.hpp"
#include "media/application/media_probe.hpp"
#include "media/application/media_reader.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace ttrally::cutting {

struct CutVideoRequest {
    std::filesystem::path video;
    std::string video_id;           ///< Id of the rallies in the cut list
    std::filesystem::path output;
    Margins margins;
    CutOptions options;
};

struct CutVideoResult {
    std::size_t rallies = 0;
    std::vector<CutSection> sections;
    CutResult cut;
    double seconds = 0.0; ///< Run time
};

/// Use case behind `ttrally cut`: takes the rallies of a cut list (detected or annotated, in the
/// label format; ignored sections are not rallies), adds the margins before and after each,
/// merges what overlaps and writes the result as one video.
class CutVideo {
  public:
    CutVideo(media::MediaProbe& probe, media::MediaReader& reader,
             annotation::AnnotationRepository& cut_list, VideoCutter& cutter,
             ProgressReporter& progress);

    /// Throws media::MediaError, std::invalid_argument (no rallies, frames outside the video,
    /// negative margins) or std::runtime_error.
    [[nodiscard]] CutVideoResult execute(const CutVideoRequest& request);

  private:
    media::MediaProbe& probe_;
    media::MediaReader& reader_;
    annotation::AnnotationRepository& cut_list_;
    VideoCutter& cutter_;
    ProgressReporter& progress_;
};

} // namespace ttrally::cutting
