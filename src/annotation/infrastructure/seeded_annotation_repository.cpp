// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "annotation/infrastructure/seeded_annotation_repository.hpp"

namespace ttrally::annotation {

StoredLabels SeededAnnotationRepository::load(const std::string& video_id) {
    return std::filesystem::exists(target_.file_for(video_id)) ? target_.load(video_id)
                                                               : seed_.load(seed_id_);
}

} // namespace ttrally::annotation
