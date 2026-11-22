// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "cutting/application/video_cutter.hpp"
#include "highlights/domain/highlight_order.hpp"
#include "highlights/domain/highlight_selection.hpp"
#include "highlights/domain/title.hpp"
#include "shared/application/progress_reporter.hpp"

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ttrally::highlights {

/// What is kept for the highlights of a game.
struct StoredHighlights {
    std::filesystem::path video; ///< the original (absolute)
    std::vector<HighlightRally> rallies;
    bool coupled = true;
    Title title;
};

/// Port: the highlight marks of the games of a match day.
class HighlightStore {
  public:
    virtual ~HighlightStore() = default;
    /// Video ids of the games with stored highlights, sorted.
    [[nodiscard]] virtual std::vector<std::string> games() = 0;
    [[nodiscard]] virtual std::optional<StoredHighlights> load(const std::string& video_id) = 0;
    virtual void save(const std::string& video_id, const StoredHighlights& highlights) = 0;
    /// Size and position of the titles in the match day video, the same for every game (each
    /// game's own ones apply to its game video); the defaults if none were saved.
    [[nodiscard]] virtual TitleLayout match_day_layout() = 0;
    virtual void save_match_day_layout(const TitleLayout& layout) = 0;
    /// The match (e.g. "Team A - Team B"), shown in every highlight video of the match day;
    /// nullopt if none was saved (then default_match_title() of the folder applies).
    [[nodiscard]] virtual std::optional<Title> match_title() = 0;
    virtual void save_match_title(const Title& title) = 0;
    /// The font of all titles of the match day; empty: the default font.
    [[nodiscard]] virtual std::filesystem::path title_font() = 0;
    virtual void save_title_font(const std::filesystem::path& font) = 0;
    /// The order of the rallies chosen for a highlight video: `game` is the game's video id for
    /// its game video, empty for the match day video. Empty if none was chosen (time order).
    [[nodiscard]] virtual std::vector<RallyKey> rally_order(const std::string& game) = 0;
    virtual void save_rally_order(const std::string& game, const std::vector<RallyKey>& order) = 0;
};

/// The rallies a game's highlights are chosen from.
struct RallyList {
    std::vector<HighlightRally> rallies; ///< without marks
    double fps = 0.0;                    ///< frame rate of the original
    std::string source;                  ///< e.g. "complete labels", "refined detections"
};

/// Port: the rallies of a game, from the same cut list `cut` uses (complete labels, else the
/// refined detections, else the detections).
class RallySource {
  public:
    virtual ~RallySource() = default;
    /// nullopt if the game has neither labels nor detections.
    [[nodiscard]] virtual std::optional<RallyList> rallies(const std::string& video_id) = 0;
};

/// How a slow motion clip is shown: every frame `factor` times, and only the section of the
/// picture (shares of its width and height: left and top edge, size; cut from the full-size
/// picture, then fitted to the output).
struct SlowPlayback {
    int factor = SlowMotion::kMinFactor;
    double left = 0.0;
    double top = 0.0;
    double size = 1.0;
    bool sound = false; ///< the sound stretched to the slower pace (same pitch); else silence
};

/// A part of a video that goes into a highlight video.
struct Clip {
    std::filesystem::path video;
    double start_s = 0.0; ///< on the video's timeline
    double end_s = 0.0;
    std::vector<Title> titles; ///< drawn over the whole clip (those visible), in this order
    std::optional<SlowPlayback> slow{}; ///< a slow motion clip; lasts `factor` times as long

    /// Seconds the clip lasts in the highlight video.
    [[nodiscard]] double shown_s() const noexcept {
        return (end_s - start_s) * (slow ? slow->factor : 1);
    }
};

struct RenderOptions {
    double crossfade_s = 0.5; ///< overlap of consecutive clips; 0: hard cuts
    cutting::CutOptions encoding; ///< codec, encoder and bit rate (the mode is ignored)
    std::filesystem::path font;   ///< TrueType font for titles; empty: the match day's, else a
                                  ///< system font
    std::filesystem::path shapes{}; ///< folder of the SVG shapes of title backdrops
};

/// Port: writes clips (possibly from different videos) one after the other into a video, with
/// cross-fades and titles. The output has the size and frame rate of the first clip's video.
class HighlightRenderer {
  public:
    virtual ~HighlightRenderer() = default;
    /// Throws media::MediaError or std::runtime_error (e.g. no font for a title).
    [[nodiscard]] virtual cutting::CutResult render(std::span<const Clip> clips,
                                                    const std::filesystem::path& output,
                                                    const RenderOptions& options,
                                                    ProgressReporter& progress) = 0;
};

} // namespace ttrally::highlights
