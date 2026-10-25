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
    // Frames at and after the centre are needed first, then the ones before it, nearest first
    for (std::int64_t i = window.centre; i <= window.last; ++i) {
        if (!cache_.contains(i)) {
            return i;
        }
    }
    for (std::int64_t i = window.centre - 1; i >= window.first; --i) {
        if (!cache_.contains(i)) {
            return i;
        }
    }
    return std::nullopt;
}

bool FramePrefetcher::still_useful(std::int64_t missing) const {
    // A newer request (e.g. the next step) keeps the pass going if it still needs the frame the
    // pass was started for and has its own frame already; restarting would seek again
    return missing >= window_.first && missing <= window_.last && cache_.contains(window_.centre);
}

std::int64_t FramePrefetcher::decode_end(const Window& window, std::int64_t missing) const {
    const auto next_cached = cache_.upper_bound(missing);
    return next_cached != cache_.end() ? std::min(next_cached->first - 1, window.last)
                                       : window.last;
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
        const std::int64_t last = decode_end(window, *missing);

        lock.unlock();
        bool delivered = false;
        try {
            decoder_->decode_with_lead_in(*missing, last, [&](VideoFrame&& frame) {
                const std::lock_guard guard(mutex_);
                delivered = true;
                // Lead-in frames before the window help going back, so they are kept too
                if (frame.index <= std::max(window.last, window_.last)) {
                    store(std::move(frame), window_.centre);
                }
                return !stopping_ && (requested_ == generation || still_useful(*missing));
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
