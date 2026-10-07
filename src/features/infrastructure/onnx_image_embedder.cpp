// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "features/infrastructure/onnx_image_embedder.hpp"

#include "shared/io/file_cache.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <onnxruntime_cxx_api.h>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace ttrally::features {

namespace {

/// ONNX Runtime's name of a provider, as reported by Ort::GetAvailableProviders().
std::string_view runtime_name(ExecutionProvider provider) {
    switch (provider) {
    case ExecutionProvider::Cuda:
        return "CUDAExecutionProvider";
    case ExecutionProvider::TensorRt:
        return "TensorrtExecutionProvider";
    case ExecutionProvider::MiGraphX:
        return "MIGraphXExecutionProvider";
    case ExecutionProvider::WebGpu:
        return "WebGpuExecutionProvider";
    case ExecutionProvider::Cpu:
    case ExecutionProvider::Auto:
        break;
    }
    return "CPUExecutionProvider";
}

void append_provider(Ort::SessionOptions& options, ExecutionProvider provider) {
    switch (provider) {
    case ExecutionProvider::Cuda:
        options.AppendExecutionProvider_CUDA(OrtCUDAProviderOptions{});
        break;
    case ExecutionProvider::TensorRt:
        options.AppendExecutionProvider_TensorRT(OrtTensorRTProviderOptions{});
        break;
    case ExecutionProvider::MiGraphX:
        options.AppendExecutionProvider_MIGraphX(OrtMIGraphXProviderOptions{});
        break;
    case ExecutionProvider::WebGpu:
        options.AppendExecutionProvider("WebGPU");
        break;
    case ExecutionProvider::Cpu:
    case ExecutionProvider::Auto:
        break;
    }
}

/// Elements of a flat JSON array such as ["cls", "mean"] or [0.485, 0.456, 0.406].
std::vector<std::string> json_array(std::string_view text) {
    std::vector<std::string> values;
    std::string current;
    bool in_string = false;
    for (const char c : text) {
        if (c == '"') {
            in_string = !in_string;
        } else if (!in_string && (c == ',' || c == ']')) {
            if (!current.empty()) {
                values.push_back(current);
            }
            current.clear();
        } else if (in_string || (c != '[' && c != ' ')) {
            current += c;
        }
    }
    return values;
}

template <typename T> T parse_number(const std::string& text, std::string_view what) {
    T value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        throw std::runtime_error("invalid model metadata " + std::string(what) + ": " + text);
    }
    return value;
}

std::array<float, 3> parse_triple(const std::string& text, std::string_view what) {
    const auto values = json_array(text);
    if (values.size() != 3) {
        throw std::runtime_error("model metadata " + std::string(what) + " needs 3 values");
    }
    return {parse_number<float>(values[0], what), parse_number<float>(values[1], what),
            parse_number<float>(values[2], what)};
}

} // namespace

