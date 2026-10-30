// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "annotation/infrastructure/csv_annotation_repository.hpp"

namespace ttrally::annotation {

/// Label files that start out as a copy of other labels, e.g. refined rallies starting from the
/// detections: until a label file has been saved, the seed's labels of `seed_id` are loaded
/// instead.
class SeededAnnotationRepository final : public AnnotationRepository {
  public:
    SeededAnnotationRepository(CsvAnnotationRepository& target, AnnotationRepository& seed,
                               std::string seed_id)
        : target_(target), seed_(seed), seed_id_(std::move(seed_id)) {}

    [[nodiscard]] StoredLabels load(const std::string& video_id) override;
    void save(const AnnotationSheet& sheet) override { target_.save(sheet); }

  private:
    CsvAnnotationRepository& target_;
    AnnotationRepository& seed_;
    std::string seed_id_;
};

} // namespace ttrally::annotation
