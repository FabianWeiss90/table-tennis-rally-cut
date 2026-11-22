// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "gui/white_balance_panel.hpp"

#include "gui/frame_texture.hpp"
#include "gui/gui_context.hpp"
#include "shared/kernel/parallel.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <future>
#include <imgui.h>
#include <map>
#include <optional>
#include <tuple>
#include <string>
#include <vector>

namespace ttrally::gui {

namespace {

using cutting::FileWhiteBalance;
using cutting::RelativeRect;
using cutting::RgbColor;
using cutting::RgbGains;

constexpr int kPictureHeight = 270;         ///< lines of the pictures read
constexpr double kPictureFraction = 0.5;    ///< where in a file its picture is taken
constexpr unsigned kParallelFiles = 4;      ///< each decode uses several threads itself
constexpr float kAreaCardWidth = 280.0F;    ///< a file's picture the area is dragged on
constexpr float kReferenceWidth = 560.0F;   ///< the reference's picture
constexpr float kResultWidth = 200.0F;      ///< a corrected picture
constexpr float kSliderWidth = 300.0F;
constexpr float kSliderScale = 100.0F;      ///< the sliders show -100 to 100
constexpr float kDefaultShape = 9.0F / 16.0F; ///< height per width before a picture is read
constexpr int kLimitedBlack = 16;
constexpr int kChromaZero = 128;
const ImVec4 kMuted{0.6F, 0.6F, 0.65F, 1.0F};
const ImVec4 kPartly{0.95F, 0.75F, 0.35F, 1.0F};
const ImU32 kAreaColor = IM_COL32(255, 80, 80, 255);
const ImU32 kTakenAreaColor = IM_COL32(255, 180, 80, 255); ///< an area taken from before
const ImVec4 kReferenceColor{0.55F, 0.75F, 1.0F, 1.0F};

/// A file of the match day whose white balance is chosen: a single video or a part of a match.
struct BalancedFile {
    std::string name;            ///< file name in the match day folder
    std::filesystem::path path;  ///< the file
    std::string label;           ///< e.g. "kaether-baumgarten, part 2"
};

std::vector<BalancedFile> files_of(const launcher::WorkspaceSnapshot& workspace,
                                   const std::string& day) {
    std::vector<BalancedFile> files;
    for (const launcher::VideoEntry& video : workspace.videos) {
        if (video.match_day != day) {
            continue;
        }
        if (video.parts.empty()) {
            const std::filesystem::path path(video.original);
            files.push_back({path.filename().string(), path, video.video_id});
        }
        for (std::size_t i = 0; i < video.parts.size(); ++i) {
            const std::filesystem::path path(video.parts[i]);
            files.push_back({path.filename().string(), path,
                             std::format("{}, part {}", video.video_id, i + 1)});
        }
    }
    return files;
}

/// The frame with the gains applied to its YUV values (limited range, BT.709).
media::VideoFrame balanced(media::VideoFrame frame, RgbGains gains) {
    if (gains.identity()) {
        return frame;
    }
    const std::array<double, 9> m = cutting::yuv_matrix(gains);
    const int width = frame.size.width;
    const int height = frame.size.height;
    const auto luma = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    std::uint8_t* y_plane = frame.planes.data();
    std::uint8_t* u_plane = y_plane + luma;
    std::uint8_t* v_plane = u_plane + luma / 4;
    const auto clamp = [](double value) {
        return static_cast<std::uint8_t>(std::clamp(std::lround(value), 0L, 255L));
    };
    for (int cy = 0; cy < height / 2; ++cy) {
        for (int cx = 0; cx < width / 2; ++cx) {
            const auto c = static_cast<std::size_t>(cy) * static_cast<std::size_t>(width / 2) +
                           static_cast<std::size_t>(cx);
            const double u = u_plane[c] - kChromaZero;
            const double v = v_plane[c] - kChromaZero;
            double luma_sum = 0;
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    const auto at = static_cast<std::size_t>(2 * cy + dy) *
                                        static_cast<std::size_t>(width) +
                                    static_cast<std::size_t>(2 * cx + dx);
                    std::uint8_t& y_value = y_plane[at];
                    const double y = y_value - kLimitedBlack;
                    luma_sum += y;
                    y_value = clamp(m[0] * y + m[1] * u + m[2] * v + kLimitedBlack);
                }
            }
            const double y = luma_sum / 4;
            u_plane[c] = clamp(m[3] * y + m[4] * u + m[5] * v + kChromaZero);
            v_plane[c] = clamp(m[6] * y + m[7] * u + m[8] * v + kChromaZero);
        }
    }
    return frame;
}

