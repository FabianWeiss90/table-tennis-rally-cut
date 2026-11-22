// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "highlights/application/ports.hpp"

#include <filesystem>

namespace ttrally::highlights {

/// Keeps the highlights of each game in a folder (normally <match day>/ttrally-data/highlights):
/// <video id>.csv with the rallies and their marks, <video id>.settings.csv with the original
/// video (relative to the match day folder if it lies inside), the coupling and the players'
/// title; match-day-title.csv with the size, position and colours of the players' titles in the
/// match day video, the match title and the font of all titles.
class CsvHighlightStore final : public HighlightStore {
  public:
    explicit CsvHighlightStore(std::filesystem::path directory);

    [[nodiscard]] std::vector<std::string> games() override;
    [[nodiscard]] std::optional<StoredHighlights> load(const std::string& video_id) override;
    void save(const std::string& video_id, const StoredHighlights& highlights) override;
    [[nodiscard]] TitleLayout match_day_layout() override;
    void save_match_day_layout(const TitleLayout& layout) override;
    [[nodiscard]] std::optional<Title> match_title() override;
    void save_match_title(const Title& title) override;
    [[nodiscard]] std::filesystem::path title_font() override;
    void save_title_font(const std::filesystem::path& font) override;
    [[nodiscard]] std::vector<RallyKey> rally_order(const std::string& game) override;
    void save_rally_order(const std::string& game, const std::vector<RallyKey>& order) override;

  private:
    [[nodiscard]] std::filesystem::path match_day() const;

    std::filesystem::path directory_;
};

} // namespace ttrally::highlights
