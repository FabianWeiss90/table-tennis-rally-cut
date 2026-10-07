// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

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

} // namespace ttrally::features