/// The mean colour of the area on the frame; nullopt if the area is empty.
std::optional<RgbColor> color_in(const media::VideoFrame& frame, RelativeRect area) {
    if (area.empty()) {
        return std::nullopt;
    }
    const int width = frame.size.width;
    const int height = frame.size.height;
    const int left = std::clamp(static_cast<int>(area.left * width) / 2, 0, width / 2 - 1);
    const int right = std::clamp(static_cast<int>(std::ceil(area.right * width)) / 2, left + 1,
                                 width / 2);
    const int top = std::clamp(static_cast<int>(area.top * height) / 2, 0, height / 2 - 1);
    const int bottom = std::clamp(static_cast<int>(std::ceil(area.bottom * height)) / 2, top + 1,
                                  height / 2);
    const auto y_plane = frame.y();
    const auto u_plane = frame.u();
    const auto v_plane = frame.v();
    double y = 0;
    double u = 0;
    double v = 0;
    int samples = 0;
    for (int cy = top; cy < bottom; ++cy) {
        for (int cx = left; cx < right; ++cx) {
            const auto c = static_cast<std::size_t>(cy) * static_cast<std::size_t>(width / 2) +
                           static_cast<std::size_t>(cx);
            u += u_plane[c];
            v += v_plane[c];
            y += y_plane[static_cast<std::size_t>(2 * cy) * static_cast<std::size_t>(width) +
                         static_cast<std::size_t>(2 * cx)];
            ++samples;
        }
    }
    return cutting::rgb_of(y / samples, u / samples, v / samples);
}

} // namespace

struct WhiteBalancePanel::Impl {
    Impl(const GuiContext& gui, launcher::VideoPreview& preview, cutting::WhiteBalanceStore& store)
        : renderer(gui.renderer()), preview(preview), store(store) {}
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;
    ~Impl() {
        if (job.valid()) {
            job.wait();
        }
    }

    using Pictures = std::map<std::filesystem::path, std::optional<media::VideoFrame>>;

    /// A picture shown, with the gains it was made with.
    struct Shown {
        std::unique_ptr<FrameTexture> texture;
        RgbGains gains;
        bool valid = false;
    };

    SDL_Renderer* renderer;
    launcher::VideoPreview& preview;
    cutting::WhiteBalanceStore& store;
    std::string day;
    std::filesystem::path work_dir;
    cutting::WhiteBalanceSettings settings;
    std::vector<BalancedFile> files;
    Pictures pictures; ///< a frame from the middle of each file
    std::future<Pictures> job;
    std::map<std::string, Shown> shown; ///< by "<file>|<use>"
    std::int64_t uploads = 0;           ///< a new texture content each time
    std::optional<std::string> dragging; ///< file whose area is being dragged
    ImVec2 drag_start{};
    RelativeRect dragged{};
    std::string error;

    void open(const launcher::WorkspaceSnapshot& workspace, const std::string& match_day) {
        files = files_of(workspace, match_day);
        if (match_day != day) {
            day = match_day;
            work_dir.clear();
            for (const launcher::VideoEntry& video : workspace.videos) {
                if (video.match_day == day) {
                    work_dir = video.work_dir;
                }
            }
            try {
                settings = store.load(work_dir);
                error.clear();
            } catch (const std::exception& failure) {
                settings = {};
                error = std::format("White balance of {}: {}", day, failure.what());
            }
        }
        // One entry per file, in the list's order (an area is taken from the file before)
        std::vector<FileWhiteBalance> ordered;
        for (const BalancedFile& file : files) {
            const auto stored =
                std::ranges::find(settings.files, file.name, &FileWhiteBalance::file);
            ordered.push_back(stored != settings.files.end()
                                  ? *stored
                                  : FileWhiteBalance{file.name, std::nullopt, {}});
        }
        settings.files = std::move(ordered);
    }