struct OnnxImageEmbedder::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "ttrally"};
    std::optional<Ort::Session> session;
    EmbedderInfo info;
    std::string input_name;
    std::string output_name;

    Impl(const std::filesystem::path& model, ExecutionProvider requested) {
        open_session(model, requested);
        read_metadata(model);
        Ort::AllocatorWithDefaultOptions allocator;
        input_name = session->GetInputNameAllocated(0, allocator).get();
        output_name = session->GetOutputNameAllocated(0, allocator).get();
    }

    void open_session(const std::filesystem::path& model, ExecutionProvider requested) {
        const std::vector<ExecutionProvider> candidates =
            requested == ExecutionProvider::Auto ? automatic_provider_order()
                                                 : std::vector<ExecutionProvider>{requested};
        const auto compiled = compiled_execution_providers();
        std::string errors;
        for (const ExecutionProvider candidate : candidates) {
            if (std::find(compiled.begin(), compiled.end(), candidate) == compiled.end()) {
                errors += std::string(to_string(candidate)) + ": not in this ONNX Runtime; ";
                continue;
            }
            try {
                Ort::SessionOptions options;
                options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
                append_provider(options, candidate);
                session.emplace(env, model.c_str(), options);
                info.provider = candidate;
                return;
            } catch (const Ort::Exception& error) {
                errors += std::string(to_string(candidate)) + ": " + error.what() + "; ";
            }
        }
        throw std::runtime_error("no execution provider could run the model (" + errors + ")");
    }

    void read_metadata(const std::filesystem::path& model) {
        Ort::AllocatorWithDefaultOptions allocator;
        const Ort::ModelMetadata metadata = session->GetModelMetadata();
        auto lookup = [&](const char* key) {
            auto value = metadata.LookupCustomMetadataMapAllocated(key, allocator);
            if (!value) {
                throw std::runtime_error(model.string() + " lacks the metadata " + key +
                                         " (export it with training/ttrally_training)");
            }
            return std::string(value.get());
        };
        info.model_name = lookup("ttrally.backbone");
        info.model_fingerprint = io::cache_key(model, "model");
        info.input.size = {parse_number<int>(lookup("ttrally.input_width"), "input_width"),
                           parse_number<int>(lookup("ttrally.input_height"), "input_height")};
        info.input.mean = parse_triple(lookup("ttrally.mean"), "mean");
        info.input.stddev = parse_triple(lookup("ttrally.std"), "std");
        info.parts = json_array(lookup("ttrally.parts"));
        info.part_dims = parse_number<std::size_t>(lookup("ttrally.part_dims"), "part_dims");
        if (info.parts.empty() || info.part_dims == 0) {
            throw std::runtime_error(model.string() + " describes no output features");
        }
    }

    std::vector<float> embed(std::span<const float> images, std::size_t batch) {
        const auto& size = info.input.size;
        const std::array<std::int64_t, 4> shape{static_cast<std::int64_t>(batch), 3, size.height,
                                                size.width};
        const std::size_t expected = batch * 3 * static_cast<std::size_t>(size.width) *
                                     static_cast<std::size_t>(size.height);
        if (images.size() != expected) {
            throw std::invalid_argument("image batch has the wrong size");
        }
        const Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        // ONNX Runtime does not modify input tensors; the API just lacks a const overload.
        Ort::Value input = Ort::Value::CreateTensor<float>(
            memory, const_cast<float*>(images.data()), images.size(), shape.data(), shape.size());
        const char* input_names[] = {input_name.c_str()};
        const char* output_names[] = {output_name.c_str()};
        auto outputs = session->Run(Ort::RunOptions{nullptr}, input_names, &input, 1, output_names, 1);
        const auto output_shape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
        if (output_shape.size() != 2 || output_shape[0] != static_cast<std::int64_t>(batch) ||
            output_shape[1] != static_cast<std::int64_t>(info.dims())) {
            throw std::runtime_error("the model's output does not match its metadata");
        }
        const float* data = outputs[0].GetTensorData<float>();
        return {data, data + batch * info.dims()};
    }
};

OnnxImageEmbedder::OnnxImageEmbedder(const std::filesystem::path& model,
                                     ExecutionProvider requested)
    : impl_(std::make_unique<Impl>(model, requested)) {}

OnnxImageEmbedder::~OnnxImageEmbedder() = default;

const EmbedderInfo& OnnxImageEmbedder::info() const { return impl_->info; }

std::vector<float> OnnxImageEmbedder::embed(std::span<const float> images, std::size_t batch) {
    return impl_->embed(images, batch);
}

std::vector<ExecutionProvider> compiled_execution_providers() {
    std::vector<ExecutionProvider> providers;
    const auto available = Ort::GetAvailableProviders();
    for (const ExecutionProvider provider : automatic_provider_order()) {
        if (std::find(available.begin(), available.end(), runtime_name(provider)) !=
            available.end()) {
            providers.push_back(provider);
        }
    }
    return providers;
}

std::string onnxruntime_version() { return Ort::GetVersionString(); }

} // namespace ttrally::features
