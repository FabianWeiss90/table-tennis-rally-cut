// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "highlights/application/make_highlight_video.hpp"

#include "cutting/application/section_planning.hpp"
#include "media/application/media_error.hpp"

#include <chrono>
#include <format>
#include <stdexcept>

namespace ttrally::highlights {

MakeHighlightVideo::MakeHighlightVideo(media::MediaProbe& probe, media::MediaReader& reader,
                                       HighlightStore& store, HighlightRenderer& renderer,
                                       ProgressReporter& progress)
    : probe_(probe), reader_(reader), store_(store), renderer_(renderer), progress_(progress) {}

MakeHighlightsResult MakeHighlightVideo::execute(const MakeHighlightsRequest& request) {
    const auto started = std::chrono::steady_clock::now();
    const bool match_day = !request.video_id.has_value();
    const std::vector<std::string> ids =
        match_day ? store_.games() : std::vector<std::string>{*request.video_id};
    const std::optional<TitleLayout> layout =
        match_day ? std::optional(store_.match_day_layout()) : std::nullopt;
    const Title match =
        store_.match_title().value_or(default_match_title(request.match_day_name));
    MakeHighlightsResult result;
    std::vector<Clip> clips;
    for (const std::string& id : ids) {
        const std::optional<StoredHighlights> game = store_.load(id);
        if (!game) {
            throw std::invalid_argument("no highlights are marked for " + id);
        }
        std::size_t rallies = 0;
        std::vector<Clip> game_clips = clips_of(*game, layout, match, request, rallies);
        if (game_clips.empty()) {
            continue;
        }
        progress_.report(std::format("{}: {} rallies in {} clips", id, rallies, game_clips.size()));
        result.rallies += rallies;
        result.games.push_back(id);
        clips.insert(clips.end(), game_clips.begin(), game_clips.end());
    }
    if (clips.empty()) {
        throw std::invalid_argument(match_day ? "no match day highlights are marked"
                                              : "no game highlights are marked for " + ids.front());
    }
    result.clips = clips.size();
    RenderOptions render = request.render;
    if (render.font.empty()) {
        render.font = store_.title_font();
    }
    result.render = renderer_.render(clips, request.output, render, progress_);
    result.seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return result;
}

std::vector<Clip> MakeHighlightVideo::clips_of(const StoredHighlights& game,
                                               const std::optional<TitleLayout>& match_day,
                                               const Title& match,
                                               const MakeHighlightsRequest& request,
                                               std::size_t& rallies) {
    std::vector<cutting::FrameSpan> marked;
    for (const HighlightRally& rally : game.rallies) {
        if (match_day ? rally.day : rally.game) {
            marked.push_back({rally.start_frame, rally.end_frame});
        }
    }
    rallies = marked.size();
    if (marked.empty()) {
        return {};
    }
    const media::MediaInfo info = probe_.probe(game.video);
    if (!info.video) {
        throw media::MediaError(game.video.string() + " has no video stream");
    }
    const auto timestamps =
        *reader_
             .read(game.video,
                   {.audio_sample_rate = std::nullopt, .video_stream_index = info.video->index})
             .video_timestamps;
    const FrameTimeline timeline = timestamps.timeline(info.video->nominal_frame_rate());
    std::vector<Title> titles;
    if (match.visible()) {
        titles.push_back(match);
    }
    if (game.title.visible() && (!match_day || request.rally_titles)) {
        titles.push_back(match_day ? match_day->applied_to(game.title) : game.title);
    }
    std::vector<Clip> clips;
    for (const cutting::CutSection& section :
         cutting::plan_sections(marked, request.margins, timeline, timestamps.frame_count())) {
        clips.push_back({.video = game.video,
                         .start_s = section.start_s,
                         .end_s = section.end_s,
                         .titles = titles});
    }
    return clips;
}

} // namespace ttrally::highlights
