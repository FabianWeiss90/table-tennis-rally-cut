// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "media/domain/decode_backend.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ttrally::features {

/// Hardware the image model runs on (ONNX Runtime execution provider).
enum class ExecutionProvider { Auto, Cuda, TensorRt, MiGraphX, WebGpu, Cpu };

[[nodiscard]] std::string_view to_string(ExecutionProvider provider) noexcept;
[[nodiscard]] std::optional<ExecutionProvider>
parse_execution_provider(std::string_view name) noexcept;
[[nodiscard]] std::vector<std::string> execution_provider_names();

/// Order in which `auto` tries the providers: CUDA, TensorRT, MIGraphX, WebGPU, then CPU.
[[nodiscard]] std::vector<ExecutionProvider> automatic_provider_order();

/// Decode backend for feature extraction. With `auto`, video is decoded on the CPU while the
/// model runs on a GPU: hardware decoding would compete with the model for the GPU and slow both
/// down, while the CPU is idle. With the model on the CPU, hardware decoding keeps the CPU free.
/// An explicitly requested backend is kept.
[[nodiscard]] media::DecodeBackend feature_decode_backend(media::DecodeBackend requested,
                                                          ExecutionProvider model) noexcept;

} // namespace ttrally::features
