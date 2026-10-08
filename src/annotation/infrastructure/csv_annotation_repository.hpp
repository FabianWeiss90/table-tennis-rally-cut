// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "annotation/application/ports.hpp"

#include <filesystem>

namespace ttrally::annotation {

/// Label files <directory>/<video_id>.csv in the project's label format:
///   video_id,rally_id,start_frame,end_frame,fps,serve_contact_frame,flags,notes
/// Ignored sections are rows with the flag `ignore` and an empty rally_id, so the rally ids
/// stay 1, 2, 3, ...; all rows are sorted by start frame.
class CsvAnnotationRepository final : public AnnotationRepository {
  public:
    explicit CsvAnnotationRepository(std::filesystem::path directory)
        : directory_(std::move(directory)) {}

    [[nodiscard]] StoredLabels load(const std::string& video_id) override;
    void save(const AnnotationSheet& sheet) override;

    [[nodiscard]] std::filesystem::path file_for(const std::string& video_id) const;

  private:
    std::filesystem::path directory_;
};

/// Frame rate as written in label files: two decimals without trailing zeros (59.94, 60, 25).
[[nodiscard]] std::string format_label_fps(Rational fps);

} // namespace ttrally::annotation