    /// Starts reading the pictures of files that have none yet.
    void read_missing() {
        if (job.valid()) {
            if (job.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                return;
            }
            for (auto& [path, frame] : job.get()) {
                pictures[path] = std::move(frame);
            }
        }
        std::vector<std::filesystem::path> missing;
        for (const BalancedFile& file : files) {
            if (!pictures.contains(file.path)) {
                missing.push_back(file.path);
            }
        }
        if (missing.empty()) {
            return;
        }
        job = std::async(std::launch::async, [&preview = preview, missing] {
            std::vector<std::optional<media::VideoFrame>> read(missing.size());
            parallel_for(missing.size(), kParallelFiles, [&](std::size_t i) {
                read[i] = preview.frame_at(missing[i], kPictureFraction, kPictureHeight);
            });
            Pictures found;
            for (std::size_t i = 0; i < missing.size(); ++i) {
                found[missing[i]] = std::move(read[i]);
            }
            return found;
        });
    }

    [[nodiscard]] const media::VideoFrame* picture_of(const BalancedFile& file) const {
        const auto found = pictures.find(file.path);
        return found == pictures.end() || !found->second ? nullptr : &*found->second;
    }

    [[nodiscard]] std::size_t reference_index() const {
        const auto found = std::ranges::find(files, settings.reference, &BalancedFile::name);
        return static_cast<std::size_t>(found - files.begin());
    }

    /// Every file's gains again from the colours in their areas, then saved.
    void update() {
        const auto areas = cutting::effective_areas(settings.files);
        if (settings.reference.empty() ||
            std::ranges::find(files, settings.reference, &BalancedFile::name) == files.end()) {
            const auto first =
                std::ranges::find_if(areas, [](const auto& area) { return area.has_value(); });
            const auto index = static_cast<std::size_t>(first - areas.begin());
            settings.reference = first == areas.end() ? std::string() : files[index].name;
        }
        std::vector<std::optional<RgbColor>> measured;
        for (std::size_t i = 0; i < files.size(); ++i) {
            const media::VideoFrame* frame = picture_of(files[i]);
            measured.push_back(frame == nullptr || !areas[i] ? std::nullopt
                                                             : color_in(*frame, *areas[i]));
        }
        const auto gains = cutting::balance_gains(measured, reference_index(), settings.temperature,
                                                  settings.tint, settings.match_brightness);
        for (std::size_t i = 0; i < files.size(); ++i) {
            settings.files[i].gains = gains[i];
        }
    }

    void save() {
        update();
        try {
            store.save(work_dir, settings);
            error.clear();
        } catch (const std::exception& failure) {
            error = std::format("Saving the white balance: {}", failure.what());
        }
    }

    /// The texture of a file's picture with gains (made again when the gains changed).
    SDL_Texture* texture_of(const BalancedFile& file, const std::string& use, RgbGains gains) {
        const media::VideoFrame* frame = picture_of(file);
        if (frame == nullptr) {
            return nullptr;
        }
        Shown& entry = shown[file.name + "|" + use];
        if (!entry.texture) {
            entry.texture = std::make_unique<FrameTexture>(renderer);
        }
        if (!entry.valid || !(entry.gains == gains)) {
            media::VideoFrame made = balanced(*frame, gains);
            made.index = ++uploads;
            entry.texture->show(made);
            entry.gains = gains;
            entry.valid = true;
        }
        return entry.texture->texture();
    }

    [[nodiscard]] ImVec2 extent_of(const BalancedFile& file, float width) const {
        const media::VideoFrame* frame = picture_of(file);
        const float shape = frame == nullptr ? kDefaultShape
                                             : static_cast<float>(frame->size.height) /
                                                   static_cast<float>(frame->size.width);
        return {width, width * shape};
    }

    /// Places the next card next to the previous one while it fits, else on a new line.
    static void next_card(std::size_t index, float width, float right_edge) {
        if (index > 0 && ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + width <=
                             right_edge) {
            ImGui::SameLine();
        }
    }

