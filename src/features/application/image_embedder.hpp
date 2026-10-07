// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "features/domain/execution_provider.hpp"
#include "features/domain/model_input.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace ttrally::features {

/// Description of an image model, read from the model itself.
struct EmbedderInfo {
    std::string model_name;          ///< e.g. "facebook/dinov2-base"
    std::string model_fingerprint;   ///< Changes whenever the model file changes
    ModelInputSpec input;
    std::vector<std::string> parts;  ///< e.g. cls, mean, top_left, ...
    std::size_t part_dims = 0;
    ExecutionProvider provider = ExecutionProvider::Cpu; ///< Provider actually used

    [[nodiscard]] std::size_t dims() const noexcept { return parts.size() * part_dims; }
};

/// Port: turns a batch of normalised images (N x 3 x H x W, planar) into N feature vectors.
class ImageEmbedder {
  public:
    virtual ~ImageEmbedder() = default;
    [[nodiscard]] virtual const EmbedderInfo& info() const = 0;
    /// Returns batch * info().dims() values, row by row.
    [[nodiscard]] virtual std::vector<float> embed(std::span<const float> images,
                                                   std::size_t batch) = 0;
};

} // namespace ttrally::features
