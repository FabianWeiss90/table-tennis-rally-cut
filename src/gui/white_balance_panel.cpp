// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "gui/white_balance_panel.hpp"

#include "gui/frame_texture.hpp"
#include "gui/gui_context.hpp"
#include "cutting/domain/color_match.hpp"
#include "shared/io/formatting.hpp"
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

constexpr int kPictureHeight = 540;         ///< lines of the pictures read
constexpr double kPictureFraction = 0.5;    ///< where in a file its picture is taken
constexpr unsigned kParallelFiles = 4;      ///< each decode uses several threads itself
constexpr float kAreaCardWidth = 640.0F;    ///< a file's picture the area is set on
constexpr float kReferenceWidth = 640.0F;   ///< the reference's picture
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
constexpr int kReferenceFrames = 9;   ///< read from the reference rally for its median
constexpr double kSameTime = 1e-3;     ///< seconds within which two rallies start together
constexpr double kFallbackFps = 30.0;

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

/// The frame as a small RGB picture (every `step`th pixel; limited range, BT.709), for measuring.
cutting::RgbImage rgb_image_of(const media::VideoFrame& frame, int step = 4) {
    cutting::RgbImage picture;
    picture.width = frame.size.width / step;
    picture.height = frame.size.height / step;
    const auto y_plane = frame.y();
    const auto u_plane = frame.u();
    const auto v_plane = frame.v();
    const auto width = static_cast<std::size_t>(frame.size.width);
    for (int y = 0; y < picture.height; ++y) {
        for (int x = 0; x < picture.width; ++x) {
            const auto fx = static_cast<std::size_t>(x * step);
            const auto fy = static_cast<std::size_t>(y * step);
            const std::size_t c = (fy / 2) * (width / 2) + fx / 2;
            const RgbColor color =
                cutting::rgb_of(y_plane[fy * width + fx], u_plane[c], v_plane[c]);
            for (const double channel : {color.red, color.green, color.blue}) {
                picture.rgb.push_back(
                    static_cast<std::uint8_t>(std::clamp(std::lround(channel), 0L, 255L)));
            }
        }
    }
    return picture;
}

/// The frame through the curves (limited range, BT.709), as the cut applies them.
media::VideoFrame curved(media::VideoFrame frame, const cutting::ToneCurves& curves) {
    if (curves.is_identity()) {
        return frame;
    }
    constexpr double kr = 0.2126;
    constexpr double kb = 0.0722;
    constexpr double kg = 1.0 - kr - kb;
    constexpr double kLuma = 255.0 / 219.0;
    constexpr double kChroma = 255.0 / 224.0;
    const int width = frame.size.width;
    const int height = frame.size.height;
    const auto luma_size = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    std::uint8_t* y_plane = frame.planes.data();
    std::uint8_t* u_plane = y_plane + luma_size;
    std::uint8_t* v_plane = u_plane + luma_size / 4;
    const auto clamp = [](double value) {
        return static_cast<std::uint8_t>(std::clamp(std::lround(value), 0L, 255L));
    };
    const auto through = [&](int channel, double value) {
        return static_cast<double>(curves.curve[static_cast<std::size_t>(channel)][clamp(value)]);
    };
    for (int cy = 0; cy < height / 2; ++cy) {
        for (int cx = 0; cx < width / 2; ++cx) {
            const auto c = static_cast<std::size_t>(cy) * static_cast<std::size_t>(width / 2) +
                           static_cast<std::size_t>(cx);
            const double u = (u_plane[c] - kChromaZero) * kChroma;
            const double v = (v_plane[c] - kChromaZero) * kChroma;
            double sums[3] = {0, 0, 0};
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    const auto at = static_cast<std::size_t>(2 * cy + dy) *
                                        static_cast<std::size_t>(width) +
                                    static_cast<std::size_t>(2 * cx + dx);
                    const double y = (y_plane[at] - kLimitedBlack) * kLuma;
                    const double red = through(0, y + 2.0 * (1.0 - kr) * v);
                    const double green = through(
                        1, y - 2.0 * kb * (1.0 - kb) / kg * u - 2.0 * kr * (1.0 - kr) / kg * v);
                    const double blue = through(2, y + 2.0 * (1.0 - kb) * u);
                    sums[0] += red;
                    sums[1] += green;
                    sums[2] += blue;
                    const double luma = kr * red + kg * green + kb * blue;
                    y_plane[at] = clamp(luma / kLuma + kLimitedBlack);
                }
            }
            const double red = sums[0] / 4;
            const double green = sums[1] / 4;
            const double blue = sums[2] / 4;
            const double luma = kr * red + kg * green + kb * blue;
            u_plane[c] = clamp((blue - luma) / (2.0 * (1.0 - kb)) / kChroma + kChromaZero);
            v_plane[c] = clamp((red - luma) / (2.0 * (1.0 - kr)) / kChroma + kChromaZero);
        }
    }
    return frame;
}