    void draw(const launcher::WorkspaceSnapshot& workspace, const std::string& match_day) {
        open(workspace, match_day);
        read_missing();
        update(); // the pictures may have arrived
        ImGui::TextWrapped(
            "Correct the white balance of videos recorded with automatic white balance (each "
            "part of a match on its own, since the camera may change between files). 1: in every "
            "file, drag a rectangle over the same spot, one that should look the same in all of "
            "them (a file without one takes the rectangle of the file before it). 2: tune one of "
            "them, the reference, with temperature and tint. 3: every other file is corrected so "
            "that its spot looks like the reference's. Cut and highlights apply it; labels, "
            "features and detections keep the colours as recorded.");
        if (job.valid()) {
            ImGui::TextColored(kPartly, "Reading a picture from the middle of every file...");
        }
        if (!error.empty()) {
            ImGui::TextColored(ImVec4(1.0F, 0.45F, 0.4F, 1.0F), "%s", error.c_str());
        }
        if (files.empty()) {
            return;
        }
        draw_areas();
        draw_reference();
        draw_result();
    }

    void draw_areas() {
        ImGui::SeparatorText("1. The same spot in every file");
        const auto areas = cutting::effective_areas(settings.files);
        const float right_edge = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        for (std::size_t i = 0; i < files.size(); ++i) {
            next_card(i, kAreaCardWidth, right_edge);
            ImGui::BeginGroup();
            ImGui::PushID(static_cast<int>(i));
            draw_area_card(i, areas[i]);
            ImGui::PopID();
            ImGui::EndGroup();
        }
    }

    void draw_area_card(std::size_t i, const std::optional<RelativeRect>& area) {
        const BalancedFile& file = files[i];
        FileWhiteBalance& balance = settings.files[i];
        ImGui::TextUnformatted(file.label.c_str());
        ImGui::SetItemTooltip("%s", file.path.string().c_str());
        SDL_Texture* texture = texture_of(file, "area", {});
        const ImVec2 extent = extent_of(file, kAreaCardWidth);
        if (texture == nullptr) {
            ImGui::Button("...", extent);
            return;
        }
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::Image(ImTextureRef(reinterpret_cast<ImTextureID>(texture)), extent);
        ImGui::SetCursorScreenPos(origin);
        ImGui::InvisibleButton("##area", extent);
        const auto relative = [&](ImVec2 point) {
            return ImVec2{std::clamp((point.x - origin.x) / extent.x, 0.0F, 1.0F),
                          std::clamp((point.y - origin.y) / extent.y, 0.0F, 1.0F)};
        };
        if (ImGui::IsItemActivated()) {
            dragging = file.name;
            drag_start = relative(ImGui::GetIO().MousePos);
        }
        const bool mine = dragging == file.name;
        if (mine) {
            const ImVec2 now = relative(ImGui::GetIO().MousePos);
            dragged = {std::min(drag_start.x, now.x), std::min(drag_start.y, now.y),
                       std::max(drag_start.x, now.x), std::max(drag_start.y, now.y)};
        }
        if (mine && ImGui::IsItemDeactivated()) {
            dragging.reset();
            if (!dragged.empty()) {
                balance.area = dragged;
                save();
            }
        }
        ImGui::SetItemTooltip("Drag a rectangle over the spot that looks the same in every file");
        const std::optional<RelativeRect> shown_area = mine ? std::optional(dragged) : area;
        if (shown_area) {
            const bool own = mine || balance.area.has_value();
            ImGui::GetWindowDrawList()->AddRect(
                {origin.x + static_cast<float>(shown_area->left) * extent.x,
                 origin.y + static_cast<float>(shown_area->top) * extent.y},
                {origin.x + static_cast<float>(shown_area->right) * extent.x,
                 origin.y + static_cast<float>(shown_area->bottom) * extent.y},
                own ? kAreaColor : kTakenAreaColor, 0.0F, 0, 2.0F);
        }
        if (balance.area) {
            if (ImGui::SmallButton(i == 0 ? "Remove" : "Take the one before")) {
                balance.area.reset();
                save();
            }
            ImGui::SetItemTooltip("Removes this file's own rectangle%s",
                                  i == 0 ? "" : ": it takes the one of the file before it again");
        } else {
            ImGui::TextColored(kMuted, "%s", area ? "Taken from the file before" : "No area yet");
        }
    }

