// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "highlights/application/ports.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace ttrally::highlights {

/// The highlights of a game as edited in the launcher.
struct GameHighlights {
    std::string video_id;
    std::filesystem::path video; ///< the original
    HighlightSelection selection;
    Title title;
    double fps = 0.0;
    std::string source; ///< where the rallies come from, e.g. "refined detections"
};

/// The highlights of a game with the current rallies of its cut list: the marks saved before
/// (carried over to changed rallies), or, the first time, the `preselect` longest rallies marked
/// and the players from the file name as title. Saved at once, so that the stored rallies always
/// match the cut list. nullopt if the game has no rallies yet.
[[nodiscard]] std::optional<GameHighlights> open_game_highlights(RallySource& rallies,
                                                                 HighlightStore& store,
                                                                 const std::string& video_id,
                                                                 const std::filesystem::path& video,
                                                                 std::size_t preselect);

void save_game_highlights(HighlightStore& store, const GameHighlights& game);

/// Port: the highlight store and rally source of a match day, by its work folder.
class MatchDayHighlights {
  public:
    virtual ~MatchDayHighlights() = default;
    [[nodiscard]] virtual std::unique_ptr<HighlightStore>
    store(const std::filesystem::path& work_dir) = 0;
    [[nodiscard]] virtual std::unique_ptr<RallySource>
    rallies(const std::filesystem::path& work_dir) = 0;
};

} // namespace ttrally::highlights
