// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "launcher/domain/forms.hpp"

#include <algorithm>
#include <filesystem>
#include <format>
#include <initializer_list>

namespace ttrally::launcher {

namespace {

const std::vector<std::string> kDecodeBackends{"auto",    "vaapi",  "cuda", "d3d11va",
                                               "d3d12va", "vulkan", "cpu"};
const std::vector<std::string> kExecutionProviders{"auto",    "cuda",   "tensorrt",
                                                   "migraphx", "webgpu", "cpu"};
constexpr std::string_view kPreferredBackbone = "fp16"; ///< the variant the detector is trained on

Field positional(std::string label, FieldKind kind, std::string value, std::string help) {
    return {.flag = {}, .label = std::move(label), .kind = kind, .value = std::move(value),
            .default_value = {}, .suggestions = {}, .choices = {}, .required = true,
            .advanced = false, .project_path = false, .help = std::move(help)};
}

Field option(std::string flag, std::string label, FieldKind kind, std::string value,
             std::string default_value, std::string help) {
    return {.flag = std::move(flag), .label = std::move(label), .kind = kind,
            .value = std::move(value), .default_value = std::move(default_value),
            .suggestions = {}, .choices = {}, .required = false, .advanced = false,
            .project_path = false, .help = std::move(help)};
}

Field advanced(Field field) {
    field.advanced = true;
    return field;
}

Field required(Field field) {
    field.required = true;
    return field;
}

Field with_suggestions(Field field, std::vector<std::string> suggestions) {
    field.suggestions = std::move(suggestions);
    return field;
}

Field choice(std::string flag, std::string label, std::vector<std::string> choices,
             std::string default_value, std::string help) {
    Field field = option(std::move(flag), std::move(label), FieldKind::Choice, default_value,
                         default_value, std::move(help));
    field.choices = std::move(choices);
    return field;
}

Field flag(std::string name, std::string label, std::string help) {
    return option(std::move(name), std::move(label), FieldKind::Flag, "", "", std::move(help));
}

std::string first_or(const std::vector<std::string>& values, std::string fallback) {
    return values.empty() ? std::move(fallback) : values.front();
}

std::string preferred_backbone(const WorkspaceSnapshot& workspace) {
    const auto fp16 = std::ranges::find_if(workspace.backbones, [](const std::string& path) {
        return path.find(kPreferredBackbone) != std::string::npos;
    });
    return fp16 != workspace.backbones.end()
               ? *fp16
               : first_or(workspace.backbones,
                          (std::filesystem::path(workspace.shared_dir()) / "models" /
                           "dinov2-vitb14-fp16.onnx")
                              .string());
}

/// A file or folder in a video's work folder (the match day's ttrally-data).
std::string in_work_dir(const VideoEntry& video, std::initializer_list<std::string> parts) {
    std::filesystem::path path(video.work_dir);
    for (const std::string& part : parts) {
        path /= part;
    }
    return path.string();
}

Field cache_dir(const VideoEntry& video) {
    return advanced(option("--cache-dir", "Cache directory", FieldKind::Directory,
                           in_work_dir(video, {"cache"}), "data/cache",
                           "Frame timestamps and decoded audio"));
}

Field decode_backend() {
    return advanced(choice("--decode-backend", "Video decoding", kDecodeBackends, "auto",
                           "Hardware decoder; auto tries the GPU first"));
}

CommandForm align_form(const VideoEntry& video) {
    const std::string id = video.video_id;
    return {"align", "Align", "Find the segments of a pre-cut video (e.g. from Liimba) in the "
                             "original via the audio; the result is the list of candidates for "
                             "annotate.",
            Program::Ttrally, "", "", {"align"},
            {positional("Original video", FieldKind::File, video.original, "Untrimmed original"),
             with_suggestions(positional("Pre-cut video", FieldKind::File,
                                         first_or(video.cut_candidates, ""),
                                         "Cut video made from the original"),
                              video.cut_candidates),
             required(option("--out", "Segments CSV", FieldKind::File,
                             in_work_dir(video, {"align", id + ".csv"}), "",
                             "Gaps are written next to it")),
             option("--report", "HTML report", FieldKind::File,
                    in_work_dir(video, {"align", id + ".html"}), "", "Summary with plot"),
             cache_dir(video),
             advanced(flag("--no-cache", "Do not use the cache", "")),
             decode_backend(),
             advanced(flag("--no-visual-check", "Skip the visual check",
                           "Do not compare one frame per segment")),
             advanced(option("--min-confidence", "Minimum confidence", FieldKind::Text, "2", "2",
                             "Peak ratio below which a window counts as uncertain")),
             advanced(option("--local-search", "Local search (s)", FieldKind::Text, "120",
                             "120", "Seconds searched after the previous match")),
             advanced(option("--threads", "Threads", FieldKind::Text, "0", "0",
                             "0 = all cores"))}};
}

CommandForm annotate_form(const VideoEntry& video) {
    const std::string id = video.video_id;
    const bool pre_cut = !video.cut_candidates.empty();
    return {"annotate", "Annotate",
            pre_cut ? "Open the annotation window to mark the rallies in the segments found by "
                      "align and check the gaps between them; it saves every change at once."
                    : "Open the annotation window to mark every rally by hand: without a pre-cut "
                      "video the whole video is one gap, checked with R at the end. It saves "
                      "every change at once.",
            Program::Ttrally, "", "", {"annotate"},
            {positional("Original video", FieldKind::File, video.original, "Untrimmed original"),
             option("--segments", "Segments CSV (from align)", FieldKind::File,
                    pre_cut ? in_work_dir(video, {"align", id + ".csv"}) : "", "",
                    "Empty: mark every rally by hand in the whole video"),
             required(option("--video-id", "Video id", FieldKind::Text, id, "",
                             "Name of the label file")),
             advanced(option("--gaps", "Gaps CSV", FieldKind::File, "", "",
                             "Default: <segments>.gaps.csv")),
             advanced(option("--annotations-dir", "Labels directory", FieldKind::Directory,
                             in_work_dir(video, {"annotations"}), "data/annotations", "")),
             cache_dir(video),
             decode_backend(),
             advanced(option("--display-height", "Display height", FieldKind::Text, "1080",
                             "1080", "Pixels of the shown video")),
             advanced(option("--frame-memory", "Frame memory (MB)", FieldKind::Text, "1024",
                             "1024", "More allows longer steps back without decoding"))}};
}

CommandForm features_form(const VideoEntry& video, const WorkspaceSnapshot& workspace) {
    return {"features", "Features", "Compute the image features the detector is trained on "
                                    "and runs on (10 per second).",
            Program::Ttrally, "", "", {"features"},
            {positional("Video", FieldKind::File, video.original, "Original video"),
             option("--video-id", "Video id", FieldKind::Text, video.video_id, "",
                    "Name of the output directory"),
             with_suggestions(option("--model", "Image model", FieldKind::File,
                                     preferred_backbone(workspace),
                                     "data/models/dinov2-vitb14.onnx",
                                     "Use the variant the detector is trained with (fp16)"),
                              workspace.backbones),
             advanced(option("--out-dir", "Output directory", FieldKind::Directory,
                             in_work_dir(video, {"features"}), "data/features", "")),
             cache_dir(video),
             advanced(choice("--ep", "Model runs on", kExecutionProviders, "auto",
                             "auto picks the GPU if possible")),
             decode_backend(),
             advanced(option("--rate", "Samples per second", FieldKind::Text, "10", "10", "")),
             advanced(option("--batch", "Batch size", FieldKind::Text, "16", "16",
                             "Images per model run")),
             advanced(flag("--force", "Recompute", "Even if the stored features are current"))}};
}

CommandForm detect_form(const VideoEntry& video, const WorkspaceSnapshot& workspace) {
    return {"detect", "Detect", "Find the rallies with a trained detector; compares them with "
                                "the labels if the video has any.",
            Program::Ttrally, "", "", {"detect"},
            {positional("Video", FieldKind::File, video.original, "Original video"),
             option("--video-id", "Video id", FieldKind::Text, video.video_id, "", ""),
             with_suggestions(option("--detector", "Detector", FieldKind::File,
                                     first_or(workspace.detectors, "weights/rally-detector.onnx"),
                                     "weights/rally-detector.onnx",
                                     "Exported by the training"),
                              workspace.detectors),
             with_suggestions(advanced(option("--backbone", "Image model", FieldKind::File,
                                              preferred_backbone(workspace),
                                              "data/models/dinov2-vitb14-fp16.onnx",
                                              "For missing features; the detector's model")),
                              workspace.backbones),
             advanced(option("--features-dir", "Features directory", FieldKind::Directory,
                             in_work_dir(video, {"features"}), "data/features", "")),
             cache_dir(video),
             advanced(option("--out-dir", "Output directory", FieldKind::Directory,
                             in_work_dir(video, {"detections"}), "data/detections", "")),
             advanced(option("--labels-dir", "Labels directory", FieldKind::Directory,
                             in_work_dir(video, {"annotations"}), "data/annotations",
                             "Compared with the detections if present")),
             advanced(flag("--no-evaluation", "Do not compare with labels", "")),
             advanced(choice("--ep", "Image model runs on", kExecutionProviders, "auto", "")),
             decode_backend(),
             advanced(option("--batch", "Batch size", FieldKind::Text, "16", "16", ""))}};
}

CommandForm cut_form(const VideoEntry& video) {
    const std::string id = video.video_id;
    const std::string detections = in_work_dir(video, {"detections", id + ".csv"});
    const std::string labels = in_work_dir(video, {"annotations", id + ".csv"});
    // Complete labels are exact, so they come first; then the detections; labels still in
    // progress would leave out the rallies not marked yet
    std::vector<std::string> cut_lists;
    if (video.annotation == AnnotationProgress::Complete) {
        cut_lists.push_back(labels);
    }
    if (video.detected) {
        cut_lists.push_back(detections);
    }
    if (video.annotation == AnnotationProgress::InProgress) {
        cut_lists.push_back(labels);
    }
    return {"cut", "Cut", "Cut the video down to its rallies, with a margin before and after "
                          "each, into one video.",
            Program::Ttrally, "", "", {"cut"},
            {positional("Video", FieldKind::File, video.original, "Original video"),
             with_suggestions(required(option("--rallies", "Rallies", FieldKind::File,
                                              first_or(cut_lists, detections), "",
                                              "Default: the annotations if complete, else the "
                                              "detections")),
                              cut_lists),
             option("--out", "Output video", FieldKind::File,
                    in_work_dir(video, {"cuts", id + ".mp4"}), "", ""),
             option("--before", "Seconds before each rally", FieldKind::Text, "1", "1", ""),
             option("--after", "Seconds after each rally", FieldKind::Text, "1", "1", ""),
             with_suggestions(option("--bitrate", "Bit rate (Mbit/s)", FieldKind::Text, "", "",
                                     "From 1; empty: 4 with H.265, 8 with H.264 (Full HD, "
                                     "60 fps). Size per minute: about 7.5 MB per Mbit/s"),
                              {"1", "2", "3", "4", "5", "6", "8", "12"}),
             choice("--codec", "Codec", {"h265", "h264"}, "h265",
                    "H.265: smaller at the same quality; H.264: plays everywhere"),
             flag("--fast", "Fast (no re-encoding)",
                  "Seconds instead of minutes, but cuts snap to keyframes (often several s)"),
             advanced(option("--video-id", "Video id", FieldKind::Text, id, "", "")),
             advanced(option("--encoder", "Encoder", FieldKind::Text, "auto", "auto",
                             "e.g. hevc_vaapi, h264_nvenc, libopenh264")),
             cache_dir(video)}};
}

CommandForm train_form(const WorkspaceSnapshot& workspace) {
    const std::filesystem::path shared(workspace.shared_dir());
    Field videos = option("--videos", "Training videos", FieldKind::MultiChoice, "", "",
                          "Videos with complete labels and features, by match day; the ones "
                          "not ticked validate the model");
    videos.choices = workspace.trainable;
    videos.required = true;
    return {"train", "Train", "Train the rally detector on the ticked videos and export it "
                              "(Python, runs with uv in training/; takes a long time). All other "
                              "videos listed here validate it: the decoding is chosen on them and "
                              "the result is measured on them.",
            Program::Uv, workspace.uv, "training",
            {"run", "python", "-u", "-m", "ttrally_training.train"},
            {videos,
             required(option("--library", "Library", FieldKind::Directory, workspace.library, "",
                             "Folder with the match day folders")),
             option("--out", "Detector file", FieldKind::File,
                    (shared / "models" / "rally-detector.onnx").string(), "",
                    "A report is written next to it"),
             option("--epochs", "Epochs", FieldKind::Text, "120", "120", ""),
             advanced(option("--hidden", "Hidden channels", FieldKind::Text, "128", "128", "")),
             advanced(option("--levels", "Levels", FieldKind::Text, "8", "8", "")),
             advanced(option("--stages", "Stages", FieldKind::Text, "4", "4", "")),
             advanced(option("--seed", "Seed", FieldKind::Text, "42", "42", "")),
             advanced(choice("--device", "Device", {"auto", "cpu", "cuda"}, "auto", ""))}};
}

} // namespace

std::vector<CommandForm> forms_for(const VideoEntry& video, const WorkspaceSnapshot& workspace) {
    return {align_form(video),          annotate_form(video),
            features_form(video, workspace), detect_form(video, workspace),
            cut_form(video),            train_form(workspace)};
}

} // namespace ttrally::launcher