    void draw_reference() {
        ImGui::SeparatorText("2. Tune the reference");
        const std::size_t reference = reference_index();
        if (reference >= files.size()) {
            ImGui::TextColored(kMuted, "Drag a rectangle in a file first.");
            return;
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Reference");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(360);
        if (ImGui::BeginCombo("##reference", files[reference].label.c_str())) {
            for (const BalancedFile& file : files) {
                if (ImGui::Selectable(file.label.c_str(), file.name == settings.reference)) {
                    settings.reference = file.name;
                    save();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("The file whose look the others get");
        const BalancedFile& file = files[reference];
        ImGui::BeginGroup();
        if (SDL_Texture* texture =
                texture_of(file, "reference", settings.files[reference].gains)) {
            ImGui::Image(ImTextureRef(reinterpret_cast<ImTextureID>(texture)),
                         extent_of(file, kReferenceWidth));
        }
        ImGui::EndGroup();
        ImGui::SameLine();
        ImGui::BeginGroup();
        bool changed = false;
        bool finished = false;
        auto slider = [&](const char* label, double& value, const char* help) {
            auto shown = static_cast<float>(value * kSliderScale);
            ImGui::SetNextItemWidth(kSliderWidth);
            if (ImGui::SliderFloat(label, &shown, -kSliderScale, kSliderScale, "%.0f")) {
                value = shown / kSliderScale;
                changed = true;
            }
            finished |= ImGui::IsItemDeactivatedAfterEdit();
            ImGui::SetItemTooltip("%s", help);
        };
        slider("Temperature", settings.temperature, "Higher: warmer (more red, less blue)");
        slider("Tint", settings.tint, "Higher: more magenta, less green");
        if (ImGui::Button("Area neutral")) {
            const auto areas = cutting::effective_areas(settings.files);
            const media::VideoFrame* frame = picture_of(file);
            if (frame != nullptr && areas[reference]) {
                if (const auto color = color_in(*frame, *areas[reference])) {
                    std::tie(settings.temperature, settings.tint) =
                        cutting::neutral_temperature_tint(*color);
                    finished = true;
                }
            }
        }
        ImGui::SetItemTooltip("Temperature and tint that make the reference's spot grey: a "
                              "start for spots that are white or grey");
        ImGui::SameLine();
        if (ImGui::Button("Reset")) {
            settings.temperature = 0;
            settings.tint = 0;
            finished = true;
        }
        if (ImGui::Checkbox("Also match the brightness", &settings.match_brightness)) {
            finished = true;
        }
        ImGui::SetItemTooltip("Off: the other files get the spot's colour tone and keep their "
                              "own brightness. On: also its brightness (evens out exposure, but "
                              "may brighten a dark video a lot)");
        ImGui::EndGroup();
        if (finished) {
            save();
        } else if (changed) {
            update(); // shown at once, saved when the slider is let go
        }
    }

    void draw_result() {
        ImGui::SeparatorText("3. Result");
        const float right_edge = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        for (std::size_t i = 0; i < files.size(); ++i) {
            next_card(i, kResultWidth, right_edge);
            ImGui::BeginGroup();
            const BalancedFile& file = files[i];
            if (SDL_Texture* texture = texture_of(file, "result", settings.files[i].gains)) {
                ImGui::Image(ImTextureRef(reinterpret_cast<ImTextureID>(texture)),
                             extent_of(file, kResultWidth));
            } else {
                ImGui::Button("...", extent_of(file, kResultWidth));
            }
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + kResultWidth);
            ImGui::TextColored(file.name == settings.reference ? kReferenceColor : kMuted, "%s%s",
                               file.label.c_str(),
                               file.name == settings.reference ? " (reference)" : "");
            ImGui::PopTextWrapPos();
            ImGui::EndGroup();
        }
    }
};

WhiteBalancePanel::WhiteBalancePanel(const GuiContext& gui, launcher::VideoPreview& preview,
                                     cutting::WhiteBalanceStore& store)
    : impl_(std::make_unique<Impl>(gui, preview, store)) {}

WhiteBalancePanel::~WhiteBalancePanel() = default;

void WhiteBalancePanel::draw(const launcher::WorkspaceSnapshot& workspace,
                             const std::string& day) {
    impl_->draw(workspace, day);
}

} // namespace ttrally::gui
