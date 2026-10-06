// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "alignment/domain/segmentation.hpp"

#include <algorithm>
#include <cstdlib>
#include <format>

namespace ttrally::alignment {

namespace {

template <typename T> T median(std::vector<T> values) {
    const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
    std::nth_element(values.begin(), middle, values.end());
    return *middle;
}

/// Collects the windows of the segment that is currently being built.
class SegmentBuilder {
  public:
    void start(std::size_t window_index) {
        segment_ = MatchedSegment{};
        segment_.first_window = window_index;
        offsets_.clear();
        confidences_.clear();
    }

    void add(std::size_t window_index, const WindowMatch& window) {
        segment_.last_window = window_index;
        ++segment_.window_count;
        offsets_.push_back(window.offset);
        confidences_.push_back(window.confidence);
    }

    [[nodiscard]] bool empty() const { return offsets_.empty(); }
    [[nodiscard]] std::ptrdiff_t last_offset() const { return offsets_.back(); }

    [[nodiscard]] MatchedSegment build() const {
        MatchedSegment segment = segment_;
        segment.offset = median(offsets_);
        segment.confidence = median(confidences_);
        return segment;
    }

  private:
    MatchedSegment segment_;
    std::vector<std::ptrdiff_t> offsets_;
    std::vector<float> confidences_;
};

} // namespace

std::vector<MatchedSegment> group_into_segments(const std::vector<WindowMatch>& windows,
                                                std::size_t offset_tolerance) {
    std::vector<MatchedSegment> segments;
    SegmentBuilder builder;
    for (std::size_t i = 0; i < windows.size(); ++i) {
        const WindowMatch& window = windows[i];
        if (!window.confident) {
            continue;
        }
        const bool jump = !builder.empty() &&
                          static_cast<std::size_t>(std::abs(window.offset -
                                                            builder.last_offset())) >
                              offset_tolerance;
        if (jump) {
            segments.push_back(builder.build());
        }
        if (builder.empty() || jump) {
            builder.start(i);
        }
        builder.add(i, window);
    }
    if (!builder.empty()) {
        segments.push_back(builder.build());
    }
    return segments;
}

namespace {

bool is_weaker(const MatchedSegment& a, const MatchedSegment& b) {
    if (a.window_count != b.window_count) {
        return a.window_count < b.window_count;
    }
    return a.confidence < b.confidence;
}

/// Index of the first segment whose offset is smaller than its predecessor's, or size().
std::size_t first_order_conflict(const std::vector<MatchedSegment>& segments,
                                 std::size_t offset_tolerance) {
    const auto tolerance = static_cast<std::ptrdiff_t>(offset_tolerance);
    for (std::size_t k = 1; k < segments.size(); ++k) {
        if (segments[k].offset + tolerance < segments[k - 1].offset) {
            return k;
        }
    }
    return segments.size();
}

} // namespace

std::vector<std::string> remove_out_of_order_segments(std::vector<MatchedSegment>& segments,
                                                      const std::vector<WindowMatch>& windows,
                                                      std::size_t offset_tolerance,
                                                      int sample_rate) {
    std::vector<std::string> warnings;
    for (std::size_t k = first_order_conflict(segments, offset_tolerance); k < segments.size();
         k = first_order_conflict(segments, offset_tolerance)) {
        const std::size_t weaker = is_weaker(segments[k], segments[k - 1]) ? k : k - 1;
        const MatchedSegment& removed = segments[weaker];
        const auto rate = static_cast<double>(sample_rate);
        warnings.push_back(std::format(
            "Discarded an ambiguous match in the cut at {:.1f} s ({} window(s), confidence "
            "{:.1f}): it would lie before the neighbouring segment in the original.",
            static_cast<double>(windows[removed.first_window].cut_index) / rate,
            removed.window_count, removed.confidence));
        segments.erase(segments.begin() + static_cast<std::ptrdiff_t>(weaker));
    }
    return warnings;
}

} // namespace ttrally::alignment
