// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "launcher/domain/forms.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <string_view>

namespace ttrally::launcher {

namespace {

const std::vector<std::string> kDecodeBackends{"auto",    "vaapi",  "cuda", "d3d11va",
                                               "d3d12va", "vulkan", "cpu"};
const std::vector<std::string> kExecutionProviders{"auto",    "cuda",   "tensorrt",
                                                   "migraphx", "webgpu", "cpu"};
constexpr std::string_view kPreferredBackbone = "fp16"; ///< the variant the detector is trained on
/// Bit rates proposed per resolution with H.265 at 60 fps: what the encoder would pick (H.264
/// takes twice as much). The original's size is not known here: empty, the encoder chooses.
struct ResolutionBitrate {
    std::string_view resolution;
    int h265_mbps;
};
constexpr std::array<ResolutionBitrate, 3> kResolutionBitrates{
    {{"1080p", 4}, {"1440p", 7}, {"2160p", 16}}};
constexpr int kH264Factor = 2;
constexpr const char* kOriginalResolution = "original";
const std::vector<std::string> kResolutions{kOriginalResolution, "2160p", "1440p", "1080p"};
constexpr const char* kBitrateHelp =
    "Mbit/s, from 1. Empty (auto): chosen from the resolution and frame rate. Proposed for 60 "
    "fps with H.265: 4 for 1080p, 7 for 1440p, 16 for 2160p (4K); H.264 twice that. Follows the "
    "codec and resolution until you enter your own";


Field positional(std::string label, FieldKind kind, std::string value, std::string help) {
    return {.flag = {}, .label = std::move(label), .kind = kind, .value = value,
            .default_value = {}, .suggestions = {}, .choices = {}, .required = true,
            .advanced = false, .project_path = false, .help = std::move(help), .choice_help = {},
            .initial_value = value};
}

Field option(std::string flag, std::string label, FieldKind kind, std::string value,
             std::string default_value, std::string help) {
    return {.flag = std::move(flag), .label = std::move(label), .kind = kind, .value = value,
            .default_value = std::move(default_value), .suggestions = {}, .choices = {},
            .required = false, .advanced = false, .project_path = false,
            .help = std::move(help), .choice_help = {}, .initial_value = value};
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

/// The output size: the original's or smaller (never enlarged).
Field resolution_field() {
    return choice("--resolution", "Resolution", kResolutions, kOriginalResolution,
                  "Height of the written video: 2160p is 4K (3840x2160), 1440p is 2560x1440, "
                  "1080p is Full HD. A smaller original is not enlarged");
}

/// The match day's white balance, applied when cutting.
Field white_balance_field(const VideoEntry& video) {
    return advanced(option("--white-balance", "White balance", FieldKind::File,
                           (std::filesystem::path(video.work_dir) / "white-balance.csv").string(),
                           "",
                           "Set in the White balance tab of the whole match day; empty: the "
                           "colours as recorded. Not applied by a fast cut"));
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

/// Video encoders to choose from (in the order "auto" tries them, see the cutting
/// infrastructure), each with what it runs on and what it needs.
Field encoder_field() {
    struct Encoder {
        std::string name;
        std::string runs_on;
        std::string needs;
    };
    const std::string ignores_codec = " Ignores the codec choice.";
    const std::string nvenc = "Needs an NVIDIA graphics card and its driver.";
    const std::string qsv = "Needs Intel graphics with the Intel media driver and the oneVPL "
                            "runtime.";
    const Encoder openh264{"libopenh264",
                           "H.264 on the CPU (Cisco OpenH264), about 3x real time on a modern "
                           "CPU; a simple H.264 profile, so lower quality at the same bit rate.",
                           "Needs no graphics card, only FFmpeg built with OpenH264."};
    const Encoder mpeg4{"mpeg4",
                        "MPEG-4 Part 2 on the CPU. Last resort: clearly lower quality at the "
                        "same bit rate.",
                        "Built into every FFmpeg."};
#ifdef _WIN32
    const std::string amf = "Needs an AMD graphics card and its driver.";
    const std::string mf = "Uses whichever hardware encoder Windows offers.";
    const std::vector<Encoder> encoders{
        {"auto",
         "The first that works: for H.265 hevc_amf, hevc_nvenc, hevc_qsv, hevc_mf; for H.264 "
         "h264_amf, h264_nvenc, h264_qsv, h264_mf, then libopenh264 and mpeg4 on the CPU.",
         ""},
        {"hevc_amf", "H.265 on the graphics card (AMD AMF)." + ignores_codec, amf},
        {"hevc_nvenc", "H.265 on the graphics card (NVIDIA NVENC)." + ignores_codec, nvenc},
        {"hevc_qsv", "H.265 on the graphics card (Intel Quick Sync)." + ignores_codec, qsv},
        {"hevc_mf", "H.265 via Windows Media Foundation." + ignores_codec, mf},
        {"h264_amf", "H.264 on the graphics card (AMD AMF)." + ignores_codec, amf},
        {"h264_nvenc", "H.264 on the graphics card (NVIDIA NVENC)." + ignores_codec, nvenc},
        {"h264_qsv", "H.264 on the graphics card (Intel Quick Sync)." + ignores_codec, qsv},
        {"h264_mf", "H.264 via Windows Media Foundation." + ignores_codec, mf},
        openh264,
        mpeg4};
#else
    const std::string vaapi = "Needs a VA-API driver that can encode (Fedora: "
                              "mesa-va-drivers-freeworld from RPM Fusion).";
    const std::vector<Encoder> encoders{
        {"auto",
         "The first that works: for H.265 hevc_vaapi, hevc_nvenc, hevc_qsv on the graphics "
         "card; for H.264 h264_vaapi, h264_nvenc, h264_qsv, then libopenh264 and mpeg4 on "
         "the CPU.",
         ""},
        {"hevc_vaapi", "H.265 on the graphics card via VA-API (AMD or Intel)." + ignores_codec,
         vaapi},
        {"hevc_nvenc", "H.265 on the graphics card (NVIDIA NVENC)." + ignores_codec, nvenc},
        {"hevc_qsv", "H.265 on the graphics card (Intel Quick Sync)." + ignores_codec, qsv},
        {"h264_vaapi",
         "H.264 on the graphics card via VA-API (AMD or Intel), always at constant bit rate "
         "(the AMD driver misreads the default variable rate)." + ignores_codec,
         vaapi},
        {"h264_nvenc", "H.264 on the graphics card (NVIDIA NVENC)." + ignores_codec, nvenc},
        {"h264_qsv", "H.264 on the graphics card (Intel Quick Sync)." + ignores_codec, qsv},
        openh264,
        mpeg4};
#endif
    Field field = choice("--encoder", "Encoder", {}, "auto",
                         "Which encoder makes the video; hover an entry for details");
    for (const Encoder& encoder : encoders) {
        field.choices.push_back(encoder.name);
        field.choice_help.push_back(encoder.needs.empty() ? encoder.runs_on
                                                          : encoder.runs_on + " " + encoder.needs);
    }
    return advanced(std::move(field));
}

Field cache_dir(const VideoEntry& video) {
    return advanced(option("--cache-dir", "Cache directory", FieldKind::Directory,
                           in_work_dir(video, {"cache"}), "data/cache",
                           "Frame timestamps and decoded audio"));
}

Field decode_backend(std::string default_value = "auto",
                     std::string help = "Hardware decoder; auto tries the GPU first") {
    return advanced(choice("--decode-backend", "Video decoding", kDecodeBackends,
                           std::move(default_value), std::move(help)));
}

CommandForm align_form(const VideoEntry& video) {
    const std::string id = video.video_id;
    return {"align", "Align", "Find the segments of a pre-cut video (e.g. from Liimba) in the "
                             "original via the audio; the result is the list of candidates for "
                             "annotate. A pre-cut video chosen here is kept for the video.",
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
             advanced(option("--min-confidence", "Minimum confidence", FieldKind::Number, "2", "2",
                             "Peak ratio below which a window counts as uncertain")),
             advanced(option("--local-search", "Local search (s)", FieldKind::Number, "120",
                             "120", "Seconds searched after the previous match")),
             advanced(option("--threads", "Threads", FieldKind::Number, "0", "0",
                             "0 = all cores"))}};
}

CommandForm annotate_form(const VideoEntry& video) {
    const std::string id = video.video_id;
    const bool pre_cut = !video.cut_candidates.empty() || video.aligned;
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
             decode_backend("cpu", "cpu is fastest for stepping (every frame is needed in "
                                   "main memory); auto tries the GPU first"),
             advanced(option("--display-height", "Display height", FieldKind::Number, "1080",
                             "1080", "Pixels of the shown video")),
             advanced(option("--frame-memory", "Frame memory (MB)", FieldKind::Number, "3072",
                             "3072", "About 16 s of Full HD frames: a gap of up to 10 s "
                                     "as a whole plus buffers"))}};
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
             advanced(option("--rate", "Samples per second", FieldKind::Number, "10", "10", "")),
             advanced(option("--batch", "Batch size", FieldKind::Number, "16", "16",
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
             advanced(option("--batch", "Batch size", FieldKind::Number, "16", "16", ""))}};
}

CommandForm refine_form(const VideoEntry& video) {
    const std::string id = video.video_id;
    return {"refine", "Refine",
            "Correct the rallies found by detect in the annotation window: drag their edges on "
            "the timeline (onto a neighbour to merge two halves), add missed rallies and check "
            "the gaps between them. The result is kept apart from the labels for training; cut "
            "uses it instead of the detections.",
            Program::Ttrally, "", "", {"annotate"},
            {positional("Original video", FieldKind::File, video.original, "Untrimmed original"),
             required(option("--detections", "Detections", FieldKind::File,
                             in_work_dir(video, {"detections", id + ".csv"}), "",
                             "Written by detect; loaded until refined rallies are saved")),
             required(option("--video-id", "Video id", FieldKind::Text, id, "", "")),
             advanced(option("--annotations-dir", "Refined rallies directory",
                             FieldKind::Directory, in_work_dir(video, {"refined"}),
                             "data/annotations", "Not the labels for training")),
             cache_dir(video),
             decode_backend("cpu", "cpu is fastest for stepping (every frame is needed in "
                                   "main memory); auto tries the GPU first"),
             advanced(option("--display-height", "Display height", FieldKind::Number, "1080",
                             "1080", "Pixels of the shown video")),
             advanced(option("--frame-memory", "Frame memory (MB)", FieldKind::Number, "3072",
                             "3072", ""))}};
}

CommandForm cut_form(const VideoEntry& video) {
    const std::string id = video.video_id;
    const std::string detections = in_work_dir(video, {"detections", id + ".csv"});
    const std::string labels = in_work_dir(video, {"annotations", id + ".csv"});
    // Complete labels are exact, so they come first; then the refined detections, the
    // detections; labels still in progress would leave out the rallies not marked yet
    std::vector<std::string> cut_lists;
    if (video.annotation == AnnotationProgress::Complete) {
        cut_lists.push_back(labels);
    }
    if (video.refinement != AnnotationProgress::None) {
        cut_lists.push_back(in_work_dir(video, {"refined", id + ".csv"}));
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
                                              "refined detections, else the detections")),
                              cut_lists),
             advanced(option("--out-dir", "Output folder", FieldKind::Directory,
                             in_work_dir(video, {"cuts"}), "data/cuts",
                             "The video is named <video id>-<height>p.mp4, e.g. -1080p")),
             advanced(option("--out", "Output video", FieldKind::File, "", "",
                             "Empty: <video id>-<height>p.mp4 in the output folder; a name "
                             "given here is used as it is")),
             option("--before", "Seconds before each rally", FieldKind::Number, "1", "1", ""),
             option("--after", "Seconds after each rally", FieldKind::Number, "1.5", "1.5", ""),
             resolution_field(),
             with_suggestions(option("--bitrate", "Bit rate (Mbit/s)", FieldKind::Number, "", "",
                                     std::string(kBitrateHelp) +
                                         ". Size per minute: about 7.5 MB per Mbit/s"),
                              {"1", "2", "3", "4", "5", "8", "12", "16", "24"}),
             choice("--codec", "Codec", {"h265", "h264"}, "h265",
                    "H.265: smaller at the same quality; H.264: plays everywhere"),
             white_balance_field(video),
             flag("--fast", "Fast (no re-encoding)",
                  "Seconds instead of minutes, but cuts snap to keyframes (often several s); "
                  "keeps the original resolution"),
             advanced(option("--video-id", "Video id", FieldKind::Text, id, "", "")),
             encoder_field(),
             cache_dir(video)}};
}

