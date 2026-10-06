// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "alignment/domain/boundary_refinement.hpp"

#include <algorithm>
#include <cmath>

namespace ttrally::alignment {

namespace {

constexpr double kMinEnergy = 1e-12;

/// Local similarity max(0, cos) between cut[t] and original[t + offset] for t in [lo, hi), each
/// computed over [t - half_window, t + half_window].
std::vector<float> local_similarity(std::span<const float> cut, std::span<const float> original,
                                    std::size_t lo, std::size_t hi, std::ptrdiff_t offset,
                                    std::size_t half_window) {
    const std::size_t ext_lo = lo >= half_window ? lo - half_window : 0;
    const std::size_t ext_hi = std::min(cut.size(), hi + half_window + 1);
    const std::size_t n = ext_hi - ext_lo;

    // Prefix sums of x*a, x*x and a*a over the extended range
    std::vector<double> xa(n + 1, 0.0);
    std::vector<double> xx(n + 1, 0.0);
    std::vector<double> aa(n + 1, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t t = ext_lo + i;
        const double x = cut[t];
        const std::ptrdiff_t source = static_cast<std::ptrdiff_t>(t) + offset;
        const bool inside = source >= 0 && source < static_cast<std::ptrdiff_t>(original.size());
        const double a = inside ? original[static_cast<std::size_t>(source)] : 0.0;
        xa[i + 1] = xa[i] + x * a;
        xx[i + 1] = xx[i] + x * x;
        aa[i + 1] = aa[i] + a * a;
    }

    std::vector<float> similarity(hi - lo, 0.0F);
    for (std::size_t t = lo; t < hi; ++t) {
        const std::size_t w_lo = std::max(ext_lo, t >= half_window ? t - half_window : 0) - ext_lo;
        const std::size_t w_hi = std::min(ext_hi, t + half_window + 1) - ext_lo;
        const double cross = xa[w_hi] - xa[w_lo];
        const double energy = (xx[w_hi] - xx[w_lo]) * (aa[w_hi] - aa[w_lo]);
        if (energy > kMinEnergy) {
            similarity[t - lo] = static_cast<float>(std::max(0.0, cross / std::sqrt(energy)));
        }
    }
    return similarity;
}

/// Window geometry and parameters shared by all boundary searches of one alignment.
struct BoundarySearch {
    std::span<const float> cut;
    std::span<const float> original;
    std::size_t half_window;
    float threshold;

    [[nodiscard]] std::size_t locate(std::size_t lo, std::size_t hi,
                                     std::optional<std::ptrdiff_t> before,
                                     std::optional<std::ptrdiff_t> after) const {
        return locate_boundary(cut, original, lo, hi, before, after, half_window, threshold);
    }
};

} // namespace

std::size_t locate_boundary(std::span<const float> cut, std::span<const float> original,
                            std::size_t lo, std::size_t hi,
                            std::optional<std::ptrdiff_t> before_offset,
                            std::optional<std::ptrdiff_t> after_offset, std::size_t half_window,
                            float threshold) {
    hi = std::min(hi, cut.size());
    if (lo >= hi) {
        return hi;
    }
    const std::size_t n = hi - lo;
    const std::vector<float> unmatched(n, threshold);
    const std::vector<float> before =
        before_offset ? local_similarity(cut, original, lo, hi, *before_offset, half_window)
                      : unmatched;
    const std::vector<float> after =
        after_offset ? local_similarity(cut, original, lo, hi, *after_offset, half_window)
                     : unmatched;

    // Maximise P(k) = sum_{t < lo + k} (before - after). The total is constant, so this also
    // maximises the advantage of "after" right of the boundary.
    double prefix = 0.0;
    double best = 0.0;
    std::size_t best_k = 0;
    for (std::size_t k = 0; k < n; ++k) {
        prefix += static_cast<double>(before[k]) - static_cast<double>(after[k]);
        if (prefix > best) {
            best = prefix;
            best_k = k + 1;
        }
    }
    return lo + best_k;
}

void refine_segment_boundaries(std::vector<MatchedSegment>& segments,
                               const std::vector<WindowMatch>& windows,
                               std::span<const float> original, std::span<const float> cut,
                               const AlignmentSettings& settings) {
    const BoundarySearch search{
        cut, original, std::max<std::size_t>(1, settings.samples(settings.refine_half_window_s)),
        settings.refine_threshold};
    const std::size_t window_length = settings.window_samples();
    const std::size_t max_gap = settings.samples(settings.max_two_sided_gap_s);
    // A boundary lies at most about one window plus one hop outside the confident windows.
    const std::size_t reach = window_length + settings.hop_samples();

    for (std::size_t k = 0; k < segments.size(); ++k) {
        MatchedSegment& segment = segments[k];
        const std::size_t first_start = windows[segment.first_window].cut_index;
        const std::size_t first_end = first_start + window_length;
        const std::size_t earliest_start = first_start - std::min(first_start, reach);

        if (k == 0) {
            segment.cut_start =
                search.locate(earliest_start, first_end, std::nullopt, segment.offset);
        } else {
            MatchedSegment& previous = segments[k - 1];
            const std::size_t previous_start = windows[previous.last_window].cut_index;
            const std::size_t previous_end = previous_start + window_length;
            if (first_start <= previous_end + max_gap) {
                // Direct cut from the previous segment to this one
                const std::size_t boundary = search.locate(previous_start, first_end,
                                                           previous.offset, segment.offset);
                previous.cut_end = boundary;
                segment.cut_start = boundary;
            } else {
                // Unmatched material in between: locate both ends separately
                previous.cut_end =
                    search.locate(previous_start, std::min(previous_end + reach, first_start),
                                  previous.offset, std::nullopt);
                segment.cut_start = search.locate(std::max(previous.cut_end, earliest_start),
                                                  first_end, std::nullopt, segment.offset);
            }
        }

        if (k + 1 == segments.size()) {
            const std::size_t last_start = windows[segment.last_window].cut_index;
            const std::size_t latest_end = std::min(cut.size(), last_start + window_length + reach);
            segment.cut_end = search.locate(last_start, latest_end, segment.offset, std::nullopt);
        }
    }
}

} // namespace ttrally::alignment
