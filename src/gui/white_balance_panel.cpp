// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "gui/white_balance_panel.hpp"

#include "gui/frame_texture.hpp"
#include "gui/gui_context.hpp"
#include "launcher/application/detect_scenes.hpp"
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
#include <string>
#include <vector>

namespace ttrally::gui {

namespace {

using cutting::FileWhiteBalance;
using cutting::RelativeRect;
using cutting::RgbColor;
using cutting::RgbGains;
using cutting::WhiteBalanceChoice;

constexpr int kBackgroundHeight = 216;     ///< lines of the backgrounds read
constexpr unsigned kParallelFiles = 4;     ///< each decode uses several threads itself
constexpr float kAreaImageWidth = 560.0F;  ///< the scene's picture the area is dragged on
constexpr float kThumbnailWidth = 170.0F;  ///< a choice's picture
constexpr float kFileLabelWidth = 260.0F;
constexpr int kLimitedBlack = 16;
constexpr int kChromaZero = 128;
const ImVec4 kMuted{0.6F, 0.6F, 0.65F, 1.0F};
const ImVec4 kPartly{0.95F, 0.75F, 0.35F, 1.0F};
const ImU32 kAreaColor = IM_COL32(255, 80, 80, 255);
const ImU32 kChosenColor = IM_COL32(90, 170, 255, 255);

/// A file of the match day whose white balance is chosen: a single video or a part of a match.
struct BalancedFile {
    std::string name;            ///< file name in the match day folder
    std::filesystem::path path;  ///< the file
    std::string label;           ///< e.g. "kaether-baumgarten, part 2"
    std::string scene;
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
            files.push_back({path.filename().string(), path, video.video_id, video.scene});
        }
        for (std::size_t i = 0; i < video.parts.size(); ++i) {
            const std::filesystem::path path(video.parts[i]);
            files.push_back({path.filename().string(), path,
                             std::format("{}, part {}", video.video_id, i + 1), video.scene});
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

const char* choice_label(WhiteBalanceChoice choice) {
    switch (choice) {
    case WhiteBalanceChoice::Neutral:
        return "Neutral";
    case WhiteBalanceChoice::Like:
        return "Like";
    case WhiteBalanceChoice::Unchanged:
        break;
    }
    return "As recorded";
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

    using Backgrounds = std::map<std::filesystem::path, std::optional<media::VideoFrame>>;

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
    Backgrounds backgrounds;
    std::future<Backgrounds> job;
    std::map<std::string, Shown> shown; ///< by "<file>|<variant>"
    std::int64_t uploads = 0;           ///< a new texture content each time
    std::optional<std::string> dragging; ///< scene whose area is being dragged
    ImVec2 drag_start{};
    RelativeRect dragged{};
    std::string error;

    void open(const launcher::WorkspaceSnapshot& workspace, const std::string& match_day) {
        files = files_of(workspace, match_day);
        if (match_day == day) {
            return;
        }
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

    /// Starts reading the backgrounds of files that have none yet.
    void read_missing() {
        if (job.valid()) {
            if (job.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                return;
            }
            for (auto& [path, frame] : job.get()) {
                backgrounds[path] = std::move(frame);
            }
        }
        std::vector<std::filesystem::path> missing;
        for (const BalancedFile& file : files) {
            if (!backgrounds.contains(file.path)) {
                missing.push_back(file.path);
            }
        }
        if (missing.empty()) {
            return;
        }
        job = std::async(std::launch::async, [&preview = preview, missing] {
            std::vector<std::optional<media::VideoFrame>> read(missing.size());
            parallel_for(missing.size(), kParallelFiles, [&](std::size_t i) {
                read[i] = launcher::color_background(preview, missing[i], kBackgroundHeight);
            });
            Backgrounds found;
            for (std::size_t i = 0; i < missing.size(); ++i) {
                found[missing[i]] = std::move(read[i]);
            }
            return found;
        });
    }

    [[nodiscard]] const media::VideoFrame* background(const BalancedFile& file) const {
        const auto found = backgrounds.find(file.path);
        return found == backgrounds.end() || !found->second ? nullptr : &*found->second;
    }

    [[nodiscard]] std::optional<RelativeRect> area_of(const std::string& scene) const {
        const auto found = std::ranges::find(settings.areas, scene,
                                             &std::pair<std::string, RelativeRect>::first);
        return found == settings.areas.end() ? std::nullopt : std::optional(found->second);
    }

    FileWhiteBalance& balance_of(const BalancedFile& file) {
        auto found = std::ranges::find(settings.files, file.name, &FileWhiteBalance::file);
        if (found == settings.files.end()) {
            settings.files.push_back({file.name, WhiteBalanceChoice::Unchanged, "", {}});
            found = std::prev(settings.files.end());
        }
        return *found;
    }

    [[nodiscard]] std::optional<RgbColor> measured(const BalancedFile& file) const {
        const media::VideoFrame* frame = background(file);
        const auto area = area_of(file.scene);
        return frame == nullptr || !area ? std::nullopt : color_in(*frame, *area);
    }

    /// The gains for a choice of a file (identity if its colour or the other's is unknown).
    [[nodiscard]] RgbGains gains_for(const BalancedFile& file, WhiteBalanceChoice choice,
                                     const std::string& like) const {
        const auto own = measured(file);
        if (!own || choice == WhiteBalanceChoice::Unchanged) {
            return {};
        }
        if (choice == WhiteBalanceChoice::Neutral) {
            return cutting::neutral_gains(*own);
        }
        const auto other = std::ranges::find(files, like, &BalancedFile::name);
        const auto reference = other == files.end() ? std::nullopt : measured(*other);
        return reference ? cutting::matching_gains(*own, *reference) : RgbGains{};
    }

    void save() {
        try {
            store.save(work_dir, settings);
            error.clear();
        } catch (const std::exception& failure) {
            error = std::format("Saving the white balance: {}", failure.what());
        }
    }

    /// After the area of a scene changed: the gains of its files again.
    void update_gains(const std::string& scene) {
        for (const BalancedFile& file : files) {
            if (file.scene == scene) {
                FileWhiteBalance& balance = balance_of(file);
                balance.gains = gains_for(file, balance.choice, balance.like);
            }
        }
    }

    /// The texture of a file's background with gains (made again when the gains changed).
    SDL_Texture* picture(const BalancedFile& file, const std::string& variant, RgbGains gains) {
        const media::VideoFrame* frame = background(file);
        if (frame == nullptr) {
            return nullptr;
        }
        Shown& entry = shown[file.name + "|" + variant];
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

    void draw(const launcher::WorkspaceSnapshot& workspace, const std::string& match_day) {
        open(workspace, match_day);
        read_missing();
        ImGui::TextWrapped(
            "Correct the white balance of videos recorded with automatic white balance. Per "
            "scene (same hall and camera position), drag a rectangle over something that should "
            "be white or grey, e.g. the table's white edge or a wall. Then choose for every "
            "file (each part of a match on its own) how it should look: as recorded, neutral, or "
            "like another file of the scene. Cut and highlights apply it; the labels, features "
            "and detections keep the colours as recorded.");
        if (job.valid()) {
            ImGui::TextColored(kPartly, "Reading the backgrounds of the files (a few seconds "
                                        "each)...");
        }
        if (!error.empty()) {
            ImGui::TextColored(ImVec4(1.0F, 0.45F, 0.4F, 1.0F), "%s", error.c_str());
        }
        std::vector<std::string> scenes;
        for (const BalancedFile& file : files) {
            if (std::ranges::find(scenes, file.scene) == scenes.end()) {
                scenes.push_back(file.scene);
            }
        }
        std::ranges::sort(scenes);
        for (const std::string& scene : scenes) {
            ImGui::PushID(scene.c_str());
            if (scene.empty()) {
                ImGui::SeparatorText("Files without a scene");
                ImGui::TextColored(kMuted, "Detect the scenes of the match day first (button "
                                           "above): the area is set per scene.");
            } else {
                draw_scene(scene);
            }
            ImGui::PopID();
        }
    }

    void draw_scene(const std::string& scene) {
        ImGui::SeparatorText(std::format("Scene {}", scene).c_str());
        const auto first = std::ranges::find(files, scene, &BalancedFile::scene);
        draw_area(scene, *first);
        const bool has_area = area_of(scene).has_value();
        for (const BalancedFile& file : files) {
            if (file.scene == scene) {
                draw_choices(file, scene, has_area);
            }
        }
    }

    /// The scene's picture (its first file's background) with the area, dragged anew.
    void draw_area(const std::string& scene, const BalancedFile& file) {
        SDL_Texture* texture = picture(file, "area", {});
        if (texture == nullptr) {
            ImGui::TextColored(kMuted, "No background of %s yet", file.name.c_str());
            return;
        }
        const float width = std::min(kAreaImageWidth, ImGui::GetContentRegionAvail().x);
        const media::FrameSize size = background(file)->size;
        const ImVec2 extent{width, width * static_cast<float>(size.height) /
                                       static_cast<float>(size.width)};
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::Image(ImTextureRef(reinterpret_cast<ImTextureID>(texture)), extent);
        ImGui::SetCursorScreenPos(origin);
        ImGui::InvisibleButton("##area", extent);
        const auto relative = [&](ImVec2 point) {
            return ImVec2{std::clamp((point.x - origin.x) / extent.x, 0.0F, 1.0F),
                          std::clamp((point.y - origin.y) / extent.y, 0.0F, 1.0F)};
        };
        if (ImGui::IsItemActivated()) {
            dragging = scene;
            drag_start = relative(ImGui::GetIO().MousePos);
        }
        const bool mine = dragging == scene;
        if (mine) {
            const ImVec2 now = relative(ImGui::GetIO().MousePos);
            dragged = {std::min(drag_start.x, now.x), std::min(drag_start.y, now.y),
                       std::max(drag_start.x, now.x), std::max(drag_start.y, now.y)};
        }
        if (mine && ImGui::IsItemDeactivated()) {
            dragging.reset();
            if (!dragged.empty()) {
                auto found = std::ranges::find(settings.areas, scene,
                                               &std::pair<std::string, RelativeRect>::first);
                if (found == settings.areas.end()) {
                    settings.areas.emplace_back(scene, dragged);
                } else {
                    found->second = dragged;
                }
                update_gains(scene);
                save();
            }
        }
        ImGui::SetItemTooltip("Drag a rectangle over something that should be white or grey");
        const std::optional<RelativeRect> area = mine ? std::optional(dragged) : area_of(scene);
        if (area) {
            ImGui::GetWindowDrawList()->AddRect(
                {origin.x + static_cast<float>(area->left) * extent.x,
                 origin.y + static_cast<float>(area->top) * extent.y},
                {origin.x + static_cast<float>(area->right) * extent.x,
                 origin.y + static_cast<float>(area->bottom) * extent.y},
                kAreaColor, 0.0F, 0, 2.0F);
        }
        ImGui::TextColored(kMuted, "Background of %s (the players removed)%s", file.name.c_str(),
                           area ? "" : ": drag the area");
    }

    /// The choices of one file, each as its background with that white balance.
    void draw_choices(const BalancedFile& file, const std::string& scene, bool has_area) {
        ImGui::PushID(file.name.c_str());
        ImGui::Spacing();
        ImGui::BeginGroup();
        ImGui::TextUnformatted(file.label.c_str());
        ImGui::SetItemTooltip("%s", file.path.string().c_str());
        ImGui::SameLine(kFileLabelWidth);
        ImGui::TextColored(kMuted, "%s", file.name.c_str());
        FileWhiteBalance& chosen = balance_of(file);
        struct Variant {
            WhiteBalanceChoice choice;
            std::string like;
            std::string caption;
        };
        std::vector<Variant> variants{{WhiteBalanceChoice::Unchanged, "", "As recorded"},
                                      {WhiteBalanceChoice::Neutral, "", "Neutral"}};
        for (const BalancedFile& other : files) {
            if (other.scene == scene && other.name != file.name) {
                variants.push_back({WhiteBalanceChoice::Like, other.name,
                                    std::format("Like {}", other.label)});
            }
        }
        const ImGuiStyle& style = ImGui::GetStyle();
        const float right_edge = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        for (std::size_t i = 0; i < variants.size(); ++i) {
            const Variant& variant = variants[i];
            // Next to the previous one while it fits, else on a new line
            if (i > 0 && ImGui::GetItemRectMax().x + style.ItemSpacing.x + kThumbnailWidth +
                                 2 * style.FramePadding.x <=
                             right_edge) {
                ImGui::SameLine();
            }
            ImGui::BeginGroup();
            const RgbGains gains = gains_for(file, variant.choice, variant.like);
            const std::string key =
                std::format("{}:{}", choice_label(variant.choice), variant.like);
            SDL_Texture* texture = picture(file, key, gains);
            const bool usable = variant.choice == WhiteBalanceChoice::Unchanged || has_area;
            const bool selected = chosen.choice == variant.choice &&
                                  (variant.choice != WhiteBalanceChoice::Like ||
                                   chosen.like == variant.like);
            const media::VideoFrame* frame = background(file);
            const float shape = frame == nullptr ? 9.0F / 16.0F
                                                 : static_cast<float>(frame->size.height) /
                                                       static_cast<float>(frame->size.width);
            const ImVec2 extent{kThumbnailWidth, kThumbnailWidth * shape};
            ImGui::BeginDisabled(!usable || texture == nullptr);
            ImGui::PushID(static_cast<int>(i));
            const bool clicked =
                texture != nullptr
                    ? ImGui::ImageButton("##choice",
                                         ImTextureRef(reinterpret_cast<ImTextureID>(texture)),
                                         extent)
                    : ImGui::Button("...", extent);
            ImGui::PopID();
            ImGui::EndDisabled();
            if (selected) {
                ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(),
                                                    ImGui::GetItemRectMax(), kChosenColor, 0.0F,
                                                    0, 3.0F);
            }
            if (clicked && usable) {
                chosen.choice = variant.choice;
                chosen.like = variant.like;
                chosen.gains = gains;
                save();
            }
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + extent.x);
            ImGui::TextColored(selected ? ImVec4(0.55F, 0.75F, 1.0F, 1.0F) : kMuted, "%s",
                               variant.caption.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndGroup();
        }
        ImGui::EndGroup();
        ImGui::PopID();
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
