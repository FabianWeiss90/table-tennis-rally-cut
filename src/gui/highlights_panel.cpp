// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "gui/highlights_panel.hpp"

#include "gui/frame_texture.hpp"
#include "gui/gui_context.hpp"
#include "highlights/application/highlight_length.hpp"
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
constexpr float kMinListHeight = 280.0F; ///< also when the player is lower
constexpr float kTitleMarginShare = 0.03F;  ///< as the rendered title
constexpr float kEnlargedShare = 0.85F;     ///< of the window, for the enlarged preview
constexpr float kControlsHeight = 40.0F;    ///< below the enlarged picture
constexpr float kLayoutLabelWidth = 130.0F;
constexpr int kPreviewGame = 0;
constexpr int kPreviewMatchDay = 1;
const ImVec4 kMuted{0.6F, 0.6F, 0.65F, 1.0F};
const ImVec4 kError{1.0F, 0.45F, 0.4F, 1.0F};
const ImVec4 kPlaying{0.45F, 0.75F, 1.0F, 1.0F};

/// A video's length for the eye: "4:05" or "1:02:03".
std::string length_text(double seconds) {
    const auto total = static_cast<long long>(std::llround(std::max(0.0, seconds)));
    constexpr long long kHour = 3600;
    constexpr long long kMinute = 60;
    return total >= kHour ? std::format("{}:{:02}:{:02}", total / kHour, total / kMinute % kMinute,
                                        total % kMinute)
                          : std::format("{}:{:02}", total / kMinute, total % kMinute);
}

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

const char* backdrop_label(highlights::BackdropShape shape) {
    using highlights::BackdropShape;
    switch (shape) {
    case BackdropShape::None:
        return "No background";
    case BackdropShape::RoundedBox:
        return "Rounded box";
    case BackdropShape::Pill:
        return "Pill";
    case BackdropShape::Cloud:
        return "Cloud";
    case BackdropShape::TornEnds:
        return "Banner, torn ends";
    case BackdropShape::PaperStrip:
        return "Banner, paper strip";
    case BackdropShape::Tape:
        return "Banner, tape";
    case BackdropShape::DoubleStrip:
        return "Banner, two layers";
    case BackdropShape::Brush:
        return "Brush stroke";
    case BackdropShape::SpeechBubble:
        return "Speech bubble";
    case BackdropShape::Ribbon:
        return "Ribbon";
    case BackdropShape::SportBar:
        return "Sport bar";
    case BackdropShape::Svg:
        return "SVG shape";
    }
    return "";
}

const char* backdrop_help(highlights::BackdropShape shape) {
    using highlights::BackdropShape;
    switch (shape) {
    case BackdropShape::None:
        return "Only the letters";
    case BackdropShape::RoundedBox:
        return "A box with round corners (radius under Effects)";
    case BackdropShape::Pill:
        return "A box with half-round ends";
    case BackdropShape::Cloud:
        return "Round bumps around the text; Reroll for other bumps";
    case BackdropShape::TornEnds:
        return "A band with straight edges, its ends torn off";
    case BackdropShape::PaperStrip:
        return "A strip torn from a roll: slightly uneven edges, ragged ends";
    case BackdropShape::Tape:
        return "A slightly tilted piece of tape with finely jagged, slanted ends";
    case BackdropShape::DoubleStrip:
        return "Two paper strips, the one behind turned a little and darker";
    case BackdropShape::Brush:
        return "A broad brush stroke with frayed edges and bristly ends";
    case BackdropShape::SpeechBubble:
        return "A rounded bubble with its tip towards the nearer edge of the picture";
    case BackdropShape::Ribbon:
        return "A band with V-notched tails behind it (darker)";
    case BackdropShape::SportBar:
        return "A slanted bar, as in sports TV graphics";
    case BackdropShape::Svg:
        return "An SVG file from the library's shapes folder";
    }
    return "";
}

/// A slider for a length given as share of the line height, shown in percent.
bool length_slider(const char* label, double& value, double max, const char* help) {
    constexpr float kPercent = 100.0F;
    auto percent = static_cast<float>(value * kPercent);
    ImGui::SetNextItemWidth(140);
    const bool changed = ImGui::SliderFloat(label, &percent, 0.0F,
                                            static_cast<float>(max) * kPercent, "%.0f %%");
    ImGui::SetItemTooltip("%s", help);
    if (changed) {
        value = std::clamp(static_cast<double>(percent) / kPercent, 0.0, max);
    }
    return changed;
}

