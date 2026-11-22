// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "launcher/infrastructure/library_scanner.hpp"

#include "annotation/infrastructure/csv_annotation_repository.hpp"
#include "annotation/infrastructure/csv_review_state_store.hpp"
#include "launcher/domain/video_id.hpp"
#include "launcher/infrastructure/csv_match_store.hpp"
#include "launcher/infrastructure/csv_pre_cut_store.hpp"
#include "launcher/infrastructure/csv_scene_store.hpp"
#include "media/infrastructure/part_list.hpp"
#include "shared/io/json.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace ttrally::launcher {

namespace fs = std::filesystem;

namespace {

constexpr std::array<std::string_view, 7> kVideoExtensions{".mp4", ".mov", ".mkv", ".mts",
                                                           ".m4v", ".avi", ".webm"};
constexpr std::string_view kPreCutFolderHint = "liimba";
/// A folder in the library root that holds videos the tool should not list.
constexpr std::string_view kExcludedFolder = "exclude";

std::string lower(std::string text) {
    std::ranges::transform(text, text.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool is_video(const fs::path& path) {
    return std::ranges::find(kVideoExtensions, lower(path.extension().string())) !=
           kVideoExtensions.end();
}

template <typename Filter> std::vector<fs::path> entries(const fs::path& directory, Filter keep) {
    std::vector<fs::path> found;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(directory, error)) {
        if (keep(entry)) {
            found.push_back(entry.path());
        }
    }
    std::ranges::sort(found);
    return found;
}

std::vector<fs::path> files_with(const fs::path& directory, std::string_view extension) {
    return entries(directory, [&](const fs::directory_entry& entry) {
        return entry.is_regular_file() && lower(entry.path().extension().string()) == extension;
    });
}

std::string features_model(const fs::path& manifest) {
    std::ifstream in(manifest, std::ios::binary);
    if (!in) {
        return {};
    }
    std::stringstream text;
    text << in.rdbuf();
    try {
        return io::parse_json(text.str()).at("model").as_string();
    } catch (const io::JsonError&) {
        return "(unreadable manifest)";
    }
}

AnnotationProgress annotation_progress(const fs::path& directory, const std::string& id) {
    if (!fs::exists(directory / (id + ".csv"))) {
        return AnnotationProgress::None;
    }
    const auto statuses = annotation::CsvReviewStateStore(directory).load(id);
    const bool complete =
        !statuses.empty() && std::ranges::none_of(statuses, [](const auto& entry) {
            return entry.second == annotation::ReviewStatus::Open;
        });
    return complete ? AnnotationProgress::Complete : AnnotationProgress::InProgress;
}

/// A video (or a match's part list) of the match day with its id and progress.
VideoEntry describe(const fs::path& video, const fs::path& work, std::string video_id) {
    VideoEntry entry;
    entry.match_day = work.parent_path().filename().string();
    entry.original = video.string();
    entry.video_id = std::move(video_id);
    entry.work_dir = work.string();
    const std::string& id = entry.video_id;
    entry.aligned = fs::exists(work / "align" / (id + ".csv"));
    entry.annotation = annotation_progress(work / "annotations", id);
    if (entry.annotation != AnnotationProgress::None) {
        try {
            entry.rallies =
                annotation::CsvAnnotationRepository(work / "annotations").load(id).rallies.size();
        } catch (const std::exception&) {
            entry.rallies = 0; // shown as labels without rallies; annotate reports the error
        }
    }
    entry.features_model = features_model(work / "features" / id / "manifest.json");
    entry.detected = fs::exists(work / "detections" / (id + ".csv"));
    entry.refinement = annotation_progress(work / "refined", id);
    for (const fs::path& cut : files_with(work / "cuts", ".mp4")) {
        if (cut.stem().string() == id || is_derived_from(cut.stem().string(), id)) {
            entry.cuts.push_back(cut.string());
        }
    }
    return entry;
}

/// Videos in the match day's pre-cut folders (any subfolder whose name contains "liimba").
std::vector<fs::path> pre_cut_videos(const fs::path& match_day) {
    std::vector<fs::path> videos;
    for (const fs::path& folder : entries(match_day, [](const fs::directory_entry& entry) {
             return entry.is_directory() &&
                    lower(entry.path().filename().string()).find(kPreCutFolderHint) !=
                        std::string::npos;
         })) {
        for (const fs::path& video : entries(folder, [](const fs::directory_entry& entry) {
                 return entry.is_regular_file() && is_video(entry.path());
             })) {
            videos.push_back(video);
        }
    }
    return videos;
}

/// The pre-cut versions of a video: the one chosen for it first, then those named after it.
std::vector<std::string> pre_cuts_of(const fs::path& video, const std::string& id,
                                     const std::map<std::string, fs::path>& chosen,
                                     const std::vector<fs::path>& pre_cuts) {
    std::vector<std::string> found;
    if (const auto it = chosen.find(id); it != chosen.end() && fs::is_regular_file(it->second)) {
        found.push_back(it->second.string()); // first: the default
    }
    for (const fs::path& pre_cut : pre_cuts) {
        if (is_pre_cut_of(pre_cut.stem().string(), video.stem().string()) &&
            std::ranges::find(found, pre_cut.string()) == found.end()) {
            found.push_back(pre_cut.string());
        }
    }
    return found;
}

/// A match of several files as one video: its part list, and the list of its parts' pre-cuts
/// (each part's first, in order) if any part has one. Nullopt if a list cannot be written.
std::optional<VideoEntry> describe_match(const Match& match, const fs::path& match_day,
                                         const std::map<std::string, fs::path>& chosen,
                                         const std::vector<fs::path>& pre_cuts) {
    const fs::path work = match_day / kWorkFolder;
    const fs::path lists = work / kMatchesFolder;
    std::vector<fs::path> parts;
    std::vector<fs::path> part_pre_cuts;
    for (const std::string& name : match.parts) {
        const fs::path part = match_day / name;
        parts.push_back(part);
        const auto found =
            pre_cuts_of(part, suggest_video_id(part.stem().string()), chosen, pre_cuts);
        if (!found.empty()) {
            part_pre_cuts.emplace_back(found.front());
        }
    }
    const fs::path list = lists / part_list_name(match.id);
    const fs::path pre_cut_list = lists / pre_cut_list_name(match.id);
    try {
        media::write_part_list(list, parts);
        if (!part_pre_cuts.empty()) {
            media::write_part_list(pre_cut_list, part_pre_cuts);
        }
    } catch (const std::exception&) {
        return std::nullopt; // a missing or unreadable part: not offered
    }
    VideoEntry entry = describe(list, work, match.id);
    for (const fs::path& part : parts) {
        entry.parts.push_back(part.string());
    }
    for (const media::PartSpan& part : media::part_spans(list)) {
        entry.part_starts_s.push_back(part.start_s);
    }
    if (const auto it = chosen.find(match.id);
        it != chosen.end() && fs::is_regular_file(it->second)) {
        entry.cut_candidates.push_back(it->second.string());
    }
    if (!part_pre_cuts.empty() &&
        std::ranges::find(entry.cut_candidates, pre_cut_list.string()) ==
            entry.cut_candidates.end()) {
        entry.cut_candidates.push_back(pre_cut_list.string());
    }
    return entry;
}

template <typename Load> auto load_or_empty(Load load) -> decltype(load()) {
    try {
        return load();
    } catch (const std::exception&) {
        return {}; // an unreadable file only loses what it held
    }
}

void scan_match_day(const fs::path& match_day, WorkspaceSnapshot& workspace) {
    const fs::path work = match_day / kWorkFolder;
    const std::vector<fs::path> pre_cuts = pre_cut_videos(match_day);
    const auto scenes = load_or_empty([&] { return CsvSceneStore().load(work); });
    const auto chosen = load_or_empty([&] { return CsvPreCutStore().load(work); });
    const auto matches = load_or_empty([&] { return CsvMatchStore().load(work); });
    std::vector<VideoEntry> day;
    for (const fs::path& video : entries(match_day, [](const fs::directory_entry& entry) {
             return entry.is_regular_file() && is_video(entry.path());
         })) {
        const std::string name = video.filename().string();
        if (std::ranges::any_of(matches, [&](const Match& match) {
                return std::ranges::find(match.parts, name) != match.parts.end();
            })) {
            continue; // a part of a match: listed as the match
        }
        VideoEntry entry = describe(video, work, suggest_video_id(video.stem().string()));
        entry.cut_candidates = pre_cuts_of(video, entry.video_id, chosen, pre_cuts);
        day.push_back(std::move(entry));
    }
    for (const Match& match : matches) {
        if (auto entry = describe_match(match, match_day, chosen, pre_cuts)) {
            day.push_back(std::move(*entry));
        }
    }
    std::ranges::stable_sort(day, {}, &VideoEntry::video_id);
    for (VideoEntry& entry : day) {
        if (const auto scene = scenes.find(entry.video_id); scene != scenes.end()) {
            entry.scene = scene->second;
        }
        if (entry.annotation == AnnotationProgress::Complete && !entry.features_model.empty()) {
            workspace.trainable.push_back(entry.key());
        }
        workspace.videos.push_back(std::move(entry));
    }
}

} // namespace

WorkspaceSnapshot LibraryScanner::scan(const fs::path& library) {
    WorkspaceSnapshot workspace;
    if (library.empty()) {
        return workspace; // none chosen yet
    }
    workspace.library = fs::absolute(library).lexically_normal().string();
    while (workspace.library.size() > 1 &&
           (workspace.library.back() == '/' || workspace.library.back() == '\\')) {
        workspace.library.pop_back();
    }
    for (const fs::path& match_day : entries(library, [](const fs::directory_entry& entry) {
             const std::string name = entry.path().filename().string();
             return entry.is_directory() && name != kWorkFolder && !name.starts_with(".") &&
                    lower(name) != kExcludedFolder;
         })) {
        scan_match_day(fs::absolute(match_day), workspace);
    }
    for (const fs::path& model : files_with(fs::path(workspace.shared_dir()) / "models", ".onnx")) {
        auto& list = model.filename().string().find("detector") != std::string::npos
                         ? workspace.detectors
                         : workspace.backbones;
        list.push_back(model.string());
    }
    for (const fs::path& detector : files_with(project_ / "weights", ".onnx")) {
        workspace.detectors.push_back(fs::absolute(detector).lexically_normal().string());
    }
    // The newest first: the form preselects it (by name, "rally-detector-test.onnx" would come
    // before "rally-detector.onnx")
    std::ranges::stable_sort(workspace.detectors, std::greater{}, [](const std::string& path) {
        std::error_code error;
        return fs::last_write_time(path, error);
    });
    for (const char* uv : {"external/uv/uv", "external/uv/uv.exe"}) {
        if (fs::exists(project_ / uv)) {
            workspace.uv = fs::absolute(project_ / uv).lexically_normal().string();
        }
    }
    return workspace;
}

std::optional<fs::path> FileLauncherSettings::line(std::size_t index) const {
    std::ifstream in(file_);
    std::string text;
    for (std::size_t i = 0; i <= index; ++i) {
        if (!std::getline(in, text)) {
            return std::nullopt;
        }
    }
    return text.empty() ? std::nullopt : std::optional<fs::path>(text);
}

void FileLauncherSettings::write(const std::vector<std::string>& lines) const {
    std::error_code error;
    fs::create_directories(file_.parent_path(), error);
    std::ofstream out(file_);
    for (const std::string& text : lines) {
        out << text << '\n';
    }
}

std::optional<fs::path> FileLauncherSettings::library() { return line(0); }

void FileLauncherSettings::save_library(const fs::path& library) {
    write({fs::absolute(library).string()});
}

std::optional<fs::path> FileLauncherSettings::last_video() { return line(1); }

void FileLauncherSettings::save_last_video(const fs::path& original) {
    write({library().value_or(fs::path()).string(), fs::absolute(original).string()});
}

fs::path default_settings_file() {
#ifdef _WIN32
    const char* base = std::getenv("APPDATA");
    const fs::path directory = base != nullptr ? fs::path(base) : fs::temp_directory_path();
#else
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    const fs::path directory = xdg != nullptr && *xdg != '\0' ? fs::path(xdg)
                               : home != nullptr          ? fs::path(home) / ".config"
                                                          : fs::temp_directory_path();
#endif
    return directory / "ttrally" / "launcher-library.txt";
}

} // namespace ttrally::launcher