/// A rally offered as reference.
struct ReferenceRally {
    double start_s = 0.0;
    double end_s = 0.0;
    std::string label; ///< e.g. "#12  0:14:03  6.2 s"
};

} // namespace

struct WhiteBalancePanel::Impl {
    Impl(const GuiContext& gui, launcher::VideoPreview& preview, cutting::WhiteBalanceStore& store,
         highlights::MatchDayHighlights* days)
        : renderer(gui.renderer()), preview(preview), store(store), days(days) {}
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;
    ~Impl() {
        if (job.valid()) {
            job.wait();
        }
        if (reference_job.valid()) {
            reference_job.wait();
        }
    }

    using Pictures = std::map<std::filesystem::path, std::optional<media::VideoFrame>>;

    /// The reference rally as read: a frame from its middle (shown) and the median of its
    /// frames (measured).
    struct ReferencePicture {
        std::optional<media::VideoFrame> frame;
        cutting::RgbImage median;
    };

    /// A picture shown, with the revision of the settings it was made for.
    struct Shown {
        std::unique_ptr<FrameTexture> texture;
        std::uint64_t revision = 0;
        bool valid = false;
    };

    SDL_Renderer* renderer;
    launcher::VideoPreview& preview;
    cutting::WhiteBalanceStore& store;
    highlights::MatchDayHighlights* days;
    std::string day;
    std::filesystem::path work_dir;
    std::vector<launcher::VideoEntry> videos; ///< of the match day
    cutting::WhiteBalanceSettings settings;
    std::uint64_t revision = 1; ///< counts the changes of the settings
    std::vector<BalancedFile> files;
    Pictures pictures; ///< a frame from the middle of each file
    std::future<Pictures> job;
    std::map<std::string, std::vector<ReferenceRally>> rallies; ///< by video id, once read
    std::optional<ReferencePicture> reference;
    std::future<ReferencePicture> reference_job;
    std::string reference_read; ///< what `reference` shows: video|start|end
    std::map<std::string, Shown> shown; ///< by "<file>|<use>"
    std::int64_t uploads = 0;           ///< a new texture content each time
    std::optional<std::string> dragging; ///< file whose area is being dragged
    ImVec2 drag_start{};
    RelativeRect drag_origin{}; ///< the area moved, as it was when the drag began
    RelativeRect dragged{};
    std::string error;

