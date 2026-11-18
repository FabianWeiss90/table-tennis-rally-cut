// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "gui/highlights_panel.hpp"

#include "gui/frame_texture.hpp"
#include "gui/gui_context.hpp"
#include "media/application/frame_prefetcher.hpp"
#include "shared/io/formatting.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <future>
#include <imgui.h>
#include <numeric>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace ttrally::gui {

namespace {

using highlights::GameHighlights;
using highlights::Title;
using highlights::TitlePosition;

constexpr int kDefaultPreselect = 5;
constexpr int kMaxPreselect = 100;
constexpr int kPlayerHeight = 540;          ///< decoded size of the played frames
constexpr std::size_t kPlayerFrames = 240;  ///< frames kept (about 0.8 MB each)
constexpr double kBufferAheadSeconds = 2.0;
constexpr double kFallbackFps = 30.0;
constexpr float kListShare = 0.45F;         ///< of the width, next to the player
constexpr float kListHeight = 280.0F;
constexpr float kTitleMarginShare = 0.03F;  ///< as the rendered title
constexpr float kOutlineShare = 0.07F;
constexpr float kLayoutLabelWidth = 130.0F;
constexpr int kPreviewGame = 0;
constexpr int kPreviewMatchDay = 1;
const ImVec4 kMuted{0.6F, 0.6F, 0.65F, 1.0F};
const ImVec4 kError{1.0F, 0.45F, 0.4F, 1.0F};
const ImVec4 kPlaying{0.45F, 0.75F, 1.0F, 1.0F};

const char* position_label(TitlePosition position) {
    switch (position) {
    case TitlePosition::TopLeft:
        return "Top left";
    case TitlePosition::TopCenter:
        return "Top centre";
    case TitlePosition::TopRight:
        return "Top right";
    case TitlePosition::BottomLeft:
        return "Bottom left";
    case TitlePosition::BottomCenter:
        return "Bottom centre";
    case TitlePosition::BottomRight:
        return "Bottom right";
    }
    return "";
}

/// Multi-line text input on a std::string.
bool input_multiline(const char* id, std::string& value, ImVec2 size) {
    auto resize = [](ImGuiInputTextCallbackData* data) {
        if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
            auto* text = static_cast<std::string*>(data->UserData);
            text->resize(static_cast<std::size_t>(data->BufTextLen));
            data->Buf = text->data();
        }
        return 0;
    };
    return ImGui::InputTextMultiline(id, value.data(), value.capacity() + 1, size,
                                     ImGuiInputTextFlags_CallbackResize, resize, &value);
}

std::vector<std::string> lines_of(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        lines.push_back(line);
    }
    return lines;
}

