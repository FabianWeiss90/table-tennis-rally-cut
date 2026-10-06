// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "annotation/application/annotation_session.hpp"
#include "media/application/frame_prefetcher.hpp"
#include "shared/kernel/rational.hpp"

#include <filesystem>
#include <string>

namespace ttrally::gui {

struct AnnotatorOptions {
    std::string title = "ttrally annotate";
    int max_frames = 0; ///< Stop after this many rendered frames (0 = until closed; for tests)
    std::filesystem::path screenshot; ///< If set, the last rendered frame is saved here (BMP)
};

/// Opens the annotation window and runs it until it is closed. Every change is saved
/// immediately by the session.
void run_annotator(annotation::AnnotationSession& session, media::FramePrefetcher& frames,
                   Rational fps, const AnnotatorOptions& options);

} // namespace ttrally::gui
