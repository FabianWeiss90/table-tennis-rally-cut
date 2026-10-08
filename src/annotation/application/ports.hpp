// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "annotation/domain/annotation_sheet.hpp"
#include "annotation/domain/review_plan.hpp"

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace ttrally::annotation {

/// Port: persistent rally labels of a video (the label files).
class AnnotationRepository {
  public:
    virtual ~AnnotationRepository() = default;
    /// The saved rallies of a video; empty if none were saved yet.
    [[nodiscard]] virtual std::vector<RallyLabel> load(const std::string& video_id) = 0;
    virtual void save(const AnnotationSheet& sheet) = 0;
};

/// Port: candidates and gaps to review, e.g. from the output of `ttrally align`.
class ReviewItemSource {
  public:
    virtual ~ReviewItemSource() = default;
    [[nodiscard]] virtual std::vector<ReviewItem> load() = 0;
};

/// Manual review statuses, keyed by item kind and source id.
using ReviewStatusMap = std::map<std::pair<ReviewKind, int>, ReviewStatus>;

/// Port: progress of the review (which candidates were rejected, which gaps were checked).
/// Saved with all items, their frame ranges and current statuses, so that training can tell
/// whether every part of a video was looked at.
class ReviewStateStore {
  public:
    virtual ~ReviewStateStore() = default;
    /// Saved statuses; Annotated entries are recomputed from the labels by the session.
    [[nodiscard]] virtual ReviewStatusMap load(const std::string& video_id) = 0;
    virtual void save(const std::string& video_id, const std::vector<ReviewItem>& items) = 0;
};

} // namespace ttrally::annotation