/// The title over the shown picture, placed and sized as the highlight video will show it (the
/// launcher's font, so the letters differ slightly).
void draw_title_preview(const Title& title, ImVec2 origin, ImVec2 size) {
    if (!title.visible()) {
        return;
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImFont* font = ImGui::GetFont();
    const float line = static_cast<float>(title.size) * size.y;
    const float margin = kTitleMarginShare * size.y;
    const float outline = std::max(1.0F, line * kOutlineShare * 0.5F);
    const std::vector<std::string> lines = lines_of(title.text);
    float width = 0;
    for (const std::string& text : lines) {
        width = std::max(width, font->CalcTextSizeA(line, FLT_MAX, 0, text.c_str()).x);
    }
    const float height = line * static_cast<float>(lines.size());
    const bool top = title.position == TitlePosition::TopLeft ||
                     title.position == TitlePosition::TopCenter ||
                     title.position == TitlePosition::TopRight;
    const float y0 = top ? origin.y + margin : origin.y + size.y - margin - height;
    draw->PushClipRect(origin, {origin.x + size.x, origin.y + size.y}, true);
    for (std::size_t l = 0; l < lines.size(); ++l) {
        const float w = font->CalcTextSizeA(line, FLT_MAX, 0, lines[l].c_str()).x;
        float x = origin.x + margin;
        if (title.position == TitlePosition::TopCenter ||
            title.position == TitlePosition::BottomCenter) {
            x = origin.x + (size.x - w) / 2;
        } else if (title.position == TitlePosition::TopRight ||
                   title.position == TitlePosition::BottomRight) {
            x = origin.x + size.x - margin - w;
        }
        const ImVec2 at{x, y0 + line * static_cast<float>(l)};
        for (int dx = -1; dx <= 1; ++dx) {
            for (int dy = -1; dy <= 1; ++dy) {
                if (dx != 0 || dy != 0) {
                    draw->AddText(font, line,
                                  {at.x + static_cast<float>(dx) * outline,
                                   at.y + static_cast<float>(dy) * outline},
                                  IM_COL32(0, 0, 0, 255), lines[l].c_str());
                }
            }
        }
        draw->AddText(font, line, at, IM_COL32(255, 255, 255, 255), lines[l].c_str());
    }
    draw->PopClipRect();
}

/// A game of the match day and its highlights (nullopt: no rallies yet).
struct Game {
    launcher::VideoEntry video;
    std::optional<GameHighlights> highlights;
};

/// Plays a part of a video: decodes on a background thread, opened in the background too
/// (reading the frame timestamps of a long video the first time takes a while).
class Player {
  public:
    Player(const GuiContext& gui, highlights::PlaybackSource& source)
        : source_(source), texture_(gui.renderer()) {}

    /// Plays frames first..last of the video (fps: its frame rate).
    void play(const launcher::VideoEntry& video, std::int64_t first, std::int64_t last,
              double fps) {
        if (video.original != video_) {
            video_ = video.original;
            frames_.reset();
            error_.clear();
            opening_ = std::async(std::launch::async,
                                  [&source = source_, path = std::filesystem::path(video.original),
                                   cache = std::filesystem::path(video.work_dir) / "cache"] {
                                      return source.open(path, cache, kPlayerHeight);
                                  });
        }
        fps_ = fps > 0 ? fps : kFallbackFps;
        first_ = std::max<std::int64_t>(0, first);
        last_ = std::max(first_, last);
        current_ = first_;
        clock_ = 0;
        playing_ = true;
        request();
    }

    /// Advances the playback and draws the picture with the title and the controls; with
    /// `shown` false (another game's video is loaded), only a hint.
    void draw(const Title& title, bool shown) {
        collect_opened();
        if (!shown) {
            playing_ = false;
            ImGui::TextColored(kMuted, "Click a rally's > to watch it.");
            return;
        }
        advance();
        const float width = ImGui::GetContentRegionAvail().x;
        const auto size = frames_ ? frames_->frame_size() : media::FrameSize{16, 9};
        const ImVec2 image{width, width * static_cast<float>(size.height) /
                                      static_cast<float>(std::max(1, size.width))};
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        if (frames_) {
            if (auto frame = frames_->frame(current_)) {
                texture_.show(*frame);
            }
        }
        if (frames_ && error_.empty()) {
            if (const auto failed = frames_->error()) {
                error_ = *failed;
            }
        }
        if (frames_ && error_.empty() && texture_.texture() != nullptr &&
            texture_.frame_index() >= 0) {
            ImGui::Image(ImTextureRef(reinterpret_cast<ImTextureID>(texture_.texture())), image);
            if (ImGui::IsItemClicked()) {
                toggle();
            }
            ImGui::SetItemTooltip("Click: play or pause");
        } else {
            ImGui::Dummy(image);
            ImDrawList* draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(origin, {origin.x + image.x, origin.y + image.y},
                                IM_COL32(20, 20, 24, 255));
            const char* text = !error_.empty()     ? error_.c_str()
                               : opening_.valid()  ? "Opening the video..."
                               : video_.empty()    ? "Click a rally's > to watch it"
                                                   : "Loading...";
            draw->AddText({origin.x + 10, origin.y + 10},
                          ImGui::GetColorU32(error_.empty() ? kMuted : kError), text);
        }
        draw_title_preview(title, origin, image);
        draw_controls();
    }

    [[nodiscard]] const std::string& video() const noexcept { return video_; }

  private:
    void collect_opened() {
        if (!opening_.valid() ||
            opening_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            return;
        }
        try {
            frames_ = std::make_unique<media::FramePrefetcher>(opening_.get(), kPlayerFrames);
            request();
        } catch (const std::exception& error) {
            error_ = error.what();
        }
    }

    void request() {
        if (frames_) {
            frames_->request(current_, 0,
                             static_cast<std::int64_t>(std::lround(kBufferAheadSeconds * fps_)));
        }
    }

    void advance() {
        if (!playing_ || !frames_) {
            return;
        }
        clock_ += ImGui::GetIO().DeltaTime * fps_;
        const auto steps = static_cast<std::int64_t>(clock_);
        if (steps <= 0) {
            return;
        }
        const std::int64_t target = std::min(current_ + steps, last_);
        if (frames_->frame(target)) {
            clock_ -= static_cast<double>(steps);
            current_ = target;
            request();
        } else {
            // Wait for the decoder instead of skipping frames. No new request: it would stop the
            // decoder's pass towards the frame before it gets there.
            clock_ = 0;
        }
        if (current_ >= last_) {
            playing_ = false;
        }
    }

    /// Play or pause; at the end, playing starts again from the beginning.
    void toggle() {
        if (!frames_) {
            return;
        }
        if (!playing_ && current_ >= last_) {
            current_ = first_;
        }
        playing_ = !playing_;
        clock_ = 0;
        request();
    }

    void draw_controls() {
        const bool ready = frames_ != nullptr;
        ImGui::BeginDisabled(!ready);
        if (ImGui::Button(playing_ ? "Pause" : "Play", {70, 0})) {
            toggle();
        }
        ImGui::SameLine();
        if (ImGui::Button("Again", {70, 0})) {
            current_ = first_;
            playing_ = true;
            clock_ = 0;
            request();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(std::max(80.0F, ImGui::GetContentRegionAvail().x - 90));
        auto frame = static_cast<int>(current_ - first_);
        const int length = static_cast<int>(last_ - first_);
        if (ImGui::SliderInt("##position", &frame, 0, std::max(1, length), "")) {
            current_ = first_ + frame;
            playing_ = false;
            request();
        }
        ImGui::SameLine();
        ImGui::TextColored(kMuted, "%s",
                           io::format_clock(static_cast<double>(current_) / fps_).c_str());
        ImGui::EndDisabled();
    }

    highlights::PlaybackSource& source_;
    FrameTexture texture_;
    std::string video_;
    std::future<std::unique_ptr<media::FrameDecoder>> opening_;
    std::unique_ptr<media::FramePrefetcher> frames_;
    std::string error_;
    double fps_ = kFallbackFps;
    std::int64_t first_ = 0;
    std::int64_t last_ = 0;
    std::int64_t current_ = 0;
    double clock_ = 0;
    bool playing_ = false;
};

} // namespace

struct HighlightsPanel::Impl {
    Impl(const GuiContext& gui, highlights::MatchDayHighlights& days,
         highlights::PlaybackSource& playback)
        : days(days), player(gui, playback) {}

    highlights::MatchDayHighlights& days;
    Player player;
    std::string day;  ///< match day shown
    std::string selected; ///< key of the selected video
    std::vector<Game> games;
    std::unique_ptr<highlights::HighlightStore> store;
    std::unique_ptr<highlights::RallySource> rallies;
    int preselect = kDefaultPreselect;
    highlights::TitleLayout day_layout; ///< of the titles in the match day video
    int preview = kPreviewGame;
    std::optional<std::size_t> playing; ///< rally played, in the selected game
    std::string error;

    [[nodiscard]] Game* current() {
        const auto found = std::ranges::find_if(
            games, [&](const Game& game) { return game.video.key() == selected; });
        return found == games.end() ? nullptr : &*found;
    }

    void load(const launcher::WorkspaceSnapshot& workspace, const launcher::VideoEntry& video) {
        day = video.match_day;
        games.clear();
        playing.reset();
        error.clear();
        store = days.store(video.work_dir);
        rallies = days.rallies(video.work_dir);
        try {
            day_layout = store->match_day_layout();
        } catch (const std::exception& failure) {
            error = std::format("Match day title: {}", failure.what());
        }
        for (const launcher::VideoEntry& entry : workspace.videos) {
            if (entry.match_day != day) {
                continue;
            }
            Game game{entry, std::nullopt};
            try {
                game.highlights = highlights::open_game_highlights(
                    *rallies, *store, entry.video_id, entry.original,
                    static_cast<std::size_t>(preselect));
            } catch (const std::exception& failure) {
                error = std::format("{}: {}", entry.video_id, failure.what());
            }
            games.push_back(std::move(game));
        }
    }

    void save(const GameHighlights& game) {
        try {
            highlights::save_game_highlights(*store, game);
        } catch (const std::exception& failure) {
            error = std::format("Saving the highlights of {}: {}", game.video_id, failure.what());
        }
    }

    void draw_overview() {
        std::size_t day_marks = 0;
        std::size_t with_rallies = 0;
        for (const Game& game : games) {
            if (game.highlights) {
                ++with_rallies;
                day_marks += game.highlights->selection.day_count();
            }
        }
        ImGui::Text("Match day %s: %zu match day highlights in %zu of %zu games", day.c_str(),
                    day_marks, with_rallies, games.size());
        if (with_rallies < games.size()) {
            ImGui::SetItemTooltip("Games without rallies need detect (or complete labels) first");
        }
    }

    void draw_preselection(GameHighlights& game) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Preselect the longest");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        ImGui::InputInt("##count", &preselect);
        preselect = std::clamp(preselect, 0, kMaxPreselect);
        ImGui::SameLine();
        if (ImGui::Button("This game")) {
            game.selection.preselect(static_cast<std::size_t>(preselect));
            save(game);
        }
        ImGui::SetItemTooltip("Marks the longest rallies as game and match day highlights and "
                              "removes all other marks of this game");
        ImGui::SameLine();
        if (ImGui::Button("All games")) {
            for (Game& other : games) {
                if (other.highlights) {
                    other.highlights->selection.preselect(static_cast<std::size_t>(preselect));
                    save(*other.highlights);
                }
            }
        }
        ImGui::SetItemTooltip("The same for every game of the match day");
        ImGui::SameLine();
        if (ImGui::Button("Clear")) {
            game.selection.clear();
            save(game);
        }
        ImGui::SetItemTooltip("Removes every mark of this game");
        ImGui::SameLine();
        bool coupled = game.selection.coupled();
        if (ImGui::Checkbox("Match day marks the game too", &coupled)) {
            game.selection.set_coupled(coupled);
            save(game);
        }
        ImGui::SetItemTooltip("On: a match day highlight is always a game highlight. Off: the "
                              "two marks are independent (for this game).");
    }

    /// Column ids of the rally list, for sorting.
    enum RallyColumn : ImGuiID {
        kNumberColumn,
        kStartColumn,
        kLengthColumn,
        kGameColumn,
        kDayColumn,
        kRallyColumns = 6,
    };

    /// Indices of the rallies in the order the clicked column header asks for (by time when
    /// unsorted; ties keep the time order).
    static std::vector<std::size_t> sorted_rallies(
        const std::vector<highlights::HighlightRally>& rallies, const ImGuiTableSortSpecs* specs) {
        std::vector<std::size_t> order(rallies.size());
        std::iota(order.begin(), order.end(), std::size_t{0});
        if (specs == nullptr || specs->SpecsCount == 0) {
            return order;
        }
        const ImGuiTableColumnSortSpecs& spec = specs->Specs[0];
        const auto key = [&](std::size_t i) -> std::int64_t {
            const highlights::HighlightRally& rally = rallies[i];
            switch (spec.ColumnUserID) {
            case kLengthColumn:
                return rally.frame_count();
            case kGameColumn:
                return rally.game ? 1 : 0;
            case kDayColumn:
                return rally.day ? 1 : 0;
            default:
                return static_cast<std::int64_t>(i); // # and Start: by time
            }
        };
        const bool descending = spec.SortDirection == ImGuiSortDirection_Descending;
        std::ranges::stable_sort(order, [&](std::size_t a, std::size_t b) {
            return descending ? key(a) > key(b) : key(a) < key(b);
        });
        return order;
    }

    void draw_rallies(Game& game, cutting::Margins margins) {
        GameHighlights& marks = *game.highlights;
        const double fps = marks.fps > 0 ? marks.fps : kFallbackFps;
        ImGui::TextColored(kMuted, "%zu rallies from the %s; %zu game highlights, %zu match day",
                           marks.selection.rallies().size(), marks.source.c_str(),
                           marks.selection.game_count(), marks.selection.day_count());
        constexpr ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                          ImGuiTableFlags_BordersInnerV |
                                          ImGuiTableFlags_SizingFixedFit |
                                          ImGuiTableFlags_Sortable;
        if (!ImGui::BeginTable("##rallies", kRallyColumns, flags, {0, kListHeight})) {
            return;
        }
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_DefaultSort, 0.0F, kNumberColumn);
        ImGui::TableSetupColumn("Start", ImGuiTableColumnFlags_None, 0.0F, kStartColumn);
        ImGui::TableSetupColumn("Length", ImGuiTableColumnFlags_PreferSortDescending, 0.0F,
                                kLengthColumn);
        ImGui::TableSetupColumn("Game", ImGuiTableColumnFlags_PreferSortDescending, 0.0F,
                                kGameColumn);
        ImGui::TableSetupColumn("Day", ImGuiTableColumnFlags_PreferSortDescending, 0.0F,
                                kDayColumn);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch |
                                        ImGuiTableColumnFlags_NoSort);
        ImGui::TableHeadersRow();
        const auto& rallies = marks.selection.rallies();
        for (const std::size_t i : sorted_rallies(rallies, ImGui::TableGetSortSpecs())) {
            const highlights::HighlightRally& rally = rallies[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            const bool is_playing = playing == i;
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            if (is_playing) {
                ImGui::TextColored(kPlaying, "%zu", i + 1);
            } else {
                ImGui::Text("%zu", i + 1);
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(
                io::format_clock(static_cast<double>(rally.start_frame) / fps).c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%.1f s", static_cast<double>(rally.frame_count()) / fps);
            ImGui::TableNextColumn();
            bool game_mark = rally.game;
            if (ImGui::Checkbox("##game", &game_mark)) {
                marks.selection.set_game(i, game_mark);
                save(marks);
            }
            ImGui::SetItemTooltip("Game highlight");
            ImGui::TableNextColumn();
            bool day_mark = rally.day;
            if (ImGui::Checkbox("##day", &day_mark)) {
                marks.selection.set_day(i, day_mark);
                save(marks);
            }
            ImGui::SetItemTooltip("Match day highlight");
            ImGui::TableNextColumn();
            if (ImGui::SmallButton(">")) {
                playing = i;
                const auto before = static_cast<std::int64_t>(std::lround(margins.before_s * fps));
                const auto after = static_cast<std::int64_t>(std::lround(margins.after_s * fps));
                player.play(game.video, rally.start_frame - before, rally.end_frame + after, fps);
            }
            ImGui::SetItemTooltip("Watch it (with the margins before and after)");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    /// Size and position; true when a change is to be saved (the size when released).
    static bool draw_layout(const char* label, double& size, TitlePosition& position) {
        ImGui::PushID(label);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(kLayoutLabelWidth);
        ImGui::SetNextItemWidth(140);
        auto percent = static_cast<float>(size * 100);
        if (ImGui::SliderFloat("Size", &percent, static_cast<float>(Title::kMinSize * 100),
                               static_cast<float>(Title::kMaxSize * 100), "%.1f %%")) {
            size = percent / 100.0;
        }
        bool save = ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SetItemTooltip("Height of a line, as share of the picture height");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140);
        if (ImGui::BeginCombo("Position", position_label(position))) {
            for (const TitlePosition choice : highlights::kTitlePositions) {
                if (ImGui::Selectable(position_label(choice), choice == position)) {
                    position = choice;
                    save = true;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::PopID();
        return save;
    }

    void draw_title(GameHighlights& game) {
        Title& title = game.title;
        ImGui::SeparatorText("Title");
        if (ImGui::Checkbox("Show", &title.shown)) {
            save(game);
        }
        ImGui::SetItemTooltip("Shown over the whole game video; in the match day video if "
                              "\"Players on each rally\" is ticked");
        ImGui::SameLine();
        if (ImGui::Button("Swap players")) {
            title.text = highlights::swap_players(title.text);
            save(game);
        }
        ImGui::SetItemTooltip("\"A - B\" in the first line becomes \"B - A\"");
        ImGui::SameLine();
        ImGui::TextUnformatted("   Preview as");
        ImGui::SameLine();
        ImGui::RadioButton("game video", &preview, kPreviewGame);
        ImGui::SameLine();
        ImGui::RadioButton("match day video", &preview, kPreviewMatchDay);
        if (draw_layout("Game video", title.size, title.position)) {
            save(game);
        }
        if (draw_layout("Match day video", day_layout.size, day_layout.position)) {
            save_day_layout();
        }
        ImGui::SameLine();
        ImGui::TextColored(kMuted, "(the same for every game)");
        input_multiline("##text", title.text, {-FLT_MIN, ImGui::GetTextLineHeight() * 3.5F});
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            save(game);
        }
        ImGui::SetItemTooltip("Several lines are possible (Enter)");
    }

    void save_day_layout() {
        try {
            store->save_match_day_layout(day_layout);
        } catch (const std::exception& failure) {
            error = std::format("Saving the match day title: {}", failure.what());
        }
    }

    /// The title as the player shows it: in the game's or the match day's layout.
    [[nodiscard]] Title previewed(const GameHighlights& game) const {
        return preview == kPreviewMatchDay ? day_layout.applied_to(game.title) : game.title;
    }
};

HighlightsPanel::HighlightsPanel(const GuiContext& gui, highlights::MatchDayHighlights& days,
                                 highlights::PlaybackSource& playback)
    : impl_(std::make_unique<Impl>(gui, days, playback)) {}

HighlightsPanel::~HighlightsPanel() = default;

void HighlightsPanel::draw(const launcher::WorkspaceSnapshot& workspace,
                           const launcher::VideoEntry& video, bool reload,
                           cutting::Margins margins) {
    Impl& self = *impl_;
    if (reload || video.match_day != self.day || !self.store) {
        self.load(workspace, video);
    }
    if (video.key() != self.selected) {
        self.selected = video.key();
        self.playing.reset();
    }
    self.draw_overview();
    ImGui::SameLine();
    if (ImGui::SmallButton("Reload")) {
        self.load(workspace, video);
    }
    ImGui::SetItemTooltip("Read the rallies again (e.g. after detect or refine)");
    if (!self.error.empty()) {
        ImGui::TextColored(kError, "%s", self.error.c_str());
    }
    Game* game = self.current();
    if (game == nullptr || !game->highlights) {
        ImGui::TextColored(kMuted, "%s has no rallies yet: run detect (or annotate it completely).",
                           video.video_id.c_str());
        return;
    }
    ImGui::SeparatorText(std::format("Game {}", video.video_id).c_str());
    self.draw_preselection(*game->highlights);
    if (ImGui::BeginTable("##layout", 2, ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("list", ImGuiTableColumnFlags_WidthStretch, kListShare);
        ImGui::TableSetupColumn("player", ImGuiTableColumnFlags_WidthStretch, 1.0F - kListShare);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        self.draw_rallies(*game, margins);
        ImGui::TableNextColumn();
        self.player.draw(self.previewed(*game->highlights),
                         self.player.video() == game->video.original);
        ImGui::EndTable();
    }
    self.draw_title(*game->highlights);
}

HighlightCounts HighlightsPanel::counts() const {
    HighlightCounts counts;
    for (const Game& game : impl_->games) {
        if (!game.highlights) {
            continue;
        }
        counts.match_day += game.highlights->selection.day_count();
        if (game.video.key() == impl_->selected) {
            counts.game_loaded = true;
            counts.game = game.highlights->selection.game_count();
        }
    }
    return counts;
}

} // namespace ttrally::gui
