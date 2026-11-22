// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace ttrally::highlights {

/// A short part of a rally shown again slowed down, right after the rally's clip, optionally
/// zoomed into a section of the picture (cut from the full-size original, then fitted to the
/// output). Frames of the original, end inclusive.
struct SlowMotion {
    static constexpr int kMinFactor = 2;
    static constexpr int kMaxFactor = 4;

    std::int64_t start_frame = 0;
    std::int64_t end_frame = 0;
    int factor = kMinFactor; ///< times slower; every frame is shown this many times
    /// The section, as shares of the picture's width and height (the picture's shape): its left
    /// and top edge and its size; size 1 is the whole picture.
    double left = 0.0;
    double top = 0.0;
    double size = 1.0;
    bool sound = false; ///< the sound stretched (same pitch); else silence

    [[nodiscard]] std::int64_t frame_count() const noexcept { return end_frame - start_frame + 1; }
    friend bool operator==(const SlowMotion&, const SlowMotion&) = default;
};

/// A rally of a game with its highlight marks; frames of the original, end inclusive.
struct HighlightRally {
    std::int64_t start_frame = 0;
    std::int64_t end_frame = 0;
    bool game = false; ///< in the game's highlight video
    bool day = false;  ///< in the match day's highlight video
    std::optional<SlowMotion> slow{}; ///< shown after the rally in both videos

    [[nodiscard]] std::int64_t frame_count() const noexcept { return end_frame - start_frame + 1; }
    friend bool operator==(const HighlightRally&, const HighlightRally&) = default;
};

/// The highlight marks of one game (one video), rallies sorted by start.
///
/// While coupled (the default), a match day highlight is always a game highlight too: marking
/// one for the match day marks it for the game, and unmarking a game highlight unmarks it for
/// the match day. Uncoupled, the two marks are independent.
class HighlightSelection {
  public:
    HighlightSelection() = default;
    explicit HighlightSelection(std::vector<HighlightRally> rallies, bool coupled = true);

    [[nodiscard]] const std::vector<HighlightRally>& rallies() const noexcept { return rallies_; }
    [[nodiscard]] bool coupled() const noexcept { return coupled_; }
    /// Coupling again marks every match day highlight as game highlight.
    void set_coupled(bool coupled);

    /// Throws std::out_of_range for an invalid index.
    void set_game(std::size_t index, bool marked);
    void set_day(std::size_t index, bool marked);

    /// Gives a rally a slow motion (nullopt: none). Throws std::out_of_range for an invalid index.
    void set_slow_motion(std::size_t index, std::optional<SlowMotion> slow);

    /// Marks the `count` longest rallies (equal lengths: the earlier first) as game and match day
    /// highlights and removes all other marks.
    void preselect(std::size_t count);
    /// Marks the `count` longest rallies (equal lengths: the earlier first) as match day
    /// highlights and unmarks the others for the match day; game marks stay (while coupled, the
    /// marked rallies become game highlights too).
    void mark_longest_for_day(std::size_t count);
    /// Removes all marks.
    void clear();

    [[nodiscard]] std::size_t game_count() const noexcept;
    [[nodiscard]] std::size_t day_count() const noexcept;

  private:
    /// Indices of the rallies from the longest down; equal lengths: the earlier first.
    [[nodiscard]] std::vector<std::size_t> by_length() const;

    std::vector<HighlightRally> rallies_;
    bool coupled_ = true;
};

/// The rallies of the current cut list with the marks saved for an earlier one (e.g. before the
/// detections were refined): a rally takes the marks of the saved rally it shares the most
/// frames with, if those cover at least half of the shorter of the two.
[[nodiscard]] std::vector<HighlightRally>
carry_over_marks(std::vector<HighlightRally> rallies, const std::vector<HighlightRally>& saved);

} // namespace ttrally::highlights
