// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "alignment/infrastructure/pocketfft_signal_matcher.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <numeric>
#include <optional>
#include <vector>

// Cache FFT plans: the same sizes are used for every window.
#define POCKETFFT_CACHE_SIZE 16
#include <pocketfft_hdronly.h>

namespace ttrally::alignment {

namespace {

constexpr float kSecondPeakFloor = 0.01F;
constexpr double kMinEnergyPerSample = 1e-12;

using Spectrum = std::vector<std::complex<float>>;

std::size_t next_power_of_two(std::size_t n) {
    std::size_t power = 1;
    while (power < n) {
        power <<= 1U;
    }
    return power;
}

void forward_fft(const std::vector<float>& input, Spectrum& output) {
    const pocketfft::shape_t shape{input.size()};
    const pocketfft::stride_t stride_in{sizeof(float)};
    const pocketfft::stride_t stride_out{sizeof(std::complex<float>)};
    pocketfft::r2c(shape, stride_in, stride_out, 0, pocketfft::FORWARD, input.data(),
                   output.data(), 1.0F, 1);
}

void inverse_fft(const Spectrum& input, std::vector<float>& output) {
    const pocketfft::shape_t shape{output.size()};
    const pocketfft::stride_t stride_in{sizeof(std::complex<float>)};
    const pocketfft::stride_t stride_out{sizeof(float)};
    pocketfft::c2r(shape, stride_in, stride_out, 0, pocketfft::BACKWARD, input.data(),
                   output.data(), 1.0F / static_cast<float>(output.size()), 1);
}

/// The needle with its mean removed, transformed once for all haystack blocks. With a zero-mean
/// needle the correlation numerator equals the covariance with each haystack window.
struct PreparedNeedle {
    Spectrum conjugate_spectrum;
    double norm = 0.0;
    std::size_t length = 0;
};

std::optional<PreparedNeedle> prepare_needle(std::span<const float> needle, std::size_t n_fft) {
    const std::size_t m = needle.size();
    const double mean = std::accumulate(needle.begin(), needle.end(), 0.0) / static_cast<double>(m);
    std::vector<float> buffer(n_fft, 0.0F);
    double energy = 0.0;
    for (std::size_t i = 0; i < m; ++i) {
        const double centred = needle[i] - mean;
        buffer[i] = static_cast<float>(centred);
        energy += centred * centred;
    }
    if (energy <= kMinEnergyPerSample * static_cast<double>(m)) {
        return std::nullopt; // silence cannot be matched
    }
    PreparedNeedle prepared;
    prepared.conjugate_spectrum.resize(n_fft / 2 + 1);
    forward_fft(buffer, prepared.conjugate_spectrum);
    for (auto& value : prepared.conjugate_spectrum) {
        value = std::conj(value);
    }
    prepared.norm = std::sqrt(energy);
    prepared.length = m;
    return prepared;
}

/// Running sum and sum of squares of a haystack window, for its standard deviation.
class WindowEnergy {
  public:
    WindowEnergy(std::span<const float> haystack, std::size_t start, std::size_t length)
        : haystack_(haystack), length_(length) {
        for (std::size_t i = start; i < start + length; ++i) {
            add(haystack[i]);
        }
    }

    /// Moves the window one sample to the right; `start` is the new first sample.
    void slide_to(std::size_t start) {
        remove(haystack_[start - 1]);
        add(haystack_[start + length_ - 1]);
    }

    /// sqrt(sum((x - mean)^2)), or 0 for (near) silence.
    [[nodiscard]] double centred_norm() const {
        const double variance = sum_sq_ - sum_ * sum_ / static_cast<double>(length_);
        return variance > kMinEnergyPerSample * static_cast<double>(length_) ? std::sqrt(variance)
                                                                            : 0.0;
    }

  private:
    void add(double x) {
        sum_ += x;
        sum_sq_ += x * x;
    }
    void remove(double x) {
        sum_ -= x;
        sum_sq_ -= x * x;
    }

