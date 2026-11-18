// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "gui/highlights_panel.hpp"

#include "gui/frame_texture.hpp"
#include "gui/gui_context.hpp"
#include "highlights/application/title_fonts.hpp"
#include "media/application/frame_prefetcher.hpp"
#include "shared/io/formatting.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <functional>
#include <future>
#include <imgui.h>
#include <map>
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
constexpr float kEnlargedShare = 0.85F;     ///< of the window, for the enlarged preview
constexpr float kControlsHeight = 40.0F;    ///< below the enlarged picture
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

/// Titles over the shown picture, written in the chosen font and placed, sized and coloured as
/// the highlight video will show them.
class TitleOverlay {
  public:
    TitleOverlay(SDL_Renderer* renderer, highlights::TitleFonts& fonts)
        : renderer_(renderer), fonts_(fonts) {}
    ~TitleOverlay() { clear(); }
    TitleOverlay(const TitleOverlay&) = delete;
    TitleOverlay& operator=(const TitleOverlay&) = delete;

    void draw(const std::vector<Title>& titles, const std::filesystem::path& font, ImVec2 origin,
              ImVec2 size) {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->PushClipRect(origin, {origin.x + size.x, origin.y + size.y}, true);
        for (const Title& title : titles) {
            if (title.visible()) {
                draw_one(*draw, title, font, origin, size);
            }
        }
        draw->PopClipRect();
    }

    /// Why the last title could not be drawn (e.g. an unreadable font); empty if it could.
    [[nodiscard]] const std::string& error() const noexcept { return error_; }

  private:
    struct Image {
        SDL_Texture* texture = nullptr;
        int width = 0;
        int height = 0;
    };

    void draw_one(ImDrawList& draw, const Title& title, const std::filesystem::path& font,
                  ImVec2 origin, ImVec2 size) {
        const int line_height =
            std::max(1, static_cast<int>(std::lround(title.size * size.y)));
        const Image* image = image_of(title, font, line_height);
        if (image == nullptr) {
            return;
        }
        const float margin = std::round(kTitleMarginShare * size.y);
        const auto width = static_cast<float>(image->width);
        const auto height = static_cast<float>(image->height);
        float x = origin.x + margin;
        if (title.position == TitlePosition::TopCenter ||
            title.position == TitlePosition::BottomCenter) {
            x = origin.x + std::floor((size.x - width) / 2);
        } else if (title.position == TitlePosition::TopRight ||
                   title.position == TitlePosition::BottomRight) {
            x = origin.x + size.x - margin - width;
        }
        const bool top = title.position == TitlePosition::TopLeft ||
                         title.position == TitlePosition::TopCenter ||
                         title.position == TitlePosition::TopRight;
        const float y = top ? origin.y + margin : origin.y + size.y - margin - height;
        draw.AddImage(ImTextureRef(reinterpret_cast<ImTextureID>(image->texture)), {x, y},
                      {x + width, y + height});
    }

