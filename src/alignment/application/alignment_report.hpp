// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "alignment/domain/alignment_settings.hpp"
#include "alignment/domain/candidates.hpp"
#include "alignment/domain/visual_verification.hpp"
#include "media/domain/decode_backend.hpp"
#include "media/domain/media_info.hpp"
#include "shared/kernel/rational.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ttrally::alignment {

/// One alignment window, for plotting the offset over time.
struct WindowPoint {
    double cut_time_s = 0.0; ///< Window centre on the cut's timeline
    double offset_s = 0.0;   ///< Original time minus cut time
    double confidence = 0.0;
    bool confident = false;
};

struct AlignmentStatistics {
    std::size_t window_count = 0;
    std::size_t confident_windows = 0;
    std::size_t global_searches = 0;
};

/// Everything the `align` use case found out; read model for the CSV and HTML outputs.
struct AlignmentReport {
    std::string tool_version;
    media::MediaInfo original;
    media::MediaInfo cut;
    Rational original_fps;
    bool original_constant_frame_rate = true;
    std::int64_t original_frame_count = 0;
    AlignmentSettings settings;
    AlignmentStatistics statistics;
    std::optional<VisualCheckPolicy> visual_check; ///< Set if the spot check was run
    std::optional<media::DecodeBackend> original_decode_backend;
    std::optional<media::DecodeBackend> cut_decode_backend;
    std::vector<CandidateSegment> candidates;
    std::vector<Gap> gaps;
    std::vector<WindowPoint> windows;
    std::vector<std::string> warnings;
};

} // namespace ttrally::alignment
