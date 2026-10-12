// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "launcher/infrastructure/filesystem_workspace_scanner.hpp"

#include "annotation/infrastructure/csv_annotation_repository.hpp"
#include "annotation/infrastructure/csv_review_state_store.hpp"
#include "launcher/domain/video_id.hpp"
#include "shared/io/json.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <sstream>

namespace ttrally::launcher {

namespace fs = std::filesystem;

namespace {

constexpr std::array<std::string_view, 6> kVideoExtensions{".mp4", ".mov", ".mkv",
                                                           ".mts", ".m4v", ".avi"};

std::string lower(std::string text) {
    std::ranges::transform(text, text.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool is_video(const fs::path& path) {
    return std::ranges::find(kVideoExtensions, lower(path.extension().string())) !=
           kVideoExtensions.end();
}

/// Regular files of a directory with one of the extensions, sorted; none if it is missing.
std::vector<fs::path> files_in(const fs::path& directory, std::string_view extension) {
    std::vector<fs::path> files;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(directory, error)) {
        if (entry.is_regular_file() && lower(entry.path().extension().string()) == extension) {
            files.push_back(entry.path());
        }
    }
    std::ranges::sort(files);
    return files;
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

} // namespace

std::string FilesystemWorkspaceScanner::relative(const fs::path& path) const {
    return path.lexically_relative(root_).generic_string();
}

WorkspaceSnapshot FilesystemWorkspaceScanner::scan() {
    WorkspaceSnapshot workspace;
    std::vector<fs::path> videos;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(root_ / "data", error)) {
        if (entry.is_regular_file() && is_video(entry.path())) {
            videos.push_back(entry.path());
        }
    }
    std::ranges::sort(videos);
    std::vector<fs::path> originals;
    for (const fs::path& video : videos) {
        const std::string stem = video.stem().string();
        const bool derived = std::ranges::any_of(videos, [&](const fs::path& other) {
            return other != video && is_derived_from(stem, other.stem().string());
        });
        if (!derived) {
            originals.push_back(video);
        }
    }
    for (const fs::path& original : originals) {
        VideoEntry entry = describe(original);
        for (const fs::path& video : videos) {
            if (is_derived_from(video.stem().string(), original.stem().string())) {
                entry.cut_candidates.push_back(relative(video));
            }
        }
        if (entry.annotation == AnnotationProgress::Complete && !entry.features_model.empty()) {
            workspace.trainable.push_back(entry.video_id);
        }
        workspace.videos.push_back(std::move(entry));
    }
    for (const fs::path& model : files_in(root_ / "data" / "models", ".onnx")) {
        auto& list = model.filename().string().find("detector") != std::string::npos
                         ? workspace.detectors
                         : workspace.backbones;
        list.push_back(relative(model));
    }
    for (const fs::path& detector : files_in(root_ / "weights", ".onnx")) {
        workspace.detectors.insert(workspace.detectors.begin(), relative(detector));
    }
    for (const char* uv : {"external/uv/uv", "external/uv/uv.exe"}) {
        if (fs::exists(root_ / uv)) {
            workspace.uv = fs::absolute(root_ / uv).lexically_normal().string();
        }
    }
    return workspace;
}

VideoEntry FilesystemWorkspaceScanner::describe(const fs::path& video) const {
    VideoEntry entry;
    entry.original = relative(video);
    entry.video_id = suggest_video_id(video.stem().string());
    const std::string& id = entry.video_id;
    const fs::path data = root_ / "data";
    entry.aligned = fs::exists(data / "align" / (id + ".csv"));
    entry.annotation = annotation_progress(data / "annotations", id);
    if (entry.annotation != AnnotationProgress::None) {
        try {
            entry.rallies =
                annotation::CsvAnnotationRepository(data / "annotations").load(id).rallies.size();
        } catch (const std::exception&) {
            entry.rallies = 0; // shown as labels without rallies; annotate reports the error
        }
    }
    entry.features_model = features_model(data / "features" / id / "manifest.json");
    entry.detected = fs::exists(data / "detections" / (id + ".csv"));
    for (const fs::path& cut : files_in(data / "cuts", ".mp4")) {
        if (cut.stem().string() == id || is_derived_from(cut.stem().string(), id)) {
            entry.cuts.push_back(relative(cut));
        }
    }
    return entry;
}

} // namespace ttrally::launcher