    const Image* image_of(const Title& title, const std::filesystem::path& font,
                          int line_height) {
        const std::string key =
            std::format("{}|{}|{}|{}|{}|{}|{}", font.string(), title.text, line_height,
                        highlights::to_hex(title.color), title.outlined,
                        highlights::to_hex(title.outline), highlights::to_string(title.position));
        if (const auto found = images_.find(key); found != images_.end()) {
            return &found->second;
        }
        if (images_.size() >= kMaxImages) {
            clear();
        }
        highlights::TitleImage mask;
        try {
            mask = fonts_.render(font, title, line_height);
            error_.clear();
        } catch (const std::exception& failure) {
            error_ = failure.what();
            return nullptr;
        }
        Image image{.texture = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32,
                                                 SDL_TEXTUREACCESS_STATIC, mask.width,
                                                 mask.height),
                    .width = mask.width,
                    .height = mask.height};
        if (image.texture == nullptr) {
            error_ = SDL_GetError();
            return nullptr;
        }
        SDL_SetTextureBlendMode(image.texture, SDL_BLENDMODE_BLEND);
        const std::vector<std::uint8_t> pixels = rgba_of(mask, title);
        SDL_UpdateTexture(image.texture, nullptr, pixels.data(), mask.width * 4);
        return &images_.emplace(key, image).first->second;
    }

    /// Letters over their border, with straight (not premultiplied) alpha.
    static std::vector<std::uint8_t> rgba_of(const highlights::TitleImage& mask,
                                             const Title& title) {
        constexpr float kFull = 255.0F;
        std::vector<std::uint8_t> pixels(mask.text.size() * 4);
        for (std::size_t i = 0; i < mask.text.size(); ++i) {
            const float text = mask.text[i] / kFull;
            const float border = title.outlined ? mask.outline[i] / kFull : 0.0F;
            const float alpha = text + border * (1 - text);
            const auto channel = [&](std::uint8_t letter, std::uint8_t edge) {
                return alpha <= 0 ? std::uint8_t{0}
                                  : static_cast<std::uint8_t>(std::lround(
                                        (letter * text + edge * border * (1 - text)) / alpha));
            };
            pixels[4 * i] = channel(title.color.r, title.outline.r);
            pixels[4 * i + 1] = channel(title.color.g, title.outline.g);
            pixels[4 * i + 2] = channel(title.color.b, title.outline.b);
            pixels[4 * i + 3] = static_cast<std::uint8_t>(std::lround(alpha * kFull));
        }
        return pixels;
    }

    void clear() {
        for (auto& [key, image] : images_) {
            SDL_DestroyTexture(image.texture);
        }
        images_.clear();
    }

    static constexpr std::size_t kMaxImages = 64;
    SDL_Renderer* renderer_;
    highlights::TitleFonts& fonts_;
    std::map<std::string, Image> images_;
    std::string error_;
};

/// Draws titles over a picture of the given position and size.
using Overlay = std::function<void(ImVec2 origin, ImVec2 size)>;

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

    /// Advances the playback and draws the picture with the titles and the controls, and the
    /// enlarged preview if it is open; with `shown` false (another game's video is loaded),
    /// only the titles on a dark picture.
    void draw(const Overlay& overlay, bool shown) {
        collect_opened();
        if (!shown) {
            playing_ = false;
        }
        advance();
        const float width = ImGui::GetContentRegionAvail().x;
        draw_picture(overlay, {width, width / aspect()}, shown);
        draw_controls(shown);
        if (enlarged_) {
            draw_enlarged(overlay, shown);
        }
    }

    [[nodiscard]] const std::string& video() const noexcept { return video_; }

  private:
    [[nodiscard]] float aspect() const {
        const auto size = frames_ ? frames_->frame_size() : media::FrameSize{16, 9};
        return static_cast<float>(size.width) / static_cast<float>(std::max(1, size.height));
    }

    void draw_picture(const Overlay& overlay, ImVec2 image, bool shown) {
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        if (shown && frames_) {
            if (auto frame = frames_->frame(current_)) {
                texture_.show(*frame);
            }
            if (error_.empty()) {
                if (const auto failed = frames_->error()) {
                    error_ = *failed;
                }
            }
        }
        if (shown && frames_ && error_.empty() && texture_.texture() != nullptr &&
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
            const char* text = !shown                ? "Click a rally's > to watch it"
                               : !error_.empty()     ? error_.c_str()
                               : opening_.valid()    ? "Opening the video..."
                               : video_.empty()      ? "Click a rally's > to watch it"
                                                     : "Loading...";
            draw->AddText({origin.x + 10, origin.y + image.y / 2},
                          ImGui::GetColorU32(error_.empty() || !shown ? kMuted : kError), text);
        }
        overlay(origin, image);
    }

    /// The picture as large as the window allows, in a window of its own.
    void draw_enlarged(const Overlay& overlay, bool shown) {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowSize(
            {viewport->WorkSize.x * kEnlargedShare, viewport->WorkSize.y * kEnlargedShare},
            ImGuiCond_Appearing);
        ImGui::SetNextWindowPos({viewport->WorkPos.x + viewport->WorkSize.x / 2,
                                 viewport->WorkPos.y + viewport->WorkSize.y / 2},
                                ImGuiCond_Appearing, {0.5F, 0.5F});
        if (ImGui::Begin("Preview##enlarged", &enlarged_, ImGuiWindowFlags_NoCollapse)) {
            const ImVec2 room = ImGui::GetContentRegionAvail();
            const float height = std::max(1.0F, room.y - kControlsHeight);
            ImVec2 image{room.x, room.x / aspect()};
            if (image.y > height) {
                image = {height * aspect(), height};
            }
            ImGui::PushID("enlarged");
            draw_picture(overlay, image, shown);
            draw_controls(shown);
            ImGui::PopID();
        }
        ImGui::End();
    }

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

    void draw_controls(bool shown) {
        const bool ready = frames_ != nullptr && shown;
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
        ImGui::SetNextItemWidth(std::max(80.0F, ImGui::GetContentRegionAvail().x - 190));
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
        ImGui::SameLine();
        if (ImGui::Button(enlarged_ ? "Shrink" : "Enlarge")) {
            enlarged_ = !enlarged_;
        }
        ImGui::SetItemTooltip("The preview in a large window of its own");
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
    bool enlarged_ = false;
};

} // namespace

