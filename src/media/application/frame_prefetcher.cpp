// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "media/application/frame_prefetcher.hpp"

#include <algorithm>
#include <cstdlib>
#include <exception>

namespace ttrally::media {

FramePrefetcher::FramePrefetcher(std::unique_ptr<FrameDecoder> decoder, std::size_t capacity)
    : decoder_(std::move(decoder)), capacity_(std::max<std::size_t>(1, capacity)),
      frame_count_(decoder_->frame_count()), frame_size_(decoder_->frame_size()),
      backend_(decoder_->backend()), worker_([this] { run(); }) {}

FramePrefetcher::~FramePrefetcher() {
    {
        const std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    changed_.notify_all();
    worker_.join();
}

void FramePrefetcher::request(std::int64_t index, std::int64_t behind, std::int64_t ahead) {
    {
        const std::lock_guard lock(mutex_);
        const std::int64_t centre = std::clamp<std::int64_t>(index, 0, frame_count_ - 1);
        window_ = {std::max<std::int64_t>(0, centre - behind),
                   std::min(frame_count_ - 1, centre + ahead), centre};
        ++requested_;
    }
    changed_.notify_all();
}

std::shared_ptr<const VideoFrame> FramePrefetcher::frame(std::int64_t index) const {
    const std::lock_guard lock(mutex_);
    const auto it = cache_.find(index);
    return it != cache_.end() ? it->second : nullptr;
}

void FramePrefetcher::wait_until_idle() const {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [this] { return fulfilled_ == requested_ || error_ || stopping_; });
}

std::optional<std::string> FramePrefetcher::error() const {
    const std::lock_guard lock(mutex_);
    return error_;
}

std::optional<std::int64_t> FramePrefetcher::first_missing(const Window& window) const {
    // Frames at and after the centre are needed first, then the ones before it.
    for (std::int64_t i = window.centre; i <= window.last; ++i) {
        if (!cache_.contains(i)) {
            return i;
        }
    }
    for (std::int64_t i = window.first; i < window.centre; ++i) {
        if (!cache_.contains(i)) {
            return i;
        }
    }
    return std::nullopt;
}

void FramePrefetcher::store(VideoFrame&& frame, std::int64_t centre) {
    const std::int64_t index = frame.index;
    cache_[index] = std::make_shared<const VideoFrame>(std::move(frame));
    while (cache_.size() > capacity_) {
        // Drop whichever end of the cache is farther from the centre.
        const auto first = cache_.begin();
        const auto last = std::prev(cache_.end());
        cache_.erase(std::abs(first->first - centre) >= std::abs(last->first - centre) ? first
                                                                                        : last);
    }
}

void FramePrefetcher::run() {
    std::unique_lock lock(mutex_);
    for (;;) {
        changed_.wait(lock, [this] { return stopping_ || (requested_ != fulfilled_ && !error_); });
        if (stopping_) {
            return;
        }
        const std::uint64_t generation = requested_;
        const Window window = window_;
        const auto missing = first_missing(window);
        if (!missing) {
            fulfilled_ = generation;
            changed_.notify_all();
            continue;
        }

        lock.unlock();
        bool delivered = false;
        try {
            decoder_->decode(*missing, window.last, [&](VideoFrame&& frame) {
                const std::lock_guard guard(mutex_);
                delivered = true;
                if (frame.index >= window.first && frame.index <= window.last) {
                    store(std::move(frame), window.centre);
                }
                return requested_ == generation && !stopping_;
            });
        } catch (const std::exception& exception) {
            lock.lock();
            error_ = exception.what();
            changed_.notify_all();
            continue;
        }
        lock.lock();
        // Give up on frames that a full pass could not produce (e.g. past the end or corrupt).
        if (requested_ == generation && (!delivered || first_missing(window) == missing)) {
            fulfilled_ = generation;
            changed_.notify_all();
        }
    }
}

} // namespace ttrally::media
