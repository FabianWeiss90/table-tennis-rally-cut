// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "features/application/image_embedder.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace ttrally::features {

/// Image model run with ONNX Runtime. Input size, normalisation and the layout of the output are
/// read from the model's metadata (written by training/ttrally_training/export_backbone.py).
class OnnxImageEmbedder final : public ImageEmbedder {
  public:
    /// With ExecutionProvider::Auto the providers are tried in automatic_provider_order(); the
    /// first one that is compiled into ONNX Runtime and works is used.
    OnnxImageEmbedder(const std::filesystem::path& model, ExecutionProvider requested);
    ~OnnxImageEmbedder() override;
    OnnxImageEmbedder(const OnnxImageEmbedder&) = delete;
    OnnxImageEmbedder& operator=(const OnnxImageEmbedder&) = delete;

    [[nodiscard]] const EmbedderInfo& info() const override;
    [[nodiscard]] std::vector<float> embed(std::span<const float> images,
                                           std::size_t batch) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Execution providers compiled into the ONNX Runtime library in use.
[[nodiscard]] std::vector<ExecutionProvider> compiled_execution_providers();

/// Version of the ONNX Runtime library in use.
[[nodiscard]] std::string onnxruntime_version();

} // namespace ttrally::features