struct HighlightsPanel::Impl {
    Impl(const GuiContext& gui, highlights::MatchDayHighlights& days,
         highlights::PlaybackSource& playback, highlights::TitleFonts& fonts)
        : days(days), player(gui, playback), fonts(fonts), overlay(gui.renderer(), fonts) {}
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;
    ~Impl() { save_pending(true); }

    highlights::MatchDayHighlights& days;
    Player player;
    highlights::TitleFonts& fonts;
    TitleOverlay overlay;
    std::filesystem::path font; ///< of all titles of the match day; empty: the fallback
    highlights::FontChoice fallback;
    std::future<std::vector<highlights::FontChoice>> font_scan;
    std::optional<std::vector<highlights::FontChoice>> installed;
    std::optional<std::filesystem::path> hovered_font; ///< previewed while choosing
    ImGuiTextFilter font_filter;
    std::string day;  ///< match day shown
    std::string selected; ///< key of the selected video
    std::vector<Game> games;
    std::unique_ptr<highlights::HighlightStore> store;
    std::unique_ptr<highlights::RallySource> rallies;
    int preselect = kDefaultPreselect;
    highlights::TitleLayout day_layout; ///< of the players' titles in the match day video
    Title match;                        ///< the match, over every clip
    // Changes saved once no control is in use any more (a colour picker changes all the time)
    std::string pending_game; ///< video id of a game whose title changed
    bool pending_layout = false;
    bool pending_match = false;
    int preview = kPreviewGame;
    std::optional<std::size_t> playing; ///< rally played, in the selected game
    std::string error;

    [[nodiscard]] Game* current() {
        const auto found = std::ranges::find_if(
            games, [&](const Game& game) { return game.video.key() == selected; });
        return found == games.end() ? nullptr : &*found;
    }

