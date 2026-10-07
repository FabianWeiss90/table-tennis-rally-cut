// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

// Internal: shrinks hardware frames on the GPU before they are copied to system memory, which
// is much cheaper than copying them at full size.

#pragma once

#include "media/domain/video_frame.hpp"
#include "media/infrastructure/ffmpeg_api.hpp"

extern "C" {
#include <libavfilter/avfilter.h>
}

#include <memory>

namespace ttrally::media::ff {

/// Size a frame is shrunk to on the GPU when it is finally needed at `final_size`: each side is
/// at most halved, because the GPU scalers take too few source pixels into account for larger
/// factors (visible aliasing; a ball of a few pixels flickers). Never smaller than final_size
/// and never larger than the source; the rest is done by the software scaler. Even sides.
[[nodiscard]] FrameSize gpu_scaled_size(FrameSize source, FrameSize final_size) noexcept;

class GpuScaler {
  public:
    /// Scaler for hardware frames like `frame` of the given backend, shrinking them to `size`.
    /// nullptr if FFmpeg has no scaling filter for the backend or it cannot be set up; callers
    /// then copy the frames at full size.
    [[nodiscard]] static std::unique_ptr<GpuScaler> create(DecodeBackend backend,
                                                           const AVFrame& frame, FrameSize size);

    GpuScaler(const GpuScaler&) = delete;
    GpuScaler& operator=(const GpuScaler&) = delete;
    ~GpuScaler();

    /// The hardware frame shrunk on the GPU and copied to system memory. Throws MediaError.
    [[nodiscard]] FramePtr download(const AVFrame& frame);

  private:
    struct GraphFreer {
        void operator()(AVFilterGraph* graph) const noexcept { avfilter_graph_free(&graph); }
    };

    GpuScaler() = default;
    void build(const char* scale_filter, const AVFrame& frame, FrameSize size);

    std::unique_ptr<AVFilterGraph, GraphFreer> graph_;
    AVFilterContext* source_ = nullptr; ///< Owned by graph_
    AVFilterContext* sink_ = nullptr;   ///< Owned by graph_
};

} // namespace ttrally::media::ff
