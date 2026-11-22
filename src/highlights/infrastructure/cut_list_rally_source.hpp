// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "highlights/application/game_highlights.hpp"

#include <filesystem>

namespace ttrally::highlights {

/// The rallies of a game from the files in a match day's work folder (annotations/, refined/,
/// detections/), chosen like `ttrally cut` does.
class CutListRallySource final : public RallySource {
  public:
    explicit CutListRallySource(std::filesystem::path work_dir);
    [[nodiscard]] std::optional<RallyList> rallies(const std::string& video_id) override;

  private:
    std::filesystem::path work_dir_;
};

/// Highlight stores and rally sources in the work folders of match days (highlights/ inside).
class FileMatchDayHighlights final : public MatchDayHighlights {
  public:
    [[nodiscard]] std::unique_ptr<HighlightStore>
    store(const std::filesystem::path& work_dir) override;
    [[nodiscard]] std::unique_ptr<RallySource>
    rallies(const std::filesystem::path& work_dir) override;
    [[nodiscard]] std::int64_t recorded_at(const std::filesystem::path& video) override;
};

} // namespace ttrally::highlights
