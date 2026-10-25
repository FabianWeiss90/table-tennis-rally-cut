// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "media/application/frame_decoder.hpp"

#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace ttrally::media {

/// Decodes frames around a requested position on a background thread and keeps them in a
/// bounded cache, so that stepping and scrubbing stay responsive in both directions. Frames at
/// and after the position come first, then the ones before it, nearest first. Every pass keeps
/// the frames decoded on the way from the keyframe, so going back costs one pass per keyframe
/// interval, and right after a jump the frames before the target are mostly there already. A
/// newer request lets a running pass finish if it still needs it, so stepping does not restart
/// passes. When the cache is full, the frames farthest from the latest requested position are
/// dropped.
class FramePrefetcher {
  public:
    FramePrefetcher(std::unique_ptr<FrameDecoder> decoder, std::size_t capacity);
    ~FramePrefetcher();
    FramePrefetcher(const FramePrefetcher&) = delete;
    FramePrefetcher& operator=(const FramePrefetcher&) = delete;

    /// Asks for the frames [index - behind, index + ahead]. Returns immediately; a newer request
    /// replaces an older one.
    void request(std::int64_t index, std::int64_t behind, std::int64_t ahead);

    /// The frame if it is already decoded, otherwise nullptr.
    [[nodiscard]] std::shared_ptr<const VideoFrame> frame(std::int64_t index) const;

    /// Blocks until the latest request is fulfilled (for tests and scripted use).
    void wait_until_idle() const;

    /// Error message if decoding failed.
    [[nodiscard]] std::optional<std::string> error() const;

    [[nodiscard]] std::int64_t frame_count() const { return frame_count_; }
    [[nodiscard]] FrameSize frame_size() const { return frame_size_; }
    [[nodiscard]] DecodeBackend backend() const { return backend_; }

  private:
    struct Window {
        std::int64_t first = 0;
        std::int64_t last = -1;
        std::int64_t centre = 0;
    };

    void run();
    [[nodiscard]] std::optional<std::int64_t> first_missing(const Window& window) const;
    /// Whether a running pass for `missing` should go on after a newer request.
    [[nodiscard]] bool still_useful(std::int64_t missing) const;
    /// Last frame to decode for a missing one: up to the next cached frame or the window's end.
    [[nodiscard]] std::int64_t decode_end(const Window& window, std::int64_t missing) const;
    void store(VideoFrame&& frame, std::int64_t centre);

    std::unique_ptr<FrameDecoder> decoder_;
    const std::size_t capacity_;
    const std::int64_t frame_count_;
    const FrameSize frame_size_;
    const DecodeBackend backend_;

    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    std::map<std::int64_t, std::shared_ptr<const VideoFrame>> cache_;
    Window window_;
    std::uint64_t requested_ = 0; ///< Generation of the latest request
    std::uint64_t fulfilled_ = 0; ///< Generation of the latest fulfilled request
    bool stopping_ = false;
    std::optional<std::string> error_;
    std::thread worker_;
};

} // namespace ttrally::media