    void load(const launcher::WorkspaceSnapshot& workspace, const launcher::VideoEntry& video) {
        save_pending(true);
        day = video.match_day;
        games.clear();
        playing.reset();
        error.clear();
        store = days.store(video.work_dir);
        rallies = days.rallies(video.work_dir);
        try {
            day_layout = store->match_day_layout();
            match = store->match_title().value_or(highlights::default_match_title(day));
            font = store->title_font();
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

    /// Size, position and colours of a title; true if one changed.
    static bool draw_layout(const char* label, double& size, TitlePosition& position,
                            highlights::Rgb& color, bool& outlined, highlights::Rgb& outline) {
        ImGui::PushID(label);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(kLayoutLabelWidth);
        ImGui::SetNextItemWidth(140);
        auto percent = static_cast<float>(size * 100);
        bool changed = false;
        if (ImGui::SliderFloat("Size", &percent, static_cast<float>(Title::kMinSize * 100),
                               static_cast<float>(Title::kMaxSize * 100), "%.1f %%")) {
            size = percent / 100.0;
            changed = true;
        }
        ImGui::SetItemTooltip("Height of a line, as share of the picture height");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140);
        if (ImGui::BeginCombo("Position", position_label(position))) {
            for (const TitlePosition choice : highlights::kTitlePositions) {
                if (ImGui::Selectable(position_label(choice), choice == position)) {
                    position = choice;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        changed |= draw_color("Text", color);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Border", &outlined);
        ImGui::SetItemTooltip("A border around the letters (for busy backgrounds)");
        ImGui::SameLine();
        ImGui::BeginDisabled(!outlined);
        changed |= draw_color("##border", outline);
        ImGui::EndDisabled();
        ImGui::PopID();
        return changed;
    }

    /// A colour swatch that opens a picker; true if the colour changed.
    static bool draw_color(const char* label, highlights::Rgb& color) {
        constexpr float kScale = 255.0F;
        std::array<float, 3> rgb{color.r / kScale, color.g / kScale, color.b / kScale};
        if (!ImGui::ColorEdit3(label, rgb.data(), ImGuiColorEditFlags_NoInputs)) {
            return false;
        }
        const auto channel = [](float value) {
            return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * kScale));
        };
        color = {channel(rgb[0]), channel(rgb[1]), channel(rgb[2])};
        return true;
    }

    void start_font_scan() {
        if (!installed && !font_scan.valid()) {
            fallback = fonts.fallback();
            font_scan = std::async(std::launch::async, [&fonts = fonts] {
                return fonts.installed();
            });
        }
    }

    void collect_font_scan() {
        if (font_scan.valid() &&
            font_scan.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            try {
                installed = font_scan.get();
            } catch (const std::exception& failure) {
                installed.emplace();
                error = std::format("Looking for fonts: {}", failure.what());
            }
        }
    }

    [[nodiscard]] std::string font_label(const std::filesystem::path& file) const {
        if (file.empty()) {
            return std::format("Default ({})", fallback.name.empty() ? "none found"
                                                                     : fallback.name);
        }
        if (installed) {
            const auto found = std::ranges::find(*installed, file, &highlights::FontChoice::file);
            if (found != installed->end()) {
                return found->name;
            }
        }
        return file.filename().string();
    }

    /// The font of all titles of the match day; the hovered one is shown in the player.
    void draw_font() {
        start_font_scan();
        collect_font_scan();
        hovered_font.reset();
        ImGui::SeparatorText("Font");
        ImGui::SetNextItemWidth(360);
        if (ImGui::BeginCombo("##font", font_label(font).c_str(),
                              ImGuiComboFlags_HeightLarge)) {
            if (!installed) {
                ImGui::TextColored(kMuted, "Looking for the installed fonts...");
            } else {
                font_filter.Draw("Filter", 200);
                if (ImGui::Selectable(font_label({}).c_str(), font.empty())) {
                    choose_font({});
                }
                if (ImGui::IsItemHovered()) {
                    hovered_font = std::filesystem::path();
                }
                for (const highlights::FontChoice& choice : *installed) {
                    if (!font_filter.PassFilter(choice.name.c_str())) {
                        continue;
                    }
                    ImGui::PushID(choice.file.string().c_str());
                    if (ImGui::Selectable(choice.name.c_str(), choice.file == font)) {
                        choose_font(choice.file);
                    }
                    if (ImGui::IsItemHovered()) {
                        hovered_font = choice.file;
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::TextColored(kMuted, "all titles of the match day");
        ImGui::SetItemTooltip("Point at a font in the list to see it in the player; "
                              "\"Enlarge\" shows the player large");
        if (!overlay.error().empty()) {
            ImGui::TextColored(kError, "%s", overlay.error().c_str());
        }
    }

    void choose_font(const std::filesystem::path& file) {
        font = file;
        try {
            store->save_title_font(font);
        } catch (const std::exception& failure) {
            error = std::format("Saving the font: {}", failure.what());
        }
    }

    /// The titles over the player's picture, in the hovered or the chosen font.
    [[nodiscard]] Overlay overlay_for(const GameHighlights& game) {
        return [this, titles = previewed(game),
                shown_font = hovered_font.value_or(font)](ImVec2 origin, ImVec2 size) {
            overlay.draw(titles, shown_font, origin, size);
        };
    }

    void draw_match() {
        ImGui::PushID("match");
        ImGui::SeparatorText("Match");
        pending_match |= ImGui::Checkbox("Show", &match.shown);
        ImGui::SetItemTooltip("Shown over every clip of the game videos and of the match day "
                              "video (the same for all games of the match day)");
        ImGui::SameLine();
        if (ImGui::Button("From the folder name")) {
            match.text = highlights::match_from_folder_name(day);
            pending_match = true;
        }
        ImGui::SetItemTooltip("e.g. 2026-11-14-TeamA-TeamB becomes \"TeamA - TeamB\"");
        pending_match |=
            draw_layout("Layout", match.size, match.position, match.color, match.outlined,
                        match.outline);
        pending_match |= input_multiline("##text", match.text,
                                         {-FLT_MIN, ImGui::GetTextLineHeight() * 2.5F});
        ImGui::SetItemTooltip("Several lines are possible (Enter), e.g. the final score");
        ImGui::PopID();
    }

    void draw_title(GameHighlights& game) {
        Title& title = game.title;
        bool changed = false;
        ImGui::PushID("players");
        ImGui::SeparatorText("Players");
        changed |= ImGui::Checkbox("Show", &title.shown);
        ImGui::SetItemTooltip("Shown over the whole game video; in the match day video if "
                              "\"Players on each rally\" is ticked");
        ImGui::SameLine();
        if (ImGui::Button("Swap players")) {
            title.text = highlights::swap_players(title.text);
            changed = true;
        }
        ImGui::SetItemTooltip("\"A - B\" in the first line becomes \"B - A\"");
        ImGui::SameLine();
        ImGui::TextUnformatted("   Preview as");
        ImGui::SameLine();
        ImGui::RadioButton("game video", &preview, kPreviewGame);
        ImGui::SameLine();
        ImGui::RadioButton("match day video", &preview, kPreviewMatchDay);
        changed |= draw_layout("Game video", title.size, title.position, title.color,
                               title.outlined, title.outline);
        pending_layout |= draw_layout("Match day video", day_layout.size, day_layout.position,
                                      day_layout.color, day_layout.outlined, day_layout.outline);
        ImGui::SameLine();
        ImGui::TextColored(kMuted, "(the same for every game)");
        changed |= input_multiline("##text", title.text,
                                   {-FLT_MIN, ImGui::GetTextLineHeight() * 3.5F});
        ImGui::SetItemTooltip("Several lines are possible (Enter)");
        ImGui::PopID();
        if (changed) {
            pending_game = game.video_id;
        }
    }

    /// Saves what changed, once no control is in use (or at once with `now`).
    void save_pending(bool now) {
        if (!now && (ImGui::IsAnyItemActive() || ImGui::IsMouseDown(ImGuiMouseButton_Left) ||
                     ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup))) {
            return;
        }
        if (!pending_game.empty()) {
            const auto game = std::ranges::find_if(games, [&](const Game& g) {
                return g.highlights && g.highlights->video_id == pending_game;
            });
            if (game != games.end()) {
                save(*game->highlights);
            }
            pending_game.clear();
        }
        if (pending_layout && store) {
            try {
                store->save_match_day_layout(day_layout);
            } catch (const std::exception& failure) {
                error = std::format("Saving the match day title: {}", failure.what());
            }
        }
        if (pending_match && store) {
            try {
                store->save_match_title(match);
            } catch (const std::exception& failure) {
                error = std::format("Saving the match: {}", failure.what());
            }
        }
        pending_layout = false;
        pending_match = false;
    }

    /// The titles as the player shows them: the match, and the players in the game's or the
    /// match day's layout.
    [[nodiscard]] std::vector<Title> previewed(const GameHighlights& game) const {
        return {match,
                preview == kPreviewMatchDay ? day_layout.applied_to(game.title) : game.title};
    }
};

HighlightsPanel::HighlightsPanel(const GuiContext& gui, highlights::MatchDayHighlights& days,
                                 highlights::PlaybackSource& playback,
                                 highlights::TitleFonts& fonts)
    : impl_(std::make_unique<Impl>(gui, days, playback, fonts)) {}

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
        self.player.draw(self.overlay_for(*game->highlights),
                         self.player.video() == game->video.original);
        ImGui::EndTable();
    }
    self.draw_font();
    self.draw_match();
    self.draw_title(*game->highlights);
    self.save_pending(false);
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
