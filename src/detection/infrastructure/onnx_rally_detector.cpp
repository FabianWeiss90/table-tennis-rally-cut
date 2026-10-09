// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "detection/infrastructure/onnx_rally_detector.hpp"

#include "shared/io/json.hpp"

#include <array>
#include <charconv>
#include <format>
#include <onnxruntime_cxx_api.h>
#include <optional>
#include <stdexcept>
#include <string>

namespace ttrally::detection {

namespace {

constexpr std::string_view kKind = "rally-detector";
constexpr std::string_view kFormatVersion = "1";

double number(std::string_view text, std::string_view what) {
    double value = 0.0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        throw std::runtime_error(std::format("invalid detector metadata {}: {}", what, text));
    }
    return value;
}

} // namespace

DecodingParams parse_decoding(std::string_view json) {
    try {
        const io::JsonValue params = io::parse_json(json);
        const std::string& method = params.at("method").as_string();
        if (method == "threshold") {
            return ThresholdDecoding{params.at("threshold").as_number(),
                                     params.at("merge_gap_s").as_number(),
                                     params.at("min_rally_s").as_number()};
        }
        if (method == "viterbi") {
            return ViterbiDecoding{params.at("mean_rally_s").as_number(),
                                   params.at("mean_pause_s").as_number()};
        }
        throw std::runtime_error("unknown decoding method \"" + method + "\"");
    } catch (const io::JsonError& error) {
        throw std::runtime_error(std::string("invalid decoding in the detector: ") + error.what());
    }
}

struct OnnxRallyDetector::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "ttrally"};
    std::optional<Ort::Session> session;
    DetectorInfo info;
    std::string input_name;
    std::string output_name;

    explicit Impl(const std::filesystem::path& model) {
        Ort::SessionOptions options;
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        session.emplace(env, model.c_str(), options);
        read_metadata(model);
        Ort::AllocatorWithDefaultOptions allocator;
        input_name = session->GetInputNameAllocated(0, allocator).get();
        output_name = session->GetOutputNameAllocated(0, allocator).get();
    }

    void read_metadata(const std::filesystem::path& model) {
        Ort::AllocatorWithDefaultOptions allocator;
        const Ort::ModelMetadata metadata = session->GetModelMetadata();
        auto lookup = [&](const char* key) -> std::optional<std::string> {
            auto value = metadata.LookupCustomMetadataMapAllocated(key, allocator);
            return value ? std::optional<std::string>(value.get()) : std::nullopt;
        };
        auto required = [&](const char* key) {
            auto value = lookup(key);
            if (!value) {
                throw std::runtime_error(std::format(
                    "{} lacks the metadata {} (export it with training/ttrally_training)",
                    model.string(), key));
            }
            return *value;
        };
        if (lookup("ttrally.kind") != kKind) {
            throw std::runtime_error(model.string() + " is not a rally detector");
        }
        if (required("ttrally.format_version") != kFormatVersion) {
            throw std::runtime_error(model.string() + " was exported by another version");
        }
        try {
            info.backbone = required("ttrally.backbone");
            info.backbone_execution_provider = lookup("ttrally.backbone_execution_provider")
                                                   .value_or("");
            const io::JsonValue parts = io::parse_json(required("ttrally.parts"));
            for (const auto& part : parts.as_array()) {
                info.parts.push_back(part.as_string());
            }
            info.part_dims =
                static_cast<std::size_t>(number(required("ttrally.part_dims"), "part_dims"));
            const auto size = io::parse_json(required("ttrally.input_size"));
            info.input_width = static_cast<int>(size.as_array().at(0).as_number());
            info.input_height = static_cast<int>(size.as_array().at(1).as_number());
        } catch (const io::JsonError& error) {
            throw std::runtime_error(model.string() + ": invalid metadata: " + error.what());
        }
        info.sample_rate_hz = number(required("ttrally.sample_rate_hz"), "sample_rate_hz");
        info.decoding = parse_decoding(required("ttrally.decoding"));
    }

    std::vector<float> run(std::span<const float> features, std::size_t rows) {
        const std::size_t dims = info.dims();
        if (features.size() != rows * dims) {
            throw std::invalid_argument("the features do not have the detector's dimensions");
        }
        const std::array<std::int64_t, 3> shape{1, static_cast<std::int64_t>(rows),
                                                static_cast<std::int64_t>(dims)};
        const Ort::MemoryInfo memory =
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        // ONNX Runtime does not modify input tensors; the API just lacks a const overload.
        Ort::Value input = Ort::Value::CreateTensor<float>(
            memory, const_cast<float*>(features.data()), features.size(), shape.data(),
            shape.size());
        const char* input_names[] = {input_name.c_str()};
        const char* output_names[] = {output_name.c_str()};
        auto outputs =
            session->Run(Ort::RunOptions{nullptr}, input_names, &input, 1, output_names, 1);
        const auto output_shape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
        if (output_shape.size() != 2 || output_shape[1] != static_cast<std::int64_t>(rows)) {
            throw std::runtime_error("the detector's output does not have one value per row");
        }
        const float* data = outputs[0].GetTensorData<float>();
        return {data, data + rows};
    }
};

OnnxRallyDetector::OnnxRallyDetector(const std::filesystem::path& model)
    : impl_(std::make_unique<Impl>(model)) {}

OnnxRallyDetector::~OnnxRallyDetector() = default;

const DetectorInfo& OnnxRallyDetector::info() const { return impl_->info; }

std::vector<float> OnnxRallyDetector::probabilities(std::span<const float> features,
                                                    std::size_t rows) {
    return impl_->run(features, rows);
}

} // namespace ttrally::detection
