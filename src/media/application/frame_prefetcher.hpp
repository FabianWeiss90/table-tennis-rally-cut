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
/// bounded cache, so that stepping and scrubbing stay responsive. When the cache is full, the
/// frames farthest from the latest requested position are dropped.
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