bool opacity_slider(const char* label, int& opacity) {
    ImGui::SetNextItemWidth(110);
    const bool changed = ImGui::SliderInt(label, &opacity, 0, highlights::kMaxOpacity, "%d %%");
    ImGui::SetItemTooltip("0 %%: invisible, 100 %%: opaque");
    opacity = std::clamp(opacity, 0, highlights::kMaxOpacity);
    return changed;
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

    /// The folder SVG backdrops are read from.
    void set_shapes(const std::filesystem::path& shapes) {
        if (shapes != shapes_) {
            shapes_ = shapes;
            clear();
        }
    }

    /// Drops the rendered titles (e.g. after an SVG file changed).
    void refresh() { clear(); }

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
            std::format("{}|{}|{}", font.string(), line_height, highlights::look_key(title));
        if (const auto found = images_.find(key); found != images_.end()) {
            return &found->second;
        }
        if (images_.size() >= kMaxImages) {
            clear();
        }
        highlights::TitleImage mask;
        try {
            mask = fonts_.render(font, title, line_height, shapes_);
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
        SDL_UpdateTexture(image.texture, nullptr, mask.rgba.data(), mask.width * 4);
        return &images_.emplace(key, image).first->second;
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
    std::filesystem::path shapes_;
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
    std::filesystem::path shapes_dir; ///< the library's SVG shapes for title backdrops
    std::vector<std::string> svg_names; ///< in it, read when the shape list opens
    highlights::TitleLayout day_layout; ///< of the players' titles in the match day video
    Title match;                        ///< the match, over every clip
    // Changes saved once no control is in use any more (a colour picker changes all the time)
    std::string pending_game; ///< video id of a game whose title changed
    bool pending_layout = false;
    bool pending_match = false;
    int preview = kPreviewMatchDay;
    /// The rally played: key of its game's video and its index.
    std::optional<std::pair<std::string, std::size_t>> playing;
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

    static constexpr ImGuiTableFlags kListFlags =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV |
        ImGuiTableFlags_SizingFixedFit;

    /// The columns of a rally list, sorted by clicking their headers (lengths and marks first
    /// in descending order).
    static void setup_rally_columns() {
        constexpr ImGuiTableColumnFlags descending = ImGuiTableColumnFlags_PreferSortDescending;
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_DefaultSort, 0.0F, kNumberColumn);
        ImGui::TableSetupColumn("Start", ImGuiTableColumnFlags_None, 0.0F, kStartColumn);
        ImGui::TableSetupColumn("Length", descending, 0.0F, kLengthColumn);
        ImGui::TableSetupColumn("Game", descending, 0.0F, kGameColumn);
        ImGui::TableSetupColumn("Day", descending, 0.0F, kDayColumn);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch |
                                        ImGuiTableColumnFlags_NoSort);
        ImGui::TableHeadersRow();
    }

    /// One rally of a game in a list: number, start, length, both marks and play.
    void draw_rally_row(Game& game, std::size_t i, cutting::Margins margins) {
        GameHighlights& marks = *game.highlights;
        const double fps = marks.fps > 0 ? marks.fps : kFallbackFps;
        const highlights::HighlightRally& rally = marks.selection.rallies()[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::TableNextRow();
        const bool is_playing = playing && playing->first == game.video.key() &&
                                playing->second == i;
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
            playing = {game.video.key(), i};
            const auto before = static_cast<std::int64_t>(std::lround(margins.before_s * fps));
            const auto after = static_cast<std::int64_t>(std::lround(margins.after_s * fps));
            player.play(game.video, rally.start_frame - before, rally.end_frame + after, fps);
        }
        ImGui::SetItemTooltip("Watch it (with the margins before and after)");
        ImGui::PopID();
    }

    /// `bottom`: screen y down to which the list may reach (the player's bottom edge).
    void draw_rallies(Game& game, cutting::Margins margins, float bottom) {
        GameHighlights& marks = *game.highlights;
        ImGui::TextColored(kMuted, "%zu rallies from the %s; %zu game highlights, %zu match day",
                           marks.selection.rallies().size(), marks.source.c_str(),
                           marks.selection.game_count(), marks.selection.day_count());
        const float height = std::max(kMinListHeight, bottom - ImGui::GetCursorScreenPos().y);
        if (!ImGui::BeginTable("##rallies", kRallyColumns,
                               kListFlags | ImGuiTableFlags_Sortable, {0, height})) {
            return;
        }
        setup_rally_columns();
        for (const std::size_t i :
             sorted_rallies(marks.selection.rallies(), ImGui::TableGetSortSpecs())) {
            draw_rally_row(game, i, margins);
        }
        ImGui::EndTable();
    }

    /// Every game's rallies in one list, each game under a row with its name; a click on a
    /// column header sorts the rallies within each game (by time when unsorted).
    void draw_day_rallies(cutting::Margins margins, double crossfade_s, float bottom) {
        const float height = std::max(kMinListHeight, bottom - ImGui::GetCursorScreenPos().y);
        if (!ImGui::BeginTable("##day_rallies", kRallyColumns,
                               kListFlags | ImGuiTableFlags_Sortable, {0, height})) {
            return;
        }
        setup_rally_columns();
        const ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs();
        for (Game& game : games) {
            ImGui::PushID(game.video.key().c_str());
            ImGui::TableNextRow();
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImGuiCol_Header));
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            if (!game.highlights) {
                ImGui::TextUnformatted(game.video.video_id.c_str());
                ImGui::TableSetColumnIndex(kRallyColumns - 1);
                ImGui::TextColored(kMuted, "no rallies yet");
                ImGui::PopID();
                continue;
            }
            const highlights::HighlightSelection& selection = game.highlights->selection;
            ImGui::TextUnformatted(game.video.video_id.c_str());
            ImGui::TableSetColumnIndex(kStartColumn);
            ImGui::TextColored(kMuted, "%zu rallies", selection.rallies().size());
            ImGui::TableSetColumnIndex(kLengthColumn);
            ImGui::TextColored(kMuted, "%s",
                               length_text(video_length(game, highlights::HighlightVideo::Game,
                                                        margins, crossfade_s))
                                   .c_str());
            ImGui::SetItemTooltip("Length of this game's highlight video");
            ImGui::TableSetColumnIndex(kGameColumn);
            ImGui::Text("%zu", selection.game_count());
            ImGui::SetItemTooltip("Game highlights of this game");
            ImGui::TableSetColumnIndex(kDayColumn);
            ImGui::Text("%zu", selection.day_count());
            ImGui::SetItemTooltip("Match day highlights of this game");
            // Sorted within the game; the game rows keep their order
            for (const std::size_t i : sorted_rallies(selection.rallies(), specs)) {
                draw_rally_row(game, i, margins);
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    /// The clips a game adds to a highlight video, in seconds.
    static std::vector<double> clips_of(const Game& game, highlights::HighlightVideo video,
                                        cutting::Margins margins) {
        if (!game.highlights) {
            return {};
        }
        return highlights::clip_lengths(game.highlights->selection.rallies(), video, margins,
                                        game.highlights->fps > 0 ? game.highlights->fps
                                                                 : kFallbackFps);
    }

    static double video_length(const Game& game, highlights::HighlightVideo video,
                               cutting::Margins margins, double crossfade_s) {
        return highlights::joined_length(clips_of(game, video, margins), crossfade_s);
    }

    /// The match day video: the match day highlights of every game, one after another.
    [[nodiscard]] double day_length(cutting::Margins margins, double crossfade_s) const {
        std::vector<double> clips;
        for (const Game& game : games) {
            const std::vector<double> own =
                clips_of(game, highlights::HighlightVideo::MatchDay, margins);
            clips.insert(clips.end(), own.begin(), own.end());
        }
        return highlights::joined_length(clips, crossfade_s);
    }

    /// How long the highlight videos will be with the current marks; `game`: also its video.
    void draw_lengths(const Game* game, cutting::Margins margins, double crossfade_s) const {
        if (game != nullptr && game->highlights) {
            ImGui::Text("Game video: about %s",
                        length_text(video_length(*game, highlights::HighlightVideo::Game,
                                                 margins, crossfade_s))
                            .c_str());
            ImGui::SetItemTooltip("With the margins and cross-fades set below");
            ImGui::SameLine(0, 30);
        }
        ImGui::Text("Match day video: about %s",
                    length_text(day_length(margins, crossfade_s)).c_str());
        ImGui::SetItemTooltip("The match day highlights of all games, with the margins and "
                              "cross-fades set below");
    }

    /// Size, position, colours and decoration of a title; true if one changed.
    bool draw_layout(const char* label, highlights::TitleLayout& layout) {
        ImGui::PushID(label);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(kLayoutLabelWidth);
        ImGui::SetNextItemWidth(140);
        auto percent = static_cast<float>(layout.size * 100);
        bool changed = false;
        if (ImGui::SliderFloat("Size", &percent, static_cast<float>(Title::kMinSize * 100),
                               static_cast<float>(Title::kMaxSize * 100), "%.1f %%")) {
            layout.size = percent / 100.0;
            changed = true;
        }
        ImGui::SetItemTooltip("Height of a line, as share of the picture height");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140);
        if (ImGui::BeginCombo("Position", position_label(layout.position))) {
            for (const TitlePosition choice : highlights::kTitlePositions) {
                if (ImGui::Selectable(position_label(choice), choice == layout.position)) {
                    layout.position = choice;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        changed |= draw_color("Text", layout.color);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Border", &layout.outlined);
        ImGui::SetItemTooltip("A border around the letters (for busy backgrounds)");
        ImGui::SameLine();
        ImGui::BeginDisabled(!layout.outlined);
        changed |= draw_color("##border", layout.outline);
        ImGui::EndDisabled();
        changed |= draw_decoration(layout.decoration);
        ImGui::PopID();
        return changed;
    }

    /// The backdrop row below a layout: shape, colour, opacity, per line, reroll, and the
    /// effects in a popup; true if one changed.
    bool draw_decoration(highlights::TitleDecoration& decoration) {
        using highlights::BackdropShape;
        highlights::Backdrop& backdrop = decoration.backdrop;
        bool changed = false;
        ImGui::Dummy({kLayoutLabelWidth - ImGui::GetStyle().ItemSpacing.x, 0});
        ImGui::SameLine(kLayoutLabelWidth);
        ImGui::SetNextItemWidth(180);
        const std::string preview = backdrop.shape == BackdropShape::Svg
                                        ? backdrop.svg
                                        : std::string(backdrop_label(backdrop.shape));
        if (ImGui::BeginCombo("##backdrop", preview.c_str(), ImGuiComboFlags_HeightLarge)) {
            if (ImGui::IsWindowAppearing()) {
                svg_names = fonts.shapes(shapes_dir);
                overlay.refresh(); // an SVG file may have changed
            }
            for (const BackdropShape shape : highlights::kBuiltInBackdrops) {
                if (ImGui::Selectable(backdrop_label(shape), backdrop.shape == shape)) {
                    backdrop.shape = shape;
                    changed = true;
                }
                ImGui::SetItemTooltip("%s", backdrop_help(shape));
            }
            ImGui::SeparatorText("SVG shapes of the library");
            for (const std::string& name : svg_names) {
                const bool chosen = backdrop.shape == BackdropShape::Svg && backdrop.svg == name;
                if (ImGui::Selectable(name.c_str(), chosen)) {
                    backdrop.shape = BackdropShape::Svg;
                    backdrop.svg = name;
                    changed = true;
                }
                ImGui::SetItemTooltip("Filled with the background colour; the middle is "
                                      "stretched to the text, the ends keep their shape");
            }
            if (svg_names.empty()) {
                ImGui::TextColored(kMuted, "None yet: put SVG files into the shapes folder");
            }
            if (ImGui::Selectable("Open the shapes folder...", false,
                                  ImGuiSelectableFlags_NoAutoClosePopups)) {
                open_shapes_folder();
            }
            ImGui::SetItemTooltip("%s", shapes_dir.string().c_str());
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("%s", backdrop_help(backdrop.shape));
        const bool none = backdrop.shape == BackdropShape::None;
        ImGui::BeginDisabled(none);
        ImGui::SameLine();
        changed |= draw_color("##backdrop_color", backdrop.color);
        ImGui::SetItemTooltip("Colour of the background");
        ImGui::SameLine();
        changed |= opacity_slider("Opacity", backdrop.opacity);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Per line", &backdrop.per_line);
        ImGui::SetItemTooltip("One shape behind each line instead of one around all lines");
        ImGui::SameLine();
        ImGui::BeginDisabled(!highlights::uses_seed(backdrop.shape));
        if (ImGui::Button("Reroll")) {
            backdrop.seed = highlights::next_seed(backdrop.seed);
            changed = true;
        }
        ImGui::SetItemTooltip("Another shape of the same kind (other bumps, tears, bristles)");
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Effects...")) {
            ImGui::OpenPopup("effects");
        }
        ImGui::SetItemTooltip("Padding, soft edge and line of the background; shadow; glow");
        if (ImGui::BeginPopup("effects")) {
            changed |= draw_effects(decoration);
            ImGui::EndPopup();
        }
        return changed;
    }

    static bool draw_effects(highlights::TitleDecoration& decoration) {
        using highlights::Backdrop;
        highlights::Backdrop& backdrop = decoration.backdrop;
        bool changed = false;
        ImGui::SeparatorText("Background");
        ImGui::BeginDisabled(backdrop.shape == highlights::BackdropShape::None);
        changed |= length_slider("Padding", backdrop.padding, Backdrop::kMaxLength,
                                 "Space between the text and the shape, per line height");
        ImGui::BeginDisabled(backdrop.shape != highlights::BackdropShape::RoundedBox);
        changed |= length_slider("Corner radius", backdrop.corner, Backdrop::kMaxLength,
                                 "Of the rounded box, per line height");
        ImGui::EndDisabled();
        changed |= length_slider("Soft edge", backdrop.softness, Backdrop::kMaxLength,
                                 "Width over which the edge fades out, per line height");
        changed |= ImGui::Checkbox("Line along the edge", &backdrop.bordered);
        ImGui::SameLine();
        changed |= draw_color("##edge_color", backdrop.border);
        ImGui::BeginDisabled(!backdrop.bordered);
        changed |= length_slider("Line width", backdrop.border_width, Backdrop::kMaxBorder,
                                 "Per line height; the line is opaque");
        ImGui::EndDisabled();
        ImGui::EndDisabled();

        highlights::TitleShadow& shadow = decoration.shadow;
        ImGui::SeparatorText("Shadow");
        changed |= ImGui::Checkbox("Shadow", &shadow.shown);
        ImGui::SetItemTooltip("Behind the background, or behind the letters without one");
        ImGui::SameLine();
        changed |= draw_color("##shadow_color", shadow.color);
        ImGui::BeginDisabled(!shadow.shown);
        ImGui::SameLine();
        changed |= opacity_slider("##shadow_opacity", shadow.opacity);
        changed |= length_slider("Offset", shadow.offset, highlights::TitleShadow::kMaxLength,
                                 "Down and to the right, per line height");
        changed |= length_slider("Blur", shadow.blur, highlights::TitleShadow::kMaxLength,
                                 "Per line height");
        ImGui::EndDisabled();

        highlights::TitleGlow& glow = decoration.glow;
        ImGui::SeparatorText("Glow");
        changed |= ImGui::Checkbox("Glow", &glow.shown);
        ImGui::SetItemTooltip("A soft light around the letters");
        ImGui::SameLine();
        changed |= draw_color("##glow_color", glow.color);
        ImGui::BeginDisabled(!glow.shown);
        ImGui::SameLine();
        changed |= opacity_slider("##glow_opacity", glow.opacity);
        changed |= length_slider("Glow size", glow.size, highlights::TitleGlow::kMaxSize,
                                 "Per line height");
        ImGui::EndDisabled();
        return changed;
    }

    void open_shapes_folder() {
        std::error_code ignored;
        std::filesystem::create_directories(shapes_dir, ignored);
        const std::string url = "file://" + std::filesystem::absolute(shapes_dir).generic_string();
        if (!SDL_OpenURL(url.c_str())) {
            error = std::format("Opening {}: {}", shapes_dir.string(), SDL_GetError());
        }
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

    /// The preselection for every game at once.
    void draw_day_preselection() {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Preselect the longest");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        ImGui::InputInt("##count", &preselect);
        preselect = std::clamp(preselect, 0, kMaxPreselect);
        ImGui::SameLine();
        if (ImGui::Button("of every game")) {
            for (Game& game : games) {
                if (game.highlights) {
                    game.highlights->selection.preselect(static_cast<std::size_t>(preselect));
                    save(*game.highlights);
                }
            }
        }
        ImGui::SetItemTooltip("Marks the longest rallies of each game as game and match day "
                              "highlights and removes all other marks");
        ImGui::SameLine();
        if (ImGui::Button("Clear all")) {
            for (Game& game : games) {
                if (game.highlights) {
                    game.highlights->selection.clear();
                    save(*game.highlights);
                }
            }
        }
        ImGui::SetItemTooltip("Removes every mark of every game of the match day");
    }

    /// The game whose rally is played, else the first with rallies.
    [[nodiscard]] Game* played_game() {
        Game* first = nullptr;
        for (Game& game : games) {
            if (!game.highlights) {
                continue;
            }
            if (playing && playing->first == game.video.key()) {
                return &game;
            }
            if (first == nullptr) {
                first = &game;
            }
        }
        return first;
    }

    /// The titles over the player in the match day view: as in the match day video.
    [[nodiscard]] Overlay day_overlay_for(const GameHighlights& game) {
        return [this, titles = std::vector<Title>{match, day_layout.applied_to(game.title)},
                shown_font = hovered_font.value_or(font)](ImVec2 origin, ImVec2 size) {
            overlay.draw(titles, shown_font, origin, size);
        };
    }

    void draw_day_layout() {
        ImGui::PushID("players");
        ImGui::SeparatorText("Players in the match day video");
        pending_layout |= draw_layout("Layout", day_layout);
        ImGui::SameLine();
        ImGui::TextColored(kMuted, "(the same for every game; the names are set per game)");
        ImGui::PopID();
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
        highlights::TitleLayout layout = highlights::TitleLayout::of(match);
        if (draw_layout("Layout", layout)) {
            match = layout.applied_to(match);
            pending_match = true;
        }
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
        highlights::TitleLayout layout = highlights::TitleLayout::of(title);
        if (draw_layout("Game video", layout)) {
            title = layout.applied_to(title);
            changed = true;
        }
        pending_layout |= draw_layout("Match day video", day_layout);
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
                           cutting::Margins margins, double crossfade_s) {
    Impl& self = *impl_;
    if (reload || video.match_day != self.day || !self.store) {
        self.load(workspace, video);
    }
    self.shapes_dir = workspace.shapes_dir();
    self.overlay.set_shapes(self.shapes_dir);
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
    self.draw_lengths(game, margins, crossfade_s);
    self.draw_preselection(*game->highlights);
    if (ImGui::BeginTable("##layout", 2, ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("list", ImGuiTableColumnFlags_WidthStretch, kListShare);
        ImGui::TableSetupColumn("player", ImGuiTableColumnFlags_WidthStretch, 1.0F - kListShare);
        ImGui::TableNextRow();
        // The player first: its height decides how far down the list reaches.
        ImGui::TableSetColumnIndex(1);
        self.player.draw(self.overlay_for(*game->highlights),
                         self.player.video() == game->video.original);
        const float player_bottom =
            ImGui::GetCursorScreenPos().y - ImGui::GetStyle().ItemSpacing.y;
        ImGui::TableSetColumnIndex(0);
        self.draw_rallies(*game, margins, player_bottom);
        ImGui::EndTable();
    }
    self.draw_font();
    self.draw_match();
    self.draw_title(*game->highlights);
    self.save_pending(false);
}

void HighlightsPanel::draw_match_day(const launcher::WorkspaceSnapshot& workspace,
                                     const std::string& match_day, bool reload,
                                     cutting::Margins margins, double crossfade_s) {
    Impl& self = *impl_;
    const auto first =
        std::ranges::find(workspace.videos, match_day, &launcher::VideoEntry::match_day);
    if (first == workspace.videos.end()) {
        return;
    }
    if (reload || match_day != self.day || !self.store) {
        self.load(workspace, *first);
    }
    self.shapes_dir = workspace.shapes_dir();
    self.overlay.set_shapes(self.shapes_dir);
    self.draw_overview();
    ImGui::SameLine();
    if (ImGui::SmallButton("Reload")) {
        self.load(workspace, *first);
    }
    ImGui::SetItemTooltip("Read the rallies again (e.g. after detect or refine)");
    if (!self.error.empty()) {
        ImGui::TextColored(kError, "%s", self.error.c_str());
    }
    Game* shown = self.played_game();
    if (shown == nullptr) {
        ImGui::TextColored(kMuted, "No game of %s has rallies yet: run detect (or annotate the "
                                   "games completely).",
                           match_day.c_str());
        return;
    }
    ImGui::SeparatorText("Match day highlights");
    self.draw_lengths(nullptr, margins, crossfade_s);
    self.draw_day_preselection();
    if (ImGui::BeginTable("##layout", 2, ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("list", ImGuiTableColumnFlags_WidthStretch, kListShare);
        ImGui::TableSetupColumn("player", ImGuiTableColumnFlags_WidthStretch, 1.0F - kListShare);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(1);
        self.player.draw(self.day_overlay_for(*shown->highlights),
                         self.player.video() == shown->video.original);
        const float player_bottom =
            ImGui::GetCursorScreenPos().y - ImGui::GetStyle().ItemSpacing.y;
        ImGui::TableSetColumnIndex(0);
        self.draw_day_rallies(margins, crossfade_s, player_bottom);
        ImGui::EndTable();
    }
    self.draw_font();
    self.draw_match();
    self.draw_day_layout();
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