    void open(const launcher::WorkspaceSnapshot& workspace, const std::string& match_day) {
        files = files_of(workspace, match_day);
        videos.clear();
        for (const launcher::VideoEntry& video : workspace.videos) {
            if (video.match_day == match_day) {
                videos.push_back(video);
            }
        }
        if (match_day != day) {
            day = match_day;
            work_dir = videos.empty() ? std::filesystem::path()
                                      : std::filesystem::path(videos.front().work_dir);
            rallies.clear();
            reference.reset();
            reference_read.clear();
            try {
                settings = store.load(work_dir);
                error.clear();
            } catch (const std::exception& failure) {
                settings = {};
                error = std::format("White balance of {}: {}", day, failure.what());
            }
            ++revision;
        }
        // One entry per file, in the list's order (an area is taken from the file before)
        std::vector<FileWhiteBalance> ordered;
        for (const BalancedFile& file : files) {
            const auto stored =
                std::ranges::find(settings.files, file.name, &FileWhiteBalance::file);
            ordered.push_back(stored != settings.files.end() ? *stored
                                                             : FileWhiteBalance{file.name, {}});
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
            ++revision;
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

    /// The video of the reference rally, if it is one of the match day's.
    [[nodiscard]] const launcher::VideoEntry* reference_video() const {
        for (const launcher::VideoEntry& video : videos) {
            if (relative_to_day(video) == settings.reference_video) {
                return &video;
            }
        }
        return nullptr;
    }

    /// A video's path in the match day folder, as the settings keep it.
    [[nodiscard]] static std::string relative_to_day(const launcher::VideoEntry& video) {
        const std::filesystem::path day_folder =
            std::filesystem::path(video.work_dir).parent_path();
        return std::filesystem::path(video.original)
            .lexically_normal()
            .lexically_relative(day_folder.lexically_normal())
            .generic_string();
    }

    /// The area of the reference rally: that of its video's file at the rally's middle.
    [[nodiscard]] std::optional<RelativeRect> reference_area() const {
        const launcher::VideoEntry* video = reference_video();
        if (video == nullptr) {
            return std::nullopt;
        }
        std::string name = std::filesystem::path(video->original).filename().string();
        if (!video->parts.empty()) {
            const double middle = (settings.reference_start_s + settings.reference_end_s) / 2;
            std::size_t part = 0;
            while (part + 1 < video->part_starts_s.size() &&
                   video->part_starts_s[part + 1] <= middle) {
                ++part;
            }
            name = std::filesystem::path(video->parts[std::min(part, video->parts.size() - 1)])
                       .filename()
                       .string();
        }
        const auto areas = cutting::effective_areas(settings.files);
        for (std::size_t i = 0; i < settings.files.size(); ++i) {
            if (settings.files[i].file == name) {
                return areas[i];
            }
        }
        return std::nullopt;
    }

    /// Starts reading the reference rally when it changed; takes the result when ready.
    void read_reference() {
        if (reference_job.valid() &&
            reference_job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            reference = reference_job.get();
            ++revision;
        }
        const launcher::VideoEntry* video = reference_video();
        if (video == nullptr || !settings.has_reference() || reference_job.valid()) {
            return;
        }
        const std::string key = std::format("{}|{}|{}", settings.reference_video,
                                            settings.reference_start_s, settings.reference_end_s);
        if (key == reference_read) {
            return;
        }
        reference_read = key;
        reference.reset();
        reference_job = std::async(std::launch::async, [&preview = preview,
                                                        path = std::filesystem::path(
                                                            video->original),
                                                        start = settings.reference_start_s,
                                                        end = settings.reference_end_s] {
            ReferencePicture picture;
            std::vector<cutting::RgbImage> samples;
            for (int k = 0; k < kReferenceFrames; ++k) {
                const double t = start + (end - start) * (k + 0.5) / kReferenceFrames;
                if (auto frame = preview.frame_at_time(path, t, kPictureHeight)) {
                    samples.push_back(rgb_image_of(*frame));
                    if (k == kReferenceFrames / 2) {
                        picture.frame = std::move(frame);
                    }
                }
            }
            picture.median = cutting::median_of(samples);
            return picture;
        });
    }

    /// What every rally is corrected towards, once the reference is read.
    [[nodiscard]] std::optional<cutting::ColorTarget> target() const {
        if (!reference || reference->median.empty()) {
            return std::nullopt;
        }
        return cutting::tuned_target(reference->median, reference_area(), settings);
    }

    void save() {
        ++revision;
        try {
            store.save(work_dir, settings);
            error.clear();
        } catch (const std::exception& failure) {
            error = std::format("Saving the white balance: {}", failure.what());
        }
    }

    /// The texture of a picture made from a frame, made again when the settings changed.
    template <typename Make>
    SDL_Texture* texture_of(const std::string& key, const media::VideoFrame* frame,
                            std::uint64_t made_for, Make make) {
        if (frame == nullptr) {
            return nullptr;
        }
        Shown& entry = shown[key];
        if (!entry.texture) {
            entry.texture = std::make_unique<FrameTexture>(renderer);
        }
        if (!entry.valid || entry.revision != made_for) {
            media::VideoFrame made = make(*frame);
            made.index = ++uploads;
            entry.texture->show(made);
            entry.revision = made_for;
            entry.valid = true;
        }
        return entry.texture->texture();
    }

    [[nodiscard]] static ImVec2 extent_for(const media::VideoFrame* frame, float width) {
        const float shape = frame == nullptr ? kDefaultShape
                                             : static_cast<float>(frame->size.height) /
                                                   static_cast<float>(frame->size.width);
        return {width, width * shape};
    }

    [[nodiscard]] ImVec2 extent_of(const BalancedFile& file, float width) const {
        return extent_for(picture_of(file), width);
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
        read_reference();
        ImGui::TextWrapped(
            "Correct the colours of videos recorded with automatic white balance and exposure. "
            "1: in the first file, drag a rectangle over a spot that should look the same in all "
            "of them; in the others it appears where it is in the file before and can be moved "
            "onto the same spot (each part of a match on its own). 2: choose a reference rally "
            "and tune it with temperature, tint and brightness. When cutting, every rally is "
            "measured on its own frames and corrected towards it: the spot's colour tone, and "
            "if chosen the brightness of the whole picture. 3 shows the middle of each file "
            "corrected that way. Labels, features and detections keep the colours as "
            "recorded.");
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

    /// A file's picture with its area: drawn in the first file, only moved in the others.
    void draw_area_card(std::size_t i, const std::optional<RelativeRect>& area) {
        const BalancedFile& file = files[i];
        FileWhiteBalance& balance = settings.files[i];
        const bool first = i == 0;
        ImGui::TextUnformatted(file.label.c_str());
        ImGui::SetItemTooltip("%s", file.path.string().c_str());
        SDL_Texture* texture =
            texture_of(file.name + "|area", picture_of(file), 0,
                       [](const media::VideoFrame& frame) { return frame; });
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
        const bool movable = first || area.has_value();
        if (ImGui::IsItemActivated() && movable) {
            dragging = file.name;
            drag_start = relative(ImGui::GetIO().MousePos);
            drag_origin = area.value_or(RelativeRect{});
        }
        const bool mine = dragging == file.name;
        if (mine) {
            const ImVec2 now = relative(ImGui::GetIO().MousePos);
            if (first) { // drawn anew
                dragged = {std::min(drag_start.x, now.x), std::min(drag_start.y, now.y),
                           std::max(drag_start.x, now.x), std::max(drag_start.y, now.y)};
            } else { // moved, its size kept
                const double width = drag_origin.right - drag_origin.left;
                const double height = drag_origin.bottom - drag_origin.top;
                const double left =
                    std::clamp(drag_origin.left + now.x - drag_start.x, 0.0, 1.0 - width);
                const double top =
                    std::clamp(drag_origin.top + now.y - drag_start.y, 0.0, 1.0 - height);
                dragged = {left, top, left + width, top + height};
            }
        }
        if (mine && ImGui::IsItemDeactivated()) {
            dragging.reset();
            if (!dragged.empty()) {
                balance.area = dragged;
                save();
            }
        }
        ImGui::SetItemTooltip("%s", first ? "Drag a rectangle over a spot that looks the same "
                                            "in every file"
                                          : "Drag to move the rectangle onto the same spot");
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
        if (first) {
            ImGui::TextColored(kMuted, "%s", area ? "Drag anew to change the rectangle"
                                                  : "Drag a rectangle over the spot");
        } else if (!area) {
            ImGui::TextColored(kMuted, "Set the rectangle in the first file");
        } else if (balance.area) {
            if (ImGui::SmallButton("Back to the file before")) {
                balance.area.reset();
                save();
            }
            ImGui::SetItemTooltip("Places the rectangle where it is in the file before");
        } else {
            ImGui::TextColored(kMuted, "Where it is in the file before: drag to move it");
        }
    }

    /// The rallies of a video from its cut list, read once.
    const std::vector<ReferenceRally>& rallies_of(const launcher::VideoEntry& video) {
        auto found = rallies.find(video.video_id);
        if (found != rallies.end()) {
            return found->second;
        }
        std::vector<ReferenceRally> list;
        if (days != nullptr) {
            try {
                if (const auto read = days->rallies(video.work_dir)->rallies(video.video_id)) {
                    const double fps = read->fps > 0 ? read->fps : kFallbackFps;
                    for (std::size_t i = 0; i < read->rallies.size(); ++i) {
                        const auto& rally = read->rallies[i];
                        const double start = static_cast<double>(rally.start_frame) / fps;
                        const double end = static_cast<double>(rally.end_frame + 1) / fps;
                        list.push_back({start, end,
                                        std::format("#{}  {}  {:.1f} s", i + 1,
                                                    io::format_clock(start), end - start)});
                    }
                }
            } catch (const std::exception& failure) {
                error = std::format("Rallies of {}: {}", video.video_id, failure.what());
            }
        }
        return rallies.emplace(video.video_id, std::move(list)).first->second;
    }

    void draw_reference() {
        ImGui::SeparatorText("2. The reference rally, tuned");
        const launcher::VideoEntry* current = reference_video();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Video");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(320);
        if (ImGui::BeginCombo("##video", current != nullptr ? current->video_id.c_str()
                                                            : "Choose...")) {
            for (const launcher::VideoEntry& video : videos) {
                if (ImGui::Selectable(video.video_id.c_str(), &video == current)) {
                    settings.reference_video = relative_to_day(video);
                    settings.reference_start_s = 0;
                    settings.reference_end_s = 0;
                    save();
                }
            }
            ImGui::EndCombo();
        }
        if (current == nullptr) {
            ImGui::TextColored(kMuted, "Choose the video of the reference rally.");
            return;
        }
        const std::vector<ReferenceRally>& choices = rallies_of(*current);
        ImGui::SameLine();
        ImGui::TextUnformatted("Rally");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(260);
        const auto chosen = std::ranges::find_if(choices, [&](const ReferenceRally& rally) {
            return std::abs(rally.start_s - settings.reference_start_s) < kSameTime;
        });
        if (ImGui::BeginCombo("##rally", chosen != choices.end() ? chosen->label.c_str()
                                                                 : "Choose...")) {
            for (const ReferenceRally& rally : choices) {
                if (ImGui::Selectable(rally.label.c_str(), chosen != choices.end() &&
                                                               &rally == &*chosen)) {
                    settings.reference_start_s = rally.start_s;
                    settings.reference_end_s = rally.end_s;
                    save();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("The rally every other is made to look like; from the video's "
                              "cut list (labels, refined or detected rallies)");
        if (choices.empty()) {
            ImGui::TextColored(kMuted, "%s has no rallies yet: run detect (or annotate it).",
                               current->video_id.c_str());
            return;
        }
        if (!settings.has_reference()) {
            return;
        }
        if (!reference) {
            ImGui::TextColored(kPartly, "Reading the reference rally...");
            return;
        }
        const RgbGains tuning = reference_gains();
        ImGui::BeginGroup();
        if (SDL_Texture* texture =
                texture_of("reference", reference->frame ? &*reference->frame : nullptr,
                           revision, [&](const media::VideoFrame& frame) {
                               return balanced(frame, tuning);
                           })) {
            ImGui::Image(ImTextureRef(reinterpret_cast<ImTextureID>(texture)),
                         extent_for(&*reference->frame, kReferenceWidth));
        }
        ImGui::EndGroup();
        ImGui::SameLine();
        ImGui::BeginGroup();
        draw_tuning();
        ImGui::EndGroup();
    }

    /// The gains of the reference's tuning, measured on its area (else the whole picture).
    [[nodiscard]] RgbGains reference_gains() const {
        if (!reference || reference->median.empty()) {
            return {};
        }
        const RgbColor measured = cutting::mean_in(
            reference->median, reference_area().value_or(RelativeRect{0.0, 0.0, 1.0, 1.0}));
        return cutting::temperature_tint_gains(settings.temperature, settings.tint,
                                               settings.brightness, measured);
    }

    void draw_tuning() {
        bool changed = false;
        bool finished = false;
        auto slider = [&](const char* label, double& value, const char* help) {
            auto shown_value = static_cast<float>(value * kSliderScale);
            ImGui::SetNextItemWidth(kSliderWidth);
            if (ImGui::SliderFloat(label, &shown_value, -kSliderScale, kSliderScale, "%.0f")) {
                value = shown_value / kSliderScale;
                changed = true;
            }
            finished |= ImGui::IsItemDeactivatedAfterEdit();
            ImGui::SetItemTooltip("%s", help);
        };
        slider("Temperature", settings.temperature, "Higher: warmer (more red, less blue)");
        slider("Tint", settings.tint, "Higher: more magenta, less green");
        slider("Brightness", settings.brightness,
               "Higher: brighter (up to 1.5 times), lower: darker (down to half)");
        if (ImGui::Button("Area neutral")) {
            if (const auto area = reference_area(); area && reference) {
                std::tie(settings.temperature, settings.tint) = cutting::neutral_temperature_tint(
                    cutting::mean_in(reference->median, *area));
                finished = true;
            }
        }
        ImGui::SetItemTooltip("Temperature and tint that make the spot grey in the reference "
                              "rally: a start for white or grey spots");
        ImGui::SameLine();
        if (ImGui::Button("Reset")) {
            settings.temperature = 0;
            settings.tint = 0;
            settings.brightness = 0;
            finished = true;
        }
        if (ImGui::Checkbox("Also match the brightness", &settings.match_brightness)) {
            finished = true;
        }
        ImGui::SetItemTooltip("Off: every rally gets the spot's colour tone of the reference and "
                              "keeps its own brightness. On: its whole picture also becomes as "
                              "bright on average as the reference's (evens out exposure)");
        if (finished) {
            save();
        } else if (changed) {
            ++revision; // shown at once, saved when the slider is let go
        }
    }

    void draw_result() {
        ImGui::SeparatorText("3. Result (the middle of each file)");
        const std::optional<cutting::ColorTarget> wanted = target();
        if (!wanted) {
            ImGui::TextColored(kMuted, "Choose and read the reference rally first.");
            return;
        }
        ImGui::TextColored(kMuted, "When cutting, every rally is measured on its own frames; "
                                   "here a frame from each file's middle stands for them.");
        const auto areas = cutting::effective_areas(settings.files);
        const float right_edge = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        for (std::size_t i = 0; i < files.size(); ++i) {
            next_card(i, kResultWidth, right_edge);
            ImGui::BeginGroup();
            const BalancedFile& file = files[i];
            const media::VideoFrame* frame = picture_of(file);
            SDL_Texture* texture = texture_of(
                file.name + "|result", frame, revision, [&](const media::VideoFrame& picture) {
                    const cutting::ToneCurves curves = cutting::rally_correction(
                        rgb_image_of(picture), areas[i], *wanted, settings);
                    return curved(picture, curves);
                });
            if (texture != nullptr) {
                ImGui::Image(ImTextureRef(reinterpret_cast<ImTextureID>(texture)),
                             extent_of(file, kResultWidth));
            } else {
                ImGui::Button("...", extent_of(file, kResultWidth));
            }
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + kResultWidth);
            ImGui::TextColored(kMuted, "%s", file.label.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndGroup();
        }
    }
};

WhiteBalancePanel::WhiteBalancePanel(const GuiContext& gui, launcher::VideoPreview& preview,
                                     cutting::WhiteBalanceStore& store,
                                     highlights::MatchDayHighlights* days)
    : impl_(std::make_unique<Impl>(gui, preview, store, days)) {}

WhiteBalancePanel::~WhiteBalancePanel() = default;

void WhiteBalancePanel::draw(const launcher::WorkspaceSnapshot& workspace,
                             const std::string& day) {
    impl_->draw(workspace, day);
}

} // namespace ttrally::gui
