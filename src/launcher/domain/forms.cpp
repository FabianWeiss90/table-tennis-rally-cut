// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "launcher/domain/forms.hpp"

#include <algorithm>
#include <format>

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
    return fp16 != workspace.backbones.end() ? *fp16
                                             : first_or(workspace.backbones,
                                                        "data/models/dinov2-vitb14-fp16.onnx");
}

std::string joined_lines(const std::vector<std::string>& values) {
    std::string text;
    for (const std::string& value : values) {
        text += value + "\n";
    }
    return text;
}

Field cache_dir() {
    return advanced(option("--cache-dir", "Cache directory", FieldKind::Directory, "data/cache",
                           "data/cache", "Frame timestamps and decoded audio"));
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
                             std::format("data/align/{}.csv", id), "",
                             "Gaps are written next to it")),
             option("--report", "HTML report", FieldKind::File,
                    std::format("data/align/{}.html", id), "", "Summary with plot"),
             cache_dir(),
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
    return {"annotate", "Annotate", "Open the annotation window to mark the rallies; it saves "
                                    "every change at once.",
            Program::Ttrally, "", "", {"annotate"},
            {positional("Original video", FieldKind::File, video.original, "Untrimmed original"),
             required(option("--segments", "Segments CSV (from align)", FieldKind::File,
                             std::format("data/align/{}.csv", id), "", "")),
             option("--video-id", "Video id", FieldKind::Text, id, "",
                    "Name of the label file"),
             advanced(option("--gaps", "Gaps CSV", FieldKind::File, "", "",
                             "Default: <segments>.gaps.csv")),
             advanced(option("--annotations-dir", "Labels directory", FieldKind::Directory,
                             "data/annotations", "data/annotations", "")),
             cache_dir(),
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
                             "data/features", "data/features", "")),
             cache_dir(),
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
                             "data/features", "data/features", "")),
             cache_dir(),
             advanced(option("--out-dir", "Output directory", FieldKind::Directory,
                             "data/detections", "data/detections", "")),
             advanced(option("--labels-dir", "Labels directory", FieldKind::Directory,
                             "data/annotations", "data/annotations",
                             "Compared with the detections if present")),
             advanced(flag("--no-evaluation", "Do not compare with labels", "")),
             advanced(choice("--ep", "Image model runs on", kExecutionProviders, "auto", "")),
             decode_backend(),
             advanced(option("--batch", "Batch size", FieldKind::Text, "16", "16", ""))}};
}

CommandForm cut_form(const VideoEntry& video) {
    const std::string id = video.video_id;
    const std::string detections = std::format("data/detections/{}.csv", id);
    const std::string labels = std::format("data/annotations/{}.csv", id);
    std::vector<std::string> cut_lists;
    if (video.detected) {
        cut_lists.push_back(detections);
    }
    if (video.annotation != AnnotationProgress::None) {
        cut_lists.push_back(labels);
    }
    return {"cut", "Cut", "Cut the video down to its rallies, with a margin before and after "
                          "each, into one video.",
            Program::Ttrally, "", "", {"cut"},
            {positional("Video", FieldKind::File, video.original, "Original video"),
             with_suggestions(required(option("--rallies", "Rallies", FieldKind::File,
                                              first_or(cut_lists, detections), "",
                                              "Detections or annotations (label format)")),
                              cut_lists),
             option("--out", "Output video", FieldKind::File,
                    std::format("data/cuts/{}.mp4", id), "", ""),
             option("--before", "Seconds before each rally", FieldKind::Text, "2", "2", ""),
             option("--after", "Seconds after each rally", FieldKind::Text, "2", "2", ""),
             with_suggestions(option("--bitrate", "Bit rate (Mbit/s)", FieldKind::Text, "", "",
                                     "Empty: 5 with H.265, 10 with H.264 (Full HD, 60 fps)"),
                              {"5", "6", "8", "12"}),
             choice("--codec", "Codec", {"h265", "h264"}, "h265",
                    "H.265: smaller at the same quality; H.264: plays everywhere"),
             flag("--fast", "Fast (no re-encoding)",
                  "Seconds instead of minutes, but cuts snap to keyframes (often several s)"),
             advanced(option("--video-id", "Video id", FieldKind::Text, id, "", "")),
             advanced(option("--encoder", "Encoder", FieldKind::Text, "auto", "auto",
                             "e.g. hevc_vaapi, h264_nvenc, libopenh264")),
             cache_dir()}};
}

CommandForm train_form(const WorkspaceSnapshot& workspace) {
    Field videos = option("--videos", "Videos", FieldKind::MultiChoice,
                          joined_lines(workspace.trainable), "",
                          "Videos with complete labels and features");
    videos.choices = workspace.trainable;
    std::vector<Field> fields{
        videos,
        with_suggestions(option("--groups", "Groups CSV", FieldKind::File, "", "",
                                "video_id,group: held out together in the evaluation"),
                         {"data/annotations/groups.csv"}),
        option("--out", "Detector file", FieldKind::File, "weights/rally-detector.onnx",
               "weights/rally-detector.onnx", "A report is written next to it"),
        option("--epochs", "Epochs", FieldKind::Text, "120", "120", ""),
        option("--runs", "Runs", FieldKind::Text, "1", "1",
               "Cross-validation runs with different seeds, averaged"),
        advanced(flag("--skip-evaluation", "Skip the cross-validation", "")),
        advanced(option("--features-dir", "Features directory", FieldKind::Directory,
                        "data/features", "data/features", "")),
        advanced(option("--annotations-dir", "Labels directory", FieldKind::Directory,
                        "data/annotations", "data/annotations", "")),
        advanced(option("--hidden", "Hidden channels", FieldKind::Text, "128", "128", "")),
        advanced(option("--levels", "Levels", FieldKind::Text, "8", "8", "")),
        advanced(option("--stages", "Stages", FieldKind::Text, "4", "4", "")),
        advanced(option("--seed", "Seed", FieldKind::Text, "42", "42", "")),
        advanced(choice("--device", "Device", {"auto", "cpu", "cuda"}, "auto", ""))};
    for (Field& field : fields) {
        // The training runs in training/; paths are entered relative to the project. Its own
        // defaults (../data/..., ../weights/...) are the same places, so they are left out.
        field.project_path = field.kind == FieldKind::File || field.kind == FieldKind::Directory;
    }
    return {"train", "Train", "Train the rally detector on the selected videos and export it "
                              "(Python, runs with uv in training/; takes a long time).",
            Program::Uv, workspace.uv, "training",
            {"run", "python", "-u", "-m", "ttrally_training.train"}, fields};
}

} // namespace

std::vector<CommandForm> forms_for(const VideoEntry& video, const WorkspaceSnapshot& workspace) {
    return {align_form(video),          annotate_form(video),
            features_form(video, workspace), detect_form(video, workspace),
            cut_form(video),            train_form(workspace)};
}

} // namespace ttrally::launcher
