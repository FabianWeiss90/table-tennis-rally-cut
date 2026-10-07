// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "features/application/extract_features.hpp"

#include "features/domain/model_input.hpp"
#include "features/domain/sampling.hpp"
#include "media/application/media_error.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <exception>
#include <format>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace ttrally::features {

namespace {

constexpr int kProgressSteps = 20;

/// Batches waiting for the model. Two keep the GPU busy while the next one is decoded without
/// letting decoded frames pile up in memory when the model is the slower side.
constexpr std::size_t kQueuedBatches = 2;

/// Model input of several frames, ready for one model run.
struct Batch {
    std::vector<float> images;
    std::vector<std::int64_t> frames;
};

/// Hands batches from the decoding thread to the model. Either side can close it: the decoder
/// when all frames are delivered, the model side when it fails and decoding should stop.
class BatchQueue {
  public:
    /// Returns false if the queue was closed, i.e. nobody takes batches any more.
    bool push(Batch batch) {
        std::unique_lock lock(mutex_);
        has_room_.wait(lock, [&] { return closed_ || batches_.size() < kQueuedBatches; });
        if (closed_) {
            return false;
        }
        batches_.push_back(std::move(batch));
        has_batch_.notify_one();
        return true;
    }

    /// The next batch, or nothing once the queue is closed and empty.
    std::optional<Batch> pop() {
        std::unique_lock lock(mutex_);
        has_batch_.wait(lock, [&] { return closed_ || !batches_.empty(); });
        if (batches_.empty()) {
            return std::nullopt;
        }
        Batch batch = std::move(batches_.front());
        batches_.pop_front();
        has_room_.notify_one();
        return batch;
    }

    void close() {
        const std::lock_guard lock(mutex_);
        closed_ = true;
        has_batch_.notify_all();
        has_room_.notify_all();
    }

  private:
    std::mutex mutex_;
    std::condition_variable has_batch_;
    std::condition_variable has_room_;
    std::deque<Batch> batches_;
    bool closed_ = false;
};

/// Decodes the frames on a background thread, so decoding the next frames overlaps with the
/// model run on the current batch. Errors are rethrown by finish().
class BatchDecoder {
  public:
    BatchDecoder(media::FrameDecoder& decoder, std::span<const std::int64_t> frames,
                 const ModelInputSpec& input, std::size_t batch_size, BatchQueue& queue)
        : worker_([&decoder, frames, &input, batch_size, &queue, this] {
              try {
                  decode(decoder, frames, input, std::max<std::size_t>(1, batch_size), queue);
              } catch (...) {
                  error_ = std::current_exception();
              }
              queue.close();
          }) {}
    BatchDecoder(const BatchDecoder&) = delete;
    BatchDecoder& operator=(const BatchDecoder&) = delete;
    ~BatchDecoder() {
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    /// Waits for the decoding thread and rethrows its error, if any.
    void finish() {
        worker_.join();
        if (error_) {
            std::rethrow_exception(error_);
        }
    }

  private:
    static void decode(media::FrameDecoder& decoder, std::span<const std::int64_t> frames,
                       const ModelInputSpec& input, std::size_t batch_size, BatchQueue& queue) {
        Batch batch;
        bool open = true;
        decoder.decode_selected(frames, [&](media::VideoFrame&& frame) {
            append_model_input(frame, input, batch.images);
            batch.frames.push_back(frame.index);
            if (batch.frames.size() == batch_size) {
                open = queue.push(std::exchange(batch, {}));
            }
            return open;
        });
        if (open && !batch.frames.empty()) {
            queue.push(std::move(batch));
        }
    }

    std::exception_ptr error_;
    std::thread worker_;
};

/// Runs the model on a batch and writes the feature rows of all samples that use one of its
/// frames (several samples can share one frame in variable-rate videos).
void embed_batch(ImageEmbedder& embedder, const Batch& batch,
                 const std::multimap<std::int64_t, std::size_t>& rows_of_frame,
                 FeatureSet& features) {
    const std::size_t dims = embedder.info().dims();
    const std::vector<float> output = embedder.embed(batch.images, batch.frames.size());
    for (std::size_t i = 0; i < batch.frames.size(); ++i) {
        const auto [first, last] = rows_of_frame.equal_range(batch.frames[i]);
        for (auto it = first; it != last; ++it) {
            std::memcpy(features.values.data() + it->second * dims, output.data() + i * dims,
                        dims * sizeof(float));
        }
    }
}

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
                         samples.size(),
                         std::string(to_string(model.provider))};
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
    auto decoder = decoders_.open(request.video, timestamps,
                                  feature_decode_backend(request.decode_backend, model.provider),
                                  {.height = model.input.size.height,
                                   .width = model.input.size.width,
                                   .layout = media::PixelLayout::Rgb24});
    progress_.report(std::format("Computing features of {} frames ({} per second, {}, decoder {}, "
                                 "model on {})...",
                                 frames.size(), request.sample_rate_hz, model.model_name,
                                 media::to_string(decoder->backend()), to_string(model.provider)));

    BatchQueue queue;
    std::size_t done = 0;
    std::size_t next_report = frames.size() / kProgressSteps;
    {
        BatchDecoder decoding(*decoder, frames, model.input, request.batch_size, queue);
        try {
            while (const auto batch = queue.pop()) {
                embed_batch(embedder_, *batch, rows_of_frame, features);
                done += batch->frames.size();
                if (done >= next_report && next_report > 0) {
                    progress_.report(std::format("  {:3} %", 100 * done / frames.size()));
                    next_report += frames.size() / kProgressSteps;
                }
            }
        } catch (...) {
            queue.close(); // stops the decoding thread
            throw;
        }
        decoding.finish();
    }
    if (done != frames.size()) {
        throw media::MediaError(
            std::format("only {} of {} frames could be decoded", done, frames.size()));
    }

    store_.save(features);
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return {false, samples.size(), model.dims(), seconds};
}

} // namespace ttrally::features
