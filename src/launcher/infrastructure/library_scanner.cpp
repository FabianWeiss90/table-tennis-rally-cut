// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "launcher/infrastructure/library_scanner.hpp"

#include "annotation/infrastructure/csv_annotation_repository.hpp"
#include "annotation/infrastructure/csv_review_state_store.hpp"
#include "launcher/domain/video_id.hpp"
#include "launcher/infrastructure/csv_scene_store.hpp"
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

VideoEntry describe(const fs::path& video, const fs::path& work) {
    VideoEntry entry;
    entry.match_day = video.parent_path().filename().string();
    entry.original = video.string();
    entry.video_id = suggest_video_id(video.stem().string());
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

void scan_match_day(const fs::path& match_day, WorkspaceSnapshot& workspace) {
    const fs::path work = match_day / kWorkFolder;
    const std::vector<fs::path> pre_cuts = pre_cut_videos(match_day);
    std::map<std::string, std::string> scenes;
    try {
        scenes = CsvSceneStore().load(work);
    } catch (const std::exception&) {
        scenes.clear(); // an unreadable file only loses the scenes
    }
    for (const fs::path& video : entries(match_day, [](const fs::directory_entry& entry) {
             return entry.is_regular_file() && is_video(entry.path());
         })) {
        VideoEntry entry = describe(video, work);
        if (const auto scene = scenes.find(entry.video_id); scene != scenes.end()) {
            entry.scene = scene->second;
        }
        for (const fs::path& pre_cut : pre_cuts) {
            if (is_pre_cut_of(pre_cut.stem().string(), video.stem().string())) {
                entry.cut_candidates.push_back(pre_cut.string());
            }
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
    for (const char* uv : {"external/uv/uv", "external/uv/uv.exe"}) {
        if (fs::exists(project_ / uv)) {
            workspace.uv = fs::absolute(project_ / uv).lexically_normal().string();
        }
    }
    return workspace;
}

std::optional<fs::path> FileLauncherSettings::library() {
    std::ifstream in(file_);
    std::string line;
    if (!in || !std::getline(in, line) || line.empty()) {
        return std::nullopt;
    }
    return fs::path(line);
}

void FileLauncherSettings::save_library(const fs::path& library) {
    std::error_code error;
    fs::create_directories(file_.parent_path(), error);
    std::ofstream(file_) << fs::absolute(library).string() << '\n';
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
