// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "media/application/frame_prefetcher.hpp"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <stdexcept>

using namespace ttrally::media;

namespace {

/// Delivers frames whose single pixel value is the frame index; counts seeks.
class CountingDecoder final : public FrameDecoder {
  public:
    explicit CountingDecoder(std::int64_t frames, std::atomic<int>& decoded)
        : frames_(frames), decoded_(decoded) {}
    DecodeBackend backend() const override { return DecodeBackend::Cpu; }
    std::int64_t frame_count() const override { return frames_; }
    FrameSize frame_size() const override { return {2, 2}; }
    void decode(std::int64_t first, std::int64_t last, const FrameConsumer& consume) override {
        if (fail_at && *fail_at >= first && *fail_at <= last) {
            throw std::runtime_error("corrupt frame");
        }
        for (std::int64_t i = std::max<std::int64_t>(first, 0); i <= std::min(last, frames_ - 1);
             ++i) {
            ++decoded_;
            VideoFrame frame;
            frame.index = i;
            frame.size = {2, 2};
            frame.planes.assign(VideoFrame::bytes_for(frame.size), static_cast<std::uint8_t>(i));
            if (!consume(std::move(frame))) {
                return;
            }
        }
    }
    std::optional<std::int64_t> fail_at;

  private:
    std::int64_t frames_;
    std::atomic<int>& decoded_;
};

} // namespace

TEST_CASE("requested frames become available and the cache stays bounded") {
    std::atomic<int> decoded{0};
    FramePrefetcher prefetcher(std::make_unique<CountingDecoder>(1000, decoded), 50);
    prefetcher.request(500, 10, 20);
    prefetcher.wait_until_idle();
    REQUIRE(prefetcher.frame(500));
    CHECK(prefetcher.frame(500)->planes.front() == static_cast<std::uint8_t>(500));
    CHECK(prefetcher.frame(490));
    CHECK(prefetcher.frame(520));
    CHECK_FALSE(prefetcher.frame(521));

    const int before = decoded.load();
    prefetcher.request(505, 10, 10); // fully cached already
    prefetcher.wait_until_idle();
    CHECK(decoded.load() == before);

    prefetcher.request(100, 0, 60); // far away: the old frames are evicted
    prefetcher.wait_until_idle();
    CHECK(prefetcher.frame(100));
    CHECK_FALSE(prefetcher.frame(500));
}

TEST_CASE("requests are clamped to the video") {
    std::atomic<int> decoded{0};
    FramePrefetcher prefetcher(std::make_unique<CountingDecoder>(10, decoded), 50);
    prefetcher.request(8, 2, 100);
    prefetcher.wait_until_idle();
    CHECK(prefetcher.frame(9));
    CHECK_FALSE(prefetcher.error());
}

TEST_CASE("decoding errors are reported instead of retried forever") {
    std::atomic<int> decoded{0};
    auto decoder = std::make_unique<CountingDecoder>(100, decoded);
    decoder->fail_at = 40;
    FramePrefetcher prefetcher(std::move(decoder), 50);
    prefetcher.request(40, 0, 0);
    prefetcher.wait_until_idle();
    REQUIRE(prefetcher.error());
    CHECK(*prefetcher.error() == "corrupt frame");
}
