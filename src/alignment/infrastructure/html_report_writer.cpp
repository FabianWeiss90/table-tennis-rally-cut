// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "alignment/infrastructure/html_report_writer.hpp"

#include "shared/io/formatting.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace ttrally::alignment {

namespace {

using io::format_clock;

std::string escape(std::string_view text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (const char c : text) {
        switch (c) {
        case '&':
            escaped += "&amp;";
            break;
        case '<':
            escaped += "&lt;";
            break;
        case '>':
            escaped += "&gt;";
            break;
        case '"':
            escaped += "&quot;";
            break;
        default:
            escaped += c;
        }
    }
    return escaped;
}

std::string seconds_or_dash(double seconds) {
    return std::isnan(seconds) ? "–" : std::format("{:.6f} s", seconds);
}

std::string duration_text(double seconds) {
    return std::format("{:.3f} s ({})", seconds, format_clock(seconds));
}

std::string describe_video(const media::MediaInfo& info) {
    if (!info.video) {
        return "none";
    }
    const auto& v = *info.video;
    return std::format("{} {}x{} @ {} fps", v.codec, v.width, v.height,
                       io::format_fps(v.nominal_frame_rate()));
}

std::string describe_audio(const media::MediaInfo& info) {
    if (!info.audio) {
        return "none";
    }
    const auto& a = *info.audio;
    return std::format("{} {} Hz, {} ch", a.codec, a.sample_rate, a.channels);
}

constexpr std::string_view kStyle = R"(
:root { --bg: #ffffff; --fg: #1f2328; --muted: #59636e; --line: #d1d9e0; --head: #f6f8fa;
        --accent: #0969da; --warn: #9a6700; --warn-bg: #fff8c5; --bad: #cf222e; }
@media (prefers-color-scheme: dark) {
  :root { --bg: #0d1117; --fg: #e6edf3; --muted: #9198a1; --line: #3d444d; --head: #151b23;
          --accent: #4493f8; --warn: #d29922; --warn-bg: #272115; --bad: #f85149; }
}
body { background: var(--bg); color: var(--fg); margin: 0 auto; padding: 16px; max-width: 1100px;
       font: 14px/1.5 system-ui, -apple-system, "Segoe UI", sans-serif; }
h1 { font-size: 22px; margin: 8px 0 4px; } h2 { font-size: 17px; margin: 28px 0 8px; }
.muted { color: var(--muted); }
.scroll { overflow-x: auto; }
table { border-collapse: collapse; width: 100%; font-variant-numeric: tabular-nums; }
th, td { border-bottom: 1px solid var(--line); padding: 4px 8px; text-align: right;
         white-space: nowrap; }
th { background: var(--head); } td.l, th.l { text-align: left; white-space: normal; }
.kv td { text-align: left; }
.kv td:first-child { color: var(--muted); width: 30%; }
.warn { background: var(--warn-bg); color: var(--warn); border-radius: 6px; padding: 8px 12px; }
.warn li { margin: 2px 0; } .bad { color: var(--bad); font-weight: 600; }
.plot { width: 100%; height: auto; }
.plot .grid { stroke: var(--line); stroke-width: 1; }
.plot .tick, .plot .axis { fill: var(--muted); font-size: 11px; }
.plot .segment { stroke: var(--accent); stroke-width: 6; stroke-opacity: 0.25;
                 stroke-linecap: round; }
.plot .window { fill: var(--accent); }
.plot .uncertain { stroke: var(--warn); stroke-width: 1; }
)";

/// Inline SVG plot of the window offsets over the cut time.
class OffsetPlot {
  public:
    explicit OffsetPlot(const AlignmentReport& report) : report_(report) { compute_ranges(); }

    [[nodiscard]] std::string render() const {
        if (!std::isfinite(y_min_)) {
            return "<p>No confident windows to plot.</p>\n";
        }
        std::string svg = std::format(R"(<svg class="plot" viewBox="0 0 {} {}" role="img" )"
                                      R"(aria-label="Offset over cut time">)",
                                      kWidth, kHeight);
        svg += render_grid();
        svg += render_data();
        svg += "</svg>\n";
        return svg;
    }

  private:
    static constexpr double kWidth = 960.0;
    static constexpr double kHeight = 320.0;
    static constexpr double kLeft = 70.0;
    static constexpr double kRight = 16.0;
    static constexpr double kTop = 12.0;
    static constexpr double kBottom = 40.0;

    void compute_ranges() {
        for (const auto& w : report_.windows) {
            x_max_ = std::max(x_max_, w.cut_time_s);
            if (w.confident) {
                include_offset(w.offset_s);
            }
        }
        for (const auto& c : report_.candidates) {
            x_max_ = std::max(x_max_, c.cut_end_s);
            include_offset(c.offset_s);
        }
        const double padding = std::max(1.0, 0.05 * (y_max_ - y_min_));
        y_min_ -= padding;
        y_max_ += padding;
    }

    void include_offset(double offset) {
        y_min_ = std::min(y_min_, offset);
        y_max_ = std::max(y_max_, offset);
    }

    /// A "nice" tick step (1, 2 or 5 times a power of ten) for about `count` ticks.
    static double nice_step(double range, int count) {
        const double raw = range / count;
        const double magnitude = std::pow(10.0, std::floor(std::log10(raw)));
        for (const double factor : {1.0, 2.0, 5.0}) {
            if (factor * magnitude >= raw) {
                return factor * magnitude;
            }
        }
        return 10.0 * magnitude;
    }

    [[nodiscard]] double px(double x) const {
        return kLeft + x / x_max_ * (kWidth - kLeft - kRight);
    }
    [[nodiscard]] double py(double y) const {
        return kTop + (y_max_ - y) / (y_max_ - y_min_) * (kHeight - kTop - kBottom);
    }

    [[nodiscard]] std::string render_grid() const {
        std::string svg;
        const double axis_y = kHeight - kBottom;
        const double x_step = nice_step(x_max_, 10);
        for (double x = 0.0; x <= x_max_ + 1e-9; x += x_step) {
            svg += std::format(
                R"(<line class="grid" x1="{0:.1f}" y1="{1:.1f}" x2="{0:.1f}" y2="{2:.1f}"/>)",
                px(x), kTop, axis_y);
            svg += std::format(
                R"(<text class="tick" x="{:.1f}" y="{:.1f}" text-anchor="middle">{}</text>)",
                px(x), axis_y + 16, format_clock(x).substr(0, 7));
        }
        const double y_step = nice_step(y_max_ - y_min_, 6);
        for (double y = std::ceil(y_min_ / y_step) * y_step; y <= y_max_; y += y_step) {
            svg += std::format(
                R"(<line class="grid" x1="{0:.1f}" y1="{1:.1f}" x2="{2:.1f}" y2="{1:.1f}"/>)",
                kLeft, py(y), kWidth - kRight);
            svg += std::format(
                R"(<text class="tick" x="{:.1f}" y="{:.1f}" text-anchor="end">{:.0f} s</text>)",
                kLeft - 6, py(y) + 4, y);
        }
        svg += std::format(R"(<text class="axis" x="{:.1f}" y="{:.1f}" )"
                           R"(text-anchor="middle">time in cut video</text>)",
                           (kLeft + kWidth - kRight) / 2, kHeight - 6);
        return svg;
    }

    [[nodiscard]] std::string render_data() const {
        std::string svg;
        const double axis_y = kHeight - kBottom;
        for (const auto& w : report_.windows) {
            if (!w.confident) {
                svg += std::format(R"(<line class="uncertain" x1="{0:.1f}" y1="{1:.1f}" )"
                                   R"(x2="{0:.1f}" y2="{2:.1f}"/>)",
                                   px(w.cut_time_s), axis_y - 6, axis_y);
            }
        }
        for (const auto& c : report_.candidates) {
            svg += std::format(R"(<line class="segment" x1="{0:.1f}" y1="{1:.1f}" x2="{2:.1f}" )"
                               R"(y2="{1:.1f}"><title>Segment {3}</title></line>)",
                               px(c.cut_start_s), py(c.offset_s), px(c.cut_end_s), c.id);
        }
        for (const auto& w : report_.windows) {
            if (w.confident) {
                svg += std::format(R"(<circle class="window" cx="{:.1f}" cy="{:.1f}" r="2"/>)",
                                   px(w.cut_time_s), py(w.offset_s));
            }
        }
        return svg;
    }

    const AlignmentReport& report_;
    double x_max_ = 1.0;
    double y_min_ = std::numeric_limits<double>::infinity();
    double y_max_ = -std::numeric_limits<double>::infinity();
};

std::string render_summary(const AlignmentReport& report) {
    double total_duration = 0.0;
    for (const auto& c : report.candidates) {
        total_duration += c.duration_s();
    }
    const auto& stats = report.statistics;
    const double confident_percent =
        stats.window_count > 0 ? 100.0 * static_cast<double>(stats.confident_windows) /
                                     static_cast<double>(stats.window_count)
                               : 0.0;

    std::string html = "<h2>Summary</h2>\n<table class=\"kv\">\n";
    auto row = [&html](std::string_view key, const std::string& value) {
        html += std::format("<tr><td>{}</td><td>{}</td></tr>\n", key, value);
    };
    row("Segments", std::to_string(report.candidates.size()));
    row("Total segment duration",
        std::format("{:.1f} s ({})", total_duration, format_clock(total_duration)));
    row("Gaps", std::to_string(report.gaps.size()));
    row("Windows", std::format("{} total, {} confident ({:.1f} %), {} with global search",
                               stats.window_count, stats.confident_windows, confident_percent,
                               stats.global_searches));
    row("Original frame rate",
        std::format("{} fps, {}, {} frames", io::format_fps(report.original_fps),
                    report.original_constant_frame_rate ? "constant" : "variable (PTS list)",
                    report.original_frame_count));
    row("Minimum confidence", std::format("{:.2f}", report.settings.min_confidence));
    if (report.original_decode_backend && report.cut_decode_backend) {
        row("Decode backend (spot check)",
            std::format("original: {}, cut: {}", media::to_string(*report.original_decode_backend),
                        media::to_string(*report.cut_decode_backend)));
    }
    html += "</table>\n";
    return html;
}

std::string render_warnings(const AlignmentReport& report) {
    std::string html = "<h2>Warnings</h2>\n";
    if (report.warnings.empty()) {
        return html + "<p>None.</p>\n";
    }
    html += "<ul class=\"warn\">\n";
    for (const auto& warning : report.warnings) {
        html += "<li>" + escape(warning) + "</li>\n";
    }
    return html + "</ul>\n";
}

std::string render_files(const AlignmentReport& report) {
    const auto& o = report.original;
    const auto& c = report.cut;
    std::string html = "<h2>Files</h2>\n<div class=\"scroll\"><table>\n<tr><th class=\"l\"></th>"
                       "<th class=\"l\">Original</th><th class=\"l\">Cut</th></tr>\n";
    auto row = [&html](std::string_view key, const std::string& a, const std::string& b) {
        html += std::format("<tr><td class=\"l\">{}</td><td class=\"l\">{}</td>"
                            "<td class=\"l\">{}</td></tr>\n",
                            key, a, b);
    };
    const auto video_start = [](const media::MediaInfo& info) {
        return info.video ? seconds_or_dash(info.video->start_time_s) : "–";
    };
    const auto audio_start = [](const media::MediaInfo& info) {
        return info.audio ? seconds_or_dash(info.audio->start_time_s) : "–";
    };
    row("Path", escape(o.path.string()), escape(c.path.string()));
    row("Container", escape(o.container), escape(c.container));
    row("Duration", duration_text(o.duration_s), duration_text(c.duration_s));
    row("Video", escape(describe_video(o)), escape(describe_video(c)));
    row("Audio", escape(describe_audio(o)), escape(describe_audio(c)));
    row("Video start_time", video_start(o), video_start(c));
    row("Audio start_time", audio_start(o), audio_start(c));
    html += "</table></div>\n<p class=\"muted\">Times in this report are presentation times on "
            "each file's own timeline. Frame indices refer to the decoded frames of the "
            "original.</p>\n";
    return html;
}

std::string render_plot(const AlignmentReport& report) {
    return "<h2>Offset over time</h2>\n" + OffsetPlot(report).render() +
           "<p class=\"muted\">Dots: confident windows (offset = original time &minus; cut time). "
           "Bands: segments. Marks on the axis: uncertain windows.</p>\n";
}

std::string visual_cell(const CandidateSegment& candidate, const AlignmentReport& report) {
    if (!candidate.visual_similarity) {
        return "–";
    }
    std::string text = std::format("{:.2f}", *candidate.visual_similarity);
    if (report.visual_check && *candidate.visual_similarity < report.visual_check->min_similarity) {
        text = "<span class=\"bad\">" + text + "</span>";
    }
    return text;
}

std::string render_segments(const AlignmentReport& report) {
    std::string html =
        "<h2>Segments</h2>\n<div class=\"scroll\"><table>\n<tr><th>#</th><th>Cut start</th>"
        "<th>Cut end</th><th>Original start</th><th>Original end</th><th>Start frame</th>"
        "<th>End frame</th><th>Duration</th><th>Offset</th><th>Confidence</th>"
        "<th>Windows</th><th>Visual</th></tr>\n";
    for (const auto& c : report.candidates) {
        html += std::format(
            "<tr><td>{}</td><td>{}</td><td>{}</td><td>{}</td><td>{}</td><td>{}</td><td>{}</td>"
            "<td>{:.2f} s</td><td>{:.3f} s</td><td>{:.1f}</td><td>{}</td><td>{}</td></tr>\n",
            c.id, format_clock(c.cut_start_s), format_clock(c.cut_end_s),
            format_clock(c.orig_start_s), format_clock(c.orig_end_s), c.orig_start_frame,
            c.orig_end_frame, c.duration_s(), c.offset_s, c.confidence, c.window_count,
            visual_cell(c, report));
    }
    return html + "</table></div>\n";
}

std::string render_gaps(const AlignmentReport& report) {
    std::string html =
        "<h2>Gaps in the original</h2>\n<p class=\"muted\">Parts of the original that are not "
        "in the cut video. Check them for missed rallies.</p>\n<div class=\"scroll\"><table>\n"
        "<tr><th>#</th><th>After segment</th><th>Start</th><th>End</th><th>Start frame</th>"
        "<th>End frame</th><th>Duration</th></tr>\n";
    for (const auto& g : report.gaps) {
        html += std::format(
            "<tr><td>{}</td><td>{}</td><td>{}</td><td>{}</td><td>{}</td><td>{}</td>"
            "<td>{:.1f} s</td></tr>\n",
            g.id, g.after_candidate > 0 ? std::to_string(g.after_candidate) : "start",
            format_clock(g.orig_start_s), format_clock(g.orig_end_s), g.orig_start_frame,
            g.orig_end_frame, g.duration_s());
    }
    return html + "</table></div>\n";
}

std::string render_document(const AlignmentReport& report) {
    std::string html = "<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
                       "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
                       "<title>Alignment report</title>\n<style>";
    html += kStyle;
    html += "</style>\n</head>\n<body>\n<h1>Alignment report</h1>\n";
    html += std::format("<p class=\"muted\">ttrally {} &middot; {} &rarr; {}</p>\n",
                        escape(report.tool_version), escape(report.cut.path.string()),
                        escape(report.original.path.string()));
    html += render_summary(report);
    html += render_warnings(report);
    html += render_files(report);
    html += render_plot(report);
    html += render_segments(report);
    html += render_gaps(report);
    html += "</body>\n</html>\n";
    return html;
}

} // namespace

void write_html_report(const std::filesystem::path& path, const AlignmentReport& report) {
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("cannot write " + path.string());
    }
    out << render_document(report);
    if (!out) {
        throw std::runtime_error("error while writing " + path.string());
    }
}

} // namespace ttrally::alignment
