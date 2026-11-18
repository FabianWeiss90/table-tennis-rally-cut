// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "highlights/application/ports.hpp"
#include "media/application/media_probe.hpp"
#include "media/application/media_reader.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ttrally::highlights {

struct MakeHighlightsRequest {
    /// One game: its highlights. None: the match day highlights of every game in the store, in
    /// the order of their video ids.
    std::optional<std::string> video_id;
    std::filesystem::path output;
    cutting::Margins margins;
    /// Match day video: each clip shows the title of its game (a game video always shows it).
    bool rally_titles = false;
    RenderOptions render;
};

struct MakeHighlightsResult {
    std::size_t rallies = 0; ///< highlight rallies in the video
    std::size_t clips = 0;   ///< after merging rallies whose margins overlap
    std::vector<std::string> games; ///< video ids of the games in the video
    cutting::CutResult render;
    double seconds = 0.0; ///< run time
};

/// Use case behind `ttrally highlights`: the marked rallies of one game (game highlights) or of
/// all games of a match day (match day highlights), each with the margins, as one video with
/// cross-fades and titles. In the match day video every title has the match day's size and
/// position.
class MakeHighlightVideo {
  public:
    MakeHighlightVideo(media::MediaProbe& probe, media::MediaReader& reader, HighlightStore& store,
                       HighlightRenderer& renderer, ProgressReporter& progress);

    /// Throws std::invalid_argument (nothing marked, unknown game, rallies outside the video),
    /// media::MediaError or std::runtime_error.
    [[nodiscard]] MakeHighlightsResult execute(const MakeHighlightsRequest& request);

  private:
    /// Clips of a game's marked rallies; `match_day`: the match day video's title layout.
    [[nodiscard]] std::vector<Clip> clips_of(const StoredHighlights& game,
                                             const std::optional<TitleLayout>& match_day,
                                             const MakeHighlightsRequest& request,
                                             std::size_t& rallies);

    media::MediaProbe& probe_;
    media::MediaReader& reader_;
    HighlightStore& store_;
    HighlightRenderer& renderer_;
    ProgressReporter& progress_;
};

} // namespace ttrally::highlights
