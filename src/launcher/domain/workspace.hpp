// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace ttrally::launcher {

/// Folder the tool keeps its own files in: one per match day (next to the videos) and one in the
/// library itself for what belongs to all match days (image models, detectors).
inline constexpr std::string_view kWorkFolder = "ttrally-data";

/// How far the annotation of a video has got.
enum class AnnotationProgress {
    None,       ///< no labels yet
    InProgress, ///< labels exist, but some segments or gaps are not done
    Complete,   ///< every segment and gap is done: usable for training
};

/// An original video of a match day and how far it has been processed. Paths are absolute.
struct VideoEntry {
    std::string match_day; ///< name of the match day folder
    std::string original;
    std::string video_id;  ///< unique within the match day
    std::string work_dir;  ///< the match day's ttrally-data folder
    std::vector<std::string> cut_candidates; ///< Pre-cut versions (e.g. from Liimba) for align
    bool aligned = false;
    AnnotationProgress annotation = AnnotationProgress::None;
    std::size_t rallies = 0;
    std::string features_model; ///< Image model of the stored features; empty: none
    bool detected = false;
    AnnotationProgress refinement = AnnotationProgress::None; ///< of the detections (refine)
    std::vector<std::string> cuts; ///< Cut videos
    std::string scene; ///< e.g. "A": videos of a match day with the same hall and camera position

    /// Unique in the library: "<match day>/<video id>".
    [[nodiscard]] std::string key() const { return match_day + "/" + video_id; }
};

/// Everything the launcher shows and suggests.
struct WorkspaceSnapshot {
    std::string library;                ///< the library folder (absolute)
    std::vector<VideoEntry> videos;     ///< sorted by match day, then name
    std::vector<std::string> backbones; ///< Image models for features
    std::vector<std::string> detectors; ///< Trained rally detectors
    std::vector<std::string> trainable; ///< Keys of videos with complete labels and features
    std::string uv = "uv";              ///< Program that runs the Python training

    /// The library's own ttrally-data folder.
    [[nodiscard]] std::string shared_dir() const { return library + "/" + std::string(kWorkFolder); }
};

} // namespace ttrally::launcher
