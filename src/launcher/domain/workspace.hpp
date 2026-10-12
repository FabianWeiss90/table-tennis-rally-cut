// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace ttrally::launcher {

/// How far the annotation of a video has got.
enum class AnnotationProgress {
    None,       ///< no labels yet
    InProgress, ///< labels exist, but some segments or gaps are not done
    Complete,   ///< every segment and gap is done: usable for training
};

/// An original video in the data directory and how far it has been processed. Paths are
/// relative to the project directory (e.g. "data/match.mp4").
struct VideoEntry {
    std::string original;
    std::string video_id;
    std::vector<std::string> cut_candidates; ///< Pre-cut versions (e.g. from Liimba) for align
    bool aligned = false;
    AnnotationProgress annotation = AnnotationProgress::None;
    std::size_t rallies = 0;
    std::string features_model; ///< Image model of the stored features; empty: none
    bool detected = false;
    std::vector<std::string> cuts; ///< Cut videos in data/cuts
};

/// Everything the launcher shows and suggests.
struct WorkspaceSnapshot {
    std::vector<VideoEntry> videos;
    std::vector<std::string> backbones; ///< Image models for features
    std::vector<std::string> detectors; ///< Trained rally detectors
    std::vector<std::string> trainable; ///< Ids of videos with complete labels and features
    std::string uv = "uv";              ///< Program that runs the Python training
};

} // namespace ttrally::launcher
