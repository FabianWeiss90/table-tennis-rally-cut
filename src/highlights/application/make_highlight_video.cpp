// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "highlights/application/make_highlight_video.hpp"

#include "cutting/domain/resolution.hpp"
#include "cutting/application/section_planning.hpp"
#include "media/application/media_error.hpp"

#include <algorithm>
#include <chrono>
#include <map>
#include <format>
#include <stdexcept>

namespace ttrally::highlights {

MakeHighlightVideo::MakeHighlightVideo(media::MediaProbe& probe, media::MediaReader& reader,
                                       HighlightStore& store, HighlightRenderer& renderer,
                                       ProgressReporter& progress)
    : probe_(probe), reader_(reader), store_(store), renderer_(renderer), progress_(progress) {}

namespace {

/// A marked rally of a game in the order of the video.
struct OrderedRally {
    const StoredHighlights* game;
    std::string id;
    HighlightRally rally;
};

/// Runs of rallies that can share clips: one game, rising in time (margins that overlap are
/// merged within a run only, so a chosen order is kept).
std::vector<std::vector<OrderedRally>> runs_of(const std::vector<OrderedRally>& rallies) {
    std::vector<std::vector<OrderedRally>> runs;
    for (const OrderedRally& rally : rallies) {
        const bool continues = !runs.empty() && runs.back().back().id == rally.id &&
                               runs.back().back().rally.start_frame < rally.rally.start_frame;
        if (!continues) {
            runs.emplace_back();
        }
        runs.back().push_back(rally);
    }
    return runs;
}

} // namespace

MakeHighlightsResult MakeHighlightVideo::execute(const MakeHighlightsRequest& request) {
    const auto started = std::chrono::steady_clock::now();
    const bool match_day = !request.video_id.has_value();
    std::vector<std::string> ids =
        match_day ? store_.games() : std::vector<std::string>{*request.video_id};
    const std::optional<TitleLayout> layout =
        match_day ? std::optional(store_.match_day_layout()) : std::nullopt;
    const Title match =
        store_.match_title().value_or(default_match_title(request.match_day_name));
    std::map<std::string, StoredHighlights> games;
    for (const std::string& id : ids) {
        std::optional<StoredHighlights> game = store_.load(id);
        if (!game) {
            throw std::invalid_argument("no highlights are marked for " + id);
        }
        games.emplace(id, std::move(*game));
    }
    if (match_day) { // the games in the order they were recorded
        std::map<std::string, std::int64_t> recorded;
        for (const auto& [id, game] : games) {
            recorded[id] = probe_.probe(game.video).recorded_at_s;
        }
        std::ranges::stable_sort(ids, {}, [&](const std::string& id) { return recorded[id]; });
    }
    std::vector<RallyKey> chronological;
    std::map<std::pair<std::string, std::int64_t>, HighlightRally> marked;
    for (const std::string& id : ids) {
        for (const HighlightRally& rally : games.at(id).rallies) {
            if (match_day ? rally.day : rally.game) {
                chronological.push_back({id, rally.start_frame});
                marked[{id, rally.start_frame}] = rally;
            }
        }
    }
    std::vector<OrderedRally> ordered;
    for (const RallyKey& key :
         apply_order(chronological, store_.rally_order(match_day ? "" : ids.front()))) {
        ordered.push_back({&games.at(key.game), key.game, marked.at({key.game, key.start_frame})});
    }
    if (ordered.empty()) {
        throw std::invalid_argument(match_day ? "no match day highlights are marked"
                                              : "no game highlights are marked for " + ids.front());
    }
    MakeHighlightsResult result;
    result.rallies = ordered.size();
    std::vector<Clip> clips;
    std::map<std::string, GameTimeline> timelines;
    for (const auto& run : runs_of(ordered)) {
        const std::string& id = run.front().id;
        if (!timelines.contains(id)) {
            timelines.emplace(id, timeline_of(*run.front().game));
        }
        if (std::ranges::find(result.games, id) == result.games.end()) {
            result.games.push_back(id);
        }
        std::vector<HighlightRally> rallies;
        for (const OrderedRally& rally : run) {
            rallies.push_back(rally.rally);
        }
        const std::vector<Clip> own =
            clips_of(*run.front().game, rallies, timelines.at(id), layout, match, request);
        clips.insert(clips.end(), own.begin(), own.end());
    }
    progress_.report(std::format("{} rallies of {} game(s) in {} clips", result.rallies,
                                 result.games.size(), clips.size()));
    result.clips = clips.size();
    RenderOptions render = request.render;
    if (render.font.empty()) {
        render.font = store_.title_font();
    }
    result.output = request.output;
    if (request.resolution_in_name) {
        // The video takes the size of the first clip's video
        const media::MediaInfo info = probe_.probe(clips.front().video);
        result.output = cutting::with_resolution(
            request.output, cutting::output_height(info.video ? info.video->height : 0,
                                                   render.encoding.max_height));
    }
    result.render = renderer_.render(clips, result.output, render, progress_);
    result.seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return result;
}

GameTimeline MakeHighlightVideo::timeline_of(const StoredHighlights& game) {
    const media::MediaInfo info = probe_.probe(game.video);
    if (!info.video) {
        throw media::MediaError(game.video.string() + " has no video stream");
    }
    const auto timestamps =
        *reader_
             .read(game.video,
                   {.audio_sample_rate = std::nullopt, .video_stream_index = info.video->index})
             .video_timestamps;
    return {timestamps.timeline(info.video->nominal_frame_rate()), timestamps.frame_count()};
}

std::vector<Clip> MakeHighlightVideo::clips_of(const StoredHighlights& game,
                                               const std::vector<HighlightRally>& rallies,
                                               const GameTimeline& timeline,
                                               const std::optional<TitleLayout>& match_day,
                                               const Title& match,
                                               const MakeHighlightsRequest& request) {
    std::vector<cutting::FrameSpan> spans;
    for (const HighlightRally& rally : rallies) {
        spans.push_back({rally.start_frame, rally.end_frame});
    }
    std::vector<Title> titles;
    if (match.visible()) {
        titles.push_back(match);
    }
    if (game.title.visible() && (!match_day || request.rally_titles)) {
        titles.push_back(match_day ? match_day->applied_to(game.title) : game.title);
    }
    std::vector<Clip> clips;
    for (const cutting::CutSection& section :
         cutting::plan_sections(spans, request.margins, timeline.timeline, timeline.frames)) {
        clips.push_back({.video = game.video,
                         .start_s = section.start_s,
                         .end_s = section.end_s,
                         .titles = titles});
    }
    return clips;
}

} // namespace ttrally::highlights
