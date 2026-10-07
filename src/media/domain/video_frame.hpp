// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ttrally::media {

/// Size of decoded frames in pixels.
struct FrameSize {
    int width = 0;
    int height = 0;
};

/// Memory layout of decoded frames.
enum class PixelLayout {
    Yuv420, ///< Planar Y, U, V with half-resolution chroma (for display; even sizes)
    Rgb24,  ///< Interleaved R, G, B, one byte each (for image models)
};

/// What a frame decoder should produce.
struct FrameOutput {
    int height = 0;
    int width = 0; ///< 0: derived from the height, keeping the aspect ratio
    PixelLayout layout = PixelLayout::Yuv420;
};

/// One decoded video frame.
struct VideoFrame {
    std::int64_t index = -1; ///< Position in the sorted list of presentation timestamps
    double time_s = 0.0;     ///< Presentation time on the file's timeline
    FrameSize size;
    PixelLayout layout = PixelLayout::Yuv420;
    std::vector<std::uint8_t> planes;

    [[nodiscard]] int chroma_width() const noexcept { return size.width / 2; }
    [[nodiscard]] int chroma_height() const noexcept { return size.height / 2; }

    // Planes of a Yuv420 frame
    [[nodiscard]] std::span<const std::uint8_t> y() const noexcept {
        return {planes.data(), pixel_count()};
    }
    [[nodiscard]] std::span<const std::uint8_t> u() const noexcept {
        return {planes.data() + pixel_count(), chroma_bytes()};
    }
    [[nodiscard]] std::span<const std::uint8_t> v() const noexcept {
        return {planes.data() + pixel_count() + chroma_bytes(), chroma_bytes()};
    }

    /// Pixels of an Rgb24 frame, row by row.
    [[nodiscard]] std::span<const std::uint8_t> rgb() const noexcept {
        return {planes.data(), planes.size()};
    }

    /// Bytes needed for a frame of the given size and layout.
    [[nodiscard]] static std::size_t bytes_for(FrameSize size,
                                               PixelLayout layout = PixelLayout::Yuv420) noexcept {
        const auto pixels =
            static_cast<std::size_t>(size.width) * static_cast<std::size_t>(size.height);
        return layout == PixelLayout::Rgb24 ? 3 * pixels : pixels + pixels / 2;
    }

  private:
    [[nodiscard]] std::size_t pixel_count() const noexcept {
        return static_cast<std::size_t>(size.width) * static_cast<std::size_t>(size.height);
    }
    [[nodiscard]] std::size_t chroma_bytes() const noexcept {
        return static_cast<std::size_t>(chroma_width()) * static_cast<std::size_t>(chroma_height());
    }
};

} // namespace ttrally::media
