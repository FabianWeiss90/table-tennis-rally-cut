// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ttrally::media {

/// Output size of decoded frames (both dimensions even).
struct FrameSize {
    int width = 0;
    int height = 0;
};

/// One decoded video frame as planar YUV 4:2:0 (Y plane, then U, then V).
struct VideoFrame {
    std::int64_t index = -1; ///< Position in the sorted list of presentation timestamps
    double time_s = 0.0;     ///< Presentation time on the file's timeline
    FrameSize size;
    std::vector<std::uint8_t> planes;

    [[nodiscard]] int chroma_width() const noexcept { return size.width / 2; }
    [[nodiscard]] int chroma_height() const noexcept { return size.height / 2; }

    [[nodiscard]] std::span<const std::uint8_t> y() const noexcept {
        return {planes.data(), luma_bytes()};
    }
    [[nodiscard]] std::span<const std::uint8_t> u() const noexcept {
        return {planes.data() + luma_bytes(), chroma_bytes()};
    }
    [[nodiscard]] std::span<const std::uint8_t> v() const noexcept {
        return {planes.data() + luma_bytes() + chroma_bytes(), chroma_bytes()};
    }

    /// Bytes needed for a frame of the given size.
    [[nodiscard]] static std::size_t bytes_for(FrameSize size) noexcept {
        const auto luma =
            static_cast<std::size_t>(size.width) * static_cast<std::size_t>(size.height);
        return luma + luma / 2;
    }

  private:
    [[nodiscard]] std::size_t luma_bytes() const noexcept {
        return static_cast<std::size_t>(size.width) * static_cast<std::size_t>(size.height);
    }
    [[nodiscard]] std::size_t chroma_bytes() const noexcept {
        return static_cast<std::size_t>(chroma_width()) * static_cast<std::size_t>(chroma_height());
    }
};

} // namespace ttrally::media
