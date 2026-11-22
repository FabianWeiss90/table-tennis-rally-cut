// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "highlights/application/ports.hpp"
#include "media/application/media_probe.hpp"
#include "media/application/media_reader.hpp"
#include "shared/kernel/frame_timeline.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ttrally::highlights {

/// The frame times of a game's video.
struct GameTimeline {
    FrameTimeline timeline;
    std::int64_t frames = 0;
};

struct MakeHighlightsRequest {
    /// One game: its highlights. None: the match day highlights of every game in the store.
    std::optional<std::string> video_id;
    std::filesystem::path output;
    /// Name of the match day folder, for the match title if none was saved.
    std::string match_day_name;
    cutting::Margins margins;
    /// Match day video: each clip shows the title of its game (a game video always shows it).
    bool rally_titles = false;
    RenderOptions render;
    /// The height of the written video is added to the output's name ("game-1080p.mp4").
    bool resolution_in_name = false;
};

struct MakeHighlightsResult {
    std::filesystem::path output; ///< the written video
    std::size_t rallies = 0; ///< highlight rallies in the video
    std::size_t clips = 0;   ///< after merging rallies whose margins overlap
    std::vector<std::string> games; ///< video ids of the games in the video
    cutting::CutResult render;
    double seconds = 0.0; ///< run time
};

/// Use case behind `ttrally highlights`: the marked rallies of one game (game highlights) or of
/// all games of a match day (match day highlights; the games in the order they were recorded),
/// in the order chosen in the launcher (else by time), each with the margins, as one video with
/// cross-fades and titles: the match over every clip, and the players (always in a game video,
/// on request in the match day video, there with the match day's size, position and colours).
class MakeHighlightVideo {
  public:
    MakeHighlightVideo(media::MediaProbe& probe, media::MediaReader& reader, HighlightStore& store,
                       HighlightRenderer& renderer, ProgressReporter& progress);

    /// Throws std::invalid_argument (nothing marked, unknown game, rallies outside the video),
    /// media::MediaError or std::runtime_error.
    [[nodiscard]] MakeHighlightsResult execute(const MakeHighlightsRequest& request);

  private:
    /// The frame times of a game's video.
    [[nodiscard]] GameTimeline timeline_of(const StoredHighlights& game);
    /// Clips of rallies of a game (sorted by time; overlapping margins merged);
    /// `match_day`: the match day video's title layout; `match`: the match title.
    [[nodiscard]] std::vector<Clip> clips_of(const StoredHighlights& game,
                                             const std::vector<HighlightRally>& rallies,
                                             const GameTimeline& timeline,
                                             const std::optional<TitleLayout>& match_day,
                                             const Title& match,
                                             const MakeHighlightsRequest& request);

    media::MediaProbe& probe_;
    media::MediaReader& reader_;
    HighlightStore& store_;
    HighlightRenderer& renderer_;
    ProgressReporter& progress_;
};

} // namespace ttrally::highlights
