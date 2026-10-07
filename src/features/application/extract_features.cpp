// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "features/application/extract_features.hpp"

#include "features/domain/sampling.hpp"
#include "media/application/media_error.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>
#include <map>

namespace ttrally::features {

namespace {

constexpr int kProgressSteps = 20;

/// Collects decoded frames into batches, runs the model and writes the feature rows of all
/// samples that use a frame (several samples can share one frame in variable-rate videos).
class BatchRunner {
  public:
    BatchRunner(ImageEmbedder& embedder, std::size_t batch_size, FeatureSet& features,
                const std::multimap<std::int64_t, std::size_t>& rows_of_frame)
        : embedder_(embedder), batch_size_(std::max<std::size_t>(1, batch_size)),
          dims_(embedder.info().dims()), features_(features), rows_of_frame_(rows_of_frame) {}

    void add(const media::VideoFrame& frame) {
        append_model_input(frame, embedder_.info().input, images_);
        frames_.push_back(frame.index);
        if (frames_.size() == batch_size_) {
            flush();
        }
    }

    void flush() {
        if (frames_.empty()) {
            return;
        }
        const std::vector<float> output = embedder_.embed(images_, frames_.size());
        for (std::size_t i = 0; i < frames_.size(); ++i) {
            const auto [first, last] = rows_of_frame_.equal_range(frames_[i]);
            for (auto it = first; it != last; ++it) {
                std::memcpy(features_.values.data() + it->second * dims_, output.data() + i * dims_,
                            dims_ * sizeof(float));
            }
        }
        done_ += frames_.size();
        images_.clear();
        frames_.clear();
    }

    [[nodiscard]] std::size_t done() const noexcept { return done_; }

  private:
    ImageEmbedder& embedder_;
    std::size_t batch_size_;
    std::size_t dims_;
    FeatureSet& features_;
    const std::multimap<std::int64_t, std::size_t>& rows_of_frame_;
    std::vector<float> images_;
    std::vector<std::int64_t> frames_;
    std::size_t done_ = 0;
};

} // namespace

ExtractFeatures::ExtractFeatures(media::MediaProbe& probe, media::MediaReader& reader,
                                 media::FrameDecoderFactory& decoders, ImageEmbedder& embedder,
                                 FeatureStore& store, ProgressReporter& progress)
    : probe_(probe), reader_(reader), decoders_(decoders), embedder_(embedder), store_(store),
      progress_(progress) {}

ExtractFeaturesResult ExtractFeatures::execute(const ExtractFeaturesRequest& request) {
    const auto start = std::chrono::steady_clock::now();
    const media::MediaInfo info = probe_.probe(request.video);
    if (!info.video) {
        throw media::MediaError(request.video.string() + " has no video stream");
    }
    const auto timestamps =
        *reader_
             .read(request.video,
                   {.audio_sample_rate = std::nullopt, .video_stream_index = info.video->index})
             .video_timestamps;
    const FrameTimeline timeline = timestamps.timeline(info.video->nominal_frame_rate());
    const std::vector<Sample> samples =
        plan_samples(timeline, timestamps.frame_count(), request.sample_rate_hz);

    const EmbedderInfo& model = embedder_.info();
    FeatureSet features;
    features.manifest = {request.video_id,
                         request.video.string(),
                         request.video_fingerprint,
                         model.model_name,
                         model.model_fingerprint,
                         model.parts,
                         model.part_dims,
                         request.sample_rate_hz,
                         model.input.size.width,
                         model.input.size.height,
                         timeline.nominal_fps(),
                         timestamps.frame_count(),
                         samples.size()};
    if (!request.force && store_.is_current(features.manifest)) {
        progress_.report("Features are up to date.");
        return {true, samples.size(), model.dims(), 0.0};
    }

    std::multimap<std::int64_t, std::size_t> rows_of_frame;
    for (std::size_t row = 0; row < samples.size(); ++row) {
        rows_of_frame.emplace(samples[row].frame, row);
        features.times_s.push_back(samples[row].time_s);
        features.frames.push_back(samples[row].frame);
    }
    features.values.assign(samples.size() * model.dims(), 0.0F);

    const std::vector<std::int64_t> frames = frames_to_decode(samples);
    auto decoder = decoders_.open(request.video, timestamps, request.decode_backend,
                                  {.height = model.input.size.height,
                                   .width = model.input.size.width,
                                   .layout = media::PixelLayout::Rgb24});
    progress_.report(std::format("Computing features of {} frames ({} per second, {}, decoder {}, "
                                 "model on {})...",
                                 frames.size(), request.sample_rate_hz, model.model_name,
                                 media::to_string(decoder->backend()), to_string(model.provider)));

    BatchRunner runner(embedder_, request.batch_size, features, rows_of_frame);
    std::size_t next_report = frames.size() / kProgressSteps;
    decoder->decode_selected(frames, [&](media::VideoFrame&& frame) {
        runner.add(frame);
        if (runner.done() >= next_report && next_report > 0) {
            progress_.report(std::format("  {:3} %", 100 * runner.done() / frames.size()));
            next_report += frames.size() / kProgressSteps;
        }
        return true;
    });
    runner.flush();
    if (runner.done() != frames.size()) {
        throw media::MediaError(std::format("only {} of {} frames could be decoded",
                                            runner.done(), frames.size()));
    }

    store_.save(features);
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return {false, samples.size(), model.dims(), seconds};
}

} // namespace ttrally::features
