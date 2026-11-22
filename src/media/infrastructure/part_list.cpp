// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "media/infrastructure/part_list.hpp"

#include "media/infrastructure/ffmpeg_api.hpp"

#include <format>
#include <limits>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace ttrally::media {

namespace fs = std::filesystem;

namespace {

constexpr std::string_view kHeader = "ffconcat version 1.0";
constexpr std::string_view kFileDirective = "file ";
constexpr std::string_view kDurationDirective = "duration ";

/// A path in the concat format's quotes: ' is written as '\''.
std::string quoted(const std::string& path) {
    std::string text = "'";
    for (const char c : path) {
        text += c == '\'' ? std::string("'\\''") : std::string(1, c);
    }
    return text + "'";
}

std::string unquoted(std::string_view text) {
    std::string path;
    bool inside = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\'') {
            inside = !inside;
        } else if (!inside && text[i] == '\\' && i + 1 < text.size()) {
            path += text[++i];
        } else {
            path += text[i];
        }
    }
    return path;
}

double duration_of(const fs::path& part) {
    try {
        const ff::FormatPtr format = ff::open_input(part);
        if (format->duration <= 0) {
            throw std::runtime_error(std::format("{} has no known duration", part.string()));
        }
        return static_cast<double>(format->duration) / AV_TIME_BASE;
    } catch (const std::exception& error) {
        throw std::runtime_error(error.what());
    }
}

std::string read_text(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

} // namespace

namespace {

/// The list names exactly these parts and none of them changed since it was written.
bool is_current(const fs::path& list, std::span<const fs::path> parts) {
    std::error_code error;
    const auto written = fs::last_write_time(list, error);
    if (error) {
        return false;
    }
    std::vector<fs::path> wanted;
    for (const fs::path& part : parts) {
        wanted.push_back(fs::absolute(part).lexically_normal());
        if (fs::last_write_time(part, error) > written || error) {
            return false;
        }
    }
    return read_part_list(list) == wanted;
}

} // namespace

void write_part_list(const fs::path& list, std::span<const fs::path> parts) {
    if (is_current(list, parts)) {
        return; // without reading every part again
    }
    const fs::path folder = fs::absolute(list).lexically_normal().parent_path();
    std::string text = std::string(kHeader) + "\n";
    for (const fs::path& part : parts) {
        const fs::path absolute = fs::absolute(part).lexically_normal();
        text += std::format("file {}\nduration {:.6f}\n",
                            quoted(absolute.lexically_relative(folder).generic_string()),
                            duration_of(absolute));
    }
    fs::create_directories(folder);
    std::ofstream out(list, std::ios::binary | std::ios::trunc);
    out << text;
    if (!out) {
        throw std::runtime_error("cannot write " + list.string());
    }
}

std::vector<PartSpan> part_spans(const fs::path& video) {
    if (video.extension() != ff::kPartListExtension) {
        return {{fs::absolute(video).lexically_normal(), 0.0,
                 std::numeric_limits<double>::infinity()}};
    }
    std::vector<PartSpan> spans;
    std::istringstream in(read_text(video));
    const fs::path folder = fs::absolute(video).lexically_normal().parent_path();
    double start = 0.0;
    for (std::string line; std::getline(in, line);) {
        if (line.starts_with(kFileDirective)) {
            spans.push_back({(folder / unquoted(line.substr(kFileDirective.size())))
                                 .lexically_normal(),
                             start, std::numeric_limits<double>::infinity()});
        } else if (line.starts_with(kDurationDirective) && !spans.empty()) {
            start += std::stod(line.substr(kDurationDirective.size()));
            spans.back().end_s = start;
        }
    }
    return spans;
}

std::vector<fs::path> read_part_list(const fs::path& list) {
    std::vector<fs::path> parts;
    std::istringstream in(read_text(list));
    const fs::path folder = fs::absolute(list).lexically_normal().parent_path();
    for (std::string line; std::getline(in, line);) {
        if (line.starts_with(kFileDirective)) {
            parts.push_back((folder / unquoted(line.substr(kFileDirective.size())))
                                .lexically_normal());
        }
    }
    return parts;
}

} // namespace ttrally::media
