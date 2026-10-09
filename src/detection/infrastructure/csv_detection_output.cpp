// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "detection/infrastructure/csv_detection_output.hpp"

#include "annotation/infrastructure/csv_annotation_repository.hpp"
#include "shared/io/npy.hpp"

#include <array>
#include <format>

namespace ttrally::detection {

void CsvDetectionOutput::save(const Detection& detection) {
    annotation::AnnotationSheet sheet(detection.video_id, detection.video_fps,
                                      detection.video_frame_count);
    for (const DetectedRally& rally : detection.rallies) {
        annotation::RallyLabel label;
        label.start_frame = rally.frames.start_frame;
        label.end_frame = rally.frames.end_frame;
        label.notes = std::format("p={:.2f}", rally.mean_probability);
        sheet.add(std::move(label));
    }
    std::filesystem::create_directories(directory_);
    annotation::CsvAnnotationRepository(directory_).save(sheet);
    const std::array<std::size_t, 1> shape{detection.probabilities.size()};
    io::write_npy<float>(directory_ / (detection.video_id + ".probabilities.npy"),
                         detection.probabilities, shape);
}

std::string CsvDetectionOutput::location(const std::string& video_id) const {
    return (directory_ / (video_id + ".csv")).string();
}

} // namespace ttrally::detection