CommandForm highlights_form(const VideoEntry& video, const WorkspaceSnapshot& workspace) {
    return {"highlights", "Highlights",
            "Choose the highlights of each game: the longest rallies are preselected, tick or "
            "untick them after watching. A match day highlight is also a game highlight (unless "
            "uncoupled for the game). Then make the video of the game or of the whole match "
            "day.",
            Program::Ttrally, "", "", {"highlights"},
            {advanced(option("--video-id", "Video id", FieldKind::Text, video.video_id, "",
                             "Set by the buttons below")),
             advanced(flag("--match-day", "Match day video", "Set by the buttons below")),
             option("--before", "Seconds before each rally", FieldKind::Number, "1", "1", ""),
             option("--after", "Seconds after each rally", FieldKind::Number, "1.5", "1.5", ""),
             option("--crossfade", "Cross-fade (s)", FieldKind::Number, "0.5", "0.5",
                    "Seconds two clips fade into each other; 0: hard cuts"),
             option("--rally-titles", "Players on each rally", FieldKind::Flag, "1", "",
                    "Match day video: each clip shows its game's title (the game video always "
                    "shows it)"),
             resolution_field(),
             with_suggestions(option("--bitrate", "Bit rate (Mbit/s)", FieldKind::Number, "", "",
                                     kBitrateHelp),
                              {"2", "4", "8", "12", "16", "24"}),
             choice("--codec", "Codec", {"h265", "h264"}, "h265",
                    "H.265: smaller at the same quality; H.264: plays everywhere"),
             advanced(option("--highlights-dir", "Highlights directory", FieldKind::Directory,
                             in_work_dir(video, {"highlights"}), "data/highlights",
                             "The marks of every game; the videos are written here")),
             advanced(option("--out", "Output video", FieldKind::File, "", "",
                             "Empty: <video id>-<height>p.mp4 or <match day>-highlights-"
                             "<height>p.mp4 in the highlights directory (e.g. -1080p); a bare "
                             "name given here goes there as it is, \".mp4\" is added if "
                             "missing")),
             advanced(option("--font", "Title font", FieldKind::File, "", "",
                             "TrueType font; empty: a bold system font")),
             white_balance_field(video),
             advanced(option("--shapes-dir", "Title shapes", FieldKind::Directory,
                             workspace.shapes_dir(), "",
                             "SVG files for title backdrops, shared by all match days")),
             encoder_field(),
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
             option("--epochs", "Epochs", FieldKind::Number, "120", "120", ""),
             advanced(option("--hidden", "Hidden channels", FieldKind::Number, "128", "128", "")),
             advanced(option("--levels", "Levels", FieldKind::Number, "8", "8", "")),
             advanced(option("--stages", "Stages", FieldKind::Number, "4", "4", "")),
             advanced(option("--seed", "Seed", FieldKind::Number, "42", "42", "")),
             advanced(choice("--device", "Device", {"auto", "cpu", "cuda"}, "auto", ""))}};
}

} // namespace

void propose_bitrate(CommandForm& form) {
    const Field* codec = form.field("--codec");
    const Field* resolution = form.field("--resolution");
    Field* bitrate = form.field("--bitrate");
    if (codec == nullptr || bitrate == nullptr) {
        return;
    }
    std::string proposed; // the original: the encoder chooses
    const std::string chosen = resolution != nullptr ? resolution->value : kOriginalResolution;
    for (const ResolutionBitrate& entry : kResolutionBitrates) {
        if (entry.resolution == chosen) {
            proposed = std::to_string(entry.h265_mbps * (codec->value == "h264" ? kH264Factor : 1));
        }
    }
    if (bitrate->value.empty() || bitrate->value == bitrate->initial_value) {
        bitrate->value = proposed;
    }
    bitrate->initial_value = proposed;
}

std::vector<CommandForm> forms_for(const VideoEntry& video, const WorkspaceSnapshot& workspace) {
    return {align_form(video),           annotate_form(video),
            features_form(video, workspace), detect_form(video, workspace),
            refine_form(video),          cut_form(video),
            highlights_form(video, workspace), train_form(workspace)};
}

} // namespace ttrally::launcher
