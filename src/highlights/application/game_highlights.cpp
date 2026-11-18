// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "highlights/application/game_highlights.hpp"

namespace ttrally::highlights {

std::optional<GameHighlights> open_game_highlights(RallySource& rallies, HighlightStore& store,
                                                   const std::string& video_id,
                                                   const std::filesystem::path& video,
                                                   std::size_t preselect) {
    std::optional<RallyList> list = rallies.rallies(video_id);
    if (!list || list->rallies.empty()) {
        return std::nullopt;
    }
    GameHighlights game{.video_id = video_id, .video = video, .selection = {}, .title = {},
                        .fps = list->fps, .source = list->source};
    if (const std::optional<StoredHighlights> saved = store.load(video_id)) {
        game.selection = HighlightSelection(carry_over_marks(list->rallies, saved->rallies),
                                            saved->coupled);
        game.title = saved->title;
    } else {
        game.selection = HighlightSelection(list->rallies);
        game.selection.preselect(preselect);
        game.title.text = title_from_file_name(video.stem().string());
    }
    save_game_highlights(store, game);
    return game;
}

void save_game_highlights(HighlightStore& store, const GameHighlights& game) {
    store.save(game.video_id, {.video = game.video,
                               .rallies = game.selection.rallies(),
                               .coupled = game.selection.coupled(),
                               .title = game.title});
}

} // namespace ttrally::highlights
