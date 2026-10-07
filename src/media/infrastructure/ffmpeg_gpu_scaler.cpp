// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "media/infrastructure/ffmpeg_gpu_scaler.hpp"

#include "media/application/media_error.hpp"

extern "C" {
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
}

#include <algorithm>
#include <format>
#include <string>

namespace ttrally::media::ff {

namespace {

/// FFmpeg filter that scales frames in the memory of a hardware backend, if there is one.
const char* scale_filter_name(DecodeBackend backend) {
    switch (backend) {
    case DecodeBackend::Vaapi:
        return "scale_vaapi";
    case DecodeBackend::Cuda:
        return "scale_cuda";
    case DecodeBackend::Vulkan:
        return "scale_vulkan";
    case DecodeBackend::D3d11va:
        return "scale_d3d11";
    case DecodeBackend::D3d12va:
    case DecodeBackend::Cpu:
    case DecodeBackend::Auto:
        break;
    }
    return nullptr;
}

/// Half of `source` (rounded up to even), but at least `final_side` and at most `source`.
int shrunk_side(int source, int final_side) {
    const int half = (source / 2 + 1) / 2 * 2;
    return std::min(source, std::max(half, final_side + final_side % 2));
}

AVFilterContext* add_filter(AVFilterGraph* graph, const char* name, const char* label,
                            const std::string& args = {}) {
    const AVFilter* filter = avfilter_get_by_name(name);
    if (filter == nullptr) {
        throw MediaError(std::string("FFmpeg lacks the filter ") + name);
    }
    AVFilterContext* context = nullptr;
    check(avfilter_graph_create_filter(&context, filter, label,
                                       args.empty() ? nullptr : args.c_str(), nullptr, graph),
          std::string("filter ") + name);
    return context;
}

} // namespace

FrameSize gpu_scaled_size(FrameSize source, FrameSize final_size) noexcept {
    return {shrunk_side(source.width, final_size.width),
            shrunk_side(source.height, final_size.height)};
}

std::unique_ptr<GpuScaler> GpuScaler::create(DecodeBackend backend, const AVFrame& frame,
                                             FrameSize size) {
    const char* filter = scale_filter_name(backend);
    if (filter == nullptr || avfilter_get_by_name(filter) == nullptr ||
        frame.hw_frames_ctx == nullptr) {
        return nullptr;
    }
    const QuietLogScope quiet; // a failing setup is expected on some drivers
    std::unique_ptr<GpuScaler> scaler(new GpuScaler());
    try {
        scaler->build(filter, frame, size);
    } catch (const MediaError&) {
        return nullptr;
    }
    return scaler;
}

GpuScaler::~GpuScaler() = default;

/// Graph: buffer (hardware frames) -> GPU scaler -> hwdownload -> format -> buffersink. The
/// format filter names the layout of the downloaded frames, which hwdownload cannot choose.
void GpuScaler::build(const char* scale_filter, const AVFrame& frame, FrameSize size) {
    graph_.reset(avfilter_graph_alloc());
    if (!graph_) {
        throw MediaError("out of memory (avfilter_graph_alloc)");
    }
    source_ = avfilter_graph_alloc_filter(graph_.get(), avfilter_get_by_name("buffer"), "in");
    AVBufferSrcParameters* parameters = av_buffersrc_parameters_alloc();
    if (source_ == nullptr || parameters == nullptr) {
        av_free(parameters);
        throw MediaError("out of memory (buffer source)");
    }
    parameters->format = frame.format;
    parameters->width = frame.width;
    parameters->height = frame.height;
    parameters->time_base = {1, 1}; // timestamps are not used
    parameters->sample_aspect_ratio = frame.sample_aspect_ratio;
    parameters->color_space = frame.colorspace;
    parameters->color_range = frame.color_range;
    parameters->hw_frames_ctx = frame.hw_frames_ctx;
    const int set = av_buffersrc_parameters_set(source_, parameters);
    av_free(parameters);
    check(set, "buffer source parameters");
    check(avfilter_init_str(source_, nullptr), "buffer source");

    AVFilterContext* scale = add_filter(graph_.get(), scale_filter, "scale",
                                        std::format("w={}:h={}", size.width, size.height));
    AVFilterContext* download = add_filter(graph_.get(), "hwdownload", "download");
    const auto* frames = reinterpret_cast<const AVHWFramesContext*>(frame.hw_frames_ctx->data);
    AVFilterContext* format = add_filter(
        graph_.get(), "format", "format",
        std::string("pix_fmts=") + av_get_pix_fmt_name(frames->sw_format));
    sink_ = add_filter(graph_.get(), "buffersink", "out");
    check(avfilter_link(source_, 0, scale, 0), "filter link");
    check(avfilter_link(scale, 0, download, 0), "filter link");
    check(avfilter_link(download, 0, format, 0), "filter link");
    check(avfilter_link(format, 0, sink_, 0), "filter link");
    check(avfilter_graph_config(graph_.get(), nullptr), "GPU scaling setup");
}

FramePtr GpuScaler::download(const AVFrame& frame) {
    check(av_buffersrc_write_frame(source_, &frame), "GPU scaling");
    auto scaled = make_frame();
    check(av_buffersink_get_frame(sink_, scaled.get()), "GPU scaling");
    return scaled;
}

} // namespace ttrally::media::ff