    std::span<const float> haystack_;
    std::size_t length_;
    double sum_ = 0.0;
    double sum_sq_ = 0.0;
};

/// Tracks the best correlation and the best value per bucket of `bucket_size` lags; the second
/// peak is the best bucket not adjacent to the bucket of the main peak, which excludes at least
/// `bucket_size` lags on each side.
class PeakTracker {
  public:
    PeakTracker(std::size_t lag_lo, std::size_t lag_count, std::size_t bucket_size)
        : lag_lo_(lag_lo), bucket_size_(std::max<std::size_t>(1, bucket_size)),
          buckets_((lag_count + bucket_size_ - 1) / bucket_size_,
                   -std::numeric_limits<float>::infinity()) {}

    void add(std::size_t lag, float value) {
        if (value > best_) {
            best_ = value;
            best_lag_ = lag;
        }
        float& bucket = buckets_[(lag - lag_lo_) / bucket_size_];
        bucket = std::max(bucket, value);
    }

    [[nodiscard]] MatchResult result() const {
        const std::size_t peak_bucket = (best_lag_ - lag_lo_) / bucket_size_;
        float second = 0.0F;
        for (std::size_t b = 0; b < buckets_.size(); ++b) {
            const std::size_t distance = b > peak_bucket ? b - peak_bucket : peak_bucket - b;
            if (distance > 1) {
                second = std::max(second, buckets_[b]);
            }
        }
        MatchResult match;
        match.valid = true;
        match.lag = best_lag_;
        match.peak = best_;
        match.second_peak = second;
        match.confidence = best_ / std::max(second, kSecondPeakFloor);
        return match;
    }

  private:
    std::size_t lag_lo_;
    std::size_t bucket_size_;
    std::vector<float> buckets_;
    float best_ = -std::numeric_limits<float>::infinity();
    std::size_t best_lag_ = 0;
};

} // namespace

MatchResult PocketFftSignalMatcher::find_best_match(std::span<const float> needle,
                                                    std::span<const float> haystack,
                                                    std::size_t lag_lo, std::size_t lag_hi,
                                                    std::size_t exclusion_radius) const {
    const std::size_t m = needle.size();
    if (m == 0 || haystack.size() < m) {
        return {};
    }
    lag_hi = std::min(lag_hi, haystack.size() - m);
    if (lag_lo > lag_hi) {
        return {};
    }
    const std::size_t n_fft = std::max(next_power_of_two(fft_size_), next_power_of_two(2 * m));
    const auto prepared = prepare_needle(needle, n_fft);
    if (!prepared) {
        return {};
    }

    PeakTracker peaks(lag_lo, lag_hi - lag_lo + 1, exclusion_radius);
    std::vector<float> block(n_fft);
    Spectrum spectrum(n_fft / 2 + 1);
    std::vector<float> correlation(n_fft);

    // Overlap-save: each block of n_fft haystack samples yields n_fft - m + 1 valid lags.
    const std::size_t step = n_fft - m + 1;
    for (std::size_t block_start = lag_lo; block_start <= lag_hi; block_start += step) {
        const std::size_t available = std::min(n_fft, haystack.size() - block_start);
        const auto first = haystack.begin() + static_cast<std::ptrdiff_t>(block_start);
        std::copy_n(first, available, block.begin());
        std::fill(block.begin() + static_cast<std::ptrdiff_t>(available), block.end(), 0.0F);
        forward_fft(block, spectrum);
        for (std::size_t k = 0; k < spectrum.size(); ++k) {
            spectrum[k] *= prepared->conjugate_spectrum[k];
        }
        inverse_fft(spectrum, correlation);

        WindowEnergy energy(haystack, block_start, m); // recomputed per block against drift
        const std::size_t lags = std::min(step, lag_hi - block_start + 1);
        for (std::size_t k = 0; k < lags; ++k) {
            const std::size_t lag = block_start + k;
            if (k > 0) {
                energy.slide_to(lag);
            }
            const double norm = energy.centred_norm();
            const float value =
                norm > 0.0 ? static_cast<float>(correlation[k] / (prepared->norm * norm)) : 0.0F;
            peaks.add(lag, value);
        }
    }
    return peaks.result();
}

} // namespace ttrally::alignment
