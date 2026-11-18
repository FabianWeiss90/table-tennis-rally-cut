// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "highlights/application/ports.hpp"

#include <filesystem>

namespace ttrally::highlights {

/// Keeps the highlights of each game in a folder (normally <match day>/ttrally-data/highlights):
/// <video id>.csv with the rallies and their marks, <video id>.settings.csv with the original
/// video (relative to the match day folder if it lies inside), the coupling and the title;
/// match-day-title.csv with the size and position of the titles in the match day video.
class CsvHighlightStore final : public HighlightStore {
  public:
    explicit CsvHighlightStore(std::filesystem::path directory);

    [[nodiscard]] std::vector<std::string> games() override;
    [[nodiscard]] std::optional<StoredHighlights> load(const std::string& video_id) override;
    void save(const std::string& video_id, const StoredHighlights& highlights) override;
    [[nodiscard]] TitleLayout match_day_layout() override;
    void save_match_day_layout(const TitleLayout& layout) override;

  private:
    [[nodiscard]] std::filesystem::path match_day() const;

    std::filesystem::path directory_;
};

} // namespace ttrally::highlights
