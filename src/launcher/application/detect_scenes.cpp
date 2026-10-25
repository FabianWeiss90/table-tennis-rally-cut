// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "launcher/application/detect_scenes.hpp"

#include "shared/kernel/parallel.hpp"

#include <algorithm>

namespace ttrally::launcher {

namespace {

constexpr int kBackgroundFrames = 15;
constexpr int kBackgroundHeight = 54; ///< about 96 x 54 for 16:9 videos
constexpr unsigned kParallelVideos = 4; ///< each decode uses several threads itself

} // namespace

Background video_background(VideoPreview& preview, const std::filesystem::path& video) {
    std::vector<media::VideoFrame> frames;
    for (int i = 0; i < kBackgroundFrames; ++i) {
        const double fraction = (i + 0.5) / kBackgroundFrames;
        if (auto frame = preview.frame_at(video, fraction, kBackgroundHeight)) {
            frames.push_back(std::move(*frame));
        }
    }
    if (frames.empty()) {
        return {};
    }
    const media::FrameSize size = frames.front().size;
    std::erase_if(frames, [&](const media::VideoFrame& frame) {
        return frame.size.width != size.width || frame.size.height != size.height;
    });
    Background background{size.width, size.height, {}};
    const auto pixels = static_cast<std::size_t>(size.width) * static_cast<std::size_t>(size.height);
    background.pixels.resize(pixels);
    std::vector<std::uint8_t> values(frames.size());
    for (std::size_t p = 0; p < pixels; ++p) {
        for (std::size_t f = 0; f < frames.size(); ++f) {
            values[f] = frames[f].planes[p]; // the luma plane comes first
        }
        const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
        std::ranges::nth_element(values, middle);
        background.pixels[p] = *middle;
    }
    return background;
}

std::vector<std::string> detect_scenes(VideoPreview& preview,
                                       const std::vector<std::filesystem::path>& videos) {
    std::vector<Background> backgrounds(videos.size());
    parallel_for(videos.size(), kParallelVideos,
                 [&](std::size_t i) { backgrounds[i] = video_background(preview, videos[i]); });
    std::vector<std::vector<double>> similarities(videos.size(),
                                                  std::vector<double>(videos.size(), 1.0));
    for (std::size_t a = 0; a < videos.size(); ++a) {
        for (std::size_t b = a + 1; b < videos.size(); ++b) {
            similarities[a][b] = similarities[b][a] = similarity(backgrounds[a], backgrounds[b]);
        }
    }
    return group_into_scenes(similarities);
}

} // namespace ttrally::launcher
